// Options rendered from the core's schemas: a slider, a switch, a menu and a
// text field are found through the running app (never a hard-coded list),
// operated with the mouse and must change the converted file. Presets and
// the Compress toggle are exercised wherever the catalogue offers them.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <optional>

#include "common.hpp"

using namespace e2e;

namespace {

struct Pick {
  std::string target;
  Json spec;
};

/// Targets (in the core's order) whose schema has an option of `type` usable
/// for the test; basic options come before advanced ones.
std::vector<Pick> find_options(const Discovery& d, const std::string& type) {
  std::vector<Pick> out;
  for (const bool advanced : {false, true}) {
    for (const auto& t : d.targets) {
      for (const Json& spec : d.by_target.at(t)["optionSpecs"].items()) {
        if (spec["type"].str() != type || spec["advanced"].as_bool() != advanced) continue;
        if (spec["key"].str() == "preset") continue;
        if (type == "int" && spec["control"].str() != "slider") continue;  // wide ranges are steppers
        if (type == "enum" && spec["choices"].size() < 2) continue;
        out.push_back(Pick{t, spec});
      }
    }
  }
  return out;
}

const std::string kText =
    "Convertible end-to-end option test.\nThe quick brown fox jumps over the lazy dog 0123456789.\n"
    "Lines of text, long enough to wrap at a narrow width and to show encodings: abc def ghi jkl mno pqr stu.\n";

struct OptionRun {
  Json set;   // state after operating the control
  Json done;  // state after converting and saving
  Bytes changed;
  Bytes reference;
};

/// Queues the same text twice with `target`, operates the control on the
/// first card only, converts both and saves them.
OptionRun run_control(Session& s, const Pick& pick, const Json& interaction) {
  const std::string a = s.write_input("changed.txt", kText);
  const std::string b = s.write_input("reference.txt", kText);
  Json steps = Json::array();
  Json files = Json::array();
  files.push_back(a);
  files.push_back(b);
  steps.push_back(step({{"op", "open"}, {"files", files}}));
  steps.push_back(step({{"op", "setTarget"}, {"index", 0}, {"target", pick.target}}));
  steps.push_back(step({{"op", "setTarget"}, {"index", 1}, {"target", pick.target}}));
  steps.push_back(step({{"op", "frames"}, {"count", 30}}));
  if (pick.spec["advanced"].as_bool()) {
    steps.push_back(step({{"op", "click"}, {"label", "Advanced"}, {"exact", true}, {"nth", 0}}));
    steps.push_back(step({{"op", "frames"}, {"count", 3}}));
  }
  for (const Json& st : interaction.items()) steps.push_back(st);
  steps.push_back(step({{"op", "frames"}, {"count", 3}}));
  steps.push_back(step({{"op", "dump"}, {"path", "set.json"}}));
  steps.push_back(step({{"op", "screenshot"}, {"path", "set.bmp"}}));
  steps.push_back(step({{"op", "convertAll"}}));
  steps.push_back(step({{"op", "waitIdle"}}));
  steps.push_back(step({{"op", "saveAll"}, {"dir", "saved"}, {"expect", 2}}));
  steps.push_back(step({{"op", "dump"}, {"path", "done.json"}}));
  REQUIRE(s.ok(s.run_inline(steps)));
  OptionRun r;
  r.set = s.state("set.json");
  r.done = s.state("done.json");
  REQUIRE(at(r.done, "items.0.status").str() == "done");
  REQUIRE(at(r.done, "items.1.status").str() == "done");
  r.changed = saved(s, "saved/" + at(r.done, "items.0.outputName").str());
  r.reference = saved(s, "saved/" + at(r.done, "items.1.outputName").str());
  return r;
}

Discovery discover_text(Session& s) { return discover(s, s.write_input("probe.txt", kText), "text"); }

/// Some options do not change every input (e.g. a source encoding of pure
/// ASCII text). Tries the candidates in order until operating the control
/// changes the converted file, then hands that run to `verify`.
template <class MakeInteraction, class Verify>
void exercise(const std::string& name, const std::string& type, MakeInteraction make, Verify verify) {
  Session probe(name);
  const Discovery d = discover_text(probe);
  const auto picks = find_options(d, type);
  if (picks.empty()) SKIP("no text target offers a ", type, " option");
  // Many options leave plain text unchanged (an image quality for a text
  // document, say), and with the full catalogue they come first: try more.
  int attempts = 0;
  for (const Pick& pick : picks) {
    if (++attempts > 40) break;
    Session s(name + "-" + std::to_string(attempts));
    const Json interaction = make(pick);
    const OptionRun r = run_control(s, pick, interaction);
    if (r.changed == r.reference) {
      MESSAGE("operating ", pick.target, "/", pick.spec["key"].str(), " left this input unchanged; trying the next");
      continue;
    }
    INFO("target ", pick.target, " option ", pick.spec["key"].str());
    verify(s, pick, r);
    return;
  }
  FAIL_CHECK("no ", type, " option changed the output in ", attempts, " attempts");
}

}  // namespace

