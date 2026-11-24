// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include "support/test.hpp"

#include "codec/colorspace.hpp"

using namespace cvt;
using namespace cvt::ir;
using cvt::codec::convert_frame;
using cvt::codec::from_rgba;
using cvt::codec::to_rgba;

namespace {

constexpr PixelFormat kYuvFormats[] = {PixelFormat::I420,    PixelFormat::I422,    PixelFormat::I444,    PixelFormat::NV12,
                                       PixelFormat::I420P10, PixelFormat::I422P10, PixelFormat::I444P10, PixelFormat::P010};
constexpr ColorMatrix kMatrices[] = {ColorMatrix::BT601, ColorMatrix::BT709, ColorMatrix::BT2020};
constexpr ColorRange kRanges[] = {ColorRange::Limited, ColorRange::Full};
constexpr ChromaSiting kSitings[] = {ChromaSiting::Left, ChromaSiting::Center, ChromaSiting::TopLeft};

struct Rgb {
  int r = 0;
  int g = 0;
  int b = 0;
  friend bool operator==(const Rgb&, const Rgb&) = default;
};

struct Yuv {
  int y = 0;
  int cb = 0;
  int cr = 0;
  friend bool operator==(const Yuv&, const Yuv&) = default;
};

[[nodiscard]] bool semi_planar(PixelFormat format) { return format == PixelFormat::NV12 || format == PixelFormat::P010; }

/// Byte position of sample (x, y) of component 0 = Y, 1 = Cb, 2 = Cr.
[[nodiscard]] std::size_t sample_offset(const VideoFrame& frame, std::size_t component, std::size_t x, std::size_t y,
                                        std::size_t& plane) {
  const std::size_t bytes = bytes_per_sample(frame.format);
  const bool interleaved = component != 0 && semi_planar(frame.format);
  plane = interleaved ? 1 : component;
  const std::size_t step = interleaved ? 2 * bytes : bytes;
  const std::size_t first = (interleaved && component == 2) ? bytes : 0;
  return y * frame.planes[plane].stride + x * step + first;
}

/// The sample value (0..255 or 0..1023) stored at (x, y) of a component.
[[nodiscard]] int sample(const VideoFrame& frame, std::size_t component, std::size_t x, std::size_t y) {
  std::size_t plane = 0;
  const std::size_t at = sample_offset(frame, component, x, y, plane);
  const std::vector<std::uint8_t>& data = frame.planes[plane].data;
  if (bytes_per_sample(frame.format) == 1) return data[at];
  const int word = data[at] | (data[at + 1] << 8);
  return frame.format == PixelFormat::P010 ? word >> 6 : word;
}

void set_sample(VideoFrame& frame, std::size_t component, std::size_t x, std::size_t y, int value) {
  std::size_t plane = 0;
  const std::size_t at = sample_offset(frame, component, x, y, plane);
  std::vector<std::uint8_t>& data = frame.planes[plane].data;
  if (bytes_per_sample(frame.format) == 1) {
    data[at] = static_cast<std::uint8_t>(value);
    return;
  }
  const int word = frame.format == PixelFormat::P010 ? value << 6 : value;
  data[at] = static_cast<std::uint8_t>(word & 0xFF);
  data[at + 1] = static_cast<std::uint8_t>(word >> 8);
}

[[nodiscard]] std::size_t component_width(const VideoFrame& frame, std::size_t component) {
  return plane_width(frame.format, component == 0 ? 0 : 1, frame.width);
}
[[nodiscard]] std::size_t component_height(const VideoFrame& frame, std::size_t component) {
  return plane_height(frame.format, component == 0 ? 0 : 1, frame.height);
}

[[nodiscard]] VideoFrame make_frame(PixelFormat format, std::uint32_t width, std::uint32_t height, ColorInfo color = {}) {
  VideoFrame frame = VideoFrame::allocate(format, width, height);
  frame.color = color;
  return frame;
}

void fill_component(VideoFrame& frame, std::size_t component, int value) {
  for (std::size_t y = 0; y < component_height(frame, component); ++y) {
    for (std::size_t x = 0; x < component_width(frame, component); ++x) set_sample(frame, component, x, y, value);
  }
}

[[nodiscard]] VideoFrame flat_yuv(PixelFormat format, std::uint32_t width, std::uint32_t height, ColorInfo color, Yuv value) {
  VideoFrame frame = make_frame(format, width, height, color);
  fill_component(frame, 0, value.y);
  fill_component(frame, 1, value.cb);
  fill_component(frame, 2, value.cr);
  return frame;
}

void set_pixel(VideoFrame& rgba, std::size_t x, std::size_t y, Rgb value, int alpha = 255) {
  std::uint8_t* p = rgba.planes[0].data.data() + y * rgba.planes[0].stride + x * 4;
  p[0] = static_cast<std::uint8_t>(value.r);
  p[1] = static_cast<std::uint8_t>(value.g);
  p[2] = static_cast<std::uint8_t>(value.b);
  p[3] = static_cast<std::uint8_t>(alpha);
}

[[nodiscard]] Rgb pixel(const VideoFrame& rgba, std::size_t x, std::size_t y) {
  const std::uint8_t* p = rgba.planes[0].data.data() + y * rgba.planes[0].stride + x * 4;
  return {p[0], p[1], p[2]};
}

[[nodiscard]] VideoFrame solid_rgba(std::uint32_t width, std::uint32_t height, Rgb value) {
  VideoFrame frame = make_frame(PixelFormat::RGBA8, width, height);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) set_pixel(frame, x, y, value);
  }
  return frame;
}

/// Deterministic pseudo-random numbers (xorshift32).
struct Random {
  std::uint32_t state;
  [[nodiscard]] int next(int range) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<int>((state >> 8) % static_cast<std::uint32_t>(range));
  }
};

[[nodiscard]] VideoFrame random_rgba(std::uint32_t width, std::uint32_t height, std::uint32_t seed) {
  VideoFrame frame = make_frame(PixelFormat::RGBA8, width, height);
  Random random{seed * 2654435761u + 1u};
  for (std::uint8_t& byte : frame.planes[0].data) byte = static_cast<std::uint8_t>(random.next(256));
  return frame;
}

[[nodiscard]] VideoFrame random_yuv(PixelFormat format, std::uint32_t width, std::uint32_t height, ColorInfo color,
                                    std::uint32_t seed) {
  VideoFrame frame = make_frame(format, width, height, color);
  Random random{seed * 2654435761u + 1u};
  const int range = 1 << bit_depth(format);
  for (std::size_t component = 0; component < 3; ++component) {
    for (std::size_t y = 0; y < component_height(frame, component); ++y) {
      for (std::size_t x = 0; x < component_width(frame, component); ++x) {
        set_sample(frame, component, x, y, random.next(range));
      }
    }
  }
  return frame;
}

/// The same picture with `extra` junk bytes after every row and no slack
/// after the last one, so that any read of padding or past the end shows up
/// as a wrong result or a sanitizer report.
[[nodiscard]] VideoFrame with_padding(const VideoFrame& frame, std::size_t extra) {
  VideoFrame out = frame;
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    const std::size_t row_bytes = plane_row_bytes(frame.format, p, frame.width);
    const std::size_t rows = plane_height(frame.format, p, frame.height);
    Plane& plane = out.planes[p];
    plane.stride = row_bytes + extra;
    plane.data.assign(plane.stride * (rows - 1) + row_bytes, std::uint8_t{0xEE});
    for (std::size_t row = 0; row < rows; ++row) {
      std::copy_n(frame.planes[p].data.begin() + static_cast<std::ptrdiff_t>(row * frame.planes[p].stride), row_bytes,
                  plane.data.begin() + static_cast<std::ptrdiff_t>(row * plane.stride));
    }
  }
  return out;
}

[[nodiscard]] bool same_planes(const VideoFrame& a, const VideoFrame& b) {
  if (a.format != b.format || a.width != b.width || a.height != b.height || a.planes.size() != b.planes.size()) return false;
  for (std::size_t p = 0; p < a.planes.size(); ++p) {
    if (a.planes[p].stride != b.planes[p].stride || a.planes[p].data != b.planes[p].data) return false;
  }
  return true;
}

[[nodiscard]] bool tightly_packed(const VideoFrame& frame) {
  if (frame.planes.size() != plane_count(frame.format)) return false;
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    const std::size_t row_bytes = plane_row_bytes(frame.format, p, frame.width);
    if (frame.planes[p].stride != row_bytes) return false;
    if (frame.planes[p].data.size() != row_bytes * plane_height(frame.format, p, frame.height)) return false;
  }
  return true;
}

/// Largest per-channel R, G, B difference between two RGBA frames, looking
/// only at pixels at least `margin` away from every border.
[[nodiscard]] int max_rgb_difference(const VideoFrame& a, const VideoFrame& b, std::size_t margin = 0) {
  int worst = 0;
  for (std::size_t y = margin; y + margin < a.height; ++y) {
    for (std::size_t x = margin; x + margin < a.width; ++x) {
      const Rgb p = pixel(a, x, y);
      const Rgb q = pixel(b, x, y);
      worst = std::max({worst, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b)});
    }
  }
  return worst;
}

/// Round-trip bound per channel: 8-bit full range +/-1, 8-bit limited range
/// +/-2; 10-bit codes are fine enough to return 8-bit RGB exactly.
[[nodiscard]] int tolerance(PixelFormat format, ColorRange range) {
  if (bit_depth(format) == 10) return 0;
  return range == ColorRange::Full ? 1 : 2;
}

[[nodiscard]] bool is_error(const Result<VideoFrame>& result, Status status) {
  return !result && result.error().code == status;
}

/// to_rgba(from_rgba(rgba)) for one format and colour description.
[[nodiscard]] VideoFrame round_trip(const VideoFrame& rgba, PixelFormat format, ColorInfo color) {
  const auto yuv = from_rgba(rgba, format, color);
  REQUIRE(yuv);
  REQUIRE(yuv->valid());
  const auto back = to_rgba(*yuv);
  REQUIRE(back);
  return *back;
}

struct KnownColor {
  ColorMatrix matrix;
  ColorRange range;
  Rgb rgb;   ///< R'G'B' input
  Yuv yuv;   ///< Y'CbCr codes, rounded from the defining equations
  Rgb back;  ///< R'G'B' those rounded codes convert back to
};

