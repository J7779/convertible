// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <cstdint>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "codec/ids.hpp"
#include "engine/registry.hpp"
#include "families/image/image_ops.hpp"
#include "support/fixtures.hpp"
#include "util/crc.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

ir::Image png(std::string_view name) {
  auto image = test::read_as<ir::Image>("png", test::fixture(std::string("image/png/") + std::string(name)));
  if (!image) {
    FAIL_CHECK("decoding ", name, ": ", image.error().message);
    return {};
  }
  return std::move(*image);
}

std::uint8_t ramp(std::uint32_t position, std::uint32_t count) {
  return static_cast<std::uint8_t>(position * 255u / (count - 1));
}

// The 16-bit sample formula of make_fixtures.py.
std::uint16_t wide_sample(std::uint32_t x, std::uint32_t y, std::uint32_t c, std::uint32_t channels) {
  if (c == channels - 1 && (channels == 2 || channels == 4)) {
    return (x + y) % 3 ? 65535 : static_cast<std::uint16_t>((x * 4096) % 65536);
  }
  return static_cast<std::uint16_t>((x * 2039 + y * 977 + c * 12345) % 65536);
}

ir::Image single(ir::Frame frame) {
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

// A photo-like picture: smooth gradient plus fine grain (hard for lossless filters).
ir::Frame photo_frame(std::uint32_t width, std::uint32_t height) {
  ir::Frame frame = test::gradient_frame(width, height);
  const Bytes noise = test::random_bytes(frame.rgba.size(), 11);
  for (std::size_t i = 0; i < frame.rgba.size(); ++i) {
    if (i % 4 == 3) continue;
    const int v = frame.rgba[i] + (std::to_integer<int>(noise[i]) % 25) - 12;
    frame.rgba[i] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
  }
  return frame;
}

std::size_t png_color_type(const Bytes& png) { return std::to_integer<std::size_t>(png[25]); }
std::size_t png_bit_depth(const Bytes& png) { return std::to_integer<std::size_t>(png[24]); }

}  // namespace

TEST_CASE("image/png: decodes 8-bit RGB, RGBA, interlaced and ImageIO files exactly") {
  const ir::Frame gradient = test::gradient_frame(32, 24);
  const ir::Frame alpha = test::alpha_gradient_frame(32, 24);
  for (const char* name : {"rgb8.png", "rgb8_interlaced.png", "sips_rgb8.png"}) {
    INFO(name);
    const ir::Image image = png(name);
    REQUIRE(image.frames.size() == 1);
    CHECK(test::pixels_equal(image.frames[0], gradient));
  }
  for (const char* name : {"rgba8.png", "rgba8_interlaced.png"}) {
    INFO(name);
    const ir::Image image = png(name);
    REQUIRE(image.frames.size() == 1);
    CHECK(test::pixels_equal(image.frames[0], alpha));
  }
  // Grey + alpha (Pillow converts with L = 0.299 R + 0.587 G + 0.114 B).
  const ir::Image la = png("greyalpha8.png");
  REQUIRE(la.valid());
  CHECK(image::is_grayscale(la.frames[0]));
  for (std::uint32_t i = 0; i < 32 * 24; ++i) CHECK(la.frames[0].rgba[i * 4 + 3] == alpha.rgba[i * 4 + 3]);
  CHECK(la.source_grayscale);
}

TEST_CASE("image/png: decodes grey at 1, 2, 4 and 8 bits") {
  for (const int depth : {1, 2, 4, 8}) {
    INFO("depth ", depth);
    const ir::Image image = png("grey" + std::to_string(depth) + ".png");
    REQUIRE(image.valid());
    CHECK(image.source_bit_depth == depth);
    const int levels = (1 << depth) - 1;
    bool all = true;
    for (std::uint32_t y = 0; y < 24; ++y) {
      for (std::uint32_t x = 0; x < 32; ++x) {
        const auto expected = static_cast<std::uint8_t>((ramp(x, 32) >> (8 - depth)) * 255 / levels);
        const std::uint8_t* p = image.frames[0].rgba.data() + image.frames[0].offset(x, y);
        all = all && p[0] == expected && p[1] == expected && p[2] == expected && p[3] == 255;
      }
    }
    CHECK(all);
  }
}

