// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdio>
#include <format>

#include "codec/registry.hpp"
#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/audio_lossy/lossy_signals.hpp"

using namespace cvt;
using lossy_test::music;
using lossy_test::round_trip;
using lossy_test::Trip;

namespace {

struct Case {
  const char* format;
  const char* name;
  std::size_t frames;
  double min_snr;  ///< against ffmpeg's decode (16-bit), first 0.2 s
};

}  // namespace

TEST_CASE("audio_lossy/aac: decodes files of independent encoders like ffmpeg") {
  // Gapless MPEG-4 files (iTunSMPB, edit lists) decode to exactly the 0.6 s
  // the encoders were given; ADTS has no such information. ffmpeg's own
  // AAC encoder uses perceptual noise substitution, whose noise differs
  // between decoders by design, hence the lower bound for those files.
  for (const Case& c : {Case{"m4a", "aac/af_aaclc_128_st_44100.m4a", 26460, 70}, Case{"aac", "aac/af_aaclc_64_mono_22050.aac", 15360, 70},
                        Case{"caf", "aac/af_aaclc_112_st_48000.caf", 0, 70}, Case{"m4a", "aac/fdk_lc_vbr4_st_44100.m4a", 26460, 70},
                        Case{"aac", "aac/fdk_lc_64_mono_16000.aac", 12288, 70}, Case{"m4a", "aac/ff_aac_128_st_44100.m4a", 26460, 5},
                        Case{"aac", "aac/ff_aac_48_mono_22050.aac", 14336, 4}}) {
    INFO(c.name);
    const lossy_test::Match m = lossy_test::against_reference(c.format, c.name);
    std::printf("%-36s frames %zu rms %.3g snr %.1f\n", c.name, m.frames, m.rms, m.snr);
    if (c.frames != 0) CHECK(m.frames == c.frames);
    CHECK(m.snr >= c.min_snr);
  }
}

TEST_CASE("audio_lossy/aac: HE-AAC (SBR) and HE-AAC v2") {
  // Spectral band replication is not specified sample-exactly: compare
  // with the encoder's input instead (alignment and length) and with
  // ffmpeg's decode loosely.
  const auto he = lossy_test::read_audio("m4a", test::fixture("audio_lossy/aac/af_heaac_48_st_44100.m4a"));
  REQUIRE_OK(he);
  CHECK(he->sample_rate == 44100);
  CHECK(he->channels == 2);
  CHECK(he->frame_count() == 26460);
  const auto mono = lossy_test::read_audio("m4a", test::fixture("audio_lossy/aac/af_heaac_24_mono_22050.m4a"));
  REQUIRE_OK(mono);
  CHECK(mono->sample_rate == 22050);
  CHECK(mono->frame_count() == 13230);
  // v2: parametric stereo is not decoded (see the family notes): mono core.
  const auto v2 = lossy_test::read_audio("m4a", test::fixture("audio_lossy/aac/af_heaacv2_32_st_44100.m4a"));
  REQUIRE_OK(v2);
  CHECK(v2->sample_rate == 44100);
  CHECK(v2->frame_count() == 26460);
}

TEST_CASE("audio_lossy/aac: encoder quality levels, gapless m4a, ADTS") {
  const ir::Audio source = music(44100, 2, 1.0);
  std::size_t previous = 0;
  for (const auto& [quality, min_snr] : {std::pair{90, 14.0}, std::pair{75, 12.0}, std::pair{50, 9.0}, std::pair{20, 4.0}}) {
    INFO(std::format("quality {}", quality));
    const Trip t = round_trip("m4a", source, std::format(R"({{"quality":{}}})", quality));
    std::printf("m4a q%d: %zu bytes, snr %.2f dB\n", quality, t.bytes, t.snr);
    CHECK(t.frames == source.frame_count());
    CHECK(t.snr >= min_snr);
    if (previous != 0) CHECK(t.bytes < previous);
    previous = t.bytes;
  }
  const Trip adts = round_trip("aac", music(22050, 1, 0.5), R"({"bitrate_kbps":48})", 4000);
  std::printf("adts 48k: %zu bytes, snr %.2f\n", adts.bytes, adts.snr);
  CHECK(adts.snr > 8.0);
  // Tags survive in iTunes atoms when kept.
  ir::Audio tagged = music(44100, 2, 0.3);
  tagged.tags = {{"title", "Tone"}, {"track", "2/9"}};
  const auto kept = test::write_from("m4a", tagged, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  const auto back = lossy_test::read_audio("m4a", *kept);
  REQUIRE_OK(back);
  CHECK(back->tags.at("title") == "Tone");
  CHECK(back->tags.at("track") == "2/9");
  CHECK(detect_format(Registry::instance(), *kept, "")->format->id == "m4a");
}

TEST_CASE("audio_lossy/aac: Apple Lossless stays lossless and other codecs still read") {
  // An ALAC .m4a is detected as "alac": its compress path is the lossless
  // writer, never the AAC encoder.
  const Bytes alac = test::fixture("audio_lossless/af_alac16_stereo_11025.m4a");
  const auto detected = detect_format(Registry::instance(), alac, "song.m4a");
  REQUIRE(detected.has_value());
  CHECK(detected->format->id == "alac");
  // Through the AAC family's m4a reader ALAC is handed to the generic reader.
  const auto via_m4a = lossy_test::read_audio("m4a", alac);
  const auto via_alac = test::read_as<ir::Audio>("alac", alac);
  REQUIRE_OK(via_m4a);
  REQUIRE_OK(via_alac);
  CHECK(via_m4a->samples == via_alac->samples);
}

TEST_CASE("audio_lossy/aac: codec registry and damaged input") {
  const auto& media = Registry::instance().media();
  REQUIRE(media.has_audio_decoder("aac"));
  REQUIRE(media.has_audio_encoder("aac"));
  for (const char* name : {"aac/af_aaclc_64_mono_22050.aac", "aac/fdk_lc_vbr4_st_44100.m4a"}) {
    const std::string n(name);
    const std::string format = n.ends_with(".aac") ? "aac" : "m4a";
    const Bytes data = test::fixture(std::string("audio_lossy/") + name);
    std::size_t runs = 0;
    for (std::size_t len = 0; len < data.size(); len += std::max<std::size_t>(1, data.size() / 80)) {
      (void)lossy_test::read_audio(format, ByteView(data).first(len));
      ++runs;
    }
    for (std::size_t i = 0; i < data.size(); i += std::max<std::size_t>(1, data.size() / 80)) {
      Bytes broken = data;
      broken[i] = static_cast<std::byte>(std::to_integer<int>(broken[i]) ^ 0x5A);
      (void)lossy_test::read_audio(format, broken);
      ++runs;
    }
    CHECK(runs > 100);
  }
  CHECK_STATUS(lossy_test::read_audio("aac", test::random_bytes(3000, 9)), Status::Decode);
}
