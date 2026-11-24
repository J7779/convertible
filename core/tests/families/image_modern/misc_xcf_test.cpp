// Tests of the GIMP XCF reader.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <string>
#include <vector>

#include "families/image/image_ops.hpp"
#include "families/image_modern/misc_test_util.hpp"
#include "util/bytes.hpp"

using namespace cvt;
using test::misc_util::data;
using test::misc_util::load;
using test::misc_util::max_diff_visible;
using test::misc_util::reference;

namespace {

struct Case {
  const char* xcf;
  const char* png;  // GIMP's own flattened rendering
  int max_diff;     // largest channel difference allowed
  std::uint8_t depth;
};

void check(const Case& c) {
  INFO(c.xcf);
  const ir::Image image = load("xcf", std::string("xcf/") + c.xcf);
  REQUIRE(image.valid());
  const ir::Frame expected = reference(std::string("xcf/") + c.png);
  const int diff = max_diff_visible(image.frames[0], expected);
  INFO("max diff ", diff);
  CHECK(diff <= c.max_diff);
  CHECK(test::psnr(image.frames[0], expected) > 48.0);
  CHECK(image.source_bit_depth == c.depth);
  CHECK(image.hdr.has_value() == (c.depth > 8));
}

}  // namespace

TEST_CASE("image_modern/misc/xcf: GIMP 2.10 documents (v11/v12, 64-bit offsets) at every precision") {
  // Each document: gradient background, a 60 % box (normal mode: linear-light
  // compositing), a 50 % legacy-mode box (perceptual compositing), a layer
  // with a gradient layer mask, a hidden layer and a group at 80 % holding a
  // child partly off the canvas.
  const Case cases[] = {
      {"rgb8.xcf", "rgb8.png", 1, 8},
      // GIMP re-quantises its merged 8-bit *linear* layer before converting
      // it for export; we convert straight from the composite, so the darkest
      // tones differ by a few levels.
      {"u8_linear.xcf", "u8_linear.png", 6, 8},
      {"u16_gamma.xcf", "u16_gamma.png", 1, 16},
      {"u32_linear.xcf", "u32_linear.png", 1, 16},
      {"half_linear.xcf", "half_linear.png", 1, 32},
      {"float_gamma.xcf", "float_gamma.png", 1, 32},
      {"double_linear.xcf", "double_linear.png", 1, 32},
      {"grey8.xcf", "grey8.png", 1, 8},
      {"indexed.xcf", "indexed.png", 0, 8},
      // The RLE tiles of GIMP's files re-stored as zlib / uncompressed tiles.
      {"rgb8_zlib.xcf", "rgb8.png", 1, 8},
      {"rgb8_raw.xcf", "rgb8.png", 1, 8},
      {"float_gamma_zlib.xcf", "float_gamma.png", 1, 32},
      // Saved by GIMP 3.0 (v11 with GIMP 3 properties and an ICC profile parasite).
      {"gimp3.xcf", "gimp3.png", 1, 8},
  };
  for (const Case& c : cases) check(c);
  CHECK(load("xcf", "xcf/rgb8.xcf").dpi_x == 96.0);
  CHECK(load("xcf", "xcf/grey8.xcf").source_grayscale);
  CHECK(!load("xcf", "xcf/gimp3.xcf").icc_profile.empty());
}

TEST_CASE("image_modern/misc/xcf: GIMP 2.8 documents (v0, v1, v3, 32-bit offsets)") {
  const Case cases[] = {
      {"v28_rgb.xcf", "v28_rgb.png", 2, 8},
      {"v28_grey.xcf", "v28_grey.png", 1, 8},
      {"v28_indexed.xcf", "v28_indexed.png", 1, 8},
      {"v28_group.xcf", "v28_group.png", 2, 8},
  };
  for (const Case& c : cases) check(c);
}

TEST_CASE("image_modern/misc/xcf: HDR pixels and conversion paths") {
  const ir::Image half = load("xcf", "xcf/half_linear.xcf");
  REQUIRE(half.hdr.has_value());
  CHECK(half.hdr->width == half.width());
  // The same picture: tone-free linear values map back to the 8-bit frame.
  CHECK(test::max_abs_diff(image::from_linear(*half.hdr), half.frames[0]) <= 1.0);
  const test::Converted png = test::convert(data("xcf/u16_gamma.xcf"), "", "png", "", "u16_gamma.xcf");
  REQUIRE(png.ok());
  CHECK(test::read_as<ir::Image>("png", png.data)->source_bit_depth == 16);
}

TEST_CASE("image_modern/misc/xcf: damaged files are errors, never crashes") {
  for (const char* name : {"xcf/rgb8.xcf", "xcf/rgb8_zlib.xcf", "xcf/v28_group.xcf", "xcf/v28_indexed.xcf",
                           "xcf/half_linear.xcf"}) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Image>("xcf", data(name), 300) > 0);
  }
  // A header declaring a huge canvas with no layers is rejected by the limits.
  util::ByteWriter huge;
  huge.text("gimp xcf file").u8(0).u32be(1u << 20).u32be(1u << 20).u32be(0).u32be(0).u32be(0).u32be(0).u32be(0);
  CHECK(!test::read_as<ir::Image>("xcf", huge.data()));
  // A layer whose tile table points past the end.
  util::ByteWriter bad;
  bad.text("gimp xcf file").u8(0).u32be(8).u32be(8).u32be(0).u32be(0).u32be(0);  // header, no properties
  bad.u32be(46).u32be(0).u32be(0);                                                // one layer, no channels
  bad.u32be(8).u32be(8).u32be(0).u32be(1).u8(0).u32be(0).u32be(0).u32be(0xFFFFFF).u32be(0);
  REQUIRE(bad.size() > 46);
  CHECK(!test::read_as<ir::Image>("xcf", bad.data()));
}

TEST_CASE("image_modern/misc/xcf: detection") {
  using test::misc_util::detected;
  CHECK(detected(data("xcf/rgb8.xcf")) == "xcf");
  CHECK(detected(data("xcf/v28_rgb.xcf")) == "xcf");
  CHECK(detected({}, "picture.xcf") == "xcf");
}
