// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <string>
#include <vector>

#include "families/image/image_ops.hpp"
#include "families/image/image_test_util.hpp"

using namespace cvt;
using test::image_util::load;
using test::image_util::photo_frame;
using test::image_util::single;

namespace {

ir::Image fixture(std::string_view format, std::string_view name) {
  return load(format, std::string("image/psd/") + std::string(name));
}

}  // namespace

TEST_CASE("image/psd: decodes the composite of RGB, RGBA, grey, 16-bit, CMYK and PSB documents") {
  const ir::Frame gradient = test::gradient_frame(32, 24);
  CHECK(test::pixels_equal(fixture("psd", "rgb8.psd").frames[0], gradient));
  CHECK(test::pixels_equal(fixture("psb", "rgb8_raw.psb").frames[0], gradient));
  const ir::Image rgba = fixture("psd", "rgba8.psd");
  REQUIRE(rgba.valid());
  CHECK(test::psnr(rgba.frames[0], test::alpha_gradient_frame(32, 24)) > 40.0);
  const ir::Image grey = fixture("psd", "grey8.psd");
  REQUIRE(grey.valid());
  CHECK(image::is_grayscale(grey.frames[0]));
  const ir::Image wide = fixture("psd", "rgb16.psd");
  REQUIRE(wide.hdr.has_value());
  CHECK(wide.source_bit_depth == 16);
  CHECK(test::pixels_equal(wide.frames[0], gradient));
  const ir::Image cmyk = fixture("psd", "cmyk8.psd");
  REQUIRE(cmyk.valid());
  CHECK(test::max_abs_diff(cmyk.frames[0], gradient) <= 2.0);
}

TEST_CASE("image/psd: writes flat documents that read back") {
  for (const ir::Frame& f : {photo_frame(40, 30), test::alpha_gradient_frame(20, 20), test::checker_frame(20, 10, 3)}) {
    for (const char* format : {"psd", "psb"}) {
      const auto encoded = test::write_from(format, single(f));
      REQUIRE_OK(encoded);
      const auto decoded = test::read_as<ir::Image>(format, *encoded);
      REQUIRE_OK(decoded);
      // Alpha is stored blended onto white, so very transparent pixels lose some precision.
      CHECK(test::psnr(decoded->frames[0], f) > 45.0);
    }
  }
}

TEST_CASE("image/dds: decodes uncompressed, DX10 and block-compressed textures") {
  const ir::Frame alpha = test::alpha_gradient_frame(32, 24);
  CHECK(test::pixels_equal(fixture("dds", "rgba.dds").frames[0], alpha));
  CHECK(test::pixels_equal(fixture("dds", "dx10_rgba.dds").frames[0], alpha));
  CHECK(test::psnr(fixture("dds", "dxt1.dds").frames[0], test::gradient_frame(32, 24)) > 32.0);
  CHECK(test::psnr(fixture("dds", "dxt5.dds").frames[0], alpha) > 32.0);
  CHECK(test::psnr(fixture("dds", "dxt3.dds").frames[0], alpha) > 30.0);
}

TEST_CASE("image/dds: writes uncompressed, BC1 and BC3 with mipmaps") {
  const ir::Image photo = single(photo_frame(64, 48));
  const ir::Image alpha = single(test::alpha_gradient_frame(64, 48));
  const auto raw = test::write_from("dds", alpha);
  REQUIRE_OK(raw);
  CHECK(test::pixels_equal(test::read_as<ir::Image>("dds", *raw)->frames[0], alpha.frames[0]));
  const auto bc1 = test::write_from("dds", photo, R"({"dds_format":"bc1"})");
  REQUIRE_OK(bc1);
  CHECK(test::psnr(test::read_as<ir::Image>("dds", *bc1)->frames[0], photo.frames[0]) > 27.0);
  const auto smooth = test::write_from("dds", single(test::gradient_frame(64, 48)), R"({"dds_format":"bc1"})");
  REQUIRE_OK(smooth);
  CHECK(test::psnr(test::read_as<ir::Image>("dds", *smooth)->frames[0], test::gradient_frame(64, 48)) > 35.0);
  const auto bc3 = test::write_from("dds", alpha, R"({"dds_format":"bc3","mipmaps":true})");
  REQUIRE_OK(bc3);
  CHECK(test::psnr(test::read_as<ir::Image>("dds", *bc3)->frames[0], alpha.frames[0]) > 33.0);
  CHECK(bc3->size() > 64 * 48 + 128);  // the mip chain follows the top level
  const auto cutout = test::write_from("dds", single(test::checker_frame(16, 16, 4)), R"({"dds_format":"bc1"})");
  REQUIRE_OK(cutout);
  CHECK(test::pixels_equal(test::read_as<ir::Image>("dds", *cutout)->frames[0], test::checker_frame(16, 16, 4)));
}

TEST_CASE("image/psd+dds: damaged files are errors, never crashes") {
  const std::pair<const char*, const char*> cases[] = {{"psd", "rgb8.psd"}, {"psd", "rgba8.psd"}, {"psd", "rgb16.psd"},
                                                       {"psb", "rgb8_raw.psb"}, {"dds", "dxt1.dds"}, {"dds", "dxt5.dds"},
                                                       {"dds", "rgba.dds"},     {"dds", "dx10_rgba.dds"}};
  for (const auto& [format, name] : cases) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Image>(format, test::fixture(std::string("image/psd/") + name)) > 0);
    CHECK(test::convert(test::fixture(std::string("image/psd/") + name), "", "png", "", name).ok());
  }
}
