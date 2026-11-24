// Tests for the text transforms: HTML entities, percent-encoded text,
// Unicode escapes, JSON string literals, ROT13 and ROT47.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/encoding/encoding.hpp"
#include "support/test_support.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace cvt::encoding;
using namespace std::string_view_literals;
using test::convert;

namespace {

template <class T>
Status status_of(const Result<T>& result) { return result ? Status::Ok : result.error().code; }

// UTF-8 samples used throughout: e-acute (2 bytes), euro sign (3), grinning face (4).
constexpr std::string_view e_acute = "\xC3\xA9";
constexpr std::string_view euro = "\xE2\x82\xAC";
constexpr std::string_view grin = "\xF0\x9F\x98\x80";

std::string mixed() { return "caf" + std::string(e_acute) + " " + std::string(euro) + "5 " + std::string(grin); }

}  // namespace

// --------------------------------------------------------------------------- HTML entities

TEST_CASE("encoding: html-entities encoding") {
  CHECK(html_entities_encode("<a href=\"x\">Tom & Jerry's</a>", false) ==
        "&lt;a href=&quot;x&quot;&gt;Tom &amp; Jerry&#39;s&lt;/a&gt;");
  CHECK(html_entities_encode("plain text, nothing special", false) == "plain text, nothing special");
  CHECK(html_entities_encode("", true).empty());
  CHECK(html_entities_encode(mixed(), false) == mixed());
  CHECK(html_entities_encode(mixed(), true) == "caf&#xE9; &#x20AC;5 &#x1F600;");
  CHECK(html_entities_encode("&amp;", false) == "&amp;amp;");
  // C1 controls cannot be written as references (HTML reads them as Windows-1252), so they stay.
  CHECK(html_entities_encode("a\xC2\x85" "b", true) == "a\xC2\x85" "b");

  CHECK(convert("a < b", "txt", "html-entities").text() == "a &lt; b");
  CHECK(convert(mixed(), "txt", "html-entities", R"({"non_ascii":true})").text() == "caf&#xE9; &#x20AC;5 &#x1F600;");
}

