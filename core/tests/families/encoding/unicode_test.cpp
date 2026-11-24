// Tests of Unicode normalisation, code point views, IDNA (unicode.cpp) and the
// full HTML5 entity table (html_entities.cpp), against values computed with
// Python's unicodedata, html and encodings.idna (tests/data/encoding/unicode/SOURCES.md).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/encoding/encoding.hpp"
#include "families/encoding/unicode.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

using namespace cvt;
using encoding::NormalForm;
using encoding::normalize;
using test::convert;

namespace {

std::string from_hex_list(std::string_view list) {
  std::string out;
  for (const std::string_view part : util::split(list, ' ', false)) {
    util::append_utf8(out, static_cast<char32_t>(util::parse_int(part, 16).value_or(0xFFFD)));
  }
  return out;
}

}  // namespace

TEST_CASE("encoding/unicode-normalize: every code point of planes 0-2 matches unicodedata") {
  REQUIRE(encoding::unicode_data_version() == "16.0.0");
  const std::string expected = test::fixture_text("encoding/unicode/single_code_points.sha256");
  for (const auto& [name, form] : {std::pair{"NFC", NormalForm::Nfc}, std::pair{"NFD", NormalForm::Nfd},
                                   std::pair{"NFKC", NormalForm::Nfkc}, std::pair{"NFKD", NormalForm::Nfkd}}) {
    CAPTURE(name);
    util::Sha256 sha;
    std::string one;
    for (char32_t cp = 0; cp < 0x30000; ++cp) {
      if (cp >= 0xD800 && cp <= 0xDFFF) continue;
      one.clear();
      util::append_utf8(one, cp);
      const std::string normal = normalize(one, form) + "\n";
      sha.update(as_bytes(normal));
    }
    const auto digest = sha.finish();
    std::string hex;
    for (const std::uint8_t b : digest) hex += std::format("{:02x}", b);
    CHECK(expected.find(std::string(name) + " " + hex) != std::string::npos);
  }
}

TEST_CASE("encoding/unicode-normalize: combining sequences match unicodedata") {
  std::size_t checked = 0;
  const std::string text = test::fixture_text("encoding/unicode/sequences.txt");
  for (const std::string_view line : util::split_lines(text)) {
    const auto columns = util::split(line, ';', true);
    if (columns.size() != 5) continue;
    const std::string source = from_hex_list(columns[0]);
    INFO(line);
    CHECK(normalize(source, NormalForm::Nfc) == from_hex_list(columns[1]));
    CHECK(normalize(source, NormalForm::Nfd) == from_hex_list(columns[2]));
    CHECK(normalize(source, NormalForm::Nfkc) == from_hex_list(columns[3]));
    CHECK(normalize(source, NormalForm::Nfkd) == from_hex_list(columns[4]));
    ++checked;
  }
  CHECK(checked == 400);
  // Hangul, singletons and the engine route.
  CHECK(normalize("각", NormalForm::Nfc) == "각");
  CHECK(normalize("각", NormalForm::Nfd) == "각");
  CHECK(normalize("Å", NormalForm::Nfc) == "Å");  // ANGSTROM SIGN is a singleton
  CHECK(normalize("ﬁx²", NormalForm::Nfkc) == "fix2");
  CHECK(convert("é", "txt", "unicode-normalize").text() == "é");
  CHECK(convert("é", "txt", "unicode-normalize", R"({"form":"nfd"})").text() == "é");
  // A long run of combining marks is reordered in O(n log n), not quadratically.
  std::string marks = "a";
  for (int i = 0; i < 20000; ++i) marks += i % 2 == 0 ? "́" : "̣";
  CHECK(util::utf8_length(normalize(marks, NormalForm::Nfc)) == 20000);
}

TEST_CASE("encoding/codepoints and utf8-bytes") {
  using encoding::code_points;
  CHECK(code_points("Hi\U0001F600", "u-plus") == "U+0048 U+0069 U+1F600");
  CHECK(code_points("Hi", "hex") == "0x48 0x69");
  CHECK(code_points("Hi", "decimal") == "72 105");
  CHECK(code_points("H€", "html") == "&#x48;&#x20AC;");
  CHECK(code_points("H", "css") == "\\48 ");
  CHECK(code_points("H", "js") == "\\u{48}");
  CHECK(*encoding::parse_code_points("U+0048 U+0069, U+1F600") == "Hi\U0001F600");
  CHECK(*encoding::parse_code_points("&#x48;&#105;\\u{1F600}0x41 66") == "Hi\U0001F600AB");
  CHECK_STATUS(encoding::parse_code_points("U+D800"), Status::Decode);
  CHECK_STATUS(encoding::parse_code_points("U+110000"), Status::Decode);
  CHECK(convert("U+00E9", "codepoints", "txt").text() == "é");
  CHECK(encoding::utf8_byte_table("A€") == "U+0041  A  41\nU+20AC  €  E2 82 AC\n");
}

TEST_CASE("encoding/idna: UTS #46 basics against Python's IDNA 2003 codec") {
  const std::string text = test::fixture_text("encoding/unicode/idna.txt");
  for (const std::string_view line : util::split_lines(text)) {
    const auto columns = util::split(line, '\t', true);
    if (columns.size() != 2) continue;
    INFO(line);
    if (columns[0].find("\xC3\x9F") != std::string_view::npos) {
      // ß: IDNA 2003 maps it to "ss"; UTS #46 non-transitional processing (what browsers do now) keeps it.
      CHECK(encoding::idna_to_ascii(columns[0])->starts_with("xn--"));
      continue;
    }
    const auto ascii = encoding::idna_to_ascii(columns[0]);
    REQUIRE_OK(ascii);
    CHECK(*ascii == util::to_lower(columns[1]));
    const auto back = encoding::idna_to_unicode(*ascii);
    REQUIRE_OK(back);
    CHECK(*encoding::idna_to_ascii(*back) == *ascii);
  }
  CHECK(*encoding::idna_to_ascii("fa\xC3\x9F.de") == "xn--fa-hia.de");
  CHECK(*encoding::idna_to_unicode("xn--bcher-kva.example") == "b\xC3\xBC" "cher.example");
  CHECK_STATUS(encoding::idna_to_ascii("bad_label!.example"), Status::Decode);
  CHECK_STATUS(encoding::idna_to_ascii("-leading.example"), Status::Decode);
  CHECK_STATUS(encoding::idna_to_ascii(std::string(64, 'a') + ".example"), Status::Decode);
  CHECK(convert("b\xC3\xBC" "cher.example\n", "txt", "idna").text() == "xn--bcher-kva.example\n");
  CHECK(convert("xn--bcher-kva.example\n", "idna", "txt").text() == "b\xC3\xBC" "cher.example\n");
}

TEST_CASE("encoding/html-entities: the full HTML5 table, as html.unescape decodes it") {
  const std::string text = test::fixture_text("encoding/unicode/entities.txt");
  std::size_t checked = 0;
  for (const std::string_view line : util::split_lines(text)) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string_view::npos) continue;
    INFO(line);
    CHECK(encoding::html_entities_decode(line.substr(0, tab)) == from_hex_list(line.substr(tab + 1)));
    ++checked;
  }
  CHECK(checked == 34);
  CHECK(encoding::html_entities_encode("é → <", false, true) == "&eacute; &rarr; &lt;");
  CHECK(convert("café", "txt", "html-entities", R"({"named":true})").text() == "caf&eacute;");
  CHECK(convert("caf&eacute; &NotEqualTilde;", "html-entities", "txt").text() == "café ≂̸");
}
