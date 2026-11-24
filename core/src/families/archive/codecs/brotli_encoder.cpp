// Convertible core - archive family: Brotli encoder (RFC 7932).
//
// Level mapping (all decisions use integer arithmetic, so the output is the
// same on every platform):
//   0     uncompressed meta-blocks
//   1-4   greedy matching over hash buckets of 1..16 recent positions,
//         distance cache (short distance codes), one prefix code per
//         alphabet and meta-block
//   5-8   lazy matching (two-step at 7-8) over 32..256 positions, literal
//         context modelling (UTF8 or signed mode, contexts clustered into
//         8..24 trees), distance trees by copy length, greedy literal block
//         splitting into 4..8 block types
//   9     ("ultra") shortest-path parsing over all candidate matches with a
//         cost model taken from the previous pass, static-dictionary
//         references (whole words under the transforms that keep them
//         intact: suffixes, prefixes, upper-case first letter), up to 32
//         literal trees and 12 literal block types
// The window is 4 MiB (16 MiB at level 9), never larger than the input
// needs; EncodeOptions::dictionary_size overrides it. Meta-blocks that do
// not shrink are stored uncompressed. Memory: the input, at most 16 MiB of
// hash buckets, and (level 9) about 10 MiB of parser state.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/brotli_internal.hpp"
#include "families/archive/codecs/huffman.hpp"

namespace cvt::compression::detail {

namespace {

namespace br = brotli;

struct LevelParams {
  int depth;          // hash bucket size (candidates per position), a power of two
  int lazy;           // 0 greedy, 1 lazy, 2 two-step lazy
  int nice;           // stop searching at this length
  int hash_bits;
  int lit_clusters;   // maximum literal context clusters (1 = no context modelling)
  int lit_types;      // maximum literal block types (1 = no block splitting)
  bool dictionary;    // static dictionary references
  std::uint32_t metablock;  // target meta-block size
  bool optimal;       // shortest-path parsing with an iterated cost model
};

constexpr std::array<LevelParams, 10> levels = {{
    {0, 0, 0, 0, 1, 1, false, 1u << 20, false},
    {1, 0, 32, 16, 1, 1, false, 1u << 18, false},
    {4, 0, 48, 15, 1, 1, false, 1u << 18, false},
    {8, 0, 64, 15, 1, 1, false, 1u << 18, false},
    {16, 0, 96, 14, 1, 1, false, 1u << 18, false},
    {32, 1, 128, 16, 8, 4, false, 1u << 20, false},
    {64, 1, 192, 16, 12, 6, false, 1u << 20, false},
    {128, 2, 256, 16, 16, 8, false, 1u << 21, false},
    {256, 2, 384, 16, 24, 8, false, 1u << 21, false},
    {128, 2, 1024, 16, 32, 12, true, 1u << 22, true},
}};

constexpr std::uint32_t max_copy = 1u << 20;
constexpr std::uint32_t max_insert_run = 1u << 20;

// ---------------------------------------------------------------------------- integer log2

// log2(x) in 1/256 bit units, integer only.
constexpr std::uint32_t log2_q8_exact(std::uint64_t x) {
  if (x == 0) return 0;
  const int n = std::bit_width(x) - 1;
  std::uint64_t v = n >= 31 ? (x >> (n - 31)) : (x << (31 - n));  // Q31 in [1, 2)
  std::uint32_t frac = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v * v) >> 31;
    frac <<= 1;
    if (v >= (std::uint64_t{1} << 32)) {
      v >>= 1;
      frac |= 1;
    }
  }
  return static_cast<std::uint32_t>(n) * 256u + frac;
}

constexpr std::array<std::uint32_t, 1024> log2_table = [] {
  std::array<std::uint32_t, 1024> t{};
  for (std::size_t i = 0; i < t.size(); ++i) t[i] = log2_q8_exact(i);
  return t;
}();

inline std::uint32_t log2_q8(std::uint64_t x) {
  if (x < log2_table.size()) return log2_table[static_cast<std::size_t>(x)];
  const int shift = std::bit_width(x) - 10;
  return log2_table[static_cast<std::size_t>(x >> shift)] + static_cast<std::uint32_t>(shift) * 256u;
}

inline int floor_log2(std::uint64_t x) { return std::bit_width(x) - 1; }

// ---------------------------------------------------------------------------- bit writer

class Writer {
 public:
  explicit Writer(Bytes& out) : out_(out) {}
  void put(std::uint64_t value, int n) {
    acc_ |= value << count_;
    count_ += n;
    if (count_ >= 32) {
      for (int i = 0; i < 4; ++i) out_.push_back(static_cast<std::byte>((acc_ >> (8 * i)) & 0xFFu));
      acc_ >>= 32;
      count_ -= 32;
    }
  }
  // Values of up to 56 bits.
  void put_long(std::uint64_t value, int n) {
    if (n > 24) {
      put(value & 0xFFFFFFu, 24);
      put(value >> 24, n - 24);
    } else {
      put(value, n);
    }
  }
  void align() {
    const int pad = (8 - count_ % 8) % 8;
    if (pad != 0) put(0, pad);
    while (count_ >= 8) {
      out_.push_back(static_cast<std::byte>(acc_ & 0xFFu));
      acc_ >>= 8;
      count_ -= 8;
    }
  }
  void raw(ByteView bytes) {
    align();
    out_.insert(out_.end(), bytes.begin(), bytes.end());
  }
  void finish() { align(); }
  [[nodiscard]] std::uint64_t bits() const { return std::uint64_t{out_.size()} * 8 + static_cast<std::uint64_t>(count_); }

  struct Mark {
    std::size_t size;
    std::uint64_t acc;
    int count;
  };
  [[nodiscard]] Mark mark() const { return Mark{out_.size(), acc_, count_}; }
  void restore(const Mark& m) {
    out_.resize(m.size);
    acc_ = m.acc;
    count_ = m.count;
  }

 private:
  Bytes& out_;
  std::uint64_t acc_ = 0;
  int count_ = 0;
};

// ---------------------------------------------------------------------------- prefix codes

using Histogram = std::vector<std::uint32_t>;

struct Code {
  std::vector<std::uint8_t> depth;
  std::vector<std::uint16_t> bits;  // bit-reversed for LSB-first output
};

void build_code(const Histogram& hist, Code& code, int max_bits = 15) {
  const std::size_t n = hist.size();
  code.depth.assign(n, 0);
  code.bits.assign(n, 0);
  std::size_t used = 0;
  for (const std::uint32_t c : hist) used += c != 0 ? 1u : 0u;
  if (used <= 1) return;  // a one-symbol code takes zero bits
  huffman_lengths(hist, max_bits, code.depth);
  std::vector<std::uint32_t> codes(n);
  canonical_codes(code.depth, codes);
  for (std::size_t i = 0; i < n; ++i) {
    if (code.depth[i] != 0) code.bits[i] = static_cast<std::uint16_t>(reverse_bits(codes[i], code.depth[i]));
  }
}

inline void put_symbol(Writer& w, const Code& c, std::size_t s) { w.put(c.bits[s], c.depth[s]); }

struct Token {
  std::uint8_t code;
  std::uint8_t extra;
};

void rle_repeats(std::vector<Token>& out, std::uint8_t prev, std::uint8_t value, std::size_t reps) {
  if (prev != value) {
    out.push_back({value, 0});
    --reps;
  }
  if (reps == 7) {
    out.push_back({value, 0});
    --reps;
  }
  if (reps < 3) {
    for (std::size_t i = 0; i < reps; ++i) out.push_back({value, 0});
    return;
  }
  reps -= 3;
  const std::size_t start = out.size();
  while (true) {
    out.push_back({16, static_cast<std::uint8_t>(reps & 3u)});
    reps >>= 2;
    if (reps == 0) break;
    --reps;
  }
  std::reverse(out.begin() + static_cast<std::ptrdiff_t>(start), out.end());
}

void rle_zeros(std::vector<Token>& out, std::size_t reps) {
  if (reps == 11) {
    out.push_back({0, 0});
    --reps;
  }
  if (reps < 3) {
    for (std::size_t i = 0; i < reps; ++i) out.push_back({0, 0});
    return;
  }
  reps -= 3;
  const std::size_t start = out.size();
  while (true) {
    out.push_back({17, static_cast<std::uint8_t>(reps & 7u)});
    reps >>= 3;
    if (reps == 0) break;
    --reps;
  }
  std::reverse(out.begin() + static_cast<std::ptrdiff_t>(start), out.end());
}

// Static code for the code length code lengths, by value: (bits, length).
constexpr std::array<std::uint8_t, 6> cl_static_bits = {0, 7, 3, 2, 1, 15};
constexpr std::array<std::uint8_t, 6> cl_static_len = {2, 4, 3, 2, 2, 4};

