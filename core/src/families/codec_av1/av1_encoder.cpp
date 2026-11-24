// Convertible core - codec_av1 family: the AV1 stream encoder
// (codec::VideoEncoder for codec::id::av1) and the speed/quality policy.
//
// Packets are temporal units in the Section 5 layout WITHOUT temporal
// delimiters (the form MP4 and Matroska store; IVF/.obu writers add one with
// av1::with_temporal_delimiter). Hidden alt-reference frames travel in the
// packet of the next shown frame, so packets come out in presentation order
// with dts == pts. extradata() is the av1C record.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>

#include "codec/colorspace.hpp"
#include "families/codec_av1/av1_api.hpp"
#include "families/codec_av1/av1_codec.hpp"
#include "families/codec_av1/obu.hpp"

namespace cvt::av1 {

// Speeds were measured with the generic C build (Release, one thread, Apple
// M-series; numbers in core/tests/data/codec_av1/SOURCES.md):
//
//   1280x720 video, q90:  good cpu-used 6 (lag 19)  1.6 fps, smallest output
//                         realtime cpu-used 8        8 fps, about 2.5x larger
//                         realtime cpu-used 10      14 fps, about 2.7x larger
//   good cpu-used 7..9 behave like 6 in this libaom version.
//   12 MP still, q90:     all-intra cpu-used 6  5.8 s;  cpu-used 9  0.6 s
//                         (about 1.5x larger);  cpu-used 5  46 s
//
// "Small" always takes libaom's good-quality mode. "Balanced" (the default)
// keeps it up to standard definition, where it still runs at several frames
// per second, and switches to the real-time mode above, where the good mode
// would take minutes per second of video. "Fast" is always real-time.
SpeedChoice choose_video_speed(codec::EncodePreset preset, std::uint64_t pixels) {
  const bool large = pixels > 640ull * 480ull;
  const bool huge = pixels > 1920ull * 1088ull;
  switch (preset) {
    case codec::EncodePreset::Small: return {detail::Usage::GoodQuality, 6, 19};
    case codec::EncodePreset::Balanced:
      if (large) return {detail::Usage::Realtime, huge ? 9 : 8, 0};
      return {detail::Usage::GoodQuality, 6, 19};
    case codec::EncodePreset::Fast: return {detail::Usage::Realtime, large ? 10 : 9, 0};
  }
  return {};
}

SpeedChoice choose_still_speed(codec::EncodePreset preset, std::uint64_t pixels) {
  switch (preset) {
    case codec::EncodePreset::Small: return {detail::Usage::AllIntra, pixels <= 1000000ull ? 5 : 6, 0};
    case codec::EncodePreset::Balanced: return {detail::Usage::AllIntra, 6, 0};
    case codec::EncodePreset::Fast: return {detail::Usage::AllIntra, 9, 0};
  }
  return {};
}

int quality_to_cq_level(int quality) {
  // Piecewise linear through measured anchor points: 90 is visually
  // transparent for typical content, 50 is a small but decent file.
  struct Anchor {
    int quality;
    int cq;
  };
  static constexpr Anchor anchors[] = {{1, 63}, {30, 52}, {50, 42}, {75, 30}, {90, 20}, {100, 6}};
  quality = std::clamp(quality, 1, 100);
  for (std::size_t i = 1; i < std::size(anchors); ++i) {
    if (quality <= anchors[i].quality) {
      const Anchor a = anchors[i - 1];
      const Anchor b = anchors[i];
      const double t = static_cast<double>(quality - a.quality) / static_cast<double>(b.quality - a.quality);
      return static_cast<int>(std::lround(a.cq + t * (b.cq - a.cq)));
    }
  }
  return anchors[std::size(anchors) - 1].cq;
}

PictureColor cicp_from_color_info(const ir::ColorInfo& info, bool ten_bit) {
  PictureColor c;
  switch (info.matrix) {
    case ir::ColorMatrix::BT709:
      c.color_primaries = 1;
      c.transfer_characteristics = 1;
      c.matrix_coefficients = 1;
      break;
    case ir::ColorMatrix::BT601:
      c.color_primaries = 6;
      c.transfer_characteristics = 6;
      c.matrix_coefficients = 6;
      break;
    case ir::ColorMatrix::BT2020:
      c.color_primaries = 9;
      c.transfer_characteristics = ten_bit ? 14 : 1;
      c.matrix_coefficients = 9;
      break;
  }
  c.full_range = info.range == ir::ColorRange::Full;
  switch (info.siting) {
    case ir::ChromaSiting::Left: c.chroma_sample_position = 1; break;
    case ir::ChromaSiting::TopLeft: c.chroma_sample_position = 2; break;
    case ir::ChromaSiting::Center: c.chroma_sample_position = 0; break;
  }
  return c;
}

namespace {

std::optional<ir::PixelFormat> coded_format_for(ir::PixelFormat input) {
  switch (input) {
    case ir::PixelFormat::I420:
    case ir::PixelFormat::I422:
    case ir::PixelFormat::I444:
    case ir::PixelFormat::I420P10:
    case ir::PixelFormat::I422P10:
    case ir::PixelFormat::I444P10: return input;
    case ir::PixelFormat::NV12:
    case ir::PixelFormat::RGBA8: return ir::PixelFormat::I420;
    case ir::PixelFormat::P010: return ir::PixelFormat::I420P10;
  }
  return std::nullopt;
}

class Av1Encoder final : public codec::VideoEncoder {
 public:
  Result<void> configure(const codec::VideoEncodeParams& params) override {
    if (configured_) return fail(Status::Internal, "AV1 encoder configured twice");
    configured_ = true;
    if (params.width == 0 || params.height == 0 || params.width > 65536 || params.height > 65536) {
      return fail(Status::InvalidArgument, "AV1 cannot encode {}x{} video", params.width, params.height);
    }
    const auto coded = coded_format_for(params.pixel_format);
    if (!coded) return fail(Status::InvalidArgument, "unsupported pixel format for the AV1 encoder");
    timescale_ = params.timescale == 0 ? 90000 : params.timescale;
    color_ = params.color;
    const double fps = params.fps.value();
    default_duration_ms_ = fps > 0.5 && fps < 1000.0 ? std::max<std::int64_t>(1, std::lround(1000.0 / fps)) : 33;

    const std::uint64_t pixels = std::uint64_t{params.width} * params.height;
    const SpeedChoice speed = choose_video_speed(params.preset, pixels);
    detail::EncoderSettings s;
    s.width = params.width;
    s.height = params.height;
    s.format = *coded;
    s.usage = speed.usage;
    s.cpu_used = speed.cpu_used;
    s.lag_in_frames = speed.lag_in_frames;
    s.cq_level = quality_to_cq_level(params.quality);
    s.bitrate_kbps = params.bitrate_kbps;
    s.keyframe_interval = params.keyframe_interval;
    s.color = cicp_from_color_info(params.color, ir::bit_depth(*coded) > 8);
    format_ = *coded;
    width_ = params.width;
    height_ = params.height;
    CVT_TRY(core_.open(s));
    const Bytes sequence_header = core_.sequence_header();
    if (!sequence_header.empty()) {
      auto av1c = make_av1c(sequence_header);
      if (av1c) extradata_ = std::move(*av1c);
    }
    return {};
  }

