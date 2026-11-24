// Convertible core - audio_lossless family: WavPack 4/5 block decoder and
// lossless encoder.
//
// A block holds one mono channel or a stereo pair: a 32-byte header, then
// metadata sub-blocks (decorrelation terms, weights and history, entropy
// medians, hybrid profile, float/int32 info, channel info, sample rate) and
// the entropy-coded residual bitstream (LSB-first). Decoding: adaptive-
// median Golomb words with zero runs, then the cascade of sign-LMS
// decorrelation passes (terms 1-8 and 17/18 within a channel, -1..-3 across
// channels), joint stereo, and the integer/float fix-ups. Hybrid (lossy)
// streams are decoded without their correction file. Every block's checksum
// is verified (mismatches are counted, see StreamDecoder::crc_errors).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossless/wavpack.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace cvt::audio::wavpack {

namespace {

// ---- header flags
constexpr std::uint32_t bytes_stored_mask = 3;
constexpr std::uint32_t mono_flag = 4;
constexpr std::uint32_t hybrid_flag = 8;
constexpr std::uint32_t joint_stereo = 0x10;
constexpr std::uint32_t float_data = 0x80;
constexpr std::uint32_t int32_data = 0x100;
constexpr std::uint32_t hybrid_bitrate = 0x200;
constexpr std::uint32_t hybrid_balance = 0x400;
constexpr std::uint32_t initial_block = 0x800;
constexpr std::uint32_t final_block = 0x1000;
constexpr std::uint32_t shift_lsb = 13;
constexpr std::uint32_t shift_mask = 0x1Fu << shift_lsb;
constexpr std::uint32_t mag_lsb = 18;
constexpr std::uint32_t srate_lsb = 23;
constexpr std::uint32_t srate_mask = 0xFu << srate_lsb;
constexpr std::uint32_t false_stereo = 0x40000000u;
constexpr std::uint32_t dsd_flag = 0x80000000u;

// ---- metadata ids
constexpr std::uint8_t id_function = 0x3F;
constexpr std::uint8_t id_odd_size = 0x40;
constexpr std::uint8_t id_large = 0x80;
constexpr std::uint8_t id_decorr_terms = 0x2;
constexpr std::uint8_t id_decorr_weights = 0x3;
constexpr std::uint8_t id_decorr_samples = 0x4;
constexpr std::uint8_t id_entropy_vars = 0x5;
constexpr std::uint8_t id_hybrid_profile = 0x6;
constexpr std::uint8_t id_float_info = 0x8;
constexpr std::uint8_t id_int32_info = 0x9;
constexpr std::uint8_t id_wv_bitstream = 0xA;
constexpr std::uint8_t id_wvx_bitstream = 0xC;
constexpr std::uint8_t id_channel_info = 0xD;
constexpr std::uint8_t id_md5_checksum = 0x26;
constexpr std::uint8_t id_sample_rate = 0x27;

constexpr std::uint32_t max_term = 8;
constexpr std::uint32_t limit_ones = 16;
constexpr int sls = 8;
constexpr std::int32_t slo = 1 << (sls - 1);

constexpr std::uint8_t float_shift_ones = 1;
constexpr std::uint8_t float_shift_same = 2;
constexpr std::uint8_t float_shift_sent = 4;
constexpr std::uint8_t float_zeros_sent = 8;
constexpr std::uint8_t float_neg_zeros = 0x10;

constexpr std::array<std::uint32_t, 15> sample_rates = {6000,  8000,  9600,  11025, 12000, 16000, 22050, 24000,
                                                        32000, 44100, 48000, 64000, 88200, 96000, 192000};

constexpr std::array<std::uint8_t, 256> exp2_table = {
    0x00, 0x01, 0x01, 0x02, 0x03, 0x03, 0x04, 0x05, 0x06, 0x06, 0x07, 0x08, 0x08, 0x09, 0x0a, 0x0b,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0e, 0x0f, 0x10, 0x10, 0x11, 0x12, 0x13, 0x13, 0x14, 0x15, 0x16, 0x16,
    0x17, 0x18, 0x19, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1d, 0x1e, 0x1f, 0x20, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x24, 0x25, 0x26, 0x27, 0x28, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3a, 0x3b, 0x3c, 0x3d,
    0x3e, 0x3f, 0x40, 0x41, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x48, 0x49, 0x4a, 0x4b,
    0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x5b, 0x5c, 0x5d, 0x5e, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x87, 0x88, 0x89, 0x8a,
    0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b,
    0x9c, 0x9d, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad,
    0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc8, 0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf, 0xd0, 0xd2, 0xd3, 0xd4,
    0xd6, 0xd7, 0xd8, 0xd9, 0xdb, 0xdc, 0xdd, 0xde, 0xe0, 0xe1, 0xe2, 0xe4, 0xe5, 0xe6, 0xe8, 0xe9,
    0xea, 0xec, 0xed, 0xee, 0xf0, 0xf1, 0xf2, 0xf4, 0xf5, 0xf6, 0xf8, 0xf9, 0xfa, 0xfc, 0xfd, 0xff};

constexpr std::array<std::uint8_t, 256> log2_table = {
    0x00, 0x01, 0x03, 0x04, 0x06, 0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x10, 0x11, 0x12, 0x14, 0x15,
    0x16, 0x18, 0x19, 0x1a, 0x1c, 0x1d, 0x1e, 0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2a,
    0x2c, 0x2d, 0x2e, 0x2f, 0x31, 0x32, 0x33, 0x34, 0x36, 0x37, 0x38, 0x39, 0x3b, 0x3c, 0x3d, 0x3e,
    0x3f, 0x41, 0x42, 0x43, 0x44, 0x45, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4d, 0x4e, 0x4f, 0x50, 0x51,
    0x52, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63,
    0x64, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85,
    0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
    0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb2,
    0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc0,
    0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcb, 0xcc, 0xcd, 0xce,
    0xcf, 0xd0, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd8, 0xd9, 0xda, 0xdb,
    0xdc, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe4, 0xe5, 0xe6, 0xe7, 0xe7,
    0xe8, 0xe9, 0xea, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xee, 0xef, 0xf0, 0xf1, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf4, 0xf5, 0xf6, 0xf7, 0xf7, 0xf8, 0xf9, 0xf9, 0xfa, 0xfb, 0xfc, 0xfc, 0xfd, 0xfe, 0xff, 0xff};

int nbits(std::uint32_t v) noexcept { return v == 0 ? 0 : 32 - std::countl_zero(v); }

std::int32_t wp_exp2s(std::int32_t log) noexcept {
  if (log < 0) return -wp_exp2s(-log);
  const std::uint32_t value = exp2_table[static_cast<std::size_t>(log & 0xFF)] | 0x100u;
  const std::int32_t exponent = log >> 8;
  if (exponent <= 9) return static_cast<std::int32_t>(value >> (9 - exponent));
  if (exponent - 9 >= 23) return std::numeric_limits<std::int32_t>::max();
  return static_cast<std::int32_t>(value << (exponent - 9));
}

std::int32_t wp_log2(std::uint32_t avalue) noexcept {
  avalue += avalue >> 9;
  if (avalue < (1u << 8)) {
    const int dbits = nbits(avalue);
    return (dbits << 8) + log2_table[(avalue << (9 - dbits)) & 0xFF];
  }
  const int dbits = nbits(avalue);
  return (dbits << 8) + log2_table[(avalue >> (dbits - 9)) & 0xFF];
}

std::int32_t wp_log2s(std::int32_t value) noexcept {
  return value < 0 ? -wp_log2(static_cast<std::uint32_t>(-static_cast<std::int64_t>(value)))
                   : wp_log2(static_cast<std::uint32_t>(value));
}

std::int32_t restore_weight(std::int8_t weight) noexcept {
  std::int32_t result = static_cast<std::int32_t>(weight) * 8;
  if (result > 0) result += (result + 64) >> 7;
  return result;
}

std::int8_t store_weight(std::int32_t weight) noexcept {
  weight = std::clamp(weight, -1024, 1024);
  if (weight > 0) weight -= (weight + 64) >> 7;
  return static_cast<std::int8_t>((weight + 4) >> 3);
}

std::int32_t wrap(std::int64_t v) noexcept { return static_cast<std::int32_t>(static_cast<std::uint32_t>(v)); }

std::int32_t apply_weight(std::int32_t weight, std::int32_t sample) noexcept {
  if (sample == static_cast<std::int16_t>(sample)) return wrap((static_cast<std::int64_t>(weight) * sample + 512) >> 10);
  const std::int32_t low = wrap((static_cast<std::int64_t>(sample & 0xFFFF) * weight) >> 9);
  const std::int32_t high = wrap(static_cast<std::int64_t>((sample & ~0xFFFF) >> 9) * weight);
  return wrap(static_cast<std::int64_t>(wrap(static_cast<std::int64_t>(low) + high + 1)) >> 1);
}

void update_weight(std::int32_t& weight, std::int32_t delta, std::int32_t source, std::int32_t result) noexcept {
  if (source != 0 && result != 0) {
    const std::int32_t s = (source ^ result) >> 31;
    weight = (delta ^ s) + (weight - s);
  }
}

void update_weight_clip(std::int32_t& weight, std::int32_t delta, std::int32_t source, std::int32_t result) noexcept {
  if (source != 0 && result != 0) {
    const std::int32_t s = (source ^ result) >> 31;
    weight = (weight ^ s) + (delta - s);
    if (weight > 1024) weight = 1024;
    weight = (weight ^ s) - s;
  }
}

struct DecorrPass {
  std::int32_t term = 0;
  std::int32_t delta = 0;
  std::int32_t weight_a = 0;
  std::int32_t weight_b = 0;
  std::array<std::int32_t, max_term> samples_a{};
  std::array<std::int32_t, max_term> samples_b{};
};

struct Entropy {
  std::array<std::uint32_t, 3> median{};
  std::int32_t slow_level = 0;
  std::uint32_t error_limit = 0;
};

struct Words {
  std::array<std::int32_t, 2> bitrate_delta{};
  std::array<std::uint32_t, 2> bitrate_acc{};
  std::uint32_t pend_data = 0, holding_one = 0, zeros_acc = 0;
  std::uint32_t holding_zero = 0, pend_count = 0;
  std::array<Entropy, 2> c{};
};

std::uint32_t get_med(const Entropy& c, int i) noexcept { return (c.median[static_cast<std::size_t>(i)] >> 4) + 1; }
void inc_med(Entropy& c, int i) noexcept {
  static constexpr std::array<std::uint32_t, 3> div = {128, 64, 32};
  auto& m = c.median[static_cast<std::size_t>(i)];
  m += ((m + div[static_cast<std::size_t>(i)]) / div[static_cast<std::size_t>(i)]) * 5;
}
void dec_med(Entropy& c, int i) noexcept {
  static constexpr std::array<std::uint32_t, 3> div = {128, 64, 32};
  auto& m = c.median[static_cast<std::size_t>(i)];
  m -= ((m + (div[static_cast<std::size_t>(i)] - 2)) / div[static_cast<std::size_t>(i)]) * 2;
}

void update_error_limit(Words& w, std::uint32_t flags) {
  std::int32_t bitrate_0 = static_cast<std::int32_t>((w.bitrate_acc[0] += static_cast<std::uint32_t>(w.bitrate_delta[0])) >> 16);
  if ((flags & mono_flag) != 0 || (flags & false_stereo) != 0) {
    if ((flags & hybrid_bitrate) != 0) {
      const std::int32_t slow_log_0 = (w.c[0].slow_level + slo) >> sls;
      w.c[0].error_limit = slow_log_0 - bitrate_0 > -0x100 ? static_cast<std::uint32_t>(wp_exp2s(slow_log_0 - bitrate_0 + 0x100)) : 0;
    } else {
      w.c[0].error_limit = static_cast<std::uint32_t>(wp_exp2s(bitrate_0));
    }
    return;
  }
  std::int32_t bitrate_1 = static_cast<std::int32_t>((w.bitrate_acc[1] += static_cast<std::uint32_t>(w.bitrate_delta[1])) >> 16);
  if ((flags & hybrid_bitrate) != 0) {
    const std::int32_t slow_log_0 = (w.c[0].slow_level + slo) >> sls;
    const std::int32_t slow_log_1 = (w.c[1].slow_level + slo) >> sls;
    if ((flags & hybrid_balance) != 0) {
      const std::int32_t balance = (slow_log_1 - slow_log_0 + bitrate_1 + 1) >> 1;
      if (balance > bitrate_0) {
        bitrate_1 = bitrate_0 * 2;
        bitrate_0 = 0;
      } else if (-balance > bitrate_0) {
        bitrate_0 = bitrate_0 * 2;
        bitrate_1 = 0;
      } else {
        bitrate_1 = bitrate_0 + balance;
        bitrate_0 = bitrate_0 - balance;
      }
    }
    w.c[0].error_limit = slow_log_0 - bitrate_0 > -0x100 ? static_cast<std::uint32_t>(wp_exp2s(slow_log_0 - bitrate_0 + 0x100)) : 0;
    w.c[1].error_limit = slow_log_1 - bitrate_1 > -0x100 ? static_cast<std::uint32_t>(wp_exp2s(slow_log_1 - bitrate_1 + 0x100)) : 0;
  } else {
    w.c[0].error_limit = static_cast<std::uint32_t>(wp_exp2s(bitrate_0));
    w.c[1].error_limit = static_cast<std::uint32_t>(wp_exp2s(bitrate_1));
  }
}

// ---- LSB-first bit reader
class LsbReader {
 public:
  LsbReader() = default;
  explicit LsbReader(ByteView data) : data_(as_u8(data)) {}
  std::uint32_t bit() noexcept {
    if (pos_ >= data_.size() * 8) {
      overrun_ = true;
      return 0;
    }
    const std::uint32_t b = (data_[static_cast<std::size_t>(pos_ >> 3)] >> (pos_ & 7)) & 1u;
    ++pos_;
    return b;
  }
  std::uint32_t bits(int count) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < count; ++i) v |= bit() << i;
    return v;
  }
  [[nodiscard]] std::uint32_t peek(int count) const noexcept {
    std::uint32_t v = 0;
    std::uint64_t p = pos_;
    for (int i = 0; i < count; ++i, ++p) {
      if (p >= data_.size() * 8) break;
      v |= static_cast<std::uint32_t>((data_[static_cast<std::size_t>(p >> 3)] >> (p & 7)) & 1u) << i;
    }
    return v;
  }
  void skip(int count) noexcept {
    pos_ += static_cast<std::uint64_t>(count);
    if (pos_ > data_.size() * 8) overrun_ = true;
  }
  [[nodiscard]] bool open() const noexcept { return !data_.empty(); }
  [[nodiscard]] bool overrun() const noexcept { return overrun_; }

 private:
  std::span<const std::uint8_t> data_{};
  std::uint64_t pos_ = 0;
  bool overrun_ = false;
};