void store_code(Writer& w, const Code& code, std::size_t alphabet) {
  const std::size_t n = code.depth.size();
  std::array<std::uint16_t, 4> sym{};
  std::size_t used = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (code.depth[i] != 0) {
      if (used < 4) sym[used] = static_cast<std::uint16_t>(i);
      ++used;
    }
  }
  const int sym_bits = std::bit_width(alphabet - 1);
  if (used <= 4) {
    std::sort(sym.begin(), sym.begin() + static_cast<std::ptrdiff_t>(used), [&](std::uint16_t a, std::uint16_t b) {
      return code.depth[a] != code.depth[b] ? code.depth[a] < code.depth[b] : a < b;
    });
    w.put(1, 2);
    w.put(used - 1, 2);
    for (std::size_t i = 0; i < used; ++i) w.put(sym[i], sym_bits);
    if (used == 4) w.put(code.depth[sym[0]] == 1 ? 1u : 0u, 1);
    return;
  }
  std::size_t last = n;
  while (last > 0 && code.depth[last - 1] == 0) --last;
  std::vector<Token> tokens;
  tokens.reserve(last);
  std::uint8_t prev = 8;
  for (std::size_t i = 0; i < last;) {
    const std::uint8_t v = code.depth[i];
    std::size_t reps = 1;
    while (i + reps < last && code.depth[i + reps] == v) ++reps;
    if (v == 0) {
      rle_zeros(tokens, reps);
    } else {
      rle_repeats(tokens, prev, v, reps);
      prev = v;
    }
    i += reps;
  }
  Histogram clh(18, 0);
  for (const Token& t : tokens) ++clh[t.code];
  Code cl;
  build_code(clh, cl, 5);
  std::size_t cl_used = 0;
  for (const std::uint32_t c : clh) cl_used += c != 0 ? 1u : 0u;
  std::array<std::uint8_t, 18> cl_depth{};
  for (std::size_t i = 0; i < 18; ++i) cl_depth[i] = cl.depth[i];
  if (cl_used == 1) {
    for (std::size_t i = 0; i < 18; ++i) {
      if (clh[i] != 0) cl_depth[i] = 1;  // the decoder then reads it with zero bits
    }
  }
  std::size_t skip = 0;
  if (cl_depth[br::code_length_order[0]] == 0 && cl_depth[br::code_length_order[1]] == 0) {
    skip = cl_depth[br::code_length_order[2]] == 0 ? 3 : 2;
  }
  std::size_t cl_last = 18;
  if (cl_used != 1) {
    while (cl_last > 0 && cl_depth[br::code_length_order[cl_last - 1]] == 0) --cl_last;
  }
  w.put(skip, 2);
  for (std::size_t i = skip; i < cl_last; ++i) {
    const std::uint8_t d = cl_depth[br::code_length_order[i]];
    w.put(cl_static_bits[d], cl_static_len[d]);
  }
  for (const Token& t : tokens) {
    if (cl_used != 1) put_symbol(w, cl, t.code);
    if (t.code == 16) w.put(t.extra, 2);
    if (t.code == 17) w.put(t.extra, 3);
  }
}

/// Build and store a code; a histogram with one (or no) symbol becomes a zero-bit simple code.
void build_and_store(Writer& w, const Histogram& hist, std::size_t alphabet, Code& code) {
  build_code(hist, code);
  std::size_t used = 0;
  std::size_t only = 0;
  for (std::size_t i = 0; i < hist.size(); ++i) {
    if (hist[i] != 0) {
      ++used;
      only = i;
    }
  }
  if (used <= 1) {
    w.put(1, 2);
    w.put(0, 2);
    w.put(only, std::bit_width(alphabet - 1));
    return;
  }
  store_code(w, code, alphabet);
}

// ---------------------------------------------------------------------------- cost model

// Estimated cost in 1/256 bits of coding a histogram with its own code, including the code.
std::uint64_t population_cost(const Histogram& h) {
  std::uint64_t total = 0;
  std::uint32_t used = 0;
  for (const std::uint32_t c : h) {
    total += c;
    used += c != 0 ? 1u : 0u;
  }
  if (used <= 1) return 12 * 256;
  const std::uint64_t lt = log2_q8(total);
  std::uint64_t bits = 0;
  for (const std::uint32_t c : h) {
    if (c != 0) bits += c * (lt - log2_q8(c));
  }
  const std::uint64_t tree = used <= 4 ? (6 + used * 9) * 256u : (40 + 4 * std::uint64_t{used}) * 256u;
  return bits + tree;
}

std::uint64_t merged_cost(const Histogram& a, const Histogram& b) {
  Histogram m(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) m[i] = a[i] + b[i];
  return population_cost(m);
}

/// Greedy agglomerative clustering: merges histograms while that lowers the
/// estimated cost or while there are more than `max_clusters`. Returns the
/// cluster index of every input histogram (empty ones join cluster 0) and
/// the cluster histograms, numbered in order of first use.
std::vector<std::uint32_t> cluster(const std::vector<Histogram>& in, std::size_t max_clusters,
                                   std::vector<Histogram>& out) {
  const std::size_t n = in.size();
  std::vector<std::uint32_t> assign(n, 0);
  std::vector<Histogram> hist;
  std::vector<std::uint64_t> cost;
  std::vector<std::vector<std::uint32_t>> members;
  for (std::size_t i = 0; i < n; ++i) {
    bool empty = true;
    for (const std::uint32_t c : in[i]) {
      if (c != 0) {
        empty = false;
        break;
      }
    }
    if (empty) continue;
    hist.push_back(in[i]);
    cost.push_back(population_cost(in[i]));
    members.push_back({static_cast<std::uint32_t>(i)});
  }
  const std::size_t m = hist.size();
  if (m == 0) {
    out.assign(1, Histogram(in.empty() ? 1 : in[0].size(), 0));
    return assign;
  }
  std::vector<bool> alive(m, true);
  // Pairwise merge deltas (cost(a+b) - cost(a) - cost(b)), as signed values.
  std::vector<std::int64_t> delta(m * m, 0);
  const auto pair_delta = [&](std::size_t a, std::size_t b) {
    return static_cast<std::int64_t>(merged_cost(hist[a], hist[b])) - static_cast<std::int64_t>(cost[a]) -
           static_cast<std::int64_t>(cost[b]);
  };
  for (std::size_t a = 0; a < m; ++a) {
    for (std::size_t b = a + 1; b < m; ++b) delta[a * m + b] = pair_delta(a, b);
  }
  std::size_t count = m;
  while (count > 1) {
    std::int64_t best = 0;
    std::size_t ba = m;
    std::size_t bb = m;
    for (std::size_t a = 0; a < m; ++a) {
      if (!alive[a]) continue;
      for (std::size_t b = a + 1; b < m; ++b) {
        if (!alive[b]) continue;
        const std::int64_t d = delta[a * m + b];
        if (ba == m || d < best) {
          best = d;
          ba = a;
          bb = b;
        }
      }
    }
    if (ba == m || (best >= 0 && count <= max_clusters)) break;
    for (std::size_t i = 0; i < hist[ba].size(); ++i) hist[ba][i] += hist[bb][i];
    cost[ba] = population_cost(hist[ba]);
    members[ba].insert(members[ba].end(), members[bb].begin(), members[bb].end());
    alive[bb] = false;
    --count;
    for (std::size_t o = 0; o < m; ++o) {
      if (!alive[o] || o == ba) continue;
      const std::size_t a = std::min(o, ba);
      const std::size_t b = std::max(o, ba);
      delta[a * m + b] = pair_delta(a, b);
    }
  }
  // Number clusters by first use.
  std::vector<std::int32_t> index_of(m, -1);
  std::vector<std::int32_t> owner(n, -1);
  for (std::size_t c = 0; c < m; ++c) {
    if (!alive[c]) continue;
    for (const std::uint32_t i : members[c]) owner[i] = static_cast<std::int32_t>(c);
  }
  out.clear();
  for (std::size_t i = 0; i < n; ++i) {
    if (owner[i] < 0) continue;
    const auto c = static_cast<std::size_t>(owner[i]);
    if (index_of[c] < 0) {
      index_of[c] = static_cast<std::int32_t>(out.size());
      out.push_back(hist[c]);
    }
    assign[i] = static_cast<std::uint32_t>(index_of[c]);
  }
  return assign;
}

// ---------------------------------------------------------------------------- commands

struct Command {
  std::uint32_t insert = 0;
  std::uint32_t copy = 0;      // copy length code value (dictionary: word length); 0 = insert only
  std::uint32_t produced = 0;  // bytes the copy produces
  std::uint32_t distance = 0;  // backward distance, or the dictionary's virtual distance
  bool dictionary = false;
};

using Ring = std::array<std::uint32_t, 4>;

void push_distance(Ring& ring, std::uint32_t d) {
  ring[3] = ring[2];
  ring[2] = ring[1];
  ring[1] = ring[0];
  ring[0] = d;
}

/// Short distance code for `d` (0..15), or -1.
int short_code(const Ring& ring, std::uint32_t d) {
  if (d == ring[0]) return 0;
  if (d == ring[1]) return 1;
  if (d == ring[2]) return 2;
  if (d == ring[3]) return 3;
  static constexpr std::array<std::int8_t, 12> delta = {-1, 1, -2, 2, -3, 3, -1, 1, -2, 2, -3, 3};
  for (int k = 4; k < 16; ++k) {
    const std::int64_t v = static_cast<std::int64_t>(ring[k < 10 ? 0 : 1]) + delta[static_cast<std::size_t>(k - 4)];
    if (v == static_cast<std::int64_t>(d)) return k;
  }
  return -1;
}

