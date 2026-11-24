// Convertible core - table family: shared helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/table/common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <string>
#include <utility>

#include "families/table/codepages.hpp"
#include "util/text.hpp"

namespace cvt::table {

// --------------------------------------------------------------------------- numbers

std::string format_number(double value) {
  if (std::isfinite(value) && value == std::trunc(value) && std::fabs(value) < 1e15) {
    if (value == 0.0) return "0";  // also -0
    return std::format("{}", static_cast<std::int64_t>(value));
  }
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
  return util::format_double(value);
}

ir::Cell number_cell(double value) { return ir::Cell::numeric(value, format_number(value)); }

ir::Cell integer_cell(std::int64_t value) {
  return ir::Cell::numeric(static_cast<double>(value), std::format("{}", value));
}

// --------------------------------------------------------------------------- dates

std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

CivilDate civil_from_days(std::int64_t days) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return CivilDate{y + (m <= 2 ? 1 : 0), m, d};
}

double excel_serial_to_unix(double serial, bool date1904) noexcept {
  // Serials 1..59 (1900-01-01 .. 1900-02-28) are one day off because Excel
  // believes 1900 was a leap year. Day 0 holds pure times of day.
  if (!date1904 && serial >= 1.0 && serial < 60.0) serial += 1.0;
  const double seconds = (date1904 ? excel_1904_epoch_unix : excel_1900_epoch_unix) + serial * 86400.0;
  // A serial carries about 1e-5 s of floating-point noise; Excel shows at
  // most milliseconds, so round to them (12:29:59.9999998 is 12:30:00).
  return std::isfinite(seconds) ? std::round(seconds * 1000.0) / 1000.0 : seconds;
}

double unix_to_excel_serial(double unix_seconds, bool date1904) noexcept {
  // A time of day is a fraction of a day in either date system.
  if (is_time_only(unix_seconds)) return (unix_seconds - excel_1900_epoch_unix) / 86400.0;
  if (date1904) return (unix_seconds - excel_1904_epoch_unix) / 86400.0;
  double serial = (unix_seconds - excel_1900_epoch_unix) / 86400.0;
  if (serial >= 2.0 && serial < 61.0) serial -= 1.0;
  return serial;
}

std::string format_datetime(double unix_seconds, DateStyle style) {
  if (!std::isfinite(unix_seconds) || std::fabs(unix_seconds) > 1e14) return format_number(unix_seconds);
  const auto total_ms = static_cast<std::int64_t>(std::llround(unix_seconds * 1000.0));
  constexpr std::int64_t ms_per_day = 86'400'000;
  std::int64_t days = total_ms / ms_per_day;
  std::int64_t ms = total_ms % ms_per_day;
  if (ms < 0) {
    ms += ms_per_day;
    --days;
  }
  const CivilDate date = civil_from_days(days);
  if (style == DateStyle::Auto) {
    if (ms == 0) {
      style = DateStyle::Date;
    } else if (is_time_only(unix_seconds)) {
      style = DateStyle::Time;
    } else {
      style = DateStyle::DateTime;
    }
  }
  const std::string date_text = date.year >= 0 && date.year <= 9999
                                    ? std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day)
                                    : std::format("{:+}-{:02}-{:02}", date.year, date.month, date.day);
  const auto hours = ms / 3'600'000;
  const auto minutes = ms / 60'000 % 60;
  const auto seconds = ms / 1000 % 60;
  const auto millis = ms % 1000;
  std::string time_text = std::format("{:02}:{:02}:{:02}", hours, minutes, seconds);
  if (millis != 0) time_text += std::format(".{:03}", millis);
  switch (style) {
    case DateStyle::Date: return date_text;
    case DateStyle::Time: return time_text;
    case DateStyle::DateTime:
    case DateStyle::Auto: break;
  }
  return date_text + "T" + time_text;
}

ir::Cell datetime_cell(double unix_seconds, DateStyle style) {
  return ir::Cell::datetime(unix_seconds, format_datetime(unix_seconds, style));
}

