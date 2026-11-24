// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "families/audio_lossless/audio_dsp.hpp"
#include "families/audio_lossless/audio_test_util.hpp"

using namespace cvt;
namespace dsp = cvt::audio::dsp;

namespace {

std::vector<float> tone(std::uint32_t rate, double hz, std::size_t frames, double amplitude = 0.5, double phase = 0.0) {
  std::vector<float> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    out[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate + phase));
  }
  return out;
}

// SNR of `test` against the ideal tone, ignoring `margin` frames at each end.
double tone_snr(const std::vector<float>& test, std::uint32_t rate, double hz, double amplitude, std::size_t margin) {
  double signal = 0.0, noise = 0.0;
  for (std::size_t i = margin; i + margin < test.size(); ++i) {
    const double ideal = amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
    signal += ideal * ideal;
    noise += (test[i] - ideal) * (test[i] - ideal);
  }
  return noise > 0.0 ? 10.0 * std::log10(signal / noise) : 300.0;
}

double rms(const std::vector<float>& v, std::size_t margin) {
  double sum = 0.0;
  std::size_t n = 0;
  for (std::size_t i = margin; i + margin < v.size(); ++i, ++n) sum += static_cast<double>(v[i]) * v[i];
  return n == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(n));
}

struct QualityCase {
  dsp::ResampleQuality quality;
  double min_snr;
  double min_stopband;
};

constexpr QualityCase quality_cases[] = {
    {dsp::ResampleQuality::Fast, 60.0, 65.0},
    {dsp::ResampleQuality::Medium, 90.0, 95.0},
    {dsp::ResampleQuality::High, 110.0, 120.0},
    {dsp::ResampleQuality::Best, 115.0, 130.0},
};

}  // namespace

TEST_CASE("audio_lossless/dsp: resampler passband SNR per quality (44.1k -> 48k, 48k -> 44.1k)") {
  for (const QualityCase& q : quality_cases) {
    for (const auto& [in, out] : {std::pair<std::uint32_t, std::uint32_t>{44100, 48000}, {48000, 44100}}) {
      CAPTURE(in);
      CAPTURE(out);
      CAPTURE(static_cast<int>(q.quality));
      for (const double hz : {997.0, 9000.0, 0.9 * 22050.0 * (q.quality == dsp::ResampleQuality::Fast ? 0.9 : 1.0)}) {
        CAPTURE(hz);
        const std::vector<float> input = tone(in, hz, 8000);
        auto output = dsp::resample(input, 1, in, out, q.quality);
        REQUIRE_OK(output);
        CHECK(output->size() == (8000ull * out + in - 1) / in);
        CHECK(tone_snr(*output, out, hz, 0.5, 600) >= q.min_snr);
      }
    }
  }
}

TEST_CASE("audio_lossless/dsp: resampler stopband attenuation (no aliasing)") {
  // 48 kHz -> 32 kHz: a 20 kHz tone lies above the new Nyquist frequency and
  // must vanish instead of folding down to 12 kHz.
  for (const QualityCase& q : quality_cases) {
    CAPTURE(static_cast<int>(q.quality));
    const std::vector<float> input = tone(48000, 20000.0, 12000, 0.5);
    auto output = dsp::resample(input, 1, 48000, 32000, q.quality);
    REQUIRE_OK(output);
    const double attenuation = -20.0 * std::log10(rms(*output, 800) / (0.5 / std::sqrt(2.0)) + 1e-30);
    CHECK(attenuation >= q.min_stopband);
  }
  // Upsampling must not leave images: 8 kHz -> 48 kHz of a 3 kHz tone.
  const std::vector<float> input = tone(8000, 3000.0, 4000, 0.5);
  auto output = dsp::resample(input, 1, 8000, 48000, dsp::ResampleQuality::High);
  REQUIRE_OK(output);
  CHECK(tone_snr(*output, 48000, 3000.0, 0.5, 3000) >= 100.0);
}

