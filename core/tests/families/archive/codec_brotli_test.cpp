// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/archive/codec_test_util.hpp"
#include "families/archive/codecs/brotli_internal.hpp"
#include "util/hash.hpp"

using namespace cvt;
namespace cz = cvt::compression;
namespace tc = cvt::test::codec;

namespace {

Bytes br_fixture(std::string_view path) {
  INFO("fixture ", path);
  auto out = cz::decompress(cz::Method::Brotli, test::fixture(std::string(path)));
  REQUIRE_OK(out);
  return std::move(*out);
}

Bytes exec_sh() {
  for (const test::tree::File& f : test::tree::files()) {
    if (f.path == "exec.sh") return f.bytes;
  }
  return {};
}

/// Fixed-size little-endian records (the input of records_*.br).
Bytes records(std::uint32_t count) {
  Bytes out;
  const auto le32 = [&](std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
  };
  for (std::uint32_t i = 0; i < count; ++i) {
    le32(i * 3);
    le32((i / 7) * 1000);
    le32(0x10);
    le32(0xEEFFu);
  }
  return out;
}

Bytes slice(const Bytes& b, std::size_t n) { return Bytes(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n)); }

Bytes encode(ByteView in, int level, std::uint32_t dictionary_size = 0) {
  cz::EncodeOptions e;
  e.level = level;
  e.dictionary_size = dictionary_size;
  auto packed = cz::compress(cz::Method::Brotli, in, e);
  REQUIRE_OK(packed);
  return std::move(*packed);
}

/// Text mixed with UTF-8 words, binary records and a stretch of noise (several block types).
Bytes heterogeneous() {
  Bytes out = tc::text_corpus(40000);
  append(out, test::fixture("archive/brotli/utf8.txt"));
  append(out, records(2000));
  append(out, test::tree::noise(5000));
  append(out, tc::text_corpus(20000));
  return out;
}

}  // namespace

TEST_CASE("archive/brotli: decodes streams written by brotli 1.0.9 and python brotli") {
  CHECK(br_fixture("archive/codecs/lorem_q0.br") == tc::lorem());
  CHECK(br_fixture("archive/codecs/lorem_q5.br") == tc::lorem());
  CHECK(br_fixture("archive/codecs/mixed_q11.br") == tc::mixed());
  CHECK(br_fixture("archive/codecs/exec_q9.br") == exec_sh());
  CHECK(br_fixture("archive/codecs/empty.br").empty());
  // Static dictionary references (python brotli, text mode).
  const Bytes prose = test::fixture("archive/codecs/prose.txt");
  CHECK(br_fixture("archive/codecs/prose_q11.br") == prose);
  CHECK(br_fixture("archive/codecs/prose_q4.br") == prose);
}

TEST_CASE("archive/brotli: block switching, context modes, NPOSTFIX/NDIRECT, windows, metadata") {
  const Bytes text = tc::text_corpus(60000);
  CHECK(br_fixture("archive/brotli/text_q11_w18.br") == text);  // block types for all three categories
  CHECK(br_fixture("archive/brotli/text_q7_w10.br") == slice(text, 20000));
  CHECK(br_fixture("archive/brotli/text_q2_w24.br") == slice(text, 20000));
  const Bytes rec = records(3000);
  CHECK(br_fixture("archive/brotli/records_p2_d12.br") == rec);
  CHECK(br_fixture("archive/brotli/records_p3_d120.br") == rec);
  CHECK(br_fixture("archive/brotli/records_q11.br") == rec);  // signed context mode
  const Bytes utf8 = test::fixture("archive/brotli/utf8.txt");
  CHECK(br_fixture("archive/brotli/utf8_q11.br") == utf8);  // UTF8 context mode on multi-byte text
  CHECK(br_fixture("archive/brotli/utf8_q6.br") == utf8);
  CHECK(br_fixture("archive/brotli/noise_q5.br") == test::tree::noise(20000));  // uncompressed meta-blocks
  CHECK(br_fixture("archive/brotli/metadata.br") == to_bytes("hello"));
  // Every one of the 121 word transforms, applied to ASCII and UTF-8 dictionary words.
  CHECK(br_fixture("archive/brotli/transforms.br") == test::fixture("archive/brotli/transforms.bin"));
  // Large-window streams are an extension outside RFC 7932.
  CHECK_STATUS(cz::decompress(cz::Method::Brotli, test::fixture("archive/brotli/large_window.br")), Status::Decode);
}

