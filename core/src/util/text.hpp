// Convertible core - text utilities: ASCII helpers, number parsing, UTF-8/16/32
// and legacy single-byte transcoding, line endings.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::util {

// ---------------------------------------------------------------------------
// ASCII helpers (locale independent)
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr bool is_ascii_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] constexpr bool is_ascii_alpha(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
[[nodiscard]] constexpr char ascii_upper(char c) noexcept {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

/// ASCII-only case mapping; bytes >= 0x80 pass through untouched.
[[nodiscard]] std::string to_lower(std::string_view text);
[[nodiscard]] std::string to_upper(std::string_view text);

[[nodiscard]] constexpr std::string_view trim_left(std::string_view text) noexcept {
  while (!text.empty() && is_ascii_space(text.front())) text.remove_prefix(1);
  return text;
}
[[nodiscard]] constexpr std::string_view trim_right(std::string_view text) noexcept {
  while (!text.empty() && is_ascii_space(text.back())) text.remove_suffix(1);
  return text;
}
/// Strip ASCII whitespace from both ends.
[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept {
  return trim_right(trim_left(text));
}

/// ASCII case-insensitive comparisons.
[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}
[[nodiscard]] constexpr bool istarts_with(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}
[[nodiscard]] constexpr bool iends_with(std::string_view text, std::string_view suffix) noexcept {
  return text.size() >= suffix.size() && iequals(text.substr(text.size() - suffix.size()), suffix);
}

/// Split on a single character. "a,,b" -> {"a","","b"}; with
/// `keep_empty = false` empty pieces are dropped. Views point into `text`.
[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char separator, bool keep_empty = true);
/// Split into lines on LF, CRLF or lone CR. A trailing line terminator does
/// not produce a final empty line. Views point into `text`.
[[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text);
[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view separator);
[[nodiscard]] std::string join(const std::vector<std::string_view>& parts, std::string_view separator);
[[nodiscard]] std::string replace_all(std::string_view text, std::string_view from, std::string_view to);

// ---------------------------------------------------------------------------
// Numbers (locale independent)
// ---------------------------------------------------------------------------

/// Whole-string integer parse (optional leading '+'/'-'); nullopt on any
/// trailing garbage or overflow. Surrounding whitespace is NOT skipped.
[[nodiscard]] std::optional<std::int64_t> parse_int(std::string_view text, int base = 10) noexcept;
/// Whole-string floating point parse ("1", "-2.5", "1e-3", "inf", "nan");
/// always uses '.' as the decimal separator.
[[nodiscard]] std::optional<double> parse_double(std::string_view text) noexcept;
/// Shortest decimal representation that round-trips ("0.1", "1e+30", "42").
[[nodiscard]] std::string format_double(double value);

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

inline constexpr char32_t replacement_character = 0xFFFD;

/// Result of decoding one scalar value from UTF-8.
struct Utf8Decoded {
  char32_t code_point = replacement_character;  ///< U+FFFD when `ok` is false
  std::size_t length = 1;                       ///< bytes consumed (>= 1 unless at end)
  bool ok = false;
};

/// Decode the scalar value starting at `text[position]`. Rejects overlong
/// forms, surrogates and values above U+10FFFF. On malformed input, `length`
/// covers the maximal invalid subpart, so replacing it with one U+FFFD and
/// continuing matches the Unicode/WHATWG recommended practice.
[[nodiscard]] constexpr Utf8Decoded decode_utf8(std::string_view text, std::size_t position) noexcept {
  if (position >= text.size()) return {replacement_character, 0, false};
  const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
  const std::uint8_t b0 = byte(position);
  if (b0 < 0x80) return {b0, 1, true};
  std::size_t need = 0;
  std::uint8_t lo = 0x80;
  std::uint8_t hi = 0xBF;
  char32_t cp = 0;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    need = 1;
    cp = b0 & 0x1Fu;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    need = 2;
    cp = b0 & 0x0Fu;
    if (b0 == 0xE0) lo = 0xA0;
    if (b0 == 0xED) hi = 0x9F;
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    need = 3;
    cp = b0 & 0x07u;
    if (b0 == 0xF0) lo = 0x90;
    if (b0 == 0xF4) hi = 0x8F;
  } else {
    return {replacement_character, 1, false};
  }
  for (std::size_t k = 1; k <= need; ++k) {
    if (position + k >= text.size()) return {replacement_character, k, false};
    const std::uint8_t b = byte(position + k);
    if (b < lo || b > hi) return {replacement_character, k, false};
    cp = (cp << 6) | (b & 0x3Fu);
    lo = 0x80;
    hi = 0xBF;
  }
  return {cp, need + 1, true};
}

[[nodiscard]] constexpr bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t i = 0;
  while (i < text.size()) {
    const Utf8Decoded d = decode_utf8(text, i);
    if (!d.ok) return false;
    i += d.length;
  }
  return true;
}

/// True for values that may appear in well-formed Unicode text.
[[nodiscard]] constexpr bool is_scalar_value(char32_t cp) noexcept {
  return cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

/// Append one scalar value as UTF-8; invalid values append U+FFFD.
constexpr void append_utf8(std::string& out, char32_t cp) {
  if (!is_scalar_value(cp)) cp = replacement_character;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

/// Replace every malformed sequence with U+FFFD. `replaced`, when given,
/// receives the number of substitutions. Valid input is returned unchanged.
[[nodiscard]] std::string sanitize_utf8(std::string_view text, std::size_t* replaced = nullptr);

/// Lenient conversions between Unicode encoding forms: malformed input units
/// (bad UTF-8, lone surrogates, out-of-range UTF-32) become U+FFFD.
[[nodiscard]] std::u32string utf8_to_utf32(std::string_view text);
[[nodiscard]] std::string utf32_to_utf8(std::u32string_view text);
[[nodiscard]] std::u16string utf8_to_utf16(std::string_view text);
[[nodiscard]] std::string utf16_to_utf8(std::u16string_view text);
/// `wchar_t` strings (UTF-16 on Windows, UTF-32 elsewhere).
[[nodiscard]] std::wstring utf8_to_wide(std::string_view text);
[[nodiscard]] std::string wide_to_utf8(std::wstring_view text);

/// Number of scalar values in (possibly malformed) UTF-8.
[[nodiscard]] std::size_t utf8_length(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Byte-level text encodings
// ---------------------------------------------------------------------------

enum class TextEncoding { Utf8, Utf16LE, Utf16BE, Utf32LE, Utf32BE, Latin1, Windows1252 };

/// Canonical lowercase label: "utf-8", "utf-16le", "utf-16be", "utf-32le",
/// "utf-32be", "latin1", "windows-1252".
[[nodiscard]] std::string_view encoding_name(TextEncoding encoding) noexcept;
/// Accepts the canonical labels plus common aliases ("utf8", "UTF-16",
/// "ucs-2", "iso-8859-1", "cp1252", "ansi", ...). "utf-16"/"utf-32" without
/// an explicit byte order mean little endian.
[[nodiscard]] std::optional<TextEncoding> parse_encoding(std::string_view label) noexcept;

struct Bom {
  TextEncoding encoding = TextEncoding::Utf8;
  std::size_t length = 0;  ///< size of the mark in bytes
};
/// Byte order mark at the start of `data`, if any.
[[nodiscard]] std::optional<Bom> detect_bom(ByteView data) noexcept;
/// The byte order mark for an encoding (empty for the single-byte ones).
[[nodiscard]] std::string_view bom_bytes(TextEncoding encoding) noexcept;

struct EncodingGuess {
  TextEncoding encoding = TextEncoding::Utf8;
  std::size_t bom_length = 0;  ///< bytes to skip at the start
  bool confident = false;      ///< false when falling back to Windows-1252
};
/// Best-effort encoding detection: BOM, then valid UTF-8, then BOM-less
/// UTF-16/32 (NUL-pattern heuristic), else Windows-1252.
[[nodiscard]] EncodingGuess guess_encoding(ByteView data) noexcept;

struct DecodedText {
  std::string text;          ///< valid UTF-8
  std::size_t replaced = 0;  ///< malformed units replaced with U+FFFD
};
/// Decode bytes in `encoding` to UTF-8. A leading BOM of that same encoding
/// is dropped. Never fails: malformed input is replaced and counted.
[[nodiscard]] DecodedText decode_text(ByteView data, TextEncoding encoding);

struct EncodedText {
  Bytes bytes;
  std::size_t replaced = 0;  ///< characters not representable, written as '?'
};
/// Encode UTF-8 text into `encoding`, optionally prefixed with a BOM (ignored
/// for single-byte encodings). Malformed UTF-8 input counts as replaced.
[[nodiscard]] EncodedText encode_text(std::string_view utf8, TextEncoding encoding, bool with_bom = false);

/// Heuristic used by format detection: true when `sample` is plausibly human
/// readable text (UTF-8, or UTF-16/32 with a BOM) rather than binary data.
[[nodiscard]] bool looks_like_text(ByteView sample) noexcept;

// ---------------------------------------------------------------------------
// Line endings
// ---------------------------------------------------------------------------

enum class LineEnding { Lf, CrLf, Cr };

/// Rewrite every LF, CRLF or lone CR as `ending`.
[[nodiscard]] std::string convert_line_endings(std::string_view text, LineEnding ending);
/// The dominant line ending in `text`, or nullopt when it has no line breaks.
[[nodiscard]] std::optional<LineEnding> detect_line_ending(std::string_view text) noexcept;

}  // namespace cvt::util
