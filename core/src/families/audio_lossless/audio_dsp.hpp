// Convertible core - audio_lossless family: the audio DSP toolbox.
//
// A small, dependency-light set of signal-processing building blocks shared
// by every audio and video family (it includes nothing but the standard
// library, ir/audio.hpp and engine/errors.hpp):
//
//   * Resampler          - windowed-sinc polyphase sample-rate conversion,
//                          streaming (any chunk size gives bit-identical
//                          output to one call), four quality levels, exact
//                          output length, zero latency as seen by the caller
//   * channel layouts    - WAVE channel masks, standard up/down-mix matrices
//                          (mono, stereo, 3.0, quad, 5.0, 5.1, 6.1, 7.1)
//   * Quantizer          - float -> integer with TPDF dither and optional
//                          noise shaping, deterministic (seeded) noise
//   * measurement        - sample peak, true peak, RMS, ITU-R BS.1770-4 /
//                          EBU R128 integrated loudness
//   * helpers            - gain, peak/loudness normalisation, trim, fades,
//                          interleave / deinterleave
//
// Everything is deterministic and platform independent (no global state, no
// random devices, no locale). Sample values are float in -1..+1 as in
// ir::Audio, with interleaved channels unless stated otherwise.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "ir/audio.hpp"

namespace cvt::audio::dsp {

// =========================================================================== channel layouts

/// WAVE_FORMAT_EXTENSIBLE speaker bits (also the order of interleaved channels).
namespace speaker {
inline constexpr std::uint32_t front_left = 0x1;
inline constexpr std::uint32_t front_right = 0x2;
inline constexpr std::uint32_t front_center = 0x4;
inline constexpr std::uint32_t lfe = 0x8;
inline constexpr std::uint32_t back_left = 0x10;
inline constexpr std::uint32_t back_right = 0x20;
inline constexpr std::uint32_t front_left_of_center = 0x40;
inline constexpr std::uint32_t front_right_of_center = 0x80;
inline constexpr std::uint32_t back_center = 0x100;
inline constexpr std::uint32_t side_left = 0x200;
inline constexpr std::uint32_t side_right = 0x400;
inline constexpr std::uint32_t top_center = 0x800;
inline constexpr std::uint32_t top_front_left = 0x1000;
inline constexpr std::uint32_t top_front_center = 0x2000;
inline constexpr std::uint32_t top_front_right = 0x4000;
inline constexpr std::uint32_t top_back_left = 0x8000;
inline constexpr std::uint32_t top_back_center = 0x10000;
inline constexpr std::uint32_t top_back_right = 0x20000;
}  // namespace speaker

/// Common layouts.
namespace layout {
inline constexpr std::uint32_t mono = speaker::front_center;
inline constexpr std::uint32_t stereo = speaker::front_left | speaker::front_right;
inline constexpr std::uint32_t surround_3_0 = stereo | speaker::front_center;
inline constexpr std::uint32_t quad = stereo | speaker::back_left | speaker::back_right;
inline constexpr std::uint32_t surround_5_0 = surround_3_0 | speaker::back_left | speaker::back_right;
inline constexpr std::uint32_t surround_5_1 = surround_5_0 | speaker::lfe;
inline constexpr std::uint32_t surround_6_1 =
    surround_3_0 | speaker::lfe | speaker::back_center | speaker::side_left | speaker::side_right;
inline constexpr std::uint32_t surround_7_1 = surround_5_1 | speaker::side_left | speaker::side_right;
}  // namespace layout

/// The conventional mask for a channel count (1 mono, 2 stereo, 3 = 3.0,
/// 4 quad, 5 = 5.0, 6 = 5.1, 7 = 6.1, 8 = 7.1); 0 for other counts.
[[nodiscard]] std::uint32_t default_channel_mask(std::uint16_t channels) noexcept;
/// `mask` when it names exactly `channels` speakers, else the default mask.
[[nodiscard]] std::uint32_t effective_channel_mask(std::uint16_t channels, std::uint32_t mask) noexcept;
/// Number of bits set.
[[nodiscard]] int speaker_count(std::uint32_t mask) noexcept;
/// "mono", "stereo", "5.1", "7.1", ... or "N channels".
[[nodiscard]] std::string layout_name(std::uint16_t channels, std::uint32_t mask);

/// A channel mixing matrix: `out[o] = sum_i coefficient(o, i) * in[i]`.
struct MixMatrix {
  std::uint16_t inputs = 0;
  std::uint16_t outputs = 0;
  /// Row-major, outputs x inputs.
  std::vector<float> coefficients;

