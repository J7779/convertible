// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/font_test_util.hpp"
#include "families/font/glyf.hpp"
#include "engine/detect.hpp"
#include "ir/archive.hpp"
#include "support/test.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace cvt::font;
using namespace cvt::font::testing;

TEST_CASE("font/ttf: decodes a fontTools subset of Inter and matches the reference data") {
  const ir::Font font = load("ttf", "inter_subset.ttf");
  const Json ref = reference("inter_subset_ref.json");
  REQUIRE(!font.tables.empty());
  REQUIRE(ref.is_object());
  CHECK(font.flavor == ir::FontFlavor::TrueType);
  CHECK_EQ(static_cast<std::int64_t>(num_glyphs(font)), ref["numGlyphs"].as_int64());
  CHECK_EQ(static_cast<std::int64_t>(units_per_em(font)), ref["unitsPerEm"].as_int64());
  REQUIRE(font.names.has_value());
  CHECK_EQ(font.names->family, ref["names"]["1"].as_string());
  CHECK_EQ(font.names->full_name, ref["names"]["4"].as_string());
  CHECK_EQ(font.names->postscript_name, ref["names"]["6"].as_string());

  auto cmap = parse_cmap(font.find("cmap")->bytes, num_glyphs(font));
  REQUIRE_OK(cmap);
  CHECK_EQ(cmap->size(), ref["cmap"].size());
  for (const auto& member : ref["cmap"].items()) {
    const auto code = static_cast<std::uint32_t>(std::stoul(member.key()));
    INFO("code ", code);
    REQUIRE(cmap->contains(code));
    CHECK_EQ(static_cast<std::int64_t>(cmap->at(code)), member.value()[1].as_int64());
  }
  auto metrics = parse_hmtx(font);
  REQUIRE_OK(metrics);
  const std::vector<std::string> names = glyph_names(font);
  for (std::size_t g = 0; g < metrics->size(); ++g) {
    INFO("glyph ", g);
    CHECK_EQ(static_cast<std::int64_t>((*metrics)[g].advance), ref["advances"][g].as_int64());
    CHECK_EQ(static_cast<std::int64_t>((*metrics)[g].lsb), ref["lsbs"][g].as_int64());
    const std::string& expected = ref["glyphOrder"][g].as_string();
    if (!expected.starts_with("glyph")) CHECK_EQ(names[g], expected);
  }
  // Outlines, composites flattened, point by point.
  auto slices = glyph_slices(font);
  REQUIRE_OK(slices);
  for (const auto& member : ref["glyphs"].items()) {
    const Json& glyph = member.value();
    const auto gid = static_cast<std::uint32_t>(glyph["gid"].as_int64());
    INFO(member.key());
    auto resolved = resolve_glyph(*slices, gid);
    REQUIRE_OK(resolved);
    std::size_t index = 0;
    const Json& contours = glyph["contours"];
    CHECK_EQ(resolved->end_points.size(), contours.size());
    for (const Json& contour : contours.elements()) {
      for (const Json& p : contour.elements()) {
        REQUIRE(index < resolved->points.size());
        CHECK_EQ(resolved->points[index].x, p[0].as_int64());
        CHECK_EQ(resolved->points[index].y, p[1].as_int64());
        CHECK_EQ(resolved->points[index].on_curve, p[2].as_int64() == 1);
        ++index;
      }
    }
    CHECK_EQ(index, resolved->points.size());
  }
}

TEST_CASE("font/ttf: repackaging keeps every table bit for bit with valid checksums") {
  const Bytes source = test::fixture("font/inter_subset.ttf");
  const ir::Font font = read_bytes("ttf", source);
  auto written = write_sfnt(font);
  REQUIRE_OK(written);
  CHECK(checksums_valid(*written));
  CHECK(checksums_valid(source));
  const ir::Font again = read_bytes("ttf", *written);
  CHECK(table_differences(font, again).empty());
  // Through the engine (same-format "compress" path).
  const test::Converted converted = test::convert(source, "ttf", "ttf", R"({"strip_metadata":false})");
  REQUIRE(converted.ok());
  CHECK(checksums_valid(converted.data));
  CHECK_LE(converted.data.size(), source.size());
  const ir::Font compressed = read_bytes("ttf", converted.data);
  CHECK(table_differences(font, compressed, {"glyf", "loca"}).empty());
  CHECK_LT(max_outline_deviation(font, compressed), 1e-9);
}

TEST_CASE("font/ttf: Apple 'true' fonts are read and written as standard TrueType") {
  const ir::Font mac = load("ttf", "inter_mac_true.ttf");
  REQUIRE(!mac.tables.empty());
  CHECK_EQ(mac.sfnt_version, tag_true);
  const ir::Font plain = load("ttf", "inter_subset.ttf");
  CHECK(table_differences(mac, plain).empty());
  auto written = write_sfnt(mac);
  REQUIRE_OK(written);
  CHECK_EQ(get_u32(*written, 0), tag_true_type);
}

