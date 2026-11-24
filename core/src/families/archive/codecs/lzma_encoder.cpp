// Convertible core - archive family: LZMA encoder.
//
// Match finder: hash chains over 2-, 3- and 4-byte hashes. Parsing: levels
// 0-3 greedy with repeat-distance preference; levels 4-9 price-based optimal
// parsing over a look-ahead window (the classic LZMA "normal" mode): every
// position in the window is reached by the cheapest sequence of literals,
// matches, repeat matches and short repeats, priced from the live
// probabilities in 1/16-bit units with integer arithmetic only, so the output
// is identical on every platform.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cstring>

#include "families/archive/codecs/lzma_internal.hpp"
#include "util/crc.hpp"

namespace cvt::compression::detail::lzma {

namespace {

constexpr std::uint32_t top = 1u << 24;
constexpr int price_shift = 4;
constexpr std::uint32_t infinity_price = 1u << 30;
constexpr int opt_max = 4096;

// Integer -log2(p) table in 1/16 bit units, indexed by prob >> 4.
constexpr std::array<std::uint32_t, 128> make_prob_prices() noexcept {
  std::array<std::uint32_t, 128> t{};
  for (std::uint32_t i = 0; i < 128; ++i) {
    std::uint32_t w = (i << 4) + 8;
    std::uint32_t bits = 0;
    for (int j = 0; j < price_shift; ++j) {
      w = w * w;
      bits <<= 1;
      while (w >= (1u << 16)) {
        w >>= 1;
        ++bits;
      }
    }
    t[i] = (11u << price_shift) - 15u - bits;
  }
  return t;
}
constexpr std::array<std::uint32_t, 128> prob_prices = make_prob_prices();

inline std::uint32_t price0(std::uint16_t p) noexcept { return prob_prices[p >> 4]; }
inline std::uint32_t price1(std::uint16_t p) noexcept { return prob_prices[(2048u - p) >> 4]; }
inline std::uint32_t price_bit(std::uint16_t p, std::uint32_t bit) noexcept { return bit ? price1(p) : price0(p); }

template <std::size_t N>
std::uint32_t tree_price(const std::array<std::uint16_t, N>& probs, std::size_t base, int bits, std::uint32_t symbol) {
  std::uint32_t price = 0;
  std::uint32_t m = 1;
  for (int i = bits - 1; i >= 0; --i) {
    const std::uint32_t b = (symbol >> i) & 1u;
    price += price_bit(probs[base + m], b);
    m = (m << 1) | b;
  }
  return price;
}
template <std::size_t N>
std::uint32_t reverse_tree_price(const std::array<std::uint16_t, N>& probs, std::size_t base, int bits,
                                 std::uint32_t symbol) {
  std::uint32_t price = 0;
  std::uint32_t m = 1;
  for (int i = 0; i < bits; ++i) {
    const std::uint32_t b = symbol & 1u;
    symbol >>= 1;
    price += price_bit(probs[base + m], b);
    m = (m << 1) | b;
  }
  return price;
}

std::uint32_t dist_slot(std::uint32_t dist) noexcept {
  if (dist < 4) return dist;
  const int n = std::bit_width(dist) - 1;
  return static_cast<std::uint32_t>(2 * n) + ((dist >> (n - 1)) & 1u);
}

class RangeEncoder {
 public:
  explicit RangeEncoder(Bytes& out) : out_(&out) {}
  void bit(std::uint16_t& prob, std::uint32_t b) {
    const std::uint32_t bound = (range_ >> 11) * prob;
    if (b == 0) {
      range_ = bound;
      prob = static_cast<std::uint16_t>(prob + ((2048u - prob) >> 5));
    } else {
      low_ += bound;
      range_ -= bound;
      prob = static_cast<std::uint16_t>(prob - (prob >> 5));
    }
    while (range_ < top) {
      range_ <<= 8;
      shift_low();
    }
  }
  void direct(std::uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
      range_ >>= 1;
      if ((value >> i) & 1u) low_ += range_;
      while (range_ < top) {
        range_ <<= 8;
        shift_low();
      }
    }
  }
  template <std::size_t N>
  void tree(std::array<std::uint16_t, N>& probs, std::size_t base, int bits, std::uint32_t symbol) {
    std::uint32_t m = 1;
    for (int i = bits - 1; i >= 0; --i) {
      const std::uint32_t b = (symbol >> i) & 1u;
      bit(probs[base + m], b);
      m = (m << 1) | b;
    }
  }
  template <std::size_t N>
  void reverse_tree(std::array<std::uint16_t, N>& probs, std::size_t base, int bits, std::uint32_t symbol) {
    std::uint32_t m = 1;
    for (int i = 0; i < bits; ++i) {
      const std::uint32_t b = symbol & 1u;
      symbol >>= 1;
      bit(probs[base + m], b);
      m = (m << 1) | b;
    }
  }
  void flush() {
    for (int i = 0; i < 5; ++i) shift_low();
  }
  [[nodiscard]] std::size_t pending() const noexcept { return out_->size() + static_cast<std::size_t>(cache_size_) + 4; }

