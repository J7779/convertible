// Convertible core - archive family: Brotli decoder (RFC 7932) and codec table.
//
// The decoder works one meta-block at a time. The incremental decoder keeps
// the unread input of the current meta-block buffered and retries once it
// has grown enough (a meta-block whose input is incomplete is rolled back),
// plus the sliding window of output that back-references can reach.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/brotli_internal.hpp"
#include "families/archive/codecs/huffman.hpp"

namespace cvt::compression::detail {

Result<Bytes> brotli_encode(ByteView data, const EncodeOptions& options);  // brotli_encoder.cpp


namespace {

namespace br = brotli;

// LSB-first bit reader with raw byte access (uncompressed meta-blocks).
// Reading past the end yields zero bits and sets `overrun()`.
class Bits {
 public:
  Bits(const std::uint8_t* data, std::size_t size, std::uint64_t bit_pos) noexcept : data_(data), size_(size) {
    pos_ = static_cast<std::size_t>(std::min<std::uint64_t>(bit_pos / 8, size));
    if (bit_pos / 8 <= size) skip(static_cast<int>(bit_pos % 8));
  }
  [[nodiscard]] std::uint32_t peek(int n) noexcept {
    if (bits_ < n) refill();
    return static_cast<std::uint32_t>(buffer_ & ((std::uint64_t{1} << n) - 1u));
  }
  void skip(int n) noexcept {
    if (bits_ < n) refill();
    buffer_ >>= n;
    bits_ -= n;
  }
  [[nodiscard]] std::uint32_t get(int n) noexcept {
    if (n == 0) return 0;
    const std::uint32_t v = peek(n);
    buffer_ >>= n;
    bits_ -= n;
    return v;
  }
  [[nodiscard]] std::uint64_t position() const noexcept {
    return std::uint64_t{pos_} * 8 + std::uint64_t{over_} * 8 - static_cast<std::uint64_t>(bits_);
  }
  [[nodiscard]] bool overrun() const noexcept { return position() > std::uint64_t{size_} * 8; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  /// Skip to the next byte boundary; returns the skipped bits.
  std::uint32_t align() noexcept { return get(bits_ % 8); }
  /// After `align()`: skip `n` raw bytes, copying the first `copy` of them to
  /// `out` (false, with `overrun()` set, when they are not all there).
  bool raw(std::uint8_t* out, std::size_t copy, std::uint64_t n) noexcept {
    const std::uint64_t byte = position() / 8;
    if (byte > size_ || n > size_ - byte) {
      over_ += 1;
      return false;
    }
    if (copy != 0) std::memcpy(out, data_ + byte, copy);
    pos_ = static_cast<std::size_t>(byte + n);
    over_ = 0;
    buffer_ = 0;
    bits_ = 0;
    return true;
  }

 private:
  void refill() noexcept {
    if (pos_ + 8 <= size_) {
      std::uint64_t v = 0;
      for (int i = 7; i >= 0; --i) v = (v << 8) | data_[pos_ + static_cast<std::size_t>(i)];
      buffer_ |= v << bits_;
      const int take = (63 - bits_) >> 3;
      pos_ += static_cast<std::size_t>(take);
      bits_ += take * 8;
      return;
    }
    while (bits_ <= 56) {
      if (pos_ < size_) {
        buffer_ |= static_cast<std::uint64_t>(data_[pos_]) << bits_;
        ++pos_;
      } else {
        ++over_;
      }
      bits_ += 8;
    }
  }
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  std::uint32_t over_ = 0;  // zero bytes supplied past the end
  std::uint64_t buffer_ = 0;
  int bits_ = 0;
};

struct PrefixCode {
  HuffmanTable table;
  int single = -1;
  [[nodiscard]] int decode(Bits& r) const { return single >= 0 ? single : table.decode(r); }
};

Error corrupt(const char* what) { return Error{Status::Decode, std::string("Brotli stream is corrupt: ") + what}; }

// Static code for the code length code lengths (RFC 7932 section 3.5), indexed by the next 4 bits.
constexpr std::array<std::uint8_t, 16> cl_prefix_length = {2, 2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 4};
constexpr std::array<std::uint8_t, 16> cl_prefix_value = {0, 4, 3, 2, 0, 4, 3, 1, 0, 4, 3, 2, 0, 4, 3, 5};

Result<void> read_prefix_code(Bits& r, std::size_t alphabet, PrefixCode& code) {
  code.single = -1;
  const std::uint32_t hskip = r.get(2);
  std::vector<std::uint8_t> lengths(alphabet, 0);
  if (hskip == 1) {
    const int nsym = static_cast<int>(r.get(2)) + 1;
    const int bits = std::bit_width(alphabet - 1);
    std::array<std::uint32_t, 4> sym{};
    for (int i = 0; i < nsym; ++i) {
      sym[static_cast<std::size_t>(i)] = r.get(bits);
      if (sym[static_cast<std::size_t>(i)] >= alphabet) return fail(corrupt("prefix code symbol out of range"));
      for (int j = 0; j < i; ++j) {
        if (sym[static_cast<std::size_t>(j)] == sym[static_cast<std::size_t>(i)]) {
          return fail(corrupt("duplicate symbol in a simple prefix code"));
        }
      }
    }
    if (nsym == 1) {
      code.single = static_cast<int>(sym[0]);
      return {};
    }
    static constexpr std::array<std::array<std::uint8_t, 4>, 5> shapes = {
        {{1, 1, 0, 0}, {1, 2, 2, 0}, {2, 2, 2, 2}, {1, 2, 3, 3}, {0, 0, 0, 0}}};
    std::size_t shape = static_cast<std::size_t>(nsym - 2);
    if (nsym == 4 && r.get(1) != 0) shape = 3;
    for (int i = 0; i < nsym; ++i) lengths[sym[static_cast<std::size_t>(i)]] = shapes[shape][static_cast<std::size_t>(i)];
  } else {
    std::array<std::uint8_t, 18> cl{};
    int space = 32;
    int used = 0;
    for (std::size_t i = hskip; i < 18; ++i) {
      const std::uint32_t v = r.peek(4);
      r.skip(cl_prefix_length[v]);
      const std::uint8_t len = cl_prefix_value[v];
      cl[br::code_length_order[i]] = len;
      if (len != 0) {
        space -= 32 >> len;
        ++used;
        if (space <= 0) break;
      }
    }
    if (!(used == 1 || space == 0)) return fail(corrupt("invalid code length code"));
    PrefixCode clcode;
    if (used == 1) {
      for (std::size_t i = 0; i < 18; ++i) {
        if (cl[i] != 0) clcode.single = static_cast<int>(i);
      }
    } else if (!clcode.table.build(cl, true, true)) {
      return fail(corrupt("invalid code length code"));
    }
    std::size_t symbol = 0;
    int prev = 8;
    int repeat = 0;
    int repeat_len = 0;
    std::int32_t left = 32768;
    while (symbol < alphabet && left > 0) {
      const int c = clcode.decode(r);
      if (c < 0) return fail(corrupt("invalid code length"));
      if (c < 16) {
        repeat = 0;
        lengths[symbol++] = static_cast<std::uint8_t>(c);
        if (c != 0) {
          prev = c;
          left -= 32768 >> c;
        }
      } else {
        const int extra = c == 16 ? 2 : 3;
        const int new_len = c == 16 ? prev : 0;
        if (repeat_len != new_len) {
          repeat = 0;
          repeat_len = new_len;
        }
        const int old = repeat;
        if (repeat > 0) repeat = (repeat - 2) << extra;
        repeat += static_cast<int>(r.get(extra)) + 3;
        const int delta = repeat - old;
        if (delta <= 0 || static_cast<std::size_t>(delta) > alphabet - symbol) {
          return fail(corrupt("code length repeat overflows the alphabet"));
        }
        std::fill_n(lengths.begin() + static_cast<std::ptrdiff_t>(symbol), delta, static_cast<std::uint8_t>(repeat_len));
        symbol += static_cast<std::size_t>(delta);
        if (repeat_len != 0) left -= delta * (32768 >> repeat_len);
      }
      if (r.overrun()) return fail(corrupt("truncated prefix code"));
    }
    if (left != 0) return fail(corrupt("incomplete prefix code"));
  }
  if (!code.table.build(lengths, true, true)) return fail(corrupt("invalid prefix code"));
  return {};
}

std::uint32_t read_count_1_256(Bits& r) {
  if (r.get(1) == 0) return 1;
  const int n = static_cast<int>(r.get(3));
  if (n == 0) return 2;
  return (1u << n) + r.get(n) + 1u;
}

struct BlockSwitch {
  std::uint32_t types = 1;
  PrefixCode type_code;
  PrefixCode count_code;
  std::uint32_t type = 0;
  std::uint32_t last = 0;
  std::uint32_t second_last = 1;
  std::uint32_t left = 0xFFFFFFFFu;

