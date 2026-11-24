// JSON and its dialects: exact numbers, Unicode, options, errors, limits.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/data/data_test_support.hpp"

using namespace cvt;
namespace td = cvt::test::data;

TEST_CASE("data/json: numbers are exact") {
  const auto value = td::read("json", R"([9223372036854775807, -9223372036854775808, 18446744073709551615,
      18446744073709551616, -9223372036854775809, 0.1, 1e400, 1e-400, 0.1000000000000000000001, -0, -0.0, 1E+2, 5e-324])");
  REQUIRE_OK(value);
  CHECK((*value)[0] == Json(std::int64_t{9223372036854775807}));
  CHECK((*value)[1] == Json(std::numeric_limits<std::int64_t>::min()));
  CHECK((*value)[2] == Json(std::uint64_t{18446744073709551615u}));
  CHECK((*value)[3] == td::json(R"({"$bigint": "18446744073709551616"})"));
  CHECK((*value)[4] == td::json(R"({"$bigint": "-9223372036854775809"})"));
  CHECK((*value)[5].is_float());
  CHECK((*value)[6] == td::json(R"({"$decimal": "1e400"})"));
  CHECK((*value)[7] == td::json(R"({"$decimal": "1e-400"})"));
  CHECK((*value)[8] == td::json(R"({"$decimal": "0.1000000000000000000001"})"));
  CHECK((*value)[9].is_integer());
  CHECK(std::signbit((*value)[10].as_double()));
  CHECK((*value)[11].as_double() == 100.0);
  CHECK((*value)[12].as_double() == 5e-324);
  // And they are written back digit for digit.
  const auto text = td::write("json", *value, R"({"minify": true})");
  REQUIRE_OK(text);
  CHECK(*text == "[9223372036854775807,-9223372036854775808,18446744073709551615,18446744073709551616,"
                 "-9223372036854775809,0.1,1e400,1e-400,0.1000000000000000000001,0,-0.0,100.0,5e-324]");
  // Rounding is available on request.
  const auto rounded = td::read("json", "[18446744073709551616, 0.1000000000000000000001]", R"({"big_numbers": "double"})");
  REQUIRE_OK(rounded);
  CHECK((*rounded)[0].is_float());
  CHECK_STATUS(td::read("json", "[1e400]", R"({"big_numbers": "error"})"), Status::Decode);
  CHECK(td::read("json", "[1e400]", R"({"big_numbers": "string"})").value()[0] == Json("1e400"));
}

TEST_CASE("data/json: Unicode, escapes and surrogates") {
  const auto value = td::read("json", R"(["\u00e9\ud83d\ude00\/\b\f\n\r\t\"\\", "\u0000", "direct é 😀"])");
  REQUIRE_OK(value);
  CHECK((*value)[0].as_string() == "\xC3\xA9\xF0\x9F\x98\x80/\b\f\n\r\t\"\\");
  CHECK((*value)[1].as_string() == std::string(1, '\0'));
  std::vector<std::string> warnings;
  const auto lone = td::read("json", R"(["\ud800x"])", {}, &warnings);
  REQUIRE_OK(lone);
  CHECK((*lone)[0].as_string() == "\xEF\xBF\xBDx");
  CHECK(warnings.size() == 1);
  // ASCII-only output escapes beyond the BMP as surrogate pairs.
  CHECK(td::write("json", *value, R"({"ascii_only": true, "minify": true})").value() ==
        R"(["\u00e9\ud83d\ude00/\b\f\n\r\t\"\\","\u0000","direct \u00e9 \ud83d\ude00"])");
  // UTF-16 input with a BOM is decoded; invalid UTF-8 is an error with a position.
  const std::string utf16 = std::string("\xFF\xFE", 2) + std::string("[\0\"\0\xE9\0\"\0]\0", 10);
  CHECK(td::read("json", utf16).value()[0].as_string() == "\xC3\xA9");
  const auto invalid = td::read("json", "[\"\xFF\"]");
  REQUIRE_STATUS(invalid, Status::Decode);
  CHECK(td::mentions_position(invalid.error(), 1, 3));
}

TEST_CASE("data/json: strict and lenient syntax") {
  const std::string_view relaxed = "// comment\n{\"a\": [1, 2,], /* x */ \"b\": NaN, \"c\": -Infinity,}";
  std::vector<std::string> warnings;
  const auto lenient = td::read("json", relaxed, {}, &warnings);
  REQUIRE_OK(lenient);
  CHECK(std::isnan((*lenient)["b"].as_double()));
  CHECK(warnings.size() == 1);
  const auto strict = td::read("json", relaxed, R"({"strict": true})");
  REQUIRE_STATUS(strict, Status::Decode);
  CHECK(td::mentions_position(strict.error(), 1, 1));
}

