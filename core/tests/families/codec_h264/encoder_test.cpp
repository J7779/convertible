// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// H.264 encoding: quality levels, rate control, keyframes, timestamps and the
// avcC record, verified by decoding the output again.
#include <algorithm>
#include <cmath>
#include <string>

#include "codec/colorspace.hpp"
#include "codec/ids.hpp"
#include "families/codec_h264/h264_test_support.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
namespace th = cvt::test::h264;

TEST_CASE("codec_h264/encoder: quality levels meet PSNR floors and lower quality is smaller") {
  // quality -> minimum PSNR of the worst frame (dB), for each kind of content
  // (measured: synthetic 53.8 / 49.3 / 44.3 / 41.3 / 37.0, natural-like with
  // per-frame grain 50.0 / 40.8 / 40.2 / 38.4 / 33.8).
  struct Level {
    int quality;
    double synthetic_floor;
    double natural_floor;
  };
  constexpr Level levels[] = {{100, 51.0, 47.0}, {90, 46.0, 39.5}, {75, 42.0, 38.5}, {50, 39.0, 36.5}, {20, 35.0, 32.0}};
  for (const bool natural : {false, true}) {
    INFO(natural ? "natural-like content" : "synthetic content");
    const std::vector<codec::VideoFrame> clip = natural ? th::natural_clip(192, 128, 20) : th::synthetic_clip(192, 128, 20);
    std::size_t previous_size = std::numeric_limits<std::size_t>::max();
    for (const Level& level : levels) {
      INFO("quality ", level.quality);
      const auto encoded = th::encode(th::params_for(192, 128, level.quality), clip);
      REQUIRE_OK(encoded);
      CHECK(encoded->packets.size() == clip.size());
      const auto decoded = th::decode(*encoded);
      REQUIRE_OK(decoded);
      REQUIRE(decoded->size() == clip.size());
      const double worst = test::min_psnr(clip, *decoded);
      CHECK(worst >= (natural ? level.natural_floor : level.synthetic_floor));
      CHECK(encoded->bytes < previous_size);
      previous_size = encoded->bytes;
    }
  }
}

TEST_CASE("codec_h264/encoder: avcC extradata, profile, level and VUI") {
  auto params = th::params_for(320, 180, 90);
  params.fps = {30000, 1001};
  params.color = {ir::ColorMatrix::BT709, ir::ColorRange::Full, ir::ChromaSiting::TopLeft};
  const auto encoded = th::encode(params, th::natural_clip(320, 180, 6));
  REQUIRE_OK(encoded);
  const auto config = cvt::h264::parse_avcc(encoded->extradata);
  REQUIRE_OK(config);
  CHECK(config->length_size == 4);
  REQUIRE(config->sps.size() == 1);
  REQUIRE(config->pps.size() == 1);
  const auto sps = cvt::h264::parse_sps(config->sps[0]);
  REQUIRE_OK(sps);
  CHECK(sps->profile_idc == 77);  // Main: CABAC and B-frames
  CHECK(config->profile_idc == 77);
  CHECK(sps->level_idc >= 30);
  CHECK(sps->width == 320);
  CHECK(sps->height == 180);  // 192 coded lines, cropped
  CHECK(sps->coded_height == 192);
  CHECK(sps->video_full_range);
  CHECK(sps->matrix_coefficients == 1);
  CHECK(sps->colour_primaries == 1);
  CHECK(sps->chroma_loc_present);
  CHECK(sps->chroma_sample_loc_type == 2);
  CHECK(sps->frame_rate() == ir::Rational{30000, 1001});
  CHECK(sps->bitstream_restriction);
  CHECK(sps->max_num_reorder_frames == 1);
  // Samples are length-prefixed, without in-band parameter sets.
  for (const codec::Packet& packet : encoded->packets) {
    const auto nals = cvt::h264::split_length_prefixed(packet.bytes, 4);
    REQUIRE_OK(nals);
    for (const ByteView nal : *nals) CHECK(cvt::h264::nal_type(nal) != cvt::h264::nal_sps);
  }
  // The decoder reports the signalled colour interpretation.
  const auto decoded = th::decode(*encoded);
  REQUIRE_OK(decoded);
  REQUIRE(!decoded->empty());
  CHECK(decoded->front().color == params.color);
  CHECK(decoded->front().width == 320);
  CHECK(decoded->front().height == 180);
}

