// Convertible core - font family: parsers and builders of the common sfnt tables.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "ir/font.hpp"
#include "util/bytes.hpp"

namespace cvt::font {

// --------------------------------------------------------------------------- name

struct NameRecord {
  std::uint16_t platform = 3;
  std::uint16_t encoding = 1;
  std::uint16_t language = 0x0409;
  std::uint16_t name_id = 0;
  /// The string as stored (UTF-16BE for Unicode/Windows records).
  Bytes raw;
  /// Decoded text (UTF-8); empty when the encoding is unknown.
  std::string text;
};

/// Name IDs that carry legal notices and must survive every clean-up:
/// copyright (0), trademark (7), licence description (13), licence URL (14).
[[nodiscard]] bool is_legal_name_id(std::uint16_t id) noexcept;

[[nodiscard]] Result<std::vector<NameRecord>> parse_name(ByteView table);
/// A name table (format 0) from records; records are sorted as required.
[[nodiscard]] Bytes build_name(std::vector<NameRecord> records);
/// Best English text for a name ID (Windows Unicode first, then Mac Roman).
[[nodiscard]] std::string best_name(std::span<const NameRecord> records, std::uint16_t name_id);
/// A Windows Unicode record for `text`.
[[nodiscard]] NameRecord make_name(std::uint16_t name_id, std::string_view text);

// --------------------------------------------------------------------------- cmap

/// Unicode code point -> glyph id.
using CharMap = std::map<std::uint32_t, std::uint16_t>;

/// Parse the best Unicode subtable(s) of a cmap (formats 0, 4, 6, 10, 12, 13;
/// symbol fonts are mapped from U+F0xx to their 8-bit codes as well).
/// Glyph ids >= `num_glyphs` are dropped.
[[nodiscard]] Result<CharMap> parse_cmap(ByteView table, std::uint32_t num_glyphs);
/// Variation sequences (format 14): {selector, base, glyph} with glyph =
/// 0xFFFF... for "default" mappings. Only used for reporting.
struct VariationSequence {
  std::uint32_t selector = 0;
  std::uint32_t base = 0;
  std::optional<std::uint16_t> glyph;
};
[[nodiscard]] std::vector<VariationSequence> parse_cmap14(ByteView table);
/// A cmap with format 4 (platforms 0/3 and 3/1) and, when needed, format 12
/// (0/4 and 3/10) subtables.
[[nodiscard]] Bytes build_cmap(const CharMap& map);

// --------------------------------------------------------------------------- metrics

struct HorizontalMetric {
  std::uint16_t advance = 0;
  std::int16_t lsb = 0;
};
/// Read hmtx for `num_glyphs` glyphs (missing data repeats the last advance).
[[nodiscard]] Result<std::vector<HorizontalMetric>> parse_hmtx(const ir::Font& font);
/// Build hmtx with trailing equal advances folded; returns numberOfHMetrics.
[[nodiscard]] Bytes build_hmtx(std::span<const HorizontalMetric> metrics, std::uint16_t& number_of_hmetrics);

[[nodiscard]] std::uint16_t num_glyphs(const ir::Font& font) noexcept;
[[nodiscard]] std::uint16_t units_per_em(const ir::Font& font) noexcept;

// --------------------------------------------------------------------------- glyph names

/// The 258 standard Macintosh glyph names (post format 1 / 2).
[[nodiscard]] std::span<const std::string_view> mac_glyph_names() noexcept;
/// The 391 CFF standard strings.
[[nodiscard]] std::span<const std::string_view> cff_standard_strings() noexcept;
/// Adobe StandardEncoding: code -> glyph name ("" when unassigned).
[[nodiscard]] std::string_view standard_encoding(std::uint8_t code) noexcept;
/// Unicode value of an AGL-style glyph name ("A", "Agrave", "uni00C0",
/// "u1F600", "Euro" ...), or nullopt.
[[nodiscard]] std::optional<std::uint32_t> unicode_from_glyph_name(std::string_view name);
/// A conventional glyph name for a code point ("A", "uni20AC", "u1F600").
[[nodiscard]] std::string glyph_name_for_unicode(std::uint32_t codepoint);

/// Glyph names from post (format 1 / 2) or the CFF charset; empty strings
/// where the font has none.
[[nodiscard]] std::vector<std::string> post_glyph_names(ByteView post, std::uint32_t num_glyphs);
/// A post table: format 2 with `names` or format 3 when `names` is empty.
/// The 32-byte header is taken from `source` when it is a post table.
[[nodiscard]] Bytes build_post(ByteView source, std::span<const std::string> names);

/// Names for every glyph: post/CFF names, else from the cmap, else "glyphN".
/// Unique and valid as PostScript names.
[[nodiscard]] std::vector<std::string> glyph_names(const ir::Font& font);

// --------------------------------------------------------------------------- misc

/// A minimal but valid OS/2 table (version 4) from basic metrics.
struct Os2Basics {
  std::int16_t average_width = 500;
  std::uint16_t weight_class = 400;
  std::int16_t ascender = 800;
  std::int16_t descender = -200;
  std::int16_t line_gap = 0;
  std::uint16_t win_ascent = 800;
  std::uint16_t win_descent = 200;
  std::int16_t x_height = 0;
  std::int16_t cap_height = 0;
  std::uint16_t first_char = 0x20;
  std::uint16_t last_char = 0x7E;
  bool italic = false;
  bool bold = false;
  std::uint32_t unicode_range1 = 1;  // Basic Latin
  std::uint32_t code_page1 = 1;      // Latin 1
};
[[nodiscard]] Bytes build_os2(const Os2Basics& basics);

/// A head table (version 1.0).
struct HeadBasics {
  std::uint16_t units_per_em = 1000;
  std::int16_t x_min = 0, y_min = 0, x_max = 0, y_max = 0;
  std::uint16_t mac_style = 0;
  std::int16_t index_to_loc_format = 0;
  std::uint32_t revision = 0x00010000;
};
[[nodiscard]] Bytes build_head(const HeadBasics& basics);
/// An hhea table.
struct HheaBasics {
  std::int16_t ascender = 800, descender = -200, line_gap = 0;
  std::uint16_t advance_width_max = 0;
  std::int16_t min_lsb = 0, min_rsb = 0, x_max_extent = 0;
  std::uint16_t number_of_hmetrics = 0;
};
[[nodiscard]] Bytes build_hhea(const HheaBasics& basics);

}  // namespace cvt::font
