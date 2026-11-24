// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// OpenEXR reader: files written by the reference OpenEXR library (see
// core/tests/data/image_modern/exr/SOURCES.md) decoded and checked against
// the known sample values or the reference library's own decode.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/image/image_ops.hpp"
#include "ir/image.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

constexpr std::uint32_t kW = 37;
constexpr std::uint32_t kH = 23;

ir::Image load(const std::string& name, std::vector<std::string>* warnings = nullptr,
               std::string_view options = {}) {
  auto image = test::read_as<ir::Image>("exr", test::fixture("image_modern/exr/" + name), options, warnings);
  if (!image) {
    FAIL_CHECK("decoding ", name, ": ", image.error().message);
    return {};
  }
  return std::move(*image);
}

/// The RGB test pattern of the fixtures (exact in half precision).
double pattern(std::uint32_t c, double x, double y) {
  if (c == 0) return x / 8.0;
  if (c == 1) return y / 4.0;
  return 0.25 + (x + y) / 64.0;
}

/// The busy pattern of the noise_* fixtures.
double busy(std::uint32_t c, double x, double y) {
  const double noise = std::fmod(x * 7919 + y * 104729, 1000.0) / 250.0;
  if (c == 0) return noise;
  if (c == 1) return std::fmod(noise + x / 16.0, 3.0);
  return 0.5 + std::sin(x / 3.0) * std::cos(y / 2.0);
}

float at(const ir::HdrPixels& p, std::uint32_t x, std::uint32_t y, std::uint32_t c) {
  return p.rgba[(std::size_t{y} * p.width + x) * 4 + c];
}

/// Largest |decoded - pattern| over the RGB channels (and alpha == 1 check).
double pattern_error(const ir::Image& image, double (*fn)(std::uint32_t, double, double), bool* opaque = nullptr) {
  if (!image.hdr || image.hdr->width != kW || image.hdr->height != kH) return 1e9;
  double worst = 0;
  bool all_opaque = true;
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      for (std::uint32_t c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(at(*image.hdr, x, y, c) - fn(c, x, y)));
      all_opaque = all_opaque && at(*image.hdr, x, y, 3) == 1.0f;
    }
  }
  if (opaque != nullptr) *opaque = all_opaque;
  return worst;
}

/// Half floats -> float, IEEE binary16 (independent of the codec).
float half_value(std::uint16_t h) {
  const std::uint32_t exponent = (h >> 10) & 0x1Fu;
  const std::uint32_t mantissa = h & 0x3FFu;
  double v = exponent == 0 ? std::ldexp(mantissa, -24) : std::ldexp(1024.0 + mantissa, static_cast<int>(exponent) - 25);
  if (exponent == 31) v = mantissa == 0 ? INFINITY : NAN;
  return static_cast<float>((h & 0x8000u) != 0 ? -v : v);
}

/// A reference decode stored as interleaved little-endian halves.
std::vector<float> reference(const std::string& name) {
  const Bytes data = test::fixture("image_modern/exr/" + name);
  std::vector<float> out(data.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = half_value(static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(data[2 * i]) |
                                                   (std::to_integer<std::uint16_t>(data[2 * i + 1]) << 8)));
  }
  return out;
}

/// Largest difference to a reference in units of the half precision step at
/// that magnitude (0 = bit-exact).
double ulp_error(const ir::HdrPixels& p, const std::vector<float>& ref, std::uint32_t channels) {
  double worst = 0;
  for (std::size_t i = 0; i < std::size_t{p.width} * p.height; ++i) {
    for (std::uint32_t c = 0; c < channels; ++c) {
      const float r = ref[i * channels + c];
      const float v = p.rgba[i * 4 + c];
      const double step = std::ldexp(1.0, std::max(std::ilogb(std::max(std::fabs(r), 6.1e-5f)), -14) - 10);
      worst = std::max(worst, std::fabs(static_cast<double>(v) - r) / step);
    }
  }
  return worst;
}

}  // namespace

