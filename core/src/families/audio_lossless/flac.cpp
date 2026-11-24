// Convertible core - audio_lossless family: FLAC files (native and Ogg FLAC).
//
// Reading: optional leading ID3v2 tag, metadata blocks (STREAMINFO,
// SEEKTABLE, VORBIS_COMMENT -> tags, PICTURE/APPLICATION/CUESHEET/PADDING
// skipped), then frames; damaged frames are skipped with a warning by
// searching for the next valid frame header; the MD5 signature is checked.
// Ogg FLAC (".oga") is unwrapped page by page (CRC-checked) first.
// Writing: in-house encoder (levels 0-8 like the reference encoder), MD5,
// a seek table every 10 seconds, Vorbis comments only when metadata is kept.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/flac_codec.hpp"
#include "families/audio_lossless/flac_file.hpp"
#include "families/audio_lossless/ogg.hpp"
#include "families/audio_lossless/tags.hpp"
#include "util/text.hpp"

namespace cvt::audio {

namespace {

std::size_t skip_id3(ByteView data) {
  util::ByteReader r(data);
  if (!r.consume("ID3") || r.remaining() < 7) return 0;
  r.skip(2);
  const std::uint8_t flags = r.u8();
  const std::uint32_t raw = r.u32be();
  const std::uint32_t size = ((raw >> 24) & 0x7F) << 21 | ((raw >> 16) & 0x7F) << 14 | ((raw >> 8) & 0x7F) << 7 | (raw & 0x7F);
  const std::size_t total = 10 + std::size_t{size} + ((flags & 0x10) != 0 ? 10 : 0);
  return total <= data.size() ? total : 0;
}

}  // namespace

// =========================================================================== shared decode

Result<void> FlacStream::add_metadata_block(std::uint8_t type, ByteView body, tags::TagMap& tag_map) {
  if (type == 0) {
    CVT_TRY_ASSIGN(info, flac::parse_streaminfo(body));
    have_info = true;
  } else if (type == 4) {
    tags::TagMap found;
    tags::parse_vorbis_comment(body, found);
    if (const auto it = found.find("waveformatextensible_channel_mask"); it != found.end()) {
      std::string_view text = it->second;
      if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
      channel_mask = static_cast<std::uint32_t>(util::parse_int(text, 16).value_or(0));
      found.erase(it);
    }
    for (auto& [k, v] : found) tag_map.emplace(k, v);
  } else if (type == 127) {
    return fail(Status::Decode, "invalid FLAC metadata block");
  }
  return {};
}

Result<ir::Audio> FlacStream::finish(const std::vector<std::int32_t>& samples, const Context& ctx, bool truncated) {
  ir::Audio audio = make_audio(info.sample_rate, static_cast<std::uint16_t>(info.channels),
                               static_cast<std::uint16_t>(info.bits), false, channel_mask);
  std::size_t count = samples.size();
  if (info.total_samples != 0) {
    const std::uint64_t wanted = info.total_samples * info.channels;
    if (wanted < count) count = static_cast<std::size_t>(wanted);
    if (wanted > samples.size() && !truncated) ctx.warn("The FLAC stream is shorter than its header says; the file may be truncated.");
  }
  bool all_zero = true;
  for (const std::uint8_t b : info.md5) all_zero = all_zero && b == 0;
  if (!all_zero && !truncated && (info.total_samples == 0 || info.total_samples * info.channels == count)) {
    flac::SampleMd5 md5(info.bits);
    md5.update(std::span<const std::int32_t>(samples).first(count));
    if (md5.finish() != info.md5) ctx.warn("The FLAC MD5 signature does not match the decoded audio.");
    else md5_verified = true;
  }
  audio.samples.resize(count);
  const double scale = 1.0 / static_cast<double>(std::uint64_t{1} << (info.bits - 1));
  for (std::size_t i = 0; i < count; ++i) audio.samples[i] = static_cast<float>(samples[i] * scale);
  return audio;
}

namespace {

// Decode frames from a contiguous byte range (native FLAC).
Result<bool> decode_frames(ByteView data, FlacStream& stream, const Context& ctx, std::vector<std::int32_t>& samples) {
  std::size_t pos = 0;
  std::size_t errors = 0;
  bool damaged = false;
  const flac::StreamInfo* info = stream.have_info ? &stream.info : nullptr;
  if (stream.have_info && stream.info.total_samples != 0) {
    CVT_TRY(check_sample_budget(ctx, stream.info.total_samples * stream.info.channels));
    samples.reserve(static_cast<std::size_t>(stream.info.total_samples * stream.info.channels));
  }
  std::size_t frame_index = 0;
  while (pos + 2 <= data.size()) {
    if ((++frame_index & 15) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / data.size(), "Decoding"));
    flac::FrameInfo frame;
    const std::size_t before = samples.size();
    auto used = flac::decode_frame(data.subspan(pos), info, samples, frame);
    if (used) {
      if (!stream.have_info) {
        // A stream without STREAMINFO (e.g. raw frames): take the first frame's format.
        stream.info.sample_rate = frame.sample_rate;
        stream.info.channels = frame.channels;
        stream.info.bits = frame.bits;
        stream.have_info = true;
        info = &stream.info;
      }
      if (frame.channels != stream.info.channels || frame.bits != stream.info.bits) {
        samples.resize(before);
        return fail(Status::Decode, "the FLAC stream changes format midway");
      }
      CVT_TRY(check_sample_budget(ctx, samples.size()));
      pos += *used;
      continue;
    }
    samples.resize(before);
    if (used.error().code == Status::Limit) return std::unexpected(used.error());
    // Trailing garbage (ID3v1, padding) after the last frame is harmless.
    if (stream.info.total_samples != 0 && samples.size() >= stream.info.total_samples * stream.info.channels) break;
    damaged = true;
    if (++errors > 10000) break;
    std::size_t next = pos + 1;
    while (next + 2 <= data.size() && !flac::frame_header_at(data.subspan(next), info)) ++next;
    pos = next;
  }
  if (damaged) ctx.warn("Damaged FLAC frames were skipped.");
  return damaged;
}

Result<AnyIR> read_ogg_flac(ReadContext& ctx) {
  CVT_TRY_ASSIGN(OggStream ogg, read_ogg_stream(ctx.input, "\x7F" "FLAC", ctx));
  if (ogg.packets.empty()) return fail(Status::Decode, "the Ogg file holds no FLAC stream");
  const ByteView first = ogg.packets.front();
  if (first.size() < 13 + 38 || as_chars(first.subspan(9, 4)) != "fLaC") {
    return fail(Status::Decode, "invalid Ogg FLAC header");
  }
  FlacStream stream;
  tags::TagMap tag_map;
  CVT_TRY(stream.add_metadata_block(0, first.subspan(17, 34), tag_map));
  std::size_t index = 1;
  for (; index < ogg.packets.size(); ++index) {
    const ByteView packet = ogg.packets[index];
    if (packet.empty()) continue;
    if (std::to_integer<std::uint8_t>(packet[0]) == 0xFF) break;  // first audio frame
    if (packet.size() < 4) return fail(Status::Decode, "invalid Ogg FLAC metadata packet");
    const auto type = static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(packet[0]) & 0x7F);
    CVT_TRY(stream.add_metadata_block(type, packet.subspan(4), tag_map));
  }
  std::vector<std::int32_t> samples;
  if (stream.info.total_samples != 0) CVT_TRY(check_sample_budget(ctx, stream.info.total_samples * stream.info.channels));
  bool damaged = ogg.damaged;
  for (; index < ogg.packets.size(); ++index) {
    if ((index & 31) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / ogg.packets.size(), "Decoding"));
    flac::FrameInfo frame;
    const std::size_t before = samples.size();
    auto used = flac::decode_frame(ogg.packets[index], &stream.info, samples, frame);
    if (!used) {
      samples.resize(before);
      damaged = true;
      continue;
    }
    CVT_TRY(check_sample_budget(ctx, samples.size()));
  }
  if (damaged) ctx.warn("Damaged parts of the Ogg FLAC stream were skipped.");
  CVT_TRY_ASSIGN(ir::Audio audio, stream.finish(samples, ctx, damaged));
  audio.tags = std::move(tag_map);
  return audio;
}

}  // namespace

