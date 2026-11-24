// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/cff.hpp"
#include "families/font/font_test_util.hpp"
#include "families/font/raster.hpp"
#include "support/metrics.hpp"
#include "support/test.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace cvt::font;
using namespace cvt::font::testing;

namespace {

void check_same_metrics(const ir::Font& a, const ir::Font& b) {
  auto ma = parse_hmtx(a), mb = parse_hmtx(b);
  REQUIRE_OK(ma);
  REQUIRE_OK(mb);
  REQUIRE_EQ(ma->size(), mb->size());
  for (std::size_t g = 0; g < ma->size(); ++g) CHECK_EQ((*ma)[g].advance, (*mb)[g].advance);
  auto ca = parse_cmap(a.find("cmap")->bytes, num_glyphs(a));
  auto cb = parse_cmap(b.find("cmap")->bytes, num_glyphs(b));
  REQUIRE_OK(ca);
  REQUIRE_OK(cb);
  CHECK(*ca == *cb);
}

}  // namespace

TEST_CASE("font/otf: decodes fontTools' CFF font and matches its cubic outlines") {
  const ir::Font font = load("otf", "inter_subset.otf");
  REQUIRE(font.find("CFF ") != nullptr);
  const Json ref = reference("inter_subset_otf_ref.json");
  auto source = GlyphSource::open(font);
  REQUIRE_OK(source);
  for (const auto& member : ref["glyphs"].items()) {
    const Json& glyph = member.value();
    INFO(member.key());
    const auto number = [](const Json& v) { return v.is_string() ? util::parse_double(v.as_string()).value_or(-1) : v.as_double(); };
    const auto gid = static_cast<std::uint32_t>(number(glyph["gid"]));
    CHECK_EQ(source->advance(gid), number(glyph["advance"]));
    // The reference lists the pen commands as text; compare the points in order.
    auto path = source->outline(gid);
    REQUIRE_OK(path);
    std::vector<double> numbers;
    for (const Json& command : glyph["commands"].elements()) {
      for (const Json& point : command[1].elements()) {
        numbers.push_back(point[0].as_double());
        numbers.push_back(point[1].as_double());
      }
    }
    std::vector<double> ours;
    for (const Point& p : path->points) {
      ours.push_back(p.x);
      ours.push_back(p.y);
    }
    CHECK_EQ(ours.size(), numbers.size());
    for (std::size_t i = 0; i < std::min(ours.size(), numbers.size()); ++i) CHECK_APPROX(ours[i], numbers[i], 1e-6);
  }
}

TEST_CASE("font/otf: CFF2 outlines equal the CFF outlines they were made from") {
  const ir::Font cff = load("otf", "inter_subset.otf");
  const ir::Font cff2 = load("otf", "inter_subset_cff2.otf");
  REQUIRE(cff2.find("CFF2") != nullptr);
  std::size_t glyphs = 0;
  CHECK_LT(max_outline_deviation(cff, cff2, &glyphs), 1e-6);
  CHECK_EQ(glyphs, 153u);
  // otf -> otf keeps CFF2 as it is (container conversion).
  const test::Converted kept = test::convert(test::fixture("font/inter_subset_cff2.otf"), "otf", "otf", R"({"strip_metadata":false})");
  REQUIRE(kept.ok());
  CHECK(read_bytes("otf", kept.data).find("CFF2") != nullptr);
}

TEST_CASE("font/ttf->otf: quadratic to cubic conversion is exact up to rounding") {
  const ir::Font ttf = load("ttf", "inter_subset.ttf");
  for (const char* options : {"{}", R"({"exact_curves":true})"}) {
    INFO(options);
    const test::Converted out = test::convert(test::fixture("font/inter_subset.ttf"), "ttf", "otf", options);
    REQUIRE(out.ok());
    CHECK(checksums_valid(out.data));
    const ir::Font otf = read_bytes("otf", out.data);
    REQUIRE(otf.find("CFF ") != nullptr);
    CHECK(otf.find("glyf") == nullptr);
    std::size_t compared = 0;
    const double deviation = max_outline_deviation(ttf, otf, &compared);
    MESSAGE("ttf->otf ", options, ": max deviation ", deviation, " font units over ", compared, " glyphs");
    CHECK_LT(deviation, std::string_view(options) == "{}" ? 1.0 : 0.02);
    check_same_metrics(ttf, otf);
    // Layout tables are kept verbatim (glyph ids did not change).
    for (const char* tag : {"GSUB", "GPOS", "GDEF"}) CHECK(otf.find(tag)->bytes == ttf.find(tag)->bytes);
    CHECK_EQ(glyph_names(otf)[1], std::string("A"));
  }
}

