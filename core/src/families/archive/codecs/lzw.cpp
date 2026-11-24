// Convertible core - archive family: Unix `compress` (.Z) LZW (Method::Lzw).
//
// Codes are packed LSB first. `compress` writes them in groups of n_bits
// bytes (eight codes); when the code width grows or the table is cleared,
// the rest of the current group is padding. Groups are counted from the
// end of the 3-byte header for the first width and
// from the end of the previous padding afterwards. The encoder mirrors
// ncompress: 16-bit maximum, block mode, and a CLEAR code when the
// compression ratio starts falling after the table is full.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <vector>

#include "families/archive/codec_impl.hpp"

namespace cvt::compression::detail {

namespace {

constexpr int init_bits = 9;
constexpr std::uint32_t clear_code = 256;
constexpr std::uint32_t first_code = 257;

std::uint64_t align_up(std::uint64_t pos, std::uint64_t origin, int n_bits) {
  const std::uint64_t group = static_cast<std::uint64_t>(n_bits) * 8;
  const std::uint64_t rel = pos - origin;
  return origin + (rel + group - 1) / group * group;
}

Result<Bytes> decode_lzw(ByteView data, const DecodeOptions& o) {
  if (data.size() < 3 || std::to_integer<int>(data[0]) != 0x1F || std::to_integer<int>(data[1]) != 0x9D) {
    return fail(Status::Decode, "not a compress (.Z) file");
  }
  const std::uint8_t flags = std::to_integer<std::uint8_t>(data[2]);
  const int max_bits = flags & 0x1F;
  const bool block_mode = (flags & 0x80u) != 0;
  if (max_bits < init_bits || max_bits > 16 || (flags & 0x60u) != 0) {
    return fail(Status::Decode, "compress header uses {} bits, which is not supported", max_bits);
  }
  const std::uint32_t max_max_code = 1u << max_bits;
  std::vector<std::uint16_t> prefix(max_max_code, 0);
  std::vector<std::uint8_t> suffix(max_max_code, 0);
  for (std::uint32_t i = 0; i < 256; ++i) suffix[i] = static_cast<std::uint8_t>(i);
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::uint64_t total_bits = static_cast<std::uint64_t>(data.size()) * 8;
  std::uint64_t pos = 24;
  std::uint64_t origin = 24;  // groups count from the end of the 3-byte header
  int n_bits = init_bits;
  std::uint32_t max_code = (1u << n_bits) - 1;
  std::uint32_t free_ent = block_mode ? first_code : 256;
  std::int64_t old_code = -1;
  std::uint8_t fin_char = 0;
  Bytes out;
  std::vector<std::uint8_t> stack;
  stack.reserve(max_max_code);
  const std::uint64_t limit = o.stop_after != 0 ? std::min<std::uint64_t>(o.max_output, o.stop_after + max_max_code) : o.max_output;
  while (true) {
    if (free_ent > max_code) {
      pos = align_up(pos, origin, n_bits);
      origin = pos;
      ++n_bits;
      max_code = n_bits == max_bits ? max_max_code : (1u << n_bits) - 1;
    }
    if (pos + static_cast<std::uint64_t>(n_bits) > total_bits) break;
    std::uint32_t code = 0;
    for (int k = 0; k < n_bits; ++k) {
      const std::uint64_t bit = pos + static_cast<std::uint64_t>(k);
      code |= static_cast<std::uint32_t>((p[bit >> 3] >> (bit & 7)) & 1u) << k;
    }
    pos += static_cast<std::uint64_t>(n_bits);
    if (old_code == -1) {
      if (code >= 256) return fail(Status::Decode, "compress data is damaged");
      old_code = code;
      fin_char = static_cast<std::uint8_t>(code);
      CVT_TRY(check_output_room(out.size(), 1, limit));
      out.push_back(static_cast<std::byte>(fin_char));
      continue;
    }
    if (code == clear_code && block_mode) {
      free_ent = first_code - 1;
      pos = align_up(pos, origin, n_bits);
      origin = pos;
      n_bits = init_bits;
      max_code = (1u << n_bits) - 1;
      continue;
    }
    const std::uint32_t in_code = code;
    stack.clear();
    if (code >= free_ent) {
      if (code > free_ent) return fail(Status::Decode, "compress data is damaged (code {} beyond the table)", code);
      stack.push_back(fin_char);
      code = static_cast<std::uint32_t>(old_code);
    }
    while (code >= 256) {
      stack.push_back(suffix[code]);
      code = prefix[code];
      if (stack.size() > max_max_code) return fail(Status::Decode, "compress data is damaged");
    }
    fin_char = suffix[code];
    stack.push_back(fin_char);
    CVT_TRY(check_output_room(out.size(), stack.size(), limit));
    for (std::size_t k = stack.size(); k-- > 0;) out.push_back(static_cast<std::byte>(stack[k]));
    if (free_ent < max_max_code) {
      prefix[free_ent] = static_cast<std::uint16_t>(old_code);
      suffix[free_ent] = fin_char;
      ++free_ent;
    }
    old_code = in_code;
    if (o.stop_after != 0 && out.size() >= o.stop_after) break;
  }
  if (o.stop_after != 0 && out.size() > o.stop_after) out.resize(static_cast<std::size_t>(o.stop_after));
  if (o.output_size && o.stop_after == 0 && out.size() != *o.output_size) {
    return fail(Status::Decode, "compress data decodes to {} bytes, expected {}", out.size(), *o.output_size);
  }
  return out;
}

class CodeWriter {
 public:
  explicit CodeWriter(Bytes& out) : out_(&out) {}
  void put(std::uint32_t code, int n_bits) {
    for (int k = 0; k < n_bits; ++k) put_bit((code >> k) & 1u);
  }
  void pad_to(std::uint64_t target) {
    while (pos_ < target) put_bit(0);
  }
  [[nodiscard]] std::uint64_t position() const noexcept { return pos_; }
  void flush() {
    if (pos_ % 8 != 0) out_->push_back(static_cast<std::byte>(acc_));
  }

