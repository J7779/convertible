// Damaged VP8/VP9 input: truncated, bit-flipped and garbage packets, and
// frames that declare absurd sizes, must fail cleanly (run under ASan).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "codec/ids.hpp"
#include "families/codec_vpx/vpx_bitstream.hpp"
#include "families/codec_vpx/vpx_codecs.hpp"
#include "families/codec_vpx/vpx_test_util.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace std::string_view_literals;

namespace {

/// Feed packets until the decoder refuses one; the decoder must never
/// crash, and every frame it does return must be well formed.
std::size_t decode_leniently(std::string_view codec_id, const std::vector<ir::Packet>& packets) {
  auto decoder = test::vpx::media().make_video_decoder(codec_id);
  codec::CodecParams params;
  params.codec = std::string(codec_id);
  if (!decoder || !decoder->configure(params)) return 0;
  std::size_t frames = 0;
  const auto drain = [&] {
    while (true) {
      const auto frame = decoder->receive_frame();
      if (!frame || !*frame) return;
      CHECK((*frame)->valid());
      ++frames;
    }
  };
  for (const ir::Packet& packet : packets) {
    if (!decoder->send_packet(packet)) {
      // After an error the decoder refuses further use instead of misbehaving.
      CHECK(!decoder->send_packet(packet));
      return frames;
    }
    drain();
  }
  if (decoder->flush()) drain();
  return frames;
}

struct Stream {
  std::string_view fixture;
  std::string_view codec;
};

constexpr Stream streams[] = {
    {"codec_vpx/vp8_64x64.ivf", codec::id::vp8},
    {"codec_vpx/vp8_altref_96x64.ivf", codec::id::vp8},
    {"codec_vpx/vp9_p0_altref_176x144.ivf", codec::id::vp9},
    {"codec_vpx/vp9_p2_10bit_64x64.ivf", codec::id::vp9},
    {"codec_vpx/vp9_p1_444_64x48.ivf", codec::id::vp9},
    {"codec_vpx/vp9_resize.ivf", codec::id::vp9},
};

}  // namespace

TEST_CASE("codec_vpx: truncated packets fail cleanly") {
  for (const Stream& stream : streams) {
    INFO("fixture ", stream.fixture);
    const auto ivf = test::vpx::parse_ivf(test::fixture(stream.fixture));
    REQUIRE(ivf.has_value());
    // Every prefix (bounded) of the keyframe, then of an inter frame.
    for (const std::size_t victim : {std::size_t{0}, std::size_t{2}}) {
      const Bytes& whole = ivf->packets[victim].bytes;
      const std::size_t step = whole.size() / 200 + 1;
      for (std::size_t length = 0; length < whole.size(); length += step) {
        std::vector<ir::Packet> packets(ivf->packets.begin(), ivf->packets.begin() + 4);
        packets[victim].bytes.resize(length);
        (void)decode_leniently(stream.codec, packets);
      }
    }
    // The stream cut short after a few packets decodes what it has.
    std::vector<ir::Packet> head(ivf->packets.begin(), ivf->packets.begin() + 3);
    CHECK(decode_leniently(stream.codec, head) >= 2u);
  }
}

