// Convertible core - video family: splitting video elementary streams into
// access units (MPEG-1/2 pictures, H.264/HEVC access units) for raw files and
// for the program/transport streams whose packets do not align with frames.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <optional>

#include "families/video/bitstream.hpp"
#include "families/video/containers.hpp"

namespace cvt::video {

void EsStream::add(std::uint64_t offset, std::uint64_t length) {
  if (length == 0) return;
  size += length;
  if (!spans.empty() && !spans.back().owned && spans.back().offset + spans.back().size == offset &&
      std::uint64_t{spans.back().size} + length <= 0x7FFFFFFFu) {
    spans.back().size += static_cast<std::uint32_t>(length);
    return;
  }
  while (length > 0) {
    const auto piece = static_cast<std::uint32_t>(std::min<std::uint64_t>(length, 0x7FFFFFFFu));
    spans.push_back(Span{offset, piece, false});
    offset += piece;
    length -= piece;
  }
}

std::vector<Span> EsStream::slice(std::uint64_t begin, std::uint64_t end) const {
  std::vector<Span> out;
  std::uint64_t at = 0;
  for (const Span& s : spans) {
    const std::uint64_t s_end = at + s.size;
    if (s_end > begin && at < end) {
      const std::uint64_t from = std::max(at, begin);
      const std::uint64_t to = std::min(s_end, end);
      out.push_back(Span{s.offset + (from - at), static_cast<std::uint32_t>(to - from), s.owned});
    }
    at = s_end;
    if (at >= end) break;
  }
  return out;
}

Bytes EsStream::read(ByteView input, std::uint64_t position, std::size_t count) const {
  Bytes out;
  std::uint64_t at = 0;
  for (const Span& s : spans) {
    const std::uint64_t s_end = at + s.size;
    if (s_end > position && out.size() < count) {
      const std::uint64_t from = std::max(at, position) - at;
      for (std::uint64_t k = from; k < s.size && out.size() < count; ++k) {
        const std::uint64_t abs = s.offset + k;
        if (abs >= input.size()) break;
        out.push_back(input[static_cast<std::size_t>(abs)]);
      }
    }
    at = s_end;
    if (out.size() >= count) break;
  }
  return out;
}

namespace {

struct StartCode {
  std::uint64_t position = 0;  // of the 00 00 01 prefix, in stream offsets
  std::uint8_t code = 0;
  std::uint8_t next1 = 0;  // the two bytes after the code byte
  std::uint8_t next2 = 0;
};

std::vector<StartCode> scan(ByteView input, const EsStream& es) {
  std::vector<StartCode> out;
  std::uint32_t window = 0xFFFFFF;  // last three bytes
  std::uint64_t pos = 0;
  std::vector<std::size_t> pending;  // start codes still waiting for their trailing bytes
  int pending_need = 0;
  for (const Span& s : es.spans) {
    if (s.owned || s.offset >= input.size()) {
      pos += s.size;
      continue;
    }
    const std::uint64_t n = std::min<std::uint64_t>(s.size, input.size() - s.offset);
    const auto* p = reinterpret_cast<const std::uint8_t*>(input.data() + s.offset);
    for (std::uint64_t i = 0; i < n; ++i, ++pos) {
      const std::uint8_t b = p[i];
      if (pending_need > 0 && !out.empty()) {
        StartCode& last = out.back();
        if (pending_need == 3) {
          last.code = b;
        } else if (pending_need == 2) {
          last.next1 = b;
        } else {
          last.next2 = b;
        }
        --pending_need;
      }
      if ((window & 0xFFFFFF) == 0x000001 && pending_need == 0) {
        // `b` is the code byte; the prefix began three bytes earlier.
        out.push_back(StartCode{pos - 3, b, 0, 0});
        pending_need = 2;
      }
      window = (window << 8) | b;
    }
  }
  (void)pending;
  return out;
}

struct Frame {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  bool key = false;
  int type = 0;  // MPEG-2 picture coding type (1 I, 2 P, 3 B)
  int tref = 0;
  bool gop = false;
  std::optional<std::int64_t> pts;
  std::optional<std::int64_t> dts;
};

void attach_markers(std::vector<Frame>& frames, const std::vector<EsMarker>& markers) {
  // A PES timestamp belongs to the first access unit starting in that PES.
  std::size_t f = 0;
  for (std::size_t m = 0; m < markers.size(); ++m) {
    if (!markers[m].pts) continue;
    const std::uint64_t at = markers[m].offset;
    const std::uint64_t limit = m + 1 < markers.size() ? markers[m + 1].offset : UINT64_MAX;
    while (f < frames.size() && frames[f].begin < at) ++f;
    if (f < frames.size() && frames[f].begin < limit && !frames[f].pts) {
      frames[f].pts = markers[m].pts;
      frames[f].dts = markers[m].dts ? markers[m].dts : markers[m].pts;
    }
  }
}

void emit(Demuxed& demuxed, TrackInfo& track, const EsStream& es, const std::vector<Frame>& frames) {
  for (const Frame& fr : frames) {
    PacketBuilder builder(demuxed);
    for (const Span& s : es.slice(fr.begin, fr.end)) builder.input(s.offset, s.size);
    if (builder.empty()) continue;
    track.packets.push_back(builder.finish(fr.pts.value_or(0), fr.dts.value_or(fr.pts.value_or(0)), 0, fr.key));
  }
}

std::int64_t frame_ticks(double fps) { return static_cast<std::int64_t>(std::llround(90000.0 / (fps > 0 ? fps : 25.0))); }

}  // namespace

void split_mpeg12_video(Demuxed& demuxed, TrackInfo& track, const EsStream& es, const std::vector<EsMarker>& markers,
                        double fallback_fps) {
  const std::vector<StartCode> codes = scan(demuxed.input, es);
  std::vector<Frame> frames;
  std::optional<std::uint64_t> header_start;
  bool saw_gop = false;
  for (const StartCode& c : codes) {
    if (c.code == 0xB3 || c.code == 0xB8) {
      if (!header_start) header_start = c.position;
      if (c.code == 0xB8) saw_gop = true;
    } else if (c.code == 0x00) {
      const std::uint64_t cut = header_start.value_or(c.position);
      if (!frames.empty()) frames.back().end = cut;
      Frame f;
      f.begin = frames.empty() ? 0 : cut;
      f.end = es.size;
      f.tref = (c.next1 << 2) | (c.next2 >> 6);
      f.type = (c.next2 >> 3) & 7;
      f.key = f.type == 1;
      f.gop = saw_gop || frames.empty();
      frames.push_back(f);
      header_start.reset();
      saw_gop = false;
    } else if (c.code >= 0x01 && c.code <= 0xAF) {
      header_start.reset();
      saw_gop = false;
    }
  }
  if (frames.empty()) return;
  attach_markers(frames, markers);
  double fps = fallback_fps;
  if (const auto seq = bits::parse_mpeg12_sequence(es.read(demuxed.input, 0, 256)); seq && seq->fps_num > 0) {
    fps = static_cast<double>(seq->fps_num) / static_cast<double>(seq->fps_den);
  }
  const std::int64_t d = frame_ticks(fps);
  const int delay = std::ranges::any_of(frames, [](const Frame& f) { return f.type == 3; }) ? 1 : 0;
  // Decode times advance one frame per picture; presentation times follow the
  // temporal reference within each group of pictures.
  std::vector<std::size_t> gop_first(frames.size(), 0);
  for (std::size_t i = 1; i < frames.size(); ++i) gop_first[i] = frames[i].gop ? i : gop_first[i - 1];
  std::int64_t d0 = -static_cast<std::int64_t>(delay) * d;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (frames[i].pts) {
      d0 = *frames[i].pts - (static_cast<std::int64_t>(frames[i].tref) + delay) * d - static_cast<std::int64_t>(gop_first[i]) * d;
      break;
    }
  }
  for (std::size_t i = 0; i < frames.size(); ++i) {
    Frame& f = frames[i];
    const std::int64_t gop_dts = d0 + static_cast<std::int64_t>(gop_first[i]) * d;
    if (!f.pts) f.pts = gop_dts + (static_cast<std::int64_t>(f.tref) + delay) * d;
    if (!f.dts || f.dts == f.pts) f.dts = std::min(*f.pts, d0 + static_cast<std::int64_t>(i) * d);
  }
  track.timescale = 90000;
  if (track.fps.num == 0 && fps > 0) track.fps = ir::Rational{static_cast<std::int64_t>(std::llround(fps * 1000)), 1000};
  emit(demuxed, track, es, frames);
}