TEST_CASE("audio_lossless/dsp: resampler sweep keeps a flat passband") {
  // A 20 Hz .. 18 kHz sweep at 44.1 kHz -> 96 kHz -> 44.1 kHz returns almost unchanged.
  const ir::Audio sweep = test::sweep(44100, 1, 20.0, 18000.0, 0.5, 0.5);
  auto up = dsp::resample(sweep.samples, 1, 44100, 96000, dsp::ResampleQuality::High);
  REQUIRE_OK(up);
  auto down = dsp::resample(*up, 1, 96000, 44100, dsp::ResampleQuality::High);
  REQUIRE_OK(down);
  REQUIRE(down->size() == sweep.samples.size());
  double signal = 0.0, noise = 0.0;
  for (std::size_t i = 500; i + 500 < down->size(); ++i) {
    signal += static_cast<double>(sweep.samples[i]) * sweep.samples[i];
    noise += ((*down)[i] - sweep.samples[i]) * ((*down)[i] - sweep.samples[i]);
  }
  CHECK(10.0 * std::log10(signal / noise) >= 90.0);
}

TEST_CASE("audio_lossless/dsp: resampler streaming is chunk-independent and length exact") {
  const ir::Audio input = test::white_noise(44100, 2, 0.25, 5, 0.5);
  auto whole = dsp::resample(input.samples, 2, 44100, 48000);
  REQUIRE_OK(whole);
  dsp::Resampler r;
  REQUIRE_OK(r.configure(44100, 48000, 2));
  std::vector<float> pieces;
  std::size_t at = 0;
  std::size_t step = 1;
  while (at < input.samples.size()) {
    const std::size_t n = std::min(input.samples.size() - at, step * 2);
    r.process(std::span<const float>(input.samples).subspan(at, n), pieces);
    at += n;
    step = step * 3 % 997 + 1;
  }
  r.flush(pieces);
  CHECK(pieces == *whole);
  for (const std::uint64_t frames : {0ull, 1ull, 2ull, 147ull, 160ull, 1001ull}) {
    for (const auto& [a, b] : {std::pair<std::uint32_t, std::uint32_t>{44100, 48000}, {48000, 44100}, {8000, 44101}}) {
      std::vector<float> in(static_cast<std::size_t>(frames), 0.25f);
      auto out = dsp::resample(in, 1, a, b, dsp::ResampleQuality::Fast);
      REQUIRE_OK(out);
      CHECK(out->size() == (frames * b + a - 1) / a);
    }
  }
}

TEST_CASE("audio_lossless/dsp: resampler handles awkward ratios and no clicks at the edges") {
  // 44100 -> 44101 uses interpolated phases.
  const std::vector<float> input = tone(44100, 1000.0, 20000);
  auto output = dsp::resample(input, 1, 44100, 44101, dsp::ResampleQuality::High);
  REQUIRE_OK(output);
  CHECK(tone_snr(*output, 44101, 1000.0, 0.5, 1000) >= 100.0);
  // A DC signal stays DC away from the edges and starts smoothly (no overshoot above 1.1x).
  std::vector<float> dc(4000, 0.5f);
  auto dc_out = dsp::resample(dc, 1, 48000, 44100);
  REQUIRE_OK(dc_out);
  for (std::size_t i = 300; i + 300 < dc_out->size(); ++i) CHECK_APPROX((*dc_out)[i], 0.5, 1e-5);
  for (const float s : *dc_out) CHECK(std::abs(s) <= 0.56f);
  CHECK_STATUS(dsp::resample(dc, 1, 0, 44100), Status::InvalidArgument);
}

