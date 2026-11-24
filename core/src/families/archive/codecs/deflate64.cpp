// Convertible core - archive family: Deflate64 (ZIP method 9) and CAB MSZIP decoders.
//
// Both use this file's own inflate engine (RFC 1951 block structure).
// Deflate64 differs from DEFLATE only in its 64 KiB window, length code 285
// (base 3 + 16 extra bits instead of the fixed 258) and the distance codes
// 30 and 31 (32769 and 49153, 14 extra bits each). MSZIP is plain DEFLATE
// cut into CAB data blocks: each block starts with the signature "CK", holds
// a complete deflate stream that ends with a final block on a byte boundary,
// and may refer back into the 32 KiB of output produced by earlier blocks of
// the same folder. One call decodes one folder; `block_sizes`, when given,
// are the uncompressed sizes of its blocks and are verified.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>

#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/bitio.hpp"
#include "families/archive/codecs/huffman.hpp"
#include "families/archive/codecs/zip_legacy_common.hpp"

namespace cvt::compression::detail {

namespace {

constexpr std::array<std::uint16_t, 29> length_base = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> length_extra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint32_t, 32> dist_base = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,    25,    33,    49,    65,    97,    129,   193,
    257,  385,  513,  769,  1025, 1537,  2049,  3073,  4097,  6145,  8193,  12289, 16385, 24577, 32769, 49153};
constexpr std::array<std::uint8_t, 32> dist_extra = {0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,  6,  6,
                                                     7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14};
constexpr std::array<std::uint8_t, 19> clen_order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

class Inflater {
 public:
  Inflater(ByteView data, LegacyOutput& out, bool deflate64, const DecodeOptions& options)
      : in_(data), out_(out), d64_(deflate64), options_(options) {}

  /// Decode blocks until the final block is done or the output is full.
  /// Returns true when the final block was completed.
  Result<bool> stream() {
    while (true) {
      // A prefix request stops as soon as it is satisfied; otherwise the
      // blocks are followed to the final one (MSZIP needs the exact end).
      if (out_.done() && out_.prefix()) return false;
      const bool final_block = in_.get(1) != 0;
      const std::uint32_t type = in_.get(2);
      if (in_.overrun()) return fail(Status::Decode, "{} data is truncated", name());
      if (type == 0) {
        CVT_TRY(stored());
      } else if (type == 1) {
        CVT_TRY(fixed_tables());
        CVT_TRY(codes());
      } else if (type == 2) {
        CVT_TRY(dynamic_tables());
        CVT_TRY(codes());
      } else {
        return fail(Status::Decode, "{} block has the reserved type 3", name());
      }
      if (mid_block_) return false;
      if (final_block) return true;
      if (++blocks_ % 16 == 0 && !legacy_progress(options_, in_.position() / 8, in_.size())) {
        return fail(Status::Cancelled, "the conversion was cancelled");
      }
    }
  }

  LsbBitReader& reader() noexcept { return in_; }

 private:
  std::string_view name() const noexcept { return d64_ ? "Deflate64" : "MSZIP"; }

  Result<void> stored() {
    in_.align();
    const std::uint32_t len = in_.get(16);
    const std::uint32_t nlen = in_.get(16);
    if (in_.overrun()) return fail(Status::Decode, "{} data is truncated", name());
    if ((len ^ 0xFFFFu) != nlen) return fail(Status::Decode, "{} stored block length check failed", name());
    const std::size_t at = in_.byte_position();
    if (len > in_.size() - std::min(at, in_.size())) return fail(Status::Decode, "{} data is truncated", name());
    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(len, out_.room()));
    out_.append_raw(bytes_ + at, take);
    mid_block_ = take < len;
    // Re-position the reader after the stored bytes.
    in_ = LsbBitReader(ByteView(reinterpret_cast<const std::byte*>(bytes_), in_.size()));
    in_.skip(0);
    skip_bytes(at + take);
    return {};
  }

  void skip_bytes(std::size_t n) {
    while (n >= 4) {
      in_.skip(32);
      n -= 4;
    }
    if (n != 0) in_.skip(static_cast<int>(n * 8));
  }

