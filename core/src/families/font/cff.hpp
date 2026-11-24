// Convertible core - font family: CFF and CFF2 (Type 2 charstrings).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine/errors.hpp"
#include "families/font/outline.hpp"
#include "util/bytes.hpp"

namespace cvt::font {

/// DICT operator keys: one-byte operators as is, escaped ones as 1200 + b.
using CffDict = std::map<int, std::vector<double>>;

struct CffPrivate {
  CffDict dict;
  std::vector<ByteView> subrs;
  double default_width = 0;
  double nominal_width = 0;
};

/// A parsed CFF or CFF2 font (views into the table bytes).
struct CffFont {
  bool cff2 = false;
  std::string name;
  CffDict top;
  std::vector<std::string> strings;  ///< custom strings (SID 391 + i)
  std::vector<ByteView> charstrings;
  std::vector<ByteView> global_subrs;
  std::vector<CffPrivate> privates;   ///< one, or one per FD for CID-keyed / CFF2 fonts
  std::vector<CffDict> fd_dicts;      ///< Font DICTs of the FDArray (empty for name-keyed fonts)
  std::vector<std::uint8_t> fd_select;  ///< glyph -> private index (empty: all 0)
  std::vector<std::uint16_t> charset;   ///< SID (or CID) per glyph; empty for CFF2
  bool cid_keyed = false;
  /// Region counts of the CFF2 variation store, per ItemVariationData.
  std::vector<std::uint16_t> region_counts;
  /// Units per em implied by FontMatrix (1000 for the default matrix).
  double units_per_em = 1000;
  std::vector<double> font_matrix;

  [[nodiscard]] std::string string_for(int sid) const;
  [[nodiscard]] std::vector<double> top_value(int op, std::vector<double> fallback = {}) const;
};

[[nodiscard]] Result<CffFont> parse_cff(ByteView data);

/// Outline and width of one glyph (CFF2 blends are evaluated at the default
/// instance, i.e. ignored). `width` is the advance from the charstring, or
/// nullopt when the charstring does not give one (CFF2) .
struct CffGlyph {
  Path path;
  std::optional<double> width;
};
[[nodiscard]] Result<CffGlyph> cff_glyph(const CffFont& font, std::uint32_t glyph);

/// Glyph names from a CFF table (empty for CFF2 or on errors).
[[nodiscard]] std::vector<std::string> cff_glyph_names(ByteView cff, std::uint32_t count);

/// Everything needed to write a name-keyed CFF table.
struct CffBuildInput {
  std::string postscript_name = "Untitled";
  std::string full_name, family_name, weight, notice, copyright, version;
  std::vector<std::string> glyph_names;  ///< [0] must be ".notdef"
  std::vector<Path> outlines;            ///< any curves; converted to cubic
  std::vector<double> advances;
  double units_per_em = 1000;
  double italic_angle = 0;
  double underline_position = -100;
  double underline_thickness = 50;
  bool fixed_pitch = false;
  /// Optional private DICT entries copied from a source (BlueValues, StdHW...).
  CffDict private_hints;
  /// Round coordinates to whole units (smaller) instead of 16.16 fixed.
  bool round_coordinates = true;
};
[[nodiscard]] Result<Bytes> build_cff(const CffBuildInput& input);

/// Re-serialise a CFF (version 1) font with new CharStrings: `glyphs` lists
/// the source glyph id of every output glyph (subsetting / renumbering);
/// `replacement` optionally gives new charstring bytes per output glyph
/// (empty entry = copy the source charstring). With `keep_subrs` false the
/// local and global subroutines are dropped (only valid when every
/// charstring was replaced by one that calls none).
[[nodiscard]] Result<Bytes> rebuild_cff(const CffFont& font, const std::vector<std::uint32_t>& glyphs,
                                        const std::vector<Bytes>& replacement, bool keep_subrs);
/// A charstring for an outline (hint-free; `width` relative to nominal).
[[nodiscard]] Bytes cff_charstring(const Path& path, std::optional<double> width, bool round_coordinates);

/// Encode a DICT/charstring number (used by the Type 1 writer too).
void cff_encode_dict_int(Bytes& out, std::int32_t value);

}  // namespace cvt::font
