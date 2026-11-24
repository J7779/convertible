// Tests for Base32 (three alphabets), Base58, Base36, Ascii85, Z85 and gzip+Base64.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/encoding/encoding.hpp"
#include "support/test_support.hpp"
#include "util/deflate.hpp"

using namespace cvt;
using namespace cvt::encoding;
using namespace std::string_view_literals;
using test::convert;
using test::str;

namespace {

struct Vector {
  std::string_view plain;
  std::string_view encoded;
};

constexpr std::string_view leviathan =
    "Man is distinguished, not only by his reason, but by this singular passion from other animals, which is a "
    "lust of the mind, that by a perseverance of delight in the continued and indefatigable generation of "
    "knowledge, exceeds the short vehemence of any carnal pleasure.";

constexpr std::string_view leviathan_ascii85 =
    "<~9jqo^BlbD-BleB1DJ+*+F(f,q/0JhKF<GL>Cj@.4Gp$d7F!,L7@<6@)/0JDEF<G%<+EV:2F!,O<DJ+*.@<*K0@<6L(Df-\\0Ec5e;DffZ(EZee."
    "Bl.9pF\"AGXBPCsi+DGm>@3BB/F*&OCAfu2/AKYi(DIb:@FD,*)+C]U=@3BN#EcYf8ATD3s@q?d$AftVqCh[NqF<G:8+EV:.+Cf>-FD5W8ARlolDIal("
    "DId<j@<?3r@:F%a+D58'ATD4$Bl@l3De:,-DJs`8ARoFb/0JMK@qB4^F!,R<AKZ&-DfTqBG%G>uD.RTpAKYo'+CT/5+Cei#DII?(E,9)oF*2M7/c~>";

Bytes zeros(std::size_t count) { return Bytes(count, std::byte{0}); }

template <class T>
Status status_of(const Result<T>& result) { return result ? Status::Ok : result.error().code; }

}  // namespace

TEST_CASE("encoding: base32 RFC 4648 test vectors") {
  const Vector standard[] = {{"", ""},
                             {"f", "MY======"},
                             {"fo", "MZXQ===="},
                             {"foo", "MZXW6==="},
                             {"foob", "MZXW6YQ="},
                             {"fooba", "MZXW6YTB"},
                             {"foobar", "MZXW6YTBOI======"}};
  const Vector hex[] = {{"", ""},
                        {"f", "CO======"},
                        {"fo", "CPNG===="},
                        {"foo", "CPNMU==="},
                        {"foob", "CPNMUOG="},
                        {"fooba", "CPNMUOJ1"},
                        {"foobar", "CPNMUOJ1E8======"}};
  for (const Vector& v : standard) {
    CHECK(base32_encode(as_bytes(v.plain)) == v.encoded);
    const auto decoded = base32_decode(v.encoded);
    REQUIRE(decoded);
    CHECK(str(*decoded) == v.plain);
  }
  for (const Vector& v : hex) {
    CHECK(base32_encode(as_bytes(v.plain), {.alphabet = Base32Alphabet::Hex}) == v.encoded);
    const auto decoded = base32_decode(v.encoded, Base32Alphabet::Hex);
    REQUIRE(decoded);
    CHECK(str(*decoded) == v.plain);
  }
}

TEST_CASE("encoding: base32 padding, wrapping and lenient decoding") {
  CHECK(base32_encode(as_bytes("foob"), {.padding = false}) == "MZXW6YQ");
  CHECK(base32_encode(as_bytes("foobar"), {.line_width = 8}) == "MZXW6YTB\nOI======\n");
  CHECK(base32_encode(as_bytes("foobar"), {.padding = false, .line_width = 4}) == "MZXW\n6YTB\nOI\n");
  CHECK(base32_encode({}, {.line_width = 8}).empty());

  CHECK(str(*base32_decode("mzxw6ytboi")) == "foobar");            // lower case, no padding
  CHECK(str(*base32_decode(" MZXW\r\n6YTB OI==\n====\n")) == "foobar");  // whitespace anywhere
  CHECK(str(*base32_decode("cpnmuoj1e8", Base32Alphabet::Hex)) == "foobar");

  for (const std::string_view bad : {"M", "MZX", "MZXW6Y", "MY=", "MY=====", "MY======MY======", "MZXW6YT!", "MZXW1YTB",
                                     "MZXW6YT\xC3\xA9", "=MY"}) {
    CHECK_MESSAGE(status_of(base32_decode(bad)) == Status::Decode, "should reject: ", bad);
  }
  // 'W' and '8' belong to one alphabet each.
  CHECK(status_of(base32_decode("CPNMUOJ8", Base32Alphabet::Standard)) == Status::Decode);
  CHECK(status_of(base32_decode("CPNMUOJW", Base32Alphabet::Hex)) == Status::Decode);
}

