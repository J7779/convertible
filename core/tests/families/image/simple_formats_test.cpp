// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Netpbm, QOI, TGA, PCX, Radiance HDR, farbfeld, WBMP, XBM, XPM, SGI, Sun.
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "families/image/image_ops.hpp"
#include "families/image/image_test_util.hpp"

using namespace cvt;
using test::image_util::load;
using test::image_util::photo_frame;
using test::image_util::single;

namespace {

ir::Image misc(std::string_view format, std::string_view name) {
  return load(format, std::string("image/misc/") + std::string(name));
}

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

ir::Frame checker() {
  ir::Frame f = ir::Frame::filled(32, 24, {0, 0, 0, 255});
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      if (((x / 4) + (y / 4)) % 2 != 0) std::memset(f.rgba.data() + f.offset(x, y), 255, 3);
    }
  }
  return f;
}

std::uint16_t wide(std::uint32_t x, std::uint32_t y, std::uint32_t c) {
  return static_cast<std::uint16_t>((x * 2039 + y * 977 + c * 12345) % 65536);
}

/// Worst difference of 16-bit samples against the `wide` pattern.
double wide_error(const ir::Image& image, bool grey) {
  if (!image.hdr) return 1e9;
  const std::vector<std::uint16_t> s = image::srgb16_from_linear(*image.hdr);
  double worst = 0;
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      for (std::uint32_t c = 0; c < 3; ++c) {
        double expected = wide(x, y, c);
        if (grey) {
          const double lin = 0.2126 * image::srgb_to_linear(wide(x, y, 0) / 65535.0f) +
                             0.7152 * image::srgb_to_linear(wide(x, y, 1) / 65535.0f) +
                             0.0722 * image::srgb_to_linear(wide(x, y, 2) / 65535.0f);
          expected = image::linear_to_srgb16(static_cast<float>(lin));
        }
        worst = std::max(worst, std::abs(s[(std::size_t{y} * 32 + x) * 4 + c] - expected));
      }
    }
  }
  return worst;
}

}  // namespace

TEST_CASE("image/pnm: decodes binary, ASCII, 16-bit, PAM and PFM files") {
  const ir::Frame gradient = test::gradient_frame(32, 24);
  CHECK(test::pixels_equal(misc("ppm", "grad.ppm").frames[0], gradient));
  CHECK(test::pixels_equal(misc("ppm", "grad_ascii.ppm").frames[0], gradient));
  CHECK(test::pixels_equal(misc("pbm", "checker.pbm").frames[0], checker()));
  CHECK(test::pixels_equal(misc("pbm", "checker_ascii.pbm").frames[0], checker()));
  CHECK(test::pixels_equal(misc("pam", "alpha.pam").frames[0], test::alpha_gradient_frame(32, 24)));
  const ir::Image grey16 = misc("pgm", "grey16.pgm");
  CHECK(grey16.source_bit_depth == 16);
  CHECK(image::is_grayscale(grey16.frames[0]));
  REQUIRE(grey16.hdr.has_value());
  const ir::Image pfm = misc("pfm", "wide.pfm");
  REQUIRE(pfm.hdr.has_value());
  CHECK(wide_error(pfm, false) <= 2.0);
  // The generic reader accepts every variant.
  CHECK(test::pixels_equal(misc("pnm", "grad.ppm").frames[0], gradient));
}

