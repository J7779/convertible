// Tests for punycode, Morse code and the NATO phonetic alphabet.
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

template <class T>
Status status_of(const Result<T>& result) { return result ? Status::Ok : result.error().code; }

struct Sample {
  std::u32string_view text;
  std::string_view punycode;
};

// RFC 3492 section 7.1. Samples (I), (L)-(Q) appear in the RFC with
// mixed-case annotation; plain punycode keeps basic code points as they are.
constexpr Sample rfc3492_samples[] = {
    // (A) Arabic (Egyptian)
    {U"\u0644\u064A\u0647\u0645\u0627\u0628\u062A\u0643\u0644\u0645\u0648\u0634\u0639\u0631\u0628\u064A\u061F",
     "egbpdaj6bu4bxfgehfvwxn"},
    // (B) Chinese (simplified)
    {U"\u4ED6\u4EEC\u4E3A\u4EC0\u4E48\u4E0D\u8BF4\u4E2D\u6587", "ihqwcrb4cv8a8dqg056pqjye"},
    // (C) Chinese (traditional)
    {U"\u4ED6\u5011\u7232\u4EC0\u9EBD\u4E0D\u8AAA\u4E2D\u6587", "ihqwctvzc91f659drss3x8bo0yb"},
    // (D) Czech
    {U"Pro\u010Dprost\u011Bnemluv\u00ED\u010Desky", "Proprostnemluvesky-uyb24dma41a"},
    // (E) Hebrew
    {U"\u05DC\u05DE\u05D4\u05D4\u05DD\u05E4\u05E9\u05D5\u05D8\u05DC\u05D0\u05DE\u05D3\u05D1\u05E8\u05D9\u05DD\u05E2\u05D1"
     U"\u05E8\u05D9\u05EA",
     "4dbcagdahymbxekheh6e0a7fei0b"},
    // (H) Korean
    {U"\uC138\uACC4\uC758\uBAA8\uB4E0\uC0AC\uB78C\uB4E4\uC774\uD55C\uAD6D\uC5B4\uB97C\uC774\uD574\uD55C\uB2E4\uBA74\uC5BC"
     U"\uB9C8\uB098\uC88B\uC744\uAE4C",
     "989aomsvi5e83db1d2a355cv1e0vak1dwrv93d5xbh15a0dt30a5jpsd879ccm6fea98c"},
    // (I) Russian (Cyrillic)
    {U"\u043F\u043E\u0447\u0435\u043C\u0443\u0436\u0435\u043E\u043D\u0438\u043D\u0435\u0433\u043E\u0432\u043E\u0440\u044F"
     U"\u0442\u043F\u043E\u0440\u0443\u0441\u0441\u043A\u0438",
     "b1abfaaepdrnnbgefbadotcwatmq2g4l"},
    // (J) Spanish
    {U"Porqu\u00E9nopuedensimplementehablarenEspa\u00F1ol", "PorqunopuedensimplementehablarenEspaol-fmd56a"},
    // (L) 3<nen>B<gumi><kinpachi><sensei>
    {U"3\u5E74B\u7D44\u91D1\u516B\u5148\u751F", "3B-ww4c5e180e575a65lsy2b"},
    // (M) <amuro><namie>-with-SUPER-MONKEYS
    {U"\u5B89\u5BA4\u5948\u7F8E\u6075-with-SUPER-MONKEYS", "-with-SUPER-MONKEYS-pc58ag80a8qai00g7n9n"},
    // (N) Hello-Another-Way-<sorezore><no><basho>
    {U"Hello-Another-Way-\u305D\u308C\u305E\u308C\u306E\u5834\u6240", "Hello-Another-Way--fc4qua05auwb3674vfr0b"},
    // (O) <hitotsu><yane><no><shita>2
    {U"\u3072\u3068\u3064\u5C4B\u6839\u306E\u4E0B" U"2", "2-u9tlzr9756bt3uc0v"},
    // (P) Maji<de>Koi<suru>5<byou><mae>
    {U"Maji\u3067Koi\u3059\u308B" U"5\u79D2\u524D", "MajiKoi5-783gue6qz075azm5e"},
    // (Q) <pafii>de<runba>
    {U"\u30D1\u30D5\u30A3\u30FCde\u30EB\u30F3\u30D0", "de-jg4avhby1noc0d"},
    // (R) <sono><supiido><de>
    {U"\u305D\u306E\u30B9\u30D4\u30FC\u30C9\u3067", "d9juau41awczczp"},
    // (S) -> $1.00 <-
    {U"-> $1.00 <-", "-> $1.00 <--"},
};

