// Convertible core - video family: Ogg (OGV, OGG, OGA, OPUS) reader and
// writer: the page and packet layers and the stream mappings of Theora,
// Vorbis, Opus and FLAC (headers, granule positions, timestamps).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <format>
#include <map>

#include "codec/ids.hpp"
#include "families/video/adapt.hpp"
#include "families/video/bitstream.hpp"
#include "families/video/containers.hpp"

namespace cvt::video {

namespace {

namespace id = codec::id;

std::uint8_t at(ByteView d, std::size_t i) { return std::to_integer<std::uint8_t>(d[i]); }

constexpr std::array<std::uint32_t, 256> ogg_crc_table = [] {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t r = i << 24;
    for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
    t[i] = r;
  }
  return t;
}();

std::uint32_t ogg_crc(ByteView data) {
  std::uint32_t crc = 0;
  for (const std::byte b : data) crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) ^ std::to_integer<std::uint32_t>(b)) & 0xFF];
  return crc;
}

int flac_block_samples(ByteView frame) {
  if (frame.size() < 6 || at(frame, 0) != 0xFF || (at(frame, 1) & 0xFE) != 0xF8) return 0;
  const int code = at(frame, 2) >> 4;
  if (code == 1) return 192;
  if (code >= 2 && code <= 5) return 576 << (code - 2);
  if (code >= 8) return 256 << (code - 8);
  if (code == 6 || code == 7) {
    // Skip the UTF-8 coded frame/sample number.
    std::size_t i = 4;
    const std::uint8_t lead = at(frame, i);
    int extra = 0;
    while (extra < 7 && (lead & (0x80 >> extra)) != 0) ++extra;
    i += extra == 0 ? 1 : static_cast<std::size_t>(extra);
    if (code == 6 && i < frame.size()) return at(frame, i) + 1;
    if (code == 7 && i + 1 < frame.size()) return ((at(frame, i) << 8) | at(frame, i + 1)) + 1;
  }
  return 0;
}

struct OggStream {
  std::uint32_t serial = 0;
  std::size_t track = 0;
  std::string codec;
  int headers_needed = 0;
  std::vector<Bytes> headers;
  bits::VorbisDurations vorbis;
  bits::TheoraInfo theora;
  std::uint16_t pre_skip = 0;
  // packets with their duration and the granule of the page they ended on
  struct Pending {
    PacketRef ref;
    std::int64_t duration = 0;
    std::int64_t granule = -1;
    bool key = true;
  };
  std::vector<Pending> packets;
  PacketBuilder* builder = nullptr;
  bool continuing = false;
};

}  // namespace

