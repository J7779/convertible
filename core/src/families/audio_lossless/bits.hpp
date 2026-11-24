// Convertible core - audio_lossless family: MSB-first bit reader and writer
// shared by the FLAC, ALAC, WavPack and TTA codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::audio {

/// Big-endian (MSB-first) bit reader. Reading past the end yields zero bits
/// and sets `overrun()`, so a decoder can check once per frame.
class BitReader {
 public:
  BitReader() = default;
  explicit BitReader(ByteView data) noexcept : data_(as_u8(data)) {}

  /// Read 0..32 bits.
  std::uint32_t read(unsigned count) noexcept {
    if (count == 0) return 0;
    const std::uint64_t v = peek64(count);
    position_ += count;
    if (position_ > bit_size()) overrun_ = true;
    return static_cast<std::uint32_t>(v);
  }
  std::uint64_t read64(unsigned count) noexcept {
    if (count <= 32) return read(count);
    const std::uint64_t high = read(count - 32);
    return (high << 32) | read(32);
  }
  std::int32_t read_signed(unsigned count) noexcept {
    if (count == 0) return 0;
    const std::uint32_t v = read(count);
    if (count >= 32) return static_cast<std::int32_t>(v);
    const std::uint32_t sign = 1u << (count - 1);
    return static_cast<std::int32_t>((v ^ sign)) - static_cast<std::int32_t>(sign);
  }
  bool bit() noexcept { return read(1) != 0; }
  /// Peek up to 32 bits without consuming.
  [[nodiscard]] std::uint32_t peek(unsigned count) const noexcept { return static_cast<std::uint32_t>(peek64(count)); }
  /// Count consecutive zero bits (consumed) until a one (consumed too);
  /// stops at `limit` zeros.
  std::uint32_t unary_zeros(std::uint32_t limit) noexcept {
    std::uint32_t count = 0;
    while (count < limit) {
      if (position_ >= bit_size()) {
        overrun_ = true;
        return count;
      }
      const auto byte = static_cast<std::size_t>(position_ >> 3);
      const unsigned shift = static_cast<unsigned>(position_ & 7);
      const std::uint8_t rest = static_cast<std::uint8_t>(data_[byte] << shift);
      if (rest == 0) {
        const unsigned skipped = 8 - shift;
        count += skipped;
        position_ += skipped;
        continue;
      }
      unsigned lead = 0;
      for (std::uint8_t probe = rest; (probe & 0x80) == 0; probe = static_cast<std::uint8_t>(probe << 1)) ++lead;
      count += lead;
      position_ += lead + 1;
      return count;
    }
    return count;
  }
  void skip(std::uint64_t count) noexcept {
    position_ += count;
    if (position_ > bit_size()) overrun_ = true;
  }
  void align() noexcept { position_ = (position_ + 7) & ~std::uint64_t{7}; }
  [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
  void set_position(std::uint64_t position) noexcept { position_ = position; }
  [[nodiscard]] std::uint64_t bit_size() const noexcept { return std::uint64_t{data_.size()} * 8; }
  [[nodiscard]] std::size_t byte_position() const noexcept { return static_cast<std::size_t>(position_ >> 3); }
  [[nodiscard]] bool overrun() const noexcept { return overrun_ || position_ > bit_size(); }
  [[nodiscard]] std::uint64_t remaining_bits() const noexcept {
    return position_ >= bit_size() ? 0 : bit_size() - position_;
  }

 private:
  [[nodiscard]] std::uint64_t peek64(unsigned count) const noexcept {
    std::uint64_t v = 0;
    std::uint64_t pos = position_;
    const std::uint64_t end = bit_size();
    unsigned left = count;
    while (left > 0) {
      if (pos >= end) {
        v <<= left;  // zeros past the end; read() flags the overrun
        break;
      }
      const unsigned offset = static_cast<unsigned>(pos & 7);
      const unsigned avail = 8 - offset;
      const unsigned take = left < avail ? left : avail;
      const std::uint8_t byte = data_[static_cast<std::size_t>(pos >> 3)];
      const unsigned bits = (byte >> (avail - take)) & ((1u << take) - 1u);
      v = (v << take) | bits;
      pos += take;
      left -= take;
    }
    return v;
  }

  std::span<const std::uint8_t> data_{};
  std::uint64_t position_ = 0;
  bool overrun_ = false;
};

/// Big-endian bit writer.
class BitWriter {
 public:
  void write(std::uint32_t value, unsigned count) {
    if (count == 0) return;
    if (count < 32) value &= (1u << count) - 1u;
    accumulator_ = (accumulator_ << count) | value;
    filled_ += count;
    while (filled_ >= 8) {
      filled_ -= 8;
      bytes_.push_back(static_cast<std::uint8_t>(accumulator_ >> filled_));
    }
    accumulator_ &= (std::uint64_t{1} << filled_) - 1u;
  }
  void write64(std::uint64_t value, unsigned count) {
    if (count > 32) {
      write(static_cast<std::uint32_t>(value >> 32), count - 32);
      write(static_cast<std::uint32_t>(value), 32);
    } else {
      write(static_cast<std::uint32_t>(value), count);
    }
  }
  void write_signed(std::int32_t value, unsigned count) { write(static_cast<std::uint32_t>(value), count); }
  /// `count` zero bits then a one.
  void unary(std::uint32_t zeros) {
    while (zeros >= 32) {
      write(0, 32);
      zeros -= 32;
    }
    write(1, zeros + 1);
  }
  void align() {
    if (filled_ > 0) write(0, 8 - filled_);
  }
  /// Append everything another writer holds, including its partial byte.
  void append(const BitWriter& other) {
    for (const std::uint8_t byte : other.bytes_) write(byte, 8);
    if (other.filled_ > 0) write(static_cast<std::uint32_t>(other.accumulator_), other.filled_);
  }
  [[nodiscard]] std::uint64_t bit_count() const noexcept { return std::uint64_t{bytes_.size()} * 8 + filled_; }
  /// The bytes written so far (call align() first for a complete stream).
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<std::uint8_t> take() {
    align();
    std::vector<std::uint8_t> out;
    out.swap(bytes_);
    return out;
  }
  void clear() {
    bytes_.clear();
    accumulator_ = 0;
    filled_ = 0;
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::uint64_t accumulator_ = 0;
  unsigned filled_ = 0;
};

}  // namespace cvt::audio
