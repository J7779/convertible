// Convertible core - encoding family: non-cryptographic hashes and checksums
// (xxHash32/64, XXH3-64, MurmurHash3, FNV-1/1a, SipHash-2-4, CRC catalogue,
// Adler-32, Fletcher-16/32/64).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "util/bytes.hpp"
#include "families/encoding/hashes.hpp"
#include "util/crc.hpp"

namespace cvt::encoding {

namespace {

using std::rotl;

std::uint32_t rd32(const std::uint8_t* p) noexcept {
  return (std::uint32_t{p[3]} << 24) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[1]} << 8) | p[0];
}
std::uint64_t rd64(const std::uint8_t* p) noexcept { return (std::uint64_t{rd32(p + 4)} << 32) | rd32(p); }

Bytes big_endian(std::uint64_t value, std::size_t bytes) {
  Bytes out(bytes);
  for (std::size_t i = 0; i < bytes; ++i) out[bytes - 1 - i] = static_cast<std::byte>(value >> (8 * i));
  return out;
}

/// Base for algorithms that are simplest to compute over the whole input.
/// `update` collects a copy; `digest_of` hashes a buffer in place.
class WholeInput : public Hasher {
 public:
  void update(ByteView data) override { owned_.insert(owned_.end(), data.begin(), data.end()); }
  Bytes finish() final { return digest(reinterpret_cast<const std::uint8_t*>(owned_.data()), owned_.size()); }
  [[nodiscard]] bool whole_input() const noexcept final { return true; }
  Bytes digest_of(ByteView data) final {
    if (!owned_.empty()) return Hasher::digest_of(data);
    return digest(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
  }

 protected:
  virtual Bytes digest(const std::uint8_t* p, std::size_t n) = 0;

 private:
  Bytes owned_;
};

// --------------------------------------------------------------------------- xxHash

constexpr std::uint32_t p32_1 = 0x9E3779B1u, p32_2 = 0x85EBCA77u, p32_3 = 0xC2B2AE3Du, p32_4 = 0x27D4EB2Fu,
                        p32_5 = 0x165667B1u;
constexpr std::uint64_t p64_1 = 0x9E3779B185EBCA87ULL, p64_2 = 0xC2B2AE3D27D4EB4FULL, p64_3 = 0x165667B19E3779F9ULL,
                        p64_4 = 0x85EBCA77C2B2AE63ULL, p64_5 = 0x27D4EB2F165667C5ULL;

std::uint32_t xxh32(const std::uint8_t* p, std::size_t n, std::uint32_t seed) {
  const auto round = [](std::uint32_t acc, std::uint32_t input) { return rotl(acc + input * p32_2, 13) * p32_1; };
  const std::uint8_t* const end = p + n;
  std::uint32_t h = 0;
  if (n >= 16) {
    std::uint32_t v1 = seed + p32_1 + p32_2, v2 = seed + p32_2, v3 = seed, v4 = seed - p32_1;
    while (end - p >= 16) {
      v1 = round(v1, rd32(p));
      v2 = round(v2, rd32(p + 4));
      v3 = round(v3, rd32(p + 8));
      v4 = round(v4, rd32(p + 12));
      p += 16;
    }
    h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
  } else {
    h = seed + p32_5;
  }
  h += static_cast<std::uint32_t>(n);
  while (end - p >= 4) {
    h = rotl(h + rd32(p) * p32_3, 17) * p32_4;
    p += 4;
  }
  while (p < end) h = rotl(h + *p++ * p32_5, 11) * p32_1;
  h ^= h >> 15;
  h *= p32_2;
  h ^= h >> 13;
  h *= p32_3;
  h ^= h >> 16;
  return h;
}

std::uint64_t xxh64_avalanche(std::uint64_t h) {
  h ^= h >> 33;
  h *= p64_2;
  h ^= h >> 29;
  h *= p64_3;
  h ^= h >> 32;
  return h;
}

std::uint64_t xxh64(const std::uint8_t* p, std::size_t n, std::uint64_t seed) {
  const auto round = [](std::uint64_t acc, std::uint64_t input) { return rotl(acc + input * p64_2, 31) * p64_1; };
  const auto merge = [&round](std::uint64_t acc, std::uint64_t v) { return (acc ^ round(0, v)) * p64_1 + p64_4; };
  const std::uint8_t* const end = p + n;
  std::uint64_t h = 0;
  if (n >= 32) {
    std::uint64_t v1 = seed + p64_1 + p64_2, v2 = seed + p64_2, v3 = seed, v4 = seed - p64_1;
    while (end - p >= 32) {
      v1 = round(v1, rd64(p));
      v2 = round(v2, rd64(p + 8));
      v3 = round(v3, rd64(p + 16));
      v4 = round(v4, rd64(p + 24));
      p += 32;
    }
    h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    h = merge(merge(merge(merge(h, v1), v2), v3), v4);
  } else {
    h = seed + p64_5;
  }
  h += n;
  while (end - p >= 8) {
    h = rotl(h ^ round(0, rd64(p)), 27) * p64_1 + p64_4;
    p += 8;
  }
  if (end - p >= 4) {
    h = rotl(h ^ (std::uint64_t{rd32(p)} * p64_1), 23) * p64_2 + p64_3;
    p += 4;
  }
  while (p < end) h = rotl(h ^ (*p++ * p64_5), 11) * p64_1;
  return xxh64_avalanche(h);
}

// --------------------------------------------------------------------------- XXH3 (64-bit)

// The default secret of the reference implementation (xxhash.h, XXH3_kSecret).
constexpr std::array<std::uint8_t, 192> xxh3_secret = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c, 0xde, 0xd4, 0x6d,
    0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f, 0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0,
    0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21, 0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0,
    0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c, 0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b,
    0x1b, 0x53, 0x2e, 0xa3, 0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac,
    0xd8, 0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d, 0x8a, 0x51,
    0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64, 0xea, 0xc5, 0xac, 0x83, 0x34,
    0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb, 0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49,
    0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e, 0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8,
    0xd1, 0x7a, 0xd0, 0x31, 0xce, 0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b,
    0x40, 0x7e,
};
constexpr std::uint64_t prime_mx1 = 0x165667919E3779F9ULL, prime_mx2 = 0x9FB21C651E98DF25ULL;

