// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/fakes.hpp"
#include "engine/planner.hpp"

using namespace cvt;
using test::fake_bridge;
using test::fake_codec;
using test::fake_format;

namespace {

std::vector<std::string> target_ids(const Registry& registry, std::string_view source) {
  std::vector<std::string> ids;
  for (const TargetInfo& target : list_targets(registry, source)) ids.push_back(target.format->id);
  return ids;
}

}  // namespace

TEST_CASE("planner: direct route through a shared kind") {
  Registry registry;
  registry.add_codec(fake_codec("png", Kind::Image, true, true, 10, 12));
  registry.add_codec(fake_codec("jpg", Kind::Image, true, true, 20, 25, true));
  registry.finalize();

  const auto plan = plan_conversion(registry, "png", "jpg");
  REQUIRE_OK(plan);
  CHECK(plan->source == registry.find("png"));
  CHECK(plan->target == registry.find("jpg"));
  REQUIRE(plan->reader != nullptr);
  CHECK(plan->reader_kind == Kind::Image);
  CHECK(plan->bridges.empty());
  CHECK(plan->writer->format_id == "jpg");
  CHECK(plan->cost == 10 + 25);
  CHECK(plan->lossy);
  CHECK(plan->describe() == "png -> Image -> jpg");

  const auto back = plan_conversion(registry, "jpg", "png");
  REQUIRE_OK(back);
  CHECK(back->cost == 20 + 12);
  CHECK(back->lossy);  // the jpg entry is flagged lossy for both directions
}

TEST_CASE("planner: errors") {
  Registry registry;
  registry.add_codec(fake_codec("png", Kind::Image, true, true));
  registry.add_codec(fake_codec("wav", Kind::Audio, true, true));
  registry.add_format(fake_format("known-only"));
  registry.finalize();
  CHECK_STATUS(plan_conversion(registry, "nope", "png"), Status::UnknownFormat);
  CHECK_STATUS(plan_conversion(registry, "png", "nope"), Status::UnknownFormat);
  CHECK_STATUS(plan_conversion(registry, "png", "wav"), Status::UnsupportedConversion);
  CHECK_STATUS(plan_conversion(registry, "png", "known-only"), Status::UnsupportedConversion);
  CHECK(list_targets(registry, "nope").empty());
}

TEST_CASE("planner: the cheapest route wins, bridges included") {
  Registry registry;
  registry.add_codec(fake_codec("csv", Kind::Table, true, true, 10, 10));
  registry.add_codec(fake_codec("csv", Kind::PlainText, true, false, 5, 10));
  registry.add_codec(fake_codec("pdf", Kind::Document, false, true, 10, 30));
  registry.add_codec(fake_codec("pdf", Kind::Image, false, true, 10, 40));
  registry.add_bridge(fake_bridge(Kind::Table, Kind::Document, 20, false, "table as document"));
  registry.add_bridge(fake_bridge(Kind::PlainText, Kind::Document, 40, false, "text as document"));
  registry.add_bridge(fake_bridge(Kind::Table, Kind::Image, 5, true, "render table"));
  registry.finalize();

  // Candidates: Table->Document->pdf = 10+20+30 = 60; PlainText->Document->pdf = 5+40+30 = 75;
  // Table->Image->pdf = 10+5+40 = 55 (cheapest, although lossy: cost decides).
  const auto plan = plan_conversion(registry, "csv", "pdf");
  REQUIRE_OK(plan);
  CHECK(plan->cost == 55);
  CHECK(plan->reader_kind == Kind::Table);
  REQUIRE(plan->bridges.size() == 1);
  CHECK(plan->bridges[0]->label == "render table");
  CHECK(plan->writer->kind == Kind::Image);
  CHECK(plan->lossy);
  CHECK(plan->describe() == "csv -> Table -> [render table] -> Image -> pdf");
}

TEST_CASE("planner: at most two bridges are chained") {
  Registry registry;
  registry.add_codec(fake_codec("src", Kind::Data, true, false));
  registry.add_codec(fake_codec("two", Kind::Document, false, true));
  registry.add_codec(fake_codec("three", Kind::Image, false, true));
  registry.add_bridge(fake_bridge(Kind::Data, Kind::Table, 10));
  registry.add_bridge(fake_bridge(Kind::Table, Kind::Document, 10));
  registry.add_bridge(fake_bridge(Kind::Document, Kind::Image, 10));
  registry.finalize();

  const auto two = plan_conversion(registry, "src", "two");
  REQUIRE_OK(two);
  REQUIRE(two->bridges.size() == 2);
  CHECK(two->bridges[0]->to == Kind::Table);
  CHECK(two->bridges[1]->to == Kind::Document);
  CHECK(two->cost == 10 + 10 + 10 + 10);
  CHECK_STATUS(plan_conversion(registry, "src", "three"), Status::UnsupportedConversion);
}

