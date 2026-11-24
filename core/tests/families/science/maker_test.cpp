// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <numbers>

#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::detected;

namespace {
Bytes maker(const std::string& name) { return test::fixture("science/maker/" + name); }
}  // namespace

TEST_CASE("science/gcode: toolpath summary and preview") {
  const auto data = test::read_as<ir::Data>("gcode", maker("square.gcode"));
  REQUIRE_OK(data);
  const Json& s = data->value;
  CHECK(s["units"].as_string() == "mm");
  // Plunge 6 + 10 + 10 + the clockwise G2 from (10,10) to (0,20) around (0,10): three quarters of a circle of radius 10, + 20.
  const double expected = 6 + 10 + 10 + std::numbers::pi * 10 * 1.5 + 20;
  CHECK_APPROX(s["cutting_distance"].as_double(), expected, 0.01);
  CHECK_APPROX(s["bounds"]["min"][0].as_double(), -10.0, 1e-6);  // the arc swings left of the square
  CHECK_APPROX(s["bounds"]["min"][1].as_double(), 0.0, 1e-6);
  CHECK_APPROX(s["bounds"]["max"][1].as_double(), 20.0, 1e-9);
  CHECK_APPROX(s["bounds"]["max"][0].as_double(), 10.0, 1e-9);
  const auto image = test::read_as<ir::Image>("gcode", maker("square.gcode"), R"({"preview_size":200})");
  REQUIRE_OK(image);
  CHECK(image->height() == 200);
  // The bottom edge of the square is drawn near the bottom of the picture.
  const auto& f = image->frames[0];
  bool dark_bottom = false;
  for (std::uint32_t y = f.height - 12; y < f.height; ++y) dark_bottom = dark_bottom || f.rgba[f.offset(f.width / 2, y)] < 128;
  CHECK(dark_bottom);
  CHECK(detected(maker("square.gcode"), "square.gcode") == "gcode");
}

TEST_CASE("science/excellon and gerber: summaries and rendered layers") {
  const auto drill = test::read_as<ir::Data>("excellon", maker("drill.drl"));
  REQUIRE_OK(drill);
  CHECK(drill->value["units"].as_string() == "mm");
  CHECK(drill->value["hits"].as_int64() == 3);
  CHECK(drill->value["tools"]["T1"]["diameter"].as_double() == 0.3);
  CHECK(drill->value["tools"]["T1"]["hits"].as_int64() == 2);
  CHECK(drill->value["bounds"]["max"] == Json::array({5, 3}));
  const auto drill_image = test::read_as<ir::Image>("excellon", maker("drill.drl"), R"({"preview_size":300})");
  REQUIRE_OK(drill_image);
  const auto gerber = test::read_as<ir::Data>("gbr", maker("layer.gbr"));
  REQUIRE_OK(gerber);
  CHECK(gerber->value["flashes"].as_int64() == 3);
  CHECK(gerber->value["draws"].as_int64() == 2);
  CHECK(gerber->value["apertures"].as_int64() == 3);
  CHECK_APPROX(gerber->value["bounds"]["min"][0].as_double(), -0.5, 1e-9);  // half the 1.0 mm pad
  CHECK_APPROX(gerber->value["bounds"]["max"][1].as_double(), 3.3, 1e-9);   // via radius 0.3 above y = 3
  const auto layer = test::read_as<ir::Image>("gbr", maker("layer.gbr"), R"({"preview_size":400,"layer_color":"ff0000"})");
  REQUIRE_OK(layer);
  const auto& f = layer->frames[0];
  // The via at (2.5, 3) is red; a corner of the board is background.
  std::size_t red = 0;
  for (std::size_t i = 0; i < f.rgba.size(); i += 4) red += f.rgba[i] == 255 && f.rgba[i + 1] == 0 ? 1 : 0;
  CHECK(red > 1000);
  CHECK(f.rgba[0] == 255);
  CHECK(f.rgba[1] == 255);
  CHECK(detected(maker("layer.gbr")) == "gbr");
  CHECK(detected(maker("drill.drl")) == "excellon");
  CHECK_STATUS(test::read_as<ir::Image>("gbr", maker("layer.gbr"), R"({"layer_color":"red"})"), Status::InvalidArgument);
}

TEST_CASE("science/maker: hostile input") {
  CHECK(test::fuzz_reader<ir::Data>("gcode", maker("square.gcode")) > 0);
  CHECK(test::fuzz_reader<ir::Image>("gcode", maker("square.gcode"), 16) > 0);
  CHECK(test::fuzz_reader<ir::Data>("excellon", maker("drill.drl")) > 0);
  CHECK(test::fuzz_reader<ir::Data>("gbr", maker("layer.gbr")) > 0);
  CHECK(test::fuzz_reader<ir::Image>("gbr", maker("layer.gbr"), 16) > 0);
  CHECK_STATUS(test::read_as<ir::Data>("gbr", as_bytes("%FSLAX99Y99*%")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Image>("gcode", as_bytes("G21\nG0 X1\n"), R"({"preview_size":100000})"), Status::Decode);
}
