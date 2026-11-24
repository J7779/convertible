// Convertible core - pdf family: tokenizer and object parser.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "families/pdf/object.hpp"
#include "util/bytes.hpp"

namespace cvt::pdf {

[[nodiscard]] constexpr bool is_pdf_space(std::uint8_t c) noexcept {
  return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
[[nodiscard]] constexpr bool is_pdf_delimiter(std::uint8_t c) noexcept {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
         c == '%';
}
[[nodiscard]] constexpr bool is_pdf_regular(std::uint8_t c) noexcept { return !is_pdf_space(c) && !is_pdf_delimiter(c); }

struct Token {
  enum class Kind : std::uint8_t {
    End,
    Integer,
    Real,
    String,     ///< text holds the decoded bytes
    HexString,  ///< text holds the decoded bytes
    Name,       ///< text holds the decoded name
    Keyword,    ///< text holds the keyword (true, false, null, R, obj, stream, operators...)
    ArrayOpen,
    ArrayClose,
    DictOpen,
    DictClose,
    BraceOpen,
    BraceClose,
    Error,
  };
  Kind kind = Kind::End;
  std::int64_t integer = 0;
  double real = 0.0;
  std::string text;
  std::size_t position = 0;  ///< offset of the token start
};

/// Tokenizer over a byte range. Never reads out of bounds; malformed input
/// yields best-effort tokens (PDF readers are expected to be forgiving).
class Lexer {
 public:
  explicit Lexer(ByteView data, std::size_t position = 0) noexcept : data_(data), pos_(position) {}

  [[nodiscard]] Token next();
  [[nodiscard]] Token peek();
  void skip_whitespace() noexcept;
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  void seek(std::size_t position) noexcept { pos_ = position < data_.size() ? position : data_.size(); }
  [[nodiscard]] ByteView data() const noexcept { return data_; }
  [[nodiscard]] bool at_end() const noexcept { return pos_ >= data_.size(); }
  [[nodiscard]] std::uint8_t byte_at(std::size_t i) const noexcept {
    return i < data_.size() ? std::to_integer<std::uint8_t>(data_[i]) : 0;
  }

 private:
  ByteView data_;
  std::size_t pos_ = 0;
};

/// Parse one object at the lexer's position. `R` references are recognised
/// ("1 0 R"). Nesting deeper than `max_depth` and runaway collections are
/// cut short (the partial object is returned). Streams are NOT handled here
/// (see PdfFile); a dictionary followed by `stream` is returned as a dict.
/// On garbage the result is Null and `ok` (when given) is set to false.
[[nodiscard]] Object parse_object(Lexer& lexer, int max_depth = 64, bool* ok = nullptr);

/// Decode a PDF text string (UTF-16BE with BOM, UTF-8 with BOM, or
/// PDFDocEncoding) into UTF-8.
[[nodiscard]] std::string decode_text_string(std::string_view bytes);

/// Encode UTF-8 as a PDF text string: PDFDocEncoding-compatible ASCII when
/// possible, else UTF-16BE with BOM.
[[nodiscard]] std::string encode_text_string(std::string_view utf8);

}  // namespace cvt::pdf
