// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "families/audio_lossless/audio_test_util.hpp"
#include "families/audio_lossless/flac_codec.hpp"
#include "util/hash.hpp"

using namespace cvt;
namespace ta = cvt::test::audio;

TEST_CASE("audio_lossless/flac: reference-encoder files of every kind decode bit-exactly") {
  struct Case {
    const char* file;
    const char* reference;
    std::uint16_t bits;
  };
  for (const Case c : {Case{"flac_l0_s16_stereo_11025.flac", "ref_s16_stereo_11025.wav", 16},
                       Case{"flac_l5_s16_stereo_11025.flac", "ref_s16_stereo_11025.wav", 16},
                       Case{"flac_l8_s16_stereo_11025.flac", "ref_s16_stereo_11025.wav", 16},
                       Case{"flac_exhaustive_s16_stereo_11025.flac", "ref_s16_stereo_11025.wav", 16},
                       Case{"flac_b16384_s16_stereo_11025.flac", "ref_s16_stereo_11025.wav", 16},
                       Case{"flac_b1152_s16_mono_8000.flac", "ref_s16_mono_8000.wav", 16},
                       Case{"flac_s24_stereo_22050.flac", "ref_s24_stereo_22050.wav", 24},
                       Case{"flac_u8_mono_8000.flac", "ref_u8_mono_8000.wav", 8},
                       Case{"flac_s16_6ch_16000.flac", "ref_s16_6ch_16000.wav", 16},
                       Case{"flac_tags_s16_mono_8000.flac", "ref_s16_mono_8000.wav", 16}}) {
    INFO(c.file);
    std::vector<std::string> warnings;
    const auto audio = ta::load("flac", c.file, {}, &warnings);
    REQUIRE_OK(audio);
    CHECK(ta::identical(ta::reference(c.reference), *audio));
    CHECK(audio->bits_per_sample == c.bits);
    CHECK(warnings.empty());  // includes the MD5 check
  }
  // 32-bit FLAC: the float IR keeps 24 significant bits.
  const auto s32 = ta::load("flac", "flac_s32_stereo_8000.flac");
  REQUIRE_OK(s32);
  CHECK(s32->bits_per_sample == 32);
  CHECK(ta::max_error(ta::reference("ref_s32_stereo_8000.wav"), *s32) <= 1.0 / (1 << 23));
  // Ogg FLAC (".oga").
  const auto ogg = ta::load("ogg-flac", "flac_s16_stereo_11025.oga");
  REQUIRE_OK(ogg);
  CHECK(ta::identical(ta::reference("ref_s16_stereo_11025.wav"), *ogg));
  const auto via_flac_reader = ta::load("flac", "flac_s16_stereo_11025.oga");
  REQUIRE_OK(via_flac_reader);
  CHECK(ta::identical(*ogg, *via_flac_reader));
}