// Y' = Kr R' + Kg G' + Kb B' (R'G'B' in 0..1), Pb = (B' - Y') / (2 (1 - Kb)),
// Pr = (R' - Y') / (2 (1 - Kr)); limited: 16 + 219 Y', 128 + 224 Pb/Pr;
// full: 255 Y', 128 + 255 Pb/Pr. E.g. BT.601 limited red: Y' = 0.299 ->
// 16 + 65.48 = 81; Pb = -0.299 / 1.772 -> 128 - 37.80 = 90; Pr = 0.5 -> 240.
// Full-range yellow and cyan put one chroma value at exactly 0.5, which
// rounds away from neutral (128) to 0; red and blue reach 255.5, stored as 255.
constexpr KnownColor kKnown8[] = {
    {ColorMatrix::BT601, ColorRange::Limited, {0, 0, 0}, {16, 128, 128}, {0, 0, 0}},
    {ColorMatrix::BT601, ColorRange::Limited, {255, 255, 255}, {235, 128, 128}, {255, 255, 255}},
    {ColorMatrix::BT601, ColorRange::Limited, {255, 0, 0}, {81, 90, 240}, {254, 0, 0}},
    {ColorMatrix::BT601, ColorRange::Limited, {0, 255, 0}, {145, 54, 34}, {0, 255, 1}},
    {ColorMatrix::BT601, ColorRange::Limited, {0, 0, 255}, {41, 240, 110}, {0, 0, 255}},
    {ColorMatrix::BT601, ColorRange::Limited, {128, 128, 128}, {126, 128, 128}, {128, 128, 128}},
    {ColorMatrix::BT601, ColorRange::Limited, {255, 255, 0}, {210, 16, 146}, {255, 255, 0}},
    {ColorMatrix::BT601, ColorRange::Limited, {0, 255, 255}, {170, 166, 16}, {1, 255, 255}},
    {ColorMatrix::BT601, ColorRange::Limited, {255, 0, 255}, {106, 202, 222}, {255, 0, 254}},
    {ColorMatrix::BT601, ColorRange::Limited, {250, 130, 20}, {148, 62, 189}, {251, 130, 21}},
    {ColorMatrix::BT601, ColorRange::Full, {0, 0, 0}, {0, 128, 128}, {0, 0, 0}},
    {ColorMatrix::BT601, ColorRange::Full, {255, 255, 255}, {255, 128, 128}, {255, 255, 255}},
    {ColorMatrix::BT601, ColorRange::Full, {255, 0, 0}, {76, 85, 255}, {254, 0, 0}},
    {ColorMatrix::BT601, ColorRange::Full, {0, 255, 0}, {150, 44, 21}, {0, 255, 1}},
    {ColorMatrix::BT601, ColorRange::Full, {0, 0, 255}, {29, 255, 107}, {0, 0, 254}},
    {ColorMatrix::BT601, ColorRange::Full, {128, 128, 128}, {128, 128, 128}, {128, 128, 128}},
    {ColorMatrix::BT601, ColorRange::Full, {255, 255, 0}, {226, 0, 149}, {255, 255, 0}},
    {ColorMatrix::BT601, ColorRange::Full, {0, 255, 255}, {179, 171, 0}, {0, 255, 255}},
    {ColorMatrix::BT601, ColorRange::Full, {255, 0, 255}, {105, 212, 235}, {255, 0, 254}},
    {ColorMatrix::BT601, ColorRange::Full, {250, 130, 20}, {153, 53, 197}, {250, 130, 20}},
    {ColorMatrix::BT709, ColorRange::Limited, {0, 0, 0}, {16, 128, 128}, {0, 0, 0}},
    {ColorMatrix::BT709, ColorRange::Limited, {255, 255, 255}, {235, 128, 128}, {255, 255, 255}},
    {ColorMatrix::BT709, ColorRange::Limited, {255, 0, 0}, {63, 102, 240}, {255, 1, 0}},
    {ColorMatrix::BT709, ColorRange::Limited, {0, 255, 0}, {173, 42, 26}, {0, 255, 1}},
    {ColorMatrix::BT709, ColorRange::Limited, {0, 0, 255}, {32, 240, 118}, {1, 0, 255}},
    {ColorMatrix::BT709, ColorRange::Limited, {128, 128, 128}, {126, 128, 128}, {128, 128, 128}},
    {ColorMatrix::BT709, ColorRange::Limited, {255, 255, 0}, {219, 16, 138}, {254, 255, 0}},
    {ColorMatrix::BT709, ColorRange::Limited, {0, 255, 255}, {188, 154, 16}, {0, 254, 255}},
    {ColorMatrix::BT709, ColorRange::Limited, {255, 0, 255}, {78, 214, 230}, {255, 0, 254}},
    {ColorMatrix::BT709, ColorRange::Limited, {250, 130, 20}, {143, 68, 185}, {250, 130, 21}},
    {ColorMatrix::BT709, ColorRange::Full, {0, 0, 0}, {0, 128, 128}, {0, 0, 0}},
    {ColorMatrix::BT709, ColorRange::Full, {255, 255, 255}, {255, 128, 128}, {255, 255, 255}},
    {ColorMatrix::BT709, ColorRange::Full, {255, 0, 0}, {54, 99, 255}, {254, 0, 0}},
    {ColorMatrix::BT709, ColorRange::Full, {0, 255, 0}, {182, 30, 12}, {0, 255, 0}},
    {ColorMatrix::BT709, ColorRange::Full, {0, 0, 255}, {18, 255, 116}, {0, 0, 254}},
    {ColorMatrix::BT709, ColorRange::Full, {128, 128, 128}, {128, 128, 128}, {128, 128, 128}},
    {ColorMatrix::BT709, ColorRange::Full, {255, 255, 0}, {237, 0, 140}, {255, 255, 0}},
    {ColorMatrix::BT709, ColorRange::Full, {0, 255, 255}, {201, 157, 0}, {0, 255, 255}},
    {ColorMatrix::BT709, ColorRange::Full, {255, 0, 255}, {73, 226, 244}, {255, 0, 255}},
    {ColorMatrix::BT709, ColorRange::Full, {250, 130, 20}, {148, 59, 193}, {250, 130, 20}},
    {ColorMatrix::BT2020, ColorRange::Limited, {0, 0, 0}, {16, 128, 128}, {0, 0, 0}},
    {ColorMatrix::BT2020, ColorRange::Limited, {255, 255, 255}, {235, 128, 128}, {255, 255, 255}},
    {ColorMatrix::BT2020, ColorRange::Limited, {255, 0, 0}, {74, 97, 240}, {255, 0, 1}},
    {ColorMatrix::BT2020, ColorRange::Limited, {0, 255, 0}, {164, 47, 25}, {0, 254, 0}},
    {ColorMatrix::BT2020, ColorRange::Limited, {0, 0, 255}, {29, 240, 119}, {0, 0, 255}},
    {ColorMatrix::BT2020, ColorRange::Limited, {128, 128, 128}, {126, 128, 128}, {128, 128, 128}},
    {ColorMatrix::BT2020, ColorRange::Limited, {255, 255, 0}, {222, 16, 137}, {255, 255, 0}},
    {ColorMatrix::BT2020, ColorRange::Limited, {0, 255, 255}, {177, 159, 16}, {0, 255, 254}},
    {ColorMatrix::BT2020, ColorRange::Limited, {255, 0, 255}, {87, 209, 231}, {255, 1, 255}},
    {ColorMatrix::BT2020, ColorRange::Limited, {250, 130, 20}, {149, 65, 185}, {251, 130, 20}},
    {ColorMatrix::BT2020, ColorRange::Full, {0, 0, 0}, {0, 128, 128}, {0, 0, 0}},
    {ColorMatrix::BT2020, ColorRange::Full, {255, 255, 255}, {255, 128, 128}, {255, 255, 255}},
    {ColorMatrix::BT2020, ColorRange::Full, {255, 0, 0}, {67, 92, 255}, {254, 0, 0}},
    {ColorMatrix::BT2020, ColorRange::Full, {0, 255, 0}, {173, 36, 11}, {0, 255, 0}},
    {ColorMatrix::BT2020, ColorRange::Full, {0, 0, 255}, {15, 255, 118}, {0, 0, 254}},
    {ColorMatrix::BT2020, ColorRange::Full, {128, 128, 128}, {128, 128, 128}, {128, 128, 128}},
    {ColorMatrix::BT2020, ColorRange::Full, {255, 255, 0}, {240, 0, 138}, {255, 255, 0}},
    {ColorMatrix::BT2020, ColorRange::Full, {0, 255, 255}, {188, 164, 0}, {0, 255, 255}},
    {ColorMatrix::BT2020, ColorRange::Full, {255, 0, 255}, {82, 220, 245}, {255, 0, 255}},
    {ColorMatrix::BT2020, ColorRange::Full, {250, 130, 20}, {155, 56, 192}, {249, 130, 20}},
};

// The same equations at 10 bits: limited 64 + 876 Y', 512 + 896 Pb/Pr; full
// 1023 Y', 512 + 1023 Pb/Pr. Every one of these converts back to the exact
// 8-bit colour.
struct KnownColor10 {
  ColorMatrix matrix;
  ColorRange range;
  Rgb rgb;
  Yuv yuv;
};

