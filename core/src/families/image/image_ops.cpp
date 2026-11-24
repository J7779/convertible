// Convertible core - image family: the shared raster toolbox.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "families/image/image_ops.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>

#include "engine/standard_options.hpp"

namespace cvt::image {

namespace {

// --------------------------------------------------------------------------- sRGB tables

double srgb_decode(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double srgb_encode(double v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; }

struct SrgbTables {
  std::array<float, 256> to_linear{};
  // thresholds[i] = linear value halfway (in encoded space) between code i and i+1.
  std::array<float, 255> thresholds{};
  // Coarse guess of the code for a linear value, refined with `thresholds`.
  std::array<std::uint8_t, 4097> guess{};

  SrgbTables() {
    for (int i = 0; i < 256; ++i) to_linear[static_cast<std::size_t>(i)] = static_cast<float>(srgb_decode(i / 255.0));
    for (int i = 0; i < 255; ++i) {
      thresholds[static_cast<std::size_t>(i)] = static_cast<float>(srgb_decode((i + 0.5) / 255.0));
    }
    for (int i = 0; i <= 4096; ++i) {
      const double encoded = srgb_encode(i / 4096.0) * 255.0;
      guess[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::clamp(std::floor(encoded), 0.0, 255.0));
    }
  }
};

const SrgbTables& tables() {
  static const SrgbTables instance;
  return instance;
}

inline std::uint8_t encode8(const SrgbTables& t, float linear) noexcept {
  if (!(linear > 0.0f)) return 0;
  if (linear >= 1.0f) return 255;
  int code = t.guess[static_cast<std::size_t>(linear * 4096.0f)];
  while (code < 255 && linear > t.thresholds[static_cast<std::size_t>(code)]) ++code;
  while (code > 0 && linear <= t.thresholds[static_cast<std::size_t>(code - 1)]) --code;
  return static_cast<std::uint8_t>(code);
}

inline std::uint8_t clamp_u8(float v) noexcept {
  if (!(v > 0.0f)) return 0;
  if (v >= 255.0f) return 255;
  return static_cast<std::uint8_t>(v + 0.5f);
}

}  // namespace

float srgb_to_linear(std::uint8_t value) noexcept { return tables().to_linear[value]; }
float srgb_to_linear(float encoded) noexcept {
  return static_cast<float>(srgb_decode(std::clamp(static_cast<double>(encoded), 0.0, 1.0)));
}
float linear_to_srgb(float linear) noexcept {
  if (!(linear > 0.0f)) return 0.0f;
  if (linear >= 1.0f) return 1.0f;
  return static_cast<float>(srgb_encode(linear));
}
std::uint8_t linear_to_srgb8(float linear) noexcept { return encode8(tables(), linear); }
std::uint16_t linear_to_srgb16(float linear) noexcept {
  if (!(linear > 0.0f)) return 0;
  if (linear >= 1.0f) return 65535;
  return static_cast<std::uint16_t>(std::lround(srgb_encode(static_cast<double>(linear)) * 65535.0));
}

ir::HdrPixels to_linear(const ir::Frame& frame) {
  const SrgbTables& t = tables();
  ir::HdrPixels out;
  out.width = frame.width;
  out.height = frame.height;
  out.rgba.resize(frame.rgba.size());
  for (std::size_t i = 0; i + 3 < frame.rgba.size(); i += 4) {
    out.rgba[i] = t.to_linear[frame.rgba[i]];
    out.rgba[i + 1] = t.to_linear[frame.rgba[i + 1]];
    out.rgba[i + 2] = t.to_linear[frame.rgba[i + 2]];
    out.rgba[i + 3] = frame.rgba[i + 3] / 255.0f;
  }
  return out;
}

ir::Frame from_linear(const ir::HdrPixels& pixels) {
  const SrgbTables& t = tables();
  ir::Frame out;
  out.width = pixels.width;
  out.height = pixels.height;
  out.rgba.resize(pixels.rgba.size());
  for (std::size_t i = 0; i + 3 < pixels.rgba.size(); i += 4) {
    out.rgba[i] = encode8(t, pixels.rgba[i]);
    out.rgba[i + 1] = encode8(t, pixels.rgba[i + 1]);
    out.rgba[i + 2] = encode8(t, pixels.rgba[i + 2]);
    out.rgba[i + 3] = clamp_u8(pixels.rgba[i + 3] * 255.0f);
  }
  return out;
}

ir::HdrPixels linear_from_srgb16(std::span<const std::uint16_t> rgba16, std::uint32_t width, std::uint32_t height) {
  ir::HdrPixels out;
  out.width = width;
  out.height = height;
  out.rgba.resize(rgba16.size());
  for (std::size_t i = 0; i + 3 < rgba16.size(); i += 4) {
    for (std::size_t c = 0; c < 3; ++c) {
      out.rgba[i + c] = static_cast<float>(srgb_decode(rgba16[i + c] / 65535.0));
    }
    out.rgba[i + 3] = static_cast<float>(rgba16[i + 3] / 65535.0);
  }
  return out;
}

std::vector<std::uint16_t> srgb16_from_linear(const ir::HdrPixels& pixels) {
  std::vector<std::uint16_t> out(pixels.rgba.size());
  for (std::size_t i = 0; i + 3 < pixels.rgba.size(); i += 4) {
    for (std::size_t c = 0; c < 3; ++c) out[i + c] = linear_to_srgb16(pixels.rgba[i + c]);
    const float a = pixels.rgba[i + 3];
    out[i + 3] = !(a > 0.0f) ? 0 : a >= 1.0f ? 65535 : static_cast<std::uint16_t>(std::lround(a * 65535.0));
  }
  return out;
}

// --------------------------------------------------------------------------- tone mapping

ir::Frame tone_map(const ir::HdrPixels& pixels, const ToneMapOptions& options) {
  const SrgbTables& t = tables();
  const float gain = std::exp2(options.exposure_stops);
  ToneMapOperator op = options.op;
  float max_luma = 0.0f;
  if (op == ToneMapOperator::Auto || op == ToneMapOperator::Reinhard) {
    float max_value = 0.0f;
    for (std::size_t i = 0; i + 3 < pixels.rgba.size(); i += 4) {
      for (std::size_t c = 0; c < 3; ++c) {
        const float v = pixels.rgba[i + c] * gain;
        if (std::isfinite(v)) max_value = std::max(max_value, v);
      }
      const float l = (0.2126f * pixels.rgba[i] + 0.7152f * pixels.rgba[i + 1] + 0.0722f * pixels.rgba[i + 2]) * gain;
      if (std::isfinite(l)) max_luma = std::max(max_luma, l);
    }
    if (op == ToneMapOperator::Auto) op = max_value <= 1.0f ? ToneMapOperator::Clamp : ToneMapOperator::Filmic;
  }
  const auto filmic = [](float x) {
    // Narkowicz 2015 ACES approximation, input pre-scaled so mid grey stays put.
    x *= 0.6f;
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
  };
  const float white2 = std::max(max_luma, 1.0f) * std::max(max_luma, 1.0f);
  ir::Frame out;
  out.width = pixels.width;
  out.height = pixels.height;
  out.rgba.resize(pixels.rgba.size());
  for (std::size_t i = 0; i + 3 < pixels.rgba.size(); i += 4) {
    float rgb[3];
    for (std::size_t c = 0; c < 3; ++c) {
      const float v = pixels.rgba[i + c] * gain;
      rgb[c] = std::isfinite(v) && v > 0.0f ? v : (v > 0.0f ? 1e30f : 0.0f);
    }
    if (op == ToneMapOperator::Filmic) {
      for (float& v : rgb) v = filmic(v);
    } else if (op == ToneMapOperator::Reinhard) {
      const float l = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
      if (l > 0.0f) {
        const float mapped = l * (1.0f + l / white2) / (1.0f + l);
        for (float& v : rgb) v = v * mapped / l;
      }
    }
    for (std::size_t c = 0; c < 3; ++c) out.rgba[i + c] = encode8(t, rgb[c]);
    const float a = pixels.rgba[i + 3];
    out.rgba[i + 3] = std::isfinite(a) ? clamp_u8(a * 255.0f) : 255;
  }
  return out;
}

// --------------------------------------------------------------------------- alpha

void premultiply(ir::Frame& frame) noexcept {
  for (std::size_t i = 0; i + 3 < frame.rgba.size(); i += 4) {
    const std::uint32_t a = frame.rgba[i + 3];
    if (a == 255) continue;
    for (std::size_t c = 0; c < 3; ++c) frame.rgba[i + c] = static_cast<std::uint8_t>((frame.rgba[i + c] * a + 127) / 255);
  }
}

void unpremultiply(ir::Frame& frame) noexcept {
  for (std::size_t i = 0; i + 3 < frame.rgba.size(); i += 4) {
    const std::uint32_t a = frame.rgba[i + 3];
    if (a == 255) continue;
    if (a == 0) {
      frame.rgba[i] = frame.rgba[i + 1] = frame.rgba[i + 2] = 0;
      continue;
    }
    for (std::size_t c = 0; c < 3; ++c) {
      frame.rgba[i + c] = static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (frame.rgba[i + c] * 255u + a / 2) / a));
    }
  }
}

