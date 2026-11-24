// Convertible core - archive family: PPMd var.I revision 1 (Method::Ppmd8,
// ZIP method 98) decoder.
//
// Differences from var.H (ppmd_h.cpp): contexts store NumStats - 1 in one byte
// next to a Flags byte (0x04 rescaled, 0x08 holds a symbol >= 0x40, 0x10 was
// reached through a symbol >= 0x40), SEE and binary contexts are indexed
// differently, and a model that runs out of memory either restarts or is
// pruned ("cut off") depending on the restoration method in the stream.
//
// Memory layout as in ppmd_h.cpp, except context = u8 NumStats, u8 Flags,
// u16 SummFreq, u32 Stats, u32 Suffix, and free nodes = u32 Stamp
// (0xFFFFFFFF), u32 Next, u32 NU.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "util/bytes.hpp"
#include "families/archive/codecs/ppmd.hpp"

namespace cvt::compression::detail {

namespace {

constexpr std::uint32_t kUnit = 12;
constexpr unsigned kNumIndexes = 38;
constexpr unsigned kMaxFreq = 124;
constexpr unsigned kPeriodBits = 7;
constexpr unsigned kBinScale = 1u << 14;
constexpr std::uint32_t kTop = 1u << 24;
constexpr std::uint32_t kBot = 1u << 15;
constexpr std::uint32_t kEmptyNode = 0xFFFFFFFFu;
constexpr unsigned kRestoreRestart = 0;
constexpr unsigned kRestoreCutOff = 1;
constexpr std::uint64_t kMemoryCeiling = std::uint64_t{1} << 31;

struct See {
  std::uint16_t summ = 0;
  std::uint8_t shift = 0;
  std::uint8_t count = 0;
};

struct Input {
  const std::uint8_t* p = nullptr;
  std::size_t size = 0;
  std::size_t pos = 0;
  std::size_t overrun = 0;
  std::uint8_t next() {
    if (pos < size) return p[pos++];
    ++overrun;
    return 0;
  }
};

/// var.I range decoder (carry-less; `code` is kept relative to `low`).
struct RangeDecoder {
  std::uint32_t low = 0;
  std::uint32_t range = 0xFFFFFFFFu;
  std::uint32_t code = 0;
  Input* in = nullptr;

  bool init(Input& input) {
    in = &input;
    low = 0;
    range = 0xFFFFFFFFu;
    code = 0;
    for (int i = 0; i < 4; ++i) code = (code << 8) | in->next();
    return code < 0xFFFFFFFFu;
  }
  std::uint32_t threshold(std::uint32_t total) { return code / (range /= total); }
  void decode(std::uint32_t start, std::uint32_t size) {
    start *= range;
    low += start;
    code -= start;
    range *= size;
    for (;;) {
      if ((low ^ (low + range)) >= kTop) {
        if (range >= kBot) return;
        range = (0u - low) & (kBot - 1);
      }
      code = (code << 8) | in->next();
      range <<= 8;
      low <<= 8;
    }
  }
};

class ModelI {
 public:
  bool probe = false;
  bool probe_failed = false;

  [[nodiscard]] bool allocate(std::uint32_t size) {
    heap_.reset();
    const std::uint32_t align = 4u - (size & 3u);
    const std::uint64_t total = std::uint64_t{align} + size + kUnit;
    if (total > std::numeric_limits<std::size_t>::max()) return false;
    heap_ = ppmd_alloc(static_cast<std::size_t>(total));
    if (!heap_) return false;
    b_ = heap_.get();
    size_ = size;
    align_ = align;
    return true;
  }

  void init(unsigned max_order, unsigned restore_method) {
    max_order_ = max_order;
    restore_method_ = restore_method;
    restart();
    dummy_see_.shift = kPeriodBits;
    dummy_see_.summ = 0;
    dummy_see_.count = 64;
  }

  int decode_symbol(RangeDecoder& rc);

  ModelI() {
    ns2bsindx_[0] = 0;
    ns2bsindx_[1] = 2;
    for (unsigned i = 2; i < 11; ++i) ns2bsindx_[i] = 4;
    for (unsigned i = 11; i < 256; ++i) ns2bsindx_[i] = 6;
    unsigned i = 0;
    for (; i < 5; ++i) ns2indx_[i] = static_cast<std::uint8_t>(i);
    for (unsigned m = i, k = 1; i < 260; ++i) {
      ns2indx_[i] = static_cast<std::uint8_t>(m);
      if (--k == 0) k = (++m) - 4;
    }
  }

 private:
  [[nodiscard]] std::uint16_t ld16(std::uint32_t at) const {
    std::uint16_t v;
    ::cvt::util::copy_bytes(&v, b_ + at, 2);
    return v;
  }
  void st16(std::uint32_t at, unsigned v) {
    const auto x = static_cast<std::uint16_t>(v);
    ::cvt::util::copy_bytes(b_ + at, &x, 2);
  }
  [[nodiscard]] std::uint32_t ld32(std::uint32_t at) const {
    std::uint32_t v;
    ::cvt::util::copy_bytes(&v, b_ + at, 4);
    return v;
  }
  void st32(std::uint32_t at, std::uint32_t v) { ::cvt::util::copy_bytes(b_ + at, &v, 4); }

