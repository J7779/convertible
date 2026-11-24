// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// JPEG XL (imported libjxl): reference files from cjxl/djxl, round trips,
// metadata, options, limits, hostile input.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "ir/image.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

Bytes jxl_fixture(std::string_view name) { return test::fixture("image_modern/jxl/" + std::string(name)); }

ir::Image load(std::string_view format, std::string_view name) {
  auto image = test::read_as<ir::Image>(format, jxl_fixture(name));
  if (!image) {
    FAIL_CHECK("decoding ", name, ": ", image.error().message);
    return {};
  }
  return std::move(*image);
}

ir::Image jxl(std::string_view name) { return load("jxl", name); }
ir::Image png(std::string_view name) { return load("png", name); }

ir::Image single(ir::Frame frame) {
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

ir::Frame photo(std::uint32_t width, std::uint32_t height) {
  ir::Frame frame = test::gradient_frame(width, height);
  const Bytes noise = test::random_bytes(frame.rgba.size(), 5);
  for (std::size_t i = 0; i < frame.rgba.size(); ++i) {
    if (i % 4 == 3) continue;
    const int v = frame.rgba[i] + std::to_integer<int>(noise[i]) % 9 - 4;
    frame.rgba[i] = static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
  }
  return frame;
}

ir::Image decode(const Bytes& data) {
  auto image = test::read_as<ir::Image>("jxl", data);
  if (!image) {
    FAIL_CHECK("decoding our output: ", image.error().message);
    return {};
  }
  return std::move(*image);
}

/// Extended sRGB decoding (sign-symmetric), as libjxl's CMS applies it.
double srgb_eotf(double v) {
  const double a = std::fabs(v);
  const double lin = a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4);
  return v < 0 ? -lin : lin;
}

}  // namespace

TEST_CASE("image_modern/jxl: lossless reference files decode exactly") {
  CHECK(test::pixels_equal(jxl("lossless_rgb.jxl"), png("src_rgb.png")));
  const ir::Image rgba = jxl("lossless_rgba.jxl");
  CHECK(test::pixels_equal(rgba, png("src_rgba.png")));
  CHECK(rgba.frames[0].has_transparency());
  const ir::Image grey = jxl("lossless_grey.jxl");
  CHECK(test::pixels_equal(grey, png("src_grey.png")));
  CHECK(grey.source_grayscale);
  CHECK(grey.source_bit_depth == 8);
  CHECK_FALSE(grey.hdr.has_value());
}

TEST_CASE("image_modern/jxl: 16-bit and float files fill hdr") {
  const ir::Image wide = jxl("lossless_rgb16.jxl");
  const ir::Image reference = png("src_rgb16.png");
  REQUIRE(wide.hdr.has_value());
  REQUIRE(reference.hdr.has_value());
  CHECK(wide.source_bit_depth == 16);
  CHECK(image::srgb16_from_linear(*wide.hdr) == image::srgb16_from_linear(*reference.hdr));
  CHECK(test::max_abs_diff(wide, reference) <= 1.0);

  // cjxl tags PFM input as sRGB-encoded, so the decoder linearises the
  // stored values (src_float.pfm: r = 4x/31, g = 2y/23, b = 0.25 or 0.75).
  const ir::Image fl = jxl("lossless_float.jxl");
  REQUIRE(fl.hdr.has_value());
  CHECK(fl.source_bit_depth == 32);
  REQUIRE(fl.hdr->width == 32);
  REQUIRE(fl.hdr->height == 24);
  double worst = 0;
  double worst_bright = 0;
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const float* px = fl.hdr->rgba.data() + (std::size_t{y} * 32 + x) * 4;
      const double expected[3] = {srgb_eotf(4.0 * x / 31), srgb_eotf(2.0 * y / 23),
                                  srgb_eotf(0.25 + 0.5 * ((x + y) % 2))};
      for (int c = 0; c < 3; ++c) {
        const double error = std::fabs(px[c] - expected[c]) / std::max(1.0, expected[c]);
        // libjxl evaluates the sRGB curve with a rational approximation that
        // drifts for values far above 1 (1.6 % at 4.0): checked separately.
        if (expected[c] <= 1.5) {
          worst = std::max(worst, error);
        } else {
          worst_bright = std::max(worst_bright, error);
        }
      }
      CHECK(px[3] == 1.0f);
    }
  }
  CHECK(worst < 2e-3);
  CHECK(worst_bright < 0.03);
  // Values above 1 exist and the 8-bit rendition is tone mapped, not empty.
  CHECK(fl.hdr->rgba[(std::size_t{0} * 32 + 31) * 4] > 4.0f);
  CHECK(fl.frames[0].valid());
}

