// Convertible core - pdf family: fonts as used by content streams
// (encodings, widths, Unicode mapping, glyph outlines).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "families/pdf/cmap.hpp"
#include "families/pdf/document.hpp"
#include "families/pdf/font_program.hpp"
#include "families/pdf/geom.hpp"

namespace cvt::pdf {

/// Substitute faces shared by all fonts of one conversion.
struct FontCache {
  std::shared_ptr<const font::TrueType> sans;
  std::shared_ptr<const font::TrueType> sans_bold;
  std::shared_ptr<const font::TrueType> mono;
  bool loaded = false;
  void ensure_loaded();
};

struct GlyphCode {
  std::uint32_t code = 0;
  int length = 1;
  std::uint32_t cid = 0;
};

class PdfFont {
 public:
  enum class Kind : std::uint8_t { Type1, TrueType, Type3, Type0 };

  /// Load a font dictionary. Never fails: broken fonts degrade to a
  /// standard-14 substitute with best-effort widths.
  [[nodiscard]] static std::shared_ptr<PdfFont> load(PdfFile& file, const Object& font_dict, FontCache& cache);

  /// Split a show-string into character codes.
  void decode(std::string_view bytes, std::vector<GlyphCode>& out) const;
  /// Horizontal advance in text space units (1 = font size), before
  /// character/word spacing and horizontal scaling.
  [[nodiscard]] double width(const GlyphCode& g) const;
  /// Vertical writing: advance (negative = downwards) and position vector.
  [[nodiscard]] double vertical_advance(const GlyphCode& g) const;
  [[nodiscard]] Point vertical_origin(const GlyphCode& g) const;
  /// Unicode text of a glyph; empty when unknown.
  [[nodiscard]] std::u32string unicode(const GlyphCode& g) const;
  /// True for the single-byte code 32 (word spacing applies).
  [[nodiscard]] bool is_space_code(const GlyphCode& g) const noexcept { return g.length == 1 && g.code == 32; }

  /// Glyph outline in TEXT SPACE (font matrix applied, width substitution
  /// applied); null for Type 3 fonts or glyphs without outline.
  [[nodiscard]] const Path* outline(const GlyphCode& g) const;

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool vertical() const noexcept { return vertical_; }
  [[nodiscard]] const std::string& base_font() const noexcept { return base_font_; }
  [[nodiscard]] bool bold() const noexcept { return bold_; }
  [[nodiscard]] bool italic() const noexcept { return italic_; }
  [[nodiscard]] bool monospace() const noexcept { return mono_; }
  [[nodiscard]] bool serif() const noexcept { return serif_; }
  /// Ascent / descent in text space units (descent negative).
  [[nodiscard]] double ascent() const noexcept { return ascent_; }
  [[nodiscard]] double descent() const noexcept { return descent_; }
  [[nodiscard]] bool embedded() const noexcept { return embedded_; }

  // Type 3
  [[nodiscard]] const Matrix& type3_matrix() const noexcept { return type3_matrix_; }
  [[nodiscard]] Object type3_charproc(const GlyphCode& g) const;
  [[nodiscard]] const Object& type3_resources() const noexcept { return type3_resources_; }

 private:
  void load_simple(PdfFile& file, const Dict& dict, FontCache& cache);
  void load_type0(PdfFile& file, const Dict& dict, FontCache& cache);
  void load_program(PdfFile& file, const Dict& descriptor, FontCache& cache, bool cid);
  void apply_descriptor(PdfFile& file, const Dict& descriptor);
  [[nodiscard]] Path build_outline(const GlyphCode& g) const;
  [[nodiscard]] std::string glyph_name(std::uint32_t code) const;

  Kind kind_ = Kind::Type1;
  std::string base_font_;
  bool vertical_ = false;
  bool bold_ = false, italic_ = false, mono_ = false, serif_ = false, symbolic_ = false, embedded_ = false;
  double ascent_ = 0.8, descent_ = -0.2;

  // simple fonts
  std::array<std::string, 256> names_;      // effective glyph names by code
  std::array<double, 256> widths_{};        // in 1/1000 em (text space * 1000)
  std::array<bool, 256> has_width_{};
  double missing_width_ = 0;
  bool has_differences_ = false;
  bool encoding_from_name_ = false;          // /Encoding was a name or dict with a base
  // composite fonts
  CMap cmap_;
  std::unordered_map<std::uint32_t, double> cid_widths_;  // 1/1000
  double default_width_ = 1000;
  std::unordered_map<std::uint32_t, std::array<double, 3>> cid_vmetrics_;  // w1y, vx, vy
  std::array<double, 2> default_vmetrics_{880, -1000};
  std::vector<std::uint16_t> cid_to_gid_;
  bool cid_to_gid_identity_ = true;
  std::string ordering_;
  // Unicode
  std::optional<CMap> to_unicode_;
  // programs
  std::shared_ptr<const font::TrueType> truetype_;
  std::shared_ptr<const font::Cff> cff_;
  std::shared_ptr<const font::Type1> type1_;
  std::shared_ptr<const font::TrueType> substitute_;
  std::map<std::uint16_t, char32_t> program_unicode_;  // gid -> code point (TrueType cmaps)
  // Type 3
  Matrix type3_matrix_{0.001, 0, 0, 0.001, 0, 0};
  Dict type3_procs_;
  Object type3_resources_;

  mutable std::unordered_map<std::uint64_t, std::unique_ptr<Path>> outline_cache_;
};

}  // namespace cvt::pdf
