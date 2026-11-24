// Tests of the further binary-to-text encodings (more_bases.cpp, base62 in
// base_n.cpp): published vectors, fixtures written by independent
// implementations (see tests/data/encoding/bases/SOURCES.md), round trips,
// options and damaged input.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/encoding/bases.hpp"
#include "families/encoding/encoding.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace std::string_view_literals;
using test::convert;

namespace {

Bytes sample() { return test::fixture("encoding/bases/sample.bin"); }
Bytes sample_prefix(std::size_t n) {
  const Bytes all = sample();
  return Bytes(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n));
}

std::string text_of(std::string_view name) { return test::fixture_text("encoding/bases/" + std::string(name)); }

Bytes hex(std::string_view digits) {
  auto decoded = encoding::parse_hex_text(digits);
  REQUIRE_OK(decoded);
  return *decoded;
}

void check_round_trip(std::string_view format, ByteView data, std::string_view options = {}) {
  INFO(format);
  const test::Converted encoded = convert(data, "bin", format, options, "data.bin");
  REQUIRE_MESSAGE(encoded.ok(), encoded.message);
  const test::Converted decoded = convert(encoded.data, format, "bin");
  REQUIRE_MESSAGE(decoded.ok(), decoded.message);
  CHECK(decoded.data == Bytes(data.begin(), data.end()));
}

std::string detected(ByteView data, std::string_view filename) {
  const auto detection = detect_format(Registry::instance(), data, filename);
  return detection ? std::string(detection->format->id) : std::string("none");
}

}  // namespace

TEST_CASE("encoding/base16 and encoding/base45: RFC vectors") {
  CHECK(encoding::base16_encode(as_bytes("foobar"sv)) == "666F6F626172");
  CHECK(test::str(*encoding::base16_decode("666f6f\n626172")) == "foobar");
  CHECK_STATUS(encoding::base16_decode("6"), Status::Decode);
  CHECK_STATUS(encoding::base16_decode("zz"), Status::Decode);
  // RFC 9285 section 4.
  CHECK(encoding::base45_encode(as_bytes("AB"sv)) == "BB8");
  CHECK(encoding::base45_encode(as_bytes("Hello!!"sv)) == "%69 VD92EX0");
  CHECK(encoding::base45_encode(as_bytes("base-45"sv)) == "UJCLQE7W581");
  CHECK(test::str(*encoding::base45_decode("QED8WEX0")) == "ietf!");
  CHECK_STATUS(encoding::base45_decode("GGW"), Status::Decode);  // 65536 does not fit two bytes
  CHECK_STATUS(encoding::base45_decode("A"), Status::Decode);
  CHECK_STATUS(encoding::base45_decode("ab"), Status::Decode);  // lower case is not in the alphabet
  // Fixture written by the Python `base45` package.
  CHECK(encoding::base45_encode(sample()) == text_of("sample.b45"));
  CHECK(convert(text_of("sample.b45"), "base45", "bin").data == sample());
}

TEST_CASE("encoding/base62, base91, base92, base122: vectors and round trips") {
  CHECK(*encoding::base62_encode(as_bytes("Hello"sv)) == "5TP3P3v");
  CHECK(*encoding::base62_encode(as_bytes("\0\0\x01"sv)) == "001");
  CHECK(test::str(*encoding::base62_decode("5TP3P3v")) == "Hello");
  CHECK(encoding::base91_encode(as_bytes("test"sv)) == "fPNKd");
  CHECK(encoding::base91_encode(sample()) == text_of("sample.b91"));  // Python `base91` package
  CHECK(encoding::base91_decode(text_of("sample.b91")) == sample());
  CHECK(encoding::base92_encode({}) == "~");
  CHECK(encoding::base92_encode(as_bytes("a"sv)) == "D,");
  CHECK(encoding::base92_decode("~")->empty());
  CHECK_STATUS(encoding::base92_decode("\x7f\x7f"), Status::Decode);
  CHECK(encoding::base122_encode(as_bytes("\0"sv)) == "\xC2\x80");
  CHECK(encoding::base122_decode("\xC2\x80").value() == Bytes{std::byte{0}});
  CHECK_STATUS(encoding::base122_decode("\xFF"), Status::Decode);
  const Bytes all = test::all_byte_values();
  const Bytes random = test::random_bytes(1000, 21);
  for (const std::string_view format : {"base16", "base45", "base62", "base64-mime", "pem", "base91", "base92",
                                        "base122", "bech32", "base58check", "xxe", "hqx"}) {
    check_round_trip(format, all);
    check_round_trip(format, random);
    check_round_trip(format, ByteView{});
    for (std::size_t n = 1; n < 20; ++n) check_round_trip(format, ByteView(random).first(n));
  }
  check_round_trip("bech32", random, R"({"variant":"bech32m","hrp":"tb"})");
}

