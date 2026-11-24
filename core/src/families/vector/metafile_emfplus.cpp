// Convertible core - vector family: EMF+ records embedded in EMF comments, after [MS-EMFPLUS].
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "families/image/image_api.hpp"
#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/metafile_internal.hpp"
#include "util/text.hpp"

namespace cvt::vector::metafile {

namespace {

constexpr double pi = std::numbers::pi;
constexpr std::size_t max_region_nodes = 4096;
constexpr int max_region_depth = 64;
constexpr std::uint64_t max_object_bytes = 512ull * 1024 * 1024;

Color argb(std::uint32_t v) {
  return Color::rgb8(static_cast<int>((v >> 16) & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>(v & 0xFF),
                     static_cast<int>(v >> 24));
}

bool finite_matrix(const Matrix& m) {
  return std::isfinite(m.a) && std::isfinite(m.b) && std::isfinite(m.c) && std::isfinite(m.d) && std::isfinite(m.e) &&
         std::isfinite(m.f);
}

Matrix read_matrix(util::ByteReader& r) {
  Matrix m;
  m.a = r.f32le();
  m.b = r.f32le();
  m.c = r.f32le();
  m.d = r.f32le();
  m.e = r.f32le();
  m.f = r.f32le();
  return finite_matrix(m) ? m : Matrix{};
}

/// One EmfPlusInteger7 / EmfPlusInteger15.
std::int32_t read_relative(util::ByteReader& r) {
  const std::uint8_t first = r.u8();
  if ((first & 0x80) == 0) return (first & 0x40) != 0 ? static_cast<std::int32_t>(first) - 128 : first;
  const std::uint8_t second = r.u8();
  const std::int32_t v = ((first & 0x7F) << 8) | second;
  return (v & 0x4000) != 0 ? v - 0x8000 : v;
}

std::vector<Point> read_points(util::ByteReader& r, std::uint32_t count, bool compressed, bool relative) {
  std::vector<Point> out;
  const std::size_t min_each = relative ? 2 : (compressed ? 4 : 8);
  if (std::uint64_t{count} * min_each > r.remaining()) return out;
  out.reserve(count);
  Point last{0, 0};
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    Point p;
    if (relative) {
      p.x = last.x + read_relative(r);
      p.y = last.y + read_relative(r);
    } else if (compressed) {
      p.x = r.i16le();
      p.y = r.i16le();
    } else {
      p.x = r.f32le();
      p.y = r.f32le();
    }
    last = p;
    out.push_back(p);
  }
  if (!r.ok()) out.clear();
  return out;
}

Rect read_rect(util::ByteReader& r, bool compressed) {
  double x, y, w, h;
  if (compressed) {
    x = r.i16le();
    y = r.i16le();
    w = r.i16le();
    h = r.i16le();
  } else {
    x = r.f32le();
    y = r.f32le();
    w = r.f32le();
    h = r.f32le();
  }
  return Rect{x, y, w, h};  // x0,y0 = origin; x1,y1 = width, height
}

Path rect_path(const Rect& xywh) {
  Path p;
  p.move_to({xywh.x0, xywh.y0});
  p.line_to({xywh.x0 + xywh.x1, xywh.y0});
  p.line_to({xywh.x0 + xywh.x1, xywh.y0 + xywh.y1});
  p.line_to({xywh.x0, xywh.y0 + xywh.y1});
  p.close();
  return p;
}

/// Cardinal spline through `pts` as cubic Béziers (GDI+ tension semantics).
Path cardinal(const std::vector<Point>& pts, double tension, bool closed) {
  Path p;
  const std::size_t n = pts.size();
  if (n < 2) return p;
  const double t = tension / 3.0;
  const auto at = [&](std::ptrdiff_t i) -> Point {
    if (closed) return pts[static_cast<std::size_t>((i % static_cast<std::ptrdiff_t>(n) + static_cast<std::ptrdiff_t>(n)) %
                                                    static_cast<std::ptrdiff_t>(n))];
    return pts[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(i, 0, static_cast<std::ptrdiff_t>(n) - 1))];
  };
  p.move_to(pts[0]);
  const std::size_t segments = closed ? n : n - 1;
  for (std::size_t s = 0; s < segments; ++s) {
    const auto i = static_cast<std::ptrdiff_t>(s);
    const Point p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
    p.cubic_to(p1 + (p2 - p0) * t, p2 - (p3 - p1) * t, p2);
  }
  if (closed) p.close();
  return p;
}

// ---- objects

struct PlusImage {
  FramePtr frame;
  std::shared_ptr<const Scene> metafile;
  double width = 0, height = 0;  // pixels
};

struct PlusBrush {
  enum class Type : std::uint8_t { Solid, Hatch, Texture, PathGradient, Linear };
  Type type = Type::Solid;
  Color color{0, 0, 0, 1};
  Color back{0, 0, 0, 0};
  std::uint32_t hatch = 0;
  std::int32_t wrap = 0;
  Rect rect;  // linear: x, y, w, h
  std::vector<GradientStop> stops;
  Matrix transform;
  Point center;
  Rect boundary;  // path gradient bounds
  std::shared_ptr<const PlusImage> image;
};

struct PlusPen {
  PlusBrush brush;
  double width = 1.0;
  std::uint32_t unit = 0;
  Matrix transform;
  LineCap cap = LineCap::Butt;
  LineJoin join = LineJoin::Miter;
  double miter_limit = 10.0;
  std::int32_t style = 0;
  std::vector<double> dash;
  double dash_offset = 0.0;
};

struct PlusPath {
  Path path;
};

struct RegionNode {
  std::uint32_t type = 0x10000003u;
  Rect rect;  // x, y, w, h
  Path path;
  std::size_t left = 0, right = 0;
};

struct PlusRegion {
  std::vector<RegionNode> nodes;  // nodes[0] is the root
};

struct PlusFont {
  double em = 12.0;
  std::uint32_t unit = 3;
  std::int32_t style = 0;
  std::string family;
};

struct PlusFormat {
  std::uint32_t flags = 0;
  std::uint32_t align = 0;
  std::uint32_t line_align = 0;
};

using PlusObject = std::variant<std::monostate, PlusBrush, PlusPen, PlusPath, PlusRegion,
                                std::shared_ptr<const PlusImage>, PlusFont, PlusFormat>;

struct Saved {
  std::uint32_t id = 0;
  Matrix world;
  std::uint32_t unit = 2;
  double scale = 1.0;
  ClipRef clip;
};

}  // namespace

struct EmfPlusPlayer::Impl {
  GdiPlayer& gdi;
  ReadOptions options;
  int depth;
  bool active = false;
  double dpi_x = 96.0, dpi_y = 96.0;
  Matrix world;
  std::uint32_t page_unit = 2;
  double page_scale = 1.0;
  ClipRef clip;
  std::vector<Saved> stack;
  std::array<PlusObject, 64> objects{};
  // continued objects
  Bytes partial;
  std::uint64_t partial_total = 0;
  int partial_id = -1;
  std::uint16_t partial_flags = 0;

  Impl(GdiPlayer& g, const ReadOptions& o, int d) : gdi(g), options(o), depth(d) {}

  // ---- coordinate systems

  [[nodiscard]] double unit_factor(std::uint32_t unit, bool vertical) const {
    const double dpi = vertical ? dpi_y : dpi_x;
    switch (unit) {
      case 3: return dpi / 72.0;
      case 4: return dpi;
      case 5: return dpi / 300.0;
      case 6: return dpi / 25.4;
      default: return 1.0;  // world, display, pixel
    }
  }
  [[nodiscard]] Matrix page_matrix() const {
    return Matrix::scale(unit_factor(page_unit, false) * page_scale, unit_factor(page_unit, true) * page_scale);
  }
  /// World -> page px.
  [[nodiscard]] Matrix total() const { return gdi.device_to_page() * page_matrix() * world; }

