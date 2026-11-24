// Convertible core - archive family: XPRESS decoders ([MS-XCA] plain LZ77 and LZ77+Huffman).
//
// Plain LZ77 (Method::XpressPlain): 32-bit little-endian flag words (most
// significant bit first: 0 = literal byte, 1 = match). A match is a 16-bit
// word (offset - 1 in the top 13 bits, length - 3 in the low 3 bits); length 7
// continues in a shared half byte, then a byte, then a 16-bit or 32-bit field.
//
// LZ77+Huffman (Method::XpressHuffman, the WIM "XPRESS" chunks): every 64 KiB
// of output starts with a 256-byte table of 512 4-bit code lengths. Symbols
// 0..255 are literals; 256 + (log2(offset) << 4 | length - 3) are matches,
// length nibble 15 continuing in a byte and a 16-bit field read from the byte
// stream between the 16-bit bit-buffer words. `output_size` is required.
// LZMS is not implemented (reported as unavailable).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>

#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/huffman.hpp"
#include "families/archive/codecs/zip_legacy_common.hpp"

namespace cvt::compression::detail {

namespace {

Result<Bytes> decode_plain(ByteView data, const DecodeOptions& options) {
  LegacyOutput out;
  CVT_TRY(out.start(options, "XPRESS"));
  const auto* in = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::size_t size = data.size();
  std::size_t pos = 0;
  std::uint32_t flags = 0;
  int flag_count = 0;
  std::size_t half_byte = 0;  // position of a pending shared length half byte (0 = none)
  std::uint32_t steps = 0;
  while (!out.done()) {
    if (flag_count == 0) {
      if (size - pos < 4) break;
      flags = legacy_le32(in + pos);
      pos += 4;
      flag_count = 32;
    }
    --flag_count;
    if (((flags >> flag_count) & 1u) == 0) {
      if (pos >= size) break;
      out.put(in[pos++]);
      continue;
    }
    if (pos >= size) break;  // the end of the data falls on a match flag
    if (size - pos < 2) return fail(Status::Decode, "XPRESS data is truncated");
    const std::uint32_t word = legacy_le16(in + pos);
    pos += 2;
    std::uint64_t length = word & 7u;
    const std::uint32_t offset = (word >> 3) + 1;
    if (length == 7) {
      if (half_byte == 0) {
        if (pos >= size) return fail(Status::Decode, "XPRESS data is truncated");
        length = in[pos] & 15u;
        half_byte = pos + 1;  // stored +1 so that 0 means "none"
        ++pos;
      } else {
        length = in[half_byte - 1] >> 4;
        half_byte = 0;
      }
      if (length == 15) {
        if (pos >= size) return fail(Status::Decode, "XPRESS data is truncated");
        length = in[pos++];
        if (length == 255) {
          if (size - pos < 2) return fail(Status::Decode, "XPRESS data is truncated");
          length = legacy_le16(in + pos);
          pos += 2;
          if (length == 0) {
            if (size - pos < 4) return fail(Status::Decode, "XPRESS data is truncated");
            length = legacy_le32(in + pos);
            pos += 4;
          }
          if (length < 15 + 7) return fail(Status::Decode, "XPRESS match length is invalid");
          length -= 15 + 7;
        }
        length += 15;
      }
      length += 7;
    }
    length += 3;
    if (!out.copy(offset, static_cast<std::size_t>(std::min<std::uint64_t>(length, out.room() + 1)))) {
      return fail(Status::Decode, "XPRESS match refers back before the start of the data");
    }
    if ((++steps & 0xFFFF) == 0 && !legacy_progress(options, pos, size)) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  return out.finish(pos < size);
}

/// [MS-XCA] LZ77+Huffman bit buffer: 32 bits, refilled with 16-bit words;
/// extra length bytes are taken from the byte stream at the refill position.
class XcaBits {
 public:
  XcaBits(const std::uint8_t* data, std::size_t size, std::size_t pos) : data_(data), size_(size), pos_(pos) {
    next_ = (std::uint32_t{word()} << 16);
    next_ |= word();
    extra_ = 16;
  }
  [[nodiscard]] std::uint32_t peek(int n) const noexcept { return n == 0 ? 0 : next_ >> (32 - n); }
  void skip(int n) noexcept {
    next_ <<= n;
    extra_ -= n;
    if (extra_ < 0) {
      next_ |= std::uint32_t{word()} << (-extra_);
      extra_ += 16;
    }
  }
  [[nodiscard]] std::uint32_t get(int n) noexcept {
    const std::uint32_t v = peek(n);
    skip(n);
    return v;
  }
  [[nodiscard]] bool byte(std::uint32_t& v) noexcept {
    if (pos_ >= size_) return false;
    v = data_[pos_++];
    return true;
  }
  [[nodiscard]] std::size_t pos() const noexcept { return pos_; }

 private:
  std::uint16_t word() noexcept {
    if (pos_ + 2 > size_) {
      // Past the end: zero bits. Only an error if they are actually used,
      // which the caller detects through `overrun()` after the block.
      if (pos_ < size_ + 4) pos_ += 2;
      overrun_words_ += 1;
      return 0;
    }
    const std::uint16_t w = legacy_le16(data_ + pos_);
    pos_ += 2;
    return w;
  }
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_;
  std::uint32_t next_ = 0;
  int extra_ = 0;

 public:
  int overrun_words_ = 0;
};

Result<Bytes> decode_huffman(ByteView data, const DecodeOptions& options) {
  if (!options.output_size) {
    return fail(Status::InvalidArgument, "XPRESS Huffman decoding needs the uncompressed size");
  }
  LegacyOutput out;
  CVT_TRY(out.start(options, "XPRESS Huffman"));
  const auto* in = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::size_t size = data.size();
  std::size_t pos = 0;
  HuffmanTable table;
  while (!out.done()) {
    if (size - pos < 256 + 4) return fail(Status::Decode, "XPRESS Huffman data is truncated");
    std::array<std::uint8_t, 512> lengths{};
    for (std::size_t i = 0; i < 256; ++i) {
      lengths[2 * i] = in[pos + i] & 15u;
      lengths[2 * i + 1] = in[pos + i] >> 4;
    }
    if (!table.build(lengths, false)) return fail(Status::Decode, "XPRESS Huffman code is invalid");
    XcaBits bits(in, size, pos + 256);
    const std::uint64_t block_end = out.size() + 65536;
    while (out.size() < block_end && !out.done()) {
      const int sym = table.decode(bits);
      if (sym < 0) return fail(Status::Decode, "XPRESS Huffman data is damaged");
      if (sym < 256) {
        out.put(static_cast<std::uint8_t>(sym));
      } else {
        const std::uint32_t s = static_cast<std::uint32_t>(sym - 256);
        std::uint32_t length = s & 15u;
        const int log2 = static_cast<int>(s >> 4);
        if (length == 15) {
          std::uint32_t b = 0;
          if (!bits.byte(b)) return fail(Status::Decode, "XPRESS Huffman data is truncated");
          length = b;
          if (length == 255) {
            std::uint32_t lo = 0;
            std::uint32_t hi = 0;
            if (!bits.byte(lo) || !bits.byte(hi)) return fail(Status::Decode, "XPRESS Huffman data is truncated");
            length = lo | (hi << 8);
            if (length < 15) return fail(Status::Decode, "XPRESS Huffman match length is invalid");
            length -= 15;
          }
          length += 15;
        }
        length += 3;
        const std::uint32_t offset = (1u << log2) | bits.get(log2);
        if (!out.copy(offset, length)) {
          return fail(Status::Decode, "XPRESS Huffman match refers back before the start of the data");
        }
      }
      if (bits.overrun_words_ > 2) return fail(Status::Decode, "XPRESS Huffman data is truncated");
    }
    pos = std::min(bits.pos(), size);
    if (!legacy_progress(options, pos, size)) return fail(Status::Cancelled, "the conversion was cancelled");
  }
  return out.finish(false);
}

constexpr std::array<CodecImpl, 2> table = {{
    {Method::XpressPlain, decode_plain, nullptr, nullptr, nullptr, nullptr},
    {Method::XpressHuffman, decode_huffman, nullptr, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> xpress_codecs() { return table; }

}  // namespace cvt::compression::detail
