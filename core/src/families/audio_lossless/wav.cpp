// Convertible core - audio_lossless family: WAVE (RIFF, RF64, BW64) and Sony Wave64.
//
// Reads PCM (u8, s16/s24/s32 and odd widths in larger containers), IEEE
// float 32/64, A-law, mu-law, IMA ADPCM, Microsoft ADPCM and GSM 6.10, plain
// or WAVE_FORMAT_EXTENSIBLE (channel masks honoured). Streamed files whose
// sizes were never filled in (0 or 0xFFFFFFFF, or larger than the file) are
// read to the end. LIST/INFO and ID3 tags are read; bext, cue, smpl, PEAK,
// JUNK and unknown chunks are skipped; odd chunk sizes are padded as the
// RIFF rules say (with a fallback for writers that forgot the pad byte).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "util/bytes.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/pcm.hpp"
#include "families/audio_lossless/tags.hpp"
#include "families/audio_lossless/wav.hpp"

namespace cvt::audio {

namespace {

constexpr std::array<std::uint8_t, 14> ksdataformat_tail = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                            0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

// Wave64 GUIDs: the first four bytes are a FourCC, the rest is fixed.
constexpr std::array<std::uint8_t, 16> w64_riff = {'r', 'i', 'f', 'f', 0x2E, 0x91, 0xCF, 0x11,
                                                   0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
constexpr std::array<std::uint8_t, 12> w64_tail = {0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1,
                                                   0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};

bool plausible_chunk_id(ByteView data, std::size_t at) {
  if (at + 4 > data.size()) return false;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto c = std::to_integer<std::uint8_t>(data[at + i]);
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

}  // namespace

// =========================================================================== fmt

Result<WaveFormat> parse_wave_format(ByteView body) {
  util::ByteReader r(body);
  WaveFormat f;
  f.tag = r.u16le();
  f.channels = r.u16le();
  f.sample_rate = r.u32le();
  f.byte_rate = r.u32le();
  f.block_align = r.u16le();
  f.bits = r.u16le();
  if (!r.ok()) return fail(Status::Decode, "the WAVE format chunk is truncated");
  const std::uint16_t extra = r.remaining() >= 2 ? r.u16le() : 0;
  ByteView ext = r.bytes(std::min<std::size_t>(extra, r.remaining()));
  util::ByteReader e(ext);
  if (f.tag == wave_tag::extensible) {
    if (ext.size() < 22) return fail(Status::Decode, "WAVE_FORMAT_EXTENSIBLE header is truncated");
    f.extensible = true;
    f.valid_bits = e.u16le();
    f.channel_mask = e.u32le();
    const ByteView guid = e.bytes(16);
    if (::cvt::util::compare_bytes(guid.data() + 2, ksdataformat_tail.data(), ksdataformat_tail.size()) != 0) {
      return fail(Status::Decode, "unsupported WAVE_FORMAT_EXTENSIBLE sub-format");
    }
    f.tag = static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(guid[0]) |
                                       (std::to_integer<std::uint16_t>(guid[1]) << 8));
  } else if (f.tag == wave_tag::ima_adpcm && ext.size() >= 2) {
    f.samples_per_block = e.u16le();
  } else if (f.tag == wave_tag::ms_adpcm && ext.size() >= 4) {
    f.samples_per_block = e.u16le();
    const std::uint16_t count = e.u16le();
    for (std::uint16_t i = 0; i < count && e.remaining() >= 4; ++i) {
      f.coefficients.c1.push_back(e.i16le());
      f.coefficients.c2.push_back(e.i16le());
    }
  }
  CVT_TRY(check_stream_params(f.sample_rate, f.channels, "the WAVE file"));
  return f;
}

Result<std::optional<PcmFormat>> wave_pcm_layout(const WaveFormat& f) {
  const auto container_for = [&](int bits) -> int {
    if (f.channels != 0 && f.block_align % f.channels == 0) {
      const int c = f.block_align / f.channels;
      if (c * 8 >= bits && c >= 1) return c;
    }
    return (bits + 7) / 8;
  };
  switch (f.tag) {
    case wave_tag::pcm: {
      if (f.bits == 0 || f.bits > 32) return fail(Status::Decode, "unsupported PCM bit depth {}", f.bits);
      const int container = container_for(f.bits);
      if (container > 4) return fail(Status::Decode, "unsupported PCM sample size of {} bytes", container);
      PcmFormat p;
      p.container_bytes = static_cast<std::uint16_t>(container);
      p.encoding = container == 1 ? PcmEncoding::Unsigned : PcmEncoding::Signed;
      const int valid = f.valid_bits != 0 && f.valid_bits <= container * 8 ? f.valid_bits : f.bits;
      p.bits = static_cast<std::uint16_t>(std::min(valid, container * 8));
      return std::optional<PcmFormat>(p);
    }
    case wave_tag::ieee_float: {
      const int container = container_for(f.bits);
      if (container != 4 && container != 8) return fail(Status::Decode, "unsupported float sample size {}", f.bits);
      return std::optional<PcmFormat>(pcm_float(container * 8));
    }
    case wave_tag::alaw: return std::optional<PcmFormat>(pcm_alaw());
    case wave_tag::mulaw: return std::optional<PcmFormat>(pcm_mulaw());
    default: return std::optional<PcmFormat>();
  }
}

Result<void> decode_wave_data(const WaveFormat& f, ByteView data, std::uint64_t fact_frames, const Context& ctx,
                              ir::Audio& audio) {
  audio.sample_rate = f.sample_rate;
  audio.channels = f.channels;
  audio.channel_mask = f.channel_mask;
  CVT_TRY_ASSIGN(const std::optional<PcmFormat> layout, wave_pcm_layout(f));
  if (layout) {
    const std::uint64_t frame_bytes = std::uint64_t{layout->container_bytes} * f.channels;
    const std::uint64_t frames = data.size() / frame_bytes;
    CVT_TRY(check_sample_budget(ctx, frames * f.channels));
    audio.bits_per_sample = layout->encoding == PcmEncoding::Alaw || layout->encoding == PcmEncoding::Mulaw
                                ? 16
                                : layout->bits;
    audio.float_samples = layout->encoding == PcmEncoding::Float;
    audio.samples.reserve(static_cast<std::size_t>(frames * f.channels));
    decode_pcm(data.first(static_cast<std::size_t>(frames * frame_bytes)), *layout, audio.samples);
    return {};
  }
  if (f.block_align == 0) return fail(Status::Decode, "the WAVE file has a zero block size");
  const std::uint64_t blocks = (data.size() + f.block_align - 1) / f.block_align;
  audio.bits_per_sample = 16;
  switch (f.tag) {
    case wave_tag::ima_adpcm: {
      const std::size_t per_block = ima_wav_frames_per_block(f.block_align, f.channels);
      CVT_TRY(check_sample_budget(ctx, blocks * per_block * f.channels));
      CVT_TRY(decode_ima_wav(data, f.channels, f.block_align, audio.samples));
      break;
    }
    case wave_tag::ms_adpcm: {
      const std::size_t per_block = ms_adpcm_frames_per_block(f.block_align, f.channels);
      CVT_TRY(check_sample_budget(ctx, blocks * per_block * f.channels));
      const MsAdpcmCoefficients coefficients =
          f.coefficients.c1.empty() ? ms_adpcm_standard_coefficients() : f.coefficients;
      CVT_TRY(decode_ms_adpcm(data, f.channels, f.block_align, coefficients, audio.samples));
      break;
    }
    case wave_tag::gsm610: {
      if (f.channels != 1) return fail(Status::Decode, "GSM 6.10 audio must be mono");
      CVT_TRY(check_sample_budget(ctx, blocks * 320));
      CVT_TRY(decode_gsm610_wav(data, audio.samples));
      break;
    }
    default:
      return fail(Status::Decode, "unsupported WAVE encoding 0x{:04X}", f.tag);
  }
  if (fact_frames != 0) set_frame_count(audio, fact_frames);
  return {};
}

namespace {

// =========================================================================== reading

struct ChunkScan {
  std::optional<WaveFormat> format;
  ByteView data;
  bool has_data = false;
  bool data_truncated = false;
  std::uint64_t fact_frames = 0;
  tags::TagMap tags;
};

void handle_tag_chunk(std::string_view id, ByteView body, ChunkScan& scan) {
  if (id == "LIST" && body.size() >= 4 && as_chars(body.first(4)) == "INFO") {
    tags::parse_riff_info(body.subspan(4), scan.tags);
  } else if (id == "id3 " || id == "ID3 " || id == "ID32") {
    tags::parse_id3v2(body, scan.tags);
  } else if (id == "bext" && body.size() >= 256) {
    // Broadcast WAVE: only the free-text description is user content.
    std::string_view description = as_chars(body.first(256));
    description = description.substr(0, description.find('\0'));
    tags::put(scan.tags, "comment", description);
  }
}

Result<ChunkScan> scan_riff(ByteView input, ReadContext& ctx) {
  util::ByteReader r(input);
  const std::string_view magic = r.chars(4);
  r.skip(4);
  if (!r.consume("WAVE")) return fail(Status::Decode, "not a WAVE file");
  const bool rf64 = magic == "RF64" || magic == "BW64";
  if (magic != "RIFF" && !rf64) return fail(Status::Decode, "not a WAVE file");
  ChunkScan scan;
  std::uint64_t ds64_data = 0;
  std::size_t pos = 12;
  std::size_t guard = 0;
  while (pos + 8 <= input.size() && ++guard < 100000) {
    util::ByteReader h(input.subspan(pos, 8));
    const std::string_view id = h.chars(4);
    std::uint64_t size = h.u32le();
    const std::size_t body_at = pos + 8;
    const std::size_t remaining = input.size() - body_at;
    if (rf64 && id == "data" && size == 0xFFFFFFFFu && ds64_data != 0) size = ds64_data;
    if (id == "data") {
      bool streamed = size == 0 || size == 0xFFFFFFFFu || (size > remaining);
      if (size > remaining && size != 0xFFFFFFFFu && size != 0) {
        // Common for files cut short or written by streaming tools.
        scan.data_truncated = size - remaining > 4096;
      }
      const std::size_t take = streamed ? remaining : static_cast<std::size_t>(size);
      scan.data = input.subspan(body_at, take);
      scan.has_data = true;
      if (streamed) break;
    }
    if (size > remaining) {
      if (id != "data") break;  // a damaged trailing chunk: stop, keep what we have
    }
    const ByteView body = input.subspan(body_at, static_cast<std::size_t>(std::min<std::uint64_t>(size, remaining)));
    if (id == "fmt ") {
      if (!scan.format) {
        CVT_TRY_ASSIGN(scan.format, parse_wave_format(body));
      }
    } else if (id == "ds64" && body.size() >= 24) {
      util::ByteReader d(body);
      d.skip(8);
      ds64_data = d.u64le();
    } else if (id == "fact" && body.size() >= 4) {
      scan.fact_frames = util::ByteReader(body).u32le();
    } else {
      handle_tag_chunk(id, body, scan);
    }
    std::size_t next = body_at + static_cast<std::size_t>(size);
    if (size % 2 == 1) {
      // RIFF pads odd chunks; some writers forget. Prefer whichever lands on a chunk id.
      if (next + 1 <= input.size() && (plausible_chunk_id(input, next + 1) || !plausible_chunk_id(input, next))) ++next;
    }
    pos = next;
  }
  if (!scan.format) return fail(Status::Decode, "the WAVE file has no format chunk");
  if (!scan.has_data) return fail(Status::Decode, "the WAVE file has no audio data");
  if (scan.data_truncated) ctx.warn("The audio data is shorter than the header says; the file may be truncated.");
  return scan;
}

Result<ChunkScan> scan_w64(ByteView input, ReadContext& ctx) {
  util::ByteReader r(input);
  const ByteView riff = r.bytes(16);
  r.skip(8);
  const ByteView wave = r.bytes(16);
  if (!r.ok() || ::cvt::util::compare_bytes(riff.data(), w64_riff.data(), 16) != 0 || as_chars(wave.first(4)) != "wave" ||
      ::cvt::util::compare_bytes(wave.data() + 4, w64_tail.data(), 12) != 0) {
    return fail(Status::Decode, "not a Wave64 file");
  }
  ChunkScan scan;
  std::size_t pos = 40;
  std::size_t guard = 0;
  while (pos + 24 <= input.size() && ++guard < 100000) {
    util::ByteReader h(input.subspan(pos, 24));
    const ByteView guid = h.bytes(16);
    const std::uint64_t size = h.u64le();
    const std::string_view id = as_chars(guid.first(4));
    if (size < 24) break;
    const std::size_t body_at = pos + 24;
    const std::uint64_t body_size = size - 24;
    const std::size_t remaining = input.size() - body_at;
    if (id == "data") {
      const bool streamed = body_size > remaining;
      if (streamed) scan.data_truncated = body_size - remaining > 4096;
      scan.data = input.subspan(body_at, streamed ? remaining : static_cast<std::size_t>(body_size));
      scan.has_data = true;
      if (streamed) break;
    }
    if (body_size > remaining) break;
    const ByteView body = input.subspan(body_at, static_cast<std::size_t>(body_size));
    if (id == "fmt ") {
      if (!scan.format) {
        CVT_TRY_ASSIGN(scan.format, parse_wave_format(body));
      }
    } else if (id == "fact" && body.size() >= 4) {
      scan.fact_frames = util::ByteReader(body).u64le();
    } else if (id == "list") {
      if (body.size() >= 4 && as_chars(body.first(4)) == "INFO") tags::parse_riff_info(body.subspan(4), scan.tags);
    }
    std::uint64_t next = body_at + body_size;
    next = (next + 7) & ~std::uint64_t{7};
    if (next > input.size()) break;
    pos = static_cast<std::size_t>(next);
  }
  if (!scan.format) return fail(Status::Decode, "the Wave64 file has no format chunk");
  if (!scan.has_data) return fail(Status::Decode, "the Wave64 file has no audio data");
  if (scan.data_truncated) ctx.warn("The audio data is shorter than the header says; the file may be truncated.");
  return scan;
}

Result<AnyIR> finish_read(ChunkScan& scan, ReadContext& ctx) {
  ir::Audio audio;
  CVT_TRY(ctx.checkpoint(0.0, "Decoding"));
  CVT_TRY(decode_wave_data(*scan.format, scan.data, scan.fact_frames, ctx, audio));
  audio.tags = std::move(scan.tags);
  return audio;
}

Result<AnyIR> read_wav(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ChunkScan scan, scan_riff(ctx.input, ctx));
  return finish_read(scan, ctx);
}

Result<AnyIR> read_w64(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ChunkScan scan, scan_w64(ctx.input, ctx));
  return finish_read(scan, ctx);
}

// =========================================================================== writing

const std::vector<std::string>& wav_sample_formats() {
  static const std::vector<std::string> choices = {"auto", "u8",   "s16",   "s24",       "s32",     "f32",
                                                   "f64",  "alaw", "mulaw", "ima_adpcm", "ms_adpcm"};
  return choices;
}

}  // namespace