TEST_CASE("image/png: 16-bit files keep full precision in the HDR pixels") {
  struct Case {
    const char* name;
    std::uint32_t channels;
  };
  for (const Case c : {Case{"grey16.png", 1}, Case{"greyalpha16.png", 2}, Case{"rgb16.png", 3}, Case{"rgba16.png", 4},
                       Case{"rgba16_interlaced.png", 4}}) {
    INFO(c.name);
    const ir::Image image = png(c.name);
    REQUIRE(image.valid());
    REQUIRE(image.hdr.has_value());
    CHECK(image.source_bit_depth == 16);
    const std::vector<std::uint16_t> wide = image::srgb16_from_linear(*image.hdr);
    bool exact = true;
    bool eight = true;
    for (std::uint32_t y = 0; y < 12; ++y) {
      for (std::uint32_t x = 0; x < 16; ++x) {
        std::uint16_t expected[4];
        if (c.channels <= 2) {
          expected[0] = expected[1] = expected[2] = wide_sample(x, y, 0, c.channels);
          expected[3] = c.channels == 2 ? wide_sample(x, y, 1, 2) : 65535;
        } else {
          for (std::uint32_t k = 0; k < 3; ++k) expected[k] = wide_sample(x, y, k, c.channels);
          expected[3] = c.channels == 4 ? wide_sample(x, y, 3, 4) : 65535;
        }
        const std::size_t at = (static_cast<std::size_t>(y) * 16 + x) * 4;
        for (std::size_t k = 0; k < 4; ++k) {
          exact = exact && wide[at + k] == expected[k];
          eight = eight && image.frames[0].rgba[at + k] == (expected[k] * 255u + 32767u) / 65535u;
        }
      }
    }
    CHECK(exact);
    CHECK(eight);
  }
}

TEST_CASE("image/png: decodes palettes and transparency") {
  const ir::Image p8 = png("palette8.png");
  REQUIRE(p8.valid());
  bool ok = true;
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const std::uint32_t i = (x * 5 + y * 3) % 200;
      const std::uint8_t* p = p8.frames[0].rgba.data() + p8.frames[0].offset(x, y);
      ok = ok && p[0] == (i * 7) % 256 && p[1] == (i * 13) % 256 && p[2] == (i * 29) % 256 && p[3] == 255;
    }
  }
  CHECK(ok);
  for (const auto& [depth, count] : {std::pair{4, 16u}, std::pair{2, 4u}, std::pair{1, 2u}}) {
    INFO("palette depth ", depth);
    const ir::Image image = png("palette" + std::to_string(depth) + ".png");
    REQUIRE(image.valid());
    bool same = true;
    for (std::uint32_t y = 0; y < 24; ++y) {
      for (std::uint32_t x = 0; x < 32; ++x) {
        const std::uint32_t i = (x + y) % count;
        const std::uint8_t* p = image.frames[0].rgba.data() + image.frames[0].offset(x, y);
        same = same && p[0] == (i * 70) % 256 && p[1] == (i * 130) % 256 && p[2] == (255 - i * 50) % 256;
      }
    }
    CHECK(same);
  }
  const ir::Image pt = png("palette_trns.png");
  REQUIRE(pt.valid());
  const std::uint8_t alphas[8] = {0, 64, 128, 192, 255, 255, 255, 255};
  bool alpha_ok = true;
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      alpha_ok = alpha_ok && pt.frames[0].rgba[pt.frames[0].offset(x, y) + 3] == alphas[(x / 4 + y / 4) % 8];
    }
  }
  CHECK(alpha_ok);
  const ir::Image key_rgb = png("rgb_trns.png");
  REQUIRE(key_rgb.valid());
  CHECK(key_rgb.frames[0].rgba[3] == 0);
  CHECK(key_rgb.frames[0].rgba[7] == 255);
  const ir::Image key_grey = png("grey_trns.png");
  REQUIRE(key_grey.valid());
  CHECK(key_grey.frames[0].rgba[3] == 0);              // x = 0 -> value 0 -> transparent
  CHECK(key_grey.frames[0].rgba[4 * 1 + 3] == 255);    // x = 1 -> value 8
  CHECK(key_grey.frames[0].rgba[4 * 32 + 3] == 0);     // x = 32 wraps to 0 on the next row
}

