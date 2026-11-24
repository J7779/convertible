// Convertible core - codec_legacy family: 8x8 DCT and IDCT.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/codec_legacy/common/dct.hpp"

namespace cvt::legacy {

namespace {

// T[x][u] = round(2^20 * C(u)/2 * cos((2x+1) u pi / 16)), C(0) = 1/sqrt(2).
constexpr std::int64_t kCos[8][8] = {
    {370728, 514214, 484379, 435930, 370728, 291279, 200636, 102284},
    {370728, 435930, 200636, -102284, -370728, -514214, -484379, -291279},
    {370728, 291279, -200636, -514214, -370728, 102284, 484379, 435930},
    {370728, 102284, -484379, -291279, 370728, 435930, -200636, -514214},
    {370728, -102284, -484379, 291279, 370728, -435930, -200636, 514214},
    {370728, -291279, -200636, 514214, -370728, -102284, 484379, -435930},
    {370728, -435930, 200636, 102284, -370728, 514214, -484379, 291279},
    {370728, -514214, 484379, -435930, 370728, -291279, 200636, -102284}};

constexpr int kConstBits = 20;
constexpr int kPassBits = 10;

[[nodiscard]] constexpr std::int64_t round_shift(std::int64_t v, int shift) noexcept {
  // Round half away from zero, symmetric for negative values.
  const std::int64_t half = std::int64_t{1} << (shift - 1);
  return v >= 0 ? (v + half) >> shift : -((-v + half) >> shift);
}

// One inverse 1-D transform of 8 values with the given input stride, using
// the even/odd symmetry f(x) = E + O, f(7 - x) = E - O.
template <class In>
inline void inverse_1d(const In* in, std::ptrdiff_t in_stride, std::int64_t* out, std::ptrdiff_t out_stride, int shift) noexcept {
  const std::int64_t f0 = in[0], f1 = in[in_stride], f2 = in[2 * in_stride], f3 = in[3 * in_stride];
  const std::int64_t f4 = in[4 * in_stride], f5 = in[5 * in_stride], f6 = in[6 * in_stride], f7 = in[7 * in_stride];
  for (int x = 0; x < 4; ++x) {
    const std::int64_t* t = kCos[x];
    const std::int64_t even = t[0] * f0 + t[2] * f2 + t[4] * f4 + t[6] * f6;
    const std::int64_t odd = t[1] * f1 + t[3] * f3 + t[5] * f5 + t[7] * f7;
    out[x * out_stride] = round_shift(even + odd, shift);
    out[(7 - x) * out_stride] = round_shift(even - odd, shift);
  }
}

void idct_core(const std::int32_t* in, std::int64_t* out, int extra_shift = 0) noexcept {
  std::int64_t tmp[64];
  for (int r = 0; r < 8; ++r) {
    const std::int32_t* row = in + r * 8;
    bool ac_zero = true;
    for (int i = 1; i < 8; ++i) {
      if (row[i] != 0) {
        ac_zero = false;
        break;
      }
    }
    if (ac_zero) {
      const std::int64_t dc = round_shift(kCos[0][0] * row[0], kConstBits - kPassBits);
      for (int i = 0; i < 8; ++i) tmp[r * 8 + i] = dc;
    } else {
      inverse_1d(row, 1, tmp + r * 8, 1, kConstBits - kPassBits);
    }
  }
  for (int c = 0; c < 8; ++c) inverse_1d(tmp + c, 8, out + c, 8, kConstBits + kPassBits + extra_shift);
}

}  // namespace

void idct8x8(const std::int32_t* coefficients, std::int32_t* out) noexcept {
  std::int64_t wide[64];
  idct_core(coefficients, wide);
  for (int i = 0; i < 64; ++i) out[i] = static_cast<std::int32_t>(wide[i]);
}

void idct8x8_scaled(const std::int32_t* coefficients, std::int32_t* out, int shift) noexcept {
  std::int64_t wide[64];
  idct_core(coefficients, wide, shift);
  for (int i = 0; i < 64; ++i) out[i] = static_cast<std::int32_t>(wide[i]);
}

void idct8x8_put(const std::int32_t* coefficients, std::uint8_t* dst, std::ptrdiff_t stride) noexcept {
  std::int64_t wide[64];
  idct_core(coefficients, wide);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) dst[y * stride + x] = clamp_u8(static_cast<int>(wide[y * 8 + x]));
  }
}

void idct8x8_add(const std::int32_t* coefficients, std::uint8_t* dst, std::ptrdiff_t stride) noexcept {
  std::int64_t wide[64];
  idct_core(coefficients, wide);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      std::uint8_t& p = dst[y * stride + x];
      p = clamp_u8(p + static_cast<int>(wide[y * 8 + x]));
    }
  }
}

void fdct8x8(const std::int32_t* samples, std::int32_t* out) noexcept {
  // F(u) = sum_x T[x][u] f(x): the transpose of the inverse transform.
  std::int64_t tmp[64];
  for (int r = 0; r < 8; ++r) {
    const std::int32_t* row = samples + r * 8;
    for (int u = 0; u < 8; ++u) {
      std::int64_t acc = 0;
      for (int x = 0; x < 8; ++x) acc += kCos[x][u] * row[x];
      tmp[r * 8 + u] = round_shift(acc, kConstBits - kPassBits);
    }
  }
  for (int c = 0; c < 8; ++c) {
    for (int v = 0; v < 8; ++v) {
      std::int64_t acc = 0;
      for (int y = 0; y < 8; ++y) acc += kCos[y][v] * tmp[y * 8 + c];
      out[v * 8 + c] = static_cast<std::int32_t>(round_shift(acc, kConstBits + kPassBits));
    }
  }
}

}  // namespace cvt::legacy
