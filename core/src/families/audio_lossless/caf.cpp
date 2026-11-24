// Convertible core - audio_lossless family: Core Audio Format (.caf).
//
// Reads linear PCM (integer/float, either byte order), mu-law, A-law, Apple
// IMA4 and ALAC (packet table with priming/remainder frames honoured
// exactly), plus 'chan' channel bitmaps and 'info' strings as tags; other
// codecs (AAC, Opus, ...) are handed to the media codec registry when a
// decoder is registered. Writes the same set except AAC/Opus.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "codec/ids.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/alac.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/mp4_boxes.hpp"
#include "families/audio_lossless/packet_decode.hpp"
#include "families/audio_lossless/pcm.hpp"
#include "families/audio_lossless/tags.hpp"
#include "util/text.hpp"

namespace cvt::audio {

namespace {

struct Desc {
  double rate = 0.0;
  std::string format;
  std::uint32_t flags = 0;
  std::uint32_t bytes_per_packet = 0;
  std::uint32_t frames_per_packet = 0;
  std::uint32_t channels = 0;
  std::uint32_t bits = 0;
};

struct PacketTable {
  std::int64_t packets = 0;
  std::int64_t valid_frames = 0;
  std::int32_t priming = 0;
  std::int32_t remainder = 0;
  ByteView entries;
  bool present = false;
};

constexpr std::uint32_t use_channel_bitmap = 1u << 16;

struct InfoKey {
  std::string_view caf;
  std::string_view key;
};
constexpr std::array<InfoKey, 12> info_keys = {{{"title", "title"},
                                                {"artist", "artist"},
                                                {"album", "album"},
                                                {"year", "date"},
                                                {"recorded date", "date"},
                                                {"track number", "track"},
                                                {"genre", "genre"},
                                                {"comments", "comment"},
                                                {"composer", "composer"},
                                                {"copyright", "copyright"},
                                                {"encoding application", "encoder"},
                                                {"lyricist", "lyricist"}}};

bool read_varint(util::ByteReader& r, std::uint64_t& value) {
  value = 0;
  for (int i = 0; i < 10; ++i) {
    const std::uint8_t b = r.u8();
    if (!r.ok()) return false;
    value = (value << 7) | (b & 0x7F);
    if ((b & 0x80) == 0) return true;
  }
  return false;
}

Result<AnyIR> read_caf(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("caff")) return fail(Status::Decode, "not a CAF file");
  const std::uint16_t version = r.u16be();
  r.skip(2);
  if (version != 1) return fail(Status::Decode, "unsupported CAF version {}", version);
  std::optional<Desc> desc;
  PacketTable pakt;
  ByteView cookie;
  ByteView data;
  bool have_data = false;
  std::uint32_t mask = 0;
  tags::TagMap tag_map;
  std::size_t guard = 0;
  while (r.remaining() >= 12 && ++guard < 100000) {
    const std::string_view type = r.chars(4);
    const std::int64_t size = r.i64be();
    const bool to_end = size == -1;
    if (!to_end && (size < 0 || static_cast<std::uint64_t>(size) > r.remaining())) {
      if (type == "data") {
        ctx.warn("The audio data is shorter than the header says.");
      } else {
        break;
      }
    }
    const std::size_t length =
        to_end || static_cast<std::uint64_t>(size) > r.remaining() ? r.remaining() : static_cast<std::size_t>(size);
    const ByteView body = r.bytes(length);
    util::ByteReader b(body);
    if (type == "desc") {
      Desc d;
      d.rate = b.f64be();
      d.format = std::string(b.chars(4));
      d.flags = b.u32be();
      d.bytes_per_packet = b.u32be();
      d.frames_per_packet = b.u32be();
      d.channels = b.u32be();
      d.bits = b.u32be();
      if (!b.ok()) return fail(Status::Decode, "the CAF description is truncated");
      desc = d;
    } else if (type == "pakt") {
      pakt.packets = b.i64be();
      pakt.valid_frames = b.i64be();
      pakt.priming = b.i32be();
      pakt.remainder = b.i32be();
      if (!b.ok() || pakt.packets < 0 || pakt.valid_frames < 0 || pakt.priming < 0 || pakt.remainder < 0) {
        return fail(Status::Decode, "the CAF packet table is damaged");
      }
      pakt.entries = b.rest();
      pakt.present = true;
    } else if (type == "kuki") {
      cookie = body;
    } else if (type == "data") {
      b.skip(4);  // edit count
      data = b.rest();
      have_data = true;
    } else if (type == "chan") {
      const std::uint32_t tag = b.u32be();
      const std::uint32_t bitmap = b.u32be();
      if (b.ok() && tag == use_channel_bitmap) mask = bitmap;
    } else if (type == "info") {
      const std::uint32_t count = b.u32be();
      for (std::uint32_t i = 0; i < count && b.ok() && !b.at_end(); ++i) {
        const std::string key = util::to_lower(b.cstring());
        const std::string_view value = b.cstring();
        if (!b.ok()) break;
        for (const InfoKey& k : info_keys) {
          if (k.caf == key) tags::put(tag_map, k.key, value);
        }
      }
    }
    if (to_end) break;
  }
  if (!desc) return fail(Status::Decode, "the CAF file has no audio description");
  if (!have_data) return fail(Status::Decode, "the CAF file has no audio data");
  if (!std::isfinite(desc->rate) || desc->rate < 1.0 || desc->rate > 10'000'000.0) {
    return fail(Status::Decode, "the CAF file has an invalid sample rate");
  }
  const auto rate = static_cast<std::uint32_t>(std::llround(desc->rate));
  CVT_TRY(check_stream_params(rate, desc->channels, "the CAF file"));
  const auto channels = static_cast<std::uint16_t>(desc->channels);
  CVT_TRY(ctx.checkpoint(0.0, "Decoding"));
  const std::string& f = desc->format;
  if (f == "lpcm" || f == "ulaw" || f == "alaw") {
    PcmFormat layout;
    if (f == "ulaw") {
      layout = pcm_mulaw();
    } else if (f == "alaw") {
      layout = pcm_alaw();
    } else {
      const bool is_float = (desc->flags & 1u) != 0;
      const std::endian order = (desc->flags & 2u) != 0 ? std::endian::little : std::endian::big;
      if (channels == 0 || desc->bytes_per_packet % channels != 0) return fail(Status::Decode, "inconsistent CAF PCM layout");
      const std::uint32_t container = desc->bytes_per_packet / channels;
      if (is_float) {
        if (container != 4 && container != 8) return fail(Status::Decode, "unsupported CAF float size");
        layout = pcm_float(static_cast<int>(container * 8), order);
      } else {
        if (container < 1 || container > 4 || desc->bits == 0 || desc->bits > container * 8) {
          return fail(Status::Decode, "unsupported CAF integer layout");
        }
        layout = pcm_signed(static_cast<int>(desc->bits), order);
        layout.container_bytes = static_cast<std::uint16_t>(container);
      }
    }
    const bool law = layout.encoding == PcmEncoding::Alaw || layout.encoding == PcmEncoding::Mulaw;
    ir::Audio audio = make_audio(rate, channels, law ? 16 : static_cast<std::uint16_t>(layout.bits),
                                 layout.encoding == PcmEncoding::Float, mask);
    const std::uint64_t frame_bytes = std::uint64_t{layout.container_bytes} * channels;
    const std::uint64_t frames = data.size() / frame_bytes;
    CVT_TRY(check_sample_budget(ctx, frames * channels));
    decode_pcm(data.first(static_cast<std::size_t>(frames * frame_bytes)), layout, audio.samples);
    audio.tags = std::move(tag_map);
    return audio;
  }
  if (f == "ima4") {
    ir::Audio audio = make_audio(rate, channels, 16, false, mask);
    CVT_TRY(check_sample_budget(ctx, data.size() / 34 * 64 + 64));
    CVT_TRY(decode_ima4(data, channels, audio.samples));
    if (pakt.present) {
      if (pakt.priming > 0) {
        const std::size_t skip = std::min<std::size_t>(static_cast<std::size_t>(pakt.priming), audio.frame_count());
        audio.samples.erase(audio.samples.begin(), audio.samples.begin() + static_cast<std::ptrdiff_t>(skip * channels));
      }
      set_frame_count(audio, static_cast<std::uint64_t>(pakt.valid_frames));
    }
    audio.tags = std::move(tag_map);
    return audio;
  }
  // Variable-size packets: ALAC and codecs from the registry.
  PacketTrack track;
  track.sample_rate = rate;
  track.channels = channels;
  track.channel_mask = mask;
  track.extradata = to_bytes(cookie);
  if (f == "alac") {
    track.codec = std::string(codec::id::alac);
  } else if (f == "aac ") {
    track.codec = std::string(codec::id::aac);
  } else if (f == "opus") {
    track.codec = std::string(codec::id::opus);
  } else if (f == "flac") {
    track.codec = std::string(codec::id::flac);
  } else if (f == ".mp3") {
    track.codec = std::string(codec::id::mp3);
  } else {
    return fail(Status::Decode, "unsupported CAF audio format '{}'", f);
  }
  if (desc->bytes_per_packet != 0) {
    for (std::size_t at = 0; at + desc->bytes_per_packet <= data.size(); at += desc->bytes_per_packet) {
      track.packets.push_back(data.subspan(at, desc->bytes_per_packet));
    }
  } else {
    if (!pakt.present) return fail(Status::Decode, "the CAF file has no packet table");
    if (static_cast<std::uint64_t>(pakt.packets) > data.size() + 1) return fail(Status::Decode, "the CAF packet table is damaged");
    util::ByteReader e(pakt.entries);
    std::size_t at = 0;
    for (std::int64_t i = 0; i < pakt.packets; ++i) {
      std::uint64_t size = 0;
      if (!read_varint(e, size)) return fail(Status::Decode, "the CAF packet table is truncated");
      if (desc->frames_per_packet == 0) {
        std::uint64_t ignored = 0;
        if (!read_varint(e, ignored)) return fail(Status::Decode, "the CAF packet table is truncated");
      }
      if (size > data.size() - at) {
        ctx.warn("The audio data is shorter than the packet table says.");
        break;
      }
      track.packets.push_back(data.subspan(at, static_cast<std::size_t>(size)));
      at += static_cast<std::size_t>(size);
    }
  }
  if (pakt.present && track.codec == codec::id::alac) {
    track.priming = static_cast<std::uint64_t>(pakt.priming);
    track.valid_frames = static_cast<std::uint64_t>(pakt.valid_frames);
  }
  CVT_TRY_ASSIGN(ir::Audio audio, decode_track(track, ctx));
  if (mask != 0) audio.channel_mask = mask;
  audio.tags = std::move(tag_map);
  return audio;
}

// =========================================================================== writing

const std::vector<std::string>& caf_formats() {
  static const std::vector<std::string> choices = {"auto", "s8",   "s16",   "s24",  "s32", "f32",
                                                   "f64",  "alaw", "mulaw", "ima4", "alac"};
  return choices;
}

void chunk(util::ByteWriter& out, std::string_view type, ByteView body) {
  out.text(type);
  out.i64be(static_cast<std::int64_t>(body.size()));
  out.bytes(body);
}

void write_varint(util::ByteWriter& out, std::uint64_t v) {
  std::array<std::uint8_t, 10> tmp{};
  std::size_t n = 0;
  do {
    tmp[n++] = static_cast<std::uint8_t>(v & 0x7F);
    v >>= 7;
  } while (v != 0 && n < tmp.size());
  for (std::size_t i = n; i-- > 0;) out.u8(static_cast<std::uint8_t>(tmp[i] | (i != 0 ? 0x80 : 0)));
}

Result<Bytes> write_caf(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx));
  std::string choice = ctx.options.get_enum(key_sample_format, "auto", caf_formats());
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
  util::ByteWriter desc;
  desc.f64be(static_cast<double>(audio.sample_rate));
  Bytes payload;
  Bytes cookie;
  Bytes pakt;
  const std::uint64_t frames = audio.frame_count();
  if (choice == "alac") {
    CVT_TRY_ASSIGN(const AlacStream stream, encode_alac_stream(audio, ctx));
    const std::uint32_t depth = stream.config.bit_depth;
    desc.text("alac");
    desc.u32be(depth == 16 ? 1 : depth == 20 ? 2 : depth == 24 ? 3 : 4);
    desc.u32be(0);
    desc.u32be(stream.config.frame_length);
    desc.u32be(audio.channels);
    desc.u32be(0);
    cookie = alac::serialize_config(stream.config);
    util::ByteWriter table;
    table.i64be(static_cast<std::int64_t>(stream.packets.size()));
    table.i64be(static_cast<std::int64_t>(frames));
    table.i32be(0);
    table.i32be(static_cast<std::int32_t>(stream.packets.size() * std::uint64_t{stream.config.frame_length} - frames));
    util::ByteWriter body;
    for (const Bytes& p : stream.packets) {
      write_varint(table, p.size());
      body.bytes(p);
    }
    pakt = table.take();
    payload = body.take();
  } else if (choice == "ima4") {
    const std::vector<std::int32_t> ints = quantize_for(audio, 16, ctx.options);
    const std::vector<std::int16_t> pcm16(ints.begin(), ints.end());
    payload = encode_ima4(pcm16, audio.channels);
    desc.text("ima4");
    desc.u32be(0);
    desc.u32be(34u * audio.channels);
    desc.u32be(64);
    desc.u32be(audio.channels);
    desc.u32be(0);
    const std::uint64_t packets = (frames + 63) / 64;
    util::ByteWriter table;
    table.i64be(static_cast<std::int64_t>(packets));
    table.i64be(static_cast<std::int64_t>(frames));
    table.i32be(0);
    table.i32be(static_cast<std::int32_t>(packets * 64 - frames));
    pakt = table.take();
  } else {
    const ParsedSampleFormat parsed = parse_sample_format(choice);
    PcmFormat layout;
    std::string id = "lpcm";
    std::uint32_t flags = 0;
    switch (parsed.encoding) {
      case PcmEncoding::Signed: layout = pcm_signed(parsed.bits, std::endian::little); flags = 2; break;
      case PcmEncoding::Float: layout = pcm_float(parsed.bits, std::endian::little); flags = 3; break;
      case PcmEncoding::Alaw: layout = pcm_alaw(); id = "alaw"; break;
      case PcmEncoding::Mulaw: layout = pcm_mulaw(); id = "ulaw"; break;
      case PcmEncoding::Unsigned: return fail(Status::InvalidArgument, "CAF has no unsigned sample format");
    }
    payload = encode_pcm(audio.samples, audio.channels, layout, resolved_dither(ctx.options), audio.sample_rate);
    desc.text(id);
    desc.u32be(flags);
    desc.u32be(std::uint32_t{layout.container_bytes} * audio.channels);
    desc.u32be(1);
    desc.u32be(audio.channels);
    desc.u32be(layout.bits);
  }
  CVT_TRY(ctx.limits.check_output(payload.size() + pakt.size() + 1024));
  util::ByteWriter out(payload.size() + pakt.size() + 1024);
  out.text("caff");
  out.u16be(1);
  out.u16be(0);
  chunk(out, "desc", desc.view());
  const std::uint32_t mask = dsp::effective_channel_mask(audio.channels, audio.channel_mask);
  if (audio.channels > 2 && mask != 0) {
    util::ByteWriter chan;
    chan.u32be(use_channel_bitmap);
    chan.u32be(mask);
    chan.u32be(0);
    chunk(out, "chan", chan.view());
  }
  if (!cookie.empty()) chunk(out, "kuki", cookie);
  if (!options::resolved_strip_metadata(ctx.options) && !audio.tags.empty()) {
    util::ByteWriter info;
    std::uint32_t count = 0;
    util::ByteWriter entries;
    for (const auto& [key, val] : audio.tags) {
      for (const InfoKey& k : info_keys) {
        if (k.key == key && k.caf != "recorded date") {
          entries.text(k.caf);
          entries.u8(0);
          entries.text(val);
          entries.u8(0);
          ++count;
          break;
        }
      }
    }
    if (count > 0) {
      info.u32be(count);
      info.bytes(entries.view());
      chunk(out, "info", info.view());
    }
  }
  if (!pakt.empty()) chunk(out, "pakt", pakt);
  out.text("data");
  out.i64be(static_cast<std::int64_t>(payload.size() + 4));
  out.u32be(0);
  out.bytes(payload);
  return out.take();
}

}  // namespace

void register_caf(Registry& registry) {
  FormatInfo info;
  info.id = "caf";
  info.name = "Core Audio Format";
  info.description = "Apple's flexible audio container: PCM, ALAC, IMA4, A-law/mu-law and more.";
  info.category = Category::Audio;
  info.extensions = {"caf", "caff"};
  info.mime = "audio/x-caf";
  info.popularity = 25;
  info.signatures = {Signature::at(0, "'caff' 00 01")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "caf";
  codec.kind = Kind::Audio;
  codec.read = read_caf;
  codec.write = write_caf;
  codec.write_options = standard_write_options();
  codec.write_options.insert(codec.write_options.begin(), options::compression_level(9));
  codec.write_options.insert(codec.write_options.begin(), sample_format_option(caf_formats()));
  codec.write_options.push_back(dither_option());
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::audio
