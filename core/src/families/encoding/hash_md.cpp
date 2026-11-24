// Convertible core - encoding family: block-iterated hash functions
// (MD2, MD4, MD5, SHA-1, SHA-2, RIPEMD-160, SM3, Whirlpool).
//
// Constants that are defined arithmetically are listed with their
// definition; they were computed with exact integer arithmetic (integer
// square and cube roots, the MD2 permutation from the digits of pi) and are
// verified indirectly by the known-answer tests of every algorithm.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "families/encoding/hashes.hpp"
#include "util/hash.hpp"

namespace cvt::encoding {

namespace {

using std::rotl;
using std::rotr;

std::uint32_t load32be(const std::uint8_t* p) noexcept {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
std::uint32_t load32le(const std::uint8_t* p) noexcept {
  return (std::uint32_t{p[3]} << 24) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[1]} << 8) | p[0];
}
std::uint64_t load64be(const std::uint8_t* p) noexcept {
  return (std::uint64_t{load32be(p)} << 32) | load32be(p + 4);
}
void store32be(Bytes& out, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::byte>(v >> s));
}
void store32le(Bytes& out, std::uint32_t v) {
  for (int s = 0; s < 32; s += 8) out.push_back(static_cast<std::byte>(v >> s));
}
void store64be(Bytes& out, std::uint64_t v) {
  for (int s = 56; s >= 0; s -= 8) out.push_back(static_cast<std::byte>(v >> s));
}

/// Buffers input into blocks of `Block` bytes and hands full blocks to
/// `compress`; implements the usual "append 0x80, zeros, bit length" padding.
template <std::size_t Block>
class BlockBuffer {
 public:
  template <class F>
  void feed(ByteView data, F&& compress) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t n = data.size();
    total_ += n;
    if (filled_ > 0) {
      const std::size_t take = std::min(n, Block - filled_);
      std::memcpy(buffer_.data() + filled_, p, take);
      filled_ += take;
      p += take;
      n -= take;
      if (filled_ < Block) return;
      compress(buffer_.data());
      filled_ = 0;
    }
    while (n >= Block) {
      compress(p);
      p += Block;
      n -= Block;
    }
    if (n > 0) std::memcpy(buffer_.data(), p, n);
    filled_ = n;
  }

  /// Merkle-Damgard strengthening: 0x80, zeros, then the message length in
  /// bits in `length_bytes` bytes (8 or 16 for SHA-512, 32 for Whirlpool).
  template <class F>
  void pad(std::size_t length_bytes, bool big_endian, F&& compress) {
    const std::uint64_t bits = total_ * 8;
    buffer_[filled_++] = 0x80;
    if (filled_ > Block - length_bytes) {
      std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(filled_), buffer_.end(), std::uint8_t{0});
      compress(buffer_.data());
      filled_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(filled_), buffer_.end(), std::uint8_t{0});
    for (std::size_t i = 0; i < 8; ++i) {
      const auto byte = static_cast<std::uint8_t>(bits >> (8 * i));
      if (big_endian) {
        buffer_[Block - 1 - i] = byte;
      } else {
        buffer_[Block - length_bytes + i] = byte;
      }
    }
    compress(buffer_.data());
    filled_ = 0;
  }

  [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
  std::array<std::uint8_t, Block>& raw() noexcept { return buffer_; }
  [[nodiscard]] std::size_t filled() const noexcept { return filled_; }

 private:
  std::array<std::uint8_t, Block> buffer_{};
  std::size_t filled_ = 0;
  std::uint64_t total_ = 0;
};

// --------------------------------------------------------------------------- MD2 (RFC 1319)

