// Convertible core - archive family: LZFSE and LZVN decoding (Method::Lzfse).
//
// An LZFSE stream is a series of blocks, each starting with a 4-byte magic:
//   "bvx-"  stored:   u32 size, raw bytes
//   "bvxn"  LZVN:     u32 raw size, u32 payload size, LZVN opcodes ending with EOS
//   "bvx1"  LZFSE v1: header with plain 16-bit frequency tables
//   "bvx2"  LZFSE v2: header with bit-packed fields and frequency tables
//   "bvx$"  end of stream
// An LZFSE block codes literals with four interleaved FSE (tANS) states and
// (literal length, match length, distance) triples with three more; both
// bit streams are read backwards from the end of their payload. Matches may
// reach back into earlier blocks of the same stream.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "families/archive/codecs/apple_codecs.hpp"

namespace cvt::compression::detail {

namespace {

constexpr std::uint32_t magic_stored = 0x2D787662u;  // "bvx-"
constexpr std::uint32_t magic_end = 0x24787662u;     // "bvx$"
constexpr std::uint32_t magic_v1 = 0x31787662u;      // "bvx1"
constexpr std::uint32_t magic_v2 = 0x32787662u;      // "bvx2"
constexpr std::uint32_t magic_lzvn = 0x6E787662u;    // "bvxn"

constexpr unsigned kLStates = 64;
constexpr unsigned kMStates = 64;
constexpr unsigned kDStates = 256;
constexpr unsigned kLiteralStates = 1024;
constexpr unsigned kLSymbols = 20;
constexpr unsigned kMSymbols = 20;
constexpr unsigned kDSymbols = 64;
constexpr unsigned kLiteralSymbols = 256;
constexpr unsigned kNumFreqs = kLSymbols + kMSymbols + kDSymbols + kLiteralSymbols;
constexpr std::uint32_t kMaxLiterals = 40000 + 64;
constexpr std::size_t kV1HeaderSize = 772;
constexpr std::size_t kV2FixedHeader = 32;

constexpr std::array<std::uint8_t, kLSymbols> l_extra = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 5, 8};
constexpr std::array<std::int32_t, kLSymbols> l_base = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                                                        10, 11, 12, 13, 14, 15, 16, 20, 28, 60};
constexpr std::array<std::uint8_t, kMSymbols> m_extra = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 5, 8, 11};
constexpr std::array<std::int32_t, kMSymbols> m_base = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                                                        10, 11, 12, 13, 14, 15, 16, 24, 56, 312};

/// Distance classes: four per extra-bit count 0..15.
constexpr std::array<std::uint8_t, kDSymbols> make_d_extra() {
  std::array<std::uint8_t, kDSymbols> t{};
  for (unsigned i = 0; i < kDSymbols; ++i) t[i] = static_cast<std::uint8_t>(i / 4);
  return t;
}
constexpr std::array<std::int32_t, kDSymbols> make_d_base() {
  std::array<std::int32_t, kDSymbols> t{};
  std::int32_t v = 0;
  for (unsigned i = 0; i < kDSymbols; ++i) {
    t[i] = v;
    v += std::int32_t{1} << (i / 4);
  }
  return t;
}
constexpr std::array<std::uint8_t, kDSymbols> d_extra = make_d_extra();
constexpr std::array<std::int32_t, kDSymbols> d_base = make_d_base();
static_assert(d_base[63] == 229372);

std::uint32_t le32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint64_t le64(const std::uint8_t* p) {
  std::uint64_t v;
  std::memcpy(&v, p, 8);
  if constexpr (std::endian::native == std::endian::big) {
    v = ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) | ((v & 0x0000000000FF0000ull) << 24) |
        ((v & 0x00000000FF000000ull) << 8) | ((v & 0x000000FF00000000ull) >> 8) | ((v & 0x0000FF0000000000ull) >> 24) |
        ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
  }
  return v;
}

