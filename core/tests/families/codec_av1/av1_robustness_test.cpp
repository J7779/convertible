// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// OBU syntax helpers and hostile input: truncated, corrupted and absurd AV1
// data must give an error (or a picture), never a crash, hang or huge
// allocation. Run under the asan preset.
#include <cstdint>
#include <vector>

#include "families/codec_av1/av1_api.hpp"
#include "families/codec_av1/av1_test_support.hpp"
#include "families/codec_av1/obu.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::av1test;
using namespace std::string_view_literals;

namespace {

/// A minimal Section 5 sequence header OBU (profile 0, 8-bit 4:2:0,
/// reduced still-picture header) for the given maximum frame size.
Bytes sequence_header_obu(std::uint32_t width, std::uint32_t height) {
  // Bits: profile(3)=0 still(1)=1 reduced(1)=1 level(5)=31
  //       width_bits-1(4)=15 height_bits-1(4)=15 w-1(16) h-1(16)
  //       sb128(1)=0 filter_intra(1)=0 edge(1)=0 superres(1)=0 cdef(1)=0 lr(1)=0
  //       high_bitdepth(1)=0 mono(1)=0 color_desc(1)=0 range(1)=0 csp(2)=0
  //       separate_uv_delta_q(1)=0 film_grain(1)=0, then trailing bits.
  std::vector<int> bits;
  const auto put = [&](std::uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) bits.push_back(static_cast<int>((value >> i) & 1u));
  };
  put(0, 3);
  put(1, 1);
  put(1, 1);
  put(31, 5);
  put(15, 4);
  put(15, 4);
  put(width - 1, 16);
  put(height - 1, 16);
  put(0, 6);
  put(0, 1);
  put(0, 1);
  put(0, 1);
  put(0, 1);
  put(0, 2);
  put(0, 1);
  put(0, 1);
  put(1, 1);  // trailing one bit
  while (bits.size() % 8 != 0) bits.push_back(0);
  Bytes payload;
  for (std::size_t i = 0; i < bits.size(); i += 8) {
    std::uint8_t byte = 0;
    for (std::size_t b = 0; b < 8; ++b) byte = static_cast<std::uint8_t>((byte << 1) | bits[i + b]);
    payload.push_back(static_cast<std::byte>(byte));
  }
  Bytes obu;
  obu.push_back(std::byte{0x0A});  // OBU_SEQUENCE_HEADER, has_size_field
  av1::write_leb128(obu, static_cast<std::uint32_t>(payload.size()));
  obu.insert(obu.end(), payload.begin(), payload.end());
  return obu;
}

}  // namespace

TEST_CASE("codec_av1: LEB128") {
  for (const std::uint32_t value : {0u, 1u, 127u, 128u, 300u, 16383u, 16384u, 0xFFFFFFFFu}) {
    Bytes encoded;
    av1::write_leb128(encoded, value);
    const auto decoded = av1::read_leb128(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->first == value);
    CHECK(decoded->second == encoded.size());
  }
  CHECK_FALSE(av1::read_leb128(test::bytes("\x80\x80")).has_value());                       // truncated
  CHECK_FALSE(av1::read_leb128(test::bytes("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x01")).has_value());  // too long
  CHECK_FALSE(av1::read_leb128(test::bytes("\xFF\xFF\xFF\xFF\x7F")).has_value());          // > 2^32 - 1
}

TEST_CASE("codec_av1: sequence header parsing of a hand-made header") {
  const Bytes obu = sequence_header_obu(640, 360);
  const auto obus = av1::split_section5(obu);
  REQUIRE_OK(obus);
  REQUIRE(obus->size() == 1);
  const auto header = av1::parse_sequence_header((*obus)[0].payload);
  REQUIRE_OK(header);
  CHECK(header->max_frame_width == 640);
  CHECK(header->max_frame_height == 360);
  CHECK(header->still_picture);
  CHECK(header->bit_depth == 8);
  const auto av1c = av1::make_av1c(obu);
  REQUIRE_OK(av1c);
  CHECK(av1c->size() == 4 + obu.size());
  CHECK(std::to_integer<int>((*av1c)[0]) == 0x81);
  CHECK(std::to_integer<int>((*av1c)[1]) == 31);  // profile 0, level 31
  CHECK(std::to_integer<int>((*av1c)[2]) == 0x0C);  // 4:2:0, chroma position unknown
  // Truncated payloads are errors (the last byte holds only trailing bits).
  for (std::size_t n = 0; n + 1 < (*obus)[0].payload.size(); ++n) {
    CHECK_STATUS(av1::parse_sequence_header((*obus)[0].payload.first(n)), Status::Decode);
  }
  CHECK_STATUS(av1::parse_av1c(test::bytes("\x81\x00"sv)), Status::Decode);
  CHECK_STATUS(av1::parse_av1c(test::bytes("\x01\x00\x00\x00"sv)), Status::Decode);
}