namespace {

bool digits(std::string_view text, std::size_t at, std::size_t count, int& value) {
  if (at + count > text.size()) return false;
  value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[at + i];
    if (!util::is_ascii_digit(c)) return false;
    value = value * 10 + (c - '0');
  }
  return true;
}

unsigned days_in_month(std::int64_t year, unsigned month) noexcept {
  static constexpr unsigned lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
  }
  return lengths[month - 1];
}

// "H:MM" or "HH:MM", then optional ":SS" and fraction, at `at`. Returns the
// end position, or 0 when there is no valid time there.
std::size_t parse_time(std::string_view text, std::size_t at, double& seconds_of_day) {
  int h = 0;
  int m = 0;
  int s = 0;
  std::size_t pos = 0;
  if (digits(text, at, 2, h) && at + 2 < text.size() && text[at + 2] == ':' && digits(text, at + 3, 2, m)) {
    pos = at + 5;
  } else if (digits(text, at, 1, h) && at + 1 < text.size() && text[at + 1] == ':' && digits(text, at + 2, 2, m)) {
    pos = at + 4;
  } else {
    return 0;
  }
  double fraction = 0.0;
  if (pos < text.size() && text[pos] == ':') {
    if (!digits(text, pos + 1, 2, s)) return 0;
    pos += 3;
    if (pos < text.size() && (text[pos] == '.' || text[pos] == ',')) {
      std::size_t end = pos + 1;
      double scale = 0.1;
      while (end < text.size() && util::is_ascii_digit(text[end]) && end - pos <= 9) {
        fraction += (text[end] - '0') * scale;
        scale /= 10;
        ++end;
      }
      if (end == pos + 1) return 0;
      pos = end;
    }
  }
  if (h > 24 || m > 59 || s > 60 || (h == 24 && (m != 0 || s != 0))) return 0;
  seconds_of_day = h * 3600.0 + m * 60.0 + s + fraction;
  return pos;
}

}  // namespace

std::optional<ParsedDate> parse_iso_datetime(std::string_view text) noexcept {
  text = util::trim(text);
  if (text.size() < 4) return std::nullopt;
  // Time of day only.
  if (text.size() <= 15 && (text[1] == ':' || text[2] == ':')) {
    double seconds = 0.0;
    const std::size_t end = parse_time(text, 0, seconds);
    if (end == 0 || end != text.size() || seconds >= 86400.0) return std::nullopt;
    return ParsedDate{excel_1900_epoch_unix + seconds, DateStyle::Time};
  }
  int year = 0;
  int month = 0;
  int day = 0;
  if (text.size() < 10 || !digits(text, 0, 4, year) || text[4] != '-' || !digits(text, 5, 2, month) ||
      text[7] != '-' || !digits(text, 8, 2, day)) {
    return std::nullopt;
  }
  if (month < 1 || month > 12 || day < 1 || static_cast<unsigned>(day) > days_in_month(year, static_cast<unsigned>(month))) {
    return std::nullopt;
  }
  double unix_seconds = static_cast<double>(days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day))) * 86400.0;
  if (text.size() == 10) return ParsedDate{unix_seconds, DateStyle::Date};
  if (text[10] != 'T' && text[10] != 't' && text[10] != ' ') return std::nullopt;
  double seconds = 0.0;
  std::size_t pos = parse_time(text, 11, seconds);
  if (pos == 0) return std::nullopt;
  unix_seconds += seconds;
  if (pos < text.size()) {
    const char z = text[pos];
    if ((z == 'Z' || z == 'z') && pos + 1 == text.size()) {
      pos += 1;
    } else if (z == '+' || z == '-') {
      int oh = 0;
      int om = 0;
      std::string_view rest = text.substr(pos + 1);
      if (rest.size() == 5 && rest[2] == ':' && digits(rest, 0, 2, oh) && digits(rest, 3, 2, om)) {
      } else if (rest.size() == 4 && digits(rest, 0, 2, oh) && digits(rest, 2, 2, om)) {
      } else if (rest.size() == 2 && digits(rest, 0, 2, oh)) {
      } else {
        return std::nullopt;
      }
      if (oh > 23 || om > 59) return std::nullopt;
      const double offset = oh * 3600.0 + om * 60.0;
      unix_seconds += z == '+' ? -offset : offset;
      pos = text.size();
    } else {
      return std::nullopt;
    }
  }
  if (pos != text.size()) return std::nullopt;
  return ParsedDate{unix_seconds, DateStyle::DateTime};
}

