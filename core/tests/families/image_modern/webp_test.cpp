// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "families/image/common.hpp"
#include "families/image/image_test_util.hpp"

using namespace cvt;
using test::image_util::photo_frame;
using test::image_util::single;

namespace {

std::string path(std::string_view name) { return std::string("image_modern/webp/") + std::string(name); }

ir::Image webp(std::string_view name) { return test::image_util::load("webp", path(name)); }

// Independent parser for the 8-bit RGB_ALPHA PAM files written by dwebp,
// anim_dump and ImageMagick (and the pattern generator).
ir::Frame parse_pam(const Bytes& data) {
  const std::string_view text = as_chars(data);
  const std::size_t end = text.find("ENDHDR\n");
  if (text.substr(0, 3) != "P7\n" || end == std::string_view::npos) return {};
  const auto field = [&](std::string_view key) -> std::uint32_t {
    const std::size_t at = text.find(key);
    if (at == std::string_view::npos || at > end) return 0;
    std::uint32_t value = 0;
    for (std::size_t i = at + key.size() + 1; i < end && text[i] >= '0' && text[i] <= '9'; ++i) {
      value = value * 10 + static_cast<std::uint32_t>(text[i] - '0');
    }
    return value;
  };
  const std::uint32_t width = field("WIDTH");
  const std::uint32_t height = field("HEIGHT");
  const std::size_t start = end + 7;
  if (width == 0 || height == 0 || field("DEPTH") != 4 || field("MAXVAL") != 255 ||
      text.size() < start + std::size_t{width} * height * 4) {
    return {};
  }
  ir::Frame frame = ir::Frame::filled(width, height);
  std::memcpy(frame.rgba.data(), text.data() + start, frame.rgba.size());
  return frame;
}

ir::Frame pam(std::string_view name) { return parse_pam(test::fixture(path(name))); }
ir::Frame ppm(std::string_view name) { return test::image_util::parse_ppm(test::fixture(path(name))); }

ir::Image decode(const Bytes& data) {
  auto image = test::read_as<ir::Image>("webp", data);
  if (!image) {
    FAIL_CHECK("decoding our own output: ", image.error().message);
    return {};
  }
  return std::move(*image);
}

Bytes encode(const ir::Image& image, std::string_view options = {}) {
  auto data = test::write_from("webp", image, options);
  if (!data) {
    FAIL_CHECK("encoding: ", data.error().message);
    return {};
  }
  return std::move(*data);
}

// A picture with fully transparent, half transparent and opaque areas whose
// hidden colours are not black.
ir::Frame alpha_frame(std::uint32_t width, std::uint32_t height) {
  ir::Frame frame = photo_frame(width, height, 5);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t& a = frame.rgba[frame.offset(x, y) + 3];
      a = x < width / 4 ? 0 : static_cast<std::uint8_t>(std::min<std::uint32_t>(255, x * 4 + y));
    }
  }
  return frame;
}

// True when every pixel with alpha > 0 matches exactly (fully transparent
// pixels may carry any colour).
bool visible_pixels_equal(const ir::Frame& a, const ir::Frame& b) {
  if (a.width != b.width || a.height != b.height) return false;
  for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
    if (a.rgba[i + 3] != b.rgba[i + 3]) return false;
    if (a.rgba[i + 3] != 0 && std::memcmp(&a.rgba[i], &b.rgba[i], 3) != 0) return false;
  }
  return true;
}

ir::Image three_frame_animation() {
  ir::Image image;
  for (std::uint32_t i = 0; i < 3; ++i) {
    ir::Frame frame = photo_frame(48, 32, 20 + i);
    for (std::uint32_t y = 4; y < 14; ++y) {
      for (std::uint32_t x = 4 + i * 12; x < 14 + i * 12; ++x) {
        std::uint8_t* p = frame.rgba.data() + frame.offset(x, y);
        p[0] = 250;
        p[1] = 220;
        p[2] = 10;
        p[3] = 255;
      }
    }
    if (i == 2) {
      for (std::uint32_t x = 0; x < 48; ++x) frame.rgba[frame.offset(x, 31) + 3] = 0;  // transparent last row
    }
    frame.delay_ms = 50 + 20 * i;
    image.frames.push_back(std::move(frame));
  }
  image.loop_count = 4;
  return image;
}

}  // namespace

