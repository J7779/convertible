// Convertible core - video family: ISO base media file writer (MP4, MOV, M4V,
// M4A, M4B, 3GP, 3G2). Fast start (movie header before the media data),
// configuration boxes built from the stream, edit lists for composition
// offsets and audio priming, 64-bit chunk offsets when needed.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>

#include "codec/ids.hpp"
#include "families/video/adapt.hpp"
#include "families/video/bitstream.hpp"
#include "families/video/containers.hpp"

namespace cvt::video {

namespace {

using bits::fourcc;

// Nested box writer: open() returns a position whose size is patched by close().
class BoxWriter {
 public:
  std::size_t open(std::string_view type) {
    const std::size_t at = w_.size();
    w_.u32be(0);
    w_.text(type.substr(0, 4));
    return at;
  }
  std::size_t open_full(std::string_view type, std::uint8_t version, std::uint32_t flags) {
    const std::size_t at = open(type);
    w_.u8(version).u24be(flags);
    return at;
  }
  void close(std::size_t at) { w_.patch_u32be(at, static_cast<std::uint32_t>(w_.size() - at)); }
  util::ByteWriter& w() { return w_; }
  [[nodiscard]] Bytes take() { return w_.take(); }

 private:
  util::ByteWriter w_;
};

struct Sample {
  std::uint32_t size = 0;
  std::int64_t dts = 0;
  std::int64_t pts = 0;
  std::int64_t duration = 0;
  bool key = false;
};

struct Chunk {
  std::uint64_t offset = 0;  // relative to the start of mdat payload
  std::uint32_t samples = 0;
};

struct Mp4Track {
  MuxTrack desc;
  StreamAdapter adapter;
  std::vector<Sample> samples;
  std::vector<Chunk> chunks;
  Bytes extradata_override;
  bool have_override = false;
  ByteView first_packet_view;
  Bytes first_packet;  // kept for configuration records needing the bitstream (vpcC, ac-3)
  std::uint32_t source_timescale = 1000;
  std::uint32_t frame_bytes = 0;  // PCM: bytes per sample frame
  std::uint64_t total_frames = 0;
};

std::uint32_t pcm_frame_bytes(const MuxTrack& t) {
  if (!t.codec.starts_with("pcm_")) return 0;
  const std::uint32_t channels = t.channels == 0 ? 1 : t.channels;
  if (t.codec == codec::id::pcm_s16le || t.codec == codec::id::pcm_s16be) return 2 * channels;
  return channels;  // 8-bit flavours
}

enum class Brand { Mp4, Mov, M4a, M4b, M4v, ThreeGp, ThreeG2 };

Brand brand_for(std::string_view id) {
  if (id == "mov") return Brand::Mov;
  if (id == "m4a") return Brand::M4a;
  if (id == "m4b") return Brand::M4b;
  if (id == "m4v") return Brand::M4v;
  if (id == "3gp") return Brand::ThreeGp;
  if (id == "3g2") return Brand::ThreeG2;
  return Brand::Mp4;
}

class Mp4Muxer final : public Muxer {
 public:
  explicit Mp4Muxer(std::string_view format_id) : brand_(brand_for(format_id)), format_id_(format_id) {}

  Result<void> begin(std::vector<MuxTrack> tracks, const MuxSettings& settings) override {
    settings_ = settings;
    for (MuxTrack& t : tracks) {
      if (!container_accepts(format_id_, t.type, t.codec)) {
        return fail(Status::Encode, "{} cannot store {} {}", format_id_,
                    t.type == ir::TrackType::Video ? "video" : "audio", t.codec);
      }
      Mp4Track track;
      track.adapter = StreamAdapter(t.codec, t.extradata, Packaging{});
      track.desc = std::move(t);
      if (track.desc.timescale == 0) track.desc.timescale = 1000;
      track.source_timescale = track.desc.timescale;
      track.frame_bytes = pcm_frame_bytes(track.desc);
      if (track.frame_bytes != 0) {
        if (track.desc.sample_rate == 0) return fail(Status::Encode, "PCM audio without a sample rate cannot be stored");
        track.desc.timescale = track.desc.sample_rate;
      }
      tracks_.push_back(std::move(track));
    }
    return {};
  }