// The permutation of 0..255 "constructed from the digits of pi" (RFC 1319, 3.2).
constexpr std::array<std::uint8_t, 256> md2_s = {{
    41,  46,  67,  201, 162, 216, 124, 1,   61,  54,  84,  161, 236, 240, 6,   19,  98,  167, 5,   243, 192, 199,
    115, 140, 152, 147, 43,  217, 188, 76,  130, 202, 30,  155, 87,  60,  253, 212, 224, 22,  103, 66,  111, 24,
    138, 23,  229, 18,  190, 78,  196, 214, 218, 158, 222, 73,  160, 251, 245, 142, 187, 47,  238, 122, 169, 104,
    121, 145, 21,  178, 7,   63,  148, 194, 16,  137, 11,  34,  95,  33,  128, 127, 93,  154, 90,  144, 50,  39,
    53,  62,  204, 231, 191, 247, 151, 3,   255, 25,  48,  179, 72,  165, 181, 209, 215, 94,  146, 42,  172, 86,
    170, 198, 79,  184, 56,  210, 150, 164, 125, 182, 118, 252, 107, 226, 156, 116, 4,   241, 69,  157, 112, 89,
    100, 113, 135, 32,  134, 91,  207, 101, 230, 45,  168, 2,   27,  96,  37,  173, 174, 176, 185, 246, 28,  70,
    97,  105, 52,  64,  126, 15,  85,  71,  163, 35,  221, 81,  175, 58,  195, 92,  249, 206, 186, 197, 234, 38,
    44,  83,  13,  110, 133, 40,  132, 9,   211, 223, 205, 244, 65,  129, 77,  82,  106, 220, 55,  200, 108, 193,
    171, 250, 36,  225, 123, 8,   12,  189, 177, 74,  120, 136, 149, 139, 227, 99,  232, 109, 233, 203, 213, 254,
    59,  0,   29,  57,  242, 239, 183, 14,  102, 88,  208, 228, 166, 119, 114, 248, 235, 117, 75,  10,  49,  68,
    80,  180, 143, 237, 31,  26,  219, 153, 141, 51,  159, 17,  131, 20,
}};

class Md2 final : public Hasher {
 public:
  void update(ByteView data) override {
    buffer_.feed(data, [this](const std::uint8_t* block) { compress(block); });
  }
  Bytes finish() override {
    const std::size_t pad = 16 - buffer_.filled();
    std::array<std::uint8_t, 16> padding{};
    padding.fill(static_cast<std::uint8_t>(pad));
    update(as_bytes(std::span<const std::uint8_t>(padding.data(), pad)));
    const std::array<std::uint8_t, 16> checksum = checksum_;
    update(as_bytes(std::span<const std::uint8_t>(checksum.data(), checksum.size())));
    Bytes out(16);
    std::memcpy(out.data(), state_.data(), 16);
    return out;
  }

 private:
  void compress(const std::uint8_t* block) {
    for (std::size_t j = 0; j < 16; ++j) {
      state_[16 + j] = block[j];
      state_[32 + j] = static_cast<std::uint8_t>(state_[16 + j] ^ state_[j]);
    }
    std::uint8_t t = 0;
    for (std::size_t round = 0; round < 18; ++round) {
      for (std::uint8_t& x : state_) t = x = static_cast<std::uint8_t>(x ^ md2_s[t]);
      t = static_cast<std::uint8_t>(t + round);
    }
    std::uint8_t last = checksum_[15];
    for (std::size_t j = 0; j < 16; ++j) last = checksum_[j] = static_cast<std::uint8_t>(checksum_[j] ^ md2_s[block[j] ^ last]);
  }
  BlockBuffer<16> buffer_;
  std::array<std::uint8_t, 48> state_{};
  std::array<std::uint8_t, 16> checksum_{};
};

// --------------------------------------------------------------------------- MD4 (RFC 1320)

class Md4 final : public Hasher {
 public:
  void update(ByteView data) override {
    buffer_.feed(data, [this](const std::uint8_t* block) { compress(block); });
  }
  Bytes finish() override {
    buffer_.pad(8, false, [this](const std::uint8_t* block) { compress(block); });
    Bytes out;
    for (const std::uint32_t v : h_) store32le(out, v);
    return out;
  }

