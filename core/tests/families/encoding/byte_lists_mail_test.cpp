// Tests for octal / decimal byte lists, quoted-printable and yEnc.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/encoding/encoding.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace cvt::encoding;
using namespace std::string_view_literals;
using test::convert;
using test::str;

namespace {

template <class T>
Status status_of(const Result<T>& result) { return result ? Status::Ok : result.error().code; }

Bytes byte_values(std::initializer_list<int> values) {
  Bytes out;
  for (const int value : values) out.push_back(static_cast<std::byte>(value));
  return out;
}

// The longest line of `text`, without its terminator.
std::size_t longest_line(std::string_view text) {
  std::size_t longest = 0;
  for (const std::string_view line : util::split_lines(text)) longest = std::max(longest, line.size());
  return longest;
}

}  // namespace

TEST_CASE("encoding: octal byte list") {
  CHECK(octal_text(as_bytes("Hel")) == "110 145 154");
  CHECK(octal_text(byte_values({0, 7, 8, 255}), ", ") == "000, 007, 010, 377");
  CHECK(octal_text(as_bytes("Hi"), "\n") == "110\n151\n");
  CHECK(octal_text({}).empty());
  CHECK(octal_text({}, "\n").empty());

  CHECK(str(*parse_octal_text("110 145 154")) == "Hel");
  CHECK(*parse_octal_text("0,7 ,\n10\t377\r\n") == byte_values({0, 7, 8, 255}));
  CHECK(*parse_octal_text("0000000000000000000000000000000000000017") == byte_values({15}));  // leading zeros are harmless
  CHECK(parse_octal_text(" \n")->empty());
  for (const std::string_view bad : {"400", "777", "18", "9", "12a", "0x12", "-1", "1.5", "377777777777777777777777"}) {
    CHECK_MESSAGE(status_of(parse_octal_text(bad)) == Status::Decode, "should reject: ", bad);
  }
}

TEST_CASE("encoding: decimal byte list") {
  CHECK(decimal_text(as_bytes("Hel")) == "72, 101, 108");
  CHECK(decimal_text(byte_values({0, 9, 10, 255}), " ") == "0 9 10 255");
  CHECK(decimal_text(as_bytes("Hi"), "\n") == "72\n105\n");
  CHECK(decimal_text({}).empty());

  CHECK(str(*parse_decimal_text("72, 101, 108")) == "Hel");
  CHECK(*parse_decimal_text("0\n9,10 255,") == byte_values({0, 9, 10, 255}));
  CHECK(*parse_decimal_text("007") == byte_values({7}));
  CHECK(parse_decimal_text("")->empty());
  for (const std::string_view bad : {"256", "1000", "99999999999999999999999999", "12a", "0x10", "-1", "+1", "1.5", "1;2"}) {
    CHECK_MESSAGE(status_of(parse_decimal_text(bad)) == Status::Decode, "should reject: ", bad);
  }
}

TEST_CASE("encoding: byte list separators through the engine") {
  CHECK(convert("Hel", "bin", "octal").text() == "110 145 154");
  CHECK(convert("Hel", "bin", "octal", R"({"separator":"comma"})").text() == "110, 145, 154");
  CHECK(convert("Hel", "bin", "octal", R"({"separator":"newline"})").text() == "110\n145\n154\n");
  CHECK(convert("Hel", "bin", "decimal").text() == "72, 101, 108");
  CHECK(convert("Hel", "bin", "decimal", R"({"separator":"space"})").text() == "72 101 108");
  CHECK(convert("Hel", "bin", "decimal", R"({"separator":"NEWLINE"})").text() == "72\n101\n108\n");
  CHECK(convert("Hel", "bin", "decimal", R"({"separator":"tab"})").status == Status::InvalidArgument);
  // Each decoder reads every separator style, whatever wrote the list.
  for (const std::string_view separator : {"space", "comma", "newline"}) {
    const std::string options = std::string(R"({"separator":")") + std::string(separator) + "\"}";
    const Bytes all = test::all_byte_values();
    CHECK(convert(convert(all, "bin", "octal", options).data, "octal", "bin").data == all);
    CHECK(convert(convert(all, "bin", "decimal", options).data, "decimal", "bin").data == all);
  }
  CHECK(convert("72, 101, 300", "decimal", "bin").status == Status::Decode);
}

