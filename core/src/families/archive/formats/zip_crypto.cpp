// Convertible core - archive family: ZIP encryption (traditional PKWARE "ZipCrypto" and WinZip AES).
//
// ZipCrypto follows APPNOTE 6.1: three 32-bit keys updated with CRC-32 and a
// linear congruence, a 12-byte encrypted header whose last byte checks the
// password. WinZip AES (AE-1/AE-2) follows WinZip's specification: PBKDF2-
// HMAC-SHA1 with 1000 iterations derives the AES key, the HMAC-SHA1 key and a
// 2-byte password verifier from the password and a salt of 8/12/16 bytes;
// data is encrypted with AES in counter mode (16-byte little-endian counter
// starting at 1) and authenticated with the first 10 bytes of HMAC-SHA1 over
// the encrypted data.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <format>

#include "families/archive/crypto.hpp"
#include "families/archive/formats/zip_internal.hpp"
#include "util/crc.hpp"
#include "util/hash.hpp"

namespace cvt::archive::zip {

namespace {

std::uint32_t crc_byte(std::uint32_t crc, std::uint8_t b) noexcept {
  return util::detail::crc32_table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
}

std::uint16_t le16(ByteView v, std::size_t at) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(v[at]) | (std::to_integer<unsigned>(v[at + 1]) << 8));
}

struct AesKeys {
  Aes aes;
  Bytes mac_key;
  std::array<std::uint8_t, 2> verifier{};
};

AesKeys derive_keys(std::string_view password, std::uint8_t strength, ByteView salt) {
  const std::size_t key_size = strength == 1 ? 16 : (strength == 2 ? 24 : 32);
  Bytes material = pbkdf2_hmac_sha1(as_bytes(password), salt, 1000, 2 * key_size + aes_verifier_size);
  AesKeys keys;
  (void)keys.aes.set_key(ByteView(material).first(key_size));
  keys.mac_key.assign(material.begin() + static_cast<std::ptrdiff_t>(key_size),
                      material.begin() + static_cast<std::ptrdiff_t>(2 * key_size));
  keys.verifier = {std::to_integer<std::uint8_t>(material[2 * key_size]),
                   std::to_integer<std::uint8_t>(material[2 * key_size + 1])};
  secure_zero(std::span<std::byte>(material));
  return keys;
}

// AES-CTR with WinZip's little-endian counter; the same operation both ways.
void aes_ctr(const Aes& aes, std::span<std::byte> data) {
  std::array<std::uint8_t, 16> counter{};
  std::array<std::uint8_t, 16> stream{};
  for (std::size_t pos = 0; pos < data.size(); pos += 16) {
    for (std::uint8_t& c : counter) {
      if (++c != 0) break;
    }
    aes.encrypt_block(counter.data(), stream.data());
    const std::size_t n = std::min<std::size_t>(16, data.size() - pos);
    for (std::size_t i = 0; i < n; ++i) data[pos + i] ^= static_cast<std::byte>(stream[i]);
  }
}

}  // namespace

// ---------------------------------------------------------------------------- misc

std::string method_description(std::uint16_t method) {
  switch (method) {
    case 0: return "0 (stored)";
    case 1: return "1 (shrink)";
    case 2:
    case 3:
    case 4:
    case 5: return std::format("{} (reduce)", method);
    case 6: return "6 (implode)";
    case 7: return "7 (tokenizing)";
    case 8: return "8 (deflate)";
    case 9: return "9 (deflate64)";
    case 10: return "10 (PKWARE DCL implode)";
    case 12: return "12 (bzip2)";
    case 14: return "14 (LZMA)";
    case 16: return "16 (IBM z/OS CMPSC)";
    case 18: return "18 (IBM TERSE)";
    case 19: return "19 (IBM LZ77 z)";
    case 20: return "20 (Zstandard, deprecated id)";
    case 93: return "93 (Zstandard)";
    case 94: return "94 (MP3)";
    case 95: return "95 (xz)";
    case 96: return "96 (WinZip JPEG)";
    case 97: return "97 (WavPack)";
    case 98: return "98 (PPMd)";
    case 99: return "99 (AES)";
    default: return std::format("{} (unknown)", method);
  }
}

ByteView find_extra(ByteView extras, std::uint16_t id) noexcept {
  std::size_t pos = 0;
  while (pos + 4 <= extras.size()) {
    const std::uint16_t tag = le16(extras, pos);
    const std::size_t size = le16(extras, pos + 2);
    if (size > extras.size() - pos - 4) return {};
    if (tag == id) return extras.subspan(pos + 4, size);
    pos += 4 + size;
  }
  return {};
}

// ---------------------------------------------------------------------------- ZipCrypto

ZipCrypto::ZipCrypto(std::string_view password) noexcept {
  for (const char c : password) update(static_cast<std::uint8_t>(c));
}

void ZipCrypto::update(std::uint8_t plain) noexcept {
  k0_ = crc_byte(k0_, plain);
  k1_ = (k1_ + (k0_ & 0xFFu)) * 134775813u + 1u;
  k2_ = crc_byte(k2_, static_cast<std::uint8_t>(k1_ >> 24));
}

