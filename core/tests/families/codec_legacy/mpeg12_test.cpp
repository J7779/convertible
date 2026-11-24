// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <limits>
#include <span>
#include <string>

#include "families/codec_legacy/legacy_test_support.hpp"
#include "support/test.hpp"

using namespace cvt;
namespace tl = cvt::test::legacy;

namespace {

/// Decodes a fixture and returns the minimum PSNR against ffmpeg's decode.
double decode_fixture_psnr(const std::string& name, std::size_t* frame_count = nullptr) {
  const tl::PacketFile file = tl::load_packets(name);
  const tl::RefFile ref = tl::load_reference(name);
  auto frames = tl::decode_file(file);
  REQUIRE_OK(frames);
  if (frame_count != nullptr) *frame_count = frames->size();
  CHECK(frames->size() == ref.total);
  for (std::size_t i = 1; i < frames->size(); ++i) CHECK((*frames)[i].pts_ms > (*frames)[i - 1].pts_ms);
  const double value = tl::compare_with_reference(*frames, ref);
  INFO(name, " PSNR ", value);
  return value;
}

}  // namespace

TEST_CASE("codec_legacy/mpeg1video: decodes an ffmpeg stream with B-pictures") {
  CHECK(decode_fixture_psnr("mpeg1_96x64") >= 60.0);  // measured 65.7 dB
}

TEST_CASE("codec_legacy/mpeg2video: decodes progressive, interlaced, 4:2:2 and custom-matrix streams") {
  // Measured 65.9 / 65.2 / 65.8 dB: only IDCT rounding differs from ffmpeg.
  CHECK(decode_fixture_psnr("mpeg2_prog_96x64") >= 60.0);
  CHECK(decode_fixture_psnr("mpeg2_interlaced_96x64") >= 60.0);
  CHECK(decode_fixture_psnr("mpeg2_422_matrix_64x64") >= 60.0);
}

TEST_CASE("codec_legacy/mpeg2video: no drift over a 120-frame GOP") {
  CHECK(decode_fixture_psnr("mpeg2_long_gop_48x48") >= 55.0);  // frame 119 after 119 P pictures: 61.1 dB
}

namespace {

struct RoundTrip {
  std::size_t bytes = 0;
  double psnr = 0;
};

RoundTrip round_trip(std::string_view codec_id, int quality, codec::EncodePreset preset, std::uint32_t w = 96,
                     std::uint32_t h = 64, std::uint32_t count = 14) {
  const auto frames = tl::textured_frames(ir::PixelFormat::I420, w, h, count);
  codec::VideoEncodeParams params;
  params.width = w;
  params.height = h;
  params.fps = {25, 1};
  params.quality = quality;
  params.preset = preset;
  params.color = {ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Left};
  auto encoded = tl::encode_frames(codec_id, params, frames);
  REQUIRE_OK(encoded);
  CHECK(encoded->packets.size() == frames.size());
  CHECK(encoded->packets.front().keyframe);
  const std::string dump_name = std::string(codec_id) + "_q" + std::to_string(quality);
  tl::dump_for_interop(dump_name, codec_id, w, h, params.timescale, *encoded);
  tl::dump_raw_for_interop(dump_name, frames);
  auto decoder = cvt::legacy::make_video_decoder(codec_id);
  codec::CodecParams cp;
  cp.codec = std::string(codec_id);
  cp.timescale = params.timescale;
  cp.extradata = encoded->extradata;
  auto decoded = tl::decode_packets(*decoder, cp, encoded->packets);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->size() == frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    CHECK((*decoded)[i].pts_ms == frames[i].pts_ms);
    (*decoded)[i].color = frames[i].color;
  }
  RoundTrip out;
  out.bytes = encoded->total_bytes();
  out.psnr = test::min_psnr(frames, *decoded);
  INFO(codec_id, " q", quality, ": ", out.bytes, " bytes, min PSNR ", out.psnr);
  return out;
}

}  // namespace