TEST_CASE("data/json: errors name the line and column") {
  struct Bad {
    std::string_view json;
    std::size_t line;
    std::size_t column;
  };
  const Bad cases[] = {
      {"{\"a\": 1,\n \"b\" 2}", 2, 6},  {"[1, 2", 1, 6},       {"[01]", 1, 2},       {"{'a': 1}", 1, 2},
      {"[\"a\nb\"]", 1, 4},             {"[1] [2]", 1, 5},     {"", 1, 1},           {"[\"\\x41\"]", 1, 3},
      {"[tru]", 1, 2},                  {"{\"a\":1,\"a\":2}", 1, 8},
  };
  for (const Bad& bad : cases) {
    INFO("JSON: ", bad.json);
    const auto result = td::read("json", bad.json, R"({"duplicate_keys": "error", "strict": true})");
    REQUIRE_STATUS(result, Status::Decode);
    CHECK_MESSAGE(td::mentions_position(result.error(), bad.line, bad.column), result.error().message);
  }
}

TEST_CASE("data/json: duplicate keys follow the policy") {
  CHECK(td::read("json", R"({"a": 1, "b": 2, "a": 3})").value() == td::json(R"({"a": 3, "b": 2})"));
  CHECK(td::read("json", R"({"a": 1, "b": 2, "a": 3})", R"({"duplicate_keys": "first"})").value() ==
        td::json(R"({"a": 1, "b": 2})"));
  CHECK_STATUS(td::read("json", R"({"a": 1, "a": 3})", R"({"duplicate_keys": "error"})"), Status::Decode);
}

TEST_CASE("data/json: nesting limits") {
  CHECK_STATUS(td::read("json", std::string(300, '[') + std::string(300, ']')), Status::Limit);
  CHECK_OK(td::read("json", std::string(250, '[') + std::string(250, ']')));
  CHECK_OK(td::read("json", std::string(1000, '[') + std::string(1000, ']'), R"({"max_depth": 1500})"));
  CHECK_STATUS(td::read("json", "[[[1]]]", R"({"max_depth": 2})"), Status::Limit);
  // A tree deeper than the writers handle is refused, not overflowed.
  Json deep = Json(1);
  for (int i = 0; i < 1100; ++i) deep = Json::array({std::move(deep)});
  CHECK_STATUS(td::write("json", deep), Status::Encode);
}

TEST_CASE("data/json: writer options") {
  const Json value = td::json(R"({"b": [1, {"c": "/"}], "a": {}, "n": {"$float": "NaN"}, "bin": {"$binary": "AAE="}})");
  CHECK(td::write("json", value).value() ==
        "{\n  \"b\": [\n    1,\n    {\n      \"c\": \"/\"\n    }\n  ],\n  \"a\": {},\n  \"n\": {\n    \"$float\": \"NaN\"\n  },\n"
        "  \"bin\": {\n    \"$binary\": \"AAE=\"\n  }\n}\n");
  CHECK(td::write("json", value, R"({"minify": true, "sort_keys": true, "escape_slash": true})").value() ==
        R"({"a":{},"b":[1,{"c":"\/"}],"bin":{"$binary":"AAE="},"n":{"$float":"NaN"}})");
  CHECK(td::write("json", value, R"({"minify": true, "non_finite": "null"})").value().find("\"n\":null") != std::string::npos);
  CHECK(td::write("json", value, R"({"minify": true, "non_finite": "literal"})").value().find("\"n\":NaN") != std::string::npos);
  CHECK(td::write("json", value, R"({"minify": true, "non_finite": "string"})").value().find("\"n\":\"NaN\"") != std::string::npos);
  CHECK_STATUS(td::write("json", value, R"({"non_finite": "error"})"), Status::Encode);
  CHECK(td::write("json", value, R"({"minify": true, "tagged_values": "plain"})").value().find("\"bin\":\"AAE=\"") != std::string::npos);
  CHECK_STATUS(td::write("json", value, R"({"tagged_values": "error"})"), Status::Encode);
  CHECK(td::write("json", value, R"({"indent": 0})").value().starts_with("{\n\"b\": [\n1,"));
  CHECK_STATUS(td::write("json", value, R"({"non_finite": "bogus"})"), Status::InvalidArgument);
}

