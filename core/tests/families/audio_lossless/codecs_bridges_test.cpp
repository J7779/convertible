// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/audio_test_util.hpp"
#include "families/audio_lossless/pcm.hpp"
#include "families/audio_lossless/tags.hpp"

using namespace cvt;
namespace ta = cvt::test::audio;

namespace {

// Encode with a registry encoder, decode with the registry decoder.
Result<ir::Audio> codec_round_trip(std::string_view id, const ir::Audio& input, int compression_level = 5,
                                   std::unique_ptr<codec::AudioEncoder> encoder = nullptr) {
  const codec::Registry& media = Registry::instance().media();
  if (!encoder) encoder = media.make_audio_encoder(id);
  if (!encoder) return fail(Status::UnsupportedConversion, "no encoder");
  codec::AudioEncodeParams params;
  params.sample_rate = input.sample_rate;
  params.channels = input.channels;
  params.compression_level = compression_level;
  CVT_TRY(encoder->configure(params));
  std::vector<codec::Packet> packets;
  // Feed in uneven chunks.
  std::size_t at = 0;
  std::size_t step = 77 * input.channels;
  while (at < input.samples.size()) {
    codec::AudioChunk chunk;
    const std::size_t n = std::min(step, input.samples.size() - at);
    chunk.samples.assign(input.samples.begin() + static_cast<std::ptrdiff_t>(at),
                         input.samples.begin() + static_cast<std::ptrdiff_t>(at + n));
    at += n;
    step = step * 3 % (4096 * input.channels) + input.channels;
    CVT_TRY(encoder->send_chunk(chunk));
    while (true) {
      CVT_TRY_ASSIGN(auto packet, encoder->receive_packet());
      if (!packet) break;
      packets.push_back(std::move(*packet));
    }
  }
  CVT_TRY(encoder->flush());
  while (true) {
    CVT_TRY_ASSIGN(auto packet, encoder->receive_packet());
    if (!packet) break;
    packets.push_back(std::move(*packet));
  }
  auto decoder = media.make_audio_decoder(id);
  if (!decoder) return fail(Status::UnsupportedConversion, "no decoder");
  codec::CodecParams params_in;
  params_in.codec = std::string(id);
  params_in.extradata = encoder->extradata();
  params_in.sample_rate = input.sample_rate;
  params_in.channels = input.channels;
  params_in.timescale = input.sample_rate;
  if (id == codec::id::adpcm_ima || id == codec::id::adpcm_ms) {
    params_in.block_align = static_cast<std::uint32_t>(packets.empty() ? 0 : packets.front().bytes.size());
  }
  CVT_TRY(decoder->configure(params_in));
  ir::Audio out;
  out.sample_rate = input.sample_rate;
  out.channels = input.channels;
  for (const codec::Packet& p : packets) {
    CVT_TRY(decoder->send_packet(p));
    while (true) {
      CVT_TRY_ASSIGN(auto chunk, decoder->receive_chunk());
      if (!chunk) break;
      out.samples.insert(out.samples.end(), chunk->samples.begin(), chunk->samples.end());
    }
  }
  CVT_TRY(decoder->flush());
  if (decoder->sample_rate() != input.sample_rate || decoder->channels() != input.channels) {
    return fail(Status::Internal, "decoder reports a different format");
  }
  return out;
}

}  // namespace

TEST_CASE("audio_lossless/codecs: every audio codec of the family is registered") {
  const codec::Registry& media = Registry::instance().media();
  for (const std::string_view id :
       {codec::id::pcm_s16le, codec::id::pcm_s16be, codec::id::pcm_s24le, codec::id::pcm_f32le, codec::id::pcm_u8,
        codec::id::pcm_alaw, codec::id::pcm_mulaw, codec::id::adpcm_ima, codec::id::adpcm_ms, codec::id::flac,
        codec::id::alac, codec::id::wavpack}) {
    INFO(id);
    CHECK(media.has_audio_decoder(id));
    CHECK(media.has_audio_encoder(id));
  }
  CHECK(media.problems().empty());
}

TEST_CASE("audio_lossless/codecs: lossless codecs round-trip exactly through the codec interfaces") {
  const ir::Audio input = ta::music(44100, 2, 0.5, 16);
  for (const std::string_view id : {codec::id::flac, codec::id::alac, codec::id::wavpack, codec::id::pcm_s16le,
                                    codec::id::pcm_s16be, codec::id::pcm_s24le, codec::id::pcm_f32le}) {
    INFO(id);
    const auto out = codec_round_trip(id, input);
    REQUIRE_OK(out);
    CHECK(ta::identical(input, *out));
  }
  // 24-bit through the precision-aware factory.
  const ir::Audio deep = ta::music(48000, 2, 0.2, 24);
  for (const std::string_view id : {codec::id::flac, codec::id::alac, codec::id::wavpack}) {
    INFO(id);
    const auto out = codec_round_trip(id, deep, 8, audio::make_lossless_encoder(id, 24));
    REQUIRE_OK(out);
    CHECK(ta::identical(deep, *out));
  }
  CHECK(audio::make_lossless_encoder("mp3", 16) == nullptr);
}

TEST_CASE("audio_lossless/codecs: lossy sample codecs stay within their quality") {
  const ir::Audio input = ta::music(22050, 2, 0.3, 16);
  for (const auto& [id, snr] : {std::pair<std::string_view, double>{codec::id::pcm_u8, 40.0}, {codec::id::pcm_alaw, 30.0},
                                {codec::id::pcm_mulaw, 30.0}, {codec::id::adpcm_ima, 20.0}, {codec::id::adpcm_ms, 20.0}}) {
    INFO(id);
    auto out = codec_round_trip(id, input);
    REQUIRE_OK(out);
    out->samples.resize(std::min(out->samples.size(), input.samples.size()));
    CHECK(test::snr_db(input, *out) >= snr);
  }
}