TEST_CASE("font/otf->ttf: cubic to quadratic approximation stays within the tolerance") {
  const ir::Font otf = load("otf", "inter_subset.otf");
  std::size_t previous_size = 0;
  for (const double tolerance : {0.25, 1.0, 4.0}) {
    INFO("tolerance ", tolerance);
    const test::Converted out = test::convert(test::fixture("font/inter_subset.otf"), "otf", "ttf",
                                              std::format(R"({{"curve_tolerance":{}}})", tolerance));
    REQUIRE(out.ok());
    const ir::Font ttf = read_bytes("ttf", out.data);
    REQUIRE(ttf.find("glyf") != nullptr);
    const double deviation = max_outline_deviation(otf, ttf);
    MESSAGE("otf->ttf tolerance ", tolerance, ": max deviation ", deviation, ", ", out.data.size(), " bytes");
    CHECK_LT(deviation, tolerance + 1.0);  // + rounding to integer coordinates
    check_same_metrics(otf, ttf);
    if (previous_size) CHECK_LE(out.data.size(), previous_size);
    previous_size = out.data.size();
  }
  // CFF2 sources convert as well (instantiated at the default).
  const test::Converted from_cff2 = test::convert(test::fixture("font/inter_subset_cff2.otf"), "otf", "ttf");
  REQUIRE(from_cff2.ok());
  CHECK_LT(max_outline_deviation(otf, read_bytes("ttf", from_cff2.data)), 2.0);
}

TEST_CASE("font/variable: outline conversion instantiates the default and says so") {
  const test::Converted out = test::convert(test::fixture("font/tiny_variable.ttf"), "ttf", "otf");
  REQUIRE(out.ok());
  bool warned = false;
  for (const std::string& w : out.warnings) warned |= w.find("default axis values") != std::string::npos;
  CHECK(warned);
  const ir::Font otf = read_bytes("otf", out.data);
  CHECK(otf.find("fvar") == nullptr);
  CHECK(otf.find("gvar") == nullptr);
  CHECK_LT(max_outline_deviation(load("ttf", "tiny_variable.ttf"), otf), 1.0);
}

TEST_CASE("font/raster: our rasteriser matches FreeType's renders (SSIM)") {
  const ir::Font font = load("ttf", "inter_subset.ttf");
  const ir::Font otf = load("otf", "inter_subset.otf");
  const Json ref = reference("inter_subset_ref.json");
  for (const Json& item : ref["renders"]["items"].elements()) {
    const std::string file = item["file"].as_string();
    INFO(file);
    auto png = test::read_as<ir::Image>("png", test::fixture("font/" + file));
    if (!png) SKIP("png reader not available");
    const auto width = static_cast<std::uint32_t>(item["canvas"][0].as_int64());
    const auto height = static_cast<std::uint32_t>(item["canvas"][1].as_int64());
    const double size = item["size_px"].as_double();
    const double baseline = item["baseline_y"].as_double();
    for (const ir::Font* f : {&font, &otf}) {
      // Place each glyph at FreeType's (hinted, rounded) pen position.
      auto source = GlyphSource::open(*f);
      REQUIRE_OK(source);
      Rasterizer r(width, height);
      for (const Json& pen : item["pen_positions"].elements()) {
        const std::u32string ch = util::utf8_to_utf32(pen[0].as_string());
        const auto it = source->cmap().find(ch[0]);
        auto path = source->outline(it == source->cmap().end() ? 0 : it->second);
        REQUIRE_OK(path);
        r.add(*path, size / source->units_per_em(), pen[1].as_double(), baseline);
      }
      const ir::Frame ours = coverage_frame(r.coverage(), width, height);
      const double score = test::ssim(ours, png->frames[0]);
      MESSAGE(file, f == &font ? " (TrueType)" : " (CFF)", ": SSIM against FreeType ", score);
      CHECK_GT(score, 0.80);
    }
  }
}

TEST_CASE("font/curves: cubic-to-quadratic splitting respects the tolerance") {
  const Point p0{0, 0}, p1{0, 1000}, p2{1000, 1000}, p3{1000, 0};
  Path cubic;
  cubic.move_to(p0);
  cubic.cubic_to(p1, p2, p3);
  cubic.close();
  for (const double tolerance : {0.1, 1.0, 10.0}) {
    const Path quad = cubic.to_quadratic(tolerance);
    CHECK_LT(outline_distance(cubic, quad), tolerance + 0.01);
  }
  CHECK_GT(cubic.to_quadratic(0.1).points.size(), cubic.to_quadratic(10.0).points.size());
  // Quadratic -> cubic is exact.
  Path quad;
  quad.move_to({0, 0});
  quad.quad_to({500, 1000}, {1000, 0});
  quad.close();
  CHECK_LT(outline_distance(quad, quad.to_cubic()), 0.01);
  CHECK_LT(outline_distance(quad, quad.reversed()), 0.01);
}
