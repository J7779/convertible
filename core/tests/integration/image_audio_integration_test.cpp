// Image <-> image through the real codecs, audio between the lossless family's
// formats, and the conversions that need families still to come (AAC, MP3,
// Opus, Vorbis, WebP, AVIF/HEIC): those skip with a reason until the codec
// is registered, and then run automatically.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <format>

#include "families/image/image_ops.hpp"
#include "integration/integration_support.hpp"

using namespace cvt;
using namespace cvt::test::integration;

namespace {

ir::Image gradient_image(std::uint32_t w, std::uint32_t h) {
  ir::Image image;
  image.frames.push_back(test::gradient_frame(w, h));
  return image;
}

bool has_image_codec(std::string_view format) {
  return test::find_reader(format, Kind::Image) != nullptr && test::find_writer(format, Kind::Image) != nullptr;
}

}  // namespace

TEST_CASE("integration/image: lossless formats round trip exactly, lossy ones meet their floor") {
  const auto png = test::write_from("png", gradient_image(96, 64));
  REQUIRE_OK(png);
  const ir::Image original = gradient_image(96, 64);
  for (const char* target : {"bmp", "tiff", "tga", "qoi", "ppm", "gif", "jpg", "webp", "avif", "heic"}) {
    INFO(target);
    if (!has_image_codec(target)) {
      MESSAGE("skipped: no ", target, " codec in this build yet");
      continue;
    }
    const CapiResult out = capi_convert(*png, "picture.png", target);
    REQUIRE(out.ok());
    const CapiResult back = capi_convert(out.data, std::string("picture.") + target, "png");
    REQUIRE(back.ok());
    auto decoded = test::read_as<ir::Image>("png", back.data);
    REQUIRE_OK(decoded);
    const std::string_view t = target;
    if (t == "jpg" || t == "webp" || t == "avif" || t == "heic") {
      CHECK(test::psnr(original.frames[0], decoded->frames[0]) >= 38.0);
    } else if (t == "gif") {
      CHECK(test::psnr(original.frames[0], decoded->frames[0]) >= 28.0);  // 256 colours, dithered
    } else {
      CHECK(test::pixels_equal(original, *decoded));
    }
  }
  // Compress: PNG -> PNG is offered and never larger than the input.
  const CapiResult compressed = capi_convert(*png, "picture.png", "png", R"({"compression_level":9})");
  REQUIRE(compressed.ok());
  CHECK(compressed.data.size() <= png->size());
  // JPEG quality: lower is smaller.
  std::size_t previous = SIZE_MAX;
  for (const int quality : {95, 75, 40}) {
    const CapiResult jpg = capi_convert(*png, "picture.png", "jpg", std::format(R"({{"quality":{}}})", quality));
    REQUIRE(jpg.ok());
    CHECK(jpg.data.size() < previous);
    previous = jpg.data.size();
  }
}

TEST_CASE("integration/audio: lossless audio formats carry the same samples") {
  const ir::Audio tone = test::sine(8000, 1, 440.0, 0.5, 0.5);
  const auto wav = test::write_from("wav", tone);
  REQUIRE_OK(wav);
  for (const char* target : {"flac", "alac", "aiff", "wv", "caf", "au"}) {
    INFO(target);
    const CapiResult out = capi_convert(*wav, "tone.wav", target);
    REQUIRE(out.ok());
    const CapiResult back = capi_convert(out.data, std::string("tone.") + out.format, "wav");
    REQUIRE(back.ok());
    auto audio = test::read_as<ir::Audio>("wav", back.data);
    REQUIRE_OK(audio);
    CHECK(audio->frame_count() == tone.frame_count());
    CHECK(test::snr_db(tone, *audio) >= 80.0);  // 16-bit, half scale, dithered
  }
}