constexpr KnownColor10 kKnown10[] = {
    {ColorMatrix::BT601, ColorRange::Limited, {0, 0, 0}, {64, 512, 512}},
    {ColorMatrix::BT601, ColorRange::Limited, {255, 255, 255}, {940, 512, 512}},
    {ColorMatrix::BT601, ColorRange::Limited, {255, 0, 0}, {326, 361, 960}},
    {ColorMatrix::BT601, ColorRange::Limited, {0, 255, 0}, {578, 215, 137}},
    {ColorMatrix::BT601, ColorRange::Limited, {0, 0, 255}, {164, 960, 439}},
    {ColorMatrix::BT601, ColorRange::Limited, {128, 128, 128}, {504, 512, 512}},
    {ColorMatrix::BT601, ColorRange::Limited, {255, 255, 0}, {840, 64, 585}},
    {ColorMatrix::BT601, ColorRange::Limited, {250, 130, 20}, {591, 248, 754}},
    {ColorMatrix::BT601, ColorRange::Full, {0, 0, 0}, {0, 512, 512}},
    {ColorMatrix::BT601, ColorRange::Full, {255, 255, 255}, {1023, 512, 512}},
    {ColorMatrix::BT601, ColorRange::Full, {255, 0, 0}, {306, 339, 1023}},
    {ColorMatrix::BT601, ColorRange::Full, {0, 255, 0}, {601, 173, 84}},
    {ColorMatrix::BT601, ColorRange::Full, {0, 0, 255}, {117, 1023, 429}},
    {ColorMatrix::BT601, ColorRange::Full, {128, 128, 128}, {514, 512, 512}},
    {ColorMatrix::BT601, ColorRange::Full, {255, 255, 0}, {906, 0, 595}},
    {ColorMatrix::BT601, ColorRange::Full, {250, 130, 20}, {615, 210, 789}},
    {ColorMatrix::BT709, ColorRange::Limited, {0, 0, 0}, {64, 512, 512}},
    {ColorMatrix::BT709, ColorRange::Limited, {255, 255, 255}, {940, 512, 512}},
    {ColorMatrix::BT709, ColorRange::Limited, {255, 0, 0}, {250, 409, 960}},
    {ColorMatrix::BT709, ColorRange::Limited, {0, 255, 0}, {691, 167, 105}},
    {ColorMatrix::BT709, ColorRange::Limited, {0, 0, 255}, {127, 960, 471}},
    {ColorMatrix::BT709, ColorRange::Limited, {128, 128, 128}, {504, 512, 512}},
    {ColorMatrix::BT709, ColorRange::Limited, {255, 255, 0}, {877, 64, 553}},
    {ColorMatrix::BT709, ColorRange::Limited, {250, 130, 20}, {571, 270, 741}},
    {ColorMatrix::BT709, ColorRange::Full, {0, 0, 0}, {0, 512, 512}},
    {ColorMatrix::BT709, ColorRange::Full, {255, 255, 255}, {1023, 512, 512}},
    {ColorMatrix::BT709, ColorRange::Full, {255, 0, 0}, {217, 395, 1023}},
    {ColorMatrix::BT709, ColorRange::Full, {0, 255, 0}, {732, 118, 47}},
    {ColorMatrix::BT709, ColorRange::Full, {0, 0, 255}, {74, 1023, 465}},
    {ColorMatrix::BT709, ColorRange::Full, {128, 128, 128}, {514, 512, 512}},
    {ColorMatrix::BT709, ColorRange::Full, {255, 255, 0}, {949, 0, 559}},
    {ColorMatrix::BT709, ColorRange::Full, {250, 130, 20}, {592, 236, 773}},
    {ColorMatrix::BT2020, ColorRange::Limited, {0, 0, 0}, {64, 512, 512}},
    {ColorMatrix::BT2020, ColorRange::Limited, {255, 255, 255}, {940, 512, 512}},
    {ColorMatrix::BT2020, ColorRange::Limited, {255, 0, 0}, {294, 387, 960}},
    {ColorMatrix::BT2020, ColorRange::Limited, {0, 255, 0}, {658, 189, 100}},
    {ColorMatrix::BT2020, ColorRange::Limited, {0, 0, 255}, {116, 960, 476}},
    {ColorMatrix::BT2020, ColorRange::Limited, {128, 128, 128}, {504, 512, 512}},
    {ColorMatrix::BT2020, ColorRange::Limited, {255, 255, 0}, {888, 64, 548}},
    {ColorMatrix::BT2020, ColorRange::Limited, {250, 130, 20}, {596, 260, 738}},
    {ColorMatrix::BT2020, ColorRange::Full, {0, 0, 0}, {0, 512, 512}},
    {ColorMatrix::BT2020, ColorRange::Full, {255, 255, 255}, {1023, 512, 512}},
    {ColorMatrix::BT2020, ColorRange::Full, {255, 0, 0}, {269, 369, 1023}},
    {ColorMatrix::BT2020, ColorRange::Full, {0, 255, 0}, {694, 143, 42}},
    {ColorMatrix::BT2020, ColorRange::Full, {0, 0, 255}, {61, 1023, 471}},
    {ColorMatrix::BT2020, ColorRange::Full, {128, 128, 128}, {514, 512, 512}},
    {ColorMatrix::BT2020, ColorRange::Full, {255, 255, 0}, {962, 0, 553}},
    {ColorMatrix::BT2020, ColorRange::Full, {250, 130, 20}, {622, 224, 770}},
};

/// True when every sample of each component equals `expected`.
[[nodiscard]] bool all_samples_are(const VideoFrame& frame, Yuv expected) {
  const int values[3] = {expected.y, expected.cb, expected.cr};
  for (std::size_t component = 0; component < 3; ++component) {
    for (std::size_t y = 0; y < component_height(frame, component); ++y) {
      for (std::size_t x = 0; x < component_width(frame, component); ++x) {
        if (sample(frame, component, x, y) != values[component]) return false;
      }
    }
  }
  return true;
}

[[nodiscard]] bool all_pixels_are(const VideoFrame& rgba, Rgb expected) {
  for (std::size_t y = 0; y < rgba.height; ++y) {
    for (std::size_t x = 0; x < rgba.width; ++x) {
      if (!(pixel(rgba, x, y) == expected)) return false;
      if (rgba.planes[0].data[y * rgba.planes[0].stride + x * 4 + 3] != 255) return false;
    }
  }
  return true;
}

/// One row of R (or G, B) values of an RGBA frame.
[[nodiscard]] std::vector<int> channel_row(const VideoFrame& rgba, std::size_t y, int channel) {
  std::vector<int> out;
  for (std::size_t x = 0; x < rgba.width; ++x) {
    const Rgb p = pixel(rgba, x, y);
    out.push_back(channel == 0 ? p.r : channel == 1 ? p.g : p.b);
  }
  return out;
}

[[nodiscard]] std::vector<int> channel_column(const VideoFrame& rgba, std::size_t x, int channel) {
  std::vector<int> out;
  for (std::size_t y = 0; y < rgba.height; ++y) {
    const Rgb p = pixel(rgba, x, y);
    out.push_back(channel == 0 ? p.r : channel == 1 ? p.g : p.b);
  }
  return out;
}

/// All samples of one component, row by row.
[[nodiscard]] std::vector<int> component_values(const VideoFrame& frame, std::size_t component) {
  std::vector<int> out;
  for (std::size_t y = 0; y < component_height(frame, component); ++y) {
    for (std::size_t x = 0; x < component_width(frame, component); ++x) out.push_back(sample(frame, component, x, y));
  }
  return out;
}

using Values = std::vector<int>;

}  // namespace

TEST_CASE("colorspace: known 8-bit codes for every matrix and range, both directions") {
  for (const KnownColor& known : kKnown8) {
    INFO("matrix ", static_cast<int>(known.matrix), " range ", static_cast<int>(known.range), " rgb ", known.rgb.r, ",",
         known.rgb.g, ",", known.rgb.b);
    const ColorInfo color{known.matrix, known.range, ChromaSiting::Left};
    const VideoFrame rgba = solid_rgba(4, 4, known.rgb);
    for (const PixelFormat format : {PixelFormat::I444, PixelFormat::I422, PixelFormat::I420, PixelFormat::NV12}) {
      const auto yuv = from_rgba(rgba, format, color);
      REQUIRE(yuv);
      CHECK(all_samples_are(*yuv, known.yuv));
      const auto back = to_rgba(flat_yuv(format, 4, 4, color, known.yuv));
      REQUIRE(back);
      CHECK(all_pixels_are(*back, known.back));
    }
    // The table itself honours the round-trip bounds.
    const int bound = known.range == ColorRange::Full ? 1 : 2;
    CHECK(std::abs(known.back.r - known.rgb.r) <= bound);
    CHECK(std::abs(known.back.g - known.rgb.g) <= bound);
    CHECK(std::abs(known.back.b - known.rgb.b) <= bound);
  }
}

TEST_CASE("colorspace: known 10-bit codes and their storage") {
  for (const KnownColor10& known : kKnown10) {
    INFO("matrix ", static_cast<int>(known.matrix), " range ", static_cast<int>(known.range), " rgb ", known.rgb.r, ",",
         known.rgb.g, ",", known.rgb.b);
    const ColorInfo color{known.matrix, known.range, ChromaSiting::Left};
    const VideoFrame rgba = solid_rgba(4, 2, known.rgb);
    for (const PixelFormat format : {PixelFormat::I444P10, PixelFormat::I422P10, PixelFormat::I420P10, PixelFormat::P010}) {
      const auto yuv = from_rgba(rgba, format, color);
      REQUIRE(yuv);
      CHECK(all_samples_are(*yuv, known.yuv));
      const auto back = to_rgba(flat_yuv(format, 4, 2, color, known.yuv));
      REQUIRE(back);
      CHECK(all_pixels_are(*back, known.rgb));
    }
  }

  // BT.709 limited white is (940, 512, 512). The planar formats store the
  // value itself, P010 stores value << 6, both as little-endian words.
  const ColorInfo bt709{ColorMatrix::BT709, ColorRange::Limited, ChromaSiting::Left};
  const VideoFrame white = solid_rgba(2, 2, {255, 255, 255});
  const auto planar = from_rgba(white, PixelFormat::I420P10, bt709);
  REQUIRE(planar);
  using ByteVector = std::vector<std::uint8_t>;
  CHECK(planar->planes[0].data == ByteVector{0xAC, 0x03, 0xAC, 0x03, 0xAC, 0x03, 0xAC, 0x03});  // 940 = 0x03AC
  CHECK(planar->planes[1].data == ByteVector{0x00, 0x02});                                      // 512 = 0x0200
  CHECK(planar->planes[2].data == ByteVector{0x00, 0x02});
  const auto p010 = from_rgba(white, PixelFormat::P010, bt709);
  REQUIRE(p010);
  CHECK(p010->planes[0].data == ByteVector{0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB});  // 940 << 6 = 0xEB00
  CHECK(p010->planes[1].data == ByteVector{0x00, 0x80, 0x00, 0x80});                          // 512 << 6 = 0x8000

  const auto black = from_rgba(solid_rgba(2, 2, {0, 0, 0}), PixelFormat::P010, bt709);
  REQUIRE(black);
  CHECK(black->planes[0].data == ByteVector{0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10});  // 64 << 6 = 0x1000
  CHECK(black->planes[1].data == ByteVector{0x00, 0x80, 0x00, 0x80});

  // NV12 and P010 interleave Cb then Cr. BT.2020 limited red is (294, 387, 960)
  // at 10 bits and (74, 97, 240) at 8 bits.
  const ColorInfo bt2020{ColorMatrix::BT2020, ColorRange::Limited, ChromaSiting::TopLeft};
  const VideoFrame red = solid_rgba(2, 2, {255, 0, 0});
  const auto red10 = from_rgba(red, PixelFormat::P010, bt2020);
  REQUIRE(red10);
  CHECK(red10->planes[1].data == ByteVector{0xC0, 0x60, 0x00, 0xF0});  // 387 << 6 = 0x60C0, 960 << 6 = 0xF000
  const auto red8 = from_rgba(red, PixelFormat::NV12, bt2020);
  REQUIRE(red8);
  CHECK(red8->planes[0].data == ByteVector{74, 74, 74, 74});
  CHECK(red8->planes[1].data == ByteVector{97, 240});
}

