// Convertible core - pdf family: built-in font data.
//
// The standard single-byte encodings of PDF (Annex D), glyph-name to
// Unicode mapping (Adobe glyph naming conventions: AGLFN names, uniXXXX,
// uXXXX[XX], afii*, ligature names joined with '_', suffixes after '.'),
// and the widths of the 14 standard fonts for files that use them without
// /Widths. All tables are compiled in; lookups are pure functions.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cvt::pdf::font {

enum class BaseEncoding : std::uint8_t {
  Standard,       ///< StandardEncoding (Type 1 default)
  WinAnsi,        ///< WinAnsiEncoding (CP1252-like, PDF Annex D rules)
  MacRoman,       ///< MacRomanEncoding (PDF's variant)
  MacExpert,      ///< MacExpertEncoding
  PdfDoc,         ///< PDFDocEncoding (text strings)
  Symbol,         ///< built-in encoding of the Symbol font
  ZapfDingbats,   ///< built-in encoding of ZapfDingbats
};

/// Glyph name of `code` in `encoding`; empty view when the slot is unused.
[[nodiscard]] std::string_view encoding_glyph_name(BaseEncoding encoding, std::uint8_t code) noexcept;

/// Unicode text for a glyph name: "A" -> "A", "fi" -> "fi" (U+FB01 is NOT
/// used: ligatures decompose to their letters), "uni00410042" -> "AB",
/// "u1F600" -> U+1F600, "a_b" -> "ab", "A.sc" -> "A", "afii10017" -> U+0410,
/// ZapfDingbats "a1"... names -> dingbats. nullopt when unknown (names such
/// as "g123", "cid00012", "glyph5" carry no meaning).
[[nodiscard]] std::optional<std::u32string> glyph_name_to_unicode(std::string_view name);

/// Single code point when the name maps to exactly one (used for cmap
/// lookups in TrueType fonts); nullopt otherwise.
[[nodiscard]] std::optional<char32_t> glyph_name_to_codepoint(std::string_view name);

/// Reverse: a glyph name for a code point ("A", "eacute", else "uniXXXX" /
/// "uXXXXX"). Used by the writer.
[[nodiscard]] std::string codepoint_to_glyph_name(char32_t cp);

/// Decode a PDFDocEncoding byte to Unicode (0 when undefined).
[[nodiscard]] char32_t pdfdoc_to_unicode(std::uint8_t byte) noexcept;

enum class Standard14 : std::uint8_t {
  Helvetica, HelveticaBold, HelveticaOblique, HelveticaBoldOblique,
  TimesRoman, TimesBold, TimesItalic, TimesBoldItalic,
  Courier, CourierBold, CourierOblique, CourierBoldOblique,
  Symbol, ZapfDingbats,
};

/// Recognise a standard-14 font by its BaseFont name, including the common
/// aliases (Arial -> Helvetica, TimesNewRoman -> Times-Roman, CourierNew ->
/// Courier, ",Bold"/"-BoldMT" suffixes, subset prefixes "ABCDEF+").
[[nodiscard]] std::optional<Standard14> standard14_from_name(std::string_view base_font) noexcept;

/// Advance width in 1/1000 em of a glyph of a standard-14 font, by glyph
/// name (AFM metrics); nullopt when the font has no such glyph.
[[nodiscard]] std::optional<double> standard14_width(Standard14 font, std::string_view glyph_name) noexcept;

/// Font descriptor-like facts of a standard-14 font.
struct Standard14Info {
  double ascent = 0;
  double descent = 0;
  double cap_height = 0;
  bool serif = false;
  bool fixed_pitch = false;
  bool bold = false;
  bool italic = false;
  bool symbolic = false;
};
[[nodiscard]] Standard14Info standard14_info(Standard14 font) noexcept;

}  // namespace cvt::pdf::font