/// Copy `m` bytes from `distance` back (the regions may overlap).
inline void copy_match(std::uint8_t* dst, std::size_t distance, std::size_t m) {
  const std::uint8_t* from = dst - distance;
  if (distance >= m) {
    std::memcpy(dst, from, m);
  } else if (distance == 1) {
    std::memset(dst, *from, m);
  } else {
    // Repeat the period in non-overlapping pieces of `distance` bytes.
    for (std::size_t done = 0; done < m;) {
      const std::size_t n = std::min(distance, m - done);
      std::memcpy(dst + done, from + done, n);
      done += n;
    }
  }
}

// ------------------------------------------------------------------ FSE

struct LiteralEntry {
  std::uint8_t k = 0;
  std::uint8_t symbol = 0;
  std::int16_t delta = 0;
};

struct ValueEntry {
  std::uint8_t total_bits = 0;
  std::uint8_t value_bits = 0;
  std::int16_t delta = 0;
  std::int32_t vbase = 0;
};

/// The frequencies of one block, validated: each table sums to at most its state count.
struct Tables {
  std::array<std::uint16_t, kNumFreqs> freq{};
  [[nodiscard]] std::span<const std::uint16_t> l() const { return std::span(freq).subspan(0, kLSymbols); }
  [[nodiscard]] std::span<const std::uint16_t> m() const { return std::span(freq).subspan(kLSymbols, kMSymbols); }
  [[nodiscard]] std::span<const std::uint16_t> d() const {
    return std::span(freq).subspan(kLSymbols + kMSymbols, kDSymbols);
  }
  [[nodiscard]] std::span<const std::uint16_t> literal() const {
    return std::span(freq).subspan(kLSymbols + kMSymbols + kDSymbols, kLiteralSymbols);
  }
};

bool freq_ok(std::span<const std::uint16_t> f, unsigned states) {
  unsigned sum = 0;
  for (const std::uint16_t v : f) sum += v;
  return sum <= states;
}

/// State s of a symbol with frequency f covers 2^k or 2^(k-1) next states
/// (k chosen so that states <= f << k < 2 * states).
template <class Emit>
void build_states(unsigned nstates, std::span<const std::uint16_t> freq, Emit emit) {
  const int n_clz = std::countl_zero(static_cast<std::uint32_t>(nstates));
  unsigned pos = 0;
  for (unsigned i = 0; i < freq.size(); ++i) {
    const unsigned f = freq[i];
    if (f == 0) continue;
    const int k = std::countl_zero(static_cast<std::uint32_t>(f)) - n_clz;
    const unsigned j0 = ((2 * nstates) >> k) - f;
    for (unsigned j = 0; j < f; ++j, ++pos) {
      if (j < j0) {
        emit(pos, i, k, static_cast<int>(((f + j) << k) - nstates));
      } else {
        emit(pos, i, k - 1, static_cast<int>((j - j0) << (k - 1)));
      }
    }
  }
}

void build_literal_table(std::span<const std::uint16_t> freq, std::vector<LiteralEntry>& t) {
  t.assign(kLiteralStates, LiteralEntry{});
  build_states(kLiteralStates, freq, [&](unsigned pos, unsigned sym, int k, int delta) {
    t[pos] = LiteralEntry{static_cast<std::uint8_t>(k), static_cast<std::uint8_t>(sym), static_cast<std::int16_t>(delta)};
  });
}

template <std::size_t N>
void build_value_table(unsigned nstates, std::span<const std::uint16_t> freq, const std::array<std::uint8_t, N>& extra,
                       const std::array<std::int32_t, N>& base, std::vector<ValueEntry>& t) {
  t.assign(nstates, ValueEntry{});
  build_states(nstates, freq, [&](unsigned pos, unsigned sym, int k, int delta) {
    t[pos] = ValueEntry{static_cast<std::uint8_t>(k + extra[sym]), extra[sym], static_cast<std::int16_t>(delta), base[sym]};
  });
}

