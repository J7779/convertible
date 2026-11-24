// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// NAL framing, SPS parsing and avcC records, checked against files written by
// x264 + ffmpeg (Annex B streams and the avcC/samples of the same streams in
// MP4).
#include <string>

#include "families/codec_h264/h264_bitstream.hpp"
#include "families/codec_h264/h264_test_support.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace std::string_view_literals;
namespace th = cvt::test::h264;
namespace h = cvt::h264;

namespace {

std::string path(std::string_view name, std::string_view extension) {
  return "codec_h264/" + std::string(name) + std::string(extension);
}

h::Sps first_sps(ByteView stream) {
  for (const ByteView nal : h::split_annexb(stream)) {
    if (h::nal_type(nal) == h::nal_sps) {
      if (auto sps = h::parse_sps(nal)) return *sps;
    }
  }
  return {};
}

}  // namespace

TEST_CASE("codec_h264/bitstream: Annex B splitting handles 3- and 4-byte start codes") {
  const Bytes stream = test::bytes("\x00\x00\x00\x01\x67\x42\x00\x00\x01\x68\xCE\x00\x00\x00\x00\x01\x65\x88\x00\x00\x03\x01"sv);
  const std::vector<ByteView> nals = h::split_annexb(stream);
  REQUIRE(nals.size() == 3);
  CHECK(test::str(Bytes(nals[0].begin(), nals[0].end())) == "\x67\x42"sv);
  CHECK(test::str(Bytes(nals[1].begin(), nals[1].end())) == "\x68\xCE"sv);  // trailing zeros removed
  CHECK(nals[2].size() == 6);                                                // 00 00 03 is payload
  CHECK(h::nal_type(nals[2]) == h::nal_slice_idr);
  CHECK(h::split_annexb(test::bytes("no start code")).empty());
  CHECK(h::nal_to_rbsp(nals[2]) == test::bytes("\x88\x00\x00\x01"sv));  // emulation prevention removed
}

TEST_CASE("codec_h264/bitstream: Annex B and length-prefixed framing convert both ways") {
  const th::PacketFile file = th::read_packet_file(test::fixture(path("main_cabac_bframes_176x144", ".pkt")));
  REQUIRE(file.packets.size() == 16);
  for (const codec::Packet& packet : file.packets) {
    const auto annexb = h::length_prefixed_to_annexb(packet.bytes, 4);
    REQUIRE_OK(annexb);
    CHECK(h::starts_with_start_code(*annexb));
    const auto back = h::annexb_to_length_prefixed(*annexb, 4);
    REQUIRE_OK(back);
    CHECK(*back == packet.bytes);
    for (const int size : {1, 2}) {
      const auto narrow = h::annexb_to_length_prefixed(*annexb, size);
      if (!narrow) continue;  // a NAL unit longer than the prefix allows
      const auto again = h::length_prefixed_to_annexb(*narrow, size);
      REQUIRE_OK(again);
      CHECK(*again == *annexb);
    }
  }
  CHECK_STATUS(h::split_length_prefixed(test::bytes("\x00\x00\x00\x09\x65"sv), 4), Status::Decode);
  CHECK_STATUS(h::split_length_prefixed(test::bytes("\x00\x00"sv), 4), Status::Decode);
  CHECK_STATUS(h::split_length_prefixed(test::bytes("\x01"sv), 3), Status::Decode);
  const Bytes big(300, std::byte{0x65});
  CHECK_STATUS(h::to_length_prefixed(std::vector<ByteView>{ByteView(big)}, 1), Status::Encode);
}

TEST_CASE("codec_h264/bitstream: access units are found in Annex B streams") {
  struct Case {
    const char* name;
    std::size_t units;
  };
  // One unit per frame (MBAFF codes frames, not separate fields).
  constexpr Case cases[] = {{"baseline_cavlc_176x144", 12}, {"main_cabac_bframes_176x144", 16},
                            {"high_cavlc_4slices_176x144", 10}, {"mbaff_176x144", 10},
                            {"videotoolbox_main_bframes_176x144", 12}};
  for (const Case& c : cases) {
    INFO(c.name);
    const Bytes stream = test::fixture(path(c.name, ".h264"));
    const std::vector<ByteView> units = h::split_access_units(stream);
    CHECK(units.size() == c.units);
    std::size_t covered = 0;
    for (const ByteView unit : units) covered += unit.size();
    CHECK(covered <= stream.size());
    CHECK(covered + 4 >= stream.size());
  }
}

