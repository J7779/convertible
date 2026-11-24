// Convertible core - audio_lossless family: WavPack files (.wv).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/tags.hpp"
#include "families/audio_lossless/wavpack.hpp"

namespace cvt::audio {

namespace {

Result<AnyIR> read_wv(ReadContext& ctx) {
  tags::TagMap tag_map;
  const std::size_t tail = tags::parse_apev2(ctx.input, tag_map);
  const ByteView input = ctx.input.first(ctx.input.size() - tail);
  // Skip anything before the first block (e.g. an ID3v2 tag).
  const std::string_view text = as_chars(input);
  const std::size_t first = text.find("wvpk");
  if (first == std::string_view::npos) return fail(Status::Decode, "not a WavPack file");
  CVT_TRY_ASSIGN(const wavpack::BlockHeader header, wavpack::parse_header(input.subspan(first)));
  if (header.total_known && header.total_samples > 0) CVT_TRY(check_sample_budget(ctx, header.total_samples));
  wavpack::StreamDecoder decoder;
  std::vector<float> samples;
  std::size_t pos = first;
  std::size_t frames = 0;
  bool damaged = false;
  while (pos + 32 <= input.size()) {
    if ((++frames & 15) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / input.size(), "Decoding"));
    auto used = decoder.decode_frame(input.subspan(pos), samples);
    if (!used) {
      if (samples.empty()) return std::unexpected(used.error());
      damaged = true;
      // Resynchronise on the next block that starts a frame.
      std::size_t next = text.find("wvpk", pos + 1);
      if (next == std::string_view::npos) break;
      pos = next;
      continue;
    }
    CVT_TRY(check_sample_budget(ctx, samples.size()));
    pos += *used;
  }
  if (decoder.channels() == 0) return fail(Status::Decode, "the WavPack file holds no audio");
  if (damaged) ctx.warn("Damaged WavPack blocks were skipped.");
  if (decoder.crc_errors() > 0) ctx.warn("Some WavPack blocks failed their checksum; the audio may be damaged.");
  if (decoder.lossy()) ctx.warn("This is a lossy (hybrid) WavPack file; it was decoded without its correction file.");
  ir::Audio audio = make_audio(decoder.sample_rate(), decoder.channels(), decoder.bits(), decoder.float_samples(),
                               decoder.channel_mask());
  if (decoder.lossy()) audio.bits_per_sample = 0;
  CVT_TRY(check_stream_params(audio.sample_rate, audio.channels, "the WavPack file"));
  audio.samples = std::move(samples);
  if (header.total_known && header.total_samples != 0) set_frame_count(audio, header.total_samples);
  audio.tags = std::move(tag_map);
  return audio;
}

Result<Bytes> write_wv(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx));
  const std::string choice = ctx.options.get_enum(key_sample_format, "auto", {"auto", "s8", "s16", "s24"});
  int bits;
  if (choice == "auto") {
    static constexpr std::array<int, 3> allowed = {8, 16, 24};
    bits = default_bit_depth(audio, allowed, audio.float_samples ? 24 : 16);
    if (audio.float_samples || audio.bits_per_sample > 24) {
      ctx.warn("The WavPack encoder stores up to 24-bit integers; the audio was stored as 24-bit.");
    }
  } else {
    bits = choice == "s8" ? 8 : choice == "s16" ? 16 : 24;
  }
  const std::vector<std::int32_t> ints = quantize_for(audio, bits, ctx.options);
  wavpack::BlockEncoder encoder;
  CVT_TRY(encoder.configure(audio.sample_rate, audio.channels, bits, options::resolved_compression_level(ctx.options, 9),
                            dsp::effective_channel_mask(audio.channels, audio.channel_mask)));
  const std::uint64_t total = audio.frame_count();
  encoder.set_total_samples(total);
  util::ByteWriter out;
  const std::uint32_t block = encoder.block_frames();
  std::size_t index = 0;
  for (std::uint64_t first = 0; first < total; first += block, ++index) {
    if ((index & 3) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(first) / static_cast<double>(total), "Encoding"));
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(block, total - first));
    CVT_TRY_ASSIGN(const Bytes bytes, encoder.encode_block(std::span<const std::int32_t>(ints).subspan(
                                                               static_cast<std::size_t>(first * audio.channels),
                                                               std::size_t{count} * audio.channels),
                                                           count));
    out.bytes(bytes);
    CVT_TRY(ctx.limits.check_output(out.size()));
  }
  if (!options::resolved_strip_metadata(ctx.options)) out.bytes(tags::apev2_tag(audio.tags));
  return out.take();
}

}  // namespace

void register_wavpack(Registry& registry) {
  FormatInfo info;
  info.id = "wv";
  info.name = "WavPack audio";
  info.description = "WavPack lossless (or hybrid) compressed audio.";
  info.category = Category::Audio;
  info.extensions = {"wv"};
  info.mime = "audio/x-wavpack";
  info.popularity = 25;
  info.signatures = {Signature::at(0, "'wvpk'")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "wv";
  codec.kind = Kind::Audio;
  codec.read = read_wv;
  codec.write = write_wv;
  codec.write_options = standard_write_options();
  codec.write_options.insert(codec.write_options.begin(), options::compression_level(9));
  codec.write_options.insert(codec.write_options.begin(), options::preset());
  codec.write_options.insert(codec.write_options.begin(), sample_format_option({"auto", "s8", "s16", "s24"}));
  codec.write_options.push_back(dither_option());
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::audio