struct Coded {
  std::uint16_t symbol = 0;
  std::uint32_t insert_extra = 0;
  std::uint32_t copy_extra = 0;
  std::uint8_t insert_bits = 0;
  std::uint8_t copy_bits = 0;
  bool has_distance = false;
  std::uint8_t dist_context = 0;
  std::uint16_t dist_symbol = 0;
  std::uint8_t dist_bits = 0;
  std::uint32_t dist_extra = 0;
};

std::uint16_t command_symbol(int ic, int cc, bool implicit_distance) {
  if (implicit_distance && ic < 8 && cc < 16) {
    return static_cast<std::uint16_t>((cc < 8 ? 0 : 64) + (ic << 3) + (cc & 7));
  }
  static constexpr std::array<std::array<std::uint8_t, 3>, 3> cells = {{{2, 3, 6}, {4, 5, 8}, {7, 9, 10}}};
  const int cell = cells[static_cast<std::size_t>(ic >> 3)][static_cast<std::size_t>(cc >> 3)];
  return static_cast<std::uint16_t>(cell * 64 + ((ic & 7) << 3) + (cc & 7));
}

/// Encode a command given the distance ring (which it updates).
Coded code_command(const Command& c, Ring& ring) {
  Coded k;
  const int ic = br::insert_code_of(c.insert);
  k.insert_bits = br::insert_codes[static_cast<std::size_t>(ic)].extra;
  k.insert_extra = c.insert - br::insert_codes[static_cast<std::size_t>(ic)].base;
  if (c.copy == 0) {
    k.symbol = command_symbol(ic, 0, true);
    return k;
  }
  const int cc = br::copy_code_of(c.copy);
  k.copy_bits = br::copy_codes[static_cast<std::size_t>(cc)].extra;
  k.copy_extra = c.copy - br::copy_codes[static_cast<std::size_t>(cc)].base;
  const int sc = c.dictionary ? -1 : short_code(ring, c.distance);
  if (sc == 0) {
    k.symbol = command_symbol(ic, cc, true);
    k.has_distance = k.symbol >= 128;
    k.dist_symbol = 0;
  } else {
    k.symbol = command_symbol(ic, cc, false);
    k.has_distance = true;
    if (sc > 0) {
      k.dist_symbol = static_cast<std::uint16_t>(sc);
    } else {
      const std::uint64_t v = std::uint64_t{c.distance} + 3u;
      const int nb = floor_log2(v) - 1;
      const std::uint64_t hb = (v >> nb) & 1u;
      k.dist_symbol = static_cast<std::uint16_t>(16 + 2 * (nb - 1) + static_cast<int>(hb));
      k.dist_bits = static_cast<std::uint8_t>(nb);
      k.dist_extra = static_cast<std::uint32_t>(v & ((std::uint64_t{1} << nb) - 1u));
    }
    if (!c.dictionary) push_distance(ring, c.distance);
  }
  k.dist_context = static_cast<std::uint8_t>(c.copy > 4 ? 3 : c.copy - 2);
  return k;
}

// ---------------------------------------------------------------------------- block splits

struct BlockSplit {
  std::uint32_t types = 1;
  std::vector<std::uint8_t> type;     // per block
  std::vector<std::uint32_t> length;  // per block
};

/// Block-switch codes: type symbol and count code per block (the first block has no type symbol).
struct SplitCoder {
  const BlockSplit* split = nullptr;
  Code type_code;
  Code count_code;
  std::size_t block = 0;
  std::uint32_t left = 0;

  static std::uint32_t type_symbol(std::uint32_t t, std::uint32_t& last, std::uint32_t& second, std::uint32_t n) {
    std::uint32_t s;
    if (t == second) {
      s = 0;
    } else if (t == (last + 1) % n) {
      s = 1;
    } else {
      s = t + 2;
    }
    second = last;
    last = t;
    return s;
  }

  void put_count(Writer& w, std::uint32_t count) const {
    const int c = br::block_count_code_of(count);
    put_symbol(w, count_code, static_cast<std::size_t>(c));
    w.put(count - br::block_count_codes[static_cast<std::size_t>(c)].base,
          br::block_count_codes[static_cast<std::size_t>(c)].extra);
  }

  void store_header(Writer& w, const BlockSplit& s) {
    split = &s;
    const std::uint32_t n = s.types;
    if (n == 1) {
      w.put(0, 1);
      left = 0xFFFFFFFFu;
      return;
    }
    // NBLTYPES
    w.put(1, 1);
    if (n == 2) {
      w.put(0, 3);
    } else {
      const std::uint32_t x = n - 1;
      const int nb = floor_log2(x);
      w.put(static_cast<std::uint32_t>(nb), 3);
      w.put(x - (1u << nb), nb);
    }
    Histogram th(n + 2, 0);
    Histogram ch(br::block_count_alphabet, 0);
    std::uint32_t last = 0;
    std::uint32_t second = 1;
    for (std::size_t i = 0; i < s.type.size(); ++i) {
      if (i > 0) ++th[type_symbol(s.type[i], last, second, n)];
      ++ch[static_cast<std::size_t>(br::block_count_code_of(s.length[i]))];
    }
    build_and_store(w, th, n + 2, type_code);
    build_and_store(w, ch, br::block_count_alphabet, count_code);
    put_count(w, s.length[0]);
    block = 0;
    left = s.length[0];
    last_ = 0;
    second_ = 1;
  }

  /// Before each symbol of this category: emit a block switch when the current block is used up.
  /// Returns the current block type.
  std::uint32_t step(Writer& w) {
    if (left == 0) {
      ++block;
      const std::uint32_t t = split->type[block];
      put_symbol(w, type_code, type_symbol(t, last_, second_, split->types));
      left = split->length[block];
      put_count(w, left);
    }
    --left;
    return split == nullptr || split->types == 1 ? 0u : split->type[block];
  }

  std::uint32_t last_ = 0;
  std::uint32_t second_ = 1;
};

void store_count_1_256(Writer& w, std::uint32_t n) {
  if (n == 1) {
    w.put(0, 1);
    return;
  }
  w.put(1, 1);
  if (n == 2) {
    w.put(0, 3);
    return;
  }
  const std::uint32_t x = n - 1;
  const int nb = floor_log2(x);
  w.put(static_cast<std::uint32_t>(nb), 3);
  w.put(x - (1u << nb), nb);
}

void store_context_map(Writer& w, const std::vector<std::uint32_t>& map, std::uint32_t trees) {
  store_count_1_256(w, trees);
  if (trees == 1) return;
  // Move-to-front, then run-length code the zeros.
  std::vector<std::uint32_t> mtf_values(map.size());
  std::array<std::uint8_t, 256> mtf{};
  for (std::size_t k = 0; k < 256; ++k) mtf[k] = static_cast<std::uint8_t>(k);
  for (std::size_t i = 0; i < map.size(); ++i) {
    const auto v = static_cast<std::uint8_t>(map[i]);
    std::size_t index = 0;
    while (mtf[index] != v) ++index;
    mtf_values[i] = static_cast<std::uint32_t>(index);
    for (std::size_t k = index; k > 0; --k) mtf[k] = mtf[k - 1];
    mtf[0] = v;
  }
  std::size_t longest = 0;
  for (std::size_t i = 0; i < mtf_values.size();) {
    std::size_t run = 0;
    while (i + run < mtf_values.size() && mtf_values[i + run] == 0) ++run;
    longest = std::max(longest, run);
    i += run == 0 ? 1 : run;
  }
  const std::uint32_t rlemax = longest >= 2 ? static_cast<std::uint32_t>(std::min(16, floor_log2(longest))) : 0u;
  struct Sym {
    std::uint32_t symbol;
    std::uint32_t extra;
  };
  std::vector<Sym> syms;
  for (std::size_t i = 0; i < mtf_values.size();) {
    if (mtf_values[i] != 0) {
      syms.push_back({mtf_values[i] + rlemax, 0});
      ++i;
      continue;
    }
    std::size_t run = 0;
    while (i + run < mtf_values.size() && mtf_values[i + run] == 0) ++run;
    i += run;
    while (run > 0) {
      if (run == 1 || rlemax == 0) {
        syms.push_back({0, 0});
        --run;
        continue;
      }
      const std::uint32_t k = std::min<std::uint32_t>(rlemax, static_cast<std::uint32_t>(floor_log2(run)));
      const std::size_t take = std::min<std::size_t>(run, (std::size_t{2} << k) - 1);
      syms.push_back({k, static_cast<std::uint32_t>(take - (std::size_t{1} << k))});
      run -= take;
    }
  }
  if (rlemax > 0) {
    w.put(1, 1);
    w.put(rlemax - 1, 4);
  } else {
    w.put(0, 1);
  }
  Histogram h(trees + rlemax, 0);
  for (const Sym& s : syms) ++h[s.symbol];
  Code code;
  build_and_store(w, h, trees + rlemax, code);
  for (const Sym& s : syms) {
    put_symbol(w, code, s.symbol);
    if (s.symbol > 0 && s.symbol <= rlemax) w.put(s.extra, static_cast<int>(s.symbol));
  }
  w.put(1, 1);  // IMTF
}

