// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/json.hpp"

using namespace cvt;

namespace {

/// |value - expected| <= tolerance (false for NaN).
bool near(double value, double expected, double tolerance) { return std::abs(value - expected) <= tolerance; }

bool is_positive_infinity(double value) { return std::isinf(value) && value > 0.0; }

/// `frame` with `delta` added to channel `channel` of every pixel.
ir::Frame with_channel_offset(ir::Frame frame, std::size_t channel, int delta) {
  for (std::size_t i = channel; i < frame.rgba.size(); i += 4) {
    frame.rgba[i] = static_cast<std::uint8_t>(frame.rgba[i] + delta);
  }
  return frame;
}

/// `frame` with uniform noise in [-amplitude, +amplitude] added to R, G and B.
ir::Frame with_noise(ir::Frame frame, int amplitude, std::uint32_t seed) {
  const Bytes noise = test::random_bytes(frame.rgba.size(), seed);
  for (std::size_t i = 0; i < frame.rgba.size(); ++i) {
    if (i % 4 == 3) continue;
    const int delta = std::to_integer<int>(noise[i]) % (2 * amplitude + 1) - amplitude;
    frame.rgba[i] = static_cast<std::uint8_t>(std::clamp(frame.rgba[i] + delta, 0, 255));
  }
  return frame;
}

/// `audio` delayed by `frames` (zeros inserted at the start) or, for a
/// negative count, advanced (frames removed from the start).
ir::Audio shifted(const ir::Audio& audio, std::ptrdiff_t frames) {
  ir::Audio out = audio;
  if (frames >= 0) {
    out.samples.insert(out.samples.begin(), static_cast<std::size_t>(frames) * audio.channels, 0.0f);
  } else {
    out.samples.erase(out.samples.begin(),
                      out.samples.begin() + (-frames) * static_cast<std::ptrdiff_t>(audio.channels));
  }
  return out;
}

/// Adds `delta` (in stored units) to every valid sample of every plane.
ir::VideoFrame with_sample_offset(ir::VideoFrame frame, int delta) {
  const std::size_t bytes = ir::bytes_per_sample(frame.format);
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    const std::size_t rows = ir::plane_height(frame.format, p, frame.height);
    const std::size_t row_bytes = ir::plane_row_bytes(frame.format, p, frame.width);
    for (std::size_t y = 0; y < rows; ++y) {
      std::uint8_t* row = frame.planes[p].data.data() + y * frame.planes[p].stride;
      for (std::size_t i = 0; i < row_bytes; i += bytes) {
        if (bytes == 1) {
          row[i] = static_cast<std::uint8_t>(row[i] + delta);
        } else {
          const int value = (row[i] | (row[i + 1] << 8)) + delta;
          row[i] = static_cast<std::uint8_t>(value & 0xFF);
          row[i + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        }
      }
    }
  }
  return frame;
}

/// The same picture with every stride enlarged and the padding filled with junk.
ir::VideoFrame with_padded_strides(const ir::VideoFrame& frame, std::size_t padding) {
  ir::VideoFrame out = frame;
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    const std::size_t rows = ir::plane_height(frame.format, p, frame.height);
    const std::size_t row_bytes = ir::plane_row_bytes(frame.format, p, frame.width);
    out.planes[p].stride = row_bytes + padding;
    out.planes[p].data.assign(out.planes[p].stride * rows, std::uint8_t{0xA5});
    for (std::size_t y = 0; y < rows; ++y) {
      std::copy_n(frame.planes[p].data.begin() + static_cast<std::ptrdiff_t>(y * frame.planes[p].stride), row_bytes,
                  out.planes[p].data.begin() + static_cast<std::ptrdiff_t>(y * out.planes[p].stride));
    }
  }
  return out;
}

/// Stored sample `index` of row `y` of a plane (a byte, or a little-endian 16-bit word).
int sample_at(const ir::VideoFrame& frame, std::size_t plane, std::size_t index, std::size_t y) {
  const std::uint8_t* row = frame.planes[plane].data.data() + y * frame.planes[plane].stride;
  if (ir::bytes_per_sample(frame.format) == 1) return row[index];
  return row[index * 2] | (row[index * 2 + 1] << 8);
}

/// "R G B A" of one pixel, for readable failure messages.
std::string pixel(const ir::Frame& frame, std::uint32_t x, std::uint32_t y) {
  const std::size_t o = frame.offset(x, y);
  return std::format("{} {} {} {}", frame.rgba[o], frame.rgba[o + 1], frame.rgba[o + 2], frame.rgba[o + 3]);
}

Json json(std::string_view text) {
  auto parsed = util::parse_json(text);
  REQUIRE_MESSAGE(parsed, "test JSON does not parse: ", text);
  return std::move(*parsed);
}

/// A unit cube: 8 vertices, 12 triangles.
ir::Mesh cube() {
  ir::Mesh mesh;
  for (int i = 0; i < 8; ++i) {
    mesh.positions.push_back({static_cast<float>(i & 1), static_cast<float>((i >> 1) & 1),
                              static_cast<float>((i >> 2) & 1)});
  }
  mesh.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                  2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
  return mesh;
}

const double pi = std::numbers::pi;

}  // namespace

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

TEST_CASE("support: image PSNR has the documented known answers") {
  const ir::Frame base = ir::Frame::filled(16, 12, {100, 110, 120, 255});

  // One channel off by 1 everywhere: MSE over the 4 channels is 1/4.
  const double one_channel = 10.0 * std::log10(255.0 * 255.0 * 4.0);
  CHECK(near(one_channel, 54.15140352195873, 1e-9));
  for (const std::size_t channel : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) {
    CAPTURE(channel);
    CHECK(near(test::psnr(base, with_channel_offset(base, channel, 1)), one_channel, 1e-9));
    CHECK(near(test::psnr(base, with_channel_offset(base, channel, -1)), one_channel, 1e-9));
    CHECK(test::max_abs_diff(base, with_channel_offset(base, channel, 1)) == 1.0);
  }
  // R, G and B off by 1: MSE 3/4. Off by 2 costs 6.02 dB.
  const ir::Frame rgb_off = with_channel_offset(with_channel_offset(with_channel_offset(base, 0, 1), 1, 1), 2, 1);
  CHECK(near(test::psnr(base, rgb_off), 10.0 * std::log10(255.0 * 255.0 * 4.0 / 3.0), 1e-9));
  CHECK(near(test::psnr(base, with_channel_offset(base, 1, 2)), one_channel - 20.0 * std::log10(2.0), 1e-9));
  // Alpha is a channel of its own: black pixels, alpha 255 vs 254.
  const ir::Frame black = ir::Frame::filled(16, 12, {0, 0, 0, 255});
  CHECK(near(test::psnr(black, with_channel_offset(black, 3, -1)), one_channel, 1e-9));
  CHECK(test::max_abs_diff(black, with_channel_offset(black, 3, -1)) == 1.0);
  // The metric is symmetric.
  CHECK(test::psnr(rgb_off, base) == test::psnr(base, rgb_off));
  // Opposite extremes: 0 dB.
  CHECK(near(test::psnr(ir::Frame::filled(4, 4, {0, 0, 0, 0}), ir::Frame::filled(4, 4, {255, 255, 255, 255})), 0.0,
             1e-9));
}

TEST_CASE("support: identical images score as identical") {
  for (const ir::Frame& frame : {test::gradient_frame(33, 17), test::alpha_gradient_frame(20, 20),
                                 test::noise_frame(9, 40, 5), test::checker_frame(16, 16, 3)}) {
    CHECK(is_positive_infinity(test::psnr(frame, frame)));
    CHECK(test::ssim(frame, frame) == 1.0);
    CHECK(test::max_abs_diff(frame, frame) == 0.0);
    CHECK(test::pixels_equal(frame, frame));
    CHECK(test::same_size(frame, frame));
  }
}

TEST_CASE("support: image metrics are alpha-aware") {
  // Same alpha everywhere; colours differ only where alpha is 0.
  ir::Frame a = test::alpha_gradient_frame(16, 16);
  for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
    if (i % 12 == 0) a.rgba[i + 3] = 0;
  }
  ir::Frame b = a;
  std::size_t changed = 0;
  for (std::size_t i = 0; i < b.rgba.size(); i += 4) {
    if (b.rgba[i + 3] != 0) continue;
    b.rgba[i] = static_cast<std::uint8_t>(255 - b.rgba[i]);
    b.rgba[i + 1] = 17;
    b.rgba[i + 2] = static_cast<std::uint8_t>(b.rgba[i + 2] ^ 0x55);
    ++changed;
  }
  REQUIRE(changed > 10);
  CHECK(a.rgba != b.rgba);
  CHECK(is_positive_infinity(test::psnr(a, b)));
  CHECK(test::max_abs_diff(a, b) == 0.0);
  CHECK(test::pixels_equal(a, b));
  CHECK(test::ssim(a, b) == 1.0);

  // Under partial transparency a colour difference counts in proportion to alpha.
  const ir::Frame half = ir::Frame::filled(8, 8, {100, 100, 100, 128});
  const ir::Frame half_off = with_channel_offset(half, 0, 2);
  CHECK(near(test::max_abs_diff(half, half_off), 2.0 * 128.0 / 255.0, 1e-12));
  CHECK_FALSE(test::pixels_equal(half, half_off));
  const double mse = (2.0 * 128.0 / 255.0) * (2.0 * 128.0 / 255.0) / 4.0;
  CHECK(near(test::psnr(half, half_off), 10.0 * std::log10(255.0 * 255.0 / mse), 1e-9));

  // An alpha difference always counts, even on black where premultiplied colour is 0 either way.
  const ir::Frame opaque_black = ir::Frame::filled(16, 16, {0, 0, 0, 255});
  ir::Frame holes = opaque_black;
  for (std::size_t i = 0; i < holes.rgba.size(); i += 4) {
    if ((i / 4) % 3 == 0) holes.rgba[i + 3] = 0;
  }
  CHECK_FALSE(test::pixels_equal(opaque_black, holes));
  CHECK(test::max_abs_diff(opaque_black, holes) == 255.0);
  CHECK(test::psnr(opaque_black, holes) < 15.0);
  CHECK(test::ssim(opaque_black, holes) < 0.5);
}

