// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "gfx/path.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace cvd::gfx {

Path& Path::move_to(Vec2 p) {
  verbs_.push_back(Verb::Move);
  points_.push_back(p);
  start_ = current_ = p;
  open_ = true;
  return *this;
}

Path& Path::line_to(Vec2 p) {
  if (!open_) move_to(current_);
  verbs_.push_back(Verb::Line);
  points_.push_back(p);
  current_ = p;
  return *this;
}

Path& Path::quad_to(Vec2 c, Vec2 p) {
  if (!open_) move_to(current_);
  verbs_.push_back(Verb::Quad);
  points_.push_back(c);
  points_.push_back(p);
  current_ = p;
  return *this;
}

Path& Path::cubic_to(Vec2 c1, Vec2 c2, Vec2 p) {
  if (!open_) move_to(current_);
  verbs_.push_back(Verb::Cubic);
  points_.push_back(c1);
  points_.push_back(c2);
  points_.push_back(p);
  current_ = p;
  return *this;
}

Path& Path::close() {
  if (open_) {
    verbs_.push_back(Verb::Close);
    current_ = start_;
    open_ = false;
  }
  return *this;
}

Path& Path::arc_to(float rx, float ry, float rotation_deg, bool large_arc, bool sweep, Vec2 p) {
  // SVG implementation notes F.6.5: endpoint to centre parametrisation, then
  // approximate each <= 90 degree piece with a cubic.
  const Vec2 p0 = current_;
  if (p0 == p) return *this;
  rx = std::fabs(rx);
  ry = std::fabs(ry);
  if (rx < 1e-6f || ry < 1e-6f) return line_to(p);
  const float phi = rotation_deg * kPi / 180.0f;
  const float cphi = std::cos(phi);
  const float sphi = std::sin(phi);
  const float dx2 = (p0.x - p.x) * 0.5f;
  const float dy2 = (p0.y - p.y) * 0.5f;
  const float x1p = cphi * dx2 + sphi * dy2;
  const float y1p = -sphi * dx2 + cphi * dy2;
  const float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
  if (lambda > 1.0f) {
    const float s = std::sqrt(lambda);
    rx *= s;
    ry *= s;
  }
  const float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
  const float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
  float coef = den > 0.0f ? std::sqrt(std::max(0.0f, num / den)) : 0.0f;
  if (large_arc == sweep) coef = -coef;
  const float cxp = coef * (rx * y1p / ry);
  const float cyp = coef * -(ry * x1p / rx);
  const float cx = cphi * cxp - sphi * cyp + (p0.x + p.x) * 0.5f;
  const float cy = sphi * cxp + cphi * cyp + (p0.y + p.y) * 0.5f;
  const auto angle = [](float ux, float uy, float vx, float vy) {
    const float dot = ux * vx + uy * vy;
    const float len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    float a = std::acos(clampf(dot / len, -1.0f, 1.0f));
    if (ux * vy - uy * vx < 0.0f) a = -a;
    return a;
  };
  const float theta1 = angle(1.0f, 0.0f, (x1p - cxp) / rx, (y1p - cyp) / ry);
  float dtheta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
  if (!sweep && dtheta > 0.0f) dtheta -= 2.0f * kPi;
  if (sweep && dtheta < 0.0f) dtheta += 2.0f * kPi;
  const int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(dtheta) / (kPi * 0.5f) - 1e-4f)));
  const float delta = dtheta / static_cast<float>(segments);
  const float k = 4.0f / 3.0f * std::tan(delta / 4.0f);
  float t = theta1;
  const auto point_on = [&](float a) {
    const float ca = std::cos(a);
    const float sa = std::sin(a);
    return Vec2{cx + rx * ca * cphi - ry * sa * sphi, cy + rx * ca * sphi + ry * sa * cphi};
  };
  const auto deriv = [&](float a) {
    const float ca = std::cos(a);
    const float sa = std::sin(a);
    return Vec2{-rx * sa * cphi - ry * ca * sphi, -rx * sa * sphi + ry * ca * cphi};
  };
  for (int i = 0; i < segments; ++i) {
    const float t2 = t + delta;
    const Vec2 a0 = point_on(t);
    const Vec2 a1 = (i == segments - 1) ? p : point_on(t2);
    const Vec2 c1 = a0 + deriv(t) * k;
    const Vec2 c2 = a1 - deriv(t2) * k;
    cubic_to(c1, c2, a1);
    t = t2;
  }
  return *this;
}