  Result<void> fixed_tables() {
    std::array<std::uint8_t, 288> lit{};
    for (std::size_t i = 0; i < 288; ++i) lit[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    std::array<std::uint8_t, 32> dist{};
    dist.fill(5);
    if (!lit_.build(lit, true) || !dist_.build(dist, true)) return fail(Status::Internal, "fixed Huffman tables");
    return {};
  }

  Result<void> dynamic_tables() {
    const std::uint32_t hlit = in_.get(5) + 257;
    const std::uint32_t hdist = in_.get(5) + 1;
    const std::uint32_t hclen = in_.get(4) + 4;
    if (hlit > 286 || (!d64_ && hdist > 30)) return fail(Status::Decode, "{} block declares too many codes", name());
    std::array<std::uint8_t, 19> clen{};
    for (std::uint32_t i = 0; i < hclen; ++i) clen[clen_order[i]] = static_cast<std::uint8_t>(in_.get(3));
    HuffmanTable pre;
    if (!pre.build(clen, true)) return fail(Status::Decode, "{} code length code is invalid", name());
    std::array<std::uint8_t, 320> lengths{};
    std::uint32_t n = 0;
    while (n < hlit + hdist) {
      const int sym = pre.decode(in_);
      if (sym < 0 || in_.overrun()) return fail(Status::Decode, "{} code lengths are damaged", name());
      if (sym < 16) {
        lengths[n++] = static_cast<std::uint8_t>(sym);
        continue;
      }
      std::uint32_t repeat = 0;
      std::uint8_t value = 0;
      if (sym == 16) {
        if (n == 0) return fail(Status::Decode, "{} repeats a code length that does not exist", name());
        value = lengths[n - 1];
        repeat = 3 + in_.get(2);
      } else if (sym == 17) {
        repeat = 3 + in_.get(3);
      } else {
        repeat = 11 + in_.get(7);
      }
      if (n + repeat > hlit + hdist) return fail(Status::Decode, "{} code lengths overflow", name());
      for (std::uint32_t k = 0; k < repeat; ++k) lengths[n++] = value;
    }
    if (lengths[256] == 0) return fail(Status::Decode, "{} block has no end-of-block code", name());
    if (!lit_.build(std::span<const std::uint8_t>(lengths.data(), hlit), true) ||
        !dist_.build(std::span<const std::uint8_t>(lengths.data() + hlit, hdist), true)) {
      return fail(Status::Decode, "{} Huffman code is over-subscribed", name());
    }
    return {};
  }

  Result<void> codes() {
    mid_block_ = false;
    while (true) {
      const int sym = lit_.decode(in_);
      if (sym != 256 && out_.done()) {
        mid_block_ = true;  // more data than the output may hold
        return {};
      }
      if (sym < 256) {
        if (sym < 0 || in_.overrun()) return fail(Status::Decode, "{} data is damaged or truncated", name());
        out_.put(static_cast<std::uint8_t>(sym));
        continue;
      }
      if (sym == 256) {
        if (in_.overrun()) return fail(Status::Decode, "{} data is truncated", name());
        return {};
      }
      const std::size_t li = static_cast<std::size_t>(sym - 257);
      if (li >= 29) return fail(Status::Decode, "{} uses an invalid length code", name());
      std::uint32_t length = 0;
      if (li == 28 && d64_) {
        length = 3 + in_.get(16);
      } else {
        length = length_base[li] + in_.get(length_extra[li]);
      }
      const int ds = dist_.decode(in_);
      if (ds < 0 || (!d64_ && ds >= 30)) return fail(Status::Decode, "{} uses an invalid distance code", name());
      const std::size_t di = static_cast<std::size_t>(ds);
      const std::uint32_t distance = dist_base[di] + in_.get(dist_extra[di]);
      if (in_.overrun()) return fail(Status::Decode, "{} data is truncated", name());
      if (!out_.copy(distance, length)) {
        return fail(Status::Decode, "{} refers back before the start of the data", name());
      }
    }
  }

  LsbBitReader in_;
  LegacyOutput& out_;
  bool d64_;
  const DecodeOptions& options_;
  const std::uint8_t* bytes_ = nullptr;
  HuffmanTable lit_;
  HuffmanTable dist_;
  bool mid_block_ = false;
  std::uint32_t blocks_ = 0;

 public:
  void set_bytes(const std::uint8_t* b) noexcept { bytes_ = b; }
};

Result<Bytes> decode_deflate64(ByteView data, const DecodeOptions& options) {
  LegacyOutput out;
  CVT_TRY(out.start(options, "Deflate64"));
  Inflater inflater(data, out, true, options);
  inflater.set_bytes(reinterpret_cast<const std::uint8_t*>(data.data()));
  CVT_TRY_ASSIGN(const bool ended, inflater.stream());
  if (!ended && !out.done()) return fail(Status::Decode, "Deflate64 data is truncated");
  return out.finish(!ended);
}

Result<Bytes> decode_mszip(ByteView data, const DecodeOptions& options) {
  LegacyOutput out;
  CVT_TRY(out.start(options, "MSZIP"));
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t offset = 0;
  std::size_t block = 0;
  bool more = false;
  while (offset < data.size() && !out.done()) {
    if (!options.block_sizes.empty() && block >= options.block_sizes.size()) {
      return fail(Status::Decode, "MSZIP folder holds more blocks than declared");
    }
    if (data.size() - offset < 2 || bytes[offset] != 'C' || bytes[offset + 1] != 'K') {
      return fail(Status::Decode, "MSZIP block {} does not start with the CK signature", block);
    }
    const std::size_t before = out.size();
    Inflater inflater(data.subspan(offset + 2), out, false, options);
    inflater.set_bytes(bytes + offset + 2);
    CVT_TRY_ASSIGN(const bool ended, inflater.stream());
    const std::size_t produced = out.size() - before;
    if (!ended) {
      if (out.done() && (out.prefix() || !out.exact())) {
        more = true;
        break;
      }
      return fail(Status::Decode, "MSZIP block {} holds more data than declared", block);
    }
    if (!options.block_sizes.empty() && produced != options.block_sizes[block] && !out.prefix()) {
      return fail(Status::Decode, "MSZIP block {} holds {} bytes instead of {}", block, produced,
                  options.block_sizes[block]);
    }
    inflater.reader().align();
    offset += 2 + inflater.reader().byte_position();
    ++block;
  }
  if (!out.prefix() && !options.block_sizes.empty() && block < options.block_sizes.size() && !out.done()) {
    return fail(Status::Decode, "MSZIP folder is truncated");
  }
  return out.finish(more || offset < data.size());
}

constexpr std::array<CodecImpl, 2> table = {{
    {Method::Deflate64, decode_deflate64, nullptr, nullptr, nullptr, nullptr},
    {Method::Mszip, decode_mszip, nullptr, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> deflate64_codecs() { return table; }

}  // namespace cvt::compression::detail