// ---------------------------------------------------------------------------- matching

inline std::uint32_t load32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::size_t match_length(const std::uint8_t* a, const std::uint8_t* b, std::size_t limit) {
  std::size_t n = 0;
  while (n + 8 <= limit) {
    std::uint64_t x;
    std::uint64_t y;
    std::memcpy(&x, a + n, 8);
    std::memcpy(&y, b + n, 8);
    if (x != y) {
      if constexpr (std::endian::native == std::endian::little) {
        return n + static_cast<std::size_t>(std::countr_zero(x ^ y) / 8);
      } else {
        while (a[n] == b[n]) ++n;
        return n;
      }
    }
    n += 8;
  }
  while (n < limit && a[n] == b[n]) ++n;
  return n;
}

constexpr std::uint32_t no_pos = 0xFFFFFFFFu;
constexpr std::int64_t score_base = 1920;
constexpr std::int64_t literal_score = 135;

inline std::int64_t explicit_score(std::size_t len, std::uint64_t dist) {
  return score_base + literal_score * static_cast<std::int64_t>(len) - 30 * floor_log2(dist);
}
inline std::int64_t cache_score(std::size_t len, int code) {
  if (code == 0) return score_base + literal_score * static_cast<std::int64_t>(len) + 15;
  return score_base + literal_score * static_cast<std::int64_t>(len) - (39 + ((0x1CA10 >> (code & 0xE)) & 0xE));
}

struct Match {
  std::uint32_t len = 0;
  std::uint32_t distance = 0;
  std::uint32_t produced = 0;
  bool dictionary = false;
  std::int64_t score = 0;
};

// Static dictionary lookup (level 9): words indexed by a hash of their first four bytes.
class DictionaryIndex {
 public:
  static constexpr int bits = 15;
  DictionaryIndex() : head_(std::size_t{1} << bits, no_pos) {
    const auto dict = br::dictionary();
    for (int len = br::min_dictionary_word; len <= br::max_dictionary_word; ++len) {
      const std::uint32_t words = 1u << br::ndbits[static_cast<std::size_t>(len)];
      for (std::uint32_t i = 0; i < words; ++i) {
        const std::uint8_t* w = dict.data() + br::dictionary_offset[static_cast<std::size_t>(len)] + std::size_t{i} * static_cast<std::size_t>(len);
        const std::uint32_t h = hash(w);
        entries_.push_back({static_cast<std::uint8_t>(len), i, head_[h]});
        head_[h] = static_cast<std::uint32_t>(entries_.size() - 1);
      }
    }
    build_groups();
  }
  static std::uint32_t hash(const std::uint8_t* p) { return (load32(p) * 0x1E35A7BDu) >> (32 - bits); }

  struct Entry {
    std::uint8_t len;
    std::uint32_t index;
    std::uint32_t next;
  };
  [[nodiscard]] std::uint32_t head(std::uint32_t h) const { return head_[h]; }
  [[nodiscard]] const Entry& entry(std::uint32_t i) const { return entries_[i]; }

  /// Transforms that keep the whole word (as is, or with an upper-case first
  /// letter), grouped by prefix: one lookup per group serves all its suffixes.
  struct Group {
    std::string_view prefix;
    bool upper_first;
    std::vector<std::pair<std::uint8_t, std::string_view>> suffixes;  // transform id, suffix
  };
  [[nodiscard]] const std::vector<Group>& groups() const { return groups_; }

 private:
  void build_groups() {
    for (int id = 0; id < br::transform_count; ++id) {
      const br::Transform& t = br::transforms[static_cast<std::size_t>(id)];
      if (t.type != br::Identity && t.type != br::UppercaseFirst) continue;
      const std::string_view prefix(t.prefix, t.prefix_size);
      const bool upper = t.type == br::UppercaseFirst;
      Group* g = nullptr;
      for (Group& candidate : groups_) {
        if (candidate.prefix == prefix && candidate.upper_first == upper) g = &candidate;
      }
      if (g == nullptr) {
        groups_.push_back(Group{prefix, upper, {}});
        g = &groups_.back();
      }
      g->suffixes.emplace_back(static_cast<std::uint8_t>(id), std::string_view(t.suffix, t.suffix_size));
    }
  }

  std::vector<std::uint32_t> head_;
  std::vector<Entry> entries_;
  std::vector<Group> groups_;
};

class Matcher {
 public:
  Matcher(ByteView data, const LevelParams& p, int wbits, const DictionaryIndex* dict)
      : data_(reinterpret_cast<const std::uint8_t*>(data.data())),
        size_(data.size()),
        p_(p),
        max_distance_((std::uint64_t{1} << wbits) - 16),
        dict_(dict) {
    // Buckets of the most recent positions per hash value: at most about
    // four slots per input byte and 4M slots (16 MiB) in all.
    bucket_bits_ = std::bit_width(static_cast<std::uint32_t>(p.depth)) - 1;
    const int fit = std::min(std::bit_width(std::uint64_t{size_}) + 1, max_slot_bits) - bucket_bits_;
    hash_bits_ = std::clamp(fit, 8, p.hash_bits);
    count_.assign(std::size_t{1} << hash_bits_, 0);
    bucket_.assign(std::size_t{1} << (hash_bits_ + bucket_bits_), 0);
  }

  void insert(std::size_t pos) {
    if (pos + 4 > size_) return;
    const std::uint32_t h = hash(pos);
    const std::uint32_t n = count_[h]++;
    bucket_[(std::size_t{h} << bucket_bits_) + (n & ((1u << bucket_bits_) - 1u))] = static_cast<std::uint32_t>(pos);
  }

  /// Best match at `pos` (len 0 when nothing pays).
  Match find(std::size_t pos, const Ring& ring) const {
    Match best;
    best.score = score_base + 100;  // minimum score that pays
    if (pos + 4 > size_) return best;
    const std::size_t limit = std::min<std::size_t>(size_ - pos, max_copy);
    const std::uint8_t* here = data_ + pos;
    const std::uint64_t reach = std::min<std::uint64_t>(max_distance_, pos);
    // Distance cache.
    for (int k = 0; k < 4; ++k) {
      const std::uint32_t d = ring[static_cast<std::size_t>(k)];
      if (d == 0 || d > reach) continue;
      if (k > 0 && d == ring[0]) continue;
      const std::uint8_t* there = here - d;
      if (there[0] != here[0] || there[1] != here[1]) continue;
      const std::size_t len = match_length(here, there, limit);
      if (len < 3 && !(k == 0 && len == 2)) continue;
      const std::int64_t s = cache_score(len, k);
      if (s > best.score) {
        best.score = s;
        best.len = static_cast<std::uint32_t>(len);
        best.distance = d;
      }
    }
    // Hash bucket, newest first.
    const std::uint32_t h = hash(pos);
    const std::uint32_t count = count_[h];
    const std::uint32_t mask = (1u << bucket_bits_) - 1u;
    const std::uint32_t* bucket = bucket_.data() + (std::size_t{h} << bucket_bits_);
    const std::uint32_t tries = std::min(count, mask + 1u);
    std::size_t best_len = best.len;
    const std::uint32_t here4 = load32(here);
    for (std::uint32_t i = 1; i <= tries; ++i) {
      const std::uint32_t cand = bucket[(count - i) & mask];
      const std::size_t dist = pos - cand;
      if (dist == 0 || dist > reach) break;
      const std::uint8_t* there = data_ + cand;
      if (best_len < limit && there[best_len] == here[best_len] && load32(there) == here4) {
        const std::size_t len = match_length(here, there, limit);
        if (len >= 4) {
          const std::int64_t s = explicit_score(len, dist);
          if (s > best.score) {
            best.score = s;
            best.len = static_cast<std::uint32_t>(len);
            best.distance = static_cast<std::uint32_t>(dist);
            best_len = len;
            if (len >= static_cast<std::size_t>(p_.nice)) break;
          }
        }
      }
    }
    if (dict_ != nullptr && best.len < 24) find_dictionary(pos, reach, best);
    best.produced = best.len;
    return best;
  }

 private:
  void find_dictionary(std::size_t pos, std::uint64_t reach, Match& best) const {
    for_each_dictionary_match(pos, reach, data_ + size_, [&](std::uint32_t n, std::uint32_t distance, std::uint32_t len) {
      if (n <= best.len) return;
      // Dictionary distances are long; charge them like explicit distances plus a little.
      const std::int64_t s = explicit_score(n, distance) - 40;
      if (s > best.score) {
        best.score = s;
        best.len = n;
        best.distance = distance;
        best.dictionary = true;
        best.produced = n;
        dict_len_ = len;
      }
    });
  }

