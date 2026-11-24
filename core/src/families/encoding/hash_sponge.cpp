// Convertible core - encoding family: SHA-3, SHAKE and Keccak (FIPS 202),
// BLAKE2b/BLAKE2s (RFC 7693) and BLAKE3.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "util/bytes.hpp"
#include "families/encoding/hashes.hpp"

namespace cvt::encoding {

namespace {

using std::rotl;
using std::rotr;

const std::uint8_t* bytes_of(ByteView data) noexcept { return reinterpret_cast<const std::uint8_t*>(data.data()); }

std::uint64_t load64le(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
std::uint32_t load32le(const std::uint8_t* p) noexcept {
  return (std::uint32_t{p[3]} << 24) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[1]} << 8) | p[0];
}

// --------------------------------------------------------------------------- Keccak-f[1600]

// Round constants from the degree-8 LFSR of FIPS 202, 3.2.5.
constexpr bool keccak_rc_bit(unsigned t) noexcept {
  if (t % 255 == 0) return true;
  unsigned r = 1;
  for (unsigned i = 1; i <= t % 255; ++i) {
    r <<= 1;
    if (r & 0x100u) r ^= 0x171u;
  }
  return (r & 1u) != 0;
}
constexpr std::array<std::uint64_t, 24> make_keccak_round_constants() noexcept {
  std::array<std::uint64_t, 24> rc{};
  for (unsigned round = 0; round < 24; ++round) {
    for (unsigned j = 0; j <= 6; ++j) {
      if (keccak_rc_bit(j + 7 * round)) rc[round] |= std::uint64_t{1} << ((1u << j) - 1);
    }
  }
  return rc;
}
constexpr std::array<std::uint64_t, 24> keccak_rc = make_keccak_round_constants();
// The rho offsets and pi lane order, walked along the (x, y) -> (y, 2x + 3y) cycle.
constexpr std::array<int, 24> keccak_rho = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
                                            27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
constexpr std::array<std::size_t, 24> keccak_pi = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
                                                   15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

void keccak_f(std::array<std::uint64_t, 25>& a) noexcept {
  for (const std::uint64_t rc : keccak_rc) {
    std::array<std::uint64_t, 5> c{};
    for (std::size_t x = 0; x < 5; ++x) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
    for (std::size_t x = 0; x < 5; ++x) {
      const std::uint64_t d = c[(x + 4) % 5] ^ rotl(c[(x + 1) % 5], 1);
      for (std::size_t y = 0; y < 25; y += 5) a[y + x] ^= d;
    }
    std::uint64_t carry = a[1];
    for (std::size_t i = 0; i < 24; ++i) {
      const std::size_t j = keccak_pi[i];
      const std::uint64_t next = a[j];
      a[j] = rotl(carry, keccak_rho[i]);
      carry = next;
    }
    for (std::size_t y = 0; y < 25; y += 5) {
      std::array<std::uint64_t, 5> row{};
      for (std::size_t x = 0; x < 5; ++x) row[x] = a[y + x];
      for (std::size_t x = 0; x < 5; ++x) a[y + x] = row[x] ^ (~row[(x + 1) % 5] & row[(x + 2) % 5]);
    }
    a[0] ^= rc;
  }
}

/// The sponge construction with a byte-aligned domain separator: 0x06 for
/// SHA-3, 0x1F for SHAKE, 0x01 for the original Keccak submission.
class Sponge final : public Hasher {
 public:
  Sponge(std::size_t rate, std::uint8_t domain, std::size_t output) : rate_(rate), domain_(domain), output_(output) {}
  void update(ByteView data) override {
    for (const std::byte b : data) {
      lane_byte(position_) ^= std::to_integer<std::uint8_t>(b);
      if (++position_ == rate_) {
        keccak_f(state_);
        position_ = 0;
      }
    }
  }
  Bytes finish() override {
    lane_byte(position_) ^= domain_;
    lane_byte(rate_ - 1) ^= 0x80;
    keccak_f(state_);
    Bytes out;
    out.reserve(output_);
    std::size_t at = 0;
    while (out.size() < output_) {
      if (at == rate_) {
        keccak_f(state_);
        at = 0;
      }
      out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(lane_byte(at++))));
    }
    return out;
  }

 private:
  // Lanes are little-endian: byte i of the state is byte (i % 8) of lane i / 8.
  struct LaneByte {
    std::uint64_t& lane;
    unsigned shift;
    LaneByte& operator^=(std::uint8_t v) {
      lane ^= std::uint64_t{v} << shift;
      return *this;
    }
    operator std::uint8_t() const { return static_cast<std::uint8_t>(lane >> shift); }  // NOLINT
  };
  LaneByte lane_byte(std::size_t i) { return LaneByte{state_[i / 8], static_cast<unsigned>((i % 8) * 8)}; }

  std::array<std::uint64_t, 25> state_{};
  std::size_t rate_;
  std::uint8_t domain_;
  std::size_t output_;
  std::size_t position_ = 0;
};

