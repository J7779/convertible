// Convertible core - audio_lossy family: registration of the Xiph area
// (Ogg Opus, Ogg Vorbis, Ogg Speex; opus/vorbis/speex codecs).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossy/common.hpp"
#include "families/audio_lossy/lossy.hpp"
#include "families/audio_lossy/xiph/xiph.hpp"

namespace cvt::lossy {

namespace {

/// The kind of the first logical stream of an Ogg file.
std::string_view ogg_kind(ByteView data) {
  const auto d = as_u8(data);
  if (d.size() < 28 + 8 || std::memcmp(d.data(), "OggS", 4) != 0) return {};
  const std::size_t segments = d[26];
  const std::size_t body = 27 + segments;
  if (d.size() < body + 8) return {};
  const auto* p = d.data() + body;
  if (std::memcmp(p, "OpusHead", 8) == 0) return "opus";
  if (std::memcmp(p, "\x01vorbis", 7) == 0) return "vorbis";
  if (std::memcmp(p, "Speex   ", 8) == 0) return "speex";
  if (std::memcmp(p, "\x7F" "FLAC", 5) == 0) return "flac";
  return "other";
}

std::vector<OptionSpec> lossy_options() {
  std::vector<OptionSpec> o = {options::quality(), options::preset(), options::bitrate_kbps()};
  for (OptionSpec& spec : audio::standard_write_options()) o.push_back(std::move(spec));
  return o;
}

Result<AnyIR> read_ogg_any(ReadContext& ctx) {
  const std::string_view kind = ogg_kind(ctx.input);
  if (kind == "opus") {
    CVT_TRY_ASSIGN(ir::Audio a, xiph::read_ogg_opus(ctx.input, ctx));
    return a;
  }
  if (kind == "speex") {
    CVT_TRY_ASSIGN(ir::Audio a, xiph::read_ogg_speex(ctx.input, ctx));
    return a;
  }
  if (kind == "flac" && ctx.registry != nullptr) {
    // Ogg FLAC belongs to the lossless family: use its reader.
    for (const Codec* codec : ctx.registry->codecs_for("ogg-flac")) {
      if (codec->kind == Kind::Audio && codec->read) return codec->read(ctx);
    }
  }
  CVT_TRY_ASSIGN(ir::Audio a, xiph::read_ogg_vorbis(ctx.input, ctx));
  return a;
}

Result<AnyIR> read_opus(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Audio a, xiph::read_ogg_opus(ctx.input, ctx));
  return a;
}

Result<AnyIR> read_spx(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Audio a, xiph::read_ogg_speex(ctx.input, ctx));
  return a;
}

constexpr std::array<std::uint32_t, 1> opus_rates = {48000};

Result<Bytes> write_opus(const AnyIR& value, WriteContext& ctx) {
  warn_if_reencoding(ctx);
  CVT_TRY_ASSIGN(ir::Audio audio, prepare_lossy(value.get<ir::Audio>(), ctx, opus_rates, 8));
  xiph::OpusSettings s;
  const int quality = lossy_quality(ctx.options);
  const double per_channel = quality_curve(quality, {{1, 12}, {10, 16}, {30, 32}, {50, 48}, {75, 64}, {90, 80}, {100, 128}});
  double total = per_channel * audio.channels;
  if (audio.channels > 2) total *= 0.75;  // surround streams share bits well
  s.bitrate_bps = static_cast<std::uint32_t>(total * 1000.0);
  if (const auto kbps = options::resolved_bitrate_kbps(ctx.options)) s.bitrate_bps = *kbps * 1000;
  return xiph::write_ogg_opus(audio, s, ctx);
}

