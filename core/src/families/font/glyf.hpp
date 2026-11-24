// Convertible core - font family: TrueType glyf/loca outlines.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/errors.hpp"
#include "families/font/outline.hpp"
#include "ir/font.hpp"
#include "util/bytes.hpp"

namespace cvt::font {

/// Composite glyph flags.
namespace composite {
inline constexpr std::uint16_t arg_words = 0x0001;
inline constexpr std::uint16_t args_xy = 0x0002;
inline constexpr std::uint16_t round_xy = 0x0004;
inline constexpr std::uint16_t have_scale = 0x0008;
inline constexpr std::uint16_t more = 0x0020;
inline constexpr std::uint16_t xy_scale = 0x0040;
inline constexpr std::uint16_t two_by_two = 0x0080;
inline constexpr std::uint16_t instructions = 0x0100;
inline constexpr std::uint16_t use_metrics = 0x0200;
inline constexpr std::uint16_t overlap = 0x0400;
inline constexpr std::uint16_t scaled_offset = 0x0800;
inline constexpr std::uint16_t unscaled_offset = 0x1000;
}  // namespace composite

struct TtPoint {
  std::int32_t x = 0;
  std::int32_t y = 0;
  bool on_curve = true;
  friend bool operator==(const TtPoint&, const TtPoint&) = default;
};

struct TtComponent {
  std::uint16_t flags = 0;
  std::uint16_t glyph = 0;
  std::int32_t arg1 = 0;
  std::int32_t arg2 = 0;
  /// 2x2 transform in F2Dot14 units (raw), as stored.
  std::int16_t xx = 0x4000, xy = 0, yx = 0, yy = 0x4000;
};

/// One decoded glyf entry.
struct TtGlyph {
  bool empty = true;  ///< zero-length glyph
  std::int16_t num_contours = 0;
  std::int16_t x_min = 0, y_min = 0, x_max = 0, y_max = 0;
  std::vector<std::uint16_t> end_points;
  std::vector<TtPoint> points;
  std::vector<TtComponent> components;
  Bytes instructions;
  bool overlap_simple = false;
  [[nodiscard]] bool is_composite() const noexcept { return num_contours < 0; }
};

[[nodiscard]] Result<TtGlyph> parse_glyph(ByteView data);
/// Serialise a glyph (simple or composite); simple glyph bounds are
/// recomputed from the points when `recompute_bounds` is true.
[[nodiscard]] Bytes encode_glyph(const TtGlyph& glyph, bool recompute_bounds = true);

/// The glyph data slices addressed by loca (bounds checked).
[[nodiscard]] Result<std::vector<ByteView>> glyph_slices(const ir::Font& font);

/// Points of a TrueType contour list as a Path (implied points restored).
[[nodiscard]] Path contours_to_path(std::span<const TtPoint> points, std::span<const std::uint16_t> end_points);
/// Path (lines and quadratics only) to TrueType contours; coordinates are
/// rounded, redundant implied on-curve points are dropped.
void path_to_contours(const Path& path, std::vector<TtPoint>& points, std::vector<std::uint16_t>& end_points);

/// Resolve a glyph, including nested composites (depth and point counts
/// are capped), to a Path in font units. Also returns the flat point list.
struct ResolvedGlyph {
  Path path;
  std::vector<TtPoint> points;
  std::vector<std::uint16_t> end_points;
};
[[nodiscard]] Result<ResolvedGlyph> resolve_glyph(std::span<const ByteView> slices, std::uint32_t glyph);

/// Build glyf and loca from encoded glyphs; returns indexToLocFormat.
struct GlyfLoca {
  Bytes glyf;
  Bytes loca;
  std::int16_t index_format = 0;
};
[[nodiscard]] GlyfLoca build_glyf_loca(std::span<const Bytes> glyphs);

}  // namespace cvt::font
