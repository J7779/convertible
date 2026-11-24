// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/cff.hpp"
#include "families/font/font_test_util.hpp"
#include "families/font/glyf.hpp"
#include "support/test.hpp"

using namespace cvt;
using namespace cvt::font;
using namespace cvt::font::testing;

TEST_CASE("font/robustness: every reader survives truncation and corruption") {
  const struct {
    const char* format;
    const char* file;
    std::size_t prefixes;
  } cases[] = {{"ttf", "inter_subset.ttf", 128}, {"otf", "inter_subset.otf", 128},  {"ttc", "pair.ttc", 96},
               {"woff", "inter_subset.woff", 96}, {"woff2", "inter_subset.woff2", 64}, {"woff2", "pair.woff2", 48},
               {"eot", "inter_subset.eot", 96},   {"dfont", "inter_subset.dfont", 96}, {"cff", "inter_subset.cff", 24},
               {"pfb", "inter_subset.pfb", 16},   {"pfa", "inter_subset.pfa", 16},   {"bdf", "mini16.bdf", 64},
               {"pcf", "mini16.pcf", 96},         {"psf", "mono16.psf", 96},         {"fnt", "mini16.fnt", 96},
               {"fon", "mini16.fon", 96},         {"otb", "mini16.otb", 96},         {"svg-font", "inter_subset.svg", 16},
               {"woff2", "jbmono_hmtx.woff2", 48}};
  for (const auto& c : cases) {
    INFO(c.file);
    CHECK_GT(test::fuzz_reader<ir::Font>(c.format, test::fixture(std::string("font/") + c.file), c.prefixes), 0u);
  }
  CHECK_GT(test::fuzz_reader<ir::Data>("afm", test::fixture("font/inter_subset.afm"), 32), 0u);
}

TEST_CASE("font/robustness: corrupted table contents never crash the converters") {
  const struct {
    const char* format;
    const char* file;
    const char* tags[4];
  } cases[] = {{"ttf", "inter_subset.ttf", {"glyf", "cmap", "GSUB", "hmtx"}},
               {"otf", "inter_subset.otf", {"CFF ", "cmap", "post", "name"}},
               {"otf", "inter_subset_cff2.otf", {"CFF2", "cmap", "hhea", "maxp"}},
               {"ttf", "tiny_variable.ttf", {"gvar", "fvar", "glyf", "loca"}}};
  std::size_t attempts = 0;
  for (const auto& c : cases) {
    const Bytes source = test::fixture(std::string("font/") + c.file);
    auto entries = read_directory(source, 0);
    REQUIRE_OK(entries);
    for (const char* tag : c.tags) {
      const auto entry = std::ranges::find_if(*entries, [&](const DirectoryEntry& e) { return e.tag == tag_value(tag); });
      REQUIRE(entry != entries->end());
      for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        Bytes damaged = source;
        const Bytes noise = test::random_bytes(24, seed * 7919 + entry->tag);
        for (std::size_t i = 0; i + 2 < noise.size(); i += 3) {
          const std::size_t at = entry->offset + (std::to_integer<std::size_t>(noise[i]) << 8 | std::to_integer<std::size_t>(noise[i + 1])) % std::max<std::uint32_t>(entry->length, 1);
          damaged[at] = noise[i + 2];
        }
        for (const char* target : {"otf", "ttf", "woff2"}) {
          const test::Converted out = test::convert(damaged, c.format, target);
          ++attempts;
          (void)out;  // success or a clean error are both fine
        }
        auto font = test::read_as<ir::Font>(c.format, damaged);
        if (font) {
          for (const Bridge& b : Registry::instance().bridges()) {
            if (b.from != Kind::Font || b.to != Kind::Image) continue;
            Context ctx;
            ctx.options = Options::parse(R"({"image_width":300,"font_size":12})").value();
            (void)b.fn(AnyIR(*font), ctx);
          }
        }
      }
    }
  }
  CHECK_GT(attempts, 100u);
}