void flatten_alpha(ir::Frame& frame, Rgb background) noexcept {
  for (std::size_t i = 0; i + 3 < frame.rgba.size(); i += 4) {
    const std::uint32_t a = frame.rgba[i + 3];
    if (a == 255) continue;
    for (std::size_t c = 0; c < 3; ++c) {
      frame.rgba[i + c] = static_cast<std::uint8_t>((frame.rgba[i + c] * a + background[c] * (255u - a) + 127u) / 255u);
    }
    frame.rgba[i + 3] = 255;
  }
}

void flatten_alpha(ir::Image& image, Rgb background) noexcept {
  if (std::ranges::any_of(image.frames, [](const ir::Frame& f) { return f.has_transparency(); })) image.clear_original();
  for (ir::Frame& frame : image.frames) flatten_alpha(frame, background);
  if (image.hdr) {
    const float bg[3] = {srgb_to_linear(background[0]), srgb_to_linear(background[1]), srgb_to_linear(background[2])};
    for (std::size_t i = 0; i + 3 < image.hdr->rgba.size(); i += 4) {
      const float a = std::clamp(image.hdr->rgba[i + 3], 0.0f, 1.0f);
      for (std::size_t c = 0; c < 3; ++c) image.hdr->rgba[i + c] = image.hdr->rgba[i + c] * a + bg[c] * (1.0f - a);
      image.hdr->rgba[i + 3] = 1.0f;
    }
  }
}

bool is_opaque(const ir::Frame& frame) noexcept { return !frame.has_transparency(); }

bool is_grayscale(const ir::Frame& frame) noexcept {
  for (std::size_t i = 0; i + 3 < frame.rgba.size(); i += 4) {
    if (frame.rgba[i] != frame.rgba[i + 1] || frame.rgba[i] != frame.rgba[i + 2]) return false;
  }
  return true;
}

bool has_binary_alpha(const ir::Frame& frame) noexcept {
  for (std::size_t i = 3; i < frame.rgba.size(); i += 4) {
    if (frame.rgba[i] != 0 && frame.rgba[i] != 255) return false;
  }
  return true;
}

