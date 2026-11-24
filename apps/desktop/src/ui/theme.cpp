// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "ui/theme.hpp"

namespace cvd::ui {

Tokens& Tokens::instance() {
  static Tokens tokens;
  return tokens;
}

bool Tokens::load(std::string_view json_text) {
  auto parsed = Json::parse(json_text);
  if (!parsed || !parsed->is_object()) return false;
  root_ = std::move(*parsed);
  return true;
}

Color Tokens::brand(std::string_view key) const { return Color::hex(root_["brand"][key].str()); }

namespace {

text::Style style_from(const Json& j, text::Style fallback) {
  if (!j.is_object()) return fallback;
  text::Style s = fallback;
  s.size = static_cast<float>(j["size"].as_number(fallback.size));
  s.line_height = static_cast<float>(j["lineHeight"].as_number(fallback.line_height));
  s.tracking = static_cast<float>(j["letterSpacing"].as_number(fallback.tracking));
  const int weight = j["weight"].as_int(400);
  const bool mono = j["family"].str() == "mono";
  if (mono) {
    s.face = weight >= 500 ? text::Face::MonoMedium : text::Face::Mono;
  } else {
    s.face = weight >= 600 ? text::Face::SemiBold : (weight >= 500 ? text::Face::Medium : text::Face::Regular);
  }
  s.uppercase = j["textTransform"].str() == "uppercase";
  return s;
}

CubicBezier easing_from(const Json& j, CubicBezier fallback) {
  if (!j.is_array() || j.size() != 4) return fallback;
  return CubicBezier(static_cast<float>(j[0].as_number()), static_cast<float>(j[1].as_number()),
                     static_cast<float>(j[2].as_number()), static_cast<float>(j[3].as_number()));
}

}  // namespace

Theme Theme::build(const Tokens& tokens, bool dark) {
  Theme t;
  t.dark = dark;
  const Json& root = tokens.root();
  const Json& c = root["color"][dark ? "dark" : "light"];
  const auto col = [&](std::string_view key, const char* fallback) {
    const std::string& v = c[key].str();
    return Color::hex(v.empty() ? fallback : v);
  };
  Palette& p = t.color;
  p.background = col("background", dark ? "#0D0F16" : "#F6F6FA");
  p.surface = col("surface", dark ? "#151823" : "#FFFFFF");
  p.surface_elevated = col("surfaceElevated", dark ? "#1D2130" : "#FFFFFF");
  p.surface_sunken = col("surfaceSunken", dark ? "#090A0F" : "#ECEDF3");
  p.border = col("border", dark ? "#2B3045" : "#E1E3EC");
  p.border_strong = col("borderStrong", dark ? "#646C8E" : "#858BA3");
  p.text_primary = col("textPrimary", dark ? "#F3F4F9" : "#12141C");
  p.text_secondary = col("textSecondary", dark ? "#A9AFC6" : "#545A72");
  p.text_disabled = col("textDisabled", dark ? "#676D87" : "#9499AE");
  p.accent = col("accent", dark ? "#7C6CFF" : "#6654F0");
  p.accent_text = col("accentText", dark ? "#A79DFF" : "#5746E0");
  p.accent_fill = col("accentFill", dark ? "#6C5AF3" : "#5746E0");
  p.accent_fill_hover = col("accentFillHover", dark ? "#705FF5" : "#4E3DD3");
  p.accent_fill_pressed = col("accentFillPressed", dark ? "#5E4CE8" : "#4435BF");
  p.on_accent = col("onAccent", "#FFFFFF");
  p.accent_subtle = col("accentSubtle", dark ? "#221F45" : "#ECEAFF");
  p.gradient_start = col("accentGradientStart", "#7C6CFF");
  p.gradient_mid = col("accentGradientMid", "#D662B0");
  p.gradient_end = col("accentGradientEnd", "#FF6452");
  p.success = col("success", dark ? "#3DD68C" : "#137C47");
  p.success_subtle = col("successSubtle", dark ? "#12301F" : "#E2F5EA");
  p.warning = col("warning", dark ? "#F5B83D" : "#8F5B00");
  p.warning_subtle = col("warningSubtle", dark ? "#33270C" : "#FBF0D9");
  p.error = col("error", dark ? "#FF6B81" : "#C42547");
  p.error_subtle = col("errorSubtle", dark ? "#3A161D" : "#FCE9ED");
  p.focus_ring = col("focusRing", dark ? "#A79DFF" : "#5746E0");
  p.scrim = col("scrim", dark ? "#05060ACC" : "#12141C66");

  for (const auto& [name, value] : root["category"].members()) {
    t.categories[name] = Color::hex(value[dark ? "dark" : "light"].str());
  }

  const Json& r = root["radius"];
  t.radius_xs = static_cast<float>(r["xs"].as_number(4));
  t.radius_sm = static_cast<float>(r["sm"].as_number(8));
  t.radius_md = static_cast<float>(r["md"].as_number(12));
  t.radius_lg = static_cast<float>(r["lg"].as_number(16));
  t.radius_xl = static_cast<float>(r["xl"].as_number(24));
  t.radius_xxl = static_cast<float>(r["xxl"].as_number(32));
  for (std::size_t i = 0; i < t.space.size(); ++i) {
    t.space[i] = static_cast<float>(root["space"][std::to_string(i)].as_number(t.space[i]));
  }
  const Json& sz = root["size"];
  t.control_sm = static_cast<float>(sz["controlSm"].as_number(32));
  t.control_md = static_cast<float>(sz["controlMd"].as_number(40));
  t.control_lg = static_cast<float>(sz["controlLg"].as_number(48));
  t.touch_target = static_cast<float>(sz["touchTarget"].as_number(44));
  t.focus_ring_width = static_cast<float>(sz["focusRingWidth"].as_number(2));
  t.focus_ring_offset = static_cast<float>(sz["focusRingOffset"].as_number(2));

  const Json& elev = root["elevation"][dark ? "dark" : "light"];
  for (std::size_t level = 0; level < t.elevation.size(); ++level) {
    for (const Json& layer : elev[std::to_string(level)].items()) {
      ShadowLayer s;
      s.x = static_cast<float>(layer["x"].as_number());
      s.y = static_cast<float>(layer["y"].as_number());
      s.blur = static_cast<float>(layer["blur"].as_number());
      s.spread = static_cast<float>(layer["spread"].as_number());
      s.color = Color::hex(layer["color"].str());
      t.elevation[level].push_back(s);
    }
  }

  const Json& m = root["motion"];
  const Json& d = m["duration"];
  t.motion.instant = static_cast<float>(d["instant"].as_number(80)) / 1000.0f;
  t.motion.fast = static_cast<float>(d["fast"].as_number(140)) / 1000.0f;
  t.motion.base = static_cast<float>(d["base"].as_number(220)) / 1000.0f;
  t.motion.slow = static_cast<float>(d["slow"].as_number(360)) / 1000.0f;
  t.motion.slower = static_cast<float>(d["slower"].as_number(560)) / 1000.0f;
  t.motion.intro = static_cast<float>(d["intro"].as_number(1800)) / 1000.0f;
  t.motion.spinner_loop = static_cast<float>(d["spinnerLoop"].as_number(1400)) / 1000.0f;
  const Json& e = m["easing"];
  t.motion.standard = easing_from(e["standard"], t.motion.standard);
  t.motion.decelerate = easing_from(e["decelerate"], t.motion.decelerate);
  t.motion.accelerate = easing_from(e["accelerate"], t.motion.accelerate);
  t.motion.in_out = easing_from(e["inOut"], t.motion.in_out);
  t.motion.emphasized = easing_from(e["emphasized"], t.motion.emphasized);
  t.motion.settle = easing_from(e["settle"], t.motion.settle);
  t.motion.overshoot = easing_from(e["overshoot"], t.motion.overshoot);

  const Json& s = root["typography"]["scale"];
  TypeScale& ty = t.type;
  ty.display = style_from(s["display"], {text::Face::SemiBold, 40, 46, -0.022f});
  ty.title1 = style_from(s["title1"], {text::Face::SemiBold, 30, 36, -0.019f});
  ty.title2 = style_from(s["title2"], {text::Face::SemiBold, 24, 30, -0.015f});
  ty.title3 = style_from(s["title3"], {text::Face::SemiBold, 20, 26, -0.011f});
  ty.body_large = style_from(s["bodyLarge"], {text::Face::Regular, 17, 24, -0.006f});
  ty.body = style_from(s["body"], {text::Face::Regular, 15, 22, -0.003f});
  ty.body_strong = style_from(s["bodyStrong"], {text::Face::Medium, 15, 22, -0.003f});
  ty.label = style_from(s["label"], {text::Face::Medium, 13, 18, 0.0f});
  ty.caption = style_from(s["caption"], {text::Face::Regular, 12, 16, 0.002f});
  ty.overline = style_from(s["overline"], {text::Face::SemiBold, 11, 14, 0.06f});
  ty.mono = style_from(s["mono"], {text::Face::Mono, 13, 18, 0.0f});
  ty.format_label = style_from(s["formatLabel"], {text::Face::MonoMedium, 11, 14, 0.04f});
  return t;
}

Color Theme::category(std::string_view id) const {
  auto it = categories.find(id);
  if (it == categories.end()) it = categories.find("other");
  if (it == categories.end()) return color.text_secondary;
  return it->second;
}

std::vector<std::pair<float, Color>> Theme::flux() const {
  return {{0.0f, color.gradient_start}, {0.5f, color.gradient_mid}, {1.0f, color.gradient_end}};
}

}  // namespace cvd::ui