TEST_CASE("image/pnm: round trips") {
  const ir::Image photo = single(photo_frame(31, 17));
  for (const char* format : {"ppm", "pam", "pnm"}) {
    for (const char* opts : {"{}", R"({"ascii":true})"}) {
      if (std::string_view(format) == "pam" && std::string_view(opts) != "{}") continue;
      INFO(format, " ", opts);
      const auto encoded = test::write_from(format, photo, opts);
      REQUIRE_OK(encoded);
      CHECK(test::pixels_equal(test::read_as<ir::Image>(format, *encoded)->frames[0], photo.frames[0]));
    }
  }
  const ir::Image alpha = single(test::alpha_gradient_frame(20, 10));
  const auto pam = test::write_from("pam", alpha);
  REQUIRE_OK(pam);
  CHECK(test::pixels_equal(test::read_as<ir::Image>("pam", *pam)->frames[0], alpha.frames[0]));
  const auto pbm = test::write_from("pbm", single(checker()));
  REQUIRE_OK(pbm);
  CHECK(test::pixels_equal(test::read_as<ir::Image>("pbm", *pbm)->frames[0], checker()));
  const ir::Image grey16 = misc("pgm", "grey16.pgm");
  const auto deep = test::write_from("pgm", grey16);
  REQUIRE_OK(deep);
  CHECK(image::srgb16_from_linear(*test::read_as<ir::Image>("pgm", *deep)->hdr) == image::srgb16_from_linear(*grey16.hdr));
  const ir::Image pfm = misc("pfm", "wide.pfm");
  const auto floats = test::write_from("pfm", pfm);
  REQUIRE_OK(floats);
  CHECK(test::read_as<ir::Image>("pfm", *floats)->hdr->rgba == pfm.hdr->rgba);
}

TEST_CASE("image/qoi: decodes reference files and round trips exactly") {
  CHECK(test::pixels_equal(misc("qoi", "grad.qoi").frames[0], test::gradient_frame(32, 24)));
  CHECK(test::pixels_equal(misc("qoi", "alpha.qoi").frames[0], test::alpha_gradient_frame(32, 24)));
  for (const ir::Frame& f : {photo_frame(70, 33), test::alpha_gradient_frame(64, 64), test::checker_frame(300, 4, 1)}) {
    const auto encoded = test::write_from("qoi", single(f));
    REQUIRE_OK(encoded);
    CHECK(test::pixels_equal(test::read_as<ir::Image>("qoi", *encoded)->frames[0], f));
  }
  // Byte-identical with the reference encoder (ImageMagick implements the specification's encoder).
  const auto again = test::write_from("qoi", misc("qoi", "alpha.qoi"));
  REQUIRE_OK(again);
  CHECK(*again == test::fixture("image/misc/alpha.qoi"));
}

TEST_CASE("image/tga: decodes RLE, alpha, palette and grey files") {
  CHECK(test::pixels_equal(misc("tga", "grad_rle.tga").frames[0], test::gradient_frame(32, 24)));
  CHECK(test::pixels_equal(misc("tga", "alpha.tga").frames[0], test::alpha_gradient_frame(32, 24)));
  CHECK(test::pixels_equal(misc("tga", "palette.tga").frames[0], palette200()));
  CHECK(test::pixels_equal(misc("tga", "grey_rle.tga").frames[0], checker()));
  for (const char* opts : {"{}", R"({"rle":false})"}) {
    for (const ir::Frame& f : {photo_frame(40, 30), test::alpha_gradient_frame(20, 20), checker()}) {
      const auto encoded = test::write_from("tga", single(f), opts);
      REQUIRE_OK(encoded);
      CHECK(test::pixels_equal(test::read_as<ir::Image>("tga", *encoded)->frames[0], f));
    }
  }
  const auto rle = test::write_from("tga", single(checker()));
  const auto raw = test::write_from("tga", single(checker()), R"({"rle":false})");
  CHECK(rle->size() < raw->size());
}

TEST_CASE("image/pcx: decodes 24-bit, palette and monochrome files") {
  CHECK(test::pixels_equal(misc("pcx", "grad.pcx").frames[0], test::gradient_frame(32, 24)));
  CHECK(test::pixels_equal(misc("pcx", "palette.pcx").frames[0], palette200()));
  CHECK(test::pixels_equal(misc("pcx", "mono.pcx").frames[0], checker()));
  for (const ir::Frame& f : {photo_frame(41, 30), palette200(), test::alpha_gradient_frame(16, 16)}) {
    const auto encoded = test::write_from("pcx", single(f));
    REQUIRE_OK(encoded);
    CHECK(test::pixels_equal(test::read_as<ir::Image>("pcx", *encoded)->frames[0], f));
  }
}