TEST_CASE("archive/brotli: embedded tables match libbrotli's") {
  namespace br = cz::detail::brotli;
  const auto dict = br::dictionary();
  CHECK(util::sha256_hex(ByteView(reinterpret_cast<const std::byte*>(dict.data()), dict.size())) ==
        "20e42eb1b511c21806d4d227d07e5dd06877d8ce7b3a817f378f313653f35c70");
  Bytes lut;
  for (const auto& mode : br::context_lookup) {
    for (const auto& half : mode) {
      for (const std::uint8_t v : half) lut.push_back(static_cast<std::byte>(v));
    }
  }
  CHECK(util::sha256_hex(lut) == "0e4abc034ef46244934d117f15876cd477d0fb2ac94a023093338e72e37f7ad9");
}

TEST_CASE("archive/brotli: round trips at levels 0, 1, 5 and 9 with real compression") {
  const Bytes text = tc::text_corpus(120000);
  const std::size_t l0 = tc::round_trip(cz::Method::Brotli, text, 0);
  const std::size_t l1 = tc::round_trip(cz::Method::Brotli, text, 1);
  const std::size_t l5 = tc::round_trip(cz::Method::Brotli, text, 5);
  const std::size_t l9 = tc::round_trip(cz::Method::Brotli, text, 9);
  CHECK_GE(l0, text.size());
  CHECK_LT(l1, text.size() / 3);
  CHECK_LE(l5, l1);
  CHECK_LE(l9, l5);
  for (const int level : {0, 1, 5, 9}) {
    INFO("level ", level);
    tc::round_trip(cz::Method::Brotli, test::random_bytes(50000, 3), level);
    tc::round_trip(cz::Method::Brotli, {}, level);
    tc::round_trip(cz::Method::Brotli, as_bytes("x"), level);
    Bytes runs(100000, std::byte{'a'});
    runs.insert(runs.end(), 50000, std::byte{0});
    tc::round_trip(cz::Method::Brotli, runs, level);
    Bytes all;
    for (int r = 0; r < 64; ++r) {
      for (int b = 0; b < 256; ++b) all.push_back(static_cast<std::byte>((b * 7 + r) & 0xFF));
    }
    tc::round_trip(cz::Method::Brotli, all, level);
    tc::round_trip(cz::Method::Brotli, heterogeneous(), level);
  }
  // The other levels too, on a smaller mixed input.
  const Bytes mixed = heterogeneous();
  for (const int level : {2, 3, 4, 6, 7, 8}) tc::round_trip(cz::Method::Brotli, mixed, level);
}

TEST_CASE("archive/brotli: levels are monotonic and deterministic") {
  const Bytes text = tc::text_corpus(60000);
  const Bytes mixed = heterogeneous();
  for (const Bytes* input : {&text, &mixed}) {
    std::size_t previous = encode(*input, 1).size();
    for (int level = 2; level <= 9; ++level) {
      const std::size_t size = encode(*input, level).size();
      INFO("level ", level);
      CHECK_LE(size, previous + previous / 100);  // never meaningfully larger than the level below
      previous = std::min(previous, size);
    }
  }
  CHECK(encode(mixed, 5) == encode(mixed, 5));
  CHECK(encode(mixed, 9) == encode(mixed, 9));
  // The streaming encoder gives the same bytes however the input is cut.
  for (const std::size_t piece : {std::size_t{1000}, std::size_t{65536}}) {
    cz::EncodeOptions e;
    e.level = 6;
    auto encoder = cz::make_encoder(cz::Method::Brotli, e);
    REQUIRE_OK(encoder);
    Bytes out;
    for (std::size_t at = 0; at < mixed.size(); at += piece) {
      REQUIRE_OK((*encoder)->encode(ByteView(mixed).subspan(at, std::min(piece, mixed.size() - at)), out));
    }
    REQUIRE_OK((*encoder)->finish(out));
    CHECK(out == encode(mixed, 6));
  }
}

