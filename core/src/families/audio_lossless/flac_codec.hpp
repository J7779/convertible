// Convertible core - audio_lossless family: FLAC frame codec (shared by the
// .flac/Ogg FLAC containers and the media codec registry).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"
#include "util/hash.hpp"

namespace cvt::audio::flac {

struct StreamInfo {
  std::uint32_t min_block = 0;
  std::uint32_t max_block = 0;
  std::uint32_t min_frame = 0;
  std::uint32_t max_frame = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::uint32_t bits = 0;
  std::uint64_t total_samples = 0;  ///< per channel; 0 = unknown
  std::array<std::uint8_t, 16> md5{};
};

/// Parse the 34-byte STREAMINFO body.
[[nodiscard]] Result<StreamInfo> parse_streaminfo(ByteView body);
[[nodiscard]] Bytes serialize_streaminfo(const StreamInfo& info);

/// What a decoded frame said about itself.
struct FrameInfo {
  std::uint32_t block_size = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::uint32_t bits = 0;
  std::uint64_t first_sample = 0;  ///< from the frame/sample number
};

/// True when a frame header plausibly starts at `data` (sync code and a
/// valid header CRC); `info` may give defaults for "see STREAMINFO" codes.
[[nodiscard]] bool frame_header_at(ByteView data, const StreamInfo* info);

/// Decode one frame at the start of `data`. Appends interleaved samples to
/// `out` and returns the number of bytes the frame occupies. `info`
/// (optional) supplies values the header defers to STREAMINFO.
[[nodiscard]] Result<std::size_t> decode_frame(ByteView data, const StreamInfo* info, std::vector<std::int32_t>& out,
                                               FrameInfo& frame);

/// Encoder settings for one compression level 0..8 (the reference
/// encoder's presets: block size, stereo decorrelation, LPC order,
/// apodization windows, partition orders).
struct Settings {
  std::uint32_t block_size = 4096;
  bool mid_side = true;
  std::uint32_t max_lpc_order = 8;
  std::uint32_t min_partition_order = 0;
  std::uint32_t max_partition_order = 5;
  int windows = 1;              ///< 1 = tukey(0.5); 2/3 = plus partial/punch-out tukeys
  bool exhaustive = false;      ///< try every LPC order exactly
  std::uint32_t qlp_precision = 0;  ///< 0 = automatic
};
[[nodiscard]] Settings settings_for_level(int level);

class FrameEncoder {
 public:
  [[nodiscard]] Result<void> configure(std::uint32_t sample_rate, std::uint32_t channels, std::uint32_t bits,
                                       const Settings& settings);
  /// Encode one block of interleaved samples (`frames` <= block size).
  [[nodiscard]] Bytes encode(std::span<const std::int32_t> interleaved, std::uint32_t frames, std::uint64_t frame_number);
  [[nodiscard]] const Settings& settings() const noexcept { return settings_; }

 private:
  std::uint32_t sample_rate_ = 0;
  std::uint32_t channels_ = 0;
  std::uint32_t bits_ = 0;
  Settings settings_;
};

/// MD5 of samples as FLAC defines it (little-endian, ceil(bits/8) bytes each).
class SampleMd5 {
 public:
  explicit SampleMd5(std::uint32_t bits);
  void update(std::span<const std::int32_t> samples);
  [[nodiscard]] std::array<std::uint8_t, 16> finish();

 private:
  std::uint32_t bytes_;
  std::vector<std::uint8_t> buffer_;
  util::Md5 md5_;
};

}  // namespace cvt::audio::flac
