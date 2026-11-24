// Convertible core - image_modern family: JPEG-LS (ITU-T T.87 / ISO 14495-1).
//
// Reader and writer of the baseline LOCO-I algorithm: regular and run mode,
// lossless and near-lossless (NEAR), 2..16 bits per sample, 1..4
// components (grey, grey + alpha, RGB, RGBA), interleave modes none / line /
// sample, default and custom (LSE) thresholds and RESET, oversize
// dimensions (LSE id 4). Not supported: restart markers, mapping tables
// (palettised JPEG-LS), point transform, subsampled components and the HP
// colour transforms.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/misc.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern {

namespace {

using image::ImageCodecSpec;

constexpr std::array<int, 32> run_j = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,  2,  3,  3,  3,  3,
                                        4, 4, 5, 5, 6, 6, 7, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// --------------------------------------------------------------------------- parameters

struct Params {
  int bits = 8;
  int maxval = 255;
  int near = 0;
  int t1 = 0;
  int t2 = 0;
  int t3 = 0;
  int reset = 64;
  // Derived.
  int range = 0;
  int qbpp = 0;
  int limit = 0;
};

int ceil_log2(int v) {
  int n = 0;
  while ((1 << n) < v) ++n;
  return n;
}

/// The default thresholds of T.87 C.2.4.1.1.
void default_thresholds(int maxval, int near, int& t1, int& t2, int& t3) {
  const auto clamp = [maxval](int i, int j) { return (i > maxval || i < j) ? j : i; };
  if (maxval >= 128) {
    const int factor = (std::min(maxval, 4095) + 128) / 256;
    t1 = clamp(factor * (3 - 2) + 2 + 3 * near, near + 1);
    t2 = clamp(factor * (7 - 3) + 3 + 5 * near, t1);
    t3 = clamp(factor * (21 - 4) + 4 + 7 * near, t2);
  } else {
    const int factor = 256 / (maxval + 1);
    t1 = clamp(std::max(2, 3 / factor + 3 * near), near + 1);
    t2 = clamp(std::max(3, 7 / factor + 5 * near), t1);
    t3 = clamp(std::max(4, 21 / factor + 7 * near), t2);
  }
}

/// Fill the derived fields. `custom_*` = 0 means "default".
Result<void> derive(Params& p, int custom_t1, int custom_t2, int custom_t3) {
  if (p.maxval < 1 || p.near < 0 || p.near > std::min(255, p.maxval / 2)) {
    return fail(Status::Decode, "JPEG-LS NEAR value is invalid");
  }
  p.range = (p.maxval + 2 * p.near) / (2 * p.near + 1) + 1;
  p.qbpp = ceil_log2(p.range);
  const int bpp = std::max(2, ceil_log2(p.maxval + 1));
  p.limit = 2 * (bpp + std::max(8, bpp));
  int t1 = 0;
  int t2 = 0;
  int t3 = 0;
  default_thresholds(p.maxval, p.near, t1, t2, t3);
  p.t1 = custom_t1 != 0 ? custom_t1 : t1;
  p.t2 = custom_t2 != 0 ? custom_t2 : t2;
  p.t3 = custom_t3 != 0 ? custom_t3 : t3;
  if (!(p.t1 >= p.near + 1 && p.t1 <= p.maxval && p.t2 >= p.t1 && p.t2 <= p.maxval && p.t3 >= p.t2 &&
        p.t3 <= p.maxval)) {
    return fail(Status::Decode, "JPEG-LS thresholds are invalid");
  }
  if (p.reset < 3 || p.reset > std::max(255, p.maxval)) return fail(Status::Decode, "JPEG-LS RESET value is invalid");
  return {};
}

// --------------------------------------------------------------------------- context state

struct RegularContext {
  int a = 0;
  int b = 0;
  int c = 0;
  int n = 1;
};

struct RunContext {
  int a = 0;
  int n = 1;
  int nn = 0;
  int type = 0;
};

struct State {
  Params p;
  std::array<RegularContext, 365> regular{};
  std::array<RunContext, 2> run{};

  void reset_contexts() {
    const int a = std::max(2, (p.range + 32) / 64);
    for (RegularContext& ctx : regular) ctx = {a, 0, 0, 1};
    run[0] = {a, 1, 0, 0};
    run[1] = {a, 1, 0, 1};
  }