TEST_CASE("audio_lossless/codecs: G.711 tables invert exactly") {
  for (int code = 0; code < 256; ++code) {
    CAPTURE(code);
    CHECK(audio::linear_to_alaw(audio::alaw_to_linear(static_cast<std::uint8_t>(code))) == code);
    const std::uint8_t mu = audio::linear_to_mulaw(audio::mulaw_to_linear(static_cast<std::uint8_t>(code)));
    // mu-law has two codes for zero (0x7F and 0xFF).
    CHECK((mu == code || (code == 0x7F && mu == 0xFF)));
  }
  CHECK(audio::mulaw_to_linear(0x00) == -32124);
  CHECK(audio::alaw_to_linear(0xD5) == 8);
}

TEST_CASE("audio_lossless/tags: ID3v2, Vorbis comments, RIFF INFO and APEv2 round trip") {
  audio::tags::TagMap tags = {{"title", "Ünïcødé ✓"}, {"artist", "A"}, {"track", "7"}, {"comment", "c"}};
  audio::tags::TagMap id3;
  audio::tags::parse_id3v2(audio::tags::id3v2_tag(tags), id3);
  CHECK(id3 == tags);
  audio::tags::TagMap vorbis;
  CHECK(audio::tags::parse_vorbis_comment(audio::tags::vorbis_comment(tags), vorbis));
  CHECK(vorbis == tags);
  audio::tags::TagMap ape;
  const Bytes ape_tag = audio::tags::apev2_tag(tags);
  CHECK(audio::tags::parse_apev2(ape_tag, ape) == ape_tag.size());
  CHECK(ape == tags);
  audio::tags::TagMap info;
  const Bytes list = audio::tags::riff_info_chunk(tags);
  audio::tags::parse_riff_info(ByteView(list).subspan(12), info);
  CHECK(info == tags);
  // Damaged tags never crash.
  for (std::size_t n = 0; n < ape_tag.size(); ++n) {
    audio::tags::TagMap ignored;
    (void)audio::tags::parse_apev2(ByteView(ape_tag).first(n), ignored);
    audio::tags::parse_id3v2(ByteView(audio::tags::id3v2_tag(tags)).first(std::min<std::size_t>(n, 40)), ignored);
  }
}

TEST_CASE("audio_lossless/bridge: audio renders as a waveform or a spectrogram picture") {
  const Bridge* bridge = nullptr;
  for (const Bridge& b : Registry::instance().bridges()) {
    if (b.from == Kind::Audio && b.to == Kind::Image) bridge = &b;
  }
  REQUIRE(bridge != nullptr);
  CHECK(bridge->label.find("waveform") != std::string::npos);
  const ir::Audio input = test::sweep(16000, 2, 50.0, 7000.0, 1.0, 0.8);
  {
    Context ctx;
    auto normalized = Options::parse(R"({"picture_width":200,"picture_height":80,"wave_color":"#ff0000"})");
    REQUIRE_OK(normalized);
    auto options = normalized->normalized(bridge->options);
    REQUIRE_OK(options);
    ctx.options = *options;
    auto picture = bridge->fn(input, ctx);
    REQUIRE_OK(picture);
    const ir::Image& image = picture->get<ir::Image>();
    REQUIRE(image.width() == 200);
    REQUIRE(image.height() == 80);
    // The envelope is drawn in red (with a darker core); the top-left corner is background white.
    const ir::Frame& f = image.frames[0];
    std::size_t red = 0;
    for (std::uint32_t y = 0; y < 80; ++y) {
      const std::size_t at = f.offset(100, y);
      if (f.rgba[at] == 255 && f.rgba[at + 1] == 0 && f.rgba[at + 2] == 0) ++red;
    }
    CHECK(red > 0);
    CHECK(f.rgba[f.offset(0, 0) + 1] == 255);
  }
  {
    Context ctx;
    auto options = Options::parse(R"({"picture_kind":"spectrogram","picture_width":64,"picture_height":64,"spectrum_fft_size":"512"})")
                       ->normalized(bridge->options);
    REQUIRE_OK(options);
    ctx.options = *options;
    auto picture = bridge->fn(input, ctx);
    REQUIRE_OK(picture);
    const ir::Frame& f = picture->get<ir::Image>().frames[0];
    // The sweep rises: early columns are bright near the bottom, late ones higher up.
    const auto brightness = [&](std::uint32_t x, std::uint32_t y) {
      const std::size_t at = f.offset(x, y);
      return f.rgba[at] + f.rgba[at + 1] + f.rgba[at + 2];
    };
    CHECK(brightness(2, 62) > brightness(2, 5));
    CHECK(brightness(61, 10) > brightness(2, 10));
  }
  {
    Context ctx;
    auto options = Options::parse(R"({"wave_color":"blue"})")->normalized(bridge->options);
    REQUIRE_OK(options);
    ctx.options = *options;
    CHECK_STATUS(bridge->fn(input, ctx), Status::InvalidArgument);
  }
  // Through the engine when an image writer exists (another family).
  if (Registry::instance().has_writer("png")) {
    const auto wav = test::write_from("wav", input);
    REQUIRE_OK(wav);
    const test::Converted png = test::convert(*wav, "wav", "png", R"({"picture_width":320,"picture_height":100})");
    CHECK(png.ok());
  }
}
