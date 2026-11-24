// Convertible core - audio_lossless family: packet list -> ir::Audio.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossless/packet_decode.hpp"

#include <algorithm>
#include <cstddef>

#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "families/audio_lossless/alac.hpp"
#include "families/audio_lossless/common.hpp"

namespace cvt::audio {

namespace {

void apply_trim(ir::Audio& audio, std::uint64_t priming, std::uint64_t valid) {
  const std::size_t frames = audio.frame_count();
  const auto skip = static_cast<std::size_t>(std::min<std::uint64_t>(priming, frames));
  if (skip > 0) audio.samples.erase(audio.samples.begin(), audio.samples.begin() + static_cast<std::ptrdiff_t>(skip * audio.channels));
  if (valid != 0) set_frame_count(audio, valid);
}

Result<ir::Audio> decode_alac(const PacketTrack& track, const Context& ctx) {
  CVT_TRY_ASSIGN(alac::Config config, alac::parse_config(track.extradata));
  alac::Decoder decoder;
  CVT_TRY(decoder.configure(config));
  const std::uint32_t rate = config.sample_rate != 0 ? config.sample_rate : track.sample_rate;
  CVT_TRY(check_stream_params(rate, config.channels, "the ALAC stream"));
  ir::Audio audio = make_audio(rate, config.channels, config.bit_depth, false, alac::channel_mask(config.channels));
  CVT_TRY(check_sample_budget(ctx, std::uint64_t{track.packets.size()} * config.frame_length * config.channels));
  std::vector<std::int32_t> ints;
  ints.reserve(track.packets.size() * config.frame_length * config.channels);
  for (std::size_t i = 0; i < track.packets.size(); ++i) {
    if ((i & 31) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(track.packets.size()), "Decoding"));
    CVT_TRY(decoder.decode(track.packets[i], ints));
  }
  const double scale = 1.0 / static_cast<double>(std::uint64_t{1} << (config.bit_depth - 1));
  audio.samples.resize(ints.size());
  for (std::size_t i = 0; i < ints.size(); ++i) audio.samples[i] = static_cast<float>(ints[i] * scale);
  apply_trim(audio, track.priming, track.valid_frames);
  return audio;
}

}  // namespace

Result<ir::Audio> decode_track(const PacketTrack& track, const Context& ctx) {
  if (track.codec == codec::id::alac) return decode_alac(track, ctx);
  if (ctx.registry == nullptr) return fail(Status::Decode, "no decoder available for {} audio", track.codec);
  auto decoder = ctx.registry->media().make_audio_decoder(track.codec);
  if (!decoder) return fail(Status::Decode, "no decoder available for {} audio", track.codec);
  codec::CodecParams params;
  params.codec = track.codec;
  params.extradata = track.extradata;
  params.sample_rate = track.sample_rate;
  params.channels = track.channels;
  params.bits_per_sample = track.bits;
  params.timescale = track.sample_rate != 0 ? track.sample_rate : 1000;
  CVT_TRY(decoder->configure(params));
  ir::Audio audio;
  const auto drain = [&]() -> Result<void> {
    while (true) {
      CVT_TRY_ASSIGN(auto chunk, decoder->receive_chunk());
      if (!chunk) return {};
      CVT_TRY(check_sample_budget(ctx, audio.samples.size() + chunk->samples.size()));
      audio.samples.insert(audio.samples.end(), chunk->samples.begin(), chunk->samples.end());
    }
  };
  for (std::size_t i = 0; i < track.packets.size(); ++i) {
    if ((i & 31) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(track.packets.size()), "Decoding"));
    codec::Packet packet;
    packet.bytes = to_bytes(track.packets[i]);
    packet.keyframe = true;
    CVT_TRY(decoder->send_packet(packet));
    CVT_TRY(drain());
  }
  CVT_TRY(decoder->flush());
  CVT_TRY(drain());
  audio.sample_rate = decoder->sample_rate() != 0 ? decoder->sample_rate() : track.sample_rate;
  audio.channels = decoder->channels() != 0 ? decoder->channels() : track.channels;
  audio.channel_mask = track.channel_mask;
  CVT_TRY(check_stream_params(audio.sample_rate, audio.channels, "the audio stream"));
  audio.samples.resize(audio.samples.size() / audio.channels * audio.channels);
  apply_trim(audio, track.priming, track.valid_frames);
  return audio;
}

}  // namespace cvt::audio
