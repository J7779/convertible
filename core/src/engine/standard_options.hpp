// Convertible core - the standard option vocabulary.
//
// The same key must mean the same thing for every format, so that a UI can
// show one "Quality" slider and one "Compress" action that work everywhere.
// Codecs therefore do not invent their own specs for these concepts: they
// declare the ready-made ones from this header and read them back with the
// matching `resolved_*` helper, which applies the shared rules (for example
// "an explicit quality beats the preset").
//
//   codec.write_options = {options::quality(), options::preset(), options::strip_metadata()};
//   ...
//   const int quality = options::resolved_quality(ctx.options);     // 1..100
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>

#include "engine/options.hpp"

namespace cvt::options {

// --------------------------------------------------------------------------- keys

inline constexpr std::string_view key_quality = "quality";
inline constexpr std::string_view key_preset = "preset";
inline constexpr std::string_view key_compression_level = "compression_level";
inline constexpr std::string_view key_lossless = "lossless";
inline constexpr std::string_view key_max_width = "max_width";
inline constexpr std::string_view key_max_height = "max_height";
inline constexpr std::string_view key_scale = "scale";
inline constexpr std::string_view key_strip_metadata = "strip_metadata";
inline constexpr std::string_view key_sample_rate = "sample_rate";
inline constexpr std::string_view key_channels = "channels";
inline constexpr std::string_view key_bitrate_kbps = "bitrate_kbps";
inline constexpr std::string_view key_fps = "fps";
inline constexpr std::string_view key_width = "width";
inline constexpr std::string_view key_height = "height";
inline constexpr std::string_view key_video_bitrate_kbps = "video_bitrate_kbps";
inline constexpr std::string_view key_audio_bitrate_kbps = "audio_bitrate_kbps";

// --------------------------------------------------------------------------- specs

/// The default of `quality`, and the quality of the "high" preset.
inline constexpr int default_quality = 90;

/// `quality`: int 1..100, default 90. Higher keeps more detail and makes
/// larger files; lower is smaller. Every lossy writer maps this scale onto
/// its own so that equal numbers look/sound comparably good across formats.
[[nodiscard]] OptionSpec quality();
/// `preset`: "high" (default) | "balanced" | "small". A one-click choice that
/// supplies the quality and effort used when those are not set explicitly.
[[nodiscard]] OptionSpec preset();
/// `compression_level`: int 0..`max_level`, default 6 (clamped). Effort of
/// lossless compression: 0 fastest/largest, max slowest/smallest. Advanced.
[[nodiscard]] OptionSpec compression_level(int max_level = 9);
/// `lossless`: bool, for formats that offer both modes.
[[nodiscard]] OptionSpec lossless(bool default_value = false);
/// `max_width` / `max_height`: int pixels, 0 = no limit. Pictures larger than
/// the box are scaled down to fit, keeping the aspect ratio; never enlarged.
[[nodiscard]] OptionSpec max_width();
[[nodiscard]] OptionSpec max_height();
/// `scale`: float 0.01..16, default 1. Resize factor applied before the
/// max_width/max_height box.
[[nodiscard]] OptionSpec scale();
/// `strip_metadata`: bool, default TRUE (privacy first): drop EXIF, GPS,
/// author, software and similar tags unless the user asks to keep them.
[[nodiscard]] OptionSpec strip_metadata();
/// `sample_rate`: int Hz, 0 = keep the source rate.
[[nodiscard]] OptionSpec sample_rate();
/// `channels`: int 0..8, 0 = keep the source layout (1 mono, 2 stereo).
[[nodiscard]] OptionSpec channels();
/// `bitrate_kbps`: int, 0 = choose from `quality`. Audio-only formats.
[[nodiscard]] OptionSpec bitrate_kbps();
/// `fps`: float frames per second, 0 = keep the source rate.
[[nodiscard]] OptionSpec fps();
/// `width` / `height`: int pixels for video output, 0 = keep (if only one is
/// set the other follows the aspect ratio).
[[nodiscard]] OptionSpec width();
[[nodiscard]] OptionSpec height();
/// `video_bitrate_kbps` / `audio_bitrate_kbps`: int, 0 = choose from `quality`.
[[nodiscard]] OptionSpec video_bitrate_kbps();
[[nodiscard]] OptionSpec audio_bitrate_kbps();

// --------------------------------------------------------------------------- resolved values

enum class Preset { High, Balanced, Small };

/// The quality each preset stands for: high 90, balanced 75, small 50.
[[nodiscard]] constexpr int preset_quality(Preset preset) noexcept {
  switch (preset) {
    case Preset::High: return 90;
    case Preset::Balanced: return 75;
    case Preset::Small: return 50;
  }
  return default_quality;
}

/// The chosen preset; High when absent or unrecognised.
[[nodiscard]] Preset resolved_preset(const Options& options);

/// Effective quality 1..100: the `quality` value when the caller set it
/// explicitly, otherwise the quality of the preset.
[[nodiscard]] int resolved_quality(const Options& options);

/// Effective lossless effort 0..`max_level`: `compression_level` when set
/// explicitly, otherwise by preset (high and balanced: 6, small: maximum),
/// scaled to `max_level` for codecs whose scale is not 0..9.
[[nodiscard]] int resolved_compression_level(const Options& options, int max_level = 9);

[[nodiscard]] bool resolved_lossless(const Options& options, bool fallback = false);
/// True unless the caller turned `strip_metadata` off.
[[nodiscard]] bool resolved_strip_metadata(const Options& options);

struct Size {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  friend bool operator==(const Size&, const Size&) = default;
};

/// Output size for a still image of `source` pixels: apply `scale`, then
/// shrink to fit `max_width` x `max_height`, keeping the aspect ratio.
/// Never returns a zero dimension for a non-empty source.
[[nodiscard]] Size resolved_image_size(const Options& options, Size source);

/// Output size for video: explicit `width`/`height` (one alone keeps the
/// aspect ratio), else the image rules above; both dimensions are then
/// rounded to even numbers, as most video codecs require.
[[nodiscard]] Size resolved_video_size(const Options& options, Size source);

/// Output sample rate / channel count: the option when non-zero, else the source's.
[[nodiscard]] std::uint32_t resolved_sample_rate(const Options& options, std::uint32_t source_rate);
[[nodiscard]] std::uint16_t resolved_channels(const Options& options, std::uint16_t source_channels);

/// Explicit bitrates, or nullopt when the codec should derive one from the quality.
[[nodiscard]] std::optional<std::uint32_t> resolved_bitrate_kbps(const Options& options);
[[nodiscard]] std::optional<std::uint32_t> resolved_video_bitrate_kbps(const Options& options);
[[nodiscard]] std::optional<std::uint32_t> resolved_audio_bitrate_kbps(const Options& options);

/// Output frame rate: the `fps` option when positive, else `source_fps`.
[[nodiscard]] double resolved_fps(const Options& options, double source_fps);

}  // namespace cvt::options
