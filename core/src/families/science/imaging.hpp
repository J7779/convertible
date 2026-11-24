// Convertible core - science family: turning numeric 2-D slices into 8-bit
// pictures (intensity windows, percentile clipping, non-linear stretches)
// for FITS, NRRD, MetaImage, NIfTI/Analyze and DICOM.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "families/science/arrays.hpp"

namespace cvt::science::imaging {

struct Window {
  std::string stretch = "linear";  ///< linear, sqrt, log, asinh
  double clip_percent = 0.0;       ///< drop this percentage at each end before scaling
  std::optional<double> low;       ///< explicit window (overrides percentiles)
  std::optional<double> high;
  bool invert = false;             ///< MONOCHROME1-style: high values dark
};

/// The standard options that drive `window_from_options`.
[[nodiscard]] std::vector<OptionSpec> window_options(std::string_view default_stretch, double default_clip);
[[nodiscard]] Window window_from_options(const Options& options);

/// Compute the [low, high] range of `values` (NaN ignored) for a window.
[[nodiscard]] std::pair<double, double> value_range(const std::vector<double>& values, const Window& window);

/// Render `width * height` values (row-major, top row first) to a grey frame.
/// NaN values become transparent.
[[nodiscard]] ir::Frame render(const std::vector<double>& values, std::uint32_t width, std::uint32_t height,
                               const Window& window, std::pair<double, double> range);

/// Grey value of a pixel (Rec. 709 luma of sRGB, alpha ignored).
[[nodiscard]] double luma(const ir::Frame& frame, std::size_t offset);
/// True when every pixel has R == G == B.
[[nodiscard]] bool is_grey(const ir::Frame& frame);

/// Slices of an array as images: shape [.., height, width] gives one frame
/// per leading index; shape [h, w, 3|4] with `rgb_last` gives colour.
[[nodiscard]] Result<ir::Image> array_to_image(const arrays::Array& array, const Window& window, bool flip_vertical,
                                              const Context& ctx);

}  // namespace cvt::science::imaging