TEST_CASE("colorspace: malformed 10-bit words are read as documented") {
  const ColorInfo color{ColorMatrix::BT709, ColorRange::Limited, ChromaSiting::Left};
  // The six low bits of P010 words are padding and must not change the result.
  VideoFrame clean = flat_yuv(PixelFormat::P010, 4, 4, color, {700, 400, 600});
  VideoFrame dirty = clean;
  for (Plane& plane : dirty.planes) {
    for (std::size_t i = 0; i < plane.data.size(); i += 2) plane.data[i] |= 0x3F;
  }
  const auto a = to_rgba(clean);
  const auto b = to_rgba(dirty);
  REQUIRE(a);
  REQUIRE(b);
  CHECK(same_planes(*a, *b));

  // Planar 10-bit values above 1023 saturate instead of wrapping.
  VideoFrame peak = flat_yuv(PixelFormat::I444P10, 2, 2, color, {1023, 512, 512});
  VideoFrame beyond = peak;
  for (std::size_t i = 0; i < beyond.planes[0].data.size(); i += 2) {
    beyond.planes[0].data[i] = 0x00;  // 0x0400 = 1024: one past the peak, not zero
    beyond.planes[0].data[i + 1] = 0x04;
  }
  const auto c = to_rgba(peak);
  const auto d = to_rgba(beyond);
  REQUIRE(c);
  REQUIRE(d);
  CHECK(same_planes(*c, *d));
  CHECK(all_pixels_are(*d, {255, 255, 255}));
}

TEST_CASE("colorspace: every format round-trips constant colours at every size") {
  constexpr Rgb colors[] = {{0, 0, 0},     {255, 255, 255}, {255, 0, 0},   {0, 255, 0},    {0, 0, 255},  {255, 255, 0},
                            {0, 255, 255}, {255, 0, 255},   {128, 128, 128}, {17, 99, 201}, {250, 130, 20}, {1, 2, 3}};
  constexpr std::uint32_t sizes[][2] = {{1, 1}, {2, 2}, {3, 5}, {17, 9}};
  for (const PixelFormat format : kYuvFormats) {
    for (const ColorMatrix matrix : kMatrices) {
      for (const ColorRange range : kRanges) {
        for (const ChromaSiting siting : kSitings) {
          const ColorInfo color{matrix, range, siting};
          int worst = 0;
          int worst_grey = 0;
          for (const auto& size : sizes) {
            for (const Rgb& value : colors) {
              const VideoFrame rgba = solid_rgba(size[0], size[1], value);
              const int difference = max_rgb_difference(round_trip(rgba, format, color), rgba);
              worst = std::max(worst, difference);
              if (value.r == value.g && value.g == value.b) worst_grey = std::max(worst_grey, difference);
            }
          }
          INFO("format ", static_cast<int>(format), " matrix ", static_cast<int>(matrix), " range ",
               static_cast<int>(range), " siting ", static_cast<int>(siting));
          CHECK(worst <= tolerance(format, range));
          if (range == ColorRange::Full) {
            CHECK(worst_grey == 0);
          }
        }
      }
    }
  }
}

TEST_CASE("colorspace: output planes have the documented sizes for odd dimensions") {
  constexpr std::uint32_t sizes[][2] = {{1, 1}, {2, 2}, {3, 5}, {17, 9}};
  for (const PixelFormat format : kYuvFormats) {
    for (const auto& size : sizes) {
      const std::uint32_t w = size[0];
      const std::uint32_t h = size[1];
      const auto yuv = from_rgba(random_rgba(w, h, w * 31 + h), format);
      REQUIRE(yuv);
      CHECK(yuv->width == w);
      CHECK(yuv->height == h);
      CHECK(yuv->format == format);
      CHECK(yuv->valid());
      CHECK(tightly_packed(*yuv));
      const std::size_t cw = chroma_shift_x(format) == 1 ? (w + 1) / 2 : w;
      const std::size_t ch = chroma_shift_y(format) == 1 ? (h + 1) / 2 : h;
      const std::size_t bytes = bytes_per_sample(format);
      CHECK(yuv->planes[0].data.size() == std::size_t{w} * h * bytes);
      if (semi_planar(format)) {
        CHECK(yuv->planes[1].data.size() == cw * ch * 2 * bytes);
      } else {
        CHECK(yuv->planes[1].data.size() == cw * ch * bytes);
        CHECK(yuv->planes[2].data.size() == cw * ch * bytes);
      }
      const auto back = to_rgba(*yuv);
      REQUIRE(back);
      CHECK(back->format == PixelFormat::RGBA8);
      CHECK(tightly_packed(*back));
      CHECK(back->planes[0].data.size() == std::size_t{w} * h * 4);
    }
  }
}

TEST_CASE("colorspace: 4:4:4 round-trips arbitrary pixels") {
  for (const PixelFormat format : {PixelFormat::I444, PixelFormat::I444P10}) {
    for (const ColorMatrix matrix : kMatrices) {
      for (const ColorRange range : kRanges) {
        const ColorInfo color{matrix, range, ChromaSiting::Center};
        INFO("format ", static_cast<int>(format), " matrix ", static_cast<int>(matrix), " range ", static_cast<int>(range));
        const VideoFrame noise = random_rgba(61, 37, 7);
        CHECK(max_rgb_difference(round_trip(noise, format, color), noise) <= tolerance(format, range));
      }
    }
  }
}

TEST_CASE("colorspace: every format round-trips smooth gradients") {
  constexpr std::uint32_t w = 21;
  constexpr std::uint32_t h = 14;
  // Grey ramp: only luma varies, so subsampling loses nothing.
  VideoFrame grey = make_frame(PixelFormat::RGBA8, w, h);
  // Colour ramp, linear in x and y with at most one code per pixel.
  VideoFrame ramp = make_frame(PixelFormat::RGBA8, w, h);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const int level = static_cast<int>(x * 11 + y * 3);
      set_pixel(grey, x, y, {level, level, level});
      set_pixel(ramp, x, y, {static_cast<int>(20 + x), static_cast<int>(60 + y), static_cast<int>(200 - x - y)});
    }
  }
  for (const PixelFormat format : kYuvFormats) {
    for (const ColorMatrix matrix : kMatrices) {
      for (const ColorRange range : kRanges) {
        for (const ChromaSiting siting : kSitings) {
          const ColorInfo color{matrix, range, siting};
          INFO("format ", static_cast<int>(format), " matrix ", static_cast<int>(matrix), " range ",
               static_cast<int>(range), " siting ", static_cast<int>(siting));
          const int bound = tolerance(format, range);
          // Greys are exact in full range and within one code in limited range
          // (only the luma quantisation is in play).
          const int grey_bound = range == ColorRange::Full ? 0 : std::min(bound, 1);
          CHECK(max_rgb_difference(round_trip(grey, format, color), grey) <= grey_bound);
          const VideoFrame back = round_trip(ramp, format, color);
          // A linear ramp survives the downsampling filter and the bilinear
          // reconstruction unchanged wherever no edge replication is involved.
          CHECK(max_rgb_difference(back, ramp, 2) <= bound);
          // At the borders replication stands in for chroma up to one luma
          // pixel away in each direction. For this ramp that is at most 1.5
          // codes of Cb (0.67 per column plus 0.83 per row), i.e. less than
          // three codes of blue.
          CHECK(max_rgb_difference(back, ramp) <= bound + 3);
        }
      }
    }
  }
}

TEST_CASE("colorspace: stride padding is ignored and never read past") {
  constexpr std::uint32_t sizes[][2] = {{1, 1}, {2, 2}, {3, 5}, {17, 9}};
  const ColorInfo other{ColorMatrix::BT601, ColorRange::Full, ChromaSiting::Center};
  for (const auto& size : sizes) {
    const std::uint32_t w = size[0];
    const std::uint32_t h = size[1];
    const VideoFrame rgba = random_rgba(w, h, 3);
    const VideoFrame rgba_padded = with_padding(rgba, 7);
    REQUIRE(rgba_padded.valid());
    for (const PixelFormat format : kYuvFormats) {
      INFO("format ", static_cast<int>(format), " size ", w, "x", h);
      const auto from_tight = from_rgba(rgba, format);
      const auto from_padded = from_rgba(rgba_padded, format);
      REQUIRE(from_tight);
      REQUIRE(from_padded);
      CHECK(same_planes(*from_tight, *from_padded));

      const VideoFrame yuv = random_yuv(format, w, h, {}, 11);
      const VideoFrame yuv_padded = with_padding(yuv, 2 * bytes_per_sample(format) + 1);
      REQUIRE(yuv_padded.valid());
      const auto to_tight = to_rgba(yuv);
      const auto to_padded = to_rgba(yuv_padded);
      REQUIRE(to_tight);
      REQUIRE(to_padded);
      CHECK(same_planes(*to_tight, *to_padded));

      for (const PixelFormat target : kYuvFormats) {
        for (const ColorInfo& color : {ColorInfo{}, other}) {
          const auto converted_tight = convert_frame(yuv, target, color);
          const auto converted_padded = convert_frame(yuv_padded, target, color);
          REQUIRE(converted_tight);
          REQUIRE(converted_padded);
          CHECK(tightly_packed(*converted_padded));
          CHECK(same_planes(*converted_tight, *converted_padded));
        }
      }
    }
    // RGBA in, RGBA out: the copy drops the padding and keeps alpha.
    const auto copy = to_rgba(rgba_padded);
    REQUIRE(copy);
    CHECK(same_planes(*copy, rgba));
    const auto copy2 = from_rgba(rgba_padded, PixelFormat::RGBA8);
    REQUIRE(copy2);
    CHECK(same_planes(*copy2, rgba));
  }
}