// --------------------------------------------------------------------------- BLAKE2 (RFC 7693)

constexpr std::array<std::array<std::uint8_t, 16>, 10> blake2_sigma = {{
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
}};

// The initialisation vectors are those of SHA-512 and SHA-256.
constexpr std::array<std::uint64_t, 8> blake2b_iv = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
constexpr std::array<std::uint32_t, 8> blake2s_iv = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                                     0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};

/// Word = uint64_t (BLAKE2b, 12 rounds, rotations 32/24/16/63) or uint32_t
/// (BLAKE2s, 10 rounds, rotations 16/12/8/7).
template <class Word>
class Blake2 final : public Hasher {
  static constexpr bool wide = sizeof(Word) == 8;
  static constexpr std::size_t block = 16 * sizeof(Word);
  static constexpr int rounds = wide ? 12 : 10;
  static constexpr std::array<int, 4> rot = wide ? std::array<int, 4>{32, 24, 16, 63} : std::array<int, 4>{16, 12, 8, 7};

 public:
  Blake2(std::size_t out_len, ByteView key) : out_len_(out_len) {
    for (std::size_t i = 0; i < 8; ++i) h_[i] = iv(i);
    h_[0] ^= static_cast<Word>(0x01010000u ^ (key.size() << 8) ^ out_len);
    if (!key.empty()) {
      std::array<std::byte, block> padded{};
      ::cvt::util::copy_bytes(padded.data(), key.data(), key.size());
      update(ByteView(padded.data(), padded.size()));
    }
  }
  void update(ByteView data) override {
    for (const std::byte b : data) {
      if (filled_ == block) {  // keep the last block for `finish`, which flags it
        counter_ += block;
        compress(false);
        filled_ = 0;
      }
      buffer_[filled_++] = std::to_integer<std::uint8_t>(b);
    }
  }
  Bytes finish() override {
    counter_ += filled_;
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(filled_), buffer_.end(), std::uint8_t{0});
    compress(true);
    Bytes out;
    for (std::size_t i = 0; i < out_len_; ++i) out.push_back(static_cast<std::byte>(h_[i / sizeof(Word)] >> (8 * (i % sizeof(Word)))));
    return out;
  }

 private:
  static constexpr Word iv(std::size_t i) {
    if constexpr (wide) {
      return blake2b_iv[i];
    } else {
      return blake2s_iv[i];
    }
  }
  static Word load(const std::uint8_t* p) {
    if constexpr (wide) {
      return load64le(p);
    } else {
      return load32le(p);
    }
  }
  void compress(bool last) {
    std::array<Word, 16> m{};
    for (std::size_t i = 0; i < 16; ++i) m[i] = load(buffer_.data() + i * sizeof(Word));
    std::array<Word, 16> v{};
    for (std::size_t i = 0; i < 8; ++i) {
      v[i] = h_[i];
      v[i + 8] = iv(i);
    }
    v[12] ^= static_cast<Word>(counter_);
    if constexpr (wide) {
      v[13] ^= 0;  // the upper half of the 128-bit counter: inputs never reach 2^64 bytes
    } else {
      v[13] ^= static_cast<Word>(counter_ >> 32);
    }
    if (last) v[14] = static_cast<Word>(~v[14]);
    const auto g = [&v](std::size_t a, std::size_t b, std::size_t c, std::size_t d, Word x, Word y) {
      v[a] = v[a] + v[b] + x;
      v[d] = rotr(static_cast<Word>(v[d] ^ v[a]), rot[0]);
      v[c] = v[c] + v[d];
      v[b] = rotr(static_cast<Word>(v[b] ^ v[c]), rot[1]);
      v[a] = v[a] + v[b] + y;
      v[d] = rotr(static_cast<Word>(v[d] ^ v[a]), rot[2]);
      v[c] = v[c] + v[d];
      v[b] = rotr(static_cast<Word>(v[b] ^ v[c]), rot[3]);
    };
    for (int r = 0; r < rounds; ++r) {
      const auto& s = blake2_sigma[static_cast<std::size_t>(r % 10)];
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

  std::array<Word, 8> h_{};
  std::array<std::uint8_t, block> buffer_{};
  std::size_t filled_ = 0;
  std::uint64_t counter_ = 0;
  std::size_t out_len_;
};

// --------------------------------------------------------------------------- BLAKE3

namespace b3 {

constexpr std::uint32_t chunk_start = 1, chunk_end = 2, parent = 4, root = 8, keyed_hash = 16;
constexpr std::size_t chunk_len = 1024;
constexpr std::array<std::uint32_t, 8> iv = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                             0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
constexpr std::array<std::size_t, 16> permutation = {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8};

using Words8 = std::array<std::uint32_t, 8>;
using Words16 = std::array<std::uint32_t, 16>;

Words16 compress(const Words8& cv, Words16 m, std::uint64_t counter, std::uint32_t block_len, std::uint32_t flags) {
  Words16 s = {cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7], iv[0], iv[1], iv[2], iv[3],
               static_cast<std::uint32_t>(counter), static_cast<std::uint32_t>(counter >> 32), block_len, flags};
  const auto g = [&s](std::size_t a, std::size_t b, std::size_t c, std::size_t d, std::uint32_t x, std::uint32_t y) {
    s[a] = s[a] + s[b] + x;
    s[d] = rotr(s[d] ^ s[a], 16);
    s[c] = s[c] + s[d];
    s[b] = rotr(s[b] ^ s[c], 12);
    s[a] = s[a] + s[b] + y;
    s[d] = rotr(s[d] ^ s[a], 8);
    s[c] = s[c] + s[d];
    s[b] = rotr(s[b] ^ s[c], 7);
  };
  for (int round = 0; round < 7; ++round) {
    g(0, 4, 8, 12, m[0], m[1]);
    g(1, 5, 9, 13, m[2], m[3]);
    g(2, 6, 10, 14, m[4], m[5]);
    g(3, 7, 11, 15, m[6], m[7]);
    g(0, 5, 10, 15, m[8], m[9]);
    g(1, 6, 11, 12, m[10], m[11]);
    g(2, 7, 8, 13, m[12], m[13]);
    g(3, 4, 9, 14, m[14], m[15]);
    Words16 permuted{};
    for (std::size_t i = 0; i < 16; ++i) permuted[i] = m[permutation[i]];
    m = permuted;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    s[i] ^= s[i + 8];
    s[i + 8] ^= cv[i];
  }
  return s;
}

Words16 words_of(const std::array<std::uint8_t, 64>& block) {
  Words16 w{};
  for (std::size_t i = 0; i < 16; ++i) w[i] = load32le(block.data() + 4 * i);
  return w;
}

Words8 first8(const Words16& w) {
  Words8 out{};
  std::copy_n(w.begin(), 8, out.begin());
  return out;
}

struct Output {
  Words8 cv;
  Words16 block;
  std::uint64_t counter;
  std::uint32_t block_len;
  std::uint32_t flags;
  [[nodiscard]] Words8 chaining_value() const { return first8(compress(cv, block, counter, block_len, flags)); }
  [[nodiscard]] Bytes root_bytes(std::size_t length) const {
    Bytes out;
    out.reserve(length);
    for (std::uint64_t block_counter = 0; out.size() < length; ++block_counter) {
      const Words16 w = compress(cv, block, block_counter, block_len, flags | root);
      for (std::size_t i = 0; i < 64 && out.size() < length; ++i) {
        out.push_back(static_cast<std::byte>(w[i / 4] >> (8 * (i % 4))));
      }
    }
    return out;
  }
};

struct ChunkState {
  Words8 cv;
  std::uint64_t counter = 0;
  std::array<std::uint8_t, 64> block{};
  std::size_t block_len = 0;
  std::size_t blocks_compressed = 0;
  std::uint32_t flags = 0;

  [[nodiscard]] std::size_t length() const { return 64 * blocks_compressed + block_len; }
  [[nodiscard]] std::uint32_t start_flag() const { return blocks_compressed == 0 ? chunk_start : 0; }
  void update(const std::uint8_t* p, std::size_t n) {
    while (n > 0) {
      if (block_len == 64) {
        cv = first8(compress(cv, words_of(block), counter, 64, flags | start_flag()));
        ++blocks_compressed;
        block.fill(0);
        block_len = 0;
      }
      const std::size_t take = std::min(64 - block_len, n);
      ::cvt::util::copy_bytes(block.data() + block_len, p, take);
      block_len += take;
      p += take;
      n -= take;
    }
  }
  [[nodiscard]] Output output() const {
    return Output{cv, words_of(block), counter, static_cast<std::uint32_t>(block_len), flags | start_flag() | chunk_end};
  }
};

class Blake3 final : public Hasher {
 public:
  Blake3(const Words8& key, std::uint32_t flags, std::size_t out_len) : key_(key), flags_(flags), out_len_(out_len) {
    chunk_ = ChunkState{key_, 0, {}, 0, 0, flags_};
  }
  void update(ByteView data) override {
    const std::uint8_t* p = bytes_of(data);
    std::size_t n = data.size();
    while (n > 0) {
      if (chunk_.length() == chunk_len) {
        Words8 chunk_cv = chunk_.output().chaining_value();
        std::uint64_t total = chunk_.counter + 1;
        // Merge completed subtrees: one merge per trailing zero bit of the chunk count.
        while ((total & 1u) == 0) {
          chunk_cv = parent_output(stack_.back(), chunk_cv).chaining_value();
          stack_.pop_back();
          total >>= 1;
        }
        stack_.push_back(chunk_cv);
        chunk_ = ChunkState{key_, chunk_.counter + 1, {}, 0, 0, flags_};
      }
      const std::size_t take = std::min(chunk_len - chunk_.length(), n);
      chunk_.update(p, take);
      p += take;
      n -= take;
    }
  }
  Bytes finish() override {
    Output output = chunk_.output();
    for (std::size_t i = stack_.size(); i-- > 0;) output = parent_output(stack_[i], output.chaining_value());
    return output.root_bytes(out_len_);
  }

 private:
  [[nodiscard]] Output parent_output(const Words8& left, const Words8& right) const {
    Words16 block{};
    std::copy(left.begin(), left.end(), block.begin());
    std::copy(right.begin(), right.end(), block.begin() + 8);
    return Output{key_, block, 0, 64, parent | flags_};
  }
  Words8 key_;
  std::uint32_t flags_;
  std::size_t out_len_;
  ChunkState chunk_{};
  std::vector<Words8> stack_;
};

}  // namespace b3

}  // namespace

