// Convertible core - byte buffers and endian-aware readers/writers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cvt {

/// Owned binary data: file contents, encoded output, embedded resources.
using Bytes = std::vector<std::byte>;
/// Borrowed binary data. Never outlives the buffer it points into.
using ByteView = std::span<const std::byte>;

/// View text as bytes (no copy).
[[nodiscard]] inline ByteView as_bytes(std::string_view text) noexcept {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}
/// View unsigned chars as bytes (no copy).
[[nodiscard]] inline ByteView as_bytes(std::span<const std::uint8_t> data) noexcept {
  return {reinterpret_cast<const std::byte*>(data.data()), data.size()};
}
/// View bytes as characters (no copy, no validation).
[[nodiscard]] inline std::string_view as_chars(ByteView data) noexcept {
  return {reinterpret_cast<const char*>(data.data()), data.size()};
}
/// View bytes as unsigned chars, the shape most C libraries want (no copy).
[[nodiscard]] inline std::span<const std::uint8_t> as_u8(ByteView data) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
}
/// Copy text into an owned buffer.
[[nodiscard]] inline Bytes to_bytes(std::string_view text) {
  const ByteView view = as_bytes(text);
  return Bytes(view.begin(), view.end());
}
/// Copy unsigned chars into an owned buffer.
[[nodiscard]] inline Bytes to_bytes(std::span<const std::uint8_t> data) {
  const ByteView view = as_bytes(data);
  return Bytes(view.begin(), view.end());
}
/// Copy a view into an owned buffer.
[[nodiscard]] inline Bytes to_bytes(ByteView data) { return Bytes(data.begin(), data.end()); }
/// Copy bytes into a string (no validation).
[[nodiscard]] inline std::string to_string(ByteView data) { return std::string(as_chars(data)); }

inline void append(Bytes& out, ByteView data) { out.insert(out.end(), data.begin(), data.end()); }
inline void append(Bytes& out, std::string_view text) { append(out, as_bytes(text)); }

namespace util {

/// Bounds-checked sequential reader over a byte span.
///
/// Reads never throw and never run past the end: a failed read returns zero
/// (or an empty view) and latches the failure flag, so a parser can read a
/// whole header and test `ok()` once:
///
///   util::ByteReader r(ctx.input);
///   auto width = r.u32be(); auto height = r.u32be();
///   if (!r.ok()) return fail(Status::Decode, "truncated header");
class ByteReader {
 public:
  constexpr ByteReader() noexcept = default;
  constexpr explicit ByteReader(ByteView data) noexcept : data_(data) {}

