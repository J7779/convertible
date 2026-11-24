// Convertible core - codec_legacy family: shared decoder/encoder plumbing.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include "codec/codec.hpp"
#include "codec/colorspace.hpp"
#include "engine/errors.hpp"

namespace cvt::legacy {

using codec::CodecParams;
using codec::EncodePreset;
using codec::VideoEncodeParams;
using ir::ColorInfo;
using ir::Packet;
using ir::PixelFormat;
using ir::VideoFrame;

/// Largest picture any decoder of this family accepts (hostile headers).
inline constexpr std::uint32_t kMaxDimension = 16384;
inline constexpr std::uint64_t kMaxPixels = std::uint64_t{8192} * 8192;

/// Status::Limit / Status::Decode for picture sizes taken from a bitstream.
[[nodiscard]] inline Result<void> check_dimensions(std::uint64_t width, std::uint64_t height) {
  if (width == 0 || height == 0) return fail(Status::Decode, "the picture has no pixels");
  if (width > kMaxDimension || height > kMaxDimension || width * height > kMaxPixels) {
    return fail(Status::Limit, "picture size {}x{} exceeds the decoder limit", width, height);
  }
  return {};
}

/// Timestamp in `timescale` ticks -> milliseconds (rounded down, exact for
/// any 64-bit tick value).
[[nodiscard]] constexpr std::int64_t ticks_to_ms(std::int64_t ticks, std::uint32_t timescale) noexcept {
  if (timescale == 0) return ticks;
  const std::int64_t ts = timescale;
  std::int64_t q = ticks / ts;
  std::int64_t r = ticks % ts;
  if (r < 0) {
    r += ts;
    --q;
  }
  return q * 1000 + (r * 1000) / ts;
}
/// Milliseconds -> ticks of `timescale` (rounded to nearest).
[[nodiscard]] constexpr std::int64_t ms_to_ticks(std::int64_t ms, std::uint32_t timescale) noexcept {
  if (timescale == 0) return ms;
  const std::int64_t ts = timescale;
  std::int64_t q = ms / 1000;
  std::int64_t r = ms % 1000;
  if (r < 0) {
    r += 1000;
    --q;
  }
  return q * ts + (r * ts + 500) / 1000;
}

/// Frames in a different layout are converted (codec/colorspace.hpp).
[[nodiscard]] inline Result<VideoFrame> to_format(const VideoFrame& frame, PixelFormat format, const ColorInfo& color) {
  if (!frame.valid()) return fail(Status::InvalidArgument, "the frame is not a valid picture");
  if (frame.format == format && (format == PixelFormat::RGBA8 || frame.color == color)) return frame;
  if (frame.format == format) {
    VideoFrame copy = frame;
    copy.color = color;  // same samples, the caller's interpretation
    return copy;
  }
  return codec::convert_frame(frame, format, color);
}

/// `quality` 1..100 -> a quantiser scale in [best, worst] (geometric, so that
/// each quality step changes the size by a similar ratio). 90 maps to
/// `at90`, which each codec chooses as its "visually transparent" scale.
[[nodiscard]] inline double quality_to_scale(int quality, double best, double at90, double worst) {
  const int q = std::clamp(quality, 1, 100);
  if (q >= 90) {
    const double t = (q - 90) / 10.0;
    return at90 * std::pow(best / at90, t);
  }
  const double t = (90 - q) / 89.0;
  return at90 * std::pow(worst / at90, t);
}

/// Base for decoders that turn each packet into at most one picture in
/// presentation order (intra-only codecs and simple delta codecs).
///
/// An empty packet repeats the previous picture with the new timestamp
/// (AVI "dropped frame" convention).
class SimpleDecoder : public codec::VideoDecoder {
 public:
  Result<void> configure(const CodecParams& params) override {
    params_ = params;
    if (params.width != 0 || params.height != 0) {
      if (params.width > kMaxDimension || params.height > kMaxDimension ||
          std::uint64_t{params.width} * params.height > kMaxPixels) {
        return fail(Status::Limit, "picture size {}x{} exceeds the decoder limit", params.width, params.height);
      }
    }
    return on_configure();
  }
  Result<void> send_packet(const Packet& packet) override {
    VideoFrame frame;
    if (packet.bytes.empty()) {
      if (!last_) return {};
      frame = *last_;
    } else {
      CVT_TRY_ASSIGN(frame, decode(packet));
    }
    frame.pts_ms = ticks_to_ms(packet.pts, params_.timescale);
    frame.duration_ms = packet.duration > 0 ? ticks_to_ms(packet.duration, params_.timescale) : 0;
    if (keep_last()) last_ = frame;
    ready_.push_back(std::move(frame));
    return {};
  }
  Result<std::optional<VideoFrame>> receive_frame() override {
    if (ready_.empty()) return std::optional<VideoFrame>{};
    VideoFrame frame = std::move(ready_.front());
    ready_.pop_front();
    return std::optional<VideoFrame>(std::move(frame));
  }
  Result<void> flush() override { return {}; }

 protected:
  virtual Result<void> on_configure() { return {}; }
  /// Decode one non-empty packet into a complete picture.
  virtual Result<VideoFrame> decode(const Packet& packet) = 0;
  /// Whether empty packets may repeat the last picture.
  virtual bool keep_last() const { return true; }

  CodecParams params_;

 private:
  std::optional<VideoFrame> last_;
  std::deque<VideoFrame> ready_;
};

/// Base for encoders that turn each picture into exactly one packet, in
/// order (intra-only codecs and simple delta codecs).
class SimpleEncoder : public codec::VideoEncoder {
 public:
  Result<void> configure(const VideoEncodeParams& params) override {
    if (params.width == 0 || params.height == 0) return fail(Status::InvalidArgument, "the picture has no pixels");
    if (params.width > kMaxDimension || params.height > kMaxDimension) {
      return fail(Status::InvalidArgument, "picture size {}x{} is too large", params.width, params.height);
    }
    params_ = params;
    if (params_.timescale == 0) params_.timescale = 90000;
    return on_configure();
  }
  Result<void> send_frame(const VideoFrame& frame) override {
    if (frame.width != params_.width || frame.height != params_.height) {
      return fail(Status::InvalidArgument, "frame size {}x{} differs from the configured {}x{}", frame.width,
                  frame.height, params_.width, params_.height);
    }
    bool keyframe = true;
    CVT_TRY_ASSIGN(Bytes data, encode(frame, keyframe));
    Packet packet;
    packet.bytes = std::move(data);
    packet.pts = ms_to_ticks(frame.pts_ms, params_.timescale);
    packet.dts = packet.pts;
    packet.duration = frame.duration_ms > 0 ? ms_to_ticks(frame.duration_ms, params_.timescale) : 0;
    packet.keyframe = keyframe;
    ready_.push_back(std::move(packet));
    ++frame_index_;
    return {};
  }
  Result<std::optional<Packet>> receive_packet() override {
    if (ready_.empty()) return std::optional<Packet>{};
    Packet packet = std::move(ready_.front());
    ready_.pop_front();
    return std::optional<Packet>(std::move(packet));
  }
  Bytes extradata() const override { return {}; }
  Result<void> flush() override { return {}; }

 protected:
  virtual Result<void> on_configure() { return {}; }
  /// Encode one picture; set `keyframe` to false for a delta frame.
  virtual Result<Bytes> encode(const VideoFrame& frame, bool& keyframe) = 0;

  VideoEncodeParams params_;
  std::uint64_t frame_index_ = 0;

 private:
  std::deque<Packet> ready_;
};

}  // namespace cvt::legacy
