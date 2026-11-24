// Convertible core - vector family: the scene model shared by every vector format.
//
// There is no vector IR kind in the engine. Every reader of this family
// produces a `Scene`; every writer consumes one. The scene reaches the engine
// through codecs of existing kinds (see vector_codecs.hpp):
//
//   Image     rasterised by render.hpp
//   Data      the scene serialised losslessly as a tagged JSON envelope
//             (scene_json.hpp); this is how svg -> eps -> dxf ... stay vector
//   Document  text runs extracted from the drawing
//   Mesh      3D faces (DXF)
//
// Coordinates: user units are CSS pixels (1/96 inch), y grows downwards,
// exactly as in SVG. Readers of y-up formats (PostScript, DXF, HPGL, CGM)
// flip into this system; writers flip back.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ir/image.hpp"
#include "ir/mesh.hpp"
#include "util/bytes.hpp"

namespace cvt::vector {

// --------------------------------------------------------------------------- geometry

struct Point {
  double x = 0.0;
  double y = 0.0;
  friend bool operator==(const Point&, const Point&) = default;
};

[[nodiscard]] inline Point operator+(Point a, Point b) noexcept { return {a.x + b.x, a.y + b.y}; }
[[nodiscard]] inline Point operator-(Point a, Point b) noexcept { return {a.x - b.x, a.y - b.y}; }
[[nodiscard]] inline Point operator*(Point a, double s) noexcept { return {a.x * s, a.y * s}; }
[[nodiscard]] inline double dot(Point a, Point b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] inline double cross(Point a, Point b) noexcept { return a.x * b.y - a.y * b.x; }
[[nodiscard]] inline double length(Point a) noexcept { return std::hypot(a.x, a.y); }
[[nodiscard]] inline Point lerp(Point a, Point b, double t) noexcept { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

/// Axis-aligned rectangle. `empty()` when it was never extended.
struct Rect {
  double x0 = 0.0, y0 = 0.0, x1 = -1.0, y1 = -1.0;

  [[nodiscard]] static Rect xywh(double x, double y, double w, double h) noexcept { return {x, y, x + w, y + h}; }
  [[nodiscard]] bool empty() const noexcept { return !(x1 >= x0 && y1 >= y0); }
  [[nodiscard]] double width() const noexcept { return empty() ? 0.0 : x1 - x0; }
  [[nodiscard]] double height() const noexcept { return empty() ? 0.0 : y1 - y0; }
  void add(Point p) noexcept {
    if (empty()) {
      x0 = x1 = p.x;
      y0 = y1 = p.y;
      return;
    }
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
  }
  void add(const Rect& r) noexcept {
    if (r.empty()) return;
    add(Point{r.x0, r.y0});
    add(Point{r.x1, r.y1});
  }
  [[nodiscard]] Rect intersect(const Rect& r) const noexcept {
    if (empty() || r.empty()) return {};
    Rect out{std::max(x0, r.x0), std::max(y0, r.y0), std::min(x1, r.x1), std::min(y1, r.y1)};
    if (out.x1 < out.x0 || out.y1 < out.y0) return {};
    return out;
  }
  friend bool operator==(const Rect&, const Rect&) = default;
};

/// 2D affine transform, SVG/PostScript convention:
///   x' = a*x + c*y + e
///   y' = b*x + d*y + f
struct Matrix {
  double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;

  [[nodiscard]] static Matrix translate(double tx, double ty) noexcept { return {1, 0, 0, 1, tx, ty}; }
  [[nodiscard]] static Matrix scale(double sx, double sy) noexcept { return {sx, 0, 0, sy, 0, 0}; }
  [[nodiscard]] static Matrix rotate(double radians) noexcept {
    const double cs = std::cos(radians), sn = std::sin(radians);
    return {cs, sn, -sn, cs, 0, 0};
  }
  [[nodiscard]] static Matrix skew_x(double radians) noexcept { return {1, 0, std::tan(radians), 1, 0, 0}; }
  [[nodiscard]] static Matrix skew_y(double radians) noexcept { return {1, std::tan(radians), 0, 1, 0, 0}; }

  [[nodiscard]] Point apply(Point p) const noexcept { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
  /// Apply without translation (for vectors).
  [[nodiscard]] Point apply_vector(Point p) const noexcept { return {a * p.x + c * p.y, b * p.x + d * p.y}; }
  /// `this` applied after `inner`: (this * inner)(p) == this(inner(p)).
  [[nodiscard]] Matrix operator*(const Matrix& inner) const noexcept {
    return {a * inner.a + c * inner.b,     b * inner.a + d * inner.b,     a * inner.c + c * inner.d,
            b * inner.c + d * inner.d,     a * inner.e + c * inner.f + e, b * inner.e + d * inner.f + f};
  }
  [[nodiscard]] double determinant() const noexcept { return a * d - b * c; }
  [[nodiscard]] bool invertible() const noexcept {
    const double det = determinant();
    return std::isfinite(det) && std::abs(det) > 1e-14;
  }
  /// Inverse; identity when not invertible.
  [[nodiscard]] Matrix inverted() const noexcept {
    const double det = determinant();
    if (!invertible()) return {};
    const double id = 1.0 / det;
    return {d * id, -b * id, -c * id, a * id, (c * f - d * e) * id, (b * e - a * f) * id};
  }
  [[nodiscard]] bool is_identity(double eps = 1e-12) const noexcept {
    return std::abs(a - 1) <= eps && std::abs(b) <= eps && std::abs(c) <= eps && std::abs(d - 1) <= eps &&
           std::abs(e) <= eps && std::abs(f) <= eps;
  }
  /// Geometric mean of the axis scale factors (sqrt |det|): how much a line
  /// width grows under this transform.
  [[nodiscard]] double mean_scale() const noexcept { return std::sqrt(std::abs(determinant())); }
  friend bool operator==(const Matrix&, const Matrix&) = default;
};

enum class Verb : std::uint8_t { Move, Line, Quad, Cubic, Close };

/// A path: subpaths of lines, quadratic and cubic Béziers. Points per verb:
/// Move 1, Line 1, Quad 2, Cubic 3, Close 0.
struct Path {
  std::vector<Verb> verbs;
  std::vector<Point> points;

  void move_to(Point p) {
    verbs.push_back(Verb::Move);
    points.push_back(p);
  }
  void line_to(Point p) {
    verbs.push_back(Verb::Line);
    points.push_back(p);
  }
  void quad_to(Point c, Point p) {
    verbs.push_back(Verb::Quad);
    points.push_back(c);
    points.push_back(p);
  }
  void cubic_to(Point c1, Point c2, Point p) {
    verbs.push_back(Verb::Cubic);
    points.push_back(c1);
    points.push_back(c2);
    points.push_back(p);
  }
  void close() { verbs.push_back(Verb::Close); }
  [[nodiscard]] bool empty() const noexcept { return verbs.empty(); }
  void clear() noexcept {
    verbs.clear();
    points.clear();
  }
  friend bool operator==(const Path&, const Path&) = default;
};

// --------------------------------------------------------------------------- paint

/// sRGB colour, components 0..1, straight alpha.
struct Color {
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
  [[nodiscard]] static Color rgb8(int r, int g, int b, int a = 255) noexcept {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f,
            static_cast<float>(a) / 255.0f};
  }
  friend bool operator==(const Color&, const Color&) = default;
};

struct GradientStop {
  double offset = 0.0;  ///< 0..1, non-decreasing
  Color color;          ///< includes stop-opacity in `a`
  friend bool operator==(const GradientStop&, const GradientStop&) = default;
};

enum class Spread : std::uint8_t { Pad, Reflect, Repeat };

/// A linear or radial gradient in the user space of the element it paints
/// (objectBoundingBox units are resolved by readers into `transform`).
struct Gradient {
  enum class Type : std::uint8_t { Linear, Radial };
  Type type = Type::Linear;
  /// Linear: from p1 to p2.
  Point p1{0, 0}, p2{1, 0};
  /// Radial: end circle (center, radius) and focal circle (focal, focal_radius).
  Point center{0.5, 0.5};
  double radius = 0.5;
  Point focal{0.5, 0.5};
  double focal_radius = 0.0;
  std::vector<GradientStop> stops;
  Spread spread = Spread::Pad;
  /// Gradient space -> user space.
  Matrix transform;
  /// Interpolate colours in linear light (SVG color-interpolation=linearRGB).
  bool linear_rgb = false;
};

struct Group;

/// A tile repeated over the plane. `content` is drawn in pattern space;
/// the tile rectangle `tile` repeats; `transform` maps pattern space to the
/// user space of the painted element.
struct PatternPaint {
  Rect tile;
  Matrix transform;
  std::shared_ptr<const Group> content;
};

struct Paint {
  enum class Type : std::uint8_t { None, Solid, Gradient, Pattern };
  Type type = Type::None;
  Color color;
  std::shared_ptr<const Gradient> gradient;
  std::shared_ptr<const PatternPaint> pattern;
  /// fill-opacity / stroke-opacity, multiplied into every colour.
  double opacity = 1.0;

  [[nodiscard]] static Paint none() { return {}; }
  [[nodiscard]] static Paint solid(Color c, double opacity = 1.0) {
    Paint p;
    p.type = Type::Solid;
    p.color = c;
    p.opacity = opacity;
    return p;
  }
  [[nodiscard]] bool visible() const noexcept { return type != Type::None && opacity > 0.0; }
};

enum class FillRule : std::uint8_t { NonZero, EvenOdd };
enum class LineCap : std::uint8_t { Butt, Round, Square };
enum class LineJoin : std::uint8_t { Miter, Round, Bevel, MiterClip, Arcs };

struct Stroke {
  Paint paint;
  double width = 1.0;
  LineCap cap = LineCap::Butt;
  LineJoin join = LineJoin::Miter;
  double miter_limit = 4.0;
  /// Dash lengths (empty = solid); always an even count after reading.
  std::vector<double> dash;
  double dash_offset = 0.0;
  /// vector-effect: non-scaling-stroke (width in device units).
  bool non_scaling = false;
  /// Cosmetic hairline (CAD, metafiles): one device pixel wide whatever the transform.
  bool hairline = false;
};

// --------------------------------------------------------------------------- nodes

/// Marker graphics are expanded by readers into ordinary groups; nothing to model.

struct Shape {
  Path path;
  Paint fill;
  FillRule fill_rule = FillRule::NonZero;
  Stroke stroke;  ///< stroke.paint.type == None: not stroked
  /// SVG paint-order: draw the stroke below the fill.
  bool stroke_first = false;
  std::string id;
};

/// Font request of a text run.
struct FontSpec {
  std::string family = "sans-serif";
  double size = 16.0;
  int weight = 400;
  bool italic = false;
  friend bool operator==(const FontSpec&, const FontSpec&) = default;
};

/// A line of text already laid out: `outline` holds the glyph outlines in
/// node coordinates (so every writer and the rasteriser can draw it), the
/// string and font are kept for writers that can emit real text.
struct Text {
  std::string content;  ///< UTF-8
  FontSpec font;
  /// Baseline start of the run (after text-anchor has been applied).
  Point origin;
  /// Horizontal advance of the whole run in user units.
  double advance = 0.0;
  Path outline;
  Paint fill = Paint::solid({0, 0, 0, 1});
  Stroke stroke;
  std::string id;
};

/// An embedded raster picture drawn into `bounds` (user space).
struct Picture {
  std::shared_ptr<const ir::Frame> pixels;  ///< sRGB RGBA, straight alpha
  Rect bounds;
  bool smooth = true;  ///< image-rendering: false = pixelated
  /// The original encoded bytes ("png", "jpg"...) when the source embedded
  /// a file; writers may copy it instead of re-encoding.
  Bytes encoded;
  std::string encoded_format;
  std::string id;
};

struct ClipPath {
  /// Union of these paths (in the clipped group's coordinates), each with its rule.
  struct Item {
    Path path;
    FillRule rule = FillRule::NonZero;
  };
  std::vector<Item> items;
  /// Optional further clip intersected with this one.
  std::shared_ptr<const ClipPath> clip;
};

struct Mask {
  enum class Type : std::uint8_t { Luminance, Alpha };
  Type type = Type::Luminance;
  Rect region;  ///< in the masked group's coordinates; empty = unbounded
  std::shared_ptr<const Group> content;
};

enum class BlendMode : std::uint8_t {
  Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn, HardLight, SoftLight, Difference,
  Exclusion, Hue, Saturation, Color, Luminosity
};

/// One filter primitive (SVG filter effects subset). Inputs refer to
/// "SourceGraphic", "SourceAlpha", a previous `result`, or "" = previous output.
struct FilterPrimitive {
  enum class Type : std::uint8_t { GaussianBlur, Offset, Flood, Merge, ColorMatrix, DropShadow, Blend, Composite };
  Type type = Type::GaussianBlur;
  std::string in, in2, result;
  std::vector<std::string> merge_inputs;
  double std_dev_x = 0, std_dev_y = 0;  ///< blur / drop shadow
  double dx = 0, dy = 0;                ///< offset / drop shadow
  Color color;                          ///< flood / drop shadow (alpha includes opacity)
  /// ColorMatrix: 20 values row-major (type "matrix"; saturate/hueRotate/luminanceToAlpha are expanded).
  std::vector<double> matrix;
  BlendMode blend = BlendMode::Normal;
  /// Composite operator: over, in, out, atop, xor, arithmetic.
  std::string op = "over";
  double k1 = 0, k2 = 0, k3 = 0, k4 = 0;
  bool linear_rgb = true;  ///< color-interpolation-filters
};

struct Filter {
  Rect region;  ///< in the filtered group's coordinates
  std::vector<FilterPrimitive> primitives;
};

struct Node;

struct Group {
  Matrix transform;  ///< group space -> parent space
  double opacity = 1.0;
  BlendMode blend = BlendMode::Normal;
  std::shared_ptr<const ClipPath> clip;
  std::shared_ptr<const Mask> mask;
  std::shared_ptr<const Filter> filter;
  std::vector<Node> children;
  std::string id;
  /// A drawing layer (Inkscape/Illustrator layer, DXF/CGM layer).
  bool layer = false;
  std::string layer_name;
  /// Hidden layers are kept for round trips but not drawn.
  bool visible = true;
};

struct Node {
  std::variant<Shape, Text, Picture, Group> value;
};

// --------------------------------------------------------------------------- document

struct Page {
  /// Page size in user units (CSS px). Content outside is clipped by renderers.
  double width = 0.0;
  double height = 0.0;
  /// The drawing; its transform maps content into page space.
  Group root;
  std::string name;
};

struct Scene {
  std::vector<Page> pages;
  /// Physical unit the source used for its numbers ("px", "pt", "mm", "in",
  /// "unitless") - a hint so CAD/plotter writers can choose sensible scales.
  std::string source_units = "px";
  /// Title / description kept when `strip_metadata` is off; never author or tool data.
  std::map<std::string, std::string> metadata;
  /// 3D faces (DXF 3DFACE, POLYFACE, MESH); empty for pure 2D drawings.
  std::optional<ir::Mesh> mesh;

  [[nodiscard]] bool empty() const noexcept { return pages.empty(); }
};

/// CSS px per unit.
inline constexpr double px_per_pt = 96.0 / 72.0;
inline constexpr double px_per_mm = 96.0 / 25.4;
inline constexpr double px_per_in = 96.0;

}  // namespace cvt::vector