Result<WaveEncoded> encode_wave_payload(const ir::Audio& audio, const WriteContext& ctx, std::string_view choice) {
  WaveEncoded out;
  WaveFormat& f = out.format;
  f.channels = audio.channels;
  f.sample_rate = audio.sample_rate;
  f.channel_mask = dsp::effective_channel_mask(audio.channels, audio.channel_mask);
  const std::uint64_t frames = audio.frame_count();
  out.frames = frames;
  if (choice == "auto") {
    if (audio.float_samples) {
      choice = audio.bits_per_sample == 64 ? "f64" : "f32";
    } else if (audio.bits_per_sample == 0) {
      choice = "s16";
    } else if (audio.bits_per_sample <= 8) {
      choice = "u8";
    } else if (audio.bits_per_sample <= 16) {
      choice = "s16";
    } else if (audio.bits_per_sample <= 24) {
      choice = "s24";
    } else {
      choice = "s32";
    }
  }
  const dsp::Dither dither = resolved_dither(ctx.options);
  if (choice == "ima_adpcm" || choice == "ms_adpcm") {
    const bool ima = choice == "ima_adpcm";
    if (!ima && audio.channels > 2) return fail(Status::Encode, "Microsoft ADPCM holds at most two channels");
    const std::vector<std::int32_t> ints = quantize_for(audio, 16, ctx.options);
    const std::vector<std::int16_t> pcm16(ints.begin(), ints.end());
    const std::size_t scale = audio.sample_rate <= 11025 ? 1 : audio.sample_rate <= 22050 ? 2 : 4;
    f.block_align = static_cast<std::uint16_t>(256 * audio.channels * scale);
    if (f.block_align == 0 || 256u * audio.channels * scale > 65535) {
      return fail(Status::Encode, "too many channels for ADPCM in WAVE");
    }
    f.bits = 4;
    if (ima) {
      f.tag = wave_tag::ima_adpcm;
      f.samples_per_block = static_cast<std::uint16_t>(ima_wav_frames_per_block(f.block_align, f.channels));
      out.data = encode_ima_wav(pcm16, audio.channels, f.block_align);
    } else {
      f.tag = wave_tag::ms_adpcm;
      f.samples_per_block = static_cast<std::uint16_t>(ms_adpcm_frames_per_block(f.block_align, f.channels));
      f.coefficients = ms_adpcm_standard_coefficients();
      out.data = encode_ms_adpcm(pcm16, audio.channels, f.block_align);
    }
    f.byte_rate = static_cast<std::uint32_t>(std::uint64_t{f.sample_rate} * f.block_align / f.samples_per_block);
    out.needs_fact = true;
    return out;
  }
  const ParsedSampleFormat parsed = parse_sample_format(choice);
  if (!parsed.ok || (parsed.bits == 8 && parsed.encoding == PcmEncoding::Signed)) {
    return fail(Status::InvalidArgument, "sample format '{}' is not available for WAVE", choice);
  }
  PcmFormat layout;
  switch (parsed.encoding) {
    case PcmEncoding::Unsigned: layout = pcm_unsigned8(); f.tag = wave_tag::pcm; break;
    case PcmEncoding::Signed: layout = pcm_signed(parsed.bits); f.tag = wave_tag::pcm; break;
    case PcmEncoding::Float: layout = pcm_float(parsed.bits); f.tag = wave_tag::ieee_float; out.needs_fact = true; break;
    case PcmEncoding::Alaw: layout = pcm_alaw(); f.tag = wave_tag::alaw; out.needs_fact = true; break;
    case PcmEncoding::Mulaw: layout = pcm_mulaw(); f.tag = wave_tag::mulaw; out.needs_fact = true; break;
  }
  f.bits = static_cast<std::uint16_t>(layout.container_bytes * 8);
  f.valid_bits = layout.bits;
  f.block_align = static_cast<std::uint16_t>(layout.container_bytes * audio.channels);
  f.byte_rate = f.sample_rate * f.block_align;
  const bool default_mask = f.channel_mask == dsp::default_channel_mask(audio.channels);
  f.extensible = (f.tag == wave_tag::pcm || f.tag == wave_tag::ieee_float) &&
                 (audio.channels > 2 || (f.tag == wave_tag::pcm && f.bits > 16) || !default_mask);
  out.data = encode_pcm(audio.samples, audio.channels, layout, dither, audio.sample_rate);
  return out;
}

