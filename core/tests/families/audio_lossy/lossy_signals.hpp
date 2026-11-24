// Generated test signals and round-trip helpers of the audio_lossy tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string_view>

#include "families/audio_lossy/lossy_test_util.hpp"

namespace cvt::lossy_test {

/// A music-like test signal: decaying harmonic notes on the left, a chord
/// plus a sweep on the right, a noise burst in the middle.
inline ir::Audio music(std::uint32_t rate, std::uint16_t channels, double seconds) {
  ir::Audio a = test::sine(rate, channels, 440.0, seconds, 0.0);
  const ir::Audio sweep = test::sweep(rate, 1, 200.0, std::min(12000.0, rate * 0.45), seconds, 0.12);
  const ir::Audio noise = test::white_noise(rate, 1, seconds, 7, 0.25);
  for (std::size_t i = 0; i < a.frame_count(); ++i) {
    const double t = static_cast<double>(i) / rate;
    const double note_t = std::fmod(t, 0.15);
    const double f = 220.0 * std::pow(2.0, std::floor(t / 0.15) * 4 / 12.0);
    double left = 0.0;
    for (int k = 1; k <= 5; ++k) {
      if (f * k < rate / 2.0) left += std::sin(2 * std::numbers::pi * f * k * t) / k;
    }
    left *= 0.2 * std::exp(-note_t * 6.0);
    double right = sweep.samples[i] + 0.15 * std::sin(2 * std::numbers::pi * f * 1.5 * t);
    if (t > seconds * 0.55 && t < seconds * 0.55 + 0.03) {
      left += noise.samples[i];
      right += noise.samples[i];
    }
    if (channels == 1) {
      a.samples[i] = static_cast<float>((left + right) * 0.5);
    } else {
      a.samples[i * 2] = static_cast<float>(left);
      a.samples[i * 2 + 1] = static_cast<float>(right);
    }
  }
  return a;
}

struct Trip {
  std::size_t bytes = 0;
  std::size_t frames = 0;
  std::uint32_t rate = 0;
  double snr = -100;
};

inline Trip round_trip(std::string_view format, const ir::Audio& source, std::string_view options,
                       std::size_t max_shift = 2000) {
  Trip t;
  auto encoded = test::write_from(format, source, options);
  if (!encoded) {
    std::printf("encode failed: %s\n", encoded.error().message.c_str());
    return t;
  }
  t.bytes = encoded->size();
  auto decoded = read_audio(format, *encoded);
  if (!decoded) return t;
  t.frames = decoded->frame_count();
  t.rate = decoded->sample_rate;
  if (decoded->sample_rate == source.sample_rate && decoded->channels == source.channels) {
    t.snr = test::aligned_snr_db(source, *decoded, max_shift);
  }
  return t;
}

}  // namespace cvt::lossy_test