TEST_CASE("image_modern/exr: lossless compressions decode the exact half values") {
  for (const char* method : {"none", "rle", "zips", "zip", "piz", "pxr24"}) {
    INFO(method);
    const ir::Image image = load(std::string("rgb_half_") + method + ".exr");
    REQUIRE(image.hdr.has_value());
    CHECK_EQ(image.width(), kW);
    CHECK_EQ(image.height(), kH);
    CHECK_EQ(image.frames.size(), 1u);
    CHECK_EQ(image.source_bit_depth, 16);
    bool opaque = false;
    CHECK_EQ(pattern_error(image, pattern, &opaque), 0.0);
    CHECK(opaque);
    // The 8-bit rendition is the tone-mapped HDR picture.
    CHECK(test::pixels_equal(image.frames[0], image::tone_map(*image.hdr)));
  }
}

TEST_CASE("image_modern/exr: B44, B44A and DWA match the reference decoder") {
  struct Case {
    const char* name;
    double (*fn)(std::uint32_t, double, double);
    double max_ulps;           // against the reference library's decode
    double max_pattern_error;  // against the original samples (lossy)
  };
  // B44 is integer arithmetic: bit-exact. DWA is a float DCT: allow one
  // half-precision step for platform rounding differences.
  const Case cases[] = {
      {"rgb_half_b44", pattern, 0, 0.0},       {"rgb_half_b44a", pattern, 0, 0.0},
      {"noise_b44", busy, 0, 0.9},             {"rgb_half_dwaa", pattern, 1, 0.035},
      {"rgb_half_dwab", pattern, 1, 0.035},    {"noise_float_dwab", busy, 1, 0.07},
  };
  for (const Case& c : cases) {
    INFO(c.name);
    const ir::Image image = load(std::string(c.name) + ".exr");
    REQUIRE(image.hdr.has_value());
    CHECK_LE(ulp_error(*image.hdr, reference(std::string(c.name) + ".ref"), 3), c.max_ulps);
    CHECK_LE(pattern_error(image, c.fn), c.max_pattern_error);
  }
  CHECK_EQ(load("noise_float_dwab.exr").source_bit_depth, 32);
}

TEST_CASE("image_modern/exr: DWA with RLE alpha and a zlib channel") {
  std::vector<std::string> warnings;
  const ir::Image image = load("rgbaz_dwaa.exr", &warnings);
  REQUIRE(image.hdr.has_value());
  const std::vector<float> ref = reference("rgbaz_dwaa.ref");
  double worst = 0;
  for (std::size_t i = 0; i < std::size_t{kW} * kH; ++i) {
    const float alpha = image.hdr->rgba[i * 4 + 3];
    CHECK_EQ(alpha, ref[i * 4 + 3]);  // alpha is lossless
    for (std::size_t c = 0; c < 3; ++c) {
      worst = std::max(worst, std::fabs(static_cast<double>(image.hdr->rgba[i * 4 + c]) * alpha - ref[i * 4 + c]));
    }
  }
  CHECK_LE(worst, 1e-3);
  REQUIRE_EQ(warnings.size(), 1u);
  CHECK(warnings[0].find("(Z)") != std::string::npos);
}

TEST_CASE("image_modern/exr: 32-bit float and unsigned int channels") {
  for (const char* name : {"rgb_float_zip.exr", "rgb_float_piz.exr", "rgb_float_pxr24.exr"}) {
    INFO(name);
    const ir::Image image = load(name);
    REQUIRE(image.hdr.has_value());
    CHECK_EQ(image.source_bit_depth, 32);
    // PXR24 keeps 15 of the 23 mantissa bits.
    const double tolerance = std::string_view(name).find("pxr24") != std::string_view::npos ? 1.0 / 32768 : 0.0;
    double worst = 0;
    for (std::uint32_t y = 0; y < kH; ++y) {
      for (std::uint32_t x = 0; x < kW; ++x) {
        const float expected[3] = {static_cast<float>(x / 3.0), static_cast<float>(y * 0.1),
                                   static_cast<float>(1000.5 + x)};
        for (std::uint32_t c = 0; c < 3; ++c) {
          const double e = expected[c];
          worst = std::max(worst, std::fabs(at(*image.hdr, x, y, c) - e) / std::max(std::fabs(e), 1e-3));
        }
      }
    }
    CHECK_LE(worst, tolerance);
  }
  const ir::Image uint = load("rgb_uint_rle.exr");
  REQUIRE(uint.hdr.has_value());
  CHECK_EQ(uint.source_bit_depth, 32);
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      CHECK_EQ(at(*uint.hdr, x, y, 0), static_cast<float>(x * 1000));
      CHECK_EQ(at(*uint.hdr, x, y, 1), static_cast<float>(y));
      CHECK_EQ(at(*uint.hdr, x, y, 2), static_cast<float>(70000 + x * y));
    }
  }
}