  Result<std::uint32_t> read_count(Bits& r) const {
    const int sym = count_code.decode(r);
    if (sym < 0 || sym >= 26) return fail(corrupt("invalid block count"));
    const br::LengthCode& c = br::block_count_codes[static_cast<std::size_t>(sym)];
    return c.base + r.get(c.extra);
  }
  Result<void> read_header(Bits& r) {
    types = read_count_1_256(r);
    if (types < 2) return {};
    CVT_TRY(read_prefix_code(r, types + 2, type_code));
    CVT_TRY(read_prefix_code(r, br::block_count_alphabet, count_code));
    CVT_TRY_ASSIGN(left, read_count(r));
    return {};
  }
  Result<void> next(Bits& r) {
    const int sym = type_code.decode(r);
    if (sym < 0) return fail(corrupt("invalid block type"));
    std::uint32_t t;
    if (sym == 0) {
      t = second_last;
    } else if (sym == 1) {
      t = last + 1;
      if (t >= types) t -= types;
    } else {
      t = static_cast<std::uint32_t>(sym - 2);
    }
    second_last = last;
    last = t;
    type = t;
    CVT_TRY_ASSIGN(left, read_count(r));
    return {};
  }
};

Result<void> read_context_map(Bits& r, std::size_t size, std::uint32_t trees, std::vector<std::uint8_t>& map) {
  map.assign(size, 0);
  if (trees == 1) return {};
  std::uint32_t rlemax = 0;
  if (r.get(1) != 0) rlemax = r.get(4) + 1;
  PrefixCode code;
  CVT_TRY(read_prefix_code(r, trees + rlemax, code));
  std::size_t i = 0;
  while (i < size) {
    const int sym = code.decode(r);
    if (sym < 0) return fail(corrupt("invalid context map"));
    if (sym == 0) {
      map[i++] = 0;
    } else if (static_cast<std::uint32_t>(sym) <= rlemax) {
      const std::size_t run = (std::size_t{1} << sym) + r.get(sym);
      if (run > size - i) return fail(corrupt("context map run overflows"));
      i += run;  // already zero
    } else {
      map[i++] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(sym) - rlemax);
    }
    if (r.overrun()) return fail(corrupt("truncated context map"));
  }
  if (r.get(1) != 0) {
    std::array<std::uint8_t, 256> mtf{};
    for (std::size_t k = 0; k < 256; ++k) mtf[k] = static_cast<std::uint8_t>(k);
    for (std::uint8_t& v : map) {
      const std::uint8_t index = v;
      const std::uint8_t value = mtf[index];
      v = value;
      for (std::size_t k = index; k > 0; --k) mtf[k] = mtf[k - 1];
      mtf[0] = value;
    }
  }
  return {};
}

// ---------------------------------------------------------------------------- engine

enum class Outcome { Progress, NeedMore, Finished, Stopped };

class Engine {
 public:
  explicit Engine(const DecodeOptions& options) : options_(options) {}