// --------------------------------------------------------------------------- type inference

std::optional<double> parse_number(std::string_view text, char decimal_separator) noexcept {
  if (text.empty() || text.size() > 64) return std::nullopt;
  char buffer[72];
  std::size_t n = 0;
  std::size_t i = 0;
  if (text[i] == '+' || text[i] == '-') buffer[n++] = text[i++];
  std::size_t int_digits = 0;
  std::size_t frac_digits = 0;
  while (i < text.size() && util::is_ascii_digit(text[i])) {
    buffer[n++] = text[i++];
    ++int_digits;
  }
  if (i < text.size() && text[i] == decimal_separator) {
    buffer[n++] = '.';
    ++i;
    while (i < text.size() && util::is_ascii_digit(text[i])) {
      buffer[n++] = text[i++];
      ++frac_digits;
    }
  }
  if (int_digits + frac_digits == 0) return std::nullopt;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    buffer[n++] = 'e';
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) buffer[n++] = text[i++];
    std::size_t exp_digits = 0;
    while (i < text.size() && util::is_ascii_digit(text[i])) {
      buffer[n++] = text[i++];
      ++exp_digits;
    }
    if (exp_digits == 0) return std::nullopt;
  }
  if (i != text.size()) return std::nullopt;
  const auto value = util::parse_double(std::string_view(buffer, n));
  if (!value || !std::isfinite(*value)) return std::nullopt;
  return value;
}

namespace {

// Numbers whose text would change if stored as a double: leading zeros
// ("007", "0012.5") and more than 15 significant digits.
bool number_text_is_safe(std::string_view text, char decimal_separator) {
  std::size_t i = 0;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
  if (i + 1 < text.size() && text[i] == '0' && util::is_ascii_digit(text[i + 1])) return false;
  std::size_t significant = 0;
  bool leading = true;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c == 'e' || c == 'E') break;
    if (c == decimal_separator) continue;
    if (!util::is_ascii_digit(c)) continue;
    if (leading && c == '0') continue;
    leading = false;
    ++significant;
  }
  return significant <= 15;
}

// D/M/Y or M/D/Y with '/', '.' or '-' separators and a four-digit year.
std::optional<double> parse_ordered_date(std::string_view text, DateOrder order) {
  if (order == DateOrder::IsoOnly) return std::nullopt;
  int parts[3] = {0, 0, 0};
  std::size_t lengths[3] = {0, 0, 0};
  std::size_t part = 0;
  char separator = 0;
  for (const char c : text) {
    if (util::is_ascii_digit(c)) {
      if (lengths[part] >= 4) return std::nullopt;
      parts[part] = parts[part] * 10 + (c - '0');
      ++lengths[part];
    } else if ((c == '/' || c == '.' || c == '-') && part < 2 && lengths[part] > 0 && (separator == 0 || separator == c)) {
      separator = c;
      ++part;
    } else {
      return std::nullopt;
    }
  }
  if (part != 2 || lengths[0] > 2 || lengths[1] > 2 || lengths[2] != 4) return std::nullopt;
  const int day = order == DateOrder::DayFirst ? parts[0] : parts[1];
  const int month = order == DateOrder::DayFirst ? parts[1] : parts[0];
  const int year = parts[2];
  if (month < 1 || month > 12 || day < 1 || static_cast<unsigned>(day) > days_in_month(year, static_cast<unsigned>(month))) {
    return std::nullopt;
  }
  return static_cast<double>(days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day))) * 86400.0;
}

}  // namespace

