// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>
#include <string>
#include <tuple>

#include "families/table/table_test_support.hpp"

using namespace cvt;
using namespace cvt::table_test;

namespace {

ir::Table python_database() {
  ir::Table table;
  ir::Sheet people = make_sheet("people", {{S("id"), S("name"), S("height"), S("born"), S("active"), S("photo"), S("note")},
                                           {N(1), S("Ada"), N(1.65), D("1990-04-01"), B(true), S("0001ff")},
                                           {N(2), S("Grüße 世界"), E(), E(), B(false)},
                                           {N(3), S("O'Brien \"quoted\""), N(-0.5), D("2024-02-29"), B(true), S(""), S("added later")}});
  ir::Sheet odd = make_sheet("odd name", {{S("col a"), S("col b"), S("col c"), S("gen")}});
  for (int i = 1; i <= 600; ++i) odd.rows.push_back({N(i), S(std::format("v{}", i)), N(i * 1.5)});
  ir::Sheet pairs = make_sheet("pairs", {{S("k"), S("n"), S("v")}});
  std::vector<std::tuple<int, std::string, int>> keys;
  for (int i = 0; i < 300; ++i) keys.emplace_back(i % 5, std::format("k{:03}", i), i);
  std::ranges::sort(keys);
  for (const auto& [n, k, i] : keys) {
    pairs.rows.push_back({S(k), N(n), i % 40 == 0 ? S("") : S(std::string(static_cast<std::size_t>(i % 40), 'x'))});
  }
  std::string lorem;
  for (int i = 0; i < 900; ++i) lorem += "lorem ipsum ";
  ir::Sheet longs = make_sheet("long", {{S("id"), S("body")}, {N(1), S(lorem)}, {N(2), S("short")}});
  table.sheets = {people, odd, pairs, longs};
  for (ir::Sheet& sheet : table.sheets) sheet.has_header = true;
  return table;
}

}  // namespace

TEST_CASE("table/sqlite: decodes a database written by Python's sqlite3") {
  const auto table = read_fixture("sqlite", "python_sample.sqlite");
  REQUIRE_OK(table);
  const ir::Table expected = python_database();
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  CHECK(table->sheets[0].has_header);
  const auto base64 = read_fixture("sqlite", "python_sample.sqlite", R"({"blob_format":"base64"})");
  REQUIRE_OK(base64);
  CHECK(base64->sheets[0].rows[1][5].text == "AAH/");
}

TEST_CASE("table/sqlite: UTF-16 text and 512-byte pages") {
  const auto table = read_fixture("sqlite", "python_utf16_small_pages.sqlite");
  REQUIRE_OK(table);
  ir::Sheet words = make_sheet("words", {{S("w"), S("n")}});
  for (int i = 0; i < 400; ++i) words.rows.push_back({S(std::format("wörd {} ✓", i)), N(i * i)});
  CHECK_MESSAGE(test::tables_equal(*table, single(words)), test::table_diff(*table, single(words)));
}

TEST_CASE("table/sqlite: write and read back, with interior and overflow pages") {
  ir::Table table = sample_workbook(true);
  for (ir::Sheet& sheet : table.sheets) sheet.has_header = true;
  ir::Sheet big = make_sheet("Big", {{S("n"), S("text"), S("ratio")}});
  for (int i = 0; i < 5000; ++i) big.rows.push_back({N(i), S(std::format("row {} of many", i)), N(i / 7.0)});
  big.rows.push_back({N(-1), S(std::string(20000, 'z')), N(1e300)});
  big.rows.push_back({N(9007199254740993.0), S(""), E()});
  big.has_header = true;
  table.sheets.push_back(big);
  table.sheets.push_back(make_sheet("sqlite_master", {{S("x")}, {N(1)}}));  // reserved name gets a prefix
  table.sheets.back().has_header = true;
  table.sheets[1].has_header = false;  // the "Formulas" sheet starts with numbers, not titles
  const auto bytes = test::write_from("sqlite", table);
  REQUIRE_OK(bytes);
  CHECK(test::str(ByteView(*bytes).first(16)) == std::string("SQLite format 3\0", 16));
  CHECK(bytes->size() % 4096 == 0);
  const auto back = test::read_as<ir::Table>("sqlite", *bytes);
  REQUIRE_OK(back);
  ir::Table expected = table;
  expected.sheets.back().name = "t_sqlite_master";
  expected.sheets[1].rows.insert(expected.sheets[1].rows.begin(), {S("column1"), S("column2"), S("column3")});
  // SQLite has no boolean type: in a column that mixes kinds TRUE is stored as 1.
  expected.sheets[1].rows[3][1] = N(1);
  // Empty titles get generated names.
  expected.sheets[2].rows[0] = {S("Title"), S("column2"), S("column3")};
  CHECK_MESSAGE(test::tables_equal(*back, expected), test::table_diff(*back, expected));
}