TEST_CASE("image_modern/jxl: lossy reference files match djxl") {
  struct Case {
    const char* file;
    const char* reference;
    double min_vs_source;
  };
  // PSNR against the noisy source picture (stated in SOURCES.md): d1 30.3 dB,
  // d3 28.7 dB, modular d2 24.2 dB; ours against djxl is about 53 dB (djxl
  // and we round the same float output slightly differently).
  const Case cases[] = {{"lossy_d1.jxl", "lossy_d1.ref.png", 29.5},
                        {"lossy_d3.jxl", "lossy_d3.ref.png", 28.0},
                        {"lossy_modular_d2.jxl", "lossy_modular_d2.ref.png", 23.5}};
  const ir::Image source = png("src_rgb.png");
  for (const Case& c : cases) {
    INFO(c.file);
    const ir::Image ours = jxl(c.file);
    const ir::Image reference = png(c.reference);
    CHECK(test::psnr(ours, reference) > 48.0);
    CHECK(test::max_abs_diff(ours, reference) <= 2.0);
    CHECK(test::psnr(ours, source) > c.min_vs_source);
  }
  const ir::Image rgba = jxl("lossy_rgba_d1.jxl");
  CHECK(test::psnr(rgba, png("lossy_rgba_d1.ref.png")) > 48.0);
  CHECK(test::psnr(rgba, png("src_rgba.png")) > 33.0);
}

TEST_CASE("image_modern/jxl: recompressed JPEG decodes like the JPEG") {
  const ir::Image recompressed = jxl("jpeg_recompressed.jxl");
  CHECK(test::psnr(recompressed, png("jpeg_recompressed.ref.png")) > 48.0);
  // Same DCT coefficients through two independent decoders.
  CHECK(test::psnr(recompressed, load("jpg", "src_jpeg.jpg")) > 38.0);
}

TEST_CASE("image_modern/jxl: animation frames, delays and loop count") {
  const ir::Image anim = jxl("anim.jxl");
  REQUIRE(anim.frames.size() == 3);
  CHECK(anim.frames[0].delay_ms == 100);
  CHECK(anim.frames[1].delay_ms == 200);
  CHECK(anim.frames[2].delay_ms == 300);
  CHECK(anim.loop_count == 0);
  CHECK(test::pixels_equal(anim, png("src_anim.png")));
}

