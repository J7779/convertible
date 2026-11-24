// Convertible core - archive family: Zstandard encoder.
//
// Levels map to hash-chain search depth, window size and parsing strategy
// (greedy at 0-2, lazy at 3-4, two-step lazy at 5-9, minimum match 3 at 8-9).
// Every block gets Huffman-coded literals (direct or FSE-coded weights) and
// FSE-coded sequences; for each of the three sequence streams the cheapest
// of predefined, RLE or a freshly normalized table is chosen by estimated
// cost (integer arithmetic only, so output is identical on every platform).
// Blocks that do not shrink are stored raw, single-byte blocks as RLE.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include "families/archive/checksums.hpp"
#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/bitio.hpp"
#include "families/archive/codecs/huffman.hpp"
#include "families/archive/codecs/zstd_internal.hpp"

namespace cvt::compression::detail {

namespace {

using namespace zstd;

struct Level {
  int window_log;
  int depth;
  int lazy;      // 0 greedy, 1 lazy, 2 lazy2
  int min_match;
  int target;    // stop searching at this length
};

constexpr std::array<Level, 10> levels = {{
    {18, 1, 0, 4, 16},
    {19, 2, 0, 4, 24},
    {20, 4, 0, 4, 32},
    {21, 8, 1, 4, 48},
    {21, 16, 1, 4, 64},
    {22, 24, 2, 4, 96},
    {22, 48, 2, 4, 128},
    {23, 96, 2, 4, 192},
    {23, 192, 2, 3, 256},
    {24, 512, 2, 3, 999},
}};

// log2(x) in 1/256 bit units, integer only.
std::uint32_t log2_q8(std::uint64_t x) {
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

// ------------------------------------------------------------------------------- FSE encoding

struct FseCTable {
  int log = 0;
  std::vector<std::uint16_t> states;
  struct Sym {
    std::int32_t delta_find = 0;
    std::uint32_t delta_bits = 0;
  };
  std::vector<Sym> sym;

  bool build(const std::int16_t* norm, std::size_t symbols, int table_log) {
    log = table_log;
    std::vector<std::uint16_t> cells;
    if (!spread_symbols(norm, symbols, table_log, cells)) return false;
    const std::uint32_t size = 1u << table_log;
    std::vector<std::uint32_t> cumul(symbols + 1, 0);
    for (std::size_t s = 0; s < symbols; ++s) cumul[s + 1] = cumul[s] + (norm[s] == -1 ? 1u : static_cast<std::uint32_t>(std::max<std::int16_t>(norm[s], 0)));
    std::vector<std::uint32_t> fill(cumul.begin(), cumul.end() - 1);
    states.assign(size, 0);
    for (std::uint32_t u = 0; u < size; ++u) states[fill[cells[u]]++] = static_cast<std::uint16_t>(size + u);
    sym.assign(symbols, Sym{});
    std::uint32_t total = 0;
    for (std::size_t s = 0; s < symbols; ++s) {
      const int count = norm[s] == -1 ? 1 : norm[s];
      if (count == 0) {
        sym[s].delta_bits = (static_cast<std::uint32_t>(table_log + 1) << 16) - size;
        continue;
      }
      if (count == 1) {
        sym[s].delta_bits = (static_cast<std::uint32_t>(table_log) << 16) - size;
        sym[s].delta_find = static_cast<std::int32_t>(total) - 1;
        ++total;
        continue;
      }
      const int max_bits_out = table_log - (std::bit_width(static_cast<std::uint32_t>(count - 1)) - 1);
      const std::uint32_t min_state_plus = static_cast<std::uint32_t>(count) << max_bits_out;
      sym[s].delta_bits = (static_cast<std::uint32_t>(max_bits_out) << 16) - min_state_plus;
      sym[s].delta_find = static_cast<std::int32_t>(total) - count;
      total += static_cast<std::uint32_t>(count);
    }
    return true;
  }
  std::uint32_t init(std::size_t s) const {
    const Sym& t = sym[s];
    const std::uint32_t nb = (t.delta_bits + (1u << 15)) >> 16;
    const std::uint32_t value = (nb << 16) - t.delta_bits;
    return states[static_cast<std::size_t>(static_cast<std::int32_t>(value >> nb) + t.delta_find)];
  }
  void encode(LsbBitWriter& w, std::uint32_t& state, std::size_t s) const {
    const Sym& t = sym[s];
    const std::uint32_t nb = (state + t.delta_bits) >> 16;
    w.put(state, static_cast<int>(nb));
    state = states[static_cast<std::size_t>(static_cast<std::int32_t>(state >> nb) + t.delta_find)];
  }
  void flush(LsbBitWriter& w, std::uint32_t state) const { w.put(state, log); }
};

// Normalize `counts` (total > 0) to sum 1 << log; every present symbol gets >= 1.
std::vector<std::int16_t> normalize(const std::vector<std::uint32_t>& counts, int log) {
  const std::uint64_t size = std::uint64_t{1} << log;
  std::uint64_t total = 0;
  for (const auto c : counts) total += c;
  std::vector<std::int16_t> norm(counts.size(), 0);
  std::int64_t sum = 0;
  std::size_t largest = 0;
  for (std::size_t s = 0; s < counts.size(); ++s) {
    if (counts[s] == 0) continue;
    const std::uint64_t n = std::max<std::uint64_t>(1, (counts[s] * size + total / 2) / total);
    norm[s] = static_cast<std::int16_t>(n);
    sum += static_cast<std::int64_t>(n);
    if (norm[s] > norm[largest]) largest = s;
  }
  std::int64_t diff = static_cast<std::int64_t>(size) - sum;
  while (diff != 0) {
    if (diff > 0) {
      norm[largest] = static_cast<std::int16_t>(norm[largest] + diff);
      break;
    }
    // Take from the largest entries while keeping each >= 1.
    std::size_t pick = 0;
    for (std::size_t s = 0; s < norm.size(); ++s) {
      if (norm[s] > norm[pick]) pick = s;
    }
    const std::int64_t take = std::min<std::int64_t>(-diff, norm[pick] - 1);
    if (take <= 0) break;
    norm[pick] = static_cast<std::int16_t>(norm[pick] - take);
    diff += take;
  }
  return norm;
}

// FSE table description (inverse of the decoder's reader).
void write_ncount(Bytes& out, const std::vector<std::int16_t>& norm, int log) {
  LsbBitWriter w(out);
  w.put(static_cast<std::uint32_t>(log - 5), 4);
  const std::int32_t size = 1 << log;
  std::int32_t remaining = size + 1;
  std::int32_t threshold = size;
  int bits = log + 1;
  std::size_t symbol = 0;
  bool previous_zero = false;
  const std::size_t max_symbol = norm.size();
  while (symbol < max_symbol && remaining > 1) {
    if (previous_zero) {
      std::size_t start = symbol;
      while (symbol < max_symbol && norm[symbol] == 0) ++symbol;
      while (symbol >= start + 24) {
        start += 24;
        w.put(0xFFFFu, 16);
      }
      while (symbol >= start + 3) {
        start += 3;
        w.put(3, 2);
      }
      w.put(static_cast<std::uint32_t>(symbol - start), 2);
    }
    std::int32_t count = norm[symbol++];
    const std::int32_t max = 2 * threshold - 1 - remaining;
    remaining -= count < 0 ? -count : count;
    ++count;
    if (count >= threshold) count += max;
    w.put(static_cast<std::uint32_t>(count), count < max ? bits - 1 : bits);
    previous_zero = count == 1;
    while (remaining < threshold) {
      --bits;
      threshold >>= 1;
    }
  }
  w.flush();
}

int table_log_for(std::size_t n, std::size_t max_symbol, int max_log) {
  int log = max_log;
  const int src_bits = n > 1 ? std::bit_width(n - 1) - 1 : 0;
  if (src_bits - 2 < log) log = src_bits - 2;
  const int min_bits = std::min(src_bits + 1, std::bit_width(std::max<std::size_t>(max_symbol, 1)) - 1 + 2);
  if (min_bits > log) log = min_bits;
  return std::clamp(log, 5, max_log);
}

// ------------------------------------------------------------------------------- literals

bool huffman_literals(ByteView lit, Bytes& out) {
  std::array<std::uint32_t, 256> counts{};
  for (const std::byte b : lit) ++counts[std::to_integer<std::uint8_t>(b)];
  std::size_t max_symbol = 0;
  for (std::size_t s = 0; s < 256; ++s) {
    if (counts[s] != 0) max_symbol = s;
  }
  std::array<std::uint8_t, 256> lengths{};
  huffman_lengths(std::span(counts.data(), max_symbol + 1), 11, std::span(lengths.data(), max_symbol + 1), true);
  int max_bits = 0;
  for (std::size_t s = 0; s <= max_symbol; ++s) max_bits = std::max<int>(max_bits, lengths[s]);
  if (max_bits == 0) return false;
  // Weights; the last symbol's weight is implied, so it must be present.
  std::array<std::uint8_t, 256> weight{};
  for (std::size_t s = 0; s <= max_symbol; ++s) weight[s] = lengths[s] == 0 ? 0 : static_cast<std::uint8_t>(max_bits + 1 - lengths[s]);
  if (weight[max_symbol] == 0) return false;
  // Code values from the decoder's table layout.
  std::array<std::uint32_t, 256> code{};
  std::uint32_t pos = 0;
  for (int w = 1; w <= max_bits; ++w) {
    for (std::size_t s = 0; s <= max_symbol; ++s) {
      if (weight[s] != w) continue;
      code[s] = pos >> (w - 1);
      pos += 1u << (w - 1);
    }
  }
  if (pos != (1u << max_bits)) return false;
  // Tree description.
  Bytes desc;
  const std::size_t nweights = max_symbol;  // weights of symbols 0 .. max_symbol-1
  bool fse_done = false;
  if (nweights > 1) {
    // FSE-coded weights, two interleaved states.
    std::vector<std::uint32_t> wc(12, 0);
    std::size_t wmax = 0;
    for (std::size_t s = 0; s < nweights; ++s) {
      ++wc[weight[s]];
      wmax = std::max<std::size_t>(wmax, weight[s]);
    }
    wc.resize(wmax + 1);
    const int log = std::clamp(table_log_for(nweights, wmax, 6), 5, 6);
    const std::vector<std::int16_t> norm = normalize(wc, log);
    FseCTable ct;
    if (ct.build(norm.data(), norm.size(), log)) {
      Bytes body;
      write_ncount(body, norm, log);
      Bytes stream;
      LsbBitWriter w(stream);
      std::size_t i = nweights;
      std::uint32_t s1 = 0, s2 = 0;
      if (nweights & 1u) {
        s1 = ct.init(weight[--i]);
        s2 = ct.init(weight[--i]);
        ct.encode(w, s1, weight[--i]);
      } else {
        s2 = ct.init(weight[--i]);
        s1 = ct.init(weight[--i]);
      }
      while (i > 0) {
        ct.encode(w, s2, weight[--i]);
        ct.encode(w, s1, weight[--i]);
      }
      ct.flush(w, s2);
      ct.flush(w, s1);
      w.put(1, 1);
      w.flush();
      append(body, stream);
      if (body.size() < 128 && (nweights > 128 || body.size() < (nweights + 1) / 2)) {
        desc.push_back(static_cast<std::byte>(body.size()));
        append(desc, body);
        fse_done = true;
      }
    }
  }
  if (!fse_done) {
    if (nweights > 128 || nweights == 0) return false;
    desc.push_back(static_cast<std::byte>(127 + nweights));
    for (std::size_t s = 0; s < nweights; s += 2) {
      const std::uint8_t hi = weight[s];
      const std::uint8_t lo = s + 1 < nweights ? weight[s + 1] : 0;
      desc.push_back(static_cast<std::byte>((hi << 4) | lo));
    }
  }
  // Streams.
  const std::size_t n = lit.size();
  const bool single = n < 256;
  const auto encode_stream = [&](ByteView part, Bytes& dst) {
    LsbBitWriter w(dst);
    for (std::size_t i = part.size(); i-- > 0;) {
      const auto s = std::to_integer<std::uint8_t>(part[i]);
      w.put(code[s], max_bits + 1 - weight[s]);
    }
    w.put(1, 1);
    w.flush();
  };
  Bytes streams;
  if (single) {
    encode_stream(lit, streams);
  } else {
    const std::size_t part = (n + 3) / 4;
    std::array<Bytes, 4> s;
    for (std::size_t k = 0; k < 4; ++k) {
      const std::size_t begin = std::min(n, k * part);
      const std::size_t end = k < 3 ? std::min(n, begin + part) : n;
      encode_stream(lit.subspan(begin, end - begin), s[k]);
    }
    for (std::size_t k = 0; k < 3; ++k) {
      if (s[k].size() > 0xFFFF) return false;
      streams.push_back(static_cast<std::byte>(s[k].size() & 0xFFu));
      streams.push_back(static_cast<std::byte>(s[k].size() >> 8));
    }
    for (std::size_t k = 0; k < 4; ++k) append(streams, s[k]);
  }
  const std::size_t comp = desc.size() + streams.size();
  int format;
  int field;
  std::size_t header_len;
  if (single && n < 1024 && comp < 1024) {
    format = 0;
    field = 10;
    header_len = 3;
  } else if (!single && n < 1024 && comp < 1024) {
    format = 1;
    field = 10;
    header_len = 3;
  } else if (n < 16384 && comp < 16384) {
    format = 2;
    field = 14;
    header_len = 4;
  } else if (n < 262144 && comp < 262144) {
    format = 3;
    field = 18;
    header_len = 5;
  } else {
    return false;
  }
  if (single && format != 0) return false;
  if (comp + header_len >= n) return false;  // not worth it
  const std::uint64_t h = 2u | (static_cast<std::uint64_t>(format) << 2) | (static_cast<std::uint64_t>(n) << 4) |
                          (static_cast<std::uint64_t>(comp) << (4 + field));
  for (std::size_t i = 0; i < header_len; ++i) out.push_back(static_cast<std::byte>((h >> (8 * i)) & 0xFFu));
  append(out, desc);
  append(out, streams);
  return true;
}

void literals_section(ByteView lit, Bytes& out) {
  const std::size_t n = lit.size();
  bool same = n > 0;
  for (const std::byte b : lit) same = same && b == lit[0];
  if (n >= 32 && !same && huffman_literals(lit, out)) return;
  const std::uint32_t type = same && n > 1 ? 1u : 0u;
  if (n < 32) {
    out.push_back(static_cast<std::byte>(type | (n << 3)));
  } else if (n < 4096) {
    const std::uint32_t h = type | (1u << 2) | (static_cast<std::uint32_t>(n) << 4);
    out.push_back(static_cast<std::byte>(h & 0xFFu));
    out.push_back(static_cast<std::byte>(h >> 8));
  } else {
    const std::uint32_t h = type | (3u << 2) | (static_cast<std::uint32_t>(n) << 4);
    out.push_back(static_cast<std::byte>(h & 0xFFu));
    out.push_back(static_cast<std::byte>((h >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(h >> 16));
  }
  if (type == 1) {
    out.push_back(lit[0]);
  } else {
    append(out, lit);
  }
}

// ------------------------------------------------------------------------------- sequences

struct Sequence {
  std::uint32_t ll = 0;
  std::uint32_t ml = 0;
  std::uint32_t offset = 0;  // real distance
};

struct Coded {
  std::uint8_t llc, mlc, ofc;
  std::uint32_t ll_extra, ml_extra, of_extra;
};

template <std::size_t N>
std::uint8_t code_for(const std::array<std::uint32_t, N>& base, std::uint32_t v) {
  const auto it = std::upper_bound(base.begin(), base.end(), v);
  return static_cast<std::uint8_t>((it - base.begin()) - 1);
}

struct TableChoice {
  int mode = 0;  // 0 predefined, 1 rle, 2 fse
  int log = 0;
  std::vector<std::int16_t> norm;
  std::uint8_t rle_symbol = 0;
};

template <std::size_t N>
TableChoice choose_table(const std::vector<std::uint32_t>& counts, std::size_t nseq, const std::array<std::int16_t, N>& def,
                         int def_log, int max_log) {
  std::size_t used = 0;
  std::size_t max_symbol = 0;
  for (std::size_t s = 0; s < counts.size(); ++s) {
    if (counts[s] != 0) {
      ++used;
      max_symbol = s;
    }
  }
  TableChoice best;
  if (used == 1 && nseq > 2) {
    best.mode = 1;
    best.rle_symbol = static_cast<std::uint8_t>(max_symbol);
    return best;
  }
  // Cost in 1/256 bits.
  std::uint64_t predefined = ~0ull;
  if (max_symbol < N) {
    predefined = 0;
    for (std::size_t s = 0; s <= max_symbol && predefined != ~0ull; ++s) {
      if (counts[s] == 0) continue;
      const int p = def[s] == -1 ? 1 : def[s];
      if (p <= 0) {
        predefined = ~0ull;
        break;
      }
      predefined += counts[s] * (static_cast<std::uint64_t>(def_log) * 256 - log2_q8(static_cast<std::uint64_t>(p)));
    }
  }
  const int log = table_log_for(nseq, max_symbol, max_log);
  std::vector<std::uint32_t> trimmed(counts.begin(), counts.begin() + static_cast<std::ptrdiff_t>(max_symbol + 1));
  std::vector<std::int16_t> norm = normalize(trimmed, log);
  Bytes header;
  write_ncount(header, norm, log);
  std::uint64_t fse = header.size() * 8 * 256;
  for (std::size_t s = 0; s <= max_symbol; ++s) {
    if (counts[s] != 0) fse += counts[s] * (static_cast<std::uint64_t>(log) * 256 - log2_q8(static_cast<std::uint64_t>(norm[s])));
  }
  if (predefined <= fse) {
    best.mode = 0;
    return best;
  }
  best.mode = 2;
  best.log = log;
  best.norm = std::move(norm);
  return best;
}

// Encode a block's sequences section; reps are updated like the decoder will.
void sequences_section(const std::vector<Sequence>& seqs, std::array<std::uint32_t, 3>& reps, Bytes& out) {
  const std::size_t n = seqs.size();
  if (n < 128) {
    out.push_back(static_cast<std::byte>(n));
  } else if (n < 0x7F00) {
    out.push_back(static_cast<std::byte>((n >> 8) + 128));
    out.push_back(static_cast<std::byte>(n & 0xFFu));
  } else {
    out.push_back(std::byte{255});
    out.push_back(static_cast<std::byte>((n - 0x7F00) & 0xFFu));
    out.push_back(static_cast<std::byte>((n - 0x7F00) >> 8));
  }
  if (n == 0) return;
  std::vector<Coded> coded(n);
  std::vector<std::uint32_t> llc(36, 0), mlc(53, 0), ofc(32, 0);
  for (std::size_t i = 0; i < n; ++i) {
    const Sequence& s = seqs[i];
    std::uint32_t of_value;
    // Repeat offsets (RFC 8878 3.1.1.5), with the literal-length-0 shift.
    if (s.ll > 0) {
      if (s.offset == reps[0]) {
        of_value = 1;
      } else if (s.offset == reps[1]) {
        of_value = 2;
      } else if (s.offset == reps[2]) {
        of_value = 3;
      } else {
        of_value = s.offset + 3;
      }
    } else {
      if (s.offset == reps[1]) {
        of_value = 1;
      } else if (s.offset == reps[2]) {
        of_value = 2;
      } else if (reps[0] > 1 && s.offset == reps[0] - 1) {
        of_value = 3;
      } else {
        of_value = s.offset + 3;
      }
    }
    // Apply the decoder's history update.
    if (of_value > 3) {
      reps = {s.offset, reps[0], reps[1]};
    } else {
      const std::uint32_t index = of_value - 1 + (s.ll == 0 ? 1u : 0u);
      if (index != 0) {
        if (index >= 2) reps[2] = reps[1];
        reps[1] = reps[0];
        reps[0] = s.offset;
      }
    }
    Coded& c = coded[i];
    c.llc = code_for(ll_base, s.ll);
    c.mlc = code_for(ml_base, s.ml);
    c.ofc = static_cast<std::uint8_t>(std::bit_width(of_value) - 1);
    c.ll_extra = s.ll - ll_base[c.llc];
    c.ml_extra = s.ml - ml_base[c.mlc];
    c.of_extra = of_value - (1u << c.ofc);
    ++llc[c.llc];
    ++mlc[c.mlc];
    ++ofc[c.ofc];
  }
  const TableChoice tll = choose_table(llc, n, ll_default, ll_default_log, 9);
  const TableChoice tof = choose_table(ofc, n, of_default, of_default_log, 8);
  const TableChoice tml = choose_table(mlc, n, ml_default, ml_default_log, 9);
  out.push_back(static_cast<std::byte>((tll.mode << 6) | (tof.mode << 4) | (tml.mode << 2)));
  FseCTable ctll, ctof, ctml;
  const auto setup = [&](const TableChoice& t, FseCTable& ct, const auto& def, int def_log) {
    if (t.mode == 0) {
      ct.build(def.data(), def.size(), def_log);
    } else if (t.mode == 1) {
      out.push_back(static_cast<std::byte>(t.rle_symbol));
      std::vector<std::int16_t> one(static_cast<std::size_t>(t.rle_symbol) + 1, 0);
      one.back() = 1;
      ct.build(one.data(), one.size(), 0);
    } else {
      write_ncount(out, t.norm, t.log);
      ct.build(t.norm.data(), t.norm.size(), t.log);
    }
  };
  setup(tll, ctll, ll_default, ll_default_log);
  setup(tof, ctof, of_default, of_default_log);
  setup(tml, ctml, ml_default, ml_default_log);
  Bytes stream;
  LsbBitWriter w(stream);
  const Coded& last = coded[n - 1];
  std::uint32_t sml = ctml.init(last.mlc);
  std::uint32_t sof = ctof.init(last.ofc);
  std::uint32_t sll = ctll.init(last.llc);
  w.put(last.ll_extra, ll_bits[last.llc]);
  w.put(last.ml_extra, ml_bits[last.mlc]);
  w.put(last.of_extra, last.ofc);
  for (std::size_t i = n - 1; i-- > 0;) {
    const Coded& c = coded[i];
    ctof.encode(w, sof, c.ofc);
    ctml.encode(w, sml, c.mlc);
    ctll.encode(w, sll, c.llc);
    w.put(c.ll_extra, ll_bits[c.llc]);
    w.put(c.ml_extra, ml_bits[c.mlc]);
    w.put(c.of_extra, c.ofc);
  }
  ctml.flush(w, sml);
  ctof.flush(w, sof);
  ctll.flush(w, sll);
  w.put(1, 1);
  w.flush();
  append(out, stream);
}

// ------------------------------------------------------------------------------- matching

class Matcher {
 public:
  Matcher(const std::uint8_t* data, std::size_t size, const Level& lv)
      : data_(data), size_(size), lv_(lv) {
    window_ = std::size_t{1} << lv.window_log;
    const int chain_log = std::min(lv.window_log, std::max(10, std::bit_width(std::max<std::size_t>(size, 1))));
    chain_mask_ = (std::size_t{1} << chain_log) - 1;
    hash_log_ = std::clamp(chain_log - 1, 12, 20);
    head_.assign(std::size_t{1} << hash_log_, 0);
    chain_.assign(chain_mask_ + 1, 0);
    if (lv.min_match == 3) head3_.assign(std::size_t{1} << 14, 0);
  }

  void insert_until(std::size_t pos) {
    while (next_ < pos) insert(next_++);
  }

  // Best match at pos (pos must be the next uninserted position); inserts pos.
  Sequence find(std::size_t pos, std::size_t end, const std::array<std::uint32_t, 3>& reps) {
    insert_until(pos);
    Sequence best;
    const std::size_t avail = end - pos;
    const std::size_t min_match = static_cast<std::size_t>(lv_.min_match);
    if (avail < min_match) {
      insert(next_++);
      return best;
    }
    const std::uint8_t* cur = data_ + pos;
    // Repeat offsets first: they are cheap to encode.
    for (const std::uint32_t r : reps) {
      if (r == 0 || r > pos) continue;
      const std::size_t len = common(cur, cur - r, avail);
      if (len >= min_match && len > best.ml) {
        best.ml = static_cast<std::uint32_t>(len);
        best.offset = r;
      }
    }
    const std::size_t rep_len = best.ml;
    if (head3_.size() != 0 && avail >= 3) {
      const std::uint32_t h3 = hash3(cur);
      const std::size_t cand = head3_[h3];
      if (cand != 0 && pos - (cand - 1) <= window_ && cand - 1 < pos) {
        const std::size_t len = common(cur, data_ + cand - 1, avail);
        if (len >= 3 && len > best.ml && pos - (cand - 1) < 4096) {
          best.ml = static_cast<std::uint32_t>(len);
          best.offset = static_cast<std::uint32_t>(pos - (cand - 1));
        }
      }
    }
    if (avail >= 4) {
      std::size_t cand = head_[hash4(cur)];
      for (int d = 0; d < lv_.depth && cand != 0; ++d) {
        const std::size_t c = cand - 1;
        if (c >= pos || pos - c > window_ || pos - c > chain_mask_) break;
        const std::uint8_t* m = data_ + c;
        if (best.ml < avail && m[best.ml] == cur[best.ml]) {
          const std::size_t len = common(cur, m, avail);
          // A new offset must beat a repeat match by more than its extra cost.
          if (len > best.ml && (len > rep_len + 1 || rep_len == 0) && len >= 4) {
            best.ml = static_cast<std::uint32_t>(len);
            best.offset = static_cast<std::uint32_t>(pos - c);
            if (len >= static_cast<std::size_t>(lv_.target)) break;
          }
        }
        cand = chain_[c & chain_mask_];
      }
    }
    insert(next_++);
    return best;
  }

  [[nodiscard]] std::size_t next() const noexcept { return next_; }

 private:
  static std::size_t common(const std::uint8_t* a, const std::uint8_t* b, std::size_t max) {
    std::size_t n = 0;
    while (n + 8 <= max) {
      std::uint64_t x, y;
      std::memcpy(&x, a + n, 8);
      std::memcpy(&y, b + n, 8);
      if (x != y) {
        const std::uint64_t diff = x ^ y;
        return n + static_cast<std::size_t>((std::endian::native == std::endian::little ? std::countr_zero(diff)
                                                                                         : std::countl_zero(diff)) /
                                            8);
      }
      n += 8;
    }
    while (n < max && a[n] == b[n]) ++n;
    return n;
  }
  std::uint32_t hash4(const std::uint8_t* p) const {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::big) v = std::byteswap(v);
    return (v * 2654435761u) >> (32 - hash_log_);
  }
  static std::uint32_t hash3(const std::uint8_t* p) {
    const std::uint32_t v = p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16);
    return (v * 506832829u) >> (32 - 14);
  }
  void insert(std::size_t pos) {
    if (pos + 4 <= size_) {
      const std::uint32_t h = hash4(data_ + pos);
      chain_[pos & chain_mask_] = static_cast<std::uint32_t>(head_[h]);
      head_[h] = static_cast<std::uint32_t>(pos + 1);
    }
    if (!head3_.empty() && pos + 3 <= size_) head3_[hash3(data_ + pos)] = static_cast<std::uint32_t>(pos + 1);
  }

  const std::uint8_t* data_;
  std::size_t size_;
  Level lv_;
  std::size_t window_ = 0;
  std::size_t chain_mask_ = 0;
  int hash_log_ = 16;
  std::vector<std::uint32_t> head_;
  std::vector<std::uint32_t> chain_;
  std::vector<std::uint32_t> head3_;
  std::size_t next_ = 0;
};

// Score of a match: length minus the offset's approximate extra cost.
std::int64_t gain(const Sequence& m, const std::array<std::uint32_t, 3>& reps) {
  if (m.ml == 0) return -1;
  const bool rep = m.offset == reps[0] || m.offset == reps[1] || m.offset == reps[2];
  return static_cast<std::int64_t>(m.ml) * 4 - (rep ? 1 : std::bit_width(m.offset + 3u));
}

}  // namespace

Result<Bytes> zstd_encode(ByteView input, const EncodeOptions& options) {
  const Level& lv = levels[static_cast<std::size_t>(std::clamp(options.level, 0, 9))];
  int window_log = lv.window_log;
  if (options.dictionary_size != 0) window_log = std::clamp(std::bit_width(options.dictionary_size - 1u), 10, 27);
  const auto* data = reinterpret_cast<const std::uint8_t*>(input.data());
  const std::size_t size = input.size();
  Bytes out;
  // ---- frame header
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((frame_magic >> (8 * i)) & 0xFFu));
  const bool single = options.content_size && size <= (std::size_t{1} << window_log);
  int fcs_flag = 0;
  std::size_t fcs_bytes = 0;
  if (options.content_size) {
    if (single && size < 256) {
      fcs_flag = 0;
      fcs_bytes = 1;
    } else if (size >= 256 && size < 65536 + 256) {
      fcs_flag = 1;
      fcs_bytes = 2;
    } else if (size <= 0xFFFFFFFFull) {
      fcs_flag = 2;
      fcs_bytes = 4;
    } else {
      fcs_flag = 3;
      fcs_bytes = 8;
    }
  }
  const std::uint8_t fhd = static_cast<std::uint8_t>((fcs_flag << 6) | (single ? 0x20 : 0) | (options.content_checksum ? 0x04 : 0));
  out.push_back(static_cast<std::byte>(fhd));
  if (!single) out.push_back(static_cast<std::byte>((window_log - 10) << 3));
  std::uint64_t fcs_value = size;
  if (fcs_bytes == 2) fcs_value = size - 256;
  for (std::size_t i = 0; i < fcs_bytes; ++i) out.push_back(static_cast<std::byte>((fcs_value >> (8 * i)) & 0xFFu));

