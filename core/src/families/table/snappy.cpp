// Convertible core - table family: Snappy block compression.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/table/snappy.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace cvt::table {

Result<Bytes> snappy_decompress(ByteView data, std::uint64_t max_output) {
  const auto in = as_u8(data);
  std::size_t pos = 0;
  std::uint64_t length = 0;
  for (int shift = 0;; shift += 7) {
    if (pos >= in.size() || shift > 63) return fail(Status::Decode, "damaged Snappy length");
    const std::uint8_t b = in[pos++];
    length |= static_cast<std::uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) break;
  }
  if (length > max_output) return fail(Status::Limit, "Snappy block of {} bytes exceeds the limit", length);
  if (length > 0xFFFFFFFFull) return fail(Status::Decode, "Snappy block is too large");
  Bytes out;
  out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(length, in.size() * 64ull + 64)));
  while (pos < in.size()) {
    const std::uint8_t tag = in[pos++];
    const std::uint8_t kind = tag & 3;
    if (kind == 0) {
      std::uint64_t literal = (tag >> 2) + 1u;
      if (literal > 60) {
        const std::size_t extra = literal - 60;
        if (pos + extra > in.size()) return fail(Status::Decode, "truncated Snappy literal");
        literal = 0;
        for (std::size_t i = 0; i < extra; ++i) literal |= static_cast<std::uint64_t>(in[pos + i]) << (8 * i);
        literal += 1;
        pos += extra;
      }
      if (literal > in.size() - pos || out.size() + literal > length) return fail(Status::Decode, "damaged Snappy literal");
      const ByteView chunk = data.subspan(pos, static_cast<std::size_t>(literal));
      out.insert(out.end(), chunk.begin(), chunk.end());
      pos += static_cast<std::size_t>(literal);
      continue;
    }
    std::uint64_t copy = 0;
    std::uint64_t offset = 0;
    if (kind == 1) {
      if (pos >= in.size()) return fail(Status::Decode, "truncated Snappy copy");
      copy = ((tag >> 2) & 7u) + 4u;
      offset = (static_cast<std::uint64_t>(tag >> 5) << 8) | in[pos++];
    } else if (kind == 2) {
      if (pos + 2 > in.size()) return fail(Status::Decode, "truncated Snappy copy");
      copy = (tag >> 2) + 1u;
      offset = in[pos] | (static_cast<std::uint64_t>(in[pos + 1]) << 8);
      pos += 2;
    } else {
      if (pos + 4 > in.size()) return fail(Status::Decode, "truncated Snappy copy");
      copy = (tag >> 2) + 1u;
      offset = in[pos] | (static_cast<std::uint64_t>(in[pos + 1]) << 8) | (static_cast<std::uint64_t>(in[pos + 2]) << 16) |
               (static_cast<std::uint64_t>(in[pos + 3]) << 24);
      pos += 4;
    }
    if (offset == 0 || offset > out.size() || out.size() + copy > length) return fail(Status::Decode, "damaged Snappy copy");
    const std::size_t from = out.size() - static_cast<std::size_t>(offset);
    for (std::uint64_t i = 0; i < copy; ++i) out.push_back(out[from + static_cast<std::size_t>(i)]);  // may overlap
  }
  if (out.size() != length) return fail(Status::Decode, "Snappy block is shorter than declared");
  return out;
}

namespace {

void emit_literal(Bytes& out, ByteView data, std::size_t begin, std::size_t end) {
  while (begin < end) {
    const std::size_t n = std::min<std::size_t>(end - begin, 65536);
    const std::size_t value = n - 1;
    if (value < 60) {
      out.push_back(static_cast<std::byte>(value << 2));
    } else if (value < 256) {
      out.push_back(static_cast<std::byte>(60 << 2));
      out.push_back(static_cast<std::byte>(value));
    } else {
      out.push_back(static_cast<std::byte>(61 << 2));
      out.push_back(static_cast<std::byte>(value));
      out.push_back(static_cast<std::byte>(value >> 8));
    }
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(begin), data.begin() + static_cast<std::ptrdiff_t>(begin + n));
    begin += n;
  }
}

void emit_copy(Bytes& out, std::size_t offset, std::size_t length) {
  while (length > 0) {
    const std::size_t n = std::min<std::size_t>(length, 64);
    if (n >= 4 && n <= 11 && offset < 2048) {
      out.push_back(static_cast<std::byte>(1 | ((n - 4) << 2) | ((offset >> 8) << 5)));
      out.push_back(static_cast<std::byte>(offset));
    } else {
      out.push_back(static_cast<std::byte>(2 | ((n - 1) << 2)));
      out.push_back(static_cast<std::byte>(offset));
      out.push_back(static_cast<std::byte>(offset >> 8));
    }
    length -= n;
  }
}

}  // namespace

Bytes snappy_compress(ByteView data) {
  Bytes out;
  std::uint64_t length = data.size();
  do {
    out.push_back(static_cast<std::byte>((length & 0x7F) | (length >= 0x80 ? 0x80 : 0)));
    length >>= 7;
  } while (length != 0);
  const auto in = as_u8(data);
  constexpr int bits = 14;
  std::vector<std::uint32_t> table(std::size_t{1} << bits, 0xFFFFFFFFu);
  const auto load = [&](std::size_t i) {
    std::uint32_t v = 0;
    std::memcpy(&v, in.data() + i, 4);
    return v;
  };
  std::size_t literal_start = 0;
  std::size_t i = 0;
  while (i + 4 <= in.size()) {
    const std::uint32_t v = load(i);
    const std::uint32_t h = (v * 0x1E35A7BDu) >> (32 - bits);
    const std::uint32_t candidate = table[h];
    table[h] = static_cast<std::uint32_t>(i);
    // Offsets are limited to 16 bits by the 2-byte copy element used here.
    if (candidate != 0xFFFFFFFFu && i - candidate <= 65535 && load(candidate) == v) {
      std::size_t match = 4;
      while (i + match < in.size() && in[candidate + match] == in[i + match]) ++match;
      emit_literal(out, data, literal_start, i);
      std::size_t remaining = match;
      // A copy element carries at most 64 bytes; split long matches.
      while (remaining > 0) {
        std::size_t n = std::min<std::size_t>(remaining, 64);
        if (remaining - n > 0 && remaining - n < 4) n = remaining - 4;  // keep every piece >= 4 bytes
        emit_copy(out, i - candidate, n);
        remaining -= n;
      }
      i += match;
      literal_start = i;
      continue;
    }
    ++i;
  }
  emit_literal(out, data, literal_start, in.size());
  return out;
}

}  // namespace cvt::table