  /// Decode the stream header or one meta-block, starting at `bit_pos()` of `data`.
  Result<Outcome> next(ByteView data, bool last) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    Bits r(bytes, data.size(), bit_pos_);
    const std::array<std::uint32_t, 4> saved_dist = dist_;
    const std::uint64_t saved_pos = pos_;
    const std::size_t saved_hist = hist_.size();
    Result<Outcome> outcome = header_done_ ? metablock(r) : header(r);
    if (r.overrun()) {
      dist_ = saved_dist;
      pos_ = saved_pos;
      hist_.resize(saved_hist);
      if (!last) return Outcome::NeedMore;
      return fail(Status::Decode, "Brotli stream is truncated");
    }
    if (!outcome) return outcome;
    bit_pos_ = r.position();
    if (*outcome == Outcome::Progress && !header_done_) header_done_ = true;
    if (*outcome == Outcome::Progress && options_.stop_after != 0 && pos_ >= options_.stop_after) return Outcome::Stopped;
    return outcome;
  }

  [[nodiscard]] std::uint64_t bit_pos() const noexcept { return bit_pos_; }
  void rebase_input(std::uint64_t dropped_bytes) noexcept { bit_pos_ -= dropped_bytes * 8; }
  [[nodiscard]] std::uint64_t total_out() const noexcept { return pos_; }
  Bytes& history() noexcept { return hist_; }
  [[nodiscard]] std::uint64_t history_base() const noexcept { return hist_base_; }
  /// Drop history that back-references can no longer reach.
  void trim() {
    const std::size_t keep = window_size() + 8;
    if (hist_.size() > 2 * keep) {
      const std::size_t drop = hist_.size() - keep;
      hist_.erase(hist_.begin(), hist_.begin() + static_cast<std::ptrdiff_t>(drop));
      hist_base_ += drop;
    }
  }

