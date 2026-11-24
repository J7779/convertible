// Convertible core - archive family: PPMd var.H (Method::Ppmd7 decoder and
// encoder, and the RAR flavour behind RarPpmdDecoder).
//
// Memory layout (all "refs" are 32-bit byte offsets into one heap):
//   [align][ text area ... ->       <- units area: state arrays / contexts ][guard unit]
//   context (12 bytes): u16 NumStats, u16 SummFreq, u32 Stats, u32 Suffix;
//                       a binary context (NumStats 1) keeps its single state
//                       in place of SummFreq/Stats (bytes 2..7).
//   state   (6 bytes):  u8 Symbol, u8 Freq, u32 Successor (context or text ref).
// Multi-byte fields are stored in host order with memcpy; nothing depends on
// the byte order except through the model itself.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

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
constexpr std::uint32_t kMinMemory = 1u << 11;
constexpr std::uint32_t kMaxMemory = 0xFFFFFFFFu - 12u * 3u;
/// Largest model this build allocates (Status::Limit above it).
constexpr std::uint64_t kMemoryCeiling = std::uint64_t{1} << 31;

struct See {
  std::uint16_t summ = 0;
  std::uint8_t shift = 0;
  std::uint8_t count = 0;
};

/// Mean of a SEE context (and its update).
inline std::uint32_t see_mean(See& see) {
  const unsigned r = see.summ >> see.shift;
  see.summ = static_cast<std::uint16_t>(see.summ - r);
  return r + (r == 0 ? 1u : 0u);
}

inline void see_update(See& see) {
  if (see.shift < kPeriodBits && --see.count == 0) {
    see.summ = static_cast<std::uint16_t>(see.summ << 1);
    see.count = static_cast<std::uint8_t>(3u << see.shift++);
  }
}

inline std::uint16_t bin_hit(std::uint16_t prob) {
  return static_cast<std::uint16_t>(prob + (1u << 7) - ((prob + 32u) >> 7));
}
inline std::uint16_t bin_miss(std::uint16_t prob) { return static_cast<std::uint16_t>(prob - ((prob + 32u) >> 7)); }

// ------------------------------------------------------------------ range coders

/// Bounded byte input shared by the decoders; reading past the end yields
/// zeros and is reported as truncation by the caller.
struct ByteInput {
  const std::uint8_t* p = nullptr;
  std::size_t size = 0;
  std::size_t pos = 0;
  std::size_t overrun = 0;
  PpmdByteSource* source = nullptr;  // RAR: bytes come from the caller instead

  std::uint8_t next() {
    if (source != nullptr) return source->next_byte();
    if (pos < size) return p[pos++];
    ++overrun;
    return 0;
  }
};

/// The 7z range decoder (LZMA style, with carry propagation on the encoder side).
struct RangeDecoder7z {
  std::uint32_t range = 0xFFFFFFFFu;
  std::uint32_t code = 0;
  ByteInput* in = nullptr;

  bool init(ByteInput& input) {
    in = &input;
    range = 0xFFFFFFFFu;
    code = 0;
    if (in->next() != 0) return false;
    for (int i = 0; i < 4; ++i) code = (code << 8) | in->next();
    return code < 0xFFFFFFFFu;
  }
  std::uint32_t threshold(std::uint32_t total) { return code / (range /= total); }
  void normalize() {
    while (range < kTop) {
      code = (code << 8) | in->next();
      range <<= 8;
    }
  }
  void decode(std::uint32_t start, std::uint32_t size) {
    code -= start * range;
    range *= size;
    normalize();
  }
  /// Binary decision with total 2^14: 0 when the code lies below `size0`.
  unsigned decode_bit(std::uint32_t size0) {
    const std::uint32_t bound = (range >> 14) * size0;
    unsigned bit = 0;
    if (code < bound) {
      range = bound;
    } else {
      bit = 1;
      code -= bound;
      range -= bound;
    }
    normalize();
    return bit;
  }
};

/// Shkarin's original carry-less coder, as RAR uses it.
struct RangeDecoderRar {
  std::uint32_t low = 0;
  std::uint32_t range = 0xFFFFFFFFu;
  std::uint32_t code = 0;
  ByteInput* in = nullptr;

  void init(ByteInput& input) {
    in = &input;
    low = 0;
    code = 0;
    range = 0xFFFFFFFFu;
    for (int i = 0; i < 4; ++i) code = (code << 8) | in->next();
  }
  std::uint32_t threshold(std::uint32_t total) { return (code - low) / (range /= total); }
  void decode(std::uint32_t start, std::uint32_t size) {
    low += start * range;
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
  unsigned decode_bit(std::uint32_t size0) {
    const std::uint32_t count = threshold(kBinScale);
    if (count < size0) {
      decode(0, size0);
      return 0;
    }
    decode(size0, kBinScale - size0);
    return 1;
  }
};

/// The 7z range encoder.
struct RangeEncoder7z {
  std::uint64_t low = 0;
  std::uint32_t range = 0xFFFFFFFFu;
  std::uint8_t cache = 0;
  std::uint64_t cache_size = 1;
  Bytes* out = nullptr;

