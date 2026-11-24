// Convertible core - image_modern family: a bounded TIFF / TIFF-EP structure
// parser for camera RAW files (IFD chains, SubIFDs, EXIF and maker-note
// IFDs, embedded TIFF structures with their own offset base).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::image_modern::raw {

/// TIFF field types.
enum FieldType : std::uint16_t {
  kByte = 1, kAscii = 2, kShort = 3, kLong = 4, kRational = 5, kSByte = 6, kUndefined = 7,
  kSShort = 8, kSLong = 9, kSRational = 10, kFloat = 11, kDouble = 12, kIfd = 13,
};

/// One directory entry. `value_at` is an absolute position in the file.
struct Field {
  std::uint16_t tag = 0;
  std::uint16_t type = 0;
  std::uint32_t count = 0;
  std::uint64_t value_at = 0;
};

/// A parsed directory.
struct Ifd {
  /// Absolute file position of the directory.
  std::uint64_t offset = 0;
  /// 0 for the main chain, 1 for SubIFDs of a main IFD, ...
  int depth = 0;
  std::vector<Field> fields;  // sorted by tag

  [[nodiscard]] const Field* find(std::uint16_t tag) const noexcept;
  [[nodiscard]] bool has(std::uint16_t tag) const noexcept { return find(tag) != nullptr; }
};

/// A TIFF structure inside a byte buffer. Offsets stored in the structure
/// are relative to `base` (the position of the "II"/"MM" header), which is
/// 0 for plain files and non-zero for embedded structures (maker notes,
/// RAF sections ...).
class TiffData {
 public:
  TiffData() = default;
  TiffData(ByteView data, std::uint64_t base, bool little) : data_(data), base_(base), little_(little) {}

  [[nodiscard]] ByteView data() const noexcept { return data_; }
  [[nodiscard]] std::uint64_t base() const noexcept { return base_; }
  [[nodiscard]] bool little() const noexcept { return little_; }
  [[nodiscard]] bool has(std::uint64_t at, std::uint64_t n) const noexcept {
    return at <= data_.size() && n <= data_.size() - at;
  }
  /// Unsigned integer of 1..8 bytes at an absolute position (0 when out of range).
  [[nodiscard]] std::uint64_t uint(std::uint64_t at, int bytes) const noexcept;
  [[nodiscard]] std::uint16_t u16(std::uint64_t at) const noexcept { return static_cast<std::uint16_t>(uint(at, 2)); }
  [[nodiscard]] std::uint32_t u32(std::uint64_t at) const noexcept { return static_cast<std::uint32_t>(uint(at, 4)); }

  /// Parse one directory at an absolute position; nullopt when it is out of
  /// range or implausible.
  [[nodiscard]] std::optional<Ifd> parse_ifd(std::uint64_t at, int depth) const;
  /// Absolute position of the "next IFD" link of a directory (0 = none).
  [[nodiscard]] std::uint64_t next_ifd(const Ifd& ifd) const noexcept;

  /// Field values as numbers: integers, rationals (as quotients), floats.
  /// At most `limit` values.
  [[nodiscard]] std::vector<double> numbers(const Field& field, std::size_t limit = 1u << 20) const;
  [[nodiscard]] std::vector<double> numbers(const Ifd& ifd, std::uint16_t tag, std::size_t limit = 1u << 20) const;
  /// Integer values (rationals: numerators).
  [[nodiscard]] std::vector<std::uint64_t> integers(const Field& field, std::size_t limit = 1u << 24) const;
  [[nodiscard]] std::vector<std::uint64_t> integers(const Ifd& ifd, std::uint16_t tag,
                                                    std::size_t limit = 1u << 24) const;
  /// The first value as an integer, or `fallback`.
  [[nodiscard]] std::uint64_t integer(const Ifd& ifd, std::uint16_t tag, std::uint64_t fallback = 0) const;
  [[nodiscard]] double number(const Ifd& ifd, std::uint16_t tag, double fallback = 0.0) const;
  /// ASCII value, trimmed at the first NUL and of trailing spaces.
  [[nodiscard]] std::string text(const Ifd& ifd, std::uint16_t tag, std::size_t limit = 256) const;
  /// The raw bytes of a field (empty when out of range).
  [[nodiscard]] ByteView bytes(const Field& field) const noexcept;
  [[nodiscard]] ByteView bytes(const Ifd& ifd, std::uint16_t tag) const noexcept;
  /// Bytes at a structure-relative offset (empty when out of range).
  [[nodiscard]] ByteView range(std::uint64_t relative_offset, std::uint64_t length) const noexcept;

 private:
  ByteView data_;
  std::uint64_t base_ = 0;
  bool little_ = true;
};

/// Byte order and first-IFD position of a TIFF header at `base`. Accepts the
/// standard 42 magic and the RAW variants ("IIRO", "IIRS", "MMOR" for
/// Olympus, 0x55 for Panasonic). nullopt for anything else.
struct TiffHeader {
  bool little = true;
  std::uint16_t magic = 42;
  std::uint64_t first_ifd = 0;  // absolute
};
[[nodiscard]] std::optional<TiffHeader> parse_header(ByteView data, std::uint64_t base);

/// Every directory of a file: the main chain plus SubIFDs (tag 330),
/// recursively, in discovery order. Bounded: at most `max_ifds` directories,
/// no directory visited twice, recursion depth at most 4.
struct TiffTree {
  TiffData tiff;
  std::uint16_t magic = 42;
  std::vector<Ifd> ifds;
  /// The EXIF directory of IFD0 (tag 34665), when present.
  std::optional<Ifd> exif;
};
[[nodiscard]] std::optional<TiffTree> parse_tree(ByteView data, std::uint64_t base = 0, std::size_t max_ifds = 64);

}  // namespace cvt::image_modern::raw
