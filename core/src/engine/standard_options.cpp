// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/standard_options.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace cvt::options {

namespace {

std::optional<std::uint32_t> positive(const Options& options, std::string_view key) {
  const std::int64_t value = options.get_int(key, 0, 0, 4'000'000);
  if (value <= 0) return std::nullopt;
  return static_cast<std::uint32_t>(value);
}

std::uint32_t at_least_one(double value) {
  return static_cast<std::uint32_t>(std::clamp(std::round(value), 1.0, 4294967295.0));
}

}  // namespace

OptionSpec quality() {
  return OptionSpec::integer(std::string(key_quality), "Quality", default_quality, 1, 100,
                             "Higher keeps more detail and makes larger files; lower makes smaller files.");
}

OptionSpec preset() {
  return OptionSpec::choice(std::string(key_preset), "Preset", "high", {"high", "balanced", "small"},
                            "High keeps the best quality, small favours file size, balanced sits in between.");
}

OptionSpec compression_level(int max_level) {
  max_level = std::max(max_level, 1);
  return OptionSpec::integer(std::string(key_compression_level), "Compression level", std::min(6, max_level), 0,
                             max_level, "Lossless compression effort: higher is slower and smaller.")
      .advanced();
}

OptionSpec lossless(bool default_value) {
  return OptionSpec::boolean(std::string(key_lossless), "Lossless", default_value,
                             "Keep the content bit-exact instead of using lossy compression.");
}

OptionSpec max_width() {
  return OptionSpec::integer(std::string(key_max_width), "Maximum width", 0, 0, 65535,
                             "Scale down to at most this many pixels wide; 0 means no limit.");
}

OptionSpec max_height() {
  return OptionSpec::integer(std::string(key_max_height), "Maximum height", 0, 0, 65535,
                             "Scale down to at most this many pixels high; 0 means no limit.");
}

OptionSpec scale() {
  return OptionSpec::number(std::string(key_scale), "Scale", 1.0, 0.01, 16.0, "Resize factor; 0.5 halves both dimensions.")
      .advanced();
}

OptionSpec strip_metadata() {
  return OptionSpec::boolean(std::string(key_strip_metadata), "Remove metadata", true,
                             "Leave out embedded details such as location, device, author and software.");
}

OptionSpec sample_rate() {
  return OptionSpec::integer(std::string(key_sample_rate), "Sample rate", 0, 0, 384000,
                             "Samples per second, for example 44100 or 48000; 0 keeps the source rate.")
      .advanced();
}

OptionSpec channels() {
  return OptionSpec::integer(std::string(key_channels), "Channels", 0, 0, 8,
                             "1 for mono, 2 for stereo; 0 keeps the source layout.")
      .advanced();
}

OptionSpec bitrate_kbps() {
  return OptionSpec::integer(std::string(key_bitrate_kbps), "Bitrate (kbit/s)", 0, 0, 2000,
                             "Target bitrate; 0 chooses one from the quality setting.")
      .advanced();
}

OptionSpec fps() {
  return OptionSpec::number(std::string(key_fps), "Frame rate", 0.0, 0.0, 240.0,
                            "Frames per second; 0 keeps the source frame rate.")
      .advanced();
}

OptionSpec width() {
  return OptionSpec::integer(std::string(key_width), "Width", 0, 0, 16384,
                             "Output width in pixels; 0 keeps the source size or follows the height.");
}

OptionSpec height() {
  return OptionSpec::integer(std::string(key_height), "Height", 0, 0, 16384,
                             "Output height in pixels; 0 keeps the source size or follows the width.");
}

OptionSpec video_bitrate_kbps() {
  return OptionSpec::integer(std::string(key_video_bitrate_kbps), "Video bitrate (kbit/s)", 0, 0, 200000,
                             "Target video bitrate; 0 chooses one from the quality setting.")
      .advanced();
}

OptionSpec audio_bitrate_kbps() {
  return OptionSpec::integer(std::string(key_audio_bitrate_kbps), "Audio bitrate (kbit/s)", 0, 0, 2000,
                             "Target audio bitrate; 0 chooses one from the quality setting.")
      .advanced();
}

