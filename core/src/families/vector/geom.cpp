// Convertible core - vector family: path geometry.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/vector/geom.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cvt::vector {

namespace {

constexpr double pi = std::numbers::pi;

std::size_t points_of(Verb verb) {
  switch (verb) {
    case Verb::Move:
    case Verb::Line: return 1;
    case Verb::Quad: return 2;
    case Verb::Cubic: return 3;
    case Verb::Close: return 0;
  }
  return 0;
}

// Roots of a*t^2 + b*t + c in (0, 1).
int unit_roots(double a, double b, double c, double out[2]) {
  int n = 0;
  if (std::abs(a) < 1e-12) {
    if (std::abs(b) > 1e-12) {
      const double t = -c / b;
      if (t > 0 && t < 1) out[n++] = t;
    }
    return n;
  }
  const double disc = b * b - 4 * a * c;
  if (disc < 0) return 0;
  const double sq = std::sqrt(disc);
  const double t1 = (-b + sq) / (2 * a);
  const double t2 = (-b - sq) / (2 * a);
  if (t1 > 0 && t1 < 1) out[n++] = t1;
  if (t2 > 0 && t2 < 1) out[n++] = t2;
  return n;
}

double norm_angle_sweep(double sweep) { return sweep; }

}  // namespace

Point eval_quad(Point p0, Point p1, Point p2, double t) {
  const double u = 1 - t;
  return {u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y};
}

