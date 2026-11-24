// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// The raw "hevc" stream format, bitstream helpers and hostile input.
#include <algorithm>
#include <climits>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "families/codec_hevc/hevc_test_util.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

#include "engine/detect.hpp"
#include "families/codec_hevc/hevc_api.hpp"
#include "families/codec_hevc/hevc_internal.hpp"

using namespace cvt;
using namespace cvt::test::hevc_util;
using namespace std::string_view_literals;

namespace {

/// Read a stream and pull every frame; the result is only that it returned.
void decode_all(ByteView data) {
  auto video = test::read_as<ir::Video>("hevc", data);
  if (!video || !video->next_frame) return;
  for (int guard = 0; guard < 400; ++guard) {
    auto frame = video->next_frame();
    if (!frame || !*frame) return;
  }
}

}  // namespace

TEST_CASE("codec_hevc/stream: parameter sets of real streams") {
  SUBCASE("x265 Main 10 with VUI") {
    const Bytes data = test::fixture("codec_hevc/main10_96x64.h265");
    std::optional<hevc::SequenceInfo> sps;
    for (const ByteView nal : hevc::split_annexb(data)) {
      if (hevc::nal_type(nal) == 33) sps = *hevc::parse_sps(nal);
    }
    REQUIRE(sps.has_value());
    CHECK(sps->ptl.profile_idc == 2);
    CHECK(sps->width == 96);
    CHECK(sps->height == 64);
    CHECK(sps->bit_depth_luma == 10);
    CHECK(sps->chroma_format_idc == 1);
    CHECK(hevc::pixel_format(*sps) == ir::PixelFormat::I420P10);
  }
  SUBCASE("conformance window of a cropped stream") {
    const Bytes data = test::fixture("codec_hevc/main_98x66_cropped.h265");
    for (const ByteView nal : hevc::split_annexb(data)) {
      if (hevc::nal_type(nal) != 33) continue;
      const auto sps = hevc::parse_sps(nal);
      REQUIRE_OK(sps);
      CHECK(sps->coded_width > 98);
      CHECK(sps->width == 98);
      CHECK(sps->height == 66);
    }
  }
  SUBCASE("VideoToolbox hvcC: colour description and SPS details") {
    const auto file = read_sample_file(test::fixture("codec_hevc/vt_main10_176x144.hvcs"));
    REQUIRE(file.has_value());
    const auto config = hevc::parse_hvcc(file->hvcc);
    REQUIRE_OK(config);
    CHECK(config->bit_depth_luma == 10);
    CHECK(config->chroma_format_idc == 1);
    bool found = false;
    for (const Bytes& nal : config->nals) {
      if (hevc::nal_type(nal) != 33) continue;
      const auto sps = hevc::parse_sps(nal);
      REQUIRE_OK(sps);
      found = true;
      CHECK(sps->width == 176);
      CHECK(sps->height == 144);
      CHECK(sps->vui_present);
      // The colour description of a BT.709 source as written by VideoToolbox.
      CHECK(sps->matrix_coefficients == 1);
      CHECK(hevc::color_info(*sps).matrix == ir::ColorMatrix::BT709);
    }
    CHECK(found);
    // Rebuilding the record from its own NAL units keeps profile and arrays.
    std::vector<ByteView> nals;
    for (const Bytes& nal : config->nals) nals.emplace_back(nal);
    const auto rebuilt = hevc::build_hvcc(nals);
    REQUIRE_OK(rebuilt);
    const auto again = hevc::parse_hvcc(*rebuilt);
    REQUIRE_OK(again);
    CHECK(again->ptl == config->ptl);
    CHECK(again->nals == config->nals);
  }
}

