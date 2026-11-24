// Tests for the encodings registered directly in encoding_family.cpp:
// bin, base64, base64url, hex, hexdump, data-uri, c-header, uue, url-encoded,
// binary-text, checksums, txt and the PlainText -> Document bridge.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/encoding/encoding.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace std::string_view_literals;
using test::convert;
using test::str;

namespace {

// Encode `data` as `format` with the engine and return the text.
std::string encoded(ByteView data, std::string_view format, std::string_view options = {},
                    std::string_view filename = "input.bin") {
  const test::Converted result = convert(data, "bin", format, options, filename);
  REQUIRE_MESSAGE(result.ok(), format, ": ", result.message);
  return result.text();
}
std::string encoded(std::string_view text, std::string_view format, std::string_view options = {},
                    std::string_view filename = "input.bin") {
  return encoded(as_bytes(text), format, options, filename);
}

// Decode `text` from `format` back to bytes with the engine.
test::Converted decoded(std::string_view text, std::string_view format, std::string_view options = {}) {
  return convert(text, format, "bin", options);
}

constexpr std::string_view hello = "Hello\n";

}  // namespace

TEST_CASE("encoding: bin passes bytes through unchanged") {
  const Bytes all = test::all_byte_values();
  const test::Converted result = convert(all, "png", "bin", {}, "odd.png");
  REQUIRE(result.ok());
  CHECK(result.data == all);
  CHECK(result.format_id == "bin");
  CHECK(result.extension == "bin");
  CHECK(result.mime == "application/octet-stream");
  CHECK(convert(ByteView{}, "txt", "bin").data.empty());
}

TEST_CASE("encoding: base64 options and decoding") {
  CHECK(encoded("foobar", "base64") == "Zm9vYmFy\n");  // wrapped at 76 by default, so the text ends in a newline
  CHECK(encoded("foobar", "base64", R"({"line_width":0})") == "Zm9vYmFy");
  CHECK(encoded("foobar", "base64", R"({"line_width":4})") == "Zm9v\nYmFy\n");
  CHECK(encoded("foobar", "base64", R"({"line_width":4,"line_ending":"crlf"})") == "Zm9v\r\nYmFy\r\n");
  CHECK(encoded("f", "base64", R"({"line_width":0})") == "Zg==");
  CHECK(encoded("f", "base64", R"({"line_width":0,"padding":false})") == "Zg");
  CHECK(encoded("", "base64").empty());

  const std::string wrapped = encoded(test::random_bytes(200, 3), "base64");
  for (const std::string_view line : {std::string_view(wrapped).substr(0, 77), std::string_view(wrapped).substr(77, 77)}) {
    CHECK(line.size() == 77);
    CHECK(line.back() == '\n');
  }

  CHECK(decoded("Zm9v\r\nYmFy\r\n", "base64").text() == "foobar");
  CHECK(decoded("Zm9vYg", "base64").text() == "foob");
  CHECK(decoded("Zm9v!mFy", "base64").status == Status::Decode);
  CHECK(decoded("Z", "base64").status == Status::Decode);
}

TEST_CASE("encoding: base64url uses the URL-safe alphabet without padding") {
  const Bytes data = {std::byte{0xFB}, std::byte{0xFF}, std::byte{0xFE}, std::byte{0xFF}};
  CHECK(encoded(data, "base64url") == "-__-_w");
  CHECK(encoded(data, "base64url", R"({"padding":true})") == "-__-_w==");
  CHECK(encoded(data, "base64", R"({"line_width":0})") == "+//+/w==");
  CHECK(decoded("-__-_w", "base64url").data == data);
  CHECK(decoded("+//+/w==", "base64url").data == data);  // either alphabet is understood
}

