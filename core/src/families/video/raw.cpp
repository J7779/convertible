// Convertible core - video family: simple containers. IVF, YUV4MPEG2, raw
// video elementary streams (Annex-B H.264/HEVC, AV1 OBUs, MPEG-1/2 video),
// and the audio files a video's sound can be copied into unchanged (MP3, ADTS
// AAC, FLAC, WAV).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numeric>
#include <optional>

#include "codec/ids.hpp"
#include "families/video/adapt.hpp"
#include "families/video/bitstream.hpp"
#include "families/video/containers.hpp"
#include "util/text.hpp"

namespace cvt::video {

namespace {
namespace id = codec::id;
}

// =========================================================================== IVF

Result<std::shared_ptr<Demuxed>> demux_ivf(ByteView input, const ReadSettings& settings) {
  util::ByteReader r(input);
  if (!r.consume("DKIF")) return fail(Status::Decode, "not an IVF file");
  r.skip(2);
  const std::uint16_t header = r.u16le();
  const std::uint32_t fcc = std::byteswap(r.u32le());
  const std::uint16_t width = r.u16le();
  const std::uint16_t height = r.u16le();
  const std::uint32_t rate = r.u32le();
  const std::uint32_t scale = r.u32le();
  if (!r.ok() || header < 32 || !r.seek(header)) return fail(Status::Decode, "IVF header is truncated");
  auto out = std::make_shared<Demuxed>();
  out->input = input;
  TrackInfo t;
  t.type = ir::TrackType::Video;
  t.tag = bits::fourcc_text(fcc);
  if (fcc == bits::fourcc("VP80")) t.codec = std::string(id::vp8);
  if (fcc == bits::fourcc("VP90")) t.codec = std::string(id::vp9);
  if (fcc == bits::fourcc("AV01")) t.codec = std::string(id::av1);
  t.width = width;
  t.height = height;
  t.timescale = rate == 0 ? 1000 : rate;
  const std::int64_t tick = scale == 0 ? 1 : scale;
  std::uint64_t frames = 0;
  while (r.remaining() >= 12) {
    const std::uint32_t size = r.u32le();
    const auto pts = static_cast<std::int64_t>(r.u64le() & 0x7FFFFFFFFFFFFFFFull);
    const std::size_t at = r.position();
    if (size > r.remaining()) {
      settings.warnings.add("the IVF file is truncated; the last frame was dropped");
      break;
    }
    const ByteView frame = r.bytes(size);
    bool key = true;
    if (t.codec == id::vp8) {
      if (const auto info = bits::parse_vp8_frame(frame)) key = info->keyframe;
    } else if (t.codec == id::vp9) {
      if (const auto info = bits::parse_vp9_frame(frame)) key = info->keyframe;
    } else if (t.codec == id::av1) {
      key = bits::av1_is_keyframe(frame);
    }
    out->add_input_packet(t, at, size, sat_mul(pts, tick), sat_mul(pts, tick), 0, key);
    if ((++frames & 1023) == 0 && !settings.progress.report(static_cast<double>(at) / static_cast<double>(input.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  out->tracks.push_back(std::move(t));
  CVT_TRY(finish_demux(*out, "IVF"));
  return out;
}

// =========================================================================== YUV4MPEG2

namespace {

struct Y4mHeader {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::int64_t fps_num = 25;
  std::int64_t fps_den = 1;
  std::string colorspace = "420jpeg";
};

std::optional<std::pair<std::int64_t, std::int64_t>> ratio(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  const auto a = util::parse_int(text.substr(0, colon));
  const auto b = util::parse_int(text.substr(colon + 1));
  if (!a || !b || *a <= 0 || *b <= 0) return std::nullopt;
  return std::pair{*a, *b};
}

}  // namespace

Result<std::shared_ptr<Demuxed>> demux_y4m(ByteView input, const ReadSettings& settings) {
  const std::string_view text = as_chars(input);
  if (!text.starts_with("YUV4MPEG2 ")) return fail(Status::Decode, "not a YUV4MPEG2 file");
  const std::size_t eol = text.find('\n');
  if (eol == std::string_view::npos || eol > 4096) return fail(Status::Decode, "YUV4MPEG2 header is truncated");
  Y4mHeader h;
  for (const std::string_view token : util::split(text.substr(10, eol - 10), ' ', false)) {
    if (token.empty()) continue;
    const std::string_view value = token.substr(1);
    switch (token[0]) {
      case 'W': h.width = static_cast<std::uint32_t>(util::parse_int(value).value_or(0)); break;
      case 'H': h.height = static_cast<std::uint32_t>(util::parse_int(value).value_or(0)); break;
      case 'F':
        if (const auto f = ratio(value)) {
          h.fps_num = f->first;
          h.fps_den = f->second;
        }
        break;
      case 'C': h.colorspace = std::string(value); break;
      default: break;
    }
  }
  if (h.width == 0 || h.height == 0 || h.width > 65536 || h.height > 65536) {
    return fail(Status::Decode, "YUV4MPEG2 picture size is missing or invalid");
  }
  CVT_TRY(settings.limits.check_pixels(h.width, h.height));
  std::string_view layout;
  ir::PixelFormat format = ir::PixelFormat::I420;
  if (h.colorspace.starts_with("420") && h.colorspace.find("p10") == std::string::npos &&
      h.colorspace.find("p1") == std::string::npos) {
    layout = "I420";
  } else if (h.colorspace == "422") {
    layout = "Y42B";
    format = ir::PixelFormat::I422;
  } else if (h.colorspace == "444") {
    layout = "444P";
    format = ir::PixelFormat::I444;
  } else if (h.colorspace == "420p10") {
    layout = "I0AL";
    format = ir::PixelFormat::I420P10;
  } else {
    return fail(Status::Decode, "YUV4MPEG2 colour space '{}' is not supported", util::sanitize_utf8(h.colorspace));
  }
  std::uint64_t frame_bytes = 0;
  for (std::size_t p = 0; p < ir::plane_count(format); ++p) {
    frame_bytes += std::uint64_t{ir::plane_row_bytes(format, p, h.width)} * ir::plane_height(format, p, h.height);
  }
  auto out = std::make_shared<Demuxed>();
  out->input = input;
  TrackInfo t;
  t.type = ir::TrackType::Video;
  t.codec = std::string(id::rawvideo);
  t.tag = h.colorspace;
  t.extradata = to_bytes(layout);
  t.width = h.width;
  t.height = h.height;
  const bool small = h.fps_num <= 0xFFFFFFFFll;
  t.timescale = small ? static_cast<std::uint32_t>(h.fps_num) : 1000;
  const std::int64_t step = small ? h.fps_den : std::max<std::int64_t>(1, 1000 * h.fps_den / h.fps_num);
  t.fps = ir::Rational{h.fps_num, h.fps_den};
  std::size_t pos = eol + 1;
  std::int64_t n = 0;
  while (pos < text.size()) {
    if (text.substr(pos, 5) != "FRAME") return fail(Status::Decode, "YUV4MPEG2 frame header is missing at byte {}", pos);
    const std::size_t line = text.find('\n', pos);
    if (line == std::string_view::npos || line - pos > 4096) break;
    const std::size_t data = line + 1;
    if (frame_bytes > input.size() - data) {
      settings.warnings.add("the YUV4MPEG2 file is truncated; the last frame was dropped");
      break;
    }
    out->add_input_packet(t, data, frame_bytes, n * step, n * step, step, true);
    ++n;
    pos = data + static_cast<std::size_t>(frame_bytes);
    if ((n & 63) == 0 && !settings.progress.report(static_cast<double>(pos) / static_cast<double>(text.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  out->tracks.push_back(std::move(t));
  CVT_TRY(finish_demux(*out, "YUV4MPEG2"));
  return out;
}

// =========================================================================== raw elementary streams

Result<std::shared_ptr<Demuxed>> demux_raw(ByteView input, const ReadSettings& settings, std::string_view codec, double fps) {
  const double clock = fps > 0 ? fps : 25.0;  // 0 = not chosen by the user
  auto out = std::make_shared<Demuxed>();
  out->input = input;
  TrackInfo t;
  t.type = ir::TrackType::Video;
  t.codec = std::string(codec);
  t.tag = t.codec;
  EsStream es;
  es.add(0, input.size());
  if (codec == id::av1) {
    // Low-overhead OBUs; a temporal unit starts at each temporal delimiter.
    util::ByteReader r(input);
    std::optional<std::size_t> unit_start;
    std::int64_t n = 0;
    const auto d = static_cast<std::int64_t>(std::llround(90000.0 / clock));
    const auto close = [&](std::size_t end) {
      if (!unit_start || end <= *unit_start) return;
      const ByteView unit = input.subspan(*unit_start, end - *unit_start);
      out->add_input_packet(t, *unit_start, end - *unit_start, n * d, n * d, d, bits::av1_is_keyframe(unit));
      ++n;
    };
    while (!r.at_end()) {
      const std::size_t start = r.position();
      const std::uint8_t header = r.u8();
      if ((header & 0x80) != 0 || (header & 2) == 0) {
        if (!unit_start) return fail(Status::Decode, "not an AV1 low-overhead bitstream");
        settings.warnings.add("the AV1 stream has damaged data at its end");
        break;
      }
      if (header & 4) r.skip(1);
      std::uint64_t size = 0;
      int shift = 0;
      while (true) {
        const std::uint8_t b = r.u8();
        if (!r.ok() || shift > 56) break;
        size |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
      }
      if (!r.ok() || size > r.remaining()) {
        settings.warnings.add("the AV1 stream is truncated");
        r.seek(start);
        break;
      }
      if (((header >> 3) & 0xF) == 2) {
        close(start);
        unit_start = start;
      }
      if (!unit_start) unit_start = start;
      r.skip(static_cast<std::size_t>(size));
    }
    close(r.position());
    t.timescale = 90000;
    if (const auto dims = bits::av1_dimensions(input.first(std::min<std::size_t>(input.size(), 4096)))) {
      t.width = dims->first;
      t.height = dims->second;
    }
  } else if (codec == id::mpeg2video || codec == id::mpeg1video) {
    const auto seq = bits::parse_mpeg12_sequence(input.first(std::min<std::size_t>(input.size(), 4096)));
    if (!seq) return fail(Status::Decode, "not an MPEG-1/2 video stream (no sequence header)");
    t.codec = std::string(seq->mpeg2 ? id::mpeg2video : id::mpeg1video);
    t.width = seq->width;
    t.height = seq->height;
    if (seq->fps_num > 0) t.fps = ir::Rational{seq->fps_num, seq->fps_den};
    split_mpeg12_video(*out, t, es, {}, clock);
  } else {
    const bool hevc = codec == id::hevc;
    const std::string_view name = hevc ? "HEVC" : "H.264";
    if (!bits::starts_with_start_code(input)) return fail(Status::Decode, "not an Annex-B {} stream", name);
    // The first sequence parameter set gives the picture size and the VUI frame rate.
    std::optional<bits::SpsInfo> sps;
    for (const ByteView nal : bits::split_annexb(input.first(std::min<std::size_t>(input.size(), 1u << 20)))) {
      if ((hevc ? bits::hevc_nal_type(nal) : bits::h264_nal_type(nal)) != (hevc ? 33 : 7)) continue;
      sps = hevc ? bits::parse_hevc_sps(nal) : bits::parse_h264_sps(nal);
      if (sps) break;
    }
    if (!sps) return fail(Status::Decode, "not an {} stream: no valid sequence parameter set found", name);
    CVT_TRY(settings.limits.check_pixels(sps->width, sps->height));
    t.width = sps->width;
    t.height = sps->height;
    ir::Rational rate{static_cast<std::int64_t>(std::llround(fps * 1000)), 1000};
    if (fps <= 0) {
      rate = {25, 1};
      if (sps->fps_num > 0 && sps->fps_den > 0) {
        const double vui = static_cast<double>(sps->fps_num) / sps->fps_den;
        if (vui >= 1.0 && vui <= 1000.0) rate = {sps->fps_num, sps->fps_den};
      }
      const std::int64_t g = std::gcd(rate.num, rate.den);
      rate = {rate.num / g, rate.den / g};
    }
    split_annexb_video(*out, t, es, {}, rate.value());
    // Every picture lasts one frame; raw streams have no other clock.
    const auto d = static_cast<std::int64_t>(std::llround(90000.0 / rate.value()));
    for (PacketRef& p : t.packets) p.duration = d;
    derive_h264_pts(*out, t, true);
    t.fps = rate;
  }
  out->tracks.push_back(std::move(t));
  CVT_TRY(finish_demux(*out, "raw video"));
  return out;
}

// =========================================================================== writers

namespace {

class SimpleMuxer : public Muxer {
 public:
  Result<void> begin(std::vector<MuxTrack> tracks, const MuxSettings& settings) override {
    settings_ = settings;
    if (tracks.size() != 1) return fail(Status::Encode, "{} holds exactly one stream", settings.format_id);
    track_ = std::move(tracks.front());
    if (!container_accepts(settings.format_id, track_.type, track_.codec)) {
      return fail(Status::Encode, "{} cannot store {}", settings.format_id, track_.codec);
    }
    adapter_ = StreamAdapter(track_.codec, track_.extradata, packaging());
    return start();
  }
  Result<void> write(std::size_t, const ir::Packet& packet) override {
    CVT_TRY_ASSIGN(Bytes data, adapter_.convert(packet.bytes, packet.keyframe));
    CVT_TRY(frame(packet, data));
    if (out_.size() > settings_.limits.max_output_bytes) return fail(Status::Limit, "the output exceeds the output size limit");
    return {};
  }
  void set_extradata(std::size_t, Bytes extradata) override {
    track_.extradata = extradata;
    adapter_ = StreamAdapter(track_.codec, std::move(extradata), packaging());
  }
  Result<Bytes> finish() override {
    CVT_TRY(end());
    return out_.take();
  }

 protected:
  virtual Packaging packaging() const { return Packaging{}; }
  virtual Result<void> start() { return {}; }
  virtual Result<void> frame(const ir::Packet& packet, const Bytes& data) = 0;
  virtual Result<void> end() { return {}; }

  MuxSettings settings_;
  MuxTrack track_;
  StreamAdapter adapter_;
  util::ByteWriter out_;
};

class IvfMuxer final : public SimpleMuxer {
 protected:
  Packaging packaging() const override {
    Packaging p;
    p.av1_temporal_delimiters = true;
    return p;
  }
  Result<void> start() override {
    std::uint32_t fcc = bits::fourcc("VP90");
    if (track_.codec == id::vp8) fcc = bits::fourcc("VP80");
    if (track_.codec == id::av1) fcc = bits::fourcc("AV01");
    // Time base: one tick per frame when the rate is known, else milliseconds.
    if (track_.fps.num > 0 && track_.fps.den > 0 && track_.fps.num <= 0xFFFFFFFFll && track_.fps.den <= 0xFFFFFFFFll) {
      rate_ = static_cast<std::uint32_t>(track_.fps.num);
      scale_ = static_cast<std::uint32_t>(track_.fps.den);
    }
    out_.text("DKIF").u16le(0).u16le(32).u32le(std::byteswap(fcc));
    out_.u16le(static_cast<std::uint16_t>(track_.width)).u16le(static_cast<std::uint16_t>(track_.height));
    out_.u32le(rate_).u32le(scale_).u32le(0).u32le(0);
    return {};
  }
  Result<void> frame(const ir::Packet& packet, const Bytes& data) override {
    const std::int64_t ticks = rescale(packet.pts, static_cast<std::int64_t>(track_.timescale) * scale_, rate_);
    out_.u32le(static_cast<std::uint32_t>(data.size())).u64le(static_cast<std::uint64_t>(std::max<std::int64_t>(ticks, 0)));
    out_.bytes(data);
    ++frames_;
    return {};
  }
  Result<void> end() override {
    out_.patch_u32le(24, frames_);
    return {};
  }

 private:
  std::uint32_t rate_ = 1000;
  std::uint32_t scale_ = 1;
  std::uint32_t frames_ = 0;
};

class Y4mMuxer final : public SimpleMuxer {
 protected:
  Result<void> start() override {
    const Bytes tag = track_.extradata;
    std::string_view c;
    if (as_chars(tag) == "I420") c = "420jpeg";
    if (as_chars(tag) == "Y42B") c = "422";
    if (as_chars(tag) == "444P") c = "444";
    if (as_chars(tag) == "I0AL") c = "420p10";
    if (c.empty()) return fail(Status::Encode, "YUV4MPEG2 cannot store raw video in layout '{}'", util::sanitize_utf8(as_chars(tag)));
    std::int64_t num = track_.fps.num > 0 ? track_.fps.num : 25;
    std::int64_t den = track_.fps.num > 0 ? track_.fps.den : 1;
    out_.text(std::format("YUV4MPEG2 W{} H{} F{}:{} Ip A1:1 C{}\n", track_.width, track_.height, num, den, c));
    return {};
  }
  Result<void> frame(const ir::Packet&, const Bytes& data) override {
    out_.text("FRAME\n");
    out_.bytes(data);
    return {};
  }
};

class RawMuxer final : public SimpleMuxer {
 public:
  explicit RawMuxer(std::string format) : format_(std::move(format)) {}

 protected:
  Packaging packaging() const override {
    Packaging p;
    p.length_prefixed = false;
    p.av1_temporal_delimiters = true;
    p.adts = true;
    return p;
  }
  Result<void> start() override {
    if (format_ == "flac") {
      const Bytes info = track_.extradata;
      if (info.size() < 34) return fail(Status::Encode, "the FLAC stream has no STREAMINFO block");
      out_.text("fLaC").u8(0x80).u24be(34).bytes(ByteView(info).last(34));
    }
    if (format_ == "wav") {
      std::uint16_t tag = 1;
      std::uint16_t bits = 16;
      if (track_.codec == id::pcm_u8) bits = 8;
      if (track_.codec == id::pcm_s24le) bits = 24;
      if (track_.codec == id::pcm_f32le) {
        tag = 3;
        bits = 32;
      }
      if (track_.codec == id::pcm_alaw) {
        tag = 6;
        bits = 8;
      }
      if (track_.codec == id::pcm_mulaw) {
        tag = 7;
        bits = 8;
      }
      const std::uint16_t channels = track_.channels == 0 ? 1 : track_.channels;
      const auto align = static_cast<std::uint16_t>(channels * (bits / 8));
      out_.text("RIFF").u32le(0).text("WAVE").text("fmt ").u32le(16);
      out_.u16le(tag).u16le(channels).u32le(track_.sample_rate).u32le(track_.sample_rate * align).u16le(align).u16le(bits);
      out_.text("data").u32le(0);
    }
    return {};
  }
  Result<void> frame(const ir::Packet&, const Bytes& data) override {
    out_.bytes(data);
    return {};
  }
  Result<void> end() override {
    if (format_ == "wav") {
      const std::uint64_t size = out_.size();
      if (size > 0xFFFFFFFFull) return fail(Status::Limit, "WAV files cannot exceed 4 GiB");
      std::uint64_t data = size - 44;
      if (data & 1) out_.u8(0);
      out_.patch_u32le(4, static_cast<std::uint32_t>(out_.size() - 8));
      out_.patch_u32le(40, static_cast<std::uint32_t>(data));
    }
    return {};
  }

 private:
  std::string format_;
};

}  // namespace

std::unique_ptr<Muxer> make_ivf_muxer() { return std::make_unique<IvfMuxer>(); }
std::unique_ptr<Muxer> make_y4m_muxer() { return std::make_unique<Y4mMuxer>(); }
std::unique_ptr<Muxer> make_raw_muxer(std::string_view format_id) { return std::make_unique<RawMuxer>(std::string(format_id)); }

}  // namespace cvt::video
