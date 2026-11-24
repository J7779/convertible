// Convertible core - vector family: CAD formats, shared helpers (ACI palette,
// number output, bounds, arbitrary axis algorithm, bulges, NURBS curves).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <variant>

#include "families/vector/cad_internal.hpp"
#include "families/vector/geom.hpp"
#include "util/text.hpp"

namespace cvt::vector::cad {

namespace {

// The AutoCAD Colour Index: the standard 256-entry palette (0 and 256 are
// BYBLOCK / BYLAYER and never looked up as colours).
constexpr std::array<std::uint32_t, 256> aci_palette = {
    0x000000, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0xFF00FF, 0xFFFFFF,
    0x808080, 0xC0C0C0, 0xFF0000, 0xFF7F7F, 0xA50000, 0xA55252, 0x7F0000, 0x7F3F3F,
    0x4C0000, 0x4C2626, 0x260000, 0x261313, 0xFF3F00, 0xFF9F7F, 0xA52900, 0xA56752,
    0x7F1F00, 0x7F4F3F, 0x4C1300, 0x4C2F26, 0x260900, 0x261713, 0xFF7F00, 0xFFBF7F,
    0xA55200, 0xA57C52, 0x7F3F00, 0x7F5F3F, 0x4C2600, 0x4C3926, 0x261300, 0x261C13,
    0xFFBF00, 0xFFDF7F, 0xA57C00, 0xA59152, 0x7F5F00, 0x7F6F3F, 0x4C3900, 0x4C4226,
    0x261C00, 0x262113, 0xFFFF00, 0xFFFF7F, 0xA5A500, 0xA5A552, 0x7F7F00, 0x7F7F3F,
    0x4C4C00, 0x4C4C26, 0x262600, 0x262613, 0xBFFF00, 0xDFFF7F, 0x7CA500, 0x91A552,
    0x5F7F00, 0x6F7F3F, 0x394C00, 0x424C26, 0x1C2600, 0x212613, 0x7FFF00, 0xBFFF7F,
    0x52A500, 0x7CA552, 0x3F7F00, 0x5F7F3F, 0x264C00, 0x394C26, 0x132600, 0x1C2613,
    0x3FFF00, 0x9FFF7F, 0x29A500, 0x67A552, 0x1F7F00, 0x4F7F3F, 0x134C00, 0x2F4C26,
    0x092600, 0x172613, 0x00FF00, 0x7FFF7F, 0x00A500, 0x52A552, 0x007F00, 0x3F7F3F,
    0x004C00, 0x264C26, 0x002600, 0x132613, 0x00FF3F, 0x7FFF9F, 0x00A529, 0x52A567,
    0x007F1F, 0x3F7F4F, 0x004C13, 0x264C2F, 0x002609, 0x135817, 0x00FF7F, 0x7FFFBF,
    0x00A552, 0x52A57C, 0x007F3F, 0x3F7F5F, 0x004C26, 0x264C39, 0x002613, 0x13581C,
    0x00FFBF, 0x7FFFDF, 0x00A57C, 0x52A591, 0x007F5F, 0x3F7F6F, 0x004C39, 0x264C42,
    0x00261C, 0x135858, 0x00FFFF, 0x7FFFFF, 0x00A5A5, 0x52A5A5, 0x007F7F, 0x3F7F7F,
    0x004C4C, 0x264C4C, 0x002626, 0x135858, 0x00BFFF, 0x7FDFFF, 0x007CA5, 0x5291A5,
    0x005F7F, 0x3F6F7F, 0x00394C, 0x26427E, 0x001C26, 0x135858, 0x007FFF, 0x7FBFFF,
    0x0052A5, 0x527CA5, 0x003F7F, 0x3F5F7F, 0x00264C, 0x26397E, 0x001326, 0x131C58,
    0x003FFF, 0x7F9FFF, 0x0029A5, 0x5267A5, 0x001F7F, 0x3F4F7F, 0x00134C, 0x262F7E,
    0x000926, 0x131758, 0x0000FF, 0x7F7FFF, 0x0000A5, 0x5252A5, 0x00007F, 0x3F3F7F,
    0x00004C, 0x26267E, 0x000026, 0x131358, 0x3F00FF, 0x9F7FFF, 0x2900A5, 0x6752A5,
    0x1F007F, 0x4F3F7F, 0x13004C, 0x2F267E, 0x090026, 0x171358, 0x7F00FF, 0xBF7FFF,
    0x5200A5, 0x7C52A5, 0x3F007F, 0x5F3F7F, 0x26004C, 0x39267E, 0x130026, 0x1C1358,
    0xBF00FF, 0xDF7FFF, 0x7C00A5, 0x9152A5, 0x5F007F, 0x6F3F7F, 0x39004C, 0x42264C,
    0x1C0026, 0x581358, 0xFF00FF, 0xFF7FFF, 0xA500A5, 0xA552A5, 0x7F007F, 0x7F3F7F,
    0x4C004C, 0x4C264C, 0x260026, 0x581358, 0xFF00BF, 0xFF7FDF, 0xA5007C, 0xA55291,
    0x7F005F, 0x7F3F6F, 0x4C0039, 0x4C2642, 0x26001C, 0x581358, 0xFF007F, 0xFF7FBF,
    0xA50052, 0xA5527C, 0x7F003F, 0x7F3F5F, 0x4C0026, 0x4C2639, 0x260013, 0x58131C,
    0xFF003F, 0xFF7F9F, 0xA50029, 0xA55267, 0x7F001F, 0x7F3F4F, 0x4C0013, 0x4C262F,
    0x260009, 0x581317, 0x000000, 0x656565, 0x666666, 0x999999, 0xCCCCCC, 0xFFFFFF,
};

void add_bounds(const Group& group, const Matrix& parent, Rect& out, int depth) {
  if (depth > 512) return;
  const Matrix m = parent * group.transform;
  for (const Node& node : group.children) {
    if (const auto* shape = std::get_if<Shape>(&node.value)) {
      Rect r = bounds(shape->path, m);
      if (r.empty()) continue;
      if (shape->stroke.paint.visible() && !shape->stroke.hairline) {
        const double pad = shape->stroke.width * m.mean_scale() / 2;
        r = {r.x0 - pad, r.y0 - pad, r.x1 + pad, r.y1 + pad};
      }
      out.add(r);
    } else if (const auto* text = std::get_if<Text>(&node.value)) {
      out.add(bounds(text->outline, m));
    } else if (const auto* picture = std::get_if<Picture>(&node.value)) {
      out.add(transform_rect(picture->bounds, m));
    } else if (const auto* child = std::get_if<Group>(&node.value)) {
      if (child->visible) add_bounds(*child, m, out, depth + 1);
    }
  }
}

}  // namespace

std::uint32_t aci_rgb(int index) noexcept {
  if (index < 0 || index > 255) return 0;
  return aci_palette[static_cast<std::size_t>(index)];
}

Color aci_color(int index) noexcept {
  if (index == 7 || index <= 0 || index > 255) return Color{0, 0, 0, 1};
  const std::uint32_t rgb = aci_rgb(index);
  return Color::rgb8(static_cast<int>((rgb >> 16) & 0xFF), static_cast<int>((rgb >> 8) & 0xFF),
                     static_cast<int>(rgb & 0xFF));
}

int nearest_aci(Color color) noexcept {
  const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  const int r = byte(color.r), g = byte(color.g), b = byte(color.b);
  if (r == 0 && g == 0 && b == 0) return 7;  // black is drawn by colour 7 (white on dark screens)
  int best = 7;
  long long best_distance = std::numeric_limits<long long>::max();
  for (int i = 1; i <= 255; ++i) {
    const std::uint32_t rgb = aci_palette[static_cast<std::size_t>(i)];
    const long long dr = r - static_cast<int>((rgb >> 16) & 0xFF);
    const long long dg = g - static_cast<int>((rgb >> 8) & 0xFF);
    const long long db = b - static_cast<int>(rgb & 0xFF);
    const long long distance = dr * dr * 3 + dg * dg * 4 + db * db * 2;
    if (distance < best_distance) {
      best_distance = distance;
      best = i;
    }
  }
  return best;
}

void append_utf8(std::string& out, char32_t cp) {
  if (!util::is_scalar_value(cp)) cp = util::replacement_character;
  out += util::utf32_to_utf8(std::u32string_view(&cp, 1));
}

std::string number(double value, int decimals) {
  if (!std::isfinite(value)) return "0";
  std::array<char, 64> buffer{};
  auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed,
                                    std::clamp(decimals, 0, 17));
  if (error != std::errc{}) return util::format_double(value);
  std::string text(buffer.data(), end);
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
  }
  if (text == "-0" || text.empty()) text = "0";
  return text;
}

