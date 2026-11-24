// Convertible core - video family: readers for Advanced Systems Format
// (WMV, WMA, ASF) and RealMedia (RM, RMVB). Both are read-only containers:
// their usual codecs (WMV/WMA, RealVideo/Cook) are proprietary, but the
// packets are demuxed faithfully so other codecs inside them (and any decoder
// registered later) work, and the streams can be remuxed.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>

#include "codec/ids.hpp"
#include "families/video/bitstream.hpp"
#include "families/video/containers.hpp"
#include "util/text.hpp"

namespace cvt::video {

namespace {

namespace id = codec::id;

using Guid = std::array<std::uint8_t, 16>;
constexpr Guid header_guid = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr Guid data_guid = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr Guid file_props_guid = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr Guid stream_props_guid = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr Guid audio_guid = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr Guid video_guid = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr Guid spread_guid = {0x50, 0xCD, 0xC3, 0xBF, 0x8F, 0x61, 0xCF, 0x11, 0x8B, 0xB2, 0x00, 0xAA, 0x00, 0xB4, 0xE2, 0x20};
constexpr Guid content_guid = {0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};

bool is_guid(ByteView data, const Guid& g) {
  return data.size() >= 16 && std::memcmp(data.data(), g.data(), 16) == 0;
}

std::uint32_t read_by_type(util::ByteReader& r, int type) {
  switch (type & 3) {
    case 1: return r.u8();
    case 2: return r.u16le();
    case 3: return r.u32le();
    default: return 0;
  }
}

std::string utf16_field(ByteView data) {
  std::u16string text;
  for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
    const char16_t c = static_cast<char16_t>(std::to_integer<int>(data[i]) | (std::to_integer<int>(data[i + 1]) << 8));
    if (c == 0) break;
    text.push_back(c);
  }
  return util::utf16_to_utf8(text);
}

struct AsfStream {
  std::size_t track = 0;
  // media object being reassembled
  std::uint32_t object = 0xFFFFFFFF;
  std::uint32_t object_size = 0;
  std::int64_t object_pts = 0;
  bool object_key = false;
  PacketBuilder* builder = nullptr;
  std::uint64_t collected = 0;
  // audio spread (error concealment) descrambling
  int span = 0;
  std::uint32_t virtual_packet = 0;
  std::uint32_t virtual_chunk = 0;
};

}  // namespace

