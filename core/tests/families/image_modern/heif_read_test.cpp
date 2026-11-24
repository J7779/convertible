// Tests of the HEIF readers (avif, heic, heif) on files written by libheif
// (heif-enc with aom/x265) and by Apple's encoder (macOS sips), compared with
// libheif's own decodes (heif-dec) and with the synthetic source pattern.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/image/image_ops.hpp"
#include "ir/image.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"

using namespace cvt;

namespace {

/// The synthetic test card of tests/data/image_modern/heif/make_sources.py.
std::array<std::uint8_t, 3> card_pixel(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
  auto r = static_cast<int>(x * 255 / std::max<std::uint32_t>(1, w - 1));
  auto g = static_cast<int>(y * 255 / std::max<std::uint32_t>(1, h - 1));
  auto b = static_cast<int>((x + y) * 255 / std::max<std::uint32_t>(1, w + h - 2));
  if ((x / 8 + y / 8) % 5 == 0) {
    r = 255 - r;
    g = 255 - g;
    b = 255 - b;
  }
  if (x >= w / 3 && x < w / 3 + 6 && y >= h / 4 && y < h / 4 + 6) {
    r = 255;
    g = 255;
    b = 0;
  }
  return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b)};
}

/// The card evaluated on a `pattern_w` x `pattern_h` grid, point-sampled to
/// `width` x `height`, shifted horizontally by `shift` pattern pixels.
ir::Frame card(std::uint32_t width, std::uint32_t height, std::uint32_t pattern_w = 0, std::uint32_t pattern_h = 0,
               std::uint32_t shift = 0) {
  if (pattern_w == 0) pattern_w = width;
  if (pattern_h == 0) pattern_h = height;
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint32_t px = (x * pattern_w / width + shift) % pattern_w;
      const std::uint32_t py = y * pattern_h / height;
      const auto rgb = card_pixel(px, py, pattern_w, pattern_h);
      const std::size_t at = frame.offset(x, y);
      frame.rgba[at] = rgb[0];
      frame.rgba[at + 1] = rgb[1];
      frame.rgba[at + 2] = rgb[2];
    }
  }
  return frame;
}

std::string data(std::string_view name) { return std::string("image_modern/heif/") + std::string(name); }

ir::Image load(std::string_view format, std::string_view name, std::string_view options = {}) {
  auto image = test::read_as<ir::Image>(format, test::fixture(data(name)), options);
  if (!image) {
    FAIL_CHECK("decoding ", name, ": ", image.error().message);
    return {};
  }
  return std::move(*image);
}

/// libheif repeats each 4:2:0 chroma sample; libavif (avifdec, browsers)
/// interpolates. Comparisons with libheif's decodes select the former.
constexpr std::string_view like_libheif = R"({"chroma_upsampling":"nearest"})";

/// An independent decoder's output for a fixture (PNG written by heif-dec or avifdec).
ir::Image reference(std::string_view name) { return load("png", name); }

ir::Frame crop(const ir::Frame& frame, std::uint32_t left, std::uint32_t top, std::uint32_t width,
               std::uint32_t height) {
  ir::Frame out = ir::Frame::filled(width, height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      for (std::size_t c = 0; c < 4; ++c) out.rgba[out.offset(x, y) + c] = frame.rgba[frame.offset(left + x, top + y) + c];
    }
  }
  return out;
}

struct Case {
  const char* format;
  const char* file;
  const char* ref;
  std::uint32_t width;
  std::uint32_t height;
  double min_psnr;  // against libheif's decode of the same file
  double min_ssim;
};

}  // namespace