 private:
  void compress(const std::uint8_t* block) {
    std::array<std::uint32_t, 16> x{};
    for (std::size_t i = 0; i < 16; ++i) x[i] = load32le(block + 4 * i);
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    const auto f = [](std::uint32_t u, std::uint32_t v, std::uint32_t w) { return (u & v) | (~u & w); };
    const auto g = [](std::uint32_t u, std::uint32_t v, std::uint32_t w) { return (u & v) | (u & w) | (v & w); };
    const auto h = [](std::uint32_t u, std::uint32_t v, std::uint32_t w) { return u ^ v ^ w; };
    constexpr std::array<int, 4> s1 = {3, 7, 11, 19};
    constexpr std::array<int, 4> s2 = {3, 5, 9, 13};
    constexpr std::array<int, 4> s3 = {3, 9, 11, 15};
    constexpr std::array<std::size_t, 16> order3 = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    const auto step = [](std::uint32_t& r, std::uint32_t value, int shift) { r = rotl(r + value, shift); };
    for (std::size_t i = 0; i < 16; ++i) {
      const std::array<std::uint32_t*, 4> regs = {&a, &d, &c, &b};
      std::uint32_t& r = *regs[i % 4];
      const std::uint32_t u = *regs[(i + 3) % 4], v = *regs[(i + 2) % 4], w = *regs[(i + 1) % 4];
      step(r, f(u, v, w) + x[i], s1[i % 4]);
    }
    for (std::size_t i = 0; i < 16; ++i) {
      const std::array<std::uint32_t*, 4> regs = {&a, &d, &c, &b};
      std::uint32_t& r = *regs[i % 4];
      const std::uint32_t u = *regs[(i + 3) % 4], v = *regs[(i + 2) % 4], w = *regs[(i + 1) % 4];
      step(r, g(u, v, w) + x[(i % 4) * 4 + i / 4] + 0x5A827999u, s2[i % 4]);
    }
    for (std::size_t i = 0; i < 16; ++i) {
      const std::array<std::uint32_t*, 4> regs = {&a, &d, &c, &b};
      std::uint32_t& r = *regs[i % 4];
      const std::uint32_t u = *regs[(i + 3) % 4], v = *regs[(i + 2) % 4], w = *regs[(i + 1) % 4];
      step(r, h(u, v, w) + x[order3[i]] + 0x6ED9EBA1u, s3[i % 4]);
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
  }
  BlockBuffer<64> buffer_;
  std::array<std::uint32_t, 4> h_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u};
};

// --------------------------------------------------------------------------- MD5 and SHA-1 (util)

template <class Impl>
class UtilHasher final : public Hasher {
 public:
  void update(ByteView data) override { impl_.update(data); }
  Bytes finish() override {
    const auto digest = impl_.finish();
    Bytes out(digest.size());
    std::memcpy(out.data(), digest.data(), digest.size());
    return out;
  }

 private:
  Impl impl_;
};

// --------------------------------------------------------------------------- SHA-2 (FIPS 180-4)

// First 64 bits of the fractional parts of the cube roots of the first 80
// primes. SHA-256 uses the upper 32 bits of the first 64.
constexpr std::array<std::uint64_t, 80> sha512_k = {{
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
}};
// Fractional parts of the square roots of the first 8 primes (SHA-512, and
// SHA-256 in the upper halves) and of the 9th to 16th primes (SHA-384, and
// SHA-224 in the lower halves).
constexpr std::array<std::uint64_t, 8> sha512_iv = {{
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
}};
constexpr std::array<std::uint64_t, 8> sha384_iv = {{
    0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
    0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL,
}};

class Sha256Family final : public Hasher {
 public:
  Sha256Family(const std::array<std::uint32_t, 8>& iv, std::size_t digest_bytes) : h_(iv), digest_bytes_(digest_bytes) {}
  void update(ByteView data) override {
    buffer_.feed(data, [this](const std::uint8_t* block) { compress(block); });
  }
  Bytes finish() override {
    buffer_.pad(8, true, [this](const std::uint8_t* block) { compress(block); });
    Bytes out;
    for (const std::uint32_t v : h_) store32be(out, v);
    out.resize(digest_bytes_);
    return out;
  }

 private:
  void compress(const std::uint8_t* block) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) w[i] = load32be(block + 4 * i);
    for (std::size_t i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::array<std::uint32_t, 8> v = h_;
    for (std::size_t i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
      const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const std::uint32_t t1 = v[7] + s1 + ch + static_cast<std::uint32_t>(sha512_k[i] >> 32) + w[i];
      const std::uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
      const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const std::uint32_t t2 = s0 + maj;
      v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
    }
    for (std::size_t i = 0; i < 8; ++i) h_[i] += v[i];
  }
  BlockBuffer<64> buffer_;
  std::array<std::uint32_t, 8> h_;
  std::size_t digest_bytes_;
};

