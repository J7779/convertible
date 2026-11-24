// Tests that run every encoding through the real engine: round trips,
// robustness against malformed input, Unicode input files, which targets are
// offered for which sources, content detection and registry consistency.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/detect.hpp"
#include "engine/planner.hpp"
#include "families/encoding/encoding.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace std::string_view_literals;
using test::convert;

namespace {

struct BinaryFormat {
  std::string_view id;
  bool reversible;            // has a decoder
  std::string_view malformed; // an input the decoder must reject (reversible formats only)
};

// Every Blob encoding of the family. `malformed` is rejected with Status::Decode.
constexpr BinaryFormat binary_formats[] = {
    {"base64", true, "@@@@"},
    {"base64url", true, "@@@@"},
    {"hex", true, "xyz"},
    {"hexdump", true, "00000000: zz"},
    {"data-uri", true, "not a data URI"},
    {"uue", true, "no begin line"},
    {"url-encoded", true, "%zz"},
    {"binary-text", true, "0101"},
    {"base32", true, "1"},
    {"base32hex", true, "W"},
    {"base32-crockford", true, "U"},
    {"base58", true, "0"},
    {"base36", true, "!"},
    {"ascii85", true, "<~never closed"},
    {"z85", true, "abc"},
    {"octal", true, "9"},
    {"decimal", true, "256"},
    {"quoted-printable", true, "=ZZ"},
    {"yenc", true, "no begin line"},
    {"gzip-base64", true, "aGVsbG8="},
    {"c-header", false, ""},
    {"checksums", false, ""},
    {"cpp-array", false, ""},
    {"python-bytes", false, ""},
    {"js-array", false, ""},
    {"java-array", false, ""},
    {"go-array", false, ""},
    {"rust-array", false, ""},
    {"swift-array", false, ""},
    {"csharp-array", false, ""},
};

struct TextFormat {
  std::string_view id;
  bool keeps_case;             // false: decoding yields upper-case text
  std::string_view malformed;  // an input the decoder must reject; empty = the decoder accepts everything
};

constexpr TextFormat text_formats[] = {
    {"html-entities", true, ""},
    {"url-text", true, "%zz"},
    {"unicode-escapes", true, "\\q"},
    {"json-string", true, "no quotes"},
    {"rot13", true, ""},
    {"rot47", true, ""},
    {"punycode", true, "xn--"},
    {"morse", false, "x"},
    {"nato", false, "x"},
};

bool is_big_number(std::string_view id) { return id == "base58" || id == "base36"; }

std::vector<std::string> target_ids(std::string_view source) {
  std::vector<std::string> ids;
  for (const TargetInfo& target : list_targets(Registry::instance(), source)) ids.push_back(target.format->id);
  return ids;
}

bool contains(const std::vector<std::string>& ids, std::string_view id) {
  return std::ranges::find(ids, id) != ids.end();
}

std::string detected(ByteView data, std::string_view filename) {
  const auto detection = detect_format(Registry::instance(), data, filename);
  return detection ? detection->format->id : std::string("<nothing>");
}
std::string detected(std::string_view text, std::string_view filename) { return detected(as_bytes(text), filename); }

// `text` as a UTF-16 file with a byte order mark.
Bytes utf16(std::string_view text, util::TextEncoding encoding) { return util::encode_text(text, encoding, true).bytes; }

// A sample that every text transform can represent: mixed scripts, markup,
// escapes, several lines. Lower-case only inside the host names.
const std::string rich_text =
    "Tom & Jerry <tom@example.com> say \"hi\" \\ 'bye'\n"
    "caf\xC3\xA9 costs \xE2\x82\xAC" "5 \xF0\x9F\x98\x80\twww.b\xC3\xBC" "cher.de\r\n"
    "100% of a+b=c; path/to?x=1#top\n";

}  // namespace