/// Backward bit reader over [start, start + pos).
struct BitIn {
  const std::uint8_t* start = nullptr;
  std::size_t pos = 0;
  std::uint64_t accum = 0;
  int nbits = 0;

  bool init(int n) {
    if (n != 0) {
      if (pos < 8) return false;
      pos -= 8;
      accum = le64(start + pos);
      nbits = n + 64;
    } else {
      if (pos < 7) return false;
      pos -= 7;
      accum = le64_7(start + pos);
      nbits = 56;
    }
    return nbits >= 56 && nbits < 64 && (accum >> nbits) == 0;
  }
  static std::uint64_t le64_7(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 6; i >= 0; --i) v = (v << 8) | p[i];
    return v;
  }
  bool flush() {
    const int nb = (63 - nbits) & ~7;
    const auto bytes = static_cast<std::size_t>(nb >> 3);
    if (bytes > pos) return false;
    pos -= bytes;
    if (nb == 0) return true;
    std::uint64_t incoming = 0;
    if (pos + bytes >= 8) {
      incoming = le64(start + pos + bytes - 8) >> (64 - nb);  // the `bytes` bytes ending at the old position
    } else {
      for (std::size_t i = bytes; i-- > 0;) incoming = (incoming << 8) | start[pos + i];
    }
    accum = (accum << nb) | incoming;
    nbits += nb;
    return true;
  }
  std::uint64_t pull(int n) {
    nbits -= n;
    const std::uint64_t result = accum >> nbits;
    accum &= (std::uint64_t{1} << nbits) - 1;
    return result;
  }
};

std::int32_t value_decode(unsigned& state, const std::vector<ValueEntry>& t, BitIn& in) {
  const ValueEntry& e = t[state];
  const std::uint64_t bits = in.pull(e.total_bits);
  state = static_cast<unsigned>(e.delta + static_cast<int>(bits >> e.value_bits));
  return e.vbase + static_cast<std::int32_t>(bits & ((std::uint64_t{1} << e.value_bits) - 1));
}

struct BlockHeader {
  std::uint32_t n_raw = 0;
  std::uint32_t n_literals = 0;
  std::uint32_t n_matches = 0;
  std::uint32_t n_literal_payload = 0;
  std::uint32_t n_lmd_payload = 0;
  int literal_bits = 0;
  int lmd_bits = 0;
  std::array<unsigned, 4> literal_state{};
  unsigned l_state = 0;
  unsigned m_state = 0;
  unsigned d_state = 0;
  std::size_t header_size = 0;
  Tables tables;
};

/// One frequency value of the v2 header: a prefix code read LSB first.
unsigned v2_freq_value(std::uint32_t bits, int& nbits) {
  static constexpr std::array<std::int8_t, 32> nbits_table = {2, 3, 2, 5, 2, 3, 2, 8, 2, 3, 2, 5, 2, 3, 2, 14,
                                                              2, 3, 2, 5, 2, 3, 2, 8, 2, 3, 2, 5, 2, 3, 2, 14};
  static constexpr std::array<std::int8_t, 32> value_table = {0, 2, 1, 4, 0, 3, 1, -1, 0, 2, 1, 5, 0, 3, 1, -1,
                                                              0, 2, 1, 6, 0, 3, 1, -1, 0, 2, 1, 7, 0, 3, 1, -1};
  const std::uint32_t b = bits & 31u;
  nbits = nbits_table[b];
  if (nbits == 8) return 8 + ((bits >> 4) & 0xFu);
  if (nbits == 14) return 24 + ((bits >> 4) & 0x3FFu);
  return static_cast<unsigned>(value_table[b]);
}