TEST_CASE("audio_lossless/flac: Vorbis comments are read; picture and seek table are tolerated") {
  const auto tagged = ta::load("flac", "flac_tags_s16_mono_8000.flac");
  REQUIRE_OK(tagged);
  CHECK(tagged->tags.at("title") == "Test tone");
  CHECK(tagged->tags.at("artist") == "Convertible test");
  CHECK(tagged->tags.at("album") == "Fixtures");
  CHECK(tagged->tags.at("track") == "3");
  const auto kept = test::write_from("flac", *tagged, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  const auto back = test::read_as<ir::Audio>("flac", *kept);
  REQUIRE_OK(back);
  CHECK(back->tags == tagged->tags);
  const auto stripped = test::write_from("flac", *tagged);
  REQUIRE_OK(stripped);
  CHECK(test::str(*stripped).find("Test tone") == std::string::npos);
  CHECK(test::str(*stripped).find("libFLAC") == std::string::npos);
}

TEST_CASE("audio_lossless/flac: encoder round trips are bit-exact at every level and depth") {
  for (const int bits : {8, 16, 24}) {
    const ir::Audio input = ta::music(22050, 2, 0.4, bits);
    for (int level = 0; level <= 8; ++level) {
      INFO("bits ", bits, " level ", level);
      const auto back = ta::round_trip("flac", input, std::string(R"({"compression_level":)") + std::to_string(level) + "}");
      REQUIRE_OK(back);
      CHECK(ta::identical(input, *back));
      CHECK(back->bits_per_sample == bits);
    }
  }
  // Odd channel counts, odd lengths, silence, full-scale noise.
  for (const std::uint16_t channels : {1, 3, 6, 8}) {
    INFO(channels);
    const ir::Audio input = ta::noise(8000, channels, 0.1237, 16, channels, 0.99);
    const auto back = ta::round_trip("flac", input);
    REQUIRE_OK(back);
    CHECK(ta::identical(input, *back));
  }
  ir::Audio silence = test::detail::empty_audio(44100, 2, 0.3);
  silence.bits_per_sample = 16;
  Bytes encoded;
  const auto back = ta::round_trip("flac", silence, {}, &encoded);
  REQUIRE_OK(back);
  CHECK(ta::identical(silence, *back));
  CHECK(encoded.size() < 200);  // constant subframes
}

TEST_CASE("audio_lossless/flac: higher levels are smaller; MD5 and STREAMINFO are written") {
  const ir::Audio input = ta::music(44100, 2, 1.0, 16);
  const auto l0 = test::write_from("flac", input, R"({"compression_level":0})");
  const auto l5 = test::write_from("flac", input, R"({"compression_level":5})");
  const auto l8 = test::write_from("flac", input, R"({"compression_level":8})");
  REQUIRE_OK(l0);
  REQUIRE_OK(l5);
  REQUIRE_OK(l8);
  CHECK(l5->size() < l0->size());
  CHECK(l8->size() <= l5->size());
  const auto small = test::write_from("flac", input, R"({"preset":"small"})");
  REQUIRE_OK(small);
  CHECK(small->size() == l8->size());
  // STREAMINFO: total samples and MD5 of the little-endian samples.
  const auto info = audio::flac::parse_streaminfo(ByteView(*l5).subspan(8, 34));
  REQUIRE_OK(info);
  CHECK(info->total_samples == input.frame_count());
  CHECK(info->bits == 16);
  audio::flac::SampleMd5 md5(16);
  std::vector<std::int32_t> ints;
  for (const float s : input.samples) ints.push_back(static_cast<std::int32_t>(std::lround(s * 32768.0f)));
  md5.update(ints);
  CHECK(md5.finish() == info->md5);
  // Same-format compress: flac -> flac at a higher level re-encodes and verifies.
  const test::Converted compressed =
      test::convert(*l0, "flac", "flac", R"({"compression_level":8})");
  REQUIRE(compressed.ok());
  CHECK(compressed.data.size() < l0->size());
}

TEST_CASE("audio_lossless/flac: sample formats, float input and rates without a code") {
  const ir::Audio input = ta::music(37800, 1, 0.2, 24);
  const auto s16 = ta::round_trip("flac", input, R"({"sample_format":"s16"})");
  REQUIRE_OK(s16);
  CHECK(s16->bits_per_sample == 16);
  CHECK(test::snr_db(input, *s16) >= 85.0);
  const auto s20 = ta::round_trip("flac", ta::on_grid(input, 20), R"({"sample_format":"s20"})");
  REQUIRE_OK(s20);
  CHECK(ta::identical(ta::on_grid(input, 20), *s20));
  ir::Audio floats = input;
  floats.float_samples = true;
  floats.bits_per_sample = 32;
  std::vector<std::string> warnings;
  const auto encoded = test::write_from("flac", floats, {}, &warnings);
  REQUIRE_OK(encoded);
  CHECK(!warnings.empty());
  const auto back = test::read_as<ir::Audio>("flac", *encoded);
  REQUIRE_OK(back);
  CHECK(back->bits_per_sample == 24);
  CHECK(back->sample_rate == 37800);
}

TEST_CASE("audio_lossless/flac: damaged streams are skipped or rejected, never crash") {
  const Bytes valid = test::fixture("audio_lossless/flac_l5_s16_stereo_11025.flac");
  CHECK(test::fuzz_reader<ir::Audio>("flac", valid) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("flac", test::fixture("audio_lossless/flac_s24_stereo_22050.flac")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("ogg-flac", test::fixture("audio_lossless/flac_s16_stereo_11025.oga")) > 0);
  CHECK_STATUS(test::read_as<ir::Audio>("flac", test::bytes("fLaC")), Status::Decode);
  // Corrupt one byte in the middle of the audio: the frame is skipped with a warning.
  Bytes damaged = valid;
  damaged[damaged.size() / 2] ^= std::byte{0x55};
  std::vector<std::string> warnings;
  const auto audio = test::read_as<ir::Audio>("flac", damaged, {}, &warnings);
  REQUIRE_OK(audio);
  CHECK(!warnings.empty());
  // Total samples claiming more than the limits allow.
  Limits tight;
  tight.max_decompressed_bytes = 4096;
  CHECK_STATUS(test::read_as<ir::Audio>("flac", valid, {}, nullptr, tight), Status::Limit);
}
