// Convertible core - archive family: the RAR 5 (and RAR 7) LZ decoder.
//
// Compressed data is a series of byte-aligned blocks. Each block header
// holds flags (bits used in the last byte, size of the size field, "last
// block", "new tables"), a check byte and the block size. Tables are sent
// as 20 bit-length code lengths (4 bits each, 15 escapes a run of zeros)
// followed by the code lengths of the main (306), distance (64, or 80 for
// RAR 7's larger dictionaries), low-distance (16) and repeat-length (44)
// alphabets, coded with the bit-length code (16/17 repeat the previous
// length, 18/19 write zeros). Main symbols are literals (0-255), a filter
// (256), "repeat the last match" (257), one of four recent distances
// (258-261, length from the repeat-length code), or a match (262+: length
// slot, then a distance slot whose low 4 bits come from the low-distance
// code). Matches farther than 256, 8 KiB and 256 KiB get one extra byte of
// length each. Filters (DELTA, E8, E8E9, ARM) transform a block of the
// output once it has been decoded completely.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <limits>

#include "families/archive/formats/rar_internal.hpp"

namespace cvt::archive::rar {

// --------------------------------------------------------------------------- window

void LzWindow::allocate(std::uint64_t size) {
  std::uint64_t cap = 64 * 1024;
  while (cap < size) cap <<= 1;
  buf_.assign(static_cast<std::size_t>(cap), 0);
  mask_ = static_cast<std::size_t>(cap - 1);
  pos_ = 0;
  origin_ = 0;
}

bool LzWindow::copy(std::uint64_t distance, std::uint32_t length) noexcept {
  if (distance == 0 || distance > pos_ - origin_ || distance > buf_.size()) return false;
  const std::size_t size = buf_.size();
  const std::size_t dst = static_cast<std::size_t>(pos_) & mask_;
  const std::size_t src = static_cast<std::size_t>(pos_ - distance) & mask_;
  std::uint8_t* b = buf_.data();
  if (dst + length <= size && src + length <= size) {
    if (distance >= length) {
      std::memmove(b + dst, b + src, length);
    } else {
      for (std::uint32_t i = 0; i < length; ++i) b[dst + i] = b[src + i];
    }
  } else {
    for (std::uint32_t i = 0; i < length; ++i) b[(dst + i) & mask_] = b[(src + i) & mask_];
  }
  pos_ += length;
  return true;
}

void LzWindow::read(std::uint64_t from, std::size_t count, std::uint8_t* out) const noexcept {
  const std::size_t start = static_cast<std::size_t>(from) & mask_;
  const std::size_t first = std::min(count, buf_.size() - start);
  std::memcpy(out, buf_.data() + start, first);
  if (first < count) std::memcpy(out + first, buf_.data(), count - first);
}

// --------------------------------------------------------------------------- RAR 5

namespace {

constexpr std::size_t main_size = 306;
constexpr std::size_t dist_size = 64;
constexpr std::size_t dist_size_extended = 80;
constexpr std::size_t low_size = 16;
constexpr std::size_t rep_size = 44;
constexpr std::uint32_t max_filter_block = 0x400000;
constexpr std::size_t max_filters = 8192;
constexpr std::uint64_t match_margin = 0x1100;  // longer than any match

enum : std::uint8_t { filter_delta = 0, filter_e8 = 1, filter_e8e9 = 2, filter_arm = 3 };

Result<void> damaged() { return fail(Status::Decode, "RAR 5 compressed data is damaged"); }

std::uint32_t slot_length(BitInput& in, std::uint32_t slot) noexcept {
  if (slot < 8) return slot + 2;
  const int bits = static_cast<int>(slot / 4) - 1;
  return 2 + ((4u | (slot & 3u)) << bits) + in.get(bits);
}

std::uint32_t load32(const std::uint8_t* p) noexcept {
  return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}
void store32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

}  // namespace

void Unpack5::init(std::uint64_t window) {
  window_.allocate(std::max<std::uint64_t>(window, 1));
  tables_ = false;
  old_ = {};
  last_length_ = 0;
  filters_.clear();
  written_ = 0;
  file_start_ = 0;
}

Result<void> Unpack5::read_block_header(BitInput& in, Block& block) {
  in.align();
  const std::uint64_t start = in.position() / 8;
  if (start + 3 > in.size()) return fail(Status::Decode, "RAR 5 compressed data is truncated");
  const std::uint32_t flags = in.get(8);
  const std::uint32_t check = in.get(8);
  const int size_bytes = static_cast<int>((flags >> 3) & 3u) + 1;
  if (size_bytes == 4) return damaged();
  std::uint32_t size = 0;
  std::uint32_t sum = 0x5Au ^ flags;
  for (int i = 0; i < size_bytes; ++i) {
    const std::uint32_t b = in.get(8);
    size |= b << (8 * i);
    sum ^= b;
  }
  if ((sum & 0xFFu) != check) return fail(Status::Decode, "RAR 5 block header checksum mismatch (damaged data)");
  const std::uint64_t data = in.position() / 8;
  if (data + size > in.size()) return fail(Status::Decode, "RAR 5 compressed data is truncated");
  block.end_bit = size == 0 ? data * 8 : (data + size - 1) * 8 + (flags & 7u) + 1;
  block.last = (flags & 0x40u) != 0;
  if ((flags & 0x80u) != 0) {
    CVT_TRY(read_tables(in, extended_));
    if (in.position() > block.end_bit) return damaged();
  }
  return {};
}

Result<void> Unpack5::read_tables(BitInput& in, bool extended) {
  std::array<std::uint8_t, 20> bit_lengths{};
  for (std::size_t i = 0; i < bit_lengths.size();) {
    const auto length = static_cast<std::uint8_t>(in.get(4));
    if (length == 15) {
      std::uint32_t zeros = in.get(4);
      if (zeros != 0) {
        for (zeros += 2; zeros > 0 && i < bit_lengths.size(); --zeros) bit_lengths[i++] = 0;
        continue;
      }
    }
    bit_lengths[i++] = length;
  }
  compression::detail::HuffmanTable bits;
  if (!bits.build(bit_lengths, false)) return damaged();
  const std::size_t dsize = extended ? dist_size_extended : dist_size;
  const std::size_t total = main_size + dsize + low_size + rep_size;
  std::array<std::uint8_t, main_size + dist_size_extended + low_size + rep_size> lengths{};
  for (std::size_t i = 0; i < total;) {
    const int sym = bits.decode(in);
    if (sym < 0) return damaged();
    if (sym < 16) {
      lengths[i++] = static_cast<std::uint8_t>(sym);
    } else if (sym < 18) {
      if (i == 0) return damaged();
      std::uint32_t n = sym == 16 ? in.get(3) + 3 : in.get(7) + 11;
      for (; n > 0 && i < total; --n, ++i) lengths[i] = lengths[i - 1];
    } else {
      std::uint32_t n = sym == 18 ? in.get(3) + 3 : in.get(7) + 11;
      for (; n > 0 && i < total; --n) lengths[i++] = 0;
    }
    if (in.overrun()) return fail(Status::Decode, "RAR 5 compressed data is truncated");
  }
  const std::span<const std::uint8_t> all(lengths.data(), total);
  if (!main_.build(all.subspan(0, main_size), false) || !dist_.build(all.subspan(main_size, dsize), false) ||
      !low_.build(all.subspan(main_size + dsize, low_size), false) ||
      !rep_.build(all.subspan(main_size + dsize + low_size, rep_size), false)) {
    return damaged();
  }
  tables_ = true;
  return {};
}

Result<void> Unpack5::read_filter(BitInput& in) {
  const auto number = [&in]() {
    const int count = static_cast<int>(in.get(2)) + 1;
    std::uint32_t v = 0;
    for (int i = 0; i < count; ++i) v |= in.get(8) << (8 * i);
    return v;
  };
  Filter f;
  f.start = window_.pos() + number();
  const std::uint32_t length = number();
  f.type = static_cast<std::uint8_t>(in.get(3));
  if (f.type == filter_delta) f.channels = static_cast<std::uint8_t>(in.get(5) + 1);
  if (f.type > filter_arm) return fail(Status::Decode, "RAR 5 data uses unknown filter type {}", f.type);
  if (length > max_filter_block) return damaged();
  if (length == 0) return {};
  f.length = length;
  if (!filters_.empty() && f.start < filters_.back().start + filters_.back().length) return damaged();
  filters_.push_back(f);
  return {};
}

void Unpack5::apply_filter(const Filter& f, std::vector<std::uint8_t>& block, std::uint64_t file_offset) const {
  std::uint8_t* d = block.data();
  const std::uint32_t size = f.length;
  switch (f.type) {
    case filter_e8:
    case filter_e8e9: {
      constexpr std::uint32_t file_size = 0x1000000;
      const auto base = static_cast<std::uint32_t>(file_offset);
      for (std::uint32_t pos = 0; pos + 4 < size;) {
        const std::uint8_t op = d[pos++];
        if (op == 0xE8 || (f.type == filter_e8e9 && op == 0xE9)) {
          const std::uint32_t offset = (pos + base) % file_size;
          const std::uint32_t addr = load32(d + pos);
          if ((addr & 0x80000000u) != 0) {
            if (((addr + offset) & 0x80000000u) == 0) store32(d + pos, addr + file_size);
          } else if (((addr - file_size) & 0x80000000u) != 0) {
            store32(d + pos, addr - offset);
          }
          pos += 4;
        }
      }
      break;
    }
    case filter_arm: {
      const auto base = static_cast<std::uint32_t>(file_offset);
      for (std::uint32_t pos = 0; pos + 3 < size; pos += 4) {
        std::uint8_t* p = d + pos;
        if (p[3] == 0xEB) {
          std::uint32_t offset = std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16);
          offset -= (base + pos) / 4;
          p[0] = static_cast<std::uint8_t>(offset);
          p[1] = static_cast<std::uint8_t>(offset >> 8);
          p[2] = static_cast<std::uint8_t>(offset >> 16);
        }
      }
      break;
    }
    default: {  // delta: the channels are stored one after another
      std::vector<std::uint8_t> out(size);
      std::size_t src = 0;
      for (std::uint32_t ch = 0; ch < f.channels; ++ch) {
        std::uint8_t prev = 0;
        for (std::uint32_t i = ch; i < size; i += f.channels) {
          prev = static_cast<std::uint8_t>(prev - d[src++]);
          out[i] = prev;
        }
      }
      block.swap(out);
      break;
    }
  }
}

