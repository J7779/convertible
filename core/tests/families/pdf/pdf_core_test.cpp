// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// pdf family: object syntax, filters, ciphers, page ranges.
#include <string>

#include "families/pdf/cmap.hpp"
#include "families/pdf/crypto.hpp"
#include "families/pdf/filters.hpp"
#include "families/pdf/font_data.hpp"
#include "families/pdf/pdf.hpp"
#include "families/pdf/syntax.hpp"
#include "support/test_support.hpp"
#include "util/deflate.hpp"
#include "util/hex.hpp"

using namespace cvt;
using namespace std::string_view_literals;

namespace {

std::string hex_of(std::span<const std::uint8_t> v) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string s;
  for (const std::uint8_t b : v) {
    s.push_back(digits[b >> 4]);
    s.push_back(digits[b & 15]);
  }
  return s;
}

std::vector<std::uint8_t> from_hex(std::string_view h) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
  return out;
}

pdf::Object parse(std::string_view text) {
  pdf::Lexer lexer(as_bytes(text));
  return pdf::parse_object(lexer);
}

}  // namespace

TEST_CASE("pdf/syntax: objects parse with escapes, references and nesting") {
  const pdf::Object o = parse(R"(<< /Type /Page /Name#20With#20Space (a\(b\)c\n\101) /Hex <48656C6C6F> /Num -12.5 /Int 42
                                  /Ref 12 0 R /Arr [1 2.0 /N (x) [true false null]] /Null null >>)");
  REQUIRE(o.is_dict());
  const pdf::Dict& d = o.as_dict();
  CHECK(d.get("Type").is_name("Page"));
  CHECK(d.contains("Name With Space"));
  CHECK(d.get("Name With Space").as_string() == "a(b)c\nA");
  CHECK(d.get("Hex").as_string() == "Hello");
  CHECK_APPROX(d.get("Num").as_number(), -12.5, 1e-12);
  CHECK(d.get("Int").as_int() == 42);
  CHECK(d.get("Ref").as_ref() == pdf::Ref{12, 0});
  CHECK(d.get("Arr").as_array().size() == 5);
  CHECK(!d.contains("Null"));  // null values mean "absent"
  // Serialising and parsing again gives the same structure.
  std::string out;
  pdf::serialize(o, out);
  const pdf::Object again = parse(out);
  CHECK(again.as_dict().get("Name With Space").as_string() == "a(b)c\nA");
  CHECK(again.as_dict().get("Ref").as_ref() == pdf::Ref{12, 0});
}

TEST_CASE("pdf/syntax: hostile nesting and garbage never crash") {
  std::string deep(100000, '[');
  const pdf::Object o = parse(deep);
  CHECK(o.is_array());
  std::string dicts;
  for (int i = 0; i < 20000; ++i) dicts += "<< /A ";
  (void)parse(dicts);
  const Bytes noise = test::random_bytes(4096, 7);
  pdf::Lexer lexer(noise);
  for (int i = 0; i < 2000 && !lexer.at_end(); ++i) (void)pdf::parse_object(lexer);
  CHECK(parse("(unterminated").as_string() == "unterminated");
  CHECK(parse("<4142").as_string() == "AB");
}

TEST_CASE("pdf/syntax: text strings decode PDFDocEncoding and UTF-16") {
  CHECK(pdf::decode_text_string("\xFE\xFF\x00\x48\x00\xE9"sv) == "H\xC3\xA9");
  CHECK(pdf::decode_text_string("caf\xE9 \x8Dq\x8E") == "caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D");
  CHECK(pdf::decode_text_string(pdf::encode_text_string("Z\xC3\xBCrich \xE2\x82\xAC")) == "Z\xC3\xBCrich \xE2\x82\xAC");
}

