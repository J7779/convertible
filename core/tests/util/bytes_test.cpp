// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <limits>

#include "support/test_support.hpp"
#include "util/bytes.hpp"

using namespace cvt;
using util::ByteReader;
using util::ByteWriter;

TEST_CASE("bytes: views and copies") {
  const std::string_view text = "abc";
  CHECK(as_bytes(text).size() == 3);
  CHECK(as_chars(as_bytes(text)) == "abc");
  CHECK(to_string(to_bytes(text)) == "abc");
  const std::array<std::uint8_t, 2> raw = {0x01, 0xFF};
  CHECK(as_u8(as_bytes(std::span<const std::uint8_t>(raw)))[1] == 0xFF);
  Bytes out;
  append(out, "ab");
  append(out, ByteView(to_bytes("c")));
  CHECK(to_string(out) == "abc");
}

TEST_CASE("ByteReader: integers in both byte orders") {
  const std::array<std::uint8_t, 16> raw = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                            0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8};
  const ByteView data = as_bytes(std::span<const std::uint8_t>(raw));
  {
    ByteReader r(data);
    CHECK(r.u8() == 0x01);
    CHECK(r.u16le() == 0x0302);
    CHECK(r.u16be() == 0x0405);
    CHECK(r.u24be() == 0x060708);
    CHECK(r.i8() == -1);
    CHECK(r.i16be() == static_cast<std::int16_t>(0xFEFD));
    CHECK(r.position() == 11);
    CHECK(r.remaining() == 5);
    CHECK(r.ok());
  }
  {
    ByteReader r(data);
    CHECK(r.u32le() == 0x04030201u);
    CHECK(r.u32be() == 0x05060708u);
    CHECK(r.i32le() == static_cast<std::int32_t>(0xFCFDFEFFu));
    CHECK(r.u24le() == 0xF9FAFBu);
    CHECK(r.ok());
    CHECK(r.remaining() == 1);
  }
  {
    ByteReader r(data);
    CHECK(r.u64le() == 0x0807060504030201ull);
    CHECK(r.u64be() == 0xFFFEFDFCFBFAF9F8ull);
    CHECK(r.at_end());
    ByteReader again(data);
    CHECK(again.i64be() == 0x0102030405060708ll);
    CHECK(again.i64le() == static_cast<std::int64_t>(0xF8F9FAFBFCFDFEFFull));
  }
}

TEST_CASE("ByteReader: is usable in constant expressions") {
  static constexpr std::array<std::byte, 4> raw = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
  static constexpr std::uint32_t value = [] {
    ByteReader r{ByteView(raw)};
    return r.u32be();
  }();
  static_assert(value == 0x12345678u);
  CHECK(value == 0x12345678u);
}

TEST_CASE("ByteReader: out-of-bounds reads latch the failure flag") {
  const Bytes data = to_bytes("abc");
  ByteReader r(data);
  CHECK(r.u16le() == 0x6261);
  CHECK(r.u32le() == 0);  // only one byte left
  CHECK_FALSE(r.ok());
  CHECK(r.position() == 2);  // nothing was consumed by the failed read
  CHECK(r.u8() == 'c');      // reads that fit still work...
  CHECK_FALSE(r.ok());       // ...but the flag stays set
  r.clear_error();
  CHECK(r.ok());
  CHECK(r.bytes(1).empty());
  CHECK_FALSE(r.ok());

  ByteReader s(data);
  CHECK_FALSE(s.skip(4));
  CHECK_FALSE(s.ok());
  ByteReader t(data);
  CHECK(t.seek(3));
  CHECK(t.at_end());
  CHECK_FALSE(t.seek(4));
  CHECK(t.position() == 3);

  ByteReader empty;
  CHECK(empty.at_end());
  CHECK(empty.u8() == 0);
  CHECK_FALSE(empty.ok());
  CHECK(empty.peek_u8(7) == 7);
}

TEST_CASE("ByteReader: slices, strings and floats") {
  ByteWriter w;
  w.text("MAGIC").u8(0).text("name").u8(0).f32le(1.5f).f64be(-2.25).f32be(0.5f).f64le(1e300).text("tail");
  const Bytes data = w.take();
  ByteReader r(data);
  CHECK(r.starts_with("MAGIC"));
  CHECK_FALSE(r.starts_with("MAGIX"));
  CHECK_FALSE(r.consume("NOPE"));
  CHECK(r.consume("MAGIC"));
  CHECK(r.u8() == 0);
  CHECK(r.cstring() == "name");
  CHECK(r.f32le() == 1.5f);
  CHECK(r.f64be() == -2.25);
  CHECK(r.f32be() == 0.5f);
  CHECK(r.f64le() == 1e300);
  CHECK(as_chars(r.peek_rest()) == "tail");
  ByteReader sub = r.sub(2);
  CHECK(sub.chars(2) == "ta");
  CHECK(sub.at_end());
  CHECK(as_chars(r.rest()) == "il");
  CHECK(r.at_end());
  CHECK(r.cstring().empty());  // no terminator left
  CHECK_FALSE(r.ok());
}

TEST_CASE("ByteWriter: integers, patching and alignment") {
  ByteWriter w(16);
  w.u8(0xAB).u16le(0x1234).u16be(0x1234).u32le(0xDEADBEEF).u32be(0xDEADBEEF).u24le(0x010203).u24be(0x010203);
  w.i8(-2).i16le(-2).i32be(-2).u64le(0x0102030405060708ull).i64be(-1);
  const Bytes expected = {std::byte{0xAB}, std::byte{0x34}, std::byte{0x12}, std::byte{0x12}, std::byte{0x34},
                          std::byte{0xEF}, std::byte{0xBE}, std::byte{0xAD}, std::byte{0xDE}, std::byte{0xDE},
                          std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}, std::byte{0x03}, std::byte{0x02},
                          std::byte{0x01}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0xFE},
                          std::byte{0xFE}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                          std::byte{0xFE}, std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
                          std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01}, std::byte{0xFF},
                          std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                          std::byte{0xFF}, std::byte{0xFF}};
  CHECK(w.data() == expected);

  ByteWriter p;
  p.u32le(0).text("abc").align(4).zeros(2);
  CHECK(p.size() == 10);
  CHECK(p.patch_u32le(0, 0x11223344));
  CHECK(p.patch_u32be(4, 0xAABBCCDD));
  CHECK_FALSE(p.patch_u32le(8, 1));  // would run past the end
  CHECK_FALSE(p.patch<std::uint8_t>(10, 1, std::endian::little));
  ByteReader r(p.view());
  CHECK(r.u32le() == 0x11223344u);
  CHECK(r.u32be() == 0xAABBCCDDu);
  const Bytes taken = p.take();
  CHECK(taken.size() == 10);
  CHECK(p.size() == 0);
}

TEST_CASE("ByteReader/ByteWriter: round trip of extreme values") {
  ByteWriter w;
  w.i16be(std::numeric_limits<std::int16_t>::min()).i32le(std::numeric_limits<std::int32_t>::min());
  w.i64le(std::numeric_limits<std::int64_t>::min()).u64be(std::numeric_limits<std::uint64_t>::max());
  ByteReader r(w.view());
  CHECK(r.i16be() == std::numeric_limits<std::int16_t>::min());
  CHECK(r.i32le() == std::numeric_limits<std::int32_t>::min());
  CHECK(r.i64le() == std::numeric_limits<std::int64_t>::min());
  CHECK(r.u64be() == std::numeric_limits<std::uint64_t>::max());
  CHECK(r.ok());
  CHECK(r.at_end());
}