Result<Bytes> write_vorbis(const AnyIR& value, WriteContext& ctx) {
  warn_if_reencoding(ctx);
  CVT_TRY_ASSIGN(ir::Audio audio, prepare_lossy(value.get<ir::Audio>(), ctx, {}, 255));
  if (audio.sample_rate < 8000 || audio.sample_rate > 192000) {
    return fail(Status::Encode, "Vorbis supports 8 to 192 kHz (got {} Hz)", audio.sample_rate);
  }
  xiph::VorbisSettings s;
  const int quality = lossy_quality(ctx.options);
  s.quality = static_cast<float>(quality_curve(quality, {{1, -0.1}, {10, -0.1}, {30, 0.1}, {50, 0.3}, {75, 0.5}, {90, 0.7}, {100, 1.0}}));
  if (const auto kbps = options::resolved_bitrate_kbps(ctx.options)) s.bitrate = *kbps * 1000;
  return xiph::write_ogg_vorbis(audio, s, ctx);
}

constexpr std::array<std::uint32_t, 3> speex_rates = {8000, 16000, 32000};

Result<Bytes> write_spx(const AnyIR& value, WriteContext& ctx) {
  warn_if_reencoding(ctx);
  CVT_TRY_ASSIGN(ir::Audio audio, prepare_lossy(value.get<ir::Audio>(), ctx, speex_rates, 1));
  xiph::SpeexSettings s;
  s.quality = static_cast<int>(quality_curve(lossy_quality(ctx.options), {{1, 0}, {30, 3}, {50, 5}, {75, 7}, {90, 8}, {100, 10}}) + 0.5);
  return xiph::write_ogg_speex(audio, s, ctx);
}

void add(Registry& registry, std::string id, std::string name, std::string description,
         std::vector<std::string> extensions, std::string mime, int popularity,
         std::function<int(ByteView, std::string_view)> sniff) {
  FormatInfo info;
  info.id = std::move(id);
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Audio;
  info.extensions = std::move(extensions);
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.sniff = std::move(sniff);
  registry.add_format(std::move(info));
}

void add_codec(Registry& registry, std::string id, std::function<Result<AnyIR>(ReadContext&)> read,
               std::function<Result<Bytes>(const AnyIR&, WriteContext&)> write) {
  Codec reader;
  reader.format_id = id;
  reader.kind = Kind::Audio;
  reader.read = std::move(read);
  registry.add_codec(std::move(reader));
  Codec writer;
  writer.format_id = std::move(id);
  writer.kind = Kind::Audio;
  writer.write = std::move(write);
  writer.write_cost = 20;
  writer.lossy = true;
  writer.write_options = lossy_options();
  registry.add_codec(std::move(writer));
}

}  // namespace

void register_xiph(Registry& registry) {
  add(registry, "opus", "Opus audio", "Opus in Ogg: the modern, efficient codec for music and speech.", {"opus"},
      "audio/ogg; codecs=opus", 55, [](ByteView d, std::string_view) { return ogg_kind(d) == "opus" ? 96 : 0; });
  add(registry, "ogg", "Ogg Vorbis audio", "Vorbis in Ogg: the open lossy audio format.", {"ogg", "oga"}, "audio/ogg", 65,
      [](ByteView d, std::string_view) {
        const std::string_view k = ogg_kind(d);
        return k == "vorbis" ? 95 : k.empty() || k == "flac" ? 0 : 60;
      });
  add(registry, "spx", "Speex audio", "Speex in Ogg: an older open speech codec.", {"spx"}, "audio/ogg; codecs=speex", 10,
      [](ByteView d, std::string_view) { return ogg_kind(d) == "speex" ? 96 : 0; });
  add_codec(registry, "opus", read_opus, write_opus);
  add_codec(registry, "ogg", read_ogg_any, write_vorbis);
  add_codec(registry, "spx", read_spx, write_spx);

  auto& media = registry.media();
  media.register_audio_decoder(codec::id::opus, [] { return xiph::make_opus_decoder(); });
  media.register_audio_encoder(codec::id::opus, [] { return xiph::make_opus_encoder(); });
  media.register_audio_decoder(codec::id::vorbis, [] { return xiph::make_vorbis_decoder(); });
  media.register_audio_encoder(codec::id::vorbis, [] { return xiph::make_vorbis_encoder(); });
  media.register_audio_decoder(codec::id::speex, [] { return xiph::make_speex_decoder(); });
}

}  // namespace cvt::lossy
