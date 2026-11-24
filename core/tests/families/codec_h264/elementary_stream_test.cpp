// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// The raw H.264 elementary stream format ("h264": .h264/.264 files) through
// the engine: detection, reading into ir::Video, writing, the "compress"
// route and hostile input.
#include <string>

#include "codec/ids.hpp"
#include "engine/detect.hpp"
#include "engine/planner.hpp"
#include "engine/registry.hpp"
#include "families/codec_h264/h264_test_support.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace std::string_view_literals;
namespace th = cvt::test::h264;

namespace {

std::string path(std::string_view name, std::string_view extension) {
  return "codec_h264/" + std::string(name) + std::string(extension);
}

Result<std::vector<codec::VideoFrame>> pull_all(ir::Video& video) {
  std::vector<codec::VideoFrame> frames;
  for (std::size_t guard = 0; guard < 10000; ++guard) {
    CVT_TRY_ASSIGN(std::optional<codec::VideoFrame> frame, video.next_frame());
    if (!frame) return frames;
    frames.push_back(std::move(*frame));
  }
  return fail(Status::Internal, "endless video");
}

ir::Video video_of(std::vector<codec::VideoFrame> frames, ir::Rational fps = {25, 1}) {
  ir::Video video;
  video.info.width = frames.empty() ? 0 : frames.front().width;
  video.info.height = frames.empty() ? 0 : frames.front().height;
  video.info.fps = fps;
  video.info.frame_count = frames.size();
  auto shared = std::make_shared<std::vector<codec::VideoFrame>>(std::move(frames));
  auto next = std::make_shared<std::size_t>(0);
  video.next_frame = [shared, next]() -> Result<std::optional<codec::VideoFrame>> {
    if (*next >= shared->size()) return std::optional<codec::VideoFrame>{};
    return std::optional<codec::VideoFrame>{(*shared)[(*next)++]};
  };
  return video;
}

}  // namespace

TEST_CASE("codec_h264/h264: registration") {
  const Registry& registry = Registry::instance();
  CHECK(registry.validate().empty());
  CHECK(registry.media().has_video_decoder(codec::id::h264));
  CHECK(registry.media().has_video_encoder(codec::id::h264));
  const FormatInfo* format = registry.find("h264");
  REQUIRE(format != nullptr);
  CHECK(format->category == Category::Video);
  CHECK(format->extensions.front() == "h264");
  CHECK(registry.has_reader("h264"));
  CHECK(registry.has_writer("h264"));
  const auto targets = list_targets(registry, "h264");
  CHECK(std::ranges::any_of(targets, [](const TargetInfo& target) { return target.format != nullptr && target.format->id == "h264" && target.same_format; }));
}

TEST_CASE("codec_h264/h264: detection from content") {
  const Registry& registry = Registry::instance();
  for (const char* name : {"baseline_cavlc_176x144", "main_cabac_bframes_176x144", "videotoolbox_main_bframes_176x144",
                           "unsupported_high10_32x32"}) {
    INFO(name);
    const auto detected = detect_format(registry, test::fixture(path(name, ".h264")), "");
    REQUIRE(detected.has_value());
    CHECK(detected->format->id == "h264");
  }
  const auto by_name = detect_format(registry, test::fixture(path("baseline_cavlc_176x144", ".h264")), "clip.264");
  REQUIRE(by_name.has_value());
  CHECK(by_name->format->id == "h264");
  // Neighbours that also start with a start code are not taken for H.264.
  const Bytes hevc = test::bytes("\x00\x00\x00\x01\x40\x01\x0c\x01\xff\xff\x01\x60\x00\x00\x03\x00"sv);
  const Bytes mpeg2 = test::bytes("\x00\x00\x01\xb3\x0b\x00\x90\x13\xff\xff\xe0\x18\x00\x00\x01\xb5"sv);
  const Bytes mpeg_ps = test::bytes("\x00\x00\x01\xba\x44\x00\x04\x00\x04\x01\x01\x89\xc3\xf8\x00\x00"sv);
  for (const Bytes& other : {hevc, mpeg2, mpeg_ps}) {
    const auto detected = detect_format(registry, other, "");
    CHECK((!detected.has_value() || detected->format->id != "h264"));
  }
}

TEST_CASE("codec_h264/h264: reading decodes every picture") {
  const Bytes data = test::fixture(path("main_cabac_bframes_176x144", ".h264"));
  auto video = test::read_as<ir::Video>("h264", data);
  REQUIRE_OK(video);
  CHECK(video->info.width == 176);
  CHECK(video->info.height == 144);
  CHECK(video->info.fps == ir::Rational{25, 1});
  CHECK(video->info.frame_count == std::optional<std::uint64_t>(16));
  const auto frames = pull_all(*video);
  REQUIRE_OK(frames);
  const std::vector<std::string> reference = th::read_frame_md5(test::fixture_text(path("main_cabac_bframes_176x144", ".md5")));
  REQUIRE(frames->size() == reference.size());
  for (std::size_t i = 0; i < frames->size(); ++i) {
    CHECK(th::frame_md5((*frames)[i]) == reference[i]);
    CHECK((*frames)[i].pts_ms == static_cast<std::int64_t>(i) * 40);
  }
  CHECK_STATUS(test::read_as<ir::Video>("h264", test::fixture(path("unsupported_high422_32x32", ".h264"))), Status::Decode);
  Limits tight;
  tight.max_pixels = 1000;
  CHECK_STATUS(test::read_as<ir::Video>("h264", data, {}, nullptr, tight), Status::Limit);
}