std::uint32_t read_code(LsbReader& bs, std::uint32_t maxcode) {
  if (maxcode < 2) return maxcode != 0 ? bs.bit() : 0;
  const int bitcount = nbits(maxcode);
  const std::uint32_t extras = static_cast<std::uint32_t>((std::uint64_t{1} << bitcount) - maxcode - 1);
  const std::uint32_t low = bs.peek(bitcount - 1);
  if (low >= extras) {
    const std::uint32_t top = (bs.peek(bitcount) >> (bitcount - 1)) & 1u;
    bs.skip(bitcount);
    return static_cast<std::uint32_t>((std::uint64_t{low} << 1) - extras + top);
  }
  bs.skip(bitcount - 1);
  return low;
}

// Returns false at the end of the data.
bool get_word(Words& w, LsbReader& bs, int chan, std::uint32_t flags, std::int32_t& out) {
  Entropy& c = w.c[static_cast<std::size_t>(chan)];
  out = 0;
  if ((w.c[0].median[0] & ~1u) == 0 && w.holding_zero == 0 && w.holding_one == 0 && (w.c[1].median[0] & ~1u) == 0) {
    if (w.zeros_acc != 0) {
      if (--w.zeros_acc != 0) {
        c.slow_level -= (c.slow_level + slo) >> sls;
        return true;
      }
    } else {
      int cbits = 0;
      while (cbits < 33 && bs.bit() != 0) ++cbits;
      if (cbits == 33 || bs.overrun()) return false;
      if (cbits < 2) {
        w.zeros_acc = static_cast<std::uint32_t>(cbits);
      } else {
        std::uint32_t mask = 1;
        w.zeros_acc = 0;
        while (--cbits != 0) {
          if (bs.bit() != 0) w.zeros_acc |= mask;
          mask <<= 1;
        }
        w.zeros_acc |= mask;
      }
      if (w.zeros_acc != 0) {
        c.slow_level -= (c.slow_level + slo) >> sls;
        w.c[0].median.fill(0);
        w.c[1].median.fill(0);
        return true;
      }
    }
  }
  std::uint32_t ones_count = 0;
  if (w.holding_zero != 0) {
    w.holding_zero = 0;
  } else {
    while (ones_count < limit_ones + 1 && bs.bit() != 0) ++ones_count;
    if (ones_count == limit_ones + 1 || bs.overrun()) return false;
    if (ones_count == limit_ones) {
      int cbits = 0;
      while (cbits < 33 && bs.bit() != 0) ++cbits;
      if (cbits == 33) return false;
      if (cbits < 2) {
        ones_count = static_cast<std::uint32_t>(cbits);
      } else {
        std::uint32_t mask = 1;
        ones_count = 0;
        while (--cbits != 0) {
          if (bs.bit() != 0) ones_count |= mask;
          mask <<= 1;
        }
        ones_count |= mask;
      }
      ones_count += limit_ones;
    }
    if (w.holding_one != 0) {
      w.holding_one = ones_count & 1u;
      ones_count = (ones_count >> 1) + 1;
    } else {
      w.holding_one = ones_count & 1u;
      ones_count >>= 1;
    }
    w.holding_zero = ~w.holding_one & 1u;
  }
  if ((flags & hybrid_flag) != 0 && chan == 0) update_error_limit(w, flags);
  std::uint32_t low, high;
  if (ones_count == 0) {
    low = 0;
    high = get_med(c, 0) - 1;
    dec_med(c, 0);
  } else {
    low = get_med(c, 0);
    inc_med(c, 0);
    if (ones_count == 1) {
      high = low + get_med(c, 1) - 1;
      dec_med(c, 1);
    } else {
      low += get_med(c, 1);
      inc_med(c, 1);
      if (ones_count == 2) {
        high = low + get_med(c, 2) - 1;
        dec_med(c, 2);
      } else {
        low += (ones_count - 2) * get_med(c, 2);
        high = low + get_med(c, 2) - 1;
        inc_med(c, 2);
      }
    }
  }
  low &= 0x7FFFFFFFu;
  high &= 0x7FFFFFFFu;
  if (low > high) high = low;
  std::uint32_t mid = (high + low + 1) >> 1;
  if (c.error_limit == 0) {
    mid = read_code(bs, high - low) + low;
  } else {
    while (high - low > c.error_limit) {
      if (bs.bit() != 0) {
        low = mid;
        mid = (high + low + 1) >> 1;
      } else {
        high = mid - 1;
        mid = (high + low + 1) >> 1;
      }
    }
  }
  const std::uint32_t sign = bs.bit();
  if ((flags & hybrid_bitrate) != 0) {
    c.slow_level -= (c.slow_level + slo) >> sls;
    c.slow_level += wp_log2(mid);
  }
  out = sign != 0 ? static_cast<std::int32_t>(~mid) : static_cast<std::int32_t>(mid);
  return !bs.overrun();
}

