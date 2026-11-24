// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>

#include "engine/detect.hpp"
#include "families/table/delimited.hpp"
#include "families/table/table_test_support.hpp"

using namespace cvt;
using namespace cvt::table_test;
using namespace std::string_view_literals;

namespace {

// The rows of tests/data/table/csv/*: see SOURCES.md.
ir::Sheet python_rows(std::string name) {
  ir::Sheet sheet = make_sheet(std::move(name),
                               {
                                   {S("name"), S("qty"), S("price"), S("when"), S("ok"), S("note")},
                                   {S("Tea"), N(3), N(1.5), D("2024-05-01"), B(true), S("plain")},
                                   {S("Milk, oat"), N(12), N(0.99), D("2024-05-02T10:30:00"), B(false), S("says \"hi\"")},
                                   {S("Grüße 世界"), N(-7), N(1e-05), E(), B(true), S("line one\nline two")},
                                   {S("007"), N(0), E(), D("1999-12-31")},
                                   {S("  padded  "), S("12345678901234567890"), N(3.14159), D("2024-02-29 23:59:59"), B(false), S("tab\there")},
                               });
  sheet.has_header = true;
  return sheet;
}

Result<ir::Table> read_csv(std::string_view format, std::string_view text, std::string_view options = {},
                           std::vector<std::string>* warnings = nullptr) {
  return test::read_as<ir::Table>(format, as_bytes(text), options, warnings);
}

std::string write_csv(const ir::Table& table, std::string_view options = {}, std::string_view format = "csv") {
  auto bytes = test::write_from(format, table, options);
  return bytes ? test::str(*bytes) : std::string("ERROR: ") + bytes.error().message;
}

}  // namespace

TEST_CASE("table/csv: decodes files written by Python's csv module in every dialect") {
  struct Case {
    const char* format;
    const char* file;
  };
  for (const Case& c : {Case{"csv", "csv/excel_crlf.csv"}, Case{"csv", "csv/unix_all_quoted.csv"}, Case{"tsv", "csv/excel_tab.tsv"},
                        Case{"psv", "csv/pipe.psv"}, Case{"tsv", "csv/utf16le_bom.tsv"}}) {
    INFO(c.file);
    const auto table = read_fixture(c.format, c.file);
    REQUIRE_OK(table);
    const ir::Table expected = single(python_rows(""));
    CHECK_MESSAGE(test::tables_equal(renamed(*table, ""), expected), test::table_diff(renamed(*table, ""), expected));
    CHECK(table->sheets[0].has_header);
  }
  // The delimiter is sniffed even when the format id says otherwise.
  const auto tab_as_csv = read_fixture("csv", "csv/excel_tab.tsv");
  REQUIRE_OK(tab_as_csv);
  CHECK(tab_as_csv->sheets[0].column_count() == 6);
}

TEST_CASE("table/csv: BOM-less UTF-16 and legacy code pages") {
  const auto utf16 = read_fixture("csv", "csv/utf16be.csv");
  REQUIRE_OK(utf16);
  ir::Sheet first_rows = python_rows("");
  first_rows.rows.resize(3);
  CHECK_MESSAGE(test::tables_equal(renamed(*utf16, ""), single(first_rows)), test::table_diff(renamed(*utf16, ""), single(first_rows)));
  const ir::Table cp1252 = single(make_sheet("", {{S("word"), S("price")}, {S("café"), N(4)}, {S("naïve"), N(5)}, {S("€uro"), N(6)}}));
  const auto guessed = read_fixture("csv", "csv/cp1252.csv");
  REQUIRE_OK(guessed);
  CHECK_MESSAGE(test::tables_equal(renamed(*guessed, ""), cp1252), test::table_diff(renamed(*guessed, ""), cp1252));
  const auto explicit_page = read_fixture("csv", "csv/cp1252.csv", R"({"source_encoding":"windows-1252"})");
  REQUIRE_OK(explicit_page);
  CHECK(test::tables_equal(renamed(*explicit_page, ""), cp1252));
  // Read as Latin-1 the euro sign is a C1 control: a different text.
  const auto wrong = read_fixture("csv", "csv/cp1252.csv", R"({"source_encoding":"iso-8859-1"})");
  REQUIRE_OK(wrong);
  CHECK(wrong->sheets[0].rows[3][0].text != "€uro");
}

