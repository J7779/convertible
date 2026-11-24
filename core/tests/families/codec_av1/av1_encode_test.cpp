// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Encoder round trips: our encoder -> our decoder, with PSNR floors per
// quality level and size ordering. Independent decoding of the encoder's
// output (dav1d, ffmpeg) was verified while developing; see
// core/tests/data/codec_av1/SOURCES.md.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "codec/colorspace.hpp"
#include "families/codec_av1/av1_api.hpp"
#include "families/codec_av1/av1_test_support.hpp"
#include "families/codec_av1/obu.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::av1test;

namespace {

std::vector<ir::VideoFrame> pattern(ir::PixelFormat format, std::uint32_t w, std::uint32_t h, std::uint32_t count) {
  std::vector<ir::VideoFrame> frames;
  for (std::uint32_t i = 0; i < count; ++i) frames.push_back(test::test_pattern_frame(format, w, h, i));
  return frames;
}

/// The test pattern with deterministic texture (8-bit formats), so that the
/// quality levels make a visible difference.
std::vector<ir::VideoFrame> textured(std::uint32_t w, std::uint32_t h, std::uint32_t count) {
  std::vector<ir::VideoFrame> frames = pattern(ir::PixelFormat::I420, w, h, count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const Bytes noise = test::random_bytes(std::size_t{w} * h, 77);
    ir::Plane& luma = frames[i].planes[0];
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        // A texture that scrolls with the pattern: sample the noise at x + 2i.
        const std::size_t at = std::size_t{y} * w + (x + 2 * i) % w;
        const int value = luma.data[y * luma.stride + x] + (std::to_integer<int>(noise[at]) % 41) - 20;
        luma.data[y * luma.stride + x] = static_cast<std::uint8_t>(std::clamp(value, 16, 235));
      }
    }
  }
  return frames;
}

codec::VideoEncodeParams params_for(std::uint32_t w, std::uint32_t h, int quality,
                                    ir::PixelFormat format = ir::PixelFormat::I420) {
  codec::VideoEncodeParams params;
  params.width = w;
  params.height = h;
  params.fps = {25, 1};
  params.quality = quality;
  params.pixel_format = format;
  params.timescale = 90000;
  return params;
}

}  // namespace

TEST_CASE("codec_av1: encoder is registered next to the decoder") {
  CHECK(encoder() != nullptr);
  CHECK(decoder() != nullptr);
  CHECK(Registry::instance().media().problems().empty());
}

TEST_CASE("codec_av1: encode -> decode round trip per quality level") {
  const auto source = textured(64, 48, 10);
  struct Level {
    int quality;
    double min_psnr;
  };
  constexpr Level levels[] = {{20, 26.0}, {50, 31.0}, {75, 36.0}, {90, 42.0}};
  std::size_t previous_size = 0;
  for (const Level& level : levels) {
    INFO("quality ", level.quality);
    const auto encoded = encode_frames(source, params_for(64, 48, level.quality));
    REQUIRE_OK(encoded);
    REQUIRE(encoded->packets.size() == source.size());
    const auto decoded = decode_packets(encoded->packets, 90000, encoded->extradata);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->size() == source.size());
    const double worst = test::min_psnr(source, *decoded);
    MESSAGE("AV1 q", level.quality, ": ", encoded->total_bytes, " bytes, worst frame ", worst, " dB");
    CHECK(worst >= level.min_psnr);
    CHECK(encoded->total_bytes > previous_size);  // higher quality, larger output
    previous_size = encoded->total_bytes;
    for (std::size_t i = 0; i < decoded->size(); ++i) {
      CHECK((*decoded)[i].pts_ms == source[i].pts_ms);
      CHECK((*decoded)[i].duration_ms == 40);
    }
  }
}

