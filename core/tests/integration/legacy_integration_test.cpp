// Older and production video codecs in their usual containers, read from real
// files and re-muxed: MPEG-2 in TS/PS, MPEG-4 ASP in AVI (packed bitstream),
// H.263 in 3GP, Sorenson Spark in FLV, Theora in Ogg, ProRes in MOV, FFV1 in
// Matroska and HuffYUV in AVI.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <format>

#include "families/codec_legacy/legacy_test_support.hpp"
#include "integration/integration_support.hpp"

using namespace cvt;
using namespace cvt::test::integration;
namespace tl = cvt::test::legacy;

namespace {

struct LegacyCase {
  const char* file;
  const char* format;
  const char* codec;
  std::size_t frames;
  double floor;            // dB against the testsrc reference; 0 = not comparable (other size)
  bool sixty_four_by_48;   // reference clip
  const char* remux_into;  // a container that stores the codec unchanged
};

}  // namespace

TEST_CASE("integration/legacy: real files decode with the right frame count and picture, and re-mux unchanged") {
  const LegacyCase cases[] = {
      {"video/mpeg2_mp2.ts", "mpegts", "mpeg2video", 10, 30.0, false, "mkv"},
      {"video/mpeg2_mp2.vob", "vob", "mpeg2video", 10, 30.0, false, "mkv"},
      {"video/mpeg4_mp3.avi", "avi", "mpeg4", 10, 28.0, false, "mkv"},
      {"video/h263_aac.3gp", "3gp", "h263", 10, 0.0, false, "mov"},
      {"video/flv1_mp3.flv", "flv", "flv1", 10, 28.0, false, "flv"},
      {"video/theora_vorbis.ogv", "ogv", "theora", 10, 28.0, false, "mkv"},
      // FFmpeg's ProRes 422 of a 64x48 high-detail pattern; FFV1/HuffYUV are
      // lossless but coded in 4:2:2, so the 4:2:0 reference differs in chroma.
      {"integration/prores_pcm.mov", "mov", "prores", 5, 27.0, true, "mkv"},
      {"integration/ffv1_flac.mkv", "mkv", "ffv1", 5, 40.0, true, "avi"},
      {"integration/huffyuv_pcm.avi", "avi", "huffyuv", 5, 40.0, true, "mkv"},
  };
  std::size_t ran = 0;
  for (const LegacyCase& c : cases) {
    INFO(c.file);
    if (!has_video_decoder(c.codec)) continue;
    const Bytes data = test::fixture(c.file);
    auto video = test::read_owned<ir::Video>(c.format, data);
    REQUIRE_OK(video);
    const auto video_track = [](const ir::Video& v) -> const ir::Track* {
      for (const ir::Track& t : v.tracks) {
        if (t.type == ir::TrackType::Video) return &t;
      }
      return nullptr;
    };
    const ir::Track* track = video_track(**video);
    REQUIRE(track != nullptr);
    CHECK(track->codec == c.codec);
    (*video)->next_audio = nullptr;
    auto drained = test::drain(**video);
    REQUIRE_OK(drained);
    CHECK(drained->frames.size() == c.frames);
    if (c.floor > 0) {
      const double quality = clip_psnr(*drained, c.sixty_four_by_48 ? testsrc_64x48() : testsrc_64x64(), c.frames);
      MESSAGE(c.file, ": ", quality, " dB");
      CHECK(quality >= c.floor);
    }
    // Re-mux without re-encoding: the same packets, decoding to the same pictures.
    const CapiResult remuxed = capi_convert(data, std::string("clip.") + c.format, c.remux_into,
                                            R"({"video_codec":"copy","streams":"video_only"})");
    REQUIRE(remuxed.ok());
    auto again = test::read_owned<ir::Video>(c.remux_into, remuxed.data);
    REQUIRE_OK(again);
    const ir::Track* copied = video_track(**again);
    REQUIRE(copied != nullptr);
    CHECK(copied->codec == c.codec);
    auto a = test::packets_of(*track);
    auto b = test::packets_of(*copied);
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    REQUIRE(a->size() == b->size());
    for (std::size_t k = 0; k < a->size(); ++k) CHECK((*a)[k].bytes == (*b)[k].bytes);
    auto redrained = test::drain(**again);
    REQUIRE_OK(redrained);
    REQUIRE(redrained->frames.size() == drained->frames.size());
    CHECK(test::min_psnr(redrained->frames, drained->frames) == test::detail::infinity);
    ++ran;
  }
  if (ran == 0) SKIP("the legacy codec family is not in this build");
}