Rect content_bounds(const Group& group, const Matrix& parent) {
  Rect out;
  add_bounds(group, parent, out, 0);
  return out;
}

// --------------------------------------------------------------------------- 3D

Vec3 cross3(Vec3 a, Vec3 b) noexcept { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

Vec3 normalized3(Vec3 v) noexcept {
  const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (!(len > 1e-300) || !std::isfinite(len)) return {0, 0, 1};
  return v * (1.0 / len);
}

Affine3 ocs_to_wcs(Vec3 extrusion) noexcept {
  const Vec3 az = normalized3(extrusion);
  if (std::abs(az.x) < 1e-12 && std::abs(az.y) < 1e-12 && az.z > 0) return {};
  constexpr double limit = 1.0 / 64.0;
  const Vec3 ax = (std::abs(az.x) < limit && std::abs(az.y) < limit) ? normalized3(cross3({0, 1, 0}, az))
                                                                       : normalized3(cross3({0, 0, 1}, az));
  const Vec3 ay = normalized3(cross3(az, ax));
  return {ax, ay, az, {0, 0, 0}};
}

// --------------------------------------------------------------------------- bulges

void bulge_to(Path& path, Point from, Point to, double bulge) {
  const double d = length(to - from);
  if (!std::isfinite(bulge) || std::abs(bulge) < 1e-10 || d < 1e-12) {
    path.line_to(to);
    return;
  }
  const double b = std::clamp(bulge, -1e6, 1e6);
  const double radius = d * (1 + b * b) / (4 * std::abs(b));
  svg_arc_to(path, from, radius, radius, 0.0, std::abs(b) > 1.0, b > 0, to);
}

// --------------------------------------------------------------------------- NURBS

namespace {

bool knots_valid(const Nurbs& c) {
  const std::size_t n = c.control.size();
  if (c.degree < 1 || c.degree > 11 || n < static_cast<std::size_t>(c.degree) + 1) return false;
  if (c.knots.size() != n + static_cast<std::size_t>(c.degree) + 1) return false;
  for (std::size_t i = 0; i < c.knots.size(); ++i) {
    if (!std::isfinite(c.knots[i])) return false;
    if (i > 0 && c.knots[i] < c.knots[i - 1]) return false;
  }
  if (!c.weights.empty()) {
    if (c.weights.size() != n) return false;
    for (const double w : c.weights) {
      if (!(w > 0) || !std::isfinite(w)) return false;
    }
  }
  for (const Point& p : c.control) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
  }
  const auto p = static_cast<std::size_t>(c.degree);
  return c.knots[n] > c.knots[p];
}

bool rational(const Nurbs& c) {
  for (const double w : c.weights) {
    if (std::abs(w - c.weights.front()) > 1e-12) return true;
  }
  return false;
}

// Bézier segments of a clamped non-rational curve (The NURBS Book, A5.6).
bool decompose(const Nurbs& c, std::vector<std::vector<Point>>& out, std::size_t max_segments) {
  const int p = c.degree;
  const std::vector<double>& u = c.knots;
  const int m = static_cast<int>(u.size()) - 1;
  for (int i = 1; i <= p; ++i) {
    if (u[static_cast<std::size_t>(i)] != u[0] || u[static_cast<std::size_t>(m - i)] != u[static_cast<std::size_t>(m)]) {
      return false;
    }
  }
  const auto P = [&](int i) { return c.control[static_cast<std::size_t>(i)]; };
  const auto U = [&](int i) { return u[static_cast<std::size_t>(i)]; };
  int a = p, b = p + 1;
  std::vector<Point> current(static_cast<std::size_t>(p) + 1), next(static_cast<std::size_t>(p) + 1);
  for (int i = 0; i <= p; ++i) current[static_cast<std::size_t>(i)] = P(i);
  std::vector<double> alphas(static_cast<std::size_t>(p) + 1, 0.0);
  while (b < m) {
    const int i0 = b;
    while (b < m && U(b + 1) == U(b)) ++b;
    const int mult = b - i0 + 1;
    if (mult > p && b < m) return false;
    const int r = std::max(0, p - mult);
    if (mult < p) {
      const double numer = U(b) - U(a);
      for (int j = p; j > mult; --j) {
        const double den = U(a + j) - U(a);
        alphas[static_cast<std::size_t>(j - mult - 1)] = den != 0 ? numer / den : 0.0;
      }
      for (int j = 1; j <= r; ++j) {
        const int save = r - j, s = mult + j;
        for (int k = p; k >= s; --k) {
          const double alpha = alphas[static_cast<std::size_t>(k - s)];
          current[static_cast<std::size_t>(k)] =
              current[static_cast<std::size_t>(k)] * alpha + current[static_cast<std::size_t>(k - 1)] * (1 - alpha);
        }
        if (b < m) next[static_cast<std::size_t>(save)] = current[static_cast<std::size_t>(p)];
      }
    }
    if (U(a) != U(b)) out.push_back(current);  // skip zero-length spans
    if (out.size() > max_segments) return false;
    if (b < m) {
      for (int i = r; i <= p; ++i) next[static_cast<std::size_t>(i)] = P(b - p + i);
      current = next;
      a = b;
      b = b + 1;
    }
  }
  return !out.empty();
}

}  // namespace

