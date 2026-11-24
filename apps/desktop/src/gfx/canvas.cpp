// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "gfx/canvas.hpp"

#include <array>
#include <cmath>
#include <cstring>

namespace cvd::gfx {

namespace {

inline std::uint32_t scale_px(std::uint32_t s, std::uint32_t cov256) {
  const std::uint32_t rb = ((s & 0x00FF00FFu) * cov256 >> 8) & 0x00FF00FFu;
  const std::uint32_t ag = (((s >> 8) & 0x00FF00FFu) * cov256) & 0xFF00FF00u;
  return rb | ag;
}

/// Source-over blend of a premultiplied pixel scaled by coverage (0..256).
inline void blend_px(std::uint32_t& d, std::uint32_t s, std::uint32_t cov256) {
  if (cov256 == 0) return;
  if (cov256 < 256) s = scale_px(s, cov256);
  const std::uint32_t sa = s >> 24;
  if (sa == 255) {
    d = s;
    return;
  }
  if (sa == 0 && s == 0) return;
  const std::uint32_t inv = 255 - sa;
  std::uint32_t drb = (d & 0x00FF00FFu) * inv;
  drb = ((drb + 0x00800080u + ((drb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
  std::uint32_t dag = ((d >> 8) & 0x00FF00FFu) * inv;
  dag = (dag + 0x00800080u + ((dag >> 8) & 0x00FF00FFu)) & 0xFF00FF00u;
  d = s + drb + dag;
}

inline std::uint32_t cov_to_256(float c) {
  if (c <= 0.0f) return 0;
  if (c >= 1.0f) return 256;
  return static_cast<std::uint32_t>(c * 256.0f + 0.5f);
}

std::uint32_t premul(Color c, float opacity) { return c.with_alpha(c.a * opacity).premul_u32(); }

/// Area-coverage accumulation (after the approach popularised by font-rs):
/// every edge adds signed area to the cells it crosses; a running sum along
/// each row yields the winding-weighted coverage.
void accumulate_line(float* a, int stride, int w, int h, Vec2 p0, Vec2 p1) {
  if (p0.y == p1.y) return;
  float dir = 1.0f;
  if (p0.y > p1.y) {
    std::swap(p0, p1);
    dir = -1.0f;
  }
  if (p1.y <= 0.0f || p0.y >= static_cast<float>(h)) return;
  const float fw = static_cast<float>(w);
  const float dxdy = (p1.x - p0.x) / (p1.y - p0.y);
  float x = p0.x;
  const float ystart = std::max(p0.y, 0.0f);
  if (p0.y < 0.0f) x -= p0.y * dxdy;
  const int y0 = static_cast<int>(ystart);
  const int yend = std::min(h, static_cast<int>(std::ceil(p1.y)));
  for (int y = y0; y < yend; ++y) {
    float* row = a + static_cast<std::ptrdiff_t>(y) * stride;
    const float dy = std::min(static_cast<float>(y + 1), p1.y) - std::max(static_cast<float>(y), p0.y);
    float xnext = x + dxdy * dy;
    x = clampf(x, 0.0f, fw);
    xnext = clampf(xnext, 0.0f, fw);
    const float d = dy * dir;
    const float x0 = std::min(x, xnext);
    const float x1 = std::max(x, xnext);
    const float x0floor = std::floor(x0);
    const int x0i = static_cast<int>(x0floor);
    const float x1ceil = std::ceil(x1);
    const int x1i = static_cast<int>(x1ceil);
    if (x1i <= x0i + 1) {
      const float xmf = 0.5f * (x + xnext) - x0floor;
      row[x0i] += d - d * xmf;
      row[x0i + 1] += d * xmf;
    } else {
      const float s = 1.0f / (x1 - x0);
      const float x0f = x0 - x0floor;
      const float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
      const float x1f = x1 - x1ceil + 1.0f;
      const float am = 0.5f * s * x1f * x1f;
      row[x0i] += d * a0;
      if (x1i == x0i + 2) {
        row[x0i + 1] += d * (1.0f - a0 - am);
      } else {
        const float a1 = s * (1.5f - x0f);
        row[x0i + 1] += d * (a1 - a0);
        for (int xi = x0i + 2; xi < x1i - 1; ++xi) row[xi] += d * s;
        const float a2 = a1 + static_cast<float>(x1i - x0i - 3) * s;
        row[x1i - 1] += d * (1.0f - a2 - am);
      }
      row[x1i] += d * am;
    }
    x = xnext;
  }
}

/// Adds an edge, splitting it where it leaves the [0, w] column range so that
/// the parts outside are clamped onto the border (preserving winding).
void add_edge(float* a, int stride, int w, int h, Vec2 p, Vec2 q) {
  const float fw = static_cast<float>(w);
  std::array<float, 4> ts{0.0f, 1.0f, 1.0f, 1.0f};
  int count = 1;
  const float dx = q.x - p.x;
  if (dx != 0.0f) {
    for (const float bound : {0.0f, fw}) {
      const float t = (bound - p.x) / dx;
      if (t > 0.0f && t < 1.0f) ts[static_cast<std::size_t>(count++)] = t;
    }
  }
  ts[static_cast<std::size_t>(count++)] = 1.0f;
  std::sort(ts.begin(), ts.begin() + count);
  for (int i = 0; i + 1 < count; ++i) {
    const float t0 = ts[static_cast<std::size_t>(i)];
    const float t1 = ts[static_cast<std::size_t>(i + 1)];
    if (t1 - t0 <= 0.0f) continue;
    Vec2 s0 = p + (q - p) * t0;
    Vec2 s1 = p + (q - p) * t1;
    s0.x = clampf(s0.x, 0.0f, fw);
    s1.x = clampf(s1.x, 0.0f, fw);
    accumulate_line(a, stride, w, h, s0, s1);
  }
}

IRect contour_bounds(const std::vector<Contour>& contours) {
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (const auto& c : contours) {
    for (const Vec2 p : c.points) {
      x0 = std::min(x0, p.x);
      y0 = std::min(y0, p.y);
      x1 = std::max(x1, p.x);
      y1 = std::max(y1, p.y);
    }
  }
  if (x0 > x1) return {};
  const auto lim = [](float v) { return clampf(v, -1.0e6f, 1.0e6f); };
  return {static_cast<int>(std::floor(lim(x0))), static_cast<int>(std::floor(lim(y0))),
          static_cast<int>(std::ceil(lim(x1))) + 1, static_cast<int>(std::ceil(lim(y1))) + 1};
}

float erf_approx(float x) {
  const float s = x < 0.0f ? -1.0f : 1.0f;
  const float a = std::fabs(x);
  float t = 1.0f + (0.278393f + (0.230389f + 0.078108f * (a * a)) * a) * a;
  t *= t;
  return s - s / (t * t);
}

float gaussian(float x, float sigma) {
  return std::exp(-(x * x) / (2.0f * sigma * sigma)) / (2.50662827f * sigma);
}

// Rounded-box shadow (Evan Wallace's closed form along x, sampled along y).
float shadow_x(float x, float y, float sigma, float corner, float hw, float hh) {
  const float delta = std::min(hh - corner - std::fabs(y), 0.0f);
  const float curved = hw - corner + std::sqrt(std::max(0.0f, corner * corner - delta * delta));
  const float k = 0.70710678f / sigma;
  return 0.5f * (erf_approx((x + curved) * k) - erf_approx((x - curved) * k));
}

float shadow_value(float px, float py, float sigma, float corner, float hw, float hh) {
  const float low = py - hh;
  const float high = py + hh;
  const float start = clampf(-3.0f * sigma, low, high);
  const float end = clampf(3.0f * sigma, low, high);
  constexpr int kSteps = 6;
  const float step = (end - start) / static_cast<float>(kSteps);
  float y = start + step * 0.5f;
  float value = 0.0f;
  for (int i = 0; i < kSteps; ++i) {
    value += shadow_x(px, py - y, sigma, corner, hw, hh) * gaussian(y, sigma) * step;
    y += step;
  }
  return value;
}

float rrect_sdf(float px, float py, float hw, float hh, float r) {
  const float qx = std::fabs(px) - (hw - r);
  const float qy = std::fabs(py) - (hh - r);
  if (qx <= 0.0f && qy <= 0.0f) return std::max(qx, qy) - r;
  const float ox = std::max(qx, 0.0f);
  const float oy = std::max(qy, 0.0f);
  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shader
// ---------------------------------------------------------------------------

struct Canvas::Shader {
  Paint::Kind kind = Paint::Kind::Solid;
  std::uint32_t solid = 0;
  Affine inv;
  Vec2 p0{};
  Vec2 d{};
  float inv_len2 = 0.0f;
  float inv_radius = 0.0f;
  std::array<std::uint32_t, 256> lut{};

  Shader(const Paint& paint, const Affine& ctm, float opacity) : kind(paint.kind) {
    if (kind == Paint::Kind::Solid || paint.stops.empty()) {
      kind = Paint::Kind::Solid;
      solid = premul(paint.stops.empty() ? paint.color : paint.stops.front().color, opacity);
      return;
    }
    inv = ctm.inverted();
    p0 = paint.p0;
    d = paint.p1 - paint.p0;
    const float l2 = d.dot(d);
    inv_len2 = l2 > 1e-12f ? 1.0f / l2 : 0.0f;
    inv_radius = paint.radius > 1e-6f ? 1.0f / paint.radius : 0.0f;
    const auto& stops = paint.stops;
    for (int i = 0; i < 256; ++i) {
      const float t = static_cast<float>(i) / 255.0f;
      Color c = stops.front().color;
      if (t <= stops.front().offset) {
        c = stops.front().color;
      } else if (t >= stops.back().offset) {
        c = stops.back().color;
      } else {
        for (std::size_t s = 0; s + 1 < stops.size(); ++s) {
          if (t >= stops[s].offset && t <= stops[s + 1].offset) {
            const float span = stops[s + 1].offset - stops[s].offset;
            const float u = span > 1e-6f ? (t - stops[s].offset) / span : 0.0f;
            c = Color::lerp(stops[s].color, stops[s + 1].color, u);
            break;
          }
        }
      }
      lut[static_cast<std::size_t>(i)] = premul(c, opacity);
    }
  }

  [[nodiscard]] std::uint32_t at(int x, int y) const {
    if (kind == Paint::Kind::Solid) return solid;
    const Vec2 u = inv.apply({static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f});
    float t = 0.0f;
    if (kind == Paint::Kind::Linear) {
      t = (u - p0).dot(d) * inv_len2;
    } else {
      t = (u - p0).length() * inv_radius;
    }
    const int idx = static_cast<int>(clampf(t, 0.0f, 1.0f) * 255.0f + 0.5f);
    return lut[static_cast<std::size_t>(idx)];
  }

  void span(std::uint32_t* row, int y, int x0, int x1, const float* cov) const {
    for (int x = x0; x < x1; ++x) {
      const std::uint32_t c = cov_to_256(cov[x - x0]);
      if (c) blend_px(row[x], at(x, y), c);
    }
  }

  void solid_span(std::uint32_t* row, int y, int x0, int x1, std::uint32_t cov256) const {
    if (kind == Paint::Kind::Solid) {
      for (int x = x0; x < x1; ++x) blend_px(row[x], solid, cov256);
    } else {
      for (int x = x0; x < x1; ++x) blend_px(row[x], at(x, y), cov256);
    }
  }
};

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

Canvas::Canvas(Bitmap& target) : target_(&target) {
  state_.clip = {0, 0, target.width, target.height};
}

void Canvas::save() { stack_.push_back(state_); }

void Canvas::restore() {
  if (stack_.empty()) return;
  state_ = stack_.back();
  stack_.pop_back();
}

void Canvas::translate(float x, float y) { state_.ctm = state_.ctm * Affine::translate(x, y); }
void Canvas::scale(float sx, float sy) { state_.ctm = state_.ctm * Affine::scale(sx, sy); }
void Canvas::rotate(float radians) { state_.ctm = state_.ctm * Affine::rotate(radians); }
void Canvas::concat(const Affine& t) { state_.ctm = state_.ctm * t; }
void Canvas::set_transform(const Affine& t) { state_.ctm = t; }
void Canvas::set_opacity(float alpha) { state_.opacity *= clampf(alpha, 0.0f, 1.0f); }

IRect Canvas::device_bounds(const Rect& r) const {
  const Vec2 pts[4] = {state_.ctm.apply({r.x, r.y}), state_.ctm.apply({r.right(), r.y}),
                       state_.ctm.apply({r.x, r.bottom()}), state_.ctm.apply({r.right(), r.bottom()})};
  float x0 = pts[0].x, y0 = pts[0].y, x1 = x0, y1 = y0;
  for (const Vec2 p : pts) {
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
  }
  return {static_cast<int>(std::floor(x0)), static_cast<int>(std::floor(y0)), static_cast<int>(std::ceil(x1)),
          static_cast<int>(std::ceil(y1))};
}

void Canvas::clip_rect(const Rect& r) {
  // Round to the nearest pixel edge so clips line up with snapped content.
  const Vec2 a = state_.ctm.apply({r.x, r.y});
  const Vec2 b = state_.ctm.apply({r.right(), r.bottom()});
  IRect dev{static_cast<int>(std::lround(std::min(a.x, b.x))), static_cast<int>(std::lround(std::min(a.y, b.y))),
            static_cast<int>(std::lround(std::max(a.x, b.x))), static_cast<int>(std::lround(std::max(a.y, b.y)))};
  if (!state_.ctm.is_axis_aligned()) dev = device_bounds(r);
  state_.clip = state_.clip.intersect(dev);
}

void Canvas::clear(Color c) {
  const std::uint32_t v = c.premul_u32();
  const IRect clip = state_.clip;
  for (int y = clip.y0; y < clip.y1; ++y) {
    std::uint32_t* row = target_->row(y);
    std::fill(row + clip.x0, row + clip.x1, v);
  }
}

void Canvas::rasterize(const std::vector<Contour>& contours, const Paint& paint) {
  IRect bb = contour_bounds(contours).intersect(state_.clip);
  if (bb.empty() || state_.opacity <= 0.0f) return;
  const int w = bb.width();
  const int h = bb.height();
  const int stride = w + 3;
  accum_.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(h), 0.0f);
  const Vec2 origin{static_cast<float>(bb.x0), static_cast<float>(bb.y0)};
  for (const auto& c : contours) {
    const auto& pts = c.points;
    if (pts.size() < 2) continue;
    for (std::size_t i = 0; i < pts.size(); ++i) {
      const Vec2 p = pts[i] - origin;
      const Vec2 q = pts[(i + 1) % pts.size()] - origin;
      add_edge(accum_.data(), stride, w, h, p, q);
    }
  }
  const Shader shader(paint, state_.ctm, state_.opacity);
  coverage_.resize(static_cast<std::size_t>(w));
  for (int y = 0; y < h; ++y) {
    const float* a = accum_.data() + static_cast<std::ptrdiff_t>(y) * stride;
    float acc = 0.0f;
    bool any = false;
    for (int x = 0; x < w; ++x) {
      acc += a[x];
      const float cov = std::min(1.0f, std::fabs(acc));
      coverage_[static_cast<std::size_t>(x)] = cov;
      any = any || cov > 0.002f;
    }
    if (any) shader.span(target_->row(bb.y0 + y), bb.y0 + y, bb.x0, bb.x1, coverage_.data());
  }
}

void Canvas::fill_device_contours(const std::vector<Contour>& contours, const Paint& paint) {
  rasterize(contours, paint);
}

void Canvas::fill_path(const Path& path, const Paint& paint) {
  rasterize(path.flatten(state_.ctm, 0.08f), paint);
}

void Canvas::stroke_path(const Path& path, const StrokeStyle& style, const Paint& paint) {
  const auto lines = path.flatten(state_.ctm, 0.08f);
  StrokeStyle dev = style;
  dev.width = style.width * state_.ctm.mean_scale();
  std::vector<Contour> pieces;
  for (const auto& line : lines) {
    auto p = cvd::gfx::stroke_polyline(line, dev);
    pieces.insert(pieces.end(), std::make_move_iterator(p.begin()), std::make_move_iterator(p.end()));
  }
  rasterize(pieces, paint);
}

void Canvas::stroke_polyline(const std::vector<Vec2>& points, bool closed, const StrokeStyle& style,
                             const Paint& paint) {
  Contour line;
  line.closed = closed;
  line.points.reserve(points.size());
  for (const Vec2 p : points) line.points.push_back(state_.ctm.apply(p));
  StrokeStyle dev = style;
  dev.width = style.width * state_.ctm.mean_scale();
  rasterize(cvd::gfx::stroke_polyline(line, dev), paint);
}

void Canvas::fill_rect(const Rect& r, const Paint& paint) {
  if (r.empty()) return;
  if (!state_.ctm.is_axis_aligned()) {
    Path p;
    p.add_rect(r);
    fill_path(p, paint);
    return;
  }
  const Vec2 a = state_.ctm.apply({r.x, r.y});
  const Vec2 b = state_.ctm.apply({r.right(), r.bottom()});
  const IRect bb = IRect{static_cast<int>(std::floor(a.x)), static_cast<int>(std::floor(a.y)),
                         static_cast<int>(std::ceil(b.x)), static_cast<int>(std::ceil(b.y))}
                       .intersect(state_.clip);
  if (bb.empty()) return;
  const Shader shader(paint, state_.ctm, state_.opacity);
  coverage_.resize(static_cast<std::size_t>(bb.width()));
  for (int x = bb.x0; x < bb.x1; ++x) {
    const float fx = static_cast<float>(x);
    coverage_[static_cast<std::size_t>(x - bb.x0)] = clampf(std::min(fx + 1.0f, b.x) - std::max(fx, a.x), 0.0f, 1.0f);
  }
  std::vector<float> row_cov(coverage_.size());
  for (int y = bb.y0; y < bb.y1; ++y) {
    const float fy = static_cast<float>(y);
    const float cy = clampf(std::min(fy + 1.0f, b.y) - std::max(fy, a.y), 0.0f, 1.0f);
    if (cy >= 0.999f) {
      shader.span(target_->row(y), y, bb.x0, bb.x1, coverage_.data());
    } else {
      for (std::size_t i = 0; i < coverage_.size(); ++i) row_cov[i] = coverage_[i] * cy;
      shader.span(target_->row(y), y, bb.x0, bb.x1, row_cov.data());
    }
  }
}

void Canvas::fill_rrect(const Rect& r, float radius, const Paint& paint) {
  if (r.empty()) return;
  radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
  if (radius <= 0.01f) {
    fill_rect(r, paint);
    return;
  }
  if (!state_.ctm.is_axis_aligned()) {
    Path p;
    p.add_rrect(r, radius);
    fill_path(p, paint);
    return;
  }
  const Vec2 a = state_.ctm.apply({r.x, r.y});
  const Vec2 b = state_.ctm.apply({r.right(), r.bottom()});
  const float rad = radius * std::min(state_.ctm.a, state_.ctm.d);
  const float cx = (a.x + b.x) * 0.5f;
  const float cy = (a.y + b.y) * 0.5f;
  const float hw = (b.x - a.x) * 0.5f;
  const float hh = (b.y - a.y) * 0.5f;
  const IRect bb = IRect{static_cast<int>(std::floor(a.x)), static_cast<int>(std::floor(a.y)),
                         static_cast<int>(std::ceil(b.x)), static_cast<int>(std::ceil(b.y))}
                       .intersect(state_.clip);
  if (bb.empty()) return;
  const Shader shader(paint, state_.ctm, state_.opacity);
  coverage_.resize(static_cast<std::size_t>(bb.width()));
  constexpr std::uint32_t full = 256;  // opacity is already folded into the shader
  for (int y = bb.y0; y < bb.y1; ++y) {
    const float py = static_cast<float>(y) + 0.5f - cy;
    std::uint32_t* row = target_->row(y);
    int inner0 = bb.x1, inner1 = bb.x1;
    if (std::fabs(py) <= hh - rad && std::fabs(py) <= hh - 0.5f) {
      inner0 = std::max(bb.x0, static_cast<int>(std::ceil(a.x)) + 1);
      inner1 = std::min(bb.x1, static_cast<int>(std::floor(b.x)) - 1);
      if (inner1 <= inner0) inner0 = inner1 = bb.x1;  // no interior span
    }
    for (int x = bb.x0; x < bb.x1; ++x) {
      if (x == inner0) {
        shader.solid_span(row, y, inner0, inner1, full);
        x = inner1 - 1;
        continue;
      }
      const float px = static_cast<float>(x) + 0.5f - cx;
      const float cov = clampf(0.5f - rrect_sdf(px, py, hw, hh, rad), 0.0f, 1.0f);
      const std::uint32_t c = cov_to_256(cov);
      if (c) blend_px(row[x], shader.at(x, y), c);
    }
  }
}

void Canvas::stroke_rrect(const Rect& r, float radius, float width, const Paint& paint) {
  if (r.empty() || width <= 0.0f) return;
  radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
  if (!state_.ctm.is_axis_aligned()) {
    Path p;
    p.add_rrect(r.inset(width * 0.5f), std::max(0.0f, radius - width * 0.5f));
    StrokeStyle st;
    st.width = width;
    stroke_path(p, st, paint);
    return;
  }
  const Vec2 a = state_.ctm.apply({r.x, r.y});
  const Vec2 b = state_.ctm.apply({r.right(), r.bottom()});
  const float s = std::min(state_.ctm.a, state_.ctm.d);
  const float rad = std::max(radius * s, 0.0f);
  const float wd = width * s;
  const float cx = (a.x + b.x) * 0.5f;
  const float cy = (a.y + b.y) * 0.5f;
  const float hw = (b.x - a.x) * 0.5f;
  const float hh = (b.y - a.y) * 0.5f;
  const IRect bb = IRect{static_cast<int>(std::floor(a.x)), static_cast<int>(std::floor(a.y)),
                         static_cast<int>(std::ceil(b.x)), static_cast<int>(std::ceil(b.y))}
                       .intersect(state_.clip);
  if (bb.empty()) return;
  const Shader shader(paint, state_.ctm, state_.opacity);
  const float skip = std::max(rad, wd) + 1.5f;
  for (int y = bb.y0; y < bb.y1; ++y) {
    const float py = static_cast<float>(y) + 0.5f - cy;
    std::uint32_t* row = target_->row(y);
    const bool middle = std::fabs(py) < hh - skip;
    for (int x = bb.x0; x < bb.x1; ++x) {
      const float px = static_cast<float>(x) + 0.5f - cx;
      if (middle && std::fabs(px) < hw - skip) {
        // Jump over the hollow interior.
        const int jump = static_cast<int>(std::floor(cx + hw - skip)) - 1;
        if (jump > x) x = jump;
        continue;
      }
      const float d = rrect_sdf(px, py, hw, hh, rad);
      const float cov = clampf(0.5f - d, 0.0f, 1.0f) - clampf(0.5f - (d + wd), 0.0f, 1.0f);
      const std::uint32_t c = cov_to_256(cov);
      if (c) blend_px(row[x], shader.at(x, y), c);
    }
  }
}

void Canvas::fill_circle(Vec2 c, float radius, const Paint& paint) {
  fill_rrect(Rect{c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f}, radius, paint);
}

void Canvas::draw_shadow(const Rect& r, float radius, float blur, float spread, Vec2 offset, Color color) {
  if (color.a <= 0.0f) return;
  Rect sr{r.x - spread + offset.x, r.y - spread + offset.y, r.w + 2.0f * spread, r.h + 2.0f * spread};
  if (sr.empty()) return;
  const float s = state_.ctm.mean_scale();
  const float sigma = blur * 0.5f * s;
  if (sigma < 0.4f || !state_.ctm.is_axis_aligned()) {
    fill_rrect(sr, std::max(0.0f, radius + spread), color);
    return;
  }
  const Vec2 a = state_.ctm.apply({sr.x, sr.y});
  const Vec2 b = state_.ctm.apply({sr.right(), sr.bottom()});
  const float hw = (b.x - a.x) * 0.5f;
  const float hh = (b.y - a.y) * 0.5f;
  const float corner = std::min(std::max(0.0f, (radius + spread) * s), std::min(hw, hh));
  const float cx = (a.x + b.x) * 0.5f;
  const float cy = (a.y + b.y) * 0.5f;
  const float ext = 3.0f * sigma;
  const IRect bb = IRect{static_cast<int>(std::floor(a.x - ext)), static_cast<int>(std::floor(a.y - ext)),
                         static_cast<int>(std::ceil(b.x + ext)), static_cast<int>(std::ceil(b.y + ext))}
                       .intersect(state_.clip);
  if (bb.empty()) return;
  const std::uint32_t px_color = premul(color, state_.opacity);
  const float k = 0.70710678f / sigma;
  const float straight_x = hw - corner - ext;  // |px| below this: only the y profile matters
  const float straight_y = hh - corner - ext;
  for (int y = bb.y0; y < bb.y1; ++y) {
    const float py = static_cast<float>(y) + 0.5f - cy;
    std::uint32_t* row = target_->row(y);
    const float vy = 0.5f * (erf_approx((py + hh) * k) - erf_approx((py - hh) * k));
    const bool row_straight = std::fabs(py) < straight_y;
    for (int x = bb.x0; x < bb.x1; ++x) {
      const float px = static_cast<float>(x) + 0.5f - cx;
      float v;
      if (std::fabs(px) < straight_x) {
        v = vy;
      } else if (row_straight) {
        v = 0.5f * (erf_approx((px + hw) * k) - erf_approx((px - hw) * k));
      } else {
        v = shadow_value(px, py, sigma, corner, hw, hh);
      }
      const std::uint32_t c = cov_to_256(v);
      if (c) blend_px(row[x], px_color, c);
    }
  }
}

void Canvas::draw_mask(const Mask& mask, int device_x, int device_y, Color color) {
  if (mask.width <= 0 || mask.height <= 0) return;
  const int x0 = device_x + mask.left;
  const int y0 = device_y + mask.top;
  const IRect bb = IRect{x0, y0, x0 + mask.width, y0 + mask.height}.intersect(state_.clip);
  if (bb.empty()) return;
  const std::uint32_t c = premul(color, state_.opacity);
  for (int y = bb.y0; y < bb.y1; ++y) {
    std::uint32_t* row = target_->row(y);
    const std::uint8_t* m = mask.alpha.data() + static_cast<std::size_t>(y - y0) * static_cast<std::size_t>(mask.width);
    for (int x = bb.x0; x < bb.x1; ++x) {
      const std::uint32_t a = m[x - x0];
      if (a) blend_px(row[x], c, a + (a >> 7));
    }
  }
}

void Canvas::draw_image(const Bitmap& image, const Rect& dst, float alpha) {
  if (image.width <= 0 || image.height <= 0 || dst.empty()) return;
  const IRect bb = device_bounds(dst).intersect(state_.clip);
  if (bb.empty()) return;
  const Affine to_img = (state_.ctm * Affine::translate(dst.x, dst.y) *
                         Affine::scale(dst.w / static_cast<float>(image.width), dst.h / static_cast<float>(image.height)))
                            .inverted();
  const std::uint32_t op = cov_to_256(alpha * state_.opacity);
  const auto fetch = [&](int x, int y) -> std::uint32_t {
    x = std::clamp(x, 0, image.width - 1);
    y = std::clamp(y, 0, image.height - 1);
    return image.at(x, y);
  };
  for (int y = bb.y0; y < bb.y1; ++y) {
    std::uint32_t* row = target_->row(y);
    for (int x = bb.x0; x < bb.x1; ++x) {
      const Vec2 u = to_img.apply({static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f});
      if (u.x < 0.0f || u.y < 0.0f || u.x >= static_cast<float>(image.width) || u.y >= static_cast<float>(image.height)) {
        continue;
      }
      const float fx = u.x - 0.5f;
      const float fy = u.y - 0.5f;
      const int ix = static_cast<int>(std::floor(fx));
      const int iy = static_cast<int>(std::floor(fy));
      const auto wx = static_cast<std::uint32_t>((fx - static_cast<float>(ix)) * 256.0f);
      const auto wy = static_cast<std::uint32_t>((fy - static_cast<float>(iy)) * 256.0f);
      const std::uint32_t p00 = fetch(ix, iy), p10 = fetch(ix + 1, iy), p01 = fetch(ix, iy + 1),
                          p11 = fetch(ix + 1, iy + 1);
      std::uint32_t out = 0;
      for (int ch = 0; ch < 32; ch += 8) {
        const auto c00 = (p00 >> ch) & 0xFFu, c10 = (p10 >> ch) & 0xFFu, c01 = (p01 >> ch) & 0xFFu,
                   c11 = (p11 >> ch) & 0xFFu;
        const std::uint32_t top = c00 * (256 - wx) + c10 * wx;
        const std::uint32_t bot = c01 * (256 - wx) + c11 * wx;
        const std::uint32_t v = (top * (256 - wy) + bot * wy) >> 16;
        out |= std::min(v, 255u) << ch;
      }
      blend_px(row[x], out, op);
    }
  }
}

namespace {

void box_blur_line(std::uint8_t* data, int count, int step, int radius, std::vector<std::uint8_t>& tmp) {
  if (radius <= 0 || count <= 1) return;
  tmp.resize(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) tmp[static_cast<std::size_t>(i)] = data[static_cast<std::ptrdiff_t>(i) * step];
  const int window = 2 * radius + 1;
  int sum = 0;
  for (int i = -radius; i <= radius; ++i) sum += tmp[static_cast<std::size_t>(std::clamp(i, 0, count - 1))];
  for (int i = 0; i < count; ++i) {
    data[static_cast<std::ptrdiff_t>(i) * step] = static_cast<std::uint8_t>((sum + window / 2) / window);
    const int out_i = std::clamp(i - radius, 0, count - 1);
    const int in_i = std::clamp(i + radius + 1, 0, count - 1);
    sum += tmp[static_cast<std::size_t>(in_i)] - tmp[static_cast<std::size_t>(out_i)];
  }
}

}  // namespace

void blur_mask(Mask& mask, float radius) {
  const int r = static_cast<int>(std::lround(radius / 1.73f));
  if (r <= 0) return;
  std::vector<std::uint8_t> tmp;
  for (int pass = 0; pass < 3; ++pass) {
    for (int y = 0; y < mask.height; ++y) {
      box_blur_line(mask.alpha.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(mask.width), mask.width,
                    1, r, tmp);
    }
    for (int x = 0; x < mask.width; ++x) box_blur_line(mask.alpha.data() + x, mask.height, mask.width, r, tmp);
  }
}

void Canvas::blur_region(const Rect& r, float radius) {
  const IRect bb = device_bounds(r).intersect(state_.clip);
  if (bb.empty()) return;
  const int rad = static_cast<int>(std::lround(radius * state_.ctm.mean_scale() / 1.73f));
  if (rad <= 0) return;
  const int w = bb.width();
  const int h = bb.height();
  std::vector<std::uint8_t> plane(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
  std::vector<std::uint8_t> tmp;
  for (int ch = 0; ch < 32; ch += 8) {
    for (int y = 0; y < h; ++y) {
      const std::uint32_t* row = target_->row(bb.y0 + y);
      for (int x = 0; x < w; ++x) {
        plane[static_cast<std::size_t>(y * w + x)] = static_cast<std::uint8_t>((row[bb.x0 + x] >> ch) & 0xFFu);
      }
    }
    for (int pass = 0; pass < 3; ++pass) {
      for (int y = 0; y < h; ++y) box_blur_line(plane.data() + static_cast<std::size_t>(y * w), w, 1, rad, tmp);
      for (int x = 0; x < w; ++x) box_blur_line(plane.data() + x, h, w, rad, tmp);
    }
    for (int y = 0; y < h; ++y) {
      std::uint32_t* row = target_->row(bb.y0 + y);
      for (int x = 0; x < w; ++x) {
        std::uint32_t& px = row[bb.x0 + x];
        px = (px & ~(0xFFu << ch)) | (static_cast<std::uint32_t>(plane[static_cast<std::size_t>(y * w + x)]) << ch);
      }
    }
  }
}

std::vector<std::uint8_t> to_rgba8(const Bitmap& bitmap) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(bitmap.width) * static_cast<std::size_t>(bitmap.height) * 4);
  for (std::size_t i = 0; i < bitmap.pixels.size(); ++i) {
    const std::uint32_t p = bitmap.pixels[i];
    const std::uint32_t a = p >> 24;
    std::uint32_t r = (p >> 16) & 0xFFu, g = (p >> 8) & 0xFFu, b = p & 0xFFu;
    if (a > 0 && a < 255) {
      r = std::min(255u, (r * 255 + a / 2) / a);
      g = std::min(255u, (g * 255 + a / 2) / a);
      b = std::min(255u, (b * 255 + a / 2) / a);
    }
    out[i * 4 + 0] = static_cast<std::uint8_t>(r);
    out[i * 4 + 1] = static_cast<std::uint8_t>(g);
    out[i * 4 + 2] = static_cast<std::uint8_t>(b);
    out[i * 4 + 3] = static_cast<std::uint8_t>(a);
  }
  return out;
}

Mask rasterize_mask(const std::vector<Contour>& contours) {
  Mask m;
  const IRect bb = contour_bounds(contours);
  if (bb.empty()) return m;
  const int w = bb.width();
  const int h = bb.height();
  const int stride = w + 3;
  std::vector<float> acc(static_cast<std::size_t>(stride) * static_cast<std::size_t>(h), 0.0f);
  const Vec2 origin{static_cast<float>(bb.x0), static_cast<float>(bb.y0)};
  for (const auto& c : contours) {
    const auto& pts = c.points;
    if (pts.size() < 2) continue;
    for (std::size_t i = 0; i < pts.size(); ++i) {
      add_edge(acc.data(), stride, w, h, pts[i] - origin, pts[(i + 1) % pts.size()] - origin);
    }
  }
  m.width = w;
  m.height = h;
  m.left = bb.x0;
  m.top = bb.y0;
  m.alpha.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
  for (int y = 0; y < h; ++y) {
    float s = 0.0f;
    const float* a = acc.data() + static_cast<std::ptrdiff_t>(y) * stride;
    for (int x = 0; x < w; ++x) {
      s += a[x];
      const float cov = std::min(1.0f, std::fabs(s));
      m.alpha[static_cast<std::size_t>(y * w + x)] = static_cast<std::uint8_t>(cov * 255.0f + 0.5f);
    }
  }
  return m;
}

}  // namespace cvd::gfx