TEST_CASE("encoding: hex styles and tolerant parsing") {
  CHECK(encoded("\x01\xAB\xFF", "hex") == "01abff");
  CHECK(encoded("\x01\xAB\xFF", "hex", R"({"uppercase":true})") == "01ABFF");
  CHECK(encoded("\x01\xAB\xFF", "hex", R"({"separator":"space"})") == "01 ab ff");
  CHECK(encoded("\x01\xAB\xFF", "hex", R"({"separator":"colon","uppercase":true})") == "01:AB:FF");
  CHECK(encoded("\x01\xAB\xFF\x10", "hex", R"({"bytes_per_line":2,"separator":"space"})") == "01 ab\nff 10\n");
  CHECK(encoded("", "hex").empty());
  CHECK(convert(hello, "bin", "hex", R"({"separator":"dots"})").status == Status::InvalidArgument);

  CHECK(decoded("48656c6C6F", "hex").text() == "Hello");
  CHECK(decoded("0x48, 0x65, 0x6c\n6c:6f", "hex").text() == "Hello");
  CHECK(decoded("48-65-6c-6c-6f", "hex").text() == "Hello");
  CHECK(decoded("486", "hex").status == Status::Decode);
  CHECK(decoded("48 6g", "hex").status == Status::Decode);

  encoding::HexStyle style;
  style.bytes_per_line = 2;
  style.separator = " ";
  CHECK(encoding::hex_text(as_bytes("abc"), style) == "61 62\n63\n");
}

TEST_CASE("encoding: hexdump matches the xxd layout") {
  CHECK(encoded(hello, "hexdump") == "00000000: 4865 6c6c 6f0a                           Hello.\n");
  CHECK(encoding::hexdump_text(as_bytes("0123456789abcdefXYZ"), 16, false) ==
        "00000000: 3031 3233 3435 3637 3839 6162 6364 6566  0123456789abcdef\n"
        "00000010: 5859 5a                                  XYZ\n");
  CHECK(encoded("\xff\x00\x7f "sv, "hexdump", R"({"bytes_per_line":4,"uppercase":true})") == "00000000: FF00 7F20  ... \n");
  CHECK(encoded("abc", "hexdump", R"({"bytes_per_line":1})") ==
        "00000000: 61  a\n00000001: 62  b\n00000002: 63  c\n");
  CHECK(encoded("", "hexdump").empty());

  CHECK(decoded("00000000: 4865 6c6c 6f0a                           Hello.\n", "hexdump").text() == hello);
  CHECK(decoded("00000000: 6120 2062                                a  b\n", "hexdump").text() == "a  b");
  CHECK(decoded("no offset column here\n", "hexdump").status == Status::Decode);
  CHECK(decoded("00000000: 48zz\n", "hexdump").status == Status::Decode);

  const Bytes all = test::all_byte_values();
  for (const std::string_view options : {R"({})", R"({"bytes_per_line":7})", R"({"bytes_per_line":64,"uppercase":true})"}) {
    const test::Converted back = decoded(encoded(all, "hexdump", options), "hexdump");
    REQUIRE(back.ok());
    CHECK(back.data == all);
  }
}

TEST_CASE("encoding: data URIs carry the media type") {
  CHECK(encoded("Hi!", "data-uri") == "data:application/octet-stream;base64,SGkh");
  CHECK(convert("Hi!", "txt", "data-uri").text() == "data:text/plain;base64,SGkh");
  CHECK(convert("Hi!", "png", "data-uri").text() == "data:image/png;base64,SGkh");
  CHECK(encoded("Hi!", "data-uri", R"({"mime":"image/gif"})") == "data:image/gif;base64,SGkh");
  CHECK(encoded("a b&c", "data-uri", R"({"base64":false,"mime":"text/plain"})") == "data:text/plain,a%20b%26c");
  CHECK(convert("Hi!", "bin", "data-uri", R"({"mime":"text/plain,evil"})").status == Status::InvalidArgument);
  CHECK(convert("Hi!", "bin", "data-uri", R"({"mime":"text/pl ain"})").status == Status::InvalidArgument);

  const auto parsed = encoding::parse_data_uri("  data:Text/HTML;charset=utf-8;base64,PGI+\n");
  REQUIRE(parsed);
  CHECK(str(parsed->bytes) == "<b>");
  CHECK(parsed->mime == "text/html");
  CHECK(encoding::parse_data_uri("data:,A%20brief%20note")->mime == "text/plain");
  CHECK(str(encoding::parse_data_uri("data:,A%20brief%20note")->bytes) == "A brief note");

  CHECK(decoded("data:image/png;base64,SGkh", "data-uri").text() == "Hi!");
  CHECK(decoded("SGkh", "data-uri").status == Status::Decode);
  CHECK(decoded("data:text/plain;base64", "data-uri").status == Status::Decode);
  CHECK(decoded("data:;base64,S!!!", "data-uri").status == Status::Decode);
  CHECK(decoded("data:,%4", "data-uri").status == Status::Decode);
}

