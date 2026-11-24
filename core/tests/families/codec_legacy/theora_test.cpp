// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <limits>
#include <string>

#include "families/codec_legacy/legacy_test_support.hpp"
#include "support/test.hpp"

using namespace cvt;
namespace tl = cvt::test::legacy;

namespace {

/// The three header packets of Xiph-laced extradata.
std::vector<Bytes> laced_headers(const Bytes& extradata) {
  const auto d = as_u8(ByteView(extradata));
  REQUIRE(d.size() > 3);
  REQUIRE(d[0] == 2);
  std::size_t at = 1;
  std::size_t sizes[2] = {0, 0};
  for (std::size_t& s : sizes) {
    while (d[at] == 255) s += 255, ++at;
    s += d[at++];
  }
  std::vector<Bytes> out;
  out.push_back(to_bytes(ByteView(extradata).subspan(at, sizes[0])));
  out.push_back(to_bytes(ByteView(extradata).subspan(at + sizes[0], sizes[1])));
  out.push_back(to_bytes(ByteView(extradata).subspan(at + sizes[0] + sizes[1])));
  return out;
}

}  // namespace

TEST_CASE("codec_legacy/theora: decodes libtheora output bit-exactly (Xiph-laced extradata)") {
  const tl::PacketFile file = tl::load_packets("theora_64x64");
  auto frames = tl::decode_file(file);
  REQUIRE_OK(frames);
  CHECK(tl::compare_with_reference(*frames, tl::load_reference("theora_64x64")) == std::numeric_limits<double>::infinity());
  CHECK(frames->front().format == ir::PixelFormat::I420);
  CHECK(frames->front().color.matrix == ir::ColorMatrix::BT601);
  for (std::size_t i = 1; i < frames->size(); ++i) CHECK((*frames)[i].pts_ms > (*frames)[i - 1].pts_ms);
}

TEST_CASE("codec_legacy/theora: length-prefixed and in-band headers are accepted") {
  tl::PacketFile file = tl::load_packets("theora_64x64");
  const auto reference = tl::decode_file(file);
  REQUIRE_OK(reference);
  const std::vector<Bytes> headers = laced_headers(file.extradata);
  // FFmpeg/MP4 style: 16-bit big-endian lengths.
  Bytes prefixed;
  for (const Bytes& h : headers) {
    prefixed.push_back(static_cast<std::byte>(h.size() >> 8));
    prefixed.push_back(static_cast<std::byte>(h.size() & 0xFF));
    prefixed.insert(prefixed.end(), h.begin(), h.end());
  }
  tl::PacketFile alt = file;
  alt.extradata = prefixed;
  auto frames = tl::decode_file(alt);
  REQUIRE_OK(frames);
  CHECK(frames->size() == reference->size());
  // Ogg style: the headers are the first packets.
  tl::PacketFile ogg = file;
  ogg.extradata.clear();
  std::vector<ir::Packet> packets;
  for (const Bytes& h : headers) {
    ir::Packet p;
    p.bytes = h;
    packets.push_back(p);
  }
  packets.insert(packets.end(), file.packets.begin(), file.packets.end());
  auto decoder = cvt::legacy::make_video_decoder("theora");
  auto in_band = tl::decode_packets(*decoder, ogg.params(), packets);
  REQUIRE_OK(in_band);
  REQUIRE(in_band->size() == reference->size());
  CHECK(test::min_psnr(*in_band, *reference) == std::numeric_limits<double>::infinity());
}

TEST_CASE("codec_legacy/theora: encoder round trip quality and size") {
  const auto frames = tl::textured_frames(ir::PixelFormat::I420, 70, 46, 10);
  std::vector<std::size_t> sizes;
  std::vector<double> quality;
  for (const int q : {95, 80, 50, 20}) {
    codec::VideoEncodeParams params;
    params.width = 70;
    params.height = 46;
    params.fps = {25, 1};
    params.quality = q;
    params.keyframe_interval = 5;
    params.color = frames[0].color;
    auto encoded = tl::encode_frames("theora", params, frames);
    REQUIRE_OK(encoded);
    REQUIRE(encoded->packets.size() == frames.size());
    CHECK(encoded->packets[0].keyframe);
    CHECK(encoded->packets[5].keyframe);
    tl::dump_for_interop("theora_q" + std::to_string(q), "theora", 70, 46, params.timescale, *encoded);
    tl::dump_raw_for_interop("theora_q" + std::to_string(q), frames);
    auto decoder = cvt::legacy::make_video_decoder("theora");
    codec::CodecParams cp;
    cp.extradata = encoded->extradata;
    cp.timescale = params.timescale;
    auto decoded = tl::decode_packets(*decoder, cp, encoded->packets);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) (*decoded)[i].color = frames[i].color;
    sizes.push_back(encoded->total_bytes());
    quality.push_back(test::min_psnr(frames, *decoded));
  }
  INFO(quality[0], " ", quality[1], " ", quality[2], " ", quality[3]);
  CHECK(quality[0] >= 42.0);
  CHECK(quality[1] >= 36.0);
  CHECK(quality[2] >= 32.0);
  CHECK(quality[3] >= 27.0);
  CHECK(sizes[0] > sizes[1]);
  CHECK(sizes[1] > sizes[2]);
  CHECK(sizes[2] > sizes[3]);
}

TEST_CASE("codec_legacy/theora: damaged input never crashes") {
  tl::fuzz_decoder(tl::load_packets("theora_64x64"), 4);
  auto decoder = cvt::legacy::make_video_decoder("theora");
  codec::CodecParams cp;
  cp.extradata = test::bytes("\x02\x01\x01xyz");
  CHECK_STATUS(decoder->configure(cp), Status::Decode);
}
