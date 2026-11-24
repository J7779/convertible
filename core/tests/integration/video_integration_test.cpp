// Cross-family conversions of real video files through the public C ABI:
// containers x codecs, transcodes, remuxing, filters and the "compress" path,
// with every output decoded again and checked by content.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>

#include "integration/integration_support.hpp"
#include "util/fs.hpp"

using namespace cvt;
using namespace cvt::test::integration;
using cvt::test::Drained;

namespace {

// Converts, checks the conversion succeeded and that the result decodes to
// `frames` pictures of at least `floor` dB against the reference clip.
Bytes convert_and_check(const Bytes& input, std::string_view filename, std::string_view target, std::string_view options,
                        const std::vector<ir::VideoFrame>& reference, std::size_t frames, double floor) {
  INFO(filename, " -> ", target, " ", options);
  const CapiResult out = capi_convert(input, filename, target, options);
  REQUIRE(out.ok());
  CHECK(out.format == target);
  auto drained = decode_all(target, out.data);
  REQUIRE_OK(drained);
  const double quality = clip_psnr(*drained, reference, frames);
  MESSAGE(filename, " -> ", target, ": ", out.data.size(), " bytes, ", quality, " dB");
  CHECK(quality >= floor);
  return out.data;
}

}  // namespace

TEST_CASE("integration/video: a clip travels MP4 -> MKV -> MOV -> WebM -> AVI -> TS -> MP4") {
  for (const char* codec : {"h264", "vp9", "mjpeg"}) {
    if (!has_video_encoder(codec) || !has_video_decoder(codec)) SKIP(codec, " is not available in this build");
  }
  const auto& ref = testsrc_64x48();
  REQUIRE(ref.size() == 5);
  const Bytes mp4 = test::fixture("integration/h264_alac.mp4");
  // H.264 + ALAC is copied into Matroska and QuickTime unchanged.
  const Bytes mkv = convert_and_check(mp4, "clip.mp4", "mkv", "", ref, 5, 38.0);
  const Bytes mov = convert_and_check(mkv, "clip.mkv", "mov", "", ref, 5, 38.0);
  {
    auto video = test::read_owned<ir::Video>("mov", mov);
    REQUIRE_OK(video);
    REQUIRE((*video)->tracks.size() == 2);
    CHECK((*video)->tracks[0].codec == "h264");
    CHECK((*video)->tracks[1].codec == "alac");
    auto drained = test::drain(**video);
    REQUIRE_OK(drained);
    CHECK(drained->samples.size() == 4000);  // 0.5 s at 8 kHz, sample-exact
  }
  // WebM needs VP9 (re-encoded; WebM's audio codecs need the lossy audio family).
  const Bytes webm = convert_and_check(mov, "clip.mov", "webm", R"({"streams":"video_only"})", ref, 5, 34.0);
  {
    auto video = test::read_owned<ir::Video>("webm", webm);
    REQUIRE_OK(video);
    CHECK((*video)->tracks[0].codec == "vp9");
  }
  // AVI takes Motion JPEG; TS takes H.264 again.
  const Bytes avi = convert_and_check(webm, "clip.webm", "avi", "", ref, 5, 28.0);
  const Bytes ts = convert_and_check(avi, "clip.avi", "mpegts", "", ref, 5, 28.0);
  const Bytes back = convert_and_check(ts, "clip.ts", "mp4", "", ref, 5, 28.0);
  auto info = test::read_owned<ir::Video>("mp4", back);
  REQUIRE_OK(info);
  CHECK((*info)->info.width == 64);
  CHECK((*info)->info.height == 48);
  CHECK(std::llabs((*info)->info.duration_ms - 500) <= 100);
}

