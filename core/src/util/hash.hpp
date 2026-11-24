// Convertible core - MD5, SHA-1 and SHA-256 message digests.
// These exist for checksum reports and file-format needs (not for security
// decisions): MD5 and SHA-1 are cryptographically broken.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "util/bytes.hpp"

namespace cvt::util {

/// Incremental MD5 (RFC 1321). `update()` any number of times, then `finish()` once.
class Md5 {
 public:
  using Digest = std::array<std::uint8_t, 16>;
  Md5() noexcept;
  void update(ByteView data) noexcept;
  [[nodiscard]] Digest finish() noexcept;

 private:
  void transform(const std::uint8_t* block) noexcept;
  std::array<std::uint32_t, 4> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t length_ = 0;
};

/// Incremental SHA-1 (FIPS 180-4).
class Sha1 {
 public:
  using Digest = std::array<std::uint8_t, 20>;
  Sha1() noexcept;
  void update(ByteView data) noexcept;
  [[nodiscard]] Digest finish() noexcept;

 private:
  void transform(const std::uint8_t* block) noexcept;
  std::array<std::uint32_t, 5> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t length_ = 0;
};

/// Incremental SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  using Digest = std::array<std::uint8_t, 32>;
  Sha256() noexcept;
  void update(ByteView data) noexcept;
  [[nodiscard]] Digest finish() noexcept;

 private:
  void transform(const std::uint8_t* block) noexcept;
  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t length_ = 0;
};

/// One-shot digests.
[[nodiscard]] Md5::Digest md5(ByteView data) noexcept;
[[nodiscard]] Sha1::Digest sha1(ByteView data) noexcept;
[[nodiscard]] Sha256::Digest sha256(ByteView data) noexcept;

/// One-shot digests as lowercase hex strings.
[[nodiscard]] std::string md5_hex(ByteView data);
[[nodiscard]] std::string sha1_hex(ByteView data);
[[nodiscard]] std::string sha256_hex(ByteView data);

}  // namespace cvt::util
