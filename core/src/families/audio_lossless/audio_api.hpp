// Convertible core - audio_lossless family: free functions for other families.
//
// Stable entry points for the lossy-audio and video engineers (and anyone
// else holding an ir::Audio): the option vocabulary every audio writer
// shares, the "prepare samples for a writer" pipeline (rate, channels,
// loudness), PCM helpers, and the waveform/spectrogram renderers. The DSP
// building blocks themselves are in audio_dsp.hpp.
//
//   codec.write_options = audio::standard_write_options();   // + your own
//   ...
//   CVT_TRY_ASSIGN(ir::Audio audio, audio::prepare_for_writing(value.get<ir::Audio>(), ctx));
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <memory>

#include "codec/codec.hpp"
#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "families/audio_lossless/audio_dsp.hpp"
#include "ir/audio.hpp"
#include "ir/image.hpp"

namespace cvt::audio {

// --------------------------------------------------------------------------- options

inline constexpr std::string_view key_sample_format = "sample_format";
inline constexpr std::string_view key_dither = "dither";
inline constexpr std::string_view key_resample_quality = "resample_quality";
inline constexpr std::string_view key_normalize = "normalize";

/// `dither`: "tpdf" (default) | "shaped" | "none". Applied only when the
/// bit depth actually drops. Advanced.
[[nodiscard]] OptionSpec dither_option();
/// `resample_quality`: "fast" | "medium" | "high" (default) | "best". Advanced.
[[nodiscard]] OptionSpec resample_quality_option();
/// `normalize`: "none" (default) | "peak" (-1 dBFS) | "loudness" (-16 LUFS,
/// true peak <= -1 dBTP). Advanced.
[[nodiscard]] OptionSpec normalize_option();
/// `sample_format`: one of `choices` (the first is the default, normally
/// "auto" = keep the source's precision).
[[nodiscard]] OptionSpec sample_format_option(std::vector<std::string> choices);

/// sample_rate, channels, resample_quality, normalize, strip_metadata: the
/// options every audio writer of this family declares. Lossy writers in
/// other families may use it too and add quality()/preset()/bitrate.
[[nodiscard]] std::vector<OptionSpec> standard_write_options();

[[nodiscard]] dsp::Dither resolved_dither(const Options& options);
[[nodiscard]] dsp::ResampleQuality resolved_resample_quality(const Options& options);

// --------------------------------------------------------------------------- pipeline

/// Checks that `audio` is something a writer can encode (valid, at most 255
/// channels, rate below 10 MHz). Status::Encode otherwise.
[[nodiscard]] Result<void> check_writable(const ir::Audio& audio);

/// A copy of `source` converted as the standard options ask: channel
/// re-mix (`channels`), sample-rate conversion (`sample_rate`,
/// `resample_quality`), then `normalize`. Tags are dropped when
/// `strip_metadata` is on (the default). Precision hints are kept (and
/// cleared for float processing that changed the samples).
/// `max_rate`/`min_rate` clamp the target rate for formats with limits
/// (a warning is emitted when clamping happens); 0 = no limit.
[[nodiscard]] Result<ir::Audio> prepare_for_writing(const ir::Audio& source, const Context& ctx,
                                                    std::uint32_t min_rate = 0, std::uint32_t max_rate = 0,
                                                    std::uint16_t max_channels = 0);

/// The bit depth a lossless/PCM writer should use when the user did not
/// choose: the source's integer precision if known and allowed, else the
/// smallest allowed depth >= it, else `fallback`.
[[nodiscard]] int default_bit_depth(const ir::Audio& audio, std::span<const int> allowed, int fallback);

/// Integer samples of `bits` bits for an integer codec, dithered with the
/// `dither` option when (and only when) precision is reduced.
[[nodiscard]] std::vector<std::int32_t> quantize_for(const ir::Audio& audio, int bits, const Options& options);

// --------------------------------------------------------------------------- codecs

/// A lossless encoder for `codec_id` ("flac", "alac", "wavpack") at a chosen
/// integer bit depth. The registry's factories default to 16 bits because
/// AudioEncodeParams carries no precision; muxers that know the source's
/// precision (ir::Audio::bits_per_sample) can call this instead. Returns
/// nullptr for other ids. Samples sent are dithered (TPDF) only when they
/// do not fit the chosen depth exactly.
[[nodiscard]] std::unique_ptr<codec::AudioEncoder> make_lossless_encoder(std::string_view codec_id, int bits);

// --------------------------------------------------------------------------- pictures

struct WaveformStyle {
  std::uint32_t width = 1200;
  std::uint32_t height = 300;
  std::uint32_t foreground = 0x3B82F6;  ///< RGB
  std::uint32_t background = 0xFFFFFF;  ///< RGB
  bool transparent = false;             ///< background alpha 0
  bool split_channels = false;          ///< one lane per channel instead of a mono mix
};
/// Min/max envelope per pixel column with an RMS core, centred line.
[[nodiscard]] Result<ir::Image> render_waveform(const ir::Audio& audio, const WaveformStyle& style);

struct SpectrogramStyle {
  std::uint32_t width = 1200;
  std::uint32_t height = 512;
  std::uint32_t fft_size = 2048;   ///< power of two 64..16384
  double range_db = 120.0;         ///< dynamic range shown below the 0 dBFS top
  bool log_frequency = false;      ///< logarithmic frequency axis
  std::string palette = "magma";   ///< "magma", "gray", "viridis"
};
/// Short-time Fourier magnitude (Hann window), low frequencies at the
/// bottom, time left to right; colour maps level in dBFS.
[[nodiscard]] Result<ir::Image> render_spectrogram(const ir::Audio& audio, const SpectrogramStyle& style,
                                                   const Context* ctx = nullptr);

}  // namespace cvt::audio