TEST_CASE("colorspace: horizontal chroma siting places a step edge half a luma pixel apart") {
  // BT.601 full range, Y = 128, Cb = 128: R = 128 + 1.402 (Cr - 128) and
  // G = 128 - 0.714136 (Cr - 128), so
  //   Cr  64 -> R  38, G 174      Cr  96 -> R  83, G 151     Cr 128 -> R 128, G 128
  //   Cr 160 -> R 173, G 105      Cr 192 -> R 218, G  82
  // Chroma row 64 64 | 192 192 covers eight luma columns.
  //   Left   (sample i on column 2i):        64 64 64 128 192 192 192 192
  //   Center (sample i on column 2i + 0.5):  64 64 64  96 160 192 192 192
  // The edge crosses Cr = 128 at column 3 for Left and 3.5 for Center.
  const Values left_r = {38, 38, 38, 128, 218, 218, 218, 218};
  const Values left_g = {174, 174, 174, 128, 82, 82, 82, 82};
  const Values center_r = {38, 38, 38, 83, 173, 218, 218, 218};
  const Values center_g = {174, 174, 174, 151, 105, 82, 82, 82};
  const Values flat_b(8, 128);
  for (const PixelFormat format : {PixelFormat::I420, PixelFormat::NV12, PixelFormat::I422}) {
    for (const ChromaSiting siting : kSitings) {
      INFO("format ", static_cast<int>(format), " siting ", static_cast<int>(siting));
      VideoFrame frame = flat_yuv(format, 8, 2, {ColorMatrix::BT601, ColorRange::Full, siting}, {128, 128, 64});
      for (std::size_t row = 0; row < component_height(frame, 2); ++row) {
        set_sample(frame, 2, 2, row, 192);
        set_sample(frame, 2, 3, row, 192);
      }
      const auto rgba = to_rgba(frame);
      REQUIRE(rgba);
      const bool centred = siting == ChromaSiting::Center;
      for (std::size_t y = 0; y < 2; ++y) {
        CHECK(channel_row(*rgba, y, 0) == (centred ? center_r : left_r));
        CHECK(channel_row(*rgba, y, 1) == (centred ? center_g : left_g));
        CHECK(channel_row(*rgba, y, 2) == flat_b);
      }
    }
  }

  // The same edge at 10 bits, BT.709 limited, Y = Cb = 512 (R'G'B' 130.3 at
  // neutral chroma): Cr 256 -> R 16, 384 -> 73, 512 -> 130, 640 -> 188, 768 -> 245.
  for (const PixelFormat format : {PixelFormat::I420P10, PixelFormat::P010, PixelFormat::I422P10}) {
    for (const ChromaSiting siting : kSitings) {
      INFO("format ", static_cast<int>(format), " siting ", static_cast<int>(siting));
      VideoFrame frame = flat_yuv(format, 8, 2, {ColorMatrix::BT709, ColorRange::Limited, siting}, {512, 512, 256});
      for (std::size_t row = 0; row < component_height(frame, 2); ++row) {
        set_sample(frame, 2, 2, row, 768);
        set_sample(frame, 2, 3, row, 768);
      }
      const auto rgba = to_rgba(frame);
      REQUIRE(rgba);
      const Values expected = siting == ChromaSiting::Center ? Values{16, 16, 16, 73, 188, 245, 245, 245}
                                                             : Values{16, 16, 16, 130, 245, 245, 245, 245};
      CHECK(channel_row(*rgba, 0, 0) == expected);
      CHECK(channel_row(*rgba, 1, 0) == expected);
    }
  }

  // Odd width: seven luma columns over chroma 64 64 64 | 192; the last column
  // has no chroma sample to its right and replicates.
  //   Left:   64 64 64 64 64 128 192        Center: 64 64 64 64 64 96 160
  for (const ChromaSiting siting : kSitings) {
    VideoFrame frame = flat_yuv(PixelFormat::I420, 7, 1, {ColorMatrix::BT601, ColorRange::Full, siting}, {128, 128, 64});
    set_sample(frame, 2, 3, 0, 192);
    const auto rgba = to_rgba(frame);
    REQUIRE(rgba);
    const Values expected = siting == ChromaSiting::Center ? Values{38, 38, 38, 38, 38, 83, 173}
                                                           : Values{38, 38, 38, 38, 38, 128, 218};
    CHECK(channel_row(*rgba, 0, 0) == expected);
  }
}

TEST_CASE("colorspace: vertical chroma siting") {
  // Chroma rows 64 | 192 under four luma rows, same colour maths as above.
  //   Left, Center (row j at luma row 2j + 0.5):  64  96 160 192 -> R 38  83 173 218
  //   TopLeft      (row j on luma row 2j):        64 128 192 192 -> R 38 128 218 218
  for (const PixelFormat format : {PixelFormat::I420, PixelFormat::NV12}) {
    for (const ChromaSiting siting : kSitings) {
      INFO("format ", static_cast<int>(format), " siting ", static_cast<int>(siting));
      VideoFrame frame = flat_yuv(format, 2, 4, {ColorMatrix::BT601, ColorRange::Full, siting}, {128, 128, 64});
      set_sample(frame, 2, 0, 1, 192);
      const auto rgba = to_rgba(frame);
      REQUIRE(rgba);
      const Values expected = siting == ChromaSiting::TopLeft ? Values{38, 128, 218, 218} : Values{38, 83, 173, 218};
      CHECK(channel_column(*rgba, 0, 0) == expected);
      CHECK(channel_column(*rgba, 1, 0) == expected);
    }
  }
  // 4:2:2 has a chroma row per luma row: no vertical interpolation at all.
  for (const ChromaSiting siting : kSitings) {
    VideoFrame frame = flat_yuv(PixelFormat::I422, 2, 4, {ColorMatrix::BT601, ColorRange::Full, siting}, {128, 128, 64});
    set_sample(frame, 2, 0, 2, 192);
    set_sample(frame, 2, 0, 3, 192);
    const auto rgba = to_rgba(frame);
    REQUIRE(rgba);
    CHECK(channel_column(*rgba, 0, 0) == Values{38, 38, 218, 218});
  }
  // Odd height: three luma rows over chroma rows 64 | 192.
  //   Left/Center: 64 96 160      TopLeft: 64 128 192
  for (const ChromaSiting siting : kSitings) {
    VideoFrame frame = flat_yuv(PixelFormat::I420, 1, 3, {ColorMatrix::BT601, ColorRange::Full, siting}, {128, 128, 64});
    set_sample(frame, 2, 0, 1, 192);
    const auto rgba = to_rgba(frame);
    REQUIRE(rgba);
    CHECK(channel_column(*rgba, 0, 0) == (siting == ChromaSiting::TopLeft ? Values{38, 128, 218} : Values{38, 83, 173}));
  }
}

TEST_CASE("colorspace: chroma downsampling uses the kernel of the siting") {
  // BT.601 full range, full-resolution chroma (Cb, Cr) of the four pixels:
  //   red   ( 84.97, 255.50)   green  (43.53,  21.23)
  //   blue  (255.50, 107.27)   yellow ( 0.50, 148.73)
  VideoFrame block = make_frame(PixelFormat::RGBA8, 2, 2);
  set_pixel(block, 0, 0, {255, 0, 0});
  set_pixel(block, 1, 0, {0, 255, 0});
  set_pixel(block, 0, 1, {0, 0, 255});
  set_pixel(block, 1, 1, {255, 255, 0});
  const auto chroma = [&](PixelFormat format, ChromaSiting siting) {
    const auto yuv = from_rgba(block, format, {ColorMatrix::BT601, ColorRange::Full, siting});
    REQUIRE(yuv);
    // Luma is never filtered: 76.245, 149.685, 29.07, 225.93.
    CHECK(component_values(*yuv, 0) == Values{76, 150, 29, 226});
    Values out = component_values(*yuv, 1);
    const Values cr = component_values(*yuv, 2);
    out.insert(out.end(), cr.begin(), cr.end());
    return out;
  };
  for (const PixelFormat format : {PixelFormat::I420, PixelFormat::NV12}) {
    INFO("format ", static_cast<int>(format));
    // Center: the mean of the four -> Cb 96.125, Cr 133.18.
    CHECK(chroma(format, ChromaSiting::Center) == Values{96, 133});
    // Left: [1 2 1]/4 across with the missing left neighbour replicated
    // (3/4, 1/4), box down: weights 3/8 1/8 / 3/8 1/8 -> Cb 133.18, Cr 157.28.
    CHECK(chroma(format, ChromaSiting::Left) == Values{133, 157});
    // TopLeft: (3/4, 1/4) in both directions: weights 9/16 3/16 / 3/16 1/16,
    // the top-left pixel dominating -> Cb 103.90, Cr 177.11.
    CHECK(chroma(format, ChromaSiting::TopLeft) == Values{104, 177});
  }
  // 4:2:2 filters each row on its own.
  //   Center: row means -> Cb 64.25, 128.00; Cr 138.37, 128.00
  //   Left/TopLeft: (3/4, 1/4) -> Cb 74.61, 191.75; Cr 196.93, 117.63
  CHECK(chroma(PixelFormat::I422, ChromaSiting::Center) == Values{64, 128, 138, 128});
  CHECK(chroma(PixelFormat::I422, ChromaSiting::Left) == Values{75, 192, 197, 118});
  CHECK(chroma(PixelFormat::I422, ChromaSiting::TopLeft) == Values{75, 192, 197, 118});
}

TEST_CASE("colorspace: chroma downsampling impulse responses") {
  // One blue pixel (Cb 255.5, i.e. 127.5 above neutral) on mid-grey. A weight
  // of 1/4 gives 159.9 -> 160, 1/2 gives 191.75 -> 192, 3/4 gives 223.6 -> 224
  // and a weight of 1 gives 255.5, stored as 255.
  const Rgb grey{128, 128, 128};
  const Rgb blue{0, 0, 255};
  // Six samples -> three. Box: pairs (0 1) (2 3) (4 5).
  const std::vector<Values> box6 = {{192, 128, 128}, {192, 128, 128}, {128, 192, 128},
                                    {128, 192, 128}, {128, 128, 192}, {128, 128, 192}};
  // [1 2 1]/4 centred on samples 0, 2 and 4; sample 0 counts its missing
  // left neighbour again and sample 5 only reaches the tap centred on 4.
  const std::vector<Values> tent6 = {{224, 128, 128}, {160, 160, 128}, {128, 192, 128},
                                     {128, 160, 160}, {128, 128, 192}, {128, 128, 160}};
  // Five samples -> three. Box: pairs (0 1) (2 3), sample 4 stands alone.
  const std::vector<Values> box5 = {{192, 128, 128}, {192, 128, 128}, {128, 192, 128}, {128, 192, 128}, {128, 128, 255}};
  // Tent: the tap centred on sample 4 counts it again for the missing sample 5.
  const std::vector<Values> tent5 = {{224, 128, 128}, {160, 160, 128}, {128, 192, 128}, {128, 160, 160}, {128, 128, 224}};

  for (const std::uint32_t length : {6u, 5u}) {
    const std::vector<Values>& box = length == 6 ? box6 : box5;
    const std::vector<Values>& tent = length == 6 ? tent6 : tent5;
    for (std::uint32_t at = 0; at < length; ++at) {
      VideoFrame across = solid_rgba(length, 1, grey);
      set_pixel(across, at, 0, blue);
      VideoFrame down = solid_rgba(1, length, grey);
      set_pixel(down, 0, at, blue);
      for (const ChromaSiting siting : kSitings) {
        INFO("length ", length, " impulse at ", at, " siting ", static_cast<int>(siting));
        const ColorInfo color{ColorMatrix::BT601, ColorRange::Full, siting};
        const bool centred_x = siting == ChromaSiting::Center;
        const bool cosited_y = siting == ChromaSiting::TopLeft;
        for (const PixelFormat format : {PixelFormat::I422, PixelFormat::I420, PixelFormat::NV12}) {
          const auto yuv = from_rgba(across, format, color);
          REQUIRE(yuv);
          CHECK(component_values(*yuv, 1) == (centred_x ? box[at] : tent[at]));
        }
        for (const PixelFormat format : {PixelFormat::I420, PixelFormat::NV12}) {
          const auto yuv = from_rgba(down, format, color);
          REQUIRE(yuv);
          CHECK(component_values(*yuv, 1) == (cosited_y ? tent[at] : box[at]));
        }
        // 4:2:2 keeps every row.
        const auto rows = from_rgba(down, PixelFormat::I422, color);
        REQUIRE(rows);
        Values expected(length, 128);
        expected[at] = 255;
        CHECK(component_values(*rows, 1) == expected);
      }
    }
  }
}

