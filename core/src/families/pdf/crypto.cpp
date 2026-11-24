// Convertible core - pdf family: RC4, AES and SHA-2 (384/512) for the
// standard security handler.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "families/pdf/crypto.hpp"

#include <cstring>

#include "util/hash.hpp"

namespace cvt::pdf::crypto {

namespace {

constexpr std::array<std::uint8_t, 256> make_sbox() {
  std::array<std::uint8_t, 256> s{};
  std::uint8_t p = 1, q = 1;
  do {
    p = static_cast<std::uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
    q = static_cast<std::uint8_t>(q ^ (q << 1));
    q = static_cast<std::uint8_t>(q ^ (q << 2));
    q = static_cast<std::uint8_t>(q ^ (q << 4));
    if (q & 0x80) q ^= 0x09;
    const auto rotl = [](std::uint8_t x, int n) { return static_cast<std::uint8_t>((x << n) | (x >> (8 - n))); };
    s[p] = static_cast<std::uint8_t>(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4) ^ 0x63);
  } while (p != 1);
  s[0] = 0x63;
  return s;
}

constexpr std::array<std::uint8_t, 256> kSbox = make_sbox();

constexpr std::array<std::uint8_t, 256> make_inv_sbox() {
  std::array<std::uint8_t, 256> inv{};
  for (int i = 0; i < 256; ++i) inv[kSbox[static_cast<std::size_t>(i)]] = static_cast<std::uint8_t>(i);
  return inv;
}
constexpr std::array<std::uint8_t, 256> kInvSbox = make_inv_sbox();

constexpr std::uint8_t xtime(std::uint8_t x) { return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
constexpr std::uint8_t gmul(std::uint8_t a, std::uint8_t b) {
  std::uint8_t r = 0;
  while (b) {
    if (b & 1) r ^= a;
    a = xtime(a);
    b >>= 1;
  }
  return r;
}

struct MulTables {
  std::array<std::uint8_t, 256> m9{}, m11{}, m13{}, m14{};
};
constexpr MulTables make_mul_tables() {
  MulTables t;
  for (int i = 0; i < 256; ++i) {
    const auto v = static_cast<std::uint8_t>(i);
    t.m9[static_cast<std::size_t>(i)] = gmul(v, 9);
    t.m11[static_cast<std::size_t>(i)] = gmul(v, 11);
    t.m13[static_cast<std::size_t>(i)] = gmul(v, 13);
    t.m14[static_cast<std::size_t>(i)] = gmul(v, 14);
  }
  return t;
}
constexpr MulTables kMul = make_mul_tables();

std::uint32_t sub_word(std::uint32_t w) {
  return (std::uint32_t{kSbox[(w >> 24) & 0xFF]} << 24) | (std::uint32_t{kSbox[(w >> 16) & 0xFF]} << 16) |
         (std::uint32_t{kSbox[(w >> 8) & 0xFF]} << 8) | kSbox[w & 0xFF];
}

}  // namespace

ByteVec rc4(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
  ByteVec out(data.begin(), data.end());
  if (key.empty()) return out;
  std::array<std::uint8_t, 256> s{};
  for (int i = 0; i < 256; ++i) s[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
  std::uint8_t j = 0;
  for (std::size_t i = 0; i < 256; ++i) {
    j = static_cast<std::uint8_t>(j + s[i] + key[i % key.size()]);
    std::swap(s[i], s[j]);
  }
  std::uint8_t a = 0, b = 0;
  for (std::uint8_t& byte : out) {
    a = static_cast<std::uint8_t>(a + 1);
    b = static_cast<std::uint8_t>(b + s[a]);
    std::swap(s[a], s[b]);
    byte ^= s[static_cast<std::uint8_t>(s[a] + s[b])];
  }
  return out;
}

Aes::Aes(std::span<const std::uint8_t> key) {
  const std::size_t nk = key.size() / 4;
  if (key.size() != 16 && key.size() != 24 && key.size() != 32) return;
  rounds_ = static_cast<int>(nk) + 6;
  const std::size_t total = 4 * static_cast<std::size_t>(rounds_ + 1);
  for (std::size_t i = 0; i < nk; ++i) {
    enc_[i] = (std::uint32_t{key[4 * i]} << 24) | (std::uint32_t{key[4 * i + 1]} << 16) |
              (std::uint32_t{key[4 * i + 2]} << 8) | key[4 * i + 3];
  }
  std::uint8_t rcon = 1;
  for (std::size_t i = nk; i < total; ++i) {
    std::uint32_t t = enc_[i - 1];
    if (i % nk == 0) {
      t = sub_word((t << 8) | (t >> 24)) ^ (std::uint32_t{rcon} << 24);
      rcon = xtime(rcon);
    } else if (nk > 6 && i % nk == 4) {
      t = sub_word(t);
    }
    enc_[i] = enc_[i - nk] ^ t;
  }
  dec_ = enc_;
}

void Aes::encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const noexcept {
  std::uint8_t s[16];
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) s[c * 4 + r] = static_cast<std::uint8_t>(in[c * 4 + r] ^ (enc_[static_cast<std::size_t>(c)] >> (24 - 8 * r)));
  }
  for (int round = 1; round <= rounds_; ++round) {
    for (std::uint8_t& v : s) v = kSbox[v];
    std::uint8_t t[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
    }
    if (round != rounds_) {
      for (int c = 0; c < 4; ++c) {
        const std::uint8_t a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
        s[c * 4] = static_cast<std::uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        s[c * 4 + 1] = static_cast<std::uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        s[c * 4 + 2] = static_cast<std::uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        s[c * 4 + 3] = static_cast<std::uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
      }
    } else {
      ::cvt::util::copy_bytes(s, t, 16);
    }
    for (int c = 0; c < 4; ++c) {
      const std::uint32_t w = enc_[static_cast<std::size_t>(round * 4 + c)];
      for (int r = 0; r < 4; ++r) s[c * 4 + r] = static_cast<std::uint8_t>(s[c * 4 + r] ^ (w >> (24 - 8 * r)));
    }
  }
  ::cvt::util::copy_bytes(out, s, 16);
}