  [[nodiscard]] constexpr std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] constexpr std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] constexpr std::size_t remaining() const noexcept { return data_.size() - pos_; }
  [[nodiscard]] constexpr bool at_end() const noexcept { return pos_ == data_.size(); }
  /// False once any read, skip or seek went out of bounds.
  [[nodiscard]] constexpr bool ok() const noexcept { return !failed_; }
  constexpr void clear_error() noexcept { failed_ = false; }

  /// Absolute reposition; fails (and stays put) when `position > size()`.
  constexpr bool seek(std::size_t position) noexcept {
    if (position > data_.size()) return set_failed();
    pos_ = position;
    return true;
  }
  constexpr bool skip(std::size_t count) noexcept {
    if (count > remaining()) return set_failed();
    pos_ += count;
    return true;
  }

  /// Read an integer of any width in the given byte order.
  template <std::integral T>
  [[nodiscard]] constexpr T read(std::endian order) noexcept {
    using U = std::make_unsigned_t<T>;
    if (sizeof(T) > remaining()) {
      set_failed();
      return T{};
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      const auto byte = static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data_[pos_ + i]));
      if (order == std::endian::little) {
        value |= byte << (8 * i);
      } else {
        value = (value << 8) | byte;
      }
    }
    pos_ += sizeof(T);
    return static_cast<T>(static_cast<U>(value));
  }

  [[nodiscard]] constexpr std::uint8_t u8() noexcept { return read<std::uint8_t>(std::endian::little); }
  [[nodiscard]] constexpr std::int8_t i8() noexcept { return read<std::int8_t>(std::endian::little); }
  [[nodiscard]] constexpr std::uint16_t u16le() noexcept { return read<std::uint16_t>(std::endian::little); }
  [[nodiscard]] constexpr std::uint16_t u16be() noexcept { return read<std::uint16_t>(std::endian::big); }
  [[nodiscard]] constexpr std::int16_t i16le() noexcept { return read<std::int16_t>(std::endian::little); }
  [[nodiscard]] constexpr std::int16_t i16be() noexcept { return read<std::int16_t>(std::endian::big); }
  [[nodiscard]] constexpr std::uint32_t u24le() noexcept { return read_u24(std::endian::little); }
  [[nodiscard]] constexpr std::uint32_t u24be() noexcept { return read_u24(std::endian::big); }
  [[nodiscard]] constexpr std::uint32_t u32le() noexcept { return read<std::uint32_t>(std::endian::little); }
  [[nodiscard]] constexpr std::uint32_t u32be() noexcept { return read<std::uint32_t>(std::endian::big); }
  [[nodiscard]] constexpr std::int32_t i32le() noexcept { return read<std::int32_t>(std::endian::little); }
  [[nodiscard]] constexpr std::int32_t i32be() noexcept { return read<std::int32_t>(std::endian::big); }
  [[nodiscard]] constexpr std::uint64_t u64le() noexcept { return read<std::uint64_t>(std::endian::little); }
  [[nodiscard]] constexpr std::uint64_t u64be() noexcept { return read<std::uint64_t>(std::endian::big); }
  [[nodiscard]] constexpr std::int64_t i64le() noexcept { return read<std::int64_t>(std::endian::little); }
  [[nodiscard]] constexpr std::int64_t i64be() noexcept { return read<std::int64_t>(std::endian::big); }
  [[nodiscard]] constexpr float f32le() noexcept { return std::bit_cast<float>(u32le()); }
  [[nodiscard]] constexpr float f32be() noexcept { return std::bit_cast<float>(u32be()); }
  [[nodiscard]] constexpr double f64le() noexcept { return std::bit_cast<double>(u64le()); }
  [[nodiscard]] constexpr double f64be() noexcept { return std::bit_cast<double>(u64be()); }

  /// Take the next `count` bytes; empty view (and failure) if not available.
  [[nodiscard]] constexpr ByteView bytes(std::size_t count) noexcept {
    if (count > remaining()) {
      set_failed();
      return {};
    }
    const ByteView out = data_.subspan(pos_, count);
    pos_ += count;
    return out;
  }
  /// Same as `bytes()`, viewed as characters.
  [[nodiscard]] std::string_view chars(std::size_t count) noexcept { return as_chars(bytes(count)); }
  /// Everything not yet consumed; the reader moves to the end.
  [[nodiscard]] constexpr ByteView rest() noexcept { return bytes(remaining()); }
  /// The unconsumed part without consuming it.
  [[nodiscard]] constexpr ByteView peek_rest() const noexcept { return data_.subspan(pos_); }
  /// Next byte without consuming it; `fallback` at the end of input.
  [[nodiscard]] constexpr std::uint8_t peek_u8(std::uint8_t fallback = 0) const noexcept {
    return pos_ < data_.size() ? std::to_integer<std::uint8_t>(data_[pos_]) : fallback;
  }
  /// True when the unconsumed input begins with `magic` (nothing is consumed).
  [[nodiscard]] bool starts_with(std::string_view magic) const noexcept {
    return remaining() >= magic.size() && std::memcmp(data_.data() + pos_, magic.data(), magic.size()) == 0;
  }
  /// Consume `magic` if present.
  bool consume(std::string_view magic) noexcept {
    if (!starts_with(magic)) return false;
    pos_ += magic.size();
    return true;
  }
  /// Characters up to (not including) the next NUL, which is consumed too.
  /// Fails if no terminator is found.
  [[nodiscard]] std::string_view cstring() noexcept {
    const std::string_view tail = as_chars(data_.subspan(pos_));
    const std::size_t end = tail.find('\0');
    if (end == std::string_view::npos) {
      set_failed();
      return {};
    }
    pos_ += end + 1;
    return tail.substr(0, end);
  }
  /// A sub-reader over the next `count` bytes (consumed from this reader).
  [[nodiscard]] constexpr ByteReader sub(std::size_t count) noexcept { return ByteReader(bytes(count)); }

 private:
  constexpr bool set_failed() noexcept {
    failed_ = true;
    return false;
  }
  constexpr std::uint32_t read_u24(std::endian order) noexcept {
    if (remaining() < 3) {
      set_failed();
      return 0;
    }
    const auto b0 = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[pos_]));
    const auto b1 = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[pos_ + 1]));
    const auto b2 = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[pos_ + 2]));
    pos_ += 3;
    return order == std::endian::little ? (b0 | (b1 << 8) | (b2 << 16)) : ((b0 << 16) | (b1 << 8) | b2);
  }

  ByteView data_{};
  std::size_t pos_ = 0;
  bool failed_ = false;
};

