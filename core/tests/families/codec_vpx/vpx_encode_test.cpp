// VP8 and VP9 encoders: quality per level, size versus quality, lossless
// VP9, 10 bit, keyframe interval, bitrate mode, timestamps and vpcC.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string_view>
#include <vector>

#include "codec/colorspace.hpp"
#include "codec/ids.hpp"
#include "families/codec_vpx/vpx_bitstream.hpp"
#include "families/codec_vpx/vpx_codecs.hpp"
#include "families/codec_vpx/vpx_test_util.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

codec::VideoEncodeParams params_for(std::uint32_t width, std::uint32_t height, int quality,
                                    ir::PixelFormat format = ir::PixelFormat::I420) {
  codec::VideoEncodeParams params;
  params.width = width;
  params.height = height;
  params.fps = {25, 1};
  params.quality = quality;
  params.pixel_format = format;
  params.timescale = 1000;
  params.preset = codec::EncodePreset::Fast;  // keep the Debug test run quick
  return params;
}

/// Encode, decode with our decoder, and return the worst per-frame PSNR.
struct RoundTrip {
  double psnr = 0;
  std::size_t bytes = 0;
  test::vpx::Encoded encoded;
  std::vector<ir::VideoFrame> decoded;
};

RoundTrip round_trip(std::string_view codec_id, const codec::VideoEncodeParams& params,
                     const std::vector<ir::VideoFrame>& frames) {
  RoundTrip result;
  auto encoded = test::vpx::encode_all(codec_id, params, frames);
  REQUIRE_OK(encoded);
  result.encoded = std::move(*encoded);
  result.bytes = result.encoded.bytes();
  auto decoded = test::vpx::decode_all(codec_id, result.encoded.packets, params.timescale);
  REQUIRE_OK(decoded);
  result.decoded = std::move(*decoded);
  // VP8 always codes BT.601 limited range; compare against the input as the
  // encoder had to see it.
  std::vector<ir::VideoFrame> reference;
  for (const ir::VideoFrame& frame : frames) {
    const ir::ColorInfo target = result.decoded.empty() ? frame.color : result.decoded.front().color;
    if (frame.format == ir::PixelFormat::RGBA8 || frame.color == target) {
      reference.push_back(frame);
      continue;
    }
    auto converted = codec::convert_frame(frame, frame.format, target);
    REQUIRE_OK(converted);
    reference.push_back(std::move(*converted));
  }
  result.psnr = test::min_psnr(reference, result.decoded);
  return result;
}

constexpr std::uint32_t width = 96;
constexpr std::uint32_t height = 64;
constexpr std::uint32_t frame_count = 12;

}  // namespace

TEST_CASE("codec_vpx: quality maps onto the quantiser monotonically") {
  CHECK(vpx::quantizer_for_quality(100) == 0);
  CHECK(vpx::quantizer_for_quality(1) == 63);
  CHECK(vpx::quantizer_for_quality(90) == 20);
  for (int q = 2; q <= 100; ++q) CHECK(vpx::quantizer_for_quality(q) <= vpx::quantizer_for_quality(q - 1));
  CHECK(vpx::cpu_used_for_preset(vpx::Codec::Vp9, codec::EncodePreset::Small) <
        vpx::cpu_used_for_preset(vpx::Codec::Vp9, codec::EncodePreset::Fast));
  CHECK(vpx::default_keyframe_interval(25.0) == 250u);
  CHECK(vpx::default_keyframe_interval(0.0) == 300u);
}