 public:
  /// Calls f(produced, virtual distance, word length) for every dictionary
  /// word (under the transforms tried) that the input at `pos` starts with,
  /// not reaching past `end`.
  template <class F>
  void for_each_dictionary_match(std::size_t pos, std::uint64_t reach, const std::uint8_t* end, F&& f) const {
    if (dict_ == nullptr) return;
    const std::uint8_t* here = data_ + pos;
    const std::size_t avail = static_cast<std::size_t>(end - here);
    const auto dict = br::dictionary();
    for (const DictionaryIndex::Group& g : dict_->groups()) {
      const std::size_t pre = g.prefix.size();
      if (avail < pre + 4 || std::memcmp(here, g.prefix.data(), pre) != 0) continue;
      const std::uint8_t* at = here + pre;
      std::array<std::uint8_t, 4> key{};
      std::memcpy(key.data(), at, 4);
      if (g.upper_first) {
        if (key[0] < 'A' || key[0] > 'Z') continue;
        key[0] ^= 32u;
      }
      for (std::uint32_t e = dict_->head(DictionaryIndex::hash(key.data())); e != no_pos; e = dict_->entry(e).next) {
        const DictionaryIndex::Entry& entry = dict_->entry(e);
        const std::size_t len = entry.len;
        if (pre + len > avail) continue;
        const std::uint8_t* word = dict.data() + br::dictionary_offset[len] + std::size_t{entry.index} * len;
        if (word[0] != key[0] || std::memcmp(word + 1, at + 1, len - 1) != 0) continue;
        if (g.upper_first && (word[0] < 'a' || word[0] > 'z')) continue;
        for (const auto& [id, suffix] : g.suffixes) {
          const std::size_t n = pre + len + suffix.size();
          if (n > avail || std::memcmp(at + len, suffix.data(), suffix.size()) != 0) continue;
          const std::uint64_t word_id = (std::uint64_t{id} << br::ndbits[len]) + entry.index;
          f(static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(reach + 1 + word_id), static_cast<std::uint32_t>(len));
        }
      }
    }
  }

  struct Candidate {
    std::uint32_t len;
    std::uint32_t distance;
  };
  /// Matches at `pos` from the hash bucket, nearest first, each longer than
  /// the previous one (at most `limit` bytes).
  void collect(std::size_t pos, std::size_t limit, std::vector<Candidate>& out) const {
    if (pos + 4 > size_ || limit < 4) return;
    const std::uint8_t* here = data_ + pos;
    const std::uint64_t reach = std::min<std::uint64_t>(max_distance_, pos);
    const std::uint32_t h = hash(pos);
    const std::uint32_t count = count_[h];
    const std::uint32_t mask = (1u << bucket_bits_) - 1u;
    const std::uint32_t* bucket = bucket_.data() + (std::size_t{h} << bucket_bits_);
    const std::uint32_t tries = std::min(count, mask + 1u);
    std::size_t best_len = 3;
    const std::uint32_t here4 = load32(here);
    for (std::uint32_t i = 1; i <= tries; ++i) {
      const std::uint32_t cand = bucket[(count - i) & mask];
      const std::size_t dist = pos - cand;
      if (dist == 0 || dist > reach) break;
      const std::uint8_t* there = data_ + cand;
      if (there[best_len] != here[best_len] || load32(there) != here4) continue;
      const std::size_t len = match_length(here, there, limit);
      if (len > best_len) {
        out.push_back({static_cast<std::uint32_t>(len), static_cast<std::uint32_t>(dist)});
        best_len = len;
        if (len >= limit || len >= static_cast<std::size_t>(p_.nice)) break;
      }
    }
  }
  [[nodiscard]] std::uint64_t max_distance() const { return max_distance_; }

 public:
  /// Word length of the last dictionary match found.
  [[nodiscard]] std::uint32_t dictionary_word_length() const { return dict_len_; }

 private:
  std::uint32_t hash(std::size_t pos) const { return (load32(data_ + pos) * 0x1E35A7BDu) >> (32 - hash_bits_); }

  static constexpr int max_slot_bits = 22;

  const std::uint8_t* data_;
  std::size_t size_;
  LevelParams p_;
  std::uint64_t max_distance_;
  int hash_bits_ = 8;
  int bucket_bits_ = 0;
  std::vector<std::uint32_t> count_;
  std::vector<std::uint32_t> bucket_;
  const DictionaryIndex* dict_;
  mutable std::uint32_t dict_len_ = 0;
};

// ---------------------------------------------------------------------------- optimal parsing

inline int fast_insert_code(std::uint32_t n) {
  if (n < 6) return static_cast<int>(n);
  if (n < 130) {
    const int nbits = floor_log2(n - 2) - 1;
    return (nbits << 1) + static_cast<int>((n - 2) >> nbits) + 2;
  }
  if (n < 2114) return floor_log2(n - 66) + 10;
  if (n < 6210) return 21;
  if (n < 22594) return 22;
  return 23;
}

inline int fast_copy_code(std::uint32_t n) {
  if (n < 10) return static_cast<int>(n) - 2;
  if (n < 134) {
    const int nbits = floor_log2(n - 6) - 1;
    return (nbits << 1) + static_cast<int>((n - 6) >> nbits) + 4;
  }
  if (n < 2118) return floor_log2(n - 70) + 12;
  return 23;
}

/// Symbol costs (1/256 bit) used by the optimal parser.
struct CostModel {
  std::array<std::uint32_t, 256> literal{};
  std::array<std::uint32_t, br::command_alphabet> command{};
  std::array<std::uint32_t, 64> distance{};

  template <std::size_t N>
  static void from_counts(const std::array<std::uint32_t, N>& counts, std::array<std::uint32_t, N>& cost,
                          std::uint32_t unseen_penalty) {
    std::uint64_t total = 0;
    for (const std::uint32_t c : counts) total += c;
    const std::uint32_t lt = log2_q8(total + 1);
    for (std::size_t i = 0; i < N; ++i) {
      cost[i] = counts[i] == 0 ? lt + unseen_penalty : std::max<std::uint32_t>(lt - log2_q8(counts[i]), 16);
    }
  }

  /// Cost of the distance part of a command.
  [[nodiscard]] std::uint32_t distance_cost(int short_code, std::uint32_t dist) const {
    if (short_code >= 0) return distance[static_cast<std::size_t>(short_code)];
    const std::uint64_t v = std::uint64_t{dist} + 3u;
    const int nb = floor_log2(v) - 1;
    return distance[static_cast<std::size_t>(16 + 2 * (nb - 1) + static_cast<int>((v >> nb) & 1u))] +
           static_cast<std::uint32_t>(nb) * 256u;
  }
  /// Cost of a command with insert code `ic` (its extra bits not included) and copy length `copy`.
  [[nodiscard]] std::uint32_t command_cost(int ic, std::uint32_t copy, int short_code, std::uint32_t dist_cost) const {
    const int cc = fast_copy_code(copy);
    const std::uint32_t extra = std::uint32_t{br::copy_codes[static_cast<std::size_t>(cc)].extra} * 256u;
    if (short_code == 0 && ic < 8 && cc < 16) return command[command_symbol(ic, cc, true)] + extra;
    return command[command_symbol(ic, cc, false)] + extra + dist_cost;
  }
};

class OptimalParser {
 public:
  OptimalParser(ByteView data, const LevelParams& p, int wbits, const DictionaryIndex* dict)
      : data_(reinterpret_cast<const std::uint8_t*>(data.data())), size_(data.size()), m_(data, p, wbits, dict) {}

  Result<std::vector<Command>> run(const Progress& progress) {
    std::vector<Command> cmds;
    initial_model(0, std::min<std::size_t>(size_, segment));
    std::uint32_t pending = 0;  // literals not yet part of a command
    for (std::size_t s = 0; s < size_; s += segment) {
      const std::size_t e = std::min(size_, s + segment);
      gather(s, e);
      std::vector<Command> seg;
      std::uint32_t seg_pending = 0;
      // Two passes on the first segment (the second with the statistics of
      // the first); later segments start from the previous segment's model.
      for (int pass = 0; pass < (s == 0 ? 2 : 1); ++pass) {
        seg.clear();
        Ring r = ring_;
        seg_pending = solve(s, e, pending, r, seg);
        update_model(s, seg, pending);
      }
      // Commit the segment: the ring follows the chosen commands.
      for (const Command& c : seg) {
        if (!c.dictionary && c.distance != ring_[0]) push_distance(ring_, c.distance);
        cmds.push_back(c);
      }
      pending = seg_pending;
      if (!progress.report(0.9 * static_cast<double>(e) / static_cast<double>(size_))) {
        return fail(Status::Cancelled, "the conversion was cancelled");
      }
    }
    if (pending != 0 || cmds.empty()) {
      Command c;
      c.insert = pending;
      cmds.push_back(c);
    }
    return cmds;
  }

 private:
  static constexpr std::size_t segment = 1u << 17;
  static constexpr std::uint32_t long_match = 325;
  /// Matches are tried at every length up to this one, longer ones only at their full length.
  static constexpr std::uint32_t exhaustive = 16;
  static constexpr std::uint64_t infinite = ~std::uint64_t{0};

  struct Node {
    std::uint64_t cost = infinite;
    std::uint32_t step = 0;      // bytes of the step that ends here (0 = literal)
    std::uint32_t copy = 0;      // copy length code value (dictionary: word length)
    std::uint32_t distance = 0;
    std::uint32_t insert = 0;    // literals since the last copy on the best path
    bool dictionary = false;
    Ring ring{};
  };
  struct DictCandidate {
    std::uint32_t produced;
    std::uint32_t distance;
    std::uint32_t len;
  };

