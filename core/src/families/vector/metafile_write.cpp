// Convertible core - vector family: EMF and WMF writers.
//
// Both writers walk the scene with the accumulated transform and draw in
// page space. EMF: MM_ANISOTROPIC with 16 logical units per CSS pixel, a
// 96 dpi reference device, paths as BeginPath/PolyBezierTo/EndPath brackets,
// geometric pens, clipping with SelectClipPath, transparency through
// AlphaBlend, gradients as clipped colour bands, pictures as StretchDIBits or
// AlphaBlend. WMF: a placeable header, flattened polygons, rectangular clips
// only and no transparency (approximated, with warnings).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/metafile_internal.hpp"
#include "families/vector/render.hpp"
#include "util/text.hpp"

namespace cvt::vector::metafile {

namespace {

constexpr double pi = std::numbers::pi;

std::uint32_t colorref(Color c) {
  const auto ch = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  return ch(c.r) | (ch(c.g) << 8) | (ch(c.b) << 16);
}

Color blend_on_white(Color c, double a) {
  const auto mix = [a](float v) { return static_cast<float>(v * a + (1.0 - a)); };
  return {mix(c.r), mix(c.g), mix(c.b), 1.0f};
}

/// Colour of a gradient at parameter t (stops, spread).
Color gradient_color(const Gradient& g, double t) {
  if (g.stops.empty()) return {0, 0, 0, 0};
  switch (g.spread) {
    case Spread::Repeat: t = t - std::floor(t); break;
    case Spread::Reflect: {
      const double m = std::fmod(std::abs(t), 2.0);
      t = m > 1 ? 2 - m : m;
      break;
    }
    default: t = std::clamp(t, 0.0, 1.0); break;
  }
  if (t <= g.stops.front().offset) return g.stops.front().color;
  for (std::size_t i = 1; i < g.stops.size(); ++i) {
    const GradientStop& a = g.stops[i - 1];
    const GradientStop& b = g.stops[i];
    if (t <= b.offset) {
      const double span = b.offset - a.offset;
      const double f = span > 1e-12 ? (t - a.offset) / span : 1.0;
      const auto mix = [f](float x, float y) { return static_cast<float>(x + (y - x) * f); };
      return {mix(a.color.r, b.color.r), mix(a.color.g, b.color.g), mix(a.color.b, b.color.b), mix(a.color.a, b.color.a)};
    }
  }
  return g.stops.back().color;
}

Color average_color(const Paint& paint) {
  if (paint.type == Paint::Type::Solid) return paint.color;
  if (paint.type == Paint::Type::Gradient && paint.gradient && !paint.gradient->stops.empty()) {
    double r = 0, g = 0, b = 0, a = 0;
    for (const auto& s : paint.gradient->stops) {
      r += s.color.r;
      g += s.color.g;
      b += s.color.b;
      a += s.color.a;
    }
    const double n = static_cast<double>(paint.gradient->stops.size());
    return {static_cast<float>(r / n), static_cast<float>(g / n), static_cast<float>(b / n), static_cast<float>(a / n)};
  }
  if (paint.type == Paint::Type::Pattern && paint.pattern && paint.pattern->content) {
    for (const Node& n : paint.pattern->content->children) {
      if (const auto* s = std::get_if<Shape>(&n.value)) {
        if (s->fill.type == Paint::Type::Solid) return s->fill.color;
      }
    }
  }
  return {0.5f, 0.5f, 0.5f, 1.0f};
}

std::string face_name(const std::string& family) {
  std::string first(util::trim(util::split(family, ',').empty() ? std::string_view(family) : util::split(family, ',').front()));
  if (first.size() >= 2 && (first.front() == '"' || first.front() == '\'')) first = first.substr(1, first.size() - 2);
  const std::string lower = util::to_lower(first);
  if (lower == "sans-serif" || lower.empty()) return "Arial";
  if (lower == "serif") return "Times New Roman";
  if (lower == "monospace") return "Courier New";
  return first.substr(0, 31);
}

bool is_similarity(const Matrix& m) {
  return std::abs(m.a - m.d) < 1e-9 * (std::abs(m.a) + 1) && std::abs(m.b + m.c) < 1e-9 * (std::abs(m.b) + 1) &&
         m.determinant() > 0;
}

/// Pixels as a bottom-up DIB: BITMAPINFOHEADER + rows (24-bit BGR, or 32-bit premultiplied BGRA).
Bytes make_dib(const ir::Frame& f, bool alpha, bool blend_white) {
  util::ByteWriter w;
  const std::uint32_t bpp = alpha ? 32 : 24;
  const std::uint64_t stride = ((std::uint64_t{f.width} * bpp + 31) / 32) * 4;
  w.u32le(40).i32le(static_cast<std::int32_t>(f.width)).i32le(static_cast<std::int32_t>(f.height)).u16le(1).u16le(
      static_cast<std::uint16_t>(bpp));
  w.u32le(0).u32le(static_cast<std::uint32_t>(stride * f.height)).i32le(3780).i32le(3780).u32le(0).u32le(0);
  for (std::uint32_t row = f.height; row-- > 0;) {
    const std::size_t start = w.size();
    for (std::uint32_t x = 0; x < f.width; ++x) {
      const std::size_t at = f.offset(x, row);
      std::uint32_t r = f.rgba[at], g = f.rgba[at + 1], b = f.rgba[at + 2];
      const std::uint32_t a = f.rgba[at + 3];
      if (alpha) {
        w.u8(static_cast<std::uint8_t>((b * a + 127) / 255))
            .u8(static_cast<std::uint8_t>((g * a + 127) / 255))
            .u8(static_cast<std::uint8_t>((r * a + 127) / 255))
            .u8(static_cast<std::uint8_t>(a));
      } else {
        if (blend_white && a < 255) {
          r = (r * a + 255 * (255 - a) + 127) / 255;
          g = (g * a + 255 * (255 - a) + 127) / 255;
          b = (b * a + 255 * (255 - a) + 127) / 255;
        }
        w.u8(static_cast<std::uint8_t>(b)).u8(static_cast<std::uint8_t>(g)).u8(static_cast<std::uint8_t>(r));
      }
    }
    while (w.size() - start < stride) w.u8(0);
  }
  return w.take();
}

bool has_transparency(const ir::Frame& f) {
  for (std::size_t i = 3; i < f.rgba.size(); i += 4) {
    if (f.rgba[i] != 255) return true;
  }
  return false;
}

/// Does `path` describe one axis-aligned rectangle? (bounds returned)
std::optional<Rect> as_rect(const Path& path) {
  if (path.points.size() < 4 || path.points.size() > 5) return std::nullopt;
  for (const Verb v : path.verbs) {
    if (v == Verb::Quad || v == Verb::Cubic) return std::nullopt;
  }
  Rect r;
  for (const Point p : path.points) r.add(p);
  for (const Point p : path.points) {
    const bool on_x = std::abs(p.x - r.x0) < 1e-6 || std::abs(p.x - r.x1) < 1e-6;
    const bool on_y = std::abs(p.y - r.y0) < 1e-6 || std::abs(p.y - r.y1) < 1e-6;
    if (!on_x || !on_y) return std::nullopt;
  }
  return r;
}

/// A gradient as solid colour bands (page space, not clipped to the shape):
/// linear gradients as parallel strips, radial ones as discs from the
/// outside in (the focal point is ignored). Each band overlaps the next one,
/// which is drawn after it, so anti-aliasing leaves no seams.
std::vector<std::pair<Path, Color>> gradient_bands(const Path& page, const Gradient& g, int steps_option) {
  std::vector<std::pair<Path, Color>> out;
  if (g.stops.empty() || !g.transform.invertible()) return out;
  const Matrix inv = g.transform.inverted();
  const Rect gb = vector::bounds(page, inv);
  if (gb.empty()) return out;
  const int steps = std::clamp(steps_option, 2, 1024);
  const auto add = [&](const Path& gradient_space, double t) {
    Color c = gradient_color(g, t);
    c.a = 1;
    out.emplace_back(transformed(gradient_space, g.transform), c);
  };
  const Point corners[4] = {{gb.x0, gb.y0}, {gb.x1, gb.y0}, {gb.x0, gb.y1}, {gb.x1, gb.y1}};
  if (g.type == Gradient::Type::Linear) {
    const Point d = g.p2 - g.p1;
    const double len2 = dot(d, d);
    if (!(len2 > 1e-18)) {
      Path p;
      add_rect(p, gb.x0, gb.y0, gb.width(), gb.height());
      add(p, 1.0);
      return out;
    }
    const Point n{-d.y, d.x};
    double t0 = 1e300, t1 = -1e300, s0 = 1e300, s1 = -1e300;
    for (const Point c : corners) {
      const double t = dot(c - g.p1, d) / len2, s = dot(c - g.p1, n) / len2;
      t0 = std::min(t0, t);
      t1 = std::max(t1, t);
      s0 = std::min(s0, s);
      s1 = std::max(s1, s);
    }
    const auto band = [&](double a, double b, double t) {
      const double pad = (b - a) * 0.5;
      Path q;
      q.move_to(g.p1 + d * a + n * s0);
      q.line_to(g.p1 + d * (b + pad) + n * s0);
      q.line_to(g.p1 + d * (b + pad) + n * s1);
      q.line_to(g.p1 + d * a + n * s1);
      q.close();
      add(q, t);
    };
    const auto uniform = [&](double lo, double hi) {
      const auto count = static_cast<std::int64_t>(std::clamp(std::ceil((hi - lo) * steps), 1.0, 20000.0));
      for (std::int64_t i = 0; i < count; ++i) {
        const double a = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(count);
        const double b = lo + (hi - lo) * static_cast<double>(i + 1) / static_cast<double>(count);
        band(a, b, (a + b) / 2);
      }
    };
    if (g.spread == Spread::Pad) {
      if (t0 < 0) band(t0, std::min(0.0, t1), 0.0);
      const double lo = std::max(t0, 0.0), hi = std::min(t1, 1.0);
      if (hi > lo) uniform(lo, hi);
      if (t1 > 1) band(std::max(1.0, t0), t1, 1.0);
    } else {
      uniform(t0, t1);
    }
    return out;
  }
  double far = 0;
  for (const Point c : corners) far = std::max(far, length(c - g.center));
  const double r = g.radius > 1e-12 ? g.radius : 1e-12;
  const double t_max = far / r;
  const double step = 1.0 / steps;
  const auto count = static_cast<std::int64_t>(std::clamp(std::ceil(t_max / step), 1.0, 20000.0));
  if (g.spread == Spread::Pad && t_max > 1) {
    Path disc;
    add_ellipse(disc, g.center.x, g.center.y, far * 1.01, far * 1.01);
    add(disc, 1.0);
  }
  for (std::int64_t i = count; i-- > 0;) {
    const double t = (static_cast<double>(i) + 0.5) * step;
    if (g.spread == Spread::Pad && t > 1) continue;
    Path disc;
    const double rad = (static_cast<double>(i) + 1.25) * step * r;
    add_ellipse(disc, g.center.x, g.center.y, rad, rad);
    add(disc, t);
  }
  return out;
}

/// Sutherland-Hodgman: `subject` clipped to the convex polygon `window`.
std::vector<Point> clip_convex(const std::vector<Point>& subject, const std::vector<Point>& window) {
  std::vector<Point> out = subject;
  if (window.size() < 3) return {};
  // Orientation of the window.
  double area = 0;
  for (std::size_t i = 0; i < window.size(); ++i) area += cross(window[i], window[(i + 1) % window.size()]);
  const double sign = area >= 0 ? 1.0 : -1.0;
  for (std::size_t e = 0; e < window.size() && !out.empty(); ++e) {
    const Point a = window[e], b = window[(e + 1) % window.size()];
    const auto inside = [&](Point p) { return sign * cross(b - a, p - a) >= 0; };
    std::vector<Point> in = std::move(out);
    out.clear();
    for (std::size_t i = 0; i < in.size(); ++i) {
      const Point cur = in[i], prev = in[(i + in.size() - 1) % in.size()];
      const bool ci = inside(cur), pi_ = inside(prev);
      if (ci != pi_) {
        const Point dir = cur - prev, edge = b - a;
        const double den = cross(edge, dir);
        if (std::abs(den) > 1e-18) {
          const double t = cross(edge, a - prev) / den;
          out.push_back(prev + dir * t);
        }
      }
      if (ci) out.push_back(cur);
    }
  }
  return out;
}

/// Picture pixel space -> page.
Matrix picture_matrix(const Picture& pic, const Matrix& m) {
  return m * Matrix::translate(pic.bounds.x0, pic.bounds.y0) *
         Matrix::scale(pic.bounds.width() / pic.pixels->width, pic.bounds.height() / pic.pixels->height);
}

/// `f` drawn through `place` resampled (bilinear) onto an upright bitmap
/// covering the transformed bounds; outside pixels are transparent.
Result<std::pair<ir::Frame, Rect>> straighten(const ir::Frame& f, const Matrix& place, const Context& ctx) {
  const Rect area = transform_rect(Rect::xywh(0, 0, f.width, f.height), place);
  if (area.empty() || !place.invertible()) return fail(Status::Encode, "a picture has no area");
  const double s = std::sqrt(std::abs(place.determinant()));  // page px per source pixel
  // At least the source density, oversampled (up to 8x) for small pictures
  // so that the rotated edges stay smooth.
  const double base_w = std::max(1.0, area.width() / s), base_h = std::max(1.0, area.height() / s);
  const double budget = std::clamp(4.0 * f.width * f.height, 65536.0, 16777216.0);
  const double q = std::clamp(std::sqrt(budget / (base_w * base_h)), 0.05, 8.0);
  double ow = std::ceil(base_w * q), oh = std::ceil(base_h * q);
  ow = std::clamp(ow, 1.0, 16384.0);
  oh = std::clamp(oh, 1.0, 16384.0);
  CVT_TRY(ctx.limits.check_pixels(static_cast<std::uint64_t>(ow), static_cast<std::uint64_t>(oh)));
  ir::Frame out = ir::Frame::filled(static_cast<std::uint32_t>(ow), static_cast<std::uint32_t>(oh));
  const Matrix inv = place.inverted();
  const auto texel = [&](std::int64_t x, std::int64_t y, std::size_t c) -> double {
    if (x < 0 || y < 0 || x >= f.width || y >= f.height) return 0.0;
    const std::size_t at = f.offset(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    if (c == 3) return f.rgba[at + 3];
    return f.rgba[at + c] * (f.rgba[at + 3] / 255.0);  // premultiplied for filtering
  };
  for (std::uint32_t y = 0; y < out.height; ++y) {
    for (std::uint32_t x = 0; x < out.width; ++x) {
      const Point page{area.x0 + (x + 0.5) * area.width() / ow, area.y0 + (y + 0.5) * area.height() / oh};
      const Point src = inv.apply(page);
      const double sx = src.x - 0.5, sy = src.y - 0.5;
      if (src.x < 0 || src.y < 0 || src.x > f.width || src.y > f.height) continue;
      const auto x0 = static_cast<std::int64_t>(std::floor(sx)), y0 = static_cast<std::int64_t>(std::floor(sy));
      const double fx = sx - static_cast<double>(x0), fy = sy - static_cast<double>(y0);
      const auto clampx = [&](std::int64_t v) { return std::clamp<std::int64_t>(v, 0, f.width - 1); };
      const auto clampy = [&](std::int64_t v) { return std::clamp<std::int64_t>(v, 0, f.height - 1); };
      double px[4];
      for (std::size_t c = 0; c < 4; ++c) {
        px[c] = texel(clampx(x0), clampy(y0), c) * (1 - fx) * (1 - fy) + texel(clampx(x0 + 1), clampy(y0), c) * fx * (1 - fy) +
                texel(clampx(x0), clampy(y0 + 1), c) * (1 - fx) * fy + texel(clampx(x0 + 1), clampy(y0 + 1), c) * fx * fy;
      }
      const std::size_t at = out.offset(x, y);
      const double a = px[3];
      out.rgba[at + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0, 255.0)));
      for (std::size_t c = 0; c < 3; ++c) {
        out.rgba[at + c] = a > 0 ? static_cast<std::uint8_t>(std::lround(std::clamp(px[c] * 255.0 / a, 0.0, 255.0))) : 0;
      }
    }
  }
  return std::pair{std::move(out), area};
}

/// The dashes of a stroke as open subpaths (flattened).
Path dashed_path(const Path& page, const Stroke& s) {
  double sum = 0;
  for (const double d : s.dash) sum += d;
  std::vector<Polyline> lines = flatten(page, 0.1);
  if (sum > 0 && std::isfinite(sum)) lines = dash(lines, s.dash, s.dash_offset);
  Path out;
  for (const Polyline& pl : lines) {
    if (pl.points.empty()) continue;
    out.move_to(pl.points.front());
    for (std::size_t i = 1; i < pl.points.size(); ++i) out.line_to(pl.points[i]);
    if (pl.points.size() == 1) out.line_to(pl.points.front());
    if (pl.closed) out.close();
  }
  return out;
}

// --------------------------------------------------------------------------- object slots

/// GDI object handles shared by both writers: lowest free slot first (what
/// WMF playback does), objects created inside a SaveDC level are deleted at
/// the matching RestoreDC.
class Objects {
 public:
  enum Kind : std::size_t { BrushKind = 0, PenKind = 1, FontKind = 2 };