  Result<void> write(std::size_t index, const ir::Packet& packet) override {
    Mp4Track& t = tracks_[index];
    CVT_TRY_ASSIGN(Bytes data, t.adapter.convert(packet.bytes, packet.keyframe));
    if (t.first_packet.empty()) t.first_packet = data;
    if (mdat_.size() + data.size() > settings_.limits.max_output_bytes) {
      return fail(Status::Limit, "the MP4 output exceeds the output size limit");
    }
    Sample s;
    s.size = static_cast<std::uint32_t>(data.size());
    s.dts = rescale(packet.dts, t.source_timescale, t.desc.timescale);
    s.pts = rescale(packet.pts, t.source_timescale, t.desc.timescale);
    s.duration = rescale(packet.duration, t.source_timescale, t.desc.timescale);
    s.key = packet.keyframe;
    if (t.frame_bytes != 0) {
      // PCM: one QuickTime sample per sample frame, one chunk per packet.
      const std::uint32_t frames = s.size / t.frame_bytes;
      s.duration = frames;
      t.chunks.push_back(Chunk{mdat_.size(), frames});
      t.total_frames += frames;
      last_track_ = index;
      t.samples.push_back(s);
      mdat_.insert(mdat_.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(std::uint64_t{frames} * t.frame_bytes));
      return {};
    }
    // Start a new chunk when another track wrote in between or the chunk grew large.
    if (last_track_ != index || t.chunks.empty() || t.chunks.back().samples >= 256 ||
        mdat_.size() - t.chunks.back().offset > (1u << 20)) {
      t.chunks.push_back(Chunk{mdat_.size(), 0});
    }
    ++t.chunks.back().samples;
    last_track_ = index;
    t.samples.push_back(s);
    mdat_.insert(mdat_.end(), data.begin(), data.end());
    return {};
  }

  void set_extradata(std::size_t track, Bytes extradata) override {
    tracks_[track].extradata_override = std::move(extradata);
    tracks_[track].have_override = true;
    tracks_[track].adapter = StreamAdapter(tracks_[track].desc.codec, tracks_[track].extradata_override, Packaging{});
  }

  Result<Bytes> finish() override {
    std::erase_if(tracks_, [](const Mp4Track& t) { return t.samples.empty(); });
    if (tracks_.empty()) return fail(Status::Encode, "nothing to write: the video has no packets");
    for (Mp4Track& t : tracks_) {
      // Durations: decode-time differences, the last one from the packet itself.
      for (std::size_t i = 0; i + 1 < t.samples.size(); ++i) {
        const std::int64_t d = t.samples[i + 1].dts - t.samples[i].dts;
        if (d < 0) return fail(Status::Encode, "decode timestamps go backwards in {} track", t.desc.codec);
        t.samples[i].duration = d;
      }
      if (t.samples.back().duration <= 0) {
        t.samples.back().duration = t.samples.size() > 1 ? t.samples[t.samples.size() - 2].duration : 1;
      }
    }
    const Bytes ftyp = build_ftyp();
    const std::uint64_t mdat_header = mdat_.size() + 8 > 0xFFFFFFFFull ? 16 : 8;
    // First pass with 32-bit offsets to learn the movie header size.
    bool co64 = false;
    CVT_TRY_ASSIGN(Bytes moov, build_moov(0, false));
    std::uint64_t data_start = ftyp.size() + moov.size() + mdat_header;
    if (data_start + mdat_.size() > 0xFFFFFFFFull) {
      co64 = true;
      CVT_TRY_ASSIGN(moov, build_moov(0, true));
      data_start = ftyp.size() + moov.size() + mdat_header;
    }
    CVT_TRY_ASSIGN(moov, build_moov(data_start, co64));
    const std::uint64_t total = data_start + mdat_.size();
    CVT_TRY(settings_.limits.check_output(total));
    Bytes out;
    out.reserve(static_cast<std::size_t>(total));
    out.insert(out.end(), ftyp.begin(), ftyp.end());
    out.insert(out.end(), moov.begin(), moov.end());
    util::ByteWriter header;
    if (mdat_header == 16) {
      header.u32be(1).text("mdat").u64be(mdat_.size() + 16);
    } else {
      header.u32be(static_cast<std::uint32_t>(mdat_.size() + 8)).text("mdat");
    }
    append(out, header.view());
    out.insert(out.end(), mdat_.begin(), mdat_.end());
    return out;
  }

