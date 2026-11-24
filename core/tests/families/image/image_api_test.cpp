// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>

#include "engine/options.hpp"
#include "engine/registry.hpp"
#include "families/image/image_api.hpp"
#include "families/image/image_test_util.hpp"

using namespace cvt;

TEST_CASE("image/api: decode and encode through the registry") {
  Context ctx;
  ctx.registry = &Registry::instance();
  const auto png = image::decode("png", test::fixture("image/png/rgb8.png"), ctx);
  REQUIRE_OK(png);
  CHECK(test::pixels_equal(png->frames[0], test::gradient_frame(32, 24)));
  const auto jpg = image::encode("jpg", *png, Options(util::parse_json(R"({"quality":95})").value()), ctx);
  REQUIRE_OK(jpg);
  const auto back = image::decode("jpeg", *jpg, ctx);  // ids and extensions are normalised
  REQUIRE_OK(back);
  CHECK(test::psnr(back->frames[0], png->frames[0]) > 38.0);
  CHECK(image::can_decode("tiff", ctx));
  CHECK(image::can_encode("gif", ctx));
  CHECK_FALSE(image::can_decode("no-such-format", ctx));
  CHECK_STATUS(image::decode("no-such-format", *jpg, ctx), Status::UnsupportedConversion);
  // Options are validated against the writer's specs.
  CHECK_STATUS(image::encode("png", *png, Options(util::parse_json(R"({"bit_depth":"7"})").value()), ctx),
               Status::InvalidArgument);
  // Limits and cancellation travel with the context.
  Context limited = ctx;
  limited.limits.max_pixels = 100;
  CHECK_STATUS(image::decode("png", test::fixture("image/png/rgb8.png"), limited), Status::Limit);
}

TEST_CASE("image/api: sniffs common formats") {
  const std::pair<const char*, const char*> cases[] = {
      {"image/png/rgb8.png", "png"},          {"image/jpeg/q75_420.jpg", "jpg"}, {"image/gif/palette.gif", "gif"},
      {"image/bmp/rgb24.bmp", "bmp"},         {"image/tiff/rgb8_none.tif", "tiff"}, {"image/tiff/rgb8_bigtiff.tif", "tiff"},
      {"image/ico/multi.ico", "ico"},         {"image/misc/alpha.qoi", "qoi"},   {"image/psd/rgb8.psd", "psd"},
      {"image/misc/wide.hdr", "hdr"},         {"image/misc/grad.ppm", "ppm"},    {"image/misc/grad.pcx", "pcx"},
  };
  for (const auto& [name, id] : cases) {
    INFO(name);
    const auto sniffed = image::sniff_format(test::fixture(name));
    REQUIRE(sniffed.has_value());
    CHECK(*sniffed == id);
  }
  CHECK_FALSE(image::sniff_format(test::bytes("hello")).has_value());
}