void Aes::decrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const noexcept {
  std::uint8_t s[16];
  for (int c = 0; c < 4; ++c) {
    const std::uint32_t w = dec_[static_cast<std::size_t>(rounds_ * 4 + c)];
    for (int r = 0; r < 4; ++r) s[c * 4 + r] = static_cast<std::uint8_t>(in[c * 4 + r] ^ (w >> (24 - 8 * r)));
  }
  for (int round = rounds_ - 1; round >= 0; --round) {
    std::uint8_t t[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) t[((c + r) % 4) * 4 + r] = s[c * 4 + r];
    }
    for (std::uint8_t& v : t) v = kInvSbox[v];
    for (int c = 0; c < 4; ++c) {
      const std::uint32_t w = dec_[static_cast<std::size_t>(round * 4 + c)];
      for (int r = 0; r < 4; ++r) t[c * 4 + r] = static_cast<std::uint8_t>(t[c * 4 + r] ^ (w >> (24 - 8 * r)));
    }
    if (round != 0) {
      for (int c = 0; c < 4; ++c) {
        const std::uint8_t a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
        s[c * 4] = static_cast<std::uint8_t>(kMul.m14[a0] ^ kMul.m11[a1] ^ kMul.m13[a2] ^ kMul.m9[a3]);
        s[c * 4 + 1] = static_cast<std::uint8_t>(kMul.m9[a0] ^ kMul.m14[a1] ^ kMul.m11[a2] ^ kMul.m13[a3]);
        s[c * 4 + 2] = static_cast<std::uint8_t>(kMul.m13[a0] ^ kMul.m9[a1] ^ kMul.m14[a2] ^ kMul.m11[a3]);
        s[c * 4 + 3] = static_cast<std::uint8_t>(kMul.m11[a0] ^ kMul.m13[a1] ^ kMul.m9[a2] ^ kMul.m14[a3]);
      }
    } else {
      ::cvt::util::copy_bytes(s, t, 16);
    }
  }
  ::cvt::util::copy_bytes(out, s, 16);
}