TEST_CASE("data/json: special values on reading") {
  const std::string_view text = R"({"b": {"$binary": "AAE="}, "d": {"$date": "2024-05-01"}, "f": {"$float": "-Infinity"}})";
  const auto tagged = td::read("json", text);
  REQUIRE_OK(tagged);
  CHECK(tagged.value()["b"] == td::json(R"({"$binary": "AAE="})"));
  CHECK(std::isinf((*tagged)["f"].as_double()));  // $float always becomes a double
  const auto plain = td::read("json", text, R"({"special_values": "plain"})");
  REQUIRE_OK(plain);
  CHECK((*plain)["b"] == Json("AAE="));
  CHECK((*plain)["d"] == Json("2024-05-01"));
  CHECK_STATUS(td::read("json", text, R"({"special_values": "error"})"), Status::Decode);
}

TEST_CASE("data/json5: the JSON5 extensions") {
  const auto value = td::read("json5", R"(// comment
{
  unquoted: 'single \'quoted\'',
  $dollar_key: 0x1F,
  leading: .5, trailing: 5., plus: +1, negHex: -0xFF,
  inf: Infinity, ninf: -Infinity, nan: NaN,
  "multi": "line \
continued",
  escapes: '\x41\0\v',
  trailingComma: [1, 2,],
  ünïcode: 1,
  /* block */
})");
  REQUIRE_OK(value);
  const Json expected = td::json(R"({"unquoted": "single 'quoted'", "$dollar_key": 31, "leading": 0.5, "trailing": 5.0,
      "plus": 1, "negHex": -255, "inf": {"$float": "Infinity"}, "ninf": {"$float": "-Infinity"}, "nan": {"$float": "NaN"},
      "multi": "line continued", "escapes": "A\u0000\u000b", "trailingComma": [1, 2], "\u00fcn\u00efcode": 1})");
  CHECK_MESSAGE(test::data_equal(*value, expected), test::data_diff(*value, expected));
  // The writer uses identifiers for keys and literals for specials.
  CHECK(td::write("json5", td::json(R"({"a b": 1, "ok": {"$float": "NaN"}, "if": 2})"), R"({"minify": true})").value() ==
        R"({"a b":1,ok:NaN,"if":2})");
  CHECK_STATUS(td::read("json5", "{a: 01}"), Status::Decode);
  CHECK_STATUS(td::read("json5", "{a: '\\1'}"), Status::Decode);
}

TEST_CASE("data/jsonc: comments and trailing commas") {
  const auto value = td::read("jsonc", "{\n  // editor setting\n  \"tabSize\": 2, /* inline */\n  \"files\": [\"a\",],\n}\n");
  REQUIRE_OK(value);
  CHECK(*value == td::json(R"({"tabSize": 2, "files": ["a"]})"));
  CHECK_STATUS(td::read("jsonc", "{a: 1}"), Status::Decode);
}

TEST_CASE("data/ndjson and json-seq: records") {
  const auto lines = td::read("jsonl", "{\"a\":1}\r\n\n  {\"a\":2}  \n[3]\n");
  REQUIRE_OK(lines);
  CHECK(*lines == td::json(R"([{"a": 1}, {"a": 2}, [3]])"));
  const auto broken = td::read("jsonl", "{\"a\":1}\n{\"a\":\n2}\n");
  REQUIRE_STATUS(broken, Status::Decode);
  CHECK(td::mentions_position(broken.error(), 2, 1));
  CHECK(td::write("jsonl", td::json(R"([{"a": 1}, "x"])")).value() == "{\"a\":1}\n\"x\"\n");
  const std::string seq = "\x1E{\"a\":1}\n\x1E[2]\n\x1E{broken\n\x1E" "3\n";
  std::vector<std::string> warnings;
  const auto records = td::read("json-seq", seq, {}, &warnings);
  REQUIRE_OK(records);
  CHECK(*records == td::json(R"([{"a": 1}, [2], 3])"));
  CHECK(warnings.size() == 1);
  CHECK(td::write("json-seq", td::json("[1, 2]")).value() == "\x1E" "1\n\x1E" "2\n");
}

TEST_CASE("data/hjson: quoteless syntax") {
  const auto value = td::read("hjson", R"(# comment
key: value with spaces, and comma
number: 42
notnumber: 42 apples
quoted: "a\tb"
multi:
  '''
  first
    indented
  last
  '''
list: [
  1
  two
]
// end
)");
  REQUIRE_OK(value);
  const Json expected = td::json(R"({"key": "value with spaces, and comma", "number": 42, "notnumber": "42 apples",
      "quoted": "a\tb", "multi": "first\n  indented\nlast", "list": [1, "two"]})");
  CHECK_MESSAGE(test::data_equal(*value, expected), test::data_diff(*value, expected));
  const auto back = td::round_trip("hjson", expected);
  REQUIRE_OK(back);
  CHECK(*back == expected);
}