 private:
  void shift_low() {
    if (static_cast<std::uint32_t>(low_) < 0xFF000000u || (low_ >> 32) != 0) {
      const auto carry = static_cast<std::uint8_t>(low_ >> 32);
      std::uint8_t temp = cache_;
      do {
        out_->push_back(static_cast<std::byte>(static_cast<std::uint8_t>(temp + carry)));
        temp = 0xFF;
      } while (--cache_size_ != 0);
      cache_ = static_cast<std::uint8_t>(low_ >> 24);
    }
    ++cache_size_;
    low_ = (low_ & 0x00FFFFFFull) << 8;
  }
  Bytes* out_;
  std::uint64_t low_ = 0;
  std::uint32_t range_ = 0xFFFFFFFFu;
  std::uint8_t cache_ = 0;
  std::uint64_t cache_size_ = 1;
};

struct Match {
  std::uint32_t len = 0;
  std::uint32_t dist = 0;  // 0-based (distance - 1)
};

// Hash-chain match finder over the whole buffer.
class MatchFinder {
 public:
  MatchFinder(const std::uint8_t* data, std::size_t size, std::uint32_t dict, int depth, int nice)
      : data_(data), size_(size), dict_(dict), depth_(depth), nice_(nice) {
    chain_size_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(dict, std::max<std::size_t>(size, 1)));
    int hb = std::clamp(std::bit_width(chain_size_) - 1, 12, 22);
    hash_bits_ = hb;
    head4_.assign(std::size_t{1} << hb, 0);
    head3_.assign(std::size_t{1} << 16, 0);
    head2_.assign(std::size_t{1} << 10, 0);
    chain_.assign(chain_size_, 0);
  }

  [[nodiscard]] std::size_t position() const noexcept { return pos_; }

  // Matches at the current position (increasing lengths), then advance.
  void find(std::size_t limit, std::vector<Match>& out) {
    out.clear();
    const std::size_t pos = pos_;
    const std::uint32_t avail = static_cast<std::uint32_t>(std::min<std::size_t>(limit - pos, match_max_len));
    if (avail < 2 || pos + 2 > size_) {
      insert_short(pos);
      ++pos_;
      return;
    }
    const std::uint8_t* cur = data_ + pos;
    const std::uint32_t temp = util::detail::crc32_table[cur[0]] ^ cur[1];
    const std::uint32_t h2 = temp & 1023u;
    std::uint32_t best = 1;
    const auto try_candidate = [&](std::uint32_t stored) {
      if (stored == 0) return;
      const std::size_t cand = stored - 1;
      if (cand >= pos || pos - cand > dict_) return;
      std::uint32_t len = 0;
      const std::uint8_t* m = data_ + cand;
      while (len < avail && m[len] == cur[len]) ++len;
      if (len > best) {
        best = len;
        out.push_back({len, static_cast<std::uint32_t>(pos - cand - 1)});
      }
    };
    try_candidate(head2_[h2]);
    head2_[h2] = static_cast<std::uint32_t>(pos + 1);
    if (pos + 3 > size_ || avail < 3) {
      ++pos_;
      return;
    }
    const std::uint32_t h3 = (temp ^ (static_cast<std::uint32_t>(cur[2]) << 8)) & 0xFFFFu;
    if (best < avail) try_candidate(head3_[h3]);
    head3_[h3] = static_cast<std::uint32_t>(pos + 1);
    if (pos + 4 > size_ || avail < 4) {
      ++pos_;
      return;
    }
    const std::uint32_t h4 = hash4(cur);
    std::uint32_t stored = head4_[h4];
    chain_[static_cast<std::uint32_t>(pos % chain_size_)] = stored;
    head4_[h4] = static_cast<std::uint32_t>(pos + 1);
    for (int d = 0; d < depth_ && stored != 0 && best < avail; ++d) {
      const std::size_t cand = stored - 1;
      if (cand >= pos || pos - cand > dict_ || pos - cand >= chain_size_) break;
      const std::uint8_t* m = data_ + cand;
      if (m[best] == cur[best] && m[0] == cur[0]) {
        std::uint32_t len = 0;
        while (len < avail && m[len] == cur[len]) ++len;
        if (len > best) {
          best = len;
          out.push_back({len, static_cast<std::uint32_t>(pos - cand - 1)});
          if (len >= static_cast<std::uint32_t>(nice_)) break;
        }
      }
      stored = chain_[static_cast<std::uint32_t>(cand % chain_size_)];
    }
    ++pos_;
  }