TEST_CASE("support: image metrics reject mismatched and invalid frames") {
  const ir::Frame a = test::gradient_frame(8, 8);
  const ir::Frame wider = test::gradient_frame(9, 8);
  const ir::Frame transposed = test::gradient_frame(4, 16);  // same pixel count
  CHECK(test::same_size(a, a));
  CHECK_FALSE(test::same_size(a, wider));
  CHECK_FALSE(test::same_size(a, transposed));
  for (const ir::Frame* other : {&wider, &transposed}) {
    CHECK(std::isnan(test::psnr(a, *other)));
    CHECK(std::isnan(test::ssim(a, *other)));
    CHECK(std::isnan(test::max_abs_diff(a, *other)));
    CHECK_FALSE(test::pixels_equal(a, *other));
  }
  // NaN fails a threshold assertion written the natural way.
  CHECK_FALSE(test::psnr(a, wider) >= 0.0);

  const ir::Frame empty;
  CHECK(std::isnan(test::psnr(empty, empty)));
  CHECK(std::isnan(test::ssim(empty, empty)));
  CHECK_FALSE(test::pixels_equal(empty, empty));
  ir::Frame truncated = a;
  truncated.rgba.pop_back();
  CHECK(std::isnan(test::psnr(a, truncated)));
  CHECK(std::isnan(test::max_abs_diff(truncated, a)));
  CHECK_FALSE(test::pixels_equal(truncated, truncated));
}

TEST_CASE("support: SSIM falls as noise grows and is low for unrelated images") {
  const ir::Frame base = test::gradient_frame(64, 48);
  double previous = 1.0;
  for (const int amplitude : {2, 6, 16, 40, 100}) {
    const double value = test::ssim(base, with_noise(base, amplitude, 11));
    CAPTURE(amplitude);
    CAPTURE(value);
    CHECK(value < previous);
    CHECK(value > 0.0);
    previous = value;
  }
  CHECK(test::ssim(base, with_noise(base, 2, 11)) > 0.9);
  CHECK(test::ssim(base, with_noise(base, 100, 11)) < 0.5);

  // PSNR falls the same way.
  CHECK(test::psnr(base, with_noise(base, 2, 11)) > test::psnr(base, with_noise(base, 16, 11)));

  const double unrelated = test::ssim(test::noise_frame(64, 48, 1), test::noise_frame(64, 48, 2));
  CAPTURE(unrelated);
  CHECK(unrelated < 0.5);
  CHECK(unrelated > -0.5);
  CHECK(test::ssim(test::noise_frame(64, 48, 1), base) < 0.5);

  // A uniform brightness shift barely changes the structure.
  const ir::Frame grey = ir::Frame::filled(32, 32, {120, 120, 120, 255});
  const ir::Frame brighter = ir::Frame::filled(32, 32, {124, 124, 124, 255});
  CHECK(test::ssim(grey, brighter) > 0.99);
  CHECK(test::ssim(grey, brighter) < 1.0);
  CHECK(test::ssim(grey, brighter) == test::ssim(brighter, grey));
}

TEST_CASE("support: SSIM covers every pixel of small and odd-sized images") {
  // Smaller than one 8x8 window in one or both dimensions.
  for (const auto& [width, height] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
           {1, 1}, {3, 5}, {7, 7}, {2, 40}, {40, 2}, {8, 8}, {13, 9}, {21, 30}}) {
    CAPTURE(width);
    CAPTURE(height);
    const ir::Frame base = test::gradient_frame(width, height);
    CHECK(test::ssim(base, base) == 1.0);
    // Change only the bottom-right pixel: the last window must see it.
    ir::Frame changed = base;
    changed.rgba[changed.offset(width - 1, height - 1) + 1] ^= 0xFF;
    const double value = test::ssim(base, changed);
    CHECK(value < 1.0);
    CHECK(std::isfinite(value));
  }
}

TEST_CASE("support: image overloads report the worst frame") {
  ir::Image a;
  a.frames = {ir::Frame::filled(16, 16, {10, 20, 30, 255}), ir::Frame::filled(16, 16, {100, 110, 120, 255}),
              ir::Frame::filled(16, 16, {200, 210, 220, 255})};
  ir::Image b = a;
  CHECK(is_positive_infinity(test::psnr(a, b)));
  CHECK(test::ssim(a, b) == 1.0);
  CHECK(test::max_abs_diff(a, b) == 0.0);
  CHECK(test::pixels_equal(a, b));

  b.frames[1] = with_channel_offset(b.frames[1], 0, 1);
  b.frames[2] = with_channel_offset(b.frames[2], 0, 3);
  CHECK(test::psnr(a, b) == test::psnr(a.frames[2], b.frames[2]));
  CHECK(test::psnr(a, b) < test::psnr(a.frames[1], b.frames[1]));
  CHECK(test::ssim(a, b) == test::ssim(a.frames[2], b.frames[2]));
  CHECK(test::max_abs_diff(a, b) == 3.0);
  CHECK_FALSE(test::pixels_equal(a, b));

  // Frame delays and metadata are not part of the comparison.
  ir::Image c = a;
  c.frames[0].delay_ms = 100;
  c.metadata["Title"] = "x";
  CHECK(test::pixels_equal(a, c));

  // Different frame counts, no frames, or a mismatched frame: not comparable.
  ir::Image shorter = a;
  shorter.frames.pop_back();
  CHECK(std::isnan(test::psnr(a, shorter)));
  CHECK(std::isnan(test::ssim(a, shorter)));
  CHECK(std::isnan(test::max_abs_diff(a, shorter)));
  CHECK_FALSE(test::pixels_equal(a, shorter));
  const ir::Image none;
  CHECK(std::isnan(test::psnr(none, none)));
  CHECK_FALSE(test::pixels_equal(none, none));
  ir::Image resized = a;
  resized.frames[1] = test::gradient_frame(8, 8);
  CHECK(std::isnan(test::psnr(a, resized)));
  CHECK(std::isnan(test::max_abs_diff(a, resized)));
  CHECK_FALSE(test::pixels_equal(a, resized));
}

