// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdio>
#include <format>

#include "codec/registry.hpp"
#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/audio_lossy/lossy_signals.hpp"

using namespace cvt;
using lossy_test::music;
using lossy_test::round_trip;
using lossy_test::Trip;

namespace {

struct Case {
  const char* format;
  const char* name;
  std::size_t frames;
  double max_rms;
};

}  // namespace

TEST_CASE("audio_lossy/xiph: decodes like the reference decoders (opusdec, oggdec, speexdec)") {
  // Lengths are the encoders' inputs (0.6 s; Opus at 48 kHz) thanks to the
  // granule positions. opusdec soft-clips peaks above full scale, which the
  // loud 96 kbit/s file has; speexdec's output is 40/80 samples longer
  // because it ignores the end granule.
  for (const Case& c : {Case{"opus", "xiph/opus_celt_96_st_48000.opus", 28800, 6.0 / 32768},
                        Case{"opus", "xiph/opus_silk_10_mono_16000.opus", 28800, 0.7 / 32768},
                        Case{"opus", "xiph/opus_hybrid_24_mono_48000.opus", 28800, 0.7 / 32768},
                        Case{"opus", "xiph/opus_celt_2p5ms_st_48000.opus", 28800, 0.7 / 32768},
                        Case{"opus", "xiph/opus_60ms_st_44100.opus", 28800, 0.7 / 32768},
                        Case{"ogg", "xiph/oggenc_q5_st_44100.ogg", 26460, 0.7 / 32768},
                        Case{"ogg", "xiph/oggenc_q0_mono_22050.ogg", 13230, 0.7 / 32768},
                        Case{"ogg", "xiph/oggenc_tags_mono_8000.ogg", 4800, 0.7 / 32768},
                        Case{"spx", "xiph/speexenc_nb_q8_8000.spx", 4800, 0.7 / 32768},
                        Case{"spx", "xiph/speexenc_wb_q6_16000.spx", 9600, 0.7 / 32768}}) {
    INFO(c.name);
    const lossy_test::Match m = lossy_test::against_reference(c.format, c.name);
    std::printf("%-38s frames %zu rms %.3g (%.2f LSB) snr %.1f\n", c.name, m.frames, m.rms, m.rms * 32768, m.snr);
    CHECK(m.frames == c.frames);
    CHECK(m.rms <= c.max_rms);
  }
  const auto tagged = lossy_test::read_audio("ogg", test::fixture("audio_lossy/xiph/oggenc_tags_mono_8000.ogg"));
  REQUIRE_OK(tagged);
  CHECK(tagged->tags.at("title") == "Vorbis Tone");
  CHECK(tagged->tags.at("artist") == "Convertible Fixtures");
  // ffmpeg's libvorbis output in .oga.
  const auto oga = lossy_test::read_audio("ogg", test::fixture("audio_lossy/xiph/ff_vorbis_q4_st_44100.oga"));
  REQUIRE_OK(oga);
  CHECK(oga->frame_count() == 26460);
}

TEST_CASE("audio_lossy/xiph: detection") {
  for (const auto& [name, format] : {std::pair{"xiph/opus_celt_96_st_48000.opus", "opus"}, std::pair{"xiph/oggenc_q5_st_44100.ogg", "ogg"},
                                     std::pair{"xiph/speexenc_nb_q8_8000.spx", "spx"}}) {
    INFO(name);
    const auto d = detect_format(Registry::instance(), test::fixture(std::string("audio_lossy/") + name), "");
    REQUIRE(d.has_value());
    CHECK(d->format->id == format);
  }
  // Ogg FLAC stays with the lossless family.
  const auto flac = detect_format(Registry::instance(), test::fixture("audio_lossless/flac_s16_stereo_11025.oga"), "x.oga");
  REQUIRE(flac.has_value());
  CHECK(flac->format->id == "ogg-flac");
}

TEST_CASE("audio_lossy/opus: encoder quality levels and gapless length") {
  const ir::Audio source = music(48000, 2, 1.0);
  std::size_t previous = 0;
  for (const auto& [quality, min_snr] : {std::pair{90, 12.0}, std::pair{75, 10.0}, std::pair{50, 8.0}, std::pair{20, 4.0}}) {
    INFO(std::format("quality {}", quality));
    const Trip t = round_trip("opus", source, std::format(R"({{"quality":{}}})", quality));
    std::printf("opus q%d: %zu bytes, snr %.2f\n", quality, t.bytes, t.snr);
    CHECK(t.frames == source.frame_count());
    CHECK(t.snr >= min_snr);
    if (previous != 0) CHECK(t.bytes < previous);
    previous = t.bytes;
  }
  // 44.1 kHz input is resampled to Opus' 48 kHz.
  const Trip resampled = round_trip("opus", music(44100, 1, 0.5), R"({"bitrate_kbps":64})");
  CHECK(resampled.rate == 48000);
  CHECK(resampled.frames == 24000);
  // Surround (mapping family 1).
  ir::Audio six = test::sine(48000, 6, 500.0, 0.4, 0.3);
  const Trip surround = round_trip("opus", six, "{}");
  CHECK(surround.frames == six.frame_count());
  CHECK(surround.snr > 6.0);  // the LFE channel is band-limited by design
}

