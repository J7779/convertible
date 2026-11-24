// Convertible core - audio_lossless family: headerless PCM, Creative VOC,
// IFF 8SVX and NIST SPHERE.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <span>
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

// =========================================================================== raw PCM

const std::vector<std::string>& raw_formats() {
  static const std::vector<std::string> choices = {"s16le", "s16be", "s24le", "s24be", "s32le", "s32be", "u8", "s8",
                                                   "f32le", "f32be", "f64le", "f64be", "alaw",  "mulaw"};
  return choices;
}

PcmFormat raw_layout(std::string_view name) {
  if (name == "u8") return pcm_unsigned8();
  if (name == "s8") return pcm_signed(8);
  if (name == "alaw") return pcm_alaw();
  if (name == "mulaw") return pcm_mulaw();
  const std::endian order = name.ends_with("be") ? std::endian::big : std::endian::little;
  const int bits = name.substr(1, 2) == "16" ? 16 : name.substr(1, 2) == "24" ? 24 : name.substr(1, 2) == "32" ? 32 : 64;
  if (name.front() == 'f') return pcm_float(bits, order);
  return pcm_signed(bits, order);
}

Result<AnyIR> read_raw(ReadContext& ctx) {
  const std::string name = ctx.options.get_enum("raw_sample_format", "s16le", raw_formats());
  const auto rate = static_cast<std::uint32_t>(ctx.options.get_int("raw_sample_rate", 44100, 1, 1'000'000));
  const auto channels = static_cast<std::uint16_t>(ctx.options.get_int("raw_channels", 2, 1, 64));
  const PcmFormat layout = raw_layout(name);
  const std::uint64_t frame_bytes = std::uint64_t{layout.container_bytes} * channels;
  const std::uint64_t frames = ctx.input.size() / frame_bytes;
  CVT_TRY(check_sample_budget(ctx, frames * channels));
  const bool law = layout.encoding == PcmEncoding::Alaw || layout.encoding == PcmEncoding::Mulaw;
  ir::Audio audio = make_audio(rate, channels, law ? 16 : layout.bits, layout.encoding == PcmEncoding::Float);
  decode_pcm(ctx.input.first(static_cast<std::size_t>(frames * frame_bytes)), layout, audio.samples);
  if (frames * frame_bytes != ctx.input.size()) ctx.warn("The file ends with a partial sample frame, which was ignored.");
  return audio;
}

Result<Bytes> write_raw(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx));
  const std::string name = ctx.options.get_enum("raw_sample_format", "s16le", raw_formats());
  Bytes data = encode_pcm(audio.samples, audio.channels, raw_layout(name), resolved_dither(ctx.options), audio.sample_rate);
  CVT_TRY(ctx.limits.check_output(data.size()));
  return data;
}

// =========================================================================== Creative VOC

constexpr std::string_view voc_magic = "Creative Voice File\x1A";

