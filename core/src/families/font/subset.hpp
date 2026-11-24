// Convertible core - font family: subsetting and optimisation.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/font.hpp"

namespace cvt::font {

/// Parse a code point list such as "U+0020-007E, U+00C0, 41-5A" ("*" = all).
/// nullopt means "all"; malformed text is InvalidArgument.
[[nodiscard]] Result<std::optional<std::set<std::uint32_t>>> parse_unicodes(std::string_view text);

struct SubsetOptions {
  std::optional<std::set<std::uint32_t>> unicodes;  ///< nullopt = keep every glyph
  bool drop_hinting = false;
  bool drop_layout = false;
  bool drop_names = false;
  bool drop_glyph_names = false;
  bool layout_closure = true;  ///< follow GSUB substitutions from kept glyphs
};

/// Glyphs reachable from `glyphs` through GSUB lookups of types 1-4, 7, 8.
[[nodiscard]] std::set<std::uint16_t> gsub_closure(const ir::Font& font, std::set<std::uint16_t> glyphs);

/// Subset and optimise one face in place. Glyph ids are kept (removed glyphs
/// become empty) while layout tables are kept; otherwise glyphs are
/// renumbered compactly. Variation tables are kept consistent.
[[nodiscard]] Result<void> subset_font(ir::Font& font, const SubsetOptions& options, const Context& ctx);

/// Remove editor/host-specific tables (FontForge timestamps, VTT sources...).
/// Never touches name records: copyright, trademark and licence notices are
/// legal information, not personal metadata.
void strip_editor_tables(ir::Font& font);

/// Re-pack glyf with compact glyph encodings and the short loca format when
/// it fits (the "compress" path for TrueType fonts).
[[nodiscard]] Result<void> compact_glyf(ir::Font& font);

}  // namespace cvt::font