  explicit Objects(std::uint32_t first) : first_(first) {}

  /// Returns the slot to create into, or nullopt when `key` is already selected.
  std::optional<std::uint32_t> need(Kind kind, const std::string& key) {
    if (sel_[kind].slot >= 0 && sel_[kind].key == key) return std::nullopt;
    for (std::size_t i = 0;; ++i) {
      if (i >= level_of_.size()) level_of_.push_back(-1);
      if (level_of_[i] < 0) {
        level_of_[i] = level_;
        max_slots_ = std::max<std::uint32_t>(max_slots_, static_cast<std::uint32_t>(i + 1));
        return static_cast<std::uint32_t>(i) + first_;
      }
    }
  }
  /// After creating and selecting `slot`: returns a slot to delete (the previous object), if any.
  std::optional<std::uint32_t> selected(Kind kind, std::uint32_t slot, const std::string& key) {
    const Sel old = sel_[kind];
    sel_[kind] = {static_cast<int>(slot - first_), key};
    if (old.slot >= 0 && level_of_[static_cast<std::size_t>(old.slot)] == level_) {
      level_of_[static_cast<std::size_t>(old.slot)] = -1;
      return static_cast<std::uint32_t>(old.slot) + first_;
    }
    return std::nullopt;
  }
  void save() {
    saved_.push_back(sel_);
    ++level_;
  }
  /// Slots to delete after RestoreDC.
  std::vector<std::uint32_t> restore() {
    std::vector<std::uint32_t> drop;
    if (saved_.empty()) return drop;
    sel_ = saved_.back();
    saved_.pop_back();
    --level_;
    for (std::size_t i = 0; i < level_of_.size(); ++i) {
      if (level_of_[i] > level_) {
        level_of_[i] = -1;
        drop.push_back(static_cast<std::uint32_t>(i) + first_);
      }
    }
    return drop;
  }
  std::vector<std::uint32_t> all_live() const {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < level_of_.size(); ++i) {
      if (level_of_[i] >= 0) out.push_back(static_cast<std::uint32_t>(i) + first_);
    }
    return out;
  }
  [[nodiscard]] std::uint32_t max_slots() const { return max_slots_; }

