// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Text (TrueType parsing, shaping, layout) and UI toolkit tests: layout,
// hit-testing, focus/keyboard, text editing, animation, tokens.
#include <cmath>

#include "base/resources.hpp"
#include "support/test.hpp"
#include "text/text.hpp"
#include "ui/anim.hpp"
#include "ui/theme.hpp"
#include "ui/ui.hpp"
#include "ui/widgets.hpp"

using namespace cvd;

namespace {

void setup() {
  REQUIRE(install_bundled_fonts());
  REQUIRE(ui::Tokens::instance().load(resource_text("brand/tokens.json")));
}

text::Style body() { return {text::Face::Regular, 15.0f, 22.0f, 0.0f}; }

}  // namespace

TEST_CASE("desktop/font: bundled fonts parse with metrics, cmap and outlines") {
  setup();
  for (int f = 0; f < text::kFaceCount; ++f) {
    const text::Font& font = text::FontSet::instance().font(static_cast<text::Face>(f));
    REQUIRE(font.valid());
    CHECK(font.units_per_em() >= 1000);
    CHECK(font.ascender() > 0);
    CHECK(font.descender() < 0);
    const auto g = font.glyph_index('A');
    CHECK(g != 0);
    CHECK(font.advance(g) > 0);
    CHECK_FALSE(font.outline(g).empty());
    CHECK(font.outline(font.glyph_index(' ')).empty());
    CHECK(font.glyph_index(0xE9) != 0);    // é (composite glyph)
    CHECK_FALSE(font.outline(font.glyph_index(0xE9)).empty());
  }
}

TEST_CASE("desktop/font: GPOS kerning and tabular figures in Inter") {
  setup();
  const text::Font& inter = text::FontSet::instance().font(text::Face::Regular);
  const int kav = inter.kerning(inter.glyph_index('A'), inter.glyph_index('V'));
  CHECK(kav < 0);  // "AV" is a classic negative pair
  CHECK(inter.kerning(inter.glyph_index('H'), inter.glyph_index('H')) == 0);
  // Tabular figures: every digit has the same advance after "tnum".
  const int a1 = inter.advance(inter.substitute(inter.glyph_index('1'), "tnum"));
  const int a8 = inter.advance(inter.substitute(inter.glyph_index('8'), "tnum"));
  CHECK(a1 == a8);
  text::Style tab = body();
  tab.tabular = true;
  CHECK_APPROX(text::measure("111", tab), text::measure("888", tab), 0.01f);
  // Kerning makes "AV" narrower than the sum of its glyphs.
  CHECK(text::measure("AV", body()) < text::measure("A", body()) + text::measure("V", body()));
}

TEST_CASE("desktop/text: measurement, truncation, wrapping and hit-testing") {
  setup();
  const text::Style st = body();
  CHECK(text::measure("", st) == 0.0f);
  CHECK(text::measure("abc", st) > text::measure("ab", st));
  text::Style big = st;
  big.size = 30.0f;
  CHECK_APPROX(text::measure("Convertible", big), 2.0f * text::measure("Convertible", st), 0.05f);

  const std::string name = "quarterly-report-final-version-2.csv";
  const std::string mid = text::ellipsize_middle(name, st, 140.0f);
  CHECK(text::measure(mid, st) <= 140.0f);
  CHECK(mid.find("\xE2\x80\xA6") != std::string::npos);
  CHECK(mid.size() >= 4u);
  CHECK(mid.substr(mid.size() - 4) == ".csv");  // the extension stays visible
  const std::string end = text::ellipsize_end(name, st, 100.0f);
  CHECK(text::measure(end, st) <= 100.0f);
  CHECK(text::ellipsize_end("short", st, 500.0f) == "short");

  const auto lines = text::wrap("the quick brown fox jumps over the lazy dog", st, 120.0f);
  CHECK(lines.size() >= 3u);
  for (const auto& l : lines) CHECK(text::measure(l, st) <= 120.5f);
  CHECK(text::wrap("a b c d e f g h", st, 20.0f, 2).size() == 2u);

  const std::string s = "h\xC3\xA9llo";
  CHECK(text::hit_test(s, st, -5.0f) == 0u);
  CHECK(text::hit_test(s, st, 1000.0f) == s.size());
  for (std::size_t i : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{4}}) {
    CHECK(text::hit_test(s, st, text::caret_x(s, st, i) + 0.1f) == i);
  }
  CHECK(text::baseline_offset(st) > 0.0f);
  CHECK(text::baseline_offset(st) < st.line_height);
}

