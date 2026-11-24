// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "util/bytes.hpp"
#include "families/image/image_ops.hpp"
#include "families/image/image_test_util.hpp"

using namespace cvt;
using test::image_util::load;
using test::image_util::photo_frame;
using test::image_util::single;

namespace {

ir::Image bmp(std::string_view name) { return load("bmp", std::string("image/bmp/") + std::string(name)); }

ir::Frame palette200() {
  ir::Frame f = ir::Frame::filled(32, 24, {0, 0, 0, 255});
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const std::uint32_t i = (x * 5 + y * 3) % 200;
      std::uint8_t* p = f.rgba.data() + f.offset(x, y);
      p[0] = static_cast<std::uint8_t>((i * 7) % 256);
      p[1] = static_cast<std::uint8_t>((i * 13) % 256);
      p[2] = static_cast<std::uint8_t>((i * 29) % 256);
    }
  }
  return f;
}

ir::Frame palette16() {
  ir::Frame f = ir::Frame::filled(32, 24, {0, 0, 0, 255});
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const std::uint32_t i = ((x / 3) + y) % 16;
      std::uint8_t* p = f.rgba.data() + f.offset(x, y);
      p[0] = static_cast<std::uint8_t>((i * 70) % 256);
      p[1] = static_cast<std::uint8_t>((i * 130) % 256);
      p[2] = static_cast<std::uint8_t>((255 - i * 50) % 256);
    }
  }
  return f;
}

}  // namespace

TEST_CASE("image/bmp: decodes every header version and bit depth") {
  const ir::Frame gradient = test::gradient_frame(32, 24);
  for (const char* name : {"rgb24.bmp", "topdown.bmp", "os2.bmp", "sips.bmp"}) {
    INFO(name);
    const ir::Image image = bmp(name);
    REQUIRE(image.valid());
    CHECK(test::pixels_equal(image.frames[0], gradient));
  }
  const ir::Image v5 = bmp("rgba32_v5.bmp");
  REQUIRE(v5.valid());
  CHECK(test::pixels_equal(v5.frames[0], test::alpha_gradient_frame(32, 24)));
  CHECK(test::pixels_equal(bmp("pal8.bmp").frames[0], palette200()));
  CHECK(test::pixels_equal(bmp("rle8.bmp").frames[0], palette200()));
  CHECK(test::pixels_equal(bmp("pal4.bmp").frames[0], palette16()));
  CHECK(test::pixels_equal(bmp("mono1.bmp").frames[0], test::checker_frame(32, 24, 4)));
  // 16-bit masks: within the 5/6-bit quantisation.
  const ir::Image rgb565 = bmp("rgb565.bmp");
  REQUIRE(rgb565.valid());
  CHECK(test::max_abs_diff(rgb565.frames[0], gradient) <= 8.0);
  const ir::Image argb = bmp("argb1555.bmp");
  REQUIRE(argb.valid());
  CHECK(image::has_binary_alpha(argb.frames[0]));
  CHECK_APPROX(image::srgb_to_linear(argb.frames[0].rgba[argb.frames[0].offset(31, 23)]), 1.0, 0.02);
}

TEST_CASE("image/bmp: RLE4 with deltas and absolute runs") {
  const ir::Image image = bmp("rle4.bmp");
  REQUIRE(image.valid());
  REQUIRE(image.width() == 8);
  const auto entry = [](int i) { return image::Rgba{static_cast<std::uint8_t>(i * 16), static_cast<std::uint8_t>(255 - i * 16), 0, 255}; };
  const int rows[3][8] = {{0, 1, 0, 1, 0, 1, 0, 1}, {2, 2, 2, 2, 3, 4, 5, 6}, {7, 8, 9, 10, 11, 12, 13, 14}};
  ir::Frame expected = ir::Frame::filled(8, 4, {0, 0, 0, 0});
  for (std::uint32_t y = 0; y < 3; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) ::cvt::util::copy_bytes(expected.rgba.data() + expected.offset(x, y), entry(rows[y][x]).data(), 4);
  }
  CHECK(test::pixels_equal(image.frames[0], expected));
}

TEST_CASE("image/bmp: round trips choose a lossless layout") {
  struct Case {
    const char* what;
    ir::Frame frame;
    int bpp;
  };
  const Case cases[] = {{"gradient", test::gradient_frame(33, 17), 24},
                        {"alpha", test::alpha_gradient_frame(20, 10), 32},
                        {"palette", palette200(), 8},
                        {"sixteen", palette16(), 4},
                        {"two", test::checker_frame(13, 7, 2), 1}};
  for (const Case& c : cases) {
    INFO(c.what);
    const auto encoded = test::write_from("bmp", single(c.frame));
    REQUIRE_OK(encoded);
    CHECK(std::to_integer<int>((*encoded)[28]) == c.bpp);
    const auto decoded = test::read_as<ir::Image>("bmp", *encoded);
    REQUIRE_OK(decoded);
    CHECK(test::pixels_equal(decoded->frames[0], c.frame));
    if (c.bpp == 8 || c.bpp == 4) {
      const auto rle = test::write_from("bmp", single(c.frame), R"({"rle":true})");
      REQUIRE_OK(rle);
      CHECK(test::pixels_equal(test::read_as<ir::Image>("bmp", *rle)->frames[0], c.frame));
    }
  }
  // Forced depths.
  const ir::Image photo = single(photo_frame(40, 30));
  const auto eight = test::write_from("bmp", photo, R"({"bit_depth":"8"})");
  REQUIRE_OK(eight);
  CHECK(test::psnr(test::read_as<ir::Image>("bmp", *eight)->frames[0], photo.frames[0]) > 28.0);
  const auto rle8 = test::write_from("bmp", photo, R"({"bit_depth":"8","rle":true})");
  REQUIRE_OK(rle8);
  CHECK(test::psnr(test::read_as<ir::Image>("bmp", *rle8)->frames[0], photo.frames[0]) > 28.0);
  const auto one = test::write_from("bmp", photo, R"({"bit_depth":"1"})");
  REQUIRE_OK(one);
  CHECK(std::to_integer<int>((*one)[28]) == 1);
  const auto opaque = test::write_from("bmp", single(test::alpha_gradient_frame(8, 8)), R"({"bit_depth":"24"})");
  REQUIRE_OK(opaque);
  CHECK(image::is_opaque(test::read_as<ir::Image>("bmp", *opaque)->frames[0]));
}

TEST_CASE("image/bmp: damaged files are errors, never crashes") {
  for (const char* name : {"rgb24.bmp", "rgba32_v5.bmp", "pal4.bmp", "rle8.bmp", "rle4.bmp", "os2.bmp", "rgb565.bmp"}) {
    CHECK(test::fuzz_reader<ir::Image>("bmp", test::fixture(std::string("image/bmp/") + name)) > 0);
  }
  Bytes bomb = test::fixture("image/bmp/rgb24.bmp");
  bomb[18] = bomb[19] = bomb[20] = std::byte{0xFF};
  bomb[22] = bomb[23] = bomb[24] = std::byte{0xFF};
  Limits limits;
  limits.max_pixels = 1'000'000;
  CHECK_STATUS(test::read_as<ir::Image>("bmp", bomb, {}, nullptr, limits), Status::Limit);
}

TEST_CASE("image/bmp: detection and conversion") {
  const test::Converted out = test::convert(test::fixture("image/bmp/rgb24.bmp"), "", "png");
  CHECK(out.ok());
  CHECK(test::convert(test::fixture("image/bmp/pal8.bmp"), "", "bmp").ok());
}