TEST_CASE("image_modern/jxl: ICC profile, orientation and metadata boxes") {
  const ir::Image icc = jxl("icc_lossless.jxl");
  const ir::Image icc_source = png("src_icc.png");
  CHECK(test::pixels_equal(icc, icc_source));
  REQUIRE_FALSE(icc.icc_profile.empty());
  CHECK(icc.icc_profile == icc_source.icc_profile);

  // Stored 40x24 with orientation 6 (rotate 90 degrees clockwise): the red
  // top-left marker ends up top-right of the 24x40 upright picture.
  const ir::Image oriented = jxl("orient6.jxl");
  REQUIRE(oriented.width() == 24);
  REQUIRE(oriented.height() == 40);
  const ir::Frame& f = oriented.frames[0];
  const std::uint8_t* marker = f.rgba.data() + f.offset(20, 3);
  CHECK(marker[0] == 255);
  CHECK(marker[1] == 0);
  CHECK(f.rgba[f.offset(3, 3)] != 255);
  REQUIRE_FALSE(oriented.exif.empty());
  CHECK(image::parse_exif(oriented.exif)->orientation == 1);

  for (const char* name : {"exif_boxes.jxl", "exif_plain.jxl"}) {
    INFO(name);
    const ir::Image meta = jxl(name);
    REQUIRE_FALSE(meta.exif.empty());
    const auto exif = image::parse_exif(meta.exif);
    REQUIRE(exif.has_value());
    CHECK(exif->text.at("Software") == "Convertible test");
    CHECK(meta.metadata.at("ImageDescription") == "Test pattern");
    CHECK(meta.xmp.find("<dc:title>Test pattern</dc:title>") != std::string::npos);
    CHECK(test::pixels_equal(meta, png("src_rgb.png")));
  }
}

TEST_CASE("image_modern/jxl: lossless writing round trips exactly") {
  const ir::Image rgb = png("src_rgb.png");
  const ir::Image rgba = png("src_rgba.png");
  const ir::Image grey = png("src_grey.png");
  for (const ir::Image* source : {&rgb, &rgba, &grey}) {
    const auto encoded = test::write_from("jxl", *source, R"({"lossless":true})");
    REQUIRE_OK(encoded);
    CHECK(test::pixels_equal(decode(*encoded), *source));
  }
  // Greyscale content is stored as one channel.
  const auto grey_jxl = test::write_from("jxl", grey, R"({"lossless":true})");
  REQUIRE_OK(grey_jxl);
  CHECK(decode(*grey_jxl).source_grayscale);

  // 16-bit: the hdr samples survive bit-exactly.
  const ir::Image wide = png("src_rgb16.png");
  const auto wide_jxl = test::write_from("jxl", wide, R"({"lossless":true})");
  REQUIRE_OK(wide_jxl);
  const ir::Image wide_back = decode(*wide_jxl);
  REQUIRE(wide_back.hdr.has_value());
  CHECK(wide_back.source_bit_depth == 16);
  CHECK(image::srgb16_from_linear(*wide_back.hdr) == image::srgb16_from_linear(*wide.hdr));

  // Float: linear values above 1 are kept exactly.
  const ir::Image fl = load("pfm", "src_float.pfm");
  REQUIRE(fl.hdr.has_value());
  const auto fl_jxl = test::write_from("jxl", fl, R"({"lossless":true})");
  REQUIRE_OK(fl_jxl);
  const ir::Image fl_back = decode(*fl_jxl);
  REQUIRE(fl_back.hdr.has_value());
  CHECK(fl_back.source_bit_depth == 32);
  CHECK(fl_back.hdr->rgba == fl.hdr->rgba);

  // Animation: frames, delays and loop count.
  ir::Image anim = png("src_anim.png");
  anim.loop_count = 3;
  const auto anim_jxl = test::write_from("jxl", anim, R"({"lossless":true})");
  REQUIRE_OK(anim_jxl);
  const ir::Image anim_back = decode(*anim_jxl);
  REQUIRE(anim_back.frames.size() == 3);
  CHECK(test::pixels_equal(anim_back, anim));
  CHECK(anim_back.frames[1].delay_ms == 200);
  CHECK(anim_back.loop_count == 3);
}

