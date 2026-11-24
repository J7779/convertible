// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Decoding of real AV1 streams written by libaom (ffmpeg, aomenc), SVT-AV1
// and rav1e, compared bit-exactly with per-frame MD5s of dav1d's output (see
// core/tests/data/codec_av1/SOURCES.md).
#include <cstdint>
#include <string>
#include <vector>

#include "families/codec_av1/av1_api.hpp"
#include "families/codec_av1/obu.hpp"
#include "families/codec_av1/av1_test_support.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::av1test;

namespace {

struct StreamCase {
  const char* name;
  ir::PixelFormat format;
  std::uint32_t width;
  std::uint32_t height;
  bool monochrome;
};

constexpr StreamCase ivf_cases[] = {
    {"aom_8bit_420_64x64", ir::PixelFormat::I420, 64, 64, false},
    {"aom_10bit_420_96x64", ir::PixelFormat::I420P10, 96, 64, false},
    {"svt_10bit_420_grain_128x72", ir::PixelFormat::I420P10, 128, 72, false},
    {"aom_8bit_420_grain_64x64", ir::PixelFormat::I420, 64, 64, false},
    {"rav1e_8bit_420_66x44", ir::PixelFormat::I420, 66, 44, false},
    {"aom_8bit_420_odd_67x45", ir::PixelFormat::I420, 67, 45, false},
    {"aom_8bit_444_tiles_176x144", ir::PixelFormat::I444, 176, 144, false},
    {"aom_12bit_422_64x48", ir::PixelFormat::I422P10, 64, 48, false},
    {"aom_mono_64x64", ir::PixelFormat::I420, 64, 64, true},
};

std::string fixture_name(const char* name, const char* extension) {
  return std::string("codec_av1/") + name + extension;
}

/// Sequence header of the first frame of an IVF fixture.
av1::SequenceHeader first_sequence_header(const Ivf& ivf) {
  const auto obus = av1::split_section5(ivf.frames.at(0).data);
  if (obus) {
    for (const av1::Obu& obu : *obus) {
      if (obu.type != av1::ObuType::SequenceHeader) continue;
      if (auto header = av1::parse_sequence_header(obu.payload)) return *header;
    }
  }
  return {};
}

}  // namespace

TEST_CASE("codec_av1: decodes independent encoders' streams bit-exactly") {
  for (const StreamCase& c : ivf_cases) {
    INFO(c.name);
    const Bytes file = test::fixture(fixture_name(c.name, ".ivf"));
    const auto ivf = read_ivf(file);
    REQUIRE(ivf.has_value());
    const std::vector<std::string> md5s = read_md5s(fixture_name(c.name, ".md5"));
    const auto frames = decode_packets(ivf_packets(*ivf), ivf->rate);
    REQUIRE_OK(frames);
    REQUIRE(frames->size() == md5s.size());
    REQUIRE(frames->size() == ivf->frames.size());  // one shown picture per temporal unit
    for (std::size_t i = 0; i < frames->size(); ++i) {
      INFO("frame ", i);
      const ir::VideoFrame& frame = (*frames)[i];
      CHECK(frame.format == c.format);
      CHECK(frame.width == c.width);
      CHECK(frame.height == c.height);
      CHECK(frame.valid());
      CHECK(frame_md5(frame) == md5s[i]);
      // Timestamps come from the packets (IVF ticks -> ms); durations from
      // the distance to the next picture, the last one repeating the previous.
      const auto expected_pts = static_cast<std::int64_t>(
          (static_cast<double>(ivf->frames[i].pts) * ivf->scale * 1000.0) / ivf->rate + 0.5);
      CHECK(frame.pts_ms == expected_pts);
      if (i + 1 < frames->size()) CHECK(frame.duration_ms == (*frames)[i + 1].pts_ms - frame.pts_ms);
      if (i > 0 && i + 1 == frames->size()) CHECK(frame.duration_ms == (*frames)[i - 1].duration_ms);
    }
    if (c.monochrome) {
      const ir::VideoFrame& frame = frames->front();
      CHECK(frame.planes[1].data == std::vector<std::uint8_t>(frame.planes[1].data.size(), 128));
    }
  }
}

TEST_CASE("codec_av1: sequence headers describe the streams") {
  struct Expect {
    const char* name;
    std::uint8_t profile;
    std::uint8_t bit_depth;
    bool monochrome;
    std::uint8_t ssx;
    std::uint8_t ssy;
    bool grain;
  };
  constexpr Expect expectations[] = {
      {"aom_8bit_420_64x64", 0, 8, false, 1, 1, false},       {"aom_10bit_420_96x64", 0, 10, false, 1, 1, false},
      {"svt_10bit_420_grain_128x72", 0, 10, false, 1, 1, true}, {"aom_8bit_420_grain_64x64", 0, 8, false, 1, 1, true},
      {"aom_8bit_444_tiles_176x144", 1, 8, false, 0, 0, false}, {"aom_12bit_422_64x48", 2, 12, false, 1, 0, false},
      {"aom_mono_64x64", 0, 8, true, 1, 1, false},
  };
  for (const Expect& e : expectations) {
    INFO(e.name);
    const Bytes file = test::fixture(fixture_name(e.name, ".ivf"));
    const auto ivf = read_ivf(file);
    REQUIRE(ivf.has_value());
    const av1::SequenceHeader s = first_sequence_header(*ivf);
    CHECK(s.profile == e.profile);
    CHECK(s.bit_depth == e.bit_depth);
    CHECK(s.monochrome == e.monochrome);
    CHECK(s.subsampling_x == e.ssx);
    CHECK(s.subsampling_y == e.ssy);
    CHECK(s.film_grain_params_present == e.grain);
    CHECK(s.max_frame_width >= ivf->width);
    CHECK(s.max_frame_height >= ivf->height);
  }
}