TEST_CASE("codec_hevc/stream: Annex-B and length-prefixed conversions") {
  const Bytes data = test::fixture("codec_hevc/main_128x96_ctu32_wpp.h265");
  const std::vector<ByteView> nals = hevc::split_annexb(data);
  REQUIRE(nals.size() > 3);
  for (const unsigned size : {1u, 2u, 4u}) {
    const auto prefixed = hevc::join_length_prefixed(std::vector<ByteView>(nals.begin(), nals.begin() + 3), size);
    REQUIRE_OK(prefixed);  // parameter sets are small enough even for 1-byte lengths
    const auto back = hevc::split_length_prefixed(*prefixed, size);
    REQUIRE_OK(back);
    CHECK(back->size() == 3);
  }
  const auto prefixed = hevc::annexb_to_length_prefixed(data, 4);
  REQUIRE_OK(prefixed);
  const auto annexb = hevc::length_prefixed_to_annexb(*prefixed, 4);
  REQUIRE_OK(annexb);
  CHECK(hevc::split_annexb(*annexb).size() == nals.size());
  CHECK_STATUS(hevc::split_length_prefixed(ByteView(*prefixed).first(prefixed->size() - 1), 4), Status::Decode);
  CHECK_STATUS(hevc::split_length_prefixed(*prefixed, 3), Status::Decode);
  // 3- and 4-byte start codes, leading garbage and trailing zero bytes.
  const Bytes mixed = test::bytes("\x12\x34\x00\x00\x01\x40\x01\xAA\x00\x00\x00\x00\x01\x42\x01\xBB\x00\x00"sv);
  const std::vector<ByteView> parts = hevc::split_annexb(mixed);
  REQUIRE(parts.size() == 2);
  CHECK(hevc::nal_type(parts[0]) == 32);
  CHECK(parts[0].size() == 3);
  CHECK(hevc::nal_type(parts[1]) == 33);
  CHECK(parts[1].size() == 3);
  // Emulation prevention bytes.
  CHECK(hevc::unescape_rbsp(test::bytes("\x00\x00\x03\x01\x00\x00\x03"sv)) == test::bytes("\x00\x00\x01\x00\x00"sv));
}

TEST_CASE("codec_hevc/stream: access units and presentation order") {
  const Bytes data = test::fixture("codec_hevc/main_176x144_ctu64_bframes.h265");
  auto video = test::read_as<ir::Video>("hevc", data);
  REQUIRE_OK(video);
  CHECK(video->info.width == 176);
  CHECK(video->info.height == 144);
  CHECK(video->info.frame_count == std::optional<std::uint64_t>{20});
  CHECK(video->info.fps == (ir::Rational{25, 1}));
  REQUIRE(video->tracks.size() == 1);
  ir::Track& track = video->tracks.front();
  CHECK(track.codec == "hevc");
  REQUIRE_OK(hevc::parse_hvcc(track.extradata));
  // The packets are in decode order; their PTS sort into 0, 1, 2, ... frames.
  std::vector<std::int64_t> pts;
  std::optional<std::int64_t> last_dts;
  bool reordered = false;
  while (true) {
    auto packet = track.next_packet();
    REQUIRE_OK(packet);
    if (!*packet) break;
    CHECK((*packet)->dts <= (*packet)->pts);
    if (last_dts) CHECK((*packet)->dts > *last_dts);
    last_dts = (*packet)->dts;
    if (!pts.empty() && (*packet)->pts < pts.back()) reordered = true;
    pts.push_back((*packet)->pts);
    CHECK(hevc::split_length_prefixed((*packet)->bytes, 4).has_value());
  }
  CHECK(reordered);  // B-frames
  std::sort(pts.begin(), pts.end());
  REQUIRE(pts.size() == 20);
  for (std::size_t i = 0; i < pts.size(); ++i) CHECK(pts[i] == static_cast<std::int64_t>(i) * 3600);
  // Those packets decode to the reference pictures.
  const std::vector<std::string> expected = reference_md5("main_176x144_ctu64_bframes");
  auto again = test::read_as<ir::Video>("hevc", data);
  REQUIRE_OK(again);
  std::vector<ir::Packet> packets;
  while (true) {
    auto packet = again->tracks.front().next_packet();
    REQUIRE_OK(packet);
    if (!*packet) break;
    packets.push_back(std::move(**packet));
  }
  codec::CodecParams params;
  params.extradata = again->tracks.front().extradata;
  params.timescale = again->tracks.front().timescale;
  const auto frames = decode_packets(params, packets);
  REQUIRE_OK(frames);
  REQUIRE(frames->size() == expected.size());
  for (std::size_t i = 0; i < frames->size(); ++i) {
    CHECK(frame_md5((*frames)[i]) == expected[i]);
    CHECK((*frames)[i].pts_ms == static_cast<std::int64_t>(i) * 40);
  }
}