class Sha512Family final : public Hasher {
 public:
  Sha512Family(const std::array<std::uint64_t, 8>& iv, std::size_t digest_bytes) : h_(iv), digest_bytes_(digest_bytes) {}
  void update(ByteView data) override {
    buffer_.feed(data, [this](const std::uint8_t* block) { compress(block); });
  }
  Bytes finish() override {
    // 128-bit length; inputs never reach 2^61 bytes, so its upper half is zero.
    buffer_.pad(16, true, [this](const std::uint8_t* block) { compress(block); });
    Bytes out;
    for (const std::uint64_t v : h_) store64be(out, v);
    out.resize(digest_bytes_);
    return out;
  }
  [[nodiscard]] const std::array<std::uint64_t, 8>& state() const noexcept { return h_; }

 private:
  void compress(const std::uint8_t* block) {
    std::array<std::uint64_t, 80> w{};
    for (std::size_t i = 0; i < 16; ++i) w[i] = load64be(block + 8 * i);
    for (std::size_t i = 16; i < 80; ++i) {
      const std::uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
      const std::uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::array<std::uint64_t, 8> v = h_;
    for (std::size_t i = 0; i < 80; ++i) {
      const std::uint64_t s1 = rotr(v[4], 14) ^ rotr(v[4], 18) ^ rotr(v[4], 41);
      const std::uint64_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const std::uint64_t t1 = v[7] + s1 + ch + sha512_k[i] + w[i];
      const std::uint64_t s0 = rotr(v[0], 28) ^ rotr(v[0], 34) ^ rotr(v[0], 39);
      const std::uint64_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      v = {t1 + s0 + maj, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
    }
    for (std::size_t i = 0; i < 8; ++i) h_[i] += v[i];
  }
  BlockBuffer<128> buffer_;
  std::array<std::uint64_t, 8> h_;
  std::size_t digest_bytes_;
};

std::array<std::uint32_t, 8> sha256_iv() {
  std::array<std::uint32_t, 8> iv{};
  for (std::size_t i = 0; i < 8; ++i) iv[i] = static_cast<std::uint32_t>(sha512_iv[i] >> 32);
  return iv;
}
std::array<std::uint32_t, 8> sha224_iv() {
  std::array<std::uint32_t, 8> iv{};
  for (std::size_t i = 0; i < 8; ++i) iv[i] = static_cast<std::uint32_t>(sha384_iv[i]);
  return iv;
}

// FIPS 180-4 5.3.6: the IV of SHA-512/t is SHA-512 of the string "SHA-512/t"
// computed from the SHA-512 IV with every word XORed with a5a5a5a5a5a5a5a5.
std::array<std::uint64_t, 8> sha512_t_iv(std::size_t bits) {
  std::array<std::uint64_t, 8> iv = sha512_iv;
  for (std::uint64_t& v : iv) v ^= 0xa5a5a5a5a5a5a5a5ULL;
  Sha512Family generator(iv, 64);
  const std::string label = "SHA-512/" + std::to_string(bits);
  generator.update(as_bytes(label));
  const Bytes digest = generator.finish();
  for (std::size_t i = 0; i < 8; ++i) iv[i] = load64be(reinterpret_cast<const std::uint8_t*>(digest.data()) + 8 * i);
  return iv;
}

// --------------------------------------------------------------------------- RIPEMD-160

constexpr std::array<std::uint8_t, 80> rmd_r = {
    0, 1,  2,  3,  4,  5,  6,  7,  8, 9, 10, 11, 12, 13, 14, 15, 7,  4,  13, 1,  10, 6,  15, 3,  12, 0, 9,
    5, 2,  14, 11, 8,  3,  10, 14, 4, 9, 15, 8,  1,  2,  7,  0,  6,  13, 11, 5,  12, 1,  9,  11, 10, 0, 8,
    12, 4, 13, 3,  7,  15, 14, 5,  6, 2, 4,  0,  5,  9,  7,  12, 2,  10, 14, 1,  3,  8,  11, 6,  15, 13};
constexpr std::array<std::uint8_t, 80> rmd_rp = {
    5,  14, 7,  0, 9, 2,  11, 4,  13, 6,  15, 8,  1,  10, 3,  12, 6,  11, 3,  7, 0,  13, 5,  10, 14, 15, 8,
    12, 4,  9,  1, 2, 15, 5,  1,  3,  7,  14, 6,  9,  11, 8,  12, 2,  10, 0,  4, 13, 8,  6,  4,  1,  3,  11,
    15, 0,  5,  12, 2, 13, 9,  7,  10, 14, 12, 15, 10, 4,  1,  5,  8,  7,  6,  2, 13, 14, 0,  3,  9,  11};
constexpr std::array<std::uint8_t, 80> rmd_s = {
    11, 14, 15, 12, 5,  8,  7,  9,  11, 13, 14, 15, 6,  7,  9,  8,  7,  6,  8,  13, 11, 9,  7,  15, 7,  12, 15,
    9,  11, 7,  13, 12, 11, 13, 6,  7,  14, 9,  13, 15, 14, 8,  13, 6,  5,  12, 7,  5,  11, 12, 14, 15, 14, 15,
    9,  8,  9,  14, 5,  6,  8,  6,  5,  12, 9,  15, 5,  11, 6,  8,  13, 12, 5,  12, 13, 14, 11, 8,  5,  6};
constexpr std::array<std::uint8_t, 80> rmd_sp = {
    8,  9,  9,  11, 13, 15, 15, 5,  7,  7,  8,  11, 14, 14, 12, 6,  9,  13, 15, 7,  12, 8,  9,  11, 7,  7,  12,
    7,  6,  15, 13, 11, 9,  7,  15, 11, 8,  6,  6,  14, 12, 13, 5,  14, 13, 13, 7,  5,  15, 5,  8,  11, 14, 14,
    6,  14, 6,  9,  12, 9,  12, 5,  15, 8,  8,  5,  12, 9,  12, 5,  14, 6,  8,  13, 6,  5,  15, 13, 11, 11};

class Ripemd160 final : public Hasher {
 public:
  void update(ByteView data) override {
    buffer_.feed(data, [this](const std::uint8_t* block) { compress(block); });
  }
  Bytes finish() override {
    buffer_.pad(8, false, [this](const std::uint8_t* block) { compress(block); });
    Bytes out;
    for (const std::uint32_t v : h_) store32le(out, v);
    return out;
  }

 private:
  static std::uint32_t f(std::size_t j, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    switch (j / 16) {
      case 0: return x ^ y ^ z;
      case 1: return (x & y) | (~x & z);
      case 2: return (x | ~y) ^ z;
      case 3: return (x & z) | (y & ~z);
      default: return x ^ (y | ~z);
    }
  }
  void compress(const std::uint8_t* block) {
    constexpr std::array<std::uint32_t, 5> k = {0x00000000u, 0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xA953FD4Eu};
    constexpr std::array<std::uint32_t, 5> kp = {0x50A28BE6u, 0x5C4DD124u, 0x6D703EF3u, 0x7A6D76E9u, 0x00000000u};
    std::array<std::uint32_t, 16> x{};
    for (std::size_t i = 0; i < 16; ++i) x[i] = load32le(block + 4 * i);
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    std::uint32_t ap = a, bp = b, cp = c, dp = d, ep = e;
    for (std::size_t j = 0; j < 80; ++j) {
      std::uint32_t t = rotl(a + f(j, b, c, d) + x[rmd_r[j]] + k[j / 16], rmd_s[j]) + e;
      a = e;
      e = d;
      d = rotl(c, 10);
      c = b;
      b = t;
      t = rotl(ap + f(79 - j, bp, cp, dp) + x[rmd_rp[j]] + kp[j / 16], rmd_sp[j]) + ep;
      ap = ep;
      ep = dp;
      dp = rotl(cp, 10);
      cp = bp;
      bp = t;
    }
    const std::uint32_t t = h_[1] + c + dp;
    h_[1] = h_[2] + d + ep;
    h_[2] = h_[3] + e + ap;
    h_[3] = h_[4] + a + bp;
    h_[4] = h_[0] + b + cp;
    h_[0] = t;
  }
  BlockBuffer<64> buffer_;
  std::array<std::uint32_t, 5> h_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
};

// --------------------------------------------------------------------------- SM3 (GB/T 32905-2016)

class Sm3 final : public Hasher {
 public:
  void update(ByteView data) override {
    buffer_.feed(data, [this](const std::uint8_t* block) { compress(block); });
  }
  Bytes finish() override {
    buffer_.pad(8, true, [this](const std::uint8_t* block) { compress(block); });
    Bytes out;
    for (const std::uint32_t v : v_) store32be(out, v);
    return out;
  }

 private:
  static std::uint32_t p0(std::uint32_t x) { return x ^ rotl(x, 9) ^ rotl(x, 17); }
  static std::uint32_t p1(std::uint32_t x) { return x ^ rotl(x, 15) ^ rotl(x, 23); }
  void compress(const std::uint8_t* block) {
    std::array<std::uint32_t, 68> w{};
    for (std::size_t i = 0; i < 16; ++i) w[i] = load32be(block + 4 * i);
    for (std::size_t j = 16; j < 68; ++j) {
      w[j] = p1(w[j - 16] ^ w[j - 9] ^ rotl(w[j - 3], 15)) ^ rotl(w[j - 13], 7) ^ w[j - 6];
    }
    std::uint32_t a = v_[0], b = v_[1], c = v_[2], d = v_[3], e = v_[4], f = v_[5], g = v_[6], h = v_[7];
    for (std::size_t j = 0; j < 64; ++j) {
      const std::uint32_t t = j < 16 ? 0x79cc4519u : 0x7a879d8au;
      const std::uint32_t ss1 = rotl(rotl(a, 12) + e + rotl(t, static_cast<int>(j % 32)), 7);
      const std::uint32_t ss2 = ss1 ^ rotl(a, 12);
      const std::uint32_t ff = j < 16 ? (a ^ b ^ c) : ((a & b) | (a & c) | (b & c));
      const std::uint32_t gg = j < 16 ? (e ^ f ^ g) : ((e & f) | (~e & g));
      const std::uint32_t tt1 = ff + d + ss2 + (w[j] ^ w[j + 4]);
      const std::uint32_t tt2 = gg + h + ss1 + w[j];
      d = c;
      c = rotl(b, 9);
      b = a;
      a = tt1;
      h = g;
      g = rotl(f, 19);
      f = e;
      e = p0(tt2);
    }
    const std::array<std::uint32_t, 8> v = {a, b, c, d, e, f, g, h};
    for (std::size_t i = 0; i < 8; ++i) v_[i] ^= v[i];
  }
  BlockBuffer<64> buffer_;
  std::array<std::uint32_t, 8> v_ = {0x7380166fu, 0x4914b2b9u, 0x172442d7u, 0xda8a0600u,
                                     0xa96f30bcu, 0x163138aau, 0xe38dee4du, 0xb0fb0e4eu};
};

// --------------------------------------------------------------------------- Whirlpool (ISO/IEC 10118-3)

// The S-box is built from the mini-boxes E and R as described by Barreto and
// Rijmen; the round tables multiply it by the circulant matrix
// cir(1, 1, 4, 1, 8, 5, 2, 9) over GF(2^8) with the polynomial x^8+x^4+x^3+x^2+1.
struct WhirlpoolTables {
  std::array<std::uint8_t, 256> sbox{};
  std::array<std::uint64_t, 256> c0{};
  std::array<std::uint64_t, 11> rc{};
};

constexpr std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) noexcept {
  std::uint32_t product = 0;
  std::uint32_t x = a;
  for (int bit = 0; bit < 8; ++bit) {
    if ((b >> bit) & 1u) product ^= x;
    x <<= 1;
    if (x & 0x100u) x ^= 0x11Du;
  }
  return static_cast<std::uint8_t>(product);
}

constexpr WhirlpoolTables make_whirlpool_tables() noexcept {
  constexpr std::array<std::uint8_t, 16> e = {0x1, 0xB, 0x9, 0xC, 0xD, 0x6, 0xF, 0x3,
                                              0xE, 0x8, 0x7, 0x4, 0xA, 0x2, 0x5, 0x0};
  constexpr std::array<std::uint8_t, 16> r = {0x7, 0xC, 0xB, 0xD, 0xE, 0x4, 0x9, 0xF,
                                              0x6, 0x3, 0x8, 0xA, 0x2, 0x5, 0x1, 0x0};
  std::array<std::uint8_t, 16> e_inv{};
  for (std::uint8_t i = 0; i < 16; ++i) e_inv[e[i]] = i;
  WhirlpoolTables t;
  for (std::size_t u = 0; u < 256; ++u) {
    const std::uint8_t a = e[u >> 4];
    const std::uint8_t b = e_inv[u & 15];
    const std::uint8_t mix = r[a ^ b];
    t.sbox[u] = static_cast<std::uint8_t>((e[a ^ mix] << 4) | e_inv[b ^ mix]);
  }
  constexpr std::array<std::uint8_t, 8> row = {1, 1, 4, 1, 8, 5, 2, 9};
  for (std::size_t x = 0; x < 256; ++x) {
    std::uint64_t value = 0;
    for (std::size_t j = 0; j < 8; ++j) value = (value << 8) | gf_mul(t.sbox[x], row[j]);
    t.c0[x] = value;
  }
  for (std::size_t round = 1; round <= 10; ++round) {
    std::uint64_t value = 0;
    for (std::size_t j = 0; j < 8; ++j) value = (value << 8) | t.sbox[8 * (round - 1) + j];
    t.rc[round] = value;
  }
  return t;
}

constexpr WhirlpoolTables whirlpool_tables = make_whirlpool_tables();

class Whirlpool final : public Hasher {
 public:
  void update(ByteView data) override {
    buffer_.feed(data, [this](const std::uint8_t* block) { compress(block); });
  }
  Bytes finish() override {
    buffer_.pad(32, true, [this](const std::uint8_t* block) { compress(block); });
    Bytes out;
    for (const std::uint64_t v : h_) store64be(out, v);
    return out;
  }