TEST_CASE("image_modern/heif: decodes libheif and Apple files like libheif does") {
  // Differences come only from YCbCr->RGB conversion (chroma upsampling and
  // rounding); the coded pictures are identical.
  const Case cases[] = {
      {"avif", "avif_420.avif", "ref_avif_420.png", 64, 48, 38.0, 0.985},
      {"avif", "avif_444_10bit.avif", "ref_avif_444_10bit.png", 48, 32, 45.0, 0.995},
      {"avif", "avif_alpha.avif", "ref_avif_alpha.png", 48, 32, 38.0, 0.98},
      {"avif", "avif_grid.avif", "ref_avif_grid.png", 80, 48, 38.0, 0.985},
      {"avif", "avif_sips.avif", "ref_avif_sips.png", 64, 48, 38.0, 0.985},
      {"heic", "heic_420.heic", "ref_heic_420.png", 64, 48, 38.0, 0.985},
      {"heic", "heic_10bit.heic", "ref_heic_10bit.png", 48, 32, 42.0, 0.99},
      {"heic", "heic_grid.heic", "ref_heic_grid.png", 80, 48, 38.0, 0.985},
      {"heic", "heic_alpha.heic", "ref_heic_alpha.png", 48, 32, 38.0, 0.98},
      {"heic", "heic_icc.heic", "ref_heic_icc.png", 32, 24, 38.0, 0.985},
      {"heic", "heic_irot_imir.heic", "ref_heic_irot_imir.png", 48, 64, 38.0, 0.985},
      {"heic", "heic_thumb.heic", "ref_heic_thumb.png", 33, 21, 38.0, 0.985},
      {"heic", "heic_sips.heic", "ref_heic_sips.png", 64, 48, 38.0, 0.985},
      {"heic", "heic_sips_10bit.heic", "ref_heic_sips_10bit.png", 48, 32, 42.0, 0.99},
      {"heic", "heic_sips_alpha.heic", "ref_heic_sips_alpha.png", 48, 32, 38.0, 0.98},
      {"heic", "heic_sips_exif6.heic", "ref_heic_sips_exif6.png", 48, 64, 38.0, 0.985},
  };
  for (const Case& c : cases) {
    INFO(c.file);
    const ir::Image ours = load(c.format, c.file, like_libheif);
    const ir::Image theirs = reference(c.ref);
    REQUIRE(ours.valid());
    REQUIRE(theirs.valid());
    CHECK(ours.width() == c.width);
    CHECK(ours.height() == c.height);
    const double psnr = test::psnr(ours.frames[0], theirs.frames[0]);
    const double ssim = test::ssim(ours.frames[0], theirs.frames[0]);
    CAPTURE(psnr);
    CAPTURE(ssim);
    CHECK(psnr >= c.min_psnr);
    CHECK(ssim >= c.min_ssim);
  }
}

TEST_CASE("image_modern/heif: by default 4:2:0 colour is interpolated as libavif does") {
  // avifdec (libavif 1.0.4 with libyuv) decodes of libheif- and Apple-written files.
  const std::pair<const char*, const char*> cases[] = {
      {"avif_420.avif", "avifdec_avif_420.png"},
      {"avif_alpha.avif", "avifdec_avif_alpha.png"},
      {"avif_sips.avif", "avifdec_avif_sips.png"},
  };
  for (const auto& [file, ref] : cases) {
    INFO(file);
    const ir::Image ours = load("avif", file);
    const ir::Image theirs = reference(ref);
    const double psnr = test::psnr(ours.frames[0], theirs.frames[0]);
    CAPTURE(psnr);
    CHECK(psnr >= 45.0);
  }
}

TEST_CASE("image_modern/heif: animated AVIF written by avifenc") {
  const ir::Image image = load("avif", "avif_anim_avifenc.avif");
  REQUIRE(image.frames.size() == 3);
  for (const ir::Frame& frame : image.frames) CHECK(frame.delay_ms == 100);
  CAPTURE(image.loop_count);
  CHECK(image.loop_count == 0);  // avifenc's default: repeat forever
  const double psnr = test::psnr(image.frames[1], reference("avifdec_avif_anim_avifenc_1.png").frames[0]);
  CAPTURE(psnr);
  CHECK(psnr >= 45.0);
  CHECK(test::psnr(image.frames[1], card(32, 24, 32, 24, 16)) >= 28.0);
}

TEST_CASE("image_modern/heif: lossless AVIF (identity matrix, 4:4:4) is exact") {
  const ir::Image image = load("avif", "avif_lossless.avif");
  const ir::Image theirs = reference("ref_avif_lossless.png");
  CHECK(test::pixels_equal(image.frames[0], theirs.frames[0]));
  CHECK(test::pixels_equal(image.frames[0], card(33, 21)));
}

TEST_CASE("image_modern/heif: decodes match the source pattern") {
  struct Source {
    const char* format;
    const char* file;
    ir::Frame expected;
    double min_psnr;
  };
  const Source sources[] = {
      {"avif", "avif_420.avif", card(64, 48), 27.5},  // heif-enc -q 75; interpolated chroma blurs the hard block edges
      {"heic", "heic_420.heic", card(64, 48), 27.5},
      {"heic", "heic_sips.heic", card(64, 48), 27.5},  // Apple, quality 0.8
      {"avif", "avif_grid.avif", card(80, 48), 27.5},
      {"heic", "heic_grid.heic", card(80, 48), 27.5},
  };
  for (const Source& s : sources) {
    INFO(s.file);
    const ir::Image image = load(s.format, s.file);
    const double psnr = test::psnr(image.frames[0], s.expected);
    CAPTURE(psnr);
    CHECK(psnr >= s.min_psnr);
  }
}