Result<ir::Audio> decode_flac_file(ByteView input, const Context& ctx) {
  std::size_t pos = skip_id3(input);
  if (input.size() < pos + 4 || as_chars(input.subspan(pos, 4)) != "fLaC") return fail(Status::Decode, "not a FLAC file");
  pos += 4;
  FlacStream stream;
  tags::TagMap tag_map;
  bool last = false;
  std::size_t guard = 0;
  while (!last && ++guard < 100000) {
    if (pos + 4 > input.size()) return fail(Status::Decode, "the FLAC metadata is truncated");
    util::ByteReader h(input.subspan(pos, 4));
    const std::uint8_t head = h.u8();
    const std::uint32_t length = h.u24be();
    last = (head & 0x80) != 0;
    pos += 4;
    if (length > input.size() - pos) return fail(Status::Decode, "the FLAC metadata is truncated");
    CVT_TRY(stream.add_metadata_block(static_cast<std::uint8_t>(head & 0x7F), input.subspan(pos, length), tag_map));
    pos += length;
  }
  if (!stream.have_info) return fail(Status::Decode, "the FLAC file has no STREAMINFO block");
  CVT_TRY(check_stream_params(stream.info.sample_rate, stream.info.channels, "the FLAC file"));
  std::vector<std::int32_t> samples;
  CVT_TRY_ASSIGN(const bool damaged, decode_frames(input.subspan(pos), stream, ctx, samples));
  CVT_TRY_ASSIGN(ir::Audio audio, stream.finish(samples, ctx, damaged));
  audio.tags = std::move(tag_map);
  return audio;
}

