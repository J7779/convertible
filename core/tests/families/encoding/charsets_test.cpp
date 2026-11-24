// Tests of the legacy character sets and the encoding detector (charsets.cpp,
// txt.cpp). Fixtures are sample texts encoded by Python's codecs
// (tests/data/encoding/charsets/SOURCES.md); the tables were also checked
// exhaustively against Python while developing (see that file).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <format>
#include <string>
#include <string_view>

#include "families/encoding/charsets.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace std::string_view_literals;
using test::convert;
using encoding::CharsetInfo;

namespace {

// Python codec names used for the fixture files, mapped to our labels.
std::string label_of(std::string_view codec) {
  if (codec == "cp932") return "windows-31j";
  if (codec == "cp037") return "ibm037";
  if (codec == "cp500") return "ibm500";
  if (codec == "cp437") return "ibm437";
  if (codec == "cp850") return "ibm850";
  if (codec == "cp852") return "ibm852";
  if (codec == "cp866") return "ibm866";
  if (codec == "mac_roman") return "macintosh";
  return std::string(codec);  // find_charset() understands the others as aliases ("cp1251", "euc_jp", ...)
}

}  // namespace

TEST_CASE("encoding/txt: legacy charsets decode and encode exactly like Python's codecs") {
  const std::string index = test::fixture_text("encoding/charsets/index.txt");
  std::size_t checked = 0;
  for (const std::string_view line : util::split_lines(index)) {
    const auto parts = util::split(line, ' ', false);
    if (parts.size() != 2) continue;
    const std::string label = label_of(parts[0]);
    INFO(parts[0], " -> ", label);
    const Bytes encoded = test::fixture("encoding/charsets/" + std::string(parts[0]) + ".txt");
    const std::string expected = test::fixture_text("encoding/charsets/" + std::string(parts[1]) + ".utf8");
    const CharsetInfo* info = encoding::find_charset(label);
    REQUIRE(info != nullptr);
    const encoding::CharsetText decoded = encoding::charset_decode(encoded, *info);
    CHECK(decoded.text == expected);
    CHECK(decoded.replaced == 0);
    const encoding::CharsetBytes back = encoding::charset_encode(expected, *info);
    CHECK(back.bytes == encoded);
    CHECK(back.replaced == 0);
    // Through the engine: txt with source_encoding / encoding.
    CHECK(convert(encoded, "txt", "txt", std::format(R"({{"source_encoding":"{}"}})", info->id)).text() == expected);
    CHECK(convert(expected, "txt", "txt", std::format(R"({{"encoding":"{}"}})", info->id)).data == encoded);
    ++checked;
  }
  CHECK(checked == 24);
}

TEST_CASE("encoding/txt: every table entry round-trips and GB18030 covers all of Unicode") {
  for (const CharsetInfo& info : encoding::legacy_charsets()) {
    CAPTURE(info.id);
    // Decode every single byte and every byte pair, then encode what came out.
    std::string all;
    for (unsigned lead = 0; lead < 256; ++lead) {
      for (unsigned trail = (info.id.size() > 0 && lead >= 0x81 ? 0x40u : 0x100u); trail < 0x100; ++trail) {
        const std::array<std::byte, 2> pair = {static_cast<std::byte>(lead), static_cast<std::byte>(trail)};
        const encoding::CharsetText d = encoding::charset_decode(pair, info);
        if (d.replaced == 0 && info.id != "iso-2022-jp") all += d.text + "\n";
      }
      const std::array<std::byte, 1> single = {static_cast<std::byte>(lead)};
      const encoding::CharsetText d = encoding::charset_decode(single, info);
      if (d.replaced == 0 && lead != '\n' && lead != 0x1B) all += d.text + "\n";
    }
    const encoding::CharsetBytes encoded = encoding::charset_encode(all, info);
    CHECK(encoded.replaced == 0);
    CHECK(encoding::charset_decode(encoded.bytes, info).text == all);
  }
  const CharsetInfo* gb = encoding::find_charset("GB18030");
  REQUIRE(gb != nullptr);
  std::string every;
  for (char32_t cp = 0x80; cp < 0x30000; cp += (cp < 0x10000 ? 1 : 97)) {
    if (cp >= 0xD800 && cp < 0xE000) continue;
    util::append_utf8(every, cp);
  }
  const encoding::CharsetBytes encoded = encoding::charset_encode(every, *gb);
  CHECK(encoded.replaced == 0);
  CHECK(encoding::charset_decode(encoded.bytes, *gb).text == every);
  // Published four-byte examples: U+0080 = 81 30 81 30, U+FFFF = 84 31 A4 39, U+10000 = 90 30 81 30.
  CHECK(encoding::charset_encode("\u0080", *gb).bytes == test::bytes("\x81\x30\x81\x30"));
  CHECK(encoding::charset_encode("￿", *gb).bytes == test::bytes("\x84\x31\xA4\x39"));
  CHECK(encoding::charset_encode("\U00010000", *gb).bytes == test::bytes("\x90\x30\x81\x30"));
}

