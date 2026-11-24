// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>

#include "families/audio_lossless/audio_test_util.hpp"

using namespace cvt;
namespace ta = cvt::test::audio;

namespace {

void exact(std::string_view format, std::string_view name, std::string_view reference) {
  INFO(name);
  const auto audio = ta::load(format, name);
  REQUIRE_OK(audio);
  CHECK(ta::identical(ta::reference(reference), *audio));
}

void close(std::string_view format, std::string_view name, std::string_view reference, double min_snr) {
  INFO(name);
  const auto audio = ta::load(format, name);
  REQUIRE_OK(audio);
  const ir::Audio ref = ta::reference(reference);
  CHECK(test::snr_db(ref, *audio) >= min_snr);
}

}  // namespace

TEST_CASE("audio_lossless/aiff: AIFF and AIFF-C variants from afconvert and ffmpeg decode exactly") {
  exact("aiff", "af_s16_stereo_11025.aiff", "ref_s16_stereo_11025.wav");
  exact("aiff", "af_s24_stereo_22050.aiff", "ref_s24_stereo_22050.wav");
  exact("aiff", "af_s32_stereo_22050.aiff", "ref_s24_stereo_22050.wav");
  exact("aifc", "af_fl32_stereo_11025.aifc", "ref_s16_stereo_11025.wav");
  exact("aifc", "ff_sowt_stereo_11025.aifc", "ref_s16_stereo_11025.wav");
  exact("aiff", "ff_id3_s16_mono_8000.aiff", "ref_s16_mono_8000.wav");
  close("aiff", "af_s8_mono_8000.aiff", "ref_s16_mono_8000.wav", 35.0);
  close("aifc", "af_s8_mono_8000.aifc", "ref_s16_mono_8000.wav", 35.0);
  close("aifc", "af_ulaw_mono_8000.aifc", "ref_s16_mono_8000.wav", 30.0);
  close("aifc", "af_alaw_mono_8000.aifc", "ref_s16_mono_8000.wav", 30.0);
  const auto s24 = ta::load("aiff", "af_s24_stereo_22050.aiff");
  REQUIRE_OK(s24);
  CHECK(s24->sample_rate == 22050);  // from the 80-bit extended field
  CHECK(s24->bits_per_sample == 24);
}

TEST_CASE("audio_lossless/aiff: Apple IMA4 matches Apple's decoder") {
  const auto ima = ta::load("aifc", "af_ima4_stereo_11025.aifc");
  REQUIRE_OK(ima);
  const ir::Audio apple = ta::reference("af_ima4_stereo_11025_afconvert_decoded.wav");
  CHECK(ta::identical(apple, *ima));
  CHECK(test::snr_db(ta::reference("ref_s16_stereo_11025.wav"), *ima) >= 20.0);
  CHECK(ta::load("aifc", "af_ima4_mono_8000.aifc").has_value());
}

