// Convertible desktop - TrueType font parsing (glyf outlines, cmap, hmtx,
// kerning from GPOS pair adjustment or the legacy kern table, and GSUB single
// substitutions for features such as tabular figures).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gfx/path.hpp"

namespace cvd::text {

class Font {
 public:
  /// Parses the font; `data` must outlive the Font. Returns false when the
  /// file is not a usable TrueType (glyf) font.
  bool load(std::span<const std::uint8_t> data);

  [[nodiscard]] bool valid() const { return valid_; }
  [[nodiscard]] int units_per_em() const { return upem_; }
  [[nodiscard]] int ascender() const { return ascender_; }    // font units, positive
  [[nodiscard]] int descender() const { return descender_; }  // font units, negative
  [[nodiscard]] int line_gap() const { return line_gap_; }
  [[nodiscard]] int num_glyphs() const { return num_glyphs_; }

  [[nodiscard]] std::uint16_t glyph_index(std::uint32_t codepoint) const;
  [[nodiscard]] int advance(std::uint16_t glyph) const;
  [[nodiscard]] int left_side_bearing(std::uint16_t glyph) const;
  /// Horizontal kerning between two glyphs in font units (0 when none).
  [[nodiscard]] int kerning(std::uint16_t left, std::uint16_t right) const;
  /// Applies a GSUB single substitution feature ("tnum", "case", ...); returns the glyph unchanged when none applies.
  [[nodiscard]] std::uint16_t substitute(std::uint16_t glyph, std::string_view feature) const;
  /// Glyph outline in font units, y up. Empty for blank glyphs.
  [[nodiscard]] gfx::Path outline(std::uint16_t glyph) const;

 private:
  struct Table {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
  };
  [[nodiscard]] Table find_table(const char* tag) const;
  [[nodiscard]] std::uint16_t u16(std::size_t off) const;
  [[nodiscard]] std::int16_t s16(std::size_t off) const;
  [[nodiscard]] std::uint32_t u32(std::size_t off) const;
  [[nodiscard]] bool in_range(std::size_t off, std::size_t len) const;

  [[nodiscard]] std::uint16_t cmap_lookup(std::uint32_t cp) const;
  [[nodiscard]] bool glyph_range(std::uint16_t glyph, std::uint32_t& start, std::uint32_t& end) const;
  void append_outline(gfx::Path& path, std::uint16_t glyph, const float m[6], int depth) const;

  [[nodiscard]] int coverage_index(std::size_t coverage_off, std::uint16_t glyph) const;
  [[nodiscard]] int class_of(std::size_t classdef_off, std::uint16_t glyph) const;
  [[nodiscard]] std::vector<std::size_t> feature_lookup_tables(std::size_t table_off, std::string_view feature,
                                                               std::uint16_t want_type, std::uint16_t ext_type) const;
  [[nodiscard]] bool gpos_pair(std::size_t subtable, std::uint16_t left, std::uint16_t right, int& value) const;
  [[nodiscard]] int legacy_kern(std::uint16_t left, std::uint16_t right) const;

  std::span<const std::uint8_t> data_;
  bool valid_ = false;
  int upem_ = 1000;
  int ascender_ = 0;
  int descender_ = 0;
  int line_gap_ = 0;
  int num_glyphs_ = 0;
  int num_hmetrics_ = 0;
  bool long_loca_ = false;
  Table glyf_, loca_, hmtx_, kern_, gpos_, gsub_;
  std::size_t cmap_subtable_ = 0;
  std::uint16_t cmap_format_ = 0;
  std::vector<std::size_t> gpos_kern_subtables_;

  mutable std::mutex cache_mutex_;
  mutable std::unordered_map<std::uint32_t, int> kern_cache_;
  mutable std::unordered_map<std::uint64_t, std::uint16_t> subst_cache_;
};

}  // namespace cvd::text
