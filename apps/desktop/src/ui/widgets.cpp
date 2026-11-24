// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "ui/widgets.hpp"

#include <cmath>
#include <map>
#include <memory>

#include "base/text_util.hpp"

namespace cvd::ui {

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------

namespace {

struct IconDef {
  std::vector<std::string_view> paths;
  bool filled = false;
};

const IconDef& icon_def(Icon which) {
  static const std::map<Icon, IconDef> defs = {
      {Icon::Plus, {{"M12 5V19", "M5 12H19"}}},
      {Icon::Close, {{"M6.5 6.5L17.5 17.5", "M17.5 6.5L6.5 17.5"}}},
      {Icon::ChevronDown, {{"M6 9.5L12 15.5L18 9.5"}}},
      {Icon::ChevronUp, {{"M6 14.5L12 8.5L18 14.5"}}},
      {Icon::ChevronRight, {{"M9.5 6L15.5 12L9.5 18"}}},
      {Icon::ChevronLeft, {{"M14.5 6L8.5 12L14.5 18"}}},
      {Icon::Check, {{"M5 12.5L9.5 17L19 7.5"}}},
      {Icon::Search, {{"M17.5 11A6.5 6.5 0 1 1 4.5 11A6.5 6.5 0 1 1 17.5 11Z", "M15.8 15.8L20 20"}}},
      {Icon::File, {{"M14 3H7.5A1.5 1.5 0 0 0 6 4.5V19.5A1.5 1.5 0 0 0 7.5 21H16.5A1.5 1.5 0 0 0 18 19.5V7Z", "M14 3V7H18"}}},
      {Icon::Folder,
       {{"M3.5 7A1.5 1.5 0 0 1 5 5.5H9.5L11.5 7.5H19A1.5 1.5 0 0 1 20.5 9V17.5A1.5 1.5 0 0 1 19 19H5A1.5 1.5 0 0 1 3.5 17.5Z"}}},
      {Icon::Save, {{"M12 4V15", "M7.5 10.5L12 15L16.5 10.5", "M5 19.5H19"}}},
      {Icon::Reveal,
       {{"M13.5 4.5H19.5V10.5", "M19.5 4.5L11 13",
         "M17.5 14V18.5A1.5 1.5 0 0 1 16 20H5.5A1.5 1.5 0 0 1 4 18.5V8A1.5 1.5 0 0 1 5.5 6.5H10"}}},
      {Icon::Refresh, {{"M19.5 12A7.5 7.5 0 1 1 17.3 6.7", "M19.5 4V8.5H15"}}},
      {Icon::Settings, {{"M4 7.5H13", "M17 7.5H20", "M15 5V10", "M4 16.5H7", "M11 16.5H20", "M9 14V19"}}},
      {Icon::Grid,
       {{"M4.5 4.5H10V10H4.5Z", "M14 4.5H19.5V10H14Z", "M4.5 14H10V19.5H4.5Z", "M14 14H19.5V19.5H14Z"}}},
      {Icon::Warning, {{"M12 4L21 19.5H3Z", "M12 10V14", "M12 16.8V17"}}},
      {Icon::Info, {{"M21 12A9 9 0 1 1 3 12A9 9 0 1 1 21 12Z", "M12 11V16.5", "M12 7.8V8"}}},
      {Icon::Shield, {{"M12 3.5L19 6V11.5C19 16 16 19.2 12 20.5C8 19.2 5 16 5 11.5V6Z", "M9 12L11.2 14.2L15.2 10"}}},
      {Icon::Compress, {{"M4.5 14H10V19.5", "M19.5 10H14V4.5", "M14 10L20 4", "M4 20L10 14"}}},
      {Icon::Trash, {{"M4.5 7H19.5", "M9.5 7V4.5H14.5V7", "M6.5 7L7.5 20H16.5L17.5 7"}}},
      {Icon::ArrowRight, {{"M5 12H19", "M13 6L19 12L13 18"}}},
      {Icon::Up, {{"M12 19V5", "M6 11L12 5L18 11"}}},
      {Icon::Stop, {{"M8 8H16V16H8Z"}, true}},
      {Icon::Home, {{"M4 11L12 4.5L20 11", "M6.5 9V19.5H17.5V9"}}},
      {Icon::Sun,
       {{"M16 12A4 4 0 1 1 8 12A4 4 0 1 1 16 12Z", "M12 3V5", "M12 19V21", "M3 12H5", "M19 12H21", "M5.6 5.6L7 7",
         "M17 17L18.4 18.4", "M5.6 18.4L7 17", "M17 7L18.4 5.6"}}},
      {Icon::Moon, {{"M19.5 14.5A8 8 0 1 1 9.5 4.5A6.5 6.5 0 0 0 19.5 14.5Z"}}},
      {Icon::Monitor, {{"M3.5 5.5H20.5V16H3.5Z", "M9 20H15", "M12 16V20"}}},
  };
  static const IconDef empty;
  const auto it = defs.find(which);
  return it == defs.end() ? empty : it->second;
}

const std::vector<gfx::Path>& icon_paths(Icon which) {
  static std::map<Icon, std::vector<gfx::Path>> cache;
  auto it = cache.find(which);
  if (it != cache.end()) return it->second;
  std::vector<gfx::Path> paths;
  for (const auto d : icon_def(which).paths) {
    if (auto p = gfx::Path::parse_svg(d)) paths.push_back(std::move(*p));
  }
  return cache.emplace(which, std::move(paths)).first->second;
}

}  // namespace

void draw_icon(gfx::Canvas& canvas, Icon which, const Rect& r, Color color, float stroke) {
  if (which == Icon::None) return;
  const float s = std::min(r.w, r.h) / 24.0f;
  canvas.save();
  canvas.translate(r.x + (r.w - 24.0f * s) * 0.5f, r.y + (r.h - 24.0f * s) * 0.5f);
  canvas.scale(s, s);
  gfx::StrokeStyle st;
  st.width = stroke / s;
  st.cap = gfx::LineCap::Round;
  st.join = gfx::LineJoin::Round;
  const bool filled = icon_def(which).filled;
  for (const auto& p : icon_paths(which)) {
    if (filled) {
      gfx::Path rounded;
      rounded.add_rrect(p.bounds(), 2.0f);
      canvas.fill_path(rounded, color);
    } else {
      canvas.stroke_path(p, st, color);
    }
  }
  canvas.restore();
}

void icon(Context& ctx, Icon which, const Rect& r, Color color, float stroke) {
  if (which == Icon::None || color.a <= 0.0f) return;
  ctx.draw([which, r, color, stroke](gfx::Canvas& c) { draw_icon(c, which, r, color, stroke); });
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

namespace {
const text::Style& button_style(Context& ctx, const ButtonOptions& opt) {
  return opt.style ? *opt.style : ctx.theme().type.body_strong;
}
}  // namespace

float button_width(Context& ctx, std::string_view label, const ButtonOptions& opt) {
  const text::Style& st = button_style(ctx, opt);
  float w = text::measure(label, st) + (opt.kind == ButtonKind::Link ? 0.0f : 32.0f);
  if (opt.icon != Icon::None) w += label.empty() ? 20.0f : 28.0f;
  if (opt.trailing != Icon::None) w += 24.0f;
  return std::ceil(w);
}

bool button(Context& ctx, WidgetId wid, const Rect& r, std::string_view label, const ButtonOptions& opt) {
  const Theme& t = ctx.theme();
  const Interaction in = ctx.interact(wid, r, opt.enabled ? (kFocusable | kKeyboardActivate) : kDisabled);
  ctx.describe(wid, Role::Button, label.empty() ? opt.tooltip : label, r, {}, opt.selected, opt.enabled);
  if (in.hovered) ctx.set_cursor(Cursor::Hand);
  const float radius = opt.radius >= 0.0f ? opt.radius : t.radius_md;
  const float hover = ctx.animate(hash_id("hover", wid), in.hovered ? 1.0f : 0.0f, t.motion.fast, t.motion.standard);
  const float press = ctx.spring(hash_id("press", wid), in.pressed ? 1.0f : 0.0f, 700.0f, 40.0f);
  const Palette& c = t.color;

  Color bg{0, 0, 0, 0};
  Color fg = c.text_primary;
  Color border{0, 0, 0, 0};
  switch (opt.kind) {
    case ButtonKind::Primary:
      bg = Color::lerp(c.accent_fill, c.accent_fill_hover, hover);
      if (in.pressed) bg = c.accent_fill_pressed;
      fg = c.on_accent;
      break;
    case ButtonKind::Secondary:
      bg = Color::lerp(c.surface_elevated, t.dark ? Color::hex("#252A3C") : c.surface_sunken, hover);
      border = Color::lerp(c.border, c.border_strong, hover * 0.7f);
      break;
    case ButtonKind::Ghost:
      bg = (opt.selected ? c.accent_subtle : c.surface_elevated).fade(opt.selected ? 1.0f : hover);
      fg = opt.selected ? c.accent_text : Color::lerp(c.text_secondary, c.text_primary, hover);
      break;
    case ButtonKind::Danger:
      bg = c.error_subtle.fade(0.6f + 0.4f * hover);
      fg = c.error;
      break;
    case ButtonKind::Link:
      fg = Color::lerp(c.accent_text, c.text_primary, hover * 0.5f);
      break;
  }
  if (!opt.enabled) {
    fg = c.text_disabled;
    if (opt.kind == ButtonKind::Primary) bg = c.surface_elevated;
    border = c.border;
  }
  // Pressed buttons sink very slightly.
  const float k = 1.0f - 0.03f * press;
  const Rect vr{r.x + r.w * (1.0f - k) * 0.5f, r.y + r.h * (1.0f - k) * 0.5f, r.w * k, r.h * k};
  if (opt.kind == ButtonKind::Primary && opt.enabled) ctx.shadow(vr, radius, hover > 0.5f ? 2 : 1);
  ctx.fill(vr, radius, bg);
  if (border.a > 0.0f) ctx.stroke(vr, radius, 1.0f, border);

  const text::Style& st = button_style(ctx, opt);
  const float tw = text::measure(label, st);
  const float iw = opt.icon != Icon::None ? (label.empty() ? 20.0f : 28.0f) : 0.0f;
  const float trail = opt.trailing != Icon::None ? 24.0f : 0.0f;
  float x = opt.kind == ButtonKind::Link ? vr.x : vr.x + (vr.w - (tw + iw + trail)) * 0.5f;
  if (opt.icon != Icon::None) {
    icon(ctx, opt.icon, {x, vr.center().y - 10.0f, 20.0f, 20.0f}, fg);
    x += iw;
  }
  if (!label.empty()) {
    const float pad = opt.kind == ButtonKind::Link ? 0.0f : 16.0f;
    const std::string shown = text::ellipsize_end(label, st, std::max(10.0f, vr.w - iw - trail - pad + 0.5f));
    ctx.text_in(shown, st, {x, vr.y, tw, vr.h}, fg);
    x += std::min(tw, vr.w - iw - trail - pad);
  }
  if (opt.trailing != Icon::None) icon(ctx, opt.trailing, {x + 4.0f, vr.center().y - 9.0f, 18.0f, 18.0f}, fg);
  ctx.focus_ring(wid, r, radius);
  if (!opt.tooltip.empty()) ctx.tooltip(wid, opt.tooltip);
  return in.clicked;
}

bool icon_button(Context& ctx, WidgetId wid, const Rect& r, Icon which, std::string_view label, bool enabled,
                 Color tint) {
  const Theme& t = ctx.theme();
  const Interaction in = ctx.interact(wid, r, enabled ? (kFocusable | kKeyboardActivate) : kDisabled);
  ctx.describe(wid, Role::Button, label, r, {}, false, enabled);
  if (in.hovered) ctx.set_cursor(Cursor::Hand);
  const float hover = ctx.animate(hash_id("hover", wid), in.hovered ? 1.0f : 0.0f, t.motion.fast, t.motion.standard);
  const float radius = std::min(r.w, r.h) * 0.5f;
  Color bg = (t.dark ? Color::hex("#FFFFFF") : Color::hex("#12141C")).fade(0.07f * hover + (in.pressed ? 0.05f : 0.0f));
  ctx.fill(r, radius, bg);
  Color fg = tint.a > 0.0f ? tint : Color::lerp(t.color.text_secondary, t.color.text_primary, hover);
  if (!enabled) fg = t.color.text_disabled;
  const float is = std::min(20.0f, std::min(r.w, r.h) - 8.0f);
  icon(ctx, which, centered(r, is, is), fg);
  ctx.focus_ring(wid, r, radius);
  ctx.tooltip(wid, label);
  return in.clicked;
}

// ---------------------------------------------------------------------------
// Segmented control
// ---------------------------------------------------------------------------

bool segmented(Context& ctx, WidgetId wid, const Rect& r, std::span<const std::string> options, int& selected,
               std::string_view label) {
  const Theme& t = ctx.theme();
  if (options.empty()) return false;
  const int n = static_cast<int>(options.size());
  const int before = selected;
  const Interaction in = ctx.interact(wid, r, kFocusable);
  if (in.focused) {
    if (ctx.key_pressed(Key::Left) || ctx.key_pressed(Key::Up)) selected = std::max(0, selected - 1);
    if (ctx.key_pressed(Key::Right) || ctx.key_pressed(Key::Down)) selected = std::min(n - 1, selected + 1);
    if (ctx.key_pressed(Key::Home)) selected = 0;
    if (ctx.key_pressed(Key::End)) selected = n - 1;
  }
  const float seg_w = r.w / static_cast<float>(n);
  int hover_index = -1;
  if (in.hovered) {
    hover_index = std::clamp(static_cast<int>((ctx.mouse().x - ctx.current_translate().x - r.x) / seg_w), 0, n - 1);
    ctx.set_cursor(Cursor::Hand);
    if (in.clicked) selected = hover_index;
  }
  ctx.describe(wid, Role::Group, label, r, selected >= 0 && selected < n ? options[static_cast<std::size_t>(selected)] : "");
  const float radius = t.radius_md;
  ctx.fill(r, radius, t.color.surface_sunken);
  ctx.stroke(r, radius, 1.0f, t.color.border.fade(0.6f));
  const float pos = ctx.spring(hash_id("thumb", wid), static_cast<float>(std::clamp(selected, 0, n - 1)), 520.0f, 42.0f);
  const Rect thumb{r.x + 3.0f + pos * seg_w, r.y + 3.0f, seg_w - 6.0f, r.h - 6.0f};
  if (selected >= 0) {
    ctx.shadow(thumb, radius - 3.0f, 1);
    ctx.fill(thumb, radius - 3.0f, t.dark ? Color::hex("#2A2F44") : t.color.surface);
  }
  for (int i = 0; i < n; ++i) {
    const Rect seg{r.x + seg_w * static_cast<float>(i), r.y, seg_w, r.h};
    const bool sel = i == selected;
    const float hov = ctx.animate(hash_id("seg-hover", wid + static_cast<WidgetId>(i)), hover_index == i ? 1.0f : 0.0f,
                                  t.motion.fast, t.motion.standard);
    const Color fg = sel ? t.color.text_primary : Color::lerp(t.color.text_secondary, t.color.text_primary, hov);
    const text::Style& st = sel ? t.type.label : t.type.label.with_face(text::Face::Regular);
    const std::string shown = text::ellipsize_end(options[static_cast<std::size_t>(i)], st, seg.w - 12.0f);
    ctx.text_in(shown, st, seg, fg, 0.5f);
  }
  ctx.focus_ring(wid, r, radius);
  return selected != before;
}

// ---------------------------------------------------------------------------
// Switch
// ---------------------------------------------------------------------------

bool toggle(Context& ctx, WidgetId wid, const Rect& r, bool& value, std::string_view label) {
  const Theme& t = ctx.theme();
  const Interaction in = ctx.interact(wid, r);
  if (in.hovered) ctx.set_cursor(Cursor::Hand);
  bool changed = false;
  if (in.clicked) {
    value = !value;
    changed = true;
  }
  ctx.describe(wid, Role::Toggle, label, r, value ? "on" : "off", value);
  const float on = ctx.spring(hash_id("knob", wid), value ? 1.0f : 0.0f, 600.0f, 38.0f);
  const Rect track = centered(r, 42.0f, 26.0f);
  const Color off_col = t.dark ? Color::hex("#3A4058") : Color::hex("#C9CCD8");
  ctx.fill(track, 13.0f, Color::lerp(off_col, t.color.accent_fill, clampf(on, 0.0f, 1.0f)));
  const float knob_x = track.x + 3.0f + on * (track.w - 26.0f);
  const Rect knob{knob_x, track.y + 3.0f, 20.0f, 20.0f};
  ctx.shadow(knob, 10.0f, 1);
  ctx.fill(knob, 10.0f, Color::hex("#FFFFFF"));
  ctx.focus_ring(wid, track, 13.0f);
  return changed;
}

// ---------------------------------------------------------------------------
// Slider
// ---------------------------------------------------------------------------

bool slider(Context& ctx, WidgetId wid, const Rect& r, double& value, double min, double max, double step,
            std::string_view label) {
  const Theme& t = ctx.theme();
  const double before = value;
  const Interaction in = ctx.interact(wid, r, kFocusable);
  std::string val = step >= 1.0 ? std::to_string(static_cast<long long>(std::llround(value))) : std::to_string(value);
  ctx.describe(wid, Role::Slider, label, r, val);
  if (in.hovered) ctx.set_cursor(Cursor::Hand);
  const Rect track{r.x + 10.0f, r.center().y - 2.0f, r.w - 20.0f, 4.0f};
  const auto quantise = [&](double v) {
    if (step > 0.0) v = min + std::round((v - min) / step) * step;
    return std::clamp(v, min, max);
  };
  if (in.pressed) {
    const float mx = ctx.mouse().x - ctx.current_translate().x;
    const double f = std::clamp(static_cast<double>((mx - track.x) / std::max(1.0f, track.w)), 0.0, 1.0);
    value = quantise(min + f * (max - min));
  }
  if (in.focused) {
    const double big = std::max(step, (max - min) / 10.0);
    Modifiers m;
    if (ctx.key_pressed(Key::Left, m) || ctx.key_pressed(Key::Down, m)) value = quantise(value - (m.shift ? big : step));
    if (ctx.key_pressed(Key::Right, m) || ctx.key_pressed(Key::Up, m)) value = quantise(value + (m.shift ? big : step));
    if (ctx.key_pressed(Key::Home)) value = min;
    if (ctx.key_pressed(Key::End)) value = max;
    if (ctx.key_pressed(Key::PageDown)) value = quantise(value - big);
    if (ctx.key_pressed(Key::PageUp)) value = quantise(value + big);
  }
  const float frac = max > min ? static_cast<float>((value - min) / (max - min)) : 0.0f;
  const float shown = ctx.spring(hash_id("slider", wid), frac, 900.0f, 60.0f);
  ctx.fill(track, 2.0f, t.color.border);
  const Rect filled{track.x, track.y, track.w * clampf(shown, 0.0f, 1.0f), track.h};
  ctx.fill_paint(filled, 2.0f,
                 gfx::Paint::linear({track.x, 0.0f}, {track.right(), 0.0f},
                                    {{0.0f, t.color.gradient_start}, {0.5f, t.color.gradient_mid}, {1.0f, t.color.gradient_end}}));
  const float hover = ctx.animate(hash_id("hover", wid), (in.hovered || in.pressed) ? 1.0f : 0.0f, t.motion.fast,
                                  t.motion.standard);
  const float kr = 9.0f + 1.5f * hover;
  const Vec2 kc{track.x + track.w * clampf(shown, 0.0f, 1.0f), track.center().y};
  const Rect knob{kc.x - kr, kc.y - kr, kr * 2.0f, kr * 2.0f};
  ctx.shadow(knob, kr, 2);
  ctx.fill(knob, kr, Color::hex("#FFFFFF"));
  ctx.focus_ring(wid, knob, kr);
  return value != before;
}

// ---------------------------------------------------------------------------
// Text field
// ---------------------------------------------------------------------------

namespace {

bool is_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         (static_cast<unsigned char>(c) >= 0x80);
}

std::size_t word_left(const std::string& s, std::size_t pos) {
  while (pos > 0 && !is_word_char(s[pos - 1])) pos = utf8_prev(s, pos);
  while (pos > 0 && is_word_char(s[pos - 1])) pos = utf8_prev(s, pos);
  return pos;
}

std::size_t word_right(const std::string& s, std::size_t pos) {
  while (pos < s.size() && !is_word_char(s[pos])) utf8_next(s, pos);
  while (pos < s.size() && is_word_char(s[pos])) utf8_next(s, pos);
  return pos;
}

}  // namespace

bool text_field(Context& ctx, WidgetId wid, const Rect& r, std::string& value, std::string_view label,
                const TextFieldOptions& opt) {
  const Theme& t = ctx.theme();
  const text::Style& st = opt.monospace ? t.type.mono : t.type.body;
  Context::TextEdit& ed = ctx.text_edit(wid);
  const std::string before = value;
  const bool was_focused = ctx.focused() == wid;
  const Interaction in = ctx.interact(wid, r, kFocusable);
  ctx.describe(wid, Role::TextField, label, r, value);
  if (in.hovered) ctx.set_cursor(Cursor::Text);
  ed.caret = std::min(ed.caret, value.size());
  ed.anchor = std::min(ed.anchor, value.size());

  const float pad_l = opt.leading != Icon::None ? 38.0f : 12.0f;
  const bool show_clear = opt.clear_button && !value.empty();
  const float pad_r = show_clear ? 36.0f : 12.0f;
  const Rect inner{r.x + pad_l, r.y, std::max(10.0f, r.w - pad_l - pad_r), r.h};
  const Vec2 tr = ctx.current_translate();

  if (in.focused && !was_focused) {
    ed.blink_start = ctx.time();
    if (opt.select_all_on_focus) {
      ed.anchor = 0;
      ed.caret = value.size();
    } else {
      ed.caret = ed.anchor = value.size();
    }
  }
  // Mouse placement and drag selection.
  if (ctx.active() == wid && ctx.mouse_down()) {
    const float mx = ctx.mouse().x - tr.x - inner.x + ed.scroll_x;
    const std::size_t pos = text::hit_test(value, st, mx);
    if (ctx.mouse_pressed_this_frame()) {
      if (in.double_clicked) {
        ed.anchor = 0;
        ed.caret = value.size();
      } else {
        ed.caret = pos;
        if (!ctx.mods().shift) ed.anchor = pos;
      }
    } else {
      ed.caret = pos;
    }
    ed.blink_start = ctx.time();
  }

  if (in.focused) {
    const auto has_sel = [&] { return ed.caret != ed.anchor; };
    const auto sel_range = [&] { return std::pair{std::min(ed.caret, ed.anchor), std::max(ed.caret, ed.anchor)}; };
    const auto erase_sel = [&] {
      const auto [a, b] = sel_range();
      value.erase(a, b - a);
      ed.caret = ed.anchor = a;
    };
    // Editing keys, in the order they were pressed.
    for (auto& kp : ctx.key_presses()) {
      if (kp.consumed) continue;
      const Modifiers& m = kp.mods;
      bool handled = true;
      switch (kp.key) {
        case Key::Left: {
          std::size_t p = ed.caret;
          if (has_sel() && !m.shift) p = sel_range().first;
          else if (m.primary()) p = 0;
          else if (m.alt || m.ctrl) p = word_left(value, ed.caret);
          else p = utf8_prev(value, ed.caret);
          ed.caret = p;
          if (!m.shift) ed.anchor = p;
          break;
        }
        case Key::Right: {
          std::size_t p = ed.caret;
          if (has_sel() && !m.shift) {
            p = sel_range().second;
          } else if (m.primary()) {
            p = value.size();
          } else if (m.alt || m.ctrl) {
            p = word_right(value, ed.caret);
          } else if (p < value.size()) {
            utf8_next(value, p);
          }
          ed.caret = p;
          if (!m.shift) ed.anchor = p;
          break;
        }
        case Key::Home:
          ed.caret = 0;
          if (!m.shift) ed.anchor = 0;
          break;
        case Key::End:
          ed.caret = value.size();
          if (!m.shift) ed.anchor = ed.caret;
          break;
        case Key::Backspace:
          if (has_sel()) {
            erase_sel();
          } else if (ed.caret > 0) {
            const std::size_t p =
                (m.alt || m.ctrl) ? word_left(value, ed.caret) : (m.primary() ? 0 : utf8_prev(value, ed.caret));
            value.erase(p, ed.caret - p);
            ed.caret = ed.anchor = p;
          }
          break;
        case Key::Delete:
          if (has_sel()) {
            erase_sel();
          } else if (ed.caret < value.size()) {
            std::size_t p = ed.caret;
            utf8_next(value, p);
            value.erase(ed.caret, p - ed.caret);
          }
          break;
        case Key::A:
        case Key::C:
        case Key::X:
        case Key::V: {
          if (!m.primary()) {
            handled = false;
            break;
          }
          if (kp.key == Key::A) {
            ed.anchor = 0;
            ed.caret = value.size();
          } else if (kp.key == Key::C || kp.key == Key::X) {
            if (has_sel() && ctx.host().set_clipboard) {
              const auto [a, b] = sel_range();
              ctx.host().set_clipboard(value.substr(a, b - a));
              if (kp.key == Key::X) erase_sel();
            }
          } else if (ctx.host().get_clipboard) {
            const std::string clip = ctx.host().get_clipboard();
            std::string clean;
            for (const char c : clip) {
              if (c == '\n' || c == '\r' || c == '\t') clean += ' ';
              else if (static_cast<unsigned char>(c) >= 0x20) clean += c;
            }
            if (has_sel()) erase_sel();
            value.insert(ed.caret, clean);
            ed.caret += clean.size();
            ed.anchor = ed.caret;
          }
          break;
        }
        default: handled = false; break;
      }
      if (handled) kp.consumed = true;
    }
    // Typed characters.
    if (!ctx.typed_text().empty()) {
      std::string clean;
      for (const char c : ctx.typed_text()) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) clean += c;
      }
      ctx.consume_typed();
      if (!clean.empty()) {
        if (has_sel()) erase_sel();
        value.insert(ed.caret, clean);
        ed.caret += clean.size();
        ed.anchor = ed.caret;
      }
    }
    if (value != before) ed.blink_start = ctx.time();
  }

  // Keep the caret visible.
  const float caret_px = text::caret_x(value, st, ed.caret);
  if (caret_px - ed.scroll_x > inner.w - 2.0f) ed.scroll_x = caret_px - inner.w + 2.0f;
  if (caret_px - ed.scroll_x < 0.0f) ed.scroll_x = caret_px;
  const float total = text::measure(value, st);
  if (total - ed.scroll_x < inner.w - 2.0f) ed.scroll_x = std::max(0.0f, total - inner.w + 2.0f);

  // Drawing.
  const float focus = ctx.animate(hash_id("focus", wid), in.focused ? 1.0f : 0.0f, t.motion.fast, t.motion.standard);
  const float hover = ctx.animate(hash_id("hover", wid), in.hovered ? 1.0f : 0.0f, t.motion.fast, t.motion.standard);
  const float radius = t.radius_md;
  ctx.fill(r, radius, t.dark ? t.color.surface_sunken : t.color.surface);
  ctx.stroke(r, radius, 1.0f, Color::lerp(Color::lerp(t.color.border, t.color.border_strong, hover * 0.6f), t.color.accent, focus));
  if (focus > 0.01f) ctx.stroke(r.inset(-3.0f), radius + 3.0f, 3.0f, t.color.accent.fade(0.25f * focus));
  if (opt.leading != Icon::None) {
    icon(ctx, opt.leading, {r.x + 11.0f, r.center().y - 10.0f, 20.0f, 20.0f},
         Color::lerp(t.color.text_secondary, t.color.accent_text, focus));
  }
  ctx.push_clip(inner);
  const float base_y = inner.y + (inner.h - st.line_height) * 0.5f + text::baseline_offset(st);
  if (in.focused && ed.caret != ed.anchor) {
    const float x0 = text::caret_x(value, st, std::min(ed.caret, ed.anchor)) - ed.scroll_x;
    const float x1 = text::caret_x(value, st, std::max(ed.caret, ed.anchor)) - ed.scroll_x;
    ctx.fill({inner.x + x0, inner.center().y - st.line_height * 0.5f, x1 - x0, st.line_height}, 3.0f,
             t.color.accent.fade(t.dark ? 0.45f : 0.28f));
  }
  if (value.empty()) {
    ctx.text(opt.placeholder, st, {inner.x, base_y}, t.color.text_secondary.fade(0.85f));
  } else {
    ctx.text(value, st, {inner.x - ed.scroll_x, base_y}, t.color.text_primary);
  }
  if (in.focused) {
    const double phase = std::fmod(ctx.time() - ed.blink_start, 1.06);
    const bool visible = ctx.reduced_motion || phase < 0.53;
    if (visible) {
      ctx.fill({inner.x + caret_px - ed.scroll_x - 0.75f, inner.center().y - st.size * 0.62f, 1.5f, st.size * 1.24f}, 0.75f,
               t.color.accent_text);
    }
    if (!ctx.reduced_motion) ctx.request_frame_at(ctx.time() + (phase < 0.53 ? 0.53 - phase : 1.06 - phase) + 0.001);
  }
  ctx.pop_clip();
  if (show_clear) {
    const Rect cb{r.right() - 34.0f, r.center().y - 14.0f, 28.0f, 28.0f};
    if (icon_button(ctx, hash_id("clear", wid), cb, Icon::Close, "Clear")) {
      value.clear();
      ed.caret = ed.anchor = 0;
      ctx.set_focus(wid);
    }
  }
  return value != before;
}