TEST_CASE("image_modern/webp: lossless files from cwebp and ImageMagick decode exactly") {
  const ir::Image rgb = webp("lossless.webp");
  REQUIRE(rgb.frames.size() == 1);
  CHECK(test::pixels_equal(rgb.frames[0], ppm("src_rgb.ppm")));

  const ir::Frame rgba = pam("src_rgba.pam");
  REQUIRE(rgba.valid());
  for (const char* name : {"lossless_alpha.webp", "magick_lossless_alpha.webp"}) {
    INFO(name);
    const ir::Image image = webp(name);
    REQUIRE(image.frames.size() == 1);
    CHECK(test::pixels_equal(image.frames[0], rgba));  // written with "exact": hidden colours kept too
  }
}

TEST_CASE("image_modern/webp: lossy files match dwebp") {
  struct Case {
    const char* file;
    const char* reference;
    bool alpha;
  };
  for (const Case& c : {Case{"lossy.webp", "lossy_dwebp.ppm", false}, Case{"magick_lossy.webp", "magick_lossy_dwebp.ppm", false},
                        Case{"lossy_alpha.webp", "lossy_alpha_dwebp.pam", true}}) {
    INFO(c.file);
    const ir::Image image = webp(c.file);
    REQUIRE(image.frames.size() == 1);
    const ir::Frame reference = c.alpha ? pam(c.reference) : ppm(c.reference);
    REQUIRE(reference.valid());
    CHECK(test::max_abs_diff(image.frames[0], reference) <= 1.0);
  }
  // And they are faithful to the pattern they were made from (cwebp -q 80).
  CHECK(test::psnr(webp("lossy.webp").frames[0], ppm("src_rgb.ppm")) >= 27.0);
  const ir::Image alpha = webp("lossy_alpha.webp");
  CHECK(alpha.frames[0].has_transparency());
}

TEST_CASE("image_modern/webp: animations are composited like anim_dump, with delays and loop count") {
  const ir::Image mux = webp("anim_mux.webp");
  REQUIRE(mux.frames.size() == 4);
  CHECK(mux.loop_count == 3);
  CHECK(mux.width() == 32);
  CHECK(mux.height() == 24);
  const std::uint32_t mux_delays[] = {100, 200, 300, 400};
  for (std::size_t i = 0; i < 4; ++i) {
    INFO("anim_mux frame ", i);
    CHECK(mux.frames[i].delay_ms == mux_delays[i]);
    CHECK(test::pixels_equal(mux.frames[i], pam(std::format("anim_mux_{:04}.pam", i))));
  }
  // Frame 2 was disposed to the background, so frame 3 shows transparency
  // where it was, and the half-transparent yellow is blended over the red.
  CHECK(mux.frames[2].rgba[mux.frames[2].offset(9, 5) + 3] == 0);

  const ir::Image lossy = webp("anim_lossy.webp");
  REQUIRE(lossy.frames.size() == 3);
  CHECK(lossy.loop_count == 0);
  const std::uint32_t lossy_delays[] = {80, 120, 40};
  for (std::size_t i = 0; i < 3; ++i) {
    INFO("anim_lossy frame ", i);
    CHECK(lossy.frames[i].delay_ms == lossy_delays[i]);
    CHECK(test::max_abs_diff(lossy.frames[i], pam(std::format("anim_lossy_{:04}.pam", i))) <= 1.0);
  }

  const ir::Image gif = webp("anim_gif2webp.webp");
  REQUIRE(gif.frames.size() == 3);
  CHECK(gif.loop_count == 2);
  const image::Rgba colours[] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
  for (std::size_t i = 0; i < 3; ++i) {
    INFO("anim_gif2webp frame ", i);
    CHECK(gif.frames[i].delay_ms == 100 * (i + 1));
    CHECK(test::pixels_equal(gif.frames[i], ir::Frame::filled(24, 16, colours[i])));
  }
}

