// Convertible core - archive family: checksums used by compression and archive formats.
//
//   crc32c        CRC-32C (Castagnoli), reflected: Snappy framing, ext4, iSCSI
//   crc64_xz      CRC-64/XZ (ECMA-182 polynomial, reflected): xz
//   crc32_msb     CRC-32/BZIP2 (polynomial 04C11DB7, MSB first): bzip2
//   crc16_arc     CRC-16/ARC (polynomial 8005, reflected): LHA, ARC, ARJ headers, RAR3 headers
//   crc32 (IEEE)  lives in util/crc.hpp
//   xxh32, xxh64  xxHash: LZ4 frames, Zstandard frames
//
// All are deterministic and allocation-free. Running values can be continued:
// f(b, f(a)) == f(a + b) for the CRCs; the xxHash classes are incremental.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "util/bytes.hpp"

namespace cvt::archive {

[[nodiscard]] std::uint32_t crc32c(ByteView data, std::uint32_t crc = 0) noexcept;
[[nodiscard]] std::uint64_t crc64_xz(ByteView data, std::uint64_t crc = 0) noexcept;
[[nodiscard]] std::uint32_t crc32_msb(ByteView data, std::uint32_t crc = 0) noexcept;
[[nodiscard]] std::uint16_t crc16_arc(ByteView data, std::uint16_t crc = 0) noexcept;

/// Snappy framing's masked CRC-32C: ((crc >> 15) | (crc << 17)) + 0xa282ead8.
[[nodiscard]] constexpr std::uint32_t snappy_mask_crc(std::uint32_t crc) noexcept {
  return ((crc >> 15) | (crc << 17)) + 0xA282EAD8u;
}

/// Incremental xxHash32.
class Xxh32 {
 public:
  explicit Xxh32(std::uint32_t seed = 0) noexcept;
  void update(ByteView data) noexcept;
  [[nodiscard]] std::uint32_t digest() const noexcept;

 private:
  std::array<std::uint32_t, 4> v_{};
  std::array<std::uint8_t, 16> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_ = 0;
  std::uint32_t seed_ = 0;
};

/// Incremental xxHash64.
class Xxh64 {
 public:
  explicit Xxh64(std::uint64_t seed = 0) noexcept;
  void update(ByteView data) noexcept;
  [[nodiscard]] std::uint64_t digest() const noexcept;

 private:
  std::array<std::uint64_t, 4> v_{};
  std::array<std::uint8_t, 32> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_ = 0;
  std::uint64_t seed_ = 0;
};

[[nodiscard]] std::uint32_t xxh32(ByteView data, std::uint32_t seed = 0) noexcept;
[[nodiscard]] std::uint64_t xxh64(ByteView data, std::uint64_t seed = 0) noexcept;

}  // namespace cvt::archive