 private:
  Bytes build_ftyp() const {
    BoxWriter b;
    const std::size_t at = b.open("ftyp");
    auto& w = b.w();
    switch (brand_) {
      case Brand::Mov: w.text("qt  ").u32be(0x200).text("qt  "); break;
      case Brand::M4a: w.text("M4A ").u32be(0x200).text("M4A isomiso2mp41"); break;
      case Brand::M4b: w.text("M4B ").u32be(0x200).text("M4B isomiso2mp41"); break;
      case Brand::M4v: w.text("M4V ").u32be(0x200).text("M4V isomiso2mp41"); break;
      case Brand::ThreeGp: w.text("3gp6").u32be(0x200).text("3gp6isom"); break;
      case Brand::ThreeG2: w.text("3g2a").u32be(0x10000).text("3g2aisom"); break;
      case Brand::Mp4: {
        w.text("isom").u32be(0x200).text("isomiso2");
        const bool avc = std::ranges::any_of(tracks_, [](const Mp4Track& t) { return t.desc.codec == codec::id::h264; });
        const bool av1 = std::ranges::any_of(tracks_, [](const Mp4Track& t) { return t.desc.codec == codec::id::av1; });
        if (avc) w.text("avc1");
        if (av1) w.text("av01");
        w.text("mp41");
        break;
      }
    }
    b.close(at);
    return b.take();
  }

  static constexpr std::uint32_t movie_timescale = 1000;

  struct Timing {
    std::int64_t dts0 = 0;
    std::int64_t start_pts = 0;
    std::int64_t end_pts = 0;
  };

  static Timing timing_of(const Mp4Track& t) {
    Timing tm;
    tm.dts0 = t.samples.front().dts;
    tm.start_pts = std::numeric_limits<std::int64_t>::max();
    tm.end_pts = std::numeric_limits<std::int64_t>::min();
    for (const Sample& s : t.samples) {
      tm.start_pts = std::min(tm.start_pts, s.pts);
      tm.end_pts = std::max(tm.end_pts, s.pts + s.duration);
    }
    return tm;
  }

  Result<Bytes> build_moov(std::uint64_t data_start, bool co64) {
    BoxWriter b;
    const std::size_t moov = b.open("moov");
    std::int64_t movie_duration = 0;
    for (const Mp4Track& t : tracks_) {
      const Timing tm = timing_of(t);
      const std::int64_t end = std::max<std::int64_t>(tm.end_pts, 0);
      movie_duration = std::max(movie_duration, rescale(end, t.desc.timescale, movie_timescale));
    }
    const bool wide = movie_duration > 0xFFFFFFFFll;
    {
      const std::size_t mvhd = b.open_full("mvhd", wide ? 1 : 0, 0);
      auto& w = b.w();
      if (wide) {
        w.u64be(0).u64be(0).u32be(movie_timescale).u64be(static_cast<std::uint64_t>(movie_duration));
      } else {
        w.u32be(0).u32be(0).u32be(movie_timescale).u32be(static_cast<std::uint32_t>(movie_duration));
      }
      w.u32be(0x00010000).u16be(0x0100).zeros(10);
      write_matrix(w, 0);
      w.zeros(24).u32be(static_cast<std::uint32_t>(tracks_.size() + 1));
      b.close(mvhd);
    }
    for (std::size_t i = 0; i < tracks_.size(); ++i) CVT_TRY(write_trak(b, tracks_[i], i, data_start, co64));
    if (!settings_.metadata.empty()) write_udta(b);
    b.close(moov);
    return b.take();
  }