Path& Path::add_rect(const Rect& r) {
  move_to(r.x, r.y);
  line_to(r.right(), r.y);
  line_to(r.right(), r.bottom());
  line_to(r.x, r.bottom());
  return close();
}

Path& Path::add_rrect(const Rect& r, float radius) {
  radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
  if (radius <= 0.0f) return add_rect(r);
  constexpr float k = 0.5522847498f;
  const float kr = radius * k;
  const float x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
  move_to(x0 + radius, y0);
  line_to(x1 - radius, y0);
  cubic_to({x1 - radius + kr, y0}, {x1, y0 + radius - kr}, {x1, y0 + radius});
  line_to(x1, y1 - radius);
  cubic_to({x1, y1 - radius + kr}, {x1 - radius + kr, y1}, {x1 - radius, y1});
  line_to(x0 + radius, y1);
  cubic_to({x0 + radius - kr, y1}, {x0, y1 - radius + kr}, {x0, y1 - radius});
  line_to(x0, y0 + radius);
  cubic_to({x0, y0 + radius - kr}, {x0 + radius - kr, y0}, {x0 + radius, y0});
  return close();
}

Path& Path::add_circle(Vec2 c, float radius) { return add_ellipse(c, radius, radius); }

Path& Path::add_ellipse(Vec2 c, float rx, float ry) {
  constexpr float k = 0.5522847498f;
  move_to(c.x + rx, c.y);
  cubic_to({c.x + rx, c.y + ry * k}, {c.x + rx * k, c.y + ry}, {c.x, c.y + ry});
  cubic_to({c.x - rx * k, c.y + ry}, {c.x - rx, c.y + ry * k}, {c.x - rx, c.y});
  cubic_to({c.x - rx, c.y - ry * k}, {c.x - rx * k, c.y - ry}, {c.x, c.y - ry});
  cubic_to({c.x + rx * k, c.y - ry}, {c.x + rx, c.y - ry * k}, {c.x + rx, c.y});
  return close();
}