Result<AnyIR> read_voc(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume(voc_magic)) return fail(Status::Decode, "not a Creative Voice file");
  const std::uint16_t header = r.u16le();
  if (!r.ok() || header < 26 || !r.seek(header)) return fail(Status::Decode, "the VOC header is damaged");
  std::uint32_t rate = 0;
  std::uint16_t channels = 0;
  std::uint16_t bits = 0;
  PcmFormat layout = pcm_unsigned8();
  bool have_format = false;
  // A pending extended block (type 8) overrides the next type-1 block.
  std::uint32_t ext_rate = 0;
  std::uint16_t ext_channels = 0;
  ir::Audio audio;
  std::vector<float>& samples = audio.samples;
  const auto set_format = [&](std::uint32_t new_rate, std::uint16_t new_channels, PcmFormat new_layout,
                              std::uint16_t new_bits) -> Result<void> {
    if (have_format && (new_rate != rate || new_channels != channels)) {
      ctx.warn("The VOC file changes format midway; later blocks were converted to the first format.");
      return {};
    }
    CVT_TRY(check_stream_params(new_rate, new_channels, "the VOC file"));
    rate = new_rate;
    channels = new_channels;
    layout = new_layout;
    bits = new_bits;
    have_format = true;
    return {};
  };
  const auto append = [&](ByteView data) -> Result<void> {
    if (!have_format) return fail(Status::Decode, "VOC sound data without a format");
    const std::uint64_t count = data.size() / layout.container_bytes;
    CVT_TRY(check_sample_budget(ctx, samples.size() + count));
    decode_pcm(data, layout, samples);
    return {};
  };
  std::size_t guard = 0;
  while (!r.at_end() && ++guard < 1'000'000) {
    const std::uint8_t type = r.u8();
    if (type == 0) break;
    const std::uint32_t size = r.u24le();
    if (!r.ok()) break;
    const bool truncated = size > r.remaining();
    util::ByteReader b = r.sub(truncated ? r.remaining() : size);
    switch (type) {
      case 1: {
        const std::uint8_t time_constant = b.u8();
        const std::uint8_t codec = b.u8();
        if (!b.ok()) return fail(Status::Decode, "truncated VOC sound block");
        std::uint32_t block_rate;
        std::uint16_t block_channels = 1;
        if (ext_rate != 0) {
          block_rate = ext_rate;
          block_channels = ext_channels;
          ext_rate = 0;
        } else {
          block_rate = static_cast<std::uint32_t>(std::lround(1'000'000.0 / (256 - time_constant)));
        }
        if (codec != 0) return fail(Status::Decode, "unsupported VOC compression {}", codec);
        CVT_TRY(set_format(block_rate, block_channels, pcm_unsigned8(), 8));
        CVT_TRY(append(b.rest()));
        break;
      }
      case 2: CVT_TRY(append(b.rest())); break;
      case 3: {
        const std::uint16_t length = b.u16le();
        if (have_format && b.ok()) {
          CVT_TRY(check_sample_budget(ctx, samples.size() + (std::uint64_t{length} + 1) * channels));
          samples.insert(samples.end(), (std::size_t{length} + 1) * channels, 0.0f);
        }
        break;
      }
      case 8: {
        const std::uint16_t time_constant = b.u16le();
        b.skip(1);
        const std::uint8_t mode = b.u8();
        ext_channels = mode == 1 ? 2 : 1;
        ext_rate = static_cast<std::uint32_t>(std::lround(256'000'000.0 / (ext_channels * (65536.0 - time_constant))));
        break;
      }
      case 9: {
        const std::uint32_t block_rate = b.u32le();
        const std::uint8_t block_bits = b.u8();
        const std::uint8_t block_channels = b.u8();
        const std::uint16_t codec = b.u16le();
        b.skip(4);
        if (!b.ok()) return fail(Status::Decode, "truncated VOC sound block");
        PcmFormat block_layout;
        std::uint16_t hint = block_bits;
        if (codec == 0 && block_bits == 8) {
          block_layout = pcm_unsigned8();
        } else if (codec == 4 && (block_bits == 16 || block_bits == 24 || block_bits == 32)) {
          block_layout = pcm_signed(block_bits);
        } else if (codec == 6) {
          block_layout = pcm_alaw();
          hint = 16;
        } else if (codec == 7) {
          block_layout = pcm_mulaw();
          hint = 16;
        } else {
          return fail(Status::Decode, "unsupported VOC codec {} ({} bits)", codec, block_bits);
        }
        CVT_TRY(set_format(block_rate, block_channels, block_layout, hint));
        CVT_TRY(append(b.rest()));
        break;
      }
      case 5: {
        std::string_view text = as_chars(b.rest());
        text = text.substr(0, text.find('\0'));
        tags::put(audio.tags, "comment", util::decode_text(as_bytes(text), util::TextEncoding::Latin1).text);
        break;
      }
      default: break;  // markers, repeats: ignored
    }
    if (truncated) break;
  }
  if (!have_format) return fail(Status::Decode, "the VOC file contains no sound");
  audio.sample_rate = rate;
  audio.channels = channels;
  audio.bits_per_sample = bits;
  samples.resize(samples.size() / channels * channels);
  return audio;
}