  [[nodiscard]] int quantize(int d) const noexcept {
    if (d <= -p.t3) return -4;
    if (d <= -p.t2) return -3;
    if (d <= -p.t1) return -2;
    if (d < -p.near) return -1;
    if (d <= p.near) return 0;
    if (d < p.t1) return 1;
    if (d < p.t2) return 2;
    if (d < p.t3) return 3;
    return 4;
  }
  [[nodiscard]] int context_id(int ra, int rb, int rc, int rd) const noexcept {
    return (quantize(rd - rb) * 9 + quantize(rb - rc)) * 9 + quantize(rc - ra);
  }
  [[nodiscard]] int clamp_sample(int v) const noexcept { return std::clamp(v, 0, p.maxval); }
  /// Reconstruct from a prediction and a (sign-applied) quantised error.
  [[nodiscard]] int reconstruct(int predicted, int error) const noexcept {
    int v = predicted + error * (2 * p.near + 1);
    if (v < -p.near) {
      v += p.range * (2 * p.near + 1);
    } else if (v > p.maxval + p.near) {
      v -= p.range * (2 * p.near + 1);
    }
    return clamp_sample(v);
  }
  /// Quantise and modulo-reduce a prediction error (encoder side).
  [[nodiscard]] int reduce_error(int e) const noexcept {
    if (p.near > 0) e = e > 0 ? (e + p.near) / (2 * p.near + 1) : -(p.near - e) / (2 * p.near + 1);
    if (e < 0) e += p.range;
    if (e >= (p.range + 1) / 2) e -= p.range;
    return e;
  }
  void update_regular(RegularContext& ctx, int error) const noexcept {
    ctx.a += std::abs(error);
    ctx.b += error * (2 * p.near + 1);
    if (ctx.n == p.reset) {
      ctx.a >>= 1;
      ctx.b >>= 1;
      ctx.n >>= 1;
    }
    ++ctx.n;
    if (ctx.b + ctx.n <= 0) {
      ctx.b += ctx.n;
      if (ctx.b <= -ctx.n) ctx.b = -ctx.n + 1;
      if (ctx.c > -128) --ctx.c;
    } else if (ctx.b > 0) {
      ctx.b -= ctx.n;
      if (ctx.b > 0) ctx.b = 0;
      if (ctx.c < 127) ++ctx.c;
    }
  }
  void update_run(RunContext& ctx, int error, int mapped) const noexcept {
    if (error < 0) ++ctx.nn;
    ctx.a += (mapped + 1 - ctx.type) >> 1;
    if (ctx.n == p.reset) {
      ctx.a >>= 1;
      ctx.n >>= 1;
      ctx.nn >>= 1;
    }
    ++ctx.n;
  }
};

int golomb_k(const RegularContext& ctx) {
  int k = 0;
  while ((ctx.n << k) < ctx.a && k < 24) ++k;
  return k;
}
int golomb_k(const RunContext& ctx) {
  const int temp = ctx.a + (ctx.n >> 1) * ctx.type;
  int k = 0;
  while ((ctx.n << k) < temp && k < 24) ++k;
  return k;
}

int predict(int ra, int rb, int rc) {
  if (rc >= std::max(ra, rb)) return std::min(ra, rb);
  if (rc <= std::min(ra, rb)) return std::max(ra, rb);
  return ra + rb - rc;
}

// --------------------------------------------------------------------------- bit I/O

/// Reads the entropy-coded segment of one scan (bit stuffing after 0xFF).
class BitReader {
 public:
  BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  [[nodiscard]] std::uint32_t bits(int n) {
    if (n == 0) return 0;
    if (count_ < n) fill();
    const std::uint32_t v = static_cast<std::uint32_t>(cache_ >> (64 - n));
    cache_ <<= n;
    count_ -= n;
    return v;
  }
  [[nodiscard]] bool bit() { return bits(1) != 0; }
  /// Number of 0 bits before the next 1 bit (which is consumed); -1 when
  /// more than `max` zeros are found.
  [[nodiscard]] int zeros(int max) {
    int total = 0;
    while (true) {
      if (count_ == 0) fill();
      // The bits below the `count_` valid ones are always zero.
      if (cache_ == 0) {
        total += count_;
        count_ = 0;
        if (total > max || overrun()) return -1;
        continue;
      }
      const int lz = std::countl_zero(cache_);
      total += lz;
      cache_ = lz + 1 >= 64 ? 0 : cache_ << (lz + 1);
      count_ -= lz + 1;
      return total > max ? -1 : total;
    }
  }
  /// True when far more bits were requested than the data held.
  [[nodiscard]] bool overrun() const noexcept { return padding_ > 64; }

 private:
  void fill() {
    while (count_ <= 56) {
      std::uint32_t value = 0;
      int n = 8;
      if (pos_ >= size_) {
        ++padding_;
      } else {
        value = data_[pos_++];
        if (previous_ff_) {
          n = 7;
          value &= 0x7F;
        }
        previous_ff_ = value == 0xFF && n == 8;
      }
      cache_ |= static_cast<std::uint64_t>(value) << (64 - count_ - n);
      count_ += n;
    }
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  std::uint64_t cache_ = 0;
  int count_ = 0;
  bool previous_ff_ = false;
  std::size_t padding_ = 0;
};

class BitWriter {
 public:
  /// Append the `n` (<= 32) low bits of `value`, most significant first.
  void put(std::uint32_t value, int n) {
    if (n == 0) return;
    acc_ = acc_ << n | (value & (n == 32 ? 0xFFFFFFFFu : (1u << n) - 1u));
    bits_ += n;
    while (true) {
      const int take = previous_ff_ ? 7 : 8;  // a byte after 0xFF carries 7 bits
      if (bits_ < take) break;
      const auto byte = static_cast<std::uint8_t>((acc_ >> (bits_ - take)) & ((1u << take) - 1u));
      bits_ -= take;
      out_.push_back(static_cast<std::byte>(byte));
      previous_ff_ = byte == 0xFF;
    }
  }
  void zeros(int n) {
    while (n > 0) {
      const int chunk = std::min(n, 31);
      put(0, chunk);
      n -= chunk;
    }
  }
  /// Pad the last byte with zero bits; a trailing 0xFF gets its stuffed byte.
  void finish() {
    if (bits_ > 0) put(0, (previous_ff_ ? 7 : 8) - bits_);
    if (previous_ff_) out_.push_back(std::byte{0});
    previous_ff_ = false;
  }
  [[nodiscard]] Bytes& bytes() noexcept { return out_; }