Point nurbs_point(const Nurbs& c, double u) {
  const int p = c.degree;
  const int n = static_cast<int>(c.control.size()) - 1;
  const auto K = [&](int i) { return c.knots[static_cast<std::size_t>(i)]; };
  u = std::clamp(u, K(p), K(n + 1));
  int span = p;
  while (span < n && u >= K(span + 1)) ++span;
  // de Boor on homogeneous coordinates.
  std::array<double, 12> wx{}, wy{}, ww{};
  for (int j = 0; j <= p; ++j) {
    const int index = span - p + j;
    const double w = c.weights.empty() ? 1.0 : c.weights[static_cast<std::size_t>(index)];
    wx[static_cast<std::size_t>(j)] = c.control[static_cast<std::size_t>(index)].x * w;
    wy[static_cast<std::size_t>(j)] = c.control[static_cast<std::size_t>(index)].y * w;
    ww[static_cast<std::size_t>(j)] = w;
  }
  for (int r = 1; r <= p; ++r) {
    for (int j = p; j >= r; --j) {
      const int i = span - p + j;
      const double den = K(i + p - r + 1) - K(i);
      const double alpha = den != 0 ? (u - K(i)) / den : 0.0;
      const auto J = static_cast<std::size_t>(j);
      wx[J] = (1 - alpha) * wx[J - 1] + alpha * wx[J];
      wy[J] = (1 - alpha) * wy[J - 1] + alpha * wy[J];
      ww[J] = (1 - alpha) * ww[J - 1] + alpha * ww[J];
    }
  }
  const auto P = static_cast<std::size_t>(p);
  if (ww[P] == 0) return {wx[P], wy[P]};
  return {wx[P] / ww[P], wy[P] / ww[P]};
}

