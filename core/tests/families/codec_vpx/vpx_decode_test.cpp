// Decoding real VP8 and VP9 streams written by libvpx (through ffmpeg and
// vpxenc), checked bit-exactly against ffmpeg's own decoder (framemd5).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codec/ids.hpp"
#include "families/codec_vpx/vpx_bitstream.hpp"
#include "families/codec_vpx/vpx_codecs.hpp"
#include "families/codec_vpx/vpx_test_util.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using test::vpx::FrameMd5;

namespace {

struct Expected {
  std::string_view name;  ///< fixture base name in codec_vpx/
  std::string_view codec;
  ir::PixelFormat format;
  ir::ColorMatrix matrix;
  ir::ColorRange range;
};

/// Decode a fixture and compare every frame with the reference MD5s, sizes
/// and timestamps produced by ffmpeg from the same file.
void check_fixture(const Expected& expected) {
  INFO("fixture ", expected.name);
  const std::string base = std::string("codec_vpx/") + std::string(expected.name);
  const auto ivf = test::vpx::parse_ivf(test::fixture(base + ".ivf"));
  REQUIRE(ivf.has_value());
  REQUIRE(ivf->scale == 1u);
  CHECK(ivf->fourcc == (expected.codec == codec::id::vp8 ? "VP80" : "VP90"));
  const std::vector<FrameMd5> reference = test::vpx::parse_framemd5(test::fixture_text(base + ".framemd5"));
  REQUIRE(!reference.empty());

  const auto frames = test::vpx::decode_all(expected.codec, ivf->packets, ivf->rate);
  REQUIRE_OK(frames);
  REQUIRE(frames->size() == reference.size());
  for (std::size_t i = 0; i < frames->size(); ++i) {
    INFO("frame ", i);
    const ir::VideoFrame& frame = (*frames)[i];
    CHECK(frame.valid());
    CHECK(frame.format == expected.format);
    CHECK(frame.color.matrix == expected.matrix);
    CHECK(frame.color.range == expected.range);
    std::size_t size = 0;
    CHECK(test::vpx::frame_md5(frame, &size) == reference[i].md5);
    CHECK(size == reference[i].size);
    const auto to_ms = [&](std::int64_t ticks) {
      const auto rate = static_cast<std::int64_t>(ivf->rate);
      return (ticks * 1000 + rate / 2) / rate;
    };
    CHECK(frame.pts_ms == to_ms(reference[i].pts));
    CHECK(frame.duration_ms == to_ms(reference[i].pts + reference[i].duration) - to_ms(reference[i].pts));
  }
}

}  // namespace

TEST_CASE("codec_vpx/vp8: decodes libvpx streams bit-exactly") {
  check_fixture({"vp8_64x64", codec::id::vp8, ir::PixelFormat::I420, ir::ColorMatrix::BT601, ir::ColorRange::Limited});
  check_fixture(
      {"vp8_odd_33x19", codec::id::vp8, ir::PixelFormat::I420, ir::ColorMatrix::BT601, ir::ColorRange::Limited});
}

TEST_CASE("codec_vpx/vp8: hidden alternate reference frames are not shown") {
  const auto ivf = test::vpx::parse_ivf(test::fixture("codec_vpx/vp8_altref_96x64.ivf"));
  REQUIRE(ivf.has_value());
  // vpxenc wrote one more packet than there are pictures: the invisible ARF.
  std::size_t hidden = 0;
  for (const ir::Packet& packet : ivf->packets) {
    const auto header = vpx::parse_vp8_header(packet.bytes);
    REQUIRE_OK(header);
    if (!header->show_frame) ++hidden;
  }
  CHECK(hidden == 1u);
  CHECK(ivf->packets.size() == 21u);
  check_fixture(
      {"vp8_altref_96x64", codec::id::vp8, ir::PixelFormat::I420, ir::ColorMatrix::BT601, ir::ColorRange::Limited});
}