TEST_CASE("desktop/text: glyph cache renders coverage masks") {
  setup();
  const auto& inter = text::FontSet::instance().font(text::Face::Regular);
  const gfx::Mask& m = text::GlyphCache::instance().get(text::Face::Regular, inter.glyph_index('O'), 20.0f, 0);
  CHECK(m.width > 5);
  CHECK(m.height > 10);
  CHECK(m.top < 0);  // above the baseline
  int full = 0;
  for (const auto a : m.alpha) full += a == 255 ? 1 : 0;
  CHECK(full > 10);
  gfx::Bitmap bmp(80, 30);
  gfx::Canvas c(bmp);
  text::draw_text(c, "Hi", body(), {4.0f, 20.0f}, Color(1, 1, 1, 1));
  int lit = 0;
  for (const auto p : bmp.pixels) lit += (p >> 24) > 0 ? 1 : 0;
  CHECK(lit > 20);
}

TEST_CASE("desktop/anim: cubic-bezier easing and springs") {
  const ui::CubicBezier linear(0.0f, 0.0f, 1.0f, 1.0f);
  CHECK(linear(0.3f) == 0.3f);
  const ui::CubicBezier ease(0.25f, 0.1f, 0.25f, 1.0f);  // CSS "ease"
  CHECK_APPROX(ease(0.5f), 0.8024f, 0.002f);
  CHECK(ease(0.0f) == 0.0f);
  CHECK(ease(1.0f) == 1.0f);
  const ui::CubicBezier overshoot(0.34f, 1.56f, 0.64f, 1.0f);
  float peak = 0.0f;
  for (int i = 0; i <= 100; ++i) peak = std::max(peak, overshoot(static_cast<float>(i) / 100.0f));
  CHECK(peak > 1.05f);  // overshoots before settling
  ui::Spring s;
  s.target = 1.0f;
  int steps = 0;
  while (s.step(1.0f / 60.0f) && steps < 600) ++steps;
  CHECK(steps < 120);
  CHECK(s.value == 1.0f);
  ui::Track track({{0.0f, 0.0f, true, ui::CubicBezier(0.0f, 0.0f, 1.0f, 1.0f)}, {100.0f, 10.0f, false, {}}});
  CHECK(track.value(-5.0f, 3.0f) == 0.0f);
  CHECK(track.value(50.0f, 3.0f) == 5.0f);
  CHECK(track.value(500.0f, 3.0f) == 10.0f);
  ui::Track hold({{0.0f, 1.0f, false, {}}, {10.0f, 2.0f, false, {}}});
  CHECK(hold.value(5.0f, 0.0f) == 1.0f);
}

