// Decoding real files: the built-in raw video and PCM codecs always, every
// other codec as soon as a codec family registers it (the tests skip cleanly
// until then). Plus the Video -> Image / Audio and Image -> Video bridges.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>

#include "codec/colorspace.hpp"
#include "families/video/video_test_util.hpp"
#include "support/metrics.hpp"

using namespace cvt;
using namespace cvt::test::video;

namespace {

std::vector<ir::VideoFrame> testsrc_reference() { return reference_i420("ref_testsrc_64x64_i420.yuv.gz", 64, 64); }

ir::Audio sine_reference() {
  ir::Audio audio;
  audio.sample_rate = 8000;
  audio.channels = 1;
  const Bytes packed = test::fixture("video/ref_sine440_8k_s16le.pcm.gz");
  auto raw = util::gzip_decompress(packed, 1u << 20);
  if (!raw) return audio;
  for (std::size_t i = 0; i + 1 < raw->size(); i += 2) {
    const auto v = static_cast<std::int16_t>(std::to_integer<std::uint16_t>((*raw)[i]) | (std::to_integer<std::uint16_t>((*raw)[i + 1]) << 8));
    audio.samples.push_back(static_cast<float>(v) / 32768.0f);
  }
  return audio;
}

const Bridge* find_bridge(Kind from, Kind to) {
  for (const Bridge& b : Registry::instance().bridges()) {
    if (b.from == from && b.to == to) return &b;
  }
  return nullptr;
}

Result<AnyIR> run_bridge(Kind from, Kind to, AnyIR value, std::string_view options = {}) {
  const Bridge* bridge = find_bridge(from, to);
  if (bridge == nullptr) return fail(Status::Internal, "bridge missing");
  CVT_TRY_ASSIGN(const Options parsed, Options::parse(options));
  Context ctx;
  CVT_TRY_ASSIGN(ctx.options, parsed.normalized(bridge->options));
  ctx.registry = &Registry::instance();
  return bridge->fn(std::move(value), ctx);
}

struct CodecFixture {
  const char* file;
  const char* format;
  const char* codec;
  double min_psnr;  // per frame, against the uncompressed test pattern
  std::size_t frames;
};

}  // namespace

TEST_CASE("video/decode: raw video in Y4M and AVI decodes to the exact test pattern") {
  const auto reference = testsrc_reference();
  REQUIRE(reference.size() == 10);
  for (const auto& [file, format] : {std::pair{"i420.y4m", "y4m"}, std::pair{"rawvideo_i420.avi", "avi"}}) {
    INFO(file);
    const Bytes video_bytes = test::fixture(std::string("video/") + file);  // must outlive the stream
    auto video = test::read_as<ir::Video>(format, video_bytes);
    REQUIRE_OK(video);
    auto drained = drain(*video);
    REQUIRE_OK(drained);
    REQUIRE(drained->frames.size() == 5);
    std::vector<ir::VideoFrame> first(reference.begin(), reference.begin() + 5);
    CHECK(test::min_psnr(drained->frames, first) == test::detail::infinity);
    CHECK(drained->frames[3].pts_ms == 300);
  }
  // 4:4:4 raw frames keep their layout.
  const Bytes i444_bytes = test::fixture("video/i444.y4m");  // must outlive the stream
  auto i444 = test::read_as<ir::Video>("y4m", i444_bytes);
  REQUIRE_OK(i444);
  auto frames = drain(*i444);
  REQUIRE_OK(frames);
  REQUIRE(frames->frames.size() == 10);
  CHECK(frames->frames[0].format == ir::PixelFormat::I444);
  CHECK(frames->frames[0].width == 32);
}

