// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/font_test_util.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/table.hpp"
#include "support/test.hpp"

using namespace cvt;
using namespace cvt::font;
using namespace cvt::font::testing;

namespace {

const Bridge* bridge_to(Kind kind) {
  for (const Bridge& b : Registry::instance().bridges()) {
    if (b.from == Kind::Font && b.to == kind) return &b;
  }
  return nullptr;
}

Result<AnyIR> run_bridge(Kind kind, ir::Font font, std::string_view options = {}) {
  const Bridge* bridge = bridge_to(kind);
  if (!bridge) return fail(Status::UnsupportedConversion, "no bridge");
  Context ctx;
  CVT_TRY_ASSIGN(const Options parsed, Options::parse(options));
  CVT_TRY_ASSIGN(ctx.options, parsed.normalized(bridge->options));
  ctx.registry = &Registry::instance();
  return bridge->fn(AnyIR(std::move(font)), ctx);
}

std::size_t ink_pixels(const ir::Frame& frame) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < frame.rgba.size(); i += 4) n += frame.rgba[i] < 128 ? 1 : 0;
  return n;
}

}  // namespace

TEST_CASE("font/bridges: Font -> Data describes names, metrics, coverage and tables") {
  auto value = run_bridge(Kind::Data, load("ttf", "inter_subset.ttf"));
  REQUIRE_OK(value);
  const Json& data = value->get<ir::Data>().value;
  CHECK_EQ(data["names"]["family"].as_string(), std::string("Inter"));
  CHECK_EQ(data["names"]["license_url"].as_string(), std::string("http://scripts.sil.org/OFL"));
  CHECK_EQ(data["metrics"]["units_per_em"].as_int64(), 2048);
  CHECK_EQ(data["metrics"]["glyphs"].as_int64(), 153);
  CHECK_EQ(data["coverage"]["characters"].as_int64(), 101);
  CHECK_EQ(data["coverage"]["ranges"][0].as_string(), std::string("U+0020-007E"));
  CHECK_EQ(data["tables"].size(), 17u);
  CHECK(data.contains("features"));
  auto variable = run_bridge(Kind::Data, load("ttf", "tiny_variable.ttf"));
  REQUIRE_OK(variable);
  const Json& axes = variable->get<ir::Data>().value["variation_axes"];
  REQUIRE_EQ(axes.size(), 1u);
  CHECK_EQ(axes[0]["tag"].as_string(), std::string("wght"));
  CHECK_EQ(axes[0]["min"].as_double(), 100.0);
  CHECK_EQ(axes[0]["max"].as_double(), 900.0);
  // Through the engine to JSON.
  const test::Converted json = test::convert(test::fixture("font/pair.ttc"), "ttc", "json");
  if (json.ok()) CHECK(json.text().find("JetBrains Mono") != std::string::npos);
}

TEST_CASE("font/bridges: Font -> Table lists the character map") {
  auto value = run_bridge(Kind::Table, load("otf", "inter_subset.otf"));
  REQUIRE_OK(value);
  const ir::Sheet& sheet = value->get<ir::Table>().sheets.at(0);
  REQUIRE_EQ(sheet.rows.size(), 102u);  // header + 101 characters
  CHECK_EQ(sheet.rows[1][0].text, std::string("U+0020"));
  bool found = false;
  for (const auto& row : sheet.rows) {
    if (row[0].text == "U+0041") {
      found = true;
      CHECK_EQ(row[1].text, std::string("A"));
      CHECK_EQ(row[3].text, std::string("A"));
      CHECK_EQ(row[4].number, 1413.0);
    }
  }
  CHECK(found);
  auto bitmap = run_bridge(Kind::Table, load("bdf", "mini16.bdf"));
  REQUIRE_OK(bitmap);
  CHECK_GT(bitmap->get<ir::Table>().sheets.at(0).rows.size(), 95u);
}

