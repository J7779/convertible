// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
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

ir::Image gif(std::string_view name) { return load("gif", std::string("image/gif/") + std::string(name)); }

ir::Frame palette_pattern() {
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

void fill(ir::Frame& f, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h, image::Rgba c) {
  for (std::uint32_t y = y0; y < y0 + h; ++y) {
    for (std::uint32_t x = x0; x < x0 + w; ++x) std::memcpy(f.rgba.data() + f.offset(x, y), c.data(), 4);
  }
}

}  // namespace

TEST_CASE("image/gif: decodes palette, interlaced, 87a and transparent files exactly") {
  const ir::Frame expected = palette_pattern();
  for (const char* name : {"palette.gif", "interlaced.gif", "palette87a.gif"}) {
    INFO(name);
    const ir::Image image = gif(name);
    REQUIRE(image.frames.size() == 1);
    CHECK(test::pixels_equal(image.frames[0], expected));
  }
  const ir::Image tr = gif("transparent.gif");
  REQUIRE(tr.valid());
  ir::Frame t = ir::Frame::filled(32, 24, {0, 0, 0, 0});
  const image::Rgba colours[3] = {{0, 0, 0, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}};
  for (std::uint32_t y = 0; y < 24; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const std::uint32_t i = (x / 8) % 4;
      if (i < 3) std::memcpy(t.rgba.data() + t.offset(x, y), colours[i].data(), 4);
    }
  }
  CHECK(test::pixels_equal(tr.frames[0], t));
  const ir::Image gradient = gif("gradient.gif");
  REQUIRE(gradient.valid());
  CHECK(test::psnr(gradient.frames[0], test::gradient_frame(32, 24)) > 30.0);
}

TEST_CASE("image/gif: animations follow every disposal method") {
  const ir::Image anim = gif("anim_disposal.gif");
  REQUIRE(anim.frames.size() == 4);
  CHECK(anim.loop_count == 3);
  CHECK(anim.frames[0].delay_ms == 100);
  CHECK(anim.frames[3].delay_ms == 400);
  ir::Frame f0 = ir::Frame::filled(32, 24, {255, 0, 0, 255});
  ir::Frame f1 = f0;
  fill(f1, 8, 4, 8, 8, {0, 0, 255, 255});
  ir::Frame f2 = f0;
  fill(f2, 8, 4, 8, 8, {0, 0, 0, 0});  // disposed to background (transparent)
  fill(f2, 16, 8, 8, 8, {0, 255, 0, 255});
  ir::Frame f3 = f0;
  fill(f3, 8, 4, 8, 8, {0, 0, 0, 0});  // frame 2 restored to the previous state
  fill(f3, 0, 0, 4, 4, {255, 255, 255, 255});
  CHECK(test::pixels_equal(anim.frames[0], f0));
  CHECK(test::pixels_equal(anim.frames[1], f1));
  CHECK(test::pixels_equal(anim.frames[2], f2));
  CHECK(test::pixels_equal(anim.frames[3], f3));
  const ir::Image pillow = gif("anim_pillow.gif");
  REQUIRE(pillow.frames.size() == 4);
  CHECK(pillow.loop_count == 0);
  for (std::uint32_t n = 0; n < 4; ++n) {
    ir::Frame e = ir::Frame::filled(32, 24, {20, 40, 60, 255});
    fill(e, 4 * n, 8, 8, 8, {250, 200, 0, 255});
    INFO("frame ", n);
    CHECK(test::pixels_equal(pillow.frames[n], e));
    CHECK(pillow.frames[n].delay_ms == 50);
  }
}