TEST_CASE("codec_h264/h264: writing, options and odd sizes") {
  const auto clip = th::natural_clip(160, 96, 12);
  const auto encode_with = [&](std::string_view options, std::vector<std::string>* warnings = nullptr) {
    return test::write_from("h264", video_of(clip), options, warnings);
  };
  const auto high = encode_with("");
  REQUIRE_OK(high);
  CHECK(cvt::h264::starts_with_start_code(*high));
  auto back = test::read_as<ir::Video>("h264", *high);
  REQUIRE_OK(back);
  const auto frames = pull_all(*back);
  REQUIRE_OK(frames);
  CHECK(test::min_psnr(clip, *frames) >= 39.0);

  const auto small = encode_with(R"({"quality":40})");
  REQUIRE_OK(small);
  CHECK(small->size() < high->size());
  const auto preset_small = encode_with(R"({"preset":"small"})");
  REQUIRE_OK(preset_small);
  CHECK(preset_small->size() < high->size());
  const auto all_intra = encode_with(R"({"keyframe_interval":1})");
  REQUIRE_OK(all_intra);
  CHECK(all_intra->size() > high->size());
  const auto fast = encode_with(R"({"speed":"fast"})");
  REQUIRE_OK(fast);
  const auto by_rate = encode_with(R"({"video_bitrate_kbps":200})");
  REQUIRE_OK(by_rate);
  for (const Bytes* stream : {&*small, &*preset_small, &*all_intra, &*fast, &*by_rate}) {
    auto video = test::read_as<ir::Video>("h264", *stream);
    REQUIRE_OK(video);
    const auto decoded = pull_all(*video);
    REQUIRE_OK(decoded);
    CHECK(decoded->size() == clip.size());
  }
  // Every keyframe repeats the parameter sets, so the stream can be cut there.
  std::size_t sps_count = 0;
  for (const ByteView nal : cvt::h264::split_annexb(*all_intra)) sps_count += cvt::h264::nal_type(nal) == cvt::h264::nal_sps;
  CHECK(sps_count == clip.size());

  // Odd sizes lose their last column/row, with a warning.
  std::vector<codec::VideoFrame> odd;
  for (std::uint32_t i = 0; i < 3; ++i) odd.push_back(th::natural_frame(65, 49, i));
  std::vector<std::string> warnings;
  const auto cropped = test::write_from("h264", video_of(odd), "", &warnings);
  REQUIRE_OK(cropped);
  CHECK(warnings.size() == 1);
  auto cropped_video = test::read_as<ir::Video>("h264", *cropped);
  REQUIRE_OK(cropped_video);
  CHECK(cropped_video->info.width == 64);
  CHECK(cropped_video->info.height == 48);

  // RGBA frames are converted.
  std::vector<codec::VideoFrame> rgba = {test::test_pattern_frame(ir::PixelFormat::RGBA8, 64, 32, 0)};
  CHECK_OK(test::write_from("h264", video_of(rgba)));
  CHECK_STATUS(test::write_from("h264", video_of({})), Status::Encode);
  std::vector<codec::VideoFrame> tiny = {test::test_pattern_frame(ir::PixelFormat::I420, 8, 8, 0)};
  CHECK_STATUS(test::write_from("h264", video_of(tiny)), Status::Encode);
}

TEST_CASE("codec_h264/h264: same-format compression through the engine") {
  const Bytes source = test::fixture(path("high_8x8_weighted_multiref_176x144", ".h264"));
  const test::Converted compressed = test::convert(source, "", "h264", R"({"quality":30})", "clip.h264");
  REQUIRE(compressed.ok());
  CHECK(compressed.format_id == "h264");
  CHECK(compressed.data.size() < source.size());
  auto video = test::read_as<ir::Video>("h264", compressed.data);
  REQUIRE_OK(video);
  const auto frames = pull_all(*video);
  REQUIRE_OK(frames);
  CHECK(frames->size() == 16);
  const test::Converted kept = test::convert(source, "h264", "h264", "", "clip.h264");
  REQUIRE(kept.ok());
}

TEST_CASE("codec_h264/h264: damaged files fail cleanly or decode what they can") {
  const Bytes data = test::fixture(path("baseline_cavlc_176x144", ".h264"));
  const auto read_everything = [](ByteView bytes) {
    auto video = test::read_as<ir::Video>("h264", bytes);
    if (!video) return;
    (void)pull_all(*video);
  };
  for (std::size_t length = 0; length < data.size(); length += data.size() / 40 + 1) {
    read_everything(ByteView(data).first(length));
  }
  for (std::uint32_t seed = 1; seed <= 16; ++seed) {
    Bytes damaged = data;
    const Bytes noise = test::random_bytes(16, seed);
    for (std::size_t k = 0; k + 1 < noise.size(); k += 2) {
      damaged[(std::to_integer<std::size_t>(noise[k]) * 61 + seed * 97) % damaged.size()] ^= noise[k + 1];
    }
    read_everything(damaged);
  }
  CHECK_STATUS(test::read_as<ir::Video>("h264", test::bytes("\x00\x00\x00\x01\x65\x88"sv)), Status::Decode);
  read_everything(test::random_bytes(4096, 5));
  (void)test::fuzz_reader<ir::Video>("h264", data);
}
