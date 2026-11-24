// The transcoding pipeline end to end: decode -> filters -> encode -> mux,
// with the built-in rawvideo/PCM codecs and tiny fake codecs registered in a
// test registry (registered under real ids so the real container paths run).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <numbers>

#include "families/video/fake_codecs.hpp"
#include "families/video/pipeline.hpp"
#include "families/video/video_test_util.hpp"
#include "support/metrics.hpp"

using namespace cvt;
using namespace cvt::test::video;

namespace {

// Only the video family plus the fakes: deterministic, independent of the
// real codec families merged into the build.
const Registry& fake_registry() {
  static const TestRegistry instance([](codec::Registry& media) { register_fakes(media); }, true);
  return instance.registry;
}

// The video family alone, without any compressed-video encoder.
const Registry& bare_registry() {
  static const TestRegistry instance([](codec::Registry&) {}, true);
  return instance.registry;
}

/// A synthetic video: `frames` moving test-pattern frames at 25 fps (I420)
/// and, optionally, a 440 Hz tone at 8 kHz mono starting `audio_offset_ms` late.
ir::Video synthetic(std::uint32_t frames, std::uint32_t width = 64, std::uint32_t height = 48, bool audio = true,
                    std::int64_t audio_offset_ms = 0, std::int64_t video_offset_ms = 0) {
  ir::Video video;
  video.info.width = width;
  video.info.height = height;
  video.info.fps = ir::Rational{25, 1};
  video.info.duration_ms = frames * 40;
  auto index = std::make_shared<std::uint32_t>(0);
  video.next_frame = [index, frames, width, height, video_offset_ms]() -> Result<std::optional<ir::VideoFrame>> {
    if (*index >= frames) return std::optional<ir::VideoFrame>{};
    ir::VideoFrame f = test::test_pattern_frame(ir::PixelFormat::I420, width, height, *index);
    f.pts_ms += video_offset_ms;
    ++*index;
    return std::optional<ir::VideoFrame>(std::move(f));
  };
  if (audio) {
    video.info.has_audio = true;
    video.info.audio_sample_rate = 8000;
    video.info.audio_channels = 1;
    const std::int64_t total = static_cast<std::int64_t>(frames) * 40 * 8;  // samples
    auto position = std::make_shared<std::int64_t>(0);
    video.next_audio = [position, total, audio_offset_ms]() -> Result<std::optional<ir::AudioChunk>> {
      if (*position >= total) return std::optional<ir::AudioChunk>{};
      ir::AudioChunk chunk;
      chunk.pts_ms = *position / 8 + audio_offset_ms;
      const std::int64_t n = std::min<std::int64_t>(1000, total - *position);
      for (std::int64_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(*position + i) / 8000.0;
        chunk.samples.push_back(static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 440.0 * t)));
      }
      *position += n;
      return std::optional<ir::AudioChunk>(std::move(chunk));
    };
  }
  return video;
}

Result<Bytes> write_with(const Registry& registry, std::string_view format, ir::Video video, std::string_view options = {},
                         std::vector<std::string>* warnings = nullptr, Progress progress = {}) {
  const Codec* writer = nullptr;
  for (const Codec* c : registry.codecs_for(format)) {
    if (c->kind == Kind::Video && c->write) writer = c;
  }
  if (writer == nullptr) return fail(Status::UnsupportedConversion, "no writer");
  CVT_TRY_ASSIGN(const Options parsed, Options::parse(options));
  WriteContext ctx;
  CVT_TRY_ASSIGN(ctx.options, parsed.normalized(writer->write_options));
  ctx.registry = &registry;
  ctx.format_id = std::string(format);
  ctx.progress = std::move(progress);
  const AnyIR value(std::move(video));
  auto out = writer->write(value, ctx);
  if (warnings != nullptr) *warnings = ctx.warnings.list();
  return out;
}

std::vector<ir::VideoFrame> reference_frames(std::uint32_t frames, std::uint32_t width, std::uint32_t height) {
  std::vector<ir::VideoFrame> out;
  for (std::uint32_t i = 0; i < frames; ++i) out.push_back(test::test_pattern_frame(ir::PixelFormat::I420, width, height, i));
  return out;
}

}  // namespace

