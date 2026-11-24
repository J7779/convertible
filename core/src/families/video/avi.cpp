// Convertible core - video family: AVI (RIFF) reader and writer, including
// OpenDML (files over 1 GiB: AVIX extension chunks, 'indx' super indexes and
// 'ix##' standard indexes) and the legacy 'idx1' index; plus the FourCC and
// WAVEFORMATEX tag tables shared with ASF and Matroska.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>

#include "codec/ids.hpp"
#include "families/video/adapt.hpp"
#include "families/video/bitstream.hpp"
#include "families/video/containers.hpp"

namespace cvt::video {

namespace {
namespace id = codec::id;
using bits::fourcc;
}  // namespace

// --------------------------------------------------------------------------- tag tables

std::string_view codec_for_bitmap_fourcc(std::uint32_t f) noexcept {
  auto upper = [](std::uint32_t v) {
    std::uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
      std::uint32_t c = (v >> (24 - 8 * i)) & 0xFF;
      if (c >= 'a' && c <= 'z') c -= 32;
      out = (out << 8) | c;
    }
    return out;
  };
  switch (upper(f)) {
    case fourcc("H264"):
    case fourcc("X264"):
    case fourcc("AVC1"):
    case fourcc("DAVC"): return id::h264;
    case fourcc("HEVC"):
    case fourcc("H265"):
    case fourcc("HVC1"): return id::hevc;
    case fourcc("XVID"):
    case fourcc("DIVX"):
    case fourcc("DX50"):
    case fourcc("FMP4"):
    case fourcc("MP4V"):
    case fourcc("M4S2"):
    case fourcc("MP4S"): return id::mpeg4;
    case fourcc("DIV3"):
    case fourcc("MP43"): return cid::msmpeg4v3;
    case fourcc("MJPG"):
    case fourcc("AVRN"):
    case fourcc("LJPG"): return id::mjpeg;
    case fourcc("I420"):
    case fourcc("IYUV"):
    case fourcc("YV12"):
    case fourcc("NV12"):
    case fourcc("Y42B"): return id::rawvideo;
    case fourcc("VP80"): return id::vp8;
    case fourcc("VP90"): return id::vp9;
    case fourcc("AV01"): return id::av1;
    case fourcc("MPG1"): return id::mpeg1video;
    case fourcc("MPG2"):
    case fourcc("MPEG"): return id::mpeg2video;
    case fourcc("H263"):
    case fourcc("S263"): return id::h263;
    case fourcc("WMV1"): return cid::wmv1;
    case fourcc("WMV2"): return cid::wmv2;
    case fourcc("WMV3"): return cid::wmv3;
    case fourcc("WVC1"): return cid::vc1;
    case 0: return id::rawvideo;  // BI_RGB
    default: return {};
  }
}

std::string_view codec_for_wave_tag(std::uint16_t tag, std::uint16_t bits) noexcept {
  switch (tag) {
    case 0x0001: return bits == 8 ? id::pcm_u8 : bits == 24 ? id::pcm_s24le : bits == 16 ? id::pcm_s16le : std::string_view{};
    case 0x0003: return bits == 32 ? id::pcm_f32le : std::string_view{};
    case 0x0006: return id::pcm_alaw;
    case 0x0007: return id::pcm_mulaw;
    case 0x0002: return id::adpcm_ms;
    case 0x0011: return id::adpcm_ima;
    case 0x0050: return id::mp2;
    case 0x0055: return id::mp3;
    case 0x00FF:
    case 0x1600:
    case 0x706D: return id::aac;
    case 0x2000: return id::ac3;
    case 0x0160: return cid::wmav1;
    case 0x0161: return cid::wmav2;
    case 0x0162: return cid::wmapro;
    case 0x704F: return id::opus;
    case 0xF1AC: return id::flac;
    default: return {};
  }
}

std::uint32_t bitmap_fourcc_for_codec(std::string_view codec) noexcept {
  if (codec == id::h264) return fourcc("H264");
  if (codec == id::hevc) return fourcc("HEVC");
  if (codec == id::mpeg4) return fourcc("FMP4");
  if (codec == id::mjpeg) return fourcc("MJPG");
  if (codec == id::rawvideo) return fourcc("I420");
  if (codec == id::vp8) return fourcc("VP80");
  if (codec == id::mpeg1video) return fourcc("MPG1");
  if (codec == id::mpeg2video) return fourcc("MPG2");
  if (codec == id::h263) return fourcc("H263");
  return 0;
}