 private:
  Bytes out_;
  std::uint64_t acc_ = 0;
  int bits_ = 0;
  bool previous_ff_ = false;
};

// --------------------------------------------------------------------------- line coding

/// One component's line pair: index 0 is the left border (x = -1).
struct Lines {
  std::vector<int> previous;
  std::vector<int> current;
  int run_index = 0;
  void init(std::uint32_t width) {
    previous.assign(std::size_t{width} + 2, 0);
    current.assign(std::size_t{width} + 2, 0);
  }
  void start_line(std::uint32_t width) {
    std::swap(previous, current);
    // previous[width + 1] (Rd past the end) = Rb of the last sample; Ra of
    // the first sample = Rb; Rc of the first sample = the previous line's Ra.
    previous[std::size_t{width} + 1] = previous[width];
    current[0] = previous[1];
  }
};

class Decoder {
 public:
  Decoder(State& state, BitReader& reader) : s_(state), r_(reader) {}

  Result<void> line(Lines& l, std::uint32_t width) {
    std::size_t x = 1;
    while (x <= width) {
      const int ra = l.current[x - 1];
      const int rb = l.previous[x];
      const int rc = l.previous[x - 1];
      const int rd = l.previous[x + 1];
      const int q = s_.context_id(ra, rb, rc, rd);
      if (q == 0) {
        CVT_TRY_ASSIGN(const std::size_t done, run(l, x, width));
        x += done;
      } else {
        CVT_TRY_ASSIGN(l.current[x], regular(q, predict(ra, rb, rc)));
        ++x;
      }
    }
    return {};
  }

  /// Sample-interleaved line of `lines.size()` components sharing contexts.
  Result<void> line_interleaved(std::vector<Lines>& lines, std::uint32_t width) {
    const std::size_t n = lines.size();
    std::size_t x = 1;
    std::array<int, 4> q{};
    while (x <= width) {
      bool all_zero = true;
      for (std::size_t c = 0; c < n; ++c) {
        const Lines& l = lines[c];
        q[c] = s_.context_id(l.current[x - 1], l.previous[x], l.previous[x - 1], l.previous[x + 1]);
        all_zero = all_zero && q[c] == 0;
      }
      if (all_zero) {
        CVT_TRY_ASSIGN(const std::size_t done, run_interleaved(lines, x, width));
        x += done;
        continue;
      }
      for (std::size_t c = 0; c < n; ++c) {
        Lines& l = lines[c];
        CVT_TRY_ASSIGN(l.current[x], regular(q[c], predict(l.current[x - 1], l.previous[x], l.previous[x - 1])));
      }
      ++x;
    }
    return {};
  }

 private:
  Result<int> value(int k, int limit) {
    const int high = r_.zeros(limit);
    if (high < 0 || r_.overrun()) return fail(Status::Decode, "JPEG-LS data is corrupt or truncated");
    if (high >= limit - (s_.p.qbpp + 1)) return static_cast<int>(r_.bits(s_.p.qbpp)) + 1;
    if (k == 0) return high;
    return (high << k) + static_cast<int>(r_.bits(k));
  }

  Result<int> regular(int q, int predicted) {
    const int sign = q < 0 ? -1 : 1;
    RegularContext& ctx = s_.regular[static_cast<std::size_t>(q * sign)];
    const int k = golomb_k(ctx);
    const int px = s_.clamp_sample(predicted + sign * ctx.c);
    CVT_TRY_ASSIGN(const int mapped, value(k, s_.p.limit));
    int error = (mapped >> 1) ^ -(mapped & 1);
    if (k == 0 && s_.p.near == 0 && 2 * ctx.b + ctx.n - 1 < 0) error = ~error;
    if (std::abs(error) > 65535) return fail(Status::Decode, "JPEG-LS data is corrupt");
    s_.update_regular(ctx, error);
    return s_.reconstruct(px, sign * error);
  }

  Result<int> run_error(RunContext& ctx, int run_index) {
    const int k = golomb_k(ctx);
    CVT_TRY_ASSIGN(const int mapped, value(k, s_.p.limit - run_j[static_cast<std::size_t>(run_index)] - 1));
    const int temp = mapped + ctx.type;
    const bool map = (temp & 1) != 0;
    const int magnitude = (temp + (map ? 1 : 0)) / 2;
    const int error = ((k != 0 || 2 * ctx.nn >= ctx.n) == map) ? -magnitude : magnitude;
    s_.update_run(ctx, error, mapped);
    return error;
  }