ByteVec aes_cbc_decrypt_pdf(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
  const Aes aes(key);
  ByteVec out;
  if (!aes.ok() || data.size() < 32) return out;
  std::uint8_t iv[16];
  ::cvt::util::copy_bytes(iv, data.data(), 16);
  const std::size_t blocks = (data.size() - 16) / 16;
  out.resize(blocks * 16);
  for (std::size_t b = 0; b < blocks; ++b) {
    const std::uint8_t* in = data.data() + 16 + b * 16;
    std::uint8_t plain[16];
    aes.decrypt_block(in, plain);
    for (int i = 0; i < 16; ++i) out[b * 16 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(plain[i] ^ iv[i]);
    ::cvt::util::copy_bytes(iv, in, 16);
  }
  if (!out.empty()) {
    const std::uint8_t pad = out.back();
    if (pad >= 1 && pad <= 16 && pad <= out.size()) {
      bool valid = true;
      for (std::size_t i = out.size() - pad; i < out.size(); ++i) valid = valid && out[i] == pad;
      if (valid) out.resize(out.size() - pad);
    }
  }
  return out;
}

ByteVec aes_cbc_encrypt_nopad(std::span<const std::uint8_t> key, std::span<const std::uint8_t> iv_in,
                              std::span<const std::uint8_t> data) {
  const Aes aes(key);
  ByteVec out;
  if (!aes.ok() || iv_in.size() < 16) return out;
  std::uint8_t iv[16];
  ::cvt::util::copy_bytes(iv, iv_in.data(), 16);
  const std::size_t blocks = data.size() / 16;
  out.resize(blocks * 16);
  for (std::size_t b = 0; b < blocks; ++b) {
    std::uint8_t block[16];
    for (int i = 0; i < 16; ++i) block[i] = static_cast<std::uint8_t>(data[b * 16 + static_cast<std::size_t>(i)] ^ iv[i]);
    aes.encrypt_block(block, out.data() + b * 16);
    ::cvt::util::copy_bytes(iv, out.data() + b * 16, 16);
  }
  return out;
}

ByteVec aes_ecb_decrypt(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
  const Aes aes(key);
  ByteVec out;
  if (!aes.ok()) return out;
  out.resize(data.size() / 16 * 16);
  for (std::size_t b = 0; b < data.size() / 16; ++b) aes.decrypt_block(data.data() + b * 16, out.data() + b * 16);
  return out;
}

ByteVec aes_cbc_decrypt_zero_iv(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
  const Aes aes(key);
  ByteVec out;
  if (!aes.ok()) return out;
  std::uint8_t iv[16] = {};
  out.resize(data.size() / 16 * 16);
  for (std::size_t b = 0; b < data.size() / 16; ++b) {
    std::uint8_t plain[16];
    aes.decrypt_block(data.data() + b * 16, plain);
    for (int i = 0; i < 16; ++i) out[b * 16 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(plain[i] ^ iv[i]);
    ::cvt::util::copy_bytes(iv, data.data() + b * 16, 16);
  }
  return out;
}

namespace {

constexpr std::uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

constexpr std::uint64_t rotr64(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

std::array<std::uint64_t, 8> sha512_core(std::span<const std::uint8_t> data, std::array<std::uint64_t, 8> h) {
  const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
  std::vector<std::uint8_t> msg(data.begin(), data.end());
  msg.push_back(0x80);
  while (msg.size() % 128 != 112) msg.push_back(0);
  for (int i = 0; i < 8; ++i) msg.push_back(0);
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>(bit_length >> (8 * i)));
  std::uint64_t w[80];
  for (std::size_t block = 0; block < msg.size(); block += 128) {
    for (int t = 0; t < 16; ++t) {
      std::uint64_t v = 0;
      for (int k = 0; k < 8; ++k) v = (v << 8) | msg[block + static_cast<std::size_t>(t * 8 + k)];
      w[t] = v;
    }
    for (int t = 16; t < 80; ++t) {
      const std::uint64_t s0 = rotr64(w[t - 15], 1) ^ rotr64(w[t - 15], 8) ^ (w[t - 15] >> 7);
      const std::uint64_t s1 = rotr64(w[t - 2], 19) ^ rotr64(w[t - 2], 61) ^ (w[t - 2] >> 6);
      w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    std::uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 80; ++t) {
      const std::uint64_t s1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
      const std::uint64_t ch = (e & f) ^ (~e & g);
      const std::uint64_t t1 = hh + s1 + ch + kSha512K[t] + w[t];
      const std::uint64_t s0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
      const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint64_t t2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  return h;
}

}  // namespace

std::array<std::uint8_t, 64> sha512(std::span<const std::uint8_t> data) {
  const auto h = sha512_core(data, {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                                    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL});
  std::array<std::uint8_t, 64> out{};
  for (std::size_t i = 0; i < 64; ++i) out[i] = static_cast<std::uint8_t>(h[i / 8] >> (56 - 8 * (i % 8)));
  return out;
}

std::array<std::uint8_t, 48> sha384(std::span<const std::uint8_t> data) {
  const auto h = sha512_core(data, {0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
                                    0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
                                    0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL});
  std::array<std::uint8_t, 48> out{};
  for (std::size_t i = 0; i < 48; ++i) out[i] = static_cast<std::uint8_t>(h[i / 8] >> (56 - 8 * (i % 8)));
  return out;
}

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data) {
  return util::sha256(as_bytes(data));
}

std::array<std::uint8_t, 16> md5(std::span<const std::uint8_t> data) { return util::md5(as_bytes(data)); }

}  // namespace cvt::pdf::crypto
