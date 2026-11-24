// Every screen and overlay (scripts/tour.json) at window sizes from the
// minimum to large, at 1x / 1.5x / 2x and with larger text: no clipped or
// overlapping controls (from the layout rectangles in the state dump), the
// design tokens at known pixels in both themes, text that renders with no
// missing glyphs, and an accessibility tree that names every control.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <set>

#include "common.hpp"

using namespace e2e;

namespace {

struct Window {
  int width;
  int height;
  double scale;
  int text_percent;
};

std::string tag(const Window& w) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%dx%d-s%g-t%d", w.width, w.height, w.scale, w.text_percent);
  return buf;
}

Rgb lerp(Rgb a, Rgb b, double t) {
  return {static_cast<int>(std::lround(a.r + (b.r - a.r) * t)), static_cast<int>(std::lround(a.g + (b.g - a.g) * t)),
          static_cast<int>(std::lround(a.b + (b.b - a.b) * t))};
}

}  // namespace

TEST_CASE("e2e/layout/no clipped or overlapping controls from the minimum window to large, at 1x to 2x") {
  const std::vector<Window> windows = {
      {520, 460, 1.0, 100},  {800, 600, 1.0, 100},  {1120, 760, 1.0, 100}, {1440, 900, 1.0, 100},
      {1920, 1200, 1.0, 100}, {1120, 760, 1.5, 100}, {1120, 760, 2.0, 100}, {520, 460, 2.0, 100},
      {800, 600, 1.0, 130},  {1120, 760, 1.0, 130}};
  for (const Window& w : windows) {
    Session s("layout-" + tag(w));
    write_text(join(s.config_dir(), "settings.json"), "{\"textScale\": " + std::to_string(w.text_percent) + "}");
    RunOptions o;
    o.width = w.width;
    o.height = w.height;
    o.scale = w.scale;
    run_tour(s, o);
    for (const auto& stop : tour_stops()) {
      INFO(tag(w), " ", stop);
      const Json st = s.state(stop + ".json");
      REQUIRE(st.is_object());
      const double zoom = w.text_percent / 100.0;
      CHECK(std::fabs(at(st, "window.width").as_number() - w.width / zoom) < 1.0);
      CHECK(std::fabs(at(st, "window.height").as_number() - w.height / zoom) < 1.0);
      CHECK(at(st, "window.scale").as_number() == w.scale);
      const Image img = s.shot(stop + ".bmp");
      CHECK(img.width == static_cast<int>(std::lround(w.width * w.scale)));
      CHECK(img.height == static_cast<int>(std::lround(w.height * w.scale)));
      const auto problems = layout_problems(st);
      for (const auto& p : problems) FAIL_CHECK(p);
      if (!problems.empty()) s.keep_failure(s.out(stop + ".bmp"), stop);
      // The primary actions of each screen are on screen.
      if (stop == "home-empty") CHECK(node(st, "Choose files", "button") != nullptr);
      if (stop == "queue") {
        CHECK(node(st, "Convert all", "button") != nullptr);
        CHECK(node(st, "Add files", "button") != nullptr);
      }
      if (stop == "browser") CHECK(node(st, "Cancel", "button") != nullptr);
    }
  }
}

TEST_CASE("e2e/themes/light and dark themes paint the design tokens") {
  for (const bool dark : {true, false}) {
    INFO(dark ? "dark" : "light");
    Session s(std::string("themes-") + (dark ? "dark" : "light"));
    write_text(join(s.config_dir(), "settings.json"), dark ? R"({"theme": "dark"})" : R"({"theme": "light"})");
    run_tour(s, {});
    const Json empty = s.state("home-empty.json");
    CHECK(empty["theme"].str() == (dark ? "dark" : "light"));
    const Image home = s.shot("home-empty.bmp");
    REQUIRE(home.valid());
    const Rgb bg = token_colour(dark, "background");
    CHECK(colour_distance(home.at(home.width - 3, home.height - 3), bg) <= 2);
    CHECK(colour_distance(home.at(3, home.height - 3), bg) <= 2);
    // The primary button is filled with the accent token.
    const Json* choose = node(empty, "Choose files", "button");
    REQUIRE(choose != nullptr);
    const Box cb = box_from((*choose)["rect"], 1.0);
    CHECK(colour_distance(home.at(cb.x + 6, cb.y + cb.h / 2), token_colour(dark, "accentFill")) <= 3);
    // Its label is drawn in the on-accent colour.
    CHECK(count_colour(home, cb, token_colour(dark, "onAccent"), 40) > 20);

    const Json queue = s.state("queue.json");
    const Image q = s.shot("queue.bmp");
    CHECK(colour_distance(q.at(q.width - 3, q.height - 3), bg) <= 2);
    const Json* card = node(queue, "a.txt", "group");
    REQUIRE(card != nullptr);
    const Box b = box_from((*card)["rect"], 1.0);
    const Rgb surface = token_colour(dark, "surface");
    const Rgb card_fill = dark ? lerp(surface, token_colour(true, "surfaceElevated"), 0.35) : surface;
    CHECK(colour_distance(q.at(b.x + 6, b.y + b.h / 2), card_fill) <= 3);
    // File names use the primary text colour.
    CHECK(count_colour(q, {b.x + 70, b.y + 10, b.w / 3, 30}, token_colour(dark, "textPrimary"), 30) > 15);
    if (colour_distance(q.at(b.x + 6, b.y + b.h / 2), card_fill) > 3) s.keep_failure(s.out("queue.bmp"), "card-fill");
  }
}

