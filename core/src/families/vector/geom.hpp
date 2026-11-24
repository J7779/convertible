// Convertible core - vector family: path geometry (construction, arcs,
// bounds, flattening, dashing and stroking).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <span>
#include <vector>

#include "families/vector/scene.hpp"

namespace cvt::vector {

/// `path` with every point mapped through `m`.
[[nodiscard]] Path transformed(const Path& path, const Matrix& m);
/// Append `src` (mapped through `m`) to `dst`.
void append(Path& dst, const Path& src, const Matrix& m = {});

/// Bounding box of the control points (cheap, may be larger than the curve).
[[nodiscard]] Rect control_bounds(const Path& path);
/// Tight bounding box of the curve (Bézier extrema solved exactly).
[[nodiscard]] Rect bounds(const Path& path);
/// Bounds of `path` after mapping through `m` (tight).
[[nodiscard]] Rect bounds(const Path& path, const Matrix& m);
/// Bounds of a rectangle mapped through `m`.
[[nodiscard]] Rect transform_rect(const Rect& rect, const Matrix& m);

/// Rectangle with optional rounded corners (rx, ry clamped to half the size).
void add_rect(Path& path, double x, double y, double w, double h, double rx = 0.0, double ry = 0.0);
/// Full ellipse as four cubic arcs, starting at (cx + rx, cy), clockwise in y-down space.
void add_ellipse(Path& path, double cx, double cy, double rx, double ry);
/// SVG elliptical arc from `from` (the current point) to `to` (F.6.5),
/// appended as cubic Béziers. Degenerate radii become a line.
void svg_arc_to(Path& path, Point from, double rx, double ry, double x_axis_rotation_degrees, bool large_arc,
                bool sweep, Point to);
/// Arc of an ellipse centred at `center` with radii rx, ry, rotated by
/// `rotation` radians, from angle `start` to angle `end` (radians, measured
/// in the ellipse's own frame from +x towards +y). When `connect`, a line is
/// drawn from the current point to the arc start (PostScript `arc`);
/// otherwise the arc starts a new subpath. The arc goes from start to end
/// in the direction of increasing angle when `positive`, else decreasing.
void add_arc(Path& path, Point center, double rx, double ry, double rotation, double start, double end, bool positive,
             bool connect);

/// The last point of the path (0,0 when empty).
[[nodiscard]] Point current_point(const Path& path);

/// A flattened subpath.
struct Polyline {
  std::vector<Point> points;
  /// Parallel to `points`: true where the point is a vertex of the original
  /// path (a corner where the stroke join applies), false for points
  /// introduced by flattening a curve.
  std::vector<bool> corners;
  bool closed = false;
};

/// Approximate every curve by line segments within `tolerance` (path units).
[[nodiscard]] std::vector<Polyline> flatten(const Path& path, double tolerance);

/// Cut polylines into dashes. `pattern` lengths (even count, all >= 0, sum > 0).
[[nodiscard]] std::vector<Polyline> dash(const std::vector<Polyline>& lines, std::span<const double> pattern,
                                         double offset);

/// The outline of the stroke of `path` as a path to be filled with the
/// non-zero rule. Width, caps, joins, miter limit and dashes come from
/// `stroke`; `tolerance` is the flattening tolerance in path units.
[[nodiscard]] Path stroke_outline(const Path& path, const Stroke& stroke, double tolerance);
/// Same for already flattened (and possibly dashed) polylines.
[[nodiscard]] Path stroke_polylines(const std::vector<Polyline>& lines, const Stroke& stroke, double tolerance);

/// Total length of the flattened path.
[[nodiscard]] double path_length(const Path& path, double tolerance);

/// Signed area of a polygon (positive = counter-clockwise in y-up space).
[[nodiscard]] double signed_area(std::span<const Point> polygon);

/// Number of segments a curve needs for the given flattening tolerance.
[[nodiscard]] int cubic_segments(Point p0, Point p1, Point p2, Point p3, double tolerance);
[[nodiscard]] int quad_segments(Point p0, Point p1, Point p2, double tolerance);

[[nodiscard]] Point eval_quad(Point p0, Point p1, Point p2, double t);
[[nodiscard]] Point eval_cubic(Point p0, Point p1, Point p2, Point p3, double t);

}  // namespace cvt::vector