// A decoded label as UTF-8, or the error message.
std::string decoded_label(std::string_view label) {
  const auto decoded = punycode_decode_label(label);
  return decoded ? util::utf32_to_utf8(*decoded) : "error: " + decoded.error().message;
}

}  // namespace

// --------------------------------------------------------------------------- punycode

TEST_CASE("encoding: punycode RFC 3492 sample strings") {
  for (const Sample& sample : rfc3492_samples) {
    CAPTURE(sample.punycode);
    const auto encoded = punycode_encode_label(sample.text);
    REQUIRE(encoded);
    CHECK(*encoded == sample.punycode);
    const auto decoded = punycode_decode_label(sample.punycode);
    REQUIRE(decoded);
    CHECK(util::utf32_to_utf8(*decoded) == util::utf32_to_utf8(sample.text));
  }
}

TEST_CASE("encoding: punycode labels") {
  CHECK(*punycode_encode_label(U"b\u00FCcher") == "bcher-kva");
  CHECK(*punycode_encode_label(U"m\u00FCnchen") == "mnchen-3ya");
  CHECK(*punycode_encode_label(U"\u65E5\u672C\u8A9E") == "wgv71a119e");
  CHECK(*punycode_encode_label(U"\U0001F600") == "e28h");
  CHECK(*punycode_encode_label(U"abc") == "abc-");  // RFC 3492 always writes the delimiter after basic characters
  CHECK(punycode_encode_label(U"")->empty());
  CHECK(decoded_label("bcher-kva") == "b\xC3\xBC" "cher");
  CHECK(decoded_label("BCHER-KVA") == "B\xC3\xBC" "CHER");  // digits are case-insensitive, letters are kept
  CHECK(decoded_label("e28h") == "\xF0\x9F\x98\x80");
  CHECK(decoded_label("abc-") == "abc");
  CHECK(punycode_decode_label("")->empty());

  for (const std::string_view bad : {
           "bcher-kv",         // ends inside a number
           "bcher-kva!",       // not a digit
           "b\xC3\xBC" "cher-kva",  // non-ASCII before the delimiter
           "kva\xC3\xBC",      // non-ASCII digit
           "-kva",             // a hyphen with nothing in front of it is not a delimiter
           "99999999999",      // a number that never ends
           "a-999999b",        // code point above U+10FFFF
       }) {
    CHECK_MESSAGE(status_of(punycode_decode_label(bad)) == Status::Decode, "should reject: ", bad);
  }

  // Labels beyond the size limit are refused instead of taking quadratic time.
  CHECK(punycode_encode_label(std::u32string(punycode_max_label, U'\u00FC')));
  CHECK(status_of(punycode_encode_label(std::u32string(punycode_max_label + 1, U'\u00FC'))) == Status::Limit);
  CHECK(status_of(punycode_decode_label(std::string(punycode_max_label + 1, 'a'))) == Status::Limit);
  CHECK(status_of(punycode_encode_label(std::u32string(1, static_cast<char32_t>(0xD800)))) == Status::Encode);
}

TEST_CASE("encoding: punycode text is converted label by label") {
  CHECK(*punycode_encode_text("b\xC3\xBC" "cher.example") == "xn--bcher-kva.example");
  CHECK(*punycode_encode_text("m\xC3\xBCnchen.de\n\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.jp\n") ==
        "xn--mnchen-3ya.de\nxn--wgv71a119e.jp\n");
  // ASCII letters of converted labels are lower-cased; pure ASCII labels are not touched.
  CHECK(*punycode_encode_text("B\xC3\xBC" "cher.Example.COM") == "xn--bcher-kva.Example.COM");
  CHECK(*punycode_encode_text("plain-ascii.example, nothing to do!") == "plain-ascii.example, nothing to do!");
  CHECK(punycode_encode_text("")->empty());
  // Labels end at any ASCII character that cannot be part of a host name.
  CHECK(*punycode_encode_text("https://www.b\xC3\xBC" "cher.de/pfad?x=1 und info@m\xC3\xBCnchen.de") ==
        "https://www.xn--bcher-kva.de/pfad?x=1 und info@xn--mnchen-3ya.de");

  CHECK(*punycode_decode_text("xn--bcher-kva.example") == "b\xC3\xBC" "cher.example");
  CHECK(*punycode_decode_text("XN--BCHER-KVA.EXAMPLE") == "B\xC3\xBC" "CHER.EXAMPLE");  // the prefix matches in any case
  CHECK(*punycode_decode_text("xn--mnchen-3ya.de\nxn--wgv71a119e.jp\n") == "m\xC3\xBCnchen.de\n\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.jp\n");
  CHECK(*punycode_decode_text("no prefixed labels here: axn--b") == "no prefixed labels here: axn--b");
  CHECK(punycode_decode_text("")->empty());

  for (const std::string_view bad : {"xn--", "www.xn--.de", "xn--bcher-kv.de", "xn--99999999999", "xn--b\xC3\xBC" "cher-kva"}) {
    CHECK_MESSAGE(status_of(punycode_decode_text(bad)) == Status::Decode, "should reject: ", bad);
  }
  CHECK(punycode_decode_text("a.xn--bcher-kv.de").error().message.find("xn--bcher-kv") != std::string::npos);

  // One very long word is a limit error, not a slow conversion.
  std::string long_word;
  for (std::size_t i = 0; i <= punycode_max_label; ++i) long_word += "\xC3\xBC";
  CHECK(status_of(punycode_encode_text(long_word)) == Status::Limit);
  CHECK(status_of(punycode_decode_text("xn--" + std::string(punycode_max_label + 1, 'a'))) == Status::Limit);
}