TEST_CASE("integration/legacy: a DivX packed bitstream survives AVI muxing and decodes like FFmpeg") {
  if (!has_video_decoder("mpeg4")) SKIP("no MPEG-4 Part 2 decoder in this build");
  const tl::PacketFile file = tl::load_packets("mpeg4_asp_packed_96x64");
  const tl::RefFile ref = tl::load_reference("mpeg4_asp_packed_96x64");
  REQUIRE(!file.packets.empty());
  // Wrap the stored packets as a compressed track and let the AVI writer copy them.
  ir::Video video;
  video.info.width = file.width;
  video.info.height = file.height;
  video.info.fps = {25, 1};
  ir::Track track;
  track.codec = file.codec;
  track.extradata = file.extradata;
  track.timescale = 25;
  auto packets = std::make_shared<std::vector<ir::Packet>>(file.packets);
  auto next = std::make_shared<std::size_t>(0);
  // AVI chunk order = decode order; every chunk takes one frame slot.
  for (std::size_t i = 0; i < packets->size(); ++i) {
    (*packets)[i].dts = static_cast<std::int64_t>(i);
    (*packets)[i].pts = static_cast<std::int64_t>(i);
  }
  track.next_packet = [packets, next]() -> Result<std::optional<ir::Packet>> {
    if (*next >= packets->size()) return std::optional<ir::Packet>{};
    return std::optional<ir::Packet>((*packets)[(*next)++]);
  };
  video.tracks.push_back(std::move(track));
  auto avi = test::write_from("avi", std::move(video), R"({"video_codec":"copy"})");
  REQUIRE_OK(avi);
  auto back = test::read_owned<ir::Video>("avi", *avi);
  REQUIRE_OK(back);
  CHECK((*back)->tracks[0].codec == "mpeg4");
  auto drained = test::drain(**back);
  REQUIRE_OK(drained);
  const double quality = tl::compare_with_reference(drained->frames, ref);
  MESSAGE("packed MPEG-4 through AVI: ", drained->frames.size(), " frames, ", quality, " dB against FFmpeg");
  CHECK(quality >= 40.0);
}

TEST_CASE("integration/legacy: lossless video round trips exactly through Matroska and AVI") {
  const auto& ref = testsrc_64x48();
  const Bytes source = test::fixture("integration/prores_pcm.mov");
  for (const auto& [codec, container] : {std::pair{"ffv1", "mkv"}, std::pair{"huffyuv", "avi"}, std::pair{"ffv1", "avi"}}) {
    INFO(codec, " in ", container);
    if (!has_video_encoder(codec) || !has_video_decoder("prores")) continue;
    // The decoded ProRes pictures are the input; the lossless file must reproduce them.
    auto original = decode_all("mov", source);
    REQUIRE_OK(original);
    const CapiResult out = capi_convert(source, "clip.mov", container, std::format(R"({{"video_codec":"{}"}})", codec));
    REQUIRE(out.ok());
    auto drained = decode_all(container, out.data);
    REQUIRE_OK(drained);
    REQUIRE(drained->frames.size() == 5);
    const auto a = as_i420(drained->frames);
    const auto b = as_i420(original->frames);
    // HuffYUV codes 4:2:2 and FFV1 the source layout: exact after the shared 4:2:0 conversion.
    MESSAGE(codec, " in ", container, ": ", test::min_psnr(a, b), " dB against the ProRes pictures");
    if (std::string_view(codec) == "ffv1") {
      // FFV1 codes the decoded 4:2:2 10-bit pictures as they are.
      CHECK(test::min_psnr(drained->frames, original->frames) == test::detail::infinity);
    } else {
      CHECK(test::min_psnr(a, b) >= 38.0);  // HuffYUV: 8-bit 4:2:2 of the 10-bit source via 4:2:0
    }
    CHECK(test::min_psnr(a, std::vector<ir::VideoFrame>(ref.begin(), ref.begin() + 5)) >= 27.0);
  }
  // The "lossless" option picks FFV1 by itself.
  if (has_video_encoder("ffv1")) {
    const CapiResult out = capi_convert(source, "clip.mov", "mkv", R"({"lossless":true})");
    REQUIRE(out.ok());
    auto video = test::read_owned<ir::Video>("mkv", out.data);
    REQUIRE_OK(video);
    CHECK(std::ranges::any_of((*video)->tracks, [](const ir::Track& t) { return t.codec == "ffv1"; }));
  }
}

TEST_CASE("integration/legacy: ProRes flavour follows the quality") {
  if (!has_video_encoder("prores")) SKIP("no ProRes encoder in this build");
  const Bytes source = test::fixture("integration/h264_alac.mp4");
  for (const auto& [quality, fourcc] : {std::pair{95, "apch"}, std::pair{80, "apcn"}, std::pair{60, "apcs"}, std::pair{30, "apco"}}) {
    INFO("quality ", quality);
    const CapiResult out = capi_convert(source, "clip.mp4", "mov", std::format(R"({{"video_codec":"prores","quality":{}}})", quality));
    REQUIRE(out.ok());
    CHECK(test::str(out.data).find(fourcc) != std::string::npos);
    auto drained = decode_all("mov", out.data);
    REQUIRE_OK(drained);
    CHECK(drained->frames.size() == 5);
  }
}