 private:
  using State = std::array<std::uint64_t, 8>;
  static State round(const State& k) {
    State l{};
    for (std::size_t i = 0; i < 8; ++i) {
      std::uint64_t v = 0;
      for (std::size_t j = 0; j < 8; ++j) {
        const auto byte = static_cast<std::uint8_t>(k[(i - j) & 7] >> (56 - 8 * j));
        v ^= rotr(whirlpool_tables.c0[byte], static_cast<int>(8 * j));
      }
      l[i] = v;
    }
    return l;
  }
  void compress(const std::uint8_t* block) {
    State m{};
    for (std::size_t i = 0; i < 8; ++i) m[i] = load64be(block + 8 * i);
    State k = h_;
    State s{};
    for (std::size_t i = 0; i < 8; ++i) s[i] = m[i] ^ k[i];
    for (std::size_t r = 1; r <= 10; ++r) {
      k = round(k);
      k[0] ^= whirlpool_tables.rc[r];
      const State next = round(s);
      for (std::size_t i = 0; i < 8; ++i) s[i] = next[i] ^ k[i];
    }
    for (std::size_t i = 0; i < 8; ++i) h_[i] ^= s[i] ^ m[i];
  }
  BlockBuffer<64> buffer_;
  State h_{};
};

}  // namespace

