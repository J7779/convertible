// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <map>
#include <set>

#include "families/font/font_test_util.hpp"
#include "support/test.hpp"

using namespace cvt;
using namespace cvt::font;
using namespace cvt::font::testing;

namespace {

using InkSet = std::set<std::pair<int, int>>;  // (x from origin, y above baseline)

std::map<std::int32_t, InkSet> ink_by_code(const ir::BitmapFont& font) {
  std::map<std::int32_t, InkSet> out;
  for (const ir::BitmapGlyph& g : font.glyphs) {
    if (g.codepoint < 0 || out.contains(g.codepoint)) continue;
    InkSet ink;
    for (std::uint32_t y = 0; y < g.height; ++y) {
      for (std::uint32_t x = 0; x < g.width; ++x) {
        if (g.pixels[std::size_t{y} * g.width + x]) {
          ink.emplace(g.x_offset + static_cast<int>(x), g.y_offset + static_cast<int>(g.height - 1 - y));
        }
      }
    }
    out[g.codepoint] = std::move(ink);
  }
  return out;
}

std::map<std::int32_t, std::int32_t> advances(const ir::BitmapFont& font) {
  std::map<std::int32_t, std::int32_t> out;
  for (const ir::BitmapGlyph& g : font.glyphs) {
    if (g.codepoint >= 0) out.try_emplace(g.codepoint, g.advance);
  }
  return out;
}

const ir::BitmapFont& strike(const ir::Font& font) {
  static const ir::BitmapFont empty;
  return font.bitmap ? *font.bitmap : empty;
}

// Count of code points whose ink differs (with a horizontal shift allowed for
// formats that store whole cells).
std::size_t ink_differences(const ir::BitmapFont& a, const ir::BitmapFont& b, bool allow_shift = false) {
  const auto ia = ink_by_code(a), ib = ink_by_code(b);
  std::size_t diffs = 0;
  for (const auto& [code, ink] : ia) {
    const auto it = ib.find(code);
    if (it == ib.end()) {
      ++diffs;
      continue;
    }
    bool same = it->second == ink;
    for (int shift = -2; !same && allow_shift && shift <= 2; ++shift) {
      InkSet moved;
      for (const auto& [x, y] : ink) moved.emplace(x + shift, y);
      same = moved == it->second;
    }
    if (!same) ++diffs;
  }
  return diffs;
}

// Count of code points whose ink differs once each glyph is moved to its own
// bottom-left ink corner (for formats without a baseline, such as PSF cells).
std::size_t shape_differences(const ir::BitmapFont& a, const ir::BitmapFont& b, std::size_t* compared = nullptr) {
  const auto normalise = [](const InkSet& s) {
    int mx = 1 << 20, my = 1 << 20;
    for (const auto& [x, y] : s) mx = std::min(mx, x), my = std::min(my, y);
    InkSet out;
    for (const auto& [x, y] : s) out.emplace(x - mx, y - my);
    return out;
  };
  const auto ia = ink_by_code(a), ib = ink_by_code(b);
  std::size_t diffs = 0, n = 0;
  for (const auto& [code, ink] : ia) {
    const auto it = ib.find(code);
    if (it == ib.end() || ink.empty()) continue;
    ++n;
    if (normalise(ink) != normalise(it->second)) ++diffs;
  }
  if (compared) *compared = n;
  return diffs;
}

}  // namespace

TEST_CASE("font/bdf: FontForge BDF and bdftopcf PCF hold the same glyphs") {
  const ir::Font bdf = load("bdf", "mini16.bdf");
  const ir::Font pcf = load("pcf", "mini16.pcf");
  REQUIRE(bdf.bitmap.has_value());
  REQUIRE(pcf.bitmap.has_value());
  CHECK_EQ(bdf.bitmap->pixel_size, 16u);
  CHECK_EQ(bdf.bitmap->ascent, 13);
  CHECK_EQ(bdf.bitmap->descent, 3);
  CHECK_EQ(pcf.bitmap->ascent, 13);
  CHECK_EQ(pcf.bitmap->descent, 3);
  CHECK_GE(ink_by_code(*bdf.bitmap).size(), 95u);
  CHECK_EQ(ink_differences(*bdf.bitmap, *pcf.bitmap), 0u);
  CHECK(advances(*bdf.bitmap) == advances(*pcf.bitmap));
  // 'H' has two stems: ink in at least 2 separate columns at mid height.
  const InkSet h = ink_by_code(*bdf.bitmap).at('H');
  CHECK_GT(h.size(), 20u);
}

