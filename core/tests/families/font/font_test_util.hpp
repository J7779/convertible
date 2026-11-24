// Shared helpers for the font family tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "families/font/build.hpp"
#include "families/font/glyf.hpp"
#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"
#include "ir/font.hpp"
#include "ir/image.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/json.hpp"

namespace cvt::font::testing {

inline ir::Font load(std::string_view format, std::string_view fixture_name, std::string_view options = {}) {
  auto font = test::read_as<ir::Font>(format, test::fixture(std::string("font/") + std::string(fixture_name)), options);
  if (!font) return ir::Font{};
  return std::move(*font);
}

inline ir::Font read_bytes(std::string_view format, ByteView bytes, std::string_view options = {}) {
  auto font = test::read_as<ir::Font>(format, bytes, options);
  if (!font) return ir::Font{};
  return std::move(*font);
}

/// Tags whose bytes differ between `a` and `b` (or exist in only one),
/// ignoring the tags listed in `skip`.
inline std::vector<std::string> table_differences(const ir::Font& a, const ir::Font& b,
                                                  std::initializer_list<std::string_view> skip = {}) {
  std::vector<std::string> diffs;
  const auto skipped = [&](std::string_view tag) {
    for (const std::string_view s : skip) {
      if (s == tag) return true;
    }
    return false;
  };
  for (const ir::FontTable& t : a.tables) {
    if (skipped(t.tag)) continue;
    const ir::FontTable* other = b.find(t.tag);
    Bytes left = t.bytes;
    Bytes right = other ? other->bytes : Bytes{};
    if (t.tag == "head") {  // checkSumAdjustment depends on the file
      put_u32(left, 8, 0);
      put_u32(right, 8, 0);
    }
    if (!other || left != right) diffs.push_back(t.tag);
  }
  for (const ir::FontTable& t : b.tables) {
    if (!skipped(t.tag) && !a.find(t.tag)) diffs.push_back(t.tag + " (only right)");
  }
  return diffs;
}

/// True when a serialised sfnt has valid table checksums and a correct
/// head.checkSumAdjustment.
inline bool checksums_valid(ByteView file) {
  auto entries = read_directory(file, 0);
  if (!entries) return false;
  for (const DirectoryEntry& e : *entries) {
    Bytes bytes(file.begin() + e.offset, file.begin() + e.offset + e.length);
    if (e.tag == tag_value("head")) put_u32(bytes, 8, 0);
    if (table_checksum(bytes) != e.checksum) return false;
  }
  return table_checksum(file) == 0xB1B0AFBAu;
}

inline Json reference(std::string_view name) {
  auto parsed = util::parse_json(test::fixture_text(std::string("font/") + std::string(name)));
  return parsed ? std::move(*parsed) : Json{};
}

/// Largest geometric distance between the glyphs of two fonts (same ids).
inline double max_outline_deviation(const ir::Font& a, const ir::Font& b, std::size_t* compared = nullptr) {
  auto sa = GlyphSource::open(a);
  auto sb = GlyphSource::open(b);
  if (!sa || !sb || sa->glyph_count() != sb->glyph_count()) return 1e9;
  double worst = 0;
  for (std::uint32_t g = 0; g < sa->glyph_count(); ++g) {
    auto pa = sa->outline(g);
    auto pb = sb->outline(g);
    if (!pa || !pb) return 1e9;
    worst = std::max(worst, outline_distance(*pa, *pb));
  }
  if (compared) *compared = sa->glyph_count();
  return worst;
}

/// Grey coverage (0..1) as an opaque frame: white ink on black, like an 'L' render.
inline ir::Frame coverage_frame(const std::vector<float>& coverage, std::uint32_t width, std::uint32_t height) {
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  for (std::size_t i = 0; i < coverage.size(); ++i) {
    const auto v = static_cast<std::uint8_t>(std::lround(std::clamp(coverage[i], 0.0f, 1.0f) * 255));
    frame.rgba[i * 4] = frame.rgba[i * 4 + 1] = frame.rgba[i * 4 + 2] = v;
  }
  return frame;
}

}  // namespace cvt::font::testing
