// Convertible core - table family: registration entry points of each module.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::table {

void register_delimited(Registry& registry);     // csv, tsv, psv, csv-semicolon, fwf
void register_xlsx(Registry& registry);          // xlsx, xlsm, xltx, xlsb (read)
void register_ods(Registry& registry);           // ods, fods
void register_xls(Registry& registry);           // xls (BIFF8 read/write, BIFF5 read)
void register_spreadsheetml(Registry& registry); // SpreadsheetML 2003 XML
void register_gnumeric(Registry& registry);      // gnumeric (read)
void register_text_tables(Registry& registry);   // md, rst, adoc, org, tex, mediawiki, jira, box tables
void register_html_table(Registry& registry);    // html tables
void register_sqlite(Registry& registry);        // sqlite database files
void register_sql_dump(Registry& registry);      // sql scripts
void register_dbf(Registry& registry);           // dBase / FoxPro
void register_parquet(Registry& registry);       // Apache Parquet
void register_arrow(Registry& registry);         // Apache Arrow IPC / Feather 2 (read)
void register_legacy_sheets(Registry& registry); // SYLK, DIF, Lotus 1-2-3, Quattro Pro
void register_statistics(Registry& registry);    // SAS XPORT, SPSS .sav, Stata .dta
void register_bridges(Registry& registry);       // Table -> Data / Document / Archive

}  // namespace cvt::table
