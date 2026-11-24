// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// The Kvazaar-based encoder: quality levels, keyframes, timestamps, extradata.
#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "families/codec_hevc/hevc_test_util.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

#include "codec/colorspace.hpp"
#include "families/codec_hevc/hevc_api.hpp"
#include "families/codec_hevc/hevc_internal.hpp"

using namespace cvt;
using namespace cvt::test::hevc_util;

namespace {

/// Busier synthetic content than the smooth test pattern: a gradient, a
/// checkerboard scrolling one pixel per frame and a band of fixed texture
/// (+-24 levels) sliding sideways, converted to I420 (BT.709, limited range).
ir::VideoFrame busy_frame(std::uint32_t width, std::uint32_t height, std::uint32_t t) {
  ir::VideoFrame rgba = ir::VideoFrame::allocate(ir::PixelFormat::RGBA8, width, height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t* px = rgba.planes[0].data.data() + y * rgba.planes[0].stride + x * 4;
      const std::uint32_t checker = (((x + t) / 8) + (y / 8)) % 2;
      std::uint32_t noise = (x + t) * 2654435761u ^ y * 40503u;
      noise ^= noise >> 13;
      noise *= 0x5bd1e995u;
      noise ^= noise >> 15;
      const bool band = y >= height / 3 && y < 2 * height / 3;
      const std::uint32_t base = (x * 255) / width;
      const auto textured = [&](std::uint32_t level, std::uint32_t shift) {
        const int value = static_cast<int>(level) + static_cast<int>((noise >> shift) % 49) - 24;
        return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
      };
      px[0] = band ? textured(base, 0) : static_cast<std::uint8_t>(checker ? 200 : base);
      px[1] = band ? textured(255 - base, 8) : static_cast<std::uint8_t>(checker ? 60 : 255 - base);
      px[2] = band ? textured((y * 255) / height, 16) : static_cast<std::uint8_t>((y * 255) / height);
      px[3] = 255;
    }
  }
  auto yuv = codec::from_rgba(rgba, ir::PixelFormat::I420, {});
  ir::VideoFrame frame = std::move(*yuv);
  frame.pts_ms = static_cast<std::int64_t>(t) * 40;
  frame.duration_ms = 40;
  return frame;
}

std::vector<ir::VideoFrame> busy(std::uint32_t width, std::uint32_t height, std::uint32_t count) {
  std::vector<ir::VideoFrame> frames;
  for (std::uint32_t i = 0; i < count; ++i) frames.push_back(busy_frame(width, height, i));
  return frames;
}

codec::VideoEncodeParams params_for(std::uint32_t width, std::uint32_t height, int quality) {
  codec::VideoEncodeParams params;
  params.width = width;
  params.height = height;
  params.fps = {25, 1};
  params.quality = quality;
  params.timescale = 90000;
  return params;
}

struct RoundTrip {
  std::vector<ir::Packet> packets;
  Bytes extradata;
  std::vector<ir::VideoFrame> decoded;
};

Result<RoundTrip> round_trip(const codec::VideoEncodeParams& params, const std::vector<ir::VideoFrame>& frames) {
  RoundTrip out;
  CVT_TRY_ASSIGN(out.packets, encode_frames(params, frames, &out.extradata));
  codec::CodecParams decode;
  decode.codec = "hevc";
  decode.extradata = out.extradata;
  decode.timescale = params.timescale;
  CVT_TRY_ASSIGN(out.decoded, decode_packets(decode, out.packets));
  return out;
}

}  // namespace

TEST_CASE("codec_hevc/encode: quality levels meet PSNR floors and size falls with quality") {
  const auto frames = busy(128, 80, 12);
  // Worst-frame PSNR floors, measured with a margin. The encoder is
  // deterministic, so Debug and Release builds produce the same bitstream.
  struct Level {
    int quality;
    double min_psnr;
  };
  for (const auto preset : {codec::EncodePreset::Fast, codec::EncodePreset::Balanced, codec::EncodePreset::Small}) {
    INFO("preset ", static_cast<int>(preset));
    std::size_t previous_size = 0;
    for (const Level level : {Level{20, 24.0}, Level{50, 29.0}, Level{75, 32.0}, Level{90, 36.0}, Level{100, 42.0}}) {
      INFO("quality ", level.quality);
      auto params = params_for(128, 80, level.quality);
      params.preset = preset;
      const auto result = round_trip(params, frames);
      REQUIRE_OK(result);
      REQUIRE(result->decoded.size() == frames.size());
      CHECK(test::min_psnr(frames, result->decoded) >= level.min_psnr);
      const std::size_t size = total_size(result->packets);
      CHECK(size > previous_size);
      previous_size = size;
    }
  }
}