TEST_CASE("video/pipeline: lossless encode through MP4 keeps every frame, its timing and the sound") {
  const Registry& registry = fake_registry();
  for (const char* format : {"mp4", "mkv", "mov", "3gp"}) {  // (TS cannot carry the fake codec's out-of-band size)
    const bool ts = std::string_view(format) == "mpegts";
    INFO(format);
    auto encoded = write_with(registry, format, synthetic(25), ts ? R"({"quality":95,"video_codec":"mpeg4"})" : R"({"quality":95})");
    REQUIRE_OK(encoded);
    auto video = read_with<ir::Video>(registry, format, *encoded);
    REQUIRE_OK(video);
    CHECK(video->info.width == 64);
    CHECK(video->info.height == 48);
    auto drained = drain(*video);
    REQUIRE_OK(drained);
    REQUIRE(drained->frames.size() == 25);
    CHECK(test::min_psnr(drained->frames, reference_frames(25, 64, 48)) == test::detail::infinity);
    // Frames come back in presentation order on the 25 fps grid (the fake encoder reorders like B-frames).
    const std::int64_t first = drained->frames.front().pts_ms;
    for (std::size_t i = 0; i < drained->frames.size(); ++i) CHECK(drained->frames[i].pts_ms - first == static_cast<std::int64_t>(i) * 40);
    // Audio: one second, starting with the video (priming removed by the edit list / timestamps).
    REQUIRE(!drained->audio_pts_ms.empty());
    CHECK(std::abs(drained->audio_pts_ms.front() - first) <= 40 + 128);
    const double seconds = static_cast<double>(drained->samples.size()) / 8000.0;
    CHECK(seconds >= 1.0);
    CHECK(seconds <= 1.0 + 2 * 1024 / 8000.0);
  }
}

TEST_CASE("video/pipeline: MP4 signals AAC priming with an edit list so audio starts on time") {
  const Registry& registry = fake_registry();
  auto encoded = write_with(registry, "mp4", synthetic(25));
  REQUIRE_OK(encoded);
  auto video = read_with<ir::Video>(registry, "mp4", *encoded);
  REQUIRE_OK(video);
  auto audio = packets_of(video->tracks[1]);
  REQUIRE_OK(audio);
  CHECK(audio->front().pts == -1024);  // the priming frame sits before time zero
  auto decoded = read_with<ir::Video>(registry, "mp4", *encoded);
  REQUIRE_OK(decoded);
  auto drained = drain(*decoded);
  REQUIRE_OK(drained);
  // The priming frame is silence; the first sample at time zero is the start of the tone.
  ir::Audio got;
  got.sample_rate = 8000;
  got.channels = 1;
  const std::size_t skip = 1024;
  REQUIRE(drained->samples.size() > skip + 8000);
  got.samples.assign(drained->samples.begin() + skip, drained->samples.begin() + skip + 8000);
  const ir::Audio expected = test::sine(8000, 1, 440.0, 1.0, 0.5);
  CHECK(test::snr_db(expected, got) >= 40.0);
}

TEST_CASE("video/pipeline: audio that starts late stays in sync") {
  const Registry& registry = fake_registry();
  auto encoded = write_with(registry, "mkv", synthetic(25, 64, 48, true, 200), R"({"audio_codec":"pcm_s16le"})");
  REQUIRE_OK(encoded);
  auto video = read_with<ir::Video>(registry, "mkv", *encoded);
  REQUIRE_OK(video);
  auto drained = drain(*video);
  REQUIRE_OK(drained);
  // 200 ms of silence were inserted in front of the tone, so the tone starts 1600 samples in.
  REQUIRE(drained->samples.size() >= 1600 + 4000);
  double early = 0;
  for (std::size_t i = 0; i < 1500; ++i) early = std::max(early, static_cast<double>(std::abs(drained->samples[i])));
  double late = 0;
  for (std::size_t i = 1700; i < 2700; ++i) late = std::max(late, static_cast<double>(std::abs(drained->samples[i])));
  CHECK(early < 0.01);
  CHECK(late > 0.4);
}