TEST_CASE("desktop/theme: tokens resolve for both appearances") {
  setup();
  const ui::Theme dark = ui::Theme::build(ui::Tokens::instance(), true);
  const ui::Theme light = ui::Theme::build(ui::Tokens::instance(), false);
  CHECK(dark.color.background == Color::hex("#0D0F16"));
  CHECK(light.color.background == Color::hex("#F6F6FA"));
  CHECK(dark.category("image") == Color::hex("#F88FCC"));
  CHECK(light.category("image") == Color::hex("#A8347F"));
  CHECK(dark.category("unknown-kind") == dark.category("other"));
  CHECK(dark.radius_lg == 16.0f);
  CHECK(dark.motion.base == 0.22f);
  CHECK(dark.type.body.size == 15.0f);
  CHECK(dark.type.overline.uppercase);
  CHECK(dark.type.format_label.face == text::Face::MonoMedium);
  // Text colours keep AA contrast on their backgrounds.
  for (const ui::Theme* t : {&dark, &light}) {
    CHECK(contrast_ratio(t->color.text_primary, t->color.background) >= 4.5f);
    CHECK(contrast_ratio(t->color.text_secondary, t->color.surface) >= 4.5f);
    CHECK(contrast_ratio(t->color.on_accent, t->color.accent_fill) >= 4.5f);
  }
}

TEST_CASE("desktop/layout: rect cutting and centring") {
  Rect r{0, 0, 100, 50};
  const Rect top = ui::cut_top(r, 10);
  CHECK(top == Rect(0, 0, 100, 10));
  CHECK(r == Rect(0, 10, 100, 40));
  const Rect right = ui::cut_right(r, 30);
  CHECK(right == Rect(70, 10, 30, 40));
  const Rect left = ui::cut_left(r, 200);  // clamps
  CHECK(left.w == 70.0f);
  CHECK(r.w == 0.0f);
  CHECK(ui::centered({0, 0, 100, 100}, 20, 10) == Rect(40, 45, 20, 10));
}

namespace {

/// Drives a ui::Context through frames without any platform.
struct Harness {
  ui::Context ctx;
  ui::Theme theme;
  double t = 0.0;
  std::string clipboard;

  Harness() {
    setup();
    theme = ui::Theme::build(ui::Tokens::instance(), true);
    ctx.host().get_clipboard = [this] { return clipboard; };
    ctx.host().set_clipboard = [this](const std::string& s) { clipboard = s; };
  }
  template <typename Fn>
  void frame(Fn&& build) {
    ctx.begin_frame({400, 300}, t, &theme);
    build();
    ctx.end_frame();
    t += 1.0 / 60.0;
  }
  void event(EventType type, Vec2 pos = {}, Key key = Key::None, Modifiers mods = {}) {
    Event e;
    e.type = type;
    e.pos = pos;
    e.key = key;
    e.mods = mods;
    ctx.handle_event(e);
  }
  void text(const std::string& s) {
    Event e;
    e.type = EventType::Text;
    e.text = s;
    ctx.handle_event(e);
  }
};

Modifiers primary() {
  Modifiers m;
#if defined(__APPLE__)
  m.super = true;
#else
  m.ctrl = true;
#endif
  return m;
}

}  // namespace

TEST_CASE("desktop/ui: hit-testing, clicks and the topmost layer") {
  Harness h;
  int clicks_a = 0, clicks_b = 0;
  const auto build = [&] {
    if (ui::button(h.ctx, h.ctx.id("a"), {10, 10, 100, 40}, "A")) ++clicks_a;
    h.ctx.set_layer(ui::Layer::Overlay);
    // An overlay panel covers the right half of button A.
    ui::panel(h.ctx, h.ctx.id("panel"), {60, 0, 100, 100}, 8.0f, 3);
    if (ui::button(h.ctx, h.ctx.id("b"), {70, 60, 60, 30}, "B")) ++clicks_b;
    h.ctx.set_layer(ui::Layer::Base);
  };
  h.frame(build);
  h.event(EventType::MouseMove, {30, 30});
  h.event(EventType::MouseDown, {30, 30});
  h.event(EventType::MouseUp, {30, 30});
  h.frame(build);
  CHECK(clicks_a == 1);
  // Over the panel: the base button underneath must not react.
  h.event(EventType::MouseMove, {80, 30});
  h.event(EventType::MouseDown, {80, 30});
  h.event(EventType::MouseUp, {80, 30});
  h.frame(build);
  CHECK(clicks_a == 1);
  h.event(EventType::MouseMove, {90, 70});
  h.event(EventType::MouseDown, {90, 70});
  h.event(EventType::MouseUp, {90, 70});
  h.frame(build);
  CHECK(clicks_b == 1);
  // Press inside, release outside: no click.
  h.event(EventType::MouseMove, {30, 30});
  h.frame(build);
  h.event(EventType::MouseDown, {30, 30});
  h.frame(build);
  h.event(EventType::MouseMove, {300, 250});
  h.event(EventType::MouseUp, {300, 250});
  h.frame(build);
  CHECK(clicks_a == 1);
}