TEST_CASE("codec_h264/encoder: timestamps, B-frames and keyframe interval") {
  const auto clip = th::natural_clip(128, 96, 23);
  for (const codec::EncodePreset preset : {codec::EncodePreset::Fast, codec::EncodePreset::Balanced, codec::EncodePreset::Small}) {
    INFO("preset ", static_cast<int>(preset));
    auto params = th::params_for(128, 96, 80);
    params.preset = preset;
    params.keyframe_interval = 5;
    const auto encoded = th::encode(params, clip);
    REQUIRE_OK(encoded);
    REQUIRE(encoded->packets.size() == clip.size());
    bool reordered = false;
    std::vector<std::int64_t> keyframes;
    for (std::size_t i = 0; i < encoded->packets.size(); ++i) {
      const codec::Packet& packet = encoded->packets[i];
      INFO("packet ", i);
      CHECK(packet.dts <= packet.pts);
      CHECK(packet.duration == 3600);  // 40 ms at 90 kHz
      if (i > 0) {
        CHECK(packet.dts > encoded->packets[i - 1].dts);
        reordered = reordered || packet.pts < encoded->packets[i - 1].pts;
      }
      if (packet.keyframe) keyframes.push_back(packet.pts / 3600);
    }
    CHECK(reordered == (preset != codec::EncodePreset::Fast));
    CHECK(encoded->packets.front().keyframe);
    CHECK(keyframes == std::vector<std::int64_t>{0, 5, 10, 15, 20});
    // Every presentation time appears once.
    std::vector<std::int64_t> pts;
    for (const codec::Packet& packet : encoded->packets) pts.push_back(packet.pts);
    std::ranges::sort(pts);
    for (std::size_t i = 0; i < pts.size(); ++i) CHECK(pts[i] == static_cast<std::int64_t>(i) * 3600);
    // Decoding restores presentation order and timing.
    const auto decoded = th::decode(*encoded);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->size() == clip.size());
    for (std::size_t i = 0; i < decoded->size(); ++i) CHECK((*decoded)[i].pts_ms == static_cast<std::int64_t>(i) * 40);
    CHECK(test::min_psnr(clip, *decoded) >= 36.0);
    // Decoding can start at any keyframe.
    std::vector<codec::Packet> tail;
    for (std::size_t i = 0; i < encoded->packets.size(); ++i) {
      if (encoded->packets[i].keyframe && encoded->packets[i].pts == 10 * 3600) {
        tail.assign(encoded->packets.begin() + static_cast<std::ptrdiff_t>(i), encoded->packets.end());
      }
    }
    th::Encoded partial = *encoded;
    partial.packets = tail;
    const auto resumed = th::decode(partial);
    REQUIRE_OK(resumed);
    CHECK(resumed->size() == 13);
  }
}

TEST_CASE("codec_h264/encoder: a target bit rate is followed") {
  const auto clip = th::natural_clip(256, 192, 25);  // 1 second at 25 fps
  std::vector<std::size_t> sizes;
  for (const std::uint32_t kbps : {150u, 600u}) {
    INFO(kbps, " kbit/s");
    auto params = th::params_for(256, 192, 90);
    params.bitrate_kbps = kbps;
    const auto encoded = th::encode(params, clip);
    REQUIRE_OK(encoded);
    CHECK(encoded->packets.size() == clip.size());
    const double actual_kbps = static_cast<double>(encoded->bytes) * 8.0 / 1000.0;
    CHECK(actual_kbps >= kbps * 0.5);
    CHECK(actual_kbps <= kbps * 1.5);
    sizes.push_back(encoded->bytes);
    const auto decoded = th::decode(*encoded);
    REQUIRE_OK(decoded);
    CHECK(test::min_psnr(clip, *decoded) >= 30.0);
  }
  CHECK(sizes[0] < sizes[1]);
}

TEST_CASE("codec_h264/encoder: RGBA and other pixel formats are converted") {
  const codec::VideoFrame source = test::test_pattern_frame(ir::PixelFormat::RGBA8, 96, 64, 0);
  auto params = th::params_for(96, 64, 95);
  params.color = {ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Left};
  const auto encoded = th::encode(params, {source, source});
  REQUIRE_OK(encoded);
  const auto decoded = th::decode(*encoded);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->size() == 2);
  const auto rgba = codec::to_rgba(decoded->front());
  REQUIRE_OK(rgba);
  CHECK(test::psnr(source, *rgba) >= 35.0);

  const codec::VideoFrame nv12 = test::test_pattern_frame(ir::PixelFormat::NV12, 96, 64, 0);
  const auto from_nv12 = th::encode(th::params_for(96, 64, 90), {nv12});
  REQUIRE_OK(from_nv12);
  CHECK(from_nv12->packets.size() == 1);
}

