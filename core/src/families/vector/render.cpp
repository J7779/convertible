// Convertible core - vector family: the anti-aliased scanline rasteriser.
//
// Coverage: every edge adds its exact signed area to an accumulation buffer
// (cell area + cover, integrated along the scanline), which gives the
// integral of the winding number over each pixel; the fill rule is applied
// to that value (non-zero: min(1, |w|), even-odd: distance to the nearest
// even number). Paint is evaluated at pixel centres; colours are composited
// as premultiplied floats in sRGB space.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/vector/render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "families/image/image_api.hpp"
#include "families/image/image_ops.hpp"
#include "families/vector/geom.hpp"

namespace cvt::vector {

namespace {

constexpr double device_tolerance = 0.03;  // flattening tolerance in pixels
constexpr int max_layer_depth = 48;
constexpr int max_pattern_depth = 4;

using Px = std::array<float, 4>;  // premultiplied RGBA

// ------------------------------------------------------------------ canvas

struct Canvas {
  int w = 0, h = 0;
  std::vector<float> px;  // premultiplied RGBA
  // Region that may be non-transparent.
  int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;

  Canvas() = default;
  Canvas(int width, int height) : w(width), h(height), px(static_cast<std::size_t>(width) * height * 4, 0.0f) {}
  float* at(int x, int y) { return &px[(static_cast<std::size_t>(y) * w + x) * 4]; }
  const float* at(int x, int y) const { return &px[(static_cast<std::size_t>(y) * w + x) * 4]; }
  void touch(int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    if (dx1 <= dx0 || dy1 <= dy0) {
      dx0 = x0, dy0 = y0, dx1 = x1, dy1 = y1;
      return;
    }
    dx0 = std::min(dx0, x0), dy0 = std::min(dy0, y0), dx1 = std::max(dx1, x1), dy1 = std::max(dy1, y1);
  }
  void touch_all() { touch(0, 0, w, h); }
  [[nodiscard]] bool blank() const { return dx1 <= dx0 || dy1 <= dy0; }
};

// ------------------------------------------------------------------ coverage

struct Coverage {
  int x0 = 0, y0 = 0, w = 0, h = 0;  // bbox in canvas pixels
  std::vector<float> cov;
  [[nodiscard]] float at(int x, int y) const {
    if (x < x0 || y < y0 || x >= x0 + w || y >= y0 + h) return 0.0f;
    return cov[static_cast<std::size_t>(y - y0) * w + (x - x0)];
  }
  [[nodiscard]] bool empty() const { return w <= 0 || h <= 0; }
};

struct Accumulator {
  int w = 0, h = 0;  // buffer has w + 2 columns
  std::vector<float> acc;

  void line(Point p0, Point p1) {
    if (p0.y == p1.y) return;
    float dir = 1.0f;
    if (p0.y > p1.y) {
      std::swap(p0, p1);
      dir = -1.0f;
    }
    if (p1.y <= 0 || p0.y >= h) return;
    const double dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    double x = p0.x;
    double ystart = p0.y;
    if (ystart < 0) {
      x -= ystart * dxdy;
      ystart = 0;
    }
    const double yend = std::min<double>(p1.y, h);
    const int ys = static_cast<int>(std::floor(ystart));
    const int ye = static_cast<int>(std::ceil(yend));
    const std::size_t stride = static_cast<std::size_t>(w) + 2;
    for (int y = ys; y < ye; ++y) {
      const double dy = std::min<double>(y + 1, yend) - std::max<double>(y, ystart);
      if (dy <= 0) continue;
      const double xnext = x + dxdy * dy;
      const float d = static_cast<float>(dy) * dir;
      float* row = &acc[static_cast<std::size_t>(y) * stride];
      double xa = std::clamp(x, 0.0, static_cast<double>(w));
      double xb = std::clamp(xnext, 0.0, static_cast<double>(w));
      if (xa > xb) std::swap(xa, xb);
      const double xa_floor = std::floor(xa);
      const int xai = static_cast<int>(xa_floor);
      const double xb_ceil = std::ceil(xb);
      const int xbi = static_cast<int>(xb_ceil);
      if (xbi <= xai + 1) {
        const double xmf = 0.5 * (xa + xb) - xa_floor;
        row[xai] += d - static_cast<float>(d * xmf);
        row[xai + 1] += static_cast<float>(d * xmf);
      } else {
        const double s = 1.0 / (xb - xa);
        const double xaf = xa - xa_floor;
        const double a0 = 0.5 * s * (1 - xaf) * (1 - xaf);
        const double xbf = xb - xb_ceil + 1;
        const double am = 0.5 * s * xbf * xbf;
        row[xai] += static_cast<float>(d * a0);
        if (xbi == xai + 2) {
          row[xai + 1] += static_cast<float>(d * (1 - a0 - am));
        } else {
          const double a1 = s * (1.5 - xaf);
          row[xai + 1] += static_cast<float>(d * (a1 - a0));
          for (int xi = xai + 2; xi < xbi - 1; ++xi) row[xi] += static_cast<float>(d * s);
          const double a2 = a1 + (xbi - xai - 3) * s;
          row[xbi - 1] += static_cast<float>(d * (1 - a2 - am));
        }
        row[xbi] += static_cast<float>(d * am);
      }
      x = xnext;
    }
  }
};

// Coverage of a path already in device space, clipped to the canvas.
Coverage rasterize(const Path& device, FillRule rule, int cw, int ch) {
  Coverage out;
  const std::vector<Polyline> lines = flatten(device, device_tolerance);
  Rect box;
  for (const Polyline& l : lines) {
    for (const Point& p : l.points) {
      if (std::isfinite(p.x) && std::isfinite(p.y)) box.add(p);
    }
  }
  if (box.empty()) return out;
  const int x0 = std::max(0, static_cast<int>(std::floor(box.x0)));
  const int y0 = std::max(0, static_cast<int>(std::floor(box.y0)));
  const int x1 = std::min(cw, static_cast<int>(std::ceil(box.x1)) + 1);
  const int y1 = std::min(ch, static_cast<int>(std::ceil(box.y1)) + 1);
  if (x1 <= x0 || y1 <= y0) return out;
  Accumulator acc;
  acc.w = x1 - x0;
  acc.h = y1 - y0;
  acc.acc.assign(static_cast<std::size_t>(acc.w + 2) * acc.h, 0.0f);
  const Point origin{static_cast<double>(x0), static_cast<double>(y0)};
  for (const Polyline& l : lines) {
    const std::size_t n = l.points.size();
    if (n < 2) continue;
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t j = i + 1 < n ? i + 1 : 0;  // fills always close their subpaths
      const Point a = l.points[i] - origin, b = l.points[j] - origin;
      if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) continue;
      // Clamp absurd coordinates so the per-row loop stays bounded.
      acc.line(a, b);
    }
  }
  out.x0 = x0;
  out.y0 = y0;
  out.w = acc.w;
  out.h = acc.h;
  out.cov.resize(static_cast<std::size_t>(out.w) * out.h);
  const std::size_t stride = static_cast<std::size_t>(acc.w) + 2;
  for (int y = 0; y < out.h; ++y) {
    double sum = 0;
    const float* row = &acc.acc[static_cast<std::size_t>(y) * stride];
    float* dst = &out.cov[static_cast<std::size_t>(y) * out.w];
    for (int x = 0; x < out.w; ++x) {
      sum += row[x];
      double v = std::abs(sum);
      if (rule == FillRule::NonZero) {
        v = std::min(1.0, v);
      } else {
        v = std::fmod(v, 2.0);
        if (v > 1.0) v = 2.0 - v;
      }
      dst[x] = static_cast<float>(v < 1e-4 ? 0.0 : v);
    }
  }
  return out;
}

