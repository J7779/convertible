// Convertible core - vector family: the anti-aliased scanline rasteriser.
//
// Renders a scene into 8-bit sRGB RGBA: exact-area coverage per pixel
// (non-zero and even-odd), solid colours, linear/radial gradients with
// spread modes, patterns, pictures (resampled), clipping, masks, group
// opacity, blend modes and the common SVG filters. Compositing happens on
// premultiplied floating-point pixels in sRGB space, as SVG specifies
// (linear light only where `color-interpolation` asks for it).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "ir/image.hpp"

namespace cvt::vector {

struct RenderOptions {
  /// Output size in pixels; 0 = from the page size and `dpi`. When only one
  /// is given the other follows the page's aspect ratio.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// Resolution for the page size (96 = one pixel per CSS px).
  double dpi = 96.0;
  /// Colour under the drawing; nullopt = transparent.
  std::optional<Color> background;
  /// Zero-based page index.
  std::size_t page = 0;
};

/// Rasterise one page. Fails with Status::Limit when the canvas exceeds
/// ctx.limits, Status::Decode when the page does not exist.
[[nodiscard]] Result<ir::Frame> render_page(const Scene& scene, const RenderOptions& options, const Context& ctx);

/// Rasterise a group onto a transparent canvas of the given size with an
/// explicit transform (user space -> pixels).
[[nodiscard]] Result<ir::Frame> render_group(const Group& group, const Matrix& transform, std::uint32_t width,
                                             std::uint32_t height, const Context& ctx);

}  // namespace cvt::vector