TEST_CASE("encoding: C header output and identifiers") {
  CHECK(encoded("Hi!", "c-header", {}, "my photo.png") ==
        "/* my photo.png, 3 bytes */\n"
        "unsigned char my_photo_png[] = {\n"
        "  0x48, 0x69, 0x21\n"
        "};\n"
        "unsigned int my_photo_png_len = 3;\n");
  const std::string two_per_line = encoded("Hello", "c-header", R"({"variable_name":"blob","bytes_per_line":2})");
  CHECK(two_per_line.find("unsigned char blob[] = {\n  0x48, 0x65,\n  0x6c, 0x6c,\n  0x6f\n};\n") != std::string::npos);
  CHECK(two_per_line.find("unsigned int blob_len = 5;") != std::string::npos);

  // Hostile names cannot break out of the declaration or the comment.
  const std::string hostile = encoded("x", "c-header", R"({"variable_name":"9 lives; drop--"})");
  CHECK(hostile.find("unsigned char _9_lives__drop__[] = {") != std::string::npos);
  CHECK(hostile.find("unsigned int _9_lives__drop___len = 1;") != std::string::npos);
  CHECK(encoding::c_header(as_bytes("x"), "v", 12, "a*/b */ c").starts_with("/* a* /b * / c */\n"));

  CHECK(encoding::c_identifier("my photo.png") == "my_photo_png");
  CHECK(encoding::c_identifier("") == "data");
  CHECK(encoding::c_identifier("...") == "data");
  CHECK(encoding::c_identifier("1st") == "_1st");
  CHECK(encoding::c_identifier("caf\xC3\xA9") == "caf__");

  // The engine clamps bytes_per_line to its declared range.
  CHECK(encoded("abc", "c-header", R"({"variable_name":"v","bytes_per_line":0})") ==
        encoded("abc", "c-header", R"({"variable_name":"v","bytes_per_line":1})"));
  CHECK(encoded("abc", "c-header", R"({"variable_name":"v","bytes_per_line":1000})") ==
        encoded("abc", "c-header", R"({"variable_name":"v","bytes_per_line":64})"));
  CHECK(test::find_reader("c-header", Kind::Blob) == nullptr);  // one-way
}

