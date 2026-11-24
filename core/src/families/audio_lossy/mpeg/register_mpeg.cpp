// Convertible core - audio_lossy family: registration of the MPEG audio area
// (mp3, mp2, mp1 files; mp3 and mp2 codecs in the media registry).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/tags.hpp"
#include "families/audio_lossless/audio_dsp.hpp"
#include "families/audio_lossy/common.hpp"
#include "families/audio_lossy/lossy.hpp"
#include "families/audio_lossy/mpeg/mpeg_audio.hpp"
#include "families/audio_lossy/mpeg/mpeg_files.hpp"

namespace cvt::lossy {

namespace {

using mpeg::FrameHeader;

std::size_t skip_id3(ByteView data) {
  const auto d = as_u8(data);
  std::size_t pos = 0;
  while (d.size() - pos >= 10 && d[pos] == 'I' && d[pos + 1] == 'D' && d[pos + 2] == '3') {
    const std::size_t size = (std::size_t{d[pos + 6] & 0x7Fu} << 21) | (std::size_t{d[pos + 7] & 0x7Fu} << 14) |
                             (std::size_t{d[pos + 8] & 0x7Fu} << 7) | (d[pos + 9] & 0x7Fu);
    pos += 10 + size + ((d[pos + 5] & 0x10) != 0 ? 10 : 0);
    if (pos >= d.size()) return d.size();
  }
  return pos;
}

int sniff_layer(ByteView data, int layer) {
  const std::size_t start = skip_id3(data);
  const auto h = mpeg::probe_stream(data, start, start == 0 ? 4096 : 16384);
  if (!h || h->layer != layer) return 0;
  return start > 0 ? 93 : 90;
}

Result<AnyIR> read_mpeg(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Audio audio, mpeg::read_mpeg_file(ctx.input, ctx));
  return audio;
}

constexpr std::array<std::uint32_t, 9> mp3_rates = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};

Result<Bytes> write_mp3(const AnyIR& value, WriteContext& ctx) {
  warn_if_reencoding(ctx);
  CVT_TRY_ASSIGN(ir::Audio audio, prepare_lossy(value.get<ir::Audio>(), ctx, mp3_rates, 2));
  mpeg::Mp3Settings settings;
  settings.quality = lossy_quality(ctx.options);
  settings.bitrate = options::resolved_bitrate_kbps(ctx.options);
  const std::string mode = ctx.options.get_enum("mp3_mode", "auto", {"auto", "vbr", "cbr", "abr"});
  if (mode == "vbr") {
    settings.mode = mpeg::Mp3Settings::Mode::Vbr;
    if (settings.bitrate) ctx.warn("bitrate_kbps is ignored in VBR mode; quality decides the size.");
    settings.bitrate.reset();
  } else if (mode == "abr") {
    settings.mode = mpeg::Mp3Settings::Mode::Abr;
    if (!settings.bitrate) settings.bitrate = 192;
  } else if (mode == "cbr") {
    settings.mode = mpeg::Mp3Settings::Mode::Cbr;
    if (!settings.bitrate) settings.bitrate = audio.sample_rate >= 32000 ? 256 : 96;
  } else {
    settings.mode = settings.bitrate ? mpeg::Mp3Settings::Mode::Cbr : mpeg::Mp3Settings::Mode::Vbr;
  }
  const std::string stereo = ctx.options.get_enum("stereo_mode", "auto", {"auto", "joint", "stereo", "mono"});
  if (stereo == "mono" && audio.channels == 2) CVT_TRY(audio::dsp::remix(audio, 1));
  settings.stereo = stereo == "joint"    ? mpeg::Mp3Settings::Stereo::Joint
                    : stereo == "stereo" ? mpeg::Mp3Settings::Stereo::Stereo
                                         : mpeg::Mp3Settings::Stereo::Auto;
  CVT_TRY_ASSIGN(Bytes stream, mpeg::encode_mp3(audio, settings, ctx));
  if (!audio.tags.empty()) {
    Bytes tag = audio::tags::id3v2_tag(audio.tags);
    tag.insert(tag.end(), stream.begin(), stream.end());
    return tag;
  }
  return stream;
}

constexpr std::array<std::uint32_t, 6> mp2_rates = {16000, 22050, 24000, 32000, 44100, 48000};

