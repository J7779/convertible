// Convertible core - codec_hevc family: internal declarations.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "codec/codec.hpp"
#include "engine/errors.hpp"
#include "families/codec_hevc/hevc_api.hpp"
#include "util/bytes.hpp"

namespace cvt {
class Registry;
}

namespace cvt::hevc {

/// Bounds-checked MSB-first bit reader over an RBSP. Reading past the end
/// yields zeros and sets the failure flag; check `ok()` once at the end.
class BitReader {
 public:
  explicit BitReader(ByteView data) noexcept : data_(data) {}

  [[nodiscard]] std::uint32_t bit() noexcept {
    if (pos_ >= data_.size() * 8) {
      failed_ = true;
      return 0;
    }
    const std::uint32_t byte = std::to_integer<std::uint32_t>(data_[pos_ >> 3]);
    const std::uint32_t value = (byte >> (7 - (pos_ & 7))) & 1u;
    ++pos_;
    return value;
  }
  /// Up to 32 bits.
  [[nodiscard]] std::uint32_t bits(unsigned count) noexcept {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value = (value << 1) | bit();
    return value;
  }
  [[nodiscard]] std::uint64_t bits64(unsigned count) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value = (value << 1) | bit();
    return value;
  }
  [[nodiscard]] bool flag() noexcept { return bit() != 0; }
  void skip(std::size_t count) noexcept {
    if (count > data_.size() * 8 - std::min(pos_, data_.size() * 8)) {
      pos_ = data_.size() * 8;
      failed_ = true;
      return;
    }
    pos_ += count;
  }
  /// Unsigned Exp-Golomb; values above 2^32 - 2 are a failure.
  [[nodiscard]] std::uint32_t ue() noexcept {
    unsigned zeros = 0;
    while (bit() == 0) {
      if (failed_ || ++zeros > 31) {
        failed_ = true;
        return 0;
      }
    }
    if (zeros == 0) return 0;
    const std::uint64_t value = (std::uint64_t{1} << zeros) - 1 + bits(zeros);
    if (value > 0xFFFFFFFEu) {
      failed_ = true;
      return 0;
    }
    return static_cast<std::uint32_t>(value);
  }
  /// Signed Exp-Golomb.
  [[nodiscard]] std::int32_t se() noexcept {
    const std::uint32_t code = ue();
    const std::int64_t magnitude = (static_cast<std::int64_t>(code) + 1) / 2;
    return static_cast<std::int32_t>((code & 1u) ? magnitude : -magnitude);
  }
  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() * 8 - std::min(pos_, data_.size() * 8); }

 private:
  ByteView data_;
  std::size_t pos_ = 0;
  bool failed_ = false;
};

/// True when `nal` (a VCL unit) is the first slice segment of a picture.
[[nodiscard]] bool first_slice_in_picture(ByteView nal) noexcept;

/// True when this non-VCL NAL type starts a new access unit if it follows
/// the VCL units of a picture (AUD, parameter sets, prefix SEI, reserved).
[[nodiscard]] constexpr bool starts_access_unit(std::uint8_t type) noexcept {
  return (type >= 32 && type <= 35) || type == 39 || (type >= 41 && type <= 44) || (type >= 48 && type <= 55);
}

/// Groups the NAL units of a stream into access units (H.265 7.4.2.4.4).
class AccessUnitSplitter {
 public:
  /// Feed NAL units in stream order; returns the units of a completed access
  /// unit when `nal` starts the next one.
  std::optional<std::vector<ByteView>> push(ByteView nal);
  /// The last, possibly incomplete, access unit.
  std::optional<std::vector<ByteView>> finish();

 private:
  std::vector<ByteView> current_;
  bool has_vcl_ = false;
};

/// Picture order count of the pictures of a stream, in decode order, as far
/// as it is needed to put them in presentation order.
class PocTracker {
 public:
  /// Remember parameter sets.
  void parameter_set(ByteView nal);
  /// POC of the picture whose first slice segment is `nal`, or nullopt when
  /// its parameter sets are unknown or the header is damaged. `new_sequence`
  /// is set for IRAP pictures that start a coded video sequence.
  std::optional<std::int64_t> picture(ByteView nal, bool& new_sequence);

 private:
  std::map<std::uint32_t, SequenceInfo> sps_;
  std::map<std::uint32_t, PictureParamsLite> pps_;
  std::int64_t prev_tid0_poc_ = 0;
  bool first_picture_ = true;
  bool after_eos_ = false;
};

/// Kvazaar's process-wide tables, set up exactly once (thread-safe).
[[nodiscard]] bool ensure_encoder_ready();

/// Map Convertible's 1..100 quality to a Kvazaar base QP.
[[nodiscard]] int quality_to_qp(int quality) noexcept;

/// Factories registered in codec::Registry.
[[nodiscard]] std::unique_ptr<codec::VideoDecoder> make_decoder();
[[nodiscard]] std::unique_ptr<codec::VideoEncoder> make_encoder();

}  // namespace cvt::hevc