Result<void> Unpack5::emit(FileOutput& out, const std::uint8_t* data, std::size_t count) {
  Bytes& bytes = *out.bytes;
  if (!out.size_known && count > out.budget - std::min<std::uint64_t>(out.budget, bytes.size())) {
    return fail(Status::Limit, "RAR data decompresses to more than the remaining limit");
  }
  const auto* p = reinterpret_cast<const std::byte*>(data);
  bytes.insert(bytes.end(), p, p + count);
  return {};
}

Result<void> Unpack5::flush(FileOutput& out, bool final) {
  const std::uint64_t file_end =
      out.size_known ? file_start_ + out.size : std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t pos = window_.pos();
  while (true) {
    const std::uint64_t limit = std::min(pos, file_end);
    if (filters_.empty()) {
      if (limit > written_) {
        const auto n = static_cast<std::size_t>(limit - written_);
        scratch_.resize(n);
        window_.read(written_, n, scratch_.data());
        CVT_TRY(emit(out, scratch_.data(), n));
        written_ = limit;
      }
      return {};
    }
    const Filter f = filters_.front();
    if (f.start < written_) return damaged();
    if (written_ < f.start) {
      const std::uint64_t to = std::min(f.start, limit);
      if (to <= written_) return {};
      const auto n = static_cast<std::size_t>(to - written_);
      scratch_.resize(n);
      window_.read(written_, n, scratch_.data());
      CVT_TRY(emit(out, scratch_.data(), n));
      written_ = to;
      continue;
    }
    if (pos < f.start + f.length) {
      if (final) return damaged();
      return {};
    }
    scratch_.resize(f.length);
    window_.read(f.start, f.length, scratch_.data());
    apply_filter(f, scratch_, f.start - file_start_);
    const std::uint64_t keep = file_end > written_ ? std::min<std::uint64_t>(f.length, file_end - written_) : 0;
    CVT_TRY(emit(out, scratch_.data(), static_cast<std::size_t>(keep)));
    written_ += f.length;
    filters_.erase(filters_.begin());
  }
}