TEST_CASE("audio_lossless/dsp: channel mixing matrices") {
  using namespace dsp;
  SUBCASE("stereo to mono is the mean") {
    const MixMatrix m = mix_matrix(2, 0, 1, 0);
    CHECK(m.at(0, 0) == 0.5f);
    CHECK(m.at(0, 1) == 0.5f);
  }
  SUBCASE("mono to stereo copies") {
    const MixMatrix m = mix_matrix(1, 0, 2, 0);
    CHECK(m.at(0, 0) == 1.0f);
    CHECK(m.at(1, 0) == 1.0f);
  }
  SUBCASE("5.1 to stereo follows BS.775 and cannot clip") {
    const MixMatrix m = mix_matrix(6, layout::surround_5_1, 2, 0);
    // FL FR FC LFE BL BR
    const float norm = m.at(0, 0);
    CHECK(norm > 0.0f);
    CHECK_APPROX(m.at(0, 2) / norm, 0.70710678, 1e-5);  // centre at -3 dB
    CHECK_APPROX(m.at(0, 4) / norm, 0.70710678, 1e-5);  // surround at -3 dB
    CHECK(m.at(0, 3) == 0.0f);                           // LFE dropped
    CHECK(m.at(0, 1) == 0.0f);
    CHECK(m.at(0, 5) == 0.0f);
    float sum = 0.0f;
    for (std::size_t i = 0; i < 6; ++i) sum += std::abs(m.at(0, i));
    CHECK(sum <= 1.0f + 1e-6f);
  }
  SUBCASE("7.1 to 5.1 folds the sides into the backs") {
    const MixMatrix m = mix_matrix(8, layout::surround_7_1, 6, layout::surround_5_1);
    CHECK(m.at(4, 6) > 0.0f);  // SL -> BL
    CHECK(m.at(5, 7) > 0.0f);  // SR -> BR
    CHECK(m.at(0, 0) == 1.0f);
  }
  SUBCASE("stereo to 5.1 invents nothing") {
    const MixMatrix m = mix_matrix(2, 0, 6, 0);
    CHECK(m.at(0, 0) == 1.0f);
    CHECK(m.at(1, 1) == 1.0f);
    for (std::size_t o = 2; o < 6; ++o) CHECK(m.at(o, 0) == 0.0f);
  }
  SUBCASE("mono to 5.1 goes to the centre") {
    const MixMatrix m = mix_matrix(1, 0, 6, 0);
    CHECK(m.at(2, 0) == 1.0f);
    CHECK(m.at(0, 0) == 0.0f);
  }
  SUBCASE("identity") { CHECK(mix_matrix(6, 0, 6, 0).is_identity()); }
  SUBCASE("remix applies to audio and keeps the layout name") {
    ir::Audio a;
    a.sample_rate = 8000;
    a.channels = 2;
    a.samples = {0.5f, -0.5f, 0.25f, 0.25f};
    REQUIRE_OK(remix(a, 1));
    CHECK(a.samples == std::vector<float>{0.0f, 0.25f});
    CHECK(layout_name(6, 0) == "5.1");
    CHECK(layout_name(8, 0) == "7.1");
    CHECK(layout_name(2, 0) == "stereo");
    CHECK_STATUS(remix(a, 0), Status::InvalidArgument);
  }
  SUBCASE("interleave and deinterleave are inverse") {
    const std::vector<float> data = {1, 2, 3, 4, 5, 6};
    const auto planes = deinterleave(data, 3);
    CHECK(planes[1] == std::vector<float>{2, 5});
    CHECK(interleave(planes) == data);
  }
}

TEST_CASE("audio_lossless/dsp: TPDF dither statistics") {
  // A constant 0.3 LSB: the dithered output averages to it, error variance ~ 1/4 LSB^2.
  constexpr int bits = 16;
  const double lsb = 1.0 / 32768.0;
  std::vector<float> input(200000, static_cast<float>(0.3 * lsb));
  dsp::Quantizer q(bits, 1, dsp::Dither::Triangular);
  const std::vector<std::int32_t> out = q.process(input);
  double mean = 0.0, var = 0.0;
  for (const std::int32_t v : out) mean += v;
  mean /= static_cast<double>(out.size());
  for (const std::int32_t v : out) var += (v - 0.3) * (v - 0.3);
  var /= static_cast<double>(out.size());
  CHECK_APPROX(mean, 0.3, 0.01);
  CHECK(var > 0.22);
  CHECK(var < 0.30);
  for (const std::int32_t v : out) CHECK(std::abs(v) <= 2);
  // Without dither the error is deterministic rounding.
  dsp::Quantizer plain(bits, 1, dsp::Dither::None);
  for (const std::int32_t v : plain.process(input)) CHECK(v == 0);
  // Deterministic: same seed, same noise.
  dsp::Quantizer again(bits, 1, dsp::Dither::Triangular);
  CHECK(again.process(input) == out);
}

TEST_CASE("audio_lossless/dsp: noise shaping moves noise to high frequencies") {
  const std::size_t n = 1 << 15;
  std::vector<float> input = tone(44100, 1000.0, n, 0.25);
  const auto error_low_fraction = [&](dsp::Dither dither) {
    dsp::Quantizer q(8, 1, dither, 44100);
    const std::vector<std::int32_t> out = q.process(input);
    std::vector<double> e(n);
    for (std::size_t i = 0; i < n; ++i) e[i] = out[i] / 128.0 - input[i];
    // Energy after a 16-tap moving average (low-pass) vs total.
    double low = 0.0, total = 0.0;
    for (std::size_t i = 16; i < n; ++i) {
      double avg = 0.0;
      for (std::size_t k = 0; k < 16; ++k) avg += e[i - k];
      avg /= 16.0;
      low += avg * avg;
      total += e[i] * e[i];
    }
    return low / total;
  };
  const double tpdf = error_low_fraction(dsp::Dither::Triangular);
  const double shaped = error_low_fraction(dsp::Dither::Shaped);
  CHECK(shaped < tpdf * 0.2);
}