Result<Bytes> write_voc(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx, 1, 0, 255));
  const std::string choice = ctx.options.get_enum(key_sample_format, "auto", {"auto", "u8", "s16", "alaw", "mulaw"});
  std::string chosen = choice;
  if (chosen == "auto") chosen = audio.bits_per_sample != 0 && audio.bits_per_sample <= 8 ? "u8" : "s16";
  PcmFormat layout = pcm_signed(16);
  std::uint8_t bits = 16;
  std::uint16_t codec = 4;
  if (chosen == "u8") {
    layout = pcm_unsigned8();
    bits = 8;
    codec = 0;
  } else if (chosen == "alaw") {
    layout = pcm_alaw();
    bits = 8;
    codec = 6;
  } else if (chosen == "mulaw") {
    layout = pcm_mulaw();
    bits = 8;
    codec = 7;
  }
  const Bytes data = encode_pcm(audio.samples, audio.channels, layout, resolved_dither(ctx.options), audio.sample_rate);
  CVT_TRY(ctx.limits.check_output(data.size() + 64));
  util::ByteWriter out(data.size() + 64);
  out.text(voc_magic);
  out.u16le(26);
  out.u16le(0x0114);
  out.u16le(static_cast<std::uint16_t>(~0x0114 + 0x1234));
  constexpr std::size_t max_block = 0xFFFFFF;
  std::size_t at = 0;
  const std::size_t first = std::min(data.size(), max_block - 12);
  out.u8(9);
  out.u24le(static_cast<std::uint32_t>(first + 12));
  out.u32le(audio.sample_rate);
  out.u8(bits);
  out.u8(static_cast<std::uint8_t>(audio.channels));
  out.u16le(codec);
  out.zeros(4);
  out.bytes(ByteView(data).first(first));
  at = first;
  while (at < data.size()) {
    const std::size_t part = std::min(data.size() - at, max_block);
    out.u8(2);
    out.u24le(static_cast<std::uint32_t>(part));
    out.bytes(ByteView(data).subspan(at, part));
    at += part;
  }
  out.u8(0);
  return out.take();
}

// =========================================================================== IFF 8SVX

Result<AnyIR> read_8svx(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("FORM")) return fail(Status::Decode, "not an IFF file");
  r.skip(4);
  if (!r.consume("8SVX")) return fail(Status::Decode, "not an 8SVX file");
  std::uint32_t rate = 0;
  std::uint8_t compression = 0;
  bool have_header = false;
  std::uint32_t chan = 2;
  ByteView body;
  bool have_body = false;
  tags::TagMap tag_map;
  std::size_t guard = 0;
  while (r.remaining() >= 8 && ++guard < 100000) {
    const std::string_view id = r.chars(4);
    const std::uint32_t size = r.u32be();
    const bool truncated = size > r.remaining();
    const ByteView data = r.bytes(truncated ? r.remaining() : size);
    if (id == "VHDR") {
      util::ByteReader h(data);
      h.skip(12);
      rate = h.u16be();
      h.skip(1);
      compression = h.u8();
      have_header = h.ok();
    } else if (id == "BODY") {
      body = data;
      have_body = true;
    } else if (id == "CHAN" && data.size() >= 4) {
      chan = util::ByteReader(data).u32be();
    } else if (id == "NAME") {
      tags::put(tag_map, "title", to_string(data));
    } else if (id == "AUTH") {
      tags::put(tag_map, "artist", to_string(data));
    } else if (id == "ANNO") {
      tags::put(tag_map, "comment", to_string(data));
    } else if (id == "(c) ") {
      tags::put(tag_map, "copyright", to_string(data));
    }
    if (truncated) break;
    if (size % 2 == 1) r.skip(1);
  }
  if (!have_header) return fail(Status::Decode, "the 8SVX file has no VHDR chunk");
  if (!have_body) return fail(Status::Decode, "the 8SVX file has no BODY chunk");
  const std::uint16_t channels = chan == 6 ? 2 : 1;
  CVT_TRY(check_stream_params(rate, channels, "the 8SVX file"));
  ir::Audio audio = make_audio(rate, channels, 8);
  audio.tags = std::move(tag_map);
  std::vector<float> mono;
  if (compression == 0) {
    CVT_TRY(check_sample_budget(ctx, body.size()));
    decode_pcm(body, pcm_signed(8), mono);
  } else if (compression == 1) {
    // Fibonacci-delta: a pad byte, the initial value, then two deltas per byte.
    static constexpr std::array<int, 16> table = {-34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21};
    CVT_TRY(check_sample_budget(ctx, body.size() * 2));
    const std::span<const std::uint8_t> bytes = as_u8(body);
    // Stereo streams compress each channel half separately.
    const std::size_t parts = channels;
    const std::size_t part_size = bytes.size() / parts;
    for (std::size_t p = 0; p < parts; ++p) {
      const std::span<const std::uint8_t> part = bytes.subspan(p * part_size, part_size);
      if (part.size() < 2) continue;
      int value = static_cast<std::int8_t>(part[1]);
      for (std::size_t i = 2; i < part.size(); ++i) {
        for (const int nibble : {part[i] >> 4, part[i] & 0x0F}) {
          value = static_cast<std::int8_t>(static_cast<std::uint8_t>(value + table[static_cast<std::size_t>(nibble)]));
          mono.push_back(static_cast<float>(value) / 128.0f);
        }
      }
    }
  } else {
    return fail(Status::Decode, "unsupported 8SVX compression {}", compression);
  }
  if (channels == 2) {
    const std::size_t half = mono.size() / 2;
    audio.samples.resize(half * 2);
    for (std::size_t i = 0; i < half; ++i) {
      audio.samples[i * 2] = mono[i];
      audio.samples[i * 2 + 1] = mono[half + i];
    }
  } else {
    audio.samples = std::move(mono);
  }
  return audio;
}