 private:
  void put_bit(std::uint32_t b) {
    acc_ |= static_cast<std::uint8_t>(b << (pos_ % 8));
    ++pos_;
    if (pos_ % 8 == 0) {
      out_->push_back(static_cast<std::byte>(acc_));
      acc_ = 0;
    }
  }
  Bytes* out_;
  std::uint64_t pos_ = 24;
  std::uint8_t acc_ = 0;
};

Result<Bytes> encode_lzw(ByteView data, const EncodeOptions& e) {
  constexpr int max_bits = 16;
  constexpr std::uint32_t max_max_code = 1u << max_bits;
  constexpr std::uint64_t check_gap = 10000;
  Bytes out = {std::byte{0x1F}, std::byte{0x9D}, std::byte{0x80 | max_bits}};
  CodeWriter w(out);
  if (data.empty()) return out;
  // Open-addressed dictionary: (prefix code, byte) -> code.
  constexpr std::size_t hsize = 1u << 18;
  std::vector<std::uint32_t> keys(hsize, 0xFFFFFFFFu);
  std::vector<std::uint32_t> codes(hsize, 0);
  const auto reset_table = [&] { std::fill(keys.begin(), keys.end(), 0xFFFFFFFFu); };
  int n_bits = init_bits;
  std::uint32_t max_code = (1u << n_bits) - 1;
  std::uint32_t free_ent = first_code;
  std::uint64_t origin = 24;  // groups count from the end of the 3-byte header
  std::uint64_t in_count = 1;
  std::uint64_t checkpoint = check_gap;
  std::uint64_t ratio = 0;
  const auto output = [&](std::uint32_t code) {
    w.put(code, n_bits);
    if (free_ent > max_code) {
      const std::uint64_t aligned = align_up(w.position(), origin, n_bits);
      w.pad_to(aligned);
      origin = aligned;
      ++n_bits;
      max_code = n_bits == max_bits ? max_max_code : (1u << n_bits) - 1;
    }
  };
  std::uint32_t ent = std::to_integer<std::uint32_t>(data[0]);
  for (std::size_t i = 1; i < data.size(); ++i) {
    if ((i & 0xFFFFu) == 0 && !e.progress.report(static_cast<double>(i) / static_cast<double>(data.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    const std::uint32_t c = std::to_integer<std::uint32_t>(data[i]);
    ++in_count;
    const std::uint32_t key = (ent << 8) | c;
    std::size_t h = (static_cast<std::size_t>(key) * 2654435761u) & (hsize - 1);
    while (keys[h] != 0xFFFFFFFFu && keys[h] != key) h = (h + 1) & (hsize - 1);
    if (keys[h] == key) {
      ent = codes[h];
      continue;
    }
    output(ent);
    ent = c;
    if (free_ent < max_max_code) {
      keys[h] = key;
      codes[h] = free_ent++;
    } else if (in_count >= checkpoint) {
      // Table full: clear it when the ratio stops improving (like ncompress).
      checkpoint = in_count + check_gap;
      const std::uint64_t bytes_out = (w.position() + 7) / 8;
      const std::uint64_t r = bytes_out == 0 ? 0 : (in_count << 8) / bytes_out;
      if (r > ratio) {
        ratio = r;
      } else {
        ratio = 0;
        reset_table();
        w.put(clear_code, n_bits);
        const std::uint64_t aligned = align_up(w.position(), origin, n_bits);
        w.pad_to(aligned);
        origin = aligned;
        n_bits = init_bits;
        max_code = (1u << n_bits) - 1;
        free_ent = first_code;
      }
    }
  }
  output(ent);
  w.flush();
  return out;
}

constexpr std::array<CodecImpl, 1> table = {{
    {Method::Lzw, decode_lzw, encode_lzw, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> lzw_codecs() { return table; }

}  // namespace cvt::compression::detail
