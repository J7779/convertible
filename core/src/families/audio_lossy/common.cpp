// Convertible core - audio_lossy family: helpers shared by the codec areas.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossy/common.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>

#include "engine/standard_options.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/audio_dsp.hpp"

namespace cvt::lossy {

bool is_lossy_audio_format(std::string_view format_id) noexcept {
  static constexpr std::array<std::string_view, 17> lossy = {"mp3", "mp2", "mp1", "aac", "m4a", "m4b", "opus",
                                                             "ogg", "oga", "spx", "amr", "awb", "gsm", "ac3",
                                                             "wma", "3ga", "m4r"};
  return std::find(lossy.begin(), lossy.end(), format_id) != lossy.end();
}

void warn_if_reencoding(const WriteContext& ctx) {
  if (is_lossy_audio_format(ctx.source_format_id)) {
    ctx.warn("The source is already lossy-compressed; encoding it again with a lossy codec reduces quality further.");
  }
}

Result<ir::Audio> prepare_lossy(const ir::Audio& source, const WriteContext& ctx,
                                std::span<const std::uint32_t> allowed_rates, std::uint16_t max_channels) {
  const std::uint32_t lo = allowed_rates.empty() ? 0 : allowed_rates.front();
  const std::uint32_t hi = allowed_rates.empty() ? 0 : allowed_rates.back();
  CVT_TRY_ASSIGN(ir::Audio audio, audio::prepare_for_writing(source, ctx, lo, hi, max_channels));
  if (!allowed_rates.empty() &&
      std::find(allowed_rates.begin(), allowed_rates.end(), audio.sample_rate) == allowed_rates.end()) {
    // The nearest allowed rate at or above the current one keeps the bandwidth.
    std::uint32_t target = allowed_rates.back();
    for (const std::uint32_t r : allowed_rates) {
      if (r >= audio.sample_rate) {
        target = r;
        break;
      }
    }
    ctx.warn(std::format("{} Hz is not available in this format; the audio was resampled to {} Hz.",
                         audio.sample_rate, target));
    CVT_TRY(ctx.checkpoint(0.0, "Resampling"));
    CVT_TRY(audio::dsp::resample(audio, target, audio::resolved_resample_quality(ctx.options)));
  }
  audio.bits_per_sample = 0;
  audio.float_samples = false;
  return audio;
}

int lossy_quality(const Options& options) { return std::clamp(options::resolved_quality(options), 1, 100); }

double quality_curve(int quality, std::initializer_list<std::pair<int, double>> points) {
  auto it = points.begin();
  auto prev = *it;
  for (; it != points.end(); ++it) {
    if (quality <= it->first) {
      if (it->first == prev.first) return it->second;
      const double t = static_cast<double>(quality - prev.first) / static_cast<double>(it->first - prev.first);
      return prev.second + (it->second - prev.second) * t;
    }
    prev = *it;
  }
  return prev.second;
}

}  // namespace cvt::lossy