TEST_CASE("font/ttc: reads every face, selects with 'face', writes and splits collections") {
  const Bytes source = test::fixture("font/pair.ttc");
  const ir::Font first = read_bytes("ttc", source);
  REQUIRE_EQ(first.other_faces.size(), 1u);
  const ir::Font inter = load("ttf", "inter_subset.ttf");
  const ir::Font mono = load("ttf", "jbmono_subset.ttf");
  CHECK(table_differences(first, inter).empty());
  CHECK(table_differences(first.other_faces[0], mono).empty());
  const ir::Font second = read_bytes("ttc", source, R"({"face":1})");
  CHECK(table_differences(second, mono).empty());
  CHECK_STATUS(test::read_as<ir::Font>("ttc", source, R"({"face":2})"), Status::InvalidArgument);

  const test::Converted rewritten = test::convert(source, "ttc", "ttc", R"({"strip_metadata":false})");
  REQUIRE(rewritten.ok());
  const ir::Font back = read_bytes("ttc", rewritten.data);
  REQUIRE_EQ(back.other_faces.size(), 1u);
  CHECK(table_differences(back, inter, {"glyf", "loca"}).empty());
  CHECK(table_differences(back.other_faces[0], mono, {"glyf", "loca"}).empty());
  // Shared tables are stored once: the collection is smaller than both fonts.
  CHECK_LE(rewritten.data.size(), source.size() + 64);

  // Split into one file per face.
  const auto archive = test::read_as<ir::Font>("ttc", source);
  REQUIRE_OK(archive);
  const test::Converted zip = test::convert(source, "ttc", "zip");
  if (zip.ok()) CHECK_GT(zip.data.size(), 1000u);  // archive family present
  // A single font written as ttf from the collection warns about the dropped face.
  std::vector<std::string> warnings;
  auto single = test::write_from("ttf", *archive, "", &warnings);
  REQUIRE_OK(single);
  CHECK_EQ(warnings.size(), 1u);
}

TEST_CASE("font/sfnt: the directory is validated strictly") {
  const Bytes source = test::fixture("font/inter_subset.ttf");
  {
    Bytes bad = source;
    put_u32(bad, 12 + 16 * 1 + 12, 0x7FFFFFFF);  // a table length beyond the file
    CHECK_STATUS(test::read_as<ir::Font>("ttf", bad), Status::Decode);
  }
  {
    Bytes bad = source;
    // second table starts inside the first one
    const std::uint32_t first_offset = get_u32(bad, 12 + 8);
    put_u32(bad, 12 + 16 + 8, first_offset + 2);
    CHECK_STATUS(test::read_as<ir::Font>("ttf", bad), Status::Decode);
  }
  {
    Bytes bad = source;
    put_u32(bad, 12 + 16, get_u32(bad, 12));  // duplicate tag
    CHECK_STATUS(test::read_as<ir::Font>("ttf", bad), Status::Decode);
  }
  {
    Bytes bad = source;
    put_u16(bad, 4, 0xFFFF);  // giant table count
    CHECK_STATUS(test::read_as<ir::Font>("ttf", bad), Status::Decode);
  }
  CHECK_STATUS(test::read_as<ir::Font>("ttf", as_bytes("ttcf\0\1\0\0\xFF\xFF\xFF\xFF")), Status::Decode);
}

TEST_CASE("font/sfnt: formats are detected from their content") {
  const struct {
    const char* file;
    const char* id;
  } cases[] = {{"inter_subset.ttf", "ttf"},   {"inter_subset.otf", "otf"},   {"pair.ttc", "ttc"},
               {"inter_subset.woff", "woff"}, {"inter_subset.woff2", "woff2"}, {"inter_subset.eot", "eot"},
               {"inter_subset.dfont", "dfont"}, {"inter_subset.cff", "cff"}, {"inter_subset.pfb", "pfb"},
               {"inter_subset.pfa", "pfa"},   {"inter_subset.afm", "afm"},   {"mini16.bdf", "bdf"},
               {"mini16.pcf", "pcf"},         {"mono16.psf", "psf"},         {"mini16.fnt", "fnt"},
               {"mini16.fon", "fon"},         {"mini16.otb", "otb"},         {"inter_subset.svg", "svg-font"},
               {"inter_mac_true.ttf", "ttf"}, {"inter_subset_cff2.otf", "otf"}};
  for (const auto& c : cases) {
    INFO(c.file);
    // Without a file name, from content only.
    const test::Converted detected = test::convert(test::fixture(std::string("font/") + c.file), "", "json");
    CHECK_EQ(detected.status, Status::Ok);
    const Bytes data = test::fixture(std::string("font/") + c.file);
    const auto info = detect_format(Registry::instance(), data, "");
    REQUIRE(info.has_value());
    CHECK_EQ(info->format->id, std::string(c.id));
  }
  std::vector<std::string> problems;
  for (const std::string& p : Registry::instance().validate()) {
    for (const char* id : {"'ttf'", "'otf'", "'ttc'", "'woff", "'eot'", "'dfont'", "'cff'", "'otb'", "'pfa'", "'pfb'",
                           "'afm'", "'bdf'", "'pcf'", "'psf'", "'fnt'", "'fon'", "'svg-font'"}) {
      if (p.find(id) != std::string::npos) {
        problems.push_back(p);
        break;
      }
    }
  }
  INFO(util::join(problems, "; "));
  CHECK(problems.empty());
}
