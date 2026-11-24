// Convertible core - font family: glyph outlines and curve conversion.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/outline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace cvt::font {

void Bounds::add(Point p) noexcept {
  if (empty) {
    x_min = x_max = p.x;
    y_min = y_max = p.y;
    empty = false;
    return;
  }
  x_min = std::min(x_min, p.x);
  y_min = std::min(y_min, p.y);
  x_max = std::max(x_max, p.x);
  y_max = std::max(y_max, p.y);
}

void Path::move_to(Point p) {
  if (!verbs.empty() && verbs.back() != Verb::Close) close();
  verbs.push_back(Verb::Move);
  points.push_back(p);
}

void Path::line_to(Point p) {
  verbs.push_back(Verb::Line);
  points.push_back(p);
}

void Path::quad_to(Point c, Point p) {
  verbs.push_back(Verb::Quad);
  points.push_back(c);
  points.push_back(p);
}

void Path::cubic_to(Point c1, Point c2, Point p) {
  verbs.push_back(Verb::Cubic);
  points.push_back(c1);
  points.push_back(c2);
  points.push_back(p);
}

void Path::close() {
  if (!verbs.empty() && verbs.back() != Verb::Close) verbs.push_back(Verb::Close);
}

std::size_t Path::contour_count() const noexcept {
  return static_cast<std::size_t>(std::ranges::count(verbs, Verb::Move));
}

Bounds Path::control_bounds() const noexcept {
  Bounds bounds;
  for (const Point& p : points) bounds.add(p);
  return bounds;
}