  // context: NumStats is the number of symbols minus one (0 = binary context)
  [[nodiscard]] unsigned num_stats(std::uint32_t c) const { return b_[c]; }
  void set_num_stats(std::uint32_t c, unsigned v) { b_[c] = static_cast<std::uint8_t>(v); }
  [[nodiscard]] unsigned flags(std::uint32_t c) const { return b_[c + 1]; }
  void set_flags(std::uint32_t c, unsigned v) { b_[c + 1] = static_cast<std::uint8_t>(v); }
  [[nodiscard]] unsigned summ_freq(std::uint32_t c) const { return ld16(c + 2); }
  void set_summ_freq(std::uint32_t c, unsigned v) { st16(c + 2, v); }
  [[nodiscard]] std::uint32_t stats(std::uint32_t c) const { return ld32(c + 4); }
  void set_stats(std::uint32_t c, std::uint32_t v) { st32(c + 4, v); }
  [[nodiscard]] std::uint32_t suffix(std::uint32_t c) const { return ld32(c + 8); }
  void set_suffix(std::uint32_t c, std::uint32_t v) { st32(c + 8, v); }
  static std::uint32_t one_state(std::uint32_t c) { return c + 2; }

  [[nodiscard]] unsigned sym(std::uint32_t s) const { return b_[s]; }
  [[nodiscard]] unsigned freq(std::uint32_t s) const { return b_[s + 1]; }
  void set_freq(std::uint32_t s, unsigned v) { b_[s + 1] = static_cast<std::uint8_t>(v); }
  [[nodiscard]] std::uint32_t succ(std::uint32_t s) const { return ld32(s + 2); }
  void set_succ(std::uint32_t s, std::uint32_t v) { st32(s + 2, v); }
  void copy_state(std::uint32_t to, std::uint32_t from) { ::cvt::util::move_bytes(b_ + to, b_ + from, 6); }
  void swap_states(std::uint32_t a, std::uint32_t c) {
    std::uint8_t t[6];
    ::cvt::util::copy_bytes(t, b_ + a, 6);
    ::cvt::util::copy_bytes(b_ + a, b_ + c, 6);
    ::cvt::util::copy_bytes(b_ + c, t, 6);
  }
  static unsigned hi_flag(unsigned symbol) { return symbol >= 0x40 ? 1u : 0u; }

  // ---- sub-allocator
  static unsigned i2u(unsigned indx) { return ppmd_units.indx2units[indx]; }
  static unsigned u2i(unsigned nu) { return ppmd_units.units2indx[nu - 1]; }
  static std::uint32_t u2b(unsigned nu) { return nu * kUnit; }

  void insert_node(std::uint32_t node, unsigned indx) {
    st32(node, kEmptyNode);
    st32(node + 4, free_list_[indx]);
    st32(node + 8, i2u(indx));
    free_list_[indx] = node;
    ++stamps_[indx];
  }
  std::uint32_t remove_node(unsigned indx) {
    const std::uint32_t node = free_list_[indx];
    free_list_[indx] = ld32(node + 4);
    --stamps_[indx];
    return node;
  }
  void split_block(std::uint32_t ptr, unsigned old_indx, unsigned new_indx) {
    const unsigned nu = i2u(old_indx) - i2u(new_indx);
    ptr += u2b(i2u(new_indx));
    unsigned i = u2i(nu);
    if (i2u(i) != nu) {
      const unsigned k = i2u(--i);
      insert_node(ptr + u2b(k), nu - k - 1);
    }
    insert_node(ptr, i);
  }

  void glue_free_blocks() {
    std::uint32_t head = 0;
    std::uint32_t prev = 0;  // 0 = `head` itself, otherwise the node whose Next field links on
    glue_count_ = 1u << 13;
    stamps_.fill(0);
    if (lo_unit_ != hi_unit_) st32(lo_unit_, 0);

    for (unsigned i = 0; i < kNumIndexes; ++i) {
      std::uint32_t next = free_list_[i];
      free_list_[i] = 0;
      while (next != 0) {
        const std::uint32_t node = next;
        if (ld32(node + 8) != 0) {
          if (prev == 0) {
            head = node;
          } else {
            st32(prev + 4, node);
          }
          prev = node;
          for (;;) {
            const std::uint32_t node2 = node + ld32(node + 8) * kUnit;
            if (ld32(node2) != kEmptyNode) break;
            st32(node + 8, ld32(node + 8) + ld32(node2 + 8));
            st32(node2 + 8, 0);
          }
        }
        next = ld32(node + 4);
      }
    }
    if (prev == 0) {
      head = 0;
    } else {
      st32(prev + 4, 0);
    }

    while (head != 0) {
      std::uint32_t node = head;
      head = ld32(node + 4);
      std::uint32_t nu = ld32(node + 8);
      if (nu == 0) continue;
      for (; nu > 128; nu -= 128, node += 128 * kUnit) insert_node(node, kNumIndexes - 1);
      unsigned i = u2i(nu);
      if (i2u(i) != nu) {
        const unsigned k = i2u(--i);
        insert_node(node + k * kUnit, nu - k - 1);
      }
      insert_node(node, i);
    }
  }

  std::uint32_t alloc_units_rare(unsigned indx) {
    if (glue_count_ == 0) {
      glue_free_blocks();
      if (free_list_[indx] != 0) return remove_node(indx);
    }
    unsigned i = indx;
    do {
      if (++i == kNumIndexes) {
        const std::uint32_t num_bytes = u2b(i2u(indx));
        --glue_count_;
        if (units_start_ - text_ > num_bytes) {
          units_start_ -= num_bytes;
          return units_start_;
        }
        return 0;
      }
    } while (free_list_[i] == 0);
    const std::uint32_t r = remove_node(i);
    split_block(r, i, indx);
    return r;
  }