void iff_chunk(util::ByteWriter& out, std::string_view id, ByteView body) {
  out.text(id);
  out.u32be(static_cast<std::uint32_t>(body.size()));
  out.bytes(body);
  if (body.size() % 2 == 1) out.u8(0);
}

Result<Bytes> write_8svx(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx, 1, 65535, 2));
  const std::vector<std::int32_t> ints = quantize_for(audio, 8, ctx.options);
  const std::size_t frames = audio.frame_count();
  if (frames * audio.channels + 64 > 0xFFFFFFF0ull) return fail(Status::Encode, "the audio is too long for 8SVX");
  util::ByteWriter vhdr;
  vhdr.u32be(static_cast<std::uint32_t>(frames));
  vhdr.u32be(0);
  vhdr.u32be(0);
  vhdr.u16be(static_cast<std::uint16_t>(audio.sample_rate));
  vhdr.u8(1);
  vhdr.u8(0);
  vhdr.u32be(0x10000);
  util::ByteWriter body;
  for (std::size_t c = 0; c < audio.channels; ++c) {
    for (std::size_t f = 0; f < frames; ++f) body.i8(static_cast<std::int8_t>(ints[f * audio.channels + c]));
  }
  util::ByteWriter form;
  form.text("8SVX");
  iff_chunk(form, "VHDR", vhdr.view());
  if (!options::resolved_strip_metadata(ctx.options)) {
    if (const auto it = audio.tags.find("title"); it != audio.tags.end()) iff_chunk(form, "NAME", as_bytes(it->second));
    if (const auto it = audio.tags.find("artist"); it != audio.tags.end()) iff_chunk(form, "AUTH", as_bytes(it->second));
    if (const auto it = audio.tags.find("copyright"); it != audio.tags.end()) iff_chunk(form, "(c) ", as_bytes(it->second));
    if (const auto it = audio.tags.find("comment"); it != audio.tags.end()) iff_chunk(form, "ANNO", as_bytes(it->second));
  }
  if (audio.channels == 2) {
    util::ByteWriter chan;
    chan.u32be(6);
    iff_chunk(form, "CHAN", chan.view());
  }
  iff_chunk(form, "BODY", body.view());
  util::ByteWriter out;
  out.text("FORM");
  out.u32be(static_cast<std::uint32_t>(form.size()));
  out.bytes(form.view());
  return out.take();
}

// =========================================================================== NIST SPHERE