Point eval_cubic(Point p0, Point p1, Point p2, Point p3, double t) {
  const double u = 1 - t;
  const double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
  return {a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

int cubic_segments(Point p0, Point p1, Point p2, Point p3, double tolerance) {
  const Point d1 = p0 - p1 * 2 + p2;
  const Point d2 = p1 - p2 * 2 + p3;
  const double m = std::max(length(d1), length(d2));
  const double tol = std::max(tolerance, 1e-6);
  const double n = std::ceil(std::sqrt(0.75 * m / tol));
  if (!std::isfinite(n)) return 1;
  return static_cast<int>(std::clamp(n, 1.0, 2000.0));
}

int quad_segments(Point p0, Point p1, Point p2, double tolerance) {
  const double m = length(p0 - p1 * 2 + p2);
  const double tol = std::max(tolerance, 1e-6);
  const double n = std::ceil(std::sqrt(0.25 * m / tol));
  if (!std::isfinite(n)) return 1;
  return static_cast<int>(std::clamp(n, 1.0, 2000.0));
}

Path transformed(const Path& path, const Matrix& m) {
  Path out;
  out.verbs = path.verbs;
  out.points.reserve(path.points.size());
  for (const Point& p : path.points) out.points.push_back(m.apply(p));
  return out;
}

void append(Path& dst, const Path& src, const Matrix& m) {
  dst.verbs.insert(dst.verbs.end(), src.verbs.begin(), src.verbs.end());
  const bool identity = m.is_identity(0.0);
  for (const Point& p : src.points) dst.points.push_back(identity ? p : m.apply(p));
}

Point current_point(const Path& path) {
  if (path.points.empty()) return {};
  // After a Close the current point is the start of the closed subpath.
  if (!path.verbs.empty() && path.verbs.back() == Verb::Close) {
    std::size_t index = 0;
    std::size_t start = 0;
    for (const Verb verb : path.verbs) {
      if (verb == Verb::Move) start = index;
      index += points_of(verb);
    }
    return start < path.points.size() ? path.points[start] : path.points.back();
  }
  return path.points.back();
}

Rect control_bounds(const Path& path) {
  Rect r;
  for (const Point& p : path.points) r.add(p);
  return r;
}

Rect bounds(const Path& path) {
  Rect r;
  std::size_t i = 0;
  Point last{};
  for (const Verb verb : path.verbs) {
    const std::size_t n = points_of(verb);
    if (i + n > path.points.size()) break;
    switch (verb) {
      case Verb::Move:
      case Verb::Line:
        r.add(path.points[i]);
        last = path.points[i];
        break;
      case Verb::Quad: {
        const Point p0 = last, p1 = path.points[i], p2 = path.points[i + 1];
        r.add(p2);
        for (int axis = 0; axis < 2; ++axis) {
          const double a0 = axis ? p0.y : p0.x, a1 = axis ? p1.y : p1.x, a2 = axis ? p2.y : p2.x;
          const double den = a0 - 2 * a1 + a2;
          if (std::abs(den) > 1e-12) {
            const double t = (a0 - a1) / den;
            if (t > 0 && t < 1) r.add(eval_quad(p0, p1, p2, t));
          }
        }
        last = p2;
        break;
      }
      case Verb::Cubic: {
        const Point p0 = last, p1 = path.points[i], p2 = path.points[i + 1], p3 = path.points[i + 2];
        r.add(p3);
        for (int axis = 0; axis < 2; ++axis) {
          const double a0 = axis ? p0.y : p0.x, a1 = axis ? p1.y : p1.x, a2 = axis ? p2.y : p2.x,
                       a3 = axis ? p3.y : p3.x;
          const double a = -a0 + 3 * a1 - 3 * a2 + a3;
          const double b = 2 * (a0 - 2 * a1 + a2);
          const double c = a1 - a0;
          double roots[2];
          const int count = unit_roots(a, b, c, roots);
          for (int k = 0; k < count; ++k) r.add(eval_cubic(p0, p1, p2, p3, roots[k]));
        }
        last = p3;
        break;
      }
      case Verb::Close: break;
    }
    i += n;
  }
  return r;
}

Rect bounds(const Path& path, const Matrix& m) { return bounds(transformed(path, m)); }

Rect transform_rect(const Rect& rect, const Matrix& m) {
  if (rect.empty()) return {};
  Rect r;
  r.add(m.apply({rect.x0, rect.y0}));
  r.add(m.apply({rect.x1, rect.y0}));
  r.add(m.apply({rect.x0, rect.y1}));
  r.add(m.apply({rect.x1, rect.y1}));
  return r;
}

void add_rect(Path& path, double x, double y, double w, double h, double rx, double ry) {
  if (!(w > 0) || !(h > 0)) return;
  rx = std::clamp(rx, 0.0, w / 2);
  ry = std::clamp(ry, 0.0, h / 2);
  if (rx <= 0 || ry <= 0) {
    path.move_to({x, y});
    path.line_to({x + w, y});
    path.line_to({x + w, y + h});
    path.line_to({x, y + h});
    path.close();
    return;
  }
  constexpr double k = 0.5522847498307936;
  const double kx = rx * k, ky = ry * k;
  path.move_to({x + rx, y});
  path.line_to({x + w - rx, y});
  path.cubic_to({x + w - rx + kx, y}, {x + w, y + ry - ky}, {x + w, y + ry});
  path.line_to({x + w, y + h - ry});
  path.cubic_to({x + w, y + h - ry + ky}, {x + w - rx + kx, y + h}, {x + w - rx, y + h});
  path.line_to({x + rx, y + h});
  path.cubic_to({x + rx - kx, y + h}, {x, y + h - ry + ky}, {x, y + h - ry});
  path.line_to({x, y + ry});
  path.cubic_to({x, y + ry - ky}, {x + rx - kx, y}, {x + rx, y});
  path.close();
}

void add_ellipse(Path& path, double cx, double cy, double rx, double ry) {
  if (!(rx > 0) || !(ry > 0)) return;
  constexpr double k = 0.5522847498307936;
  const double kx = rx * k, ky = ry * k;
  path.move_to({cx + rx, cy});
  path.cubic_to({cx + rx, cy + ky}, {cx + kx, cy + ry}, {cx, cy + ry});
  path.cubic_to({cx - kx, cy + ry}, {cx - rx, cy + ky}, {cx - rx, cy});
  path.cubic_to({cx - rx, cy - ky}, {cx - kx, cy - ry}, {cx, cy - ry});
  path.cubic_to({cx + kx, cy - ry}, {cx + rx, cy - ky}, {cx + rx, cy});
  path.close();
}

namespace {

// Cubic segments of the unit-circle arc from angle a0 sweeping `sweep`, mapped through `m`.
void append_unit_arc(Path& path, const Matrix& m, double a0, double sweep) {
  const int segments = std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / (pi / 2) - 1e-9)), 1, 64);
  const double step = sweep / segments;
  const double k = 4.0 / 3.0 * std::tan(step / 4);
  double a = a0;
  for (int s = 0; s < segments; ++s) {
    const double b = a + step;
    const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    const Point c1{ca - k * sa, sa + k * ca};
    const Point c2{cb + k * sb, sb - k * cb};
    const Point e{cb, sb};
    path.cubic_to(m.apply(c1), m.apply(c2), m.apply(e));
    a = b;
  }
}

}  // namespace