Result<Rgb> parse_color(std::string_view text) {
  std::string lower;
  for (const char c : text) {
    if (c == ' ' || c == '\t') continue;
    lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  struct Named {
    std::string_view name;
    Rgb rgb;
  };
  static constexpr Named names[] = {
      {"white", {255, 255, 255}}, {"black", {0, 0, 0}},       {"red", {255, 0, 0}},
      {"green", {0, 128, 0}},     {"lime", {0, 255, 0}},      {"blue", {0, 0, 255}},
      {"gray", {128, 128, 128}},  {"grey", {128, 128, 128}},  {"transparent", {255, 255, 255}},
      {"yellow", {255, 255, 0}},  {"cyan", {0, 255, 255}},    {"magenta", {255, 0, 255}},
  };
  for (const Named& named : names) {
    if (lower == named.name) return named.rgb;
  }
  std::string_view hex = lower;
  if (!hex.empty() && hex.front() == '#') hex.remove_prefix(1);
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (const char c : hex) {
    if (nibble(c) < 0) return fail(Status::InvalidArgument, "'{}' is not a colour (use RRGGBB hex or a name)", text);
  }
  if (hex.size() == 6) {
    Rgb rgb{};
    for (std::size_t i = 0; i < 3; ++i) rgb[i] = static_cast<std::uint8_t>(nibble(hex[i * 2]) * 16 + nibble(hex[i * 2 + 1]));
    return rgb;
  }
  if (hex.size() == 3) {
    Rgb rgb{};
    for (std::size_t i = 0; i < 3; ++i) rgb[i] = static_cast<std::uint8_t>(nibble(hex[i]) * 17);
    return rgb;
  }
  return fail(Status::InvalidArgument, "'{}' is not a colour (use RRGGBB hex or a name)", text);
}

// --------------------------------------------------------------------------- resampling

namespace {

struct Contribution {
  std::uint32_t first = 0;      // first source index
  std::uint32_t count = 0;      // number of weights
  std::uint32_t offset = 0;     // into the weight array
};

struct Contributions {
  std::vector<Contribution> list;
  std::vector<float> weights;
};

double sinc(double x) {
  if (std::abs(x) < 1e-9) return 1.0;
  const double px = std::numbers::pi * x;
  return std::sin(px) / px;
}

double kernel(ResampleFilter filter, double x) {
  x = std::abs(x);
  switch (filter) {
    case ResampleFilter::Triangle: return x < 1.0 ? 1.0 - x : 0.0;
    case ResampleFilter::Mitchell: {
      constexpr double B = 1.0 / 3.0, C = 1.0 / 3.0;
      if (x < 1.0) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6.0;
      if (x < 2.0) {
        return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6.0;
      }
      return 0.0;
    }
    case ResampleFilter::Lanczos3:
    case ResampleFilter::Auto: return x < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
    default: return x < 0.5 ? 1.0 : 0.0;
  }
}

double kernel_radius(ResampleFilter filter) {
  switch (filter) {
    case ResampleFilter::Triangle: return 1.0;
    case ResampleFilter::Mitchell: return 2.0;
    case ResampleFilter::Lanczos3:
    case ResampleFilter::Auto: return 3.0;
    default: return 0.5;
  }
}

Contributions make_contributions(std::uint32_t source, std::uint32_t target, ResampleFilter filter) {
  Contributions out;
  out.list.resize(target);
  const double scale = static_cast<double>(target) / source;  // > 1 enlarges
  const double inv = static_cast<double>(source) / target;
  std::vector<double> temp;
  for (std::uint32_t i = 0; i < target; ++i) {
    const double center = (i + 0.5) * inv;  // in source pixel-edge coordinates
    Contribution& c = out.list[i];
    c.offset = static_cast<std::uint32_t>(out.weights.size());
    if (filter == ResampleFilter::Nearest || (filter == ResampleFilter::Box && scale >= 1.0)) {
      const auto index = static_cast<std::uint32_t>(std::min<double>(std::floor(center), source - 1.0));
      c.first = index;
      c.count = 1;
      out.weights.push_back(1.0f);
      continue;
    }
    if (filter == ResampleFilter::Box) {
      // Exact area coverage of [i*inv, (i+1)*inv).
      const double lo = i * inv;
      const double hi = (i + 1) * inv;
      const auto first = static_cast<std::uint32_t>(std::floor(lo));
      const auto last = std::min<std::uint32_t>(source - 1, static_cast<std::uint32_t>(std::ceil(hi) - 1));
      c.first = first;
      temp.clear();
      double sum = 0.0;
      for (std::uint32_t s = first; s <= last; ++s) {
        const double w = std::min<double>(hi, s + 1.0) - std::max<double>(lo, s);
        temp.push_back(std::max(w, 0.0));
        sum += std::max(w, 0.0);
      }
      c.count = static_cast<std::uint32_t>(temp.size());
      for (const double w : temp) out.weights.push_back(static_cast<float>(w / sum));
      continue;
    }
    const double support_scale = scale < 1.0 ? inv : 1.0;  // widen the kernel when shrinking
    const double radius = kernel_radius(filter) * support_scale;
    const double src_center = center - 0.5;  // in source pixel-centre coordinates
    const auto lo = static_cast<std::int64_t>(std::floor(src_center - radius));
    const auto hi = static_cast<std::int64_t>(std::ceil(src_center + radius));
    // Fold taps that fall outside the image onto the edge pixels (clamp).
    const std::int64_t first = std::max<std::int64_t>(0, lo);
    const std::int64_t last = std::min<std::int64_t>(static_cast<std::int64_t>(source) - 1, hi);
    temp.assign(static_cast<std::size_t>(last - first + 1), 0.0);
    double sum = 0.0;
    for (std::int64_t s = lo; s <= hi; ++s) {
      const double w = kernel(filter, (static_cast<double>(s) - src_center) / support_scale);
      if (w == 0.0) continue;
      const std::int64_t clamped = std::clamp(s, first, last);
      temp[static_cast<std::size_t>(clamped - first)] += w;
      sum += w;
    }
    if (sum == 0.0) {
      temp.assign(1, 1.0);
      sum = 1.0;
    }
    // Trim zero weights at both ends.
    std::size_t begin = 0;
    std::size_t end = temp.size();
    while (begin + 1 < end && temp[begin] == 0.0) ++begin;
    while (end > begin + 1 && temp[end - 1] == 0.0) --end;
    c.first = static_cast<std::uint32_t>(first + static_cast<std::int64_t>(begin));
    c.count = static_cast<std::uint32_t>(end - begin);
    for (std::size_t k = begin; k < end; ++k) out.weights.push_back(static_cast<float>(temp[k] / sum));
  }
  return out;
}

// Generic separable resampler over interleaved 4-channel float rows.
// `load_row(y, out)` fills `src_w * 4` floats of source row y (premultiplied).
// `store_row(y, in)` receives `dst_w * 4` floats of output row y.
template <class Load, class Store>
void resample(std::uint32_t src_w, std::uint32_t src_h, std::uint32_t dst_w, std::uint32_t dst_h,
              ResampleFilter filter, Load&& load_row, Store&& store_row) {
  const Contributions horizontal = make_contributions(src_w, dst_w, filter);
  const Contributions vertical = make_contributions(src_h, dst_h, filter);
  // Only the source rows some output row needs are filtered horizontally.
  std::vector<float> source_row(static_cast<std::size_t>(src_w) * 4);
  std::vector<float> temp(static_cast<std::size_t>(dst_w) * src_h * 4);
  std::vector<char> needed(src_h, 0);
  for (const Contribution& c : vertical.list) {
    for (std::uint32_t k = 0; k < c.count; ++k) needed[c.first + k] = 1;
  }
  for (std::uint32_t y = 0; y < src_h; ++y) {
    if (!needed[y]) continue;
    load_row(y, source_row.data());
    float* out = temp.data() + static_cast<std::size_t>(y) * dst_w * 4;
    for (std::uint32_t x = 0; x < dst_w; ++x) {
      const Contribution& c = horizontal.list[x];
      const float* w = horizontal.weights.data() + c.offset;
      const float* s = source_row.data() + static_cast<std::size_t>(c.first) * 4;
      float r = 0, g = 0, b = 0, a = 0;
      for (std::uint32_t k = 0; k < c.count; ++k, s += 4) {
        r += w[k] * s[0];
        g += w[k] * s[1];
        b += w[k] * s[2];
        a += w[k] * s[3];
      }
      out[x * 4] = r;
      out[x * 4 + 1] = g;
      out[x * 4 + 2] = b;
      out[x * 4 + 3] = a;
    }
  }
  std::vector<float> row(static_cast<std::size_t>(dst_w) * 4);
  for (std::uint32_t y = 0; y < dst_h; ++y) {
    const Contribution& c = vertical.list[y];
    std::fill(row.begin(), row.end(), 0.0f);
    for (std::uint32_t k = 0; k < c.count; ++k) {
      const float w = vertical.weights[c.offset + k];
      const float* s = temp.data() + static_cast<std::size_t>(c.first + k) * dst_w * 4;
      for (std::size_t i = 0; i < row.size(); ++i) row[i] += w * s[i];
    }
    store_row(y, row.data());
  }
}

ResampleFilter effective_filter(ResampleFilter filter) {
  return filter == ResampleFilter::Auto ? ResampleFilter::Lanczos3 : filter;
}

Result<void> check_target(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) return fail(Status::InvalidArgument, "cannot resize to {}x{} pixels", width, height);
  const std::uint64_t bytes = std::uint64_t{width} * height * 16;
  if (width > 0x7FFFFFFFu || height > 0x7FFFFFFFu || bytes > static_cast<std::uint64_t>(SIZE_MAX / 4)) {
    return fail(Status::Limit, "a {}x{} pixel picture does not fit in memory", width, height);
  }
  return {};
}

}  // namespace

Result<ir::Frame> resize(const ir::Frame& frame, std::uint32_t width, std::uint32_t height,
                         const ResizeOptions& options) {
  if (!frame.valid()) return fail(Status::InvalidArgument, "cannot resize an empty frame");
  CVT_TRY(check_target(width, height));
  if (width == frame.width && height == frame.height) return frame;
  const SrgbTables& t = tables();
  const ResampleFilter filter = effective_filter(options.filter);
  ir::Frame out;
  out.width = width;
  out.height = height;
  out.delay_ms = frame.delay_ms;
  out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
  const bool linear = options.linear_light;
  std::array<float, 256> decode{};
  for (std::size_t i = 0; i < 256; ++i) decode[i] = linear ? t.to_linear[i] : static_cast<float>(i) / 255.0f;
  resample(
      frame.width, frame.height, width, height, filter,
      [&](std::uint32_t y, float* row) {
        const std::uint8_t* s = frame.rgba.data() + static_cast<std::size_t>(y) * frame.width * 4;
        for (std::uint32_t x = 0; x < frame.width; ++x, s += 4, row += 4) {
          const float a = s[3] * (1.0f / 255.0f);
          row[0] = decode[s[0]] * a;
          row[1] = decode[s[1]] * a;
          row[2] = decode[s[2]] * a;
          row[3] = a;
        }
      },
      [&](std::uint32_t y, const float* row) {
        std::uint8_t* d = out.rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (std::uint32_t x = 0; x < width; ++x, d += 4, row += 4) {
          const float a = std::clamp(row[3], 0.0f, 1.0f);
          const std::uint8_t alpha = clamp_u8(a * 255.0f);
          if (alpha == 0) {
            d[0] = d[1] = d[2] = d[3] = 0;
            continue;
          }
          const float inv = 1.0f / a;
          for (int c = 0; c < 3; ++c) {
            const float v = std::clamp(row[c] * inv, 0.0f, 1.0f);
            d[c] = linear ? encode8(t, v) : clamp_u8(v * 255.0f);
          }
          d[3] = alpha;
        }
      });
  return out;
}