TEST_CASE("video/decode: PCM audio in AVI, MOV and Matroska decodes to the exact samples") {
  const ir::Audio reference = sine_reference();
  REQUIRE(reference.samples.size() == 8000);
  for (const auto& [file, format] : {std::pair{"mjpeg_pcm.avi", "avi"}, std::pair{"mjpeg_pcm.mov", "mov"}}) {
    INFO(file);
    const Bytes video_bytes = test::fixture(std::string("video/") + file);  // must outlive the stream
    auto video = test::read_as<ir::Video>(format, video_bytes);
    REQUIRE_OK(video);
    video->next_frame = nullptr;  // MJPEG has no decoder in this family
    auto drained = drain(*video);
    REQUIRE_OK(drained);
    ir::Audio got;
    got.sample_rate = 8000;
    got.channels = 1;
    got.samples = drained->samples;
    CHECK(test::snr_db(reference, got) == test::detail::infinity);
  }
}

TEST_CASE("video/decode: compressed codecs decode once their codec family registers them") {
  const CodecFixture cases[] = {
      {"h264_aac.mp4", "mp4", "h264", 30.0, 10},      {"hevc.mp4", "mp4", "hevc", 28.0, 10},
      {"av1.ivf", "ivf", "av1", 25.0, 10},            {"vp8.ivf", "ivf", "vp8", 25.0, 10},
      {"vp9.ivf", "ivf", "vp9", 25.0, 10},            {"mpeg4_mp3.avi", "avi", "mpeg4", 28.0, 10},
      {"mjpeg_pcm.mov", "mov", "mjpeg", 30.0, 10},    {"mpeg2.m2v", "m2v", "mpeg2video", 28.0, 10},
      {"theora_vorbis.ogv", "ogv", "theora", 28.0, 10}, {"h264_aac.ts", "mpegts", "h264", 30.0, 10},
      {"h264_vorbis.mkv", "mkv", "h264", 30.0, 10},   {"h264.h264", "h264", "h264", 30.0, 10},
  };
  const auto reference = testsrc_reference();
  std::size_t ran = 0;
  for (const CodecFixture& c : cases) {
    if (!Registry::instance().media().has_video_decoder(c.codec)) continue;
    INFO(c.file);
    const Bytes video_bytes = test::fixture(std::string("video/") + c.file);  // must outlive the stream
    auto video = test::read_as<ir::Video>(c.format, video_bytes);
    REQUIRE_OK(video);
    video->next_audio = nullptr;
    auto drained = drain(*video);
    REQUIRE_OK(drained);
    REQUIRE(drained->frames.size() == c.frames);
    std::vector<ir::VideoFrame> i420;
    for (const ir::VideoFrame& f : drained->frames) {
      // The reference holds the encoder's limited-range input samples: keep
      // the decoded frame's matrix (a matrix change would alter the samples)
      // and only bring full-range JPEG pictures to limited range.
      ir::ColorInfo target = f.color;
      target.range = ir::ColorRange::Limited;
      // RGB pictures (AV1 identity matrix) go back through FFmpeg's default BT.601.
      if (f.format == ir::PixelFormat::RGBA8) target = ir::ColorInfo{ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Center};
      auto converted = codec::convert_frame(f, ir::PixelFormat::I420, target);
      REQUIRE_OK(converted);
      i420.push_back(std::move(*converted));
    }
    // Presentation order on the 10 fps grid (containers may start later than 0;
    // the MPEG-4 AVI skips two frame slots, as its ffprobe table shows).
    for (std::size_t i = 1; i < drained->frames.size(); ++i) {
      const std::int64_t step = drained->frames[i].pts_ms - drained->frames[i - 1].pts_ms;
      CHECK(step > 0);
      CHECK(step % 100 == 0);
    }
    CHECK(test::min_psnr(i420, reference) >= c.min_psnr);
    ++ran;
  }
  if (ran == 0) SKIP("no compressed video decoder is registered in this build");
}

