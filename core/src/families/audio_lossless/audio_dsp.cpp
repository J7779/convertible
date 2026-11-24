// Convertible core - audio_lossless family: the audio DSP toolbox.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossless/audio_dsp.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <numeric>
#include <utility>

namespace cvt::audio::dsp {

// =========================================================================== channel layouts

std::uint32_t default_channel_mask(std::uint16_t channels) noexcept {
  switch (channels) {
    case 1: return layout::mono;
    case 2: return layout::stereo;
    case 3: return layout::surround_3_0;
    case 4: return layout::quad;
    case 5: return layout::surround_5_0;
    case 6: return layout::surround_5_1;
    case 7: return layout::surround_6_1;
    case 8: return layout::surround_7_1;
    default: return 0;
  }
}

int speaker_count(std::uint32_t mask) noexcept { return std::popcount(mask); }

std::uint32_t effective_channel_mask(std::uint16_t channels, std::uint32_t mask) noexcept {
  if (mask != 0 && speaker_count(mask) == channels) return mask;
  return default_channel_mask(channels);
}

std::string layout_name(std::uint16_t channels, std::uint32_t mask) {
  const std::uint32_t m = effective_channel_mask(channels, mask);
  switch (m) {
    case layout::mono: return "mono";
    case layout::stereo: return "stereo";
    case layout::surround_3_0: return "3.0";
    case layout::quad: return "quad";
    case layout::surround_5_0: return "5.0";
    case layout::surround_5_1: return "5.1";
    case layout::surround_6_1: return "6.1";
    case layout::surround_7_1: return "7.1";
    default: break;
  }
  return std::format("{} channels", channels);
}

bool MixMatrix::is_identity() const noexcept {
  if (inputs != outputs) return false;
  for (std::size_t o = 0; o < outputs; ++o) {
    for (std::size_t i = 0; i < inputs; ++i) {
      if (at(o, i) != (o == i ? 1.0f : 0.0f)) return false;
    }
  }
  return true;
}

namespace {

constexpr double minus3db = 0.70710678118654752;

// The speaker bit of each channel index for a mask (channels beyond the mask: 0).
std::vector<std::uint32_t> speakers_of(std::uint16_t channels, std::uint32_t mask) {
  std::vector<std::uint32_t> out(channels, 0);
  std::size_t index = 0;
  for (std::uint32_t bit = 1; bit != 0 && index < channels; bit <<= 1) {
    if ((mask & bit) != 0) out[index++] = bit;
  }
  return out;
}

// Where one input speaker goes when the output lacks it: list of (speaker, gain).
struct Target {
  std::uint32_t speaker;
  double gain;
};

void route(std::uint32_t source, std::uint32_t out_mask, std::vector<Target>& targets, int depth = 0) {
  if ((out_mask & source) != 0) {
    targets.push_back({source, 1.0});
    return;
  }
  if (depth > 3) return;
  const auto has = [&](std::uint32_t s) { return (out_mask & s) != 0; };
  const auto both = [&](std::uint32_t a, std::uint32_t b, double gain) {
    std::vector<Target> sub;
    route(a, out_mask, sub, depth + 1);
    route(b, out_mask, sub, depth + 1);
    for (Target& t : sub) targets.push_back({t.speaker, t.gain * gain});
  };
  const auto one = [&](std::uint32_t a, double gain) {
    std::vector<Target> sub;
    route(a, out_mask, sub, depth + 1);
    for (Target& t : sub) targets.push_back({t.speaker, t.gain * gain});
  };
  using namespace speaker;
  switch (source) {
    case front_left:
    case front_right:
      // Only a mono output lacks a front pair: mono = (L + R) / 2.
      if (has(front_center)) targets.push_back({front_center, 0.5});
      return;
    case front_center:
      if (has(front_left) && has(front_right)) {
        targets.push_back({front_left, minus3db});
        targets.push_back({front_right, minus3db});
      }
      return;
    case lfe: return;  // dropped by convention (BS.775)
    case back_left:
      if (has(side_left)) return one(side_left, 1.0);
      return one(front_left, minus3db);
    case back_right:
      if (has(side_right)) return one(side_right, 1.0);
      return one(front_right, minus3db);
    case side_left:
      if (has(back_left)) return one(back_left, 1.0);
      return one(front_left, minus3db);
    case side_right:
      if (has(back_right)) return one(back_right, 1.0);
      return one(front_right, minus3db);
    case back_center:
      if (has(back_left) && has(back_right)) return both(back_left, back_right, minus3db);
      if (has(side_left) && has(side_right)) return both(side_left, side_right, minus3db);
      return both(front_left, front_right, 0.5);
    case front_left_of_center: return one(front_left, 1.0);
    case front_right_of_center: return one(front_right, 1.0);
    case top_center: return one(front_center, minus3db);
    case top_front_left: return one(front_left, minus3db);
    case top_front_center: return one(front_center, minus3db);
    case top_front_right: return one(front_right, minus3db);
    case top_back_left: return one(back_left, minus3db);
    case top_back_center: return one(back_center, minus3db);
    case top_back_right: return one(back_right, minus3db);
    default: return;
  }
}

}  // namespace

MixMatrix mix_matrix(std::uint16_t in_channels, std::uint32_t in_mask, std::uint16_t out_channels,
                     std::uint32_t out_mask) {
  MixMatrix matrix;
  matrix.inputs = in_channels;
  matrix.outputs = out_channels;
  matrix.coefficients.assign(std::size_t{in_channels} * out_channels, 0.0f);
  if (in_channels == 0 || out_channels == 0) return matrix;
  const std::uint32_t in_m = effective_channel_mask(in_channels, in_mask);
  const std::uint32_t out_m = effective_channel_mask(out_channels, out_mask);
  const std::vector<std::uint32_t> in_speakers = speakers_of(in_channels, in_m);
  const std::vector<std::uint32_t> out_speakers = speakers_of(out_channels, out_m);
  const auto out_index = [&](std::uint32_t speaker_bit) -> std::ptrdiff_t {
    for (std::size_t o = 0; o < out_speakers.size(); ++o) {
      if (out_speakers[o] == speaker_bit) return static_cast<std::ptrdiff_t>(o);
    }
    return -1;
  };
  std::vector<double> m(matrix.coefficients.size(), 0.0);

  // Mono input: centre, or both fronts when there is no centre.
  if (in_channels == 1 && in_m == layout::mono) {
    if (out_index(speaker::front_center) >= 0) {
      m[static_cast<std::size_t>(out_index(speaker::front_center)) * in_channels] = 1.0;
    } else if (out_index(speaker::front_left) >= 0 && out_index(speaker::front_right) >= 0) {
      m[static_cast<std::size_t>(out_index(speaker::front_left))] = 1.0;
      m[static_cast<std::size_t>(out_index(speaker::front_right))] = 1.0;
    } else {
      m[0] = 1.0;
    }
  } else {
    // Down-mixing to mono goes through a stereo fold first: mono = (L + R) / 2.
    const bool to_mono = out_channels == 1 && out_m == layout::mono;
    const std::uint32_t route_mask = to_mono ? layout::stereo : out_m;
    for (std::size_t i = 0; i < in_channels; ++i) {
      std::vector<Target> targets;
      if (in_speakers[i] != 0) route(in_speakers[i], route_mask, targets);
      if (in_speakers[i] == 0) {
        if (i < out_channels) m[i * in_channels + i] = 1.0;  // unknown speaker: pass through by index
        continue;
      }
      for (const Target& target : targets) {
        if (to_mono) {
          // L and R each contribute half to the single output.
          m[i] += target.gain * 0.5;
          continue;
        }
        const std::ptrdiff_t o = out_index(target.speaker);
        if (o >= 0) m[static_cast<std::size_t>(o) * in_channels + i] += target.gain;
      }
    }
  }
  // Rows whose gains add to more than 1 could clip a full-scale input.
  for (std::size_t o = 0; o < out_channels; ++o) {
    double sum = 0.0;
    for (std::size_t i = 0; i < in_channels; ++i) sum += std::abs(m[o * in_channels + i]);
    const double scale = sum > 1.0 + 1e-9 ? 1.0 / sum : 1.0;
    for (std::size_t i = 0; i < in_channels; ++i) {
      matrix.coefficients[o * in_channels + i] = static_cast<float>(m[o * in_channels + i] * scale);
    }
  }
  return matrix;
}

std::vector<float> apply_mix(const MixMatrix& matrix, std::span<const float> interleaved) {
  if (matrix.inputs == 0) return {};
  const std::size_t frames = interleaved.size() / matrix.inputs;
  std::vector<float> out(frames * matrix.outputs, 0.0f);
  for (std::size_t f = 0; f < frames; ++f) {
    const float* in = interleaved.data() + f * matrix.inputs;
    float* dst = out.data() + f * matrix.outputs;
    for (std::size_t o = 0; o < matrix.outputs; ++o) {
      double sum = 0.0;
      for (std::size_t i = 0; i < matrix.inputs; ++i) sum += static_cast<double>(matrix.at(o, i)) * in[i];
      dst[o] = static_cast<float>(sum);
    }
  }
  return out;
}

Result<void> remix(ir::Audio& audio, std::uint16_t out_channels, std::uint32_t out_mask) {
  if (out_channels == 0 || out_channels > 64) return fail(Status::InvalidArgument, "unsupported channel count {}", out_channels);
  if (audio.channels == 0) return fail(Status::InvalidArgument, "the audio has no channels");
  const std::uint32_t target_mask = effective_channel_mask(out_channels, out_mask);
  if (out_channels == audio.channels &&
      (target_mask == 0 || target_mask == effective_channel_mask(audio.channels, audio.channel_mask))) {
    audio.channel_mask = target_mask;
    return {};
  }
  const MixMatrix matrix = mix_matrix(audio.channels, audio.channel_mask, out_channels, out_mask);
  audio.samples = apply_mix(matrix, audio.samples);
  audio.channels = out_channels;
  audio.channel_mask = target_mask;
  return {};
}

std::vector<std::vector<float>> deinterleave(std::span<const float> interleaved, std::uint16_t channels) {
  std::vector<std::vector<float>> planes(channels);
  if (channels == 0) return planes;
  const std::size_t frames = interleaved.size() / channels;
  for (std::size_t c = 0; c < channels; ++c) {
    planes[c].resize(frames);
    for (std::size_t f = 0; f < frames; ++f) planes[c][f] = interleaved[f * channels + c];
  }
  return planes;
}

std::vector<float> interleave(const std::vector<std::vector<float>>& planes) {
  if (planes.empty()) return {};
  std::size_t frames = planes[0].size();
  for (const auto& plane : planes) frames = std::min(frames, plane.size());
  std::vector<float> out(frames * planes.size());
  for (std::size_t c = 0; c < planes.size(); ++c) {
    for (std::size_t f = 0; f < frames; ++f) out[f * planes.size() + c] = planes[c][f];
  }
  return out;
}

// =========================================================================== resampling

std::optional<ResampleQuality> parse_resample_quality(std::string_view name) noexcept {
  if (name == "fast") return ResampleQuality::Fast;
  if (name == "medium") return ResampleQuality::Medium;
  if (name == "high") return ResampleQuality::High;
  if (name == "best") return ResampleQuality::Best;
  return std::nullopt;
}

ResampleSpec resample_spec(ResampleQuality quality) noexcept {
  switch (quality) {
    case ResampleQuality::Fast: return {70.0, 0.85};
    case ResampleQuality::Medium: return {100.0, 0.90};
    case ResampleQuality::High: return {130.0, 0.94};
    case ResampleQuality::Best: return {150.0, 0.97};
  }
  return {};
}

namespace {

double bessel_i0(double x) noexcept {
  double sum = 1.0;
  double term = 1.0;
  const double half = x / 2.0;
  for (int k = 1; k < 200; ++k) {
    term *= (half / k) * (half / k);
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

double sinc(double x) noexcept {
  if (std::abs(x) < 1e-12) return 1.0;
  const double px = std::numbers::pi * x;
  return std::sin(px) / px;
}

}  // namespace

Result<void> Resampler::configure(std::uint32_t in_rate, std::uint32_t out_rate, std::uint16_t channels,
                                  ResampleQuality quality) {
  if (in_rate == 0 || out_rate == 0 || in_rate > 10'000'000 || out_rate > 10'000'000) {
    return fail(Status::InvalidArgument, "unsupported sample-rate conversion {} Hz -> {} Hz", in_rate, out_rate);
  }
  if (channels == 0 || channels > 64) return fail(Status::InvalidArgument, "unsupported channel count {}", channels);
  in_rate_ = in_rate;
  out_rate_ = out_rate;
  channels_ = channels;
  const std::uint64_t g = std::gcd(std::uint64_t{in_rate}, std::uint64_t{out_rate});
  const std::uint64_t up = out_rate / g;    // L
  const std::uint64_t down = in_rate / g;   // M
  den_ = up;
  step_int_ = down / up;
  step_num_ = down % up;

  table_.clear();
  half_ = 0;
  phases_ = 0;
  if (in_rate != out_rate) {
    const ResampleSpec spec = resample_spec(quality);
    const double ratio = std::min(1.0, static_cast<double>(out_rate) / in_rate);  // lower Nyquist / input Nyquist
    const double transition = ratio * (1.0 - spec.passband_edge);                 // in units of the input Nyquist
    const double cutoff = ratio * (1.0 + spec.passband_edge) / 2.0;               // centre of the transition band
    const double attenuation = spec.stopband_db;
    const double beta = attenuation > 50.0 ? 0.1102 * (attenuation - 8.7)
                                           : 0.5842 * std::pow(attenuation - 21.0, 0.4) + 0.07886 * (attenuation - 21.0);
    // Kaiser's estimate of the length for a transition of `transition / 2` cycles per sample.
    const double length = (attenuation - 7.95) / (14.36 * transition / 2.0);
    half_ = static_cast<std::size_t>(std::ceil(length / 2.0)) + 1;
    half_ = std::clamp<std::size_t>(half_, 4, 4096);
    exact_phases_ = up <= 2048;
    phases_ = exact_phases_ ? static_cast<std::size_t>(up) : 1024;
    const std::size_t taps = 2 * half_;
    table_.assign((phases_ + 1) * taps, 0.0f);
    std::vector<double> row(taps);
    for (std::size_t p = 0; p <= phases_; ++p) {
      const double frac = static_cast<double>(p) / static_cast<double>(phases_);
      double sum = 0.0;
      for (std::size_t j = 0; j < taps; ++j) {
        // tap j weighs input sample floor(t) - half + 1 + j, at distance d from t.
        const double d = frac + static_cast<double>(half_) - 1.0 - static_cast<double>(j);
        const double x = d / static_cast<double>(half_);
        const double window = std::abs(x) >= 1.0 ? 0.0 : bessel_i0(beta * std::sqrt(1.0 - x * x)) / bessel_i0(beta);
        row[j] = cutoff * sinc(cutoff * d) * window;
        sum += row[j];
      }
      for (std::size_t j = 0; j < taps; ++j) table_[p * taps + j] = static_cast<float>(row[j] / sum);
    }
  }
  reset();
  return {};
}

void Resampler::reset() {
  history_.assign(channels_, {});
  base_ = -static_cast<std::int64_t>(half_);
  for (auto& h : history_) h.assign(half_, 0.0f);  // silence before the start
  input_frames_ = 0;
  output_frames_ = 0;
  pos_int_ = 0;
  pos_num_ = 0;
}

float Resampler::sample_at(std::size_t channel, std::int64_t index) const noexcept {
  const std::int64_t at = index - base_;
  if (at < 0 || at >= static_cast<std::int64_t>(history_[channel].size())) return 0.0f;
  return history_[channel][static_cast<std::size_t>(at)];
}

void Resampler::process(std::span<const float> interleaved, std::vector<float>& out) {
  if (channels_ == 0) return;
  const std::size_t frames = interleaved.size() / channels_;
  if (passthrough()) {
    out.insert(out.end(), interleaved.begin(), interleaved.begin() + static_cast<std::ptrdiff_t>(frames * channels_));
    input_frames_ += frames;
    output_frames_ += frames;
    return;
  }
  for (std::size_t c = 0; c < channels_; ++c) {
    std::vector<float>& h = history_[c];
    h.reserve(h.size() + frames);
    for (std::size_t f = 0; f < frames; ++f) h.push_back(interleaved[f * channels_ + c]);
  }
  input_frames_ += frames;
  produce(out, false);
}

void Resampler::flush(std::vector<float>& out) {
  if (channels_ == 0 || passthrough()) return;
  produce(out, true);
}

void Resampler::produce(std::vector<float>& out, bool final) {
  // Total output for the input seen so far (only binding once the input has ended).
  const std::uint64_t total =
      (input_frames_ * out_rate_ + in_rate_ - 1) / in_rate_;  // ceil(N * out / in)
  const std::int64_t available_end = base_ + static_cast<std::int64_t>(history_[0].size());  // exclusive
  const std::size_t taps = 2 * half_;
  std::vector<double> acc(channels_);
  while (true) {
    if (final) {
      if (output_frames_ >= total) break;
    } else {
      // Need input up to floor(t) + half (inclusive).
      if (pos_int_ + static_cast<std::int64_t>(half_) >= available_end) break;
    }
    const std::int64_t first = pos_int_ - static_cast<std::int64_t>(half_) + 1;
    const float* row0;
    const float* row1 = nullptr;
    double weight = 0.0;
    if (exact_phases_) {
      row0 = table_.data() + static_cast<std::size_t>(pos_num_) * taps;
    } else {
      const double position = static_cast<double>(pos_num_) * static_cast<double>(phases_) / static_cast<double>(den_);
      const auto p = static_cast<std::size_t>(position);
      weight = position - static_cast<double>(p);
      row0 = table_.data() + p * taps;
      row1 = row0 + taps;
    }
    for (std::size_t c = 0; c < channels_; ++c) {
      double sum = 0.0;
      const std::int64_t offset = first - base_;
      const std::vector<float>& h = history_[c];
      if (offset >= 0 && offset + static_cast<std::int64_t>(taps) <= static_cast<std::int64_t>(h.size())) {
        const float* x = h.data() + offset;
        if (row1 == nullptr) {
          for (std::size_t j = 0; j < taps; ++j) sum += static_cast<double>(row0[j]) * x[j];
        } else {
          for (std::size_t j = 0; j < taps; ++j) {
            sum += (static_cast<double>(row0[j]) + weight * (static_cast<double>(row1[j]) - row0[j])) * x[j];
          }
        }
      } else {
        for (std::size_t j = 0; j < taps; ++j) {
          const double coefficient =
              row1 == nullptr ? row0[j] : static_cast<double>(row0[j]) + weight * (static_cast<double>(row1[j]) - row0[j]);
          sum += coefficient * sample_at(c, first + static_cast<std::int64_t>(j));
        }
      }
      acc[c] = sum;
    }
    for (std::size_t c = 0; c < channels_; ++c) out.push_back(static_cast<float>(acc[c]));
    ++output_frames_;
    pos_int_ += static_cast<std::int64_t>(step_int_);
    pos_num_ += step_num_;
    if (pos_num_ >= den_) {
      pos_num_ -= den_;
      ++pos_int_;
    }
  }
  // Drop history that no future output needs.
  const std::int64_t keep_from = pos_int_ - static_cast<std::int64_t>(half_) + 1;
  if (keep_from > base_) {
    const auto drop = static_cast<std::size_t>(
        std::min<std::int64_t>(keep_from - base_, static_cast<std::int64_t>(history_[0].size())));
    if (drop > 4096 || final) {
      for (auto& h : history_) h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(drop));
      base_ += static_cast<std::int64_t>(drop);
    }
  }
}

Result<std::vector<float>> resample(std::span<const float> interleaved, std::uint16_t channels, std::uint32_t in_rate,
                                    std::uint32_t out_rate, ResampleQuality quality) {
  Resampler resampler;
  CVT_TRY(resampler.configure(in_rate, out_rate, channels, quality));
  std::vector<float> out;
  out.reserve(static_cast<std::size_t>(
      static_cast<double>(interleaved.size()) * out_rate / in_rate + channels * 2.0));
  resampler.process(interleaved, out);
  resampler.flush(out);
  return out;
}

Result<void> resample(ir::Audio& audio, std::uint32_t out_rate, ResampleQuality quality) {
  if (audio.sample_rate == out_rate) return {};
  CVT_TRY_ASSIGN(audio.samples, resample(audio.samples, audio.channels, audio.sample_rate, out_rate, quality));
  audio.sample_rate = out_rate;
  return {};
}

// =========================================================================== quantisation

std::optional<Dither> parse_dither(std::string_view name) noexcept {
  if (name == "none") return Dither::None;
  if (name == "tpdf" || name == "triangular") return Dither::Triangular;
  if (name == "shaped") return Dither::Shaped;
  return std::nullopt;
}

bool exactly_representable(std::span<const float> samples, int bits) noexcept {
  if (bits >= 25) {
    // Every float in range has at most 24 significant bits.
    for (const float s : samples) {
      if (!(s >= -1.0f && s < 1.0f)) return false;
    }
    return true;
  }
  const double scale = static_cast<double>(std::uint64_t{1} << (bits - 1));
  for (const float s : samples) {
    const double v = static_cast<double>(s) * scale;
    if (!(v >= -scale && v <= scale - 1.0) || v != std::floor(v)) return false;
  }
  return true;
}

Quantizer::Quantizer(int bits, std::uint16_t channels, Dither dither, std::uint32_t sample_rate, std::uint32_t seed)
    : bits_(std::clamp(bits, 1, 32)),
      channels_(channels == 0 ? 1 : channels),
      dither_(dither),
      state_(0x9E3779B97F4A7C15ull ^ seed) {
  scale_ = static_cast<double>(std::uint64_t{1} << (bits_ - 1));
  max_ = scale_ - 1.0;
  min_ = -scale_;
  if (dither_ == Dither::Shaped) {
    if (sample_rate >= 40000 && sample_rate <= 50000) {
      // Lipshitz/Wannamaker minimally audible 5-tap shaping (44.1/48 kHz).
      shaping_ = {2.033, -2.165, 1.959, -1.590, 0.6149};
    } else {
      shaping_ = {1.0};  // first-order high-pass shaping at other rates
    }
  }
  error_.assign(channels_, std::vector<double>(shaping_.size(), 0.0));
}

double Quantizer::next_uniform() noexcept {
  // xorshift64*: uniform in [0, 1).
  state_ ^= state_ >> 12;
  state_ ^= state_ << 25;
  state_ ^= state_ >> 27;
  const std::uint64_t r = state_ * 0x2545F4914F6CDD1Dull;
  return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);
}

void Quantizer::process(std::span<const float> interleaved, std::span<std::int32_t> out) {
  const std::size_t n = std::min(interleaved.size(), out.size());
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t c = channel_;
    channel_ = channel_ + 1 == channels_ ? 0 : channel_ + 1;
    double v = static_cast<double>(interleaved[i]) * scale_;
    if (!std::isfinite(v)) v = 0.0;
    std::vector<double>& err = error_[c];
    double shaped = v;
    for (std::size_t k = 0; k < shaping_.size(); ++k) shaped -= shaping_[k] * err[k];
    double q = shaped;
    if (dither_ != Dither::None) q += next_uniform() - next_uniform();
    q = std::floor(q + 0.5);
    q = std::clamp(q, min_, max_);
    if (!shaping_.empty()) {
      // Error fed back; bounded so that clipping cannot make the loop unstable.
      const double e = std::clamp(q - shaped, -4.0, 4.0);
      for (std::size_t k = err.size() - 1; k > 0; --k) err[k] = err[k - 1];
      err[0] = e;
    }
    out[i] = static_cast<std::int32_t>(q);
  }
}