TEST_CASE("codec_hevc/encode: smooth content at the default quality is visually lossless") {
  std::vector<ir::VideoFrame> frames;
  for (std::uint32_t i = 0; i < 12; ++i) frames.push_back(test::test_pattern_frame(ir::PixelFormat::I420, 128, 64, i));
  const auto result = round_trip(params_for(128, 64, 90), frames);
  REQUIRE_OK(result);
  CHECK(test::min_psnr(frames, result->decoded) >= 44.0);
}

TEST_CASE("codec_hevc/encode: extradata is a valid hvcC and packets are clean samples") {
  const auto frames = busy(96, 64, 10);
  const auto result = round_trip(params_for(96, 64, 80), frames);
  REQUIRE_OK(result);
  const auto config = hevc::parse_hvcc(result->extradata);
  REQUIRE_OK(config);
  CHECK(config->length_size == 4);
  CHECK(config->ptl.profile_idc == 1);  // Main
  CHECK(config->chroma_format_idc == 1);
  CHECK(config->bit_depth_luma == 8);
  std::set<std::uint8_t> types;
  for (const Bytes& nal : config->nals) {
    types.insert(hevc::nal_type(nal));
    if (hevc::nal_type(nal) == 33) {
      const auto sps = hevc::parse_sps(nal);
      REQUIRE_OK(sps);
      CHECK(sps->width == 96);
      CHECK(sps->height == 64);
      CHECK(sps->matrix_coefficients == 1);  // BT.709 signalled
      CHECK(!sps->video_full_range);
    }
  }
  CHECK(types == std::set<std::uint8_t>{32, 33, 34});
  for (const ir::Packet& packet : result->packets) {
    const auto nals = hevc::split_length_prefixed(packet.bytes, 4);
    REQUIRE_OK(nals);
    REQUIRE(!nals->empty());
    for (const ByteView nal : *nals) CHECK(!hevc::is_parameter_set(hevc::nal_type(nal)));  // only in hvcC
    // No encoder-information SEI or other identifying text in the output.
    CHECK(test::str(packet.bytes).find("Kvazaar") == std::string::npos);
  }
}

TEST_CASE("codec_hevc/encode: timestamps, decode order and keyframe interval") {
  const auto frames = busy(96, 64, 40);
  for (const auto preset : {codec::EncodePreset::Fast, codec::EncodePreset::Balanced}) {
    INFO("preset ", static_cast<int>(preset));
    auto params = params_for(96, 64, 70);
    params.preset = preset;
    params.keyframe_interval = 17;
    const auto result = round_trip(params, frames);
    REQUIRE_OK(result);
    REQUIRE(result->packets.size() == frames.size());
    std::set<std::int64_t> pts;
    std::vector<std::int64_t> keyframes;
    for (std::size_t i = 0; i < result->packets.size(); ++i) {
      const ir::Packet& packet = result->packets[i];
      INFO("packet ", i);
      CHECK(packet.dts <= packet.pts);
      if (i > 0) CHECK(packet.dts > result->packets[i - 1].dts);
      CHECK(packet.duration == 3600);
      pts.insert(packet.pts);
      if (packet.keyframe) keyframes.push_back(packet.pts / 3600);
    }
    // Every input timestamp comes back exactly once.
    std::set<std::int64_t> expected;
    for (std::int64_t i = 0; i < 40; ++i) expected.insert(i * 3600);
    CHECK(pts == expected);
    CHECK(keyframes == std::vector<std::int64_t>{0, 17, 34});
    CHECK(result->packets.front().keyframe);
    // Decoded frames come out in presentation order with the input times.
    REQUIRE(result->decoded.size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) CHECK(result->decoded[i].pts_ms == frames[i].pts_ms);
  }
}

TEST_CASE("codec_hevc/encode: keyframe interval 1 gives intra-only video") {
  auto params = params_for(64, 64, 60);
  params.keyframe_interval = 1;
  const auto result = round_trip(params, busy(64, 64, 4));
  REQUIRE_OK(result);
  REQUIRE(result->packets.size() == 4);
  for (const ir::Packet& packet : result->packets) {
    CHECK(packet.keyframe);
    CHECK(packet.dts == packet.pts);
  }
}

