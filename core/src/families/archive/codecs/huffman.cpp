// Convertible core - archive family: canonical Huffman coding (see huffman.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/archive/codecs/huffman.hpp"

#include <numeric>

namespace cvt::compression::detail {

void huffman_lengths(std::span<const std::uint32_t> freq, int max_bits, std::span<std::uint8_t> lengths, bool min_two) {
  const std::size_t n = freq.size();
  std::fill(lengths.begin(), lengths.end(), std::uint8_t{0});
  std::vector<std::uint32_t> used;
  used.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (freq[i] != 0) used.push_back(static_cast<std::uint32_t>(i));
  }
  if (used.size() <= 1) {
    if (used.size() == 1) lengths[used[0]] = 1;
    if (min_two && n >= 2) {
      const std::uint32_t other = used.empty() || used[0] != 0 ? 0u : 1u;
      lengths[other] = 1;
      if (used.empty()) lengths[1] = 1;
    }
    return;
  }
  // Two-queue Huffman construction over leaves sorted by frequency.
  std::sort(used.begin(), used.end(), [&](std::uint32_t a, std::uint32_t b) {
    return freq[a] != freq[b] ? freq[a] < freq[b] : a < b;
  });
  const std::size_t leaves = used.size();
  std::vector<std::uint64_t> weight(2 * leaves - 1);
  std::vector<std::uint32_t> parent(2 * leaves - 1, 0);
  for (std::size_t i = 0; i < leaves; ++i) weight[i] = freq[used[i]];
  std::size_t leaf = 0;
  std::size_t inner = leaves;
  std::size_t next = leaves;
  const auto take = [&]() -> std::size_t {
    if (leaf < leaves && (inner >= next || weight[leaf] <= weight[inner])) return leaf++;
    return inner++;
  };
  while (next < 2 * leaves - 1) {
    const std::size_t a = take();
    const std::size_t b = take();
    weight[next] = weight[a] + weight[b];
    parent[a] = static_cast<std::uint32_t>(next);
    parent[b] = static_cast<std::uint32_t>(next);
    ++next;
  }
  std::vector<std::uint32_t> depth(2 * leaves - 1, 0);
  for (std::size_t i = 2 * leaves - 1; i-- > 0;) {
    if (i != 2 * leaves - 2) depth[i] = depth[parent[i]] + 1;
  }
  std::vector<int> len(leaves);
  int longest = 0;
  for (std::size_t i = 0; i < leaves; ++i) {
    len[i] = static_cast<int>(depth[i]);
    longest = std::max(longest, len[i]);
  }
  if (longest > max_bits) {
    // Clamp, then lengthen the cheapest codes until the Kraft sum fits again.
    const std::uint64_t target = std::uint64_t{1} << max_bits;
    std::uint64_t kraft = 0;
    for (int& l : len) {
      l = std::min(l, max_bits);
      kraft += std::uint64_t{1} << (max_bits - l);
    }
    while (kraft > target) {
      // Leaves are sorted by ascending frequency: the first one below the
      // maximum length with the greatest length is the cheapest to extend.
      int best = -1;
      for (std::size_t i = 0; i < leaves; ++i) {
        if (len[i] < max_bits && (best < 0 || len[i] > len[static_cast<std::size_t>(best)])) best = static_cast<int>(i);
      }
      if (best < 0) break;  // cannot happen for leaves <= 2^max_bits
      kraft -= std::uint64_t{1} << (max_bits - len[static_cast<std::size_t>(best)] - 1);
      ++len[static_cast<std::size_t>(best)];
    }
    // Spend any slack on the most frequent symbols.
    for (std::size_t i = leaves; i-- > 0;) {
      while (len[i] > 1 && kraft + (std::uint64_t{1} << (max_bits - len[i])) <= target) {
        kraft += std::uint64_t{1} << (max_bits - len[i]);
        --len[i];
      }
    }
  }
  // Make the code complete (Kraft sum exactly 1): formats such as Zstandard
  // derive the last code length from the others. Each step shortens the
  // longest code; the deficit is always a multiple of its unit.
  if (longest > max_bits) {
    const std::uint64_t target = std::uint64_t{1} << max_bits;
    std::uint64_t kraft = 0;
    for (const int l : len) kraft += std::uint64_t{1} << (max_bits - l);
    while (kraft < target) {
      std::size_t pick = 0;
      for (std::size_t i = 1; i < leaves; ++i) {
        if (len[i] > len[pick] || (len[i] == len[pick] && freq[used[i]] > freq[used[pick]])) pick = i;
      }
      if (len[pick] <= 1) break;
      kraft += std::uint64_t{1} << (max_bits - len[pick]);
      --len[pick];
    }
  }
  for (std::size_t i = 0; i < leaves; ++i) lengths[used[i]] = static_cast<std::uint8_t>(len[i]);
}