TEST_CASE("image_modern/webp: ICC, XMP and EXIF chunks are read and EXIF orientation applied") {
  const ir::Image image = webp("metadata.webp");
  REQUIRE(image.frames.size() == 1);
  CHECK(image.width() == 8);  // 16x8 stored, orientation 6 (rotate 90 degrees clockwise)
  CHECK(image.height() == 16);
  CHECK(test::pixels_equal(image.frames[0], pam("metadata_upright.pam")));
  REQUIRE(image.icc_profile.size() == 132);
  CHECK(test::str(ByteView(image.icc_profile).subspan(36, 4)) == "acsp");
  CHECK(image.xmp.find("<dc:title>pattern</dc:title>") != std::string::npos);
  REQUIRE(!image.exif.empty());
  const auto exif = image::parse_exif(image.exif);
  REQUIRE(exif.has_value());
  CHECK(exif->orientation == 1);  // reset after rotating the pixels
  CHECK(image.metadata.at("Software") == "pattern");
}

TEST_CASE("image_modern/webp: lossy quality floors and monotonic size") {
  // Floors per quality for smooth content and for grainy content. WebP lossy
  // is always 4:2:0, so independent per-channel grain caps PSNR near 34 dB
  // even at quality 100.
  struct Floor {
    int quality;
    double smooth_psnr;
    double smooth_ssim;
    double grain_psnr;
    double grain_ssim;
  };
  const ir::Image smooth = single(test::gradient_frame(128, 96));
  const ir::Image source = single(photo_frame(128, 96));
  std::size_t previous = 0;
  std::size_t previous_smooth = 0;
  for (const Floor& floor : {Floor{30, 35.0, 0.97, 30.0, 0.75}, Floor{60, 37.0, 0.975, 30.5, 0.78},
                             Floor{90, 43.0, 0.99, 32.5, 0.94}}) {
    INFO("quality ", floor.quality);
    const std::string options = std::format(R"({{"quality":{}}})", floor.quality);
    const Bytes smooth_data = encode(smooth, options);
    const ir::Image smooth_decoded = decode(smooth_data);
    CHECK(test::psnr(smooth, smooth_decoded) >= floor.smooth_psnr);
    CHECK(test::ssim(smooth, smooth_decoded) >= floor.smooth_ssim);
    const Bytes data = encode(source, options);
    const ir::Image decoded = decode(data);
    REQUIRE(decoded.frames.size() == 1);
    CHECK(test::psnr(source, decoded) >= floor.grain_psnr);
    CHECK(test::ssim(source, decoded) >= floor.grain_ssim);
    CHECK(data.size() > previous);
    CHECK(smooth_data.size() > previous_smooth);
    previous = data.size();
    previous_smooth = smooth_data.size();
  }
  // Presets map onto qualities: small < balanced < high.
  const std::size_t small = encode(source, R"({"preset":"small"})").size();
  const std::size_t balanced = encode(source, R"({"preset":"balanced"})").size();
  const std::size_t high = encode(source).size();
  CHECK(small < balanced);
  CHECK(balanced < high);
}