TEST_CASE("table/csv: semicolons, decimal commas, BOM and the Excel sep= hint") {
  const ir::Table text_numbers = single(make_sheet("", {{S("Artikel"), S("Menge"), S("Preis")}, {S("Käse"), N(2), S("3,50")}, {S("Brot; dunkel"), N(1), S("2,25")}, {S("Wein"), N(-1), S("12,00")}}));
  const ir::Table real_numbers = single(make_sheet("", {{S("Artikel"), S("Menge"), S("Preis")}, {S("Käse"), N(2), N(3.5)}, {S("Brot; dunkel"), N(1), N(2.25)}, {S("Wein"), N(-1), N(12)}}));
  const auto plain = read_fixture("csv", "csv/semicolon_bom.csv");
  REQUIRE_OK(plain);
  CHECK_MESSAGE(test::tables_equal(renamed(*plain, ""), text_numbers), test::table_diff(renamed(*plain, ""), text_numbers));
  const auto comma = read_fixture("csv-semicolon", "csv/semicolon_bom.csv", R"({"decimal_separator":"comma"})");
  REQUIRE_OK(comma);
  CHECK_MESSAGE(test::tables_equal(renamed(*comma, ""), real_numbers), test::table_diff(renamed(*comma, ""), real_numbers));
  CHECK(comma->sheets[0].rows[1][2].text == "3,50");  // the text as written
  const auto hint = read_fixture("csv", "csv/sep_hint.csv", R"({"decimal_separator":"comma"})");
  REQUIRE_OK(hint);
  CHECK_MESSAGE(test::tables_equal(renamed(*hint, ""), real_numbers), test::table_diff(renamed(*hint, ""), real_numbers));
}

TEST_CASE("table/csv: backslash escapes, ragged rows and a missing final line break") {
  const auto escaped = read_fixture("csv", "csv/backslash_escape.csv", R"({"source_escape":"backslash"})");
  REQUIRE_OK(escaped);
  const ir::Table expected = single(make_sheet("", {{S("id"), S("text")}, {N(1), S("he said \"yes\"")}, {N(2), S("back\\slash")}}));
  CHECK_MESSAGE(test::tables_equal(renamed(*escaped, ""), expected), test::table_diff(renamed(*escaped, ""), expected));
  const auto ragged = read_fixture("csv", "csv/ragged.csv");
  REQUIRE_OK(ragged);
  const ir::Table ragged_expected = single(make_sheet("", {{S("a"), S("b"), S("c")}, {N(1)}, {N(2), N(3), N(4), N(5)}, {}, {N(6), N(7)}}));
  CHECK_MESSAGE(test::tables_equal(renamed(*ragged, ""), ragged_expected), test::table_diff(renamed(*ragged, ""), ragged_expected));
  CHECK(ragged->sheets[0].name == "");  // no file name given
}

TEST_CASE("table/csv: dialect torture") {
  struct Case {
    const char* name;
    std::string_view input;
    std::vector<std::vector<std::string>> rows;  // "" = empty field
  };
  const Case cases[] = {
      {"CR line endings", "a,b\r1,2\r", {{"a", "b"}, {"1", "2"}}},
      {"mixed line endings", "a,b\n1,2\r\n3,4\r5,6", {{"a", "b"}, {"1", "2"}, {"3", "4"}, {"5", "6"}}},
      {"stray quote inside a field", "a,b\nsay \"x\",2\n", {{"a", "b"}, {"say \"x\"", "2"}}},
      {"text after closing quote", "a,b\n\"ab\"cd,2\n", {{"a", "b"}, {"abcd", "2"}}},
      {"quoted line breaks", "a,b\n\"1\r\n2\",3\n", {{"a", "b"}, {"1\r\n2", "3"}}},
      {"doubled quotes", "a\n\"\"\"\"\n\"x\"\"y\"\n", {{"a"}, {"\""}, {"x\"y"}}},
      {"trailing delimiter", "a,b,\n1,2,\n", {{"a", "b", ""}, {"1", "2", ""}}},
      {"blank lines inside", "a,b\n\n1,2\n", {{"a", "b"}, {}, {"1", "2"}}},
      {"empty quoted field", "a,\"\",c\n", {{"a", "", "c"}}},
      {"single column", "x\ny\nz\n", {{"x"}, {"y"}, {"z"}}},
      {"spaces kept", " a , b \n", {{" a ", " b "}}},
  };
  for (const Case& c : cases) {
    INFO(c.name);
    const auto table = read_csv("csv", c.input, R"({"infer_types":false,"header":"no"})");
    REQUIRE_OK(table);
    const ir::Sheet& sheet = table->sheets.at(0);
    REQUIRE(sheet.rows.size() == c.rows.size());
    for (std::size_t r = 0; r < c.rows.size(); ++r) {
      std::vector<std::string> got;
      for (const ir::Cell& cell : sheet.rows[r]) got.push_back(cell.text);
      std::vector<std::string> want = c.rows[r];
      while (!want.empty() && want.back().empty()) want.pop_back();
      CHECK(got == want);
    }
  }
  std::vector<std::string> warnings;
  const auto open = read_csv("csv", "a,\"never closed\nstill here", R"({"infer_types":false})", &warnings);
  REQUIRE_OK(open);
  CHECK(open->sheets[0].rows[0][1].text == "never closed\nstill here");
  CHECK(warnings.size() == 1);
  const auto empty = read_csv("csv", "");
  REQUIRE_OK(empty);
  CHECK(empty->sheets[0].rows.empty());
}