TEST_CASE("encoding: quoted-printable encoding rules") {
  CHECK(quoted_printable_encode(as_bytes("hello world")) == "hello world");
  CHECK(quoted_printable_encode(as_bytes("caf\xC3\xA9 = 5")) == "caf=C3=A9 =3D 5");
  CHECK(quoted_printable_encode(as_bytes("\x00\xff="sv)) == "=00=FF=3D");
  CHECK(quoted_printable_encode({}).empty());

  // CRLF pairs are line breaks; white space in front of one must be escaped.
  CHECK(quoted_printable_encode(as_bytes("line one\r\nline two \r\n")) == "line one\r\nline two=20\r\n");
  CHECK(quoted_printable_encode(as_bytes("trailing space ")) == "trailing space=20");
  CHECK(quoted_printable_encode(as_bytes("trailing tab\t")) == "trailing tab=09");
  CHECK(quoted_printable_encode(as_bytes("a \tb")) == "a \tb");

  // A lone CR or LF is data and is escaped by default.
  CHECK(quoted_printable_encode(as_bytes("tab\t\nx")) == "tab\t=0Ax");
  CHECK(quoted_printable_encode(as_bytes("a\rb\n")) == "a=0Db=0A");
  // Without escape_line_feeds a lone LF stays a line break (what Python's quopri writes).
  CHECK(quoted_printable_encode(as_bytes("tab\t\nx"), false) == "tab=09\nx");
  CHECK(quoted_printable_encode(as_bytes("a\rb\n"), false) == "a=0Db\n");
}

TEST_CASE("encoding: quoted-printable soft line breaks") {
  // 76 columns including the '=' of the soft break.
  CHECK(quoted_printable_encode(as_bytes(std::string(80, 'a'))) == std::string(75, 'a') + "=\r\n" + std::string(5, 'a'));
  CHECK(quoted_printable_encode(as_bytes(std::string(75, 'a'))) == std::string(75, 'a'));
  CHECK(quoted_printable_encode(as_bytes(std::string(76, 'a'))) == std::string(75, 'a') + "=\r\na");
  // An escape is never split: 74 characters leave no room for "=C3".
  CHECK(quoted_printable_encode(as_bytes(std::string(74, 'a') + "\xC3\xA9")) == std::string(74, 'a') + "=\r\n=C3=A9");
  CHECK(quoted_printable_encode(as_bytes(std::string(72, 'a') + "\xC3\xA9")) == std::string(72, 'a') + "=C3=\r\n=A9");

  const std::string_view prose =
      "J'interdis aux marchands de vanter trop leurs marchandises. Car ils se font vite p\xC3\xA9"
      "dagogues et t'enseignent comme but ce qui n'est par essence qu'un moyen";
  CHECK(quoted_printable_encode(as_bytes(prose), false) ==
        "J'interdis aux marchands de vanter trop leurs marchandises. Car ils se font=\n"
        " vite p=C3=A9dagogues et t'enseignent comme but ce qui n'est par essence qu=\n"
        "'un moyen");

  for (const bool escape_line_feeds : {true, false}) {
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
      const Bytes data = test::random_bytes(seed * 37, seed);
      const std::string text = quoted_printable_encode(data, escape_line_feeds);
      CHECK(longest_line(text) <= 76);
      for (const char c : text) CHECK(static_cast<unsigned char>(c) < 0x7F);
      CHECK(*quoted_printable_decode(text) == data);
    }
  }
}

