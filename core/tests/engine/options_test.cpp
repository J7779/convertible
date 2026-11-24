// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>

#include "engine/options.hpp"
#include "engine/standard_options.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

Options parsed(std::string_view json) {
  auto options = Options::parse(json);
  REQUIRE_OK(options);
  return std::move(*options);
}

std::vector<OptionSpec> sample_specs() {
  return {OptionSpec::integer("quality", "Quality", 85, 1, 100, "Higher is better."),
          OptionSpec::number("gamma", "Gamma", 2.2, 0.1, 5.0),
          OptionSpec::boolean("interlace", "Interlace", false),
          OptionSpec::choice("mode", "Mode", "fast", {"fast", "Best", "tiny"}),
          OptionSpec::text("title", "Title", "untitled").advanced()};
}

}  // namespace

TEST_CASE("options: parsing the options_json argument") {
  CHECK(parsed("").empty());
  CHECK(parsed("   \n").empty());
  CHECK(parsed("null").empty());
  CHECK(parsed("{}").empty());
  CHECK(parsed(R"({"a":1})").contains("a"));
  CHECK_STATUS(Options::parse("[1,2]"), Status::InvalidArgument);
  CHECK_STATUS(Options::parse("42"), Status::InvalidArgument);
  CHECK_STATUS(Options::parse("{\"a\":"), Status::InvalidArgument);
  CHECK_STATUS(Options::parse("not json"), Status::InvalidArgument);
  CHECK(Options(util::parse_json("[1]").value()).empty());  // non-objects are treated as empty
}

TEST_CASE("options: typed getters coerce leniently and fall back") {
  const Options o = parsed(R"({"i":7,"f":2.5,"s":"12","t":"yes","b":true,"n":null,"neg":-3,"big":1e30,
                              "word":"hello","arr":[1],"float_text":" 1.5 ","zero":0,"huge":18446744073709551615})");
  CHECK(o.get_int("i", 0) == 7);
  CHECK(o.get_int("f", 0) == 3);  // rounded (2.5 -> 3)
  CHECK(o.get_int("s", 0) == 12);
  CHECK(o.get_int("b", 0) == 1);
  CHECK(o.get_int("word", 99) == 99);
  CHECK(o.get_int("arr", 99) == 99);
  CHECK(o.get_int("n", 99) == 99);
  CHECK(o.get_int("missing", 99) == 99);
  CHECK(o.get_int("big", 0) == std::numeric_limits<std::int64_t>::max());  // saturates
  CHECK(o.get_int("huge", 0) == std::numeric_limits<std::int64_t>::max());
  CHECK(o.get_int("i", 0, 0, 5) == 5);      // clamped
  CHECK(o.get_int("neg", 0, 0, 5) == 0);
  CHECK(o.get_int("missing", 50, 0, 5) == 5);  // the fallback is clamped too
  CHECK(o.get_int("i", 0, 5, 0) == 5);      // reversed bounds are tolerated

  CHECK(o.get_float("f", 0.0) == 2.5);
  CHECK(o.get_float("i", 0.0) == 7.0);
  CHECK(o.get_float("float_text", 0.0) == 1.5);
  CHECK(o.get_float("word", 9.0) == 9.0);
  CHECK(o.get_float("f", 0.0, 0.0, 1.0) == 1.0);

  CHECK(o.get_bool("b", false));
  CHECK(o.get_bool("t", false));
  CHECK(o.get_bool("i", false));
  CHECK_FALSE(o.get_bool("zero", true));
  CHECK(o.get_bool("word", true));  // not a boolean: fallback
  CHECK_FALSE(o.get_bool("missing", false));

  CHECK(o.get_string("word") == "hello");
  CHECK(o.get_string("i") == "7");
  CHECK(o.get_string("f") == "2.5");
  CHECK(o.get_string("b") == "true");
  CHECK(o.get_string("arr", "fallback") == "fallback");
  CHECK(o.get_string("missing").empty());

  CHECK(o.get_enum("word", "x", {"Hello", "world"}) == "Hello");  // canonical spelling is returned
  CHECK(o.get_enum("word", "x", {"a", "b"}) == "x");
  const std::array<std::string, 2> choices = {"12", "13"};
  CHECK(o.get_enum("s", "x", choices) == "12");
  CHECK_FALSE(o.contains("n"));
  CHECK(o.contains("zero"));
}

