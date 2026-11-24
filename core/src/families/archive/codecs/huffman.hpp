// Convertible core - archive family: canonical Huffman coding shared by the codecs.
//
//   huffman_lengths()   length-limited code lengths from symbol frequencies
//   canonical_codes()   canonical code values (MSB-first) from lengths
//   HuffmanTable        table-driven decoder for MSB-first and LSB-first bit readers
//
// A bit reader used with HuffmanTable provides `std::uint32_t peek(int n)`
// (n <= 32; bits past the end read as zero) and `void skip(int n)`. In an
// MSB-first reader the first bit of the stream is the most significant bit
// of `peek(n)`; in an LSB-first reader it is bit 0. Either way the first bit
// read is the most significant bit of the Huffman code.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace cvt::compression::detail {

[[nodiscard]] constexpr std::uint32_t reverse_bits(std::uint32_t value, int count) noexcept {
  std::uint32_t out = 0;
  for (int i = 0; i < count; ++i) {
    out = (out << 1) | (value & 1u);
    value >>= 1;
  }
  return out;
}

/// Code lengths for `freq` with no code longer than `max_bits`. Symbols with
/// frequency 0 get length 0. When only one symbol occurs it gets length 1
/// (and, if `min_two` is set, a second symbol is given length 1 too, since
/// some formats cannot express a one-symbol code).
void huffman_lengths(std::span<const std::uint32_t> freq, int max_bits, std::span<std::uint8_t> lengths,
                     bool min_two = false);

/// Canonical code values (MSB-first) for the given lengths.
void canonical_codes(std::span<const std::uint8_t> lengths, std::span<std::uint32_t> codes);

class HuffmanTable {
 public:
  static constexpr int max_supported_bits = 24;

  /// Build from code lengths (0 = unused). Fails (returns false) on an
  /// over-subscribed set or lengths above `max_supported_bits`. Incomplete
  /// codes are accepted unless `require_complete` is set; decoding a
  /// missing code then fails.
  bool build(std::span<const std::uint8_t> lengths, bool lsb_first, bool require_complete = false);

  /// Decode one symbol; -1 on an invalid code.
  template <class Reader>
  [[nodiscard]] int decode(Reader& reader) const {
    const std::uint32_t bits = reader.peek(primary_bits_);
    const Entry e = table_[bits & primary_mask_];
    if (e.length != 0) {
      reader.skip(e.length);
      return e.symbol;
    }
    return decode_slow(reader);
  }

  [[nodiscard]] int max_length() const noexcept { return max_length_; }
  [[nodiscard]] bool empty() const noexcept { return symbols_.empty(); }
  /// The only symbol of a code with exactly one symbol (formats that send a
  /// zero-bit code for it), or -1.
  [[nodiscard]] int single_symbol() const noexcept { return symbols_.size() == 1 ? symbols_[0] : -1; }

 private:
  struct Entry {
    std::uint16_t symbol = 0;
    std::uint8_t length = 0;  // 0 = not resolved by the primary table
  };

  template <class Reader>
  int decode_slow(Reader& reader) const {
    if (max_length_ <= primary_bits_) return -1;  // a short code that is not assigned
    std::uint32_t bits = reader.peek(max_length_);
    if (lsb_first_) bits = reverse_bits(bits, max_length_);
    for (int len = 1; len <= max_length_; ++len) {
      const std::uint32_t code = bits >> (max_length_ - len);
      const std::uint32_t index = code - first_code_[static_cast<std::size_t>(len)];
      if (code >= first_code_[static_cast<std::size_t>(len)] && index < count_[static_cast<std::size_t>(len)]) {
        reader.skip(len);
        return symbols_[offset_[static_cast<std::size_t>(len)] + index];
      }
    }
    return -1;
  }

  std::vector<Entry> table_;
  std::vector<std::uint16_t> symbols_;  // sorted by (length, symbol)
  std::uint32_t first_code_[max_supported_bits + 2] = {};
  std::uint32_t count_[max_supported_bits + 2] = {};
  std::uint32_t offset_[max_supported_bits + 2] = {};
  int max_length_ = 0;
  int primary_bits_ = 0;
  std::uint32_t primary_mask_ = 0;
  bool lsb_first_ = false;
};

}  // namespace cvt::compression::detail
