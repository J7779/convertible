// Helpers for the audio_lossless tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

#include "ir/audio.hpp"

namespace cvt::test::audio {

inline constexpr std::string_view dir = "audio_lossless/";

/// Decode a fixture of this family with the reader of `format`.
inline Result<ir::Audio> load(std::string_view format, std::string_view name, std::string_view options = {},
                              std::vector<std::string>* warnings = nullptr) {
  return read_as<ir::Audio>(format, fixture(std::string(dir) + std::string(name)), options, warnings);
}

/// The reference PCM written by Python's wave module.
inline ir::Audio reference(std::string_view name) {
  auto audio = load("wav", name);
  if (!audio) return {};
  return std::move(*audio);
}

/// Number of samples that differ (all channels), or SIZE_MAX when the
/// layouts differ; lengths are compared over `frames` (0 = both whole).
inline std::size_t differing_samples(const ir::Audio& a, const ir::Audio& b, std::size_t frames = 0) {
  if (a.sample_rate != b.sample_rate || a.channels != b.channels) return static_cast<std::size_t>(-1);
  std::size_t n = std::min(a.samples.size(), b.samples.size());
  if (frames != 0) n = std::min(n, frames * a.channels);
  std::size_t diff = 0;
  for (std::size_t i = 0; i < n; ++i) diff += a.samples[i] != b.samples[i] ? 1 : 0;
  return diff;
}

/// True when `test` equals `ref` bit for bit (same layout and length).
inline bool identical(const ir::Audio& ref, const ir::Audio& test) {
  return ref.sample_rate == test.sample_rate && ref.channels == test.channels && ref.samples == test.samples;
}

/// Largest absolute sample difference.
inline double max_error(const ir::Audio& a, const ir::Audio& b) {
  double worst = 0.0;
  for (std::size_t i = 0; i < std::min(a.samples.size(), b.samples.size()); ++i) {
    worst = std::max(worst, std::abs(static_cast<double>(a.samples[i]) - b.samples[i]));
  }
  return worst;
}

/// Quantise to a `bits` grid so that integer formats round-trip exactly.
inline ir::Audio on_grid(ir::Audio audio, int bits) {
  const double scale = static_cast<double>(std::uint64_t{1} << (bits - 1));
  for (float& s : audio.samples) {
    double v = std::round(static_cast<double>(s) * scale);
    v = std::clamp(v, -scale, scale - 1.0);
    s = static_cast<float>(v / scale);
  }
  audio.bits_per_sample = static_cast<std::uint16_t>(bits);
  return audio;
}

/// White noise on a `bits` grid, `seconds` long.
inline ir::Audio noise(std::uint32_t rate, std::uint16_t channels, double seconds, int bits, std::uint32_t seed = 7,
                       double amplitude = 0.5) {
  return on_grid(white_noise(rate, channels, seconds, seed, amplitude), bits);
}

/// A tone plus noise on a `bits` grid: compressible, but not trivially.
inline ir::Audio music(std::uint32_t rate, std::uint16_t channels, double seconds, int bits) {
  ir::Audio tone = sweep(rate, channels, 100.0, 3000.0, seconds, 0.6);
  const ir::Audio hiss = white_noise(rate, channels, seconds, 3, 0.01);
  for (std::size_t i = 0; i < tone.samples.size(); ++i) tone.samples[i] += hiss.samples[i];
  return on_grid(std::move(tone), bits);
}

/// Encode then decode with the registered codecs of one format.
inline Result<ir::Audio> round_trip(std::string_view format, const ir::Audio& audio, std::string_view options = {},
                                    Bytes* encoded = nullptr) {
  CVT_TRY_ASSIGN(Bytes bytes, write_from(format, audio, options));
  auto decoded = read_as<ir::Audio>(format, bytes);
  if (encoded != nullptr) *encoded = std::move(bytes);
  return decoded;
}

}  // namespace cvt::test::audio
