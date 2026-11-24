// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <limits>

#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace cvt::util;
using namespace std::string_view_literals;

namespace {
Bytes raw(std::initializer_list<int> values) {
  Bytes out;
  for (const int v : values) out.push_back(static_cast<std::byte>(v));
  return out;
}
}  // namespace

TEST_CASE("text: ASCII helpers") {
  static_assert(trim("  a b \t\r\n") == "a b");
  static_assert(trim_left("  a ") == "a ");
  static_assert(trim_right("  a ") == "  a");
  static_assert(trim("   ").empty());
  static_assert(iequals("Hello", "hELLO"));
  static_assert(!iequals("Hello", "Hell"));
  static_assert(istarts_with("Content-Type", "content-"));
  static_assert(iends_with("photo.JPG", ".jpg"));
  static_assert(!iends_with("jpg", "photo.jpg"));
  CHECK(to_lower("ÄbC-Z") == "Äbc-z");  // only ASCII letters change
  CHECK(to_upper("abc-z9") == "ABC-Z9");
  CHECK(replace_all("a-b-c", "-", "--") == "a--b--c");
  CHECK(replace_all("aaa", "aa", "b") == "ba");
  CHECK(replace_all("abc", "", "x") == "abc");
  CHECK(replace_all("", "a", "b").empty());
}

TEST_CASE("text: split and join") {
  CHECK(split("a,,b", ',') == std::vector<std::string_view>{"a", "", "b"});
  CHECK(split("a,,b", ',', false) == std::vector<std::string_view>{"a", "b"});
  CHECK(split("", ',') == std::vector<std::string_view>{""});
  CHECK(split("", ',', false).empty());
  CHECK(split(",", ',') == std::vector<std::string_view>{"", ""});
  CHECK(split_lines("a\nb\r\nc\rd") == std::vector<std::string_view>{"a", "b", "c", "d"});
  CHECK(split_lines("a\n") == std::vector<std::string_view>{"a"});
  CHECK(split_lines("\n\na") == std::vector<std::string_view>{"", "", "a"});
  CHECK(split_lines("a\r\n\r\n") == std::vector<std::string_view>{"a", ""});
  CHECK(split_lines("").empty());
  CHECK(join(std::vector<std::string>{"a", "b", "c"}, ", ") == "a, b, c");
  CHECK(join(std::vector<std::string_view>{}, ",").empty());
  CHECK(join(std::vector<std::string_view>{"x"}, ",") == "x");
}

TEST_CASE("text: integer parsing") {
  CHECK(parse_int("0") == 0);
  CHECK(parse_int("-42") == -42);
  CHECK(parse_int("+7") == 7);
  CHECK(parse_int("ff", 16) == 255);
  CHECK(parse_int("9223372036854775807") == std::numeric_limits<std::int64_t>::max());
  CHECK_FALSE(parse_int("9223372036854775808"));
  CHECK_FALSE(parse_int(""));
  CHECK_FALSE(parse_int("+"));
  CHECK_FALSE(parse_int("+-1"));
  CHECK_FALSE(parse_int("12a"));
  CHECK_FALSE(parse_int(" 12"));
  CHECK_FALSE(parse_int("1.0"));
  CHECK_FALSE(parse_int("1", 1));
}

TEST_CASE("text: floating point parsing and formatting") {
  CHECK(parse_double("0") == 0.0);
  CHECK(parse_double("1.5") == 1.5);
  CHECK(parse_double("-2.25e2") == -225.0);
  CHECK(parse_double("+.5") == 0.5);
  CHECK(parse_double("1e-3") == 0.001);
  CHECK(parse_double("0.1") == 0.1);  // correctly rounded
  CHECK(parse_double("1e400") == std::numeric_limits<double>::infinity());
  CHECK(parse_double("-1e400") == -std::numeric_limits<double>::infinity());
  CHECK(parse_double("1e-400") == 0.0);
  CHECK(std::isinf(*parse_double("inf")));
  CHECK(std::isnan(*parse_double("nan")));
  CHECK_FALSE(parse_double(""));
  CHECK_FALSE(parse_double("."));
  CHECK_FALSE(parse_double("1,5"));
  CHECK_FALSE(parse_double("1.5x"));
  CHECK_FALSE(parse_double(" 1"));
  CHECK_FALSE(parse_double("1e"));
  CHECK_FALSE(parse_double("--1"));
  CHECK_FALSE(parse_double("0x10"));

  CHECK(format_double(0.1) == "0.1");
  CHECK(format_double(42.0) == "42");
  CHECK(format_double(-1.5) == "-1.5");
  CHECK(format_double(std::numeric_limits<double>::infinity()) == "inf");
  CHECK(format_double(-std::numeric_limits<double>::infinity()) == "-inf");
  CHECK(format_double(std::nan("")) == "nan");
  for (const double value : {1.0 / 3.0, 6.02214076e23, 5e-324, 123456789.125}) {
    CHECK(parse_double(format_double(value)) == value);
  }
}