Preset resolved_preset(const Options& options) {
  const std::string value = options.get_enum(key_preset, "high", {"high", "balanced", "small"});
  if (value == "balanced") return Preset::Balanced;
  if (value == "small") return Preset::Small;
  return Preset::High;
}

int resolved_quality(const Options& options) {
  if (options.was_set(key_quality)) return static_cast<int>(options.get_int(key_quality, default_quality, 1, 100));
  return preset_quality(resolved_preset(options));
}

int resolved_compression_level(const Options& options, int max_level) {
  max_level = std::max(max_level, 0);
  if (options.was_set(key_compression_level)) {
    return static_cast<int>(options.get_int(key_compression_level, 6, 0, max_level));
  }
  if (resolved_preset(options) == Preset::Small) return max_level;
  // Level 6 of 9, carried over to other scales (e.g. 15 of 22).
  return static_cast<int>(std::lround(max_level * 6.0 / 9.0));
}

bool resolved_lossless(const Options& options, bool fallback) { return options.get_bool(key_lossless, fallback); }

bool resolved_strip_metadata(const Options& options) { return options.get_bool(key_strip_metadata, true); }

Size resolved_image_size(const Options& options, Size source) {
  if (source.width == 0 || source.height == 0) return source;
  double factor = options.get_float(key_scale, 1.0, 0.01, 16.0);
  const double scaled_width = source.width * factor;
  const double scaled_height = source.height * factor;
  const auto limit_width = static_cast<double>(options.get_int(key_max_width, 0, 0, 1'000'000));
  const auto limit_height = static_cast<double>(options.get_int(key_max_height, 0, 0, 1'000'000));
  double shrink = 1.0;
  if (limit_width > 0 && scaled_width > limit_width) shrink = std::min(shrink, limit_width / scaled_width);
  if (limit_height > 0 && scaled_height > limit_height) shrink = std::min(shrink, limit_height / scaled_height);
  factor *= shrink;
  if (factor == 1.0) return source;
  return {at_least_one(source.width * factor), at_least_one(source.height * factor)};
}

Size resolved_video_size(const Options& options, Size source) {
  if (source.width == 0 || source.height == 0) return source;
  const auto wanted_width = static_cast<std::uint32_t>(options.get_int(key_width, 0, 0, 65535));
  const auto wanted_height = static_cast<std::uint32_t>(options.get_int(key_height, 0, 0, 65535));
  Size size = source;
  if (wanted_width > 0 && wanted_height > 0) {
    size = {wanted_width, wanted_height};
  } else if (wanted_width > 0) {
    size = {wanted_width, at_least_one(static_cast<double>(source.height) * wanted_width / source.width)};
  } else if (wanted_height > 0) {
    size = {at_least_one(static_cast<double>(source.width) * wanted_height / source.height), wanted_height};
  } else {
    size = resolved_image_size(options, source);
  }
  const auto even = [](std::uint32_t value) { return std::max<std::uint32_t>(2, (value + 1) & ~1u); };
  return {even(size.width), even(size.height)};
}

std::uint32_t resolved_sample_rate(const Options& options, std::uint32_t source_rate) {
  return positive(options, key_sample_rate).value_or(source_rate);
}

std::uint16_t resolved_channels(const Options& options, std::uint16_t source_channels) {
  const std::int64_t value = options.get_int(key_channels, 0, 0, 255);
  return value > 0 ? static_cast<std::uint16_t>(value) : source_channels;
}

std::optional<std::uint32_t> resolved_bitrate_kbps(const Options& options) { return positive(options, key_bitrate_kbps); }

std::optional<std::uint32_t> resolved_video_bitrate_kbps(const Options& options) {
  return positive(options, key_video_bitrate_kbps);
}

std::optional<std::uint32_t> resolved_audio_bitrate_kbps(const Options& options) {
  return positive(options, key_audio_bitrate_kbps);
}

double resolved_fps(const Options& options, double source_fps) {
  const double value = options.get_float(key_fps, 0.0, 0.0, 1000.0);
  return value > 0.0 ? value : source_fps;
}

}  // namespace cvt::options