TEST_CASE("codec_hevc/stream: detection") {
  const Bytes data = test::fixture("codec_hevc/main_64x64_ctu16_opengop.h265");
  const auto detected = detect_format(Registry::instance(), data, "");
  REQUIRE(detected.has_value());
  CHECK(detected->format->id == "hevc");
  for (const char* name : {"clip.h265", "clip.hevc", "clip.265"}) {
    const auto by_name = detect_format(Registry::instance(), data, name);
    REQUIRE(by_name.has_value());
    CHECK(by_name->format->id == "hevc");
  }
  // An H.264 stream (SPS NAL type 7) is not HEVC.
  const Bytes h264 = test::bytes("\x00\x00\x00\x01\x67\x42\x00\x1e\x95\xa8\x28\x0f\x64\x00\x00\x00\x01\x68\xce\x38\x80"sv);
  const auto other = detect_format(Registry::instance(), h264, "");
  CHECK((!other.has_value() || other->format->id != "hevc"));
}

TEST_CASE("codec_hevc/stream: compress hevc to hevc through the engine") {
  const Bytes source = test::fixture("codec_hevc/main_176x144_ctu64_bframes.h265");
  const auto reference = decode_stream(source);
  REQUIRE_OK(reference);
  std::size_t previous = SIZE_MAX;
  for (const int quality : {95, 75, 40}) {
    INFO("quality ", quality);
    const test::Converted out = test::convert(source, "", "hevc", std::format(R"({{"quality":{}}})", quality), "in.h265");
    REQUIRE(out.ok());
    CHECK(out.format_id == "hevc");
    CHECK(out.data.size() < previous);
    previous = out.data.size();
    const auto decoded = decode_stream(out.data);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->size() == reference->size());
    const double floor = quality >= 90 ? 36.0 : (quality >= 70 ? 32.0 : 27.0);
    CHECK(test::min_psnr(*reference, *decoded) >= floor);
    // Parameter sets lead the stream, so it starts with a VPS.
    CHECK(hevc::nal_type(hevc::split_annexb(out.data).front()) == 32);
  }
}

TEST_CASE("codec_hevc/stream: writer options") {
  const Bytes source = test::fixture("codec_hevc/main_64x64_ctu16_opengop.h265");
  const auto count_keyframes = [](ByteView data) {
    std::size_t idr = 0;
    for (const ByteView nal : hevc::split_annexb(data)) idr += hevc::is_irap(hevc::nal_type(nal)) ? 1 : 0;
    return idr;
  };
  SUBCASE("keyframe_interval") {
    const Bytes every = test::convert(source, "hevc", "hevc", R"({"keyframe_interval":1})").data;
    const Bytes rare = test::convert(source, "hevc", "hevc", R"({"keyframe_interval":0})").data;
    CHECK(count_keyframes(every) == 12);
    CHECK(count_keyframes(rare) == 1);
    CHECK(every.size() > rare.size());
  }
  SUBCASE("encoder_speed") {
    for (const char* speed : {"fast", "balanced", "slow"}) {
      const test::Converted out = test::convert(source, "hevc", "hevc", std::format(R"({{"encoder_speed":"{}"}})", speed));
      CHECK(out.ok());
      CHECK(decode_stream(out.data).has_value());
    }
  }
  SUBCASE("video_bitrate_kbps") {
    const Bytes small = test::convert(source, "hevc", "hevc", R"({"video_bitrate_kbps":20})").data;
    const Bytes large = test::convert(source, "hevc", "hevc", R"({"video_bitrate_kbps":400})").data;
    CHECK(!small.empty());
    CHECK(small.size() < large.size());
  }
  SUBCASE("preset") {
    const Bytes high = test::convert(source, "hevc", "hevc", R"({"preset":"high"})").data;
    const Bytes small = test::convert(source, "hevc", "hevc", R"({"preset":"small"})").data;
    CHECK(small.size() < high.size());
  }
}

TEST_CASE("codec_hevc/stream: limits and cancellation") {
  const Bytes data = test::fixture("codec_hevc/main_176x144_ctu64_bframes.h265");
  Limits limits;
  limits.max_pixels = 100 * 100;
  CHECK_STATUS(test::read_as<ir::Video>("hevc", data, {}, nullptr, limits), Status::Limit);

  const Codec* reader = test::find_reader("hevc", Kind::Video);
  REQUIRE(reader != nullptr);
  ReadContext ctx;
  ctx.input = data;
  ctx.registry = &Registry::instance();
  ctx.progress = Progress([](double, std::string_view) { return true; });  // cancel at the first report
  auto value = reader->read(ctx);
  REQUIRE_OK(value);
  ir::Video video = std::move(*value).get<ir::Video>();
  CHECK_STATUS(video.next_frame(), Status::Cancelled);
}

