// Convertible core - codec_legacy family: table-driven variable-length code decoding.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "families/codec_legacy/common/bitstream.hpp"

namespace cvt::legacy {

/// One code of a prefix code: `length` bits of `code` (MSB first) mean `symbol`.
struct VlcCode {
  std::uint32_t code = 0;
  std::uint8_t length = 0;
  std::int32_t symbol = 0;
};

/// Builds a code from a bit string such as "0010 1" (spaces ignored).
[[nodiscard]] constexpr VlcCode vlc(std::string_view bits, std::int32_t symbol) noexcept {
  VlcCode out{0, 0, symbol};
  for (const char c : bits) {
    if (c == '0' || c == '1') {
      out.code = (out.code << 1) | static_cast<std::uint32_t>(c - '0');
      ++out.length;
    }
  }
  return out;
}

/// A multi-level lookup table for a prefix code of up to 32 bits.
///
/// `decode` returns the symbol, or `Vlc::invalid` for a bit pattern that
/// is not a code (the bits of the longest matching prefix are consumed, at
/// least one). Codes are checked for being prefix-free at build time:
/// `build` returns false for an inconsistent code set.
class Vlc {
 public:
  static constexpr std::int32_t invalid = INT32_MIN;

  Vlc() = default;
  Vlc(std::span<const VlcCode> codes, int root_bits = 9) { ok_ = build(codes, root_bits); }

  bool build(std::span<const VlcCode> codes, int root_bits = 9) {
    table_.clear();
    root_bits_ = root_bits;
    std::vector<const VlcCode*> all;
    all.reserve(codes.size());
    for (const VlcCode& c : codes) {
      if (c.length == 0 || c.length > 32) return false;
      all.push_back(&c);
    }
    table_.assign(std::size_t{1} << root_bits, Entry{invalid, 0, 0});
    return fill(0, root_bits, all, 0);
  }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

  [[nodiscard]] std::int32_t decode(BitReader& reader) const noexcept {
    std::uint32_t base = 0;
    int bits = root_bits_;
    for (int level = 0; level < 8; ++level) {
      const Entry& e = table_[base + reader.peek(bits)];
      if (e.subtable_bits == 0) {
        reader.skip(e.length == 0 ? 1 : e.length);
        return e.length == 0 ? invalid : e.value;
      }
      reader.skip(bits);
      base = static_cast<std::uint32_t>(e.value);
      bits = e.subtable_bits;
    }
    return invalid;
  }

 private:
  struct Entry {
    std::int32_t value;          // symbol, or subtable offset when subtable_bits > 0
    std::uint8_t length;         // bits to consume for a leaf (0 = invalid)
    std::uint8_t subtable_bits;  // > 0: index bits of the subtable at `value`
  };

  // Fill the table at `base` (2^bits entries) with every code whose first
  // `prefix_length` bits have been consumed already.
  bool fill(std::size_t base, int bits, const std::vector<const VlcCode*>& codes, int prefix_length) {
    // Group codes longer than this level by their index in this table.
    std::vector<std::vector<const VlcCode*>> longer(std::size_t{1} << bits);
    for (const VlcCode* c : codes) {
      const int rest = c->length - prefix_length;
      const std::uint32_t tail = rest >= 32 ? c->code : (c->code & ((1u << rest) - 1u));
      if (rest <= bits) {
        const std::size_t first = static_cast<std::size_t>(tail) << (bits - rest);
        const std::size_t count = std::size_t{1} << (bits - rest);
        for (std::size_t i = 0; i < count; ++i) {
          Entry& e = table_[base + first + i];
          if (e.length != 0 || e.subtable_bits != 0) return false;  // not prefix-free
          e = Entry{c->symbol, static_cast<std::uint8_t>(rest), 0};
        }
      } else {
        longer[tail >> (rest - bits)].push_back(c);
      }
    }
    for (std::size_t index = 0; index < longer.size(); ++index) {
      if (longer[index].empty()) continue;
      if (table_[base + index].length != 0) return false;
      int max_rest = 0;
      for (const VlcCode* c : longer[index]) max_rest = std::max(max_rest, c->length - prefix_length - bits);
      const int sub_bits = std::min(max_rest, 9);
      const std::size_t sub_base = table_.size();
      table_.resize(sub_base + (std::size_t{1} << sub_bits), Entry{invalid, 0, 0});
      table_[base + index] = Entry{static_cast<std::int32_t>(sub_base), 0, static_cast<std::uint8_t>(sub_bits)};
      if (!fill(sub_base, sub_bits, longer[index], prefix_length + bits)) return false;
    }
    return true;
  }

  std::vector<Entry> table_;
  int root_bits_ = 9;
  bool ok_ = false;
};

}  // namespace cvt::legacy