TEST_CASE("codec_legacy/mpeg2video: encoder round trip quality and size") {
  const RoundTrip q95 = round_trip("mpeg2video", 95, codec::EncodePreset::Balanced);
  const RoundTrip q90 = round_trip("mpeg2video", 90, codec::EncodePreset::Balanced);
  const RoundTrip q70 = round_trip("mpeg2video", 70, codec::EncodePreset::Balanced);
  const RoundTrip q40 = round_trip("mpeg2video", 40, codec::EncodePreset::Balanced);
  // Measured: 46.5 / 42.4 / 40.2 / 37.4 dB (worst frame); ffmpeg decodes the
  // same streams at 49.4 / 43.0 / 40.7 / 38.4 dB average (see SOURCES.md).
  CHECK(q95.psnr >= 45.0);
  CHECK(q90.psnr >= 41.0);
  CHECK(q70.psnr >= 38.5);
  CHECK(q40.psnr >= 35.5);
  CHECK(q95.bytes > q90.bytes);
  CHECK(q90.bytes > q70.bytes);
  CHECK(q70.bytes > q40.bytes);
}

TEST_CASE("codec_legacy/mpeg1video: encoder round trip quality and size") {
  const RoundTrip q90 = round_trip("mpeg1video", 90, codec::EncodePreset::Balanced);
  const RoundTrip q60 = round_trip("mpeg1video", 60, codec::EncodePreset::Fast);
  CHECK(q90.psnr >= 41.0);
  CHECK(q60.psnr >= 38.0);
  CHECK(q90.bytes > q60.bytes);
}

TEST_CASE("codec_legacy/mpeg2video: frame metadata, colour and output order") {
  const tl::PacketFile file = tl::load_packets("mpeg2_422_matrix_64x64");
  auto frames = tl::decode_file(file);
  REQUIRE_OK(frames);
  REQUIRE(frames->size() == 8);
  for (std::size_t i = 0; i < frames->size(); ++i) {
    const ir::VideoFrame& f = (*frames)[i];
    CHECK(f.format == ir::PixelFormat::I422);
    CHECK(f.width == 64);
    CHECK(f.color.range == ir::ColorRange::Limited);
    CHECK(f.color.siting == ir::ChromaSiting::Left);
    CHECK(f.pts_ms == static_cast<std::int64_t>(i) * 40);
    CHECK(f.duration_ms == 40);
  }
  const auto mpeg1 = tl::decode_file(tl::load_packets("mpeg1_96x64"));
  REQUIRE_OK(mpeg1);
  CHECK(mpeg1->front().color.siting == ir::ChromaSiting::Center);
  CHECK(mpeg1->front().color.matrix == ir::ColorMatrix::BT601);
}

TEST_CASE("codec_legacy/mpeg2video: elementary stream in arbitrary chunks decodes identically") {
  // A PES-style feed: the whole stream cut into 1000-byte pieces, timestamps
  // only on some packets.
  const tl::PacketFile file = tl::load_packets("mpeg2_interlaced_96x64");
  auto reference = tl::decode_file(file);
  REQUIRE_OK(reference);
  Bytes stream;
  for (const auto& p : file.packets) stream.insert(stream.end(), p.bytes.begin(), p.bytes.end());
  std::vector<ir::Packet> chunks;
  for (std::size_t at = 0; at < stream.size(); at += 1000) {
    ir::Packet p;
    p.bytes.assign(stream.begin() + static_cast<std::ptrdiff_t>(at),
                   stream.begin() + static_cast<std::ptrdiff_t>(std::min(stream.size(), at + 1000)));
    p.pts = at == 0 ? 0 : INT64_MIN;  // "no timestamp" on all but the first piece
    chunks.push_back(std::move(p));
  }
  codec::CodecParams params = file.params();
  auto decoder = cvt::legacy::make_video_decoder("mpeg2video");
  auto frames = tl::decode_packets(*decoder, params, chunks);
  REQUIRE_OK(frames);
  REQUIRE(frames->size() == reference->size());
  for (std::size_t i = 0; i < frames->size(); ++i) {
    CHECK(test::psnr((*frames)[i], (*reference)[i]) == std::numeric_limits<double>::infinity());
    if (i > 0) CHECK((*frames)[i].pts_ms > (*frames)[i - 1].pts_ms);
  }
}

