// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstdint>
#include <vector>

#include "families/image/image_ops.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

double mean_channel(const ir::Frame& frame, std::size_t channel) {
  double sum = 0;
  for (std::size_t i = channel; i < frame.rgba.size(); i += 4) sum += frame.rgba[i];
  return sum / (static_cast<double>(frame.rgba.size()) / 4.0);
}

double mean_linear(const ir::Frame& frame) {
  double sum = 0;
  for (std::size_t i = 0; i < frame.rgba.size(); i += 4) sum += image::srgb_to_linear(frame.rgba[i]);
  return sum / (static_cast<double>(frame.rgba.size()) / 4.0);
}

}  // namespace

TEST_CASE("image/ops: sRGB transfer round trips exactly") {
  for (int v = 0; v < 256; ++v) {
    CHECK(image::linear_to_srgb8(image::srgb_to_linear(static_cast<std::uint8_t>(v))) == v);
  }
  std::vector<std::uint16_t> wide;
  for (std::uint32_t v = 0; v < 65536; v += 7) wide.insert(wide.end(), {static_cast<std::uint16_t>(v), 0, 0, 65535});
  const auto linear = image::linear_from_srgb16(wide, static_cast<std::uint32_t>(wide.size() / 4), 1);
  CHECK(image::srgb16_from_linear(linear) == wide);
}

TEST_CASE("image/ops: resize keeps flat colour, energy and centre") {
  const ir::Frame flat = ir::Frame::filled(37, 23, {200, 100, 50, 255});
  for (const auto filter : {image::ResampleFilter::Lanczos3, image::ResampleFilter::Box, image::ResampleFilter::Mitchell,
                            image::ResampleFilter::Triangle}) {
    const auto small = image::resize(flat, 11, 7, {filter});
    REQUIRE_OK(small);
    CHECK(test::max_abs_diff(*small, ir::Frame::filled(11, 7, {200, 100, 50, 255})) == 0.0);
    const auto big = image::resize(flat, 80, 50, {filter});
    REQUIRE_OK(big);
    CHECK(test::max_abs_diff(*big, ir::Frame::filled(80, 50, {200, 100, 50, 255})) == 0.0);
  }
  // A checkerboard shrunk by 2 averages to the mid grey of LINEAR light (~188), not 128.
  const ir::Frame checker = test::checker_frame(64, 64, 1);
  const auto half = image::resize(checker, 32, 32, {image::ResampleFilter::Box});
  REQUIRE_OK(half);
  CHECK_APPROX(mean_channel(*half, 0), 188.0, 1.0);
  // Energy (mean linear light) is preserved by Lanczos downscaling of a gradient.
  const ir::Frame gradient = test::gradient_frame(120, 90);
  const auto third = image::resize(gradient, 40, 30);
  REQUIRE_OK(third);
  CHECK_APPROX(mean_linear(*third), mean_linear(gradient), 0.01);
  // No shift: a symmetric picture stays symmetric.
  ir::Frame dot = ir::Frame::filled(9, 9, {0, 0, 0, 255});
  dot.rgba[dot.offset(4, 4)] = 255;
  const auto up = image::resize(dot, 27, 27);
  REQUIRE_OK(up);
  CHECK(up->rgba[up->offset(13, 13)] > 200);
  CHECK(up->rgba[up->offset(12, 13)] == up->rgba[up->offset(14, 13)]);
  CHECK(up->rgba[up->offset(13, 12)] == up->rgba[up->offset(13, 14)]);
}

TEST_CASE("image/ops: resize has no dark fringes at transparent edges") {
  // Opaque white next to fully transparent black: the edge must stay white.
  ir::Frame frame = ir::Frame::filled(16, 16, {0, 0, 0, 0});
  for (std::uint32_t y = 0; y < 16; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const std::size_t at = frame.offset(x, y);
      frame.rgba[at] = frame.rgba[at + 1] = frame.rgba[at + 2] = frame.rgba[at + 3] = 255;
    }
  }
  const auto small = image::resize(frame, 5, 5);
  REQUIRE_OK(small);
  for (std::size_t i = 0; i < small->rgba.size(); i += 4) {
    if (small->rgba[i + 3] > 0) CHECK(small->rgba[i] >= 250);
  }
}

TEST_CASE("image/ops: orientation") {
  ir::Frame frame = ir::Frame::filled(3, 2, {0, 0, 0, 255});
  frame.rgba[frame.offset(0, 0)] = 255;  // red marker top-left
  const ir::Frame r90 = image::rotate(frame, 90);
  CHECK(r90.width == 2);
  CHECK(r90.height == 3);
  CHECK(r90.rgba[r90.offset(1, 0)] == 255);  // top-left goes to top-right
  for (int o = 1; o <= 8; ++o) {
    const ir::Frame oriented = image::apply_orientation(frame, o);
    CHECK(oriented.rgba.size() == frame.rgba.size());
  }
  CHECK(image::rotate(image::rotate(frame, 180), 180).rgba == frame.rgba);
  CHECK(image::flip_horizontal(image::flip_horizontal(frame)).rgba == frame.rgba);
}

