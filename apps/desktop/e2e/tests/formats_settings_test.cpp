// The formats browser (counts, search, category filter) checked against the
// catalogue the app reports, and settings changed through the UI, persisted
// in an isolated configuration folder (CONVERTIBLE_CONFIG_DIR) and restored
// after a restart.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cctype>

#include "common.hpp"

using namespace e2e;

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool matches(const Json& f, const std::string& q) {
  const std::string ql = lower(q);
  if (lower(f["id"].str()).find(ql) != std::string::npos) return true;
  if (lower(f["name"].str()).find(ql) != std::string::npos) return true;
  if (lower(f["description"].str()).find(ql) != std::string::npos) return true;
  for (const Json& e : f["extensions"].items()) {
    if (lower(e.str()).rfind(ql, 0) == 0) return true;
  }
  return lower(f["category"].str()).find(ql) != std::string::npos;
}

Json catalogue(Session& s) {
  REQUIRE(s.ok(s.run_inline(script(R"JS([{"op": "dump", "path": "catalog.json", "catalog": true}])JS"))));
  return s.state("catalog.json")["catalog"];
}

}  // namespace

TEST_CASE("e2e/formats/the formats browser lists the catalogue and searches it") {
  Session s("formats");
  const Json cat = catalogue(s);
  REQUIRE(cat.size() > 0u);
  // Search for a format the catalogue has (PNG when present).
  std::string query = cat[0]["id"].str();
  for (const Json& f : cat.items()) {
    if (f["id"].str() == "png") query = "png";
  }
  REQUIRE(s.ok(s.run("formats.json", {{"QUERY", query}})));

  // The home screen's link counts the dynamic catalogue.
  const Json home = s.state("f-home.json");
  std::size_t writable = 0;
  for (const Json& f : cat.items()) writable += f["writable"].as_bool() ? 1 : 0;
  const std::string link = std::to_string(cat.size()) + " formats recognised \xC2\xB7 " + std::to_string(writable) + " can be created";
  CHECK(node(home, link, "button") != nullptr);

  const Json all = s.state("f-all.json");
  CHECK(all["screen"].str() == "formats");
  CHECK(at(all, "formats.total").as_number() == static_cast<double>(cat.size()));
  CHECK(at(all, "formats.matched").as_number() == static_cast<double>(cat.size()));

  const Json q = s.state("f-query.json");
  CHECK(at(q, "formats.query").str() == query);
  const auto expected = static_cast<double>(std::count_if(cat.items().begin(), cat.items().end(),
                                                          [&](const Json& f) { return matches(f, query); }));
  // The app also matches category names ("Images"); at least the direct matches are listed.
  CHECK(at(q, "formats.matched").as_number() >= expected);
  CHECK(at(q, "formats.matched").as_number() < static_cast<double>(cat.size()));
  const auto rows = nodes_with_role(q, "listitem");
  REQUIRE_FALSE(rows.empty());
  bool exact_hit = false;
  for (const Json* row : rows) {
    const Json* f = nullptr;
    for (const Json& c : cat.items()) {
      if (c["name"].str() == (*row)["label"].str()) f = &c;
    }
    INFO("listed: ", (*row)["label"].str());
    REQUIRE(f != nullptr);
    exact_hit = exact_hit || (*f)["id"].str() == query;
    // Read / write capability is announced.
    CHECK((*row)["value"].str().rfind((*f)["readable"].as_bool() ? "reads" : "detects", 0) == 0);
  }
  CHECK(exact_hit);
  CHECK(ink(s.shot("f-query.bmp"), box_from((*rows[0])["rect"], 1.0), token_colour(true, "background")) > 0.02);

  const Json none = s.state("f-none.json");
  CHECK(at(none, "formats.matched").as_number() == 0.0);
  CHECK(nodes_with_role(none, "listitem").empty());

  // The first category chip filters to that category, a second click clears it.
  const Json catf = s.state("f-category.json");
  const std::string category = at(catf, "formats.category").str();
  REQUIRE_FALSE(category.empty());
  const auto in_category = static_cast<double>(std::count_if(
      cat.items().begin(), cat.items().end(), [&](const Json& f) { return f["category"].str() == category; }));
  CHECK(at(catf, "formats.matched").as_number() == in_category);
  CHECK(at(s.state("f-category-off.json"), "formats.category").str().empty());
  CHECK(s.state("f-escape.json")["screen"].str() == "home");
}