std::optional<Path> Path::parse_svg(std::string_view d) {
  Path path;
  std::size_t i = 0;
  const auto skip = [&] {
    while (i < d.size() && (std::isspace(static_cast<unsigned char>(d[i])) || d[i] == ',')) ++i;
  };
  const auto number = [&]() -> std::optional<float> {
    skip();
    if (i >= d.size()) return std::nullopt;
    std::size_t start = i;
    if (d[i] == '+' || d[i] == '-') ++i;
    bool digits = false;
    bool dot = false;
    while (i < d.size()) {
      const char c = d[i];
      if (c >= '0' && c <= '9') {
        digits = true;
        ++i;
      } else if (c == '.' && !dot) {
        dot = true;
        ++i;
      } else {
        break;
      }
    }
    if (i < d.size() && (d[i] == 'e' || d[i] == 'E')) {
      ++i;
      if (i < d.size() && (d[i] == '+' || d[i] == '-')) ++i;
      while (i < d.size() && d[i] >= '0' && d[i] <= '9') ++i;
    }
    if (!digits) {
      i = start;
      return std::nullopt;
    }
    const std::string text(d.substr(start, i - start));
    return static_cast<float>(std::strtod(text.c_str(), nullptr));
  };
  const auto flag = [&]() -> std::optional<bool> {
    skip();
    if (i < d.size() && (d[i] == '0' || d[i] == '1')) return d[i++] == '1';
    return std::nullopt;
  };

  char cmd = 0;
  Vec2 cur{};
  Vec2 start{};
  Vec2 last_ctrl{};
  char last_cmd = 0;
  while (true) {
    skip();
    if (i >= d.size()) break;
    if (std::isalpha(static_cast<unsigned char>(d[i]))) {
      cmd = d[i++];
    } else if (cmd == 0) {
      return std::nullopt;
    }
    const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
    const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
    const Vec2 base = rel ? cur : Vec2{};
    switch (up) {
      case 'M': {
        auto x = number(), y = number();
        if (!x || !y) return std::nullopt;
        cur = base + Vec2{*x, *y};
        start = cur;
        path.move_to(cur);
        cmd = rel ? 'l' : 'L';  // subsequent pairs are line-tos
        break;
      }
      case 'L': {
        auto x = number(), y = number();
        if (!x || !y) return std::nullopt;
        cur = base + Vec2{*x, *y};
        path.line_to(cur);
        break;
      }
      case 'H': {
        auto x = number();
        if (!x) return std::nullopt;
        cur = Vec2{rel ? cur.x + *x : *x, cur.y};
        path.line_to(cur);
        break;
      }
      case 'V': {
        auto y = number();
        if (!y) return std::nullopt;
        cur = Vec2{cur.x, rel ? cur.y + *y : *y};
        path.line_to(cur);
        break;
      }
      case 'C': {
        auto x1 = number(), y1 = number(), x2 = number(), y2 = number(), x = number(), y = number();
        if (!x1 || !y1 || !x2 || !y2 || !x || !y) return std::nullopt;
        const Vec2 c1 = base + Vec2{*x1, *y1};
        const Vec2 c2 = base + Vec2{*x2, *y2};
        cur = base + Vec2{*x, *y};
        path.cubic_to(c1, c2, cur);
        last_ctrl = c2;
        break;
      }
      case 'S': {
        auto x2 = number(), y2 = number(), x = number(), y = number();
        if (!x2 || !y2 || !x || !y) return std::nullopt;
        const char lu = static_cast<char>(std::toupper(static_cast<unsigned char>(last_cmd)));
        const Vec2 c1 = (lu == 'C' || lu == 'S') ? cur * 2.0f - last_ctrl : cur;
        const Vec2 c2 = base + Vec2{*x2, *y2};
        cur = base + Vec2{*x, *y};
        path.cubic_to(c1, c2, cur);
        last_ctrl = c2;
        break;
      }
      case 'Q': {
        auto x1 = number(), y1 = number(), x = number(), y = number();
        if (!x1 || !y1 || !x || !y) return std::nullopt;
        const Vec2 c = base + Vec2{*x1, *y1};
        cur = base + Vec2{*x, *y};
        path.quad_to(c, cur);
        last_ctrl = c;
        break;
      }
      case 'T': {
        auto x = number(), y = number();
        if (!x || !y) return std::nullopt;
        const char lu = static_cast<char>(std::toupper(static_cast<unsigned char>(last_cmd)));
        const Vec2 c = (lu == 'Q' || lu == 'T') ? cur * 2.0f - last_ctrl : cur;
        cur = base + Vec2{*x, *y};
        path.quad_to(c, cur);
        last_ctrl = c;
        break;
      }
      case 'A': {
        auto rx = number(), ry = number(), rot = number();
        auto large = flag(), sweep = flag();
        auto x = number(), y = number();
        if (!rx || !ry || !rot || !large || !sweep || !x || !y) return std::nullopt;
        cur = base + Vec2{*x, *y};
        path.arc_to(*rx, *ry, *rot, *large, *sweep, cur);
        break;
      }
      case 'Z': {
        path.close();
        cur = start;
        break;
      }
      default: return std::nullopt;
    }
    last_cmd = up == 'M' ? 'M' : cmd;
    if (up == 'Z') {
      // A command letter must follow Z.
      skip();
      if (i < d.size() && !std::isalpha(static_cast<unsigned char>(d[i]))) return std::nullopt;
    }
  }
  return path;
}