/// Growable output buffer with endian-aware appends.
class ByteWriter {
 public:
  ByteWriter() = default;
  explicit ByteWriter(std::size_t reserve) { buffer_.reserve(reserve); }

  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] ByteView view() const noexcept { return buffer_; }
  [[nodiscard]] const Bytes& data() const noexcept { return buffer_; }
  /// Move the accumulated bytes out; the writer is empty afterwards.
  [[nodiscard]] Bytes take() noexcept { return std::exchange(buffer_, {}); }
  void reserve(std::size_t capacity) { buffer_.reserve(capacity); }

  /// Append an integer of any width in the given byte order.
  template <std::integral T>
  ByteWriter& write(T value, std::endian order) {
    const auto wide = static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value));
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      const std::size_t shift = order == std::endian::little ? 8 * i : 8 * (sizeof(T) - 1 - i);
      buffer_.push_back(static_cast<std::byte>((wide >> shift) & 0xFFu));
    }
    return *this;
  }

  ByteWriter& u8(std::uint8_t v) { return write(v, std::endian::little); }
  ByteWriter& i8(std::int8_t v) { return write(v, std::endian::little); }
  ByteWriter& u16le(std::uint16_t v) { return write(v, std::endian::little); }
  ByteWriter& u16be(std::uint16_t v) { return write(v, std::endian::big); }
  ByteWriter& i16le(std::int16_t v) { return write(v, std::endian::little); }
  ByteWriter& i16be(std::int16_t v) { return write(v, std::endian::big); }
  ByteWriter& u32le(std::uint32_t v) { return write(v, std::endian::little); }
  ByteWriter& u32be(std::uint32_t v) { return write(v, std::endian::big); }
  ByteWriter& i32le(std::int32_t v) { return write(v, std::endian::little); }
  ByteWriter& i32be(std::int32_t v) { return write(v, std::endian::big); }
  ByteWriter& u64le(std::uint64_t v) { return write(v, std::endian::little); }
  ByteWriter& u64be(std::uint64_t v) { return write(v, std::endian::big); }
  ByteWriter& i64le(std::int64_t v) { return write(v, std::endian::little); }
  ByteWriter& i64be(std::int64_t v) { return write(v, std::endian::big); }
  ByteWriter& f32le(float v) { return u32le(std::bit_cast<std::uint32_t>(v)); }
  ByteWriter& f32be(float v) { return u32be(std::bit_cast<std::uint32_t>(v)); }
  ByteWriter& f64le(double v) { return u64le(std::bit_cast<std::uint64_t>(v)); }
  ByteWriter& f64be(double v) { return u64be(std::bit_cast<std::uint64_t>(v)); }
  ByteWriter& u24le(std::uint32_t v) {
    return u8(static_cast<std::uint8_t>(v)).u8(static_cast<std::uint8_t>(v >> 8)).u8(static_cast<std::uint8_t>(v >> 16));
  }
  ByteWriter& u24be(std::uint32_t v) {
    return u8(static_cast<std::uint8_t>(v >> 16)).u8(static_cast<std::uint8_t>(v >> 8)).u8(static_cast<std::uint8_t>(v));
  }

  ByteWriter& bytes(ByteView data) {
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    return *this;
  }
  ByteWriter& bytes(std::span<const std::uint8_t> data) { return bytes(as_bytes(data)); }
  ByteWriter& text(std::string_view data) { return bytes(as_bytes(data)); }
  ByteWriter& zeros(std::size_t count) {
    buffer_.insert(buffer_.end(), count, std::byte{0});
    return *this;
  }
  /// Pad with zero bytes until `size()` is a multiple of `alignment`.
  ByteWriter& align(std::size_t alignment) {
    if (alignment > 1 && buffer_.size() % alignment != 0) zeros(alignment - buffer_.size() % alignment);
    return *this;
  }

  /// Overwrite an integer written earlier (e.g. a length field known only at
  /// the end). Returns false if the range is out of bounds.
  template <std::integral T>
  bool patch(std::size_t position, T value, std::endian order) noexcept {
    if (position > buffer_.size() || sizeof(T) > buffer_.size() - position) return false;
    const auto wide = static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value));
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      const std::size_t shift = order == std::endian::little ? 8 * i : 8 * (sizeof(T) - 1 - i);
      buffer_[position + i] = static_cast<std::byte>((wide >> shift) & 0xFFu);
    }
    return true;
  }
  bool patch_u32le(std::size_t position, std::uint32_t v) noexcept { return patch(position, v, std::endian::little); }
  bool patch_u32be(std::size_t position, std::uint32_t v) noexcept { return patch(position, v, std::endian::big); }

 private:
  Bytes buffer_;
};

}  // namespace util
}  // namespace cvt