TEST_CASE("image_modern/webp: lossless round trips are exact, including alpha") {
  const ir::Image opaque = single(photo_frame(64, 48));
  CHECK(test::pixels_equal(decode(encode(opaque, R"({"lossless":true})")), opaque));

  const ir::Image alpha = single(alpha_frame(64, 48));
  // Hidden colours under alpha 0 are simplified unless "exact" is set.
  const ir::Image plain = decode(encode(alpha, R"({"lossless":true})"));
  REQUIRE(plain.frames.size() == 1);
  CHECK(visible_pixels_equal(plain.frames[0], alpha.frames[0]));
  CHECK(test::pixels_equal(decode(encode(alpha, R"({"lossless":true,"exact":true})")), alpha));

  // Lossy output keeps alpha losslessly by default (alpha_quality 100).
  const ir::Image lossy = decode(encode(alpha));
  REQUIRE(lossy.frames.size() == 1);
  bool alpha_same = true;
  for (std::size_t i = 3; i < alpha.frames[0].rgba.size(); i += 4) {
    alpha_same = alpha_same && lossy.frames[0].rgba[i] == alpha.frames[0].rgba[i];
  }
  CHECK(alpha_same);
}

TEST_CASE("image_modern/webp: effort, near-lossless, alpha quality and sharp YUV options") {
  const ir::Image source = single(photo_frame(96, 64));
  // Lossless effort: compression level 9 beats level 0; method overrides.
  const Bytes fast = encode(source, R"({"lossless":true,"compression_level":0})");
  const Bytes best = encode(source, R"({"lossless":true,"compression_level":9})");
  CHECK(best.size() < fast.size());
  const Bytes method0 = encode(source, R"({"lossless":true,"compression_level":9,"method":0})");
  CHECK(best.size() <= method0.size());
  for (const Bytes* data : {&fast, &best, &method0}) CHECK(test::pixels_equal(decode(*data), source));

  // Lossy effort: method 6 is not larger than method 0 at the same quality.
  const Bytes lossy0 = encode(source, R"({"method":0})");
  const Bytes lossy6 = encode(source, R"({"method":6})");
  CHECK(lossy6.size() <= lossy0.size());
  CHECK(test::psnr(decode(lossy0), source) >= 32.0);

  // Near-lossless: smaller than lossless, still very close.
  const Bytes near = encode(source, R"({"near_lossless":40})");
  const ir::Image near_decoded = decode(near);
  CHECK(near.size() < best.size());
  CHECK(test::psnr(near_decoded, source) >= 38.0);
  CHECK(test::max_abs_diff(near_decoded, source) <= 16.0);

  // Alpha quality: a lower value gives a smaller file for a textured alpha
  // channel, and changes alpha only moderately.
  ir::Frame textured = photo_frame(96, 64, 9);
  for (std::size_t i = 0; i < textured.rgba.size(); i += 4) textured.rgba[i + 3] = textured.rgba[i + 1];
  const ir::Image alpha = single(textured);
  const Bytes coarse = encode(alpha, R"({"alpha_quality":20})");
  CHECK(coarse.size() < encode(alpha).size());
  CHECK(test::psnr(decode(coarse), alpha) >= 25.0);

  // Sharp YUV: a valid, faithful file.
  const Bytes sharp = encode(source, R"({"sharp_yuv":true})");
  CHECK(test::psnr(decode(sharp), source) >= 32.0);

  CHECK_STATUS(test::write_from("webp", source, R"({"method":"fast"})"), Status::InvalidArgument);
}

TEST_CASE("image_modern/webp: animations round trip with delays and loop count") {
  const ir::Image source = three_frame_animation();
  const ir::Image lossless = decode(encode(source, R"({"lossless":true,"exact":true})"));
  REQUIRE(lossless.frames.size() == 3);
  CHECK(lossless.loop_count == 4);
  for (std::size_t i = 0; i < 3; ++i) {
    INFO("frame ", i);
    CHECK(lossless.frames[i].delay_ms == source.frames[i].delay_ms);
    CHECK(test::pixels_equal(lossless.frames[i], source.frames[i]));
  }
  // Lossy frames are as faithful as the same frame encoded as a still
  // (the animation encoder may code changed rectangles only).
  const ir::Image lossy = decode(encode(source));
  REQUIRE(lossy.frames.size() == 3);
  for (std::size_t i = 0; i < 3; ++i) {
    INFO("frame ", i);
    CHECK(lossy.frames[i].delay_ms == source.frames[i].delay_ms);
    const ir::Image still = decode(encode(single(source.frames[i])));
    CHECK(test::psnr(lossy.frames[i], source.frames[i]) >= test::psnr(still.frames[0], source.frames[i]) - 1.0);
    CHECK(test::psnr(lossy.frames[i], source.frames[i]) >= 26.0);
  }
  CHECK(lossy.frames[2].rgba[lossy.frames[2].offset(10, 31) + 3] == 0);

  // A fixture animation re-encodes to the same frames.
  const ir::Image mux = webp("anim_mux.webp");
  const ir::Image again = decode(encode(mux, R"({"lossless":true,"exact":true})"));
  CHECK(test::pixels_equal(again, mux));
  CHECK(again.loop_count == 3);
}

