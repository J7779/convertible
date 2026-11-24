// Convertible core - vector family: CAD and plotter formats, internal declarations.
//
// DXF (cad_dxf_*.cpp), DWG detection, HP-GL and HP-GL/2 (cad_hpgl.cpp) and
// CGM (cad_cgm.cpp). Everything here is private to the cad_* files.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "ir/mesh.hpp"
#include "util/bytes.hpp"

namespace cvt {
class Registry;
}

namespace cvt::vector::cad {

// --------------------------------------------------------------------------- registration

void register_dxf(Registry& registry);
void register_dwg(Registry& registry);
void register_hpgl(Registry& registry);
void register_cgm(Registry& registry);

// --------------------------------------------------------------------------- shared helpers

/// The AutoCAD Colour Index palette (index 0..255, 0xRRGGBB).
[[nodiscard]] std::uint32_t aci_rgb(int index) noexcept;
/// ACI colour as a scene colour; index 7 (white/black) is drawn black.
[[nodiscard]] Color aci_color(int index) noexcept;
/// Nearest ACI index (1..255) to an sRGB colour.
[[nodiscard]] int nearest_aci(Color color) noexcept;

/// Append a code point as UTF-8.
void append_utf8(std::string& out, char32_t cp);

/// Format a number for text output: up to `decimals` digits after the
/// point, trailing zeros removed, never "-0", locale free.
[[nodiscard]] std::string number(double value, int decimals = 6);

/// Bounds of every visible thing in a group (paths, text outlines, pictures),
/// in the group's parent space.
[[nodiscard]] Rect content_bounds(const Group& group, const Matrix& parent);

// --------------------------------------------------------------------------- DXF tags

/// One group code / value pair. `text` views into the input (or is empty for
/// binary numbers, which are kept in `number`).
struct DxfTag {
  int code = 0;
  std::string_view text;
  double number = 0.0;
  bool binary_number = false;
};

[[nodiscard]] double tag_double(const DxfTag& tag) noexcept;
[[nodiscard]] std::int64_t tag_int(const DxfTag& tag) noexcept;
/// The raw string (trimmed of the trailing CR in ASCII files).
[[nodiscard]] std::string_view tag_string(const DxfTag& tag) noexcept;

/// True when the data starts with the binary DXF sentinel.
[[nodiscard]] bool is_binary_dxf(ByteView data) noexcept;
/// Split an ASCII or binary DXF into tags (comments 999 dropped).
[[nodiscard]] Result<std::vector<DxfTag>> tokenize_dxf(ByteView data, const Context& ctx);

/// Decodes DXF strings into UTF-8 according to $DWGCODEPAGE / $ACADVER and
/// resolves \U+XXXX escapes.
class DxfStrings {
 public:
  /// `codepage` as in the header (e.g. "ANSI_1251"), empty when absent.
  void configure(std::string_view acadver, std::string_view codepage);
  [[nodiscard]] std::string decode(std::string_view raw) const;
  [[nodiscard]] bool utf8() const noexcept { return utf8_; }

 private:
  bool utf8_ = false;
  bool guess_ = true;
  int charset_ = 0;
};

// --------------------------------------------------------------------------- DXF geometry helpers

struct Vec3 {
  double x = 0, y = 0, z = 0;
};
[[nodiscard]] inline Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] inline Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] inline Vec3 operator*(Vec3 a, double s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] Vec3 cross3(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] Vec3 normalized3(Vec3 v) noexcept;

/// 3D affine transform: p' = m * p + t (column vectors ax, ay, az).
struct Affine3 {
  Vec3 ax{1, 0, 0}, ay{0, 1, 0}, az{0, 0, 1}, t{0, 0, 0};
  [[nodiscard]] Vec3 apply(Vec3 p) const noexcept { return ax * p.x + ay * p.y + az * p.z + t; }
  [[nodiscard]] Vec3 apply_vector(Vec3 p) const noexcept { return ax * p.x + ay * p.y + az * p.z; }
  /// this(inner(p)).
  [[nodiscard]] Affine3 operator*(const Affine3& inner) const noexcept {
    return {apply_vector(inner.ax), apply_vector(inner.ay), apply_vector(inner.az), apply(inner.t)};
  }
  /// The 2D (top view) matrix of points in the plane z = `elevation`.
  [[nodiscard]] Matrix plane(double elevation = 0.0) const noexcept {
    const Vec3 o = apply(Vec3{0, 0, elevation});
    return {ax.x, ax.y, ay.x, ay.y, o.x, o.y};
  }
};

/// The arbitrary axis algorithm: OCS -> WCS for an extrusion direction.
[[nodiscard]] Affine3 ocs_to_wcs(Vec3 extrusion) noexcept;

/// Arc between two polyline vertices with a DXF bulge, appended to `path`
/// (the current point must be `from`).
void bulge_to(Path& path, Point from, Point to, double bulge);

/// A NURBS curve.
struct Nurbs {
  int degree = 3;
  std::vector<double> knots;
  std::vector<Point> control;
  std::vector<double> weights;  ///< empty = non-rational
};
/// Evaluate at parameter u (clamped to the knot domain).
[[nodiscard]] Point nurbs_point(const Nurbs& curve, double u);
/// Append the curve to `path` (exact Béziers for non-rational degree <= 3,
/// otherwise sampled within `tolerance`). `connect`: line to the start
/// instead of a move. Returns false when the definition is unusable.
bool append_nurbs(Path& path, const Nurbs& curve, double tolerance, bool connect, std::size_t max_segments);
/// Cubic interpolation through fit points (chord-length parameters; optional
/// end tangents), appended as Béziers.
bool append_fit_spline(Path& path, std::span<const Point> fit, std::optional<Point> start_tangent,
                       std::optional<Point> end_tangent, bool connect);

// --------------------------------------------------------------------------- DXF text

struct DxfTextStyle {
  std::string font;     ///< font file or family as stored
  double width_factor = 1.0;
  double oblique = 0.0;  ///< degrees
  double fixed_height = 0.0;
  bool bold = false;
  bool italic = false;
};

/// Ratio of cap height to em size of the bundled fonts: DXF heights are cap heights.
inline constexpr double cap_height_ratio = 0.727;

/// Replace %%c, %%d, %%p, %%nnn, %%% and drop %%u / %%o / %%k toggles.
[[nodiscard]] std::string expand_percent_codes(std::string_view text);

struct MTextRun {
  std::string text;
  double height = 1.0;  ///< cap height
  std::optional<Color> color;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  std::string font;
};
struct MTextParagraph {
  std::vector<MTextRun> runs;
};
/// Parse MTEXT inline formatting into paragraphs of runs.
[[nodiscard]] std::vector<MTextParagraph> parse_mtext(std::string_view text, double height, const MTextRun& base);

// --------------------------------------------------------------------------- DXF writing of meshes

/// A Mesh (+Y up) as a DXF R12 of 3DFACEs (+Z up).
[[nodiscard]] Result<Bytes> write_mesh_dxf(const ir::Mesh& mesh, const Context& ctx);

}  // namespace cvt::vector::cad