TEST_CASE("desktop/ui: keyboard focus order, activation and focus ring visibility") {
  Harness h;
  int a = 0, b = 0;
  bool sw = false;
  const auto build = [&] {
    if (ui::button(h.ctx, h.ctx.id("a"), {10, 10, 80, 40}, "A")) ++a;
    if (ui::button(h.ctx, h.ctx.id("b"), {10, 60, 80, 40}, "B")) ++b;
    ui::toggle(h.ctx, h.ctx.id("s"), {10, 110, 50, 30}, sw, "Switch");
  };
  h.frame(build);
  h.event(EventType::KeyDown, {}, Key::Tab);
  h.frame(build);
  CHECK(h.ctx.focused() == h.ctx.id("a"));
  CHECK(h.ctx.focus_visible());
  h.event(EventType::KeyDown, {}, Key::Tab);
  h.event(EventType::KeyDown, {}, Key::Enter);
  h.frame(build);
  CHECK(h.ctx.focused() == h.ctx.id("b"));
  CHECK(b == 1);
  Modifiers shift;
  shift.shift = true;
  h.event(EventType::KeyDown, {}, Key::Tab, shift);
  h.event(EventType::KeyDown, {}, Key::Space);
  h.frame(build);
  CHECK(a == 1);
  // Wraps around backwards to the switch, which toggles with Space.
  h.event(EventType::KeyDown, {}, Key::Tab, shift);
  h.event(EventType::KeyDown, {}, Key::Space);
  h.frame(build);
  CHECK(sw);
  // The accessibility tree mirrors the widgets.
  bool found = false;
  for (const auto& n : h.ctx.access_nodes()) {
    if (n.label == "Switch") {
      found = true;
      CHECK(n.role == ui::Role::Toggle);
      CHECK(n.value == "on");
      CHECK(n.focused);
    }
  }
  CHECK(found);
  // A mouse click hides the focus ring again.
  h.event(EventType::MouseMove, {300, 280});
  h.event(EventType::MouseDown, {300, 280});
  h.event(EventType::MouseUp, {300, 280});
  h.frame(build);
  CHECK_FALSE(h.ctx.focus_visible());
}

TEST_CASE("desktop/ui: text field editing, selection and clipboard") {
  Harness h;
  std::string value = "hello";
  const auto build = [&] { ui::text_field(h.ctx, h.ctx.id("f"), {10, 10, 300, 40}, value, "Field"); };
  h.frame(build);
  h.event(EventType::MouseMove, {200, 30});
  h.event(EventType::MouseDown, {200, 30});
  h.event(EventType::MouseUp, {200, 30});
  h.frame(build);
  REQUIRE(h.ctx.focused() == h.ctx.id("f"));
  h.text(" world");
  h.frame(build);
  CHECK(value == "hello world");
  h.event(EventType::KeyDown, {}, Key::Backspace);
  h.event(EventType::KeyDown, {}, Key::Left);
  h.frame(build);
  CHECK(value == "hello worl");
  h.text("X");
  h.frame(build);
  CHECK(value == "hello worXl");
  // Select all, copy, type over, paste back.
  h.event(EventType::KeyDown, {}, Key::A, primary());
  h.event(EventType::KeyDown, {}, Key::C, primary());
  h.frame(build);
  CHECK(h.clipboard == "hello worXl");
  h.text("\xC3\xA9");
  h.frame(build);
  CHECK(value == "\xC3\xA9");
  h.event(EventType::KeyDown, {}, Key::Backspace);
  h.frame(build);
  CHECK(value.empty());
  h.clipboard = "pasted\ntext";
  h.event(EventType::KeyDown, {}, Key::V, primary());
  h.frame(build);
  CHECK(value == "pasted text");
  Modifiers shift;
  shift.shift = true;
  h.event(EventType::KeyDown, {}, Key::Home, shift);
  h.event(EventType::KeyDown, {}, Key::X, primary());
  h.frame(build);
  CHECK(value.empty());
  CHECK(h.clipboard == "pasted text");
}

