// Convertible core - archive family: RAR checksums and key derivation.
//
// BLAKE2s follows RFC 7693; BLAKE2sp is its 8-way tree mode (fanout 8,
// depth 2, 64-byte chunks dealt round robin to the leaves, root over the 8
// leaf digests) as RAR 5 uses it for file hashes. Key derivation follows the
// RAR 5 archive format description (PBKDF2-HMAC-SHA256) and the RAR 3 scheme
// (iterated SHA-1 over the UTF-16LE password and the salt).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>

#include "families/archive/crypto.hpp"
#include "families/archive/formats/rar_internal.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

namespace cvt::archive::rar {

namespace {

constexpr std::array<std::uint32_t, 8> blake_iv = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                                   0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};

constexpr std::uint8_t sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

constexpr std::uint32_t rotr(std::uint32_t v, int n) noexcept { return (v >> n) | (v << (32 - n)); }

std::uint32_t load32(const std::uint8_t* p) noexcept {
  return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

constexpr std::uint32_t rotl(std::uint32_t v, int n) noexcept { return (v << n) | (v >> (32 - n)); }

/// SHA-1 as RAR 2.9-4.x uses it for key derivation: whenever a whole 64-byte
/// block is hashed straight from the caller's buffer, the final 16 words of
/// the message schedule are written back over that block (little-endian),
/// and later rounds hash the modified buffer. Only passwords of 29 or more
/// characters are long enough for this to happen.
class Rar3Sha1 {
 public:
  void update(std::uint8_t* data, std::size_t size) noexcept {
    std::size_t used = static_cast<std::size_t>(count_ & 63u);
    count_ += size;
    std::size_t i = 0;
    if (used + size > 63) {
      i = 64 - used;
      std::memcpy(buffer_.data() + used, data, i);
      std::uint32_t w[16];
      transform(buffer_.data(), w);
      for (; i + 63 < size; i += 64) {
        transform(data + i, w);
        for (std::size_t k = 0; k < 16; ++k) {
          for (std::size_t b = 0; b < 4; ++b) data[i + k * 4 + b] = static_cast<std::uint8_t>(w[k] >> (8 * b));
        }
      }
      used = 0;
    }
    std::memcpy(buffer_.data() + used, data + i, size - i);
  }
  [[nodiscard]] std::array<std::uint8_t, 20> finish() noexcept {
    const std::uint64_t bits = count_ * 8;
    std::uint8_t pad[72] = {0x80};
    const std::size_t used = static_cast<std::size_t>(count_ & 63u);
    const std::size_t pad_size = (used < 56 ? 56 - used : 120 - used);
    for (std::size_t b = 0; b < 8; ++b) pad[pad_size + b] = static_cast<std::uint8_t>(bits >> (56 - 8 * b));
    plain_update(pad, pad_size + 8);
    std::array<std::uint8_t, 20> out{};
    for (std::size_t k = 0; k < 5; ++k) {
      for (std::size_t b = 0; b < 4; ++b) out[k * 4 + b] = static_cast<std::uint8_t>(state_[k] >> (24 - 8 * b));
    }
    return out;
  }

 private:
  void plain_update(const std::uint8_t* data, std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
      buffer_[static_cast<std::size_t>(count_ & 63u)] = data[i];
      ++count_;
      if ((count_ & 63u) == 0) {
        std::uint32_t w[16];
        transform(buffer_.data(), w);
      }
    }
  }
  void transform(const std::uint8_t* block, std::uint32_t (&w)[16]) noexcept {
    for (std::size_t k = 0; k < 16; ++k) {
      w[k] = (std::uint32_t{block[k * 4]} << 24) | (std::uint32_t{block[k * 4 + 1]} << 16) |
             (std::uint32_t{block[k * 4 + 2]} << 8) | std::uint32_t{block[k * 4 + 3]};
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
    for (std::size_t t = 0; t < 80; ++t) {
      std::uint32_t x;
      if (t < 16) {
        x = w[t];
      } else {
        x = rotl(w[(t + 13) & 15] ^ w[(t + 8) & 15] ^ w[(t + 2) & 15] ^ w[t & 15], 1);
        w[t & 15] = x;
      }
      std::uint32_t f;
      std::uint32_t k;
      if (t < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (t < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (t < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const std::uint32_t temp = rotl(a, 5) + f + e + k + x;
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = temp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
  }

  std::array<std::uint32_t, 5> state_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t count_ = 0;
};

}  // namespace

Blake2s::Blake2s(std::uint8_t fanout, std::uint8_t depth, std::uint32_t node_offset, std::uint8_t node_depth,
                 std::uint8_t inner_length, bool last_node) noexcept
    : last_node_(last_node) {
  h_ = blake_iv;
  h_[0] ^= 32u | (std::uint32_t{fanout} << 16) | (std::uint32_t{depth} << 24);
  h_[2] ^= node_offset;
  h_[3] ^= (std::uint32_t{node_depth} << 16) | (std::uint32_t{inner_length} << 24);
}

void Blake2s::compress(const std::uint8_t* block, bool last) noexcept {
  std::uint32_t m[16];
  for (int i = 0; i < 16; ++i) m[i] = load32(block + 4 * i);
  std::uint32_t v[16];
  for (int i = 0; i < 8; ++i) {
    v[i] = h_[static_cast<std::size_t>(i)];
    v[i + 8] = blake_iv[static_cast<std::size_t>(i)];
  }
  v[12] ^= static_cast<std::uint32_t>(counter_);
  v[13] ^= static_cast<std::uint32_t>(counter_ >> 32);
  if (last) {
    v[14] = ~v[14];
    if (last_node_) v[15] = ~v[15];
  }
  const auto g = [&](int a, int b, int c, int d, std::uint32_t x, std::uint32_t y) {
    v[a] = v[a] + v[b] + x;
    v[d] = rotr(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];
    v[b] = rotr(v[b] ^ v[c], 12);
    v[a] = v[a] + v[b] + y;
    v[d] = rotr(v[d] ^ v[a], 8);
    v[c] = v[c] + v[d];
    v[b] = rotr(v[b] ^ v[c], 7);
  };
  for (const auto& s : sigma) {
    g(0, 4, 8, 12, m[s[0]], m[s[1]]);
    g(1, 5, 9, 13, m[s[2]], m[s[3]]);
    g(2, 6, 10, 14, m[s[4]], m[s[5]]);
    g(3, 7, 11, 15, m[s[6]], m[s[7]]);
    g(0, 5, 10, 15, m[s[8]], m[s[9]]);
    g(1, 6, 11, 12, m[s[10]], m[s[11]]);
    g(2, 7, 8, 13, m[s[12]], m[s[13]]);
    g(3, 4, 9, 14, m[s[14]], m[s[15]]);
  }
  for (std::size_t i = 0; i < 8; ++i) h_[i] ^= v[i] ^ v[i + 8];
}

void Blake2s::update(const std::uint8_t* data, std::size_t size) noexcept {
  while (size > 0) {
    if (buffered_ == 64) {  // a full block is only compressed once more data follows
      counter_ += 64;
      compress(buf_.data(), false);
      buffered_ = 0;
    }
    const std::size_t take = std::min(size, 64 - buffered_);
    std::memcpy(buf_.data() + buffered_, data, take);
    buffered_ += take;
    data += take;
    size -= take;
  }
}

std::array<std::uint8_t, 32> Blake2s::finish() noexcept {
  counter_ += buffered_;
  std::fill(buf_.begin() + static_cast<std::ptrdiff_t>(buffered_), buf_.end(), std::uint8_t{0});
  compress(buf_.data(), true);
  std::array<std::uint8_t, 32> out{};
  for (std::size_t i = 0; i < 8; ++i) {
    for (std::size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h_[i] >> (8 * j));
  }
  return out;
}

Blake2sp::Blake2sp() noexcept {
  for (std::uint32_t i = 0; i < 8; ++i) leaves_[i] = Blake2s(8, 2, i, 0, 32, i == 7);
}

void Blake2sp::update(ByteView data) noexcept {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t size = data.size();
  while (size > 0) {
    if (buffered_ == buf_.size()) {
      for (std::size_t i = 0; i < 8; ++i) leaves_[i].update(buf_.data() + i * 64, 64);
      buffered_ = 0;
    }
    const std::size_t take = std::min(size, buf_.size() - buffered_);
    std::memcpy(buf_.data() + buffered_, p, take);
    buffered_ += take;
    p += take;
    size -= take;
  }
}

std::array<std::uint8_t, 32> Blake2sp::finish() noexcept {
  Blake2s root(8, 2, 0, 1, 32, true);
  for (std::size_t i = 0; i < 8; ++i) {
    if (buffered_ > i * 64) leaves_[i].update(buf_.data() + i * 64, std::min<std::size_t>(buffered_ - i * 64, 64));
    const auto leaf = leaves_[i].finish();
    root.update(leaf.data(), leaf.size());
  }
  return root.finish();
}

Rar5Keys rar5_keys(std::string_view password, ByteView salt, unsigned log2_count) {
  const HmacSha256 keyed(as_bytes(password));
  Bytes first(salt.begin(), salt.end());
  for (const std::byte b : {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}}) first.push_back(b);  // block index 1
  HmacSha256 h = keyed;
  h.update(first);
  util::Sha256::Digest u = h.finish();
  util::Sha256::Digest f = u;
  Rar5Keys keys;
  const std::uint64_t rounds = std::uint64_t{1} << log2_count;
  const std::uint64_t stops[3] = {rounds, rounds + 16, rounds + 32};
  std::array<std::uint8_t, 32>* outputs[3] = {&keys.key, &keys.hash_key, nullptr};
  std::array<std::uint8_t, 32> check_value{};
  outputs[2] = &check_value;
  std::uint64_t done = 1;
  for (int stage = 0; stage < 3; ++stage) {
    for (; done < stops[stage]; ++done) {
      HmacSha256 next = keyed;
      next.update(ByteView(reinterpret_cast<const std::byte*>(u.data()), u.size()));
      u = next.finish();
      for (std::size_t i = 0; i < f.size(); ++i) f[i] ^= u[i];
    }
    std::copy(f.begin(), f.end(), outputs[stage]->begin());
  }
  for (std::size_t i = 0; i < check_value.size(); ++i) keys.check[i % 8] ^= check_value[i];
  secure_zero(std::span<std::uint8_t>(u.data(), u.size()));
  secure_zero(std::span<std::uint8_t>(f.data(), f.size()));
  secure_zero(std::span<std::uint8_t>(check_value.data(), check_value.size()));
  return keys;
}

Rar3Key rar3_key(std::string_view password, ByteView salt) {
  const std::u16string wide = util::utf8_to_utf16(password);
  std::vector<std::uint8_t> seed;
  seed.reserve(wide.size() * 2 + salt.size());
  for (const char16_t c : wide) {
    seed.push_back(static_cast<std::uint8_t>(c & 0xFFu));
    seed.push_back(static_cast<std::uint8_t>((c >> 8) & 0xFFu));
  }
  for (const std::byte b : salt) seed.push_back(std::to_integer<std::uint8_t>(b));
  constexpr std::uint32_t rounds = 0x40000;
  Rar3Sha1 sha;
  Rar3Key out;
  for (std::uint32_t i = 0; i < rounds; ++i) {
    sha.update(seed.data(), seed.size());
    std::uint8_t counter[3] = {static_cast<std::uint8_t>(i & 0xFFu), static_cast<std::uint8_t>((i >> 8) & 0xFFu),
                               static_cast<std::uint8_t>((i >> 16) & 0xFFu)};
    sha.update(counter, 3);
    if (i % (rounds / 16) == 0) {
      Rar3Sha1 copy = sha;
      out.iv[i / (rounds / 16)] = copy.finish()[19];
    }
  }
  const std::array<std::uint8_t, 20> digest = sha.finish();
  for (std::size_t word = 0; word < 4; ++word) {
    for (std::size_t j = 0; j < 4; ++j) out.key[word * 4 + j] = digest[word * 4 + 3 - j];
  }
  secure_zero(std::span<std::uint8_t>(seed.data(), seed.size()));
  return out;
}

}  // namespace cvt::archive::rar