  // ---- paint

  Paint make_paint(const PlusBrush& b, const Matrix& m) {
    switch (b.type) {
      case PlusBrush::Type::Solid: return Paint::solid(b.color);
      case PlusBrush::Type::Hatch: {
        if (b.hatch <= 5) return gdi.hatch_paint(b.hatch, b.color, b.back.a > 0, b.back);
        // Percentage and decorative hatches: a flat mix of the two colours.
        double share = 0.5;
        if (b.hatch >= 6 && b.hatch <= 13) {
          static constexpr std::array<double, 8> percent = {0.05, 0.10, 0.20, 0.25, 0.30, 0.40, 0.50, 0.60};
          share = percent[b.hatch - 6];
        }
        const auto mix = [&](float f, float k) { return static_cast<float>(f * share + k * (1 - share)); };
        gdi.note("Some EMF+ hatch brushes were approximated by flat colours.");
        return Paint::solid({mix(b.color.r, b.back.r), mix(b.color.g, b.back.g), mix(b.color.b, b.back.b),
                             mix(b.color.a, b.back.a)});
      }
      case PlusBrush::Type::Linear: {
        auto g = std::make_shared<Gradient>();
        g->type = Gradient::Type::Linear;
        g->p1 = {b.rect.x0, b.rect.y0};
        g->p2 = {b.rect.x0 + b.rect.x1, b.rect.y0};
        g->stops = b.stops;
        g->spread = b.wrap == 4 ? Spread::Pad : (b.wrap == 0 ? Spread::Repeat : Spread::Reflect);
        g->transform = m * b.transform;
        Paint p;
        p.type = Paint::Type::Gradient;
        p.gradient = std::move(g);
        return p;
      }
      case PlusBrush::Type::PathGradient: {
        gdi.note("EMF+ path gradients were approximated by radial gradients.");
        auto g = std::make_shared<Gradient>();
        g->type = Gradient::Type::Radial;
        g->center = {0, 0};
        g->focal = {0, 0};
        g->radius = 1.0;
        g->stops = b.stops;
        const double rx = std::max(std::abs(b.center.x - b.boundary.x0), std::abs(b.boundary.x0 + b.boundary.x1 - b.center.x));
        const double ry = std::max(std::abs(b.center.y - b.boundary.y0), std::abs(b.boundary.y0 + b.boundary.y1 - b.center.y));
        g->transform = m * b.transform * Matrix::translate(b.center.x, b.center.y) *
                       Matrix::scale(std::max(rx, 1e-6), std::max(ry, 1e-6));
        Paint p;
        p.type = Paint::Type::Gradient;
        p.gradient = std::move(g);
        return p;
      }
      case PlusBrush::Type::Texture: {
        if (!b.image || !b.image->frame) return Paint::solid({0.5f, 0.5f, 0.5f, 1});
        auto content = std::make_shared<Group>();
        Picture pic;
        pic.pixels = b.image->frame;
        pic.bounds = Rect::xywh(0, 0, b.image->frame->width, b.image->frame->height);
        content->children.push_back(Node{std::move(pic)});
        auto pattern = std::make_shared<PatternPaint>();
        pattern->tile = Rect::xywh(0, 0, b.image->frame->width, b.image->frame->height);
        pattern->transform = m * b.transform;
        pattern->content = std::move(content);
        Paint p;
        p.type = Paint::Type::Pattern;
        p.pattern = std::move(pattern);
        return p;
      }
    }
    return Paint::solid(b.color);
  }

  /// The brush of a fill record: an ARGB colour (S flag) or an object.
  std::optional<Paint> brush_of(std::uint16_t flags, std::uint32_t id, const Matrix& m) {
    if ((flags & 0x8000) != 0) return Paint::solid(argb(id));
    if (id >= objects.size()) return std::nullopt;
    if (const auto* b = std::get_if<PlusBrush>(&objects[id])) return make_paint(*b, m);
    return std::nullopt;
  }

  std::optional<Stroke> pen_of(std::uint32_t id, const Matrix& m) {
    if (id >= objects.size()) return std::nullopt;
    const auto* pen = std::get_if<PlusPen>(&objects[id]);
    if (pen == nullptr) return std::nullopt;
    Stroke s;
    s.paint = make_paint(pen->brush, m);
    double width = pen->width * pen->transform.mean_scale();
    if (pen->unit == 0 || pen->unit == 1) {
      width *= m.mean_scale();
    } else {
      width *= unit_factor(pen->unit, false) * gdi.device_to_page().mean_scale();
    }
    const double dev = gdi.device_pixel();
    if (!std::isfinite(width) || width <= dev) {
      width = dev;
      s.hairline = true;
    }
    s.width = width;
    s.cap = pen->cap;
    s.join = pen->join;
    s.miter_limit = std::clamp(pen->miter_limit, 1.0, 100.0);
    const double u = width;
    switch (pen->style) {
      case 1: s.dash = {3 * u, u}; break;
      case 2: s.dash = {u, u}; break;
      case 3: s.dash = {3 * u, u, u, u}; break;
      case 4: s.dash = {3 * u, u, u, u, u, u}; break;
      case 5: {
        double sum = 0;
        for (const double d : pen->dash) {
          s.dash.push_back(std::max(0.0, d) * u);
          sum += s.dash.back();
        }
        if (s.dash.size() % 2 == 1) {
          const auto copy = s.dash;
          s.dash.insert(s.dash.end(), copy.begin(), copy.end());
        }
        if (!(sum > 0) || !std::isfinite(sum)) s.dash.clear();
        break;
      }
      default: break;
    }
    s.dash_offset = std::isfinite(pen->dash_offset) ? pen->dash_offset * u : 0.0;
    return s;
  }

  // ---- emission

  Result<void> draw(const Path& world_path, const std::optional<Paint>& fill, FillRule rule,
                    const std::optional<Stroke>& stroke) {
    const Matrix m = total();
    Path page = transformed(world_path, m);
    for (const Point p : page.points) {
      if (!std::isfinite(p.x) || !std::isfinite(p.y)) return {};
    }
    if (page.empty()) return {};
    const bool has_fill = fill && fill->visible();
    const bool has_stroke = stroke && stroke->paint.visible();
    if (!has_fill && !has_stroke) return {};
    CVT_TRY(gdi.flush());
    Shape shape;
    shape.path = std::move(page);
    if (has_fill) shape.fill = *fill;
    shape.fill_rule = rule;
    if (has_stroke) shape.stroke = *stroke;
    return gdi.out().add_shape(std::move(shape), clip);
  }

  Result<void> add_node(Node node) {
    CVT_TRY(gdi.flush());
    return gdi.out().add(std::move(node), clip);
  }

  // ---- regions

  Region eval_region(const PlusRegion& region, std::size_t index, const Matrix& m, int depth_left) {
    if (index >= region.nodes.size() || depth_left <= 0) return {};
    const RegionNode& n = region.nodes[index];
    switch (n.type) {
      case 0x10000000u: {
        Region r;
        r.infinite = false;
        r.clip = make_clip(transformed(rect_path(n.rect), m), FillRule::NonZero);
        return r;
      }
      case 0x10000001u: {
        Region r;
        r.infinite = false;
        r.clip = make_clip(transformed(n.path, m), FillRule::EvenOdd);
        return r;
      }
      case 0x10000002u: return Region{false, nullptr};
      case 0x10000003u: return Region{};
      default: break;
    }
    const Region a = eval_region(region, n.left, m, depth_left - 1);
    const Region b = eval_region(region, n.right, m, depth_left - 1);
    return combine(a, b, n.type);
  }