TEST_CASE("table/csv: dialect sniffing") {
  CHECK(table::sniff_dialect("a;b;c\n1,5;2,5;3\n4;5;6\n").delimiter == ';');
  CHECK(table::sniff_dialect("a\tb\n1\t2\n").delimiter == '\t');
  CHECK(table::sniff_dialect("a|b|c\n1|2|3\n").delimiter == '|');
  CHECK(table::sniff_dialect("a,\"b;c\",d\n1,2,3\n").delimiter == ',');
  CHECK(table::sniff_dialect("just one column\nhere\n").delimiter == ',');
  const table::Dialect hint = table::sniff_dialect("sep=|\na|b\n");
  CHECK(hint.delimiter == '|');
  CHECK(hint.skip_first_line);
}

TEST_CASE("table/csv: reader options") {
  SUBCASE("source_delimiter") {
    const auto table = read_csv("csv", "a;b,c\n1;2,3\n", R"({"source_delimiter":"semicolon"})");
    REQUIRE_OK(table);
    CHECK(table->sheets[0].rows[0][1].text == "b,c");
  }
  SUBCASE("header") {
    const auto none = read_csv("csv", "2024,7\n1,2\n", R"({"header":"no"})");
    const auto yes = read_csv("csv", "2024,7\n1,2\n", R"({"header":"yes"})");
    const auto guessed = read_csv("csv", "2024,7\n1,2\n");
    REQUIRE_OK(none);
    REQUIRE_OK(yes);
    REQUIRE_OK(guessed);
    CHECK_FALSE(none->sheets[0].has_header);
    CHECK(yes->sheets[0].has_header);
    CHECK(yes->sheets[0].rows[0][0].type == ir::CellType::String);  // titles stay text
    CHECK_FALSE(guessed->sheets[0].has_header);
  }
  SUBCASE("infer_types") {
    const auto off = read_csv("csv", "n,d\n1,2024-05-01\n", R"({"infer_types":false})");
    REQUIRE_OK(off);
    CHECK(off->sheets[0].rows[1][0].type == ir::CellType::String);
    CHECK(off->sheets[0].rows[1][1].type == ir::CellType::String);
  }
  SUBCASE("date_order") {
    const auto dmy = read_csv("csv", "d\n31/12/2024\n", R"({"date_order":"dmy"})");
    const auto mdy = read_csv("csv", "d\n12/31/2024\n", R"({"date_order":"mdy"})");
    const auto iso = read_csv("csv", "d\n31/12/2024\n");
    REQUIRE_OK(dmy);
    REQUIRE_OK(mdy);
    REQUIRE_OK(iso);
    CHECK(test::tables_equal(*dmy, single(make_sheet("", {{S("d")}, {D("2024-12-31")}}))));
    CHECK(test::tables_equal(*mdy, single(make_sheet("", {{S("d")}, {D("2024-12-31")}}))));
    CHECK(iso->sheets[0].rows[1][0].type == ir::CellType::String);
  }
  SUBCASE("source_quote") {
    const auto single_quotes = read_csv("csv", "a,b\n'x,y',2\n", R"({"source_quote":"single","infer_types":false})");
    REQUIRE_OK(single_quotes);
    CHECK(single_quotes->sheets[0].rows[1][0].text == "x,y");
    const auto none = read_csv("csv", "a,b\n\"x\",2\n", R"({"source_quote":"none","infer_types":false})");
    REQUIRE_OK(none);
    CHECK(none->sheets[0].rows[1][0].text == "\"x\"");
  }
  SUBCASE("comment_prefix") {
    const auto table = read_csv("csv", "# exported\na,b\n1,2\n", R"({"comment_prefix":"#"})");
    REQUIRE_OK(table);
    CHECK(table->sheets[0].rows.size() == 2);
  }
  CHECK_STATUS(read_csv("csv", "a", R"({"source_encoding":"klingon"})"), Status::InvalidArgument);
}

TEST_CASE("table/csv: writer options and exact round trips") {
  const Bytes original = test::fixture("table/csv/excel_crlf.csv");
  const test::Converted same = test::convert(original, "csv", "csv");
  REQUIRE(same.ok());
  CHECK(same.data == original);  // Python's excel dialect is our default dialect
  const ir::Table table = single(python_rows("Data"));
  CHECK(write_csv(table, R"({"line_ending":"lf"})").starts_with("name,qty,price,when,ok,note\nTea,3,1.5,2024-05-01,TRUE,plain\n"));
  CHECK(write_csv(table, R"({"delimiter":"tab"})").starts_with("name\tqty\tprice\twhen\tok\tnote\r\n"));
  CHECK(write_csv(table, R"({"delimiter":";"})").find("Milk, oat;12") != std::string::npos);
  CHECK(write_csv(table, R"({"quoting":"all"})").starts_with("\"name\",\"qty\""));
  CHECK(write_csv(table, R"({"quoting":"nonnumeric"})").find("\"Tea\",3,1.5,\"2024-05-01\",TRUE") != std::string::npos);
  CHECK(write_csv(table, R"({"quoting":"never"})").find("Milk, oat,12") != std::string::npos);
  CHECK(write_csv(table, R"({"bom":true})").starts_with("\xEF\xBB\xBFname"));
  const std::string utf16 = write_csv(table, R"({"encoding":"utf-16le","bom":true,"delimiter":"tab"})");
  CHECK(utf16.starts_with("\xFF\xFEn\0a\0"sv));
  const std::string latin = write_csv(single(make_sheet("", {{S("café 世界")}})), R"({"encoding":"windows-1252"})");
  CHECK(latin == "caf\xE9 ??\r\n");
  CHECK(write_csv(table, R"({"delimiter":"\""})").starts_with("ERROR"));
  for (const char* options : {R"({"line_ending":"lf"})", R"({"quoting":"all"})", R"({"delimiter":"|"})", R"({"encoding":"utf-16be","bom":true})"}) {
    INFO(options);
    const auto back = round_trip("csv", table, options);
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::tables_equal(renamed(*back, "Data"), table), test::table_diff(renamed(*back, "Data"), table));
  }
}