void svg_arc_to(Path& path, Point from, double rx, double ry, double x_axis_rotation_degrees, bool large_arc,
                bool sweep, Point to) {
  if (from == to) return;
  rx = std::abs(rx);
  ry = std::abs(ry);
  if (!(rx > 0) || !(ry > 0) || !std::isfinite(rx) || !std::isfinite(ry)) {
    path.line_to(to);
    return;
  }
  const double phi = x_axis_rotation_degrees * pi / 180.0;
  const double cphi = std::cos(phi), sphi = std::sin(phi);
  const double dx2 = (from.x - to.x) / 2, dy2 = (from.y - to.y) / 2;
  const double x1p = cphi * dx2 + sphi * dy2;
  const double y1p = -sphi * dx2 + cphi * dy2;
  const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
  if (lambda > 1) {
    const double s = std::sqrt(lambda);
    rx *= s;
    ry *= s;
  }
  const double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
  const double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
  double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0.0;
  if (large_arc == sweep) coef = -coef;
  const double cxp = coef * rx * y1p / ry;
  const double cyp = -coef * ry * x1p / rx;
  const double cx = cphi * cxp - sphi * cyp + (from.x + to.x) / 2;
  const double cy = sphi * cxp + cphi * cyp + (from.y + to.y) / 2;
  const auto angle = [](double ux, double uy, double vx, double vy) {
    return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
  };
  const double theta1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
  double dtheta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
  if (!sweep && dtheta > 0) dtheta -= 2 * pi;
  if (sweep && dtheta < 0) dtheta += 2 * pi;
  const Matrix m{rx * cphi, rx * sphi, -ry * sphi, ry * cphi, cx, cy};
  const std::size_t before = path.points.size();
  append_unit_arc(path, m, theta1, dtheta);
  if (path.points.size() > before) path.points.back() = to;  // exact end point
}

void add_arc(Path& path, Point center, double rx, double ry, double rotation, double start, double end, bool positive,
             bool connect) {
  double sweep = end - start;
  if (positive) {
    while (sweep < 0) sweep += 2 * pi;
    if (sweep > 2 * pi) sweep = std::fmod(sweep, 2 * pi);
  } else {
    while (sweep > 0) sweep -= 2 * pi;
    if (sweep < -2 * pi) sweep = std::fmod(sweep, 2 * pi);
  }
  sweep = norm_angle_sweep(sweep);
  const double cr = std::cos(rotation), sr = std::sin(rotation);
  const Matrix m{rx * cr, rx * sr, -ry * sr, ry * cr, center.x, center.y};
  const Point first = m.apply({std::cos(start), std::sin(start)});
  if (connect && !path.empty() && path.verbs.back() != Verb::Close) {
    path.line_to(first);
  } else {
    path.move_to(first);
  }
  if (std::abs(sweep) < 1e-12) return;
  append_unit_arc(path, m, start, sweep);
}

std::vector<Polyline> flatten(const Path& path, double tolerance) {
  std::vector<Polyline> out;
  Polyline current;
  Point start{};
  Point last{};
  bool open = false;
  const auto finish = [&](bool closed) {
    if (open && !current.points.empty()) {
      current.closed = closed;
      out.push_back(std::move(current));
    }
    current = Polyline{};
    open = false;
  };
  const auto ensure_open = [&] {
    if (!open) {
      current.points.push_back(last);
      current.corners.push_back(true);
      start = last;
      open = true;
    }
  };
  std::size_t i = 0;
  for (const Verb verb : path.verbs) {
    const std::size_t n = points_of(verb);
    if (i + n > path.points.size()) break;
    switch (verb) {
      case Verb::Move:
        finish(false);
        last = path.points[i];
        start = last;
        current.points.push_back(last);
        current.corners.push_back(true);
        open = true;
        break;
      case Verb::Line:
        ensure_open();
        last = path.points[i];
        current.points.push_back(last);
        current.corners.push_back(true);
        break;
      case Verb::Quad: {
        ensure_open();
        const Point p1 = path.points[i], p2 = path.points[i + 1];
        const int segs = quad_segments(last, p1, p2, tolerance);
        for (int s = 1; s <= segs; ++s) {
          current.points.push_back(s == segs ? p2 : eval_quad(last, p1, p2, static_cast<double>(s) / segs));
          current.corners.push_back(s == segs);
        }
        last = p2;
        break;
      }
      case Verb::Cubic: {
        ensure_open();
        const Point p1 = path.points[i], p2 = path.points[i + 1], p3 = path.points[i + 2];
        const int segs = cubic_segments(last, p1, p2, p3, tolerance);
        for (int s = 1; s <= segs; ++s) {
          current.points.push_back(s == segs ? p3 : eval_cubic(last, p1, p2, p3, static_cast<double>(s) / segs));
          current.corners.push_back(s == segs);
        }
        last = p3;
        break;
      }
      case Verb::Close:
        if (open) {
          finish(true);
          last = start;
        }
        break;
    }
    i += n;
  }
  finish(false);
  return out;
}