TEST_CASE("planner: ties are broken by fewer bridges, fewer lossy steps, then registration order") {
  SUBCASE("fewer bridges") {
    Registry registry;
    registry.add_codec(fake_codec("a", Kind::Table, true, false, 10));
    registry.add_codec(fake_codec("a", Kind::Document, true, false, 30));
    registry.add_codec(fake_codec("b", Kind::Document, false, true));
    registry.add_bridge(fake_bridge(Kind::Table, Kind::Document, 20));
    registry.finalize();
    const auto plan = plan_conversion(registry, "a", "b");
    REQUIRE_OK(plan);
    CHECK(plan->cost == 40);
    CHECK(plan->bridges.empty());  // 30+10 direct beats 10+20+10 via a bridge
  }
  SUBCASE("fewer lossy steps") {
    Registry registry;
    registry.add_codec(fake_codec("a", Kind::Image, true, false, 10, 10, true));
    registry.add_codec(fake_codec("a", Kind::Document, true, false, 10, 10, false));
    registry.add_codec(fake_codec("b", Kind::Image, false, true));
    registry.add_codec(fake_codec("b", Kind::Document, false, true));
    registry.finalize();
    const auto plan = plan_conversion(registry, "a", "b");
    REQUIRE_OK(plan);
    CHECK(plan->reader_kind == Kind::Document);
    CHECK_FALSE(plan->lossy);
  }
  SUBCASE("registration order") {
    Registry registry;
    registry.add_codec(fake_codec("a", Kind::Table, true, false));
    registry.add_codec(fake_codec("a", Kind::Data, true, false));
    registry.add_codec(fake_codec("b", Kind::Data, false, true));
    registry.add_codec(fake_codec("b", Kind::Table, false, true));
    registry.finalize();
    const auto plan = plan_conversion(registry, "a", "b");
    REQUIRE_OK(plan);
    CHECK(plan->reader_kind == Kind::Table);  // the reader registered first
    // And the answer never changes between calls.
    for (int i = 0; i < 5; ++i) CHECK(plan_conversion(registry, "a", "b")->reader == plan->reader);
  }
  SUBCASE("the cheaper of two codecs for the same kind") {
    Registry registry;
    registry.add_codec(fake_codec("a", Kind::Image, true, false, 50));
    registry.add_codec(fake_codec("a", Kind::Image, true, false, 7));
    registry.add_codec(fake_codec("b", Kind::Image, false, true, 10, 9));
    registry.add_codec(fake_codec("b", Kind::Image, false, true, 10, 3));
    registry.finalize();
    CHECK(plan_conversion(registry, "a", "b")->cost == 7 + 3);
  }
}

TEST_CASE("planner: every format is implicitly readable as a Blob, at a high cost") {
  Registry registry;
  registry.add_format(fake_format("mystery"));
  registry.add_codec(fake_codec("png", Kind::Image, true, true));
  registry.add_codec(fake_codec("base64", Kind::Blob, true, true));
  registry.add_codec(fake_codec("zip", Kind::Archive, true, true));
  registry.add_bridge(fake_bridge(Kind::Blob, Kind::Archive, 15, false, "wrap file in archive"));
  registry.finalize();

  const auto opaque = plan_conversion(registry, "mystery", "base64");
  REQUIRE_OK(opaque);
  CHECK(opaque->reader == nullptr);
  CHECK(opaque->reader_kind == Kind::Blob);
  CHECK(opaque->cost == implicit_blob_read_cost + 10);
  CHECK_FALSE(opaque->lossy);
  CHECK(opaque->describe() == "mystery -> Blob (raw bytes) -> base64");

  const auto png = plan_conversion(registry, "png", "base64");
  REQUIRE_OK(png);
  CHECK(png->reader == nullptr);  // png has no Blob reader of its own

  // An explicit Blob reader (a decoder) replaces the implicit one.
  const auto decode = plan_conversion(registry, "base64", "zip");
  REQUIRE_OK(decode);
  REQUIRE(decode->reader != nullptr);
  CHECK(decode->cost == 10 + 15 + 10);
  CHECK(decode->describe() == "base64 -> Blob -> [wrap file in archive] -> Archive -> zip");

  // Bridges from Blob make targets universal.
  CHECK(plan_conversion(registry, "mystery", "zip")->cost == implicit_blob_read_cost + 15 + 10);
  CHECK_STATUS(plan_conversion(registry, "mystery", "png"), Status::UnsupportedConversion);
}