ir::Cell infer_cell(std::string text, const InferOptions& options) {
  if (text.empty()) return ir::Cell{};
  const std::string_view trimmed = util::trim(text);
  if (trimmed.empty()) return ir::Cell::string(std::move(text));
  if (options.booleans && (util::iequals(trimmed, "true") || util::iequals(trimmed, "false"))) {
    const bool value = util::iequals(trimmed, "true");
    ir::Cell cell = ir::Cell::boolean(value);
    cell.text = std::move(text);
    return cell;
  }
  const char first = trimmed.front();
  if (options.numbers && (util::is_ascii_digit(first) || first == '-' || first == '+' || first == '.' ||
                          first == options.decimal_separator)) {
    if (const auto value = parse_number(trimmed, options.decimal_separator);
        value && number_text_is_safe(trimmed, options.decimal_separator)) {
      return ir::Cell::numeric(*value, std::move(text));
    }
  }
  if (options.dates && util::is_ascii_digit(first)) {
    if (const auto date = parse_iso_datetime(trimmed)) return ir::Cell::datetime(date->unix_seconds, std::move(text));
    if (const auto date = parse_ordered_date(trimmed, options.date_order)) {
      return datetime_cell(*date, DateStyle::Date);
    }
  }
  return ir::Cell::string(std::move(text));
}

InferOptions infer_options_from(const Options& options) {
  InferOptions out;
  const bool infer = options.get_bool("infer_types", true);
  out.numbers = infer;
  out.booleans = infer;
  out.dates = infer;
  out.decimal_separator = options.get_enum("decimal_separator", "dot", {"dot", "comma"}) == "comma" ? ',' : '.';
  const std::string order = options.get_enum("date_order", "iso", {"iso", "dmy", "mdy"});
  out.date_order = order == "dmy" ? DateOrder::DayFirst : order == "mdy" ? DateOrder::MonthFirst : DateOrder::IsoOnly;
  return out;
}

std::vector<OptionSpec> infer_option_specs() {
  return {
      OptionSpec::boolean("infer_types", "Detect types", true,
                          "Turn numbers, TRUE/FALSE and ISO dates into typed cells instead of keeping all text."),
      OptionSpec::choice("decimal_separator", "Decimal separator", "dot", {"dot", "comma"},
                         "Character that separates the fraction in numbers of the source (1.5 or 1,5).")
          .advanced(),
      OptionSpec::choice("date_order", "Date order", "iso", {"iso", "dmy", "mdy"},
                         "Besides ISO dates, also recognise 31/12/2024 (dmy) or 12/31/2024 (mdy).")
          .advanced(),
  };
}

namespace {

bool is_typed(const ir::Cell& cell) {
  return cell.type == ir::CellType::Number || cell.type == ir::CellType::Bool || cell.type == ir::CellType::DateTime;
}

// Python's csv.Sniffer.has_header idea, simplified: a first row of distinct,
// non-empty texts above columns that hold typed values (or texts of a
// different shape) is a header.
}  // namespace

bool guess_header(const ir::Sheet& sheet) {
  if (sheet.rows.size() < 2 || sheet.rows[0].empty()) return false;
  const auto& first = sheet.rows[0];
  std::set<std::string> seen;
  for (const ir::Cell& cell : first) {
    if (cell.type != ir::CellType::String || util::trim(cell.text).empty()) return false;
    if (!seen.insert(cell.text).second) return false;
  }
  int votes = 0;
  const std::size_t sample_rows = std::min<std::size_t>(sheet.rows.size(), 50);
  for (std::size_t c = 0; c < first.size(); ++c) {
    std::size_t typed = 0;
    std::size_t texts = 0;
    std::size_t same_length = 0;
    for (std::size_t r = 1; r < sample_rows; ++r) {
      const ir::Cell& cell = sheet.at(r, c);
      if (cell.empty()) continue;
      if (is_typed(cell)) {
        ++typed;
      } else {
        ++texts;
        if (util::utf8_length(cell.text) == util::utf8_length(first[c].text)) ++same_length;
      }
    }
    if (typed > 0 && typed >= texts) {
      ++votes;
    } else if (texts > 0) {
      votes += same_length == texts ? -1 : 0;
    }
  }
  // All-text tables without evidence either way: assume a header, as
  // nearly every CSV file in the wild has one.
  return votes >= 0;
}