TEST_CASE("image/hdr: Radiance files keep HDR precision") {
  const ir::Image hdr = misc("hdr", "wide.hdr");
  REQUIRE(hdr.hdr.has_value());
  CHECK(hdr.source_bit_depth == 32);
  // RGBE keeps 8 bits of mantissa relative to the brightest channel of a pixel.
  double worst = 0;
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      double brightest = 0.01;
      for (std::uint32_t c = 0; c < 3; ++c) brightest = std::max<double>(brightest, image::srgb_to_linear(wide(x, y, c) / 65535.0f));
      for (std::uint32_t c = 0; c < 3; ++c) {
        const double expected = image::srgb_to_linear(wide(x, y, c) / 65535.0f);
        const double got = hdr.hdr->rgba[(std::size_t{y} * 32 + x) * 4 + c];
        worst = std::max(worst, std::abs(got - expected) / brightest);
      }
    }
  }
  CHECK(worst < 0.02);
  // Round trip through our writer, including values above 1.
  ir::Image bright = hdr;
  for (float& v : bright.hdr->rgba) v *= 40.0f;
  const auto encoded = test::write_from("hdr", bright);
  REQUIRE_OK(encoded);
  const auto back = test::read_as<ir::Image>("hdr", *encoded);
  REQUIRE_OK(back);
  // RGBE shares one exponent per pixel: each channel is exact to 1/256 of the brightest one.
  bool close = true;
  for (std::size_t i = 0; i < back->hdr->rgba.size(); i += 4) {
    const float* a = back->hdr->rgba.data() + i;
    const float* b = bright.hdr->rgba.data() + i;
    const float tolerance = std::max({b[0], b[1], b[2]}) / 128.0f + 1e-6f;
    for (int c = 0; c < 3; ++c) close = close && std::abs(a[c] - b[c]) <= tolerance;
  }
  CHECK(close);
  // Tone mapping options change the 8-bit rendition.
  const auto clamp = test::read_as<ir::Image>("hdr", *encoded, R"({"tone_map":"clamp"})");
  const auto filmic = test::read_as<ir::Image>("hdr", *encoded, R"({"tone_map":"filmic","exposure":-3})");
  REQUIRE_OK(clamp);
  REQUIRE_OK(filmic);
  CHECK(clamp->frames[0].rgba != filmic->frames[0].rgba);
}

TEST_CASE("image/farbfeld: 16-bit samples round trip exactly") {
  const ir::Image ff = misc("farbfeld", "wide.ff");
  CHECK(wide_error(ff, false) == 0.0);
  const auto encoded = test::write_from("farbfeld", ff);
  REQUIRE_OK(encoded);
  CHECK(*encoded == test::fixture("image/misc/wide.ff"));
}

TEST_CASE("image/wbmp+xbm: bilevel formats") {
  CHECK(test::pixels_equal(misc("wbmp", "checker.wbmp").frames[0], checker()));
  CHECK(test::pixels_equal(misc("xbm", "checker.xbm").frames[0], checker()));
  for (const char* format : {"wbmp", "xbm"}) {
    const auto encoded = test::write_from(format, single(checker()));
    REQUIRE_OK(encoded);
    CHECK(test::pixels_equal(test::read_as<ir::Image>(format, *encoded)->frames[0], checker()));
    const auto dithered = test::write_from(format, single(test::gradient_frame(64, 32)));
    REQUIRE_OK(dithered);
    CHECK(test::read_as<ir::Image>(format, *dithered)->valid());
  }
  CHECK(test::convert(test::fixture("image/misc/checker.wbmp"), "", "png", "", "checker.wbmp").ok());
}

TEST_CASE("image/xpm: palettes and transparency") {
  CHECK(test::pixels_equal(misc("xpm", "palette.xpm").frames[0], palette200()));
  const ir::Image alpha = misc("xpm", "alpha.xpm");
  REQUIRE(alpha.valid());
  CHECK(image::has_binary_alpha(alpha.frames[0]));
  CHECK(alpha.frames[0].rgba[3] == 0);
  for (const ir::Frame& f : {palette200(), checker()}) {
    const auto encoded = test::write_from("xpm", single(f));
    REQUIRE_OK(encoded);
    CHECK(test::pixels_equal(test::read_as<ir::Image>("xpm", *encoded)->frames[0], f));
  }
  const auto photo = test::write_from("xpm", single(photo_frame(40, 30)));
  REQUIRE_OK(photo);
  CHECK(test::psnr(test::read_as<ir::Image>("xpm", *photo)->frames[0], photo_frame(40, 30)) > 28.0);
}