  std::uint32_t alloc_units(unsigned indx) {
    if (free_list_[indx] != 0) return remove_node(indx);
    const std::uint32_t num_bytes = u2b(i2u(indx));
    if (num_bytes <= hi_unit_ - lo_unit_) {
      const std::uint32_t r = lo_unit_;
      lo_unit_ += num_bytes;
      return r;
    }
    if (probe) probe_failed = true;
    return alloc_units_rare(indx);
  }

  std::uint32_t alloc_context() {
    if (hi_unit_ != lo_unit_) return hi_unit_ -= kUnit;
    if (probe) probe_failed = true;
    if (free_list_[0] != 0) return remove_node(0);
    return alloc_units_rare(0);
  }

  std::uint32_t shrink_units(std::uint32_t old_ptr, unsigned old_nu, unsigned new_nu) {
    const unsigned i0 = u2i(old_nu);
    const unsigned i1 = u2i(new_nu);
    if (i0 == i1) return old_ptr;
    if (free_list_[i1] != 0) {
      const std::uint32_t ptr = remove_node(i1);
      ::cvt::util::move_bytes(b_ + ptr, b_ + old_ptr, u2b(new_nu));
      insert_node(old_ptr, i0);
      return ptr;
    }
    split_block(old_ptr, i0, i1);
    return old_ptr;
  }

  void free_units(std::uint32_t ptr, unsigned nu) { insert_node(ptr, u2i(nu)); }

  void special_free_unit(std::uint32_t ptr) {
    if (ptr != units_start_) {
      insert_node(ptr, 0);
    } else {
      units_start_ += kUnit;
    }
  }

  std::uint32_t move_units_up(std::uint32_t old_ptr, unsigned nu) {
    const unsigned indx = u2i(nu);
    if (old_ptr > units_start_ + 16 * 1024 || old_ptr > free_list_[indx]) return old_ptr;
    const std::uint32_t ptr = remove_node(indx);
    ::cvt::util::move_bytes(b_ + ptr, b_ + old_ptr, u2b(nu));
    if (old_ptr != units_start_) {
      insert_node(old_ptr, indx);
    } else {
      units_start_ += u2b(i2u(indx));
    }
    return ptr;
  }

  void expand_text_area() {
    std::array<std::uint32_t, kNumIndexes> count{};
    if (lo_unit_ != hi_unit_) st32(lo_unit_, 0);
    {
      std::uint32_t node = units_start_;
      while (ld32(node) == kEmptyNode) {
        st32(node, 0);
        const std::uint32_t nu = ld32(node + 8);
        if (nu == 0 || nu > 128) break;  // cannot happen: listed nodes carry their class size
        ++count[u2i(nu)];
        node += nu * kUnit;
      }
      units_start_ = node;
    }
    for (unsigned i = 0; i < kNumIndexes; ++i) {
      std::uint32_t link = 0;  // 0 = free_list_[i], otherwise the node whose Next field links on
      while (count[i] != 0) {
        std::uint32_t node = link == 0 ? free_list_[i] : ld32(link + 4);
        while (ld32(node) == 0) {
          node = ld32(node + 4);
          if (link == 0) {
            free_list_[i] = node;
          } else {
            st32(link + 4, node);
          }
          --stamps_[i];
          if (--count[i] == 0) break;
        }
        link = node;
      }
    }
  }

  [[nodiscard]] std::uint32_t used_memory() const {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < kNumIndexes; ++i) v += stamps_[i] * i2u(i);
    return size_ - (hi_unit_ - lo_unit_) - (units_start_ - text_) - u2b(v);
  }

  // ---- model
  void restart() {
    free_list_.fill(0);
    stamps_.fill(0);
    text_ = align_;
    hi_unit_ = text_ + size_;
    lo_unit_ = units_start_ = hi_unit_ - size_ / 8 / kUnit * 7 * kUnit;
    glue_count_ = 0;

    order_fall_ = max_order_;
    run_length_ = init_rl_ = -static_cast<std::int32_t>(max_order_ < 12 ? max_order_ : 12) - 1;
    prev_success_ = 0;

    hi_unit_ -= kUnit;
    min_ctx_ = max_ctx_ = hi_unit_;
    set_suffix(min_ctx_, 0);
    set_num_stats(min_ctx_, 255);
    set_flags(min_ctx_, 0);
    set_summ_freq(min_ctx_, 256 + 1);
    found_ = lo_unit_;
    set_stats(min_ctx_, lo_unit_);
    for (unsigned i = 0; i < 256; ++i) {
      const std::uint32_t s = lo_unit_ + i * 6;
      b_[s] = static_cast<std::uint8_t>(i);
      b_[s + 1] = 1;
      set_succ(s, 0);
    }
    lo_unit_ += u2b(256 / 2);

    for (unsigned i = 0, m = 0; m < 25; ++m) {
      while (ns2indx_[i] == m) ++i;
      for (unsigned k = 0; k < 8; ++k) {
        const auto val = static_cast<std::uint16_t>(kBinScale - ppmd_init_bin_esc[k] / (i + 1));
        for (unsigned r = 0; r < 64; r += 8) bin_summ_[m][k + r] = val;
      }
    }
    for (unsigned i = 0, m = 0; m < 24; ++m) {
      while (ns2indx_[i + 3] == m + 3) ++i;
      for (See& s : see_[m]) {
        s.shift = kPeriodBits - 4;
        s.summ = static_cast<std::uint16_t>((2 * i + 5) << s.shift);
        s.count = 7;
      }
    }
  }