TEST_CASE("codec_av1: encoder packets, timestamps and av1C") {
  const auto source = pattern(ir::PixelFormat::I420, 48, 32, 8);
  const auto encoded = encode_frames(source, params_for(48, 32, 60));
  REQUIRE_OK(encoded);
  REQUIRE(encoded->packets.size() == source.size());
  CHECK(encoded->packets.front().keyframe);
  const auto config = av1::parse_av1c(encoded->extradata);
  REQUIRE_OK(config);
  CHECK(config->profile == 0);
  CHECK_FALSE(config->high_bitdepth);
  CHECK(config->subsampling_x == 1);
  CHECK(config->subsampling_y == 1);
  const auto in_band = av1::find_sequence_header_obu(encoded->packets.front().bytes);
  REQUIRE_OK(in_band);
  REQUIRE(in_band->has_value());
  CHECK(config->config_obus == **in_band);
  for (std::size_t i = 0; i < encoded->packets.size(); ++i) {
    const ir::Packet& packet = encoded->packets[i];
    CHECK(packet.pts == static_cast<std::int64_t>(i) * 3600);  // 40 ms at 90 kHz
    CHECK(packet.dts == packet.pts);
    CHECK(packet.duration == 3600);
    // MP4/Matroska form: no temporal delimiters.
    const auto obus = av1::split_section5(packet.bytes);
    REQUIRE_OK(obus);
    for (const av1::Obu& obu : *obus) CHECK(obu.type != av1::ObuType::TemporalDelimiter);
  }
  // IVF/.obu form with temporal delimiters decodes the same.
  std::vector<ir::Packet> with_td = encoded->packets;
  for (ir::Packet& packet : with_td) packet.bytes = av1::with_temporal_delimiter(packet.bytes);
  const auto a = decode_packets(encoded->packets, 90000);
  const auto b = decode_packets(with_td, 90000);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  REQUIRE(a->size() == b->size());
  for (std::size_t i = 0; i < a->size(); ++i) CHECK(frame_md5((*a)[i]) == frame_md5((*b)[i]));
}

TEST_CASE("codec_av1: encoder output is deterministic") {
  const auto source = pattern(ir::PixelFormat::I420, 32, 32, 4);
  const auto first = encode_frames(source, params_for(32, 32, 70));
  const auto second = encode_frames(source, params_for(32, 32, 70));
  REQUIRE_OK(first);
  REQUIRE_OK(second);
  REQUIRE(first->packets.size() == second->packets.size());
  for (std::size_t i = 0; i < first->packets.size(); ++i) CHECK(first->packets[i].bytes == second->packets[i].bytes);
}

TEST_CASE("codec_av1: 10-bit, 4:4:4 and 4:2:2 encoding") {
  struct Case {
    ir::PixelFormat format;
    std::uint8_t profile;
  };
  constexpr Case cases[] = {{ir::PixelFormat::I420P10, 0},
                            {ir::PixelFormat::I444, 1},
                            {ir::PixelFormat::I422P10, 2},
                            {ir::PixelFormat::I444P10, 1}};
  for (const Case& c : cases) {
    INFO(static_cast<int>(c.format));
    const auto source = pattern(c.format, 40, 24, 4);
    const auto encoded = encode_frames(source, params_for(40, 24, 80, c.format));
    REQUIRE_OK(encoded);
    const auto config = av1::parse_av1c(encoded->extradata);
    REQUIRE_OK(config);
    CHECK(config->profile == c.profile);
    CHECK(config->high_bitdepth == (ir::bit_depth(c.format) == 10));
    const auto decoded = decode_packets(encoded->packets, 90000);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->size() == source.size());
    CHECK(decoded->front().format == c.format);
    CHECK(test::min_psnr(source, *decoded) >= 40.0);
  }
}

TEST_CASE("codec_av1: other input layouts are converted") {
  // RGBA8 and NV12 input with an I420 / NV12 configuration: converted
  // through codec/colorspace.hpp before encoding.
  for (const ir::PixelFormat input : {ir::PixelFormat::RGBA8, ir::PixelFormat::NV12, ir::PixelFormat::P010}) {
    INFO(static_cast<int>(input));
    const auto source = pattern(input, 32, 32, 3);
    const auto encoded = encode_frames(source, params_for(32, 32, 85, input));
    REQUIRE_OK(encoded);
    const auto decoded = decode_packets(encoded->packets, 90000);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->size() == 3);
    const ir::PixelFormat coded = input == ir::PixelFormat::P010 ? ir::PixelFormat::I420P10 : ir::PixelFormat::I420;
    CHECK(decoded->front().format == coded);
    const auto expected = codec::convert_frame(source[1], coded, source[1].color);
    REQUIRE_OK(expected);
    CHECK(test::psnr(*expected, (*decoded)[1]) >= 38.0);
  }
}

