// Cold start: the intro animation follows the storyboard on the virtual
// clock, ends on the static mark, skips on a key or a click, and reduced
// motion shows the static mark at once.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "harness.hpp"
#include "support/test.hpp"

using namespace e2e;

namespace {

/// The intro mark's box (accessibility node "Convertible", role image, on the modal layer).
Box intro_box(const Json& state, double scale) {
  for (const Json& n : state["accessibility"].items()) {
    if (n["role"].str() == "image" && n["label"].str() == "Convertible" && n["rect"][2].as_number() >= 90.0) {
      return box_from(n["rect"], scale);
    }
  }
  return {};
}

}  // namespace

TEST_CASE("e2e/intro/storyboard frames differ over time and end on the static mark") {
  Session s("intro-storyboard");
  RunOptions o;
  o.skip_intro = false;
  REQUIRE(s.ok(s.run("intro_storyboard.json", {}, o)));

  const Json first = s.state("intro-0000.json");
  CHECK(first["intro"].str() == "playing");
  CHECK(s.state("intro-1200.json")["intro"].str() == "playing");
  const Json end = s.state("intro-1800.json");
  CHECK(end["intro"].str() == "fading");
  CHECK(at(end, "introMs").as_number() >= 1800.0);
  CHECK(s.state("after.json")["intro"].str() == "done");

  const Box box = intro_box(first, 1.0);
  REQUIRE(box.w >= 96);
  const char* names[] = {"intro-0000.bmp", "intro-0200.bmp", "intro-0600.bmp", "intro-1200.bmp", "intro-1800.bmp"};
  std::vector<Image> frames;
  for (const char* n : names) {
    frames.push_back(s.shot(n));
    REQUIRE_MESSAGE(frames.back().valid(), n);
  }
  // Every sampled frame differs from every other one inside the mark's box.
  for (std::size_t i = 0; i < frames.size(); ++i) {
    for (std::size_t j = i + 1; j < frames.size(); ++j) {
      INFO(names[i], " vs ", names[j]);
      CHECK(diff_pixels(frames[i], frames[j], box, 8) > 50);
    }
  }
  // Mid-intro frames draw something; the background outside the mark is flat.
  const Rgb bg = token_colour(true, "background");
  CHECK(ink(frames[2], box, bg) > 0.02);
  CHECK(ink(frames[2], {0, 0, 40, 40}, bg, 2) == 0.0);

  // The last frame of the storyboard is exactly the static mark that reduced
  // motion shows from the first frame.
  Session r("intro-storyboard-reduced");
  write_text(join(r.config_dir(), "settings.json"), R"({"motion": "reduced"})");
  RunOptions ro;
  ro.skip_intro = false;
  REQUIRE(r.ok(r.run("intro_reduced.json", {}, ro)));
  const Image reduced = r.shot("reduced-0000.bmp");
  REQUIRE(reduced.valid());
  const int differing = diff_pixels(frames[4], reduced, box, 2);
  CHECK(differing == 0);
  if (differing != 0) s.keep_failure(s.out("intro-1800.bmp"), "end-vs-static");
}

TEST_CASE("e2e/intro/reduced motion shows the static mark at once") {
  Session s("intro-reduced");
  write_text(join(s.config_dir(), "settings.json"), R"({"motion": "reduced"})");
  RunOptions o;
  o.skip_intro = false;
  REQUIRE(s.ok(s.run("intro_reduced.json", {}, o)));
  const Json first = s.state("reduced-0000.json");
  CHECK(first["reducedMotion"].as_bool());
  CHECK(first["intro"].str() == "fading");  // no storyboard: straight to the fade of the static mark
  CHECK(s.state("reduced-0400.json")["intro"].str() == "done");
  const Image img = s.shot("reduced-0000.bmp");
  const Box box = intro_box(first, 1.0);
  REQUIRE(box.w > 0);
  // The full mark is there in the very first frame.
  CHECK(ink(img, box, token_colour(true, "background")) > 0.15);
}

TEST_CASE("e2e/intro/a key press or a click skips the intro and does nothing else") {
  for (const char* gesture : {"key", "click"}) {
    INFO("gesture ", gesture);
    Session s(std::string("intro-skip-") + gesture);
    RunOptions o;
    o.skip_intro = false;
    REQUIRE(s.ok(s.run("intro_skip.json", {{"GESTURE", gesture}}, o)));
    CHECK(s.state("before.json")["intro"].str() == "playing");
    const Json skipped = s.state("skipped.json");
    CHECK(skipped["intro"].str() != "playing");
    CHECK(skipped["introMs"].as_number() < 400.0);
    const Json after = s.state("after.json");
    CHECK(after["intro"].str() == "done");
    CHECK(after["screen"].str() == "home");
    CHECK_FALSE(after["browser"]["open"].as_bool());  // the gesture did not reach the drop zone
    CHECK(after["items"].size() == 0u);
    CHECK(node(after, "Choose files", "button") != nullptr);
  }
}