TEST_CASE("table/sqlite: column names and types") {
  ir::Sheet sheet = make_sheet("t", {{S("A"), S("a"), S(""), S("mixed")}, {N(1), N(1.5), S("x"), N(2)}, {N(2), N(3), B(true), S("y")}});
  sheet.has_header = true;
  const auto bytes = test::write_from("sqlite", single(sheet));
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Table>("sqlite", *bytes);
  REQUIRE_OK(back);
  CHECK(back->sheets[0].rows[0][0].text == "A");
  CHECK(back->sheets[0].rows[0][1].text == "a_2");  // names are case-insensitive in SQL
  CHECK(back->sheets[0].rows[0][2].text == "column3");
  const auto generated = test::write_from("sqlite", single(sheet), R"({"column_names":"generated"})");
  REQUIRE_OK(generated);
  const auto generated_back = test::read_as<ir::Table>("sqlite", *generated);
  REQUIRE_OK(generated_back);
  CHECK(generated_back->sheets[0].rows[0][0].text == "column1");
  CHECK(generated_back->sheets[0].rows[1][0].text == "A");
}

TEST_CASE("table/sqlite: damaged and hostile databases fail cleanly") {
  const Bytes valid = test::fixture("table/python_sample.sqlite");
  CHECK_STATUS(test::read_as<ir::Table>("sqlite", ByteView(valid).first(50)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Table>("sqlite", valid, 200) > 0);
  CHECK(test::fuzz_reader<ir::Table>("sqlite", test::fixture("table/python_utf16_small_pages.sqlite"), 100) > 0);
  // A b-tree page that points to itself.
  ir::Sheet many = make_sheet("t", {{S("n")}});
  for (int i = 0; i < 3000; ++i) many.rows.push_back({N(i)});
  many.has_header = true;
  const auto bytes = test::write_from("sqlite", single(many));
  REQUIRE_OK(bytes);
  Bytes looped = *bytes;
  // Find the interior page of table "t" (type 0x05) and aim its right-most pointer at itself.
  bool patched = false;
  for (std::size_t page = 1; page * 4096 < looped.size() && !patched; ++page) {
    const std::size_t at = page * 4096;
    if (std::to_integer<int>(looped[at]) == 5) {
      const auto number = static_cast<std::uint32_t>(page + 1);
      for (int i = 0; i < 4; ++i) looped[at + 8 + static_cast<std::size_t>(i)] = static_cast<std::byte>(number >> (24 - 8 * i));
      patched = true;
    }
  }
  REQUIRE(patched);
  CHECK_STATUS(test::read_as<ir::Table>("sqlite", looped), Status::Decode);
  // Payload sizes count against the decompression limit.
  Limits limits;
  limits.max_decompressed_bytes = 4096;
  CHECK_STATUS(test::read_as<ir::Table>("sqlite", valid, {}, nullptr, limits), Status::Limit);
}

TEST_CASE("table/sqlite: detection and conversion") {
  const test::Converted csv = test::convert(test::fixture("table/python_sample.sqlite"), "", "csv", R"({"sheet":"long"})");
  REQUIRE(csv.ok());
  CHECK(csv.text().ends_with("2,short\r\n"));
  const test::Converted xlsx = test::convert(test::fixture("table/python_sample.sqlite"), "", "xlsx");
  CHECK(xlsx.ok());
  const test::Converted same = test::convert(test::fixture("table/python_sample.sqlite"), "", "sqlite");
  CHECK(same.ok());
}
