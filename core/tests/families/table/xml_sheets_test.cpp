// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>

#include "families/table/table_test_support.hpp"
#include "util/deflate.hpp"

using namespace cvt;
using namespace cvt::table_test;

TEST_CASE("table/spreadsheetml: decodes an Excel 2003 XML workbook written by LibreOffice") {
  const auto table = read_fixture("spreadsheetml", "lo_sample_2003.xml");
  REQUIRE_OK(table);
  const ir::Table expected = sample_workbook(true);
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  CHECK(sorted_merges(table->sheets[2].merged) == sorted_merges(expected.sheets[2].merged));
  CHECK(table->sheets[1].formula_at(3, 0) == "SUM(A1:A3)");
  CHECK(table->sheets[1].formula_at(0, 1) == "A1*2");
}

TEST_CASE("table/spreadsheetml: write and read back") {
  ir::Table table = sample_workbook(true);
  table.sheets[1].formulas[{3, 0}] = "SUM(A1:A3)";
  table.sheets[1].formulas[{0, 1}] = "$A$1*2";
  table.sheets[0].has_header = true;
  table.sheets[0].column_widths = {14};
  const auto bytes = test::write_from("spreadsheetml", table, R"({"freeze_header":true})");
  REQUIRE_OK(bytes);
  CHECK(test::str(*bytes).starts_with("<?xml"));
  const auto back = test::read_as<ir::Table>("spreadsheetml", *bytes);
  REQUIRE_OK(back);
  CHECK_MESSAGE(test::tables_equal(*back, table), test::table_diff(*back, table));
  CHECK(sorted_merges(back->sheets[2].merged) == sorted_merges(table.sheets[2].merged));
  CHECK(back->sheets[3].visibility == ir::SheetVisibility::Hidden);
  CHECK(back->sheets[0].frozen_rows == 1);
  CHECK(back->sheets[1].formula_at(3, 0) == "SUM(A1:A3)");
  CHECK(back->sheets[1].formula_at(0, 1) == "$A$1*2");
  REQUIRE_FALSE(back->sheets[0].column_widths.empty());
  CHECK_APPROX(back->sheets[0].column_widths[0], 14.0, 0.05);
  const test::Converted detected = test::convert(*bytes, "", "csv", R"({"sheet":"Hidden"})");
  REQUIRE(detected.ok());
  CHECK(detected.text() == "secret\r\n99\r\n");
  const auto plain = test::write_from("spreadsheetml", table, R"({"header_style":false})");
  REQUIRE_OK(plain);
  CHECK(*plain != *bytes);
}

TEST_CASE("table/spreadsheetml: damaged files fail cleanly") {
  const Bytes valid = test::fixture("table/lo_sample_2003.xml");
  CHECK(test::fuzz_reader<ir::Table>("spreadsheetml", valid, 150) > 0);
  CHECK_STATUS(test::read_as<ir::Table>("spreadsheetml", test::bytes("<Workbook></Workbook>")), Status::Decode);
}

TEST_CASE("table/gnumeric: decodes a Gnumeric workbook") {
  const auto table = read_fixture("gnumeric", "gnumeric_sample.gnumeric");
  REQUIRE_OK(table);
  const ir::Table expected = sample_workbook(false);  // Gnumeric keeps no formula results
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  CHECK(table->sheets[3].visibility == ir::SheetVisibility::Hidden);
  CHECK(sorted_merges(table->sheets[2].merged) == sorted_merges(expected.sheets[2].merged));
  CHECK(table->sheets[1].formula_at(3, 0) == "sum(A1:A3)");
  CHECK(table->sheets[0].number_format_at(1, 5) == "yyyy-mm-dd");
  // Detected from the gzip content.
  const test::Converted csv = test::convert(test::fixture("table/gnumeric_sample.gnumeric"), "", "csv", R"({"sheet":"Merged"})");
  REQUIRE(csv.ok());
  CHECK(csv.text() == "Title,,\r\na,b,c\r\n1,2,3\r\n,tall,\r\n");
}

TEST_CASE("table/gnumeric: damaged files fail cleanly") {
  const Bytes valid = test::fixture("table/gnumeric_sample.gnumeric");
  CHECK(test::fuzz_reader<ir::Table>("gnumeric", valid, 150) > 0);
  // A gzip bomb.
  const auto bomb = util::gzip_compress(as_bytes(std::string(4 * 1024 * 1024, ' ')), 9);
  REQUIRE_OK(bomb);
  Limits limits;
  limits.max_decompressed_bytes = 64 * 1024;
  CHECK_STATUS(test::read_as<ir::Table>("gnumeric", *bomb, {}, nullptr, limits), Status::Limit);
}