TEST_CASE("integration/video: H.264 -> HEVC -> AV1 -> VP9 keeps every frame, the timing and the picture") {
  for (const char* codec : {"h264", "hevc", "av1", "vp9"}) {
    if (!has_video_encoder(codec) || !has_video_decoder(codec)) SKIP(codec, " is not available in this build");
  }
  const auto& ref = testsrc_64x64();
  const Bytes source = test::fixture("video/h264_aac.mp4");
  struct Step {
    const char* codec;
    double floor;
  };
  Bytes current = source;
  double previous = 99.0;
  for (const Step& step : {Step{"hevc", 33.0}, Step{"av1", 31.0}, Step{"vp9", 29.0}}) {
    INFO(step.codec);
    const std::string options = std::format(R"({{"video_codec":"{}","streams":"video_only","quality":90}})", step.codec);
    const CapiResult out = capi_convert(current, "clip.mkv", "mkv", options);
    REQUIRE(out.ok());
    auto video = test::read_owned<ir::Video>("mkv", out.data);
    REQUIRE_OK(video);
    CHECK((*video)->tracks[0].codec == step.codec);
    CHECK(std::llabs((*video)->info.duration_ms - 1000) <= 100);
    auto drained = test::drain(**video);
    REQUIRE_OK(drained);
    REQUIRE(drained->frames.size() == 10);
    // Per-frame floor against the uncompressed pattern; generations only lose.
    const auto i420 = as_i420(drained->frames);
    for (std::size_t i = 0; i < i420.size(); ++i) CHECK(test::psnr(i420[i], ref[i]) >= step.floor);
    const double quality = test::min_psnr(i420, std::vector<ir::VideoFrame>(ref.begin(), ref.begin() + 10));
    MESSAGE(step.codec, ": ", out.data.size(), " bytes, worst frame ", quality, " dB");
    CHECK(quality <= previous + 0.5);
    previous = quality;
    for (std::size_t i = 1; i < drained->frames.size(); ++i) {
      CHECK(drained->frames[i].pts_ms - drained->frames[i - 1].pts_ms == 100);
    }
    current = out.data;
  }
}

TEST_CASE("integration/video: remuxing copies every packet unchanged") {
  const Bytes source = test::fixture("video/h264_aac.mp4");
  const CapiResult mkv = capi_convert(source, "clip.mp4", "mkv");
  REQUIRE(mkv.ok());
  const CapiResult mov = capi_convert(mkv.data, "clip.mkv", "mov");
  REQUIRE(mov.ok());
  auto a = test::read_owned<ir::Video>("mp4", source);
  auto b = test::read_owned<ir::Video>("mov", mov.data);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  REQUIRE((*a)->tracks.size() == (*b)->tracks.size());
  for (std::size_t t = 0; t < (*a)->tracks.size(); ++t) {
    INFO("track ", t);
    CHECK((*a)->tracks[t].codec == (*b)->tracks[t].codec);
    CHECK((*a)->tracks[t].extradata == (*b)->tracks[t].extradata);
    auto pa = test::packets_of((*a)->tracks[t]);
    auto pb = test::packets_of((*b)->tracks[t]);
    REQUIRE_OK(pa);
    REQUIRE_OK(pb);
    REQUIRE(pa->size() == pb->size());
    for (std::size_t k = 0; k < pa->size(); ++k) {
      CHECK((*pa)[k].bytes == (*pb)[k].bytes);
      CHECK((*pa)[k].keyframe == (*pb)[k].keyframe);
      // Same presentation time, in milliseconds.
      CHECK(std::llabs((*pa)[k].pts * 1000 / (*a)->tracks[t].timescale - (*pb)[k].pts * 1000 / (*b)->tracks[t].timescale) <= 1);
    }
  }
  // H.264 elementary stream -> MP4 -> H.264 keeps the pictures bit-exact.
  if (has_video_decoder("h264")) {
    const Bytes raw = test::fixture("video/h264.h264");
    const CapiResult wrapped = capi_convert(raw, "clip.h264", "mp4");
    REQUIRE(wrapped.ok());
    const CapiResult unwrapped = capi_convert(wrapped.data, "clip.mp4", "h264", R"({"video_codec":"copy"})");
    REQUIRE(unwrapped.ok());
    auto x = decode_all("h264", raw);
    auto y = decode_all("h264", unwrapped.data);
    REQUIRE_OK(x);
    REQUIRE_OK(y);
    REQUIRE(x->frames.size() == y->frames.size());
    CHECK(test::min_psnr(x->frames, y->frames) == test::detail::infinity);
  }
}

