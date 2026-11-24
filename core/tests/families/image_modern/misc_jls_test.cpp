// Tests of the JPEG-LS reader and writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "families/image/image_ops.hpp"
#include "families/image_modern/misc_test_util.hpp"
#include "util/bytes.hpp"

using namespace cvt;
using test::misc_util::data;
using test::misc_util::load;
using test::misc_util::single;

namespace {

double hdr_max_diff(const ir::HdrPixels& a, const ir::HdrPixels& b) {
  if (a.rgba.size() != b.rgba.size()) return 1e9;
  double worst = 0;
  for (std::size_t i = 0; i < a.rgba.size(); ++i) worst = std::max(worst, std::abs(double{a.rgba[i]} - b.rgba[i]));
  return worst;
}

}  // namespace

TEST_CASE("image_modern/misc/jls: decodes CharLS files exactly as CharLS does") {
  // Every fixture's .pam is CharLS's own decoding of the .jls (for the
  // near-lossless files that is the only correct answer, bit for bit).
  const char* names[] = {"grey8",       "grey8_near3",      "grey5",           "grey12",
                         "grey16_near2", "rgb8_none",       "rgb8_line",       "rgb8_sample",
                         "rgb8_line_near2", "rgb8_sample_near4", "rgb16_sample", "rgba8_sample",
                         "grey8_custom", "rgb8_custom_near1"};
  for (const char* name : names) {
    INFO(name);
    const ir::Image ours = load("jls", std::string("jls/") + name + ".jls");
    const ir::Image reference = load("pam", std::string("jls/") + name + ".pam");
    REQUIRE(ours.valid());
    REQUIRE(reference.valid());
    CHECK(test::pixels_equal(ours.frames[0], reference.frames[0]));
    CHECK(ours.hdr.has_value() == reference.hdr.has_value());
    if (ours.hdr && reference.hdr) CHECK(hdr_max_diff(*ours.hdr, *reference.hdr) < 1e-4);
  }
  CHECK(load("jls", "jls/grey12.jls").source_bit_depth == 12);
  CHECK(load("jls", "jls/grey8.jls").source_grayscale);
}

TEST_CASE("image_modern/misc/jls: lossless round trips in every interleave mode") {
  const ir::Frame photo = test::noise_frame(37, 23, 5);
  const ir::Frame grad = test::gradient_frame(64, 40);
  const ir::Frame alpha = test::alpha_gradient_frame(30, 20);
  const ir::Frame checker = test::checker_frame(50, 17, 4);
  ir::Frame grey = test::gradient_frame(45, 31);
  image::to_grayscale(grey);
  const ir::Frame grey_frame = grey;
  for (const char* mode : {"sample", "line", "none"}) {
    for (const ir::Frame* f : {&photo, &grad, &alpha, &checker, &grey_frame}) {
      INFO(mode, " ", f->width);
      const auto encoded = test::write_from("jls", single(*f), std::string(R"({"jls_interleave":")") + mode + "\"}");
      REQUIRE_OK(encoded);
      const auto decoded = test::read_as<ir::Image>("jls", *encoded);
      REQUIRE_OK(decoded);
      CHECK(test::pixels_equal(decoded->frames[0], *f));
    }
  }
}

TEST_CASE("image_modern/misc/jls: near-lossless bounds the error and shrinks the file") {
  const ir::Image photo = load("pam", "jls/rgb8_sample.pam");
  std::size_t previous = 0;
  for (int near : {0, 1, 3, 8}) {
    INFO(near);
    const auto encoded = test::write_from("jls", photo, "{\"jls_near\":" + std::to_string(near) + "}");
    REQUIRE_OK(encoded);
    const auto decoded = test::read_as<ir::Image>("jls", *encoded);
    REQUIRE_OK(decoded);
    CHECK(test::max_abs_diff(decoded->frames[0], photo.frames[0]) <= near);
    if (previous != 0) CHECK(encoded->size() < previous);
    previous = encoded->size();
  }
}

TEST_CASE("image_modern/misc/jls: 16-bit sources are written with 16 bits") {
  const ir::Image wide = load("jls", "jls/rgb16_sample.jls");
  REQUIRE(wide.hdr.has_value());
  const auto encoded = test::write_from("jls", wide);
  REQUIRE_OK(encoded);
  CHECK(std::to_integer<int>((*encoded)[6]) == 16);  // SOF55 precision
  const auto decoded = test::read_as<ir::Image>("jls", *encoded);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->hdr.has_value());
  CHECK(hdr_max_diff(*decoded->hdr, *wide.hdr) < 1e-6);
  // Same-format path keeps the content.
  const test::Converted same = test::convert(data("jls/rgb8_line.jls"), "jls", "jls", R"({"jls_near":2})");
  REQUIRE(same.ok());
  CHECK(test::max_abs_diff(test::read_as<ir::Image>("jls", same.data)->frames[0],
                           load("jls", "jls/rgb8_line.jls").frames[0]) <= 2);
}

TEST_CASE("image_modern/misc/jls: damaged files are errors, never crashes") {
  for (const char* name : {"jls/grey8.jls", "jls/rgb8_sample.jls", "jls/rgb8_line_near2.jls", "jls/rgb16_sample.jls",
                           "jls/grey8_custom.jls", "jls/rgb8_none.jls"}) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Image>("jls", data(name), 400) > 0);
  }
  // A frame header announcing 65535 x 65535 pixels with no data.
  util::ByteWriter huge;
  huge.u16be(0xFFD8).u16be(0xFFF7).u16be(11).u8(8).u16be(65535).u16be(65535).u8(1).u8(1).u8(0x11).u8(0);
  huge.u16be(0xFFDA).u16be(8).u8(1).u8(1).u8(0).u8(0).u8(0).u8(0).u16be(0xFFD9);
  CHECK(!test::read_as<ir::Image>("jls", huge.data()));
}

TEST_CASE("image_modern/misc/jls: detection") {
  using test::misc_util::detected;
  CHECK(detected(data("jls/grey8.jls")) == "jls");
  CHECK(detected(data("jls/rgb8_sample.jls"), "x.jls") == "jls");
  CHECK(detected({}, "x.jls") == "jls");
}