TEST_CASE("codec_legacy/mpeg2video: encoder options, odd sizes and rate control") {
  // Odd size, no B-pictures (Fast), short GOP.
  const auto frames = tl::textured_frames(ir::PixelFormat::I420, 70, 46, 9);
  codec::VideoEncodeParams params;
  params.width = 70;
  params.height = 46;
  params.fps = {30000, 1001};
  params.preset = codec::EncodePreset::Fast;
  params.keyframe_interval = 4;
  auto encoded = tl::encode_frames("mpeg2video", params, frames);
  REQUIRE_OK(encoded);
  REQUIRE(encoded->packets.size() == 9);
  int keyframes = 0;
  for (std::size_t i = 0; i < encoded->packets.size(); ++i) {
    keyframes += encoded->packets[i].keyframe ? 1 : 0;
    CHECK(encoded->packets[i].pts == encoded->packets[i].dts);  // no reordering without B-pictures
  }
  CHECK(keyframes == 3);
  CHECK_FALSE(encoded->extradata.empty());
  auto decoder = cvt::legacy::make_video_decoder("mpeg2video");
  codec::CodecParams cp;
  cp.timescale = params.timescale;
  auto decoded = tl::decode_packets(*decoder, cp, encoded->packets);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->size() == 9);
  CHECK((*decoded)[0].width == 70);
  CHECK((*decoded)[0].height == 46);
  for (auto& f : *decoded) f.color = frames[0].color;
  CHECK(test::min_psnr(frames, *decoded) >= 40.0);

  // Bitrate mode: a lower target gives a smaller stream.
  const auto long_input = tl::textured_frames(ir::PixelFormat::I420, 96, 64, 24);
  codec::VideoEncodeParams rate;
  rate.width = 96;
  rate.height = 64;
  rate.fps = {25, 1};
  rate.bitrate_kbps = 200;
  auto low = tl::encode_frames("mpeg2video", rate, long_input);
  rate.bitrate_kbps = 1500;
  auto high = tl::encode_frames("mpeg2video", rate, long_input);
  REQUIRE_OK(low);
  REQUIRE_OK(high);
  CHECK(low->total_bytes() < high->total_bytes());

  // RGBA input is converted.
  std::vector<ir::VideoFrame> rgba;
  for (const auto& f : frames) rgba.push_back(*codec::to_rgba(f));
  CHECK_OK(tl::encode_frames("mpeg1video", params, rgba));
  // Invalid sizes are refused.
  codec::VideoEncodeParams huge = params;
  huge.width = 5000;
  CHECK_STATUS(tl::encode_frames("mpeg1video", huge, {}), Status::InvalidArgument);
}

TEST_CASE("codec_legacy/mpeg12: damaged streams never crash") {
  for (const char* name : {"mpeg1_96x64", "mpeg2_interlaced_96x64", "mpeg2_422_matrix_64x64"}) {
    tl::fuzz_decoder(tl::load_packets(name), 6);
  }
  // A 16383x16383 MPEG-2 sequence (size extension bits set) exceeds the limits.
  const std::uint8_t huge[] = {0x00, 0x00, 0x01, 0xB3, 0xFF, 0xFF, 0xFF, 0x13, 0xFF, 0xFF, 0xE0, 0x18,
                               0x00, 0x00, 0x01, 0xB5, 0x14, 0x8B, 0xE0, 0x01, 0x00, 0x00,
                               0x00, 0x00, 0x01, 0x00, 0x00, 0x0F, 0xFF, 0xF8};
  auto decoder = cvt::legacy::make_video_decoder("mpeg2video");
  ir::Packet p;
  p.bytes = to_bytes(std::span<const std::uint8_t>(huge));
  const auto result = tl::decode_packets(*decoder, {}, {p});
  CHECK_STATUS(result, Status::Limit);
}