TEST_CASE("desktop/ui: sliders, segmented controls and scroll views") {
  Harness h;
  double v = 50.0;
  int seg = 0;
  float offset = 0.0f;
  const std::vector<std::string> opts = {"High", "Balanced", "Small"};
  const auto build = [&] {
    ui::slider(h.ctx, h.ctx.id("sl"), {10, 10, 220, 30}, v, 0.0, 100.0, 1.0, "Quality");
    ui::segmented(h.ctx, h.ctx.id("seg"), {10, 50, 300, 36}, opts, seg, "Preset");
    offset = h.ctx.begin_scroll(h.ctx.id("scroll"), {10, 100, 200, 100}, 1000.0f);
    h.ctx.end_scroll();
  };
  h.frame(build);
  // Dragging the slider to the right end.
  h.event(EventType::MouseMove, {20, 25});
  h.event(EventType::MouseDown, {20, 25});
  h.frame(build);
  h.event(EventType::MouseMove, {400, 25});
  h.frame(build);
  CHECK(v == 100.0);
  h.event(EventType::MouseUp, {400, 25});
  h.frame(build);
  // Keyboard on the focused slider.
  h.event(EventType::KeyDown, {}, Key::Left);
  h.frame(build);
  CHECK(v == 99.0);
  // Segmented: click the third segment, then arrow back.
  h.event(EventType::MouseMove, {280, 68});
  h.event(EventType::MouseDown, {280, 68});
  h.event(EventType::MouseUp, {280, 68});
  h.frame(build);
  CHECK(seg == 2);
  h.event(EventType::KeyDown, {}, Key::Left);
  h.frame(build);
  CHECK(seg == 1);
  // Wheel scrolling over the scroll view settles at the requested offset.
  Event wheel;
  wheel.type = EventType::Scroll;
  wheel.pos = {50, 150};
  wheel.scroll = {0, 120};
  h.ctx.handle_event(wheel);
  for (int i = 0; i < 120; ++i) h.frame(build);
  CHECK(offset == 120.0f);
  wheel.scroll = {0, 5000};
  h.ctx.handle_event(wheel);
  for (int i = 0; i < 120; ++i) h.frame(build);
  CHECK(offset == 900.0f);  // clamped to content - viewport
}

TEST_CASE("desktop/ui: animations tween towards their targets") {
  Harness h;
  float v = 0.0f;
  float target = 0.0f;
  const auto build = [&] { v = h.ctx.animate(h.ctx.id("x"), target, 0.2f, ui::CubicBezier(0.2f, 0.0f, 0.0f, 1.0f)); };
  h.frame(build);
  CHECK(v == 0.0f);
  target = 1.0f;
  h.frame(build);
  CHECK(h.ctx.needs_frame());
  for (int i = 0; i < 6; ++i) h.frame(build);
  CHECK(v > 0.5f);
  CHECK(v < 1.0f);
  for (int i = 0; i < 20; ++i) h.frame(build);
  CHECK(v == 1.0f);
  h.ctx.reduced_motion = true;
  target = 0.0f;
  h.frame(build);
  CHECK(v == 0.0f);  // jumps without motion
}