TEST_CASE("image_modern/heif: Apple's 512x512-tiled HEIC grid") {
  const ir::Image image = load("heic", "heic_sips_grid.heic", like_libheif);
  REQUIRE(image.valid());
  CHECK(image.width() == 1030);
  CHECK(image.height() == 520);
  // libheif's decode of the region where four tiles meet (x 440..600, y 400..520).
  const ir::Image theirs = reference("ref_heic_sips_grid_crop.png");
  const double psnr = test::psnr(crop(image.frames[0], 440, 400, 160, 120), theirs.frames[0]);
  CAPTURE(psnr);
  CHECK(psnr >= 38.0);
  const double source = test::psnr(image.frames[0], card(1030, 520, 80, 48));
  CAPTURE(source);
  CHECK(source >= 30.0);
}

TEST_CASE("image_modern/heif: 10-bit pictures keep their precision") {
  for (const auto& [format, file] : {std::pair{"avif", "avif_444_10bit.avif"}, std::pair{"heic", "heic_10bit.heic"},
                                     std::pair{"heic", "heic_sips_10bit.heic"}}) {
    INFO(file);
    const ir::Image image = load(format, file);
    CHECK(image.source_bit_depth == 10);
    REQUIRE(image.hdr.has_value());
    CHECK(image.hdr->width == image.width());
    // The frame is the sRGB encoding of the linear pixels.
    const ir::Frame back = image::from_linear(*image.hdr);
    CHECK(test::max_abs_diff(back, image.frames[0]) <= 1.0);
  }
  // 8-bit files carry no HDR copy.
  const ir::Image eight = load("heic", "heic_420.heic");
  CHECK(eight.source_bit_depth == 8);
  CHECK_FALSE(eight.hdr.has_value());
}

TEST_CASE("image_modern/heif: PQ (BT.2100) AVIF becomes linear HDR plus a tone-mapped frame") {
  const ir::Image image = load("avif", "avif_pq_10bit.avif", like_libheif);
  REQUIRE(image.hdr.has_value());
  // libheif hands out the PQ code values; turn them into linear BT.709 light
  // (1.0 = 203 nits) independently and compare.
  const ir::Image theirs = reference("ref_avif_pq_10bit.png");
  REQUIRE(theirs.hdr.has_value());
  const std::vector<std::uint16_t> codes = image::srgb16_from_linear(*theirs.hdr);
  const auto pq = [](double e) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const double p = std::pow(e, 1.0 / m2);
    return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1) / 203.0;
  };
  const double m[9] = {1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187};
  double error = 0;
  double total = 0;
  for (std::size_t i = 0; i + 3 < codes.size(); i += 4) {
    const double in[3] = {pq(codes[i] / 65535.0), pq(codes[i + 1] / 65535.0), pq(codes[i + 2] / 65535.0)};
    for (std::size_t c = 0; c < 3; ++c) {
      const double expected = std::max(0.0, m[c * 3] * in[0] + m[c * 3 + 1] * in[1] + m[c * 3 + 2] * in[2]);
      error += std::abs(expected - image.hdr->rgba[i + c]);
      total += expected;
    }
  }
  CAPTURE(error / total);
  CHECK(error / total < 0.03);
  // Bright PQ values exceed SDR white; the frame is a tone-mapped rendition.
  float peak = 0;
  for (std::size_t i = 0; i < image.hdr->rgba.size(); ++i) {
    if (i % 4 != 3) peak = std::max(peak, image.hdr->rgba[i]);
  }
  CHECK(peak > 4.0f);
  CHECK(test::pixels_equal(image.frames[0], image::tone_map(*image.hdr)));
}

TEST_CASE("image_modern/heif: alpha auxiliary images") {
  for (const auto& [format, file] : {std::pair{"avif", "avif_alpha.avif"}, std::pair{"heic", "heic_alpha.heic"},
                                     std::pair{"heic", "heic_sips_alpha.heic"}}) {
    INFO(file);
    const ir::Image image = load(format, file);
    REQUIRE(image.valid());
    const ir::Frame& f = image.frames[0];
    CHECK(f.has_transparency());
    CHECK(f.rgba[f.offset(1, 1) + 3] <= 8);                       // transparent corner
    CHECK(f.rgba[f.offset(f.width / 2, f.height / 2) + 3] >= 245);  // opaque centre
  }
}