std::vector<Polyline> dash(const std::vector<Polyline>& lines, std::span<const double> pattern, double offset) {
  double total = 0;
  for (const double d : pattern) total += std::max(0.0, d);
  if (pattern.empty() || !(total > 0) || !std::isfinite(total)) return lines;
  std::vector<Polyline> out;
  for (const Polyline& line : lines) {
    std::vector<Point> pts = line.points;
    std::vector<bool> corners = line.corners;
    if (line.closed && !pts.empty()) {
      pts.push_back(pts.front());
      corners.push_back(true);
    }
    if (pts.size() < 2) continue;
    // Starting dash index and remaining length from the offset.
    double phase = std::fmod(offset, total);
    if (phase < 0) phase += total;
    std::size_t index = 0;
    while (phase >= std::max(0.0, pattern[index])) {
      phase -= std::max(0.0, pattern[index]);
      index = (index + 1) % pattern.size();
    }
    double remaining = std::max(0.0, pattern[index]) - phase;
    bool on = (index % 2) == 0;
    const std::size_t first_out = out.size();
    const bool started_on = on && phase <= 0.0;
    Polyline dash_line;
    if (on) {
      dash_line.points.push_back(pts[0]);
      dash_line.corners.push_back(true);
    }
    std::size_t guard = 0;
    for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
      Point a = pts[k];
      const Point b = pts[k + 1];
      double seg = length(b - a);
      while (seg > remaining) {
        if (++guard > 10'000'000) return out;  // absurd dash patterns
        const Point p = lerp(a, b, remaining / seg);
        seg -= remaining;
        a = p;
        if (on) {
          dash_line.points.push_back(p);
          dash_line.corners.push_back(true);
          out.push_back(std::move(dash_line));
          dash_line = Polyline{};
        } else {
          dash_line.points.push_back(p);
          dash_line.corners.push_back(true);
        }
        on = !on;
        index = (index + 1) % pattern.size();
        remaining = std::max(0.0, pattern[index]);
      }
      remaining -= seg;
      if (on) {
        dash_line.points.push_back(b);
        dash_line.corners.push_back(corners[k + 1]);
      }
    }
    if (on && dash_line.points.size() >= 2) {
      // A closed outline that starts and ends inside a dash joins them.
      if (line.closed && started_on && out.size() > first_out) {
        Polyline& head = out[first_out];
        Polyline merged = std::move(dash_line);
        merged.points.insert(merged.points.end(), head.points.begin() + 1, head.points.end());
        merged.corners.insert(merged.corners.end(), head.corners.begin() + 1, head.corners.end());
        head = std::move(merged);
      } else {
        out.push_back(std::move(dash_line));
      }
    }
  }
  return out;
}

namespace {

// Points of an arc around `c` with radius r from direction angle a0 sweeping
// `sweep` radians (excluding the first point, including the last).
void arc_points(std::vector<Point>& out, Point c, double r, double a0, double sweep, double tolerance) {
  double step = 2 * std::acos(std::clamp(1 - tolerance / std::max(r, 1e-9), -1.0, 1.0));
  if (!(step > 1e-3)) step = 1e-3;
  const int n = std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / step)), 1, 1024);
  for (int i = 1; i <= n; ++i) {
    const double a = a0 + sweep * i / n;
    out.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
  }
}

