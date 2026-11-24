// Convertible core - pdf family: embedded font programs.
//
// Parsers for the outline font formats a PDF can embed - TrueType (sfnt with
// glyf), CFF / Type 1C / CID-keyed CFF / OpenType-CFF (with a Type 2
// charstring interpreter) and Type 1 (eexec, Type 1 charstrings) - plus a
// TrueType subsetter for the writer. All of them treat the font data as
// hostile: every offset is checked, recursion (composite glyphs, subroutine
// calls, seac) is bounded, and a broken glyph yields an empty outline rather
// than an error for the whole font.
//
// Outlines are returned in FONT UNITS; `font_matrix()` maps font units to
// text space (1 unit of text space = 1 em at font size 1), e.g. 1/2048 for a
// TrueType font with 2048 units per em, [0.001 0 0 0.001 0 0] for most
// Type 1 / CFF fonts.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "families/pdf/geom.hpp"
#include "util/bytes.hpp"

namespace cvt::pdf::font {

// --------------------------------------------------------------------------- TrueType / sfnt

class TrueType {
 public:
  /// Parse an sfnt ('\0\1\0\0', 'true', 'OTTO' is rejected here - use Cff,
  /// 'ttcf' picks `face_index`). The data is copied.
  [[nodiscard]] static Result<TrueType> parse(ByteView data, int face_index = 0);

  [[nodiscard]] std::uint16_t units_per_em() const noexcept { return units_per_em_; }
  [[nodiscard]] std::uint16_t num_glyphs() const noexcept { return num_glyphs_; }
  [[nodiscard]] Matrix font_matrix() const noexcept;

  /// True when the font has a cmap subtable (platform, encoding).
  [[nodiscard]] bool has_cmap(std::uint16_t platform, std::uint16_t encoding) const noexcept;
  /// Glyph for `code` in cmap subtable (platform, encoding), formats 0, 2, 4,
  /// 6, 10, 12, 13; nullopt when unmapped or the subtable is absent.
  [[nodiscard]] std::optional<std::uint16_t> glyph_for_cmap(std::uint16_t platform, std::uint16_t encoding,
                                                            std::uint32_t code) const;
  /// Glyph for a Unicode scalar via (3,10), (3,1), (0,*) subtables.
  [[nodiscard]] std::optional<std::uint16_t> glyph_for_unicode(char32_t cp) const;
  /// Glyph by PostScript name from the 'post' table (formats 1, 2).
  [[nodiscard]] std::optional<std::uint16_t> glyph_for_name(std::string_view name) const;
  /// Reverse of the Unicode cmap for every mapped glyph (first code point
  /// found per glyph); used for text extraction when nothing better exists.
  [[nodiscard]] std::map<std::uint16_t, char32_t> unicode_map() const;

  /// Outline in font units (composite glyphs resolved). Empty for blank or
  /// broken glyphs and out-of-range ids.
  [[nodiscard]] Path outline(std::uint16_t gid) const;
  /// Advance width in font units from 'hmtx' (0 when unknown).
  [[nodiscard]] double advance(std::uint16_t gid) const noexcept;
  /// Kerning adjustment (font units) from a format-0 'kern' table; 0 when none.
  [[nodiscard]] double kerning(std::uint16_t left, std::uint16_t right) const noexcept;

  /// Typographic metrics in font units (from OS/2, falling back to hhea/head).
  [[nodiscard]] double ascent() const noexcept { return ascent_; }
  [[nodiscard]] double descent() const noexcept { return descent_; }  ///< negative
  [[nodiscard]] double cap_height() const noexcept { return cap_height_; }
  [[nodiscard]] double line_gap() const noexcept { return line_gap_; }
  [[nodiscard]] Rect bbox() const noexcept { return bbox_; }
  [[nodiscard]] int weight_class() const noexcept { return weight_class_; }
  [[nodiscard]] bool italic() const noexcept { return italic_; }
  [[nodiscard]] double italic_angle() const noexcept { return italic_angle_; }
  [[nodiscard]] bool fixed_pitch() const noexcept { return fixed_pitch_; }
  /// PostScript name from the 'name' table (id 6), ASCII; may be empty.
  [[nodiscard]] const std::string& postscript_name() const noexcept { return ps_name_; }

  /// The raw sfnt bytes of the selected face.
  [[nodiscard]] ByteView data() const noexcept { return data_; }

 private:
  struct Table {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
  };
  struct CmapSub {
    std::uint16_t platform = 0;
    std::uint16_t encoding = 0;
    std::uint32_t offset = 0;  // absolute within data_
  };
  [[nodiscard]] std::optional<Table> table(std::string_view tag) const noexcept;
  void outline_into(std::uint16_t gid, const Matrix& m, Path& out, int depth) const;