Bytes wave_format_body(const WaveFormat& f) {
  util::ByteWriter w;
  w.u16le(f.extensible ? wave_tag::extensible : f.tag);
  w.u16le(f.channels);
  w.u32le(f.sample_rate);
  w.u32le(f.byte_rate);
  w.u16le(f.block_align);
  w.u16le(f.bits);
  if (f.extensible) {
    w.u16le(22);
    w.u16le(f.valid_bits != 0 ? f.valid_bits : f.bits);
    w.u32le(f.channel_mask);
    w.u16le(f.tag);
    w.bytes(std::span<const std::uint8_t>(ksdataformat_tail));
  } else if (f.tag == wave_tag::ima_adpcm) {
    w.u16le(2);
    w.u16le(f.samples_per_block);
  } else if (f.tag == wave_tag::ms_adpcm) {
    w.u16le(static_cast<std::uint16_t>(4 + 4 * f.coefficients.c1.size()));
    w.u16le(f.samples_per_block);
    w.u16le(static_cast<std::uint16_t>(f.coefficients.c1.size()));
    for (std::size_t i = 0; i < f.coefficients.c1.size(); ++i) {
      w.i16le(f.coefficients.c1[i]);
      w.i16le(f.coefficients.c2[i]);
    }
  } else if (f.tag != wave_tag::pcm) {
    w.u16le(0);
  }
  return w.take();
}