TEST_CASE("encoding: every reversible binary encoding round-trips through the engine") {
  const Bytes kilobytes = test::random_bytes(100 * 1024, 77);
  for (const BinaryFormat& format : binary_formats) {
    if (!format.reversible) continue;
    CAPTURE(format.id);
    const auto round_trip = [&](ByteView data) {
      const test::Converted encoded = convert(data, "bin", format.id, {}, "sample.bin");
      if (format.id == "z85" && data.size() % 4 != 0) {
        CHECK(encoded.status == Status::Encode);
        return;
      }
      REQUIRE_MESSAGE(encoded.ok(), encoded.message, " (", data.size(), " bytes)");
      CHECK(encoded.format_id == format.id);
      const test::Converted decoded = convert(encoded.data, format.id, "bin");
      REQUIRE_MESSAGE(decoded.ok(), decoded.message, " (", data.size(), " bytes)");
      CHECK_MESSAGE(decoded.data == Bytes(data.begin(), data.end()), data.size(), " bytes");
    };
    round_trip({});
    for (std::size_t size = 1; size <= 70; ++size) round_trip(test::random_bytes(size, static_cast<std::uint32_t>(size)));
    round_trip(test::all_byte_values());
    round_trip(Bytes(64, std::byte{0}));
    round_trip(test::bytes("\r\n \t=\r\n.\r\n\n\n  \r"));
    if (is_big_number(format.id)) {
      const test::Converted refused = convert(kilobytes, "bin", format.id);
      CHECK(refused.status == Status::Limit);
      CHECK(refused.message.find("65536") != std::string::npos);
    } else {
      round_trip(kilobytes);
    }
  }
}

TEST_CASE("encoding: one-way binary encodings accept any input") {
  for (const BinaryFormat& format : binary_formats) {
    if (format.reversible) continue;
    CAPTURE(format.id);
    CHECK(test::find_reader(format.id, Kind::Blob) == nullptr);
    for (const Bytes& data : {Bytes{}, test::random_bytes(1), test::all_byte_values(), test::random_bytes(100 * 1024, 5)}) {
      const test::Converted encoded = convert(data, "bin", format.id, {}, "sample file.bin");
      REQUIRE_MESSAGE(encoded.ok(), encoded.message);
      CHECK_FALSE(encoded.data.empty());
      CHECK(util::is_valid_utf8(encoded.text()));
    }
  }
}

TEST_CASE("encoding: text transforms round-trip through the engine") {
  const std::string samples[] = {
      "",
      "x",
      "hello world",
      rich_text,
      "line one\nline two\n\nline four\n",
      std::string(3000, 'a') + " " + std::string(3000, 'b'),
  };
  for (const TextFormat& format : text_formats) {
    CAPTURE(format.id);
    for (const std::string& sample : samples) {
      const test::Converted encoded = convert(sample, "txt", format.id);
      REQUIRE_MESSAGE(encoded.ok(), encoded.message);
      CHECK(util::is_valid_utf8(encoded.text()));
      const test::Converted decoded = convert(encoded.data, format.id, "txt");
      REQUIRE_MESSAGE(decoded.ok(), decoded.message);
      if (format.keeps_case) {
        CHECK(decoded.text() == sample);
      } else if (sample != rich_text) {
        // Morse and NATO spell letters and digits only, in upper case.
        CHECK(decoded.text() == util::to_upper(sample));
      }
    }
  }
}

TEST_CASE("encoding: decoders reject malformed input with a decode error") {
  for (const BinaryFormat& format : binary_formats) {
    if (!format.reversible) continue;
    const test::Converted result = convert(format.malformed, format.id, "bin");
    CHECK_MESSAGE(result.status == Status::Decode, format.id, ": ", result.message);
    CHECK_FALSE(result.message.empty());
  }
  for (const TextFormat& format : text_formats) {
    const test::Converted result = convert(format.malformed.empty() ? "&bogus; anything goes"sv : format.malformed, format.id, "txt");
    if (format.malformed.empty()) {
      CHECK_MESSAGE(result.ok(), format.id, ": ", result.message);
    } else {
      CHECK_MESSAGE(result.status == Status::Decode, format.id, ": ", result.message);
      CHECK_FALSE(result.message.empty());
    }
  }
}

