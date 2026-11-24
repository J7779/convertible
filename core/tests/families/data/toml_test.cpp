// TOML: an edge-case suite written for this project (not taken from toml-test).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string_view>

#include "families/data/data_test_support.hpp"

using namespace cvt;
namespace td = cvt::test::data;

namespace {

void expect_toml(std::string_view toml, std::string_view json) {
  INFO("TOML:\n", toml);
  const auto value = td::read("toml", toml);
  REQUIRE_OK(value);
  const Json expected = td::json(json);
  CHECK_MESSAGE(test::data_equal(*value, expected), test::data_diff(*value, expected), "\n got: ", util::dump_json(*value));
}

void expect_invalid(std::string_view toml, std::size_t line, std::size_t column) {
  INFO("TOML:\n", toml);
  const auto value = td::read("toml", toml);
  REQUIRE_STATUS(value, Status::Decode);
  CHECK_MESSAGE(td::mentions_position(value.error(), line, column), value.error().message);
}

}  // namespace

TEST_CASE("data/toml: strings") {
  expect_toml(R"(a = "tab\tq\"bs\\u\u00e9\U0001F600")", R"({"a": "tab\tq\"bs\\u\u00e9\ud83d\ude00"})");
  expect_toml(R"(a = 'C:\no\escapes')", R"({"a": "C:\\no\\escapes"})");
  expect_toml("a = \"\"\"\nline1\nline2\"\"\"", R"({"a": "line1\nline2"})");
  expect_toml("a = \"\"\"\nThe quick \\\n    brown \\\n\n    fox\"\"\"", R"({"a": "The quick brown fox"})");
  expect_toml("a = '''\nraw \\n\n'''", R"({"a": "raw \\n\n"})");
  expect_toml("a = \"\"\"quotes \"\" inside\"\"\"\"\"", R"({"a": "quotes \"\" inside\"\""})");
  expect_toml("a = \"\\e\\x41\"", R"({"a": "\u001bA"})");  // TOML 1.1 escapes
  expect_toml("a = \"crlf\"\r\nb = 1\r\n", R"({"a": "crlf", "b": 1})");
}

TEST_CASE("data/toml: numbers") {
  expect_toml("a = +99\nb = -17\nc = 1_000\nd = 0xDEAD_beef\ne = 0o755\nf = 0b1101\ng = 9223372036854775807\nh = -9223372036854775808",
              R"({"a": 99, "b": -17, "c": 1000, "d": 3735928559, "e": 493, "f": 13, "g": 9223372036854775807, "h": -9223372036854775808})");
  expect_toml("a = 1.0\nb = 3.1415\nc = -0.01\nd = 5e+22\ne = 1e06\nf = -2E-2\ng = 6.626e-34\nh = 224_617.445_991",
              R"({"a": 1.0, "b": 3.1415, "c": -0.01, "d": 5e22, "e": 1e6, "f": -0.02, "g": 6.626e-34, "h": 224617.445991})");
  expect_toml("a = inf\nb = +inf\nc = -inf\nd = nan\ne = -0.0",
              R"({"a": {"$float": "Infinity"}, "b": {"$float": "Infinity"}, "c": {"$float": "-Infinity"}, "d": {"$float": "NaN"}, "e": -0.0})");
  expect_toml("a = 99999999999999999999", R"({"a": {"$bigint": "99999999999999999999"}})");
  expect_toml("a = 3.14159265358979323846264338327950288", R"({"a": {"$decimal": "3.14159265358979323846264338327950288"}})");
  expect_invalid("a = 01", 1, 5);
  expect_invalid("a = 1__0", 1, 7);
  expect_invalid("a = _1", 1, 5);
  expect_invalid("a = 1_", 1, 7);
  expect_invalid("a = +0x1", 1, 5);
  expect_invalid("a = 1.", 1, 7);
  expect_invalid("a = .5", 1, 5);
}

TEST_CASE("data/toml: dates and times") {
  expect_toml("a = 1979-05-27T07:32:00Z\nb = 1979-05-27T00:32:00-07:00\nc = 1979-05-27T00:32:00.999999-07:00\n"
              "d = 1979-05-27 07:32:00Z\ne = 1979-05-27T07:32:00\nf = 1979-05-27\ng = 07:32:00\nh = 00:32:00.999999\n"
              "i = 1979-05-27t07:32:00z\nj = 07:32",
              R"({"a": {"$date": "1979-05-27T07:32:00Z"}, "b": {"$date": "1979-05-27T00:32:00-07:00"},
                  "c": {"$date": "1979-05-27T00:32:00.999999-07:00"}, "d": {"$date": "1979-05-27T07:32:00Z"},
                  "e": {"$date": "1979-05-27T07:32:00"}, "f": {"$date": "1979-05-27"}, "g": {"$date": "07:32:00"},
                  "h": {"$date": "00:32:00.999999"}, "i": {"$date": "1979-05-27T07:32:00Z"}, "j": {"$date": "07:32:00"}})");
  expect_invalid("a = 1979-02-30", 1, 5);
  expect_invalid("a = 1979-05-27T25:00:00", 1, 5);
}