TEST_CASE("colorspace: flat chroma upsamples to exactly flat colour, borders included") {
  constexpr std::uint32_t sizes[][2] = {{1, 1}, {2, 2}, {3, 5}, {17, 9}, {8, 6}};
  for (const PixelFormat format : kYuvFormats) {
    const PixelFormat full = bit_depth(format) == 10 ? PixelFormat::I444P10 : PixelFormat::I444;
    const int scale = bit_depth(format) == 10 ? 4 : 1;
    for (const ChromaSiting siting : kSitings) {
      for (const auto& size : sizes) {
        INFO("format ", static_cast<int>(format), " siting ", static_cast<int>(siting), " size ", size[0], "x", size[1]);
        const ColorInfo color{ColorMatrix::BT709, ColorRange::Limited, siting};
        // Random luma over odd chroma values, against the same picture
        // stored without subsampling.
        VideoFrame subsampled = random_yuv(format, size[0], size[1], color, 5);
        fill_component(subsampled, 1, 91 * scale + 1);
        fill_component(subsampled, 2, 201 * scale + 3);
        VideoFrame reference = make_frame(full, size[0], size[1], color);
        for (std::size_t y = 0; y < size[1]; ++y) {
          for (std::size_t x = 0; x < size[0]; ++x) set_sample(reference, 0, x, y, sample(subsampled, 0, x, y));
        }
        fill_component(reference, 1, 91 * scale + 1);
        fill_component(reference, 2, 201 * scale + 3);
        const auto a = to_rgba(subsampled);
        const auto b = to_rgba(reference);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(same_planes(*a, *b));
      }
    }
    // With flat luma as well, every pixel is the same.
    const auto flat = to_rgba(flat_yuv(format, 9, 7, {ColorMatrix::BT601, ColorRange::Limited, ChromaSiting::Center},
                                       {81 * scale, 90 * scale, 240 * scale}));
    REQUIRE(flat);
    CHECK(all_pixels_are(*flat, pixel(*flat, 0, 0)));
    CHECK(max_rgb_difference(*flat, solid_rgba(9, 7, {255, 0, 0})) <= 2);
  }
}

TEST_CASE("colorspace: super-white and sub-black clamp instead of wrapping") {
  for (const PixelFormat format : kYuvFormats) {
    const int peak = (1 << bit_depth(format)) - 1;
    const int neutral = 1 << (bit_depth(format) - 1);
    for (const ColorMatrix matrix : kMatrices) {
      INFO("format ", static_cast<int>(format), " matrix ", static_cast<int>(matrix));
      const ColorInfo color{matrix, ColorRange::Limited, ChromaSiting::Left};
      const auto white = to_rgba(flat_yuv(format, 4, 4, color, {peak, neutral, neutral}));
      REQUIRE(white);
      CHECK(all_pixels_are(*white, {255, 255, 255}));
      const auto black = to_rgba(flat_yuv(format, 4, 4, color, {0, neutral, neutral}));
      REQUIRE(black);
      CHECK(all_pixels_are(*black, {0, 0, 0}));
      // Wildly out-of-gamut corners: every channel saturates one way or the other.
      for (const Yuv corner : {Yuv{peak, peak, peak}, Yuv{0, 0, 0}, Yuv{peak, 0, peak}, Yuv{0, peak, 0}}) {
        const auto rgba = to_rgba(flat_yuv(format, 3, 3, color, corner));
        REQUIRE(rgba);
        const Rgb p = pixel(*rgba, 1, 1);
        CHECK(all_pixels_are(*rgba, p));
        // Y at the extremes with Cr high pushes red up and green down, etc.
        if (corner.y == peak && corner.cr == peak) {
          CHECK(p.r == 255);
        }
        if (corner.y == 0 && corner.cr == 0) {
          CHECK(p.r == 0);
        }
        if (corner.y == peak && corner.cb == peak) {
          CHECK(p.b == 255);
        }
        if (corner.y == 0 && corner.cb == 0) {
          CHECK(p.b == 0);
        }
      }
    }
  }

  // YCbCr outputs are clamped to the sample range, not to the nominal
  // limited range: super-white survives an 8 -> 10 bit conversion and a
  // matrix change, and is only cut where the target cannot hold it.
  const ColorInfo bt709{ColorMatrix::BT709, ColorRange::Limited, ChromaSiting::Left};
  const VideoFrame bright = flat_yuv(PixelFormat::I420, 4, 4, bt709, {255, 128, 128});
  const auto ten = convert_frame(bright, PixelFormat::I420P10, bt709);
  REQUIRE(ten);
  CHECK(all_samples_are(*ten, {1020, 512, 512}));
  const auto bt601 = convert_frame(bright, PixelFormat::I420, {ColorMatrix::BT601, ColorRange::Limited, ChromaSiting::Left});
  REQUIRE(bt601);
  CHECK(all_samples_are(*bt601, {255, 128, 128}));
  const auto full = convert_frame(bright, PixelFormat::I420, {ColorMatrix::BT709, ColorRange::Full, ChromaSiting::Left});
  REQUIRE(full);
  CHECK(all_samples_are(*full, {255, 128, 128}));
  const VideoFrame dark = flat_yuv(PixelFormat::I420, 4, 4, bt709, {3, 128, 128});
  const auto dark601 = convert_frame(dark, PixelFormat::P010, {ColorMatrix::BT601, ColorRange::Limited, ChromaSiting::Left});
  REQUIRE(dark601);
  CHECK(all_samples_are(*dark601, {12, 512, 512}));
}

TEST_CASE("colorspace: invalid frames are refused") {
  const auto refused = [](const VideoFrame& frame) {
    return is_error(to_rgba(frame), Status::InvalidArgument) &&
           is_error(convert_frame(frame, PixelFormat::I420, {}), Status::InvalidArgument) &&
           is_error(convert_frame(frame, frame.format, frame.color), Status::InvalidArgument);
  };
  for (const PixelFormat format : kYuvFormats) {
    INFO("format ", static_cast<int>(format));
    const VideoFrame good = random_yuv(format, 6, 4, {}, 1);
    REQUIRE(good.valid());
    REQUIRE(to_rgba(good));

    VideoFrame zero_width = good;
    zero_width.width = 0;
    CHECK(refused(zero_width));
    VideoFrame zero_height = good;
    zero_height.height = 0;
    CHECK(refused(zero_height));

    VideoFrame missing_plane = good;
    missing_plane.planes.pop_back();
    CHECK(refused(missing_plane));
    VideoFrame extra_plane = good;
    extra_plane.planes.push_back(good.planes[0]);
    CHECK(refused(extra_plane));
    VideoFrame no_planes = good;
    no_planes.planes.clear();
    CHECK(refused(no_planes));

    for (std::size_t p = 0; p < good.planes.size(); ++p) {
      VideoFrame short_plane = good;
      short_plane.planes[p].data.pop_back();
      CHECK(refused(short_plane));
      VideoFrame empty_plane = good;
      empty_plane.planes[p].data.clear();
      CHECK(refused(empty_plane));
      VideoFrame narrow_stride = good;
      narrow_stride.planes[p].stride -= 1;
      CHECK(refused(narrow_stride));
      VideoFrame zero_stride = good;
      zero_stride.planes[p].stride = 0;
      CHECK(refused(zero_stride));
    }
    // A stride so large that stride * (rows - 1) wraps around to zero must not
    // be mistaken for a plane that is big enough. With five luma rows every
    // plane has three or five rows, so rows - 1 is 2 or 4.
    const VideoFrame five_rows = random_yuv(format, 6, 5, {}, 2);
    REQUIRE(to_rgba(five_rows));
    for (std::size_t p = 0; p < five_rows.planes.size(); ++p) {
      VideoFrame wrapping_stride = five_rows;
      const std::size_t rows = plane_height(format, p, 5);
      wrapping_stride.planes[p].stride = std::numeric_limits<std::size_t>::max() / (rows - 1) + 1;
      CHECK(refused(wrapping_stride));
    }
    // Taller than the planes allow.
    VideoFrame too_tall = good;
    too_tall.height += 2;
    CHECK(refused(too_tall));

    // from_rgba wants RGBA8 input, however valid the frame is otherwise.
    CHECK(is_error(from_rgba(good, PixelFormat::I420), Status::InvalidArgument));
    CHECK(is_error(from_rgba(good, PixelFormat::RGBA8), Status::InvalidArgument));
  }

  const VideoFrame rgba = random_rgba(5, 3, 1);
  REQUIRE(from_rgba(rgba, PixelFormat::I420));
  VideoFrame bad = rgba;
  bad.planes[0].data.pop_back();
  CHECK(is_error(from_rgba(bad, PixelFormat::I420), Status::InvalidArgument));
  CHECK(is_error(to_rgba(bad), Status::InvalidArgument));
  CHECK(is_error(convert_frame(bad, PixelFormat::NV12, {}), Status::InvalidArgument));
  bad = rgba;
  bad.planes[0].stride = 5 * 4 - 1;
  CHECK(is_error(from_rgba(bad, PixelFormat::I420), Status::InvalidArgument));
  bad = rgba;
  bad.width = 0;
  CHECK(is_error(from_rgba(bad, PixelFormat::I420), Status::InvalidArgument));
  bad = rgba;
  bad.planes.push_back(rgba.planes[0]);
  CHECK(is_error(from_rgba(bad, PixelFormat::I420), Status::InvalidArgument));
  CHECK(is_error(from_rgba(VideoFrame{}, PixelFormat::I420), Status::InvalidArgument));
  CHECK(is_error(to_rgba(VideoFrame{}), Status::InvalidArgument));

  // Pixel format values outside the enumeration are refused, not guessed at.
  const auto unknown = static_cast<PixelFormat>(99);
  CHECK(is_error(from_rgba(rgba, unknown), Status::InvalidArgument));
  CHECK(is_error(convert_frame(rgba, unknown, {}), Status::InvalidArgument));
  VideoFrame odd_format = rgba;
  odd_format.format = unknown;
  CHECK(is_error(to_rgba(odd_format), Status::InvalidArgument));
}

TEST_CASE("colorspace: convert_frame with identical format and colour is a packed copy") {
  for (const PixelFormat format : kYuvFormats) {
    INFO("format ", static_cast<int>(format));
    const ColorInfo color{ColorMatrix::BT2020, ColorRange::Full, ChromaSiting::TopLeft};
    VideoFrame frame = random_yuv(format, 7, 5, color, 9);
    frame.pts_ms = 1234;
    frame.duration_ms = 40;
    const auto copy = convert_frame(with_padding(frame, 5), format, color);
    REQUIRE(copy);
    CHECK(same_planes(*copy, frame));
    CHECK(copy->color == color);
    CHECK(copy->pts_ms == 1234);
    CHECK(copy->duration_ms == 40);
  }
  // RGBA to RGBA is a copy whatever colour description is passed.
  const VideoFrame rgba = random_rgba(7, 5, 2);
  const ColorInfo any{ColorMatrix::BT601, ColorRange::Full, ChromaSiting::Center};
  const auto copy = convert_frame(with_padding(rgba, 3), PixelFormat::RGBA8, any);
  REQUIRE(copy);
  CHECK(same_planes(*copy, rgba));
}