TEST_CASE("audio_lossy/vorbis: encoder quality levels, managed bit rate and tags") {
  const ir::Audio source = music(44100, 2, 1.0);
  std::size_t previous = 0;
  for (const auto& [quality, min_snr] : {std::pair{90, 20.0}, std::pair{75, 16.0}, std::pair{50, 12.0}, std::pair{20, 6.0}}) {
    INFO(std::format("quality {}", quality));
    const Trip t = round_trip("ogg", source, std::format(R"({{"quality":{}}})", quality));
    std::printf("vorbis q%d: %zu bytes, snr %.2f\n", quality, t.bytes, t.snr);
    CHECK(t.frames == source.frame_count());
    CHECK(t.snr >= min_snr);
    if (previous != 0) CHECK(t.bytes < previous);
    previous = t.bytes;
  }
  const Trip managed = round_trip("ogg", music(44100, 2, 2.0), R"({"bitrate_kbps":96})");
  CHECK_APPROX(static_cast<double>(managed.bytes) * 8 / 2.0 / 1000.0, 96.0, 30.0);
  ir::Audio tagged = music(22050, 1, 0.3);
  tagged.tags = {{"title", "Tone"}};
  const auto kept = test::write_from("ogg", tagged, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  CHECK(lossy_test::read_audio("ogg", *kept)->tags.at("title") == "Tone");
}

TEST_CASE("audio_lossy/speex: encoder") {
  const ir::Audio speech = music(16000, 1, 0.6);
  const Trip high = round_trip("spx", speech, R"({"quality":90})");
  const Trip low = round_trip("spx", speech, R"({"quality":30})");
  std::printf("speex: q90 %zu bytes %.2f dB, q30 %zu bytes %.2f dB\n", high.bytes, high.snr, low.bytes, low.snr);
  CHECK(high.frames == speech.frame_count());
  CHECK(low.bytes < high.bytes);
  CHECK(high.snr > 3.0);
}

TEST_CASE("audio_lossy/xiph: codec registry and damaged input") {
  const auto& media = Registry::instance().media();
  for (const char* id : {"opus", "vorbis", "speex"}) CHECK(media.has_audio_decoder(id));
  for (const char* id : {"opus", "vorbis"}) CHECK(media.has_audio_encoder(id));
  // Opus through the registry: OpusHead extradata, pre-skip dropped.
  auto enc = media.make_audio_encoder("opus");
  codec::AudioEncodeParams params;
  params.sample_rate = 48000;
  params.channels = 2;
  REQUIRE_OK(enc->configure(params));
  const ir::Audio source = music(48000, 2, 0.4);
  codec::AudioChunk chunk;
  chunk.samples = source.samples;
  REQUIRE_OK(enc->send_chunk(chunk));
  REQUIRE_OK(enc->flush());
  auto dec = media.make_audio_decoder("opus");
  codec::CodecParams cp;
  cp.extradata = enc->extradata();
  REQUIRE_OK(dec->configure(cp));
  ir::Audio out;
  out.sample_rate = 48000;
  out.channels = 2;
  while (true) {
    auto p = enc->receive_packet();
    REQUIRE_OK(p);
    if (!*p) break;
    REQUIRE_OK(dec->send_packet(**p));
    auto c = dec->receive_chunk();
    REQUIRE_OK(c);
    if (*c) out.samples.insert(out.samples.end(), (*c)->samples.begin(), (*c)->samples.end());
  }
  out.samples.resize(std::min(out.samples.size(), source.samples.size()));
  CHECK(test::aligned_snr_db(source, out, 10) > 10.0);
  for (const auto& [name, format] : {std::pair{"xiph/opus_silk_10_mono_16000.opus", "opus"}, std::pair{"xiph/oggenc_tags_mono_8000.ogg", "ogg"},
                                     std::pair{"xiph/speexenc_nb_q8_8000.spx", "spx"}}) {
    const Bytes data = test::fixture(std::string("audio_lossy/") + name);
    CHECK(test::fuzz_reader<ir::Audio>(format, data) > 0);
  }
}