Result<ir::HdrPixels> resize(const ir::HdrPixels& pixels, std::uint32_t width, std::uint32_t height,
                             const ResizeOptions& options) {
  if (pixels.width == 0 || pixels.height == 0 ||
      pixels.rgba.size() != static_cast<std::size_t>(pixels.width) * pixels.height * 4) {
    return fail(Status::InvalidArgument, "cannot resize empty HDR pixels");
  }
  CVT_TRY(check_target(width, height));
  if (width == pixels.width && height == pixels.height) return pixels;
  ir::HdrPixels out;
  out.width = width;
  out.height = height;
  out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
  resample(
      pixels.width, pixels.height, width, height, effective_filter(options.filter),
      [&](std::uint32_t y, float* row) {
        const float* s = pixels.rgba.data() + static_cast<std::size_t>(y) * pixels.width * 4;
        for (std::uint32_t x = 0; x < pixels.width; ++x, s += 4, row += 4) {
          const float a = std::isfinite(s[3]) ? std::clamp(s[3], 0.0f, 1.0f) : 1.0f;
          for (int c = 0; c < 3; ++c) row[c] = (std::isfinite(s[c]) ? std::max(s[c], 0.0f) : 0.0f) * a;
          row[3] = a;
        }
      },
      [&](std::uint32_t y, const float* row) {
        float* d = out.rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (std::uint32_t x = 0; x < width; ++x, d += 4, row += 4) {
          const float a = std::clamp(row[3], 0.0f, 1.0f);
          const float inv = a > 0.0f ? 1.0f / a : 0.0f;
          for (int c = 0; c < 3; ++c) d[c] = std::max(row[c] * inv, 0.0f);
          d[3] = a;
        }
      });
  return out;
}

Result<void> resize(ir::Image& image, std::uint32_t width, std::uint32_t height, const ResizeOptions& options) {
  const std::uint32_t old_width = image.width();
  const std::uint32_t old_height = image.height();
  if (width != old_width || height != old_height) image.clear_original();  // the encoded file no longer matches
  for (ir::Frame& frame : image.frames) {
    CVT_TRY_ASSIGN(frame, resize(frame, width, height, options));
  }
  if (image.hdr) {
    CVT_TRY_ASSIGN(*image.hdr, resize(*image.hdr, width, height, options));
  }
  if (old_width > 0 && old_height > 0) {
    if (image.dpi_x > 0) image.dpi_x *= static_cast<double>(width) / old_width;
    if (image.dpi_y > 0) image.dpi_y *= static_cast<double>(height) / old_height;
  }
  return {};
}

Result<bool> apply_size_options(ir::Image& image, const Options& values, const ResizeOptions& resize_options) {
  if (!image.valid()) return false;
  const options::Size target = options::resolved_image_size(values, {image.width(), image.height()});
  if (target.width == image.width() && target.height == image.height()) return false;
  CVT_TRY(resize(image, target.width, target.height, resize_options));
  return true;
}

// --------------------------------------------------------------------------- orientation

namespace {

// Generic pixel remap: destination (x, y) takes source pixel `map(x, y)`.
template <class Pixel, class Map>
std::vector<Pixel> remap_pixels(const std::vector<Pixel>& source, std::uint32_t src_w, std::uint32_t dst_w,
                                std::uint32_t dst_h, Map map) {
  std::vector<Pixel> out(source.size());
  for (std::uint32_t y = 0; y < dst_h; ++y) {
    for (std::uint32_t x = 0; x < dst_w; ++x) {
      const auto [sx, sy] = map(x, y);
      const std::size_t s = (static_cast<std::size_t>(sy) * src_w + sx) * 4;
      const std::size_t d = (static_cast<std::size_t>(y) * dst_w + x) * 4;
      out[d] = source[s];
      out[d + 1] = source[s + 1];
      out[d + 2] = source[s + 2];
      out[d + 3] = source[s + 3];
    }
  }
  return out;
}

struct Coord {
  std::uint32_t x;
  std::uint32_t y;
};

template <class Pixel>
std::vector<Pixel> orient(const std::vector<Pixel>& src, std::uint32_t w, std::uint32_t h, int orientation,
                          std::uint32_t& out_w, std::uint32_t& out_h) {
  const bool transpose = orientation >= 5 && orientation <= 8;
  out_w = transpose ? h : w;
  out_h = transpose ? w : h;
  switch (orientation) {
    case 2: return remap_pixels(src, w, out_w, out_h, [&](std::uint32_t x, std::uint32_t y) { return Coord{w - 1 - x, y}; });
    case 3:
      return remap_pixels(src, w, out_w, out_h, [&](std::uint32_t x, std::uint32_t y) { return Coord{w - 1 - x, h - 1 - y}; });
    case 4: return remap_pixels(src, w, out_w, out_h, [&](std::uint32_t x, std::uint32_t y) { return Coord{x, h - 1 - y}; });
    case 5: return remap_pixels(src, w, out_w, out_h, [&](std::uint32_t x, std::uint32_t y) { return Coord{y, x}; });
    case 6: return remap_pixels(src, w, out_w, out_h, [&](std::uint32_t x, std::uint32_t y) { return Coord{y, h - 1 - x}; });
    case 7:
      return remap_pixels(src, w, out_w, out_h, [&](std::uint32_t x, std::uint32_t y) { return Coord{w - 1 - y, h - 1 - x}; });
    case 8: return remap_pixels(src, w, out_w, out_h, [&](std::uint32_t x, std::uint32_t y) { return Coord{w - 1 - y, x}; });
    default: return src;
  }
}

}  // namespace

ir::Frame apply_orientation(const ir::Frame& frame, int orientation) {
  if (orientation < 2 || orientation > 8 || !frame.valid()) return frame;
  ir::Frame out;
  out.delay_ms = frame.delay_ms;
  out.rgba = orient(frame.rgba, frame.width, frame.height, orientation, out.width, out.height);
  return out;
}

ir::HdrPixels apply_orientation(const ir::HdrPixels& pixels, int orientation) {
  if (orientation < 2 || orientation > 8 ||
      pixels.rgba.size() != static_cast<std::size_t>(pixels.width) * pixels.height * 4) {
    return pixels;
  }
  ir::HdrPixels out;
  out.rgba = orient(pixels.rgba, pixels.width, pixels.height, orientation, out.width, out.height);
  return out;
}

void apply_orientation(ir::Image& image, int orientation) {
  if (orientation < 2 || orientation > 8) return;
  image.clear_original();
  for (ir::Frame& frame : image.frames) frame = apply_orientation(frame, orientation);
  if (image.hdr) *image.hdr = apply_orientation(*image.hdr, orientation);
  if (orientation >= 5) std::swap(image.dpi_x, image.dpi_y);
}

ir::Frame rotate(const ir::Frame& frame, int degrees_clockwise) {
  const int d = ((degrees_clockwise % 360) + 360) % 360;
  if (d == 90) return apply_orientation(frame, 6);
  if (d == 180) return apply_orientation(frame, 3);
  if (d == 270) return apply_orientation(frame, 8);
  return frame;
}
ir::Frame flip_horizontal(const ir::Frame& frame) { return apply_orientation(frame, 2); }
ir::Frame flip_vertical(const ir::Frame& frame) { return apply_orientation(frame, 4); }

// --------------------------------------------------------------------------- greyscale and bit depth

void to_grayscale(ir::Frame& frame) noexcept {
  const SrgbTables& t = tables();
  for (std::size_t i = 0; i + 3 < frame.rgba.size(); i += 4) {
    if (frame.rgba[i] == frame.rgba[i + 1] && frame.rgba[i] == frame.rgba[i + 2]) continue;
    const float y = 0.2126f * t.to_linear[frame.rgba[i]] + 0.7152f * t.to_linear[frame.rgba[i + 1]] +
                    0.0722f * t.to_linear[frame.rgba[i + 2]];
    const std::uint8_t v = encode8(t, y);
    frame.rgba[i] = frame.rgba[i + 1] = frame.rgba[i + 2] = v;
  }
}