namespace {

void riff_chunk(util::ByteWriter& out, std::string_view id, ByteView body) {
  out.text(id);
  out.u32le(static_cast<std::uint32_t>(body.size()));
  out.bytes(body);
  if (body.size() % 2 == 1) out.u8(0);
}

Result<Bytes> write_wav(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx, 1, 0xFFFFFFFFu, 0));
  CVT_TRY(ctx.checkpoint(0.1, "Encoding"));
  const std::string choice = ctx.options.get_enum(key_sample_format, "auto", wav_sample_formats());
  CVT_TRY_ASSIGN(WaveEncoded encoded, encode_wave_payload(audio, ctx, choice));
  const Bytes fmt = wave_format_body(encoded.format);
  const Bytes info = options::resolved_strip_metadata(ctx.options) ? Bytes{} : tags::riff_info_chunk(audio.tags);
  const std::uint64_t data_size = encoded.data.size();
  const std::uint64_t body_size = 4 + 8 + fmt.size() + (encoded.needs_fact ? 12 : 0) + info.size() + 8 + data_size +
                                  (data_size % 2);
  std::string container = ctx.options.get_enum("wav_container", "auto", {"auto", "riff", "rf64", "bw64"});
  if (container == "auto") container = body_size > 0xFFFFFFF0ull ? "rf64" : "riff";
  if (container == "riff" && body_size > 0xFFFFFFFFull) {
    return fail(Status::Encode, "the audio is larger than 4 GB; choose the RF64 container");
  }
  CVT_TRY(ctx.limits.check_output(body_size + 48));
  const bool big = container != "riff";
  util::ByteWriter out(static_cast<std::size_t>(body_size + 48));
  if (big) {
    out.text(container == "bw64" ? "BW64" : "RF64");
    out.u32le(0xFFFFFFFFu);
    out.text("WAVE");
    out.text("ds64");
    out.u32le(28);
    out.u64le(body_size + 36);
    out.u64le(data_size);
    out.u64le(encoded.frames);
    out.u32le(0);
  } else {
    out.text("RIFF");
    out.u32le(static_cast<std::uint32_t>(body_size));
    out.text("WAVE");
  }
  riff_chunk(out, "fmt ", fmt);
  if (encoded.needs_fact) {
    out.text("fact");
    out.u32le(4);
    out.u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(encoded.frames, 0xFFFFFFFFu)));
  }
  out.bytes(info);
  out.text("data");
  out.u32le(big ? 0xFFFFFFFFu : static_cast<std::uint32_t>(data_size));
  out.bytes(encoded.data);
  if (data_size % 2 == 1) out.u8(0);
  return out.take();
}