TEST_CASE("encoding: base32 Crockford alphabet") {
  const Vector vectors[] = {{"", ""},        {"f", "CR"},          {"fo", "CSQG"},           {"foo", "CSQPY"},
                            {"foob", "CSQPYRG"}, {"fooba", "CSQPYRK1"}, {"foobar", "CSQPYRK1E8"},
                            {"Hello, World!", "91JPRV3F5GG5EVVJDHJ22"}};
  for (const Vector& v : vectors) {
    CHECK(base32_encode(as_bytes(v.plain), {.alphabet = Base32Alphabet::Crockford}) == v.encoded);
    const auto decoded = base32_decode(v.encoded, Base32Alphabet::Crockford);
    REQUIRE(decoded);
    CHECK(str(*decoded) == v.plain);
  }
  // Padding is never written, whatever the style says.
  CHECK(base32_encode(as_bytes("f"), {.alphabet = Base32Alphabet::Crockford, .padding = true}) == "CR");
  // Case-insensitive; O reads as 0, I and L as 1; hyphens group digits.
  CHECK(str(*base32_decode("csqpyrk1e8", Base32Alphabet::Crockford)) == "foobar");
  CHECK(str(*base32_decode("CSQPY-RKIE8", Base32Alphabet::Crockford)) == "foobar");
  CHECK(str(*base32_decode("csqpyrkle8", Base32Alphabet::Crockford)) == "foobar");
  CHECK(*base32_decode("OoIiLl10", Base32Alphabet::Crockford) == *base32_decode("00111110", Base32Alphabet::Crockford));
  CHECK(status_of(base32_decode("CSQPYRKU", Base32Alphabet::Crockford)) == Status::Decode);  // U is not a digit
  CHECK(status_of(base32_decode("CR======", Base32Alphabet::Crockford)) == Status::Decode);  // nor is '='
}

TEST_CASE("encoding: base58 vectors and leading zeros") {
  const Vector vectors[] = {
      {"Hello World!", "2NEpo7TZRRrLZSi2U"},
      {"", ""},
      {"abc", "ZiCa"},
      {"The quick brown fox jumps over the lazy dog.", "USm3fpXnKG5EUBx2ndxBDMPVciP5hGey2Jh4NDv6gmeo1LkMeiKrLJUUBk6Z"},
      {"\x00"sv, "1"},
      {"\x00\x00\x01"sv, "112"},
      {"\x00\x00\x28\x7f\xb4\xcd"sv, "11233QC4"},
      {"\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", "FPBt6CHo3fovdL"},
  };
  for (const Vector& v : vectors) {
    const auto encoded = base58_encode(as_bytes(v.plain));
    REQUIRE(encoded);
    CHECK(*encoded == v.encoded);
    const auto decoded = base58_decode(v.encoded);
    REQUIRE(decoded);
    CHECK(str(*decoded) == v.plain);
  }
  CHECK(*base58_encode(zeros(5)) == "11111");
  CHECK(*base58_decode("11111") == zeros(5));
  CHECK(str(*base58_decode(" 2NEpo7TZ\nRRrLZSi2U\n")) == "Hello World!");
  for (const std::string_view bad : {"0", "O", "I", "l", "2NEpo7TZRRrLZSi2U!", "Zi Ca+", "\xC3\xA9"}) {
    CHECK_MESSAGE(status_of(base58_decode(bad)) == Status::Decode, "should reject: ", bad);
  }
}