TEST_CASE("encoding: uuencode vectors and file names") {
  CHECK(encoding::uuencode(as_bytes("Cat"), "cat.txt") == "begin 644 cat.txt\n#0V%T\n`\nend\n");
  CHECK(encoded("Cat", "uue", {}, "cat.txt") == "begin 644 cat.txt\n#0V%T\n`\nend\n");
  CHECK(encoded("Cat", "uue", R"({"filename":"other name.bin"})") == "begin 644 othername.bin\n#0V%T\n`\nend\n");
  CHECK(encoding::uuencode({}, "") == "begin 644 data.bin\n`\nend\n");

  const auto parsed = encoding::uudecode("junk before\nbegin 644 cat.txt\n#0V%T\n`\nend\n");
  REQUIRE(parsed);
  CHECK(str(parsed->bytes) == "Cat");
  CHECK(parsed->filename == "cat.txt");

  // A full 45-byte line starts with 'M'.
  const std::string long_text = encoding::uuencode(test::random_bytes(100, 9), "r.bin");
  CHECK(long_text.substr(16, 1) == "M");
  CHECK(encoding::uudecode(long_text)->bytes == test::random_bytes(100, 9));

  CHECK(decoded("#0V%T\n`\nend\n", "uue").status == Status::Decode);               // no begin line
  CHECK(decoded("begin 644 cat.txt\n#0V%T\n", "uue").status == Status::Decode);     // no end line
  CHECK(decoded("begin 644 cat.txt\n#0V\nend\n", "uue").status == Status::Decode);  // truncated line
  CHECK(decoded("begin 644 cat.txt\n#0V%\x7f\nend\n", "uue").status == Status::Decode);
}

TEST_CASE("encoding: url-encoded bytes") {
  CHECK(encoded("a b&c=d/e~f", "url-encoded") == "a%20b%26c%3Dd%2Fe~f");
  CHECK(encoded("a b", "url-encoded", R"({"space_as_plus":true})") == "a+b");
  CHECK(encoded("\xC3\xA9\x00\xFF"sv, "url-encoded") == "%C3%A9%00%FF");
  CHECK(decoded("a%20b%26c\n", "url-encoded").text() == "a b&c");
  CHECK(decoded("a+b", "url-encoded").text() == "a+b");
  CHECK(decoded("a+b", "url-encoded", R"({"plus_as_space":true})").text() == "a b");
  CHECK(decoded("%e9%E9", "url-encoded").text() == "\xE9\xE9");
  CHECK(decoded("100%", "url-encoded").status == Status::Decode);
  CHECK(decoded("%4", "url-encoded").status == Status::Decode);
  CHECK(decoded("%zz", "url-encoded").status == Status::Decode);
}

TEST_CASE("encoding: binary digits") {
  CHECK(encoded("Hi", "binary-text") == "01001000 01101001\n");
  CHECK(encoded("Hi", "binary-text", R"({"separate_bytes":false,"bytes_per_line":0})") == "0100100001101001");
  CHECK(encoded("Hi!", "binary-text", R"({"bytes_per_line":2})") == "01001000 01101001\n00100001\n");
  CHECK(encoded("", "binary-text").empty());
  CHECK(decoded("01001000 01101001\n", "binary-text").text() == "Hi");
  CHECK(decoded("0100100001101001", "binary-text").text() == "Hi");
  CHECK(decoded("0100 1000", "binary-text").text() == "H");
  CHECK(decoded("0100100", "binary-text").status == Status::Decode);
  CHECK(decoded("01002000", "binary-text").status == Status::Decode);
}

TEST_CASE("encoding: checksum report matches known digests") {
  const std::string report = convert("abc", "bin", "checksums", {}, "abc.txt").text();
  CHECK(report ==
        "File: abc.txt\n"
        "Size: 3 bytes\n"
        "CRC-32: 352441c2\n"
        "Adler-32: 024d0127\n"
        "MD5: 900150983cd24fb0d6963f7d28e17f72\n"
        "SHA-1: a9993e364706816aba3e25717850c26c9cd0d89d\n"
        "SHA-256: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n");
  const std::string upper = convert("abc", "bin", "checksums", R"({"uppercase":true})").text();
  CHECK(upper.find("File:") == std::string::npos);
  CHECK(upper.find("CRC-32: 352441C2\n") != std::string::npos);
  CHECK(upper.find("SHA-1: A9993E364706816ABA3E25717850C26C9CD0D89D\n") != std::string::npos);

  const std::string empty = encoding::checksum_report({}, "", false);
  CHECK(empty.find("Size: 0 bytes\n") != std::string::npos);
  CHECK(empty.find("CRC-32: 00000000\n") != std::string::npos);
  CHECK(empty.find("MD5: d41d8cd98f00b204e9800998ecf8427e\n") != std::string::npos);
  CHECK(empty.find("SHA-256: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n") != std::string::npos);
  CHECK(test::find_reader("checksums", Kind::Blob) == nullptr);  // a report cannot be decoded
}