TEST_CASE("e2e/text/every screen renders its text with no missing glyphs") {
  Session s("text-tour");
  run_tour(s, {});
  for (const auto& stop : tour_stops()) {
    INFO(stop);
    const Json st = s.state(stop + ".json");
    CHECK(at(st, "glyphs.shaped").as_number() > 50.0);
    CHECK(at(st, "glyphs.missing").as_number() == 0.0);
    if (at(st, "glyphs.missing").as_number() != 0.0) MESSAGE("missing code points: ", at(st, "glyphs.missingCodepoints").dump());
    // Every button with a visible label has glyph coverage inside its box.
    const Image img = s.shot(stop + ".bmp");
    for (const Json* b : nodes_with_role(st, "button")) {
      const std::string& label = (*b)["label"].str();
      // Buttons of the screen in front (not under a modal scrim).
      const bool in_front = (stop == "home-empty" && label == "Choose files") || (stop == "queue" && label == "Convert all") ||
                            (stop == "settings" && label == "Open-source licences") || (stop == "browser" && label == "Cancel");
      if (in_front) {
        const Box box = box_from((*b)["rect"], 1.0);
        const Rgb fill = img.at(box.x + 4, box.y + box.h / 2);
        INFO(label);
        CHECK(ink(img, {box.x + 8, box.y + 4, box.w - 16, box.h - 8}, fill, 60) > 0.03);
      }
    }
  }
  // The detector itself: Latin text from several scripts shapes completely,
  // a script the bundled fonts do not cover is reported.
  Session g("text-glyphs");
  REQUIRE(g.ok(g.run("text_glyphs.json")));
  const Json latin = g.state("latin.json");
  CHECK(at(latin, "formats.query").str().size() > 20u);
  CHECK(at(latin, "glyphs.missing").as_number() == 0.0);
  const Json cjk = g.state("cjk.json");
  CHECK(at(cjk, "glyphs.missing").as_number() >= 2.0);
  bool reported = false;
  for (const Json& cp : at(cjk, "glyphs.missingCodepoints").items()) reported = reported || cp.as_number() == 0x6F22;
  CHECK(reported);
  const Image latin_img = g.shot("latin.bmp");
  const Json* field = node(latin, "Search formats", "textfield");
  REQUIRE(field != nullptr);
  const Box fb = box_from((*field)["rect"], 1.0);
  CHECK(ink(latin_img, {fb.x + 40, fb.y + 6, fb.w / 2, fb.h - 12}, latin_img.at(fb.x + fb.w - 60, fb.y + fb.h / 2), 60) > 0.03);
}

TEST_CASE("e2e/a11y/every control on every screen has a role and a name") {
  for (const bool dark : {true, false}) {
    Session s(std::string("a11y-") + (dark ? "dark" : "light"));
    write_text(join(s.config_dir(), "settings.json"), dark ? R"({"theme": "dark"})" : R"({"theme": "light"})");
    run_tour(s, {});
    static const std::set<std::string> roles = {"button", "toggle", "slider", "textfield", "tab", "menuitem",
                                                "listitem", "label", "group", "image", "progress", "link"};
    for (const auto& stop : tour_stops()) {
      INFO(stop, dark ? " dark" : " light");
      const Json st = s.state(stop + ".json");
      CHECK(at(st, "a11y.focusable").as_number() > 0.0);
      // Every focusable control registered an accessibility node with a name.
      const Json& undescribed = at(st, "a11y.undescribed");
      CHECK(undescribed.size() == 0u);
      if (undescribed.size() != 0u) MESSAGE("controls without a name at ", undescribed.dump());
      std::size_t interactive = 0;
      for (const Json& n : st["accessibility"].items()) {
        const std::string& role = n["role"].str();
        INFO(role, " '", n["label"].str(), "'");
        CHECK(roles.count(role) == 1u);
        if (interactive_role(role) || role == "listitem" || role == "group") CHECK_FALSE(n["label"].str().empty());
        if (role == "toggle") CHECK((n["value"].str() == "on" || n["value"].str() == "off"));
        if (role == "slider" || role == "progress") CHECK_FALSE(n["value"].str().empty());
        interactive += interactive_role(role) ? 1 : 0;
      }
      CHECK(interactive > 0u);
      if (stop == "queue") {
        // Cards on screen (scrolled-away ones have no rectangle) are named after the file and report its status.
        std::size_t cards = 0;
        for (const Json& it : st["items"].items()) {
          const Json* card = node(st, it["name"].str(), "group");
          if (card == nullptr) continue;
          ++cards;
          CHECK((*card)["value"].str() == it["status"].str());
        }
        CHECK(cards >= 2u);
      }
      if (stop == "picker") {
        for (const Json* m : nodes_with_role(st, "menuitem")) CHECK_FALSE((*m)["value"].str().empty());
      }
    }
  }
}