// The 128-bit product of two 64-bit values, folded to 64 bits (low ^ high),
// computed portably from 32-bit halves.
std::uint64_t mul128_fold64(std::uint64_t a, std::uint64_t b) {
  const std::uint64_t a_lo = a & 0xFFFFFFFFu, a_hi = a >> 32, b_lo = b & 0xFFFFFFFFu, b_hi = b >> 32;
  const std::uint64_t lo_lo = a_lo * b_lo, hi_lo = a_hi * b_lo, lo_hi = a_lo * b_hi, hi_hi = a_hi * b_hi;
  const std::uint64_t cross = (lo_lo >> 32) + (hi_lo & 0xFFFFFFFFu) + lo_hi;
  const std::uint64_t upper = (hi_lo >> 32) + (cross >> 32) + hi_hi;
  const std::uint64_t lower = (cross << 32) | (lo_lo & 0xFFFFFFFFu);
  return lower ^ upper;
}

std::uint64_t xxh3_avalanche(std::uint64_t h) {
  h ^= h >> 37;
  h *= prime_mx1;
  h ^= h >> 32;
  return h;
}

std::uint64_t xxh3_rrmxmx(std::uint64_t h, std::uint64_t length) {
  h ^= rotl(h, 49) ^ rotl(h, 24);
  h *= prime_mx2;
  h ^= (h >> 35) + length;
  h *= prime_mx2;
  h ^= h >> 28;
  return h;
}

std::uint64_t xxh3_mix16(const std::uint8_t* in, const std::uint8_t* secret, std::uint64_t seed) {
  return mul128_fold64(rd64(in) ^ (rd64(secret) + seed), rd64(in + 8) ^ (rd64(secret + 8) - seed));
}

