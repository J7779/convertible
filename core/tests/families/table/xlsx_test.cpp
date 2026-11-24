// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <string>

#include "families/table/spreadsheet.hpp"
#include "families/table/table_test_support.hpp"
#include "util/zip.hpp"

using namespace cvt;
using namespace cvt::table_test;

TEST_CASE("table/xlsx: decodes a LibreOffice workbook cell by cell") {
  std::vector<std::string> warnings;
  const auto table = read_fixture("xlsx", "lo_sample.xlsx", {}, &warnings);
  REQUIRE_OK(table);
  const ir::Table expected = sample_workbook(true);
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  REQUIRE(table->sheets.size() == 4);
  CHECK(table->sheets[0].has_header);
  CHECK(table->sheets[3].visibility == ir::SheetVisibility::Hidden);
  CHECK(sorted_merges(table->sheets[2].merged) == sorted_merges(expected.sheets[2].merged));
  // Formulas keep their text next to the cached values.
  const ir::Sheet& formulas = table->sheets[1];
  CHECK(formulas.formula_at(3, 0) == "SUM(A1:A3)");
  CHECK(formulas.formula_at(0, 1) == "A1*2");
  CHECK(formulas.formula_at(1, 1) == "CONCATENATE(\"a\",\"b\")");
  CHECK(formulas.formula_at(3, 1) == "IF(A1>0,\"pos\",\"neg\")");
  // Date formats are kept.
  CHECK(table->sheets[0].number_format_at(1, 5) == "yyyy\\-mm\\-dd");
  CHECK(warnings.empty());
}

TEST_CASE("table/xlsx: decodes an openpyxl workbook (formulas without cached values)") {
  const auto table = read_fixture("xlsx", "openpyxl_sample.xlsx");
  REQUIRE_OK(table);
  const ir::Table expected = sample_workbook(false);
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  CHECK(table->sheets[1].formula_at(3, 0) == "SUM(A1:A3)");
  CHECK(table->sheets[1].formula_at(0, 2) == "ROUND(PI(),2)");
  CHECK(table->sheets[0].number_format_at(1, 7) == "hh:mm:ss");
}

TEST_CASE("table/xlsx: the 1904 date system") {
  const auto table = read_fixture("xlsx", "openpyxl_dates_1904.xlsx");
  REQUIRE_OK(table);
  const ir::Table expected = single(make_sheet("Mac", {{S("when"), S("n")}, {D("2024-05-01"), N(1.5)}, {D("1904-01-02T06:00:00")}}));
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
}

TEST_CASE("table/xlsx: a 10,000-row sheet streams through") {
  const auto table = read_fixture("xlsx", "openpyxl_big_10k.xlsx");
  REQUIRE_OK(table);
  REQUIRE(table->sheets.size() == 1);
  const ir::Sheet& sheet = table->sheets[0];
  REQUIRE(sheet.rows.size() == 10001);
  CHECK(sheet.rows[0][0].text == "value");
  std::size_t sevens = 0;
  for (std::size_t r = 1; r < sheet.rows.size(); ++r) sevens += sheet.at(r, 0).type == ir::CellType::Number && sheet.at(r, 0).number == 7 ? 1 : 0;
  CHECK(sevens == 10000);
}

TEST_CASE("table/xlsx: write and read back is exact, with formulas, merges, widths and panes") {
  ir::Table table = sample_workbook(true);
  table.sheets[1].formulas[{3, 0}] = "SUM(A1:A3)";
  table.sheets[0].has_header = true;
  table.sheets[0].column_widths = {12.5, 0, 9};
  table.sheets[0].frozen_rows = 1;
  table.sheets[0].number_formats[{1, 2}] = "0.00";
  const auto back = round_trip("xlsx", table);
  REQUIRE_OK(back);
  CHECK_MESSAGE(test::tables_equal(*back, table), test::table_diff(*back, table));
  CHECK(back->sheets[1].formula_at(3, 0) == "SUM(A1:A3)");
  CHECK(sorted_merges(back->sheets[2].merged) == sorted_merges(table.sheets[2].merged));
  CHECK(back->sheets[3].visibility == ir::SheetVisibility::Hidden);
  CHECK(back->sheets[0].frozen_rows == 1);
  REQUIRE(back->sheets[0].column_widths.size() >= 3);
  CHECK(back->sheets[0].column_widths[0] == 12.5);
  CHECK(back->sheets[0].number_format_at(1, 2) == "0.00");
  CHECK(back->sheets[0].number_format_at(1, 5) == "yyyy-mm-dd");
}