TEST_CASE("image/ops: quantiser quality") {
  const ir::Frame gradient = test::gradient_frame(96, 64);
  for (const auto dither : {image::Dither::None, image::Dither::FloydSteinberg, image::Dither::Ordered}) {
    image::QuantizeOptions options;
    options.dither = dither;
    const auto q = image::quantize(gradient, options);
    REQUIRE_OK(q);
    CHECK(q->palette.size() <= 256);
    const double p = test::psnr(q->to_frame(), gradient);
    INFO("dither ", static_cast<int>(dither), " psnr ", p);
    CHECK(p > (dither == image::Dither::None ? 36.0 : 32.0));
  }
  image::QuantizeOptions sixteen;
  sixteen.max_colors = 16;
  sixteen.dither = image::Dither::None;
  const auto q16 = image::quantize(gradient, sixteen);
  REQUIRE_OK(q16);
  CHECK(q16->palette.size() <= 16);
  CHECK(test::psnr(q16->to_frame(), gradient) > 24.0);
  // Exact when the colours fit.
  const ir::Frame checker = test::checker_frame(20, 20, 3);
  const auto exact = image::quantize(checker);
  REQUIRE_OK(exact);
  CHECK(exact->palette.size() == 2);
  CHECK(test::pixels_equal(exact->to_frame(), checker));
  // Alpha: transparent pixels map to a transparent entry.
  const ir::Frame alpha = test::alpha_gradient_frame(40, 40);
  const auto qa = image::quantize(alpha);
  REQUIRE_OK(qa);
  CHECK(test::psnr(qa->to_frame(), alpha) > 30.0);
  image::QuantizeOptions binary;
  binary.keep_alpha = false;
  const auto qb = image::quantize(alpha, binary);
  REQUIRE_OK(qb);
  CHECK(qb->transparent_index >= 0);
  CHECK(image::has_binary_alpha(qb->to_frame()));
}

TEST_CASE("image/ops: bit depth, greyscale, flatten, colours") {
  ir::Frame frame = test::gradient_frame(32, 32);
  CHECK(image::lossless_bit_depth(frame, false) == 8);
  image::reduce_bit_depth(frame, 4, image::Dither::None);
  CHECK(image::lossless_bit_depth(frame, false) <= 4);
  ir::Frame grey = test::gradient_frame(8, 8);
  image::to_grayscale(grey);
  CHECK(image::is_grayscale(grey));
  ir::Frame bw = test::checker_frame(8, 8, 2);
  CHECK(image::lossless_bit_depth(bw, true) == 1);
  ir::Frame transparent = ir::Frame::filled(2, 2, {0, 0, 0, 0});
  image::flatten_alpha(transparent, {10, 20, 30});
  CHECK(transparent.rgba[0] == 10);
  CHECK(transparent.rgba[3] == 255);
  CHECK(image::parse_color("#FfF").value() == image::Rgb{255, 255, 255});
  CHECK(image::parse_color("102030").value() == image::Rgb{0x10, 0x20, 0x30});
  CHECK_STATUS(image::parse_color("nope"), Status::InvalidArgument);
  ir::Frame pm = test::alpha_gradient_frame(16, 16);
  const ir::Frame original = pm;
  image::premultiply(pm);
  image::unpremultiply(pm);
  CHECK(test::psnr(pm, original) > 40.0);
}

TEST_CASE("image/ops: tone mapping") {
  ir::HdrPixels hdr;
  hdr.width = 4;
  hdr.height = 1;
  hdr.rgba = {0.0f, 0.0f, 0.0f, 1.0f, 0.18f, 0.18f, 0.18f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 50.0f, 50.0f, 50.0f, 1.0f};
  const ir::Frame mapped = image::tone_map(hdr);
  CHECK(mapped.rgba[0] == 0);
  CHECK(mapped.rgba[4] > 60);
  CHECK(mapped.rgba[8] > mapped.rgba[4]);
  CHECK(mapped.rgba[12] >= mapped.rgba[8]);
  hdr.rgba[12] = hdr.rgba[13] = hdr.rgba[14] = 0.5f;
  const ir::Frame clamped = image::tone_map(hdr);  // all <= 1: plain encoding
  CHECK(clamped.rgba[8] == 255);
  CHECK(clamped.rgba[4] == image::linear_to_srgb8(0.18f));
}