TEST_CASE("codec_h264/encoder: invalid parameters and calls are refused") {
  const auto configure = [](std::uint32_t width, std::uint32_t height) {
    cvt::h264::H264Encoder encoder;
    return encoder.configure(th::params_for(width, height, 90));
  };
  CHECK_STATUS(configure(0, 64), Status::InvalidArgument);
  CHECK_STATUS(configure(97, 64), Status::InvalidArgument);  // odd width: 4:2:0 needs even sizes
  CHECK_STATUS(configure(96, 63), Status::InvalidArgument);
  CHECK_STATUS(configure(8, 8), Status::InvalidArgument);
  CHECK_STATUS(configure(8192, 64), Status::InvalidArgument);
  CHECK_OK(configure(16, 16));

  cvt::h264::H264Encoder encoder;
  CHECK_STATUS(encoder.send_frame(test::test_pattern_frame(ir::PixelFormat::I420, 64, 64, 0)), Status::InvalidArgument);
  REQUIRE_OK(encoder.configure(th::params_for(64, 64, 90)));
  CHECK_STATUS(encoder.configure(th::params_for(64, 64, 90)), Status::InvalidArgument);
  CHECK_STATUS(encoder.send_frame(test::test_pattern_frame(ir::PixelFormat::I420, 32, 64, 0)), Status::InvalidArgument);
  codec::VideoFrame broken = test::test_pattern_frame(ir::PixelFormat::I420, 64, 64, 0);
  broken.planes[1].data.resize(10);
  CHECK_STATUS(encoder.send_frame(broken), Status::InvalidArgument);
  CHECK_OK(encoder.flush());
  CHECK_STATUS(encoder.send_frame(test::test_pattern_frame(ir::PixelFormat::I420, 64, 64, 0)), Status::InvalidArgument);
}

TEST_CASE("codec_h264/encoder: frames without timestamps get a steady clock") {
  auto clip = th::synthetic_clip(64, 48, 6);
  for (codec::VideoFrame& frame : clip) {
    frame.pts_ms = 0;
    frame.duration_ms = 0;
  }
  const auto encoded = th::encode(th::params_for(64, 48, 90), clip);
  REQUIRE_OK(encoded);
  std::vector<std::int64_t> pts;
  for (const codec::Packet& packet : encoded->packets) pts.push_back(packet.pts);
  std::ranges::sort(pts);
  REQUIRE(pts.size() == 6);
  for (std::size_t i = 0; i < pts.size(); ++i) CHECK(pts[i] == static_cast<std::int64_t>(i) * 3600);
}

TEST_CASE("codec_h264/encoder: quality to quantiser mapping") {
  CHECK(cvt::h264::quality_to_qp(100) == 10);
  CHECK(cvt::h264::quality_to_qp(90) == 21);
  CHECK(cvt::h264::quality_to_qp(75) == 27);
  CHECK(cvt::h264::quality_to_qp(50) == 33);
  CHECK(cvt::h264::quality_to_qp(1) == 46);
  CHECK(cvt::h264::quality_to_qp(0) == 46);
  CHECK(cvt::h264::quality_to_qp(500) == 10);
  for (int q = 2; q <= 100; ++q) CHECK(cvt::h264::quality_to_qp(q) <= cvt::h264::quality_to_qp(q - 1));
}

TEST_CASE("codec_h264/encoder: UHD pictures are accepted") {
  codec::VideoFrame frame = codec::VideoFrame::allocate(ir::PixelFormat::I420, 3840, 2160);
  for (std::size_t p = 0; p < 3; ++p) std::fill(frame.planes[p].data.begin(), frame.planes[p].data.end(), static_cast<std::uint8_t>(p == 0 ? 90 : 128));
  auto params = th::params_for(3840, 2160, 50);
  params.preset = codec::EncodePreset::Fast;
  const auto encoded = th::encode(params, {frame});
  REQUIRE_OK(encoded);
  REQUIRE(encoded->packets.size() == 1);
  const auto sps = cvt::h264::parse_sps(cvt::h264::parse_avcc(encoded->extradata)->sps[0]);
  REQUIRE_OK(sps);
  CHECK(sps->width == 3840);
  CHECK(sps->height == 2160);
  CHECK(sps->level_idc == 51);
  const auto decoded = th::decode(*encoded);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->size() == 1);
  CHECK(test::psnr(frame, decoded->front()) >= 40.0);
}
