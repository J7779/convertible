// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "families/audio_lossless/audio_dsp.hpp"
#include "families/audio_lossless/audio_test_util.hpp"

using namespace cvt;
namespace ta = cvt::test::audio;

namespace {

void check_decodes_exactly(std::string_view format, std::string_view name, std::string_view reference,
                           std::uint16_t bits, bool is_float = false) {
  INFO(name);
  const auto ref = ta::reference(reference);
  REQUIRE(ref.valid());
  const auto audio = ta::load(format, name);
  REQUIRE_OK(audio);
  CHECK(ta::identical(ref, *audio));
  CHECK(audio->bits_per_sample == bits);
  CHECK(audio->float_samples == is_float);
}

}  // namespace

TEST_CASE("audio_lossless/wav: decodes reference PCM written by Python's wave module") {
  const auto s16 = ta::load("wav", "ref_s16_stereo_11025.wav");
  REQUIRE_OK(s16);
  CHECK(s16->sample_rate == 11025);
  CHECK(s16->channels == 2);
  CHECK(s16->frame_count() == 2000);
  CHECK(s16->bits_per_sample == 16);
  // Known first samples: 0.45 sin + noise on the left, a sweep on the right.
  CHECK(s16->samples[0] * 32768.0f == -479.0f);
  const auto u8 = ta::load("wav", "ref_u8_mono_8000.wav");
  REQUIRE_OK(u8);
  CHECK(u8->bits_per_sample == 8);
  CHECK(u8->frame_count() == 2000);
  const auto six = ta::load("wav", "ref_s16_6ch_16000.wav");  // legacy header with 6 channels
  REQUIRE_OK(six);
  CHECK(six->channels == 6);
  CHECK(six->frame_count() == 800);
}

TEST_CASE("audio_lossless/wav: every afconvert and ffmpeg/sox PCM variant decodes bit-exactly") {
  check_decodes_exactly("wav", "af_s24_stereo_22050.wav", "ref_s24_stereo_22050.wav", 24);
  check_decodes_exactly("wav", "af_f32_stereo_11025.wav", "ref_s16_stereo_11025.wav", 32, true);
  check_decodes_exactly("wav", "af_f64_mono_8000.wav", "ref_s16_mono_8000.wav", 64, true);
  check_decodes_exactly("wav", "af_rf64_stereo_11025.wav", "ref_s16_stereo_11025.wav", 16);
  check_decodes_exactly("wav", "af_bw64_stereo_22050.wav", "ref_s24_stereo_22050.wav", 24);
  check_decodes_exactly("wav", "ff_rf64_s16_stereo_11025.wav", "ref_s16_stereo_11025.wav", 16);
  check_decodes_exactly("wav", "ff_extensible_s24_stereo_22050.wav", "ref_s24_stereo_22050.wav", 24);
  check_decodes_exactly("wav", "ff_stream_s16_stereo_11025.wav", "ref_s16_stereo_11025.wav", 16);
  check_decodes_exactly("wav", "sox_stream_s16_stereo_11025.wav", "ref_s16_stereo_11025.wav", 16);
  check_decodes_exactly("w64", "af_s16_stereo_11025.w64", "ref_s16_stereo_11025.wav", 16);
  check_decodes_exactly("w64", "ff_s24_stereo_22050.w64", "ref_s24_stereo_22050.wav", 24);
}

TEST_CASE("audio_lossless/wav: 8-bit and 32-bit sources keep their precision") {
  // afconvert's 8-bit file is the 16-bit reference rounded; compare with an 8-bit grid.
  const auto u8 = ta::load("wav", "af_u8_mono_8000.wav");
  const ir::Audio ref = ta::reference("ref_s16_mono_8000.wav");
  REQUIRE_OK(u8);
  CHECK(ta::max_error(*u8, ref) <= 1.0 / 128.0);
  const auto s32 = ta::load("wav", "af_s32_stereo_22050.wav");
  REQUIRE_OK(s32);
  CHECK(ta::identical(ta::reference("ref_s24_stereo_22050.wav"), *s32));
  CHECK(s32->bits_per_sample == 32);
}