TEST_CASE("encoding: txt reads any Unicode encoding") {
  const std::string_view euro_utf8 = "\xE2\x82\xAC" "5";
  auto text = test::read_as<ir::PlainText>("txt", as_bytes(euro_utf8));
  REQUIRE(text);
  CHECK(text->text == euro_utf8);
  CHECK(text->source_encoding == "utf-8");
  CHECK_FALSE(text->had_bom);

  text = test::read_as<ir::PlainText>("txt", as_bytes("\xEF\xBB\xBF" "abc"));
  REQUIRE(text);
  CHECK(text->text == "abc");
  CHECK(text->had_bom);

  const Bytes utf16le = {std::byte{0xFF}, std::byte{0xFE}, std::byte{0xAC}, std::byte{0x20}, std::byte{0x35}, std::byte{0x00}};
  text = test::read_as<ir::PlainText>("txt", utf16le);
  REQUIRE(text);
  CHECK(text->text == euro_utf8);
  CHECK(text->source_encoding == "utf-16le");
  CHECK(text->had_bom);

  const Bytes utf16be = {std::byte{0xFE}, std::byte{0xFF}, std::byte{0x20}, std::byte{0xAC}, std::byte{0x00}, std::byte{0x35}};
  CHECK(test::read_as<ir::PlainText>("txt", utf16be)->text == euro_utf8);

  // Bytes that are not Unicode fall back to Windows-1252, with a warning.
  std::vector<std::string> warnings;
  text = test::read_as<ir::PlainText>("txt", as_bytes("\x80" "5 caf\xE9"), {}, &warnings);
  REQUIRE(text);
  CHECK(text->text == "\xE2\x82\xAC" "5 caf\xC3\xA9");
  CHECK(text->source_encoding == "windows-1252");
  CHECK(warnings.size() == 1);

  // An explicit source encoding overrides detection.
  text = test::read_as<ir::PlainText>("txt", as_bytes("caf\xE9"), R"({"source_encoding":"latin1"})", &warnings);
  REQUIRE(text);
  CHECK(text->text == "caf\xC3\xA9");
  CHECK(warnings.empty());
  text = test::read_as<ir::PlainText>("txt", as_bytes("\xFF"), R"({"source_encoding":"utf-8"})", &warnings);
  REQUIRE(text);
  CHECK(text->text == "\xEF\xBF\xBD");
  CHECK(warnings.size() == 1);
  CHECK(test::read_as<ir::PlainText>("txt", as_bytes("x"), R"({"source_encoding":"klingon"})").error().code ==
        Status::InvalidArgument);
}