TEST_CASE("data/toml: keys, tables and arrays of tables") {
  expect_toml("\"quoted key\" = 1\n'literal' = 2\n1234 = 3\nbare-key_1 = 4\n3.14 = 5\na.\"b.c\".d = 6",
              R"({"quoted key": 1, "literal": 2, "1234": 3, "bare-key_1": 4, "3": {"14": 5}, "a": {"b.c": {"d": 6}}})");
  expect_toml("[a.b.c]\nx = 1\n[a]\ny = 2", R"({"a": {"b": {"c": {"x": 1}}, "y": 2}})");
  expect_toml("[[fruits]]\nname = \"apple\"\n[fruits.physical]\ncolor = \"red\"\n[[fruits.varieties]]\nname = \"rd\"\n"
              "[[fruits.varieties]]\nname = \"gs\"\n[[fruits]]\nname = \"banana\"",
              R"({"fruits": [{"name": "apple", "physical": {"color": "red"}, "varieties": [{"name": "rd"}, {"name": "gs"}]},
                             {"name": "banana"}]})");
  expect_toml("point = { x = 1, y.z = 2 }\nempty = {}\narr = [ 1, [2, 'x'], { a = 1 }, ]",
              R"({"point": {"x": 1, "y": {"z": 2}}, "empty": {}, "arr": [1, [2, "x"], {"a": 1}]})");
  expect_toml("arr = [\n  1, # comment\n  2,\n]\n", R"({"arr": [1, 2]})");
  expect_toml("t = {\n  a = 1,\n  b = 2,\n}\n", R"({"t": {"a": 1, "b": 2}})");  // TOML 1.1 inline tables
}

TEST_CASE("data/toml: redefinitions are errors") {
  expect_invalid("a = 1\na = 2", 2, 1);
  expect_invalid("[t]\n[t]", 2, 1);
  expect_invalid("a.b = 1\na.b.c = 2", 2, 1);
  expect_invalid("a = {b = 1}\na.c = 2", 2, 1);
  expect_invalid("a = {b = 1}\n[a]", 2, 1);
  expect_invalid("[fruit]\napple.color = 'red'\n[fruit.apple]", 3, 1);
  expect_invalid("a = [1]\n[[a]]", 2, 1);
  expect_invalid("[a.b]\n[[a]]", 2, 1);
  expect_invalid("a = 1 b = 2", 1, 7);
  expect_invalid("a = \"unterminated", 1, 5);
  expect_invalid("= 1", 1, 1);
  expect_invalid("a = \"bad \\q escape\"", 1, 10);
  expect_invalid("a = \"\x01\"", 1, 6);
}

TEST_CASE("data/toml: the writer's layout is stable") {
  const Json value = td::json(R"({"title": "x", "owner": {"name": "y", "dob": {"$date": "1979-05-27T07:32:00-08:00"}},
      "database": {"ports": [8000, 8001], "data": [["a", "b"], [1, 2]], "temp": {"cpu": 79.5}},
      "servers": [{"ip": "10.0.0.1", "role": "frontend"}, {"ip": "10.0.0.2", "role": "backend"}],
      "drop": null, "text": "line1\nline2", "weird key": true, "empty": {}})");
  std::vector<std::string> warnings;
  const auto text = td::write("toml", value, {}, &warnings);
  REQUIRE_OK(text);
  CHECK(*text ==
        "title = \"x\"\n"
        "text = \"\"\"\nline1\nline2\"\"\"\n"
        "\"weird key\" = true\n"
        "\n[owner]\nname = \"y\"\ndob = 1979-05-27T07:32:00-08:00\n"
        "\n[database]\nports = [8000, 8001]\ndata = [[\"a\", \"b\"], [1, 2]]\n"
        "\n[database.temp]\ncpu = 79.5\n"
        "\n[[servers]]\nip = \"10.0.0.1\"\nrole = \"frontend\"\n"
        "\n[[servers]]\nip = \"10.0.0.2\"\nrole = \"backend\"\n"
        "\n[empty]\n");
  CHECK(warnings.size() == 1);  // the null
  CHECK_STATUS(td::write("toml", value, R"({"null_values": "error"})"), Status::Encode);
  CHECK(td::write("toml", value, R"({"null_values": "empty_string"})").value().find("drop = \"\"") != std::string::npos);
  CHECK(td::write("toml", td::json(R"({"b": 1, "a": 2})"), R"({"sort_keys": true})").value() == "a = 2\nb = 1\n");
  // Non-object roots are wrapped.
  CHECK(td::write("toml", td::json("[1, 2]")).value() == "items = [1, 2]\n");
}

TEST_CASE("data/toml: big integers are kept exactly as text with a warning") {
  std::vector<std::string> warnings;
  const auto text = td::write("toml", td::json(R"({"n": 123456789012345678901234567890, "u": 18446744073709551615})"), {}, &warnings);
  REQUIRE_OK(text);
  CHECK(*text == "n = \"123456789012345678901234567890\"\nu = \"18446744073709551615\"\n");
  CHECK(!warnings.empty());
}