  void shift_low() {
    if (static_cast<std::uint32_t>(low) < 0xFF000000u || (low >> 32) != 0) {
      std::uint8_t temp = cache;
      do {
        out->push_back(static_cast<std::byte>(static_cast<std::uint8_t>(temp + static_cast<std::uint8_t>(low >> 32))));
        temp = 0xFF;
      } while (--cache_size != 0);
      cache = static_cast<std::uint8_t>(static_cast<std::uint32_t>(low) >> 24);
    }
    ++cache_size;
    low = static_cast<std::uint32_t>(low) << 8;
  }
  void normalize() {
    while (range < kTop) {
      range <<= 8;
      shift_low();
    }
  }
  void encode(std::uint32_t start, std::uint32_t size, std::uint32_t total) {
    low += static_cast<std::uint64_t>(start) * (range /= total);
    range *= size;
    normalize();
  }
  void encode_bit0(std::uint32_t size0) {
    range = (range >> 14) * size0;
    normalize();
  }
  void encode_bit1(std::uint32_t size0) {
    const std::uint32_t bound = (range >> 14) * size0;
    low += bound;
    range -= bound;
    normalize();
  }
  void flush() {
    for (int i = 0; i < 5; ++i) shift_low();
  }
};

// ------------------------------------------------------------------ the model

class ModelH {
 public:
  /// When set, the model runs in a heap smaller than the stream declares and
  /// flags `probe_failed` the moment its behaviour could start to differ from
  /// the full-size model (free gap or text area exhausted). Until then both
  /// models make identical decisions, so a short stream never needs the full
  /// declared memory.
  bool probe = false;
  bool probe_failed = false;

