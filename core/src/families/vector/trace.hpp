// Convertible core - vector family: raster pictures into vector scenes.
//
// Two ways: embed the picture unchanged as an image element (exact), or
// trace it into filled paths: colour quantisation (image_ops), contour
// tracing on the pixel grid, polygon simplification, corner detection and
// least-squares Bézier fitting within a tolerance. Black-and-white mode
// thresholds the picture into one layer; colour mode traces one layer per
// palette colour, stacked so that no gaps appear between regions.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "ir/image.hpp"

namespace cvt::vector {

struct TraceOptions {
  enum class Mode : std::uint8_t { Embed, BlackWhite, Color };
  Mode mode = Mode::Embed;
  /// Colour mode: palette size 2..64.
  int colors = 8;
  /// Black-and-white mode: luminance threshold 0..255 (darker = black).
  int threshold = 128;
  /// Curve-fitting tolerance in pixels (larger = fewer nodes).
  double tolerance = 1.0;
  /// Regions smaller than this many pixels are dropped.
  int speckle = 4;
  /// Turns of at least this many degrees stay corners.
  double corner_angle = 60.0;
};

/// The picture as a one-page scene: embedded or traced.
[[nodiscard]] Result<Scene> image_to_scene(const ir::Image& image, const TraceOptions& options, const Context& ctx);

}  // namespace cvt::vector
