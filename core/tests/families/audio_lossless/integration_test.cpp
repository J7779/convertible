// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <utility>
#include <vector>

#include "engine/detect.hpp"
#include "engine/planner.hpp"
#include "families/audio_lossless/audio_test_util.hpp"

using namespace cvt;
namespace ta = cvt::test::audio;

TEST_CASE("audio_lossless/integration: the registry stays consistent") {
  CHECK(Registry::instance().validate().empty());
  for (const char* id : {"wav", "w64", "aiff", "aifc", "au", "caf", "pcm", "voc", "8svx", "sph", "flac", "ogg-flac", "alac",
                         "wv", "tta"}) {
    INFO(id);
    const FormatInfo* info = Registry::instance().find(id);
    REQUIRE(info != nullptr);
    CHECK(info->category == Category::Audio);
    CHECK(Registry::instance().has_reader(id));
  }
}

TEST_CASE("audio_lossless/integration: formats are detected from content, with and without a name") {
  struct Case {
    const char* fixture;
    const char* format;
  };
  for (const Case c : {Case{"ref_s16_stereo_11025.wav", "wav"},          Case{"af_rf64_stereo_11025.wav", "wav"},
                       Case{"af_bw64_stereo_22050.wav", "wav"},          Case{"af_s16_stereo_11025.w64", "w64"},
                       Case{"af_s16_stereo_11025.aiff", "aiff"},         Case{"af_fl32_stereo_11025.aifc", "aifc"},
                       Case{"af_s16_stereo_11025.au", "au"},             Case{"af_s16_stereo_11025.caf", "caf"},
                       Case{"sox_s16_stereo_11025.voc", "voc"},          Case{"sox_s8_mono_8000.8svx", "8svx"},
                       Case{"sox_s16_stereo_11025.sph", "sph"},          Case{"flac_l5_s16_stereo_11025.flac", "flac"},
                       Case{"flac_s16_stereo_11025.oga", "ogg-flac"},    Case{"af_alac16_stereo_11025.m4a", "alac"},
                       Case{"wv_s16_stereo_11025.wv", "wv"},             Case{"tta_s16_stereo_11025.tta", "tta"}}) {
    INFO(c.fixture);
    const Bytes data = test::fixture(std::string(ta::dir) + c.fixture);
    const auto unnamed = detect_format(Registry::instance(), data, "");
    REQUIRE(unnamed.has_value());
    CHECK(unnamed->format->id == c.format);
    const auto named = detect_format(Registry::instance(), data, c.fixture);
    REQUIRE(named.has_value());
    // With an .m4a name the generic MPEG-4 audio format may win; it reads ALAC too.
    if (std::string_view(c.format) != "alac") CHECK(named->format->id == c.format);
  }
}