  void refresh(std::uint32_t ctx, unsigned old_nu, unsigned scale) {
    unsigned i = num_stats(ctx);
    std::uint32_t s = shrink_units(stats(ctx), old_nu, (i + 2) >> 1);
    set_stats(ctx, s);
    unsigned fl = (flags(ctx) & (0x10u + 0x04u * scale)) + 0x08u * hi_flag(sym(s));
    unsigned esc_freq = summ_freq(ctx) - freq(s);
    set_freq(s, (freq(s) + scale) >> scale);
    unsigned sum_freq = freq(s);
    do {
      s += 6;
      esc_freq -= freq(s);
      set_freq(s, (freq(s) + scale) >> scale);
      sum_freq += freq(s);
      fl |= 0x08u * hi_flag(sym(s));
    } while (--i != 0);
    set_summ_freq(ctx, sum_freq + ((esc_freq + scale) >> scale));
    set_flags(ctx, fl);
  }

  std::uint32_t cut_off(std::uint32_t ctx, unsigned order) {
    if (num_stats(ctx) == 0) {
      const std::uint32_t s = one_state(ctx);
      if (succ(s) >= units_start_) {
        if (order < max_order_) {
          set_succ(s, cut_off(succ(s), order + 1));
        } else {
          set_succ(s, 0);
        }
        if (succ(s) != 0 || order <= 9) return ctx;
      }
      special_free_unit(ctx);
      return 0;
    }
    const unsigned tmp = (num_stats(ctx) + 2) >> 1;
    set_stats(ctx, move_units_up(stats(ctx), tmp));
    int i = static_cast<int>(num_stats(ctx));
    const std::uint32_t st = stats(ctx);
    for (std::uint32_t s = st + static_cast<std::uint32_t>(i) * 6;; s -= 6) {
      if (succ(s) < units_start_) {
        const std::uint32_t s2 = st + static_cast<std::uint32_t>(i--) * 6;
        set_succ(s, 0);
        swap_states(s, s2);
      } else if (order < max_order_) {
        set_succ(s, cut_off(succ(s), order + 1));
      } else {
        set_succ(s, 0);
      }
      if (s == st) break;
    }
    if (i != static_cast<int>(num_stats(ctx)) && order != 0) {
      set_num_stats(ctx, static_cast<unsigned>(i) & 0xFFu);
      const std::uint32_t s = st;
      if (i < 0) {
        free_units(s, tmp);
        special_free_unit(ctx);
        return 0;
      }
      if (i == 0) {
        set_flags(ctx, (flags(ctx) & 0x10u) + 0x08u * hi_flag(sym(s)));
        copy_state(one_state(ctx), s);
        free_units(s, tmp);
        const std::uint32_t one = one_state(ctx);
        set_freq(one, (freq(one) + 11) >> 3);
      } else {
        refresh(ctx, tmp, summ_freq(ctx) > 16u * static_cast<unsigned>(i) ? 1u : 0u);
      }
    }
    return ctx;
  }

  void restore_model(std::uint32_t c1) {
    if (probe) probe_failed = true;
    text_ = align_;
    std::uint32_t c = max_ctx_;
    for (; c != c1; c = suffix(c)) {
      const unsigned ns = num_stats(c) - 1;
      set_num_stats(c, ns);
      if (ns == 0) {
        const std::uint32_t s = stats(c);
        set_flags(c, (flags(c) & 0x10u) + 0x08u * hi_flag(sym(s)));
        copy_state(one_state(c), s);
        special_free_unit(s);
        const std::uint32_t one = one_state(c);
        set_freq(one, (freq(one) + 11) >> 3);
      } else {
        refresh(c, (ns + 3) >> 1, 0);
      }
    }
    for (; c != min_ctx_; c = suffix(c)) {
      if (num_stats(c) == 0) {
        const std::uint32_t one = one_state(c);
        set_freq(one, freq(one) - (freq(one) >> 1));
      } else {
        set_summ_freq(c, summ_freq(c) + 4);
        if (summ_freq(c) > 128 + 4 * num_stats(c)) refresh(c, (num_stats(c) + 2) >> 1, 1);
      }
    }
    if (restore_method_ == kRestoreRestart || used_memory() < (size_ >> 1)) {
      restart();
    } else {
      while (suffix(max_ctx_) != 0) max_ctx_ = suffix(max_ctx_);
      unsigned rounds = 0;
      do {
        cut_off(max_ctx_, 0);
        expand_text_area();
      } while (used_memory() > 3 * (size_ >> 2) && ++rounds < 64);
      glue_count_ = 0;
      order_fall_ = max_order_;
    }
  }

  std::uint32_t create_successors(bool skip, std::uint32_t s1, std::uint32_t c) {
    const std::uint32_t up_branch = succ(found_);
    const unsigned fsym = sym(found_);
    std::array<std::uint32_t, 65> ps;
    unsigned num_ps = 0;
    if (!skip) ps[num_ps++] = found_;
    while (suffix(c) != 0) {
      c = suffix(c);
      std::uint32_t s;
      if (s1 != 0) {
        s = s1;
        s1 = 0;
      } else if (num_stats(c) != 0) {
        s = stats(c);
        while (sym(s) != fsym) s += 6;
        if (freq(s) < kMaxFreq - 9) {
          set_freq(s, freq(s) + 1);
          set_summ_freq(c, summ_freq(c) + 1);
        }
      } else {
        s = one_state(c);
        set_freq(s, freq(s) + ((num_stats(suffix(c)) == 0 ? 1u : 0u) & (freq(s) < 24 ? 1u : 0u)));
      }
      const std::uint32_t successor = succ(s);
      if (successor != up_branch) {
        c = successor;
        if (num_ps == 0) return c;
        break;
      }
      if (num_ps >= ps.size()) return 0;  // cannot happen in a consistent model
      ps[num_ps++] = s;
    }
    if (num_ps == 0) return c;

    std::uint8_t up_state[6];
    up_state[0] = b_[up_branch];
    const std::uint32_t up_succ = up_branch + 1;
    ::cvt::util::copy_bytes(up_state + 2, &up_succ, 4);
    const unsigned new_flags = 0x10u * hi_flag(fsym) + 0x08u * hi_flag(up_state[0]);
    if (num_stats(c) == 0) {
      up_state[1] = static_cast<std::uint8_t>(freq(one_state(c)));
    } else {
      std::uint32_t s = stats(c);
      while (sym(s) != up_state[0]) s += 6;
      const std::uint32_t cf = freq(s) - 1;
      const std::uint32_t s0 = summ_freq(c) - num_stats(c) - cf;
      up_state[1] = static_cast<std::uint8_t>(1 + ((2 * cf <= s0) ? (5 * cf > s0 ? 1u : 0u) : ((cf + 2 * s0 - 3) / s0)));
    }
    do {
      const std::uint32_t c1 = alloc_context();
      if (c1 == 0) return 0;
      set_num_stats(c1, 0);
      set_flags(c1, new_flags);
      ::cvt::util::copy_bytes(b_ + one_state(c1), up_state, 6);
      set_suffix(c1, c);
      set_succ(ps[--num_ps], c1);
      c = c1;
    } while (num_ps != 0);
    return c;
  }