TEST_CASE("audio_lossless/dsp: quantiser clips and recognises exact grids") {
  dsp::Quantizer q(16, 1, dsp::Dither::None);
  const std::vector<std::int32_t> out = q.process(std::vector<float>{2.0f, -2.0f, 1.0f, -1.0f});
  CHECK(out == std::vector<std::int32_t>{32767, -32768, 32767, -32768});
  CHECK(dsp::exactly_representable(std::vector<float>{0.5f, -1.0f, 1.0f / 32768.0f}, 16));
  CHECK_FALSE(dsp::exactly_representable(std::vector<float>{1.0f / 65536.0f}, 16));
  CHECK_FALSE(dsp::exactly_representable(std::vector<float>{1.0f}, 16));
  // to_integers does not dither exact input.
  const ir::Audio exact = test::audio::noise(8000, 2, 0.1, 16);
  const std::vector<std::int32_t> ints = dsp::to_integers(exact.samples, 2, 16);
  for (std::size_t i = 0; i < ints.size(); ++i) CHECK(dsp::int_to_float(ints[i], 16) == exact.samples[i]);
}

TEST_CASE("audio_lossless/dsp: loudness and peak measurement") {
  // BS.1770: a 1 kHz sine at 0 dBFS in one channel measures -3.01 LUFS; in both channels 0 LUFS.
  ir::Audio mono = test::sine(48000, 1, 1000.0, 3.0, 1.0);
  CHECK_APPROX(dsp::integrated_loudness(mono), -3.01, 0.1);
  ir::Audio stereo = test::sine(48000, 2, 1000.0, 3.0, 0.1);
  CHECK_APPROX(dsp::integrated_loudness(stereo), -20.0, 0.1);
  ir::Audio quiet = test::sine(44100, 2, 1000.0, 3.0, 0.0001);
  CHECK(dsp::integrated_loudness(quiet) < -70.0);
  // A sine at fs/4 sampled at 45 degrees peaks between samples.
  ir::Audio between;
  between.sample_rate = 48000;
  between.channels = 1;
  for (std::size_t i = 0; i < 4800; ++i) {
    between.samples.push_back(static_cast<float>(0.5 * std::sin(std::numbers::pi / 2.0 * static_cast<double>(i) + std::numbers::pi / 4.0)));
  }
  CHECK_APPROX(dsp::measure_levels(between.samples, 1).peak, 0.3536, 0.001);
  CHECK_APPROX(dsp::true_peak(between), 0.5, 0.02);
  const dsp::Levels levels = dsp::measure_levels(stereo.samples, 2);
  CHECK_APPROX(levels.rms, 0.1 / std::sqrt(2.0), 1e-3);
  CHECK_APPROX(dsp::to_db(dsp::from_db(-6.0)), -6.0, 1e-12);
}

TEST_CASE("audio_lossless/dsp: normalisation, trimming and fades") {
  ir::Audio a = test::sine(8000, 1, 440.0, 1.0, 0.1);
  dsp::normalize_peak(a, -1.0);
  CHECK_APPROX(dsp::measure_levels(a.samples, 1).peak, dsp::from_db(-1.0), 1e-5);
  ir::Audio b = test::sine(48000, 2, 1000.0, 3.0, 0.05);
  dsp::normalize_loudness(b, -16.0, -1.0);
  CHECK_APPROX(dsp::integrated_loudness(b), -16.0, 0.2);
  ir::Audio c;
  c.sample_rate = 8000;
  c.channels = 1;
  c.samples = {0.0f, 0.0f, 0.5f, -0.5f, 0.0f};
  CHECK(dsp::trim_silence(c) == 2);
  CHECK(c.samples == std::vector<float>{0.5f, -0.5f});
  ir::Audio d;
  d.sample_rate = 8000;
  d.channels = 1;
  d.samples.assign(100, 1.0f);
  dsp::fade_in(d, 10, dsp::FadeCurve::Linear);
  dsp::fade_out(d, 10, dsp::FadeCurve::EqualPower);
  CHECK(d.samples[0] == 0.0f);
  CHECK_APPROX(d.samples[5], 0.5, 1e-6);
  CHECK(d.samples[50] == 1.0f);
  CHECK(d.samples[99] == 0.0f);
  dsp::trim(d, 10, 5);
  CHECK(d.frame_count() == 5);
}
