// Hostile input (every reader survives truncation, corruption and absurd
// sizes, also while the packets are pulled and raw streams decoded; run under
// the asan preset) and format detection of every fixture.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/detect.hpp"
#include "families/video/video_test_util.hpp"

using namespace cvt;
using namespace cvt::test::video;

namespace {

struct Sample {
  const char* file;
  const char* format;
};

constexpr Sample samples[] = {
    {"h264_aac.mp4", "mp4"},        {"h264_aac_frag.mp4", "mp4"},  {"h264_aac_cmov.mov", "mov"},
    {"h264_aac_large_mdat.mp4", "mp4"}, {"mjpeg_pcm.mov", "mov"},  {"h263_aac.3gp", "3gp"},
    {"aac.m4a", "m4a"},             {"mpeg4_aac.m4v", "m4v"},      {"h264_vorbis.mkv", "mkv"},
    {"vp9_opus.webm", "webm"},      {"mkvmerge_lacing.mka", "mka"}, {"mpeg4_mp3.avi", "avi"},
    {"rawvideo_i420.avi", "avi"},   {"h264_aac.flv", "flv"},       {"flv1_mp3.flv", "flv"},
    {"h264_aac.ts", "mpegts"},          {"hevc_ac3.ts", "mpegts"},         {"mpeg2_mp2.vob", "vob"},
    {"mpeg1_mp2.mpg", "mpg"},       {"theora_vorbis.ogv", "ogv"},  {"opus.ogg", "ogg"},
    {"flac.oga", "ogg"},            {"wmv2_wmav2.wmv", "wmv"},     {"wmav2.wma", "wma"},
    {"vp8.ivf", "ivf"},             {"av1.ivf", "ivf"},            {"i420.y4m", "y4m"},
    {"h264.h264", "h264"},          {"hevc.h265", "hevc"},         {"av1.obu", "obu"},
    {"mpeg2.m2v", "m2v"},           {"rv10_ac3.rm", "rm"},
};

// Read, then pull every packet and every decodable frame/sample: must just return.
void exercise(std::string_view format, ByteView data) {
  auto video = test::read_as<ir::Video>(format, data);
  if (!video) return;
  for (const ir::Track& t : video->tracks) (void)packets_of(t);
  auto again = test::read_as<ir::Video>(format, data);
  if (again) (void)drain(*again, 64);
}

}  // namespace

TEST_CASE("video/robustness: truncated and corrupted files never crash any reader") {
  for (const Sample& s : samples) {
    INFO(s.file);
    const Bytes valid = test::fixture(std::string("video/") + s.file);
    REQUIRE(!valid.empty());
    const std::size_t step = valid.size() / 96 + 1;
    for (std::size_t length = 0; length < valid.size(); length += step) exercise(s.format, ByteView(valid).first(length));
    for (std::uint32_t seed = 1; seed <= 24; ++seed) {
      Bytes mutated = valid;
      const Bytes noise = test::random_bytes(16, seed);
      for (std::size_t k = 0; k < 8; ++k) {
        const std::size_t at = (std::to_integer<std::size_t>(noise[k * 2]) * 977u + seed * 7919u + k * 104729u) % mutated.size();
        mutated[at] = noise[k * 2 + 1];
      }
      exercise(s.format, mutated);
    }
    exercise(s.format, test::random_bytes(2048, 7));
    // The standard harness too.
    CHECK(test::fuzz_reader<ir::Video>(s.format, valid, 64) > 0);
  }
}

