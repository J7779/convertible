// Convertible core - video family: MPEG transport stream (TS, M2TS) reader
// and writer, and the MPEG program stream (MPG, VOB) reader.
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

std::uint8_t at(ByteView d, std::uint64_t i) { return std::to_integer<std::uint8_t>(d[static_cast<std::size_t>(i)]); }

std::int64_t read_timestamp(ByteView d, std::uint64_t i) {
  return (static_cast<std::int64_t>((at(d, i) >> 1) & 7) << 30) | (static_cast<std::int64_t>(at(d, i + 1)) << 22) |
         (static_cast<std::int64_t>(at(d, i + 2) >> 1) << 15) | (static_cast<std::int64_t>(at(d, i + 3)) << 7) |
         (at(d, i + 4) >> 1);
}

// Undo 33-bit wrap-around relative to the previous value.
std::int64_t unwrap(std::int64_t value, std::optional<std::int64_t>& last) {
  constexpr std::int64_t period = std::int64_t{1} << 33;
  if (last) {
    while (value - *last < -period / 2) value += period;
    while (value - *last > period / 2) value -= period;
  }
  last = value;
  return value;
}

std::string codec_for_stream_type(int type, std::uint32_t registration) {
  switch (type) {
    case 0x01: return std::string(id::mpeg1video);
    case 0x02: return std::string(id::mpeg2video);
    case 0x03:
    case 0x04: return std::string(id::mp3);  // refined from the frame header (layer II or III)
    case 0x0F: return std::string(id::aac);
    case 0x10: return std::string(id::mpeg4);
    case 0x1B: return std::string(id::h264);
    case 0x24: return std::string(id::hevc);
    case 0x81: return std::string(id::ac3);
    case 0x87: return std::string(cid::eac3);
    case 0x06:
      if (registration == bits::fourcc("AC-3")) return std::string(id::ac3);
      if (registration == bits::fourcc("Opus")) return std::string(id::opus);
      if (registration == bits::fourcc("HEVC")) return std::string(id::hevc);
      return {};
    default: return {};
  }
}

bool is_video(const std::string& codec) {
  return codec == id::h264 || codec == id::hevc || codec == id::mpeg1video || codec == id::mpeg2video || codec == id::mpeg4;
}

// Key-frame test on the start of an access unit.
bool video_key(const std::string& codec, ByteView data) {
  if (codec == id::h264) return bits::h264_is_keyframe(bits::split_annexb(data));
  if (codec == id::hevc) return bits::hevc_is_keyframe(bits::split_annexb(data));
  if (codec == id::mpeg1video || codec == id::mpeg2video) {
    for (std::size_t i = 0; i + 5 < data.size(); ++i) {
      if (at(data, i) == 0 && at(data, i + 1) == 0 && at(data, i + 2) == 1 && at(data, i + 3) == 0) {
        return ((at(data, i + 5) >> 3) & 7) == 1;
      }
    }
    return false;
  }
  if (codec == id::mpeg4) {
    for (std::size_t i = 0; i + 4 < data.size(); ++i) {
      if (at(data, i) == 0 && at(data, i + 1) == 0 && at(data, i + 2) == 1 && at(data, i + 3) == 0xB6) {
        return (at(data, i + 4) >> 6) == 0;
      }
    }
    return false;
  }
  return true;
}

struct PesStream {
  std::size_t track = 0;
  EsStream es;
  std::vector<EsMarker> markers;
  std::vector<std::uint64_t> pes_starts;  // stream offsets where each PES payload begins
  std::optional<std::int64_t> last_pts;
  std::optional<std::int64_t> last_dts;
  // PES header being collected across TS packets.
  Bytes header;
  bool in_header = false;
  bool active = false;
};

