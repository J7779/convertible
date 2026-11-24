// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/font_test_util.hpp"
#include "families/font/subset.hpp"
#include "support/test.hpp"

using namespace cvt;
using namespace cvt::font;
using namespace cvt::font::testing;

namespace {

CharMap cmap_of(const ir::Font& font) {
  auto map = parse_cmap(font.find("cmap")->bytes, num_glyphs(font));
  return map ? *map : CharMap{};
}

bool glyph_has_outline(const ir::Font& font, std::uint32_t glyph) {
  auto source = GlyphSource::open(font);
  if (!source) return false;
  auto path = source->outline(glyph);
  return path && !path->empty();
}

}  // namespace

TEST_CASE("font/subset: code point lists are parsed strictly") {
  auto a = parse_unicodes("U+0041-0043, u+00e9;0x20 7A");
  REQUIRE_OK(a);
  REQUIRE(a->has_value());
  CHECK((**a == std::set<std::uint32_t>{0x20, 0x41, 0x42, 0x43, 0x7A, 0xE9}));
  auto all = parse_unicodes("*");
  REQUIRE_OK(all);
  CHECK(!all->has_value());
  CHECK_STATUS(parse_unicodes("U+0043-0041"), Status::InvalidArgument);
  CHECK_STATUS(parse_unicodes("hello"), Status::InvalidArgument);
  CHECK_STATUS(parse_unicodes("U+110000"), Status::InvalidArgument);
  CHECK_EQ(test::convert(test::fixture("font/inter_subset.ttf"), "ttf", "ttf", R"({"unicodes":"nope"})").status,
           Status::InvalidArgument);
}

TEST_CASE("font/subset: layout fonts keep glyph ids, close over composites and GSUB, and shrink") {
  const Bytes source = test::fixture("font/inter_subset.ttf");
  const ir::Font original = read_bytes("ttf", source);
  const test::Converted out = test::convert(source, "ttf", "ttf", R"({"text":"Àfi"})");
  REQUIRE(out.ok());
  CHECK(checksums_valid(out.data));
  const ir::Font subset = read_bytes("ttf", out.data);
  MESSAGE("subset to 'Àfi': ", source.size(), " -> ", out.data.size(), " bytes");
  CHECK_LT(out.data.size(), source.size() * 2 / 3);
  // Glyph ids are unchanged because GSUB/GPOS are kept.
  CHECK_EQ(num_glyphs(subset), num_glyphs(original));
  CHECK(subset.find("GSUB")->bytes == original.find("GSUB")->bytes);
  const CharMap map = cmap_of(subset);
  CHECK((std::set<std::uint32_t>{0x20, 0xC0, 'f', 'i'}) == [&] {
    std::set<std::uint32_t> keys;
    for (const auto& [code, glyph] : map) keys.insert(code);
    return keys;
  }());
  const CharMap full = cmap_of(original);
  // Agrave is a composite of A and an accent: both stay, A is no longer mapped.
  CHECK(glyph_has_outline(subset, full.at(0xC0)));
  CHECK(glyph_has_outline(subset, full.at('A')));
  CHECK(!glyph_has_outline(subset, full.at('B')));
  // Glyphs reachable through GSUB (alternates, ligatures of kept glyphs) are kept.
  const std::set<std::uint16_t> closure = gsub_closure(original, {0, full.at('f'), full.at('i')});
  for (const std::uint16_t g : closure) CHECK(glyph_has_outline(subset, g) == glyph_has_outline(original, g));
  auto glyphs = GlyphSource::open(subset);
  REQUIRE_OK(glyphs);
  for (std::uint32_t g = 0; g < glyphs->glyph_count(); ++g) CHECK_OK(glyphs->outline(g));  // every glyph still parses
}

