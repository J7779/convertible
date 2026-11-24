// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <string>
#include <utility>

#include "engine/detect.hpp"

#include "families/table/table_test_support.hpp"

using namespace cvt;
using namespace cvt::table_test;

namespace {

// The table that tests/data/table/tabulate.* hold (see SOURCES.md).
ir::Sheet tabulate_rows(std::size_t rows = 4) {
  ir::Sheet sheet = make_sheet("", {{S("Item"), S("Qty"), S("Price"), S("Note")},
                                    {S("Tea"), N(3), N(1.5), S("green")},
                                    {S("Grüße"), N(-12), N(0.99), S("世界")},
                                    {S("pipe|char"), N(0), N(1e-05)}});
  sheet.rows.resize(rows);
  sheet.has_header = true;
  return sheet;
}

// Compare the first `rows` rows of the first sheet, ignoring the sheet name.
std::string first_rows_diff(const ir::Table& got, std::size_t rows) {
  if (got.sheets.empty()) return "no sheets";
  ir::Table trimmed = renamed(got, "");
  trimmed.sheets.resize(1);
  trimmed.sheets[0].rows.resize(std::min(rows, trimmed.sheets[0].rows.size()));
  return test::table_diff(trimmed, single(tabulate_rows(rows)));
}

}  // namespace

TEST_CASE("table/text tables: decode tables written by tabulate") {
  struct Case {
    const char* format;
    const char* file;
    std::size_t rows;  // rows comparable (an unescaped '|' makes the last one ambiguous in some syntaxes)
  };
  const Case cases[] = {
      {"md", "tabulate.md", 4},          {"rst", "tabulate.rst", 4},   {"rst", "tabulate.simple.rst", 4},
      {"tex", "tabulate.tex", 4},        {"tex", "tabulate.booktabs.tex", 4}, {"html", "tabulate.html", 4},
      {"text-table", "tabulate.box.txt", 4}, {"text-table", "tabulate.psql.txt", 3}, {"adoc", "tabulate.adoc", 3},
      {"org", "tabulate.org", 3},        {"jira", "tabulate.jira", 3}, {"mediawiki", "tabulate.wiki", 3},
  };
  for (const Case& c : cases) {
    INFO(c.file);
    const auto table = read_fixture(c.format, c.file);
    REQUIRE_OK(table);
    CHECK_MESSAGE(first_rows_diff(*table, c.rows).empty(), first_rows_diff(*table, c.rows));
    CHECK(table->sheets[0].has_header);
  }
}

TEST_CASE("table/text tables: every writer reads back with its own reader") {
  ir::Sheet sheet = tabulate_rows();
  sheet.name = "Prices";
  sheet.rows.push_back({S("multi\nline"), N(1000000), E(), S("x & y < z")});
  sheet.rows.push_back({S("wide 世界"), N(-0.5), N(2), S("back\\slash")});
  const ir::Table table = single(sheet);
  for (const char* format : {"md", "rst", "adoc", "tex", "mediawiki", "jira", "org", "text-table", "html"}) {
    INFO(format);
    const auto bytes = test::write_from(format, table);
    REQUIRE_OK(bytes);
    const auto back = test::read_as<ir::Table>(format, *bytes);
    REQUIRE_OK(back);
    ir::Table expected = table;
    // Syntaxes without line breaks inside cells fold them into spaces.
    const std::string_view f = format;
    if (f == "org" || f == "tex") expected.sheets[0].rows[4][0] = S("multi line");
    const ir::Table got = renamed(*back, "Prices");
    CHECK_MESSAGE(test::tables_equal(got, expected), test::table_diff(got, expected));
  }
}

