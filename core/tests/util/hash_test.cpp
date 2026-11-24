// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>

#include "support/test_support.hpp"
#include "util/crc.hpp"
#include "util/hash.hpp"

using namespace cvt;
using namespace cvt::util;

namespace {
constexpr std::string_view fox = "The quick brown fox jumps over the lazy dog";
constexpr std::string_view nist = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
}  // namespace

TEST_CASE("crc32: known vectors and incremental use") {
  CHECK(crc32(as_bytes("123456789")) == 0xCBF43926u);
  CHECK(crc32(as_bytes("hello")) == 0x3610A686u);
  CHECK(crc32({}) == 0u);
  CHECK(crc32(as_bytes("6789"), crc32(as_bytes("12345"))) == 0xCBF43926u);
  static constexpr std::array<std::byte, 1> a = {std::byte{'a'}};
  static_assert(crc32(ByteView(a)) == 0xE8B7BE43u);
}

TEST_CASE("adler32: known vectors and incremental use") {
  CHECK(adler32(as_bytes("Wikipedia")) == 0x11E60398u);
  CHECK(adler32({}) == 1u);
  CHECK(adler32(as_bytes("pedia"), adler32(as_bytes("Wiki"))) == 0x11E60398u);
  // Long runs of 0xFF stress the deferred modulo.
  const Bytes ones(100000, std::byte{0xFF});
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (std::size_t i = 0; i < ones.size(); ++i) {
    a = (a + 0xFF) % 65521;
    b = (b + a) % 65521;
  }
  CHECK(adler32(ones) == ((b << 16) | a));
}

TEST_CASE("md5: known vectors") {
  CHECK(md5_hex({}) == "d41d8cd98f00b204e9800998ecf8427e");
  CHECK(md5_hex(as_bytes("abc")) == "900150983cd24fb0d6963f7d28e17f72");
  CHECK(md5_hex(as_bytes(fox)) == "9e107d9d372bb6826bd81d3542a419d6");
  CHECK(md5_hex(as_bytes(nist)) == "8215ef0796a20bcaaae116d3876c664a");
  CHECK(md5_hex(as_bytes(std::string(1000000, 'a'))) == "7707d6ae4e027c70eea2a935c2296f21");
}

TEST_CASE("sha1: known vectors") {
  CHECK(sha1_hex({}) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(sha1_hex(as_bytes("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(sha1_hex(as_bytes(fox)) == "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
  CHECK(sha1_hex(as_bytes(nist)) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  CHECK(sha1_hex(as_bytes(std::string(1000000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("sha256: known vectors") {
  CHECK(sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex(as_bytes("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex(as_bytes(fox)) == "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
  CHECK(sha256_hex(as_bytes(nist)) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(sha256_hex(as_bytes(std::string(1000000, 'a'))) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("hashes: feeding data in pieces gives the same digest at every block boundary") {
  const Bytes data = test::random_bytes(300, 7);
  for (std::size_t total : {0u, 1u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 128u, 300u}) {
    const ByteView whole = ByteView(data).first(total);
    for (std::size_t split : {std::size_t{0}, total / 3, total / 2, total}) {
      Md5 m;
      Sha1 s1;
      Sha256 s2;
      for (const ByteView part : {whole.first(split), whole.subspan(split)}) {
        m.update(part);
        s1.update(part);
        s2.update(part);
      }
      CHECK(m.finish() == md5(whole));
      CHECK(s1.finish() == sha1(whole));
      CHECK(s2.finish() == sha256(whole));
    }
  }
  // Byte-at-a-time.
  Sha256 slow;
  for (const std::byte& b : data) slow.update(ByteView(&b, 1));
  CHECK(slow.finish() == sha256(data));
}