  static void write_matrix(util::ByteWriter& w, int rotation) {
    std::int32_t a = 0x10000;
    std::int32_t bb = 0;
    std::int32_t c = 0;
    std::int32_t d = 0x10000;
    switch (rotation) {
      case 90: a = 0; bb = 0x10000; c = -0x10000; d = 0; break;
      case 180: a = -0x10000; d = -0x10000; break;
      case 270: a = 0; bb = -0x10000; c = 0x10000; d = 0; break;
      default: break;
    }
    w.i32be(a).i32be(bb).u32be(0).i32be(c).i32be(d).u32be(0).u32be(0).u32be(0).u32be(0x40000000);
  }

  void write_udta(BoxWriter& b) {
    static const std::map<std::string, std::uint32_t, std::less<>> keys = {
        {"title", 0xA96E616Du}, {"artist", 0xA9415254u}, {"album", 0xA9616C62u}, {"date", 0xA9646179u},
        {"comment", 0xA9636D74u}, {"genre", 0xA967656Eu}};
    const std::size_t udta = b.open("udta");
    const std::size_t meta = b.open_full("meta", 0, 0);
    {
      const std::size_t hdlr = b.open_full("hdlr", 0, 0);
      b.w().u32be(0).text("mdirappl").zeros(9);
      b.close(hdlr);
    }
    const std::size_t ilst = b.open("ilst");
    for (const auto& [key, value] : settings_.metadata) {
      const auto found = keys.find(key);
      if (found == keys.end()) continue;
      const std::size_t item = b.w().size();
      b.w().u32be(0).u32be(found->second);
      const std::size_t data = b.open("data");
      b.w().u32be(1).u32be(0).text(value);
      b.close(data);
      b.close(item);
    }
    b.close(ilst);
    b.close(meta);
    b.close(udta);
  }

