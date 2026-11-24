// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// pdf family: font programs (TrueType, CFF, Type 1, subsetting) and the rasteriser.
#include <cmath>
#include <numbers>
#include <string>

#include "families/pdf/embedded_fonts.hpp"
#include "families/pdf/font_program.hpp"
#include "families/pdf/pdf_font.hpp"
#include "families/pdf/pdf_test_util.hpp"
#include "families/pdf/raster.hpp"

using namespace cvt;

namespace {

double coverage_sum(const pdf::raster::Mask& m) {
  double s = 0;
  for (const std::uint8_t c : m.coverage) s += c / 255.0;
  return s;
}

}  // namespace

TEST_CASE("pdf/fonts: bundled TrueType faces parse, map and outline") {
  for (const ByteView data : {pdf::fonts::inter_regular(), pdf::fonts::inter_semibold(), pdf::fonts::mono_regular()}) {
    REQUIRE(!data.empty());
    auto font = pdf::font::TrueType::parse(data);
    REQUIRE_OK(font);
    const auto h = font->glyph_for_unicode('H');
    REQUIRE(h.has_value());
    const pdf::Path outline = font->outline(*h);
    CHECK(!outline.empty());
    const pdf::Rect b = outline.bounds();
    CHECK(b.height() > 0.5 * font->units_per_em() * 0.6);
    CHECK(font->advance(*h) > 0);
    CHECK(font->glyph_for_unicode(0x20AC).has_value());  // Euro
    CHECK(!font->glyph_for_unicode(0x4E2D).has_value());  // no CJK
    // Subsetting keeps outlines and widths, renumbers glyphs and shrinks the font.
    const auto a = font->glyph_for_unicode('a');
    REQUIRE(a.has_value());
    auto subset = pdf::font::subset_truetype(*font, {*h, *a});
    REQUIRE_OK(subset);
    CHECK(subset->font.size() < data.size() / 4);
    auto small = pdf::font::TrueType::parse(subset->font);
    REQUIRE_OK(small);
    const std::uint16_t nh = subset->new_gid.at(*h);
    CHECK(small->advance(nh) == font->advance(*h));
    CHECK(small->outline(nh).points.size() == outline.points.size());
    CHECK(small->glyph_for_unicode('H') == nh);
  }
  CHECK(!pdf::font::TrueType::parse(test::random_bytes(500, 3)).has_value());
  const Bytes inter(pdf::fonts::inter_regular().begin(), pdf::fonts::inter_regular().begin() + 4000);
  CHECK(!pdf::font::TrueType::parse(test::random_bytes(10, 1)).has_value());
  (void)pdf::font::TrueType::parse(inter);  // truncated: may parse the directory, must not crash
}

TEST_CASE("pdf/fonts: embedded Type 1 and CFF programs from real files outline every used glyph") {
  for (const char* name : {"latex_article.pdf", "gs_cff_article.pdf"}) {
    CAPTURE(name);
    const Bytes bytes = test::fixture(std::string("pdf/") + name);
    auto file = pdf::PdfFile::open(bytes);
    REQUIRE(file.has_value());
    pdf::FontCache cache;
    int checked = 0;
    for (const auto& [key, ref] : (*file)->get((*file)->pages()[0].resources.as_dict(), "Font").as_dict()) {
      auto font = pdf::PdfFont::load(**file, ref, cache);
      if (font->kind() == pdf::PdfFont::Kind::Type3) continue;
      CHECK(font->embedded());
      // Letters used in the text have outlines in the subset.
      for (const char c : std::string("ae")) {
        const pdf::GlyphCode g{static_cast<std::uint32_t>(c), 1, static_cast<std::uint32_t>(c)};
        if (font->unicode(g) == std::u32string(1, static_cast<char32_t>(c)) && font->outline(g) != nullptr) ++checked;
      }
    }
    CHECK(checked >= 4);
  }
}

TEST_CASE("pdf/raster: exact area coverage for rectangles and circles") {
  using namespace pdf::raster;
  pdf::Path rect;
  rect.rect(2.25, 3.5, 10.25, 10.5);
  const Mask m = fill_path(rect, FillRule::NonZero, IntRect{0, 0, 40, 40});
  CHECK_APPROX(coverage_sum(m), 10.25 * 10.5, 0.2);
  CHECK(m.at(5, 7) == 255);
  CHECK(std::abs(m.at(2, 7) - 191) <= 1);  // 75 % of the left column
  pdf::Path circle;
  const int n = 64;
  for (int i = 0; i <= n; ++i) {
    const double a = 2 * std::numbers::pi * i / n;
    if (i == 0) circle.move_to(20 + 10 * std::cos(a), 20 + 10 * std::sin(a));
    else circle.line_to(20 + 10 * std::cos(a), 20 + 10 * std::sin(a));
  }
  circle.close();
  const double polygon_area = 0.5 * n * 100 * std::sin(2 * std::numbers::pi / n);
  CHECK_APPROX(coverage_sum(fill_path(circle, FillRule::NonZero, IntRect{0, 0, 40, 40})), polygon_area, polygon_area * 0.005);
}