TEST_CASE("image_modern/jxl: lossy quality, size and effort") {
  const ir::Image source = single(photo(160, 120));
  const auto q90 = test::write_from("jxl", source);
  const auto q50 = test::write_from("jxl", source, R"({"quality":50})");
  const auto q20 = test::write_from("jxl", source, R"({"quality":20})");
  const auto small = test::write_from("jxl", source, R"({"preset":"small"})");
  REQUIRE_OK(q90);
  REQUIRE_OK(q50);
  REQUIRE_OK(q20);
  REQUIRE_OK(small);
  CHECK(q90->size() > q50->size());
  CHECK(q50->size() > q20->size());
  CHECK(q90->size() > small->size());
  const double p90 = test::psnr(decode(*q90), source);
  const double p50 = test::psnr(decode(*q50), source);
  const double p20 = test::psnr(decode(*q20), source);
  CHECK(p90 > 36.0);
  CHECK(p50 > 31.0);
  CHECK(p20 > 27.0);
  CHECK(p90 > p50);
  CHECK(test::ssim(decode(*q90), source) > 0.95);

  const auto fast = test::write_from("jxl", source, R"({"effort":1})");
  const auto slow = test::write_from("jxl", source, R"({"effort":9})");
  REQUIRE_OK(fast);
  REQUIRE_OK(slow);
  CHECK(*fast != *slow);
  CHECK(test::psnr(decode(*fast), source) > 33.0);
  CHECK(test::psnr(decode(*slow), source) > 35.0);
  // Out-of-range efforts are clamped by the option system, never passed on.
  const auto clamped = test::write_from("jxl", source, R"({"effort":12})");
  REQUIRE_OK(clamped);
  CHECK(test::psnr(decode(*clamped), source) > 35.0);

  // Lossy alpha.
  const ir::Image rgba = png("src_rgba.png");
  const auto rgba_jxl = test::write_from("jxl", rgba);
  REQUIRE_OK(rgba_jxl);
  CHECK(test::psnr(decode(*rgba_jxl), rgba) > 33.0);

  // Size options.
  const auto half = test::write_from("jxl", source, R"({"scale":0.5})");
  REQUIRE_OK(half);
  CHECK(decode(*half).width() == 80);
  const auto boxed = test::write_from("jxl", source, R"({"max_width":40})");
  REQUIRE_OK(boxed);
  CHECK(decode(*boxed).height() == 30);
}

TEST_CASE("image_modern/jxl: metadata is written only when asked") {
  const ir::Image meta = png("src_exif.png");
  REQUIRE_FALSE(meta.exif.empty());
  const auto stripped = test::write_from("jxl", meta, R"({"lossless":true})");
  REQUIRE_OK(stripped);
  const ir::Image plain = decode(*stripped);
  CHECK(plain.exif.empty());
  CHECK(plain.xmp.empty());

  const auto kept = test::write_from("jxl", meta, R"({"lossless":true,"strip_metadata":false})");
  REQUIRE_OK(kept);
  const ir::Image back = decode(*kept);
  REQUIRE_FALSE(back.exif.empty());
  CHECK(image::parse_exif(back.exif)->text.at("Software") == "Convertible test");
  CHECK(back.xmp == meta.xmp);
  CHECK(test::pixels_equal(back, meta));

  const ir::Image icc = png("src_icc.png");
  const auto icc_kept = test::write_from("jxl", icc, R"({"lossless":true,"strip_metadata":false})");
  REQUIRE_OK(icc_kept);
  const ir::Image icc_back = decode(*icc_kept);
  CHECK(icc_back.icc_profile == icc.icc_profile);
  CHECK(test::pixels_equal(icc_back, icc));
}