 private:
  struct Sel {
    int slot = -1;
    std::string key;
  };
  std::uint32_t first_;
  std::array<Sel, 3> sel_{};
  std::vector<std::array<Sel, 3>> saved_;
  std::vector<int> level_of_;
  int level_ = 0;
  std::uint32_t max_slots_ = 0;
};

// --------------------------------------------------------------------------- shared walk

struct DrawState {
  Matrix m;
  double opacity = 1.0;
};

class Writer {
 public:
  Writer(const Context& ctx, const WriteOptions& options) : ctx_(ctx), options_(options) {}
  virtual ~Writer() = default;
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  Result<void> walk(const Group& group, const DrawState& parent, int depth) {
    if (depth > 256) return fail(Status::Encode, "the drawing is nested too deeply");
    if (!group.visible) return {};
    DrawState st;
    st.m = parent.m * group.transform;
    st.opacity = parent.opacity * std::clamp(group.opacity, 0.0, 1.0);
    if (group.opacity < 1.0 && group.children.size() > 1) {
      ctx_.warn("Group opacity was applied to each shape separately.");
    }
    if (group.mask || group.filter || group.blend != BlendMode::Normal) {
      // Effects metafiles cannot express: draw the group as a picture when the rasteriser can.
      Group alone = group;
      alone.transform = Matrix{};
      CVT_TRY_ASSIGN(const bool drawn, rasterised(alone, parent.m * group.transform, parent.opacity));
      if (drawn) return {};
      if (group.mask) ctx_.warn("Masks are not supported by metafiles and were ignored.");
      if (group.filter) ctx_.warn("Filter effects are not supported by metafiles and were ignored.");
      if (group.blend != BlendMode::Normal) ctx_.warn("Blend modes are not supported by metafiles and were ignored.");
    }
    const bool clipped = static_cast<bool>(group.clip);
    if (clipped) CVT_TRY(push_clip(*group.clip, st.m));
    for (const Node& node : group.children) {
      if (++nodes_ % 512 == 0) CVT_TRY(ctx_.checkpoint(0.5));
      if (const auto* s = std::get_if<Shape>(&node.value)) {
        CVT_TRY(shape(*s, st));
      } else if (const auto* t = std::get_if<Text>(&node.value)) {
        CVT_TRY(text(*t, st));
      } else if (const auto* p = std::get_if<Picture>(&node.value)) {
        CVT_TRY(picture(*p, st));
      } else if (const auto* g = std::get_if<Group>(&node.value)) {
        CVT_TRY(walk(*g, st, depth + 1));
      }
    }
    if (clipped) CVT_TRY(pop_clip());
    return {};
  }

  /// A paint moved from node space into page space (gradients and patterns carry geometry).
  static Paint page_paint(const Paint& paint, const Matrix& m) {
    Paint out = paint;
    if (paint.type == Paint::Type::Gradient && paint.gradient) {
      auto g = std::make_shared<Gradient>(*paint.gradient);
      g->transform = m * g->transform;
      out.gradient = std::move(g);
    } else if (paint.type == Paint::Type::Pattern && paint.pattern) {
      auto p = std::make_shared<PatternPaint>(*paint.pattern);
      p->transform = m * p->transform;
      out.pattern = std::move(p);
    }
    return out;
  }

  Result<void> shape(const Shape& node, const DrawState& st) {
    Shape s = node;
    s.fill = page_paint(node.fill, st.m);
    s.stroke.paint = page_paint(node.stroke.paint, st.m);
    const Path page = transformed(s.path, st.m);
    if (page.empty()) return {};
    if (s.stroke_first) {
      CVT_TRY(stroke(page, s.stroke, st));
      return fill(page, s.fill, s.fill_rule, st.opacity);
    }
    // Opaque solid fill and plain stroke: one record pair draws both.
    if (opaque_solid(s.fill, st.opacity) && opaque_solid(s.stroke.paint, st.opacity) && s.stroke.dash.empty()) {
      const Stroke scaled = scaled_stroke(s.stroke, st);
      if (std::isfinite(scaled.width) && scaled.width >= 0) {
        CVT_TRY_ASSIGN(const bool done, fill_and_stroke(page, s.fill.color, s.fill_rule, scaled));
        if (done) return {};
      }
    }
    if (s.fill.type == Paint::Type::Pattern && s.fill.visible()) {
      // Pattern fills become a picture of the filled shape when the rasteriser is available.
      Group alone;
      Shape filled;
      filled.path = page;
      filled.fill = s.fill;
      filled.fill_rule = s.fill_rule;
      alone.children.push_back(Node{std::move(filled)});
      CVT_TRY_ASSIGN(const bool drawn, rasterised(alone, Matrix{}, st.opacity));
      if (drawn) return stroke(page, s.stroke, st);
    }
    CVT_TRY(fill(page, s.fill, s.fill_rule, st.opacity));
    return stroke(page, s.stroke, st);
  }

  /// Draw `group` (mapped to the page by `m`) as an embedded picture at twice
  /// the page resolution. False when the rasteriser is not available.
  Result<bool> rasterised(const Group& group, const Matrix& m, double opacity) {
    const Rect area = drawing_bounds_of(group, m);
    if (area.empty() || !(area.width() > 0) || !(area.height() > 0)) return true;  // nothing visible
    double scale = 2.0;
    const double pixels = area.width() * area.height() * scale * scale;
    if (pixels > 4.0e6) scale *= std::sqrt(4.0e6 / pixels);
    const auto w = static_cast<std::uint32_t>(std::clamp(std::ceil(area.width() * scale), 1.0, 16384.0));
    const auto h = static_cast<std::uint32_t>(std::clamp(std::ceil(area.height() * scale), 1.0, 16384.0));
    auto frame = render_group(group, Matrix::scale(w / area.width(), h / area.height()) *
                                         Matrix::translate(-area.x0, -area.y0) * m,
                              w, h, ctx_);
    if (!frame) {
      if (frame.error().code == Status::Limit || frame.error().code == Status::Cancelled) return std::unexpected(frame.error());
      return false;
    }
    ctx_.warn("Effects metafiles cannot express were drawn as pictures.");
    Picture pic;
    pic.pixels = std::make_shared<const ir::Frame>(std::move(*frame));
    pic.bounds = area;
    DrawState st;
    st.opacity = opacity;
    CVT_TRY(picture(pic, st));
    return true;
  }

  static Rect drawing_bounds_of(const Group& group, const Matrix& m) {
    Group wrapper;
    wrapper.transform = m;
    wrapper.children.push_back(Node{group});
    return drawing_bounds(wrapper);
  }

  static bool opaque_solid(const Paint& p, double opacity) {
    return p.type == Paint::Type::Solid && p.color.a * p.opacity * opacity >= 0.998;
  }

  static Stroke scaled_stroke(const Stroke& stroke, const DrawState& st) {
    Stroke s = stroke;
    const double scale = st.m.mean_scale();
    if (!stroke.non_scaling && !stroke.hairline) {
      s.width = stroke.width * scale;
      for (double& d : s.dash) d *= scale;
      s.dash_offset *= scale;
    }
    return s;
  }

  /// Draw an opaque fill and an undashed opaque stroke together; false when not possible.
  virtual Result<bool> fill_and_stroke(const Path& page, Color fill, FillRule rule, const Stroke& s) = 0;