Result<std::shared_ptr<Demuxed>> demux_asf(ByteView input, const ReadSettings& settings) {
  if (!is_guid(input, header_guid) || input.size() < 30) return fail(Status::Decode, "not an ASF file");
  auto out = std::make_shared<Demuxed>();
  out->input = input;
  out->format_id = "asf";
  util::ByteReader hr(input.subspan(16));
  const std::uint64_t header_size = hr.u64le();
  const std::uint32_t objects = hr.u32le();
  if (header_size > input.size() || header_size < 30) return fail(Status::Decode, "ASF header is truncated");
  std::uint32_t packet_size = 0;
  std::int64_t preroll = 0;
  std::map<int, AsfStream> streams;
  std::vector<TrackInfo> infos;
  std::map<int, std::unique_ptr<PacketBuilder>> builders;
  std::uint64_t pos = 30;
  for (std::uint32_t i = 0; i < objects && pos + 24 <= header_size; ++i) {
    const ByteView obj = input.subspan(static_cast<std::size_t>(pos), static_cast<std::size_t>(header_size - pos));
    util::ByteReader r(obj.subspan(16));
    const std::uint64_t size = r.u64le();
    if (size < 24 || size > header_size - pos) break;
    const ByteView body = obj.subspan(24, static_cast<std::size_t>(size - 24));
    util::ByteReader b(body);
    if (is_guid(obj, file_props_guid)) {
      b.skip(16 + 8 + 8 + 8 + 8 + 8);
      preroll = static_cast<std::int64_t>(b.u64le());
      b.skip(4);
      const std::uint32_t min_size = b.u32le();
      const std::uint32_t max_size = b.u32le();
      if (min_size == max_size) packet_size = min_size;
    } else if (is_guid(obj, stream_props_guid)) {
      const ByteView type = b.bytes(16);
      const ByteView correction = b.bytes(16);
      b.skip(8);
      const std::uint32_t type_length = b.u32le();
      const std::uint32_t correction_length = b.u32le();
      const int number = b.u16le() & 0x7F;
      b.skip(4);
      const ByteView specific = b.bytes(type_length);
      const ByteView correction_data = b.bytes(correction_length);
      if (!b.ok() || streams.contains(number)) {
        pos += size;
        continue;
      }
      TrackInfo t;
      t.timescale = 1000;
      t.id = static_cast<std::uint32_t>(number);
      AsfStream s;
      if (is_guid(type, audio_guid) && specific.size() >= 16) {
        util::ByteReader a(specific);
        const std::uint16_t tag = a.u16le();
        t.type = ir::TrackType::Audio;
        t.channels = a.u16le();
        t.sample_rate = a.u32le();
        a.skip(4);
        t.block_align = a.u16le();
        t.bits_per_sample = a.u16le();
        t.codec = std::string(codec_for_wave_tag(tag, t.bits_per_sample));
        t.tag = std::format("0x{:04x}", tag);
        if (specific.size() >= 18) {
          const std::uint16_t extra = a.u16le();
          const ByteView e = specific.subspan(18, std::min<std::size_t>(extra, specific.size() - 18));
          t.extradata.assign(e.begin(), e.end());
        }
        if (is_guid(correction, spread_guid) && correction_data.size() >= 5) {
          util::ByteReader c(correction_data);
          s.span = c.u8();
          s.virtual_packet = c.u16le();
          s.virtual_chunk = c.u16le();
          if (s.span <= 1 || s.virtual_chunk == 0 || s.virtual_packet % s.virtual_chunk != 0) s.span = 0;
        }
      } else if (is_guid(type, video_guid) && specific.size() >= 11 + 40) {
        util::ByteReader v(specific);
        t.type = ir::TrackType::Video;
        t.width = std::min<std::uint32_t>(v.u32le(), 1u << 20);
        t.height = std::min<std::uint32_t>(v.u32le(), 1u << 20);
        v.skip(1 + 2);
        const std::uint32_t bih_size = v.u32le();
        v.skip(4 + 4 + 2 + 2);
        const std::uint32_t compression = std::byteswap(v.u32le());
        t.codec = std::string(codec_for_bitmap_fourcc(compression));
        t.tag = bits::fourcc_text(compression);
        if (bih_size > 40 && 11 + bih_size <= specific.size()) {
          t.extradata.assign(specific.begin() + 11 + 40, specific.begin() + 11 + bih_size);
        }
      } else {
        pos += size;
        continue;
      }
      s.track = infos.size();
      streams.emplace(number, s);
      builders.emplace(number, std::make_unique<PacketBuilder>(*out));
      infos.push_back(std::move(t));
    } else if (is_guid(obj, content_guid)) {
      const std::uint16_t title = b.u16le();
      const std::uint16_t author = b.u16le();
      b.skip(6);
      const ByteView t = b.bytes(title);
      const ByteView a = b.bytes(author);
      if (b.ok()) {
        if (!t.empty()) out->metadata["title"] = utf16_field(t);
        if (!a.empty()) out->metadata["artist"] = utf16_field(a);
      }
    }
    pos += size;
  }
  if (streams.empty()) return fail(Status::Decode, "the ASF file has no streams");
  if (packet_size == 0) return fail(Status::Decode, "ASF files with variable packet sizes are not supported");
  // Data object.
  if (header_size + 50 > input.size() || !is_guid(input.subspan(static_cast<std::size_t>(header_size)), data_guid)) {
    return fail(Status::Decode, "the ASF file has no data object");
  }
  const auto emit = [&](int number) {
    AsfStream& s = streams[number];
    PacketBuilder& builder = *builders[number];
    if (builder.empty()) return;
    TrackInfo& t = infos[s.track];
    const std::int64_t pts = s.object_pts - preroll;
    t.packets.push_back(builder.finish(pts, pts, 0, s.object_key || t.type != ir::TrackType::Video));
  };
  std::uint64_t packet = header_size + 50;
  std::uint64_t count = 0;
  for (; packet + packet_size <= input.size(); packet += packet_size) {
    if ((++count & 1023) == 0 && !settings.progress.report(static_cast<double>(packet) / static_cast<double>(input.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    util::ByteReader r(input.subspan(static_cast<std::size_t>(packet), packet_size));
    std::uint8_t lt = r.u8();
    if (lt & 0x80) {
      r.skip(lt & 0x0F);
      lt = r.u8();
    }
    const std::uint8_t pf = r.u8();
    const std::uint32_t length = read_by_type(r, lt >> 5);
    read_by_type(r, lt >> 1);
    const std::uint32_t padding = read_by_type(r, lt >> 3);
    r.skip(4 + 2);
    if (!r.ok()) continue;
    const std::uint32_t used = length != 0 && length <= packet_size ? length : packet_size;
    const std::uint64_t end = used > padding ? used - padding : 0;
    int payloads = 1;
    int length_type = 2;
    if (lt & 1) {
      const std::uint8_t flags = r.u8();
      payloads = flags & 0x3F;
      length_type = flags >> 6;
    }
    for (int p = 0; p < payloads && r.ok() && r.position() < end; ++p) {
      const std::uint8_t stream_byte = r.u8();
      const int number = stream_byte & 0x7F;
      const bool key = (stream_byte & 0x80) != 0;
      const std::uint32_t object = read_by_type(r, pf >> 4);
      const std::uint32_t offset = read_by_type(r, pf >> 2);
      const std::uint32_t replicated = read_by_type(r, pf);
      std::uint32_t object_size = 0;
      std::int64_t pts = 0;
      bool compressed = false;
      if (replicated >= 8) {
        object_size = r.u32le();
        pts = r.u32le();
        r.skip(replicated - 8);
      } else if (replicated == 1) {
        compressed = true;
        pts = offset;  // presentation time stands in the offset field
        r.skip(1);     // presentation time delta
      } else {
        r.skip(replicated);
      }
      std::uint64_t payload_length = 0;
      if (lt & 1) {
        payload_length = read_by_type(r, length_type);
      } else {
        payload_length = end > r.position() ? end - r.position() : 0;
      }
      if (!r.ok() || payload_length > r.remaining()) break;
      const std::uint64_t data = packet + r.position();
      r.skip(static_cast<std::size_t>(payload_length));
      const auto found = streams.find(number);
      if (found == streams.end()) continue;
      AsfStream& s = found->second;
      PacketBuilder& builder = *builders[number];
      if (compressed) {
        // Sub-payloads: each a whole media object.
        std::uint64_t at = data;
        const std::uint64_t stop = data + payload_length;
        while (at < stop) {
          const std::uint8_t sub = std::to_integer<std::uint8_t>(input[static_cast<std::size_t>(at)]);
          if (at + 1 + sub > stop) break;
          emit(number);
          s.object_pts = pts;
          s.object_key = key;
          builder.input(at + 1, sub);
          emit(number);
          at += 1 + sub;
        }
        continue;
      }
      if (offset == 0 || object != s.object) {
        emit(number);
        s.object = object;
        s.object_size = object_size;
        s.object_pts = pts;
        s.object_key = key;
        s.collected = 0;
        if (offset != 0) continue;  // the start of this object was lost
      }
      builder.input(data, payload_length);
      s.collected += payload_length;
      if (s.object_size != 0 && s.collected >= s.object_size) emit(number);
    }
  }
  for (auto& [number, s] : streams) emit(number);
  // Undo the audio spread scrambling (a span of virtual packets is interleaved chunk by chunk).
  for (auto& [number, s] : streams) {
    if (s.span <= 1) continue;
    TrackInfo& t = infos[s.track];
    std::vector<PacketRef> fixed;
    for (const PacketRef& ref : t.packets) {
      const Bytes data = out->packet_bytes(ref);
      const std::size_t block = static_cast<std::size_t>(s.span) * s.virtual_packet;
      if (data.size() % block != 0) {
        fixed.push_back(ref);
        continue;
      }
      Bytes plain(data.size());
      const std::size_t chunks_per_packet = s.virtual_packet / s.virtual_chunk;
      const std::size_t chunks = block / s.virtual_chunk;
      for (std::size_t base = 0; base < data.size(); base += block) {
        for (std::size_t k = 0; k < chunks; ++k) {
          const std::size_t row = k / static_cast<std::size_t>(s.span);
          const std::size_t col = k % static_cast<std::size_t>(s.span);
          const std::size_t source = base + k * s.virtual_chunk;
          const std::size_t target = base + (col * chunks_per_packet + row) * s.virtual_chunk;
          std::memcpy(plain.data() + target, data.data() + source, s.virtual_chunk);
        }
      }
      PacketBuilder builder(*out);
      builder.owned(plain);
      fixed.push_back(builder.finish(ref.pts, ref.dts, ref.duration, ref.keyframe));
    }
    t.packets = std::move(fixed);
  }
  for (TrackInfo& t : infos) out->tracks.push_back(std::move(t));
  CVT_TRY(finish_demux(*out, "ASF"));
  return out;
}

// =========================================================================== RealMedia

Result<std::shared_ptr<Demuxed>> demux_rm(ByteView input, const ReadSettings& settings) {
  if (input.size() < 18 || as_chars(input.first(4)) != ".RMF") return fail(Status::Decode, "not a RealMedia file");
  auto out = std::make_shared<Demuxed>();
  out->input = input;
  out->format_id = "rm";
  struct RmStream {
    std::size_t track = 0;
    bool swap_bytes = false;
    bool supported = true;
    // RealVideo slices of the frame being assembled
    std::vector<std::pair<std::uint64_t, std::uint64_t>> slices;
    std::uint32_t frame_length = 0;
    std::int64_t frame_pts = 0;
    bool frame_key = false;
  };
  std::map<int, RmStream> streams;
  std::vector<TrackInfo> infos;
  std::uint64_t pos = 0;
  std::uint64_t data_start = 0;
  std::uint64_t data_end = 0;
  while (pos + 10 <= input.size()) {
    util::ByteReader r(input.subspan(static_cast<std::size_t>(pos)));
    const std::uint32_t type = r.u32be();
    const std::uint32_t size = r.u32be();
    if (size < 10) break;
    const std::uint64_t end = std::min<std::uint64_t>(pos + size, input.size());
    if (type == bits::fourcc("MDPR")) {
      r.skip(2);
      const int number = r.u16be();
      r.skip(4 * 7);
      const std::uint8_t name_length = r.u8();
      r.skip(name_length);
      const std::uint8_t mime_length = r.u8();
      const std::string mime(r.chars(mime_length));
      const std::uint32_t specific_length = r.u32be();
      const ByteView specific = r.bytes(specific_length);
      if (!r.ok()) break;
      TrackInfo t;
      t.timescale = 1000;
      t.id = static_cast<std::uint32_t>(number);
      RmStream s;
      if (specific.size() >= 26 && as_chars(specific.subspan(4, 4)) == "VIDO") {
        util::ByteReader v(specific.subspan(8));
        const std::uint32_t fcc = v.u32be();
        t.type = ir::TrackType::Video;
        t.width = v.u16be();
        t.height = v.u16be();
        t.tag = bits::fourcc_text(fcc);
        if (fcc == bits::fourcc("RV10")) t.codec = std::string(cid::rv10);
        if (fcc == bits::fourcc("RV20")) t.codec = std::string(cid::rv20);
        if (fcc == bits::fourcc("RV30")) t.codec = std::string(cid::rv30);
        if (fcc == bits::fourcc("RV40")) t.codec = std::string(cid::rv40);
        t.extradata.assign(specific.begin() + 26, specific.end());
      } else if (specific.size() >= 4 && as_chars(specific.first(4)) == ".ra\xfd") {
        t.type = ir::TrackType::Audio;
        util::ByteReader a(specific.subspan(4));
        const std::uint16_t version = a.u16be();
        std::string fourcc_text;
        if (version == 4) {
          a.skip(2 + 4 + 4 + 2 + 4 + 2 + 4 + 12 + 2 + 2 + 2 + 2);
          t.sample_rate = a.u16be();
          a.skip(2);
          t.bits_per_sample = a.u16be();
          t.channels = a.u16be();
          const std::uint8_t il = a.u8();
          a.skip(il);
          const std::uint8_t cl = a.u8();
          fourcc_text = std::string(a.chars(cl));
        } else if (version == 5) {
          a.skip(2 + 4 + 4 + 2 + 4 + 2 + 4 + 12 + 2 + 2 + 2 + 2 + 6);
          t.sample_rate = a.u16be();
          a.skip(2);
          t.bits_per_sample = a.u16be();
          t.channels = a.u16be();
          a.skip(4);
          fourcc_text = std::string(a.chars(4));
        }
        t.tag = fourcc_text;
        if (fourcc_text == "dnet") {
          t.codec = std::string(id::ac3);
          s.swap_bytes = true;
        } else if (fourcc_text == "raac" || fourcc_text == "racp") {
          t.codec = std::string(id::aac);
        } else if (fourcc_text == "cook") {
          t.codec = std::string(cid::cook);
          s.supported = false;  // needs de-interleaving that is not implemented
        } else {
          s.supported = false;
        }
      } else {
        pos = end;
        continue;
      }
      if (!s.supported) settings.warnings.add(std::format("the RealMedia audio codec '{}' is not supported", t.tag));
      s.track = infos.size();
      streams.emplace(number, std::move(s));
      infos.push_back(std::move(t));
    } else if (type == bits::fourcc("DATA")) {
      data_start = pos + 18;
      data_end = end;
      break;
    } else if (type == bits::fourcc("CONT")) {
      r.skip(2);
      const std::uint16_t title = r.u16be();
      const std::string value(r.chars(title));
      if (r.ok() && !value.empty()) out->metadata["title"] = value;
    }
    pos = end;
  }
  if (data_start == 0) return fail(Status::Decode, "the RealMedia file has no data chunk");
  std::uint64_t at = data_start;
  std::uint64_t count = 0;
  while (at + 12 <= data_end) {
    util::ByteReader r(input.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(data_end - at)));
    const std::uint16_t version = r.u16be();
    const std::uint16_t length = r.u16be();
    const int number = r.u16be();
    const std::int64_t ts = r.u32be();
    if (version == 1) r.skip(1);
    r.skip(1);
    const std::uint8_t flags = r.u8();
    if (version > 1 || length < r.position() || at + length > data_end) break;
    if ((++count & 1023) == 0 && !settings.progress.report(static_cast<double>(at) / static_cast<double>(input.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    const std::uint64_t payload = at + r.position();
    const std::uint64_t payload_length = length - r.position();
    at += length;
    const auto found = streams.find(number);
    if (found == streams.end() || !found->second.supported) continue;
    RmStream& s = found->second;
    TrackInfo& t = infos[s.track];
    const bool key = (flags & 2) != 0;
    if (t.type == ir::TrackType::Audio) {
      PacketBuilder builder(*out);
      if (s.swap_bytes) {
        Bytes swapped(input.begin() + static_cast<std::ptrdiff_t>(payload),
                      input.begin() + static_cast<std::ptrdiff_t>(payload + payload_length));
        for (std::size_t i = 0; i + 1 < swapped.size(); i += 2) std::swap(swapped[i], swapped[i + 1]);
        builder.owned(swapped);
      } else {
        builder.input(payload, payload_length);
      }
      t.packets.push_back(builder.finish(ts, ts, 0, true));
      continue;
    }
    // RealVideo: a packet holds a whole frame, one slice of a frame, or several frames.
    util::ByteReader v(input.subspan(static_cast<std::size_t>(payload), static_cast<std::size_t>(payload_length)));
    std::uint64_t remaining = payload_length;
    while (remaining > 0 && v.ok()) {
      const std::uint8_t hdr = v.u8();
      const int kind = hdr >> 6;
      if (kind != 3) v.skip(1);
      std::uint32_t total = 0;
      std::uint32_t offset = 0;
      std::int64_t frame_ts = ts;
      const auto number_field = [&]() -> std::uint32_t {
        const std::uint16_t n = v.u16be();
        if (n & 0x4000) return n & 0x3FFF;
        return (static_cast<std::uint32_t>(n & 0x3FFF) << 16) | v.u16be();
      };
      if (kind != 1) {
        total = number_field();
        offset = number_field();
        v.skip(1);
      }
      if (!v.ok()) break;
      std::uint64_t len = v.remaining();
      if (kind == 3) {
        len = std::min<std::uint64_t>(total, v.remaining());
        frame_ts = offset;
      }
      const std::uint64_t data = payload + v.position();
      if (kind == 1 || kind == 3) {
        static constexpr std::byte whole[9] = {std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
                                               std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
        PacketBuilder builder(*out);
        builder.owned(whole);
        builder.input(data, len);
        t.packets.push_back(builder.finish(frame_ts, frame_ts, 0, key));
      } else {
        if (offset == 0 || s.slices.empty()) {
          s.slices.clear();
          s.frame_length = total;
          s.frame_pts = ts;
          s.frame_key = key;
        }
        s.slices.emplace_back(data, len);
        std::uint64_t collected = 0;
        for (const auto& sl : s.slices) collected += sl.second;
        if (kind == 2 || collected >= s.frame_length) {
          util::ByteWriter table;
          table.u8(static_cast<std::uint8_t>(s.slices.size() - 1));
          std::uint32_t off = 0;
          for (const auto& sl : s.slices) {
            table.u32le(1).u32le(off);
            off += static_cast<std::uint32_t>(sl.second);
          }
          PacketBuilder builder(*out);
          builder.owned(table.view());
          for (const auto& sl : s.slices) builder.input(sl.first, sl.second);
          t.packets.push_back(builder.finish(s.frame_pts, s.frame_pts, 0, s.frame_key));
          s.slices.clear();
        }
      }
      v.skip(static_cast<std::size_t>(len));
      remaining = v.remaining();
      if (kind != 3) break;
    }
  }
  for (TrackInfo& t : infos) out->tracks.push_back(std::move(t));
  CVT_TRY(finish_demux(*out, "RealMedia"));
  return out;
}

}  // namespace cvt::video
