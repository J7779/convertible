// Convertible core - audio_lossless family: WavPack (version 4/5 blocks).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::audio::wavpack {

/// The fixed 32-byte block header.
struct BlockHeader {
  std::uint32_t size = 0;  ///< ckSize: bytes after the first 8
  std::uint16_t version = 0;
  std::uint64_t total_samples = 0;  ///< 0xFFFFFFFFFF-ish values = unknown
  bool total_known = true;
  std::uint64_t block_index = 0;
  std::uint32_t block_samples = 0;
  std::uint32_t flags = 0;
  std::uint32_t crc = 0;
};

[[nodiscard]] Result<BlockHeader> parse_header(ByteView data);

/// Decodes a sequence of blocks into interleaved float samples. Keeps the
/// stream format (rate, channels, bits, mask) learnt from the blocks.
class StreamDecoder {
 public:
  /// Decode all complete blocks in `data` (one or more frames, each frame
  /// being the blocks from an INITIAL_BLOCK to a FINAL_BLOCK). Appends to `out`.
  [[nodiscard]] Result<void> decode_blocks(ByteView data, std::vector<float>& out);
  /// Decode one frame starting at `data`; returns bytes consumed.
  [[nodiscard]] Result<std::size_t> decode_frame(ByteView data, std::vector<float>& out);

  [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] std::uint16_t channels() const noexcept { return channels_; }
  [[nodiscard]] std::uint16_t bits() const noexcept { return bits_; }
  [[nodiscard]] bool float_samples() const noexcept { return float_; }
  [[nodiscard]] bool lossy() const noexcept { return lossy_; }
  [[nodiscard]] std::uint32_t channel_mask() const noexcept { return mask_; }
  /// Number of blocks whose checksum did not match.
  [[nodiscard]] std::size_t crc_errors() const noexcept { return crc_errors_; }
  /// MD5 of the original samples, when the encoder stored one.
  [[nodiscard]] const std::vector<std::uint8_t>& md5() const noexcept { return md5_; }

 private:
  std::uint32_t sample_rate_ = 0;
  std::uint16_t channels_ = 0;
  std::uint16_t bits_ = 0;
  std::uint32_t mask_ = 0;
  bool float_ = false;
  bool lossy_ = false;
  std::size_t crc_errors_ = 0;
  std::vector<std::uint8_t> md5_;
};

/// Lossless encoder of 8/16/24-bit integer audio into WavPack blocks.
class BlockEncoder {
 public:
  /// `effort` 0..9: 0-2 fast (one prediction term), 3-6 normal, 7-9 high.
  [[nodiscard]] Result<void> configure(std::uint32_t sample_rate, std::uint16_t channels, int bits, int effort,
                                       std::uint32_t channel_mask = 0);
  /// Frames per block the encoder prefers.
  [[nodiscard]] std::uint32_t block_frames() const noexcept { return block_frames_; }
  /// Set the total length written into every header (0 = unknown).
  void set_total_samples(std::uint64_t total) noexcept { total_ = total; }
  /// Encode one frame (one block per mono channel / stereo pair).
  [[nodiscard]] Result<Bytes> encode_block(std::span<const std::int32_t> interleaved, std::uint32_t frames);

 private:
  std::uint32_t sample_rate_ = 0;
  std::uint16_t channels_ = 0;
  int bits_ = 16;
  int effort_ = 5;
  std::uint32_t mask_ = 0;
  std::uint32_t block_frames_ = 22050;
  std::uint64_t total_ = 0;
  std::uint64_t position_ = 0;
  /// Adaptive state carried from block to block (as stored in the stream).
  struct GroupState {
    bool valid = false;
    std::vector<std::int32_t> weight_a, weight_b;
    std::vector<std::array<std::int32_t, 8>> samples_a, samples_b;  // oldest first
    std::array<std::uint32_t, 6> medians{};
  };
  std::vector<GroupState> groups_;
};

}  // namespace cvt::audio::wavpack
