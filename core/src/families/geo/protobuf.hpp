// Convertible core - geo family: minimal protocol-buffers wire format reader
// and writer (OSM PBF, Geobuf). Every length is checked against the buffer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "util/bytes.hpp"

namespace cvt::geo::pbf {

/// Sequential reader over one message. A failed read latches `ok = false`.
struct Reader {
  const std::uint8_t* p = nullptr;
  const std::uint8_t* end = nullptr;
  bool ok = true;

  explicit Reader(ByteView data) : p(as_u8(data).data()), end(as_u8(data).data() + data.size()) {}

  [[nodiscard]] bool at_end() const { return p >= end || !ok; }
  std::uint64_t varint() {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (p >= end) {
        ok = false;
        return 0;
      }
      const std::uint8_t b = *p++;
      v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
    }
    ok = false;
    return 0;
  }
  std::int64_t svarint() {
    const std::uint64_t v = varint();
    return static_cast<std::int64_t>((v >> 1) ^ (~(v & 1) + 1));
  }
  std::uint64_t fixed64() {
    if (end - p < 8) {
      ok = false;
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    p += 8;
    return v;
  }
  double f64() { return std::bit_cast<double>(fixed64()); }
  ByteView bytes() {
    const std::uint64_t n = varint();
    if (!ok || n > static_cast<std::uint64_t>(end - p)) {
      ok = false;
      return {};
    }
    const ByteView out(reinterpret_cast<const std::byte*>(p), static_cast<std::size_t>(n));
    p += n;
    return out;
  }
  /// Reads the next key; false at the end of the message or on an error.
  bool next(std::uint32_t& field, std::uint32_t& wire) {
    if (at_end()) return false;
    const std::uint64_t key = varint();
    if (!ok) return false;
    field = static_cast<std::uint32_t>(key >> 3);
    wire = static_cast<std::uint32_t>(key & 7);
    return true;
  }
  void skip(std::uint32_t wire) {
    switch (wire) {
      case 0: (void)varint(); break;
      case 1:
        if (end - p < 8) ok = false;
        else p += 8;
        break;
      case 2: (void)bytes(); break;
      case 5:
        if (end - p < 4) ok = false;
        else p += 4;
        break;
      default: ok = false;
    }
  }
};

/// Calls `each(reader)` for every element of a packed (or single unpacked) repeated varint field.
template <class F>
bool packed(Reader& msg, std::uint32_t wire, F&& each) {
  if (wire == 2) {
    Reader inner(msg.bytes());
    while (!inner.at_end()) each(inner);
    return msg.ok && inner.ok;
  }
  if (wire == 0) {
    each(msg);
    return msg.ok;
  }
  msg.ok = false;
  return false;
}

/// Appends protobuf fields to a buffer.
struct Writer {
  util::ByteWriter out;

  void varint(std::uint64_t v) {
    while (v >= 0x80) {
      out.u8(static_cast<std::uint8_t>(v | 0x80));
      v >>= 7;
    }
    out.u8(static_cast<std::uint8_t>(v));
  }
  static std::uint64_t zigzag(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
  }
  void key(std::uint32_t field, std::uint32_t wire) { varint((std::uint64_t{field} << 3) | wire); }
  void uint_field(std::uint32_t field, std::uint64_t v) {
    key(field, 0);
    varint(v);
  }
  void sint_field(std::uint32_t field, std::int64_t v) {
    key(field, 0);
    varint(zigzag(v));
  }
  void double_field(std::uint32_t field, double v) {
    key(field, 1);
    out.f64le(v);
  }
  void bytes_field(std::uint32_t field, ByteView data) {
    key(field, 2);
    varint(data.size());
    out.bytes(data);
  }
  void string_field(std::uint32_t field, std::string_view text) { bytes_field(field, as_bytes(text)); }
  [[nodiscard]] Bytes take() { return out.take(); }
};

}  // namespace cvt::geo::pbf
