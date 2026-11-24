// Convertible core - audio_lossless family: AIFF and AIFF-C.
//
// Reads big-endian PCM of 1..32 bits, AIFF-C 'NONE'/'twos', 'sowt'
// (little-endian), 'raw ' (offset binary), 'in24'/'in32', 'fl32'/'fl64',
// 'alaw'/'ulaw' and Apple IMA4. The 80-bit extended sample rate is decoded
// exactly. Text chunks (NAME, AUTH, (c), ANNO) and ID3 chunks become tags.
// Writes AIFF (integer PCM) and AIFF-C (all of the above except 'raw ').
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
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

struct Comm {
  std::uint16_t channels = 0;
  std::uint32_t frames = 0;
  std::uint16_t bits = 0;
  double rate = 0.0;
  std::string compression = "NONE";
};

Result<AnyIR> read_aiff(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("FORM")) return fail(Status::Decode, "not an AIFF file");
  r.skip(4);
  const std::string_view form = r.chars(4);
  if (form != "AIFF" && form != "AIFC") return fail(Status::Decode, "not an AIFF file");
  const bool aifc = form == "AIFC";
  std::optional<Comm> comm;
  ByteView sound;
  bool have_sound = false;
  tags::TagMap tag_map;
  std::size_t pos = 12;
  const ByteView input = ctx.input;
  std::size_t guard = 0;
  while (pos + 8 <= input.size() && ++guard < 100000) {
    util::ByteReader h(input.subspan(pos, 8));
    const std::string_view id = h.chars(4);
    const std::uint32_t size = h.u32be();
    const std::size_t body_at = pos + 8;
    const std::size_t remaining = input.size() - body_at;
    const bool truncated = size > remaining;
    const ByteView body = input.subspan(body_at, truncated ? remaining : size);
    if (id == "COMM") {
      util::ByteReader c(body);
      Comm cm;
      cm.channels = c.u16be();
      cm.frames = c.u32be();
      cm.bits = c.u16be();
      cm.rate = read_extended(c.bytes(10));
      if (!c.ok()) return fail(Status::Decode, "the AIFF COMM chunk is truncated");
      if (aifc) {
        const std::string_view type = c.chars(4);
        if (!c.ok()) return fail(Status::Decode, "the AIFF-C COMM chunk is truncated");
        cm.compression = std::string(type);
      }
      comm = cm;
    } else if (id == "SSND") {
      util::ByteReader s(body);
      const std::uint32_t offset = s.u32be();
      s.skip(4);
      if (!s.ok() || offset > s.remaining()) return fail(Status::Decode, "the AIFF sound chunk is damaged");
      sound = body.subspan(8 + offset);
      have_sound = true;
      if (truncated && size - remaining > 4096) ctx.warn("The audio data is shorter than the header says.");
    } else if (id == "NAME") {
      tags::put(tag_map, "title", to_string(body));
    } else if (id == "AUTH") {
      tags::put(tag_map, "artist", to_string(body));
    } else if (id == "(c) ") {
      tags::put(tag_map, "copyright", to_string(body));
    } else if (id == "ANNO") {
      tags::put(tag_map, "comment", to_string(body));
    } else if (id == "ID3 " || id == "id3 ") {
      tags::parse_id3v2(body, tag_map);
    }
    if (truncated) break;
    pos = body_at + size + (size % 2);
  }
  if (!comm) return fail(Status::Decode, "the AIFF file has no COMM chunk");
  if (!std::isfinite(comm->rate) || comm->rate < 1.0 || comm->rate > 10'000'000.0) {
    return fail(Status::Decode, "the AIFF file has an invalid sample rate");
  }
  const auto rate = static_cast<std::uint32_t>(std::llround(comm->rate));
  CVT_TRY(check_stream_params(rate, comm->channels, "the AIFF file"));
  ir::Audio audio = make_audio(rate, comm->channels, comm->bits);
  audio.tags = std::move(tag_map);
  if (!have_sound) {
    if (comm->frames == 0) return audio;  // a valid, empty file
    return fail(Status::Decode, "the AIFF file has no sound data");
  }
  CVT_TRY(ctx.checkpoint(0.0, "Decoding"));
  std::string type = comm->compression;
  for (char& c : type) c = util::ascii_lower(c);
  const std::uint64_t declared = comm->frames;
  PcmFormat layout;
  bool pcm = true;
  if (type == "none" || type == "twos") {
    if (comm->bits == 0 || comm->bits > 32) return fail(Status::Decode, "unsupported AIFF sample size {}", comm->bits);
    layout = pcm_signed(comm->bits, std::endian::big);
    layout.container_bytes = static_cast<std::uint16_t>((comm->bits + 7) / 8);
  } else if (type == "sowt") {
    if (comm->bits == 0 || comm->bits > 32) return fail(Status::Decode, "unsupported AIFF sample size {}", comm->bits);
    layout = pcm_signed(comm->bits, std::endian::little);
  } else if (type == "raw ") {
    layout = pcm_unsigned8();
    audio.bits_per_sample = 8;
  } else if (type == "in24") {
    layout = pcm_signed(24, std::endian::big);
    audio.bits_per_sample = 24;
  } else if (type == "in32") {
    layout = pcm_signed(32, std::endian::big);
    audio.bits_per_sample = 32;
  } else if (type == "fl32") {
    layout = pcm_float(32, std::endian::big);
    audio.float_samples = true;
    audio.bits_per_sample = 32;
  } else if (type == "fl64") {
    layout = pcm_float(64, std::endian::big);
    audio.float_samples = true;
    audio.bits_per_sample = 64;
  } else if (type == "alaw") {
    layout = pcm_alaw();
    audio.bits_per_sample = 16;
  } else if (type == "ulaw") {
    layout = pcm_mulaw();
    audio.bits_per_sample = 16;
  } else if (type == "ima4") {
    pcm = false;
  } else {
    return fail(Status::Decode, "unsupported AIFF-C compression '{}'", comm->compression);
  }
  if (pcm) {
    const std::uint64_t frame_bytes = std::uint64_t{layout.container_bytes} * comm->channels;
    std::uint64_t frames = sound.size() / frame_bytes;
    if (declared < frames) frames = declared;  // trailing bytes beyond the declared length
    CVT_TRY(check_sample_budget(ctx, frames * comm->channels));
    decode_pcm(sound.first(static_cast<std::size_t>(frames * frame_bytes)), layout, audio.samples);
  } else {
    audio.bits_per_sample = 16;
    const std::uint64_t packets = sound.size() / (34u * comm->channels);
    CVT_TRY(check_sample_budget(ctx, packets * 64 * comm->channels));
    CVT_TRY(decode_ima4(sound, comm->channels, audio.samples));
    // COMM counts packets for IMA4; the decoded length is a multiple of 64.
  }
  return audio;
}