void decorr_mono(DecorrPass& d, std::int32_t* buffer, std::size_t count) {
  if (d.term > static_cast<std::int32_t>(max_term)) {
    for (std::size_t i = 0; i < count; ++i) {
      const std::int32_t sam = d.term == 17 ? wrap(2ll * d.samples_a[0] - d.samples_a[1])
                                            : wrap((3ll * d.samples_a[0] - d.samples_a[1]) >> 1);
      d.samples_a[1] = d.samples_a[0];
      d.samples_a[0] = wrap(static_cast<std::int64_t>(apply_weight(d.weight_a, sam)) + buffer[i]);
      update_weight(d.weight_a, d.delta, sam, buffer[i]);
      buffer[i] = d.samples_a[0];
    }
    return;
  }
  std::size_t m = 0;
  std::size_t k = static_cast<std::size_t>(d.term) & (max_term - 1);
  for (std::size_t i = 0; i < count; ++i) {
    const std::int32_t sam = d.samples_a[m];
    d.samples_a[k] = wrap(static_cast<std::int64_t>(apply_weight(d.weight_a, sam)) + buffer[i]);
    update_weight(d.weight_a, d.delta, sam, buffer[i]);
    buffer[i] = d.samples_a[k];
    m = (m + 1) & (max_term - 1);
    k = (k + 1) & (max_term - 1);
  }
}

void decorr_stereo(DecorrPass& d, std::int32_t* buffer, std::size_t count) {
  std::int32_t* b = buffer;
  if (d.term > static_cast<std::int32_t>(max_term)) {
    for (std::size_t i = 0; i < count; ++i, b += 2) {
      std::int32_t sam = d.term == 17 ? wrap(2ll * d.samples_a[0] - d.samples_a[1])
                                      : wrap((3ll * d.samples_a[0] - d.samples_a[1]) >> 1);
      d.samples_a[1] = d.samples_a[0];
      d.samples_a[0] = wrap(static_cast<std::int64_t>(apply_weight(d.weight_a, sam)) + b[0]);
      update_weight(d.weight_a, d.delta, sam, b[0]);
      b[0] = d.samples_a[0];
      sam = d.term == 17 ? wrap(2ll * d.samples_b[0] - d.samples_b[1]) : wrap((3ll * d.samples_b[0] - d.samples_b[1]) >> 1);
      d.samples_b[1] = d.samples_b[0];
      d.samples_b[0] = wrap(static_cast<std::int64_t>(apply_weight(d.weight_b, sam)) + b[1]);
      update_weight(d.weight_b, d.delta, sam, b[1]);
      b[1] = d.samples_b[0];
    }
    return;
  }
  if (d.term > 0) {
    std::size_t m = 0;
    std::size_t k = static_cast<std::size_t>(d.term) & (max_term - 1);
    for (std::size_t i = 0; i < count; ++i, b += 2) {
      std::int32_t sam = d.samples_a[m];
      d.samples_a[k] = wrap(static_cast<std::int64_t>(apply_weight(d.weight_a, sam)) + b[0]);
      update_weight(d.weight_a, d.delta, sam, b[0]);
      b[0] = d.samples_a[k];
      sam = d.samples_b[m];
      d.samples_b[k] = wrap(static_cast<std::int64_t>(apply_weight(d.weight_b, sam)) + b[1]);
      update_weight(d.weight_b, d.delta, sam, b[1]);
      b[1] = d.samples_b[k];
      m = (m + 1) & (max_term - 1);
      k = (k + 1) & (max_term - 1);
    }
    return;
  }
  for (std::size_t i = 0; i < count; ++i, b += 2) {
    if (d.term == -1) {
      const std::int32_t sam_a = wrap(static_cast<std::int64_t>(b[0]) + apply_weight(d.weight_a, d.samples_a[0]));
      update_weight_clip(d.weight_a, d.delta, d.samples_a[0], b[0]);
      b[0] = sam_a;
      d.samples_a[0] = wrap(static_cast<std::int64_t>(b[1]) + apply_weight(d.weight_b, sam_a));
      update_weight_clip(d.weight_b, d.delta, sam_a, b[1]);
      b[1] = d.samples_a[0];
    } else if (d.term == -2) {
      const std::int32_t sam_b = wrap(static_cast<std::int64_t>(b[1]) + apply_weight(d.weight_b, d.samples_b[0]));
      update_weight_clip(d.weight_b, d.delta, d.samples_b[0], b[1]);
      b[1] = sam_b;
      d.samples_b[0] = wrap(static_cast<std::int64_t>(b[0]) + apply_weight(d.weight_a, sam_b));
      update_weight_clip(d.weight_a, d.delta, sam_b, b[0]);
      b[0] = d.samples_b[0];
    } else {
      const std::int32_t sam_a = wrap(static_cast<std::int64_t>(b[0]) + apply_weight(d.weight_a, d.samples_a[0]));
      update_weight_clip(d.weight_a, d.delta, d.samples_a[0], b[0]);
      const std::int32_t sam_b = wrap(static_cast<std::int64_t>(b[1]) + apply_weight(d.weight_b, d.samples_b[0]));
      update_weight_clip(d.weight_b, d.delta, d.samples_b[0], b[1]);
      b[0] = d.samples_b[0] = sam_a;
      b[1] = d.samples_a[0] = sam_b;
    }
  }
}

