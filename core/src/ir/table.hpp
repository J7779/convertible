// Convertible core - Table IR: a workbook of sheets of cells.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
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

/// A rectangular block of cells, 0-based and inclusive at both ends.
struct CellRange {
  std::uint32_t first_row = 0;
  std::uint32_t first_column = 0;
  std::uint32_t last_row = 0;
  std::uint32_t last_column = 0;

  [[nodiscard]] bool contains(std::size_t row, std::size_t column) const noexcept {
    return row >= first_row && row <= last_row && column >= first_column && column <= last_column;
  }
  friend bool operator==(const CellRange&, const CellRange&) = default;
};

/// Position of a cell: (row, column), 0-based. Key of the sparse per-cell
/// annotations of a Sheet.
using CellPosition = std::pair<std::uint32_t, std::uint32_t>;

/// Whether and how a sheet is shown by spreadsheet applications.
enum class SheetVisibility {
  Visible,
  Hidden,      ///< hidden, the user can unhide it
  VeryHidden,  ///< hidden, only a macro can unhide it (Excel)
};

/// One worksheet. Rows may have different lengths (a ragged grid); missing
/// trailing cells are equivalent to Empty cells.
///
/// Besides the cells, a sheet may carry presentation details that
/// spreadsheet formats can keep. Every one of them is optional and has a
/// neutral default, so text-only readers and writers can ignore them.
struct Sheet {
  /// Sheet name; may be empty for single-table formats such as CSV.
  std::string name;
  /// Row-major cells. When the source distinguishes a header, it is simply
  /// row 0 and `has_header` is set.
  std::vector<std::vector<Cell>> rows;
  /// True when row 0 is known to hold column titles.
  bool has_header = false;

  /// Hidden sheets are kept, but writers of single-table formats skip them
  /// when choosing the default sheet.
  SheetVisibility visibility = SheetVisibility::Visible;
  /// Merged blocks. The value lives in the top-left cell; the other cells of
  /// the block are Empty.
  std::vector<CellRange> merged{};
  /// Column widths in characters of the default font (Excel's unit), by
  /// column index; 0 or a missing entry means "default width".
  std::vector<double> column_widths{};
  /// Number of rows / columns frozen at the top / left (0 = none).
  std::uint32_t frozen_rows = 0;
  std::uint32_t frozen_columns = 0;
  /// Formulas as written in the source, without the leading '=' and in the
  /// A1 notation of Excel ("SUM(A1:A3)"); the cell itself holds the cached
  /// result. Sparse: only cells that have one.
  std::map<CellPosition, std::string> formulas{};
  /// Number formats in Excel's format-code syntax ("0.00", "yyyy-mm-dd",
  /// "0%"). Sparse: only cells whose format is not "General".
  std::map<CellPosition, std::string> number_formats{};

  /// Width of the widest row.
  [[nodiscard]] std::size_t column_count() const noexcept {
    std::size_t columns = 0;
    for (const auto& row : rows) columns = std::max(columns, row.size());
    return columns;
  }
  [[nodiscard]] bool hidden() const noexcept { return visibility != SheetVisibility::Visible; }
  /// The cell at (row, column), or an Empty cell outside the grid.
  [[nodiscard]] const Cell& at(std::size_t row, std::size_t column) const noexcept {
    static const Cell empty_cell{};
    if (row >= rows.size() || column >= rows[row].size()) return empty_cell;
    return rows[row][column];
  }
  /// The formula / number format of a cell, or an empty string.
  [[nodiscard]] std::string_view formula_at(std::size_t row, std::size_t column) const {
    return lookup(formulas, row, column);
  }
  [[nodiscard]] std::string_view number_format_at(std::size_t row, std::size_t column) const {
    return lookup(number_formats, row, column);
  }

 private:
  [[nodiscard]] static std::string_view lookup(const std::map<CellPosition, std::string>& map, std::size_t row,
                                               std::size_t column) {
    if (map.empty() || row > UINT32_MAX || column > UINT32_MAX) return {};
    const auto it = map.find({static_cast<std::uint32_t>(row), static_cast<std::uint32_t>(column)});
    return it == map.end() ? std::string_view() : std::string_view(it->second);
  }
};

/// A workbook. Single-table formats produce exactly one sheet.
struct Table {
  std::vector<Sheet> sheets;
};

}  // namespace cvt::ir