  /// Allocate `size` bytes of model memory (keeps an existing heap of that size).
  [[nodiscard]] bool allocate(std::uint32_t size) {
    if (heap_ && size_ == size) return true;
    heap_.reset();
    size_ = 0;
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

  void init(unsigned max_order) {
    max_order_ = max_order;
    restart();
    dummy_see_.shift = kPeriodBits;
    dummy_see_.summ = 0;
    dummy_see_.count = 64;
  }

  /// 0..255, -1 for the end marker (escape below order 0), -2 for corrupt data.
  template <class Rc>
  int decode_symbol(Rc& rc);
  /// `symbol` -1 writes the end marker.
  void encode_symbol(RangeEncoder7z& rc, int symbol);

  ModelH() {
    ns2bsindx_[0] = 0;
    ns2bsindx_[1] = 2;
    for (unsigned i = 2; i < 11; ++i) ns2bsindx_[i] = 4;
    for (unsigned i = 11; i < 256; ++i) ns2bsindx_[i] = 6;
    unsigned i = 0;
    for (; i < 3; ++i) ns2indx_[i] = static_cast<std::uint8_t>(i);
    for (unsigned m = i, k = 1; i < 256; ++i) {
      ns2indx_[i] = static_cast<std::uint8_t>(m);
      if (--k == 0) k = (++m) - 2;
    }
    for (unsigned j = 0; j < 256; ++j) hb2flag_[j] = j < 0x40 ? 0 : 8;
  }

 private:
  // ---- raw field access
  [[nodiscard]] std::uint16_t ld16(std::uint32_t at) const {
    std::uint16_t v;
    std::memcpy(&v, b_ + at, 2);
    return v;
  }
  void st16(std::uint32_t at, unsigned v) {
    const auto x = static_cast<std::uint16_t>(v);
    std::memcpy(b_ + at, &x, 2);
  }
  [[nodiscard]] std::uint32_t ld32(std::uint32_t at) const {
    std::uint32_t v;
    std::memcpy(&v, b_ + at, 4);
    return v;
  }
  void st32(std::uint32_t at, std::uint32_t v) { std::memcpy(b_ + at, &v, 4); }

  // context fields
  [[nodiscard]] unsigned num_stats(std::uint32_t c) const { return ld16(c); }
  void set_num_stats(std::uint32_t c, unsigned v) { st16(c, v); }
  [[nodiscard]] unsigned summ_freq(std::uint32_t c) const { return ld16(c + 2); }
  void set_summ_freq(std::uint32_t c, unsigned v) { st16(c + 2, v); }
  [[nodiscard]] std::uint32_t stats(std::uint32_t c) const { return ld32(c + 4); }
  void set_stats(std::uint32_t c, std::uint32_t v) { st32(c + 4, v); }
  [[nodiscard]] std::uint32_t suffix(std::uint32_t c) const { return ld32(c + 8); }
  void set_suffix(std::uint32_t c, std::uint32_t v) { st32(c + 8, v); }
  static std::uint32_t one_state(std::uint32_t c) { return c + 2; }

  // state fields
  [[nodiscard]] unsigned sym(std::uint32_t s) const { return b_[s]; }
  [[nodiscard]] unsigned freq(std::uint32_t s) const { return b_[s + 1]; }
  void set_freq(std::uint32_t s, unsigned v) { b_[s + 1] = static_cast<std::uint8_t>(v); }
  [[nodiscard]] std::uint32_t succ(std::uint32_t s) const { return ld32(s + 2); }
  void set_succ(std::uint32_t s, std::uint32_t v) { st32(s + 2, v); }
  void copy_state(std::uint32_t to, std::uint32_t from) { std::memmove(b_ + to, b_ + from, 6); }
  void swap_states(std::uint32_t a, std::uint32_t c) {
    std::uint8_t t[6];
    std::memcpy(t, b_ + a, 6);
    std::memcpy(b_ + a, b_ + c, 6);
    std::memcpy(b_ + c, t, 6);
  }

  // ---- sub-allocator
  static unsigned i2u(unsigned indx) { return ppmd_units.indx2units[indx]; }
  static unsigned u2i(unsigned nu) { return ppmd_units.units2indx[nu - 1]; }
  static std::uint32_t u2b(unsigned nu) { return nu * kUnit; }

  void insert_node(std::uint32_t node, unsigned indx) {
    st32(node, free_list_[indx]);
    free_list_[indx] = node;
  }
  std::uint32_t remove_node(unsigned indx) {
    const std::uint32_t node = free_list_[indx];
    free_list_[indx] = ld32(node);
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

  // Free-block nodes during gluing (12 bytes): u16 Stamp (0 = free), u16 NU, u32 Next, u32 Prev.
  void glue_free_blocks() {
    const std::uint32_t head = align_ + size_;
    std::uint32_t n = head;
    glue_count_ = 255;
    for (unsigned i = 0; i < kNumIndexes; ++i) {
      const unsigned nu = i2u(i);
      std::uint32_t next = free_list_[i];
      free_list_[i] = 0;
      while (next != 0) {
        const std::uint32_t node = next;
        next = ld32(node);
        st32(node + 4, n);   // Next
        st32(n + 8, node);   // Prev of the previous node
        n = node;
        st16(node, 0);       // Stamp
        st16(node + 2, nu);  // NU
      }
    }
    st16(head, 1);
    st32(head + 4, n);
    st32(n + 8, head);
    if (lo_unit_ != hi_unit_) st16(lo_unit_, 1);

    while (n != head) {
      const std::uint32_t node = n;
      std::uint32_t nu = ld16(node + 2);
      for (;;) {
        const std::uint32_t node2 = node + nu * kUnit;
        nu += ld16(node2 + 2);
        if (ld16(node2) != 0 || nu >= 0x10000) break;
        const std::uint32_t prev2 = ld32(node2 + 8);
        const std::uint32_t next2 = ld32(node2 + 4);
        st32(prev2 + 4, next2);
        st32(next2 + 8, prev2);
        st16(node + 2, nu);
      }
      n = ld32(node + 4);
    }

    for (n = ld32(head + 4); n != head;) {
      std::uint32_t node = n;
      const std::uint32_t next = ld32(node + 4);
      unsigned nu = ld16(node + 2);
      for (; nu > 128; nu -= 128, node += 128 * kUnit) insert_node(node, kNumIndexes - 1);
      unsigned i = u2i(nu);
      if (i2u(i) != nu) {
        const unsigned k = i2u(--i);
        insert_node(node + k * kUnit, nu - k - 1);
      }
      insert_node(node, i);
      n = next;
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
      std::memmove(b_ + ptr, b_ + old_ptr, u2b(new_nu));
      insert_node(old_ptr, i0);
      return ptr;
    }
    split_block(old_ptr, i0, i1);
    return old_ptr;
  }

  // ---- model
  void restart() {
    free_list_.fill(0);
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
    set_num_stats(min_ctx_, 256);
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

    for (unsigned i = 0; i < 128; ++i) {
      for (unsigned k = 0; k < 8; ++k) {
        const auto val = static_cast<std::uint16_t>(kBinScale - ppmd_init_bin_esc[k] / (i + 2));
        for (unsigned m = 0; m < 64; m += 8) bin_summ_[i][k + m] = val;
      }
    }
    for (unsigned i = 0; i < 25; ++i) {
      for (See& s : see_[i]) {
        s.shift = kPeriodBits - 4;
        s.summ = static_cast<std::uint16_t>((5 * i + 10) << s.shift);
        s.count = 4;
      }
    }
  }

  std::uint32_t create_successors(bool skip) {
    std::uint32_t c = min_ctx_;
    const std::uint32_t up_branch = succ(found_);
    std::array<std::uint32_t, 64> ps;
    unsigned num_ps = 0;
    if (!skip) ps[num_ps++] = found_;
    const unsigned fsym = sym(found_);
    while (suffix(c) != 0) {
      c = suffix(c);
      std::uint32_t s;
      if (num_stats(c) != 1) {
        s = stats(c);
        while (sym(s) != fsym) s += 6;
      } else {
        s = one_state(c);
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
    std::memcpy(up_state + 2, &up_succ, 4);
    if (num_stats(c) == 1) {
      up_state[1] = static_cast<std::uint8_t>(freq(one_state(c)));
    } else {
      std::uint32_t s = stats(c);
      while (sym(s) != up_state[0]) s += 6;
      const std::uint32_t cf = freq(s) - 1;
      const std::uint32_t s0 = summ_freq(c) - num_stats(c) - cf;
      up_state[1] = static_cast<std::uint8_t>(
          1 + ((2 * cf <= s0) ? (5 * cf > s0 ? 1u : 0u) : ((2 * cf + 3 * s0 - 1) / (2 * s0))));
    }
    do {
      const std::uint32_t c1 = alloc_context();
      if (c1 == 0) return 0;
      set_num_stats(c1, 1);
      std::memcpy(b_ + one_state(c1), up_state, 6);
      set_suffix(c1, c);
      set_succ(ps[--num_ps], c1);
      c = c1;
    } while (num_ps != 0);
    return c;
  }

  void update_model() {
    std::uint32_t f_successor = succ(found_);
    const unsigned fsym = sym(found_);
    if (freq(found_) < kMaxFreq / 4 && suffix(min_ctx_) != 0) {
      const std::uint32_t c = suffix(min_ctx_);
      if (num_stats(c) == 1) {
        const std::uint32_t s = one_state(c);
        if (freq(s) < 32) set_freq(s, freq(s) + 1);
      } else {
        std::uint32_t s = stats(c);
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

    if (order_fall_ == 0) {
      min_ctx_ = max_ctx_ = create_successors(true);
      if (min_ctx_ == 0) {
        restart_after_overflow();
        return;
      }
      set_succ(found_, min_ctx_);
      return;
    }

    b_[text_++] = static_cast<std::uint8_t>(fsym);
    std::uint32_t successor = text_;
    if (text_ >= units_start_) {
      restart_after_overflow();
      return;
    }

    if (f_successor != 0) {
      if (f_successor <= successor) {
        const std::uint32_t cs = create_successors(false);
        if (cs == 0) {
          restart_after_overflow();
          return;
        }
        f_successor = cs;
      }
      if (--order_fall_ == 0) {
        successor = f_successor;
        text_ -= (max_ctx_ != min_ctx_) ? 1u : 0u;
      }
    } else {
      set_succ(found_, successor);
      f_successor = min_ctx_;
    }

    const unsigned ns = num_stats(min_ctx_);
    const unsigned ffreq = freq(found_);
    const unsigned s0 = summ_freq(min_ctx_) - ns - (ffreq - 1);

    for (std::uint32_t c = max_ctx_; c != min_ctx_; c = suffix(c)) {
      const unsigned ns1 = num_stats(c);
      if (ns1 != 1) {
        if ((ns1 & 1) == 0) {
          const unsigned old_nu = ns1 >> 1;
          const unsigned i = u2i(old_nu);
          if (i != u2i(old_nu + 1)) {
            const std::uint32_t ptr = alloc_units(i + 1);
            if (ptr == 0) {
              restart_after_overflow();
              return;
            }
            const std::uint32_t old_ptr = stats(c);
            std::memmove(b_ + ptr, b_ + old_ptr, u2b(old_nu));
            insert_node(old_ptr, i);
            set_stats(c, ptr);
          }
        }
        const unsigned sf = summ_freq(c);
        set_summ_freq(c, sf + (2 * ns1 < ns ? 1u : 0u) + 2 * ((4 * ns1 <= ns ? 1u : 0u) & (sf <= 8 * ns1 ? 1u : 0u)));
      } else {
        const std::uint32_t s = alloc_units(0);
        if (s == 0) {
          restart_after_overflow();
          return;
        }
        copy_state(s, one_state(c));
        set_stats(c, s);
        unsigned f = freq(s);
        f = f < kMaxFreq / 4 - 1 ? f << 1 : kMaxFreq - 4;
        set_freq(s, f);
        set_summ_freq(c, f + init_esc_ + (ns > 3 ? 1u : 0u));
      }
      std::uint32_t cf = 2 * ffreq * (summ_freq(c) + 6);
      const std::uint32_t sf = s0 + summ_freq(c);
      if (cf < 6 * sf) {
        cf = 1 + (cf > sf ? 1u : 0u) + (cf >= 4 * sf ? 1u : 0u);
        set_summ_freq(c, summ_freq(c) + 3);
      } else {
        cf = 4 + (cf >= 9 * sf ? 1u : 0u) + (cf >= 12 * sf ? 1u : 0u) + (cf >= 15 * sf ? 1u : 0u);
        set_summ_freq(c, summ_freq(c) + cf);
      }
      const std::uint32_t s = stats(c) + ns1 * 6;
      set_succ(s, successor);
      b_[s] = static_cast<std::uint8_t>(fsym);
      b_[s + 1] = static_cast<std::uint8_t>(cf);
      set_num_stats(c, ns1 + 1);
    }
    max_ctx_ = min_ctx_ = f_successor;
  }

  void restart_after_overflow() {
    if (probe) probe_failed = true;
    restart();
  }

  void rescale() {
    const std::uint32_t st = stats(min_ctx_);
    std::uint32_t s = found_;
    if (s != st) {
      std::uint8_t tmp[6];
      std::memcpy(tmp, b_ + s, 6);
      for (; s != st; s -= 6) copy_state(s, s - 6);
      std::memcpy(b_ + s, tmp, 6);
    }
    unsigned esc_freq = summ_freq(min_ctx_) - freq(s);
    const unsigned adder = order_fall_ != 0 ? 1u : 0u;
    set_freq(s, (freq(s) + 4 + adder) >> 1);
    unsigned sum_freq = freq(s);
    unsigned i = num_stats(min_ctx_) - 1;
    do {
      s += 6;
      esc_freq -= freq(s);
      set_freq(s, (freq(s) + adder) >> 1);
      sum_freq += freq(s);
      if (freq(s) > freq(s - 6)) {
        std::uint32_t s1 = s;
        std::uint8_t tmp[6];
        std::memcpy(tmp, b_ + s1, 6);
        do {
          copy_state(s1, s1 - 6);
          s1 -= 6;
        } while (s1 != st && tmp[1] > freq(s1 - 6));
        std::memcpy(b_ + s1, tmp, 6);
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
      if (num_stats(min_ctx_) == 1) {
        std::uint8_t tmp[6];
        std::memcpy(tmp, b_ + st, 6);
        do {
          tmp[1] = static_cast<std::uint8_t>(tmp[1] - (tmp[1] >> 1));
          esc_freq >>= 1;
        } while (esc_freq > 1);
        insert_node(st, u2i((num_stats_old + 1) >> 1));
        found_ = one_state(min_ctx_);
        std::memcpy(b_ + found_, tmp, 6);
        return;
      }
      const unsigned n0 = (num_stats_old + 1) >> 1;
      const unsigned n1 = (num_stats(min_ctx_) + 1) >> 1;
      if (n0 != n1) set_stats(min_ctx_, shrink_units(st, n0, n1));
    }
    set_summ_freq(min_ctx_, sum_freq + esc_freq - (esc_freq >> 1));
    found_ = stats(min_ctx_);
  }

  void next_context() {
    const std::uint32_t c = succ(found_);
    if (order_fall_ == 0 && c > text_) {
      min_ctx_ = max_ctx_ = c;
    } else {
      update_model();
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
    prev_success_ = 2 * freq(found_) > summ_freq(min_ctx_) ? 1u : 0u;
    run_length_ += static_cast<std::int32_t>(prev_success_);
    set_summ_freq(min_ctx_, summ_freq(min_ctx_) + 4);
    set_freq(found_, freq(found_) + 4);
    if (freq(found_) > kMaxFreq) rescale();
    next_context();
  }

  void update_bin() {
    set_freq(found_, freq(found_) + (freq(found_) < 128 ? 1u : 0u));
    prev_success_ = 1;
    ++run_length_;
    next_context();
  }

  void update2() {
    set_freq(found_, freq(found_) + 4);
    set_summ_freq(min_ctx_, summ_freq(min_ctx_) + 4);
    if (freq(found_) > kMaxFreq) rescale();
    run_length_ = init_rl_;
    update_model();
  }

  std::uint16_t& bin_summ() {
    const std::uint32_t one = one_state(min_ctx_);
    hi_bits_flag_ = hb2flag_[sym(found_)];
    return bin_summ_[freq(one) - 1][prev_success_ + ns2bsindx_[num_stats(suffix(min_ctx_)) - 1] + hi_bits_flag_ +
                                    2u * hb2flag_[sym(one)] +
                                    ((static_cast<std::uint32_t>(run_length_) >> 26) & 0x20u)];
  }

  See& make_esc_freq(unsigned num_masked, std::uint32_t& esc_freq) {
    const unsigned ns = num_stats(min_ctx_);
    if (ns == 256) {
      esc_freq = 1;
      return dummy_see_;
    }
    const unsigned non_masked = ns - num_masked;
    See& see = see_[ns2indx_[non_masked - 1]]
                   [(non_masked < num_stats(suffix(min_ctx_)) - ns ? 1u : 0u) +
                    2u * (summ_freq(min_ctx_) < 11 * ns ? 1u : 0u) + 4u * (num_masked > non_masked ? 1u : 0u) +
                    hi_bits_flag_];
    esc_freq = see_mean(see);
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
  unsigned hi_bits_flag_ = 0;
  std::int32_t run_length_ = 0;
  std::int32_t init_rl_ = 0;

  std::uint32_t text_ = 0;
  std::uint32_t units_start_ = 0;
  std::uint32_t lo_unit_ = 0;
  std::uint32_t hi_unit_ = 0;
  unsigned glue_count_ = 0;
  std::array<std::uint32_t, kNumIndexes> free_list_{};

  std::array<std::uint8_t, 256> ns2indx_{};
  std::array<std::uint8_t, 256> ns2bsindx_{};
  std::array<std::uint8_t, 256> hb2flag_{};
  See dummy_see_{};
  std::array<std::array<See, 16>, 25> see_{};
  std::array<std::array<std::uint16_t, 64>, 128> bin_summ_{};
};

template <class Rc>
int ModelH::decode_symbol(Rc& rc) {
  std::array<std::uint8_t, 256> mask;  // 0xFF = still possible, 0 = excluded
  if (num_stats(min_ctx_) != 1) {
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
    unsigned i = num_stats(min_ctx_) - 1;
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
    hi_bits_flag_ = hb2flag_[sym(found_)];
    rc.decode(hi, total - hi);
    mask.fill(0xFF);
    mask[sym(s)] = 0;
    i = num_stats(min_ctx_) - 1;
    do {
      s -= 6;
      mask[sym(s)] = 0;
    } while (--i != 0);
  } else {
    std::uint16_t& prob = bin_summ();
    if (rc.decode_bit(prob) == 0) {
      prob = bin_hit(prob);
      found_ = one_state(min_ctx_);
      const int symbol = static_cast<int>(sym(found_));
      update_bin();
      return symbol;
    }
    prob = bin_miss(prob);
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
    for (unsigned k = 0; k < ns && n != num; ++k, s += 6) {
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
      see_update(see);
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

void ModelH::encode_symbol(RangeEncoder7z& rc, int symbol) {
  std::array<std::uint8_t, 256> mask;
  if (num_stats(min_ctx_) != 1) {
    std::uint32_t s = stats(min_ctx_);
    const unsigned total = summ_freq(min_ctx_);
    if (static_cast<int>(sym(s)) == symbol) {
      rc.encode(0, freq(s), total);
      found_ = s;
      update1_0();
      return;
    }
    prev_success_ = 0;
    std::uint32_t sum = freq(s);
    unsigned i = num_stats(min_ctx_) - 1;
    do {
      s += 6;
      if (static_cast<int>(sym(s)) == symbol) {
        rc.encode(sum, freq(s), total);
        found_ = s;
        update1();
        return;
      }
      sum += freq(s);
    } while (--i != 0);
    hi_bits_flag_ = hb2flag_[sym(found_)];
    mask.fill(0xFF);
    mask[sym(s)] = 0;
    i = num_stats(min_ctx_) - 1;
    do {
      s -= 6;
      mask[sym(s)] = 0;
    } while (--i != 0);
    rc.encode(sum, total - sum, total);
  } else {
    std::uint16_t& prob = bin_summ();
    const std::uint32_t s = one_state(min_ctx_);
    if (static_cast<int>(sym(s)) == symbol) {
      rc.encode_bit0(prob);
      prob = bin_hit(prob);
      found_ = s;
      update_bin();
      return;
    }
    rc.encode_bit1(prob);
    prob = bin_miss(prob);
    init_esc_ = ppmd_exp_escape[prob >> 10];
    mask.fill(0xFF);
    mask[sym(s)] = 0;
    prev_success_ = 0;
  }
  for (;;) {
    const unsigned num_masked = num_stats(min_ctx_);
    do {
      ++order_fall_;
      if (suffix(min_ctx_) == 0) return;  // end marker written
      min_ctx_ = suffix(min_ctx_);
    } while (num_stats(min_ctx_) == num_masked);
    std::uint32_t esc_freq = 0;
    See& see = make_esc_freq(num_masked, esc_freq);
    std::uint32_t s = stats(min_ctx_);
    std::uint32_t sum = 0;
    unsigned i = num_stats(min_ctx_);
    do {
      const unsigned cur = sym(s);
      if (static_cast<int>(cur) == symbol) {
        const std::uint32_t low = sum;
        const std::uint32_t s1 = s;
        do {
          if (mask[sym(s)] != 0) sum += freq(s);
          s += 6;
        } while (--i != 0);
        rc.encode(low, freq(s1), sum + esc_freq);
        see_update(see);
        found_ = s1;
        update2();
        return;
      }
      if (mask[cur] != 0) sum += freq(s);
      mask[cur] = 0;
      s += 6;
    } while (--i != 0);
    rc.encode(sum, esc_freq, sum + esc_freq);
    see.summ = static_cast<std::uint16_t>(see.summ + sum + esc_freq);
  }
}

// ------------------------------------------------------------------ 7z codec

struct Props7 {
  unsigned order = 0;
  std::uint32_t memory = 0;
};

Result<Props7> parse_props(ByteView props) {
  if (props.size() != 5) return fail(Status::Decode, "PPMd properties must be 5 bytes, not {}", props.size());
  Props7 p;
  p.order = std::to_integer<unsigned>(props[0]);
  for (std::size_t i = 0; i < 4; ++i) p.memory |= std::to_integer<std::uint32_t>(props[1 + i]) << (8 * i);
  if (p.order < 2 || p.order > 64) return fail(Status::Decode, "PPMd model order {} is outside 2..64", p.order);
  if (p.memory < kMinMemory || p.memory > kMaxMemory) {
    return fail(Status::Decode, "PPMd model size {} bytes is invalid", p.memory);
  }
  return p;
}

/// Model memory for a probing run: what `n` symbols can plausibly need.
std::uint32_t probe_memory(std::uint32_t declared, std::uint64_t n) {
  const std::uint64_t guess = (std::uint64_t{1} << 20) + n * 64;
  if (guess >= declared) return declared;
  return static_cast<std::uint32_t>(guess);
}

enum class RunResult { Ok, ProbeFailed };

Result<RunResult> run_decode7(ModelH& model, unsigned order, ByteView data, std::uint64_t count, bool until_end,
                              Bytes& out, const DecodeOptions& options) {
  model.init(order);
  ByteInput in;
  in.p = reinterpret_cast<const std::uint8_t*>(data.data());
  in.size = data.size();
  RangeDecoder7z rc;
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

Result<Bytes> ppmd7_decode(ByteView data, const DecodeOptions& options) { return ppmd7_decode_probed(data, options, 0); }

Result<Bytes> ppmd7_decode_probed(ByteView data, const DecodeOptions& options, std::uint32_t probe_bytes) {
  CVT_TRY_ASSIGN(const Props7 props, parse_props(options.properties));
  bool until_end = !options.output_size.has_value();
  std::uint64_t count = options.output_size.value_or(0);
  if (!until_end) CVT_TRY(check_output_room(0, count, options.max_output));
  if (options.stop_after != 0 && (until_end || options.stop_after < count)) {
    count = options.stop_after;
    until_end = false;
  }
  Bytes out;
  if (!until_end && count == 0) return out;
  ModelH model;
  // A first run in a heap sized for the output; the full declared size only
  // when the stream really needs it.
  std::uint32_t probe = until_end ? props.memory : probe_memory(props.memory, count);
  if (probe_bytes != 0) probe = std::clamp(probe_bytes, kMinMemory, props.memory);
  if (probe < props.memory) {
    if (!model.allocate(probe)) return fail(Status::Limit, "not enough memory for the PPMd model");
    model.probe = true;
    CVT_TRY_ASSIGN(const RunResult r, run_decode7(model, props.order, data, count, until_end, out, options));
    if (r == RunResult::Ok) return out;
    model = ModelH();
  }
  if (props.memory > kMemoryCeiling) {
    return fail(Status::Limit, "the PPMd model needs {} MiB of memory", props.memory >> 20);
  }
  if (!model.allocate(props.memory)) {
    return fail(Status::Limit, "not enough memory for the {} MiB PPMd model", props.memory >> 20);
  }
  CVT_TRY_ASSIGN(const RunResult r, run_decode7(model, props.order, data, count, until_end, out, options));
  (void)r;
  return out;
}

namespace {

struct EncodeChoice {
  unsigned order = 6;
  std::uint32_t memory = 16u << 20;
};

/// Level mapping (7-Zip's orders): orders 3,4,4,5,5,6,8,16,24,32 for levels
/// 0..9; memory 2^(level+19) bytes (192 MiB at 9), reduced for small inputs to
/// the power of two of at least 16x the input (64x from level 7 on, where the
/// long contexts need the room; not below 64 KiB).
EncodeChoice choose(const EncodeOptions& options, std::uint64_t input_size) {
  static constexpr std::array<unsigned, 10> orders = {3, 4, 4, 5, 5, 6, 8, 16, 24, 32};
  const int level = std::clamp(options.level, 0, 9);
  EncodeChoice c;
  c.order = orders[static_cast<std::size_t>(level)];
  if (options.dictionary_size != 0) {
    c.memory = std::clamp<std::uint32_t>(options.dictionary_size, 1u << 16, kMaxMemory);
    return c;
  }
  c.memory = level >= 9 ? (192u << 20) : (1u << (level + 19));
  const unsigned mult = level >= 7 ? 64 : 16;
  if (c.memory / mult > input_size) {
    for (unsigned i = 16; i <= 31; ++i) {
      const std::uint32_t m = 1u << i;
      if (input_size <= m / mult) {
        c.memory = std::min(c.memory, m);
        break;
      }
    }
  }
  return c;
}

}  // namespace

Result<Bytes> ppmd7_properties(const EncodeOptions& options, std::uint64_t input_size) {
  const EncodeChoice c = choose(options, input_size);
  Bytes p(5);
  p[0] = static_cast<std::byte>(c.order);
  for (std::size_t i = 0; i < 4; ++i) p[1 + i] = static_cast<std::byte>((c.memory >> (8 * i)) & 0xFFu);
  return p;
}

Result<Bytes> ppmd7_encode(ByteView data, const EncodeOptions& options) {
  const EncodeChoice c = choose(options, data.size());
  ModelH model;
  if (!model.allocate(c.memory)) return fail(Status::Limit, "not enough memory for the PPMd model");
  model.init(c.order);
  Bytes out;
  out.reserve(data.size() / 3 + 16);
  RangeEncoder7z rc;
  rc.out = &out;
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  for (std::size_t i = 0; i < data.size(); ++i) {
    if ((i & 0x3FFF) == 0x3FFF &&
        !options.progress.report(static_cast<double>(i) / static_cast<double>(data.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    model.encode_symbol(rc, p[i]);
  }
  rc.flush();
  return out;
}

// ------------------------------------------------------------------ RAR

struct RarPpmdDecoder::Impl {
  ModelH model;
  ByteInput input;
  RangeDecoderRar rc;
  bool ready = false;
  bool coder = false;
};

RarPpmdDecoder::RarPpmdDecoder() : impl_(std::make_unique<Impl>()) {}
RarPpmdDecoder::~RarPpmdDecoder() = default;

Result<void> RarPpmdDecoder::restart(int max_order, std::uint32_t memory_bytes) {
  impl_->ready = false;
  if (max_order < 2 || max_order > 64) return fail(Status::InvalidArgument, "PPMd order {} is outside 2..64", max_order);
  if (memory_bytes < kMinMemory || memory_bytes > (256u << 20)) {
    return fail(Status::InvalidArgument, "PPMd model size {} bytes is outside 2 KiB..256 MiB", memory_bytes);
  }
  if (!impl_->model.allocate(memory_bytes)) return fail(Status::Limit, "not enough memory for the PPMd model");
  impl_->model.init(static_cast<unsigned>(max_order));
  impl_->ready = true;
  return {};
}

bool RarPpmdDecoder::has_model() const noexcept { return impl_->ready; }

Result<void> RarPpmdDecoder::init_range_coder(PpmdByteSource& source) {
  impl_->input = ByteInput{};
  impl_->input.source = &source;
  impl_->rc.init(impl_->input);
  impl_->coder = true;
  return {};
}

Result<std::uint8_t> RarPpmdDecoder::decode_symbol() {
  if (!impl_->ready || !impl_->coder) return fail(Status::Decode, "PPMd block without a model");
  const int c = impl_->model.decode_symbol(impl_->rc);
  if (c < 0) {
    impl_->ready = false;  // the model state is unusable after an error
    return fail(Status::Decode, "PPMd data is corrupt");
  }
  return static_cast<std::uint8_t>(c);
}

}  // namespace cvt::compression::detail
