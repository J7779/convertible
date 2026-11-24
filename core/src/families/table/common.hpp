// Convertible core - table family: shared helpers for every tabular format.
//
// Cell construction and display text, Excel/Unix date arithmetic, locale-free
// type inference, A1 references, sheet selection for single-table writers,
// a grid builder that enforces memory limits, and the option specs that
// several codecs share.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/table.hpp"

namespace cvt::table {

// --------------------------------------------------------------------------- numbers

/// Display text of a number: integers without a fraction ("42"), others in
/// the shortest form that reads back exactly ("0.1", "1e+30").
[[nodiscard]] std::string format_number(double value);
/// A Number cell whose display text is `format_number(value)`.
[[nodiscard]] ir::Cell number_cell(double value);
/// An integer read from a binary format; the display text is exact even
/// beyond 2^53 although the numeric value is rounded to a double.
[[nodiscard]] ir::Cell integer_cell(std::int64_t value);

// --------------------------------------------------------------------------- dates

/// Seconds between 1899-12-30 (day 0 of Excel's 1900 date system, which
/// absorbs the fictitious 1900-02-29) and 1970-01-01.
inline constexpr double excel_1900_epoch_unix = -2209161600.0;
/// Seconds between 1904-01-01 (day 0 of the 1904 date system) and 1970-01-01.
inline constexpr double excel_1904_epoch_unix = -2082844800.0;

/// Excel serial day number -> Unix seconds. Serials below 61 in the 1900
/// system are shifted by one day to undo the 1900 leap-year bug.
[[nodiscard]] double excel_serial_to_unix(double serial, bool date1904 = false) noexcept;
[[nodiscard]] double unix_to_excel_serial(double unix_seconds, bool date1904 = false) noexcept;

/// Days since 1970-01-01 of a proleptic Gregorian date, and back.
[[nodiscard]] std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept;
struct CivilDate {
  std::int64_t year = 1970;
  unsigned month = 1;
  unsigned day = 1;
};
[[nodiscard]] CivilDate civil_from_days(std::int64_t days) noexcept;

/// How a DateTime cell is displayed.
enum class DateStyle { Auto, Date, Time, DateTime };
/// ISO 8601 display text: "2024-05-01" (Date, or Auto at midnight),
/// "12:30:00" (Time), "2024-05-01T12:30:00" (DateTime / Auto). Fractions of a
/// second are written to the millisecond when present.
[[nodiscard]] std::string format_datetime(double unix_seconds, DateStyle style = DateStyle::Auto);
[[nodiscard]] ir::Cell datetime_cell(double unix_seconds, DateStyle style = DateStyle::Auto);
/// Unix seconds of an ISO-8601-like text: "2024-05-01", "2024-05-01T12:30",
/// "2024-05-01 12:30:00.250", "...Z", "...+02:00", "12:30:00" (a time of
/// day on 1899-12-30, as spreadsheets store it). nullopt otherwise.
struct ParsedDate {
  double unix_seconds = 0.0;
  DateStyle style = DateStyle::Date;
};
[[nodiscard]] std::optional<ParsedDate> parse_iso_datetime(std::string_view text) noexcept;
/// The time of day of a time-only value is kept on this day.
[[nodiscard]] constexpr bool is_time_only(double unix_seconds) noexcept {
  return unix_seconds >= excel_1900_epoch_unix && unix_seconds < excel_1900_epoch_unix + 86400.0;
}

// --------------------------------------------------------------------------- type inference

/// Which day/month order slashed dates ("01/02/2024") follow.
enum class DateOrder { IsoOnly, DayFirst, MonthFirst };

struct InferOptions {
  bool numbers = true;
  bool booleans = true;
  bool dates = true;
  char decimal_separator = '.';
  DateOrder date_order = DateOrder::IsoOnly;
};

/// Turn a field of a text format into a typed cell: numbers (keeping the
/// text as written), TRUE/FALSE, ISO dates; everything else, and numbers
/// that would not survive a double (leading zeros, more than 15 significant
/// digits), stay strings. Empty text gives an Empty cell.
[[nodiscard]] ir::Cell infer_cell(std::string text, const InferOptions& options);
/// The plain number parse used by inference (no thousands separators).
[[nodiscard]] std::optional<double> parse_number(std::string_view text, char decimal_separator = '.') noexcept;

/// Read the shared inference options ("infer_types", "decimal_separator",
/// "date_order") from a reader's options.
[[nodiscard]] InferOptions infer_options_from(const Options& options);
/// The reader option specs behind `infer_options_from`.
[[nodiscard]] std::vector<OptionSpec> infer_option_specs();

/// Whether row 0 looks like column titles: distinct non-empty texts above
/// columns holding typed values (Python's csv.Sniffer idea). Tables made
/// only of text are assumed to have a header.
[[nodiscard]] bool guess_header(const ir::Sheet& sheet);
/// Set `has_header` from guess_header() on every sheet that does not have it.
void guess_headers(ir::Table& table);

// --------------------------------------------------------------------------- references

/// 0 -> "A", 25 -> "Z", 26 -> "AA" ...
[[nodiscard]] std::string column_name(std::uint32_t column);
/// "B3" -> (2, 1); "$B$3" too. nullopt when malformed.
[[nodiscard]] std::optional<ir::CellPosition> parse_a1(std::string_view reference) noexcept;
/// "A1" style reference of (row, column).
[[nodiscard]] std::string a1(std::uint32_t row, std::uint32_t column);
/// "A1:C4" (or a single cell) -> range.
[[nodiscard]] std::optional<ir::CellRange> parse_a1_range(std::string_view reference) noexcept;

// --------------------------------------------------------------------------- building grids

/// Accumulates cells into a sheet while enforcing a memory budget derived
/// from the limits, so that sparse coordinates in a hostile file (a single
/// cell at row 1,000,000 of column 16,000) cannot make us allocate huge
/// empty grids. One budget may be shared by all sheets of a workbook.
class CellBudget {
 public:
  explicit CellBudget(const Limits& limits) noexcept;
  /// Account for `cells` more cells (including the empty ones created to
  /// reach a coordinate); Status::Limit when the budget is exhausted.
  [[nodiscard]] Result<void> take(std::uint64_t cells);
  [[nodiscard]] std::uint64_t used() const noexcept { return used_; }