TEST_CASE("image_modern/webp: metadata is stripped by default and kept on request") {
  ir::Image image = single(photo_frame(32, 24));
  image.icc_profile = test::fixture(path("metadata.webp"));  // any bytes: stored verbatim
  image.icc_profile.resize(40);
  image.xmp = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>";
  image.exif = webp("metadata.webp").exif;
  REQUIRE(!image.exif.empty());

  const ir::Image stripped = decode(encode(image));
  CHECK(stripped.icc_profile.empty());
  CHECK(stripped.xmp.empty());
  CHECK(stripped.exif.empty());

  const ir::Image kept = decode(encode(image, R"({"strip_metadata":false})"));
  CHECK(kept.icc_profile == image.icc_profile);
  CHECK(kept.xmp == image.xmp);
  CHECK(kept.exif == image.exif);
  CHECK(kept.width() == 32);  // orientation 1: nothing rotated

  ir::Image animation = three_frame_animation();
  animation.xmp = image.xmp;
  const ir::Image kept_animation = decode(encode(animation, R"({"strip_metadata":false,"lossless":true})"));
  CHECK(kept_animation.frames.size() == 3);
  CHECK(kept_animation.xmp == image.xmp);
}

TEST_CASE("image_modern/webp: size options and dimension limit on write") {
  const ir::Image source = single(photo_frame(64, 48));
  const ir::Image half = decode(encode(source, R"({"max_width":32})"));
  CHECK(half.width() == 32);
  CHECK(half.height() == 24);
  CHECK(decode(encode(source, R"({"scale":0.25})")).width() == 16);
  CHECK_STATUS(test::write_from("webp", single(ir::Frame::filled(16384, 1, {1, 2, 3, 255}))), Status::Encode);
  CHECK(decode(encode(single(ir::Frame::filled(16384, 1, {1, 2, 3, 255})), R"({"max_width":16383})")).width() ==
        16383);
}

