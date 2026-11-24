// Convertible core - audio_lossless family: Sun/NeXT AU (.au, .snd).
//
// Reads big-endian (".snd") and the rare little-endian ("dns.") variant:
// mu-law, A-law, 8/16/24/32-bit linear PCM, 32/64-bit float. An unknown
// data size (0xFFFFFFFF, written by streaming tools) means "to the end".
// Writes big-endian AU with an empty annotation.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/pcm.hpp"
#include "families/audio_lossless/tags.hpp"
#include "util/text.hpp"

namespace cvt::audio {

namespace {

Result<AnyIR> read_au(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  const std::string_view magic = r.chars(4);
  const bool little = magic == "dns.";
  if (magic != ".snd" && !little) return fail(Status::Decode, "not an AU file");
  const auto u32 = [&]() { return little ? r.u32le() : r.u32be(); };
  const std::uint32_t offset = u32();
  const std::uint32_t size = u32();
  const std::uint32_t encoding = u32();
  const std::uint32_t rate = u32();
  const std::uint32_t channels = u32();
  if (!r.ok()) return fail(Status::Decode, "the AU header is truncated");
  if (offset < 24 || offset > ctx.input.size()) return fail(Status::Decode, "the AU data offset is invalid");
  CVT_TRY(check_stream_params(rate, channels, "the AU file"));
  const std::endian order = little ? std::endian::little : std::endian::big;
  PcmFormat layout;
  std::uint16_t bits = 0;
  switch (encoding) {
    case 1: layout = pcm_mulaw(); bits = 16; break;
    case 27: layout = pcm_alaw(); bits = 16; break;
    case 2: layout = pcm_signed(8, order); bits = 8; break;
    case 3: layout = pcm_signed(16, order); bits = 16; break;
    case 4: layout = pcm_signed(24, order); bits = 24; break;
    case 5: layout = pcm_signed(32, order); bits = 32; break;
    case 6: layout = pcm_float(32, order); bits = 32; break;
    case 7: layout = pcm_float(64, order); bits = 64; break;
    default: return fail(Status::Decode, "unsupported AU encoding {}", encoding);
  }
  ir::Audio audio = make_audio(rate, static_cast<std::uint16_t>(channels), bits, encoding == 6 || encoding == 7);
  // The annotation is free text; keep it as a comment when it is printable.
  const ByteView annotation = ctx.input.subspan(24, offset - 24);
  std::string_view note = as_chars(annotation);
  note = note.substr(0, note.find('\0'));
  if (!note.empty() && std::all_of(note.begin(), note.end(), [](char c) { return c >= 0x20 && c < 0x7F; })) {
    tags::put(audio.tags, "comment", note);
  }
  ByteView data = ctx.input.subspan(offset);
  if (size != 0xFFFFFFFFu && size < data.size()) data = data.first(size);
  if (size != 0xFFFFFFFFu && size > data.size() + 4096) ctx.warn("The audio data is shorter than the header says.");
  const std::uint64_t frame_bytes = std::uint64_t{layout.container_bytes} * channels;
  const std::uint64_t frames = data.size() / frame_bytes;
  CVT_TRY(check_sample_budget(ctx, frames * channels));
  decode_pcm(data.first(static_cast<std::size_t>(frames * frame_bytes)), layout, audio.samples);
  return audio;
}

const std::vector<std::string>& au_formats() {
  static const std::vector<std::string> choices = {"auto", "s8", "s16", "s24", "s32", "f32", "f64", "alaw", "mulaw"};
  return choices;
}

Result<Bytes> write_au(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx, 1, 0, 0));
  std::string choice = ctx.options.get_enum(key_sample_format, "auto", au_formats());
  if (choice == "auto") {
    if (audio.float_samples) {
      choice = audio.bits_per_sample == 64 ? "f64" : "f32";
    } else if (audio.bits_per_sample == 0 || (audio.bits_per_sample > 8 && audio.bits_per_sample <= 16)) {
      choice = "s16";
    } else if (audio.bits_per_sample <= 8) {
      choice = "s8";
    } else if (audio.bits_per_sample <= 24) {
      choice = "s24";
    } else {
      choice = "s32";
    }
  }
  const ParsedSampleFormat parsed = parse_sample_format(choice);
  PcmFormat layout;
  std::uint32_t encoding = 3;
  switch (parsed.encoding) {
    case PcmEncoding::Signed:
      layout = pcm_signed(parsed.bits, std::endian::big);
      encoding = parsed.bits == 8 ? 2 : parsed.bits == 16 ? 3 : parsed.bits == 24 ? 4 : 5;
      break;
    case PcmEncoding::Float:
      layout = pcm_float(parsed.bits, std::endian::big);
      encoding = parsed.bits == 64 ? 7 : 6;
      break;
    case PcmEncoding::Alaw: layout = pcm_alaw(); encoding = 27; break;
    case PcmEncoding::Mulaw: layout = pcm_mulaw(); encoding = 1; break;
    case PcmEncoding::Unsigned: return fail(Status::InvalidArgument, "AU has no unsigned sample format");
  }
  const Bytes data = encode_pcm(audio.samples, audio.channels, layout, resolved_dither(ctx.options), audio.sample_rate);
  CVT_TRY(ctx.limits.check_output(data.size() + 32));
  util::ByteWriter out(data.size() + 32);
  out.text(".snd");
  out.u32be(32);
  out.u32be(data.size() < 0xFFFFFFFFu ? static_cast<std::uint32_t>(data.size()) : 0xFFFFFFFFu);
  out.u32be(encoding);
  out.u32be(audio.sample_rate);
  out.u32be(audio.channels);
  out.zeros(8);  // empty annotation (the format requires at least four bytes)
  out.bytes(data);
  return out.take();
}

}  // namespace

void register_au(Registry& registry) {
  FormatInfo info;
  info.id = "au";
  info.name = "Sun/NeXT audio";
  info.description = "The classic Unix sound format (.au, .snd): mu-law, A-law, PCM or float.";
  info.category = Category::Audio;
  info.extensions = {"au", "snd"};
  info.mime = "audio/basic";
  info.popularity = 20;
  info.signatures = {Signature::at(0, "'.snd' 00 00"), Signature::at(0, "'dns.'")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "au";
  codec.kind = Kind::Audio;
  codec.read = read_au;
  codec.write = write_au;
  codec.write_options = standard_write_options();
  codec.write_options.insert(codec.write_options.begin(), sample_format_option(au_formats()));
  codec.write_options.push_back(dither_option());
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::audio