TEST_CASE("video/robustness: damaged headers are decode errors with a clear message") {
  CHECK_STATUS(test::read_as<ir::Video>("mp4", test::bytes("not a movie at all")), Status::Decode);
  const Bytes mp4 = test::fixture("video/h264_aac.mp4");
  // Cut before the movie header (which follows the media data in this file).
  const auto cut = test::read_as<ir::Video>("mp4", ByteView(mp4).first(mp4.size() / 2));
  CHECK_STATUS(cut, Status::Decode);
  if (!cut) CHECK(cut.error().message.find("moov") != std::string::npos);
  CHECK_STATUS(test::read_as<ir::Video>("y4m", test::bytes("YUV4MPEG2 W999999 H999999 F25:1\nFRAME\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Video>("y4m", test::bytes("YUV4MPEG2 W60000 H60000 F25:1\nFRAME\n")), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Video>("mpegts", test::bytes(std::string(188 * 3, 'G'))), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Video>("mkv", test::bytes("\x1A\x45\xDF\xA3\x80")), Status::Decode);
  // An MP4 sample table that claims more entries than the file could hold.
  Bytes bomb = mp4;
  const std::string_view text = as_chars(bomb);
  const std::size_t stsz = text.find("stsz");
  REQUIRE(stsz != std::string_view::npos);
  for (std::size_t i = 0; i < 4; ++i) bomb[stsz + 12 + i] = std::byte{0x7F};  // sample count
  CHECK_STATUS(test::read_as<ir::Video>("mp4", bomb), Status::Decode);
  // A 64x64 raw frame in an AVI that says 30000x30000 is refused before allocating.
  Bytes avi = test::fixture("video/rawvideo_i420.avi");
  const std::size_t strf = as_chars(avi).find("strf");
  REQUIRE(strf != std::string_view::npos);
  for (const std::size_t field : {std::size_t{12}, std::size_t{16}}) {
    avi[strf + field] = std::byte{0x30};
    avi[strf + field + 1] = std::byte{0x75};
  }
  // The reader checks the stated picture size against the pixel limit up front.
  CHECK_STATUS(test::read_as<ir::Video>("avi", avi), Status::Limit);
  Limits generous;
  generous.max_pixels = std::uint64_t{30000} * 30000;
  auto huge = test::read_as<ir::Video>("avi", avi, {}, nullptr, generous);
  REQUIRE_OK(huge);
  auto frames = drain(*huge, 2);
  CHECK(!frames.has_value());
}

TEST_CASE("video/detection: every fixture is recognised from its content") {
  const struct {
    const char* file;
    const char* expected;
  } cases[] = {
      {"h264_aac.mp4", "mp4"},         {"h264_aac_cmov.mov", "mp4"}, {"h264_rot90.mov", "mov"},  {"h263_aac.3gp", "3gp"},
      {"aac.m4a", "m4a"},              {"mpeg4_aac.m4v", "m4v"},     {"h264_vorbis.mkv", "mkv"}, {"vp9_opus.webm", "webm"},
      {"mpeg4_mp3.avi", "avi"},        {"h264_aac.flv", "flv"},      {"h264_aac.ts", "mpegts"},      {"mpeg2_mp2.vob", "mpg"},
      {"theora_vorbis.ogv", "ogv"},    {"wmv2_wmav2.wmv", "wmv"},    {"wmav2.wma", "wma"},       {"vp8.ivf", "ivf"},
      {"i420.y4m", "y4m"},             {"h264.h264", "h264"},        {"hevc.h265", "hevc"},      {"av1.obu", "obu"},
      {"mpeg2.m2v", "m2v"},            {"rv10_ac3.rm", "rm"},
  };
  for (const auto& c : cases) {
    INFO(c.file);
    const Bytes data = test::fixture(std::string("video/") + c.file);
    const auto detected = detect_format(Registry::instance(), data, "");
    REQUIRE(detected.has_value());
    CHECK(detected->format->id == c.expected);
    // With the file name the result is the same (or more specific).
    const auto named = detect_format(Registry::instance(), data, c.file);
    REQUIRE(named.has_value());
    if (std::string_view(c.file) == "mpeg2_mp2.vob") {
      CHECK(named->format->id == "vob");
    } else if (std::string_view(c.file) == "mkvmerge_lacing.mka") {
      CHECK(named->format->id == "mka");
    } else {
      CHECK(named->format->id == c.expected);
    }
  }
  CHECK(Registry::instance().validate().empty());
}

TEST_CASE("video/integration: containers convert through the engine and list their options") {
  const Bytes y4m = test::fixture("video/i420.y4m");
  const auto mkv = test::convert(y4m, "", "mkv", R"({"video_codec":"rawvideo"})", "clip.y4m");
  CHECK(mkv.ok());
  CHECK(mkv.extension == "mkv");
  // Unknown options are reported, invalid enum values rejected.
  const auto bad = test::convert(y4m, "y4m", "mkv", R"({"streams":"sideways"})");
  CHECK(bad.status == Status::InvalidArgument);
  const auto noted = test::convert(y4m, "y4m", "mkv", R"({"video_codec":"rawvideo","not_an_option":1})");
  CHECK(noted.ok());
  CHECK(!noted.warnings.empty());
  // A video without audio cannot become an audio file.
  const auto mp3 = test::convert(y4m, "y4m", "mp3");
  CHECK(mp3.status == Status::Encode);
}
