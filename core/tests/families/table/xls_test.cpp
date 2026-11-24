// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>

#include "families/table/cfb.hpp"
#include "families/table/table_test_support.hpp"
#include "families/table/xls.hpp"

using namespace cvt;
using namespace cvt::table_test;

namespace {

std::string repeat(std::string_view piece, std::size_t times) {
  std::string out;
  for (std::size_t i = 0; i < times; ++i) out += piece;
  return out;
}

}  // namespace

TEST_CASE("table/xls: decodes a LibreOffice BIFF8 workbook cell by cell") {
  const auto table = read_fixture("xls", "lo_sample.xls");
  REQUIRE_OK(table);
  const ir::Table expected = sample_workbook(true);
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  CHECK(table->sheets[3].visibility == ir::SheetVisibility::Hidden);
  CHECK(sorted_merges(table->sheets[2].merged) == sorted_merges(expected.sheets[2].merged));
  // Formula text decompiled from the parsed tokens.
  CHECK(table->sheets[1].formula_at(3, 0) == "SUM(A1:A3)");
  CHECK(table->sheets[1].formula_at(0, 1) == "A1*2");
  CHECK(table->sheets[1].formula_at(2, 1) == "A3>2");
  CHECK(table->sheets[1].formula_at(3, 1) == "IF(A1>0,\"pos\",\"neg\")");
  CHECK(table->sheets[1].formula_at(0, 2) == "ROUND(PI(),2)");
}

TEST_CASE("table/xls: decodes a Gnumeric BIFF5 (Excel 95) workbook") {
  const auto table = read_fixture("xls", "gnumeric_biff5.xls");
  REQUIRE_OK(table);
  ir::Table expected = sample_workbook(true);
  // BIFF5 stores text in the Windows-1252 code page: characters outside it were lost by the writer.
  expected.sheets[0].rows[2][8] = S("Grüße, ??");
  expected.sheets[0].rows[5][8] = S("? smile");
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  CHECK(table->sheets[3].visibility == ir::SheetVisibility::Hidden);
}

TEST_CASE("table/xls: decodes an xlwt workbook with a shared string table spanning CONTINUE records") {
  const auto table = read_fixture("xls", "xlwt_biff8.xls");
  REQUIRE_OK(table);
  REQUIRE(table->sheets.size() == 3);
  ir::Sheet data = make_sheet("Data", {{S("text"), S("number"), S("date"), S("when"), S("flag")},
                                       {S("hello"), N(42), D("2024-05-01"), D("2024-05-01T12:30:00"), B(true)},
                                       {S("Grüße 世界 😀"), N(-0.125), D("1999-12-31"), E(), B(false)},
                                       {S("line one\nline two"), N(123456789.5)},
                                       {},
                                       {S("merged across")}});
  ir::Sheet strings = make_sheet("Strings", {});
  for (int i = 0; i < 600; ++i) {
    char label[32];
    std::snprintf(label, sizeof label, "string number %05d", i);
    strings.rows.push_back({S(label), S(repeat("ü" + std::to_string(i), i % 2 != 0 ? 3 : 1))});
  }
  strings.rows.push_back({S(std::string(9000, 'L'))});
  strings.rows.push_back({S(repeat("Ж", 5000))});
  ir::Sheet hidden = make_sheet("Hidden", {{S("secret")}});
  ir::Table expected;
  expected.sheets = {data, strings, hidden};
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  CHECK(table->sheets[2].visibility == ir::SheetVisibility::Hidden);
  REQUIRE(table->sheets[0].merged.size() == 1);
  CHECK(table->sheets[0].merged[0] == ir::CellRange{5, 0, 5, 2});
}

TEST_CASE("table/xls: write and read back") {
  ir::Table table = sample_workbook(true);
  table.sheets[0].has_header = true;
  table.sheets[0].column_widths = {15, 0, 7.5};
  std::vector<std::string> warnings;
  const auto bytes = test::write_from("xls", table, R"({"freeze_header":true})", &warnings);
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Table>("xls", *bytes);
  REQUIRE_OK(back);
  CHECK_MESSAGE(test::tables_equal(*back, table), test::table_diff(*back, table));
  CHECK(sorted_merges(back->sheets[2].merged) == sorted_merges(table.sheets[2].merged));
  CHECK(back->sheets[3].visibility == ir::SheetVisibility::Hidden);
  CHECK(back->sheets[0].frozen_rows == 1);
  REQUIRE(back->sheets[0].column_widths.size() >= 3);
  CHECK(back->sheets[0].column_widths[0] == 15);
  CHECK(back->sheets[0].number_format_at(1, 6) == "yyyy-mm-dd hh:mm:ss");
  // The workbook stream sits outside the mini stream, as Excel requires.
  const auto cfb = table::CfbReader::open(*bytes);
  REQUIRE_OK(cfb);
  const auto index = cfb->find("Workbook");
  REQUIRE(index.has_value());
  CHECK(cfb->entries()[*index].size >= 4096);
}