std::uint16_t wave_tag_for_codec(std::string_view codec) noexcept {
  if (codec == id::pcm_s16le || codec == id::pcm_u8 || codec == id::pcm_s24le) return 0x0001;
  if (codec == id::pcm_f32le) return 0x0003;
  if (codec == id::pcm_alaw) return 0x0006;
  if (codec == id::pcm_mulaw) return 0x0007;
  if (codec == id::adpcm_ms) return 0x0002;
  if (codec == id::adpcm_ima) return 0x0011;
  if (codec == id::mp2) return 0x0050;
  if (codec == id::mp3) return 0x0055;
  if (codec == id::aac) return 0x00FF;
  if (codec == id::ac3) return 0x2000;
  return 0;
}

// =========================================================================== reader

namespace {

struct AviStream {
  TrackInfo info;
  std::uint32_t scale = 1;
  std::uint32_t rate = 1;
  std::uint32_t start = 0;
  std::uint32_t sample_size = 0;
  std::uint64_t tick = 1;  // track ticks per strh unit
  std::uint64_t count = 0;  // frames (VBR) or bytes (CBR) so far
  std::vector<std::uint64_t> super_index;  // offsets of 'ix##' chunks
  bool raw_rgb = false;
};

struct AviChunk {
  std::uint32_t id = 0;
  std::uint64_t data = 0;
  std::uint64_t size = 0;
};

int stream_number(std::uint32_t ckid) {
  const int a = static_cast<int>((ckid >> 24) & 0xFF) - '0';
  const int b = static_cast<int>((ckid >> 16) & 0xFF) - '0';
  if (a < 0 || a > 9 || b < 0 || b > 9) return -1;
  return a * 10 + b;
}

// RIFF chunks inside [begin, end).
std::vector<AviChunk> riff_chunks(ByteView input, std::uint64_t begin, std::uint64_t end) {
  std::vector<AviChunk> out;
  std::uint64_t pos = begin;
  end = std::min<std::uint64_t>(end, input.size());
  while (pos + 8 <= end && out.size() < 50'000'000) {
    util::ByteReader r(input.subspan(static_cast<std::size_t>(pos), 8));
    AviChunk c;
    c.id = std::byteswap(r.u32le());
    const std::uint32_t size = r.u32le();
    c.data = pos + 8;
    c.size = std::min<std::uint64_t>(size, end - c.data);
    out.push_back(c);
    pos = c.data + size + (size & 1);
  }
  return out;
}

std::uint64_t gcd64(std::uint64_t a, std::uint64_t b) {
  while (b != 0) {
    const std::uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a == 0 ? 1 : a;
}

struct AviReader {
  ByteView input;
  const ReadSettings& settings;
  std::shared_ptr<Demuxed> out;
  std::vector<AviStream> streams;
  std::vector<std::uint64_t> movi;  // position of each 'movi' list type field
  std::uint64_t idx1 = 0;
  std::uint64_t idx1_size = 0;

  void parse_strl(const AviChunk& list) {
    AviStream s;
    for (const AviChunk& c : riff_chunks(input, list.data + 4, list.data + list.size)) {
      util::ByteReader r(input.subspan(static_cast<std::size_t>(c.data), static_cast<std::size_t>(c.size)));
      if (c.id == fourcc("strh")) {
        const std::uint32_t type = std::byteswap(r.u32le());
        r.skip(4 + 4 + 2 + 2 + 4);
        s.scale = r.u32le();
        s.rate = r.u32le();
        s.start = r.u32le();
        r.skip(4 + 4 + 4);
        s.sample_size = r.u32le();
        s.info.type = type == fourcc("vids") ? ir::TrackType::Video : type == fourcc("auds") ? ir::TrackType::Audio
                                                                                               : ir::TrackType::Subtitle;
        if (type != fourcc("vids") && type != fourcc("auds")) s.info.tag = bits::fourcc_text(type);
      } else if (c.id == fourcc("strf")) {
        if (s.info.type == ir::TrackType::Video) {
          r.skip(4);
          const std::int32_t width = r.i32le();
          const std::int32_t height = r.i32le();
          r.skip(2);
          const std::uint16_t bitcount = r.u16le();
          const std::uint32_t compression = std::byteswap(r.u32le());
          if (!r.ok()) continue;
          s.info.width = static_cast<std::uint32_t>(std::min(std::abs(width), 1 << 20));
          s.info.height = static_cast<std::uint32_t>(std::min(std::abs(height), 1 << 20));
          s.info.tag = bits::fourcc_text(compression);
          s.info.codec = std::string(codec_for_bitmap_fourcc(compression));
          if (c.size > 40) s.info.extradata.assign(input.begin() + static_cast<std::ptrdiff_t>(c.data + 40),
                                                   input.begin() + static_cast<std::ptrdiff_t>(c.data + c.size));
          if (s.info.codec == id::rawvideo) {
            if (compression == 0) {
              if (bitcount == 24 && height > 0) {
                s.info.extradata = to_bytes(std::string_view("BGR3"));
              } else {
                s.info.codec.clear();  // other DIB layouts are not supported
              }
            } else {
              s.info.extradata = to_bytes(bits::fourcc_text(compression));
            }
          }
        } else if (s.info.type == ir::TrackType::Audio) {
          const std::uint16_t tag = r.u16le();
          s.info.channels = r.u16le();
          s.info.sample_rate = r.u32le();
          r.skip(4);
          s.info.block_align = r.u16le();
          s.info.bits_per_sample = r.u16le();
          if (!r.ok()) continue;
          s.info.tag = std::format("0x{:04x}", tag);
          s.info.codec = std::string(codec_for_wave_tag(tag, s.info.bits_per_sample));
          if (c.size >= 18) {
            const std::uint16_t extra = r.u16le();
            const std::uint64_t n = std::min<std::uint64_t>(extra, c.size - 18);
            s.info.extradata.assign(input.begin() + static_cast<std::ptrdiff_t>(c.data + 18),
                                    input.begin() + static_cast<std::ptrdiff_t>(c.data + 18 + n));
          }
        }
      } else if (c.id == fourcc("indx")) {
        const std::uint16_t longs = r.u16le();
        const std::uint8_t sub = r.u8();
        const std::uint8_t type = r.u8();
        const std::uint32_t entries = r.u32le();
        r.skip(4 + 12);
        if (type == 0 && sub == 0 && longs == 4) {
          for (std::uint32_t i = 0; i < entries && r.remaining() >= 16; ++i) {
            const std::uint64_t offset = r.u64le();
            r.skip(8);
            s.super_index.push_back(offset);
          }
        }
      } else if (c.id == fourcc("strn")) {
        s.info.title = std::string(as_chars(input.subspan(static_cast<std::size_t>(c.data), static_cast<std::size_t>(c.size))));
        if (const std::size_t nul = s.info.title.find('\0'); nul != std::string::npos) s.info.title.resize(nul);
      }
    }
    if (s.scale == 0) s.scale = 1;
    if (s.rate == 0) s.rate = 1;
    const std::uint64_t g = gcd64(s.rate, s.scale);
    const std::uint64_t timescale = s.rate / g;
    s.tick = s.scale / g;
    if (timescale > 0xFFFFFFFFull) {
      s.info.timescale = 1'000'000;
      s.tick = 0;
    } else {
      s.info.timescale = static_cast<std::uint32_t>(timescale);
    }
    if (s.info.type == ir::TrackType::Video) s.info.fps = ir::Rational{s.rate, s.scale};
    s.info.id = static_cast<std::uint32_t>(streams.size());
    streams.push_back(std::move(s));
  }

  std::int64_t timestamp(const AviStream& s, std::uint64_t units) const {
    if (s.tick == 0) return rescale(static_cast<std::int64_t>(units), s.rate, static_cast<std::int64_t>(s.scale) * 1'000'000);
    return static_cast<std::int64_t>((units + s.start) * s.tick);
  }

  void add(int number, std::uint64_t data, std::uint64_t size, bool key) {
    if (number < 0 || static_cast<std::size_t>(number) >= streams.size()) return;
    AviStream& s = streams[static_cast<std::size_t>(number)];
    const bool cbr = s.info.type == ir::TrackType::Audio && s.sample_size > 0;
    const std::uint64_t units = cbr ? s.count / s.sample_size : s.count;
    const std::uint64_t length = cbr ? size / s.sample_size : 1;
    if (size == 0) {  // a dropped frame keeps its time slot
      if (!cbr) ++s.count;
      return;
    }
    const std::int64_t pts = timestamp(s, units);
    const std::int64_t duration = timestamp(s, units + length) - pts;
    if (!out->add_input_packet(s.info, data, size, pts, pts, duration, key || s.info.type != ir::TrackType::Video)) {
      return;
    }
    s.count += cbr ? size : 1;
  }

  bool use_opendml() {
    if (!std::ranges::all_of(streams, [](const AviStream& s) { return !s.super_index.empty(); }) || streams.empty()) {
      return false;
    }
    for (std::size_t n = 0; n < streams.size(); ++n) {
      for (const std::uint64_t ix : streams[n].super_index) {
        if (ix + 8 + 24 > input.size()) return false;
        util::ByteReader r(input.subspan(static_cast<std::size_t>(ix)));
        r.skip(8);
        const std::uint16_t longs = r.u16le();
        const std::uint8_t sub = r.u8();
        const std::uint8_t type = r.u8();
        const std::uint32_t entries = r.u32le();
        r.skip(4);
        const std::uint64_t base = r.u64le();
        r.skip(4);
        if (longs != 2 || sub != 0 || type != 1) return false;
        for (std::uint32_t i = 0; i < entries && r.remaining() >= 8; ++i) {
          const std::uint32_t offset = r.u32le();
          const std::uint32_t size = r.u32le();
          add(static_cast<int>(n), base + offset, size & 0x7FFFFFFF, (size & 0x80000000u) == 0);
        }
      }
    }
    return true;
  }

  bool use_idx1() {
    if (idx1 == 0 || movi.empty()) return false;
    const std::uint64_t entries = idx1_size / 16;
    if (entries == 0) return false;
    util::ByteReader r(input.subspan(static_cast<std::size_t>(idx1), static_cast<std::size_t>(entries * 16)));
    // Offsets are relative to the 'movi' type field, or (rarely) absolute.
    const std::uint32_t first_id = std::byteswap(r.u32le());
    r.skip(4);
    const std::uint32_t first_offset = r.u32le();
    std::uint64_t base = movi.front();
    if (first_offset + 8 <= input.size() &&
        std::byteswap(util::ByteReader(input.subspan(first_offset, 4)).u32le()) == first_id) {
      base = 0;
    }
    r.seek(0);
    for (std::uint64_t i = 0; i < entries; ++i) {
      const std::uint32_t ckid = std::byteswap(r.u32le());
      const std::uint32_t flags = r.u32le();
      const std::uint32_t offset = r.u32le();
      const std::uint32_t size = r.u32le();
      if (ckid == fourcc("rec ")) continue;
      add(stream_number(ckid), base + offset + 8, size, (flags & 0x10) != 0);
    }
    return true;
  }

  void scan_movi(std::uint64_t begin, std::uint64_t end, int depth) {
    if (depth > 4) return;
    for (const AviChunk& c : riff_chunks(input, begin, end)) {
      if (c.id == fourcc("LIST")) {
        scan_movi(c.data + 4, c.data + c.size, depth + 1);
        continue;
      }
      const int n = stream_number(c.id);
      if (n < 0) continue;
      const std::uint32_t kind = c.id & 0xFFFF;
      if (kind == ((static_cast<std::uint32_t>('i') << 8) | 'x')) continue;
      bool key = true;
      if (n < static_cast<int>(streams.size()) && streams[static_cast<std::size_t>(n)].info.type == ir::TrackType::Video) {
        key = streams[static_cast<std::size_t>(n)].info.packets.empty();
      }
      add(n, c.data, c.size, key);
    }
  }
};

}  // namespace

Result<std::shared_ptr<Demuxed>> demux_avi(ByteView input, const ReadSettings& settings) {
  util::ByteReader head(input);
  if (!head.consume("RIFF") || (head.skip(4), !head.consume("AVI "))) return fail(Status::Decode, "not an AVI file");
  AviReader reader{input, settings, std::make_shared<Demuxed>(), {}, {}, 0, 0};
  reader.out->input = input;
  reader.out->format_id = "avi";
  std::vector<std::pair<std::uint64_t, std::uint64_t>> movi_ranges;
  for (const AviChunk& riff : riff_chunks(input, 0, input.size())) {
    if (riff.id != fourcc("RIFF")) continue;
    for (const AviChunk& c : riff_chunks(input, riff.data + 4, riff.data + riff.size)) {
      if (c.id == fourcc("LIST") && c.size >= 4) {
        const std::uint32_t type = std::byteswap(util::ByteReader(input.subspan(static_cast<std::size_t>(c.data), 4)).u32le());
        if (type == fourcc("hdrl")) {
          for (const AviChunk& h : riff_chunks(input, c.data + 4, c.data + c.size)) {
            if (h.id == fourcc("LIST") && h.size >= 4 &&
                std::byteswap(util::ByteReader(input.subspan(static_cast<std::size_t>(h.data), 4)).u32le()) == fourcc("strl")) {
              reader.parse_strl(h);
            }
          }
        } else if (type == fourcc("movi")) {
          reader.movi.push_back(c.data);
          movi_ranges.emplace_back(c.data + 4, c.data + c.size);
        } else if (type == fourcc("INFO")) {
          for (const AviChunk& i : riff_chunks(input, c.data + 4, c.data + c.size)) {
            std::string value(as_chars(input.subspan(static_cast<std::size_t>(i.data), static_cast<std::size_t>(i.size))));
            if (const std::size_t nul = value.find('\0'); nul != std::string::npos) value.resize(nul);
            if (i.id == fourcc("INAM")) reader.out->metadata["title"] = value;
            if (i.id == fourcc("IART")) reader.out->metadata["artist"] = value;
            if (i.id == fourcc("ICMT")) reader.out->metadata["comment"] = value;
          }
        }
      } else if (c.id == fourcc("idx1")) {
        reader.idx1 = c.data;
        reader.idx1_size = c.size;
      }
    }
  }
  if (reader.streams.empty()) return fail(Status::Decode, "the AVI file has no stream headers");
  if (!reader.use_opendml()) {
    for (AviStream& s : reader.streams) {
      s.info.packets.clear();
      s.count = 0;
    }
    if (!reader.use_idx1()) {
      for (const auto& [b, e] : movi_ranges) reader.scan_movi(b, e, 0);
    }
  }
  for (AviStream& s : reader.streams) {
    if (s.info.codec == id::mp3 && !s.info.packets.empty()) {
      const Bytes first = reader.out->packet_bytes(s.info.packets.front());
      if (const auto h = bits::parse_mpeg_audio(first); h && h->layer == 2) s.info.codec = std::string(id::mp2);
    }
    if (s.info.codec == id::h264) derive_h264_pts(*reader.out, s.info);
    reader.out->tracks.push_back(std::move(s.info));
  }
  CVT_TRY(finish_demux(*reader.out, "AVI"));
  return reader.out;
}

// =========================================================================== writer

namespace {

struct AviOutStream {
  MuxTrack desc;
  StreamAdapter adapter;
  bool video = false;
  std::uint32_t scale = 1;
  std::uint32_t rate = 1;
  std::uint32_t sample_size = 0;
  std::uint64_t next_frame = 0;  // video: next frame slot
  bool started = false;
  std::int64_t first_dts = 0;
  std::uint64_t length = 0;      // strh dwLength (frames, or samples/blocks for CBR audio)
  std::uint32_t max_chunk = 0;
  struct Entry {
    std::uint64_t offset;  // of the chunk header, relative to the movi data buffer
    std::uint32_t size;
    bool key;
  };
  std::vector<Entry> entries;
  Bytes first_packet;
};

class AviMuxer final : public Muxer {
 public:
  Result<void> begin(std::vector<MuxTrack> tracks, const MuxSettings& settings) override {
    settings_ = settings;
    if (tracks.size() > 99) return fail(Status::Encode, "AVI holds at most 99 streams");
    for (MuxTrack& t : tracks) {
      if (!container_accepts("avi", t.type, t.codec)) return fail(Status::Encode, "AVI cannot store {}", t.codec);
      AviOutStream s;
      s.video = t.type == ir::TrackType::Video;
      Packaging p;
      p.length_prefixed = false;
      p.adts = false;
      s.adapter = StreamAdapter(t.codec, t.extradata, p);
      if (s.video) {
        const ir::Rational fps = t.fps.num > 0 && t.fps.den > 0 ? t.fps : ir::Rational{25, 1};
        s.rate = static_cast<std::uint32_t>(std::min<std::int64_t>(fps.num, 0xFFFFFFFF));
        s.scale = static_cast<std::uint32_t>(std::min<std::int64_t>(fps.den, 0xFFFFFFFF));
      } else if (t.codec.starts_with("pcm_")) {
        const std::uint32_t align = t.block_align != 0 ? t.block_align : std::max<std::uint32_t>(1, t.channels * 2u);
        s.sample_size = align;
        s.scale = align;
        s.rate = t.sample_rate * align;
      } else {
        const std::uint32_t frame = t.codec == id::aac ? 1024 : t.codec == id::ac3 ? 1536
                                    : (t.codec == id::mp3 && t.sample_rate < 32000) ? 576 : 1152;
        s.scale = frame;
        s.rate = t.sample_rate == 0 ? 44100 : t.sample_rate;
      }
      s.desc = std::move(t);
      streams_.push_back(std::move(s));
    }
    return {};
  }