Point normal_of(Point d) { return {-d.y, d.x}; }

struct StrokeBuilder {
  const Stroke& stroke;
  double hw;
  double tolerance;
  Path out;

  void emit(const std::vector<Point>& poly) {
    if (poly.size() < 2) return;
    out.move_to(poly[0]);
    for (std::size_t i = 1; i < poly.size(); ++i) out.line_to(poly[i]);
    out.close();
  }

  // Join at vertex p between direction dp (incoming) and dn (outgoing).
  void join(std::vector<Point>& left, std::vector<Point>& right, Point p, Point dp, Point dn, bool corner) {
    const Point np = normal_of(dp), nn = normal_of(dn);
    const double cr = cross(dp, dn);
    const double dt = dot(dp, dn);
    if (std::abs(cr) < 1e-9 && dt > 0) {
      left.push_back(p + nn * hw);
      right.push_back(p - nn * hw);
      return;
    }
    // cr > 0: the left side is the inside of the turn.
    const double s = cr > 0 ? -1.0 : 1.0;  // sign of the outer side's normal
    std::vector<Point>& outer = s > 0 ? left : right;
    std::vector<Point>& inner = s > 0 ? right : left;
    inner.push_back(p - np * (s * hw));
    inner.push_back(p);
    inner.push_back(p - nn * (s * hw));

    const Point a = p + np * (s * hw);
    const Point b = p + nn * (s * hw);
    LineJoin type = stroke.join;
    if (!corner) type = (dt > 0.985) ? LineJoin::Miter : LineJoin::Round;
    outer.push_back(a);
    switch (type) {
      case LineJoin::Round: {
        const double a0 = std::atan2(a.y - p.y, a.x - p.x);
        double sweep = std::atan2(b.y - p.y, b.x - p.x) - a0;
        while (sweep > pi) sweep -= 2 * pi;
        while (sweep < -pi) sweep += 2 * pi;
        arc_points(outer, p, hw, a0, sweep, tolerance);
        return;
      }
      case LineJoin::Bevel: break;
      case LineJoin::Miter:
      case LineJoin::MiterClip:
      case LineJoin::Arcs: {
        const double half = std::sqrt(std::max(0.0, (1 + dt) / 2));  // sin(theta / 2)
        Point m = np + nn;
        const double ml = length(m);
        if (ml < 1e-12) break;
        m = m * (s / ml);
        const double limit = corner ? stroke.miter_limit : 1e9;
        if (half > 1e-12 && 1.0 / half <= limit) {
          outer.push_back(p + m * (hw / half));
        } else if (type != LineJoin::Miter) {
          // Clip the miter at miter_limit * hw from the vertex.
          const double clip = limit * hw;
          const double da = dot(dp, m);
          const double db = dot(dn, m);
          if (std::abs(da) > 1e-12 && std::abs(db) > 1e-12) {
            const double ta = (clip - dot(a - p, m)) / da;
            const double tb = (clip - dot(b - p, m)) / db;
            outer.push_back(a + dp * ta);
            outer.push_back(b - dn * (-tb));
          }
        }
        break;
      }
    }
    outer.push_back(b);
  }

  void cap(std::vector<Point>& poly, Point p, Point d, bool at_end) {
    // Called with poly ending at p + n*hw (end) or p - n*hw (start); appends the cap
    // so that it ends at the opposite side.
    const Point n = normal_of(d);
    const Point dir = at_end ? d : d * -1.0;
    const Point from = at_end ? p + n * hw : p - n * hw;
    const Point to = at_end ? p - n * hw : p + n * hw;
    switch (stroke.cap) {
      case LineCap::Butt: poly.push_back(to); break;
      case LineCap::Square:
        poly.push_back(from + dir * hw);
        poly.push_back(to + dir * hw);
        poly.push_back(to);
        break;
      case LineCap::Round: {
        const double a0 = std::atan2(from.y - p.y, from.x - p.x);
        // Sweep pi in the direction that passes through `dir`.
        const double mid = std::atan2(dir.y, dir.x);
        double diff = mid - a0;
        while (diff > pi) diff -= 2 * pi;
        while (diff < -pi) diff += 2 * pi;
        arc_points(poly, p, hw, a0, diff > 0 ? pi : -pi, tolerance);
        break;
      }
    }
  }