struct Metadata {
  std::uint8_t id = 0;
  ByteView data;
};

Result<std::vector<Metadata>> read_metadata(ByteView body) {
  std::vector<Metadata> out;
  std::size_t pos = 0;
  while (pos + 2 <= body.size()) {
    const auto id = std::to_integer<std::uint8_t>(body[pos]);
    std::size_t words = std::to_integer<std::uint8_t>(body[pos + 1]);
    std::size_t header = 2;
    if ((id & id_large) != 0) {
      if (pos + 4 > body.size()) return fail(Status::Decode, "truncated WavPack metadata");
      words |= std::size_t{std::to_integer<std::uint8_t>(body[pos + 2])} << 8;
      words |= std::size_t{std::to_integer<std::uint8_t>(body[pos + 3])} << 16;
      header = 4;
    }
    const std::size_t padded = words * 2;
    if (padded > body.size() - pos - header) return fail(Status::Decode, "truncated WavPack metadata");
    std::size_t length = padded;
    if ((id & id_odd_size) != 0 && length > 0) --length;
    out.push_back({static_cast<std::uint8_t>(id & id_function), body.subspan(pos + header, length)});
    pos += header + padded;
  }
  return out;
}

void put_float(float& out, std::uint32_t mantissa, std::uint32_t exponent, std::uint32_t sign) {
  const std::uint32_t bits = (mantissa & 0x7FFFFFu) | ((exponent & 0xFFu) << 23) | (sign << 31);
  out = std::bit_cast<float>(bits);
}

}  // namespace

// =========================================================================== header

Result<BlockHeader> parse_header(ByteView data) {
  util::ByteReader r(data);
  if (!r.consume("wvpk")) return fail(Status::Decode, "not a WavPack block");
  BlockHeader h;
  h.size = r.u32le();
  h.version = r.u16le();
  const std::uint8_t index_high = r.u8();
  const std::uint8_t total_high = r.u8();
  const std::uint32_t total_low = r.u32le();
  const std::uint32_t index_low = r.u32le();
  h.block_samples = r.u32le();
  h.flags = r.u32le();
  h.crc = r.u32le();
  if (!r.ok()) return fail(Status::Decode, "truncated WavPack block header");
  if (h.size < 24 || h.size > (1u << 24)) return fail(Status::Decode, "invalid WavPack block size");
  if (h.version < 0x402 || h.version > 0x410) return fail(Status::Decode, "unsupported WavPack version 0x{:X}", h.version);
  h.total_known = !(total_low == 0xFFFFFFFFu && (total_high == 0 || total_high == 0xFF));
  h.total_samples = (std::uint64_t{total_high} << 32) | total_low;
  h.block_index = (std::uint64_t{index_high} << 32) | index_low;
  return h;
}

// =========================================================================== decoder

