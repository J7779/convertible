// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// util/charsets.hpp as the other families use it: labels and Windows code
// page numbers, and the automatic choice for text of unknown encoding, on
// fixtures written by Python's codecs (see tests/data/subtitle/SOURCES.md).
#include <string>
#include <string_view>

#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/charsets.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace std::string_view_literals;

namespace {

std::string labelled(ByteView data, std::string_view label) {
  const auto decoded = util::decode_labelled(data, label);
  return decoded ? decoded->text : std::string("<unknown label>");
}

}  // namespace

TEST_CASE("charsets: labels, aliases and code page numbers") {
  // "日本" in Shift_JIS, "中文" in GBK, "中文" in Big5, "한국" in EUC-KR.
  const Bytes sjis = test::bytes("\x93\xFA\x96\x7B"sv);
  const Bytes gbk = test::bytes("\xD6\xD0\xCE\xC4"sv);
  const Bytes big5 = test::bytes("\xA4\xA4\xA4\xE5"sv);
  const Bytes euckr = test::bytes("\xC7\xD1\xB1\xB9"sv);
  CHECK(labelled(sjis, "Shift_JIS") == "日本");
  CHECK(labelled(sjis, "sjis") == "日本");
  CHECK(labelled(sjis, "cp932") == "日本");
  CHECK(labelled(sjis, "windows-932") == "日本");
  CHECK(labelled(gbk, "GB2312") == "中文");
  CHECK(labelled(gbk, "cp936") == "中文");
  CHECK(labelled(big5, "big5") == "中文");
  CHECK(labelled(big5, "cp950") == "中文");
  CHECK(labelled(euckr, "euc-kr") == "한국");
  CHECK(labelled(euckr, "ks_c_5601-1987") == "한국");
  CHECK(labelled(euckr, "cp949") == "한국");
  // Unicode and the browsers' ASCII rule go through util/text.hpp.
  CHECK(labelled(test::bytes("\xEF\xBB\xBFok"sv), "utf-8") == "ok");
  CHECK(labelled(test::bytes("\x93"sv), "us-ascii") == "\xE2\x80\x9C");  // read as Windows-1252
  CHECK(labelled(test::bytes("\x80"sv), "windows-874") == "€");
  CHECK_FALSE(util::decode_labelled(sjis, "klingon").has_value());
  CHECK_FALSE(util::decode_labelled(sjis, "").has_value());

  const struct {
    int code_page;
    std::string_view id;
  } pages[] = {{932, "windows-31j"}, {936, "gbk"},          {949, "euc-kr"},  {950, "big5"},       {874, "windows-874"},
               {1251, "windows-1251"}, {1258, "windows-1258"}, {437, "ibm437"}, {10000, "macintosh"}, {20866, "koi8-r"},
               {54936, "gb18030"},     {51932, "euc-jp"},       {28592, "iso-8859-2"}};
  for (const auto& p : pages) {
    INFO("code page ", p.code_page);
    const util::CharsetInfo* charset = util::charset_for_code_page(p.code_page);
    REQUIRE(charset != nullptr);
    CHECK(charset->id == p.id);
  }
  CHECK(util::charset_for_code_page(65001) == nullptr);  // UTF-8 is not a legacy charset
  CHECK(util::charset_for_code_page(12345) == nullptr);
  CHECK(util::is_multibyte_charset("big5"));
  CHECK_FALSE(util::is_multibyte_charset("windows-1251"));
}

TEST_CASE("charsets: text of unknown encoding is decoded in the right charset") {
  const struct {
    std::string_view file;
    std::string_view charset;
    std::string_view line;
  } cases[] = {
      {"subtitle/legacy/shift_jis.srt", "shift_jis", "こんにちは、世界。字幕のテストです。"},
      {"subtitle/legacy/gbk.srt", "gbk", "你好，世界。这是字幕测试。"},
      {"subtitle/legacy/big5.srt", "big5", "你好，世界。這是字幕測試。"},
      {"subtitle/legacy/euc_kr.srt", "euc-kr", "안녕하세요, 세계. 자막 시험입니다."},
  };
  for (const auto& c : cases) {
    INFO(c.file);
    const util::CharsetText decoded = util::decode_auto(test::fixture(c.file));
    CHECK(decoded.charset == c.charset);
    CHECK(decoded.replaced == 0);
    CHECK(decoded.text.find(c.line) != std::string::npos);
  }
  // Unicode stays Unicode; Western text that is not UTF-8 falls back.
  CHECK(util::decode_auto(test::bytes("plain ASCII")).charset == "utf-8");
  CHECK(util::decode_auto(test::bytes("\xFF\xFEh\0i\0"sv)).text == "hi");
  const util::CharsetText latin = util::decode_auto(test::bytes("Caf\xE9 cr\xE8me br\xFBl\xE9""e"sv));
  CHECK(latin.charset == "windows-1252");
  CHECK(latin.text == "Café crème brûlée");
  // Hostile input never crashes and always yields valid UTF-8.
  for (std::uint32_t seed = 1; seed <= 40; ++seed) {
    const util::CharsetText any = util::decode_auto(test::random_bytes(seed * 37, seed));
    CHECK(util::is_valid_utf8(any.text));
  }
}
