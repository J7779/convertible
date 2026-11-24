// Convertible core - archive family: AES, HMAC and PBKDF2 (see crypto.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "families/archive/crypto.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace cvt::archive {

namespace {

// ---- GF(2^8) arithmetic and the AES tables, all computed at compile time ----

constexpr std::uint8_t xtime(std::uint8_t x) noexcept {
  return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0x00u));
}
constexpr std::uint8_t gmul(std::uint8_t a, std::uint8_t b) noexcept {
  std::uint8_t r = 0;
  while (b != 0) {
    if (b & 1u) r ^= a;
    a = xtime(a);
    b >>= 1;
  }
  return r;
}

struct Tables {
  std::array<std::uint8_t, 256> sbox{};
  std::array<std::uint8_t, 256> inv_sbox{};
  std::array<std::uint32_t, 256> te{};  // Te0
  std::array<std::uint32_t, 256> td{};  // Td0
};

constexpr Tables make_tables() noexcept {
  Tables t{};
  // exp/log tables over the generator 3 give the multiplicative inverses.
  std::array<std::uint8_t, 256> exp{};
  std::array<std::uint8_t, 256> log{};
  std::uint8_t g = 1;
  for (unsigned i = 0; i < 255; ++i) {
    exp[i] = g;
    log[g] = static_cast<std::uint8_t>(i);
    g = static_cast<std::uint8_t>(g ^ xtime(g));
  }
  for (unsigned i = 0; i < 256; ++i) {
    const std::uint8_t inv = i == 0 ? 0 : exp[(255u - log[i]) % 255u];
    std::uint8_t s = inv;
    std::uint8_t x = inv;
    for (int k = 0; k < 4; ++k) {
      x = static_cast<std::uint8_t>((x << 1) | (x >> 7));
      s ^= x;
    }
    s ^= 0x63;
    t.sbox[i] = s;
    t.inv_sbox[s] = static_cast<std::uint8_t>(i);
  }
  for (unsigned i = 0; i < 256; ++i) {
    const std::uint8_t s = t.sbox[i];
    t.te[i] = (static_cast<std::uint32_t>(gmul(s, 2)) << 24) | (static_cast<std::uint32_t>(s) << 16) |
              (static_cast<std::uint32_t>(s) << 8) | static_cast<std::uint32_t>(gmul(s, 3));
    const std::uint8_t v = t.inv_sbox[i];
    t.td[i] = (static_cast<std::uint32_t>(gmul(v, 14)) << 24) | (static_cast<std::uint32_t>(gmul(v, 9)) << 16) |
              (static_cast<std::uint32_t>(gmul(v, 13)) << 8) | static_cast<std::uint32_t>(gmul(v, 11));
  }
  return t;
}

constexpr Tables tables = make_tables();

inline std::uint32_t te(int n, std::uint32_t byte) noexcept { return std::rotr(tables.te[byte & 0xFFu], 8 * n); }
inline std::uint32_t td(int n, std::uint32_t byte) noexcept { return std::rotr(tables.td[byte & 0xFFu], 8 * n); }

inline std::uint32_t load_be(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
inline void store_be(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}
inline std::uint32_t sub_word(std::uint32_t w) noexcept {
  return (static_cast<std::uint32_t>(tables.sbox[w >> 24]) << 24) |
         (static_cast<std::uint32_t>(tables.sbox[(w >> 16) & 0xFFu]) << 16) |
         (static_cast<std::uint32_t>(tables.sbox[(w >> 8) & 0xFFu]) << 8) |
         static_cast<std::uint32_t>(tables.sbox[w & 0xFFu]);
}

}  // namespace

bool Aes::set_key(std::span<const std::uint8_t> key) noexcept {
  const std::size_t nk = key.size() / 4;
  if (key.size() != 16 && key.size() != 24 && key.size() != 32) return false;
  rounds_ = static_cast<int>(nk) + 6;
  const std::size_t total = 4 * (static_cast<std::size_t>(rounds_) + 1);
  for (std::size_t i = 0; i < nk; ++i) enc_[i] = load_be(key.data() + 4 * i);
  std::uint32_t rcon = 1;
  for (std::size_t i = nk; i < total; ++i) {
    std::uint32_t temp = enc_[i - 1];
    if (i % nk == 0) {
      temp = sub_word(std::rotl(temp, 8)) ^ (rcon << 24);
      rcon = xtime(static_cast<std::uint8_t>(rcon));
    } else if (nk > 6 && i % nk == 4) {
      temp = sub_word(temp);
    }
    enc_[i] = enc_[i - nk] ^ temp;
  }
  // Equivalent inverse cipher: reversed round keys, InvMixColumns on the middle ones.
  const auto r = static_cast<std::size_t>(rounds_);
  for (std::size_t round = 0; round <= r; ++round) {
    for (std::size_t c = 0; c < 4; ++c) {
      std::uint32_t w = enc_[4 * (r - round) + c];
      if (round != 0 && round != r) {
        w = td(0, tables.sbox[w >> 24]) ^ td(1, tables.sbox[(w >> 16) & 0xFFu]) ^
            td(2, tables.sbox[(w >> 8) & 0xFFu]) ^ td(3, tables.sbox[w & 0xFFu]);
      }
      dec_[4 * round + c] = w;
    }
  }
  return true;
}