namespace {

// 8x8 Bayer matrix, values 0..63.
constexpr std::array<std::array<std::uint8_t, 8>, 8> bayer8 = {{
    {0, 32, 8, 40, 2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
}};

inline float bayer_offset(std::uint32_t x, std::uint32_t y) noexcept {
  return (bayer8[y & 7][x & 7] + 0.5f) / 64.0f - 0.5f;
}

}  // namespace

void reduce_bit_depth(ir::Frame& frame, int bits, Dither dither, bool include_alpha) {
  bits = std::clamp(bits, 1, 8);
  if (bits == 8 || !frame.valid()) return;
  const int levels = (1 << bits) - 1;
  const float step = 255.0f / static_cast<float>(levels);
  const std::size_t channels = include_alpha ? 4 : 3;
  const auto quantize_value = [&](float v) {
    const int level = std::clamp(static_cast<int>(std::lround(v / step)), 0, levels);
    return static_cast<std::uint8_t>((level * 255 + levels / 2) / levels);
  };
  if (dither == Dither::FloydSteinberg) {
    const std::size_t w = frame.width;
    std::vector<float> current((w + 2) * 4, 0.0f);
    std::vector<float> next((w + 2) * 4, 0.0f);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
      std::fill(next.begin(), next.end(), 0.0f);
      const bool reverse = (y & 1) != 0;
      for (std::size_t n = 0; n < w; ++n) {
        const std::size_t x = reverse ? w - 1 - n : n;
        const std::size_t at = frame.offset(static_cast<std::uint32_t>(x), y);
        for (std::size_t c = 0; c < channels; ++c) {
          const float wanted = std::clamp(frame.rgba[at + c] + current[(x + 1) * 4 + c], 0.0f, 255.0f);
          const std::uint8_t got = quantize_value(wanted);
          frame.rgba[at + c] = got;
          const float error = wanted - got;
          const std::size_t ahead = reverse ? x : x + 2;
          const std::size_t behind = reverse ? x + 2 : x;
          current[ahead * 4 + c] += error * 7.0f / 16.0f;
          next[behind * 4 + c] += error * 3.0f / 16.0f;
          next[(x + 1) * 4 + c] += error * 5.0f / 16.0f;
          next[ahead * 4 + c] += error * 1.0f / 16.0f;
        }
      }
      std::swap(current, next);
    }
    return;
  }
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      const std::size_t at = frame.offset(x, y);
      const float offset = dither == Dither::Ordered ? bayer_offset(x, y) * step : 0.0f;
      for (std::size_t c = 0; c < channels; ++c) frame.rgba[at + c] = quantize_value(frame.rgba[at + c] + offset);
    }
  }
}

int lossless_bit_depth(const ir::Frame& frame, bool include_alpha) noexcept {
  int best = 1;
  const std::size_t channels = include_alpha ? 4 : 3;
  for (std::size_t i = 0; i + 3 < frame.rgba.size(); i += 4) {
    for (std::size_t c = 0; c < channels; ++c) {
      const std::uint8_t v = frame.rgba[i + c];
      while (best < 8) {
        const int shift = 8 - best;
        const int level = v >> shift;
        const int expanded = (level * 255 + ((1 << best) - 1) / 2) / ((1 << best) - 1);
        if (expanded == v) break;
        best *= 2;
      }
      if (best == 8) return 8;
    }
  }
  return best;
}

// --------------------------------------------------------------------------- quantisation