TEST_CASE("encoding: txt writes encodings, byte order marks and line endings") {
  const std::string_view source = "\xE2\x82\xAC" "1\nb\r\nc\rd";
  CHECK(convert(source, "txt", "txt").text() == source);
  CHECK(convert(source, "txt", "txt", R"({"bom":true})").text() == "\xEF\xBB\xBF" + std::string(source));
  CHECK(convert(source, "txt", "txt", R"({"line_endings":"lf"})").text() == "\xE2\x82\xAC" "1\nb\nc\nd");
  CHECK(convert(source, "txt", "txt", R"({"line_endings":"crlf"})").text() == "\xE2\x82\xAC" "1\r\nb\r\nc\r\nd");
  CHECK(convert(source, "txt", "txt", R"({"line_endings":"cr"})").text() == "\xE2\x82\xAC" "1\rb\rc\rd");

  const test::Converted utf16 = convert("\xE2\x82\xAC" "5", "txt", "txt", R"({"encoding":"utf-16le","bom":true})");
  REQUIRE(utf16.ok());
  CHECK(utf16.data == Bytes{std::byte{0xFF}, std::byte{0xFE}, std::byte{0xAC}, std::byte{0x20}, std::byte{0x35}, std::byte{0x00}});
  const test::Converted utf16be = convert("\xE2\x82\xAC" "5", "txt", "txt", R"({"encoding":"utf-16be"})");
  CHECK(utf16be.data == Bytes{std::byte{0x20}, std::byte{0xAC}, std::byte{0x00}, std::byte{0x35}});
  const test::Converted utf32 = convert("A", "txt", "txt", R"({"encoding":"utf-32be","bom":true})");
  CHECK(utf32.data == Bytes{std::byte{0x00}, std::byte{0x00}, std::byte{0xFE}, std::byte{0xFF}, std::byte{0x00},
                            std::byte{0x00}, std::byte{0x00}, std::byte{0x41}});

  const test::Converted cp1252 = convert("\xE2\x82\xAC" "5 caf\xC3\xA9", "txt", "txt", R"({"encoding":"windows-1252"})");
  CHECK(cp1252.text() == "\x80" "5 caf\xE9");
  CHECK(cp1252.warnings.empty());

  // Characters the target encoding lacks become '?', and the user is told.
  const test::Converted latin1 = convert("\xE2\x82\xAC" "5 caf\xC3\xA9", "txt", "txt", R"({"encoding":"latin1"})");
  REQUIRE(latin1.ok());
  CHECK(latin1.text() == "?5 caf\xE9");
  CHECK(latin1.warnings.size() == 1);

  // Reading and writing compose: UTF-16 in, Windows-1252 with CRLF out.
  const Bytes utf16_input = {std::byte{0xFF}, std::byte{0xFE}, std::byte{0xE9}, std::byte{0x00}, std::byte{0x0A}, std::byte{0x00}};
  CHECK(convert(utf16_input, "txt", "txt", R"({"encoding":"windows-1252","line_endings":"crlf"})").text() == "\xE9\r\n");
  CHECK(convert("x", "txt", "txt", R"({"encoding":"ebcdic"})").status == Status::InvalidArgument);
  CHECK(convert("x", "txt", "txt", R"({"line_endings":"vertical"})").status == Status::InvalidArgument);
}

TEST_CASE("encoding: plain text becomes one paragraph per block of lines") {
  const Bridge* bridge = nullptr;
  for (const Bridge& candidate : Registry::instance().bridges()) {
    if (candidate.from == Kind::PlainText && candidate.to == Kind::Document) bridge = &candidate;
  }
  REQUIRE(bridge != nullptr);
  CHECK_FALSE(bridge->lossy);

  ir::PlainText text;
  text.text = "First line  \nsecond line\r\n\r\n   \n\nSecond paragraph\n";
  Context ctx;
  ctx.registry = &Registry::instance();
  auto result = bridge->fn(AnyIR(text), ctx);
  REQUIRE(result);
  REQUIRE(result->is<ir::Document>());
  const ir::Document& document = result->get<ir::Document>();
  REQUIRE(document.blocks.size() == 2);
  const auto paragraph_text = [&](std::size_t index) -> std::string {
    const ir::Paragraph* paragraph = document.blocks[index].get_if<ir::Paragraph>();
    if (paragraph == nullptr || paragraph->content.size() != 1) return "<not a single-run paragraph>";
    const ir::Text* run = paragraph->content[0].get_if<ir::Text>();
    return run != nullptr ? run->text : "<not text>";
  };
  CHECK(paragraph_text(0) == "First line\nsecond line");
  CHECK(paragraph_text(1) == "Second paragraph");

  text.text = " \n\t\n";
  result = bridge->fn(AnyIR(text), ctx);
  REQUIRE(result);
  CHECK(result->get<ir::Document>().blocks.empty());
}
