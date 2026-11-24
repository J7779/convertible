// Tests of the text tools (text_tools.cpp) and the percent-encoding styles.
// Expected values of the URL styles come from Python 3's urllib.parse
// (quote with safe="", quote with the reserved set, quote, quote_plus).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/encoding/encoding.hpp"
#include "families/encoding/text_tools.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace std::string_view_literals;
using test::convert;

TEST_CASE("encoding/url-text: component, uri, path and form styles match urllib.parse") {
  constexpr std::string_view text = "a b/c?d=\xC3\xA9&e~f*(x)";
  CHECK(encoding::url_text_encode(text, encoding::UrlStyle::Component) == "a%20b%2Fc%3Fd%3D%C3%A9%26e~f%2A%28x%29");
  CHECK(encoding::url_text_encode(text, encoding::UrlStyle::Uri) == "a%20b/c?d=%C3%A9&e~f*(x)");
  CHECK(encoding::url_text_encode(text, encoding::UrlStyle::Path) == "a%20b/c%3Fd%3D%C3%A9%26e~f%2A%28x%29");
  CHECK(encoding::url_text_encode(text, encoding::UrlStyle::Form) == "a+b%2Fc%3Fd%3D%C3%A9%26e~f%2A%28x%29");
  CHECK(convert(text, "txt", "url-text", R"({"mode":"form"})").text() == "a+b%2Fc%3Fd%3D%C3%A9%26e~f%2A%28x%29");
  CHECK(convert(text, "txt", "url-text", R"({"mode":"path"})").text() == "a%20b/c%3Fd%3D%C3%A9%26e~f%2A%28x%29");
  CHECK(convert(text, "txt", "url-text", R"({"component":false})").text() == "a%20b/c?d=%C3%A9&e~f*(x)");
  CHECK(convert("a+b%2Fc", "url-text", "txt", R"({"plus_as_space":true})").text() == "a b/c");
}

TEST_CASE("encoding/text-case: every style") {
  using encoding::text_case;
  CHECK(text_case("Hello Wörld ÆØÅ ΣΊΣΥΦΟΣ Привет", "lower") == "hello wörld æøå σίσυφοσ привет");
  CHECK(text_case("straße ŝ ł", "upper") == "STRASSE Ŝ Ł");
  CHECK(text_case("the QUICK brown fox's tail", "title") == "The Quick Brown Fox's Tail");
  CHECK(text_case("hello. WORLD! how are you?", "sentence") == "Hello. World! How are you?");
  CHECK(text_case("parse HTTP response 2", "camel") == "parseHttpResponse2");
  CHECK(text_case("parseHTTPResponse", "snake") == "parse_http_response");
  CHECK(text_case("XMLHttpRequest", "kebab") == "xml-http-request");
  CHECK(text_case("max-retry count", "constant") == "MAX_RETRY_COUNT");
  CHECK(text_case("some_var_name\nother-one", "pascal") == "SomeVarName\nOtherOne");
  CHECK(text_case("AbC", "swap") == "aBc");
  CHECK(convert("Hello World", "txt", "text-case", R"({"case":"snake"})").text() == "hello_world");
  CHECK(convert("x", "txt", "text-case", R"({"case":"bogus"})").status == Status::InvalidArgument);
}

TEST_CASE("encoding/slug: transliteration and options") {
  CHECK(encoding::slugify("  Crème Brûlée: 10 Ways (2024)! ") == "creme-brulee-10-ways-2024");
  CHECK(encoding::slugify("Œuvre déjà vue — Łódź") == "oeuvre-deja-vue-lodz");
  CHECK(encoding::slugify("Don't stop", '_') == "dont_stop");
  CHECK(encoding::slugify("a very long title indeed", '-', 10) == "a-very-lon");
  CHECK(convert("First Post\nSecond, Post!\n", "txt", "slug").text() == "first-post\nsecond-post\n");
}