  Bytes data_;
  std::map<std::string, Table, std::less<>> tables_;
  std::vector<CmapSub> cmaps_;
  std::uint16_t units_per_em_ = 1000;
  std::uint16_t num_glyphs_ = 0;
  std::uint16_t num_hmetrics_ = 0;
  bool long_loca_ = false;
  double ascent_ = 0, descent_ = 0, cap_height_ = 0, line_gap_ = 0, italic_angle_ = 0;
  int weight_class_ = 400;
  bool italic_ = false, fixed_pitch_ = false;
  Rect bbox_;
  std::string ps_name_;
  std::map<std::string, std::uint16_t, std::less<>> post_names_;
  std::map<std::uint32_t, std::int16_t> kern_pairs_;  // (left << 16 | right) -> value
};

// --------------------------------------------------------------------------- CFF

class Cff {
 public:
  /// Parse a bare CFF table (FontFile3 /Type1C or /CIDFontType0C) or an
  /// OpenType font with a 'CFF ' table ('OTTO'; FontFile3 /OpenType).
  /// The first font of the FontSet is used. The data is copied.
  [[nodiscard]] static Result<Cff> parse(ByteView data);

  [[nodiscard]] bool is_cid() const noexcept { return cid_; }
  [[nodiscard]] std::uint16_t num_glyphs() const noexcept;
  /// Glyph by name via the charset (name-keyed fonts).
  [[nodiscard]] std::optional<std::uint16_t> glyph_for_name(std::string_view name) const;
  /// Glyph for a CID via the charset (CID-keyed fonts); for name-keyed fonts
  /// the CID is taken to be the glyph id.
  [[nodiscard]] std::optional<std::uint16_t> glyph_for_cid(std::uint16_t cid) const;
  /// Glyph for a single-byte code through the font's built-in Encoding
  /// (standard, expert or custom, including supplements).
  [[nodiscard]] std::optional<std::uint16_t> glyph_for_code(std::uint8_t code) const;
  /// Glyph name (name-keyed fonts); empty when unknown.
  [[nodiscard]] std::string glyph_name(std::uint16_t gid) const;
  /// The 256 names of the built-in encoding ("" where unmapped).
  [[nodiscard]] std::array<std::string, 256> builtin_encoding() const;

  /// Outline in font units, via the Type 2 charstring interpreter (hint
  /// operators parsed and ignored, flex as curves, seac-style endchar
  /// accents composed, subroutine depth limited). Empty on error.
  [[nodiscard]] Path outline(std::uint16_t gid) const;
  /// Advance width in font units (charstring width or defaultWidthX).
  [[nodiscard]] double advance(std::uint16_t gid) const;
  /// Font matrix of the glyph (Top DICT FontMatrix, composed with the
  /// FDArray font's matrix for CID-keyed fonts).
  [[nodiscard]] Matrix font_matrix(std::uint16_t gid = 0) const;
  [[nodiscard]] const std::string& font_name() const noexcept { return name_; }

 private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  bool cid_ = false;
  std::string name_;
};

// --------------------------------------------------------------------------- Type 1

class Type1 {
 public:
  /// Parse a Type 1 font program: PFA text, PFB segments, or the PDF
  /// FontFile layout (cleartext of `length1` bytes followed by the binary or
  /// hex eexec section; pass 0 to locate "eexec" by searching).
  [[nodiscard]] static Result<Type1> parse(ByteView data, std::size_t length1 = 0);

  /// The /Encoding of the font: StandardEncoding or a custom array.
  [[nodiscard]] const std::array<std::string, 256>& builtin_encoding() const noexcept { return encoding_; }
  [[nodiscard]] bool has_glyph(std::string_view name) const;
  /// Outline in font units (hsbw/sbw, closepath, callsubr, callothersubr
  /// flex/hint replacement, seac composition, div). Empty when missing.
  [[nodiscard]] Path outline(std::string_view glyph_name) const;
  /// Advance width in font units from hsbw/sbw.
  [[nodiscard]] double advance(std::string_view glyph_name) const;
  [[nodiscard]] Matrix font_matrix() const noexcept { return matrix_; }
  [[nodiscard]] const std::string& font_name() const noexcept { return name_; }
  [[nodiscard]] std::vector<std::string> glyph_names() const;

 private:
  std::array<std::string, 256> encoding_;
  std::map<std::string, Bytes, std::less<>> charstrings_;  // decrypted
  std::vector<Bytes> subrs_;                               // decrypted
  Matrix matrix_{0.001, 0, 0, 0.001, 0, 0};
  std::string name_;
};

// --------------------------------------------------------------------------- subsetting

struct SubsetResult {
  /// A complete TrueType font holding only the requested glyphs (plus
  /// .notdef and the components of composite glyphs), with glyph ids
  /// renumbered densely in ascending order of the old ids. Tables kept:
  /// head, hhea, maxp, loca, glyf, hmtx, cvt , fpgm, prep, OS/2, post
  /// (format 3), name (copied as is unless `strip_names`), cmap (a minimal
  /// (3,1) format 4 table for the kept glyphs that have Unicode mappings).
  Bytes font;
  /// old glyph id -> new glyph id, for every kept glyph.
  std::map<std::uint16_t, std::uint16_t> new_gid;
};

/// Subset a TrueType (glyf) font for embedding as FontFile2.
[[nodiscard]] Result<SubsetResult> subset_truetype(const TrueType& font, const std::vector<std::uint16_t>& glyphs);

}  // namespace cvt::pdf::font