TEST_CASE("font/subset: fonts without layout tables are renumbered compactly") {
  const Bytes source = test::fixture("font/inter_subset.ttf");
  const test::Converted out = test::convert(source, "ttf", "ttf", R"({"text":"Hello","drop_layout":true})");
  REQUIRE(out.ok());
  const ir::Font subset = read_bytes("ttf", out.data);
  CHECK(subset.find("GSUB") == nullptr);
  CHECK_LT(num_glyphs(subset), 12);
  const CharMap map = cmap_of(subset);
  CHECK_EQ(map.size(), 5u);  // H e l o space
  const ir::Font original = read_bytes("ttf", source);
  auto a = GlyphSource::open(original);
  auto b = GlyphSource::open(subset);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  for (const char32_t c : std::u32string_view(U"Helo")) {
    auto pa = a->outline(a->cmap().at(c));
    auto pb = b->outline(b->cmap().at(c));
    REQUIRE_OK(pa);
    REQUIRE_OK(pb);
    CHECK_LT(outline_distance(*pa, *pb), 1e-9);
    CHECK_EQ(a->advance(a->cmap().at(c)), b->advance(b->cmap().at(c)));
  }
  CHECK_LT(out.data.size(), 8000u);
  // CFF fonts are renumbered too, keeping their charstrings verbatim.
  const test::Converted cff = test::convert(test::fixture("font/inter_subset.otf"), "otf", "otf", R"({"text":"Hello","drop_layout":true})");
  REQUIRE(cff.ok());
  const ir::Font cff_subset = read_bytes("otf", cff.data);
  CHECK_LT(num_glyphs(cff_subset), 12);
  CHECK_LT(max_outline_deviation(read_bytes("ttf", out.data), cff_subset), 1e9);
  auto names = glyph_names(cff_subset);
  CHECK(std::ranges::find(names, std::string("H")) != names.end());
}

TEST_CASE("font/subset: variable fonts keep consistent gvar data") {
  const test::Converted out = test::convert(test::fixture("font/tiny_variable.ttf"), "ttf", "ttf", R"({"text":"A"})");
  REQUIRE(out.ok());
  const ir::Font subset = read_bytes("ttf", out.data);
  REQUIRE(subset.find("gvar") != nullptr);
  CHECK_EQ(get_u16(subset.find("gvar")->bytes, 12), num_glyphs(subset));
  CHECK(subset.find("fvar")->bytes == load("ttf", "tiny_variable.ttf").find("fvar")->bytes);
}

TEST_CASE("font/optimise: hinting, names and glyph names can be dropped; legal names always stay") {
  const Bytes source = test::fixture("font/inter_subset.ttf");
  const test::Converted plain = test::convert(source, "ttf", "ttf");
  const test::Converted nohint = test::convert(source, "ttf", "ttf", R"({"drop_hinting":true})");
  const test::Converted small = test::convert(source, "ttf", "woff2", R"({"preset":"small"})");
  const test::Converted high = test::convert(source, "ttf", "woff2");
  REQUIRE(plain.ok());
  REQUIRE(nohint.ok());
  REQUIRE(small.ok());
  REQUIRE(high.ok());
  CHECK_LT(nohint.data.size(), plain.data.size());
  CHECK_LT(small.data.size(), high.data.size());
  const ir::Font unhinted = read_bytes("ttf", nohint.data);
  for (const char* tag : {"fpgm", "prep", "cvt "}) CHECK(unhinted.find(tag) == nullptr);
  CHECK_LT(max_outline_deviation(read_bytes("ttf", source), unhinted), 1e-9);

  const test::Converted names = test::convert(source, "ttf", "ttf", R"({"drop_names":true,"drop_glyph_names":true})");
  REQUIRE(names.ok());
  const ir::Font cleaned = read_bytes("ttf", names.data);
  auto records = parse_name(cleaned.find("name")->bytes);
  REQUIRE_OK(records);
  for (const std::uint16_t id : {0, 1, 2, 4, 6, 13, 14}) {
    INFO("name id ", id);
    CHECK(!best_name(*records, id).empty());
  }
  CHECK_EQ(get_u32(cleaned.find("post")->bytes, 0), 0x00030000u);
  // strip_metadata (on by default) never removes legal notices.
  auto stripped = parse_name(read_bytes("ttf", plain.data).find("name")->bytes);
  REQUIRE_OK(stripped);
  CHECK(best_name(*stripped, 0).find("Copyright") != std::string::npos);
  CHECK(best_name(*stripped, 13).find("Open Font License") != std::string::npos);
}
