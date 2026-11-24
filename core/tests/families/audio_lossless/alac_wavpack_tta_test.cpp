// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "families/audio_lossless/audio_test_util.hpp"

using namespace cvt;
namespace ta = cvt::test::audio;

namespace {

void exact(std::string_view format, std::string_view name, std::string_view reference, std::uint16_t bits) {
  INFO(name);
  std::vector<std::string> warnings;
  const auto audio = ta::load(format, name, {}, &warnings);
  REQUIRE_OK(audio);
  CHECK(ta::identical(ta::reference(reference), *audio));
  CHECK(audio->bits_per_sample == bits);
  CHECK(warnings.empty());
}

}  // namespace

// =========================================================================== ALAC

TEST_CASE("audio_lossless/alac: files from afconvert and ffmpeg decode bit-exactly") {
  exact("alac", "af_alac16_stereo_11025.m4a", "ref_s16_stereo_11025.wav", 16);
  exact("alac", "af_alac24_stereo_22050.m4a", "ref_s24_stereo_22050.wav", 24);
  exact("alac", "af_alac16_mono_8000.m4a", "ref_s16_mono_8000.wav", 16);
  exact("alac", "ff_alac_s16_stereo_11025.m4a", "ref_s16_stereo_11025.wav", 16);
  // 5.1: the element order C L R Ls Rs LFE comes back in WAVE order.
  const auto surround = ta::load("alac", "af_alac16_51_16000.m4a");
  REQUIRE_OK(surround);
  const auto source = ta::load("wav", "ff_extensible_s16_6ch_16000.wav");
  REQUIRE_OK(source);
  CHECK(surround->samples == source->samples);
  // The generic m4a reader handles ALAC too.
  const auto generic = ta::load("m4a", "af_alac16_stereo_11025.m4a");
  REQUIRE_OK(generic);
  CHECK(ta::identical(ta::reference("ref_s16_stereo_11025.wav"), *generic));
}