  /// Run length of the current run (bits read; values are written by the caller).
  Result<std::size_t> run_length(int& run_index, std::size_t available) {
    std::size_t count = 0;
    while (count < available) {
      if (r_.overrun()) return fail(Status::Decode, "JPEG-LS data is truncated");
      if (!r_.bit()) {
        count += r_.bits(run_j[static_cast<std::size_t>(run_index)]);
        if (count > available) return fail(Status::Decode, "JPEG-LS run is too long");
        return count;
      }
      const std::size_t step = std::size_t{1} << run_j[static_cast<std::size_t>(run_index)];
      const std::size_t taken = std::min(step, available - count);
      count += taken;
      if (taken == step && run_index < 31) ++run_index;
    }
    return count;
  }

  Result<std::size_t> run(Lines& l, std::size_t x, std::uint32_t width) {
    const int ra = l.current[x - 1];
    const std::size_t available = std::size_t{width} + 1 - x;
    CVT_TRY_ASSIGN(const std::size_t length, run_length(l.run_index, available));
    for (std::size_t i = 0; i < length; ++i) l.current[x + i] = ra;
    if (length == available) return length;
    const std::size_t end = x + length;
    const int rb = l.previous[end];
    if (std::abs(ra - rb) <= s_.p.near) {
      CVT_TRY_ASSIGN(const int error, run_error(s_.run[1], l.run_index));
      l.current[end] = s_.reconstruct(ra, error);
    } else {
      CVT_TRY_ASSIGN(const int error, run_error(s_.run[0], l.run_index));
      l.current[end] = s_.reconstruct(rb, error * (rb > ra ? 1 : -1));
    }
    if (l.run_index > 0) --l.run_index;
    return length + 1;
  }

  Result<std::size_t> run_interleaved(std::vector<Lines>& lines, std::size_t x, std::uint32_t width) {
    const std::size_t available = std::size_t{width} + 1 - x;
    int& run_index = lines[0].run_index;
    CVT_TRY_ASSIGN(const std::size_t length, run_length(run_index, available));
    for (Lines& l : lines) {
      const int ra = l.current[x - 1];
      for (std::size_t i = 0; i < length; ++i) l.current[x + i] = ra;
    }
    if (length == available) return length;
    const std::size_t end = x + length;
    for (Lines& l : lines) {
      const int ra = l.current[end - 1];
      const int rb = l.previous[end];
      CVT_TRY_ASSIGN(const int error, run_error(s_.run[0], run_index));
      l.current[end] = s_.reconstruct(rb, error * (rb >= ra ? 1 : -1));
    }
    if (run_index > 0) --run_index;
    return length + 1;
  }

  State& s_;
  BitReader& r_;
};

class Encoder {
 public:
  Encoder(State& state, BitWriter& writer) : s_(state), w_(writer) {}

  /// `samples(x)` gives the source value of column x (0-based).
  template <class Sample>
  void line(Lines& l, std::uint32_t width, Sample samples) {
    std::size_t x = 1;
    while (x <= width) {
      const int ra = l.current[x - 1];
      const int rb = l.previous[x];
      const int rc = l.previous[x - 1];
      const int rd = l.previous[x + 1];
      const int q = s_.context_id(ra, rb, rc, rd);
      if (q == 0) {
        x += run(l, x, width, samples);
      } else {
        l.current[x] = regular(q, predict(ra, rb, rc), samples(x - 1));
        ++x;
      }
    }
  }

  template <class Sample>
  void line_interleaved(std::vector<Lines>& lines, std::uint32_t width, Sample samples) {
    const std::size_t n = lines.size();
    std::size_t x = 1;
    std::array<int, 4> q{};
    while (x <= width) {
      bool all_zero = true;
      for (std::size_t c = 0; c < n; ++c) {
        const Lines& l = lines[c];
        q[c] = s_.context_id(l.current[x - 1], l.previous[x], l.previous[x - 1], l.previous[x + 1]);
        all_zero = all_zero && q[c] == 0;
      }
      if (all_zero) {
        x += run_interleaved(lines, x, width, samples);
        continue;
      }
      for (std::size_t c = 0; c < n; ++c) {
        Lines& l = lines[c];
        l.current[x] = regular(q[c], predict(l.current[x - 1], l.previous[x], l.previous[x - 1]), samples(c, x - 1));
      }
      ++x;
    }
  }

 private:
  void mapped_value(int k, int mapped, int limit) {
    int high = mapped >> k;
    if (high < limit - s_.p.qbpp - 1) {
      w_.zeros(high);
      w_.put(1, 1);
      if (k > 0) w_.put(static_cast<std::uint32_t>(mapped) & ((1u << k) - 1u), k);
      return;
    }
    w_.zeros(limit - s_.p.qbpp - 1);
    w_.put(1, 1);
    w_.put(static_cast<std::uint32_t>(mapped - 1) & ((1u << s_.p.qbpp) - 1u), s_.p.qbpp);
  }

