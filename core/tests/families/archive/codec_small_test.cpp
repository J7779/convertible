// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// LZ4, Snappy and Unix compress (.Z).
#include "families/archive/codec_test_util.hpp"

using namespace cvt;
namespace cz = cvt::compression;
namespace tc = cvt::test::codec;

namespace {
Bytes decode_fixture(cz::Method m, std::string_view name) {
  INFO("fixture ", name);
  auto out = cz::decompress(m, test::fixture(std::string("archive/codecs/") + std::string(name)));
  REQUIRE_OK(out);
  return std::move(*out);
}
}  // namespace

TEST_CASE("archive/lz4: decodes frames written by lz4 1.9") {
  CHECK(decode_fixture(cz::Method::Lz4Frame, "lorem_1.lz4") == tc::lorem());
  CHECK(decode_fixture(cz::Method::Lz4Frame, "mixed_9_linked.lz4") == tc::mixed());
  CHECK(decode_fixture(cz::Method::Lz4Frame, "noise_nocrc.lz4") == test::tree::noise(4096));
  CHECK(decode_fixture(cz::Method::Lz4Frame, "lorem_legacy.lz4") == tc::lorem());
  CHECK(decode_fixture(cz::Method::Lz4Frame, "empty.lz4").empty());
}

TEST_CASE("archive/lz4: frame and block round trips, HC is smaller") {
  const Bytes text = tc::text_corpus(300000);
  const std::size_t fast = tc::round_trip(cz::Method::Lz4Frame, text, 1);
  const std::size_t hc = tc::round_trip(cz::Method::Lz4Frame, text, 9);
  CHECK_LT(hc, fast);
  CHECK_LT(fast, text.size() / 2);
  tc::round_trip(cz::Method::Lz4Frame, test::random_bytes(100000, 2), 5);
  tc::round_trip(cz::Method::Lz4Frame, {}, 5);
  tc::round_trip(cz::Method::Lz4Frame, as_bytes("tiny"), 5);
  tc::round_trip(cz::Method::Lz4Block, text, 3);
  tc::round_trip(cz::Method::Lz4Block, as_bytes("short"), 3);
  cz::EncodeOptions small_blocks;
  small_blocks.block_size = 65536;
  small_blocks.content_size = false;
  const auto packed = cz::compress(cz::Method::Lz4Frame, text, small_blocks);
  REQUIRE_OK(packed);
  CHECK(*cz::decompress(cz::Method::Lz4Frame, *packed) == text);
  tc::hostile(cz::Method::Lz4Frame, test::fixture("archive/codecs/mixed_9_linked.lz4"));
  cz::DecodeOptions block_opts;
  block_opts.output_size = 3568;
  tc::hostile(cz::Method::Lz4Block, *cz::compress(cz::Method::Lz4Block, tc::lorem()), block_opts);
}

TEST_CASE("archive/snappy: raw and framed streams from python-snappy") {
  CHECK(decode_fixture(cz::Method::Snappy, "lorem.snappy") == tc::lorem());
  Bytes expected;
  for (int i = 0; i < 20; ++i) append(expected, tc::lorem());
  append(expected, test::tree::noise(4096));
  CHECK(decode_fixture(cz::Method::SnappyFramed, "mixed.sz") == expected);
  const Bytes text = tc::text_corpus(200000);
  CHECK_LT(tc::round_trip(cz::Method::Snappy, text, 6), text.size() / 2);
  tc::round_trip(cz::Method::SnappyFramed, text, 6);
  tc::round_trip(cz::Method::SnappyFramed, test::random_bytes(70000, 3), 6);
  tc::round_trip(cz::Method::SnappyFramed, {}, 6);
  tc::hostile(cz::Method::SnappyFramed, test::fixture("archive/codecs/mixed.sz"));
  tc::hostile(cz::Method::Snappy, test::fixture("archive/codecs/lorem.snappy"));
}

TEST_CASE("archive/compress: .Z files from ncompress at 12 and 16 bits") {
  CHECK(decode_fixture(cz::Method::Lzw, "lorem.txt.Z") == tc::lorem());
  CHECK(decode_fixture(cz::Method::Lzw, "mixed_b12.Z") == tc::mixed());
  const Bytes text = tc::text_corpus(400000);
  CHECK_LT(tc::round_trip(cz::Method::Lzw, text, 6), text.size() / 2);
  tc::round_trip(cz::Method::Lzw, test::random_bytes(300000, 4), 6);  // fills the table, CLEAR codes
  tc::round_trip(cz::Method::Lzw, {}, 6);
  tc::round_trip(cz::Method::Lzw, Bytes(100000, std::byte{'a'}), 6);
  tc::hostile(cz::Method::Lzw, test::fixture("archive/codecs/mixed_b12.Z"));
}