TEST_CASE("codec_vpx: quality levels meet PSNR floors and lower quality is smaller") {
  const std::vector<ir::VideoFrame> frames = test::vpx::textured_sequence(ir::PixelFormat::I420, width, height,
                                                                          frame_count);
  struct Level {
    int quality;
    double vp8_floor;
    double vp9_floor;
  };
  // Floors: what this sequence measured (VP8 45.3/41.9/38.7/33.9/28.3 dB, VP9
  // 48.0/44.4/40.2/32.0/26.3 dB at the Fast preset) minus about 1.5 dB.
  const Level levels[] = {{95, 43.5, 46.0}, {90, 40.0, 42.5}, {75, 37.0, 38.5}, {50, 32.0, 30.0}, {20, 26.5, 24.5}};
  for (const std::string_view codec_id : {codec::id::vp8, codec::id::vp9}) {
    INFO("codec ", codec_id);
    std::size_t previous_size = 0;
    for (const Level& level : levels) {
      INFO("quality ", level.quality);
      const RoundTrip result = round_trip(codec_id, params_for(width, height, level.quality), frames);
      REQUIRE(result.decoded.size() == frames.size());
      MESSAGE(codec_id, " quality ", level.quality, ": ", result.bytes, " bytes, min PSNR ", result.psnr, " dB");
      CHECK(result.psnr >= (codec_id == codec::id::vp8 ? level.vp8_floor : level.vp9_floor));
      if (previous_size != 0) CHECK(result.bytes < previous_size);
      previous_size = result.bytes;
    }
  }
}

TEST_CASE("codec_vpx/vp9: quality 100 is lossless (8 and 10 bit)") {
  for (const ir::PixelFormat format : {ir::PixelFormat::I420, ir::PixelFormat::I420P10, ir::PixelFormat::I444}) {
    INFO("format ", static_cast<int>(format));
    const auto frames = test::vpx::textured_sequence(format, 48, 32, 6);
    const RoundTrip result = round_trip(codec::id::vp9, params_for(48, 32, 100, format), frames);
    REQUIRE(result.decoded.size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
      CHECK(result.decoded[i].format == format);
      CHECK(test::vpx::frame_md5(result.decoded[i]) == test::vpx::frame_md5(frames[i]));
    }
  }
}

TEST_CASE("codec_vpx/vp9: 10-bit input is encoded as profile 2") {
  const auto frames = test::vpx::textured_sequence(ir::PixelFormat::I420P10, width, height, 8);
  const RoundTrip result = round_trip(codec::id::vp9, params_for(width, height, 90, ir::PixelFormat::I420P10), frames);
  REQUIRE(result.decoded.size() == frames.size());
  CHECK(result.decoded.front().format == ir::PixelFormat::I420P10);
  CHECK(result.psnr >= 40.0);
  const auto header = vpx::parse_vp9_header(vpx::split_vp9_superframe(result.encoded.packets.front().bytes)->front());
  REQUIRE_OK(header);
  CHECK(header->profile == 2);
  CHECK(header->bit_depth == 10);
  const auto vpcc = vpx::parse_vpcc(result.encoded.extradata);
  REQUIRE_OK(vpcc);
  CHECK(vpcc->profile == 2);
  CHECK(vpcc->bit_depth == 10);
}

TEST_CASE("codec_vpx: one packet per frame with the input timestamps") {
  const auto frames = test::vpx::textured_sequence(ir::PixelFormat::I420, width, height, 20);
  for (const std::string_view codec_id : {codec::id::vp8, codec::id::vp9}) {
    INFO("codec ", codec_id);
    codec::VideoEncodeParams params = params_for(width, height, 80);
    params.timescale = 90000;
    const auto encoded = test::vpx::encode_all(codec_id, params, frames);
    REQUIRE_OK(encoded);
    REQUIRE(encoded->packets.size() == frames.size());
    CHECK(encoded->packets.front().keyframe);
    for (std::size_t i = 0; i < frames.size(); ++i) {
      INFO("packet ", i);
      const ir::Packet& packet = encoded->packets[i];
      CHECK(packet.pts == frames[i].pts_ms * 90);
      CHECK(packet.dts == packet.pts);
      CHECK(packet.duration == 3600);
    }
    // Every packet shows exactly one picture.
    if (codec_id == codec::id::vp9) {
      for (const ir::Packet& packet : encoded->packets) {
        const auto parts = vpx::split_vp9_superframe(packet.bytes);
        REQUIRE_OK(parts);
        std::size_t shown = 0;
        for (const ByteView part : *parts) {
          const auto header = vpx::parse_vp9_header(part);
          REQUIRE_OK(header);
          if (header->show_frame || header->show_existing_frame) ++shown;
        }
        CHECK(shown == 1u);
      }
    }
    const auto decoded = test::vpx::decode_all(codec_id, encoded->packets, 90000);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
      CHECK((*decoded)[i].pts_ms == frames[i].pts_ms);
      CHECK((*decoded)[i].duration_ms == 40);
    }
  }
}