TEST_CASE("codec_av1: absurd picture sizes are refused before allocation") {
  const Bytes huge = sequence_header_obu(65535, 65535);
  CHECK_STATUS(av1::decode_still(huge), Status::Limit);
  av1::StillDecodeOptions small;
  small.max_pixels = 100 * 100;
  CHECK_STATUS(av1::decode_still(sequence_header_obu(640, 360), {}, small), Status::Limit);
  // The stream decoder applies the AV1 level 6.3 limits.
  auto dec = decoder();
  REQUIRE(dec != nullptr);
  codec::CodecParams params;
  params.timescale = 1000;
  REQUIRE_OK(dec->configure(params));
  ir::Packet packet;
  packet.bytes = sequence_header_obu(16384, 16384);
  CHECK_STATUS(dec->send_packet(packet), Status::Limit);
  // ... also from the extradata.
  auto dec2 = decoder();
  const auto av1c = av1::make_av1c(huge);
  REQUIRE_OK(av1c);
  params.extradata = *av1c;
  CHECK_STATUS(dec2->configure(params), Status::Limit);
}

TEST_CASE("codec_av1: garbage is refused") {
  CHECK_STATUS(av1::decode_still({}), Status::Decode);
  CHECK_STATUS(av1::decode_still(test::random_bytes(500, 3)), Status::Decode);
  CHECK_STATUS(av1::decode_still(test::bytes("\x12\x00"sv)), Status::Decode);  // only a temporal delimiter
  auto dec = decoder();
  codec::CodecParams params;
  REQUIRE_OK(dec->configure(params));
  ir::Packet packet;
  packet.bytes = test::bytes("\xFF\xFF\xFF\xFF");
  CHECK_STATUS(dec->send_packet(packet), Status::Decode);
  auto dec2 = decoder();
  params.extradata = test::bytes("\x7F\x7F");
  CHECK_STATUS(dec2->configure(params), Status::Decode);
}

TEST_CASE("codec_av1: truncated and corrupted streams never crash") {
  for (const char* name : {"codec_av1/aom_8bit_420_64x64.ivf", "codec_av1/svt_10bit_420_grain_128x72.ivf",
                           "codec_av1/aom_8bit_444_tiles_176x144.ivf", "codec_av1/aom_12bit_422_64x48.ivf"}) {
    INFO(name);
    const Bytes file = test::fixture(name);
    const auto ivf = read_ivf(file);
    REQUIRE(ivf.has_value());
    const ByteView key = ivf->frames[0].data;
    // Prefixes of the keyframe (sampled to keep the test fast).
    const std::size_t step = std::max<std::size_t>(1, key.size() / 400);
    std::size_t failures = 0;
    for (std::size_t n = 0; n < key.size(); n += step) {
      const auto result = av1::decode_still(key.first(n));
      if (!result) {
        ++failures;
        CHECK((result.error().code == Status::Decode || result.error().code == Status::Limit));
      }
    }
    CHECK(failures > 0);
    // Bit flips and byte overwrites across the whole stream.
    std::vector<ir::Packet> packets = ivf_packets(*ivf);
    for (std::uint32_t seed = 1; seed <= 64; ++seed) {
      std::vector<ir::Packet> damaged = packets;
      const Bytes noise = test::random_bytes(16, seed);
      for (std::size_t k = 0; k + 3 < noise.size(); k += 4) {
        ir::Packet& packet = damaged[std::to_integer<std::size_t>(noise[k]) % damaged.size()];
        if (packet.bytes.empty()) continue;
        const std::size_t at =
            ((std::to_integer<std::size_t>(noise[k + 1]) << 8) | std::to_integer<std::size_t>(noise[k + 2])) %
            packet.bytes.size();
        if (seed % 2 == 0) {
          packet.bytes[at] ^= static_cast<std::byte>(1u << (std::to_integer<unsigned>(noise[k + 3]) % 8));
        } else {
          packet.bytes[at] = noise[k + 3];
        }
      }
      (void)decode_packets(damaged, ivf->rate);  // must simply return
    }
    // Every packet truncated by a few bytes.
    std::vector<ir::Packet> cut = packets;
    for (ir::Packet& packet : cut) packet.bytes.resize(packet.bytes.size() * 2 / 3);
    (void)decode_packets(cut, ivf->rate);
  }
}

TEST_CASE("codec_av1: Annex B and still files survive damage") {
  for (const char* name : {"codec_av1/aom_annexb_64x48.obu", "codec_av1/aom_still_80x60.obu"}) {
    INFO(name);
    const Bytes file = test::fixture(name);
    const std::size_t step = 1;  // every prefix: the files are small
    for (std::size_t n = 0; n < file.size(); n += step) {
      (void)av1::decode_still(ByteView(file).first(n));
      ir::Packet packet;
      packet.bytes.assign(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n));
      (void)decode_packets({packet}, 1000);
    }
    for (std::uint32_t seed = 1; seed <= 64; ++seed) {
      Bytes damaged = file;
      const Bytes noise = test::random_bytes(4, seed);
      const std::size_t at =
          ((std::to_integer<std::size_t>(noise[0]) << 8) | std::to_integer<std::size_t>(noise[1])) % damaged.size();
      damaged[at] ^= noise[2] == std::byte{0} ? std::byte{1} : noise[2];
      (void)av1::decode_still(damaged);
    }
  }
}

TEST_CASE("codec_av1: decoder API misuse is reported") {
  auto dec = decoder();
  ir::Packet packet;
  packet.bytes = test::bytes("\x12\x00"sv);
  CHECK_STATUS(dec->send_packet(packet), Status::Internal);  // not configured
  codec::CodecParams params;
  REQUIRE_OK(dec->configure(params));
  REQUIRE_OK(dec->flush());
  CHECK_STATUS(dec->send_packet(packet), Status::Internal);  // after flush
  auto frame = dec->receive_frame();
  REQUIRE_OK(frame);
  CHECK_FALSE(frame->has_value());
}
