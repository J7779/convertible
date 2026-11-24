// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <limits>
#include <string>

#include "families/codec_legacy/legacy_test_support.hpp"
#include "support/test.hpp"

using namespace cvt;
namespace tl = cvt::test::legacy;

namespace {

bool decodes_exactly(const std::string& name) {
  const tl::PacketFile file = tl::load_packets(name);
  auto frames = tl::decode_file(file);
  REQUIRE_OK(frames);
  return tl::compare_with_reference(*frames, tl::load_reference(name)) == std::numeric_limits<double>::infinity();
}

/// Encodes, decodes with our decoder (extradata from the encoder) and
/// checks every picture is identical. Returns the total size.
std::size_t lossless_round_trip(const std::string& codec_id, ir::PixelFormat format, std::uint32_t w, std::uint32_t h,
                                codec::EncodePreset preset = codec::EncodePreset::Balanced) {
  INFO(codec_id, " ", w, "x", h);
  const auto frames = tl::pattern_frames(format, w, h, 3);
  codec::VideoEncodeParams params;
  params.width = w;
  params.height = h;
  params.pixel_format = format;
  params.preset = preset;
  params.color = frames[0].color;
  auto encoded = tl::encode_frames(codec_id, params, frames);
  REQUIRE_OK(encoded);
  const std::string dump = codec_id + "_" + std::to_string(static_cast<int>(format)) + "_" + std::to_string(w);
  tl::dump_for_interop(dump, codec_id, w, h, params.timescale, *encoded);
  tl::dump_raw_for_interop(dump, frames);
  auto decoder = cvt::legacy::make_video_decoder(codec_id);
  codec::CodecParams cp;
  cp.width = w;
  cp.height = h;
  cp.extradata = encoded->extradata;
  cp.timescale = params.timescale;
  auto decoded = tl::decode_packets(*decoder, cp, encoded->packets);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->size() == frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    (*decoded)[i].color = frames[i].color;
    CHECK(test::psnr((*decoded)[i], frames[i]) == std::numeric_limits<double>::infinity());
  }
  return encoded->total_bytes();
}

}  // namespace

TEST_CASE("codec_legacy/huffyuv: decodes 4:2:2 (left, median) and RGB (left, plane) exactly") {
  CHECK(decodes_exactly("huffyuv_yuv422_left_64x64"));
  CHECK(decodes_exactly("huffyuv_yuv422_median_64x64"));
  CHECK(decodes_exactly("huffyuv_rgb24_64x64"));
  CHECK(decodes_exactly("huffyuv_bgra_64x64"));
}

TEST_CASE("codec_legacy/huffyuv: encoder is lossless and compresses") {
  const std::size_t median = lossless_round_trip("huffyuv", ir::PixelFormat::I422, 64, 40);
  const std::size_t left = lossless_round_trip("huffyuv", ir::PixelFormat::I422, 64, 40, codec::EncodePreset::Fast);
  CHECK(median < 3 * 64 * 40 * 2);
  CHECK(left < 3 * 64 * 40 * 2);
  lossless_round_trip("huffyuv", ir::PixelFormat::RGBA8, 33, 17);
  lossless_round_trip("huffyuv", ir::PixelFormat::I422, 6, 1);
}

TEST_CASE("codec_legacy/huffyuv: damaged input never crashes") {
  tl::fuzz_decoder(tl::load_packets("huffyuv_yuv422_median_64x64"), 2);
  tl::fuzz_decoder(tl::load_packets("huffyuv_bgra_64x64"), 2);
  auto decoder = cvt::legacy::make_video_decoder("huffyuv");
  codec::CodecParams cp;
  cp.width = 64;
  cp.height = 64;
  CHECK_STATUS(decoder->configure(cp), Status::Decode);  // no tables
}

TEST_CASE("codec_legacy/ffv1: decodes versions 1 and 3, Golomb and range coded, slices, RGB and 10-bit exactly") {
  CHECK(decodes_exactly("ffv1_v1_yuv420_64x64"));
  CHECK(decodes_exactly("ffv1_v3_yuv420_slices_64x64"));
  CHECK(decodes_exactly("ffv1_v3_rgb_64x64"));
  CHECK(decodes_exactly("ffv1_v3_yuv422p10_64x64"));
}

TEST_CASE("codec_legacy/ffv1: encoder is lossless for every supported layout") {
  const std::size_t yuv = lossless_round_trip("ffv1", ir::PixelFormat::I420, 64, 48);
  CHECK(yuv < 3 * 64 * 48 * 3 / 2);
  lossless_round_trip("ffv1", ir::PixelFormat::I422, 37, 21);
  lossless_round_trip("ffv1", ir::PixelFormat::I444, 16, 16);
  lossless_round_trip("ffv1", ir::PixelFormat::I422P10, 40, 18);
  lossless_round_trip("ffv1", ir::PixelFormat::I420P10, 9, 7);
  lossless_round_trip("ffv1", ir::PixelFormat::RGBA8, 33, 17);
}

TEST_CASE("codec_legacy/ffv1: damaged input never crashes") {
  tl::fuzz_decoder(tl::load_packets("ffv1_v1_yuv420_64x64"), 2);
  tl::fuzz_decoder(tl::load_packets("ffv1_v3_yuv420_slices_64x64"), 2);
}

TEST_CASE("codec_legacy/utvideo: decodes 4:2:0, 4:2:2, RGB and RGBA with left and median prediction exactly") {
  CHECK(decodes_exactly("utvideo_yuv420_64x64"));
  CHECK(decodes_exactly("utvideo_yuv422_median_64x64"));
  CHECK(decodes_exactly("utvideo_rgb_median_64x64"));
  CHECK(decodes_exactly("utvideo_rgba_64x64"));
}

TEST_CASE("codec_legacy/utvideo: damaged input never crashes") {
  tl::fuzz_decoder(tl::load_packets("utvideo_yuv422_median_64x64"), 2);
  tl::fuzz_decoder(tl::load_packets("utvideo_rgba_64x64"), 2);
}
