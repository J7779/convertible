// Convertible desktop - text shaping (simple left-to-right with kerning),
// measurement, truncation, wrapping, hit-testing and glyph-cached drawing.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/geometry.hpp"
#include "gfx/canvas.hpp"
#include "text/font.hpp"

namespace cvd::text {

enum class Face : std::uint8_t { Regular, Medium, SemiBold, Mono, MonoMedium };
inline constexpr int kFaceCount = 5;

struct Style {
  Face face = Face::Regular;
  float size = 15.0f;         // px (logical)
  float line_height = 22.0f;  // px (logical)
  float tracking = 0.0f;      // letter spacing in em
  bool tabular = false;       // tabular figures (GSUB "tnum")
  bool uppercase = false;     // ASCII upper-casing before shaping

  [[nodiscard]] Style with_size(float s, float lh) const {
    Style copy = *this;
    copy.size = s;
    copy.line_height = lh;
    return copy;
  }
  [[nodiscard]] Style with_face(Face f) const {
    Style copy = *this;
    copy.face = f;
    return copy;
  }
};

/// The bundled font faces. `install` must be called once before text is used.
class FontSet {
 public:
  static FontSet& instance();
  /// Takes the raw font files (which must stay alive) in Face order.
  bool install(const std::array<std::span<const std::uint8_t>, kFaceCount>& files);
  [[nodiscard]] bool ready() const { return ready_; }
  [[nodiscard]] const Font& font(Face face) const { return fonts_[static_cast<std::size_t>(face)]; }

 private:
  std::array<Font, kFaceCount> fonts_;
  bool ready_ = false;
};

struct ShapedGlyph {
  std::uint16_t glyph = 0;
  Face face = Face::Regular;  // the face that supplied the glyph (fallback aware)
  float x = 0.0f;             // pen position (logical px) relative to the run start
  float advance = 0.0f;
  std::uint32_t byte = 0;     // byte offset of the source code point
};

struct ShapedRun {
  std::vector<ShapedGlyph> glyphs;
  float width = 0.0f;
};

ShapedRun shape(std::string_view text, const Style& style);

/// Diagnostics (automation and tests): printable code points that no bundled
/// face could map since the last reset, i.e. would render as a missing-glyph
/// box, and the number of glyphs shaped.
struct GlyphStats {
  std::uint64_t shaped = 0;
  std::uint64_t missing = 0;
  std::vector<std::uint32_t> missing_codepoints;  // first few, for messages
};
GlyphStats glyph_stats();
void reset_glyph_stats();
float measure(std::string_view text, const Style& style);

/// Distance from the top of a line box to the baseline.
float baseline_offset(const Style& style);
/// Ascender height (cap-ish) in px, for vertical centring of single lines.
float ascent(const Style& style);
float descent(const Style& style);  // positive

/// Truncates at the end with "…" to fit `max_width`.
std::string ellipsize_end(std::string_view text, const Style& style, float max_width);
/// Truncates in the middle, keeping the end (e.g. a file extension) visible.
std::string ellipsize_middle(std::string_view text, const Style& style, float max_width);
/// Greedy word wrap; `max_lines` > 0 limits the lines and ellipsizes the last one.
std::vector<std::string> wrap(std::string_view text, const Style& style, float width, int max_lines = 0);

/// Byte offset of the caret position nearest to `x` (relative to the run start).
std::size_t hit_test(std::string_view text, const Style& style, float x);
/// x of the caret placed before byte `index`.
float caret_x(std::string_view text, const Style& style, std::size_t index);

/// Glyph mask cache (device pixels); used on the UI thread only.
class GlyphCache {
 public:
  static GlyphCache& instance();
  const gfx::Mask& get(Face face, std::uint16_t glyph, float size_px, int subpixel);
  void clear() { cache_.clear(); }
  [[nodiscard]] std::size_t size() const { return cache_.size(); }

 private:
  std::unordered_map<std::uint64_t, gfx::Mask> cache_;
};

/// Draws a single line with its baseline at `origin` (user space of the canvas).
void draw_text(gfx::Canvas& canvas, std::string_view text, const Style& style, Vec2 origin, Color color);

}  // namespace cvd::text