  Result<void> write(std::size_t index, const ir::Packet& packet) override {
    AviOutStream& s = streams_[index];
    CVT_TRY_ASSIGN(Bytes data, s.adapter.convert(packet.bytes, packet.keyframe));
    if (s.first_packet.empty()) s.first_packet = data;
    if (s.video) {
      // AVI chunks are in decode order on a constant frame grid: slot by decode time and
      // fill skipped slots with empty (dropped) frames.
      if (!s.started) {
        s.started = true;
        s.first_dts = packet.dts;
      }
      const std::int64_t slot =
          rescale(packet.dts - s.first_dts, static_cast<std::int64_t>(s.desc.timescale) * s.scale, s.rate);
      while (static_cast<std::int64_t>(s.next_frame) < slot && slot - static_cast<std::int64_t>(s.next_frame) < 100000) {
        chunk(index, {}, false);
        ++s.next_frame;
      }
      ++s.next_frame;
      s.length = s.next_frame;
    } else {
      s.length += s.sample_size > 0 ? data.size() / s.sample_size : 1;
    }
    chunk(index, data, packet.keyframe || !s.video);
    if (movi_.size() > settings_.limits.max_output_bytes) return fail(Status::Limit, "the AVI output exceeds the output size limit");
    return {};
  }

  void set_extradata(std::size_t track, Bytes extradata) override {
    Packaging p;
    p.length_prefixed = false;
    streams_[track].adapter = StreamAdapter(streams_[track].desc.codec, std::move(extradata), p);
  }

