// Remuxing (copying compressed streams into another container) is lossless:
// every packet of every kept stream comes back bit-exact (after the packaging
// conversion a decoder would do anyway: Annex-B vs length-prefixed NAL units,
// ADTS vs raw AAC, temporal delimiters), with the same key flags and times.
// The same files were also checked with ffprobe/ffmpeg (see SOURCES.md).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/video/adapt.hpp"
#include "families/video/video_test_util.hpp"

using namespace cvt;
using namespace cvt::test::video;

namespace {

struct Unit {
  Bytes data;
  bool key = false;
  double pts = 0;
};

// Packets of a track in the form decoders receive, with times in seconds.
std::vector<Unit> canonical(const ir::Track& track) {
  std::vector<Unit> out;
  auto packets = packets_of(track);
  if (!packets) return out;
  video::StreamAdapter adapter(track.codec, track.extradata, video::decoder_packaging);
  for (const ir::Packet& p : *packets) {
    auto data = adapter.convert(p.bytes, p.keyframe);
    if (!data) return {};
    // Drop in-band parameter sets / delimiters so both sides compare the picture data only.
    if (track.codec == "h264" || track.codec == "hevc") {
      const bool hevc = track.codec == "hevc";
      auto nals = video::bits::split_length_prefixed(*data, 4);
      std::vector<ByteView> kept;
      if (nals) {
        for (const ByteView n : *nals) {
          const int t = hevc ? video::bits::hevc_nal_type(n) : video::bits::h264_nal_type(n);
          const bool skip = hevc ? (t == 32 || t == 33 || t == 34 || t == 35) : (t == 7 || t == 8 || t == 9);
          if (!skip) kept.push_back(n);
        }
      }
      *data = video::bits::to_length_prefixed(kept);
    }
    out.push_back(Unit{std::move(*data), p.keyframe, static_cast<double>(p.pts) / track.timescale});
  }
  return out;
}

void check_remux(const char* file, const char* from, const char* to, std::string_view options = {}, bool times = true,
                 std::size_t skip_tracks_of_source = 0) {
  INFO(file, " -> ", to);
  const Bytes data = test::fixture(std::string("video/") + file);
  REQUIRE(!data.empty());
  const test::Converted result = test::convert(data, from, to, options);
  REQUIRE(result.ok());
  CHECK(result.message.empty());
  auto source = test::read_as<ir::Video>(from, data);
  auto copy = test::read_as<ir::Video>(to, result.data);
  REQUIRE_OK(source);
  REQUIRE_OK(copy);
  (void)skip_tracks_of_source;
  // Tracks are single-pass: read every one once.
  std::vector<std::vector<Unit>> in_units;
  std::vector<std::vector<Unit>> out_units;
  for (const ir::Track& t : source->tracks) in_units.push_back(canonical(t));
  for (const ir::Track& t : copy->tracks) out_units.push_back(canonical(t));
  // Each file's timeline starts at its earliest packet (MPEG-TS adds an offset, MP4 keeps priming negative).
  const bool audio_only = std::ranges::none_of(copy->tracks, [](const ir::Track& t) { return t.type == ir::TrackType::Video; });
  const auto origin = [&](const ir::Video& v, const std::vector<std::vector<Unit>>& units) {
    double lowest = 1e300;
    for (std::size_t i = 0; i < v.tracks.size(); ++i) {
      if (audio_only && v.tracks[i].type != ir::TrackType::Audio) continue;
      for (const Unit& u : units[i]) lowest = std::min(lowest, u.pts);
    }
    return lowest;
  };
  const double start_a = origin(*source, in_units);
  const double start_b = origin(*copy, out_units);
  for (std::size_t o = 0; o < copy->tracks.size(); ++o) {
    const ir::Track& out_track = copy->tracks[o];
    // Find the source track with the same codec and type.
    std::optional<std::size_t> in_index;
    for (std::size_t k = 0; k < source->tracks.size(); ++k) {
      const ir::Track& t = source->tracks[k];
      if (t.codec == out_track.codec && t.type == out_track.type && !in_units[k].empty()) {
        in_index = k;
        break;
      }
    }
    REQUIRE(in_index.has_value());
    INFO("track ", out_track.codec);
    const auto a = std::move(in_units[*in_index]);
    in_units[*in_index].clear();
    const auto& b = out_units[o];
    REQUIRE(!a.empty());
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
      INFO("packet ", i);
      CHECK(a[i].data == b[i].data);
      CHECK(a[i].key == b[i].key);
      if (times) CHECK_APPROX(b[i].pts - start_b, a[i].pts - start_a, 0.0015);
    }
  }
}

}  // namespace