Result<std::size_t> StreamDecoder::decode_frame(ByteView data, std::vector<float>& out) {
  std::size_t pos = 0;
  std::vector<std::vector<float>> planes;
  std::uint32_t frame_samples = 0;
  std::uint32_t frame_channels_declared = 0;
  std::uint32_t frame_mask = 0;
  bool first = true;
  for (int guard = 0; guard < 4096; ++guard) {
    CVT_TRY_ASSIGN(const BlockHeader h, parse_header(data.subspan(pos)));
    const std::size_t block_size = std::size_t{h.size} + 8;
    if (block_size > data.size() - pos) return fail(Status::Decode, "truncated WavPack block");
    const ByteView block = data.subspan(pos, block_size);
    pos += block_size;
    if (first && (h.flags & initial_block) == 0) return fail(Status::Decode, "WavPack frame does not start with an initial block");
    if ((h.flags & dsd_flag) != 0) return fail(Status::Decode, "DSD WavPack files are not supported");
    if (first) frame_samples = h.block_samples;
    if (h.block_samples != frame_samples) return fail(Status::Decode, "WavPack blocks of one frame differ in length");
    if (frame_samples > (1u << 18)) return fail(Status::Decode, "WavPack block is too long");
    CVT_TRY_ASSIGN(const std::vector<Metadata> meta, read_metadata(block.subspan(32)));
    const std::uint32_t flags = h.flags;
    const bool mono_data = (flags & (mono_flag | false_stereo)) != 0;
    const std::size_t n = h.block_samples;
    std::vector<DecorrPass> passes;
    Words words;
    LsbReader wv, wvx;
    std::uint32_t crc_x_stored = 0;
    std::uint8_t float_flags = 0, float_shift = 0, float_max_exp = 0;
    std::uint8_t int32_sent = 0, int32_zeros = 0, int32_ones = 0, int32_dups = 0;
    bool have_wv = false;
    std::uint32_t rate = 0;
    const std::uint32_t srate_index = (flags & srate_mask) >> srate_lsb;
    if (srate_index < sample_rates.size()) rate = sample_rates[srate_index];
    for (const Metadata& m : meta) {
      const std::span<const std::uint8_t> b = as_u8(m.data);
      switch (m.id) {
        case id_decorr_terms: {
          if (b.size() > 16) return fail(Status::Decode, "too many WavPack decorrelation terms");
          passes.assign(b.size(), DecorrPass{});
          for (std::size_t i = 0; i < b.size(); ++i) {
            DecorrPass& p = passes[b.size() - 1 - i];
            p.term = static_cast<std::int32_t>(b[i] & 0x1F) - 5;
            p.delta = (b[i] >> 5) & 7;
            if (p.term == 0 || p.term < -3 || (p.term > static_cast<std::int32_t>(max_term) && p.term < 17) || p.term > 18 ||
                (mono_data && p.term < 0)) {
              return fail(Status::Decode, "invalid WavPack decorrelation term");
            }
          }
          break;
        }
        case id_decorr_weights: {
          std::size_t count = mono_data ? b.size() : b.size() / 2;
          if (count > passes.size()) return fail(Status::Decode, "invalid WavPack decorrelation weights");
          std::size_t at = 0;
          for (std::size_t i = passes.size(); i-- > 0 && count-- > 0;) {
            passes[i].weight_a = restore_weight(static_cast<std::int8_t>(b[at++]));
            if (!mono_data) passes[i].weight_b = restore_weight(static_cast<std::int8_t>(b[at++]));
          }
          break;
        }
        case id_decorr_samples: {
          util::ByteReader r(m.data);
          const auto next = [&]() { return wp_exp2s(r.i16le()); };
          for (std::size_t i = passes.size(); i-- > 0 && r.remaining() > 0;) {
            DecorrPass& p = passes[i];
            if (p.term > static_cast<std::int32_t>(max_term)) {
              p.samples_a[0] = next();
              p.samples_a[1] = next();
              if (!mono_data) {
                p.samples_b[0] = next();
                p.samples_b[1] = next();
              }
            } else if (p.term < 0) {
              p.samples_a[0] = next();
              p.samples_b[0] = next();
            } else {
              for (std::int32_t k = 0; k < p.term; ++k) {
                p.samples_a[static_cast<std::size_t>(k)] = next();
                if (!mono_data) p.samples_b[static_cast<std::size_t>(k)] = next();
              }
            }
          }
          if (!r.ok()) return fail(Status::Decode, "truncated WavPack decorrelation samples");
          break;
        }
        case id_entropy_vars: {
          if (b.size() != (mono_data ? 6u : 12u)) return fail(Status::Decode, "invalid WavPack entropy variables");
          util::ByteReader r(m.data);
          for (std::size_t c = 0; c < (mono_data ? 1u : 2u); ++c) {
            for (std::size_t k = 0; k < 3; ++k) words.c[c].median[k] = static_cast<std::uint32_t>(wp_exp2s(r.u16le()));
          }
          break;
        }
        case id_hybrid_profile: {
          util::ByteReader r(m.data);
          if ((flags & hybrid_bitrate) != 0) {
            words.c[0].slow_level = wp_exp2s(r.u16le());
            if (!mono_data) words.c[1].slow_level = wp_exp2s(r.u16le());
          }
          words.bitrate_acc[0] = static_cast<std::uint32_t>(r.u16le()) << 16;
          if (!mono_data) words.bitrate_acc[1] = static_cast<std::uint32_t>(r.u16le()) << 16;
          if (r.remaining() > 0) {
            words.bitrate_delta[0] = wp_exp2s(r.i16le());
            if (!mono_data) words.bitrate_delta[1] = wp_exp2s(r.i16le());
          }
          if (!r.ok()) return fail(Status::Decode, "invalid WavPack hybrid profile");
          break;
        }
        case id_float_info:
          if (b.size() != 4) return fail(Status::Decode, "invalid WavPack float information");
          float_flags = b[0];
          float_shift = b[1];
          float_max_exp = b[2];
          break;
        case id_int32_info:
          if (b.size() != 4) return fail(Status::Decode, "invalid WavPack integer information");
          int32_sent = b[0];
          int32_zeros = b[1];
          int32_ones = b[2];
          int32_dups = b[3];
          break;
        case id_wv_bitstream:
          wv = LsbReader(m.data);
          have_wv = true;
          break;
        case id_wvx_bitstream:
          if (b.size() > 4) {
            crc_x_stored = util::ByteReader(m.data).u32le();
            wvx = LsbReader(m.data.subspan(4));
          }
          break;
        case id_channel_info:
          if (first && !b.empty()) {
            if (b.size() >= 6) {
              frame_channels_declared = (b[0] | ((b[2] & 0x0Fu) << 8)) + 1;
              frame_mask = b[3] | (std::uint32_t{b[4]} << 8) | (std::uint32_t{b[5]} << 16);
            } else {
              frame_channels_declared = b[0];
              for (std::size_t k = 1; k < b.size(); ++k) frame_mask |= std::uint32_t{b[k]} << (8 * (k - 1));
            }
          }
          break;
        case id_sample_rate:
          if (b.size() >= 3) rate = b[0] | (std::uint32_t{b[1]} << 8) | (std::uint32_t{b[2]} << 16);
          if (b.size() >= 4) rate |= std::uint32_t{b[3] & 0x7Fu} << 24;
          break;
        case id_md5_checksum:
          if (b.size() == 16) md5_.assign(b.begin(), b.end());
          break;
        default: break;
      }
    }
    if (n > 0 && rate == 0) return fail(Status::Decode, "WavPack block without a sample rate");
    if (rate != 0) sample_rate_ = rate;
    const std::size_t block_channels = mono_data ? 1 : 2;
    std::vector<std::int32_t> buffer(n * block_channels, 0);
    if (n > 0) {
      if (!have_wv) return fail(Status::Decode, "WavPack block without audio data");
      for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t c = 0; c < block_channels; ++c) {
          std::int32_t word = 0;
          if (!get_word(words, wv, static_cast<int>(c), flags, word)) {
            return fail(Status::Decode, "WavPack audio data is truncated or damaged");
          }
          buffer[i * block_channels + c] = word;
        }
      }
      for (DecorrPass& p : passes) {
        if (mono_data) decorr_mono(p, buffer.data(), n);
        else decorr_stereo(p, buffer.data(), n);
      }
      if (!mono_data && (flags & joint_stereo) != 0) {
        for (std::size_t i = 0; i < n; ++i) {
          std::int32_t& a = buffer[i * 2];
          std::int32_t& bb = buffer[i * 2 + 1];
          bb = wrap(static_cast<std::int64_t>(bb) - (a >> 1));
          a = wrap(static_cast<std::int64_t>(a) + bb);
        }
      }
      std::uint32_t crc = 0xFFFFFFFFu;
      for (const std::int32_t v : buffer) crc = crc * 3u + static_cast<std::uint32_t>(v);
      if (crc != h.crc) ++crc_errors_;
    }
    // Fix-ups and conversion to float planes.
    const std::uint32_t bytes = (flags & bytes_stored_mask) + 1;
    const bool lossy = (flags & hybrid_flag) != 0;
    if (lossy) lossy_ = true;
    std::vector<float> values(buffer.size());
    if ((flags & float_data) != 0) {
      float_ = true;
      bits_ = 32;
      for (std::size_t i = 0; i < buffer.size(); ++i) {
        std::int32_t v = buffer[i];
        float outval = 0.0f;
        std::int32_t exp = float_max_exp;
        std::uint32_t sign = 0;
        if (wvx.open()) {
          if (v == 0) {
            if ((float_flags & float_zeros_sent) != 0) {
              if (wvx.bit() != 0) {
                const std::uint32_t mant = wvx.bits(23);
                const std::uint32_t e = exp >= 25 ? wvx.bits(8) : 0;
                sign = wvx.bit();
                put_float(outval, mant, e, sign);
              } else if ((float_flags & float_neg_zeros) != 0) {
                put_float(outval, 0, 0, wvx.bit());
              }
            }
          } else {
            v = static_cast<std::int32_t>(static_cast<std::uint32_t>(v) << float_shift);
            if (v < 0) {
              v = -v;
              sign = 1;
            }
            if (v == 0x1000000) {
              std::uint32_t mant = 0;
              if (wvx.bit() != 0) mant = wvx.bits(23);
              put_float(outval, mant, 255, sign);
            } else {
              int shift_count = 0;
              if (exp != 0) {
                while ((v & 0x800000) == 0 && --exp != 0) {
                  ++shift_count;
                  v <<= 1;
                }
              }
              if (shift_count != 0) {
                if ((float_flags & float_shift_ones) != 0 || ((float_flags & float_shift_same) != 0 && wvx.bit() != 0)) {
                  v |= (1 << shift_count) - 1;
                } else if ((float_flags & float_shift_sent) != 0) {
                  v |= static_cast<std::int32_t>(wvx.bits(shift_count) & ((1u << shift_count) - 1));
                }
              }
              put_float(outval, static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(exp), sign);
            }
          }
        } else if (v != 0) {
          v = static_cast<std::int32_t>(static_cast<std::uint32_t>(v) << float_shift);
          if (v < 0) {
            v = -v;
            sign = 1;
          }
          if (v >= 0x1000000) {
            while ((v & 0xF000000) != 0) {
              v >>= 1;
              ++exp;
            }
          } else if (exp != 0) {
            int shift_count = 0;
            while ((v & 0x800000) == 0 && --exp != 0) {
              ++shift_count;
              v <<= 1;
            }
            if (shift_count != 0 && (float_flags & float_shift_ones) != 0) v |= (1 << shift_count) - 1;
          }
          put_float(outval, static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(exp), sign);
        }
        values[i] = std::isfinite(outval) ? outval : 0.0f;
      }
      (void)crc_x_stored;
    } else {
      std::uint32_t shift = (flags & shift_mask) >> shift_lsb;
      if ((flags & int32_data) != 0) {
        if (wvx.open()) {
          for (std::int32_t& v : buffer) {
            if (int32_sent != 0) {
              const std::uint32_t data_bits = wvx.bits(int32_sent);
              v = static_cast<std::int32_t>((static_cast<std::uint32_t>(v) << int32_sent) | (data_bits & ((1u << int32_sent) - 1)));
            }
            if (int32_zeros != 0) {
              v = static_cast<std::int32_t>(static_cast<std::uint32_t>(v) << int32_zeros);
            } else if (int32_ones != 0) {
              v = static_cast<std::int32_t>((static_cast<std::uint32_t>(v + 1) << int32_ones) - 1);
            } else if (int32_dups != 0) {
              v = static_cast<std::int32_t>((static_cast<std::uint32_t>(v + (v & 1)) << int32_dups) - static_cast<std::uint32_t>(v & 1));
            }
          }
        } else if (int32_sent == 0 && (int32_zeros + int32_ones + int32_dups) != 0) {
          for (std::int32_t& v : buffer) {
            if (int32_zeros != 0) {
              v = static_cast<std::int32_t>(static_cast<std::uint32_t>(v) << int32_zeros);
            } else if (int32_ones != 0) {
              v = static_cast<std::int32_t>((static_cast<std::uint32_t>(v + 1) << int32_ones) - 1);
            } else {
              v = static_cast<std::int32_t>((static_cast<std::uint32_t>(v + (v & 1)) << int32_dups) - static_cast<std::uint32_t>(v & 1));
            }
          }
        } else {
          shift += int32_zeros + int32_sent + int32_ones + int32_dups;
        }
      }
      if (shift > 31) return fail(Status::Decode, "invalid WavPack shift");
      std::int64_t min_value = -(std::int64_t{1} << (bytes * 8 - 1));
      std::int64_t max_value = (std::int64_t{1} << (bytes * 8 - 1)) - 1;
      const double scale = 1.0 / static_cast<double>(std::uint64_t{1} << (bytes * 8 - 1));
      for (std::size_t i = 0; i < buffer.size(); ++i) {
        std::int64_t v = static_cast<std::int64_t>(buffer[i]) * (std::int64_t{1} << shift);
        if (lossy) v = std::clamp(v, min_value, max_value);
        values[i] = static_cast<float>(static_cast<double>(v) * scale);
      }
      bits_ = static_cast<std::uint16_t>(bytes * 8);
    }
    // Into per-channel planes.
    if (mono_data) {
      planes.push_back(values);
      if ((flags & false_stereo) != 0) planes.push_back(std::move(values));
    } else {
      std::vector<float> left(n), right(n);
      for (std::size_t i = 0; i < n; ++i) {
        left[i] = values[i * 2];
        right[i] = values[i * 2 + 1];
      }
      planes.push_back(std::move(left));
      planes.push_back(std::move(right));
    }
    first = false;
    if ((flags & final_block) != 0) break;
    if (pos >= data.size()) return fail(Status::Decode, "WavPack frame without a final block");
  }
  const std::uint32_t channels = frame_channels_declared != 0 ? std::min<std::uint32_t>(frame_channels_declared, static_cast<std::uint32_t>(planes.size()))
                                                              : static_cast<std::uint32_t>(planes.size());
  if (channels == 0 || channels > 255) return fail(Status::Decode, "invalid WavPack channel count");
  if (channels_ != 0 && channels_ != channels) return fail(Status::Decode, "the WavPack stream changes channel count");
  channels_ = static_cast<std::uint16_t>(channels);
  if (frame_mask != 0) mask_ = frame_mask;
  const std::size_t start = out.size();
  out.resize(start + std::size_t{frame_samples} * channels);
  for (std::size_t i = 0; i < frame_samples; ++i) {
    for (std::size_t c = 0; c < channels; ++c) out[start + i * channels + c] = planes[c][i];
  }
  return pos;
}