void canonical_codes(std::span<const std::uint8_t> lengths, std::span<std::uint32_t> codes) {
  std::uint32_t count[HuffmanTable::max_supported_bits + 2] = {};
  for (const std::uint8_t l : lengths) {
    if (l <= HuffmanTable::max_supported_bits) ++count[l];
  }
  count[0] = 0;
  std::uint32_t next[HuffmanTable::max_supported_bits + 2] = {};
  std::uint32_t code = 0;
  for (int bits = 1; bits <= HuffmanTable::max_supported_bits; ++bits) {
    code = (code + count[bits - 1]) << 1;
    next[bits] = code;
  }
  for (std::size_t i = 0; i < lengths.size(); ++i) {
    const std::uint8_t l = lengths[i];
    codes[i] = l == 0 || l > HuffmanTable::max_supported_bits ? 0 : next[l]++;
  }
}

bool HuffmanTable::build(std::span<const std::uint8_t> lengths, bool lsb_first, bool require_complete) {
  lsb_first_ = lsb_first;
  std::fill(std::begin(count_), std::end(count_), 0u);
  max_length_ = 0;
  for (const std::uint8_t l : lengths) {
    if (l > max_supported_bits) return false;
    ++count_[l];
    max_length_ = std::max<int>(max_length_, l);
  }
  count_[0] = 0;
  // Kraft check.
  std::int64_t left = 1;
  for (int len = 1; len <= max_length_; ++len) {
    left = left * 2 - static_cast<std::int64_t>(count_[len]);
    if (left < 0) return false;
  }
  if (require_complete && left != 0 && max_length_ != 0) return false;

  std::uint32_t code = 0;
  std::uint32_t offset = 0;
  for (int len = 1; len <= max_supported_bits + 1; ++len) {
    code = (code + (len > 1 ? count_[len - 1] : 0)) << 1;
    if (len == 1) code = 0;
    first_code_[len] = code;
    offset_[len] = offset;
    offset += count_[len];
  }
  symbols_.assign(offset, 0);
  std::uint32_t fill[max_supported_bits + 2] = {};
  for (std::size_t s = 0; s < lengths.size(); ++s) {
    const std::uint8_t l = lengths[s];
    if (l != 0) symbols_[offset_[l] + fill[l]++] = static_cast<std::uint16_t>(s);
  }

  primary_bits_ = std::min(max_length_, 10);
  primary_mask_ = (1u << primary_bits_) - 1u;
  table_.assign(std::size_t{1} << primary_bits_, Entry{});
  for (int len = 1; len <= primary_bits_; ++len) {
    for (std::uint32_t i = 0; i < count_[len]; ++i) {
      const std::uint32_t c = first_code_[len] + i;
      const Entry e{symbols_[offset_[len] + i], static_cast<std::uint8_t>(len)};
      const std::uint32_t span = 1u << (primary_bits_ - len);
      if (lsb_first) {
        const std::uint32_t rev = reverse_bits(c, len);
        for (std::uint32_t k = 0; k < span; ++k) table_[rev | (k << len)] = e;
      } else {
        const std::uint32_t base = c << (primary_bits_ - len);
        for (std::uint32_t k = 0; k < span; ++k) table_[base + k] = e;
      }
    }
  }
  return true;
}

}  // namespace cvt::compression::detail
