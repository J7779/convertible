// Convertible core - font family: outline extraction, TrueType <-> CFF conversion
// and building complete fonts from outlines.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/font/cff.hpp"
#include "families/font/outline.hpp"
#include "families/font/tables.hpp"
#include "ir/font.hpp"

namespace cvt::font {

/// Tables that only make sense for TrueType hinting or outlines.
[[nodiscard]] bool is_truetype_only_table(std::string_view tag) noexcept;
/// Tables of variable fonts.
[[nodiscard]] bool is_variation_table(std::string_view tag) noexcept;
[[nodiscard]] bool is_variable(const ir::Font& font) noexcept;
/// True when the font has glyph outlines (non-empty glyf, CFF or CFF2).
[[nodiscard]] bool has_outline_data(const ir::Font& font) noexcept;

/// Glyph outlines and advances of a font, whatever its flavour.
struct Outlines {
  std::vector<Path> paths;       ///< font units, y up, source winding
  std::vector<double> advances;  ///< advance widths
  double units_per_em = 1000;
  bool cubic = false;            ///< source curves were cubic (CFF)
};
[[nodiscard]] Result<Outlines> extract_outlines(const ir::Font& font, const Context& ctx);
/// Outline of one glyph (TrueType or CFF), for rendering.
class GlyphSource {
 public:
  static Result<GlyphSource> open(const ir::Font& font);
  [[nodiscard]] Result<Path> outline(std::uint32_t glyph) const;
  [[nodiscard]] double advance(std::uint32_t glyph) const;
  [[nodiscard]] std::uint32_t glyph_count() const noexcept { return count_; }
  [[nodiscard]] double units_per_em() const noexcept { return upm_; }
  [[nodiscard]] const CharMap& cmap() const noexcept { return cmap_; }

 private:
  std::vector<ByteView> slices_;
  std::optional<CffFont> cff_;
  std::vector<HorizontalMetric> metrics_;
  CharMap cmap_;
  std::uint32_t count_ = 0;
  double upm_ = 1000;
};

/// Re-encode the outlines as TrueType (quadratic) glyf/loca, approximating
/// cubic curves within `tolerance` font units. Layout tables are kept: glyph
/// ids do not change. Variable CFF2 fonts are instantiated at their default.
[[nodiscard]] Result<ir::Font> to_truetype(const ir::Font& font, double tolerance, const Context& ctx);
/// Re-encode the outlines as CFF (cubic), exactly up to coordinate rounding
/// (`round_coordinates`) . Variable TrueType fonts are instantiated at their
/// default (the glyf outlines); their variation tables are dropped.
[[nodiscard]] Result<ir::Font> to_cff(const ir::Font& font, bool round_coordinates, const Context& ctx);

/// Recompute maxp, head bounds, hhea extents and hmtx side bearings of a
/// TrueType font after its glyf table changed.
[[nodiscard]] Result<void> refresh_truetype_metrics(ir::Font& font);

/// Recompute head bounds, hhea extents and hmtx side bearings of a CFF
/// font from its charstrings.
[[nodiscard]] Result<void> refresh_cff_metrics(ir::Font& font);

/// A complete font described by outlines and metadata.
struct FontSpec {
  std::string family = "Untitled";
  std::string subfamily = "Regular";
  std::string full_name;        ///< default: family + " " + subfamily
  std::string postscript_name;  ///< default: derived from family/subfamily
  std::string version = "Version 1.000";
  std::string copyright, trademark, license, license_url, notice;
  std::uint16_t units_per_em = 1000;
  std::int16_t ascender = 800;
  std::int16_t descender = -200;
  std::int16_t line_gap = 0;
  double italic_angle = 0;
  std::int16_t underline_position = -100;
  std::int16_t underline_thickness = 50;
  bool fixed_pitch = false;
  std::uint16_t weight_class = 400;
  bool bold = false;
  bool italic = false;
  std::vector<std::string> glyph_names;  ///< [0] = ".notdef"
  std::vector<Path> paths;
  std::vector<double> advances;
  CharMap cmap;
  /// Kerning pairs (left glyph, right glyph) -> value, written as kern format 0.
  std::map<std::pair<std::uint16_t, std::uint16_t>, std::int16_t> kerning;
  CffDict private_hints;
  /// True when outer contours run counter-clockwise (PostScript/CFF
  /// convention); TrueType output reverses them, and vice versa.
  bool postscript_winding = true;
};
/// Build every required table. `cff` selects CFF outlines (else glyf).
[[nodiscard]] Result<ir::Font> build_font(const FontSpec& spec, bool cff, const Context& ctx);

}  // namespace cvt::font
