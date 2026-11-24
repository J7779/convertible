// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/colour.hpp"
#include "families/science/science_test.hpp"

using namespace cvt;
using namespace cvt::science::colour;
using test::science::detected;

namespace {

Json palette(const std::string& format, const std::string& file, std::string_view options = {}) {
  const auto data = test::read_as<ir::Data>(format, test::fixture("science/colour/" + file), options);
  if (!data) {
    FAIL_CHECK(file, ": ", data.error().message);
    return Json();
  }
  return data->value;
}

void check_four(const Json& p, bool names) {
  const Json& c = p["colors"];
  REQUIRE(c.size() == 4);
  CHECK(c[0]["hex"].as_string() == "#ff0000");
  CHECK(c[1]["hex"].as_string() == "#008000");
  CHECK(c[2]["hex"].as_string() == "#0000ff");
  CHECK(c[3]["hex"].as_string() == "#fafad2");
  if (names) CHECK(c[3]["name"].as_string() == "Light goldenrod");
}

}  // namespace

TEST_CASE("science/colour: conversions match colour-science reference values") {
  // sRGB -> Lab, D65 and D50 (Bradford), values computed with colour-science 0.4.7.
  struct Ref {
    Vec3 rgb;
    Vec3 d50;
    Vec3 d65;
  };
  const Ref refs[] = {
      {{255, 0, 0}, {54.286557, 80.825122, 69.913395}, {53.232882, 80.111178, 67.223704}},
      {{0, 128, 0}, {46.278617, -47.553089, 48.586994}, {46.228818, -51.698579, 49.901953}},
      {{250, 250, 210}, {97.514614, -4.797158, 19.296360}, {97.368797, -6.475578, 19.240976}},
      {{40, 80, 160}, {34.710379, 8.605198, -48.509668}, {35.452293, 14.964100, -47.509724}},
  };
  for (const Ref& r : refs) {
    const Vec3 rgb = {r.rgb[0] / 255, r.rgb[1] / 255, r.rgb[2] / 255};
    const Vec3 d50 = srgb_to_lab(rgb, Illuminant::D50);
    const Vec3 d65 = srgb_to_lab(rgb, Illuminant::D65);
    for (std::size_t i = 0; i < 3; ++i) {
      // colour-science derives XYZ with the 4-decimal IEC matrix; ours uses the
      // exact primaries, so allow a few hundredths.
      CHECK_APPROX(d50[i], r.d50[i], 0.05);
      CHECK_APPROX(d65[i], r.d65[i], 0.05);
    }
    // Round trips.
    const Vec3 back = lab_to_srgb(d50, Illuminant::D50);
    for (std::size_t i = 0; i < 3; ++i) CHECK_APPROX(back[i], rgb[i], 1e-6);
    const Vec3 hsl = hsl_to_rgb(rgb_to_hsl(rgb));
    const Vec3 hsv = hsv_to_rgb(rgb_to_hsv(rgb));
    const Vec3 cmyk = cmyk_to_rgb(rgb_to_cmyk(rgb));
    const Vec3 lab = lch_to_lab(lab_to_lch(d65));
    for (std::size_t i = 0; i < 3; ++i) {
      CHECK_APPROX(hsl[i], rgb[i], 1e-12);
      CHECK_APPROX(hsv[i], rgb[i], 1e-12);
      CHECK_APPROX(cmyk[i], rgb[i], 1e-12);
      CHECK_APPROX(lab[i], d65[i], 1e-9);
    }
  }
  // Published anchors: Lindbloom's calculator gives sRGB red = Lab(D65) 53.2408, 80.0925, 67.2032;
  // HSL of #ff8000; LCh hue of D65 red.
  const Vec3 red = srgb_to_lab({1, 0, 0}, Illuminant::D65);
  CHECK_APPROX(red[0], 53.2408, 5e-3);
  CHECK_APPROX(red[1], 80.0925, 5e-3);
  CHECK_APPROX(red[2], 67.2032, 5e-3);
  const Vec3 hsl = rgb_to_hsl({1.0, 128.0 / 255, 0.0});
  CHECK_APPROX(hsl[0], 30.1176, 1e-3);
  CHECK_APPROX(lab_to_lch(srgb_to_lab({1, 0, 0}, Illuminant::D65))[2], 39.999, 0.01);
}

