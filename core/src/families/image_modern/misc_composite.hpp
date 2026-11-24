// Convertible core - image_modern family: layer compositing shared by the
// layered formats (OpenRaster, GIMP XCF).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ir/image.hpp"

namespace cvt::image_modern {

/// Composite `layer` (straight alpha, sRGB-encoded) over `canvas` with the
/// "normal" / source-over operator, the layer's top-left corner placed at
/// (`left`, `top`) of the canvas (may be negative or outside the canvas),
/// its alpha scaled by `opacity` (0..1). Blending happens on the encoded
/// values, as GIMP, Krita and MyPaint do for 8-bit sRGB documents.
inline void composite_normal(ir::Frame& canvas, const ir::Frame& layer, std::int64_t left, std::int64_t top,
                             double opacity) {
  opacity = std::clamp(opacity, 0.0, 1.0);
  if (opacity <= 0.0 || !layer.valid() || !canvas.valid()) return;
  const std::int64_t x0 = std::max<std::int64_t>(0, left);
  const std::int64_t y0 = std::max<std::int64_t>(0, top);
  const std::int64_t x1 = std::min<std::int64_t>(canvas.width, left + layer.width);
  const std::int64_t y1 = std::min<std::int64_t>(canvas.height, top + layer.height);
  for (std::int64_t y = y0; y < y1; ++y) {
    for (std::int64_t x = x0; x < x1; ++x) {
      const std::uint8_t* s = layer.rgba.data() + layer.offset(static_cast<std::uint32_t>(x - left),
                                                              static_cast<std::uint32_t>(y - top));
      std::uint8_t* d = canvas.rgba.data() + canvas.offset(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
      const double sa = s[3] / 255.0 * opacity;
      if (sa <= 0.0) continue;
      const double da = d[3] / 255.0;
      const double out_a = sa + da * (1.0 - sa);
      for (int c = 0; c < 3; ++c) {
        const double v = (s[c] * sa + d[c] * da * (1.0 - sa)) / out_a;
        d[c] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
      }
      d[3] = static_cast<std::uint8_t>(std::clamp(std::lround(out_a * 255.0), 0L, 255L));
    }
  }
}

}  // namespace cvt::image_modern
