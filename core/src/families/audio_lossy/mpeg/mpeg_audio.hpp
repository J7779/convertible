// Convertible core - audio_lossy family: MPEG-1/2/2.5 audio (layers I, II,
// III) frame headers, stream scanning and the in-house decoder.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::lossy::mpeg {

/// A decoded 4-byte frame header.
struct FrameHeader {
  int version = 0;        ///< 0 = MPEG-1, 1 = MPEG-2, 2 = MPEG-2.5
  int layer = 3;          ///< 1, 2 or 3
  bool crc = false;       ///< a 16-bit CRC follows the header
  int bitrate_index = 0;  ///< 0 = free format
  int rate_index = 0;
  bool padding = false;
  int mode = 0;           ///< 0 stereo, 1 joint stereo, 2 dual channel, 3 mono
  int mode_ext = 0;
  int emphasis = 0;
  bool copyright = false;
  bool original = false;
  std::uint32_t sample_rate = 0;
  std::uint32_t bitrate_kbps = 0;  ///< 0 for free format

  [[nodiscard]] bool lsf() const noexcept { return version != 0; }
  [[nodiscard]] int channels() const noexcept { return mode == 3 ? 1 : 2; }
  /// Samples per channel in one frame.
  [[nodiscard]] std::uint32_t samples() const noexcept { return layer == 1 ? 384 : (layer == 3 && lsf()) ? 576 : 1152; }
  /// Row of the scale factor band tables (0..8).
  [[nodiscard]] int rate_slot() const noexcept { return version * 3 + rate_index; }
  /// Size of the Layer III side information in bytes.
  [[nodiscard]] std::size_t side_info_bytes() const noexcept {
    if (!lsf()) return channels() == 1 ? 17 : 32;
    return channels() == 1 ? 9 : 17;
  }
  /// Frame length in bytes for a given bit rate (header included).
  [[nodiscard]] std::size_t frame_bytes(std::uint32_t kbps) const noexcept;
  /// The fields that stay constant across the frames of one stream.
  [[nodiscard]] std::uint32_t stream_key() const noexcept;
};

/// Parse a big-endian header word; nullopt when it is not a valid header.
[[nodiscard]] std::optional<FrameHeader> parse_header(std::uint32_t word) noexcept;
[[nodiscard]] std::uint32_t header_word(ByteView data, std::size_t at) noexcept;

/// Gapless information from a Xing/Info (with LAME extension) or VBRI frame.
struct StreamInfo {
  bool has_tag = false;          ///< the first frame was an information frame (not audio)
  bool has_lame = false;
  std::uint32_t frames = 0;      ///< from the tag; 0 = unknown
  std::uint32_t encoder_delay = 0;
  std::uint32_t encoder_padding = 0;
};

/// Parse the information tag inside a frame (bytes from the header on).
[[nodiscard]] std::optional<StreamInfo> parse_info_frame(ByteView frame, const FrameHeader& header);

/// One frame found in a stream.
struct FrameRef {
  std::size_t offset = 0;
  std::size_t size = 0;
};

/// All consecutive frames of the first stream in `data` (leading ID3v2 and
/// junk skipped, trailing tags excluded). Free-format streams are measured
/// from their first two frames.
struct ScanResult {
  FrameHeader first;
  std::vector<FrameRef> frames;
  std::size_t audio_start = 0;
  std::size_t audio_end = 0;
  bool resynced = false;  ///< damage was skipped somewhere
};
[[nodiscard]] Result<ScanResult> scan_stream(ByteView data, std::size_t start, std::size_t end, const Context& ctx,
                                             int wanted_layer = 0);

/// Quick content test for detection: the header of the first frame that
/// is confirmed by two following frames within the first `window` bytes
/// after `start` (nullopt when there is none).
[[nodiscard]] std::optional<FrameHeader> probe_stream(ByteView data, std::size_t start, std::size_t window = 16384);

/// Decoder for every layer. Feed whole frames (header included) in order.
class FrameDecoder {
 public:
  FrameDecoder();
  ~FrameDecoder();
  FrameDecoder(const FrameDecoder&) = delete;
  FrameDecoder& operator=(const FrameDecoder&) = delete;

  /// Decode one frame and append `samples() * channels` interleaved floats.
  /// A frame whose bit reservoir is unavailable (stream start, damage)
  /// decodes to silence of the right length; malformed data is an error.
  [[nodiscard]] Result<void> decode(ByteView frame, std::vector<float>& out);
  /// Forget the bit reservoir and filter state (after a discontinuity).
  void reset();
  [[nodiscard]] int channels() const noexcept { return channels_; }
  [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }

 private:
  struct State;
  std::unique_ptr<State> state_;
  int channels_ = 0;
  std::uint32_t sample_rate_ = 0;
};

}  // namespace cvt::lossy::mpeg