TEST_CASE("image/png: gAMA is applied, metadata and EXIF orientation are honoured") {
  const ir::Image linear = png("gamma10.png");
  REQUIRE(linear.valid());
  for (std::uint32_t i = 0; i < 16; ++i) {
    CHECK(linear.frames[0].rgba[i * 4] == image::linear_to_srgb8(static_cast<float>(i * 17) / 255.0f));
  }
  const ir::Image meta = png("metadata.png");
  REQUIRE(meta.valid());
  // Stored 4x2 with a red marker top-left, orientation 6: upright it is 2x4 with the marker top-right.
  CHECK(meta.width() == 2);
  CHECK(meta.height() == 4);
  CHECK(meta.frames[0].rgba[meta.frames[0].offset(1, 0)] == 255);
  CHECK(meta.frames[0].rgba[meta.frames[0].offset(0, 0)] == 0);
  CHECK(meta.metadata.at("Title") == "Test pattern");
  CHECK(meta.metadata.at("Comment") == "compressed text");
  CHECK(meta.metadata.at("Description") == "caf\xC3\xA9 \xE2\x9C\x93");
  CHECK_APPROX(meta.dpi_x, 150.0, 0.1);
  REQUIRE_FALSE(meta.exif.empty());
}

TEST_CASE("image/png: decodes APNG animations with partial frames and blending") {
  const ir::Image anim = png("anim.png");
  REQUIRE(anim.frames.size() == 3);
  CHECK(anim.loop_count == 2);
  CHECK(anim.frames[0].delay_ms == 100);
  CHECK(anim.frames[1].delay_ms == 200);
  CHECK(anim.frames[2].delay_ms == 300);
  for (std::uint32_t n = 0; n < 3; ++n) {
    ir::Frame expected = ir::Frame::filled(32, 24, {0, 0, 0, 0});
    for (std::uint32_t y = 0; y < 24; ++y) {
      for (std::uint32_t x = 0; x < 8 * (n + 1); ++x) {
        std::uint8_t* p = expected.rgba.data() + expected.offset(x, y);
        p[0] = static_cast<std::uint8_t>(80 * n);
        p[1] = static_cast<std::uint8_t>(255 - 80 * n);
        p[2] = 40;
        p[3] = 255;
      }
    }
    INFO("frame ", n);
    CHECK(test::pixels_equal(anim.frames[n], expected));
  }
  // The same bytes are detected as APNG and read through that id as well.
  const Bytes bytes = test::fixture("image/png/anim.png");
  const test::Converted detected = test::convert(bytes, "", "apng");
  CHECK(detected.ok());
  CHECK(test::read_as<ir::Image>("apng", bytes)->frames.size() == 3);
}

