// Convertible core - audio_lossy family: registration of the AAC area
// (.aac ADTS, .m4a/.m4b/.m4r/.3ga MPEG-4 audio, the aac codec).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "util/bytes.hpp"
#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/mp4_boxes.hpp"
#include "families/audio_lossless/tags.hpp"
#include "families/audio_lossy/aac/aac.hpp"
#include "families/audio_lossy/common.hpp"
#include "families/audio_lossy/lossy.hpp"
#include "util/text.hpp"

namespace cvt::lossy {

namespace {

constexpr std::array<std::uint32_t, 9> aac_rates = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};

std::uint32_t aac_bitrate(const WriteContext& ctx, int channels) {
  if (const auto kbps = options::resolved_bitrate_kbps(ctx.options)) return *kbps * 1000;
  const double per_channel = quality_curve(lossy_quality(ctx.options), {{1, 16}, {10, 24}, {30, 40}, {50, 64}, {75, 96}, {90, 128}, {100, 160}});
  return static_cast<std::uint32_t>(per_channel * 1000.0 * channels);
}

Result<aac::Encoded> encode(const AnyIR& value, WriteContext& ctx, ir::Audio& audio_out) {
  warn_if_reencoding(ctx);
  CVT_TRY_ASSIGN(audio_out, prepare_lossy(value.get<ir::Audio>(), ctx, aac_rates, 2));
  return aac::encode_audio(audio_out, aac_bitrate(ctx, audio_out.channels), ctx);
}

Result<Bytes> write_m4a(const AnyIR& value, WriteContext& ctx) {
  ir::Audio audio;
  CVT_TRY_ASSIGN(aac::Encoded stream, encode(value, ctx, audio));
  return aac::write_m4a(stream, audio.tags, "M4A ", ctx);
}

Result<Bytes> write_adts(const AnyIR& value, WriteContext& ctx) {
  ir::Audio audio;
  CVT_TRY_ASSIGN(aac::Encoded stream, encode(value, ctx, audio));
  aac::AudioConfig config;
  config.object_type = 2;
  config.sample_rate = stream.sample_rate;
  config.channel_config = stream.channels;
  Bytes out = audio.tags.empty() ? Bytes{} : audio::tags::id3v2_tag(audio.tags);
  // ADTS has no gapless signalling: drop the priming frame where possible
  // (the first access unit only holds encoder priming).
  for (const Bytes& unit : stream.units) {
    const Bytes header = aac::adts_header(config, unit.size());
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), unit.begin(), unit.end());
  }
  return out;
}

Result<AnyIR> read_adts(ReadContext& ctx) {
  CVT_TRY_ASSIGN(aac::Track track, aac::read_adts(ctx.input, ctx));
  CVT_TRY_ASSIGN(ir::Audio audio, aac::decode_track(track, ctx));
  return audio;
}

Result<AnyIR> read_m4a(ReadContext& ctx) {
  CVT_TRY_ASSIGN(auto track, aac::read_mp4_aac(ctx.input, ctx));
  if (!track) {
    // Another codec (ALAC, MP3, Opus, ...): the generic MPEG-4 reader.
    CVT_TRY_ASSIGN(ir::Audio audio, audio::read_mp4_audio(ctx.input, ctx));
    return audio;
  }
  CVT_TRY_ASSIGN(ir::Audio audio, aac::decode_track(*track, ctx));
  return audio;
}

int sniff_adts(ByteView data, std::string_view) {
  const auto d = as_u8(data);
  std::size_t pos = 0;
  if (d.size() >= 10 && ::cvt::util::compare_bytes(d.data(), "ID3", 3) == 0) {
    pos = 10 + ((std::size_t{d[6] & 0x7Fu} << 21) | (std::size_t{d[7] & 0x7Fu} << 14) | (std::size_t{d[8] & 0x7Fu} << 7) |
                (d[9] & 0x7Fu));
  }
  int confirmed = 0;
  for (int i = 0; i < 3 && pos + 7 <= d.size(); ++i) {
    if (d[pos] != 0xFF || (d[pos + 1] & 0xF6) != 0xF0 || ((d[pos + 2] >> 2) & 15) > 12) return 0;
    const std::size_t length = (std::size_t{d[pos + 3] & 3u} << 11) | (std::size_t{d[pos + 4]} << 3) | (d[pos + 5] >> 5);
    if (length < 7) return 0;
    pos += length;
    ++confirmed;
  }
  return confirmed >= 2 || (confirmed == 1 && pos >= d.size()) ? 92 : 0;
}