  void initial_model(std::size_t s, std::size_t e) {
    std::array<std::uint32_t, 256> lit{};
    for (std::size_t i = s; i < e; ++i) ++lit[data_[i]];
    CostModel::from_counts(lit, model_.literal, 4 * 256);
    for (std::uint32_t& c : model_.command) c = 9 * 256;
    for (std::size_t i = 0; i < 64; ++i) model_.distance[i] = i < 4 ? 3 * 256 : (i < 16 ? 6 * 256 : 5 * 256);
    model_.distance[0] = 2 * 256;
  }

  void update_model(std::size_t s, const std::vector<Command>& seg, std::uint32_t pending) {
    std::array<std::uint32_t, 256> lit{};
    std::array<std::uint32_t, br::command_alphabet> cmd{};
    std::array<std::uint32_t, 64> dist{};
    Ring r = ring_;
    std::size_t pos = s - pending;
    for (const Command& c : seg) {
      for (std::uint32_t k = 0; k < c.insert; ++k) ++lit[data_[pos + k]];
      pos += c.insert + c.produced;
      const Coded k = code_command(c, r);
      ++cmd[k.symbol];
      if (k.has_distance) ++dist[k.dist_symbol];
    }
    CostModel::from_counts(lit, model_.literal, 4 * 256);
    CostModel::from_counts(cmd, model_.command, 3 * 256);
    CostModel::from_counts(dist, model_.distance, 3 * 256);
  }

  // Hash all positions of the segment and keep their match candidates.
  void gather(std::size_t s, std::size_t e) {
    const std::size_t n = e - s;
    offsets_.assign(n + 1, 0);
    cands_.clear();
    dict_offsets_.assign(n + 1, 0);
    dict_cands_.clear();
    std::vector<Matcher::Candidate> tmp;
    std::size_t skip_until = s;
    for (std::size_t i = s; i < e; ++i) {
      offsets_[i - s] = static_cast<std::uint32_t>(cands_.size());
      dict_offsets_[i - s] = static_cast<std::uint32_t>(dict_cands_.size());
      // In data that does not match (long runs without candidates), search
      // only every eighth position.
      if (i >= skip_until && (misses_ < 1024 || (misses_ & 7u) == 0)) {
        tmp.clear();
        m_.collect(i, std::min<std::size_t>(e - i, max_copy), tmp);
        cands_.insert(cands_.end(), tmp.begin(), tmp.end());
        if (!tmp.empty() && tmp.back().len >= long_match) skip_until = i + tmp.back().len;
        misses_ = tmp.empty() ? misses_ + 1 : 0;
        const std::uint64_t reach = std::min<std::uint64_t>(m_.max_distance(), i);
        m_.for_each_dictionary_match(i, reach, data_ + e, [&](std::uint32_t produced, std::uint32_t distance, std::uint32_t len) {
          dict_cands_.push_back({produced, distance, len});
        });
      } else if (i >= skip_until) {
        ++misses_;
      }
      m_.insert(i);
    }
    offsets_[n] = static_cast<std::uint32_t>(cands_.size());
    dict_offsets_[n] = static_cast<std::uint32_t>(dict_cands_.size());
  }

  // Shortest path over [s, e); returns the trailing literals.
  std::uint32_t solve(std::size_t s, std::size_t e, std::uint32_t pending, const Ring& ring, std::vector<Command>& out) {
    const std::size_t n = e - s;
    nodes_.assign(n + 1, Node{});
    nodes_[0].cost = 0;
    nodes_[0].insert = pending;
    nodes_[0].ring = ring;
    const auto relax = [&](std::size_t to, std::uint64_t cost, const Node& from, std::uint32_t step, std::uint32_t copy,
                           std::uint32_t distance, bool dictionary) {
      Node& t = nodes_[to];
      if (cost >= t.cost) return;
      t.cost = cost;
      t.step = step;
      t.copy = copy;
      t.distance = distance;
      t.dictionary = dictionary;
      t.insert = step == 0 ? from.insert + 1 : 0;
      t.ring = from.ring;
      if (step != 0 && !dictionary && distance != from.ring[0]) push_distance(t.ring, distance);
    };
    for (std::size_t i = 0; i < n; ++i) {
      const Node& node = nodes_[i];
      if (node.cost == infinite) continue;
      const std::size_t pos = s + i;
      relax(i + 1, node.cost + model_.literal[data_[pos]], node, 0, 0, 0, false);
      const std::size_t limit = std::min<std::size_t>(e - pos, max_copy);
      const std::uint64_t reach = std::min<std::uint64_t>(m_.max_distance(), pos);
      const std::uint8_t* here = data_ + pos;
      const int ic = fast_insert_code(node.insert);
      const std::uint64_t base = node.cost + std::uint64_t{br::insert_codes[static_cast<std::size_t>(ic)].extra} * 256u;
      std::uint32_t longest = 0;
      // Distance cache.
      for (int k = 0; k < 4 && limit >= 2; ++k) {
        const std::uint32_t d = node.ring[static_cast<std::size_t>(k)];
        if (d == 0 || d > reach || (k > 0 && d == node.ring[0])) continue;
        const std::uint8_t* there = here - d;
        if (there[0] != here[0] || there[1] != here[1]) continue;
        const auto len = static_cast<std::uint32_t>(match_length(here, there, limit));
        const std::uint32_t dc = model_.distance_cost(k, d);
        for (std::uint32_t l = 2; l <= std::min(len, exhaustive); ++l) {
          relax(i + l, base + model_.command_cost(ic, l, k, dc), node, l, l, d, false);
        }
        if (len > exhaustive) relax(i + len, base + model_.command_cost(ic, len, k, dc), node, len, len, d, false);
        longest = std::max(longest, len);
      }
      // Hash candidates.
      std::uint32_t prev = 3;
      for (std::uint32_t c = offsets_[i]; c < offsets_[i + 1]; ++c) {
        const Matcher::Candidate& cand = cands_[c];
        const int sc = short_code(node.ring, cand.distance);
        const std::uint32_t dc = model_.distance_cost(sc, cand.distance);
        for (std::uint32_t l = prev + 1; l <= std::min(cand.len, exhaustive); ++l) {
          relax(i + l, base + model_.command_cost(ic, l, sc, dc), node, l, l, cand.distance, false);
        }
        if (cand.len > exhaustive) {
          relax(i + cand.len, base + model_.command_cost(ic, cand.len, sc, dc), node, cand.len, cand.len, cand.distance, false);
        }
        prev = cand.len;
        longest = std::max(longest, cand.len);
      }
      for (std::uint32_t c = dict_offsets_[i]; c < dict_offsets_[i + 1]; ++c) {
        const DictCandidate& dc = dict_cands_[c];
        relax(i + dc.produced, base + model_.command_cost(ic, dc.len, -1, model_.distance_cost(-1, dc.distance)), node,
              dc.produced, dc.len, dc.distance, true);
      }
      // A very long match is taken as it is: the positions it covers are not evaluated.
      if (longest >= long_match) i += longest - 1;
    }
    // Walk back.
    std::vector<std::size_t> steps;
    for (std::size_t j = n; j > 0;) {
      const Node& nd = nodes_[j];
      steps.push_back(j);
      j -= nd.step == 0 ? 1 : nd.step;
    }
    std::uint32_t lits = pending;
    for (std::size_t k = steps.size(); k-- > 0;) {
      const Node& nd = nodes_[steps[k]];
      if (nd.step == 0) {
        ++lits;
        continue;
      }
      Command c;
      c.insert = lits;
      c.copy = nd.copy;
      c.produced = nd.step;
      c.distance = nd.distance;
      c.dictionary = nd.dictionary;
      out.push_back(c);
      lits = 0;
    }
    return lits;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  Matcher m_;
  CostModel model_;
  Ring ring_ = {4, 11, 15, 16};
  std::vector<std::uint32_t> offsets_;
  std::vector<Matcher::Candidate> cands_;
  std::vector<std::uint32_t> dict_offsets_;
  std::vector<DictCandidate> dict_cands_;
  std::vector<Node> nodes_;
  std::uint32_t misses_ = 0;  // consecutive positions without match candidates
};

Result<std::vector<Command>> parse(ByteView data, const LevelParams& p, int wbits, const Progress& progress) {
  std::unique_ptr<DictionaryIndex> dict;
  if (p.dictionary) dict = std::make_unique<DictionaryIndex>();
  if (p.optimal) {
    OptimalParser parser(data, p, wbits, dict.get());
    return parser.run(progress);
  }
  Matcher m(data, p, wbits, dict.get());
  std::vector<Command> cmds;
  Ring ring = {4, 11, 15, 16};
  const std::size_t n = data.size();
  std::size_t pos = 0;
  std::size_t lit_start = 0;
  std::size_t misses = 0;
  const std::size_t skip_after = p.dictionary ? 512 : (p.lazy == 0 ? 64 : 256);
  std::size_t next_report = 1u << 20;
  const auto record = [&](const Match& mt, std::uint32_t dict_len) {
    Command c;
    c.insert = static_cast<std::uint32_t>(pos - lit_start);
    c.dictionary = mt.dictionary;
    c.distance = mt.distance;
    c.produced = mt.produced;
    c.copy = mt.dictionary ? dict_len : mt.len;
    cmds.push_back(c);
    if (!mt.dictionary && mt.distance != ring[0]) push_distance(ring, mt.distance);
  };
  std::size_t hashed = 0;  // positions below this are in the hash chains
  const auto hash_to = [&](std::size_t end) {
    for (; hashed < end; ++hashed) m.insert(hashed);
  };
  while (pos + 4 <= n) {
    if (pos >= next_report) {
      next_report += 1u << 20;
      if (!progress.report(0.9 * static_cast<double>(pos) / static_cast<double>(n))) {
        return fail(Status::Cancelled, "the conversion was cancelled");
      }
    }
    hash_to(pos);
    Match best = m.find(pos, ring);
    std::uint32_t dict_len = best.dictionary ? m.dictionary_word_length() : 0;
    if (best.len == 0) {
      ++pos;
      // Skip faster through data that does not match.
      if (++misses > skip_after) {
        pos += std::min<std::size_t>((misses - skip_after) >> 5, 8);
        hashed = std::max(hashed, std::min(pos, n) - 1);
      }
      continue;
    }
    misses = 0;
    // Lazy evaluation: prefer a better match starting one byte later.
    for (int step = 0; step < p.lazy && pos + 5 <= n; ++step) {
      hash_to(pos + 1);
      const Match next = m.find(pos + 1, ring);
      if (next.len == 0 || next.score <= best.score + 175) break;
      ++pos;
      best = next;
      dict_len = best.dictionary ? m.dictionary_word_length() : 0;
    }
    record(best, dict_len);
    const std::size_t end = pos + best.produced;
    if (p.depth <= 4 && best.produced > 16) {
      hash_to(pos + 4);
      hashed = end - 8;
    }
    hash_to(std::min(end, n));
    pos = end;
    lit_start = pos;
  }
  if (lit_start < n || cmds.empty()) {
    Command c;
    c.insert = static_cast<std::uint32_t>(n - lit_start);
    cmds.push_back(c);
  }
  return cmds;
}

// ---------------------------------------------------------------------------- meta-blocks

struct MetaWriter {
  ByteView data;
  const std::uint8_t* bytes;
  LevelParams p;
  int level;
  Writer& w;
  Ring ring = {4, 11, 15, 16};

