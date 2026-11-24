// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Decoding real HEVC streams written by x265, Kvazaar and Apple VideoToolbox,
// compared bit-exactly with ffmpeg's decoder (per-frame MD5s in the fixtures).
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "families/codec_hevc/hevc_test_util.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "families/codec_hevc/hevc_api.hpp"

using namespace cvt;
using namespace cvt::test::hevc_util;

namespace {

struct StreamCase {
  const char* name;
  std::uint32_t width;
  std::uint32_t height;
  ir::PixelFormat format;
  bool luma_only;  // 4:0:0: ffmpeg hashes the Y plane only
};

constexpr StreamCase stream_cases[] = {
    {"main_176x144_ctu64_bframes", 176, 144, ir::PixelFormat::I420, false},
    {"main_64x64_ctu16_opengop", 64, 64, ir::PixelFormat::I420, false},
    {"main_128x96_ctu32_wpp", 128, 96, ir::PixelFormat::I420, false},
    {"main_128x96_scaling_weighted", 128, 96, ir::PixelFormat::I420, false},
    {"main_128x96_lossless", 128, 96, ir::PixelFormat::I420, false},
    {"main_98x66_cropped", 98, 66, ir::PixelFormat::I420, false},
    {"main10_96x64", 96, 64, ir::PixelFormat::I420P10, false},
    {"mainstill_96x64", 96, 64, ir::PixelFormat::I420, false},
    {"rext444_98x66", 98, 66, ir::PixelFormat::I444, false},
    {"rext422p10_96x64", 96, 64, ir::PixelFormat::I422P10, false},
    {"rext400_64x64", 64, 64, ir::PixelFormat::I420, true},
    {"kvz_160x128_tiles", 160, 128, ir::PixelFormat::I420, false},
};

}  // namespace

TEST_CASE("codec_hevc/decode: real streams match ffmpeg bit-exactly") {
  for (const StreamCase& c : stream_cases) {
    INFO("fixture ", c.name);
    const Bytes data = test::fixture(std::string("codec_hevc/") + c.name + ".h265");
    const std::vector<std::string> expected = reference_md5(c.name);
    REQUIRE(!expected.empty());
    const auto frames = decode_stream(data);
    REQUIRE_OK(frames);
    REQUIRE(frames->size() == expected.size());
    std::int64_t previous_pts = -1;
    for (std::size_t i = 0; i < frames->size(); ++i) {
      const ir::VideoFrame& frame = (*frames)[i];
      INFO("frame ", i);
      CHECK(frame.valid());
      CHECK(frame.width == c.width);
      CHECK(frame.height == c.height);
      CHECK(frame.format == c.format);
      CHECK(frame_md5(frame, c.luma_only) == expected[i]);
      CHECK(frame.pts_ms > previous_pts);  // presentation order
      CHECK(frame.duration_ms == 40);      // 25 fps
      previous_pts = frame.pts_ms;
    }
  }
}

TEST_CASE("codec_hevc/decode: monochrome pictures get neutral chroma") {
  const auto frames = decode_stream(test::fixture("codec_hevc/rext400_64x64.h265"));
  REQUIRE_OK(frames);
  REQUIRE(!frames->empty());
  const ir::VideoFrame& frame = frames->front();
  CHECK(std::all_of(frame.planes[1].data.begin(), frame.planes[1].data.end(), [](std::uint8_t v) { return v == 128; }));
  CHECK(std::all_of(frame.planes[2].data.begin(), frame.planes[2].data.end(), [](std::uint8_t v) { return v == 128; }));
}

