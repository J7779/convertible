// The real OS window (Cocoa, Win32 or X11; Xvfb in CI): the same automation
// drives the platform layer's event loop and presentation. Opt-in with
// CVD_E2E_WINDOWED=1 because it opens a window on the desktop.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>

#include "common.hpp"

using namespace e2e;

TEST_CASE("e2e/window/a conversion end to end in the real window") {
  if (!config().windowed) SKIP("set CVD_E2E_WINDOWED=1 to drive the real OS window");
  Session s("window");
  const Bytes data = pseudo_random(3000, 5);
  s.write_input("hello.txt", "Hello, world!\n");
  s.write_input("data.bin", data);
  make_dirs(s.out("saved"));
  RunOptions o;
  o.headless = false;
  o.skip_intro = false;
  o.width = 900;
  o.height = 640;
  o.scale = 0.0;  // the display's own scale
  REQUIRE(s.ok(s.run("window.json", {}, o)));
  CHECK(s.state("w-start.json")["intro"].str() == "playing");
  const Json done = s.state("w-done.json");
  CHECK(at(done, "items.0.status").str() == "done");
  CHECK(at(done, "items.1.status").str() == "done");
  CHECK(read_text(s.out("saved/hello.b64")) == "SGVsbG8sIHdvcmxkIQo=\n");
  CHECK(hex_decode(read_text(s.out("saved/data.hex"))).value_or(Bytes{}) == data);
  // Open went to the shell seam, not to another app.
  REQUIRE(at(done, "automation.shell").size() == 1u);
  CHECK(at(done, "automation.shell.0.action").str() == "open");
  const Image img = s.shot("w-done.bmp");
  REQUIRE(img.valid());
  const double scale = at(done, "window.scale").as_number();
  CHECK(img.width == static_cast<int>(std::lround(at(done, "window.width").as_number() * scale)));
  CHECK(ink(img, {0, 0, img.width, img.height}, img.at(img.width - 2, img.height - 2)) > 0.05);
}