Result<BlockHeader> parse_v2(const std::uint8_t* p, std::size_t avail) {
  if (avail < kV2FixedHeader) return fail(Status::Decode, "LZFSE block header is truncated");
  BlockHeader h;
  h.n_raw = le32(p + 4);
  const std::uint64_t f0 = le64(p + 8);
  const std::uint64_t f1 = le64(p + 16);
  const std::uint64_t f2 = le64(p + 24);
  h.n_literals = static_cast<std::uint32_t>(f0 & 0xFFFFFu);
  h.n_literal_payload = static_cast<std::uint32_t>((f0 >> 20) & 0xFFFFFu);
  h.n_matches = static_cast<std::uint32_t>((f0 >> 40) & 0xFFFFFu);
  h.literal_bits = static_cast<int>((f0 >> 60) & 7u) - 7;
  for (unsigned i = 0; i < 4; ++i) h.literal_state[i] = static_cast<unsigned>((f1 >> (10 * i)) & 0x3FFu);
  h.n_lmd_payload = static_cast<std::uint32_t>((f1 >> 40) & 0xFFFFFu);
  h.lmd_bits = static_cast<int>((f1 >> 60) & 7u) - 7;
  h.header_size = static_cast<std::size_t>(f2 & 0xFFFFFFFFu);
  h.l_state = static_cast<unsigned>((f2 >> 32) & 0x3FFu);
  h.m_state = static_cast<unsigned>((f2 >> 42) & 0x3FFu);
  h.d_state = static_cast<unsigned>((f2 >> 52) & 0x3FFu);
  if (h.header_size < kV2FixedHeader || h.header_size > avail) return fail(Status::Decode, "LZFSE block header is damaged");
  if (h.header_size > kV2FixedHeader) {
    const std::uint8_t* src = p + kV2FixedHeader;
    const std::uint8_t* end = p + h.header_size;
    std::uint32_t accum = 0;
    int accum_nbits = 0;
    for (unsigned i = 0; i < kNumFreqs; ++i) {
      while (src < end && accum_nbits + 8 <= 32) {
        accum |= static_cast<std::uint32_t>(*src++) << accum_nbits;
        accum_nbits += 8;
      }
      int nbits = 0;
      h.tables.freq[i] = static_cast<std::uint16_t>(v2_freq_value(accum, nbits));
      if (nbits > accum_nbits) return fail(Status::Decode, "LZFSE frequency table is damaged");
      accum >>= nbits;
      accum_nbits -= nbits;
    }
    if (accum_nbits >= 8 || src != end) return fail(Status::Decode, "LZFSE frequency table is damaged");
  }
  return h;
}

Result<BlockHeader> parse_v1(const std::uint8_t* p, std::size_t avail) {
  if (avail < kV1HeaderSize) return fail(Status::Decode, "LZFSE block header is truncated");
  BlockHeader h;
  h.n_raw = le32(p + 4);
  const std::uint32_t n_payload = le32(p + 8);
  h.n_literals = le32(p + 12);
  h.n_matches = le32(p + 16);
  h.n_literal_payload = le32(p + 20);
  h.n_lmd_payload = le32(p + 24);
  h.literal_bits = static_cast<std::int32_t>(le32(p + 28));
  for (unsigned i = 0; i < 4; ++i) h.literal_state[i] = le16(p + 32 + 2 * i);
  h.lmd_bits = static_cast<std::int32_t>(le32(p + 40));
  h.l_state = le16(p + 44);
  h.m_state = le16(p + 46);
  h.d_state = le16(p + 48);
  for (unsigned i = 0; i < kNumFreqs; ++i) h.tables.freq[i] = le16(p + 50 + 2 * i);
  h.header_size = kV1HeaderSize;
  if (std::uint64_t{h.n_literal_payload} + h.n_lmd_payload != n_payload) {
    return fail(Status::Decode, "LZFSE block header is damaged");
  }
  return h;
}

struct Scratch {
  std::vector<LiteralEntry> literal_table;
  std::vector<ValueEntry> l_table;
  std::vector<ValueEntry> m_table;
  std::vector<ValueEntry> d_table;
  std::vector<std::uint8_t> literals;
};