TEST_CASE("options: normalisation applies defaults, coercion and clamping") {
  const std::vector<OptionSpec> specs = sample_specs();
  const auto defaults = Options().normalized(specs);
  REQUIRE_OK(defaults);
  CHECK(util::dump_json(defaults->json()) ==
        R"({"quality":85,"gamma":2.2,"interlace":false,"mode":"fast","title":"untitled"})");

  const auto set = parsed(R"({"quality":"150","gamma":0,"interlace":"on","mode":"BEST","title":42,"extra":[1]})")
                       .normalized(specs);
  REQUIRE_OK(set);
  CHECK(set->get_int("quality", 0) == 100);         // "150" coerced, then clamped
  CHECK(set->get_float("gamma", 0) == 0.1);
  CHECK(set->get_bool("interlace", false));
  CHECK(set->get_string("mode") == "Best");         // canonical spelling
  CHECK(set->get_string("title") == "42");
  CHECK(set->contains("extra"));                    // unknown keys pass through

  CHECK(parsed(R"({"quality":null})").normalized(specs)->get_int("quality", 0) == 85);  // null = not set
  CHECK(parsed(R"({"quality":-5})").normalized(specs)->get_int("quality", 0) == 1);
  CHECK(parsed(R"({"quality":7.6})").normalized(specs)->get_int("quality", 0) == 8);

  for (const std::string_view bad : {R"({"quality":"high"})", R"({"quality":[1]})", R"({"gamma":"x"})",
                                     R"({"interlace":"maybe"})", R"({"mode":"slow"})", R"({"mode":3})",
                                     R"({"title":{"a":1}})"}) {
    const auto result = parsed(bad).normalized(specs);
    CHECK_MESSAGE(!result.has_value(), bad);
    if (!result) CHECK(result.error().code == Status::InvalidArgument);
  }
  const auto message = parsed(R"({"mode":"slow"})").normalized(specs);
  CHECK(message.error().message == "option 'mode' must be one of: fast, Best, tiny");
}

TEST_CASE("options: was_set tells explicit values from defaults") {
  const std::vector<OptionSpec> specs = sample_specs();
  const auto options = parsed(R"({"quality":85})").normalized(specs);
  REQUIRE_OK(options);
  CHECK(options->was_set("quality"));   // explicitly set to what happens to be the default
  CHECK_FALSE(options->was_set("gamma"));
  CHECK_FALSE(options->was_set("missing"));
  CHECK(options->contains("gamma"));
  // Normalising again (as each conversion step does) keeps the distinction.
  const auto again = options->normalized(specs);
  REQUIRE_OK(again);
  CHECK(again->was_set("quality"));
  CHECK_FALSE(again->was_set("gamma"));

  Options edited = *options;
  edited.set("gamma", 1.0);
  CHECK(edited.was_set("gamma"));
  edited.erase("quality");
  CHECK_FALSE(edited.contains("quality"));
}

TEST_CASE("options: the published schema") {
  const std::vector<OptionSpec> specs = sample_specs();
  const Json schema = option_schema_json(specs);
  CHECK(util::dump_json(schema) ==
        R"([{"key":"quality","label":"Quality","type":"int","default":85,"min":1,"max":100,"choices":[],)"
        R"("description":"Higher is better.","category":"basic"},)"
        R"({"key":"gamma","label":"Gamma","type":"float","default":2.2,"min":0.1,"max":5.0,"choices":[],)"
        R"("description":"","category":"basic"},)"
        R"({"key":"interlace","label":"Interlace","type":"bool","default":false,"min":null,"max":null,"choices":[],)"
        R"("description":"","category":"basic"},)"
        R"({"key":"mode","label":"Mode","type":"enum","default":"fast","min":null,"max":null,)"
        R"("choices":["fast","Best","tiny"],"description":"","category":"basic"},)"
        R"({"key":"title","label":"Title","type":"string","default":"untitled","min":null,"max":null,"choices":[],)"
        R"("description":"","category":"advanced"}])");
  CHECK(util::dump_json(option_schema_json({})) == "[]");
}

TEST_CASE("standard options: specs are self-consistent") {
  const std::vector<OptionSpec> all = {
      options::quality(),        options::preset(),     options::compression_level(), options::lossless(),
      options::max_width(),      options::max_height(), options::scale(),             options::strip_metadata(),
      options::sample_rate(),    options::channels(),   options::bitrate_kbps(),      options::fps(),
      options::width(),          options::height(),     options::video_bitrate_kbps(), options::audio_bitrate_kbps()};
  const auto defaults = Options().normalized(all);
  REQUIRE_OK(defaults);
  CHECK(defaults->get_int("quality", 0) == 90);
  CHECK(defaults->get_string("preset") == "high");
  CHECK(defaults->get_bool("strip_metadata", false));  // privacy first
  CHECK(defaults->get_int("compression_level", -1) == 6);
  for (const OptionSpec& spec : all) {
    CHECK_FALSE(spec.label.empty());
    CHECK_FALSE(spec.description.empty());
  }
  CHECK(options::quality().category == OptionCategory::Basic);
  CHECK(options::compression_level().category == OptionCategory::Advanced);
  CHECK(options::compression_level(22).max == 22.0);
  CHECK(options::compression_level(3).default_value == Json(3));
}