  std::uint32_t reduce_order(std::uint32_t s1, std::uint32_t c) {
    std::uint32_t s = 0;
    const std::uint32_t c1 = c;
    const std::uint32_t up_branch = text_;
    const unsigned fsym = sym(found_);
    set_succ(found_, up_branch);
    ++order_fall_;
    for (;;) {
      if (s1 != 0) {
        c = suffix(c);
        s = s1;
        s1 = 0;
      } else {
        if (suffix(c) == 0) return c;
        c = suffix(c);
        if (num_stats(c) != 0) {
          s = stats(c);
          while (sym(s) != fsym) s += 6;
          if (freq(s) < kMaxFreq - 9) {
            set_freq(s, freq(s) + 2);
            set_summ_freq(c, summ_freq(c) + 2);
          }
        } else {
          s = one_state(c);
          set_freq(s, freq(s) + (freq(s) < 32 ? 1u : 0u));
        }
      }
      if (succ(s) != 0) break;
      set_succ(s, up_branch);
      ++order_fall_;
    }
    if (succ(s) <= up_branch) {
      const std::uint32_t s2 = found_;
      found_ = s;
      const std::uint32_t successor = create_successors(false, 0, c);
      set_succ(s, successor);
      found_ = s2;
    }
    if (order_fall_ == 1 && c1 == max_ctx_) {
      set_succ(found_, succ(s));
      --text_;
    }
    return succ(s);
  }

  void update_model() {
    std::uint32_t f_successor = succ(found_);
    const unsigned ffreq = freq(found_);
    const unsigned fsym = sym(found_);
    std::uint32_t s = 0;

    if (ffreq < kMaxFreq / 4 && suffix(min_ctx_) != 0) {
      const std::uint32_t c = suffix(min_ctx_);
      if (num_stats(c) == 0) {
        s = one_state(c);
        if (freq(s) < 32) set_freq(s, freq(s) + 1);
      } else {
        s = stats(c);
        if (sym(s) != fsym) {
          do {
            s += 6;
          } while (sym(s) != fsym);
          if (freq(s) >= freq(s - 6)) {
            swap_states(s, s - 6);
            s -= 6;
          }
        }
        if (freq(s) < kMaxFreq - 9) {
          set_freq(s, freq(s) + 2);
          set_summ_freq(c, summ_freq(c) + 2);
        }
      }
    }

    std::uint32_t c = max_ctx_;
    if (order_fall_ == 0 && f_successor != 0) {
      const std::uint32_t cs = create_successors(true, s, min_ctx_);
      if (cs == 0) {
        set_succ(found_, 0);
        restore_model(c);
      } else {
        set_succ(found_, cs);
        max_ctx_ = cs;
      }
      return;
    }

    b_[text_++] = static_cast<std::uint8_t>(fsym);
    std::uint32_t successor = text_;
    if (text_ >= units_start_) {
      restore_model(c);
      return;
    }

    if (f_successor == 0) {
      const std::uint32_t cs = reduce_order(s, min_ctx_);
      if (cs == 0) {
        restore_model(c);
        return;
      }
      f_successor = cs;
    } else if (f_successor < units_start_) {
      const std::uint32_t cs = create_successors(false, s, min_ctx_);
      if (cs == 0) {
        restore_model(c);
        return;
      }
      f_successor = cs;
    }

    if (--order_fall_ == 0) {
      successor = f_successor;
      text_ -= (max_ctx_ != min_ctx_) ? 1u : 0u;
    }

    const unsigned ns = num_stats(min_ctx_);
    const unsigned s0 = summ_freq(min_ctx_) - ns - ffreq;
    const unsigned flag = 0x08u * hi_flag(fsym);

    for (; c != min_ctx_; c = suffix(c)) {
      const unsigned ns1 = num_stats(c);
      if (ns1 != 0) {
        if ((ns1 & 1) != 0) {
          const unsigned old_nu = (ns1 + 1) >> 1;
          const unsigned i = u2i(old_nu);
          if (i != u2i(old_nu + 1)) {
            const std::uint32_t ptr = alloc_units(i + 1);
            if (ptr == 0) {
              restore_model(c);
              return;
            }
            const std::uint32_t old_ptr = stats(c);
            ::cvt::util::move_bytes(b_ + ptr, b_ + old_ptr, u2b(old_nu));
            insert_node(old_ptr, i);
            set_stats(c, ptr);
          }
        }
        set_summ_freq(c, summ_freq(c) + (3 * ns1 + 1 < ns ? 1u : 0u));
      } else {
        const std::uint32_t s2 = alloc_units(0);
        if (s2 == 0) {
          restore_model(c);
          return;
        }
        copy_state(s2, one_state(c));
        set_stats(c, s2);
        unsigned f = freq(s2);
        f = f < kMaxFreq / 4 - 1 ? f << 1 : kMaxFreq - 4;
        set_freq(s2, f);
        set_summ_freq(c, f + init_esc_ + (ns > 2 ? 1u : 0u));
      }
      std::uint32_t cf = 2 * ffreq * (summ_freq(c) + 6);
      const std::uint32_t sf = s0 + summ_freq(c);
      if (cf < 6 * sf) {
        cf = 1 + (cf > sf ? 1u : 0u) + (cf >= 4 * sf ? 1u : 0u);
        set_summ_freq(c, summ_freq(c) + 4);
      } else {
        cf = 4 + (cf > 9 * sf ? 1u : 0u) + (cf > 12 * sf ? 1u : 0u) + (cf > 15 * sf ? 1u : 0u);
        set_summ_freq(c, summ_freq(c) + cf);
      }
      const std::uint32_t s2 = stats(c) + (ns1 + 1) * 6;
      set_succ(s2, successor);
      b_[s2] = static_cast<std::uint8_t>(fsym);
      b_[s2 + 1] = static_cast<std::uint8_t>(cf);
      set_flags(c, flags(c) | flag);
      set_num_stats(c, ns1 + 1);
    }
    max_ctx_ = min_ctx_ = f_successor;
  }