TEST_CASE("table/xlsx: output is anonymous, deterministic and has the expected parts") {
  const ir::Table table = sample_workbook(true);
  const auto first = test::write_from("xlsx", table);
  const auto second = test::write_from("xlsx", table);
  REQUIRE_OK(first);
  REQUIRE_OK(second);
  CHECK(*first == *second);
  auto zip = util::ZipReader::open(*first);
  REQUIRE_OK(zip);
  CHECK(zip->contains("[Content_Types].xml"));
  CHECK(zip->contains("xl/workbook.xml"));
  CHECK(zip->contains("xl/styles.xml"));
  CHECK(zip->contains("xl/sharedStrings.xml"));
  CHECK(zip->contains("xl/worksheets/sheet4.xml"));
  for (const auto& entry : zip->entries()) {
    CHECK_FALSE(entry.name.starts_with("docProps"));  // no author, no dates
    CHECK(entry.mtime == util::dos_to_unix_time(0x0021, 0));
  }
}

TEST_CASE("table/xlsx: options") {
  ir::Table table = single(types_sheet());
  table.sheets[0].has_header = true;
  SUBCASE("freeze_header") {
    const auto bytes = test::write_from("xlsx", table, R"({"freeze_header":true})");
    REQUIRE_OK(bytes);
    const auto back = test::read_as<ir::Table>("xlsx", *bytes);
    REQUIRE_OK(back);
    CHECK(back->sheets[0].frozen_rows == 1);
  }
  SUBCASE("header_style") {
    const auto styled = test::write_from("xlsx", table);
    const auto plain = test::write_from("xlsx", table, R"({"header_style":false})");
    REQUIRE_OK(styled);
    REQUIRE_OK(plain);
    auto zip = util::ZipReader::open(*plain);
    REQUIRE_OK(zip);
    const auto sheet = zip->read("xl/worksheets/sheet1.xml");
    REQUIRE_OK(sheet);
    CHECK(test::str(*sheet).find("s=\"1\"") == std::string::npos);
    CHECK(*styled != *plain);
  }
  SUBCASE("auto_column_widths") {
    const auto bytes = test::write_from("xlsx", table, R"({"auto_column_widths":false})");
    REQUIRE_OK(bytes);
    const auto back = test::read_as<ir::Table>("xlsx", *bytes);
    REQUIRE_OK(back);
    CHECK(back->sheets[0].column_widths.empty());
  }
  SUBCASE("date1904") {
    const auto back = round_trip("xlsx", table, R"({"date1904":true})");
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::tables_equal(*back, table), test::table_diff(*back, table));
  }
  SUBCASE("compression_level: higher effort is smaller") {
    ir::Table big;
    big.sheets.emplace_back();
    for (int r = 0; r < 400; ++r) big.sheets[0].rows.push_back({N(r), S("repeated text value"), N(r * 0.5)});
    const auto fast = test::write_from("xlsx", big, R"({"compression_level":0})");
    const auto small = test::write_from("xlsx", big, R"({"compression_level":9})");
    REQUIRE_OK(fast);
    REQUIRE_OK(small);
    CHECK(small->size() < fast->size());
  }
}

TEST_CASE("table/xlsx: xlsm and xltx flavours carry their content types") {
  const ir::Table table = single(types_sheet());
  for (const char* format : {"xlsm", "xltx"}) {
    INFO(format);
    const auto bytes = test::write_from(format, table);
    REQUIRE_OK(bytes);
    auto zip = util::ZipReader::open(*bytes);
    REQUIRE_OK(zip);
    const auto types = zip->read("[Content_Types].xml");
    REQUIRE_OK(types);
    const std::string text = test::str(*types);
    CHECK(text.find(std::string(format) == "xlsm" ? "macroEnabled.main" : "template.main") != std::string::npos);
    const test::Converted detected = test::convert(*bytes, "", "csv");
    CHECK(detected.ok());
    const auto back = test::read_as<ir::Table>(format, *bytes);
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::tables_equal(*back, table), test::table_diff(*back, table));
  }
}

