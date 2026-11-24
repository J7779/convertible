// Convertible core - encoding family: Unicode normalisation, code point
// views and IDNA (unicode.cpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <string_view>

#include "engine/errors.hpp"

namespace cvt::encoding {

enum class NormalForm { Nfc, Nfd, Nfkc, Nfkd };
/// Unicode Normalization Forms (UAX #15) of UTF-8 text, with the character
/// data of `unicode_data_version()`. Invalid UTF-8 is repaired first.
[[nodiscard]] std::string normalize(std::string_view text, NormalForm form);
[[nodiscard]] std::string_view unicode_data_version() noexcept;

/// "U+0048 U+0069"; `style`: u-plus, hex, decimal, html (&#x48;), css (\48 ), js (\u{48}).
[[nodiscard]] std::string code_points(std::string_view text, std::string_view style);
/// Reads any of those notations (and "0x48", "U+1F600", "\\u{48}") back to text.
[[nodiscard]] Result<std::string> parse_code_points(std::string_view text);

/// One line per character: "U+20AC  €  E2 82 AC".
[[nodiscard]] std::string utf8_byte_table(std::string_view text);

/// UTS #46 basics: width/case folding through NFKC and simple lower-casing,
/// the full stop variants as label separators, then Punycode per label; the
/// result is checked against the STD3 ASCII rules and DNS lengths.
[[nodiscard]] Result<std::string> idna_to_ascii(std::string_view domain);
[[nodiscard]] Result<std::string> idna_to_unicode(std::string_view domain);

}  // namespace cvt::encoding