TEST_CASE("image_modern/jxl: detection, engine routes and the compress path") {
  for (const char* name : {"lossless_rgb.jxl", "exif_boxes.jxl"}) {
    INFO(name);
    const Bytes data = jxl_fixture(name);
    const auto detected = detect_format(Registry::instance(), data, "");
    REQUIRE(detected.has_value());
    CHECK(detected->format->id == "jxl");
  }
  const auto* info = Registry::instance().find("jxl");
  REQUIRE(info != nullptr);
  CHECK(info->extensions.front() == "jxl");
  CHECK(info->mime == "image/jxl");

  // jxl -> jxl recompresses; png -> jxl -> png through the engine.
  const test::Converted again = test::convert(jxl_fixture("lossless_rgb.jxl"), "", "jxl", "", "picture.jxl");
  REQUIRE(again.ok());
  CHECK(again.format_id == "jxl");
  CHECK(test::psnr(decode(again.data), png("src_rgb.png")) > 29.0);
  const test::Converted small = test::convert(jxl_fixture("lossless_rgb.jxl"), "", "jxl", R"({"preset":"small"})");
  REQUIRE(small.ok());
  CHECK(small.data.size() < jxl_fixture("lossless_rgb.jxl").size());
  const test::Converted to_png = test::convert(jxl_fixture("lossless_rgba.jxl"), "", "png");
  REQUIRE(to_png.ok());
  const auto back = test::read_as<ir::Image>("png", to_png.data);
  REQUIRE_OK(back);
  CHECK(test::pixels_equal(*back, png("src_rgba.png")));
  const test::Converted from_png = test::convert(jxl_fixture("src_rgb.png"), "", "jxl", R"({"lossless":true})");
  REQUIRE(from_png.ok());
  CHECK(test::pixels_equal(decode(from_png.data), png("src_rgb.png")));
}

TEST_CASE("image_modern/jxl: limits, cancellation and hostile input") {
  const char* names[] = {"lossless_rgb.jxl", "lossy_d1.jxl", "anim.jxl", "exif_boxes.jxl", "lossless_rgb16.jxl",
                         "icc_lossless.jxl", "jpeg_recompressed.jxl", "lossy_modular_d2.jxl"};
  for (const char* name : names) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Image>("jxl", jxl_fixture(name), 256) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Image>("jxl", test::bytes("not a jxl")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Image>("jxl", ByteView(jxl_fixture("lossless_rgb.jxl")).first(40)), Status::Decode);

  // The pixel limit is checked from the basic info, before any pixel decoding.
  Limits limits;
  limits.max_pixels = 1000;
  CHECK_STATUS(test::read_as<ir::Image>("jxl", jxl_fixture("lossless_rgb.jxl"), {}, nullptr, limits), Status::Limit);
  // Frames count against the limit too (3 frames of 32x32 = 3072 pixels).
  limits.max_pixels = 2500;
  CHECK_STATUS(test::read_as<ir::Image>("jxl", jxl_fixture("anim.jxl"), {}, nullptr, limits), Status::Limit);

  // A hand-made codestream declaring 2^30 x 2^30 pixels (SizeHeader with
  // 30-bit dimensions) is refused before anything is allocated.
  const Bytes huge = {std::byte{0xFF}, std::byte{0x0A}, std::byte{0xFE}, std::byte{0xFF}, std::byte{0xFF},
                      std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F},
                      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
  const auto refused = test::read_as<ir::Image>("jxl", huge);
  REQUIRE_FALSE(refused.has_value());
  CHECK((refused.error().code == Status::Limit || refused.error().code == Status::Decode));

  // Cancellation stops the reader and the writer.
  const Codec* reader = test::find_reader("jxl", Kind::Image);
  REQUIRE(reader != nullptr);
  ReadContext rctx;
  const Bytes input = jxl_fixture("anim.jxl");
  rctx.input = input;
  rctx.registry = &Registry::instance();
  rctx.progress = Progress([](double, std::string_view) { return true; });
  CHECK_STATUS(reader->read(rctx), Status::Cancelled);
  const Codec* writer = test::find_writer("jxl", Kind::Image);
  REQUIRE(writer != nullptr);
  WriteContext wctx;
  auto options = Options().normalized(writer->write_options);
  REQUIRE_OK(options);
  wctx.options = *options;
  wctx.progress = Progress([](double, std::string_view) { return true; });
  CHECK_STATUS(writer->write(single(photo(32, 32)), wctx), Status::Cancelled);
}