std::vector<std::int32_t> Quantizer::process(std::span<const float> interleaved) {
  std::vector<std::int32_t> out(interleaved.size());
  process(interleaved, out);
  return out;
}

std::vector<std::int32_t> to_integers(std::span<const float> interleaved, std::uint16_t channels, int bits,
                                      Dither dither, std::uint32_t sample_rate) {
  if (exactly_representable(interleaved, bits)) dither = Dither::None;
  Quantizer quantizer(bits, channels, dither, sample_rate);
  return quantizer.process(interleaved);
}

// =========================================================================== measurement

Levels measure_levels(std::span<const float> interleaved, std::uint16_t channels) {
  Levels levels;
  if (channels == 0) return levels;
  levels.channel_peak.assign(channels, 0.0);
  levels.channel_rms.assign(channels, 0.0);
  const std::size_t frames = interleaved.size() / channels;
  double total = 0.0;
  for (std::size_t f = 0; f < frames; ++f) {
    for (std::size_t c = 0; c < channels; ++c) {
      const double s = interleaved[f * channels + c];
      levels.channel_peak[c] = std::max(levels.channel_peak[c], std::abs(s));
      levels.channel_rms[c] += s * s;
    }
  }
  for (std::size_t c = 0; c < channels; ++c) {
    total += levels.channel_rms[c];
    levels.channel_rms[c] = frames == 0 ? 0.0 : std::sqrt(levels.channel_rms[c] / static_cast<double>(frames));
    levels.peak = std::max(levels.peak, levels.channel_peak[c]);
  }
  levels.rms = frames == 0 ? 0.0 : std::sqrt(total / static_cast<double>(frames * channels));
  return levels;
}

