// Convertible core - Audio IR: PCM samples.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cvt::ir {

/// Uncompressed audio.
struct Audio {
  /// Sample frames per second (e.g. 44100). Must be > 0 for valid audio.
  std::uint32_t sample_rate = 0;
  /// Channels per frame: 1 = mono, 2 = stereo (L, R), more follow the
  /// WAVE/SMPTE order (L, R, C, LFE, Ls, Rs, ...). Must be > 0.
  std::uint16_t channels = 0;
  /// Interleaved 32-bit float samples, nominal range -1.0 .. +1.0. The size
  /// is a multiple of `channels`. Writers clip out-of-range values.
  std::vector<float> samples;
  /// Tags ("title", "artist", "album", "date", "track", "genre", "comment",
  /// ...). Keys are lowercase; values are UTF-8.
  std::map<std::string, std::string> tags;

  /// Number of sample frames (samples per channel).
  [[nodiscard]] std::size_t frame_count() const noexcept { return channels == 0 ? 0 : samples.size() / channels; }
  [[nodiscard]] double duration_seconds() const noexcept {
    return sample_rate == 0 ? 0.0 : static_cast<double>(frame_count()) / sample_rate;
  }
  [[nodiscard]] bool valid() const noexcept {
    return sample_rate > 0 && channels > 0 && samples.size() % channels == 0;
  }
};

}  // namespace cvt::ir