namespace {

// Perceptual weights of premultiplied R, G, B, A in the distance metric.
constexpr int weight_r = 3;
constexpr int weight_g = 4;
constexpr int weight_b = 2;
constexpr int weight_a = 4;

inline Rgba premultiplied(const std::uint8_t* p) noexcept {
  const std::uint32_t a = p[3];
  if (a == 255) return {p[0], p[1], p[2], 255};
  if (a == 0) return {0, 0, 0, 0};
  return {static_cast<std::uint8_t>((p[0] * a + 127) / 255), static_cast<std::uint8_t>((p[1] * a + 127) / 255),
          static_cast<std::uint8_t>((p[2] * a + 127) / 255), static_cast<std::uint8_t>(a)};
}

inline Rgba unpremultiplied(float r, float g, float b, float a) noexcept {
  const float alpha = std::clamp(a, 0.0f, 255.0f);
  if (alpha < 0.5f) return {0, 0, 0, 0};
  const float scale = 255.0f / alpha;
  return {clamp_u8(r * scale), clamp_u8(g * scale), clamp_u8(b * scale), clamp_u8(alpha)};
}

// Small open-addressing hash map from a 32-bit key to a 32-bit value.
class KeyMap {
 public:
  explicit KeyMap(std::size_t expected) {
    std::size_t capacity = 64;
    while (capacity < expected * 2) capacity *= 2;
    keys_.assign(capacity, 0);
    used_.assign(capacity, 0);
    values_.assign(capacity, 0);
    mask_ = capacity - 1;
  }
  // Returns a pointer to the value; `inserted` tells whether the key is new.
  std::uint32_t* find_or_insert(std::uint32_t key, bool& inserted) {
    if ((size_ + 1) * 2 > keys_.size()) grow();
    std::size_t at = hash(key) & mask_;
    while (true) {
      if (!used_[at]) {
        used_[at] = 1;
        keys_[at] = key;
        ++size_;
        inserted = true;
        return &values_[at];
      }
      if (keys_[at] == key) {
        inserted = false;
        return &values_[at];
      }
      at = (at + 1) & mask_;
    }
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  static std::size_t hash(std::uint32_t key) noexcept {
    std::uint32_t h = key * 0x9E3779B1u;
    h ^= h >> 15;
    return h;
  }
  void grow() {
    std::vector<std::uint32_t> old_keys = std::move(keys_);
    std::vector<std::uint8_t> old_used = std::move(used_);
    std::vector<std::uint32_t> old_values = std::move(values_);
    keys_.assign(old_keys.size() * 2, 0);
    used_.assign(old_keys.size() * 2, 0);
    values_.assign(old_keys.size() * 2, 0);
    mask_ = keys_.size() - 1;
    size_ = 0;
    for (std::size_t i = 0; i < old_keys.size(); ++i) {
      if (!old_used[i]) continue;
      bool inserted = false;
      *find_or_insert(old_keys[i], inserted) = old_values[i];
    }
  }
  std::vector<std::uint32_t> keys_;
  std::vector<std::uint8_t> used_;
  std::vector<std::uint32_t> values_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

inline std::uint32_t exact_key(Rgba c) noexcept {
  return static_cast<std::uint32_t>(c[0]) << 24 | static_cast<std::uint32_t>(c[1]) << 16 |
         static_cast<std::uint32_t>(c[2]) << 8 | c[3];
}

// Weighted colour sums of one histogram bin.
struct Bin {
  double r = 0, g = 0, b = 0, a = 0;  // premultiplied sums
  double count = 0;
};

struct Point {
  float v[4];  // premultiplied, weighted coordinates (sqrt(weight) * value)
  float count;
};

const std::array<float, 4> axis_scale = {std::sqrt(static_cast<float>(weight_r)), std::sqrt(static_cast<float>(weight_g)),
                                         std::sqrt(static_cast<float>(weight_b)), std::sqrt(static_cast<float>(weight_a))};

std::vector<Point> histogram(std::span<const ir::Frame* const> frames, bool keep_alpha) {
  // 5 bits per colour channel, 4 bits of alpha: at most 2^19 bins, each
  // keeping the exact mean of its pixels.
  std::size_t pixels = 0;
  for (const ir::Frame* frame : frames) pixels += frame->rgba.size() / 4;
  KeyMap map(std::min<std::size_t>(pixels, 1u << 16));
  std::vector<Bin> bins;
  for (const ir::Frame* frame : frames) {
    const std::uint8_t* p = frame->rgba.data();
    const std::size_t n = frame->rgba.size() / 4;
    for (std::size_t i = 0; i < n; ++i, p += 4) {
      std::uint8_t px[4] = {p[0], p[1], p[2], p[3]};
      if (!keep_alpha) px[3] = px[3] >= 128 ? 255 : 0;
      if (px[3] == 0) continue;  // transparent pixels get their own entry elsewhere
      const Rgba c = premultiplied(px);
      const std::uint32_t key = static_cast<std::uint32_t>(c[0] >> 3) << 14 | static_cast<std::uint32_t>(c[1] >> 3) << 9 |
                                static_cast<std::uint32_t>(c[2] >> 3) << 4 | static_cast<std::uint32_t>(c[3] >> 4);
      bool inserted = false;
      std::uint32_t* slot = map.find_or_insert(key, inserted);
      if (inserted) {
        *slot = static_cast<std::uint32_t>(bins.size());
        bins.emplace_back();
      }
      Bin& bin = bins[*slot];
      bin.r += c[0];
      bin.g += c[1];
      bin.b += c[2];
      bin.a += c[3];
      bin.count += 1;
    }
  }
  std::vector<Point> points;
  points.reserve(bins.size());
  for (const Bin& bin : bins) {
    Point p{};
    p.v[0] = static_cast<float>(bin.r / bin.count) * axis_scale[0];
    p.v[1] = static_cast<float>(bin.g / bin.count) * axis_scale[1];
    p.v[2] = static_cast<float>(bin.b / bin.count) * axis_scale[2];
    p.v[3] = static_cast<float>(bin.a / bin.count) * axis_scale[3];
    p.count = static_cast<float>(bin.count);
    points.push_back(p);
  }
  // Deterministic order regardless of hash layout.
  std::sort(points.begin(), points.end(), [](const Point& x, const Point& y) {
    for (int c = 0; c < 4; ++c) {
      if (x.v[c] != y.v[c]) return x.v[c] < y.v[c];
    }
    return x.count < y.count;
  });
  return points;
}

struct Box {
  std::size_t begin = 0;
  std::size_t end = 0;
  double error = 0.0;  // total weighted squared error around the mean
  int axis = 0;
  std::size_t split = 0;  // best split position (begin < split < end) along `axis`; 0 = cannot split
};

void analyse_box(std::vector<Point>& points, Box& box) {
  box.split = 0;
  box.error = 0.0;
  if (box.end - box.begin < 2) return;
  double sum[4] = {0, 0, 0, 0}, sq[4] = {0, 0, 0, 0}, n = 0;
  for (std::size_t i = box.begin; i < box.end; ++i) {
    const Point& p = points[i];
    for (int c = 0; c < 4; ++c) {
      sum[c] += p.v[c] * p.count;
      sq[c] += static_cast<double>(p.v[c]) * p.v[c] * p.count;
    }
    n += p.count;
  }
  double best_var = -1.0;
  for (int c = 0; c < 4; ++c) {
    const double var = sq[c] - sum[c] * sum[c] / n;
    box.error += var;
    if (var > best_var) {
      best_var = var;
      box.axis = c;
    }
  }
  if (best_var <= 0.0) return;
  const int axis = box.axis;
  std::sort(points.begin() + static_cast<std::ptrdiff_t>(box.begin), points.begin() + static_cast<std::ptrdiff_t>(box.end),
            [axis](const Point& x, const Point& y) {
              if (x.v[axis] != y.v[axis]) return x.v[axis] < y.v[axis];
              for (int c = 0; c < 4; ++c) {
                if (x.v[c] != y.v[c]) return x.v[c] < y.v[c];
              }
              return x.count < y.count;
            });
  // Choose the split that minimises the summed error of the two halves
  // (computed over all four channels with prefix sums).
  double left_sum[4] = {0, 0, 0, 0}, left_sq[4] = {0, 0, 0, 0}, left_n = 0;
  double best = std::numeric_limits<double>::infinity();
  for (std::size_t i = box.begin; i + 1 < box.end; ++i) {
    const Point& p = points[i];
    for (int c = 0; c < 4; ++c) {
      left_sum[c] += p.v[c] * p.count;
      left_sq[c] += static_cast<double>(p.v[c]) * p.v[c] * p.count;
    }
    left_n += p.count;
    if (points[i + 1].v[axis] == p.v[axis]) continue;
    const double right_n = n - left_n;
    double total = 0.0;
    for (int c = 0; c < 4; ++c) {
      total += left_sq[c] - left_sum[c] * left_sum[c] / left_n;
      const double rs = sum[c] - left_sum[c];
      total += (sq[c] - left_sq[c]) - rs * rs / right_n;
    }
    if (total < best) {
      best = total;
      box.split = i + 1;
    }
  }
}

std::vector<std::array<float, 4>> median_cut(std::vector<Point>& points, std::size_t colors) {
  std::vector<Box> boxes;
  Box first;
  first.begin = 0;
  first.end = points.size();
  analyse_box(points, first);
  boxes.push_back(first);
  while (boxes.size() < colors) {
    std::size_t pick = boxes.size();
    double worst = 0.0;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
      if (boxes[i].split != 0 && boxes[i].error > worst) {
        worst = boxes[i].error;
        pick = i;
      }
    }
    if (pick == boxes.size()) break;
    Box left = boxes[pick];
    Box right = boxes[pick];
    left.end = boxes[pick].split;
    right.begin = boxes[pick].split;
    analyse_box(points, left);
    analyse_box(points, right);
    boxes[pick] = left;
    boxes.push_back(right);
  }
  std::vector<std::array<float, 4>> centres;
  for (const Box& box : boxes) {
    double sum[4] = {0, 0, 0, 0}, n = 0;
    for (std::size_t i = box.begin; i < box.end; ++i) {
      for (int c = 0; c < 4; ++c) sum[c] += points[i].v[c] * points[i].count;
      n += points[i].count;
    }
    if (n <= 0) continue;
    centres.push_back({static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n), static_cast<float>(sum[2] / n),
                       static_cast<float>(sum[3] / n)});
  }
  return centres;
}

inline float dist2(const float* a, const float* b) noexcept {
  const float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2], d3 = a[3] - b[3];
  return d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
}

void kmeans(const std::vector<Point>& points, std::vector<std::array<float, 4>>& centres, int iterations) {
  if (centres.empty()) return;
  std::vector<std::array<double, 5>> acc(centres.size());
  for (int it = 0; it < iterations; ++it) {
    for (auto& a : acc) a = {0, 0, 0, 0, 0};
    for (const Point& p : points) {
      std::size_t best = 0;
      float best_d = std::numeric_limits<float>::infinity();
      for (std::size_t k = 0; k < centres.size(); ++k) {
        const float d = dist2(p.v, centres[k].data());
        if (d < best_d) {
          best_d = d;
          best = k;
        }
      }
      for (int c = 0; c < 4; ++c) acc[best][static_cast<std::size_t>(c)] += static_cast<double>(p.v[c]) * p.count;
      acc[best][4] += p.count;
    }
    bool moved = false;
    for (std::size_t k = 0; k < centres.size(); ++k) {
      if (acc[k][4] <= 0) continue;
      for (std::size_t c = 0; c < 4; ++c) {
        const auto v = static_cast<float>(acc[k][c] / acc[k][4]);
        if (std::abs(v - centres[k][c]) > 0.05f) moved = true;
        centres[k][c] = v;
      }
    }
    if (!moved) break;
  }
}

