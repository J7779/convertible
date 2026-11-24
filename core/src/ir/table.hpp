// Convertible core - Table IR: a workbook of sheets of cells.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace cvt::ir {

enum class CellType {
  Empty,     ///< no value
  String,    ///< `text` is the value
  Number,    ///< `number` is the value
  Bool,      ///< `number` is 0 (false) or 1 (true)
  DateTime,  ///< `number` is seconds since 1970-01-01T00:00:00Z (may be fractional or negative)
};

/// One spreadsheet cell.
struct Cell {
  /// Display text, always filled in for non-empty cells so that text-only
  /// writers (CSV, Markdown, HTML) need no formatting logic: the string
  /// itself, the number as written in the source, "TRUE"/"FALSE", or an
  /// ISO 8601 timestamp for DateTime.
  std::string text;
  CellType type = CellType::Empty;
  /// Numeric value for Number, Bool and DateTime cells; 0 otherwise.
  double number = 0.0;

  [[nodiscard]] static Cell string(std::string value) {
    return Cell{std::move(value), CellType::String, 0.0};
  }
  /// `display` is the text shown for the number (e.g. "3.50").
  [[nodiscard]] static Cell numeric(double value, std::string display) {
    return Cell{std::move(display), CellType::Number, value};
  }
  [[nodiscard]] static Cell boolean(bool value) {
    return Cell{value ? "TRUE" : "FALSE", CellType::Bool, value ? 1.0 : 0.0};
  }
  /// `iso8601` is the display form, e.g. "2024-05-01T12:30:00Z".
  [[nodiscard]] static Cell datetime(double unix_seconds, std::string iso8601) {
    return Cell{std::move(iso8601), CellType::DateTime, unix_seconds};
  }
  [[nodiscard]] bool empty() const noexcept { return type == CellType::Empty; }
};

/// One worksheet. Rows may have different lengths (a ragged grid); missing
/// trailing cells are equivalent to Empty cells.
struct Sheet {
  /// Sheet name; may be empty for single-table formats such as CSV.
  std::string name;
  /// Row-major cells. When the source distinguishes a header, it is simply
  /// row 0 and `has_header` is set.
  std::vector<std::vector<Cell>> rows;
  /// True when row 0 is known to hold column titles.
  bool has_header = false;

  /// Width of the widest row.
  [[nodiscard]] std::size_t column_count() const noexcept {
    std::size_t columns = 0;
    for (const auto& row : rows) columns = std::max(columns, row.size());
    return columns;
  }
};

/// A workbook. Single-table formats produce exactly one sheet.
struct Table {
  std::vector<Sheet> sheets;
};

}  // namespace cvt::ir
