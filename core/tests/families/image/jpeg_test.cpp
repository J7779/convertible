// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "codec/colorspace.hpp"
#include "codec/ids.hpp"
#include "engine/registry.hpp"
#include "families/image/image_test_util.hpp"
#include "families/image/image_ops.hpp"

using namespace cvt;
using test::image_util::load;
using test::image_util::parse_ppm;
using test::image_util::photo_frame;
using test::image_util::single;

namespace {

ir::Image jpg(std::string_view name) { return load("jpg", std::string("image/jpeg/") + std::string(name)); }
ir::Frame reference(std::string_view name) { return parse_ppm(test::fixture(std::string("image/jpeg/") + std::string(name))); }

std::size_t count_marker(const Bytes& data, std::uint8_t marker) {
  std::size_t n = 0;
  for (std::size_t i = 0; i + 1 < data.size(); ++i) {
    if (data[i] == std::byte{0xFF} && data[i + 1] == std::byte{marker}) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("image/jpeg: decodes like libjpeg for every sampling layout") {
  const ir::Frame source = reference("source.ppm");
  REQUIRE(source.valid());
  struct Case {
    const char* name;
    const char* ref;
    double max_diff;
    double min_psnr;
  };
  for (const Case c : {Case{"q90_444.jpg", "q90_444.ppm", 2, 38}, Case{"q75_420.jpg", "q75_420.ppm", 3, 32},
                       Case{"q85_422.jpg", "q85_422.ppm", 3, 34}, Case{"q80_411.jpg", "q80_411.ppm", 16, 30},
                       Case{"progressive.jpg", "progressive.ppm", 3, 32}}) {
    INFO(c.name);
    const ir::Image image = jpg(c.name);
    REQUIRE(image.valid());
    CHECK(image.width() == 45);
    CHECK(image.height() == 29);
    const ir::Frame libjpeg = reference(c.ref);
    REQUIRE(libjpeg.valid());
    CHECK(test::max_abs_diff(image.frames[0], libjpeg) <= c.max_diff);
    CHECK(test::psnr(image.frames[0], source) >= c.min_psnr);
  }
  for (const char* name : {"q80_440.jpg", "restart.jpg", "sips.jpg", "pillow_icc.jpg", "arith.jpg",
                           "arith_progressive.jpg"}) {
    INFO(name);
    const ir::Image image = jpg(name);
    REQUIRE(image.valid());
    CHECK(test::psnr(image.frames[0], source) >= 30.0);
    CHECK(test::ssim(image.frames[0], source) >= 0.93);
  }
  // Arithmetic coding gives the same picture as the Huffman-coded file of the same quality.
  const ir::Image grey = jpg("grey.jpg");
  REQUIRE(grey.valid());
  CHECK(grey.source_grayscale);
  CHECK(image::is_grayscale(grey.frames[0]));
}

TEST_CASE("image/jpeg: CMYK (Adobe, inverted) decodes to the colours ImageMagick shows") {
  const ir::Image cmyk = jpg("cmyk.jpg");
  REQUIRE(cmyk.valid());
  CHECK(test::max_abs_diff(cmyk.frames[0], reference("cmyk.ppm")) <= 3.0);
  CHECK(test::psnr(cmyk.frames[0], reference("source.ppm")) >= 30.0);
}

TEST_CASE("image/jpeg: EXIF orientation, ICC, comment, density") {
  const ir::Image rotated = jpg("orientation6.jpg");
  REQUIRE(rotated.valid());
  // Stored 16x8 with a red top-left corner; upright it is 8x16 with red at the top-right.
  CHECK(rotated.width() == 8);
  CHECK(rotated.height() == 16);
  const ir::Frame& f = rotated.frames[0];
  CHECK(f.rgba[f.offset(6, 1)] > 200);
  CHECK(f.rgba[f.offset(6, 1) + 2] < 60);
  CHECK(f.rgba[f.offset(1, 1)] < 60);
  CHECK_FALSE(rotated.exif.empty());
  const ir::Image pillow = jpg("pillow_icc.jpg");
  REQUIRE(pillow.valid());
  CHECK(pillow.icc_profile.size() == 128);
  CHECK(pillow.metadata.at("Comment") == "synthetic test pattern");
}

TEST_CASE("image/jpeg: MPO files give their first picture and are told apart from JPEG") {
  const Bytes mpo = test::fixture("image/jpeg/stereo.mpo");
  const auto image = test::read_as<ir::Image>("mpo", mpo);
  REQUIRE_OK(image);
  CHECK(test::max_abs_diff(image->frames[0], reference("q90_444.ppm")) <= 2.0);
  const test::Converted detected = test::convert(mpo, "", "png");
  CHECK(detected.ok());
  const auto candidates = Registry::instance().find("mpo");
  REQUIRE(candidates != nullptr);
  const test::Converted to_jpg = test::convert(mpo, "", "jpg", "", "stereo.mpo");
  CHECK(to_jpg.ok());
}

TEST_CASE("image/jpeg: round trips meet quality floors and shrink with quality") {
  const ir::Image photo = single(photo_frame(96, 64));
  std::size_t previous = SIZE_MAX;
  struct Level {
    int quality;
    double ssim;
    double psnr;
  };
  for (const Level level : {Level{95, 0.97, 36}, Level{75, 0.86, 30}, Level{50, 0.82, 28}, Level{20, 0.70, 25}}) {
    INFO("quality ", level.quality);
    const auto encoded = test::write_from("jpg", photo, std::format(R"({{"quality":{}}})", level.quality));
    REQUIRE_OK(encoded);
    CHECK(encoded->size() < previous);
    previous = encoded->size();
    const auto decoded = test::read_as<ir::Image>("jpg", *encoded);
    REQUIRE_OK(decoded);
    CHECK(test::ssim(decoded->frames[0], photo.frames[0]) >= level.ssim);
    CHECK(test::psnr(decoded->frames[0], photo.frames[0]) >= level.psnr);
  }
  // The smooth gradient is easy: q90 is near transparent.
  const ir::Image gradient = single(test::gradient_frame(77, 51));
  const auto q90 = test::write_from("jpg", gradient);
  REQUIRE_OK(q90);
  CHECK(test::psnr(test::read_as<ir::Image>("jpg", *q90)->frames[0], gradient.frames[0]) >= 40.0);
  const auto q100 = test::write_from("jpg", gradient, R"({"quality":100})");
  REQUIRE_OK(q100);
  CHECK(test::psnr(test::read_as<ir::Image>("jpg", *q100)->frames[0], gradient.frames[0]) >= 45.0);
}

TEST_CASE("image/jpeg: progressive output decodes to the same picture as baseline") {
  const ir::Image photo = single(photo_frame(70, 45, 5));
  for (const char* sub : {"444", "420", "422"}) {
    INFO(sub);
    const auto baseline = test::write_from("jpg", photo, std::format(R"({{"quality":80,"chroma_subsampling":"{}"}})", sub));
    const auto progressive =
        test::write_from("jpg", photo, std::format(R"({{"quality":80,"progressive":true,"chroma_subsampling":"{}"}})", sub));
    REQUIRE_OK(baseline);
    REQUIRE_OK(progressive);
    CHECK(count_marker(*baseline, 0xC0) == 1);
    CHECK(count_marker(*progressive, 0xC2) == 1);
    const auto a = test::read_as<ir::Image>("jpg", *baseline);
    const auto b = test::read_as<ir::Image>("jpg", *progressive);
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    CHECK(test::max_abs_diff(a->frames[0], b->frames[0]) == 0.0);
  }
  const ir::Image grey = single(test::checker_frame(40, 30, 4));
  const auto pg = test::write_from("jpg", grey, R"({"progressive":true})");
  REQUIRE_OK(pg);
  const auto decoded = test::read_as<ir::Image>("jpg", *pg);
  REQUIRE_OK(decoded);
  CHECK(test::psnr(decoded->frames[0], grey.frames[0]) > 30.0);
  CHECK(decoded->source_grayscale);
}

TEST_CASE("image/jpeg: options") {
  const ir::Image photo = single(photo_frame(64, 48, 9));
  const auto s444 = test::write_from("jpg", photo, R"({"quality":80,"chroma_subsampling":"444"})");
  const auto s420 = test::write_from("jpg", photo, R"({"quality":80,"chroma_subsampling":"420"})");
  REQUIRE_OK(s444);
  REQUIRE_OK(s420);
  CHECK(s420->size() < s444->size());
  // Auto: 4:4:4 at quality 90, 4:2:0 below.
  const auto auto90 = test::write_from("jpg", photo, R"({"quality":90})");
  const auto auto80 = test::write_from("jpg", photo, R"({"quality":80})");
  REQUIRE_OK(auto90);
  REQUIRE_OK(auto80);
  CHECK(auto80->size() == s420->size());
  const auto grey = test::write_from("jpg", photo, R"({"grayscale":true})");
  REQUIRE_OK(grey);
  CHECK(image::is_grayscale(test::read_as<ir::Image>("jpg", *grey)->frames[0]));
  // Alpha is flattened onto the background.
  const ir::Image clear = single(ir::Frame::filled(16, 16, {0, 0, 0, 0}));
  std::vector<std::string> warnings;
  const auto flat = test::write_from("jpg", clear, R"({"background":"000000"})", &warnings);
  REQUIRE_OK(flat);
  CHECK(test::read_as<ir::Image>("jpg", *flat)->frames[0].rgba[0] <= 2);
  CHECK_FALSE(warnings.empty());
  CHECK_STATUS(test::write_from("jpg", clear, R"({"background":"nope"})"), Status::InvalidArgument);
  // Presets.
  const auto small = test::write_from("jpg", photo, R"({"preset":"small"})");
  const auto high = test::write_from("jpg", photo, R"({"preset":"high"})");
  REQUIRE_OK(small);
  REQUIRE_OK(high);
  CHECK(small->size() < high->size());
}

TEST_CASE("image/jpeg: metadata is stripped by default and kept on request") {
  const Bytes source = test::fixture("image/jpeg/orientation6.jpg");
  const test::Converted stripped = test::convert(source, "jpg", "jpg");
  REQUIRE(stripped.ok());
  CHECK(count_marker(stripped.data, 0xE1) == 0);
  const auto plain = test::read_as<ir::Image>("jpg", stripped.data);
  REQUIRE_OK(plain);
  CHECK(plain->width() == 8);  // still upright
  const test::Converted kept = test::convert(source, "jpg", "jpg", R"({"strip_metadata":false})");
  REQUIRE(kept.ok());
  CHECK(count_marker(kept.data, 0xE1) == 1);
  const auto rich = test::read_as<ir::Image>("jpg", kept.data);
  REQUIRE_OK(rich);
  CHECK(rich->width() == 8);  // orientation tag was reset: no double rotation
  const test::Converted icc = test::convert(test::fixture("image/jpeg/pillow_icc.jpg"), "jpg", "jpg",
                                            R"({"strip_metadata":false})");
  REQUIRE(icc.ok());
  const auto with_icc = test::read_as<ir::Image>("jpg", icc.data);
  REQUIRE_OK(with_icc);
  CHECK(with_icc->icc_profile.size() == 128);
  CHECK(with_icc->metadata.at("Comment") == "synthetic test pattern");
}

TEST_CASE("image/jpeg: same-format compress makes files smaller at lower quality") {
  const auto big = test::write_from("jpg", single(photo_frame(120, 80, 3)), R"({"quality":95})");
  REQUIRE_OK(big);
  const test::Converted smaller = test::convert(*big, "", "jpg", R"({"quality":60})", "photo.jpg");
  REQUIRE(smaller.ok());
  CHECK(smaller.data.size() < big->size());
  const test::Converted resized = test::convert(*big, "", "jpg", R"({"max_width":60})", "photo.jpg");
  REQUIRE(resized.ok());
  CHECK(test::read_as<ir::Image>("jpg", resized.data)->width() == 60);
}

TEST_CASE("image/jpeg: damaged files are errors or partial pictures, never crashes") {
  for (const char* name : {"q75_420.jpg", "progressive.jpg", "restart.jpg", "arith.jpg", "arith_progressive.jpg",
                           "cmyk.jpg", "orientation6.jpg", "grey.jpg"}) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Image>("jpg", test::fixture(std::string("image/jpeg/") + name)) > 0);
  }
  const Bytes valid = test::fixture("image/jpeg/q75_420.jpg");
  CHECK_STATUS(test::read_as<ir::Image>("jpg", ByteView(valid).first(100)), Status::Decode);
  // A truncated scan still yields a picture plus a warning.
  std::vector<std::string> warnings;
  const auto partial = test::read_as<ir::Image>("jpg", ByteView(valid).first(valid.size() - 120), {}, &warnings);
  REQUIRE_OK(partial);
  CHECK_FALSE(warnings.empty());
  // Absurd declared size.
  Bytes bomb = valid;
  for (std::size_t i = 0; i + 8 < bomb.size(); ++i) {
    if (bomb[i] == std::byte{0xFF} && bomb[i + 1] == std::byte{0xC0}) {
      bomb[i + 5] = bomb[i + 6] = bomb[i + 7] = bomb[i + 8] = std::byte{0xFF};
      break;
    }
  }
  Limits limits;
  limits.max_pixels = 1'000'000;
  CHECK_STATUS(test::read_as<ir::Image>("jpg", bomb, {}, nullptr, limits), Status::Limit);
}

TEST_CASE("image/jpeg: detection and engine integration") {
  const test::Converted out = test::convert(test::fixture("image/jpeg/q90_444.jpg"), "", "png", "", "photo.jfif");
  CHECK(out.ok());
  CHECK(Registry::instance().resolve("jfif")->id == "jpg");
  CHECK(Registry::instance().resolve("jpe")->id == "jpg");
}

TEST_CASE("image/mjpeg: video decoder and encoder work on planar frames") {
  const auto& media = Registry::instance().media();
  auto encoder = media.make_video_encoder(codec::id::mjpeg);
  auto decoder = media.make_video_decoder(codec::id::mjpeg);
  REQUIRE(encoder != nullptr);
  REQUIRE(decoder != nullptr);
  codec::VideoEncodeParams params;
  params.width = 64;
  params.height = 48;
  params.quality = 90;
  params.timescale = 1000;
  REQUIRE_OK(encoder->configure(params));
  codec::VideoFrame rgba = codec::VideoFrame::allocate(codec::PixelFormat::RGBA8, 64, 48);
  rgba.planes[0].data = test::gradient_frame(64, 48).rgba;
  const codec::ColorInfo video_color{ir::ColorMatrix::BT709, ir::ColorRange::Limited, ir::ChromaSiting::Left};
  const auto i420 = codec::from_rgba(rgba, codec::PixelFormat::I420, video_color);
  REQUIRE_OK(i420);
  codec::VideoFrame frame = *i420;
  frame.pts_ms = 80;
  frame.duration_ms = 40;
  REQUIRE_OK(encoder->send_frame(frame));
  const auto packet = encoder->receive_packet();
  REQUIRE_OK(packet);
  REQUIRE(packet->has_value());
  CHECK((*packet)->pts == 80);
  codec::CodecParams cp;
  cp.codec = std::string(codec::id::mjpeg);
  REQUIRE_OK(decoder->configure(cp));
  REQUIRE_OK(decoder->send_packet(**packet));
  const auto decoded = decoder->receive_frame();
  REQUIRE_OK(decoded);
  REQUIRE(decoded->has_value());
  CHECK((*decoded)->format == codec::PixelFormat::I420);
  CHECK((*decoded)->color.range == ir::ColorRange::Full);
  CHECK((*decoded)->pts_ms == 80);
  const auto back = codec::to_rgba(**decoded);
  REQUIRE_OK(back);
  ir::Frame result;
  result.width = 64;
  result.height = 48;
  result.rgba = back->planes[0].data;
  CHECK(test::psnr(result, test::gradient_frame(64, 48)) > 34.0);
  // A packet without Huffman tables (AVI MJPEG style) still decodes.
  Bytes stripped;
  const Bytes& full = (*packet)->bytes;
  for (std::size_t i = 0; i < full.size();) {
    if (i + 3 < full.size() && full[i] == std::byte{0xFF} && full[i + 1] == std::byte{0xC4}) {
      const std::size_t length = std::to_integer<std::size_t>(full[i + 2]) << 8 | std::to_integer<std::size_t>(full[i + 3]);
      i += 2 + length;
      continue;
    }
    stripped.push_back(full[i]);
    ++i;
  }
  // Re-encode with standard tables so that dropping DHT is valid.
  auto plain_encoder = media.make_video_encoder(codec::id::mjpeg);
  params.preset = codec::EncodePreset::Fast;
  REQUIRE_OK(plain_encoder->configure(params));
  REQUIRE_OK(plain_encoder->send_frame(frame));
  const auto plain_packet = plain_encoder->receive_packet();
  REQUIRE_OK(plain_packet);
  Bytes no_dht;
  const Bytes& pb = (*plain_packet)->bytes;
  for (std::size_t i = 0; i < pb.size();) {
    if (i + 3 < pb.size() && pb[i] == std::byte{0xFF} && pb[i + 1] == std::byte{0xC4}) {
      i += 2 + (std::to_integer<std::size_t>(pb[i + 2]) << 8 | std::to_integer<std::size_t>(pb[i + 3]));
      continue;
    }
    no_dht.push_back(pb[i]);
    ++i;
  }
  codec::Packet bare;
  bare.bytes = no_dht;
  auto decoder2 = media.make_video_decoder(codec::id::mjpeg);
  REQUIRE_OK(decoder2->configure(cp));
  REQUIRE_OK(decoder2->send_packet(bare));
  const auto bare_frame = decoder2->receive_frame();
  REQUIRE_OK(bare_frame);
  CHECK(bare_frame->has_value());
  (void)stripped;
}