TEST_CASE("codec_av1: film grain is synthesised (and can be skipped for stills)") {
  for (const char* name : {"aom_8bit_420_grain_64x64", "svt_10bit_420_grain_128x72"}) {
    INFO(name);
    const Bytes file = test::fixture(fixture_name(name, ".ivf"));
    const auto ivf = read_ivf(file);
    REQUIRE(ivf.has_value());
    const std::vector<std::string> md5s = read_md5s(fixture_name(name, ".md5"));
    const auto grained = av1::decode_still(ivf->frames[0].data);
    REQUIRE_OK(grained);
    CHECK(frame_md5(grained->frame) == md5s[0]);
    av1::StillDecodeOptions options;
    options.apply_film_grain = false;
    const auto clean = av1::decode_still(ivf->frames[0].data, {}, options);
    REQUIRE_OK(clean);
    CHECK(frame_md5(clean->frame) != md5s[0]);
  }
}

TEST_CASE("codec_av1: alt-ref streams use show_existing_frame and keep presentation order") {
  const Bytes file = test::fixture("codec_av1/aom_8bit_420_64x64.ivf");
  const auto ivf = read_ivf(file);
  REQUIRE(ivf.has_value());
  int show_existing = 0;
  int hidden_units = 0;
  for (const IvfFrame& f : ivf->frames) {
    const auto obus = av1::split_section5(f.data);
    REQUIRE_OK(obus);
    int frames_in_unit = 0;
    for (const av1::Obu& obu : *obus) {
      if (obu.type == av1::ObuType::Frame) ++frames_in_unit;
      // show_existing_frame is the first bit of a (non-reduced) frame header.
      if (obu.type == av1::ObuType::FrameHeader && !obu.payload.empty() &&
          (std::to_integer<std::uint8_t>(obu.payload[0]) & 0x80u) != 0) {
        ++show_existing;
      }
    }
    if (frames_in_unit > 1) ++hidden_units;
  }
  CHECK(show_existing > 0);
  CHECK(hidden_units > 0);
  // Decoded pts strictly increase (the fixture test checks exact pixels).
  const auto frames = decode_packets(ivf_packets(*ivf), ivf->rate);
  REQUIRE_OK(frames);
  for (std::size_t i = 1; i < frames->size(); ++i) CHECK((*frames)[i].pts_ms > (*frames)[i - 1].pts_ms);
}

TEST_CASE("codec_av1: MP4-style samples with av1C extradata decode identically") {
  for (const char* name : {"aom_8bit_420_64x64", "aom_10bit_420_96x64", "aom_8bit_444_tiles_176x144"}) {
    INFO(name);
    const Bytes file = test::fixture(fixture_name(name, ".ivf"));
    const auto ivf = read_ivf(file);
    REQUIRE(ivf.has_value());
    const std::vector<std::string> md5s = read_md5s(fixture_name(name, ".md5"));
    // Build av1C from the in-band sequence header, then strip temporal
    // delimiters AND sequence headers from the samples: the decoder must
    // take the configuration from the extradata.
    const auto header = av1::find_sequence_header_obu(ivf->frames[0].data);
    REQUIRE_OK(header);
    REQUIRE(header->has_value());
    const auto av1c = av1::make_av1c(**header);
    REQUIRE_OK(av1c);
    const auto config = av1::parse_av1c(*av1c);
    REQUIRE_OK(config);
    CHECK(config->config_obus == **header);
    std::vector<ir::Packet> packets = ivf_packets(*ivf);
    for (ir::Packet& packet : packets) {
      const auto obus = av1::split_section5(packet.bytes);
      REQUIRE_OK(obus);
      Bytes stripped;
      for (const av1::Obu& obu : *obus) {
        if (obu.type == av1::ObuType::TemporalDelimiter || obu.type == av1::ObuType::SequenceHeader) continue;
        av1::append_obu(stripped, obu);
      }
      packet.bytes = std::move(stripped);
      packet.duration = 1;  // explicit durations are used as they are
    }
    const auto frames = decode_packets(packets, ivf->rate, *av1c);
    REQUIRE_OK(frames);
    REQUIRE(frames->size() == md5s.size());
    for (std::size_t i = 0; i < frames->size(); ++i) {
      CHECK(frame_md5((*frames)[i]) == md5s[i]);
      CHECK((*frames)[i].duration_ms == (1000 + ivf->rate / 2) / ivf->rate);
    }
    // Extradata given as bare OBUs works too.
    const auto bare = decode_packets(packets, ivf->rate, **header);
    REQUIRE_OK(bare);
    CHECK(bare->size() == md5s.size());
  }
}

