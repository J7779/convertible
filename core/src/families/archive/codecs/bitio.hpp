// Convertible core - archive family: bit readers and writers shared by the codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>

#include "util/bytes.hpp"

namespace cvt::compression::detail {

/// MSB-first bit reader (bzip2, LZW variants, Quantum). Reading past the end
/// yields zero bits and sets `overrun()`.
class MsbBitReader {
 public:
  MsbBitReader() = default;
  explicit MsbBitReader(ByteView data) noexcept
      : data_(reinterpret_cast<const std::uint8_t*>(data.data())), size_(data.size()) {}

  [[nodiscard]] std::uint32_t peek(int n) noexcept {
    if (n == 0) return 0;
    if (bits_ < n) refill();
    return static_cast<std::uint32_t>(buffer_ >> (64 - n));
  }
  void skip(int n) noexcept {
    if (bits_ < n) refill();
    buffer_ <<= n;
    bits_ -= n;
  }
  [[nodiscard]] std::uint32_t get(int n) noexcept {
    const std::uint32_t v = peek(n);
    skip(n);
    return v;
  }
  [[nodiscard]] bool bit() noexcept { return get(1) != 0; }
  /// Bits consumed so far.
  [[nodiscard]] std::uint64_t position() const noexcept { return std::uint64_t{pos_} * 8 - static_cast<std::uint64_t>(bits_); }
  [[nodiscard]] bool overrun() const noexcept { return position() > std::uint64_t{size_} * 8; }
  /// Skip to the next byte boundary.
  void align() noexcept { skip(bits_ % 8); }
  /// Byte offset of the next unread whole byte (after `align()`).
  [[nodiscard]] std::size_t byte_position() const noexcept { return static_cast<std::size_t>((position() + 7) / 8); }

 private:
  void refill() noexcept {
    while (bits_ <= 56) {
      const std::uint64_t byte = pos_ < size_ ? data_[pos_] : 0u;
      buffer_ |= byte << (56 - bits_);
      ++pos_;
      bits_ += 8;
    }
  }
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  std::uint64_t buffer_ = 0;
  int bits_ = 0;
};

/// LSB-first bit reader (Deflate family, LZX-free formats, Brotli, LZMS...).
class LsbBitReader {
 public:
  LsbBitReader() = default;
  explicit LsbBitReader(ByteView data) noexcept
      : data_(reinterpret_cast<const std::uint8_t*>(data.data())), size_(data.size()) {}

  [[nodiscard]] std::uint32_t peek(int n) noexcept {
    if (n == 0) return 0;
    if (bits_ < n) refill();
    return static_cast<std::uint32_t>(buffer_ & ((std::uint64_t{1} << n) - 1));
  }
  void skip(int n) noexcept {
    if (bits_ < n) refill();
    buffer_ >>= n;
    bits_ -= n;
  }
  [[nodiscard]] std::uint32_t get(int n) noexcept {
    const std::uint32_t v = peek(n);
    skip(n);
    return v;
  }
  [[nodiscard]] std::uint64_t position() const noexcept { return std::uint64_t{pos_} * 8 - static_cast<std::uint64_t>(bits_); }
  [[nodiscard]] bool overrun() const noexcept { return position() > std::uint64_t{size_} * 8; }
  void align() noexcept { skip(bits_ % 8); }
  [[nodiscard]] std::size_t byte_position() const noexcept { return static_cast<std::size_t>((position() + 7) / 8); }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  void refill() noexcept {
    while (bits_ <= 56) {
      const std::uint64_t byte = pos_ < size_ ? data_[pos_] : 0u;
      buffer_ |= byte << bits_;
      ++pos_;
      bits_ += 8;
    }
  }
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  std::uint64_t buffer_ = 0;
  int bits_ = 0;
};

/// MSB-first bit writer.
class MsbBitWriter {
 public:
  explicit MsbBitWriter(Bytes& out) noexcept : out_(&out) {}
  void put(std::uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) {
      acc_ = (acc_ << 1) | ((value >> i) & 1u);
      if (++count_ == 8) {
        out_->push_back(static_cast<std::byte>(acc_));
        acc_ = 0;
        count_ = 0;
      }
    }
  }
  void put_wide(std::uint64_t value, int n) {
    if (n > 32) {
      put(static_cast<std::uint32_t>(value >> 32), n - 32);
      n = 32;
    }
    put(static_cast<std::uint32_t>(value), n);
  }
  void flush() {
    if (count_ != 0) {
      out_->push_back(static_cast<std::byte>(acc_ << (8 - count_)));
      acc_ = 0;
      count_ = 0;
    }
  }

 private:
  Bytes* out_;
  std::uint32_t acc_ = 0;
  int count_ = 0;
};

/// LSB-first bit writer.
class LsbBitWriter {
 public:
  explicit LsbBitWriter(Bytes& out) noexcept : out_(&out) {}
  void put(std::uint32_t value, int n) {
    acc_ |= static_cast<std::uint64_t>(value & (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1u))) << count_;
    count_ += n;
    while (count_ >= 8) {
      out_->push_back(static_cast<std::byte>(acc_ & 0xFFu));
      acc_ >>= 8;
      count_ -= 8;
    }
  }
  void flush() {
    if (count_ > 0) out_->push_back(static_cast<std::byte>(acc_ & 0xFFu));
    acc_ = 0;
    count_ = 0;
  }
  [[nodiscard]] int pending_bits() const noexcept { return count_; }

 private:
  Bytes* out_;
  std::uint64_t acc_ = 0;
  int count_ = 0;
};

}  // namespace cvt::compression::detail