// ------------------------------------------------------------------ colours

Px premul(const Color& c, double opacity) {
  const float a = static_cast<float>(std::clamp(c.a * opacity, 0.0, 1.0));
  return {c.r * a, c.g * a, c.b * a, a};
}

float to_linear(float v) { return image::srgb_to_linear(std::clamp(v, 0.0f, 1.0f)); }
float to_srgb(float v) { return image::linear_to_srgb(std::clamp(v, 0.0f, 1.0f)); }

struct GradientLut {
  std::array<Px, 1024> table{};
  void build(const Gradient& g, double opacity) {
    std::vector<GradientStop> stops = g.stops;
    if (stops.empty()) stops.push_back({0, Color{0, 0, 0, 0}});
    double last = 0;
    for (GradientStop& s : stops) {
      s.offset = std::clamp(std::max(s.offset, last), 0.0, 1.0);
      last = s.offset;
    }
    for (std::size_t i = 0; i < table.size(); ++i) {
      const double t = static_cast<double>(i) / static_cast<double>(table.size() - 1);
      Color c;
      if (t <= stops.front().offset) {
        c = stops.front().color;
      } else if (t >= stops.back().offset) {
        c = stops.back().color;
      } else {
        std::size_t k = 1;
        while (k < stops.size() && stops[k].offset < t) ++k;
        const GradientStop& a = stops[k - 1];
        const GradientStop& b = stops[std::min(k, stops.size() - 1)];
        const double span = b.offset - a.offset;
        const double f = span > 1e-12 ? (t - a.offset) / span : 1.0;
        const auto mix = [&](float x, float y) { return static_cast<float>(x + (y - x) * f); };
        if (g.linear_rgb) {
          c.r = to_srgb(mix(to_linear(a.color.r), to_linear(b.color.r)));
          c.g = to_srgb(mix(to_linear(a.color.g), to_linear(b.color.g)));
          c.b = to_srgb(mix(to_linear(a.color.b), to_linear(b.color.b)));
          c.a = mix(a.color.a, b.color.a);
        } else {
          // Interpolate premultiplied so transparent stops do not darken.
          const float pa = a.color.a, pb = b.color.a;
          const float alpha = mix(pa, pb);
          const auto pm = [&](float ca, float cb) {
            const float v = mix(ca * pa, cb * pb);
            return alpha > 1e-6f ? v / alpha : 0.0f;
          };
          c.r = pm(a.color.r, b.color.r);
          c.g = pm(a.color.g, b.color.g);
          c.b = pm(a.color.b, b.color.b);
          c.a = alpha;
        }
      }
      table[i] = premul(c, opacity);
    }
  }
  [[nodiscard]] const Px& lookup(double t, Spread spread) const {
    if (!std::isfinite(t)) t = 0;
    switch (spread) {
      case Spread::Pad: t = std::clamp(t, 0.0, 1.0); break;
      case Spread::Repeat: t = t - std::floor(t); break;
      case Spread::Reflect: {
        double m = std::fmod(std::abs(t), 2.0);
        t = m > 1.0 ? 2.0 - m : m;
        break;
      }
    }
    const auto i = static_cast<std::size_t>(std::lround(t * static_cast<double>(table.size() - 1)));
    return table[std::min(i, table.size() - 1)];
  }
};

// ------------------------------------------------------------------ paint sources

struct Sampler {
  enum class Kind { Solid, Linear, Radial, Image, None };
  Kind kind = Kind::None;
  Px solid{};
  // Gradients.
  std::shared_ptr<GradientLut> lut;
  Matrix inv;  // device -> gradient / image space
  Spread spread = Spread::Pad;
  Point p1, p2;
  Point center, focal;
  double radius = 0, focal_radius = 0;
  // Images (premultiplied float pixels); wrap = pattern tiles.
  std::shared_ptr<const Canvas> image;
  bool wrap = false;
  bool smooth = true;
  double opacity = 1.0;

  [[nodiscard]] Px fetch(int x, int y) const {
    if (wrap) {
      x %= image->w;
      if (x < 0) x += image->w;
      y %= image->h;
      if (y < 0) y += image->h;
    } else if (x < 0 || y < 0 || x >= image->w || y >= image->h) {
      x = std::clamp(x, 0, image->w - 1);
      y = std::clamp(y, 0, image->h - 1);
    }
    const float* p = image->at(x, y);
    return {p[0], p[1], p[2], p[3]};
  }

