// Convertible core - pdf family: 2-D geometry shared by the font programs,
// the rasteriser, the content interpreter and the writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace cvt::pdf {

struct Point {
  double x = 0.0;
  double y = 0.0;
};

/// An axis-aligned rectangle; empty when x1 <= x0 or y1 <= y0.
struct Rect {
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;

  [[nodiscard]] bool empty() const noexcept { return !(x1 > x0) || !(y1 > y0); }
  [[nodiscard]] double width() const noexcept { return x1 - x0; }
  [[nodiscard]] double height() const noexcept { return y1 - y0; }
  /// Normalised copy (x0 <= x1, y0 <= y1).
  [[nodiscard]] Rect normalized() const noexcept {
    return {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
  }
  [[nodiscard]] Rect intersect(const Rect& o) const noexcept {
    return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
  }
  [[nodiscard]] Rect unite(const Rect& o) const noexcept {
    if (empty()) return o;
    if (o.empty()) return *this;
    return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
  }
  void include(Point p) noexcept {
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
  }
  /// A rectangle that `include` can grow from nothing.
  [[nodiscard]] static Rect inverted_infinite() noexcept {
    constexpr double big = std::numeric_limits<double>::max();
    return {big, big, -big, -big};
  }
};

/// An affine transformation in PDF order: [a b c d e f] maps (x, y) to
/// (a*x + c*y + e, b*x + d*y + f).
struct Matrix {
  double a = 1.0;
  double b = 0.0;
  double c = 0.0;
  double d = 1.0;
  double e = 0.0;
  double f = 0.0;

  [[nodiscard]] Point apply(Point p) const noexcept { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
  /// Apply without the translation (for vectors / distances).
  [[nodiscard]] Point apply_vector(Point p) const noexcept { return {a * p.x + c * p.y, b * p.x + d * p.y}; }

  /// The transformation "first `first`, then `then`". In PDF terms the
  /// operator `cm` with matrix M turns CTM into then(M, CTM) = M x CTM.
  [[nodiscard]] static Matrix concat(const Matrix& first, const Matrix& then) noexcept {
    return {first.a * then.a + first.b * then.c,
            first.a * then.b + first.b * then.d,
            first.c * then.a + first.d * then.c,
            first.c * then.b + first.d * then.d,
            first.e * then.a + first.f * then.c + then.e,
            first.e * then.b + first.f * then.d + then.f};
  }
  [[nodiscard]] static Matrix translate(double tx, double ty) noexcept { return {1, 0, 0, 1, tx, ty}; }
  [[nodiscard]] static Matrix scale(double sx, double sy) noexcept { return {sx, 0, 0, sy, 0, 0}; }

  [[nodiscard]] double determinant() const noexcept { return a * d - b * c; }
  [[nodiscard]] bool invertible() const noexcept {
    const double det = determinant();
    return std::isfinite(det) && std::abs(det) > 1e-300;
  }
  /// Inverse; the identity when not invertible.
  [[nodiscard]] Matrix inverted() const noexcept {
    const double det = determinant();
    if (!std::isfinite(det) || std::abs(det) <= 1e-300) return {};
    const double inv = 1.0 / det;
    return {d * inv, -b * inv, -c * inv, a * inv, (c * f - d * e) * inv, (b * e - a * f) * inv};
  }
  /// Geometric mean scale factor sqrt(|det|): how much lengths grow on average.
  [[nodiscard]] double expansion() const noexcept { return std::sqrt(std::abs(determinant())); }
  /// Largest stretch of a unit vector (spectral norm).
  [[nodiscard]] double max_expansion() const noexcept {
    const double s = a * a + b * b + c * c + d * d;
    const double det = determinant();
    const double disc = std::max(0.0, s * s - 4.0 * det * det);
    return std::sqrt((s + std::sqrt(disc)) * 0.5);
  }
  [[nodiscard]] bool finite() const noexcept {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d) && std::isfinite(e) &&
           std::isfinite(f);
  }
  /// Bounding box of a transformed rectangle.
  [[nodiscard]] Rect apply(const Rect& r) const noexcept {
    Rect out = Rect::inverted_infinite();
    out.include(apply(Point{r.x0, r.y0}));
    out.include(apply(Point{r.x1, r.y0}));
    out.include(apply(Point{r.x0, r.y1}));
    out.include(apply(Point{r.x1, r.y1}));
    return out;
  }
  friend bool operator==(const Matrix&, const Matrix&) = default;
};

enum class PathOp : std::uint8_t {
  Move,   ///< one point
  Line,   ///< one point
  Cubic,  ///< three points: control 1, control 2, end
  Close,  ///< no point; closes the current subpath (back to its start)
};

/// A vector path made of subpaths. Quadratic curves are stored as cubics.
struct Path {
  std::vector<PathOp> ops;
  std::vector<Point> points;

  [[nodiscard]] bool empty() const noexcept { return ops.empty(); }
  void clear() noexcept {
    ops.clear();
    points.clear();
  }
  void move_to(double x, double y) {
    ops.push_back(PathOp::Move);
    points.push_back({x, y});
  }
  void line_to(double x, double y) {
    ops.push_back(PathOp::Line);
    points.push_back({x, y});
  }
  void cubic_to(double x1, double y1, double x2, double y2, double x3, double y3) {
    ops.push_back(PathOp::Cubic);
    points.push_back({x1, y1});
    points.push_back({x2, y2});
    points.push_back({x3, y3});
  }
  /// Quadratic Bezier from the current point (exact cubic conversion).
  void quad_to(double cx, double cy, double x, double y) {
    const Point p0 = points.empty() ? Point{} : points.back();
    cubic_to(p0.x + 2.0 / 3.0 * (cx - p0.x), p0.y + 2.0 / 3.0 * (cy - p0.y), x + 2.0 / 3.0 * (cx - x),
             y + 2.0 / 3.0 * (cy - y), x, y);
  }
  void close() {
    if (!ops.empty() && ops.back() != PathOp::Close) ops.push_back(PathOp::Close);
  }
  void rect(double x, double y, double w, double h) {
    move_to(x, y);
    line_to(x + w, y);
    line_to(x + w, y + h);
    line_to(x, y + h);
    close();
  }
  /// Append another path, optionally transformed.
  void append(const Path& other, const Matrix& m = {}) {
    ops.insert(ops.end(), other.ops.begin(), other.ops.end());
    points.reserve(points.size() + other.points.size());
    for (const Point& p : other.points) points.push_back(m.apply(p));
  }
  void transform(const Matrix& m) noexcept {
    for (Point& p : points) p = m.apply(p);
  }
  [[nodiscard]] Path transformed(const Matrix& m) const {
    Path out = *this;
    out.transform(m);
    return out;
  }
  /// Bounding box of all points (control points included, so it may be a
  /// little larger than the exact curve bounds). Empty Rect when no points.
  [[nodiscard]] Rect bounds() const noexcept {
    if (points.empty()) return {};
    Rect r{points[0].x, points[0].y, points[0].x, points[0].y};
    for (const Point& p : points) r.include(p);
    return r;
  }
};

}  // namespace cvt::pdf