TEST_CASE("table/xls: a large shared string table is split over CONTINUE records") {
  ir::Table table;
  table.sheets.emplace_back();
  table.sheets[0].name = "Many";
  for (int i = 0; i < 3000; ++i) table.sheets[0].rows.push_back({S("unique text " + std::to_string(i)), S(repeat("Ω", static_cast<std::size_t>(i % 50)))});
  table.sheets[0].rows.push_back({S(std::string(20000, 'q')), S(repeat("中", 9000))});
  const auto back = round_trip("xls", table);
  REQUIRE_OK(back);
  CHECK_MESSAGE(test::tables_equal(*back, table), test::table_diff(*back, table));
}

TEST_CASE("table/xls: sheets larger than BIFF8 allows are cut with a warning") {
  ir::Table table;
  table.sheets.emplace_back();
  table.sheets[0].rows.push_back(std::vector<ir::Cell>(300, N(1)));
  std::vector<std::string> warnings;
  const auto bytes = test::write_from("xls", table, {}, &warnings);
  REQUIRE_OK(bytes);
  CHECK(warnings.size() == 1);
  const auto back = test::read_as<ir::Table>("xls", *bytes);
  REQUIRE_OK(back);
  CHECK(back->sheets[0].column_count() == 256);
}

TEST_CASE("table/xls: damaged and hostile files fail cleanly") {
  const Bytes valid = test::fixture("table/lo_sample.xls");
  CHECK_STATUS(test::read_as<ir::Table>("xls", ByteView(valid).first(600)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Table>("xls", valid, 200) > 0);
  CHECK(test::fuzz_reader<ir::Table>("xls", test::fixture("table/xlwt_biff8.xls"), 100) > 0);
  // A compound file without a workbook stream (a Word document, say).
  table::CfbWriter writer;
  REQUIRE_OK(writer.add_stream("WordDocument", test::bytes("not a workbook")));
  const auto doc = writer.finish();
  REQUIRE_OK(doc);
  CHECK_STATUS(test::read_as<ir::Table>("xls", *doc), Status::Decode);
  // An encrypted OOXML package.
  table::CfbWriter encrypted;
  REQUIRE_OK(encrypted.add_stream("EncryptionInfo", test::bytes("x")));
  REQUIRE_OK(encrypted.add_stream("EncryptedPackage", test::bytes("y")));
  const auto locked = encrypted.finish();
  REQUIRE_OK(locked);
  const auto result = test::read_as<ir::Table>("xls", *locked);
  CHECK_STATUS(result, Status::Decode);
  if (!result) CHECK(result.error().message.find("password") != std::string::npos);
}

TEST_CASE("table/xls: detection tells workbooks from other compound files") {
  const test::Converted csv = test::convert(test::fixture("table/lo_sample.xls"), "", "csv", R"({"sheet":"Hidden"})");
  REQUIRE(csv.ok());
  CHECK(csv.format_id == "csv");
  CHECK(csv.text() == "secret\r\n99\r\n");
  const test::Converted compressed = test::convert(test::fixture("table/xlwt_biff8.xls"), "", "xls");
  CHECK(compressed.ok());
}

TEST_CASE("table/xls: RK numbers and formula tokens") {
  CHECK(table::decode_rk(0x3FF00000u) == 1.0);           // IEEE high bits of 1.0
  CHECK(table::decode_rk((100u << 2) | 2u) == 100.0);     // integer
  CHECK(table::decode_rk((1234u << 2) | 3u) == 12.34);    // integer / 100
  CHECK(table::decode_rk(static_cast<std::uint32_t>(-5 * 4) | 2u) == -5.0);
  // =(A1+2)*$B$3 : ptgRef A1 (relative), ptgInt 2, ptgAdd, ptgParen, ptgRef $B$3, ptgMul
  const std::uint8_t tokens[] = {0x24, 0, 0, 0x00, 0xC0, 0x1E, 2, 0, 0x03, 0x15, 0x24, 2, 0, 0x01, 0x00, 0x05};
  CHECK(table::decompile_biff8_formula(as_bytes(std::span<const std::uint8_t>(tokens)), 0, 0) == std::optional<std::string>("(A1+2)*$B$3"));
  const std::uint8_t unknown[] = {0x23, 1, 0, 0, 0};  // a defined name: needs the workbook
  CHECK_FALSE(table::decompile_biff8_formula(as_bytes(std::span<const std::uint8_t>(unknown)), 0, 0).has_value());
}