TEST_CASE("standard options: quality, preset and compression level resolve consistently") {
  const std::vector<OptionSpec> specs = {options::quality(), options::preset(), options::compression_level()};
  const auto resolve = [&](std::string_view json) {
    auto options = parsed(json).normalized(specs);
    REQUIRE_OK(options);
    return std::move(*options);
  };
  CHECK(options::resolved_quality(resolve("{}")) == 90);
  CHECK(options::resolved_quality(resolve(R"({"preset":"balanced"})")) == 75);
  CHECK(options::resolved_quality(resolve(R"({"preset":"small"})")) == 50);
  CHECK(options::resolved_quality(resolve(R"({"preset":"small","quality":70})")) == 70);  // explicit quality wins
  CHECK(options::resolved_quality(resolve(R"({"preset":"small","quality":90})")) == 90);  // even at the default value
  CHECK(options::resolved_quality(resolve(R"({"quality":500})")) == 100);
  CHECK(options::resolved_quality(Options()) == 90);  // works on raw options too
  CHECK(options::resolved_preset(resolve(R"({"preset":"SMALL"})")) == options::Preset::Small);
  CHECK(options::resolved_preset(Options()) == options::Preset::High);

  CHECK(options::resolved_compression_level(resolve("{}")) == 6);
  CHECK(options::resolved_compression_level(resolve(R"({"preset":"small"})")) == 9);
  CHECK(options::resolved_compression_level(resolve(R"({"preset":"small","compression_level":2})")) == 2);
  CHECK(options::resolved_compression_level(resolve("{}"), 22) == 15);
  CHECK(options::resolved_compression_level(resolve(R"({"preset":"small"})"), 22) == 22);
  CHECK(options::resolved_lossless(Options(), true));
  CHECK(options::resolved_strip_metadata(Options()));
  CHECK_FALSE(options::resolved_strip_metadata(parsed(R"({"strip_metadata":false})")));
}

TEST_CASE("standard options: geometry and media parameters") {
  using options::Size;
  CHECK(options::resolved_image_size(Options(), {640, 480}) == Size{640, 480});
  CHECK(options::resolved_image_size(parsed(R"({"scale":0.5})"), {640, 480}) == Size{320, 240});
  CHECK(options::resolved_image_size(parsed(R"({"max_width":100})"), {640, 480}) == Size{100, 75});
  CHECK(options::resolved_image_size(parsed(R"({"max_height":100})"), {640, 480}) == Size{133, 100});
  CHECK(options::resolved_image_size(parsed(R"({"max_width":100,"max_height":50})"), {640, 480}) == Size{67, 50});
  CHECK(options::resolved_image_size(parsed(R"({"max_width":4000})"), {640, 480}) == Size{640, 480});  // never enlarges
  CHECK(options::resolved_image_size(parsed(R"({"scale":2,"max_width":800})"), {640, 480}) == Size{800, 600});
  CHECK(options::resolved_image_size(parsed(R"({"max_width":1})"), {1000, 2}) == Size{1, 1});  // never zero
  CHECK(options::resolved_image_size(parsed(R"({"scale":0.5})"), {0, 0}) == Size{0, 0});

  CHECK(options::resolved_video_size(Options(), {1920, 1080}) == Size{1920, 1080});
  CHECK(options::resolved_video_size(Options(), {853, 481}) == Size{854, 482});  // rounded to even
  CHECK(options::resolved_video_size(parsed(R"({"width":1280})"), {1920, 1080}) == Size{1280, 720});
  CHECK(options::resolved_video_size(parsed(R"({"height":360})"), {1920, 1080}) == Size{640, 360});
  CHECK(options::resolved_video_size(parsed(R"({"width":100,"height":100})"), {1920, 1080}) == Size{100, 100});
  CHECK(options::resolved_video_size(parsed(R"({"max_width":960})"), {1920, 1080}) == Size{960, 540});

  CHECK(options::resolved_sample_rate(Options(), 44100) == 44100u);
  CHECK(options::resolved_sample_rate(parsed(R"({"sample_rate":48000})"), 44100) == 48000u);
  CHECK(options::resolved_channels(Options(), 2) == 2);
  CHECK(options::resolved_channels(parsed(R"({"channels":1})"), 2) == 1);
  CHECK_FALSE(options::resolved_bitrate_kbps(Options()).has_value());
  CHECK(options::resolved_bitrate_kbps(parsed(R"({"bitrate_kbps":192})")) == 192u);
  CHECK(options::resolved_video_bitrate_kbps(parsed(R"({"video_bitrate_kbps":2500})")) == 2500u);
  CHECK_FALSE(options::resolved_audio_bitrate_kbps(parsed(R"({"audio_bitrate_kbps":0})")).has_value());
  CHECK(options::resolved_fps(Options(), 29.97) == 29.97);
  CHECK(options::resolved_fps(parsed(R"({"fps":24})"), 29.97) == 24.0);
}