std::uint8_t ZipCrypto::stream_byte() const noexcept {
  const std::uint32_t t = (k2_ | 2u) & 0xFFFFu;
  return static_cast<std::uint8_t>(((t * (t ^ 1u)) >> 8) & 0xFFu);
}

void ZipCrypto::decrypt(std::span<std::byte> data) noexcept {
  for (std::byte& b : data) {
    const auto plain = static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(b) ^ stream_byte());
    update(plain);
    b = static_cast<std::byte>(plain);
  }
}

void ZipCrypto::encrypt(std::span<std::byte> data) noexcept {
  for (std::byte& b : data) {
    const auto plain = std::to_integer<std::uint8_t>(b);
    const auto cipher = static_cast<std::uint8_t>(plain ^ stream_byte());
    update(plain);
    b = static_cast<std::byte>(cipher);
  }
}

// ---------------------------------------------------------------------------- WinZip AES

Result<AesInfo> parse_aes_extra(ByteView value) {
  if (value.size() < 7) return fail(Status::Decode, "ZIP AES extra field is missing or damaged");
  AesInfo info;
  info.vendor_version = le16(value, 0);
  if (value[2] != std::byte{'A'} || value[3] != std::byte{'E'} || info.vendor_version < 1 || info.vendor_version > 2) {
    return fail(Status::Decode, "ZIP AES extra field has an unknown vendor or version");
  }
  info.strength = std::to_integer<std::uint8_t>(value[4]);
  if (info.strength < 1 || info.strength > 3) return fail(Status::Decode, "ZIP AES key strength {} is invalid", info.strength);
  info.method = le16(value, 5);
  return info;
}

Result<Bytes> aes_decrypt(ByteView payload, std::string_view password, std::uint8_t strength) {
  const std::size_t salt_size = aes_salt_size(strength);
  if (payload.size() < salt_size + aes_verifier_size + aes_mac_size) {
    return fail(Status::Decode, "ZIP AES entry is truncated");
  }
  AesKeys keys = derive_keys(password, strength, payload.first(salt_size));
  const ByteView verifier = payload.subspan(salt_size, aes_verifier_size);
  if (std::to_integer<std::uint8_t>(verifier[0]) != keys.verifier[0] ||
      std::to_integer<std::uint8_t>(verifier[1]) != keys.verifier[1]) {
    secure_zero(std::span<std::byte>(keys.mac_key));
    return fail(Status::Decode, "wrong password");
  }
  const std::size_t body_start = salt_size + aes_verifier_size;
  const ByteView body = payload.subspan(body_start, payload.size() - body_start - aes_mac_size);
  HmacSha1 mac(keys.mac_key);
  mac.update(body);
  const auto code = mac.finish();
  secure_zero(std::span<std::byte>(keys.mac_key));
  if (std::memcmp(code.data(), payload.last(aes_mac_size).data(), aes_mac_size) != 0) {
    return fail(Status::Decode, "ZIP AES authentication failed: wrong password or damaged data");
  }
  Bytes out(body.begin(), body.end());
  aes_ctr(keys.aes, out);
  return out;
}

Bytes aes_encrypt(ByteView data, std::string_view password, std::uint8_t strength, ByteView salt) {
  AesKeys keys = derive_keys(password, strength, salt);
  Bytes out;
  out.reserve(salt.size() + aes_verifier_size + data.size() + aes_mac_size);
  append(out, salt);
  out.push_back(static_cast<std::byte>(keys.verifier[0]));
  out.push_back(static_cast<std::byte>(keys.verifier[1]));
  const std::size_t body_start = out.size();
  append(out, data);
  aes_ctr(keys.aes, std::span<std::byte>(out).subspan(body_start));
  HmacSha1 mac(keys.mac_key);
  mac.update(ByteView(out).subspan(body_start));
  const auto code = mac.finish();
  secure_zero(std::span<std::byte>(keys.mac_key));
  append(out, as_bytes(std::span<const std::uint8_t>(code.data(), aes_mac_size)));
  return out;
}

std::array<std::uint8_t, 32> entry_nonce(std::string_view label, std::string_view password, std::string_view name,
                                         std::uint32_t crc, ByteView content) {
  util::Sha256 h;
  const auto field = [&h](std::string_view s) {
    const std::array<std::byte, 4> len = {static_cast<std::byte>(s.size() & 0xFFu), static_cast<std::byte>((s.size() >> 8) & 0xFFu),
                                          static_cast<std::byte>((s.size() >> 16) & 0xFFu),
                                          static_cast<std::byte>((s.size() >> 24) & 0xFFu)};
    h.update(len);
    h.update(as_bytes(s));
  };
  field("convertible zip entry nonce");
  field(label);
  field(password);
  field(name);
  std::array<std::byte, 12> numbers{};
  for (std::size_t i = 0; i < 4; ++i) numbers[i] = static_cast<std::byte>((crc >> (8 * i)) & 0xFFu);
  const auto size = static_cast<std::uint64_t>(content.size());
  for (std::size_t i = 0; i < 8; ++i) numbers[4 + i] = static_cast<std::byte>((size >> (8 * i)) & 0xFFu);
  h.update(numbers);
  const auto digest = util::sha256(content);
  h.update(as_bytes(std::span<const std::uint8_t>(digest)));
  return h.finish();
}

}  // namespace cvt::archive::zip