std::uint64_t xxh3_64(const std::uint8_t* in, std::size_t n, std::uint64_t seed) {
  const std::uint8_t* s = xxh3_secret.data();
  const std::uint64_t len = n;
  if (n == 0) return xxh64_avalanche(seed ^ (rd64(s + 56) ^ rd64(s + 64)));
  if (n <= 3) {
    const std::uint32_t combined = (std::uint32_t{in[0]} << 16) | (std::uint32_t{in[n >> 1]} << 24) |
                                   std::uint32_t{in[n - 1]} | (static_cast<std::uint32_t>(n) << 8);
    const std::uint64_t bitflip = (rd32(s) ^ rd32(s + 4)) + seed;
    return xxh64_avalanche(std::uint64_t{combined} ^ bitflip);
  }
  if (n <= 8) {
    seed ^= std::uint64_t{std::byteswap(static_cast<std::uint32_t>(seed))} << 32;
    const std::uint64_t bitflip = (rd64(s + 8) ^ rd64(s + 16)) - seed;
    const std::uint64_t input64 = rd32(in + n - 4) + (std::uint64_t{rd32(in)} << 32);
    return xxh3_rrmxmx(input64 ^ bitflip, len);
  }
  if (n <= 16) {
    const std::uint64_t bitflip1 = (rd64(s + 24) ^ rd64(s + 32)) + seed;
    const std::uint64_t bitflip2 = (rd64(s + 40) ^ rd64(s + 48)) - seed;
    const std::uint64_t lo = rd64(in) ^ bitflip1;
    const std::uint64_t hi = rd64(in + n - 8) ^ bitflip2;
    return xxh3_avalanche(len + std::byteswap(lo) + hi + mul128_fold64(lo, hi));
  }
  if (n <= 128) {
    std::uint64_t acc = len * p64_1;
    if (n > 32) {
      if (n > 64) {
        if (n > 96) {
          acc += xxh3_mix16(in + 48, s + 96, seed);
          acc += xxh3_mix16(in + n - 64, s + 112, seed);
        }
        acc += xxh3_mix16(in + 32, s + 64, seed);
        acc += xxh3_mix16(in + n - 48, s + 80, seed);
      }
      acc += xxh3_mix16(in + 16, s + 32, seed);
      acc += xxh3_mix16(in + n - 32, s + 48, seed);
    }
    acc += xxh3_mix16(in, s, seed);
    acc += xxh3_mix16(in + n - 16, s + 16, seed);
    return xxh3_avalanche(acc);
  }
  if (n <= 240) {
    std::uint64_t acc = len * p64_1;
    const std::size_t rounds = n / 16;
    for (std::size_t i = 0; i < 8; ++i) acc += xxh3_mix16(in + 16 * i, s + 16 * i, seed);
    acc = xxh3_avalanche(acc);
    for (std::size_t i = 8; i < rounds; ++i) acc += xxh3_mix16(in + 16 * i, s + 16 * (i - 8) + 3, seed);
    acc += xxh3_mix16(in + n - 16, s + 136 - 17, seed);
    return xxh3_avalanche(acc);
  }

  // Long input: a seed is folded into a custom secret first.
  std::array<std::uint8_t, 192> custom = xxh3_secret;
  if (seed != 0) {
    for (std::size_t i = 0; i < 192; i += 16) {
      const std::uint64_t lo = rd64(xxh3_secret.data() + i) + seed;
      const std::uint64_t hi = rd64(xxh3_secret.data() + i + 8) - seed;
      for (std::size_t b = 0; b < 8; ++b) {
        custom[i + b] = static_cast<std::uint8_t>(lo >> (8 * b));
        custom[i + 8 + b] = static_cast<std::uint8_t>(hi >> (8 * b));
      }
    }
  }
  s = custom.data();
  constexpr std::size_t secret_size = 192, stripe = 64, stripes_per_block = (secret_size - stripe) / 8;
  constexpr std::size_t block = stripe * stripes_per_block;
  std::array<std::uint64_t, 8> acc = {p32_3, p64_1, p64_2, p64_3, p64_4, p32_2, p64_5, p32_1};
  const auto accumulate_512 = [&acc](const std::uint8_t* data, const std::uint8_t* key) {
    for (std::size_t i = 0; i < 8; ++i) {
      const std::uint64_t value = rd64(data + 8 * i);
      const std::uint64_t keyed = value ^ rd64(key + 8 * i);
      acc[i ^ 1] += value;
      acc[i] += (keyed & 0xFFFFFFFFu) * (keyed >> 32);
    }
  };
  const auto scramble = [&acc](const std::uint8_t* key) {
    for (std::size_t i = 0; i < 8; ++i) {
      std::uint64_t a = acc[i];
      a ^= a >> 47;
      a ^= rd64(key + 8 * i);
      a *= p32_1;
      acc[i] = a;
    }
  };
  const std::size_t blocks = (n - 1) / block;
  for (std::size_t b = 0; b < blocks; ++b) {
    for (std::size_t k = 0; k < stripes_per_block; ++k) accumulate_512(in + b * block + k * stripe, s + k * 8);
    scramble(s + secret_size - stripe);
  }
  const std::size_t last_stripes = ((n - 1) - block * blocks) / stripe;
  for (std::size_t k = 0; k < last_stripes; ++k) accumulate_512(in + blocks * block + k * stripe, s + k * 8);
  accumulate_512(in + n - stripe, s + secret_size - stripe - 7);
  std::uint64_t result = len * p64_1;
  for (std::size_t i = 0; i < 4; ++i) {
    result += mul128_fold64(acc[2 * i] ^ rd64(s + 11 + 16 * i), acc[2 * i + 1] ^ rd64(s + 11 + 16 * i + 8));
  }
  return xxh3_avalanche(result);
}

