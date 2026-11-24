// Convertible core - encoding family: everyday text tools (text_tools.cpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "engine/errors.hpp"

namespace cvt::encoding {

/// `style`: lower, upper, title, sentence, camel, pascal, snake, kebab,
/// constant or swap. Letter case follows the simple mappings of the Latin,
/// Greek, Cyrillic, Armenian and full-width Latin alphabets; the identifier
/// styles convert every line separately.
[[nodiscard]] std::string text_case(std::string_view text, std::string_view style);

/// Lower-case ASCII letters and digits joined by `separator`; Latin letters
/// with diacritics are transliterated ("Crème brûlée" -> "creme-brulee").
/// `max_length` 0 = unlimited.
[[nodiscard]] std::string slugify(std::string_view text, char separator = '-', std::size_t max_length = 0);

struct LineOptions {
  std::string sort = "none";  ///< none, asc, desc, natural, length
  bool unique = false;
  bool reverse = false;
  bool trim = false;
  bool remove_empty = false;
  bool ignore_case = false;
};
[[nodiscard]] std::string transform_lines(std::string_view text, const LineOptions& options);

struct WhitespaceOptions {
  std::string tabs = "keep";  ///< keep, expand, unexpand (indentation only)
  std::size_t tab_width = 4;
  bool trim_trailing = false;
  std::string final_newline = "keep";  ///< keep, add, remove
};
[[nodiscard]] std::string transform_whitespace(std::string_view text, const WhitespaceOptions& options);

// Classic ciphers: puzzles, not encryption.
[[nodiscard]] std::string caesar(std::string_view text, int shift);
[[nodiscard]] Result<std::string> vigenere(std::string_view text, std::string_view key, bool decrypt);
[[nodiscard]] std::string atbash(std::string_view text);
[[nodiscard]] std::string a1z26_encode(std::string_view text);
[[nodiscard]] Result<std::string> a1z26_decode(std::string_view text);

/// Grade 1 English braille as Unicode braille patterns (U+2800..U+283F).
[[nodiscard]] std::string braille_encode(std::string_view text, std::size_t* skipped = nullptr);
[[nodiscard]] Result<std::string> braille_decode(std::string_view text);

}  // namespace cvt::encoding