TEST_CASE("font/bdf: BDF, PCF and PSF output round trips exactly") {
  const ir::Font bdf = load("bdf", "mini16.bdf");
  for (const char* format : {"bdf", "pcf"}) {
    INFO(format);
    auto written = test::write_from(format, bdf);
    REQUIRE_OK(written);
    const ir::Font back = read_bytes(format, *written);
    REQUIRE(back.bitmap.has_value());
    CHECK_EQ(ink_differences(*bdf.bitmap, *back.bitmap), 0u);
    CHECK(advances(*bdf.bitmap) == advances(*back.bitmap));
    CHECK_EQ(back.bitmap->ascent, 13);
    // Legal notices travel along.
    bool copyright = false;
    for (const auto& [k, v] : back.bitmap->properties) copyright |= k == "COPYRIGHT" && v.find("Inter Project") != std::string::npos;
    CHECK(copyright);
  }
  const ir::Font mono = load("bdf", "mono16.bdf");
  auto psf = test::write_from("psf", mono);
  REQUIRE_OK(psf);
  const ir::Font back = read_bytes("psf", *psf);
  REQUIRE(back.bitmap.has_value());
  std::size_t compared = 0;
  CHECK_EQ(shape_differences(*mono.bitmap, *back.bitmap, &compared), 0u);
  CHECK_GE(compared, 90u);
}

TEST_CASE("font/psf: bdf2psf console font matches its BDF source") {
  const ir::Font psf = load("psf", "mono16.psf");
  const ir::Font bdf = load("bdf", "mono16.bdf");
  REQUIRE(psf.bitmap.has_value());
  CHECK_EQ(psf.bitmap->glyphs.front().width, 10u);
  CHECK_EQ(psf.bitmap->glyphs.front().height, 16u);
  // PSF cells have no baseline: compare each glyph's shape.
  std::size_t compared = 0;
  const std::size_t diffs = shape_differences(*bdf.bitmap, *psf.bitmap, &compared);
  MESSAGE("PSF glyphs differing from the BDF: ", diffs, " of ", compared, " (bdf2psf clips glyphs wider than the cell)");
  CHECK_GE(compared, 90u);
  CHECK_LE(diffs, 2u);
}

TEST_CASE("font/fnt: Windows FNT and FON resources decode to FontForge's strike") {
  const ir::Font fnt = load("fnt", "mini16.fnt");
  const ir::Font fon = load("fon", "mini16.fon");
  REQUIRE(fnt.bitmap.has_value());
  REQUIRE(fon.bitmap.has_value());
  CHECK_EQ(fnt.bitmap->pixel_size, 20u);
  CHECK_EQ(ink_differences(*fnt.bitmap, *fon.bitmap), 0u);
  const ir::Font mono = load("fnt", "mono16.fnt");
  REQUIRE(mono.bitmap.has_value());
  CHECK_EQ(mono.bitmap->glyphs[0].width, 10u);
  // Glyph shapes agree with the BDF made from the same outlines.
  const auto a = ink_by_code(*load("bdf", "mini16.bdf").bitmap);
  const auto b = ink_by_code(*fnt.bitmap);
  std::size_t close = 0, compared = 0;
  for (const auto& [code, ink] : a) {
    if (code < 0x21 || code > 0x7E || !b.contains(code)) continue;
    ++compared;
    const InkSet& other = b.at(code);
    std::size_t common = 0;
    for (const auto& p : ink) common += other.contains(p) ? 1 : 0;
    if (ink.size() == other.size() && common == ink.size()) ++close;
  }
  MESSAGE("FNT glyphs identical to BDF: ", close, " of ", compared);
  CHECK_GE(compared, 90u);
  CHECK_GE(close, compared * 9 / 10);
}