TEST_CASE("codec_h264/bitstream: SPS fields of real streams") {
  const h::Sps baseline = first_sps(test::fixture(path("baseline_cavlc_176x144", ".h264")));
  CHECK(baseline.profile_idc == 66);
  CHECK(baseline.profile_name() == "Constrained Baseline");
  CHECK(baseline.width == 176);
  CHECK(baseline.height == 144);
  CHECK(baseline.frame_mbs_only);
  CHECK(baseline.frame_rate() == ir::Rational{25, 1});

  const h::Sps high = first_sps(test::fixture(path("high_8x8_weighted_multiref_176x144", ".h264")));
  CHECK(high.profile_idc == 100);
  CHECK(high.chroma_format_idc == 1);
  CHECK(high.bit_depth_luma == 8);
  CHECK(high.max_num_ref_frames >= 4);
  CHECK(!h::unsupported_reason(high).has_value());

  const h::Sps mbaff = first_sps(test::fixture(path("mbaff_176x144", ".h264")));
  CHECK(!mbaff.frame_mbs_only);
  CHECK(mbaff.mb_adaptive_frame_field);
  CHECK(mbaff.height == 144);

  const h::Sps crop = first_sps(test::fixture(path("crop_98x58_fullrange_bt709", ".h264")));
  CHECK(crop.coded_width == 112);
  CHECK(crop.coded_height == 64);
  CHECK(crop.width == 98);
  CHECK(crop.height == 58);
  CHECK(crop.crop_right == 14);
  CHECK(crop.crop_bottom == 6);
  CHECK(crop.video_full_range);
  CHECK(crop.matrix_coefficients == 1);
  CHECK(crop.color_info().matrix == ir::ColorMatrix::BT709);
  CHECK(crop.frame_rate() == ir::Rational{30, 1});

  const h::Sps sd = first_sps(test::fixture(path("crop_70x34_bt601", ".h264")));
  CHECK(sd.matrix_coefficients == 6);
  CHECK(sd.color_info().matrix == ir::ColorMatrix::BT601);
  CHECK(sd.frame_rate() == ir::Rational{24, 1});

  const h::Sps ten = first_sps(test::fixture(path("unsupported_high10_32x32", ".h264")));
  CHECK(ten.profile_idc == 110);
  CHECK(ten.bit_depth_luma == 10);
  CHECK(h::unsupported_reason(ten) == std::optional<std::string>("H.264 High 10 profile with 10-bit samples is not supported yet"));
  const h::Sps four22 = first_sps(test::fixture(path("unsupported_high422_32x32", ".h264")));
  CHECK(four22.chroma_format_idc == 2);
  CHECK(h::unsupported_reason(four22).has_value());

  CHECK_STATUS(h::parse_sps(test::bytes("\x68\x42"sv)), Status::Decode);           // not an SPS
  CHECK_STATUS(h::parse_sps(test::bytes("\x67\x42\x00\x0a"sv)), Status::Decode);   // truncated
}

TEST_CASE("codec_h264/bitstream: avcC records match ffmpeg's byte for byte") {
  for (const char* name : {"main_cabac_bframes_176x144", "high_8x8_weighted_multiref_176x144", "mbaff_176x144",
                           "videotoolbox_main_bframes_176x144"}) {
    INFO(name);
    const th::PacketFile file = th::read_packet_file(test::fixture(path(name, ".pkt")));
    const auto config = h::parse_avcc(file.avcc);
    REQUIRE_OK(config);
    CHECK(config->length_size == 4);
    REQUIRE(config->sps.size() == 1);
    REQUIRE(config->pps.size() == 1);
    CHECK(config->profile_idc == std::to_integer<std::uint8_t>(config->sps[0][1]));
    const std::vector<ByteView> sps = {config->sps[0]};
    const std::vector<ByteView> pps = {config->pps[0]};
    const auto rebuilt = h::build_avcc(sps, pps, 4);
    REQUIRE_OK(rebuilt);
    CHECK(*rebuilt == file.avcc);
    // The same parameter sets appear in the Annex B version of the stream.
    const Bytes stream = test::fixture(path(name, ".h264"));
    const std::vector<ByteView> nals = h::split_annexb(stream);
    const auto contains = [&](const Bytes& set) {
      return std::ranges::any_of(nals, [&](ByteView nal) { return std::ranges::equal(nal, ByteView(set)); });
    };
    CHECK(contains(config->sps[0]));
    CHECK(contains(config->pps[0]));
  }
  CHECK_STATUS(h::parse_avcc(test::bytes("\x01\x64\x00\x1f\xff\xe1\x00\x40\x67"sv)), Status::Decode);
  CHECK_STATUS(h::parse_avcc(test::bytes("\x02\x64\x00\x1f\xff\xe1\x00"sv)), Status::Decode);
  CHECK_STATUS(h::build_avcc({}, {}, 4), Status::Encode);
}

TEST_CASE("codec_h264/bitstream: hostile parameter sets never crash the parsers") {
  const Bytes stream = test::fixture(path("crop_98x58_fullrange_bt709", ".h264"));
  const std::vector<ByteView> nals = h::split_annexb(stream);
  REQUIRE(h::nal_type(nals[0]) == h::nal_sps);
  const Bytes sps(nals[0].begin(), nals[0].end());
  for (std::size_t length = 1; length <= sps.size(); ++length) (void)h::parse_sps(ByteView(sps).first(length));
  for (std::uint32_t seed = 1; seed <= 200; ++seed) {
    Bytes damaged = sps;
    const Bytes noise = test::random_bytes(4, seed);
    damaged[1 + std::to_integer<std::size_t>(noise[0]) % (damaged.size() - 1)] ^= noise[1];
    damaged[1 + std::to_integer<std::size_t>(noise[2]) % (damaged.size() - 1)] ^= noise[3];
    (void)h::parse_sps(damaged);
    Bytes record = test::random_bytes(std::to_integer<std::size_t>(noise[0]) + 1, seed);
    record[0] = std::byte{1};
    (void)h::parse_avcc(record);
    (void)h::split_access_units(test::random_bytes(200, seed));
  }
  Bytes ones(64, std::byte{0});
  ones[0] = std::byte{0x67};
  CHECK_STATUS(h::parse_sps(ones), Status::Decode);  // ue(v) with too many leading zeros
}