TEST_CASE("codec_vpx: the keyframe interval is respected") {
  const auto frames = test::vpx::textured_sequence(ir::PixelFormat::I420, 64, 48, 16);
  for (const std::string_view codec_id : {codec::id::vp8, codec::id::vp9}) {
    INFO("codec ", codec_id);
    codec::VideoEncodeParams params = params_for(64, 48, 70);
    params.keyframe_interval = 5;
    const auto encoded = test::vpx::encode_all(codec_id, params, frames);
    REQUIRE_OK(encoded);
    REQUIRE(encoded->packets.size() == frames.size());
    std::size_t since_key = 0;
    std::size_t keyframes = 0;
    for (std::size_t i = 0; i < encoded->packets.size(); ++i) {
      const ir::Packet& packet = encoded->packets[i];
      bool key = false;
      if (codec_id == codec::id::vp8) {
        key = vpx::parse_vp8_header(packet.bytes)->keyframe;
      } else {
        key = vpx::parse_vp9_header(vpx::split_vp9_superframe(packet.bytes)->front())->keyframe;
      }
      CHECK(key == packet.keyframe);
      if (key) {
        since_key = 0;
        ++keyframes;
      } else {
        ++since_key;
      }
      CHECK(since_key < 5u);
    }
    CHECK(keyframes >= 4u);
  }
}

TEST_CASE("codec_vpx: a target bitrate controls the size") {
  const auto frames = test::vpx::textured_sequence(ir::PixelFormat::I420, width, height, frame_count);
  for (const std::string_view codec_id : {codec::id::vp8, codec::id::vp9}) {
    INFO("codec ", codec_id);
    codec::VideoEncodeParams low = params_for(width, height, 90);
    low.bitrate_kbps = 100;
    codec::VideoEncodeParams high = low;
    high.bitrate_kbps = 800;
    const RoundTrip small = round_trip(codec_id, low, frames);
    const RoundTrip large = round_trip(codec_id, high, frames);
    CHECK(small.bytes < large.bytes);
    CHECK(small.psnr < large.psnr);
    CHECK(small.psnr >= 22.0);
  }
}

TEST_CASE("codec_vpx: presets trade speed for size, all at high quality") {
  const auto frames = test::vpx::textured_sequence(ir::PixelFormat::I420, 64, 48, 6);
  for (const std::string_view codec_id : {codec::id::vp8, codec::id::vp9}) {
    for (const codec::EncodePreset preset :
         {codec::EncodePreset::Fast, codec::EncodePreset::Balanced, codec::EncodePreset::Small}) {
      INFO("codec ", codec_id, " preset ", static_cast<int>(preset));
      codec::VideoEncodeParams params = params_for(64, 48, 90);
      params.preset = preset;
      const RoundTrip result = round_trip(codec_id, params, frames);
      CHECK(result.decoded.size() == frames.size());
      CHECK(result.psnr >= 38.0);
    }
  }
}

TEST_CASE("codec_vpx: other input layouts are converted; odd sizes work") {
  // NV12 and RGBA8 into VP9 (encoded as 4:2:0), odd dimensions into VP8.
  std::vector<ir::VideoFrame> nv12;
  std::vector<ir::VideoFrame> rgba;
  for (std::uint32_t i = 0; i < 4; ++i) {
    nv12.push_back(test::test_pattern_frame(ir::PixelFormat::NV12, 33, 19, i));
    rgba.push_back(test::test_pattern_frame(ir::PixelFormat::RGBA8, 33, 19, i));
  }
  {
    const auto encoded = test::vpx::encode_all(codec::id::vp9, params_for(33, 19, 90, ir::PixelFormat::NV12), nv12);
    REQUIRE_OK(encoded);
    const auto decoded = test::vpx::decode_all(codec::id::vp9, encoded->packets, 1000);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->size() == 4u);
    CHECK(decoded->front().format == ir::PixelFormat::I420);
    const auto reference = codec::convert_frame(nv12.front(), ir::PixelFormat::I420, nv12.front().color);
    REQUIRE_OK(reference);
    CHECK(test::psnr(*reference, decoded->front()) >= 38.0);
  }
  {
    const RoundTrip result = round_trip(codec::id::vp8, params_for(33, 19, 90, ir::PixelFormat::RGBA8), rgba);
    REQUIRE(result.decoded.size() == 4u);
    CHECK(result.decoded.front().width == 33u);
    CHECK(result.decoded.front().height == 19u);
    CHECK(result.decoded.front().color.matrix == ir::ColorMatrix::BT601);
    const auto back = codec::to_rgba(result.decoded.front());
    REQUIRE_OK(back);
    CHECK(test::psnr(rgba.front(), *back) >= 32.0);
  }
}