/// Decode one v1/v2 block whose payload follows the header; returns the input bytes used.
/// The bit readers may refill from bytes before their payload (the bits they
/// load there are never consumed by a valid stream), so they are bounded by
/// the start of the whole input, `input`; the block starts at input + at.
Result<std::size_t> decode_fse_block(const BlockHeader& h, const std::uint8_t* input, std::size_t at, std::size_t avail,
                                     Bytes& out, std::size_t block_start, Scratch& sc) {
  const BlockHeader& b = h;
  if (b.literal_bits < -7 || b.literal_bits > 0 || b.lmd_bits < -7 || b.lmd_bits > 0) {
    return fail(Status::Decode, "LZFSE block header is damaged");
  }
  for (const unsigned s : b.literal_state) {
    if (s >= kLiteralStates) return fail(Status::Decode, "LZFSE block header is damaged");
  }
  if (b.l_state >= kLStates || b.m_state >= kMStates || b.d_state >= kDStates || b.n_literals > kMaxLiterals) {
    return fail(Status::Decode, "LZFSE block header is damaged");
  }
  if (!freq_ok(b.tables.l(), kLStates) || !freq_ok(b.tables.m(), kMStates) || !freq_ok(b.tables.d(), kDStates) ||
      !freq_ok(b.tables.literal(), kLiteralStates)) {
    return fail(Status::Decode, "LZFSE frequency table is damaged");
  }
  const std::uint64_t used = std::uint64_t{b.header_size} + b.n_literal_payload + b.n_lmd_payload;
  if (used > avail) return fail(Status::Decode, "LZFSE block is truncated");

  build_literal_table(b.tables.literal(), sc.literal_table);
  build_value_table(kLStates, b.tables.l(), l_extra, l_base, sc.l_table);
  build_value_table(kMStates, b.tables.m(), m_extra, m_base, sc.m_table);
  build_value_table(kDStates, b.tables.d(), d_extra, d_base, sc.d_table);

  // Literals: four interleaved states, decoded in groups of four.
  const std::uint32_t n_lit = (b.n_literals + 3u) & ~3u;
  sc.literals.assign(n_lit + 16, 0);  // 16 bytes of slack for wide copies
  {
    BitIn in;
    in.start = input;
    in.pos = at + b.header_size + b.n_literal_payload;
    if (!in.init(b.literal_bits)) return fail(Status::Decode, "LZFSE literal stream is damaged");
    std::array<unsigned, 4> st = b.literal_state;
    for (std::uint32_t i = 0; i < n_lit; i += 4) {
      if (!in.flush()) return fail(Status::Decode, "LZFSE literal stream is damaged");
      for (unsigned j = 0; j < 4; ++j) {
        const LiteralEntry& e = sc.literal_table[st[j]];
        sc.literals[i + j] = e.symbol;
        st[j] = static_cast<unsigned>(e.delta + static_cast<int>(in.pull(e.k)));
      }
    }
  }

  // (L, M, D) triples.
  BitIn in;
  in.start = input;
  in.pos = at + b.header_size + b.n_literal_payload + b.n_lmd_payload;
  if (!in.init(b.lmd_bits)) return fail(Status::Decode, "LZFSE match stream is damaged");
  unsigned l_state = b.l_state;
  unsigned m_state = b.m_state;
  unsigned d_state = b.d_state;
  std::size_t lit = 0;
  std::size_t pos = block_start;
  const std::size_t end = block_start + b.n_raw;
  std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(out.data());
  std::size_t distance = 0;
  for (std::uint32_t n = 0; n < b.n_matches; ++n) {
    if (!in.flush()) return fail(Status::Decode, "LZFSE match stream is damaged");
    const auto l = static_cast<std::size_t>(value_decode(l_state, sc.l_table, in));
    const auto m = static_cast<std::size_t>(value_decode(m_state, sc.m_table, in));
    const auto d = static_cast<std::size_t>(value_decode(d_state, sc.d_table, in));
    if (d != 0) distance = d;
    if (l > n_lit - lit || l > end - pos || m > end - pos - l) return fail(Status::Decode, "LZFSE match is out of range");
    if (end - pos >= l + 16) {
      // Wide copy: may write up to 15 bytes past the literals, inside the block.
      for (std::size_t k = 0; k < l; k += 16) std::memcpy(dst + pos + k, sc.literals.data() + lit + k, 16);
    } else {
      std::memcpy(dst + pos, sc.literals.data() + lit, l);
    }
    lit += l;
    pos += l;
    if (m != 0) {
      if (distance == 0 || distance > pos) return fail(Status::Decode, "LZFSE match distance is out of range");
      if (distance >= 16 && end - pos >= m + 16) {
        for (std::size_t k = 0; k < m; k += 16) std::memcpy(dst + pos + k, dst + pos - distance + k, 16);
      } else {
        copy_match(dst + pos, distance, m);
      }
      pos += m;
    }
  }
  if (pos != end) return fail(Status::Decode, "LZFSE block decodes to {} bytes instead of {}", pos - block_start, b.n_raw);
  return static_cast<std::size_t>(used);
}