std::unique_ptr<Hasher> make_md2(const HashParams&) { return std::make_unique<Md2>(); }
std::unique_ptr<Hasher> make_md4(const HashParams&) { return std::make_unique<Md4>(); }
std::unique_ptr<Hasher> make_md5(const HashParams&) { return std::make_unique<UtilHasher<util::Md5>>(); }
std::unique_ptr<Hasher> make_sha1(const HashParams&) { return std::make_unique<UtilHasher<util::Sha1>>(); }
std::unique_ptr<Hasher> make_sha224(const HashParams&) { return std::make_unique<Sha256Family>(sha224_iv(), 28); }
std::unique_ptr<Hasher> make_sha256(const HashParams&) { return std::make_unique<Sha256Family>(sha256_iv(), 32); }
std::unique_ptr<Hasher> make_sha384(const HashParams&) { return std::make_unique<Sha512Family>(sha384_iv, 48); }
std::unique_ptr<Hasher> make_sha512(const HashParams&) { return std::make_unique<Sha512Family>(sha512_iv, 64); }
std::unique_ptr<Hasher> make_sha512_t(std::size_t bytes) {
  bytes = std::clamp<std::size_t>(bytes, 1, 63);
  return std::make_unique<Sha512Family>(sha512_t_iv(bytes * 8), bytes);
}
std::unique_ptr<Hasher> make_sha512_224(const HashParams&) { return make_sha512_t(28); }
std::unique_ptr<Hasher> make_sha512_256(const HashParams&) { return make_sha512_t(32); }
std::unique_ptr<Hasher> make_ripemd160(const HashParams&) { return std::make_unique<Ripemd160>(); }
std::unique_ptr<Hasher> make_sm3(const HashParams&) { return std::make_unique<Sm3>(); }
std::unique_ptr<Hasher> make_whirlpool(const HashParams&) { return std::make_unique<Whirlpool>(); }

}  // namespace cvt::encoding