/// MPEG-4 file with an AAC (or other non-ALAC) sound track and no video.
int sniff_m4a(ByteView data, std::string_view filename) {
  if (data.size() < 12 || as_chars(data.subspan(4, 4)) != "ftyp") return 0;
  const std::string_view head = as_chars(data.first(std::min<std::size_t>(data.size(), 1 << 20)));
  std::string_view brand = head.substr(8, 4);
  // 3GPP files are "3gp" (the video family's format, which also reads their
  // sound) unless the name says .3ga or .m4a.
  const bool gpp = brand == "3gp4" || brand == "3gp5" || brand == "3gp6";
  if (gpp && !util::iends_with(filename, ".3ga") && !util::iends_with(filename, ".m4a")) return 0;
  const bool audio_brand = brand == "M4A " || brand == "M4B " || brand == "M4P " || gpp || brand == "mp42" ||
                           brand == "isom" || brand == "M4R ";
  if (!audio_brand) return 0;
  const std::string_view tail = data.size() > (1 << 20) ? as_chars(data.last(1 << 20)) : std::string_view{};
  const auto contains = [&](std::string_view s) {
    return head.find(s) != std::string_view::npos || tail.find(s) != std::string_view::npos;
  };
  if (contains("vide") && contains("stsd") && !(brand == "M4A " || brand == "M4B ")) return 0;
  if (!contains("soun")) return brand == "M4A " ? 70 : 0;
  if (contains("alac")) return 0;  // Apple Lossless: the lossless family's format
  return 96;
}

std::vector<OptionSpec> aac_options() {
  std::vector<OptionSpec> o = {options::quality(), options::preset(), options::bitrate_kbps()};
  for (OptionSpec& spec : audio::standard_write_options()) o.push_back(std::move(spec));
  return o;
}

void add_codecs(Registry& registry, const std::string& id, std::function<Result<AnyIR>(ReadContext&)> read,
                std::function<Result<Bytes>(const AnyIR&, WriteContext&)> write) {
  Codec reader;
  reader.format_id = id;
  reader.kind = Kind::Audio;
  reader.read = std::move(read);
  reader.read_cost = 10;
  registry.add_codec(std::move(reader));
  Codec writer;
  writer.format_id = id;
  writer.kind = Kind::Audio;
  writer.write = std::move(write);
  writer.write_cost = 20;
  writer.lossy = true;
  writer.write_options = aac_options();
  registry.add_codec(std::move(writer));
}

}  // namespace

void register_aac(Registry& registry) {
  FormatInfo adts;
  adts.id = "aac";
  adts.name = "AAC audio (ADTS)";
  adts.description = "Raw AAC stream with ADTS frame headers, as used by radio streams and some players.";
  adts.category = Category::Audio;
  adts.extensions = {"aac", "adts"};
  adts.mime = "audio/aac";
  adts.popularity = 60;
  adts.sniff = sniff_adts;
  registry.add_format(std::move(adts));

  FormatInfo m4a;
  m4a.id = "m4a";
  m4a.name = "MPEG-4 audio (AAC)";
  m4a.description = "AAC in an MPEG-4 file (.m4a), the default format of Apple devices; also audiobooks (.m4b) and ringtones (.m4r).";
  m4a.category = Category::Audio;
  m4a.extensions = {"m4a", "m4b", "m4r", "3ga"};
  m4a.mime = "audio/mp4";
  m4a.popularity = 80;
  m4a.sniff = sniff_m4a;
  registry.add_format(std::move(m4a));

  add_codecs(registry, "aac", read_adts, write_adts);
  add_codecs(registry, "m4a", read_m4a, write_m4a);

  registry.media().register_audio_decoder(codec::id::aac, [] { return aac::make_aac_decoder(); });
  registry.media().register_audio_encoder(codec::id::aac, [] { return aac::make_aac_encoder(); });
}

}  // namespace cvt::lossy