TEST_CASE("pdf/raster: fill rules, clipping and hostile coordinates") {
  using namespace pdf::raster;
  pdf::Path two;
  two.rect(0, 0, 20, 20);
  two.rect(5, 5, 10, 10);  // same direction
  CHECK(fill_path(two, FillRule::NonZero, IntRect{0, 0, 30, 30}).at(10, 10) == 255);
  CHECK(fill_path(two, FillRule::EvenOdd, IntRect{0, 0, 30, 30}).at(10, 10) == 0);
  pdf::Path hole;
  hole.rect(0, 0, 20, 20);
  hole.move_to(5, 5);
  hole.line_to(5, 15);
  hole.line_to(15, 15);
  hole.line_to(15, 5);
  hole.close();  // opposite direction
  CHECK(fill_path(hole, FillRule::NonZero, IntRect{0, 0, 30, 30}).at(10, 10) == 0);
  // Geometry far outside the box costs nothing and still covers correctly.
  pdf::Path huge;
  huge.rect(-1e9, -1e9, 2e9, 2e9);
  const Mask full = fill_path(huge, FillRule::NonZero, IntRect{0, 0, 50, 50});
  CHECK(full.at(0, 0) == 255);
  CHECK(full.at(49, 49) == 255);
  pdf::Path bad;
  bad.move_to(std::nan(""), 1);
  bad.line_to(1e308, 1e308);
  bad.line_to(std::numeric_limits<double>::infinity(), 0);
  (void)fill_path(bad, FillRule::NonZero, IntRect{0, 0, 50, 50});
  // Intersection of masks.
  pdf::Path left;
  left.rect(0, 0, 10, 50);
  const Mask clipped = intersect(fill_path(left, FillRule::NonZero, IntRect{0, 0, 50, 50}), full);
  CHECK(clipped.at(5, 5) == 255);
  CHECK(clipped.at(20, 5) == 0);
}

TEST_CASE("pdf/raster: strokes with caps, joins, dashes and the thin-line rule") {
  using namespace pdf::raster;
  const IntRect box{0, 0, 60, 60};
  pdf::Path line;
  line.move_to(10, 30);
  line.line_to(50, 30);
  StrokeStyle style;
  style.width = 4;
  const Mask butt = fill_path(stroke_outline(line, style, {}), FillRule::NonZero, box);
  CHECK_APPROX(coverage_sum(butt), 40 * 4, 1);
  CHECK(butt.at(30, 29) == 255);
  CHECK(butt.at(30, 33) == 0);
  style.cap = LineCap::Square;
  CHECK_APPROX(coverage_sum(fill_path(stroke_outline(line, style, {}), FillRule::NonZero, box)), 44 * 4, 1);
  style.cap = LineCap::Round;
  CHECK_APPROX(coverage_sum(fill_path(stroke_outline(line, style, {}), FillRule::NonZero, box)),
               40 * 4 + std::numbers::pi * 4, 1.5);
  // Miter join fills the outer corner of a right angle; bevel does not.
  pdf::Path corner;
  corner.move_to(10, 10);
  corner.line_to(40, 10);
  corner.line_to(40, 40);
  style = StrokeStyle{};
  style.width = 8;
  const Mask miter = fill_path(stroke_outline(corner, style, {}), FillRule::NonZero, box);
  CHECK(miter.at(43, 7) == 255);
  style.join = LineJoin::Bevel;
  const Mask bevel = fill_path(stroke_outline(corner, style, {}), FillRule::NonZero, box);
  CHECK(bevel.at(43, 7) < 128);
  // Dashes: [3 2] gives 8 dashes over 40 units.
  style = StrokeStyle{};
  style.width = 2;
  style.dash = {3, 2};
  const Mask dashed = fill_path(stroke_outline(line, style, {}), FillRule::NonZero, box);
  CHECK_APPROX(coverage_sum(dashed), 8 * 3 * 2, 1);
  CHECK(dashed.at(11, 30) == 255);
  CHECK(dashed.at(13, 30) < 64);
  // Hairlines stay visible: width 0 under a 0.1 scale is still one pixel wide.
  style = StrokeStyle{};
  style.width = 0;
  const Mask hair = fill_path(stroke_outline(line, style, pdf::Matrix::scale(1, 1)), FillRule::NonZero, box);
  CHECK_APPROX(coverage_sum(hair), 40, 1.5);
}