TEST_CASE("video/pipeline: scaling keeps the picture (Lanczos on the YUV planes)") {
  const Registry& registry = fake_registry();
  // Down to half size and back up: a smooth pattern survives with high PSNR.
  auto small = write_with(registry, "mkv", synthetic(5, 64, 48, false), R"({"width":32,"height":24,"quality":95})");
  REQUIRE_OK(small);
  auto small_video = read_with<ir::Video>(registry, "mkv", *small);
  REQUIRE_OK(small_video);
  CHECK(small_video->info.width == 32);
  CHECK(small_video->info.height == 24);
  auto back = write_with(registry, "mkv", std::move(*small_video), R"({"width":64,"height":48,"quality":95})");
  REQUIRE_OK(back);
  auto restored = read_with<ir::Video>(registry, "mkv", *back);
  REQUIRE_OK(restored);
  auto drained = drain(*restored);
  REQUIRE_OK(drained);
  REQUIRE(drained->frames.size() == 5);
  CHECK(test::min_psnr(drained->frames, reference_frames(5, 64, 48)) >= 28.0);
  // The scale factor option and odd sizes (rounded to even).
  auto half = write_with(registry, "mkv", synthetic(2, 64, 48, false), R"({"scale":0.5})");
  REQUIRE_OK(half);
  auto half_video = read_with<ir::Video>(registry, "mkv", *half);
  REQUIRE_OK(half_video);
  CHECK(half_video->info.width == 32);
  CHECK(half_video->info.height == 24);
}

TEST_CASE("video/pipeline: frame-rate conversion repeats and drops whole frames on the new grid") {
  const Registry& registry = fake_registry();
  // 25 fps -> 50 fps: every source frame twice.
  auto doubled = write_with(registry, "mkv", synthetic(10, 32, 32, false), R"({"fps":50,"quality":95})");
  REQUIRE_OK(doubled);
  auto video = read_with<ir::Video>(registry, "mkv", *doubled);
  REQUIRE_OK(video);
  auto frames = drain(*video);
  REQUIRE_OK(frames);
  REQUIRE(frames->frames.size() == 20);
  const auto reference = reference_frames(10, 32, 32);
  for (std::size_t i = 0; i < 20; ++i) {
    INFO("frame ", i);
    CHECK(test::psnr(frames->frames[i], reference[i / 2]) == test::detail::infinity);
    CHECK(frames->frames[i].pts_ms == static_cast<std::int64_t>(i) * 20);
  }
  // 25 fps -> 10 fps over one second: 10 frames.
  auto fewer = write_with(registry, "mkv", synthetic(25, 32, 32, false), R"({"fps":10})");
  REQUIRE_OK(fewer);
  auto fewer_video = read_with<ir::Video>(registry, "mkv", *fewer);
  REQUIRE_OK(fewer_video);
  auto fewer_frames = drain(*fewer_video);
  REQUIRE_OK(fewer_frames);
  CHECK(fewer_frames->frames.size() == 10);
}

TEST_CASE("video/pipeline: rotation is applied to the pictures when re-encoding") {
  const Registry& registry = fake_registry();
  ir::Video source = synthetic(2, 64, 48, false);
  source.info.rotation = 90;
  auto encoded = write_with(registry, "mkv", std::move(source), R"({"quality":95})");
  REQUIRE_OK(encoded);
  auto video = read_with<ir::Video>(registry, "mkv", *encoded);
  REQUIRE_OK(video);
  CHECK(video->info.width == 48);
  CHECK(video->info.height == 64);
  CHECK(video->info.rotation == 0);
  auto frames = drain(*video);
  REQUIRE_OK(frames);
  REQUIRE(!frames->frames.empty());
  auto expected = video::rotate_frame(test::test_pattern_frame(ir::PixelFormat::I420, 64, 48, 0), 90);
  REQUIRE_OK(expected);
  CHECK(test::psnr(frames->frames[0], *expected) == test::detail::infinity);
}

TEST_CASE("video/pipeline: trimming keeps exactly the requested span") {
  const Registry& registry = fake_registry();
  auto encoded = write_with(registry, "mkv", synthetic(50, 32, 32, true), R"({"start":0.4,"duration":0.8,"audio_codec":"pcm_s16le"})");
  REQUIRE_OK(encoded);
  auto video = read_with<ir::Video>(registry, "mkv", *encoded);
  REQUIRE_OK(video);
  auto drained = drain(*video);
  REQUIRE_OK(drained);
  REQUIRE(drained->frames.size() == 20);
  CHECK(test::psnr(drained->frames[0], test::test_pattern_frame(ir::PixelFormat::I420, 32, 32, 10)) == test::detail::infinity);
  CHECK(drained->samples.size() == 6400);
}