TEST_CASE("text: UTF-8 decoding accepts exactly the well-formed sequences") {
  static_assert(is_valid_utf8("plain ASCII"));
  static_assert(is_valid_utf8("\xC3\xA9"));               // U+00E9
  static_assert(is_valid_utf8("\xE2\x82\xAC"));           // U+20AC
  static_assert(is_valid_utf8("\xF0\x9F\x98\x80"));       // U+1F600
  static_assert(is_valid_utf8("\xF4\x8F\xBF\xBF"));       // U+10FFFF
  static_assert(!is_valid_utf8("\x80"));                  // lone continuation
  static_assert(!is_valid_utf8("\xC3"));                  // truncated
  static_assert(!is_valid_utf8("\xC0\xAF"));              // overlong '/'
  static_assert(!is_valid_utf8("\xE0\x80\xAF"));          // overlong
  static_assert(!is_valid_utf8("\xED\xA0\x80"));          // surrogate U+D800
  static_assert(!is_valid_utf8("\xF4\x90\x80\x80"));      // above U+10FFFF
  static_assert(!is_valid_utf8("\xF8\x88\x80\x80\x80"));  // 5-byte form
  static_assert(!is_valid_utf8("\xFF"));
  static_assert(decode_utf8("\xE2\x82\xAC", 0).code_point == 0x20AC);
  static_assert(decode_utf8("\xE2\x82\xAC", 0).length == 3);
  static_assert(!decode_utf8("a", 1).ok);
  static_assert(decode_utf8("a", 1).length == 0);

  CHECK(is_valid_utf8(""));
  CHECK(is_valid_utf8("a\0b"sv));
  CHECK(utf8_length("a\xC3\xA9\xF0\x9F\x98\x80") == 3);
  CHECK(utf8_length("\xFF\xFF") == 2);
}

