// Convertible desktop - vector paths: building, SVG path data, flattening,
// measuring/trimming and stroking into fillable polygons.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "base/geometry.hpp"

namespace cvd::gfx {

/// A flattened contour (polyline). `closed` contours connect last to first.
struct Contour {
  std::vector<Vec2> points;
  bool closed = false;
};

class Path {
 public:
  enum class Verb { Move, Line, Quad, Cubic, Close };

  Path& move_to(Vec2 p);
  Path& line_to(Vec2 p);
  Path& quad_to(Vec2 c, Vec2 p);
  Path& cubic_to(Vec2 c1, Vec2 c2, Vec2 p);
  /// SVG elliptical arc to `p` (radii, x-axis rotation in degrees, flags).
  Path& arc_to(float rx, float ry, float rotation_deg, bool large_arc, bool sweep, Vec2 p);
  Path& close();

  Path& move_to(float x, float y) { return move_to({x, y}); }
  Path& line_to(float x, float y) { return line_to({x, y}); }

  Path& add_rect(const Rect& r);
  /// Rounded rectangle; radius is clamped to half the shorter side.
  Path& add_rrect(const Rect& r, float radius);
  Path& add_circle(Vec2 c, float radius);
  Path& add_ellipse(Vec2 c, float rx, float ry);

  /// Parses SVG path data (M L H V C S Q T A Z, absolute and relative).
  static std::optional<Path> parse_svg(std::string_view d);

  [[nodiscard]] bool empty() const { return verbs_.empty(); }
  [[nodiscard]] const std::vector<Verb>& verbs() const { return verbs_; }
  [[nodiscard]] const std::vector<Vec2>& points() const { return points_; }

  /// Flattens with the transform applied; `tolerance` is the maximum chord
  /// error in output units.
  [[nodiscard]] std::vector<Contour> flatten(const Affine& t = {}, float tolerance = 0.25f) const;
  /// Bounding box of the control points (conservative).
  [[nodiscard]] Rect bounds() const;

 private:
  std::vector<Verb> verbs_;
  std::vector<Vec2> points_;
  Vec2 start_{};
  Vec2 current_{};
  bool open_ = false;
};

/// Arc-length parametrisation of a polyline, used to trim strokes.
class PolylineMeasure {
 public:
  explicit PolylineMeasure(std::vector<Vec2> points);
  [[nodiscard]] float length() const { return total_; }
  /// The part between two fractions (0..1) of the length.
  [[nodiscard]] std::vector<Vec2> trim(float start_fraction, float end_fraction) const;
  /// Point at a fraction of the length.
  [[nodiscard]] Vec2 point_at(float fraction) const;

 private:
  std::vector<Vec2> pts_;
  std::vector<float> cumulative_;
  float total_ = 0.0f;
};

enum class LineCap { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };

struct StrokeStyle {
  float width = 1.0f;
  LineCap cap = LineCap::Butt;
  LineJoin join = LineJoin::Miter;
  float miter_limit = 4.0f;
};

/// Converts a polyline to a set of convex-ish polygons whose union (with the
/// non-zero rule, all oriented the same way) is the stroke outline.
std::vector<Contour> stroke_polyline(const Contour& line, const StrokeStyle& style);

/// Signed area (positive = clockwise on screen with y down).
float signed_area(const std::vector<Vec2>& poly);

}  // namespace cvd::gfx