const std::vector<std::string>& aiff_formats() {
  static const std::vector<std::string> choices = {"auto", "s8", "s16", "s24", "s32", "f32", "f64", "alaw", "mulaw", "ima4"};
  return choices;
}
const std::vector<std::string>& aifc_formats() {
  static const std::vector<std::string> choices = {"auto", "s8",  "s16",   "s16le", "s24", "s32",
                                                   "f32",  "f64", "alaw", "mulaw", "ima4"};
  return choices;
}

void chunk(util::ByteWriter& out, std::string_view id, ByteView body) {
  out.text(id);
  out.u32be(static_cast<std::uint32_t>(body.size()));
  out.bytes(body);
  if (body.size() % 2 == 1) out.u8(0);
}

void pstring(util::ByteWriter& out, std::string_view text) {
  out.u8(static_cast<std::uint8_t>(text.size()));
  out.text(text);
  if ((text.size() + 1) % 2 == 1) out.u8(0);
}

Result<Bytes> write_aiff_common(const AnyIR& value, WriteContext& ctx, bool force_aifc) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx, 1, 0, 0));
  CVT_TRY(ctx.checkpoint(0.1, "Encoding"));
  std::string choice = ctx.options.get_enum(key_sample_format, "auto", force_aifc ? aifc_formats() : aiff_formats());
  if (choice == "auto") {
    if (audio.float_samples) {
      choice = audio.bits_per_sample == 64 ? "f64" : "f32";
    } else if (audio.bits_per_sample == 0) {
      choice = "s16";
    } else if (audio.bits_per_sample <= 8) {
      choice = "s8";
    } else if (audio.bits_per_sample <= 16) {
      choice = "s16";
    } else if (audio.bits_per_sample <= 24) {
      choice = "s24";
    } else {
      choice = "s32";
    }
  }
  const dsp::Dither dither = resolved_dither(ctx.options);
  std::string compression = "NONE";
  std::string compression_name = "not compressed";
  std::uint16_t bits = 16;
  std::uint32_t frames_field = static_cast<std::uint32_t>(std::min<std::uint64_t>(audio.frame_count(), 0xFFFFFFFFu));
  Bytes data;
  if (choice == "ima4") {
    compression = "ima4";
    compression_name = "IMA 4:1";
    const std::vector<std::int32_t> ints = quantize_for(audio, 16, ctx.options);
    const std::vector<std::int16_t> pcm16(ints.begin(), ints.end());
    data = encode_ima4(pcm16, audio.channels);
    frames_field = static_cast<std::uint32_t>((audio.frame_count() + 63) / 64);
  } else {
    PcmFormat layout;
    if (choice == "s16le") {
      layout = pcm_signed(16, std::endian::little);
      compression = "sowt";
      compression_name = "";
    } else {
      const ParsedSampleFormat parsed = parse_sample_format(choice);
      if (!parsed.ok || parsed.encoding == PcmEncoding::Unsigned) {
        return fail(Status::InvalidArgument, "sample format '{}' is not available for AIFF", choice);
      }
      switch (parsed.encoding) {
        case PcmEncoding::Signed: layout = pcm_signed(parsed.bits, std::endian::big); break;
        case PcmEncoding::Float:
          layout = pcm_float(parsed.bits, std::endian::big);
          compression = parsed.bits == 64 ? "fl64" : "fl32";
          compression_name = "IEEE float";
          break;
        case PcmEncoding::Alaw:
          layout = pcm_alaw();
          compression = "alaw";
          compression_name = "ALaw 2:1";
          break;
        case PcmEncoding::Mulaw:
          layout = pcm_mulaw();
          compression = "ulaw";
          compression_name = "\xB5Law 2:1";
          break;
        case PcmEncoding::Unsigned: break;
      }
    }
    bits = layout.encoding == PcmEncoding::Alaw || layout.encoding == PcmEncoding::Mulaw ? 16 : layout.bits;
    data = encode_pcm(audio.samples, audio.channels, layout, dither, audio.sample_rate);
  }
  const bool aifc = force_aifc || compression != "NONE";
  util::ByteWriter comm;
  comm.u16be(audio.channels);
  comm.u32be(frames_field);
  comm.u16be(bits);
  write_extended(comm, static_cast<double>(audio.sample_rate));
  if (aifc) {
    comm.text(compression);
    pstring(comm, compression_name);
  }
  util::ByteWriter body;
  body.text(aifc ? "AIFC" : "AIFF");
  if (aifc) {
    util::ByteWriter fver;
    fver.u32be(0xA2805140u);
    chunk(body, "FVER", fver.view());
  }
  chunk(body, "COMM", comm.view());
  if (!options::resolved_strip_metadata(ctx.options)) {
    const Bytes id3 = tags::id3v2_tag(audio.tags);
    if (!id3.empty()) chunk(body, "ID3 ", id3);
  }
  const std::uint64_t ssnd_size = 8 + std::uint64_t{data.size()};
  if (ssnd_size + body.size() + 16 > 0xFFFFFFF0ull) return fail(Status::Encode, "the audio is too long for AIFF (4 GB)");
  CVT_TRY(ctx.limits.check_output(ssnd_size + body.size() + 16));
  body.text("SSND");
  body.u32be(static_cast<std::uint32_t>(ssnd_size));
  body.u32be(0);
  body.u32be(0);
  body.bytes(data);
  if (data.size() % 2 == 1) body.u8(0);
  util::ByteWriter out(body.size() + 8);
  out.text("FORM");
  out.u32be(static_cast<std::uint32_t>(body.size()));
  out.bytes(body.view());
  return out.take();
}