TEST_CASE("colorspace: convert_frame agrees with to_rgba and from_rgba when RGBA is involved") {
  const ColorInfo color{ColorMatrix::BT601, ColorRange::Full, ChromaSiting::Center};
  const VideoFrame rgba = random_rgba(9, 7, 4);
  for (const PixelFormat format : kYuvFormats) {
    INFO("format ", static_cast<int>(format));
    const auto direct = from_rgba(rgba, format, color);
    const auto converted = convert_frame(rgba, format, color);
    REQUIRE(direct);
    REQUIRE(converted);
    CHECK(same_planes(*direct, *converted));
    CHECK(converted->color == color);

    const auto direct_rgba = to_rgba(*direct);
    const auto converted_rgba = convert_frame(*direct, PixelFormat::RGBA8, {});
    REQUIRE(direct_rgba);
    REQUIRE(converted_rgba);
    CHECK(same_planes(*direct_rgba, *converted_rgba));
    CHECK(converted_rgba->color == color);  // informational: the source's
  }
}

TEST_CASE("colorspace: planar and semi-planar layouts convert losslessly") {
  for (const ChromaSiting siting : kSitings) {
    const ColorInfo color{ColorMatrix::BT709, ColorRange::Limited, siting};
    const VideoFrame i420 = random_yuv(PixelFormat::I420, 17, 9, color, 21);
    const auto nv12 = convert_frame(i420, PixelFormat::NV12, color);
    REQUIRE(nv12);
    CHECK(nv12->format == PixelFormat::NV12);
    for (std::size_t component = 0; component < 3; ++component) {
      CHECK(component_values(*nv12, component) == component_values(i420, component));
    }
    const auto back = convert_frame(*nv12, PixelFormat::I420, color);
    REQUIRE(back);
    CHECK(same_planes(*back, i420));

    const VideoFrame i420p10 = random_yuv(PixelFormat::I420P10, 17, 9, color, 22);
    const auto p010 = convert_frame(i420p10, PixelFormat::P010, color);
    REQUIRE(p010);
    for (std::size_t component = 0; component < 3; ++component) {
      CHECK(component_values(*p010, component) == component_values(i420p10, component));
    }
    const auto back10 = convert_frame(*p010, PixelFormat::I420P10, color);
    REQUIRE(back10);
    CHECK(same_planes(*back10, i420p10));
  }
}

TEST_CASE("colorspace: 8-bit to 10-bit and back is lossless and scales by the range's rule") {
  constexpr PixelFormat pairs[][2] = {{PixelFormat::I420, PixelFormat::I420P10}, {PixelFormat::I422, PixelFormat::I422P10},
                                      {PixelFormat::I444, PixelFormat::I444P10}, {PixelFormat::NV12, PixelFormat::P010},
                                      {PixelFormat::I420, PixelFormat::P010},    {PixelFormat::NV12, PixelFormat::I420P10}};
  for (const auto& pair : pairs) {
    for (const ColorRange range : kRanges) {
      for (const ColorMatrix matrix : kMatrices) {
        INFO("format ", static_cast<int>(pair[0]), " range ", static_cast<int>(range), " matrix ", static_cast<int>(matrix));
        const ColorInfo color{matrix, range, ChromaSiting::Left};
        const VideoFrame eight = random_yuv(pair[0], 33, 17, color, 77);
        const auto ten = convert_frame(eight, pair[1], color);
        REQUIRE(ten);
        CHECK(ten->format == pair[1]);
        bool scaled = true;
        for (std::size_t component = 0; component < 3; ++component) {
          for (std::size_t y = 0; y < component_height(eight, component); ++y) {
            for (std::size_t x = 0; x < component_width(eight, component); ++x) {
              const int v = sample(eight, component, x, y);
              int expected = 0;
              if (range == ColorRange::Limited) {
                expected = v * 4;  // 16..235 -> 64..940, 128 -> 512
              } else if (component == 0) {
                expected = (v * 1023 * 2 + 255) / (255 * 2);  // round(v * 1023 / 255)
              } else {
                // 512 + (v - 128) * 1023 / 255, rounded away from 512, clamped at 0.
                const int excursion = std::abs(v - 128) * 1023;
                const int rounded = (excursion * 2 + 255) / (255 * 2);
                expected = std::max(0, v >= 128 ? 512 + rounded : 512 - rounded);
              }
              scaled = scaled && sample(*ten, component, x, y) == expected;
            }
          }
        }
        CHECK(scaled);
        const auto back = convert_frame(*ten, pair[0], color);
        REQUIRE(back);
        CHECK(same_planes(*back, eight));
      }
    }
  }

  // Spot values. Limited: exactly four times. Full: luma 0..255 -> 0..1023,
  // chroma 128 -> 512 with 1023/255 codes per code around it.
  const ColorInfo limited{ColorMatrix::BT709, ColorRange::Limited, ChromaSiting::Left};
  const ColorInfo full{ColorMatrix::BT709, ColorRange::Full, ChromaSiting::Left};
  const auto convert_flat = [](Yuv value, const ColorInfo& from, PixelFormat target, const ColorInfo& to) {
    const PixelFormat source = bit_depth(target) == 10 ? PixelFormat::I444 : PixelFormat::I444P10;
    const auto out = convert_frame(flat_yuv(source, 2, 2, from, value), target, to);
    REQUIRE(out);
    return Yuv{sample(*out, 0, 1, 1), sample(*out, 1, 1, 1), sample(*out, 2, 1, 1)};
  };
  CHECK(convert_flat({235, 16, 240}, limited, PixelFormat::I444P10, limited) == Yuv{940, 64, 960});
  CHECK(convert_flat({255, 128, 0}, full, PixelFormat::I444P10, full) == Yuv{1023, 512, 0});
  CHECK(convert_flat({128, 255, 1}, full, PixelFormat::I444P10, full) == Yuv{514, 1021, 3});
  CHECK(convert_flat({200, 16, 240}, full, PixelFormat::I444P10, full) == Yuv{802, 63, 961});
  // 10 -> 8 bit rounds to nearest: limited 942/4 = 235.5 -> 236, chroma 514/4
  // = 128.5 -> 129 and 510/4 = 127.5 -> 127 (away from neutral).
  CHECK(convert_flat({942, 514, 510}, limited, PixelFormat::I444, limited) == Yuv{236, 129, 127});
  CHECK(convert_flat({941, 513, 511}, limited, PixelFormat::I444, limited) == Yuv{235, 128, 128});
}

TEST_CASE("colorspace: range conversion keeps the matrix and rescales each plane") {
  // Limited -> full at 8 bits: Y (v - 16) * 255 / 219, chroma 128 + (v - 128) * 255 / 224.
  const ColorInfo limited{ColorMatrix::BT601, ColorRange::Limited, ChromaSiting::Center};
  const ColorInfo full{ColorMatrix::BT601, ColorRange::Full, ChromaSiting::Center};
  const auto to_full = [&](Yuv value) {
    const auto out = convert_frame(flat_yuv(PixelFormat::I420, 4, 4, limited, value), PixelFormat::I420, full);
    REQUIRE(out);
    CHECK(out->color == full);
    return Yuv{sample(*out, 0, 0, 0), sample(*out, 1, 0, 0), sample(*out, 2, 0, 0)};
  };
  CHECK(to_full({16, 128, 128}) == Yuv{0, 128, 128});
  CHECK(to_full({235, 128, 128}) == Yuv{255, 128, 128});
  CHECK(to_full({128, 200, 56}) == Yuv{130, 210, 46});
  // Chroma 16 maps to exactly 0.5 and 240 to 255.5: away from neutral, then clamped.
  CHECK(to_full({200, 16, 240}) == Yuv{214, 0, 255});
  CHECK(to_full({0, 0, 255}) == Yuv{0, 0, 255});
  CHECK(to_full({255, 254, 1}) == Yuv{255, 255, 0});

  // The chroma planes are not resampled by a range or depth change: a random
  // plane survives limited -> full -> limited at 10 bits within one code.
  const ColorInfo full709{ColorMatrix::BT709, ColorRange::Full, ChromaSiting::Left};
  const ColorInfo limited709{ColorMatrix::BT709, ColorRange::Limited, ChromaSiting::Left};
  const VideoFrame noisy = random_yuv(PixelFormat::I420P10, 16, 8, full709, 3);
  const auto as_limited = convert_frame(noisy, PixelFormat::I420P10, limited709);
  REQUIRE(as_limited);
  const auto again = convert_frame(*as_limited, PixelFormat::I420P10, full709);
  REQUIRE(again);
  int worst = 0;
  for (std::size_t component = 0; component < 3; ++component) {
    const Values a = component_values(noisy, component);
    const Values b = component_values(*again, component);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
  }
  CHECK(worst <= 1);
}