TEST_CASE("codec_vpx/vp9: decodes profile 0 with superframes and alternate references") {
  const auto ivf = test::vpx::parse_ivf(test::fixture("codec_vpx/vp9_p0_altref_176x144.ivf"));
  REQUIRE(ivf.has_value());
  std::size_t superframes = 0;
  std::size_t hidden = 0;
  for (const ir::Packet& packet : ivf->packets) {
    const auto frames = vpx::split_vp9_superframe(packet.bytes);
    REQUIRE_OK(frames);
    if (frames->size() > 1) ++superframes;
    for (const ByteView frame : *frames) {
      const auto header = vpx::parse_vp9_header(frame);
      REQUIRE_OK(header);
      if (!header->show_frame) ++hidden;
    }
  }
  CHECK(superframes >= 1u);
  CHECK(hidden >= 1u);
  check_fixture({"vp9_p0_altref_176x144", codec::id::vp9, ir::PixelFormat::I420, ir::ColorMatrix::BT709,
                 ir::ColorRange::Limited});
}

TEST_CASE("codec_vpx/vp9: decodes 10-bit profile 2") {
  check_fixture({"vp9_p2_10bit_64x64", codec::id::vp9, ir::PixelFormat::I420P10, ir::ColorMatrix::BT601,
                 ir::ColorRange::Limited});
}

TEST_CASE("codec_vpx/vp9: decodes 4:4:4 full-range profile 1") {
  check_fixture(
      {"vp9_p1_444_64x48", codec::id::vp9, ir::PixelFormat::I444, ir::ColorMatrix::BT601, ir::ColorRange::Full});
}

TEST_CASE("codec_vpx/vp9: decodes odd dimensions") {
  check_fixture(
      {"vp9_odd_65x47", codec::id::vp9, ir::PixelFormat::I420, ir::ColorMatrix::BT601, ir::ColorRange::Limited});
}

TEST_CASE("codec_vpx/vp9: decodes a lossless stream back to the source pictures") {
  check_fixture(
      {"vp9_lossless_64x64", codec::id::vp9, ir::PixelFormat::I420, ir::ColorMatrix::BT601, ir::ColorRange::Limited});
  // The reference MD5s are those of the uncompressed source as well.
  const auto source = test::vpx::parse_framemd5(test::fixture_text("codec_vpx/vp9_lossless_source.framemd5"));
  const auto decoded = test::vpx::parse_framemd5(test::fixture_text("codec_vpx/vp9_lossless_64x64.framemd5"));
  REQUIRE(source.size() == decoded.size());
  for (std::size_t i = 0; i < source.size(); ++i) CHECK(source[i].md5 == decoded[i].md5);
}

TEST_CASE("codec_vpx/vp9: follows a change of resolution") {
  check_fixture(
      {"vp9_resize", codec::id::vp9, ir::PixelFormat::I420, ir::ColorMatrix::BT601, ir::ColorRange::Limited});
  const auto ivf = test::vpx::parse_ivf(test::fixture("codec_vpx/vp9_resize.ivf"));
  REQUIRE(ivf.has_value());
  const auto frames = test::vpx::decode_all(codec::id::vp9, ivf->packets, ivf->rate);
  REQUIRE_OK(frames);
  REQUIRE(frames->size() == 12u);
  CHECK(frames->front().width == 64u);
  CHECK(frames->front().height == 64u);
  CHECK(frames->back().width == 96u);
  CHECK(frames->back().height == 48u);
}

TEST_CASE("codec_vpx: timestamps come from packets, durations from the next frame when missing") {
  const auto ivf = test::vpx::parse_ivf(test::fixture("codec_vpx/vp8_64x64.ivf"));
  REQUIRE(ivf.has_value());
  std::vector<ir::Packet> packets = ivf->packets;
  for (std::size_t i = 0; i < packets.size(); ++i) {
    packets[i].pts = 1000 + static_cast<std::int64_t>(i) * 3003;  // 29.97 fps in a 90 kHz time base
    packets[i].duration = i == 3 ? 3003 : 0;                      // only one packet states its duration
  }
  const auto frames = test::vpx::decode_all(codec::id::vp8, packets, 90000);
  REQUIRE_OK(frames);
  REQUIRE(frames->size() == packets.size());
  for (std::size_t i = 0; i < frames->size(); ++i) {
    INFO("frame ", i);
    const std::int64_t start = (1000 + static_cast<std::int64_t>(i) * 3003);
    CHECK((*frames)[i].pts_ms == (start * 1000 + 45000) / 90000);
    CHECK((*frames)[i].pts_ms + (*frames)[i].duration_ms == ((start + 3003) * 1000 + 45000) / 90000);
  }
}