  [[nodiscard]] float at(std::size_t out, std::size_t in) const noexcept { return coefficients[out * inputs + in]; }
  [[nodiscard]] bool is_identity() const noexcept;
};

/// The standard mixing matrix between two layouts (masks of 0 mean the
/// default for the count). Down-mixes follow ITU-R BS.775: centre and
/// surrounds enter the fronts at -3 dB, the LFE is dropped, mono is the mean
/// of left and right; rows are scaled down when their gains add up to more
/// than 1 so that a full-scale input cannot clip. Up-mixes never invent
/// content: mono goes to the centre (or to both fronts when there is no
/// centre), stereo to the fronts, other speakers stay silent. Channels the
/// masks do not describe are passed through by index.
[[nodiscard]] MixMatrix mix_matrix(std::uint16_t in_channels, std::uint32_t in_mask, std::uint16_t out_channels,
                                   std::uint32_t out_mask);

/// Apply a matrix to interleaved samples.
[[nodiscard]] std::vector<float> apply_mix(const MixMatrix& matrix, std::span<const float> interleaved);

/// Change the channel count of `audio` in place with mix_matrix().
/// `out_mask` 0 = default layout. Fails for 0 or more than 64 channels.
[[nodiscard]] Result<void> remix(ir::Audio& audio, std::uint16_t out_channels, std::uint32_t out_mask = 0);

/// Interleaved -> one vector per channel, and back.
[[nodiscard]] std::vector<std::vector<float>> deinterleave(std::span<const float> interleaved, std::uint16_t channels);
[[nodiscard]] std::vector<float> interleave(const std::vector<std::vector<float>>& planes);

// =========================================================================== resampling

enum class ResampleQuality {
  Fast,     ///< ~70 dB stopband, passband to 85 % of the lower Nyquist
  Medium,   ///< ~100 dB, 90 %
  High,     ///< ~130 dB, 94 % (default)
  Best,     ///< ~150 dB, 97 %
};

/// Parse "fast" | "medium" | "high" | "best".
[[nodiscard]] std::optional<ResampleQuality> parse_resample_quality(std::string_view name) noexcept;

/// Parameters of one quality level.
struct ResampleSpec {
  double stopband_db = 130.0;   ///< attenuation of the stopband (Kaiser design target)
  double passband_edge = 0.94;  ///< fraction of the lower Nyquist frequency kept flat
};
[[nodiscard]] ResampleSpec resample_spec(ResampleQuality quality) noexcept;

/// Streaming band-limited sample-rate converter.
///
/// Output sample k is the band-limited value of the input at time
/// k * in_rate / out_rate (input samples), so there is no delay: the first
/// output sample lines up with the first input sample. The input is treated
/// as silence before its start and after its end; a stream of N input
/// frames produces exactly ceil(N * out_rate / in_rate) output frames in
/// total. Chunking does not matter: feeding the same input in any pieces
/// yields bit-identical output.
///
///   dsp::Resampler r;
///   CVT_TRY(r.configure(44100, 48000, 2, dsp::ResampleQuality::High));
///   r.process(chunk1, out);  r.process(chunk2, out);  r.flush(out);
///
/// The filter is a Kaiser-windowed sinc with the cutoff placed so that the
/// whole transition band lies below the lower Nyquist frequency (no
/// aliasing above the stopband attenuation). Rational ratios with a small
/// numerator use exact polyphase tables; other ratios interpolate linearly
/// between 1024 phases (error far below the stopband level).
class Resampler {
 public:
  /// Fails with Status::InvalidArgument for zero or absurd rates (> 10 MHz)
  /// or channel counts outside 1..64.
  [[nodiscard]] Result<void> configure(std::uint32_t in_rate, std::uint32_t out_rate, std::uint16_t channels,
                                       ResampleQuality quality = ResampleQuality::High);
  /// Append the output that `interleaved` makes available to `out`.
  void process(std::span<const float> interleaved, std::vector<float>& out);
  /// End of input: append the remaining output. The object can be reused
  /// after `reset()`.
  void flush(std::vector<float>& out);
  /// Forget all input (keeps the configuration).
  void reset();