 private:
  [[nodiscard]] std::size_t window_size() const noexcept { return std::size_t{1} << wbits_; }

  Result<Outcome> header(Bits& r) {
    if (r.get(1) == 0) {
      wbits_ = 16;
    } else {
      const std::uint32_t n = r.get(3);
      if (n != 0) {
        wbits_ = 17 + static_cast<int>(n);
      } else {
        const std::uint32_t m = r.get(3);
        if (m == 1) return fail(Status::Decode, "large-window Brotli streams are not supported");
        wbits_ = m == 0 ? 17 : 8 + static_cast<int>(m);
      }
    }
    max_distance_ = (std::uint64_t{1} << wbits_) - 16;
    return Outcome::Progress;
  }

  Result<Outcome> metablock(Bits& r) {
    const bool is_last = r.get(1) != 0;
    if (is_last && r.get(1) != 0) return Outcome::Finished;  // ISLASTEMPTY
    const std::uint32_t nibbles_code = r.get(2);
    if (nibbles_code == 3) {
      // Metadata: skipped.
      if (r.get(1) != 0) return fail(corrupt("reserved bit is set"));
      const int skip_bytes = static_cast<int>(r.get(2));
      std::uint32_t skip = 0;
      for (int i = 0; i < skip_bytes; ++i) {
        const std::uint32_t b = r.get(8);
        if (i + 1 == skip_bytes && skip_bytes > 1 && b == 0) return fail(corrupt("non-minimal metadata length"));
        skip |= b << (8 * i);
      }
      if (skip_bytes != 0) ++skip;
      if (r.align() != 0) return fail(corrupt("non-zero padding"));
      if (!r.raw(nullptr, 0, skip)) return Outcome::NeedMore;  // overrun is flagged
      return is_last ? Outcome::Finished : Outcome::Progress;
    }
    const int nibbles = 4 + static_cast<int>(nibbles_code);
    std::uint32_t mlen = 0;
    for (int i = 0; i < nibbles; ++i) {
      const std::uint32_t nib = r.get(4);
      if (i + 1 == nibbles && nibbles > 4 && nib == 0) return fail(corrupt("non-minimal meta-block length"));
      mlen |= nib << (4 * i);
    }
    ++mlen;
    const bool uncompressed = !is_last && r.get(1) != 0;

    // Room for the output (or the part of it a prefix decode needs).
    std::uint64_t need = mlen;
    if (options_.stop_after != 0) need = std::min<std::uint64_t>(need, options_.stop_after - std::min(options_.stop_after, pos_));
    CVT_TRY(check_output_room(pos_, need, options_.max_output));
    if (uncompressed) {
      if (r.align() != 0) return fail(corrupt("non-zero padding"));
      if (r.position() / 8 + mlen > r.size()) {
        (void)r.raw(nullptr, 0, mlen);  // flags the overrun
        return Outcome::NeedMore;
      }
      const std::size_t at = hist_.size();
      hist_.resize(at + static_cast<std::size_t>(need));
      (void)r.raw(reinterpret_cast<std::uint8_t*>(hist_.data() + at), static_cast<std::size_t>(need), mlen);
      pos_ += need;
      if (need < mlen) return Outcome::Stopped;
      return Outcome::Progress;
    }
    CVT_TRY_ASSIGN(const bool stopped, compressed(r, mlen, static_cast<std::size_t>(need)));
    if (stopped) return Outcome::Stopped;
    return is_last ? Outcome::Finished : Outcome::Progress;
  }