  Result<void> stroke(const Path& page, const Stroke& stroke, const DrawState& st) {
    if (!stroke.paint.visible()) return {};
    Stroke s = stroke;
    const double scale = st.m.mean_scale();
    if (!stroke.non_scaling && !stroke.hairline) {
      s.width = stroke.width * scale;
      for (double& d : s.dash) d *= scale;
      s.dash_offset *= scale;
    }
    if (!std::isfinite(s.width) || s.width < 0) return {};
    return stroke_page(page, s, st.opacity);
  }

  virtual Result<void> fill(const Path& page, const Paint& paint, FillRule rule, double opacity) = 0;
  virtual Result<void> stroke_page(const Path& page, const Stroke& s, double opacity) = 0;
  virtual Result<void> text(const Text& t, const DrawState& st) = 0;
  virtual Result<void> picture(const Picture& p, const DrawState& st) = 0;
  virtual Result<void> push_clip(const ClipPath& clip, const Matrix& m) = 0;
  virtual Result<void> pop_clip() = 0;

 protected:
  /// Text drawn as its outline.
  Result<void> text_outline(const Text& t, const DrawState& st) {
    if (t.outline.empty()) return {};
    const Path page = transformed(t.outline, st.m);
    CVT_TRY(fill(page, page_paint(t.fill, st.m), FillRule::NonZero, st.opacity));
    Stroke s = t.stroke;
    s.paint = page_paint(t.stroke.paint, st.m);
    return stroke(page, s, st);
  }

  const Context& ctx_;
  WriteOptions options_;
  std::size_t nodes_ = 0;
};

// --------------------------------------------------------------------------- EMF

class EmfWriter final : public Writer {
 public:
  static constexpr double k = 16.0;  // logical units per page px

  EmfWriter(const Context& ctx, const WriteOptions& options) : Writer(ctx, options), objects_(1) {}

  Result<Bytes> write(const Page& page) {
    const double w = page.width, h = page.height;
    if (!(w > 0) || !(h > 0) || !std::isfinite(w) || !std::isfinite(h) || w > 1e7 || h > 1e7) {
      return fail(Status::Encode, "the drawing has no usable page size");
    }
    const auto vw = static_cast<std::int32_t>(std::max(1.0, std::round(w)));
    const auto vh = static_cast<std::int32_t>(std::max(1.0, std::round(h)));
    {
      util::ByteWriter p;
      p.u32le(8);
      record(17, p);  // SETMAPMODE MM_ANISOTROPIC
    }
    {
      util::ByteWriter p;
      p.i32le(static_cast<std::int32_t>(vw * k)).i32le(static_cast<std::int32_t>(vh * k));
      record(9, p);  // SETWINDOWEXTEX
    }
    {
      util::ByteWriter p;
      p.i32le(vw).i32le(vh);
      record(11, p);  // SETVIEWPORTEXTEX
    }
    {
      util::ByteWriter p;
      p.u32le(1);
      record(18, p);  // SETBKMODE TRANSPARENT
    }
    {
      util::ByteWriter p;
      p.u32le(24);
      record(22, p);  // SETTEXTALIGN TA_BASELINE
    }
    DrawState st;
    st.m = page.root.transform;  // one device pixel per CSS px, 16 logical units each
    Group root = page.root;
    root.transform = Matrix{};
    CVT_TRY(walk(root, st, 0));
    for (const std::uint32_t slot : objects_.all_live()) delete_object(slot);
    {
      util::ByteWriter p;
      p.u32le(0).u32le(16).u32le(20);
      record(14, p);  // EOF: no palette
    }
    return finish(w, h);
  }

 private:
  // ---- low level

  void record(std::uint32_t type, const util::ByteWriter& payload) {
    const std::size_t size = 8 + ((payload.size() + 3) & ~std::size_t{3});
    body_.u32le(type).u32le(static_cast<std::uint32_t>(size));
    body_.bytes(payload.view());
    body_.align(4);
    ++records_;
  }

  std::int32_t lx(double v) const { return static_cast<std::int32_t>(std::lround(std::clamp(v * k, -1.0e9, 1.0e9))); }
  void bounds(util::ByteWriter& p, const Rect& page_rect) const {
    // Device pixels (the page with the viewport scale).
    const Rect r = page_rect.empty() ? Rect{0, 0, 0, 0} : page_rect;
    p.i32le(static_cast<std::int32_t>(std::floor(std::clamp(r.x0, -1e9, 1e9))))
        .i32le(static_cast<std::int32_t>(std::floor(std::clamp(r.y0, -1e9, 1e9))))
        .i32le(static_cast<std::int32_t>(std::ceil(std::clamp(r.x1, -1e9, 1e9))))
        .i32le(static_cast<std::int32_t>(std::ceil(std::clamp(r.y1, -1e9, 1e9))));
  }

  void select(std::uint32_t slot) {
    util::ByteWriter p;
    p.u32le(slot);
    record(37, p);
  }
  void delete_object(std::uint32_t slot) {
    util::ByteWriter p;
    p.u32le(slot);
    record(40, p);
  }
  void fill_mode(FillRule rule) {
    const std::uint32_t mode = rule == FillRule::EvenOdd ? 1 : 2;
    if (fill_mode_ == mode) return;
    util::ByteWriter p;
    p.u32le(mode);
    record(19, p);
    fill_mode_ = mode;
  }
  void save() {
    util::ByteWriter p;
    record(33, p);
    objects_.save();
    saved_modes_.push_back({fill_mode_, miter_, text_color_});
  }
  void restore() {
    util::ByteWriter p;
    p.i32le(-1);
    record(34, p);
    for (const std::uint32_t slot : objects_.restore()) delete_object(slot);
    if (!saved_modes_.empty()) {
      fill_mode_ = saved_modes_.back()[0];
      miter_ = saved_modes_.back()[1];
      text_color_ = saved_modes_.back()[2];
      saved_modes_.pop_back();
    }
  }

  void use_brush(std::optional<Color> color) {
    const std::string key = color ? std::to_string(colorref(*color)) : std::string("null");
    const auto slot = objects_.need(Objects::BrushKind, key);
    if (!slot) return;
    util::ByteWriter p;
    p.u32le(*slot).u32le(color ? 0 : 1).u32le(color ? colorref(*color) : 0).u32le(0);
    record(39, p);
    select(*slot);
    if (const auto old = objects_.selected(Objects::BrushKind, *slot, key)) delete_object(*old);
  }

  void use_pen(const Stroke* s, Color c) {
    std::uint32_t style = 0x10000;  // PS_GEOMETRIC
    std::uint32_t width = 0;
    std::vector<std::uint32_t> dash;
    if (s == nullptr) {
      style = 5;  // PS_NULL
    } else if (s->hairline) {
      style = 0;  // cosmetic solid
      width = 1;
    } else {
      width = static_cast<std::uint32_t>(std::max<long>(1, std::lround(std::min(s->width * k, 1e9))));
      switch (s->cap) {
        case LineCap::Round: break;
        case LineCap::Square: style |= 0x100; break;
        case LineCap::Butt: style |= 0x200; break;
      }
      switch (s->join) {
        case LineJoin::Round: break;
        case LineJoin::Bevel: style |= 0x1000; break;
        default: style |= 0x2000; break;
      }
      if (!s->dash.empty()) {
        style |= 7;  // PS_USERSTYLE
        for (const double d : s->dash) dash.push_back(static_cast<std::uint32_t>(std::max<long>(0, std::lround(std::min(d * k, 1e9)))));
        if (dash.size() > 16) dash.resize(16);
      }
    }
    if (s != nullptr && !s->hairline && (s->join == LineJoin::Miter || s->join == LineJoin::MiterClip)) {
      const auto limit = static_cast<std::uint32_t>(std::clamp(std::ceil(s->miter_limit), 1.0, 100.0));
      if (limit != miter_) {
        util::ByteWriter p;
        p.u32le(limit);
        record(58, p);
        miter_ = limit;
      }
    }
    std::string key = std::to_string(style) + "/" + std::to_string(width) + "/" + std::to_string(colorref(c));
    for (const auto d : dash) key += "," + std::to_string(d);
    const auto slot = objects_.need(Objects::PenKind, key);
    if (!slot) return;
    util::ByteWriter p;
    p.u32le(*slot).u32le(0).u32le(0).u32le(0).u32le(0);
    p.u32le(style).u32le(width).u32le(style == 5 ? 1 : 0).u32le(colorref(c)).u32le(0);
    p.u32le(static_cast<std::uint32_t>(dash.size()));
    for (const auto d : dash) p.u32le(d);
    record(95, p);
    select(*slot);
    if (const auto old = objects_.selected(Objects::PenKind, *slot, key)) delete_object(*old);
  }

