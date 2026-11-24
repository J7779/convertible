// Convertible core - codec_av1 family: internal declarations.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <memory>

#include "codec/codec.hpp"
#include "families/codec_av1/libaom_glue.hpp"

namespace cvt::av1 {

/// Largest picture the stream decoder accepts: AV1 level 6.3 MaxPicSize
/// (8192x4352) and MaxHSize/MaxVSize 16384.
inline constexpr std::uint64_t max_video_pixels = 35651584;
inline constexpr std::uint32_t max_video_side = 16384;

[[nodiscard]] std::unique_ptr<codec::VideoDecoder> make_decoder();
[[nodiscard]] std::unique_ptr<codec::VideoEncoder> make_encoder();

/// libaom usage mode, speed and look-ahead chosen for a preset and size.
struct SpeedChoice {
  detail::Usage usage = detail::Usage::GoodQuality;
  int cpu_used = 6;
  int lag_in_frames = 0;
};
[[nodiscard]] SpeedChoice choose_video_speed(codec::EncodePreset preset, std::uint64_t pixels);
[[nodiscard]] SpeedChoice choose_still_speed(codec::EncodePreset preset, std::uint64_t pixels);

/// Timestamp conversion with rounding to nearest; `timescale` > 0.
[[nodiscard]] std::int64_t ticks_to_ms(std::int64_t ticks, std::uint32_t timescale);
[[nodiscard]] std::int64_t ms_to_ticks(std::int64_t ms, std::uint32_t timescale);

/// The CICP code points matching a ColorInfo, for signalling.
[[nodiscard]] PictureColor cicp_from_color_info(const ir::ColorInfo& info, bool ten_bit);

}  // namespace cvt::av1
