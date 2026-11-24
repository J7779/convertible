// Convertible core - pdf family: anti-aliased scanline rasteriser and stroker.
//
// Everything here works in DEVICE space: pixel (x, y) covers the square
// [x, x+1) x [y, y+1), y grows downwards. Callers transform paths with the
// current transformation matrix (including the page flip) before filling.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "families/pdf/geom.hpp"

namespace cvt::pdf::raster {

enum class FillRule : std::uint8_t { NonZero, EvenOdd };

/// Integer pixel rectangle [x0, x1) x [y0, y1).
struct IntRect {
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  [[nodiscard]] bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
  [[nodiscard]] int width() const noexcept { return x1 - x0; }
  [[nodiscard]] int height() const noexcept { return y1 - y0; }
  [[nodiscard]] IntRect intersect(const IntRect& o) const noexcept;
  friend bool operator==(const IntRect&, const IntRect&) = default;
};

/// 8-bit coverage over a pixel rectangle (`box`); everything outside the
/// box has coverage 0, except for a mask with `everywhere` set, which covers
/// every pixel fully (the "no clipping" mask).
struct Mask {
  IntRect box;
  std::vector<std::uint8_t> coverage;  ///< box.width() * box.height(), row-major
  bool everywhere = false;

  [[nodiscard]] static Mask full() {
    Mask m;
    m.everywhere = true;
    return m;
  }
  [[nodiscard]] bool empty() const noexcept { return !everywhere && box.empty(); }
  [[nodiscard]] std::uint8_t at(int x, int y) const noexcept {
    if (everywhere) return 255;
    if (x < box.x0 || y < box.y0 || x >= box.x1 || y >= box.y1) return 0;
    return coverage[static_cast<std::size_t>(y - box.y0) * static_cast<std::size_t>(box.width()) +
                    static_cast<std::size_t>(x - box.x0)];
  }
  /// Pointer to the coverage of row y starting at box.x0 (box must contain y).
  [[nodiscard]] const std::uint8_t* row(int y) const noexcept {
    return coverage.data() + static_cast<std::size_t>(y - box.y0) * static_cast<std::size_t>(box.width());
  }
};

/// Fill `path` (device space) with the given rule, clipped to `clip_box`
/// (normally the page bitmap). With `antialias` false, coverage is 0 or 255
/// (pixel centre sampling), as used for thin rules and image masks where
/// PDF asks for it. Curves are flattened to within 0.1 pixel. Non-finite
/// coordinates are ignored (the subpath is skipped). Work is bounded by the
/// clipped area and the number of segments.
[[nodiscard]] Mask fill_path(const Path& path, FillRule rule, const IntRect& clip_box, bool antialias = true);

/// Coverage product of two masks (intersection of clip regions).
[[nodiscard]] Mask intersect(const Mask& a, const Mask& b);

enum class LineCap : std::uint8_t { Butt = 0, Round = 1, Square = 2 };
enum class LineJoin : std::uint8_t { Miter = 0, Round = 1, Bevel = 2 };

struct StrokeStyle {
  double width = 1.0;  ///< user-space line width; 0 means the thinnest line the device can draw
  LineCap cap = LineCap::Butt;
  LineJoin join = LineJoin::Miter;
  double miter_limit = 10.0;
  std::vector<double> dash;  ///< user-space dash lengths; empty = solid; all-zero = solid
  double dash_phase = 0.0;
};

/// Outline of the stroke of `user_path` (user space) as a closed DEVICE-space
/// path to be filled with the nonzero rule. `ctm` maps user space to device
/// space; the pen is a circle of diameter `style.width` in user space, so a
/// non-uniform CTM gives an elliptical pen exactly as PDF requires.
/// Widths that map to less than about one device pixel are widened to one
/// pixel (PDF's thin-line rule). Curves are flattened in device space to
/// within 0.1 pixel. Degenerate (zero-length) subpaths draw a dot for round
/// and square caps. Dash patterns follow the PDF rules (phase, odd count
/// repeats, caps on every dash).
[[nodiscard]] Path stroke_outline(const Path& user_path, const StrokeStyle& style, const Matrix& ctm);

/// Flatten curves to line segments within `tolerance` (same units as the
/// path). Close ops are kept; the result has only Move, Line and Close.
[[nodiscard]] Path flatten(const Path& path, double tolerance);

}  // namespace cvt::pdf::raster
