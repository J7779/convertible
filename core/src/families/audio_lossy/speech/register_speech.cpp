// Convertible core - audio_lossy family: registration of the speech-codec
// area (.amr, .awb, .gsm; amr_nb/amr_wb decoders).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossy/common.hpp"
#include "families/audio_lossy/lossy.hpp"
#include "families/audio_lossy/speech/speech.hpp"

namespace cvt::lossy {

namespace {

Result<Bytes> write_amr_common(const AnyIR& value, WriteContext& ctx, bool wide) {
  warn_if_reencoding(ctx);
  const std::array<std::uint32_t, 1> rate = {wide ? 16000u : 8000u};
  CVT_TRY_ASSIGN(ir::Audio audio, prepare_lossy(value.get<ir::Audio>(), ctx, rate, 1));
  const int q = lossy_quality(ctx.options);
  std::uint32_t bitrate = wide ? static_cast<std::uint32_t>(quality_curve(q, {{1, 6600}, {30, 8850}, {50, 12650}, {75, 18250}, {90, 23850}}))
                               : static_cast<std::uint32_t>(quality_curve(q, {{1, 4750}, {30, 5900}, {50, 7950}, {75, 10200}, {90, 12200}}));
  if (const auto kbps = options::resolved_bitrate_kbps(ctx.options)) bitrate = *kbps * 1000;
  return speech::write_amr(audio, wide, bitrate, ctx.options.get_bool("dtx", false), ctx);
}

Result<AnyIR> read_amr_any(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Audio audio, speech::read_amr(ctx.input, ctx));
  return audio;
}

Result<AnyIR> read_gsm_file(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Audio audio, speech::read_gsm(ctx.input, ctx));
  return audio;
}

std::vector<OptionSpec> amr_options() {
  std::vector<OptionSpec> o = {options::quality(), options::preset(), options::bitrate_kbps(),
                               OptionSpec::boolean("dtx", "Silence compression", false,
                                                   "Send only comfort-noise updates during silence (smaller files).")
                                   .advanced()};
  for (OptionSpec& spec : audio::standard_write_options()) o.push_back(std::move(spec));
  return o;
}

void add_format(Registry& registry, std::string id, std::string name, std::string description, std::string ext,
                std::string mime, int popularity, std::vector<Signature> signatures,
                std::function<int(ByteView, std::string_view)> sniff = {}) {
  FormatInfo info;
  info.id = std::move(id);
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Audio;
  info.extensions = {std::move(ext)};
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.signatures = std::move(signatures);
  info.sniff = std::move(sniff);
  registry.add_format(std::move(info));
}

void add_reader(Registry& registry, const std::string& id, std::function<Result<AnyIR>(ReadContext&)> read) {
  Codec reader;
  reader.format_id = id;
  reader.kind = Kind::Audio;
  reader.read = std::move(read);
  registry.add_codec(std::move(reader));
}

void add_writer(Registry& registry, const std::string& id, std::function<Result<Bytes>(const AnyIR&, WriteContext&)> write) {
  Codec writer;
  writer.format_id = id;
  writer.kind = Kind::Audio;
  writer.write = std::move(write);
  writer.write_cost = 20;
  writer.lossy = true;
  writer.write_options = amr_options();
  registry.add_codec(std::move(writer));
}

}  // namespace

void register_speech(Registry& registry) {
  add_format(registry, "amr", "AMR audio", "Adaptive Multi-Rate narrowband speech, used by mobile phones.", "amr",
             "audio/amr", 30, {Signature::at(0, "'#!AMR' 0A")});
  add_format(registry, "awb", "AMR-WB audio", "Adaptive Multi-Rate wideband (HD voice) speech.", "awb", "audio/amr-wb", 15,
             {Signature::at(0, "'#!AMR-WB' 0A")});
  add_format(registry, "gsm", "GSM 06.10 audio", "Raw GSM full-rate speech frames (read only).", "gsm", "audio/x-gsm", 5, {},
             [](ByteView d, std::string_view) { return speech::is_gsm(d) ? 70 : 0; });
  add_reader(registry, "amr", read_amr_any);
  add_reader(registry, "awb", read_amr_any);
  add_reader(registry, "gsm", read_gsm_file);
  add_writer(registry, "amr", [](const AnyIR& v, WriteContext& ctx) { return write_amr_common(v, ctx, false); });
  add_writer(registry, "awb", [](const AnyIR& v, WriteContext& ctx) { return write_amr_common(v, ctx, true); });
  registry.media().register_audio_decoder(codec::id::amr_nb, [] { return speech::make_amr_decoder(false); });
  registry.media().register_audio_decoder(codec::id::amr_wb, [] { return speech::make_amr_decoder(true); });
}

}  // namespace cvt::lossy
