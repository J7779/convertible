// Convertible core - archive family: LZO1X decoder (Method::Lzo1x), as used by lzop and SquashFS.
//
// One raw LZO1X block (any of the LZO1X-1 .. LZO1X-999 encoders), decoded
// following the published description of the instruction set: the state
// (number of literals copied by the previous instruction, 0..4) selects the
// meaning of the short instructions 0..15; 16..31 are far matches (the
// distance 16384 with length 3, bytes 11 00 00, ends the block); 32..63 are
// matches within 16 KiB; 64..255 are short matches within 2 KiB. Every match
// is followed by 0..3 literals given in its low two bits.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>

#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/zip_legacy_common.hpp"

namespace cvt::compression::detail {

namespace {

class LzoReader {
 public:
  explicit LzoReader(ByteView data) noexcept
      : p_(reinterpret_cast<const std::uint8_t*>(data.data())), size_(data.size()) {}
  [[nodiscard]] bool has(std::size_t n) const noexcept { return size_ - pos_ >= n; }
  [[nodiscard]] std::uint8_t byte() noexcept { return p_[pos_++]; }
  [[nodiscard]] const std::uint8_t* here() const noexcept { return p_ + pos_; }
  void advance(std::size_t n) noexcept { pos_ += n; }
  [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  /// Length with zero-byte extension: `bits` (0 means extended) + `mask`.
  [[nodiscard]] bool length(std::uint32_t bits, std::uint32_t mask, std::uint64_t& out) noexcept {
    if (bits != 0) {
      out = bits;
      return true;
    }
    std::uint64_t n = mask;
    while (true) {
      if (!has(1)) return false;
      const std::uint8_t b = byte();
      if (b != 0) {
        out = n + b;
        return true;
      }
      n += 255;
      if (n > (std::uint64_t{1} << 32)) return false;
    }
  }

 private:
  const std::uint8_t* p_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

Result<Bytes> decode_lzo1x(ByteView data, const DecodeOptions& options) {
  LegacyOutput out;
  CVT_TRY(out.start(options, "LZO1X"));
  LzoReader in(data);
  const auto truncated = [] { return fail(Status::Decode, "LZO1X data is truncated"); };
  const auto copy_literals = [&](std::uint64_t n) -> bool {
    if (n > in.size() || !in.has(static_cast<std::size_t>(n))) return false;
    out.append_raw(in.here(), static_cast<std::size_t>(n));
    in.advance(static_cast<std::size_t>(n));
    return true;
  };
  int state = 0;
  bool ended = false;
  std::uint32_t steps = 0;
  if (!in.has(1)) return truncated();
  if (in.here()[0] > 17) {
    const std::uint32_t n = in.byte() - 17u;
    if (!copy_literals(n)) return truncated();
    state = n < 4 ? static_cast<int>(n) : 4;
  }
  while (!out.done()) {
    if (!in.has(1)) return truncated();
    const std::uint32_t op = in.byte();
    std::uint64_t length = 0;
    std::uint64_t distance = 0;
    std::uint32_t trailing = 0;
    if (op < 16) {
      if (state == 0) {
        if (!in.length(op, 15, length)) return truncated();
        length += 3;
        if (!copy_literals(length)) return truncated();
        state = 4;
        continue;
      }
      if (!in.has(1)) return truncated();
      const std::uint32_t h = in.byte();
      trailing = op & 3u;
      if (state == 4) {
        distance = (h << 2) + (op >> 2) + 2049;
        length = 3;
      } else {
        distance = (h << 2) + (op >> 2) + 1;
        length = 2;
      }
    } else if (op < 32) {
      if (!in.length(op & 7u, 7, length)) return truncated();
      length += 2;
      if (!in.has(2)) return truncated();
      const std::uint32_t w = legacy_le16(in.here());
      in.advance(2);
      trailing = w & 3u;
      distance = 16384u + ((op & 8u) << 11) + (w >> 2);
      if (distance == 16384) {
        ended = true;
        break;
      }
    } else if (op < 64) {
      if (!in.length(op & 31u, 31, length)) return truncated();
      length += 2;
      if (!in.has(2)) return truncated();
      const std::uint32_t w = legacy_le16(in.here());
      in.advance(2);
      trailing = w & 3u;
      distance = (w >> 2) + 1;
    } else {
      if (!in.has(1)) return truncated();
      const std::uint32_t h = in.byte();
      trailing = op & 3u;
      distance = (h << 3) + ((op >> 2) & 7u) + 1;
      length = op < 128 ? 3 + ((op >> 5) & 1u) : 5 + ((op >> 5) & 3u);
    }
    if (!out.copy(static_cast<std::size_t>(distance), static_cast<std::size_t>(std::min<std::uint64_t>(length, out.room() + 1)))) {
      return fail(Status::Decode, "LZO1X match refers back before the start of the data");
    }
    if (out.done()) break;
    if (!copy_literals(trailing)) return truncated();
    state = static_cast<int>(trailing);
    if ((++steps & 0xFFFF) == 0 && !legacy_progress(options, in.pos(), in.size())) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  if (ended && out.exact() && !out.prefix() && out.size() != out.target()) {
    return fail(Status::Decode, "LZO1X block holds {} bytes instead of {}", out.size(), out.target());
  }
  return out.finish(!ended);
}

constexpr std::array<CodecImpl, 1> table = {{
    {Method::Lzo1x, decode_lzo1x, nullptr, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> lzo_codecs() { return table; }

}  // namespace cvt::compression::detail