TEST_CASE("image_modern/exr: tiled parts (one level, mipmap, ripmap) and line order") {
  for (const char* name : {"tiled_zip.exr", "mipmap_piz.exr", "ripmap_rle.exr", "decreasing_zip.exr"}) {
    INFO(name);
    const ir::Image image = load(name);
    CHECK_EQ(pattern_error(image, pattern), 0.0);
  }
}

TEST_CASE("image_modern/exr: the data window is placed inside the display window") {
  const ir::Image image = load("datawindow_zip.exr");
  REQUIRE(image.hdr.has_value());
  REQUIRE_EQ(image.width(), 40u);
  REQUIRE_EQ(image.height(), 30u);
  for (std::uint32_t y = 0; y < 30; ++y) {
    for (std::uint32_t x = 0; x < 40; ++x) {
      const bool inside = x >= 5 && x <= 24 && y >= 3 && y <= 20;
      INFO(x, ",", y);
      for (std::uint32_t c = 0; c < 3; ++c) {
        CHECK_EQ(at(*image.hdr, x, y, c), inside ? static_cast<float>(pattern(c, x, y)) : 0.0f);
      }
      CHECK_EQ(at(*image.hdr, x, y, 3), inside ? 1.0f : 0.0f);
      CHECK_EQ(image.frames[0].rgba[image.frames[0].offset(x, y) + 3], inside ? 255 : 0);
    }
  }
}

TEST_CASE("image_modern/exr: premultiplied alpha, luminance and luminance-chroma") {
  const ir::Image rgba = load("rgba_half_piz.exr");
  REQUIRE(rgba.hdr.has_value());
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      const float alpha = static_cast<float>(((x + y) % 4 + 1) / 4.0);
      CHECK_EQ(at(*rgba.hdr, x, y, 3), alpha);
      for (std::uint32_t c = 0; c < 3; ++c) CHECK_EQ(at(*rgba.hdr, x, y, c), static_cast<float>(pattern(c, x, y)));
      CHECK_EQ(rgba.frames[0].rgba[rgba.frames[0].offset(x, y) + 3], static_cast<int>(std::lround(alpha * 255)));
    }
  }

  const ir::Image ya = load("ya_zip.exr");
  REQUIRE(ya.hdr.has_value());
  CHECK(ya.source_grayscale);
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      const float alpha = static_cast<float>(((x + y) % 4 + 1) / 4.0);
      const double luminance = x / 8.0 + y / 16.0;
      CHECK_EQ(at(*ya.hdr, x, y, 3), alpha);
      for (std::uint32_t c = 0; c < 3; ++c) CHECK(std::fabs(at(*ya.hdr, x, y, c) - luminance) <= luminance * 1e-3);
    }
  }

  // RGB stored by the reference library as Y + 2x2-subsampled RY/BY.
  const ir::Image yc = load("luma_chroma_piz.exr");
  REQUIRE(yc.hdr.has_value());
  REQUIRE_EQ(yc.width(), 36u);
  REQUIRE_EQ(yc.height(), 22u);
  CHECK_FALSE(yc.source_grayscale);
  double worst = 0;
  for (std::uint32_t y = 0; y < 22; ++y) {
    for (std::uint32_t x = 0; x < 36; ++x) {
      const double v = 0.5 + x / 8.0 + y / 16.0;
      const double expected[3] = {v * 1.25, v, v * 0.75};
      for (std::uint32_t c = 0; c < 3; ++c) {
        worst = std::max(worst, std::fabs(at(*yc.hdr, x, y, c) - expected[c]) / expected[c]);
      }
    }
  }
  CHECK_LE(worst, 0.01);
}

