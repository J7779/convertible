// Tests for the source-code array literals (C++, Python, JavaScript, Java, Go, Rust, Swift, C#).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>
#include <vector>

#include "families/encoding/encoding.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace cvt::encoding;
using test::convert;

namespace {

struct LanguageCase {
  ArrayLanguage language;
  std::string_view format;
  std::string_view extension;
  std::string_view expected;  // for the bytes 48 69 21 ff 80 named "blob"
};

const Bytes sample = {std::byte{0x48}, std::byte{0x69}, std::byte{0x21}, std::byte{0xFF}, std::byte{0x80}};

const LanguageCase languages[] = {
    {ArrayLanguage::Cpp, "cpp-array", "hpp",
     "#pragma once\n\n#include <array>\n#include <cstdint>\n\n"
     "inline constexpr std::array<std::uint8_t, 5> blob = {\n    0x48, 0x69, 0x21, 0xff, 0x80,\n};\n"},
    {ArrayLanguage::Python, "python-bytes", "py", "blob = bytes([\n    0x48, 0x69, 0x21, 0xff, 0x80,\n])\n"},
    {ArrayLanguage::JavaScript, "js-array", "js", "const blob = new Uint8Array([\n  0x48, 0x69, 0x21, 0xff, 0x80,\n]);\n"},
    {ArrayLanguage::Java, "java-array", "java", "byte[] blob = {\n    0x48, 0x69, 0x21, (byte) 0xff, (byte) 0x80,\n};\n"},
    {ArrayLanguage::Go, "go-array", "go", "var blob = []byte{\n\t0x48, 0x69, 0x21, 0xff, 0x80,\n}\n"},
    {ArrayLanguage::Rust, "rust-array", "rs", "pub static BLOB: [u8; 5] = [\n    0x48, 0x69, 0x21, 0xff, 0x80,\n];\n"},
    {ArrayLanguage::Swift, "swift-array", "swift", "let blob: [UInt8] = [\n    0x48, 0x69, 0x21, 0xff, 0x80,\n]\n"},
    {ArrayLanguage::CSharp, "csharp-array", "cs", "byte[] blob = new byte[] {\n    0x48, 0x69, 0x21, 0xff, 0x80,\n};\n"},
};

std::size_t count_of(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) ++count;
  return count;
}

}  // namespace

TEST_CASE("encoding: array literal shape in every language") {
  for (const LanguageCase& language : languages) {
    CAPTURE(language.format);
    CHECK(array_literal(sample, language.language, "blob", 12, "") == language.expected);

    const test::Converted result = convert(sample, "bin", language.format, R"({"variable_name":"blob"})");
    REQUIRE(result.ok());
    CHECK(result.text() == language.expected);  // no file name, so no comment line
    CHECK(result.extension == language.extension);
    CHECK(result.format_id == language.format);
    CHECK(test::find_reader(language.format, Kind::Blob) == nullptr);  // one-way
    CHECK(convert(result.data, language.format, "bin").data == result.data);  // "decoding" just copies the text
  }
}

TEST_CASE("encoding: array literal of an empty file") {
  CHECK(array_literal({}, ArrayLanguage::Cpp, "e", 12, "").ends_with("inline constexpr std::array<std::uint8_t, 0> e = {\n};\n"));
  CHECK(array_literal({}, ArrayLanguage::Python, "e", 12, "") == "e = bytes([\n])\n");
  CHECK(array_literal({}, ArrayLanguage::JavaScript, "e", 12, "") == "const e = new Uint8Array([\n]);\n");
  CHECK(array_literal({}, ArrayLanguage::Java, "e", 12, "") == "byte[] e = {\n};\n");
  CHECK(array_literal({}, ArrayLanguage::Go, "e", 12, "") == "var e = []byte{\n}\n");
  CHECK(array_literal({}, ArrayLanguage::Rust, "e", 12, "") == "pub static E: [u8; 0] = [\n];\n");
  CHECK(array_literal({}, ArrayLanguage::Swift, "e", 12, "") == "let e: [UInt8] = [\n]\n");
  CHECK(array_literal({}, ArrayLanguage::CSharp, "e", 12, "") == "byte[] e = new byte[] {\n};\n");
}

TEST_CASE("encoding: array literal names come from the file name") {
  const test::Converted python = convert(sample, "bin", "python-bytes", {}, "my photo.png");
  REQUIRE(python.ok());
  CHECK(python.text() == "# my photo.png, 5 bytes\nmy_photo_png = bytes([\n    0x48, 0x69, 0x21, 0xff, 0x80,\n])\n");
  // Rust statics are upper case by convention.
  const test::Converted rust = convert(sample, "bin", "rust-array", {}, "my photo.png");
  REQUIRE(rust.ok());
  CHECK(rust.text().starts_with("// my photo.png, 5 bytes\npub static MY_PHOTO_PNG: [u8; 5] = [\n"));
  // Without any name there is still a valid identifier.
  CHECK(convert(sample, "bin", "go-array").text().starts_with("var data = []byte{\n"));
  CHECK(convert(sample, "bin", "rust-array").text().starts_with("pub static DATA: [u8; 5] = [\n"));
  // An explicit name wins over the file name.
  CHECK(convert(sample, "bin", "swift-array", R"({"variable_name":"icon"})", "my photo.png").text().find("let icon: [UInt8] = [") !=
        std::string::npos);
}

