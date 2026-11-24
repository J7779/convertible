// Convertible core - audio_lossy family: Xiph codecs (Opus, Vorbis, Speex)
// in Ogg and in the codec registry.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "codec/codec.hpp"
#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/audio.hpp"
#include "util/bytes.hpp"

namespace cvt::lossy::xiph {

// ------------------------------------------------------------ channel order

/// Position in Vorbis/Opus order of each WAVE-order channel (1..8 channels):
/// wave[i] = vorbis[map[i]].
[[nodiscard]] std::array<int, 8> vorbis_to_wave_map(int channels) noexcept;
/// Reorder interleaved samples from Vorbis order to WAVE order (in place).
void vorbis_to_wave(std::span<float> samples, int channels);
/// And back.
void wave_to_vorbis(std::span<float> samples, int channels);

// ------------------------------------------------------------ Opus

struct OpusHead {
  int channels = 0;
  int pre_skip = 0;
  std::uint32_t input_rate = 0;
  int gain_q8 = 0;  ///< output gain, dB in Q7.8
  int family = 0;
  int streams = 1;
  int coupled = 0;
  std::array<std::uint8_t, 255> mapping{};
};
/// Parse an Ogg "OpusHead" packet or an MP4 'dOps' box body.
[[nodiscard]] std::optional<OpusHead> parse_opus_head(ByteView data);
[[nodiscard]] Bytes make_opus_head(const OpusHead& head);

struct OpusSettings {
  std::uint32_t bitrate_bps = 160000;
  int complexity = 10;
  bool vbr = true;
};
[[nodiscard]] Result<ir::Audio> read_ogg_opus(ByteView input, const Context& ctx);
/// `audio` must be at 48 kHz.
[[nodiscard]] Result<Bytes> write_ogg_opus(const ir::Audio& audio, const OpusSettings& settings, const Context& ctx);
[[nodiscard]] std::unique_ptr<codec::AudioDecoder> make_opus_decoder();
[[nodiscard]] std::unique_ptr<codec::AudioEncoder> make_opus_encoder();

// ------------------------------------------------------------ Vorbis

struct VorbisSettings {
  float quality = 0.7f;                  ///< -0.1 .. 1.0 (oggenc's -q / 10)
  std::optional<std::uint32_t> bitrate;  ///< average bit/s (managed mode) instead of quality
};
[[nodiscard]] Result<ir::Audio> read_ogg_vorbis(ByteView input, const Context& ctx);
[[nodiscard]] Result<Bytes> write_ogg_vorbis(const ir::Audio& audio, const VorbisSettings& settings, const Context& ctx);
[[nodiscard]] std::unique_ptr<codec::AudioDecoder> make_vorbis_decoder();
[[nodiscard]] std::unique_ptr<codec::AudioEncoder> make_vorbis_encoder();

// ------------------------------------------------------------ Speex

struct SpeexSettings {
  int quality = 8;  ///< 0..10
};
[[nodiscard]] Result<ir::Audio> read_ogg_speex(ByteView input, const Context& ctx);
/// `audio` must be mono at 8, 16 or 32 kHz.
[[nodiscard]] Result<Bytes> write_ogg_speex(const ir::Audio& audio, const SpeexSettings& settings, const Context& ctx);
[[nodiscard]] std::unique_ptr<codec::AudioDecoder> make_speex_decoder();

}  // namespace cvt::lossy::xiph