  [[nodiscard]] Px at(double x, double y) const {
    switch (kind) {
      case Kind::None: return {0, 0, 0, 0};
      case Kind::Solid: return solid;
      case Kind::Linear: {
        const Point q = inv.apply({x, y});
        const Point d = p2 - p1;
        const double len2 = dot(d, d);
        const double t = len2 > 1e-18 ? dot(q - p1, d) / len2 : 1.0;
        return lut->lookup(t, spread);
      }
      case Kind::Radial: {
        const Point q = inv.apply({x, y});
        // Two-point conical gradient: circles (focal, fr) at t=0 to (center, r) at t=1.
        const Point cd = center - focal;
        const double dr = radius - focal_radius;
        const Point pd = q - focal;
        const double a = dot(cd, cd) - dr * dr;
        const double b = dot(pd, cd) + focal_radius * dr;
        const double c = dot(pd, pd) - focal_radius * focal_radius;
        double t;
        if (std::abs(a) < 1e-9) {
          if (std::abs(b) < 1e-12) return {0, 0, 0, 0};
          t = c / (2 * b);
        } else {
          const double disc = b * b - a * c;
          if (disc < 0) return {0, 0, 0, 0};
          const double sq = std::sqrt(disc);
          const double t1 = (b + sq) / a, t2 = (b - sq) / a;
          t = std::max(t1, t2);
          if (focal_radius + t * dr < 0) t = std::min(t1, t2);
          if (focal_radius + t * dr < 0) return {0, 0, 0, 0};
        }
        return lut->lookup(t, spread);
      }
      case Kind::Image: {
        const Point q = inv.apply({x, y});
        Px out;
        if (!smooth) {
          out = fetch(static_cast<int>(std::floor(q.x)), static_cast<int>(std::floor(q.y)));
        } else {
          const double fx = q.x - 0.5, fy = q.y - 0.5;
          const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
          const float tx = static_cast<float>(fx - ix), ty = static_cast<float>(fy - iy);
          const Px a = fetch(ix, iy), b = fetch(ix + 1, iy), c = fetch(ix, iy + 1), d = fetch(ix + 1, iy + 1);
          for (int k = 0; k < 4; ++k) {
            const auto ku = static_cast<std::size_t>(k);
            out[ku] = (a[ku] * (1 - tx) + b[ku] * tx) * (1 - ty) + (c[ku] * (1 - tx) + d[ku] * tx) * ty;
          }
        }
        if (opacity != 1.0) {
          for (float& v : out) v *= static_cast<float>(opacity);
        }
        return out;
      }
    }
    return {0, 0, 0, 0};
  }
};

// ------------------------------------------------------------------ blending

float blend_channel(BlendMode mode, float cs, float cd) {
  switch (mode) {
    case BlendMode::Multiply: return cs * cd;
    case BlendMode::Screen: return cs + cd - cs * cd;
    case BlendMode::Overlay: return cd <= 0.5f ? 2 * cs * cd : 1 - 2 * (1 - cs) * (1 - cd);
    case BlendMode::Darken: return std::min(cs, cd);
    case BlendMode::Lighten: return std::max(cs, cd);
    case BlendMode::ColorDodge:
      if (cd == 0) return 0;
      return cs >= 1 ? 1 : std::min(1.0f, cd / (1 - cs));
    case BlendMode::ColorBurn:
      if (cd >= 1) return 1;
      return cs <= 0 ? 0 : 1 - std::min(1.0f, (1 - cd) / cs);
    case BlendMode::HardLight: return cs <= 0.5f ? 2 * cs * cd : 1 - 2 * (1 - cs) * (1 - cd);
    case BlendMode::SoftLight: {
      if (cs <= 0.5f) return cd - (1 - 2 * cs) * cd * (1 - cd);
      const float d = cd <= 0.25f ? ((16 * cd - 12) * cd + 4) * cd : std::sqrt(cd);
      return cd + (2 * cs - 1) * (d - cd);
    }
    case BlendMode::Difference: return std::abs(cs - cd);
    case BlendMode::Exclusion: return cs + cd - 2 * cs * cd;
    default: return cs;
  }
}