namespace {

Result<AnyIR> read_flac(ReadContext& ctx) {
  if (ctx.input.size() >= 4 && as_chars(ctx.input.first(4)) == "OggS") return read_ogg_flac(ctx);
  CVT_TRY_ASSIGN(ir::Audio audio, decode_flac_file(ctx.input, ctx));
  return audio;
}

// =========================================================================== writing

void metadata_block(util::ByteWriter& out, std::uint8_t type, ByteView body, bool last) {
  out.u8(static_cast<std::uint8_t>(type | (last ? 0x80 : 0)));
  out.u24be(static_cast<std::uint32_t>(body.size()));
  out.bytes(body);
}

}  // namespace

Result<Bytes> encode_flac_file(const ir::Audio& audio, int bits, int level, bool keep_tags, const Context& ctx) {
  const std::vector<std::int32_t> ints = quantize_for(audio, bits, ctx.options);
  const flac::Settings settings = flac::settings_for_level(level);
  flac::FrameEncoder encoder;
  CVT_TRY(encoder.configure(audio.sample_rate, audio.channels, static_cast<std::uint32_t>(bits), settings));
  const std::uint64_t frames = audio.frame_count();
  const std::uint32_t block = settings.block_size;
  flac::SampleMd5 md5(static_cast<std::uint32_t>(bits));
  md5.update(ints);
  util::ByteWriter body;
  std::uint32_t min_frame = 0xFFFFFF, max_frame = 0;
  struct SeekPoint {
    std::uint64_t sample, offset;
    std::uint32_t count;
  };
  std::vector<SeekPoint> seek;
  const std::uint64_t seek_interval = std::uint64_t{audio.sample_rate} * 10;
  std::uint64_t next_seek = 0;
  std::uint64_t index = 0;
  for (std::uint64_t first = 0; first < frames; first += block, ++index) {
    if ((index & 15) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(first) / static_cast<double>(frames), "Encoding"));
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(block, frames - first));
    if (frames > seek_interval && first >= next_seek) {
      seek.push_back({first, body.size(), count});
      next_seek += seek_interval;
    }
    const Bytes frame = encoder.encode(std::span<const std::int32_t>(ints).subspan(
                                           static_cast<std::size_t>(first * audio.channels), std::size_t{count} * audio.channels),
                                       count, index);
    min_frame = std::min<std::uint32_t>(min_frame, static_cast<std::uint32_t>(frame.size()));
    max_frame = std::max<std::uint32_t>(max_frame, static_cast<std::uint32_t>(frame.size()));
    body.bytes(frame);
    CVT_TRY(ctx.limits.check_output(body.size()));
  }
  flac::StreamInfo info;
  info.min_block = block;  // as the reference encoder: the last block may be shorter
  info.max_block = block;
  info.min_frame = frames == 0 ? 0 : min_frame;
  info.max_frame = max_frame;
  info.sample_rate = audio.sample_rate;
  info.channels = audio.channels;
  info.bits = static_cast<std::uint32_t>(bits);
  info.total_samples = frames;
  info.md5 = md5.finish();

  tags::TagMap comments;
  if (keep_tags) comments = audio.tags;
  const std::uint32_t mask = dsp::effective_channel_mask(audio.channels, audio.channel_mask);
  if (mask != 0 && mask != dsp::default_channel_mask(audio.channels)) {
    comments["waveformatextensible_channel_mask"] = std::format("0x{:X}", mask);
  }
  util::ByteWriter out(body.size() + 1024);
  out.text("fLaC");
  const bool has_seek = !seek.empty();
  const bool has_comments = !comments.empty();
  metadata_block(out, 0, flac::serialize_streaminfo(info), !has_seek && !has_comments);
  if (has_seek) {
    util::ByteWriter table;
    for (const SeekPoint& p : seek) {
      table.u64be(p.sample);
      table.u64be(p.offset);
      table.u16be(static_cast<std::uint16_t>(p.count));
    }
    metadata_block(out, 3, table.view(), !has_comments);
  }
  if (has_comments) {
    tags::TagMap renamed;
    for (const auto& [k, v] : comments) renamed[k] = v;
    Bytes vc = tags::vorbis_comment(renamed);
    metadata_block(out, 4, vc, true);
  }
  out.bytes(body.view());
  return out.take();
}