TEST_CASE("image_modern/webp: damaged and hostile files are errors, never crashes") {
  for (const char* name : {"lossless.webp", "lossy.webp", "lossy_alpha.webp", "lossless_alpha.webp", "anim_mux.webp",
                           "anim_lossy.webp", "anim_gif2webp.webp", "metadata.webp"}) {
    INFO(name);
    const Bytes data = test::fixture(path(name));
    CHECK(test::fuzz_reader<ir::Image>("webp", data) > 0);
    CHECK_STATUS(test::read_as<ir::Image>("webp", ByteView(data).first(20)), Status::Decode);
    if (std::string_view(name) != "metadata.webp") {
      CHECK_STATUS(test::read_as<ir::Image>("webp", ByteView(data).first(data.size() - 3)), Status::Decode);
    }
  }
  // A cut in the metadata after a complete picture keeps the picture.
  const Bytes meta = test::fixture(path("metadata.webp"));
  std::vector<std::string> warnings;
  const auto cut = test::read_as<ir::Image>("webp", ByteView(meta).first(meta.size() - 3), {}, &warnings);
  REQUIRE_OK(cut);
  CHECK(cut->width() == 8);  // the complete EXIF chunk is still applied
  CHECK(cut->icc_profile.size() == 132);
  CHECK(cut->xmp.empty());  // the cut one is not
  CHECK(warnings.size() == 1);
  CHECK_STATUS(test::read_as<ir::Image>("webp", test::bytes("RIFF\x04\0\0\0WEBP")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Image>("webp", test::bytes("not a webp file at all")), Status::Decode);

  // Limits are checked against the declared size before any pixel is
  // allocated: the still and every frame of an animation count.
  Limits limits;
  limits.max_pixels = 1000;
  CHECK_STATUS(test::read_as<ir::Image>("webp", test::fixture(path("lossless.webp")), {}, nullptr, limits),
               Status::Limit);
  limits.max_pixels = 32 * 24 * 3;  // three canvases fit, the four frames do not
  CHECK_STATUS(test::read_as<ir::Image>("webp", test::fixture(path("anim_mux.webp")), {}, nullptr, limits),
               Status::Limit);

  // An animation declaring a 16384 x 16384 canvas: each frame alone is
  // within the default pixel limit, all four are not.
  Bytes huge = test::fixture(path("anim_mux.webp"));
  REQUIRE(test::str(ByteView(huge).subspan(12, 4)) == "VP8X");
  const std::uint8_t canvas[6] = {0xFF, 0x3F, 0x00, 0xFF, 0x3F, 0x00};
  for (std::size_t i = 0; i < 6; ++i) huge[24 + i] = std::byte{canvas[i]};
  CHECK_STATUS(test::read_as<ir::Image>("webp", huge), Status::Limit);
}

TEST_CASE("image_modern/webp: cancellation stops decoding and encoding") {
  const Codec* reader = test::find_reader("webp", Kind::Image);
  REQUIRE(reader != nullptr);
  const Bytes data = test::fixture(path("anim_mux.webp"));
  ReadContext read;
  read.input = data;
  read.registry = &Registry::instance();
  read.progress = Progress([](double, std::string_view) { return true; });
  CHECK_STATUS(reader->read(read), Status::Cancelled);

  const Codec* writer = test::find_writer("webp", Kind::Image);
  REQUIRE(writer != nullptr);
  WriteContext write;
  write.registry = &Registry::instance();
  write.progress = Progress([](double fraction, std::string_view) { return fraction > 0.3; });
  auto options = Options().normalized(writer->write_options);
  REQUIRE_OK(options);
  write.options = *options;
  const AnyIR value(single(photo_frame(128, 96)));
  CHECK_STATUS(writer->write(value, write), Status::Cancelled);
}

TEST_CASE("image_modern/webp: detection, engine routes and the compress path") {
  const Bytes lossless = test::fixture(path("photo_lossless.webp"));
  const test::Converted detected = test::convert(lossless, "", "png", "", "picture.bin");
  REQUIRE(detected.ok());
  CHECK(test::convert(test::fixture(path("anim_mux.webp")), "", "gif").ok());

  // Same-format compress: a lossless photo becomes a smaller, faithful lossy file.
  const test::Converted compressed = test::convert(lossless, "", "webp", "", "photo.webp");
  REQUIRE(compressed.ok());
  CHECK(compressed.data.size() < lossless.size());
  const ir::Image original = decode(lossless);
  CHECK(test::psnr(original, decode(compressed.data)) >= 38.0);
  // ... and keeps losslessness on request.
  const test::Converted kept = test::convert(lossless, "webp", "webp", R"({"lossless":true})");
  REQUIRE(kept.ok());
  CHECK(test::pixels_equal(decode(kept.data), original));

  // PNG -> WebP through the engine.
  const test::Converted to_png = test::convert(test::fixture(path("lossless.webp")), "webp", "png");
  REQUIRE(to_png.ok());
  const test::Converted back = test::convert(to_png.data, "png", "webp", R"({"lossless":true})");
  REQUIRE(back.ok());
  CHECK(back.extension == "webp");
  CHECK(back.mime == "image/webp");
  CHECK(test::pixels_equal(decode(back.data).frames.at(0), ppm("src_rgb.ppm")));
}
