// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/archive/codec_test_util.hpp"

using namespace cvt;
namespace cz = cvt::compression;
namespace tc = cvt::test::codec;

namespace {
Bytes zstd_fixture(std::string_view name) {
  INFO("fixture ", name);
  auto out = cz::decompress(cz::Method::Zstd, test::fixture(std::string("archive/codecs/") + std::string(name)));
  REQUIRE_OK(out);
  return std::move(*out);
}
}  // namespace

TEST_CASE("archive/zstd: decodes frames written by zstd 1.5 and python-zstandard") {
  CHECK(zstd_fixture("lorem_1.zst") == tc::lorem());
  CHECK(zstd_fixture("mixed_19.zst") == tc::mixed());
  CHECK(zstd_fixture("noise_nocheck.zst") == test::tree::noise(4096));
  CHECK(zstd_fixture("lorem_stream.zst") == tc::lorem());
  CHECK(zstd_fixture("empty.zst").empty());
  Bytes frames = to_bytes("first frame ");
  append(frames, tc::lorem());
  CHECK(zstd_fixture("frames_skippable.zst") == frames);
  Bytes runs(70000, std::byte{'a'});
  runs.insert(runs.end(), 70000, std::byte{'b'});
  CHECK(zstd_fixture("runs.zst") == runs);
}

TEST_CASE("archive/zstd: round trips at every level, with real compression") {
  const Bytes text = tc::text_corpus(400000);
  std::size_t level1 = 0;
  std::size_t level9 = 0;
  for (const int level : {0, 1, 3, 6, 9}) {
    const std::size_t size = tc::round_trip(cz::Method::Zstd, text, level);
    if (level == 1) level1 = size;
    if (level == 9) level9 = size;
  }
  CHECK_LT(level9, level1);
  CHECK_LT(level1, text.size() / 3);
  tc::round_trip(cz::Method::Zstd, test::random_bytes(200000, 6), 3);
  tc::round_trip(cz::Method::Zstd, {}, 3);
  tc::round_trip(cz::Method::Zstd, as_bytes("z"), 3);
  tc::round_trip(cz::Method::Zstd, Bytes(300000, std::byte{7}), 3);
  // Binary data with more than 128 distinct literal values (FSE-coded Huffman weights).
  Bytes binary;
  for (std::uint32_t i = 0; i < 60000; ++i) binary.push_back(static_cast<std::byte>((i * i) % 251 ^ (i % 7)));
  append(binary, tc::text_corpus(30000));
  tc::round_trip(cz::Method::Zstd, binary, 6);
  // Without content size or checksum.
  cz::EncodeOptions e;
  e.content_size = false;
  e.content_checksum = false;
  const auto packed = cz::compress(cz::Method::Zstd, text, e);
  REQUIRE_OK(packed);
  const auto plain = cz::decompress(cz::Method::Zstd, *packed);
  REQUIRE_OK(plain);
  CHECK(*plain == text);
}

TEST_CASE("archive/zstd: prefixes, limits, dictionaries and hostile input") {
  const Bytes text = tc::text_corpus(300000);
  const auto packed = cz::compress(cz::Method::Zstd, text);
  REQUIRE_OK(packed);
  cz::DecodeOptions prefix;
  prefix.stop_after = 1000;
  const auto head = cz::decompress(cz::Method::Zstd, *packed, prefix);
  REQUIRE_OK(head);
  CHECK(*head == Bytes(text.begin(), text.begin() + 1000));
  cz::DecodeOptions small;
  small.max_output = 1000;
  CHECK_STATUS(cz::decompress(cz::Method::Zstd, *packed, small), Status::Limit);
  // A frame that names a dictionary is rejected with a clear message.
  Bytes dict_frame = test::bytes(std::string_view("\x28\xB5\x2F\xFD\x01\x00\x07\x01\x00\x00", 10));
  const auto needs = cz::decompress(cz::Method::Zstd, dict_frame);
  CHECK_STATUS(needs, Status::Decode);
  Bytes bad = *packed;
  bad[bad.size() - 2] ^= std::byte{0x40};
  CHECK_STATUS(cz::decompress(cz::Method::Zstd, bad), Status::Decode);
  tc::hostile(cz::Method::Zstd, test::fixture("archive/codecs/mixed_19.zst"));
  tc::hostile(cz::Method::Zstd, test::fixture("archive/codecs/lorem_1.zst"));
  tc::hostile(cz::Method::Zstd, ByteView(*packed).first(5000));
}