  void store_mlen(std::uint32_t mlen) {
    const std::uint32_t v = mlen - 1;
    const int nibbles = std::max(4, (std::bit_width(v) + 3) / 4);
    w.put(static_cast<std::uint32_t>(nibbles - 4), 2);
    w.put(v, nibbles * 4);
  }

  void store_uncompressed(std::size_t start, std::uint32_t mlen) {
    w.put(0, 1);  // ISLAST
    store_mlen(mlen);
    w.put(1, 1);  // ISUNCOMPRESSED
    w.raw(data.subspan(start, mlen));
  }

  [[nodiscard]] std::uint8_t at(std::size_t i) const { return bytes[i]; }

  int choose_mode(std::size_t start, std::uint32_t mlen) const {
    // UTF-8 / ASCII text -> UTF8 mode, otherwise signed.
    std::size_t high = 0;
    std::size_t bad = 0;
    for (std::size_t i = start; i < start + mlen; ++i) {
      const std::uint8_t b = at(i);
      if (b >= 0x80) ++high;
      if (b < 9 || (b > 13 && b < 32)) ++bad;
    }
    if (bad * 50 > mlen) return br::Signed;
    if (high * 4 > mlen) return br::Signed;
    return br::Utf8;
  }

  /// Greedy literal block splitting: fixed-size chunks are assigned to the
  /// type whose histogram they extend most cheaply, or open a new type.
  BlockSplit split_literals(const std::vector<std::uint8_t>& lits) const {
    BlockSplit s;
    if (p.lit_types <= 1 || lits.size() < 4096) {
      s.type.push_back(0);
      s.length.push_back(static_cast<std::uint32_t>(std::max<std::size_t>(lits.size(), 1)));
      return s;
    }
    const std::size_t chunk = level >= 9 ? 512 : 1024;
    const std::size_t chunks = (lits.size() + chunk - 1) / chunk;
    std::vector<Histogram> types;
    std::vector<std::uint64_t> type_cost;
    std::vector<std::uint8_t> assign(chunks, 0);
    Histogram h(256);
    std::uint8_t current = 0;
    for (std::size_t c = 0; c < chunks; ++c) {
      std::fill(h.begin(), h.end(), 0u);
      const std::size_t from = c * chunk;
      const std::size_t to = std::min(lits.size(), from + chunk);
      for (std::size_t i = from; i < to; ++i) ++h[lits[i]];
      const std::uint64_t alone = population_cost(h);
      std::int64_t best = 0;
      int best_type = -1;
      for (std::size_t t = 0; t < types.size(); ++t) {
        std::int64_t d = static_cast<std::int64_t>(merged_cost(types[t], h)) - static_cast<std::int64_t>(type_cost[t]);
        if (t != current) d += 16 * 256;  // block switch
        if (best_type < 0 || d < best) {
          best = d;
          best_type = static_cast<int>(t);
        }
      }
      if (best_type < 0 ||
          (types.size() < static_cast<std::size_t>(p.lit_types) && static_cast<std::int64_t>(alone) + 64 * 256 < best)) {
        types.push_back(h);
        type_cost.push_back(alone);
        best_type = static_cast<int>(types.size() - 1);
      } else {
        auto& th = types[static_cast<std::size_t>(best_type)];
        for (std::size_t i = 0; i < 256; ++i) th[i] += h[i];
        type_cost[static_cast<std::size_t>(best_type)] = population_cost(th);
      }
      assign[c] = static_cast<std::uint8_t>(best_type);
      current = assign[c];
    }
    // Merge runs into blocks; renumber types by first use (the first block must be type 0).
    std::vector<int> renumber(types.size(), -1);
    std::uint32_t next_type = 0;
    for (std::size_t c = 0; c < chunks; ++c) {
      const std::size_t len = std::min(lits.size(), (c + 1) * chunk) - c * chunk;
      if (renumber[assign[c]] < 0) renumber[assign[c]] = static_cast<int>(next_type++);
      const auto t = static_cast<std::uint8_t>(renumber[assign[c]]);
      if (!s.type.empty() && s.type.back() == t) {
        s.length.back() += static_cast<std::uint32_t>(len);
      } else {
        s.type.push_back(t);
        s.length.push_back(static_cast<std::uint32_t>(len));
      }
    }
    s.types = next_type;
    // Block counts are limited to the block count alphabet's range.
    BlockSplit out;
    out.types = s.types;
    for (std::size_t b = 0; b < s.type.size(); ++b) {
      std::uint32_t len = s.length[b];
      while (len > 0) {
        const std::uint32_t take = std::min<std::uint32_t>(len, 16625u + (1u << 24) - 1u);
        out.type.push_back(s.type[b]);
        out.length.push_back(take);
        len -= take;
      }
    }
    if (out.types == 1) {
      out.type.assign(1, 0);
      out.length.assign(1, static_cast<std::uint32_t>(lits.size()));
    }
    return out;
  }

  /// Write one compressed meta-block for commands [c0, c1) covering bytes [start, start + mlen).
  void store_compressed(std::span<const Command> cmds, std::size_t start, std::uint32_t mlen) {
    // Code the commands (distance codes depend on the ring).
    std::vector<Coded> coded(cmds.size());
    std::size_t total_lits = 0;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
      coded[i] = code_command(cmds[i], ring);
      total_lits += cmds[i].insert;
    }
    // The last command may end the meta-block after its literals.
    // Literals in order, with their contexts.
    std::vector<std::uint8_t> lits;
    lits.reserve(total_lits);
    {
      std::size_t pos = start;
      for (const Command& c : cmds) {
        for (std::uint32_t k = 0; k < c.insert; ++k) lits.push_back(at(pos + k));
        pos += c.insert + c.produced;
      }
    }
    const bool contexts = p.lit_clusters > 1 && total_lits >= 1024;
    const int mode = contexts ? choose_mode(start, mlen) : br::Lsb6;
    const BlockSplit lit_split = split_literals(lits);
    // Literal histograms per (block type, context).
    const std::size_t lit_types = lit_split.types;
    std::vector<Histogram> lit_hist(lit_types * 64, Histogram(256, 0));
    std::vector<std::uint8_t> lit_ctx(total_lits);
    {
      std::size_t pos = start;
      std::size_t li = 0;
      std::size_t block = 0;
      std::uint32_t left = lit_split.length[0];
      for (const Command& c : cmds) {
        for (std::uint32_t k = 0; k < c.insert; ++k) {
          const std::size_t q = pos + k;
          const std::uint8_t p1 = q > 0 ? at(q - 1) : 0;
          const std::uint8_t p2 = q > 1 ? at(q - 2) : 0;
          if (left == 0) {
            ++block;
            left = lit_split.length[block];
          }
          --left;
          const std::uint32_t ctx = contexts ? br::literal_context(mode, p1, p2) : 0u;
          lit_ctx[li] = static_cast<std::uint8_t>(ctx);
          ++lit_hist[std::size_t{lit_split.type[block]} * 64 + ctx][lits[li]];
          ++li;
        }
        pos += c.insert + c.produced;
      }
    }
    // Cluster each block type's contexts, then the resulting clusters of all types.
    const std::size_t max_clusters = contexts ? static_cast<std::size_t>(p.lit_clusters) : 1;
    std::vector<Histogram> stage;
    std::vector<std::uint32_t> lit_map(lit_types * 64, 0);
    for (std::size_t t = 0; t < lit_types; ++t) {
      const std::vector<Histogram> part(lit_hist.begin() + static_cast<std::ptrdiff_t>(t * 64),
                                        lit_hist.begin() + static_cast<std::ptrdiff_t>(t * 64 + 64));
      std::vector<Histogram> local;
      const std::vector<std::uint32_t> local_map = cluster(part, max_clusters, local);
      for (std::size_t c = 0; c < 64; ++c) lit_map[t * 64 + c] = static_cast<std::uint32_t>(stage.size()) + local_map[c];
      stage.insert(stage.end(), local.begin(), local.end());
    }
    std::vector<Histogram> lit_clusters;
    const std::vector<std::uint32_t> stage_map = cluster(stage, max_clusters, lit_clusters);
    for (std::uint32_t& v : lit_map) v = stage_map[v];