Result<std::shared_ptr<Demuxed>> demux_ogg(ByteView input, const ReadSettings& settings) {
  if (input.size() < 27 || as_chars(input.first(4)) != "OggS") return fail(Status::Decode, "not an Ogg file");
  auto out = std::make_shared<Demuxed>();
  out->input = input;
  out->format_id = "ogg";
  std::map<std::uint32_t, OggStream> streams;
  std::vector<std::uint32_t> order;
  std::map<std::uint32_t, std::unique_ptr<PacketBuilder>> builders;
  std::size_t pos = 0;
  std::uint64_t pages = 0;
  while (pos + 27 <= input.size()) {
    if (as_chars(input.subspan(pos, 4)) != "OggS") {
      const std::size_t next = as_chars(input.subspan(pos + 1)).find("OggS");
      if (next == std::string_view::npos) break;
      pos += 1 + next;
      continue;
    }
    util::ByteReader r(input.subspan(pos));
    r.skip(4);
    r.skip(1);
    const std::uint8_t flags = r.u8();
    const std::int64_t granule = r.i64le();
    const std::uint32_t serial = r.u32le();
    r.skip(8);
    const std::uint8_t segments = r.u8();
    const ByteView lacing = r.bytes(segments);
    if (!r.ok()) break;
    std::size_t body = pos + 27 + segments;
    std::size_t total = 0;
    for (std::size_t i = 0; i < segments; ++i) total += at(lacing, i);
    if (body + total > input.size()) {
      settings.warnings.add("the Ogg file is truncated; the last page was dropped");
      break;
    }
    if ((++pages & 1023) == 0 && !settings.progress.report(static_cast<double>(pos) / static_cast<double>(input.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    if ((flags & 2) && !streams.contains(serial)) {
      OggStream s;
      s.serial = serial;
      streams.emplace(serial, std::move(s));
      order.push_back(serial);
      builders.emplace(serial, std::make_unique<PacketBuilder>(*out));
    }
    const auto found = streams.find(serial);
    if (found == streams.end()) {
      pos = body + total;
      continue;
    }
    OggStream& s = found->second;
    PacketBuilder& builder = *builders[serial];
    if (!(flags & 1) && !builder.empty()) {
      (void)builder.finish(0, 0, 0, true);  // a continued packet was lost: drop it
    }
    std::size_t first_completed = s.packets.size();
    std::size_t packet_start = body;
    std::size_t length = 0;
    for (std::size_t i = 0; i < segments; ++i) {
      const std::size_t lace = at(lacing, i);
      length += lace;
      if (lace == 255 && i + 1 < segments) continue;
      if (lace == 255) break;  // continues on the next page
      builder.input(packet_start, length);
      packet_start += length;
      length = 0;
      if (builder.size() > 0xFFFFFFF) return fail(Status::Limit, "Ogg packet is too large");
      // A complete packet.
      if (s.headers_needed > 0 || (s.codec.empty() && s.headers.empty())) {
        const PacketRef ref = builder.finish(0, 0, 0, true);
        Bytes data = out->packet_bytes(ref);
        if (s.headers.empty()) {
          const std::string_view head = as_chars(ByteView(data).first(std::min<std::size_t>(data.size(), 8)));
          if (head.starts_with("\x01vorbis")) {
            s.codec = std::string(id::vorbis);
            s.headers_needed = 3;
          } else if (head.starts_with("\x80theora")) {
            s.codec = std::string(id::theora);
            s.headers_needed = 3;
          } else if (head.starts_with("OpusHead")) {
            s.codec = std::string(id::opus);
            s.headers_needed = 2;
          } else if (head.starts_with("\x7F" "FLAC")) {
            s.codec = std::string(id::flac);
            s.headers_needed = data.size() >= 9 ? 1 + ((at(data, 7) << 8) | at(data, 8)) : 1;
          } else if (head.starts_with("Speex")) {
            s.codec = std::string(id::speex);
            s.headers_needed = 2;
          } else {
            s.codec = "unknown";
            s.headers_needed = 0;
          }
        }
        s.headers.push_back(std::move(data));
        if (s.headers_needed > 0) --s.headers_needed;
        if (s.headers_needed == 0 && s.codec == id::vorbis && s.headers.size() >= 3) s.vorbis.init(s.headers[0], s.headers[2]);
        continue;
      }
      const PacketRef ref = builder.finish(0, 0, 0, true);
      OggStream::Pending p;
      p.ref = ref;
      const Bytes head = out->packet_bytes(ref);
      if (s.codec == id::opus) p.duration = bits::opus_packet_samples(head);
      if (s.codec == id::vorbis) p.duration = std::max(0, s.vorbis.next(head));
      if (s.codec == id::flac) p.duration = flac_block_samples(head);
      if (s.codec == id::theora) {
        p.duration = 1;
        p.key = !head.empty() && (at(head, 0) & 0x40) == 0;
      }
      s.packets.push_back(p);
    }
    if (length > 0) builder.input(packet_start, length);
    if (granule != -1 && s.packets.size() > first_completed) s.packets.back().granule = granule;
    pos = body + total;
  }
  for (const std::uint32_t serial : order) {
    OggStream& s = streams[serial];
    if (s.codec.empty() || s.codec == "unknown" || s.headers.empty()) continue;
    TrackInfo t;
    t.codec = s.codec;
    t.tag = s.codec;
    t.id = serial;
    t.type = s.codec == id::theora ? ir::TrackType::Video : ir::TrackType::Audio;
    std::int64_t step = 1;
    if (s.codec == id::vorbis) {
      t.extradata = bits::join_xiph_lacing(s.headers);
      if (const auto info = bits::parse_vorbis_id(s.headers[0])) {
        t.sample_rate = info->sample_rate;
        t.channels = static_cast<std::uint16_t>(info->channels);
      }
      t.timescale = t.sample_rate == 0 ? 48000 : t.sample_rate;
    } else if (s.codec == id::theora) {
      t.extradata = bits::join_xiph_lacing(s.headers);
      if (const auto info = bits::parse_theora_id(s.headers[0])) {
        s.theora = *info;
        t.width = info->width;
        t.height = info->height;
        if (info->fps_num > 0 && info->fps_den > 0) t.fps = ir::Rational{info->fps_num, info->fps_den};
      }
      t.timescale = s.theora.fps_num == 0 ? 25 : s.theora.fps_num;
      step = s.theora.fps_den == 0 ? 1 : s.theora.fps_den;
    } else if (s.codec == id::opus) {
      t.extradata = s.headers[0];
      t.sample_rate = 48000;
      t.channels = s.headers[0].size() > 9 ? at(s.headers[0], 9) : 2;
      s.pre_skip = bits::opus_pre_skip(t.extradata);
      t.priming = s.pre_skip;
      t.timescale = 48000;
    } else if (s.codec == id::flac) {
      const Bytes& h = s.headers[0];
      if (h.size() >= 9 + 4 + 4 + 34) t.extradata.assign(h.begin() + 17, h.begin() + 51);
      Bytes block = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{34}};
      block.insert(block.end(), t.extradata.begin(), t.extradata.end());
      if (const auto info = bits::parse_flac_streaminfo(block)) {
        t.sample_rate = info->sample_rate;
        t.channels = static_cast<std::uint16_t>(info->channels);
        t.bits_per_sample = static_cast<std::uint16_t>(info->bits);
      }
      t.timescale = t.sample_rate == 0 ? 44100 : t.sample_rate;
    } else {
      t.extradata = s.headers[0];
      t.timescale = 1000;
    }
    // Anchor the timeline on the first page that completes a packet, then run forward.
    std::int64_t start = 0;
    for (std::size_t i = 0; i < s.packets.size(); ++i) {
      if (s.packets[i].granule < 0) continue;
      std::int64_t end_position = s.packets[i].granule;
      if (s.codec == id::theora) {
        const int shift = s.theora.keyframe_shift;
        const std::int64_t frames = (end_position >> shift) + (end_position & ((std::int64_t{1} << shift) - 1));
        end_position = frames;  // frame count after this packet (Theora 3.2.1 numbers frames from 1)
      }
      std::int64_t sum = 0;
      for (std::size_t k = 0; k <= i; ++k) sum += s.packets[k].duration;
      start = sat_add(end_position, -sum);
      if (s.codec == id::opus) start -= s.pre_skip;
      break;
    }
    std::int64_t position = start;
    for (OggStream::Pending& p : s.packets) {
      const std::int64_t pts = sat_mul(position, step);
      p.ref.pts = pts;
      p.ref.dts = pts;
      p.ref.duration = sat_mul(p.duration, step);
      p.ref.keyframe = p.key;
      t.packets.push_back(p.ref);
      position = sat_add(position, p.duration);
    }
    out->tracks.push_back(std::move(t));
  }
  CVT_TRY(finish_demux(*out, "Ogg"));
  return out;
}

// =========================================================================== writer

namespace {

class OggMuxer final : public Muxer {
 public:
  Result<void> begin(std::vector<MuxTrack> tracks, const MuxSettings& settings) override {
    settings_ = settings;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      MuxTrack& t = tracks[i];
      if (!container_accepts(settings.format_id, t.type, t.codec)) {
        return fail(Status::Encode, "{} cannot store {}", settings.format_id, t.codec);
      }
      Stream s;
      s.desc = std::move(t);
      s.serial = 0x43565400u + static_cast<std::uint32_t>(i);  // fixed, deterministic serial numbers
      streams_.push_back(std::move(s));
    }
    // Theora first: its BOS page must come first in an Ogg video file.
    std::ranges::stable_sort(streams_order_ = make_order(), [&](std::size_t a, std::size_t b) {
      return streams_[a].desc.type == ir::TrackType::Video && streams_[b].desc.type != ir::TrackType::Video;
    });
    return {};
  }

  Result<void> write(std::size_t index, const ir::Packet& packet) override {
    Stream& s = streams_[index];
    if (!s.headers_ready) CVT_TRY(prepare_headers(s, packet));
    std::int64_t duration = 1;
    const ByteView data = packet.bytes;
    if (s.desc.codec == id::opus) {
      duration = bits::opus_packet_samples(data);
    } else if (s.desc.codec == id::vorbis) {
      const int d = s.vorbis.next(data);
      duration = s.first_audio ? 0 : std::max(0, d);
      s.first_audio = false;
    } else if (s.desc.codec == id::flac) {
      duration = flac_block_samples(data);
    }
    if (s.desc.codec == id::theora) {
      const bool key = packet.keyframe;
      if (key) s.last_key = s.frame;
      ++s.frame;
      s.granule = ((s.last_key + 1) << s.theora_shift) | (s.frame - 1 - s.last_key);
    } else {
      s.granule += duration;
    }
    s.pending.push_back(Bytes(data.begin(), data.end()));
    s.pending_granule = s.granule;
    std::size_t pending_bytes = 0;
    for (const Bytes& b : s.pending) pending_bytes += b.size();
    if (pending_bytes >= 4096 || s.pending.size() >= 50) flush(s, false);
    if (out_.size() > settings_.limits.max_output_bytes) return fail(Status::Limit, "the Ogg output exceeds the output size limit");
    return {};
  }

  void set_extradata(std::size_t track, Bytes extradata) override { streams_[track].desc.extradata = std::move(extradata); }

  Result<Bytes> finish() override {
    for (Stream& s : streams_) {
      if (!s.headers_ready) continue;
      flush(s, true);
    }
    if (out_.size() == 0) return fail(Status::Encode, "nothing to write: the stream has no packets");
    // BOS pages first, then header pages, then the data pages in write order.
    util::ByteWriter file;
    for (const std::size_t i : streams_order_) file.bytes(streams_[i].bos.view());
    for (const std::size_t i : streams_order_) file.bytes(streams_[i].header_pages.view());
    file.bytes(out_.view());
    CVT_TRY(settings_.limits.check_output(file.size()));
    return file.take();
  }

 private:
  struct Stream {
    MuxTrack desc;
    std::uint32_t serial = 0;
    std::uint32_t sequence = 0;
    bool headers_ready = false;
    bits::VorbisDurations vorbis;
    bool first_audio = true;
    int theora_shift = 6;
    std::int64_t frame = 0;
    std::int64_t last_key = 0;
    std::int64_t granule = 0;
    std::int64_t pending_granule = -1;
    std::vector<Bytes> pending;
    util::ByteWriter bos;
    util::ByteWriter header_pages;
  };

  std::vector<std::size_t> make_order() const {
    std::vector<std::size_t> v(streams_.size());
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = i;
    return v;
  }

  Result<void> prepare_headers(Stream& s, const ir::Packet& first) {
    std::vector<Bytes> headers;
    const std::string& c = s.desc.codec;
    if (c == id::vorbis || c == id::theora) {
      auto split = bits::split_xiph_lacing(s.desc.extradata);
      if (!split || split->size() != 3) return fail(Status::Encode, "the {} stream has no setup headers", c);
      headers = std::move(*split);
      if (c == id::vorbis && !s.vorbis.init(headers[0], headers[2])) return fail(Status::Encode, "invalid Vorbis setup header");
      if (c == id::theora) {
        if (const auto info = bits::parse_theora_id(headers[0])) s.theora_shift = info->keyframe_shift;
      }
    } else if (c == id::opus) {
      Bytes head = s.desc.extradata;
      const std::int64_t priming = first.pts < 0 ? rescale(-first.pts, s.desc.timescale, 48000) : 0;
      if (head.size() < 19) {
        head = bits::make_opus_head(s.desc.channels == 0 ? 2 : s.desc.channels, s.desc.sample_rate,
                                    static_cast<std::uint16_t>(std::min<std::int64_t>(priming, 0xFFFF)));
      }
      headers.push_back(head);
      util::ByteWriter tags;
      tags.text("OpusTags").u32le(0).u32le(0);
      headers.push_back(tags.take());
    } else if (c == id::flac) {
      if (s.desc.extradata.size() < 34) return fail(Status::Encode, "the FLAC stream has no STREAMINFO");
      util::ByteWriter h;
      h.u8(0x7F).text("FLAC").u8(1).u8(0).u16be(1).text("fLaC").u8(0x00).u24be(34);
      h.bytes(ByteView(s.desc.extradata).last(34));
      headers.push_back(h.take());
      util::ByteWriter comment;
      comment.u8(0x84).u24be(8).u32le(0).u32le(0);  // last block: empty VORBIS_COMMENT
      headers.push_back(comment.take());
    } else {
      return fail(Status::Encode, "Ogg cannot store {}", c);
    }
    // The first header alone on the BOS page; the rest on their own page(s).
    page(s, s.bos, {headers[0]}, 0, 0x02);
    std::vector<Bytes> rest(headers.begin() + 1, headers.end());
    if (!rest.empty()) page(s, s.header_pages, rest, 0, 0);
    if (c == id::opus) s.granule = 0;
    s.headers_ready = true;
    return {};
  }

  void flush(Stream& s, bool last) {
    if (s.pending.empty() && !last) return;
    page(s, out_, s.pending, s.pending.empty() ? s.granule : s.pending_granule, last ? 0x04 : 0);
    s.pending.clear();
  }

  // Write the packets as one or more pages (a page holds at most 255 lacing values).
  void page(Stream& s, util::ByteWriter& w, const std::vector<Bytes>& packets, std::int64_t granule, std::uint8_t flags) {
    std::vector<std::uint8_t> lacing;
    Bytes body;
    bool continued = false;
    const auto emit = [&](bool final_page, bool next_continued) {
      util::ByteWriter p;
      const std::uint8_t page_flags = static_cast<std::uint8_t>((continued ? 1 : 0) | (final_page ? flags : (flags & 0x02)));
      p.text("OggS").u8(0).u8(page_flags).i64le(final_page ? granule : -1).u32le(s.serial).u32le(s.sequence++).u32le(0);
      p.u8(static_cast<std::uint8_t>(lacing.size()));
      for (const std::uint8_t l : lacing) p.u8(l);
      p.bytes(body);
      Bytes bytes = p.take();
      const std::uint32_t crc = ogg_crc(bytes);
      bytes[22] = static_cast<std::byte>(crc & 0xFF);
      bytes[23] = static_cast<std::byte>((crc >> 8) & 0xFF);
      bytes[24] = static_cast<std::byte>((crc >> 16) & 0xFF);
      bytes[25] = static_cast<std::byte>(crc >> 24);
      w.bytes(bytes);
      lacing.clear();
      body.clear();
      flags = static_cast<std::uint8_t>(flags & ~0x02);
      continued = next_continued;
    };
    for (std::size_t i = 0; i < packets.size(); ++i) {
      const Bytes& packet = packets[i];
      std::size_t left = packet.size();
      std::size_t offset = 0;
      while (true) {
        const std::size_t lace = std::min<std::size_t>(left, 255);
        lacing.push_back(static_cast<std::uint8_t>(lace));
        body.insert(body.end(), packet.begin() + static_cast<std::ptrdiff_t>(offset),
                    packet.begin() + static_cast<std::ptrdiff_t>(offset + lace));
        offset += lace;
        left -= lace;
        const bool done = lace < 255;
        if (lacing.size() == 255 && !(done && i + 1 == packets.size())) emit(false, !done);
        if (done) break;
      }
    }
    if (!lacing.empty() || packets.empty()) emit(true, false);
  }

  MuxSettings settings_;
  std::vector<Stream> streams_;
  std::vector<std::size_t> streams_order_;
  util::ByteWriter out_;
};

}  // namespace

std::unique_ptr<Muxer> make_ogg_muxer() { return std::make_unique<OggMuxer>(); }

}  // namespace cvt::video