TEST_CASE("science/palettes: GPL, JASC, RIFF and ACT give the same four colours") {
  check_four(palette("gpl", "four.gpl"), true);
  check_four(palette("jasc-pal", "four.pal"), false);
  check_four(palette("riff-pal", "four_riff.pal"), false);
  check_four(palette("act", "four.act"), false);
  CHECK(palette("gpl", "four.gpl")["name"].as_string() == "Four colours");
  CHECK(detected(test::fixture("science/colour/four.gpl")) == "gpl");
  CHECK(detected(test::fixture("science/colour/four.pal"), "four.pal") == "jasc-pal");
  CHECK(detected(test::fixture("science/colour/four_riff.pal"), "four.pal") == "riff-pal");
  CHECK(detected(test::fixture("science/colour/four.act"), "four.act") == "act");
}

TEST_CASE("science/palettes: ACO colour spaces and names") {
  const Json p = palette("aco", "swatches.aco");
  const Json& c = p["colors"];
  REQUIRE(c.size() == 5);
  CHECK(c[0]["name"].as_string() == "pink");
  CHECK(c[0]["hex"].as_string() == "#ff0080");
  CHECK(c[1]["space"].as_string() == "HSB");
  CHECK(c[1]["hex"].as_string() == "#00ff00");
  CHECK(c[2]["space"].as_string() == "CMYK");
  CHECK(c[2]["values"] == Json::array({0, 100, 100, 0}));
  CHECK(c[2]["hex"].as_string() == "#ff0000");
  CHECK(c[3]["space"].as_string() == "LAB");
  CHECK(c[3]["values"] == Json::array({50, 20, -30}));
  // Lab 50/20/-30 (D50) -> sRGB, reference from colour-science: (119, 114, 166) approx.
  const Vec3 rgb = lab_to_srgb({50, 20, -30}, Illuminant::D50);
  CHECK(c[3]["rgb"] == Json::array({std::lround(std::clamp(rgb[0], 0.0, 1.0) * 255), std::lround(std::clamp(rgb[1], 0.0, 1.0) * 255),
                                    std::lround(std::clamp(rgb[2], 0.0, 1.0) * 255)}));
  CHECK(c[4]["hex"].as_string() == "#bfbfbf");
  // D65 interpretation changes the Lab colour only.
  const Json d65 = palette("aco", "swatches.aco", R"({"lab_white":"d65"})");
  CHECK(d65["colors"][3]["hex"] != c[3]["hex"]);
  CHECK(d65["colors"][0]["hex"] == c[0]["hex"]);
}

TEST_CASE("science/palettes: ASE written by the swatch package") {
  const Json p = palette("ase", "swatches.ase");
  const Json& c = p["colors"];
  REQUIRE(c.size() == 4);
  CHECK(c[0]["name"].as_string() == "Warm Red");
  CHECK(c[0]["hex"].as_string() == "#ff3300");
  CHECK(c[1]["group"].as_string() == "Grouped");
  CHECK(c[1]["space"].as_string() == "CMYK");
  CHECK(c[1]["hex"].as_string() == "#00ffff");
  CHECK(c[2]["space"].as_string() == "LAB");
  CHECK(c[2]["values"] == Json::array({50, 20, -30}));
  CHECK(c[2]["hex"] == palette("aco", "swatches.aco")["colors"][3]["hex"]);
  CHECK(c[3]["hex"].as_string() == "#808080");
  CHECK(detected(test::fixture("science/colour/swatches.ase")) == "ase");
}