TEST_CASE("image/png: lossless round trips choose compact storage") {
  struct Case {
    const char* what;
    ir::Frame frame;
    std::size_t color_type;
    std::size_t bit_depth;
  };
  ir::Frame grey_alpha = test::alpha_gradient_frame(40, 30);
  image::to_grayscale(grey_alpha);
  ir::Frame keyed = test::gradient_frame(30, 20);
  keyed.rgba[3] = 0;
  ir::Frame few = test::checker_frame(33, 17, 2);
  few.rgba[few.offset(1, 1)] = 200;  // three colours
  const Case cases[] = {
      {"gradient", test::gradient_frame(64, 48), 2, 8},
      {"alpha", test::alpha_gradient_frame(64, 48), 6, 8},
      {"checker", test::checker_frame(31, 9, 3), 0, 1},
      {"noise", test::noise_frame(40, 40, 7), 2, 8},
      {"grey alpha", grey_alpha, 4, 8},
      {"colour key", keyed, 2, 8},
      {"three colours", few, 3, 2},
  };
  for (const Case& c : cases) {
    INFO(c.what);
    const auto encoded = test::write_from("png", single(c.frame));
    REQUIRE_OK(encoded);
    CHECK(png_color_type(*encoded) == c.color_type);
    CHECK(png_bit_depth(*encoded) == c.bit_depth);
    const auto decoded = test::read_as<ir::Image>("png", *encoded);
    REQUIRE_OK(decoded);
    CHECK(test::pixels_equal(decoded->frames[0], c.frame));
    const auto interlaced = test::write_from("png", single(c.frame), R"({"interlace":true})");
    REQUIRE_OK(interlaced);
    CHECK(test::pixels_equal(test::read_as<ir::Image>("png", *interlaced)->frames[0], c.frame));
  }
}

TEST_CASE("image/png: 16-bit sources round trip exactly") {
  for (const char* name : {"rgba16.png", "grey16.png", "rgb16.png"}) {
    INFO(name);
    const ir::Image source = png(name);
    REQUIRE(source.hdr.has_value());
    const auto encoded = test::write_from("png", source);
    REQUIRE_OK(encoded);
    CHECK(png_bit_depth(*encoded) == 16);
    const auto decoded = test::read_as<ir::Image>("png", *encoded);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->hdr.has_value());
    CHECK(image::srgb16_from_linear(*decoded->hdr) == image::srgb16_from_linear(*source.hdr));
    const auto eight = test::write_from("png", source, R"({"bit_depth":"8"})");
    REQUIRE_OK(eight);
    CHECK(png_bit_depth(*eight) == 8);
  }
}

TEST_CASE("image/png: compression level and the lossy small path") {
  const ir::Image photo = single(photo_frame(160, 120));
  const ir::Image smooth = single(test::checker_frame(160, 120, 5));
  ir::Image text_like = single(test::gradient_frame(160, 120));
  for (std::size_t i = 0; i < text_like.frames[0].rgba.size(); i += 4 * 7) text_like.frames[0].rgba[i] = 0;
  const auto fast = test::write_from("png", text_like, R"({"compression_level":1})");
  const auto best = test::write_from("png", text_like, R"({"compression_level":9})");
  const auto stored = test::write_from("png", smooth, R"({"compression_level":0})");
  const auto packed = test::write_from("png", smooth, R"({"compression_level":6})");
  REQUIRE_OK(stored);
  REQUIRE_OK(packed);
  CHECK(packed->size() * 4 < stored->size());
  REQUIRE_OK(fast);
  REQUIRE_OK(best);
  CHECK(best->size() <= fast->size());
  const auto lossless = test::write_from("png", photo);
  REQUIRE_OK(lossless);
  std::size_t previous = lossless->size();
  for (const int quality : {80, 50, 20}) {
    INFO("quality ", quality);
    const auto small = test::write_from("png", photo, std::format(R"({{"quality":{}}})", quality));
    REQUIRE_OK(small);
    CHECK(png_color_type(*small) == 3);
    CHECK(small->size() < previous);
    previous = small->size();
    const auto decoded = test::read_as<ir::Image>("png", *small);
    REQUIRE_OK(decoded);
    CHECK(test::psnr(decoded->frames[0], photo.frames[0]) >= 15.0 + 0.2 * quality);
  }
  // lossless=true wins over quality; preset small alone also selects the palette path.
  const auto forced = test::write_from("png", photo, R"({"quality":50,"lossless":true})");
  REQUIRE_OK(forced);
  CHECK(png_color_type(*forced) == 2);
  const auto preset = test::write_from("png", photo, R"({"preset":"small"})");
  REQUIRE_OK(preset);
  CHECK(png_color_type(*preset) == 3);
  // Noise cannot meet the floor with 256 colours at high quality: stays lossless.
  const ir::Image noise = single(test::noise_frame(64, 64, 3));
  const auto noisy = test::write_from("png", noise, R"({"quality":95})");
  REQUIRE_OK(noisy);
  CHECK(test::pixels_equal(test::read_as<ir::Image>("png", *noisy)->frames[0], noise.frames[0]));
  // Alpha survives quantisation.
  const ir::Image alpha = single(test::alpha_gradient_frame(64, 64));
  const auto qa = test::write_from("png", alpha, R"({"quality":70,"dither":"none"})");
  REQUIRE_OK(qa);
  CHECK(test::psnr(test::read_as<ir::Image>("png", *qa)->frames[0], alpha.frames[0]) > 30.0);
}