Result<void> StreamDecoder::decode_blocks(ByteView data, std::vector<float>& out) {
  std::size_t pos = 0;
  while (pos + 32 <= data.size()) {
    CVT_TRY_ASSIGN(const std::size_t used, decode_frame(data.subspan(pos), out));
    pos += used;
  }
  return {};
}

// =========================================================================== encoder

namespace {

class LsbWriter {
 public:
  void bit(std::uint32_t b) {
    if (b != 0) current_ |= static_cast<std::uint8_t>(1u << filled_);
    if (++filled_ == 8) {
      bytes_.push_back(current_);
      current_ = 0;
      filled_ = 0;
    }
  }
  void bits(std::uint32_t value, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) bit((value >> i) & 1u);
  }
  Bytes finish() {
    if (filled_ > 0) {
      bytes_.push_back(current_);
      current_ = 0;
      filled_ = 0;
    }
    if (bytes_.size() % 2 == 1) bytes_.push_back(0);
    return to_bytes(std::span<const std::uint8_t>(bytes_));
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::uint8_t current_ = 0;
  std::uint32_t filled_ = 0;
};

class WordEncoder {
 public:
  explicit WordEncoder(LsbWriter& out) : out_(out) {}
  void set_median(int chan, int k, std::uint32_t value) {
    w_.c[static_cast<std::size_t>(chan)].median[static_cast<std::size_t>(k)] = value;
  }
  [[nodiscard]] std::uint32_t median(int chan, int k) const {
    return w_.c[static_cast<std::size_t>(chan)].median[static_cast<std::size_t>(k)];
  }
  void send(std::int32_t value, int chan) {
    Entropy& c = w_.c[static_cast<std::size_t>(chan)];
    const bool negative = value < 0;
    if (w_.c[0].median[0] < 2 && w_.holding_zero == 0 && w_.c[1].median[0] < 2) {
      if (w_.zeros_acc != 0) {
        if (value != 0) {
          flush();
        } else {
          c.slow_level -= (c.slow_level + slo) >> sls;
          ++w_.zeros_acc;
          return;
        }
      } else if (value != 0) {
        out_.bit(0);
      } else {
        c.slow_level -= (c.slow_level + slo) >> sls;
        w_.c[0].median.fill(0);
        w_.c[1].median.fill(0);
        w_.zeros_acc = 1;
        return;
      }
    }
    const std::uint32_t v = negative ? static_cast<std::uint32_t>(~value) : static_cast<std::uint32_t>(value);
    std::uint32_t ones_count, low, high;
    if (v < get_med(c, 0)) {
      ones_count = 0;
      low = 0;
      high = get_med(c, 0) - 1;
      dec_med(c, 0);
    } else {
      low = get_med(c, 0);
      inc_med(c, 0);
      if (v - low < get_med(c, 1)) {
        ones_count = 1;
        high = low + get_med(c, 1) - 1;
        dec_med(c, 1);
      } else {
        low += get_med(c, 1);
        inc_med(c, 1);
        if (v - low < get_med(c, 2)) {
          ones_count = 2;
          high = low + get_med(c, 2) - 1;
          dec_med(c, 2);
        } else {
          ones_count = 2 + (v - low) / get_med(c, 2);
          low += (ones_count - 2) * get_med(c, 2);
          high = low + get_med(c, 2) - 1;
          inc_med(c, 2);
        }
      }
    }
    if (w_.holding_zero != 0) {
      if (ones_count != 0) ++w_.holding_one;
      flush();
      if (ones_count != 0) {
        w_.holding_zero = 1;
        --ones_count;
      } else {
        w_.holding_zero = 0;
      }
    } else {
      w_.holding_zero = 1;
    }
    w_.holding_one = ones_count * 2;
    if (high != low) {
      const std::uint32_t maxcode = high - low;
      const std::uint32_t code = v - low;
      const int bitcount = nbits(maxcode);
      const std::uint32_t extras = static_cast<std::uint32_t>((std::uint64_t{1} << bitcount) - maxcode - 1);
      if (code < extras) {
        pend(code, static_cast<std::uint32_t>(bitcount - 1));
      } else {
        pend((code + extras) >> 1, static_cast<std::uint32_t>(bitcount - 1));
        pend((code + extras) & 1u, 1);
      }
    }
    pend(negative ? 1u : 0u, 1);
    if (w_.holding_zero == 0) flush();
  }

  void flush() {
    if (w_.zeros_acc != 0) {
      int cbits = nbits(w_.zeros_acc);
      while (cbits-- > 0) out_.bit(1);
      out_.bit(0);
      while (w_.zeros_acc > 1) {
        out_.bit(w_.zeros_acc & 1u);
        w_.zeros_acc >>= 1;
      }
      w_.zeros_acc = 0;
    }
    if (w_.holding_one != 0) {
      if (w_.holding_one >= limit_ones) {
        out_.bits((1u << limit_ones) - 1u, limit_ones + 1);
        w_.holding_one -= limit_ones;
        int cbits = nbits(w_.holding_one);
        while (cbits-- > 0) out_.bit(1);
        out_.bit(0);
        while (w_.holding_one > 1) {
          out_.bit(w_.holding_one & 1u);
          w_.holding_one >>= 1;
        }
        w_.holding_zero = 0;
      } else {
        out_.bits((1u << w_.holding_one) - 1u, w_.holding_one);
      }
      w_.holding_one = 0;
    }
    if (w_.holding_zero != 0) {
      out_.bit(0);
      w_.holding_zero = 0;
    }
    for (const auto& [value, count] : pending_) out_.bits(value, count);
    pending_.clear();
  }

