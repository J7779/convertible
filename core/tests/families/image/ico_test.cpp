// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <string>
#include <vector>

#include "families/image/image_ops.hpp"
#include "families/image/image_test_util.hpp"

using namespace cvt;
using test::image_util::load;
using test::image_util::single;

namespace {

ir::Frame alpha_square(std::uint32_t n) {
  ir::Frame f = ir::Frame::filled(n, n, {0, 0, 0, 0});
  for (std::uint32_t y = 0; y < n; ++y) {
    for (std::uint32_t x = 0; x < n; ++x) {
      std::uint8_t* p = f.rgba.data() + f.offset(x, y);
      p[0] = static_cast<std::uint8_t>(x * 255 / (n - 1));
      p[1] = static_cast<std::uint8_t>(y * 255 / (n - 1));
      p[2] = static_cast<std::uint8_t>(255 - (x + y) * 255 / (2 * n - 2));
      p[3] = static_cast<std::uint8_t>((x + y) * 255 / (2 * n - 2));
    }
  }
  return f;
}

std::uint16_t entries_of(const Bytes& icon) {
  return static_cast<std::uint16_t>(std::to_integer<int>(icon[4]) | std::to_integer<int>(icon[5]) << 8);
}

}  // namespace

TEST_CASE("image/ico: decodes bitmap, PNG and palette entries") {
  const ir::Image multi = load("ico", "image/ico/multi.ico");
  REQUIRE(multi.valid());
  CHECK(multi.width() == 48);  // the largest entry
  CHECK(test::psnr(multi.frames[0], alpha_square(48)) > 40.0);
  const ir::Image png = load("ico", "image/ico/png_entry.ico");
  REQUIRE(png.valid());
  CHECK(test::pixels_equal(png.frames[0], alpha_square(64)));
  const ir::Image cur = load("cur", "image/ico/cursor.cur");
  REQUIRE(cur.valid());
  CHECK(test::psnr(cur.frames[0], alpha_square(48)) > 40.0);
  const ir::Image pal = load("ico", "image/ico/pal8_mask.ico");
  REQUIRE(pal.valid());
  ir::Frame expected = ir::Frame::filled(16, 16, {0, 0, 0, 0});
  for (std::uint32_t y = 0; y < 16; ++y) {
    for (std::uint32_t x = 4; x < 16; ++x) {
      const std::uint32_t i = (x + 16 * y) % 256;
      std::uint8_t* p = expected.rgba.data() + expected.offset(x, y);
      p[0] = static_cast<std::uint8_t>(i);
      p[1] = static_cast<std::uint8_t>(255 - i);
      p[2] = 128;
      p[3] = 255;
    }
  }
  CHECK(test::pixels_equal(pal.frames[0], expected));
}

TEST_CASE("image/icns: decodes iconutil output and run-length entries") {
  const ir::Image set = load("icns", "image/ico/iconset.icns");
  REQUIRE(set.valid());
  CHECK(set.width() == 128);
  CHECK(test::psnr(set.frames[0], alpha_square(128)) > 40.0);
  const ir::Image rle = load("icns", "image/ico/rle.icns");
  REQUIRE(rle.valid());
  ir::Frame expected = ir::Frame::filled(32, 32, {0, 0, 0, 255});
  for (std::uint32_t y = 0; y < 32; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      std::uint8_t* p = expected.rgba.data() + expected.offset(x, y);
      p[0] = static_cast<std::uint8_t>(x * 8);
      p[1] = static_cast<std::uint8_t>(y * 8);
      p[2] = 77;
      p[3] = y == 0 ? 0 : 255;
    }
  }
  CHECK(test::pixels_equal(rle.frames[0], expected));
}

TEST_CASE("image/ico: writes multi-size icon sets") {
  const ir::Image source = single(alpha_square(300));
  const auto icon = test::write_from("ico", source);
  REQUIRE_OK(icon);
  CHECK(entries_of(*icon) == 7);  // 16, 24, 32, 48, 64, 128, 256
  const auto back = test::read_as<ir::Image>("ico", *icon);
  REQUIRE_OK(back);
  CHECK(back->width() == 256);
  const auto expected = image::resize(alpha_square(300), 256, 256);
  REQUIRE_OK(expected);
  CHECK(test::psnr(back->frames[0], *expected) > 45.0);
  // Explicit sizes, bitmap entries, small sources are not enlarged.
  const auto custom = test::write_from("ico", source, R"({"sizes":"16,32","entry_format":"bmp"})");
  REQUIRE_OK(custom);
  CHECK(entries_of(*custom) == 2);
  const auto small = test::read_as<ir::Image>("ico", *custom);
  REQUIRE_OK(small);
  CHECK(small->width() == 32);
  const auto tiny = test::write_from("ico", single(alpha_square(20)));
  REQUIRE_OK(tiny);
  CHECK(entries_of(*tiny) == 1);  // only 16: smaller sizes are never enlarged
  CHECK_STATUS(test::write_from("ico", source, R"({"sizes":"16,999"})"), Status::InvalidArgument);
  // Non-square pictures are centred on a transparent square.
  const auto wide = test::write_from("ico", single(test::gradient_frame(64, 32)), R"({"sizes":"64"})");
  REQUIRE_OK(wide);
  const auto squared = test::read_as<ir::Image>("ico", *wide);
  REQUIRE_OK(squared);
  CHECK(squared->frames[0].rgba[squared->frames[0].offset(10, 2) + 3] == 0);
  CHECK(squared->frames[0].rgba[squared->frames[0].offset(10, 32) + 3] == 255);
  // Cursors.
  const auto cur = test::write_from("cur", single(alpha_square(32)), R"({"hotspot_x":128})");
  REQUIRE_OK(cur);
  CHECK(std::to_integer<int>((*cur)[2]) == 2);
  CHECK(test::read_as<ir::Image>("cur", *cur)->valid());
}

TEST_CASE("image/icns: writes PNG-based icons") {
  const auto icns = test::write_from("icns", single(alpha_square(200)));
  REQUIRE_OK(icns);
  const auto back = test::read_as<ir::Image>("icns", *icns);
  REQUIRE_OK(back);
  CHECK(back->width() == 128);  // 16 ... 128: never enlarged
}

TEST_CASE("image/ico: damaged files are errors, never crashes") {
  for (const char* name : {"multi.ico", "png_entry.ico", "pal8_mask.ico", "cursor.cur"}) {
    CHECK(test::fuzz_reader<ir::Image>("ico", test::fixture(std::string("image/ico/") + name)) > 0);
  }
  for (const char* name : {"iconset.icns", "rle.icns"}) {
    CHECK(test::fuzz_reader<ir::Image>("icns", test::fixture(std::string("image/ico/") + name)) > 0);
  }
  CHECK(test::convert(test::fixture("image/ico/multi.ico"), "", "png").ok());
  CHECK(test::convert(test::fixture("image/ico/rle.icns"), "", "png").ok());
  CHECK(test::convert(test::fixture("image/ico/cursor.cur"), "", "png", "", "pointer.cur").ok());
}
