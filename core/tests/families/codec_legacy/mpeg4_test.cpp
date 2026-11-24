// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <limits>
#include <string>

#include "families/codec_legacy/legacy_test_support.hpp"
#include "support/test.hpp"

using namespace cvt;
namespace tl = cvt::test::legacy;

namespace {

double fixture_psnr(const std::string& name) {
  const tl::PacketFile file = tl::load_packets(name);
  const tl::RefFile ref = tl::load_reference(name);
  auto frames = tl::decode_file(file);
  REQUIRE_OK(frames);
  CHECK(frames->size() == ref.total);
  for (std::size_t i = 1; i < frames->size(); ++i) CHECK((*frames)[i].pts_ms > (*frames)[i - 1].pts_ms);
  const double value = tl::compare_with_reference(*frames, ref);
  INFO(name, " PSNR ", value);
  return value;
}

struct RoundTrip {
  std::size_t bytes = 0;
  double psnr = 0;
};

RoundTrip round_trip(const std::string& codec_id, int quality, std::uint32_t w, std::uint32_t h,
                     codec::EncodePreset preset = codec::EncodePreset::Balanced, std::uint32_t count = 12) {
  const auto frames = tl::textured_frames(ir::PixelFormat::I420, w, h, count);
  codec::VideoEncodeParams params;
  params.width = w;
  params.height = h;
  params.fps = {25, 1};
  params.quality = quality;
  params.preset = preset;
  params.keyframe_interval = 8;
  auto encoded = tl::encode_frames(codec_id, params, frames);
  REQUIRE_OK(encoded);
  REQUIRE(encoded->packets.size() == frames.size());
  CHECK(encoded->packets[0].keyframe);
  CHECK(encoded->packets[8].keyframe);
  CHECK_FALSE(encoded->packets[1].keyframe);
  const std::string dump = codec_id + "_q" + std::to_string(quality);
  tl::dump_for_interop(dump, codec_id, w, h, params.timescale, *encoded);
  tl::dump_raw_for_interop(dump, frames);
  auto decoder = cvt::legacy::make_video_decoder(codec_id);
  codec::CodecParams cp;
  cp.codec = codec_id;
  cp.timescale = params.timescale;
  cp.extradata = encoded->extradata;
  auto decoded = tl::decode_packets(*decoder, cp, encoded->packets);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->size() == frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    CHECK((*decoded)[i].pts_ms == frames[i].pts_ms);
    (*decoded)[i].color = frames[i].color;
  }
  RoundTrip out{encoded->total_bytes(), test::min_psnr(frames, *decoded)};
  INFO(codec_id, " q", quality, ": ", out.bytes, " bytes, min PSNR ", out.psnr);
  return out;
}

}  // namespace

TEST_CASE("codec_legacy/mpeg4: decodes Simple and Advanced Simple Profile streams") {
  // ffmpeg's decode is the reference; our IDCT differs by rounding only.
  // Measured 60.9 / 60.1 / 61.9 / 61.3 dB on the worst stored frame.
  CHECK(fixture_psnr("mpeg4_sp_partitioned_96x64") >= 55.0);  // data partitioning, video packets
  CHECK(fixture_psnr("mpeg4_global_header_96x64") >= 55.0);   // VOL in extradata, MPEG matrices, B-VOPs
  CHECK(fixture_psnr("mpeg4_asp_packed_96x64") >= 55.0);      // quarter sample, 4MV, B-VOPs, DivX packing
  CHECK(fixture_psnr("mpeg4_interlaced_96x64") >= 55.0);      // field DCT and field motion vectors
}

TEST_CASE("codec_legacy/mpeg4: no drift over a 120-frame GOP") {
  CHECK(fixture_psnr("mpeg4_long_gop_48x48") >= 55.0);  // frame 119: 61.4 dB
}

