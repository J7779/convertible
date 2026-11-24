// Convertible core - codec_legacy family: MSB-first bit reader and writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::legacy {

/// Reads bits most-significant first from a byte span.
///
/// Never reads outside the span: past the end it delivers zero bits and
/// latches `overrun()`, so a decoder can parse a whole syntax element and
/// test once. Reads of up to 32 bits at a time; `peek` up to 32 bits.
class BitReader {
 public:
  BitReader() noexcept = default;
  explicit BitReader(std::span<const std::uint8_t> data) noexcept
      : data_(data.data()), size_(data.size()), total_bits_(static_cast<std::uint64_t>(data.size()) * 8) {}
  explicit BitReader(ByteView data) noexcept : BitReader(as_u8(data)) {}

  /// Look at the next `n` (0..32) bits without consuming them.
  [[nodiscard]] std::uint32_t peek(int n) noexcept {
    if (n == 0) return 0;
    refill();
    return static_cast<std::uint32_t>(cache_ >> (64 - n));
  }
  /// Consume `n` (0..32) bits.
  void skip(int n) noexcept {
    while (n > 32) {
      skip(32);
      n -= 32;
    }
    if (n <= 0) return;
    refill();
    cache_ <<= n;
    cached_ -= n;
    pos_ += static_cast<std::uint64_t>(n);
  }
  /// Consume bits up to an absolute position (for skipping large payloads).
  void skip_bits(std::uint64_t n) noexcept { seek(pos_ + n); }
  /// Read `n` (0..32) bits as an unsigned number.
  [[nodiscard]] std::uint32_t bits(int n) noexcept {
    const std::uint32_t value = peek(n);
    skip(n);
    return value;
  }
  [[nodiscard]] bool bit() noexcept { return bits(1) != 0; }
  /// Read `n` (1..32) bits as a two's complement number.
  [[nodiscard]] std::int32_t sbits(int n) noexcept {
    const std::uint32_t value = bits(n);
    if (n >= 32) return static_cast<std::int32_t>(value);
    const std::uint32_t sign = 1u << (n - 1);
    return static_cast<std::int32_t>((value ^ sign) - sign);
  }
  /// Skip to the next byte boundary.
  void align() noexcept { skip(static_cast<int>((8 - pos_ % 8) % 8)); }
  [[nodiscard]] bool byte_aligned() const noexcept { return pos_ % 8 == 0; }

  /// Bits consumed so far (may exceed the size after an overrun).
  [[nodiscard]] std::uint64_t position() const noexcept { return pos_; }
  /// Bits left before the end (0 after an overrun).
  [[nodiscard]] std::uint64_t bits_left() const noexcept { return pos_ >= total_bits_ ? 0 : total_bits_ - pos_; }
  /// True once a read went past the end of the data.
  [[nodiscard]] bool overrun() const noexcept { return pos_ > total_bits_; }
  /// Byte offset of the next unread bit (rounded down).
  [[nodiscard]] std::size_t byte_position() const noexcept {
    return static_cast<std::size_t>(std::min<std::uint64_t>(pos_ / 8, size_));
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }

  /// Jump to an absolute bit position.
  void seek(std::uint64_t bit_position) noexcept {
    const std::uint64_t byte = bit_position / 8;
    pos_ = byte * 8;
    cache_ = 0;
    cached_ = 0;
    fill_byte_ = byte >= size_ ? size_ + 1 : static_cast<std::size_t>(byte);
    if (byte >= size_) {
      pos_ = bit_position;  // past the end: every further read is zero
      return;
    }
    skip(static_cast<int>(bit_position % 8));
  }

 private:
  void refill() noexcept {
    while (cached_ <= 56) {
      std::uint64_t byte = 0;
      if (fill_byte_ < size_) byte = data_[fill_byte_];
      ++fill_byte_;
      cache_ |= byte << (56 - cached_);
      cached_ += 8;
    }
  }

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::uint64_t total_bits_ = 0;
  std::uint64_t pos_ = 0;
  std::uint64_t cache_ = 0;
  int cached_ = 0;
  std::size_t fill_byte_ = 0;
};

/// Writes bits most-significant first.
class BitWriter {
 public:
  /// Append the low `n` (0..32) bits of `value`.
  void put(int n, std::uint32_t value) {
    if (n <= 0) return;
    const std::uint64_t masked = n >= 32 ? value : (value & ((1u << n) - 1u));
    acc_ = (acc_ << n) | masked;
    count_ += n;
    while (count_ >= 8) {
      count_ -= 8;
      out_.push_back(static_cast<std::uint8_t>(acc_ >> count_));
    }
    acc_ &= (1ull << count_) - 1ull;
  }
  void put_bit(bool value) { put(1, value ? 1u : 0u); }
  /// Append `n` bits of a signed number in two's complement.
  void put_signed(int n, std::int32_t value) { put(n, static_cast<std::uint32_t>(value)); }
  /// Pad with `fill` bits (0 or 1) to the next byte boundary.
  void align(bool fill = false) {
    const int pad = (8 - count_ % 8) % 8;
    if (pad != 0) put(pad, fill ? (1u << pad) - 1u : 0u);
  }
  [[nodiscard]] bool byte_aligned() const noexcept { return count_ == 0; }
  /// Bits written so far.
  [[nodiscard]] std::uint64_t bit_count() const noexcept { return static_cast<std::uint64_t>(out_.size()) * 8 + static_cast<std::uint64_t>(count_); }
  /// Append whole bytes (the writer must be byte aligned).
  void put_bytes(std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t b : bytes) put(8, b);
  }
  /// The bytes written so far; call after `align()`.
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return out_; }
  [[nodiscard]] std::vector<std::uint8_t> take() {
    align();
    return std::move(out_);
  }
  void clear() {
    out_.clear();
    acc_ = 0;
    count_ = 0;
  }

 private:
  std::vector<std::uint8_t> out_;
  std::uint64_t acc_ = 0;
  int count_ = 0;
};

/// Copy a byte vector into Bytes.
[[nodiscard]] inline Bytes to_cvt_bytes(const std::vector<std::uint8_t>& data) {
  return to_bytes(std::span<const std::uint8_t>(data));
}

}  // namespace cvt::legacy