  Result<void> write_trak(BoxWriter& b, Mp4Track& t, std::size_t index, std::uint64_t data_start, bool co64) {
    const bool video = t.desc.type == ir::TrackType::Video;
    const Timing tm = timing_of(t);
    const std::int64_t media_duration = t.samples.back().dts + t.samples.back().duration - tm.dts0;
    const std::int64_t presentation_start = std::max<std::int64_t>(tm.start_pts, 0);
    const std::int64_t presentation_end = std::max(tm.end_pts, presentation_start);
    const std::int64_t shown = presentation_end - presentation_start;
    const std::int64_t track_duration = rescale(presentation_end, t.desc.timescale, movie_timescale);
    const std::size_t trak = b.open("trak");
    {
      const bool wide = track_duration > 0xFFFFFFFFll;
      const std::size_t tkhd = b.open_full("tkhd", wide ? 1 : 0, 3);
      auto& w = b.w();
      if (wide) {
        w.u64be(0).u64be(0).u32be(static_cast<std::uint32_t>(index + 1)).u32be(0).u64be(static_cast<std::uint64_t>(track_duration));
      } else {
        w.u32be(0).u32be(0).u32be(static_cast<std::uint32_t>(index + 1)).u32be(0).u32be(static_cast<std::uint32_t>(track_duration));
      }
      w.zeros(8).u16be(0).u16be(video ? 0 : 1).u16be(video ? 0 : 0x0100).u16be(0);
      write_matrix(w, video ? t.desc.rotation : 0);
      w.u32be(video ? t.desc.width << 16 : 0).u32be(video ? t.desc.height << 16 : 0);
      b.close(tkhd);
    }
    // Edit list: map movie time 0 to the right media time and delay late starts.
    const std::int64_t media_start = presentation_start - tm.dts0;
    const std::int64_t empty = tm.start_pts > 0 ? rescale(tm.start_pts, t.desc.timescale, movie_timescale) : 0;
    if (media_start != 0 || empty != 0) {
      const std::size_t edts = b.open("edts");
      const std::size_t elst = b.open_full("elst", 0, 0);
      b.w().u32be(empty != 0 ? 2 : 1);
      if (empty != 0) b.w().u32be(static_cast<std::uint32_t>(empty)).i32be(-1).u32be(0x00010000);
      b.w()
          .u32be(static_cast<std::uint32_t>(std::max<std::int64_t>(0, rescale(shown, t.desc.timescale, movie_timescale))))
          .i32be(static_cast<std::int32_t>(media_start))
          .u32be(0x00010000);
      b.close(elst);
      b.close(edts);
    }
    const std::size_t mdia = b.open("mdia");
    {
      const bool wide = media_duration > 0xFFFFFFFFll;
      const std::size_t mdhd = b.open_full("mdhd", wide ? 1 : 0, 0);
      auto& w = b.w();
      if (wide) {
        w.u64be(0).u64be(0).u32be(t.desc.timescale).u64be(static_cast<std::uint64_t>(media_duration));
      } else {
        w.u32be(0).u32be(0).u32be(t.desc.timescale).u32be(static_cast<std::uint32_t>(media_duration));
      }
      std::string lang = t.desc.language.size() == 3 ? t.desc.language : "und";
      std::uint16_t packed = 0;
      for (const char c : lang) packed = static_cast<std::uint16_t>((packed << 5) | ((c - 0x60) & 0x1F));
      w.u16be(packed).u16be(0);
      b.close(mdhd);
    }
    {
      const std::size_t hdlr = b.open_full("hdlr", 0, 0);
      b.w().u32be(0).text(video ? "vide" : "soun").zeros(12).text(video ? "VideoHandler" : "SoundHandler").u8(0);
      b.close(hdlr);
    }
    const std::size_t minf = b.open("minf");
    if (video) {
      const std::size_t vmhd = b.open_full("vmhd", 0, 1);
      b.w().zeros(8);
      b.close(vmhd);
    } else {
      const std::size_t smhd = b.open_full("smhd", 0, 0);
      b.w().zeros(4);
      b.close(smhd);
    }
    {
      const std::size_t dinf = b.open("dinf");
      const std::size_t dref = b.open_full("dref", 0, 0);
      b.w().u32be(1);
      const std::size_t url = b.open_full("url ", 0, 1);
      b.close(url);
      b.close(dref);
      b.close(dinf);
    }
    const std::size_t stbl = b.open("stbl");
    CVT_TRY(write_stsd(b, t));
    if (t.frame_bytes != 0) {
      const std::size_t stts = b.open_full("stts", 0, 0);
      b.w().u32be(1).u32be(static_cast<std::uint32_t>(t.total_frames)).u32be(1);
      b.close(stts);
    } else {
      // stts
      std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
      for (const Sample& s : t.samples) {
        const auto d = static_cast<std::uint32_t>(std::clamp<std::int64_t>(s.duration, 0, 0xFFFFFFFFll));
        if (!runs.empty() && runs.back().second == d) {
          ++runs.back().first;
        } else {
          runs.emplace_back(1, d);
        }
      }
      const std::size_t stts = b.open_full("stts", 0, 0);
      b.w().u32be(static_cast<std::uint32_t>(runs.size()));
      for (const auto& [count, delta] : runs) b.w().u32be(count).u32be(delta);
      b.close(stts);
    }
    {
      // ctts (version 1 allows negative offsets)
      bool any = false;
      bool negative = false;
      for (const Sample& s : t.samples) {
        if (s.pts != s.dts) any = true;
        if (s.pts < s.dts) negative = true;
      }
      if (any && t.frame_bytes == 0) {
        std::vector<std::pair<std::uint32_t, std::int64_t>> runs;
        for (const Sample& s : t.samples) {
          const std::int64_t o = s.pts - s.dts;
          if (!runs.empty() && runs.back().second == o) {
            ++runs.back().first;
          } else {
            runs.emplace_back(1, o);
          }
        }
        const std::size_t ctts = b.open_full("ctts", negative ? 1 : 0, 0);
        b.w().u32be(static_cast<std::uint32_t>(runs.size()));
        for (const auto& [count, offset] : runs) {
          b.w().u32be(count).i32be(static_cast<std::int32_t>(std::clamp<std::int64_t>(offset, INT32_MIN, INT32_MAX)));
        }
        b.close(ctts);
      }
    }
    if (video && !std::ranges::all_of(t.samples, [](const Sample& s) { return s.key; })) {
      std::vector<std::uint32_t> keys;
      for (std::size_t i = 0; i < t.samples.size(); ++i) {
        if (t.samples[i].key) keys.push_back(static_cast<std::uint32_t>(i + 1));
      }
      const std::size_t stss = b.open_full("stss", 0, 0);
      b.w().u32be(static_cast<std::uint32_t>(keys.size()));
      for (const std::uint32_t k : keys) b.w().u32be(k);
      b.close(stss);
    }
    {
      const std::size_t stsc = b.open_full("stsc", 0, 0);
      std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;  // first chunk, samples
      for (std::size_t c = 0; c < t.chunks.size(); ++c) {
        if (runs.empty() || runs.back().second != t.chunks[c].samples) {
          runs.emplace_back(static_cast<std::uint32_t>(c + 1), t.chunks[c].samples);
        }
      }
      b.w().u32be(static_cast<std::uint32_t>(runs.size()));
      for (const auto& [first, samples] : runs) b.w().u32be(first).u32be(samples).u32be(1);
      b.close(stsc);
    }
    {
      const std::size_t stsz = b.open_full("stsz", 0, 0);
      const bool constant = std::ranges::all_of(t.samples, [&](const Sample& s) { return s.size == t.samples[0].size; });
      if (t.frame_bytes != 0) {
        b.w().u32be(t.frame_bytes).u32be(static_cast<std::uint32_t>(t.total_frames));
      } else {
        b.w().u32be(constant ? t.samples[0].size : 0).u32be(static_cast<std::uint32_t>(t.samples.size()));
      }
      if (!constant && t.frame_bytes == 0) {
        for (const Sample& s : t.samples) b.w().u32be(s.size);
      }
      b.close(stsz);
    }
    {
      const std::size_t stco = b.open_full(co64 ? "co64" : "stco", 0, 0);
      b.w().u32be(static_cast<std::uint32_t>(t.chunks.size()));
      for (const Chunk& c : t.chunks) {
        if (co64) {
          b.w().u64be(data_start + c.offset);
        } else {
          b.w().u32be(static_cast<std::uint32_t>(data_start + c.offset));
        }
      }
      b.close(stco);
    }
    b.close(stbl);
    b.close(minf);
    b.close(mdia);
    b.close(trak);
    return {};
  }