void guess_headers(ir::Table& table) {
  for (ir::Sheet& sheet : table.sheets) {
    if (!sheet.has_header) sheet.has_header = guess_header(sheet);
  }
}

// --------------------------------------------------------------------------- references

std::string column_name(std::uint32_t column) {
  std::string out;
  std::uint64_t n = std::uint64_t{column} + 1;
  while (n > 0) {
    const auto rem = static_cast<char>((n - 1) % 26);
    out.insert(out.begin(), static_cast<char>('A' + rem));
    n = (n - 1) / 26;
  }
  return out;
}

std::string a1(std::uint32_t row, std::uint32_t column) {
  return column_name(column) + std::to_string(std::uint64_t{row} + 1);
}

std::optional<ir::CellPosition> parse_a1(std::string_view reference) noexcept {
  std::size_t i = 0;
  if (i < reference.size() && reference[i] == '$') ++i;
  std::uint64_t column = 0;
  std::size_t letters = 0;
  while (i < reference.size() && util::is_ascii_alpha(reference[i])) {
    column = column * 26 + static_cast<std::uint64_t>(util::ascii_upper(reference[i]) - 'A' + 1);
    if (++letters > 6) return std::nullopt;
    ++i;
  }
  if (letters == 0) return std::nullopt;
  if (i < reference.size() && reference[i] == '$') ++i;
  std::uint64_t row = 0;
  std::size_t numbers = 0;
  while (i < reference.size() && util::is_ascii_digit(reference[i])) {
    row = row * 10 + static_cast<std::uint64_t>(reference[i] - '0');
    if (++numbers > 10) return std::nullopt;
    ++i;
  }
  if (numbers == 0 || i != reference.size() || row == 0 || row > UINT32_MAX || column > UINT32_MAX) {
    return std::nullopt;
  }
  return ir::CellPosition{static_cast<std::uint32_t>(row - 1), static_cast<std::uint32_t>(column - 1)};
}

std::optional<ir::CellRange> parse_a1_range(std::string_view reference) noexcept {
  const std::size_t colon = reference.find(':');
  const auto first = parse_a1(reference.substr(0, colon));
  if (!first) return std::nullopt;
  auto last = first;
  if (colon != std::string_view::npos) last = parse_a1(reference.substr(colon + 1));
  if (!last) return std::nullopt;
  ir::CellRange range;
  range.first_row = std::min(first->first, last->first);
  range.last_row = std::max(first->first, last->first);
  range.first_column = std::min(first->second, last->second);
  range.last_column = std::max(first->second, last->second);
  return range;
}

// --------------------------------------------------------------------------- building grids

CellBudget::CellBudget(const Limits& limits) noexcept
    : max_(std::max<std::uint64_t>(limits.max_decompressed_bytes / 64, 1024)) {}

Result<void> CellBudget::take(std::uint64_t cells) {
  if (cells > max_ - used_) {
    return fail(Status::Limit, "the table needs more than {} cells, which exceeds the memory limit", max_);
  }
  used_ += cells;
  return {};
}

Result<void> put_cell(ir::Sheet& sheet, std::uint64_t row, std::uint64_t column, ir::Cell cell, CellBudget& budget) {
  if (row >= UINT32_MAX || column >= UINT32_MAX) return fail(Status::Limit, "cell coordinates are out of range");
  if (row >= sheet.rows.size()) {
    CVT_TRY(budget.take(row + 1 - sheet.rows.size()));
    sheet.rows.resize(static_cast<std::size_t>(row + 1));
  }
  auto& cells = sheet.rows[static_cast<std::size_t>(row)];
  if (column >= cells.size()) {
    CVT_TRY(budget.take(column + 1 - cells.size()));
    cells.resize(static_cast<std::size_t>(column + 1));
  }
  cells[static_cast<std::size_t>(column)] = std::move(cell);
  return {};
}