TEST_CASE("encoding: decoders survive truncated, random and odd input") {
  const Bytes payload = test::random_bytes(200, 21);
  const std::string_view odd_inputs[] = {
      "\0\x01\x02\x7F\x1B"sv,
      "\xFF\xFE\xFD\xC3\x28\xE2\x82\xF0\x9F",
      "=====~~~~~<~<~~>~>%%%%%&&&&&#####;;;;;",
      "\\u\\x\\U\\u{\\ud800\\\\\\",
      "=ybegin =yend begin 644 end data: xn-- xn--- &# &#x \"\\",
      "\r\r\r\n\n\n \t \t\r\n",
      "0 00 000 0000 00000 1 11 111 1111 11111 z zz zzz zzzz zzzzz",
      "\xEF\xBB\xBF\xEF\xBB\xBF",
      "\xFF\xFE\x41\x00\x0A\x00\x00\xD8"sv,
      "\xFE\xFF\x00"sv,
  };
  const auto acceptable = [](Status status) { return status == Status::Ok || status == Status::Decode || status == Status::Limit; };

  for (const BinaryFormat& format : binary_formats) {
    if (!format.reversible) continue;
    CAPTURE(format.id);
    const test::Converted valid = convert(format.id == "z85" ? ByteView(payload) : ByteView(payload).first(199), "bin", format.id,
                                          {}, "payload.bin");
    REQUIRE(valid.ok());
    // Every prefix of valid data: an error or a result, never a crash or another kind of failure.
    for (std::size_t length = 0; length <= valid.data.size(); ++length) {
      const auto result = test::read_as<ir::Blob>(format.id, ByteView(valid.data).first(length));
      CHECK_MESSAGE(acceptable(result ? Status::Ok : result.error().code), "prefix of ", length, " bytes");
    }
    CHECK(test::fuzz_reader<ir::Blob>(format.id, valid.data) > 64);
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
      const auto result = test::read_as<ir::Blob>(format.id, test::random_bytes(seed * 13, seed));
      CHECK(acceptable(result ? Status::Ok : result.error().code));
    }
    for (const std::string_view odd : odd_inputs) {
      const test::Converted result = convert(odd, format.id, "bin");
      CHECK_MESSAGE(acceptable(result.status), result.message);
    }
  }

  for (const TextFormat& format : text_formats) {
    CAPTURE(format.id);
    const test::Converted valid = convert(format.keeps_case ? std::string_view(rich_text) : "plain words 123\nsecond line"sv, "txt",
                                          format.id);
    REQUIRE(valid.ok());
    for (std::size_t length = 0; length <= valid.data.size(); ++length) {
      const auto result = test::read_as<ir::PlainText>(format.id, ByteView(valid.data).first(length));
      CHECK_MESSAGE(acceptable(result ? Status::Ok : result.error().code), "prefix of ", length, " bytes");
      if (result) CHECK(util::is_valid_utf8(result->text));
    }
    CHECK(test::fuzz_reader<ir::PlainText>(format.id, valid.data) > 64);
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
      const auto result = test::read_as<ir::PlainText>(format.id, test::random_bytes(seed * 13, seed));
      CHECK(acceptable(result ? Status::Ok : result.error().code));
      if (result) CHECK(util::is_valid_utf8(result->text));
    }
    for (const std::string_view odd : odd_inputs) {
      const test::Converted result = convert(odd, format.id, "txt");
      CHECK_MESSAGE(acceptable(result.status), result.message);
    }
  }
}

TEST_CASE("encoding: encoded text may arrive as UTF-16 with a byte order mark") {
  using util::TextEncoding;
  CHECK(convert(utf16("MZXW6YTBOI======\r\n", TextEncoding::Utf16LE), "base32", "bin").text() == "foobar");
  CHECK(convert(utf16("MZXW6YTBOI======\r\n", TextEncoding::Utf16BE), "base32", "bin").text() == "foobar");
  CHECK(convert(utf16("Zm9vYmFy", TextEncoding::Utf16LE), "base64", "bin").text() == "foobar");
  CHECK(convert(utf16("66 6f 6f\n", TextEncoding::Utf16BE), "hex", "bin").text() == "foo");
  CHECK(convert(utf16("<~BOu!rD]j7BEbo7~>", TextEncoding::Utf16LE), "ascii85", "bin").text() == "hello world");
  CHECK(convert(utf16("2NEpo7TZRRrLZSi2U", TextEncoding::Utf32LE), "base58", "bin").text() == "Hello World!");
  CHECK(convert(utf16("caf=C3=A9", TextEncoding::Utf16LE), "quoted-printable", "bin").text() == "caf\xC3\xA9");
  CHECK(convert("\xEF\xBB\xBF" "MZXW6YTBOI======", "base32", "bin").text() == "foobar");  // UTF-8 with a BOM

  // Text transforms: the decoded text comes back as UTF-8.
  CHECK(convert(utf16("Uryyb, Jbeyq!", TextEncoding::Utf16LE), "rot13", "txt").text() == "Hello, World!");
  CHECK(convert(utf16("... --- ...", TextEncoding::Utf16BE), "morse", "txt").text() == "SOS");
  CHECK(convert(utf16("caf&eacute; &#8364;", TextEncoding::Utf16LE), "html-entities", "txt").text() == "caf\xC3\xA9 \xE2\x82\xAC");
  CHECK(convert(utf16("\"caf\\u00e9\"", TextEncoding::Utf16BE), "json-string", "txt").text() == "caf\xC3\xA9");
  CHECK(convert(utf16("www.xn--bcher-kva.de", TextEncoding::Utf16LE), "punycode", "txt").text() == "www.b\xC3\xBC" "cher.de");
  // And a UTF-16 text file can be the source of a transform.
  CHECK(convert(utf16("caf\xC3\xA9", TextEncoding::Utf16LE), "txt", "unicode-escapes").text() == "caf\\u00e9");
  CHECK(convert(utf16("Hello", TextEncoding::Utf16BE), "txt", "rot13").text() == "Uryyb");
}