TEST_CASE("table/csv: formula injection guard") {
  const ir::Table table = single(make_sheet("", {{S("=HYPERLINK(\"x\")"), S("+1"), S("-2"), S("@cmd"), N(-3), S("safe")}}));
  CHECK(write_csv(table) == "\"=HYPERLINK(\"\"x\"\")\",+1,-2,@cmd,-3,safe\r\n");
  std::vector<std::string> warnings;
  const auto guarded = test::write_from("csv", table, R"({"formula_guard":true})", &warnings);
  REQUIRE_OK(guarded);
  CHECK(test::str(*guarded) == "\"'=HYPERLINK(\"\"x\"\")\",'+1,'-2,'@cmd,-3,safe\r\n");
  CHECK(warnings.size() == 1);
}

TEST_CASE("table/csv: multi-sheet workbooks choose a sheet") {
  const ir::Table book = sample_workbook(true);
  std::vector<std::string> warnings;
  const auto first = test::write_from("csv", book, {}, &warnings);
  REQUIRE_OK(first);
  CHECK(test::str(*first).starts_with("Label,Integer"));
  CHECK(warnings.size() == 1);
  CHECK(write_csv(book, R"({"sheet":"Merged"})") == "Title,,\r\na,b,c\r\n1,2,3\r\n,tall,\r\n");
  CHECK(write_csv(book, R"({"sheet":"3"})") == "secret\r\n99\r\n");
  CHECK(write_csv(book, R"({"sheet":"merged"})").starts_with("Title"));
  CHECK_STATUS(test::write_from("csv", book, R"({"sheet":"Nope"})"), Status::InvalidArgument);
  // Hidden sheets are skipped when choosing the default.
  ir::Table hidden_first = book;
  std::swap(hidden_first.sheets[0], hidden_first.sheets[3]);
  CHECK(write_csv(hidden_first).starts_with("1,2,3.14"));  // "Formulas", the first visible sheet
  CHECK(write_csv(ir::Table{}) == "");
}