TEST_CASE("encoding: quoted-printable decoding") {
  CHECK(str(*quoted_printable_decode("caf=C3=A9 =3D 5")) == "caf\xC3\xA9 = 5");
  CHECK(str(*quoted_printable_decode("caf=c3=a9")) == "caf\xC3\xA9");  // lower-case digits are tolerated
  // Soft breaks ending in CRLF, LF, or padded with blanks; '=' at the very end.
  CHECK(str(*quoted_printable_decode("foo=\r\nbar=\nbaz= \t\r\nqux=")) == "foobarbazqux");
  // Hard breaks are kept as written; blanks in front of them are transport padding.
  CHECK(str(*quoted_printable_decode("a=\nb=3D=C3=A9 \nx=20\n")) == "ab=\xC3\xA9\nx \n");
  CHECK(str(*quoted_printable_decode("one \t\r\ntwo  ")) == "one\r\ntwo");
  CHECK(str(*quoted_printable_decode("a \tb")) == "a \tb");
  // A CR without LF is not a line break: it is data, and so are blanks in front of it.
  CHECK(str(*quoted_printable_decode("a \rb\r")) == "a \rb\r");
  CHECK(quoted_printable_decode("")->empty());

  CHECK(quoted_printable_decode("=")->empty());  // a soft break at the end of the data
  for (const std::string_view bad : {"a=4", "a=4\n", "=G1", "=1G", "a= b", "100%=ZZ", "=\xC3\xA9", "soft=\rbreak"}) {
    CHECK_MESSAGE(status_of(quoted_printable_decode(bad)) == Status::Decode, "should reject: ", bad);
  }
}

TEST_CASE("encoding: quoted-printable through the engine") {
  const test::Converted encoded = convert("na\xC3\xAFve\r\nUnix line\n", "bin", "quoted-printable");
  REQUIRE(encoded.ok());
  CHECK(encoded.text() == "na=C3=AFve\r\nUnix line=0A");
  CHECK(encoded.extension == "qp");
  CHECK(convert("Unix line\n", "bin", "quoted-printable", R"({"escape_line_feeds":false})").text() == "Unix line\n");
  CHECK(convert(encoded.data, "quoted-printable", "bin").text() == "na\xC3\xAFve\r\nUnix line\n");
  CHECK(convert("broken =ZZ escape", "quoted-printable", "bin").status == Status::Decode);
}

TEST_CASE("encoding: yEnc encoding") {
  CHECK(yenc_encode(as_bytes("Cat"), "cat.txt") ==
        "=ybegin line=128 size=3 name=cat.txt\r\nm\x8B\x9E\r\n=yend size=3 crc32=a6130548\r\n");
  // The critical characters NUL, LF, CR and '=' are escaped.
  CHECK(yenc_encode(byte_values({214, 224, 227, 19, 0, 1}), "t.bin") ==
        "=ybegin line=128 size=6 name=t.bin\r\n=@=J=M=}*+\r\n=yend size=6 crc32=f0dc5a27\r\n");
  CHECK(yenc_encode({}, "") == "=ybegin line=128 size=0 name=data.bin\r\n=yend size=0 crc32=00000000\r\n");
  // File names keep spaces but can never add a line.
  CHECK(yenc_encode({}, " my\r\nfile \x01name.bin ").starts_with("=ybegin line=128 size=0 name=myfile name.bin\r\n"));

  // TAB and SPACE at either end of a line and a leading '.' are escaped too.
  CHECK(yenc_encode(byte_values({223, 55, 223}), "x").find("\r\n=Ia=I\r\n") != std::string::npos);
  CHECK(yenc_encode(byte_values({246, 55, 246, 55}), "x").find("\r\n=`a a\r\n") != std::string::npos);
  CHECK(yenc_encode(byte_values({4, 4}), "x").find("\r\n=n.\r\n") != std::string::npos);

  // Lines hold 128 characters; an escape may make one 129 long, never more.
  const std::string text = yenc_encode(test::random_bytes(5000, 8), "r.bin");
  const std::vector<std::string_view> lines = util::split_lines(text);
  REQUIRE(lines.size() > 30);
  for (std::size_t i = 1; i + 2 < lines.size(); ++i) {
    CHECK(lines[i].size() >= 128);
    CHECK(lines[i].size() <= 129);
  }
  CHECK(lines.back().starts_with("=yend size=5000 crc32="));
}

