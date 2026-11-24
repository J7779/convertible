// Convertible core - audio_lossless family: WAVE format chunk handling,
// shared by WAV, Wave64 and the PCM/ADPCM media codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/audio_lossless/pcm.hpp"
#include "ir/audio.hpp"
#include "util/bytes.hpp"

namespace cvt::audio {

namespace wave_tag {
inline constexpr std::uint16_t pcm = 0x0001;
inline constexpr std::uint16_t ms_adpcm = 0x0002;
inline constexpr std::uint16_t ieee_float = 0x0003;
inline constexpr std::uint16_t alaw = 0x0006;
inline constexpr std::uint16_t mulaw = 0x0007;
inline constexpr std::uint16_t ima_adpcm = 0x0011;
inline constexpr std::uint16_t gsm610 = 0x0031;
inline constexpr std::uint16_t extensible = 0xFFFE;
}  // namespace wave_tag

/// A parsed `fmt ` chunk (extensible sub-format already resolved into `tag`).
struct WaveFormat {
  std::uint16_t tag = wave_tag::pcm;
  std::uint16_t channels = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t byte_rate = 0;
  std::uint16_t block_align = 0;
  std::uint16_t bits = 0;
  bool extensible = false;
  std::uint16_t valid_bits = 0;
  std::uint32_t channel_mask = 0;
  std::uint16_t samples_per_block = 0;
  MsAdpcmCoefficients coefficients;
};

[[nodiscard]] Result<WaveFormat> parse_wave_format(ByteView body);
/// The PCM layout of a format, or nullopt for block codecs (ADPCM, GSM).
[[nodiscard]] Result<std::optional<PcmFormat>> wave_pcm_layout(const WaveFormat& format);
/// Decode a data chunk into `audio` (rate, channels, hints and samples).
/// `fact_frames` (0 = unknown) trims block-coded streams.
[[nodiscard]] Result<void> decode_wave_data(const WaveFormat& format, ByteView data, std::uint64_t fact_frames,
                                            const Context& ctx, ir::Audio& audio);

struct WaveEncoded {
  WaveFormat format;
  Bytes data;
  std::uint64_t frames = 0;
  bool needs_fact = false;
};
/// Encode samples for a WAVE-family container with a sample format name
/// ("auto", "u8", "s16", "s24", "s32", "f32", "f64", "alaw", "mulaw",
/// "ima_adpcm", "ms_adpcm").
[[nodiscard]] Result<WaveEncoded> encode_wave_payload(const ir::Audio& audio, const WriteContext& ctx,
                                                      std::string_view sample_format);
/// The body of a `fmt ` chunk.
[[nodiscard]] Bytes wave_format_body(const WaveFormat& format);

}  // namespace cvt::audio