TEST_CASE("table/csv: detection from content") {
  CHECK(test::convert(test::fixture("table/csv/excel_crlf.csv"), "", "tsv").format_id == "tsv");
  const Registry& registry = Registry::instance();
  const auto best = [&](std::string_view text, std::string_view filename = {}) {
    const auto found = detect_format(registry, as_bytes(text), filename);
    return found ? found->format->id : std::string("none");
  };
  CHECK(best("name,qty\nTea,3\nMilk,4\n") == "csv");
  CHECK(best("name\tqty\nTea\t3\nMilk\t4\n") == "tsv");
  CHECK(best("name|qty\nTea|3\nMilk|4\n") == "psv");
  CHECK(best("| a | b |\n|---|---|\n| 1 | 2 |\n") != "psv");  // a Markdown table
  CHECK(best("name;qty\nTea;3\nMilk;4\n", "x.csv") != "txt");
  CHECK(best("plain words, nothing more") != "csv");
}

TEST_CASE("table/csv: hostile input") {
  CHECK(test::fuzz_reader<ir::Table>("csv", test::fixture("table/csv/excel_crlf.csv")) > 0);
  CHECK(test::fuzz_reader<ir::Table>("tsv", test::fixture("table/csv/utf16le_bom.tsv")) > 0);
  Limits limits;
  limits.max_decompressed_bytes = 64 * 1024;  // about a thousand cells
  const std::string wide(200000, ',');
  CHECK_STATUS(test::read_as<ir::Table>("csv", as_bytes(wide), {}, nullptr, limits), Status::Limit);
}

TEST_CASE("table/fwf: fixed-width text") {
  const auto table = read_fixture("fwf", "csv/report.fwf");
  REQUIRE_OK(table);
  const ir::Table expected = single(make_sheet("", {{S("Item"), S("Qty"), S("Price"), S("Date")},
                                                    {S("Tea"), N(3), N(1.5), D("2024-05-01")},
                                                    {S("Milk oat"), N(12), N(0.99), D("2024-05-02")},
                                                    {S("Grüße"), N(-7), N(100), D("1999-12-31")}}));
  CHECK_MESSAGE(test::tables_equal(renamed(*table, ""), expected), test::table_diff(renamed(*table, ""), expected));
  CHECK(table->sheets[0].has_header);
  const auto widths = read_fixture("fwf", "csv/report.fwf", R"({"column_widths":"12,6"})");
  REQUIRE_OK(widths);
  CHECK(widths->sheets[0].rows[1][0].text == "Tea");
  CHECK(widths->sheets[0].rows[1][1].number == 3);
  CHECK_STATUS(read_fixture("fwf", "csv/report.fwf", R"({"column_widths":"x"})"), Status::InvalidArgument);
  // Written aligned, numbers to the right; reads back the same.
  const std::string text = test::str(test::write_from("fwf", expected).value());
  CHECK(text.find("\nTea         3    1.5  2024-05-01\n") != std::string::npos);  // numbers to the right
  const auto back = test::read_as<ir::Table>("fwf", as_bytes(text));
  REQUIRE_OK(back);
  CHECK_MESSAGE(test::tables_equal(renamed(*back, ""), expected), test::table_diff(renamed(*back, ""), expected));
  const std::string gap = test::str(test::write_from("fwf", expected, R"({"column_gap":4})").value());
  CHECK(gap.find("Item        ") == 0);
  CHECK(test::fuzz_reader<ir::Table>("fwf", test::fixture("table/csv/report.fwf")) > 0);
}