  static void write_descriptor(util::ByteWriter& w, std::uint8_t tag, ByteView body) {
    w.u8(tag);
    const auto size = static_cast<std::uint32_t>(body.size());
    w.u8(static_cast<std::uint8_t>(0x80 | ((size >> 21) & 0x7F)))
        .u8(static_cast<std::uint8_t>(0x80 | ((size >> 14) & 0x7F)))
        .u8(static_cast<std::uint8_t>(0x80 | ((size >> 7) & 0x7F)))
        .u8(static_cast<std::uint8_t>(size & 0x7F));
    w.bytes(body);
  }

  void write_esds(BoxWriter& b, const Mp4Track& t, std::uint8_t object_type, const Bytes& config) {
    std::uint64_t bytes = 0;
    for (const Sample& s : t.samples) bytes += s.size;
    const Timing tm = timing_of(t);
    const double seconds = static_cast<double>(std::max<std::int64_t>(1, tm.end_pts - tm.start_pts)) / t.desc.timescale;
    const auto avg = static_cast<std::uint32_t>(std::min(4.0e9, static_cast<double>(bytes) * 8.0 / seconds));
    std::uint32_t max_size = 0;
    for (const Sample& s : t.samples) max_size = std::max(max_size, s.size);
    util::ByteWriter dc;
    const bool audio = t.desc.type == ir::TrackType::Audio;
    dc.u8(object_type).u8(audio ? 0x15 : 0x11).u24be(std::min<std::uint32_t>(max_size, 0xFFFFFF)).u32be(avg).u32be(avg);
    if (!config.empty()) write_descriptor(dc, 5, config);
    util::ByteWriter es;
    es.u16be(static_cast<std::uint16_t>(1)).u8(0);
    write_descriptor(es, 4, dc.view());
    const std::byte sl[] = {std::byte{2}};
    write_descriptor(es, 6, sl);
    const std::size_t esds = b.open_full("esds", 0, 0);
    write_descriptor(b.w(), 3, es.view());
    b.close(esds);
  }

