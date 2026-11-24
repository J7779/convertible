// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// LZFSE / LZVN (Method::Lzfse): streams written by Apple's compression_tool.
#include <string>

#include "families/archive/codec_test_util.hpp"

using namespace cvt;
namespace cz = cvt::compression;
namespace tc = cvt::test::codec;

namespace {

Bytes fixture(std::string_view name) { return test::fixture(std::string("archive/apple_codecs/") + std::string(name)); }

Bytes decode(std::string_view name, cz::DecodeOptions d = {}) {
  INFO("fixture ", name);
  auto out = cz::decompress(cz::Method::Lzfse, fixture(name), d);
  REQUIRE_OK(out);
  return std::move(*out);
}

void put32(Bytes& b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

std::uint64_t le(ByteView b, std::size_t at, int bytes) {
  std::uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | std::to_integer<std::uint64_t>(b[at + static_cast<std::size_t>(i)]);
  return v;
}

/// Rewrite the first "bvx2" block of `stream` as the equivalent "bvx1" block
/// (unpacked header with 16-bit frequency tables); the payload is unchanged.
Bytes v2_to_v1(ByteView stream) {
  REQUIRE(stream.size() > 32);
  REQUIRE_EQ(le(stream, 0, 4), 0x32787662u);
  const std::uint64_t f0 = le(stream, 8, 8);
  const std::uint64_t f1 = le(stream, 16, 8);
  const std::uint64_t f2 = le(stream, 24, 8);
  const std::size_t header = static_cast<std::size_t>(f2 & 0xFFFFFFFFu);
  // Unpack the frequency tables (same prefix code as the decoder; written independently here).
  std::vector<std::uint16_t> freq;
  std::uint32_t accum = 0;
  int bits = 0;
  std::size_t at = 32;
  while (freq.size() < 360) {
    while (at < header && bits <= 24) {
      accum |= std::to_integer<std::uint32_t>(stream[at++]) << bits;
      bits += 8;
    }
    int n = 0;
    std::uint32_t v = 0;
    if ((accum & 1u) == 0) {
      n = 2;
      v = (accum >> 1) & 1u;
    } else if ((accum & 3u) == 1) {
      n = 3;
      v = 2 + ((accum >> 2) & 1u);
    } else if ((accum & 7u) == 3) {
      n = 5;
      v = 4 + ((accum >> 3) & 3u);
    } else if ((accum & 15u) == 7) {
      n = 8;
      v = 8 + ((accum >> 4) & 15u);
    } else {
      n = 14;
      v = 24 + ((accum >> 4) & 0x3FFu);
    }
    freq.push_back(static_cast<std::uint16_t>(v));
    accum >>= n;
    bits -= n;
  }
  const std::uint32_t lit_payload = static_cast<std::uint32_t>((f0 >> 20) & 0xFFFFFu);
  const std::uint32_t lmd_payload = static_cast<std::uint32_t>((f1 >> 40) & 0xFFFFFu);
  Bytes v1;
  put32(v1, 0x31787662u);
  put32(v1, static_cast<std::uint32_t>(le(stream, 4, 4)));
  put32(v1, lit_payload + lmd_payload);
  put32(v1, static_cast<std::uint32_t>(f0 & 0xFFFFFu));
  put32(v1, static_cast<std::uint32_t>((f0 >> 40) & 0xFFFFFu));
  put32(v1, lit_payload);
  put32(v1, lmd_payload);
  put32(v1, static_cast<std::uint32_t>(static_cast<std::int32_t>((f0 >> 60) & 7u) - 7));
  for (int i = 0; i < 4; ++i) {
    const auto s = static_cast<std::uint16_t>((f1 >> (10 * i)) & 0x3FFu);
    v1.push_back(static_cast<std::byte>(s & 0xFFu));
    v1.push_back(static_cast<std::byte>(s >> 8));
  }
  put32(v1, static_cast<std::uint32_t>(static_cast<std::int32_t>((f1 >> 60) & 7u) - 7));
  for (const int shift : {32, 42, 52}) {
    const auto s = static_cast<std::uint16_t>((f2 >> shift) & 0x3FFu);
    v1.push_back(static_cast<std::byte>(s & 0xFFu));
    v1.push_back(static_cast<std::byte>(s >> 8));
  }
  for (const std::uint16_t f : freq) {
    v1.push_back(static_cast<std::byte>(f & 0xFFu));
    v1.push_back(static_cast<std::byte>(f >> 8));
  }
  while (v1.size() < 772) v1.push_back(std::byte{0});
  v1.insert(v1.end(), stream.begin() + static_cast<std::ptrdiff_t>(header), stream.end());
  return v1;
}

}  // namespace

TEST_CASE("archive/lzfse: streams from compression_tool decode exactly") {
  CHECK(decode("lorem.lzfse") == tc::lorem());                     // LZVN block
  CHECK(decode("corpus4000.lzfse") == tc::text_corpus(4000));      // LZVN block
  CHECK(decode("hello.lzfse") == to_bytes("Hello, archive!\n"));  // stored block
  CHECK(decode("noise2000.lzfse") == test::tree::noise(2000));    // stored block
  CHECK(decode("empty.lzfse").empty());
  CHECK(decode("corpus60k.lzfse") == tc::text_corpus(60000));    // LZFSE v2 block
  CHECK(decode("corpus130k.lzfse") == tc::text_corpus(130000));  // two v2 blocks, matches across them
  CHECK(decode("mixed.lzfse") == tc::mixed());
  CHECK(decode("noise.lzfse") == test::tree::noise(4096));  // literal-only v2 block
}

TEST_CASE("archive/lzfse: v1 headers, stored blocks and block sequences") {
  const Bytes v1 = v2_to_v1(fixture("corpus60k.lzfse"));
  const auto plain = cz::decompress(cz::Method::Lzfse, v1);
  REQUIRE_OK(plain);
  CHECK(*plain == tc::text_corpus(60000));

  // Hand-made stream: a stored block, then an LZVN block that copies from it, then the end.
  Bytes s;
  put32(s, 0x2D787662u);
  put32(s, 5);
  append(s, as_bytes("abcde"));
  put32(s, 0x6E787662u);
  put32(s, 8);   // raw bytes
  put32(s, 11);  // payload
  // sml_d: L = 1, M = 3 + 4 = 7, D = 5: "x" then 7 bytes from 5 back.
  s.push_back(std::byte{0x60});
  s.push_back(std::byte{0x05});
  s.push_back(std::byte{'x'});
  for (const int b : {0x06, 0, 0, 0, 0, 0, 0, 0}) s.push_back(static_cast<std::byte>(b));
  put32(s, 0x24787662u);
  const auto out = cz::decompress(cz::Method::Lzfse, s);
  REQUIRE_OK(out);
  CHECK(test::str(*out) == "abcdexbcdexbc");
  // stop_after returns a prefix; max_output is enforced.
  cz::DecodeOptions d;
  d.stop_after = 7;
  CHECK(test::str(*cz::decompress(cz::Method::Lzfse, s, d)) == "abcdexb");
  cz::DecodeOptions small;
  small.max_output = 10;
  CHECK_STATUS(cz::decompress(cz::Method::Lzfse, s, small), Status::Limit);
  cz::DecodeOptions small_big;
  small_big.max_output = 1000;
  CHECK_STATUS(cz::decompress(cz::Method::Lzfse, fixture("corpus60k.lzfse"), small_big), Status::Limit);
}

TEST_CASE("archive/lzfse: damaged streams fail cleanly") {
  // Missing end-of-stream block, unknown block, absurd sizes.
  Bytes cut = fixture("corpus60k.lzfse");
  cut.resize(cut.size() - 4);
  CHECK_STATUS(cz::decompress(cz::Method::Lzfse, cut), Status::Decode);
  CHECK_STATUS(cz::decompress(cz::Method::Lzfse, as_bytes("bvx?\0\0\0\0")), Status::Decode);
  CHECK_STATUS(cz::decompress(cz::Method::Lzfse, {}), Status::Decode);
  Bytes huge;
  put32(huge, 0x6E787662u);
  put32(huge, 0xFFFFFFF0u);
  put32(huge, 8);
  for (const int b : {0x06, 0, 0, 0, 0, 0, 0, 0}) huge.push_back(static_cast<std::byte>(b));
  put32(huge, 0x24787662u);
  CHECK_STATUS(cz::decompress(cz::Method::Lzfse, huge), Status::Decode);
  cz::DecodeOptions limited;
  limited.max_output = 1u << 20;
  CHECK_STATUS(cz::decompress(cz::Method::Lzfse, huge, limited), Status::Limit);
  // Every LZVN opcode class with a bad distance or undefined opcode.
  for (const int op : {0x1E, 0x70, 0xD0, 0x46 /* previous distance, none yet */, 0xF3 /* same */}) {
    Bytes b;
    put32(b, 0x6E787662u);
    put32(b, 10);
    put32(b, 10);
    b.push_back(static_cast<std::byte>(op));
    b.push_back(std::byte{'a'});
    for (const int x : {0x06, 0, 0, 0, 0, 0, 0, 0}) b.push_back(static_cast<std::byte>(x));
    put32(b, 0x24787662u);
    INFO("opcode ", op);
    CHECK_STATUS(cz::decompress(cz::Method::Lzfse, b), Status::Decode);
  }
  for (const char* name : {"corpus60k.lzfse", "mixed.lzfse", "lorem.lzfse", "corpus4000.lzfse", "corpus130k.lzfse"}) {
    INFO("fixture ", name);
    tc::hostile(cz::Method::Lzfse, fixture(name));
  }
  // Every prefix of a small stream.
  const Bytes small = fixture("lorem.lzfse");
  for (std::size_t n = 0; n < small.size(); ++n) {
    CHECK_FALSE(cz::decompress(cz::Method::Lzfse, ByteView(small).first(n)).has_value());
  }
}

TEST_CASE("archive/lzfse: streaming decoder and progress") {
  const Bytes packed = fixture("corpus130k.lzfse");
  auto decoder = cz::make_decoder(cz::Method::Lzfse);
  REQUIRE_OK(decoder);
  Bytes out;
  for (std::size_t at = 0; at < packed.size(); at += 1000) {
    const std::size_t take = std::min<std::size_t>(1000, packed.size() - at);
    REQUIRE_OK((*decoder)->decode(ByteView(packed).subspan(at, take), out, at + take == packed.size()));
  }
  CHECK((*decoder)->finished());
  CHECK(out == tc::text_corpus(130000));
  cz::DecodeOptions d;
  d.progress = Progress([](double, std::string_view) { return true; });
  CHECK_STATUS(cz::decompress(cz::Method::Lzfse, packed, d), Status::Cancelled);
}