  /// a OP b with CombineMode-like codes 1 AND, 2 OR, 3 XOR, 4 EXCLUDE (a - b), 5 COMPLEMENT (b - a).
  Region combine(const Region& a, const Region& b, std::uint32_t op) {
    const auto empty = [](const Region& r) { return !r.infinite && !r.clip; };
    bool approx = false;
    Region out;
    out.infinite = false;
    switch (op) {
      case 1:
        if (a.infinite) return b;
        if (b.infinite) return a;
        if (empty(a) || empty(b)) return out;
        out.clip = combine_clip(a.clip, b.clip, 1, approx);
        break;
      case 2:
      case 3:
        if (a.infinite || b.infinite) return Region{};
        if (empty(a)) return b;
        if (empty(b)) return a;
        out.clip = combine_clip(a.clip, b.clip, 2, approx);
        if (op == 3) approx = true;
        break;
      case 4:
        if (empty(a) || b.infinite) return out;
        if (empty(b)) return a;
        out.clip = combine_clip(a.infinite ? nullptr : a.clip, b.clip, 4, approx);
        break;
      case 5:
        return combine(b, a, 4);
      default:
        return b;
    }
    if (approx) gdi.note("Some clipping regions were approximated.");
    if (!out.clip) out.infinite = true;
    return out;
  }

  void set_clip(const Region& incoming, std::uint32_t mode) {
    // CombineMode: 0 replace, 1 intersect, 2 union, 3 xor, 4 exclude, 5 complement.
    const Region current = clip ? Region{false, clip} : Region{};
    Region result;
    switch (mode) {
      case 0: result = incoming; break;
      case 1: result = combine(current, incoming, 1); break;
      case 2: result = combine(current, incoming, 2); break;
      case 3: result = combine(current, incoming, 3); break;
      case 4: result = combine(current, incoming, 4); break;
      case 5: result = combine(current, incoming, 5); break;
      default: result = incoming; break;
    }
    if (result.infinite) {
      clip = nullptr;
    } else {
      clip = result.clip ? result.clip : empty_clip();
    }
  }

  // ---- object parsing