  // Insert the current position without searching, then advance.
  void skip() {
    const std::size_t pos = pos_;
    insert_short(pos);
    if (pos + 4 <= size_) {
      const std::uint32_t h4 = hash4(data_ + pos);
      chain_[static_cast<std::uint32_t>(pos % chain_size_)] = head4_[h4];
      head4_[h4] = static_cast<std::uint32_t>(pos + 1);
    }
    ++pos_;
  }

 private:
  std::uint32_t hash4(const std::uint8_t* cur) const noexcept {
    const std::uint32_t temp = util::detail::crc32_table[cur[0]] ^ cur[1] ^ (static_cast<std::uint32_t>(cur[2]) << 8);
    return (temp ^ (util::detail::crc32_table[cur[3]] << 5)) & ((1u << hash_bits_) - 1u);
  }
  void insert_short(std::size_t pos) {
    if (pos + 2 <= size_) {
      const std::uint8_t* cur = data_ + pos;
      const std::uint32_t temp = util::detail::crc32_table[cur[0]] ^ cur[1];
      head2_[temp & 1023u] = static_cast<std::uint32_t>(pos + 1);
      if (pos + 3 <= size_) head3_[(temp ^ (static_cast<std::uint32_t>(cur[2]) << 8)) & 0xFFFFu] = static_cast<std::uint32_t>(pos + 1);
    }
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::uint32_t dict_;
  int depth_;
  int nice_;
  int hash_bits_ = 16;
  std::uint32_t chain_size_ = 1;
  std::size_t pos_ = 0;
  std::vector<std::uint32_t> head4_, head3_, head2_, chain_;
};

}  // namespace

EncoderParams preset(int level, std::uint64_t input_size, std::uint32_t dict_override) {
  struct Row {
    std::uint32_t dict;
    int nice;
    int depth;
    bool optimal;
  };
  static constexpr std::array<Row, 10> rows = {{
      {1u << 18, 32, 4, false},
      {1u << 20, 64, 8, false},
      {1u << 21, 96, 16, false},
      {1u << 22, 128, 24, false},
      {1u << 22, 32, 16, true},
      {1u << 23, 48, 24, true},
      {1u << 23, 64, 48, true},
      {1u << 24, 64, 64, true},
      {1u << 25, 96, 96, true},
      {1u << 26, 128, 160, true},
  }};
  const Row& r = rows[static_cast<std::size_t>(std::clamp(level, 0, 9))];
  EncoderParams p;
  std::uint32_t dict = dict_override != 0 ? dict_override : r.dict;
  // Never use (or describe) a dictionary larger than the input needs.
  if (input_size < dict) {
    std::uint32_t d = 1u << 12;
    while (d < input_size && d < dict) d <<= 1;
    dict = std::min(dict, d);
  }
  p.props.dict_size = std::max<std::uint32_t>(dict, 1u << 12);
  p.nice_len = r.nice;
  p.depth = r.depth;
  p.optimal = r.optimal;
  return p;
}

struct Encoder::Impl {
  Impl(ByteView data, const EncoderParams& params)
      : data(reinterpret_cast<const std::uint8_t*>(data.data())),
        size(data.size()),
        p(params),
        finder(this->data, size, params.props.dict_size, params.depth, params.nice_len) {
    reset_state();
    opt.resize(opt_max + match_max_len + 2);
  }

  const std::uint8_t* data;
  std::size_t size;
  EncoderParams p;
  Model model;
  CoderState st;
  MatchFinder finder;
  std::size_t pos = 0;  // next byte to encode
  RangeEncoder* rc = nullptr;