TEST_CASE("codec_vpx: vpcC describes the stream") {
  codec::VideoEncodeParams params = params_for(1280, 720, 90);
  params.fps = {30, 1};
  params.color = {ir::ColorMatrix::BT709, ir::ColorRange::Full, ir::ChromaSiting::Left};
  auto encoder = test::vpx::media().make_video_encoder(codec::id::vp9);
  REQUIRE(encoder != nullptr);
  REQUIRE_OK(encoder->configure(params));
  const auto vpcc = vpx::parse_vpcc(encoder->extradata());
  REQUIRE_OK(vpcc);
  CHECK(vpcc->profile == 0);
  CHECK(vpcc->level == 31);  // 1280x720 at 30 fps
  CHECK(vpcc->bit_depth == 8);
  CHECK(vpcc->chroma_subsampling == 0);
  CHECK(vpcc->full_range);
  CHECK(vpcc->matrix_coefficients == 1);
  CHECK(encoder->extradata().size() == 12u);

  auto vp8 = test::vpx::media().make_video_encoder(codec::id::vp8);
  REQUIRE_OK(vp8->configure(params));
  const auto vp8_config = vpx::parse_vpcc(vp8->extradata());
  REQUIRE_OK(vp8_config);
  CHECK(vp8_config->matrix_coefficients == 6);  // VP8 is always BT.601 limited range
  CHECK(!vp8_config->full_range);
}

TEST_CASE("codec_vpx: VP9 signals its colour description in the bitstream") {
  const auto frames = test::vpx::textured_sequence(ir::PixelFormat::I420, 64, 48, 3);
  std::vector<ir::VideoFrame> tagged = frames;
  for (ir::VideoFrame& frame : tagged) frame.color = {ir::ColorMatrix::BT2020, ir::ColorRange::Full, {}};
  codec::VideoEncodeParams params = params_for(64, 48, 90);
  params.color = tagged.front().color;
  const auto encoded = test::vpx::encode_all(codec::id::vp9, params, tagged);
  REQUIRE_OK(encoded);
  const auto decoded = test::vpx::decode_all(codec::id::vp9, encoded->packets, 1000);
  REQUIRE_OK(decoded);
  REQUIRE(!decoded->empty());
  CHECK(decoded->front().color.matrix == ir::ColorMatrix::BT2020);
  CHECK(decoded->front().color.range == ir::ColorRange::Full);
}

TEST_CASE("codec_vpx: encoder misuse is refused") {
  auto encoder = test::vpx::media().make_video_encoder(codec::id::vp9);
  REQUIRE(encoder != nullptr);
  CHECK_STATUS(encoder->configure(params_for(0, 16, 90)), Status::InvalidArgument);
  auto huge = test::vpx::media().make_video_encoder(codec::id::vp8);
  CHECK_STATUS(huge->configure(params_for(20000, 16, 90)), Status::InvalidArgument);

  auto good = test::vpx::media().make_video_encoder(codec::id::vp8);
  REQUIRE_OK(good->configure(params_for(32, 32, 90)));
  CHECK_STATUS(good->send_frame(test::test_pattern_frame(ir::PixelFormat::I420, 16, 16, 0)), Status::InvalidArgument);
  ir::VideoFrame broken = test::test_pattern_frame(ir::PixelFormat::I420, 32, 32, 0);
  broken.planes[1].data.clear();
  CHECK_STATUS(good->send_frame(broken), Status::InvalidArgument);
  REQUIRE_OK(good->flush());
  CHECK_STATUS(good->send_frame(test::test_pattern_frame(ir::PixelFormat::I420, 32, 32, 0)), Status::Internal);
}