TEST_CASE("integration/video: raw HEVC with B-frames gets presentation times in MP4") {
  if (!has_video_decoder("hevc")) SKIP("no HEVC decoder in this build");
  const Bytes raw = test::fixture("codec_hevc/main_176x144_ctu64_bframes.h265");
  const CapiResult mp4 = capi_convert(raw, "clip.h265", "mp4");
  REQUIRE(mp4.ok());
  auto video = test::read_owned<ir::Video>("mp4", mp4.data);
  REQUIRE_OK(video);
  auto packets = test::packets_of((*video)->tracks[0]);
  REQUIRE_OK(packets);
  REQUIRE(packets->size() == 20);
  std::vector<std::int64_t> pts;
  bool reordered = false;
  for (const ir::Packet& p : *packets) {
    CHECK(p.dts <= p.pts);
    if (!pts.empty() && p.pts < pts.back()) reordered = true;
    pts.push_back(p.pts);
  }
  CHECK(reordered);  // B-frames keep their order of presentation
  auto drained = test::drain(**video);
  REQUIRE_OK(drained);
  REQUIRE(drained->frames.size() == 20);
  for (std::size_t i = 1; i < drained->frames.size(); ++i) CHECK(drained->frames[i].pts_ms - drained->frames[i - 1].pts_ms == 40);
}

TEST_CASE("integration/video: pictures out of a video, and a video out of an animation") {
  const auto& ref = testsrc_64x48();
  const Bytes mp4 = test::fixture("integration/h264_alac.mp4");
  if (!has_video_decoder("h264")) SKIP("no H.264 decoder in this build");
  // GIF and APNG animations keep every frame and its timing.
  for (const char* target : {"gif", "apng"}) {
    INFO(target);
    const CapiResult out = capi_convert(mp4, "clip.mp4", target);
    REQUIRE(out.ok());
    auto image = test::read_as<ir::Image>(target, out.data);
    REQUIRE_OK(image);
    CHECK(image->frames.size() == 5);
    CHECK(image->width() == 64);
  }
  // A PNG poster frame matches the first picture.
  const CapiResult poster = capi_convert(mp4, "clip.mp4", "png", R"({"image_mode":"poster"})");
  REQUIRE(poster.ok());
  auto png = test::read_as<ir::Image>("png", poster.data);
  REQUIRE_OK(png);
  REQUIRE(png->frames.size() == 1);
  ir::VideoFrame rgba = ir::VideoFrame::allocate(ir::PixelFormat::RGBA8, png->width(), png->height());
  rgba.planes[0].data = png->frames[0].rgba;
  const auto as_yuv = as_i420({rgba});
  REQUIRE(as_yuv.size() == 1);
  CHECK(test::psnr(as_yuv[0], ref[0]) >= 28.0);  // H.264, then RGB and back

  // Animated GIF -> MP4: one video frame per GIF frame.
  if (!has_video_encoder("h264")) SKIP("no H.264 encoder in this build");
  const Bytes gif = test::fixture("image/gif/anim_pillow.gif");
  auto source = test::read_as<ir::Image>("gif", gif);
  REQUIRE_OK(source);
  const CapiResult video = capi_convert(gif, "anim.gif", "mp4");
  REQUIRE(video.ok());
  auto drained = decode_all("mp4", video.data);
  REQUIRE_OK(drained);
  CHECK(drained->frames.size() >= source->frames.size());
}

TEST_CASE("integration/video: the sound of a video becomes WAV and FLAC, sample for sample") {
  const Bytes mp4 = test::fixture("integration/h264_alac.mp4");
  for (const char* target : {"wav", "flac"}) {
    INFO(target);
    const CapiResult out = capi_convert(mp4, "clip.mp4", target);
    REQUIRE(out.ok());
    auto audio = test::read_as<ir::Audio>(target, out.data);
    REQUIRE_OK(audio);
    CHECK(audio->sample_rate == 8000);
    CHECK(audio->channels == 1);
    CHECK(audio->frame_count() == 4000);
    const ir::Audio tone = test::sine(8000, 1, 440.0, 0.5, 0.125);
    CHECK(test::aligned_snr_db(tone, *audio, 16) >= 30.0);
  }
}