TEST_CASE("table/text tables: writer output looks right") {
  const ir::Table table = single(tabulate_rows());
  const auto text = [&](std::string_view format, std::string_view options = {}) {
    const auto bytes = test::write_from(format, table, options);
    return bytes ? test::str(*bytes) : std::string("ERROR");
  };
  CHECK(text("md") ==
        "| Item       | Qty | Price | Note  |\n"
        "| ---------- | --: | ----: | ----- |\n"
        "| Tea        |   3 |   1.5 | green |\n"
        "| Grüße      | -12 |  0.99 | 世界  |\n"
        "| pipe\\|char |   0 | 1e-05 |       |\n");
  CHECK(text("md", R"({"align_columns":false})").starts_with("| Item | Qty | Price | Note |\n| --- | --: | --: | --- |\n"));
  CHECK(text("tex").starts_with("\\begin{tabular}{lrrl}\n\\hline\n\\textbf{Item}"));
  CHECK(text("tex", R"({"booktabs":true})").find("\\midrule") != std::string::npos);
  CHECK(text("jira").starts_with("||Item||Qty||Price||Note||\n|Tea|3|1.5|green|\n"));
  CHECK(text("text-table", R"({"box_style":"ascii"})").starts_with("+-----------+-----+-------+-------+\n| Item      | Qty | Price | Note  |\n+===="));
  CHECK(text("text-table", R"({"box_style":"double"})").starts_with("╔"));
  CHECK(text("text-table", R"({"box_style":"rounded"})").starts_with("╭"));
  CHECK(text("org").find("\n|-----------------+-----+-------+-------|\n") != std::string::npos);
  CHECK(text("mediawiki").starts_with("{| class=\"wikitable\"\n! Item !! Qty"));
  CHECK(text("adoc").starts_with("[cols=\"4\", options=\"header\"]\n|===\n| Item | Qty"));
  CHECK(text("rst").find("+===========+=====+") != std::string::npos);
}

TEST_CASE("table/text tables: every table of a document becomes a sheet") {
  const std::string markdown =
      "# Fruit\n\nIntro text.\n\n| a | b |\n|---|:-:|\n| 1 | `x|y` |\n| 2 | \\| |\n\nMore text.\n\n## Veg\n\n| c |\n|---|\n| carrot |\n";
  const auto table = test::read_as<ir::Table>("md", as_bytes(markdown));
  REQUIRE_OK(table);
  REQUIRE(table->sheets.size() == 2);
  CHECK(table->sheets[0].name == "Fruit");
  CHECK(table->sheets[1].name == "Veg");
  CHECK(table->sheets[0].rows[1][1].text == "`x|y`");
  CHECK(table->sheets[0].rows[2][1].text == "|");
  CHECK(table->sheets[1].rows[1][0].text == "carrot");
  CHECK_STATUS(test::read_as<ir::Table>("md", test::bytes("no tables here")), Status::Decode);
}

TEST_CASE("table/text tables: LaTeX commands and escapes") {
  const std::string tex = R"(\begin{table}\begin{tabular}{|l|c|} \hline
\textbf{Name} & \multicolumn{1}{c}{Cost \%} \\ \hline
A\&B & 5\$ \\ % a comment & ignored
C\_D & \textit{7} \\ \hline
\end{tabular}\end{table})";
  const auto table = test::read_as<ir::Table>("tex", as_bytes(tex));
  REQUIRE_OK(table);
  const ir::Table expected = single(make_sheet("", {{S("Name"), S("Cost %")}, {S("A&B"), S("5$")}, {S("C_D"), N(7)}}));
  CHECK_MESSAGE(test::tables_equal(renamed(*table, ""), expected), test::table_diff(renamed(*table, ""), expected));
  CHECK(table->sheets[0].has_header);
}

TEST_CASE("table/text tables: detection and hostile input") {
  const Registry& registry = Registry::instance();
  CHECK(detect_format(registry, test::fixture("table/tabulate.jira"), "")->format->id == "jira");
  CHECK(detect_format(registry, test::fixture("table/tabulate.box.txt"), "")->format->id == "text-table");
  for (const auto& [format, file] : std::vector<std::pair<const char*, const char*>>{
           {"md", "tabulate.md"}, {"rst", "tabulate.rst"}, {"adoc", "tabulate.adoc"}, {"org", "tabulate.org"},
           {"tex", "tabulate.tex"}, {"mediawiki", "tabulate.wiki"}, {"jira", "tabulate.jira"},
           {"text-table", "tabulate.box.txt"}, {"html", "tabulate.html"}}) {
    INFO(file);
    CHECK(test::fuzz_reader<ir::Table>(format, test::fixture(std::string("table/") + file)) > 0);
  }
}