TEST_CASE("codec_vpx: VP8 colours follow a vpcC record when one is given") {
  const auto ivf = test::vpx::parse_ivf(test::fixture("codec_vpx/vp8_64x64.ivf"));
  REQUIRE(ivf.has_value());
  vpx::VpcConfig config;
  config.matrix_coefficients = 1;  // BT.709
  config.full_range = true;
  const auto frames = test::vpx::decode_all(codec::id::vp8, ivf->packets, ivf->rate, vpx::make_vpcc(config));
  REQUIRE_OK(frames);
  REQUIRE(!frames->empty());
  CHECK(frames->front().color.matrix == ir::ColorMatrix::BT709);
  CHECK(frames->front().color.range == ir::ColorRange::Full);
}

TEST_CASE("codec_vpx: decoders are registered for vp8 and vp9 and follow the calling pattern") {
  CHECK(test::vpx::media().has_video_decoder(codec::id::vp8));
  CHECK(test::vpx::media().has_video_decoder(codec::id::vp9));
  CHECK(test::vpx::media().has_video_encoder(codec::id::vp8));
  CHECK(test::vpx::media().has_video_encoder(codec::id::vp9));
  CHECK(test::vpx::media().problems().empty());
  CHECK(vpx::libvpx_version() == "v1.16.0");

  auto decoder = test::vpx::media().make_video_decoder(codec::id::vp9);
  REQUIRE(decoder != nullptr);
  CHECK_STATUS(decoder->send_packet(ir::Packet{}), Status::Internal);  // not configured
  codec::CodecParams params;
  params.codec = "vp9";
  REQUIRE_OK(decoder->configure(params));
  const auto none = decoder->receive_frame();
  REQUIRE_OK(none);
  CHECK(!none->has_value());
  REQUIRE_OK(decoder->flush());
  CHECK_STATUS(decoder->send_packet(ir::Packet{}), Status::Internal);  // after flush
}

TEST_CASE("codec_vpx/vp9: a show-existing frame re-displays a reference") {
  const auto ivf = test::vpx::parse_ivf(test::fixture("codec_vpx/vp9_odd_65x47.ivf"));
  REQUIRE(ivf.has_value());
  // The last shown frame among the first four packets that refreshed slot 0.
  std::optional<std::size_t> slot0;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto frames = vpx::split_vp9_superframe(ivf->packets[i].bytes);
    REQUIRE_OK(frames);
    for (const ByteView frame : *frames) {
      const auto header = vpx::parse_vp9_header(frame);
      REQUIRE_OK(header);
      if ((header->refresh_frame_flags & 1u) != 0) {
        slot0 = header->show_frame ? std::optional<std::size_t>(i) : std::nullopt;
      }
    }
  }
  REQUIRE(slot0.has_value());
  // A one-byte frame: marker 10, profile 0, show_existing_frame 1, slot 000.
  std::vector<ir::Packet> packets(ivf->packets.begin(), ivf->packets.begin() + 4);
  ir::Packet show_existing;
  show_existing.bytes = {std::byte{0x88}};
  show_existing.pts = 100;
  packets.push_back(show_existing);
  const auto frames = test::vpx::decode_all(codec::id::vp9, packets, ivf->rate);
  REQUIRE_OK(frames);
  REQUIRE(frames->size() == 5u);
  CHECK(test::vpx::frame_md5((*frames)[4]) == test::vpx::frame_md5((*frames)[*slot0]));
  CHECK((*frames)[4].pts_ms == 4000);
}