TEST_CASE("codec_av1: bitrate, keyframe interval and presets") {
  const auto source = pattern(ir::PixelFormat::I420, 64, 64, 12);
  SUBCASE("lower bitrate gives smaller output") {
    auto low = params_for(64, 64, 90);
    low.bitrate_kbps = 40;
    auto high = params_for(64, 64, 90);
    high.bitrate_kbps = 800;
    const auto a = encode_frames(source, low);
    const auto b = encode_frames(source, high);
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    CHECK(a->total_bytes < b->total_bytes);
  }
  SUBCASE("keyframe interval") {
    auto params = params_for(64, 64, 60);
    params.keyframe_interval = 4;
    const auto encoded = encode_frames(source, params);
    REQUIRE_OK(encoded);
    int keyframes = 0;
    int gap = 0;
    int longest_gap = 0;
    for (const ir::Packet& packet : encoded->packets) {
      if (packet.keyframe) {
        ++keyframes;
        gap = 0;
      } else {
        longest_gap = std::max(longest_gap, ++gap);
      }
    }
    CHECK(keyframes >= 3);
    CHECK(longest_gap < 4);
    const auto decoded = decode_packets(encoded->packets, 90000);
    REQUIRE_OK(decoded);
    CHECK(decoded->size() == source.size());
  }
  SUBCASE("every preset decodes") {
    for (const codec::EncodePreset preset :
         {codec::EncodePreset::Fast, codec::EncodePreset::Balanced, codec::EncodePreset::Small}) {
      auto params = params_for(64, 64, 75);
      params.preset = preset;
      const auto encoded = encode_frames(source, params);
      REQUIRE_OK(encoded);
      const auto decoded = decode_packets(encoded->packets, 90000);
      REQUIRE_OK(decoded);
      CHECK(test::min_psnr(source, *decoded) >= 38.0);
    }
  }
}

TEST_CASE("codec_av1: encoder rejects bad configurations and frames") {
  auto enc = encoder();
  REQUIRE(enc != nullptr);
  CHECK_STATUS(enc->configure(params_for(0, 16, 50)), Status::InvalidArgument);
  auto enc2 = encoder();
  REQUIRE_OK(enc2->configure(params_for(32, 32, 50)));
  CHECK_STATUS(enc2->send_frame(test::test_pattern_frame(ir::PixelFormat::I420, 16, 16, 0)), Status::InvalidArgument);
  ir::VideoFrame broken = test::test_pattern_frame(ir::PixelFormat::I420, 32, 32, 0);
  broken.planes[2].data.resize(3);
  CHECK_STATUS(enc2->send_frame(broken), Status::InvalidArgument);
  // Timestamps that do not increase are made monotonic rather than refused.
  auto enc3 = encoder();
  REQUIRE_OK(enc3->configure(params_for(32, 32, 50)));
  ir::VideoFrame frame = test::test_pattern_frame(ir::PixelFormat::I420, 32, 32, 0);
  REQUIRE_OK(enc3->send_frame(frame));
  REQUIRE_OK(enc3->send_frame(frame));
  REQUIRE_OK(enc3->flush());
  std::vector<std::int64_t> pts;
  while (true) {
    auto packet = enc3->receive_packet();
    REQUIRE_OK(packet);
    if (!*packet) break;
    pts.push_back((*packet)->pts);
  }
  REQUIRE(pts.size() == 2);
  CHECK(pts[1] > pts[0]);
}