  int regular(int q, int predicted, int x) {
    const int sign = q < 0 ? -1 : 1;
    RegularContext& ctx = s_.regular[static_cast<std::size_t>(q * sign)];
    const int k = golomb_k(ctx);
    const int px = s_.clamp_sample(predicted + sign * ctx.c);
    const int error = s_.reduce_error(sign * (x - px));
    int corrected = error;
    if (k == 0 && s_.p.near == 0 && 2 * ctx.b + ctx.n - 1 < 0) corrected = ~corrected;
    mapped_value(k, corrected >= 0 ? 2 * corrected : -2 * corrected - 1, s_.p.limit);
    s_.update_regular(ctx, error);
    return s_.reconstruct(px, sign * error);
  }

  void run_error(RunContext& ctx, int error, int run_index) {
    const int k = golomb_k(ctx);
    bool map = false;
    if (k == 0 && error > 0 && 2 * ctx.nn < ctx.n) map = true;
    if (error < 0 && 2 * ctx.nn >= ctx.n) map = true;
    if (error < 0 && k != 0) map = true;
    const int mapped = 2 * std::abs(error) - ctx.type - (map ? 1 : 0);
    mapped_value(k, mapped, s_.p.limit - run_j[static_cast<std::size_t>(run_index)] - 1);
    s_.update_run(ctx, error, mapped);
  }

  /// Encode a run of `length` samples (end_of_line: the run reached the end).
  void run_length(int& run_index, std::size_t length, bool end_of_line) {
    while (length >= (std::size_t{1} << run_j[static_cast<std::size_t>(run_index)])) {
      w_.put(1, 1);
      length -= std::size_t{1} << run_j[static_cast<std::size_t>(run_index)];
      if (run_index < 31) ++run_index;
    }
    if (end_of_line) {
      if (length != 0) w_.put(1, 1);
      return;
    }
    w_.put(0, 1);
    w_.put(static_cast<std::uint32_t>(length), run_j[static_cast<std::size_t>(run_index)]);
  }

  template <class Sample>
  std::size_t run(Lines& l, std::size_t x, std::uint32_t width, Sample samples) {
    const int ra = l.current[x - 1];
    std::size_t length = 0;
    while (x + length <= width && std::abs(samples(x + length - 1) - ra) <= s_.p.near) {
      l.current[x + length] = ra;
      ++length;
    }
    const bool end_of_line = x + length > width;
    run_length(l.run_index, length, end_of_line);
    if (end_of_line) return length;
    const std::size_t end = x + length;
    const int value = samples(end - 1);
    const int rb = l.previous[end];
    if (std::abs(ra - rb) <= s_.p.near) {
      const int error = s_.reduce_error(value - ra);
      run_error(s_.run[1], error, l.run_index);
      l.current[end] = s_.reconstruct(ra, error);
    } else {
      const int sign = rb > ra ? 1 : -1;
      const int error = s_.reduce_error(sign * (value - rb));
      run_error(s_.run[0], error, l.run_index);
      l.current[end] = s_.reconstruct(rb, error * sign);
    }
    if (l.run_index > 0) --l.run_index;
    return length + 1;
  }

  template <class Sample>
  std::size_t run_interleaved(std::vector<Lines>& lines, std::size_t x, std::uint32_t width, Sample samples) {
    std::size_t length = 0;
    while (x + length <= width) {
      bool same = true;
      for (std::size_t c = 0; c < lines.size() && same; ++c) {
        same = std::abs(samples(c, x + length - 1) - lines[c].current[x - 1]) <= s_.p.near;
      }
      if (!same) break;
      for (Lines& l : lines) l.current[x + length] = l.current[x - 1];
      ++length;
    }
    int& run_index = lines[0].run_index;
    const bool end_of_line = x + length > width;
    run_length(run_index, length, end_of_line);
    if (end_of_line) return length;
    const std::size_t end = x + length;
    for (std::size_t c = 0; c < lines.size(); ++c) {
      Lines& l = lines[c];
      const int ra = l.current[end - 1];
      const int rb = l.previous[end];
      const int sign = rb >= ra ? 1 : -1;
      const int error = s_.reduce_error(sign * (samples(c, end - 1) - rb));
      run_error(s_.run[0], error, run_index);
      l.current[end] = s_.reconstruct(rb, error * sign);
    }
    if (run_index > 0) --run_index;
    return length + 1;
  }

  State& s_;
  BitWriter& w_;
};

// --------------------------------------------------------------------------- reading

struct Frame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int bits = 0;
  std::vector<int> component_ids;
};

/// Find the end of an entropy-coded segment: the first 0xFF followed by a
/// byte with its top bit set (a marker).
std::size_t scan_end(ByteView data, std::size_t start) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  for (std::size_t i = start; i + 1 < data.size(); ++i) {
    if (p[i] == 0xFF && p[i + 1] >= 0x80) return i;
  }
  return data.size();
}