TEST_CASE("audio_lossless/wav: extensible channel masks and multichannel layouts") {
  const auto af = ta::load("wav", "af_s16_51_16000.wav");
  REQUIRE_OK(af);
  CHECK(af->channels == 6);
  CHECK(af->channel_mask == 0x60F);  // 5.1 with side surrounds (MPEG_5_1_A)
  CHECK(ta::identical(ta::reference("ref_s16_6ch_16000.wav"), *af));
  const auto ff = ta::load("wav", "ff_extensible_s16_6ch_16000.wav");
  REQUIRE_OK(ff);
  CHECK(ff->channel_mask != 0);
  CHECK(ta::identical(ta::reference("ref_s16_6ch_16000.wav"), *ff));
}

TEST_CASE("audio_lossless/wav: G.711 and ADPCM match the reference decoders") {
  // A-law and mu-law: afconvert encoded the 16-bit reference; G.711 keeps ~38 dB.
  const ir::Audio ref = ta::reference("ref_s16_mono_8000.wav");
  for (const char* name : {"af_ulaw_mono_8000.wav", "af_alaw_mono_8000.wav"}) {
    INFO(name);
    const auto audio = ta::load("wav", name);
    REQUIRE_OK(audio);
    CHECK(test::snr_db(ref, *audio) >= 30.0);
  }
  // ADPCM and GSM: bit-exact against Apple's decoder, and the fact chunk trims the padding.
  struct Case {
    const char* file;
    const char* decoded;
    const char* reference;
    double min_snr;
  };
  for (const Case c : {Case{"sox_imaadpcm_mono_8000.wav", "sox_imaadpcm_mono_8000_afconvert_decoded.wav", "ref_s16_mono_8000.wav", 25.0},
                       Case{"ff_imaadpcm_stereo_11025.wav", "ff_imaadpcm_stereo_11025_afconvert_decoded.wav", "ref_s16_stereo_11025.wav", 20.0},
                       Case{"sox_msadpcm_stereo_11025.wav", "sox_msadpcm_stereo_11025_afconvert_decoded.wav", "ref_s16_stereo_11025.wav", 20.0},
                       Case{"ff_msadpcm_stereo_11025.wav", "ff_msadpcm_stereo_11025_afconvert_decoded.wav", "ref_s16_stereo_11025.wav", 15.0},
                       Case{"sox_gsm610_mono_8000.wav", "sox_gsm610_mono_8000_afconvert_decoded.wav", "ref_s16_mono_8000.wav", 12.0}}) {
    INFO(c.file);
    const auto audio = ta::load("wav", c.file);
    REQUIRE_OK(audio);
    const ir::Audio apple = ta::reference(c.decoded);
    const ir::Audio original = ta::reference(c.reference);
    CHECK(audio->frame_count() == original.frame_count());
    CHECK(ta::differing_samples(*audio, apple, audio->frame_count()) == 0);
    CHECK(test::snr_db(original, *audio) >= c.min_snr);
  }
  CHECK(ta::load("wav", "sox_msadpcm_mono_8000.wav").has_value());
}

