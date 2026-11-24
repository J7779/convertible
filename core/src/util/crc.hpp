// Convertible core - CRC-32 (IEEE 802.3, as used by zip/gzip/png) and Adler-32.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "util/bytes.hpp"

namespace cvt::util {

namespace detail {
constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    table[i] = c;
  }
  return table;
}
inline constexpr std::array<std::uint32_t, 256> crc32_table = make_crc32_table();
}  // namespace detail

/// CRC-32 of `data`. Pass a previous result as `crc` to checksum a stream in
/// pieces: `crc32(b, crc32(a)) == crc32(a + b)`.
[[nodiscard]] constexpr std::uint32_t crc32(ByteView data, std::uint32_t crc = 0) noexcept {
  std::uint32_t c = ~crc;
  for (const std::byte b : data) {
    c = detail::crc32_table[(c ^ std::to_integer<std::uint32_t>(b)) & 0xFFu] ^ (c >> 8);
  }
  return ~c;
}

/// Adler-32 of `data`. The initial value is 1; pass a previous result as
/// `adler` to continue a running checksum.
[[nodiscard]] constexpr std::uint32_t adler32(ByteView data, std::uint32_t adler = 1) noexcept {
  constexpr std::uint32_t modulus = 65521;
  std::uint32_t a = adler & 0xFFFFu;
  std::uint32_t b = (adler >> 16) & 0xFFFFu;
  std::size_t i = 0;
  while (i < data.size()) {
    // 5552 is the largest block for which the sums cannot overflow 32 bits.
    const std::size_t end = i + (data.size() - i < 5552 ? data.size() - i : 5552);
    for (; i < end; ++i) {
      a += std::to_integer<std::uint32_t>(data[i]);
      b += a;
    }
    a %= modulus;
    b %= modulus;
  }
  return (b << 16) | a;
}

}  // namespace cvt::util