  // price tables
  std::array<std::array<std::uint32_t, match_max_len + 1>, 1 << pos_bits_max> len_prices{};
  std::array<std::array<std::uint32_t, match_max_len + 1>, 1 << pos_bits_max> rep_len_prices{};
  std::array<std::array<std::uint32_t, 64>, len_to_pos_states> slot_prices{};
  std::array<std::array<std::uint32_t, num_full_distances>, len_to_pos_states> dist_prices{};
  std::array<std::uint32_t, 1 << align_bits> align_prices{};
  int len_counter = 0;
  int dist_counter = 0;

  struct Node {
    std::uint32_t price = infinity_price;
    std::uint32_t prev = 0;
    std::uint32_t back = 0;  // 0..3 rep index, >= 4: dist + 4, literal_back, short_rep_back
    std::uint32_t len = 0;
    int state = 0;
    std::array<std::uint32_t, 4> reps{};
  };
  static constexpr std::uint32_t literal_back = 0xFFFFFFFFu;
  static constexpr std::uint32_t short_rep_back = 0xFFFFFFFEu;
  std::vector<Node> opt;
  std::vector<Match> matches;
  std::size_t cached_at = static_cast<std::size_t>(-1);
  std::vector<Match> cached;

  // Matches at `at`. The finder advances one position per call; a parse step
  // that stops early asks again for the position it already searched.
  void find_at(std::size_t at, std::size_t limit, std::vector<Match>& out) {
    if (finder.position() == at + 1 && cached_at == at) {
      out = cached;
      return;
    }
    while (finder.position() < at) finder.skip();
    if (finder.position() != at) {
      out.clear();
      return;
    }
    finder.find(limit, out);
    cached_at = at;
    cached = out;
  }

  void reset_state() {
    model.reset(p.props.lc, p.props.lp);
    st.reset();
    len_counter = 0;
    dist_counter = 0;
  }

  [[nodiscard]] std::uint32_t pos_state(std::size_t at) const noexcept {
    return static_cast<std::uint32_t>(at) & ((1u << p.props.pb) - 1u);
  }

  // ---- prices ----------------------------------------------------------------
  std::uint32_t length_price_raw(const Model::Length& l, std::uint32_t len, std::uint32_t ps) const {
    const std::uint32_t v = len - match_min_len;
    if (v < 8) return price0(l.choice) + tree_price(l.low, ps << 3, 3, v);
    if (v < 16) return price1(l.choice) + price0(l.choice2) + tree_price(l.mid, ps << 3, 3, v - 8);
    return price1(l.choice) + price1(l.choice2) + tree_price(l.high, 0, 8, v - 16);
  }
  void update_len_prices() {
    const std::uint32_t states = 1u << p.props.pb;
    for (std::uint32_t ps = 0; ps < states; ++ps) {
      for (std::uint32_t len = match_min_len; len <= match_max_len; ++len) {
        len_prices[ps][len] = length_price_raw(model.len, len, ps);
        rep_len_prices[ps][len] = length_price_raw(model.rep_len, len, ps);
      }
    }
    len_counter = 256;
  }
  void update_dist_prices() {
    for (std::uint32_t ls = 0; ls < len_to_pos_states; ++ls) {
      for (std::uint32_t slot = 0; slot < 64; ++slot) {
        std::uint32_t price = tree_price(model.pos_slot, ls << 6, 6, slot);
        if (slot >= end_pos_model) price += ((slot >> 1) - 1 - align_bits) << price_shift;
        slot_prices[ls][slot] = price;
      }
      for (std::uint32_t dist = 0; dist < num_full_distances; ++dist) {
        const std::uint32_t slot = dist_slot(dist);
        std::uint32_t price = slot_prices[ls][slot];
        if (slot >= start_pos_model) {
          const int direct_bits = static_cast<int>(slot >> 1) - 1;
          const std::uint32_t base = (2u | (slot & 1u)) << direct_bits;
          price += reverse_tree_price(model.spec_pos, base - slot, direct_bits, dist - base);
        }
        dist_prices[ls][dist] = price;
      }
    }
    for (std::uint32_t i = 0; i < (1u << align_bits); ++i) align_prices[i] = reverse_tree_price(model.align, 0, align_bits, i);
    dist_counter = 128;
  }
  std::uint32_t match_dist_price(std::uint32_t dist, std::uint32_t len) const {
    const std::uint32_t ls = std::min<std::uint32_t>(len - match_min_len, len_to_pos_states - 1);
    if (dist < num_full_distances) return dist_prices[ls][dist];
    return slot_prices[ls][dist_slot(dist)] + align_prices[dist & ((1u << align_bits) - 1)];
  }
  std::uint32_t literal_price(std::size_t at, int state, std::uint32_t rep0) const {
    const std::uint32_t prev = at > 0 ? data[at - 1] : 0u;
    const std::uint16_t* probs = model.literal.data() +
                                 std::size_t{0x300} * (((static_cast<std::uint32_t>(at) & ((1u << p.props.lp) - 1u)) << p.props.lc) +
                                                       (prev >> (8 - p.props.lc)));
    std::uint32_t symbol = data[at] | 0x100u;
    std::uint32_t price = 0;
    if (state >= 7 && at > rep0) {
      std::uint32_t match_byte = data[at - rep0 - 1];
      std::uint32_t offs = 0x100;
      do {
        match_byte <<= 1;
        price += price_bit(probs[offs + (match_byte & offs) + (symbol >> 8)], (symbol >> 7) & 1u);
        symbol <<= 1;
        offs &= ~(match_byte ^ symbol);
      } while (symbol < 0x10000);
      return price;
    }
    do {
      price += price_bit(probs[symbol >> 8], (symbol >> 7) & 1u);
      symbol <<= 1;
    } while (symbol < 0x10000);
    return price;
  }
  std::uint32_t rep_price(std::uint32_t index, int state, std::uint32_t ps) const {
    const auto s = static_cast<std::size_t>(state);
    std::uint32_t price = price1(model.is_match[(s << pos_bits_max) + ps]) + price1(model.is_rep[s]);
    if (index == 0) {
      price += price0(model.is_rep_g0[s]) + price1(model.is_rep0_long[(s << pos_bits_max) + ps]);
    } else {
      price += price1(model.is_rep_g0[s]);
      if (index == 1) {
        price += price0(model.is_rep_g1[s]);
      } else {
        price += price1(model.is_rep_g1[s]) + price_bit(model.is_rep_g2[s], index - 2);
      }
    }
    return price;
  }
  std::uint32_t short_rep_price(int state, std::uint32_t ps) const {
    const auto s = static_cast<std::size_t>(state);
    return price1(model.is_match[(s << pos_bits_max) + ps]) + price1(model.is_rep[s]) + price0(model.is_rep_g0[s]) +
           price0(model.is_rep0_long[(s << pos_bits_max) + ps]);
  }