  // ---- blocks
  Level effective = lv;
  effective.window_log = window_log;
  Matcher matcher(data, size, effective);
  std::array<std::uint32_t, 3> reps = {1, 4, 8};
  std::array<std::uint32_t, 3> parse_reps = reps;
  std::size_t bstart = 0;
  if (size == 0) {
    out.push_back(std::byte{1});
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
  }
  while (bstart < size) {
    if (!options.progress.report(static_cast<double>(bstart) / static_cast<double>(size))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    const std::size_t bend = std::min(size, bstart + block_max);
    const bool last = bend == size;
    std::vector<Sequence> seqs;
    Bytes lits;
    std::size_t pos = bstart;
    std::size_t lit_start = bstart;
    const std::array<std::uint32_t, 3> block_reps = reps;
    parse_reps = reps;
    while (pos < bend) {
      Sequence m = matcher.find(pos, bend, parse_reps);
      // A short match far away costs more than its literals.
      if (m.ml == 0 || gain(m, parse_reps) <= 0) {
        ++pos;
        continue;
      }
      // Lazy evaluation: a better match one or two bytes later wins.
      std::size_t start = pos;
      for (int step = 0; step < lv.lazy && start + 1 < bend; ++step) {
        const Sequence next = matcher.find(start + 1, bend, parse_reps);
        if (next.ml != 0 && gain(next, parse_reps) > gain(m, parse_reps) + 4) {
          m = next;
          ++start;
        } else {
          break;
        }
      }
      Sequence s;
      s.ll = static_cast<std::uint32_t>(start - lit_start);
      s.ml = m.ml;
      s.offset = m.offset;
      append(lits, ByteView(input).subspan(lit_start, start - lit_start));
      seqs.push_back(s);
      // Track repeat offsets as the decoder will see them.
      if (s.offset == parse_reps[0] && s.ll > 0) {
      } else if (s.offset == parse_reps[1]) {
        parse_reps = {s.offset, parse_reps[0], parse_reps[2]};
      } else if (s.offset == parse_reps[2]) {
        parse_reps = {s.offset, parse_reps[0], parse_reps[1]};
      } else if (s.offset != parse_reps[0]) {
        parse_reps = {s.offset, parse_reps[0], parse_reps[1]};
      }
      pos = start + m.ml;
      lit_start = pos;
      matcher.insert_until(pos);
    }
    matcher.insert_until(bend);
    append(lits, ByteView(input).subspan(lit_start, bend - lit_start));
    // ---- block body
    Bytes body;
    literals_section(lits, body);
    std::array<std::uint32_t, 3> encoded_reps = block_reps;
    sequences_section(seqs, encoded_reps, body);
    const std::size_t raw = bend - bstart;
    bool all_same = true;
    for (std::size_t i = bstart + 1; i < bend && all_same; ++i) all_same = data[i] == data[bstart];
    std::uint32_t header;
    if (all_same && raw > 4) {
      header = (last ? 1u : 0u) | (1u << 1) | (static_cast<std::uint32_t>(raw) << 3);
      for (int i = 0; i < 3; ++i) out.push_back(static_cast<std::byte>((header >> (8 * i)) & 0xFFu));
      out.push_back(static_cast<std::byte>(data[bstart]));
      // reps unchanged: an RLE block carries no sequences
    } else if (body.size() >= raw) {
      header = (last ? 1u : 0u) | (static_cast<std::uint32_t>(raw) << 3);
      for (int i = 0; i < 3; ++i) out.push_back(static_cast<std::byte>((header >> (8 * i)) & 0xFFu));
      append(out, ByteView(input).subspan(bstart, raw));
    } else {
      header = (last ? 1u : 0u) | (2u << 1) | (static_cast<std::uint32_t>(body.size()) << 3);
      for (int i = 0; i < 3; ++i) out.push_back(static_cast<std::byte>((header >> (8 * i)) & 0xFFu));
      append(out, body);
      reps = encoded_reps;
    }
    bstart = bend;
  }
  if (options.content_checksum) {
    const auto h = static_cast<std::uint32_t>(archive::xxh64(input));
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((h >> (8 * i)) & 0xFFu));
  }
  return out;
}

}  // namespace cvt::compression::detail
