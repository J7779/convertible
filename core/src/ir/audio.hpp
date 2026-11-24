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

  // -- source description (optional; zero/false = unknown) --------------------
  // Readers fill these so that writers can keep the source's precision and
  // speaker layout by default (a 24-bit WAV stays 24-bit when compressed to
  // FLAC). Writers treat them as hints only: `samples` is authoritative.

  /// Precision of the source samples in bits: the integer bit depth (8, 16,
  /// 24, 32, also 4/12/20 for some codecs), or 32/64 for floating point.
  /// 0 = unknown (e.g. decoded from a lossy codec).
  std::uint16_t bits_per_sample = 0;
  /// True when the source samples were floating point.
  bool float_samples = false;
  /// Speaker positions as a WAVE_FORMAT_EXTENSIBLE channel mask (bit 0 =
  /// front left, 1 = front right, 2 = centre, 3 = LFE, 4 = back left, 5 =
  /// back right, 6/7 = front left/right of centre, 8 = back centre, 9/10 =
  /// side left/right, ...). 0 = the default layout for `channels`.
  std::uint32_t channel_mask = 0;

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