bool append_nurbs(Path& path, const Nurbs& curve, double tolerance, bool connect, std::size_t max_segments) {
  if (!knots_valid(curve)) return false;
  const auto begin = [&](Point first) {
    if (connect && !path.empty() && path.verbs.back() != Verb::Close) path.line_to(first);
    else path.move_to(first);
  };
  if (curve.degree <= 3 && !rational(curve)) {
    std::vector<std::vector<Point>> pieces;
    if (decompose(curve, pieces, max_segments)) {
      begin(pieces.front().front());
      for (const std::vector<Point>& q : pieces) {
        if (curve.degree == 1) path.line_to(q[1]);
        else if (curve.degree == 2) path.quad_to(q[1], q[2]);
        else path.cubic_to(q[1], q[2], q[3]);
      }
      return true;
    }
  }
  // Sampling: per non-empty knot span, enough points for the tolerance.
  const int p = curve.degree;
  const int n = static_cast<int>(curve.control.size()) - 1;
  double polygon = 0;
  for (std::size_t i = 1; i < curve.control.size(); ++i) polygon += length(curve.control[i] - curve.control[i - 1]);
  int spans = 0;
  for (int i = p; i <= n; ++i) {
    if (curve.knots[static_cast<std::size_t>(i + 1)] > curve.knots[static_cast<std::size_t>(i)]) ++spans;
  }
  if (spans == 0) return false;
  const double tol = tolerance > 0 ? tolerance : 1e-3;
  const int per_span = std::clamp(
      static_cast<int>(std::ceil(std::sqrt(polygon / std::max(spans, 1) / tol) * 2)), 4,
      std::max(4, static_cast<int>(std::min<std::size_t>(256, max_segments / static_cast<std::size_t>(spans) + 1))));
  begin(nurbs_point(curve, curve.knots[static_cast<std::size_t>(p)]));
  for (int i = p; i <= n; ++i) {
    const double u0 = curve.knots[static_cast<std::size_t>(i)], u1 = curve.knots[static_cast<std::size_t>(i + 1)];
    if (!(u1 > u0)) continue;
    for (int s = 1; s <= per_span; ++s) path.line_to(nurbs_point(curve, u0 + (u1 - u0) * s / per_span));
  }
  return true;
}