  // Returns true when a prefix decode reached `stop_after`.
  Result<bool> compressed(Bits& r, std::uint32_t mlen, std::size_t cap) {
    std::array<BlockSwitch, 3> bs;  // literal, insert&copy, distance
    for (BlockSwitch& b : bs) CVT_TRY(b.read_header(r));
    const std::uint32_t npostfix = r.get(2);
    const std::uint32_t ndirect = r.get(4) << npostfix;
    std::vector<std::uint8_t> modes(bs[0].types);
    for (std::uint8_t& m : modes) m = static_cast<std::uint8_t>(r.get(2));
    const std::uint32_t ntrees_l = read_count_1_256(r);
    std::vector<std::uint8_t> cmap_l;
    CVT_TRY(read_context_map(r, std::size_t{64} * bs[0].types, ntrees_l, cmap_l));
    const std::uint32_t ntrees_d = read_count_1_256(r);
    std::vector<std::uint8_t> cmap_d;
    CVT_TRY(read_context_map(r, std::size_t{4} * bs[2].types, ntrees_d, cmap_d));
    if (r.overrun()) return fail(corrupt("truncated meta-block header"));
    std::vector<PrefixCode> lit(ntrees_l);
    for (PrefixCode& c : lit) CVT_TRY(read_prefix_code(r, br::literal_alphabet, c));
    std::vector<PrefixCode> cmd(bs[1].types);
    for (PrefixCode& c : cmd) CVT_TRY(read_prefix_code(r, br::command_alphabet, c));
    const std::size_t dist_alphabet = 16 + ndirect + (std::size_t{48} << npostfix);
    std::vector<PrefixCode> dst(ntrees_d);
    for (PrefixCode& c : dst) CVT_TRY(read_prefix_code(r, dist_alphabet, c));

    const std::size_t start = hist_.size();
    // The output grows as it is produced (a hostile header may announce far
    // more than the data delivers).
    hist_.resize(start + std::min<std::size_t>(cap, 1u << 16));
    std::uint8_t* o = reinterpret_cast<std::uint8_t*>(hist_.data());
    std::size_t w = start;            // write index into hist_
    const std::size_t end = start + cap;
    std::uint32_t produced = 0;       // bytes of this meta-block (logical)
    std::uint8_t p1 = w > 0 ? o[w - 1] : 0;
    std::uint8_t p2 = w > 1 ? o[w - 2] : 0;
    const std::uint32_t postfix_mask = (1u << npostfix) - 1u;
    std::uint32_t lit_type = 0;
    const std::uint8_t* lit_map = cmap_l.data();
    int lit_mode = modes[0];
    const auto ensure = [&](std::size_t n) {
      if (w + n > hist_.size()) {
        hist_.resize(std::min(end, std::max(w + n, w + (w - start) + (std::size_t{1} << 16))));
        o = reinterpret_cast<std::uint8_t*>(hist_.data());
      }
    };
    const auto stop_here = [&]() {
      hist_.resize(w);
      pos_ += w - start;
    };

    while (produced < mlen) {
      if (r.overrun()) return fail(corrupt("truncated meta-block"));
      if (bs[1].left == 0) CVT_TRY(bs[1].next(r));
      --bs[1].left;
      const int sym = cmd[bs[1].type].decode(r);
      if (sym < 0) return fail(corrupt("invalid command code"));
      const std::size_t cell = static_cast<std::size_t>(sym) >> 6;
      const br::LengthCode& ic =
          br::insert_codes[br::cell_insert[cell] + ((static_cast<std::uint32_t>(sym) >> 3) & 7u)];
      const br::LengthCode& cc = br::copy_codes[br::cell_copy[cell] + (static_cast<std::uint32_t>(sym) & 7u)];
      const std::uint32_t insert = ic.base + r.get(ic.extra);
      const std::uint32_t copy = cc.base + r.get(cc.extra);
      if (insert > mlen - produced) return fail(corrupt("insert length exceeds the meta-block"));
      ensure(std::min<std::size_t>(insert, end - w));
      for (std::uint32_t k = 0; k < insert; ++k) {
        if (bs[0].left == 0) {
          CVT_TRY(bs[0].next(r));
          lit_type = bs[0].type;
          lit_map = cmap_l.data() + std::size_t{64} * lit_type;
          lit_mode = modes[lit_type];
        }
        --bs[0].left;
        const std::uint32_t ctx = br::literal_context(lit_mode, p1, p2);
        const int b = lit[lit_map[ctx]].decode(r);
        if (b < 0) return fail(corrupt("invalid literal code"));
        p2 = p1;
        p1 = static_cast<std::uint8_t>(b);
        o[w++] = p1;
        if (w == end && cap < mlen) {
          stop_here();
          return true;
        }
      }
      produced += insert;
      if (produced == mlen) break;

      // Distance.
      std::uint64_t distance;
      bool push = true;
      if (sym < 128) {
        distance = dist_[0];
        push = false;
      } else {
        if (bs[2].left == 0) CVT_TRY(bs[2].next(r));
        --bs[2].left;
        const std::uint32_t dctx = copy > 4 ? 3u : copy - 2u;
        const int dcode = dst[cmap_d[bs[2].type * 4u + dctx]].decode(r);
        if (dcode < 0) return fail(corrupt("invalid distance code"));
        const auto d = static_cast<std::uint32_t>(dcode);
        if (d < 16) {
          static constexpr std::array<std::uint8_t, 16> index = {0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1};
          static constexpr std::array<std::int8_t, 16> delta = {0, 0, 0, 0, -1, 1, -2, 2, -3, 3, -1, 1, -2, 2, -3, 3};
          const std::int64_t v = static_cast<std::int64_t>(dist_[index[d]]) + delta[d];
          if (v <= 0) return fail(corrupt("invalid distance"));
          distance = static_cast<std::uint64_t>(v);
          if (d == 0) push = false;
        } else if (d < 16 + ndirect) {
          distance = d - 15;
        } else {
          const std::uint32_t h = d - ndirect - 16;
          const int nbits = 1 + static_cast<int>(h >> (npostfix + 1));
          const std::uint64_t offset = ((std::uint64_t{2} + ((h >> npostfix) & 1u)) << nbits) - 4u;
          distance = ((offset + r.get(nbits)) << npostfix) + (h & postfix_mask) + ndirect + 1u;
        }
      }
      const std::uint64_t here = pos_ + (w - start);
      const std::uint64_t reach = std::min(max_distance_, here);
      if (distance > reach) {
        // Static dictionary reference.
        if (copy < static_cast<std::uint32_t>(br::min_dictionary_word) ||
            copy > static_cast<std::uint32_t>(br::max_dictionary_word)) {
          return fail(corrupt("invalid dictionary reference length"));
        }
        const int nb = br::ndbits[copy];
        const std::uint64_t word_id = distance - reach - 1;
        const std::uint64_t transform = word_id >> nb;
        if (transform >= static_cast<std::uint64_t>(br::transform_count)) {
          return fail(corrupt("invalid dictionary reference"));
        }
        const std::uint64_t index = word_id & ((std::uint64_t{1} << nb) - 1u);
        const std::uint8_t* word = br::dictionary().data() + br::dictionary_offset[copy] + index * copy;
        std::array<std::uint8_t, 64> tmp{};
        const std::size_t n = br::apply_transform(tmp.data(), word, static_cast<int>(copy), static_cast<int>(transform));
        if (n > mlen - produced) return fail(corrupt("dictionary word exceeds the meta-block"));
        const std::size_t room = end - w;
        ensure(std::min(n, room));
        std::memcpy(o + w, tmp.data(), std::min(n, room));
        if (n >= room && cap < mlen) {
          w += room;
          stop_here();
          return true;
        }
        w += n;
        produced += static_cast<std::uint32_t>(n);
      } else {
        if (copy > mlen - produced) return fail(corrupt("copy length exceeds the meta-block"));
        if (distance > w) return fail(corrupt("distance beyond the available history"));
        if (push) {
          dist_[3] = dist_[2];
          dist_[2] = dist_[1];
          dist_[1] = dist_[0];
          dist_[0] = static_cast<std::uint32_t>(distance);
        }
        const std::size_t room = end - w;
        const std::size_t n = std::min<std::size_t>(copy, room);
        ensure(n);
        const std::size_t from = w - static_cast<std::size_t>(distance);
        if (distance >= n) {
          std::memcpy(o + w, o + from, n);
        } else {
          for (std::size_t k = 0; k < n; ++k) o[w + k] = o[from + k];
        }
        w += n;
        if (n < copy || (w == end && cap < mlen)) {
          stop_here();
          return true;
        }
        produced += copy;
      }
      p1 = w > 0 ? o[w - 1] : 0;
      p2 = w > 1 ? o[w - 2] : 0;
    }
    if (produced != mlen) return fail(corrupt("meta-block length mismatch"));
    hist_.resize(w);
    pos_ += w - start;
    return cap < mlen;
  }

