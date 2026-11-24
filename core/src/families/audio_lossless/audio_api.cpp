// Convertible core - audio_lossless family: free functions for other families.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossless/audio_api.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <format>
#include <numbers>
#include <utility>

#include "engine/standard_options.hpp"

namespace cvt::audio {

// =========================================================================== options

OptionSpec dither_option() {
  return OptionSpec::choice(std::string(key_dither), "Dither", "tpdf", {"tpdf", "shaped", "none"},
                            "Noise added when the bit depth is reduced, to avoid distortion; shaped moves it "
                            "to less audible frequencies.")
      .advanced();
}

OptionSpec resample_quality_option() {
  return OptionSpec::choice(std::string(key_resample_quality), "Resampling quality", "high",
                            {"fast", "medium", "high", "best"},
                            "Filter quality used when the sample rate changes; higher is cleaner and slower.")
      .advanced();
}

OptionSpec normalize_option() {
  return OptionSpec::choice(std::string(key_normalize), "Normalise", "none", {"none", "peak", "loudness"},
                            "Adjust the volume: peak raises the loudest sample to -1 dBFS, loudness targets "
                            "-16 LUFS (EBU R128) without exceeding -1 dB true peak.")
      .advanced();
}

OptionSpec sample_format_option(std::vector<std::string> choices) {
  std::string first = choices.empty() ? std::string("auto") : choices.front();
  if (choices.empty()) choices.push_back(first);
  return OptionSpec::choice(std::string(key_sample_format), "Sample format", std::move(first), std::move(choices),
                            "Bit depth and encoding of the samples; auto keeps the source's precision.");
}

std::vector<OptionSpec> standard_write_options() {
  return {options::sample_rate(), options::channels(), resample_quality_option(), normalize_option(),
          options::strip_metadata()};
}

dsp::Dither resolved_dither(const Options& options) {
  const std::string value = options.get_enum(key_dither, "tpdf", {"tpdf", "shaped", "none"});
  return dsp::parse_dither(value).value_or(dsp::Dither::Triangular);
}

dsp::ResampleQuality resolved_resample_quality(const Options& options) {
  const std::string value = options.get_enum(key_resample_quality, "high", {"fast", "medium", "high", "best"});
  return dsp::parse_resample_quality(value).value_or(dsp::ResampleQuality::High);
}

// =========================================================================== pipeline

Result<void> check_writable(const ir::Audio& audio) {
  if (!audio.valid()) return fail(Status::Encode, "the audio has no valid sample rate or channel layout");
  if (audio.channels > 255) return fail(Status::Encode, "{} channels are more than this format can hold", audio.channels);
  if (audio.sample_rate > 10'000'000) return fail(Status::Encode, "sample rate {} Hz is out of range", audio.sample_rate);
  return {};
}

Result<ir::Audio> prepare_for_writing(const ir::Audio& source, const Context& ctx, std::uint32_t min_rate,
                                      std::uint32_t max_rate, std::uint16_t max_channels) {
  CVT_TRY(check_writable(source));
  ir::Audio audio = source;
  std::uint16_t channels = options::resolved_channels(ctx.options, audio.channels);
  if (max_channels != 0 && channels > max_channels) {
    ctx.warn(std::format("The format holds at most {} channels; the audio was mixed down.", max_channels));
    channels = max_channels;
  }
  if (channels != audio.channels) CVT_TRY(dsp::remix(audio, channels));
  std::uint32_t rate = options::resolved_sample_rate(ctx.options, audio.sample_rate);
  if (max_rate != 0 && rate > max_rate) {
    ctx.warn(std::format("The format supports at most {} Hz; the audio was resampled.", max_rate));
    rate = max_rate;
  }
  if (min_rate != 0 && rate < min_rate) {
    ctx.warn(std::format("The format needs at least {} Hz; the audio was resampled.", min_rate));
    rate = min_rate;
  }
  if (rate != audio.sample_rate) {
    CVT_TRY(ctx.checkpoint(0.0, "Resampling"));
    CVT_TRY(dsp::resample(audio, rate, resolved_resample_quality(ctx.options)));
  }
  const std::string normalize = ctx.options.get_enum(key_normalize, "none", {"none", "peak", "loudness"});
  if (normalize == "peak") {
    dsp::normalize_peak(audio, -1.0);
  } else if (normalize == "loudness") {
    dsp::normalize_loudness(audio, -16.0, -1.0);
  }
  if (options::resolved_strip_metadata(ctx.options)) audio.tags.clear();
  return audio;
}

int default_bit_depth(const ir::Audio& audio, std::span<const int> allowed, int fallback) {
  if (audio.bits_per_sample == 0 || audio.float_samples) return fallback;
  int best = 0;
  for (const int bits : allowed) {
    if (bits == audio.bits_per_sample) return bits;
    if (bits > audio.bits_per_sample && (best == 0 || bits < best)) best = bits;
  }
  return best != 0 ? best : fallback;
}

std::vector<std::int32_t> quantize_for(const ir::Audio& audio, int bits, const Options& options) {
  return dsp::to_integers(audio.samples, audio.channels, bits, resolved_dither(options), audio.sample_rate);
}

// =========================================================================== pictures

namespace {

struct Rgb {
  double r, g, b;
};

Rgb unpack(std::uint32_t rgb) {
  return {static_cast<double>((rgb >> 16) & 0xFF), static_cast<double>((rgb >> 8) & 0xFF),
          static_cast<double>(rgb & 0xFF)};
}

void put_pixel(ir::Frame& frame, std::uint32_t x, std::uint32_t y, Rgb c, std::uint8_t alpha = 255) {
  const std::size_t at = frame.offset(x, y);
  frame.rgba[at] = static_cast<std::uint8_t>(std::clamp(c.r + 0.5, 0.0, 255.0));
  frame.rgba[at + 1] = static_cast<std::uint8_t>(std::clamp(c.g + 0.5, 0.0, 255.0));
  frame.rgba[at + 2] = static_cast<std::uint8_t>(std::clamp(c.b + 0.5, 0.0, 255.0));
  frame.rgba[at + 3] = alpha;
}

Rgb mix(Rgb a, Rgb b, double t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

void fft(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; (j & bit) != 0; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double angle = -2.0 * std::numbers::pi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(angle), std::sin(angle));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k];
        const std::complex<double> v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

Rgb palette_colour(std::string_view palette, double t) {
  t = std::clamp(t, 0.0, 1.0);
  static constexpr std::array<std::array<double, 3>, 5> magma = {
      {{0, 0, 4}, {81, 18, 124}, {183, 55, 121}, {252, 137, 97}, {252, 253, 191}}};
  static constexpr std::array<std::array<double, 3>, 5> viridis = {
      {{68, 1, 84}, {59, 82, 139}, {33, 145, 140}, {94, 201, 98}, {253, 231, 37}}};
  if (palette == "gray") return {255.0 * t, 255.0 * t, 255.0 * t};
  const auto& stops = palette == "viridis" ? viridis : magma;
  const double position = t * 4.0;
  const auto i = std::min<std::size_t>(3, static_cast<std::size_t>(position));
  const double f = position - static_cast<double>(i);
  const Rgb a{stops[i][0], stops[i][1], stops[i][2]};
  const Rgb b{stops[i + 1][0], stops[i + 1][1], stops[i + 1][2]};
  return mix(a, b, f);
}

}  // namespace

Result<ir::Image> render_waveform(const ir::Audio& audio, const WaveformStyle& style) {
  if (!audio.valid()) return fail(Status::Encode, "the audio is not valid");
  if (style.width == 0 || style.height == 0 || style.width > 16384 || style.height > 16384) {
    return fail(Status::InvalidArgument, "waveform size {}x{} is out of range", style.width, style.height);
  }
  ir::Frame frame = ir::Frame::filled(style.width, style.height);
  const Rgb background = unpack(style.background);
  const Rgb foreground = unpack(style.foreground);
  const Rgb core = mix(foreground, Rgb{0, 0, 0}, 0.35);
  const std::uint8_t back_alpha = style.transparent ? 0 : 255;
  for (std::uint32_t y = 0; y < style.height; ++y) {
    for (std::uint32_t x = 0; x < style.width; ++x) put_pixel(frame, x, y, background, back_alpha);
  }
  const std::size_t frames = audio.frame_count();
  const std::size_t channels = audio.channels;
  const std::size_t lanes = style.split_channels ? std::min<std::size_t>(channels, style.height) : 1;
  const double lane_height = static_cast<double>(style.height) / static_cast<double>(lanes);
  for (std::size_t lane = 0; lane < lanes; ++lane) {
    const double top = lane_height * static_cast<double>(lane);
    const double centre = top + lane_height / 2.0;
    const double amplitude = (lane_height - 2.0) / 2.0;
    for (std::uint32_t x = 0; x < style.width; ++x) {
      if (frames == 0) break;
      const std::size_t first = static_cast<std::size_t>(static_cast<std::uint64_t>(x) * frames / style.width);
      std::size_t last = static_cast<std::size_t>(static_cast<std::uint64_t>(x + 1) * frames / style.width);
      last = std::max(last, first + 1);
      last = std::min(last, frames);
      double lo = 0.0, hi = 0.0, sum = 0.0;
      bool started = false;
      for (std::size_t f = first; f < last; ++f) {
        double v = 0.0;
        if (style.split_channels) {
          v = audio.samples[f * channels + lane];
        } else {
          for (std::size_t c = 0; c < channels; ++c) v += audio.samples[f * channels + c];
          v /= static_cast<double>(channels);
        }
        v = std::clamp(v, -1.0, 1.0);
        if (!started) {
          lo = hi = v;
          started = true;
        }
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        sum += v * v;
      }
      const double rms = std::sqrt(sum / static_cast<double>(last - first));
      const auto row = [&](double v) {
        const double y = centre - v * amplitude;
        return static_cast<std::uint32_t>(std::clamp(y, top, top + lane_height - 1.0));
      };
      const std::uint32_t y0 = row(hi), y1 = row(lo);
      for (std::uint32_t y = y0; y <= y1; ++y) put_pixel(frame, x, y, foreground);
      const std::uint32_t r0 = row(std::min(rms, hi)), r1 = row(std::max(-rms, lo));
      for (std::uint32_t y = r0; y <= r1; ++y) put_pixel(frame, x, y, core);
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

Result<ir::Image> render_spectrogram(const ir::Audio& audio, const SpectrogramStyle& style, const Context* ctx) {
  if (!audio.valid()) return fail(Status::Encode, "the audio is not valid");
  if (style.width == 0 || style.height == 0 || style.width > 16384 || style.height > 16384) {
    return fail(Status::InvalidArgument, "spectrogram size {}x{} is out of range", style.width, style.height);
  }
  const std::size_t n = style.fft_size;
  if (n < 64 || n > 16384 || (n & (n - 1)) != 0) {
    return fail(Status::InvalidArgument, "FFT size {} must be a power of two from 64 to 16384", n);
  }
  ir::Frame frame = ir::Frame::filled(style.width, style.height, {0, 0, 0, 255});
  const std::size_t frames = audio.frame_count();
  const std::size_t channels = audio.channels;
  std::vector<double> window(n);
  double window_sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
    window_sum += window[i];
  }
  const std::size_t bins = n / 2;
  const double nyquist = audio.sample_rate / 2.0;
  // Bin range of every row (row 0 = top = highest frequency).
  std::vector<std::pair<std::size_t, std::size_t>> rows(style.height);
  for (std::uint32_t y = 0; y < style.height; ++y) {
    const double u0 = static_cast<double>(style.height - 1 - y) / style.height;
    const double u1 = static_cast<double>(style.height - y) / style.height;
    double f0, f1;
    if (style.log_frequency) {
      const double low = std::min(20.0, nyquist / 2.0);
      f0 = low * std::pow(nyquist / low, u0);
      f1 = low * std::pow(nyquist / low, u1);
      if (y == style.height - 1) f0 = 0.0;
    } else {
      f0 = u0 * nyquist;
      f1 = u1 * nyquist;
    }
    auto b0 = static_cast<std::size_t>(f0 / nyquist * static_cast<double>(bins));
    auto b1 = static_cast<std::size_t>(std::ceil(f1 / nyquist * static_cast<double>(bins)));
    b0 = std::min(b0, bins - 1);
    b1 = std::clamp(b1, b0 + 1, bins);
    rows[y] = {b0, b1};
  }
  std::vector<std::complex<double>> buffer(n);
  std::vector<double> level(bins);
  for (std::uint32_t x = 0; x < style.width; ++x) {
    if (ctx != nullptr && (x & 31) == 0) CVT_TRY(ctx->checkpoint(static_cast<double>(x) / style.width, "Analysing"));
    const double centre = (static_cast<double>(x) + 0.5) * static_cast<double>(frames) / style.width;
    const auto start = static_cast<std::int64_t>(centre) - static_cast<std::int64_t>(n / 2);
    for (std::size_t i = 0; i < n; ++i) {
      const std::int64_t f = start + static_cast<std::int64_t>(i);
      double v = 0.0;
      if (f >= 0 && static_cast<std::size_t>(f) < frames) {
        for (std::size_t c = 0; c < channels; ++c) v += audio.samples[static_cast<std::size_t>(f) * channels + c];
        v /= static_cast<double>(channels);
      }
      buffer[i] = {v * window[i], 0.0};
    }
    fft(buffer);
    for (std::size_t k = 0; k < bins; ++k) {
      const double magnitude = std::abs(buffer[k]) * 2.0 / window_sum;
      level[k] = magnitude > 1e-12 ? 20.0 * std::log10(magnitude) : -240.0;
    }
    for (std::uint32_t y = 0; y < style.height; ++y) {
      double best = -240.0;
      for (std::size_t k = rows[y].first; k < rows[y].second; ++k) best = std::max(best, level[k]);
      const double t = (best + style.range_db) / style.range_db;
      put_pixel(frame, x, y, palette_colour(style.palette, t));
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

}  // namespace cvt::audio