  void dot_cap(Point p) {
    std::vector<Point> poly;
    if (stroke.cap == LineCap::Round) {
      poly.push_back({p.x + hw, p.y});
      arc_points(poly, p, hw, 0, 2 * pi, tolerance);
    } else if (stroke.cap == LineCap::Square) {
      poly = {{p.x - hw, p.y - hw}, {p.x + hw, p.y - hw}, {p.x + hw, p.y + hw}, {p.x - hw, p.y + hw}};
    }
    emit(poly);
  }

  void line(const Polyline& line) {
    std::vector<Point> pts;
    std::vector<bool> corners;
    for (std::size_t i = 0; i < line.points.size(); ++i) {
      const Point p = line.points[i];
      if (!std::isfinite(p.x) || !std::isfinite(p.y)) continue;
      const bool c = i < line.corners.size() ? line.corners[i] : true;
      if (!pts.empty() && length(p - pts.back()) < 1e-9) {
        corners.back() = corners.back() || c;
        continue;
      }
      pts.push_back(p);
      corners.push_back(c);
    }
    bool closed = line.closed;
    if (closed && pts.size() > 1 && length(pts.front() - pts.back()) < 1e-9) {
      pts.pop_back();
      corners.pop_back();
    }
    if (pts.empty()) return;
    if (pts.size() == 1) {
      dot_cap(pts[0]);
      return;
    }
    const std::size_t n = pts.size();
    if (closed && n == 2) closed = false;
    const std::size_t segs = closed ? n : n - 1;
    std::vector<Point> dirs(segs);
    for (std::size_t i = 0; i < segs; ++i) {
      const Point d = pts[(i + 1) % n] - pts[i];
      dirs[i] = d * (1.0 / length(d));
    }
    std::vector<Point> left, right;
    if (!closed) {
      left.push_back(pts[0] + normal_of(dirs[0]) * hw);
      right.push_back(pts[0] - normal_of(dirs[0]) * hw);
      for (std::size_t j = 1; j + 1 < n; ++j) join(left, right, pts[j], dirs[j - 1], dirs[j], corners[j]);
      left.push_back(pts[n - 1] + normal_of(dirs[segs - 1]) * hw);
      right.push_back(pts[n - 1] - normal_of(dirs[segs - 1]) * hw);
      std::vector<Point> poly = std::move(left);
      cap(poly, pts[n - 1], dirs[segs - 1], true);
      for (std::size_t k = right.size(); k-- > 1;) poly.push_back(right[k - 1]);
      cap(poly, pts[0], dirs[0], false);
      emit(poly);
    } else {
      for (std::size_t j = 0; j < n; ++j) join(left, right, pts[j], dirs[(j + segs - 1) % segs], dirs[j], corners[j]);
      emit(left);
      std::reverse(right.begin(), right.end());
      emit(right);
    }
  }
};

}  // namespace

Path stroke_polylines(const std::vector<Polyline>& lines, const Stroke& stroke, double tolerance) {
  StrokeBuilder builder{stroke, stroke.width / 2, std::max(tolerance, 1e-4), {}};
  if (!(builder.hw > 0) || !std::isfinite(builder.hw)) return {};
  for (const Polyline& line : lines) builder.line(line);
  return std::move(builder.out);
}

Path stroke_outline(const Path& path, const Stroke& stroke, double tolerance) {
  std::vector<Polyline> lines = flatten(path, tolerance);
  if (!stroke.dash.empty()) lines = dash(lines, stroke.dash, stroke.dash_offset);
  return stroke_polylines(lines, stroke, tolerance);
}

double path_length(const Path& path, double tolerance) {
  double total = 0;
  for (const Polyline& line : flatten(path, tolerance)) {
    for (std::size_t i = 1; i < line.points.size(); ++i) total += length(line.points[i] - line.points[i - 1]);
    if (line.closed && line.points.size() > 1) total += length(line.points.front() - line.points.back());
  }
  return total;
}

double signed_area(std::span<const Point> polygon) {
  double area = 0;
  for (std::size_t i = 0; i < polygon.size(); ++i) {
    const Point a = polygon[i], b = polygon[(i + 1) % polygon.size()];
    area += a.x * b.y - b.x * a.y;
  }
  return area / 2;
}

}  // namespace cvt::vector
