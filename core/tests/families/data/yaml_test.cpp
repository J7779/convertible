// YAML: an edge-case suite written for this project (not taken from the
// upstream test suite), covering the syntax the reader accepts, the schema
// resolution, aliases and their limits, and the writer's quoting.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/data/data_test_support.hpp"

using namespace cvt;
namespace td = cvt::test::data;

namespace {

/// Read YAML and compare with JSON.
void expect_yaml(std::string_view yaml, std::string_view json, std::string_view options = {}) {
  INFO("YAML:\n", yaml);
  const auto value = td::read("yaml", yaml, options);
  REQUIRE_OK(value);
  const Json expected = td::json(json);
  CHECK_MESSAGE(test::data_equal(*value, expected), test::data_diff(*value, expected), "\n got: ", util::dump_json(*value));
}

}  // namespace

TEST_CASE("data/yaml: block mappings and sequences") {
  expect_yaml("a: 1\nb:\n  c: 2\n  d: [3, 4]\n", R"({"a": 1, "b": {"c": 2, "d": [3, 4]}})");
  expect_yaml("- a\n- - b\n  - c\n- d: 1\n  e: 2\n", R"(["a", ["b", "c"], {"d": 1, "e": 2}])");
  expect_yaml("key:\n- one\n- two\nnext: x\n", R"({"key": ["one", "two"], "next": "x"})");  // compact sequence
  expect_yaml("key:\n  - one\n  - two\n", R"({"key": ["one", "two"]})");
  expect_yaml("a:\nb: \n", R"({"a": null, "b": null})");
  expect_yaml("? complex\n: value\n? [x, y]\n: list key\n", R"({"complex": "value", "[\"x\",\"y\"]": "list key"})");
  expect_yaml("\"quoted key\": 1\n'single': 2\n", R"({"quoted key": 1, "single": 2})");
}