  /// The path as a path bracket (BeginPath ... EndPath).
  void path_bracket(const Path& page) {
    {
      util::ByteWriter p;
      record(59, p);
    }
    std::vector<Point> batch;
    std::uint32_t batch_type = 0;  // 6 POLYLINETO, 5 POLYBEZIERTO
    const auto flush = [&] {
      if (batch.empty()) return;
      bool small = true;
      for (const Point q : batch) {
        const std::int32_t x = lx(q.x), y = lx(q.y);
        if (x < -32768 || x > 32767 || y < -32768 || y > 32767) small = false;
      }
      Rect r;
      for (const Point q : batch) r.add(q);
      util::ByteWriter p;
      bounds(p, Rect{r.x0, r.y0, r.x1, r.y1});
      p.u32le(static_cast<std::uint32_t>(batch.size()));
      for (const Point q : batch) {
        if (small) {
          p.i16le(static_cast<std::int16_t>(lx(q.x))).i16le(static_cast<std::int16_t>(lx(q.y)));
        } else {
          p.i32le(lx(q.x)).i32le(lx(q.y));
        }
      }
      record(small ? batch_type + 83 : batch_type, p);  // 89 POLYLINETO16, 88 POLYBEZIERTO16
      batch.clear();
    };
    Point cur{};
    std::size_t pi_index = 0;
    for (const Verb v : page.verbs) {
      switch (v) {
        case Verb::Move: {
          flush();
          cur = page.points[pi_index++];
          util::ByteWriter p;
          p.i32le(lx(cur.x)).i32le(lx(cur.y));
          record(27, p);
          break;
        }
        case Verb::Line:
          if (batch_type != 6) flush();
          batch_type = 6;
          cur = page.points[pi_index++];
          batch.push_back(cur);
          break;
        case Verb::Quad: {
          if (batch_type != 5) flush();
          batch_type = 5;
          const Point q = page.points[pi_index], e = page.points[pi_index + 1];
          pi_index += 2;
          batch.push_back(cur + (q - cur) * (2.0 / 3.0));
          batch.push_back(e + (q - e) * (2.0 / 3.0));
          batch.push_back(e);
          cur = e;
          break;
        }
        case Verb::Cubic:
          if (batch_type != 5) flush();
          batch_type = 5;
          batch.push_back(page.points[pi_index]);
          batch.push_back(page.points[pi_index + 1]);
          batch.push_back(page.points[pi_index + 2]);
          cur = page.points[pi_index + 2];
          pi_index += 3;
          break;
        case Verb::Close: {
          flush();
          util::ByteWriter p;
          record(61, p);
          break;
        }
      }
    }
    flush();
    util::ByteWriter p;
    record(60, p);
  }

  void path_op(std::uint32_t type, const Rect& b) {
    util::ByteWriter p;
    bounds(p, b);
    record(type, p);
  }

  void clip_to(const Path& page, FillRule rule) {
    fill_mode(rule);
    path_bracket(page);
    util::ByteWriter p;
    p.u32le(1);  // RGN_AND
    record(67, p);
  }