TEST_CASE("support: image generators are deterministic") {
  const ir::Frame gradient = test::gradient_frame(16, 8);
  REQUIRE(gradient.valid());
  CHECK(gradient.width == 16);
  CHECK(gradient.height == 8);
  CHECK(gradient.rgba == test::gradient_frame(16, 8).rgba);
  CHECK_FALSE(gradient.has_transparency());
  CHECK(pixel(gradient, 0, 0) == "0 0 255 255");
  CHECK(pixel(gradient, 15, 0) == "255 0 82 255");
  CHECK(pixel(gradient, 0, 7) == "0 255 174 255");
  CHECK(pixel(gradient, 15, 7) == "255 255 0 255");
  CHECK(pixel(gradient, 5, 3) == "85 109 163 255");
  CHECK(pixel(test::gradient_frame(1, 1), 0, 0) == "0 0 255 255");
  CHECK(test::gradient_frame(1, 5).valid());
  CHECK(test::gradient_frame(5, 1).valid());

  const ir::Frame checker = test::checker_frame(8, 6, 2);
  REQUIRE(checker.valid());
  CHECK(checker.rgba == test::checker_frame(8, 6, 2).rgba);
  CHECK(pixel(checker, 0, 0) == "0 0 0 255");
  CHECK(pixel(checker, 1, 1) == "0 0 0 255");
  CHECK(pixel(checker, 2, 0) == "255 255 255 255");
  CHECK(pixel(checker, 0, 2) == "255 255 255 255");
  CHECK(pixel(checker, 2, 2) == "0 0 0 255");
  CHECK(pixel(checker, 7, 5) == "255 255 255 255");
  CHECK(test::checker_frame(4, 4, 0).rgba == test::checker_frame(4, 4, 1).rgba);

  const ir::Frame noise = test::noise_frame(4, 4, 7);
  REQUIRE(noise.valid());
  CHECK(noise.rgba == test::noise_frame(4, 4, 7).rgba);
  CHECK(noise.rgba != test::noise_frame(4, 4, 8).rgba);
  CHECK_FALSE(noise.has_transparency());
  CHECK(pixel(noise, 0, 0) == "230 81 207 255");
  CHECK(pixel(noise, 1, 0) == "172 251 39 255");
  CHECK(pixel(noise, 3, 0) == "66 68 58 255");

  const ir::Frame alpha = test::alpha_gradient_frame(16, 8);
  REQUIRE(alpha.valid());
  CHECK(alpha.rgba == test::alpha_gradient_frame(16, 8).rgba);
  CHECK(alpha.has_transparency());
  CHECK(pixel(alpha, 0, 0) == "0 0 255 0");
  CHECK(pixel(alpha, 15, 7) == "255 255 0 255");
  CHECK(pixel(alpha, 5, 3) == "85 109 163 92");
  CHECK(pixel(test::alpha_gradient_frame(1, 1), 0, 0) == "0 0 255 255");
  // Apart from alpha it is the plain gradient.
  ir::Frame opaque = alpha;
  for (std::size_t i = 3; i < opaque.rgba.size(); i += 4) opaque.rgba[i] = 255;
  CHECK(opaque.rgba == gradient.rgba);
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

TEST_CASE("support: audio SNR and RMS error have the documented known answers") {
  const ir::Audio reference = test::sine(48000, 2, 440.0, 1.0);
  REQUIRE(reference.valid());
  REQUIRE(reference.frame_count() == 48000);

  CHECK(is_positive_infinity(test::snr_db(reference, reference)));
  CHECK(test::rms_error(reference, reference) == 0.0);

  // Scaling by 0.9 leaves an error of amplitude 0.1 relative to the signal: 20 dB.
  ir::Audio scaled = reference;
  for (float& sample : scaled.samples) sample = static_cast<float>(static_cast<double>(sample) * 0.9);
  const double snr = test::snr_db(reference, scaled);
  CAPTURE(snr);
  CHECK(near(snr, 20.0, 1e-6));
  // The error is a sine of amplitude 0.05: RMS 0.05 / sqrt(2).
  CHECK(near(test::rms_error(reference, scaled), 0.05 / std::sqrt(2.0), 1e-7));
  CHECK(test::rms_error(reference, scaled) == test::rms_error(scaled, reference));
  // Scaling by 0.99: 40 dB.
  ir::Audio closer = reference;
  for (float& sample : closer.samples) sample = static_cast<float>(static_cast<double>(sample) * 0.99);
  CHECK(near(test::snr_db(reference, closer), 40.0, 1e-4));

  // Silence against a signal: the error is the signal, 0 dB; the other way round -infinity.
  ir::Audio silence = reference;
  std::fill(silence.samples.begin(), silence.samples.end(), 0.0f);
  CHECK(near(test::snr_db(reference, silence), 0.0, 1e-12));
  CHECK(std::isinf(test::snr_db(silence, reference)));
  CHECK(test::snr_db(silence, reference) < 0.0);
  CHECK(is_positive_infinity(test::snr_db(silence, silence)));
  CHECK(near(test::rms_error(reference, silence), 0.5 / std::sqrt(2.0), 1e-7));
}

TEST_CASE("support: unaligned audio metrics count missing audio as error") {
  const ir::Audio reference = test::sine(48000, 1, 440.0, 1.0);
  // Half the audio is missing (220 whole periods remain): the error holds half the energy.
  ir::Audio half = reference;
  half.samples.resize(half.samples.size() / 2);
  CHECK(near(test::snr_db(reference, half), 10.0 * std::log10(2.0), 1e-6));
  CHECK(near(test::rms_error(reference, half), 0.5 / 2.0, 1e-7));
  CHECK(near(test::rms_error(half, reference), 0.5 / 2.0, 1e-7));
  CHECK(near(test::correlation(reference, half), std::sqrt(0.5), 1e-7));

  // Mismatched formats are not comparable.
  const ir::Audio other_rate = test::sine(44100, 1, 440.0, 1.0);
  const ir::Audio stereo = test::sine(48000, 2, 440.0, 1.0);
  for (const ir::Audio* other : {&other_rate, &stereo}) {
    CHECK(std::isnan(test::rms_error(reference, *other)));
    CHECK(std::isnan(test::snr_db(reference, *other)));
    CHECK(std::isnan(test::correlation(reference, *other)));
    CHECK(std::isnan(test::aligned_snr_db(reference, *other, 64)));
    CHECK(test::best_alignment(reference, *other, 64) == 0);
  }
  const ir::Audio invalid;
  CHECK(std::isnan(test::snr_db(invalid, invalid)));
  CHECK(std::isnan(test::rms_error(invalid, invalid)));
  CHECK(test::best_alignment(invalid, invalid, 8) == 0);

  // Valid but empty signals are trivially identical.
  ir::Audio empty;
  empty.sample_rate = 48000;
  empty.channels = 1;
  CHECK(test::rms_error(empty, empty) == 0.0);
  CHECK(is_positive_infinity(test::snr_db(empty, empty)));
  CHECK(test::correlation(empty, empty) == 1.0);
  CHECK(test::best_alignment(empty, reference, 8) == 0);
  CHECK(std::isnan(test::aligned_snr_db(empty, reference, 8)));
}

TEST_CASE("support: audio correlation") {
  const ir::Audio tone = test::sine(44100, 2, 1000.0, 0.5);
  CHECK(near(test::correlation(tone, tone), 1.0, 1e-12));
  ir::Audio inverted = tone;
  for (float& sample : inverted.samples) sample = -sample;
  CHECK(near(test::correlation(tone, inverted), -1.0, 1e-12));
  // Gain does not matter.
  ir::Audio quiet = tone;
  for (float& sample : quiet.samples) sample *= 0.25f;
  CHECK(near(test::correlation(tone, quiet), 1.0, 1e-12));
  // Independent noise is uncorrelated.
  const double unrelated = test::correlation(test::white_noise(44100, 2, 0.5, 1), test::white_noise(44100, 2, 0.5, 2));
  CAPTURE(unrelated);
  CHECK(std::abs(unrelated) < 0.05);
  // Silence: defined, not NaN.
  ir::Audio silence = tone;
  std::fill(silence.samples.begin(), silence.samples.end(), 0.0f);
  CHECK(test::correlation(silence, silence) == 1.0);
  CHECK(test::correlation(tone, silence) == 0.0);
  CHECK(test::correlation(silence, tone) == 0.0);
}

TEST_CASE("support: best_alignment recovers known delays") {
  const ir::Audio noise = test::white_noise(44100, 2, 0.2, 42);
  const ir::Audio chirp = test::sweep(48000, 1, 100.0, 8000.0, 0.2);
  for (const ir::Audio* reference : {&noise, &chirp}) {
    for (const std::ptrdiff_t delay : {std::ptrdiff_t{37}, std::ptrdiff_t{-12}, std::ptrdiff_t{0}, std::ptrdiff_t{1},
                                       std::ptrdiff_t{-1}, std::ptrdiff_t{2112}, std::ptrdiff_t{-1105}}) {
      CAPTURE(delay);
      const ir::Audio test_signal = shifted(*reference, delay);
      CHECK(test::best_alignment(*reference, test_signal, 4096) == delay);
      // Swapping the roles flips the sign.
      CHECK(test::best_alignment(test_signal, *reference, 4096) == -delay);
      // After alignment the overlapping samples are identical.
      CHECK(is_positive_infinity(test::aligned_snr_db(*reference, test_signal, 4096)));
      if (delay != 0) {
        // Without alignment the comparison is useless.
        CHECK(test::snr_db(*reference, test_signal) < 10.0);
      }
    }
  }
  // The search range is honoured: the true delay 37 lies outside +-8.
  const std::ptrdiff_t limited = test::best_alignment(noise, shifted(noise, 37), 8);
  CHECK(limited >= -8);
  CHECK(limited <= 8);
  CHECK(test::best_alignment(noise, shifted(noise, 37), 37) == 37);
  CHECK(test::best_alignment(noise, noise, 0) == 0);
  // A huge search range is clamped to shifts that keep half of the signal overlapping.
  const ir::Audio brief = test::white_noise(8000, 1, 0.05, 9);
  CHECK(test::best_alignment(brief, shifted(brief, 5), 1'000'000) == 5);
}

TEST_CASE("support: aligned SNR measures quality behind a delay") {
  const ir::Audio reference = test::sweep(44100, 2, 200.0, 6000.0, 0.5);
  // Delay by 37 frames, pad the end, and scale by 0.9: 20 dB once aligned.
  ir::Audio degraded = shifted(reference, 37);
  degraded.samples.resize(degraded.samples.size() + 2 * 500, 0.0f);
  for (float& sample : degraded.samples) sample = static_cast<float>(static_cast<double>(sample) * 0.9);
  CHECK(test::best_alignment(reference, degraded, 1024) == 37);
  const double aligned = test::aligned_snr_db(reference, degraded, 1024);
  CAPTURE(aligned);
  CHECK(near(aligned, 20.0, 1e-5));
  CHECK(test::snr_db(reference, degraded) < 10.0);

  // An early (truncated at the start) copy with a little added noise.
  ir::Audio early = shifted(reference, -12);
  const ir::Audio hiss = test::white_noise(44100, 2, 0.5, 3, 0.0005);
  for (std::size_t i = 0; i < early.samples.size(); ++i) early.samples[i] += hiss.samples[i];
  CHECK(test::best_alignment(reference, early, 1024) == -12);
  const double noisy = test::aligned_snr_db(reference, early, 1024);
  CAPTURE(noisy);
  CHECK(noisy > 55.0);
  CHECK(noisy < 70.0);
  CHECK(test::aligned_snr_db(reference, reference, 1024) == test::snr_db(reference, reference));
}

TEST_CASE("support: audio generators are deterministic") {
  // 12 kHz at 48 kHz: the samples are 0.5 * sin(n * pi / 2) = 0, 0.5, 0, -0.5, ...
  const ir::Audio tone = test::sine(48000, 2, 12000.0, 0.01);
  REQUIRE(tone.valid());
  CHECK(tone.sample_rate == 48000);
  CHECK(tone.channels == 2);
  CHECK(tone.frame_count() == 480);
  CHECK(tone.samples == test::sine(48000, 2, 12000.0, 0.01).samples);
  const double expected[] = {0.0, 0.5, 0.0, -0.5, 0.0, 0.5};
  for (std::size_t frame = 0; frame < 6; ++frame) {
    CHECK(near(tone.samples[frame * 2], expected[frame], 1e-6));
    CHECK(tone.samples[frame * 2] == tone.samples[frame * 2 + 1]);
  }
  CHECK(near(test::sine(8000, 1, 1000.0, 1.0, 0.25).samples[2], 0.25, 1e-6));  // sin(pi / 2)
  CHECK(test::sine(44100, 1, 440.0, 0.0).samples.empty());
  CHECK(test::sine(44100, 1, 440.0, 0.0).valid());
  CHECK(test::sine(44100, 3, 440.0, 0.1).frame_count() == 4410);

  const ir::Audio chirp = test::sweep(48000, 2, 100.0, 4000.0, 0.25);
  REQUIRE(chirp.valid());
  CHECK(chirp.frame_count() == 12000);
  CHECK(chirp.samples == test::sweep(48000, 2, 100.0, 4000.0, 0.25).samples);
  CHECK(chirp.samples[0] == 0.0f);
  // Frame 1: phase 2 pi (f0 t + (f1 - f0) t^2 / (2 T)).
  const double t = 1.0 / 48000.0;
  CHECK(near(chirp.samples[2], 0.5 * std::sin(2.0 * pi * (100.0 * t + 3900.0 * t * t / 0.5)), 1e-7));
  float chirp_peak = 0.0f;
  for (const float sample : chirp.samples) chirp_peak = std::max(chirp_peak, std::abs(sample));
  CHECK(chirp_peak <= 0.5f);
  CHECK(chirp_peak > 0.49f);
  // A constant-frequency "sweep" is the sine.
  const ir::Audio flat = test::sweep(48000, 1, 1000.0, 1000.0, 0.1);
  const ir::Audio pure = test::sine(48000, 1, 1000.0, 0.1);
  REQUIRE(flat.samples.size() == pure.samples.size());
  for (std::size_t i = 0; i < flat.samples.size(); i += 97) CHECK(near(flat.samples[i], pure.samples[i], 1e-6));

  const ir::Audio noise = test::white_noise(8000, 2, 0.5, 3);
  REQUIRE(noise.valid());
  CHECK(noise.frame_count() == 4000);
  CHECK(noise.samples == test::white_noise(8000, 2, 0.5, 3).samples);
  CHECK(noise.samples != test::white_noise(8000, 2, 0.5, 4).samples);
  // Exact on every platform: 16-bit values scaled by a power of two.
  CHECK(noise.samples[0] == 0.3279876708984375f);
  CHECK(noise.samples[1] == -0.2479705810546875f);
  CHECK(noise.samples[2] == -0.0131683349609375f);
  CHECK(noise.samples[3] == 0.344757080078125f);
  double sum = 0.0, energy = 0.0;
  float lowest = 0.0f, highest = 0.0f;
  for (const float sample : noise.samples) {
    lowest = std::min(lowest, sample);
    highest = std::max(highest, sample);
    sum += sample;
    energy += static_cast<double>(sample) * sample;
  }
  CHECK(lowest >= -0.5f);
  CHECK(lowest < -0.49f);
  CHECK(highest < 0.5f);
  CHECK(highest > 0.49f);
  // Uniform in [-0.5, 0.5): mean 0, variance 1/12.
  CHECK(std::abs(sum / static_cast<double>(noise.samples.size())) < 0.01);
  CHECK(near(energy / static_cast<double>(noise.samples.size()), 1.0 / 12.0, 0.005));
  CHECK(near(test::white_noise(8000, 1, 0.01, 3, 1.0).samples[0], 2.0 * 0.3279876708984375, 1e-7));
}

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

TEST_CASE("support: video PSNR for 8-bit and 10-bit frames with a uniform difference") {
  const double off_by_one_8 = 20.0 * std::log10(255.0);
  const double off_by_one_10 = 20.0 * std::log10(1023.0);
  CHECK(near(off_by_one_8, 48.1308036086791, 1e-9));
  CHECK(near(off_by_one_10, 60.1975126742432, 1e-9));

  for (const ir::PixelFormat format :
       {ir::PixelFormat::I420, ir::PixelFormat::I422, ir::PixelFormat::I444, ir::PixelFormat::NV12}) {
    CAPTURE(static_cast<int>(format));
    const ir::VideoFrame frame = test::test_pattern_frame(format, 33, 19, 2);
    REQUIRE(frame.valid());
    CHECK(is_positive_infinity(test::psnr(frame, frame)));
    CHECK(near(test::psnr(frame, with_sample_offset(frame, 1)), off_by_one_8, 1e-9));
    CHECK(near(test::psnr(with_sample_offset(frame, -1), frame), off_by_one_8, 1e-9));
    CHECK(near(test::psnr(frame, with_sample_offset(frame, 4)), off_by_one_8 - 20.0 * std::log10(4.0), 1e-9));
  }
  for (const ir::PixelFormat format :
       {ir::PixelFormat::I420P10, ir::PixelFormat::I422P10, ir::PixelFormat::I444P10}) {
    CAPTURE(static_cast<int>(format));
    const ir::VideoFrame frame = test::test_pattern_frame(format, 33, 19, 2);
    REQUIRE(frame.valid());
    CHECK(is_positive_infinity(test::psnr(frame, frame)));
    CHECK(near(test::psnr(frame, with_sample_offset(frame, 1)), off_by_one_10, 1e-9));
    CHECK(near(test::psnr(frame, with_sample_offset(frame, -4)), off_by_one_10 - 20.0 * std::log10(4.0), 1e-9));
  }
  // P010 keeps its 10 bits in the top of each word: one step is 64, and the
  // six padding bits do not count.
  const ir::VideoFrame p010 = test::test_pattern_frame(ir::PixelFormat::P010, 33, 19, 2);
  REQUIRE(p010.valid());
  CHECK(near(test::psnr(p010, with_sample_offset(p010, 64)), off_by_one_10, 1e-9));
  CHECK(near(test::psnr(p010, with_sample_offset(p010, -128)), off_by_one_10 - 20.0 * std::log10(2.0), 1e-9));
  CHECK(is_positive_infinity(test::psnr(p010, with_sample_offset(p010, 63))));

  // Only one plane differs: the error is pooled over all samples. A 32x16
  // I420 frame has 512 luma and 2 * 128 chroma samples.
  const ir::VideoFrame base = test::test_pattern_frame(ir::PixelFormat::I420, 32, 16, 0);
  ir::VideoFrame chroma_off = base;
  for (std::uint8_t& value : chroma_off.planes[1].data) value = static_cast<std::uint8_t>(value + 1);
  CHECK(near(test::psnr(base, chroma_off), 10.0 * std::log10(255.0 * 255.0 * 768.0 / 128.0), 1e-9));
}

TEST_CASE("support: video PSNR handles RGBA, strides and mismatches") {
  // RGBA8 frames are scored exactly like images (alpha-aware).
  ir::Frame image_a = test::alpha_gradient_frame(20, 10);
  ir::Frame image_b = with_noise(image_a, 9, 4);
  const auto as_video = [](const ir::Frame& image) {
    ir::VideoFrame frame = ir::VideoFrame::allocate(ir::PixelFormat::RGBA8, image.width, image.height);
    frame.planes[0].data = image.rgba;
    return frame;
  };
  const double as_image = test::psnr(image_a, image_b);
  CHECK(std::isfinite(as_image));
  CHECK(test::psnr(as_video(image_a), as_video(image_b)) == as_image);
  const ir::VideoFrame rgba = test::test_pattern_frame(ir::PixelFormat::RGBA8, 16, 16, 0);
  CHECK(is_positive_infinity(test::psnr(rgba, rgba)));
  ir::VideoFrame rgba_off = rgba;
  for (std::size_t i = 0; i < rgba_off.planes[0].data.size(); i += 4) rgba_off.planes[0].data[i] ^= 1;
  CHECK(near(test::psnr(rgba, rgba_off), 10.0 * std::log10(255.0 * 255.0 * 4.0), 1e-9));

  // Row padding is never compared, whatever it contains.
  for (const ir::PixelFormat format : {ir::PixelFormat::RGBA8, ir::PixelFormat::I420, ir::PixelFormat::NV12,
                                       ir::PixelFormat::I444P10, ir::PixelFormat::P010}) {
    CAPTURE(static_cast<int>(format));
    const ir::VideoFrame frame = test::test_pattern_frame(format, 21, 11, 5);
    const ir::VideoFrame padded = with_padded_strides(frame, 13);
    REQUIRE(padded.valid());
    CHECK(is_positive_infinity(test::psnr(frame, padded)));
    CHECK(is_positive_infinity(test::psnr(padded, frame)));
    const ir::VideoFrame next = test::test_pattern_frame(format, 21, 11, 6);
    CHECK(test::psnr(frame, next) == test::psnr(padded, with_padded_strides(next, 3)));
    CHECK(std::isfinite(test::psnr(frame, next)));
  }

  // Different format or size, or an invalid frame: NaN.
  const ir::VideoFrame i420 = test::test_pattern_frame(ir::PixelFormat::I420, 16, 16, 0);
  CHECK(std::isnan(test::psnr(i420, test::test_pattern_frame(ir::PixelFormat::NV12, 16, 16, 0))));
  CHECK(std::isnan(test::psnr(i420, test::test_pattern_frame(ir::PixelFormat::I420, 16, 18, 0))));
  CHECK(std::isnan(test::psnr(i420, test::test_pattern_frame(ir::PixelFormat::I420, 18, 16, 0))));
  ir::VideoFrame broken = i420;
  broken.planes[2].data.pop_back();
  CHECK(std::isnan(test::psnr(i420, broken)));
  CHECK(std::isnan(test::psnr(broken, broken)));
  const ir::VideoFrame empty;
  CHECK(std::isnan(test::psnr(empty, empty)));
  // Colour description and timing are ignored.
  ir::VideoFrame retimed = i420;
  retimed.pts_ms = 999;
  retimed.color.matrix = ir::ColorMatrix::BT601;
  CHECK(is_positive_infinity(test::psnr(i420, retimed)));
}

TEST_CASE("support: per-frame and minimum video PSNR") {
  std::vector<ir::VideoFrame> reference;
  for (std::uint32_t i = 0; i < 4; ++i) reference.push_back(test::test_pattern_frame(ir::PixelFormat::I420, 32, 16, i));
  std::vector<ir::VideoFrame> decoded = reference;
  CHECK(is_positive_infinity(test::min_psnr(reference, decoded)));

  decoded[1] = with_sample_offset(decoded[1], 1);
  decoded[3] = with_sample_offset(decoded[3], 2);
  const std::vector<double> values = test::per_frame_psnr(reference, decoded);
  REQUIRE(values.size() == 4);
  CHECK(is_positive_infinity(values[0]));
  CHECK(near(values[1], 20.0 * std::log10(255.0), 1e-9));
  CHECK(is_positive_infinity(values[2]));
  CHECK(near(values[3], 20.0 * std::log10(255.0 / 2.0), 1e-9));
  CHECK(test::min_psnr(reference, decoded) == values[3]);
  CHECK(test::min_psnr(decoded, reference) == values[3]);

  // A missing frame shows up as NaN at its index and makes the minimum NaN.
  decoded.pop_back();
  const std::vector<double> shorter = test::per_frame_psnr(reference, decoded);
  REQUIRE(shorter.size() == 4);
  CHECK(near(shorter[1], values[1], 0.0));
  CHECK(std::isnan(shorter[3]));
  CHECK(std::isnan(test::min_psnr(reference, decoded)));
  CHECK(test::per_frame_psnr(decoded, reference).size() == 4);
  CHECK(std::isnan(test::min_psnr({}, {})));
  CHECK(test::per_frame_psnr({}, {}).empty());
}

TEST_CASE("support: test_pattern_frame writes valid, moving, deterministic frames") {
  const ir::PixelFormat formats[] = {ir::PixelFormat::RGBA8,   ir::PixelFormat::I420,    ir::PixelFormat::I422,
                                     ir::PixelFormat::I444,    ir::PixelFormat::NV12,    ir::PixelFormat::I420P10,
                                     ir::PixelFormat::I422P10, ir::PixelFormat::I444P10, ir::PixelFormat::P010};
  for (const ir::PixelFormat format : formats) {
    CAPTURE(static_cast<int>(format));
    for (const auto& [width, height] :
         std::vector<std::pair<std::uint32_t, std::uint32_t>>{{1, 1}, {17, 9}, {64, 48}}) {
      const ir::VideoFrame frame = test::test_pattern_frame(format, width, height, 3);
      REQUIRE(frame.valid());
      CHECK(frame.format == format);
      CHECK(frame.width == width);
      CHECK(frame.height == height);
      CHECK(frame.pts_ms == 120);
      CHECK(frame.duration_ms == 40);
      CHECK(frame.color == ir::ColorInfo{});
      // Tightly packed, deterministic.
      const ir::VideoFrame again = test::test_pattern_frame(format, width, height, 3);
      for (std::size_t p = 0; p < frame.planes.size(); ++p) {
        CHECK(frame.planes[p].stride == ir::plane_row_bytes(format, p, width));
        CHECK(frame.planes[p].data.size() == frame.planes[p].stride * ir::plane_height(format, p, height));
        CHECK(frame.planes[p].data == again.planes[p].data);
      }
    }
    // The picture moves: consecutive frames differ, measurably but not wildly.
    const ir::VideoFrame first = test::test_pattern_frame(format, 64, 48, 0);
    const ir::VideoFrame second = test::test_pattern_frame(format, 64, 48, 1);
    CHECK(first.planes[0].data != second.planes[0].data);
    const double motion = test::psnr(first, second);
    CAPTURE(motion);
    CHECK(motion > 20.0);
    CHECK(motion < 60.0);

    if (!ir::is_yuv(format)) continue;
    // Limited range, and for P010 nothing in the padding bits.
    const bool wide = ir::bytes_per_sample(format) == 2;
    const unsigned shift = format == ir::PixelFormat::P010 ? 6u : 0u;
    const int low = wide ? 64 : 16;
    const int luma_high = wide ? 940 : 235;
    const int chroma_high = wide ? 960 : 240;
    const ir::VideoFrame frame = test::test_pattern_frame(format, 600, 300, 1);
    int luma_min = 0xFFFF, luma_max = 0, chroma_min = 0xFFFF, chroma_max = 0;
    for (std::size_t p = 0; p < frame.planes.size(); ++p) {
      const std::size_t samples = ir::plane_row_bytes(format, p, 600) / ir::bytes_per_sample(format);
      for (std::size_t y = 0; y < ir::plane_height(format, p, 300); ++y) {
        for (std::size_t i = 0; i < samples; ++i) {
          const int stored = sample_at(frame, p, i, y);
          if (shift != 0 && (stored & 0x3F) != 0) FAIL("P010 padding bits are set");
          const int value = stored >> shift;
          if (p == 0) {
            luma_min = std::min(luma_min, value);
            luma_max = std::max(luma_max, value);
          } else {
            chroma_min = std::min(chroma_min, value);
            chroma_max = std::max(chroma_max, value);
          }
        }
      }
    }
    // A 600x300 frame spans whole periods of every ramp: the full range is
    // used (subsampled chroma only visits every other step of its ramp, so
    // it may stop one step short of the top).
    CHECK(luma_min == low);
    CHECK(luma_max == luma_high);
    CHECK(chroma_min == low);
    CHECK(chroma_max <= chroma_high);
    CHECK(chroma_max >= chroma_high - (wide ? 4 : 1));
  }

  // Fixed spot values.
  const ir::VideoFrame rgba = test::test_pattern_frame(ir::PixelFormat::RGBA8, 8, 8, 0);
  const std::uint8_t* first_pixels = rgba.planes[0].data.data();
  CHECK(first_pixels[0] == 0);
  CHECK(first_pixels[1] == 170);
  CHECK(first_pixels[2] == 170);
  CHECK(first_pixels[3] == 255);
  CHECK(first_pixels[4] == 2);
  CHECK(first_pixels[5] == 171);
  CHECK(first_pixels[6] == 167);
  CHECK(first_pixels[7] == 255);
  const ir::VideoFrame i420 = test::test_pattern_frame(ir::PixelFormat::I420, 8, 8, 0);
  CHECK(sample_at(i420, 0, 0, 0) == 16);
  CHECK(sample_at(i420, 0, 1, 0) == 18);   // 16 + round(2 * 219 / 255)
  CHECK(sample_at(i420, 0, 0, 1) == 17);   // 16 + round(1 * 219 / 255)
  CHECK(sample_at(i420, 1, 0, 0) == 16);
  CHECK(sample_at(i420, 1, 3, 0) == 21);   // luma x = 6: 16 + round(6 * 224 / 255)
  CHECK(sample_at(i420, 2, 0, 0) == 240);  // triangle(255) = 255
  CHECK(sample_at(i420, 2, 0, 2) == 236);  // luma y = 4: triangle(259) = 251
  const ir::VideoFrame i420_later = test::test_pattern_frame(ir::PixelFormat::I420, 8, 8, 10);
  CHECK(sample_at(i420_later, 0, 0, 0) == 50);  // 16 + round(40 * 219 / 255)
  const ir::VideoFrame p10 = test::test_pattern_frame(ir::PixelFormat::I420P10, 8, 8, 0);
  CHECK(sample_at(p10, 0, 0, 0) == 64);
  CHECK(sample_at(p10, 0, 1, 0) == 71);  // 64 + round(2 * 876 / 255)
  CHECK(sample_at(p10, 2, 0, 0) == 960);
  const ir::VideoFrame p010 = test::test_pattern_frame(ir::PixelFormat::P010, 8, 8, 0);
  CHECK(sample_at(p010, 0, 1, 0) == 71 << 6);
  CHECK(sample_at(p010, 1, 0, 0) == 64 << 6);   // Cb of the first chroma sample
  CHECK(sample_at(p010, 1, 1, 0) == 960 << 6);  // Cr of the first chroma sample
  const ir::VideoFrame nv12 = test::test_pattern_frame(ir::PixelFormat::NV12, 8, 8, 0);
  CHECK(sample_at(nv12, 1, 6, 0) == sample_at(i420, 1, 3, 0));  // Cb, interleaved
  CHECK(sample_at(nv12, 1, 1, 2) == sample_at(i420, 2, 0, 2));  // Cr, interleaved
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

TEST_CASE("support: normalized_text collapses white space") {
  CHECK(test::normalized_text("") == "");
  CHECK(test::normalized_text(" \t\r\n ") == "");
  CHECK(test::normalized_text("plain") == "plain");
  CHECK(test::normalized_text("  two   words  ") == "two words");
  CHECK(test::normalized_text("line one\r\nline two\rline three\n") == "line one line two line three");
  CHECK(test::normalized_text("a\t\tb\n\n\nc") == "a b c");
  // Unicode spaces (NBSP, EM SPACE, IDEOGRAPHIC SPACE, LINE SEPARATOR) count as white space.
  CHECK(test::normalized_text("a\xC2\xA0" "b\xE2\x80\x83" "c\xE3\x80\x80" "d\xE2\x80\xA8" "e") == "a b c d e");
  // A leading byte order mark is dropped; other characters are untouched.
  CHECK(test::normalized_text("\xEF\xBB\xBF" "caf\xC3\xA9 ") == "caf\xC3\xA9");
  CHECK(test::normalized_text("Mixed CASE stays") == "Mixed CASE stays");
  // Malformed UTF-8 becomes U+FFFD instead of being dropped.
  CHECK(test::normalized_text("a\xFF" "b") == "a\xEF\xBF\xBD" "b");
}

TEST_CASE("support: text similarity counts code-point edits") {
  CHECK(test::text_similarity("", "") == 1.0);
  CHECK(test::text_similarity("  \n", "\t") == 1.0);
  CHECK(test::text_similarity("same text", "same text") == 1.0);
  CHECK(test::text_similarity("abc", "") == 0.0);
  CHECK(test::text_similarity("", "abc") == 0.0);
  CHECK(test::text_similarity("abc", "xyz") == 0.0);

  // White-space and line-ending differences do not count.
  CHECK(test::text_similarity("one two\r\nthree\r\n", "one  two\nthree") == 1.0);
  CHECK(test::text_similarity("  one\ttwo three  ", "one two\n\nthree") == 1.0);
  CHECK(test::text_similarity("\xEF\xBB\xBFone two", "one two") == 1.0);
  // But joining or splitting words does.
  CHECK(test::text_similarity("onetwo", "one two") < 1.0);

  // One substitution in a 10-code-point string.
  CHECK(near(test::text_similarity("0123456789", "01234x6789"), 0.9, 1e-12));
  // One insertion: distance 1 over the longer length 11.
  CHECK(near(test::text_similarity("0123456789", "01234x56789"), 1.0 - 1.0 / 11.0, 1e-12));
  // The textbook example: kitten -> sitting needs 3 edits.
  CHECK(near(test::text_similarity("kitten", "sitting"), 1.0 - 3.0 / 7.0, 1e-12));
  CHECK(test::text_similarity("kitten", "sitting") == test::text_similarity("sitting", "kitten"));
  CHECK(near(test::text_similarity("flaw", "lawn"), 0.5, 1e-12));
  // Case matters.
  CHECK(near(test::text_similarity("Hello", "hello"), 0.8, 1e-12));

  // Non-ASCII characters are one unit each, however many bytes they take:
  // the accented "naive cafe" is 10 code points in 12 bytes, the Japanese
  // sample 7 code points in 21 bytes.
  const std::string_view naive = "na\xC3\xAFve caf\xC3\xA9";
  CHECK(near(test::text_similarity(naive, "na\xC3\xAFve cafe"), 0.9, 1e-12));
  CHECK(near(test::text_similarity(naive, "naive cafe"), 0.8, 1e-12));
  const std::string_view japanese = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88";
  const std::string_view japanese_edit =
      "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x8A";
  CHECK(near(test::text_similarity(japanese, japanese_edit), 1.0 - 1.0 / 7.0, 1e-12));
  // A four-byte emoji replaced by an ASCII letter: one edit in 3.
  CHECK(near(test::text_similarity("a\xF0\x9F\x98\x80" "b", "axb"), 1.0 - 1.0 / 3.0, 1e-12));
}

TEST_CASE("support: text similarity of large inputs compares words") {
  // 2000 words of 5 to 8 characters: about 15 000 code points per text, far
  // beyond the exact limit of 25 million cell pairs.
  std::vector<std::string> words;
  for (int i = 0; i < 2000; ++i) words.push_back("word" + std::to_string(i));
  const auto joined = [](const std::vector<std::string>& list, std::string_view separator) {
    std::string out;
    for (const std::string& word : list) {
      out += word;
      out += separator;
    }
    return out;
  };
  const std::string original = joined(words, " ");
  REQUIRE(static_cast<std::uint64_t>(original.size()) * original.size() > test::text_similarity_exact_limit);

  CHECK(test::text_similarity(original, original) == 1.0);
  CHECK(test::text_similarity(original, joined(words, "\r\n")) == 1.0);

  // Change ONE character of one word: the word-level distance is 1 of 2000
  // words, where the code-point path would report 1 of about 15 000.
  std::vector<std::string> edited = words;
  edited[700][0] = 'W';
  CHECK(near(test::text_similarity(original, joined(edited, " ")), 1.0 - 1.0 / 2000.0, 1e-12));
  // Replace 20 words (the one edited above among them), delete 10 and insert
  // 5: 35 edits; the longer text has 2000 words.
  for (int i = 0; i < 20; ++i) edited[static_cast<std::size_t>(100 + i * 50)] = "changed";
  edited.erase(edited.begin() + 1500, edited.begin() + 1510);
  edited.insert(edited.begin() + 300, std::size_t{5}, std::string("inserted"));
  CHECK(near(test::text_similarity(original, joined(edited, " ")), 1.0 - 35.0 / 2000.0, 1e-12));
  CHECK(near(test::text_similarity(joined(edited, "\n"), original), 1.0 - 35.0 / 2000.0, 1e-12));
  // Completely different long texts.
  std::vector<std::string> other;
  for (int i = 0; i < 2000; ++i) other.push_back("other" + std::to_string(i));
  CHECK(test::text_similarity(original, joined(other, " ")) == 0.0);

  // Just below the limit the comparison is still per code point: two
  // 5000-code-point texts with one substitution.
  std::string exact_a(5000, 'a');
  std::string exact_b = exact_a;
  exact_b[2500] = 'b';
  REQUIRE(static_cast<std::uint64_t>(exact_a.size()) * exact_b.size() == test::text_similarity_exact_limit);
  CHECK(near(test::text_similarity(exact_a, exact_b), 1.0 - 1.0 / 5000.0, 1e-12));
  // One more character tips it over: now each text is a single different "word".
  exact_a.push_back('a');
  exact_b.push_back('a');
  CHECK(test::text_similarity(exact_a, exact_b) == 0.0);
}

TEST_CASE("support: document text similarity ignores formatting") {
  using namespace cvt::ir::build;
  ir::Document a;
  a.blocks.push_back(heading(1, "Title"));
  a.blocks.push_back(paragraph({text("Plain, "), strong("bold"), text(" and "), hyperlink("https://example.invalid", "a link")}));
  a.blocks.push_back(list(false, {{paragraph("first")}, {paragraph("second")}}));

  // Same words with different inline structure and heading level.
  ir::Document b;
  b.blocks.push_back(heading(3, {emphasis("Title")}));
  b.blocks.push_back(paragraph("Plain, bold and a link"));
  b.blocks.push_back(list(false, {{paragraph("first")}, {paragraph({text("sec"), code("ond")})}}));
  b.metadata.title = "ignored";
  CHECK(test::text_similarity(a, b) == 1.0);

  // A changed word lowers the score a little; an empty document scores 0.
  ir::Document c = b;
  c.blocks[1] = paragraph("Plain, bald and a link");
  const double changed = test::text_similarity(a, c);
  CHECK(changed < 1.0);
  CHECK(changed > 0.9);
  CHECK(changed == test::text_similarity(std::string_view(ir::plain_text(a)), std::string_view(ir::plain_text(c))));
  CHECK(test::text_similarity(a, ir::Document{}) == 0.0);
  CHECK(test::text_similarity(ir::Document{}, ir::Document{}) == 1.0);
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

TEST_CASE("support: data_equal compares structure, not spelling") {
  // Key order is ignored at every level; array order is not.
  CHECK(test::data_equal(json(R"({"a":1,"b":{"x":[1,2],"y":null}})"), json(R"({"b":{"y":null,"x":[1,2]},"a":1})")));
  CHECK_FALSE(test::data_equal(json("[1,2,3]"), json("[3,2,1]")));
  CHECK(test::data_equal(json("[]"), json("[]")));
  CHECK(test::data_equal(json("{}"), json("{}")));
  CHECK_FALSE(test::data_equal(json("{}"), json("[]")));
  CHECK(test::data_equal(json("null"), json("null")));
  CHECK(test::data_equal(json(R"("text")"), json(R"("text")")));
  CHECK_FALSE(test::data_equal(json(R"("text")"), json(R"("Text")")));
  CHECK_FALSE(test::data_equal(json("true"), json("false")));

  // Numbers compare by value across integer and float representations.
  CHECK(test::data_equal(json("1"), json("1.0")));
  CHECK(test::data_equal(json("[1, -2, 3e2]"), json("[1.0, -2.0, 300]")));
  CHECK(test::data_equal(json("0"), json("-0.0")));
  CHECK_FALSE(test::data_equal(json("1"), json("1.5")));
  CHECK_FALSE(test::data_equal(json("1"), json("2")));
  CHECK_FALSE(test::data_equal(json("-1"), json("18446744073709551615")));
  CHECK(test::data_equal(json("18446744073709551615"), json("18446744073709551615")));
  // 64-bit integers are compared exactly (as doubles these two would be equal).
  CHECK_FALSE(test::data_equal(json("9007199254740993"), json("9007199254740992")));
  CHECK(test::data_equal(json("9007199254740993"), json("9007199254740993")));
  CHECK(test::data_equal(json("9007199254740993"), json("9007199254740992"), 1e-12));

  // No coercion between types.
  CHECK_FALSE(test::data_equal(json("1"), json(R"("1")")));
  CHECK_FALSE(test::data_equal(json("1"), json("true")));
  CHECK_FALSE(test::data_equal(json("0"), json("null")));
  CHECK_FALSE(test::data_equal(json(R"({"a":null})"), json("{}")));
  CHECK_FALSE(test::data_equal(json(R"("")"), json("null")));
}

TEST_CASE("support: data_equal number tolerance is absolute or relative") {
  CHECK_FALSE(test::data_equal(json("1.0"), json("1.0000001")));
  CHECK(test::data_equal(json("1.0"), json("1.0000001"), 1e-6));
  CHECK_FALSE(test::data_equal(json("1.0"), json("1.0000001"), 1e-8));
  // Relative: 1e12 vs 1e12 + 1000 differ by 1e-9 of their size.
  CHECK(test::data_equal(json("1e12"), json("1000000001000"), 1e-8));
  CHECK_FALSE(test::data_equal(json("1e12"), json("1000000001000"), 1e-10));
  // Absolute: tiny values near zero.
  CHECK(test::data_equal(json("0"), json("1e-9"), 1e-6));
  CHECK_FALSE(test::data_equal(json("0"), json("1e-3"), 1e-6));
  // Integers obey the tolerance too, and it applies at any depth.
  CHECK(test::data_equal(json("100"), json("101"), 0.02));
  CHECK(test::data_equal(json(R"({"p":[{"v":0.1}]})"), json(R"({"p":[{"v":0.10000000001}]})"), 1e-9));
  CHECK_FALSE(test::data_equal(json(R"({"p":[{"v":0.1}]})"), json(R"({"p":[{"v":0.2}]})"), 1e-9));
  // The tolerance never bridges types or strings.
  CHECK_FALSE(test::data_equal(json(R"("1.0")"), json(R"("1.00")"), 1.0));
  // Non-finite values (not expressible in JSON text, but possible in the IR).
  const Json nan_value = std::numeric_limits<double>::quiet_NaN();
  const Json inf_value = std::numeric_limits<double>::infinity();
  CHECK(test::data_equal(nan_value, nan_value));
  CHECK(test::data_equal(inf_value, inf_value));
  CHECK_FALSE(test::data_equal(inf_value, Json(1e308), 0.5));
  CHECK_FALSE(test::data_equal(nan_value, Json(0.0), 1.0));
}

TEST_CASE("support: data_diff names the first difference") {
  CHECK(test::data_diff(json(R"({"a":[1,{"b":2}]})"), json(R"({"a":[1,{"b":2.0}]})")) == "");

  CHECK(test::data_diff(json(R"({"a":{"b":[0,{"c":2}]}})"), json(R"({"a":{"b":[0,{"c":3}]}})")) == "/a/b/1/c: 2 vs 3");
  CHECK(test::data_diff(json("1"), json("2")) == "(root): 1 vs 2");
  CHECK(test::data_diff(json(R"({"k":"x"})"), json(R"({"k":"y"})")) == R"(/k: "x" vs "y")");
  CHECK(test::data_diff(json(R"({"k":"1"})"), json(R"({"k":1})")) == R"(/k: string "1" vs number 1)");
  CHECK(test::data_diff(json("[1,2]"), json(R"({"a":1})")) == R"((root): array [1,2] vs object {"a":1})");
  CHECK(test::data_diff(json(R"({"k":true})"), json(R"({"k":null})")) == "/k: boolean true vs null null");
  CHECK(test::data_diff(json(R"({"k":[1,2,3]})"), json(R"({"k":[1,2]})")) == "/k: array length 3 vs 2");
  CHECK(test::data_diff(json(R"({"k":[1,2]})"), json(R"({"k":[1,5,3]})")) == "/k/1: 2 vs 5");
  CHECK(test::data_diff(json(R"({"a":1,"gone":2})"), json(R"({"a":1})")) == "/gone: only in the first value");
  CHECK(test::data_diff(json(R"({"a":1})"), json(R"({"new":{"x":1},"a":1})")) == "/new: only in the second value");
  // JSON-pointer escaping of "/" and "~" in keys; the empty key is a valid token.
  CHECK(test::data_diff(json(R"({"a/b":{"m~n":1}})"), json(R"({"a/b":{"m~n":2}})")) == "/a~1b/m~0n: 1 vs 2");
  CHECK(test::data_diff(json(R"({"":1})"), json(R"({"":2})")) == "/: 1 vs 2");
  // The first difference in the first value's key order is reported.
  CHECK(test::data_diff(json(R"({"z":1,"a":1})"), json(R"({"a":2,"z":2})")) == "/z: 1 vs 2");
  // The tolerance is honoured.
  CHECK(test::data_diff(json(R"({"v":1.0})"), json(R"({"v":1.001})"), 0.01) == "");
  CHECK(test::data_diff(json(R"({"v":1.0})"), json(R"({"v":1.5})"), 0.01) == "/v: 1.0 vs 1.5");
  // Long values are abbreviated.
  const std::string difference =
      test::data_diff(Json(std::string(500, 'x')), Json(std::string(500, 'y')));
  CHECK(difference.size() < 160);
  CHECK(difference.starts_with("(root): \"xxxx"));
  CHECK(difference.find("...") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

TEST_CASE("support: tables_equal ignores padding but not content") {
  using ir::Cell;
  ir::Table a;
  a.sheets.push_back({"Prices",
                      {{Cell::string("item"), Cell::string("price"), Cell::string("in stock")},
                       {Cell::string("tea"), Cell::numeric(3.5, "3.50"), Cell::boolean(true)},
                       {Cell::string("milk")},
                       {Cell::string("cake"), Cell{}, Cell::datetime(1714566600.0, "2024-05-01T12:30:00Z")}},
                      true});
  CHECK(test::tables_equal(a, a));
  CHECK(test::table_diff(a, a) == "");
  CHECK(test::tables_equal(ir::Table{}, ir::Table{}));

  // Ragged rows, trailing Empty cells and trailing empty rows are the same grid.
  ir::Table padded = a;
  padded.sheets[0].rows[2].resize(3);                                   // "milk", Empty, Empty
  padded.sheets[0].rows[0].push_back(Cell{});                           // trailing Empty
  padded.sheets[0].rows.push_back({});                                  // empty row
  padded.sheets[0].rows.push_back({Cell{}, Cell{}});                    // row of Empty cells
  CHECK(test::tables_equal(a, padded));
  CHECK(test::tables_equal(padded, a));
  // Display text of non-string cells and the header flag are not compared.
  ir::Table reformatted = a;
  reformatted.sheets[0].rows[1][1].text = "3.5";
  reformatted.sheets[0].rows[1][2].text = "true";
  reformatted.sheets[0].has_header = false;
  CHECK(test::tables_equal(a, reformatted));

  // An Empty cell in the middle is significant: it shifts nothing.
  ir::Table interior = a;
  interior.sheets[0].rows[3] = {Cell::string("cake"), Cell::datetime(1714566600.0, "2024-05-01T12:30:00Z")};
  CHECK_FALSE(test::tables_equal(a, interior));
  CHECK(test::table_diff(a, interior) == R"(sheet 0 ("Prices") row 3 column 1: Empty vs DateTime 1714566600)");
  // An empty row in the middle is significant too.
  ir::Table gap = a;
  gap.sheets[0].rows.insert(gap.sheets[0].rows.begin() + 1, std::vector<Cell>{});
  CHECK_FALSE(test::tables_equal(a, gap));
  CHECK(test::table_diff(a, gap) == R"(sheet 0 ("Prices") row 1 column 0: String "tea" vs Empty)");

  // Cell content.
  ir::Table text_changed = a;
  text_changed.sheets[0].rows[2][0] = Cell::string("Milk");
  CHECK(test::table_diff(a, text_changed) == R"(sheet 0 ("Prices") row 2 column 0: String "milk" vs String "Milk")");
  ir::Table type_changed = a;
  type_changed.sheets[0].rows[1][1] = Cell::string("3.50");
  CHECK(test::table_diff(a, type_changed) == R"(sheet 0 ("Prices") row 1 column 1: Number 3.5 vs String "3.50")");
  ir::Table empty_string = a;
  empty_string.sheets[0].rows[3][1] = Cell::string("");
  CHECK(test::table_diff(a, empty_string) == R"(sheet 0 ("Prices") row 3 column 1: Empty vs String "")");
  ir::Table bool_changed = a;
  bool_changed.sheets[0].rows[1][2] = Cell::boolean(false);
  CHECK(test::table_diff(a, bool_changed) == R"(sheet 0 ("Prices") row 1 column 2: Bool 1 vs Bool 0)");
  ir::Table extra_cell = a;
  extra_cell.sheets[0].rows[2].push_back(Cell::numeric(1.0, "1"));
  CHECK(test::table_diff(a, extra_cell) == R"(sheet 0 ("Prices") row 2 column 1: Empty vs Number 1)");
  ir::Table extra_row = a;
  extra_row.sheets[0].rows.push_back({Cell{}, Cell::string("end")});
  CHECK(test::table_diff(extra_row, a) == R"(sheet 0 ("Prices") row 4 column 1: String "end" vs Empty)");

  // Number tolerance applies to Number, Bool and DateTime values.
  ir::Table rounded = a;
  rounded.sheets[0].rows[1][1].number = 3.5000001;
  rounded.sheets[0].rows[3][2].number = 1714566600.4;
  CHECK_FALSE(test::tables_equal(a, rounded));
  CHECK(test::tables_equal(a, rounded, 1e-6));
  CHECK_FALSE(test::tables_equal(a, rounded, 1e-12));
  CHECK(test::table_diff(a, rounded, 1e-12) == R"(sheet 0 ("Prices") row 1 column 1: Number 3.5 vs Number 3.5000001)");

  // Sheets: count, names and order.
  ir::Table renamed = a;
  renamed.sheets[0].name = "Costs";
  CHECK(test::table_diff(a, renamed) == R"(sheet 0: name "Prices" vs "Costs")");
  ir::Table two = a;
  two.sheets.push_back({"Second", {}, false});
  CHECK(test::table_diff(a, two) == "sheet count 1 vs 2");
  ir::Table two_changed = two;
  two_changed.sheets[1].rows.push_back({Cell::numeric(7.0, "7")});
  CHECK(test::table_diff(two, two_changed) == R"(sheet 1 ("Second") row 0 column 0: Empty vs Number 7)");
  ir::Table swapped = two;
  std::swap(swapped.sheets[0], swapped.sheets[1]);
  CHECK_FALSE(test::tables_equal(two, swapped));
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

TEST_CASE("support: bounding_box") {
  const test::BBox unit = test::bounding_box(cube());
  CHECK(unit.min == ir::Vec3{0, 0, 0});
  CHECK(unit.max == ir::Vec3{1, 1, 1});
  ir::Mesh mesh;
  mesh.positions = {{-1.5f, 2, 3}, {4, -5, 6}, {0, 0, -7.25f}};
  const test::BBox box = test::bounding_box(mesh);
  CHECK(box.min == ir::Vec3{-1.5f, -5, -7.25f});
  CHECK(box.max == ir::Vec3{4, 2, 6});
  const test::BBox none = test::bounding_box(ir::Mesh{});
  CHECK(none.min == ir::Vec3{});
  CHECK(none.max == ir::Vec3{});
}

TEST_CASE("support: meshes_close accepts reordering within tolerance") {
  const ir::Mesh original = cube();
  REQUIRE(original.valid());
  CHECK(test::meshes_close(original, original, 0.0f));
  CHECK(test::mesh_diff(original, original, 0.0f) == "");
  CHECK(test::meshes_close(ir::Mesh{}, ir::Mesh{}, 0.0f));

  // Vertices stored in reverse order with the indices remapped, faces rotated.
  ir::Mesh permuted = original;
  std::reverse(permuted.positions.begin(), permuted.positions.end());
  for (std::uint32_t& index : permuted.indices) index = 7 - index;
  std::rotate(permuted.indices.begin(), permuted.indices.begin() + 9, permuted.indices.end());
  REQUIRE(permuted.valid());
  CHECK(permuted.positions != original.positions);
  CHECK(test::meshes_close(original, permuted, 0.0f));
  CHECK(test::meshes_close(permuted, original, 0.0f));

  // Un-welded: every triangle gets its own three vertices (as STL stores it).
  ir::Mesh soup;
  for (const std::uint32_t index : original.indices) {
    soup.indices.push_back(static_cast<std::uint32_t>(soup.positions.size()));
    soup.positions.push_back(original.positions[index]);
  }
  REQUIRE(soup.positions.size() == 36);
  CHECK(test::meshes_close(original, soup, 0.0f));

  // Quads instead of triangles: counted as their fan triangulation.
  ir::Mesh quads;
  quads.positions = original.positions;
  quads.indices = {0, 2, 3, 1, 4, 5, 7, 6, 0, 1, 5, 4, 2, 6, 7, 3, 0, 4, 6, 2, 1, 3, 7, 5};
  quads.face_sizes = {4, 4, 4, 4, 4, 4};
  REQUIRE(quads.valid());
  CHECK(test::meshes_close(original, quads, 0.0f));

  // Small coordinate noise (a text format with 4 decimals) passes a matching tolerance only.
  ir::Mesh jittered = permuted;
  for (std::size_t i = 0; i < jittered.positions.size(); ++i) {
    jittered.positions[i].x += (i % 2 == 0 ? 4e-5f : -4e-5f);
    jittered.positions[i].z -= 3e-5f;
  }
  CHECK(test::meshes_close(original, jittered, 1e-4f));
  CHECK(test::meshes_close(jittered, original, 1e-4f));
  CHECK_FALSE(test::meshes_close(original, jittered, 1e-5f));
  CHECK_FALSE(test::meshes_close(original, jittered, 0.0f));
  // Names, normals and other attributes are not compared.
  ir::Mesh decorated = original;
  decorated.name = "cube";
  decorated.normals.assign(8, ir::Vec3{0, 1, 0});
  CHECK(test::meshes_close(original, decorated, 0.0f));
}

TEST_CASE("support: mesh_diff explains tolerance failures") {
  const ir::Mesh original = cube();

  // A missing triangle.
  ir::Mesh fewer = original;
  fewer.indices.resize(fewer.indices.size() - 3);
  CHECK_FALSE(test::meshes_close(original, fewer, 0.1f));
  CHECK(test::mesh_diff(original, fewer, 0.1f) == "triangle count 12 vs 11");

  // A vertex moved outwards changes the bounding box.
  ir::Mesh stretched = original;
  stretched.positions[7].y = 1.5f;
  CHECK(test::mesh_diff(original, stretched, 0.25f) == "bounding box max (1, 1, 1) vs (1, 1.5, 1)");
  CHECK(test::meshes_close(original, stretched, 0.5f));
  ir::Mesh lowered = original;
  lowered.positions[0].z = -2.0f;
  CHECK(test::mesh_diff(original, lowered, 0.25f) == "bounding box min (0, 0, 0) vs (0, 0, -2)");

  // A vertex moved inwards keeps box and triangle count; only the point match catches it.
  ir::Mesh dented = original;
  dented.positions.push_back({0.5f, 0.5f, 0.5f});
  dented.indices[0] = 8;
  REQUIRE(dented.valid());
  CHECK_FALSE(test::meshes_close(original, dented, 0.01f));
  CHECK(test::mesh_diff(original, dented, 0.01f) ==
        "vertex 8 of the second mesh (0.5, 0.5, 0.5) has no vertex of the first mesh within 0.01");
  CHECK(test::mesh_diff(dented, original, 0.01f) ==
        "vertex 8 of the first mesh (0.5, 0.5, 0.5) has no vertex of the second mesh within 0.01");
  // The distance is Euclidean: (0.5, 0.5, 0.5) is 0.866 from every corner.
  CHECK_FALSE(test::meshes_close(original, dented, 0.86f));
  CHECK(test::meshes_close(original, dented, 0.87f));

  // Degenerate inputs never match silently.
  CHECK(test::mesh_diff(original, original, -1.0f) == "tolerance must be >= 0");
  CHECK_FALSE(test::meshes_close(original, original, std::numeric_limits<float>::quiet_NaN()));
  ir::Mesh broken = original;
  broken.positions[3].y = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(test::meshes_close(broken, broken, 0.5f));
  CHECK_FALSE(test::meshes_close(original, ir::Mesh{}, 10.0f));
}

TEST_CASE("support: mesh matching scales to large vertex counts") {
  // 40 000 vertices on a grid, compared against a shuffled copy: fast with
  // the sorted search, far too slow if every pair were tested.
  ir::Mesh grid;
  for (int i = 0; i < 40000; ++i) {
    grid.positions.push_back({static_cast<float>(i) * 0.25f, static_cast<float>(i % 200), static_cast<float>(i % 7)});
  }
  ir::Mesh shuffled = grid;
  const Bytes order = test::random_bytes(shuffled.positions.size() * 2, 17);
  for (std::size_t i = shuffled.positions.size() - 1; i > 0; --i) {
    const std::size_t pick =
        (std::to_integer<std::size_t>(order[i * 2]) * 256 + std::to_integer<std::size_t>(order[i * 2 + 1])) % (i + 1);
    std::swap(shuffled.positions[i], shuffled.positions[pick]);
  }
  CHECK(shuffled.positions != grid.positions);
  CHECK(test::meshes_close(grid, shuffled, 0.0f));
  shuffled.positions[12345].y += 0.5f;
  CHECK_FALSE(test::meshes_close(grid, shuffled, 0.1f));
  CHECK(test::meshes_close(grid, shuffled, 0.5f));
}
