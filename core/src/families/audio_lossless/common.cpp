// Convertible core - audio_lossless family: shared helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossless/common.hpp"

#include <cmath>
#include <cstddef>
#include <limits>

namespace cvt::audio {

Result<void> check_sample_budget(const Context& ctx, std::uint64_t samples) {
  if (samples > std::numeric_limits<std::uint64_t>::max() / 4) return fail(Status::Limit, "the audio is too long");
  CVT_TRY(ctx.limits.check_decompressed(samples * 4));
  if (samples * 4 > std::numeric_limits<std::size_t>::max() / 2) return fail(Status::Limit, "the audio is too long");
  return {};
}

ir::Audio make_audio(std::uint32_t sample_rate, std::uint16_t channels, std::uint16_t bits, bool float_samples,
                     std::uint32_t channel_mask) {
  ir::Audio audio;
  audio.sample_rate = sample_rate;
  audio.channels = channels;
  audio.bits_per_sample = bits;
  audio.float_samples = float_samples;
  audio.channel_mask = channel_mask;
  return audio;
}

Result<void> check_stream_params(std::uint32_t sample_rate, std::uint32_t channels, std::string_view what) {
  if (channels == 0 || channels > 255) return fail(Status::Decode, "{} declares {} channels", what, channels);
  if (sample_rate == 0 || sample_rate > 10'000'000) {
    return fail(Status::Decode, "{} declares an invalid sample rate of {} Hz", what, sample_rate);
  }
  return {};
}

void set_frame_count(ir::Audio& audio, std::uint64_t frames) {
  if (audio.channels == 0) return;
  const std::uint64_t wanted = frames * audio.channels;
  if (wanted < audio.samples.size()) audio.samples.resize(static_cast<std::size_t>(wanted));
}

double read_extended(ByteView ten_bytes) noexcept {
  if (ten_bytes.size() < 10) return 0.0;
  util::ByteReader r(ten_bytes);
  const std::uint16_t sign_exponent = r.u16be();
  const std::uint64_t mantissa = r.u64be();
  const int exponent = sign_exponent & 0x7FFF;
  if (exponent == 0 && mantissa == 0) return 0.0;
  if (exponent == 0x7FFF) return 0.0;  // infinity / NaN: not a usable rate
  double value = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
  if ((sign_exponent & 0x8000) != 0) value = -value;
  return value;
}

void write_extended(util::ByteWriter& out, double value) {
  if (!(value > 0.0) || !std::isfinite(value)) {
    out.zeros(10);
    return;
  }
  int exponent = 0;
  const double fraction = std::frexp(value, &exponent);  // value = fraction * 2^exponent, fraction in [0.5, 1)
  const auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 64));
  out.u16be(static_cast<std::uint16_t>(exponent - 1 + 16383));
  out.u64be(mantissa);
}

}  // namespace cvt::audio
