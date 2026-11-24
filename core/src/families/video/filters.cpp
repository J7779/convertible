// Convertible core - video family: frame and audio filters of the pipeline.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <numbers>

#include "codec/colorspace.hpp"
#include "families/audio_lossless/audio_dsp.hpp"
#include "families/video/pipeline.hpp"

namespace cvt::video {

namespace {

struct Taps {
  std::vector<std::uint32_t> first;   // first source index per output sample
  std::vector<std::uint32_t> count;   // taps per output sample
  std::vector<float> weights;         // `max_taps` weights per output sample
  std::uint32_t max_taps = 0;
};

double lanczos3(double x) {
  x = std::abs(x);
  if (x < 1e-9) return 1.0;
  if (x >= 3.0) return 0.0;
  const double px = std::numbers::pi * x;
  return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}

Taps make_taps(std::uint32_t src, std::uint32_t dst) {
  Taps taps;
  const double ratio = static_cast<double>(src) / static_cast<double>(dst);
  const double stretch = std::max(1.0, ratio);
  const double support = 3.0 * stretch;
  taps.max_taps = static_cast<std::uint32_t>(std::ceil(support) * 2 + 1);
  taps.first.resize(dst);
  taps.count.resize(dst);
  taps.weights.assign(static_cast<std::size_t>(dst) * taps.max_taps, 0.0f);
  for (std::uint32_t i = 0; i < dst; ++i) {
    const double center = (i + 0.5) * ratio - 0.5;
    const auto lo = static_cast<std::int64_t>(std::floor(center - support)) + 1;
    const auto hi = static_cast<std::int64_t>(std::floor(center + support));
    std::vector<double> w;
    std::vector<std::int64_t> idx;
    double sum = 0.0;
    for (std::int64_t j = lo; j <= hi; ++j) {
      const double value = lanczos3((static_cast<double>(j) - center) / stretch);
      if (value == 0.0) continue;
      w.push_back(value);
      idx.push_back(std::clamp<std::int64_t>(j, 0, static_cast<std::int64_t>(src) - 1));
      sum += value;
    }
    if (w.empty() || sum == 0.0) {
      w = {1.0};
      idx = {std::clamp<std::int64_t>(static_cast<std::int64_t>(std::lround(center)), 0, static_cast<std::int64_t>(src) - 1)};
      sum = 1.0;
    }
    // Clamped indices are contiguous runs; fold them into weights over [min, max].
    const std::int64_t first = *std::ranges::min_element(idx);
    const std::int64_t last = *std::ranges::max_element(idx);
    const auto count = static_cast<std::uint32_t>(last - first + 1);
    taps.first[i] = static_cast<std::uint32_t>(first);
    taps.count[i] = std::min(count, taps.max_taps);
    float* row = &taps.weights[static_cast<std::size_t>(i) * taps.max_taps];
    for (std::size_t k = 0; k < w.size(); ++k) {
      const auto at = static_cast<std::size_t>(idx[k] - first);
      if (at < taps.max_taps) row[at] += static_cast<float>(w[k] / sum);
    }
  }
  return taps;
}

std::uint32_t load(const std::uint8_t* p, bool wide) { return wide ? (p[0] | (static_cast<std::uint32_t>(p[1]) << 8)) : p[0]; }
void store(std::uint8_t* p, bool wide, std::uint32_t v) {
  if (wide) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
  } else {
    p[0] = static_cast<std::uint8_t>(v);
  }
}

// Resize one plane of `channels` interleaved samples of 1 or 2 bytes.
void scale_plane(const ir::Plane& in, std::uint32_t sw, std::uint32_t sh, ir::Plane& out, std::uint32_t dw,
                 std::uint32_t dh, std::uint32_t channels, bool wide, std::uint32_t max_value, std::uint32_t mask) {
  const std::size_t bps = wide ? 2 : 1;
  const Taps hx = make_taps(sw, dw);
  const Taps vy = make_taps(sh, dh);
  std::vector<float> temp(static_cast<std::size_t>(dw) * sh * channels);
  for (std::uint32_t y = 0; y < sh; ++y) {
    const std::uint8_t* row = in.data.data() + static_cast<std::size_t>(y) * in.stride;
    float* t = &temp[static_cast<std::size_t>(y) * dw * channels];
    for (std::uint32_t x = 0; x < dw; ++x) {
      const float* w = &hx.weights[static_cast<std::size_t>(x) * hx.max_taps];
      for (std::uint32_t c = 0; c < channels; ++c) {
        float acc = 0.0f;
        for (std::uint32_t k = 0; k < hx.count[x]; ++k) {
          acc += w[k] * static_cast<float>(load(row + ((hx.first[x] + k) * channels + c) * bps, wide));
        }
        t[x * channels + c] = acc;
      }
    }
  }
  for (std::uint32_t y = 0; y < dh; ++y) {
    std::uint8_t* row = out.data.data() + static_cast<std::size_t>(y) * out.stride;
    const float* w = &vy.weights[static_cast<std::size_t>(y) * vy.max_taps];
    for (std::uint32_t x = 0; x < dw * channels; ++x) {
      float acc = 0.0f;
      for (std::uint32_t k = 0; k < vy.count[y]; ++k) {
        acc += w[k] * temp[static_cast<std::size_t>(vy.first[y] + k) * dw * channels + x];
      }
      const auto v = static_cast<std::uint32_t>(std::clamp(std::lround(acc), 0L, static_cast<long>(max_value)));
      store(row + x * bps, wide, v & mask);
    }
  }
}

std::uint32_t plane_channels(ir::PixelFormat f, std::size_t plane) {
  if (f == ir::PixelFormat::RGBA8) return 4;
  if (plane == 1 && (f == ir::PixelFormat::NV12 || f == ir::PixelFormat::P010)) return 2;
  return 1;
}

}  // namespace

