// Convertible core - vector family: DXF writer (R12 / AC1009, 2000 / AC1015,
// 2018 / AC1032) and the 3DFACE writer of meshes.
//
// Scene px (y down) become drawing units (y up). R12 holds POLYLINEs,
// CIRCLEs and TEXT with curves flattened; 2000 and 2018 add LWPOLYLINE,
// cubic SPLINEs, ELLIPSEs, solid HATCH fills, true colours, line weights,
// handles and the minimal OBJECTS dictionaries.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/standard_options.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "families/vector/cad_dxf.hpp"
#include "families/vector/cad_internal.hpp"
#include "families/vector/geom.hpp"
#include "util/text.hpp"

namespace cvt::vector::cad {

namespace {

constexpr double pi = std::numbers::pi;
constexpr std::array<int, 24> standard_lineweights = {0,  5,  9,  13, 15, 18,  20,  25,  30,  35,  40,  50,
                                                      53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};

enum class Version : std::uint8_t { R12, R2000, R2018 };

struct LayerOut {
  std::string name;
  bool visible = true;
};

struct LinetypeOut {
  std::string name;
  std::vector<double> elements;  // DXF convention: dash > 0, gap < 0
};

struct EntityStyle {
  std::string layer = "0";
  Color color{0, 0, 0, 1};
  int lineweight = -3;   // 1/100 mm, -3 = default
  std::string linetype;  // empty = BYLAYER
};

std::string sanitize_layer(std::string_view name) {
  std::string out;
  for (const char c : name) {
    if (std::string_view("<>/\\\":;?*|=`,").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20) {
      out.push_back('_');
    } else {
      out.push_back(c);
    }
  }
  out = std::string(util::trim(out));
  if (out.size() > 255) out.resize(255);
  return out.empty() ? std::string("Layer") : out;
}

class Writer {
 public:
  Writer(Version version, int precision) : version_(version), precision_(precision) {}

  void tag(int code, std::string_view value) {
    body_ += std::format("{:>3}\n", code);
    body_ += value;
    body_ += '\n';
  }
  void num(int code, double value) { tag(code, number(value, precision_)); }
  void integer(int code, std::int64_t value) { tag(code, std::to_string(value)); }
  void point(int code, Point p) {
    num(code, p.x);
    num(code + 10, p.y);
    num(code + 20, 0.0);
  }
  void point3(int code, double x, double y, double z) {
    num(code, x);
    num(code + 10, y);
    num(code + 20, z);
  }
  std::string handle() { return std::format("{:X}", next_handle_++); }
  [[nodiscard]] bool handles() const { return version_ != Version::R12; }
  [[nodiscard]] Version version() const { return version_; }
  [[nodiscard]] std::uint64_t handseed() const { return next_handle_; }
  std::string take() { return std::exchange(body_, {}); }
  void append(std::string_view raw) { body_ += raw; }

  /// Text in the file's encoding: UTF-8 for 2018, else ANSI_1252 with \U+XXXX escapes.
  [[nodiscard]] std::string text(std::string_view utf8) const {
    std::string out;
    for (const char32_t cp : util::utf8_to_utf32(utf8)) {
      if (cp == '\n' || cp == '\r' || cp == '\t') {
        out.push_back(' ');
      } else if (cp < 0x20) {
        continue;
      } else if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
      } else if (version_ == Version::R2018) {
        append_utf8(out, cp);
      } else if (cp >= 0xA0 && cp <= 0xFF) {
        out.push_back(static_cast<char>(cp));
      } else if (cp <= 0xFFFF) {
        out += std::format("\\U+{:04X}", static_cast<std::uint32_t>(cp));
      } else {
        out.push_back('?');
      }
    }
    return out;
  }