TEST_CASE("encoding: base36 vectors and leading zeros") {
  const Vector vectors[] = {
      {"Hello World!", "2678lx5gvmsv1dro9b5"},
      {"", ""},
      {"abc", "3ssir"},
      {"The quick brown fox jumps over the lazy dog.",
       "g5qzbp4tj09hv0mjegv4p824avisplw3iy3ag5m01ibnboqsbyev6zendz44cetkbrce"},
      {"\x00"sv, "0"},
      {"\x00\x00\x01"sv, "001"},
      {"\x00\x00\x28\x7f\xb4\xcd"sv, "00b8j559"},
      {"\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", "5gv2rma270x9hhj3"},
  };
  for (const Vector& v : vectors) {
    const auto encoded = base36_encode(as_bytes(v.plain));
    REQUIRE(encoded);
    CHECK(*encoded == v.encoded);
    const auto decoded = base36_decode(v.encoded);
    REQUIRE(decoded);
    CHECK(str(*decoded) == v.plain);
  }
  CHECK(*base36_encode(as_bytes("abc"), true) == "3SSIR");
  CHECK(str(*base36_decode("3SsIr")) == "abc");  // either case
  CHECK(str(*base36_decode("3ss\nir\n")) == "abc");
  for (const std::string_view bad : {"3ss-ir", "abc!", "_", "\xC3\xA9"}) {
    CHECK_MESSAGE(status_of(base36_decode(bad)) == Status::Decode, "should reject: ", bad);
  }
}