TEST_CASE("image_modern/heif: irot/imir are applied once; EXIF orientation is not applied again") {
  // heif-enc --rotate-cw 90 --flip-h: the reference shows the intended picture.
  const ir::Image rotated = load("heic", "heic_irot_imir.heic");
  CHECK(rotated.width() == 48);
  CHECK(rotated.height() == 64);
  // sips: pixels stored as in the JPEG, irot 270 (anticlockwise) AND EXIF orientation 6.
  const ir::Image apple = load("heic", "heic_sips_exif6.heic");
  REQUIRE(apple.valid());
  CHECK(apple.width() == 48);
  CHECK(apple.height() == 64);
  ir::Frame upright = image::apply_orientation(card(64, 48), 6);
  CHECK(test::psnr(apple.frames[0], upright) >= 27.5);
  // The EXIF block is kept (for writers that keep metadata) with orientation 1.
  REQUIRE_FALSE(apple.exif.empty());
  const std::string_view tiff = as_chars(apple.exif);
  CHECK(tiff.substr(0, 4) == std::string_view("MM\0*", 4));
  const std::size_t tag = tiff.find(std::string_view("\x01\x12\x00\x03\x00\x00\x00\x01\x00", 9));
  REQUIRE(tag != std::string_view::npos);
  CHECK(static_cast<unsigned char>(tiff[tag + 9]) == 1);
  CHECK(apple.metadata.at("Software") == "TestPattern");
  CHECK(apple.dpi_x == 72.0);
  CHECK(apple.xmp.find("TestPattern") != std::string::npos);
}

TEST_CASE("image_modern/heif: ICC profiles and thumbnails") {
  const ir::Image icc = load("heic", "heic_icc.heic");
  REQUIRE(icc.icc_profile.size() > 128);
  CHECK(as_chars(icc.icc_profile).find("Test RGB") != std::string_view::npos);
  CHECK(as_chars(icc.icc_profile).substr(36, 4) == "acsp");
  // The 16-pixel thumbnail item is ignored: the primary picture is returned.
  const ir::Image thumb = load("heic", "heic_thumb.heic");
  CHECK(thumb.width() == 33);
  CHECK(thumb.height() == 21);
  CHECK(test::psnr(thumb.frames[0], card(33, 21)) >= 24.0);  // x265 at -q 80 on a tiny, edge-heavy card
}

TEST_CASE("image_modern/heif: AVIF image sequences become animations") {
  const ir::Image image = load("avif", "avif_sequence.avif", like_libheif);
  REQUIRE(image.frames.size() == 3);
  CHECK(image.width() == 32);
  CHECK(image.height() == 24);
  const std::uint32_t shifts[] = {0, 16, 32};
  for (std::size_t i = 0; i < 3; ++i) {
    INFO(i);
    CHECK(image.frames[i].delay_ms == 100);
    const double psnr = test::psnr(image.frames[i], card(32, 24, 32, 24, shifts[i]));
    CAPTURE(psnr);
    CHECK(psnr >= 28.0);
  }
  CHECK(test::psnr(image.frames[1], card(32, 24, 32, 24, 0)) < 20.0);  // the frames really differ
  // libheif decodes the first frame for its still image.
  CHECK(test::psnr(image.frames[0], reference("ref_avif_sequence.png").frames[0]) >= 38.0);
}

TEST_CASE("image_modern/heif: detection") {
  const std::pair<const char*, const char*> cases[] = {
      {"avif_420.avif", "avif"},      {"avif_sequence.avif", "avif"}, {"avif_sips.avif", "avif"},
      {"heic_420.heic", "heic"},      {"heic_sips_grid.heic", "heic"}, {"heic_alpha.heic", "heic"},
  };
  for (const auto& [file, id] : cases) {
    INFO(file);
    const Bytes bytes = test::fixture(data(file));
    const auto found = detect_format(Registry::instance(), bytes, "");
    REQUIRE(found.has_value());
    CHECK(found->format->id == id);
    const auto named = detect_format(Registry::instance(), bytes, file);
    REQUIRE(named.has_value());
    CHECK(named->format->id == id);
  }
  for (const std::string& problem : Registry::instance().validate()) {
    INFO(problem);
    CHECK(problem.find("avif") == std::string::npos);
    CHECK(problem.find("heic") == std::string::npos);
    CHECK(problem.find("heif") == std::string::npos);
  }
  CHECK(Registry::instance().resolve("heif")->id == "heif");
  CHECK(Registry::instance().resolve(".avif")->id == "avif");
}

TEST_CASE("image_modern/heif: converts through the engine, including the compress path") {
  const Bytes heic = test::fixture(data("heic_sips.heic"));
  const test::Converted png = test::convert(heic, "", "png", "", "photo.heic");
  REQUIRE(png.ok());
  const test::Converted same = test::convert(heic, "", "heic", R"({"quality":40})", "photo.heic");
  REQUIRE(same.ok());
  CHECK(same.data.size() < heic.size());
  const Bytes avif = test::fixture(data("avif_420.avif"));
  const test::Converted avif_same = test::convert(avif, "", "avif", R"({"quality":30})", "pic.avif");
  REQUIRE(avif_same.ok());
  CHECK(avif_same.data.size() < avif.size());
  const auto back = test::read_as<ir::Image>("avif", avif_same.data);
  REQUIRE_OK(back);
  CHECK(test::psnr(back->frames[0], card(64, 48)) >= 24.0);
}