TEST_CASE("encoding: hostile variable names and file names stay harmless") {
  for (const LanguageCase& language : languages) {
    CAPTURE(language.format);
    const std::string expected_name =
        language.language == ArrayLanguage::Rust ? "_9_LIVES__DROP__" : "_9_lives__drop__";
    const test::Converted result = convert(sample, "bin", language.format, R"({"variable_name":"9 lives; drop--"})");
    REQUIRE(result.ok());
    const std::string text = result.text();
    CHECK(count_of(text, expected_name) == 1);
    CHECK(text.find("drop--") == std::string::npos);
    CHECK(text.find("9 lives") == std::string::npos);
    // Only the punctuation of the declaration itself: its terminator, and the "[u8; N]" of Rust.
    CHECK(count_of(text, ";") <= (language.language == ArrayLanguage::Rust ? 2u : 1u));

    // A name made of line breaks and comment terminators can neither end the
    // comment line nor add code.
    const std::string with_comment =
        array_literal(sample, language.language, "x\ny", 12, "evil\nimport os\rA\xC2\x85" "B\xE2\x80\xA8" "C\xE2\x80\xA9" "D */ \x01");
    const std::vector<std::string_view> lines = util::split_lines(with_comment);
    const std::string without_comment = array_literal(sample, language.language, "x_y", 12, "");
    const std::vector<std::string_view> plain = util::split_lines(without_comment);
    REQUIRE(lines.size() == plain.size() + 1);
    CHECK(lines[0].ends_with("evil import os A B C D */  "));
    CHECK(with_comment.find("\xC2\x85") == std::string::npos);
    CHECK(with_comment.find("\xE2\x80\xA8") == std::string::npos);
    CHECK(with_comment.find("\xE2\x80\xA9") == std::string::npos);
    for (std::size_t i = 0; i < plain.size(); ++i) CHECK(lines[i + 1] == plain[i]);
  }

  CHECK(array_identifier("9 lives; drop--", ArrayLanguage::Python) == "_9_lives__drop__");
  CHECK(array_identifier("", ArrayLanguage::Go) == "data");
  CHECK(array_identifier("\xE2\x98\x83", ArrayLanguage::Go) == "data");  // nothing usable left
  CHECK(array_identifier("caf\xC3\xA9.txt", ArrayLanguage::Swift) == "caf___txt");
  // Reserved words get a trailing underscore.
  CHECK(array_identifier("class", ArrayLanguage::Java) == "class_");
  CHECK(array_identifier("for", ArrayLanguage::Python) == "for_");
  CHECK(array_identifier("static", ArrayLanguage::CSharp) == "static_");
  CHECK(array_identifier("static", ArrayLanguage::Rust) == "STATIC");
  CHECK(array_identifier("classic", ArrayLanguage::Java) == "classic");
}

TEST_CASE("encoding: array literal line length option") {
  const Bytes data = test::random_bytes(30, 6);
  const auto element_lines = [&](std::string_view options) {
    const std::string text = convert(data, "bin", "js-array", options).text();
    std::vector<std::size_t> counts;
    for (const std::string_view line : util::split_lines(text)) {
      if (line.starts_with("  0x")) counts.push_back(count_of(line, "0x"));
    }
    return counts;
  };
  CHECK(element_lines({}) == std::vector<std::size_t>{12, 12, 6});
  CHECK(element_lines(R"({"bytes_per_line":8})") == std::vector<std::size_t>{8, 8, 8, 6});
  CHECK(element_lines(R"({"bytes_per_line":30})") == std::vector<std::size_t>{30});
  CHECK(element_lines(R"({"bytes_per_line":1})") == std::vector<std::size_t>(30, 1));
  // The engine clamps the value to 1..64.
  CHECK(element_lines(R"({"bytes_per_line":0})") == std::vector<std::size_t>(30, 1));
  CHECK(element_lines(R"({"bytes_per_line":-3})") == std::vector<std::size_t>(30, 1));
  CHECK(element_lines(R"({"bytes_per_line":1000})") == std::vector<std::size_t>{30});
  const Bytes large = test::random_bytes(200, 6);
  const std::string widest = convert(large, "bin", "java-array", R"({"bytes_per_line":1000})").text();
  CHECK(count_of(util::split_lines(widest)[1], "0x") == 64);
  CHECK(convert(data, "bin", "js-array", R"({"bytes_per_line":"many"})").status == Status::InvalidArgument);

  // Every byte value appears, in order, in every language.
  const Bytes all = test::all_byte_values();
  for (const LanguageCase& language : languages) {
    const std::string text = array_literal(all, language.language, "all", 16, "");
    CHECK(count_of(text, "0x") == 256);
    CHECK(text.find("0x00, 0x01, 0x02") != std::string::npos);
    CHECK(text.find(language.language == ArrayLanguage::Java ? "(byte) 0xfe, (byte) 0xff,\n" : "0xfe, 0xff,\n") !=
          std::string::npos);
    CHECK(count_of(text, "(byte)") == (language.language == ArrayLanguage::Java ? 128u : 0u));
  }
}
