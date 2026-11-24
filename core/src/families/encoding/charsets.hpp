// Convertible core - encoding family: legacy character sets (charsets.cpp).
//
// Single-byte code pages (ISO-8859-1..16, Windows-1250..1258, KOI8-R/U, Mac
// Roman, IBM 437/850/852/866, EBCDIC 037/500/1047) and the East Asian
// multi-byte encodings (Shift_JIS, Windows-31J/cp932, EUC-JP, ISO-2022-JP,
// GBK, GB18030, Big5, EUC-KR), decoded to and encoded from UTF-8 with
// tables taken from Python's codecs (charsets_gen.py), plus a detector that
// ranks plausible encodings of unknown text with a confidence.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::encoding {

struct CharsetInfo {
  std::string_view id;       ///< canonical label, e.g. "shift_jis", "windows-1251"
  std::string_view name;     ///< "Japanese (Shift_JIS)"
  std::string_view aliases;  ///< space-separated other labels, matched ignoring case and punctuation
};

/// The charsets of this module, in the order a UI should offer them.
[[nodiscard]] std::span<const CharsetInfo> legacy_charsets() noexcept;
/// By id or alias ("cp1251", "Latin-2", "sjis", "x-mac-roman" ...); nullptr when unknown.
[[nodiscard]] const CharsetInfo* find_charset(std::string_view label) noexcept;

struct CharsetText {
  std::string text;          ///< valid UTF-8
  std::size_t replaced = 0;  ///< undecodable sequences, replaced with U+FFFD
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

}  // namespace cvt::encoding
