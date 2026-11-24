// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <limits>
#include <string>

#include "families/codec_legacy/legacy_test_support.hpp"
#include "support/test.hpp"

using namespace cvt;
namespace tl = cvt::test::legacy;

TEST_CASE("codec_legacy/raw: decodes ffmpeg's uncompressed variants exactly") {
  for (const char* name : {"raw_uyvy_64x32", "raw_yuy2_64x32", "raw_bgr24_64x32", "raw_v210_48x32"}) {
    INFO(name);
    tl::PacketFile file = tl::load_packets(name);
    // ffmpeg's AVI stores raw BGR top-down (negative height in the BITMAPINFOHEADER).
    if (file.codec == "bgr24") file.extradata = test::bytes("top-down");
    auto frames = tl::decode_file(file);
    REQUIRE_OK(frames);
    CHECK(tl::compare_with_reference(*frames, tl::load_reference(name)) == std::numeric_limits<double>::infinity());
  }
  // r210 is reduced from 10 to 8 bits: within one level of ffmpeg's conversion.
  const tl::PacketFile file = tl::load_packets("raw_r210_64x32");
  auto frames = tl::decode_file(file);
  REQUIRE_OK(frames);
  const tl::RefFile ref = tl::load_reference("raw_r210_64x32");
  REQUIRE(frames->size() == ref.total);
  for (std::size_t i = 0; i < ref.indices.size(); ++i) {
    CHECK(test::psnr((*frames)[ref.indices[i]], tl::raw_to_frame(ref, ref.frames[i])) >= 48.0);
  }
}

TEST_CASE("codec_legacy/raw: round trips are exact in the native layouts") {
  for (const std::string codec_id : {"uyvy", "yuy2", "v210", "bgr24", "r210"}) {
    INFO(codec_id);
    const bool rgb = codec_id == "bgr24" || codec_id == "r210";
    const ir::PixelFormat format = rgb ? ir::PixelFormat::RGBA8
                                       : (codec_id == "v210" ? ir::PixelFormat::I422P10 : ir::PixelFormat::I422);
    for (const std::uint32_t w : {50u, 7u}) {
      const auto frames = tl::pattern_frames(format, w, 11, 3);
      codec::VideoEncodeParams params;
      params.width = w;
      params.height = 11;
      params.color = frames[0].color;
      auto encoded = tl::encode_frames(codec_id, params, frames);
      REQUIRE_OK(encoded);
      auto decoder = cvt::legacy::make_video_decoder(codec_id);
      codec::CodecParams cp;
      cp.width = w;
      cp.height = 11;
      cp.timescale = params.timescale;
      auto decoded = tl::decode_packets(*decoder, cp, encoded->packets);
      REQUIRE_OK(decoded);
      REQUIRE(decoded->size() == 3);
      for (std::size_t i = 0; i < 3; ++i) {
        (*decoded)[i].color = frames[i].color;
        CHECK(test::psnr((*decoded)[i], frames[i]) >= (codec_id == "r210" ? 100.0 : 1e9));
        CHECK((*decoded)[i].pts_ms == frames[i].pts_ms);
      }
    }
  }
}

TEST_CASE("codec_legacy/raw: truncated pictures and missing sizes are errors") {
  auto decoder = cvt::legacy::make_video_decoder("uyvy");
  codec::CodecParams cp;
  cp.width = 64;
  cp.height = 32;
  ir::Packet p;
  p.bytes.resize(100);
  CHECK_STATUS(tl::decode_packets(*decoder, cp, {p}), Status::Decode);
  auto other = cvt::legacy::make_video_decoder("v210");
  CHECK_STATUS(other->configure({}), Status::Decode);
  tl::fuzz_decoder(tl::load_packets("raw_v210_48x32"), 2);
}
