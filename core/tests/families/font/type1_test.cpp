// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/font_test_util.hpp"
#include "support/test.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace cvt::font;
using namespace cvt::font::testing;

namespace {

// Glyph outline of a character in each font, compared geometrically.
double char_deviation(const ir::Font& a, const ir::Font& b, std::u32string_view text, double* advance_diff = nullptr) {
  auto sa = GlyphSource::open(a);
  auto sb = GlyphSource::open(b);
  if (!sa || !sb) return 1e9;
  double worst = 0;
  for (const char32_t c : text) {
    const auto ia = sa->cmap().find(c), ib = sb->cmap().find(c);
    if (ia == sa->cmap().end() || ib == sb->cmap().end()) return 1e9;
    auto pa = sa->outline(ia->second), pb = sb->outline(ib->second);
    if (!pa || !pb) return 1e9;
    const double scale = sa->units_per_em() / sb->units_per_em();
    pb->transform(scale, 0, 0, scale, 0, 0);
    worst = std::max(worst, outline_distance(*pa, *pb));
    if (advance_diff) *advance_diff = std::max(*advance_diff, std::abs(sa->advance(ia->second) - sb->advance(ib->second) * scale));
  }
  return worst;
}

constexpr std::u32string_view sample = U"AOgaHamburgefonstiv0123&@À";

}  // namespace

TEST_CASE("font/pfb: FontForge Type 1 fonts decode to the same outlines as the OpenType source") {
  const ir::Font otf = load("otf", "inter_subset.otf");
  for (const char* format : {"pfb", "pfa"}) {
    INFO(format);
    const ir::Font type1 = load(format, std::string("inter_subset.") + format);
    REQUIRE(type1.find("CFF ") != nullptr);
    REQUIRE(type1.names.has_value());
    CHECK_EQ(type1.names->family, std::string("Inter"));
    CHECK_EQ(type1.names->postscript_name, std::string("Inter-Regular"));
    CHECK_EQ(units_per_em(type1), 2048);
    double advance = 0;
    const double deviation = char_deviation(otf, type1, sample, &advance);
    MESSAGE(format, ": max outline deviation from the OTF ", deviation, " units, advance difference ", advance);
    CHECK_LT(deviation, 1.5);  // FontForge rounds Type 1 coordinates to integers
    CHECK_LE(advance, 1.0);
    // The licence notice survives.
    auto records = parse_name(type1.find("name")->bytes);
    REQUIRE_OK(records);
    CHECK(best_name(*records, 0).find("Inter Project Authors") != std::string::npos);
  }
}

TEST_CASE("font/pfb: Type 1 output round trips and converts on to TrueType") {
  const ir::Font otf = load("otf", "inter_subset.otf");
  for (const char* format : {"pfa", "pfb"}) {
    INFO(format);
    auto written = test::write_from(format, otf);
    REQUIRE_OK(written);
    if (std::string_view(format) == "pfb") CHECK_EQ(std::to_integer<int>((*written)[0]), 0x80);
    else CHECK(to_string(ByteView(*written)).starts_with("%!PS-AdobeFont-1.0: Inter-Regular"));
    const ir::Font back = read_bytes(format, *written);
    REQUIRE(!back.tables.empty());
    CHECK_LT(char_deviation(otf, back, sample), 1.0);
    const test::Converted ttf = test::convert(*written, format, "ttf");
    REQUIRE(ttf.ok());
    CHECK_LT(char_deviation(otf, read_bytes("ttf", ttf.data), sample), 2.0);
  }
  // TrueType sources go through the cubic conversion first.
  const test::Converted from_ttf = test::convert(test::fixture("font/inter_subset.ttf"), "ttf", "pfb");
  REQUIRE(from_ttf.ok());
  CHECK_LT(char_deviation(load("ttf", "inter_subset.ttf"), read_bytes("pfb", from_ttf.data), sample), 1.0);
}

