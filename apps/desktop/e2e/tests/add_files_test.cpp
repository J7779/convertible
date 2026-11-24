// Adding files: command line, drag and drop anywhere on the window (with the
// overlay), the in-house file browser and the native-dialog test seam.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <set>

#include "harness.hpp"
#include "support/test.hpp"

using namespace e2e;

namespace {

std::set<std::string> names_of(const Json& state) {
  std::set<std::string> out;
  for (const Json& it : state["items"].items()) out.insert(it["name"].str());
  return out;
}

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

TEST_CASE("e2e/add-files/command line files are queued with detected-format chips") {
  Session s("add-cmdline");
  RunOptions o;
  o.files = {s.fixture("hello.txt"), s.write_input("data.json", R"({"a": [1, 2, 3]})"),
             s.write_input("blob.zzq", pseudo_random(300, 7))};
  REQUIRE(s.ok(s.run("add_cmdline.json", {}, o)));
  const Json st = s.state("queue.json");
  REQUIRE(st["items"].size() == 3u);
  CHECK(names_of(st) == std::set<std::string>{"hello.txt", "data.json", "blob.zzq"});
  for (const Json& it : st["items"].items()) {
    INFO(it["name"].str());
    CHECK(it["status"].str() == "ready");
    CHECK(it["targetIds"].size() > 0u);
    CHECK_FALSE(it["target"].str().empty());
    // Recognised formats show the catalogue's first extension as the chip;
    // unknown files fall back to their own extension and still get the universal targets.
    const std::string& source = it["source"].str();
    if (source.empty()) {
      CHECK(it["sourceLabel"].str() == "ZZQ");
      bool has_base64 = false;
      for (const Json& t : it["targetIds"].items()) has_base64 = has_base64 || t.str() == "base64";
      CHECK(has_base64);
    } else {
      const Json* format = nullptr;
      for (const Json& f : st["catalog"].items()) {
        if (f["id"].str() == source) format = &f;
      }
      REQUIRE(format != nullptr);
      const std::string ext = (*format)["extensions"].size() ? (*format)["extensions"][0].str() : source;
      CHECK(it["sourceLabel"].str() == upper(ext));
      CHECK(it["category"].str() == (*format)["category"].str());
    }
    // Each file has a card in the accessibility tree.
    CHECK(node(st, it["name"].str(), "group") != nullptr);
  }
  CHECK(at(st, "items.0.source").str() == "txt");
  CHECK(at(st, "items.1.source").str() == "json");
  CHECK(node(st, "3 files ready", "label") != nullptr);
}

TEST_CASE("e2e/add-files/dropping anywhere on the window shows the overlay and adds the files") {
  Session s("add-drop");
  s.write_input("one.txt", "first file\n");
  s.write_input("two.txt", "second file\n");
  s.write_input("three.json", "{\"three\": 3}\n");
  REQUIRE(s.ok(s.run("add_drop.json")));

  const Json overlay = s.state("overlay.json");
  CHECK(overlay["dropOverlay"].as_bool());
  const Json* zone = node(overlay, "Drop to add files", "group");
  REQUIRE(zone != nullptr);
  // The overlay covers the window: an accent outline 20 pt inside every edge.
  const Image img = s.shot("overlay.bmp");
  REQUIRE(img.valid());
  const Rgb accent = token_colour(true, "accent");
  const int h = img.height;
  const int w = img.width;
  CHECK(count_colour(img, {18, h / 4, 5, h / 2}, accent, 40) > h / 4);
  CHECK(count_colour(img, {w - 23, h / 4, 5, h / 2}, accent, 40) > h / 4);
  CHECK(ink(img, box_from((*zone)["rect"], 1.0), token_colour(true, "background")) > 0.01);

  CHECK_FALSE(s.state("left.json")["dropOverlay"].as_bool());
  const Json dropped = s.state("dropped.json");
  CHECK_FALSE(dropped["dropOverlay"].as_bool());
  CHECK(names_of(dropped) == std::set<std::string>{"one.txt", "two.txt"});
  // A second drop on the queue (over the action bar) adds the new file once.
  const Json second = s.state("second.json");
  CHECK(second["items"].size() == 3u);
  CHECK(names_of(second) == std::set<std::string>{"one.txt", "two.txt", "three.json"});
  CHECK_FALSE(second["dropOverlay"].as_bool());
}

TEST_CASE("e2e/add-files/the in-house file browser opens files by path and selection") {
  Session s("add-browser");
  s.write_input("alpha.txt", "alpha\n");
  s.write_input("beta.txt", "beta\n");
  s.write_input("gamma.txt", "gamma\n");
  make_dirs(s.in("sub"));
  REQUIRE(s.ok(s.run("add_browser.json")));

  const Json home = s.state("browser-home.json");
  CHECK(home["browser"]["open"].as_bool());
  CHECK(home["browser"]["mode"].str() == "open");
  const Json in = s.state("browser-in.json");
  CHECK(same_path(in["browser"]["directory"].str(), s.in()));
  // Folders first, then files by name.
  const Json& entries = in["browser"]["entries"];
  REQUIRE(entries.size() == 4u);
  CHECK(entries[0].str() == "sub");
  CHECK(entries[1].str() == "alpha.txt");
  const Json opened = s.state("opened.json");
  CHECK_FALSE(opened["browser"]["open"].as_bool());
  CHECK(names_of(opened) == std::set<std::string>{"alpha.txt", "beta.txt"});
  const Json cancelled = s.state("cancelled.json");
  CHECK_FALSE(cancelled["browser"]["open"].as_bool());
  CHECK(cancelled["items"].size() == 2u);
}

TEST_CASE("e2e/add-files/the native dialog seam adds files, and a cancelled dialog adds nothing") {
  Session s("add-seam");
  s.write_input("alpha.txt", "alpha\n");
  s.write_input("beta.txt", "beta\n");
  s.write_input("three.json", "{}\n");
  REQUIRE(s.ok(s.run("add_seam.json")));
  const Json chosen = s.state("chosen.json");
  CHECK(names_of(chosen) == std::set<std::string>{"alpha.txt", "beta.txt"});
  REQUIRE(chosen["automation"]["dialogs"].size() == 1u);
  CHECK(at(chosen, "automation.dialogs.0.kind").str() == "open");
  CHECK_FALSE(chosen["browser"]["open"].as_bool());  // the seam replaced every dialog
  CHECK(s.state("cancelled.json")["items"].size() == 2u);
  const Json shortcut = s.state("shortcut.json");
  CHECK(shortcut["items"].size() == 3u);
  CHECK(shortcut["automation"]["dialogs"].size() == 3u);
}

TEST_CASE("e2e/add-files/folders and missing files are refused with a message") {
  Session s("add-rejects");
  make_dirs(s.in("folder"));
  REQUIRE(s.ok(s.run("add_rejects.json")));
  const Json st = s.state("rejected.json");
  CHECK(st["items"].size() == 0u);
  REQUIRE(st["toasts"].size() == 2u);
  CHECK(st["toasts"][0].str().find("folder") != std::string::npos);
  CHECK(st["toasts"][1].str().find("missing.txt") != std::string::npos);
  CHECK(node(st, st["toasts"][0].str(), "label") != nullptr);
}