TEST_CASE("encoding/bech32: BIP 173 and BIP 350 test vectors") {
  const auto empty = encoding::bech32_decode("A12UEL5L");
  REQUIRE_OK(empty);
  CHECK(empty->hrp == "a");
  CHECK(empty->data.empty());
  CHECK(empty->variant == encoding::Bech32Variant::Bech32);
  const auto full = encoding::bech32_decode("abcdef1qpzry9x8gf2tvdw0s3jn54khce6mua7lmqqqxw");
  REQUIRE_OK(full);
  CHECK(full->data == hex("00443214c74254b635cf84653a56d7c675be77df"));
  const auto m = encoding::bech32_decode("abcdef1l7aum6echk45nj3s0wdvt2fg8x9yrzpqzd3ryx");
  REQUIRE_OK(m);
  CHECK(m->variant == encoding::Bech32Variant::Bech32m);
  CHECK(m->data == hex("ffbbcdeb38bdab49ca307b9ac5a928398a418820"));
  CHECK(encoding::bech32_decode("a1lqfn3a")->variant == encoding::Bech32Variant::Bech32m);
  CHECK(*encoding::bech32_encode("abcdef", hex("00443214c74254b635cf84653a56d7c675be77df"),
                                 encoding::Bech32Variant::Bech32) == "abcdef1qpzry9x8gf2tvdw0s3jn54khce6mua7lmqqqxw");
  CHECK_STATUS(encoding::bech32_decode("abcdef1qpzry9x8gf2tvdw0s3jn54khce6mua7lmqqqxx"), Status::Decode);
  CHECK_STATUS(encoding::bech32_decode("A1g7sgd8"), Status::Decode);  // mixed case
  CHECK_STATUS(encoding::bech32_decode("1qzzfhee"), Status::Decode);  // empty prefix
  // Written by the `bech32` reference package with the prefix "test".
  const Bytes first32 = sample_prefix(32);
  CHECK(*encoding::bech32_encode("test", first32, encoding::Bech32Variant::Bech32) == text_of("sample32.bech32"));
  CHECK(convert(text_of("sample32.bech32"), "bech32", "bin").data == first32);
  CHECK(convert("abc", "bin", "bech32", R"({"hrp":"Bad"})").status == Status::InvalidArgument);
}

TEST_CASE("encoding/base58check, base64-mime, pem: independent fixtures") {
  const Bytes first32 = sample_prefix(32);
  CHECK(*encoding::base58check_encode(first32) == text_of("sample32.b58c"));  // Python `base58`
  CHECK(convert(text_of("sample32.b58c"), "base58check", "bin").data == first32);
  std::string corrupted = text_of("sample32.b58c");
  corrupted[5] = corrupted[5] == 'a' ? 'b' : 'a';
  CHECK(convert(corrupted, "base58check", "bin").status == Status::Decode);
  // email.base64mime.body_encode(maxlinelen=76, eol="\r\n")
  CHECK(convert(sample(), "bin", "base64-mime").text() == text_of("sample.mime.b64"));
  // ssl.DER_cert_to_PEM_cert
  CHECK(convert(sample(), "bin", "pem", R"({"label":"CERTIFICATE"})").text() == text_of("sample.pem"));
  CHECK(convert(text_of("sample.pem"), "pem", "bin").data == sample());
  CHECK(detected(as_bytes(text_of("sample.pem")), "cert.pem") == "pem");
  CHECK(convert("x", "bin", "pem", R"({"label":"BAD-LABEL"})").status == Status::InvalidArgument);
  const auto headers = encoding::pem_decode("-----BEGIN X-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: A\n\nZm9v\n-----END X-----\n");
  REQUIRE_OK(headers);
  CHECK(test::str(headers->data) == "foo");
  CHECK_STATUS(encoding::pem_decode("-----BEGIN X-----\nZm9v\n"), Status::Decode);
}