void split_annexb_video(Demuxed& demuxed, TrackInfo& track, const EsStream& es, const std::vector<EsMarker>& markers,
                        double fallback_fps) {
  const bool hevc = track.codec == "hevc";
  const std::vector<StartCode> codes = scan(demuxed.input, es);
  std::vector<Frame> frames;
  bool has_vcl = false;
  for (const StartCode& c : codes) {
    // Include a preceding zero byte of a 4-byte start code in the new unit.
    std::uint64_t at = c.position;
    if (at > 0 && !hevc) {  // HEVC: the zero byte stays with the previous unit, as in common tools
      const Bytes before = es.read(demuxed.input, at - 1, 1);
      if (!before.empty() && std::to_integer<int>(before[0]) == 0) --at;
    }
    bool starts_au = false;
    bool vcl = false;
    bool key = false;
    if (hevc) {
      const int type = (c.code >> 1) & 0x3F;
      vcl = type <= 31;
      key = type >= 16 && type <= 23;
      if (vcl) {
        // c.next1 is the second header byte; the slice header starts after it.
        const Bytes after = es.read(demuxed.input, c.position + 5, 1);
        const bool first_slice = !after.empty() && (std::to_integer<int>(after[0]) & 0x80) != 0;
        starts_au = first_slice && has_vcl;
      } else if (type == 35 || type == 32 || type == 33 || type == 34 || type == 39 || (type >= 41 && type <= 44) ||
                 (type >= 48 && type <= 55)) {
        starts_au = has_vcl;
      }
    } else {
      const int type = c.code & 0x1F;
      vcl = type == 1 || type == 5;
      key = type == 5;
      if (vcl) {
        const bool first_mb_zero = (c.next1 & 0x80) != 0;
        starts_au = first_mb_zero && has_vcl;
      } else if (type == 9 || type == 7 || type == 8 || type == 6 || type == 14 || (type >= 15 && type <= 18)) {
        starts_au = has_vcl;
      }
    }
    if (frames.empty() || starts_au) {
      if (!frames.empty()) frames.back().end = at;
      Frame f;
      f.begin = frames.empty() ? 0 : at;
      f.end = es.size;
      frames.push_back(f);
      has_vcl = false;
    }
    if (vcl) has_vcl = true;
    if (key) frames.back().key = true;
  }
  if (frames.empty()) return;
  attach_markers(frames, markers);
  const std::int64_t d = frame_ticks(fallback_fps);
  std::int64_t next = 0;
  for (Frame& f : frames) {
    if (f.pts) {
      next = f.dts.value_or(*f.pts) + d;
      continue;
    }
    f.pts = next;
    f.dts = next;
    next += d;
  }
  track.timescale = 90000;
  emit(demuxed, track, es, frames);
}