  /// A 1x1 pixel stretched over `area` with constant alpha (inside the current clip).
  void alpha_rect(const Rect& area, Color c, double alpha) {
    ir::Frame one = ir::Frame::filled(1, 1,
                                      {static_cast<std::uint8_t>(std::lround(c.r * 255)), static_cast<std::uint8_t>(std::lround(c.g * 255)),
                                       static_cast<std::uint8_t>(std::lround(c.b * 255)), 255});
    alpha_blend(one, area, static_cast<std::uint8_t>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255)), false);
  }

  void alpha_blend(const ir::Frame& f, const Rect& area, std::uint8_t constant, bool per_pixel) {
    const Bytes dib = make_dib(f, true, false);
    util::ByteWriter p;
    bounds(p, area);
    p.i32le(lx(area.x0)).i32le(lx(area.y0)).i32le(lx(area.x1) - lx(area.x0)).i32le(lx(area.y1) - lx(area.y0));
    p.u8(0).u8(0).u8(constant).u8(per_pixel ? 1 : 0);
    p.i32le(0).i32le(0);
    p.f32le(1).f32le(0).f32le(0).f32le(1).f32le(0).f32le(0);
    p.u32le(0).u32le(0);
    p.u32le(108).u32le(40).u32le(148).u32le(static_cast<std::uint32_t>(dib.size() - 40));
    p.i32le(static_cast<std::int32_t>(f.width)).i32le(static_cast<std::int32_t>(f.height));
    p.bytes(dib);
    record(114, p);
  }

  void stretch_dib(const ir::Frame& f, const Rect& area) {
    const Bytes dib = make_dib(f, false, false);
    util::ByteWriter p;
    bounds(p, area);
    p.i32le(lx(area.x0)).i32le(lx(area.y0));
    p.i32le(0).i32le(0).i32le(static_cast<std::int32_t>(f.width)).i32le(static_cast<std::int32_t>(f.height));
    p.u32le(80).u32le(40).u32le(120).u32le(static_cast<std::uint32_t>(dib.size() - 40));
    p.u32le(0).u32le(0x00CC0020);
    p.i32le(lx(area.x1) - lx(area.x0)).i32le(lx(area.y1) - lx(area.y0));
    p.bytes(dib);
    record(81, p);
  }

  // ---- Writer

  Result<void> fill(const Path& page, const Paint& paint, FillRule rule, double opacity) override {
    if (!paint.visible() || opacity <= 0) return {};
    const Rect b = vector::bounds(page);
    if (b.empty()) return {};
    if (paint.type == Paint::Type::Solid) {
      const double a = paint.color.a * paint.opacity * opacity;
      if (a <= 0.002) return {};
      if (a >= 0.998) {
        fill_mode(rule);
        use_brush(paint.color);
        use_pen(nullptr, {});
        path_bracket(page);
        path_op(62, b);
        return {};
      }
      save();
      clip_to(page, rule);
      alpha_rect(b, paint.color, a);
      restore();
      return {};
    }
    if (paint.type == Paint::Type::Gradient && paint.gradient) return gradient(page, rule, *paint.gradient, paint.opacity * opacity);
    ctx_.warn("Pattern fills were approximated by a flat colour.");
    Paint flat = Paint::solid(average_color(paint), paint.opacity);
    return fill(page, flat, rule, opacity);
  }

  Result<void> gradient(const Path& page, FillRule rule, const Gradient& g, double opacity) {
    const auto bands = gradient_bands(page, g, options_.gradient_steps);
    if (bands.empty()) return {};
    bool translucent = opacity < 0.998;
    for (const auto& s : g.stops) translucent = translucent || s.color.a < 0.998;
    if (translucent) ctx_.warn("Transparent gradients were drawn as opaque colour bands.");
    save();
    clip_to(page, rule);
    use_pen(nullptr, {});
    fill_mode(FillRule::NonZero);
    for (const auto& [band, color] : bands) {
      use_brush(color);
      path_bracket(band);
      path_op(62, vector::bounds(band));
    }
    restore();
    return {};
  }

  static Path rect_path(const Rect& r) {
    Path p;
    add_rect(p, r.x0, r.y0, r.width(), r.height());
    return p;
  }

  Result<bool> fill_and_stroke(const Path& page, Color fill, FillRule rule, const Stroke& s) override {
    const Rect b = vector::bounds(page);
    if (b.empty()) return false;
    fill_mode(rule);
    use_brush(fill);
    use_pen(&s, s.paint.color);
    path_bracket(page);
    const double half = s.width / 2;
    path_op(63, Rect{b.x0 - half, b.y0 - half, b.x1 + half, b.y1 + half});  // STROKEANDFILLPATH
    return true;
  }

  Result<void> stroke_page(const Path& page, const Stroke& s, double opacity) override {
    const Rect b = vector::bounds(page);
    if (b.empty() && page.points.empty()) return {};
    const bool solid = s.paint.type == Paint::Type::Solid;
    const double a = solid ? s.paint.color.a * s.paint.opacity * opacity : 1.0;
    if (solid && a >= 0.998) {
      use_brush(std::nullopt);
      if (!s.dash.empty()) {
        // Dash patterns (PS_USERSTYLE) are poorly supported by readers: draw the dashes as separate lines.
        Stroke plain = s;
        plain.dash.clear();
        use_pen(&plain, s.paint.color);
        path_bracket(dashed_path(page, s));
      } else {
        use_pen(&s, s.paint.color);
        path_bracket(page);
      }
      const double half = s.width / 2;
      path_op(64, Rect{b.x0 - half, b.y0 - half, b.x1 + half, b.y1 + half});
      return {};
    }
    if (solid && a <= 0.002) return {};
    // Translucent or gradient strokes: fill the stroke outline.
    Stroke outline_style = s;
    if (s.hairline) outline_style.width = std::max(s.width, 1.0);
    const Path outline = stroke_outline(page, outline_style, 0.05);
    return fill(outline, s.paint, FillRule::NonZero, opacity);
  }

  Result<void> text(const Text& t, const DrawState& st) override {
    if (!options_.text_as_fonts || t.content.empty() || t.fill.type != Paint::Type::Solid || !is_similarity(st.m) ||
        t.stroke.paint.visible()) {
      return text_outline(t, st);
    }
    const double a = t.fill.color.a * t.fill.opacity * st.opacity;
    if (a < 0.998) return text_outline(t, st);
    const double scale = st.m.mean_scale();
    const double size = t.font.size * scale;
    if (!(size > 0) || !std::isfinite(size)) return {};
    const double angle = std::atan2(st.m.b, st.m.a);
    const std::u16string u16 = util::utf8_to_utf16(t.content);
    if (u16.empty() || u16.size() > 8192) return text_outline(t, st);
    // Font.
    const std::string face = face_name(t.font.family);
    const auto esc = static_cast<std::int32_t>(std::lround(-angle * 1800 / pi));
    const std::string key = face + "/" + std::to_string(lx(size)) + "/" + std::to_string(t.font.weight) + "/" +
                            std::to_string(t.font.italic) + "/" + std::to_string(esc);
    if (const auto slot = objects_.need(Objects::FontKind, key)) {
      util::ByteWriter p;
      p.u32le(*slot);
      p.i32le(-lx(size)).i32le(0).i32le(esc).i32le(esc).i32le(t.font.weight);
      p.u8(t.font.italic ? 1 : 0).u8(0).u8(0).u8(1).u8(0).u8(0).u8(4).u8(0);
      const std::u16string f16 = util::utf8_to_utf16(face);
      for (std::size_t i = 0; i < 32; ++i) p.u16le(i < f16.size() && i < 31 ? static_cast<std::uint16_t>(f16[i]) : 0);
      record(82, p);
      select(*slot);
      if (const auto old = objects_.selected(Objects::FontKind, *slot, key)) delete_object(*old);
    }
    const std::uint32_t color = colorref(t.fill.color);
    if (color != text_color_) {
      util::ByteWriter p;
      p.u32le(color);
      record(24, p);
      text_color_ = color;
    }
    // Advances from our layout (one per UTF-16 unit).
    const TextLayout layout = layout_text(t.content, t.font);
    const std::u32string u32 = util::utf8_to_utf32(t.content);
    std::vector<std::int32_t> dx;
    for (std::size_t i = 0; i < u32.size(); ++i) {
      const double next = i + 1 < layout.positions.size() ? layout.positions[i + 1] : layout.advance;
      const double cur = i < layout.positions.size() ? layout.positions[i] : layout.advance;
      const std::int32_t step = static_cast<std::int32_t>(std::lround((next - cur) * scale * k));
      dx.push_back(step);
      if (u32[i] > 0xFFFF) dx.push_back(0);
    }
    dx.resize(u16.size(), 0);
    const Point o = st.m.apply(t.origin);
    const Rect b = vector::bounds(t.outline, st.m);
    util::ByteWriter p;
    bounds(p, b);
    p.u32le(1).f32le(0).f32le(0);
    p.i32le(lx(o.x)).i32le(lx(o.y));
    p.u32le(static_cast<std::uint32_t>(u16.size()));
    const std::uint32_t off_string = 76;
    const std::uint32_t string_bytes = static_cast<std::uint32_t>((u16.size() * 2 + 3) & ~std::size_t{3});
    p.u32le(off_string).u32le(0);
    p.i32le(0).i32le(0).i32le(-1).i32le(-1);
    p.u32le(off_string + string_bytes);
    for (const char16_t c : u16) p.u16le(static_cast<std::uint16_t>(c));
    p.align(4);
    for (const std::int32_t d : dx) p.i32le(d);
    record(84, p);
    return {};
  }

  Result<void> picture(const Picture& pic, const DrawState& st) override {
    if (!pic.pixels || !pic.pixels->valid() || pic.bounds.empty()) return {};
    const ir::Frame& f = *pic.pixels;
    const bool alpha = has_transparency(f) || st.opacity < 0.998;
    const Matrix m = st.m;
    const bool axis = std::abs(m.b) < 1e-12 && std::abs(m.c) < 1e-12 && m.a > 0 && m.d > 0;
    if (axis) {
      const Rect area = transform_rect(pic.bounds, m);
      if (alpha) {
        alpha_blend(f, area, static_cast<std::uint8_t>(std::lround(std::clamp(st.opacity, 0.0, 1.0) * 255)), true);
      } else {
        stretch_dib(f, area);
      }
      return {};
    }
    // Rotated, skewed or mirrored: resample onto an upright bitmap with transparent corners.
    CVT_TRY_ASSIGN(const auto upright, straighten(f, picture_matrix(pic, m), ctx_));
    alpha_blend(upright.first, upright.second, static_cast<std::uint8_t>(std::lround(std::clamp(st.opacity, 0.0, 1.0) * 255)),
                true);
    return {};
  }

  Result<void> push_clip(const ClipPath& clip, const Matrix& m) override {
    save();
    const ClipPath* level = &clip;
    int guard = 0;
    while (level != nullptr && guard++ < 64) {
      Path all;
      FillRule rule = FillRule::NonZero;
      for (const auto& item : level->items) {
        append(all, item.path, m);
        rule = item.rule;
      }
      if (level->items.size() > 1) rule = FillRule::NonZero;
      if (all.empty()) {
        Path nothing;
        add_rect(nothing, 0, 0, 0, 0);
        all = nothing;
      }
      clip_to(all, rule);
      level = level->clip.get();
    }
    return {};
  }

  Result<void> pop_clip() override {
    restore();
    return {};
  }

  Result<Bytes> finish(double w, double h) {
    const double frame_w = w * 2540.0 / 96.0, frame_h = h * 2540.0 / 96.0;
    util::ByteWriter head;
    const std::uint32_t header_size = 108;
    head.u32le(1).u32le(header_size);
    head.i32le(0).i32le(0).i32le(static_cast<std::int32_t>(std::ceil(w)) - 1).i32le(static_cast<std::int32_t>(std::ceil(h)) - 1);
    head.i32le(0).i32le(0).i32le(static_cast<std::int32_t>(std::lround(frame_w))).i32le(static_cast<std::int32_t>(std::lround(frame_h)));
    head.u32le(0x464D4520u).u32le(0x10000);
    const std::uint64_t total = header_size + body_.size();
    CVT_TRY(ctx_.limits.check_output(total));
    if (total > 0xFFFFFFFFull) return fail(Status::Encode, "the EMF file would exceed 4 GB");
    head.u32le(static_cast<std::uint32_t>(total)).u32le(records_ + 1);
    head.u16le(static_cast<std::uint16_t>(std::min<std::uint32_t>(objects_.max_slots() + 1, 0xFFFF))).u16le(0);
    head.u32le(0).u32le(0).u32le(0);
    // A 96 dpi reference device: 9600 px over 2540 mm.
    head.i32le(9600).i32le(9600).i32le(2540).i32le(2540);
    head.u32le(0).u32le(0).u32le(0);
    head.u32le(2540000).u32le(2540000);
    Bytes out = head.take();
    const Bytes body = body_.take();
    out.insert(out.end(), body.begin(), body.end());
    return out;
  }

  util::ByteWriter body_;
  std::uint32_t records_ = 0;
  Objects objects_;
  std::uint32_t fill_mode_ = 0;
  std::uint32_t miter_ = 10;
  std::uint32_t text_color_ = 0;
  std::vector<std::array<std::uint32_t, 3>> saved_modes_;
};

// --------------------------------------------------------------------------- WMF

class WmfWriter final : public Writer {
 public:
  WmfWriter(const Context& ctx, const WriteOptions& options) : Writer(ctx, options), objects_(0) {}

