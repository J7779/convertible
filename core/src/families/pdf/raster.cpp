// Convertible core - pdf family: anti-aliased scanline rasteriser and stroker.
//
// Filling uses exact-area accumulation: every edge adds its signed area
// contribution to the cells it crosses, and a running sum along each row
// yields the winding coverage, folded by the fill rule. Edges are clipped to
// the target box first (left of the box they become a vertical edge at the
// left border, right of it they vanish), so huge coordinates cost nothing.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/raster.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cvt::pdf::raster {

IntRect IntRect::intersect(const IntRect& o) const noexcept {
  IntRect r{std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
  if (r.empty()) return IntRect{};
  return r;
}

namespace {

bool finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }

void flatten_cubic(Point p0, Point p1, Point p2, Point p3, double tolerance, std::vector<Point>& out, int depth) {
  // Distance of control points from the chord as flatness estimate.
  const double ux = 3 * p1.x - 2 * p0.x - p3.x, uy = 3 * p1.y - 2 * p0.y - p3.y;
  const double vx = 3 * p2.x - 2 * p3.x - p0.x, vy = 3 * p2.y - 2 * p3.y - p0.y;
  const double d = std::max(ux * ux, vx * vx) + std::max(uy * uy, vy * vy);
  if (depth >= 16 || d <= 16 * tolerance * tolerance) {
    out.push_back(p3);
    return;
  }
  const Point p01{(p0.x + p1.x) / 2, (p0.y + p1.y) / 2}, p12{(p1.x + p2.x) / 2, (p1.y + p2.y) / 2},
      p23{(p2.x + p3.x) / 2, (p2.y + p3.y) / 2};
  const Point p012{(p01.x + p12.x) / 2, (p01.y + p12.y) / 2}, p123{(p12.x + p23.x) / 2, (p12.y + p23.y) / 2};
  const Point mid{(p012.x + p123.x) / 2, (p012.y + p123.y) / 2};
  flatten_cubic(p0, p01, p012, mid, tolerance, out, depth + 1);
  flatten_cubic(mid, p123, p23, p3, tolerance, out, depth + 1);
}

/// Polylines of a path (curves flattened); `closed` per polyline.
struct Polyline {
  std::vector<Point> points;
  bool closed = false;
};

std::vector<Polyline> to_polylines(const Path& path, double tolerance) {
  std::vector<Polyline> out;
  std::size_t pi = 0;
  Polyline current;
  bool bad = false;
  bool implicit = false;  // current starts at the start point of a closed subpath
  const auto flush = [&](bool closed) {
    if (!current.points.empty() && !bad && !(implicit && current.points.size() == 1)) {
      current.closed = closed;
      out.push_back(std::move(current));
    }
    current = Polyline{};
    bad = false;
    implicit = false;
  };
  for (const PathOp op : path.ops) {
    switch (op) {
      case PathOp::Move: {
        flush(false);
        if (pi >= path.points.size()) return out;
        const Point p = path.points[pi++];
        if (!finite(p)) bad = true;
        current.points.push_back(p);
        break;
      }
      case PathOp::Line: {
        if (pi >= path.points.size()) return out;
        const Point p = path.points[pi++];
        if (!finite(p)) bad = true;
        if (current.points.empty()) current.points.push_back(p);
        current.points.push_back(p);
        break;
      }
      case PathOp::Cubic: {
        if (pi + 3 > path.points.size()) return out;
        const Point c1 = path.points[pi], c2 = path.points[pi + 1], p = path.points[pi + 2];
        pi += 3;
        if (!finite(c1) || !finite(c2) || !finite(p)) {
          bad = true;
          break;
        }
        if (current.points.empty()) current.points.push_back(c1);
        if (!bad) flatten_cubic(current.points.back(), c1, c2, p, tolerance, current.points, 0);
        break;
      }
      case PathOp::Close: {
        if (!current.points.empty()) {
          const Point start = current.points.front();
          flush(true);
          current.points.push_back(start);  // a following segment continues from the start point
          implicit = true;
        }
        break;
      }
    }
  }
  flush(false);
  // Drop single-point remnants created by Close.
  std::vector<Polyline> kept;
  for (Polyline& p : out) {
    if (!p.points.empty()) kept.push_back(std::move(p));
  }
  return kept;
}

struct Accumulator {
  int x0, y0, w, h;
  std::vector<float> acc;  // (w + 2) * h

  void line(Point a, Point b) {
    // Coordinates relative to the box.
    a.x -= x0;
    b.x -= x0;
    a.y -= y0;
    b.y -= y0;
    if (a.y == b.y) return;
    // Clip in y.
    const double top = 0, bottom = h;
    if ((a.y <= top && b.y <= top) || (a.y >= bottom && b.y >= bottom)) return;
    const auto at_y = [&](double y) { return Point{a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y), y}; };
    Point p = a, q = b;
    if (p.y < top) p = at_y(top);
    if (p.y > bottom) p = at_y(bottom);
    if (q.y < top) q = at_y(top);
    if (q.y > bottom) q = at_y(bottom);
    // Split at the left and right borders so clamping keeps the geometry.
    const double right = w;
    const auto at_x = [&](Point s, Point e, double x) {
      return Point{x, s.y + (e.y - s.y) * (x - s.x) / (e.x - s.x)};
    };
    Point pieces[4];
    int n = 0;
    pieces[n++] = p;
    std::vector<double> cuts;
    if ((p.x < 0) != (q.x < 0) && p.x != q.x) cuts.push_back(0);
    if ((p.x > right) != (q.x > right) && p.x != q.x) cuts.push_back(right);
    if (cuts.size() == 2 && ((p.x < q.x) != (cuts[0] < cuts[1]))) std::swap(cuts[0], cuts[1]);
    for (const double c : cuts) pieces[n++] = at_x(p, q, c);
    pieces[n++] = q;
    for (int i = 0; i + 1 < n; ++i) {
      Point s = pieces[i], e = pieces[i + 1];
      s.x = std::clamp(s.x, 0.0, right);
      e.x = std::clamp(e.x, 0.0, right);
      draw(s, e);
    }
  }

  void draw(Point p0, Point p1) {
    if (p0.y == p1.y) return;
    double dir = 1;
    if (p0.y > p1.y) {
      dir = -1;
      std::swap(p0, p1);
    }
    const double dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    double x = p0.x;
    const int ystart = std::max(0, static_cast<int>(std::floor(p0.y)));
    const int yend = std::min(h, static_cast<int>(std::ceil(p1.y)));
    if (p0.y < 0) x -= p0.y * dxdy;
    const std::size_t stride = static_cast<std::size_t>(w) + 2;
    for (int y = ystart; y < yend; ++y) {
      const double ylo = std::max(static_cast<double>(y), p0.y), yhi = std::min(static_cast<double>(y + 1), p1.y);
      const double dy = yhi - ylo;
      const double xnext = std::clamp(x + dxdy * dy, 0.0, static_cast<double>(w));
      const double d = dy * dir;
      const double xa = std::min(x, xnext), xb = std::max(x, xnext);
      float* row = acc.data() + static_cast<std::size_t>(y) * stride;
      const double xa_floor = std::floor(xa);
      const int xai = static_cast<int>(xa_floor);
      const double xb_ceil = std::ceil(xb);
      const int xbi = static_cast<int>(xb_ceil);
      if (xbi <= xai + 1) {
        const double xmf = 0.5 * (x + xnext) - xa_floor;
        row[xai] += static_cast<float>(d - d * xmf);
        row[xai + 1] += static_cast<float>(d * xmf);
      } else {
        const double s = 1.0 / (xb - xa);
        const double x0f = xa - xa_floor;
        const double a0 = 0.5 * s * (1 - x0f) * (1 - x0f);
        const double x1f = xb - xb_ceil + 1;
        const double am = 0.5 * s * x1f * x1f;
        row[xai] += static_cast<float>(d * a0);
        if (xbi == xai + 2) {
          row[xai + 1] += static_cast<float>(d * (1 - a0 - am));
        } else {
          const double a1 = s * (1.5 - x0f);
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

Mask fill_aliased(const std::vector<Polyline>& polys, FillRule rule, const IntRect& box) {
  struct Edge {
    double x0, y0, x1, y1;
    int dir;
  };
  std::vector<Edge> edges;
  for (const Polyline& pl : polys) {
    const std::size_t n = pl.points.size();
    for (std::size_t i = 0; i < n; ++i) {
      const Point a = pl.points[i], b = pl.points[(i + 1) % n];
      if (a.y == b.y) continue;
      if (a.y < b.y) edges.push_back({a.x, a.y, b.x, b.y, 1});
      else edges.push_back({b.x, b.y, a.x, a.y, -1});
    }
  }
  Mask mask;
  mask.box = box;
  mask.coverage.assign(static_cast<std::size_t>(box.width()) * static_cast<std::size_t>(box.height()), 0);
  std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
  std::vector<const Edge*> active;
  std::size_t next = 0;
  std::vector<std::pair<double, int>> xs;
  for (int y = box.y0; y < box.y1; ++y) {
    const double yc = y + 0.5;
    while (next < edges.size() && edges[next].y0 <= yc) active.push_back(&edges[next++]);
    std::erase_if(active, [&](const Edge* e) { return e->y1 <= yc; });
    xs.clear();
    for (const Edge* e : active) {
      if (e->y0 > yc) continue;
      xs.emplace_back(e->x0 + (e->x1 - e->x0) * (yc - e->y0) / (e->y1 - e->y0), e->dir);
    }
    std::sort(xs.begin(), xs.end());
    int winding = 0;
    std::uint8_t* row = mask.coverage.data() + static_cast<std::size_t>(y - box.y0) * static_cast<std::size_t>(box.width());
    for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
      winding += xs[i].second;
      const bool inside = rule == FillRule::NonZero ? winding != 0 : (winding & 1) != 0;
      if (!inside) continue;
      const double xa = std::clamp(std::ceil(xs[i].first - 0.5), static_cast<double>(box.x0), static_cast<double>(box.x1));
      const double xb = std::clamp(std::ceil(xs[i + 1].first - 0.5), static_cast<double>(box.x0), static_cast<double>(box.x1));
      for (int x = static_cast<int>(xa); x < static_cast<int>(xb); ++x) row[x - box.x0] = 255;
    }
  }
  return mask;
}

}  // namespace

Path flatten(const Path& path, double tolerance) {
  Path out;
  for (const Polyline& pl : to_polylines(path, std::max(tolerance, 1e-6))) {
    out.move_to(pl.points[0].x, pl.points[0].y);
    for (std::size_t i = 1; i < pl.points.size(); ++i) out.line_to(pl.points[i].x, pl.points[i].y);
    if (pl.closed) out.close();
  }
  return out;
}

Mask fill_path(const Path& path, FillRule rule, const IntRect& clip_box, bool antialias) {
  Mask mask;
  if (clip_box.empty() || path.empty()) return mask;
  const std::vector<Polyline> polys = to_polylines(path, 0.1);
  // Bounding box of the finite geometry, clipped.
  double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
  std::size_t points = 0;
  for (const Polyline& pl : polys) {
    for (const Point& p : pl.points) {
      minx = std::min(minx, p.x);
      miny = std::min(miny, p.y);
      maxx = std::max(maxx, p.x);
      maxy = std::max(maxy, p.y);
      ++points;
    }
  }
  if (points < 2) return mask;
  const auto to_int = [](double v, int lo, int hi) {
    return static_cast<int>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
  };
  IntRect box{to_int(std::floor(minx), clip_box.x0, clip_box.x1), to_int(std::floor(miny), clip_box.y0, clip_box.y1),
              to_int(std::ceil(maxx) + 1, clip_box.x0, clip_box.x1), to_int(std::ceil(maxy) + 1, clip_box.y0, clip_box.y1)};
  // Content left of the box still covers it: extend to the left clip border.
  if (minx < clip_box.x0) box.x0 = clip_box.x0;
  box = box.intersect(clip_box);
  if (box.empty()) return mask;
  if (!antialias) return fill_aliased(polys, rule, box);
  Accumulator acc{box.x0, box.y0, box.width(), box.height(), {}};
  acc.acc.assign((static_cast<std::size_t>(box.width()) + 2) * static_cast<std::size_t>(box.height()), 0.0f);
  for (const Polyline& pl : polys) {
    const std::size_t n = pl.points.size();
    for (std::size_t i = 0; i < n; ++i) acc.line(pl.points[i], pl.points[(i + 1) % n]);
  }
  mask.box = box;
  mask.coverage.resize(static_cast<std::size_t>(box.width()) * static_cast<std::size_t>(box.height()));
  const std::size_t stride = static_cast<std::size_t>(box.width()) + 2;
  for (int y = 0; y < box.height(); ++y) {
    const float* row = acc.acc.data() + static_cast<std::size_t>(y) * stride;
    std::uint8_t* out = mask.coverage.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(box.width());
    double sum = 0;
    for (int x = 0; x < box.width(); ++x) {
      sum += row[x];
      double c = std::abs(sum);
      if (rule == FillRule::NonZero) {
        c = std::min(c, 1.0);
      } else {
        c = std::fmod(c, 2.0);
        if (c > 1) c = 2 - c;
      }
      out[x] = static_cast<std::uint8_t>(c * 255.0 + 0.5);
    }
  }
  return mask;
}

Mask intersect(const Mask& a, const Mask& b) {
  if (a.everywhere) return b;
  if (b.everywhere) return a;
  Mask out;
  out.box = a.box.intersect(b.box);
  if (out.box.empty()) {
    out.box = IntRect{};
    return out;
  }
  out.coverage.resize(static_cast<std::size_t>(out.box.width()) * static_cast<std::size_t>(out.box.height()));
  for (int y = out.box.y0; y < out.box.y1; ++y) {
    const std::uint8_t* ra = a.row(y) + (out.box.x0 - a.box.x0);
    const std::uint8_t* rb = b.row(y) + (out.box.x0 - b.box.x0);
    std::uint8_t* ro = out.coverage.data() + static_cast<std::size_t>(y - out.box.y0) * static_cast<std::size_t>(out.box.width());
    for (int x = 0; x < out.box.width(); ++x) ro[x] = static_cast<std::uint8_t>((ra[x] * rb[x] + 127) / 255);
  }
  return out;
}

// --------------------------------------------------------------------------- stroking

namespace {

struct StrokeBuilder {
  Path out;
  Matrix ctm;
  double hw;
  double tolerance;  // user space

  void polygon(std::vector<Point> pts) {
    if (pts.size() < 3) return;
    double area = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
      const Point a = pts[i], b = pts[(i + 1) % pts.size()];
      area += a.x * b.y - b.x * a.y;
    }
    if (area < 0) std::reverse(pts.begin(), pts.end());
    if (area == 0) return;
    const Point first = ctm.apply(pts[0]);
    out.move_to(first.x, first.y);
    for (std::size_t i = 1; i < pts.size(); ++i) {
      const Point p = ctm.apply(pts[i]);
      out.line_to(p.x, p.y);
    }
    out.close();
  }
  void circle(Point c) {
    // Segment count from the radius so the error stays under the tolerance.
    const double ratio = std::clamp(tolerance / std::max(hw, 1e-9), 1e-6, 1.0);
    int n = static_cast<int>(std::ceil(std::numbers::pi / std::acos(1 - ratio)));
    n = std::clamp(n, 8, 256);
    std::vector<Point> pts;
    pts.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double a = 2 * std::numbers::pi * i / n;
      pts.push_back({c.x + hw * std::cos(a), c.y + hw * std::sin(a)});
    }
    polygon(std::move(pts));
  }
  void segment(Point a, Point b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len = std::hypot(dx, dy);
    if (len == 0) return;
    const double nx = -dy / len * hw, ny = dx / len * hw;
    polygon({{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}});
  }
  void join(Point prev, Point v, Point next, LineJoin kind, double miter_limit) {
    const double d1x = v.x - prev.x, d1y = v.y - prev.y, d2x = next.x - v.x, d2y = next.y - v.y;
    const double l1 = std::hypot(d1x, d1y), l2 = std::hypot(d2x, d2y);
    if (l1 == 0 || l2 == 0) return;
    const double cross = d1x * d2y - d1y * d2x;
    const double dot = d1x * d2x + d1y * d2y;
    if (std::abs(cross) < 1e-12 * l1 * l2 && dot > 0) return;  // straight
    if (kind == LineJoin::Round) {
      circle(v);
      return;
    }
    // Outer side normals.
    const double side = cross > 0 ? -1 : 1;
    const Point n1{-d1y / l1 * hw * side, d1x / l1 * hw * side};
    const Point n2{-d2y / l2 * hw * side, d2x / l2 * hw * side};
    const Point a{v.x + n1.x, v.y + n1.y}, b{v.x + n2.x, v.y + n2.y};
    if (kind == LineJoin::Miter) {
      const double cos_theta = std::clamp(-dot / (l1 * l2), -1.0, 1.0);  // angle between segments
      const double theta = std::acos(cos_theta);
      const double ratio = theta > 1e-9 ? 1.0 / std::sin(theta / 2) : 1e9;
      if (ratio <= miter_limit) {
        // Intersection of the two offset lines.
        const double denom = d1x * d2y - d1y * d2x;
        if (std::abs(denom) > 1e-12) {
          const double t = ((b.x - a.x) * d2y - (b.y - a.y) * d2x) / denom;
          const Point m{a.x + d1x * t, a.y + d1y * t};
          polygon({v, a, m, b});
          return;
        }
      }
    }
    polygon({v, a, b});
  }
  void cap(Point end, Point from, LineCap kind) {
    if (kind == LineCap::Butt) return;
    if (kind == LineCap::Round) {
      circle(end);
      return;
    }
    const double dx = end.x - from.x, dy = end.y - from.y;
    const double len = std::hypot(dx, dy);
    if (len == 0) return;
    const double ux = dx / len * hw, uy = dy / len * hw;
    const double nx = -uy, ny = ux;
    polygon({{end.x + nx, end.y + ny}, {end.x + nx + ux, end.y + ny + uy}, {end.x - nx + ux, end.y - ny + uy},
             {end.x - nx, end.y - ny}});
  }
  void dot(Point p, LineCap kind) {
    if (kind == LineCap::Round) circle(p);
    else if (kind == LineCap::Square) polygon({{p.x - hw, p.y - hw}, {p.x + hw, p.y - hw}, {p.x + hw, p.y + hw}, {p.x - hw, p.y + hw}});
  }
  void polyline(const std::vector<Point>& raw, bool closed, const StrokeStyle& style) {
    std::vector<Point> pts;
    for (const Point& p : raw) {
      if (pts.empty() || std::hypot(p.x - pts.back().x, p.y - pts.back().y) > 1e-9) pts.push_back(p);
    }
    if (closed && pts.size() > 2 && std::hypot(pts.front().x - pts.back().x, pts.front().y - pts.back().y) <= 1e-9) {
      pts.pop_back();
    }
    if (pts.size() == 1) {
      dot(pts[0], style.cap);
      return;
    }
    const std::size_t n = pts.size();
    const std::size_t segments = closed ? n : n - 1;
    for (std::size_t i = 0; i < segments; ++i) segment(pts[i], pts[(i + 1) % n]);
    for (std::size_t i = 1; i + 1 < n; ++i) join(pts[i - 1], pts[i], pts[i + 1], style.join, style.miter_limit);
    if (closed && n > 2) {
      join(pts[n - 2], pts[n - 1], pts[0], style.join, style.miter_limit);
      join(pts[n - 1], pts[0], pts[1], style.join, style.miter_limit);
    } else if (!closed) {
      cap(pts[0], pts[1], style.cap);
      cap(pts[n - 1], pts[n - 2], style.cap);
    }
  }
};

}  // namespace

Path stroke_outline(const Path& user_path, const StrokeStyle& style, const Matrix& ctm) {
  StrokeBuilder builder;
  builder.ctm = ctm;
  const double expansion = std::max(ctm.expansion(), 1e-12);
  const double min_width = 1.0 / expansion;  // one device pixel
  const double width = std::max(style.width, min_width);
  builder.hw = width / 2;
  builder.tolerance = 0.1 / std::max(ctm.max_expansion(), 1e-12);
  const std::vector<Polyline> polys = to_polylines(user_path, builder.tolerance);
  // Dashes.
  std::vector<double> dash;
  double total = 0;
  for (const double d : style.dash) {
    if (std::isfinite(d) && d >= 0) {
      dash.push_back(d);
      total += d;
    }
  }
  if (dash.size() % 2 == 1) {
    const std::vector<double> copy = dash;
    dash.insert(dash.end(), copy.begin(), copy.end());
    total *= 2;
  }
  // Very fine dash patterns would produce millions of pieces: treat as solid.
  const bool dashed = !dash.empty() && total > 0 && total * expansion > 0.5;
  std::size_t emitted = 0;
  for (const Polyline& pl : polys) {
    if (!dashed) {
      builder.polyline(pl.points, pl.closed, style);
      continue;
    }
    std::vector<Point> pts = pl.points;
    if (pl.closed && !pts.empty()) pts.push_back(pts.front());
    // Dash state at the start of each subpath.
    double phase = std::fmod(std::max(0.0, style.dash_phase), total);
    std::size_t index = 0;
    while (phase >= dash[index]) {
      phase -= dash[index];
      index = (index + 1) % dash.size();
    }
    double remaining = dash[index] - phase;
    bool on = index % 2 == 0;
    std::vector<Point> piece;
    if (on && !pts.empty()) piece.push_back(pts[0]);
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
      Point a = pts[i];
      const Point b = pts[i + 1];
      double seg = std::hypot(b.x - a.x, b.y - a.y);
      while (seg > remaining) {
        const double t = remaining / seg;
        const Point m{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        if (on) {
          piece.push_back(m);
          builder.polyline(piece, false, style);
          piece.clear();
          if (++emitted > 200000) return std::move(builder.out);
        } else {
          piece.push_back(m);
        }
        seg -= remaining;
        a = m;
        on = !on;
        index = (index + 1) % dash.size();
        remaining = dash[index];
        if (remaining == 0 && !on) {
          // zero-length gap: continue the dash
        }
      }
      remaining -= seg;
      if (on) piece.push_back(b);
    }
    if (on && piece.size() >= 1) builder.polyline(piece, false, style);
  }
  return std::move(builder.out);
}

}  // namespace cvt::pdf::raster