TEST_CASE("e2e/options/a slider from the schema changes the output") {
  exercise(
      "options-slider", "int",
      [](const Pick& pick) {
        const double mn = pick.spec["min"].as_number();
        const double mx = pick.spec["max"].as_number();
        const double frac = (pick.spec["default"].as_number() - mn) / std::max(1.0, mx - mn);
        Json steps = Json::array();
        steps.push_back(step({{"op", "click"}, {"label", pick.spec["label"].str()}, {"role", "slider"}, {"nth", 0},
                              {"dx", frac < 0.5 ? 0.8 : 0.1}}));
        return steps;
      },
      [](Session&, const Pick& pick, const OptionRun& r) {
        const std::string key = pick.spec["key"].str();
        const double v = at(r.set, "items.0.options." + key).as_number();
        const double def = pick.spec["default"].as_number();
        CHECK(v != def);
        CHECK(v >= pick.spec["min"].as_number());
        CHECK(v <= pick.spec["max"].as_number());
        CHECK(v == std::round(v));  // integers stay integers
        CHECK(at(r.set, "items.1.options." + key).as_number() == def);
        // The slider's accessible value follows the option.
        const Json* sl = node(r.set, pick.spec["label"].str(), "slider");
        REQUIRE(sl != nullptr);
        CHECK((*sl)["value"].str() == std::to_string(static_cast<long long>(v)));
        if (pick.target == "base64" && key == "line_width") {
          // Lines are at most the chosen width and the content still decodes to the input.
          const std::string out = to_string(r.changed);
          for (std::size_t start = 0; start < out.size();) {
            std::size_t nl = out.find('\n', start);
            if (nl == std::string::npos) nl = out.size();
            CHECK(nl - start <= static_cast<std::size_t>(v));
            start = nl + 1;
          }
          CHECK(base64_decode(out).value_or(Bytes{}) == to_bytes(kText));
        }
      });
}

TEST_CASE("e2e/options/a wide numeric range is a stepper: typed values and +/- steps") {
  // Ranges too wide to drag through (a sample rate of 0..384000, a bit rate
  // of 0..2000) are number fields with - and + buttons.
  Session probe("options-stepper-probe");
  const Discovery d = discover_text(probe);
  std::optional<Pick> pick;
  for (const auto& t : d.targets) {
    for (const Json& spec : d.by_target.at(t)["optionSpecs"].items()) {
      if (!pick && spec["type"].str() == "int" && spec["control"].str() == "stepper" && spec.contains("min") &&
          spec.contains("max") && !spec["advanced"].as_bool()) {
        pick = Pick{t, spec};
      }
    }
  }
  if (!pick) SKIP("no text target offers a basic integer option with a wide range");
  INFO("target ", pick->target, " option ", pick->spec["key"].str());
  const double mn = pick->spec["min"].as_number();
  const double mx = pick->spec["max"].as_number();
  const long long typed = static_cast<long long>(std::floor(mn + (mx - mn) * 0.37));
  Session s("options-stepper");
  const std::string a = s.write_input("stepper.txt", kText);
  const std::string label = pick->spec["label"].str();
  Json files = Json::array();
  files.push_back(a);
  Json steps = Json::array();
  steps.push_back(step({{"op", "open"}, {"files", files}}));
  steps.push_back(step({{"op", "setTarget"}, {"index", 0}, {"target", pick->target}}));
  steps.push_back(step({{"op", "frames"}, {"count", 30}}));
  steps.push_back(step({{"op", "click"}, {"label", label}, {"role", "textfield"}, {"exact", true}, {"nth", 0}}));
  steps.push_back(step({{"op", "key"}, {"key", "A"}, {"primary", true}}));
  steps.push_back(step({{"op", "type"}, {"text", std::to_string(typed)}}));
  steps.push_back(step({{"op", "frames"}, {"count", 3}}));
  steps.push_back(step({{"op", "dump"}, {"path", "typed.json"}}));
  steps.push_back(step({{"op", "click"}, {"label", "Increase " + label}, {"exact", true}, {"nth", 0}}));
  steps.push_back(step({{"op", "frames"}, {"count", 3}}));
  steps.push_back(step({{"op", "dump"}, {"path", "stepped.json"}}));
  REQUIRE(s.ok(s.run_inline(steps)));
  const std::string key = pick->spec["key"].str();
  const Json typed_state = s.state("typed.json");
  CHECK(at(typed_state, "items.0.options." + key).as_number() == static_cast<double>(typed));
  CHECK(node(typed_state, label, "slider") == nullptr);
  const double stepped = at(s.state("stepped.json"), "items.0.options." + key).as_number();
  CHECK(stepped > static_cast<double>(typed));
  CHECK(stepped <= mx);
}