namespace {

Result<Bytes> write_flac(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx, 1, 1048575, 8));
  const std::string choice = ctx.options.get_enum(key_sample_format, "auto", {"auto", "s8", "s12", "s16", "s20", "s24", "s32"});
  int bits;
  if (choice == "auto") {
    static constexpr std::array<int, 6> allowed = {8, 12, 16, 20, 24, 32};
    bits = default_bit_depth(audio, allowed, audio.float_samples ? 24 : 16);
    if (audio.float_samples) ctx.warn("FLAC stores integer samples; the floating-point audio was stored as 24-bit.");
  } else {
    bits = static_cast<int>(util::parse_int(std::string_view(choice).substr(1)).value_or(16));
  }
  const int level = options::resolved_compression_level(ctx.options, 8);
  return encode_flac_file(audio, bits, level, !options::resolved_strip_metadata(ctx.options), ctx);
}

int sniff_ogg_flac(ByteView data, std::string_view) {
  return data.size() >= 37 && as_chars(data.first(4)) == "OggS" && as_chars(data.subspan(28, 5)) == "\x7F" "FLAC" ? 97 : 0;
}

}  // namespace

void register_flac(Registry& registry) {
  FormatInfo info;
  info.id = "flac";
  info.name = "FLAC audio";
  info.description = "Free Lossless Audio Codec: bit-exact compression to about half the size of WAV.";
  info.category = Category::Audio;
  info.extensions = {"flac"};
  info.mime = "audio/flac";
  info.popularity = 80;
  info.signatures = {Signature::at(0, "'fLaC'")};
  registry.add_format(std::move(info));

  FormatInfo ogg;
  ogg.id = "ogg-flac";
  ogg.name = "Ogg FLAC audio";
  ogg.description = "FLAC audio in an Ogg container.";
  ogg.category = Category::Audio;
  ogg.extensions = {"oga"};
  ogg.mime = "audio/ogg";
  ogg.popularity = 10;
  ogg.sniff = sniff_ogg_flac;
  registry.add_format(std::move(ogg));

  std::vector<OptionSpec> options = standard_write_options();
  options.insert(options.begin(), options::compression_level(8));
  options.insert(options.begin(), options::preset());
  options.insert(options.begin(), sample_format_option({"auto", "s8", "s12", "s16", "s20", "s24", "s32"}));
  options.push_back(dither_option());

  Codec codec;
  codec.format_id = "flac";
  codec.kind = Kind::Audio;
  codec.read = read_flac;
  codec.write = write_flac;
  codec.write_options = options;
  registry.add_codec(std::move(codec));

  Codec ogg_codec;
  ogg_codec.format_id = "ogg-flac";
  ogg_codec.kind = Kind::Audio;
  ogg_codec.read = read_ogg_flac;
  registry.add_codec(std::move(ogg_codec));
}

}  // namespace cvt::audio