TEST_CASE("data/yaml: plain scalars, folding and comments") {
  expect_yaml("text: one\n  two\n\n  three\n", R"({"text": "one two\nthree"})");
  expect_yaml("url: http://example.invalid/a#b\n", R"({"url": "http://example.invalid/a#b"})");
  expect_yaml("value: x # comment\n# full line\nnext: y\n", R"({"value": "x", "next": "y"})");
  expect_yaml("colon: a:b\nhash: a#b\n", R"({"colon": "a:b", "hash": "a#b"})");
  expect_yaml("- -1\n- -x\n- ?x\n- :x\n", R"([-1, "-x", "?x", ":x"])");
}

TEST_CASE("data/yaml: quoted scalars and escapes") {
  expect_yaml(R"(s: 'it''s')", R"({"s": "it's"})");
  expect_yaml("d: \"tab\\tnl\\nq\\\"bs\\\\ \\x41\\u00e9\\U0001F600\\N\\_\\L\\P\\0\\e\"\n",
              R"({"d": "tab\tnl\nq\"bs\\ A\u00e9\ud83d\ude00\u0085\u00a0\u2028\u2029\u0000\u001b"})");
  expect_yaml("d: \"surrogates \\ud83d\\ude00\"\n", R"({"d": "surrogates \ud83d\ude00"})");
  expect_yaml("d: \"folded\n  line\n\n  para\"\n", R"({"d": "folded line\npara"})");
  expect_yaml("d: \"join\\\n  ed\"\n", R"({"d": "joined"})");
  expect_yaml("s: 'multi\n  line'\n", R"({"s": "multi line"})");
  expect_yaml("s: \"  spaces kept  \"\n", R"({"s": "  spaces kept  "})");
}

TEST_CASE("data/yaml: literal and folded block scalars with chomping") {
  expect_yaml("a: |\n  line 1\n  line 2\n", R"({"a": "line 1\nline 2\n"})");
  expect_yaml("a: |-\n  strip\n\n", R"({"a": "strip"})");
  expect_yaml("a: |+\n  keep\n\n\nb: 1\n", R"({"a": "keep\n\n\n", "b": 1})");
  expect_yaml("a: >\n  folded\n  text\n\n  next\n    more\n  end\n", R"({"a": "folded text\nnext\n  more\nend\n"})");
  expect_yaml("a: >-\n  one\n  two\n", R"({"a": "one two"})");
  expect_yaml("a: |2\n    indented\n  base\n", R"({"a": "  indented\nbase\n"})");
  expect_yaml("- |\n  item\n- >\n  next\n", R"(["item\n", "next\n"])");
  expect_yaml("a: |\n  no newline at end", R"({"a": "no newline at end"})");
  expect_yaml("a: |\n\n  after blank\n", R"({"a": "\nafter blank\n"})");
  expect_yaml("a: | # comment\n  x\n", R"({"a": "x\n"})");
  expect_yaml("--- |\n  top level\n", R"("top level\n")");
}

TEST_CASE("data/yaml: flow collections") {
  expect_yaml("{a: 1, b: [2, {c: 3}], \"d\": x}", R"({"a": 1, "b": [2, {"c": 3}], "d": "x"})");
  expect_yaml("[a, b: c, {d}, ]", R"(["a", {"b": "c"}, {"d": null}])");
  expect_yaml("{\"json\":\"like\",'k':1}", R"({"json": "like", "k": 1})");
  expect_yaml("[\n  multi,\n  line # comment\n]\n", R"(["multi", "line"])");
  expect_yaml("{a: , b: }", R"({"a": null, "b": null})");
  expect_yaml("[http://x.invalid/a, a:b]", R"(["http://x.invalid/a", "a:b"])");
}

TEST_CASE("data/yaml: core schema resolution (1.2) and the 1.1 schema") {
  expect_yaml("[null, Null, NULL, ~, '', true, True, FALSE, 1, -2, +3, 0o17, 0x1F, 1.5, -.5, 1e3, .inf, -.Inf, 007, yes, on, 0b101, 1_000]",
              R"([null, null, null, null, "", true, true, false, 1, -2, 3, 15, 31, 1.5, -0.5, 1000.0,
                  {"$float": "Infinity"}, {"$float": "-Infinity"}, 7, "yes", "on", "0b101", "1_000"])");
  expect_yaml("[.nan]", R"([{"$float": "NaN"}])");
  expect_yaml("%YAML 1.1\n---\n[yes, no, on, off, 0b101, 017, 1_000, 190:20:30, 1.5e+3]\n",
              R"([true, false, true, false, 5, 15, 1000, 685230, 1500.0])");
  expect_yaml("[yes, 017]", R"([true, 15])", R"({"yaml_version": "1.1"})");
  expect_yaml("%YAML 1.1\n---\n[yes]\n", R"(["yes"])", R"({"yaml_version": "1.2"})");
  // Exact numbers beyond 64 bits and doubles.
  expect_yaml("[123456789012345678901234567890, 0.1000000000000000000001, 18446744073709551615]",
              R"([{"$bigint": "123456789012345678901234567890"}, {"$decimal": "0.1000000000000000000001"}, 18446744073709551615])");
}

TEST_CASE("data/yaml: timestamps") {
  expect_yaml("[2001-12-14, 2001-12-14t21:59:43.10-05:00, 2001-12-14 21:59:43.10 -5, 2002-12-14T21:59:43Z, 2001-12-15 2:59:43.10]",
              R"([{"$date": "2001-12-14"}, {"$date": "2001-12-14T21:59:43.10-05:00"}, {"$date": "2001-12-14T21:59:43.10-05:00"},
                  {"$date": "2002-12-14T21:59:43Z"}, {"$date": "2001-12-15T02:59:43.10"}])");
  expect_yaml("[2001-12-14, '2001-12-14', 2001-02-30]", R"(["2001-12-14", "2001-12-14", "2001-02-30"])",
              R"({"timestamps": false})");
  expect_yaml("[2001-02-30]", R"(["2001-02-30"])");  // not a valid date: a string
}

TEST_CASE("data/yaml: tags") {
  expect_yaml("[!!str 1, !!int '7', !!float 3, !!bool 'yes', !!null '', !!binary aGVsbG8=, ! 12, !local x]",
              R"(["1", 7, 3.0, true, null, {"$binary": "aGVsbG8="}, "12", "x"])");
  expect_yaml("b: !!binary |\n  aGVs\n  bG8=\n", R"({"b": {"$binary": "aGVsbG8="}})");
  expect_yaml("t: !!timestamp 2001-12-14\n", R"({"t": {"$date": "2001-12-14"}})");
  expect_yaml("o: !!omap\n  - b: 1\n  - a: 2\n", R"({"o": {"b": 1, "a": 2}})");
  expect_yaml("p: !!pairs\n  - a: 1\n  - a: 2\n", R"({"p": [{"a": 1}, {"a": 2}]})");
  expect_yaml("s: !!set {x, y}\n", R"({"s": {"x": null, "y": null}})");
  expect_yaml("%TAG !e! tag:example.invalid,2000:\n---\n!e!thing x\n", R"("x")");
  expect_yaml("!<tag:yaml.org,2002:str> 5\n", R"("5")");
  CHECK_STATUS(td::read("yaml", "!!int abc\n"), Status::Decode);
  CHECK_STATUS(td::read("yaml", "!!map [1]\n"), Status::Decode);
}

TEST_CASE("data/yaml: anchors, aliases and merge keys") {
  expect_yaml("a: &x {k: v}\nb: *x\n", R"({"a": {"k": "v"}, "b": {"k": "v"}})");
  expect_yaml("- &s scalar\n- *s\n- &l [1, 2]\n- *l\n", R"(["scalar", "scalar", [1, 2], [1, 2]])");
  expect_yaml("base: &b {x: 1, y: 2}\nderived:\n  <<: *b\n  y: 3\n  z: 4\n",
              R"({"base": {"x": 1, "y": 2}, "derived": {"x": 1, "y": 3, "z": 4}})");
  expect_yaml("a: &a {x: 1}\nb: &b {x: 2, y: 2}\nc:\n  <<: [*a, *b]\n", R"({"a": {"x": 1}, "b": {"x": 2, "y": 2}, "c": {"x": 1, "y": 2}})");
  expect_yaml("a: &a {x: 1}\nc:\n  <<: *a\n", R"({"a": {"x": 1}, "c": {"<<": {"x": 1}}})", R"({"merge_keys": false})");
  expect_yaml("&anchor key: value\n", R"({"key": "value"})");
  expect_yaml("a: !!str &s 5\nb: *s\n", R"({"a": "5", "b": "5"})");
}

TEST_CASE("data/yaml: hostile aliases are rejected") {
  // Billion laughs: nine levels of nine aliases.
  std::string bomb = "a: &a [\"lol\",\"lol\",\"lol\",\"lol\",\"lol\",\"lol\",\"lol\",\"lol\",\"lol\"]\n";
  for (char level = 'b'; level <= 'i'; ++level) {
    const char previous = static_cast<char>(level - 1);
    bomb += std::format("{0}: &{0} [*{1},*{1},*{1},*{1},*{1},*{1},*{1},*{1},*{1}]\n", level, previous);
  }
  CHECK_STATUS(td::read("yaml", bomb), Status::Limit);
  // A node that contains itself.
  const auto recursive = td::read("yaml", "a: &a [*a]\n");
  REQUIRE_STATUS(recursive, Status::Decode);
  CHECK(recursive.error().message.find("recursive") != std::string::npos);
  CHECK_STATUS(td::read("yaml", "a: *missing\n"), Status::Decode);
}

TEST_CASE("data/yaml: multi-document streams") {
  const std::string_view stream = "--- 1\n--- 2\n...\n---\n- 3\n";
  expect_yaml(stream, "[1, 2, [3]]");
  expect_yaml(stream, "1", R"({"documents": "first"})");
  expect_yaml(stream, "[3]", R"({"documents": "index", "document_index": 2})");
  expect_yaml("--- x\n", R"(["x"])", R"({"documents": "array"})");
  CHECK_STATUS(td::read("yaml", stream, R"({"documents": "index", "document_index": 5})"), Status::InvalidArgument);
  expect_yaml("", "null");
  expect_yaml("# only a comment\n", "null");
  expect_yaml("%YAML 1.2\n---\na: 1\n...\n", R"({"a": 1})");
}

TEST_CASE("data/yaml: errors name the line and column") {
  struct Bad {
    std::string_view yaml;
    std::size_t line;
    std::size_t column;
  };
  const Bad cases[] = {
      {"a: 1\n b: 2\n", 2, 3},          // a mapping inside a plain scalar (PyYAML reports the same)
      {"a: [1, 2\n", 1, 4},             // unterminated flow sequence
      {"a: \"open\n", 1, 4},            // unterminated string
      {"a:\n\t- x\n", 2, 1},            // tab indentation
      {"- a\nb: c\n", 2, 1},            // mapping after a top-level sequence
      {"a: 1\na: 2\n", 2, 1},           // duplicate key with duplicate_keys = error
      {"key: \x01\n", 1, 6},            // control character
  };
  for (const Bad& bad : cases) {
    INFO("YAML:\n", bad.yaml);
    const auto result = td::read("yaml", bad.yaml, R"({"duplicate_keys": "error"})");
    REQUIRE_STATUS(result, Status::Decode);
    CHECK_MESSAGE(td::mentions_position(result.error(), bad.line, bad.column), result.error().message);
  }
}

TEST_CASE("data/yaml: deep nesting is limited") {
  std::string deep(5000, '[');
  CHECK_STATUS(td::read("yaml", deep), Status::Limit);
  std::string block;
  for (int i = 0; i < 300; ++i) block += std::string(static_cast<std::size_t>(i) * 2, ' ') + "- \n" ;
  CHECK(td::read("yaml", block).has_value() || td::read("yaml", block).error().code == Status::Limit);
  CHECK_STATUS(td::read("yaml", "[[[[1]]]]", R"({"max_depth": 3})"), Status::Limit);
}

TEST_CASE("data/yaml: the writer quotes exactly what would change meaning") {
  const Json value = td::json(R"({"plain": "hello world", "yes": "yes", "number": "123", "float": "1.5", "null": "null",
      "tilde": "~", "empty": "", "date": "2024-05-01", "colon": "a: b", "hash": "a #b", "lead": " x", "trail": "x ",
      "dash": "- x", "quote": "it's", "both": "\"it's\"", "ctrl": "bell\u0007", "multi": "a\nb\n", "multi_strip": "a\nb",
      "multi_keep": "a\n\n", "indent_first": "  a\nb", "bom": "\ufeffx", "octal": "012", "under": "1_0", "sexa": "1:20",
      "ok": "plain-text_with.punctuation!"})");
  const auto text = td::write("yaml", value);
  REQUIRE_OK(text);
  INFO(*text);
  CHECK(text->find("plain: hello world\n") != std::string::npos);
  CHECK(text->find("ok: plain-text_with.punctuation!\n") != std::string::npos);
  CHECK(text->find("'yes': 'yes'") != std::string::npos);
  CHECK(text->find("number: '123'") != std::string::npos);
  CHECK(text->find("multi: |\n  a\n  b\n") != std::string::npos);
  CHECK(text->find("multi_strip: |-\n") != std::string::npos);
  CHECK(text->find("ctrl: \"bell\\a\"") != std::string::npos);
  // Whatever the spelling, both schemas read the same strings back.
  for (const std::string_view version : {"1.2", "1.1"}) {
    const auto back = td::read("yaml", *text, std::format(R"({{"yaml_version": "{}"}})", version));
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::data_equal(*back, value), test::data_diff(*back, value));
  }
}

TEST_CASE("data/yaml: writer options") {
  const Json value = td::json(R"({"b": [1, {"c": 2}], "a": {}})");
  CHECK(td::write("yaml", value).value() == "b:\n  - 1\n  - c: 2\na: {}\n");
  CHECK(td::write("yaml", value, R"({"indent": 4, "sort_keys": true, "indent_sequences": false})").value() ==
        "a: {}\nb:\n- 1\n- c: 2\n");
  CHECK(td::write("yaml", value, R"({"yaml_style": "flow"})").value() == "{b: [1, {c: 2}], a: {}}\n");
  CHECK(td::write("yaml", value, R"({"explicit_start": true})").value().starts_with("---\n"));
  CHECK(td::write("yaml", td::json("[{\"a\": 1}, 2]"), R"({"split_documents": true})").value() == "---\na: 1\n---\n2\n");
  CHECK(td::write("yaml", td::json(R"({"t": "x\ny"})"), R"({"block_literals": false})").value() == "t: \"x\\ny\"\n");
  CHECK(td::write("yaml", td::json(R"({"f": 1e16, "g": 0.5, "n": {"$float": "NaN"}})")).value() == "f: 1.0e+16\ng: 0.5\n'n': .nan\n");
}