TEST_CASE("codec_hevc/decode: VideoToolbox samples with hvcC, in presentation order") {
  struct VtCase {
    const char* name;
    std::uint32_t width;
    std::uint32_t height;
    ir::PixelFormat format;
    std::uint8_t profile;
  };
  for (const VtCase c : {VtCase{"vt_main10_176x144", 176, 144, ir::PixelFormat::I420P10, 2},
                         VtCase{"vt_main_160x96", 160, 96, ir::PixelFormat::I420, 1}}) {
    INFO("fixture ", c.name);
    const auto file = read_sample_file(test::fixture(std::string("codec_hevc/") + c.name + ".hvcs"));
    REQUIRE(file.has_value());
    const auto config = hevc::parse_hvcc(file->hvcc);
    REQUIRE_OK(config);
    CHECK(config->ptl.profile_idc == c.profile);
    CHECK(config->length_size == 4);

    codec::CodecParams params;
    params.codec = "hevc";
    params.extradata = file->hvcc;
    params.timescale = 12800;  // the movie's media timescale
    std::vector<ir::Packet> packets;
    for (const Sample& sample : file->samples) {
      ir::Packet packet;
      packet.bytes = sample.bytes;
      packet.pts = sample.pts;
      packet.dts = sample.dts;
      packet.duration = 512;
      packets.push_back(std::move(packet));
    }
    const auto frames = decode_packets(params, packets);
    REQUIRE_OK(frames);
    const std::vector<std::string> expected = reference_md5(c.name);
    REQUIRE(frames->size() == expected.size());
    for (std::size_t i = 0; i < frames->size(); ++i) {
      INFO("frame ", i);
      CHECK((*frames)[i].format == c.format);
      CHECK((*frames)[i].width == c.width);
      CHECK((*frames)[i].height == c.height);
      CHECK(frame_md5((*frames)[i]) == expected[i]);
      CHECK((*frames)[i].pts_ms == static_cast<std::int64_t>(i) * 40);
      CHECK((*frames)[i].duration_ms == 40);
    }
  }
}

TEST_CASE("codec_hevc/decode: Annex-B packets need no extradata") {
  // The VideoToolbox samples converted to Annex-B with the parameter sets
  // in front of the first one, as an MPEG-TS demuxer would deliver them.
  const auto file = read_sample_file(test::fixture("codec_hevc/vt_main_160x96.hvcs"));
  REQUIRE(file.has_value());
  const auto config = hevc::parse_hvcc(file->hvcc);
  REQUIRE_OK(config);
  std::vector<ir::Packet> packets;
  for (std::size_t i = 0; i < file->samples.size(); ++i) {
    ir::Packet packet;
    auto annexb = hevc::length_prefixed_to_annexb(file->samples[i].bytes, 4);
    REQUIRE_OK(annexb);
    if (i == 0) {
      std::vector<ByteView> sets;
      for (const Bytes& nal : config->nals) sets.emplace_back(nal);
      packet.bytes = hevc::join_annexb(sets);
    }
    packet.bytes.insert(packet.bytes.end(), annexb->begin(), annexb->end());
    packet.pts = file->samples[i].pts;
    packets.push_back(std::move(packet));
  }
  codec::CodecParams params;
  params.codec = "hevc";
  params.timescale = 12800;
  const auto frames = decode_packets(params, packets);
  REQUIRE_OK(frames);
  const std::vector<std::string> expected = reference_md5("vt_main_160x96");
  REQUIRE(frames->size() == expected.size());
  for (std::size_t i = 0; i < frames->size(); ++i) CHECK(frame_md5((*frames)[i]) == expected[i]);
}

TEST_CASE("codec_hevc/decode: single picture entry point for HEIF items") {
  const Bytes still = test::fixture("codec_hevc/mainstill_96x64.h265");
  const std::vector<std::string> expected = reference_md5("mainstill_96x64");
  REQUIRE(expected.size() == 1);

  SUBCASE("Annex-B with in-band parameter sets") {
    const auto frame = hevc::decode_picture({}, still);
    REQUIRE_OK(frame);
    CHECK(frame->width == 96);
    CHECK(frame->height == 64);
    CHECK(frame_md5(*frame) == expected[0]);
  }
  SUBCASE("hvcC plus length-prefixed item data, as stored in HEIC") {
    std::vector<ByteView> sets;
    std::vector<ByteView> picture;
    for (const ByteView nal : hevc::split_annexb(still)) {
      (hevc::is_parameter_set(hevc::nal_type(nal)) ? sets : picture).push_back(nal);
    }
    const auto hvcc = hevc::build_hvcc(sets);
    REQUIRE_OK(hvcc);
    const auto item = hevc::join_length_prefixed(picture, 4);
    REQUIRE_OK(item);
    const auto frame = hevc::decode_picture(*hvcc, *item);
    REQUIRE_OK(frame);
    CHECK(frame_md5(*frame) == expected[0]);
    // Main Still Picture profile, as HEIC requires.
    const auto config = hevc::parse_hvcc(*hvcc);
    REQUIRE_OK(config);
    CHECK(config->ptl.profile_idc == 3);
  }
  SUBCASE("size limit") {
    CHECK_STATUS(hevc::decode_picture({}, still, 1000), Status::Limit);
  }
}