TEST_CASE("table/xlsx: text that XML cannot hold survives") {
  const ir::Table table = single(make_sheet("Odd", {{S("tab\there"), S("bell\x07"), S("_x0041_ literal"), S("  spaced  ")}, {S("cr\r\nlf")}}));
  const auto back = round_trip("xlsx", table);
  REQUIRE_OK(back);
  CHECK_MESSAGE(test::tables_equal(*back, table), test::table_diff(*back, table));
}

TEST_CASE("table/xlsx: limits of the format are enforced with warnings") {
  ir::Table table;
  table.sheets.emplace_back();
  table.sheets[0].name = "a/very:long*sheet?name[that]goes on and on";
  table.sheets[0].rows = {{S(std::string(40000, 'x')), D("1850-01-01")}};
  std::vector<std::string> warnings;
  const auto bytes = test::write_from("xlsx", table, {}, &warnings);
  REQUIRE_OK(bytes);
  CHECK(warnings.size() == 2);
  const auto back = test::read_as<ir::Table>("xlsx", *bytes);
  REQUIRE_OK(back);
  CHECK(back->sheets[0].name.size() <= 31);
  CHECK(back->sheets[0].name.find('/') == std::string::npos);
  CHECK(back->sheets[0].rows[0][0].text.size() == 32767);
  CHECK(back->sheets[0].rows[0][1].text == "1850-01-01");  // before 1900: kept as text
}