// Parse a PES header; returns payload offset within `h` and fills timestamps, or nullopt if incomplete.
std::optional<std::size_t> parse_pes_header(ByteView h, std::optional<std::int64_t>& pts, std::optional<std::int64_t>& dts) {
  if (h.size() < 9) return std::nullopt;
  if (at(h, 0) != 0 || at(h, 1) != 0 || at(h, 2) != 1) return std::size_t{0};
  const std::uint8_t id = at(h, 3);
  if (id == 0xBC || id == 0xBE || id == 0xBF || id == 0xF0 || id == 0xF1 || id == 0xFF || id == 0xF2 || id == 0xF8) {
    return std::size_t{6};
  }
  if ((at(h, 6) & 0xC0) == 0x80) {  // MPEG-2
    const std::uint8_t flags = at(h, 7);
    const std::size_t length = at(h, 8);
    if (h.size() < 9 + length) return std::nullopt;
    if ((flags & 0x80) && length >= 5) pts = read_timestamp(h, 9);
    if ((flags & 0xC0) == 0xC0 && length >= 10) dts = read_timestamp(h, 14);
    return 9 + length;
  }
  // MPEG-1 header
  std::size_t i = 6;
  while (i < h.size() && at(h, i) == 0xFF && i < 6 + 16) ++i;
  if (i >= h.size()) return std::nullopt;
  if ((at(h, i) & 0xC0) == 0x40) i += 2;
  if (i >= h.size()) return std::nullopt;
  if ((at(h, i) & 0xF0) == 0x20) {
    if (h.size() < i + 5) return std::nullopt;
    pts = read_timestamp(h, i);
    i += 5;
  } else if ((at(h, i) & 0xF0) == 0x30) {
    if (h.size() < i + 10) return std::nullopt;
    pts = read_timestamp(h, i);
    dts = read_timestamp(h, i + 5);
    i += 10;
  } else {
    ++i;
  }
  return i;
}

// Turn collected elementary streams into packets.
void packetize(Demuxed& out, std::vector<TrackInfo>& infos, std::vector<PesStream*> streams, bool pes_aligned_video) {
  for (PesStream* s : streams) {
    TrackInfo& t = infos[s->track];
    t.timescale = 90000;
    if (is_video(t.codec)) {
      if (!pes_aligned_video && (t.codec == id::mpeg1video || t.codec == id::mpeg2video)) {
        split_mpeg12_video(out, t, s->es, s->markers, 25.0);
      } else if (t.codec == id::hevc || (!pes_aligned_video && t.codec == id::h264)) {
        split_annexb_video(out, t, s->es, s->markers, 25.0);
      } else {
        for (std::size_t m = 0; m < s->markers.size(); ++m) {
          const std::uint64_t begin = s->markers[m].offset;
          const std::uint64_t end = m + 1 < s->markers.size() ? s->markers[m + 1].offset : s->es.size;
          PacketBuilder builder(out);
          for (const Span& span : s->es.slice(begin, end)) builder.input(span.offset, span.size);
          if (builder.empty()) continue;
          const Bytes head = s->es.read(out.input, begin, static_cast<std::size_t>(std::min<std::uint64_t>(end - begin, 65536)));
          const std::int64_t pts = s->markers[m].pts.value_or(t.packets.empty() ? 0 : t.packets.back().pts);
          t.packets.push_back(builder.finish(pts, s->markers[m].dts.value_or(pts), 0, video_key(t.codec, head)));
        }
      }
      if (t.width == 0) {
        const Bytes head = s->es.read(out.input, 0, 65536);
        if (const auto seq = bits::parse_mpeg12_sequence(head); seq && !(t.codec == id::h264 || t.codec == id::hevc)) {
          t.width = seq->width;
          t.height = seq->height;
          if (seq->fps_num > 0) t.fps = ir::Rational{seq->fps_num, seq->fps_den};
        }
      }
    } else {
      split_audio_frames(out, t, s->es, s->markers);
    }
  }
}

}  // namespace

// =========================================================================== TS reader