std::unique_ptr<Hasher> make_sha3_224(const HashParams&) { return std::make_unique<Sponge>(144, 0x06, 28); }
std::unique_ptr<Hasher> make_sha3_256(const HashParams&) { return std::make_unique<Sponge>(136, 0x06, 32); }
std::unique_ptr<Hasher> make_sha3_384(const HashParams&) { return std::make_unique<Sponge>(104, 0x06, 48); }
std::unique_ptr<Hasher> make_sha3_512(const HashParams&) { return std::make_unique<Sponge>(72, 0x06, 64); }
std::unique_ptr<Hasher> make_shake128(const HashParams& p) {
  return std::make_unique<Sponge>(168, 0x1F, p.length == 0 ? 32 : p.length);
}
std::unique_ptr<Hasher> make_shake256(const HashParams& p) {
  return std::make_unique<Sponge>(136, 0x1F, p.length == 0 ? 64 : p.length);
}
std::unique_ptr<Hasher> make_keccak224(const HashParams&) { return std::make_unique<Sponge>(144, 0x01, 28); }
std::unique_ptr<Hasher> make_keccak256(const HashParams&) { return std::make_unique<Sponge>(136, 0x01, 32); }
std::unique_ptr<Hasher> make_keccak384(const HashParams&) { return std::make_unique<Sponge>(104, 0x01, 48); }
std::unique_ptr<Hasher> make_keccak512(const HashParams&) { return std::make_unique<Sponge>(72, 0x01, 64); }

std::unique_ptr<Hasher> make_blake2b(const HashParams& p) {
  return std::make_unique<Blake2<std::uint64_t>>(p.length == 0 ? 64 : std::min<std::size_t>(p.length, 64),
                                                 ByteView(p.key).first(std::min<std::size_t>(p.key.size(), 64)));
}
std::unique_ptr<Hasher> make_blake2s(const HashParams& p) {
  return std::make_unique<Blake2<std::uint32_t>>(p.length == 0 ? 32 : std::min<std::size_t>(p.length, 32),
                                                 ByteView(p.key).first(std::min<std::size_t>(p.key.size(), 32)));
}
std::unique_ptr<Hasher> make_blake3(const HashParams& p) {
  const std::size_t length = p.length == 0 ? 32 : p.length;
  if (p.key.size() == 32) {
    b3::Words8 key{};
    for (std::size_t i = 0; i < 8; ++i) key[i] = load32le(reinterpret_cast<const std::uint8_t*>(p.key.data()) + 4 * i);
    return std::make_unique<b3::Blake3>(key, b3::keyed_hash, length);
  }
  return std::make_unique<b3::Blake3>(b3::iv, 0, length);
}

}  // namespace cvt::encoding
