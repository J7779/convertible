// Convertible core - audio_lossy family: the in-house MPEG audio Layer III
// encoder (streaming core shared by the .mp3 writer and the codec registry).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/errors.hpp"
#include "families/audio_lossy/mpeg/mpeg_files.hpp"
#include "util/bytes.hpp"

namespace cvt::lossy::mpeg {

class Layer3Encoder {
 public:
  Layer3Encoder();
  ~Layer3Encoder();
  Layer3Encoder(const Layer3Encoder&) = delete;
  Layer3Encoder& operator=(const Layer3Encoder&) = delete;

  /// `channels` 1 or 2; `sample_rate` one of the nine MPEG rates.
  [[nodiscard]] Result<void> configure(std::uint32_t sample_rate, int channels, const Mp3Settings& settings);
  /// Interleaved samples, any count.
  void push(const float* samples, std::size_t frames);
  /// Encode what can be encoded; with `final`, pad and drain everything.
  [[nodiscard]] Result<void> encode(bool final);
  /// Finished frames (complete, in order), removed from the encoder.
  [[nodiscard]] std::vector<Bytes> take_frames();

  /// Samples (per channel) a decoder must drop at the start, excluding the
  /// 529-sample decoder delay that LAME-style tags assume.
  [[nodiscard]] std::uint32_t encoder_delay() const noexcept;
  /// Samples per channel pushed so far.
  [[nodiscard]] std::uint64_t input_frames() const noexcept;
  [[nodiscard]] std::uint32_t samples_per_frame() const noexcept;
  /// An Info/Xing frame (LAME extension with delay and padding) describing
  /// the finished stream of `frames` frames and `bytes` bytes (tag frame
  /// excluded), and the frame header of the stream.
  [[nodiscard]] Bytes info_frame(std::uint32_t frames, std::uint64_t bytes, bool vbr) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cvt::lossy::mpeg