Result<ir::Image> read_jls(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (r.u16be() != 0xFFD8) return fail(Status::Decode, "not a JPEG-LS file");
  Frame frame;
  Params preset;
  preset.maxval = 0;  // 0 = default (from the sample size)
  preset.reset = 0;
  int custom_t1 = 0;
  int custom_t2 = 0;
  int custom_t3 = 0;
  bool have_frame = false;
  std::vector<std::vector<std::uint16_t>> planes;  // one per component, width*height
  std::vector<bool> decoded;
  std::uint32_t oversize_w = 0;
  std::uint32_t oversize_h = 0;
  bool done = false;
  while (!done) {
    // Skip fill bytes and find the next marker.
    std::uint8_t b = r.u8();
    if (!r.ok()) break;
    if (b != 0xFF) return fail(Status::Decode, "JPEG-LS marker expected");
    std::uint8_t marker = r.u8();
    while (marker == 0xFF && r.ok()) marker = r.u8();
    if (!r.ok()) break;
    if (marker == 0xD9) break;  // EOI
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    const std::uint16_t length = r.u16be();
    if (!r.ok() || length < 2 || length - 2u > r.remaining()) return fail(Status::Decode, "JPEG-LS segment is truncated");
    util::ByteReader seg(r.bytes(length - 2u));
    switch (marker) {
      case 0xF7: {  // SOF55
        if (have_frame) return fail(Status::Decode, "JPEG-LS file has two frame headers");
        frame.bits = seg.u8();
        frame.height = seg.u16be();
        frame.width = seg.u16be();
        const int count = seg.u8();
        if (!seg.ok() || count < 1 || count > 4 || frame.bits < 2 || frame.bits > 16) {
          return fail(Status::UnsupportedConversion, "JPEG-LS frame layout is not supported");
        }
        for (int i = 0; i < count; ++i) {
          const int id = seg.u8();
          const int sampling = seg.u8();
          (void)seg.u8();
          if (!seg.ok()) return fail(Status::Decode, "JPEG-LS frame header is truncated");
          if (sampling != 0x11) return fail(Status::UnsupportedConversion, "subsampled JPEG-LS components are not supported");
          frame.component_ids.push_back(id);
        }
        if (frame.width == 0 && oversize_w != 0) frame.width = oversize_w;
        if (frame.height == 0 && oversize_h != 0) frame.height = oversize_h;
        if (frame.height == 0) return fail(Status::UnsupportedConversion, "JPEG-LS files with a DNL height are not supported");
        CVT_TRY(image::check_size(ctx, frame.width, frame.height));
        // Every line of every component costs at least one bit.
        if (std::uint64_t{frame.height} * static_cast<std::uint64_t>(count) > std::uint64_t{ctx.input.size()} * 8) {
          return fail(Status::Decode, "JPEG-LS image is larger than its data allows");
        }
        if (std::uint64_t{frame.width} * frame.height * frame.component_ids.size() * 2 >
            std::numeric_limits<std::size_t>::max() / 2) {
          return fail(Status::Limit, "the JPEG-LS image is too large for this platform");
        }
        planes.assign(frame.component_ids.size(), std::vector<std::uint16_t>(std::size_t{frame.width} * frame.height));
        decoded.assign(frame.component_ids.size(), false);
        have_frame = true;
        break;
      }
      case 0xF8: {  // LSE
        const int id = seg.u8();
        if (id == 1) {
          const int maxval = seg.u16be();
          custom_t1 = seg.u16be();
          custom_t2 = seg.u16be();
          custom_t3 = seg.u16be();
          const int reset = seg.u16be();
          if (!seg.ok()) return fail(Status::Decode, "JPEG-LS preset parameters are truncated");
          preset.maxval = maxval;  // 0 = default
          preset.reset = reset;    // 0 = default
        } else if (id == 4) {
          const int wxy = seg.u8();
          if (wxy < 2 || wxy > 4) return fail(Status::Decode, "JPEG-LS oversize header is invalid");
          for (std::uint32_t* v : {&oversize_h, &oversize_w}) {
            std::uint32_t value = 0;
            for (int i = 0; i < wxy; ++i) value = value << 8 | seg.u8();
            *v = value;
          }
          if (!seg.ok()) return fail(Status::Decode, "JPEG-LS oversize header is truncated");
          if (have_frame) return fail(Status::Decode, "JPEG-LS oversize header after the frame header");
        } else {
          return fail(Status::UnsupportedConversion, "JPEG-LS mapping tables are not supported");
        }
        break;
      }
      case 0xDD: {  // DRI
        std::uint32_t interval = 0;
        while (seg.remaining() > 0) interval = interval << 8 | seg.u8();
        if (interval != 0) return fail(Status::UnsupportedConversion, "JPEG-LS restart markers are not supported");
        break;
      }
      case 0xDA: {  // SOS
        if (!have_frame) return fail(Status::Decode, "JPEG-LS scan before the frame header");
        const int count = seg.u8();
        std::vector<std::size_t> components;
        for (int i = 0; i < count; ++i) {
          const int id = seg.u8();
          const int table = seg.u8();
          if (table != 0) return fail(Status::UnsupportedConversion, "JPEG-LS mapping tables are not supported");
          const auto it = std::find(frame.component_ids.begin(), frame.component_ids.end(), id);
          if (it == frame.component_ids.end()) return fail(Status::Decode, "JPEG-LS scan names an unknown component");
          const auto index = static_cast<std::size_t>(it - frame.component_ids.begin());
          if (decoded[index]) return fail(Status::Decode, "JPEG-LS component is coded twice");
          components.push_back(index);
        }
        const int near = seg.u8();
        const int ilv = seg.u8();
        const int transform = seg.u8();
        if (!seg.ok() || count < 1) return fail(Status::Decode, "JPEG-LS scan header is truncated");
        if ((transform & 0x0F) != 0) return fail(Status::UnsupportedConversion, "JPEG-LS point transform is not supported");
        if (ilv > 2 || (count == 1 && ilv != 0) || (count > 1 && ilv == 0)) {
          return fail(Status::Decode, "JPEG-LS interleave mode is invalid");
        }
        State state;
        state.p.bits = frame.bits;
        state.p.maxval = preset.maxval != 0 ? preset.maxval : (1 << frame.bits) - 1;
        if (state.p.maxval >= (1 << frame.bits)) return fail(Status::Decode, "JPEG-LS MAXVAL exceeds the sample size");
        state.p.near = near;
        state.p.reset = preset.reset != 0 ? preset.reset : 64;
        CVT_TRY(derive(state.p, custom_t1, custom_t2, custom_t3));
        state.reset_contexts();

        const std::size_t start = r.position();
        const std::size_t end = scan_end(ctx.input, start);
        BitReader bits(reinterpret_cast<const std::uint8_t*>(ctx.input.data()) + start, end - start);
        Decoder decoder(state, bits);
        std::vector<Lines> lines(components.size());
        for (Lines& l : lines) l.init(frame.width);
        for (std::uint32_t y = 0; y < frame.height; ++y) {
          if ((y & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / frame.height));
          for (Lines& l : lines) l.start_line(frame.width);
          if (ilv == 2) {
            CVT_TRY(decoder.line_interleaved(lines, frame.width));
          } else {
            for (Lines& l : lines) CVT_TRY(decoder.line(l, frame.width));
          }
          for (std::size_t c = 0; c < components.size(); ++c) {
            std::uint16_t* row = planes[components[c]].data() + std::size_t{y} * frame.width;
            for (std::uint32_t x = 0; x < frame.width; ++x) row[x] = static_cast<std::uint16_t>(lines[c].current[x + 1]);
          }
        }
        if (bits.overrun()) return fail(Status::Decode, "JPEG-LS scan is truncated");
        for (const std::size_t c : components) decoded[c] = true;
        (void)r.seek(end);
        if (std::all_of(decoded.begin(), decoded.end(), [](bool v) { return v; })) {
          // Keep reading only for trailing markers; the picture is complete.
          done = true;
        }
        preset.maxval = state.p.maxval;  // MAXVAL applies to the frame
        break;
      }
      default: break;  // APPn (SPIFF, ...), COM: skipped
    }
  }
  if (!have_frame || decoded.empty() || !std::all_of(decoded.begin(), decoded.end(), [](bool v) { return v; })) {
    return fail(Status::Decode, "JPEG-LS file is incomplete");
  }

  const int maxval = preset.maxval != 0 ? preset.maxval : (1 << frame.bits) - 1;
  const std::size_t channels = planes.size();
  const std::size_t pixels = std::size_t{frame.width} * frame.height;
  ir::Image image;
  ir::Frame out = ir::Frame::filled(frame.width, frame.height, {0, 0, 0, 255});
  const bool wide = frame.bits > 8;
  std::vector<std::uint16_t> rgba16;
  if (wide) rgba16.assign(pixels * 4, 65535);
  for (std::size_t i = 0; i < pixels; ++i) {
    std::array<std::uint32_t, 4> v{};
    for (std::size_t c = 0; c < channels; ++c) v[c] = planes[c][i];
    std::array<std::uint32_t, 4> rgba{};
    if (channels <= 2) {
      rgba = {v[0], v[0], v[0], channels == 2 ? v[1] : static_cast<std::uint32_t>(maxval)};
    } else {
      rgba = {v[0], v[1], v[2], channels == 4 ? v[3] : static_cast<std::uint32_t>(maxval)};
    }
    for (std::size_t c = 0; c < 4; ++c) {
      out.rgba[i * 4 + c] = image::scale_max_to_8(rgba[c], static_cast<std::uint32_t>(maxval));
      if (wide) {
        rgba16[i * 4 + c] = static_cast<std::uint16_t>(
            (std::min<std::uint64_t>(rgba[c], static_cast<std::uint64_t>(maxval)) * 65535u + static_cast<std::uint64_t>(maxval) / 2) /
            static_cast<std::uint64_t>(maxval));
      }
    }
  }
  image.frames.push_back(std::move(out));
  if (wide) {
    image.hdr = image::linear_from_srgb16(rgba16, frame.width, frame.height);
    image.source_bit_depth = static_cast<std::uint8_t>(frame.bits);
  }
  image.source_grayscale = channels <= 2;
  return image;
}