Result<AnyIR> read_sphere(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  if (!text.starts_with("NIST_1A")) return fail(Status::Decode, "not a NIST SPHERE file");
  const std::string_view second = text.size() > 16 ? text.substr(8, 8) : std::string_view{};
  const auto header_size = util::parse_int(util::trim(second));
  if (!header_size || *header_size < 16 || static_cast<std::uint64_t>(*header_size) > ctx.input.size() ||
      *header_size > 1'000'000) {
    return fail(Status::Decode, "the SPHERE header size is invalid");
  }
  std::map<std::string, std::string, std::less<>> fields;
  for (const std::string_view line : util::split_lines(text.substr(16, static_cast<std::size_t>(*header_size) - 16))) {
    const std::string_view trimmed = util::trim(line);
    if (trimmed == "end_head") break;
    const std::size_t space = trimmed.find(' ');
    if (space == std::string_view::npos) continue;
    const std::string_view key = trimmed.substr(0, space);
    std::string_view rest = util::trim(trimmed.substr(space + 1));
    const std::size_t type_end = rest.find(' ');
    if (type_end == std::string_view::npos) continue;
    std::string_view v = rest.substr(type_end + 1);
    if (rest.starts_with("-s")) {
      // "-sN value": take exactly N characters when given.
      const auto n = util::parse_int(rest.substr(2, type_end - 2));
      if (n && *n >= 0 && static_cast<std::size_t>(*n) <= v.size()) v = v.substr(0, static_cast<std::size_t>(*n));
    }
    fields.emplace(std::string(key), std::string(util::trim(v)));
  }
  const auto get_int = [&](std::string_view key, std::int64_t fallback) {
    const auto it = fields.find(key);
    if (it == fields.end()) return fallback;
    return util::parse_int(it->second).value_or(fallback);
  };
  const std::int64_t channels = get_int("channel_count", 1);
  const std::int64_t rate = get_int("sample_rate", 0);
  const std::int64_t bytes = get_int("sample_n_bytes", 2);
  const std::int64_t declared = get_int("sample_count", -1);
  if (rate <= 0 || rate > 10'000'000) return fail(Status::Decode, "the SPHERE file has no valid sample rate");
  if (channels <= 0 || channels > 255) return fail(Status::Decode, "the SPHERE file has an invalid channel count");
  const auto coding_it = fields.find("sample_coding");
  const std::string coding = coding_it == fields.end() ? "pcm" : util::to_lower(coding_it->second);
  const auto order_it = fields.find("sample_byte_format");
  const std::string order = order_it == fields.end() ? "01" : order_it->second;
  PcmFormat layout;
  std::uint16_t hint;
  if (coding.starts_with("pcm,") || coding.find("shorten") != std::string::npos) {
    return fail(Status::Decode, "compressed SPHERE files ({}) are not supported", coding);
  }
  if (coding == "ulaw" || coding == "mu-law") {
    layout = pcm_mulaw();
    hint = 16;
  } else if (coding == "alaw") {
    layout = pcm_alaw();
    hint = 16;
  } else if (coding == "pcm") {
    if (bytes < 1 || bytes > 4) return fail(Status::Decode, "unsupported SPHERE sample size {}", bytes);
    layout = pcm_signed(static_cast<int>(bytes * 8), order == "10" ? std::endian::big : std::endian::little);
    hint = static_cast<std::uint16_t>(bytes * 8);
  } else {
    return fail(Status::Decode, "unsupported SPHERE sample coding '{}'", coding);
  }
  ir::Audio audio = make_audio(static_cast<std::uint32_t>(rate), static_cast<std::uint16_t>(channels), hint);
  ByteView data = ctx.input.subspan(static_cast<std::size_t>(*header_size));
  const std::uint64_t frame_bytes = std::uint64_t{layout.container_bytes} * static_cast<std::uint64_t>(channels);
  std::uint64_t frames = data.size() / frame_bytes;
  if (declared >= 0 && static_cast<std::uint64_t>(declared) < frames) frames = static_cast<std::uint64_t>(declared);
  CVT_TRY(check_sample_budget(ctx, frames * static_cast<std::uint64_t>(channels)));
  decode_pcm(data.first(static_cast<std::size_t>(frames * frame_bytes)), layout, audio.samples);
  return audio;
}

Result<Bytes> write_sphere(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx));
  std::string choice = ctx.options.get_enum(key_sample_format, "auto", {"auto", "s16", "s24", "s32", "alaw", "mulaw"});
  if (choice == "auto") {
    choice = audio.bits_per_sample > 16 && !audio.float_samples ? (audio.bits_per_sample > 24 ? "s32" : "s24") : "s16";
  }
  PcmFormat layout;
  std::string coding = "pcm";
  if (choice == "mulaw") {
    layout = pcm_mulaw();
    coding = "ulaw";
  } else if (choice == "alaw") {
    layout = pcm_alaw();
    coding = "alaw";
  } else {
    layout = pcm_signed(choice == "s24" ? 24 : choice == "s32" ? 32 : 16);
  }
  const Bytes data = encode_pcm(audio.samples, audio.channels, layout, resolved_dither(ctx.options), audio.sample_rate);
  std::string header = "NIST_1A\n   1024\n";
  header += std::format("sample_count -i {}\n", audio.frame_count());
  header += std::format("sample_rate -i {}\n", audio.sample_rate);
  header += std::format("channel_count -i {}\n", audio.channels);
  header += std::format("sample_n_bytes -i {}\n", layout.container_bytes);
  if (layout.container_bytes > 1) header += "sample_byte_format -s2 01\n";
  header += std::format("sample_coding -s{} {}\n", coding.size(), coding);
  if (layout.encoding == PcmEncoding::Signed) header += std::format("sample_sig_bits -i {}\n", layout.bits);
  header += "end_head\n";
  header.resize(1024, ' ');
  CVT_TRY(ctx.limits.check_output(data.size() + 1024));
  util::ByteWriter out(data.size() + 1024);
  out.text(header);
  out.bytes(data);
  return out.take();
}

}  // namespace

