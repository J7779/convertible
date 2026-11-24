// Convertible core - font family: glyph outlines and curve conversion.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <vector>

namespace cvt::font {

struct Point {
  double x = 0;
  double y = 0;
  friend bool operator==(const Point&, const Point&) = default;
};

enum class Verb : std::uint8_t { Move, Line, Quad, Cubic, Close };

struct Bounds {
  double x_min = 0, y_min = 0, x_max = 0, y_max = 0;
  bool empty = true;
  void add(Point p) noexcept;
};

/// A glyph outline: closed contours of lines, quadratic and cubic curves, in
/// font units with y up. Every contour starts with Move and ends with Close.
struct Path {
  std::vector<Verb> verbs;
  std::vector<Point> points;  // Move/Line: 1, Quad: 2, Cubic: 3, Close: 0

  void move_to(Point p);
  void line_to(Point p);
  void quad_to(Point c, Point p);
  void cubic_to(Point c1, Point c2, Point p);
  /// Close the current contour (no-op when none is open).
  void close();
  [[nodiscard]] bool empty() const noexcept { return verbs.empty(); }
  [[nodiscard]] std::size_t contour_count() const noexcept;
  /// Bounds of all on- and off-curve points (control box).
  [[nodiscard]] Bounds control_bounds() const noexcept;
  /// Exact bounds of the curves (extrema of quadratic and cubic segments).
  [[nodiscard]] Bounds exact_bounds() const noexcept;
  /// Apply x' = xx*x + yx*y + dx, y' = xy*x + yy*y + dy.
  void transform(double xx, double xy, double yx, double yy, double dx, double dy);
  void append(const Path& other);
  /// The same outline with every contour reversed.
  [[nodiscard]] Path reversed() const;
  /// Every quadratic curve as the exactly equivalent cubic.
  [[nodiscard]] Path to_cubic() const;
  /// Every cubic as quadratic splines within `tolerance` font units.
  [[nodiscard]] Path to_quadratic(double tolerance) const;
};

/// Approximate one cubic by `count` or more quadratics within `tolerance`;
/// appends (control, end) pairs to `out`. Returns the number of pieces.
std::size_t cubic_to_quadratics(Point p0, Point p1, Point p2, Point p3, double tolerance, std::vector<Point>& out);

/// Largest distance between corresponding samples of two outlines with the
/// same contour structure after flattening, or a large value when the
/// structures differ. Used by tests and by the converters' self-checks.
[[nodiscard]] double outline_distance(const Path& a, const Path& b);

}  // namespace cvt::font
