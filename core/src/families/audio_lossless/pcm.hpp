// Convertible core - audio_lossless family: PCM sample formats, G.711 and
// ADPCM sample codecs shared by the container readers and writers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "families/audio_lossless/audio_dsp.hpp"
#include "util/bytes.hpp"

namespace cvt::audio {

enum class PcmEncoding { Unsigned, Signed, Float, Alaw, Mulaw };

/// How samples are laid out in a PCM byte stream.
struct PcmFormat {
  PcmEncoding encoding = PcmEncoding::Signed;
  /// Significant bits (e.g. 24 in a 32-bit container, 12 in 16).
  std::uint16_t bits = 16;
  /// Bytes per sample in the stream (1..8).
  std::uint16_t container_bytes = 2;
  std::endian order = std::endian::little;

  [[nodiscard]] bool valid() const noexcept;
  /// "s16le", "u8", "f32be", "alaw", ...
  [[nodiscard]] std::string name() const;
  friend bool operator==(const PcmFormat&, const PcmFormat&) = default;
};

/// Convenience constructors.
[[nodiscard]] PcmFormat pcm_signed(int bits, std::endian order = std::endian::little);
[[nodiscard]] PcmFormat pcm_unsigned8();
[[nodiscard]] PcmFormat pcm_float(int bits, std::endian order = std::endian::little);
[[nodiscard]] PcmFormat pcm_alaw();
[[nodiscard]] PcmFormat pcm_mulaw();

/// The user-facing sample format names accepted by the `sample_format`
/// option of the PCM writers: "u8", "s8", "s16", "s24", "s32", "f32", "f64",
/// "alaw", "mulaw" (container byte order is the writer's choice).
[[nodiscard]] const std::vector<std::string>& sample_format_choices();
/// Parse one of `sample_format_choices()` (also "auto" -> nullopt result via
/// `ok == false`).
struct ParsedSampleFormat {
  bool ok = false;
  PcmEncoding encoding = PcmEncoding::Signed;
  std::uint16_t bits = 16;
};
[[nodiscard]] ParsedSampleFormat parse_sample_format(std::string_view name);

/// Decode `data` (whole samples only; a trailing partial sample is ignored)
/// and append floats in -1..+1 to `out`.
void decode_pcm(ByteView data, const PcmFormat& format, std::vector<float>& out);
/// Encode interleaved floats. Integer targets are quantised with `dither`
/// unless the samples are exactly representable (see dsp::to_integers).
[[nodiscard]] Bytes encode_pcm(std::span<const float> samples, std::uint16_t channels, const PcmFormat& format,
                               dsp::Dither dither = dsp::Dither::Triangular, std::uint32_t sample_rate = 48000);
/// Append already-quantised integer samples of `format.bits` bits.
void write_pcm_integers(std::span<const std::int32_t> samples, const PcmFormat& format, util::ByteWriter& out);

// --------------------------------------------------------------------------- G.711

[[nodiscard]] std::int16_t alaw_to_linear(std::uint8_t code) noexcept;
[[nodiscard]] std::int16_t mulaw_to_linear(std::uint8_t code) noexcept;
[[nodiscard]] std::uint8_t linear_to_alaw(std::int16_t sample) noexcept;
[[nodiscard]] std::uint8_t linear_to_mulaw(std::int16_t sample) noexcept;

// --------------------------------------------------------------------------- IMA ADPCM

/// Sample frames in one WAV IMA ADPCM block.
[[nodiscard]] std::size_t ima_wav_frames_per_block(std::size_t block_align, std::uint16_t channels) noexcept;
/// Decode WAV/DVI IMA ADPCM (format 0x11) blocks; a final short block is
/// decoded as far as it goes. Appends interleaved float samples.
[[nodiscard]] Result<void> decode_ima_wav(ByteView data, std::uint16_t channels, std::size_t block_align,
                                          std::vector<float>& out);
/// Encode 16-bit samples as WAV IMA ADPCM blocks of `block_align` bytes
/// (the last block is padded with silence). Returns the data.
/// `step_indices` (optional, one per channel) carries the adaptation state
/// between calls so that a stream encoded block by block keeps its quality.
[[nodiscard]] Bytes encode_ima_wav(std::span<const std::int16_t> samples, std::uint16_t channels,
                                   std::size_t block_align, std::vector<int>* step_indices = nullptr);
/// Apple IMA4 ("ima4", AIFF-C/CAF): 34-byte packets of 64 frames per channel.
[[nodiscard]] Result<void> decode_ima4(ByteView data, std::uint16_t channels, std::vector<float>& out);
[[nodiscard]] Bytes encode_ima4(std::span<const std::int16_t> samples, std::uint16_t channels);

// --------------------------------------------------------------------------- Microsoft ADPCM

/// The seven standard predictor coefficient pairs.
struct MsAdpcmCoefficients {
  std::vector<std::int16_t> c1;
  std::vector<std::int16_t> c2;
};
[[nodiscard]] MsAdpcmCoefficients ms_adpcm_standard_coefficients();
[[nodiscard]] std::size_t ms_adpcm_frames_per_block(std::size_t block_align, std::uint16_t channels) noexcept;
[[nodiscard]] Result<void> decode_ms_adpcm(ByteView data, std::uint16_t channels, std::size_t block_align,
                                           const MsAdpcmCoefficients& coefficients, std::vector<float>& out);
[[nodiscard]] Bytes encode_ms_adpcm(std::span<const std::int16_t> samples, std::uint16_t channels,
                                    std::size_t block_align);

// --------------------------------------------------------------------------- GSM 06.10

/// Decode Microsoft GSM 6.10 (WAVE format 0x31): 65-byte blocks of two
/// 160-sample frames, mono 8 kHz. Appends float samples.
[[nodiscard]] Result<void> decode_gsm610_wav(ByteView data, std::vector<float>& out);

}  // namespace cvt::audio
