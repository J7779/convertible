// Convertible core - archive family: Snappy raw format and framing format
// (Method::Snappy, Method::SnappyFramed).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstring>

#include "families/archive/checksums.hpp"
#include "families/archive/codec_impl.hpp"

namespace cvt::compression::detail {

namespace {

constexpr std::size_t chunk_max = 65536;

Result<void> decode_raw(ByteView data, Bytes& out, std::uint64_t max_output, std::uint64_t* expected = nullptr) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::size_t n = data.size();
  std::size_t i = 0;
  std::uint64_t length = 0;
  for (int shift = 0;; shift += 7) {
    if (i >= n || shift > 28) return fail(Status::Decode, "Snappy length is damaged");
    const std::uint8_t b = p[i++];
    length |= static_cast<std::uint64_t>(b & 0x7Fu) << shift;
    if ((b & 0x80u) == 0) break;
  }
  if (expected != nullptr) *expected = length;
  if (length > max_output - std::min<std::uint64_t>(max_output, out.size())) {
    return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", max_output);
  }
  const std::size_t start = out.size();
  const std::size_t end = start + static_cast<std::size_t>(length);
  out.reserve(end);
  while (i < n) {
    const std::uint8_t tag = p[i++];
    std::size_t len = 0;
    std::size_t offset = 0;
    switch (tag & 3u) {
      case 0: {
        len = (tag >> 2) + 1u;
        if (len > 60) {
          const std::size_t extra = len - 60;
          if (i + extra > n) return fail(Status::Decode, "Snappy literal is truncated");
          len = 0;
          for (std::size_t k = 0; k < extra; ++k) len |= static_cast<std::size_t>(p[i + k]) << (8 * k);
          len += 1;
          i += extra;
        }
        if (len > n - i || len > end - out.size()) return fail(Status::Decode, "Snappy literal is out of range");
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(i), data.begin() + static_cast<std::ptrdiff_t>(i + len));
        i += len;
        continue;
      }
      case 1:
        if (i >= n) return fail(Status::Decode, "Snappy copy is truncated");
        len = 4 + ((tag >> 2) & 7u);
        offset = (static_cast<std::size_t>(tag >> 5) << 8) | p[i++];
        break;
      case 2:
        if (i + 2 > n) return fail(Status::Decode, "Snappy copy is truncated");
        len = (tag >> 2) + 1u;
        offset = p[i] | (static_cast<std::size_t>(p[i + 1]) << 8);
        i += 2;
        break;
      default:
        if (i + 4 > n) return fail(Status::Decode, "Snappy copy is truncated");
        len = (tag >> 2) + 1u;
        offset = p[i] | (static_cast<std::size_t>(p[i + 1]) << 8) | (static_cast<std::size_t>(p[i + 2]) << 16) |
                 (static_cast<std::size_t>(p[i + 3]) << 24);
        i += 4;
        break;
    }
    if (offset == 0 || offset > out.size() - start || len > end - out.size()) {
      return fail(Status::Decode, "Snappy copy is out of range");
    }
    const std::size_t from = out.size() - offset;
    for (std::size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
  }
  if (out.size() != end) return fail(Status::Decode, "Snappy data is truncated");
  return {};
}

void encode_raw(ByteView data, Bytes& out) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::size_t n = data.size();
  std::uint64_t v = n;
  while (v >= 0x80) {
    out.push_back(static_cast<std::byte>((v & 0x7Fu) | 0x80u));
    v >>= 7;
  }
  out.push_back(static_cast<std::byte>(v));
  const auto literal = [&](std::size_t from, std::size_t to) {
    std::size_t len = to - from;
    if (len == 0) return;
    if (len <= 60) {
      out.push_back(static_cast<std::byte>((len - 1) << 2));
    } else {
      std::size_t m = len - 1;
      int bytes = 0;
      std::array<std::uint8_t, 4> b{};
      while (m > 0) {
        b[static_cast<std::size_t>(bytes++)] = static_cast<std::uint8_t>(m & 0xFFu);
        m >>= 8;
      }
      out.push_back(static_cast<std::byte>((59 + bytes) << 2));
      for (int k = 0; k < bytes; ++k) out.push_back(static_cast<std::byte>(b[static_cast<std::size_t>(k)]));
    }
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(from), data.begin() + static_cast<std::ptrdiff_t>(to));
  };
  const auto copy = [&](std::size_t offset, std::size_t len) {
    while (len > 0) {
      std::size_t take = std::min<std::size_t>(len, 64);
      if (len > 64 && len - 64 < 4) take = len - 4;  // leave a copyable tail
      if (take >= 4 && take <= 11 && offset < 2048) {
        out.push_back(static_cast<std::byte>(1u | ((take - 4) << 2) | ((offset >> 8) << 5)));
        out.push_back(static_cast<std::byte>(offset & 0xFFu));
      } else if (offset < 65536) {
        out.push_back(static_cast<std::byte>(2u | ((take - 1) << 2)));
        out.push_back(static_cast<std::byte>(offset & 0xFFu));
        out.push_back(static_cast<std::byte>(offset >> 8));
      } else {
        out.push_back(static_cast<std::byte>(3u | ((take - 1) << 2)));
        for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::byte>((offset >> (8 * k)) & 0xFFu));
      }
      len -= take;
    }
  };
  constexpr int hash_log = 14;
  std::vector<std::uint32_t> table(std::size_t{1} << hash_log, 0);
  const auto load = [&](std::size_t i) {
    return p[i] | (static_cast<std::uint32_t>(p[i + 1]) << 8) | (static_cast<std::uint32_t>(p[i + 2]) << 16) |
           (static_cast<std::uint32_t>(p[i + 3]) << 24);
  };
  std::size_t anchor = 0;
  std::size_t i = 0;
  while (n >= 15 && i + 15 <= n) {
    const std::uint32_t h = (load(i) * 0x1E35A7BDu) >> (32 - hash_log);
    const std::size_t cand = table[h];
    table[h] = static_cast<std::uint32_t>(i + 1);
    if (cand != 0 && i - (cand - 1) < (std::size_t{1} << 30) && load(cand - 1) == load(i)) {
      const std::size_t c = cand - 1;
      std::size_t len = 4;
      while (i + len < n && p[c + len] == p[i + len]) ++len;
      literal(anchor, i);
      copy(i - c, len);
      i += len;
      anchor = i;
      continue;
    }
    ++i;
  }
  literal(anchor, n);
}