void trim_sheet(ir::Sheet& sheet) {
  for (auto& row : sheet.rows) {
    while (!row.empty() && row.back().empty()) row.pop_back();
  }
  while (!sheet.rows.empty() && sheet.rows.back().empty()) sheet.rows.pop_back();
}

// --------------------------------------------------------------------------- choosing a sheet

OptionSpec sheet_option() {
  return OptionSpec::text("sheet", "Sheet", "",
                          "Sheet to convert when the target holds a single table: its name or 0-based index. "
                          "Empty means the first visible sheet.");
}

const ir::Sheet& empty_sheet() {
  static const ir::Sheet sheet{};
  return sheet;
}

Result<const ir::Sheet*> select_sheet(const ir::Table& table, const Context& ctx, bool allow_empty) {
  if (table.sheets.empty()) {
    if (allow_empty) return &empty_sheet();
    return fail(Status::Encode, "the workbook has no sheets");
  }
  const std::string wanted(util::trim(ctx.options.get_string("sheet", "")));
  const ir::Sheet* chosen = nullptr;
  if (wanted.empty()) {
    for (const ir::Sheet& sheet : table.sheets) {
      if (!sheet.hidden()) {
        chosen = &sheet;
        break;
      }
    }
    if (chosen == nullptr) chosen = &table.sheets.front();
  } else {
    for (const ir::Sheet& sheet : table.sheets) {
      if (sheet.name == wanted) {
        chosen = &sheet;
        break;
      }
    }
    if (chosen == nullptr) {
      for (const ir::Sheet& sheet : table.sheets) {
        if (util::iequals(sheet.name, wanted)) {
          chosen = &sheet;
          break;
        }
      }
    }
    if (chosen == nullptr) {
      if (const auto index = util::parse_int(wanted); index && *index >= 0 &&
                                                       static_cast<std::uint64_t>(*index) < table.sheets.size()) {
        chosen = &table.sheets[static_cast<std::size_t>(*index)];
      }
    }
    if (chosen == nullptr) {
      return fail(Status::InvalidArgument, "the workbook has no sheet named or numbered \"{}\" (it has {} sheet(s))",
                  wanted, table.sheets.size());
    }
  }
  if (table.sheets.size() > 1) {
    ctx.warn(std::format("The workbook has {} sheets; only \"{}\" was converted. Pick another with the 'sheet' option, "
                         "or convert to an archive to get one file per sheet.",
                         table.sheets.size(), chosen->name));
  }
  return chosen;
}

// --------------------------------------------------------------------------- text helpers

std::size_t display_width(std::string_view text) noexcept {
  std::size_t width = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    const util::Utf8Decoded d = util::decode_utf8(text, i);
    i += d.length == 0 ? 1 : d.length;
    const char32_t cp = d.code_point;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) continue;
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0xFE00 && cp <= 0xFE0F)) {
      continue;
    }
    const bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
                      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                      (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
                      (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
    width += wide ? 2 : 1;
  }
  return width;
}

namespace {

std::vector<std::string> encoding_choices(bool with_auto) {
  std::vector<std::string> out;
  if (with_auto) out.emplace_back("auto");
  for (const char* name : {"utf-8", "utf-16le", "utf-16be", "utf-32le", "utf-32be"}) out.emplace_back(name);
  for (const CodePageInfo& page : code_pages()) out.emplace_back(page.name);
  return out;
}

}  // namespace

OptionSpec source_encoding_option() {
  return OptionSpec::choice("source_encoding", "Source encoding", "auto", encoding_choices(true),
                            "Character encoding of the input; auto detects byte order marks, UTF-8 and UTF-16.")
      .advanced();
}