using Rgb = std::array<float, 3>;
float lum(const Rgb& c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
Rgb clip_color(Rgb c) {
  const float l = lum(c);
  const float n = std::min({c[0], c[1], c[2]}), x = std::max({c[0], c[1], c[2]});
  for (float& v : c) {
    if (n < 0) v = l + (v - l) * l / (l - n);
    if (x > 1) v = l + (v - l) * (1 - l) / (x - l);
  }
  return c;
}
Rgb set_lum(Rgb c, float l) {
  const float d = l - lum(c);
  for (float& v : c) v += d;
  return clip_color(c);
}
float sat(const Rgb& c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }
Rgb set_sat(Rgb c, float s) {
  std::array<int, 3> idx = {0, 1, 2};
  std::sort(idx.begin(), idx.end(), [&](int a, int b) { return c[static_cast<std::size_t>(a)] < c[static_cast<std::size_t>(b)]; });
  float& mn = c[static_cast<std::size_t>(idx[0])];
  float& md = c[static_cast<std::size_t>(idx[1])];
  float& mx = c[static_cast<std::size_t>(idx[2])];
  if (mx > mn) {
    md = (md - mn) * s / (mx - mn);
    mx = s;
  } else {
    md = mx = 0;
  }
  mn = 0;
  return c;
}

// Composite premultiplied `s` onto `d` with a blend mode (source-over).
void composite(float* d, const float* s, BlendMode mode) {
  const float sa = s[3], da = d[3];
  if (sa <= 0) return;
  if (mode == BlendMode::Normal || da <= 0) {
    for (int k = 0; k < 4; ++k) d[k] = s[k] + d[k] * (1 - sa);
    return;
  }
  const Rgb cs = {s[0] / sa, s[1] / sa, s[2] / sa};
  const Rgb cd = {d[0] / da, d[1] / da, d[2] / da};
  Rgb b{};
  switch (mode) {
    case BlendMode::Hue: b = set_lum(set_sat(cs, sat(cd)), lum(cd)); break;
    case BlendMode::Saturation: b = set_lum(set_sat(cd, sat(cs)), lum(cd)); break;
    case BlendMode::Color: b = set_lum(cs, lum(cd)); break;
    case BlendMode::Luminosity: b = set_lum(cd, lum(cs)); break;
    default:
      for (std::size_t k = 0; k < 3; ++k) b[k] = blend_channel(mode, cs[k], cd[k]);
  }
  for (std::size_t k = 0; k < 3; ++k) {
    d[k] = (1 - da) * s[k] + (1 - sa) * d[k] + sa * da * std::clamp(b[k], 0.0f, 1.0f);
  }
  d[3] = sa + da - sa * da;
}

// ------------------------------------------------------------------ renderer

struct Renderer {
  Renderer(const Context& context, int width, int height) : ctx(context), w(width), h(height) {}
  const Context& ctx;
  int w, h;
  int layer_depth = 0;
  int pattern_depth = 0;
  std::uint64_t budget = 0;  // float pixels still allowed for layers
  std::uint64_t nodes = 0;
  std::map<const ir::Frame*, std::shared_ptr<const Canvas>> picture_cache;
  std::map<const Picture*, std::shared_ptr<const ir::Frame>> decoded;

  Result<Canvas> new_canvas() {
    const std::uint64_t n = static_cast<std::uint64_t>(w) * h;
    if (n > budget) return fail(Status::Limit, "the drawing needs more memory for layers than the limits allow");
    budget -= n;
    return Canvas(w, h);
  }
  void release(const Canvas& c) { budget += static_cast<std::uint64_t>(c.w) * c.h; }

  Result<void> tick() {
    if ((++nodes & 255) == 0) CVT_TRY(ctx.checkpoint(0.5, "Rendering"));
    return {};
  }

  // ---------------------------------------------------------------- paint
  Result<Sampler> sampler(const Paint& paint, const Matrix& m, const Rect& device_box) {
    Sampler s;
    if (!paint.visible()) return s;
    switch (paint.type) {
      case Paint::Type::None: return s;
      case Paint::Type::Solid:
        s.kind = Sampler::Kind::Solid;
        s.solid = premul(paint.color, paint.opacity);
        return s;
      case Paint::Type::Gradient: {
        if (!paint.gradient) {
          s.kind = Sampler::Kind::Solid;
          s.solid = premul(paint.color, paint.opacity);
          return s;
        }
        const Gradient& g = *paint.gradient;
        const Matrix gm = m * g.transform;
        if (!gm.invertible()) {
          // Degenerate gradient space: the last stop colour (SVG).
          s.kind = Sampler::Kind::Solid;
          s.solid = g.stops.empty() ? Px{0, 0, 0, 0} : premul(g.stops.back().color, paint.opacity);
          return s;
        }
        if (g.stops.size() == 1) {
          s.kind = Sampler::Kind::Solid;
          s.solid = premul(g.stops.front().color, paint.opacity);
          return s;
        }
        s.lut = std::make_shared<GradientLut>();
        s.lut->build(g, paint.opacity);
        s.inv = gm.inverted();
        s.spread = g.spread;
        if (g.type == Gradient::Type::Linear) {
          s.kind = Sampler::Kind::Linear;
          s.p1 = g.p1;
          s.p2 = g.p2;
          if (length(g.p2 - g.p1) < 1e-12) {
            s.kind = Sampler::Kind::Solid;
            s.solid = premul(g.stops.back().color, paint.opacity);
          }
        } else {
          s.kind = Sampler::Kind::Radial;
          s.center = g.center;
          s.radius = std::max(0.0, g.radius);
          s.focal = g.focal;
          s.focal_radius = std::max(0.0, g.focal_radius);
          if (s.radius <= 0) {
            s.kind = Sampler::Kind::Solid;
            s.solid = premul(g.stops.back().color, paint.opacity);
          }
        }
        return s;
      }
      case Paint::Type::Pattern: {
        if (!paint.pattern || !paint.pattern->content || paint.pattern->tile.empty() ||
            pattern_depth >= max_pattern_depth) {
          s.kind = Sampler::Kind::Solid;
          s.solid = premul(paint.color, paint.opacity * 0.0);
          return s;
        }
        const PatternPaint& p = *paint.pattern;
        const Matrix pm = m * p.transform;
        if (!pm.invertible() || !(p.tile.width() > 0) || !(p.tile.height() > 0)) return s;
        const double sx = length(pm.apply_vector({p.tile.width(), 0}));
        const double sy = length(pm.apply_vector({0, p.tile.height()}));
        // Tile resolution: device resolution, capped.
        int tw = static_cast<int>(std::clamp(std::ceil(sx), 1.0, 2048.0));
        int th = static_cast<int>(std::clamp(std::ceil(sy), 1.0, 2048.0));
        (void)device_box;
        Renderer sub{ctx, tw, th};
        sub.budget = std::min<std::uint64_t>(budget, 64ull * 1024 * 1024);
        sub.pattern_depth = pattern_depth + 1;
        CVT_TRY_ASSIGN(Canvas tile, sub.new_canvas());
        const Matrix tm = Matrix::scale(tw / p.tile.width(), th / p.tile.height()) *
                          Matrix::translate(-p.tile.x0, -p.tile.y0);
        CVT_TRY(sub.draw_group(*p.content, tm, tile));
        if (paint.opacity < 1) {
          for (float& v : tile.px) v *= static_cast<float>(paint.opacity);
        }
        s.kind = Sampler::Kind::Image;
        s.image = std::make_shared<const Canvas>(std::move(tile));
        s.wrap = true;
        // device -> tile pixels
        s.inv = Matrix::scale(tw / p.tile.width(), th / p.tile.height()) * Matrix::translate(-p.tile.x0, -p.tile.y0) *
                pm.inverted();
        return s;
      }
    }
    return s;
  }

  void fill_coverage(Canvas& target, const Coverage& cov, const Sampler& s) {
    if (cov.empty() || s.kind == Sampler::Kind::None) return;
    int tx0 = target.w, ty0 = target.h, tx1 = 0, ty1 = 0;
    for (int y = 0; y < cov.h; ++y) {
      const int cy = cov.y0 + y;
      const float* row = &cov.cov[static_cast<std::size_t>(y) * cov.w];
      for (int x = 0; x < cov.w; ++x) {
        const float c = row[x];
        if (c <= 0) continue;
        const int cx = cov.x0 + x;
        Px src = s.at(cx + 0.5, cy + 0.5);
        if (src[3] <= 0) continue;
        float* d = target.at(cx, cy);
        const float a = src[3] * c;
        for (std::size_t k = 0; k < 4; ++k) d[k] = src[k] * c + d[k] * (1 - a);
        tx0 = std::min(tx0, cx), ty0 = std::min(ty0, cy), tx1 = std::max(tx1, cx + 1), ty1 = std::max(ty1, cy + 1);
      }
    }
    target.touch(tx0, ty0, tx1, ty1);
  }

  Result<void> paint_path(Canvas& target, const Path& device_path, FillRule rule, const Paint& paint, const Matrix& m) {
    if (!paint.visible() || device_path.empty()) return {};
    const Coverage cov = rasterize(device_path, rule, w, h);
    if (cov.empty()) return {};
    CVT_TRY_ASSIGN(const Sampler s, sampler(paint, m, {}));
    fill_coverage(target, cov, s);
    return {};
  }

  Path stroke_device(const Path& path, const Stroke& stroke, const Matrix& m) {
    if (stroke.hairline || stroke.non_scaling) {
      Stroke s = stroke;
      if (stroke.hairline) s.width = 1.0;
      return stroke_outline(transformed(path, m), s, device_tolerance);
    }
    const double scale = std::max(m.mean_scale(), 1e-9);
    return transformed(stroke_outline(path, stroke, device_tolerance / scale), m);
  }

  Result<void> draw_shape(const Shape& shape, const Matrix& m, Canvas& target) {
    const auto fill = [&]() -> Result<void> {
      if (!shape.fill.visible()) return {};
      return paint_path(target, transformed(shape.path, m), shape.fill_rule, shape.fill, m);
    };
    const auto stroke = [&]() -> Result<void> {
      if (!shape.stroke.paint.visible() || !(shape.stroke.width > 0 || shape.stroke.hairline)) return {};
      return paint_path(target, stroke_device(shape.path, shape.stroke, m), FillRule::NonZero, shape.stroke.paint, m);
    };
    if (shape.stroke_first) {
      CVT_TRY(stroke());
      return fill();
    }
    CVT_TRY(fill());
    return stroke();
  }

  Result<void> draw_text(const Text& text, const Matrix& m, Canvas& target) {
    if (text.fill.visible()) CVT_TRY(paint_path(target, transformed(text.outline, m), FillRule::NonZero, text.fill, m));
    if (text.stroke.paint.visible() && text.stroke.width > 0) {
      CVT_TRY(paint_path(target, stroke_device(text.outline, text.stroke, m), FillRule::NonZero, text.stroke.paint, m));
    }
    return {};
  }

  Result<std::shared_ptr<const ir::Frame>> picture_pixels(const Picture& p) {
    if (p.pixels && p.pixels->valid()) return p.pixels;
    if (auto it = decoded.find(&p); it != decoded.end()) return it->second;
    if (p.encoded.empty()) return std::shared_ptr<const ir::Frame>{};
    std::string format = p.encoded_format;
    if (format.empty()) {
      if (auto sniffed = image::sniff_format(p.encoded)) format = *sniffed;
    }
    auto image = image::decode(format, p.encoded, ctx);
    if (!image || !image->valid()) {
      ctx.warn("An embedded picture could not be decoded and was left out.");
      decoded[&p] = nullptr;
      return std::shared_ptr<const ir::Frame>{};
    }
    auto frame = std::make_shared<const ir::Frame>(std::move(image->frames.front()));
    decoded[&p] = frame;
    return frame;
  }

  Result<void> draw_picture(const Picture& p, const Matrix& m, Canvas& target) {
    CVT_TRY_ASSIGN(std::shared_ptr<const ir::Frame> frame, picture_pixels(p));
    if (!frame || p.bounds.empty() || !(p.bounds.width() > 0) || !(p.bounds.height() > 0)) return {};
    const Matrix img_to_user = Matrix::translate(p.bounds.x0, p.bounds.y0) *
                               Matrix::scale(p.bounds.width() / frame->width, p.bounds.height() / frame->height);
    Matrix t = m * img_to_user;
    if (!t.invertible()) return {};
    // Pre-shrink large reductions for quality (area filtering).
    const double sx = length(t.apply_vector({1, 0})), sy = length(t.apply_vector({0, 1}));
    const ir::Frame* src = frame.get();
    ir::Frame shrunk;
    double fx = 1, fy = 1;
    if (p.smooth && (sx < 0.5 || sy < 0.5)) {
      const auto nw = static_cast<std::uint32_t>(std::max(1.0, std::ceil(frame->width * std::min(1.0, sx * 2))));
      const auto nh = static_cast<std::uint32_t>(std::max(1.0, std::ceil(frame->height * std::min(1.0, sy * 2))));
      auto r = image::resize(*frame, nw, nh);
      if (r) {
        shrunk = std::move(*r);
        fx = static_cast<double>(frame->width) / nw;
        fy = static_cast<double>(frame->height) / nh;
        src = &shrunk;
      }
    }
    auto canvas = std::make_shared<Canvas>(static_cast<int>(src->width), static_cast<int>(src->height));
    for (std::size_t i = 0; i < src->rgba.size(); i += 4) {
      const float a = src->rgba[i + 3] / 255.0f;
      canvas->px[i] = src->rgba[i] / 255.0f * a;
      canvas->px[i + 1] = src->rgba[i + 1] / 255.0f * a;
      canvas->px[i + 2] = src->rgba[i + 2] / 255.0f * a;
      canvas->px[i + 3] = a;
    }
    t = t * Matrix::scale(fx, fy);
    Sampler s;
    s.kind = Sampler::Kind::Image;
    s.image = canvas;
    s.inv = t.inverted();
    s.smooth = p.smooth;
    Path rect;
    add_rect(rect, 0, 0, src->width, src->height);
    const Coverage cov = rasterize(transformed(rect, t), FillRule::NonZero, w, h);
    fill_coverage(target, cov, s);
    return {};
  }

  Result<void> draw_node(const Node& node, const Matrix& m, Canvas& target) {
    CVT_TRY(tick());
    if (const auto* s = std::get_if<Shape>(&node.value)) return draw_shape(*s, m, target);
    if (const auto* t = std::get_if<Text>(&node.value)) return draw_text(*t, m, target);
    if (const auto* p = std::get_if<Picture>(&node.value)) return draw_picture(*p, m, target);
    if (const auto* g = std::get_if<Group>(&node.value)) return draw_group(*g, m, target);
    return {};
  }

  // Coverage of a clip path chain, full canvas size.
  Result<std::vector<float>> clip_mask(const ClipPath& clip, const Matrix& m, int depth) {
    if (depth > 32) return fail(Status::Limit, "clip paths are nested too deeply");
    std::vector<float> mask(static_cast<std::size_t>(w) * h, 0.0f);
    for (const ClipPath::Item& item : clip.items) {
      const Coverage cov = rasterize(transformed(item.path, m), item.rule, w, h);
      for (int y = 0; y < cov.h; ++y) {
        for (int x = 0; x < cov.w; ++x) {
          float& v = mask[static_cast<std::size_t>(cov.y0 + y) * w + (cov.x0 + x)];
          v = std::max(v, cov.cov[static_cast<std::size_t>(y) * cov.w + x]);
        }
      }
    }
    if (clip.clip) {
      CVT_TRY_ASSIGN(const std::vector<float> inner, clip_mask(*clip.clip, m, depth + 1));
      for (std::size_t i = 0; i < mask.size(); ++i) mask[i] *= inner[i];
    }
    return mask;
  }

  Result<void> apply_filter(Canvas& layer, const Filter& filter, const Matrix& m);

  Result<void> draw_group(const Group& group, const Matrix& parent, Canvas& target) {
    if (!group.visible) return {};
    const Matrix m = parent * group.transform;
    if (!(group.opacity > 0)) return {};
    const bool needs_layer = group.opacity < 1.0 || group.clip || group.mask || group.filter ||
                             group.blend != BlendMode::Normal;
    if (!needs_layer || layer_depth >= max_layer_depth) {
      if (needs_layer) ctx.warn("Effects nested too deeply were drawn without their opacity, clip or mask.");
      for (const Node& child : group.children) CVT_TRY(draw_node(child, m, target));
      return {};
    }
    ++layer_depth;
    CVT_TRY_ASSIGN(Canvas layer, new_canvas());
    for (const Node& child : group.children) CVT_TRY(draw_node(child, m, layer));
    if (group.filter) {
      CVT_TRY(apply_filter(layer, *group.filter, m));
    }
    if (layer.blank()) {
      release(layer);
      --layer_depth;
      return {};
    }
    if (group.clip) {
      CVT_TRY_ASSIGN(const std::vector<float> clip, clip_mask(*group.clip, m, 0));
      for (int y = layer.dy0; y < layer.dy1; ++y) {
        for (int x = layer.dx0; x < layer.dx1; ++x) {
          float* p = layer.at(x, y);
          const float c = clip[static_cast<std::size_t>(y) * w + x];
          for (int k = 0; k < 4; ++k) p[k] *= c;
        }
      }
    }
    if (group.mask && group.mask->content) {
      CVT_TRY_ASSIGN(Canvas mask, new_canvas());
      CVT_TRY(draw_group(*group.mask->content, m, mask));
      std::vector<float> region;
      if (!group.mask->region.empty()) {
        Path r;
        add_rect(r, group.mask->region.x0, group.mask->region.y0, group.mask->region.width(),
                 group.mask->region.height());
        const Coverage cov = rasterize(transformed(r, m), FillRule::NonZero, w, h);
        region.assign(static_cast<std::size_t>(w) * h, 0.0f);
        for (int y = 0; y < cov.h; ++y) {
          for (int x = 0; x < cov.w; ++x) {
            region[static_cast<std::size_t>(cov.y0 + y) * w + cov.x0 + x] = cov.cov[static_cast<std::size_t>(y) * cov.w + x];
          }
        }
      }
      for (int y = layer.dy0; y < layer.dy1; ++y) {
        for (int x = layer.dx0; x < layer.dx1; ++x) {
          const float* q = mask.at(x, y);
          float v = q[3];
          if (group.mask->type == Mask::Type::Luminance) {
            // Luminance of the un-premultiplied colour times alpha = luminance of premultiplied.
            v = 0.2125f * q[0] + 0.7154f * q[1] + 0.0721f * q[2];
          }
          if (!region.empty()) v *= region[static_cast<std::size_t>(y) * w + x];
          float* p = layer.at(x, y);
          for (int k = 0; k < 4; ++k) p[k] *= v;
        }
      }
      release(mask);
    }
    const float opacity = static_cast<float>(group.opacity);
    for (int y = layer.dy0; y < layer.dy1; ++y) {
      for (int x = layer.dx0; x < layer.dx1; ++x) {
        float s[4];
        const float* p = layer.at(x, y);
        for (int k = 0; k < 4; ++k) s[k] = p[k] * opacity;
        composite(target.at(x, y), s, group.blend);
      }
    }
    target.touch(layer.dx0, layer.dy0, layer.dx1, layer.dy1);
    release(layer);
    --layer_depth;
    return {};
  }
};

// ------------------------------------------------------------------ filters

using Plane = std::vector<float>;  // w*h*4 premultiplied

void box_blur_h(const Plane& src, Plane& dst, int w, int h, int r_left, int r_right) {
  const int size = r_left + r_right + 1;
  for (int y = 0; y < h; ++y) {
    for (int k = 0; k < 4; ++k) {
      double sum = 0;
      const auto px = [&](int x) -> float {
        if (x < 0 || x >= w) return 0.0f;
        return src[(static_cast<std::size_t>(y) * w + x) * 4 + k];
      };
      for (int x = -r_left; x <= r_right; ++x) sum += px(x);
      for (int x = 0; x < w; ++x) {
        dst[(static_cast<std::size_t>(y) * w + x) * 4 + k] = static_cast<float>(sum / size);
        sum += px(x + r_right + 1) - px(x - r_left);
      }
    }
  }
}

void transpose(const Plane& src, Plane& dst, int w, int h) {
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (int k = 0; k < 4; ++k) {
        dst[(static_cast<std::size_t>(x) * h + y) * 4 + k] = src[(static_cast<std::size_t>(y) * w + x) * 4 + k];
      }
    }
  }
}