  Result<Bytes> write(const Page& page) {
    const double w = page.width, h = page.height;
    if (!(w > 0) || !(h > 0) || !std::isfinite(w) || !std::isfinite(h) || w > 1e7 || h > 1e7) {
      return fail(Status::Encode, "the drawing has no usable page size");
    }
    // Units per px so that the page fits 16-bit coordinates.
    const double big = std::max(w, h);
    double per_px = std::min(16.0, 30000.0 / big);
    inch_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::floor(96.0 * per_px)));
    inch_ = std::min<std::uint32_t>(inch_, 65535);
    k_ = inch_ / 96.0;
    ext_w_ = static_cast<std::int16_t>(std::clamp<long>(std::lround(w * k_), 1, 32767));
    ext_h_ = static_cast<std::int16_t>(std::clamp<long>(std::lround(h * k_), 1, 32767));
    record(0x0103, {8});                     // SETMAPMODE MM_ANISOTROPIC
    record(0x020B, {0, 0});                  // SETWINDOWORG
    record(0x020C, {ext_h_, ext_w_});        // SETWINDOWEXT
    record(0x0102, {1});                     // SETBKMODE TRANSPARENT
    record(0x012E, {24});                    // SETTEXTALIGN TA_BASELINE
    DrawState st;
    st.m = page.root.transform;
    Group root = page.root;
    root.transform = Matrix{};
    CVT_TRY(walk(root, st, 0));
    for (const std::uint32_t slot : objects_.all_live()) record(0x01F0, {static_cast<std::int16_t>(slot)});
    record(0x0000, {});  // EOF
    return finish();
  }

 private:
  void record(std::uint16_t function, std::initializer_list<std::int16_t> params) {
    util::ByteWriter p;
    for (const std::int16_t v : params) p.i16le(v);
    record(function, p);
  }
  void record(std::uint16_t function, const util::ByteWriter& payload) {
    util::ByteWriter p;
    p.bytes(payload.view());
    p.align(2);
    const std::size_t words = 3 + p.size() / 2;
    body_.u32le(static_cast<std::uint32_t>(words)).u16le(function);
    body_.bytes(p.view());
    max_record_ = std::max<std::uint32_t>(max_record_, static_cast<std::uint32_t>(words));
  }
  std::int16_t lx(double v) const { return static_cast<std::int16_t>(std::clamp<long>(std::lround(std::clamp(v * k_, -1e6, 1e6)), -32768, 32767)); }

  void save() {
    record(0x001E, {});
    objects_.save();
    saved_modes_.push_back(fill_mode_);
    saved_text_.push_back(text_color_);
  }
  void restore() {
    record(0x0127, {-1});
    for (const std::uint32_t slot : objects_.restore()) record(0x01F0, {static_cast<std::int16_t>(slot)});
    if (!saved_modes_.empty()) {
      fill_mode_ = saved_modes_.back();
      saved_modes_.pop_back();
      text_color_ = saved_text_.back();
      saved_text_.pop_back();
    }
  }
  void fill_mode(FillRule rule) {
    const std::int16_t mode = rule == FillRule::EvenOdd ? 1 : 2;
    if (mode == fill_mode_) return;
    record(0x0106, {mode});
    fill_mode_ = mode;
  }
  void use_brush(std::optional<Color> c) {
    const std::string key = c ? std::to_string(colorref(*c)) : std::string("null");
    const auto slot = objects_.need(Objects::BrushKind, key);
    if (!slot) return;
    util::ByteWriter p;
    p.u16le(c ? 0 : 1).u32le(c ? colorref(*c) : 0).u16le(0);
    record(0x02FC, p);
    record(0x012D, {static_cast<std::int16_t>(*slot)});
    if (const auto old = objects_.selected(Objects::BrushKind, *slot, key)) record(0x01F0, {static_cast<std::int16_t>(*old)});
  }
  void use_pen(const Stroke* s, Color c) {
    std::uint16_t style = 5;
    std::int16_t width = 0;
    if (s != nullptr) {
      style = 0;
      if (!s->hairline) {
        width = static_cast<std::int16_t>(std::clamp<long>(std::lround(s->width * k_), 1, 32767));
        switch (s->cap) {
          case LineCap::Round: break;
          case LineCap::Square: style |= 0x100; break;
          case LineCap::Butt: style |= 0x200; break;
        }
        switch (s->join) {
          case LineJoin::Round: break;
          case LineJoin::Bevel: style |= 0x1000; break;
          default: style |= 0x2000; break;
        }
      }
    }
    const std::string key = std::to_string(style) + "/" + std::to_string(width) + "/" + std::to_string(colorref(c));
    const auto slot = objects_.need(Objects::PenKind, key);
    if (!slot) return;
    util::ByteWriter p;
    p.u16le(style).i16le(width).i16le(0).u32le(colorref(c));
    record(0x02FA, p);
    record(0x012D, {static_cast<std::int16_t>(*slot)});
    if (const auto old = objects_.selected(Objects::PenKind, *slot, key)) record(0x01F0, {static_cast<std::int16_t>(*old)});
  }

  std::vector<std::vector<Point>> polygons(const Path& page) const {
    std::vector<std::vector<Point>> out;
    for (const Polyline& pl : flatten(page, 0.2)) {
      std::vector<Point> pts;
      for (const Point p : pl.points) {
        if (pts.empty() || lx(p.x) != lx(pts.back().x) || lx(p.y) != lx(pts.back().y)) pts.push_back(p);
      }
      if (pts.size() >= 2) out.push_back(std::move(pts));
    }
    return out;
  }

  void points(util::ByteWriter& p, const std::vector<Point>& pts) const {
    for (const Point q : pts) p.i16le(lx(q.x)).i16le(lx(q.y));
  }

  Color flat(const Paint& paint, double opacity) {
    Color c = average_color(paint);
    if (paint.type != Paint::Type::Solid) ctx_.warn("Gradients and patterns were approximated by flat colours in WMF.");
    const double a = c.a * paint.opacity * opacity;
    if (a < 0.998) {
      ctx_.warn("WMF has no transparency; translucent colours were blended onto white.");
      c = blend_on_white(c, a);
    }
    c.a = 1;
    return c;
  }

  Result<void> fill(const Path& page, const Paint& paint, FillRule rule, double opacity) override {
    if (!paint.visible() || opacity <= 0) return {};
    if (paint.type == Paint::Type::Solid && paint.color.a * paint.opacity * opacity <= 0.002) return {};
    auto polys = polygons(page);
    if (polys.empty()) return {};
    if (paint.type == Paint::Type::Gradient && paint.gradient) {
      // Colour bands, each intersected with the shape (bands are convex).
      bool translucent = paint.opacity * opacity < 0.998;
      for (const auto& s : paint.gradient->stops) translucent = translucent || s.color.a < 0.998;
      if (translucent) ctx_.warn("WMF has no transparency; gradients were drawn opaque.");
      for (const auto& [band, color] : gradient_bands(page, *paint.gradient, std::min(options_.gradient_steps, 64))) {
        const auto window = flatten(band, 0.2);
        if (window.empty()) continue;
        std::vector<std::vector<Point>> pieces;
        for (const auto& poly : polys) {
          auto piece = clip_convex(poly, window.front().points);
          if (piece.size() >= 3) pieces.push_back(std::move(piece));
        }
        if (!pieces.empty()) fill_polygons(std::move(pieces), rule, color);
      }
      return {};
    }
    fill_polygons(std::move(polys), rule, flat(paint, opacity));
    return {};
  }

  void fill_polygons(std::vector<std::vector<Point>> polys, FillRule rule, Color color) {
    fill_mode(rule);
    use_brush(color);
    use_pen(nullptr, {});
    emit_polygons(std::move(polys));
  }

  void emit_polygons(std::vector<std::vector<Point>> polys) {
    std::size_t total = 0;
    for (auto& poly : polys) {
      if (poly.size() > 32000) poly.resize(32000);
      total += poly.size();
    }
    if (polys.size() == 1) {
      util::ByteWriter p;
      p.u16le(static_cast<std::uint16_t>(polys[0].size()));
      points(p, polys[0]);
      record(0x0324, p);
      return;
    }
    if (polys.size() > 32000 || total > 1000000) {
      ctx_.warn("Very complex shapes were simplified for WMF.");
      polys.resize(std::min<std::size_t>(polys.size(), 32000));
    }
    util::ByteWriter p;
    p.u16le(static_cast<std::uint16_t>(polys.size()));
    for (const auto& poly : polys) p.u16le(static_cast<std::uint16_t>(poly.size()));
    for (const auto& poly : polys) points(p, poly);
    record(0x0538, p);
  }

  Result<bool> fill_and_stroke(const Path& page, Color fill, FillRule rule, const Stroke& s) override {
    // Only closed outlines: PolyPolygon strokes every polygon closed.
    for (std::size_t i = 0, open = 0; i < page.verbs.size(); ++i) {
      if (page.verbs[i] == Verb::Move) {
        if (open != 0) return false;
        open = 1;
      } else if (page.verbs[i] == Verb::Close) {
        open = 0;
      } else if (i + 1 == page.verbs.size()) {
        return false;
      }
    }
    auto polys = polygons(page);
    if (polys.empty()) return false;
    fill_mode(rule);
    use_brush(fill);
    use_pen(&s, s.paint.color);
    emit_polygons(std::move(polys));
    return true;
  }

  Result<void> stroke_page(const Path& page, const Stroke& s, double opacity) override {
    std::vector<Polyline> lines = flatten(page, 0.2);
    if (lines.empty()) return {};
    if (!s.dash.empty()) {
      double sum = 0;
      for (const double d : s.dash) sum += d;
      if (sum > 0 && std::isfinite(sum)) lines = dash(lines, s.dash, s.dash_offset);
    }
    use_brush(std::nullopt);
    use_pen(&s, flat(s.paint, opacity));
    for (const Polyline& pl : lines) {
      std::vector<Point> pts;
      for (const Point p : pl.points) {
        if (pts.empty() || lx(p.x) != lx(pts.back().x) || lx(p.y) != lx(pts.back().y)) pts.push_back(p);
      }
      if (pts.size() < 2) {
        if (pts.size() == 1 && s.cap != LineCap::Butt) pts.push_back(pts.front());
        else continue;
      }
      for (std::size_t start = 0; start + 1 < pts.size(); start += 31999) {
        const std::size_t end = std::min(pts.size(), start + 32000);
        std::vector<Point> chunk(pts.begin() + static_cast<std::ptrdiff_t>(start), pts.begin() + static_cast<std::ptrdiff_t>(end));
        util::ByteWriter p;
        p.u16le(static_cast<std::uint16_t>(chunk.size()));
        points(p, chunk);
        record(pl.closed && start == 0 && end == pts.size() ? 0x0324 : 0x0325, p);
      }
    }
    return {};
  }

  Result<void> text(const Text& t, const DrawState& st) override {
    if (!options_.text_as_fonts || t.content.empty() || t.fill.type != Paint::Type::Solid || !is_similarity(st.m) ||
        t.stroke.paint.visible()) {
      return text_outline(t, st);
    }
    // Only Windows-1252 text can be written as WMF text.
    const std::u32string u32 = util::utf8_to_utf32(t.content);
    std::string bytes;
    for (const char32_t c : u32) {
      if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) {
        bytes.push_back(static_cast<char>(c));
        continue;
      }
      return text_outline(t, st);
    }
    if (bytes.size() > 4000) return text_outline(t, st);
    const double scale = st.m.mean_scale();
    const double size = t.font.size * scale;
    const double angle = std::atan2(st.m.b, st.m.a);
    const std::string face = face_name(t.font.family);
    const auto esc = static_cast<std::int16_t>(std::lround(-angle * 1800 / pi));
    const std::string key = face + "/" + std::to_string(lx(size)) + "/" + std::to_string(t.font.weight) + "/" +
                            std::to_string(t.font.italic) + "/" + std::to_string(esc);
    if (const auto slot = objects_.need(Objects::FontKind, key)) {
      util::ByteWriter p;
      p.i16le(static_cast<std::int16_t>(-std::max<std::int16_t>(1, lx(size)))).i16le(0).i16le(esc).i16le(esc);
      p.i16le(static_cast<std::int16_t>(t.font.weight));
      p.u8(t.font.italic ? 1 : 0).u8(0).u8(0).u8(0).u8(0).u8(0).u8(4).u8(0);
      for (std::size_t i = 0; i < 32; ++i) p.u8(i < face.size() && i < 31 ? static_cast<std::uint8_t>(face[i]) : 0);
      record(0x02FB, p);
      record(0x012D, {static_cast<std::int16_t>(*slot)});
      if (const auto old = objects_.selected(Objects::FontKind, *slot, key)) record(0x01F0, {static_cast<std::int16_t>(*old)});
    }
    const std::uint32_t color = colorref(t.fill.color);
    if (color != text_color_) {
      util::ByteWriter p;
      p.u32le(color);
      record(0x0209, p);
      text_color_ = color;
    }
    const TextLayout layout = layout_text(t.content, t.font);
    const Point o = st.m.apply(t.origin);
    util::ByteWriter p;
    p.i16le(lx(o.y)).i16le(lx(o.x)).i16le(static_cast<std::int16_t>(bytes.size())).u16le(0);
    p.text(bytes);
    p.align(2);
    for (std::size_t i = 0; i < u32.size(); ++i) {
      const double next = i + 1 < layout.positions.size() ? layout.positions[i + 1] : layout.advance;
      const double cur = i < layout.positions.size() ? layout.positions[i] : layout.advance;
      p.i16le(static_cast<std::int16_t>(std::clamp<long>(std::lround((next - cur) * scale * k_), -32768, 32767)));
    }
    record(0x0A32, p);
    return {};
  }

  void stretch_dib(const ir::Frame& f, const Rect& area, std::uint32_t rop) {
    const Bytes dib = make_dib(f, false, true);
    util::ByteWriter p;
    p.u32le(rop).u16le(0);
    p.i16le(static_cast<std::int16_t>(std::min<std::uint32_t>(f.height, 32767)))
        .i16le(static_cast<std::int16_t>(std::min<std::uint32_t>(f.width, 32767)))
        .i16le(0)
        .i16le(0);
    p.i16le(static_cast<std::int16_t>(lx(area.y1) - lx(area.y0))).i16le(static_cast<std::int16_t>(lx(area.x1) - lx(area.x0)));
    p.i16le(lx(area.y0)).i16le(lx(area.x0));
    p.bytes(dib);
    record(0x0F43, p);
  }

  Result<void> picture(const Picture& pic, const DrawState& st) override {
    if (!pic.pixels || !pic.pixels->valid() || pic.bounds.empty()) return {};
    const Matrix& m = st.m;
    const bool axis = std::abs(m.b) < 1e-12 && std::abs(m.c) < 1e-12 && m.a > 0 && m.d > 0;
    ir::Frame straightened;
    const ir::Frame* f = pic.pixels.get();
    Rect area = transform_rect(pic.bounds, m);
    if (!axis) {
      CVT_TRY_ASSIGN(auto upright, straighten(*f, picture_matrix(pic, m), ctx_));
      straightened = std::move(upright.first);
      area = upright.second;
      f = &straightened;
    }
    if (!has_transparency(*f) && st.opacity >= 0.998) {
      stretch_dib(*f, area, 0x00CC0020);
      return {};
    }
    // WMF has no alpha: show pixels at least half opaque, through an AND mask
    // followed by an OR of the colours (the classic transparent blit).
    bool partial = st.opacity < 0.998;
    ir::Frame mask = ir::Frame::filled(f->width, f->height, {255, 255, 255, 255});
    ir::Frame colour = ir::Frame::filled(f->width, f->height, {0, 0, 0, 255});
    for (std::size_t i = 0; i < f->rgba.size(); i += 4) {
      const double a = f->rgba[i + 3] * std::clamp(st.opacity, 0.0, 1.0);
      if (a > 0 && a < 255) partial = true;
      if (a >= 127.5) {
        mask.rgba[i] = mask.rgba[i + 1] = mask.rgba[i + 2] = 0;
        colour.rgba[i] = f->rgba[i];
        colour.rgba[i + 1] = f->rgba[i + 1];
        colour.rgba[i + 2] = f->rgba[i + 2];
      }
    }
    if (partial) ctx_.warn("WMF has no transparency; semi-transparent picture pixels were made fully opaque or clear.");
    stretch_dib(mask, area, 0x008800C6);    // SRCAND
    stretch_dib(colour, area, 0x00EE0086);  // SRCPAINT
    return {};
  }

  Result<void> push_clip(const ClipPath& clip, const Matrix& m) override {
    save();
    const ClipPath* level = &clip;
    int guard = 0;
    while (level != nullptr && guard++ < 64) {
      Path all;
      for (const auto& item : level->items) append(all, item.path, m);
      Rect r = vector::bounds(all);
      if (!as_rect(all) && !warned_clip_) {
        ctx_.warn("WMF clips only to rectangles; other clipping shapes were replaced by their bounding boxes.");
        warned_clip_ = true;
      }
      if (r.empty()) r = Rect{0, 0, 0, 0};
      record(0x0416, {lx(r.y1), lx(r.x1), lx(r.y0), lx(r.x0)});
      level = level->clip.get();
    }
    return {};
  }

  Result<void> pop_clip() override {
    restore();
    return {};
  }

  Result<Bytes> finish() {
    util::ByteWriter out;
    // Placeable header.
    out.u32le(0x9AC6CDD7u).u16le(0);
    out.i16le(0).i16le(0).i16le(ext_w_).i16le(ext_h_);
    out.u16le(static_cast<std::uint16_t>(inch_)).u32le(0);
    std::uint16_t checksum = 0;
    {
      const ByteView v = out.view();
      for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
        checksum ^= static_cast<std::uint16_t>(static_cast<std::uint8_t>(v[i]) | (static_cast<std::uint8_t>(v[i + 1]) << 8));
      }
    }
    out.u16le(checksum);
    const std::uint64_t words = 9 + body_.size() / 2;
    CVT_TRY(ctx_.limits.check_output(22 + words * 2));
    if (words > 0xFFFFFFFFull) return fail(Status::Encode, "the WMF file is too large");
    out.u16le(1).u16le(9).u16le(0x0300).u32le(static_cast<std::uint32_t>(words));
    out.u16le(static_cast<std::uint16_t>(std::min<std::uint32_t>(objects_.max_slots(), 0xFFFF)));
    out.u32le(max_record_).u16le(0);
    out.bytes(body_.view());
    return out.take();
  }

  util::ByteWriter body_;
  Objects objects_;
  std::uint32_t inch_ = 1536;
  double k_ = 16;
  std::int16_t ext_w_ = 1, ext_h_ = 1;
  std::uint32_t max_record_ = 3;
  std::int16_t fill_mode_ = 0;
  std::uint32_t text_color_ = 0;
  std::vector<std::int16_t> saved_modes_;
  std::vector<std::uint32_t> saved_text_;
  bool warned_clip_ = false;
};

}  // namespace

Result<Bytes> write_emf(const Scene& scene, const Context& ctx, const WriteOptions& options) {
  if (scene.pages.empty()) return fail(Status::Encode, "the drawing has no pages");
  if (scene.pages.size() > 1) ctx.warn("EMF holds one page; only the first page was written.");
  EmfWriter writer(ctx, options);
  return writer.write(scene.pages.front());
}

Result<Bytes> write_wmf(const Scene& scene, const Context& ctx, const WriteOptions& options) {
  if (scene.pages.empty()) return fail(Status::Encode, "the drawing has no pages");
  if (scene.pages.size() > 1) ctx.warn("WMF holds one page; only the first page was written.");
  WmfWriter writer(ctx, options);
  return writer.write(scene.pages.front());
}

}  // namespace cvt::vector::metafile
