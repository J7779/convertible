// Convertible core - table family: helpers shared by the spreadsheet formats
// (XLSX, XLSB, XLS, ODS, SpreadsheetML, Gnumeric): Excel number formats,
// formula notation conversions and OOXML string escapes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "families/table/common.hpp"
#include "ir/table.hpp"

namespace cvt::table {

// --------------------------------------------------------------------------- number formats

/// The format code of a built-in Excel number format id ("0.00" for 2,
/// "mm-dd-yy" for 14...), or empty for General and unknown ids.
[[nodiscard]] std::string_view builtin_number_format(std::uint32_t id) noexcept;
/// The built-in id whose code is exactly `code`, if any.
[[nodiscard]] std::optional<std::uint32_t> builtin_number_format_id(std::string_view code) noexcept;

enum class FormatKind { General, Number, Date, Time, DateTime, Text };
/// What a format code displays: dates (y, d, m without h/s), times (h, s,
/// AM/PM), both, text (@) or numbers. Quoted literals, escapes, colours and
/// conditions are ignored; elapsed-time codes ([h]:mm) count as numbers.
[[nodiscard]] FormatKind classify_number_format(std::string_view code) noexcept;
[[nodiscard]] constexpr bool is_date_kind(FormatKind kind) noexcept {
  return kind == FormatKind::Date || kind == FormatKind::Time || kind == FormatKind::DateTime;
}
[[nodiscard]] DateStyle date_style_for(FormatKind kind) noexcept;

/// The cell for a number stored in a spreadsheet with the given format:
/// a DateTime when the format shows a date or time, else a Number.
[[nodiscard]] ir::Cell spreadsheet_number(double value, FormatKind kind, bool date1904);

/// Format codes used when writing DateTime cells that carry no format.
inline constexpr std::string_view iso_date_format = "yyyy-mm-dd";
inline constexpr std::string_view iso_datetime_format = "yyyy-mm-dd hh:mm:ss";
inline constexpr std::string_view iso_time_format = "hh:mm:ss";
/// The format code a writer should give a cell: its own from the sheet,
/// otherwise a date/time code for DateTime cells, otherwise empty (General).
[[nodiscard]] std::string cell_number_format(const ir::Sheet& sheet, std::size_t row, std::size_t column);

// --------------------------------------------------------------------------- formulas

/// Move the relative references of an A1-notation formula by (rows, columns),
/// as Excel does when it copies a shared formula. References that would
/// leave the sheet become #REF!.
[[nodiscard]] std::string shift_formula(std::string_view formula, std::int64_t rows, std::int64_t columns);
/// OpenFormula ("of:=SUM([.A1:.B2];[$Sheet2.C3])") -> Excel ("SUM(A1:B2,Sheet2!C3)").
[[nodiscard]] std::string odf_to_excel_formula(std::string_view formula);
/// Excel -> OpenFormula, including the "of:=" prefix.
[[nodiscard]] std::string excel_to_odf_formula(std::string_view formula);
/// R1C1 ("R[-1]C+RC[2]") <-> A1 ("B1+D2") relative to the formula's cell.
[[nodiscard]] std::string r1c1_to_a1_formula(std::string_view formula, std::uint32_t row, std::uint32_t column);
[[nodiscard]] std::string a1_to_r1c1_formula(std::string_view formula, std::uint32_t row, std::uint32_t column);

// --------------------------------------------------------------------------- OOXML strings

/// Undo the "_xHHHH_" escapes OOXML uses for characters XML cannot carry.
[[nodiscard]] std::string ooxml_unescape(std::string_view text);
/// Append text escaped for an OOXML element: XML escapes, plus "_xHHHH_"
/// for control characters and for literal "_xHHHH_" sequences.
void ooxml_escape_append(std::string& out, std::string_view text);
/// Excel error literals (#N/A, #DIV/0!, ...) by BIFF error code.
[[nodiscard]] std::string_view excel_error_text(std::uint8_t code) noexcept;

}  // namespace cvt::table