TEST_CASE("audio_lossless/alac: iTunes tags are read and written only on request") {
  const auto tagged = ta::load("alac", "ff_alac_tags_s16_mono_8000.m4a");
  REQUIRE_OK(tagged);
  CHECK(tagged->tags.at("title") == "Test tone");
  CHECK(tagged->tags.at("artist") == "Convertible test");
  ir::Audio with_track = *tagged;
  with_track.tags["track"] = "3/12";
  with_track.tags["mood"] = "calm";
  const auto kept = test::write_from("alac", with_track, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  const auto back = test::read_as<ir::Audio>("alac", *kept);
  REQUIRE_OK(back);
  CHECK(back->tags.at("title") == "Test tone");
  CHECK(back->tags.at("track") == "3/12");
  CHECK(back->tags.at("mood") == "calm");
  const auto stripped = test::write_from("alac", with_track);
  REQUIRE_OK(stripped);
  CHECK(test::str(*stripped).find("Test tone") == std::string::npos);
  CHECK(test::str(*stripped).find("Lavf") == std::string::npos);
}

TEST_CASE("audio_lossless/alac: encoder round trips exactly for 16/20/24/32-bit and 1-8 channels") {
  for (const int bits : {16, 20, 24}) {
    INFO(bits);
    const ir::Audio input = ta::music(44100, 2, 0.3, bits);
    const auto back = ta::round_trip("alac", input, std::string(R"({"sample_format":"s)") + std::to_string(bits) + "\"}");
    REQUIRE_OK(back);
    CHECK(ta::identical(input, *back));
    CHECK(back->bits_per_sample == bits);
  }
  const ir::Audio wide = ta::noise(8000, 2, 0.2, 24);
  const auto s32 = ta::round_trip("alac", wide, R"({"sample_format":"s32"})");
  REQUIRE_OK(s32);
  CHECK(ta::identical(wide, *s32));
  for (std::uint16_t channels = 1; channels <= 8; ++channels) {
    INFO(channels);
    const ir::Audio input = ta::noise(16000, channels, 0.3, 16, channels);
    const auto back = ta::round_trip("alac", input);
    REQUIRE_OK(back);
    CHECK(ta::identical(input, *back));
  }
  // Effort changes size, never content.
  const ir::Audio input = ta::music(44100, 2, 1.0, 16);
  const auto fast = test::write_from("alac", input, R"({"compression_level":0})");
  const auto best = test::write_from("alac", input, R"({"compression_level":9})");
  REQUIRE_OK(fast);
  REQUIRE_OK(best);
  CHECK(best->size() <= fast->size());
  CHECK(best->size() < input.samples.size() * 2);  // smaller than 16-bit PCM
  CHECK(test::fuzz_reader<ir::Audio>("alac", *best) > 0);
}

TEST_CASE("audio_lossless/alac: damaged files fail cleanly") {
  CHECK(test::fuzz_reader<ir::Audio>("alac", test::fixture("audio_lossless/af_alac16_stereo_11025.m4a")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("alac", test::fixture("audio_lossless/ff_alac_s16_stereo_11025.m4a")) > 0);
  CHECK_STATUS(test::read_as<ir::Audio>("alac", test::bytes("\x00\x00\x00\x08" "ftyp")), Status::Decode);
}

// =========================================================================== WavPack

TEST_CASE("audio_lossless/wavpack: reference-encoder files decode bit-exactly with checksums") {
  exact("wv", "wv_s16_stereo_11025.wv", "ref_s16_stereo_11025.wav", 16);
  exact("wv", "wv_hh_s16_stereo_11025.wv", "ref_s16_stereo_11025.wav", 16);
  exact("wv", "wv_fast_s16_stereo_11025.wv", "ref_s16_stereo_11025.wav", 16);
  exact("wv", "wv_s24_stereo_22050.wv", "ref_s24_stereo_22050.wav", 24);
  exact("wv", "wv_u8_mono_8000.wv", "ref_u8_mono_8000.wav", 8);
  exact("wv", "wv_s16_6ch_16000.wv", "ref_s16_6ch_16000.wav", 16);
  exact("wv", "wv_f32_stereo_11025.wv", "ref_s16_stereo_11025.wav", 32);
  const auto s32 = ta::load("wv", "wv_s32_stereo_8000.wv");
  REQUIRE_OK(s32);
  CHECK(ta::max_error(ta::reference("ref_s32_stereo_8000.wav"), *s32) <= 1.0 / (1 << 23));
}

TEST_CASE("audio_lossless/wavpack: hybrid (lossy) files decode without the correction file") {
  struct Case {
    const char* file;
    const char* reference;
    double min_snr;
  };
  for (const Case c : {Case{"wv_hybrid_s16_stereo_11025.wv", "ref_s16_stereo_11025.wav", 20.0},
                       Case{"wv_hybrid_s24_stereo_22050.wv", "ref_s24_stereo_22050.wav", 20.0},
                       Case{"wv_hybridc_s16_mono_8000.wv", "ref_s16_mono_8000.wav", 25.0}}) {
    INFO(c.file);
    std::vector<std::string> warnings;
    const auto audio = ta::load("wv", c.file, {}, &warnings);
    REQUIRE_OK(audio);
    const ir::Audio ref = ta::reference(c.reference);
    CHECK(audio->frame_count() == ref.frame_count());
    CHECK(test::snr_db(ref, *audio) >= c.min_snr);
    REQUIRE(warnings.size() == 1);  // "lossy", but no checksum failure
    CHECK(warnings[0].find("hybrid") != std::string::npos);
  }
}

TEST_CASE("audio_lossless/wavpack: encoder round trips exactly; effort changes size") {
  for (const int bits : {8, 16, 24}) {
    for (const std::uint16_t channels : {1, 2, 3, 6}) {
      INFO(bits, " bits, ", channels, " channels");
      const ir::Audio input = ta::music(22050, channels, 0.3, bits);
      const auto back = ta::round_trip("wv", input);
      REQUIRE_OK(back);
      CHECK(ta::identical(input, *back));
    }
  }
  const ir::Audio input = ta::reference("ref_s16_stereo_11025.wav");
  const auto fast = test::write_from("wv", input, R"({"compression_level":0})");
  const auto normal = test::write_from("wv", input);
  const auto high = test::write_from("wv", input, R"({"compression_level":9})");
  REQUIRE_OK(fast);
  REQUIRE_OK(normal);
  REQUIRE_OK(high);
  CHECK(normal->size() < fast->size());
  CHECK(high->size() < normal->size());
  CHECK(high->size() < input.samples.size() * 2);
  ir::Audio tagged = input;
  tagged.tags["title"] = "Sweep";
  const auto with_tags = ta::round_trip("wv", tagged, R"({"strip_metadata":false})");
  REQUIRE_OK(with_tags);
  CHECK(with_tags->tags.at("title") == "Sweep");
}

TEST_CASE("audio_lossless/wavpack: damaged files fail cleanly") {
  CHECK(test::fuzz_reader<ir::Audio>("wv", test::fixture("audio_lossless/wv_s16_stereo_11025.wv")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("wv", test::fixture("audio_lossless/wv_f32_stereo_11025.wv")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("wv", test::fixture("audio_lossless/wv_hybrid_s16_stereo_11025.wv")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("wv", test::fixture("audio_lossless/wv_s32_stereo_8000.wv")) > 0);
  CHECK_STATUS(test::read_as<ir::Audio>("wv", test::bytes("wvpk")), Status::Decode);
}

// =========================================================================== TTA

TEST_CASE("audio_lossless/tta: ffmpeg-encoded files decode bit-exactly") {
  exact("tta", "tta_s16_stereo_11025.tta", "ref_s16_stereo_11025.wav", 16);
  exact("tta", "tta_s24_stereo_22050.tta", "ref_s24_stereo_22050.wav", 24);
  exact("tta", "tta_u8_mono_8000.tta", "ref_u8_mono_8000.wav", 8);
}

TEST_CASE("audio_lossless/tta: encoder round trips exactly with tags on request") {
  for (const int bits : {8, 16, 24}) {
    for (const std::uint16_t channels : {1, 2, 5}) {
      INFO(bits, " bits, ", channels, " channels");
      const ir::Audio input = ta::music(44100, channels, 0.4, bits);
      Bytes encoded;
      const auto back = ta::round_trip("tta", input, {}, &encoded);
      REQUIRE_OK(back);
      CHECK(ta::identical(input, *back));
      CHECK(encoded.size() < input.samples.size() * static_cast<std::size_t>(bits / 8));
    }
  }
  ir::Audio tagged = ta::music(8000, 1, 0.1, 16);
  tagged.tags["artist"] = "Nobody";
  const auto back = ta::round_trip("tta", tagged, R"({"strip_metadata":false})");
  REQUIRE_OK(back);
  CHECK(back->tags.at("artist") == "Nobody");
  CHECK(test::fuzz_reader<ir::Audio>("tta", test::fixture("audio_lossless/tta_s16_stereo_11025.tta")) > 0);
  CHECK_STATUS(test::read_as<ir::Audio>("tta", test::bytes("TTA1")), Status::Decode);
}