Result<Bytes> decode_snappy(ByteView data, const DecodeOptions& o) {
  Bytes out;
  CVT_TRY(decode_raw(data, out, o.stop_after != 0 ? std::max(o.max_output, std::uint64_t{0}) : o.max_output));
  if (o.output_size && out.size() != *o.output_size) return fail(Status::Decode, "Snappy data decodes to {} bytes, expected {}", out.size(), *o.output_size);
  if (o.stop_after != 0 && out.size() > o.stop_after) out.resize(static_cast<std::size_t>(o.stop_after));
  return out;
}

Result<Bytes> encode_snappy(ByteView data, const EncodeOptions&) {
  Bytes out;
  encode_raw(data, out);
  return out;
}

Result<Bytes> decode_framed(ByteView data, const DecodeOptions& o) {
  Bytes out;
  std::size_t pos = 0;
  bool seen_header = false;
  while (pos < data.size()) {
    if (pos + 4 > data.size()) return fail(Status::Decode, "Snappy frame is truncated");
    const std::uint8_t type = std::to_integer<std::uint8_t>(data[pos]);
    const std::size_t len = std::to_integer<std::size_t>(data[pos + 1]) | (std::to_integer<std::size_t>(data[pos + 2]) << 8) |
                            (std::to_integer<std::size_t>(data[pos + 3]) << 16);
    pos += 4;
    if (len > data.size() - pos) return fail(Status::Decode, "Snappy frame chunk is truncated");
    const ByteView body = data.subspan(pos, len);
    pos += len;
    if (type == 0xFF) {
      if (len != 6 || as_chars(body) != "sNaPpY") return fail(Status::Decode, "Snappy stream identifier is damaged");
      seen_header = true;
      continue;
    }
    if (!seen_header) return fail(Status::Decode, "not a Snappy framed stream");
    if (type == 0x00 || type == 0x01) {
      if (len < 4) return fail(Status::Decode, "Snappy chunk is too short");
      const std::uint32_t stored = std::to_integer<std::uint32_t>(body[0]) | (std::to_integer<std::uint32_t>(body[1]) << 8) |
                                   (std::to_integer<std::uint32_t>(body[2]) << 16) | (std::to_integer<std::uint32_t>(body[3]) << 24);
      const std::size_t start = out.size();
      if (type == 0x00) {
        std::uint64_t declared = 0;
        const std::uint64_t room = std::min<std::uint64_t>(o.max_output, start + chunk_max);
        CVT_TRY(decode_raw(body.subspan(4), out, room, &declared));
      } else {
        if (len - 4 > chunk_max) return fail(Status::Decode, "Snappy chunk is too large");
        CVT_TRY(check_output_room(out.size(), len - 4, o.max_output));
        append(out, body.subspan(4));
      }
      if (o.verify_checksums && archive::snappy_mask_crc(archive::crc32c(ByteView(out).subspan(start))) != stored) {
        return fail(Status::Decode, "Snappy chunk checksum mismatch");
      }
      if (o.stop_after != 0 && out.size() >= o.stop_after) {
        out.resize(static_cast<std::size_t>(o.stop_after));
        return out;
      }
      continue;
    }
    if (type >= 0x02 && type <= 0x7F) return fail(Status::Decode, "Snappy stream has an unskippable reserved chunk (0x{:02x})", type);
    // 0x80..0xFE: skippable (0xFE = padding)
  }
  if (!seen_header) return fail(Status::Decode, "not a Snappy framed stream");
  if (o.output_size && out.size() != *o.output_size) return fail(Status::Decode, "Snappy data decodes to {} bytes, expected {}", out.size(), *o.output_size);
  return out;
}

Result<Bytes> encode_framed(ByteView data, const EncodeOptions& e) {
  Bytes out = to_bytes(std::string_view("\xFF\x06\x00\x00sNaPpY", 10));
  Bytes chunk;
  for (std::size_t at = 0; at < data.size(); at += chunk_max) {
    if (!e.progress.report(static_cast<double>(at) / static_cast<double>(data.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    const ByteView piece = data.subspan(at, std::min(chunk_max, data.size() - at));
    chunk.clear();
    encode_raw(piece, chunk);
    const std::uint32_t crc = archive::snappy_mask_crc(archive::crc32c(piece));
    const bool raw = e.level == 0 || chunk.size() >= piece.size();
    const ByteView body = raw ? piece : ByteView(chunk);
    const std::size_t len = body.size() + 4;
    out.push_back(std::byte{static_cast<std::uint8_t>(raw ? 0x01 : 0x00)});
    out.push_back(static_cast<std::byte>(len & 0xFFu));
    out.push_back(static_cast<std::byte>((len >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(len >> 16));
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::byte>((crc >> (8 * k)) & 0xFFu));
    append(out, body);
  }
  return out;
}

constexpr std::array<CodecImpl, 2> table = {{
    {Method::Snappy, decode_snappy, encode_snappy, nullptr, nullptr, nullptr},
    {Method::SnappyFramed, decode_framed, encode_framed, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> snappy_codecs() { return table; }

}  // namespace cvt::compression::detail