  Result<void> send_frame(const codec::VideoFrame& frame) override {
    if (!configured_) return fail(Status::Internal, "AV1 encoder used before configure()");
    if (flushed_) return fail(Status::Internal, "AV1 encoder fed after flush()");
    if (frame.width != width_ || frame.height != height_) {
      return fail(Status::InvalidArgument, "frame size {}x{} differs from the configured {}x{}", frame.width, frame.height,
                  width_, height_);
    }
    if (!frame.valid()) return fail(Status::InvalidArgument, "invalid video frame");
    std::int64_t pts = frame.pts_ms;
    const std::int64_t duration = frame.duration_ms > 0 ? frame.duration_ms : default_duration_ms_;
    if (have_last_pts_ && pts <= last_pts_) pts = last_pts_ + std::max<std::int64_t>(1, last_duration_);
    have_last_pts_ = true;
    last_pts_ = pts;
    last_duration_ = duration;
    if (frame.format == format_) {
      CVT_TRY(core_.encode(frame, pts, duration));
    } else {
      CVT_TRY_ASSIGN(const codec::VideoFrame converted, codec::convert_frame(frame, format_, color_));
      CVT_TRY(core_.encode(converted, pts, duration));
    }
    return drain();
  }

  Result<std::optional<codec::Packet>> receive_packet() override {
    if (packets_.empty()) return std::optional<codec::Packet>();
    codec::Packet packet = std::move(packets_.front());
    packets_.pop_front();
    return std::optional<codec::Packet>(std::move(packet));
  }

  Bytes extradata() const override { return extradata_; }

  Result<void> flush() override {
    if (!configured_) return fail(Status::Internal, "AV1 encoder used before configure()");
    if (flushed_) return {};
    flushed_ = true;
    for (int guard = 0; guard < 100000; ++guard) {
      CVT_TRY_ASSIGN(const bool more, core_.flush_step());
      CVT_TRY(drain());
      if (!more) break;
    }
    return {};
  }

 private:
  Result<void> drain() {
    while (std::optional<detail::EncodedUnit> unit = core_.next_unit()) {
      if (unit->keyframe && !extradata_from_stream_) {
        CVT_TRY_ASSIGN(const std::optional<Bytes> header, find_sequence_header_obu(unit->data));
        if (header) {
          CVT_TRY_ASSIGN(extradata_, make_av1c(*header));
          extradata_from_stream_ = true;
        }
      }
      codec::Packet packet;
      CVT_TRY_ASSIGN(packet.bytes, strip_temporal_delimiters(unit->data));
      packet.pts = ms_to_ticks(unit->pts_ms, timescale_);
      packet.dts = packet.pts;
      packet.duration = ms_to_ticks(unit->duration_ms, timescale_);
      packet.keyframe = unit->keyframe;
      packets_.push_back(std::move(packet));
    }
    return {};
  }

  detail::EncoderCore core_;
  ir::PixelFormat format_ = ir::PixelFormat::I420;
  ir::ColorInfo color_;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t timescale_ = 90000;
  std::int64_t default_duration_ms_ = 33;
  std::int64_t last_pts_ = 0;
  std::int64_t last_duration_ = 0;
  bool have_last_pts_ = false;
  bool configured_ = false;
  bool flushed_ = false;
  bool extradata_from_stream_ = false;
  Bytes extradata_;
  std::deque<codec::Packet> packets_;
};

}  // namespace

std::unique_ptr<codec::VideoEncoder> make_encoder() { return std::make_unique<Av1Encoder>(); }

}  // namespace cvt::av1