class Xxh final : public WholeInput {
 public:
  enum class Variant { X32, X64, X3 };
  Xxh(Variant variant, std::uint64_t seed) : variant_(variant), seed_(seed) {}

 protected:
  Bytes digest(const std::uint8_t* p, std::size_t n) override {
    switch (variant_) {
      case Variant::X32: return big_endian(xxh32(p, n, static_cast<std::uint32_t>(seed_)), 4);
      case Variant::X64: return big_endian(xxh64(p, n, seed_), 8);
      case Variant::X3: return big_endian(xxh3_64(p, n, seed_), 8);
    }
    return {};
  }

 private:
  Variant variant_;
  std::uint64_t seed_;
};

// --------------------------------------------------------------------------- MurmurHash3

std::uint32_t fmix32(std::uint32_t h) {
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}
std::uint64_t fmix64(std::uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

class Murmur3 final : public WholeInput {
 public:
  Murmur3(bool wide, std::uint32_t seed) : wide_(wide), seed_(seed) {}

 protected:
  Bytes digest(const std::uint8_t* p, std::size_t n) override { return wide_ ? x64_128(p, n) : x86_32(p, n); }

 private:
  [[nodiscard]] Bytes x86_32(const std::uint8_t* p, std::size_t n) const {
    constexpr std::uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    std::uint32_t h = seed_;
    const std::size_t blocks = n / 4;
    for (std::size_t i = 0; i < blocks; ++i) {
      std::uint32_t k = rd32(p + 4 * i);
      k = rotl(k * c1, 15) * c2;
      h = rotl(h ^ k, 13) * 5 + 0xe6546b64u;
    }
    const std::uint8_t* tail = p + blocks * 4;
    std::uint32_t k = 0;
    switch (n & 3) {
      case 3: k ^= std::uint32_t{tail[2]} << 16; [[fallthrough]];
      case 2: k ^= std::uint32_t{tail[1]} << 8; [[fallthrough]];
      case 1:
        k ^= tail[0];
        h ^= rotl(k * c1, 15) * c2;
        break;
      default: break;
    }
    h ^= static_cast<std::uint32_t>(n);
    return big_endian(fmix32(h), 4);
  }
  [[nodiscard]] Bytes x64_128(const std::uint8_t* p, std::size_t n) const {
    constexpr std::uint64_t c1 = 0x87c37b91114253d5ULL, c2 = 0x4cf5ad432745937fULL;
    std::uint64_t h1 = seed_, h2 = seed_;
    const std::size_t blocks = n / 16;
    for (std::size_t i = 0; i < blocks; ++i) {
      std::uint64_t k1 = rd64(p + 16 * i), k2 = rd64(p + 16 * i + 8);
      h1 ^= rotl(k1 * c1, 31) * c2;
      h1 = (rotl(h1, 27) + h2) * 5 + 0x52dce729u;
      h2 ^= rotl(k2 * c2, 33) * c1;
      h2 = (rotl(h2, 31) + h1) * 5 + 0x38495ab5u;
    }
    const std::uint8_t* tail = p + blocks * 16;
    std::uint64_t k1 = 0, k2 = 0;
    const std::size_t rest = n & 15;
    for (std::size_t i = rest; i-- > 8;) k2 ^= std::uint64_t{tail[i]} << (8 * (i - 8));
    for (std::size_t i = std::min<std::size_t>(rest, 8); i-- > 0;) k1 ^= std::uint64_t{tail[i]} << (8 * i);
    if (rest > 8) h2 ^= rotl(k2 * c2, 33) * c1;
    if (rest > 0) h1 ^= rotl(k1 * c1, 31) * c2;
    h1 ^= n;
    h2 ^= n;
    h1 += h2;
    h2 += h1;
    h1 = fmix64(h1);
    h2 = fmix64(h2);
    h1 += h2;
    h2 += h1;
    Bytes out = big_endian(h1, 8);
    const Bytes second = big_endian(h2, 8);
    out.insert(out.end(), second.begin(), second.end());
    return out;
  }
  bool wide_;
  std::uint32_t seed_;
};

// --------------------------------------------------------------------------- FNV

template <class Word>
class Fnv final : public Hasher {
 public:
  Fnv(Word offset, Word prime, bool xor_first) : h_(offset), prime_(prime), xor_first_(xor_first) {}
  void update(ByteView data) override {
    for (const std::byte b : data) {
      const auto v = std::to_integer<std::uint8_t>(b);
      if (xor_first_) {
        h_ = static_cast<Word>((h_ ^ v) * prime_);
      } else {
        h_ = static_cast<Word>((h_ * prime_) ^ v);
      }
    }
  }
  Bytes finish() override { return big_endian(h_, sizeof(Word)); }

 private:
  Word h_;
  Word prime_;
  bool xor_first_;
};

// --------------------------------------------------------------------------- SipHash-2-4

class SipHash final : public WholeInput {
 public:
  explicit SipHash(const Bytes& key) {
    std::array<std::uint8_t, 16> k{};
    if (key.size() == 16) {
      ::cvt::util::copy_bytes(k.data(), key.data(), 16);
    } else {
      for (std::uint8_t i = 0; i < 16; ++i) k[i] = i;  // the reference test key 00 01 .. 0f
    }
    k0_ = rd64(k.data());
    k1_ = rd64(k.data() + 8);
  }

 protected:
  Bytes digest(const std::uint8_t* p, std::size_t n) override {
    std::uint64_t v0 = k0_ ^ 0x736f6d6570736575ULL, v1 = k1_ ^ 0x646f72616e646f6dULL;
    std::uint64_t v2 = k0_ ^ 0x6c7967656e657261ULL, v3 = k1_ ^ 0x7465646279746573ULL;
    const auto sipround = [&] {
      v0 += v1;
      v1 = rotl(v1, 13) ^ v0;
      v0 = rotl(v0, 32);
      v2 += v3;
      v3 = rotl(v3, 16) ^ v2;
      v0 += v3;
      v3 = rotl(v3, 21) ^ v0;
      v2 += v1;
      v1 = rotl(v1, 17) ^ v2;
      v2 = rotl(v2, 32);
    };
    const auto absorb = [&](std::uint64_t m) {
      v3 ^= m;
      sipround();
      sipround();
      v0 ^= m;
    };
    const std::size_t whole = n / 8 * 8;
    for (std::size_t i = 0; i < whole; i += 8) absorb(rd64(p + i));
    std::uint64_t last = static_cast<std::uint64_t>(n & 0xFF) << 56;
    for (std::size_t i = whole; i < n; ++i) last |= std::uint64_t{p[i]} << (8 * (i - whole));
    absorb(last);
    v2 ^= 0xFF;
    for (int i = 0; i < 4; ++i) sipround();
    const std::uint64_t result = v0 ^ v1 ^ v2 ^ v3;
    // Byte order of the reference implementation's output (little-endian).
    Bytes out(8);
    for (std::size_t i = 0; i < 8; ++i) out[i] = static_cast<std::byte>(result >> (8 * i));
    return out;
  }

 private:
  std::uint64_t k0_ = 0;
  std::uint64_t k1_ = 0;
};

// --------------------------------------------------------------------------- CRC

constexpr std::uint64_t ones = ~std::uint64_t{0};

// Parameters and check values from the catalogue of parametrised CRC algorithms.
constexpr std::array<CrcModel, 31> crc_catalogue = {{
    {"crc8", "CRC-8/SMBUS", 8, 0x07, 0x00, false, false, 0x00, 0xF4},
    {"crc8-maxim", "CRC-8/MAXIM-DOW", 8, 0x31, 0x00, true, true, 0x00, 0xA1},
    {"crc8-autosar", "CRC-8/AUTOSAR", 8, 0x2F, 0xFF, false, false, 0xFF, 0xDF},
    {"crc8-cdma2000", "CRC-8/CDMA2000", 8, 0x9B, 0xFF, false, false, 0x00, 0xDA},
    {"crc8-wcdma", "CRC-8/WCDMA", 8, 0x9B, 0x00, true, true, 0x00, 0x25},
    {"crc8-bluetooth", "CRC-8/BLUETOOTH", 8, 0xA7, 0x00, true, true, 0x00, 0x26},
    {"crc8-rohc", "CRC-8/ROHC", 8, 0x07, 0xFF, true, true, 0x00, 0xD0},
    {"crc16", "CRC-16/ARC", 16, 0x8005, 0x0000, true, true, 0x0000, 0xBB3D},
    {"crc16-ccitt-false", "CRC-16/IBM-3740 (CCITT-FALSE)", 16, 0x1021, 0xFFFF, false, false, 0x0000, 0x29B1},
    {"crc16-xmodem", "CRC-16/XMODEM", 16, 0x1021, 0x0000, false, false, 0x0000, 0x31C3},
    {"crc16-kermit", "CRC-16/KERMIT", 16, 0x1021, 0x0000, true, true, 0x0000, 0x2189},
    {"crc16-modbus", "CRC-16/MODBUS", 16, 0x8005, 0xFFFF, true, true, 0x0000, 0x4B37},
    {"crc16-x25", "CRC-16/IBM-SDLC (X-25)", 16, 0x1021, 0xFFFF, true, true, 0xFFFF, 0x906E},
    {"crc16-usb", "CRC-16/USB", 16, 0x8005, 0xFFFF, true, true, 0xFFFF, 0xB4C8},
    {"crc16-maxim", "CRC-16/MAXIM-DOW", 16, 0x8005, 0x0000, true, true, 0xFFFF, 0x44C2},
    {"crc16-dnp", "CRC-16/DNP", 16, 0x3D65, 0x0000, true, true, 0xFFFF, 0xEA82},
    {"crc16-genibus", "CRC-16/GENIBUS", 16, 0x1021, 0xFFFF, false, false, 0xFFFF, 0xD64E},
    {"crc16-umts", "CRC-16/UMTS (BUYPASS)", 16, 0x8005, 0x0000, false, false, 0x0000, 0xFEE8},
    {"crc24-openpgp", "CRC-24/OPENPGP", 24, 0x864CFB, 0xB704CE, false, false, 0x000000, 0x21CF02},
    {"crc32", "CRC-32", 32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0xFFFFFFFF, 0xCBF43926},
    {"crc32c", "CRC-32C/ISCSI (Castagnoli)", 32, 0x1EDC6F41, 0xFFFFFFFF, true, true, 0xFFFFFFFF, 0xE3069283},
    {"crc32-bzip2", "CRC-32/BZIP2", 32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0xFFFFFFFF, 0xFC891918},
    {"crc32-mpeg2", "CRC-32/MPEG-2", 32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0x00000000, 0x0376E6E7},
    {"crc32-posix", "CRC-32/CKSUM", 32, 0x04C11DB7, 0x00000000, false, false, 0xFFFFFFFF, 0x765E7680},
    {"crc32-jamcrc", "CRC-32/JAMCRC", 32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0x00000000, 0x340BC6D9},
    {"crc32-autosar", "CRC-32/AUTOSAR", 32, 0xF4ACFB13, 0xFFFFFFFF, true, true, 0xFFFFFFFF, 0x1697D06A},
    {"crc32-xfer", "CRC-32/XFER", 32, 0x000000AF, 0x00000000, false, false, 0x00000000, 0xBD0BE338},
    {"crc64", "CRC-64/ECMA-182", 64, 0x42F0E1EBA9EA3693ULL, 0, false, false, 0, 0x6C40DF5F0B497347ULL},
    {"crc64-xz", "CRC-64/XZ", 64, 0x42F0E1EBA9EA3693ULL, ones, true, true, ones, 0x995DC9BBDF1939FAULL},
    {"crc64-go-iso", "CRC-64/GO-ISO", 64, 0x000000000000001BULL, ones, true, true, ones, 0xB90956C775A41001ULL},
    {"crc64-we", "CRC-64/WE", 64, 0x42F0E1EBA9EA3693ULL, ones, false, false, ones, 0x62EC59E3F1A4F00AULL},
}};

constexpr std::uint64_t width_mask(unsigned width) noexcept { return width >= 64 ? ones : (std::uint64_t{1} << width) - 1; }
constexpr std::uint64_t reflect(std::uint64_t value, unsigned width) noexcept {
  std::uint64_t out = 0;
  for (unsigned i = 0; i < width; ++i) {
    out = (out << 1) | (value & 1u);
    value >>= 1;
  }
  return out;
}

class Crc final : public Hasher {
 public:
  explicit Crc(const CrcModel& model) : model_(model), mask_(width_mask(model.width)) {
    const std::uint64_t poly = model.refin ? reflect(model.poly, model.width) : model.poly;
    for (std::uint64_t i = 0; i < 256; ++i) {
      std::uint64_t c = 0;
      if (model.refin) {
        c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ poly : c >> 1;
      } else {
        c = i << (model.width - 8);
        const std::uint64_t top = std::uint64_t{1} << (model.width - 1);
        for (int k = 0; k < 8; ++k) c = (c & top) ? ((c << 1) ^ poly) & mask_ : (c << 1) & mask_;
      }
      table_[i] = c;
    }
    crc_ = model.refin ? reflect(model.init, model.width) : model.init;
  }
  void update(ByteView data) override {
    const unsigned shift = model_.width - 8;
    for (const std::byte b : data) {
      const auto v = std::to_integer<std::uint64_t>(b);
      if (model_.refin) {
        crc_ = (crc_ >> 8) ^ table_[(crc_ ^ v) & 0xFFu];
      } else {
        crc_ = ((crc_ << 8) ^ table_[((crc_ >> shift) ^ v) & 0xFFu]) & mask_;
      }
    }
  }
  Bytes finish() override {
    std::uint64_t value = crc_;
    if (model_.refin != model_.refout) value = reflect(value, model_.width);
    return big_endian((value ^ model_.xorout) & mask_, (model_.width + 7) / 8);
  }

 private:
  CrcModel model_;
  std::uint64_t mask_;
  std::array<std::uint64_t, 256> table_{};
  std::uint64_t crc_ = 0;
};

// --------------------------------------------------------------------------- Adler and Fletcher

class Adler32 final : public Hasher {
 public:
  void update(ByteView data) override { value_ = util::adler32(data, value_); }
  Bytes finish() override { return big_endian(value_, 4); }

 private:
  std::uint32_t value_ = 1;
};

/// Fletcher checksums over little-endian words of `word_bytes` bytes (an odd
/// tail is padded with zeros), modulo 2^(8 * word_bytes) - 1.
class Fletcher final : public Hasher {
 public:
  explicit Fletcher(std::size_t word_bytes) : word_bytes_(word_bytes), modulus_((std::uint64_t{1} << (8 * word_bytes)) - 1) {}
  void update(ByteView data) override {
    for (const std::byte b : data) {
      word_ |= std::to_integer<std::uint64_t>(b) << (8 * filled_);
      if (++filled_ == word_bytes_) flush();
    }
  }
  Bytes finish() override {
    if (filled_ > 0) flush();
    return big_endian((sum2_ << (8 * word_bytes_)) | sum1_, 2 * word_bytes_);
  }

 private:
  void flush() {
    sum1_ = (sum1_ + word_) % modulus_;
    sum2_ = (sum2_ + sum1_) % modulus_;
    word_ = 0;
    filled_ = 0;
  }
  std::size_t word_bytes_;
  std::uint64_t modulus_;
  std::uint64_t sum1_ = 0;
  std::uint64_t sum2_ = 0;
  std::uint64_t word_ = 0;
  std::size_t filled_ = 0;
};

}  // namespace

