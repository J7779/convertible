// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/archive/codec_test_util.hpp"

using namespace cvt;
namespace cz = cvt::compression;
namespace tc = cvt::test::codec;

namespace {
Bytes decode_ok(cz::Method m, std::string_view fixture) {
  INFO("fixture ", fixture);
  auto out = cz::decompress(m, test::fixture(std::string("archive/codecs/") + std::string(fixture)));
  REQUIRE_OK(out);
  return std::move(*out);
}
}  // namespace

TEST_CASE("archive/xz: decodes files written by xz 5.4") {
  CHECK(decode_ok(cz::Method::Xz, "lorem.txt.xz") == tc::lorem());
  CHECK(decode_ok(cz::Method::Xz, "noise_crc32.xz") == test::tree::noise(4096));
  CHECK(decode_ok(cz::Method::Xz, "mixed_sha256.xz") == tc::mixed());
  CHECK(test::str(decode_ok(cz::Method::Xz, "hello_nocheck.xz")) == "Hello, archive!\n");
  CHECK(decode_ok(cz::Method::Xz, "lorem_blocks.xz") == tc::lorem());
  CHECK(decode_ok(cz::Method::Xz, "mixed_x86.xz") == tc::mixed());
  CHECK(decode_ok(cz::Method::Xz, "mixed_delta4.xz") == tc::mixed());
  CHECK(decode_ok(cz::Method::Xz, "mixed_arm64.xz") == tc::mixed());
  CHECK(test::str(decode_ok(cz::Method::Xz, "two_streams.xz")) == "Hello, archive!\nHello, archive!\n");
  CHECK(decode_ok(cz::Method::Xz, "empty.xz").empty());
}

TEST_CASE("archive/lzma: decodes .lzma and lzip files") {
  CHECK(decode_ok(cz::Method::LzmaAlone, "lorem.txt.lzma") == tc::lorem());
  CHECK(decode_ok(cz::Method::Lzip, "lorem.txt.lz") == tc::lorem());
  CHECK(test::str(decode_ok(cz::Method::Lzip, "two_members.lz")) == "Hello, archive!\nHello, archive!\n");
}

TEST_CASE("archive/lzma: every container round trips at every level") {
  const Bytes text = tc::text_corpus(200000);
  const Bytes noise = test::random_bytes(70000, 5);
  for (const cz::Method m : {cz::Method::Xz, cz::Method::LzmaAlone, cz::Method::Lzip}) {
    INFO("method ", cz::method_name(m));
    std::size_t previous = 0;
    for (const int level : {0, 3, 6, 9}) {
      const std::size_t size = tc::round_trip(m, text, level);
      if (previous != 0 && level >= 6) CHECK_LE(size, previous + previous / 100);
      previous = size;
    }
    CHECK_LT(previous, text.size() / 3);
    tc::round_trip(m, noise, 6);
    tc::round_trip(m, {}, 6);
    tc::round_trip(m, as_bytes("x"), 1);
    tc::round_trip(m, Bytes(100000, std::byte{0}), 9);
  }
}

TEST_CASE("archive/lzma: raw LZMA and LZMA2 with properties") {
  const Bytes text = tc::text_corpus(50000);
  for (const cz::Method m : {cz::Method::LzmaRaw, cz::Method::Lzma2Raw}) {
    cz::EncodeOptions e;
    e.level = 5;
    const auto props = cz::encoder_properties(m, e, text.size());
    REQUIRE_OK(props);
    CHECK_EQ(props->size(), m == cz::Method::LzmaRaw ? 5u : 1u);
    const auto packed = cz::compress(m, text, e);
    REQUIRE_OK(packed);
    cz::DecodeOptions d;
    d.properties = *props;
    d.output_size = text.size();
    const auto plain = cz::decompress(m, *packed, d);
    REQUIRE_OK(plain);
    CHECK(*plain == text);
    d.properties = {};
    CHECK_STATUS(cz::decompress(m, *packed, d), Status::Decode);
  }
  // LZMA2 stores incompressible data in uncompressed chunks and resets state afterwards.
  Bytes mixed = test::random_bytes(150000, 8);
  append(mixed, tc::text_corpus(150000));
  append(mixed, test::random_bytes(80000, 9));
  append(mixed, tc::text_corpus(90000));
  tc::round_trip(cz::Method::Xz, mixed, 4);
}

