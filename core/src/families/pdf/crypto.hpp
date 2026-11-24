// Convertible core - pdf family: ciphers and hashes of the standard security
// handler (RC4, AES-128/256 in CBC mode, SHA-384/512).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cvt::pdf::crypto {

using ByteVec = std::vector<std::uint8_t>;

/// RC4 keystream applied to `data` (encrypt == decrypt).
[[nodiscard]] ByteVec rc4(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);

/// AES key schedule for 128/192/256-bit keys.
class Aes {
 public:
  /// `key` must be 16, 24 or 32 bytes; otherwise the object is unusable (ok() false).
  explicit Aes(std::span<const std::uint8_t> key);
  [[nodiscard]] bool ok() const noexcept { return rounds_ != 0; }
  void encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const noexcept;
  void decrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const noexcept;

 private:
  std::array<std::uint32_t, 60> enc_{};
  std::array<std::uint32_t, 60> dec_{};
  int rounds_ = 0;
};

/// AES-CBC decryption of PDF data: the first 16 bytes are the IV, PKCS#5
/// padding is removed when valid (kept otherwise, as readers tolerate).
/// Data shorter than one block decrypts to nothing.
[[nodiscard]] ByteVec aes_cbc_decrypt_pdf(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);
/// AES-CBC encryption with an explicit IV, no padding (length must be a
/// multiple of 16; the tail is ignored otherwise). Used by the R6 hash.
[[nodiscard]] ByteVec aes_cbc_encrypt_nopad(std::span<const std::uint8_t> key, std::span<const std::uint8_t> iv,
                                            std::span<const std::uint8_t> data);
/// AES-ECB decryption without padding (for the /Perms check).
[[nodiscard]] ByteVec aes_ecb_decrypt(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);
/// AES-CBC decryption with IV zero, no padding (for /UE and /OE in R5/R6).
[[nodiscard]] ByteVec aes_cbc_decrypt_zero_iv(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);

[[nodiscard]] std::array<std::uint8_t, 64> sha512(std::span<const std::uint8_t> data);
[[nodiscard]] std::array<std::uint8_t, 48> sha384(std::span<const std::uint8_t> data);
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data);
[[nodiscard]] std::array<std::uint8_t, 16> md5(std::span<const std::uint8_t> data);

}  // namespace cvt::pdf::crypto