TEST_CASE("pdf/filters: LZW, ASCII85, ASCIIHex, RunLength") {
  // PDF Reference example: "-----A---B".
  const auto lzw = pdf::lzw_decode(as_bytes("\x80\x0B\x60\x50\x22\x0C\x0C\x85\x01"sv), true, 1 << 20);
  REQUIRE_OK(lzw);
  CHECK(test::str(*lzw) == "-----A---B");
  const Bytes data = test::random_bytes(1000, 3);
  const auto a85 = pdf::ascii85_decode(pdf::ascii85_encode(data), 1 << 20);
  REQUIRE_OK(a85);
  CHECK(*a85 == data);
  CHECK(test::str(*pdf::ascii85_decode(as_bytes("<~9jqo^~>"sv), 100)) == "Man ");
  CHECK(test::str(*pdf::asciihex_decode(as_bytes("48 65 6c6C 6>"sv), 100)) == "Hell`");
  Bytes runs(500, std::byte{7});
  runs.insert(runs.end(), data.begin(), data.begin() + 300);
  const auto rl = pdf::runlength_decode(pdf::runlength_encode(runs), 1 << 20);
  REQUIRE_OK(rl);
  CHECK(*rl == runs);
  CHECK_STATUS(pdf::runlength_decode(pdf::runlength_encode(runs), 100), Status::Limit);
}

TEST_CASE("pdf/filters: Flate with PNG and TIFF predictors, damaged streams") {
  // An RGB gradient predicted with PNG filters and compressed.
  const int w = 37, h = 11;
  Bytes raw;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      raw.push_back(static_cast<std::byte>(x * 7));
      raw.push_back(static_cast<std::byte>(y * 20));
      raw.push_back(static_cast<std::byte>((x + y) * 3));
    }
  }
  const Bytes predicted = pdf::png_predict(raw, 3, 8, w);
  const Bytes compressed = *util::zlib_compress(predicted, 9);
  pdf::Dict parms;
  parms.set("Predictor", 15);
  parms.set("Colors", 3);
  parms.set("Columns", w);
  const auto out = pdf::apply_filters(compressed, pdf::Object::name("FlateDecode"), pdf::Object(parms), pdf::FilterEnv{});
  REQUIRE_OK(out);
  CHECK(out->data == raw);
  // TIFF predictor 2 on 8-bit samples.
  Bytes diff = raw;
  for (int y = 0; y < h; ++y) {
    for (int x = w - 1; x >= 1; --x) {
      for (int c = 0; c < 3; ++c) {
        const std::size_t i = static_cast<std::size_t>((y * w + x) * 3 + c);
        diff[i] = static_cast<std::byte>(std::to_integer<int>(raw[i]) - std::to_integer<int>(raw[i - 3]));
      }
    }
  }
  const auto tiff = pdf::apply_predictor(diff, 2, 3, 8, w);
  REQUIRE_OK(tiff);
  CHECK(*tiff == raw);
  // A truncated stream yields what could be decoded.
  bool damaged = false;
  const auto partial = pdf::flate_decode_tolerant(ByteView(compressed).first(compressed.size() / 2), 1 << 20, &damaged);
  REQUIRE_OK(partial);
  CHECK(damaged);
  CHECK(!partial->empty());
  // Decompression bombs stop at the limit.
  const Bytes zeros(1 << 20);
  const Bytes bomb = *util::zlib_compress(zeros, 9);
  pdf::FilterEnv env;
  env.max_output = 1000;
  CHECK_STATUS(pdf::apply_filters(bomb, pdf::Object::name("Fl"), {}, env), Status::Limit);
  CHECK_STATUS(pdf::apply_filters(bomb, pdf::Object::name("NoSuchFilter"), {}, env), Status::Decode);
}