TEST_CASE("planner: same-format conversion needs a real re-encode") {
  Registry registry;
  registry.add_codec(fake_codec("png", Kind::Image, true, true));
  registry.add_codec(fake_codec("bin", Kind::Blob, false, true));
  registry.add_codec(fake_codec("zip", Kind::Archive, true, true));
  registry.add_bridge(fake_bridge(Kind::Blob, Kind::Archive, 15));
  registry.finalize();

  const auto png = plan_conversion(registry, "png", "png");
  REQUIRE_OK(png);
  REQUIRE(png->reader != nullptr);
  CHECK(png->cost == 20);
  // bin -> bin would only be a byte copy through the implicit reader.
  CHECK_STATUS(plan_conversion(registry, "bin", "bin"), Status::UnsupportedConversion);
  // zip -> zip re-compresses through its own reader, never "zip inside zip".
  const auto zip = plan_conversion(registry, "zip", "zip");
  REQUIRE_OK(zip);
  CHECK(zip->bridges.empty());
  CHECK(zip->reader_kind == Kind::Archive);

  const auto targets = list_targets(registry, "png");
  REQUIRE(targets.size() == 3);
  for (const TargetInfo& target : targets) CHECK(target.same_format == (target.format->id == "png"));
  CHECK(target_ids(registry, "bin") == std::vector<std::string>{"zip"});
}

TEST_CASE("planner: targets are sorted by category, losslessness, popularity, id") {
  Registry registry;
  registry.add_format(fake_format("src", Category::Image, 10));
  registry.add_format(fake_format("img-lossy", Category::Image, 99));
  registry.add_format(fake_format("img-b", Category::Image, 50));
  registry.add_format(fake_format("img-a", Category::Image, 50));
  registry.add_format(fake_format("img-top", Category::Image, 80));
  registry.add_format(fake_format("doc-popular", Category::Document, 100));
  registry.add_format(fake_format("enc-lossy", Category::Encoding, 100));
  registry.add_format(fake_format("unreachable", Category::Image, 100));
  registry.add_codec(fake_codec("src", Kind::Image, true, false));
  registry.add_codec(fake_codec("img-lossy", Kind::Image, false, true, 10, 10, true));
  for (const char* id : {"img-b", "img-a", "img-top", "doc-popular"}) {
    registry.add_codec(fake_codec(id, Kind::Image, false, true));
  }
  registry.add_codec(fake_codec("enc-lossy", Kind::Image, false, true, 10, 10, true));
  registry.add_codec(fake_codec("unreachable", Kind::Audio, false, true));
  registry.finalize();

  CHECK(target_ids(registry, "src") ==
        std::vector<std::string>{"img-top", "img-a", "img-b", "img-lossy", "doc-popular", "enc-lossy"});
  const auto targets = list_targets(registry, "src");
  CHECK(targets[3].lossy);
  CHECK_FALSE(targets[0].lossy);
  CHECK(targets[0].cost == 20);
}

TEST_CASE("planner: option specs of all steps, first declaration of a key wins") {
  Registry registry;
  Codec reader = fake_codec("a", Kind::Table, true, false);
  reader.read_options = {OptionSpec::text("delimiter", "Delimiter", ","), OptionSpec::integer("level", "Reader level", 1, 0, 9)};
  reader.write_options = {OptionSpec::boolean("unused", "Unused", false)};
  registry.add_codec(reader);
  Bridge bridge = fake_bridge(Kind::Table, Kind::Document, 10);
  bridge.options = {OptionSpec::integer("sheet", "Sheet", 0, 0, 100)};
  registry.add_bridge(bridge);
  Codec writer = fake_codec("b", Kind::Document, false, true);
  writer.write_options = {OptionSpec::integer("level", "Writer level", 6, 0, 9), OptionSpec::boolean("toc", "Contents", true)};
  registry.add_codec(writer);
  registry.finalize();

  const auto plan = plan_conversion(registry, "a", "b");
  REQUIRE_OK(plan);
  const std::vector<OptionSpec> specs = plan->option_specs();
  REQUIRE(specs.size() == 4);
  CHECK(specs[0].key == "delimiter");
  CHECK(specs[1].key == "level");
  CHECK(specs[1].label == "Reader level");
  CHECK(specs[2].key == "sheet");
  CHECK(specs[3].key == "toc");
}

TEST_CASE("planner: plans against the shared registry") {
  const Registry& registry = Registry::instance();
  const auto encode = plan_conversion(registry, "png", "base64");
  REQUIRE_OK(encode);
  CHECK(encode->reader == nullptr);
  const auto decode = plan_conversion(registry, "base64", "bin");
  REQUIRE_OK(decode);
  CHECK(decode->reader != nullptr);
  const auto recode = plan_conversion(registry, "txt", "txt");
  REQUIRE_OK(recode);
  CHECK(recode->reader_kind == Kind::PlainText);
  // Universal targets exist for a detection-only format.
  const auto targets = list_targets(registry, "pdf");
  CHECK(std::ranges::any_of(targets, [](const TargetInfo& t) { return t.format->id == "checksums"; }));
  CHECK(std::ranges::none_of(targets, [](const TargetInfo& t) { return t.same_format; }));
}
