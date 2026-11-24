// Tests of the AVIF, HEIC and HEIF writers: round trips with quality floors,
// lossless exactness, alpha, 10 bit, chroma, options and metadata.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "families/image/image_ops.hpp"
#include "ir/image.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"

using namespace cvt;

namespace {

/// A photo-like picture: smooth gradients, a few edges and some grain.
ir::Frame photo(std::uint32_t width, std::uint32_t height) {
  ir::Frame frame = test::gradient_frame(width, height);
  const Bytes noise = test::random_bytes(frame.rgba.size(), 5);
  for (std::size_t i = 0; i < frame.rgba.size(); ++i) {
    if (i % 4 == 3) continue;
    const int v = frame.rgba[i] + (std::to_integer<int>(noise[i]) % 5) - 2;
    frame.rgba[i] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
  }
  for (std::uint32_t y = height / 3; y < height / 2; ++y) {
    for (std::uint32_t x = width / 4; x < width / 2; ++x) {
      const std::size_t at = frame.offset(x, y);
      frame.rgba[at] = 230;
      frame.rgba[at + 1] = 40;
      frame.rgba[at + 2] = 30;
    }
  }
  return frame;
}

ir::Image single(ir::Frame frame) {
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

ir::Image round_trip(std::string_view format, const ir::Image& image, std::string_view options = {},
                     std::size_t* size = nullptr) {
  const auto encoded = test::write_from(format, image, options);
  if (!encoded) {
    FAIL_CHECK("encoding: ", encoded.error().message);
    return {};
  }
  if (size != nullptr) *size = encoded->size();
  auto decoded = test::read_as<ir::Image>(format, *encoded);
  if (!decoded) {
    FAIL_CHECK("decoding our output: ", decoded.error().message);
    return {};
  }
  return std::move(*decoded);
}

bool contains(ByteView data, std::string_view text) { return as_chars(data).find(text) != std::string_view::npos; }

}  // namespace

TEST_CASE("image_modern/heif: AVIF round trips meet quality floors and shrink with quality") {
  const ir::Image source = single(photo(96, 64));
  struct Level {
    int quality;
    double min_psnr;
    double min_ssim;
  };
  const Level levels[] = {{95, 33.5, 0.985}, {90, 33.0, 0.98}, {75, 31.5, 0.96}, {50, 28.5, 0.85}};
  std::size_t previous = SIZE_MAX;
  for (const Level& level : levels) {
    INFO(level.quality);
    std::size_t size = 0;
    const ir::Image back = round_trip("avif", source, std::format(R"({{"quality":{}}})", level.quality), &size);
    REQUIRE(back.valid());
    const double psnr = test::psnr(back, source);
    const double ssim = test::ssim(back, source);
    CAPTURE(psnr);
    CAPTURE(ssim);
    CAPTURE(size);
    CHECK(psnr >= level.min_psnr);
    CHECK(ssim >= level.min_ssim);
    CHECK(size < previous);
    previous = size;
  }
}

TEST_CASE("image_modern/heif: HEIC round trips meet quality floors and shrink with quality") {
  const ir::Image source = single(photo(96, 64));
  struct Level {
    int quality;
    double min_psnr;
  };
  const Level levels[] = {{90, 33.5}, {75, 31.5}, {50, 28.0}};
  std::size_t previous = SIZE_MAX;
  for (const Level& level : levels) {
    INFO(level.quality);
    std::size_t size = 0;
    const ir::Image back = round_trip("heic", source, std::format(R"({{"quality":{}}})", level.quality), &size);
    REQUIRE(back.valid());
    const double psnr = test::psnr(back, source);
    CAPTURE(psnr);
    CAPTURE(size);
    CHECK(psnr >= level.min_psnr);
    CHECK(size < previous);
    previous = size;
  }
}

TEST_CASE("image_modern/heif: odd sizes survive (AVIF directly, HEVC through a clean aperture)") {
  const ir::Image source = single(photo(33, 21));
  for (const char* format : {"avif", "heic", "heif"}) {
    INFO(format);
    const ir::Image back = round_trip(format, source);
    REQUIRE(back.valid());
    CHECK(back.width() == 33);
    CHECK(back.height() == 21);
    CHECK(test::psnr(back, source) >= 27.5);  // small and edge-heavy at 4:2:0
  }
}

TEST_CASE("image_modern/heif: lossless AVIF is exact") {
  const ir::Image source = single(test::noise_frame(40, 24, 3));
  ir::Image opaque = source;
  for (std::size_t i = 3; i < opaque.frames[0].rgba.size(); i += 4) opaque.frames[0].rgba[i] = 255;
  const ir::Image back = round_trip("avif", opaque, R"({"lossless":true})");
  CHECK(test::pixels_equal(back, opaque));
  // With alpha, both items are lossless.
  const ir::Image with_alpha = single(test::alpha_gradient_frame(40, 24));
  const ir::Image back_alpha = round_trip("avif", with_alpha, R"({"lossless":true})");
  CHECK(test::pixels_equal(back_alpha, with_alpha));
}

TEST_CASE("image_modern/heif: alpha is stored as an auxiliary image") {
  const ir::Image source = single(test::alpha_gradient_frame(64, 40));
  for (const char* format : {"avif", "heic"}) {
    INFO(format);
    const auto encoded = test::write_from(format, source, R"({"keep_alpha":true})");
    REQUIRE_OK(encoded);
    CHECK(contains(*encoded, "auxl"));
    CHECK(contains(*encoded, format == std::string_view("avif") ? "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha"
                                                                : "urn:mpeg:hevc:2015:auxid:1"));
    const auto back = test::read_as<ir::Image>(format, *encoded);
    REQUIRE_OK(back);
    // Compare the alpha channel on its own.
    ir::Frame a = source.frames[0];
    ir::Frame b = back->frames[0];
    for (ir::Frame* f : {&a, &b}) {
      for (std::size_t i = 0; i < f->rgba.size(); i += 4) {
        f->rgba[i] = f->rgba[i + 1] = f->rgba[i + 2] = f->rgba[i + 3];
        f->rgba[i + 3] = 255;
      }
    }
    const double psnr = test::psnr(a, b);
    CAPTURE(psnr);
    CHECK(psnr >= 38.0);
  }
  // Opaque pictures get no alpha item.
  const auto opaque = test::write_from("avif", single(photo(16, 16)));
  REQUIRE_OK(opaque);
  CHECK_FALSE(contains(*opaque, "auxl"));
  // HEIC keeps transparency only on request (Apple's decoder rejects 4:2:0
  // alpha items); by default it is blended onto the background, with a note.
  std::vector<std::string> warnings;
  const auto flat = test::write_from("heic", source, R"({"background":"000000"})", &warnings);
  REQUIRE_OK(flat);
  CHECK_FALSE(contains(*flat, "auxl"));
  CHECK(warnings.size() == 1);
  const auto back = test::read_as<ir::Image>("heic", *flat);
  REQUIRE_OK(back);
  CHECK_FALSE(back->frames[0].has_transparency());
  ir::Frame expected = source.frames[0];
  image::flatten_alpha(expected, {0, 0, 0});
  CHECK(test::psnr(back->frames[0], expected) >= 34.0);
}

TEST_CASE("image_modern/heif: 10-bit AVIF keeps more than 8 bits") {
  // A shallow 16-bit ramp: 8-bit storage would band it.
  ir::Image source;
  ir::HdrPixels hdr;
  hdr.width = 256;
  hdr.height = 8;
  hdr.rgba.resize(std::size_t{hdr.width} * hdr.height * 4);
  for (std::uint32_t y = 0; y < hdr.height; ++y) {
    for (std::uint32_t x = 0; x < hdr.width; ++x) {
      const std::size_t at = (std::size_t{y} * hdr.width + x) * 4;
      const float v = image::srgb_to_linear(0.25f + 0.25f * static_cast<float>(x) / 255.0f);
      hdr.rgba[at] = hdr.rgba[at + 1] = hdr.rgba[at + 2] = v;
      hdr.rgba[at + 3] = 1.0f;
    }
  }
  source.frames.push_back(image::from_linear(hdr));
  source.hdr = hdr;
  source.source_bit_depth = 16;

  const ir::Image ten = round_trip("avif", source, R"({"quality":95})");
  REQUIRE(ten.hdr.has_value());
  CHECK(ten.source_bit_depth == 10);
  const ir::Image eight = round_trip("avif", source, R"({"quality":95,"bit_depth":"8"})");
  CHECK_FALSE(eight.hdr.has_value());
  CHECK(eight.source_bit_depth == 8);
  // Count distinct encoded levels along the ramp: 10 bit keeps ~256, 8 bit ~64.
  const auto levels = [](const ir::HdrPixels& p) {
    std::vector<std::uint16_t> codes = image::srgb16_from_linear(p);
    std::vector<std::uint16_t> row;
    for (std::uint32_t x = 0; x < p.width; ++x) row.push_back(static_cast<std::uint16_t>(codes[x * 4] >> 6));
    std::sort(row.begin(), row.end());
    return std::unique(row.begin(), row.end()) - row.begin();
  };
  CAPTURE(levels(*ten.hdr));
  CHECK(levels(*ten.hdr) > 150);
  // Lossless 10 bit is exact in 10 bits.
  const ir::Image lossless = round_trip("avif", source, R"({"lossless":true,"bit_depth":"10"})");
  REQUIRE(lossless.hdr.has_value());
  const std::vector<std::uint16_t> want = image::srgb16_from_linear(hdr);
  const std::vector<std::uint16_t> got = image::srgb16_from_linear(*lossless.hdr);
  int worst = 0;
  for (std::size_t i = 0; i < want.size(); ++i) {
    worst = std::max(worst, std::abs(static_cast<int>(want[i] * 1023 / 65535) - static_cast<int>(got[i] * 1023 / 65535)));
  }
  CHECK(worst <= 1);
}

TEST_CASE("image_modern/heif: 4:4:4 keeps colour edges that 4:2:0 blurs") {
  const ir::Image source = single(test::checker_frame(64, 64, 1));
  ir::Image coloured = source;
  for (std::size_t i = 0; i < coloured.frames[0].rgba.size(); i += 4) {
    if (coloured.frames[0].rgba[i] > 128) coloured.frames[0].rgba[i + 1] = 0;  // red/black checker
  }
  const ir::Image sub = round_trip("avif", coloured, R"({"quality":95,"chroma":"420"})");
  const ir::Image full = round_trip("avif", coloured, R"({"quality":95,"chroma":"444"})");
  const double p420 = test::psnr(sub, coloured);
  const double p444 = test::psnr(full, coloured);
  CAPTURE(p420);
  CAPTURE(p444);
  CHECK(p444 > p420 + 3.0);
}

TEST_CASE("image_modern/heif: options (effort, size, brands)") {
  const ir::Image source = single(photo(80, 60));
  for (const char* effort : {"fast", "balanced", "best"}) {
    INFO(effort);
    for (const char* format : {"avif", "heic"}) {
      const ir::Image back = round_trip(format, source, std::format(R"({{"effort":"{}"}})", effort));
      CHECK(test::psnr(back, source) >= 33.0);
    }
  }
  const ir::Image smaller = round_trip("heic", source, R"({"max_width":40})");
  CHECK(smaller.width() == 40);
  CHECK(smaller.height() == 30);
  const auto avif = test::write_from("avif", source);
  const auto heic = test::write_from("heic", source);
  const auto heif = test::write_from("heif", source);
  REQUIRE_OK(avif);
  REQUIRE_OK(heic);
  REQUIRE_OK(heif);
  CHECK(as_chars(*avif).substr(4, 8) == "ftypavif");
  CHECK(as_chars(*heic).substr(4, 8) == "ftypheic");
  CHECK(as_chars(*heif).substr(4, 8) == "ftypmif1");
  CHECK_STATUS(test::write_from("avif", source, R"({"bit_depth":"12"})"), Status::InvalidArgument);
  // Only the first frame of an animation is written, with a warning.
  ir::Image animation = source;
  animation.frames.push_back(source.frames[0]);
  std::vector<std::string> warnings;
  CHECK_OK(test::write_from("heic", animation, {}, &warnings));
  CHECK(warnings.size() == 1);
}

TEST_CASE("image_modern/heif: metadata is written only when strip_metadata is false") {
  ir::Image source = single(photo(32, 24));
  // A minimal big-endian EXIF block with Orientation 1 and Software "Pattern".
  const std::string_view exif(
      "MM\0*\0\0\0\x08\0\x02\x01\x12\0\x03\0\0\0\x01\0\x01\0\0\x01\x31\0\x02\0\0\0\x08\0\0\0\x26\0\0\0\0Pattern\0", 46);
  source.exif = to_bytes(exif);
  source.xmp = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF/></x:xmpmeta>";
  source.icc_profile = to_bytes(std::string_view("not really a profile but bytes to carry"));
  for (const char* format : {"avif", "heic"}) {
    INFO(format);
    const auto stripped = test::write_from(format, source);
    REQUIRE_OK(stripped);
    CHECK_FALSE(contains(*stripped, "Exif"));
    CHECK_FALSE(contains(*stripped, "xmpmeta"));
    CHECK_FALSE(contains(*stripped, "prof"));
    const auto kept = test::write_from(format, source, R"({"strip_metadata":false})");
    REQUIRE_OK(kept);
    const auto back = test::read_as<ir::Image>(format, *kept);
    REQUIRE_OK(back);
    CHECK(back->exif == source.exif);
    CHECK(back->metadata.at("Software") == "Pattern");
    CHECK(back->xmp == source.xmp);
    CHECK(back->icc_profile == source.icc_profile);
  }
}

TEST_CASE("image_modern/heif: HEIC beyond one HEVC picture is written as a tile grid") {
  // 16900 pixels exceed HEVC's 16888-sample line limit.
  const ir::Image source = single(test::gradient_frame(16900, 8));
  const auto encoded = test::write_from("heic", source, R"({"quality":75,"effort":"fast"})");
  REQUIRE_OK(encoded);
  CHECK(contains(*encoded, "grid"));
  const auto back = test::read_as<ir::Image>("heic", *encoded);
  REQUIRE_OK(back);
  CHECK(back->width() == 16900);
  CHECK(back->height() == 8);
  CHECK(test::psnr(*back, source) >= 28.0);
}
