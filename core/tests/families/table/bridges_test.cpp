// Bridges from tables to data, documents and archives, and the shared helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <string>

#include "families/table/codepages.hpp"
#include "families/table/table_test_support.hpp"
#include "util/json.hpp"

using namespace cvt;
using namespace cvt::table_test;

namespace {

const Bridge* find_bridge(Kind from, Kind to) {
  for (const Bridge& bridge : Registry::instance().bridges()) {
    if (bridge.from == from && bridge.to == to) return &bridge;
  }
  return nullptr;
}

Result<AnyIR> run_bridge(Kind to, const ir::Table& table, std::string_view options_json = {}) {
  const Bridge* bridge = find_bridge(Kind::Table, to);
  if (bridge == nullptr) return fail(Status::UnsupportedConversion, "no bridge");
  CVT_TRY_ASSIGN(const Options options, Options::parse(options_json));
  Context ctx;
  CVT_TRY_ASSIGN(ctx.options, options.normalized(bridge->options));
  ctx.registry = &Registry::instance();
  return bridge->fn(AnyIR(table), ctx);
}

}  // namespace

TEST_CASE("table/bridges: table to data as records, arrays or all sheets") {
  ir::Table table = sample_workbook(true);
  table.sheets[0].has_header = true;
  const auto records = run_bridge(Kind::Data, table);
  REQUIRE_OK(records);
  const Json& rows = records->get<ir::Data>().value;
  REQUIRE(rows.is_array());
  REQUIRE(rows.size() == 5);
  CHECK(rows[0]["Label"].as_string() == "plain");
  CHECK(rows[0]["Integer"].as_int64() == 42);
  CHECK(rows[0]["Integer"].is_integer());
  CHECK(rows[0]["Big"].as_int64() == 12345678901);
  CHECK(rows[0]["Float"].as_double() == 3.25);
  CHECK(rows[0]["Bool"].as_bool() == true);
  CHECK(rows[0]["Date"].as_string() == "2024-05-01");
  CHECK(rows[0]["DateTime"].as_string() == "2024-05-01T12:30:00");
  CHECK(rows[0]["Time"].as_string() == "12:30:00");
  CHECK(rows[2]["Time"].is_null());
  const auto arrays = run_bridge(Kind::Data, table, R"({"data_layout":"arrays","sheet":"Merged"})");
  REQUIRE_OK(arrays);
  CHECK(util::dump_json(arrays->get<ir::Data>().value) == R"([["Title"],["a","b","c"],[1,2,3],[null,"tall"]])");
  const auto all = run_bridge(Kind::Data, table, R"({"all_sheets":true})");
  REQUIRE_OK(all);
  const Json& book = all->get<ir::Data>().value;
  REQUIRE(book.is_object());
  CHECK(book.size() == 4);
  CHECK(book["Hidden"].is_array());
  CHECK(util::dump_json(book["Formulas"]) == R"([[1,2,3.14],[2,"ab"],[3,true],[6,"pos"]])");
  // Duplicate and empty titles get unique keys.
  ir::Sheet titled = make_sheet("t", {{S("a"), S("a"), E()}, {N(1), N(2), N(3)}});
  titled.has_header = true;
  const auto keyed = run_bridge(Kind::Data, single(titled));
  REQUIRE_OK(keyed);
  CHECK(util::dump_json(keyed->get<ir::Data>().value) == R"([{"a":1,"a_2":2,"column_3":3}])");
}