  Result<Path> parse_path(util::ByteReader& r) {
    r.skip(4);  // version
    const std::uint32_t count = r.u32le();
    const std::uint16_t flags = r.u16le();
    r.skip(2);
    if (!r.ok()) return fail(Status::Decode, "truncated EMF+ path");
    const bool relative = (flags & 0x0800) != 0;
    const bool compressed = (flags & 0x4000) != 0;
    const bool rle = (flags & 0x1000) != 0;
    std::vector<Point> pts = read_points(r, count, compressed, relative);
    if (pts.size() != count) return fail(Status::Decode, "truncated EMF+ path points");
    std::vector<std::uint8_t> types;
    types.reserve(count);
    while (types.size() < count && r.ok()) {
      if (rle) {
        const std::uint8_t head = r.u8();
        const std::uint8_t value = r.u8();
        const std::uint32_t run = head & 0x3F;
        for (std::uint32_t i = 0; i < run && types.size() < count; ++i) types.push_back(value);
        if (run == 0) break;
      } else {
        types.push_back(r.u8());
      }
    }
    if (types.size() != count) return fail(Status::Decode, "truncated EMF+ path types");
    Path path;
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint8_t kind = types[i] & 0x07;
      if (kind == 0 || path.empty()) {
        path.move_to(pts[i]);
      } else if (kind == 3 && i + 2 < count) {
        path.cubic_to(pts[i], pts[i + 1], pts[i + 2]);
        i += 2;
      } else {
        path.line_to(pts[i]);
      }
      if ((types[std::min<std::size_t>(i, count - 1)] & 0x80) != 0) path.close();
    }
    return path;
  }

  Result<std::shared_ptr<const PlusImage>> parse_image(util::ByteReader& r) {
    r.skip(4);  // version
    const std::uint32_t type = r.u32le();
    auto image = std::make_shared<PlusImage>();
    const Context& ctx = gdi.ctx();
    if (type == 1) {
      const std::int32_t w = r.i32le(), h = r.i32le(), stride = r.i32le();
      const std::uint32_t format = r.u32le();
      const std::uint32_t kind = r.u32le();
      if (!r.ok() || w <= 0 || h <= 0) return fail(Status::Decode, "malformed EMF+ bitmap");
      image->width = w;
      image->height = h;
      CVT_TRY(ctx.limits.check_pixels(static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h)));
      if (gdi.pixels().used + std::uint64_t(w) * std::uint64_t(h) > ctx.limits.max_pixels) {
        return fail(Status::Limit, "bitmaps in the metafile exceed the limit of {} pixels", ctx.limits.max_pixels);
      }
      const ByteView rest = r.rest();
      if (kind == 1) {
        const auto id = image::sniff_format(rest);
        if (!id || !image::can_decode(*id, ctx)) {
          gdi.note("Some EMF+ pictures use a format that cannot be decoded and were left out.");
          return std::shared_ptr<const PlusImage>(image);
        }
        auto decoded = image::decode(*id, rest, ctx);
        if (!decoded || decoded->frames.empty()) {
          if (!decoded && (decoded.error().code == Status::Limit || decoded.error().code == Status::Cancelled)) {
            return std::unexpected(decoded.error());
          }
          gdi.note("Some EMF+ pictures could not be decoded and were left out.");
          return std::shared_ptr<const PlusImage>(image);
        }
        gdi.pixels().used += std::uint64_t{decoded->frames.front().width} * decoded->frames.front().height;
        image->frame = std::make_shared<const ir::Frame>(std::move(decoded->frames.front()));
        return std::shared_ptr<const PlusImage>(image);
      }
      // Raw pixels.
      const std::uint32_t bpp = (format >> 8) & 0xFF;
      const bool indexed = (format & 0x00010000u) != 0;
      std::vector<std::array<std::uint8_t, 4>> palette;
      util::ByteReader px(rest);
      if (indexed) {
        px.skip(4);  // palette flags
        const std::uint32_t n = std::min<std::uint32_t>(px.u32le(), 256);
        for (std::uint32_t i = 0; i < n; ++i) {
          const std::uint32_t c = px.u32le();
          palette.push_back({static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8),
                             static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(c >> 24)});
        }
      }
      if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        gdi.note("Some EMF+ bitmap pixel formats are not supported.");
        return std::shared_ptr<const PlusImage>(image);
      }
      const std::uint64_t row_bytes = (std::uint64_t(w) * bpp + 7) / 8;
      const std::uint64_t pitch = stride > 0 ? static_cast<std::uint64_t>(stride) : ((row_bytes + 3) & ~std::uint64_t{3});
      const ByteView bits = px.peek_rest();
      if (pitch < row_bytes || pitch * static_cast<std::uint64_t>(h - 1) + row_bytes > bits.size()) {
        gdi.note("Some EMF+ bitmaps are truncated and were left out.");
        return std::shared_ptr<const PlusImage>(image);
      }
      gdi.pixels().used += std::uint64_t(w) * std::uint64_t(h);
      auto frame = std::make_shared<ir::Frame>(ir::Frame::filled(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)));
      const bool alpha = (format & 0x00040000u) != 0;
      const bool premultiplied = (format & 0x00080000u) != 0;
      for (std::int32_t y = 0; y < h; ++y) {
        const std::uint64_t row = pitch * static_cast<std::uint64_t>(y);
        for (std::int32_t x = 0; x < w; ++x) {
          std::array<std::uint8_t, 4> c{0, 0, 0, 255};
          const auto byte = [&](std::uint64_t at) { return static_cast<std::uint8_t>(bits[static_cast<std::size_t>(at)]); };
          switch (bpp) {
            case 1: case 4: case 8: {
              const std::uint64_t bit = std::uint64_t(x) * bpp;
              const std::uint32_t v = (byte(row + bit / 8) >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
              if (v < palette.size()) c = palette[v];
              break;
            }
            case 16: {
              const std::uint32_t v = byte(row + std::uint64_t(x) * 2) | (byte(row + std::uint64_t(x) * 2 + 1) << 8);
              if ((format & 0xFF) == 0x06) {  // RGB565
                c = {static_cast<std::uint8_t>(((v >> 11) & 31) * 255 / 31), static_cast<std::uint8_t>(((v >> 5) & 63) * 255 / 63),
                     static_cast<std::uint8_t>((v & 31) * 255 / 31), 255};
              } else {
                c = {static_cast<std::uint8_t>(((v >> 10) & 31) * 255 / 31), static_cast<std::uint8_t>(((v >> 5) & 31) * 255 / 31),
                     static_cast<std::uint8_t>((v & 31) * 255 / 31),
                     static_cast<std::uint8_t>(alpha ? ((v & 0x8000) != 0 ? 255 : 0) : 255)};
              }
              break;
            }
            case 24: {
              const std::uint64_t at = row + std::uint64_t(x) * 3;
              c = {byte(at + 2), byte(at + 1), byte(at), 255};
              break;
            }
            default: {
              const std::uint64_t at = row + std::uint64_t(x) * 4;
              c = {byte(at + 2), byte(at + 1), byte(at), alpha ? byte(at + 3) : std::uint8_t{255}};
              if (premultiplied && c[3] != 0 && c[3] != 255) {
                for (int k = 0; k < 3; ++k) c[k] = static_cast<std::uint8_t>(std::min(255, c[k] * 255 / c[3]));
              }
              break;
            }
          }
          std::copy(c.begin(), c.end(), frame->rgba.begin() + static_cast<std::ptrdiff_t>(frame->offset(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))));
        }
      }
      image->frame = std::move(frame);
      return std::shared_ptr<const PlusImage>(image);
    }
    if (type == 2) {
      r.skip(4);  // metafile type
      const std::uint32_t size = r.u32le();
      const ByteView data = r.rest();
      if (size > data.size()) return fail(Status::Decode, "truncated EMF+ metafile image");
      if (depth + 1 > max_metafile_nesting) {
        gdi.note("Deeply nested metafile pictures were left out.");
        return std::shared_ptr<const PlusImage>(image);
      }
      auto nested = read_any_metafile(data.first(size), ctx, options, depth + 1);
      if (!nested) {
        if (nested.error().code == Status::Limit || nested.error().code == Status::Cancelled) {
          return std::unexpected(nested.error());
        }
        gdi.note("Some embedded metafiles could not be read and were left out.");
        return std::shared_ptr<const PlusImage>(image);
      }
      if (!nested->pages.empty()) {
        image->width = nested->pages.front().width;
        image->height = nested->pages.front().height;
        image->metafile = std::make_shared<const Scene>(std::move(*nested));
      }
      return std::shared_ptr<const PlusImage>(image);
    }
    return std::shared_ptr<const PlusImage>(image);
  }

  Result<PlusBrush> parse_brush(util::ByteReader& r) {
    r.skip(4);  // version
    const std::uint32_t type = r.u32le();
    PlusBrush b;
    switch (type) {
      case 0: b.type = PlusBrush::Type::Solid; b.color = argb(r.u32le()); break;
      case 1:
        b.type = PlusBrush::Type::Hatch;
        b.hatch = r.u32le();
        b.color = argb(r.u32le());
        b.back = argb(r.u32le());
        break;
      case 2: {
        b.type = PlusBrush::Type::Texture;
        const std::uint32_t flags = r.u32le();
        b.wrap = r.i32le();
        if ((flags & 0x02) != 0) b.transform = read_matrix(r);
        CVT_TRY_ASSIGN(b.image, parse_image(r));
        break;
      }
      case 3: {
        b.type = PlusBrush::Type::PathGradient;
        const std::uint32_t flags = r.u32le();
        b.wrap = r.i32le();
        const Color center = argb(r.u32le());
        b.center.x = r.f32le();
        b.center.y = r.f32le();
        const std::uint32_t n = r.u32le();
        Color surround = center;
        for (std::uint32_t i = 0; i < n && r.ok() && r.remaining() >= 4; ++i) {
          const Color c = argb(r.u32le());
          if (i == 0) surround = c;
        }
        Rect bounds;
        if ((flags & 0x01) != 0) {
          const std::uint32_t size = r.u32le();
          util::ByteReader sub(r.bytes(std::min<std::size_t>(size, r.remaining())));
          auto path = parse_path(sub);
          if (path) bounds = vector::bounds(*path);
        } else {
          const std::uint32_t count = r.u32le();
          const auto pts = read_points(r, count, false, false);
          for (const Point p : pts) bounds.add(p);
        }
        if ((flags & 0x02) != 0) b.transform = read_matrix(r);
        if (bounds.empty()) bounds = Rect{b.center.x - 1, b.center.y - 1, b.center.x + 1, b.center.y + 1};
        b.boundary = Rect{bounds.x0, bounds.y0, bounds.width(), bounds.height()};
        b.stops = {{0.0, center}, {1.0, surround}};
        b.color = center;
        break;
      }
      case 4: {
        b.type = PlusBrush::Type::Linear;
        const std::uint32_t flags = r.u32le();
        b.wrap = r.i32le();
        b.rect = read_rect(r, false);
        const Color start = argb(r.u32le()), end = argb(r.u32le());
        r.skip(8);
        if ((flags & 0x02) != 0) b.transform = read_matrix(r);
        b.stops = {{0.0, start}, {1.0, end}};
        b.color = start;
        if ((flags & 0x04) != 0) {  // preset colours
          const std::uint32_t n = r.u32le();
          if (n >= 2 && std::uint64_t{n} * 8 <= r.remaining()) {
            std::vector<double> pos(n);
            for (auto& p : pos) p = r.f32le();
            b.stops.clear();
            for (std::uint32_t i = 0; i < n; ++i) b.stops.push_back({std::clamp(pos[i], 0.0, 1.0), argb(r.u32le())});
          }
        } else if ((flags & 0x08) != 0 || (flags & 0x10) != 0) {  // blend factors
          const std::uint32_t n = r.u32le();
          if (n >= 2 && std::uint64_t{n} * 8 <= r.remaining()) {
            std::vector<double> pos(n), factor(n);
            for (auto& p : pos) p = r.f32le();
            for (auto& f : factor) f = r.f32le();
            b.stops.clear();
            for (std::uint32_t i = 0; i < n; ++i) {
              const double f = std::clamp(factor[i], 0.0, 1.0);
              const auto mix = [&](float a, float z) { return static_cast<float>(a + (z - a) * f); };
              b.stops.push_back({std::clamp(pos[i], 0.0, 1.0),
                                 Color{mix(start.r, end.r), mix(start.g, end.g), mix(start.b, end.b), mix(start.a, end.a)}});
            }
          }
        }
        std::sort(b.stops.begin(), b.stops.end(), [](const GradientStop& x, const GradientStop& y) { return x.offset < y.offset; });
        if (!std::isfinite(b.rect.x1) || !std::isfinite(b.rect.y1) || b.rect.x1 == 0) b.rect.x1 = 1;
        break;
      }
      default: return fail(Status::Decode, "unknown EMF+ brush type {}", type);
    }
    if (!r.ok()) return fail(Status::Decode, "truncated EMF+ brush");
    return b;
  }

  Result<PlusPen> parse_pen(util::ByteReader& r) {
    r.skip(8);  // version, type
    PlusPen pen;
    const std::uint32_t flags = r.u32le();
    pen.unit = r.u32le();
    pen.width = r.f32le();
    if (!std::isfinite(pen.width) || pen.width < 0) pen.width = 0;
    const auto cap = [](std::int32_t c) {
      switch (c) {
        case 1: case 0x11: return LineCap::Square;
        case 2: case 0x12: return LineCap::Round;
        default: return LineCap::Butt;
      }
    };
    if ((flags & 0x0001) != 0) pen.transform = read_matrix(r);
    if ((flags & 0x0002) != 0) pen.cap = cap(r.i32le());
    if ((flags & 0x0004) != 0) (void)r.i32le();
    if ((flags & 0x0008) != 0) {
      switch (r.u32le()) {
        case 1: pen.join = LineJoin::Bevel; break;
        case 2: pen.join = LineJoin::Round; break;
        case 3: pen.join = LineJoin::MiterClip; break;
        default: pen.join = LineJoin::Miter; break;
      }
    }
    if ((flags & 0x0010) != 0) pen.miter_limit = r.f32le();
    if ((flags & 0x0020) != 0) pen.style = r.i32le();
    if ((flags & 0x0040) != 0) (void)r.i32le();
    if ((flags & 0x0080) != 0) pen.dash_offset = r.f32le();
    if ((flags & 0x0100) != 0) {
      const std::uint32_t n = r.u32le();
      if (std::uint64_t{n} * 4 > r.remaining()) return fail(Status::Decode, "truncated EMF+ pen");
      for (std::uint32_t i = 0; i < n; ++i) {
        const double d = r.f32le();
        if (pen.dash.size() < 64) pen.dash.push_back(std::isfinite(d) ? d : 0.0);
      }
      pen.style = 5;
    }
    if ((flags & 0x0200) != 0) (void)r.i32le();
    if ((flags & 0x0400) != 0) {
      const std::uint32_t n = r.u32le();
      if (std::uint64_t{n} * 4 > r.remaining()) return fail(Status::Decode, "truncated EMF+ pen");
      r.skip(std::size_t{n} * 4);
    }
    if ((flags & 0x0800) != 0) r.skip(std::min<std::size_t>(r.u32le(), r.remaining()));
    if ((flags & 0x1000) != 0) r.skip(std::min<std::size_t>(r.u32le(), r.remaining()));
    if (!r.ok()) return fail(Status::Decode, "truncated EMF+ pen");
    CVT_TRY_ASSIGN(pen.brush, parse_brush(r));
    return pen;
  }

  Result<std::size_t> parse_region_node(util::ByteReader& r, PlusRegion& region, int depth_left) {
    if (depth_left <= 0 || region.nodes.size() >= max_region_nodes) {
      return fail(Status::Limit, "EMF+ region is too complex");
    }
    const std::size_t index = region.nodes.size();
    region.nodes.emplace_back();
    const std::uint32_t type = r.u32le();
    if (!r.ok()) return fail(Status::Decode, "truncated EMF+ region");
    region.nodes[index].type = type;
    switch (type) {
      case 0x10000000u: region.nodes[index].rect = read_rect(r, false); break;
      case 0x10000001u: {
        const std::uint32_t size = r.u32le();
        if (size > r.remaining()) return fail(Status::Decode, "truncated EMF+ region path");
        util::ByteReader sub(r.bytes(size));
        CVT_TRY_ASSIGN(Path p, parse_path(sub));
        region.nodes[index].path = std::move(p);
        break;
      }
      case 0x10000002u: case 0x10000003u: break;
      case 1: case 2: case 3: case 4: case 5: {
        CVT_TRY_ASSIGN(const std::size_t left, parse_region_node(r, region, depth_left - 1));
        CVT_TRY_ASSIGN(const std::size_t right, parse_region_node(r, region, depth_left - 1));
        region.nodes[index].left = left;
        region.nodes[index].right = right;
        break;
      }
      default: return fail(Status::Decode, "unknown EMF+ region node {}", type);
    }
    if (!r.ok()) return fail(Status::Decode, "truncated EMF+ region");
    return index;
  }

  Result<void> define_object(std::uint16_t flags, ByteView data) {
    const std::uint8_t id = flags & 0xFF;
    const std::uint32_t type = (flags >> 8) & 0x7F;
    if (id >= objects.size()) return {};
    util::ByteReader r(data);
    Result<PlusObject> parsed = PlusObject{};
    switch (type) {
      case 1: {
        auto b = parse_brush(r);
        if (b) parsed = PlusObject{std::move(*b)}; else parsed = std::unexpected(b.error());
        break;
      }
      case 2: {
        auto p = parse_pen(r);
        if (p) parsed = PlusObject{std::move(*p)}; else parsed = std::unexpected(p.error());
        break;
      }
      case 3: {
        auto p = parse_path(r);
        if (p) parsed = PlusObject{PlusPath{std::move(*p)}}; else parsed = std::unexpected(p.error());
        break;
      }
      case 4: {
        PlusRegion region;
        r.skip(8);  // version, node count
        auto root = parse_region_node(r, region, max_region_depth);
        if (root) parsed = PlusObject{std::move(region)}; else parsed = std::unexpected(root.error());
        break;
      }
      case 5: {
        auto img = parse_image(r);
        if (img) parsed = PlusObject{std::move(*img)}; else parsed = std::unexpected(img.error());
        break;
      }
      case 6: {
        PlusFont f;
        r.skip(4);
        f.em = r.f32le();
        f.unit = r.u32le();
        f.style = r.i32le();
        r.skip(4);
        const std::uint32_t len = r.u32le();
        std::u16string name;
        for (std::uint32_t i = 0; i < len && r.remaining() >= 2 && i < 256; ++i) name.push_back(static_cast<char16_t>(r.u16le()));
        f.family = util::utf16_to_utf8(name);
        if (!r.ok() || !std::isfinite(f.em) || f.em <= 0) {
          parsed = fail(Status::Decode, "malformed EMF+ font");
        } else {
          parsed = PlusObject{std::move(f)};
        }
        break;
      }
      case 7: {
        PlusFormat fmt;
        r.skip(4);
        fmt.flags = r.u32le();
        r.skip(4);
        fmt.align = r.u32le();
        fmt.line_align = r.u32le();
        parsed = PlusObject{fmt};
        break;
      }
      default: parsed = PlusObject{}; break;  // image attributes, custom line caps
    }
    if (!parsed) {
      if (parsed.error().code == Status::Limit || parsed.error().code == Status::Cancelled) {
        return std::unexpected(parsed.error());
      }
      gdi.note("Some malformed EMF+ objects were ignored.");
      objects[id] = PlusObject{};
      return {};
    }
    objects[id] = std::move(*parsed);
    return {};
  }

  Result<void> object_record(std::uint16_t flags, ByteView data) {
    const bool continued = (flags & 0x8000) != 0;
    const int id = flags & 0xFF;
    if (continued) {
      util::ByteReader r(data);
      const std::uint32_t total_size = r.u32le();
      if (!r.ok()) return {};
      if (partial_id != id) {
        partial.clear();
        partial_id = id;
        partial_total = total_size;
        partial_flags = flags & 0x7FFF;
        if (partial_total > max_object_bytes || partial_total > gdi.ctx().limits.max_decompressed_bytes) {
          return fail(Status::Limit, "an EMF+ object is too large");
        }
      }
      const ByteView rest = r.rest();
      partial.insert(partial.end(), rest.begin(), rest.end());
      if (partial.size() >= partial_total) {
        Bytes whole = std::move(partial);
        partial.clear();
        partial_id = -1;
        return define_object(partial_flags, whole);
      }
      return {};
    }
    if (partial_id == id) {
      partial.insert(partial.end(), data.begin(), data.end());
      Bytes whole = std::move(partial);
      partial.clear();
      partial_id = -1;
      return define_object(flags, whole);
    }
    return define_object(flags, data);
  }

  // ---- drawing records

  Result<void> draw_image(std::uint8_t id, const Rect& src, std::uint32_t src_unit, Point p0, Point p1, Point p2) {
    const auto* holder = std::get_if<std::shared_ptr<const PlusImage>>(&objects[id]);
    if (holder == nullptr || !*holder) return {};
    const PlusImage& img = **holder;
    const Matrix m = total();
    const Point u = p1 - p0, v = p2 - p0;
    if (img.frame) {
      double sx = 1.0, sy = 1.0;
      if (src_unit != 2 && src_unit != 0 && src_unit != 1) {
        sx = unit_factor(src_unit, false);
        sy = unit_factor(src_unit, true);
      }
      FramePtr frame = crop_frame(img.frame, static_cast<std::int64_t>(std::floor(src.x0 * sx)),
                                  static_cast<std::int64_t>(std::floor(src.y0 * sy)),
                                  static_cast<std::int64_t>(std::ceil(src.x1 * sx)),
                                  static_cast<std::int64_t>(std::ceil(src.y1 * sy)));
      if (!frame) return {};
      const double w = frame->width, h = frame->height;
      Group g;
      g.transform = m * Matrix{u.x / w, u.y / w, v.x / h, v.y / h, p0.x, p0.y};
      if (!finite_matrix(g.transform) || !g.transform.invertible()) return {};
      Picture pic;
      pic.pixels = std::move(frame);
      pic.bounds = Rect::xywh(0, 0, w, h);
      g.children.push_back(Node{std::move(pic)});
      return add_node(Node{std::move(g)});
    }
    if (img.metafile && !img.metafile->pages.empty()) {
      const Page& page = img.metafile->pages.front();
      if (!(page.width > 0) || !(page.height > 0)) return {};
      Group g;
      g.transform = m * Matrix{u.x / page.width, u.y / page.width, v.x / page.height, v.y / page.height, p0.x, p0.y};
      if (!finite_matrix(g.transform) || !g.transform.invertible()) return {};
      Path frame;
      add_rect(frame, 0, 0, page.width, page.height);
      g.clip = make_clip(std::move(frame), FillRule::NonZero);
      g.children.push_back(Node{page.root});
      return add_node(Node{std::move(g)});
    }
    return {};
  }

  /// Lay out and draw a string inside a layout rectangle (world units).
  Result<void> draw_string(const PlusFont& font, const PlusFormat* format, std::u16string_view u16, Rect layout,
                           const Paint& paint) {
    const std::string utf8 = util::utf16_to_utf8(std::u16string(u16));
    FontSpec spec;
    spec.family = font.family.empty() ? std::string("sans-serif") : font.family;
    spec.weight = (font.style & 1) != 0 ? 700 : 400;
    spec.italic = (font.style & 2) != 0;
    double em = font.em;
    if (font.unit != 0) em = font.em * unit_factor(font.unit, true) / (unit_factor(page_unit, true) * page_scale);
    if (!std::isfinite(em) || em <= 0 || em > 1e7) return {};
    spec.size = em;
    CVT_TRY(gdi.out().charge_points(utf8.size() * 16));

    // Lines: explicit breaks, then greedy word wrap inside the rectangle width.
    std::vector<std::string> lines;
    // Word wrapping measures every candidate line; long strings are not wrapped.
    const bool no_wrap = (format != nullptr && (format->flags & 0x1000) != 0) || utf8.size() > 4096;
    for (const auto piece : util::split_lines(utf8)) {
      if (no_wrap || !(layout.x1 > 0)) {
        lines.emplace_back(piece);
        continue;
      }
      std::string line;
      for (const auto word : util::split(piece, ' ')) {
        const std::string candidate = line.empty() ? std::string(word) : line + " " + std::string(word);
        if (!line.empty() && layout_text(candidate, spec).advance > layout.x1 * 1.001) {
          lines.push_back(line);
          line = std::string(word);
        } else {
          line = candidate;
        }
      }
      lines.push_back(line);
    }
    if (lines.empty()) return {};
    const TextLayout probe = layout_text("", spec);
    const double ascent = probe.ascent, descent = probe.descent;
    const double line_h = (ascent + descent) * 1.0;
    const double block = line_h * static_cast<double>(lines.size());
    const std::uint32_t align = format != nullptr ? format->align : 0;
    const std::uint32_t valign = format != nullptr ? format->line_align : 0;
    double top = layout.y0;
    if (valign == 1) top = layout.y1 > 0 ? layout.y0 + (layout.y1 - block) / 2 : layout.y0 - block / 2;
    if (valign == 2) top = layout.y1 > 0 ? layout.y0 + layout.y1 - block : layout.y0 - block;

    Group g;
    g.transform = total();
    if (!finite_matrix(g.transform)) return {};
    for (std::size_t i = 0; i < lines.size(); ++i) {
      const TextLayout lay = layout_text(lines[i], spec);
      double x = layout.x0;
      if (align == 1) x = layout.x1 > 0 ? layout.x0 + (layout.x1 - lay.advance) / 2 : layout.x0 - lay.advance / 2;
      if (align == 2) x = layout.x1 > 0 ? layout.x0 + layout.x1 - lay.advance : layout.x0 - lay.advance;
      const double baseline = top + ascent + line_h * static_cast<double>(i);
      Text t;
      t.content = lines[i];
      t.font = spec;
      t.origin = {x, baseline};
      t.advance = lay.advance;
      t.outline = transformed(lay.outline, Matrix::translate(x, baseline));
      t.fill = paint;
      g.children.push_back(Node{std::move(t)});
      const auto deco = [&](double dy) {
        Shape s;
        add_rect(s.path, x, baseline + dy, lay.advance, em * 0.06);
        s.fill = paint;
        g.children.push_back(Node{std::move(s)});
      };
      if ((font.style & 4) != 0) deco(em * 0.1);
      if ((font.style & 8) != 0) deco(-ascent * 0.32);
    }
    return add_node(Node{std::move(g)});
  }

  Result<void> record(std::uint16_t type, std::uint16_t flags, ByteView data) {
    util::ByteReader r(data);
    const bool compressed = (flags & 0x4000) != 0;
    const bool relative = (flags & 0x0800) != 0;
    const std::uint8_t low = flags & 0xFF;
    switch (type) {
      case 0x4001: {  // Header
        r.skip(8);
        const std::uint32_t dx = r.u32le(), dy = r.u32le();
        if (r.ok() && dx > 0 && dx < 100000) dpi_x = dx;
        if (r.ok() && dy > 0 && dy < 100000) dpi_y = dy;
        active = true;
        return gdi.set_drawing_enabled(false);
      }
      case 0x4002: return {};               // EndOfFile
      case 0x4003: return {};               // Comment
      case 0x4004: return gdi.set_drawing_enabled(true);  // GetDC: the following GDI records draw
      case 0x4008: return object_record(flags, data);
      case 0x4009: {  // Clear
        const Color c = argb(r.u32le());
        CVT_TRY(gdi.flush());
        Shape s;
        s.path = huge_rect_path();
        s.fill = Paint::solid(c);
        return gdi.out().add_shape(std::move(s), clip);
      }
      case 0x400A: case 0x400B: {  // FillRects, DrawRects
        const std::uint32_t brush = type == 0x400A ? r.u32le() : 0;
        const std::uint32_t count = r.u32le();
        if (std::uint64_t{count} * (compressed ? 8 : 16) > r.remaining()) return {};
        Path p;
        for (std::uint32_t i = 0; i < count; ++i) append(p, rect_path(read_rect(r, compressed)));
        if (type == 0x400A) return draw(p, brush_of(flags, brush, total()), FillRule::NonZero, std::nullopt);
        return draw(p, std::nullopt, FillRule::NonZero, pen_of(low, total()));
      }
      case 0x400C: {  // FillPolygon
        const std::uint32_t brush = r.u32le();
        const std::uint32_t count = r.u32le();
        const auto pts = read_points(r, count, compressed, relative);
        if (pts.size() < 2) return {};
        Path p;
        p.move_to(pts[0]);
        for (std::size_t i = 1; i < pts.size(); ++i) p.line_to(pts[i]);
        p.close();
        return draw(p, brush_of(flags, brush, total()), FillRule::EvenOdd, std::nullopt);
      }
      case 0x400D: {  // DrawLines
        const std::uint32_t count = r.u32le();
        const auto pts = read_points(r, count, compressed, relative);
        if (pts.size() < 2) return {};
        Path p;
        p.move_to(pts[0]);
        for (std::size_t i = 1; i < pts.size(); ++i) p.line_to(pts[i]);
        if ((flags & 0x2000) != 0) p.close();
        return draw(p, std::nullopt, FillRule::NonZero, pen_of(low, total()));
      }
      case 0x400E: case 0x400F: {  // FillEllipse, DrawEllipse
        const std::uint32_t brush = type == 0x400E ? r.u32le() : 0;
        const Rect e = read_rect(r, compressed);
        if (!r.ok()) return {};
        Path p;
        add_ellipse(p, e.x0 + e.x1 / 2, e.y0 + e.y1 / 2, std::abs(e.x1) / 2, std::abs(e.y1) / 2);
        if (type == 0x400E) return draw(p, brush_of(flags, brush, total()), FillRule::NonZero, std::nullopt);
        return draw(p, std::nullopt, FillRule::NonZero, pen_of(low, total()));
      }
      case 0x4010: case 0x4011: case 0x4012: {  // FillPie, DrawPie, DrawArc
        const std::uint32_t brush = type == 0x4010 ? r.u32le() : 0;
        const double start = r.f32le(), sweep = r.f32le();
        const Rect e = read_rect(r, compressed);
        if (!r.ok() || !std::isfinite(start) || !std::isfinite(sweep)) return {};
        const double sw = std::clamp(sweep, -360.0, 360.0);
        const Point c{e.x0 + e.x1 / 2, e.y0 + e.y1 / 2};
        const double rx = std::abs(e.x1) / 2, ry = std::abs(e.y1) / 2;
        if (!(rx > 0) || !(ry > 0)) return {};
        const double a0 = std::fmod(start, 360.0) * pi / 180, a1 = (std::fmod(start, 360.0) + sw) * pi / 180;
        // GDI+ angles are measured on the ellipse as seen, clockwise (y down).
        const auto param = [&](double a) { return std::atan2(std::sin(a) / ry, std::cos(a) / rx); };
        double p0 = param(a0), p1 = param(a1);
        if (std::abs(sw) >= 360.0) p1 = p0 + (sw > 0 ? 2 * pi : -2 * pi);
        Path p;
        if (type == 0x4012) {
          add_arc(p, c, rx, ry, 0, p0, p1, sw > 0, false);
          return draw(p, std::nullopt, FillRule::NonZero, pen_of(low, total()));
        }
        p.move_to(c);
        add_arc(p, c, rx, ry, 0, p0, p1, sw > 0, true);
        p.close();
        if (type == 0x4010) return draw(p, brush_of(flags, brush, total()), FillRule::NonZero, std::nullopt);
        return draw(p, std::nullopt, FillRule::NonZero, pen_of(low, total()));
      }
      case 0x4013: {  // FillRegion
        const std::uint32_t brush = r.u32le();
        const auto* region = std::get_if<PlusRegion>(&objects[low]);
        if (region == nullptr) return {};
        const Region rg = eval_region(*region, 0, total(), max_region_depth);
        if (!rg.infinite && !rg.clip) return {};
        const ClipRef saved = clip;
        if (!rg.infinite) {
          bool approx = false;
          clip = combine_clip(clip, rg.clip, 1, approx);
        }
        CVT_TRY(gdi.flush());
        Shape s;
        s.path = huge_rect_path();
        const auto paint = brush_of(flags, brush, total());
        if (paint) s.fill = *paint;
        auto res = s.fill.visible() ? gdi.out().add_shape(std::move(s), clip) : Result<void>{};
        clip = saved;
        return res;
      }
      case 0x4014: {  // FillPath
        const std::uint32_t brush = r.u32le();
        const auto* path = std::get_if<PlusPath>(&objects[low]);
        if (path == nullptr) return {};
        return draw(path->path, brush_of(flags, brush, total()), FillRule::EvenOdd, std::nullopt);
      }
      case 0x4015: {  // DrawPath
        const std::uint32_t pen = r.u32le();
        const auto* path = std::get_if<PlusPath>(&objects[low]);
        if (path == nullptr) return {};
        return draw(path->path, std::nullopt, FillRule::NonZero, pen_of(pen, total()));
      }
      case 0x4016: {  // FillClosedCurve
        const std::uint32_t brush = r.u32le();
        const double tension = r.f32le();
        const std::uint32_t count = r.u32le();
        const auto pts = read_points(r, count, compressed, relative);
        if (pts.size() < 3 || !std::isfinite(tension)) return {};
        return draw(cardinal(pts, tension, true), brush_of(flags, brush, total()),
                    (flags & 0x2000) != 0 ? FillRule::NonZero : FillRule::EvenOdd, std::nullopt);
      }
      case 0x4017: {  // DrawClosedCurve
        const double tension = r.f32le();
        const std::uint32_t count = r.u32le();
        const auto pts = read_points(r, count, compressed, relative);
        if (pts.size() < 3 || !std::isfinite(tension)) return {};
        return draw(cardinal(pts, tension, true), std::nullopt, FillRule::NonZero, pen_of(low, total()));
      }
      case 0x4018: {  // DrawCurve
        const double tension = r.f32le();
        const std::uint32_t offset = r.u32le(), segments = r.u32le(), count = r.u32le();
        const auto pts = read_points(r, count, compressed, false);
        if (pts.size() < 2 || !std::isfinite(tension)) return {};
        Path full = cardinal(pts, tension, false);
        // Keep segments [offset, offset + segments).
        Path p;
        std::size_t seg = 0;
        for (std::size_t i = 0, pt = 0; i < full.verbs.size(); ++i) {
          if (full.verbs[i] == Verb::Move) {
            ++pt;
            continue;
          }
          if (seg >= offset && seg < std::uint64_t{offset} + segments) {
            if (p.empty()) p.move_to(full.points[pt - 1]);
            p.cubic_to(full.points[pt], full.points[pt + 1], full.points[pt + 2]);
          }
          pt += 3;
          ++seg;
        }
        return draw(p, std::nullopt, FillRule::NonZero, pen_of(low, total()));
      }
      case 0x4019: {  // DrawBeziers
        const std::uint32_t count = r.u32le();
        const auto pts = read_points(r, count, compressed, relative);
        if (pts.size() < 4) return {};
        Path p;
        p.move_to(pts[0]);
        for (std::size_t i = 1; i + 2 < pts.size(); i += 3) p.cubic_to(pts[i], pts[i + 1], pts[i + 2]);
        return draw(p, std::nullopt, FillRule::NonZero, pen_of(low, total()));
      }
      case 0x401A: {  // DrawImage
        r.skip(4);
        const std::uint32_t unit = r.u32le();
        const Rect src = read_rect(r, false);
        const Rect dst = read_rect(r, compressed);
        if (!r.ok()) return {};
        return draw_image(low, src, unit, {dst.x0, dst.y0}, {dst.x0 + dst.x1, dst.y0}, {dst.x0, dst.y0 + dst.y1});
      }
      case 0x401B: {  // DrawImagePoints
        r.skip(4);
        const std::uint32_t unit = r.u32le();
        const Rect src = read_rect(r, false);
        const std::uint32_t count = r.u32le();
        const auto pts = read_points(r, count, compressed, relative);
        if (pts.size() != 3) return {};
        return draw_image(low, src, unit, pts[0], pts[1], pts[2]);
      }
      case 0x401C: {  // DrawString
        const std::uint32_t brush = r.u32le();
        const std::uint32_t format_id = r.u32le();
        const std::uint32_t length = r.u32le();
        const Rect layout = read_rect(r, false);
        if (!r.ok() || std::uint64_t{length} * 2 > r.remaining()) return {};
        std::u16string text;
        for (std::uint32_t i = 0; i < length; ++i) text.push_back(static_cast<char16_t>(r.u16le()));
        const auto* font = std::get_if<PlusFont>(&objects[low]);
        if (font == nullptr) return {};
        const PlusFormat* format = format_id < objects.size() ? std::get_if<PlusFormat>(&objects[format_id]) : nullptr;
        const auto paint = brush_of(flags, brush, Matrix{});
        if (!paint) return {};
        return draw_string(*font, format, text, layout, *paint);
      }
      case 0x4036: {  // DrawDriverString
        const std::uint32_t brush = r.u32le();
        const std::uint32_t opts = r.u32le();
        const std::uint32_t has_matrix = r.u32le();
        const std::uint32_t count = r.u32le();
        if (!r.ok() || std::uint64_t{count} * 10 > r.remaining()) return {};
        std::u32string chars;
        for (std::uint32_t i = 0; i < count; ++i) chars.push_back(r.u16le());
        const auto pos = read_points(r, count, false, false);
        Matrix extra;
        if (has_matrix != 0) extra = read_matrix(r);
        const auto* font = std::get_if<PlusFont>(&objects[low]);
        if (font == nullptr || pos.size() != count || count == 0) return {};
        if ((opts & 1) == 0) {
          gdi.note("EMF+ text stored as glyph indices cannot be shown; it was left out.");
          return {};
        }
        const auto paint = brush_of(flags, brush, Matrix{});
        if (!paint) return {};
        FontSpec spec;
        spec.family = font->family.empty() ? std::string("sans-serif") : font->family;
        spec.weight = (font->style & 1) != 0 ? 700 : 400;
        spec.italic = (font->style & 2) != 0;
        double em = font->em;
        if (font->unit != 0) em = font->em * unit_factor(font->unit, true) / (unit_factor(page_unit, true) * page_scale);
        if (!std::isfinite(em) || em <= 0 || em > 1e7) return {};
        spec.size = em;
        CVT_TRY(gdi.out().charge_points(chars.size() * 16));
        Text t;
        t.font = spec;
        t.content = util::utf32_to_utf8(chars);
        t.origin = pos[0];
        t.fill = *paint;
        double pen_x = pos[0].x;
        for (std::size_t i = 0; i < chars.size(); ++i) {
          char32_t c = chars[i];
          if (!util::is_scalar_value(c)) c = U'�';
          const TextLayout glyph = layout_text(util::utf32_to_utf8(std::u32string_view(&c, 1)), spec);
          const Point at = (opts & 4) != 0 ? Point{pen_x, pos[0].y} : pos[i];
          append(t.outline, glyph.outline, Matrix::translate(at.x, at.y));
          pen_x = at.x + glyph.advance;
        }
        t.advance = pen_x - pos[0].x;
        Group g;
        g.transform = total() * extra;
        if (!finite_matrix(g.transform)) return {};
        g.children.push_back(Node{std::move(t)});
        return add_node(Node{std::move(g)});
      }
      case 0x4025: case 0x4028: {  // Save, BeginContainerNoParams
        if (stack.size() >= 4096) return {};
        stack.push_back({r.u32le(), world, page_unit, page_scale, clip});
        return {};
      }
      case 0x4027: {  // BeginContainer
        const Rect dst = read_rect(r, false), src = read_rect(r, false);
        const std::uint32_t id = r.u32le();
        if (stack.size() < 4096) stack.push_back({id, world, page_unit, page_scale, clip});
        if (r.ok() && src.x1 != 0 && src.y1 != 0) {
          const double f = unit_factor(low, false) / unit_factor(page_unit, false) / page_scale;
          const Matrix map = Matrix::translate(dst.x0 * f, dst.y0 * f) *
                             Matrix::scale(dst.x1 * f / src.x1, dst.y1 * f / src.y1) *
                             Matrix::translate(-src.x0, -src.y0);
          if (finite_matrix(map)) world = world * map;
        }
        return {};
      }
      case 0x4026: case 0x4029: {  // Restore, EndContainer
        const std::uint32_t id = r.u32le();
        for (std::size_t i = stack.size(); i-- > 0;) {
          if (stack[i].id == id) {
            world = stack[i].world;
            page_unit = stack[i].unit;
            page_scale = stack[i].scale;
            clip = stack[i].clip;
            stack.resize(i);
            break;
          }
        }
        return {};
      }
      case 0x402A: world = read_matrix(r); return {};
      case 0x402B: world = Matrix{}; return {};
      case 0x402C: case 0x402D: case 0x402E: case 0x402F: {
        Matrix m;
        if (type == 0x402C) m = read_matrix(r);
        if (type == 0x402D) {
          const double dx = r.f32le(), dy = r.f32le();
          m = Matrix::translate(dx, dy);
        }
        if (type == 0x402E) {
          const double sx = r.f32le(), sy = r.f32le();
          m = Matrix::scale(sx, sy);
        }
        if (type == 0x402F) m = Matrix::rotate(r.f32le() * pi / 180.0);
        if (!r.ok() || !finite_matrix(m)) return {};
        world = (flags & 0x2000) != 0 ? m * world : world * m;
        return {};
      }
      case 0x4030: {  // SetPageTransform
        const double scale = r.f32le();
        page_unit = low;
        page_scale = std::isfinite(scale) && scale > 0 ? scale : 1.0;
        return {};
      }
      case 0x4031: clip = nullptr; return {};
      case 0x4032: {  // SetClipRect
        const Rect rc = read_rect(r, false);
        if (!r.ok()) return {};
        set_clip(Region{false, make_clip(transformed(rect_path(rc), total()), FillRule::NonZero)}, (flags >> 8) & 0x0F);
        return {};
      }
      case 0x4033: {  // SetClipPath
        const auto* path = std::get_if<PlusPath>(&objects[low]);
        if (path == nullptr) return {};
        set_clip(Region{false, make_clip(transformed(path->path, total()), FillRule::EvenOdd)}, (flags >> 8) & 0x0F);
        return {};
      }
      case 0x4034: {  // SetClipRegion
        const auto* region = std::get_if<PlusRegion>(&objects[low]);
        if (region == nullptr) return {};
        set_clip(eval_region(*region, 0, total(), max_region_depth), (flags >> 8) & 0x0F);
        return {};
      }
      case 0x4035: {  // OffsetClip
        const double dx = r.f32le(), dy = r.f32le();
        if (!r.ok()) return {};
        const Point d = total().apply_vector({dx, dy});
        clip = transform_clip(clip, Matrix::translate(d.x, d.y));
        return {};
      }
      case 0x4037:
        gdi.note("EMF+ StrokeFillPath records were ignored.");
        return {};
      default: return {};  // rendering hints, compositing, terminal-server records
    }
  }

  Result<void> play(ByteView records) {
    util::ByteReader r(records);
    while (r.remaining() >= 12) {
      const std::uint16_t type = r.u16le();
      const std::uint16_t flags = r.u16le();
      const std::uint32_t size = r.u32le();
      const std::uint32_t data_size = r.u32le();
      if (size < 12 || size - 12 > r.remaining() || data_size > size - 12) {
        gdi.note("Some EMF+ records are malformed and were skipped.");
        return {};
      }
      const ByteView data = r.bytes(size - 12).first(data_size);
      if (active && type != 0x4004 && type != 0x4001) CVT_TRY(gdi.set_drawing_enabled(false));
      if (!active && type != 0x4001) continue;  // records before the header
      CVT_TRY(record(type, flags, data));
    }
    return {};
  }
};

EmfPlusPlayer::EmfPlusPlayer(GdiPlayer& gdi, const ReadOptions& options, int depth)
    : impl_(std::make_unique<Impl>(gdi, options, depth)) {}

EmfPlusPlayer::~EmfPlusPlayer() = default;

Result<void> EmfPlusPlayer::play(ByteView records) { return impl_->play(records); }

bool EmfPlusPlayer::active() const noexcept { return impl_->active; }

}  // namespace cvt::vector::metafile