TEST_CASE("science/palettes: every writer round trips through its reader") {
  const auto source = test::read_as<ir::Data>("ase", test::fixture("science/colour/swatches.ase"));
  REQUIRE_OK(source);
  for (const char* format : {"gpl", "jasc-pal", "riff-pal", "act", "aco", "ase", "palette-css", "palette-scss", "palette-less", "color-tokens"}) {
    INFO(format);
    const auto written = test::write_from(format, *source);
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Data>(format, *written);
    REQUIRE_OK(back);
    const Json& a = source->value["colors"];
    const Json& b = back->value["colors"];
    REQUIRE(b.size() >= a.size());
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(b[i]["hex"] == a[i]["hex"]);
    const std::string_view f(format);
    if (f == "aco" || f == "ase") {
      for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(b[i]["space"] == a[i]["space"]);
        CHECK(b[i]["name"] == a[i]["name"]);
      }
    }
  }
  const auto css = test::write_from("palette-css", *source);
  REQUIRE_OK(css);
  CHECK(test::str(*css) == ":root {\n  --warm-red: #ff3300;\n  --cyan-ink: #00ffff;\n  --lab-mid: " +
                               source->value["colors"][2]["hex"].as_string() + ";\n  --half-grey: #808080;\n}\n");
  // Generic JSON shapes become palettes too.
  const auto json = util::parse_json(R"j({"brand":{"primary":{"$type":"color","$value":"#112233"}},"accent":"hsl(120, 100%, 50%)"})j");
  REQUIRE_OK(json);
  const auto gpl = test::write_from("gpl", ir::Data{*json});
  REQUIRE_OK(gpl);
  CHECK(test::str(*gpl).find(" 17  34  51\tbrand.primary") != std::string::npos);
  CHECK(test::str(*gpl).find("  0 255   0\taccent") != std::string::npos);
}

TEST_CASE("science/palettes: swatch sheets, table view and palettes from pictures") {
  const auto sheet = test::read_as<ir::Image>("gpl", test::fixture("science/colour/four.gpl"), R"({"swatch_size":8,"swatch_columns":2})");
  REQUIRE_OK(sheet);
  CHECK(sheet->width() == 16);
  CHECK(sheet->height() == 16);
  CHECK(sheet->frames[0].rgba[sheet->frames[0].offset(9, 9)] == 250);
  const auto table = test::read_as<ir::Table>("gpl", test::fixture("science/colour/four.gpl"));
  REQUIRE_OK(table);
  CHECK(test::science::cell(*table, 2, 1) == "#008000");
  const auto from_table = test::write_from("act", *table);
  REQUIRE_OK(from_table);
  CHECK(from_table->size() == 772);
  // Two-colour picture -> two dominant colours, most frequent first.
  ir::Image picture;
  picture.frames.push_back(ir::Frame::filled(10, 10, {10, 200, 30, 255}));
  for (std::size_t i = 0; i < 30 * 4; i += 4) {
    picture.frames[0].rgba[i] = 250;
    picture.frames[0].rgba[i + 1] = 5;
    picture.frames[0].rgba[i + 2] = 5;
  }
  const auto extracted = test::write_from("gpl", picture, R"({"palette_colors":4})");
  REQUIRE_OK(extracted);
  const auto back = test::read_as<ir::Data>("gpl", *extracted);
  REQUIRE_OK(back);
  REQUIRE(back->value["colors"].size() == 2);
  CHECK(back->value["colors"][0]["hex"].as_string() == "#0ac81e");
  CHECK(back->value["colors"][1]["hex"].as_string() == "#fa0505");
}

TEST_CASE("science/icc: summarises an lcms sRGB profile") {
  const auto data = test::read_as<ir::Data>("icc", test::fixture("science/colour/srgb_lcms.icc"));
  REQUIRE_OK(data);
  const Json& h = data->value["header"];
  CHECK(h["device_class"].as_string() == "mntr");
  CHECK(h["color_space"].as_string() == "RGB");
  CHECK(h["pcs"].as_string() == "XYZ");
  CHECK(h["created"].as_string() == "2000-01-01T00:00:00");
  const Json& t = data->value["tags"];
  CHECK(t["desc"].as_string() == "sRGB built-in");
  CHECK_APPROX(t["wtpt"][0].as_double(), 0.9642, 1e-3);
  CHECK_APPROX(t["rXYZ"][0].as_double(), 0.4361, 1e-3);
  CHECK(t["rTRC"]["type"].as_string() == "para");
  CHECK_APPROX(t["rTRC"]["parameters"][0].as_double(), 2.4, 1e-4);
  CHECK(detected(test::fixture("science/colour/srgb_lcms.icc")) == "icc");
}

TEST_CASE("science/palettes: hostile input") {
  for (const auto& [format, name] : std::vector<std::pair<std::string, std::string>>{
           {"aco", "swatches.aco"}, {"ase", "swatches.ase"}, {"riff-pal", "four_riff.pal"}, {"gpl", "four.gpl"},
           {"jasc-pal", "four.pal"}, {"icc", "srgb_lcms.icc"}}) {
    CHECK(test::fuzz_reader<ir::Data>(format, test::fixture("science/colour/" + name)) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Data>("act", as_bytes("short")), Status::Decode);
}
