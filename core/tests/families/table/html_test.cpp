// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>

#include "families/table/table_test_support.hpp"

using namespace cvt;
using namespace cvt::table_test;

TEST_CASE("table/html: decodes the tables of a page exported by LibreOffice") {
  const auto table = read_fixture("html", "lo_sample.html");
  REQUIRE_OK(table);
  // LibreOffice leaves hidden sheets out of HTML exports.
  ir::Table expected;
  expected.sheets = {types_sheet(), formulas_sheet(true), merged_sheet()};
  expected.sheets[0].name = "Sheet 1: Types";
  expected.sheets[1].name = "Sheet 2: Formulas";
  expected.sheets[2].name = "Sheet 3: Merged";
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  CHECK(sorted_merges(table->sheets[2].merged) == sorted_merges(expected.sheets[2].merged));
}

TEST_CASE("table/html: spans, entities, nested tables, captions and whitespace") {
  const std::string page = R"(<!DOCTYPE html><html><head><title>t</title><style>td{}</style><script>var x="<table>";</script></head><body>
<table id="outer"><caption>Shop &amp; more</caption>
<thead><tr><th>Item</th><th colspan=2>Price&nbsp;range</th></tr></thead>
<tbody>
<tr><td rowspan="2">Caf&eacute;</td><td>1.50<td>2&#46;5</tr>
<tr><td>  spaced   out  </td><td>line<br>break</td></tr>
<tr><td>inner<table><tr><td>nested</td></tr></table></td><td><!-- note -->x</td><td>&#x263A;</td></tr>
</tbody></table>
<p>Not a table</p>
</body></html>)";
  const auto table = test::read_as<ir::Table>("html", as_bytes(page));
  REQUIRE_OK(table);
  REQUIRE(table->sheets.size() == 2);
  const ir::Sheet& sheet = table->sheets[0];
  const ir::Sheet expected = make_sheet("Shop & more", {{S("Item"), S("Price range")},
                                                        {S("Café"), N(1.5), N(2.5)},
                                                        {E(), S("spaced out"), S("line\nbreak")},
                                                        {S("inner"), S("x"), S("☺")}});
  CHECK_MESSAGE(test::tables_equal(single(sheet), single(expected)), test::table_diff(single(sheet), single(expected)));
  CHECK(sheet.has_header);
  CHECK(sorted_merges(sheet.merged) == sorted_merges({ir::CellRange{0, 1, 0, 2}, ir::CellRange{1, 0, 2, 0}}));
  CHECK(table->sheets[1].rows[0][0].text == "nested");
  CHECK_STATUS(test::read_as<ir::Table>("html", test::bytes("<p>no tables</p>")), Status::Decode);
}

TEST_CASE("table/html: writes a styled page or a fragment that reads back") {
  ir::Table table = sample_workbook(true);
  table.sheets[0].has_header = true;
  const auto page = test::write_from("html", table);
  REQUIRE_OK(page);
  const std::string text = test::str(*page);
  CHECK(text.starts_with("<!DOCTYPE html>"));
  CHECK(text.find("<style>") != std::string::npos);
  CHECK(text.find("<h2>Types</h2>") != std::string::npos);
  CHECK(text.find("Hidden") == std::string::npos);  // hidden sheets are not shown
  CHECK(text.find("<td class=\"num\">42</td>") != std::string::npos);
  CHECK(text.find("colspan=\"3\"") != std::string::npos);
  CHECK(text.find("rowspan=\"2\"") != std::string::npos);
  CHECK(text.find("line one<br>line two") != std::string::npos);
  const auto back = test::read_as<ir::Table>("html", *page);
  REQUIRE_OK(back);
  ir::Table expected;
  expected.sheets = {types_sheet(), formulas_sheet(true), merged_sheet()};
  CHECK_MESSAGE(test::tables_equal(*back, expected), test::table_diff(*back, expected));
  const auto fragment = test::write_from("html", table, R"({"html_fragment":true,"sheet":"Merged"})");
  REQUIRE_OK(fragment);
  CHECK(test::str(*fragment).starts_with("<table>"));
}

TEST_CASE("table/html: hostile pages") {
  CHECK(test::fuzz_reader<ir::Table>("html", test::fixture("table/lo_sample.html")) > 0);
  std::string deep;
  for (int i = 0; i < 100; ++i) deep += "<table><tr><td>";
  CHECK_STATUS(test::read_as<ir::Table>("html", as_bytes(deep)), Status::Limit);
  const std::string spans = "<table><tr><td colspan=1000 rowspan=65534>x</td></tr></table>";
  Limits limits;
  limits.max_decompressed_bytes = 1024 * 1024;
  CHECK_STATUS(test::read_as<ir::Table>("html", as_bytes(spans), {}, nullptr, limits), Status::Limit);
}