  /// Half the filter length in input samples (how far ahead the filter looks).
  [[nodiscard]] std::size_t half_length() const noexcept { return half_; }
  [[nodiscard]] bool passthrough() const noexcept { return in_rate_ == out_rate_; }

 private:
  void produce(std::vector<float>& out, bool final);
  [[nodiscard]] float sample_at(std::size_t channel, std::int64_t index) const noexcept;

  std::uint32_t in_rate_ = 0;
  std::uint32_t out_rate_ = 0;
  std::uint16_t channels_ = 0;
  std::uint64_t step_int_ = 0;  // integer part of in/out
  std::uint64_t step_num_ = 0;  // fractional numerator (denominator den_)
  std::uint64_t den_ = 1;       // reduced output rate
  std::size_t half_ = 0;        // taps on each side
  std::size_t phases_ = 0;      // rows in table_ minus one
  bool exact_phases_ = false;
  std::vector<float> table_;    // (phases_ + 1) x (2 * half_)

  // streaming state
  std::vector<std::vector<float>> history_;  // per channel, input samples from absolute index base_
  std::int64_t base_ = 0;                    // absolute index of history_[c][0]
  std::uint64_t input_frames_ = 0;           // total frames received
  std::uint64_t output_frames_ = 0;          // total frames produced
  std::int64_t pos_int_ = 0;                 // position of the next output (input samples)
  std::uint64_t pos_num_ = 0;
};

/// One-shot conversion of interleaved samples.
[[nodiscard]] Result<std::vector<float>> resample(std::span<const float> interleaved, std::uint16_t channels,
                                                  std::uint32_t in_rate, std::uint32_t out_rate,
                                                  ResampleQuality quality = ResampleQuality::High);
/// Resample `audio` in place (no-op when the rate already matches).
[[nodiscard]] Result<void> resample(ir::Audio& audio, std::uint32_t out_rate,
                                    ResampleQuality quality = ResampleQuality::High);

// =========================================================================== quantisation

enum class Dither {
  None,        ///< plain rounding
  Triangular,  ///< TPDF dither of +-1 LSB (default when reducing precision)
  Shaped,      ///< TPDF dither with error-feedback noise shaping (pushes noise above ~15 kHz at 44.1/48 kHz)
};
/// Parse "none" | "tpdf" | "shaped".
[[nodiscard]] std::optional<Dither> parse_dither(std::string_view name) noexcept;

/// True when every sample is exactly an integer step of a `bits`-bit grid
/// (value * 2^(bits-1) is an integer in range), so conversion to that depth
/// is lossless and needs no dither.
[[nodiscard]] bool exactly_representable(std::span<const float> samples, int bits) noexcept;

/// Converts float samples to signed integers of 1..32 bits, range
/// [-2^(bits-1), 2^(bits-1) - 1], clipping out-of-range input. Keeps the
/// dither and noise-shaping state between calls so it can stream.
/// Deterministic: the same seed gives the same noise on every platform.
class Quantizer {
 public:
  Quantizer(int bits, std::uint16_t channels, Dither dither, std::uint32_t sample_rate = 48000,
            std::uint32_t seed = 0x2545F491u);
  void process(std::span<const float> interleaved, std::span<std::int32_t> out);
  [[nodiscard]] std::vector<std::int32_t> process(std::span<const float> interleaved);
  [[nodiscard]] int bits() const noexcept { return bits_; }

