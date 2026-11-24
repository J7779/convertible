// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::detected;

namespace {

const Json* find_var(const Json& data, std::string_view name) {
  for (const Json& v : data["variables"].elements()) {
    if (v["name"].as_string() == name) return &v;
  }
  return nullptr;
}

void check_multi(const Json& data) {
  const Json& primary = *find_var(data, "PRIMARY");
  CHECK(primary["dtype"].as_string() == "uint16");
  CHECK(primary["shape"] == Json::array({6, 8}));
  CHECK(primary["data"][5][7].as_int64() == 7500);
  CHECK(primary["attributes"]["OBJECT"].as_string() == "test pattern");
  CHECK(primary["attributes"]["EXPTIME"].as_double() == 12.5);
  const Json& cube = *find_var(data, "CUBE");
  CHECK(cube["dtype"].as_string() == "float32");
  CHECK(cube["shape"] == Json::array({2, 3, 4}));
  CHECK(cube["data"][1][2][3].as_double() == 9.0);
  const Json& scaled = *find_var(data, "SCALED");
  CHECK(scaled["data"] == Json::array({Json::array({10.0, 10.5}), Json::array({11.0, 11.5})}));
  const Json& catalog = *find_var(data, "CATALOG");
  CHECK(catalog["dtype"].as_string() == "record");
  CHECK(catalog["data"][2]["ID"].as_int64() == 3);
  CHECK(catalog["data"][2]["FLUX"].as_double() == 1e10);
  CHECK(catalog["data"][1]["LABEL"].as_string() == "beta");
  CHECK(catalog["data"][1]["FLAG"].as_bool(true) == false);
  CHECK(catalog["data"][2]["VEC_1"].as_double() == 6.0);
  const Json& ascii = *find_var(data, "ASCII");
  CHECK(ascii["data"][1]["N"].as_int64() == -8);
  CHECK(ascii["data"][0]["X"].as_double() == 1.25);
  CHECK(ascii["data"][1]["S"].as_string() == "wxyz");
}

}  // namespace

TEST_CASE("science/fits: decodes an astropy file with images and tables") {
  const auto data = test::read_as<ir::Data>("fits", test::fixture("science/arrays/multi.fits"));
  REQUIRE_OK(data);
  check_multi(data->value);
  CHECK(detected(test::fixture("science/arrays/multi.fits")) == "fits");
}

TEST_CASE("science/fits: renders images with exact grey levels and stretches") {
  const auto image = test::read_as<ir::Image>("fits", test::fixture("science/arrays/gray8.fits"), R"({"clip_percent":0})");
  REQUIRE_OK(image);
  REQUIRE(image->width() == 3);
  REQUIRE(image->height() == 2);
  const auto& px = image->frames[0].rgba;
  // FITS stores the bottom row first; the top displayed row is data row 1.
  CHECK(px[0] == 192);
  CHECK(px[4] == 255);
  CHECK(px[8] == 32);
  CHECK(px[12] == 0);
  CHECK(px[16] == 64);
  const auto unflipped =
      test::read_as<ir::Image>("fits", test::fixture("science/arrays/gray8.fits"), R"({"clip_percent":0,"flip_vertical":false})");
  REQUIRE_OK(unflipped);
  CHECK(unflipped->frames[0].rgba[0] == 0);
  const auto sqrt = test::read_as<ir::Image>("fits", test::fixture("science/arrays/gray8.fits"),
                                             R"({"clip_percent":0,"stretch":"sqrt"})");
  REQUIRE_OK(sqrt);
  CHECK(sqrt->frames[0].rgba[16] == 128);  // sqrt(64/255) * 255
  const auto window = test::read_as<ir::Image>("fits", test::fixture("science/arrays/gray8.fits"),
                                               R"({"window_low":64,"window_high":192})");
  REQUIRE_OK(window);
  CHECK(window->frames[0].rgba[0] == 255);
  CHECK(window->frames[0].rgba[16] == 0);
  // A cube gives one frame per plane; HDU choice.
  const auto cube = test::read_as<ir::Image>("fits", test::fixture("science/arrays/multi.fits"), R"({"fits_hdu":1})");
  REQUIRE_OK(cube);
  CHECK(cube->frames.size() == 2);
  CHECK(cube->width() == 4);
  CHECK_STATUS(test::read_as<ir::Image>("fits", test::fixture("science/arrays/multi.fits"), R"({"fits_hdu":3})"), Status::Decode);
}

TEST_CASE("science/fits: writer round trips data, images and tables") {
  const auto data = test::read_as<ir::Data>("fits", test::fixture("science/arrays/multi.fits"));
  REQUIRE_OK(data);
  const auto written = test::write_from("fits", *data);
  REQUIRE_OK(written);
  CHECK(written->size() % 2880 == 0);
  const auto back = test::read_as<ir::Data>("fits", *written);
  REQUIRE_OK(back);
  check_multi(back->value);
  // Image -> FITS -> Image is lossless for grey 8-bit pictures.
  ir::Image grey;
  grey.frames.push_back(test::gradient_frame(16, 9));
  for (std::size_t i = 0; i < grey.frames[0].rgba.size(); i += 4) {
    grey.frames[0].rgba[i + 1] = grey.frames[0].rgba[i + 2] = grey.frames[0].rgba[i];
    grey.frames[0].rgba[i + 3] = 255;
  }
  for (const char* depth : {"8", "16"}) {
    INFO(depth);
    const auto fits = test::write_from("fits", grey, std::string(R"({"fits_bitpix":")") + depth + "\"}");
    REQUIRE_OK(fits);
    const auto decoded = test::read_as<ir::Image>("fits", *fits, R"({"clip_percent":0,"window_low":0,"window_high":)" +
                                                                    std::string(std::string_view(depth) == "8" ? "255" : "65535") + "}");
    REQUIRE_OK(decoded);
    CHECK(test::pixels_equal(decoded->frames[0], grey.frames[0]));
  }
  // Colour pictures become a 3-plane cube.
  ir::Image colour;
  colour.frames.push_back(test::gradient_frame(8, 4));
  const auto cube = test::write_from("fits", colour);
  REQUIRE_OK(cube);
  const auto cube_data = test::read_as<ir::Data>("fits", *cube);
  REQUIRE_OK(cube_data);
  CHECK(cube_data->value["variables"][0]["shape"] == Json::array({3, 4, 8}));
  // Header privacy: OBSERVER is dropped by default and kept on request.
  Json with_observer = data->value;
  with_observer["variables"][0]["attributes"]["OBSERVER"] = "Someone";
  const auto stripped = test::write_from("fits", ir::Data{with_observer});
  const auto kept = test::write_from("fits", ir::Data{with_observer}, R"({"strip_metadata":false})");
  REQUIRE_OK(stripped);
  REQUIRE_OK(kept);
  CHECK(test::str(*stripped).find("OBSERVER") == std::string::npos);
  CHECK(test::str(*kept).find("OBSERVER= 'Someone '") != std::string::npos);
}

TEST_CASE("science/fits: hostile input") {
  CHECK(test::fuzz_reader<ir::Data>("fits", test::fixture("science/arrays/gray8.fits"), 64) > 0);
  CHECK(test::fuzz_reader<ir::Image>("fits", test::fixture("science/arrays/gray8.fits"), 64) > 0);
  Bytes bomb = test::fixture("science/arrays/gray8.fits");
  // NAXIS1 = 99999999 on the fourth card.
  const std::string card = "NAXIS1  =             99999999";
  std::memcpy(bomb.data() + 240, card.data(), card.size());
  CHECK_STATUS(test::read_as<ir::Data>("fits", bomb), Status::Decode);
}