TEST_CASE("e2e/options/a switch from the schema changes the output") {
  exercise(
      "options-switch", "bool",
      [](const Pick& pick) {
        Json steps = Json::array();
        steps.push_back(step({{"op", "click"}, {"label", pick.spec["label"].str()}, {"role", "toggle"}, {"nth", 0}}));
        return steps;
      },
      [](Session&, const Pick& pick, const OptionRun& r) {
        const std::string key = pick.spec["key"].str();
        const bool def = pick.spec["default"].as_bool();
        CHECK(at(r.set, "items.0.options." + key).as_bool() == !def);
        CHECK(at(r.set, "items.1.options." + key).as_bool() == def);
        const Json* sw = node(r.set, pick.spec["label"].str(), "toggle");
        REQUIRE(sw != nullptr);
        CHECK((*sw)["value"].str() == (def ? "off" : "on"));
        if (pick.target == "base64" && key == "padding") {
          CHECK(to_string(r.changed).find('=') == std::string::npos);
          CHECK(base64_decode(to_string(r.changed)).value_or(Bytes{}) == to_bytes(kText));
        }
      });
}

TEST_CASE("e2e/options/a menu from the schema changes the output") {
  exercise(
      "options-menu", "enum",
      [](const Pick& pick) {
        // The neighbour of the default choice, chosen with the keyboard: long
        // menus scroll, and the opened menu shows the current choice.
        const Json& choices = pick.spec["choices"];
        int at_default = 0;
        for (std::size_t k = 0; k < choices.size(); ++k) {
          if (choices[k] == pick.spec["default"]) at_default = static_cast<int>(k);
        }
        const bool down = at_default + 1 < static_cast<int>(choices.size());
        Json steps = Json::array();
        steps.push_back(
            step({{"op", "click"}, {"label", pick.spec["label"].str()}, {"role", "button"}, {"exact", true}, {"nth", 0}}));
        steps.push_back(step({{"op", "frames"}, {"count", 12}}));
        steps.push_back(step({{"op", "dump"}, {"path", "menu-open.json"}}));
        steps.push_back(step({{"op", "key"}, {"key", down ? "Down" : "Up"}}));
        steps.push_back(step({{"op", "key"}, {"key", "Enter"}}));
        return steps;
      },
      [](Session& s, const Pick& pick, const OptionRun& r) {
        const std::string key = pick.spec["key"].str();
        const Json& choices = pick.spec["choices"];
        const Json menu = s.state("menu-open.json");
        CHECK(menu["menu"]["open"].as_bool());
        CHECK(menu["menu"]["choices"].size() == choices.size());
        // The menu button is named after the option and reports the choice.
        const Json* button = node(menu, pick.spec["label"].str(), "button");
        REQUIRE(button != nullptr);
        CHECK_FALSE((*button)["value"].str().empty());
        CHECK_FALSE(r.set["menu"]["open"].as_bool());
        CHECK(contains(choices, at(r.set, "items.0.options." + key).str()));
        CHECK_FALSE(at(r.set, "items.0.options." + key) == pick.spec["default"]);
        CHECK(at(r.set, "items.1.options." + key) == pick.spec["default"]);
      });
}

TEST_CASE("e2e/options/a text field from the schema changes the output") {
  exercise(
      "options-text", "string",
      [](const Pick& pick) {
        Json steps = Json::array();
        steps.push_back(step({{"op", "click"}, {"label", pick.spec["label"].str()}, {"role", "textfield"}, {"nth", 0}}));
        // Replace whatever default the field holds.
        steps.push_back(step({{"op", "key"}, {"key", "A"}, {"primary", true}}));
        steps.push_back(step({{"op", "type"}, {"text", "e2e_blob"}}));
        return steps;
      },
      [](Session&, const Pick& pick, const OptionRun& r) {
        const std::string key = pick.spec["key"].str();
        CHECK(at(r.set, "items.0.options." + key).str() == "e2e_blob");
        CHECK(at(r.set, "items.1.options." + key) == pick.spec["default"]);
        CHECK(to_string(r.changed).find("e2e_blob") != std::string::npos);
      });
}