TEST_CASE("codec_hevc/encode: bitrate mode follows the target") {
  const auto frames = busy(160, 96, 25);  // one second
  auto low = params_for(160, 96, 90);
  low.bitrate_kbps = 60;
  auto high = params_for(160, 96, 90);
  high.bitrate_kbps = 240;
  const auto a = round_trip(low, frames);
  const auto b = round_trip(high, frames);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  const double kbps_low = static_cast<double>(total_size(a->packets)) * 8.0 / 1000.0;
  const double kbps_high = static_cast<double>(total_size(b->packets)) * 8.0 / 1000.0;
  CAPTURE(kbps_low);
  CAPTURE(kbps_high);
  CHECK(kbps_low < kbps_high);
  CHECK(kbps_low < 60.0 * 1.6);
  CHECK(kbps_high > 240.0 * 0.5);
  CHECK(kbps_high < 240.0 * 1.6);
  CHECK(test::min_psnr(frames, b->decoded) > test::min_psnr(frames, a->decoded));
}

TEST_CASE("codec_hevc/encode: other pixel formats and odd sizes") {
  SUBCASE("I444P10, RGBA and NV12 input are converted") {
    for (const auto format : {ir::PixelFormat::I444P10, ir::PixelFormat::RGBA8, ir::PixelFormat::NV12}) {
      INFO("format ", static_cast<int>(format));
      std::vector<ir::VideoFrame> frames;
      for (std::uint32_t i = 0; i < 4; ++i) frames.push_back(test::test_pattern_frame(format, 64, 48, i));
      const auto result = round_trip(params_for(64, 48, 90), frames);
      REQUIRE_OK(result);
      REQUIRE(result->decoded.size() == 4);
      for (std::size_t i = 0; i < 4; ++i) {
        const auto reference = codec::convert_frame(frames[i], ir::PixelFormat::I420, {});
        REQUIRE_OK(reference);
        CHECK(test::psnr(*reference, result->decoded[i]) >= 38.0);
      }
    }
  }
  SUBCASE("odd dimensions are padded to even") {
    std::vector<ir::VideoFrame> frames;
    for (std::uint32_t i = 0; i < 3; ++i) frames.push_back(test::test_pattern_frame(ir::PixelFormat::I420, 63, 45, i));
    const auto result = round_trip(params_for(63, 45, 90), frames);
    REQUIRE_OK(result);
    REQUIRE(result->decoded.size() == 3);
    CHECK(result->decoded[0].width == 64);
    CHECK(result->decoded[0].height == 46);
  }
}

TEST_CASE("codec_hevc/encode: invalid use is an error") {
  auto encoder = Registry::instance().media().make_video_encoder(codec::id::hevc);
  REQUIRE(encoder != nullptr);
  CHECK_STATUS(encoder->send_frame(test::test_pattern_frame(ir::PixelFormat::I420, 16, 16, 0)),
               Status::InvalidArgument);
  CHECK_STATUS(encoder->configure(params_for(0, 16, 90)), Status::InvalidArgument);
  CHECK_STATUS(encoder->configure(params_for(20000, 16, 90)), Status::InvalidArgument);
  REQUIRE_OK(encoder->configure(params_for(32, 32, 90)));
  CHECK_STATUS(encoder->send_frame(test::test_pattern_frame(ir::PixelFormat::I420, 16, 16, 0)),
               Status::InvalidArgument);
}

TEST_CASE("codec_hevc/encode: quality maps monotonically to QP") {
  int previous = 99;
  for (int q = 1; q <= 100; ++q) {
    const int qp = hevc::quality_to_qp(q);
    CHECK(qp <= previous);
    CHECK(qp >= 0);
    CHECK(qp <= 51);
    previous = qp;
  }
  CHECK(hevc::quality_to_qp(90) == 23);
}

TEST_CASE("codec_hevc/encode: single intra picture for HEIF") {
  const ir::VideoFrame frame = test::test_pattern_frame(ir::PixelFormat::I420, 96, 64, 3);
  hevc::PictureEncodeOptions options;
  options.quality = 90;
  const auto encoded = hevc::encode_picture(frame, options);
  REQUIRE_OK(encoded);
  CHECK(encoded->width == 96);
  CHECK(encoded->height == 64);
  const auto nals = hevc::split_length_prefixed(encoded->sample, 4);
  REQUIRE_OK(nals);
  REQUIRE(!nals->empty());
  CHECK(hevc::is_idr(hevc::nal_type(nals->front())));
  const auto decoded = hevc::decode_picture(encoded->hvcc, encoded->sample);
  REQUIRE_OK(decoded);
  CHECK(test::psnr(frame, *decoded) >= 42.0);
}
