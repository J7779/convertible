// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "util/bytes.hpp"
#include "families/image/image_ops.hpp"
#include "families/image/image_test_util.hpp"

using namespace cvt;
using test::image_util::load;
using test::image_util::parse_ppm;
using test::image_util::photo_frame;
using test::image_util::single;

namespace {

ir::Image tiff(std::string_view name) { return load("tiff", std::string("image/tiff/") + std::string(name)); }

std::uint16_t wide(std::uint32_t x, std::uint32_t y, std::uint32_t c) {
  return static_cast<std::uint16_t>((x * 2039 + y * 977 + c * 12345) % 65536);
}

ir::Frame bilevel() {
  ir::Frame f = ir::Frame::filled(61, 23, {0, 0, 0, 255});
  for (std::uint32_t y = 0; y < 23; ++y) {
    for (std::uint32_t x = 0; x < 61; ++x) {
      const bool white = (((x / 4) + (y / 4)) % 2 == 0) != (x == y);
      if (white) ::cvt::util::fill_bytes(f.rgba.data() + f.offset(x, y), 255, 3);
    }
  }
  return f;
}

}  // namespace

TEST_CASE("image/tiff: decodes every compression and layout of 8-bit RGB exactly") {
  const ir::Frame gradient = test::gradient_frame(32, 24);
  for (const char* name : {"rgb8_none.tif", "rgb8_lzw_predictor.tif", "rgb8_packbits.tif", "rgb8_lzw_bigendian.tif",
                           "rgb8_tiled_deflate.tif", "rgb8_planar.tif", "rgb8_bigtiff.tif"}) {
    INFO(name);
    const ir::Image image = tiff(name);
    REQUIRE(image.frames.size() == 1);
    CHECK(test::pixels_equal(image.frames[0], gradient));
  }
  CHECK(test::pixels_equal(tiff("rgba8_deflate.tif").frames[0], test::alpha_gradient_frame(32, 24)));
  const ir::Image associated = tiff("rgba8_associated.tif");
  REQUIRE(associated.valid());
  CHECK(test::psnr(associated.frames[0], test::alpha_gradient_frame(32, 24)) > 38.0);
}

TEST_CASE("image/tiff: palette, CMYK, Lab, JPEG and orientation") {
  const ir::Frame gradient = test::gradient_frame(32, 24);
  const ir::Image palette = tiff("palette8.tif");
  REQUIRE(palette.valid());
  bool ok = true;
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const std::uint32_t i = (x * 5 + y * 3) % 200;
      const std::uint8_t* p = palette.frames[0].rgba.data() + palette.frames[0].offset(x, y);
      ok = ok && p[0] == (i * 7) % 256 && p[1] == (i * 13) % 256 && p[2] == (i * 29) % 256;
    }
  }
  CHECK(ok);
  CHECK(test::max_abs_diff(tiff("cmyk.tif").frames[0], gradient) <= 2.0);
  CHECK(test::psnr(tiff("lab.tif").frames[0], gradient) > 30.0);
  const ir::Image jpeg = tiff("jpeg.tif");
  REQUIRE(jpeg.valid());
  CHECK(test::psnr(jpeg.frames[0], gradient) > 32.0);
  const ir::Image oriented = tiff("orientation6.tif");
  REQUIRE(oriented.valid());
  CHECK(oriented.width() == 24);
  CHECK(oriented.height() == 32);
  CHECK(test::pixels_equal(oriented.frames[0], image::apply_orientation(gradient, 6)));
}

TEST_CASE("image/tiff: 16-bit and floating point samples keep their precision") {
  const ir::Image rgb16 = tiff("rgb16_lzw.tif");
  REQUIRE(rgb16.hdr.has_value());
  CHECK(rgb16.source_bit_depth == 16);
  const std::vector<std::uint16_t> samples = image::srgb16_from_linear(*rgb16.hdr);
  bool exact = true;
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      for (std::uint32_t c = 0; c < 3; ++c) exact = exact && samples[(y * 32 + x) * 4 + c] == wide(x, y, c);
    }
  }
  CHECK(exact);
  const ir::Image grey16 = tiff("grey16_deflate.tif");
  REQUIRE(grey16.hdr.has_value());
  CHECK(image::is_grayscale(grey16.frames[0]));
  for (const char* name : {"float32_predictor3.tif", "float16.tif"}) {
    INFO(name);
    const ir::Image f = tiff(name);
    REQUIRE(f.hdr.has_value());
    const std::vector<std::uint16_t> s = image::srgb16_from_linear(*f.hdr);
    double worst = 0;
    for (std::uint32_t y = 0; y < 24; ++y) {
      for (std::uint32_t x = 0; x < 32; ++x) {
        for (std::uint32_t c = 0; c < 3; ++c) {
          worst = std::max(worst, std::abs(static_cast<double>(s[(y * 32 + x) * 4 + c]) - wide(x, y, c)));
        }
      }
    }
    CHECK(worst <= 40.0);  // half floats keep ~11 bits
  }
}