// Nearest-palette search in the weighted premultiplied space. Entries are
// sorted along the green axis so the scan can stop early.
class Matcher {
 public:
  explicit Matcher(std::span<const Rgba> palette) {
    entries_.reserve(palette.size());
    for (std::size_t i = 0; i < palette.size(); ++i) {
      const Rgba p = premultiplied(palette[i].data());
      Entry e{};
      for (std::size_t c = 0; c < 4; ++c) e.v[c] = p[c] * axis_scale[c];
      e.index = static_cast<std::uint8_t>(i);
      entries_.push_back(e);
    }
    std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.v[1] < b.v[1]; });
  }
  // `v` in weighted premultiplied space.
  [[nodiscard]] std::uint8_t nearest(const float* v) noexcept {
    if (v[3] == opaque_alpha_ && entries_.size() > 8) return nearest_opaque(v);
    return nearest_scan(v);
  }

 private:
  // Opaque queries use a 32x32x32 grid of candidate lists (built lazily):
  // a palette entry is a candidate for a cell when its smallest possible
  // distance to the cell does not exceed the smallest largest distance of
  // any entry, so the exact nearest entry is always among the candidates.
  std::uint8_t nearest_opaque(const float* v) noexcept {
    int cell_index = 0;
    int coords[3];
    for (int c = 0; c < 3; ++c) {
      const float value = v[c] / axis_scale[static_cast<std::size_t>(c)];
      coords[c] = std::clamp(static_cast<int>(value) >> 3, 0, 31);
      cell_index = cell_index * 32 + coords[c];
    }
    if (cells_.empty()) cells_.assign(32 * 32 * 32, Cell{0, 0, false});
    Cell& cell = cells_[static_cast<std::size_t>(cell_index)];
    if (!cell.built) build_cell(cell, coords);
    float best_d = std::numeric_limits<float>::infinity();
    std::uint8_t best = 0;
    for (std::uint32_t k = 0; k < cell.count; ++k) {
      const Entry& e = entries_[candidates_[cell.start + k]];
      const float d = dist2(e.v, v);
      if (d < best_d || (d == best_d && e.index < best)) {
        best_d = d;
        best = e.index;
      }
    }
    return best;
  }

  struct Cell {
    std::uint32_t start;
    std::uint32_t count;
    bool built;
  };

  void build_cell(Cell& cell, const int* coords) {
    float lo[4];
    float hi[4];
    for (int c = 0; c < 3; ++c) {
      lo[c] = static_cast<float>(coords[c] * 8) * axis_scale[static_cast<std::size_t>(c)];
      hi[c] = static_cast<float>(coords[c] * 8 + 8) * axis_scale[static_cast<std::size_t>(c)];
    }
    lo[3] = hi[3] = opaque_alpha_;
    float min_max = std::numeric_limits<float>::infinity();
    std::vector<float> mins(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      float dmin = 0.0f;
      float dmax = 0.0f;
      for (int c = 0; c < 4; ++c) {
        const float x = entries_[i].v[c];
        const float below = lo[c] - x;
        const float above = x - hi[c];
        const float out = std::max({below, above, 0.0f});
        dmin += out * out;
        const float far = std::max(std::abs(x - lo[c]), std::abs(x - hi[c]));
        dmax += far * far;
      }
      mins[i] = dmin;
      min_max = std::min(min_max, dmax);
    }
    cell.start = static_cast<std::uint32_t>(candidates_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (mins[i] <= min_max) candidates_.push_back(static_cast<std::uint16_t>(i));
    }
    cell.count = static_cast<std::uint32_t>(candidates_.size() - cell.start);
    cell.built = true;
  }

 public:
  [[nodiscard]] std::uint8_t nearest_scan(const float* v) const noexcept {
    // Binary search on green, then expand both ways.
    std::size_t lo = 0, hi = entries_.size();
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2;
      if (entries_[mid].v[1] < v[1]) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    float best_d = std::numeric_limits<float>::infinity();
    std::uint8_t best = entries_.empty() ? 0 : entries_[0].index;
    std::size_t up = lo, down = lo;
    bool up_open = true, down_open = true;
    while (up_open || down_open) {
      if (up_open) {
        if (up >= entries_.size()) {
          up_open = false;
        } else {
          const float dg = entries_[up].v[1] - v[1];
          if (dg * dg >= best_d) {
            up_open = false;
          } else {
            const float d = dist2(entries_[up].v, v);
            if (d < best_d || (d == best_d && entries_[up].index < best)) {
              best_d = d;
              best = entries_[up].index;
            }
            ++up;
          }
        }
      }
      if (down_open) {
        if (down == 0) {
          down_open = false;
        } else {
          const float dg = v[1] - entries_[down - 1].v[1];
          if (dg * dg >= best_d) {
            down_open = false;
          } else {
            const float d = dist2(entries_[down - 1].v, v);
            if (d < best_d || (d == best_d && entries_[down - 1].index < best)) {
              best_d = d;
              best = entries_[down - 1].index;
            }
            --down;
          }
        }
      }
    }
    return best;
  }

 private:
  struct Entry {
    float v[4];
    std::uint8_t index;
  };
  std::vector<Entry> entries_;
  float opaque_alpha_ = 255.0f * axis_scale[3];
  std::vector<Cell> cells_;
  std::vector<std::uint16_t> candidates_;
};

}  // namespace

std::uint32_t color_distance(Rgba a, Rgba b) noexcept {
  const Rgba pa = premultiplied(a.data());
  const Rgba pb = premultiplied(b.data());
  const int weights[4] = {weight_r, weight_g, weight_b, weight_a};
  std::uint32_t sum = 0;
  for (std::size_t c = 0; c < 4; ++c) {
    const int d = static_cast<int>(pa[c]) - static_cast<int>(pb[c]);
    sum += static_cast<std::uint32_t>(weights[c] * d * d);
  }
  return sum;
}

ir::Frame Quantized::to_frame() const {
  ir::Frame frame;
  frame.width = width;
  frame.height = height;
  frame.rgba.resize(indices.size() * 4);
  for (std::size_t i = 0; i < indices.size(); ++i) {
    const Rgba c = indices[i] < palette.size() ? palette[indices[i]] : Rgba{0, 0, 0, 0};
    ::cvt::util::copy_bytes(frame.rgba.data() + i * 4, c.data(), 4);
  }
  return frame;
}

std::optional<Quantized> exact_palette(const ir::Frame& frame, int max_colors) {
  if (!frame.valid()) return std::nullopt;
  max_colors = std::clamp(max_colors, 1, 256);
  Quantized q;
  q.width = frame.width;
  q.height = frame.height;
  q.indices.resize(frame.rgba.size() / 4);
  KeyMap map(64);
  for (std::size_t i = 0; i < q.indices.size(); ++i) {
    const std::uint8_t* p = frame.rgba.data() + i * 4;
    const Rgba c = p[3] == 0 ? Rgba{0, 0, 0, 0} : Rgba{p[0], p[1], p[2], p[3]};
    bool inserted = false;
    std::uint32_t* slot = map.find_or_insert(exact_key(c), inserted);
    if (inserted) {
      if (q.palette.size() >= static_cast<std::size_t>(max_colors)) return std::nullopt;
      *slot = static_cast<std::uint32_t>(q.palette.size());
      if (c[3] == 0) q.transparent_index = static_cast<int>(q.palette.size());
      q.palette.push_back(c);
    }
    q.indices[i] = static_cast<std::uint8_t>(*slot);
  }
  return q;
}

