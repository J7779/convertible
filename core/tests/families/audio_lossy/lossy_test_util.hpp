// Shared helpers of the audio_lossy tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

namespace cvt::lossy_test {

/// Decode with the cheapest registered audio reader of `format` (what the
/// planner picks; test::read_as takes the first registered one, which for
/// "m4a" is the lossless family's generic reader).
inline Result<ir::Audio> read_audio(std::string_view format, ByteView input, std::vector<std::string>* warnings = nullptr,
                                    const Limits& limits = {}) {
  const Codec* best = nullptr;
  for (const Codec* codec : Registry::instance().codecs_for(format)) {
    if (codec->kind == Kind::Audio && codec->read && (best == nullptr || codec->read_cost < best->read_cost)) best = codec;
  }
  if (best == nullptr) return fail(Status::UnsupportedConversion, "no audio reader for {}", format);
  ReadContext ctx;
  ctx.limits = limits;
  ctx.registry = &Registry::instance();
  ctx.input = input;
  ctx.format_id = std::string(format);
  auto value = best->read(ctx);
  if (warnings != nullptr) *warnings = ctx.warnings.list();
  if (!value) return std::unexpected(std::move(value).error());
  return std::move(*value).template get<ir::Audio>();
}

/// Comparison of a decode with the 16-bit reference of an independent decoder.
struct Match {
  std::size_t frames = 0;
  std::size_t reference_frames = 0;
  double rms = 1.0;   ///< RMS difference (full scale = 1)
  double peak = 1.0;  ///< largest absolute difference
  double snr = 0.0;   ///< dB
};

/// Compare sample by sample over the common length (no alignment search:
/// both decoders must agree on the start).
inline Match compare(const ir::Audio& ours, const ir::Audio& reference) {
  Match m;
  m.frames = ours.frame_count();
  m.reference_frames = reference.frame_count();
  if (ours.channels != reference.channels || ours.sample_rate != reference.sample_rate) return m;
  const std::size_t n = std::min(m.frames, m.reference_frames) * ours.channels;
  double err = 0.0, sig = 0.0, peak = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double a = std::clamp(static_cast<double>(ours.samples[i]), -1.0, 32767.0 / 32768.0);
    const double b = reference.samples[i];
    const double d = a - b;
    err += d * d;
    sig += b * b;
    peak = std::max(peak, std::abs(d));
  }
  if (n == 0) return m;
  m.rms = std::sqrt(err / static_cast<double>(n));
  m.peak = peak;
  m.snr = err > 0 ? 10.0 * std::log10(sig / err) : 200.0;
  return m;
}

/// Decode `name` (below core/tests/data/audio_lossy/) as `format` and compare
/// with `<name without extension>_ref.flac`.
inline Match against_reference(std::string_view format, const std::string& name) {
  const Bytes data = test::fixture("audio_lossy/" + name);
  const std::string ref_name = "audio_lossy/" + name.substr(0, name.rfind('.')) + "_ref.flac";
  const Bytes ref = test::fixture(ref_name);
  const auto ours = read_audio(format, data);
  const auto theirs = test::read_as<ir::Audio>("flac", ref);
  if (!ours || !theirs) return {};
  return compare(*ours, *theirs);
}

}  // namespace cvt::lossy_test
