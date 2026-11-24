// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <limits>
#include <string>

#include "families/codec_legacy/legacy_test_support.hpp"
#include "support/test.hpp"

using namespace cvt;
namespace tl = cvt::test::legacy;

namespace {

/// Decodes a fixture with the coded depth a container would pass, and
/// compares every frame with ffmpeg's decode (MD5).
bool decodes_exactly(const std::string& name, std::uint16_t depth) {
  tl::PacketFile file = tl::load_packets(name);
  codec::CodecParams params = file.params();
  params.bits_per_sample = depth;
  auto decoder = cvt::legacy::make_video_decoder(file.codec);
  REQUIRE(decoder != nullptr);
  auto frames = tl::decode_packets(*decoder, params, file.packets);
  REQUIRE_OK(frames);
  const tl::RefFile ref = tl::load_reference(name);
  return tl::compare_with_reference(*frames, ref) == std::numeric_limits<double>::infinity();
}

}  // namespace

TEST_CASE("codec_legacy/qtrle: decodes 24-bit, 32-bit, 16-bit and grey QuickTime Animation") {
  CHECK(decodes_exactly("qtrle_rgb24_64x64", 24));
  CHECK(decodes_exactly("qtrle_argb_64x64", 32));
  CHECK(decodes_exactly("qtrle_rgb555_64x64", 16));
  CHECK(decodes_exactly("qtrle_gray_64x64", 40));
}

TEST_CASE("codec_legacy/msrle: decodes 8-bit Microsoft RLE with its palette") {
  CHECK(decodes_exactly("msrle_64x64", 8));
}

TEST_CASE("codec_legacy/msvideo1: decodes 16-bit Microsoft Video 1") {
  CHECK(decodes_exactly("msvideo1_64x64", 16));
}

TEST_CASE("codec_legacy/rpza: decodes QuickTime Video") {
  CHECK(decodes_exactly("rpza_64x64", 16));
}

TEST_CASE("codec_legacy/smc: decodes QuickTime Graphics with the sample description palette") {
  CHECK(decodes_exactly("smc_64x64", 8));
}

TEST_CASE("codec_legacy/cinepak: decodes Cinepak key and delta frames") {
  CHECK(decodes_exactly("cinepak_64x64", 24));
}

TEST_CASE("codec_legacy/qtrle: encoder round trip is lossless; delta frames are smaller") {
  for (const ir::PixelFormat format : {ir::PixelFormat::I420, ir::PixelFormat::RGBA8}) {
    std::vector<ir::VideoFrame> frames;
    for (std::uint32_t i = 0; i < 6; ++i) {
      // Static for three pictures, then a small moving square.
      ir::VideoFrame f = test::test_pattern_frame(ir::PixelFormat::RGBA8, 61, 37, 0);
      if (i >= 3) {
        for (std::uint32_t y = 5; y < 12; ++y) {
          for (std::uint32_t x = 3 * i; x < 3 * i + 9; ++x) f.planes[0].data[y * f.planes[0].stride + x * 4] = 0;
        }
      }
      if (format == ir::PixelFormat::RGBA8) {
        for (std::size_t k = 3; k < f.planes[0].data.size(); k += 4) f.planes[0].data[k] = static_cast<std::uint8_t>(k * 7);
      }
      frames.push_back(std::move(f));
    }
    codec::VideoEncodeParams params;
    params.width = 61;
    params.height = 37;
    params.pixel_format = format;
    params.keyframe_interval = 4;
    auto encoded = tl::encode_frames("qtrle", params, frames);
    REQUIRE_OK(encoded);
    REQUIRE(encoded->packets.size() == 6);
    CHECK(encoded->packets[0].keyframe);
    CHECK_FALSE(encoded->packets[1].keyframe);
    CHECK(encoded->packets[4].keyframe);
    CHECK(encoded->packets[1].bytes.size() < 16);  // identical picture: "no change"
    CHECK(encoded->packets[5].bytes.size() < encoded->packets[4].bytes.size());
    const std::uint16_t depth = format == ir::PixelFormat::RGBA8 ? 32 : 24;
    tl::dump_for_interop(std::string("qtrle_") + std::to_string(depth), "qtrle", 61, 37, params.timescale, *encoded);
    tl::dump_raw_for_interop(std::string("qtrle_") + std::to_string(depth), frames);
    auto decoder = cvt::legacy::make_video_decoder("qtrle");
    codec::CodecParams cp;
    cp.width = 61;
    cp.height = 37;
    cp.bits_per_sample = depth;
    cp.timescale = params.timescale;
    auto decoded = tl::decode_packets(*decoder, cp, encoded->packets);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->size() == 6);
    for (std::size_t i = 0; i < 6; ++i) {
      ir::VideoFrame expected = frames[i];
      if (depth == 24) {
        for (std::size_t k = 3; k < expected.planes[0].data.size(); k += 4) expected.planes[0].data[k] = 255;
      }
      CHECK(test::psnr((*decoded)[i], expected) == std::numeric_limits<double>::infinity());
    }
  }
}

TEST_CASE("codec_legacy/simple: damaged streams never crash") {
  for (const auto& [name, depth] : std::initializer_list<std::pair<const char*, std::uint16_t>>{
           {"qtrle_rgb24_64x64", 24}, {"qtrle_gray_64x64", 40}, {"msrle_64x64", 8}, {"msvideo1_64x64", 16},
           {"rpza_64x64", 16}, {"smc_64x64", 8}, {"cinepak_64x64", 24}}) {
    tl::PacketFile file = tl::load_packets(name);
    (void)depth;
    tl::fuzz_decoder(file, 3);
  }
}
