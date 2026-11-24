// Convertible core - archive family: PKZIP 1.x methods Shrink (1), Reduce (2..5) and Implode (6),
// decoded following the descriptions in PKWARE's APPNOTE.TXT.
//
//   Shrink: LZW with 9..13-bit codes (LSB first). Code 256 is an escape:
//       followed by 1 = one bit wider codes, 2 = partial clear (every code
//       that is not the prefix of another code is freed). New strings take the
//       lowest free code.
//   Reduce: 256 "follower sets" (the bytes likely to follow each byte),
//       then bytes coded as a 1-bit escape + 8 bits or an index into the
//       current follower set; the byte stream is a run/back-reference coding
//       with the escape byte 144 (factor 1..4 = `level`).
//   Implode: Shannon-Fano trees (literal tree optional, `flags` bit 2;
//       length and distance trees), 4 KiB or 8 KiB window (`flags` bit 1).
//       The stored trees are lists of (count, bit length) bytes; the codes are
//       the bit-complement of canonical codes, read LSB first.
// None of these marks its end: `output_size` is required.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <vector>

#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/bitio.hpp"
#include "families/archive/codecs/huffman.hpp"
#include "families/archive/codecs/zip_legacy_common.hpp"

namespace cvt::compression::detail {

namespace {

Result<void> need_size(const DecodeOptions& options, std::string_view name) {
  if (!options.output_size) return fail(Status::InvalidArgument, "{} decoding needs the uncompressed size", name);
  return {};
}

/// LZ copy where positions before the start of the data read as zeros.
void copy_zeros(LegacyOutput& out, std::size_t distance, std::size_t length) {
  if (distance <= out.size()) {
    (void)out.copy(distance, length);
    return;
  }
  if (length > out.room()) {
    out.mark_overflow();
    length = static_cast<std::size_t>(out.room());
  }
  for (std::size_t k = 0; k < length; ++k) {
    const std::size_t n = out.size();
    out.put(distance > n ? std::uint8_t{0} : out.at(n - distance));
  }
}

// ---------------------------------------------------------------------------- Shrink

Result<Bytes> decode_shrink(ByteView data, const DecodeOptions& options) {
  CVT_TRY(need_size(options, "Shrink"));
  LegacyOutput out;
  CVT_TRY(out.start(options, "Shrink"));
  constexpr int max_bits = 13;
  constexpr std::uint32_t table_size = 1u << max_bits;
  constexpr std::uint32_t first_free = 257;
  constexpr std::uint32_t none = 0xFFFFFFFFu;
  std::vector<std::uint16_t> prefix(table_size, 0);
  std::vector<std::uint8_t> suffix(table_size, 0);
  std::vector<std::uint8_t> used(table_size, 0);
  for (std::uint32_t i = 0; i < 256; ++i) {
    suffix[i] = static_cast<std::uint8_t>(i);
    used[i] = 1;
  }
  used[256] = 1;
  std::vector<std::uint8_t> stack(table_size + 1);
  std::uint32_t next_free = first_free;
  const auto advance_free = [&] {
    while (next_free < table_size && used[next_free]) ++next_free;
  };
  LsbBitReader in(data);
  const std::uint64_t total_bits = std::uint64_t{data.size()} * 8;
  int width = 9;
  std::uint32_t prev = none;
  std::uint32_t steps = 0;
  while (!out.done()) {
    if (in.position() + static_cast<std::uint64_t>(width) > total_bits) break;
    const std::uint32_t code = in.get(width);
    if (code == 256) {
      const std::uint32_t control = in.get(width);
      if (in.overrun()) break;
      if (control == 1) {
        if (width == max_bits) return fail(Status::Decode, "Shrink code width exceeds 13 bits");
        ++width;
      } else if (control == 2) {
        std::vector<std::uint8_t> is_prefix(table_size, 0);
        for (std::uint32_t i = first_free; i < table_size; ++i) {
          if (used[i]) is_prefix[prefix[i]] = 1;
        }
        for (std::uint32_t i = first_free; i < table_size; ++i) {
          if (used[i] && !is_prefix[i]) used[i] = 0;
        }
        next_free = first_free;
        advance_free();
      } else {
        return fail(Status::Decode, "Shrink stream has an unknown control code {}", control);
      }
      continue;
    }
    // Expand `code` (the "KwKwK" case: the code about to be defined).
    std::size_t depth = 0;
    std::uint32_t c = code;
    bool kwk = false;
    if (!used[c]) {
      if (prev == none || c != next_free) return fail(Status::Decode, "Shrink stream uses an undefined code");
      kwk = true;
      c = prev;
    }
    while (c >= first_free) {
      if (depth >= table_size || !used[c]) return fail(Status::Decode, "Shrink code table is damaged");
      stack[depth++] = suffix[c];
      c = prefix[c];
    }
    stack[depth++] = static_cast<std::uint8_t>(c);
    const std::uint8_t first = stack[depth - 1];
    while (depth > 0 && !out.done()) out.put(stack[--depth]);
    if (kwk && !out.done()) out.put(first);
    if (prev != none && next_free < table_size) {
      prefix[next_free] = static_cast<std::uint16_t>(prev);
      suffix[next_free] = first;
      used[next_free] = 1;
      advance_free();
    }
    prev = code;
    if ((++steps & 0xFFFF) == 0 && !legacy_progress(options, in.position() / 8, data.size())) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  return out.finish(false);
}

// ---------------------------------------------------------------------------- Reduce

Result<Bytes> decode_reduce(ByteView data, const DecodeOptions& options) {
  CVT_TRY(need_size(options, "Reduce"));
  const int factor = options.level;
  if (factor < 1 || factor > 4) return fail(Status::InvalidArgument, "Reduce factor must be 1..4");
  LegacyOutput out;
  CVT_TRY(out.start(options, "Reduce"));
  LsbBitReader in(data);
  std::array<std::array<std::uint8_t, 64>, 256> followers{};
  std::array<std::uint8_t, 256> count{};
  for (int j = 255; j >= 0; --j) {
    const std::uint32_t n = in.get(6);
    if (n > 32) return fail(Status::Decode, "Reduce follower set is too large");
    count[static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(n);
    for (std::uint32_t k = 0; k < n; ++k) followers[static_cast<std::size_t>(j)][k] = static_cast<std::uint8_t>(in.get(8));
  }
  if (in.overrun()) return fail(Status::Decode, "Reduce data is truncated");
  const auto bits_for = [](std::uint32_t n) {
    int b = 1;
    while ((1u << b) < n) ++b;
    return b;
  };
  const std::uint32_t mask = 0x7Fu >> (factor - 1);
  std::uint8_t last = 0;
  int state = 0;
  std::uint32_t v = 0;
  std::uint32_t length = 0;
  std::uint32_t steps = 0;
  while (!out.done()) {
    std::uint8_t c = 0;
    const std::uint32_t n = count[last];
    if (n == 0 || in.get(1) != 0) {
      c = static_cast<std::uint8_t>(in.get(8));
    } else {
      const std::uint32_t index = in.get(bits_for(n));
      if (index >= n) return fail(Status::Decode, "Reduce follower index is out of range");
      c = followers[last][index];
    }
    if (in.overrun()) return fail(Status::Decode, "Reduce data is truncated");
    last = c;
    switch (state) {
      case 0:
        if (c != 144) {
          out.put(c);
        } else {
          state = 1;
        }
        break;
      case 1:
        if (c == 0) {
          out.put(144);
          state = 0;
        } else {
          v = c;
          length = v & mask;
          state = length == mask ? 2 : 3;
        }
        break;
      case 2:
        length += c;
        state = 3;
        break;
      default: {
        const std::size_t distance = ((v >> (8 - factor)) << 8) + c + 1;
        copy_zeros(out, distance, length + 3);
        state = 0;
        break;
      }
    }
    if ((++steps & 0xFFFF) == 0 && !legacy_progress(options, in.position() / 8, data.size())) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  return out.finish(false);
}

// ---------------------------------------------------------------------------- Implode

/// LSB-first reader that hands out complemented bits (Shannon-Fano codes).
class ComplementReader {
 public:
  explicit ComplementReader(LsbBitReader& in) : in_(in) {}
  std::uint32_t peek(int n) { return n == 0 ? 0 : ~in_.peek(n) & ((1u << n) - 1u); }
  void skip(int n) { in_.skip(n); }

 private:
  LsbBitReader& in_;
};

Result<void> read_sf_tree(LsbBitReader& in, std::size_t symbols, HuffmanTable& table) {
  const std::uint32_t bytes = in.get(8) + 1;
  std::vector<std::uint8_t> lengths;
  lengths.reserve(symbols);
  for (std::uint32_t i = 0; i < bytes; ++i) {
    const std::uint32_t b = in.get(8);
    const std::uint32_t len = (b & 15u) + 1;
    const std::uint32_t n = (b >> 4) + 1;
    if (lengths.size() + n > symbols) return fail(Status::Decode, "Implode tree describes too many symbols");
    lengths.insert(lengths.end(), n, static_cast<std::uint8_t>(len));
  }
  if (in.overrun()) return fail(Status::Decode, "Implode data is truncated");
  if (lengths.size() != symbols) return fail(Status::Decode, "Implode tree describes too few symbols");
  if (!table.build(lengths, true)) return fail(Status::Decode, "Implode tree is over-subscribed");
  return {};
}

Result<Bytes> decode_implode(ByteView data, const DecodeOptions& options) {
  CVT_TRY(need_size(options, "Implode"));
  LegacyOutput out;
  CVT_TRY(out.start(options, "Implode"));
  const bool big_window = (options.flags & 2u) != 0;
  const bool literal_tree = (options.flags & 4u) != 0;
  LsbBitReader in(data);
  HuffmanTable lit;
  HuffmanTable len;
  HuffmanTable dist;
  if (literal_tree) CVT_TRY(read_sf_tree(in, 256, lit));
  CVT_TRY(read_sf_tree(in, 64, len));
  CVT_TRY(read_sf_tree(in, 64, dist));
  ComplementReader sf(in);
  const int low_bits = big_window ? 7 : 6;
  const std::uint32_t min_match = literal_tree ? 3 : 2;
  std::uint32_t steps = 0;
  while (!out.done()) {
    if (in.get(1) != 0) {
      int c = 0;
      if (literal_tree) {
        c = lit.decode(sf);
        if (c < 0) return fail(Status::Decode, "Implode data is damaged");
      } else {
        c = static_cast<int>(in.get(8));
      }
      out.put(static_cast<std::uint8_t>(c));
    } else {
      const std::uint32_t low = in.get(low_bits);
      const int high = dist.decode(sf);
      int l = len.decode(sf);
      if (high < 0 || l < 0) return fail(Status::Decode, "Implode data is damaged");
      std::uint32_t length = static_cast<std::uint32_t>(l);
      if (length == 63) length += in.get(8);
      length += min_match;
      const std::size_t distance = ((static_cast<std::size_t>(high) << low_bits) | low) + 1;
      copy_zeros(out, distance, length);
    }
    if (in.overrun()) return fail(Status::Decode, "Implode data is truncated");
    if ((++steps & 0xFFFF) == 0 && !legacy_progress(options, in.position() / 8, data.size())) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  return out.finish(false);
}

constexpr std::array<CodecImpl, 3> table = {{
    {Method::Implode, decode_implode, nullptr, nullptr, nullptr, nullptr},
    {Method::Shrink, decode_shrink, nullptr, nullptr, nullptr, nullptr},
    {Method::Reduce, decode_reduce, nullptr, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> zip_legacy_codecs() { return table; }

}  // namespace cvt::compression::detail