/// LZVN payload into out[block_start, block_start + n_raw).
Result<void> decode_lzvn(const std::uint8_t* src, std::size_t n, Bytes& out, std::size_t block_start, std::size_t n_raw) {
  std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(out.data());
  std::size_t pos = block_start;
  const std::size_t end = block_start + n_raw;
  std::size_t i = 0;
  std::size_t d_prev = 0;
  const auto bad = [] { return fail(Status::Decode, "LZVN data is damaged"); };
  for (;;) {
    if (i >= n) return fail(Status::Decode, "LZVN data is truncated");
    const unsigned op = src[i];
    std::size_t lit = 0;
    std::size_t match = 0;
    std::size_t dist = d_prev;
    std::size_t len = 1;
    if (op == 0x06) {  // end of stream: opcode + 7 padding bytes
      if (n - i < 8) return fail(Status::Decode, "LZVN data is truncated");
      break;
    }
    if (op == 0x0E || op == 0x16) {  // nop
      ++i;
      continue;
    }
    if (op >= 0xF0) {
      if (op == 0xF0) {
        if (n - i < 2) return fail(Status::Decode, "LZVN data is truncated");
        match = src[i + 1] + 16u;
        len = 2;
      } else {
        match = op & 0x0Fu;
      }
    } else if (op >= 0xE0) {
      if (op == 0xE0) {
        if (n - i < 2) return fail(Status::Decode, "LZVN data is truncated");
        lit = src[i + 1] + 16u;
        len = 2;
      } else {
        lit = op & 0x0Fu;
      }
    } else if ((op & 0xE0u) == 0xA0u) {  // medium distance
      if (n - i < 3) return fail(Status::Decode, "LZVN data is truncated");
      const unsigned b1 = src[i + 1];
      const unsigned b2 = src[i + 2];
      lit = (op >> 3) & 3u;
      match = (((op & 7u) << 2) | (b1 & 3u)) + 3u;
      dist = (static_cast<std::size_t>(b2) << 6) | (b1 >> 2);
      len = 3;
    } else if ((op & 0xF0u) == 0x70u || (op & 0xF0u) == 0xD0u || ((op & 7u) == 6u && op < 0x40u)) {
      return bad();
    } else {
      lit = op >> 6;
      match = ((op >> 3) & 7u) + 3u;
      if ((op & 7u) == 7u) {  // large distance
        if (n - i < 3) return fail(Status::Decode, "LZVN data is truncated");
        dist = src[i + 1] | (static_cast<std::size_t>(src[i + 2]) << 8);
        len = 3;
      } else if ((op & 7u) != 6u) {  // small distance (6 = previous distance)
        if (n - i < 2) return fail(Status::Decode, "LZVN data is truncated");
        dist = (static_cast<std::size_t>(op & 7u) << 8) | src[i + 1];
        len = 2;
      }
    }
    i += len;
    if (lit != 0) {
      if (lit > n - i) return fail(Status::Decode, "LZVN data is truncated");
      if (lit > end - pos) return bad();
      std::memcpy(dst + pos, src + i, lit);
      i += lit;
      pos += lit;
    }
    if (match != 0) {
      if (dist == 0 || dist > pos || match > end - pos) return bad();
      copy_match(dst + pos, dist, match);
      pos += match;
      d_prev = dist;
    }
  }
  if (pos != end) return fail(Status::Decode, "LZVN block decodes to {} bytes instead of {}", pos - block_start, n_raw);
  return {};
}

}  // namespace