void w64_chunk(util::ByteWriter& out, std::string_view id, ByteView body) {
  out.text(id);
  out.bytes(std::span<const std::uint8_t>(w64_tail));
  out.u64le(body.size() + 24);
  out.bytes(body);
  out.align(8);
}

Result<Bytes> write_w64(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx, 1, 0xFFFFFFFFu, 0));
  CVT_TRY(ctx.checkpoint(0.1, "Encoding"));
  const std::string choice = ctx.options.get_enum(key_sample_format, "auto", wav_sample_formats());
  CVT_TRY_ASSIGN(WaveEncoded encoded, encode_wave_payload(audio, ctx, choice));
  CVT_TRY(ctx.limits.check_output(encoded.data.size() + 256));
  util::ByteWriter out(encoded.data.size() + 256);
  out.bytes(std::span<const std::uint8_t>(w64_riff));
  out.u64le(0);  // patched below
  out.text("wave");
  out.bytes(std::span<const std::uint8_t>(w64_tail));
  w64_chunk(out, "fmt ", wave_format_body(encoded.format));
  if (encoded.needs_fact) {
    util::ByteWriter fact;
    fact.u64le(encoded.frames);
    w64_chunk(out, "fact", fact.view());
  }
  if (!options::resolved_strip_metadata(ctx.options)) {
    const Bytes info = tags::riff_info_chunk(audio.tags);
    if (!info.empty()) w64_chunk(out, "list", ByteView(info).subspan(8));
  }
  w64_chunk(out, "data", encoded.data);
  out.patch(16, static_cast<std::uint64_t>(out.size()), std::endian::little);
  return out.take();
}

}  // namespace