  Result<Bytes> finish() override {
    if (streams_.empty()) return fail(Status::Encode, "nothing to write");
    util::ByteWriter out;
    // Segment the data into RIFF parts of at most ~1 GiB (OpenDML).
    constexpr std::uint64_t part_limit = 1000ull * 1000 * 1000;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> parts;  // [begin, end) of movi_ chunks
    {
      std::uint64_t begin = 0;
      std::uint64_t at = 0;
      for (const ChunkRef& c : order_) {
        if (c.offset - begin > part_limit && c.offset > begin) {
          parts.emplace_back(begin, c.offset);
          begin = c.offset;
        }
        at = c.offset;
      }
      (void)at;
      parts.emplace_back(begin, movi_.size());
    }
    const Bytes hdrl_probe = header(std::vector<std::vector<std::uint64_t>>(streams_.size()), parts.size());
    // Positions: RIFF(12) + hdrl + LIST movi header(12) ...
    std::vector<std::vector<std::uint64_t>> ix_offsets(streams_.size());
    std::vector<Bytes> part_bytes;
    std::uint64_t file_pos = 12 + hdrl_probe.size();
    for (std::size_t p = 0; p < parts.size(); ++p) {
      const auto [begin, end] = parts[p];
      const std::uint64_t movi_data = file_pos + (p == 0 ? 0 : 12) + 12;  // where movi_[begin] lands
      util::ByteWriter movi;
      movi.text("movi");
      movi.bytes(ByteView(movi_).subspan(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin)));
      // Standard index chunks for this part.
      for (std::size_t n = 0; n < streams_.size(); ++n) {
        std::vector<const AviOutStream::Entry*> list;
        for (const auto& e : streams_[n].entries) {
          if (e.offset >= begin && e.offset < end) list.push_back(&e);
        }
        if (list.empty()) continue;
        ix_offsets[n].push_back(movi_data - 4 + movi.size());
        const std::uint64_t base = movi_data - begin;
        movi.text(std::format("ix{:02}", n)).u32le(static_cast<std::uint32_t>(24 + list.size() * 8));
        movi.u16le(2).u8(0).u8(1).u32le(static_cast<std::uint32_t>(list.size())).u32le(std::byteswap(chunk_id(n)));
        movi.u64le(base).u32le(0);
        for (const auto* e : list) {
          movi.u32le(static_cast<std::uint32_t>(e->offset - begin + 8));
          movi.u32le(e->size | (e->key ? 0u : 0x80000000u));
        }
      }
      util::ByteWriter part;
      if (p > 0) part.text("RIFF").u32le(0).text("AVIX");
      part.text("LIST").u32le(static_cast<std::uint32_t>(movi.size())).bytes(movi.view());
      if (p == 0) {
        // Legacy index for players without OpenDML support.
        util::ByteWriter idx;
        for (const ChunkRef& c : order_) {
          if (c.offset >= end) break;
          const AviOutStream::Entry& e = streams_[c.stream].entries[c.entry];
          idx.u32le(std::byteswap(chunk_id(c.stream))).u32le(e.key ? 0x10 : 0).u32le(static_cast<std::uint32_t>(4 + e.offset));
          idx.u32le(e.size);
        }
        part.text("idx1").u32le(static_cast<std::uint32_t>(idx.size())).bytes(idx.view());
      } else {
        part.patch_u32le(4, static_cast<std::uint32_t>(part.size() - 8));
      }
      file_pos += part.size() - (p == 0 ? 0 : 0);
      if (p == 0) file_pos += 0;
      part_bytes.push_back(part.take());
    }
    const Bytes hdrl = header(ix_offsets, parts.size());
    out.text("RIFF").u32le(0).text("AVI ").bytes(hdrl);
    out.bytes(part_bytes.front());
    out.patch_u32le(4, static_cast<std::uint32_t>(out.size() - 8));
    for (std::size_t p = 1; p < part_bytes.size(); ++p) out.bytes(part_bytes[p]);
    CVT_TRY(settings_.limits.check_output(out.size()));
    return out.take();
  }