TEST_CASE("encoding: base58 and base36 refuse more than 64 KiB") {
  // Exactly at the limit still works, also for the largest possible number.
  const Bytes largest(big_number_max_bytes, std::byte{0xFF});
  const auto text58 = base58_encode(largest);
  REQUIRE(text58);
  CHECK(text58->size() == 89'500);
  CHECK(*base58_decode(*text58) == largest);
  const auto text36 = base36_encode(largest);
  REQUIRE(text36);
  CHECK(text36->size() == 101'412);
  CHECK(*base36_decode(*text36) == largest);

  const Bytes too_large = test::random_bytes(big_number_max_bytes + 1, 5);
  CHECK(status_of(base58_encode(too_large)) == Status::Limit);
  CHECK(status_of(base36_encode(too_large)) == Status::Limit);
  CHECK(base58_encode(too_large).error().message.find("65536 bytes") != std::string::npos);

  CHECK(status_of(base58_decode(*text58 + "2")) == Status::Limit);
  CHECK(status_of(base36_decode(*text36 + "1")) == Status::Limit);
  CHECK(status_of(base58_decode(std::string(200'000, '1'))) == Status::Limit);
  // Whitespace does not count towards the limit.
  CHECK(*base58_decode(*text58 + std::string(1000, '\n')) == largest);
}

TEST_CASE("encoding: ascii85 Adobe vectors") {
  CHECK(ascii85_encode(as_bytes(leviathan)) == leviathan_ascii85);
  const auto decoded = ascii85_decode(leviathan_ascii85);
  REQUIRE(decoded);
  CHECK(str(*decoded) == leviathan);

  const Vector vectors[] = {
      {"", "<~~>"},
      {"a", "<~@/~>"},
      {"ab", "<~@:B~>"},
      {"abc", "<~@:E^~>"},
      {"abcd", "<~@:E_W~>"},
      {"\x00\x00\x00\x00"sv, "<~z~>"},
      {"\x00\x00\x00\x00\x00"sv, "<~z!!~>"},
      {"\x00\x00\x00"sv, "<~!!!!~>"},  // 'z' only stands for a complete group
      {"\xff\xff\xff\xff", "<~s8W-!~>"},
      {"hello world", "<~BOu!rD]j7BEbo7~>"},
  };
  for (const Vector& v : vectors) {
    CHECK(ascii85_encode(as_bytes(v.plain)) == v.encoded);
    const auto back = ascii85_decode(v.encoded);
    REQUIRE(back);
    CHECK(str(*back) == v.plain);
  }
}

TEST_CASE("encoding: ascii85 wrapper, line width and decoder tolerance") {
  CHECK(ascii85_encode(as_bytes("hello world"), {.wrapper = false}) == "BOu!rD]j7BEbo7");
  CHECK(ascii85_encode({}, {.wrapper = false}).empty());
  CHECK(ascii85_encode(as_bytes("hello world"), {.line_width = 8}) == "<~BOu!rD\n]j7BEbo7\n~>\n");
  CHECK(ascii85_encode(as_bytes("hello world"), {.wrapper = false, .line_width = 5}) == "BOu!r\nD]j7B\nEbo7\n");

  // Bare data, wrapped data, an end marker alone and whitespace are all fine.
  CHECK(str(*ascii85_decode("BOu!rD]j7BEbo7")) == "hello world");
  CHECK(str(*ascii85_decode("BOu!rD]j7BEbo7~>")) == "hello world");
  CHECK(str(*ascii85_decode("  <~BOu!r\r\n\tD]j7B Ebo7\n~>\n")) == "hello world");
  CHECK(str(*ascii85_decode(ascii85_encode(as_bytes(leviathan), {.line_width = 40}))) == leviathan);
  CHECK(ascii85_decode("")->empty());
  CHECK(ascii85_decode("<~~>")->empty());

  for (const std::string_view bad : {
           "<~BOu!rD]j7BEbo7",    // wrapper never closed
           "<~BOu!rD]j7BEbo7~",   // lone '~'
           "<~BOu!r~>D]j7B",      // data after the end marker
           "<~BOu!rD]j7BEbo7BB~>", // one character left over
           "<~BOuz!r~>",          // z inside a group
           "<~s8W-\"~>",          // group above 2^32 - 1
           "<~BOu!v~>",           // 'v' is outside the alphabet
           "<~uuuu~>",            // partial group that overflows when padded
           "BOu!r\xC3\xA9",
       }) {
    CHECK_MESSAGE(status_of(ascii85_decode(bad)) == Status::Decode, "should reject: ", bad);
  }
}

TEST_CASE("encoding: z85 specification vector and length rules") {
  const Bytes spec = {std::byte{0x86}, std::byte{0x4F}, std::byte{0xD2}, std::byte{0x6F},
                      std::byte{0xB5}, std::byte{0x59}, std::byte{0xF7}, std::byte{0x5B}};
  CHECK(*z85_encode(spec) == "HelloWorld");
  CHECK(*z85_decode("HelloWorld") == spec);
  CHECK(*z85_decode(" Hello\nWorld\n") == spec);
  CHECK(z85_encode({})->empty());
  CHECK(z85_decode("")->empty());
  CHECK(*z85_encode(zeros(4)) == "00000");
  CHECK(*z85_encode(Bytes(4, std::byte{0xFF})) == "%nSc0");
  CHECK(*z85_decode("%nSc0") == Bytes(4, std::byte{0xFF}));

  for (const std::size_t size : {1u, 2u, 3u, 5u, 7u}) {
    const auto encoded = z85_encode(test::random_bytes(size));
    REQUIRE_FALSE(encoded);
    CHECK(encoded.error().code == Status::Encode);
    CHECK(encoded.error().message.find("multiple of 4") != std::string::npos);
  }
  CHECK(z85_decode("Hello"));  // five characters are one complete group
  for (const std::string_view bad : {"Hell", "HelloWorl", "HelloWorld!", "Hello World~", "Hell\"", "#####", "%nSc1"}) {
    CHECK_MESSAGE(status_of(z85_decode(bad)) == Status::Decode, "should reject: ", bad);
  }
}

TEST_CASE("encoding: gzip-base64 compresses and restores") {
  // A stream written by another gzip implementation.
  const auto reference = gzip_base64_decode("H4sIAAAAAAAC/8tIzcnJV8hAJwHjUT2NFwAAAA==", 1024);
  REQUIRE(reference);
  CHECK(str(*reference) == "hello hello hello hello");

  const Bytes repetitive = test::bytes(std::string(10'000, 'a'));
  const auto text = gzip_base64_encode(repetitive, 9, 0);
  REQUIRE(text);
  CHECK(text->starts_with("H4sI"));  // Base64 of the gzip magic 1f 8b 08
  CHECK(text->size() < 200);
  CHECK(text->find('\n') == std::string::npos);
  CHECK(*gzip_base64_decode(*text, 1 << 20) == repetitive);

  const auto wrapped = gzip_base64_encode(test::random_bytes(300, 4), 6, 76);
  REQUIRE(wrapped);
  CHECK(wrapped->find('\n') == 76);
  CHECK(wrapped->back() == '\n');
  CHECK(*gzip_base64_decode(*wrapped, 1 << 20) == test::random_bytes(300, 4));

  // The decompression limit stops bombs.
  CHECK(status_of(gzip_base64_decode(*text, 9'999)) == Status::Limit);
  CHECK(status_of(gzip_base64_decode("", 1024)) == Status::Decode);
  CHECK(status_of(gzip_base64_decode("aGVsbG8=", 1024)) == Status::Decode);    // Base64, but not gzip
  CHECK(status_of(gzip_base64_decode("H4sI!!!!", 1024)) == Status::Decode);    // not Base64
  CHECK(status_of(gzip_base64_decode(text->substr(0, text->size() / 2), 1 << 20)) == Status::Decode);

  // Through the engine: options and limits.
  const test::Converted stored = convert(repetitive, "bin", "gzip-base64", R"({"compression_level":0,"line_width":0})");
  const test::Converted best = convert(repetitive, "bin", "gzip-base64", R"({"compression_level":9,"line_width":0})");
  REQUIRE(stored.ok());
  REQUIRE(best.ok());
  CHECK(stored.data.size() > 10'000);
  CHECK(best.data.size() < 200);
  CHECK(convert(best.data, "gzip-base64", "bin").data == repetitive);
  Limits tight;
  tight.max_decompressed_bytes = 5'000;
  CHECK(convert(best.data, "gzip-base64", "bin", {}, {}, tight).status == Status::Limit);
  const test::Converted default_width = convert(test::random_bytes(500, 2), "bin", "gzip-base64");
  REQUIRE(default_width.ok());
  CHECK(default_width.text().find('\n') == 76);
}

TEST_CASE("encoding: base-N option handling through the engine") {
  CHECK(convert("foob", "bin", "base32").text() == "MZXW6YQ=");
  CHECK(convert("foob", "bin", "base32", R"({"padding":false})").text() == "MZXW6YQ");
  CHECK(convert("foobar", "bin", "base32", R"({"line_width":8})").text() == "MZXW6YTB\nOI======\n");
  CHECK(convert("foob", "bin", "base32hex", R"({"padding":false})").text() == "CPNMUOG");
  CHECK(convert("foobar", "bin", "base32-crockford").text() == "CSQPYRK1E8");
  CHECK(convert("abc", "bin", "base36").text() == "3ssir");
  CHECK(convert("abc", "bin", "base36", R"({"uppercase":true})").text() == "3SSIR");
  CHECK(convert("hello world", "bin", "ascii85").text() == "<~BOu!rD]j7BEbo7~>");
  CHECK(convert("hello world", "bin", "ascii85", R"({"wrapper":false})").text() == "BOu!rD]j7BEbo7");
  CHECK(convert("hello world", "bin", "ascii85", R"({"line_width":8})").text() == "<~BOu!rD\n]j7BEbo7\n~>\n");
  // Out-of-range numbers are clamped by the engine, wrong types are refused.
  CHECK(convert("foobar", "bin", "base32", R"({"line_width":-5})").text() == "MZXW6YTBOI======");
  CHECK(convert("foobar", "bin", "base32", R"({"line_width":"wide"})").status == Status::InvalidArgument);

  const test::Converted z85 = convert("abc", "bin", "z85");
  CHECK(z85.status == Status::Encode);
  CHECK(z85.message.find("multiple of 4") != std::string::npos);
  CHECK(convert("abcd", "bin", "z85").ok());

  const test::Converted result = convert("Hello World!", "bin", "base58");
  REQUIRE(result.ok());
  CHECK(result.text() == "2NEpo7TZRRrLZSi2U");
  CHECK(result.extension == "b58");
  CHECK(result.mime == "text/plain");
}
