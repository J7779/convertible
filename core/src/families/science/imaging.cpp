// Convertible core - science family: numeric slices to pictures.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/imaging.hpp"

#include <algorithm>
#include <cmath>

namespace cvt::science::imaging {

std::vector<OptionSpec> window_options(std::string_view default_stretch, double default_clip) {
  return {
      OptionSpec::choice("stretch", "Intensity stretch", std::string(default_stretch), {"linear", "sqrt", "log", "asinh"},
                         "How data values map to grey levels; non-linear stretches show faint detail."),
      OptionSpec::number("clip_percent", "Clip percent", default_clip, 0.0, 25.0,
                         "Ignore this percentage of the darkest and brightest values when choosing the range."),
      OptionSpec::number("window_low", "Window low", 0.0, -1e300, 1e300,
                         "Data value shown as black; with window high, overrides the automatic range (both 0 = automatic).")
          .advanced(),
      OptionSpec::number("window_high", "Window high", 0.0, -1e300, 1e300, "Data value shown as white.").advanced(),
  };
}

Window window_from_options(const Options& options) {
  Window w;
  w.stretch = options.get_string("stretch", "linear");
  w.clip_percent = options.get_float("clip_percent", 0.0);
  const double lo = options.get_float("window_low", 0.0);
  const double hi = options.get_float("window_high", 0.0);
  if (std::isfinite(lo) && std::isfinite(hi) && hi > lo) {
    w.low = lo;
    w.high = hi;
  }
  return w;
}

std::pair<double, double> value_range(const std::vector<double>& values, const Window& window) {
  if (window.low && window.high) return {*window.low, *window.high};
  std::vector<double> finite;
  finite.reserve(values.size());
  for (const double v : values) {
    if (std::isfinite(v)) finite.push_back(v);
  }
  if (finite.empty()) return {0.0, 1.0};
  if (window.clip_percent <= 0.0) {
    const auto [lo, hi] = std::minmax_element(finite.begin(), finite.end());
    return {*lo, *hi};
  }
  const auto pick = [&finite](double fraction) {
    const std::size_t k = std::min(finite.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(finite.size() - 1) + 0.5));
    std::nth_element(finite.begin(), finite.begin() + static_cast<std::ptrdiff_t>(k), finite.end());
    return finite[k];
  };
  const double lo = pick(window.clip_percent / 100.0);
  const double hi = pick(1.0 - window.clip_percent / 100.0);
  return {lo, hi};
}

ir::Frame render(const std::vector<double>& values, std::uint32_t width, std::uint32_t height, const Window& window,
                 std::pair<double, double> range) {
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  const double lo = range.first;
  const double span = range.second > range.first ? range.second - range.first : 1.0;
  const auto curve = [&window](double t) {
    t = std::clamp(t, 0.0, 1.0);
    if (window.stretch == "sqrt") return std::sqrt(t);
    if (window.stretch == "log") return std::log1p(1000.0 * t) / std::log1p(1000.0);
    if (window.stretch == "asinh") return std::asinh(10.0 * t) / std::asinh(10.0);
    return t;
  };
  for (std::size_t i = 0; i < values.size() && i < std::size_t{width} * height; ++i) {
    const double v = values[i];
    const std::size_t at = i * 4;
    if (!std::isfinite(v)) {
      frame.rgba[at + 3] = 0;
      continue;
    }
    double t = curve((v - lo) / span);
    if (window.invert) t = 1.0 - t;
    const auto g = static_cast<std::uint8_t>(std::lround(t * 255.0));
    frame.rgba[at] = frame.rgba[at + 1] = frame.rgba[at + 2] = g;
  }
  return frame;
}

double luma(const ir::Frame& frame, std::size_t offset) {
  return 0.2126 * frame.rgba[offset] + 0.7152 * frame.rgba[offset + 1] + 0.0722 * frame.rgba[offset + 2];
}

bool is_grey(const ir::Frame& frame) {
  for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
    if (frame.rgba[i] != frame.rgba[i + 1] || frame.rgba[i] != frame.rgba[i + 2]) return false;
  }
  return true;
}

Result<ir::Image> array_to_image(const arrays::Array& array, const Window& window, bool flip_vertical, const Context& ctx) {
  using arrays::ScalarKind;
  if (array.dtype.kind == ScalarKind::Record || array.dtype.kind == ScalarKind::Bytes ||
      array.dtype.kind == ScalarKind::Unicode) {
    return fail(Status::Decode, "'{}' is not a numeric array", array.name);
  }
  std::vector<std::uint64_t> shape = array.shape;
  while (shape.size() > 2 && shape.front() == 1) shape.erase(shape.begin());
  if (shape.size() < 2) return fail(Status::Decode, "'{}' has fewer than two dimensions", array.name);
  const bool rgb = shape.size() == 3 && (shape.back() == 3 || shape.back() == 4) && shape[0] > 4;
  const std::uint64_t height = rgb ? shape[0] : shape[shape.size() - 2];
  const std::uint64_t width = rgb ? shape[1] : shape.back();
  const std::uint64_t frames = rgb ? 1 : array.count() / std::max<std::uint64_t>(1, width * height);
  if (width == 0 || height == 0 || width > 0xFFFFFFFFu || height > 0xFFFFFFFFu) return fail(Status::Decode, "empty image");
  CVT_TRY(ctx.limits.check_pixels(width, height, frames));
  std::vector<double> values(static_cast<std::size_t>(array.count()));
  for (std::uint64_t i = 0; i < array.count(); ++i) values[static_cast<std::size_t>(i)] = array.number(i);
  ir::Image image;
  // One range for the whole stack, so slices are comparable.
  const auto range = value_range(values, window);
  const auto w = static_cast<std::uint32_t>(width);
  const auto h = static_cast<std::uint32_t>(height);
  if (rgb) {
    const std::uint64_t channels = shape.back();
    ir::Frame frame = ir::Frame::filled(w, h, {0, 0, 0, 255});
    const bool bytes = array.dtype.kind == ScalarKind::UInt && array.dtype.size == 1;
    for (std::uint64_t p = 0; p < width * height; ++p) {
      for (std::uint64_t c = 0; c < channels; ++c) {
        double v = values[static_cast<std::size_t>(p * channels + c)];
        if (!bytes) v = (v - range.first) / std::max(1e-300, range.second - range.first) * 255.0;
        frame.rgba[static_cast<std::size_t>(p * 4 + c)] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
      }
    }
    image.frames.push_back(std::move(frame));
  } else {
    for (std::uint64_t f = 0; f < frames; ++f) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(f) / static_cast<double>(frames)));
      std::vector<double> slice(values.begin() + static_cast<std::ptrdiff_t>(f * width * height),
                                values.begin() + static_cast<std::ptrdiff_t>((f + 1) * width * height));
      image.frames.push_back(render(slice, w, h, window, range));
    }
  }
  if (flip_vertical) {
    for (ir::Frame& frame : image.frames) {
      for (std::uint32_t y = 0; y < h / 2; ++y) {
        std::swap_ranges(frame.rgba.begin() + static_cast<std::ptrdiff_t>(frame.offset(0, y)),
                         frame.rgba.begin() + static_cast<std::ptrdiff_t>(frame.offset(0, y) + std::size_t{w} * 4),
                         frame.rgba.begin() + static_cast<std::ptrdiff_t>(frame.offset(0, h - 1 - y)));
      }
    }
  }
  return image;
}

}  // namespace cvt::science::imaging