// Gaussian approximation by three box blurs (SVG 1.1 filter effects, feGaussianBlur).
void blur_axis(Plane& p, int w, int h, double sigma) {
  if (!(sigma > 0)) return;
  Plane tmp(p.size());
  if (sigma < 2.0) {
    // True Gaussian kernel for small deviations.
    const int r = static_cast<int>(std::ceil(sigma * 3));
    std::vector<double> kernel(static_cast<std::size_t>(2 * r + 1));
    double total = 0;
    for (int i = -r; i <= r; ++i) {
      const double v = std::exp(-(i * i) / (2 * sigma * sigma));
      kernel[static_cast<std::size_t>(i + r)] = v;
      total += v;
    }
    for (double& v : kernel) v /= total;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        for (int k = 0; k < 4; ++k) {
          double sum = 0;
          for (int i = -r; i <= r; ++i) {
            const int xx = x + i;
            if (xx < 0 || xx >= w) continue;
            sum += kernel[static_cast<std::size_t>(i + r)] * p[(static_cast<std::size_t>(y) * w + xx) * 4 + k];
          }
          tmp[(static_cast<std::size_t>(y) * w + x) * 4 + k] = static_cast<float>(sum);
        }
      }
    }
    p.swap(tmp);
    return;
  }
  const int d = static_cast<int>(std::floor(sigma * 3 * std::sqrt(2 * std::numbers::pi) / 4 + 0.5));
  if (d % 2 == 1) {
    const int r = d / 2;
    box_blur_h(p, tmp, w, h, r, r);
    box_blur_h(tmp, p, w, h, r, r);
    box_blur_h(p, tmp, w, h, r, r);
  } else {
    const int r = d / 2;
    box_blur_h(p, tmp, w, h, r, r - 1);
    box_blur_h(tmp, p, w, h, r - 1, r);
    box_blur_h(p, tmp, w, h, r, r);
  }
  p.swap(tmp);
}