// ---------------------------------------------------------------------------
// Disclosure, chips, panels, paragraphs
// ---------------------------------------------------------------------------

bool disclosure(Context& ctx, WidgetId wid, const Rect& r, std::string_view label, bool& open) {
  const Theme& t = ctx.theme();
  const Interaction in = ctx.interact(wid, r);
  ctx.describe(wid, Role::Button, label, r, open ? "expanded" : "collapsed", open);
  if (in.hovered) ctx.set_cursor(Cursor::Hand);
  bool toggled = false;
  if (in.clicked) {
    open = !open;
    toggled = true;
  }
  const float rot = ctx.spring(hash_id("chev", wid), open ? 1.0f : 0.0f, 500.0f, 36.0f);
  const float hover = ctx.animate(hash_id("hover", wid), in.hovered ? 1.0f : 0.0f, t.motion.fast, t.motion.standard);
  const Color fg = Color::lerp(t.color.text_secondary, t.color.text_primary, hover);
  const Rect ic{r.x, r.center().y - 9.0f, 18.0f, 18.0f};
  ctx.draw([ic, rot, fg](gfx::Canvas& c) {
    c.save();
    c.translate(ic.center().x, ic.center().y);
    c.rotate(rot * kPi * 0.5f);
    draw_icon(c, Icon::ChevronRight, {-9.0f, -9.0f, 18.0f, 18.0f}, fg);
    c.restore();
  });
  ctx.text_in(label, t.type.label, {r.x + 24.0f, r.y, r.w - 24.0f, r.h}, fg);
  ctx.focus_ring(wid, r, t.radius_sm);
  return toggled;
}

