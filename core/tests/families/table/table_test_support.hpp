// Shared expectations for the table family tests: the synthetic sample
// workbook that tests/data/table/SOURCES.md describes, and cell builders.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <string>
#include <tuple>
#include <string_view>
#include <vector>

#include "families/table/common.hpp"
#include "ir/table.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

namespace cvt::table_test {

inline ir::Cell S(std::string text) { return ir::Cell::string(std::move(text)); }
inline ir::Cell N(double value) { return ir::Cell::numeric(value, table::format_number(value)); }
inline ir::Cell B(bool value) { return ir::Cell::boolean(value); }
inline ir::Cell E() { return ir::Cell{}; }
/// A DateTime from ISO text ("2024-05-01", "2024-05-01T12:30:00", "12:30:00").
inline ir::Cell D(std::string_view iso) {
  const auto parsed = table::parse_iso_datetime(iso);
  return parsed ? ir::Cell::datetime(parsed->unix_seconds, std::string(iso)) : ir::Cell::string("BAD DATE " + std::string(iso));
}

inline ir::Sheet make_sheet(std::string name, std::vector<std::vector<ir::Cell>> rows) {
  ir::Sheet sheet;
  sheet.name = std::move(name);
  sheet.rows = std::move(rows);
  return sheet;
}

/// Sheet "Types" of the sample workbook (see SOURCES.md).
inline ir::Sheet types_sheet() {
  return make_sheet("Types",
                    {
                        {S("Label"), S("Integer"), S("Float"), S("Big"), S("Bool"), S("Date"), S("DateTime"), S("Time"), S("Text")},
                        {S("plain"), N(42), N(3.25), N(12345678901.0), B(true), D("2024-05-01"), D("2024-05-01T12:30:00"),
                         D("12:30:00"), S("hello")},
                        {S("negative"), N(-7), N(-0.125), N(-98765432109.0), B(false), D("1999-12-31"),
                         D("2000-01-01T00:00:01"), D("00:00:01"), S("Grüße, 世界")},
                        {S("quotes"), N(0), N(1e-05), N(1e20), B(true), D("1970-01-01"), D("1960-02-29T06:00:00"), E(),
                         S("say \"hi\", it's")},
                        {S("multiline"), N(1000000), N(2.5), E(), E(), D("2100-12-31"), E(), D("23:59:59"),
                         S("line one\nline two")},
                        {S("emoji"), E(), N(0.1), N(123), B(false), E(), D("2024-02-29T23:59:59"), D("06:00:00"),
                         S("😀 smile")},
                    });
}

/// Sheet "Formulas": values as cached by a spreadsheet application, or only
/// the constants when the writer stores no results.
inline ir::Sheet formulas_sheet(bool cached_values) {
  if (!cached_values) return make_sheet("Formulas", {{N(1)}, {N(2)}, {N(3)}});
  return make_sheet("Formulas", {{N(1), N(2), N(3.14)}, {N(2), S("ab")}, {N(3), B(true)}, {N(6), S("pos")}});
}

inline ir::Sheet merged_sheet() {
  ir::Sheet sheet = make_sheet("Merged", {{S("Title")}, {S("a"), S("b"), S("c")}, {N(1), N(2), N(3)}, {E(), S("tall")}});
  sheet.merged = {ir::CellRange{0, 0, 0, 2}, ir::CellRange{3, 1, 4, 1}};
  return sheet;
}

inline ir::Sheet hidden_sheet() {
  ir::Sheet sheet = make_sheet("Hidden", {{S("secret")}, {N(99)}});
  sheet.visibility = ir::SheetVisibility::Hidden;
  return sheet;
}

inline ir::Table sample_workbook(bool cached_values = true) {
  ir::Table table;
  table.sheets = {types_sheet(), formulas_sheet(cached_values), merged_sheet(), hidden_sheet()};
  return table;
}

/// Merged ranges in a canonical order, for comparisons.
inline std::vector<ir::CellRange> sorted_merges(std::vector<ir::CellRange> ranges) {
  std::ranges::sort(ranges, [](const ir::CellRange& a, const ir::CellRange& b) {
    return std::tie(a.first_row, a.first_column, a.last_row, a.last_column) <
           std::tie(b.first_row, b.first_column, b.last_row, b.last_column);
  });
  return ranges;
}

/// A table with a single sheet.
inline ir::Table single(ir::Sheet sheet) {
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

/// Read a fixture of tests/data/table with the registered reader.
inline Result<ir::Table> read_fixture(std::string_view format, std::string_view name, std::string_view options = {},
                                      std::vector<std::string>* warnings = nullptr) {
  const Bytes bytes = test::fixture(std::string("table/") + std::string(name));
  return test::read_as<ir::Table>(format, bytes, options, warnings);
}

/// Write then read back with the same format.
inline Result<ir::Table> round_trip(std::string_view format, const ir::Table& table, std::string_view write_options = {},
                                    std::string_view read_options = {}) {
  CVT_TRY_ASSIGN(const Bytes bytes, test::write_from(format, table, write_options));
  return test::read_as<ir::Table>(format, bytes, read_options);
}

/// Rename every sheet (single-table formats name sheets after the file).
inline ir::Table renamed(ir::Table table, std::string_view name) {
  for (ir::Sheet& sheet : table.sheets) sheet.name = std::string(name);
  return table;
}

}  // namespace cvt::table_test