TEST_CASE("image/png: animations round trip as APNG with frame differencing") {
  ir::Image anim;
  anim.loop_count = 3;
  for (std::uint32_t n = 0; n < 4; ++n) {
    ir::Frame frame = test::gradient_frame(48, 32);
    for (std::uint32_t y = 10; y < 14; ++y) {
      for (std::uint32_t x = 5 * n; x < 5 * n + 6; ++x) frame.rgba[frame.offset(x, y) + 3] = static_cast<std::uint8_t>(40 * n);
    }
    frame.delay_ms = 40 + n * 10;
    anim.frames.push_back(std::move(frame));
  }
  const auto encoded = test::write_from("png", anim);
  REQUIRE_OK(encoded);
  const auto decoded = test::read_as<ir::Image>("png", *encoded);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->frames.size() == 4);
  CHECK(decoded->loop_count == 3);
  CHECK(test::pixels_equal(*decoded, anim));
  for (std::size_t n = 0; n < 4; ++n) CHECK(decoded->frames[n].delay_ms == anim.frames[n].delay_ms);
}

TEST_CASE("image/png: metadata is stripped by default and kept on request") {
  const Bytes source = test::fixture("image/png/metadata.png");
  const test::Converted stripped = test::convert(source, "png", "png");
  REQUIRE(stripped.ok());
  const auto plain = test::read_as<ir::Image>("png", stripped.data);
  REQUIRE_OK(plain);
  CHECK(plain->metadata.empty());
  CHECK(plain->exif.empty());
  CHECK(plain->width() == 2);  // orientation stays applied to the pixels
  const test::Converted kept = test::convert(source, "png", "png", R"({"strip_metadata":false})");
  REQUIRE(kept.ok());
  const auto rich = test::read_as<ir::Image>("png", kept.data);
  REQUIRE_OK(rich);
  CHECK(rich->metadata.at("Title") == "Test pattern");
  CHECK(rich->metadata.at("Description") == "caf\xC3\xA9 \xE2\x9C\x93");
  CHECK_FALSE(rich->exif.empty());
  CHECK(rich->width() == 2);  // orientation was reset to 1 in the stored EXIF: no double rotation
  CHECK(rich->frames[0].rgba[rich->frames[0].offset(1, 0)] == 255);
  CHECK_APPROX(rich->dpi_x, 150.0, 0.1);
}

TEST_CASE("image/png: size options resize on write") {
  const ir::Image big = single(test::gradient_frame(200, 100));
  const auto encoded = test::write_from("png", big, R"({"max_width":50})");
  REQUIRE_OK(encoded);
  const auto decoded = test::read_as<ir::Image>("png", *encoded);
  REQUIRE_OK(decoded);
  CHECK(decoded->width() == 50);
  CHECK(decoded->height() == 25);
  const auto scaled = test::read_as<ir::Image>("png", *test::write_from("png", big, R"({"scale":0.1})"));
  REQUIRE_OK(scaled);
  CHECK(scaled->width() == 20);
  CHECK(test::ssim(scaled->frames[0], test::gradient_frame(20, 10)) > 0.97);
}