TEST_CASE("font/robustness: hostile glyph structures are rejected") {
  ir::Font font = load("ttf", "inter_subset.ttf");
  auto slices = glyph_slices(font);
  REQUIRE_OK(slices);
  // Agrave (glyph 4) is a composite; make its first component point to itself.
  const auto offset = static_cast<std::size_t>((*slices)[4].data() - font.find("glyf")->bytes.data());
  {
    ir::Font self = font;
    put_u16(self.find("glyf")->bytes, offset + 12, 4);
    auto again = glyph_slices(self);
    REQUIRE_OK(again);
    CHECK_STATUS(resolve_glyph(*again, 4), Status::Decode);
    const test::Converted out = test::convert(write_sfnt(self).value(), "ttf", "otf");
    CHECK_EQ(out.status, Status::Decode);
  }
  {
    ir::Font missing = font;
    put_u16(missing.find("glyf")->bytes, offset + 12, 60000);
    auto again = glyph_slices(missing);
    REQUIRE_OK(again);
    CHECK_STATUS(resolve_glyph(*again, 4), Status::Decode);
  }
  {
    // maxp claims many more glyphs than loca holds.
    ir::Font giant = font;
    put_u16(giant.find("maxp")->bytes, 4, 65535);
    CHECK_STATUS(glyph_slices(giant), Status::Decode);
    CHECK_EQ(test::convert(write_sfnt(giant).value(), "ttf", "otf").status, Status::Decode);
  }
  {
    // A cmap format 12 group spanning every code point.
    util::ByteWriter cmap;
    cmap.u16be(0).u16be(1).u16be(3).u16be(10).u32be(12);
    cmap.u16be(12).u16be(0).u32be(28).u32be(0).u32be(1).u32be(0).u32be(0x10FFFF).u32be(1);
    auto parsed = parse_cmap(cmap.view(), 153);
    REQUIRE_OK(parsed);
    CHECK_EQ(parsed->size(), 152u);  // glyph ids past the font are dropped, the rest is bounded
  }
}

TEST_CASE("font/robustness: hostile charstrings are rejected") {
  const ir::Font otf = load("otf", "inter_subset.otf");
  auto parsed = parse_cff(otf.find("CFF ")->bytes);
  REQUIRE_OK(parsed);
  CffFont font = *parsed;
  // Stack bomb: 60 numbers then an operator.
  Bytes bomb(60, std::byte{139});
  bomb.push_back(std::byte{5});
  font.charstrings[1] = bomb;
  CHECK_STATUS(cff_glyph(font, 1), Status::Decode);
  // Unbounded recursion through a global subroutine calling itself.
  const Bytes recursive = {std::byte{32}, std::byte{29}};  // -107 callgsubr -> subroutine 0
  font.global_subrs = {recursive};
  font.charstrings[1] = recursive;
  CHECK_STATUS(cff_glyph(font, 1), Status::Decode);
  // Calls to subroutines that do not exist.
  font.global_subrs.clear();
  CHECK_STATUS(cff_glyph(font, 1), Status::Decode);
  // Truncated numbers and reserved operators.
  const Bytes truncated = {std::byte{28}, std::byte{1}};
  font.charstrings[1] = truncated;
  CHECK_STATUS(cff_glyph(font, 1), Status::Decode);
  const Bytes reserved = {std::byte{2}};
  font.charstrings[1] = reserved;
  CHECK_STATUS(cff_glyph(font, 1), Status::Decode);
  // INDEX offsets that run backwards.
  Bytes cff = otf.find("CFF ")->bytes;
  const std::size_t names = 4;  // Name INDEX right after the 4-byte header
  cff[names + 3] = std::byte{0x7F};
  CHECK_STATUS(parse_cff(cff), Status::Decode);
}

TEST_CASE("font/robustness: cancellation and limits are honoured") {
  const Bytes source = test::fixture("font/inter_subset.ttf");
  Limits limits;
  limits.max_pixels = 1000;
  CHECK_EQ(test::convert(source, "ttf", "png", "", "", limits).status, Status::Limit);
  limits = Limits{};
  limits.max_archive_entries = 1;
  CHECK_EQ(test::convert(test::fixture("font/pair.ttc"), "ttc", "ttf", "", "", limits).status, Status::Limit);
  // Progress cancellation stops a long conversion.
  ReadContext ctx;
  ctx.input = source;
  ctx.registry = &Registry::instance();
  ctx.progress = Progress([](double, std::string_view) { return true; });  // true = cancel
  const ir::Font font = load("ttf", "inter_subset.ttf");
  CHECK_STATUS(to_cff(font, true, ctx), Status::Cancelled);
}
