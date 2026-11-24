// Tests of the WebAssembly disassembler (wasm.cpp) against modules assembled
// by wabt's wat2wasm and disassembled by its wasm2wat
// (tests/data/code/wasm/SOURCES.md).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/code/wasm.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace std::string_view_literals;
using test::convert;

TEST_CASE("code/wasm: disassembly is identical to wasm2wat") {
  for (const std::string_view name : {"basics", "features"}) {
    INFO(name);
    const Bytes module = test::fixture("code/wasm/" + std::string(name) + ".wasm");
    const auto wat = code::disassemble_wasm(module, Context{});
    REQUIRE_OK(wat);
    CHECK(*wat == test::fixture_text("code/wasm/" + std::string(name) + ".wasm2wat.wat"));
    // Through the engine: detected by its magic number, converted to the text format.
    const test::Converted result = convert(module, "", "wat", {}, "module.bin");
    REQUIRE(result.ok());
    CHECK(result.text() == *wat);
  }
  const auto detection = detect_format(Registry::instance(), test::fixture("code/wasm/basics.wasm"), "x");
  REQUIRE(detection.has_value());
  CHECK(detection->format->id == "wasm");
  // As a highlighted document.
  const std::string html = convert(test::fixture("code/wasm/basics.wasm"), "wasm", "html-highlight").text();
  CHECK(html.find("language-wat") != std::string::npos);
  CHECK(html.find("<span class=\"hl-keyword\">func</span>") != std::string::npos);
  // The source text is readable as code too.
  CHECK(convert(test::fixture("code/wasm/basics.wat"), "wat", "txt").ok());
}

TEST_CASE("code/wasm: malformed and unsupported modules are errors") {
  const Bytes module = test::fixture("code/wasm/features.wasm");
  CHECK_STATUS(code::disassemble_wasm(test::bytes("\0asm\x02\0\0\0"sv), Context{}), Status::Decode);
  CHECK_STATUS(code::disassemble_wasm(test::bytes("not wasm"), Context{}), Status::Decode);
  std::size_t complete = 0;  // prefixes that end exactly at a section boundary are valid modules
  for (std::size_t n = 9; n < module.size(); ++n) {
    const auto prefix = code::disassemble_wasm(ByteView(module).first(n), Context{});
    if (prefix) {
      ++complete;
    } else {
      CHECK(prefix.error().code == Status::Decode);
    }
  }
  CHECK(complete <= 3);
  // A function body with a SIMD instruction (v128.const).
  const Bytes simd = test::bytes(
      "\0asm\x01\0\0\0"
      "\x01\x04\x01\x60\x00\x00"                  // type: () -> ()
      "\x03\x02\x01\x00"                          // one function of type 0
      "\x0a\x07\x01\x05\x00\xfd\x0c\x00\x0b"sv);  // body: v128.const (truncated) end
  const auto result = code::disassemble_wasm(simd, Context{});
  CHECK_STATUS(result, Status::Decode);
  CHECK(result.error().message.find("SIMD") != std::string::npos);
  // A huge declared count is rejected before anything is allocated.
  CHECK_STATUS(code::disassemble_wasm(test::bytes("\0asm\x01\0\0\0\x01\x05\xff\xff\xff\xff\x0f"sv), Context{}), Status::Decode);
  CHECK(test::fuzz_reader<ir::PlainText>("wasm", module) > 0);
}