TEST_CASE("audio_lossless/wav: tags from LIST/INFO and bext are read and written only on request") {
  const auto info = ta::load("wav", "ff_info_s16_mono_8000.wav");
  REQUIRE_OK(info);
  CHECK(info->tags.at("title") == "Test tone");
  CHECK(info->tags.at("artist") == "Convertible test");
  CHECK(info->tags.at("comment") == "synthetic");
  CHECK(ta::identical(ta::reference("ref_s16_mono_8000.wav"), *info));
  const auto bext = ta::load("wav", "ff_bext_s16_mono_8000.wav");
  REQUIRE_OK(bext);
  CHECK(bext->tags.at("title") == "Test tone");

  const auto stripped = test::write_from("wav", *info);
  REQUIRE_OK(stripped);
  CHECK(test::str(*stripped).find("Test tone") == std::string::npos);
  const auto kept = test::write_from("wav", *info, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  const auto back = test::read_as<ir::Audio>("wav", *kept);
  REQUIRE_OK(back);
  CHECK(back->tags.at("title") == "Test tone");
  CHECK(back->tags.at("artist") == "Convertible test");
}

TEST_CASE("audio_lossless/wav: every writer sample format round-trips within its precision") {
  const ir::Audio input = ta::music(22050, 2, 0.2, 24);
  struct Case {
    const char* format;
    double min_snr;
    bool exact;
  };
  for (const Case c : {Case{"u8", 40.0, false}, Case{"s16", 85.0, false}, Case{"s24", 130.0, true}, Case{"s32", 130.0, true},
                       Case{"f32", 130.0, true}, Case{"f64", 130.0, true}, Case{"alaw", 30.0, false},
                       Case{"mulaw", 30.0, false}, Case{"ima_adpcm", 20.0, false}, Case{"ms_adpcm", 20.0, false}}) {
    INFO(c.format);
    Bytes encoded;
    const auto decoded = ta::round_trip("wav", input, std::string(R"({"sample_format":")") + c.format + "\"}", &encoded);
    REQUIRE_OK(decoded);
    CHECK(decoded->frame_count() == input.frame_count());
    if (c.exact) {
      CHECK(ta::identical(input, *decoded));
    } else {
      CHECK(test::snr_db(input, *decoded) >= c.min_snr);
    }
  }
  // Same for Wave64.
  const auto w64 = ta::round_trip("w64", input, R"({"sample_format":"s24"})");
  REQUIRE_OK(w64);
  CHECK(ta::identical(input, *w64));
}

TEST_CASE("audio_lossless/wav: auto keeps the source precision; lower depth is smaller") {
  const ir::Audio s24 = ta::music(8000, 1, 0.1, 24);
  const auto out = test::write_from("wav", s24);
  REQUIRE_OK(out);
  const auto back = test::read_as<ir::Audio>("wav", *out);
  REQUIRE_OK(back);
  CHECK(back->bits_per_sample == 24);
  CHECK(ta::identical(s24, *back));
  const auto s16 = test::write_from("wav", s24, R"({"sample_format":"s16"})");
  const auto u8 = test::write_from("wav", s24, R"({"sample_format":"u8"})");
  const auto ima = test::write_from("wav", s24, R"({"sample_format":"ima_adpcm"})");
  REQUIRE_OK(s16);
  REQUIRE_OK(u8);
  REQUIRE_OK(ima);
  CHECK(u8->size() < s16->size());
  CHECK(ima->size() < u8->size());
  CHECK(s16->size() < out->size());
}

TEST_CASE("audio_lossless/wav: containers RF64/BW64 and the extensible header are written correctly") {
  const ir::Audio input = ta::noise(16000, 6, 0.05, 16);
  for (const char* container : {"riff", "rf64", "bw64"}) {
    INFO(container);
    const auto bytes = test::write_from("wav", input, std::string(R"({"wav_container":")") + container + "\"}");
    REQUIRE_OK(bytes);
    const std::string head = test::str(ByteView(*bytes).first(4));
    CHECK(head == (std::string(container) == "riff" ? "RIFF" : std::string(container) == "rf64" ? "RF64" : "BW64"));
    const auto back = test::read_as<ir::Audio>("wav", *bytes);
    REQUIRE_OK(back);
    CHECK(ta::identical(input, *back));
    CHECK(back->channel_mask == 0x3F);
  }
}

TEST_CASE("audio_lossless/wav: options for rate, channels and normalisation") {
  const ir::Audio input = ta::music(44100, 2, 0.2, 16);
  const auto resampled = ta::round_trip("wav", input, R"({"sample_rate":22050,"channels":1})");
  REQUIRE_OK(resampled);
  CHECK(resampled->sample_rate == 22050);
  CHECK(resampled->channels == 1);
  CHECK(resampled->frame_count() == input.frame_count() / 2);
  const auto normalised = ta::round_trip("wav", input, R"({"normalize":"peak","sample_format":"f32"})");
  REQUIRE_OK(normalised);
  double peak = 0.0;
  for (const float s : normalised->samples) peak = std::max(peak, static_cast<double>(std::abs(s)));
  CHECK_APPROX(peak, 0.891, 0.002);
  CHECK_STATUS(test::write_from("wav", input, R"({"sample_format":"s12"})"), Status::InvalidArgument);
}

TEST_CASE("audio_lossless/wav: dither, resampling quality and loudness options act as documented") {
  // A quiet 24-bit tone reduced to 8 bits: without dither the error is
  // signal-correlated (deterministic rounding), with TPDF it is noise.
  const ir::Audio quiet = ta::on_grid(test::sine(8000, 1, 440.0, 0.5, 0.02), 24);
  const auto plain = ta::round_trip("wav", quiet, R"({"sample_format":"u8","dither":"none"})");
  const auto tpdf = ta::round_trip("wav", quiet, R"({"sample_format":"u8"})");
  const auto shaped = ta::round_trip("wav", quiet, R"({"sample_format":"u8","dither":"shaped"})");
  REQUIRE_OK(plain);
  REQUIRE_OK(tpdf);
  REQUIRE_OK(shaped);
  CHECK(plain->samples != tpdf->samples);
  CHECK(tpdf->samples != shaped->samples);
  CHECK(test::snr_db(quiet, *tpdf) > 5.0);
  // Exact input is never dithered.
  const ir::Audio exact16 = ta::noise(8000, 1, 0.1, 16);
  const auto undithered = ta::round_trip("wav", exact16, R"({"sample_format":"s16","dither":"tpdf"})");
  REQUIRE_OK(undithered);
  CHECK(ta::identical(exact16, *undithered));
  // Resampling quality: "best" keeps a 10 kHz tone cleaner than "fast" at 48k -> 44.1k.
  const ir::Audio tone = test::sine(48000, 1, 10000.0, 0.2, 0.5);
  const auto fast = ta::round_trip("wav", tone, R"({"sample_rate":44100,"resample_quality":"fast","sample_format":"f32"})");
  const auto best = ta::round_trip("wav", tone, R"({"sample_rate":44100,"resample_quality":"best","sample_format":"f32"})");
  REQUIRE_OK(fast);
  REQUIRE_OK(best);
  const ir::Audio ideal = test::sine(44100, 1, 10000.0, 0.2, 0.5);
  CHECK(test::snr_db(ideal, *best) > test::snr_db(ideal, *fast));
  // Loudness normalisation targets -16 LUFS.
  const auto loud = ta::round_trip("wav", test::sine(48000, 2, 1000.0, 3.0, 0.05),
                                   R"({"normalize":"loudness","sample_format":"f32"})");
  REQUIRE_OK(loud);
  CHECK_APPROX(audio::dsp::integrated_loudness(*loud), -16.0, 0.3);
  CHECK_STATUS(test::write_from("wav", quiet, R"({"dither":"loud"})"), Status::InvalidArgument);
}

TEST_CASE("audio_lossless/wav: damaged and hostile files fail cleanly") {
  const Bytes valid = test::fixture("audio_lossless/ref_s16_mono_8000.wav");
  CHECK(test::fuzz_reader<ir::Audio>("wav", valid) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("wav", test::fixture("audio_lossless/sox_gsm610_mono_8000.wav")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("wav", test::fixture("audio_lossless/ff_msadpcm_stereo_11025.wav")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("wav", test::fixture("audio_lossless/ff_imaadpcm_stereo_11025.wav")) > 0);
  CHECK(test::fuzz_reader<ir::Audio>("w64", test::fixture("audio_lossless/af_s16_stereo_11025.w64")) > 0);
  CHECK_STATUS(test::read_as<ir::Audio>("wav", ByteView(valid).first(30)), Status::Decode);
  // A header promising 2^32 samples on a tiny file: read what is there.
  Bytes huge = valid;
  huge[40] = std::byte{0xF0};
  huge[41] = std::byte{0xFF};
  huge[42] = std::byte{0xFF};
  huge[43] = std::byte{0x7F};
  std::vector<std::string> warnings;
  const auto truncated = test::read_as<ir::Audio>("wav", huge, {}, &warnings);
  REQUIRE_OK(truncated);
  CHECK(truncated->frame_count() == 2000);
  CHECK(!warnings.empty());
  // The limit on decoded data applies.
  Limits tight;
  tight.max_decompressed_bytes = 1000;
  CHECK_STATUS(test::read_as<ir::Audio>("wav", valid, {}, nullptr, tight), Status::Limit);
}