void register_raw(Registry& registry) {
  FormatInfo info;
  info.id = "pcm";
  info.name = "Raw PCM audio";
  info.description = "Headerless sample data; the layout is given by options.";
  info.category = Category::Audio;
  info.extensions = {"pcm"};
  info.mime = "audio/L16";
  info.popularity = 15;
  registry.add_format(std::move(info));

  const OptionSpec format_option = OptionSpec::choice("raw_sample_format", "Raw sample format", "s16le", raw_formats(),
                                                      "Encoding, bit depth and byte order of the raw samples.");
  Codec codec;
  codec.format_id = "pcm";
  codec.kind = Kind::Audio;
  codec.read = read_raw;
  codec.write = write_raw;
  codec.read_options = {
      format_option,
      OptionSpec::integer("raw_sample_rate", "Raw sample rate", 44100, 1, 1'000'000, "Sample frames per second."),
      OptionSpec::integer("raw_channels", "Raw channels", 2, 1, 64, "Interleaved channels per frame."),
  };
  codec.write_options = standard_write_options();
  codec.write_options.insert(codec.write_options.begin(), format_option);
  codec.write_options.push_back(dither_option());
  registry.add_codec(std::move(codec));
}

void register_voc(Registry& registry) {
  FormatInfo info;
  info.id = "voc";
  info.name = "Creative Voice audio";
  info.description = "Sound Blaster era sample format.";
  info.category = Category::Audio;
  info.extensions = {"voc"};
  info.mime = "audio/x-voc";
  info.popularity = 8;
  info.signatures = {Signature::at(0, "'Creative Voice File' 1A")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "voc";
  codec.kind = Kind::Audio;
  codec.read = read_voc;
  codec.write = write_voc;
  codec.write_options = standard_write_options();
  codec.write_options.insert(codec.write_options.begin(),
                             sample_format_option({"auto", "u8", "s16", "alaw", "mulaw"}));
  codec.write_options.push_back(dither_option());
  registry.add_codec(std::move(codec));
}

void register_svx(Registry& registry) {
  FormatInfo info;
  info.id = "8svx";
  info.name = "Amiga 8SVX audio";
  info.description = "IFF 8-bit sampled voice from the Amiga.";
  info.category = Category::Audio;
  info.extensions = {"8svx", "svx", "iff"};
  info.mime = "audio/x-8svx";
  info.popularity = 5;
  info.signatures = {Signature::at(0, "'FORM' ?? ?? ?? ?? '8SVX'")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "8svx";
  codec.kind = Kind::Audio;
  codec.read = read_8svx;
  codec.write = write_8svx;
  codec.write_options = standard_write_options();
  codec.write_options.push_back(dither_option());
  registry.add_codec(std::move(codec));
}

void register_sphere(Registry& registry) {
  FormatInfo info;
  info.id = "sph";
  info.name = "NIST SPHERE audio";
  info.description = "Speech corpus audio with a text header.";
  info.category = Category::Audio;
  info.extensions = {"sph", "nist"};
  info.mime = "audio/x-nist-sphere";
  info.popularity = 5;
  info.signatures = {Signature::at(0, "'NIST_1A' 0A")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "sph";
  codec.kind = Kind::Audio;
  codec.read = read_sphere;
  codec.write = write_sphere;
  codec.write_options = standard_write_options();
  codec.write_options.insert(codec.write_options.begin(),
                             sample_format_option({"auto", "s16", "s24", "s32", "alaw", "mulaw"}));
  codec.write_options.push_back(dither_option());
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::audio
