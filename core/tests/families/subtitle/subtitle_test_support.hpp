// Shared helpers for the subtitle family tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "families/subtitle/subtitle.hpp"
#include "ir/subtitles.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

namespace cvt::test::subs {

struct Expected {
  std::int64_t start_ms;
  std::int64_t end_ms;
  std::string_view text;  ///< IR text; compared with markup unless `plain` is set
};

/// The synthetic script used by most fixtures (see tests/data/subtitle/SOURCES.md).
inline const std::vector<Expected>& master_script() {
  static const std::vector<Expected> script = {
      {1000, 3500, "Hello, Ada Example."},
      {4000, 6250, "<i>Café déjà vu</i>\nsecond line"},
      {6500, 9000, "שלום עולם — مرحبا بالعالم"},
      {9100, 12345, "Emoji test 🎬 and <b>bold</b> words"},
      {12000, 14000, "Overlapping cue"},
      {15000, 20000, "This is a deliberately long subtitle line that keeps going well past forty-two characters"},
      {21000, 23000, "Positioned at the top"},
      {3601000, 3603500, "One hour later"},
  };
  return script;
}

/// Compare cues with the expectation: times within `tolerance_ms`, text equal
/// (markup-insensitive when `plain`).
inline void check_cues(const ir::Subtitles& subs, const std::vector<Expected>& expected, std::int64_t tolerance_ms,
                       bool plain, bool check_end = true) {
  REQUIRE(subs.cues.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    INFO("cue ", i);
    const ir::Cue& cue = subs.cues[i];
    CHECK(std::llabs(cue.start_ms - expected[i].start_ms) <= tolerance_ms);
    if (check_end) CHECK(std::llabs(cue.end_ms - expected[i].end_ms) <= tolerance_ms);
    if (plain) {
      CHECK(subtitle::strip_markup(cue.text) == subtitle::strip_markup(expected[i].text));
    } else {
      CHECK(cue.text == expected[i].text);
    }
  }
}

/// A clean, non-overlapping track with every feature most formats keep.
inline ir::Subtitles sample_track() {
  ir::Subtitles subs;
  const std::vector<Expected> cues = {
      {1000, 2500, "First line"},
      {3000, 5000, "<i>Two lines</i>\nwith ünïcödé"},
      {6000, 8000, "Ελληνικά and שלום 🎬"},
      {10000, 12000, "<b>Bold</b> & <u>under</u>"},
      {3601000, 3602000, "After an hour"},
  };
  for (const Expected& e : cues) {
    ir::Cue cue;
    cue.start_ms = e.start_ms;
    cue.end_ms = e.end_ms;
    cue.text = std::string(e.text);
    subs.cues.push_back(std::move(cue));
  }
  return subs;
}

inline std::vector<Expected> expected_of(const ir::Subtitles& subs) {
  std::vector<Expected> out;
  for (const ir::Cue& cue : subs.cues) out.push_back({cue.start_ms, cue.end_ms, cue.text});
  return out;
}

/// Run the registered bridge from Subtitles to `to` with options.
inline Result<AnyIR> run_bridge(Kind to, ir::Subtitles subs, std::string_view options_json = {}) {
  for (const Bridge& bridge : Registry::instance().bridges()) {
    if (bridge.from != Kind::Subtitles || bridge.to != to) continue;
    CVT_TRY_ASSIGN(const Options options, Options::parse(options_json));
    Context ctx;
    CVT_TRY_ASSIGN(ctx.options, options.normalized(bridge.options));
    ctx.registry = &Registry::instance();
    return bridge.fn(AnyIR(std::move(subs)), ctx);
  }
  return fail(Status::UnsupportedConversion, "no bridge");
}

}  // namespace cvt::test::subs
