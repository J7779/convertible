// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "support/test_support.hpp"
#include "util/base64.hpp"
#include "util/hex.hpp"

using namespace cvt;
using namespace cvt::util;
using test::bytes;
using test::str;

TEST_CASE("base64: RFC 4648 test vectors") {
  const std::pair<std::string_view, std::string_view> vectors[] = {
      {"", ""},         {"f", "Zg=="},         {"fo", "Zm8="},         {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
  };
  for (const auto& [plain, encoded] : vectors) {
    CHECK(base64_encode(as_bytes(plain)) == encoded);
    const auto decoded = base64_decode(encoded);
    REQUIRE(decoded);
    CHECK(str(*decoded) == plain);
  }
}

TEST_CASE("base64: alphabets, padding and wrapping") {
  const Bytes data = {std::byte{0xFB}, std::byte{0xFF}, std::byte{0xFE}};
  CHECK(base64_encode(data) == "+//+");
  CHECK(base64_encode(data, {.url_safe = true}) == "-__-");
  CHECK(base64_encode(as_bytes("f"), {.padding = false}) == "Zg");
  CHECK(base64_encode(as_bytes("fo"), {.url_safe = true, .padding = false}) == "Zm8");

  const std::string wrapped = base64_encode(as_bytes("foobarfoobar"), {.line_width = 4});
  CHECK(wrapped == "Zm9v\nYmFy\nZm9v\nYmFy");
  CHECK(base64_encode(as_bytes("foobar"), {.line_width = 5, .newline = "\r\n"}) == "Zm9vY\r\nmFy");
  CHECK(base64_encode(as_bytes("f"), {.line_width = 3}) == "Zg=\n=");
}

TEST_CASE("base64: decoding is lenient about layout and strict about content") {
  CHECK(str(*base64_decode(" Zm9v\r\nYmFy \n")) == "foobar");
  CHECK(str(*base64_decode("Zg")) == "f");       // padding optional
  CHECK(str(*base64_decode("Zm8")) == "fo");
  CHECK(str(*base64_decode("Zg==\n")) == "f");
  CHECK(*base64_decode("-__-") == *base64_decode("+//+"));  // both alphabets
  CHECK(base64_decode("")->empty());

  for (const std::string_view bad : {"Zm9v!", "Z", "Zm9vY", "Zg=", "Zg===", "Zm9v=", "Zg==Zg==", "=Zm9v", "Zm9\x80"}) {
    const auto result = base64_decode(bad);
    CHECK_MESSAGE(!result, "should reject: ", bad);
    if (!result) CHECK(result.error().code == Status::Decode);
  }
}

TEST_CASE("base64: round trip of arbitrary data at every length") {
  for (std::size_t length = 0; length < 70; ++length) {
    const Bytes data = test::random_bytes(length, static_cast<std::uint32_t>(length) + 1);
    for (const bool url_safe : {false, true}) {
      for (const bool padding : {false, true}) {
        const auto decoded = base64_decode(base64_encode(data, {.url_safe = url_safe, .padding = padding, .line_width = 20}));
        REQUIRE(decoded);
        CHECK(*decoded == data);
      }
    }
  }
  const Bytes all = test::all_byte_values();
  CHECK(*base64_decode(base64_encode(all)) == all);
}

TEST_CASE("hex: encode and decode") {
  static_assert(hex_digit_value('a') == 10 && hex_digit_value('F') == 15 && hex_digit_value('g') == -1);
  CHECK(hex_encode(bytes("\x01\xAB\xFF")) == "01abff");
  CHECK(hex_encode(bytes("\x01\xAB\xFF"), true) == "01ABFF");
  CHECK(hex_encode({}).empty());
  CHECK(str(*hex_decode("48656c6C6F")) == "Hello");
  CHECK(str(*hex_decode(" 48 65\n6c\t6c 6f ")) == "Hello");
  CHECK(hex_decode("")->empty());
  CHECK(hex_decode("abc").error().code == Status::Decode);  // odd digit count
  CHECK(hex_decode("zz").error().code == Status::Decode);
  CHECK(hex_decode("0x41").error().code == Status::Decode);
  const Bytes all = test::all_byte_values();
  CHECK(*hex_decode(hex_encode(all)) == all);
  CHECK(*hex_decode(hex_encode(all, true)) == all);
}
