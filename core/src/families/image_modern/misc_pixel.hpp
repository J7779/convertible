// Convertible core - image_modern family: small pixel helpers shared by the
// assorted formats (half floats, big/little-endian loads from raw buffers).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace cvt::image_modern {

/// IEEE 754 binary16 -> float (subnormals, infinities and NaN included).
[[nodiscard]] inline float half_to_float(std::uint16_t h) noexcept {
  const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
  const std::uint32_t exponent = (h >> 10) & 0x1Fu;
  std::uint32_t mantissa = h & 0x3FFu;
  std::uint32_t bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // Subnormal: normalise.
      int e = -1;
      do {
        ++e;
        mantissa <<= 1;
      } while ((mantissa & 0x400u) == 0);
      bits = sign | static_cast<std::uint32_t>(127 - 15 - e) << 23 | (mantissa & 0x3FFu) << 13;
    }
  } else if (exponent == 0x1F) {
    bits = sign | 0x7F800000u | mantissa << 13;
  } else {
    bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
  }
  return std::bit_cast<float>(bits);
}

[[nodiscard]] inline std::uint16_t load_be16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}
[[nodiscard]] inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
  return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}
[[nodiscard]] inline std::uint64_t load_be64(const std::uint8_t* p) noexcept {
  return std::uint64_t{load_be32(p)} << 32 | load_be32(p + 4);
}
[[nodiscard]] inline std::uint16_t load_le16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[1] << 8 | p[0]);
}
[[nodiscard]] inline std::uint32_t load_le32(const std::uint8_t* p) noexcept {
  return std::uint32_t{p[3]} << 24 | std::uint32_t{p[2]} << 16 | std::uint32_t{p[1]} << 8 | p[0];
}

/// A float that is NaN becomes 0; others are returned unchanged.
[[nodiscard]] inline float finite_or_zero(float v) noexcept { return std::isfinite(v) ? v : 0.0f; }

}  // namespace cvt::image_modern
