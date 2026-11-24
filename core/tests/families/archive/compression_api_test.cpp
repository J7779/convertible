// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// The compression.hpp dispatcher, exercised through the methods that wrap
// util/deflate (Store, Deflate, zlib, gzip), plus detection and names.
#include <string>

#include "support/test_support.hpp"

#include "families/archive/compression.hpp"

using namespace cvt;
namespace cz = cvt::compression;

TEST_CASE("archive/compression: names round trip and aliases resolve") {
  for (const cz::Method m : {cz::Method::Store, cz::Method::Deflate, cz::Method::Zlib, cz::Method::Gzip, cz::Method::Bzip2,
                             cz::Method::Xz, cz::Method::Zstd, cz::Method::Brotli, cz::Method::Lz4Frame,
                             cz::Method::SnappyFramed, cz::Method::Lzw, cz::Method::Ppmd7, cz::Method::Adc}) {
    const auto back = cz::parse_method(cz::method_name(m));
    REQUIRE(back.has_value());
    CHECK(*back == m);
  }
  CHECK(cz::parse_method("GZ") == cz::Method::Gzip);
  CHECK(cz::parse_method("zst") == cz::Method::Zstd);
  CHECK(cz::parse_method("br") == cz::Method::Brotli);
  CHECK_FALSE(cz::parse_method("nonsense").has_value());
}

TEST_CASE("archive/compression: deflate family round trips through the dispatcher") {
  const Bytes text = to_bytes(std::string(5000, 'a') + "some text that repeats some text that repeats");
  const Bytes noise = test::random_bytes(3000, 3);
  for (const cz::Method m : {cz::Method::Store, cz::Method::Deflate, cz::Method::Zlib, cz::Method::Gzip}) {
    INFO("method ", cz::method_name(m));
    CHECK(cz::can_decode(m));
    CHECK(cz::can_encode(m));
    for (const Bytes* input : {&text, &noise}) {
      for (const int level : {0, 1, 6, 9}) {
        cz::EncodeOptions e;
        e.level = level;
        const auto packed = cz::compress(m, *input, e);
        REQUIRE_OK(packed);
        const auto plain = cz::decompress(m, *packed);
        REQUIRE_OK(plain);
        CHECK(*plain == *input);
      }
    }
    const auto empty = cz::compress(m, {});
    REQUIRE_OK(empty);
    const auto back = cz::decompress(m, *empty);
    REQUIRE_OK(back);
    CHECK(back->empty());
  }
}

TEST_CASE("archive/compression: limits, prefixes and truncation") {
  const Bytes text = to_bytes(std::string(100000, 'z'));
  const auto packed = cz::compress(cz::Method::Gzip, text);
  REQUIRE_OK(packed);
  cz::DecodeOptions limited;
  limited.max_output = 1000;
  CHECK_STATUS(cz::decompress(cz::Method::Gzip, *packed, limited), Status::Limit);
  cz::DecodeOptions prefix;
  prefix.stop_after = 512;
  const auto head = cz::decompress(cz::Method::Gzip, *packed, prefix);
  REQUIRE_OK(head);
  CHECK_EQ(head->size(), 512u);
  CHECK_STATUS(cz::decompress(cz::Method::Gzip, ByteView(*packed).first(packed->size() - 3)), Status::Decode);
  CHECK_STATUS(cz::decompress(cz::Method::Zlib, test::random_bytes(64, 9)), Status::Decode);
}

TEST_CASE("archive/compression: streaming equals one-shot, in any split") {
  const Bytes input = test::random_bytes(20000, 11);
  Bytes doubled = input;
  append(doubled, input);  // compressible
  const auto packed = cz::compress(cz::Method::Gzip, doubled);
  REQUIRE_OK(packed);
  // gzip with two members joins them.
  Bytes two_members = *packed;
  append(two_members, *packed);
  for (const std::size_t piece : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
    INFO("piece ", piece);
    auto decoder = cz::make_decoder(cz::Method::Gzip);
    REQUIRE_OK(decoder);
    Bytes out;
    for (std::size_t at = 0; at < two_members.size(); at += piece) {
      const std::size_t n = std::min(piece, two_members.size() - at);
      const bool last = at + n == two_members.size();
      REQUIRE_OK((*decoder)->decode(ByteView(two_members).subspan(at, n), out, last));
    }
    CHECK((*decoder)->finished());
    CHECK_EQ(out.size(), doubled.size() * 2);
  }
  auto encoder = cz::make_encoder(cz::Method::Zlib);
  REQUIRE_OK(encoder);
  Bytes streamed;
  for (std::size_t at = 0; at < doubled.size(); at += 999) {
    REQUIRE_OK((*encoder)->encode(ByteView(doubled).subspan(at, std::min<std::size_t>(999, doubled.size() - at)), streamed));
  }
  REQUIRE_OK((*encoder)->finish(streamed));
  const auto oneshot = cz::compress(cz::Method::Zlib, doubled);
  REQUIRE_OK(oneshot);
  CHECK(streamed == *oneshot);
}

TEST_CASE("archive/compression: detection by magic number") {
  CHECK(cz::detect(*cz::compress(cz::Method::Gzip, as_bytes("x"))) == cz::Method::Gzip);
  CHECK(cz::detect(*cz::compress(cz::Method::Zlib, as_bytes("x"))) == cz::Method::Zlib);
  CHECK(cz::detect(test::bytes("BZh91AY&SY")) == cz::Method::Bzip2);
  CHECK(cz::detect(test::bytes(std::string_view("\xFD" "7zXZ\0\0\x04", 8))) == cz::Method::Xz);
  CHECK(cz::detect(test::bytes(std::string_view("\x28\xB5\x2F\xFD\x00", 5))) == cz::Method::Zstd);
  CHECK(cz::detect(test::bytes(std::string_view("\x04\x22\x4D\x18\x64", 5))) == cz::Method::Lz4Frame);
  CHECK(cz::detect(test::bytes("LZIP\x01")) == cz::Method::Lzip);
  CHECK(cz::detect(test::bytes(std::string_view("\x1F\x9D\x90", 3))) == cz::Method::Lzw);
  CHECK(cz::detect(test::bytes(std::string_view("\xFF\x06\x00\x00sNaPpY", 10))) == cz::Method::SnappyFramed);
  CHECK(cz::detect(test::bytes(std::string_view("\x5D\x00\x00\x80\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", 14))) ==
        cz::Method::LzmaAlone);
  CHECK_FALSE(cz::detect(test::bytes("plain text")).has_value());
  CHECK_FALSE(cz::detect({}).has_value());
}
