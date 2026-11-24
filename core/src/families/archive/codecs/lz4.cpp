// Convertible core - archive family: LZ4 block and frame formats (Method::Lz4Block, Method::Lz4Frame).
//
// Encoder: levels 0-2 use a single-probe hash table (fast mode), levels 3-9
// hash chains with growing depth and one-step lazy matching (HC mode).
// Frames use linked 4 MiB blocks by default, xxHash32 content checksums and
// the content size when the options ask for them.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include "families/archive/checksums.hpp"
#include "families/archive/codec_impl.hpp"

namespace cvt::compression::detail {

namespace {

constexpr std::uint32_t frame_magic = 0x184D2204u;
constexpr std::uint32_t legacy_magic = 0x184C2102u;
constexpr std::size_t min_match = 4;
constexpr std::size_t last_literals = 5;
constexpr std::size_t mf_limit = 12;
constexpr std::size_t max_distance = 65535;

std::uint32_t le32(const std::uint8_t* p) {
  return p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}
void put32(Bytes& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

// Decode one block into `out` (which also serves as history for linked blocks).
Result<void> decode_block(ByteView block, Bytes& out, std::size_t history_start, std::uint64_t max_output,
                          std::uint64_t block_max) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(block.data());
  const std::size_t n = block.size();
  std::size_t i = 0;
  const std::size_t start = out.size();
  while (i < n) {
    const std::uint8_t token = p[i++];
    std::size_t lit = token >> 4;
    if (lit == 15) {
      std::uint8_t b;
      do {
        if (i >= n) return fail(Status::Decode, "LZ4 block is truncated");
        b = p[i++];
        lit += b;
        if (lit > block_max) return fail(Status::Decode, "LZ4 literal run is too long");
      } while (b == 255);
    }
    if (lit > n - i) return fail(Status::Decode, "LZ4 block is truncated");
    CVT_TRY(check_output_room(out.size(), lit, max_output));
    if (out.size() - start + lit > block_max) return fail(Status::Decode, "LZ4 block is larger than declared");
    out.insert(out.end(), block.begin() + static_cast<std::ptrdiff_t>(i), block.begin() + static_cast<std::ptrdiff_t>(i + lit));
    i += lit;
    if (i == n) break;  // last sequence: literals only
    if (i + 2 > n) return fail(Status::Decode, "LZ4 block is truncated");
    const std::size_t offset = p[i] | (static_cast<std::size_t>(p[i + 1]) << 8);
    i += 2;
    std::size_t len = (token & 15u) + min_match;
    if ((token & 15u) == 15) {
      std::uint8_t b;
      do {
        if (i >= n) return fail(Status::Decode, "LZ4 block is truncated");
        b = p[i++];
        len += b;
        if (len > block_max) return fail(Status::Decode, "LZ4 match is too long");
      } while (b == 255);
    }
    if (offset == 0 || offset > out.size() - history_start) return fail(Status::Decode, "LZ4 match offset is out of range");
    CVT_TRY(check_output_room(out.size(), len, max_output));
    if (out.size() - start + len > block_max) return fail(Status::Decode, "LZ4 block is larger than declared");
    const std::size_t from = out.size() - offset;
    if (offset >= len) {
      out.insert(out.end(), out.begin() + static_cast<std::ptrdiff_t>(from), out.begin() + static_cast<std::ptrdiff_t>(from + len));
    } else {
      for (std::size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
    }
  }
  return {};
}

// ------------------------------------------------------------------------------- block encoder

class BlockEncoder {
 public:
  BlockEncoder(const std::uint8_t* data, std::size_t size, int level) : data_(data), size_(size), level_(level) {
    hc_ = level >= 3;
    depth_ = level <= 2 ? 1 : (1 << std::min(level - 1, 9));
    head_.assign(std::size_t{1} << hash_log, 0);
    if (hc_) chain_.assign(std::size_t{1} << 16, 0);
  }

  // Encode data[begin, end) with history from data[max(0, begin - 64K), begin).
  void encode(std::size_t begin, std::size_t end, Bytes& out) {
    insert_until(begin > max_distance ? begin - max_distance : 0, begin);
    std::size_t anchor = begin;
    std::size_t pos = begin;
    const std::size_t limit = end >= mf_limit ? end - mf_limit : begin;
    while (pos < limit && end - begin >= mf_limit + 1) {
      Match m = find(pos, end - last_literals);
      if (m.len < min_match) {
        pos += 1 + (hc_ ? 0 : ((pos - anchor) >> 6));
        continue;
      }
      if (hc_ && pos + 1 < limit) {
        const Match next = find(pos + 1, end - last_literals);
        if (next.len > m.len + 1) {
          m = next;
          ++pos;
        }
      }
      emit(out, anchor, pos, m);
      pos += m.len;
      insert_until(next_, std::min(pos, limit));
      anchor = pos;
    }
    // Trailing literals.
    emit_literals(out, anchor, end);
  }

 private:
  static constexpr int hash_log = 16;
  struct Match {
    std::size_t len = 0;
    std::size_t offset = 0;
  };
  std::uint32_t hash(std::size_t pos) const {
    return (le32(data_ + pos) * 2654435761u) >> (32 - hash_log);
  }
  void insert(std::size_t pos) {
    const std::uint32_t h = hash(pos);
    if (hc_) chain_[pos & 0xFFFFu] = static_cast<std::uint32_t>(head_[h]);
    head_[h] = static_cast<std::uint32_t>(pos + 1);
  }
  void insert_until(std::size_t from, std::size_t to) {
    if (next_ < from) next_ = from;
    while (next_ < to) {
      if (next_ + 4 <= size_) insert(next_);
      ++next_;
    }
  }
  Match find(std::size_t pos, std::size_t match_end) {
    insert_until(next_, pos);
    Match best;
    std::size_t cand = head_[hash(pos)];
    for (int d = 0; d < depth_ && cand != 0; ++d) {
      const std::size_t c = cand - 1;
      if (c >= pos || pos - c > max_distance) break;
      if (le32(data_ + c) == le32(data_ + pos)) {
        std::size_t len = 4;
        while (pos + len < match_end && data_[c + len] == data_[pos + len]) ++len;
        if (len > best.len) {
          best = {len, pos - c};
          if (len >= 256 + static_cast<std::size_t>(level_) * 32) break;
        }
      }
      if (!hc_) break;
      cand = chain_[c & 0xFFFFu];
    }
    if (next_ == pos) {
      insert(pos);
      ++next_;
    }
    return best;
  }
  static void put_length(Bytes& out, std::size_t v) {
    while (v >= 255) {
      out.push_back(std::byte{255});
      v -= 255;
    }
    out.push_back(static_cast<std::byte>(v));
  }
  void emit_literals(Bytes& out, std::size_t from, std::size_t to) {
    const std::size_t lit = to - from;
    out.push_back(static_cast<std::byte>(std::min<std::size_t>(lit, 15) << 4));
    if (lit >= 15) put_length(out, lit - 15);
    out.insert(out.end(), reinterpret_cast<const std::byte*>(data_ + from), reinterpret_cast<const std::byte*>(data_ + to));
  }
  void emit(Bytes& out, std::size_t anchor, std::size_t pos, const Match& m) {
    const std::size_t lit = pos - anchor;
    const std::size_t ml = m.len - min_match;
    out.push_back(static_cast<std::byte>((std::min<std::size_t>(lit, 15) << 4) | std::min<std::size_t>(ml, 15)));
    if (lit >= 15) put_length(out, lit - 15);
    out.insert(out.end(), reinterpret_cast<const std::byte*>(data_ + anchor), reinterpret_cast<const std::byte*>(data_ + pos));
    out.push_back(static_cast<std::byte>(m.offset & 0xFFu));
    out.push_back(static_cast<std::byte>(m.offset >> 8));
    if (ml >= 15) put_length(out, ml - 15);
  }

  const std::uint8_t* data_;
  std::size_t size_;
  int level_;
  bool hc_ = false;
  int depth_ = 1;
  std::vector<std::uint32_t> head_;
  std::vector<std::uint32_t> chain_;
  std::size_t next_ = 0;
};

// ------------------------------------------------------------------------------- block API

Result<Bytes> decode_lz4_block(ByteView data, const DecodeOptions& o) {
  Bytes out;
  std::uint64_t max = o.max_output;
  if (o.output_size) {
    if (*o.output_size > o.max_output) return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", o.max_output);
    max = *o.output_size;
    out.reserve(static_cast<std::size_t>(*o.output_size));
  }
  CVT_TRY(decode_block(data, out, 0, max, max));
  if (o.output_size && out.size() != *o.output_size) return fail(Status::Decode, "LZ4 block decodes to {} bytes, expected {}", out.size(), *o.output_size);
  if (o.stop_after != 0 && out.size() > o.stop_after) out.resize(static_cast<std::size_t>(o.stop_after));
  return out;
}

Result<Bytes> encode_lz4_block(ByteView data, const EncodeOptions& e) {
  Bytes out;
  BlockEncoder enc(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), e.level);
  enc.encode(0, data.size(), out);
  return out;
}

// ------------------------------------------------------------------------------- frames

Result<std::size_t> decode_frame(ByteView data, Bytes& out, const DecodeOptions& o) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  if (data.size() < 4) return fail(Status::Decode, "LZ4 frame is truncated");
  const std::uint32_t magic = le32(p);
  if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {
    if (data.size() < 8) return fail(Status::Decode, "LZ4 skippable frame is truncated");
    const std::uint64_t size = le32(p + 4);
    if (size > data.size() - 8) return fail(Status::Decode, "LZ4 skippable frame is truncated");
    return static_cast<std::size_t>(8 + size);
  }
  const std::uint64_t max = o.stop_after != 0 ? std::min<std::uint64_t>(o.max_output, o.stop_after + (8u << 20)) : o.max_output;
  if (magic == legacy_magic) {
    // Legacy frame: 8 MiB blocks with 4-byte sizes, until the data or the blocks end.
    std::size_t pos = 4;
    while (pos + 4 <= data.size()) {
      const std::uint32_t size = le32(p + pos);
      if (size == frame_magic || size == legacy_magic || (size & 0xFFFFFFF0u) == 0x184D2A50u) break;
      pos += 4;
      if (size > data.size() - pos) return fail(Status::Decode, "LZ4 legacy block is truncated");
      const std::size_t block_start = out.size();
      CVT_TRY(decode_block(data.subspan(pos, size), out, block_start, max, 8u << 20));
      pos += size;
      if (o.stop_after != 0 && out.size() >= o.stop_after) return data.size();
    }
    return pos;
  }
  if (magic != frame_magic) return fail(Status::Decode, "not an LZ4 frame");
  if (data.size() < 7) return fail(Status::Decode, "LZ4 frame header is truncated");
  const std::uint8_t flg = p[4];
  const std::uint8_t bd = p[5];
  if ((flg >> 6) != 1) return fail(Status::Decode, "LZ4 frame version is not supported");
  if ((flg & 0x02u) != 0 || (bd & 0x8Fu) != 0) return fail(Status::Decode, "LZ4 frame header uses reserved bits");
  const bool independent = (flg & 0x20u) != 0;
  const bool block_checksum = (flg & 0x10u) != 0;
  const bool has_size = (flg & 0x08u) != 0;
  const bool content_checksum = (flg & 0x04u) != 0;
  const bool has_dict = (flg & 0x01u) != 0;
  const int bsid = (bd >> 4) & 7;
  if (bsid < 4) return fail(Status::Decode, "LZ4 block size code is invalid");
  const std::uint64_t block_max = std::uint64_t{1} << (8 + 2 * bsid);
  std::size_t pos = 6;
  std::uint64_t content_size = 0;
  if (has_size) {
    if (pos + 8 > data.size()) return fail(Status::Decode, "LZ4 frame header is truncated");
    content_size = static_cast<std::uint64_t>(le32(p + pos)) | (static_cast<std::uint64_t>(le32(p + pos + 4)) << 32);
    pos += 8;
  }
  if (has_dict) return fail(Status::Decode, "this LZ4 frame needs a dictionary, which is not available");
  if (pos >= data.size()) return fail(Status::Decode, "LZ4 frame header is truncated");
  if (o.verify_checksums && ((archive::xxh32(data.subspan(4, pos - 4)) >> 8) & 0xFFu) != p[pos]) {
    return fail(Status::Decode, "LZ4 frame header checksum mismatch");
  }
  ++pos;
  if (has_size && o.stop_after == 0 && content_size > o.max_output) {
    return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", o.max_output);
  }
  const std::size_t frame_start = out.size();
  while (true) {
    if (pos + 4 > data.size()) return fail(Status::Decode, "LZ4 frame is truncated");
    const std::uint32_t word = le32(p + pos);
    pos += 4;
    if (word == 0) break;
    const bool raw = (word & 0x80000000u) != 0;
    const std::size_t size = word & 0x7FFFFFFFu;
    if (size > block_max) return fail(Status::Decode, "LZ4 block is larger than the frame allows");
    if (size > data.size() - pos) return fail(Status::Decode, "LZ4 block is truncated");
    const ByteView block = data.subspan(pos, size);
    pos += size;
    if (block_checksum) {
      if (pos + 4 > data.size()) return fail(Status::Decode, "LZ4 block checksum is truncated");
      if (o.verify_checksums && archive::xxh32(block) != le32(p + pos)) return fail(Status::Decode, "LZ4 block checksum mismatch");
      pos += 4;
    }
    if (raw) {
      CVT_TRY(check_output_room(out.size(), size, max));
      append(out, block);
    } else {
      CVT_TRY(decode_block(block, out, independent ? out.size() : frame_start, max, block_max));
    }
    if (o.stop_after != 0 && out.size() >= o.stop_after) return data.size();
    if (!o.progress.report(static_cast<double>(pos) / static_cast<double>(data.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  if (has_size && out.size() - frame_start != content_size) return fail(Status::Decode, "LZ4 frame size does not match its header");
  if (content_checksum) {
    if (pos + 4 > data.size()) return fail(Status::Decode, "LZ4 content checksum is truncated");
    if (o.verify_checksums && archive::xxh32(ByteView(out).subspan(frame_start)) != le32(p + pos)) {
      return fail(Status::Decode, "LZ4 content checksum mismatch");
    }
    pos += 4;
  }
  return pos;
}

Result<Bytes> decode_lz4_frame(ByteView data, const DecodeOptions& o) {
  Bytes out;
  std::size_t pos = 0;
  bool any = false;
  while (pos < data.size()) {
    CVT_TRY_ASSIGN(const std::size_t used, decode_frame(data.subspan(pos), out, o));
    pos += used;
    any = true;
    if (o.stop_after != 0 && out.size() >= o.stop_after) {
      out.resize(static_cast<std::size_t>(o.stop_after));
      return out;
    }
  }
  if (!any) return fail(Status::Decode, "not an LZ4 frame");
  if (o.output_size && out.size() != *o.output_size) return fail(Status::Decode, "LZ4 data decodes to {} bytes, expected {}", out.size(), *o.output_size);
  return out;
}

Result<Bytes> encode_lz4_frame(ByteView data, const EncodeOptions& e) {
  std::uint32_t block_size = e.block_size == 0 ? (4u << 20) : e.block_size;
  int bsid = 7;
  if (block_size <= (64u << 10)) {
    bsid = 4;
  } else if (block_size <= (256u << 10)) {
    bsid = 5;
  } else if (block_size <= (1u << 20)) {
    bsid = 6;
  }
  block_size = 1u << (8 + 2 * bsid);
  Bytes out;
  put32(out, frame_magic);
  const std::size_t descriptor = out.size();
  std::uint8_t flg = 0x40;  // version 01, linked blocks
  if (e.content_size) flg |= 0x08;
  if (e.content_checksum) flg |= 0x04;
  out.push_back(static_cast<std::byte>(flg));
  out.push_back(static_cast<std::byte>(bsid << 4));
  if (e.content_size) {
    put32(out, static_cast<std::uint32_t>(data.size() & 0xFFFFFFFFu));
    put32(out, static_cast<std::uint32_t>(static_cast<std::uint64_t>(data.size()) >> 32));
  }
  out.push_back(static_cast<std::byte>((archive::xxh32(ByteView(out).subspan(descriptor)) >> 8) & 0xFFu));
  BlockEncoder enc(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), e.level);
  Bytes block;
  for (std::size_t at = 0; at < data.size(); at += block_size) {
    if (!e.progress.report(static_cast<double>(at) / static_cast<double>(data.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    const std::size_t end = std::min(data.size(), at + block_size);
    block.clear();
    if (e.level > 0) enc.encode(at, end, block);
    if (e.level == 0 || block.size() >= end - at) {
      put32(out, static_cast<std::uint32_t>(end - at) | 0x80000000u);
      append(out, data.subspan(at, end - at));
    } else {
      put32(out, static_cast<std::uint32_t>(block.size()));
      append(out, block);
    }
  }
  put32(out, 0);
  if (e.content_checksum) put32(out, archive::xxh32(data));
  return out;
}

constexpr std::array<CodecImpl, 2> table = {{
    {Method::Lz4Frame, decode_lz4_frame, encode_lz4_frame, nullptr, nullptr, nullptr},
    {Method::Lz4Block, decode_lz4_block, encode_lz4_block, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> lz4_codecs() { return table; }

}  // namespace cvt::compression::detail
