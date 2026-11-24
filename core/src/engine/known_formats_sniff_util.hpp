// Convertible core - helpers shared by the content sniffers (private header).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "engine/sniffers.hpp"
#include "util/bytes.hpp"

namespace cvt::sniff::detail {

/// Bounds-checked random access to the sniffed bytes. Out-of-range reads
/// yield zero; callers test `has()` where a zero would be meaningful.
struct View {
  ByteView d;

  [[nodiscard]] std::size_t size() const noexcept { return d.size(); }
  [[nodiscard]] bool has(std::size_t offset, std::size_t count) const noexcept {
    return offset <= d.size() && count <= d.size() - offset;
  }
  [[nodiscard]] bool has64(std::uint64_t offset, std::uint64_t count) const noexcept {
    return offset <= d.size() && count <= d.size() - offset;
  }
  [[nodiscard]] std::uint8_t u8(std::size_t i) const noexcept {
    return i < d.size() ? std::to_integer<std::uint8_t>(d[i]) : std::uint8_t{0};
  }
  [[nodiscard]] std::uint16_t u16le(std::size_t i) const noexcept {
    return static_cast<std::uint16_t>(u8(i) | (u8(i + 1) << 8));
  }
  [[nodiscard]] std::uint16_t u16be(std::size_t i) const noexcept {
    return static_cast<std::uint16_t>((u8(i) << 8) | u8(i + 1));
  }
  [[nodiscard]] std::uint32_t u32le(std::size_t i) const noexcept {
    return static_cast<std::uint32_t>(u16le(i)) | (static_cast<std::uint32_t>(u16le(i + 2)) << 16);
  }
  [[nodiscard]] std::uint32_t u32be(std::size_t i) const noexcept {
    return (static_cast<std::uint32_t>(u16be(i)) << 16) | static_cast<std::uint32_t>(u16be(i + 2));
  }
  [[nodiscard]] std::uint64_t u64le(std::size_t i) const noexcept {
    return static_cast<std::uint64_t>(u32le(i)) | (static_cast<std::uint64_t>(u32le(i + 4)) << 32);
  }
  [[nodiscard]] std::uint64_t u64be(std::size_t i) const noexcept {
    return (static_cast<std::uint64_t>(u32be(i)) << 32) | static_cast<std::uint64_t>(u32be(i + 4));
  }
  [[nodiscard]] std::uint16_t u16(std::size_t i, bool big) const noexcept { return big ? u16be(i) : u16le(i); }
  [[nodiscard]] std::uint32_t u32(std::size_t i, bool big) const noexcept { return big ? u32be(i) : u32le(i); }
  [[nodiscard]] std::uint64_t u64(std::size_t i, bool big) const noexcept { return big ? u64be(i) : u64le(i); }
  [[nodiscard]] std::string_view chars() const noexcept { return as_chars(d); }
  /// `count` bytes at `offset` as characters, clipped to the data.
  [[nodiscard]] std::string_view str(std::size_t offset, std::size_t count) const noexcept {
    if (offset >= d.size()) return {};
    return chars().substr(offset, std::min(count, d.size() - offset));
  }
  /// True when the bytes at `offset` equal `text`.
  [[nodiscard]] bool eq(std::size_t offset, std::string_view text) const noexcept {
    return has(offset, text.size()) && chars().substr(offset, text.size()) == text;
  }
  /// First occurrence of `needle` in [from, to), or npos.
  [[nodiscard]] std::size_t find(std::string_view needle, std::size_t from, std::size_t to) const noexcept {
    to = std::min(to, d.size());
    if (from >= to) return std::string_view::npos;
    const std::size_t at = chars().substr(from, to - from).find(needle);
    return at == std::string_view::npos ? at : from + at;
  }
};

// ---------------------------------------------------------------- verdicts

[[nodiscard]] constexpr Verdict nothing() noexcept { return {}; }
[[nodiscard]] constexpr Verdict container_only() noexcept { return {{}, 0, false, true}; }
[[nodiscard]] constexpr Verdict found(std::string_view id, int confidence) noexcept {
  return {id, confidence, false, true};
}
[[nodiscard]] constexpr Verdict base_reading(std::string_view id, int confidence) noexcept {
  return {id, confidence, true, true};
}

// ---------------------------------------------------------------- ASCII helpers

[[nodiscard]] constexpr char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] constexpr bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
[[nodiscard]] constexpr bool is_alnum(char c) noexcept { return is_digit(c) || is_alpha(c); }
[[nodiscard]] constexpr bool is_hex(char c) noexcept {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
[[nodiscard]] constexpr bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
[[nodiscard]] constexpr bool iequal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}
[[nodiscard]] constexpr bool istarts(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && iequal(text.substr(0, prefix.size()), prefix);
}
[[nodiscard]] constexpr bool iends(std::string_view text, std::string_view suffix) noexcept {
  return text.size() >= suffix.size() && iequal(text.substr(text.size() - suffix.size()), suffix);
}
/// Case-insensitive search; npos when absent.
[[nodiscard]] constexpr std::size_t ifind(std::string_view text, std::string_view needle, std::size_t from = 0) noexcept {
  if (needle.empty()) return from <= text.size() ? from : std::string_view::npos;
  for (std::size_t i = from; i + needle.size() <= text.size(); ++i) {
    if (lower(text[i]) == lower(needle[0]) && iequal(text.substr(i, needle.size()), needle)) return i;
  }
  return std::string_view::npos;
}
[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}
[[nodiscard]] constexpr std::string_view trim_left(std::string_view text) noexcept {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  return text;
}

