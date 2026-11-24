// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdio>
#include <format>

#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "families/audio_lossy/lossy_signals.hpp"

using namespace cvt;
using lossy_test::music;
using lossy_test::round_trip;
using lossy_test::Trip;

TEST_CASE("audio_lossy/mp3: encoder quality levels, sizes and gapless length") {
  const ir::Audio source = music(44100, 2, 1.0);
  struct Level {
    int quality;
    double min_snr;
  };
  std::size_t previous = 0;
  for (const Level& l : {Level{100, 19}, Level{90, 17}, Level{80, 3}, Level{70, 3}, Level{65, 3}, Level{60, 3}, Level{55, 3}, Level{50, 8}, Level{20, 3}}) {
    INFO(std::format("quality {}", l.quality));
    const Trip t = round_trip("mp3", source, std::format(R"({{"quality":{}}})", l.quality));
    std::printf("mp3 q%d: %zu bytes, snr %.2f dB\n", l.quality, t.bytes, t.snr);
    CHECK(t.frames == source.frame_count());  // gapless: exactly the input length
    CHECK(t.snr >= l.min_snr);
    if (previous != 0) CHECK(t.bytes < previous);
    previous = t.bytes;
  }
}

TEST_CASE("audio_lossy/mp3: constant and average bit rates are honoured") {
  const ir::Audio source = music(44100, 2, 2.0);
  for (const int kbps : {96, 128, 192, 320}) {
    INFO(std::to_string(kbps));
    const Trip t = round_trip("mp3", source, std::format(R"({{"bitrate_kbps":{}}})", kbps));
    const double actual = static_cast<double>(t.bytes) * 8.0 / 2.0 / 1000.0;
    std::printf("mp3 cbr %d: %.1f kbit/s, snr %.2f\n", kbps, actual, t.snr);
    CHECK_APPROX(actual, kbps, kbps * 0.08 + 4);  // plus one Info frame
    CHECK(t.frames == source.frame_count());
  }
  const Trip abr = round_trip("mp3", source, R"({"bitrate_kbps":160,"mp3_mode":"abr"})");
  const double abr_rate = static_cast<double>(abr.bytes) * 8.0 / 2.0 / 1000.0;
  std::printf("mp3 abr 160: %.1f kbit/s, snr %.2f\n", abr_rate, abr.snr);
  CHECK_APPROX(abr_rate, 160.0, 30.0);
}

TEST_CASE("audio_lossy/mp3: every sample rate and channel layout") {
  for (const std::uint32_t rate : {8000u, 11025u, 12000u, 16000u, 22050u, 24000u, 32000u, 44100u, 48000u}) {
    for (const std::uint16_t channels : {std::uint16_t{1}, std::uint16_t{2}}) {
      INFO(std::format("{} Hz, {} ch", rate, channels));
      const ir::Audio source = music(rate, channels, 0.5);
      const Trip t = round_trip("mp3", source, R"({"quality":80})");
      CHECK(t.frames == source.frame_count());
      CHECK(t.snr >= 8.0);
    }
  }
  // A rate MP3 lacks is resampled (with a warning) to the next supported one.
  std::vector<std::string> warnings;
  const auto odd = test::write_from("mp3", music(37800, 2, 0.3), "{}", &warnings);
  REQUIRE_OK(odd);
  CHECK(!warnings.empty());
  const auto decoded = lossy_test::read_audio("mp3", *odd);
  REQUIRE_OK(decoded);
  CHECK(decoded->sample_rate == 44100);
}

