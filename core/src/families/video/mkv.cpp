// Convertible core - video family: Matroska and WebM (EBML) reader and writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <format>
#include <map>
#include <optional>

#include "codec/ids.hpp"
#include "families/video/adapt.hpp"
#include "families/video/bitstream.hpp"
#include "families/video/containers.hpp"

namespace cvt::video {

namespace {

namespace id = codec::id;

// --------------------------------------------------------------------------- element ids
constexpr std::uint32_t kEbml = 0x1A45DFA3, kDocType = 0x4282, kSegment = 0x18538067, kSeekHead = 0x114D9B74,
                        kSeek = 0x4DBB, kSeekId = 0x53AB, kSeekPosition = 0x53AC, kInfo = 0x1549A966,
                        kTimestampScale = 0x2AD7B1, kDuration = 0x4489, kTitle = 0x7BA9, kMuxingApp = 0x4D80,
                        kWritingApp = 0x5741, kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7,
                        kTrackUid = 0x73C5, kTrackType = 0x83, kFlagDefault = 0x88, kFlagLacing = 0x9C,
                        kDefaultDuration = 0x23E383, kCodecId = 0x86, kCodecPrivate = 0x63A2, kCodecDelay = 0x56AA,
                        kSeekPreRoll = 0x56BB, kLanguage = 0x22B59C, kName = 0x536E, kVideo = 0xE0,
                        kPixelWidth = 0xB0, kPixelHeight = 0xBA, kColourSpace = 0x2EB524, kProjection = 0x7670,
                        kProjectionType = 0x7671, kPoseRoll = 0x7675, kAudio = 0xE1, kSamplingFrequency = 0xB5,
                        kOutputSamplingFrequency = 0x78B5, kChannels = 0x9F, kBitDepth = 0x6264,
                        kContentEncodings = 0x6D80, kContentEncoding = 0x6240, kContentCompression = 0x5034,
                        kContentCompAlgo = 0x4254, kContentCompSettings = 0x4255, kContentEncryption = 0x5035,
                        kCluster = 0x1F43B675, kTimestamp = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1,
                        kBlockDuration = 0x9B, kReferenceBlock = 0xFB, kCues = 0x1C53BB6B, kCuePoint = 0xBB,
                        kCueTime = 0xB3, kCueTrackPositions = 0xB7, kCueTrack = 0xF7, kCueClusterPosition = 0xF1,
                        kTags = 0x1254C367, kTag = 0x7373, kSimpleTag = 0x67C8, kTagName = 0x45A3, kTagString = 0x4487,
                        kChapters = 0x1043A770, kAttachments = 0x1941A469;

bool is_top_level(std::uint32_t e) {
  return e == kSeekHead || e == kInfo || e == kTracks || e == kCluster || e == kCues || e == kTags || e == kChapters ||
         e == kAttachments || e == kSegment || e == kEbml;
}

struct Element {
  std::uint32_t id = 0;
  std::uint64_t start = 0;  // header position
  std::uint64_t data = 0;   // payload position
  std::uint64_t size = 0;   // payload size (clipped to the input)
  bool unknown = false;
};

std::optional<std::uint32_t> read_id(util::ByteReader& r) {
  const std::uint8_t first = r.u8();
  if (!r.ok() || first == 0) return std::nullopt;
  int length = 1;
  while (length <= 4 && (first & (0x80 >> (length - 1))) == 0) ++length;
  if (length > 4) return std::nullopt;
  std::uint32_t value = first;
  for (int i = 1; i < length; ++i) value = (value << 8) | r.u8();
  if (!r.ok()) return std::nullopt;
  return value;
}

std::optional<std::uint64_t> read_size(util::ByteReader& r, bool& unknown) {
  const std::uint8_t first = r.u8();
  if (!r.ok() || first == 0) return std::nullopt;
  int length = 1;
  while ((first & (0x80 >> (length - 1))) == 0) ++length;
  std::uint64_t value = first & (0xFFu >> length);
  bool all_ones = value == (0xFFu >> length);
  for (int i = 1; i < length; ++i) {
    const std::uint8_t b = r.u8();
    value = (value << 8) | b;
    all_ones = all_ones && b == 0xFF;
  }
  if (!r.ok()) return std::nullopt;
  unknown = all_ones;
  return value;
}

// Children of [begin, end) of the input.
std::vector<Element> children(ByteView input, std::uint64_t begin, std::uint64_t end) {
  std::vector<Element> out;
  util::ByteReader r(input.first(static_cast<std::size_t>(std::min<std::uint64_t>(end, input.size()))));
  if (!r.seek(static_cast<std::size_t>(begin))) return out;
  while (!r.at_end() && out.size() < 1'000'000) {
    Element e;
    e.start = r.position();
    const auto eid = read_id(r);
    bool unknown = false;
    const auto size = read_size(r, unknown);
    if (!eid || !size) break;
    e.id = *eid;
    e.data = r.position();
    e.unknown = unknown;
    const std::uint64_t available = r.remaining();
    e.size = unknown ? available : std::min(*size, available);
    out.push_back(e);
    if (unknown || *size > available) break;
    r.skip(static_cast<std::size_t>(*size));
  }
  return out;
}

std::uint64_t as_uint(ByteView input, const Element& e) {
  std::uint64_t v = 0;
  for (std::uint64_t i = 0; i < std::min<std::uint64_t>(e.size, 8); ++i) v = (v << 8) | std::to_integer<std::uint8_t>(input[e.data + i]);
  return v;
}
double as_float(ByteView input, const Element& e) {
  util::ByteReader r(input.subspan(static_cast<std::size_t>(e.data), static_cast<std::size_t>(e.size)));
  if (e.size == 4) return static_cast<double>(r.f32be());
  if (e.size == 8) return r.f64be();
  return 0.0;
}
std::string as_string(ByteView input, const Element& e) {
  std::string s(as_chars(input.subspan(static_cast<std::size_t>(e.data), static_cast<std::size_t>(e.size))));
  if (const std::size_t nul = s.find('\0'); nul != std::string::npos) s.resize(nul);
  return s;
}
ByteView as_bytes_of(ByteView input, const Element& e) {
  return input.subspan(static_cast<std::size_t>(e.data), static_cast<std::size_t>(e.size));
}

struct MkvTrack {
  TrackInfo info;
  std::uint64_t number = 0;
  std::uint64_t default_duration_ns = 0;
  std::uint64_t codec_delay_ns = 0;
  Bytes strip_prefix;
  bool unsupported_encoding = false;
  bits::VorbisDurations vorbis;
  bool vorbis_ready = false;
};

void map_codec(const std::string& codec_id, MkvTrack& t, ByteView priv) {
  TrackInfo& info = t.info;
  info.tag = codec_id;
  info.extradata.assign(priv.begin(), priv.end());
  const auto set = [&](std::string_view c) { info.codec = std::string(c); };
  if (codec_id == "V_MPEG4/ISO/AVC") set(id::h264);
  else if (codec_id == "V_MPEGH/ISO/HEVC") set(id::hevc);
  else if (codec_id == "V_AV1") set(id::av1);
  else if (codec_id == "V_VP8") set(id::vp8);
  else if (codec_id == "V_VP9") set(id::vp9);
  else if (codec_id.starts_with("V_MPEG4/ISO/")) set(id::mpeg4);
  else if (codec_id == "V_MPEG1") set(id::mpeg1video);
  else if (codec_id == "V_MPEG2") set(id::mpeg2video);
  else if (codec_id == "V_THEORA") set(id::theora);
  else if (codec_id == "V_MJPEG") set(id::mjpeg);
  else if (codec_id == "V_PRORES") set(id::prores);
  else if (codec_id == "V_UNCOMPRESSED") set(id::rawvideo);
  else if (codec_id == "V_FFV1") set(id::ffv1);
  else if (codec_id == "V_MS/VFW/FOURCC") {
    if (priv.size() >= 40) {
      util::ByteReader r(priv);
      r.skip(14);
      info.bits_per_sample = r.u16le();  // biBitCount: the coded depth
      const std::uint32_t compression = std::byteswap(r.u32le());
      set(codec_for_bitmap_fourcc(compression));
      info.tag = bits::fourcc_text(compression);
      info.extradata.assign(priv.begin() + 40, priv.end());
    }
  } else if (codec_id.starts_with("A_AAC")) {
    set(id::aac);
  } else if (codec_id == "A_MPEG/L3") set(id::mp3);
  else if (codec_id == "A_MPEG/L2") set(id::mp2);
  else if (codec_id == "A_OPUS") set(id::opus);
  else if (codec_id == "A_VORBIS") set(id::vorbis);
  else if (codec_id == "A_FLAC") {
    set(id::flac);
    // Canonical FLAC configuration: the bare 34-byte STREAMINFO body.
    if (priv.size() >= 4 + 4 + 34 && as_chars(priv.first(4)) == "fLaC") info.extradata.assign(priv.begin() + 8, priv.begin() + 42);
  } else if (codec_id == "A_ALAC") set(id::alac);
  else if (codec_id == "A_AC3") set(id::ac3);
  else if (codec_id == "A_EAC3") set(cid::eac3);
  else if (codec_id == "A_PCM/INT/LIT") {
    set(info.bits_per_sample == 24 ? id::pcm_s24le : info.bits_per_sample == 8 ? id::pcm_u8 : id::pcm_s16le);
  } else if (codec_id == "A_PCM/INT/BIG") {
    if (info.bits_per_sample == 16) set(id::pcm_s16be);
  } else if (codec_id == "A_PCM/FLOAT/IEEE") {
    if (info.bits_per_sample == 32) set(id::pcm_f32le);
  } else if (codec_id == "A_MS/ACM") {
    if (priv.size() >= 18) {
      util::ByteReader r(priv);
      const std::uint16_t tag = r.u16le();
      r.skip(12);
      const std::uint16_t bits = r.u16le();
      set(codec_for_wave_tag(tag, bits));
      info.extradata.assign(priv.begin() + 18, priv.end());
    }
  } else if (codec_id == "S_TEXT/UTF8") set(cid::subrip);
  else if (codec_id == "S_TEXT/WEBVTT") set(cid::webvtt);
  else if (codec_id == "S_TEXT/ASS" || codec_id == "S_TEXT/SSA") set(cid::ass);
}

struct MkvReader {
  ByteView input;
  const ReadSettings& settings;
  std::shared_ptr<Demuxed> out;
  std::vector<MkvTrack> tracks;
  std::uint64_t scale_ns = 1'000'000;
  std::uint32_t timescale = 1000;
  double duration = 0;