TEST_CASE("text: malformed UTF-8 is replaced one maximal subpart at a time") {
  std::size_t replaced = 0;
  CHECK(sanitize_utf8("ok", &replaced) == "ok");
  CHECK(replaced == 0);
  // Truncated three-byte sequence followed by ASCII: one replacement.
  CHECK(sanitize_utf8("\xE2\x82" "A", &replaced) == "\xEF\xBF\xBD" "A");
  CHECK(replaced == 1);
  // Two stray continuation bytes: two replacements.
  CHECK(sanitize_utf8("\x80\x80", &replaced) == "\xEF\xBF\xBD\xEF\xBF\xBD");
  CHECK(replaced == 2);
  // Surrogate encoded in UTF-8: the lead is rejected, then each continuation.
  CHECK(sanitize_utf8("\xED\xA0\x80", &replaced) == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
  CHECK(replaced == 3);
  // Truncation at the very end.
  CHECK(sanitize_utf8("a\xF0\x9F\x98", &replaced) == "a\xEF\xBF\xBD");
  CHECK(replaced == 1);
  CHECK(is_valid_utf8(sanitize_utf8(as_chars(test::random_bytes(4096, 3)))));
}

TEST_CASE("text: append_utf8 covers every length and rejects non-scalars") {
  std::string out;
  append_utf8(out, U'A');
  append_utf8(out, 0xE9);
  append_utf8(out, 0x20AC);
  append_utf8(out, 0x1F600);
  CHECK(out == "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
  std::string bad;
  append_utf8(bad, 0xD800);
  append_utf8(bad, 0x110000);
  CHECK(bad == "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST_CASE("text: conversions between UTF-8, UTF-16 and UTF-32") {
  const std::string utf8 = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  const std::u16string utf16 = u"Aé€\U0001F600";
  const std::u32string utf32 = U"Aé€\U0001F600";
  CHECK(utf8_to_utf16(utf8) == utf16);
  CHECK(utf16_to_utf8(utf16) == utf8);
  CHECK(utf8_to_utf32(utf8) == utf32);
  CHECK(utf32_to_utf8(utf32) == utf8);
  CHECK(wide_to_utf8(utf8_to_wide(utf8)) == utf8);
  CHECK(utf8_to_utf16("").empty());

  // Lone surrogates and out-of-range values become U+FFFD.
  const char16_t lone_high[] = {u'a', 0xD83D, u'b', 0};
  CHECK(utf16_to_utf8(lone_high) == "a\xEF\xBF\xBD" "b");
  const char16_t lone_low[] = {0xDE00, 0};
  CHECK(utf16_to_utf8(lone_low) == "\xEF\xBF\xBD");
  const char16_t high_at_end[] = {u'x', 0xD83D, 0};
  CHECK(utf16_to_utf8(high_at_end) == "x\xEF\xBF\xBD");
  const char32_t bad32[] = {0x110000, 0xDFFF, U'z', 0};
  CHECK(utf32_to_utf8(bad32) == "\xEF\xBF\xBD\xEF\xBF\xBDz");
  CHECK(utf8_to_utf32("\xFF" "a") == U"�a");
  CHECK(utf8_to_utf16("\xC3") == u"�");
}

TEST_CASE("text: encoding labels") {
  CHECK(parse_encoding("UTF-8") == TextEncoding::Utf8);
  CHECK(parse_encoding("utf8") == TextEncoding::Utf8);
  CHECK(parse_encoding(" utf_16 ") == TextEncoding::Utf16LE);
  CHECK(parse_encoding("UTF-16BE") == TextEncoding::Utf16BE);
  CHECK(parse_encoding("utf-32be") == TextEncoding::Utf32BE);
  CHECK(parse_encoding("ISO-8859-1") == TextEncoding::Latin1);
  CHECK(parse_encoding("cp1252") == TextEncoding::Windows1252);
  CHECK_FALSE(parse_encoding("klingon"));
  CHECK_FALSE(parse_encoding(""));
  CHECK_FALSE(parse_encoding("a-very-long-label-that-is-not-an-encoding"));
  for (const TextEncoding encoding : {TextEncoding::Utf8, TextEncoding::Utf16LE, TextEncoding::Utf16BE,
                                      TextEncoding::Utf32LE, TextEncoding::Utf32BE, TextEncoding::Latin1,
                                      TextEncoding::Windows1252}) {
    CHECK(parse_encoding(encoding_name(encoding)) == encoding);
  }
}

TEST_CASE("text: byte order marks") {
  CHECK(detect_bom(raw({0xEF, 0xBB, 0xBF, 'a'}))->encoding == TextEncoding::Utf8);
  CHECK(detect_bom(raw({0xEF, 0xBB, 0xBF}))->length == 3);
  CHECK(detect_bom(raw({0xFF, 0xFE, 'a', 0}))->encoding == TextEncoding::Utf16LE);
  CHECK(detect_bom(raw({0xFE, 0xFF, 0, 'a'}))->encoding == TextEncoding::Utf16BE);
  CHECK(detect_bom(raw({0xFF, 0xFE, 0, 0}))->encoding == TextEncoding::Utf32LE);
  CHECK(detect_bom(raw({0, 0, 0xFE, 0xFF}))->encoding == TextEncoding::Utf32BE);
  CHECK_FALSE(detect_bom(raw({'a', 'b'})));
  CHECK_FALSE(detect_bom(raw({0xEF, 0xBB})));
  CHECK_FALSE(detect_bom({}));
  CHECK(bom_bytes(TextEncoding::Latin1).empty());
  CHECK(bom_bytes(TextEncoding::Utf32LE).size() == 4);
}

TEST_CASE("text: encoding detection") {
  CHECK(guess_encoding({}).encoding == TextEncoding::Utf8);
  CHECK(guess_encoding(as_bytes("plain")).encoding == TextEncoding::Utf8);
  CHECK(guess_encoding(as_bytes("\xC3\xA9t\xC3\xA9")).confident);
  const EncodingGuess bom = guess_encoding(raw({0xFE, 0xFF, 0, 'a'}));
  CHECK(bom.encoding == TextEncoding::Utf16BE);
  CHECK(bom.bom_length == 2);

  const std::string_view sample = "Hello, world! This is plain text.";
  CHECK(guess_encoding(encode_text(sample, TextEncoding::Utf16LE).bytes).encoding == TextEncoding::Utf16LE);
  CHECK(guess_encoding(encode_text(sample, TextEncoding::Utf16BE).bytes).encoding == TextEncoding::Utf16BE);
  CHECK(guess_encoding(encode_text("Hello, world! This is text.."sv, TextEncoding::Utf32LE).bytes).encoding ==
        TextEncoding::Utf32LE);
  CHECK(guess_encoding(encode_text("Hello, world! This is text.."sv, TextEncoding::Utf32BE).bytes).encoding ==
        TextEncoding::Utf32BE);

  const EncodingGuess legacy = guess_encoding(raw({'c', 'a', 'f', 0xE9}));
  CHECK(legacy.encoding == TextEncoding::Windows1252);
  CHECK_FALSE(legacy.confident);
}

TEST_CASE("text: decode_text for every encoding") {
  CHECK(decode_text(raw({0xEF, 0xBB, 0xBF, 'h', 'i'}), TextEncoding::Utf8).text == "hi");
  CHECK(decode_text(raw({'h', 0xFF, 'i'}), TextEncoding::Utf8).text == "h\xEF\xBF\xBDi");
  CHECK(decode_text(raw({'h', 0xFF, 'i'}), TextEncoding::Utf8).replaced == 1);

  CHECK(decode_text(raw({0xE9, 0x80, 0x41}), TextEncoding::Latin1).text == "\xC3\xA9\xC2\x80" "A");
  CHECK(decode_text(raw({0xE9, 0x80, 0x93, 0x81}), TextEncoding::Windows1252).text ==
        "\xC3\xA9\xE2\x82\xAC\xE2\x80\x9C\xC2\x81");

  CHECK(decode_text(raw({0xFF, 0xFE, 0x41, 0x00, 0xAC, 0x20}), TextEncoding::Utf16LE).text == "A\xE2\x82\xAC");
  CHECK(decode_text(raw({0x00, 0x41, 0x20, 0xAC}), TextEncoding::Utf16BE).text == "A\xE2\x82\xAC");
  CHECK(decode_text(raw({0x3D, 0xD8, 0x00, 0xDE}), TextEncoding::Utf16LE).text == "\xF0\x9F\x98\x80");
  // Malformed UTF-16: lone high, lone low, high at the end, odd trailing byte.
  const DecodedText broken16 = decode_text(raw({0x3D, 0xD8, 0x41, 0x00, 0x00, 0xDE, 0x3D, 0xD8, 0x42}), TextEncoding::Utf16LE);
  CHECK(broken16.text == "\xEF\xBF\xBD" "A" "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
  CHECK(broken16.replaced == 4);

  CHECK(decode_text(raw({0x00, 0xF6, 0x01, 0x00}), TextEncoding::Utf32LE).text == "\xF0\x9F\x98\x80");
  CHECK(decode_text(raw({0x00, 0x01, 0xF6, 0x00}), TextEncoding::Utf32BE).text == "\xF0\x9F\x98\x80");
  const DecodedText broken32 = decode_text(raw({0x00, 0x00, 0x11, 0x00, 0x41, 0x00, 0x00, 0x00, 0x42}), TextEncoding::Utf32LE);
  CHECK(broken32.text == "\xEF\xBF\xBD" "A" "\xEF\xBF\xBD");
  CHECK(broken32.replaced == 2);

  // A BOM of a different encoding is content, not a mark.
  CHECK(decode_text(raw({0xEF, 0xBB, 0xBF}), TextEncoding::Latin1).text == "\xC3\xAF\xC2\xBB\xC2\xBF");
  for (const TextEncoding encoding : {TextEncoding::Utf8, TextEncoding::Utf16LE, TextEncoding::Utf32BE, TextEncoding::Latin1}) {
    CHECK(decode_text({}, encoding).text.empty());
  }
}

TEST_CASE("text: encode_text for every encoding") {
  const std::string_view text = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // A, e-acute, euro sign, emoji
  CHECK(encode_text(text, TextEncoding::Utf8).bytes == to_bytes(text));
  CHECK(encode_text("a", TextEncoding::Utf8, true).bytes == raw({0xEF, 0xBB, 0xBF, 'a'}));
  CHECK(encode_text(text, TextEncoding::Utf16LE).bytes ==
        raw({0x41, 0x00, 0xE9, 0x00, 0xAC, 0x20, 0x3D, 0xD8, 0x00, 0xDE}));
  CHECK(encode_text(text, TextEncoding::Utf16BE, true).bytes ==
        raw({0xFE, 0xFF, 0x00, 0x41, 0x00, 0xE9, 0x20, 0xAC, 0xD8, 0x3D, 0xDE, 0x00}));
  CHECK(encode_text("A\xF0\x9F\x98\x80", TextEncoding::Utf32LE, true).bytes ==
        raw({0xFF, 0xFE, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0xF6, 0x01, 0x00}));
  CHECK(encode_text("A", TextEncoding::Utf32BE).bytes == raw({0x00, 0x00, 0x00, 0x41}));

  const EncodedText latin1 = encode_text(text, TextEncoding::Latin1, true);
  CHECK(latin1.bytes == raw({0x41, 0xE9, '?', '?'}));  // no BOM for single-byte encodings
  CHECK(latin1.replaced == 2);
  const EncodedText cp1252 = encode_text(text, TextEncoding::Windows1252);
  CHECK(cp1252.bytes == raw({0x41, 0xE9, 0x80, '?'}));
  CHECK(cp1252.replaced == 1);

  const EncodedText malformed = encode_text("a\xFFz", TextEncoding::Utf16LE);
  CHECK(malformed.bytes == raw({'a', 0x00, 0xFD, 0xFF, 'z', 0x00}));
  CHECK(malformed.replaced == 1);
}

TEST_CASE("text: every code point survives a round trip through the Unicode encodings") {
  std::string all;
  for (char32_t cp = 0; cp < 0x110000; cp += (cp < 0x3000 ? 1 : 257)) {
    if (is_scalar_value(cp)) append_utf8(all, cp);
  }
  append_utf8(all, 0x10FFFF);
  REQUIRE(is_valid_utf8(all));
  for (const TextEncoding encoding : {TextEncoding::Utf8, TextEncoding::Utf16LE, TextEncoding::Utf16BE,
                                      TextEncoding::Utf32LE, TextEncoding::Utf32BE}) {
    for (const bool bom : {false, true}) {
      const EncodedText encoded = encode_text(all, encoding, bom);
      CHECK(encoded.replaced == 0);
      const DecodedText decoded = decode_text(encoded.bytes, encoding);
      CHECK(decoded.replaced == 0);
      // A text starting with U+0000 has no BOM confusion; compare everything.
      CHECK(decoded.text == all);
    }
  }
  // All 256 byte values round-trip through the single-byte encodings.
  const Bytes bytes = test::all_byte_values();
  for (const TextEncoding encoding : {TextEncoding::Latin1, TextEncoding::Windows1252}) {
    const DecodedText decoded = decode_text(bytes, encoding);
    CHECK(decoded.replaced == 0);
    const EncodedText encoded = encode_text(decoded.text, encoding);
    CHECK(encoded.replaced == 0);
    CHECK(encoded.bytes == bytes);
  }
}

TEST_CASE("text: looks_like_text") {
  CHECK(looks_like_text(as_bytes("Just some words.\nAnd a second line.\n")));
  CHECK(looks_like_text(as_bytes("\xEF\xBB\xBFwith a BOM")));
  CHECK(looks_like_text(as_bytes("tabs\tand\x1B[0m escapes")));
  CHECK(looks_like_text(raw({0xFF, 0xFE, 'h', 0, 'i', 0})));
  CHECK_FALSE(looks_like_text({}));
  CHECK_FALSE(looks_like_text(as_bytes("nul\0inside"sv)));
  CHECK_FALSE(looks_like_text(raw({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})));
  CHECK_FALSE(looks_like_text(raw({0x01, 0x02, 0x03, 0x04, 0x05, 0x06})));
  CHECK_FALSE(looks_like_text(test::random_bytes(2048, 5)));
}

TEST_CASE("text: line endings") {
  const std::string_view mixed = "a\nb\r\nc\rd";
  CHECK(convert_line_endings(mixed, LineEnding::Lf) == "a\nb\nc\nd");
  CHECK(convert_line_endings(mixed, LineEnding::CrLf) == "a\r\nb\r\nc\r\nd");
  CHECK(convert_line_endings(mixed, LineEnding::Cr) == "a\rb\rc\rd");
  CHECK(convert_line_endings("\r\n\r\n", LineEnding::Lf) == "\n\n");
  CHECK(convert_line_endings("\r", LineEnding::CrLf) == "\r\n");
  CHECK(convert_line_endings("", LineEnding::CrLf).empty());
  CHECK(detect_line_ending("a\r\nb\r\nc\n") == LineEnding::CrLf);
  CHECK(detect_line_ending("a\nb") == LineEnding::Lf);
  CHECK(detect_line_ending("a\rb\rc") == LineEnding::Cr);
  CHECK_FALSE(detect_line_ending("no breaks"));
}
