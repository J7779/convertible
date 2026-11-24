// Convertible core - codec_legacy family: 8x8 DCT and IDCT.
//
// Integer-only and therefore identical on every platform. The transforms are
// the exact orthonormal DCT-II/DCT-III of ITU-T T.81 / ISO 13818-2 Annex A,
// evaluated with 20-bit cosine constants and 10 extra bits between the two
// passes: well inside the IEEE 1180 accuracy limits, so decoders using them
// stay close to every conforming reference decoder and do not drift.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>

namespace cvt::legacy {

/// Inverse DCT of one 8x8 block (row-major coefficients) into `out`,
/// rounded to nearest, not clamped.
void idct8x8(const std::int32_t* coefficients, std::int32_t* out) noexcept;
/// Inverse DCT of coefficients scaled up by 2^shift (shift 0..8): the
/// result is divided by 2^shift with a single rounding at the end.
void idct8x8_scaled(const std::int32_t* coefficients, std::int32_t* out, int shift) noexcept;
/// Inverse DCT, clamped to 0..255 and stored into an 8x8 area of `dst`.
void idct8x8_put(const std::int32_t* coefficients, std::uint8_t* dst, std::ptrdiff_t stride) noexcept;
/// Inverse DCT added to the 8x8 area of `dst` (prediction), clamped to 0..255.
void idct8x8_add(const std::int32_t* coefficients, std::uint8_t* dst, std::ptrdiff_t stride) noexcept;
/// Forward DCT of one 8x8 block of samples (any range), rounded to nearest:
/// F(u,v) = 1/4 C(u) C(v) sum f(x,y) cos(...) cos(...).
void fdct8x8(const std::int32_t* samples, std::int32_t* out) noexcept;

/// Zig-zag scan: scan position -> raster index (ISO 13818-2 Figure 7-2).
inline constexpr std::uint8_t zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
/// Alternate scan: scan position -> raster index (ISO 13818-2 Figure 7-3).
inline constexpr std::uint8_t alternate_scan[64] = {
    0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3,  11,
    4,  12, 19, 27, 34, 42, 50, 58, 35, 43, 51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44,
    52, 60, 37, 45, 53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};

[[nodiscard]] constexpr std::uint8_t clamp_u8(int v) noexcept {
  return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace cvt::legacy