TEST_CASE("archive/xz: checks, filters and options") {
  const Bytes text = tc::text_corpus(30000);
  for (const cz::XzCheck check : {cz::XzCheck::None, cz::XzCheck::Crc32, cz::XzCheck::Crc64, cz::XzCheck::Sha256}) {
    cz::EncodeOptions e;
    e.check = check;
    const auto packed = cz::compress(cz::Method::Xz, text, e);
    REQUIRE_OK(packed);
    CHECK_EQ(std::to_integer<int>((*packed)[7]), static_cast<int>(check));
    const auto plain = cz::decompress(cz::Method::Xz, *packed);
    REQUIRE_OK(plain);
    CHECK(*plain == text);
    if (check != cz::XzCheck::None) {
      // A damaged check is reported.
      Bytes bad = *packed;
      bad[bad.size() - 32 - (check == cz::XzCheck::Sha256 ? 0 : 0) - 13] ^= std::byte{1};
      CHECK_FALSE(cz::decompress(cz::Method::Xz, bad).has_value());
    }
  }
  // Branch filters and delta: x86-like code calling a few functions (relative E8 targets).
  Bytes code;
  for (std::uint32_t i = 0; i < 20000; ++i) {
    code.push_back(std::byte{0xE8});
    const std::uint32_t function = 0x400 * (1 + (i * 7) % 13);
    const std::uint32_t relative = function - static_cast<std::uint32_t>(code.size() + 4);
    for (int k = 0; k < 4; ++k) code.push_back(static_cast<std::byte>((relative >> (8 * k)) & 0xFF));
    code.push_back(std::byte{0x90});
  }
  for (const cz::FilterKind kind : {cz::FilterKind::X86, cz::FilterKind::Arm, cz::FilterKind::ArmThumb,
                                    cz::FilterKind::Arm64, cz::FilterKind::PowerPc, cz::FilterKind::Sparc,
                                    cz::FilterKind::Ia64, cz::FilterKind::Delta}) {
    INFO("filter ", static_cast<int>(kind));
    cz::EncodeOptions e;
    e.filters = {cz::Filter{kind, kind == cz::FilterKind::Delta ? 4u : 0u}};
    const auto packed = cz::compress(cz::Method::Xz, code, e);
    REQUIRE_OK(packed);
    const auto plain = cz::decompress(cz::Method::Xz, *packed);
    REQUIRE_OK(plain);
    CHECK(*plain == code);
  }
  cz::EncodeOptions plain_e;
  cz::EncodeOptions x86_e;
  x86_e.filters = {cz::Filter{cz::FilterKind::X86, 0}};
  CHECK_LT(cz::compress(cz::Method::Xz, code, x86_e)->size(), cz::compress(cz::Method::Xz, code, plain_e)->size());
}

TEST_CASE("archive/lzma: prefixes, limits and hostile input") {
  const Bytes text = tc::text_corpus(100000);
  for (const cz::Method m : {cz::Method::Xz, cz::Method::LzmaAlone, cz::Method::Lzip}) {
    INFO("method ", cz::method_name(m));
    const auto packed = cz::compress(m, text);
    REQUIRE_OK(packed);
    cz::DecodeOptions prefix;
    prefix.stop_after = 777;
    const auto head = cz::decompress(m, *packed, prefix);
    REQUIRE_OK(head);
    CHECK(*head == Bytes(text.begin(), text.begin() + 777));
    cz::DecodeOptions small;
    small.max_output = 10000;
    CHECK_STATUS(cz::decompress(m, *packed, small), Status::Limit);
    CHECK_STATUS(cz::decompress(m, ByteView(*packed).first(packed->size() / 2)), Status::Decode);
  }
  tc::hostile(cz::Method::Xz, test::fixture("archive/codecs/lorem.txt.xz"));
  tc::hostile(cz::Method::Xz, test::fixture("archive/codecs/mixed_x86.xz"));
  tc::hostile(cz::Method::LzmaAlone, test::fixture("archive/codecs/lorem.txt.lzma"));
  tc::hostile(cz::Method::Lzip, test::fixture("archive/codecs/lorem.txt.lz"));
  // An absurd declared size never allocates up front.
  Bytes bomb = test::fixture("archive/codecs/lorem.txt.lzma");
  for (int i = 5; i < 13; ++i) bomb[static_cast<std::size_t>(i)] = std::byte{0x7F};
  CHECK_FALSE(cz::decompress(cz::Method::LzmaAlone, bomb, {.max_output = 1u << 20}).has_value());
}