TEST_CASE("codec_av1: still pictures for AVIF") {
  const ir::VideoFrame picture = test::test_pattern_frame(ir::PixelFormat::I420, 61, 37, 3);
  SUBCASE("quality levels") {
    std::size_t previous = 0;
    for (const int quality : {30, 60, 90}) {
      INFO("quality ", quality);
      av1::StillEncodeOptions options;
      options.quality = quality;
      options.color = {1, 13, 6, true, 0};
      const auto still = av1::encode_still(picture, options);
      REQUIRE_OK(still);
      CHECK(still->sequence.still_picture);
      CHECK(still->sequence.reduced_still_picture_header);
      CHECK(still->sequence.matrix_coefficients == 6);
      CHECK(still->sequence.full_range);
      CHECK(still->obus.size() > previous);
      previous = still->obus.size();
      const auto config = av1::parse_av1c(still->av1c);
      REQUIRE_OK(config);
      CHECK(config->profile == 0);
      const auto decoded = av1::decode_still(still->obus);
      REQUIRE_OK(decoded);
      CHECK(decoded->color.matrix_coefficients == 6);
      CHECK(decoded->color.transfer_characteristics == 13);
      CHECK(decoded->frame.color.range == ir::ColorRange::Full);
      const double floor = quality >= 90 ? 42.0 : quality >= 60 ? 36.0 : 30.0;
      CHECK(test::psnr(picture, decoded->frame) >= floor);
    }
  }
  SUBCASE("lossless, 8 and 10 bit, 4:2:0 and 4:4:4") {
    for (const ir::PixelFormat format : {ir::PixelFormat::I420, ir::PixelFormat::I444, ir::PixelFormat::I420P10}) {
      INFO(static_cast<int>(format));
      const ir::VideoFrame source = test::test_pattern_frame(format, 33, 17, 1);
      av1::StillEncodeOptions options;
      options.lossless = true;
      const auto still = av1::encode_still(source, options);
      REQUIRE_OK(still);
      const auto decoded = av1::decode_still(still->obus, still->av1c);
      REQUIRE_OK(decoded);
      CHECK(frame_md5(decoded->frame) == frame_md5(source));
    }
  }
  SUBCASE("lossless RGB through the identity matrix") {
    const ir::VideoFrame rgba = test::test_pattern_frame(ir::PixelFormat::RGBA8, 21, 13, 2);
    av1::StillEncodeOptions options;
    options.lossless = true;
    options.color = {1, 13, 0, true, 0};
    const auto still = av1::encode_still(rgba, options);
    REQUIRE_OK(still);
    CHECK(still->sequence.profile == 1);
    const auto decoded = av1::decode_still(still->obus);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->frame.format == ir::PixelFormat::I444);
    CHECK(decoded->color.matrix_coefficients == 0);
    // Planes hold G, B, R exactly.
    bool exact = true;
    for (std::uint32_t y = 0; y < rgba.height; ++y) {
      for (std::uint32_t x = 0; x < rgba.width; ++x) {
        const std::uint8_t* px = rgba.planes[0].data.data() + rgba.planes[0].stride * y + 4 * x;
        const auto at = [&](std::size_t p) { return decoded->frame.planes[p].data[decoded->frame.planes[p].stride * y + x]; };
        exact = exact && at(0) == px[1] && at(1) == px[2] && at(2) == px[0];
      }
    }
    CHECK(exact);
  }
  SUBCASE("monochrome alpha plane") {
    ir::VideoFrame alpha = ir::VideoFrame::allocate(ir::PixelFormat::I420, 30, 20);
    for (std::uint32_t y = 0; y < 20; ++y) {
      for (std::uint32_t x = 0; x < 30; ++x) alpha.planes[0].data[y * alpha.planes[0].stride + x] = static_cast<std::uint8_t>(x * 8);
    }
    av1::StillEncodeOptions options;
    options.monochrome = true;
    options.lossless = true;
    options.color.full_range = true;
    const auto still = av1::encode_still(alpha, options);
    REQUIRE_OK(still);
    CHECK(still->sequence.monochrome);
    const auto config = av1::parse_av1c(still->av1c);
    REQUIRE_OK(config);
    CHECK(config->monochrome);
    const auto decoded = av1::decode_still(still->obus);
    REQUIRE_OK(decoded);
    CHECK(decoded->monochrome);
    CHECK(decoded->color.full_range);
    CHECK(decoded->frame.planes[0].data == alpha.planes[0].data);
    options.lossless = false;
    options.quality = 70;
    const auto lossy = av1::encode_still(alpha, options);
    REQUIRE_OK(lossy);
    CHECK(lossy->obus.size() < still->obus.size());
  }
  SUBCASE("invalid input") {
    ir::VideoFrame broken = picture;
    broken.planes.pop_back();
    CHECK_STATUS(av1::encode_still(broken), Status::InvalidArgument);
  }
}

TEST_CASE("codec_av1: quality scale is monotonic") {
  int previous = 64;
  for (int q = 1; q <= 100; ++q) {
    const int cq = av1::quality_to_cq_level(q);
    CHECK(cq <= previous);
    CHECK(cq >= 0);
    previous = cq;
  }
  CHECK(av1::quality_to_cq_level(90) == 20);
}