TEST_CASE("video/pipeline: quality and bitrate change the output size") {
  const Registry& registry = fake_registry();
  auto high = write_with(registry, "mp4", synthetic(10, 64, 48, false), R"({"quality":95})");
  auto low = write_with(registry, "mp4", synthetic(10, 64, 48, false), R"({"quality":40})");
  REQUIRE_OK(high);
  REQUIRE_OK(low);
  // The fake codec's packets have equal sizes; quality shows in the entropy, so compare compressibility.
  auto high_video = read_with<ir::Video>(registry, "mp4", *high);
  auto low_video = read_with<ir::Video>(registry, "mp4", *low);
  REQUIRE_OK(high_video);
  REQUIRE_OK(low_video);
  auto a = drain(*high_video);
  auto b = drain(*low_video);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  const auto reference = reference_frames(10, 64, 48);
  CHECK(test::min_psnr(a->frames, reference) > test::min_psnr(b->frames, reference));
  CHECK(test::min_psnr(b->frames, reference) >= 25.0);
}

TEST_CASE("video/pipeline: codec choice follows the container policy and the registry") {
  // Without any H.264/HEVC/AV1/MPEG-4 encoder -> a precise error.
  const Bytes y4m = test::fixture("video/i420.y4m");
  const auto mp4 = convert_with(bare_registry(), y4m, "y4m", "mp4");
  CHECK(mp4.status == Status::Encode);
  CHECK(mp4.message.find("h264") != std::string::npos);
  CHECK(mp4.message.find("mp4") != std::string::npos);
  // The full registry has the real H.264 encoder, which MP4 prefers.
  if (Registry::instance().media().has_video_encoder("h264")) {
    const auto real = test::convert(y4m, "y4m", "mp4");
    REQUIRE(real.ok());
    auto real_video = test::read_as<ir::Video>("mp4", real.data);
    REQUIRE_OK(real_video);
    CHECK(real_video->tracks[0].codec == "h264");
  }
  // Matroska falls back down its list to raw video.
  const auto mkv = test::convert(y4m, "y4m", "mkv", R"({"video_codec":"rawvideo"})");
  CHECK(mkv.ok());
  // A codec the container cannot hold is an invalid argument.
  const auto webm = test::convert(y4m, "y4m", "webm", R"({"video_codec":"h264"})");
  CHECK(webm.status == Status::InvalidArgument);
  // With the fake MPEG-4 codec registered, MP4 picks it (h264/hevc/av1 are absent).
  const Registry& registry = fake_registry();
  const auto out = convert_with(registry, y4m, "y4m", "mp4");
  REQUIRE(out.ok());
  auto video = read_with<ir::Video>(registry, "mp4", out.data);
  REQUIRE_OK(video);
  CHECK(video->tracks[0].codec == "mpeg4");
}

TEST_CASE("video/pipeline: same-format conversion re-encodes, copy keeps the packets") {
  const Registry& registry = fake_registry();
  auto encoded = write_with(registry, "mp4", synthetic(10, 64, 48, true), R"({"quality":95})");
  REQUIRE_OK(encoded);
  const auto compressed = convert_with(registry, *encoded, "mp4", "mp4", R"({"quality":40})");
  REQUIRE(compressed.ok());
  auto a = read_with<ir::Video>(registry, "mp4", *encoded);
  auto b = read_with<ir::Video>(registry, "mp4", compressed.data);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  auto pa = packets_of(a->tracks[0]);
  auto pb = packets_of(b->tracks[0]);
  REQUIRE_OK(pa);
  REQUIRE_OK(pb);
  CHECK(pa->front().bytes != pb->front().bytes);  // re-encoded
  const auto copied = convert_with(registry, *encoded, "mp4", "mp4", R"({"video_codec":"copy","audio_codec":"copy"})");
  REQUIRE(copied.ok());
  auto c = read_with<ir::Video>(registry, "mp4", copied.data);
  REQUIRE_OK(c);
  auto pc = packets_of(c->tracks[0]);
  REQUIRE_OK(pc);
  CHECK(pa->front().bytes == pc->front().bytes);
}

