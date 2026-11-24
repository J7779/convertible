// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>

#include "families/table/table_test_support.hpp"
#include "util/zip.hpp"

using namespace cvt;
using namespace cvt::table_test;

TEST_CASE("table/ods: decodes a LibreOffice spreadsheet cell by cell") {
  const auto table = read_fixture("ods", "lo_sample.ods");
  REQUIRE_OK(table);
  const ir::Table expected = sample_workbook(true);
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  CHECK(table->sheets[3].visibility == ir::SheetVisibility::Hidden);
  CHECK(sorted_merges(table->sheets[2].merged) == sorted_merges(expected.sheets[2].merged));
  CHECK(table->sheets[1].formula_at(3, 0) == "SUM(A1:A3)");
  CHECK(table->sheets[1].formula_at(3, 1) == "IF(A1>0,\"pos\",\"neg\")");
  CHECK_FALSE(table->sheets[0].column_widths.empty());
}

TEST_CASE("table/fods: decodes a flat LibreOffice spreadsheet") {
  const auto table = read_fixture("fods", "lo_sample.fods");
  REQUIRE_OK(table);
  const ir::Table expected = sample_workbook(true);
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  CHECK(table->sheets[3].visibility == ir::SheetVisibility::Hidden);
}

TEST_CASE("table/ods: write and read back, flat and zipped") {
  ir::Table table = sample_workbook(true);
  table.sheets[1].formulas[{3, 0}] = "SUM(A1:A3)";
  table.sheets[1].formulas[{0, 2}] = "ROUND(PI(),2)";
  table.sheets[0].has_header = true;
  table.sheets[0].column_widths = {20, 8.43};
  table.sheets[0].rows[3][8] = S("  two  spaces\tand tab");
  for (const char* format : {"ods", "fods", "ots"}) {
    INFO(format);
    const auto back = round_trip(format, table);
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::tables_equal(*back, table), test::table_diff(*back, table));
    CHECK(sorted_merges(back->sheets[2].merged) == sorted_merges(table.sheets[2].merged));
    CHECK(back->sheets[3].visibility == ir::SheetVisibility::Hidden);
    CHECK(back->sheets[1].formula_at(3, 0) == "SUM(A1:A3)");
    CHECK(back->sheets[0].has_header);
    REQUIRE(back->sheets[0].column_widths.size() >= 2);
    CHECK_APPROX(back->sheets[0].column_widths[0], 20.0, 0.05);
  }
}

TEST_CASE("table/ods: package layout, options and compression") {
  const ir::Table table = sample_workbook(true);
  const auto bytes = test::write_from("ods", table, R"({"freeze_header":true})");
  REQUIRE_OK(bytes);
  // "mimetype" first and stored, so that the magic number at offset 30 identifies the file.
  CHECK(test::str(ByteView(*bytes).subspan(30, 54)) == "mimetypeapplication/vnd.oasis.opendocument.spreadsheet");
  auto zip = util::ZipReader::open(*bytes);
  REQUIRE_OK(zip);
  CHECK(zip->contains("META-INF/manifest.xml"));
  CHECK(zip->contains("settings.xml"));  // frozen panes
  CHECK_FALSE(zip->contains("meta.xml"));  // no metadata at all
  const auto plain = test::write_from("ods", table, R"({"header_style":false,"auto_column_widths":false})");
  REQUIRE_OK(plain);
  CHECK(*plain != *bytes);
  const auto stored = test::write_from("ods", table, R"({"compression_level":0})");
  const auto packed = test::write_from("ods", table, R"({"compression_level":9})");
  REQUIRE_OK(stored);
  REQUIRE_OK(packed);
  CHECK(packed->size() < stored->size());
  const test::Converted detected = test::convert(*bytes, "", "csv", R"({"sheet":"Hidden"})");
  REQUIRE(detected.ok());
  CHECK(detected.text() == "secret\r\n99\r\n");
}

TEST_CASE("table/ods: repeated rows and cells are expanded only where they hold content") {
  const std::string content = R"(<?xml version="1.0" encoding="UTF-8"?>
<office:document-content xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"><office:body><office:spreadsheet><table:table table:name="R">
<table:table-row table:number-rows-repeated="3"><table:table-cell office:value-type="float" office:value="5" table:number-columns-repeated="2"><text:p>5</text:p></table:table-cell><table:table-cell table:number-columns-repeated="16380"/></table:table-row>
<table:table-row table:number-rows-repeated="1048573"><table:table-cell table:number-columns-repeated="16384"/></table:table-row>
</table:table></office:spreadsheet></office:body></office:document-content>)";
  util::ZipWriter zip;
  util::ZipEntryOptions stored;
  stored.level = 0;
  REQUIRE_OK(zip.add("mimetype", "application/vnd.oasis.opendocument.spreadsheet", stored));
  REQUIRE_OK(zip.add("content.xml", content));
  const auto bytes = zip.finish();
  REQUIRE_OK(bytes);
  const auto table = test::read_as<ir::Table>("ods", *bytes);
  REQUIRE_OK(table);
  const ir::Table expected = single(make_sheet("R", {{N(5), N(5)}, {N(5), N(5)}, {N(5), N(5)}}));
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  // The same with content in every one of a million repeated rows is a bomb.
  std::string bomb = content;
  bomb.replace(bomb.find("number-rows-repeated=\"3\""), 24, "number-rows-repeated=\"9000000\"");
  util::ZipWriter bomb_zip;
  REQUIRE_OK(bomb_zip.add("mimetype", "application/vnd.oasis.opendocument.spreadsheet", stored));
  REQUIRE_OK(bomb_zip.add("content.xml", bomb));
  const auto bomb_bytes = bomb_zip.finish();
  REQUIRE_OK(bomb_bytes);
  Limits limits;
  limits.max_decompressed_bytes = 64 * 1024 * 1024;
  CHECK_STATUS(test::read_as<ir::Table>("ods", *bomb_bytes, {}, nullptr, limits), Status::Limit);
}

TEST_CASE("table/ods: damaged files fail cleanly") {
  const Bytes valid = test::fixture("table/lo_sample.ods");
  CHECK_STATUS(test::read_as<ir::Table>("ods", ByteView(valid).first(500)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Table>("ods", valid, 150) > 0);
  const Bytes flat = test::fixture("table/lo_sample.fods");
  CHECK(test::fuzz_reader<ir::Table>("fods", flat, 150) > 0);
  CHECK_STATUS(test::read_as<ir::Table>("fods", test::bytes("<office:document><unclosed>")), Status::Decode);
}