TEST_CASE("image/tiff: CCITT fax and bilevel pictures") {
  const ir::Frame expected = bilevel();
  for (const char* name : {"mono_g4.tif", "mono_g3.tif", "mono_mh.tif", "mono_packbits.tif", "mono_g3_lsb.tif"}) {
    INFO(name);
    const ir::Image image = tiff(name);
    REQUIRE(image.valid());
    CHECK(test::pixels_equal(image.frames[0], expected));
  }
}

TEST_CASE("image/tiff: multi-page files become frames") {
  const ir::Image pages = tiff("multipage.tif");
  REQUIRE(pages.frames.size() == 3);
  const ir::Frame gradient = test::gradient_frame(32, 24);
  CHECK(test::pixels_equal(pages.frames[0], gradient));
  CHECK(test::pixels_equal(pages.frames[2], image::flip_horizontal(gradient)));
  CHECK(pages.frames[1].rgba[0] == 255 - gradient.rgba[0]);
}

TEST_CASE("image/tiff: round trips for every compression") {
  const ir::Image photo = single(photo_frame(70, 50));
  const ir::Image alpha = single(test::alpha_gradient_frame(40, 30));
  for (const char* c : {"none", "lzw", "deflate", "packbits"}) {
    INFO(c);
    const std::string opts = std::format(R"({{"compression":"{}"}})", c);
    const auto encoded = test::write_from("tiff", photo, opts);
    REQUIRE_OK(encoded);
    CHECK(test::pixels_equal(test::read_as<ir::Image>("tiff", *encoded)->frames[0], photo.frames[0]));
    const auto with_alpha = test::write_from("tiff", alpha, opts);
    REQUIRE_OK(with_alpha);
    CHECK(test::pixels_equal(test::read_as<ir::Image>("tiff", *with_alpha)->frames[0], alpha.frames[0]));
  }
  const auto jpeg = test::write_from("tiff", photo, R"({"compression":"jpeg","quality":85})");
  REQUIRE_OK(jpeg);
  CHECK(test::psnr(test::read_as<ir::Image>("tiff", *jpeg)->frames[0], photo.frames[0]) > 30.0);
  const auto lzw = test::write_from("tiff", photo, R"({"compression":"lzw"})");
  REQUIRE_OK(lzw);
  CHECK(jpeg->size() < lzw->size());
  // 16-bit sources stay 16-bit.
  const ir::Image wide16 = tiff("rgb16_lzw.tif");
  const auto deep = test::write_from("tiff", wide16);
  REQUIRE_OK(deep);
  const auto back = test::read_as<ir::Image>("tiff", *deep);
  REQUIRE_OK(back);
  REQUIRE(back->hdr.has_value());
  CHECK(image::srgb16_from_linear(*back->hdr) == image::srgb16_from_linear(*wide16.hdr));
  // Pages.
  ir::Image pages;
  pages.frames = {test::gradient_frame(20, 10), test::checker_frame(20, 10, 2), photo_frame(20, 10)};
  const auto multi = test::write_from("tiff", pages);
  REQUIRE_OK(multi);
  CHECK(test::pixels_equal(*test::read_as<ir::Image>("tiff", *multi), pages));
  // Grey stays grey.
  const auto grey = test::write_from("tiff", single(test::checker_frame(16, 16, 3)));
  REQUIRE_OK(grey);
  CHECK(test::pixels_equal(test::read_as<ir::Image>("tiff", *grey)->frames[0], test::checker_frame(16, 16, 3)));
}

TEST_CASE("image/tiff: damaged files are errors, never crashes") {
  for (const char* name : {"rgb8_lzw_predictor.tif", "rgb8_tiled_deflate.tif", "mono_g4.tif", "mono_g3.tif",
                           "jpeg.tif", "multipage.tif", "rgb8_bigtiff.tif", "float32_predictor3.tif", "palette8.tif"}) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Image>("tiff", test::fixture(std::string("image/tiff/") + name)) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Image>("tiff", test::bytes("II*\0")), Status::Decode);
}

TEST_CASE("image/tiff: detection and conversion") {
  CHECK(test::convert(test::fixture("image/tiff/rgb8_bigtiff.tif"), "", "png").ok());
  CHECK(test::convert(test::fixture("image/tiff/multipage.tif"), "", "tiff").ok());
}