  void rescale() {
    const std::uint32_t st = stats(min_ctx_);
    std::uint32_t s = found_;
    if (s != st) {
      std::uint8_t tmp[6];
      ::cvt::util::copy_bytes(tmp, b_ + s, 6);
      for (; s != st; s -= 6) copy_state(s, s - 6);
      ::cvt::util::copy_bytes(b_ + s, tmp, 6);
    }
    unsigned esc_freq = summ_freq(min_ctx_) - freq(s);
    const unsigned adder = order_fall_ != 0 ? 1u : 0u;
    set_freq(s, (freq(s) + 4 + adder) >> 1);
    unsigned sum_freq = freq(s);
    unsigned i = num_stats(min_ctx_);
    do {
      s += 6;
      esc_freq -= freq(s);
      set_freq(s, (freq(s) + adder) >> 1);
      sum_freq += freq(s);
      if (freq(s) > freq(s - 6)) {
        std::uint32_t s1 = s;
        std::uint8_t tmp[6];
        ::cvt::util::copy_bytes(tmp, b_ + s1, 6);
        do {
          copy_state(s1, s1 - 6);
          s1 -= 6;
        } while (s1 != st && tmp[1] > freq(s1 - 6));
        ::cvt::util::copy_bytes(b_ + s1, tmp, 6);
      }
    } while (--i != 0);

    if (freq(s) == 0) {
      const unsigned num_stats_old = num_stats(min_ctx_);
      i = 0;
      do {
        ++i;
        s -= 6;
      } while (freq(s) == 0);
      esc_freq += i;
      set_num_stats(min_ctx_, num_stats_old - i);
      if (num_stats(min_ctx_) == 0) {
        std::uint8_t tmp[6];
        ::cvt::util::copy_bytes(tmp, b_ + st, 6);
        unsigned f = (2 * tmp[1] + esc_freq - 1) / esc_freq;
        if (f > kMaxFreq / 3) f = kMaxFreq / 3;
        tmp[1] = static_cast<std::uint8_t>(f);
        insert_node(st, u2i((num_stats_old + 2) >> 1));
        set_flags(min_ctx_, (flags(min_ctx_) & 0x10u) + 0x08u * hi_flag(tmp[0]));
        found_ = one_state(min_ctx_);
        ::cvt::util::copy_bytes(b_ + found_, tmp, 6);
        return;
      }
      const unsigned n0 = (num_stats_old + 2) >> 1;
      const unsigned n1 = (num_stats(min_ctx_) + 2) >> 1;
      if (n0 != n1) set_stats(min_ctx_, shrink_units(st, n0, n1));
      unsigned fl = flags(min_ctx_) & ~0x08u;
      std::uint32_t t = stats(min_ctx_);
      fl |= 0x08u * hi_flag(sym(t));
      i = num_stats(min_ctx_);
      do {
        t += 6;
        fl |= 0x08u * hi_flag(sym(t));
      } while (--i != 0);
      set_flags(min_ctx_, fl);
    }
    set_summ_freq(min_ctx_, sum_freq + esc_freq - (esc_freq >> 1));
    set_flags(min_ctx_, flags(min_ctx_) | 0x04u);
    found_ = stats(min_ctx_);
  }

  void next_context() {
    const std::uint32_t c = succ(found_);
    if (order_fall_ == 0 && c >= units_start_) {
      min_ctx_ = max_ctx_ = c;
    } else {
      update_model();
      min_ctx_ = max_ctx_;
    }
  }

  void update1() {
    std::uint32_t s = found_;
    set_freq(s, freq(s) + 4);
    set_summ_freq(min_ctx_, summ_freq(min_ctx_) + 4);
    if (freq(s) > freq(s - 6)) {
      swap_states(s, s - 6);
      found_ = s -= 6;
      if (freq(s) > kMaxFreq) rescale();
    }
    next_context();
  }

