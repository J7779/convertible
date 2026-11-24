// Convertible desktop - design tokens (parsed from assets/brand/tokens.json,
// embedded at build time) resolved into a theme for one appearance.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "base/geometry.hpp"
#include "base/json.hpp"
#include "text/text.hpp"
#include "ui/anim.hpp"

namespace cvd::ui {

struct ShadowLayer {
  float x = 0.0f, y = 0.0f, blur = 0.0f, spread = 0.0f;
  Color color;
};

struct Palette {
  Color background, surface, surface_elevated, surface_sunken, border, border_strong;
  Color text_primary, text_secondary, text_disabled;
  Color accent, accent_text, accent_fill, accent_fill_hover, accent_fill_pressed, on_accent, accent_subtle;
  Color gradient_start, gradient_mid, gradient_end;
  Color success, success_subtle, warning, warning_subtle, error, error_subtle;
  Color focus_ring, scrim;
};

struct TypeScale {
  text::Style display, title1, title2, title3, body_large, body, body_strong, label, caption, overline, mono, format_label;
};

struct Motion {
  float instant = 0.08f, fast = 0.14f, base = 0.22f, slow = 0.36f, slower = 0.56f;  // seconds
  float intro = 1.8f, spinner_loop = 1.4f;
  CubicBezier standard{0.2f, 0.0f, 0.0f, 1.0f};
  CubicBezier decelerate{0.16f, 1.0f, 0.3f, 1.0f};
  CubicBezier accelerate{0.7f, 0.0f, 0.84f, 0.0f};
  CubicBezier in_out{0.65f, 0.0f, 0.35f, 1.0f};
  CubicBezier emphasized{0.7f, 0.0f, 0.2f, 1.0f};
  CubicBezier settle{0.22f, 0.61f, 0.36f, 1.0f};
  CubicBezier overshoot{0.34f, 1.56f, 0.64f, 1.0f};
};

class Tokens {
 public:
  /// Parses tokens.json text; false on malformed input (defaults are kept).
  bool load(std::string_view json_text);
  static Tokens& instance();

  [[nodiscard]] const Json& root() const { return root_; }
  [[nodiscard]] Color brand(std::string_view key) const;

 private:
  Json root_;
};

struct Theme {
  bool dark = true;
  Palette color;
  TypeScale type;
  Motion motion;
  std::array<std::vector<ShadowLayer>, 5> elevation;
  float radius_xs = 4, radius_sm = 8, radius_md = 12, radius_lg = 16, radius_xl = 24, radius_xxl = 32;
  std::array<float, 13> space{0, 2, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 80};
  float control_sm = 32, control_md = 40, control_lg = 48, touch_target = 44;
  float focus_ring_width = 2, focus_ring_offset = 2;

  /// Category accent ("image", "audio", ...); "other" for unknown ones.
  [[nodiscard]] Color category(std::string_view id) const;
  /// The Flux gradient stops for this appearance.
  [[nodiscard]] std::vector<std::pair<float, Color>> flux() const;

  static Theme build(const Tokens& tokens, bool dark);

  std::map<std::string, Color, std::less<>> categories;
};

}  // namespace cvd::ui