void derive_h264_pts(Demuxed& demuxed, TrackInfo& track) {
  const std::size_t n = track.packets.size();
  if (track.codec != "h264" || n < 2) return;
  bits::H264PocTracker tracker;
  int length_size = 0;
  if (const auto sets = bits::parse_avcc(track.extradata)) {
    length_size = sets->length_size;
    std::vector<ByteView> config;
    for (const Bytes& s : sets->sps) config.emplace_back(s);
    for (const Bytes& p : sets->pps) config.emplace_back(p);
    (void)tracker.next(config);
  }
  std::vector<std::pair<std::int64_t, bool>> pocs;
  pocs.reserve(n);
  for (const PacketRef& ref : track.packets) {
    const Bytes data = demuxed.packet_bytes(ref);
    std::vector<ByteView> nals;
    if (length_size == 0 || bits::starts_with_start_code(data)) {
      nals = bits::split_annexb(data);
    } else if (auto split = bits::split_length_prefixed(data, length_size)) {
      nals = std::move(*split);
    }
    const auto poc = tracker.next(nals);
    if (!poc) return;  // unsupported stream: keep presentation = decode order
    pocs.push_back(*poc);
  }
  // Rank pictures by POC within each IDR period.
  std::vector<std::size_t> rank(n, 0);
  std::size_t start = 0;
  std::int64_t delay = 0;
  for (std::size_t i = 1; i <= n; ++i) {
    if (i < n && !pocs[i].second) continue;
    std::vector<std::size_t> order(i - start);
    for (std::size_t k = 0; k < order.size(); ++k) order[k] = start + k;
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return pocs[a].first < pocs[b].first; });
    for (std::size_t k = 0; k < order.size(); ++k) {
      rank[order[k]] = start + k;
      delay = std::max<std::int64_t>(delay, static_cast<std::int64_t>(order[k]) - static_cast<std::int64_t>(start + k));
    }
    start = i;
  }
  bool reordered = false;
  for (std::size_t i = 0; i < n; ++i) reordered = reordered || rank[i] != i;
  if (!reordered) return;
  const std::int64_t step = track.packets[1].dts - track.packets[0].dts > 0 ? track.packets[1].dts - track.packets[0].dts : 1;
  const auto decode_time = [&](std::size_t k) {
    return k < n ? track.packets[k].dts : track.packets[n - 1].dts + static_cast<std::int64_t>(k - n + 1) * step;
  };
  for (std::size_t i = 0; i < n; ++i) track.packets[i].pts = decode_time(rank[i] + static_cast<std::size_t>(delay));
}