  void update1_0() {
    prev_success_ = 2 * freq(found_) >= summ_freq(min_ctx_) ? 1u : 0u;
    run_length_ += static_cast<std::int32_t>(prev_success_);
    set_summ_freq(min_ctx_, summ_freq(min_ctx_) + 4);
    set_freq(found_, freq(found_) + 4);
    if (freq(found_) > kMaxFreq) rescale();
    next_context();
  }

  void update_bin() {
    set_freq(found_, freq(found_) + (freq(found_) < 196 ? 1u : 0u));
    prev_success_ = 1;
    ++run_length_;
    next_context();
  }

  void update2() {
    set_summ_freq(min_ctx_, summ_freq(min_ctx_) + 4);
    set_freq(found_, freq(found_) + 4);
    if (freq(found_) > kMaxFreq) rescale();
    run_length_ = init_rl_;
    update_model();
    min_ctx_ = max_ctx_;
  }

  std::uint16_t& bin_summ() {
    const std::uint32_t one = one_state(min_ctx_);
    // The row and column stay in range in a consistent model (binary contexts
    // never carry flag 0x04, their frequency stays below 197); the clamps only
    // keep a damaged model inside the tables.
    const unsigned row = std::min<unsigned>(ns2indx_[freq(one) - 1], 24);
    const unsigned col = ns2bsindx_[num_stats(suffix(min_ctx_))] + prev_success_ + flags(min_ctx_) +
                         ((static_cast<std::uint32_t>(run_length_) >> 26) & 0x20u);
    return bin_summ_[row][col & 63u];
  }

  See& make_esc_freq(unsigned num_masked, std::uint32_t& esc_freq) {
    const unsigned ns = num_stats(min_ctx_);
    if (ns == 0xFF) {
      esc_freq = 1;
      return dummy_see_;
    }
    See& see = see_[ns2indx_[ns + 2] - 3u][(summ_freq(min_ctx_) > 11 * (ns + 1) ? 1u : 0u) +
                                           2u * (2 * ns < num_stats(suffix(min_ctx_)) + num_masked ? 1u : 0u) +
                                           flags(min_ctx_)];
    const unsigned r = see.summ >> see.shift;
    see.summ = static_cast<std::uint16_t>(see.summ - r);
    esc_freq = r + (r == 0 ? 1u : 0u);
    return see;
  }

  PpmdHeap heap_;
  std::uint8_t* b_ = nullptr;
  std::uint32_t size_ = 0;
  std::uint32_t align_ = 0;

  std::uint32_t min_ctx_ = 0;
  std::uint32_t max_ctx_ = 0;
  std::uint32_t found_ = 0;
  unsigned order_fall_ = 0;
  unsigned init_esc_ = 0;
  unsigned prev_success_ = 0;
  unsigned max_order_ = 0;
  unsigned restore_method_ = 0;
  std::int32_t run_length_ = 0;
  std::int32_t init_rl_ = 0;

  std::uint32_t text_ = 0;
  std::uint32_t units_start_ = 0;
  std::uint32_t lo_unit_ = 0;
  std::uint32_t hi_unit_ = 0;
  unsigned glue_count_ = 0;
  std::array<std::uint32_t, kNumIndexes> free_list_{};
  std::array<std::uint32_t, kNumIndexes> stamps_{};