 private:
  struct ChunkRef {
    std::uint64_t offset;
    std::size_t stream;
    std::size_t entry;
  };

  std::uint32_t chunk_id(std::size_t n) const {
    const AviOutStream& s = streams_[n];
    const char a = static_cast<char>('0' + n / 10);
    const char b = static_cast<char>('0' + n % 10);
    const char* kind = s.video ? (s.desc.codec == id::rawvideo ? "db" : "dc") : "wb";
    return fourcc(std::string{a, b, kind[0], kind[1]});
  }

  void chunk(std::size_t n, ByteView data, bool key) {
    AviOutStream& s = streams_[n];
    const std::uint64_t at = movi_.size();
    util::ByteWriter w;
    w.u32le(std::byteswap(chunk_id(n))).u32le(static_cast<std::uint32_t>(data.size())).bytes(data);
    if (data.size() & 1) w.u8(0);
    append(movi_, w.view());
    s.entries.push_back({at, static_cast<std::uint32_t>(data.size()), key});
    s.max_chunk = std::max<std::uint32_t>(s.max_chunk, static_cast<std::uint32_t>(data.size()));
    order_.push_back({at, n, s.entries.size() - 1});
  }

  Bytes header(const std::vector<std::vector<std::uint64_t>>& ix_offsets, std::size_t parts) {
    util::ByteWriter hdrl;
    hdrl.text("hdrl");
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frames = 0;
    std::uint32_t usec = 40000;
    for (const AviOutStream& s : streams_) {
      if (s.video && width == 0) {
        width = s.desc.width;
        height = s.desc.height;
        frames = s.length;
        usec = static_cast<std::uint32_t>(std::llround(1e6 * s.scale / std::max<std::uint32_t>(1, s.rate)));
      }
    }
    hdrl.text("avih").u32le(56).u32le(usec).u32le(0).u32le(0).u32le(0x10 | 0x100);
    hdrl.u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, 0xFFFFFFFF))).u32le(0);
    hdrl.u32le(static_cast<std::uint32_t>(streams_.size())).u32le(1 << 20).u32le(width).u32le(height).zeros(16);
    for (std::size_t n = 0; n < streams_.size(); ++n) {
      const AviOutStream& s = streams_[n];
      util::ByteWriter strl;
      strl.text("strl");
      strl.text("strh").u32le(56);
      strl.text(s.video ? "vids" : "auds");
      {
        const Bytes tag = s.adapter.extradata();
        std::uint32_t handler = s.video ? bitmap_fourcc_for_codec(s.desc.codec) : 0;
        if (s.video && s.desc.codec == id::rawvideo && tag.size() == 4) handler = fourcc(as_chars(tag));
        strl.u32le(std::byteswap(handler));
      }
      strl.u32le(0).u16le(0).u16le(0).u32le(0).u32le(s.scale).u32le(s.rate).u32le(0);
      strl.u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(s.length, 0xFFFFFFFF))).u32le(s.max_chunk).u32le(0xFFFFFFFF);
      strl.u32le(s.sample_size).u16le(0).u16le(0).u16le(static_cast<std::uint16_t>(s.desc.width)).u16le(static_cast<std::uint16_t>(s.desc.height));
      Bytes extra = s.adapter.extradata();
      if (s.video) {
        const bool raw = s.desc.codec == id::rawvideo;
        std::uint32_t compression = bitmap_fourcc_for_codec(s.desc.codec);
        if (raw && extra.size() == 4) compression = fourcc(as_chars(extra));
        if (raw) extra.clear();
        util::ByteWriter f;
        f.u32le(40 + static_cast<std::uint32_t>(extra.size())).i32le(static_cast<std::int32_t>(s.desc.width));
        f.i32le(static_cast<std::int32_t>(s.desc.height)).u16le(1).u16le(raw ? 12 : 24);
        f.u32le(std::byteswap(compression));
        f.u32le(raw ? s.desc.width * s.desc.height * 3 / 2 : s.desc.width * s.desc.height * 3).zeros(16);
        f.bytes(extra);
        strl.text("strf").u32le(static_cast<std::uint32_t>(f.size())).bytes(f.view());
      } else {
        const std::uint16_t tag = wave_tag_for_codec(s.desc.codec);
        std::uint16_t bits = 0;
        std::uint16_t align = 1;
        std::uint32_t bytes_per_second = 0;
        if (s.sample_size > 0) {
          align = static_cast<std::uint16_t>(s.sample_size);
          bits = static_cast<std::uint16_t>(s.desc.codec == id::pcm_s24le ? 24 : s.desc.codec == id::pcm_f32le ? 32
                                            : (s.desc.codec == id::pcm_u8 || s.desc.codec == id::pcm_alaw ||
                                               s.desc.codec == id::pcm_mulaw) ? 8 : 16);
          bytes_per_second = s.desc.sample_rate * align;
        } else {
          std::uint64_t total = 0;
          for (const auto& e : s.entries) total += e.size;
          const double seconds = static_cast<double>(s.length) * s.scale / std::max<std::uint32_t>(1, s.rate);
          bytes_per_second = seconds > 0 ? static_cast<std::uint32_t>(static_cast<double>(total) / seconds) : 0;
        }
        if (s.desc.codec == id::mp3) {
          util::ByteWriter mp3;
          mp3.u16le(1).u32le(2).u16le(static_cast<std::uint16_t>(s.max_chunk)).u16le(1).u16le(0);
          extra = mp3.take();
        }
        util::ByteWriter f;
        f.u16le(tag).u16le(s.desc.channels == 0 ? 1 : s.desc.channels).u32le(s.desc.sample_rate).u32le(bytes_per_second);
        f.u16le(align).u16le(bits).u16le(static_cast<std::uint16_t>(extra.size())).bytes(extra);
        strl.text("strf").u32le(static_cast<std::uint32_t>(f.size())).bytes(f.view());
        if (f.size() & 1) strl.u8(0);
      }
      // OpenDML super index.
      util::ByteWriter indx;
      indx.u16le(4).u8(0).u8(0).u32le(static_cast<std::uint32_t>(ix_offsets[n].size())).u32le(std::byteswap(chunk_id(n)));
      indx.zeros(12);
      const std::size_t slots = std::max<std::size_t>(parts, ix_offsets[n].size());
      for (std::size_t i = 0; i < slots; ++i) {
        const std::uint64_t at = i < ix_offsets[n].size() ? ix_offsets[n][i] : 0;
        std::uint32_t size = 0;
        std::uint32_t duration = 0;
        if (i < ix_offsets[n].size()) {
          std::uint32_t count = 0;
          for (const auto& e : s.entries) {
            (void)e;
            ++count;
          }
          size = 32 + count * 8;
          duration = static_cast<std::uint32_t>(std::min<std::uint64_t>(s.length, 0xFFFFFFFF));
        }
        indx.u64le(at).u32le(size).u32le(duration);
      }
      strl.text("indx").u32le(static_cast<std::uint32_t>(indx.size())).bytes(indx.view());
      hdrl.text("LIST").u32le(static_cast<std::uint32_t>(strl.size())).bytes(strl.view());
    }
    util::ByteWriter odml;
    odml.text("odml").text("dmlh").u32le(248).u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, 0xFFFFFFFF))).zeros(244);
    hdrl.text("LIST").u32le(static_cast<std::uint32_t>(odml.size())).bytes(odml.view());
    util::ByteWriter list;
    list.text("LIST").u32le(static_cast<std::uint32_t>(hdrl.size())).bytes(hdrl.view());
    return list.take();
  }

  MuxSettings settings_;
  std::vector<AviOutStream> streams_;
  Bytes movi_;
  std::vector<ChunkRef> order_;
};

}  // namespace

std::unique_ptr<Muxer> make_avi_muxer() { return std::make_unique<AviMuxer>(); }

}  // namespace cvt::video