TEST_CASE("image/png: damaged files are errors, never crashes") {
  const char* names[] = {"rgb8.png", "rgba16_interlaced.png", "palette4.png", "anim.png", "metadata.png", "grey1.png"};
  for (const char* name : names) {
    const Bytes valid = test::fixture(std::string("image/png/") + name);
    CHECK(test::fuzz_reader<ir::Image>("png", valid) > 0);
  }
  const Bytes valid = test::fixture("image/png/rgb8.png");
  CHECK_STATUS(test::read_as<ir::Image>("png", ByteView(valid).first(20)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Image>("png", test::bytes("not a png")), Status::Decode);
  // Declared size far beyond the pixel limit.
  Bytes bomb = valid;
  bomb[16] = std::byte{0x00};
  bomb[17] = std::byte{0xFF};
  bomb[20] = std::byte{0x00};
  bomb[21] = std::byte{0xFF};
  // Fix the IHDR CRC so the size check is what fails.
  const std::uint32_t crc = util::crc32(ByteView(bomb).subspan(12, 17));
  for (int i = 0; i < 4; ++i) bomb[29 + i] = static_cast<std::byte>(crc >> (24 - 8 * i));
  Limits limits;
  limits.max_pixels = 1'000'000;
  CHECK_STATUS(test::read_as<ir::Image>("png", bomb, {}, nullptr, limits), Status::Limit);
}

TEST_CASE("image/png: detection and engine routes") {
  const Bytes still = test::fixture("image/png/rgb8.png");
  const test::Converted compress = test::convert(still, "", "png", "", "picture.png");
  CHECK(compress.ok());
  CHECK(compress.format_id == "png");
  CHECK(test::convert(test::fixture("image/png/anim.png"), "", "png").ok());
  const auto* reg = Registry::instance().find("apng");
  REQUIRE(reg != nullptr);
  CHECK(Registry::instance().validate().empty());
}

TEST_CASE("image/png: the png video codec round trips frames") {
  const auto& media = Registry::instance().media();
  auto encoder = media.make_video_encoder(codec::id::png);
  auto decoder = media.make_video_decoder(codec::id::png);
  REQUIRE(encoder != nullptr);
  REQUIRE(decoder != nullptr);
  codec::VideoEncodeParams params;
  params.width = 16;
  params.height = 8;
  params.pixel_format = codec::PixelFormat::RGBA8;
  params.timescale = 1000;
  REQUIRE_OK(encoder->configure(params));
  codec::VideoFrame frame = codec::VideoFrame::allocate(codec::PixelFormat::RGBA8, 16, 8);
  const ir::Frame gradient = test::gradient_frame(16, 8);
  frame.planes[0].data = gradient.rgba;
  frame.pts_ms = 40;
  REQUIRE_OK(encoder->send_frame(frame));
  auto packet = encoder->receive_packet();
  REQUIRE_OK(packet);
  REQUIRE(packet->has_value());
  CHECK((*packet)->keyframe);
  codec::CodecParams cp;
  cp.codec = std::string(codec::id::png);
  REQUIRE_OK(decoder->configure(cp));
  REQUIRE_OK(decoder->send_packet(**packet));
  auto out = decoder->receive_frame();
  REQUIRE_OK(out);
  REQUIRE(out->has_value());
  CHECK((*out)->pts_ms == 40);
  CHECK((*out)->planes[0].data == gradient.rgba);
}

TEST_CASE("image/png: the small path never makes a file larger") {
  const ir::Image smooth = single(test::gradient_frame(160, 120));
  const auto lossless = test::write_from("png", smooth);
  const auto small = test::write_from("png", smooth, R"({"quality":60})");
  REQUIRE_OK(lossless);
  REQUIRE_OK(small);
  CHECK(small->size() <= lossless->size());
}