  std::array<std::uint8_t, 260> ns2indx_{};
  std::array<std::uint8_t, 256> ns2bsindx_{};
  See dummy_see_{};
  std::array<std::array<See, 32>, 24> see_{};
  std::array<std::array<std::uint16_t, 64>, 25> bin_summ_{};
};

int ModelI::decode_symbol(RangeDecoder& rc) {
  std::array<std::uint8_t, 256> mask;  // 0xFF = still possible, 0 = excluded
  if (num_stats(min_ctx_) != 0) {
    std::uint32_t s = stats(min_ctx_);
    const unsigned total = summ_freq(min_ctx_);
    const std::uint32_t count = rc.threshold(total);
    std::uint32_t hi = freq(s);
    if (count < hi) {
      rc.decode(0, hi);
      found_ = s;
      const int symbol = static_cast<int>(sym(s));
      update1_0();
      return symbol;
    }
    prev_success_ = 0;
    unsigned i = num_stats(min_ctx_);
    do {
      s += 6;
      hi += freq(s);
      if (hi > count) {
        rc.decode(hi - freq(s), freq(s));
        found_ = s;
        const int symbol = static_cast<int>(sym(s));
        update1();
        return symbol;
      }
    } while (--i != 0);
    if (count >= total) return -2;
    rc.decode(hi, total - hi);
    mask.fill(0xFF);
    mask[sym(s)] = 0;
    i = num_stats(min_ctx_);
    do {
      s -= 6;
      mask[sym(s)] = 0;
    } while (--i != 0);
  } else {
    std::uint16_t& prob = bin_summ();
    const std::uint32_t count = rc.threshold(kBinScale);
    if (count < prob) {
      rc.decode(0, prob);
      prob = static_cast<std::uint16_t>(prob + (1u << 7) - ((prob + 32u) >> 7));
      found_ = one_state(min_ctx_);
      const int symbol = static_cast<int>(sym(found_));
      update_bin();
      return symbol;
    }
    rc.decode(prob, kBinScale - prob);
    prob = static_cast<std::uint16_t>(prob - ((prob + 32u) >> 7));
    init_esc_ = ppmd_exp_escape[prob >> 10];
    mask.fill(0xFF);
    mask[sym(one_state(min_ctx_))] = 0;
    prev_success_ = 0;
  }
  for (;;) {
    std::array<std::uint32_t, 256> ps;
    const unsigned num_masked = num_stats(min_ctx_);
    do {
      ++order_fall_;
      if (suffix(min_ctx_) == 0) return -1;
      min_ctx_ = suffix(min_ctx_);
    } while (num_stats(min_ctx_) == num_masked);
    const unsigned ns = num_stats(min_ctx_);
    if (ns < num_masked) return -2;  // cannot happen in a consistent model
    std::uint32_t hi = 0;
    std::uint32_t s = stats(min_ctx_);
    const unsigned num = ns - num_masked;
    unsigned n = 0;
    for (unsigned k = 0; k <= ns && n != num; ++k, s += 6) {
      if (mask[sym(s)] != 0) {
        hi += freq(s);
        ps[n++] = s;
      }
    }
    if (n != num) return -2;
    std::uint32_t freq_sum = 0;
    See& see = make_esc_freq(num_masked, freq_sum);
    freq_sum += hi;
    const std::uint32_t count = rc.threshold(freq_sum);
    if (count < hi) {
      std::uint32_t acc = 0;
      unsigned k = 0;
      for (;; ++k) {
        acc += freq(ps[k]);
        if (acc > count) break;
      }
      s = ps[k];
      rc.decode(acc - freq(s), freq(s));
      if (see.shift < kPeriodBits && --see.count == 0) {
        see.summ = static_cast<std::uint16_t>(see.summ << 1);
        see.count = static_cast<std::uint8_t>(3u << see.shift++);
      }
      found_ = s;
      const int symbol = static_cast<int>(sym(s));
      update2();
      return symbol;
    }
    if (count >= freq_sum) return -2;
    rc.decode(hi, freq_sum - hi);
    see.summ = static_cast<std::uint16_t>(see.summ + freq_sum);
    for (unsigned k = 0; k < n; ++k) mask[sym(ps[k])] = 0;
  }
}

enum class RunResult { Ok, ProbeFailed };

Result<RunResult> run_decode(ModelI& model, unsigned order, unsigned restore, ByteView data, std::uint64_t count,
                             bool until_end, Bytes& out, const DecodeOptions& options) {
  model.init(order, restore);
  Input in;
  in.p = reinterpret_cast<const std::uint8_t*>(data.data());
  in.size = data.size();
  RangeDecoder rc;
  if (!rc.init(in)) return fail(Status::Decode, "PPMd data does not start with a valid range coder header");
  out.clear();
  out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, std::uint64_t{1} << 26)));
  for (std::uint64_t i = 0; until_end || i < count; ++i) {
    if ((i & 0x3FFF) == 0x3FFF && !options.progress.report(static_cast<double>(in.pos) / static_cast<double>(in.size))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    const int c = model.decode_symbol(rc);
    if (model.probe_failed) return RunResult::ProbeFailed;
    if (c < 0) {
      if (c == -1 && until_end) break;
      return fail(Status::Decode, c == -1 ? "PPMd data ends before the declared size" : "PPMd data is corrupt");
    }
    if (in.overrun != 0) return fail(Status::Decode, "PPMd data is truncated");
    if (until_end) CVT_TRY(check_output_room(out.size(), 1, options.max_output));
    out.push_back(static_cast<std::byte>(c));
  }
  if (in.overrun != 0) return fail(Status::Decode, "PPMd data is truncated");
  return RunResult::Ok;
}

}  // namespace

Result<Bytes> ppmd8_decode(ByteView data, const DecodeOptions& options) { return ppmd8_decode_probed(data, options, 0); }

Result<Bytes> ppmd8_decode_probed(ByteView data, const DecodeOptions& options, std::uint32_t probe_bytes) {
  if (data.size() < 2) return fail(Status::Decode, "PPMd data is too short for its parameter word");
  const unsigned word = std::to_integer<unsigned>(data[0]) | (std::to_integer<unsigned>(data[1]) << 8);
  const unsigned order = (word & 0x0Fu) + 1;
  const std::uint32_t memory = (((word >> 4) & 0xFFu) + 1) << 20;
  const unsigned restore = word >> 12;
  if (order < 2) return fail(Status::Decode, "PPMd model order {} is below 2", order);
  if (restore > kRestoreCutOff) return fail(Status::Decode, "PPMd restoration method {} is not supported", restore);
  bool until_end = !options.output_size.has_value();
  std::uint64_t count = options.output_size.value_or(0);
  if (!until_end) CVT_TRY(check_output_room(0, count, options.max_output));
  if (options.stop_after != 0 && (until_end || options.stop_after < count)) {
    count = options.stop_after;
    until_end = false;
  }
  Bytes out;
  if (!until_end && count == 0) return out;
  const ByteView payload = data.subspan(2);
  ModelI model;
  if (!until_end) {
    const std::uint64_t guess =
        probe_bytes != 0 ? std::max<std::uint32_t>(probe_bytes, 1u << 11) : (std::uint64_t{1} << 20) + count * 64;
    if (guess < memory) {
      if (!model.allocate(static_cast<std::uint32_t>(guess))) return fail(Status::Limit, "not enough memory for the PPMd model");
      model.probe = true;
      CVT_TRY_ASSIGN(const RunResult r, run_decode(model, order, restore, payload, count, until_end, out, options));
      if (r == RunResult::Ok) return out;
      model = ModelI();
    }
  }
  if (memory > kMemoryCeiling) return fail(Status::Limit, "the PPMd model needs {} MiB of memory", memory >> 20);
  if (!model.allocate(memory)) return fail(Status::Limit, "not enough memory for the {} MiB PPMd model", memory >> 20);
  CVT_TRY_ASSIGN(const RunResult r, run_decode(model, order, restore, payload, count, until_end, out, options));
  (void)r;
  return out;
}

}  // namespace cvt::compression::detail