TEST_CASE("image_modern/exr: layers, multi-part files and chromaticities") {
  std::vector<std::string> warnings;
  const ir::Image layers = load("layers_zip.exr", &warnings);
  CHECK_EQ(pattern_error(layers, pattern), 0.0);
  REQUIRE_EQ(warnings.size(), 1u);
  CHECK(warnings[0].find("(Z)") != std::string::npos);

  const ir::Image parts = load("multipart.exr");
  REQUIRE_EQ(parts.frames.size(), 3u);
  CHECK_EQ(pattern_error(parts, pattern), 0.0);  // hdr = first part
  // Part 2 mirrors R (4.5 - x/8): bright on the left where part 1 is dark.
  CHECK_GT(parts.frames[1].rgba[0], parts.frames[0].rgba[0]);
  // Part 3: a 4x4 grey patch at (10,10) in an otherwise empty display window.
  const ir::Frame& patch = parts.frames[2];
  REQUIRE_EQ(patch.width, kW);
  CHECK_EQ(patch.rgba[patch.offset(0, 0) + 3], 0);
  CHECK_EQ(patch.rgba[patch.offset(11, 12) + 3], 255);
  CHECK_EQ(patch.rgba[patch.offset(11, 12)], image::linear_to_srgb8(0.75f));

  // ACES AP0 primaries are converted to Rec.709 (Bradford D60 -> D65).
  const ir::Image aces = load("aces_zip.exr");
  REQUIRE(aces.hdr.has_value());
  const double red[3] = {2.52169, -0.27648, -0.01538};
  const double blue[3] = {-0.38756, -0.09624, 1.16835};
  for (std::uint32_t c = 0; c < 3; ++c) {
    CHECK(std::fabs(at(*aces.hdr, 0, 0, c) - red[c]) < 5e-3);
    CHECK(std::fabs(at(*aces.hdr, 2, 1, c) - blue[c]) < 5e-3);
    CHECK(std::fabs(at(*aces.hdr, 3, 0, c) - 0.5) < 2e-3);
  }
}

TEST_CASE("image_modern/exr: tone mapping options") {
  const ir::Image plain = load("rgb_half_zip.exr");
  const ir::Image darker = load("rgb_half_zip.exr", nullptr, R"({"exposure":-3})");
  const ir::Image clamped = load("rgb_half_zip.exr", nullptr, R"({"tone_map":"clamp"})");
  REQUIRE(plain.valid());
  REQUIRE(darker.valid());
  REQUIRE(clamped.valid());
  CHECK(plain.hdr->rgba == darker.hdr->rgba);  // only the 8-bit rendition changes
  const auto sum = [](const ir::Frame& f) {
    std::uint64_t s = 0;
    for (const std::uint8_t v : f.rgba) s += v;
    return s;
  };
  CHECK_LT(sum(darker.frames[0]), sum(plain.frames[0]));
  // Clamping keeps everything above 1.0 at full white: (36, 0) has R = 4.5.
  CHECK_EQ(clamped.frames[0].rgba[clamped.frames[0].offset(36, 0)], 255);
  CHECK(clamped.frames[0].rgba != plain.frames[0].rgba);
}

TEST_CASE("image_modern/exr: detection and conversion") {
  const Bytes data = test::fixture("image_modern/exr/rgb_half_piz.exr");
  const auto detected = detect_format(Registry::instance(), data, "");
  REQUIRE(detected.has_value());
  CHECK_EQ(detected->format->id, "exr");
  const auto by_name = detect_format(Registry::instance(), {}, "render.EXR");
  REQUIRE(by_name.has_value());
  CHECK_EQ(by_name->format->id, "exr");
  const test::Converted png = test::convert(data, "", "png");
  REQUIRE(png.ok());
  const auto decoded = test::read_as<ir::Image>("png", png.data);
  REQUIRE_OK(decoded);
  CHECK(test::pixels_equal(decoded->frames[0], load("rgb_half_piz.exr").frames[0]));
}