  const DecodeOptions& options_;
  bool header_done_ = false;
  int wbits_ = 16;
  std::uint64_t max_distance_ = 0;
  std::uint64_t bit_pos_ = 0;
  std::uint64_t pos_ = 0;
  std::array<std::uint32_t, 4> dist_ = {4, 11, 15, 16};  // last, second-to-last, ...
  Bytes hist_;
  std::uint64_t hist_base_ = 0;
};

// ---------------------------------------------------------------------------- one-shot

Result<Bytes> decode_brotli(ByteView data, const DecodeOptions& options) {
  Engine engine(options);
  while (true) {
    CVT_TRY_ASSIGN(const Outcome outcome, engine.next(data, true));
    if (outcome == Outcome::Stopped) break;
    if (outcome == Outcome::Finished) {
      const std::uint64_t used = (engine.bit_pos() + 7) / 8;
      if (used < data.size()) return fail(Status::Decode, "data follows the end of the Brotli stream");
      break;
    }
    if (!options.progress.report(static_cast<double>(engine.bit_pos() / 8) / static_cast<double>(std::max<std::size_t>(1, data.size())))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  Bytes out = std::move(engine.history());
  if (options.output_size && options.stop_after == 0 && out.size() != *options.output_size) {
    return fail(Status::Decode, "Brotli data decodes to {} bytes, expected {}", out.size(), *options.output_size);
  }
  return out;
}

// ---------------------------------------------------------------------------- incremental

class BrotliDecoder final : public Decoder {
 public:
  explicit BrotliDecoder(const DecodeOptions& options) : options_(options), engine_(options_) {}

  Result<Step> decode(ByteView input, Bytes& output, bool last) override {
    if (error_) return fail(*error_);
    Step step;
    if (finished_) {
      step.finished = true;
      return step;
    }
    append(in_, input);
    step.consumed = input.size();
    while (true) {
      const std::uint64_t avail = std::uint64_t{in_.size()} * 8 - engine_.bit_pos();
      if (!last && avail < retry_bits_) break;
      auto outcome = engine_.next(in_, last);
      if (!outcome) {
        error_ = outcome.error();
        return fail(*error_);
      }
      if (*outcome == Outcome::NeedMore) {
        retry_bits_ = avail * 2 + 64;
        break;
      }
      retry_bits_ = 0;
      step.produced += flush(output);
      const std::size_t whole = static_cast<std::size_t>(engine_.bit_pos() / 8);
      if (*outcome != Outcome::Progress) {
        finished_ = true;
        step.finished = true;
        const std::size_t used = static_cast<std::size_t>((engine_.bit_pos() + 7) / 8);
        const std::size_t unused = in_.size() - std::min(used, in_.size());
        if (*outcome == Outcome::Finished && unused > input.size()) {
          error_ = Error{Status::Decode, "data follows the end of the Brotli stream"};
          return fail(*error_);
        }
        if (*outcome == Outcome::Finished) step.consumed = input.size() - unused;
        total_in_ += in_.size() - unused;
        in_.clear();
        break;
      }
      in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(whole));
      total_in_ += whole;
      engine_.rebase_input(whole);
      if (!options_.progress.report(0.0)) {
        error_ = Error{Status::Cancelled, "the conversion was cancelled"};
        return fail(*error_);
      }
    }
    if (last && !finished_) {
      error_ = Error{Status::Decode, "Brotli stream is truncated"};
      return fail(*error_);
    }
    return step;
  }
  bool finished() const noexcept override { return finished_; }
  std::uint64_t total_in() const noexcept override { return total_in_; }
  std::uint64_t total_out() const noexcept override { return emitted_; }

 private:
  std::size_t flush(Bytes& output) {
    Bytes& hist = engine_.history();
    const std::uint64_t base = engine_.history_base();
    const std::size_t from = static_cast<std::size_t>(emitted_ - base);
    const std::size_t n = hist.size() - from;
    output.insert(output.end(), hist.begin() + static_cast<std::ptrdiff_t>(from), hist.end());
    emitted_ += n;
    engine_.trim();
    return n;
  }

  DecodeOptions options_;
  Engine engine_;
  Bytes in_;
  std::optional<Error> error_;
  bool finished_ = false;
  std::uint64_t retry_bits_ = 0;
  std::uint64_t total_in_ = 0;
  std::uint64_t emitted_ = 0;
};

std::unique_ptr<Decoder> make_brotli_decoder(const DecodeOptions& options) {
  return std::make_unique<BrotliDecoder>(options);
}

constexpr std::array<CodecImpl, 1> table = {{
    {Method::Brotli, decode_brotli, brotli_encode, make_brotli_decoder, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> brotli_codecs() { return table; }

}  // namespace cvt::compression::detail
