// Convertible core - archive family: the cryptography that archive formats need.
//
// AES-128/192/256 (FIPS-197) with CBC helpers, HMAC-SHA1 / HMAC-SHA256
// (RFC 2104) and PBKDF2 (RFC 8018). Used for WinZip AES and traditional
// ZipCrypto (zip), 7z AES-256 + SHA-256 key derivation, and RAR 3/5
// encryption. Implemented in-house on top of util/hash.hpp. These routines
// exist to read and write password-protected archives; they are written to
// be correct and deterministic, not hardened against side channels.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"
#include "util/hash.hpp"

namespace cvt::archive {

/// One AES key schedule. Encrypts and decrypts single 16-byte blocks.
class Aes {
 public:
  using Block = std::array<std::uint8_t, 16>;

  Aes() = default;
  /// Key of 16, 24 or 32 bytes; false for any other length.
  [[nodiscard]] bool set_key(std::span<const std::uint8_t> key) noexcept;
  [[nodiscard]] bool set_key(ByteView key) noexcept { return set_key(as_u8(key)); }

  void encrypt_block(const std::uint8_t* in, std::uint8_t* out) const noexcept;
  void decrypt_block(const std::uint8_t* in, std::uint8_t* out) const noexcept;

  [[nodiscard]] int rounds() const noexcept { return rounds_; }

 private:
  std::array<std::uint32_t, 60> enc_{};
  std::array<std::uint32_t, 60> dec_{};
  int rounds_ = 0;
};

/// AES-CBC over whole 16-byte blocks (no padding is added or removed).
/// `iv` is updated so that consecutive calls continue the chain.
/// Fails with InvalidArgument when `data.size()` is not a multiple of 16.
[[nodiscard]] Result<void> aes_cbc_decrypt(const Aes& aes, Aes::Block& iv, std::span<std::uint8_t> data);
[[nodiscard]] Result<void> aes_cbc_encrypt(const Aes& aes, Aes::Block& iv, std::span<std::uint8_t> data);
[[nodiscard]] inline Result<void> aes_cbc_decrypt(const Aes& aes, Aes::Block& iv, std::span<std::byte> data) {
  return aes_cbc_decrypt(aes, iv, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(data.data()), data.size()));
}
[[nodiscard]] inline Result<void> aes_cbc_encrypt(const Aes& aes, Aes::Block& iv, std::span<std::byte> data) {
  return aes_cbc_encrypt(aes, iv, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(data.data()), data.size()));
}

/// Incremental HMAC-SHA1 (RFC 2104).
class HmacSha1 {
 public:
  explicit HmacSha1(ByteView key) noexcept;
  void update(ByteView data) noexcept;
  [[nodiscard]] util::Sha1::Digest finish() noexcept;

 private:
  util::Sha1 inner_;
  util::Sha1 outer_;
};

/// Incremental HMAC-SHA256 (RFC 2104 / RFC 4231).
class HmacSha256 {
 public:
  explicit HmacSha256(ByteView key) noexcept;
  void update(ByteView data) noexcept;
  [[nodiscard]] util::Sha256::Digest finish() noexcept;

 private:
  util::Sha256 inner_;
  util::Sha256 outer_;
};

[[nodiscard]] util::Sha1::Digest hmac_sha1(ByteView key, ByteView data) noexcept;
[[nodiscard]] util::Sha256::Digest hmac_sha256(ByteView key, ByteView data) noexcept;

/// PBKDF2 (RFC 8018) producing `length` bytes.
[[nodiscard]] Bytes pbkdf2_hmac_sha1(ByteView password, ByteView salt, std::uint32_t iterations, std::size_t length);
[[nodiscard]] Bytes pbkdf2_hmac_sha256(ByteView password, ByteView salt, std::uint32_t iterations, std::size_t length);

/// Overwrite key material before it is released.
void secure_zero(std::span<std::byte> data) noexcept;
void secure_zero(std::span<std::uint8_t> data) noexcept;

}  // namespace cvt::archive