  // ---- symbol output ----------------------------------------------------------
  void put_literal() {
    const std::size_t at = pos;
    const std::uint32_t ps = pos_state(at);
    rc->bit(model.is_match[(static_cast<std::size_t>(st.state) << pos_bits_max) + ps], 0);
    const std::uint32_t prev = at > 0 ? data[at - 1] : 0u;
    std::uint16_t* probs = model.literal.data() +
                           std::size_t{0x300} * (((static_cast<std::uint32_t>(at) & ((1u << p.props.lp) - 1u)) << p.props.lc) +
                                                 (prev >> (8 - p.props.lc)));
    std::uint32_t symbol = data[at] | 0x100u;
    if (st.state >= 7) {
      std::uint32_t match_byte = data[at - st.reps[0] - 1];
      std::uint32_t offs = 0x100;
      do {
        match_byte <<= 1;
        rc->bit(probs[offs + (match_byte & offs) + (symbol >> 8)], (symbol >> 7) & 1u);
        symbol <<= 1;
        offs &= ~(match_byte ^ symbol);
      } while (symbol < 0x10000);
    } else {
      do {
        rc->bit(probs[symbol >> 8], (symbol >> 7) & 1u);
        symbol <<= 1;
      } while (symbol < 0x10000);
    }
    st.state = state_after_literal(st.state);
    ++pos;
  }
  void put_length(Model::Length& l, std::uint32_t len, std::uint32_t ps) {
    const std::uint32_t v = len - match_min_len;
    if (v < 8) {
      rc->bit(l.choice, 0);
      rc->tree(l.low, ps << 3, 3, v);
    } else {
      rc->bit(l.choice, 1);
      if (v < 16) {
        rc->bit(l.choice2, 0);
        rc->tree(l.mid, ps << 3, 3, v - 8);
      } else {
        rc->bit(l.choice2, 1);
        rc->tree(l.high, 0, 8, v - 16);
      }
    }
  }
  void put_match(std::uint32_t dist, std::uint32_t len) {
    const std::uint32_t ps = pos_state(pos);
    const auto s = static_cast<std::size_t>(st.state);
    rc->bit(model.is_match[(s << pos_bits_max) + ps], 1);
    rc->bit(model.is_rep[s], 0);
    put_length(model.len, len, ps);
    const std::uint32_t ls = std::min<std::uint32_t>(len - match_min_len, len_to_pos_states - 1);
    const std::uint32_t slot = dist_slot(dist);
    rc->tree(model.pos_slot, ls << 6, 6, slot);
    if (slot >= start_pos_model) {
      const int direct_bits = static_cast<int>(slot >> 1) - 1;
      const std::uint32_t base = (2u | (slot & 1u)) << direct_bits;
      const std::uint32_t reduced = dist - base;
      if (slot < end_pos_model) {
        rc->reverse_tree(model.spec_pos, base - slot, direct_bits, reduced);
      } else {
        rc->direct(reduced >> align_bits, direct_bits - align_bits);
        rc->reverse_tree(model.align, 0, align_bits, reduced & ((1u << align_bits) - 1u));
      }
    }
    st.reps = {dist, st.reps[0], st.reps[1], st.reps[2]};
    st.state = state_after_match(st.state);
    pos += len;
    --len_counter;
    --dist_counter;
  }
  void put_rep(std::uint32_t index, std::uint32_t len) {
    const std::uint32_t ps = pos_state(pos);
    const auto s = static_cast<std::size_t>(st.state);
    rc->bit(model.is_match[(s << pos_bits_max) + ps], 1);
    rc->bit(model.is_rep[s], 1);
    if (index == 0) {
      rc->bit(model.is_rep_g0[s], 0);
      rc->bit(model.is_rep0_long[(s << pos_bits_max) + ps], 1);
    } else {
      rc->bit(model.is_rep_g0[s], 1);
      const std::uint32_t dist = st.reps[index];
      if (index == 1) {
        rc->bit(model.is_rep_g1[s], 0);
      } else {
        rc->bit(model.is_rep_g1[s], 1);
        rc->bit(model.is_rep_g2[s], index - 2);
        if (index == 3) st.reps[3] = st.reps[2];
        st.reps[2] = st.reps[1];
      }
      st.reps[1] = st.reps[0];
      st.reps[0] = dist;
    }
    put_length(model.rep_len, len, ps);
    st.state = state_after_rep(st.state);
    pos += len;
    --len_counter;
  }
  void put_short_rep() {
    const std::uint32_t ps = pos_state(pos);
    const auto s = static_cast<std::size_t>(st.state);
    rc->bit(model.is_match[(s << pos_bits_max) + ps], 1);
    rc->bit(model.is_rep[s], 1);
    rc->bit(model.is_rep_g0[s], 0);
    rc->bit(model.is_rep0_long[(s << pos_bits_max) + ps], 0);
    st.state = state_after_short_rep(st.state);
    ++pos;
  }
  void put_end_marker() {
    const std::uint32_t ps = pos_state(pos);
    const auto s = static_cast<std::size_t>(st.state);
    rc->bit(model.is_match[(s << pos_bits_max) + ps], 1);
    rc->bit(model.is_rep[s], 0);
    put_length(model.len, match_min_len, ps);
    rc->tree(model.pos_slot, 0, 6, 63);
    rc->direct((1u << 26) - 1u, 26);
    rc->reverse_tree(model.align, 0, align_bits, 15);
  }