TEST_CASE("archive/brotli: window size follows the level, the input and dictionary_size") {
  const Bytes text = tc::text_corpus(100000);
  // WBITS sits in the first bits of the stream: 1 + 3 bits for 18..24.
  const auto wbits = [](const Bytes& s) {
    const unsigned b = std::to_integer<unsigned>(s[0]);
    if ((b & 1u) == 0) return 16u;
    const unsigned n = (b >> 1) & 7u;
    if (n != 0) return 17u + n;
    const unsigned m = (b >> 4) & 7u;
    return m == 0 ? 17u : 8u + m;
  };
  CHECK_EQ(wbits(encode(text, 5)), 17u);  // 100 kB needs no more than 128 KiB
  CHECK_EQ(wbits(encode(tc::text_corpus(5000), 9)), 13u);
  const Bytes small_window = encode(text, 5, 1024);
  CHECK_EQ(wbits(small_window), 11u);
  const auto back = cz::decompress(cz::Method::Brotli, small_window);
  REQUIRE_OK(back);
  CHECK(*back == text);
  // Repetition farther away than the window cannot be used.
  Bytes twice = test::random_bytes(20000, 5);
  append(twice, test::random_bytes(20000, 5));
  CHECK_LT(encode(twice, 5).size(), 21000u);
  CHECK_GT(encode(twice, 5, 8000).size(), 39000u);
}

TEST_CASE("archive/brotli: incremental decoding, prefixes, limits and cancellation") {
  const Bytes mixed = heterogeneous();
  const Bytes packed = encode(mixed, 6);
  for (const std::size_t piece : {std::size_t{1}, std::size_t{7}, std::size_t{4096}, packed.size()}) {
    if (piece == 1 && packed.size() > 20000) continue;
    INFO("piece ", piece);
    auto decoder = cz::make_decoder(cz::Method::Brotli);
    REQUIRE_OK(decoder);
    Bytes out;
    for (std::size_t at = 0; at < packed.size(); at += piece) {
      const std::size_t n = std::min(piece, packed.size() - at);
      const auto step = (*decoder)->decode(ByteView(packed).subspan(at, n), out, at + n == packed.size());
      REQUIRE_OK(step);
    }
    CHECK((*decoder)->finished());
    CHECK(out == mixed);
    CHECK_EQ((*decoder)->total_out(), mixed.size());
  }
  // One-byte pieces through a stream with small meta-blocks.
  {
    const Bytes small = encode(tc::text_corpus(3000), 5);
    auto decoder = cz::make_decoder(cz::Method::Brotli);
    REQUIRE_OK(decoder);
    Bytes out;
    for (std::size_t at = 0; at < small.size(); ++at) {
      REQUIRE_OK((*decoder)->decode(ByteView(small).subspan(at, 1), out, at + 1 == small.size()));
    }
    CHECK(out == tc::text_corpus(3000));
  }
  // Data after the stream is reported as not consumed by the incremental decoder.
  {
    Bytes trailing = packed;
    append(trailing, as_bytes("TRAILER"));
    auto decoder = cz::make_decoder(cz::Method::Brotli);
    REQUIRE_OK(decoder);
    Bytes out;
    const auto step = (*decoder)->decode(trailing, out, true);
    REQUIRE_OK(step);
    CHECK(step->finished);
    CHECK_EQ(step->consumed, packed.size());
    CHECK_STATUS(cz::decompress(cz::Method::Brotli, trailing), Status::Decode);
  }
  // Truncated input fails cleanly.
  {
    auto decoder = cz::make_decoder(cz::Method::Brotli);
    REQUIRE_OK(decoder);
    Bytes out;
    CHECK_STATUS((*decoder)->decode(ByteView(packed).first(packed.size() / 2), out, true), Status::Decode);
  }
  // Prefixes are exact, also in the middle of a copy or a meta-block.
  for (const std::uint64_t n : {std::uint64_t{1}, std::uint64_t{999}, std::uint64_t{40000}, std::uint64_t{70001}}) {
    cz::DecodeOptions d;
    d.stop_after = n;
    const auto head = cz::decompress(cz::Method::Brotli, packed, d);
    REQUIRE_OK(head);
    CHECK(*head == slice(mixed, static_cast<std::size_t>(n)));
    auto decoder = cz::make_decoder(cz::Method::Brotli, d);
    REQUIRE_OK(decoder);
    Bytes out;
    REQUIRE_OK((*decoder)->decode(packed, out, true));
    CHECK(out == slice(mixed, static_cast<std::size_t>(n)));
  }
  {
    cz::DecodeOptions d;
    d.stop_after = 100;
    const auto head = cz::decompress(cz::Method::Brotli, test::fixture("archive/brotli/noise_q5.br"), d);
    REQUIRE_OK(head);
    CHECK(*head == slice(test::tree::noise(100), 100));
  }
  // Limits, sizes and cancellation.
  cz::DecodeOptions small;
  small.max_output = 1000;
  CHECK_STATUS(cz::decompress(cz::Method::Brotli, packed, small), Status::Limit);
  CHECK_STATUS(cz::decompress(cz::Method::Brotli, test::fixture("archive/brotli/noise_q5.br"), small), Status::Limit);
  cz::DecodeOptions sized;
  sized.output_size = mixed.size() + 1;
  CHECK_STATUS(cz::decompress(cz::Method::Brotli, packed, sized), Status::Decode);
  cz::DecodeOptions cancelled;
  Progress p([](double, std::string_view) { return false; });
  p.cancel();
  cancelled.progress = p;
  CHECK_STATUS(cz::decompress(cz::Method::Brotli, packed, cancelled), Status::Cancelled);
  cz::EncodeOptions e;
  e.level = 5;
  e.progress = p;
  CHECK_STATUS(cz::compress(cz::Method::Brotli, mixed, e), Status::Cancelled);
}