Result<Bytes> write_mp2(const AnyIR& value, WriteContext& ctx) {
  warn_if_reencoding(ctx);
  CVT_TRY_ASSIGN(ir::Audio audio, prepare_lossy(value.get<ir::Audio>(), ctx, mp2_rates, 2));
  mpeg::Mp2Settings settings;
  const int quality = lossy_quality(ctx.options);
  const double per_channel = quality_curve(quality, {{1, 32}, {30, 48}, {50, 64}, {75, 96}, {90, 128}, {100, 192}});
  const bool lsf = audio.sample_rate < 32000;
  settings.bitrate = static_cast<std::uint32_t>(per_channel * audio.channels * (lsf ? 0.5 : 1.0));
  if (const auto kbps = options::resolved_bitrate_kbps(ctx.options)) settings.bitrate = *kbps;
  CVT_TRY_ASSIGN(Bytes stream, mpeg::encode_mp2(audio, settings, ctx));
  if (!audio.tags.empty()) {
    Bytes tag = audio::tags::id3v2_tag(audio.tags);
    tag.insert(tag.end(), stream.begin(), stream.end());
    return tag;
  }
  return stream;
}

std::vector<OptionSpec> mp2_options() {
  std::vector<OptionSpec> o = {options::quality(), options::preset(), options::bitrate_kbps()};
  for (OptionSpec& spec : audio::standard_write_options()) o.push_back(std::move(spec));
  return o;
}

std::vector<OptionSpec> mp3_options() {
  std::vector<OptionSpec> o = {
      options::quality(),
      options::preset(),
      options::bitrate_kbps(),
      OptionSpec::choice("mp3_mode", "Bit-rate mode", "auto", {"auto", "vbr", "cbr", "abr"},
                         "vbr follows the quality setting; cbr keeps a constant bit rate (bitrate_kbps, default "
                         "256); abr targets an average bit rate; auto = cbr when a bit rate is given, else vbr."),
      OptionSpec::choice("stereo_mode", "Stereo coding", "auto", {"auto", "joint", "stereo", "mono"},
                         "auto chooses mid/side coding frame by frame; mono mixes the channels down.")
          .advanced(),
  };
  for (OptionSpec& spec : audio::standard_write_options()) o.push_back(std::move(spec));
  return o;
}

void add_mpeg_format(Registry& registry, std::string id, std::string name, std::string description,
                     std::vector<std::string> extensions, int popularity, int layer) {
  FormatInfo info;
  info.id = std::move(id);
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Audio;
  info.extensions = std::move(extensions);
  info.mime = "audio/mpeg";
  info.popularity = popularity;
  info.sniff = [layer](ByteView data, std::string_view) { return sniff_layer(data, layer); };
  if (layer == 3) {
    // A bare ID3v2 tag or a typical first frame header: likely MP3 even when
    // the frames cannot be confirmed (tiny or cut files).
    info.signatures = {Signature::at(0, "'ID3'"), Signature::at(0, "FF FB"), Signature::at(0, "FF F3"),
                       Signature::at(0, "FF F2"), Signature::at(0, "FF FA")};
  }
  registry.add_format(std::move(info));
}

}  // namespace

void register_mpeg_audio(Registry& registry) {
  add_mpeg_format(registry, "mp3", "MP3 audio", "MPEG-1/2 Audio Layer III: the most widely supported lossy audio format.",
                  {"mp3"}, 95, 3);
  add_mpeg_format(registry, "mp2", "MPEG Layer II audio",
                  "MPEG-1/2 Audio Layer II, used by DAB radio, DVB television and Video CDs.", {"mp2", "mpa", "m2a"}, 25,
                  2);
  add_mpeg_format(registry, "mp1", "MPEG Layer I audio", "MPEG-1/2 Audio Layer I (read only).", {"mp1"}, 5, 1);

  for (const char* id : {"mp3", "mp2", "mp1"}) {
    Codec reader;
    reader.format_id = id;
    reader.kind = Kind::Audio;
    reader.read = read_mpeg;
    registry.add_codec(std::move(reader));
  }
  Codec mp3_writer;
  mp3_writer.format_id = "mp3";
  mp3_writer.kind = Kind::Audio;
  mp3_writer.write = write_mp3;
  mp3_writer.write_cost = 20;
  mp3_writer.lossy = true;
  mp3_writer.write_options = mp3_options();
  registry.add_codec(std::move(mp3_writer));
  Codec mp2_writer;
  mp2_writer.format_id = "mp2";
  mp2_writer.kind = Kind::Audio;
  mp2_writer.write = write_mp2;
  mp2_writer.write_cost = 20;
  mp2_writer.lossy = true;
  mp2_writer.write_options = mp2_options();
  registry.add_codec(std::move(mp2_writer));

  registry.media().register_audio_decoder(codec::id::mp3, [] { return mpeg::make_mpeg_decoder(); });
  registry.media().register_audio_decoder(codec::id::mp2, [] { return mpeg::make_mpeg_decoder(); });
  registry.media().register_audio_encoder(codec::id::mp3, [] { return mpeg::make_mp3_encoder(); });
  registry.media().register_audio_encoder(codec::id::mp2, [] { return mpeg::make_mp2_encoder(); });
}

}  // namespace cvt::lossy