TEST_CASE("audio_lossy/mp3: options, tags and the compress path") {
  const ir::Audio source = music(44100, 2, 0.5);
  const auto mono = test::write_from("mp3", source, R"({"stereo_mode":"mono"})");
  REQUIRE_OK(mono);
  CHECK(lossy_test::read_audio("mp3", *mono)->channels == 1);
  const Trip lr = round_trip("mp3", source, R"({"stereo_mode":"stereo","quality":70})");
  const Trip ms = round_trip("mp3", source, R"({"stereo_mode":"joint","quality":70})");
  CHECK(lr.snr > 8);
  CHECK(ms.snr > 8);
  CHECK_STATUS(test::write_from("mp3", source, R"({"mp3_mode":"loud"})"), Status::InvalidArgument);
  ir::Audio tagged = source;
  tagged.tags = {{"title", "Tone"}, {"artist", "Somebody"}};
  const auto stripped = test::write_from("mp3", tagged);
  REQUIRE_OK(stripped);
  CHECK(lossy_test::read_audio("mp3", *stripped)->tags.empty());
  const auto kept = test::write_from("mp3", tagged, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  CHECK(lossy_test::read_audio("mp3", *kept)->tags.at("title") == "Tone");
  // mp3 -> mp3 ("compress"): re-encodes smaller and warns about generation loss.
  const Bytes original = test::fixture("audio_lossy/mpeg/lame_cbr128_js_44100.mp3");
  const test::Converted smaller = test::convert(original, "", "mp3", R"({"quality":40})", "song.mp3");
  REQUIRE(smaller.ok());
  CHECK(smaller.format_id == "mp3");
  CHECK(smaller.data.size() < original.size());
  CHECK(!smaller.warnings.empty());
}

TEST_CASE("audio_lossy/mp3: codec registry encoder and decoder") {
  const auto& media = Registry::instance().media();
  auto encoder = media.make_audio_encoder("mp3");
  auto decoder = media.make_audio_decoder("mp3");
  REQUIRE(encoder != nullptr);
  REQUIRE(decoder != nullptr);
  codec::AudioEncodeParams params;
  params.sample_rate = 48000;
  params.channels = 2;
  params.quality = 80;
  REQUIRE_OK(encoder->configure(params));
  const ir::Audio source = music(48000, 2, 0.5);
  codec::AudioChunk chunk;
  chunk.samples = source.samples;
  REQUIRE_OK(encoder->send_chunk(chunk));
  REQUIRE_OK(encoder->flush());
  codec::CodecParams cp;
  cp.codec = "mp3";
  REQUIRE_OK(decoder->configure(cp));
  ir::Audio out;
  out.sample_rate = 48000;
  out.channels = 2;
  while (true) {
    auto p = encoder->receive_packet();
    REQUIRE_OK(p);
    if (!*p) break;
    REQUIRE_OK(decoder->send_packet(**p));
    while (true) {
      auto c = decoder->receive_chunk();
      REQUIRE_OK(c);
      if (!*c) break;
      out.samples.insert(out.samples.end(), (*c)->samples.begin(), (*c)->samples.end());
    }
  }
  // The decoded stream starts with the encoder's priming.
  REQUIRE(out.samples.size() > std::size_t{encoder->delay()} * 2);
  out.samples.erase(out.samples.begin(), out.samples.begin() + static_cast<std::ptrdiff_t>(encoder->delay()) * 2);
  out.samples.resize(std::min(out.samples.size(), source.samples.size()));
  CHECK(decoder->sample_rate() == 48000);
  CHECK(test::aligned_snr_db(source, out, 100) > 10.0);
}

TEST_CASE("audio_lossy/mp2: Layer II encoder") {
  const ir::Audio source = music(48000, 2, 1.0);
  const Trip high = round_trip("mp2", source, R"({"quality":90})");
  const Trip low = round_trip("mp2", source, R"({"quality":40})");
  std::printf("mp2: q90 %zu bytes %.2f dB, q40 %zu bytes %.2f dB\n", high.bytes, high.snr, low.bytes, low.snr);
  CHECK(high.snr > 12.0);
  CHECK(low.snr > 6.0);
  CHECK(low.bytes < high.bytes);
  const Trip mono = round_trip("mp2", music(24000, 1, 0.5), R"({"quality":75})");
  CHECK(mono.snr > 8.0);
}
