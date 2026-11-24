// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "util/hash.hpp"

#include <bit>
#include <cstring>

#include "util/hex.hpp"

namespace cvt::util {

namespace {

constexpr std::uint32_t load_le32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
constexpr std::uint32_t load_be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
constexpr void store_le32(std::uint8_t* p, std::uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
constexpr void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * (3 - i)));
}

// Shared Merkle-Damgard buffering for the three 64-byte-block hashes.
template <class Transform>
void absorb(std::array<std::uint8_t, 64>& buffer, std::uint64_t& length, ByteView data, Transform&& transform) noexcept {
  const auto* input = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t remaining = data.size();
  if (remaining == 0) return;
  std::size_t filled = static_cast<std::size_t>(length % 64);
  length += remaining;
  if (filled > 0) {
    const std::size_t take = remaining < 64 - filled ? remaining : 64 - filled;
    ::cvt::util::copy_bytes(buffer.data() + filled, input, take);
    filled += take;
    input += take;
    remaining -= take;
    if (filled < 64) return;
    transform(buffer.data());
  }
  while (remaining >= 64) {
    transform(input);
    input += 64;
    remaining -= 64;
  }
  if (remaining > 0) ::cvt::util::copy_bytes(buffer.data(), input, remaining);
}

// Appends the 0x80 terminator, zero padding and the 64-bit message bit length.
template <class Transform>
void pad(std::array<std::uint8_t, 64>& buffer, std::uint64_t length, bool big_endian_length,
         Transform&& transform) noexcept {
  std::size_t filled = static_cast<std::size_t>(length % 64);
  buffer[filled++] = 0x80;
  if (filled > 56) {
    ::cvt::util::fill_bytes(buffer.data() + filled, 0, 64 - filled);
    transform(buffer.data());
    filled = 0;
  }
  ::cvt::util::fill_bytes(buffer.data() + filled, 0, 56 - filled);
  const std::uint64_t bit_length = length * 8;
  for (int i = 0; i < 8; ++i) {
    const int shift = big_endian_length ? 8 * (7 - i) : 8 * i;
    buffer[56 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bit_length >> shift);
  }
  transform(buffer.data());
}

constexpr std::array<std::uint32_t, 64> md5_k = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr std::array<int, 64> md5_s = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

constexpr std::array<std::uint32_t, 64> sha256_k = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

template <class Digest>
std::string digest_hex(const Digest& digest) {
  return hex_encode(as_bytes(std::span<const std::uint8_t>(digest)));
}

}  // namespace

// --------------------------------------------------------------------------- MD5

Md5::Md5() noexcept : state_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476} {}

void Md5::transform(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 16> m{};
  for (std::size_t i = 0; i < 16; ++i) m[i] = load_le32(block + 4 * i);
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  for (std::size_t i = 0; i < 64; ++i) {
    std::uint32_t f = 0;
    std::size_t g = 0;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    const std::uint32_t next = d;
    d = c;
    c = b;
    b = b + std::rotl(a + f + md5_k[i] + m[g], md5_s[i]);
    a = next;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
}

void Md5::update(ByteView data) noexcept {
  absorb(buffer_, length_, data, [this](const std::uint8_t* block) { transform(block); });
}

Md5::Digest Md5::finish() noexcept {
  pad(buffer_, length_, false, [this](const std::uint8_t* block) { transform(block); });
  Digest digest{};
  for (std::size_t i = 0; i < 4; ++i) store_le32(digest.data() + 4 * i, state_[i]);
  return digest;
}

// --------------------------------------------------------------------------- SHA-1

Sha1::Sha1() noexcept : state_{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0} {}

void Sha1::transform(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 80> w{};
  for (std::size_t i = 0; i < 16; ++i) w[i] = load_be32(block + 4 * i);
  for (std::size_t i = 16; i < 80; ++i) w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  for (std::size_t i = 0; i < 80; ++i) {
    std::uint32_t f = 0;
    std::uint32_t k = 0;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    const std::uint32_t t = std::rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = std::rotl(b, 30);
    b = a;
    a = t;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
}

void Sha1::update(ByteView data) noexcept {
  absorb(buffer_, length_, data, [this](const std::uint8_t* block) { transform(block); });
}

Sha1::Digest Sha1::finish() noexcept {
  pad(buffer_, length_, true, [this](const std::uint8_t* block) { transform(block); });
  Digest digest{};
  for (std::size_t i = 0; i < 5; ++i) store_be32(digest.data() + 4 * i, state_[i]);
  return digest;
}

// --------------------------------------------------------------------------- SHA-256

Sha256::Sha256() noexcept
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::transform(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16; ++i) w[i] = load_be32(block + 4 * i);
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::array<std::uint32_t, 8> v = state_;
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = std::rotr(v[4], 6) ^ std::rotr(v[4], 11) ^ std::rotr(v[4], 25);
    const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
    const std::uint32_t t1 = v[7] + s1 + ch + sha256_k[i] + w[i];
    const std::uint32_t s0 = std::rotr(v[0], 2) ^ std::rotr(v[0], 13) ^ std::rotr(v[0], 22);
    const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
    const std::uint32_t t2 = s0 + maj;
    v[7] = v[6];
    v[6] = v[5];
    v[5] = v[4];
    v[4] = v[3] + t1;
    v[3] = v[2];
    v[2] = v[1];
    v[1] = v[0];
    v[0] = t1 + t2;
  }
  for (std::size_t i = 0; i < 8; ++i) state_[i] += v[i];
}

void Sha256::update(ByteView data) noexcept {
  absorb(buffer_, length_, data, [this](const std::uint8_t* block) { transform(block); });
}

Sha256::Digest Sha256::finish() noexcept {
  pad(buffer_, length_, true, [this](const std::uint8_t* block) { transform(block); });
  Digest digest{};
  for (std::size_t i = 0; i < 8; ++i) store_be32(digest.data() + 4 * i, state_[i]);
  return digest;
}

// --------------------------------------------------------------------------- one-shot helpers

Md5::Digest md5(ByteView data) noexcept {
  Md5 h;
  h.update(data);
  return h.finish();
}
Sha1::Digest sha1(ByteView data) noexcept {
  Sha1 h;
  h.update(data);
  return h.finish();
}
Sha256::Digest sha256(ByteView data) noexcept {
  Sha256 h;
  h.update(data);
  return h.finish();
}

std::string md5_hex(ByteView data) { return digest_hex(md5(data)); }
std::string sha1_hex(ByteView data) { return digest_hex(sha1(data)); }
std::string sha256_hex(ByteView data) { return digest_hex(sha256(data)); }

}  // namespace cvt::util