void register_wav(Registry& registry) {
  FormatInfo wav;
  wav.id = "wav";
  wav.name = "WAVE audio";
  wav.description = "Uncompressed (or ADPCM) audio in a RIFF container; also RF64/BW64 for very long recordings.";
  wav.category = Category::Audio;
  wav.extensions = {"wav", "wave", "bwf"};
  wav.mime = "audio/wav";
  wav.popularity = 85;
  wav.signatures = {Signature::at(0, "'RIFF' ?? ?? ?? ?? 'WAVE'"), Signature::at(0, "'RF64' ?? ?? ?? ?? 'WAVE'"),
                    Signature::at(0, "'BW64' ?? ?? ?? ?? 'WAVE'")};
  registry.add_format(std::move(wav));

  FormatInfo w64;
  w64.id = "w64";
  w64.name = "Sony Wave64 audio";
  w64.description = "64-bit variant of WAVE for recordings larger than 4 GB.";
  w64.category = Category::Audio;
  w64.extensions = {"w64"};
  w64.mime = "audio/x-w64";
  w64.popularity = 12;
  w64.signatures = {Signature::at(0, "'riff' 2E 91 CF 11 A5 D6 28 DB 04 C1 00 00 ?? ?? ?? ?? ?? ?? ?? ?? 'wave' F3 AC D3 11")};
  registry.add_format(std::move(w64));

  std::vector<OptionSpec> options = standard_write_options();
  options.insert(options.begin(), sample_format_option(wav_sample_formats()));
  options.push_back(dither_option());
  options.push_back(OptionSpec::choice("wav_container", "Container", "auto", {"auto", "riff", "rf64", "bw64"},
                                       "RIFF for ordinary files; RF64/BW64 allow more than 4 GB (auto picks "
                                       "RF64 only when needed).")
                        .advanced());

  Codec codec;
  codec.format_id = "wav";
  codec.kind = Kind::Audio;
  codec.read = read_wav;
  codec.write = write_wav;
  codec.write_options = options;
  registry.add_codec(std::move(codec));

  Codec w64_codec;
  w64_codec.format_id = "w64";
  w64_codec.kind = Kind::Audio;
  w64_codec.read = read_w64;
  w64_codec.write = write_w64;
  options.pop_back();
  w64_codec.write_options = options;
  registry.add_codec(std::move(w64_codec));
}

}  // namespace cvt::audio
