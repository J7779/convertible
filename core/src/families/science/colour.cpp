// Convertible core - science family: colour-space conversions.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/colour.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cvt::science::colour {

namespace {

using Mat3 = std::array<Vec3, 3>;

Vec3 mul(const Mat3& m, const Vec3& v) noexcept {
  return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
          m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

Mat3 inverse(const Mat3& m) noexcept {
  const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  return {{{(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det},
           {(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det},
           {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}}};
}

Mat3 product(const Mat3& x, const Mat3& y) noexcept {
  Mat3 out{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) out[r][c] = x[r][0] * y[0][c] + x[r][1] * y[1][c] + x[r][2] * y[2][c];
  }
  return out;
}

Vec3 xy_to_xyz(double x, double y) noexcept { return {x / y, 1.0, (1 - x - y) / y}; }

// White points from their CIE 1931 chromaticities (as in IEC 61966-2-1 and CIE 15).
Vec3 d65_white() noexcept { return xy_to_xyz(0.3127, 0.3290); }
Vec3 d50_white() noexcept { return xy_to_xyz(0.3457, 0.3585); }

/// RGB -> XYZ matrix of the sRGB primaries, derived from their chromaticities.
Mat3 srgb_matrix() noexcept {
  const Vec3 r = xy_to_xyz(0.64, 0.33), g = xy_to_xyz(0.30, 0.60), b = xy_to_xyz(0.15, 0.06);
  const Mat3 p = {{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
  const Vec3 s = mul(inverse(p), d65_white());
  return {{{p[0][0] * s[0], p[0][1] * s[1], p[0][2] * s[2]},
           {p[1][0] * s[0], p[1][1] * s[1], p[1][2] * s[2]},
           {p[2][0] * s[0], p[2][1] * s[1], p[2][2] * s[2]}}};
}

/// Bradford chromatic adaptation from white `from` to white `to`.
Mat3 bradford(const Vec3& from, const Vec3& to) noexcept {
  constexpr Mat3 m = {{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
  const Vec3 a = mul(m, from), b = mul(m, to);
  const Mat3 scale = {{{b[0] / a[0], 0, 0}, {0, b[1] / a[1], 0}, {0, 0, b[2] / a[2]}}};
  return product(inverse(m), product(scale, m));
}

constexpr double epsilon = 216.0 / 24389.0;
constexpr double kappa = 24389.0 / 27.0;

}  // namespace

double srgb_to_linear(double v) noexcept { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double linear_to_srgb(double v) noexcept { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; }

Vec3 srgb_to_xyz_d65(const Vec3& rgb) noexcept {
  return mul(srgb_matrix(), {srgb_to_linear(rgb[0]), srgb_to_linear(rgb[1]), srgb_to_linear(rgb[2])});
}

Vec3 xyz_d65_to_srgb(const Vec3& xyz) noexcept {
  const Vec3 l = mul(inverse(srgb_matrix()), xyz);
  return {linear_to_srgb(l[0]), linear_to_srgb(l[1]), linear_to_srgb(l[2])};
}

Vec3 d65_to_d50(const Vec3& xyz) noexcept { return mul(bradford(d65_white(), d50_white()), xyz); }
Vec3 d50_to_d65(const Vec3& xyz) noexcept { return mul(bradford(d50_white(), d65_white()), xyz); }

Vec3 white(Illuminant illuminant) noexcept {
  return illuminant == Illuminant::D50 ? d50_white() : d65_white();
}

Vec3 xyz_to_lab(const Vec3& xyz, Illuminant illuminant) noexcept {
  const Vec3 w = white(illuminant);
  const auto f = [](double t) { return t > epsilon ? std::cbrt(t) : (kappa * t + 16.0) / 116.0; };
  const double fx = f(xyz[0] / w[0]), fy = f(xyz[1] / w[1]), fz = f(xyz[2] / w[2]);
  return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

Vec3 lab_to_xyz(const Vec3& lab, Illuminant illuminant) noexcept {
  const Vec3 w = white(illuminant);
  const double fy = (lab[0] + 16.0) / 116.0;
  const double fx = fy + lab[1] / 500.0;
  const double fz = fy - lab[2] / 200.0;
  const auto inv = [](double f) { return f * f * f > epsilon ? f * f * f : (116.0 * f - 16.0) / kappa; };
  const double yr = lab[0] > kappa * epsilon ? fy * fy * fy : lab[0] / kappa;
  return {inv(fx) * w[0], yr * w[1], inv(fz) * w[2]};
}

Vec3 srgb_to_lab(const Vec3& rgb, Illuminant illuminant) noexcept {
  Vec3 xyz = srgb_to_xyz_d65(rgb);
  if (illuminant == Illuminant::D50) xyz = d65_to_d50(xyz);
  return xyz_to_lab(xyz, illuminant);
}

Vec3 lab_to_srgb(const Vec3& lab, Illuminant illuminant) noexcept {
  Vec3 xyz = lab_to_xyz(lab, illuminant);
  if (illuminant == Illuminant::D50) xyz = d50_to_d65(xyz);
  return xyz_d65_to_srgb(xyz);
}

Vec3 lab_to_lch(const Vec3& lab) noexcept {
  double h = std::atan2(lab[2], lab[1]) * 180.0 / std::numbers::pi;
  if (h < 0) h += 360.0;
  return {lab[0], std::hypot(lab[1], lab[2]), h};
}

Vec3 lch_to_lab(const Vec3& lch) noexcept {
  const double h = lch[2] * std::numbers::pi / 180.0;
  return {lch[0], lch[1] * std::cos(h), lch[1] * std::sin(h)};
}

Vec3 rgb_to_hsl(const Vec3& rgb) noexcept {
  const double mx = std::max({rgb[0], rgb[1], rgb[2]});
  const double mn = std::min({rgb[0], rgb[1], rgb[2]});
  const double l = (mx + mn) / 2;
  const double d = mx - mn;
  if (d == 0) return {0, 0, l};
  const double s = d / (1 - std::fabs(2 * l - 1));
  double h;
  if (mx == rgb[0]) h = std::fmod((rgb[1] - rgb[2]) / d, 6.0);
  else if (mx == rgb[1]) h = (rgb[2] - rgb[0]) / d + 2;
  else h = (rgb[0] - rgb[1]) / d + 4;
  h *= 60;
  if (h < 0) h += 360;
  return {h, s, l};
}

namespace {
Vec3 from_chroma(double h, double c, double m) {
  const double hp = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0) / 60.0;
  const double x = c * (1 - std::fabs(std::fmod(hp, 2.0) - 1));
  Vec3 rgb{};
  if (hp < 1) rgb = {c, x, 0};
  else if (hp < 2) rgb = {x, c, 0};
  else if (hp < 3) rgb = {0, c, x};
  else if (hp < 4) rgb = {0, x, c};
  else if (hp < 5) rgb = {x, 0, c};
  else rgb = {c, 0, x};
  return {rgb[0] + m, rgb[1] + m, rgb[2] + m};
}
}  // namespace

Vec3 hsl_to_rgb(const Vec3& hsl) noexcept {
  const double c = (1 - std::fabs(2 * hsl[2] - 1)) * hsl[1];
  return from_chroma(hsl[0], c, hsl[2] - c / 2);
}

Vec3 rgb_to_hsv(const Vec3& rgb) noexcept {
  const double mx = std::max({rgb[0], rgb[1], rgb[2]});
  const double mn = std::min({rgb[0], rgb[1], rgb[2]});
  const double d = mx - mn;
  const Vec3 hsl = rgb_to_hsl(rgb);
  return {hsl[0], mx == 0 ? 0 : d / mx, mx};
}

Vec3 hsv_to_rgb(const Vec3& hsv) noexcept {
  const double c = hsv[2] * hsv[1];
  return from_chroma(hsv[0], c, hsv[2] - c);
}

std::array<double, 4> rgb_to_cmyk(const Vec3& rgb) noexcept {
  const double k = 1 - std::max({rgb[0], rgb[1], rgb[2]});
  if (k >= 1) return {0, 0, 0, 1};
  return {(1 - rgb[0] - k) / (1 - k), (1 - rgb[1] - k) / (1 - k), (1 - rgb[2] - k) / (1 - k), k};
}

Vec3 cmyk_to_rgb(const std::array<double, 4>& c) noexcept {
  return {(1 - c[0]) * (1 - c[3]), (1 - c[1]) * (1 - c[3]), (1 - c[2]) * (1 - c[3])};
}

}  // namespace cvt::science::colour