 private:
  [[nodiscard]] double next_uniform() noexcept;
  int bits_;
  std::uint16_t channels_;
  Dither dither_;
  std::uint64_t state_;
  double scale_;
  double max_;
  double min_;
  std::vector<double> shaping_;           // feedback coefficients
  std::vector<std::vector<double>> error_;  // per channel error history
  std::size_t channel_ = 0;
};

/// Convenience: quantise with dither only when needed, i.e. `dither` unless
/// the samples are already exactly representable at `bits`.
[[nodiscard]] std::vector<std::int32_t> to_integers(std::span<const float> interleaved, std::uint16_t channels,
                                                    int bits, Dither dither = Dither::Triangular,
                                                    std::uint32_t sample_rate = 48000);
/// Integer sample of `bits` bits -> float (exact for bits <= 24).
[[nodiscard]] constexpr float int_to_float(std::int32_t value, int bits) noexcept {
  return static_cast<float>(static_cast<double>(value) / static_cast<double>(std::uint64_t{1} << (bits - 1)));
}

// =========================================================================== measurement

struct Levels {
  double peak = 0.0;                  ///< largest |sample| over all channels (linear)
  double rms = 0.0;                   ///< RMS over all samples (linear)
  std::vector<double> channel_peak;   ///< per channel
  std::vector<double> channel_rms;    ///< per channel
};
[[nodiscard]] Levels measure_levels(std::span<const float> interleaved, std::uint16_t channels);
/// Inter-sample ("true") peak estimated by 4x oversampling (BS.1770-4 Annex 2).
[[nodiscard]] double true_peak(const ir::Audio& audio);
/// Integrated loudness in LUFS (ITU-R BS.1770-4: K-weighting, 400 ms blocks
/// with 75 % overlap, absolute gate -70 LUFS, relative gate -10 LU, channel
/// weights 1.0 for fronts/centre and 1.41 for surrounds, LFE excluded).
/// -infinity for silence or audio shorter than one block.
[[nodiscard]] double integrated_loudness(const ir::Audio& audio);
/// Linear amplitude <-> dBFS.
[[nodiscard]] double to_db(double linear) noexcept;
[[nodiscard]] double from_db(double db) noexcept;

// =========================================================================== editing helpers

/// Multiply every sample by `gain`.
void apply_gain(std::span<float> samples, float gain) noexcept;
/// Scale so that the sample peak becomes `target_dbfs` (e.g. -1.0). Silent
/// audio is left alone. Returns the gain applied.
double normalize_peak(ir::Audio& audio, double target_dbfs);
/// Scale towards `target_lufs` (e.g. -16), but never above `ceiling_dbfs`
/// true peak. Returns the gain applied (1 for silence).
double normalize_loudness(ir::Audio& audio, double target_lufs, double ceiling_dbfs = -1.0);
/// Remove leading/trailing frames whose every channel is below
/// `threshold_dbfs`. Returns the frames removed at the start.
std::size_t trim_silence(ir::Audio& audio, double threshold_dbfs = -60.0);
/// Keep frames [first, first + count) (clamped).
void trim(ir::Audio& audio, std::size_t first_frame, std::size_t frame_count);

enum class FadeCurve { Linear, EqualPower, Logarithmic };
/// Fade in over the first / out over the last `frames` frames.
void fade_in(ir::Audio& audio, std::size_t frames, FadeCurve curve = FadeCurve::EqualPower);
void fade_out(ir::Audio& audio, std::size_t frames, FadeCurve curve = FadeCurve::EqualPower);

}  // namespace cvt::audio::dsp