 private:
  std::uint64_t max_ = 0;
  std::uint64_t used_ = 0;
};

/// Put `cell` at (row, column), growing the grid as needed under `budget`.
[[nodiscard]] Result<void> put_cell(ir::Sheet& sheet, std::uint64_t row, std::uint64_t column, ir::Cell cell,
                                    CellBudget& budget);
/// Drop trailing empty cells of every row and trailing empty rows.
void trim_sheet(ir::Sheet& sheet);

// --------------------------------------------------------------------------- choosing a sheet

/// `sheet` writer option: index (0-based) or name; empty = first visible sheet.
[[nodiscard]] OptionSpec sheet_option();
/// The sheet a single-table writer should write, per the `sheet` option.
/// Warns when other sheets are left out. Status::Encode for an empty
/// workbook (a blank sheet is returned instead when `allow_empty`), and
/// InvalidArgument for a sheet that does not exist.
[[nodiscard]] Result<const ir::Sheet*> select_sheet(const ir::Table& table, const Context& ctx,
                                                    bool allow_empty = true);

/// Shared empty sheet returned for empty workbooks.
[[nodiscard]] const ir::Sheet& empty_sheet();

// --------------------------------------------------------------------------- text helpers

/// Number of terminal columns a UTF-8 string occupies (wide East Asian
/// characters count two, combining marks zero).
[[nodiscard]] std::size_t display_width(std::string_view text) noexcept;
/// Decode a text file per a `source_encoding` option value ("auto" = BOM,
/// UTF-8, UTF-16 heuristics, then Windows-1252). Warns about replacements.
[[nodiscard]] Result<std::string> decode_source_text(ByteView input, std::string_view encoding, const Context& ctx,
                                                     std::string* detected = nullptr);
/// `source_encoding` reader option.
[[nodiscard]] OptionSpec source_encoding_option();
/// Encode UTF-8 output per an `encoding` writer option value.
[[nodiscard]] Result<Bytes> encode_output_text(std::string_view utf8, std::string_view encoding, bool bom,
                                               const Context& ctx);
/// Unique, non-empty sheet names: empty names become "Sheet<n>", duplicates
/// get " (2)" suffixes; `max_length` truncates (in code points) first, and
/// `forbidden` characters are replaced by '_'.
[[nodiscard]] std::vector<std::string> unique_sheet_names(const ir::Table& table, std::size_t max_length,
                                                          std::string_view forbidden);
/// Truncate UTF-8 text to at most `max_code_points` code points.
[[nodiscard]] std::string truncate_utf8(std::string_view text, std::size_t max_code_points);

}  // namespace cvt::table