TEST_CASE("e2e/options/quality presets and the Compress toggle where the catalogue offers them") {
  Session s("options-presets");
  // Inputs the suite makes itself; whatever the catalogue offers for them is used.
  struct Candidate {
    std::string name;
    Bytes data;
  };
  const std::vector<Candidate> candidates = {
      {"photo.png", make_png(96, 72, 11)}, {"tone.wav", make_wav(8000, 0.5, 440.0)}, {"text.txt", to_bytes(kText)}};
  std::string preset_input, preset_target, compress_input, compress_target;
  for (const auto& c : candidates) {
    const std::string path = s.write_input(c.name, c.data);
    const Discovery d = discover(s, path, c.name);
    if (compress_input.empty() && !d.item["compressTarget"].str().empty()) {
      compress_input = c.name;
      compress_target = d.item["compressTarget"].str();
    }
    for (const auto& t : d.targets) {
      if (preset_input.empty() && d.by_target.at(t)["hasPreset"].as_bool()) {
        preset_input = c.name;
        preset_target = t;
      }
    }
  }
  bool exercised = false;
  if (!preset_input.empty()) {
    exercised = true;
    INFO("preset: ", preset_input, " -> ", preset_target);
    s.write_input("copy-" + preset_input, read_bytes(s.in(preset_input)).value_or(Bytes{}));
    const Json steps = script(R"JS([
      {"op": "open", "files": ["${IN}/${A}", "${IN}/copy-${A}"]},
      {"op": "setTarget", "index": 0, "target": "${T}"},
      {"op": "setTarget", "index": 1, "target": "${T}"},
      {"op": "frames", "count": 30},
      {"op": "click", "label": "Quality preset", "role": "group", "nth": 0, "dx": 0.17},
      {"op": "click", "label": "Quality preset", "role": "group", "nth": 1, "dx": 0.83},
      {"op": "frames", "count": 3},
      {"op": "dump", "path": "preset-set.json"},
      {"op": "convertAll"}, {"op": "waitIdle"},
      {"op": "saveAll", "dir": "preset", "expect": 2},
      {"op": "dump", "path": "preset-done.json"}
    ])JS", {{"A", preset_input}, {"T", preset_target}, {"IN", s.in()}});
    REQUIRE(s.ok(s.run_inline(steps)));
    const Json set = s.state("preset-set.json");
    CHECK(at(set, "items.0.options.preset").str() == "high");
    CHECK(at(set, "items.1.options.preset").str() == "small");
    const Json* seg = node(set, "Quality preset", "group");
    REQUIRE(seg != nullptr);
    CHECK((*seg)["value"].str() == "High");
    const Json done = s.state("preset-done.json");
    const double high = at(done, "items.0.outputSize").as_number();
    const double small = at(done, "items.1.outputSize").as_number();
    CHECK(small > 0.0);
    CHECK(small <= high);
  }
  if (!compress_input.empty()) {
    exercised = true;
    INFO("compress: ", compress_input, " -> ", compress_target);
    const Json steps = script(R"JS([
      {"op": "open", "files": ["${IN}/${A}"]},
      {"op": "frames", "count": 30},
      {"op": "click", "label": "Compress (same format, smaller)"},
      {"op": "frames", "count": 3},
      {"op": "dump", "path": "compress-set.json"},
      {"op": "convertAll"}, {"op": "waitIdle"},
      {"op": "saveAll", "dir": "compressed", "expect": 1},
      {"op": "dump", "path": "compress-done.json"}
    ])JS", {{"A", compress_input}, {"IN", s.in()}});
    REQUIRE(s.ok(s.run_inline(steps)));
    const Json set = s.state("compress-set.json");
    CHECK(at(set, "items.0.target").str() == compress_target);
    bool toggled = false;
    for (const Json& n : set["accessibility"].items()) toggled = toggled || n["label"].str().rfind("Compressing to a smaller", 0) == 0;
    CHECK(toggled);
    const Json done = s.state("compress-done.json");
    CHECK(at(done, "items.0.status").str() == "done");
    const std::string name = at(done, "items.0.outputName").str();
    CHECK(name.find("(compressed)") != std::string::npos);
    CHECK(exists(s.out("compressed/" + name)));
  }
  if (!exercised) SKIP("the catalogue offers no preset or same-format compression for the generated inputs");
}