Result<ir::VideoFrame> scale_frame(const ir::VideoFrame& frame, std::uint32_t width, std::uint32_t height) {
  if (!frame.valid()) return fail(Status::InvalidArgument, "cannot scale an invalid frame");
  if (width == 0 || height == 0) return fail(Status::InvalidArgument, "cannot scale to an empty size");
  if (width == frame.width && height == frame.height) return frame;
  ir::VideoFrame out = ir::VideoFrame::allocate(frame.format, width, height);
  out.color = frame.color;
  out.pts_ms = frame.pts_ms;
  out.duration_ms = frame.duration_ms;
  const bool wide = ir::bytes_per_sample(frame.format) == 2;
  const bool p010 = frame.format == ir::PixelFormat::P010;
  const std::uint32_t max_value = !wide ? 255u : p010 ? 65535u : 1023u;
  const std::uint32_t mask = p010 ? 0xFFC0u : 0xFFFFu;
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    scale_plane(frame.planes[p], ir::plane_width(frame.format, p, frame.width), ir::plane_height(frame.format, p, frame.height),
                out.planes[p], ir::plane_width(frame.format, p, width), ir::plane_height(frame.format, p, height),
                plane_channels(frame.format, p), wide, max_value, mask);
  }
  return out;
}

Result<ir::VideoFrame> rotate_frame(const ir::VideoFrame& frame, int degrees) {
  degrees = ((degrees % 360) + 360) % 360;
  if (degrees == 0) return frame;
  if (degrees % 90 != 0) return fail(Status::InvalidArgument, "rotation must be a multiple of 90 degrees");
  ir::VideoFrame source = frame;
  if (frame.format == ir::PixelFormat::I422 || frame.format == ir::PixelFormat::I422P10) {
    // 4:2:2 turned sideways would be 4:4:0; go through 4:4:4 instead.
    const ir::PixelFormat full = frame.format == ir::PixelFormat::I422 ? ir::PixelFormat::I444 : ir::PixelFormat::I444P10;
    CVT_TRY_ASSIGN(source, codec::convert_frame(frame, full, frame.color));
  }
  if (!source.valid()) return fail(Status::InvalidArgument, "cannot rotate an invalid frame");
  const bool swap = degrees != 180;
  const std::uint32_t w = swap ? source.height : source.width;
  const std::uint32_t h = swap ? source.width : source.height;
  ir::VideoFrame out = ir::VideoFrame::allocate(source.format, w, h);
  out.color = source.color;
  out.pts_ms = source.pts_ms;
  out.duration_ms = source.duration_ms;
  const std::size_t bps = ir::bytes_per_sample(source.format);
  for (std::size_t p = 0; p < source.planes.size(); ++p) {
    const std::uint32_t sw = ir::plane_width(source.format, p, source.width);
    const std::uint32_t sh = ir::plane_height(source.format, p, source.height);
    const std::size_t unit = plane_channels(source.format, p) * bps;
    const ir::Plane& in = source.planes[p];
    ir::Plane& o = out.planes[p];
    for (std::uint32_t y = 0; y < sh; ++y) {
      const std::uint8_t* row = in.data.data() + static_cast<std::size_t>(y) * in.stride;
      for (std::uint32_t x = 0; x < sw; ++x) {
        std::uint32_t nx = x;
        std::uint32_t ny = y;
        if (degrees == 90) {
          nx = sh - 1 - y;
          ny = x;
        } else if (degrees == 180) {
          nx = sw - 1 - x;
          ny = sh - 1 - y;
        } else {
          nx = y;
          ny = sw - 1 - x;
        }
        std::copy_n(row + x * unit, unit, o.data.data() + static_cast<std::size_t>(ny) * o.stride + nx * unit);
      }
    }
  }
  return out;
}