Result<std::string> decode_source_text(ByteView input, std::string_view encoding, const Context& ctx,
                                       std::string* detected) {
  std::size_t replaced = 0;
  std::string text;
  std::string label;
  if (encoding.empty() || encoding == "auto") {
    const util::EncodingGuess guess = util::guess_encoding(input);
    util::DecodedText decoded = util::decode_text(input, guess.encoding);
    text = std::move(decoded.text);
    replaced = decoded.replaced;
    label = std::string(util::encoding_name(guess.encoding));
  } else if (const auto unicode = util::parse_encoding(encoding);
             unicode && *unicode != util::TextEncoding::Latin1 && *unicode != util::TextEncoding::Windows1252) {
    util::DecodedText decoded = util::decode_text(input, *unicode);
    text = std::move(decoded.text);
    replaced = decoded.replaced;
    label = std::string(util::encoding_name(*unicode));
  } else if (const CodePageInfo* page = find_code_page(encoding)) {
    ByteView body = input;
    if (body.size() >= 3 && as_chars(body).starts_with("\xEF\xBB\xBF")) body = body.subspan(3);
    text = decode_code_page(body, *page, &replaced);
    label = std::string(page->name);
  } else {
    return fail(Status::InvalidArgument, "unknown source encoding '{}'", encoding);
  }
  if (replaced > 0) {
    ctx.warn(std::format("{} invalid byte sequence(s) in the {} input were replaced with U+FFFD.", replaced, label));
  }
  if (detected != nullptr) *detected = std::move(label);
  return text;
}

Result<Bytes> encode_output_text(std::string_view utf8, std::string_view encoding, bool bom, const Context& ctx) {
  std::size_t replaced = 0;
  Bytes out;
  if (const auto unicode = util::parse_encoding(encoding.empty() ? "utf-8" : encoding);
      unicode && *unicode != util::TextEncoding::Latin1 && *unicode != util::TextEncoding::Windows1252) {
    util::EncodedText encoded = util::encode_text(utf8, *unicode, bom);
    out = std::move(encoded.bytes);
    replaced = encoded.replaced;
  } else if (const CodePageInfo* page = find_code_page(encoding)) {
    out = to_bytes(encode_code_page(utf8, *page, &replaced));
  } else {
    return fail(Status::InvalidArgument, "unknown encoding '{}'", encoding);
  }
  if (replaced > 0) {
    ctx.warn(std::format("{} character(s) cannot be represented in {} and were written as '?'.", replaced, encoding));
  }
  return out;
}

std::string truncate_utf8(std::string_view text, std::size_t max_code_points) {
  std::size_t i = 0;
  std::size_t count = 0;
  while (i < text.size() && count < max_code_points) {
    const util::Utf8Decoded d = util::decode_utf8(text, i);
    i += d.length == 0 ? 1 : d.length;
    ++count;
  }
  return std::string(text.substr(0, i));
}

std::vector<std::string> unique_sheet_names(const ir::Table& table, std::size_t max_length, std::string_view forbidden) {
  std::vector<std::string> names;
  std::set<std::string> used_lower;
  for (std::size_t i = 0; i < table.sheets.size(); ++i) {
    std::string name = util::sanitize_utf8(table.sheets[i].name);
    for (char& c : name) {
      if (forbidden.find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20) c = '_';
    }
    name = std::string(util::trim(name));
    if (name.empty()) name = std::format("Sheet{}", i + 1);
    name = truncate_utf8(name, max_length);
    std::string candidate = name;
    for (int n = 2; used_lower.contains(util::to_lower(candidate)); ++n) {
      const std::string suffix = std::format(" ({})", n);
      candidate = truncate_utf8(name, max_length > suffix.size() ? max_length - suffix.size() : 1) + suffix;
    }
    used_lower.insert(util::to_lower(candidate));
    names.push_back(std::move(candidate));
  }
  return names;
}

}  // namespace cvt::table