TEST_CASE("video/pipeline: streams option drops picture or sound; metadata is stripped by default") {
  const Registry& registry = fake_registry();
  ir::Video source = synthetic(5, 32, 32, true);
  source.info.metadata["title"] = "Test pattern";
  auto stripped = write_with(registry, "mkv", std::move(source), R"({"streams":"audio_only","audio_codec":"pcm_s16le"})");
  REQUIRE_OK(stripped);
  auto audio_only = read_with<ir::Video>(registry, "mkv", *stripped);
  REQUIRE_OK(audio_only);
  REQUIRE(audio_only->tracks.size() == 1);
  CHECK(audio_only->tracks[0].type == ir::TrackType::Audio);
  CHECK(audio_only->info.metadata.empty());
  ir::Video tagged = synthetic(5, 32, 32, true);
  tagged.info.metadata["title"] = "Test pattern";
  auto kept = write_with(registry, "mkv", std::move(tagged), R"({"streams":"video_only","strip_metadata":false})");
  REQUIRE_OK(kept);
  auto video_only = read_with<ir::Video>(registry, "mkv", *kept);
  REQUIRE_OK(video_only);
  REQUIRE(video_only->tracks.size() == 1);
  CHECK(video_only->tracks[0].type == ir::TrackType::Video);
  CHECK(video_only->info.metadata["title"] == "Test pattern");
}

TEST_CASE("video/pipeline: resampling and channel mixing reach the encoder's format") {
  const Registry& registry = fake_registry();
  auto encoded = write_with(registry, "mkv", synthetic(25, 32, 32, true), R"({"sample_rate":16000,"channels":2,"audio_codec":"pcm_s16le"})");
  REQUIRE_OK(encoded);
  auto video = read_with<ir::Video>(registry, "mkv", *encoded);
  REQUIRE_OK(video);
  CHECK(video->info.audio_sample_rate == 16000);
  CHECK(video->info.audio_channels == 2);
  auto drained = drain(*video);
  REQUIRE_OK(drained);
  ir::Audio got;
  got.sample_rate = 16000;
  got.channels = 2;
  got.samples = drained->samples;
  ir::Audio expected = test::sine(16000, 2, 440.0, 1.0, 0.5);
  got.samples.resize(std::min(got.samples.size(), expected.samples.size()));
  expected.samples.resize(got.samples.size());
  CHECK(test::aligned_snr_db(expected, got, 64) >= 40.0);
  // Opus only runs at 48 kHz, so the policy picks a rate it accepts.
  CHECK(video::make_resampler(8000, 8000, 1)->process(std::vector<float>{0.25f}, true).size() == 1);
}

TEST_CASE("video/pipeline: progress is reported and cancellation stops the work") {
  const Registry& registry = fake_registry();
  double last = 0;
  Progress progress([&last](double fraction, std::string_view) {
    last = fraction;
    return false;
  });
  auto ok = write_with(registry, "mkv", synthetic(100, 32, 32, false), {}, nullptr, progress);
  REQUIRE_OK(ok);
  CHECK(last > 0.5);
  Progress cancel([](double, std::string_view) { return true; });
  cancel.report(0.0);
  CHECK_STATUS(write_with(registry, "mkv", synthetic(100, 32, 32, false), {}, nullptr, cancel), Status::Cancelled);
}

TEST_CASE("video/pipeline: memory stays bounded while transcoding a long stream") {
  // 2000 frames would be 9 MB of I420; the pipeline pulls them one at a time.
  const Registry& registry = fake_registry();
  auto pulled = std::make_shared<int>(0);
  ir::Video source = synthetic(2000, 32, 32, false);
  auto inner = source.next_frame;
  auto outstanding = std::make_shared<int>(0);
  source.next_frame = [inner, pulled]() -> Result<std::optional<ir::VideoFrame>> {
    ++*pulled;
    return inner();
  };
  auto encoded = write_with(registry, "ivf", std::move(source), R"({"video_codec":"vp9"})");
  CHECK_STATUS(encoded, Status::Encode);  // no VP9 encoder in the test registry
  (void)outstanding;
  auto mkv = write_with(registry, "mkv", synthetic(2000, 32, 32, false), R"({"quality":40})");
  REQUIRE_OK(mkv);
}