TEST_CASE("encoding/lines: sorting, uniqueness and cleanup") {
  constexpr std::string_view text = "file10\nFile2\nfile1\n\nfile2\nfile10\n";
  encoding::LineOptions natural;
  natural.sort = "natural";
  natural.ignore_case = true;
  natural.unique = true;
  natural.remove_empty = true;
  CHECK(encoding::transform_lines(text, natural) == "file1\nFile2\nfile10\n");
  CHECK(convert("b\na\nc\n", "txt", "lines").text() == "a\nb\nc\n");
  CHECK(convert("b\na\nc\n", "txt", "lines", R"({"sort":"desc"})").text() == "c\nb\na\n");
  CHECK(convert("b\na\nb\n", "txt", "lines", R"({"sort":"none","unique":true,"reverse":true})").text() == "a\nb\n");
  CHECK(convert("ccc\na\nbb", "txt", "lines", R"({"sort":"length"})").text() == "a\nbb\nccc");
  CHECK(convert("  x  \n\n y\n", "txt", "lines", R"({"sort":"none","trim":true,"remove_empty":true})").text() == "x\ny\n");
}

TEST_CASE("encoding/whitespace: tabs, trailing blanks, final newline") {
  CHECK(convert("a\tb\n\tc", "txt", "whitespace").text() == "a   b\n    c");
  CHECK(convert("a\tb", "txt", "whitespace", R"({"tab_width":8})").text() == "a       b");
  CHECK(convert("        x\n    y \t", "txt", "whitespace", R"({"tabs":"unexpand","trim_trailing":true})").text() ==
        "\t\tx\n\ty");
  CHECK(convert("x", "txt", "whitespace", R"({"tabs":"keep","final_newline":"add"})").text() == "x\n");
  CHECK(convert("x\n\n\n", "txt", "whitespace", R"({"tabs":"keep","final_newline":"remove"})").text() == "x");
  CHECK(convert("é\tx", "txt", "whitespace").text() == "é   x");  // columns count characters, not bytes
}

TEST_CASE("encoding/caesar, vigenere, atbash, a1z26: classic ciphers") {
  CHECK(encoding::caesar("Hello, World!", 13) == "Uryyb, Jbeyq!");  // Python codecs rot13
  CHECK(encoding::caesar("xyz ABC", 3) == "abc DEF");
  CHECK(convert("Khoor", "caesar", "txt").text() == "Hello");
  CHECK(convert("Hello", "txt", "caesar", R"({"shift":1})").text() == "Ifmmp");
  // The classic textbook example: ATTACKATDAWN with key LEMON.
  CHECK(*encoding::vigenere("ATTACKATDAWN", "LEMON", false) == "LXFOPVEFRNHR");
  CHECK(*encoding::vigenere("LXFOPVEFRNHR", "LEMON", true) == "ATTACKATDAWN");
  CHECK(convert("attack at dawn", "txt", "vigenere", R"({"key":"lemon"})").text() == "lxfopv ef rnhr");
  CHECK(convert("x", "txt", "vigenere", R"({"key":"123"})").status == Status::InvalidArgument);
  CHECK(encoding::atbash("Hello") == "Svool");
  CHECK(encoding::a1z26_encode("Hi there") == "8-9 20-8-5-18-5");
  CHECK(*encoding::a1z26_decode("8-9 20-8-5-18-5") == "hi there");
  CHECK_STATUS(encoding::a1z26_decode("27"), Status::Decode);
}

TEST_CASE("encoding/braille: Grade 1 cells, capitals and numbers") {
  CHECK(encoding::braille_encode("abc") == "⠁⠃⠉");
  CHECK(encoding::braille_encode("Hi 42.") == "⠠⠓⠊⠀⠼⠙⠃⠲");
  CHECK(*encoding::braille_decode("⠠⠓⠊⠀⠼⠙⠃⠲") == "Hi 42.");
  CHECK(*encoding::braille_decode(encoding::braille_encode("The year 2025, really?")) == "The year 2025, really?");
  std::vector<std::string> warnings;
  const auto out = test::write_from("braille", ir::PlainText{"a€b", "", false}, {}, &warnings);
  REQUIRE_OK(out);
  CHECK(warnings.size() == 1);
  CHECK_STATUS(encoding::braille_decode("x"), Status::Decode);
}