std::unique_ptr<Hasher> make_xxh32(const HashParams& p) { return std::make_unique<Xxh>(Xxh::Variant::X32, p.seed); }
std::unique_ptr<Hasher> make_xxh64(const HashParams& p) { return std::make_unique<Xxh>(Xxh::Variant::X64, p.seed); }
std::unique_ptr<Hasher> make_xxh3_64(const HashParams& p) { return std::make_unique<Xxh>(Xxh::Variant::X3, p.seed); }
std::unique_ptr<Hasher> make_murmur3_32(const HashParams& p) {
  return std::make_unique<Murmur3>(false, static_cast<std::uint32_t>(p.seed));
}
std::unique_ptr<Hasher> make_murmur3_128(const HashParams& p) {
  return std::make_unique<Murmur3>(true, static_cast<std::uint32_t>(p.seed));
}
std::unique_ptr<Hasher> make_fnv1_32(const HashParams&) {
  return std::make_unique<Fnv<std::uint32_t>>(0x811c9dc5u, 0x01000193u, false);
}
std::unique_ptr<Hasher> make_fnv1a_32(const HashParams&) {
  return std::make_unique<Fnv<std::uint32_t>>(0x811c9dc5u, 0x01000193u, true);
}
std::unique_ptr<Hasher> make_fnv1_64(const HashParams&) {
  return std::make_unique<Fnv<std::uint64_t>>(0xcbf29ce484222325ULL, 0x100000001b3ULL, false);
}
std::unique_ptr<Hasher> make_fnv1a_64(const HashParams&) {
  return std::make_unique<Fnv<std::uint64_t>>(0xcbf29ce484222325ULL, 0x100000001b3ULL, true);
}
std::unique_ptr<Hasher> make_siphash24(const HashParams& p) { return std::make_unique<SipHash>(p.key); }
std::unique_ptr<Hasher> make_adler32(const HashParams&) { return std::make_unique<Adler32>(); }
std::unique_ptr<Hasher> make_fletcher16(const HashParams&) { return std::make_unique<Fletcher>(1); }
std::unique_ptr<Hasher> make_fletcher32(const HashParams&) { return std::make_unique<Fletcher>(2); }
std::unique_ptr<Hasher> make_fletcher64(const HashParams&) { return std::make_unique<Fletcher>(4); }

std::span<const CrcModel> crc_models() noexcept { return crc_catalogue; }
std::unique_ptr<Hasher> make_crc(const CrcModel& model) { return std::make_unique<Crc>(model); }

std::uint64_t crc_reference(const CrcModel& model, ByteView data) noexcept {
  const std::uint64_t mask = width_mask(model.width);
  const std::uint64_t top = std::uint64_t{1} << (model.width - 1);
  std::uint64_t crc = model.init;
  for (const std::byte b : data) {
    auto v = std::to_integer<std::uint64_t>(b);
    if (model.refin) v = reflect(v, 8);
    for (int bit = 7; bit >= 0; --bit) {
      const bool feedback = ((crc & top) != 0) != (((v >> bit) & 1u) != 0);
      crc = (crc << 1) & mask;
      if (feedback) crc ^= model.poly;
    }
  }
  if (model.refout) crc = reflect(crc, model.width);
  return (crc ^ model.xorout) & mask;
}

}  // namespace cvt::encoding