namespace {

Point lerp(Point a, Point b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

Point cubic_at(Point p0, Point p1, Point p2, Point p3, double t) {
  const double u = 1 - t;
  const double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
  return {a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

Point quad_at(Point p0, Point p1, Point p2, double t) {
  const double u = 1 - t;
  return {u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y};
}

// Roots in (0, 1) of the derivative of a cubic/quadratic in one coordinate.
void cubic_extrema(double p0, double p1, double p2, double p3, std::vector<double>& ts) {
  const double a = -p0 + 3 * p1 - 3 * p2 + p3;
  const double b = 2 * (p0 - 2 * p1 + p2);
  const double c = p1 - p0;
  if (std::abs(a) < 1e-12) {
    if (std::abs(b) > 1e-12) ts.push_back(-c / b);
    return;
  }
  const double disc = b * b - 4 * a * c;
  if (disc < 0) return;
  const double root = std::sqrt(disc);
  ts.push_back((-b + root) / (2 * a));
  ts.push_back((-b - root) / (2 * a));
}

}  // namespace

Bounds Path::exact_bounds() const noexcept {
  Bounds bounds;
  std::size_t pi = 0;
  Point current{};
  for (const Verb verb : verbs) {
    switch (verb) {
      case Verb::Move:
      case Verb::Line:
        current = points[pi++];
        bounds.add(current);
        break;
      case Verb::Quad: {
        const Point c = points[pi], p = points[pi + 1];
        pi += 2;
        bounds.add(p);
        for (int axis = 0; axis < 2; ++axis) {
          const double a0 = axis ? current.y : current.x, a1 = axis ? c.y : c.x, a2 = axis ? p.y : p.x;
          const double d = a0 - 2 * a1 + a2;
          if (std::abs(d) > 1e-12) {
            const double t = (a0 - a1) / d;
            if (t > 0 && t < 1) bounds.add(quad_at(current, c, p, t));
          }
        }
        current = p;
        break;
      }
      case Verb::Cubic: {
        const Point c1 = points[pi], c2 = points[pi + 1], p = points[pi + 2];
        pi += 3;
        bounds.add(p);
        std::vector<double> ts;
        cubic_extrema(current.x, c1.x, c2.x, p.x, ts);
        cubic_extrema(current.y, c1.y, c2.y, p.y, ts);
        for (const double t : ts) {
          if (t > 0 && t < 1) bounds.add(cubic_at(current, c1, c2, p, t));
        }
        current = p;
        break;
      }
      case Verb::Close:
        break;
    }
  }
  return bounds;
}

void Path::transform(double xx, double xy, double yx, double yy, double dx, double dy) {
  for (Point& p : points) {
    const double x = p.x, y = p.y;
    p.x = xx * x + yx * y + dx;
    p.y = xy * x + yy * y + dy;
  }
}

void Path::append(const Path& other) {
  if (!verbs.empty()) close();
  verbs.insert(verbs.end(), other.verbs.begin(), other.verbs.end());
  points.insert(points.end(), other.points.begin(), other.points.end());
  close();
}

namespace {

struct Segment {
  Verb verb;
  Point pts[4];  // start, then controls and end
};

// Split a path into contours of segments (each contour implicitly closed).
std::vector<std::vector<Segment>> contours_of(const Path& path) {
  std::vector<std::vector<Segment>> contours;
  std::size_t pi = 0;
  Point start{}, current{};
  for (const Verb verb : path.verbs) {
    switch (verb) {
      case Verb::Move:
        contours.emplace_back();
        start = current = path.points[pi++];
        break;
      case Verb::Line:
        if (contours.empty()) contours.emplace_back();
        contours.back().push_back({Verb::Line, {current, path.points[pi], {}, {}}});
        current = path.points[pi++];
        break;
      case Verb::Quad:
        if (contours.empty()) contours.emplace_back();
        contours.back().push_back({Verb::Quad, {current, path.points[pi], path.points[pi + 1], {}}});
        current = path.points[pi + 1];
        pi += 2;
        break;
      case Verb::Cubic:
        if (contours.empty()) contours.emplace_back();
        contours.back().push_back({Verb::Cubic, {current, path.points[pi], path.points[pi + 1], path.points[pi + 2]}});
        current = path.points[pi + 2];
        pi += 3;
        break;
      case Verb::Close:
        if (!contours.empty() && !(current == start)) {
          contours.back().push_back({Verb::Line, {current, start, {}, {}}});
        }
        current = start;
        break;
    }
  }
  return contours;
}

}  // namespace

Path Path::reversed() const {
  Path out;
  for (const auto& contour : contours_of(*this)) {
    if (contour.empty()) continue;
    const Point start = contour.front().pts[0];
    out.move_to(start);
    for (auto it = contour.rbegin(); it != contour.rend(); ++it) {
      switch (it->verb) {
        case Verb::Line: out.line_to(it->pts[0]); break;
        case Verb::Quad: out.quad_to(it->pts[1], it->pts[0]); break;
        case Verb::Cubic: out.cubic_to(it->pts[2], it->pts[1], it->pts[0]); break;
        default: break;
      }
    }
    // The last emitted segment ends at `start`; drop a redundant closing line.
    if (out.verbs.back() == Verb::Line && out.points.back() == start) {
      out.verbs.pop_back();
      out.points.pop_back();
    }
    out.close();
  }
  return out;
}

Path Path::to_cubic() const {
  Path out;
  std::size_t pi = 0;
  Point current{};
  for (const Verb verb : verbs) {
    switch (verb) {
      case Verb::Move: current = points[pi++]; out.move_to(current); break;
      case Verb::Line: current = points[pi++]; out.line_to(current); break;
      case Verb::Quad: {
        const Point c = points[pi], p = points[pi + 1];
        pi += 2;
        out.cubic_to(lerp(current, c, 2.0 / 3.0), lerp(p, c, 2.0 / 3.0), p);
        current = p;
        break;
      }
      case Verb::Cubic:
        out.cubic_to(points[pi], points[pi + 1], points[pi + 2]);
        current = points[pi + 2];
        pi += 3;
        break;
      case Verb::Close: out.close(); break;
    }
  }
  return out;
}

std::size_t cubic_to_quadratics(Point p0, Point p1, Point p2, Point p3, double tolerance, std::vector<Point>& out) {
  constexpr int max_pieces = 32;
  for (int n = 1; n <= max_pieces; ++n) {
    std::vector<Point> candidate;
    bool good = true;
    for (int i = 0; i < n && good; ++i) {
      const double t0 = static_cast<double>(i) / n, t1 = static_cast<double>(i + 1) / n;
      // Sub-curve control points by evaluating the cubic's blossom.
      const auto blossom = [&](double a, double b, double c) {
        const Point ab0 = lerp(lerp(p0, p1, a), lerp(p1, p2, a), b);
        const Point ab1 = lerp(lerp(p1, p2, a), lerp(p2, p3, a), b);
        return lerp(ab0, ab1, c);
      };
      const Point q0 = blossom(t0, t0, t0), q1 = blossom(t0, t0, t1), q2 = blossom(t0, t1, t1),
                  q3 = blossom(t1, t1, t1);
      const Point c{(3 * (q1.x + q2.x) - (q0.x + q3.x)) / 4, (3 * (q1.y + q2.y) - (q0.y + q3.y)) / 4};
      for (int s = 1; s < 8; ++s) {
        const double t = s / 8.0;
        const Point a = cubic_at(q0, q1, q2, q3, t), b = quad_at(q0, c, q3, t);
        if (std::hypot(a.x - b.x, a.y - b.y) > tolerance) {
          good = false;
          break;
        }
      }
      candidate.push_back(c);
      candidate.push_back(q3);
    }
    if (good || n == max_pieces) {
      out.insert(out.end(), candidate.begin(), candidate.end());
      return static_cast<std::size_t>(n);
    }
  }
  return 0;
}

Path Path::to_quadratic(double tolerance) const {
  Path out;
  std::size_t pi = 0;
  Point current{};
  std::vector<Point> pieces;
  for (const Verb verb : verbs) {
    switch (verb) {
      case Verb::Move: current = points[pi++]; out.move_to(current); break;
      case Verb::Line: current = points[pi++]; out.line_to(current); break;
      case Verb::Quad:
        out.quad_to(points[pi], points[pi + 1]);
        current = points[pi + 1];
        pi += 2;
        break;
      case Verb::Cubic: {
        pieces.clear();
        cubic_to_quadratics(current, points[pi], points[pi + 1], points[pi + 2], tolerance, pieces);
        for (std::size_t i = 0; i + 1 < pieces.size(); i += 2) out.quad_to(pieces[i], pieces[i + 1]);
        current = points[pi + 2];
        pi += 3;
        break;
      }
      case Verb::Close: out.close(); break;
    }
  }
  return out;
}

namespace {

// Flatten with a chord error below `tolerance` font units.
std::vector<std::vector<Point>> flatten(const Path& path, double tolerance) {
  std::vector<std::vector<Point>> polylines;
  for (const auto& contour : contours_of(path)) {
    std::vector<Point> poly;
    for (const Segment& s : contour) {
      if (poly.empty()) poly.push_back(s.pts[0]);
      if (s.verb == Verb::Line) {
        poly.push_back(s.pts[1]);
      } else if (s.verb == Verb::Quad) {
        const double dd = std::hypot(s.pts[0].x - 2 * s.pts[1].x + s.pts[2].x, s.pts[0].y - 2 * s.pts[1].y + s.pts[2].y);
        const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(dd / (8 * tolerance)))), 1, 4096);
        for (int i = 1; i <= n; ++i) poly.push_back(quad_at(s.pts[0], s.pts[1], s.pts[2], double(i) / n));
      } else {
        const double dd = std::max(std::hypot(s.pts[0].x - 2 * s.pts[1].x + s.pts[2].x, s.pts[0].y - 2 * s.pts[1].y + s.pts[2].y),
                                   std::hypot(s.pts[1].x - 2 * s.pts[2].x + s.pts[3].x, s.pts[1].y - 2 * s.pts[2].y + s.pts[3].y));
        const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(3 * dd / (4 * tolerance)))), 1, 4096);
        for (int i = 1; i <= n; ++i) poly.push_back(cubic_at(s.pts[0], s.pts[1], s.pts[2], s.pts[3], double(i) / n));
      }
    }
    if (!poly.empty()) polylines.push_back(std::move(poly));
  }
  return polylines;
}