TEST_CASE("codec_av1: Annex B and Section 5 .obu streams") {
  SUBCASE("Annex B, whole file as one packet") {
    const Bytes file = test::fixture("codec_av1/aom_annexb_64x48.obu");
    CHECK(av1::is_annexb(file));
    CHECK_FALSE(av1::is_section5(file));
    const std::vector<std::string> md5s = read_md5s("codec_av1/aom_annexb_64x48.md5");
    ir::Packet packet;
    packet.bytes = file;
    const auto frames = decode_packets({packet}, 1000);
    REQUIRE_OK(frames);
    REQUIRE(frames->size() == md5s.size());
    for (std::size_t i = 0; i < frames->size(); ++i) CHECK(frame_md5((*frames)[i]) == md5s[i]);
    // The converted stream is valid Section 5 with one temporal unit per frame.
    const auto section5 = av1::annexb_to_section5(file);
    REQUIRE_OK(section5);
    CHECK(av1::is_section5(*section5));
    const auto units = av1::split_temporal_units(*section5);
    REQUIRE_OK(units);
    CHECK(units->size() == md5s.size());
  }
  SUBCASE("Section 5 still picture (reduced still-picture header)") {
    const Bytes file = test::fixture("codec_av1/aom_still_80x60.obu");
    const std::vector<std::string> md5s = read_md5s("codec_av1/aom_still_80x60.md5");
    REQUIRE(md5s.size() == 1);
    const auto header = av1::find_sequence_header_obu(file);
    REQUIRE_OK(header);
    REQUIRE(header->has_value());
    const auto obus = av1::split_section5(**header);
    REQUIRE_OK(obus);
    const auto s = av1::parse_sequence_header((*obus)[0].payload);
    REQUIRE_OK(s);
    CHECK(s->still_picture);
    CHECK(s->reduced_still_picture_header);
    const auto picture = av1::decode_still(file);
    REQUIRE_OK(picture);
    CHECK(picture->frame.format == ir::PixelFormat::I420);
    CHECK(picture->frame.width == 80);
    CHECK(picture->frame.height == 60);
    CHECK(picture->bit_depth == 8);
    CHECK(frame_md5(picture->frame) == md5s[0]);
    // The same through the stream decoder.
    ir::Packet packet;
    packet.bytes = file;
    const auto frames = decode_packets({packet}, 1000);
    REQUIRE_OK(frames);
    REQUIRE(frames->size() == 1);
    CHECK(frame_md5(frames->front()) == md5s[0]);
    // AVIF-style: item data without sequence header + av1C.
    const auto av1c = av1::make_av1c(**header);
    REQUIRE_OK(av1c);
    const auto all = av1::split_section5(file);
    REQUIRE_OK(all);
    Bytes item;
    for (const av1::Obu& obu : *all) {
      if (obu.type == av1::ObuType::Frame || obu.type == av1::ObuType::FrameHeader ||
          obu.type == av1::ObuType::TileGroup) {
        av1::append_obu(item, obu);
      }
    }
    const auto from_item = av1::decode_still(item, *av1c);
    REQUIRE_OK(from_item);
    CHECK(frame_md5(from_item->frame) == md5s[0]);
    CHECK_STATUS(av1::decode_still(item), Status::Decode);  // no sequence header anywhere
  }
}

TEST_CASE("codec_av1: several temporal units in one packet") {
  const Bytes file = test::fixture("codec_av1/aom_10bit_420_96x64.ivf");
  const auto ivf = read_ivf(file);
  REQUIRE(ivf.has_value());
  const std::vector<std::string> md5s = read_md5s("codec_av1/aom_10bit_420_96x64.md5");
  ir::Packet packet;
  for (const IvfFrame& f : ivf->frames) packet.bytes.insert(packet.bytes.end(), f.data.begin(), f.data.end());
  const auto frames = decode_packets({packet}, 1000);
  REQUIRE_OK(frames);
  REQUIRE(frames->size() == md5s.size());
  for (std::size_t i = 0; i < frames->size(); ++i) CHECK(frame_md5((*frames)[i]) == md5s[i]);
}

TEST_CASE("codec_av1: colour metadata reaches the frames") {
  const Bytes file = test::fixture("codec_av1/aom_10bit_420_96x64.ivf");
  const auto ivf = read_ivf(file);
  REQUIRE(ivf.has_value());
  const auto picture = av1::decode_still(ivf->frames[0].data);
  REQUIRE_OK(picture);
  CHECK(picture->bit_depth == 10);
  CHECK_FALSE(picture->monochrome);
  CHECK(picture->frame.color.range == ir::ColorRange::Limited);
  // Unspecified matrix at SD size: BT.601 is assumed.
  CHECK(picture->color.matrix_coefficients == 2);
  CHECK(picture->frame.color.matrix == ir::ColorMatrix::BT601);
}
