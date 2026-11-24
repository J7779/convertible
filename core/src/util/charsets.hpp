// Convertible core - legacy character sets for every family.
//
// Single-byte code pages (ISO-8859-1..16, Windows-1250..1258 and 874, KOI8-R/U,
// Mac Roman, IBM 437/850/852/866, EBCDIC 037/500/1047) and the East Asian
// multi-byte encodings (Shift_JIS, Windows-31J/cp932, EUC-JP, ISO-2022-JP,
// GBK/cp936, GB18030, Big5/cp950, EUC-KR/cp949), decoded to and encoded from
// UTF-8 with tables taken from Python's codecs (charsets_gen.py writes
// charset_tables.inc), plus a detector that ranks plausible encodings of
// unknown text with a confidence.
//
// This is the one copy of these tables in the core: the encoding family's
// `txt` reader and writer, subtitles, HTML and text documents, and the
// office family's help files and e-books all decode through it.
//
//   util::decode_labelled(bytes, "gbk")        // any label, Unicode ones included
//   util::decode_auto(bytes)                   // BOM, UTF-8, else the best guess
//   util::charset_for_code_page(932)           // Windows code page numbers
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::util {

struct CharsetInfo {
  std::string_view id;       ///< canonical label, e.g. "shift_jis", "windows-1251"
  std::string_view name;     ///< "Japanese (Shift_JIS)"
  std::string_view aliases;  ///< space-separated other labels, matched ignoring case and punctuation
};

/// The charsets of this module, in the order a UI should offer them.
[[nodiscard]] std::span<const CharsetInfo> legacy_charsets() noexcept;
/// By id or alias ("cp1251", "Latin-2", "sjis", "x-mac-roman" ...); nullptr when unknown.
[[nodiscard]] const CharsetInfo* find_charset(std::string_view label) noexcept;
/// By Windows code page number (932, 936, 949, 950, 874, 1250..1258, 437,
/// 850, 852, 866, 10000, 20866, 21866, 28591..28605, 51932, 51949, 54936,
/// 50220 ...); nullptr when the number is unknown or is a Unicode one.
[[nodiscard]] const CharsetInfo* charset_for_code_page(int code_page) noexcept;

struct CharsetText {
  std::string text;          ///< valid UTF-8
  std::size_t replaced = 0;  ///< undecodable sequences, replaced with U+FFFD
  std::string charset{};     ///< the encoding that was used (decode_labelled, decode_auto)
};
[[nodiscard]] CharsetText charset_decode(ByteView data, const CharsetInfo& charset);

struct CharsetBytes {
  Bytes bytes;
  std::size_t replaced = 0;  ///< characters the charset cannot represent, written as '?'
};
[[nodiscard]] CharsetBytes charset_encode(std::string_view utf8, const CharsetInfo& charset);

struct CharsetGuess {
  std::string id;      ///< "utf-8", "utf-16le", ... or a legacy charset id
  int confidence = 0;  ///< 1..100
};
/// Plausible encodings of `data`, most likely first (at most 8).
[[nodiscard]] std::vector<CharsetGuess> detect_charsets(ByteView data);

/// True for the multi-byte East Asian charsets (Shift_JIS, GBK, Big5, ...).
[[nodiscard]] bool is_multibyte_charset(std::string_view id) noexcept;

/// Decode with any label: the Unicode encodings and Latin-1/Windows-1252 of
/// util/text.hpp, "us-ascii" (read as Windows-1252, as browsers do), every
/// legacy charset and its aliases, and "cp<number>" / "windows-<number>".
/// A byte order mark matching the label is dropped. nullopt for an unknown
/// label.
[[nodiscard]] std::optional<CharsetText> decode_labelled(ByteView data, std::string_view label);

/// Decode text of unknown encoding: a byte order mark decides; valid UTF-8
/// stays UTF-8; otherwise the detector's best guess is used when it is
/// confident (East Asian text decodes cleanly in exactly one multi-byte
/// charset), else `fallback` (a label for decode_labelled).
[[nodiscard]] CharsetText decode_auto(ByteView data, std::string_view fallback = "windows-1252");

}  // namespace cvt::util