TEST_CASE("codec_legacy/mpeg4: GMC with three warping points is reported, not mis-decoded") {
  const tl::PacketFile file = tl::load_packets("mpeg4_xvid_gmc_96x64");
  CHECK_STATUS(tl::decode_file(file), Status::Decode);
}

TEST_CASE("codec_legacy/h263: decodes baseline H.263, H.263+ and Sorenson Spark") {
  CHECK(fixture_psnr("h263_128x96") >= 55.0);        // 64.7 dB
  CHECK(fixture_psnr("h263p_umv_160x120") >= 55.0);  // PLUSPTYPE custom size, UMV, slices: 64.6 dB
  CHECK(fixture_psnr("flv1_64x64") >= 55.0);         // 61.8 dB
}

TEST_CASE("codec_legacy/mpeg4: extradata and in-band headers are equivalent") {
  tl::PacketFile file = tl::load_packets("mpeg4_global_header_96x64");
  auto with_extradata = tl::decode_file(file);
  REQUIRE_OK(with_extradata);
  // Move the configuration in band: prepend it to the first packet.
  Bytes first = file.extradata;
  first.insert(first.end(), file.packets[0].bytes.begin(), file.packets[0].bytes.end());
  file.packets[0].bytes = first;
  file.extradata.clear();
  auto in_band = tl::decode_file(file);
  REQUIRE_OK(in_band);
  REQUIRE(in_band->size() == with_extradata->size());
  for (std::size_t i = 0; i < in_band->size(); ++i) {
    CHECK(test::psnr((*in_band)[i], (*with_extradata)[i]) == std::numeric_limits<double>::infinity());
  }
}

TEST_CASE("codec_legacy/mpeg4: encoder round trip quality and size") {
  const RoundTrip q95 = round_trip("mpeg4", 95, 96, 64);
  const RoundTrip q90 = round_trip("mpeg4", 90, 96, 64);
  const RoundTrip q60 = round_trip("mpeg4", 60, 96, 64);
  const RoundTrip q30 = round_trip("mpeg4", 30, 96, 64, codec::EncodePreset::Fast);
  CHECK(q95.psnr >= 44.0);
  CHECK(q90.psnr >= 40.0);
  CHECK(q60.psnr >= 36.0);
  CHECK(q30.psnr >= 31.0);
  CHECK(q95.bytes > q90.bytes);
  CHECK(q90.bytes > q60.bytes);
  CHECK(q60.bytes > q30.bytes);
}

TEST_CASE("codec_legacy/h263: encoder round trips (standard size, custom size, Sorenson)") {
  const RoundTrip h_q90 = round_trip("h263", 90, 128, 96);
  const RoundTrip h_q50 = round_trip("h263", 50, 128, 96);
  CHECK(h_q90.psnr >= 39.0);
  CHECK(h_q50.psnr >= 33.0);
  CHECK(h_q90.bytes > h_q50.bytes);
  const RoundTrip custom = round_trip("h263", 90, 96, 72);
  CHECK(custom.psnr >= 39.0);
  const RoundTrip f_q90 = round_trip("flv1", 90, 72, 40);
  const RoundTrip f_q50 = round_trip("flv1", 50, 72, 40);
  CHECK(f_q90.psnr >= 40.0);
  CHECK(f_q50.psnr >= 34.0);
  CHECK(f_q90.bytes > f_q50.bytes);
  codec::VideoEncodeParams bad;
  bad.width = 90;
  bad.height = 70;
  CHECK_STATUS(tl::encode_frames("h263", bad, {}), Status::InvalidArgument);
}

TEST_CASE("codec_legacy/mpeg4: damaged streams never crash") {
  for (const char* name : {"mpeg4_asp_packed_96x64", "mpeg4_sp_partitioned_96x64", "mpeg4_interlaced_96x64",
                           "h263_128x96", "h263p_umv_160x120", "flv1_64x64"}) {
    tl::fuzz_decoder(tl::load_packets(name), 6);
  }
}