TEST_CASE("e2e/settings/preferences persist across restarts in the isolated config folder") {
  Session s("settings");
  // An input with quality presets (when the catalogue offers one) shows the default preset.
  std::string preset_input = "plain.txt";
  std::string preset_target = "base64";
  s.write_input("plain.txt", "plain\n");
  {
    const std::string png = s.write_input("photo.png", make_png(64, 48, 5));
    const Discovery d = discover(s, png, "png");
    for (const auto& t : d.targets) {
      if (preset_input == "plain.txt" && d.by_target.at(t)["hasPreset"].as_bool()) {
        preset_input = "photo.png";
        preset_target = t;
      }
    }
  }
  remove_all(s.config_dir());
  make_dirs(s.config_dir());

  REQUIRE(s.ok(s.run("settings_change.json")));
  const Json before = s.state("set-0.json");
  CHECK(same_path(at(before, "configDir").str(), s.config_dir()));
  CHECK(at(before, "settings.theme").str() == "system");
  const Json changed = s.state("set-1.json");
  CHECK(at(changed, "settings.theme").str() == "light");
  CHECK(at(changed, "settings.motion").str() == "reduced");
  CHECK(at(changed, "settings.textScale").as_int() == 115);
  CHECK(at(changed, "settings.defaultPreset").str() == "small");
  CHECK_FALSE(at(changed, "settings.stripMetadata").as_bool());
  CHECK(changed["theme"].str() == "light");
  CHECK(changed["reducedMotion"].as_bool());
  CHECK(at(changed, "window.zoom").as_number() == static_cast<double>(1.15f));

  // Written to the folder named by CONVERTIBLE_CONFIG_DIR, nowhere else.
  const Json file = load_json(join(s.config_dir(), "settings.json"));
  CHECK(file["theme"].str() == "light");
  CHECK(file["motion"].str() == "reduced");
  CHECK(file["textScale"].as_int() == 115);
  CHECK(file["defaultPreset"].str() == "small");
  CHECK_FALSE(file["stripMetadata"].as_bool(true));
  CHECK(list_dir(s.config_dir()) == std::vector<std::string>{"settings.json"});

  // A new process reads them back (and plays the reduced-motion intro).
  RunOptions o;
  o.skip_intro = false;
  REQUIRE(s.ok(s.run("settings_check.json", {{"PRESET_INPUT", preset_input}, {"PRESET_TARGET", preset_target}}, o)));
  const Json first = s.state("restart-0.json");
  CHECK(first["intro"].str() == "fading");
  CHECK(first["theme"].str() == "light");
  CHECK(first["reducedMotion"].as_bool());
  CHECK(at(first, "settings.textScale").as_int() == 115);
  const Json later = s.state("restart-1.json");
  CHECK(at(later, "window.zoom").as_number() == static_cast<double>(1.15f));
  CHECK(at(later, "settings.defaultPreset").str() == "small");
  const Json& options = at(later, "items.0.options");
  if (options.contains("preset")) CHECK(options["preset"].str() == "small");
  if (options.contains("strip_metadata")) CHECK_FALSE(options["strip_metadata"].as_bool());
  // Light theme pixels: the window background is the light token.
  const Image img = s.shot("restart-1.bmp");
  REQUIRE(img.valid());
  CHECK(colour_distance(img.at(img.width - 3, img.height - 3), token_colour(false, "background")) <= 2);
}
