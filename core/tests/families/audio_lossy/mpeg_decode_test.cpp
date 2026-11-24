// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdio>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/audio_lossy/lossy_test_util.hpp"

using namespace cvt;

namespace {

struct Case {
  const char* format;
  const char* name;
  std::size_t frames;  ///< expected decoded length
};

// Lengths: files with a LAME/Info tag decode to exactly the encoder's input
// (0.6 s); files without one keep the encoder and decoder delays, as
// ffmpeg's and mpg123's outputs do.
constexpr Case cases[] = {
    {"mp3", "mpeg/lame_cbr128_js_44100.mp3", 26460},     // MPEG-1, joint stereo (M/S), Info tag
    {"mp3", "mpeg/lame_v2_st_44100.mp3", 26460},         // VBR, Xing tag, stereo
    {"mp3", "mpeg/lame_cbr64_mono_22050.mp3", 13230},    // MPEG-2 LSF, mono
    {"mp3", "mpeg/lame_cbr24_mono_8000.mp3", 4800},      // MPEG-2.5
    {"mp3", "mpeg/lame_free200_js_44100.mp3", 27648},    // free format, no tag
    {"mp3", "mpeg/lame_cbr160_nores_32000.mp3", 19200},  // no bit reservoir
    {"mp3", "mpeg/lame_notag_st_48000.mp3", 20736},      // no tag: delays kept
    {"mp2", "mpeg/ff_mp2_192_st_48000.mp2", 28800},
    {"mp2", "mpeg/ff_mp2_48_mono_24000.mp2", 14976},     // MPEG-2 LSF Layer II
    {"mp2", "mpeg/twolame_160_js_44100.mp2", 26496},     // joint stereo (intensity bound)
    {"mp2", "mpeg/twolame_64_mono_32000.mp2", 19584},    // low-rate allocation table
};

}  // namespace

TEST_CASE("audio_lossy/mp3: decodes like independent decoders (ffmpeg, mpg123)") {
  // The references are 16-bit: the expected difference is the rounding of
  // the reference itself (RMS about 0.3 LSB per channel, peak 1 LSB).
  for (const Case& c : cases) {
    INFO(c.name);
    const lossy_test::Match m = lossy_test::against_reference(c.format, c.name);
    std::printf("%-40s frames %zu rms %.3g (%.3f LSB) peak %.3g snr %.1f\n", c.name, m.frames, m.rms, m.rms * 32768, m.peak,
                m.snr);
    CHECK(m.frames == c.frames);
    CHECK(m.rms <= 0.7 / 32768);
    CHECK(m.peak <= 1.1 / 32768);
  }
}

TEST_CASE("audio_lossy/mp3: tags") {
  const auto v23 = lossy_test::read_audio("mp3", test::fixture("audio_lossy/mpeg/lame_id3v23_mono_22050.mp3"));
  REQUIRE_OK(v23);
  CHECK(v23->tags.at("title") == "Test Tone");
  CHECK(v23->tags.at("artist") == "Convertible Fixtures");
  CHECK(v23->tags.at("album") == "Synthetic");
  CHECK(v23->tags.at("genre") == "Ambient");
  const auto v1 = lossy_test::read_audio("mp3", test::fixture("audio_lossy/mpeg/lame_id3v1_mono_22050.mp3"));
  REQUIRE_OK(v1);
  CHECK(v1->tags.at("title") == "Test Tone");
  CHECK(v1->tags.at("date") == "2024");
  CHECK(v1->tags.at("track") == "3");
  CHECK(v1->frame_count() == 13230);
  // ID3v2.4 with an attached picture: the picture is skipped, the audio intact.
  const auto cover = lossy_test::read_audio("mp3", test::fixture("audio_lossy/mpeg/ff_id3v24_cover_mono_22050.mp3"));
  REQUIRE_OK(cover);
  CHECK(cover->tags.at("title") == "Cover Test");
  CHECK(cover->frame_count() > 13000);
}

TEST_CASE("audio_lossy/mp3: detection and conversion through the engine") {
  for (const auto& [name, format] : {std::pair{"mpeg/lame_cbr128_js_44100.mp3", "mp3"}, std::pair{"mpeg/lame_id3v23_mono_22050.mp3", "mp3"},
                                     std::pair{"mpeg/ff_mp2_192_st_48000.mp2", "mp2"}, std::pair{"mpeg/twolame_64_mono_32000.mp2", "mp2"}}) {
    INFO(name);
    const test::Converted wav = test::convert(test::fixture(std::string("audio_lossy/") + name), "", "wav");
    CHECK(wav.ok());
    const auto detected = detect_format(Registry::instance(), test::fixture(std::string("audio_lossy/") + name), "");
    REQUIRE(detected.has_value());
    CHECK(detected->format->id == format);
  }
}

TEST_CASE("audio_lossy/mp3: damaged input is an error or a partial decode, never a crash") {
  for (const char* name : {"mpeg/lame_cbr24_mono_8000.mp3", "mpeg/ff_mp2_48_mono_24000.mp2", "mpeg/lame_free200_js_44100.mp3"}) {
    const Bytes data = test::fixture(std::string("audio_lossy/") + name);
    CHECK(test::fuzz_reader<ir::Audio>("mp3", ByteView(data).first(std::min<std::size_t>(data.size(), 3000))) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Audio>("mp3", test::random_bytes(4000, 3)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Audio>("mp3", as_bytes(std::string_view("ID3\x04\0\0\0\0\0\0", 10))), Status::Decode);
}
