// Hostile input for the text readers: nesting bombs, truncation, noise.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/data/data_test_support.hpp"

using namespace cvt;
namespace td = cvt::test::data;

namespace {

std::string repeat(std::string_view piece, std::size_t n) {
  std::string out;
  out.reserve(piece.size() * n);
  for (std::size_t i = 0; i < n; ++i) out += piece;
  return out;
}

}  // namespace

TEST_CASE("data/hostile: every recursive reader stops at the nesting limit") {
  constexpr std::size_t levels = 20000;
  struct Bomb {
    std::string_view format;
    std::string text;
  };
  const Bomb bombs[] = {
      {"json", repeat("[", levels)},
      {"json5", repeat("[", levels)},
      {"jsonc", repeat("{\"a\":", levels)},
      {"hjson", repeat("[", levels)},
      {"yaml", repeat("[", levels)},
      {"yaml", repeat("{a: ", levels)},
      {"yaml", repeat("- ", levels)},
      {"toml", "a = " + repeat("[", levels)},
      {"toml", "a = " + repeat("{b = ", levels)},
      {"edn", repeat("[", levels)},
      {"sexp", repeat("(", levels)},
      {"kdl", repeat("a {\n", levels)},
      {"hocon", repeat("a {", levels)},
      {"ron", repeat("[", levels)},
      {"cson", repeat("[", levels)},
      {"lua-table", repeat("{", levels)},
      {"python-literal", repeat("[", levels)},
      {"php-serialized", repeat("a:1:{i:0;", levels)},
      {"ion", repeat("[", levels)},
      {"openstep-plist", repeat("(", levels)},
      {"plist", "<plist>" + repeat("<array>", levels)},
      {"xml", repeat("<a>", levels)},
      {"querystring", "a" + repeat("[b]", levels) + "=1"},
  };
  for (const Bomb& bomb : bombs) {
    INFO("format ", bomb.format);
    const auto result = td::read(bomb.format, bomb.text);
    REQUIRE_FALSE(result.has_value());
    CHECK((result.error().code == Status::Limit || result.error().code == Status::Decode));
  }
}

TEST_CASE("data/hostile: truncations, corruptions and noise never crash a text reader") {
  const Bytes noise = test::random_bytes(4096, 7);
  const std::pair<std::string_view, std::string_view> samples[] = {
      {"yaml", "a: &x [1, {b: c}]\nd: *x\ne: |\n  text\n"},
      {"toml", "[a]\nb = [1, {c = 'd'}]\n[[e]]\nf = 1979-05-27T07:32:00Z\n"},
      {"ini", "[s]\nk = v\n"},
      {"properties", "a = b\\\n  c\n"},
      {"env", "A=\"x\"\n"},
      {"hocon", "a { b = ${c} }\nc = 1\n"},
      {"kdl", "a 1 b=2 { c \"d\" }\n"},
      {"edn", "{:a [1 2 #{3}]}"},
      {"sexp", "((a 1) (b (2 3)))"},
      {"ron", "S(a: [1, 2], b: Some(\"x\"))"},
      {"cson", "a:\n  b: [1, 2]\n"},
      {"lua-table", "return { a = { 1, 2 }, ['b'] = [[x]] }"},
      {"php-serialized", "a:1:{s:1:\"a\";a:1:{i:0;d:1.5;}}"},
      {"python-literal", "{'a': [1, (2, 3)], 'b': b'x'}"},
      {"json5", "{a: [1, 'x', 0x1F]}"},
      {"hjson", "a: b\nc: [1, 2]\n"},
      {"ion", "{a: [1, 2d0, 2007T, {{aGk=}}]}"},
      {"xml", "<a b=\"1\"><c>d</c></a>"},
      {"openstep-plist", "{ a = (1, 2); }"},
      {"querystring", "a[b][]=1&c=2"},
      {"http-headers", "A: b\r\nC: d\r\n"},
      {"reg", "Windows Registry Editor Version 5.00\r\n[K]\r\n\"a\"=dword:00000001\r\n"},
      {"jsonl", "{\"a\":1}\n{\"b\":2}\n"},
      {"json-seq", "\x1e{\"a\":1}\n"},
  };
  for (const auto& [format, text] : samples) {
    INFO("format ", format);
    CHECK_OK(td::read(format, text));
    CHECK(test::fuzz_reader<ir::Data>(format, as_bytes(text)) > 0);
    (void)td::read_bytes(format, noise);
  }
}
