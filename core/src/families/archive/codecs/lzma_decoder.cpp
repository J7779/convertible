// Convertible core - archive family: LZMA decoder (range decoder + model).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "families/archive/codecs/lzma_internal.hpp"

namespace cvt::compression::detail::lzma {

void Model::reset(int lc, int lp) {
  literal.assign(std::size_t{0x300} << (lc + lp), static_cast<std::uint16_t>(prob_init));
  const auto fill = [](auto& a) { a.fill(static_cast<std::uint16_t>(prob_init)); };
  fill(is_match);
  fill(is_rep);
  fill(is_rep_g0);
  fill(is_rep_g1);
  fill(is_rep_g2);
  fill(is_rep0_long);
  fill(pos_slot);
  fill(spec_pos);
  fill(align);
  for (Length* l : {&len, &rep_len}) {
    l->choice = l->choice2 = static_cast<std::uint16_t>(prob_init);
    fill(l->low);
    fill(l->mid);
    fill(l->high);
  }
}

namespace {

constexpr std::uint32_t top = 1u << 24;

class RangeDecoder {
 public:
  RangeDecoder(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool init() {
    if (size_ < 5 || data_[0] != 0) return false;
    code_ = 0;
    for (int i = 1; i < 5; ++i) code_ = (code_ << 8) | data_[i];
    pos_ = 5;
    range_ = 0xFFFFFFFFu;
    return code_ != range_;
  }
  void normalize() {
    if (range_ < top) {
      range_ <<= 8;
      code_ = (code_ << 8) | (pos_ < size_ ? data_[pos_] : 0u);
      ++pos_;
    }
  }
  int bit(std::uint16_t& prob) {
    const std::uint32_t bound = (range_ >> 11) * prob;
    int b;
    if (code_ < bound) {
      range_ = bound;
      prob = static_cast<std::uint16_t>(prob + ((2048u - prob) >> 5));
      b = 0;
    } else {
      range_ -= bound;
      code_ -= bound;
      prob = static_cast<std::uint16_t>(prob - (prob >> 5));
      b = 1;
    }
    normalize();
    return b;
  }
  std::uint32_t direct(int count) {
    std::uint32_t result = 0;
    for (int i = 0; i < count; ++i) {
      range_ >>= 1;
      const std::uint32_t t = code_ >= range_ ? 1u : 0u;
      code_ -= range_ & (0u - t);
      result = (result << 1) | t;
      normalize();
    }
    return result;
  }
  template <std::size_t N>
  std::uint32_t tree(std::array<std::uint16_t, N>& probs, std::size_t base, int bits) {
    std::uint32_t m = 1;
    for (int i = 0; i < bits; ++i) m = (m << 1) | static_cast<std::uint32_t>(bit(probs[base + m]));
    return m - (1u << bits);
  }
  template <std::size_t N>
  std::uint32_t reverse_tree(std::array<std::uint16_t, N>& probs, std::size_t base, int bits) {
    std::uint32_t m = 1;
    std::uint32_t symbol = 0;
    for (int i = 0; i < bits; ++i) {
      const auto b = static_cast<std::uint32_t>(bit(probs[base + m]));
      m = (m << 1) | b;
      symbol |= b << i;
    }
    return symbol;
  }
  [[nodiscard]] std::size_t consumed() const noexcept { return pos_; }
  [[nodiscard]] bool overrun() const noexcept { return pos_ > size_; }
  [[nodiscard]] bool finished_cleanly() const noexcept { return code_ == 0; }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  std::uint32_t range_ = 0;
  std::uint32_t code_ = 0;
};

std::uint32_t decode_len(RangeDecoder& rc, Model::Length& l, std::uint32_t pos_state) {
  if (rc.bit(l.choice) == 0) return rc.tree(l.low, pos_state << 3, 3);
  if (rc.bit(l.choice2) == 0) return 8 + rc.tree(l.mid, pos_state << 3, 3);
  return 16 + rc.tree(l.high, 0, 8);
}

}  // namespace

void Decoder::set_properties(const Properties& props) {
  props_ = props;
  have_props_ = true;
  reset_state();
}

void Decoder::reset_state() {
  model_.reset(props_.lc, props_.lp);
  st_.reset();
}

Result<Decoder::Outcome> Decoder::decode(ByteView input, Bytes& out, std::uint64_t out_limit, bool allow_end_marker,
                                         std::uint64_t max_output, const Progress& progress) {
  if (!have_props_) return fail(Status::Decode, "LZMA properties are missing");
  RangeDecoder rc(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
  if (!rc.init()) return fail(Status::Decode, "LZMA data is damaged (bad range coder start)");
  Outcome outcome;
  const std::uint32_t pb_mask = (1u << props_.pb) - 1u;
  const std::uint32_t lp_mask = (1u << props_.lp) - 1u;
  const int lc = props_.lc;
  // Grow the output in steps so that huge declared sizes never allocate up front.
  const auto reserve_more = [&](std::uint64_t want) -> Result<void> {
    if (want > max_output) return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", max_output);
    if (out.capacity() < want) out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
        std::max<std::uint64_t>(want, std::uint64_t{out.capacity()} * 2), std::max(out_limit, want))));
    return {};
  };
  int state = st_.state;
  std::uint32_t rep0 = st_.reps[0], rep1 = st_.reps[1], rep2 = st_.reps[2], rep3 = st_.reps[3];
  std::uint64_t steps = 0;
  while (out.size() < out_limit) {
    if (rc.overrun()) return fail(Status::Decode, "LZMA data is truncated");
    if ((++steps & 0xFFFFu) == 0 && !progress.report(static_cast<double>(rc.consumed()) / static_cast<double>(input.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    const std::size_t pos = out.size();
    // Positions count from the last dictionary reset (LZMA2), like the reference decoders.
    const std::size_t dict_len = pos - dict_start_;
    const auto rel = static_cast<std::uint32_t>(dict_len);
    const std::uint32_t pos_state = rel & pb_mask;
    if (rc.bit(model_.is_match[(static_cast<std::size_t>(state) << pos_bits_max) + pos_state]) == 0) {
      const std::uint32_t prev = dict_len > 0 ? std::to_integer<std::uint32_t>(out[pos - 1]) : 0u;
      std::uint16_t* probs = model_.literal.data() +
                             std::size_t{0x300} * (((rel & lp_mask) << lc) + (prev >> (8 - lc)));
      std::uint32_t symbol = 1;
      if (state >= 7) {
        if (rep0 >= dict_len) return fail(Status::Decode, "LZMA data is damaged (distance beyond the dictionary)");
        std::uint32_t match_byte = std::to_integer<std::uint32_t>(out[pos - rep0 - 1]);
        do {
          const std::uint32_t match_bit = (match_byte >> 7) & 1u;
          match_byte <<= 1;
          const auto b = static_cast<std::uint32_t>(rc.bit(probs[0x100 + (match_bit << 8) + symbol]));
          symbol = (symbol << 1) | b;
          if (match_bit != b) break;
        } while (symbol < 0x100);
      }
      while (symbol < 0x100) symbol = (symbol << 1) | static_cast<std::uint32_t>(rc.bit(probs[symbol]));
      CVT_TRY(reserve_more(pos + 1));
      out.push_back(static_cast<std::byte>(symbol & 0xFFu));
      state = state_after_literal(state);
      continue;
    }
    std::uint32_t len = 0;
    if (rc.bit(model_.is_rep[static_cast<std::size_t>(state)]) != 0) {
      if (dict_len == 0) return fail(Status::Decode, "LZMA data is damaged (repeat at stream start)");
      if (rc.bit(model_.is_rep_g0[static_cast<std::size_t>(state)]) == 0) {
        if (rc.bit(model_.is_rep0_long[(static_cast<std::size_t>(state) << pos_bits_max) + pos_state]) == 0) {
          if (rep0 >= dict_len) return fail(Status::Decode, "LZMA data is damaged (distance beyond the dictionary)");
          state = state_after_short_rep(state);
          CVT_TRY(reserve_more(pos + 1));
          out.push_back(out[pos - rep0 - 1]);
          continue;
        }
      } else {
        std::uint32_t dist;
        if (rc.bit(model_.is_rep_g1[static_cast<std::size_t>(state)]) == 0) {
          dist = rep1;
        } else {
          if (rc.bit(model_.is_rep_g2[static_cast<std::size_t>(state)]) == 0) {
            dist = rep2;
          } else {
            dist = rep3;
            rep3 = rep2;
          }
          rep2 = rep1;
        }
        rep1 = rep0;
        rep0 = dist;
      }
      len = decode_len(rc, model_.rep_len, pos_state);
      state = state_after_rep(state);
    } else {
      rep3 = rep2;
      rep2 = rep1;
      rep1 = rep0;
      len = decode_len(rc, model_.len, pos_state);
      state = state_after_match(state);
      const std::uint32_t len_state = std::min<std::uint32_t>(len, len_to_pos_states - 1);
      const std::uint32_t slot = rc.tree(model_.pos_slot, len_state << 6, 6);
      if (slot < 4) {
        rep0 = slot;
      } else {
        const int direct_bits = static_cast<int>(slot >> 1) - 1;
        std::uint32_t dist = (2u | (slot & 1u)) << direct_bits;
        if (slot < end_pos_model) {
          dist += rc.reverse_tree(model_.spec_pos, dist - slot, direct_bits);
        } else {
          dist += rc.direct(direct_bits - align_bits) << align_bits;
          dist += rc.reverse_tree(model_.align, 0, align_bits);
        }
        rep0 = dist;
        if (rep0 == 0xFFFFFFFFu) {
          if (!allow_end_marker) return fail(Status::Decode, "LZMA data has an unexpected end marker");
          outcome.end_marker = true;
          break;
        }
      }
    }
    len += match_min_len;
    if (rep0 >= dict_len || rep0 >= props_.dict_size) {
      return fail(Status::Decode, "LZMA data is damaged (distance beyond the dictionary)");
    }
    const std::uint64_t room = out_limit - out.size();
    if (len > room) return fail(Status::Decode, "LZMA match runs past the end of the data");
    CVT_TRY(reserve_more(pos + len));
    const std::size_t from = pos - rep0 - 1;
    if (rep0 + 1 >= len) {
      out.insert(out.end(), out.begin() + static_cast<std::ptrdiff_t>(from),
                 out.begin() + static_cast<std::ptrdiff_t>(from + len));
    } else {
      for (std::uint32_t i = 0; i < len; ++i) out.push_back(out[from + i]);
    }
  }
  if (rc.overrun()) return fail(Status::Decode, "LZMA data is truncated");
  st_.state = state;
  st_.reps = {rep0, rep1, rep2, rep3};
  outcome.consumed = rc.consumed();
  return outcome;
}

}  // namespace cvt::compression::detail::lzma