std::optional<Ac3Header> parse_ac3(ByteView data) noexcept {
  if (data.size() < 7 || std::to_integer<int>(data[0]) != 0x0B || std::to_integer<int>(data[1]) != 0x77) return std::nullopt;
  bits::BitReader r(data.subspan(4));
  Ac3Header h;
  h.fscod = static_cast<int>(r.bits(2));
  const int frmsizecod = static_cast<int>(r.bits(6));
  h.bsid = static_cast<int>(r.bits(5));
  h.bsmod = static_cast<int>(r.bits(3));
  h.acmod = static_cast<int>(r.bits(3));
  if (h.fscod == 3 || frmsizecod > 37 || h.bsid > 10) return std::nullopt;
  if ((h.acmod & 1) && h.acmod != 1) r.bits(2);
  if (h.acmod & 4) r.bits(2);
  if (h.acmod == 2) r.bits(2);
  h.lfe = static_cast<int>(r.bits(1));
  static constexpr int rates[] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
  const int kbps = rates[frmsizecod / 2];
  h.bitrate_code = frmsizecod;
  static constexpr std::uint32_t sample_rates[] = {48000, 44100, 32000};
  h.sample_rate = sample_rates[h.fscod];
  if (h.fscod == 0) h.frame_size = static_cast<std::size_t>(kbps) * 4;
  if (h.fscod == 2) h.frame_size = static_cast<std::size_t>(kbps) * 6;
  if (h.fscod == 1) h.frame_size = 2 * (static_cast<std::size_t>(kbps) * 1000 * 1536 / 44100 / 16 + static_cast<std::size_t>(frmsizecod & 1));
  static constexpr int channels[] = {2, 1, 2, 3, 3, 4, 4, 5};
  h.channels = channels[h.acmod] + h.lfe;
  return h;
}

void split_audio_frames(Demuxed& demuxed, TrackInfo& track, const EsStream& es, const std::vector<EsMarker>& markers) {
  const std::string& c = track.codec;
  const bool framed = c == "aac" || c == "mp2" || c == "mp3" || c == "ac3";
  track.timescale = 90000;
  if (!framed) {
    for (std::size_t m = 0; m < markers.size(); ++m) {
      const std::uint64_t end = m + 1 < markers.size() ? markers[m + 1].offset : es.size;
      PacketBuilder builder(demuxed);
      for (const Span& s : es.slice(markers[m].offset, end)) builder.input(s.offset, s.size);
      if (builder.empty()) continue;
      const std::int64_t pts = markers[m].pts.value_or(0);
      track.packets.push_back(builder.finish(pts, markers[m].dts.value_or(pts), 0, true));
    }
    return;
  }
  std::uint64_t pos = 0;
  std::size_t marker = 0;
  std::optional<std::int64_t> next_pts;
  std::uint64_t resyncs = 0;
  while (pos + 7 <= es.size && resyncs < 1'000'000) {
    const Bytes head = es.read(demuxed.input, pos, 10);
    std::size_t length = 0;
    std::int64_t samples = 0;
    std::uint32_t rate = 0;
    if (c == "aac") {
      if (const auto h = bits::parse_adts(head)) {
        length = h->frame_size;
        samples = 1024;
        rate = h->config.sample_rate;
        if (track.sample_rate == 0) {
          track.sample_rate = rate;
          track.channels = static_cast<std::uint16_t>(h->config.channel_config);
        }
      }
    } else if (c == "ac3") {
      if (const auto h = parse_ac3(head)) {
        length = h->frame_size;
        samples = 1536;
        rate = h->sample_rate;
        if (track.sample_rate == 0) {
          track.sample_rate = rate;
          track.channels = static_cast<std::uint16_t>(h->channels);
        }
      }
    } else if (const auto h = bits::parse_mpeg_audio(head)) {
      length = h->frame_size;
      samples = h->samples_per_frame;
      rate = h->sample_rate;
      if (track.sample_rate == 0) {
        track.sample_rate = rate;
        track.channels = static_cast<std::uint16_t>(h->channels);
        track.codec = h->layer == 3 ? "mp3" : "mp2";
      }
    }
    if (length == 0 || rate == 0) {
      ++pos;
      ++resyncs;
      continue;
    }
    if (pos + length > es.size) break;
    // A PES timestamp applies to the first frame that starts in that PES.
    std::optional<std::int64_t> pts;
    while (marker < markers.size() && markers[marker].offset <= pos) {
      if (markers[marker].pts) pts = markers[marker].pts;
      ++marker;
    }
    if (!pts) pts = next_pts;
    const std::int64_t duration = samples * 90000 / rate;
    PacketBuilder builder(demuxed);
    for (const Span& s : es.slice(pos, pos + length)) builder.input(s.offset, s.size);
    const std::int64_t value = pts.value_or(0);
    track.packets.push_back(builder.finish(value, value, duration, true));
    next_pts = value + duration;
    pos += length;
  }
}

}  // namespace cvt::video