double point_segment(Point p, Point a, Point b) {
  const double dx = b.x - a.x, dy = b.y - a.y;
  const double len = dx * dx + dy * dy;
  double t = len > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len : 0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

// Largest distance from a point of `from` to the nearest segment of `to`,
// using a uniform grid over the target segments.
double directed(const std::vector<std::vector<Point>>& from, const std::vector<std::vector<Point>>& to) {
  struct Seg {
    Point a, b;
  };
  std::vector<Seg> segments;
  Bounds box;
  for (const auto& poly : to) {
    for (std::size_t i = 0; i < poly.size(); ++i) {
      segments.push_back({poly[i], poly[(i + 1) % poly.size()]});
      box.add(poly[i]);
    }
  }
  if (segments.empty()) return std::numeric_limits<double>::max();
  const double extent = std::max({box.x_max - box.x_min, box.y_max - box.y_min, 1.0});
  const double cell = std::max(extent / 64, 1.0);
  const auto columns = static_cast<std::int64_t>(std::floor((box.x_max - box.x_min) / cell)) + 1;
  const auto rows = static_cast<std::int64_t>(std::floor((box.y_max - box.y_min) / cell)) + 1;
  std::vector<std::vector<std::uint32_t>> grid(static_cast<std::size_t>(columns * rows));
  const auto cell_x = [&](double x) { return std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor((x - box.x_min) / cell)), 0, columns - 1); };
  const auto cell_y = [&](double y) { return std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor((y - box.y_min) / cell)), 0, rows - 1); };
  for (std::uint32_t i = 0; i < segments.size(); ++i) {
    const Seg& s = segments[i];
    for (std::int64_t y = cell_y(std::min(s.a.y, s.b.y)); y <= cell_y(std::max(s.a.y, s.b.y)); ++y) {
      for (std::int64_t x = cell_x(std::min(s.a.x, s.b.x)); x <= cell_x(std::max(s.a.x, s.b.x)); ++x) {
        grid[static_cast<std::size_t>(y * columns + x)].push_back(i);
      }
    }
  }
  double worst = 0;
  for (const auto& poly : from) {
    for (const Point& p : poly) {
      const std::int64_t px = static_cast<std::int64_t>(std::floor((p.x - box.x_min) / cell));
      const std::int64_t py = static_cast<std::int64_t>(std::floor((p.y - box.y_min) / cell));
      double best = std::numeric_limits<double>::max();
      for (std::int64_t ring = 0; ring <= columns + rows + 2; ++ring) {
        for (std::int64_t y = py - ring; y <= py + ring; ++y) {
          for (std::int64_t x = px - ring; x <= px + ring; ++x) {
            if (std::max(std::abs(x - px), std::abs(y - py)) != ring) continue;
            if (x < 0 || y < 0 || x >= columns || y >= rows) continue;
            for (const std::uint32_t i : grid[static_cast<std::size_t>(y * columns + x)]) {
              best = std::min(best, point_segment(p, segments[i].a, segments[i].b));
            }
          }
        }
        if (best <= static_cast<double>(ring) * cell) break;
      }
      worst = std::max(worst, best);
    }
  }
  return worst;
}

}  // namespace

double outline_distance(const Path& a, const Path& b) {
  const auto fa = flatten(a, 0.002), fb = flatten(b, 0.002);
  if (fa.empty() && fb.empty()) return 0;
  if (fa.empty() || fb.empty()) return std::numeric_limits<double>::max();
  return std::max(directed(fa, fb), directed(fb, fa));
}

}  // namespace cvt::font