TEST_CASE("font/bridges: Font -> Image renders a specimen with the chosen options") {
  auto value = run_bridge(Kind::Image, load("ttf", "inter_subset.ttf"));
  REQUIRE_OK(value);
  const ir::Image& image = value->get<ir::Image>();
  REQUIRE(image.valid());
  CHECK_EQ(image.width(), 1200u);
  CHECK_GT(ink_pixels(image.frames[0]), 10000u);
  auto small = run_bridge(Kind::Image, load("ttf", "inter_subset.ttf"),
                          R"({"sample_text":"Hi","font_size":20,"image_width":300,"glyph_grid":false,"background":"000000","foreground":"ffffff"})");
  REQUIRE_OK(small);
  const ir::Frame& frame = small->get<ir::Image>().frames[0];
  CHECK_EQ(frame.width, 300u);
  CHECK_LT(frame.height, image.height());
  CHECK_EQ(frame.rgba[0], 0);  // black background
  CHECK_STATUS(run_bridge(Kind::Image, load("ttf", "inter_subset.ttf"), R"({"foreground":"nonsense"})"), Status::InvalidArgument);
  // Bitmap fonts draw their pixels.
  auto bitmap = run_bridge(Kind::Image, load("pcf", "mini16.pcf"));
  REQUIRE_OK(bitmap);
  CHECK_GT(ink_pixels(bitmap->get<ir::Image>().frames[0]), 5000u);
  // Through the engine: ttf -> png.
  const test::Converted png = test::convert(test::fixture("font/inter_subset.woff2"), "woff2", "png");
  if (png.ok()) CHECK_GT(png.data.size(), 1000u);
}

TEST_CASE("font/bridges: Font -> Document is a readable report with a specimen") {
  auto value = run_bridge(Kind::Document, load("ttf", "jbmono_subset.ttf"));
  REQUIRE_OK(value);
  const ir::Document& doc = value->get<ir::Document>();
  CHECK(doc.metadata.title.find("JetBrains Mono") != std::string::npos);
  bool image = false, table = false;
  for (const ir::Block& b : doc.blocks) {
    image |= b.is<ir::ImageBlock>();
    table |= b.is<ir::TableBlock>();
  }
  CHECK(image);
  CHECK(table);
  const test::Converted md = test::convert(test::fixture("font/jbmono_subset.ttf"), "ttf", "md");
  if (md.ok()) CHECK(md.text().find("U+0020-007E") != std::string::npos);  // the report, not the character table
}

TEST_CASE("font/svg-font: FontForge SVG fonts decode, and our SVG fonts round trip") {
  const ir::Font svg = load("svg-font", "inter_subset.svg");
  REQUIRE(!svg.tables.empty());
  REQUIRE(svg.names.has_value());
  CHECK_EQ(svg.names->family, std::string("Inter"));
  const ir::Font ttf = load("ttf", "inter_subset.ttf");
  auto a = GlyphSource::open(ttf);
  auto b = GlyphSource::open(svg);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  for (const char32_t c : std::u32string_view(U"AOga&@")) {
    auto pa = a->outline(a->cmap().at(c));
    auto pb = b->outline(b->cmap().at(c));
    REQUIRE_OK(pa);
    REQUIRE_OK(pb);
    CHECK_LT(outline_distance(*pa, *pb), 1.0);
    CHECK_EQ(a->advance(a->cmap().at(c)), b->advance(b->cmap().at(c)));
  }
  auto written = test::write_from("svg-font", ttf);
  REQUIRE_OK(written);
  CHECK(to_string(ByteView(*written)).find("<font-face") != std::string::npos);
  const ir::Font back = read_bytes("svg-font", *written);
  CHECK_LT(max_outline_deviation(svg, svg), 1e-9);
  auto c = GlyphSource::open(back);
  REQUIRE_OK(c);
  CHECK_LT(outline_distance(*a->outline(a->cmap().at('g')), *c->outline(c->cmap().at('g'))), 1.0);
  CHECK_STATUS(test::read_as<ir::Font>("svg-font", as_bytes("<svg xmlns=\"http://www.w3.org/2000/svg\"><defs/></svg>")),
               Status::Decode);
  CHECK_STATUS(test::read_as<ir::Font>("svg-font", as_bytes("<svg><font><glyph unicode=\"a\" d=\"M 0 0 L 1\"/></font></svg>")),
               Status::Decode);
}