 private:
  Version version_;
  int precision_;
  std::string body_;
  std::uint64_t next_handle_ = 0x20;
};

int lineweight_of(double width_mm) {
  if (!(width_mm > 0) || !std::isfinite(width_mm)) return 0;
  const double hundredths = width_mm * 100;
  int best = standard_lineweights[0];
  for (const int w : standard_lineweights) {
    if (std::abs(w - hundredths) < std::abs(best - hundredths)) best = w;
  }
  return best;
}

std::uint32_t rgb_of(Color c) {
  const auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  return (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
}

// Ellipse detection: a closed subpath of four cubic quarter arcs (as made
// by add_ellipse). Returns the centre and conjugate semi-diameters.
struct EllipseFit {
  Point center, u, v;
};

struct Subpath {
  std::vector<Verb> verbs;
  std::vector<Point> points;
};

std::optional<EllipseFit> as_ellipse(const Subpath& s) {
  const std::vector<Verb>& verbs = s.verbs;
  const std::vector<Point>& pts = s.points;
  if (verbs.size() < 5 || verbs.size() > 6 || verbs[0] != Verb::Move) return std::nullopt;
  for (std::size_t i = 1; i <= 4; ++i) {
    if (verbs[i] != Verb::Cubic) return std::nullopt;
  }
  if (verbs.size() == 6 && verbs[5] != Verb::Close) return std::nullopt;
  if (pts.size() != 13) return std::nullopt;
  const Point p0 = pts[0], p1 = pts[3], p2 = pts[6], p3 = pts[9], p4 = pts[12];
  const double scale = std::max(length(p2 - p0), length(p3 - p1));
  if (!(scale > 0) || !std::isfinite(scale)) return std::nullopt;
  const double tol = scale * 1e-6;
  if (length(p4 - p0) > tol) return std::nullopt;
  const Point c = (p0 + p2) * 0.5;
  if (length((p1 + p3) * 0.5 - c) > tol) return std::nullopt;
  const Point u = p0 - c, v = p1 - c;
  constexpr double k = 0.5522847498307936;
  const Point w = p2 - c, z = p3 - c;
  const double t = scale * 1e-4;
  if (length(pts[1] - (p0 + v * k)) > t || length(pts[2] - (p1 + u * k)) > t) return std::nullopt;
  if (length(pts[4] - (p1 + w * k)) > t || length(pts[5] - (p2 + v * k)) > t) return std::nullopt;
  if (length(pts[7] - (p2 + z * k)) > t || length(pts[10] - (p3 + u * k)) > t) return std::nullopt;
  return EllipseFit{c, u, v};
}

// Principal axes from conjugate semi-diameters u, v: P(t) = c + u cos t + v sin t.
void principal_axes(Point u, Point v, Point& major, double& ratio) {
  const double t = 0.5 * std::atan2(2 * dot(u, v), dot(u, u) - dot(v, v));
  const Point a = u * std::cos(t) + v * std::sin(t);
  const Point b = u * -std::sin(t) + v * std::cos(t);
  if (length(a) >= length(b)) {
    major = a;
    ratio = length(a) > 0 ? length(b) / length(a) : 1.0;
  } else {
    major = b;
    ratio = length(b) > 0 ? length(a) / length(b) : 1.0;
  }
}

std::vector<Subpath> split_subpaths(const Path& path) {
  std::vector<Subpath> out;
  std::size_t at = 0;
  Point start{};
  for (const Verb v : path.verbs) {
    const std::size_t n = v == Verb::Move || v == Verb::Line ? 1 : v == Verb::Quad ? 2 : v == Verb::Cubic ? 3 : 0;
    if (at + n > path.points.size()) break;
    if (v == Verb::Move) {
      out.emplace_back();
      start = path.points[at];
    } else if (out.empty() || out.back().verbs.empty() || out.back().verbs.back() == Verb::Close) {
      out.emplace_back();
      out.back().verbs.push_back(Verb::Move);
      out.back().points.push_back(start);
    }
    out.back().verbs.push_back(v);
    for (std::size_t k = 0; k < n; ++k) out.back().points.push_back(path.points[at + k]);
    at += n;
  }
  std::erase_if(out, [](const Subpath& s) { return s.verbs.size() < 2 || s.points.size() < 2; });
  return out;
}

class SceneExporter {
 public:
  SceneExporter(Writer& w, WriteContext& ctx, double px_per_unit, const Matrix& base, bool fills)
      : w_(w), ctx_(ctx), k_(px_per_unit), base_(base), fills_(fills) {
    layers_["0"] = {"0", true};
    layer_order_.push_back("0");
  }

  std::string model_space_handle;

  Result<void> export_group(const Group& group, const Matrix& parent, const std::string& layer, bool visible, int depth) {
    if (depth > 256) return fail(Status::Encode, "the drawing is nested too deeply");
    const Matrix m = parent * group.transform;
    std::string current = layer;
    bool shown = visible && group.visible;
    if (group.layer) {
      const std::string name = sanitize_layer(group.layer_name.empty() ? group.id : group.layer_name);
      const std::string key = util::to_upper(name);
      if (!layers_.contains(key)) {
        layers_[key] = {name, shown};
        layer_order_.push_back(key);
      }
      current = layers_[key].name;
    } else if (!group.visible) {
      return {};
    }
    if (group.clip || group.mask || group.filter) note("Clipping, masks and filters cannot be written to DXF and were ignored.");
    for (const Node& node : group.children) {
      if (++count_ % 4096 == 0) CVT_TRY(ctx_.checkpoint(0.5));
      if (const auto* shape = std::get_if<Shape>(&node.value)) {
        CVT_TRY(export_shape(*shape, m, current, group.opacity));
      } else if (const auto* text = std::get_if<Text>(&node.value)) {
        CVT_TRY(export_text(*text, m, current));
      } else if (const auto* child = std::get_if<Group>(&node.value)) {
        CVT_TRY(export_group(*child, m, current, shown, depth + 1));
      } else if (std::get_if<Picture>(&node.value) != nullptr) {
        note("Raster pictures cannot be embedded in DXF and were left out.");
      }
    }
    return {};
  }

  [[nodiscard]] const std::map<std::string, LayerOut>& layers() const { return layers_; }
  [[nodiscard]] const std::vector<std::string>& layer_order() const { return layer_order_; }
  [[nodiscard]] const std::vector<LinetypeOut>& linetypes() const { return linetypes_; }
  [[nodiscard]] const Rect& extents() const { return extents_; }

 private:
  void note(std::string_view message) {
    const std::string m(message);
    if (std::find(notes_.begin(), notes_.end(), m) != notes_.end()) return;
    notes_.push_back(m);
    ctx_.warn(m);
  }

  [[nodiscard]] Matrix to_dxf(const Matrix& m) const { return base_ * m; }

  std::string linetype_for(const Stroke& stroke, double scale) {
    if (stroke.dash.empty()) return {};
    std::vector<double> elements;
    for (std::size_t i = 0; i < stroke.dash.size(); ++i) {
      const double v = stroke.dash[i] * scale / k_;
      elements.push_back(i % 2 == 0 ? v : -v);
    }
    for (const LinetypeOut& lt : linetypes_) {
      if (lt.elements == elements) return lt.name;
    }
    if (linetypes_.size() >= 1000) return {};
    linetypes_.push_back({std::format("DASH{}", linetypes_.size() + 1), elements});
    return linetypes_.back().name;
  }

  void common(std::string_view type, const EntityStyle& style) {
    w_.tag(0, type);
    if (w_.handles()) {
      w_.tag(5, w_.handle());
      w_.tag(330, model_space_handle);
      w_.tag(100, "AcDbEntity");
    }
    w_.tag(8, w_.text(style.layer));
    if (!style.linetype.empty()) w_.tag(6, style.linetype);
    w_.integer(62, nearest_aci(style.color));
    if (w_.version() != Version::R12) {
      w_.integer(420, rgb_of(style.color));
      w_.integer(370, style.lineweight);
      if (w_.version() == Version::R2018 && style.color.a < 1.0f) {
        const auto alpha = static_cast<std::uint32_t>(std::lround(std::clamp(style.color.a, 0.0f, 1.0f) * 255));
        w_.integer(440, static_cast<std::int64_t>(0x02000000u | alpha));
      }
    }
  }

  void grow(Point p) { extents_.add(p); }

  void write_polyline(const std::vector<Point>& pts, bool closed, const EntityStyle& style) {
    if (pts.size() < 2) return;
    for (const Point& p : pts) grow(p);
    if (w_.version() == Version::R12) {
      common("POLYLINE", style);
      w_.integer(66, 1);
      w_.point(10, {0, 0});
      w_.integer(70, closed ? 1 : 0);
      for (const Point& p : pts) {
        w_.tag(0, "VERTEX");
        w_.tag(8, w_.text(style.layer));
        w_.point(10, p);
      }
      w_.tag(0, "SEQEND");
      w_.tag(8, w_.text(style.layer));
      return;
    }
    common("LWPOLYLINE", style);
    w_.tag(100, "AcDbPolyline");
    w_.integer(90, static_cast<std::int64_t>(pts.size()));
    w_.integer(70, closed ? 1 : 0);
    for (const Point& p : pts) {
      w_.num(10, p.x);
      w_.num(20, p.y);
    }
  }

  void write_circle_or_ellipse(const EllipseFit& e, const EntityStyle& style) {
    Point major;
    double ratio = 1;
    principal_axes(e.u, e.v, major, ratio);
    for (const double sx : {-1.0, 1.0}) {
      for (const double sy : {-1.0, 1.0}) grow(e.center + e.u * sx + e.v * sy);
    }
    if (ratio > 1 - 1e-7) {
      common("CIRCLE", style);
      if (w_.handles()) w_.tag(100, "AcDbCircle");
      w_.point(10, e.center);
      w_.num(40, length(major));
      return;
    }
    common("ELLIPSE", style);
    w_.tag(100, "AcDbEllipse");
    w_.point(10, e.center);
    w_.point(11, major);
    w_.num(40, ratio);
    w_.num(41, 0.0);
    w_.num(42, 2 * pi);
  }

  void write_spline(const Subpath& s, const EntityStyle& style) {
    std::vector<Point> cps;
    std::size_t at = 0;
    Point current{};
    for (const Verb v : s.verbs) {
      if (v == Verb::Move) {
        current = s.points[at++];
        cps.push_back(current);
      } else if (v == Verb::Line) {
        const Point p = s.points[at++];
        cps.push_back(lerp(current, p, 1.0 / 3));
        cps.push_back(lerp(current, p, 2.0 / 3));
        cps.push_back(p);
        current = p;
      } else if (v == Verb::Quad) {
        const Point c = s.points[at], p = s.points[at + 1];
        at += 2;
        cps.push_back(current + (c - current) * (2.0 / 3));
        cps.push_back(p + (c - p) * (2.0 / 3));
        cps.push_back(p);
        current = p;
      } else if (v == Verb::Cubic) {
        cps.push_back(s.points[at]);
        cps.push_back(s.points[at + 1]);
        cps.push_back(s.points[at + 2]);
        current = s.points[at + 2];
        at += 3;
      } else if (v == Verb::Close && !cps.empty() && length(current - cps.front()) > 1e-12) {
        const Point p = cps.front();
        cps.push_back(lerp(current, p, 1.0 / 3));
        cps.push_back(lerp(current, p, 2.0 / 3));
        cps.push_back(p);
        current = p;
      }
    }
    const std::size_t segments = cps.empty() ? 0 : (cps.size() - 1) / 3;
    if (segments == 0) return;
    for (const Point& p : cps) grow(p);
    common("SPLINE", style);
    w_.tag(100, "AcDbSpline");
    w_.point3(210, 0, 0, 1);
    w_.integer(70, 8);
    w_.integer(71, 3);
    w_.integer(72, static_cast<std::int64_t>(segments * 3 + 5));
    w_.integer(73, static_cast<std::int64_t>(cps.size()));
    w_.integer(74, 0);
    w_.num(42, 1e-10);
    w_.num(43, 1e-10);
    for (int r = 0; r < 4; ++r) w_.num(40, 0.0);
    for (std::size_t i = 1; i < segments; ++i) {
      for (int r = 0; r < 3; ++r) w_.num(40, static_cast<double>(i));
    }
    for (int r = 0; r < 4; ++r) w_.num(40, static_cast<double>(segments));
    for (const Point& p : cps) w_.point(10, p);
  }

  void write_hatch(const std::vector<Polyline>& loops, const EntityStyle& style) {
    std::size_t count = 0;
    for (const Polyline& l : loops) count += l.points.size() >= 3 ? 1 : 0;
    if (count == 0) return;
    common("HATCH", style);
    w_.tag(100, "AcDbHatch");
    w_.point(10, {0, 0});
    w_.point3(210, 0, 0, 1);
    w_.tag(2, "SOLID");
    w_.integer(70, 1);
    w_.integer(71, 0);
    w_.integer(91, static_cast<std::int64_t>(count));
    for (const Polyline& l : loops) {
      if (l.points.size() < 3) continue;
      w_.integer(92, 2);
      w_.integer(72, 0);
      w_.integer(73, 1);
      w_.integer(93, static_cast<std::int64_t>(l.points.size()));
      for (const Point& p : l.points) {
        grow(p);
        w_.num(10, p.x);
        w_.num(20, p.y);
      }
      w_.integer(97, 0);
    }
    w_.integer(75, 0);
    w_.integer(76, 1);
    w_.integer(98, 0);
  }

  static Color paint_color(const Paint& paint, double opacity) {
    Color c = paint.color;
    if (paint.type == Paint::Type::Gradient && paint.gradient && !paint.gradient->stops.empty()) {
      c = paint.gradient->stops.front().color;
    }
    c.a = static_cast<float>(std::clamp(c.a * paint.opacity * opacity, 0.0, 1.0));
    return c;
  }

  Result<void> export_shape(const Shape& shape, const Matrix& m, const std::string& layer, double opacity) {
    const Matrix t = to_dxf(m);
    const Path path = transformed(shape.path, t);
    const double tolerance = 0.1 / k_;  // 0.1 px
    if (shape.fill.visible()) {
      EntityStyle style;
      style.layer = layer;
      style.color = paint_color(shape.fill, opacity);
      if (fills_ && w_.version() != Version::R12) {
        if (shape.fill.type != Paint::Type::Solid) note("Gradient and pattern fills were written as solid fills.");
        write_hatch(flatten(path, tolerance), style);
      } else if (!shape.stroke.paint.visible()) {
        if (w_.version() == Version::R12) note("DXF R12 has no area fills; filled shapes were written as outlines.");
        CVT_TRY(export_outline(path, style, tolerance));
      }
    }
    if (shape.stroke.paint.visible()) {
      EntityStyle style;
      style.layer = layer;
      style.color = paint_color(shape.stroke.paint, opacity);
      const double scale = shape.stroke.non_scaling ? 1.0 : m.mean_scale();
      style.lineweight = shape.stroke.hairline ? 0 : lineweight_of(shape.stroke.width * scale / px_per_mm);
      style.linetype = linetype_for(shape.stroke, scale);
      CVT_TRY(export_outline(path, style, tolerance));
    }
    return {};
  }

  Result<void> export_outline(const Path& path, const EntityStyle& style, double tolerance) {
    for (const Subpath& s : split_subpaths(path)) {
      if (const auto e = as_ellipse(s)) {
        Point major;
        double ratio = 1;
        principal_axes(e->u, e->v, major, ratio);
        if (w_.version() != Version::R12 || ratio > 1 - 1e-7) {
          write_circle_or_ellipse(*e, style);
          continue;
        }
      }
      const bool curved =
          std::any_of(s.verbs.begin(), s.verbs.end(), [](Verb v) { return v == Verb::Quad || v == Verb::Cubic; });
      if (curved && w_.version() != Version::R12) {
        write_spline(s, style);
        continue;
      }
      Path sub;
      sub.verbs = s.verbs;
      sub.points = s.points;
      for (const Polyline& pl : flatten(sub, tolerance)) {
        std::vector<Point> pts = pl.points;
        bool closed = pl.closed;
        if (pts.size() > 2 && length(pts.back() - pts.front()) < 1e-12) {
          pts.pop_back();
          closed = true;
        }
        write_polyline(pts, closed, style);
      }
    }
    return {};
  }

  Result<void> export_text(const Text& text, const Matrix& m, const std::string& layer) {
    EntityStyle style;
    style.layer = layer;
    style.color = paint_color(text.fill.visible() ? text.fill : text.stroke.paint, 1.0);
    const std::string content(util::trim(text.content));
    const Matrix t = to_dxf(m);
    if (content.empty()) {
      if (!text.outline.empty() && w_.version() != Version::R12) {
        write_hatch(flatten(transformed(text.outline, t), 0.05 / k_), style);
      }
      return {};
    }
    const Point origin = t.apply(text.origin);
    const Point xaxis = t.apply_vector({1, 0});
    const Point yaxis = t.apply_vector({0, -1});  // the text's up direction in DXF space
    const double sx = length(xaxis), sy = length(yaxis);
    if (!(sx > 0) || !(sy > 0)) return {};
    const double height = text.font.size * cap_height_ratio * sy;
    if (!(height > 0) || !std::isfinite(height)) return {};
    const double rotation = std::atan2(xaxis.y, xaxis.x) * 180 / pi;
    grow(origin);
    grow(origin + xaxis * (text.advance > 0 ? text.advance : text.font.size));
    grow(origin + yaxis * (text.font.size * cap_height_ratio));
    common("TEXT", style);
    if (w_.handles()) w_.tag(100, "AcDbText");
    w_.point(10, origin);
    w_.num(40, height);
    w_.tag(1, w_.text(content));
    if (std::abs(rotation) > 1e-9) w_.num(50, rotation);
    if (std::abs(sx / sy - 1) > 1e-6) w_.num(41, sx / sy);
    if (text.font.italic) w_.num(51, 15.0);
    if (w_.handles()) w_.tag(100, "AcDbText");
    return {};
  }

  Writer& w_;
  WriteContext& ctx_;
  double k_;
  Matrix base_;
  bool fills_;
  std::map<std::string, LayerOut> layers_;
  std::vector<std::string> layer_order_;
  std::vector<LinetypeOut> linetypes_;
  Rect extents_;
  std::uint64_t count_ = 0;
  std::vector<std::string> notes_;
};

// --------------------------------------------------------------------------- document assembly

struct Handles {
  std::string vport_table, ltype_table, layer_table, style_table, view_table, ucs_table, appid_table, dimstyle_table,
      block_record_table;
  std::string vport, ltype_byblock, ltype_bylayer, ltype_continuous, style_standard, appid_acad, dimstyle_standard;
  std::string model_record, paper_record, model_block, model_endblk, paper_block, paper_endblk;
  std::string root_dict, group_dict, layout_dict, model_layout, paper_layout, plotstyle_dict, placeholder;
};

void table_head(Writer& w, std::string_view name, const std::string& handle, std::size_t count) {
  w.tag(0, "TABLE");
  w.tag(2, name);
  if (w.handles()) {
    w.tag(5, handle);
    w.tag(330, "0");
    w.tag(100, "AcDbSymbolTable");
  }
  w.integer(70, static_cast<std::int64_t>(count));
}

void record_head(Writer& w, std::string_view type, std::string_view subclass, const std::string& handle,
                 const std::string& owner, int handle_code = 5) {
  w.tag(0, type);
  if (w.handles()) {
    w.tag(handle_code, handle);
    w.tag(330, owner);
    w.tag(100, "AcDbSymbolTableRecord");
    w.tag(100, subclass);
  }
}

void write_layout(Writer& w, const Handles& h, const std::string& handle, std::string_view name, int order,
                  const std::string& block_record) {
  w.tag(0, "LAYOUT");
  w.tag(5, handle);
  w.tag(102, "{ACAD_REACTORS");
  w.tag(330, h.layout_dict);
  w.tag(102, "}");
  w.tag(330, h.layout_dict);
  w.tag(100, "AcDbPlotSettings");
  w.tag(1, "");
  w.tag(2, "none_device");
  w.tag(4, "");
  w.tag(6, "");
  for (const int code : {40, 41, 42, 43, 44, 45, 46, 47, 48, 49}) w.num(code, 0.0);
  w.num(140, 0.0);
  w.num(141, 0.0);
  w.num(142, 1.0);
  w.num(143, 1.0);
  w.integer(70, 688);
  w.integer(72, 0);
  w.integer(73, 0);
  w.integer(74, 5);
  w.tag(7, "");
  w.integer(75, 16);
  w.integer(76, 0);
  w.integer(77, 2);
  w.integer(78, 300);
  w.num(147, 1.0);
  w.num(148, 0.0);
  w.num(149, 0.0);
  w.tag(100, "AcDbLayout");
  w.tag(1, name);
  w.integer(70, 1);
  w.integer(71, order);
  w.num(10, 0.0);
  w.num(20, 0.0);
  w.num(11, 420.0);
  w.num(21, 297.0);
  w.point(12, {0, 0});
  w.point(14, {0, 0});
  w.point(15, {0, 0});
  w.num(146, 0.0);
  w.point(13, {0, 0});
  w.point3(16, 1, 0, 0);
  w.point3(17, 0, 1, 0);
  w.integer(76, 0);
  w.tag(330, block_record);
}

std::string assemble(Writer& w, SceneExporter& exporter, std::string entities, Handles& h, int insunits, Rect extents) {
  const bool handles = w.handles();
  const Version version = w.version();
  // Table record handles for layers and line types (allocated now: handles are just unique numbers).
  std::vector<std::string> layer_handles, ltype_handles;
  for (std::size_t i = 0; i < exporter.layer_order().size(); ++i) layer_handles.push_back(w.handle());
  for (std::size_t i = 0; i < exporter.linetypes().size(); ++i) ltype_handles.push_back(w.handle());
  const std::string handseed = std::format("{:X}", w.handseed() + 16);
  if (extents.empty()) extents = {0, 0, 0, 0};

  // ---- HEADER
  w.tag(0, "SECTION");
  w.tag(2, "HEADER");
  w.tag(9, "$ACADVER");
  w.tag(1, version == Version::R12 ? "AC1009" : version == Version::R2000 ? "AC1015" : "AC1032");
  if (version != Version::R12) {
    w.tag(9, "$ACADMAINTVER");
    w.integer(70, version == Version::R2000 ? 6 : 105);
  }
  w.tag(9, "$DWGCODEPAGE");
  w.tag(3, "ANSI_1252");
  w.tag(9, "$INSBASE");
  w.point(10, {0, 0});
  w.tag(9, "$EXTMIN");
  w.point(10, {extents.x0, extents.y0});
  w.tag(9, "$EXTMAX");
  w.point(10, {extents.x1, extents.y1});
  w.tag(9, "$LIMMIN");
  w.num(10, extents.x0);
  w.num(20, extents.y0);
  w.tag(9, "$LIMMAX");
  w.num(10, extents.x1);
  w.num(20, extents.y1);
  w.tag(9, "$LTSCALE");
  w.num(40, 1.0);
  if (handles) {
    w.tag(9, "$HANDSEED");
    w.tag(5, handseed);
    w.tag(9, "$MEASUREMENT");
    w.integer(70, insunits == 1 || insunits == 2 ? 0 : 1);
    w.tag(9, "$INSUNITS");
    w.integer(70, insunits);
    w.tag(9, "$LWDISPLAY");
    w.integer(290, 1);
  }
  w.tag(0, "ENDSEC");

  if (handles) {
    w.tag(0, "SECTION");
    w.tag(2, "CLASSES");
    w.tag(0, "ENDSEC");
  }

  // ---- TABLES
  w.tag(0, "SECTION");
  w.tag(2, "TABLES");
  table_head(w, "VPORT", h.vport_table, 1);
  record_head(w, "VPORT", "AcDbViewportTableRecord", h.vport, h.vport_table);
  w.tag(2, handles ? "*Active" : "*ACTIVE");
  w.integer(70, 0);
  w.num(10, 0.0);
  w.num(20, 0.0);
  w.num(11, 1.0);
  w.num(21, 1.0);
  w.num(12, (extents.x0 + extents.x1) / 2);
  w.num(22, (extents.y0 + extents.y1) / 2);
  w.num(13, 0.0);
  w.num(23, 0.0);
  w.num(14, 10.0);
  w.num(24, 10.0);
  w.num(15, 10.0);
  w.num(25, 10.0);
  w.point3(16, 0, 0, 1);
  w.point(17, {0, 0});
  w.num(40, std::max(extents.height() * 1.1, 1.0));
  w.num(41, extents.height() > 0 ? std::clamp(extents.width() / extents.height(), 0.1, 10.0) : 1.0);
  w.num(42, 50.0);
  w.num(43, 0.0);
  w.num(44, 0.0);
  w.num(50, 0.0);
  w.num(51, 0.0);
  w.integer(71, 0);
  w.integer(72, 100);
  w.integer(73, 1);
  w.integer(74, 3);
  w.integer(75, 0);
  w.integer(76, 0);
  w.integer(77, 0);
  w.integer(78, 0);
  w.tag(0, "ENDTAB");

  const std::size_t ltype_count = exporter.linetypes().size() + (handles ? 3 : 1);
  table_head(w, "LTYPE", h.ltype_table, ltype_count);
  const auto ltype = [&](const std::string& handle, std::string_view name, std::string_view description,
                         const std::vector<double>& elements) {
    record_head(w, "LTYPE", "AcDbLinetypeTableRecord", handle, h.ltype_table);
    w.tag(2, name);
    w.integer(70, 0);
    w.tag(3, description);
    w.integer(72, 65);
    w.integer(73, static_cast<std::int64_t>(elements.size()));
    double total = 0;
    for (const double e : elements) total += std::abs(e);
    w.num(40, total);
    for (const double e : elements) {
      w.num(49, e);
      if (handles) w.integer(74, 0);
    }
  };
  if (handles) {
    ltype(h.ltype_byblock, "ByBlock", "", {});
    ltype(h.ltype_bylayer, "ByLayer", "", {});
    ltype(h.ltype_continuous, "Continuous", "Solid line", {});
  } else {
    ltype(h.ltype_continuous, "CONTINUOUS", "Solid line", {});
  }
  for (std::size_t i = 0; i < exporter.linetypes().size(); ++i) {
    ltype(ltype_handles[i], exporter.linetypes()[i].name, "Dashed", exporter.linetypes()[i].elements);
  }
  w.tag(0, "ENDTAB");

  table_head(w, "LAYER", h.layer_table, exporter.layer_order().size());
  for (std::size_t i = 0; i < exporter.layer_order().size(); ++i) {
    const LayerOut& layer = exporter.layers().at(exporter.layer_order()[i]);
    record_head(w, "LAYER", "AcDbLayerTableRecord", layer_handles[i], h.layer_table);
    w.tag(2, w.text(layer.name));
    w.integer(70, 0);
    w.integer(62, layer.visible ? 7 : -7);
    w.tag(6, handles ? "Continuous" : "CONTINUOUS");
    if (handles) {
      w.integer(370, -3);
      w.tag(390, h.placeholder);
    }
  }
  w.tag(0, "ENDTAB");

  table_head(w, "STYLE", h.style_table, 1);
  record_head(w, "STYLE", "AcDbTextStyleTableRecord", h.style_standard, h.style_table);
  w.tag(2, handles ? "Standard" : "STANDARD");
  w.integer(70, 0);
  w.num(40, 0.0);
  w.num(41, 1.0);
  w.num(50, 0.0);
  w.integer(71, 0);
  w.num(42, 2.5);
  w.tag(3, "arial.ttf");
  w.tag(4, "");
  w.tag(0, "ENDTAB");

  table_head(w, "VIEW", h.view_table, 0);
  w.tag(0, "ENDTAB");
  table_head(w, "UCS", h.ucs_table, 0);
  w.tag(0, "ENDTAB");

  table_head(w, "APPID", h.appid_table, 1);
  record_head(w, "APPID", "AcDbRegAppTableRecord", h.appid_acad, h.appid_table);
  w.tag(2, "ACAD");
  w.integer(70, 0);
  w.tag(0, "ENDTAB");

  table_head(w, "DIMSTYLE", h.dimstyle_table, 1);
  if (handles) {
    w.tag(100, "AcDbDimStyleTable");
    w.integer(71, 1);
    w.tag(340, h.dimstyle_standard);
  }
  record_head(w, "DIMSTYLE", "AcDbDimStyleTableRecord", h.dimstyle_standard, h.dimstyle_table, 105);
  w.tag(2, handles ? "Standard" : "STANDARD");
  w.integer(70, 0);
  w.tag(0, "ENDTAB");

  if (handles) {
    table_head(w, "BLOCK_RECORD", h.block_record_table, 2);
    record_head(w, "BLOCK_RECORD", "AcDbBlockTableRecord", h.model_record, h.block_record_table);
    w.tag(2, "*Model_Space");
    w.tag(340, h.model_layout);
    w.integer(70, 0);
    w.integer(280, 1);
    w.integer(281, 0);
    record_head(w, "BLOCK_RECORD", "AcDbBlockTableRecord", h.paper_record, h.block_record_table);
    w.tag(2, "*Paper_Space");
    w.tag(340, h.paper_layout);
    w.integer(70, 0);
    w.integer(280, 1);
    w.integer(281, 0);
    w.tag(0, "ENDTAB");
  }
  w.tag(0, "ENDSEC");

  // ---- BLOCKS
  w.tag(0, "SECTION");
  w.tag(2, "BLOCKS");
  if (handles) {
    const auto block = [&](const std::string& begin, const std::string& end, const std::string& record,
                           std::string_view name, bool paper) {
      w.tag(0, "BLOCK");
      w.tag(5, begin);
      w.tag(330, record);
      w.tag(100, "AcDbEntity");
      if (paper) w.integer(67, 1);
      w.tag(8, "0");
      w.tag(100, "AcDbBlockBegin");
      w.tag(2, name);
      w.integer(70, 0);
      w.point(10, {0, 0});
      w.tag(3, name);
      w.tag(1, "");
      w.tag(0, "ENDBLK");
      w.tag(5, end);
      w.tag(330, record);
      w.tag(100, "AcDbEntity");
      if (paper) w.integer(67, 1);
      w.tag(8, "0");
      w.tag(100, "AcDbBlockEnd");
    };
    block(h.model_block, h.model_endblk, h.model_record, "*Model_Space", false);
    block(h.paper_block, h.paper_endblk, h.paper_record, "*Paper_Space", true);
  }
  w.tag(0, "ENDSEC");

  // ---- ENTITIES
  w.tag(0, "SECTION");
  w.tag(2, "ENTITIES");
  w.append(entities);
  w.tag(0, "ENDSEC");

  // ---- OBJECTS
  if (handles) {
    w.tag(0, "SECTION");
    w.tag(2, "OBJECTS");
    w.tag(0, "DICTIONARY");
    w.tag(5, h.root_dict);
    w.tag(330, "0");
    w.tag(100, "AcDbDictionary");
    w.integer(281, 1);
    w.tag(3, "ACAD_GROUP");
    w.tag(350, h.group_dict);
    w.tag(3, "ACAD_LAYOUT");
    w.tag(350, h.layout_dict);
    w.tag(3, "ACAD_PLOTSTYLENAME");
    w.tag(350, h.plotstyle_dict);
    w.tag(0, "DICTIONARY");
    w.tag(5, h.group_dict);
    w.tag(330, h.root_dict);
    w.tag(100, "AcDbDictionary");
    w.integer(281, 1);
    w.tag(0, "DICTIONARY");
    w.tag(5, h.layout_dict);
    w.tag(330, h.root_dict);
    w.tag(100, "AcDbDictionary");
    w.integer(281, 1);
    w.tag(3, "Layout1");
    w.tag(350, h.paper_layout);
    w.tag(3, "Model");
    w.tag(350, h.model_layout);
    w.tag(0, "ACDBDICTIONARYWDFLT");
    w.tag(5, h.plotstyle_dict);
    w.tag(330, h.root_dict);
    w.tag(100, "AcDbDictionary");
    w.integer(281, 1);
    w.tag(3, "Normal");
    w.tag(350, h.placeholder);
    w.tag(100, "AcDbDictionaryWithDefault");
    w.tag(340, h.placeholder);
    w.tag(0, "ACDBPLACEHOLDER");
    w.tag(5, h.placeholder);
    w.tag(330, h.plotstyle_dict);
    write_layout(w, h, h.model_layout, "Model", 0, h.model_record);
    write_layout(w, h, h.paper_layout, "Layout1", 1, h.paper_record);
    w.tag(0, "ENDSEC");
  }
  w.tag(0, "EOF");
  return w.take();
}

}  // namespace

Result<Bytes> write_dxf(const Scene& scene, WriteContext& ctx) {
  if (scene.pages.empty()) return fail(Status::Encode, "the drawing has no pages");
  const std::string version_name = ctx.options.get_enum("version", "2000", {"r12", "2000", "2018"});
  const Version version = version_name == "r12" ? Version::R12 : version_name == "2018" ? Version::R2018 : Version::R2000;
  std::string units = ctx.options.get_enum("units", "auto", {"auto", "mm", "cm", "m", "in", "ft", "px"});
  if (units == "auto") {
    const std::string& s = scene.source_units;
    units = s == "mm" || s == "cm" || s == "m" || s == "in" || s == "ft" ? s : "mm";
  }
  const double k = units == "mm" ? px_per_mm : units == "cm" ? px_per_mm * 10 : units == "m" ? px_per_mm * 1000
                   : units == "in" ? px_per_in : units == "ft" ? px_per_in * 12 : 1.0;
  const int insunits = units == "mm" ? 4 : units == "cm" ? 5 : units == "m" ? 6 : units == "in" ? 1 : units == "ft" ? 2 : 0;
  const bool fills = ctx.options.get_bool("fills", true);
  const int precision = static_cast<int>(ctx.options.get_int("precision", 6, 1, 12));
  if (scene.pages.size() > 1) ctx.warn("DXF holds one drawing; only the first page was written.");
  const Page& page = scene.pages.front();

  Writer w(version, precision);
  Handles h;
  if (w.handles()) {
    for (std::string* s : {&h.vport_table, &h.ltype_table, &h.layer_table, &h.style_table, &h.view_table, &h.ucs_table,
                           &h.appid_table, &h.dimstyle_table, &h.block_record_table, &h.vport, &h.ltype_byblock,
                           &h.ltype_bylayer, &h.ltype_continuous, &h.style_standard, &h.appid_acad,
                           &h.dimstyle_standard, &h.model_record, &h.paper_record, &h.model_block, &h.model_endblk,
                           &h.paper_block, &h.paper_endblk, &h.root_dict, &h.group_dict, &h.layout_dict,
                           &h.model_layout, &h.paper_layout, &h.plotstyle_dict, &h.placeholder}) {
      *s = w.handle();
    }
  }
  // Page px -> drawing units (y up). A page made by the DXF reader maps
  // drawing units to px with a plain scale and flip: invert it so that the
  // original coordinates come back unchanged.
  Matrix base{1 / k, 0, 0, -1 / k, 0, page.height / k};
  const Matrix& r = page.root.transform;
  if (std::abs(r.a - k) <= 1e-9 * k && std::abs(r.d + k) <= 1e-9 * k && std::abs(r.b) <= 1e-12 * k &&
      std::abs(r.c) <= 1e-12 * k) {
    base = r.inverted();
  }
  SceneExporter exporter(w, ctx, k, base, fills);
  exporter.model_space_handle = h.model_record;
  CVT_TRY(exporter.export_group(page.root, Matrix{}, "0", true, 0));
  std::string entities = w.take();
  const Rect extents = exporter.extents();
  std::string text = assemble(w, exporter, std::move(entities), h, insunits, extents);
  CVT_TRY(ctx.limits.check_output(text.size()));
  // R12 and 2000 are 8-bit ANSI_1252: map the UTF-8 kept by w.text (Latin-1 bytes) back.
  return to_bytes(text);
}

// --------------------------------------------------------------------------- meshes

Result<Bytes> write_mesh_dxf(const ir::Mesh& mesh, const Context& ctx) {
  if (!mesh.valid()) return fail(Status::Encode, "the mesh is not valid (index out of range or bad face sizes)");
  if (mesh.positions.empty()) return fail(Status::Encode, "the mesh has no vertices");
  Writer w(Version::R12, 6);
  Rect box;
  // The coordinates arrive in DXF's +Z up: the caller prepared them with the
  // mesh family's Z-up convention.
  const auto at = [&](std::uint32_t i) {
    const ir::Vec3& v = mesh.positions[i];
    return std::array<double, 3>{v.x, v.y, v.z};
  };
  for (const ir::Vec3& v : mesh.positions) box.add(Point{v.x, v.y});
  std::string entities;
  std::size_t base = 0;
  const std::size_t faces = mesh.face_count();
  for (std::size_t f = 0; f < faces; ++f) {
    if ((f & 0xFFF) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(f) / faces));
    const std::size_t size = mesh.face_sizes.empty() ? 3 : mesh.face_sizes[f];
    std::vector<std::uint32_t> poly(mesh.indices.begin() + static_cast<std::ptrdiff_t>(base),
                                    mesh.indices.begin() + static_cast<std::ptrdiff_t>(base + size));
    base += size;
    std::vector<std::array<std::uint32_t, 4>> quads;
    if (poly.size() == 3) quads.push_back({poly[0], poly[1], poly[2], poly[2]});
    else if (poly.size() == 4) quads.push_back({poly[0], poly[1], poly[2], poly[3]});
    else {
      // 3DFACE holds at most four corners: larger polygons are triangulated
      // by ear clipping (a fan would overlap itself on concave polygons).
      std::vector<ir::Vec3> points;
      points.reserve(poly.size());
      for (const std::uint32_t i : poly) points.push_back(mesh.positions[i]);
      const std::vector<std::uint32_t> tris = cvt::mesh::triangulate_polygon(points);
      for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
        quads.push_back({poly[tris[t]], poly[tris[t + 1]], poly[tris[t + 2]], poly[tris[t + 2]]});
      }
    }
    for (const auto& q : quads) {
      w.tag(0, "3DFACE");
      w.tag(8, "0");
      for (int c = 0; c < 4; ++c) {
        const auto p = at(q[static_cast<std::size_t>(c)]);
        w.point3(10 + c, p[0], p[1], p[2]);
      }
    }
  }
  entities = w.take();
  if (box.empty()) box = {0, 0, 0, 0};
  w.tag(0, "SECTION");
  w.tag(2, "HEADER");
  w.tag(9, "$ACADVER");
  w.tag(1, "AC1009");
  w.tag(9, "$EXTMIN");
  w.point(10, {box.x0, box.y0});
  w.tag(9, "$EXTMAX");
  w.point(10, {box.x1, box.y1});
  w.tag(0, "ENDSEC");
  w.tag(0, "SECTION");
  w.tag(2, "TABLES");
  w.tag(0, "TABLE");
  w.tag(2, "LTYPE");
  w.integer(70, 1);
  w.tag(0, "LTYPE");
  w.tag(2, "CONTINUOUS");
  w.integer(70, 0);
  w.tag(3, "Solid line");
  w.integer(72, 65);
  w.integer(73, 0);
  w.num(40, 0.0);
  w.tag(0, "ENDTAB");
  w.tag(0, "TABLE");
  w.tag(2, "LAYER");
  w.integer(70, 1);
  w.tag(0, "LAYER");
  w.tag(2, "0");
  w.integer(70, 0);
  w.integer(62, 7);
  w.tag(6, "CONTINUOUS");
  w.tag(0, "ENDTAB");
  w.tag(0, "ENDSEC");
  w.tag(0, "SECTION");
  w.tag(2, "ENTITIES");
  w.append(entities);
  w.tag(0, "ENDSEC");
  w.tag(0, "EOF");
  std::string text = w.take();
  CVT_TRY(ctx.limits.check_output(text.size()));
  return to_bytes(text);
}

}  // namespace cvt::vector::cad