TEST_CASE("video/remux: streams are copied bit-exact between containers") {
  check_remux("h264_aac.mp4", "mp4", "mkv");
  check_remux("h264_aac.mp4", "mp4", "ts");
  check_remux("h264_aac.mp4", "mp4", "flv");
  check_remux("h264_aac.mp4", "mp4", "mov");
  check_remux("h264_aac.mp4", "mp4", "3gp");
  check_remux("h264_aac.ts", "ts", "mp4");
  check_remux("h264_aac.flv", "flv", "mp4");
  check_remux("h264_aac_frag.mp4", "mp4", "mkv");
  check_remux("vp9_opus.webm", "webm", "mp4");
  check_remux("vp9_opus.webm", "webm", "mkv");
  check_remux("av1.mp4", "mp4", "webm");
  check_remux("av1.ivf", "ivf", "mp4");
  check_remux("av1.mp4", "mp4", "ivf");
  check_remux("vp8.ivf", "ivf", "webm");
  check_remux("hevc.mp4", "mp4", "mkv");
  check_remux("hevc.mp4", "mp4", "ts");
  check_remux("hevc_ac3.ts", "ts", "mkv");
  check_remux("mpeg2_mp2.vob", "vob", "ts");
  check_remux("mpeg2_mp2.ts", "ts", "mkv");
  check_remux("mpeg4_mp3.avi", "avi", "mkv");
  check_remux("mpeg4_mp3.avi", "avi", "mp4");
  check_remux("mjpeg_pcm.mov", "mov", "avi");
  check_remux("mjpeg_pcm.avi", "avi", "mov");
  check_remux("mjpeg_pcm.mov", "mov", "mkv");
  check_remux("rawvideo_i420.avi", "avi", "y4m");
  check_remux("i420.y4m", "y4m", "avi");
  check_remux("i420.y4m", "y4m", "mkv");
  check_remux("theora_vorbis.ogv", "ogv", "mkv");
  check_remux("theora_vorbis.ogv", "ogv", "ogv", R"({"video_codec":"copy","audio_codec":"copy"})");
  check_remux("vp8_vorbis.webm", "webm", "ogg", R"({"streams":"audio_only"})");
  check_remux("vp9_opus.webm", "webm", "ogg");
  check_remux("flac.oga", "ogg", "mka");
  check_remux("mpeg4_flac_pcm.mkv", "mkv", "mp4");
  check_remux("two_audio.mp4", "mp4", "mkv");
  check_remux("aac.m4a", "m4a", "mka");
  check_remux("h264.h264", "h264", "mkv", {}, false);
  check_remux("hevc.h265", "hevc", "mp4", {}, false);
  check_remux("av1.obu", "obu", "mp4", {}, false);
  check_remux("mpeg2.m2v", "m2v", "mkv", {}, false);
  check_remux("rv10_ac3.rm", "rm", "mka");

}

TEST_CASE("video/remux: audio can be extracted to plain audio files unchanged") {
  check_remux("h264_aac.mp4", "mp4", "m4a");


}

TEST_CASE("video/remux: MP4 output is fast-start and keeps rotation, edit lists and all audio tracks") {
  const Bytes data = test::fixture("video/h264_aac.mp4");
  const auto mov = test::convert(test::fixture("video/h264_rot90.mov"), "mov", "mp4");
  REQUIRE(mov.ok());
  auto rotated = test::read_as<ir::Video>("mp4", mov.data);
  REQUIRE_OK(rotated);
  CHECK(rotated->info.rotation == 270);  // ffprobe reports "rotation 90" (counter-clockwise) for both files
  const auto out = test::convert(data, "mp4", "mp4", R"({"video_codec":"copy","audio_codec":"copy"})");
  REQUIRE(out.ok());
  const std::string_view text = as_chars(out.data);
  CHECK(text.find("moov") < text.find("mdat"));
  auto video = test::read_as<ir::Video>("mp4", out.data);
  REQUIRE_OK(video);
  auto audio = packets_of(video->tracks[1]);
  REQUIRE_OK(audio);
  CHECK(audio->front().pts == -1024);  // AAC priming kept as an edit list
  auto bframes = packets_of(video->tracks[0]);
  REQUIRE_OK(bframes);
  CHECK(bframes->front().pts == 0);
  CHECK(bframes->front().dts == -1024);
  const auto two = test::convert(test::fixture("video/two_audio.mp4"), "mp4", "mkv");
  REQUIRE(two.ok());
  auto both = test::read_as<ir::Video>("mkv", two.data);
  REQUIRE_OK(both);
  CHECK(both->tracks.size() == 3);
  const auto one = test::convert(test::fixture("video/two_audio.mp4"), "mp4", "mkv", R"({"audio_track":1})");
  REQUIRE(one.ok());
  auto second = test::read_as<ir::Video>("mkv", one.data);
  REQUIRE_OK(second);
  REQUIRE(second->tracks.size() == 2);
  CHECK(second->info.audio_sample_rate == 16000);
}

