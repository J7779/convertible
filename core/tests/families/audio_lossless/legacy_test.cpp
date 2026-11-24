// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>

#include "families/audio_lossless/audio_test_util.hpp"

using namespace cvt;
namespace ta = cvt::test::audio;

TEST_CASE("audio_lossless/voc: Creative Voice files from sox decode") {
  const auto s16 = ta::load("voc", "sox_s16_stereo_11025.voc");
  REQUIRE_OK(s16);
  const ir::Audio ref = ta::reference("ref_s16_stereo_11025.wav");
  CHECK(s16->sample_rate == 11025);
  CHECK(s16->channels == 2);
  // sox's block header undercounts the data by 8 bytes; what it declares is exact.
  CHECK(s16->frame_count() == 1998);
  CHECK(ta::differing_samples(ref, *s16, 1998) == 0);
  const auto u8 = ta::load("voc", "sox_u8_mono_8000.voc");
  REQUIRE_OK(u8);
  CHECK(ta::identical(ta::reference("ref_u8_mono_8000.wav"), *u8));
  for (const char* sample : {"u8", "s16", "alaw", "mulaw"}) {
    INFO(sample);
    const ir::Audio input = ta::music(8000, 2, 0.2, 16);
    const auto back = ta::round_trip("voc", input, std::string(R"({"sample_format":")") + sample + "\"}");
    REQUIRE_OK(back);
    CHECK(test::snr_db(input, *back) >= (std::string_view(sample) == "s16" ? 300.0 : 30.0));
  }
  CHECK(test::fuzz_reader<ir::Audio>("voc", test::fixture("audio_lossless/sox_s16_stereo_11025.voc")) > 0);
}

TEST_CASE("audio_lossless/8svx: Amiga IFF samples from sox decode and round-trip") {
  const auto audio = ta::load("8svx", "sox_s8_mono_8000.8svx");
  REQUIRE_OK(audio);
  CHECK(audio->sample_rate == 8000);
  CHECK(audio->frame_count() == 2000);
  CHECK(test::snr_db(ta::reference("ref_s16_mono_8000.wav"), *audio) >= 30.0);  // sox wrote it with shaped dither
  const ir::Audio stereo = ta::music(11025, 2, 0.2, 8);
  const auto back = ta::round_trip("8svx", stereo);
  REQUIRE_OK(back);
  CHECK(ta::identical(stereo, *back));
  CHECK(test::fuzz_reader<ir::Audio>("8svx", test::fixture("audio_lossless/sox_s8_mono_8000.8svx")) > 0);
}

TEST_CASE("audio_lossless/sph: NIST SPHERE files from sox decode and round-trip") {
  const auto pcm = ta::load("sph", "sox_s16_stereo_11025.sph");
  REQUIRE_OK(pcm);
  CHECK(ta::identical(ta::reference("ref_s16_stereo_11025.wav"), *pcm));
  const auto ulaw = ta::load("sph", "sox_ulaw_mono_8000.sph");
  REQUIRE_OK(ulaw);
  CHECK(test::snr_db(ta::reference("ref_s16_mono_8000.wav"), *ulaw) >= 30.0);
  const ir::Audio input = ta::music(16000, 1, 0.2, 24);
  const auto back = ta::round_trip("sph", input, R"({"sample_format":"s24"})");
  REQUIRE_OK(back);
  CHECK(ta::identical(input, *back));
  CHECK(test::fuzz_reader<ir::Audio>("sph", test::fixture("audio_lossless/sox_s16_stereo_11025.sph")) > 0);
}

TEST_CASE("audio_lossless/pcm: headerless PCM uses explicit options") {
  const ir::Audio input = ta::noise(22050, 2, 0.1, 16);
  const auto raw = test::write_from("pcm", input, R"({"raw_sample_format":"s16be"})");
  REQUIRE_OK(raw);
  CHECK(raw->size() == input.samples.size() * 2);
  const auto back = test::read_as<ir::Audio>("pcm", *raw, R"({"raw_sample_format":"s16be","raw_sample_rate":22050,"raw_channels":2})");
  REQUIRE_OK(back);
  CHECK(ta::identical(input, *back));
  for (const char* f : {"s24le", "s32be", "f32le", "f64be", "u8", "s8", "alaw", "mulaw"}) {
    INFO(f);
    const std::string options = std::string(R"({"raw_sample_format":")") + f + R"(","raw_sample_rate":22050,"raw_channels":2})";
    const auto encoded = test::write_from("pcm", input, options);
    REQUIRE_OK(encoded);
    const auto decoded = test::read_as<ir::Audio>("pcm", *encoded, options);
    REQUIRE_OK(decoded);
    CHECK(test::snr_db(input, *decoded) >= 30.0);
  }
  CHECK_STATUS(test::read_as<ir::Audio>("pcm", *raw, R"({"raw_sample_format":"s17"})"), Status::InvalidArgument);
}