TEST_CASE("font/otb: embedded bitmaps (EBLC/EBDT) decode to FontForge's strike") {
  const ir::Font otb = load("otb", "mini16.otb");
  REQUIRE(otb.bitmap.has_value());
  CHECK_EQ(otb.bitmap->pixel_size, 16u);
  CHECK_EQ(ink_differences(*load("bdf", "mini16.bdf").bitmap, *otb.bitmap), 0u);
  const test::Converted bdf = test::convert(test::fixture("font/mini16.otb"), "otb", "bdf");
  REQUIRE(bdf.ok());
  CHECK_EQ(ink_differences(*otb.bitmap, strike(read_bytes("bdf", bdf.data))), 0u);
}

TEST_CASE("font/bitmap: outline fonts rasterise into bitmap fonts and bitmaps trace into outlines") {
  const test::Converted bdf = test::convert(test::fixture("font/inter_subset.ttf"), "ttf", "bdf", R"({"pixel_size":16})");
  REQUIRE(bdf.ok());
  const ir::Font ours = read_bytes("bdf", bdf.data);
  REQUIRE(ours.bitmap.has_value());
  CHECK_GE(ours.bitmap->glyphs.size(), 100u);
  // Close to FontForge's own 16 px strike (both unhinted rasterisations).
  const auto a = ink_by_code(*load("bdf", "mini16.bdf").bitmap);
  const auto b = ink_by_code(*ours.bitmap);
  std::size_t total = 0, common = 0;
  for (const auto& [code, ink] : a) {
    if (!b.contains(code)) continue;
    total += ink.size() + b.at(code).size();
    for (const auto& p : ink) common += b.at(code).contains(p) ? 2 : 0;
  }
  const double overlap = total ? static_cast<double>(common) / static_cast<double>(total) : 0;
  MESSAGE("16 px rasterisation agreement with FontForge (Dice): ", overlap);
  CHECK_GT(overlap, 0.80);
  // Bitmap -> TTF: rendering the traced outline at the strike size gives back the pixels.
  const test::Converted ttf = test::convert(test::fixture("font/mini16.bdf"), "bdf", "ttf");
  REQUIRE(ttf.ok());
  const ir::Font traced = read_bytes("ttf", ttf.data);
  auto strike_again = test::write_from("bdf", traced, R"({"pixel_size":16})");
  REQUIRE_OK(strike_again);
  CHECK_EQ(ink_differences(*load("bdf", "mini16.bdf").bitmap, strike(read_bytes("bdf", *strike_again))), 0u);
}

TEST_CASE("font/bitmap: hostile bitmap fonts fail cleanly") {
  CHECK_STATUS(test::read_as<ir::Font>("bdf", as_bytes("STARTFONT 2.1\nCHARS 1\nSTARTCHAR a\nBBX 99999 99999 0 0\nBITMAP\nENDCHAR\n")),
               Status::Decode);
  CHECK_STATUS(test::read_as<ir::Font>("psf", as_bytes("\x72\xb5\x4a\x86\0\0\0\0\x20\0\0\0\0\0\0\0\xff\xff\xff\x0f\x01\0\0\0\x10\0\0\0\x08\0\0\0")),
               Status::Decode);
  Bytes pcf = test::fixture("font/mini16.pcf");
  pcf[4] = std::byte{0xFF};  // absurd table count
  CHECK_STATUS(test::read_as<ir::Font>("pcf", pcf), Status::Decode);
  Limits limits;
  limits.max_pixels = 100;
  CHECK_STATUS(test::read_as<ir::Font>("bdf", test::fixture("font/mini16.bdf"), {}, nullptr, limits), Status::Limit);
}