TEST_CASE("table/bridges: table to document") {
  ir::Table table = sample_workbook(true);
  table.sheets[0].has_header = true;
  const auto document = run_bridge(Kind::Document, table);
  REQUIRE_OK(document);
  const ir::Document& doc = document->get<ir::Document>();
  // Three visible sheets: heading + table each.
  REQUIRE(doc.blocks.size() == 6);
  CHECK(doc.blocks[0].is<ir::Heading>());
  CHECK(ir::plain_text(doc.blocks[0].get_if<ir::Heading>()->content) == "Types");
  const ir::TableBlock* types = doc.blocks[1].get_if<ir::TableBlock>();
  REQUIRE(types != nullptr);
  CHECK(types->rows[0].header);
  CHECK(ir::plain_text(types->rows[1].cells[1].content) == "42");
  CHECK(ir::plain_text(types->rows[4].cells[8].content) == "line one\nline two");
  const ir::TableBlock* merged = doc.blocks[5].get_if<ir::TableBlock>();
  REQUIRE(merged != nullptr);
  CHECK(merged->rows[0].cells.size() == 1);
  CHECK(merged->rows[0].cells[0].colspan == 3);
  CHECK(merged->rows[3].cells[1].rowspan == 2);
  CHECK(merged->rows.size() == 4);
  const auto with_hidden = run_bridge(Kind::Document, table, R"({"include_hidden":true,"sheet_headings":false})");
  REQUIRE_OK(with_hidden);
  CHECK(with_hidden->get<ir::Document>().blocks.size() == 4);
}

TEST_CASE("table/bridges: table to archive, one file per sheet") {
  ir::Table table = sample_workbook(true);
  table.sheets.push_back(make_sheet("Types", {{S("duplicate name")}}));
  table.sheets.push_back(make_sheet("a/b:c", {{S("odd name")}}));
  const auto archive = run_bridge(Kind::Archive, table);
  REQUIRE_OK(archive);
  const ir::Archive& files = archive->get<ir::Archive>();
  std::vector<std::string> names;
  for (const ir::ArchiveEntry& entry : files.entries) names.push_back(entry.path);
  CHECK(names == std::vector<std::string>{"Types.csv", "Formulas.csv", "Merged.csv", "Types (2).csv", "a_b_c.csv"});
  CHECK(test::str(files.entries[2].bytes) == "Title,,\r\na,b,c\r\n1,2,3\r\n,tall,\r\n");
  const auto xlsx = run_bridge(Kind::Archive, sample_workbook(true), R"({"sheet_format":"xlsx","include_hidden":true})");
  REQUIRE_OK(xlsx);
  const ir::Archive& books = xlsx->get<ir::Archive>();
  REQUIRE(books.entries.size() == 4);
  CHECK(books.entries[3].path == "Hidden.xlsx");
  const auto hidden = test::read_as<ir::Table>("xlsx", books.entries[3].bytes);
  REQUIRE_OK(hidden);
  CHECK(test::tables_equal(*hidden, single(hidden_sheet())));
  CHECK_STATUS(run_bridge(Kind::Archive, table, R"({"sheet_format":"png"})"), Status::InvalidArgument);
}

