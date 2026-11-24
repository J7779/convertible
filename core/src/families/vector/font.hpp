// Convertible core - vector family: TrueType outlines and simple text layout.
//
// An in-house reader for TrueType ('glyf') and CFF-less OpenType fonts:
// cmap (formats 4, 6, 12), hmtx, kern (format 0) and glyph outlines
// (simple and composite). Used for SVG text, metafile/CAD/plotter labels
// and PostScript Type 42 fonts. The bundled fallback faces (Inter for
// sans-serif and serif requests, JetBrains Mono for monospace; SIL Open
// Font Licence) are compiled into the core by family.cmake.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "util/bytes.hpp"

namespace cvt::vector {

class TrueTypeFont {
 public:
  /// Parse a TrueType/OpenType (glyf) font, or one face of a collection
  /// (`index`). The bytes must outlive the font object (it keeps views).
  [[nodiscard]] static Result<TrueTypeFont> parse(ByteView bytes, std::uint32_t index = 0);

  [[nodiscard]] std::uint16_t units_per_em() const noexcept { return units_per_em_; }
  [[nodiscard]] std::uint16_t glyph_count() const noexcept { return glyph_count_; }
  /// Glyph id of a Unicode code point (0 = .notdef / missing).
  [[nodiscard]] std::uint16_t glyph_index(char32_t code_point) const noexcept;
  /// Advance width in font units.
  [[nodiscard]] std::uint16_t advance(std::uint16_t glyph) const noexcept;
  /// Pair kerning in font units ('kern' format 0), 0 when none.
  [[nodiscard]] std::int16_t kerning(std::uint16_t left, std::uint16_t right) const noexcept;
  /// Outline in font units, y UP (as stored); quadratic segments. Empty for
  /// blanks or malformed glyphs.
  [[nodiscard]] Path outline(std::uint16_t glyph) const;
  /// Typographic ascender / descender (font units, descender negative).
  [[nodiscard]] std::int16_t ascender() const noexcept { return ascender_; }
  [[nodiscard]] std::int16_t descender() const noexcept { return descender_; }

 private:
  Path outline_at(std::uint16_t glyph, int depth) const;

  ByteView data_;
  ByteView cmap_, glyf_, loca_, hmtx_, kern_;
  std::uint16_t units_per_em_ = 1000;
  std::uint16_t glyph_count_ = 0;
  std::uint16_t metric_count_ = 0;
  bool long_loca_ = false;
  std::int16_t ascender_ = 800;
  std::int16_t descender_ = -200;
  std::uint32_t cmap_subtable_ = 0;  // offset of the chosen subtable inside cmap
  std::uint16_t cmap_format_ = 0;
};

/// The bundled fallback faces.
enum class FallbackFace : std::uint8_t { Sans, SansBold, Mono };
/// The parsed bundled face (parsed on each call; cheap: no copy of the data).
[[nodiscard]] Result<TrueTypeFont> fallback_font(FallbackFace face);
/// Pick a bundled face for a CSS-like family list and weight
/// ("Courier New, monospace" -> Mono; weight >= 600 -> SansBold).
[[nodiscard]] FallbackFace choose_fallback(std::string_view family, int weight);

struct TextLayout {
  /// Glyph outlines with the baseline start at (0, 0), y DOWN (scene
  /// convention), scaled to `font.size` user units. Italic requests are
  /// slanted synthetically.
  Path outline;
  /// Total advance in user units.
  double advance = 0.0;
  /// Pen x position of every code point (same count as code points), for
  /// per-character positioning.
  std::vector<double> positions;
  /// Ascent and descent (positive) at this size.
  double ascent = 0.0;
  double descent = 0.0;
};

/// Lay out one line of UTF-8 text with the bundled fallback fonts.
/// `letter_spacing` and `word_spacing` are in user units.
[[nodiscard]] TextLayout layout_text(std::string_view utf8, const FontSpec& font, double letter_spacing = 0.0,
                                     double word_spacing = 0.0);

/// Same with a specific parsed font (e.g. an embedded TrueType font).
[[nodiscard]] TextLayout layout_text(const TrueTypeFont& face, std::string_view utf8, double size, bool italic = false,
                                     double letter_spacing = 0.0, double word_spacing = 0.0);

/// A complete Text node for `utf8` at baseline origin `origin` with the
/// bundled fonts: outline already placed at the origin.
[[nodiscard]] Text make_text(std::string_view utf8, const FontSpec& font, Point origin, Paint fill);

}  // namespace cvt::vector