// --------------------------------------------------------------------------- writing

Result<Bytes> write_jls(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const image::WriteImage prepared, image::prepare_write(source, ctx));
  image::warn_if_animated(*prepared, ctx, "JPEG-LS");
  const ir::Frame& frame = prepared->frames.front();
  if (frame.width > 65535 || frame.height > 65535) return fail(Status::Encode, "JPEG-LS images are limited to 65535 pixels per side");
  const int near = static_cast<int>(ctx.options.get_int("jls_near", 0, 0, 255));
  const std::string interleave = ctx.options.get_enum("jls_interleave", "sample", {"sample", "line", "none"});
  // 16 bits when the source has more than 8 (and HDR pixels to take them from).
  const bool wide = prepared->hdr.has_value() && prepared->source_bit_depth > 8 &&
                    prepared->hdr->width == frame.width && prepared->hdr->height == frame.height;
  const int bits = wide ? 16 : 8;
  const bool alpha = frame.has_transparency();
  const bool grey = image::is_grayscale(frame);
  const std::size_t channels = grey ? (alpha ? 2 : 1) : (alpha ? 4 : 3);
  const std::size_t pixels = std::size_t{frame.width} * frame.height;

  std::vector<std::uint16_t> wide_rgba;
  if (wide) wide_rgba = image::srgb16_from_linear(*prepared->hdr);
  const auto sample = [&](std::size_t c, std::size_t index) -> int {
    const std::size_t channel = channels <= 2 ? (c == 0 ? 0 : 3) : c;
    return wide ? wide_rgba[index * 4 + channel] : frame.rgba[index * 4 + channel];
  };

  State state;
  state.p.bits = bits;
  state.p.maxval = (1 << bits) - 1;
  state.p.near = std::min(near, state.p.maxval / 2);
  state.p.reset = 64;
  CVT_TRY(derive(state.p, 0, 0, 0));

  util::ByteWriter out;
  out.u16be(0xFFD8);
  out.u16be(0xFFF7).u16be(static_cast<std::uint16_t>(8 + 3 * channels)).u8(static_cast<std::uint8_t>(bits));
  out.u16be(static_cast<std::uint16_t>(frame.height)).u16be(static_cast<std::uint16_t>(frame.width));
  out.u8(static_cast<std::uint8_t>(channels));
  for (std::size_t c = 0; c < channels; ++c) out.u8(static_cast<std::uint8_t>(c + 1)).u8(0x11).u8(0);

  const int ilv = channels == 1 ? 0 : (interleave == "sample" ? 2 : interleave == "line" ? 1 : 0);
  const auto scan = [&](const std::vector<std::size_t>& comps) -> Result<void> {
    out.u16be(0xFFDA).u16be(static_cast<std::uint16_t>(6 + 2 * comps.size())).u8(static_cast<std::uint8_t>(comps.size()));
    for (const std::size_t c : comps) out.u8(static_cast<std::uint8_t>(c + 1)).u8(0);
    out.u8(static_cast<std::uint8_t>(state.p.near)).u8(static_cast<std::uint8_t>(comps.size() == 1 ? 0 : ilv)).u8(0);
    state.reset_contexts();
    BitWriter bw;
    Encoder encoder(state, bw);
    std::vector<Lines> lines(comps.size());
    for (Lines& l : lines) l.init(frame.width);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
      if ((y & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / frame.height));
      const std::size_t row = std::size_t{y} * frame.width;
      for (Lines& l : lines) l.start_line(frame.width);
      if (comps.size() > 1 && ilv == 2) {
        encoder.line_interleaved(lines, frame.width,
                                 [&](std::size_t c, std::size_t x) { return sample(comps[c], row + x); });
      } else {
        for (std::size_t c = 0; c < comps.size(); ++c) {
          encoder.line(lines[c], frame.width, [&](std::size_t x) { return sample(comps[c], row + x); });
        }
      }
    }
    bw.finish();
    out.bytes(bw.bytes());
    return {};
  };
  if (ilv == 0) {
    for (std::size_t c = 0; c < channels; ++c) CVT_TRY(scan({c}));
  } else {
    std::vector<std::size_t> all(channels);
    for (std::size_t c = 0; c < channels; ++c) all[c] = c;
    CVT_TRY(scan(all));
  }
  (void)pixels;
  out.u16be(0xFFD9);
  return out.take();
}

}  // namespace

void register_misc_jls(Registry& registry) {
  FormatInfo info;
  info.id = "jls";
  info.name = "JPEG-LS image";
  info.description = "Lossless / near-lossless JPEG (ITU-T T.87), used in medical imaging.";
  info.category = Category::Image;
  info.extensions = {"jls"};
  info.mime = "image/jls";
  info.popularity = 10;
  info.signatures = {Signature::at(0, "FF D8 FF F7"), Signature::at(0, "FF D8 FF E8 ?? ?? 'SPIFF' 00")};
  registry.add_format(std::move(info));

  ImageCodecSpec codec;
  codec.format_id = "jls";
  codec.read = image::reader(read_jls);
  codec.write = image::writer(write_jls);
  codec.write_options = image::with(
      image::plain_writer_options(),
      {OptionSpec::integer("jls_near", "Near-lossless tolerance", 0, 0, 255,
                           "0 = lossless; n = every sample within n of the original (smaller files)"),
       OptionSpec::choice("jls_interleave", "Interleave", "sample", {"sample", "line", "none"},
                          "How colour components are interleaved")
           .advanced()});
  image::add_image_codec(registry, std::move(codec));
}

}  // namespace cvt::image_modern