  Result<void> write_stsd(BoxWriter& b, Mp4Track& t) {
    const std::size_t stsd = b.open_full("stsd", 0, 0);
    b.w().u32be(1);
    const std::string& codec = t.desc.codec;
    const Bytes extradata = t.adapter.extradata();
    if (t.desc.type == ir::TrackType::Video) {
      std::string_view type = "mp4v";
      if (codec == codec::id::h264) type = "avc1";
      if (codec == codec::id::hevc) type = "hvc1";
      if (codec == codec::id::av1) type = "av01";
      if (codec == codec::id::vp9) type = "vp09";
      if (codec == codec::id::vp8) type = "vp08";
      if (codec == codec::id::mjpeg) type = "jpeg";
      if (codec == codec::id::h263) type = "s263";
      if (codec == codec::id::png) type = "png ";
      if (codec == codec::id::prores) type = "apcn";
      const std::size_t entry = b.open(type);
      auto& w = b.w();
      w.zeros(6).u16be(1).zeros(16);
      w.u16be(static_cast<std::uint16_t>(t.desc.width)).u16be(static_cast<std::uint16_t>(t.desc.height));
      w.u32be(0x00480000).u32be(0x00480000).u32be(0).u16be(1).zeros(32).u16be(0x18).i16be(-1);
      if (codec == codec::id::h264 || codec == codec::id::hevc) {
        if (extradata.empty()) return fail(Status::Encode, "the {} stream carries no parameter sets", codec);
        const std::size_t c = b.open(codec == codec::id::h264 ? "avcC" : "hvcC");
        w.bytes(extradata);
        b.close(c);
      } else if (codec == codec::id::av1) {
        if (extradata.empty()) return fail(Status::Encode, "the AV1 stream has no sequence header");
        const std::size_t c = b.open("av1C");
        w.bytes(extradata);
        b.close(c);
      } else if (codec == codec::id::vp9 || codec == codec::id::vp8) {
        bits::Vp9Info info;
        if (codec == codec::id::vp9) {
          if (const auto parsed = bits::parse_vp9_frame(t.first_packet)) info = *parsed;
        }
        info.width = t.desc.width;
        info.height = t.desc.height;
        const std::size_t c = b.open("vpcC");
        w.bytes(bits::build_vpcc(info));
        b.close(c);
      } else if (codec == codec::id::mpeg4 || codec == codec::id::mpeg2video || codec == codec::id::mpeg1video) {
        const std::uint8_t oti = codec == codec::id::mpeg4 ? 0x20 : codec == codec::id::mpeg2video ? 0x61 : 0x6A;
        write_esds(b, t, oti, extradata);
      } else if (codec == codec::id::h263) {
        const std::size_t c = b.open("d263");
        w.text("CVT ").u8(0).u8(10).u8(0);
        b.close(c);
      }
      if (t.desc.width != 0) {
        const std::size_t pasp = b.open("pasp");
        w.u32be(1).u32be(1);
        b.close(pasp);
      }
      b.close(entry);
    } else {
      const bool mov = brand_ == Brand::Mov;
      std::string_view type = "mp4a";
      if (codec == codec::id::opus) type = "Opus";
      if (codec == codec::id::flac) type = "fLaC";
      if (codec == codec::id::alac) type = "alac";
      if (codec == codec::id::ac3) type = "ac-3";
      if (codec == codec::id::pcm_s16le) type = "sowt";
      if (codec == codec::id::pcm_s16be) type = "twos";
      if (codec == codec::id::pcm_u8) type = "raw ";
      if (codec == codec::id::pcm_alaw) type = "alaw";
      if (codec == codec::id::pcm_mulaw) type = "ulaw";
      if (codec == codec::id::amr_nb) type = "samr";
      if (codec == codec::id::amr_wb) type = "sawb";
      const bool pcm = codec.starts_with("pcm_");
      const std::size_t entry = b.open(type);
      auto& w = b.w();
      std::uint32_t rate = t.desc.sample_rate;
      std::uint16_t channels = t.desc.channels;
      if (codec == codec::id::opus) rate = 48000;
      if (codec == codec::id::aac) {
        if (const auto asc = bits::parse_asc(extradata)) {
          if (rate == 0) rate = asc->sample_rate;
          if (channels == 0) channels = static_cast<std::uint16_t>(asc->channel_config);
        }
      }
      std::uint16_t sample_bits = 16;
      if (codec == codec::id::pcm_u8 || codec == codec::id::pcm_alaw || codec == codec::id::pcm_mulaw) sample_bits = 8;
      w.zeros(6).u16be(1);
      w.u16be(0).u16be(0).u32be(0);
      w.u16be(channels == 0 ? 2 : channels).u16be(sample_bits).u16be(0).u16be(0);
      w.u32be(rate < 65536 ? rate << 16 : 0);
      (void)mov;
      if (codec == codec::id::aac || codec == codec::id::mp3 || codec == codec::id::mp2) {
        const std::uint8_t oti = codec == codec::id::aac ? 0x40 : codec == codec::id::mp3 ? 0x6B : 0x69;
        if (codec == codec::id::aac && extradata.empty()) return fail(Status::Encode, "the AAC stream has no configuration");
        write_esds(b, t, oti, extradata);
      } else if (codec == codec::id::opus) {
        Bytes head = extradata;
        if (head.empty()) head = bits::make_opus_head(channels == 0 ? 2 : channels, t.desc.sample_rate, 312);
        const auto dops = bits::opus_head_to_dops(head);
        if (!dops) return fail(Status::Encode, "the Opus stream has an invalid header");
        const std::size_t c = b.open("dOps");
        w.bytes(*dops);
        b.close(c);
      } else if (codec == codec::id::flac) {
        if (extradata.size() < 34) return fail(Status::Encode, "the FLAC stream has no STREAMINFO block");
        const std::size_t c = b.open_full("dfLa", 0, 0);
        const ByteView info = ByteView(extradata).last(34);
        w.u8(0x80).u24be(34).bytes(info);
        b.close(c);
      } else if (codec == codec::id::alac) {
        const std::size_t c = b.open_full("alac", 0, 0);
        w.bytes(extradata);
        b.close(c);
      } else if (codec == codec::id::ac3) {
        const auto h = parse_ac3(t.first_packet);
        if (!h) return fail(Status::Encode, "the AC-3 stream does not start with a sync frame");
        bits::BitWriter bw;
        bw.put(static_cast<std::uint32_t>(h->fscod), 2);
        bw.put(static_cast<std::uint32_t>(h->bsid), 5);
        bw.put(static_cast<std::uint32_t>(h->bsmod), 3);
        bw.put(static_cast<std::uint32_t>(h->acmod), 3);
        bw.put(static_cast<std::uint32_t>(h->lfe), 1);
        bw.put(static_cast<std::uint32_t>(h->bitrate_code >> 1), 5);
        bw.put(0, 5);
        const std::size_t c = b.open("dac3");
        w.bytes(bw.take());
        b.close(c);
      } else if (codec == codec::id::amr_nb || codec == codec::id::amr_wb) {
        const std::size_t c = b.open("damr");
        w.text("CVT ").u8(0).u16be(0x81FF).u8(0).u8(1);
        b.close(c);
      } else if (pcm) {
        // sample size and channels above describe the PCM layout
      }
      b.close(entry);
    }
    b.close(stsd);
    return {};
  }

  Brand brand_;
  std::string format_id_;
  MuxSettings settings_;
  std::vector<Mp4Track> tracks_;
  Bytes mdat_;
  std::size_t last_track_ = static_cast<std::size_t>(-1);
};

}  // namespace

std::unique_ptr<Muxer> make_mp4_muxer(std::string_view format_id) { return std::make_unique<Mp4Muxer>(format_id); }

}  // namespace cvt::video