TEST_CASE("codec_vpx: corrupted packets fail cleanly") {
  for (const Stream& stream : streams) {
    INFO("fixture ", stream.fixture);
    const auto ivf = test::vpx::parse_ivf(test::fixture(stream.fixture));
    REQUIRE(ivf.has_value());
    for (std::uint32_t seed = 1; seed <= 48; ++seed) {
      std::vector<ir::Packet> packets = ivf->packets;
      const Bytes noise = test::random_bytes(16, seed);
      for (std::size_t k = 0; k < 4; ++k) {
        ir::Packet& packet = packets[(std::to_integer<std::size_t>(noise[k * 4]) + seed) % packets.size()];
        if (packet.bytes.empty()) continue;
        const std::size_t at =
            (std::to_integer<std::size_t>(noise[k * 4 + 1]) * 257u + std::to_integer<std::size_t>(noise[k * 4 + 2])) %
            packet.bytes.size();
        packet.bytes[at] ^= noise[k * 4 + 3] | std::byte{1};
      }
      (void)decode_leniently(stream.codec, packets);
    }
    // Pure garbage, and garbage after a valid keyframe.
    std::vector<ir::Packet> garbage(3);
    for (std::size_t i = 0; i < garbage.size(); ++i) {
      garbage[i].bytes = test::random_bytes(700, 50 + static_cast<std::uint32_t>(i));
    }
    CHECK(decode_leniently(stream.codec, garbage) == 0u);
    std::vector<ir::Packet> mixed = {ivf->packets[0], garbage[0], ivf->packets[1]};
    (void)decode_leniently(stream.codec, mixed);
  }
}

TEST_CASE("codec_vpx: frames declaring absurd sizes are refused before allocation") {
  {
    // VP8 keyframe of 16383 x 16383 pixels.
    const auto ivf = test::vpx::parse_ivf(test::fixture("codec_vpx/vp8_64x64.ivf"));
    REQUIRE(ivf.has_value());
    ir::Packet packet = ivf->packets[0];
    packet.bytes[6] = std::byte{0xFF};
    packet.bytes[7] = std::byte{0x3F};
    packet.bytes[8] = std::byte{0xFF};
    packet.bytes[9] = std::byte{0x3F};
    auto decoder = test::vpx::media().make_video_decoder(codec::id::vp8);
    REQUIRE_OK(decoder->configure(codec::CodecParams{}));
    CHECK_STATUS(decoder->send_packet(packet), Status::Limit);
  }
  {
    // VP9 profile 0 keyframe: frame marker, profile 0, key, shown, sync code,
    // BT.601 limited, then a 65536 x 65536 frame size.
    util::ByteWriter header;
    // Bits: 10 0 0 0 0 1 0 | 01001001 10000011 01000010 | 001 0 | 16 x 1 | 16 x 1 | 0
    const std::string bits =
        "10" "0" "0" "0" "0" "1" "0" "010010011000001101000010" "001" "0" "1111111111111111" "1111111111111111" "0";
    std::uint8_t current = 0;
    int filled = 0;
    for (const char bit : bits) {
      current = static_cast<std::uint8_t>((current << 1) | (bit == '1' ? 1 : 0));
      if (++filled == 8) {
        header.u8(current);
        current = 0;
        filled = 0;
      }
    }
    header.u8(static_cast<std::uint8_t>(current << (8 - filled)));
    header.zeros(64);
    const auto parsed = vpx::parse_vp9_header(header.view());
    REQUIRE_OK(parsed);
    CHECK(parsed->keyframe);
    CHECK(parsed->width == 65536u);
    CHECK(parsed->height == 65536u);
    ir::Packet packet;
    packet.bytes = header.take();
    auto decoder = test::vpx::media().make_video_decoder(codec::id::vp9);
    REQUIRE_OK(decoder->configure(codec::CodecParams{}));
    CHECK_STATUS(decoder->send_packet(packet), Status::Limit);
  }
}

TEST_CASE("codec_vpx: a superframe index that overruns its packet is an error") {
  // Two frames of 0x30 bytes each claimed by a 2-byte magnitude index in a
  // packet that is far too short.
  const Bytes packet = test::bytes("\x82\x49\x83\x42\x00\xC9\x30\x00\x30\x00\xC9"sv);
  CHECK_STATUS(vpx::split_vp9_superframe(packet), Status::Decode);
  auto decoder = test::vpx::media().make_video_decoder(codec::id::vp9);
  REQUIRE_OK(decoder->configure(codec::CodecParams{}));
  ir::Packet bad;
  bad.bytes = packet;
  CHECK_STATUS(decoder->send_packet(bad), Status::Decode);
}
