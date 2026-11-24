// Convertible core - audio_lossy family: helpers shared by the codec areas.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/audio.hpp"

namespace cvt::lossy {

/// True for the formats whose audio has already been through a lossy codec.
[[nodiscard]] bool is_lossy_audio_format(std::string_view format_id) noexcept;

/// Warn when a lossy source is encoded again with a lossy codec.
void warn_if_reencoding(const WriteContext& ctx);

/// The standard pipeline (channels, rate, normalisation, tag stripping) of
/// audio_api.hpp, then a final resampling to the nearest rate in
/// `allowed_rates` (sorted ascending) when the result is not one of them.
[[nodiscard]] Result<ir::Audio> prepare_lossy(const ir::Audio& source, const WriteContext& ctx,
                                              std::span<const std::uint32_t> allowed_rates,
                                              std::uint16_t max_channels);

/// Quality 1..100 from `quality`/`preset`.
[[nodiscard]] int lossy_quality(const Options& options);

/// Linear interpolation over (quality, value) points sorted by quality.
[[nodiscard]] double quality_curve(int quality, std::initializer_list<std::pair<int, double>> points);

}  // namespace cvt::lossy