TEST_CASE("encoding: text transforms are only offered for text sources") {
  const std::vector<std::string> from_png = target_ids("png");
  const std::vector<std::string> from_text = target_ids("txt");
  const std::vector<std::string> from_bin = target_ids("bin");
  REQUIRE_FALSE(from_png.empty());
  for (const TextFormat& format : text_formats) {
    CAPTURE(format.id);
    CHECK_FALSE(contains(from_png, format.id));
    CHECK_FALSE(contains(from_bin, format.id));
    CHECK(contains(from_text, format.id));
    const auto plan = plan_conversion(Registry::instance(), "png", format.id);
    REQUIRE_FALSE(plan);
    CHECK(plan.error().code == Status::UnsupportedConversion);
    CHECK(convert("abc", "png", format.id).status == Status::UnsupportedConversion);
    // Transforms chain with each other and with plain text.
    CHECK(contains(target_ids(format.id), "txt"));
    CHECK(contains(target_ids(format.id), "rot47"));
  }
  for (const BinaryFormat& format : binary_formats) {
    CAPTURE(format.id);
    CHECK(contains(from_png, format.id));
    CHECK(contains(from_text, format.id));
    CHECK(contains(target_ids("rot13"), format.id));
  }
  CHECK(contains(from_png, "bin"));
  CHECK(contains(from_text, "txt"));  // re-encode: another character set or line ending

  // Decoding is offered too: an encoded file converts back to raw bytes.
  for (const BinaryFormat& format : binary_formats) {
    if (format.reversible) CHECK_MESSAGE(contains(target_ids(format.id), "bin"), format.id);
  }
}

TEST_CASE("encoding: lossy and lossless routes are labelled") {
  for (const TargetInfo& target : list_targets(Registry::instance(), "txt")) {
    const std::string& id = target.format->id;
    if (id == "morse" || id == "nato" || id == "punycode") CHECK_MESSAGE(target.lossy, id);
    if (id == "rot13" || id == "html-entities" || id == "json-string" || id == "base32" || id == "yenc") {
      CHECK_MESSAGE(!target.lossy, id);
    }
  }
}

TEST_CASE("encoding: formats are recognised by content, whatever the file is called") {
  const Bytes payload = test::random_bytes(100, 3);
  const auto produce = [&](std::string_view format) {
    const test::Converted result = convert(payload, "bin", format, {}, "payload.bin");
    REQUIRE_MESSAGE(result.ok(), format, ": ", result.message);
    return result.data;
  };

  for (const std::string_view format : {"hexdump", "data-uri", "uue", "checksums", "c-header", "yenc", "cpp-array",
                                        "python-bytes", "js-array", "java-array", "go-array", "rust-array", "swift-array",
                                        "csharp-array"}) {
    CAPTURE(format);
    const Bytes data = produce(format);
    CHECK(detected(data, "notes.txt") == format);
    CHECK(detected(data, "holiday photo.png") == format);
    CHECK(detected(data, "") == format);
    const FormatInfo* info = Registry::instance().find(format);
    REQUIRE(info != nullptr);
    CHECK(detected(data, "payload." + info->extensions.front()) == format);
    // Auto-detection is what the engine uses when no source format is given.
    const test::Converted via_engine = convert(data, "", "base64", {}, "notes.txt");
    REQUIRE(via_engine.ok());
  }

  // Detected yEnc decodes without being told the format.
  CHECK(convert(produce("yenc"), "", "bin", {}, "attachment.txt").data == payload);
  CHECK(convert(produce("hexdump"), "", "bin", {}, "dump.txt").data == payload);

  // Plain prose stays plain text, and real source code is not mistaken for an array literal.
  CHECK(detected("Dear diary,\n\ntoday I converted a file.\n", "notes.txt") == "txt");
  CHECK(detected("Dear diary,\n\ntoday I converted a file.\n", "") == "txt");
  CHECK(detected("import os\n\nprint(os.getcwd())\n", "script.py") == "py");
  CHECK(detected("const answer = 42;\nconsole.log(answer);\n", "app.js") == "js");
  CHECK(detected("#pragma once\n#include <vector>\nstd::vector<int> numbers();\n", "numbers.hpp") == "cpp");
  CHECK(detected("package main\n\nfunc main() {}\n", "main.go") == "go");
  CHECK(detected("fn main() {}\n", "main.rs") == "rs");
  CHECK(detected("public class Main {}\n", "Main.java") == "java");
  CHECK(detected("int main(void) { return 0; }\n", "main.h") == "c");

  // Encodings without a content test are found by their extension.
  CHECK(detected("MZXW6YTBOI======", "secret.b32") == "base32");
  CHECK(detected("2NEpo7TZRRrLZSi2U", "address.b58") == "base58");
  CHECK(detected("... --- ...", "signal.morse") == "morse");
  CHECK(detected("H4sIAAAAAAAC/8tIzcnJV8hAJwHjUT2NFwAAAA==", "bundle.gz.b64") == "gzip-base64");
  CHECK(detected("Zm9vYmFy", "plain.b64") == "base64");
}

