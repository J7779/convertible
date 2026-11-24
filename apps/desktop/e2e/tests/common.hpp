// Helpers shared by the end-to-end test groups.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "harness.hpp"
#include "support/test.hpp"

namespace e2e {

/// Builds a script from JSON text (placeholders substituted) for run_inline.
inline Json script(const std::string& text, const Vars& vars = {}) {
  auto j = Json::parse(substitute(text, vars));
  REQUIRE_MESSAGE(j.has_value(), "bad inline script");
  return *j;
}

inline Json step(std::initializer_list<std::pair<const char*, Json>> fields) {
  Json j = Json::object();
  for (const auto& [k, v] : fields) j.set(k, v);
  return j;
}

/// What the app offers for one input: its state-dump item for every target
/// (option schemas, preset, compress target), asked through the running app.
struct Discovery {
  Json item;                       // the item as first queued (default target)
  std::map<std::string, Json> by_target;  // target id -> item dump with that target
  std::vector<std::string> targets;       // in the core's order
};

inline Discovery discover(Session& s, const std::string& input, const std::string& tag) {
  Json steps = Json::array();
  Json files = Json::array();
  files.push_back(input);
  steps.push_back(step({{"op", "open"}, {"files", files}, {"drop", false}}));
  steps.push_back(step({{"op", "frames"}, {"count", 1}}));
  steps.push_back(step({{"op", "dump"}, {"path", "discover-" + tag + "/first.json"}}));
  RunResult r = s.run_inline(steps);
  REQUIRE(s.ok(r));
  Discovery d;
  d.item = at(s.state("discover-" + tag + "/first.json"), "items.0");
  REQUIRE_MESSAGE(d.item.is_object(), "nothing queued for ", input);
  Json more = Json::array();
  more.push_back(steps[0]);
  more.push_back(steps[1]);
  for (const Json& t : d.item["targetIds"].items()) {
    d.targets.push_back(t.str());
    more.push_back(step({{"op", "setTarget"}, {"index", 0}, {"target", t.str()}}));
    more.push_back(step({{"op", "dump"}, {"path", "discover-" + tag + "/t-" + t.str() + ".json"}}));
  }
  REQUIRE(s.ok(s.run_inline(more)));
  for (const auto& t : d.targets) d.by_target[t] = at(s.state("discover-" + tag + "/t-" + t + ".json"), "items.0");
  return d;
}

inline bool contains(const Json& arr, const std::string& v) {
  for (const Json& e : arr.items()) {
    if (e.str() == v) return true;
  }
  return false;
}

/// The stops of scripts/tour.json (each leaves <stop>.bmp and <stop>.json).
inline const std::vector<std::string>& tour_stops() {
  static const std::vector<std::string> stops = {"home-empty", "queue",    "picker",   "formats",
                                                 "settings",   "licenses", "browser"};
  return stops;
}

/// Runs the tour of every screen with the given window options.
inline void run_tour(Session& s, const RunOptions& options) {
  s.write_input("a.txt", "Alpha text file.\n");
  s.write_input("b.txt", "Beta text file.\n");
  s.write_input("c.json", "{\"gamma\": [1, 2, 3]}\n");
  s.write_input("d.bin", pseudo_random(4096, 3));
  REQUIRE(s.ok(s.run("tour.json", {}, options)));
}

/// Saved file bytes (fails the check when missing).
inline Bytes saved(const Session& s, const std::string& rel) {
  const auto b = read_bytes(s.out(rel));
  CHECK_MESSAGE(b.has_value(), "missing output ", rel);
  return b.value_or(Bytes{});
}

}  // namespace e2e
