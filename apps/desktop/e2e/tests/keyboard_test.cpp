// Keyboard only: the main flow (open through the file browser, pick a
// target, convert, save) without a single click, focus kept inside modal
// sheets, and a visible focus ring in the screenshot.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <set>

#include "common.hpp"

using namespace e2e;

TEST_CASE("e2e/keyboard/the main flow works with the keyboard alone and shows the focus ring") {
  Session s("keyboard");
  s.write_input("alpha.txt", "Typed by keyboard only.\n");
  s.write_input("beta.txt", "Not chosen.\n");
  make_dirs(s.out("saved"));
  REQUIRE(s.ok(s.run("keyboard.json")));

  // Focus stays inside the file browser while it is open.
  const Json cycle = s.state("kb-cycle.json");
  REQUIRE(cycle.size() == 14u);
  const std::set<std::string> browser_controls = {"Enclosing folder", "Home folder", "Folder", "Open", "Cancel",
                                                  "Show hidden files", "alpha.txt", "beta.txt"};
  for (const Json& label : cycle.items()) {
    INFO("focused while the browser is open: '", label.str(), "'");
    CHECK(browser_controls.count(label.str()) == 1u);
  }
  const Json browser = s.state("kb-browser.json");
  CHECK(same_path(browser["browser"]["directory"].str(), s.in()));
  CHECK(browser["focusVisible"].as_bool());

  const Json queued = s.state("kb-queued.json");
  REQUIRE(queued["items"].size() == 1u);
  CHECK(at(queued, "items.0.name").str() == "alpha.txt");

  // The focused target button carries a focus ring in the theme's colour.
  const Json focus = s.state("kb-focus.json");
  const Json* button = node(focus, "Target format for alpha.txt", "button");
  REQUIRE(button != nullptr);
  CHECK((*button)["focused"].as_bool());
  const Box b = box_from((*button)["rect"], 1.0);
  const Rgb ring = token_colour(true, "focusRing");
  const Image img = s.shot("kb-focus.bmp");
  const Image before = s.shot("kb-start.bmp");
  REQUIRE(img.valid());
  // The ring is drawn 2 px outside the control, 2 px wide: sample the band left of it.
  const Box band{b.x - 6, b.y + b.h / 4, 5, b.h / 2};
  const int ring_px = count_colour(img, band, ring, 48);
  CHECK(ring_px >= b.h / 2);
  if (ring_px < b.h / 2) s.keep_failure(s.out("kb-focus.bmp"), "focus-ring");
  CHECK(count_colour(before, band, ring, 48) == 0);

  const Json target = s.state("kb-target.json");
  CHECK_FALSE(target["picker"]["open"].as_bool());
  CHECK(at(target, "items.0.target").str() == "hex");
  CHECK(at(s.state("kb-done.json"), "items.0.status").str() == "done");
  const Json after = s.state("kb-saved.json");
  CHECK_FALSE(after["browser"]["open"].as_bool());
  CHECK(read_text(s.out("saved/alpha.hex")) == hex_lower(reinterpret_cast<const std::uint8_t*>("Typed by keyboard only.\n"), 24));
}
