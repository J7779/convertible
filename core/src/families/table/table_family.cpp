// Convertible core - table family.
//
// Tabular data read into and written from ir::Table: delimited and
// fixed-width text, Excel (xlsx/xlsm/xltx, xlsb, xls), OpenDocument (ods,
// fods), SpreadsheetML 2003, Gnumeric, text-markup tables (Markdown, reST,
// AsciiDoc, Org, LaTeX, MediaWiki, Jira, box drawings, HTML), databases and
// columnar files (SQLite, SQL dumps, dBase, Parquet), legacy spreadsheets
// (SYLK, DIF, Lotus 1-2-3, Quattro Pro) and statistics packages (SAS XPORT,
// SPSS, Stata). Plus the bridges from tables to data, documents and
// archives of one file per sheet.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/table/table.hpp"

namespace cvt {

void register_table_family(Registry& registry) {
  table::register_delimited(registry);
  table::register_xlsx(registry);
  table::register_ods(registry);
  table::register_xls(registry);
  table::register_spreadsheetml(registry);
  table::register_gnumeric(registry);
  table::register_text_tables(registry);
  table::register_html_table(registry);
  table::register_sqlite(registry);
  table::register_sql_dump(registry);
  table::register_dbf(registry);
  table::register_parquet(registry);
  table::register_arrow(registry);
  table::register_legacy_sheets(registry);
  table::register_statistics(registry);
  table::register_bridges(registry);
}

}  // namespace cvt