TEST_CASE("table/common: dates, numbers and type inference") {
  using table::DateStyle;
  CHECK(table::excel_serial_to_unix(45413) == 1714521600.0);  // 2024-05-01
  CHECK(table::format_datetime(table::excel_serial_to_unix(1)) == "1900-01-01");
  CHECK(table::format_datetime(table::excel_serial_to_unix(59)) == "1900-02-28");
  CHECK(table::format_datetime(table::excel_serial_to_unix(61)) == "1900-03-01");
  CHECK(table::format_datetime(table::excel_serial_to_unix(0, true)) == "1904-01-01");
  CHECK(table::unix_to_excel_serial(table::excel_serial_to_unix(45413.75)) == 45413.75);
  CHECK(table::unix_to_excel_serial(table::excel_serial_to_unix(30.5)) == 30.5);
  CHECK(table::format_datetime(table::excel_serial_to_unix(0.5)) == "12:00:00");
  CHECK(table::format_datetime(1714566600.25, DateStyle::DateTime) == "2024-05-01T12:30:00.250");
  CHECK(table::format_number(42) == "42");
  CHECK(table::format_number(-0.0) == "0");
  CHECK(table::format_number(0.1) == "0.1");
  CHECK(table::format_number(1e20) == "1e+20");
  CHECK(table::parse_iso_datetime("2024-02-30") == std::nullopt);
  CHECK(table::parse_iso_datetime("2023-02-29") == std::nullopt);
  CHECK(table::parse_iso_datetime("2024-05-01T12:30:00+02:00")->unix_seconds == 1714559400.0);
  CHECK(table::parse_iso_datetime("2024-05-01 9:05")->unix_seconds == 1714554300.0);
  CHECK(table::parse_iso_datetime("25:00") == std::nullopt);
  table::InferOptions infer;
  CHECK(table::infer_cell("42", infer).type == ir::CellType::Number);
  CHECK(table::infer_cell("-1.5e3", infer).number == -1500);
  CHECK(table::infer_cell(".5", infer).number == 0.5);
  CHECK(table::infer_cell("007", infer).type == ir::CellType::String);
  CHECK(table::infer_cell("1234567890123456", infer).type == ir::CellType::String);
  CHECK(table::infer_cell("inf", infer).type == ir::CellType::String);
  CHECK(table::infer_cell("1,5", infer).type == ir::CellType::String);
  CHECK(table::infer_cell("True", infer).type == ir::CellType::Bool);
  CHECK(table::infer_cell("", infer).type == ir::CellType::Empty);
  CHECK(table::infer_cell(" ", infer).type == ir::CellType::String);
  infer.decimal_separator = ',';
  CHECK(table::infer_cell("1,5", infer).number == 1.5);
  CHECK(table::column_name(0) == "A");
  CHECK(table::column_name(25) == "Z");
  CHECK(table::column_name(26) == "AA");
  CHECK(table::column_name(16383) == "XFD");
  CHECK(table::parse_a1("$XFD$1048576") == ir::CellPosition{1048575, 16383});
  CHECK(table::parse_a1("A0") == std::nullopt);
  CHECK(table::parse_a1("1A") == std::nullopt);
  CHECK(table::display_width("世界 ok") == 7);
  CHECK(table::display_width("é") == 1);
}

TEST_CASE("table/common: code pages") {
  const table::CodePageInfo* cp437 = table::find_code_page("ibm437");
  REQUIRE(cp437 != nullptr);
  CHECK(table::decode_code_page(test::bytes("\x82\x9B"), *cp437) == "é¢");
  std::size_t replaced = 0;
  CHECK(table::encode_code_page("é€", *cp437, &replaced) == "\x82?");
  CHECK(replaced == 1);
  CHECK(table::find_code_page(1251) != nullptr);
  CHECK(table::find_code_page("klingon") == nullptr);
  for (const table::CodePageInfo& page : table::code_pages()) {
    INFO(page.name);
    // Every assigned byte maps back to itself.
    for (int b = 0x80; b < 0x100; ++b) {
      const Bytes one{static_cast<std::byte>(b)};
      const std::string text = table::decode_code_page(one, page);
      if (text == "\xEF\xBF\xBD") continue;
      CHECK(table::encode_code_page(text, page) == std::string(1, static_cast<char>(b)));
    }
  }
}

TEST_CASE("table/registry: formats are registered consistently") {
  const Registry& registry = Registry::instance();
  CHECK(registry.validate().empty());
  for (const char* id : {"csv", "tsv", "psv", "fwf", "xlsx", "xlsm", "xltx", "xlsb", "xls", "ods", "ots", "fods",
                         "spreadsheetml", "gnumeric", "md", "rst", "adoc", "org", "tex", "wiki", "jira", "text-table",
                         "html", "sqlite", "sql", "dbf", "parquet", "slk", "dif", "wk1", "wq1"}) {
    INFO(id);
    CHECK(registry.find(id) != nullptr);
    CHECK(registry.has_reader(id));
  }
  for (const char* id : {"xlsb", "gnumeric", "wk1", "wq1"}) CHECK_FALSE(registry.has_writer(id));
  // A target only: semicolon files are read (and detected) as csv.
  const FormatInfo* semicolon = registry.find("csv-semicolon");
  REQUIRE(semicolon != nullptr);
  CHECK(semicolon->output_only);
  CHECK(registry.has_writer("csv-semicolon"));
}