TEST_CASE("pdf/crypto: AES, RC4 and SHA-2 match published vectors") {
  const auto key128 = from_hex("000102030405060708090a0b0c0d0e0f");
  const auto key256 = from_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  const auto plain = from_hex("00112233445566778899aabbccddeeff");
  std::uint8_t out[16];
  pdf::crypto::Aes(key128).encrypt_block(plain.data(), out);
  CHECK(hex_of(out) == "69c4e0d86a7b0430d8cdb78070b4c55a");
  pdf::crypto::Aes(key256).encrypt_block(plain.data(), out);
  CHECK(hex_of(out) == "8ea2b7ca516745bfeafc49904b496089");
  std::uint8_t back[16];
  pdf::crypto::Aes(key256).decrypt_block(out, back);
  CHECK(hex_of(back) == hex_of(plain));
  const std::string msg = "Plaintext";
  const auto rc4 = pdf::crypto::rc4(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("Key"), 3),
                                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size()));
  CHECK(hex_of(rc4) == "bbf316e8d940af0ad3");
  const std::uint8_t abc[] = {'a', 'b', 'c'};
  CHECK(hex_of(pdf::crypto::sha384(abc)) ==
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
  CHECK(hex_of(pdf::crypto::sha512(abc)) ==
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
  // CBC round trip with PKCS#5 padding.
  std::vector<std::uint8_t> iv(16, 9), data(40, 3);
  std::vector<std::uint8_t> padded = data;
  padded.insert(padded.end(), 8, 8);
  auto enc = pdf::crypto::aes_cbc_encrypt_nopad(key128, iv, padded);
  enc.insert(enc.begin(), iv.begin(), iv.end());
  CHECK(pdf::crypto::aes_cbc_decrypt_pdf(key128, enc) == data);
}

TEST_CASE("pdf/cmap: ToUnicode ranges, arrays and codespaces") {
  const std::string program =
      "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
      "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
      "2 beginbfchar <0003> <0020> <0024> <00660069> endbfchar\n"
      "2 beginbfrange <0010> <0012> <0041> <0020> <0021> [<03B1> <D83DDE00>] endbfrange\n"
      "endcmap end end\n";
  const pdf::CMap map = pdf::CMap::parse(as_bytes(program));
  const auto code = map.next_code("\x00\x11"sv, 0);
  CHECK(code.length == 2);
  CHECK(map.unicode(code).value() == U"B");
  CHECK(map.unicode({0x24, 2}).value() == U"fi");
  CHECK(map.unicode({0x21, 2}).value() == U"\U0001F600");
  CHECK(!map.unicode({0x99, 2}).has_value());
  const pdf::CMap identity = pdf::CMap::predefined("Identity-H");
  CHECK(identity.cid(identity.next_code("\x01\x02"sv, 0)) == 0x0102);
  CHECK(pdf::CMap::predefined("UniJIS-UCS2-H").code_is_unicode());
}

TEST_CASE("pdf/font_data: encodings and glyph names") {
  using namespace pdf::font;
  CHECK(encoding_glyph_name(BaseEncoding::WinAnsi, 0x80) == "Euro");
  CHECK(encoding_glyph_name(BaseEncoding::WinAnsi, 0x93) == "quotedblleft");
  CHECK(encoding_glyph_name(BaseEncoding::Standard, 0x27) == "quoteright");
  CHECK(encoding_glyph_name(BaseEncoding::MacRoman, 0xCA) == "space");
  CHECK(encoding_glyph_name(BaseEncoding::Symbol, 0x61) == "alpha");
  CHECK(glyph_name_to_unicode("Aacute").value() == U"Á");
  CHECK(glyph_name_to_unicode("fi").value() == U"fi");
  CHECK(glyph_name_to_unicode("uni2003").value() == U" ");
  CHECK(glyph_name_to_unicode("u1F600").value() == U"\U0001F600");
  CHECK(glyph_name_to_unicode("afii10017").value() == U"А");
  CHECK(glyph_name_to_unicode("f_f_i").value() == U"ffi");
  CHECK(glyph_name_to_unicode("T.sc").value() == U"T");
  CHECK(!glyph_name_to_unicode("g17").has_value());
  CHECK(standard14_width(Standard14::Helvetica, "a").value() == 556);
  CHECK(standard14_width(Standard14::TimesRoman, "a").value() == 444);
  CHECK(standard14_width(Standard14::HelveticaBold, "A").value() == 722);
  CHECK(standard14_width(Standard14::Courier, "W").value() == 600);
  CHECK(standard14_from_name("ABCDEF+Arial,Bold") == Standard14::HelveticaBold);
  CHECK(standard14_from_name("TimesNewRomanPS-ItalicMT") == Standard14::TimesItalic);
  CHECK(pdfdoc_to_unicode(0x80) == 0x2022);
}

TEST_CASE("pdf/pages option: ranges parse and validate") {
  CHECK(pdf::parse_page_ranges("", 3).value() == std::vector<int>{0, 1, 2});
  CHECK(pdf::parse_page_ranges("2-3, 1", 5).value() == std::vector<int>{1, 2, 0});
  CHECK(pdf::parse_page_ranges("4-", 5).value() == std::vector<int>{3, 4});
  CHECK(pdf::parse_page_ranges("9", 5).has_value() == false);
  CHECK_STATUS(pdf::parse_page_ranges("x", 5), Status::InvalidArgument);
}