  MkvTrack* track_by_number(std::uint64_t n) {
    for (MkvTrack& t : tracks) {
      if (t.number == n) return &t;
    }
    return nullptr;
  }

  void parse_track(const Element& entry) {
    MkvTrack t;
    std::string codec_id;
    ByteView priv;
    std::uint64_t type = 0;
    double rate = 8000.0;  // Matroska default
    double output_rate = 0;
    for (const Element& e : children(input, entry.data, entry.data + entry.size)) {
      switch (e.id) {
        case kTrackNumber: t.number = as_uint(input, e); break;
        case kTrackType: type = as_uint(input, e); break;
        case kFlagDefault: t.info.is_default = as_uint(input, e) != 0; break;
        case kDefaultDuration: t.default_duration_ns = as_uint(input, e); break;
        case kCodecId: codec_id = as_string(input, e); break;
        case kCodecPrivate: priv = as_bytes_of(input, e); break;
        case kCodecDelay: t.codec_delay_ns = as_uint(input, e); break;
        case kLanguage: t.info.language = as_string(input, e); break;
        case kName: t.info.title = as_string(input, e); break;
        case kVideo:
          for (const Element& v : children(input, e.data, e.data + e.size)) {
            if (v.id == kPixelWidth) t.info.width = static_cast<std::uint32_t>(std::min<std::uint64_t>(as_uint(input, v), 1u << 20));
            if (v.id == kPixelHeight) t.info.height = static_cast<std::uint32_t>(std::min<std::uint64_t>(as_uint(input, v), 1u << 20));
            if (v.id == kColourSpace && v.size == 4) {
              const ByteView fcc = as_bytes_of(input, v);
              t.strip_prefix.clear();
              t.info.extradata.assign(fcc.begin(), fcc.end());
            }
            if (v.id == kProjection) {
              for (const Element& p : children(input, v.data, v.data + v.size)) {
                if (p.id == kPoseRoll) {
                  const double roll = as_float(input, p);
                  int degrees = static_cast<int>(std::lround(-roll / 90.0)) * 90;
                  t.info.rotation = ((degrees % 360) + 360) % 360;
                }
              }
            }
          }
          break;
        case kAudio:
          for (const Element& a : children(input, e.data, e.data + e.size)) {
            if (a.id == kSamplingFrequency) rate = as_float(input, a);
            if (a.id == kOutputSamplingFrequency) output_rate = as_float(input, a);
            if (a.id == kChannels) t.info.channels = static_cast<std::uint16_t>(std::min<std::uint64_t>(as_uint(input, a), 64));
            if (a.id == kBitDepth) t.info.bits_per_sample = static_cast<std::uint16_t>(std::min<std::uint64_t>(as_uint(input, a), 64));
          }
          break;
        case kContentEncodings:
          for (const Element& ce : children(input, e.data, e.data + e.size)) {
            if (ce.id != kContentEncoding) continue;
            for (const Element& c : children(input, ce.data, ce.data + ce.size)) {
              if (c.id == kContentEncryption) t.unsupported_encoding = true;
              if (c.id != kContentCompression) continue;
              std::uint64_t algo = 0;
              ByteView settings_bytes;
              for (const Element& cc : children(input, c.data, c.data + c.size)) {
                if (cc.id == kContentCompAlgo) algo = as_uint(input, cc);
                if (cc.id == kContentCompSettings) settings_bytes = as_bytes_of(input, cc);
              }
              if (algo == 3) {
                t.strip_prefix.assign(settings_bytes.begin(), settings_bytes.end());
              } else {
                t.unsupported_encoding = true;
              }
            }
          }
          break;
        default: break;
      }
    }
    if (type == 1) {
      t.info.type = ir::TrackType::Video;
    } else if (type == 2) {
      if (t.info.channels == 0) t.info.channels = 1;  // Matroska default
      t.info.type = ir::TrackType::Audio;
    } else if (type == 17) {
      t.info.type = ir::TrackType::Subtitle;
    } else {
      return;
    }
    const double r = output_rate > 0 ? output_rate : rate;
    if (r > 0 && r < 1e7) t.info.sample_rate = static_cast<std::uint32_t>(std::lround(r));
    const Bytes colour_space = codec_id == "V_UNCOMPRESSED" ? t.info.extradata : Bytes{};
    map_codec(codec_id, t, priv);
    if (codec_id == "V_UNCOMPRESSED") t.info.extradata = colour_space;
    if (t.unsupported_encoding) {
      settings.warnings.add(std::format("Matroska track {} uses compressed or encrypted content and was skipped", t.number));
      return;
    }
    if (t.info.codec == id::aac && t.info.extradata.empty() && t.info.sample_rate != 0) {
      bits::AacConfig c;
      c.object_type = codec_id.find("/SBR") != std::string::npos ? 5 : 2;
      c.sample_rate = t.info.sample_rate;
      c.channel_config = t.info.channels;
      t.info.extradata = bits::build_asc(c);
    }
    if (t.info.codec == id::vorbis) {
      if (auto headers = bits::split_xiph_lacing(t.info.extradata); headers && headers->size() == 3) {
        t.vorbis_ready = t.vorbis.init((*headers)[0], (*headers)[2]);
      }
    }
    if (t.info.codec == id::opus) t.info.priming = bits::opus_pre_skip(t.info.extradata);
    t.info.timescale = timescale;
    t.info.id = static_cast<std::uint32_t>(t.number);
    if (t.info.type == ir::TrackType::Video && t.default_duration_ns > 0) {
      t.info.fps = ir::Rational{1'000'000'000, static_cast<std::int64_t>(t.default_duration_ns)};
    }
    if (track_by_number(t.number) == nullptr) tracks.push_back(std::move(t));
  }

  std::int64_t frame_duration_ns(MkvTrack& t, ByteView frame_head) {
    if (t.default_duration_ns > 0) return static_cast<std::int64_t>(t.default_duration_ns);
    if (t.info.codec == id::opus) return static_cast<std::int64_t>(bits::opus_packet_samples(frame_head)) * 1'000'000'000 / 48000;
    if (t.info.codec == id::vorbis && t.vorbis_ready && t.info.sample_rate != 0) {
      const int samples = t.vorbis.next(frame_head);
      return samples > 0 ? static_cast<std::int64_t>(samples) * 1'000'000'000 / t.info.sample_rate : 0;
    }
    return 0;
  }

  Result<void> parse_block(const Element& e, std::int64_t cluster_ts, bool simple, bool has_reference, std::int64_t block_duration) {
    util::ByteReader r(input.first(static_cast<std::size_t>(e.data + e.size)));
    r.seek(static_cast<std::size_t>(e.data));
    bool unknown = false;
    const auto number = read_size(r, unknown);
    const std::int16_t rel = r.i16be();
    const std::uint8_t flags = r.u8();
    if (!number || !r.ok()) return {};
    MkvTrack* t = track_by_number(*number);
    if (t == nullptr) return {};
    const bool key = simple ? (flags & 0x80) != 0 : !has_reference;
    const int lacing = (flags >> 1) & 3;
    std::vector<std::uint64_t> sizes;
    if (lacing == 0) {
      sizes.push_back(r.remaining());
    } else {
      const int count = r.u8() + 1;
      if (!r.ok()) return {};
      std::uint64_t used = 0;
      if (lacing == 1) {
        for (int i = 0; i + 1 < count; ++i) {
          std::uint64_t size = 0;
          while (true) {
            const std::uint8_t b = r.u8();
            if (!r.ok()) return {};
            size += b;
            if (b != 255) break;
          }
          sizes.push_back(size);
          used += size;
        }
      } else if (lacing == 3) {
        bool unk = false;
        auto first = read_size(r, unk);
        if (!first) return {};
        sizes.push_back(*first);
        used = *first;
        std::int64_t previous = static_cast<std::int64_t>(*first);
        for (int i = 1; i + 1 < count; ++i) {
          const std::size_t before = r.position();
          auto raw = read_size(r, unk);
          if (!raw) return {};
          const auto length = static_cast<int>(r.position() - before);
          const std::int64_t bias = (std::int64_t{1} << (7 * length - 1)) - 1;
          const std::int64_t size = previous + static_cast<std::int64_t>(*raw) - bias;
          if (size < 0) return {};
          sizes.push_back(static_cast<std::uint64_t>(size));
          used += static_cast<std::uint64_t>(size);
          previous = size;
        }
      } else {
        const std::uint64_t each = r.remaining() / static_cast<std::uint64_t>(count);
        for (int i = 0; i + 1 < count; ++i) sizes.push_back(each);
        used = each * static_cast<std::uint64_t>(count - 1);
      }
      if (used > r.remaining()) return {};
      sizes.push_back(r.remaining() - used);
    }
    std::uint64_t at = r.position();
    std::int64_t ns = sat_mul(sat_add(cluster_ts, rel), static_cast<std::int64_t>(scale_ns));
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      const std::uint64_t size = sizes[i];
      if (at + size > e.data + e.size) return {};
      PacketBuilder builder(*out);
      builder.owned(t->strip_prefix);
      builder.input(at, size);
      const ByteView head = input.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(std::min<std::uint64_t>(size, 64)));
      std::int64_t dur_ns = frame_duration_ns(*t, head);
      if (sizes.size() == 1 && block_duration > 0) dur_ns = block_duration * static_cast<std::int64_t>(scale_ns);
      // As other players do: the delay is rounded to the track time base first.
      const std::int64_t pts = rescale(ns, 1'000'000'000, timescale) -
                               rescale(static_cast<std::int64_t>(t->codec_delay_ns), 1'000'000'000, timescale);
      const std::int64_t duration = rescale(dur_ns, 1'000'000'000, timescale);
      t->info.packets.push_back(builder.finish(pts, pts, duration, key || t->info.type != ir::TrackType::Video));
      at += size;
      ns = sat_add(ns, dur_ns);
    }
    return {};
  }

  Result<void> parse_cluster(const Element& cluster) {
    std::int64_t ts = 0;
    for (const Element& e : children(input, cluster.data, cluster.data + cluster.size)) {
      if (is_top_level(e.id)) break;
      if (e.id == kTimestamp) ts = static_cast<std::int64_t>(as_uint(input, e));
      if (e.id == kSimpleBlock) CVT_TRY(parse_block(e, ts, true, false, 0));
      if (e.id == kBlockGroup) {
        bool reference = false;
        std::int64_t duration = 0;
        const Element* block = nullptr;
        const auto parts = children(input, e.data, e.data + e.size);
        for (const Element& g : parts) {
          if (g.id == kReferenceBlock) reference = true;
          if (g.id == kBlockDuration) duration = static_cast<std::int64_t>(as_uint(input, g));
          if (g.id == kBlock) block = &g;
        }
        if (block != nullptr) CVT_TRY(parse_block(*block, ts, false, reference, duration));
      }
    }
    return {};
  }

  // An unknown-sized cluster ends where the next top-level element starts.
  std::uint64_t cluster_end(const Element& cluster) {
    if (!cluster.unknown) return cluster.data + cluster.size;
    std::uint64_t end = cluster.data;
    for (const Element& e : children(input, cluster.data, input.size())) {
      if (is_top_level(e.id)) return e.start;
      end = e.data + e.size;
    }
    return end;
  }

  Result<void> parse_segment(std::uint64_t begin, std::uint64_t end) {
    std::uint64_t pos = begin;
    std::uint64_t visited = 0;
    while (pos < end) {
      const auto list = children(input, pos, end);
      if (list.empty()) break;
      std::uint64_t next = end;
      for (const Element& e : list) {
        if (++visited > 10'000'000) return fail(Status::Limit, "Matroska file has too many elements");
        if ((visited & 255) == 0 && !settings.progress.report(static_cast<double>(e.start) / static_cast<double>(input.size()))) {
          return fail(Status::Cancelled, "the conversion was cancelled");
        }
        switch (e.id) {
          case kInfo:
            for (const Element& i : children(input, e.data, e.data + e.size)) {
              if (i.id == kTimestampScale) {
                const std::uint64_t s = as_uint(input, i);
                if (s > 0 && s <= 1'000'000'000) scale_ns = s;
              }
              if (i.id == kDuration) duration = as_float(input, i);
              if (i.id == kTitle) out->metadata["title"] = as_string(input, i);
            }
            timescale = 1'000'000'000 % scale_ns == 0 ? static_cast<std::uint32_t>(1'000'000'000 / scale_ns) : 1'000'000'000;
            if (1'000'000'000 % scale_ns != 0) scale_ns = std::max<std::uint64_t>(scale_ns, 1);
            break;
          case kTracks:
            for (const Element& t : children(input, e.data, e.data + e.size)) {
              if (t.id == kTrackEntry) parse_track(t);
            }
            break;
          case kCluster: {
            const std::uint64_t ce = cluster_end(e);
            Element c = e;
            c.size = ce - e.data;
            CVT_TRY(parse_cluster(c));
            if (e.unknown) {
              next = ce;
              goto restart;
            }
            break;
          }
          case kTags:
            for (const Element& tag : children(input, e.data, e.data + e.size)) {
              if (tag.id != kTag) continue;
              for (const Element& st : children(input, tag.data, tag.data + tag.size)) {
                if (st.id != kSimpleTag) continue;
                std::string name;
                std::string value;
                for (const Element& f : children(input, st.data, st.data + st.size)) {
                  if (f.id == kTagName) name = as_string(input, f);
                  if (f.id == kTagString) value = as_string(input, f);
                }
                if (!name.empty() && !value.empty()) {
                  std::string key;
                  for (const char ch : name) key.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
                  out->metadata.emplace(key, value);
                }
              }
            }
            break;
          default: break;
        }
        if (e.unknown) {
          next = e.data + e.size;
          goto restart;
        }
      }
      break;
    restart:
      if (next <= pos) break;
      pos = next;
    }
    return {};
  }
};

}  // namespace

Result<std::shared_ptr<Demuxed>> demux_mkv(ByteView input, const ReadSettings& settings) {
  MkvReader reader{input, settings, std::make_shared<Demuxed>(), {}, 1'000'000, 1000, 0};
  reader.out->input = input;
  reader.out->format_id = "mkv";
  const auto top = children(input, 0, input.size());
  if (top.empty() || top.front().id != kEbml) return fail(Status::Decode, "not a Matroska/WebM file (no EBML header)");
  const Element* segment = nullptr;
  for (const Element& e : top) {
    if (e.id == kSegment) {
      segment = &e;
      break;
    }
  }
  if (segment == nullptr) return fail(Status::Decode, "the Matroska file has no segment; it is truncated");
  CVT_TRY(reader.parse_segment(segment->data, segment->data + segment->size));
  Demuxed& out = *reader.out;
  for (MkvTrack& t : reader.tracks) {
    // Matroska stores presentation times only; derive monotone decode times.
    std::vector<PacketRef>& packets = t.info.packets;
    if (t.info.type == ir::TrackType::Video && !packets.empty()) {
      std::vector<std::int64_t> sorted;
      sorted.reserve(packets.size());
      for (const PacketRef& p : packets) sorted.push_back(p.pts);
      std::ranges::sort(sorted);
      std::int64_t delay = 0;
      for (std::size_t i = 0; i < packets.size(); ++i) delay = std::max(delay, sorted[i] - packets[i].pts);
      for (std::size_t i = 0; i < packets.size(); ++i) packets[i].dts = sorted[i] - delay;
    }
    out.tracks.push_back(std::move(t.info));
  }
  if (reader.duration > 0 && reader.duration < 1e15) {
    out.duration_ms = static_cast<std::int64_t>(reader.duration * static_cast<double>(reader.scale_ns) / 1e6);
  }
  CVT_TRY(finish_demux(out, "Matroska"));
  return reader.out;
}

// =========================================================================== writer

namespace {

class Ebml {
 public:
  static void id(util::ByteWriter& w, std::uint32_t value) {
    if (value >= 0x1000000) w.u8(static_cast<std::uint8_t>(value >> 24));
    if (value >= 0x10000) w.u8(static_cast<std::uint8_t>(value >> 16));
    if (value >= 0x100) w.u8(static_cast<std::uint8_t>(value >> 8));
    w.u8(static_cast<std::uint8_t>(value));
  }
  static void size(util::ByteWriter& w, std::uint64_t value) {
    int length = 1;
    while (length < 8 && value >= (std::uint64_t{1} << (7 * length)) - 1) ++length;
    const std::uint64_t marked = value | (std::uint64_t{1} << (7 * length));
    for (int i = length - 1; i >= 0; --i) w.u8(static_cast<std::uint8_t>(marked >> (8 * i)));
  }
  static void size8(util::ByteWriter& w, std::uint64_t value) {
    w.u8(0x01);
    for (int i = 6; i >= 0; --i) w.u8(static_cast<std::uint8_t>(value >> (8 * i)));
  }
  static void binary(util::ByteWriter& w, std::uint32_t eid, ByteView data) {
    id(w, eid);
    size(w, data.size());
    w.bytes(data);
  }
  static void uint(util::ByteWriter& w, std::uint32_t eid, std::uint64_t value) {
    int n = 1;
    while (n < 8 && (value >> (8 * n)) != 0) ++n;
    id(w, eid);
    size(w, static_cast<std::uint64_t>(n));
    for (int i = n - 1; i >= 0; --i) w.u8(static_cast<std::uint8_t>(value >> (8 * i)));
  }
  static void uint8(util::ByteWriter& w, std::uint32_t eid, std::uint64_t value) {
    id(w, eid);
    size(w, 8);
    w.u64be(value);
  }
  static void real(util::ByteWriter& w, std::uint32_t eid, double value) {
    id(w, eid);
    size(w, 8);
    w.f64be(value);
  }
  static void string(util::ByteWriter& w, std::uint32_t eid, std::string_view text) { binary(w, eid, as_bytes(text)); }
  static void master(util::ByteWriter& w, std::uint32_t eid, const util::ByteWriter& body) { binary(w, eid, body.view()); }
};

struct MkvOutTrack {
  MuxTrack desc;
  StreamAdapter adapter;
  std::int64_t shift = 0;  // added to timestamps (track timescale) to avoid negative times
  bool started = false;
  std::int64_t end_ms = 0;
  bool have_override = false;
  Bytes first_packet;
};

std::string_view mkv_codec_id(std::string_view codec, std::uint16_t bits) {
  if (codec == id::h264) return "V_MPEG4/ISO/AVC";
  if (codec == id::hevc) return "V_MPEGH/ISO/HEVC";
  if (codec == id::av1) return "V_AV1";
  if (codec == id::vp8) return "V_VP8";
  if (codec == id::vp9) return "V_VP9";
  if (codec == id::mpeg4) return "V_MPEG4/ISO/ASP";
  if (codec == id::mpeg1video) return "V_MPEG1";
  if (codec == id::mpeg2video) return "V_MPEG2";
  if (codec == id::theora) return "V_THEORA";
  if (codec == id::mjpeg) return "V_MJPEG";
  if (codec == id::prores) return "V_PRORES";
  if (codec == id::rawvideo) return "V_UNCOMPRESSED";
  if (codec == id::ffv1) return "V_FFV1";
  if (codec == id::huffyuv || codec == id::utvideo) return "V_MS/VFW/FOURCC";
  if (codec == id::aac) return "A_AAC";
  if (codec == id::mp3) return "A_MPEG/L3";
  if (codec == id::mp2) return "A_MPEG/L2";
  if (codec == id::opus) return "A_OPUS";
  if (codec == id::vorbis) return "A_VORBIS";
  if (codec == id::flac) return "A_FLAC";
  if (codec == id::alac) return "A_ALAC";
  if (codec == id::ac3) return "A_AC3";
  if (codec == cid::eac3) return "A_EAC3";
  if (codec == id::pcm_s16le || codec == id::pcm_s24le || codec == id::pcm_u8) return "A_PCM/INT/LIT";
  if (codec == id::pcm_s16be) return "A_PCM/INT/BIG";
  if (codec == id::pcm_f32le) return "A_PCM/FLOAT/IEEE";
  (void)bits;
  return {};
}

class MkvMuxer final : public Muxer {
 public:
  explicit MkvMuxer(bool webm) : webm_(webm) {}

  Result<void> begin(std::vector<MuxTrack> tracks, const MuxSettings& settings) override {
    settings_ = settings;
    const std::string format = webm_ ? std::string("webm") : settings.format_id;
    for (MuxTrack& t : tracks) {
      if (!container_accepts(format, t.type, t.codec) || mkv_codec_id(t.codec, 0).empty()) {
        return fail(Status::Encode, "{} cannot store {}", webm_ ? "WebM" : "Matroska", t.codec);
      }
      MkvOutTrack o;
      o.adapter = StreamAdapter(t.codec, t.extradata, Packaging{});
      o.desc = std::move(t);
      if (o.desc.timescale == 0) o.desc.timescale = 1000;
      tracks_.push_back(std::move(o));
    }
    return {};
  }

  Result<void> write(std::size_t index, const ir::Packet& packet) override {
    MkvOutTrack& t = tracks_[index];
    CVT_TRY_ASSIGN(Bytes data, t.adapter.convert(packet.bytes, packet.keyframe));
    if (t.first_packet.empty()) t.first_packet = data;
    if (!t.started) {
      t.started = true;
      if (packet.pts < 0) t.shift = -packet.pts;
    }
    const std::int64_t pts_ms = rescale(packet.pts + t.shift, t.desc.timescale, 1000);
    const bool video = t.desc.type == ir::TrackType::Video;
    const bool key = !video || packet.keyframe;
    const bool new_cluster = cluster_.size() == 0 || pts_ms - cluster_ts_ > 30000 || pts_ms - cluster_ts_ < -30000 ||
                             (video && key && pts_ms - cluster_ts_ >= 1000) || cluster_.size() > (5u << 20) ||
                             (!has_video() && pts_ms - cluster_ts_ >= 5000);
    if (new_cluster) {
      flush_cluster();
      cluster_ts_ = std::max<std::int64_t>(pts_ms, 0);
      cluster_offset_ = clusters_.size();
    }
    if (video && key) cues_.push_back({cluster_ts_ == pts_ms ? pts_ms : pts_ms, index + 1, cluster_offset_});
    util::ByteWriter block;
    block.u8(static_cast<std::uint8_t>(0x80 | (index + 1)));
    block.i16be(static_cast<std::int16_t>(std::clamp<std::int64_t>(pts_ms - cluster_ts_, -32768, 32767)));
    block.u8(key ? 0x80 : 0x00);
    block.bytes(data);
    Ebml::binary(cluster_, kSimpleBlock, block.view());
    const std::int64_t end = pts_ms + rescale(std::max<std::int64_t>(packet.duration, 0), t.desc.timescale, 1000);
    t.end_ms = std::max(t.end_ms, end);
    if (clusters_.size() + cluster_.size() > settings_.limits.max_output_bytes) {
      return fail(Status::Limit, "the Matroska output exceeds the output size limit");
    }
    return {};
  }

  void set_extradata(std::size_t track, Bytes extradata) override {
    tracks_[track].adapter = StreamAdapter(tracks_[track].desc.codec, std::move(extradata), Packaging{});
    tracks_[track].adapter.learn(tracks_[track].first_packet);
  }

  Result<Bytes> finish() override {
    flush_cluster();
    if (clusters_.size() == 0) return fail(Status::Encode, "nothing to write: the video has no packets");
    CVT_TRY_ASSIGN(const Bytes track_bytes, build_tracks());
    util::ByteWriter info;
    {
      util::ByteWriter body;
      Ebml::uint(body, kTimestampScale, 1'000'000);
      std::int64_t duration = 0;
      for (const MkvOutTrack& t : tracks_) duration = std::max(duration, t.end_ms);
      Ebml::real(body, kDuration, static_cast<double>(duration));
      Ebml::string(body, kMuxingApp, "");
      Ebml::string(body, kWritingApp, "");
      if (const auto title = settings_.metadata.find("title"); title != settings_.metadata.end()) {
        Ebml::string(body, kTitle, title->second);
      }
      Ebml::master(info, kInfo, body);
    }
    // Layout of the segment payload: SeekHead, Info, Tracks, Clusters, Cues.
    const auto seek_entry = [](util::ByteWriter& w, std::uint32_t target, std::uint64_t position) {
      util::ByteWriter seek;
      util::ByteWriter target_id;
      Ebml::id(target_id, target);
      Ebml::binary(seek, kSeekId, target_id.view());
      Ebml::uint8(seek, kSeekPosition, position);
      Ebml::master(w, kSeek, seek);
    };
    util::ByteWriter probe;
    {
      util::ByteWriter body;
      seek_entry(body, kInfo, 0);
      seek_entry(body, kTracks, 0);
      seek_entry(body, kCues, 0);
      Ebml::master(probe, kSeekHead, body);
    }
    const std::uint64_t seek_size = probe.size();
    const std::uint64_t info_at = seek_size;
    const std::uint64_t tracks_at = info_at + info.size();
    const std::uint64_t clusters_at = tracks_at + track_bytes.size();
    const std::uint64_t cues_at = clusters_at + clusters_.size();
    util::ByteWriter seekhead;
    {
      util::ByteWriter body;
      seek_entry(body, kInfo, info_at);
      seek_entry(body, kTracks, tracks_at);
      seek_entry(body, kCues, cues_at);
      Ebml::master(seekhead, kSeekHead, body);
    }
    util::ByteWriter cues;
    {
      util::ByteWriter body;
      for (const Cue& c : cues_) {
        util::ByteWriter point;
        Ebml::uint(point, kCueTime, static_cast<std::uint64_t>(std::max<std::int64_t>(c.time_ms, 0)));
        util::ByteWriter pos;
        Ebml::uint(pos, kCueTrack, c.track);
        Ebml::uint(pos, kCueClusterPosition, clusters_at + c.cluster_offset);
        Ebml::master(point, kCueTrackPositions, pos);
        Ebml::master(body, kCuePoint, point);
      }
      if (!cues_.empty()) Ebml::master(cues, kCues, body);
    }
    util::ByteWriter out;
    {
      util::ByteWriter header;
      Ebml::uint(header, 0x4286, 1);
      Ebml::uint(header, 0x42F7, 1);
      Ebml::uint(header, 0x42F2, 4);
      Ebml::uint(header, 0x42F3, 8);
      Ebml::string(header, kDocType, webm_ ? "webm" : "matroska");
      Ebml::uint(header, 0x4287, 4);
      Ebml::uint(header, 0x4285, 2);
      Ebml::master(out, kEbml, header);
    }
    const std::uint64_t segment_size = seekhead.size() + info.size() + track_bytes.size() + clusters_.size() + cues.size();
    CVT_TRY(settings_.limits.check_output(out.size() + 12 + segment_size));
    Ebml::id(out, kSegment);
    Ebml::size8(out, segment_size);
    out.bytes(seekhead.view());
    out.bytes(info.view());
    out.bytes(track_bytes);
    out.bytes(clusters_.view());
    out.bytes(cues.view());
    return out.take();
  }

 private:
  struct Cue {
    std::int64_t time_ms = 0;
    std::size_t track = 0;
    std::uint64_t cluster_offset = 0;
  };

  bool has_video() const {
    return std::ranges::any_of(tracks_, [](const MkvOutTrack& t) { return t.desc.type == ir::TrackType::Video; });
  }

  void flush_cluster() {
    if (cluster_.size() == 0) return;
    util::ByteWriter body;
    Ebml::uint(body, kTimestamp, static_cast<std::uint64_t>(cluster_ts_));
    body.bytes(cluster_.view());
    Ebml::master(clusters_, kCluster, body);
    cluster_ = util::ByteWriter();
  }

  Result<Bytes> codec_private(MkvOutTrack& t) {
    const std::string& c = t.desc.codec;
    Bytes extra = t.adapter.extradata();
    if (c == id::h264 || c == id::hevc || c == id::av1) {
      if (extra.empty()) return fail(Status::Encode, "the {} stream carries no configuration", c);
      return extra;
    }
    if (c == id::opus) {
      if (extra.empty()) extra = bits::make_opus_head(t.desc.channels == 0 ? 2 : t.desc.channels, t.desc.sample_rate, 0);
      return extra;
    }
    if (c == id::flac) {
      if (extra.size() < 34) return fail(Status::Encode, "the FLAC stream has no STREAMINFO");
      util::ByteWriter w;
      w.text("fLaC").u8(0x80).u24be(34).bytes(ByteView(extra).last(34));
      return w.take();
    }
    if (c == id::vorbis || c == id::theora) {
      if (extra.empty()) return fail(Status::Encode, "the {} stream has no setup headers", c);
      return extra;
    }
    if (c == id::huffyuv || c == id::utvideo) {
      // V_MS/VFW/FOURCC: a BITMAPINFOHEADER followed by the codec's own data.
      util::ByteWriter w;
      w.u32le(40 + static_cast<std::uint32_t>(extra.size())).i32le(static_cast<std::int32_t>(t.desc.width));
      w.i32le(static_cast<std::int32_t>(t.desc.height)).u16le(1).u16le(t.desc.bits_per_sample != 0 ? t.desc.bits_per_sample : 16);
      w.u32le(std::byteswap(bitmap_fourcc_for_codec(c))).u32le(0).zeros(16);
      w.bytes(extra);
      return w.take();
    }
    if (c == id::rawvideo || c == id::vp8 || c == id::vp9 || c.starts_with("pcm_") || c == id::mp3 || c == id::mp2 ||
        c == id::ac3) {
      return Bytes{};
    }
    return extra;
  }

  Result<Bytes> build_tracks() {
    util::ByteWriter body;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
      MkvOutTrack& t = tracks_[i];
      util::ByteWriter e;
      Ebml::uint(e, kTrackNumber, i + 1);
      Ebml::uint(e, kTrackUid, i + 1);
      const bool video = t.desc.type == ir::TrackType::Video;
      Ebml::uint(e, kTrackType, video ? 1 : 2);
      Ebml::uint(e, kFlagLacing, 0);
      Ebml::string(e, kLanguage, t.desc.language.empty() ? "und" : t.desc.language);
      Ebml::string(e, kCodecId, mkv_codec_id(t.desc.codec, t.desc.bits_per_sample));
      CVT_TRY_ASSIGN(const Bytes priv, codec_private(t));
      if (!priv.empty()) Ebml::binary(e, kCodecPrivate, priv);
      if (t.shift > 0 && !video) {
        Ebml::uint(e, kCodecDelay, static_cast<std::uint64_t>(rescale(t.shift, t.desc.timescale, 1'000'000'000)));
      }
      if (t.desc.codec == id::opus) Ebml::uint(e, kSeekPreRoll, 80'000'000);
      if (video) {
        if (t.desc.fps.num > 0 && t.desc.fps.den > 0) {
          Ebml::uint(e, kDefaultDuration,
                     static_cast<std::uint64_t>(std::llround(1e9 * static_cast<double>(t.desc.fps.den) / static_cast<double>(t.desc.fps.num))));
        }
        util::ByteWriter v;
        Ebml::uint(v, kPixelWidth, t.desc.width);
        Ebml::uint(v, kPixelHeight, t.desc.height);
        if (t.desc.codec == id::rawvideo) {
          const Bytes tag = t.adapter.extradata();
          if (tag.size() == 4) Ebml::binary(v, kColourSpace, tag);
        }
        if (t.desc.rotation != 0) {
          util::ByteWriter p;
          Ebml::uint(p, kProjectionType, 0);
          Ebml::real(p, kPoseRoll, -static_cast<double>(t.desc.rotation > 180 ? t.desc.rotation - 360 : t.desc.rotation));
          Ebml::master(v, kProjection, p);
        }
        Ebml::master(e, kVideo, v);
      } else {
        util::ByteWriter a;
        const double rate = t.desc.codec == id::opus ? 48000.0 : static_cast<double>(t.desc.sample_rate);
        Ebml::real(a, kSamplingFrequency, rate);
        Ebml::uint(a, kChannels, t.desc.channels == 0 ? 2 : t.desc.channels);
        if (t.desc.codec.starts_with("pcm_")) {
          const std::uint16_t bits = t.desc.codec == id::pcm_s24le ? 24 : t.desc.codec == id::pcm_f32le ? 32
                                     : t.desc.codec == id::pcm_u8 ? 8 : 16;
          Ebml::uint(a, kBitDepth, bits);
        }
        Ebml::master(e, kAudio, a);
      }
      Ebml::master(body, kTrackEntry, e);
    }
    util::ByteWriter out;
    Ebml::master(out, kTracks, body);
    return out.take();
  }

  bool webm_;
  MuxSettings settings_;
  std::vector<MkvOutTrack> tracks_;
  util::ByteWriter clusters_;
  util::ByteWriter cluster_;
  std::int64_t cluster_ts_ = 0;
  std::uint64_t cluster_offset_ = 0;
  std::vector<Cue> cues_;
};

}  // namespace

std::unique_ptr<Muxer> make_mkv_muxer(bool webm) { return std::make_unique<MkvMuxer>(webm); }

}  // namespace cvt::video
