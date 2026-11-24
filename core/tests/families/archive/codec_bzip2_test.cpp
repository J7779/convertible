// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/archive/codec_test_util.hpp"

using namespace cvt;
namespace cz = cvt::compression;
namespace tc = cvt::test::codec;

TEST_CASE("archive/bzip2: decodes files written by bzip2 1.0.8") {
  const auto lorem = cz::decompress(cz::Method::Bzip2, test::fixture("archive/codecs/lorem.txt.bz2"));
  REQUIRE_OK(lorem);
  CHECK(*lorem == tc::lorem());
  const auto noise = cz::decompress(cz::Method::Bzip2, test::fixture("archive/codecs/noise.bin.bz2"));
  REQUIRE_OK(noise);
  CHECK(*noise == test::tree::noise(4096));
  const auto empty = cz::decompress(cz::Method::Bzip2, test::fixture("archive/codecs/empty.bz2"));
  REQUIRE_OK(empty);
  CHECK(empty->empty());
  const auto two = cz::decompress(cz::Method::Bzip2, test::fixture("archive/codecs/two_streams.bz2"));
  REQUIRE_OK(two);
  Bytes expected = to_bytes("Hello, archive!\n");
  append(expected, tc::lorem());
  CHECK(*two == expected);
}

TEST_CASE("archive/bzip2: round trips at every level, smaller at higher levels") {
  const Bytes text = tc::text_corpus(300000);
  std::size_t previous = 0;
  for (const int level : {1, 5, 9}) {
    const std::size_t size = tc::round_trip(cz::Method::Bzip2, text, level);
    if (previous != 0) CHECK_LE(size, previous + previous / 200);
    previous = size;
  }
  CHECK_LT(previous, text.size() / 3);
  tc::round_trip(cz::Method::Bzip2, test::random_bytes(50000, 4), 9);
  tc::round_trip(cz::Method::Bzip2, {}, 9);
  tc::round_trip(cz::Method::Bzip2, as_bytes("a"), 1);
  // Degenerate inputs: long runs, a short period, and run lengths around the RLE1 limits.
  tc::round_trip(cz::Method::Bzip2, Bytes(250000, std::byte{'x'}), 1);
  std::string period;
  for (int i = 0; i < 40000; ++i) period += "abc";
  tc::round_trip(cz::Method::Bzip2, as_bytes(period), 2);
  Bytes runs;
  for (int len = 1; len < 600; len += 7) runs.insert(runs.end(), static_cast<std::size_t>(len), static_cast<std::byte>(len));
  tc::round_trip(cz::Method::Bzip2, runs, 1);
}

TEST_CASE("archive/bzip2: streaming encoder equals one-shot; prefix and limits") {
  const Bytes text = tc::text_corpus(150000);
  auto encoder = cz::make_encoder(cz::Method::Bzip2, {.level = 1});
  REQUIRE_OK(encoder);
  Bytes streamed;
  for (std::size_t at = 0; at < text.size(); at += 3333) {
    REQUIRE_OK((*encoder)->encode(ByteView(text).subspan(at, std::min<std::size_t>(3333, text.size() - at)), streamed));
  }
  REQUIRE_OK((*encoder)->finish(streamed));
  cz::EncodeOptions one;
  one.level = 1;
  const auto oneshot = cz::compress(cz::Method::Bzip2, text, one);
  REQUIRE_OK(oneshot);
  CHECK(streamed == *oneshot);

  cz::DecodeOptions prefix;
  prefix.stop_after = 1000;
  const auto head = cz::decompress(cz::Method::Bzip2, streamed, prefix);
  REQUIRE_OK(head);
  CHECK(*head == Bytes(text.begin(), text.begin() + 1000));
  cz::DecodeOptions small;
  small.max_output = 5000;
  CHECK_STATUS(cz::decompress(cz::Method::Bzip2, streamed, small), Status::Limit);
}

TEST_CASE("archive/bzip2: damaged input fails cleanly") {
  const Bytes valid = test::fixture("archive/codecs/lorem.txt.bz2");
  CHECK_STATUS(cz::decompress(cz::Method::Bzip2, ByteView(valid).first(valid.size() - 5)), Status::Decode);
  Bytes flipped = valid;
  flipped[200] ^= std::byte{0x10};
  CHECK_FALSE(cz::decompress(cz::Method::Bzip2, flipped).has_value());
  CHECK_STATUS(cz::decompress(cz::Method::Bzip2, as_bytes("BZh0")), Status::Decode);
  tc::hostile(cz::Method::Bzip2, valid);
  tc::hostile(cz::Method::Bzip2, test::fixture("archive/codecs/noise.bin.bz2"));
}
