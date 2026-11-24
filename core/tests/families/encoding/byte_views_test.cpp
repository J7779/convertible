// Tests of the byte views (byte_views.cpp): dumps compared byte for byte with
// util-linux hexdump and GNU od (tests/data/encoding/dumps/SOURCES.md), their
// reverse, the assembler/include listings, the JSON envelope, the new array
// literal languages and MIME sniffing for data: URIs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace std::string_view_literals;
using namespace std::string_literals;
using test::convert;

namespace {
Bytes sample() { return test::fixture("encoding/bases/sample.bin"); }
Bytes repeats() { return test::fixture("encoding/dumps/repeats.bin"); }
std::string dump(std::string_view name) { return test::fixture_text("encoding/dumps/" + std::string(name)); }
}  // namespace

TEST_CASE("encoding/hexdump-c: identical to hexdump -C, and reversible") {
  CHECK(convert(sample(), "bin", "hexdump-c").text() == dump("sample.hexdump-c"));
  CHECK(convert(repeats(), "bin", "hexdump-c").text() == dump("repeats.hexdump-c"));
  CHECK(convert(dump("sample.hexdump-c"), "hexdump-c", "bin").data == sample());
  CHECK(convert(dump("repeats.hexdump-c"), "hexdump-c", "bin").data == repeats());
  CHECK(convert(dump("repeats.hexdump-c"), "", "bin", {}, "dump.txt").data == repeats());  // detected
  const std::string full = convert(repeats(), "bin", "hexdump-c", R"({"squeeze":false})").text();
  CHECK(full.find("\n*\n") == std::string::npos);
  CHECK(convert(full, "hexdump-c", "bin").data == repeats());
  CHECK(convert(ByteView{}, "bin", "hexdump-c").text().empty());
  CHECK(convert("00000000  4x\n", "hexdump-c", "bin").status == Status::Decode);
}

TEST_CASE("encoding/od: identical to od, od -t x1 and od -t x2, and reversible") {
  CHECK(convert(sample(), "bin", "od").text() == dump("sample.od"));
  CHECK(convert(repeats(), "bin", "od").text() == dump("repeats.od"));
  CHECK(convert(sample(), "bin", "od", R"({"type":"x1"})").text() == dump("sample.od-x1"));
  CHECK(convert(repeats(), "bin", "od", R"({"type":"x2"})").text() == dump("repeats.od-x2"));
  for (const std::string_view name : {"sample.od", "repeats.od", "sample.od-x1", "repeats.od-x2"}) {
    INFO(name);
    CHECK(convert(dump(name), "od", "bin").data == (name.starts_with("sample") ? sample() : repeats()));
  }
  // An odd length: od pads the last word, the final offset restores the size.
  const Bytes odd = test::random_bytes(33, 2);
  CHECK(convert(convert(odd, "bin", "od").data, "od", "bin").data == odd);
  CHECK(test::fuzz_reader<ir::Blob>("od", as_bytes(dump("repeats.od"))) > 0);
  // A '*' run cannot expand past the limits.
  Limits small;
  small.max_decompressed_bytes = 1000;
  CHECK(convert("0000000 000000 000000 000000 000000 000000 000000 000000 000000\n*\n7777777\n", "od", "bin", {}, {},
                small)
            .status == Status::Limit);
}

TEST_CASE("encoding/asm-bytes and encoding/bytes-inc: listings read back") {
  const Bytes data = test::random_bytes(40, 8);
  for (const std::string_view syntax : {"nasm", "gas", "masm"}) {
    INFO(syntax);
    const test::Converted listing = convert(data, "bin", "asm-bytes", std::format(R"({{"syntax":"{}"}})", syntax), "blob.bin");
    REQUIRE(listing.ok());
    CHECK(convert(listing.data, "asm-bytes", "bin").data == data);
  }
  CHECK(convert("AB", "bin", "asm-bytes", R"({"label":"x"})").text() == "global x\nx:\n    db 0x41, 0x42\nx_len equ $ - x\n");
  CHECK(convert("AB", "bin", "asm-bytes", R"({"syntax":"gas","label":"x"})").text() ==
        "    .global x\nx:\n    .byte 0x41, 0x42\n    .set x_len, . - x\n");
  CHECK(convert("\xFF"sv, "bin", "asm-bytes", R"({"syntax":"masm","label":"x"})").text() == "x\n    db 0FFh\n");
  CHECK(convert("ABC", "bin", "bytes-inc").text() == "0x41, 0x42, 0x43,\n");
  CHECK(convert(convert(data, "bin", "bytes-inc").data, "bytes-inc", "bin").data == data);
  CHECK(convert("0x41, 300\n", "bytes-inc", "bin").status == Status::Decode);
}