  std::uint32_t match_len_at(std::size_t at, std::uint32_t dist, std::uint32_t avail) const {
    if (at <= dist) return 0;
    const std::uint8_t* a = data + at;
    const std::uint8_t* b = data + at - dist - 1;
    std::uint32_t len = 0;
    while (len < avail && a[len] == b[len]) ++len;
    return len;
  }

  // ---- fast parsing -----------------------------------------------------------
  void step_fast(std::size_t limit) {
    find_at(pos, limit, matches);
    const auto avail = static_cast<std::uint32_t>(std::min<std::size_t>(limit - pos, match_max_len));
    std::uint32_t rep_len = 0;
    std::uint32_t rep_index = 0;
    if (pos > 0) {
      for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t l = match_len_at(pos, st.reps[i], avail);
        if (l > rep_len) {
          rep_len = l;
          rep_index = i;
        }
      }
    }
    std::uint32_t main_len = matches.empty() ? 0 : matches.back().len;
    std::uint32_t main_dist = matches.empty() ? 0 : matches.back().dist;
    // A short match far away costs more than literals.
    if (main_len == 2 && main_dist >= 0x80) main_len = 0;
    if (main_len == 3 && main_dist >= (1u << 15)) main_len = 0;
    if (rep_len >= 2 && rep_len + 1 >= main_len) {
      const std::size_t start = pos;
      put_rep(rep_index, rep_len);
      while (finder.position() < start + rep_len) finder.skip();
      return;
    }
    if (main_len >= 2) {
      const std::size_t start = pos;
      put_match(main_dist, main_len);
      while (finder.position() < start + main_len) finder.skip();
      return;
    }
    if (pos > st.reps[0] && data[pos] == data[pos - st.reps[0] - 1] && st.state >= 7) {
      put_short_rep();
      return;
    }
    put_literal();
  }

  // ---- optimal parsing --------------------------------------------------------
  void step_optimal(std::size_t limit) {
    const std::size_t start = pos;
    find_at(start, limit, matches);
    const auto avail = static_cast<std::uint32_t>(std::min<std::size_t>(limit - start, match_max_len));
    if (avail < 2) {
      put_literal();
      return;
    }
    std::array<std::uint32_t, 4> rep_lens{};
    std::uint32_t best_rep = 0;
    for (std::uint32_t i = 0; i < 4; ++i) {
      rep_lens[i] = match_len_at(start, st.reps[i], avail);
      if (rep_lens[i] > rep_lens[best_rep]) best_rep = i;
    }
    const auto nice = static_cast<std::uint32_t>(p.nice_len);
    if (rep_lens[best_rep] >= nice) {
      put_rep(best_rep, rep_lens[best_rep]);
      while (finder.position() < pos) finder.skip();
      return;
    }
    const std::uint32_t main_len = matches.empty() ? 0 : matches.back().len;
    if (main_len >= nice) {
      put_match(matches.back().dist, main_len);
      while (finder.position() < pos) finder.skip();
      return;
    }
    if (main_len < 2 && rep_lens[best_rep] < 2) {
      if (st.state >= 7 && start > st.reps[0] && data[start] == data[start - st.reps[0] - 1] &&
          short_rep_price(st.state, pos_state(start)) <
              price0(model.is_match[(static_cast<std::size_t>(st.state) << pos_bits_max) + pos_state(start)]) +
                  literal_price(start, st.state, st.reps[0])) {
        put_short_rep();
      } else {
        put_literal();
      }
      return;
    }
    if (len_counter <= 0) update_len_prices();
    if (dist_counter <= 0) update_dist_prices();

    // Node 0 is the current position.
    Node& root = opt[0];
    root.price = 0;
    root.state = st.state;
    root.reps = st.reps;
    std::uint32_t end = std::max(main_len, rep_lens[best_rep]);
    for (std::uint32_t i = 1; i <= end; ++i) opt[i].price = infinity_price;
    relax_from(0, start, matches, rep_lens, end);

    std::uint32_t cur = 0;
    std::vector<Match> cur_matches;
    while (true) {
      ++cur;
      if (cur >= end) break;
      Node& node = opt[cur];
      // Derive the state at this node from its predecessor.
      const Node& prev = opt[node.prev];
      node.reps = prev.reps;
      if (node.back == literal_back) {
        node.state = state_after_literal(prev.state);
      } else if (node.back == short_rep_back) {
        node.state = state_after_short_rep(prev.state);
      } else if (node.back < 4) {
        node.state = state_after_rep(prev.state);
        const std::uint32_t d = prev.reps[node.back];
        for (std::uint32_t k = node.back; k > 0; --k) node.reps[k] = node.reps[k - 1];
        node.reps[0] = d;
      } else {
        node.state = state_after_match(prev.state);
        node.reps = {node.back - 4, prev.reps[0], prev.reps[1], prev.reps[2]};
      }
      const std::size_t at = start + cur;
      find_at(at, limit, cur_matches);
      const auto cur_avail = static_cast<std::uint32_t>(std::min<std::size_t>(limit - at, match_max_len));
      if (!cur_matches.empty() && cur_matches.back().len >= nice) {
        // A long match starts here: settle the path up to this node first.
        break;
      }
      if (cur_avail == 0) break;
      std::array<std::uint32_t, 4> lens{};
      for (std::uint32_t i = 0; i < 4; ++i) lens[i] = cur_avail >= 2 ? match_len_at(at, node.reps[i], cur_avail) : 0;
      std::uint32_t reach = cur + std::max<std::uint32_t>(1, std::max({lens[0], lens[1], lens[2], lens[3],
                                                                      cur_matches.empty() ? 0u : cur_matches.back().len}));
      if (reach > opt_max) {
        break;
      }
      for (std::uint32_t i = end + 1; i <= reach; ++i) opt[i].price = infinity_price;
      end = std::max(end, reach);
      relax_from(cur, at, cur_matches, lens, end);
      if (cur >= opt_max - match_max_len - 1) break;
    }
    // Walk back from `cur` (the furthest settled node) and emit the path.
    std::uint32_t node_index = std::min(cur, end);
    std::vector<std::uint32_t> path;
    while (node_index != 0) {
      path.push_back(node_index);
      node_index = opt[node_index].prev;
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      const Node& n = opt[*it];
      if (n.back == literal_back) {
        put_literal();
      } else if (n.back == short_rep_back) {
        put_short_rep();
      } else if (n.back < 4) {
        put_rep(n.back, n.len);
      } else {
        put_match(n.back - 4, n.len);
      }
    }
    while (finder.position() < pos) finder.skip();
  }

  void relax_from(std::uint32_t cur, std::size_t at, const std::vector<Match>& found,
                  const std::array<std::uint32_t, 4>& lens, std::uint32_t /*end*/) {
    const Node& node = opt[cur];
    const std::uint32_t ps = pos_state(at);
    const auto s = static_cast<std::size_t>(node.state);
    const std::uint32_t base = node.price;
    // literal / short rep
    {
      const std::uint32_t lit = base + price0(model.is_match[(s << pos_bits_max) + ps]) + literal_price(at, node.state, node.reps[0]);
      Node& next = opt[cur + 1];
      if (lit < next.price) {
        next.price = lit;
        next.prev = cur;
        next.back = literal_back;
        next.len = 1;
      }
      if (at > node.reps[0] && data[at] == data[at - node.reps[0] - 1]) {
        const std::uint32_t sr = base + short_rep_price(node.state, ps);
        if (sr < next.price) {
          next.price = sr;
          next.prev = cur;
          next.back = short_rep_back;
          next.len = 1;
        }
      }
    }
    // repeat matches
    for (std::uint32_t i = 0; i < 4; ++i) {
      if (lens[i] < 2) continue;
      const std::uint32_t rp = base + rep_price(i, node.state, ps);
      for (std::uint32_t l = 2; l <= lens[i]; ++l) {
        const std::uint32_t price = rp + rep_len_prices[ps][l];
        Node& target = opt[cur + l];
        if (price < target.price) {
          target.price = price;
          target.prev = cur;
          target.back = i;
          target.len = l;
        }
      }
    }
    // normal matches
    const std::uint32_t mp = base + price1(model.is_match[(s << pos_bits_max) + ps]) + price0(model.is_rep[s]);
    std::uint32_t l = 2;
    for (const Match& m : found) {
      for (; l <= m.len; ++l) {
        const std::uint32_t price = mp + len_prices[ps][l] + match_dist_price(m.dist, l);
        Node& target = opt[cur + l];
        if (price < target.price) {
          target.price = price;
          target.prev = cur;
          target.back = m.dist + 4;
          target.len = l;
        }
      }
    }
  }
};