namespace {

void flatten_quad(std::vector<Vec2>& out, Vec2 p0, Vec2 c, Vec2 p1, float tol) {
  const Vec2 dd = p0 - c * 2.0f + p1;
  const float dist = dd.length();
  int n = static_cast<int>(std::ceil(std::sqrt(dist / (8.0f * tol) * 4.0f)));
  n = std::clamp(n, 1, 256);
  for (int i = 1; i <= n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(n);
    const float mt = 1.0f - t;
    out.push_back(p0 * (mt * mt) + c * (2.0f * mt * t) + p1 * (t * t));
  }
}

void flatten_cubic(std::vector<Vec2>& out, Vec2 p0, Vec2 c1, Vec2 c2, Vec2 p1, float tol) {
  const Vec2 d1 = p0 - c1 * 2.0f + c2;
  const Vec2 d2 = c1 - c2 * 2.0f + p1;
  const float dd = std::max(d1.length(), d2.length());
  int n = static_cast<int>(std::ceil(std::sqrt(dd * 6.0f / (8.0f * tol))));
  n = std::clamp(n, 1, 512);
  for (int i = 1; i <= n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(n);
    const float mt = 1.0f - t;
    out.push_back(p0 * (mt * mt * mt) + c1 * (3.0f * mt * mt * t) + c2 * (3.0f * mt * t * t) + p1 * (t * t * t));
  }
}

}  // namespace

std::vector<Contour> Path::flatten(const Affine& t, float tolerance) const {
  std::vector<Contour> out;
  std::size_t pi = 0;
  Vec2 cur{};
  for (const Verb v : verbs_) {
    switch (v) {
      case Verb::Move:
        if (!out.empty() && out.back().points.size() < 2 && !out.back().closed) out.pop_back();
        out.push_back({});
        cur = t.apply(points_[pi++]);
        out.back().points.push_back(cur);
        break;
      case Verb::Line:
        cur = t.apply(points_[pi++]);
        out.back().points.push_back(cur);
        break;
      case Verb::Quad: {
        const Vec2 c = t.apply(points_[pi]);
        const Vec2 p = t.apply(points_[pi + 1]);
        pi += 2;
        flatten_quad(out.back().points, cur, c, p, tolerance);
        cur = p;
        break;
      }
      case Verb::Cubic: {
        const Vec2 c1 = t.apply(points_[pi]);
        const Vec2 c2 = t.apply(points_[pi + 1]);
        const Vec2 p = t.apply(points_[pi + 2]);
        pi += 3;
        flatten_cubic(out.back().points, cur, c1, c2, p, tolerance);
        cur = p;
        break;
      }
      case Verb::Close:
        if (!out.empty()) {
          out.back().closed = true;
          cur = out.back().points.front();
        }
        break;
    }
  }
  return out;
}

Rect Path::bounds() const {
  if (points_.empty()) return {};
  float x0 = points_[0].x, y0 = points_[0].y, x1 = x0, y1 = y0;
  for (const Vec2 p : points_) {
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
  }
  return Rect::from_points(x0, y0, x1, y1);
}

// ---------------------------------------------------------------------------
// Measuring
// ---------------------------------------------------------------------------

PolylineMeasure::PolylineMeasure(std::vector<Vec2> points) : pts_(std::move(points)) {
  cumulative_.reserve(pts_.size());
  float acc = 0.0f;
  for (std::size_t i = 0; i < pts_.size(); ++i) {
    if (i > 0) acc += (pts_[i] - pts_[i - 1]).length();
    cumulative_.push_back(acc);
  }
  total_ = acc;
}

Vec2 PolylineMeasure::point_at(float fraction) const {
  if (pts_.empty()) return {};
  const float target = clampf(fraction, 0.0f, 1.0f) * total_;
  for (std::size_t i = 1; i < pts_.size(); ++i) {
    if (cumulative_[i] >= target) {
      const float seg = cumulative_[i] - cumulative_[i - 1];
      const float u = seg > 0.0f ? (target - cumulative_[i - 1]) / seg : 0.0f;
      return pts_[i - 1] + (pts_[i] - pts_[i - 1]) * u;
    }
  }
  return pts_.back();
}

std::vector<Vec2> PolylineMeasure::trim(float start_fraction, float end_fraction) const {
  std::vector<Vec2> out;
  if (pts_.size() < 2) return out;
  const float s = clampf(start_fraction, 0.0f, 1.0f) * total_;
  const float e = clampf(end_fraction, 0.0f, 1.0f) * total_;
  if (e - s <= 1e-4f) return out;
  out.push_back(point_at(start_fraction));
  for (std::size_t i = 1; i < pts_.size(); ++i) {
    if (cumulative_[i] > s && cumulative_[i] < e) out.push_back(pts_[i]);
  }
  out.push_back(point_at(end_fraction));
  return out;
}

