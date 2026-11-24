// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Output must be byte-identical for identical input and options (core rule 2):
// every target the planner offers for a set of real files is converted twice
// and the two results compared. Uninitialised memory, iteration over unordered
// containers, clocks or random seeds in a writer show up here (run it under
// the asan preset as well).
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "engine/planner.hpp"
#include "engine/registry.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

struct Source {
  const char* fixture;
  const char* format;
};

// One small real file per kind of content.
constexpr Source sources[] = {
    {"detect/audio/tone.wav", "wav"},     {"detect/image/pattern.png", "png"}, {"detect/text/notes.md", "md"},
    {"detect/text/table.csv", "csv"},     {"detect/text/data.json", "json"},   {"mesh/obj/cube_blender.obj", "obj"},
    {"detect/text/drawing.svg", "svg"},
};

}  // namespace

TEST_CASE("determinism: every offered target gives identical bytes for identical input") {
  const Registry& registry = Registry::instance();
  std::size_t compared = 0;
  for (const Source& source : sources) {
    if (registry.find(source.format) == nullptr) continue;
    const Bytes input = test::fixture(source.fixture);
    const std::string name = std::string(source.fixture).substr(std::string(source.fixture).rfind('/') + 1);
    std::map<Category, int> per_category;
    for (const TargetInfo& target : list_targets(registry, source.format)) {
      // Rendered turntable videos of a mesh take seconds each in a Debug
      // build; the video encoders' determinism is covered by their families.
      if (target.format->category == Category::Video && source.format == std::string_view("obj")) continue;
      // The leading targets of every category (the list is ordered by what
      // users pick): every writer family is exercised while the test stays fast.
      if (++per_category[target.format->category] > 6) continue;
      INFO(source.format, " -> ", target.format->id);
      const test::Converted first = test::convert(input, source.format, target.format->id, "", name);
      if (!first.ok()) continue;  // whether every offered target works is checked by the integration tests
      const test::Converted second = test::convert(input, source.format, target.format->id, "", name);
      REQUIRE(second.ok());
      CHECK(first.data == second.data);
      ++compared;
    }
  }
  CHECK(compared > 100);
}