TEST_CASE("colorspace: convert_frame resamples chroma only where its layout changes") {
  const ColorInfo left{ColorMatrix::BT709, ColorRange::Limited, ChromaSiting::Left};
  const ColorInfo center{ColorMatrix::BT709, ColorRange::Limited, ChromaSiting::Center};
  const ColorInfo top_left{ColorMatrix::BT709, ColorRange::Limited, ChromaSiting::TopLeft};

  // 8x4 luma, 4x2 chroma; Cr rows: 64 64 192 192 / 64 64 192 192.
  VideoFrame frame = flat_yuv(PixelFormat::I420, 8, 4, left, {100, 128, 64});
  for (std::size_t row = 0; row < 2; ++row) {
    set_sample(frame, 2, 2, row, 192);
    set_sample(frame, 2, 3, row, 192);
  }

  // To 4:4:4: the bilinear reconstruction itself.
  const auto full_left = convert_frame(frame, PixelFormat::I444, left);
  REQUIRE(full_left);
  for (std::size_t y = 0; y < 4; ++y) {
    Values row;
    for (std::size_t x = 0; x < 8; ++x) row.push_back(sample(*full_left, 2, x, y));
    CHECK(row == Values{64, 64, 64, 128, 192, 192, 192, 192});
  }
  VideoFrame centred = frame;
  centred.color = center;
  const auto full_center = convert_frame(centred, PixelFormat::I444, center);
  REQUIRE(full_center);
  CHECK(sample(*full_center, 2, 3, 0) == 96);
  CHECK(sample(*full_center, 2, 4, 0) == 160);
  CHECK(all_samples_are(*convert_frame(flat_yuv(PixelFormat::I420, 5, 3, left, {100, 77, 201}), PixelFormat::I444, left),
                        {100, 77, 201}));

  // Left -> Center moves the samples half a luma pixel to the right: the
  // reconstruction 64 64 64 128 192 192 192 192 averaged in pairs.
  const auto moved = convert_frame(frame, PixelFormat::I420, center);
  REQUIRE(moved);
  CHECK(component_values(*moved, 2) == Values{64, 96, 192, 192, 64, 96, 192, 192});
  CHECK(component_values(*moved, 0) == component_values(frame, 0));
  CHECK(moved->color == center);

  // Left -> TopLeft only differs vertically, where this picture is constant:
  // the horizontal step must come through untouched.
  const auto shifted = convert_frame(frame, PixelFormat::NV12, top_left);
  REQUIRE(shifted);
  CHECK(component_values(*shifted, 2) == component_values(frame, 2));

  // 4:2:0 -> 4:2:2 keeps the columns and interpolates rows (64 | 192 -> 64 96 160 192).
  VideoFrame rows = flat_yuv(PixelFormat::I420, 4, 4, left, {100, 128, 64});
  set_sample(rows, 2, 0, 1, 192);
  set_sample(rows, 2, 1, 1, 192);
  // Left -> TopLeft moves the samples up by half a luma row: [1 2 1]/4 over
  // the reconstruction 64 96 160 192 around rows 0 and 2, the row above the
  // top replicated: (64 + 128 + 96) / 4 = 72 and (96 + 320 + 192) / 4 = 152.
  const auto raised = convert_frame(rows, PixelFormat::I420, top_left);
  REQUIRE(raised);
  CHECK(component_values(*raised, 2) == Values{72, 72, 152, 152});
  // TopLeft -> Left: the reconstruction is 64 128 192 192, boxed in pairs.
  VideoFrame cosited = rows;
  cosited.color = top_left;
  const auto lowered = convert_frame(cosited, PixelFormat::NV12, left);
  REQUIRE(lowered);
  CHECK(component_values(*lowered, 2) == Values{96, 96, 192, 192});
  const auto taller = convert_frame(rows, PixelFormat::I422, left);
  REQUIRE(taller);
  CHECK(component_values(*taller, 2) == Values{64, 64, 96, 96, 160, 160, 192, 192});
  // ... and 4:2:2 -> 4:2:0 box-filters the rows again without touching columns.
  VideoFrame columns = flat_yuv(PixelFormat::I422, 4, 4, left, {100, 128, 64});
  for (std::size_t row = 0; row < 4; ++row) set_sample(columns, 2, 1, row, 192);
  set_sample(columns, 2, 0, 3, 128);
  const auto shorter = convert_frame(columns, PixelFormat::I420, left);
  REQUIRE(shorter);
  CHECK(component_values(*shorter, 2) == Values{64, 192, 96, 192});

  // 4:4:4 -> 4:2:0 Center is the 2x2 box.
  VideoFrame dense = flat_yuv(PixelFormat::I444, 2, 2, center, {100, 128, 128});
  set_sample(dense, 1, 0, 0, 10);
  set_sample(dense, 1, 1, 0, 20);
  set_sample(dense, 1, 0, 1, 30);
  set_sample(dense, 1, 1, 1, 60);
  const auto boxed = convert_frame(dense, PixelFormat::I420, center);
  REQUIRE(boxed);
  CHECK(component_values(*boxed, 1) == Values{30});
  // ... and TopLeft weights 9/16 3/16 3/16 1/16: 5.625 + 3.75 + 5.625 + 3.75 = 18.75.
  const auto weighted = convert_frame(dense, PixelFormat::I420, top_left);
  REQUIRE(weighted);
  CHECK(component_values(*weighted, 1) == Values{19});
}

TEST_CASE("colorspace: matrix conversion matches the direct route to RGB") {
  const ColorInfo source{ColorMatrix::BT601, ColorRange::Full, ChromaSiting::Left};
  const ColorInfo target{ColorMatrix::BT709, ColorRange::Limited, ChromaSiting::Left};

  // Arbitrary in-gamut pixels at 4:4:4.
  const auto noise = from_rgba(random_rgba(40, 30, 6), PixelFormat::I444, source);
  REQUIRE(noise);
  const auto converted = convert_frame(*noise, PixelFormat::I444, target);
  REQUIRE(converted);
  CHECK(converted->color == target);
  const auto direct = to_rgba(*noise);
  const auto via = to_rgba(*converted);
  REQUIRE(direct);
  REQUIRE(via);
  CHECK(max_rgb_difference(*direct, *via) <= 2);

  // Known colours through every subsampled layout: BT.601 full red (76, 85, 255)
  // becomes BT.709 limited red (63, 102, 240) within the rounding of the source.
  constexpr Rgb colors[] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}, {0, 0, 0}, {90, 160, 30}};
  for (const PixelFormat from : kYuvFormats) {
    for (const PixelFormat to : kYuvFormats) {
      int worst = 0;
      for (const Rgb& value : colors) {
        const VideoFrame rgba = solid_rgba(7, 5, value);
        const auto yuv = from_rgba(rgba, from, source);
        REQUIRE(yuv);
        const auto other = convert_frame(*yuv, to, target);
        REQUIRE(other);
        REQUIRE(other->valid());
        const auto a = to_rgba(*yuv);
        const auto b = to_rgba(*other);
        REQUIRE(a);
        REQUIRE(b);
        worst = std::max(worst, max_rgb_difference(*a, *b));
      }
      INFO("from ", static_cast<int>(from), " to ", static_cast<int>(to));
      CHECK(worst <= 2);
    }
  }
  const auto red = convert_frame(flat_yuv(PixelFormat::I420, 4, 4, source, {76, 85, 255}), PixelFormat::NV12, target);
  REQUIRE(red);
  CHECK(std::abs(sample(*red, 0, 0, 0) - 63) <= 1);
  CHECK(std::abs(sample(*red, 1, 0, 0) - 102) <= 1);
  CHECK(std::abs(sample(*red, 2, 0, 0) - 240) <= 1);

  // 10 bit -> 10 bit does not pass through 8 bits: all 877 grey levels of a
  // BT.709 limited ramp stay distinct (and identical) under BT.2020.
  const ColorInfo bt709{ColorMatrix::BT709, ColorRange::Limited, ChromaSiting::Left};
  const ColorInfo bt2020{ColorMatrix::BT2020, ColorRange::Limited, ChromaSiting::Left};
  VideoFrame ramp = make_frame(PixelFormat::I444P10, 877, 1, bt709);
  fill_component(ramp, 1, 512);
  fill_component(ramp, 2, 512);
  for (std::size_t x = 0; x < 877; ++x) set_sample(ramp, 0, x, 0, static_cast<int>(64 + x));
  const auto wide = convert_frame(ramp, PixelFormat::I444P10, bt2020);
  REQUIRE(wide);
  CHECK(same_planes(*wide, ramp));
  // ... and a saturated 10-bit colour keeps its precision: BT.709 limited
  // red (250, 409, 960) is BT.2020 limited (294, 387, 960) for R'G'B' red.
  const auto red10 = convert_frame(flat_yuv(PixelFormat::P010, 4, 4, bt709, {250, 409, 960}), PixelFormat::I420P10, bt2020);
  REQUIRE(red10);
  CHECK(std::abs(sample(*red10, 0, 0, 0) - 294) <= 1);
  CHECK(std::abs(sample(*red10, 1, 0, 0) - 387) <= 1);
  CHECK(std::abs(sample(*red10, 2, 0, 0) - 960) <= 1);
}

TEST_CASE("colorspace: timestamps and colour info are carried over") {
  const ColorInfo source{ColorMatrix::BT601, ColorRange::Full, ChromaSiting::Center};
  const ColorInfo target{ColorMatrix::BT2020, ColorRange::Limited, ChromaSiting::TopLeft};
  VideoFrame rgba = random_rgba(6, 4, 8);
  rgba.pts_ms = -40;
  rgba.duration_ms = 33;
  rgba.color = source;  // ignored for RGBA8, but kept on RGBA8 outputs

  for (const PixelFormat format : kYuvFormats) {
    INFO("format ", static_cast<int>(format));
    const auto yuv = from_rgba(rgba, format, target);
    REQUIRE(yuv);
    CHECK(yuv->format == format);
    CHECK(yuv->color == target);
    CHECK(yuv->pts_ms == -40);
    CHECK(yuv->duration_ms == 33);

    VideoFrame frame = *yuv;
    frame.pts_ms = 9'000'000'000;
    frame.duration_ms = 17;
    const auto back = to_rgba(frame);
    REQUIRE(back);
    CHECK(back->format == PixelFormat::RGBA8);
    CHECK(back->color == target);
    CHECK(back->pts_ms == 9'000'000'000);
    CHECK(back->duration_ms == 17);

    for (const ColorInfo& color : {target, source, ColorInfo{ColorMatrix::BT2020, ColorRange::Full, ChromaSiting::Left}}) {
      const auto converted = convert_frame(frame, PixelFormat::I422P10, color);
      REQUIRE(converted);
      CHECK(converted->format == PixelFormat::I422P10);
      CHECK(converted->color == color);
      CHECK(converted->pts_ms == 9'000'000'000);
      CHECK(converted->duration_ms == 17);
    }
  }

  const auto copy = to_rgba(rgba);
  REQUIRE(copy);
  CHECK(copy->color == source);
  CHECK(copy->pts_ms == -40);
  CHECK(copy->duration_ms == 33);
  const auto copy2 = from_rgba(rgba, PixelFormat::RGBA8, target);
  REQUIRE(copy2);
  CHECK(copy2->color == source);
  CHECK(copy2->pts_ms == -40);
  CHECK(copy2->duration_ms == 33);
  // The default colour description is BT.709 limited, left-sited.
  const auto defaulted = from_rgba(rgba, PixelFormat::I420);
  REQUIRE(defaulted);
  CHECK(defaulted->color == ColorInfo{});
}

TEST_CASE("colorspace: alpha is ignored on input and opaque on output") {
  VideoFrame opaque = random_rgba(8, 6, 12);
  VideoFrame clear = opaque;
  for (std::size_t i = 3; i < opaque.planes[0].data.size(); i += 4) {
    opaque.planes[0].data[i] = 255;
    clear.planes[0].data[i] = 0;
  }
  for (const PixelFormat format : kYuvFormats) {
    const auto a = from_rgba(opaque, format);
    const auto b = from_rgba(clear, format);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(same_planes(*a, *b));
    const auto back = to_rgba(*b);
    REQUIRE(back);
    bool all_opaque = true;
    for (std::size_t i = 3; i < back->planes[0].data.size(); i += 4) {
      all_opaque = all_opaque && back->planes[0].data[i] == 255;
    }
    CHECK(all_opaque);
  }
  // An RGBA copy keeps the alpha it was given.
  const auto copy = to_rgba(clear);
  REQUIRE(copy);
  CHECK(same_planes(*copy, clear));
}
