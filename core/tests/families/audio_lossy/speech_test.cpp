// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdio>
#include <format>

#include "families/audio_lossy/lossy_test_util.hpp"

using namespace cvt;

TEST_CASE("audio_lossy/speech: decodes like the reference decoders") {
  struct Case {
    const char* format;
    const char* name;
    double min_snr;
  };
  // ffmpeg's own AMR decoders are independent implementations with
  // floating-point postfilters: close but not bit-identical to the 3GPP
  // fixed-point reference that opencore implements.
  for (const Case& c : {Case{"amr", "speech/oc_amrnb_122_8000.amr", 15}, Case{"amr", "speech/oc_amrnb_475_8000.amr", 10},
                        Case{"amr", "speech/oc_amrnb_dtx_795_8000.amr", 8}, Case{"awb", "speech/vo_amrwb_2385_16000.awb", 15},
                        Case{"awb", "speech/vo_amrwb_1265_16000.awb", 10}, Case{"gsm", "speech/sox_speech_8000.gsm", 90}}) {
    INFO(c.name);
    const lossy_test::Match m = lossy_test::against_reference(c.format, c.name);
    std::printf("%-40s frames %zu ref %zu rms %.3g peak %.3g snr %.1f\n", c.name, m.frames, m.reference_frames, m.rms, m.peak,
                m.snr);
    CHECK(m.frames == (std::string(c.format) == "awb" ? 9600u : 4800u));
    CHECK(m.snr >= c.min_snr);
  }
}

TEST_CASE("audio_lossy/amr: encoders, modes and damaged input") {
  const ir::Audio voice = test::sweep(16000, 1, 150.0, 3000.0, 1.0, 0.4);
  std::size_t previous = 0;
  for (const int q : {90, 50, 10}) {
    const auto wb = test::write_from("awb", voice, std::format(R"({{"quality":{}}})", q));
    REQUIRE_OK(wb);
    const auto back = lossy_test::read_audio("awb", *wb);
    REQUIRE_OK(back);
    CHECK(back->frame_count() == 16000);
    if (previous != 0) CHECK(wb->size() < previous);
    previous = wb->size();
  }
  ir::Audio narrow = test::sweep(8000, 1, 150.0, 3000.0, 1.0, 0.4);
  const auto nb = test::write_from("amr", narrow, R"({"bitrate_kbps":13})");
  REQUIRE_OK(nb);
  CHECK(nb->size() == 6 + 50 * 32);  // 12.2 kbit/s: 32-byte frames
  const auto dtx = test::write_from("amr", test::sine(8000, 1, 440, 1.0, 0.0), R"({"dtx":true})");
  REQUIRE_OK(dtx);
  CHECK(dtx->size() < nb->size() / 4);  // silence: comfort-noise frames only
  const auto back = lossy_test::read_audio("amr", *nb);
  REQUIRE_OK(back);
  CHECK(test::aligned_snr_db(narrow, *back, 200) > 5.0);
  for (const auto& [name, format] : {std::pair{"speech/oc_amrnb_122_8000.amr", "amr"}, std::pair{"speech/vo_amrwb_2385_16000.awb", "awb"},
                                     std::pair{"speech/sox_speech_8000.gsm", "gsm"}}) {
    CHECK(test::fuzz_reader<ir::Audio>(format, test::fixture(std::string("audio_lossy/") + name)) > 0);
  }
}