TEST_CASE("integration/video: rotation, trimming, scaling and frame rate") {
  if (!has_video_encoder("h264") || !has_video_decoder("h264")) SKIP("no H.264 codec in this build");
  // Rotation: re-encoding turns the pictures upright; copying keeps the flag.
  const Bytes rotated = test::fixture("video/h264_rot90.mov");
  auto source = test::read_owned<ir::Video>("mov", rotated);
  REQUIRE_OK(source);
  const std::uint32_t w = (*source)->info.width;
  const std::uint32_t h = (*source)->info.height;
  REQUIRE((*source)->info.rotation != 0);
  const CapiResult upright = capi_convert(rotated, "clip.mov", "mp4", R"({"quality":80})");
  REQUIRE(upright.ok());
  auto turned = test::read_owned<ir::Video>("mp4", upright.data);
  REQUIRE_OK(turned);
  CHECK((*turned)->info.width == h);
  CHECK((*turned)->info.height == w);
  CHECK((*turned)->info.rotation == 0);
  const CapiResult copied = capi_convert(rotated, "clip.mov", "mp4");
  REQUIRE(copied.ok());
  auto flagged = test::read_owned<ir::Video>("mp4", copied.data);
  REQUIRE_OK(flagged);
  CHECK((*flagged)->info.rotation == (*source)->info.rotation);

  const auto& ref = testsrc_64x64();
  const Bytes mp4 = test::fixture("video/h264_aac.mp4");
  // Trim: 0.3 s from 0.2 s = frames 2, 3 and 4.
  const CapiResult trimmed = capi_convert(mp4, "clip.mp4", "mkv", R"({"start":0.2,"duration":0.3,"streams":"video_only"})");
  REQUIRE(trimmed.ok());
  auto cut = decode_all("mkv", trimmed.data);
  REQUIRE_OK(cut);
  REQUIRE(cut->frames.size() == 3);
  CHECK(test::psnr(as_i420(cut->frames)[0], ref[2]) >= 30.0);
  // Scale: half size.
  const CapiResult half = capi_convert(mp4, "clip.mp4", "mp4", R"({"width":32,"streams":"video_only"})");
  REQUIRE(half.ok());
  auto small = decode_all("mp4", half.data);
  REQUIRE_OK(small);
  REQUIRE(!small->frames.empty());
  CHECK(small->frames[0].width == 32);
  CHECK(small->frames[0].height == 32);
  // Frame rate: 10 -> 5 fps keeps every second frame over the same second.
  const CapiResult slower = capi_convert(mp4, "clip.mp4", "mp4", R"({"fps":5,"streams":"video_only"})");
  REQUIRE(slower.ok());
  auto fewer = decode_all("mp4", slower.data);
  REQUIRE_OK(fewer);
  CHECK(fewer->frames.size() == 5);
  for (std::size_t i = 1; i < fewer->frames.size(); ++i) CHECK(fewer->frames[i].pts_ms - fewer->frames[i - 1].pts_ms == 200);
}

TEST_CASE("integration/video: compressing an MP4 gets smaller as quality drops") {
  if (!has_video_encoder("h264") || !has_video_decoder("h264")) SKIP("no H.264 codec in this build");
  const Bytes mp4 = test::fixture("video/h264_aac.mp4");
  // The engine offers mp4 -> mp4 as "compress".
  const std::string targets = [] {
    char* json = cvt_targets_json("mp4");
    std::string text = json != nullptr ? json : "";
    cvt_string_free(json);
    return text;
  }();
  CHECK(targets.find(R"("id":"mp4")") != std::string::npos);
  std::size_t previous = SIZE_MAX;
  for (const int quality : {90, 60, 30}) {
    INFO("quality ", quality);
    const CapiResult out = capi_convert(mp4, "clip.mp4", "mp4", std::format(R"({{"quality":{},"streams":"video_only"}})", quality));
    REQUIRE(out.ok());
    CHECK(out.data.size() < previous);
    previous = out.data.size();
    auto drained = decode_all("mp4", out.data);
    REQUIRE_OK(drained);
    CHECK(drained->frames.size() == 10);
    CHECK(clip_psnr(*drained, testsrc_64x64(), 10) >= (quality >= 90 ? 36.0 : quality >= 60 ? 30.0 : 24.0));
  }
}

TEST_CASE("integration/video: cvt_convert_file maps the input and writes the output") {
  auto dir = util::TempDir::create("cvt-integration");
  REQUIRE_OK(dir);
  const auto input = dir->path() / "clip.mp4";
  const auto output = dir->path() / "clip.wav";
  REQUIRE_OK(util::write_file(input, test::fixture("integration/h264_alac.mp4")));
  cvt_result* result = cvt_convert_file(input.string().c_str(), output.string().c_str(), nullptr, "wav", nullptr, nullptr, nullptr);
  CHECK(cvt_result_status(result) == CVT_OK);
  CHECK(cvt_result_size(result) == 0);
  CHECK(std::string(cvt_result_format(result)) == "wav");
  cvt_result_free(result);
  auto written = util::read_file(output);
  REQUIRE_OK(written);
  auto audio = test::read_as<ir::Audio>("wav", *written);
  REQUIRE_OK(audio);
  CHECK(audio->frame_count() == 4000);
}