TEST_CASE("archive/brotli: a decompression bomb hits the limit") {
  // 4 MB of zeros compress to a few hundred bytes; the limit stops them early.
  const Bytes zeros(4u << 20, std::byte{0});
  const Bytes bomb = encode(zeros, 1);
  CHECK_LT(bomb.size(), 1000u);
  cz::DecodeOptions d;
  d.max_output = 1u << 20;
  CHECK_STATUS(cz::decompress(cz::Method::Brotli, bomb, d), Status::Limit);
}

TEST_CASE("archive/brotli: truncation, bit flips and garbage") {
  cz::DecodeOptions limit;
  limit.max_output = 1u << 17;
  // Every prefix of a small stream is rejected.
  const Bytes small = encode(tc::text_corpus(2000), 9);
  for (std::size_t n = 0; n < small.size(); ++n) {
    INFO("prefix ", n);
    CHECK_STATUS(cz::decompress(cz::Method::Brotli, ByteView(small).first(n)), Status::Decode);
  }
  tc::hostile(cz::Method::Brotli, test::fixture("archive/codecs/mixed_q11.br"), limit);
  tc::hostile(cz::Method::Brotli, test::fixture("archive/brotli/utf8_q11.br"), limit);
  tc::hostile(cz::Method::Brotli, ByteView(test::fixture("archive/brotli/records_q11.br")).first(2000), limit);
  tc::hostile(cz::Method::Brotli, test::fixture("archive/brotli/transforms.br"), limit);
  tc::hostile(cz::Method::Brotli, test::fixture("archive/codecs/prose_q11.br"), limit);
  Bytes part = slice(heterogeneous(), 6000);
  append(part, test::random_bytes(500, 9));
  tc::hostile(cz::Method::Brotli, encode(part, 9), limit);
  tc::hostile(cz::Method::Brotli, encode(part, 5), limit);
  tc::hostile(cz::Method::Brotli, encode(part, 1), limit);
  // Garbage of every first byte.
  for (int b = 0; b < 256; ++b) {
    Bytes g = test::random_bytes(64, static_cast<std::uint32_t>(b) + 100);
    g[0] = static_cast<std::byte>(b);
    (void)cz::decompress(cz::Method::Brotli, g);
  }
}

TEST_CASE("archive/brotli: level 9 uses the static dictionary") {
  const Bytes prose = test::fixture("archive/codecs/prose.txt");
  const Bytes l9 = encode(prose, 9);
  const Bytes l8 = encode(prose, 8);
  CHECK_LT(l9.size(), l8.size());
  const auto back = cz::decompress(cz::Method::Brotli, l9);
  REQUIRE_OK(back);
  CHECK(*back == prose);
  // Words that occur once each: only the dictionary can shorten them.
  const Bytes words = to_bytes(
      "Information about international development, available from the government. Management of the university: "
      "description, performance and environment. Technology makes a different, important community experience.");
  const Bytes w9 = encode(words, 9);
  INFO("level 9 ", w9.size(), " level 8 ", encode(words, 8).size());
  CHECK_LT(w9.size() * 4, encode(words, 8).size() * 3);
  const auto words_back = cz::decompress(cz::Method::Brotli, w9);
  REQUIRE_OK(words_back);
  CHECK(*words_back == words);
}