TEST_CASE("encoding: html-entities decoding") {
  CHECK(html_entities_decode("&lt;&gt;&amp;&quot;&apos;") == "<>&\"'");
  CHECK(html_entities_decode("&#8364;&#x20AC;&#X20ac;&euro;") == std::string(euro) + std::string(euro) + std::string(euro) + std::string(euro));
  CHECK(html_entities_decode("&#128512;&#x1F600;") == std::string(grin) + std::string(grin));
  CHECK(html_entities_decode("&#0038;&#x000026;") == "&&");  // leading zeros
  CHECK(html_entities_decode("&amp;lt;") == "&lt;");         // one level only
  CHECK(html_entities_decode("no references here") == "no references here");
  CHECK(html_entities_decode("").empty());

  // A selection of the HTML 4 names: Latin-1, Greek, punctuation, symbols, arrows.
  const std::pair<std::string_view, std::string_view> named[] = {
      {"&nbsp;", "\xC2\xA0"},       {"&copy;", "\xC2\xA9"},       {"&reg;", "\xC2\xAE"},        {"&eacute;", "\xC3\xA9"},
      {"&Eacute;", "\xC3\x89"},     {"&uuml;", "\xC3\xBC"},       {"&szlig;", "\xC3\x9F"},      {"&yuml;", "\xC3\xBF"},
      {"&AElig;", "\xC3\x86"},      {"&alpha;", "\xCE\xB1"},      {"&Alpha;", "\xCE\x91"},      {"&omega;", "\xCF\x89"},
      {"&Omega;", "\xCE\xA9"},      {"&thetasym;", "\xCF\x91"},   {"&mdash;", "\xE2\x80\x94"},  {"&ndash;", "\xE2\x80\x93"},
      {"&hellip;", "\xE2\x80\xA6"}, {"&laquo;", "\xC2\xAB"},      {"&raquo;", "\xC2\xBB"},      {"&lsquo;", "\xE2\x80\x98"},
      {"&rdquo;", "\xE2\x80\x9D"},  {"&trade;", "\xE2\x84\xA2"},  {"&rarr;", "\xE2\x86\x92"},   {"&lArr;", "\xE2\x87\x90"},
      {"&larr;", "\xE2\x86\x90"},   {"&hearts;", "\xE2\x99\xA5"}, {"&infin;", "\xE2\x88\x9E"},  {"&ne;", "\xE2\x89\xA0"},
      {"&sum;", "\xE2\x88\x91"},    {"&frac12;", "\xC2\xBD"},     {"&sup2;", "\xC2\xB2"},       {"&there4;", "\xE2\x88\xB4"},
      {"&OElig;", "\xC5\x92"},      {"&zwj;", "\xE2\x80\x8D"},    {"&Dagger;", "\xE2\x80\xA1"}, {"&dagger;", "\xE2\x80\xA0"},
  };
  for (const auto& [reference, character] : named) CHECK_MESSAGE(html_entities_decode(reference) == character, reference);

  // Anything that is not a known reference is left untouched.
  for (const std::string_view untouched : {"&bogus;", "&;", "&#;", "&#x;", "&#xZZ;", "&#12a;", "& ", "&", "AT&T", "a&b;c",
                                           "&EACUTE;", "&thisnameisfartoolongtobeanentityreference;"}) {
    CHECK_MESSAGE(html_entities_decode(untouched) == untouched, untouched);
  }
  CHECK(html_entities_decode("&&amp;&") == "&&&");
  CHECK(html_entities_decode("&amp") == "&");  // HTML5 legacy name without the semicolon
  CHECK(html_entities_decode("&lt;&bogus;&gt;") == "<&bogus;>");

  // Code points that are not characters become U+FFFD; 0x80..0x9F map through Windows-1252.
  CHECK(html_entities_decode("&#0;&#xD800;&#x110000;&#99999999999999999999;") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
  CHECK(html_entities_decode("&#150;&#x80;&#146;") == "\xE2\x80\x93" + std::string(euro) + "\xE2\x80\x99");
  CHECK(html_entities_decode("&#x81;") == "\xC2\x81");

  CHECK(convert("&lt;b&gt; &eacute;", "html-entities", "txt").text() == "<b> " + std::string(e_acute));
}

TEST_CASE("encoding: html-entities round trip") {
  const std::string samples[] = {mixed(), "<&>\"'", "&amp; &lt; &#39; &#x26; &bogus;", "a\xC2\x85" "b", "5 > 3 && 2 < 4"};
  for (const std::string& sample : samples) {
    for (const bool non_ascii : {false, true}) {
      CHECK(html_entities_decode(html_entities_encode(sample, non_ascii)) == sample);
    }
  }
}

// --------------------------------------------------------------------------- percent-encoded text

TEST_CASE("encoding: url-text") {
  const std::string_view url = "https://example.com/a b?q=caf\xC3\xA9&x=1#top";
  CHECK(url_text_encode(url, true) == "https%3A%2F%2Fexample.com%2Fa%20b%3Fq%3Dcaf%C3%A9%26x%3D1%23top");
  CHECK(url_text_encode(url, false) == "https://example.com/a%20b?q=caf%C3%A9&x=1#top");
  CHECK(url_text_encode("AZaz09-._~", true) == "AZaz09-._~");
  CHECK(url_text_encode(":/?#[]@!$&'()*+,;=", false) == ":/?#[]@!$&'()*+,;=");
  CHECK(url_text_encode(":/?#[]@!$&'()*+,;=", true) == "%3A%2F%3F%23%5B%5D%40%21%24%26%27%28%29%2A%2B%2C%3B%3D");
  CHECK(url_text_encode("100% \"<>\\^`{|}\n", false) == "100%25%20%22%3C%3E%5C%5E%60%7B%7C%7D%0A");
  CHECK(url_text_encode(grin, true) == "%F0%9F%98%80");
  CHECK(url_text_encode("", true).empty());

  CHECK(*url_text_decode("a%20b%3Fq%3Dcaf%C3%A9", false) == "a b?q=caf\xC3\xA9");
  CHECK(*url_text_decode("a%2fb%2Fc", false) == "a/b/c");  // either case
  CHECK(*url_text_decode("a+b", false) == "a+b");
  CHECK(*url_text_decode("a+b%2B", true) == "a b+");
  CHECK(*url_text_decode("one%20line\ntwo%20lines\n", false) == "one line\ntwo lines\n");  // line structure is kept
  CHECK(*url_text_decode("%E9t%C3%A9", false) == "\xEF\xBF\xBDt\xC3\xA9");                    // not UTF-8: replaced
  for (const std::string_view bad : {"100%", "%4", "%zz", "%4g", "abc%", "%%20"}) {
    CHECK_MESSAGE(status_of(url_text_decode(bad, false)) == Status::Decode, "should reject: ", bad);
  }

  CHECK(convert(url, "txt", "url-text").text() == "https%3A%2F%2Fexample.com%2Fa%20b%3Fq%3Dcaf%C3%A9%26x%3D1%23top");
  CHECK(convert(url, "txt", "url-text", R"({"component":false})").text() == "https://example.com/a%20b?q=caf%C3%A9&x=1#top");
  CHECK(convert("a+b%20c", "url-text", "txt").text() == "a+b c");
  CHECK(convert("a+b%20c", "url-text", "txt", R"({"plus_as_space":true})").text() == "a b c");
  // The binary format is still there and still works on bytes.
  CHECK(convert("\xFF\x01", "bin", "url-encoded").text() == "%FF%01");
}

// --------------------------------------------------------------------------- Unicode escapes

TEST_CASE("encoding: unicode-escapes in four styles") {
  const std::string text = mixed();
  CHECK(unicode_escape(text, UnicodeEscapeStyle::U4, true) == "caf\\u00e9 \\u20ac5 \\ud83d\\ude00");
  CHECK(unicode_escape(text, UnicodeEscapeStyle::UBraces, true) == "caf\\u{e9} \\u{20ac}5 \\u{1f600}");
  CHECK(unicode_escape(text, UnicodeEscapeStyle::XBytes, true) == "caf\\xc3\\xa9 \\xe2\\x82\\xac5 \\xf0\\x9f\\x98\\x80");
  CHECK(unicode_escape(text, UnicodeEscapeStyle::Python, true) == "caf\\u00e9 \\u20ac5 \\U0001f600");
  CHECK(unicode_escape("plain ASCII stays", UnicodeEscapeStyle::U4, true) == "plain ASCII stays");
  CHECK(unicode_escape("", UnicodeEscapeStyle::U4, true).empty());

  // Control characters and the backslash.
  const std::string_view controls = "a\tb\r\nc\\d\x01\x7F\0e"sv;
  CHECK(unicode_escape(controls, UnicodeEscapeStyle::U4, true) == "a\\tb\\r\\nc\\\\d\\u0001\\u007f\\u0000e");
  CHECK(unicode_escape(controls, UnicodeEscapeStyle::UBraces, true) == "a\\tb\\r\\nc\\\\d\\u{1}\\u{7f}\\u{0}e");
  CHECK(unicode_escape(controls, UnicodeEscapeStyle::XBytes, true) == "a\\tb\\r\\nc\\\\d\\x01\\x7f\\x00e");
  CHECK(unicode_escape(controls, UnicodeEscapeStyle::Python, true) == "a\\tb\\r\\nc\\\\d\\u0001\\u007f\\u0000e");
  CHECK(unicode_escape(controls, UnicodeEscapeStyle::U4, false) == "a\tb\r\nc\\\\d\x01\x7F\0e"sv);

  for (const UnicodeEscapeStyle style :
       {UnicodeEscapeStyle::U4, UnicodeEscapeStyle::UBraces, UnicodeEscapeStyle::XBytes, UnicodeEscapeStyle::Python}) {
    for (const bool escape_controls : {true, false}) {
      for (const std::string& sample : {text, std::string(controls), std::string("C:\\new\\table \\u0041 \\\\")}) {
        const std::string escaped = unicode_escape(sample, style, escape_controls);
        const auto back = unicode_unescape(escaped);
        REQUIRE(back);
        CHECK(*back == sample);
        if (escape_controls) {
          for (const char c : escaped) CHECK((c >= 0x20 && c < 0x7F));
        }
      }
    }
  }
}

TEST_CASE("encoding: unicode-escapes decoding") {
  CHECK(*unicode_unescape("caf\\u00E9 \\u20ac") == "caf\xC3\xA9 \xE2\x82\xAC");
  CHECK(*unicode_unescape("\\ud83d\\ude00 \\uD83D\\uDE00") == std::string(grin) + " " + std::string(grin));
  CHECK(*unicode_unescape("\\u{1F600}\\u{e9}\\u{0041}\\u{10FFFF}") == std::string(grin) + std::string(e_acute) + "A\xF4\x8F\xBF\xBF");
  CHECK(*unicode_unescape("\\U0001F600\\U000000e9") == std::string(grin) + std::string(e_acute));
  CHECK(*unicode_unescape("\\xf0\\x9f\\x98\\x80\\xC3\\xA9") == std::string(grin) + std::string(e_acute));
  // \x escapes that are not UTF-8 are read as Latin-1 characters.
  CHECK(*unicode_unescape("caf\\xe9!") == "caf\xC3\xA9!");
  CHECK(*unicode_unescape("\\x41\\x0a") == "A\n");
  CHECK(*unicode_unescape("\\n\\r\\t\\\\\\\"\\'\\/\\b\\f\\v") == "\n\r\t\\\"'/\b\f\v");
  CHECK(*unicode_unescape("a\\0b") == "a\0b"sv);
  CHECK(*unicode_unescape("no escapes, caf\xC3\xA9") == "no escapes, caf\xC3\xA9");
  CHECK(unicode_unescape("")->empty());
  // Lone surrogates cannot be represented and become U+FFFD.
  CHECK(*unicode_unescape("\\ud83d!") == "\xEF\xBF\xBD!");
  CHECK(*unicode_unescape("\\ude00\\ud83d") == "\xEF\xBF\xBD\xEF\xBF\xBD");
  CHECK(*unicode_unescape("\\ud83d\\u0041") == "\xEF\xBF\xBD" "A");

  for (const std::string_view bad : {"\\", "abc\\", "\\q", "\\u", "\\u12", "\\u12G4", "\\u{}", "\\u{110000}", "\\u{1F600",
                                     "\\u{1234567}", "\\u{12 34}", "\\U0001F60", "\\U00110000", "\\UFFFFFFFF", "\\x", "\\x4",
                                     "\\xZZ", "\\ ", "\\\xC3\xA9"}) {
    CHECK_MESSAGE(status_of(unicode_unescape(bad)) == Status::Decode, "should reject: ", bad);
  }
}

TEST_CASE("encoding: unicode-escapes through the engine") {
  const std::string text = mixed() + "\n";
  CHECK(convert(text, "txt", "unicode-escapes").text() == "caf\\u00e9 \\u20ac5 \\ud83d\\ude00\\n");
  CHECK(convert(text, "txt", "unicode-escapes", R"({"style":"u-braces"})").text() == "caf\\u{e9} \\u{20ac}5 \\u{1f600}\\n");
  CHECK(convert(text, "txt", "unicode-escapes", R"({"style":"x-bytes"})").text() ==
        "caf\\xc3\\xa9 \\xe2\\x82\\xac5 \\xf0\\x9f\\x98\\x80\\n");
  CHECK(convert(text, "txt", "unicode-escapes", R"({"style":"python"})").text() == "caf\\u00e9 \\u20ac5 \\U0001f600\\n");
  CHECK(convert(text, "txt", "unicode-escapes", R"({"escape_ascii_controls":false})").text() ==
        "caf\\u00e9 \\u20ac5 \\ud83d\\ude00\n");
  CHECK(convert(text, "txt", "unicode-escapes", R"({"style":"klingon"})").status == Status::InvalidArgument);
  CHECK(convert("caf\\u00e9\\n", "unicode-escapes", "txt").text() == "caf\xC3\xA9\n");
  CHECK(convert("caf\\u00", "unicode-escapes", "txt").status == Status::Decode);
}

// --------------------------------------------------------------------------- JSON string literal

TEST_CASE("encoding: json-string encoding") {
  CHECK(json_string_encode("", false) == "\"\"");
  CHECK(json_string_encode("plain", false) == "\"plain\"");
  CHECK(json_string_encode("He said \"hi\"\n\t\\ / \b\f\r", false) == "\"He said \\\"hi\\\"\\n\\t\\\\ / \\b\\f\\r\"");
  CHECK(json_string_encode("\x01\x1F\0"sv, false) == "\"\\u0001\\u001f\\u0000\"");
  CHECK(json_string_encode("\x7F", false) == "\"\x7F\"");
  CHECK(json_string_encode("\x7F", true) == "\"\\u007f\"");
  CHECK(json_string_encode(mixed(), false) == "\"" + mixed() + "\"");
  CHECK(json_string_encode(mixed(), true) == "\"caf\\u00e9 \\u20ac5 \\ud83d\\ude00\"");

  // The output is JSON that the JSON parser reads back to the same text.
  for (const std::string& sample : {mixed(), std::string("a\"b\\c\n\x01\x7F"), std::string("\0\0"sv), std::string()}) {
    for (const bool ascii_only : {false, true}) {
      const auto parsed = util::parse_json(json_string_encode(sample, ascii_only));
      REQUIRE(parsed);
      REQUIRE(parsed->is_string());
      CHECK(parsed->as_string() == sample);
    }
  }
  CHECK(convert("say \"hi\"\n", "txt", "json-string").text() == "\"say \\\"hi\\\"\\n\"");
  CHECK(convert(mixed(), "txt", "json-string", R"({"ascii_only":true})").text() == "\"caf\\u00e9 \\u20ac5 \\ud83d\\ude00\"");
}

TEST_CASE("encoding: json-string decoding is strict") {
  CHECK(*json_string_decode("\"\"") == "");
  CHECK(*json_string_decode("\"plain\"") == "plain");
  CHECK(*json_string_decode(" \t\r\n\"padded\"\n") == "padded");
  CHECK(*json_string_decode("\"\\\" \\\\ \\/ \\b \\f \\n \\r \\t\"") == "\" \\ / \b \f \n \r \t");
  CHECK(*json_string_decode("\"caf\\u00E9 \\u20ac \\ud83d\\ude00 \\u0000\"") == "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 \0"sv);
  CHECK(*json_string_decode("\"" + mixed() + "\"") == mixed());
  CHECK(*json_string_decode("\"it's\"") == "it's");

  for (const std::string_view bad : {
           "",                      // nothing
           "plain",                 // no quotes
           "'single'",              // wrong quotes
           "\"open",                // not closed
           "\"",                    // lone quote
           "\"a\" trailing",        // trailing garbage
           "\"a\"\"b\"",            // two literals
           "\"a\",",                // trailing comma
           "[\"a\"]",               // some other JSON value
           "\"line\nbreak\"",       // raw control character
           "\"tab\there\"",         // raw tab
           "\"bad \\q escape\"",    // unknown escape
           "\"bad \\' escape\"",    // not a JSON escape
           "\"\\u12\"",             // short \u
           "\"\\u12G4\"",           // not hexadecimal
           "\"\\ud83d\"",           // lone high surrogate
           "\"\\ud83d\\u0041\"",    // high surrogate without a low one
           "\"\\ude00\"",           // lone low surrogate
           "\"\xFF\"",              // invalid UTF-8
           "\"\xC3\"",              // truncated UTF-8
           "\"ends with\\",         // backslash at the end
           "\"ends with\\\"",       // escaped closing quote
           "\x0C\"form feed is not JSON white space\"",
       }) {
    CHECK_MESSAGE(status_of(json_string_decode(bad)) == Status::Decode, "should reject: ", bad);
  }
  CHECK(json_string_decode("\"a\" x").error().message.find("after the JSON string") != std::string::npos);

  CHECK(convert("\"say \\\"hi\\\"\\n\"\n", "json-string", "txt").text() == "say \"hi\"\n");
  CHECK(convert("\"a\" b", "json-string", "txt").status == Status::Decode);
}

// --------------------------------------------------------------------------- rotations

TEST_CASE("encoding: rot13 and rot47") {
  CHECK(rot13("Hello, World!") == "Uryyb, Jbeyq!");
  CHECK(rot13("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") == "NOPQRSTUVWXYZABCDEFGHIJKLMnopqrstuvwxyzabcdefghijklm");
  CHECK(rot13("123 caf\xC3\xA9 \n") == "123 pns\xC3\xA9 \n");  // only ASCII letters move
  CHECK(rot13("").empty());
  CHECK(rot47("Hello, World!") == "w6==@[ (@C=5P");
  CHECK(rot47("!~ \t\n") == "PO \t\n");
  CHECK(rot47("caf\xC3\xA9") == "427\xC3\xA9");

  std::string printable;
  for (char c = ' '; c <= '~'; ++c) printable.push_back(c);
  CHECK(rot13(rot13(printable)) == printable);
  CHECK(rot47(rot47(printable)) == printable);
  CHECK(rot13(rot13(mixed())) == mixed());
  CHECK(rot47(rot47(mixed())) == mixed());

  // Both directions of the codec are the same function.
  CHECK(convert("Hello, World!", "txt", "rot13").text() == "Uryyb, Jbeyq!");
  CHECK(convert("Uryyb, Jbeyq!", "rot13", "txt").text() == "Hello, World!");
  CHECK(convert("Hello, World!", "txt", "rot47").text() == "w6==@[ (@C=5P");
  CHECK(convert("w6==@[ (@C=5P", "rot47", "txt").text() == "Hello, World!");
  CHECK(convert("Hello", "rot13", "rot13").text() == "Hello");  // decode, then encode again
}