TEST_CASE("video/decode: compressed audio decodes once its codec family registers it") {
  const struct {
    const char* file;
    const char* format;
    const char* codec;
  } cases[] = {{"aac.m4a", "m4a", "aac"},           {"mpeg4_mp3.avi", "avi", "mp3"},  {"opus.ogg", "ogg", "opus"},
               {"flac.oga", "ogg", "flac"},         {"h264_vorbis.mkv", "mkv", "vorbis"}, {"mpeg2_mp2.ts", "mpegts", "mp2"},
               {"rv10_ac3.rm", "rm", "ac3"}};
  const ir::Audio reference = sine_reference();
  std::size_t ran = 0;
  for (const auto& c : cases) {
    if (!Registry::instance().media().has_audio_decoder(c.codec)) continue;
    INFO(c.file);
    const Bytes video_bytes = test::fixture(std::string("video/") + c.file);  // must outlive the stream
    auto video = test::read_as<ir::Video>(c.format, video_bytes);
    REQUIRE_OK(video);
    auto audio = run_bridge(Kind::Video, Kind::Audio, std::move(*video));
    REQUIRE_OK(audio);
    const ir::Audio& got = audio->get<ir::Audio>();
    // The fixtures hold a 440 Hz tone of amplitude 1/8 (ffmpeg sine filter).
    CHECK(test::aligned_snr_db(test::sine(got.sample_rate, got.channels, 440.0, 0.8, 0.125), got, 8192) >= 15.0);
    ++ran;
  }
  (void)reference;
  if (ran == 0) SKIP("no compressed audio decoder is registered in this build");
}

TEST_CASE("video/bridges: Video -> Audio decodes the sound of a video") {
  const Bytes video_bytes = test::fixture("video/mjpeg_pcm.avi");  // must outlive the stream
  auto video = test::read_as<ir::Video>("avi", video_bytes);
  REQUIRE_OK(video);
  auto audio = run_bridge(Kind::Video, Kind::Audio, std::move(*video));
  REQUIRE_OK(audio);
  const ir::Audio& got = audio->get<ir::Audio>();
  CHECK(got.sample_rate == 8000);
  CHECK(got.channels == 1);
  CHECK(test::snr_db(sine_reference(), got) == test::detail::infinity);
  // A clear message when the codec is missing.
  const Bytes mp4_bytes = test::fixture("video/h264_aac.mp4");  // must outlive the stream
  auto mp4 = test::read_as<ir::Video>("mp4", mp4_bytes);
  REQUIRE_OK(mp4);
  auto missing = run_bridge(Kind::Video, Kind::Audio, std::move(*mp4));
  if (!Registry::instance().media().has_audio_decoder("aac")) {
    REQUIRE(!missing);
    CHECK(missing.error().message.find("aac") != std::string::npos);
  }
}

TEST_CASE("video/bridges: Video -> Image gives an animation, a poster frame or a contact sheet") {
  const auto reference = testsrc_reference();
  auto rgba_of = [](const ir::VideoFrame& f) {
    auto rgba = codec::to_rgba(f);
    ir::Frame out;
    out.width = rgba->width;
    out.height = rgba->height;
    out.rgba = rgba->planes[0].data;
    return out;
  };
  {
    const Bytes video_bytes = test::fixture("video/i420.y4m");  // must outlive the stream
    auto video = test::read_as<ir::Video>("y4m", video_bytes);
    REQUIRE_OK(video);
    auto image = run_bridge(Kind::Video, Kind::Image, std::move(*video), R"({"fps":0})");
    REQUIRE_OK(image);
    const ir::Image& anim = image->get<ir::Image>();
    REQUIRE(anim.frames.size() == 5);
    CHECK(anim.frames[0].delay_ms == 100);
    ir::Image expected;
    ir::VideoFrame ref = reference[2];
    ref.color = ir::ColorInfo{ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Center};
    expected.frames.push_back(rgba_of(ref));
    ir::Image got;
    got.frames.push_back(anim.frames[2]);
    CHECK(test::psnr(expected, got) >= 45.0);
  }
  {
    const Bytes video_bytes = test::fixture("video/i420.y4m");  // must outlive the stream
    auto video = test::read_as<ir::Video>("y4m", video_bytes);
    REQUIRE_OK(video);
    auto image = run_bridge(Kind::Video, Kind::Image, std::move(*video), R"({"fps":20})");
    REQUIRE_OK(image);
    CHECK(image->get<ir::Image>().frames.size() == 10);  // 0.5 s at 20 fps
    CHECK(image->get<ir::Image>().frames[0].delay_ms == 50);
  }
  {
    const Bytes video_bytes = test::fixture("video/i420.y4m");  // must outlive the stream
    auto video = test::read_as<ir::Video>("y4m", video_bytes);
    REQUIRE_OK(video);
    auto image = run_bridge(Kind::Video, Kind::Image, std::move(*video), R"({"image_mode":"poster","poster_time":0.3,"width":32})");
    REQUIRE_OK(image);
    const ir::Image& poster = image->get<ir::Image>();
    REQUIRE(poster.frames.size() == 1);
    CHECK(poster.width() == 32);
    CHECK(poster.height() == 32);
  }
  {
    const Bytes video_bytes = test::fixture("video/i420.y4m");  // must outlive the stream
    auto video = test::read_as<ir::Video>("y4m", video_bytes);
    REQUIRE_OK(video);
    auto image = run_bridge(Kind::Video, Kind::Image, std::move(*video), R"({"image_mode":"contact_sheet","sheet_columns":2,"sheet_rows":2})");
    REQUIRE_OK(image);
    const ir::Image& sheet = image->get<ir::Image>();
    REQUIRE(sheet.frames.size() == 1);
    CHECK(sheet.width() == 2 * 64 + 3 * 4);
    CHECK(sheet.height() == 2 * 64 + 3 * 4);
  }
}