double true_peak(const ir::Audio& audio) {
  if (!audio.valid() || audio.samples.empty()) return 0.0;
  const std::uint64_t rate4 = std::uint64_t{audio.sample_rate} * 4;
  if (rate4 > 10'000'000) return measure_levels(audio.samples, audio.channels).peak;
  auto up = resample(audio.samples, audio.channels, audio.sample_rate, static_cast<std::uint32_t>(rate4),
                     ResampleQuality::Medium);
  double peak = measure_levels(audio.samples, audio.channels).peak;
  if (up) {
    for (const float s : *up) peak = std::max(peak, static_cast<double>(std::abs(s)));
  }
  return peak;
}

namespace {

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double z1 = 0, z2 = 0;
  double run(double x) noexcept {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

// The two K-weighting stages of BS.1770 for any sample rate (the reference
// coefficients at 48 kHz derived back to analogue prototypes).
std::array<Biquad, 2> k_weighting(double rate) {
  std::array<Biquad, 2> filters{};
  {
    const double f0 = 1681.974450955533, gain_db = 3.999843853973347, q = 0.7071752369554196;
    const double k = std::tan(std::numbers::pi * f0 / rate);
    const double vh = std::pow(10.0, gain_db / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    Biquad& s = filters[0];
    s.b0 = (vh + vb * k / q + k * k) / a0;
    s.b1 = 2.0 * (k * k - vh) / a0;
    s.b2 = (vh - vb * k / q + k * k) / a0;
    s.a1 = 2.0 * (k * k - 1.0) / a0;
    s.a2 = (1.0 - k / q + k * k) / a0;
  }
  {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(std::numbers::pi * f0 / rate);
    const double a0 = 1.0 + k / q + k * k;
    Biquad& s = filters[1];
    s.b0 = 1.0;
    s.b1 = -2.0;
    s.b2 = 1.0;
    s.a1 = 2.0 * (k * k - 1.0) / a0;
    s.a2 = (1.0 - k / q + k * k) / a0;
  }
  return filters;
}

}  // namespace

double integrated_loudness(const ir::Audio& audio) {
  constexpr double minus_inf = -std::numeric_limits<double>::infinity();
  if (!audio.valid() || audio.sample_rate < 1000) return minus_inf;
  const std::size_t channels = audio.channels;
  const std::size_t frames = audio.frame_count();
  const std::size_t block = static_cast<std::size_t>(std::llround(0.4 * audio.sample_rate));
  const std::size_t hop = block / 4;
  if (frames < block || hop == 0) return minus_inf;
  const std::vector<std::uint32_t> speakers =
      speakers_of(audio.channels, effective_channel_mask(audio.channels, audio.channel_mask));
  std::vector<double> weight(channels, 1.0);
  for (std::size_t c = 0; c < channels; ++c) {
    const std::uint32_t s = speakers[c];
    if (s == speaker::lfe) weight[c] = 0.0;
    if (s == speaker::back_left || s == speaker::back_right || s == speaker::side_left || s == speaker::side_right) {
      weight[c] = 1.41;
    }
  }
  // Weighted squared K-filtered signal summed over channels, per hop.
  const std::size_t hops = frames / hop;
  std::vector<double> hop_energy(hops, 0.0);
  for (std::size_t c = 0; c < channels; ++c) {
    if (weight[c] == 0.0) continue;
    auto filters = k_weighting(audio.sample_rate);
    for (std::size_t f = 0; f < hops * hop; ++f) {
      const double y = filters[1].run(filters[0].run(audio.samples[f * channels + c]));
      hop_energy[f / hop] += weight[c] * y * y;
    }
  }
  std::vector<double> blocks;
  for (std::size_t h = 0; h + 4 <= hops; ++h) {
    const double z = (hop_energy[h] + hop_energy[h + 1] + hop_energy[h + 2] + hop_energy[h + 3]) /
                     static_cast<double>(hop * 4);
    blocks.push_back(z);
  }
  const auto loudness = [](double z) { return z > 0.0 ? -0.691 + 10.0 * std::log10(z) : minus_inf; };
  double sum = 0.0;
  std::size_t count = 0;
  for (const double z : blocks) {
    if (loudness(z) > -70.0) {
      sum += z;
      ++count;
    }
  }
  if (count == 0) return minus_inf;
  const double relative_gate = loudness(sum / static_cast<double>(count)) - 10.0;
  sum = 0.0;
  count = 0;
  for (const double z : blocks) {
    const double l = loudness(z);
    if (l > -70.0 && l > relative_gate) {
      sum += z;
      ++count;
    }
  }
  if (count == 0) return minus_inf;
  return loudness(sum / static_cast<double>(count));
}

double to_db(double linear) noexcept {
  return linear > 0.0 ? 20.0 * std::log10(linear) : -std::numeric_limits<double>::infinity();
}
double from_db(double db) noexcept { return std::pow(10.0, db / 20.0); }

// =========================================================================== editing helpers

void apply_gain(std::span<float> samples, float gain) noexcept {
  for (float& s : samples) s *= gain;
}

double normalize_peak(ir::Audio& audio, double target_dbfs) {
  const double peak = measure_levels(audio.samples, audio.channels).peak;
  if (!(peak > 0.0)) return 1.0;
  const double gain = from_db(target_dbfs) / peak;
  apply_gain(audio.samples, static_cast<float>(gain));
  return gain;
}

double normalize_loudness(ir::Audio& audio, double target_lufs, double ceiling_dbfs) {
  const double loudness = integrated_loudness(audio);
  if (!std::isfinite(loudness)) return 1.0;
  double gain = from_db(target_lufs - loudness);
  const double peak = true_peak(audio);
  if (peak > 0.0) gain = std::min(gain, from_db(ceiling_dbfs) / peak);
  apply_gain(audio.samples, static_cast<float>(gain));
  return gain;
}

void trim(ir::Audio& audio, std::size_t first_frame, std::size_t frame_count) {
  const std::size_t frames = audio.frame_count();
  first_frame = std::min(first_frame, frames);
  frame_count = std::min(frame_count, frames - first_frame);
  const std::size_t ch = audio.channels;
  std::vector<float> kept(audio.samples.begin() + static_cast<std::ptrdiff_t>(first_frame * ch),
                          audio.samples.begin() + static_cast<std::ptrdiff_t>((first_frame + frame_count) * ch));
  audio.samples = std::move(kept);
}

std::size_t trim_silence(ir::Audio& audio, double threshold_dbfs) {
  const double threshold = from_db(threshold_dbfs);
  const std::size_t frames = audio.frame_count();
  const std::size_t ch = audio.channels;
  const auto loud = [&](std::size_t f) {
    for (std::size_t c = 0; c < ch; ++c) {
      if (std::abs(audio.samples[f * ch + c]) > threshold) return true;
    }
    return false;
  };
  std::size_t first = 0;
  while (first < frames && !loud(first)) ++first;
  std::size_t last = frames;
  while (last > first && !loud(last - 1)) --last;
  trim(audio, first, last - first);
  return first;
}

namespace {
double fade_gain(double x, FadeCurve curve) noexcept {
  x = std::clamp(x, 0.0, 1.0);
  switch (curve) {
    case FadeCurve::Linear: return x;
    case FadeCurve::EqualPower: return std::sin(x * std::numbers::pi / 2.0);
    case FadeCurve::Logarithmic: return x <= 0.0 ? 0.0 : std::pow(10.0, (x - 1.0) * 3.0);  // 60 dB range
  }
  return x;
}
}  // namespace

void fade_in(ir::Audio& audio, std::size_t frames, FadeCurve curve) {
  frames = std::min(frames, audio.frame_count());
  for (std::size_t f = 0; f < frames; ++f) {
    const auto g = static_cast<float>(fade_gain(static_cast<double>(f) / static_cast<double>(frames), curve));
    for (std::size_t c = 0; c < audio.channels; ++c) audio.samples[f * audio.channels + c] *= g;
  }
}

void fade_out(ir::Audio& audio, std::size_t frames, FadeCurve curve) {
  const std::size_t total = audio.frame_count();
  frames = std::min(frames, total);
  for (std::size_t k = 0; k < frames; ++k) {
    const std::size_t f = total - frames + k;
    const auto g = static_cast<float>(
        fade_gain(static_cast<double>(frames - 1 - k) / static_cast<double>(frames), curve));
    for (std::size_t c = 0; c < audio.channels; ++c) audio.samples[f * audio.channels + c] *= g;
  }
}

}  // namespace cvt::audio::dsp