void gaussian_blur(Plane& p, int w, int h, double sx, double sy) {
  blur_axis(p, w, h, sx);
  if (sy > 0) {
    Plane t(p.size());
    transpose(p, t, w, h);
    blur_axis(t, h, w, sy);
    transpose(t, p, h, w);
  }
}

Plane offset_plane(const Plane& p, int w, int h, double dx, double dy) {
  Plane out(p.size(), 0.0f);
  const int ix = static_cast<int>(std::lround(dx)), iy = static_cast<int>(std::lround(dy));
  for (int y = 0; y < h; ++y) {
    const int sy = y - iy;
    if (sy < 0 || sy >= h) continue;
    for (int x = 0; x < w; ++x) {
      const int sx = x - ix;
      if (sx < 0 || sx >= w) continue;
      for (int k = 0; k < 4; ++k) {
        out[(static_cast<std::size_t>(y) * w + x) * 4 + k] = p[(static_cast<std::size_t>(sy) * w + sx) * 4 + k];
      }
    }
  }
  return out;
}

void convert_plane(Plane& p, bool to_lin) {
  for (std::size_t i = 0; i < p.size(); i += 4) {
    const float a = p[i + 3];
    if (a <= 0) continue;
    for (std::size_t k = 0; k < 3; ++k) {
      const float c = p[i + k] / a;
      p[i + k] = (to_lin ? to_linear(c) : to_srgb(c)) * a;
    }
  }
}