TEST_CASE("video/bridges: Image -> Video turns animations and stills into video") {
  ir::Image anim;
  for (std::uint32_t i = 0; i < 3; ++i) {
    ir::Frame f = test::gradient_frame(32, 16);
    f.delay_ms = 200;
    f.rgba[0] = static_cast<std::uint8_t>(i * 80);
    anim.frames.push_back(std::move(f));
  }
  auto video = run_bridge(Kind::Image, Kind::Video, anim);
  REQUIRE_OK(video);
  // Written as raw video in Matroska and read back.
  auto mkv = test::write_from("mkv", std::move(*video), R"({"video_codec":"rawvideo"})");
  REQUIRE_OK(mkv);
  auto back = test::read_as<ir::Video>("mkv", *mkv);
  REQUIRE_OK(back);
  auto drained = drain(*back);
  REQUIRE_OK(drained);
  REQUIRE(drained->frames.size() == 3);
  CHECK(drained->frames[1].pts_ms == 200);
  ir::Image still;
  still.frames.push_back(test::gradient_frame(16, 16));
  auto clip = run_bridge(Kind::Image, Kind::Video, still, R"({"fps":10,"still_duration":2})");
  REQUIRE_OK(clip);
  auto y4m = test::write_from("y4m", std::move(*clip));
  REQUIRE_OK(y4m);
  auto frames = test::read_as<ir::Video>("y4m", *y4m);
  REQUIRE_OK(frames);
  CHECK(frames->info.frame_count == 20);
}

TEST_CASE("video/builtin: raw video and PCM codecs are registered only once") {
  const codec::Registry& media = Registry::instance().media();
  CHECK(media.has_video_decoder("rawvideo"));
  CHECK(media.has_video_encoder("rawvideo"));
  CHECK(media.has_audio_decoder("pcm_s16le"));
  CHECK(media.has_audio_encoder("pcm_s16le"));
  CHECK(media.problems().empty());
  // rawvideo is this family's own; PCM is the lossless audio family's, and the
  // video family's PCM codecs are only fallbacks, replaced in either order.
  CHECK_FALSE(media.is_fallback("rawvideo", codec::CodecRole::VideoDecoder));
  CHECK_FALSE(media.is_fallback("pcm_s16le", codec::CodecRole::AudioDecoder));
  Registry video_only;
  register_video_family(video_only);
  CHECK(video_only.media().has_audio_decoder("pcm_s16le"));
  CHECK(video_only.media().is_fallback("pcm_s16le", codec::CodecRole::AudioDecoder));
  bool used = false;
  CHECK(video_only.media().register_audio_decoder("pcm_s16le", [&used]() -> std::unique_ptr<codec::AudioDecoder> {
    used = true;
    return nullptr;
  }));
  CHECK(video_only.media().problems().empty());
  (void)video_only.media().make_audio_decoder("pcm_s16le");
  CHECK(used);
}