TEST_CASE("codec_hevc/stream: damaged input fails cleanly") {
  CHECK_STATUS(test::read_as<ir::Video>("hevc", test::bytes("not video")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Video>("hevc", Bytes{}), Status::Decode);
  // An SPS claiming 65535 x 65535 pixels.
  const Bytes huge = test::bytes(
      "\x00\x00\x00\x01\x42\x01\x01\x01\x60\x00\x00\x03\x00\x90\x00\x00\x03\x00\x00\x03\x00\x5d\xa0\x00\x00\x40\x00\x00\x40\x00\x03\x00\x00"sv);
  CHECK(!test::read_as<ir::Video>("hevc", huge).has_value());
  CHECK_STATUS(hevc::parse_hvcc(test::bytes("\x01\x02\x03"sv)), Status::Decode);
  CHECK_STATUS(hevc::parse_sps(test::bytes("\x42\x01"sv)), Status::Decode);

  // Packets fed straight to the decoder: garbage never crashes it.
  auto decoder = Registry::instance().media().make_video_decoder(codec::id::hevc);
  REQUIRE(decoder != nullptr);
  REQUIRE_OK(decoder->configure({}));
  for (std::uint32_t seed = 1; seed <= 16; ++seed) {
    ir::Packet packet;
    packet.bytes = test::random_bytes(64 + seed * 13, seed);
    (void)decoder->send_packet(packet);
    while (true) {
      auto frame = decoder->receive_frame();
      if (!frame || !*frame) break;
    }
  }
  (void)decoder->flush();
}

TEST_CASE("codec_hevc/stream: truncated and corrupted streams never crash") {
  for (const char* name : {"main_64x64_ctu16_opengop", "main10_96x64", "rext422p10_96x64", "kvz_160x128_tiles",
                           "main_128x96_scaling_weighted"}) {
    INFO("fixture ", name);
    const Bytes valid = test::fixture(std::string("codec_hevc/") + name + ".h265");
    // Truncations (every 97th byte) with full decoding.
    for (std::size_t length = 0; length < valid.size(); length += 97) decode_all(ByteView(valid).first(length));
    // Deterministic corruptions after the parameter sets.
    for (std::uint32_t seed = 1; seed <= 24; ++seed) {
      Bytes mutated = valid;
      const Bytes noise = test::random_bytes(16, seed);
      for (std::size_t k = 0; k < 6; ++k) {
        const std::size_t position =
            (std::to_integer<std::size_t>(noise[k * 2]) * 131u + seed * 7919u + k * 104729u) % mutated.size();
        mutated[position] = noise[k * 2 + 1];
      }
      decode_all(mutated);
    }
  }
  // Corrupted samples with a valid hvcC, through the decoder interface.
  const auto file = read_sample_file(test::fixture("codec_hevc/vt_main_160x96.hvcs"));
  REQUIRE(file.has_value());
  for (std::uint32_t seed = 1; seed <= 8; ++seed) {
    codec::CodecParams params;
    params.extradata = file->hvcc;
    std::vector<ir::Packet> packets;
    for (const Sample& sample : file->samples) {
      ir::Packet packet;
      packet.bytes = sample.bytes;
      const Bytes noise = test::random_bytes(4, seed + static_cast<std::uint32_t>(packets.size()) * 31u);
      const std::size_t at = (std::to_integer<std::size_t>(noise[0]) * 257u + std::to_integer<std::size_t>(noise[1])) %
                             packet.bytes.size();
      packet.bytes[at] = noise[2];
      if (seed % 3 == 0) packet.bytes.resize(packet.bytes.size() / 2);  // truncated samples too
      packets.push_back(std::move(packet));
    }
    (void)decode_packets(params, packets);
  }
  // A damaged configuration record.
  codec::CodecParams params;
  params.extradata = test::bytes("\x01\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x05"sv);
  auto decoder = Registry::instance().media().make_video_decoder(codec::id::hevc);
  CHECK(!decoder->configure(params).has_value());
}

TEST_CASE("codec_hevc/stream: registry") {
  CHECK(Registry::instance().media().has_video_decoder(codec::id::hevc));
  CHECK(Registry::instance().media().has_video_encoder(codec::id::hevc));
  CHECK(Registry::instance().media().problems().empty());
  CHECK(Registry::instance().validate().empty());
}