Result<void> Renderer::apply_filter(Canvas& layer, const Filter& filter, const Matrix& m) {
  const std::size_t n = static_cast<std::size_t>(w) * h * 4;
  // Device region.
  Rect region = filter.region.empty() ? Rect::xywh(0, 0, w, h) : transform_rect(filter.region, m);
  region = region.intersect(Rect::xywh(0, 0, w, h));
  const double sx_scale = std::hypot(m.a, m.b), sy_scale = std::hypot(m.c, m.d);
  const bool linear = filter.primitives.empty() || filter.primitives.front().linear_rgb;
  std::map<std::string, Plane> results;
  Plane source = layer.px;
  if (linear) convert_plane(source, true);
  Plane alpha(n, 0.0f);
  for (std::size_t i = 3; i < n; i += 4) alpha[i] = source[i];
  Plane last = source;
  const auto input = [&](const std::string& name) -> const Plane& {
    if (name == "SourceGraphic") return source;
    if (name == "SourceAlpha") return alpha;
    if (!name.empty()) {
      if (auto it = results.find(name); it != results.end()) return it->second;
    }
    return last;
  };
  for (const FilterPrimitive& prim : filter.primitives) {
    CVT_TRY(ctx.checkpoint(0.5, "Filtering"));
    Plane out;
    switch (prim.type) {
      case FilterPrimitive::Type::GaussianBlur:
        out = input(prim.in);
        gaussian_blur(out, w, h, prim.std_dev_x * sx_scale, prim.std_dev_y * sy_scale);
        break;
      case FilterPrimitive::Type::Offset: {
        const Point d = m.apply_vector({prim.dx, prim.dy});
        out = offset_plane(input(prim.in), w, h, d.x, d.y);
        break;
      }
      case FilterPrimitive::Type::Flood: {
        Color c = prim.color;
        if (linear) c = {to_linear(c.r), to_linear(c.g), to_linear(c.b), c.a};
        const Px f = premul(c, 1.0);
        out.assign(n, 0.0f);
        for (std::size_t i = 0; i < n; i += 4) {
          for (std::size_t k = 0; k < 4; ++k) out[i + k] = f[k];
        }
        break;
      }
      case FilterPrimitive::Type::Merge: {
        out.assign(n, 0.0f);
        for (const std::string& name : prim.merge_inputs) {
          const Plane& in = input(name);
          for (std::size_t i = 0; i < n; i += 4) composite(&out[i], &in[i], BlendMode::Normal);
        }
        break;
      }
      case FilterPrimitive::Type::ColorMatrix: {
        out = input(prim.in);
        if (prim.matrix.size() == 20) {
          const auto& M = prim.matrix;
          for (std::size_t i = 0; i < n; i += 4) {
            const float a = out[i + 3];
            const double r = a > 0 ? out[i] / a : 0, g = a > 0 ? out[i + 1] / a : 0, b = a > 0 ? out[i + 2] / a : 0;
            double v[4];
            for (std::size_t row = 0; row < 4; ++row) {
              v[row] = std::clamp(M[row * 5] * r + M[row * 5 + 1] * g + M[row * 5 + 2] * b + M[row * 5 + 3] * a +
                                      M[row * 5 + 4],
                                  0.0, 1.0);
            }
            out[i] = static_cast<float>(v[0] * v[3]);
            out[i + 1] = static_cast<float>(v[1] * v[3]);
            out[i + 2] = static_cast<float>(v[2] * v[3]);
            out[i + 3] = static_cast<float>(v[3]);
          }
        }
        break;
      }
      case FilterPrimitive::Type::DropShadow: {
        const Plane& in = input(prim.in);
        Plane shadow(n, 0.0f);
        Color c = prim.color;
        if (linear) c = {to_linear(c.r), to_linear(c.g), to_linear(c.b), c.a};
        for (std::size_t i = 0; i < n; i += 4) {
          const float a = in[i + 3] * c.a;
          shadow[i] = c.r * a;
          shadow[i + 1] = c.g * a;
          shadow[i + 2] = c.b * a;
          shadow[i + 3] = a;
        }
        gaussian_blur(shadow, w, h, prim.std_dev_x * sx_scale, prim.std_dev_y * sy_scale);
        const Point d = m.apply_vector({prim.dx, prim.dy});
        out = offset_plane(shadow, w, h, d.x, d.y);
        for (std::size_t i = 0; i < n; i += 4) composite(&out[i], &in[i], BlendMode::Normal);
        break;
      }
      case FilterPrimitive::Type::Blend: {
        out = input(prim.in2);
        const Plane& top = input(prim.in);
        for (std::size_t i = 0; i < n; i += 4) composite(&out[i], &top[i], prim.blend);
        break;
      }
      case FilterPrimitive::Type::Composite: {
        const Plane& a = input(prim.in);
        const Plane& b = input(prim.in2);
        out.assign(n, 0.0f);
        for (std::size_t i = 0; i < n; i += 4) {
          const float aa = a[i + 3], ba = b[i + 3];
          for (std::size_t k = 0; k < 4; ++k) {
            float v = 0;
            if (prim.op == "in") v = a[i + k] * ba;
            else if (prim.op == "out") v = a[i + k] * (1 - ba);
            else if (prim.op == "atop") v = a[i + k] * ba + b[i + k] * (1 - aa);
            else if (prim.op == "xor") v = a[i + k] * (1 - ba) + b[i + k] * (1 - aa);
            else if (prim.op == "arithmetic")
              v = static_cast<float>(prim.k1 * a[i + k] * b[i + k] + prim.k2 * a[i + k] + prim.k3 * b[i + k] + prim.k4);
            else v = a[i + k] + b[i + k] * (1 - aa);
            out[i + k] = std::clamp(v, 0.0f, 1.0f);
          }
          if (prim.op == "arithmetic") {
            for (std::size_t k = 0; k < 3; ++k) out[i + k] = std::min(out[i + k], out[i + 3]);
          }
        }
        break;
      }
    }
    // Clip each result to the filter region.
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        if (x + 0.5 < region.x0 || x + 0.5 > region.x1 || y + 0.5 < region.y0 || y + 0.5 > region.y1) {
          float* p = &out[(static_cast<std::size_t>(y) * w + x) * 4];
          p[0] = p[1] = p[2] = p[3] = 0;
        }
      }
    }
    if (!prim.result.empty()) results[prim.result] = out;
    last = std::move(out);
  }
  if (linear) convert_plane(last, false);
  layer.px = std::move(last);
  layer.touch_all();
  return {};
}