TEST_CASE("font/cff: a bare CFF table becomes a complete OpenType font") {
  const ir::Font otf = load("otf", "inter_subset.otf");
  const ir::Font cff = load("cff", "inter_subset.cff");
  REQUIRE(cff.find("CFF ") != nullptr);
  CHECK(cff.find("CFF ")->bytes == test::fixture("font/inter_subset.cff"));
  CHECK_LT(char_deviation(otf, cff, sample), 1e-6);
  const test::Converted out = test::convert(test::fixture("font/inter_subset.cff"), "cff", "otf");
  REQUIRE(out.ok());
  CHECK(checksums_valid(out.data));
  const test::Converted back = test::convert(out.data, "otf", "cff");
  REQUIRE(back.ok());
  CHECK(back.data == test::fixture("font/inter_subset.cff"));
}

TEST_CASE("font/afm: metrics are read as data and written from fonts") {
  auto data = test::read_as<ir::Data>("afm", test::fixture("font/inter_subset.afm"));
  REQUIRE_OK(data);
  CHECK_EQ(data->value["FontName"].as_string(), std::string("Inter-Regular"));
  CHECK_EQ(data->value["characters"].size(), 153u);
  CHECK_EQ(data->value["characters"][0]["name"].as_string(), std::string("space"));
  CHECK_EQ(data->value["characters"][0]["width"].as_double(), 281.0);
  CHECK_GT(data->value["kerning"].size(), 1000u);
  const test::Converted afm = test::convert(test::fixture("font/inter_subset.ttf"), "ttf", "afm");
  REQUIRE(afm.ok());
  auto ours = test::read_as<ir::Data>("afm", afm.data);
  REQUIRE_OK(ours);
  // Same advance for 'A' as FontForge's AFM (both scaled to 1000 units per em).
  double fontforge = 0, mine = 0;
  for (const Json& c : data->value["characters"].elements()) {
    if (c["name"].as_string() == "A") fontforge = c["width"].as_double();
  }
  for (const Json& c : ours->value["characters"].elements()) {
    if (c["name"].as_string() == "A") mine = c["width"].as_double();
  }
  CHECK_GT(fontforge, 0.0);
  CHECK_APPROX(mine, fontforge, 1.0);
}

TEST_CASE("font/type1: damaged Type 1 data fails cleanly") {
  CHECK_STATUS(test::read_as<ir::Font>("pfa", as_bytes("%!PS-AdobeFont-1.0: X\n/FontName /X def\n")), Status::Decode);
  Bytes pfb = test::fixture("font/inter_subset.pfb");
  pfb[1] = std::byte{9};  // unknown segment type
  CHECK_STATUS(test::read_as<ir::Font>("pfb", pfb), Status::Decode);
  // A subroutine calling itself forever, inside a well-formed font.
  std::string priv = "dup /Private 5 dict dup begin /lenIV -1 def /Subrs 1 array\ndup 0 2 RD ";
  priv += std::string("\x8b\x0a", 2) + " NP\nND\n2 index /CharStrings 2 dict dup begin\n/.notdef 5 RD ";
  priv += std::string("\x8b\xf7\x8c\x0d\x0e", 5) + " ND\n/A 2 RD " + std::string("\x8b\x0a", 2) + " ND\nend\nend\n";
  Bytes plain(4, std::byte{0});
  append(plain, priv);
  std::uint16_t r = 55665;
  std::string hex;
  for (const std::byte b : plain) {
    const auto c = static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(b) ^ (r >> 8));
    r = static_cast<std::uint16_t>((c + r) * 52845u + 22719u);
    hex += std::format("{:02x}", c);
  }
  const std::string font = "%!PS-AdobeFont-1.0: L\n/FontName /L def\n/Encoding StandardEncoding def\ncurrentfile eexec\n" + hex +
                           "\n0000000000000000\ncleartomark\n";
  CHECK_STATUS(test::read_as<ir::Font>("pfa", as_bytes(font)), Status::Decode);
}