TEST_CASE("encoding/txt: labels, EBCDIC 1047 and unmappable characters") {
  CHECK(encoding::find_charset("CP1251")->id == "windows-1251");
  CHECK(encoding::find_charset("x-mac-roman")->id == "macintosh");
  CHECK(encoding::find_charset("Shift-JIS")->id == "shift_jis");
  CHECK(encoding::find_charset("latin-2")->id == "iso-8859-2");
  CHECK(encoding::find_charset("klingon") == nullptr);
  // IBM-1047 differs from IBM-037 in six positions (checked against glibc iconv).
  const CharsetInfo* ibm1047 = encoding::find_charset("ibm1047");
  REQUIRE(ibm1047 != nullptr);
  CHECK(encoding::charset_encode("[]^\xC2\xAC", *ibm1047).bytes == test::bytes("\xAD\xBD\x5F\xB0"));
  CHECK(encoding::charset_encode("Hello", *ibm1047).bytes == test::bytes("\xC8\x85\x93\x93\x96"));
  std::vector<std::string> warnings;
  const auto out = test::write_from("txt", ir::PlainText{"a€日", "", false}, R"({"encoding":"koi8-r"})", &warnings);
  REQUIRE_OK(out);
  CHECK(test::str(*out) == "a??");
  CHECK(warnings.size() == 1);
  CHECK(convert("x", "txt", "txt", R"({"encoding":"shift_jis","bom":true})").warnings.size() == 1);
}

TEST_CASE("encoding/charset-detect: the detector finds the language-specific charsets") {
  const auto best = [](std::string_view file) {
    return encoding::detect_charsets(test::fixture("encoding/charsets/" + std::string(file))).front();
  };
  CHECK(best("shift_jis.txt").id == "shift_jis");
  CHECK(best("euc_jp.txt").id == "euc-jp");
  CHECK(best("gbk.txt").id == "gbk");
  CHECK(best("big5.txt").id == "big5");
  CHECK(best("euc_kr.txt").id == "euc-kr");
  CHECK(best("cp1251.txt").id == "windows-1251");
  CHECK(best("koi8_r.txt").id == "koi8-r");
  CHECK(best("cp1253.txt").id == "windows-1253");
  CHECK(best("shift_jis.txt").confidence >= 50);
  CHECK(encoding::detect_charsets(as_bytes("plain ascii"sv)).front().id == "utf-8");
  CHECK(encoding::detect_charsets(as_bytes("caf\xC3\xA9"sv)).front().id == "utf-8");
  CHECK(encoding::detect_charsets(test::bytes("\xFF\xFEh\0i\0")).front().id == "utf-16le");
  // Auto-detection in the txt reader uses a clear winner and says so.
  std::vector<std::string> warnings;
  const auto text = test::read_as<ir::PlainText>("txt", test::fixture("encoding/charsets/shift_jis.txt"), {}, &warnings);
  REQUIRE_OK(text);
  CHECK(text->text == test::fixture_text("encoding/charsets/ja.utf8"));
  CHECK(text->source_encoding == "shift_jis");
  REQUIRE(warnings.size() == 1);
  CHECK(warnings[0].find("shift_jis") != std::string::npos);
  const std::string report = convert(test::fixture("encoding/charsets/big5.txt"), "bin", "charset-detect").text();
  CHECK(report.starts_with("Most likely encoding: big5 ("));
  CHECK(test::fuzz_reader<ir::PlainText>("txt", test::fixture("encoding/charsets/gb18030.txt")) > 0);
}