ir::Frame to_frame(const Canvas& c, const std::optional<Color>& background) {
  ir::Frame frame = ir::Frame::filled(static_cast<std::uint32_t>(c.w), static_cast<std::uint32_t>(c.h));
  const Px bg = background ? premul(*background, 1.0) : Px{0, 0, 0, 0};
  for (std::size_t i = 0, o = 0; i < c.px.size(); i += 4, o += 4) {
    float p[4] = {bg[0], bg[1], bg[2], bg[3]};
    composite(p, &c.px[i], BlendMode::Normal);
    const float a = std::clamp(p[3], 0.0f, 1.0f);
    const auto q = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    frame.rgba[o + 3] = q(a);
    if (a > 0) {
      frame.rgba[o] = q(p[0] / a);
      frame.rgba[o + 1] = q(p[1] / a);
      frame.rgba[o + 2] = q(p[2] / a);
    }
  }
  return frame;
}

std::uint64_t layer_budget(const Context& ctx, std::uint64_t pixels) {
  // Up to 16 full-size layers, never more than four times the pixel limit.
  const std::uint64_t cap = ctx.limits.max_pixels * 4;
  return std::min<std::uint64_t>(pixels * 16, std::max<std::uint64_t>(cap, pixels));
}

}  // namespace

Result<ir::Frame> render_group(const Group& group, const Matrix& transform, std::uint32_t width, std::uint32_t height,
                               const Context& ctx) {
  if (width == 0 || height == 0) return fail(Status::Encode, "nothing to render: the canvas has no pixels");
  CVT_TRY(ctx.limits.check_pixels(width, height));
  const std::uint64_t pixels = std::uint64_t{width} * height;
  Renderer r{ctx, static_cast<int>(width), static_cast<int>(height)};
  r.budget = layer_budget(ctx, pixels);
  CVT_TRY_ASSIGN(Canvas canvas, r.new_canvas());
  CVT_TRY(r.draw_group(group, transform, canvas));
  return to_frame(canvas, std::nullopt);
}

Result<ir::Frame> render_page(const Scene& scene, const RenderOptions& options, const Context& ctx) {
  if (options.page >= scene.pages.size()) {
    if (scene.pages.empty()) return fail(Status::Decode, "the drawing has no pages");
    return fail(Status::InvalidArgument, "page {} does not exist; the drawing has {} page(s)", options.page + 1,
                scene.pages.size());
  }
  const Page& page = scene.pages[options.page];
  if (!(page.width > 0) || !(page.height > 0) || !std::isfinite(page.width) || !std::isfinite(page.height)) {
    return fail(Status::Decode, "the drawing has no size");
  }
  const double dpi = options.dpi > 0 ? options.dpi : 96.0;
  double pw = page.width * dpi / 96.0, ph = page.height * dpi / 96.0;
  if (options.width > 0 && options.height > 0) {
    pw = options.width;
    ph = options.height;
  } else if (options.width > 0) {
    ph = ph * options.width / pw;
    pw = options.width;
  } else if (options.height > 0) {
    pw = pw * options.height / ph;
    ph = options.height;
  }
  const double wd = std::max(1.0, std::round(pw)), hd = std::max(1.0, std::round(ph));
  if (wd > 1e6 || hd > 1e6) return fail(Status::Limit, "the rendered picture would be {}x{} pixels", wd, hd);
  const auto width = static_cast<std::uint32_t>(wd), height = static_cast<std::uint32_t>(hd);
  CVT_TRY(ctx.limits.check_pixels(width, height));
  const Matrix m = Matrix::scale(width / page.width, height / page.height);
  Renderer r{ctx, static_cast<int>(width), static_cast<int>(height)};
  r.budget = layer_budget(ctx, std::uint64_t{width} * height);
  CVT_TRY_ASSIGN(Canvas canvas, r.new_canvas());
  CVT_TRY(r.draw_group(page.root, m, canvas));
  CVT_TRY(ctx.checkpoint(1.0, "Rendering"));
  return to_frame(canvas, options.background);
}

}  // namespace cvt::vector