TEST_CASE("image/sgi: verbatim, RLE and 16-bit") {
  CHECK(test::pixels_equal(misc("sgi", "grad_rle.sgi").frames[0], test::gradient_frame(32, 24)));
  CHECK(test::pixels_equal(misc("sgi", "alpha.sgi").frames[0], test::alpha_gradient_frame(32, 24)));
  const ir::Image deep = misc("sgi", "wide16.sgi");
  CHECK(wide_error(deep, false) == 0.0);
  for (const ir::Frame& f : {photo_frame(40, 30), test::alpha_gradient_frame(20, 20), checker()}) {
    const auto encoded = test::write_from("sgi", single(f));
    REQUIRE_OK(encoded);
    CHECK(test::pixels_equal(test::read_as<ir::Image>("sgi", *encoded)->frames[0], f));
  }
  const auto encoded16 = test::write_from("sgi", deep);
  REQUIRE_OK(encoded16);
  CHECK(wide_error(*test::read_as<ir::Image>("sgi", *encoded16), false) == 0.0);
}

TEST_CASE("image/sun: 24-bit, palette and monochrome") {
  CHECK(test::pixels_equal(misc("sun", "grad.ras").frames[0], test::gradient_frame(32, 24)));
  CHECK(test::pixels_equal(misc("sun", "palette.ras").frames[0], palette200()));
  CHECK(test::pixels_equal(misc("sun", "mono.ras").frames[0], checker()));
  for (const char* opts : {"{}", R"({"rle":true})"}) {
    for (const ir::Frame& f : {photo_frame(41, 30), palette200(), checker()}) {
      const auto encoded = test::write_from("sun", single(f), opts);
      REQUIRE_OK(encoded);
      CHECK(test::pixels_equal(test::read_as<ir::Image>("sun", *encoded)->frames[0], f));
    }
  }
}

TEST_CASE("image/misc: damaged files are errors, never crashes") {
  const std::pair<const char*, const char*> cases[] = {
      {"ppm", "grad.ppm"},      {"ppm", "grad_ascii.ppm"}, {"pbm", "checker.pbm"},   {"pam", "alpha.pam"},
      {"pfm", "wide.pfm"},      {"pgm", "grey16.pgm"},     {"qoi", "alpha.qoi"},     {"tga", "grad_rle.tga"},
      {"tga", "palette.tga"},   {"pcx", "palette.pcx"},    {"pcx", "mono.pcx"},      {"hdr", "wide.hdr"},
      {"farbfeld", "wide.ff"},  {"wbmp", "checker.wbmp"},  {"xbm", "checker.xbm"},   {"xpm", "palette.xpm"},
      {"sgi", "grad_rle.sgi"},  {"sgi", "wide16.sgi"},     {"sun", "grad.ras"},      {"sun", "palette.ras"},
  };
  for (const auto& [format, name] : cases) {
    INFO(format, " ", name);
    CHECK(test::fuzz_reader<ir::Image>(format, test::fixture(std::string("image/misc/") + name), 128) > 0);
  }
}

TEST_CASE("image/misc: detection") {
  const std::pair<const char*, const char*> cases[] = {
      {"grad.ppm", "ppm"},       {"checker.pbm", "pbm"}, {"grey16.pgm", "pgm"}, {"alpha.pam", "pam"},
      {"wide.pfm", "pfm"},       {"alpha.qoi", "qoi"},   {"grad.pcx", "pcx"},   {"wide.hdr", "hdr"},
      {"wide.ff", "farbfeld"},   {"checker.xbm", "xbm"}, {"palette.xpm", "xpm"}, {"grad_rle.sgi", "sgi"},
      {"grad.ras", "sun"},       {"alpha.tga", "tga"},
  };
  for (const auto& [name, id] : cases) {
    INFO(name);
    const test::Converted out = test::convert(test::fixture(std::string("image/misc/") + name), "", "png", "", name);
    CHECK(out.ok());
    const test::Converted same = test::convert(test::fixture(std::string("image/misc/") + name), "", id, "", name);
    CHECK(same.ok());
  }
}
