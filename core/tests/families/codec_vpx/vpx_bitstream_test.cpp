// VP8/VP9 header parsing, superframe splitting, vpcC records and levels.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/codec_vpx/vpx_bitstream.hpp"
#include "families/codec_vpx/vpx_test_util.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace std::string_view_literals;

TEST_CASE("codec_vpx: VP8 keyframe headers give size and visibility") {
  const auto ivf = test::vpx::parse_ivf(test::fixture("codec_vpx/vp8_odd_33x19.ivf"));
  REQUIRE(ivf.has_value());
  const auto key = vpx::parse_vp8_header(ivf->packets[0].bytes);
  REQUIRE_OK(key);
  CHECK(key->keyframe);
  CHECK(key->show_frame);
  CHECK(key->width == 33u);
  CHECK(key->height == 19u);
  const auto inter = vpx::parse_vp8_header(ivf->packets[1].bytes);
  REQUIRE_OK(inter);
  CHECK(!inter->keyframe);
  CHECK_STATUS(vpx::parse_vp8_header(ByteView(ivf->packets[0].bytes).first(5)), Status::Decode);
  CHECK_STATUS(vpx::parse_vp8_header(test::bytes("\x00\x00\x00\x00\x00\x00\x01\x00\x01\x00"sv)), Status::Decode);
}

TEST_CASE("codec_vpx: VP9 headers give profile, depth, colour and sizes through references") {
  const auto ivf = test::vpx::parse_ivf(test::fixture("codec_vpx/vp9_p2_10bit_64x64.ivf"));
  REQUIRE(ivf.has_value());
  vpx::Vp9RefSizes refs;
  for (std::size_t i = 0; i < ivf->packets.size(); ++i) {
    INFO("packet ", i);
    const auto frames = vpx::split_vp9_superframe(ivf->packets[i].bytes);
    REQUIRE_OK(frames);
    for (const ByteView frame : *frames) {
      const auto header = vpx::parse_vp9_header(frame, &refs);
      REQUIRE_OK(header);
      CHECK(header->profile == 2);
      CHECK(header->width == 64u);  // inter frames resolve their size through `refs`
      CHECK(header->height == 64u);
      if (i == 0) {
        CHECK(header->keyframe);
        CHECK(header->bit_depth == 10);
        CHECK(header->subsampling_x == 1);
        CHECK(!header->full_range);
      }
    }
  }
  const auto p1 = test::vpx::parse_ivf(test::fixture("codec_vpx/vp9_p1_444_64x48.ivf"));
  REQUIRE(p1.has_value());
  const auto header = vpx::parse_vp9_header(vpx::split_vp9_superframe(p1->packets[0].bytes)->front());
  REQUIRE_OK(header);
  CHECK(header->profile == 1);
  CHECK(header->subsampling_x == 0);
  CHECK(header->subsampling_y == 0);
  CHECK(header->full_range);
  CHECK(header->width == 64u);
  CHECK(header->height == 48u);
  CHECK_STATUS(vpx::parse_vp9_header(test::bytes("\x00"sv)), Status::Decode);
  CHECK_STATUS(vpx::parse_vp9_header(test::bytes("\x82\x49"sv)), Status::Decode);
}

TEST_CASE("codec_vpx: VP9 superframes split into their frames") {
  // Frames of 2 and 3 bytes, then an index with 1-byte sizes: marker 0b11000001.
  const Bytes packet = test::bytes("\xAA\xBB\x11\x22\x33\xC1\x02\x03\xC1"sv);
  const auto frames = vpx::split_vp9_superframe(packet);
  REQUIRE_OK(frames);
  REQUIRE(frames->size() == 2u);
  CHECK((*frames)[0].size() == 2u);
  CHECK((*frames)[1].size() == 3u);
  CHECK(std::to_integer<int>((*frames)[1][0]) == 0x11);
  // No index: one frame. A marker byte without its twin is not an index.
  const auto single = vpx::split_vp9_superframe(test::bytes("\x82\x49\x83\xC1"sv));
  REQUIRE_OK(single);
  CHECK(single->size() == 1u);
}

TEST_CASE("codec_vpx: vpcC round trip and VP9 levels") {
  vpx::VpcConfig config;
  config.profile = 2;
  config.level = 41;
  config.bit_depth = 10;
  config.chroma_subsampling = 1;
  config.full_range = true;
  config.colour_primaries = 9;
  config.transfer_characteristics = 16;
  config.matrix_coefficients = 9;
  const Bytes payload = vpx::make_vpcc(config);
  CHECK(payload == test::bytes("\x01\x00\x00\x00\x02\x29\xA3\x09\x10\x09\x00\x00"sv));
  const auto parsed = vpx::parse_vpcc(payload);
  REQUIRE_OK(parsed);
  CHECK(*parsed == config);
  CHECK_STATUS(vpx::parse_vpcc(ByteView(payload).first(7)), Status::Decode);
  CHECK_STATUS(vpx::parse_vpcc(test::bytes("\x00\x00\x00\x00\x02\x29\xA3\x09\x10\x09\x00\x00"sv)), Status::Decode);

  CHECK(vpx::vp9_level(176, 144, 30) == 10);
  CHECK(vpx::vp9_level(640, 360, 30) == 21);
  CHECK(vpx::vp9_level(1280, 720, 30) == 31);
  CHECK(vpx::vp9_level(1920, 1080, 30) == 40);
  CHECK(vpx::vp9_level(1920, 1080, 60) == 41);
  CHECK(vpx::vp9_level(3840, 2160, 30) == 50);
  CHECK(vpx::vp9_level(1920, 1080, 0) == 40);
  CHECK(vpx::vp9_level(20000, 20000, 30) == 0);
}
