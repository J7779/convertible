// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/font_test_util.hpp"
#include "support/test.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace cvt::font;
using namespace cvt::font::testing;

TEST_CASE("font/woff: fontTools WOFF decodes to the original tables bit for bit") {
  const ir::Font original = load("ttf", "inter_subset.ttf");
  const ir::Font woff = load("woff", "inter_subset.woff");
  REQUIRE(!woff.tables.empty());
  CHECK(table_differences(original, woff).empty());
  const test::Converted ttf = test::convert(test::fixture("font/inter_subset.woff"), "woff", "ttf",
                                            R"({"strip_metadata":false})");
  REQUIRE(ttf.ok());
  CHECK(checksums_valid(ttf.data));
  CHECK(table_differences(original, read_bytes("ttf", ttf.data), {"glyf", "loca"}).empty());
}

TEST_CASE("font/woff: metadata and private blocks are read and written") {
  const ir::Font font = load("woff", "inter_meta.woff");
  REQUIRE(!font.tables.empty());
  CHECK(to_string(ByteView(font.woff_metadata)).find("<text lang=\"en\">Test font</text>") != std::string::npos);
  CHECK_EQ(to_string(ByteView(font.woff_private)), std::string("private-test"));
  auto kept = test::write_from("woff", font, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  const ir::Font back = read_bytes("woff", *kept);
  CHECK(back.woff_metadata == font.woff_metadata);
  CHECK(back.woff_private == font.woff_private);
  CHECK(table_differences(font, back).empty());
  // Privacy default: the private block goes, the metadata (licence) stays.
  auto stripped = test::write_from("woff", font);
  REQUIRE_OK(stripped);
  const ir::Font clean = read_bytes("woff", *stripped);
  CHECK(clean.woff_private.empty());
  CHECK(clean.woff_metadata == font.woff_metadata);
  // Metadata supplied as an option.
  auto with_meta = test::write_from("woff", load("ttf", "inter_subset.ttf"),
                                    R"({"woff_metadata":"<?xml version=\"1.0\"?><metadata version=\"1.0\"/>"})");
  REQUIRE_OK(with_meta);
  CHECK(to_string(ByteView(read_bytes("woff", *with_meta).woff_metadata)).starts_with("<?xml"));
}

TEST_CASE("font/woff: our WOFF round trips and level 0 stores tables") {
  const ir::Font original = load("ttf", "inter_subset.ttf");
  auto packed = test::write_from("woff", original, R"({"strip_metadata":false})");
  auto stored = test::write_from("woff", original, R"({"strip_metadata":false,"compression_level":0})");
  REQUIRE_OK(packed);
  REQUIRE_OK(stored);
  CHECK_LT(packed->size(), stored->size());
  CHECK(table_differences(original, read_bytes("woff", *packed), {"glyf", "loca"}).empty());
  CHECK(table_differences(original, read_bytes("woff", *stored), {"glyf", "loca"}).empty());
}

namespace {

void check_woff2_matches(const ir::Font& original, const ir::Font& decoded) {
  // glyf/loca are rebuilt by the WOFF2 transform; everything else is exact.
  const std::vector<std::string> diffs = table_differences(original, decoded, {"glyf", "loca", "head"});
  INFO("differing tables: ", util::join(diffs, ", "));
  // head: only checkSumAdjustment and indexToLocFormat may change (WOFF2 rebuilds loca).
  Bytes ha = original.find("head")->bytes, hb = decoded.find("head")->bytes;
  for (Bytes* h : {&ha, &hb}) {
    put_u32(*h, 8, 0);
    put_u16(*h, 50, 0);
    put_u16(*h, 16, static_cast<std::uint16_t>(get_u16(*h, 16) & ~0x0800));  // flag 11: "transformed by WOFF2"
  }
  CHECK(ha == hb);
  CHECK(diffs.empty());
  CHECK_LT(max_outline_deviation(original, decoded), 1e-9);
  auto a = glyph_slices(original), b = glyph_slices(decoded);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  REQUIRE_EQ(a->size(), b->size());
}

}  // namespace

TEST_CASE("font/woff2: Google's and fontTools' encoders decode to the original font") {
  const ir::Font original = load("ttf", "inter_subset.ttf");
  for (const char* file : {"inter_subset.woff2", "inter_subset_ft.woff2"}) {
    INFO(file);
    const ir::Font decoded = load("woff2", file);
    REQUIRE(!decoded.tables.empty());
    check_woff2_matches(original, decoded);
  }
  // The hmtx transform (side bearings rebuilt from glyph boxes).
  const ir::Font mono = load("ttf", "jbmono_subset.ttf");
  const ir::Font transformed = load("woff2", "jbmono_hmtx.woff2");
  check_woff2_matches(mono, transformed);
}

TEST_CASE("font/woff2: collections decode to every face") {
  const ir::Font pair = load("woff2", "pair.woff2");
  REQUIRE_EQ(pair.other_faces.size(), 1u);
  check_woff2_matches(load("ttf", "inter_subset.ttf"), pair);
  check_woff2_matches(load("ttf", "jbmono_subset.ttf"), pair.other_faces[0]);
  const ir::Font second = load("woff2", "pair.woff2", R"({"face":1})");
  check_woff2_matches(load("ttf", "jbmono_subset.ttf"), second);
  // And our encoder writes collections too.
  const test::Converted out = test::convert(test::fixture("font/pair.ttc"), "ttc", "woff2", R"({"strip_metadata":false})");
  REQUIRE(out.ok());
  const ir::Font back = read_bytes("woff2", out.data);
  REQUIRE_EQ(back.other_faces.size(), 1u);
  check_woff2_matches(load("ttf", "jbmono_subset.ttf"), back.other_faces[0]);
}