TEST_CASE("encoding/base64-json: envelope with media type and checksum") {
  // A PNG signature is recognised by the registry even with a neutral file name.
  const std::string png = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x01\0\0\0\x01\x08\x06\0\0\0\x1f\x15\xc4\x89"s;
  const std::string json = convert(png, "bin", "base64-json", {}, "pixel.dat").text();
  CHECK(json.find("\"filename\": \"pixel.dat\"") != std::string::npos);
  CHECK(json.find("\"mime\": \"image/png\"") != std::string::npos);
  CHECK(json.find("\"size\": 33") != std::string::npos);
  const test::Converted back = convert(json, "", "bin", {}, "pixel.json");
  REQUIRE(back.ok());
  CHECK(test::str(back.data) == png);
  std::string tampered = json;
  tampered.replace(tampered.find("\"data\": \"") + 9, 1, "A");
  CHECK(convert(tampered, "base64-json", "bin").status == Status::Decode);
  CHECK(convert("{\"x\":1}", "base64-json", "bin").status == Status::Decode);
  // data: URIs sniff the media type the same way.
  CHECK(convert(png, "bin", "data-uri", {}, "pixel.dat").text().starts_with("data:image/png;base64,"));
}

TEST_CASE("encoding/array literals: TypeScript, Kotlin, PHP, Ruby and Zig") {
  const Bytes data = {std::byte{0x00}, std::byte{0x7F}, std::byte{0x80}, std::byte{0xFF}};
  CHECK(convert(data, "bin", "ts-array", R"({"variable_name":"blob"})", "").text() ==
        "export const blob: Uint8Array = new Uint8Array([\n  0x00, 0x7f, 0x80, 0xff,\n]);\n");
  CHECK(convert(data, "bin", "kotlin-array", R"({"variable_name":"blob"})", "").text() ==
        "val blob = byteArrayOf(\n    0x00, 0x7f, 0x80.toByte(), 0xff.toByte(),\n)\n");
  CHECK(convert(data, "bin", "php-array", R"({"variable_name":"blob"})", "x.bin").text() ==
        "<?php\n\n// x.bin, 4 bytes\n$blob = [\n    0x00, 0x7f, 0x80, 0xff,\n];\n");
  CHECK(convert(data, "bin", "ruby-bytes", R"({"variable_name":"blob"})", "").text() ==
        "BLOB = [\n  0x00, 0x7f, 0x80, 0xff,\n].pack('C*').freeze\n");
  CHECK(convert(data, "bin", "zig-array", R"({"variable_name":"test"})", "").text() ==
        "pub const test_ = [_]u8{\n    0x00, 0x7f, 0x80, 0xff,\n};\n");  // `test` is a Zig keyword
  // Each format is detected as itself, not as a neighbour.
  for (const std::string_view format : {"ts-array", "js-array", "kotlin-array", "php-array", "ruby-bytes", "zig-array"}) {
    INFO(format);
    const test::Converted out = convert(data, "bin", format);
    REQUIRE(out.ok());
    const auto detection = detect_format(Registry::instance(), out.data, std::string("blob.") + out.extension);
    REQUIRE(detection.has_value());
    CHECK(detection->format->id == format);
  }
}

TEST_CASE("encoding/hex: prefixes and separators") {
  CHECK(convert("AB", "bin", "hex", R"({"prefix":"0x","separator":"comma-space"})").text() == "0x41, 0x42");
  CHECK(convert("AB", "bin", "hex", R"({"prefix":"\\x"})").text() == "\\x41\\x42");
  CHECK(convert("AB", "bin", "hex", R"({"prefix":"%","uppercase":true})").text() == "%41%42");
  CHECK(convert("AB", "bin", "hex", R"({"separator":"dash"})").text() == "41-42");
  for (const std::string_view text : {"0x41, 0x42", "\\x41\\x42", "%41%42", "41-42", "41;42"}) {
    CHECK(convert(text, "hex", "bin").text() == "AB");
  }
}