TEST_CASE("audio_lossless/aiff: ID3 tags in AIFF are read and written only on request") {
  const auto tagged = ta::load("aiff", "ff_id3_s16_mono_8000.aiff");
  REQUIRE_OK(tagged);
  CHECK(tagged->tags.at("title") == "Test tone");
  CHECK(tagged->tags.at("artist") == "Convertible test");
  const auto kept = test::write_from("aiff", *tagged, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  const auto back = test::read_as<ir::Audio>("aiff", *kept);
  REQUIRE_OK(back);
  CHECK(back->tags.at("title") == "Test tone");
  const auto stripped = test::write_from("aiff", *tagged);
  REQUIRE_OK(stripped);
  CHECK(test::str(*stripped).find("Test tone") == std::string::npos);
}

TEST_CASE("audio_lossless/aiff: writer round trips for every sample format") {
  const ir::Audio input = ta::music(22050, 2, 0.15, 24);
  for (const char* format : {"aiff", "aifc"}) {
    for (const char* sample : {"s24", "s32", "f32", "f64", "s16le"}) {
      if (std::string_view(format) == "aiff" && std::string_view(sample) == "s16le") continue;
      INFO(format, " ", sample);
      Bytes encoded;
      const auto back = ta::round_trip(format, ta::on_grid(input, 16), std::string(R"({"sample_format":")") + sample + "\"}", &encoded);
      REQUIRE_OK(back);
      CHECK(ta::identical(ta::on_grid(input, 16), *back));
    }
    for (const char* sample : {"s8", "alaw", "mulaw", "ima4"}) {
      INFO(format, " ", sample);
      const auto back = ta::round_trip(format, input, std::string(R"({"sample_format":")") + sample + "\"}");
      REQUIRE_OK(back);
      CHECK(test::snr_db(input, *back) >= 20.0);
    }
  }
  // Plain AIFF for integer PCM, AIFF-C as soon as a compression type is needed.
  const auto plain = test::write_from("aiff", ta::on_grid(input, 16));
  REQUIRE_OK(plain);
  CHECK(test::str(ByteView(*plain).subspan(8, 4)) == "AIFF");
  const auto compressed = test::write_from("aiff", input, R"({"sample_format":"f32"})");
  REQUIRE_OK(compressed);
  CHECK(test::str(ByteView(*compressed).subspan(8, 4)) == "AIFC");
}

TEST_CASE("audio_lossless/au: Sun/NeXT files decode and round-trip") {
  exact("au", "af_s16_stereo_11025.au", "ref_s16_stereo_11025.wav");
  exact("au", "af_s24_stereo_22050.au", "ref_s24_stereo_22050.wav");
  exact("au", "ff_f32_stereo_11025.au", "ref_s16_stereo_11025.wav");
  exact("au", "sox_s16_stereo_11025.au", "ref_s16_stereo_11025.wav");
  close("au", "af_ulaw_mono_8000.au", "ref_s16_mono_8000.wav", 30.0);
  close("au", "sox_alaw_stereo_11025.au", "ref_s16_stereo_11025.wav", 30.0);
  const ir::Audio input = ta::music(8000, 1, 0.1, 24);
  for (const char* sample : {"s24", "s32", "f32", "f64"}) {
    INFO(sample);
    const auto back = ta::round_trip("au", input, std::string(R"({"sample_format":")") + sample + "\"}");
    REQUIRE_OK(back);
    CHECK(ta::identical(input, *back));
  }
  CHECK(test::fuzz_reader<ir::Audio>("au", test::fixture("audio_lossless/af_ulaw_mono_8000.au")) > 0);
}

TEST_CASE("audio_lossless/caf: PCM, IMA4, mu-law and ALAC in CAF decode exactly") {
  exact("caf", "af_s16_stereo_11025.caf", "ref_s16_stereo_11025.wav");
  exact("caf", "af_s24be_stereo_22050.caf", "ref_s24_stereo_22050.wav");
  exact("caf", "af_alac16_stereo_11025.caf", "ref_s16_stereo_11025.wav");
  exact("caf", "af_alac24_stereo_22050.caf", "ref_s24_stereo_22050.wav");
  exact("caf", "ff_alac_s16_stereo_11025.caf", "ref_s16_stereo_11025.wav");
  close("caf", "af_ulaw_mono_8000.caf", "ref_s16_mono_8000.wav", 30.0);
  // IMA4 in CAF: the packet table trims the last packet to the true length.
  const auto ima = ta::load("caf", "af_ima4_stereo_11025.caf");
  REQUIRE_OK(ima);
  CHECK(ima->frame_count() == 2000);
  const ir::Audio apple = ta::reference("af_ima4_stereo_11025_afconvert_decoded.wav");
  CHECK(ta::differing_samples(apple, *ima, 2000) == 0);
}

TEST_CASE("audio_lossless/caf: writer round trips (PCM exact, ALAC exact, IMA4/G.711 close)") {
  const ir::Audio input = ta::music(16000, 2, 0.6, 16);
  for (const char* sample : {"s16", "s24", "s32", "f32", "f64", "alac"}) {
    INFO(sample);
    const auto back = ta::round_trip("caf", input, std::string(R"({"sample_format":")") + sample + "\"}");
    REQUIRE_OK(back);
    CHECK(ta::identical(input, *back));
  }
  for (const char* sample : {"ima4", "alaw", "mulaw", "s8"}) {
    INFO(sample);
    const auto back = ta::round_trip("caf", input, std::string(R"({"sample_format":")") + sample + "\"}");
    REQUIRE_OK(back);
    CHECK(back->frame_count() == input.frame_count());
    CHECK(test::snr_db(input, *back) >= 20.0);
  }
  // Multichannel layouts survive through the 'chan' chunk.
  ir::Audio six = ta::noise(8000, 6, 0.05, 16);
  six.channel_mask = 0x60F;
  const auto back = ta::round_trip("caf", six);
  REQUIRE_OK(back);
  CHECK(back->channel_mask == 0x60F);
  CHECK(ta::identical(six, *back));
}

TEST_CASE("audio_lossless/aiff,au,caf: damaged files fail cleanly") {
  CHECK(test::fuzz_reader<ir::Audio>("aiff", test::fixture("audio_lossless/af_s8_mono_8000.aiff")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("aifc", test::fixture("audio_lossless/af_ima4_mono_8000.aifc")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("caf", test::fixture("audio_lossless/af_alac16_stereo_11025.caf")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("caf", test::fixture("audio_lossless/af_ulaw_mono_8000.caf")) > 0);
  CHECK_STATUS(test::read_as<ir::Audio>("caf", test::bytes("caff\x00\x01\x00\x00")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Audio>("aiff", test::bytes("FORM\x00\x00\x00\x04" "AIFF")), Status::Decode);
}