Result<void> Unpack5::decode(ByteView packed, bool solid, bool extended, FileOutput& out) {
  if (!solid) {
    tables_ = false;
    old_ = {};
    last_length_ = 0;
    window_.restart();
  }
  extended_ = extended;
  filters_.clear();
  written_ = window_.pos();
  file_start_ = written_;
  if (out.size_known) out.bytes->reserve(static_cast<std::size_t>(out.size));

  BitInput in(packed);
  Block block;
  CVT_TRY(read_block_header(in, block));
  const std::uint64_t capacity = window_.capacity();
  std::uint32_t tick = 0;
  while (true) {
    if (in.position() >= block.end_bit) {
      if (block.last) break;
      in.seek(block.end_bit);
      CVT_TRY(read_block_header(in, block));
      continue;
    }
    if (!tables_) return fail(Status::Decode, "RAR 5 data starts without Huffman tables (damaged data or a missing solid predecessor)");
    if (window_.pos() - written_ >= capacity - match_margin) {
      CVT_TRY(flush(out, false));
      if (window_.pos() - written_ >= capacity - match_margin) {
        return fail(Status::Decode, "RAR 5 data is longer than its declared size or uses an oversized filter");
      }
    }
    if ((++tick & 0xFFFFu) == 0) {
      const double done = out.size_known && out.size != 0
                              ? static_cast<double>(window_.pos() - file_start_) / static_cast<double>(out.size)
                              : 0.0;
      if (!out.progress.report(std::min(done, 1.0))) return fail(Status::Cancelled, "the conversion was cancelled");
    }

    const int sym = main_.decode(in);
    if (sym < 0) return damaged();
    if (sym < 256) {
      window_.put(static_cast<std::uint8_t>(sym));
      continue;
    }
    if (sym >= 262) {
      std::uint32_t length = slot_length(in, static_cast<std::uint32_t>(sym - 262));
      const int slot = dist_.decode(in);
      if (slot < 0) return damaged();
      std::uint64_t distance = 1;
      if (slot < 4) {
        distance += static_cast<std::uint64_t>(slot);
      } else {
        const int bits = slot / 2 - 1;
        distance += std::uint64_t{2u | (static_cast<std::uint32_t>(slot) & 1u)} << bits;
        if (bits >= 4) {
          const int high = bits - 4;
          if (high > 32) {
            distance += std::uint64_t{in.get_wide(high - 32)} << 36;
            distance += std::uint64_t{in.get_wide(32)} << 4;
          } else if (high > 0) {
            distance += std::uint64_t{in.get_wide(high)} << 4;
          }
          const int low = low_.decode(in);
          if (low < 0) return damaged();
          distance += static_cast<std::uint64_t>(low);
        } else {
          distance += in.get(bits);
        }
      }
      if (distance > 0x100) {
        ++length;
        if (distance > 0x2000) {
          ++length;
          if (distance > 0x40000) ++length;
        }
      }
      old_[3] = old_[2];
      old_[2] = old_[1];
      old_[1] = old_[0];
      old_[0] = distance;
      last_length_ = length;
      if (!window_.copy(distance, length)) return damaged();
      continue;
    }
    if (sym == 256) {
      if (filters_.size() >= max_filters) CVT_TRY(flush(out, false));
      if (filters_.size() >= max_filters) return damaged();
      CVT_TRY(read_filter(in));
      continue;
    }
    if (sym == 257) {
      if (last_length_ != 0 && !window_.copy(old_[0], last_length_)) return damaged();
      continue;
    }
    const auto index = static_cast<std::size_t>(sym - 258);
    const std::uint64_t distance = old_[index];
    for (std::size_t i = index; i > 0; --i) old_[i] = old_[i - 1];
    old_[0] = distance;
    const int slot = rep_.decode(in);
    if (slot < 0) return damaged();
    const std::uint32_t length = slot_length(in, static_cast<std::uint32_t>(slot));
    last_length_ = length;
    if (!window_.copy(distance, length)) return damaged();
  }
  if (in.overrun()) return fail(Status::Decode, "RAR 5 compressed data is truncated");
  CVT_TRY(flush(out, true));
  if (out.size_known && out.bytes->size() != out.size) {
    return fail(Status::Decode, "RAR 5 data decompressed to {} bytes instead of {}", out.bytes->size(), out.size);
  }
  return {};
}

}  // namespace cvt::archive::rar
