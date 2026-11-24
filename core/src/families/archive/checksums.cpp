// Convertible core - archive family: checksums (see checksums.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "families/archive/checksums.hpp"

#include <bit>
#include <cstring>

namespace cvt::archive {

namespace {

template <class T, T Poly>
constexpr std::array<T, 256> reflected_table() noexcept {
  std::array<T, 256> table{};
  for (unsigned i = 0; i < 256; ++i) {
    T c = static_cast<T>(i);
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? static_cast<T>(Poly ^ (c >> 1)) : static_cast<T>(c >> 1);
    table[i] = c;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> msb_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i << 24;
    for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
    table[i] = c;
  }
  return table;
}

constexpr auto crc32c_table = reflected_table<std::uint32_t, 0x82F63B78u>();
constexpr auto crc64_table = reflected_table<std::uint64_t, 0xC96C5795D7870F42ull>();
constexpr auto crc16_table = reflected_table<std::uint16_t, static_cast<std::uint16_t>(0xA001u)>();
constexpr auto crc32_msb_table = msb_table();

inline std::uint32_t load32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t load64(const std::uint8_t* p) noexcept {
  return static_cast<std::uint64_t>(load32(p)) | (static_cast<std::uint64_t>(load32(p + 4)) << 32);
}

constexpr std::uint32_t p32_1 = 0x9E3779B1u, p32_2 = 0x85EBCA77u, p32_3 = 0xC2B2AE3Du, p32_4 = 0x27D4EB2Fu,
                        p32_5 = 0x165667B1u;
constexpr std::uint64_t p64_1 = 0x9E3779B185EBCA87ull, p64_2 = 0xC2B2AE3D27D4EB4Full, p64_3 = 0x165667B19E3779F9ull,
                        p64_4 = 0x85EBCA77C2B2AE63ull, p64_5 = 0x27D4EB2F165667C5ull;

inline std::uint32_t round32(std::uint32_t acc, std::uint32_t input) noexcept {
  acc += input * p32_2;
  acc = std::rotl(acc, 13);
  return acc * p32_1;
}
inline std::uint64_t round64(std::uint64_t acc, std::uint64_t input) noexcept {
  acc += input * p64_2;
  acc = std::rotl(acc, 31);
  return acc * p64_1;
}
inline std::uint64_t merge64(std::uint64_t acc, std::uint64_t val) noexcept {
  val = round64(0, val);
  acc ^= val;
  return acc * p64_1 + p64_4;
}

}  // namespace

std::uint32_t crc32c(ByteView data, std::uint32_t crc) noexcept {
  std::uint32_t c = ~crc;
  for (const std::byte b : data) c = crc32c_table[(c ^ std::to_integer<std::uint32_t>(b)) & 0xFFu] ^ (c >> 8);
  return ~c;
}

std::uint64_t crc64_xz(ByteView data, std::uint64_t crc) noexcept {
  std::uint64_t c = ~crc;
  for (const std::byte b : data) c = crc64_table[(c ^ std::to_integer<std::uint64_t>(b)) & 0xFFu] ^ (c >> 8);
  return ~c;
}

std::uint32_t crc32_msb(ByteView data, std::uint32_t crc) noexcept {
  std::uint32_t c = ~crc;
  for (const std::byte b : data) c = (c << 8) ^ crc32_msb_table[((c >> 24) ^ std::to_integer<std::uint32_t>(b)) & 0xFFu];
  return ~c;
}

std::uint16_t crc16_arc(ByteView data, std::uint16_t crc) noexcept {
  std::uint16_t c = crc;
  for (const std::byte b : data) {
    c = static_cast<std::uint16_t>(crc16_table[(c ^ std::to_integer<std::uint16_t>(b)) & 0xFFu] ^ (c >> 8));
  }
  return c;
}

// --------------------------------------------------------------------------- xxHash32

Xxh32::Xxh32(std::uint32_t seed) noexcept : seed_(seed) {
  v_ = {seed + p32_1 + p32_2, seed + p32_2, seed, seed - p32_1};
}

void Xxh32::update(ByteView data) noexcept {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t n = data.size();
  total_ += n;
  if (buffered_ + n < 16) {
    if (n != 0) ::cvt::util::copy_bytes(buffer_.data() + buffered_, p, n);
    buffered_ += n;
    return;
  }
  if (buffered_ != 0) {
    const std::size_t take = 16 - buffered_;
    ::cvt::util::copy_bytes(buffer_.data() + buffered_, p, take);
    for (int i = 0; i < 4; ++i) v_[i] = round32(v_[i], load32(buffer_.data() + 4 * i));
    p += take;
    n -= take;
    buffered_ = 0;
  }
  while (n >= 16) {
    for (int i = 0; i < 4; ++i) v_[i] = round32(v_[i], load32(p + 4 * i));
    p += 16;
    n -= 16;
  }
  if (n != 0) ::cvt::util::copy_bytes(buffer_.data(), p, n);
  buffered_ = n;
}

std::uint32_t Xxh32::digest() const noexcept {
  std::uint32_t h = 0;
  if (total_ >= 16) {
    h = std::rotl(v_[0], 1) + std::rotl(v_[1], 7) + std::rotl(v_[2], 12) + std::rotl(v_[3], 18);
  } else {
    h = seed_ + p32_5;
  }
  h += static_cast<std::uint32_t>(total_);
  const std::uint8_t* p = buffer_.data();
  std::size_t n = buffered_;
  while (n >= 4) {
    h += load32(p) * p32_3;
    h = std::rotl(h, 17) * p32_4;
    p += 4;
    n -= 4;
  }
  while (n > 0) {
    h += static_cast<std::uint32_t>(*p) * p32_5;
    h = std::rotl(h, 11) * p32_1;
    ++p;
    --n;
  }
  h ^= h >> 15;
  h *= p32_2;
  h ^= h >> 13;
  h *= p32_3;
  h ^= h >> 16;
  return h;
}

// --------------------------------------------------------------------------- xxHash64

Xxh64::Xxh64(std::uint64_t seed) noexcept : seed_(seed) {
  v_ = {seed + p64_1 + p64_2, seed + p64_2, seed, seed - p64_1};
}

void Xxh64::update(ByteView data) noexcept {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t n = data.size();
  total_ += n;
  if (buffered_ + n < 32) {
    if (n != 0) ::cvt::util::copy_bytes(buffer_.data() + buffered_, p, n);
    buffered_ += n;
    return;
  }
  if (buffered_ != 0) {
    const std::size_t take = 32 - buffered_;
    ::cvt::util::copy_bytes(buffer_.data() + buffered_, p, take);
    for (int i = 0; i < 4; ++i) v_[i] = round64(v_[i], load64(buffer_.data() + 8 * i));
    p += take;
    n -= take;
    buffered_ = 0;
  }
  while (n >= 32) {
    for (int i = 0; i < 4; ++i) v_[i] = round64(v_[i], load64(p + 8 * i));
    p += 32;
    n -= 32;
  }
  if (n != 0) ::cvt::util::copy_bytes(buffer_.data(), p, n);
  buffered_ = n;
}

std::uint64_t Xxh64::digest() const noexcept {
  std::uint64_t h = 0;
  if (total_ >= 32) {
    h = std::rotl(v_[0], 1) + std::rotl(v_[1], 7) + std::rotl(v_[2], 12) + std::rotl(v_[3], 18);
    for (int i = 0; i < 4; ++i) h = merge64(h, v_[i]);
  } else {
    h = seed_ + p64_5;
  }
  h += total_;
  const std::uint8_t* p = buffer_.data();
  std::size_t n = buffered_;
  while (n >= 8) {
    h ^= round64(0, load64(p));
    h = std::rotl(h, 27) * p64_1 + p64_4;
    p += 8;
    n -= 8;
  }
  if (n >= 4) {
    h ^= static_cast<std::uint64_t>(load32(p)) * p64_1;
    h = std::rotl(h, 23) * p64_2 + p64_3;
    p += 4;
    n -= 4;
  }
  while (n > 0) {
    h ^= static_cast<std::uint64_t>(*p) * p64_5;
    h = std::rotl(h, 11) * p64_1;
    ++p;
    --n;
  }
  h ^= h >> 33;
  h *= p64_2;
  h ^= h >> 29;
  h *= p64_3;
  h ^= h >> 32;
  return h;
}

std::uint32_t xxh32(ByteView data, std::uint32_t seed) noexcept {
  Xxh32 state(seed);
  state.update(data);
  return state.digest();
}

std::uint64_t xxh64(ByteView data, std::uint64_t seed) noexcept {
  Xxh64 state(seed);
  state.update(data);
  return state.digest();
}

}  // namespace cvt::archive