Result<Bytes> lzfse_decode(ByteView data, const DecodeOptions& options) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::size_t n = data.size();
  std::size_t at = 0;
  Bytes out;
  Scratch sc;
  for (;;) {
    if (options.stop_after != 0 && out.size() >= options.stop_after) return out;
    if (!options.progress.report(static_cast<double>(at) / static_cast<double>(n == 0 ? 1 : n))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    if (n - at < 4) return fail(Status::Decode, "LZFSE stream is truncated (no end-of-stream block)");
    const std::uint32_t magic = le32(p + at);
    if (magic == magic_end) return out;
    if (n - at < 8) return fail(Status::Decode, "LZFSE block header is truncated");
    const std::uint32_t n_raw = le32(p + at + 4);
    CVT_TRY(check_output_room(out.size(), n_raw, options.max_output));
    if (n_raw > std::numeric_limits<std::size_t>::max() - out.size()) return fail(Status::Limit, "LZFSE block is too large");
    const std::size_t start = out.size();
    switch (magic) {
      case magic_stored: {
        if (n_raw > n - at - 8) return fail(Status::Decode, "LZFSE stored block is truncated");
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at + 8),
                   data.begin() + static_cast<std::ptrdiff_t>(at + 8 + n_raw));
        at += 8 + std::size_t{n_raw};
        break;
      }
      case magic_lzvn: {
        if (n - at < 12) return fail(Status::Decode, "LZVN block header is truncated");
        const std::uint32_t n_payload = le32(p + at + 8);
        if (n_payload > n - at - 12) return fail(Status::Decode, "LZVN block is truncated");
        // No LZVN opcode expands to more than 136 bytes per input byte: refuse
        // to allocate what the payload cannot possibly produce.
        if (std::uint64_t{n_raw} > std::uint64_t{n_payload} * 300 + 300) {
          return fail(Status::Decode, "LZVN block declares an impossible size");
        }
        out.resize(start + n_raw);
        CVT_TRY(decode_lzvn(p + at + 12, n_payload, out, start, n_raw));
        at += 12 + std::size_t{n_payload};
        break;
      }
      case magic_v1:
      case magic_v2: {
        Result<BlockHeader> h = magic == magic_v1 ? parse_v1(p + at, n - at) : parse_v2(p + at, n - at);
        if (!h) return fail(h.error());
        // Every match triple yields at most 315 + 2359 bytes; literals one each.
        const std::uint64_t most = std::uint64_t{h->n_matches} * (315 + 2359) + h->n_literals;
        if (n_raw > most) return fail(Status::Decode, "LZFSE block declares an impossible size");
        out.resize(start + n_raw);
        CVT_TRY_ASSIGN(const std::size_t used, decode_fse_block(*h, p, at, n - at, out, start, sc));
        at += used;
        break;
      }
      default:
        return fail(Status::Decode, "unknown LZFSE block type {:08X}", magic);
    }
  }
}

}  // namespace cvt::compression::detail