TEST_CASE("font/woff2: our encoder round trips, transforms help and the size is close to Google's") {
  const ir::Font original = load("ttf", "inter_subset.ttf");
  auto transformed = test::write_from("woff2", original, R"({"strip_metadata":false})");
  auto plain = test::write_from("woff2", original, R"({"strip_metadata":false,"woff2_transform":false})");
  auto woff = test::write_from("woff", original, R"({"strip_metadata":false})");
  REQUIRE_OK(transformed);
  REQUIRE_OK(plain);
  REQUIRE_OK(woff);
  check_woff2_matches(original, read_bytes("woff2", *transformed));
  // Without transforms every table comes back bit for bit.
  CHECK(table_differences(original, read_bytes("woff2", *plain)).empty());
  CHECK_LT(transformed->size(), plain->size());
  CHECK_LT(transformed->size(), woff->size());
  const std::size_t google = test::fixture("font/inter_subset.woff2").size();
  MESSAGE("WOFF2 size: ours ", transformed->size(), " bytes, Google woff2_compress ", google, " bytes");
  CHECK_LT(static_cast<double>(transformed->size()), google * 1.06);
  auto fast = test::write_from("woff2", original, R"({"compression_level":1})");
  REQUIRE_OK(fast);
  CHECK_GE(fast->size(), transformed->size());
  // hmtx transform for monospaced fonts.
  const ir::Font mono = load("ttf", "jbmono_subset.ttf");
  auto mono_out = test::write_from("woff2", mono, R"({"strip_metadata":false})");
  REQUIRE_OK(mono_out);
  check_woff2_matches(mono, read_bytes("woff2", *mono_out));
}

TEST_CASE("font/woff2: hostile headers are rejected") {
  const Bytes valid = test::fixture("font/inter_subset.woff2");
  Bytes bad = valid;
  put_u16(bad, 12, 0);  // no tables
  CHECK_STATUS(test::read_as<ir::Font>("woff2", bad), Status::Decode);
  Bytes truncated(valid.begin(), valid.begin() + 60);
  CHECK_STATUS(test::read_as<ir::Font>("woff2", truncated), Status::Decode);
  // A decompression bomb is stopped by the limits.
  Limits limits;
  limits.max_decompressed_bytes = 1000;
  CHECK_STATUS(test::read_as<ir::Font>("woff2", valid, {}, nullptr, limits), Status::Limit);
}

TEST_CASE("font/eot: ttf2eot-style EOT decodes; our EOT versions and XOR round trip") {
  const ir::Font original = load("ttf", "inter_subset.ttf");
  const ir::Font eot = load("eot", "inter_subset.eot");
  REQUIRE(!eot.tables.empty());
  CHECK(table_differences(original, eot).empty());
  for (const char* options : {R"({"eot_version":"1","strip_metadata":false})", R"({"eot_version":"2.1","strip_metadata":false})",
                              R"({"eot_version":"2.2","eot_obfuscate":true,"strip_metadata":false})"}) {
    INFO(options);
    auto written = test::write_from("eot", original, options);
    REQUIRE_OK(written);
    const ir::Font back = read_bytes("eot", *written);
    CHECK(table_differences(original, back, {"glyf", "loca"}).empty());
    CHECK_EQ(get_u16(*written, 34), 0x4C50);  // 'LP' magic, little endian
  }
  // MicroType Express compressed EOTs are reported clearly.
  Bytes compressed = test::fixture("font/inter_subset.eot");
  compressed[12] = std::byte{4};
  const auto result = test::read_as<ir::Font>("eot", compressed);
  CHECK_STATUS(result, Status::Decode);
  if (!result) CHECK(result.error().message.find("MicroType") != std::string::npos);
}

TEST_CASE("font/dfont: FontForge suitcase decodes; our suitcases round trip") {
  const ir::Font font = load("dfont", "inter_subset.dfont");
  REQUIRE(!font.tables.empty());
  CHECK_EQ(num_glyphs(font), 155);
  REQUIRE(font.names.has_value());
  CHECK_EQ(font.names->family, std::string("Inter"));
  const test::Converted out = test::convert(test::fixture("font/pair.ttc"), "ttc", "dfont", R"({"strip_metadata":false})");
  REQUIRE(out.ok());
  const ir::Font back = read_bytes("dfont", out.data);
  REQUIRE_EQ(back.other_faces.size(), 1u);
  CHECK(table_differences(load("ttf", "jbmono_subset.ttf"), back.other_faces[0], {"glyf", "loca"}).empty());
}

TEST_CASE("font/variable: containers pass variation tables through unchanged") {
  const ir::Font variable = load("ttf", "tiny_variable.ttf");
  REQUIRE(variable.find("fvar") != nullptr);
  REQUIRE(variable.find("gvar") != nullptr);
  const ir::Font google = load("woff2", "tiny_variable.woff2");
  const std::vector<std::string> diffs = table_differences(variable, google, {"glyf", "loca", "head"});
  INFO("differing tables: ", util::join(diffs, ", "));
  CHECK(diffs.empty());
  for (const char* target : {"woff", "woff2", "ttc"}) {
    INFO(target);
    const test::Converted out = test::convert(test::fixture("font/tiny_variable.ttf"), "ttf", target, R"({"strip_metadata":false})");
    REQUIRE(out.ok());
    const ir::Font back = read_bytes(target, out.data);
    CHECK(back.find("gvar")->bytes == variable.find("gvar")->bytes);
    CHECK(back.find("fvar")->bytes == variable.find("fvar")->bytes);
  }
}