TEST_CASE("audio_lossless/integration: end-to-end conversions and the compress route") {
  const Bytes wav = test::fixture("audio_lossless/ref_s16_stereo_11025.wav");
  for (const char* target : {"flac", "alac", "wv", "tta", "aiff", "aifc", "caf", "au", "w64", "voc", "sph", "8svx", "wav", "pcm"}) {
    INFO(target);
    const test::Converted out = test::convert(wav, "", target, "", "tone.wav");
    REQUIRE(out.ok());
    CHECK(out.format_id == target);
    if (std::string_view(target) == "pcm" || std::string_view(target) == "8svx") continue;
    // Back to WAV: lossless targets restore the samples exactly.
    const test::Converted back = test::convert(out.data, target, "wav");
    REQUIRE(back.ok());
    if (std::string_view(target) != "voc") CHECK(back.data == wav);
  }
  // Same-format "compress" routes: every lossless writer re-encodes.
  for (const auto& [format, fixture] : {std::pair<const char*, const char*>{"flac", "flac_l0_s16_stereo_11025.flac"},
                                        {"wv", "wv_fast_s16_stereo_11025.wv"},
                                        {"alac", "ff_alac_s16_stereo_11025.m4a"},
                                        {"tta", "tta_s16_stereo_11025.tta"},
                                        {"wav", "af_f32_stereo_11025.wav"},
                                        {"caf", "af_s16_stereo_11025.caf"},
                                        {"aiff", "af_s24_stereo_22050.aiff"}}) {
    INFO(format);
    const Bytes input = test::fixture(std::string(ta::dir) + fixture);
    const auto plan = plan_conversion(Registry::instance(), format, format);
    REQUIRE_OK(plan);
    CHECK(plan->reader != nullptr);
    const test::Converted out = test::convert(input, format, format, R"({"preset":"small"})");
    REQUIRE(out.ok());
    const auto original = test::read_as<ir::Audio>(format, input);
    const auto again = test::read_as<ir::Audio>(format, out.data);
    REQUIRE_OK(original);
    REQUIRE_OK(again);
    CHECK(ta::identical(*original, *again));
  }
  // wav -> wav at a lower depth and rate is smaller.
  const test::Converted smaller = test::convert(wav, "wav", "wav", R"({"sample_format":"u8","sample_rate":8000})");
  REQUIRE(smaller.ok());
  CHECK(smaller.data.size() < wav.size() / 2);
  // flac -> flac at a higher level is smaller than level 0.
  const Bytes l0 = test::fixture("audio_lossless/flac_l0_s16_stereo_11025.flac");
  const test::Converted l8 = test::convert(l0, "flac", "flac", R"({"compression_level":8})");
  REQUIRE(l8.ok());
  CHECK(l8.data.size() < l0.size());
}

TEST_CASE("audio_lossless/integration: progress is reported and cancellation stops readers and writers") {
  const Codec* reader = test::find_reader("flac", Kind::Audio);
  REQUIRE(reader != nullptr);
  const ir::Audio input = ta::music(44100, 2, 2.0, 16);
  const auto flac = test::write_from("flac", input);
  REQUIRE_OK(flac);
  ReadContext ctx;
  ctx.input = *flac;
  ctx.registry = &Registry::instance();
  int calls = 0;
  ctx.progress = Progress([&calls](double, std::string_view) {
    ++calls;
    return true;  // true asks to cancel
  });
  CHECK_STATUS(reader->read(ctx), Status::Cancelled);
  CHECK(calls >= 1);
  const Codec* writer = test::find_writer("flac", Kind::Audio);
  REQUIRE(writer != nullptr);
  WriteContext wctx;
  auto options = Options().normalized(writer->write_options);
  REQUIRE_OK(options);
  wctx.options = *options;
  wctx.progress = Progress([](double fraction, std::string_view) { return fraction > 0.3; });
  CHECK_STATUS(writer->write(input, wctx), Status::Cancelled);
}

TEST_CASE("audio_lossless/integration: writers refuse what their format cannot hold") {
  // Formats with a channel limit mix down (with a note) instead of failing.
  const ir::Audio nine = ta::noise(8000, 9, 0.01, 16);
  for (const char* format : {"alac", "flac"}) {
    INFO(format);
    std::vector<std::string> notes;
    const auto encoded = test::write_from(format, nine, {}, &notes);
    REQUIRE_OK(encoded);
    CHECK(!notes.empty());
    const auto back = test::read_as<ir::Audio>(format, *encoded);
    REQUIRE_OK(back);
    CHECK(back->channels == 8);
  }
  const ir::Audio empty;
  CHECK_STATUS(test::write_from("wav", empty), Status::Encode);
  ir::Audio four = ta::noise(8000, 4, 0.01, 16);
  CHECK_STATUS(test::write_from("wav", four, R"({"sample_format":"ms_adpcm"})"), Status::Encode);
  // 8SVX holds at most stereo: mixed down with a warning.
  std::vector<std::string> warnings;
  const auto svx = test::write_from("8svx", four, {}, &warnings);
  REQUIRE_OK(svx);
  CHECK(!warnings.empty());
}