TEST_CASE("image/gif: round trips") {
  // Few colours: exact.
  const ir::Image exact = single(palette_pattern());
  const auto encoded = test::write_from("gif", exact);
  REQUIRE_OK(encoded);
  CHECK(test::pixels_equal(test::read_as<ir::Image>("gif", *encoded)->frames[0], exact.frames[0]));
  // Transparency survives.
  ir::Frame clear = test::checker_frame(20, 20, 4);
  fill(clear, 0, 0, 6, 6, {0, 0, 0, 0});
  const auto tr = test::write_from("gif", single(clear));
  REQUIRE_OK(tr);
  CHECK(test::pixels_equal(test::read_as<ir::Image>("gif", *tr)->frames[0], clear));
  // Photos are quantised with a PSNR floor; fewer colours at lower quality.
  const ir::Image photo = single(photo_frame(80, 60));
  std::size_t previous = SIZE_MAX;
  for (const int q : {90, 50, 20}) {
    INFO("quality ", q);
    const auto out = test::write_from("gif", photo, std::format(R"({{"quality":{}}})", q));
    REQUIRE_OK(out);
    CHECK(out->size() < previous);
    previous = out->size();
    CHECK(test::psnr(test::read_as<ir::Image>("gif", *out)->frames[0], photo.frames[0]) > (q >= 90 ? 30.0 : 24.0));
  }
  const auto ordered = test::write_from("gif", photo, R"({"dither":"ordered"})");
  const auto none = test::write_from("gif", photo, R"({"dither":"none"})");
  REQUIRE_OK(ordered);
  REQUIRE_OK(none);
  CHECK(test::psnr(test::read_as<ir::Image>("gif", *none)->frames[0], photo.frames[0]) > 28.0);
}

TEST_CASE("image/gif: animations round trip with frame differencing") {
  ir::Image anim;
  anim.loop_count = 0;
  for (std::uint32_t n = 0; n < 6; ++n) {
    ir::Frame f = test::noise_frame(64, 48, 4);
    for (std::size_t i = 0; i < f.rgba.size(); i += 4) f.rgba[i] &= 0xC0, f.rgba[i + 1] &= 0xC0, f.rgba[i + 2] &= 0xC0;
    fill(f, 5 * n, 10, 10, 10, {255, 255, 0, 255});
    if (n == 3) fill(f, 50, 30, 6, 6, {0, 0, 0, 0});  // a hole appears: needs disposal
    f.delay_ms = 70;
    anim.frames.push_back(std::move(f));
  }
  const auto encoded = test::write_from("gif", anim);
  REQUIRE_OK(encoded);
  const auto decoded = test::read_as<ir::Image>("gif", *encoded);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->frames.size() == 6);
  CHECK(decoded->loop_count == 0);
  CHECK(test::pixels_equal(*decoded, anim));
  CHECK(decoded->frames[2].delay_ms == 70);
  // Differencing keeps the file far smaller than six full frames.
  const auto still = test::write_from("gif", single(anim.frames[0]));
  REQUIRE_OK(still);
  CHECK(encoded->size() < still->size() * 2);
  // A many-colour animation with local palettes.
  ir::Image photos;
  for (std::uint32_t n = 0; n < 3; ++n) photos.frames.push_back(photo_frame(40, 30, n + 1));
  for (const char* mode : {"local", "global"}) {
    INFO(mode);
    const auto out = test::write_from("gif", photos, std::format(R"({{"palette":"{}"}})", mode));
    REQUIRE_OK(out);
    const auto back = test::read_as<ir::Image>("gif", *out);
    REQUIRE_OK(back);
    CHECK(test::psnr(*back, photos) > 26.0);
  }
}

TEST_CASE("image/gif: damaged files are errors, never crashes") {
  for (const char* name : {"palette.gif", "interlaced.gif", "anim_disposal.gif", "anim_pillow.gif", "gradient.gif"}) {
    CHECK(test::fuzz_reader<ir::Image>("gif", test::fixture(std::string("image/gif/") + name)) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Image>("gif", test::bytes("GIF89a")), Status::Decode);
  const Bytes bomb = test::bytes(std::string("GIF89a\xFF\xFF\xFF\xFF\x00\x00\x00", 13));
  Limits limits;
  limits.max_pixels = 1'000'000;
  CHECK_STATUS(test::read_as<ir::Image>("gif", bomb, {}, nullptr, limits), Status::Limit);
}

TEST_CASE("image/gif: detection and conversion") {
  const test::Converted out = test::convert(test::fixture("image/gif/anim_pillow.gif"), "", "png");
  REQUIRE(out.ok());
  CHECK(test::read_as<ir::Image>("png", out.data)->frames.size() == 4);  // becomes APNG
  CHECK(test::convert(test::fixture("image/gif/palette.gif"), "", "gif").ok());
}