 private:
  void pend(std::uint32_t value, std::uint32_t count) {
    if (count > 0) pending_.push_back({value, count});
  }
  LsbWriter& out_;
  Words w_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> pending_;
};

// Encoder side of one decorrelation pass (inverse of the decoder's).
struct EncPass {
  std::int32_t term = 0;
  std::int32_t delta = 2;
  std::int32_t weight_a = 0, weight_b = 0;
  std::array<std::int32_t, max_term> samples_a{}, samples_b{};
};

void encode_pass_mono(EncPass& d, std::int32_t* buffer, std::size_t count) {
  if (d.term > static_cast<std::int32_t>(max_term)) {
    for (std::size_t i = 0; i < count; ++i) {
      const std::int32_t sam = d.term == 17 ? wrap(2ll * d.samples_a[0] - d.samples_a[1])
                                            : wrap((3ll * d.samples_a[0] - d.samples_a[1]) >> 1);
      d.samples_a[1] = d.samples_a[0];
      d.samples_a[0] = buffer[i];
      buffer[i] = wrap(static_cast<std::int64_t>(buffer[i]) - apply_weight(d.weight_a, sam));
      update_weight(d.weight_a, d.delta, sam, buffer[i]);
    }
    return;
  }
  std::size_t m = 0;
  std::size_t k = static_cast<std::size_t>(d.term) & (max_term - 1);
  for (std::size_t i = 0; i < count; ++i) {
    const std::int32_t sam = d.samples_a[m];
    d.samples_a[k] = buffer[i];
    buffer[i] = wrap(static_cast<std::int64_t>(buffer[i]) - apply_weight(d.weight_a, sam));
    update_weight(d.weight_a, d.delta, sam, buffer[i]);
    m = (m + 1) & (max_term - 1);
    k = (k + 1) & (max_term - 1);
  }
}

void encode_pass_stereo(EncPass& d, std::int32_t* buffer, std::size_t count) {
  std::int32_t* b = buffer;
  if (d.term > static_cast<std::int32_t>(max_term)) {
    for (std::size_t i = 0; i < count; ++i, b += 2) {
      std::int32_t sam = d.term == 17 ? wrap(2ll * d.samples_a[0] - d.samples_a[1])
                                      : wrap((3ll * d.samples_a[0] - d.samples_a[1]) >> 1);
      d.samples_a[1] = d.samples_a[0];
      d.samples_a[0] = b[0];
      b[0] = wrap(static_cast<std::int64_t>(b[0]) - apply_weight(d.weight_a, sam));
      update_weight(d.weight_a, d.delta, sam, b[0]);
      sam = d.term == 17 ? wrap(2ll * d.samples_b[0] - d.samples_b[1]) : wrap((3ll * d.samples_b[0] - d.samples_b[1]) >> 1);
      d.samples_b[1] = d.samples_b[0];
      d.samples_b[0] = b[1];
      b[1] = wrap(static_cast<std::int64_t>(b[1]) - apply_weight(d.weight_b, sam));
      update_weight(d.weight_b, d.delta, sam, b[1]);
    }
    return;
  }
  if (d.term > 0) {
    std::size_t m = 0;
    std::size_t k = static_cast<std::size_t>(d.term) & (max_term - 1);
    for (std::size_t i = 0; i < count; ++i, b += 2) {
      std::int32_t sam = d.samples_a[m];
      d.samples_a[k] = b[0];
      b[0] = wrap(static_cast<std::int64_t>(b[0]) - apply_weight(d.weight_a, sam));
      update_weight(d.weight_a, d.delta, sam, b[0]);
      sam = d.samples_b[m];
      d.samples_b[k] = b[1];
      b[1] = wrap(static_cast<std::int64_t>(b[1]) - apply_weight(d.weight_b, sam));
      update_weight(d.weight_b, d.delta, sam, b[1]);
      m = (m + 1) & (max_term - 1);
      k = (k + 1) & (max_term - 1);
    }
    return;
  }
  for (std::size_t i = 0; i < count; ++i, b += 2) {
    const std::int32_t out0 = b[0], out1 = b[1];
    if (d.term == -1) {
      b[0] = wrap(static_cast<std::int64_t>(out0) - apply_weight(d.weight_a, d.samples_a[0]));
      update_weight_clip(d.weight_a, d.delta, d.samples_a[0], b[0]);
      b[1] = wrap(static_cast<std::int64_t>(out1) - apply_weight(d.weight_b, out0));
      update_weight_clip(d.weight_b, d.delta, out0, b[1]);
      d.samples_a[0] = out1;
    } else if (d.term == -2) {
      b[1] = wrap(static_cast<std::int64_t>(out1) - apply_weight(d.weight_b, d.samples_b[0]));
      update_weight_clip(d.weight_b, d.delta, d.samples_b[0], b[1]);
      b[0] = wrap(static_cast<std::int64_t>(out0) - apply_weight(d.weight_a, out1));
      update_weight_clip(d.weight_a, d.delta, out1, b[0]);
      d.samples_b[0] = out0;
    } else {
      b[0] = wrap(static_cast<std::int64_t>(out0) - apply_weight(d.weight_a, d.samples_a[0]));
      update_weight_clip(d.weight_a, d.delta, d.samples_a[0], b[0]);
      b[1] = wrap(static_cast<std::int64_t>(out1) - apply_weight(d.weight_b, d.samples_b[0]));
      update_weight_clip(d.weight_b, d.delta, d.samples_b[0], b[1]);
      d.samples_b[0] = out0;
      d.samples_a[0] = out1;
    }
  }
}

void metadata(util::ByteWriter& out, std::uint8_t id, ByteView data) {
  const std::size_t words = (data.size() + 1) / 2;
  std::uint8_t header = id;
  if (data.size() % 2 == 1) header |= id_odd_size;
  if (words > 255) header |= id_large;
  out.u8(header);
  out.u8(static_cast<std::uint8_t>(words));
  if (words > 255) {
    out.u8(static_cast<std::uint8_t>(words >> 8));
    out.u8(static_cast<std::uint8_t>(words >> 16));
  }
  out.bytes(data);
  if (data.size() % 2 == 1) out.u8(0);
}

// Decorrelation terms in encoding order (the first is applied to the raw
// samples); the decoder applies them in reverse.
std::vector<std::int32_t> encoding_terms(int effort, bool stereo) {
  if (effort <= 2) return {17, 17};
  if (effort <= 6) return stereo ? std::vector<std::int32_t>{18, 18, 2, 3, -2} : std::vector<std::int32_t>{18, 18, 2, 3};
  return stereo ? std::vector<std::int32_t>{18, 18, 18, -2, 2, 3, 17, 4} : std::vector<std::int32_t>{18, 18, 18, 2, 3, 17, 4, 5};
}

// The same terms in the decoder's order.
std::vector<std::int32_t> terms_for(int effort, bool stereo) {
  std::vector<std::int32_t> terms = encoding_terms(effort, stereo);
  std::reverse(terms.begin(), terms.end());
  return terms;
}

}  // namespace

Result<void> BlockEncoder::configure(std::uint32_t sample_rate, std::uint16_t channels, int bits, int effort,
                                     std::uint32_t channel_mask) {
  if (channels == 0 || channels > 255) return fail(Status::InvalidArgument, "WavPack holds 1 to 255 channels");
  if (bits != 8 && bits != 16 && bits != 24) return fail(Status::InvalidArgument, "WavPack encoding supports 8, 16 or 24 bits");
  if (sample_rate == 0 || sample_rate > 0x7FFFFFFF) return fail(Status::InvalidArgument, "invalid sample rate");
  sample_rate_ = sample_rate;
  channels_ = channels;
  bits_ = bits;
  effort_ = std::clamp(effort, 0, 9);
  mask_ = channel_mask;
  // About half a second per block, as the reference encoder for mono/stereo.
  block_frames_ = std::clamp<std::uint32_t>(sample_rate / 2, 512, 131072);
  position_ = 0;
  groups_.clear();
  return {};
}