Result<std::shared_ptr<Demuxed>> demux_ts(ByteView input, const ReadSettings& settings) {
  std::size_t packet_size = 0;
  std::size_t lead = 0;
  for (const std::size_t size : {std::size_t{188}, std::size_t{192}, std::size_t{204}}) {
    const std::size_t l = size == 192 ? 4 : 0;
    if (input.size() >= l + 1 && at(input, l) == 0x47 && (input.size() < l + size + 1 || at(input, l + size) == 0x47)) {
      packet_size = size;
      lead = l;
      break;
    }
  }
  if (packet_size == 0) return fail(Status::Decode, "not an MPEG transport stream (no sync bytes)");
  auto out = std::make_shared<Demuxed>();
  out->input = input;
  out->format_id = "ts";
  std::optional<std::uint16_t> pmt_pid;
  std::map<std::uint16_t, PesStream> pes;  // by PID
  std::vector<std::uint16_t> order;
  std::vector<TrackInfo> infos;
  bool pmt_seen = false;
  Bytes section;  // PAT/PMT section being collected
  std::uint16_t section_pid = 0xFFFF;
  const auto handle_section = [&](ByteView sec, std::uint16_t pid) {
    if (sec.size() < 8) return;
    const std::uint8_t table = at(sec, 0);
    const std::size_t length = ((at(sec, 1) & 0x0F) << 8) | at(sec, 2);
    if (length + 3 > sec.size() || length < 9) return;
    const ByteView body = sec.subspan(8, length - 5 - 4);  // without header and CRC
    if (pid == 0 && table == 0 && !pmt_pid) {
      for (std::size_t i = 0; i + 4 <= body.size(); i += 4) {
        const std::uint16_t program = static_cast<std::uint16_t>((at(body, i) << 8) | at(body, i + 1));
        const std::uint16_t p = static_cast<std::uint16_t>(((at(body, i + 2) & 0x1F) << 8) | at(body, i + 3));
        if (program != 0) {
          pmt_pid = p;
          break;
        }
      }
    } else if (pmt_pid && pid == *pmt_pid && table == 2 && !pmt_seen) {
      pmt_seen = true;
      if (body.size() < 4) return;
      const std::size_t info_length = ((at(body, 2) & 0x0F) << 8) | at(body, 3);
      std::size_t i = 4 + info_length;
      while (i + 5 <= body.size()) {
        const int type = at(body, i);
        const std::uint16_t epid = static_cast<std::uint16_t>(((at(body, i + 1) & 0x1F) << 8) | at(body, i + 2));
        const std::size_t es_length = ((at(body, i + 3) & 0x0F) << 8) | at(body, i + 4);
        if (i + 5 + es_length > body.size()) break;
        std::uint32_t registration = 0;
        std::string language;
        bool ac3_descriptor = false;
        for (std::size_t d = i + 5; d + 2 <= i + 5 + es_length;) {
          const std::uint8_t tag = at(body, d);
          const std::uint8_t dl = at(body, d + 1);
          if (d + 2 + dl > i + 5 + es_length) break;
          if (tag == 0x05 && dl >= 4) {
            registration = (static_cast<std::uint32_t>(at(body, d + 2)) << 24) | (static_cast<std::uint32_t>(at(body, d + 3)) << 16) |
                           (static_cast<std::uint32_t>(at(body, d + 4)) << 8) | at(body, d + 5);
          }
          if (tag == 0x0A && dl >= 3) language = std::string(as_chars(body.subspan(d + 2, 3)));
          if (tag == 0x6A) ac3_descriptor = true;
          d += 2 + dl;
        }
        std::string codec = codec_for_stream_type(type, registration);
        if (type == 0x06 && ac3_descriptor) codec = std::string(id::ac3);
        TrackInfo t;
        t.codec = codec;
        t.tag = std::format("stream type 0x{:02x}", type);
        t.type = is_video(codec) ? ir::TrackType::Video : (codec.empty() ? ir::TrackType::Subtitle : ir::TrackType::Audio);
        t.language = language == "und" ? std::string() : language;
        t.id = epid;
        t.timescale = 90000;
        if (!codec.empty() && !pes.contains(epid)) {
          PesStream s;
          s.track = infos.size();
          pes.emplace(epid, std::move(s));
          order.push_back(epid);
          infos.push_back(std::move(t));
        }
        i += 5 + es_length;
      }
    }
  };

  std::uint64_t count = 0;
  for (std::uint64_t pos = lead; pos + 188 <= input.size(); pos += packet_size) {
    if (at(input, pos) != 0x47) {
      // Lost sync: look for the next sync byte.
      std::uint64_t next = pos + 1;
      while (next + 188 <= input.size() && at(input, next) != 0x47) ++next;
      if (next + 188 > input.size()) break;
      pos = next - packet_size;
      continue;
    }
    if ((++count & 4095) == 0 && !settings.progress.report(static_cast<double>(pos) / static_cast<double>(input.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    const bool pusi = (at(input, pos + 1) & 0x40) != 0;
    const std::uint16_t pid = static_cast<std::uint16_t>(((at(input, pos + 1) & 0x1F) << 8) | at(input, pos + 2));
    const int afc = (at(input, pos + 3) >> 4) & 3;
    std::uint64_t payload = pos + 4;
    if (afc == 2 || afc == 0) continue;
    if (afc == 3) payload += 1 + at(input, pos + 4);
    if (payload >= pos + 188) continue;
    const ByteView data = input.subspan(static_cast<std::size_t>(payload), static_cast<std::size_t>(pos + 188 - payload));
    if (pid == 0 || (pmt_pid && pid == *pmt_pid)) {
      if (pusi) {
        const std::size_t pointer = at(data, 0);
        if (pointer + 1 > data.size()) continue;
        section.assign(data.begin() + 1 + static_cast<std::ptrdiff_t>(pointer), data.end());
        section_pid = pid;
      } else if (section_pid == pid) {
        section.insert(section.end(), data.begin(), data.end());
      }
      if (section_pid == pid && section.size() >= 3) {
        const std::size_t need = (((at(section, 1) & 0x0F) << 8) | at(section, 2)) + 3;
        if (section.size() >= need) {
          handle_section(section, pid);
          section.clear();
          section_pid = 0xFFFF;
        }
      }
      continue;
    }
    const auto found = pes.find(pid);
    if (found == pes.end()) continue;
    PesStream& s = found->second;
    if (pusi) {
      s.header.assign(data.begin(), data.end());
      s.in_header = true;
      s.active = true;
    } else if (s.in_header) {
      s.header.insert(s.header.end(), data.begin(), data.end());
    } else if (s.active) {
      s.es.add(payload, data.size());
      continue;
    } else {
      continue;
    }
    std::optional<std::int64_t> pts;
    std::optional<std::int64_t> dts;
    const auto header_end = parse_pes_header(s.header, pts, dts);
    if (!header_end) continue;  // header continues in the next packet
    s.in_header = false;
    if (pts) pts = unwrap(*pts, s.last_pts);
    if (dts) dts = unwrap(*dts, s.last_dts);
    s.markers.push_back(EsMarker{s.es.size, pts, dts});
    // The header may have spanned several packets: map the payload back onto this packet.
    const std::uint64_t collected = s.header.size();
    const std::uint64_t in_this = data.size();
    const std::uint64_t payload_in_header = collected - *header_end;
    if (payload_in_header <= in_this) {
      s.es.add(payload + (in_this - payload_in_header), payload_in_header);
    }
    s.header.clear();
  }
  if (!pmt_seen) return fail(Status::Decode, "the transport stream has no program map table");
  std::vector<PesStream*> streams;
  for (const std::uint16_t pid : order) streams.push_back(&pes[pid]);
  packetize(*out, infos, streams, true);
  for (TrackInfo& t : infos) {
    if (t.codec == id::h264 && !t.packets.empty()) {
      // Picture size from the in-band SPS.
      const Bytes first = out->packet_bytes(t.packets.front());
      for (const ByteView nal : bits::split_annexb(first)) {
        if (const auto sps = bits::parse_h264_sps(nal)) {
          t.width = sps->width;
          t.height = sps->height;
        }
      }
    }
    out->tracks.push_back(std::move(t));
  }
  CVT_TRY(finish_demux(*out, "MPEG-TS"));
  return out;
}

// =========================================================================== PS reader

Result<std::shared_ptr<Demuxed>> demux_ps(ByteView input, const ReadSettings& settings) {
  if (input.size() < 4 || at(input, 0) != 0 || at(input, 1) != 0 || at(input, 2) != 1 || (at(input, 3) != 0xBA && at(input, 3) != 0xB3)) {
    return fail(Status::Decode, "not an MPEG program stream");
  }
  if (at(input, 3) == 0xB3) return demux_raw(input, settings, id::mpeg2video, 25.0);
  auto out = std::make_shared<Demuxed>();
  out->input = input;
  out->format_id = "mpg";
  std::map<int, PesStream> streams;  // by stream id (private stream 1 sub-streams as 0x100 + sub id)
  std::vector<int> order;
  std::vector<TrackInfo> infos;
  std::uint64_t pos = 0;
  std::uint64_t count = 0;
  while (pos + 4 <= input.size()) {
    if (at(input, pos) != 0 || at(input, pos + 1) != 0 || at(input, pos + 2) != 1) {
      ++pos;  // resynchronise
      continue;
    }
    if ((++count & 4095) == 0 && !settings.progress.report(static_cast<double>(pos) / static_cast<double>(input.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    const std::uint8_t code = at(input, pos + 3);
    if (code == 0xBA) {
      if (pos + 14 <= input.size() && (at(input, pos + 4) & 0xC0) == 0x40) {
        pos += 14 + (at(input, pos + 13) & 7);
      } else {
        pos += 12;
      }
      continue;
    }
    if (code == 0xB9) {
      pos += 4;
      continue;
    }
    if (code < 0xBB || pos + 6 > input.size()) {
      ++pos;
      continue;
    }
    const std::uint64_t length = (static_cast<std::uint64_t>(at(input, pos + 4)) << 8) | at(input, pos + 5);
    const std::uint64_t end = std::min<std::uint64_t>(pos + 6 + length, input.size());
    const bool video = code >= 0xE0 && code <= 0xEF;
    const bool audio = code >= 0xC0 && code <= 0xDF;
    if (video || audio || code == 0xBD) {
      std::optional<std::int64_t> pts;
      std::optional<std::int64_t> dts;
      const ByteView packet = input.subspan(static_cast<std::size_t>(pos), static_cast<std::size_t>(end - pos));
      const auto header = parse_pes_header(packet, pts, dts);
      if (header && *header <= packet.size()) {
        std::uint64_t payload = pos + *header;
        int key = code;
        std::string codec;
        ir::TrackType type = ir::TrackType::Audio;
        if (code == 0xBD) {
          if (payload >= end) {
            pos = end;
            continue;
          }
          const std::uint8_t sub = at(input, payload);
          key = 0x100 + sub;
          if (sub >= 0x80 && sub <= 0x87) {
            codec = std::string(id::ac3);
            payload += 4;
          } else {
            pos = end;
            continue;  // DTS, LPCM and subpictures are not supported
          }
        } else if (video) {
          type = ir::TrackType::Video;
          const ByteView head = input.subspan(static_cast<std::size_t>(payload), static_cast<std::size_t>(end - payload));
          codec = std::string(id::mpeg2video);
          if (head.size() >= 5 && bits::starts_with_start_code(head)) {
            const std::size_t sc = at(head, 2) == 1 ? 3 : 4;
            const std::uint8_t b = at(head, sc);
            if (sc == 4 && ((b & 0x1F) == 9 || (b & 0x1F) == 7)) codec = std::string(id::h264);
            if (b == 0xB0 || b <= 0x2F) codec = b == 0x00 ? std::string(id::mpeg2video) : std::string(id::mpeg4);
          }
        } else {
          codec = std::string(id::mp3);
        }
        if (!streams.contains(key)) {
          PesStream s;
          s.track = infos.size();
          streams.emplace(key, std::move(s));
          order.push_back(key);
          TrackInfo t;
          t.codec = codec;
          t.type = type;
          t.tag = std::format("stream 0x{:02x}", code);
          t.id = static_cast<std::uint32_t>(key);
          infos.push_back(std::move(t));
        }
        PesStream& s = streams[key];
        if (pts) pts = unwrap(*pts, s.last_pts);
        if (dts) dts = unwrap(*dts, s.last_dts);
        s.markers.push_back(EsMarker{s.es.size, pts, dts});
        if (payload < end) s.es.add(payload, end - payload);
      }
    }
    pos = std::max(end, pos + 4);
  }
  // Decide MPEG-1 or MPEG-2 from the sequence header.
  for (const int key : order) {
    PesStream& s = streams[key];
    TrackInfo& t = infos[s.track];
    if (t.codec == id::mpeg2video) {
      const Bytes head = s.es.read(input, 0, 4096);
      if (const auto seq = bits::parse_mpeg12_sequence(head)) {
        t.codec = std::string(seq->mpeg2 ? id::mpeg2video : id::mpeg1video);
        t.width = seq->width;
        t.height = seq->height;
        if (seq->fps_num > 0) t.fps = ir::Rational{seq->fps_num, seq->fps_den};
      }
    }
  }
  std::vector<PesStream*> list;
  for (const int key : order) list.push_back(&streams[key]);
  packetize(*out, infos, list, false);
  for (TrackInfo& t : infos) out->tracks.push_back(std::move(t));
  CVT_TRY(finish_demux(*out, "MPEG-PS"));
  return out;
}

// =========================================================================== TS writer

namespace {

std::uint32_t crc32_mpeg(ByteView data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::byte b : data) {
    crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b)) << 24;
    for (int i = 0; i < 8; ++i) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
  }
  return crc;
}

void put_timestamp(util::ByteWriter& w, int prefix, std::int64_t ts) {
  const auto v = static_cast<std::uint64_t>(ts) & ((std::uint64_t{1} << 33) - 1);
  w.u8(static_cast<std::uint8_t>((prefix << 4) | (((v >> 30) & 7) << 1) | 1));
  w.u16be(static_cast<std::uint16_t>((((v >> 15) & 0x7FFF) << 1) | 1));
  w.u16be(static_cast<std::uint16_t>(((v & 0x7FFF) << 1) | 1));
}

class TsMuxer final : public Muxer {
 public:
  Result<void> begin(std::vector<MuxTrack> tracks, const MuxSettings& settings) override {
    settings_ = settings;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      MuxTrack& t = tracks[i];
      if (!container_accepts("ts", t.type, t.codec)) return fail(Status::Encode, "MPEG-TS cannot store {}", t.codec);
      Out o;
      Packaging p;
      p.length_prefixed = false;
      p.adts = true;
      p.access_unit_delimiters = true;
      o.adapter = StreamAdapter(t.codec, t.extradata, p);
      o.pid = static_cast<std::uint16_t>(0x100 + i);
      o.desc = std::move(t);
      if (o.desc.type == ir::TrackType::Video && !pcr_pid_) pcr_pid_ = o.pid;
      streams_.push_back(std::move(o));
    }
    if (!pcr_pid_ && !streams_.empty()) pcr_pid_ = streams_.front().pid;
    return {};
  }

  Result<void> write(std::size_t index, const ir::Packet& packet) override {
    Out& s = streams_[index];
    CVT_TRY_ASSIGN(Bytes data, s.adapter.convert(packet.bytes, packet.keyframe));
    std::int64_t pts = rescale(packet.pts, s.desc.timescale, 90000);
    std::int64_t dts = rescale(packet.dts, s.desc.timescale, 90000);
    if (!started_) {
      started_ = true;
      offset_ = 126000 + std::max<std::int64_t>(0, -dts);
    }
    pts += offset_;
    dts += offset_;
    if (pts < 0 || dts < 0) return {};
    if (packets_written_ == 0 || dts - last_tables_ >= 45000) {
      write_tables();
      last_tables_ = dts;
    }
    const bool video = s.desc.type == ir::TrackType::Video;
    const std::uint8_t stream_id = video ? 0xE0 : s.desc.codec == id::ac3 ? 0xBD : 0xC0;
    util::ByteWriter pes;
    const bool with_dts = video && dts != pts;
    const std::size_t header = 3 + (with_dts ? 10 : 5);
    const std::size_t total = header + data.size();
    pes.u8(0).u8(0).u8(1).u8(stream_id).u16be(video || total > 0xFFFF ? 0 : static_cast<std::uint16_t>(total));
    pes.u8(0x84).u8(with_dts ? 0xC0 : 0x80).u8(static_cast<std::uint8_t>(with_dts ? 10 : 5));
    put_timestamp(pes, with_dts ? 3 : 2, pts);
    if (with_dts) put_timestamp(pes, 1, dts);
    pes.bytes(data);
    const bool pcr = s.pid == *pcr_pid_;
    packetize(s, pes.view(), pcr ? std::optional<std::int64_t>(dts > 63000 ? dts - 63000 : 0) : std::nullopt,
              packet.keyframe && video);
    if (out_.size() > settings_.limits.max_output_bytes) return fail(Status::Limit, "the MPEG-TS output exceeds the output size limit");
    return {};
  }

  void set_extradata(std::size_t track, Bytes extradata) override {
    Packaging p;
    p.length_prefixed = false;
    p.adts = true;
    p.access_unit_delimiters = true;
    streams_[track].adapter = StreamAdapter(streams_[track].desc.codec, std::move(extradata), p);
  }

  Result<Bytes> finish() override {
    if (packets_written_ == 0) return fail(Status::Encode, "nothing to write: the video has no packets");
    return out_.take();
  }

 private:
  struct Out {
    MuxTrack desc;
    StreamAdapter adapter;
    std::uint16_t pid = 0;
    std::uint8_t cc = 0;
  };

  void ts_packet(std::uint16_t pid, bool start, std::uint8_t& cc, ByteView payload, std::optional<std::int64_t> pcr, bool random_access) {
    util::ByteWriter p;
    std::size_t adaptation = 0;
    if (pcr || random_access) adaptation = 2 + (pcr ? 6 : 0);
    const std::size_t room = 184 - (adaptation > 0 ? adaptation : 0);
    std::size_t take = std::min(room, payload.size());
    std::size_t stuffing = room - take;
    if (stuffing > 0 && adaptation == 0) {
      adaptation = 1;
      take = std::min<std::size_t>(183, payload.size());
      stuffing = 183 - take;
    }
    p.u8(0x47).u8(static_cast<std::uint8_t>((start ? 0x40 : 0) | ((pid >> 8) & 0x1F))).u8(static_cast<std::uint8_t>(pid & 0xFF));
    p.u8(static_cast<std::uint8_t>((adaptation > 0 ? 0x30 : 0x10) | (cc & 0x0F)));
    cc = static_cast<std::uint8_t>((cc + 1) & 0x0F);
    if (adaptation > 0) {
      const std::size_t length = adaptation - 1 + stuffing;
      p.u8(static_cast<std::uint8_t>(length));
      if (length > 0) {
        p.u8(static_cast<std::uint8_t>((random_access ? 0x40 : 0) | (pcr ? 0x10 : 0)));
        if (pcr) {
          const auto base = static_cast<std::uint64_t>(*pcr) & ((std::uint64_t{1} << 33) - 1);
          p.u32be(static_cast<std::uint32_t>(base >> 1)).u8(static_cast<std::uint8_t>(((base & 1) << 7) | 0x7E)).u8(0);
        }
        const std::size_t fill = length - 1 - (pcr ? 6 : 0);
        for (std::size_t i = 0; i < fill; ++i) p.u8(0xFF);
      }
    }
    p.bytes(payload.first(take));
    out_.bytes(p.view());
    ++packets_written_;
  }

  void packetize(Out& s, ByteView pes, std::optional<std::int64_t> pcr, bool random_access) {
    bool first = true;
    while (!pes.empty() || first) {
      const std::size_t before = out_.size();
      ts_packet(s.pid, first, s.cc, pes, first ? pcr : std::nullopt, first && random_access);
      // How much payload went in: the packet's tail.
      const ByteView written = out_.view().subspan(before);
      std::size_t header = 4;
      if ((std::to_integer<int>(written[3]) & 0x20) != 0) header += 1 + std::to_integer<std::size_t>(written[4]);
      pes = pes.subspan(std::min(pes.size(), 188 - header));
      first = false;
    }
  }

  void write_tables() {
    std::uint8_t dummy = 0;
    {
      util::ByteWriter sec;
      sec.u8(0).u16be(0xB000 | 13).u16be(1).u8(0xC1).u8(0).u8(0);
      sec.u16be(1).u16be(0xE000 | 0x1000);
      sec.u32be(crc32_mpeg(sec.view()));
      util::ByteWriter payload;
      payload.u8(0).bytes(sec.view());
      ts_packet(0, true, pat_cc_, payload.view(), std::nullopt, false);
    }
    {
      util::ByteWriter streams;
      for (const Out& s : streams_) {
        int type = 0x1B;
        const std::string& c = s.desc.codec;
        if (c == id::hevc) type = 0x24;
        if (c == id::mpeg2video) type = 0x02;
        if (c == id::mpeg1video) type = 0x01;
        if (c == id::mpeg4) type = 0x10;
        if (c == id::aac) type = 0x0F;
        if (c == id::mp2 || c == id::mp3) type = 0x04;
        if (c == id::ac3) type = 0x81;
        util::ByteWriter desc;
        if (c == id::ac3) desc.u8(0x05).u8(4).text("AC-3");
        if (!s.desc.language.empty() && s.desc.language.size() == 3) desc.u8(0x0A).u8(4).text(s.desc.language).u8(0);
        streams.u8(static_cast<std::uint8_t>(type)).u16be(0xE000 | s.pid).u16be(static_cast<std::uint16_t>(0xF000 | desc.size()));
        streams.bytes(desc.view());
      }
      util::ByteWriter sec;
      sec.u8(2).u16be(static_cast<std::uint16_t>(0xB000 | (13 + streams.size()))).u16be(1).u8(0xC1).u8(0).u8(0);
      sec.u16be(static_cast<std::uint16_t>(0xE000 | *pcr_pid_)).u16be(0xF000);
      sec.bytes(streams.view());
      sec.u32be(crc32_mpeg(sec.view()));
      util::ByteWriter payload;
      payload.u8(0).bytes(sec.view());
      ts_packet(0x1000, true, pmt_cc_, payload.view(), std::nullopt, false);
    }
    (void)dummy;
  }

  MuxSettings settings_;
  std::vector<Out> streams_;
  std::optional<std::uint16_t> pcr_pid_;
  util::ByteWriter out_;
  std::uint8_t pat_cc_ = 0;
  std::uint8_t pmt_cc_ = 0;
  bool started_ = false;
  std::int64_t offset_ = 0;
  std::int64_t last_tables_ = 0;
  std::uint64_t packets_written_ = 0;
};

}  // namespace

std::unique_ptr<Muxer> make_ts_muxer() { return std::make_unique<TsMuxer>(); }

}  // namespace cvt::video