void Aes::encrypt_block(const std::uint8_t* in, std::uint8_t* out) const noexcept {
  const std::uint32_t* rk = enc_.data();
  std::uint32_t s0 = load_be(in) ^ rk[0], s1 = load_be(in + 4) ^ rk[1], s2 = load_be(in + 8) ^ rk[2],
                s3 = load_be(in + 12) ^ rk[3];
  for (int round = 1; round < rounds_; ++round) {
    rk += 4;
    const std::uint32_t t0 = te(0, s0 >> 24) ^ te(1, s1 >> 16) ^ te(2, s2 >> 8) ^ te(3, s3) ^ rk[0];
    const std::uint32_t t1 = te(0, s1 >> 24) ^ te(1, s2 >> 16) ^ te(2, s3 >> 8) ^ te(3, s0) ^ rk[1];
    const std::uint32_t t2 = te(0, s2 >> 24) ^ te(1, s3 >> 16) ^ te(2, s0 >> 8) ^ te(3, s1) ^ rk[2];
    const std::uint32_t t3 = te(0, s3 >> 24) ^ te(1, s0 >> 16) ^ te(2, s1 >> 8) ^ te(3, s2) ^ rk[3];
    s0 = t0;
    s1 = t1;
    s2 = t2;
    s3 = t3;
  }
  rk += 4;
  const auto last = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t k) {
    return ((static_cast<std::uint32_t>(tables.sbox[a >> 24]) << 24) |
            (static_cast<std::uint32_t>(tables.sbox[(b >> 16) & 0xFFu]) << 16) |
            (static_cast<std::uint32_t>(tables.sbox[(c >> 8) & 0xFFu]) << 8) |
            static_cast<std::uint32_t>(tables.sbox[d & 0xFFu])) ^
           k;
  };
  store_be(out, last(s0, s1, s2, s3, rk[0]));
  store_be(out + 4, last(s1, s2, s3, s0, rk[1]));
  store_be(out + 8, last(s2, s3, s0, s1, rk[2]));
  store_be(out + 12, last(s3, s0, s1, s2, rk[3]));
}

void Aes::decrypt_block(const std::uint8_t* in, std::uint8_t* out) const noexcept {
  const std::uint32_t* rk = dec_.data();
  std::uint32_t s0 = load_be(in) ^ rk[0], s1 = load_be(in + 4) ^ rk[1], s2 = load_be(in + 8) ^ rk[2],
                s3 = load_be(in + 12) ^ rk[3];
  for (int round = 1; round < rounds_; ++round) {
    rk += 4;
    const std::uint32_t t0 = td(0, s0 >> 24) ^ td(1, s3 >> 16) ^ td(2, s2 >> 8) ^ td(3, s1) ^ rk[0];
    const std::uint32_t t1 = td(0, s1 >> 24) ^ td(1, s0 >> 16) ^ td(2, s3 >> 8) ^ td(3, s2) ^ rk[1];
    const std::uint32_t t2 = td(0, s2 >> 24) ^ td(1, s1 >> 16) ^ td(2, s0 >> 8) ^ td(3, s3) ^ rk[2];
    const std::uint32_t t3 = td(0, s3 >> 24) ^ td(1, s2 >> 16) ^ td(2, s1 >> 8) ^ td(3, s0) ^ rk[3];
    s0 = t0;
    s1 = t1;
    s2 = t2;
    s3 = t3;
  }
  rk += 4;
  const auto last = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t k) {
    return ((static_cast<std::uint32_t>(tables.inv_sbox[a >> 24]) << 24) |
            (static_cast<std::uint32_t>(tables.inv_sbox[(b >> 16) & 0xFFu]) << 16) |
            (static_cast<std::uint32_t>(tables.inv_sbox[(c >> 8) & 0xFFu]) << 8) |
            static_cast<std::uint32_t>(tables.inv_sbox[d & 0xFFu])) ^
           k;
  };
  store_be(out, last(s0, s3, s2, s1, rk[0]));
  store_be(out + 4, last(s1, s0, s3, s2, rk[1]));
  store_be(out + 8, last(s2, s1, s0, s3, rk[2]));
  store_be(out + 12, last(s3, s2, s1, s0, rk[3]));
}

Result<void> aes_cbc_decrypt(const Aes& aes, Aes::Block& iv, std::span<std::uint8_t> data) {
  if (data.size() % 16 != 0) return fail(Status::InvalidArgument, "AES-CBC data is not a whole number of blocks");
  Aes::Block plain{};
  for (std::size_t i = 0; i < data.size(); i += 16) {
    Aes::Block cipher{};
    ::cvt::util::copy_bytes(cipher.data(), data.data() + i, 16);
    aes.decrypt_block(cipher.data(), plain.data());
    for (std::size_t k = 0; k < 16; ++k) data[i + k] = static_cast<std::uint8_t>(plain[k] ^ iv[k]);
    iv = cipher;
  }
  return {};
}