TEST_CASE("encoding: every format of the family is described") {
  const Registry& registry = Registry::instance();
  std::vector<std::string_view> ids = {"bin", "txt"};
  for (const BinaryFormat& format : binary_formats) ids.push_back(format.id);
  for (const TextFormat& format : text_formats) ids.push_back(format.id);
  for (const std::string_view id : ids) {
    CAPTURE(id);
    const FormatInfo* info = registry.find(id);
    REQUIRE(info != nullptr);
    CHECK_FALSE(info->name.empty());
    CHECK_FALSE(info->description.empty());
    CHECK(info->description.back() == '.');
    CHECK_FALSE(info->extensions.empty());
    CHECK_FALSE(info->mime.empty());
    CHECK(info->popularity >= 5);
    CHECK(info->popularity <= 90);
    CHECK(registry.has_writer(id));
    // "txt" as an extension is only allowed together with a content test.
    if (id != "txt" && std::ranges::find(info->extensions, "txt") != info->extensions.end()) CHECK(static_cast<bool>(info->sniff));
    for (const Codec* codec : registry.codecs_for(id)) {
      for (const std::vector<OptionSpec>* specs : {&codec->read_options, &codec->write_options}) {
        for (const OptionSpec& spec : *specs) {
          CAPTURE(spec.key);
          CHECK_FALSE(spec.label.empty());
          CHECK(util::ascii_upper(spec.label.front()) == spec.label.front());
          CHECK_FALSE(spec.description.empty());
          CHECK(spec.description.back() == '.');
        }
      }
    }
  }
  // The formats added on top of the original set sit in the 5..40 popularity band.
  for (const std::string_view id : {"base32", "base32hex", "base32-crockford", "base58", "base36", "ascii85", "z85", "octal",
                                    "decimal", "quoted-printable", "yenc", "gzip-base64", "cpp-array", "python-bytes",
                                    "js-array", "java-array", "go-array", "rust-array", "swift-array", "csharp-array",
                                    "html-entities", "url-text", "unicode-escapes", "json-string", "rot13", "rot47",
                                    "punycode", "morse", "nato"}) {
    const FormatInfo* info = registry.find(id);
    REQUIRE_MESSAGE(info != nullptr, id);
    CHECK_MESSAGE(info->popularity <= 40, id);
    CHECK_MESSAGE(info->category == Category::Encoding, id);
  }
}

TEST_CASE("encoding: output limits are enforced before and after encoding") {
  Limits tight;
  tight.max_output_bytes = 1000;
  const Bytes data = test::random_bytes(900, 12);
  for (const BinaryFormat& format : binary_formats) {
    if (format.id == "checksums") continue;  // fixed-size report
    CAPTURE(format.id);
    const test::Converted result = convert(data, "bin", format.id, {}, {}, tight);
    CHECK(result.status == Status::Limit);
  }
  CHECK(convert(data, "png", "bin", {}, {}, tight).ok());
  CHECK(convert(data, "bin", "checksums", {}, {}, tight).ok());

  const std::string text(900, '&');
  for (const std::string_view id : {"html-entities", "url-text", "unicode-escapes", "json-string", "punycode", "morse", "nato"}) {
    CAPTURE(id);
    CHECK(convert(text, "txt", id, {}, {}, tight).status == Status::Limit);
  }
  CHECK(convert(text, "txt", "rot13", {}, {}, tight).ok());

  // Decoded output is limited as well.
  Limits small_output;
  small_output.max_decompressed_bytes = 10;
  CHECK(convert("MZXW6YTBMZXW6YTBMZXW6YTB", "base32", "bin", {}, {}, small_output).status == Status::Limit);
}

TEST_CASE("encoding: the registry is consistent") {
  const std::vector<std::string> problems = Registry::instance().validate();
  for (const std::string& problem : problems) MESSAGE(problem);
  CHECK(problems.empty());
}
