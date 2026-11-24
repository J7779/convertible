// Convertible core - archive family: ZIP internals shared by the reader, the writer and the encryption code.
//
// Written from PKWARE's APPNOTE.TXT (6.3.10), the Info-ZIP extra-field list
// and WinZip's published "AES Encryption Information" (AE-1 / AE-2).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/archive/archive_common.hpp"
#include "ir/archive.hpp"
#include "util/bytes.hpp"

namespace cvt::archive::zip {

// ---------------------------------------------------------------------------- record signatures

inline constexpr std::uint32_t sig_local = 0x04034b50;        // PK 03 04
inline constexpr std::uint32_t sig_central = 0x02014b50;      // PK 01 02
inline constexpr std::uint32_t sig_eocd = 0x06054b50;         // PK 05 06
inline constexpr std::uint32_t sig_zip64_eocd = 0x06064b50;   // PK 06 06
inline constexpr std::uint32_t sig_zip64_locator = 0x07064b50;  // PK 06 07
inline constexpr std::uint32_t sig_descriptor = 0x08074b50;   // PK 07 08 (also the spanning marker)

// ---------------------------------------------------------------------------- general-purpose flags

inline constexpr std::uint16_t flag_encrypted = 1u << 0;
inline constexpr std::uint16_t flag_lzma_eos = 1u << 1;
inline constexpr std::uint16_t flag_descriptor = 1u << 3;
inline constexpr std::uint16_t flag_strong_encryption = 1u << 6;
inline constexpr std::uint16_t flag_utf8 = 1u << 11;
inline constexpr std::uint16_t flag_masked_headers = 1u << 13;

// ---------------------------------------------------------------------------- methods and extras

inline constexpr std::uint16_t method_store = 0;
inline constexpr std::uint16_t method_deflate = 8;
inline constexpr std::uint16_t method_bzip2 = 12;
inline constexpr std::uint16_t method_lzma = 14;
inline constexpr std::uint16_t method_zstd = 93;
inline constexpr std::uint16_t method_xz = 95;
inline constexpr std::uint16_t method_aes = 99;

inline constexpr std::uint16_t extra_zip64 = 0x0001;
inline constexpr std::uint16_t extra_ntfs = 0x000a;
inline constexpr std::uint16_t extra_timestamp = 0x5455;    // "UT"
inline constexpr std::uint16_t extra_unicode_path = 0x7075;  // "up"
inline constexpr std::uint16_t extra_aes = 0x9901;

inline constexpr std::uint8_t host_msdos = 0;
inline constexpr std::uint8_t host_unix = 3;
inline constexpr std::uint8_t host_osx = 19;

/// Human description of a method number for error messages.
[[nodiscard]] std::string method_description(std::uint16_t method);

/// The value of extra field `id` inside an extra block, or an empty view.
[[nodiscard]] ByteView find_extra(ByteView extras, std::uint16_t id) noexcept;

// ---------------------------------------------------------------------------- traditional PKWARE encryption

/// The "ZipCrypto" stream cipher (APPNOTE 6.1). Weak; offered for
/// compatibility with tools that know nothing else.
class ZipCrypto {
 public:
  explicit ZipCrypto(std::string_view password) noexcept;
  /// Decrypt in place.
  void decrypt(std::span<std::byte> data) noexcept;
  /// Encrypt in place.
  void encrypt(std::span<std::byte> data) noexcept;

 private:
  void update(std::uint8_t plain) noexcept;
  [[nodiscard]] std::uint8_t stream_byte() const noexcept;
  std::uint32_t k0_ = 0x12345678u;
  std::uint32_t k1_ = 0x23456789u;
  std::uint32_t k2_ = 0x34567890u;
};

inline constexpr std::size_t zipcrypto_header_size = 12;

// ---------------------------------------------------------------------------- WinZip AES

/// The 7-byte value of extra field 0x9901.
struct AesInfo {
  std::uint16_t vendor_version = 2;  ///< 1 = AE-1 (CRC stored), 2 = AE-2 (CRC zero, the HMAC protects the data)
  std::uint8_t strength = 3;         ///< 1 = AES-128, 2 = AES-192, 3 = AES-256
  std::uint16_t method = 0;          ///< the real compression method
};

[[nodiscard]] Result<AesInfo> parse_aes_extra(ByteView value);
[[nodiscard]] constexpr std::size_t aes_salt_size(std::uint8_t strength) noexcept {
  return strength == 1 ? 8 : (strength == 2 ? 12 : 16);
}
inline constexpr std::size_t aes_verifier_size = 2;
inline constexpr std::size_t aes_mac_size = 10;

/// Decrypt a WinZip AES payload (salt + verifier + data + 10-byte HMAC) and
/// return the compressed data. Decode "wrong password" when the verifier or
/// the authentication code does not match.
[[nodiscard]] Result<Bytes> aes_decrypt(ByteView payload, std::string_view password, std::uint8_t strength);
/// Encrypt compressed data with the given salt (aes_salt_size bytes).
[[nodiscard]] Bytes aes_encrypt(ByteView data, std::string_view password, std::uint8_t strength, ByteView salt);

/// Deterministic per-entry bytes for salts and ZipCrypto headers: SHA-256 of
/// a domain label, the password, the entry name, its CRC-32 and size and the
/// SHA-256 of its content. Never the clock, never a random generator; an
/// identical file under an identical name and password gives an identical
/// encrypted entry, and different content always gives a different key.
[[nodiscard]] std::array<std::uint8_t, 32> entry_nonce(std::string_view label, std::string_view password,
                                                       std::string_view name, std::uint32_t crc, ByteView content);

// ---------------------------------------------------------------------------- reader / writer

[[nodiscard]] Result<ir::Archive> read_zip(ReadContext& ctx);
[[nodiscard]] Result<Bytes> write_zip(const ir::Archive& archive, WriteContext& ctx);

/// Offset of the end-of-central-directory record, or npos.
[[nodiscard]] std::size_t find_eocd(ByteView data) noexcept;

}  // namespace cvt::archive::zip