// ---------------------------------------------------------------------------
// Stroking
// ---------------------------------------------------------------------------

float signed_area(const std::vector<Vec2>& poly) {
  float a = 0.0f;
  for (std::size_t i = 0, n = poly.size(); i < n; ++i) {
    const Vec2 p = poly[i];
    const Vec2 q = poly[(i + 1) % n];
    a += p.x * q.y - q.x * p.y;
  }
  return a * 0.5f;
}

namespace {

void push_oriented(std::vector<Contour>& out, std::vector<Vec2> poly) {
  if (poly.size() < 3) return;
  if (signed_area(poly) < 0.0f) std::reverse(poly.begin(), poly.end());
  out.push_back({std::move(poly), true});
}

void add_round(std::vector<Contour>& out, Vec2 c, float r) {
  std::vector<Vec2> poly;
  const int n = std::clamp(static_cast<int>(r * 1.5f) + 8, 8, 96);
  for (int i = 0; i < n; ++i) {
    const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(n);
    poly.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
  }
  push_oriented(out, std::move(poly));
}

}  // namespace

std::vector<Contour> stroke_polyline(const Contour& line, const StrokeStyle& style) {
  std::vector<Contour> out;
  // Remove consecutive duplicates.
  std::vector<Vec2> pts;
  for (const Vec2 p : line.points) {
    if (pts.empty() || (p - pts.back()).length() > 1e-4f) pts.push_back(p);
  }
  if (line.closed && pts.size() > 2 && (pts.front() - pts.back()).length() < 1e-4f) pts.pop_back();
  const float hw = style.width * 0.5f;
  if (pts.size() < 2 || hw <= 0.0f) return out;
  const bool closed = line.closed && pts.size() > 2;
  const std::size_t n = pts.size();
  const std::size_t segs = closed ? n : n - 1;

  // Segment quads.
  for (std::size_t i = 0; i < segs; ++i) {
    const Vec2 a = pts[i];
    const Vec2 b = pts[(i + 1) % n];
    const Vec2 dir = (b - a).normalized();
    const Vec2 nrm = dir.perp() * hw;
    Vec2 a0 = a, b0 = b;
    if (!closed && style.cap == LineCap::Square) {
      if (i == 0) a0 = a - dir * hw;
      if (i == segs - 1) b0 = b + dir * hw;
    }
    push_oriented(out, {a0 + nrm, b0 + nrm, b0 - nrm, a0 - nrm});
  }

  // Joins.
  const std::size_t first_join = closed ? 0 : 1;
  const std::size_t last_join = closed ? n : n - 1;
  for (std::size_t j = first_join; j < last_join; ++j) {
    const Vec2 p = pts[j];
    const Vec2 prev = pts[(j + n - 1) % n];
    const Vec2 next = pts[(j + 1) % n];
    const Vec2 d0 = (p - prev).normalized();
    const Vec2 d1 = (next - p).normalized();
    const float turn = d0.cross(d1);
    if (std::fabs(turn) < 1e-6f && d0.dot(d1) > 0.0f) continue;  // straight
    if (style.join == LineJoin::Round) {
      add_round(out, p, hw);
      continue;
    }
    // Outer side: the side opposite to the turn direction.
    const float side = turn > 0.0f ? -1.0f : 1.0f;
    const Vec2 o0 = p + d0.perp() * (hw * side);
    const Vec2 o1 = p + d1.perp() * (hw * side);
    bool mitered = false;
    if (style.join == LineJoin::Miter) {
      const Vec2 bis = (d0.perp() + d1.perp()).normalized() * side;
      const float cos_half = bis.dot(d0.perp() * side);
      if (cos_half > 1e-4f) {
        const float miter_len = hw / cos_half;
        if (miter_len / hw <= style.miter_limit) {
          push_oriented(out, {p, o0, p + bis * miter_len, o1});
          mitered = true;
        }
      }
    }
    if (!mitered) push_oriented(out, {p, o0, o1});
  }

  if (!closed && style.cap == LineCap::Round) {
    add_round(out, pts.front(), hw);
    add_round(out, pts.back(), hw);
  }
  return out;
}

}  // namespace cvd::gfx