Result<Bytes> write_aiff(const AnyIR& value, WriteContext& ctx) { return write_aiff_common(value, ctx, false); }
Result<Bytes> write_aifc(const AnyIR& value, WriteContext& ctx) { return write_aiff_common(value, ctx, true); }

}  // namespace

void register_aiff(Registry& registry) {
  FormatInfo aiff;
  aiff.id = "aiff";
  aiff.name = "AIFF audio";
  aiff.description = "Apple's uncompressed audio format (big-endian PCM).";
  aiff.category = Category::Audio;
  aiff.extensions = {"aiff", "aif"};
  aiff.mime = "audio/aiff";
  aiff.popularity = 45;
  aiff.signatures = {Signature::at(0, "'FORM' ?? ?? ?? ?? 'AIFF'")};
  registry.add_format(std::move(aiff));

  FormatInfo aifc;
  aifc.id = "aifc";
  aifc.name = "AIFF-C audio";
  aifc.description = "AIFF with compression types: little-endian or float PCM, A-law, mu-law, IMA4.";
  aifc.category = Category::Audio;
  aifc.extensions = {"aifc"};
  aifc.mime = "audio/x-aifc";
  aifc.popularity = 20;
  aifc.signatures = {Signature::at(0, "'FORM' ?? ?? ?? ?? 'AIFC'")};
  registry.add_format(std::move(aifc));

  std::vector<OptionSpec> options = standard_write_options();
  options.push_back(dither_option());
  Codec codec;
  codec.format_id = "aiff";
  codec.kind = Kind::Audio;
  codec.read = read_aiff;
  codec.write = write_aiff;
  codec.write_options = options;
  codec.write_options.insert(codec.write_options.begin(), sample_format_option(aiff_formats()));
  registry.add_codec(std::move(codec));

  Codec c2;
  c2.format_id = "aifc";
  c2.kind = Kind::Audio;
  c2.read = read_aiff;
  c2.write = write_aifc;
  c2.write_options = options;
  c2.write_options.insert(c2.write_options.begin(), sample_format_option(aifc_formats()));
  registry.add_codec(std::move(c2));
}

}  // namespace cvt::audio