Result<void> aes_cbc_encrypt(const Aes& aes, Aes::Block& iv, std::span<std::uint8_t> data) {
  if (data.size() % 16 != 0) return fail(Status::InvalidArgument, "AES-CBC data is not a whole number of blocks");
  for (std::size_t i = 0; i < data.size(); i += 16) {
    Aes::Block block{};
    for (std::size_t k = 0; k < 16; ++k) block[k] = static_cast<std::uint8_t>(data[i + k] ^ iv[k]);
    aes.encrypt_block(block.data(), iv.data());
    ::cvt::util::copy_bytes(data.data() + i, iv.data(), 16);
  }
  return {};
}

// --------------------------------------------------------------------------- HMAC

namespace {

template <class Hash, std::size_t BlockSize>
void hmac_init(Hash& inner, Hash& outer, ByteView key) noexcept {
  std::array<std::uint8_t, BlockSize> k{};
  if (key.size() > BlockSize) {
    Hash h;
    h.update(key);
    const auto digest = h.finish();
    ::cvt::util::copy_bytes(k.data(), digest.data(), digest.size());
  } else if (!key.empty()) {
    ::cvt::util::copy_bytes(k.data(), key.data(), key.size());
  }
  std::array<std::uint8_t, BlockSize> pad{};
  for (std::size_t i = 0; i < BlockSize; ++i) pad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36u);
  inner.update(as_bytes(std::span<const std::uint8_t>(pad)));
  for (std::size_t i = 0; i < BlockSize; ++i) pad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5Cu);
  outer.update(as_bytes(std::span<const std::uint8_t>(pad)));
  secure_zero(std::span<std::uint8_t>(k));
  secure_zero(std::span<std::uint8_t>(pad));
}

template <class Hmac>
Bytes pbkdf2(ByteView password, ByteView salt, std::uint32_t iterations, std::size_t length) {
  Bytes out;
  out.reserve(length);
  if (iterations == 0) iterations = 1;
  const Hmac keyed(password);  // the keyed state is copied instead of re-hashing the pads every round
  for (std::uint32_t block = 1; out.size() < length; ++block) {
    const std::array<std::uint8_t, 4> index = {static_cast<std::uint8_t>(block >> 24),
                                               static_cast<std::uint8_t>(block >> 16),
                                               static_cast<std::uint8_t>(block >> 8), static_cast<std::uint8_t>(block)};
    Hmac first = keyed;
    first.update(salt);
    first.update(as_bytes(std::span<const std::uint8_t>(index)));
    auto u = first.finish();
    auto t = u;
    for (std::uint32_t i = 1; i < iterations; ++i) {
      Hmac next = keyed;
      next.update(as_bytes(std::span<const std::uint8_t>(u)));
      u = next.finish();
      for (std::size_t k = 0; k < t.size(); ++k) t[k] ^= u[k];
    }
    const std::size_t take = std::min(t.size(), length - out.size());
    for (std::size_t k = 0; k < take; ++k) out.push_back(static_cast<std::byte>(t[k]));
  }
  return out;
}

}  // namespace

HmacSha1::HmacSha1(ByteView key) noexcept { hmac_init<util::Sha1, 64>(inner_, outer_, key); }
void HmacSha1::update(ByteView data) noexcept { inner_.update(data); }
util::Sha1::Digest HmacSha1::finish() noexcept {
  const auto inner = inner_.finish();
  outer_.update(as_bytes(std::span<const std::uint8_t>(inner)));
  return outer_.finish();
}

HmacSha256::HmacSha256(ByteView key) noexcept { hmac_init<util::Sha256, 64>(inner_, outer_, key); }
void HmacSha256::update(ByteView data) noexcept { inner_.update(data); }
util::Sha256::Digest HmacSha256::finish() noexcept {
  const auto inner = inner_.finish();
  outer_.update(as_bytes(std::span<const std::uint8_t>(inner)));
  return outer_.finish();
}

util::Sha1::Digest hmac_sha1(ByteView key, ByteView data) noexcept {
  HmacSha1 h(key);
  h.update(data);
  return h.finish();
}

util::Sha256::Digest hmac_sha256(ByteView key, ByteView data) noexcept {
  HmacSha256 h(key);
  h.update(data);
  return h.finish();
}

Bytes pbkdf2_hmac_sha1(ByteView password, ByteView salt, std::uint32_t iterations, std::size_t length) {
  return pbkdf2<HmacSha1>(password, salt, iterations, length);
}

Bytes pbkdf2_hmac_sha256(ByteView password, ByteView salt, std::uint32_t iterations, std::size_t length) {
  return pbkdf2<HmacSha256>(password, salt, iterations, length);
}

void secure_zero(std::span<std::byte> data) noexcept {
  volatile std::byte* p = data.data();
  for (std::size_t i = 0; i < data.size(); ++i) p[i] = std::byte{0};
}

void secure_zero(std::span<std::uint8_t> data) noexcept {
  volatile std::uint8_t* p = data.data();
  for (std::size_t i = 0; i < data.size(); ++i) p[i] = 0;
}

}  // namespace cvt::archive
