// Convertible core - audio_lossless family: Apple Lossless (ALAC) codec.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::audio::alac {

/// ALACSpecificConfig (the 24-byte "magic cookie").
struct Config {
  std::uint32_t frame_length = 4096;
  std::uint8_t compatible_version = 0;
  std::uint8_t bit_depth = 16;
  std::uint8_t pb = 40;
  std::uint8_t mb = 10;
  std::uint8_t kb = 14;
  std::uint8_t channels = 2;
  std::uint16_t max_run = 255;
  std::uint32_t max_frame_bytes = 0;
  std::uint32_t avg_bit_rate = 0;
  std::uint32_t sample_rate = 44100;
};

/// Parse a cookie: the bare 24 bytes, or wrapped in 'frma'/'alac' atoms
/// (as found in CAF 'kuki' chunks and MP4 'alac' boxes).
[[nodiscard]] Result<Config> parse_config(ByteView cookie);
/// The bare 24-byte cookie.
[[nodiscard]] Bytes serialize_config(const Config& config);

/// ALAC element order -> WAVE order, for the standard layouts of 1..8 channels.
[[nodiscard]] std::array<std::uint8_t, 8> channel_order(std::uint8_t channels) noexcept;
/// WAVE channel mask of the ALAC default layout for a channel count.
[[nodiscard]] std::uint32_t channel_mask(std::uint8_t channels) noexcept;

class Decoder {
 public:
  [[nodiscard]] Result<void> configure(const Config& config);
  /// Decode one packet; appends interleaved samples (WAVE channel order) of
  /// `config.bit_depth` bits to `out`. Returns the number of frames.
  [[nodiscard]] Result<std::uint32_t> decode(ByteView packet, std::vector<std::int32_t>& out);
  [[nodiscard]] const Config& config() const noexcept { return config_; }

 private:
  Config config_;
  std::vector<std::int32_t> predictor_, mix_u_, mix_v_, shift_;
};

class Encoder {
 public:
  /// `effort` 0..9 widens the per-frame search (stereo mix, predictor order,
  /// starting coefficients).
  [[nodiscard]] Result<void> configure(const Config& config, int effort);
  /// Encode up to `frame_length` frames of interleaved samples (WAVE order,
  /// `bit_depth` bits). Fewer frames make a partial (last) packet.
  [[nodiscard]] Bytes encode(std::span<const std::int32_t> interleaved, std::uint32_t frames);
  [[nodiscard]] const Config& config() const noexcept { return config_; }

 private:
  Config config_;
  int effort_ = 6;
  std::array<std::array<std::int16_t, 32>, 16> coefs_{};  // carried adaptive state per channel
};

}  // namespace cvt::audio::alac