// --------------------------------------------------------------------------- audio
//
// Through the seam of pipeline.hpp, the pipeline uses the lossless audio
// family's DSP toolbox (families/audio_lossless/audio_dsp.hpp): the polyphase
// Kaiser resampler (exact output length, no delay) and the BS.775 mixing
// matrices, so that audio extracted from a video and audio converted on its
// own sound the same.

namespace {

class PassThrough final : public AudioResampler {
 public:
  std::vector<float> process(std::span<const float> interleaved, bool) override {
    return std::vector<float>(interleaved.begin(), interleaved.end());
  }
};

class DspResampler final : public AudioResampler {
 public:
  explicit DspResampler(audio::dsp::Resampler resampler) : resampler_(std::move(resampler)) {}
  std::vector<float> process(std::span<const float> interleaved, bool flush) override {
    std::vector<float> out;
    if (!interleaved.empty()) resampler_.process(interleaved, out);
    if (flush) resampler_.flush(out);
    return out;
  }

 private:
  audio::dsp::Resampler resampler_;
};

}  // namespace

std::unique_ptr<AudioResampler> make_resampler(std::uint32_t in_rate, std::uint32_t out_rate, std::uint16_t channels) {
  if (in_rate == out_rate || in_rate == 0 || out_rate == 0) return std::make_unique<PassThrough>();
  audio::dsp::Resampler resampler;
  if (!resampler.configure(in_rate, out_rate, channels == 0 ? 1 : channels, audio::dsp::ResampleQuality::High)) {
    return std::make_unique<PassThrough>();  // absurd rates were refused by the encoder settings already
  }
  return std::make_unique<DspResampler>(std::move(resampler));
}

std::vector<float> remix_channels(std::span<const float> in, std::uint16_t from, std::uint16_t to) {
  if (from == to || from == 0 || to == 0) return std::vector<float>(in.begin(), in.end());
  return audio::dsp::apply_mix(audio::dsp::mix_matrix(from, 0, to, 0), in);
}

}  // namespace cvt::video