TEST_CASE("encoding: punycode through the engine") {
  const test::Converted encoded = convert("www.b\xC3\xBC" "cher.de\n", "txt", "punycode");
  REQUIRE(encoded.ok());
  CHECK(encoded.text() == "www.xn--bcher-kva.de\n");
  CHECK(encoded.extension == "punycode");
  CHECK(convert(encoded.data, "punycode", "txt").text() == "www.b\xC3\xBC" "cher.de\n");
  CHECK(convert("xn--bcher-kv.de", "punycode", "txt").status == Status::Decode);
}

// --------------------------------------------------------------------------- Morse

TEST_CASE("encoding: morse encoding") {
  CHECK(morse_encode("SOS") == "... --- ...");
  CHECK(morse_encode("Hello World") == ".... . .-.. .-.. --- / .-- --- .-. .-.. -..");
  CHECK(morse_encode("abcdefghijklmnopqrstuvwxyz") ==
        ".- -... -.-. -.. . ..-. --. .... .. .--- -.- .-.. -- -. --- .--. --.- .-. ... - ..- ...- .-- -..- -.-- --..");
  CHECK(morse_encode("0123456789") == "----- .---- ..--- ...-- ....- ..... -.... --... ---.. ----.");
  CHECK(morse_encode(".,?'!/()&:;=+-_\"$@") ==
        ".-.-.- --..-- ..--.. .----. -.-.-- -..-. -.--. -.--.- .-... ---... -.-.-. -...- .-.-. -....- ..--.- .-..-. ...-..- "
        ".--.-.");
  // Runs of blanks are one word gap; line breaks are kept; blanks at line ends vanish.
  CHECK(morse_encode("  a   b \t c  \nd\r\n\ne ") == ".- / -... / -.-.\n-..\r\n\n.");
  CHECK(morse_encode("").empty());
  CHECK(morse_encode(" \n ") == "\n");

  // Characters without a code are skipped and counted.
  std::size_t skipped = 99;
  CHECK(morse_encode("a#b caf\xC3\xA9 \xF0\x9F\x98\x80", &skipped) == ".- -... / -.-. .- ..-.");
  CHECK(skipped == 3);
  CHECK(morse_encode("ok", &skipped) == "--- -.-");
  CHECK(skipped == 0);
}

TEST_CASE("encoding: morse decoding") {
  CHECK(*morse_decode("... --- ...") == "SOS");
  CHECK(*morse_decode(".... . .-.. .-.. --- / .-- --- .-. .-.. -..") == "HELLO WORLD");
  CHECK(*morse_decode("  ...   ---\t...  ") == "SOS");
  CHECK(*morse_decode(".-/-...") == "A B");            // no spaces around the word separator
  CHECK(*morse_decode(".- | -...") == "A B");          // alternative word separator
  CHECK(*morse_decode("/ .- / / -... /") == "A B");    // empty words collapse
  CHECK(*morse_decode(".-\r\n-...\n\n-.-.") == "A\nB\n\nC");
  CHECK(*morse_decode(".-.-.- --..-- ..--..") == ".,?");
  CHECK(morse_decode("")->empty());
  // Typographic dots and dashes: middle dot, bullet, minus sign, en dash, em dash.
  CHECK(*morse_decode("\xC2\xB7\xC2\xB7\xC2\xB7 \xE2\x88\x92\xE2\x88\x92\xE2\x88\x92 \xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2") == "SOS");
  CHECK(*morse_decode("\xE2\x80\x93.\xE2\x80\x94.") == "C");

  for (const std::string_view bad : {"........", "..--", ".- x -...", "SOS", ".-,-...", "... --- ... \xC3\xA9", "\xFF", "-.-.-.-.-"}) {
    CHECK_MESSAGE(status_of(morse_decode(bad)) == Status::Decode, "should reject: ", bad);
  }

  // Encoding upper-cases, so text survives a round trip up to letter case.
  const std::string_view text = "The quick brown fox, 42 times!\nSecond line: (a+b)=c";
  CHECK(*morse_decode(morse_encode(text)) == util::to_upper(text));
}

