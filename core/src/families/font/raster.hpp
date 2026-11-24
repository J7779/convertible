// Convertible core - font family: anti-aliased outline rasteriser and text layout.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/font/outline.hpp"
#include "ir/font.hpp"
#include "ir/image.hpp"

namespace cvt::font {

/// Coverage mask (0..1 per pixel, rows top to bottom), non-zero fill rule
/// approximated by accumulated signed area (exact for non-overlapping
/// contours, clamped for overlaps).
class Rasterizer {
 public:
  Rasterizer(std::uint32_t width, std::uint32_t height);
  /// Add an outline in font units: pixel = (origin_x + x * scale, baseline_y - y * scale).
  void add(const Path& path, double scale, double origin_x, double baseline_y);
  /// The coverage of everything added so far.
  [[nodiscard]] std::vector<float> coverage() const;
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

 private:
  void line(Point a, Point b);
  std::uint32_t width_, height_;
  std::size_t stride_;
  std::vector<float> accumulation_;
};

/// Blend a coverage mask in colour `ink` onto `frame` (straight sRGB).
void composite_mask(ir::Frame& frame, const std::vector<float>& coverage, std::uint32_t mask_width,
               std::uint32_t mask_height, std::int32_t x, std::int32_t y, std::array<std::uint8_t, 4> ink);

/// Options of the specimen renderer.
struct SpecimenOptions {
  std::string sample_text = "The quick brown fox jumps over the lazy dog. 0123456789";
  double font_size = 48;              ///< pixels per em of the sample lines
  std::uint32_t width = 1200;         ///< image width in pixels
  std::array<std::uint8_t, 4> foreground = {20, 20, 20, 255};
  std::array<std::uint8_t, 4> background = {255, 255, 255, 255};
  bool glyph_grid = true;             ///< append a grid of every mapped character
  std::uint32_t max_grid_glyphs = 512;
};

/// A specimen sheet: the family name, the sample text at several sizes and
/// a glyph grid. Works for outline and bitmap-only fonts.
[[nodiscard]] Result<ir::Image> render_specimen(const ir::Font& font, const SpecimenOptions& options, const Context& ctx);

/// Rasterise a single line of text with an outline font at `pixel_size`
/// pixels per em, pen starting at (`origin_x`, `baseline_y`) on a canvas of
/// the given size. No kerning, no shaping (cmap + hmtx only). Used by tests
/// to compare against independent renderers.
[[nodiscard]] Result<std::vector<float>> render_line(const ir::Font& font, std::u32string_view text, double pixel_size,
                                                     std::uint32_t width, std::uint32_t height, double origin_x,
                                                     double baseline_y);

/// Rasterise every mapped glyph of an outline font into a bitmap strike
/// (threshold 50 % coverage) at `pixel_size` pixels per em.
[[nodiscard]] Result<ir::BitmapFont> bitmap_from_outlines(const ir::Font& font, std::uint32_t pixel_size,
                                                          const Context& ctx);

}  // namespace cvt::font
