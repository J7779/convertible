// Convertible desktop - basic geometry and colour types.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace cvd {

inline constexpr float kPi = 3.14159265358979323846f;

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;

  constexpr Vec2() = default;
  constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

  constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
  constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
  constexpr Vec2 operator-() const { return {-x, -y}; }
  Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
  Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
  constexpr bool operator==(const Vec2&) const = default;

  [[nodiscard]] float length() const { return std::sqrt(x * x + y * y); }
  [[nodiscard]] constexpr float dot(Vec2 o) const { return x * o.x + y * o.y; }
  [[nodiscard]] constexpr float cross(Vec2 o) const { return x * o.y - y * o.x; }
  [[nodiscard]] Vec2 normalized() const {
    const float len = length();
    return len > 1e-12f ? Vec2{x / len, y / len} : Vec2{0.0f, 0.0f};
  }
  [[nodiscard]] constexpr Vec2 perp() const { return {-y, x}; }
};

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;

  constexpr Rect() = default;
  constexpr Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
  static constexpr Rect from_points(float x0, float y0, float x1, float y1) { return {x0, y0, x1 - x0, y1 - y0}; }

  [[nodiscard]] constexpr float left() const { return x; }
  [[nodiscard]] constexpr float top() const { return y; }
  [[nodiscard]] constexpr float right() const { return x + w; }
  [[nodiscard]] constexpr float bottom() const { return y + h; }
  [[nodiscard]] constexpr Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
  [[nodiscard]] constexpr bool empty() const { return w <= 0.0f || h <= 0.0f; }
  [[nodiscard]] constexpr bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
  [[nodiscard]] constexpr Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
  [[nodiscard]] constexpr Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
  [[nodiscard]] constexpr Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
  [[nodiscard]] Rect intersect(const Rect& o) const {
    const float x0 = std::max(x, o.x);
    const float y0 = std::max(y, o.y);
    const float x1 = std::min(right(), o.right());
    const float y1 = std::min(bottom(), o.bottom());
    if (x1 <= x0 || y1 <= y0) return {x0, y0, 0.0f, 0.0f};
    return {x0, y0, x1 - x0, y1 - y0};
  }
  [[nodiscard]] Rect unite(const Rect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    const float x0 = std::min(x, o.x);
    const float y0 = std::min(y, o.y);
    return {x0, y0, std::max(right(), o.right()) - x0, std::max(bottom(), o.bottom()) - y0};
  }
  constexpr bool operator==(const Rect&) const = default;
};

/// Straight (non-premultiplied) sRGB colour with components in 0..1.
struct Color {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;

  constexpr Color() = default;
  constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}

  static constexpr Color rgba8(int r, int g, int b, int a = 255) {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f,
            static_cast<float>(a) / 255.0f};
  }
  /// Parses "#RGB", "#RRGGBB" or "#RRGGBBAA"; returns magenta for malformed input.
  static Color hex(std::string_view text);

  [[nodiscard]] constexpr Color with_alpha(float alpha) const { return {r, g, b, alpha}; }
  [[nodiscard]] constexpr Color fade(float k) const { return {r, g, b, a * k}; }
  [[nodiscard]] static constexpr Color lerp(Color p, Color q, float t) {
    return {p.r + (q.r - p.r) * t, p.g + (q.g - p.g) * t, p.b + (q.b - p.b) * t, p.a + (q.a - p.a) * t};
  }
  /// Packs to premultiplied BGRA (0xAARRGGBB in a little-endian uint32).
  [[nodiscard]] std::uint32_t premul_u32() const;
  /// WCAG relative luminance.
  [[nodiscard]] float luminance() const;
  constexpr bool operator==(const Color&) const = default;
};

/// WCAG 2.1 contrast ratio between two opaque colours.
float contrast_ratio(Color a, Color b);

/// 2D affine transform: x' = a*x + c*y + e, y' = b*x + d*y + f.
struct Affine {
  float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;

  static constexpr Affine identity() { return {}; }
  static constexpr Affine translate(float tx, float ty) { return {1.0f, 0.0f, 0.0f, 1.0f, tx, ty}; }
  static constexpr Affine scale(float sx, float sy) { return {sx, 0.0f, 0.0f, sy, 0.0f, 0.0f}; }
  static Affine rotate(float radians) {
    const float cs = std::cos(radians);
    const float sn = std::sin(radians);
    return {cs, sn, -sn, cs, 0.0f, 0.0f};
  }
  /// this * o : applies o first, then this.
  [[nodiscard]] constexpr Affine operator*(const Affine& o) const {
    return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d, a * o.e + c * o.f + e,
            b * o.e + d * o.f + f};
  }
  [[nodiscard]] constexpr Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
  [[nodiscard]] constexpr Vec2 apply_vector(Vec2 p) const { return {a * p.x + c * p.y, b * p.x + d * p.y}; }
  [[nodiscard]] Affine inverted() const {
    const float det = a * d - b * c;
    if (std::fabs(det) < 1e-12f) return identity();
    const float id = 1.0f / det;
    return {d * id, -b * id, -c * id, a * id, (c * f - d * e) * id, (b * e - a * f) * id};
  }
  /// True when the transform is only a translation plus positive axis scaling.
  [[nodiscard]] constexpr bool is_axis_aligned() const { return b == 0.0f && c == 0.0f && a > 0.0f && d > 0.0f; }
  /// Average linear scale factor.
  [[nodiscard]] float mean_scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace cvd