TEST_CASE("encoding: morse through the engine warns once about skipped characters") {
  const test::Converted clean = convert("sos", "txt", "morse");
  REQUIRE(clean.ok());
  CHECK(clean.text() == "... --- ...");
  CHECK(clean.warnings.empty());
  CHECK(clean.extension == "morse");

  const test::Converted lossy = convert("a # b % c ^ d", "txt", "morse");
  REQUIRE(lossy.ok());
  CHECK(lossy.text() == ".- / -... / -.-. / -..");
  REQUIRE(lossy.warnings.size() == 1);
  CHECK(lossy.warnings[0].find("3 character(s)") != std::string::npos);

  CHECK(convert("... --- ...\n", "morse", "txt").text() == "SOS\n");
  CHECK(convert("... --- ... !", "morse", "txt").status == Status::Decode);
  const Codec* codec = test::find_writer("morse", Kind::PlainText);
  REQUIRE(codec != nullptr);
  CHECK(codec->lossy);
}

// --------------------------------------------------------------------------- NATO alphabet

TEST_CASE("encoding: nato encoding") {
  CHECK(nato_encode("abc") == "Alfa Bravo Charlie");
  CHECK(nato_encode("Hi 12") == "Hotel India / One Two");
  CHECK(nato_encode("abcdefghijklmnopqrstuvwxyz") ==
        "Alfa Bravo Charlie Delta Echo Foxtrot Golf Hotel India Juliett Kilo Lima Mike November Oscar Papa Quebec Romeo "
        "Sierra Tango Uniform Victor Whiskey X-ray Yankee Zulu");
  CHECK(nato_encode("0123456789") == "Zero One Two Three Four Five Six Seven Eight Nine");
  CHECK(nato_encode("a b\nc") == "Alfa / Bravo\nCharlie");
  CHECK(nato_encode("").empty());
  std::size_t skipped = 0;
  CHECK(nato_encode("a-b! \xC3\xA9", &skipped) == "Alfa Bravo");
  CHECK(skipped == 3);
}

TEST_CASE("encoding: nato decoding") {
  CHECK(*nato_decode("Alfa Bravo Charlie") == "ABC");
  CHECK(*nato_decode("Hotel India / One Two") == "HI 12");
  CHECK(*nato_decode("alfa BRAVO cHaRlIe") == "ABC");                       // any case
  CHECK(*nato_decode("Alpha Juliet Xray X-Ray x-ray Whisky Niner") == "AJXXXW9");  // common spellings
  CHECK(*nato_decode("Alfa / Bravo\nCharlie\r\n\nDelta") == "A B\nC\n\nD");
  CHECK(*nato_decode("Alfa, Bravo,Charlie") == "ABC");
  CHECK(*nato_decode("  Alfa  /  / Bravo /") == "A B");
  CHECK(nato_decode("")->empty());

  for (const std::string_view bad : {"Alfa Beta", "Alf", "Alfa Bravo 3", "Alfa.Bravo", "Novemberish", "Alfa Br\xC3\xA1vo",
                                     "Supercalifragilistic", "-"}) {
    CHECK_MESSAGE(status_of(nato_decode(bad)) == Status::Decode, "should reject: ", bad);
  }
  CHECK(nato_decode("Alfa Beta").error().message.find("'Beta'") != std::string::npos);

  const std::string_view text = "Flight 370 to gate B12\nboarding now";
  CHECK(*nato_decode(nato_encode(text)) == util::to_upper(text));
}

TEST_CASE("encoding: nato through the engine") {
  const test::Converted encoded = convert("ok go!", "txt", "nato");
  REQUIRE(encoded.ok());
  CHECK(encoded.text() == "Oscar Kilo / Golf Oscar");
  REQUIRE(encoded.warnings.size() == 1);
  CHECK(encoded.warnings[0].find("1 character(s)") != std::string::npos);
  CHECK(convert(encoded.data, "nato", "txt").text() == "OK GO");
  CHECK(convert("Alfa Bravo Gamma", "nato", "txt").status == Status::Decode);
  const Codec* codec = test::find_writer("nato", Kind::PlainText);
  REQUIRE(codec != nullptr);
  CHECK(codec->lossy);
}