TEST_CASE("integration/pending: lossy audio formats (AAC, MP3, Opus, Vorbis)") {
  const ir::Audio tone = test::sine(48000, 2, 440.0, 1.0, 0.5);
  const auto wav = test::write_from("wav", tone);
  REQUIRE_OK(wav);
  std::size_t ran = 0;
  for (const auto& [codec, target] : {std::pair{"aac", "m4a"}, std::pair{"mp3", "mp3"}, std::pair{"opus", "opus"},
                                      std::pair{"vorbis", "ogg"}}) {
    INFO(target);
    if (!has_audio_encoder(codec) || !has_audio_decoder(codec)) continue;
    const CapiResult out = capi_convert(*wav, "tone.wav", target);
    REQUIRE(out.ok());
    const CapiResult back = capi_convert(out.data, std::string("tone.") + target, "wav");
    REQUIRE(back.ok());
    auto audio = test::read_as<ir::Audio>("wav", back.data);
    REQUIRE_OK(audio);
    CHECK(test::aligned_snr_db(tone, *audio, 4096) >= 20.0);
    // In a video: MP4 with H.264 + AAC is the most common file there is.
    if (std::string_view(codec) == "aac" && has_video_encoder("h264")) {
      const Bytes mp4 = test::fixture("integration/h264_alac.mp4");
      const CapiResult with_aac = capi_convert(mp4, "clip.mp4", "mp4", R"({"audio_codec":"aac"})");
      CHECK(with_aac.ok());
    }
    ++ran;
  }
  if (ran == 0) SKIP("pending: no AAC/MP3/Opus/Vorbis codec is registered yet (audio_lossy family)");
}

TEST_CASE("integration/pending: WebM with Opus or Vorbis sound") {
  if (!has_audio_encoder("opus") && !has_audio_encoder("vorbis")) SKIP("pending: WebM audio needs the Opus or Vorbis encoder (audio_lossy family)");
  if (!has_video_encoder("vp9")) SKIP("no VP9 encoder in this build");
  const Bytes mp4 = test::fixture("integration/h264_alac.mp4");
  const CapiResult webm = capi_convert(mp4, "clip.mp4", "webm");
  REQUIRE(webm.ok());
  auto drained = decode_all("webm", webm.data);
  REQUIRE_OK(drained);
  CHECK(drained->frames.size() == 5);
  CHECK(drained->samples.size() >= 3800);
}

TEST_CASE("integration/pending: MPEG-4 Part 2 and Theora encoders in their containers") {
  std::size_t ran = 0;
  const Bytes mp4 = test::fixture("integration/h264_alac.mp4");
  for (const auto& [codec, container] : {std::pair{"mpeg4", "avi"}, std::pair{"theora", "ogv"}, std::pair{"mpeg2video", "mpegts"}}) {
    INFO(codec);
    if (!has_video_encoder(codec) || !has_video_decoder(codec)) continue;
    const CapiResult out = capi_convert(mp4, "clip.mp4", container, std::format(R"({{"video_codec":"{}","streams":"video_only"}})", codec));
    REQUIRE(out.ok());
    auto drained = decode_all(container, out.data);
    REQUIRE_OK(drained);
    CHECK(clip_psnr(*drained, testsrc_64x48(), 5) >= 28.0);
    ++ran;
  }
  if (ran == 0) SKIP("pending: no MPEG-4 Part 2 / Theora / MPEG-2 encoder is registered (codec_legacy family)");
}

TEST_CASE("integration/image: a JPEG goes into a PDF unchanged unless asked to re-encode") {
  if (test::find_reader("jpg", Kind::Image) == nullptr) SKIP("no JPEG reader in this build");
  const Bytes jpeg = test::fixture("pdf/photo.jpg");
  auto image = test::read_as<ir::Image>("jpg", jpeg);
  REQUIRE_OK(image);
  CHECK(image->original_mime == "image/jpeg");
  CHECK(image->original_bytes == jpeg);
  const CapiResult pdf = capi_convert(jpeg, "photo.jpg", "pdf");
  REQUIRE(pdf.ok());
  CHECK(test::str(pdf.data).find(test::str(jpeg)) != std::string::npos);  // embedded byte for byte
  const CapiResult smaller = capi_convert(jpeg, "photo.jpg", "pdf", R"({"quality":30})");
  REQUIRE(smaller.ok());
  CHECK(test::str(smaller.data).find(test::str(jpeg)) == std::string::npos);  // re-encoded
  // Pixel-changing operations drop the original bytes.
  ir::Image copy = *image;
  REQUIRE_OK(cvt::image::resize(copy, 16, 12, cvt::image::ResizeOptions{}));
  CHECK(copy.original_bytes.empty());
}