Result<Bytes> BlockEncoder::encode_block(std::span<const std::int32_t> interleaved, std::uint32_t frames) {
  util::ByteWriter out;
  std::uint32_t srate_index = 15;
  for (std::size_t i = 0; i < sample_rates.size(); ++i) {
    if (sample_rates[i] == sample_rate_) srate_index = static_cast<std::uint32_t>(i);
  }
  const auto bytes = static_cast<std::uint32_t>(bits_ / 8);
  for (std::uint32_t c0 = 0; c0 < channels_;) {
    const bool stereo = c0 + 1 < channels_;
    const std::size_t width = stereo ? 2 : 1;
    std::vector<std::int32_t> buffer(std::size_t{frames} * width);
    std::uint32_t crc = 0xFFFFFFFFu;
    std::uint32_t magnitude = 0;
    for (std::size_t i = 0; i < frames; ++i) {
      for (std::size_t c = 0; c < width; ++c) {
        const std::int32_t v = interleaved[i * channels_ + c0 + c];
        buffer[i * width + c] = v;
        crc = crc * 3u + static_cast<std::uint32_t>(v);
        magnitude |= static_cast<std::uint32_t>(v < 0 ? ~v : v);
      }
    }
    if (stereo) {
      for (std::size_t i = 0; i < frames; ++i) {
        const std::int32_t l = buffer[i * 2], r = buffer[i * 2 + 1];
        const std::int32_t a = l - r;
        buffer[i * 2] = a;
        buffer[i * 2 + 1] = r + (a >> 1);
      }
    }
    const std::vector<std::int32_t> terms = terms_for(effort_, stereo);
    const std::size_t group = c0 / 2;
    if (groups_.size() <= group) groups_.resize(group + 1);
    GroupState& state = groups_[group];
    if (!state.valid || state.weight_a.size() != terms.size()) {
      state = GroupState{};
      state.weight_a.assign(terms.size(), 0);
      state.weight_b.assign(terms.size(), 0);
      state.samples_a.assign(terms.size(), {});
      state.samples_b.assign(terms.size(), {});
      state.valid = true;
    }
    // Start from the state exactly as the decoder will restore it.
    std::vector<EncPass> passes(terms.size());
    util::ByteWriter wts, sm, ent;
    for (std::size_t i = 0; i < terms.size(); ++i) passes[i].term = terms[i];
    for (std::size_t i = passes.size(); i-- > 0;) {
      EncPass& p = passes[i];
      const std::int8_t wa = store_weight(state.weight_a[i]);
      const std::int8_t wb = store_weight(state.weight_b[i]);
      p.weight_a = restore_weight(wa);
      p.weight_b = restore_weight(wb);
      wts.i8(wa);
      if (stereo) wts.i8(wb);
      const auto put = [&](std::int32_t value, std::int32_t& target) {
        const std::int32_t stored = wp_log2s(value);
        sm.i16le(static_cast<std::int16_t>(stored));
        target = wp_exp2s(stored);
      };
      if (p.term > static_cast<std::int32_t>(max_term)) {
        put(state.samples_a[i][0], p.samples_a[0]);
        put(state.samples_a[i][1], p.samples_a[1]);
        if (stereo) {
          put(state.samples_b[i][0], p.samples_b[0]);
          put(state.samples_b[i][1], p.samples_b[1]);
        }
      } else if (p.term < 0) {
        put(state.samples_a[i][0], p.samples_a[0]);
        put(state.samples_b[i][0], p.samples_b[0]);
      } else {
        for (std::int32_t k = 0; k < p.term; ++k) {
          put(state.samples_a[i][static_cast<std::size_t>(k)], p.samples_a[static_cast<std::size_t>(k)]);
          if (stereo) put(state.samples_b[i][static_cast<std::size_t>(k)], p.samples_b[static_cast<std::size_t>(k)]);
        }
      }
    }
    // The decoder applies pass 0 first; undo in reverse order.
    for (std::size_t i = passes.size(); i-- > 0;) {
      if (stereo) encode_pass_stereo(passes[i], buffer.data(), frames);
      else encode_pass_mono(passes[i], buffer.data(), frames);
    }
    LsbWriter bits;
    WordEncoder words(bits);
    for (std::size_t c = 0; c < width; ++c) {
      for (int k = 0; k < 3; ++k) {
        const std::int32_t stored = wp_log2(state.medians[c * 3 + static_cast<std::size_t>(k)]);
        ent.u16le(static_cast<std::uint16_t>(stored));
        words.set_median(static_cast<int>(c), k, static_cast<std::uint32_t>(wp_exp2s(stored)));
      }
    }
    for (std::size_t i = 0; i < frames; ++i) {
      for (std::size_t c = 0; c < width; ++c) words.send(buffer[i * width + c], static_cast<int>(c));
    }
    words.flush();
    const Bytes stream = bits.finish();
    // Save the end state for the next block (sample history oldest first).
    for (std::size_t i = 0; i < passes.size(); ++i) {
      const EncPass& p = passes[i];
      state.weight_a[i] = p.weight_a;
      state.weight_b[i] = p.weight_b;
      if (p.term > 0 && p.term <= static_cast<std::int32_t>(max_term)) {
        for (std::size_t j = 0; j < max_term; ++j) {
          state.samples_a[i][j] = p.samples_a[(frames + j) & (max_term - 1)];
          state.samples_b[i][j] = p.samples_b[(frames + j) & (max_term - 1)];
        }
      } else {
        state.samples_a[i] = p.samples_a;
        state.samples_b[i] = p.samples_b;
      }
    }
    for (std::size_t c = 0; c < width; ++c) {
      for (int k = 0; k < 3; ++k) state.medians[c * 3 + static_cast<std::size_t>(k)] = words.median(static_cast<int>(c), k);
    }

    util::ByteWriter body;
    {
      util::ByteWriter t;
      for (std::size_t i = passes.size(); i-- > 0;) t.u8(static_cast<std::uint8_t>(((passes[i].term + 5) & 0x1F) | (passes[i].delta << 5)));
      metadata(body, id_decorr_terms, t.view());
      metadata(body, id_decorr_weights, wts.view());
      metadata(body, id_decorr_samples, sm.view());
      metadata(body, id_entropy_vars, ent.view());
    }
    if (c0 == 0 && (channels_ > 2 || (mask_ != 0 && mask_ != (channels_ == 1 ? 4u : 3u)))) {
      util::ByteWriter info;
      info.u8(static_cast<std::uint8_t>(channels_));
      info.u32le(mask_);
      metadata(body, id_channel_info, info.view());
    }
    if (srate_index == 15) {
      util::ByteWriter rate;
      rate.u8(static_cast<std::uint8_t>(sample_rate_));
      rate.u8(static_cast<std::uint8_t>(sample_rate_ >> 8));
      rate.u8(static_cast<std::uint8_t>(sample_rate_ >> 16));
      metadata(body, id_sample_rate, rate.view());
    }
    metadata(body, id_wv_bitstream, stream);

    std::uint32_t flags = (bytes - 1) | (stereo ? joint_stereo : mono_flag);
    if (c0 == 0) flags |= initial_block;
    if (c0 + width >= channels_) flags |= final_block;
    flags |= (static_cast<std::uint32_t>(std::min(nbits(magnitude), 31)) << mag_lsb) & (0x1Fu << mag_lsb);
    flags |= srate_index << srate_lsb;
    const std::uint64_t total = total_ != 0 ? total_ : 0xFFFFFFFFFFull;
    out.text("wvpk");
    out.u32le(static_cast<std::uint32_t>(body.size() + 24));
    out.u16le(0x407);
    out.u8(static_cast<std::uint8_t>(position_ >> 32));
    out.u8(static_cast<std::uint8_t>(total >> 32));
    out.u32le(static_cast<std::uint32_t>(total));
    out.u32le(static_cast<std::uint32_t>(position_));
    out.u32le(frames);
    out.u32le(flags);
    out.u32le(crc);
    out.bytes(body.view());
    c0 += static_cast<std::uint32_t>(width);
  }
  position_ += frames;
  return out.take();
}

}  // namespace cvt::audio::wavpack