TEST_CASE("encoding: yEnc decoding verifies size and checksum") {
  const std::string_view cat = "=ybegin line=128 size=3 name=cat.txt\r\nm\x8B\x9E\r\n=yend size=3 crc32=a6130548\r\n";
  const auto parsed = yenc_decode(cat);
  REQUIRE(parsed);
  CHECK(str(parsed->bytes) == "Cat");
  CHECK(parsed->filename == "cat.txt");

  // LF line endings, upper-case CRC, text around the block, a name with spaces.
  const auto lenient = yenc_decode("Subject: test\n\n=ybegin line=128 size=3 name=my cat.txt \nm\x8B\x9E\n=yend size=3 crc32=A6130548\nbye\n");
  REQUIRE(lenient);
  CHECK(str(lenient->bytes) == "Cat");
  CHECK(lenient->filename == "my cat.txt");
  // Size and CRC are optional in the trailer.
  CHECK(str(yenc_decode("=ybegin line=128 size=3 name=c\nm\x8B\x9E\n=yend\n")->bytes) == "Cat");
  CHECK(yenc_decode("=ybegin line=128 size=6 name=t\r\n=@=J=M=}*+\r\n=yend size=6 crc32=f0dc5a27\r\n")->bytes ==
        byte_values({214, 224, 227, 19, 0, 1}));

  const std::pair<std::string_view, std::string_view> bad[] = {
      {"m\x8B\x9E\r\n=yend size=3\r\n", "'=ybegin'"},
      {"=ybegin line=128 size=3 name=c\r\nm\x8B\x9E\r\n", "no '=yend'"},
      {"=ybegin line=128 size=3 name=c\r\nm\x8B\x9E\r\n=yend size=3 crc32=a6130549\r\n", "checksum mismatch"},
      {"=ybegin line=128 size=3 name=c\r\nm\x8B\x9E\r\n=yend size=3 crc32=xyz\r\n", "not a hexadecimal"},
      {"=ybegin line=128 size=3 name=c\r\nm\x8B\x9E\r\n=yend size=4 crc32=a6130548\r\n", "declares 4"},
      {"=ybegin line=128 size=4 name=c\r\nm\x8B\x9E\r\n=yend size=3 crc32=a6130548\r\n", "declares 4"},
      {"=ybegin line=128 size=abc name=c\r\nm\x8B\x9E\r\n=yend\r\n", "not a number"},
      {"=ybegin line=128 size=3 name=c\r\nm\x8B=\r\n=yend size=3\r\n", "escape"},
      {"=ybegin part=1 total=2 line=128 size=3 name=c\r\n=ypart begin=1 end=3\r\nm\x8B\x9E\r\n=yend size=3 part=1\r\n",
       "multi-part"},
  };
  for (const auto& [text, reason] : bad) {
    const auto result = yenc_decode(text);
    REQUIRE_MESSAGE(!result, "should reject: ", reason);
    CHECK(result.error().code == Status::Decode);
    CHECK_MESSAGE(result.error().message.find(reason) != std::string::npos, result.error().message);
  }
}

TEST_CASE("encoding: yEnc through the engine") {
  const Bytes all = test::all_byte_values();
  const test::Converted encoded = convert(all, "bin", "yenc", {}, "all bytes.bin");
  REQUIRE(encoded.ok());
  CHECK(encoded.extension == "yenc");
  CHECK(encoded.mime == "application/x-yenc");
  CHECK(encoded.text().starts_with("=ybegin line=128 size=256 name=all bytes.bin\r\n"));
  CHECK_FALSE(util::is_valid_utf8(encoded.text()));  // 8-bit data, by design

  const test::Converted renamed = convert(all, "bin", "yenc", R"({"filename":"other.dat"})", "all bytes.bin");
  CHECK(renamed.text().starts_with("=ybegin line=128 size=256 name=other.dat\r\n"));

  const test::Converted back = convert(encoded.data, "yenc", "bin");
  REQUIRE(back.ok());
  CHECK(back.data == all);

  // The decoded blob carries the stored file name on to the next writer.
  const test::Converted uue = convert(encoded.data, "yenc", "uue");
  REQUIRE(uue.ok());
  CHECK(uue.text().starts_with("begin 644 allbytes.bin\n"));

  Bytes corrupted = encoded.data;
  corrupted[60] = static_cast<std::byte>(std::to_integer<int>(corrupted[60]) ^ 0x01);
  CHECK(convert(corrupted, "yenc", "bin").status == Status::Decode);
}