std::vector<Rgba> build_palette(std::span<const ir::Frame* const> frames, const QuantizeOptions& options) {
  const int max_colors = std::clamp(options.max_colors, 2, 256);
  bool any_transparent = false;
  for (const ir::Frame* frame : frames) {
    for (std::size_t i = 3; i < frame->rgba.size(); i += 4) {
      if (frame->rgba[i] == 0 || (!options.keep_alpha && frame->rgba[i] < 128)) {
        any_transparent = true;
        break;
      }
    }
    if (any_transparent) break;
  }
  std::vector<Point> points = histogram(frames, options.keep_alpha);
  std::vector<Rgba> palette;
  bool need_transparent = any_transparent;
  if (options.reserved) {
    const Rgba reserved = (*options.reserved)[3] == 0 ? Rgba{0, 0, 0, 0} : *options.reserved;
    palette.push_back(reserved);
    if (reserved[3] == 0) need_transparent = false;
  }
  if (need_transparent) palette.push_back({0, 0, 0, 0});
  const std::size_t colors = std::max<std::size_t>(1, static_cast<std::size_t>(max_colors) - palette.size());
  if (!points.empty()) {
    std::vector<std::array<float, 4>> centres = median_cut(points, std::min<std::size_t>(colors, points.size()));
    kmeans(points, centres, std::clamp(options.refine_iterations, 0, 32));
    for (const auto& c : centres) {
      Rgba color = unpremultiplied(c[0] / axis_scale[0], c[1] / axis_scale[1], c[2] / axis_scale[2], c[3] / axis_scale[3]);
      if (!options.keep_alpha) color[3] = 255;
      if (color[3] == 0) continue;
      if (std::find(palette.begin(), palette.end(), color) == palette.end()) palette.push_back(color);
      if (palette.size() >= static_cast<std::size_t>(max_colors)) break;
    }
  }
  if (palette.empty()) palette.push_back({0, 0, 0, 0});
  return palette;
}

Result<Quantized> remap(const ir::Frame& frame, std::span<const Rgba> palette, const QuantizeOptions& options) {
  if (!frame.valid()) return fail(Status::InvalidArgument, "cannot quantise an empty frame");
  if (palette.empty() || palette.size() > 256) return fail(Status::InvalidArgument, "a palette needs 1..256 colours");
  Quantized q;
  q.width = frame.width;
  q.height = frame.height;
  q.palette.assign(palette.begin(), palette.end());
  for (std::size_t i = 0; i < palette.size(); ++i) {
    if (palette[i][3] == 0) {
      q.transparent_index = static_cast<int>(i);
      break;
    }
  }
  q.indices.resize(static_cast<std::size_t>(frame.width) * frame.height);
  Matcher matcher(palette);
  std::vector<std::array<float, 4>> weighted(palette.size());
  for (std::size_t i = 0; i < palette.size(); ++i) {
    const Rgba p = premultiplied(palette[i].data());
    for (std::size_t c = 0; c < 4; ++c) weighted[i][c] = p[c];
  }
  const float strength = std::clamp(options.dither_strength, 0.0f, 1.0f);
  const Dither dither = strength <= 0.0f ? Dither::None : options.dither;
  const float spread = 255.0f / std::cbrt(static_cast<float>(palette.size())) * 0.75f * strength;
  // Cache of exact colour -> index for undithered lookups.
  KeyMap cache(1024);
  const std::size_t w = frame.width;
  std::vector<float> current((w + 2) * 4, 0.0f);
  std::vector<float> next((w + 2) * 4, 0.0f);
  double squared_error = 0.0;
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    const bool reverse = dither == Dither::FloydSteinberg && (y & 1) != 0;
    if (dither == Dither::FloydSteinberg) std::fill(next.begin(), next.end(), 0.0f);
    for (std::size_t n = 0; n < w; ++n) {
      const std::size_t x = reverse ? w - 1 - n : n;
      const std::uint8_t* p = frame.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      std::uint8_t px[4] = {p[0], p[1], p[2], p[3]};
      if (!options.keep_alpha) px[3] = px[3] >= 128 ? 255 : 0;
      std::uint8_t index = 0;
      if (px[3] == 0 && q.transparent_index >= 0) {
        index = static_cast<std::uint8_t>(q.transparent_index);
        q.indices[static_cast<std::size_t>(y) * w + x] = index;
        continue;
      }
      const Rgba pm = premultiplied(px);
      float target[4] = {static_cast<float>(pm[0]), static_cast<float>(pm[1]), static_cast<float>(pm[2]),
                         static_cast<float>(pm[3])};
      if (dither == Dither::FloydSteinberg) {
        for (std::size_t c = 0; c < 4; ++c) {
          target[c] = std::clamp(target[c] + current[(x + 1) * 4 + c], 0.0f, 255.0f);
        }
        if (!options.keep_alpha) target[3] = static_cast<float>(pm[3]);
        // Premultiplied colour cannot exceed alpha.
        for (std::size_t c = 0; c < 3; ++c) target[c] = std::min(target[c], target[3]);
      } else if (dither == Dither::Ordered) {
        const float offset = bayer_offset(static_cast<std::uint32_t>(x), y) * spread * (pm[3] / 255.0f);
        for (std::size_t c = 0; c < 3; ++c) target[c] = std::clamp(target[c] + offset, 0.0f, target[3]);
      }
      const bool exact = dither == Dither::None ||
                         (dither == Dither::FloydSteinberg && target[0] == pm[0] && target[1] == pm[1] &&
                          target[2] == pm[2] && target[3] == pm[3]);
      if (exact) {
        bool inserted = false;
        std::uint32_t* slot = cache.find_or_insert(exact_key(pm), inserted);
        if (inserted) {
          float v[4];
          for (std::size_t c = 0; c < 4; ++c) v[c] = target[c] * axis_scale[c];
          *slot = matcher.nearest(v);
        }
        index = static_cast<std::uint8_t>(*slot);
      } else {
        float v[4];
        for (std::size_t c = 0; c < 4; ++c) v[c] = target[c] * axis_scale[c];
        index = matcher.nearest(v);
      }
      q.indices[static_cast<std::size_t>(y) * w + x] = index;
      const auto& chosen = weighted[index];
      for (std::size_t c = 0; c < 4; ++c) {
        const float d = static_cast<float>(pm[c]) - chosen[c];
        squared_error += static_cast<double>(d) * d;
      }
      if (dither == Dither::FloydSteinberg) {
        const std::size_t ahead = reverse ? x : x + 2;
        const std::size_t behind = reverse ? x + 2 : x;
        for (std::size_t c = 0; c < 4; ++c) {
          // Damp the diffused error slightly to avoid "worms" and runaway accumulation.
          const float error = std::clamp((target[c] - chosen[c]) * strength * 0.875f, -96.0f, 96.0f);
          current[ahead * 4 + c] += error * (7.0f / 16.0f);
          next[behind * 4 + c] += error * (3.0f / 16.0f);
          next[(x + 1) * 4 + c] += error * (5.0f / 16.0f);
          next[ahead * 4 + c] += error * (1.0f / 16.0f);
        }
      }
    }
    if (dither == Dither::FloydSteinberg) std::swap(current, next);
  }
  q.mse = squared_error / (static_cast<double>(q.indices.size()) * 4.0);
  return q;
}

Result<Quantized> quantize(const ir::Frame& frame, const QuantizeOptions& options) {
  if (!frame.valid()) return fail(Status::InvalidArgument, "cannot quantise an empty frame");
  const int max_colors = std::clamp(options.max_colors, 2, 256);
  if (options.keep_alpha && !options.reserved) {
    if (auto exact = exact_palette(frame, max_colors)) return std::move(*exact);
  }
  const ir::Frame* frames[1] = {&frame};
  const std::vector<Rgba> palette = build_palette(frames, options);
  return remap(frame, palette, options);
}

}  // namespace cvt::image