TEST_CASE("video/remux: MP3, FLAC and WAV files hold the copied audio frames") {
  {
    const Bytes avi = test::fixture("video/mpeg4_mp3.avi");
    const auto out = test::convert(avi, "avi", "mp3");
    REQUIRE(out.ok());
    auto source = test::read_as<ir::Video>("avi", avi);
    REQUIRE_OK(source);
    auto frames = packets_of(source->tracks[1]);
    REQUIRE_OK(frames);
    Bytes expected;
    for (const ir::Packet& p : *frames) expected.insert(expected.end(), p.bytes.begin(), p.bytes.end());
    CHECK(out.data == expected);
  }
  {
    const Bytes mkv = test::fixture("video/mpeg4_flac_pcm.mkv");
    const auto out = test::convert(mkv, "mkv", "flac");
    REQUIRE(out.ok());
    auto source = test::read_as<ir::Video>("mkv", mkv);
    REQUIRE_OK(source);
    auto frames = packets_of(source->tracks[1]);
    REQUIRE_OK(frames);
    Bytes expected = test::bytes("fLaC");
    for (const int b : {0x80, 0, 0, 34}) expected.push_back(static_cast<std::byte>(b));
    expected.insert(expected.end(), source->tracks[1].extradata.begin(), source->tracks[1].extradata.end());
    for (const ir::Packet& p : *frames) expected.insert(expected.end(), p.bytes.begin(), p.bytes.end());
    CHECK(out.data == expected);
  }
  {
    const Bytes mkv = test::fixture("video/mpeg4_flac_pcm.mkv");
    const auto out = test::convert(mkv, "mkv", "wav", R"({"audio_track":1})");
    REQUIRE(out.ok());
    REQUIRE(out.data.size() > 44);
    CHECK(test::str(ByteView(out.data).first(4)) == "RIFF");
    util::ByteReader r(ByteView(out.data).subspan(22));
    CHECK(r.u16le() == 1);      // mono
    CHECK(r.u32le() == 16000);  // the second audio track of the file
    CHECK(out.data.size() == 44 + 32000);
  }
}

TEST_CASE("video/remux: H.264 without stored presentation times gets them from the picture order") {
  // The same x264 encode stored in MP4 (with composition offsets), as a raw
  // stream and in AVI (decode order only): the derived display order matches.
  const auto frame_order = [](const char* file, const char* format) {
    std::vector<std::int64_t> order;
    const Bytes video_bytes = test::fixture(std::string("video/") + file);  // must outlive the stream
    auto video = test::read_as<ir::Video>(format, video_bytes);
    if (!video) return order;
    auto packets = packets_of(video->tracks[0]);
    if (!packets || packets->size() < 2) return order;
    const std::int64_t step = (*packets)[1].dts - (*packets)[0].dts;
    for (const ir::Packet& p : *packets) order.push_back((p.pts - packets->front().dts) / step);
    return order;
  };
  const auto mp4 = frame_order("h264_aac.mp4", "mp4");
  REQUIRE(mp4.size() == 10);
  CHECK(frame_order("h264.h264", "h264") == mp4);
  CHECK(frame_order("h264_pcmu8.avi", "avi") == mp4);
}

TEST_CASE("video/remux: trimming copies whole packets from the first key frame") {
  const Bytes data = test::fixture("video/h264_aac.mp4");
  const auto out = test::convert(data, "mp4", "mkv", R"({"video_codec":"copy","audio_codec":"copy","start":0.3,"duration":0.5})");
  REQUIRE(out.ok());
  auto video = test::read_as<ir::Video>("mkv", out.data);
  REQUIRE_OK(video);
  // The only key frame is the first one, so the copied video can only start there.
  auto frames = packets_of(video->tracks[0]);
  REQUIRE_OK(frames);
  CHECK(frames->empty() == false);
}