bool append_fit_spline(Path& path, std::span<const Point> fit_in, std::optional<Point> start_tangent,
                       std::optional<Point> end_tangent, bool connect) {
  std::vector<Point> fit;
  for (const Point& q : fit_in) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y)) return false;
    if (fit.empty() || length(q - fit.back()) > 1e-12) fit.push_back(q);
  }
  if (fit.empty()) return false;
  if (connect && !path.empty() && path.verbs.back() != Verb::Close) path.line_to(fit.front());
  else path.move_to(fit.front());
  if (fit.size() == 1) return true;
  const std::size_t n = fit.size() - 1;
  std::vector<double> h(n);
  for (std::size_t i = 0; i < n; ++i) h[i] = length(fit[i + 1] - fit[i]);
  const auto unit = [](std::optional<Point> t) -> std::optional<Point> {
    if (!t) return std::nullopt;
    const double len = length(*t);
    if (!(len > 1e-12) || !std::isfinite(len)) return std::nullopt;
    return *t * (1.0 / len);
  };
  start_tangent = unit(start_tangent);
  end_tangent = unit(end_tangent);
  // Tridiagonal system for the first derivatives D_i (C2 cubic, chord-length parameters).
  std::vector<double> lower(n + 1, 0.0), diag(n + 1, 0.0), upper(n + 1, 0.0);
  std::vector<Point> rhs(n + 1);
  if (start_tangent) {
    diag[0] = 1;
    rhs[0] = *start_tangent;
  } else {
    diag[0] = 2;
    upper[0] = 1;
    rhs[0] = (fit[1] - fit[0]) * (3.0 / h[0]);
  }
  for (std::size_t i = 1; i < n; ++i) {
    lower[i] = h[i];
    diag[i] = 2 * (h[i - 1] + h[i]);
    upper[i] = h[i - 1];
    rhs[i] = (fit[i] - fit[i - 1]) * (3.0 * h[i] / h[i - 1]) + (fit[i + 1] - fit[i]) * (3.0 * h[i - 1] / h[i]);
  }
  if (end_tangent) {
    diag[n] = 1;
    lower[n] = 0;
    rhs[n] = *end_tangent;
  } else {
    lower[n] = 1;
    diag[n] = 2;
    rhs[n] = (fit[n] - fit[n - 1]) * (3.0 / h[n - 1]);
  }
  // Thomas algorithm.
  for (std::size_t i = 1; i <= n; ++i) {
    const double w = lower[i] / diag[i - 1];
    diag[i] -= w * upper[i - 1];
    rhs[i] = rhs[i] - rhs[i - 1] * w;
  }
  std::vector<Point> d(n + 1);
  d[n] = rhs[n] * (1.0 / diag[n]);
  for (std::size_t i = n; i-- > 0;) d[i] = (rhs[i] - d[i + 1] * upper[i]) * (1.0 / diag[i]);
  for (std::size_t i = 0; i < n; ++i) {
    path.cubic_to(fit[i] + d[i] * (h[i] / 3), fit[i + 1] - d[i + 1] * (h[i] / 3), fit[i + 1]);
  }
  return true;
}

}  // namespace cvt::vector::cad