    Histogram cmd_hist(br::command_alphabet, 0);
    std::vector<Histogram> dist_hist(4, Histogram(64, 0));
    for (std::size_t i = 0; i < cmds.size(); ++i) {
      ++cmd_hist[coded[i].symbol];
      if (coded[i].has_distance && !(i + 1 == cmds.size() && cmds[i].copy == 0)) {
        ++dist_hist[p.lit_clusters > 1 ? coded[i].dist_context : 0][coded[i].dist_symbol];
      }
    }
    std::vector<Histogram> dist_clusters;
    const std::vector<std::uint32_t> dist_map = cluster(dist_hist, 4, dist_clusters);

    // ---- header
    w.put(0, 1);  // ISLAST
    store_mlen(mlen);
    w.put(0, 1);  // ISUNCOMPRESSED
    SplitCoder lsc;
    lsc.store_header(w, lit_split);
    BlockSplit one;
    one.type.push_back(0);
    one.length.push_back(1);
    SplitCoder isc;
    isc.store_header(w, one);
    SplitCoder dsc;
    dsc.store_header(w, one);
    w.put(0, 2);  // NPOSTFIX
    w.put(0, 4);  // NDIRECT
    for (std::size_t t = 0; t < lit_types; ++t) w.put(static_cast<std::uint32_t>(mode), 2);
    store_context_map(w, lit_map, static_cast<std::uint32_t>(lit_clusters.size()));
    store_context_map(w, dist_map, static_cast<std::uint32_t>(dist_clusters.size()));
    std::vector<Code> lit_codes(lit_clusters.size());
    for (std::size_t i = 0; i < lit_clusters.size(); ++i) build_and_store(w, lit_clusters[i], 256, lit_codes[i]);
    Code cmd_code;
    build_and_store(w, cmd_hist, br::command_alphabet, cmd_code);
    std::vector<Code> dist_codes(dist_clusters.size());
    for (std::size_t i = 0; i < dist_clusters.size(); ++i) build_and_store(w, dist_clusters[i], 64, dist_codes[i]);

    // ---- data
    std::size_t li = 0;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
      const Command& c = cmds[i];
      const Coded& k = coded[i];
      (void)isc.step(w);
      put_symbol(w, cmd_code, k.symbol);
      w.put(k.insert_extra, k.insert_bits);
      w.put(k.copy_extra, k.copy_bits);
      for (std::uint32_t j = 0; j < c.insert; ++j, ++li) {
        const std::uint32_t t = lsc.step(w);
        const std::uint32_t tree = lit_map[t * 64 + lit_ctx[li]];
        put_symbol(w, lit_codes[tree], lits[li]);
      }
      if (c.copy == 0) break;  // insert-only: ends the meta-block
      if (k.has_distance) {
        (void)dsc.step(w);
        const std::uint32_t tree = dist_map[p.lit_clusters > 1 ? k.dist_context : 0];
        put_symbol(w, dist_codes[tree], k.dist_symbol);
        w.put(k.dist_extra, k.dist_bits);
      }
    }
  }

  /// Nearly all literals with a flat byte distribution: not worth an attempt to compress.
  [[nodiscard]] bool incompressible(std::span<const Command> cmds, std::size_t start, std::uint32_t mlen) const {
    std::uint64_t copied = 0;
    for (const Command& c : cmds) copied += c.produced;
    if (copied * 128 >= mlen) return false;
    Histogram h(256, 0);
    std::size_t pos = start;
    std::uint64_t lits = 0;
    for (const Command& c : cmds) {
      for (std::uint32_t k = 0; k < c.insert; ++k) ++h[at(pos + k)];
      lits += c.insert;
      pos += c.insert + c.produced;
    }
    return population_cost(h) >= lits * 8 * 254;  // about 7.94 bits per literal or more
  }

  void store_metablock(std::span<const Command> cmds, std::size_t start, std::uint32_t mlen) {
    if (incompressible(cmds, start, mlen)) {
      store_uncompressed(start, mlen);
      return;
    }
    const Writer::Mark mark = w.mark();
    const Ring saved = ring;
    store_compressed(cmds, start, mlen);
    const std::uint64_t used = w.bits() - (std::uint64_t{mark.size} * 8 + static_cast<std::uint64_t>(mark.count));
    if (used > std::uint64_t{mlen} * 8 + 48) {
      w.restore(mark);
      ring = saved;
      store_uncompressed(start, mlen);
    }
  }
};

int window_bits(std::size_t size, const EncodeOptions& options) {
  int wbits = options.level >= 9 ? 24 : 22;
  if (options.dictionary_size != 0) {
    wbits = std::clamp(std::bit_width(std::uint64_t{options.dictionary_size} + 15) , 10, 24);
  }
  int need = 10;
  while (need < wbits && (std::uint64_t{1} << need) - 16 < size) ++need;
  return std::min(wbits, need);
}

void store_window_bits(Writer& w, int wbits) {
  if (wbits == 16) {
    w.put(0, 1);
  } else if (wbits == 17) {
    w.put(1, 1);
    w.put(0, 3);
    w.put(0, 3);
  } else if (wbits >= 18) {
    w.put(1, 1);
    w.put(static_cast<std::uint32_t>(wbits - 17), 3);
  } else {
    w.put(1, 1);
    w.put(0, 3);
    w.put(static_cast<std::uint32_t>(wbits - 8), 3);
  }
}

}  // namespace

Result<Bytes> brotli_encode(ByteView data, const EncodeOptions& options) {
  if (data.size() > 0xFFFFFFF0u) return fail(Status::InvalidArgument, "input is too large for the Brotli encoder");
  const int level = std::clamp(options.level, 0, 9);
  const LevelParams& p = levels[static_cast<std::size_t>(level)];
  const int wbits = window_bits(data.size(), options);
  Bytes out;
  out.reserve(data.size() / 3 + 64);
  Writer w(out);
  store_window_bits(w, wbits);
  MetaWriter enc{data, reinterpret_cast<const std::uint8_t*>(data.data()), p, level, w};
  if (level == 0) {
    for (std::size_t pos = 0; pos < data.size();) {
      const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(data.size() - pos, p.metablock));
      enc.store_uncompressed(pos, n);
      pos += n;
    }
  } else if (!data.empty()) {
    CVT_TRY_ASSIGN(std::vector<Command> cmds, parse(data, p, wbits, options.progress));
    // Split long insert runs: an insert-only command ends a meta-block.
    std::vector<Command> split;
    split.reserve(cmds.size());
    for (const Command& c : cmds) {
      Command rest = c;
      while (rest.insert > max_insert_run) {
        Command head;
        head.insert = max_insert_run;
        split.push_back(head);
        rest.insert -= max_insert_run;
      }
      if (rest.insert != 0 || rest.copy != 0) split.push_back(rest);
    }
    std::size_t pos = 0;
    std::size_t i = 0;
    while (i < split.size()) {
      std::size_t j = i;
      std::uint64_t mlen = 0;
      while (j < split.size()) {
        const std::uint64_t add = std::uint64_t{split[j].insert} + split[j].produced;
        if (mlen != 0 && mlen + add > p.metablock) break;
        mlen += add;
        ++j;
        if (split[j - 1].copy == 0) break;
      }
      enc.store_metablock(std::span<const Command>(split).subspan(i, j - i), pos, static_cast<std::uint32_t>(mlen));
      pos += static_cast<std::size_t>(mlen);
      i = j;
      if (!options.progress.report(0.9 + 0.1 * static_cast<double>(pos) / static_cast<double>(data.size()))) {
        return fail(Status::Cancelled, "the conversion was cancelled");
      }
    }
  }
  w.put(3, 2);  // ISLAST, ISLASTEMPTY
  w.finish();
  return out;
}

}  // namespace cvt::compression::detail