TEST_CASE("encoding/xxe and encoding/hqx: files written by uudeview and macutils") {
  const auto xx = encoding::xxdecode(text_of("sample.xxe"));
  REQUIRE_OK(xx);
  CHECK(xx->bytes == sample());
  CHECK(xx->filename == "sample.bin");
  // uuenview writes the same lines as we do.
  const std::string ours = encoding::xxencode(sample(), "sample.bin");
  CHECK(util::split_lines(text_of("sample.xxe"))[1] == util::split_lines(ours)[1]);
  CHECK(detected(as_bytes(ours), "") == "xxe");

  const auto hqx = encoding::binhex_decode(text_of("sample.hqx"), 1 << 20);
  REQUIRE_OK(hqx);
  CHECK(hqx->data == sample());
  CHECK(hqx->filename == "sample.bin");
  CHECK(hqx->type == "BINA");
  CHECK(hqx->creator == "CVRT");
  CHECK(detected(as_bytes(text_of("sample.hqx")), "x.hqx") == "hqx");
  const test::Converted via_engine = convert(text_of("sample.hqx"), "", "bin", {}, "sample.hqx");
  CHECK(via_engine.data == sample());
  // A flipped character breaks a CRC.
  std::string damaged = text_of("sample.hqx");
  const std::size_t at = damaged.find(':') + 40;
  damaged[at] = damaged[at] == 'A' ? 'B' : 'A';
  CHECK(convert(damaged, "hqx", "bin").status == Status::Decode);
  // Runs are compressed, 0x90 bytes survive.
  Bytes runs(1000, std::byte{0x90});
  runs.insert(runs.end(), 500, std::byte{7});
  check_round_trip("hqx", runs);
  CHECK(encoding::binhex_encode(runs, "r").size() < 200);
  CHECK(test::fuzz_reader<ir::Blob>("hqx", as_bytes(encoding::binhex_encode(test::random_bytes(64, 3), "f"))) > 0);
  CHECK(test::fuzz_reader<ir::Blob>("xxe", as_bytes(ours)) > 0);
}

TEST_CASE("encoding/mime-words: RFC 2047 against Python's email.header") {
  const std::string text = text_of("words_text.txt");
  CHECK(*encoding::mime_words_decode(text_of("words_b.txt")) == text);
  CHECK(*encoding::mime_words_decode(text_of("words_q.txt")) == text);
  CHECK(*encoding::mime_words_decode(text_of("words_mixed.txt")) == text_of("words_mixed_decoded.txt"));
  for (const bool q : {false, true}) {
    const std::string encoded = encoding::mime_words_encode(text, q);
    CHECK(*encoding::mime_words_decode(encoded) == text);
    for (const std::string_view line : util::split_lines(encoded)) CHECK(util::trim(line).size() <= 75);
  }
  CHECK(convert(text, "txt", "mime-words", R"({"encoding":"q"})").text().starts_with("=?UTF-8?Q?Gr=C3=BC"));
  CHECK(convert("=?UTF-8?B?w6l0w6k=?=", "mime-words", "txt").text() == "été");
  CHECK(convert("=?KOI9?B?w6k=?=", "mime-words", "txt").status == Status::Decode);
  CHECK(*encoding::mime_words_decode("no words here") == "no words here");
  CHECK(*encoding::mime_words_decode("=?broken") == "=?broken");
}