// ---------------------------------------------------------------- file names

/// The last path component.
[[nodiscard]] constexpr std::string_view leaf(std::string_view filename) noexcept {
  const std::size_t slash = filename.find_last_of("/\\");
  return slash == std::string_view::npos ? filename : filename.substr(slash + 1);
}
/// True when the file name ends in "." + `extension` (case-insensitive), the
/// way `filename_extensions` would report it ("a.tar.gz" has "tar.gz" and "gz").
[[nodiscard]] constexpr bool has_ext(std::string_view filename, std::string_view extension) noexcept {
  const std::string_view name = leaf(filename);
  if (extension.empty() || name.size() < extension.size() + 2) return false;
  const std::size_t dot = name.size() - extension.size() - 1;
  if (name[dot] != '.') return false;
  // The part before the extension must not be only dots (".gitignore" has no extension).
  bool named = false;
  for (std::size_t i = 0; i < dot; ++i) named = named || name[i] != '.';
  return named && iequal(name.substr(dot + 1), extension);
}
/// The first entry of `ids` that is also an extension of `filename`, else `fallback`.
[[nodiscard]] constexpr std::string_view refine(std::string_view filename, std::string_view fallback,
                                                std::initializer_list<std::string_view> ids) noexcept {
  for (const std::string_view id : ids) {
    if (has_ext(filename, id)) return id;
  }
  return fallback;
}

// ---------------------------------------------------------------- text

/// The first few KiB of a file as ASCII-compatible text, decoded from UTF-8
/// (BOM removed) or UTF-16 (with a BOM, or ASCII-heavy without one).
/// Characters outside ASCII become '\x80' in UTF-16 input. No allocation.
struct TextHead {
  static constexpr std::size_t capacity = 4096;
  std::array<char, capacity> buffer;  // only [0, length) is meaningful
  std::size_t length = 0;
  /// True when the sample is plausibly text (no NULs in UTF-8, few control characters).
  bool text = false;
  bool utf16 = false;
  /// True when the sample is all of the file (not cut at `capacity`).
  bool complete = false;

  [[nodiscard]] std::string_view view() const noexcept { return {buffer.data(), length}; }
};

/// Decode the head of `data` (see TextHead).
void decode_head(ByteView data, TextHead& head) noexcept;

/// Iterate over lines of `text` (without the line break); at most `limit` lines.
template <class F>
constexpr void for_each_line(std::string_view text, std::size_t limit, F&& visit) {
  std::size_t start = 0;
  for (std::size_t n = 0; n < limit && start < text.size(); ++n) {
    std::size_t end = text.find('\n', start);
    const bool last = end == std::string_view::npos;
    if (last) end = text.size();
    std::string_view line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!visit(line)) return;
    if (last) return;
    start = end + 1;
  }
}

// ---------------------------------------------------------------- the text classifiers

Verdict classify_markup(ByteView data, std::string_view filename, std::string_view wanted) noexcept;
Verdict classify_json(ByteView data, std::string_view filename, std::string_view wanted) noexcept;
Verdict classify_text(ByteView data, std::string_view filename, std::string_view wanted) noexcept;

}  // namespace cvt::sniff::detail