TEST_CASE("table/xlsx: damaged and hostile files fail cleanly") {
  const Bytes valid = test::fixture("table/lo_sample.xlsx");
  CHECK_STATUS(test::read_as<ir::Table>("xlsx", ByteView(valid).first(100)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Table>("xlsx", valid, 200) > 0);
  // A sheet that claims a gigantic dimension and puts one cell at XFD1048576.
  util::ZipWriter zip;
  REQUIRE_OK(zip.add("xl/workbook.xml", R"(<workbook xmlns:r="r"><sheets><sheet name="S" r:id="rId1"/></sheets></workbook>)"));
  REQUIRE_OK(zip.add("xl/_rels/workbook.xml.rels",
                     R"(<Relationships><Relationship Id="rId1" Type="x/worksheet" Target="worksheets/sheet1.xml"/></Relationships>)"));
  std::string sheet = R"(<worksheet><dimension ref="A1:XFD1048576"/><sheetData>)";
  for (int r = 1; r <= 64; ++r) sheet += "<row r=\"" + std::to_string(r * 16000) + "\"><c r=\"XFD" + std::to_string(r * 16000) + "\"><v>1</v></c></row>";
  sheet += "</sheetData></worksheet>";
  REQUIRE_OK(zip.add("xl/worksheets/sheet1.xml", sheet));
  const auto bomb = zip.finish();
  REQUIRE_OK(bomb);
  Limits limits;
  limits.max_decompressed_bytes = 64 * 1024 * 1024;  // budget of about one million cells
  CHECK_STATUS(test::read_as<ir::Table>("xlsx", *bomb, {}, nullptr, limits), Status::Limit);
  // A zip bomb: a highly compressible sheet larger than the decompression limit.
  util::ZipWriter bomb_zip;
  REQUIRE_OK(bomb_zip.add("xl/workbook.xml", R"(<workbook><sheets><sheet name="S" r:id="rId1"/></sheets></workbook>)"));
  REQUIRE_OK(bomb_zip.add("xl/_rels/workbook.xml.rels",
                          R"(<Relationships><Relationship Id="rId1" Type="x/worksheet" Target="worksheets/sheet1.xml"/></Relationships>)"));
  REQUIRE_OK(bomb_zip.add("xl/worksheets/sheet1.xml", std::string(8 * 1024 * 1024, ' ')));
  const auto zipped = bomb_zip.finish();
  REQUIRE_OK(zipped);
  Limits tight;
  tight.max_decompressed_bytes = 1024 * 1024;
  CHECK_STATUS(test::read_as<ir::Table>("xlsx", *zipped, {}, nullptr, tight), Status::Limit);
}

TEST_CASE("table/xlsx: detection and the compress route") {
  const Bytes bytes = test::fixture("table/lo_sample.xlsx");
  const test::Converted compressed = test::convert(bytes, "", "xlsx", "", "book.xlsx");
  REQUIRE(compressed.ok());
  CHECK(compressed.format_id == "xlsx");
  const auto back = test::read_as<ir::Table>("xlsx", compressed.data);
  REQUIRE_OK(back);
  CHECK_MESSAGE(test::tables_equal(*back, sample_workbook(true)), test::table_diff(*back, sample_workbook(true)));
  // Detected from content even without a file name.
  const test::Converted csv = test::convert(bytes, "", "csv", R"({"sheet":"Merged"})");
  REQUIRE(csv.ok());
  CHECK(csv.text() == "Title,,\r\na,b,c\r\n1,2,3\r\n,tall,\r\n");
}

TEST_CASE("table/formulas: shared formula expansion and notation conversions") {
  CHECK(table::shift_formula("A1+$B$2+B$3+$C4", 2, 1) == "B3+$B$2+C$3+$C6");
  CHECK(table::shift_formula("SUM(A1:A3)*Sheet2!B1", 1, 0) == "SUM(A2:A4)*Sheet2!B2");
  CHECK(table::shift_formula("\"A1\"&A1", 1, 1) == "\"A1\"&B2");
  CHECK(table::shift_formula("LOG10(A1)", 0, 1) == "LOG10(B1)");
  CHECK(table::shift_formula("A1", -1, 0) == "#REF!");
  CHECK(table::excel_to_odf_formula("SUM(A1:B2,'My Sheet'!C3)") == "of:=SUM([.A1:.B2];[$'My Sheet'.C3])");
  CHECK(table::odf_to_excel_formula("of:=SUM([.A1:.B2];[$'My Sheet'.C3])") == "SUM(A1:B2,'My Sheet'!C3)");
  CHECK(table::odf_to_excel_formula("of:=IF([.A1]>0;\"a;b\";[$Sheet2.B$2])") == "IF(A1>0,\"a;b\",Sheet2!B$2)");
  CHECK(table::r1c1_to_a1_formula("R[-1]C+RC[2]+R1C1+ROUND(R2C[-1],0)", 4, 3) == "D4+F5+$A$1+ROUND(C$2,0)");
  CHECK(table::a1_to_r1c1_formula("D4+F5+$A$1", 4, 3) == "R[-1]C+RC[2]+R1C1");
  CHECK(table::ooxml_unescape("a_x0009_b_x005F_x0041_") == "a\tb_x0041_");
}

TEST_CASE("table/number formats: dates are recognised from their codes") {
  using table::FormatKind;
  CHECK(table::classify_number_format("yyyy-mm-dd") == FormatKind::Date);
  CHECK(table::classify_number_format("YYYY\\-MM\\-DD") == FormatKind::Date);
  CHECK(table::classify_number_format("m/d/yy h:mm") == FormatKind::DateTime);
  CHECK(table::classify_number_format("h:mm AM/PM") == FormatKind::Time);
  CHECK(table::classify_number_format("[h]:mm:ss") == FormatKind::Number);
  CHECK(table::classify_number_format("0.00E+00") == FormatKind::Number);
  CHECK(table::classify_number_format("\"Day\" 0") == FormatKind::Number);
  CHECK(table::classify_number_format("[Red]#,##0;[Blue]-#,##0") == FormatKind::Number);
  CHECK(table::classify_number_format("@") == FormatKind::Text);
  CHECK(table::classify_number_format("General") == FormatKind::General);
  CHECK(table::builtin_number_format(14) == "mm-dd-yy");
  CHECK(table::builtin_number_format_id("0.00") == 2u);
}