Encoder::Encoder(ByteView data, const EncoderParams& params) : impl_(std::make_unique<Impl>(data, params)) {}
Encoder::~Encoder() = default;

void Encoder::reset_state() { impl_->reset_state(); }
std::size_t Encoder::position() const noexcept { return impl_->pos; }
bool Encoder::at_end() const noexcept { return impl_->pos >= impl_->size; }

Result<std::size_t> Encoder::encode(Bytes& out, std::size_t max_in, std::size_t max_out, bool end_marker,
                                    const Progress& progress) {
  Impl& m = *impl_;
  RangeEncoder rc(out);
  m.rc = &rc;
  const std::size_t base_out = out.size();
  const std::size_t start = m.pos;
  const std::size_t limit = std::min(m.size, start + max_in);
  std::uint64_t steps = 0;
  while (m.pos < limit) {
    if (max_out != 0 && rc.pending() - base_out + 6000 > max_out) break;
    if ((++steps & 0x3FFu) == 0 && !progress.report(static_cast<double>(m.pos) / static_cast<double>(m.size))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    if (m.p.optimal) {
      m.step_optimal(limit);
    } else {
      m.step_fast(limit);
    }
  }
  if (end_marker && m.pos >= m.size) m.put_end_marker();
  rc.flush();
  m.rc = nullptr;
  return m.pos - start;
}

}  // namespace cvt::compression::detail::lzma