float chip_width(Context& ctx, std::string_view label) {
  return std::ceil(text::measure(label, ctx.theme().type.format_label) + 28.0f);
}

void chip(Context& ctx, const Rect& r, std::string_view label, Color tint, bool square_dot) {
  const Theme& t = ctx.theme();
  ctx.fill(r, r.h * 0.5f, tint.fade(t.dark ? 0.14f : 0.10f));
  ctx.stroke(r, r.h * 0.5f, 1.0f, tint.fade(t.dark ? 0.28f : 0.22f));
  const float d = 6.0f;
  const Rect dot{r.x + 10.0f, r.center().y - d * 0.5f, d, d};
  ctx.fill(dot, square_dot ? 1.0f : d * 0.5f, tint);
  ctx.text_in(label, t.type.format_label, {r.x + 21.0f, r.y, r.w - 25.0f, r.h}, tint);
}

void panel(Context& ctx, WidgetId wid, const Rect& r, float radius, int elevation) {
  const Theme& t = ctx.theme();
  ctx.interact(wid, r, kNone);  // swallows hover and clicks for what is below
  ctx.shadow(r, radius, elevation);
  ctx.fill(r, radius, t.color.surface_elevated);
  ctx.stroke(r, radius, 1.0f, t.dark ? t.color.border : t.color.border.fade(0.9f));
}

float paragraph(Context& ctx, std::string_view str, const text::Style& style, const Rect& r, Color c, int max_lines) {
  const auto lines = text::wrap(str, style, r.w, max_lines);
  float y = r.y;
  for (const auto& line : lines) {
    ctx.text(line, style, {r.x, y + text::baseline_offset(style)}, c);
    y += style.line_height;
  }
  return y - r.y;
}

}  // namespace cvd::ui
