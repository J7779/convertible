// Convertible core - table family: number formats, formula notations and
// OOXML escapes shared by the spreadsheet formats.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/table/spreadsheet.hpp"

#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "families/table/xml_pull.hpp"
#include "util/text.hpp"

namespace cvt::table {

// --------------------------------------------------------------------------- number formats

namespace {

struct BuiltinFormat {
  std::uint32_t id;
  std::string_view code;
};

constexpr BuiltinFormat builtin_formats[] = {
    {1, "0"},
    {2, "0.00"},
    {3, "#,##0"},
    {4, "#,##0.00"},
    {9, "0%"},
    {10, "0.00%"},
    {11, "0.00E+00"},
    {12, "# ?/?"},
    {13, "# ?\?/??"},
    {14, "mm-dd-yy"},
    {15, "d-mmm-yy"},
    {16, "d-mmm"},
    {17, "mmm-yy"},
    {18, "h:mm AM/PM"},
    {19, "h:mm:ss AM/PM"},
    {20, "h:mm"},
    {21, "h:mm:ss"},
    {22, "m/d/yy h:mm"},
    {37, "#,##0 ;(#,##0)"},
    {38, "#,##0 ;[Red](#,##0)"},
    {39, "#,##0.00;(#,##0.00)"},
    {40, "#,##0.00;[Red](#,##0.00)"},
    {45, "mm:ss"},
    {46, "[h]:mm:ss"},
    {47, "mmss.0"},
    {48, "##0.0E+0"},
    {49, "@"},
};

}  // namespace

std::string_view builtin_number_format(std::uint32_t id) noexcept {
  for (const BuiltinFormat& format : builtin_formats) {
    if (format.id == id) return format.code;
  }
  // East Asian locale date formats; the codes depend on the locale, so a
  // neutral equivalent of the right kind is used.
  if ((id >= 27 && id <= 31) || (id >= 34 && id <= 36) || (id >= 50 && id <= 58)) return "yyyy-mm-dd";
  if (id == 32 || id == 33) return "h:mm:ss";
  return {};
}

std::optional<std::uint32_t> builtin_number_format_id(std::string_view code) noexcept {
  if (code.empty() || util::iequals(code, "General")) return 0u;
  for (const BuiltinFormat& format : builtin_formats) {
    if (format.code == code) return format.id;
  }
  return std::nullopt;
}

FormatKind classify_number_format(std::string_view code) noexcept {
  if (code.empty() || util::iequals(code, "General")) return FormatKind::General;
  bool date = false;
  bool time = false;
  bool month = false;
  bool text = false;
  bool elapsed = false;
  for (std::size_t i = 0; i < code.size(); ++i) {
    const char c = code[i];
    if (c == ';') break;  // only the first (positive number) section decides
    if (c == '"') {
      const std::size_t close = code.find('"', i + 1);
      if (close == std::string_view::npos) break;
      i = close;
      continue;
    }
    if (c == '\\' || c == '_' || c == '*') {
      ++i;
      continue;
    }
    if (c == '[') {
      const std::size_t close = code.find(']', i + 1);
      if (close == std::string_view::npos) break;
      const std::string_view inner = code.substr(i + 1, close - i - 1);
      if (!inner.empty()) {
        const char first = util::ascii_lower(inner[0]);
        bool all_same = true;
        for (const char k : inner) all_same = all_same && util::ascii_lower(k) == first;
        if (all_same && (first == 'h' || first == 'm' || first == 's')) elapsed = true;
      }
      i = close;
      continue;
    }
    const char l = util::ascii_lower(c);
    if (l == 'y' || l == 'd' || l == 'e' || l == 'g' || l == 'b') {
      if (l == 'g' && util::istarts_with(code.substr(i), "general")) {
        i += 6;
        continue;
      }
      if (l == 'e' && i > 0 && (code[i - 1] == '0' || code[i - 1] == '#' || code[i - 1] == '.')) continue;  // 0.00E+00
      if (l == 'e' && i + 1 < code.size() && (code[i + 1] == '+' || code[i + 1] == '-')) continue;
      if (l == 'b' || l == 'g') continue;
      date = true;
    } else if (l == 'h' || l == 's') {
      time = true;
    } else if (l == 'm') {
      month = true;
    } else if (l == 'a' && (util::istarts_with(code.substr(i), "am/pm") || util::istarts_with(code.substr(i), "a/p"))) {
      time = true;
      i += util::istarts_with(code.substr(i), "am/pm") ? 4 : 2;
    } else if (c == '@') {
      text = true;
    }
  }
  if (elapsed) return FormatKind::Number;
  if (month && !time) date = true;
  if (date && time) return FormatKind::DateTime;
  if (date) return FormatKind::Date;
  if (time) return FormatKind::Time;
  if (text) return FormatKind::Text;
  return FormatKind::Number;
}

DateStyle date_style_for(FormatKind kind) noexcept {
  switch (kind) {
    case FormatKind::Date: return DateStyle::Date;
    case FormatKind::Time: return DateStyle::Time;
    case FormatKind::DateTime: return DateStyle::DateTime;
    case FormatKind::General:
    case FormatKind::Number:
    case FormatKind::Text: break;
  }
  return DateStyle::Auto;
}

ir::Cell spreadsheet_number(double value, FormatKind kind, bool date1904) {
  if (is_date_kind(kind) && std::isfinite(value) && value >= 0.0 && value < 2958466.0) {
    // Times of day are day fractions in either date system.
    const double unix_seconds = excel_serial_to_unix(value, date1904 && !(kind == FormatKind::Time && value < 1.0));
    // A "date" format on a value with a time part shows both.
    DateStyle style = date_style_for(kind);
    if (style == DateStyle::Date && value != std::floor(value)) style = DateStyle::DateTime;
    if (style == DateStyle::Time && value >= 1.0) style = DateStyle::DateTime;
    return datetime_cell(unix_seconds, style);
  }
  return number_cell(value);
}

std::string cell_number_format(const ir::Sheet& sheet, std::size_t row, std::size_t column) {
  const std::string_view own = sheet.number_format_at(row, column);
  const ir::Cell& cell = sheet.at(row, column);
  if (cell.type == ir::CellType::DateTime) {
    if (!own.empty() && is_date_kind(classify_number_format(own))) return std::string(own);
    if (is_time_only(cell.number)) return std::string(iso_time_format);
    const double seconds_of_day = cell.number - std::floor(cell.number / 86400.0) * 86400.0;
    return std::string(seconds_of_day == 0.0 ? iso_date_format : iso_datetime_format);
  }
  if (cell.type == ir::CellType::Number && !own.empty() && !is_date_kind(classify_number_format(own))) {
    return std::string(own);
  }
  return {};
}

// --------------------------------------------------------------------------- formulas

namespace {

bool word_char(char c) noexcept {
  return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '.' || c == '$' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// Copy a string literal starting at formula[i] (a '"'), with doubled quotes.
std::size_t copy_string(std::string_view formula, std::size_t i, std::string& out) {
  const std::size_t start = i++;
  while (i < formula.size()) {
    if (formula[i] == '"') {
      if (i + 1 < formula.size() && formula[i + 1] == '"') {
        i += 2;
        continue;
      }
      ++i;
      break;
    }
    ++i;
  }
  out.append(formula.substr(start, i - start));
  return i;
}

// A cell reference with its absolute markers.
struct Ref {
  bool column_absolute = false;
  bool row_absolute = false;
  std::int64_t row = 0;
  std::int64_t column = 0;
};

std::optional<Ref> parse_ref(std::string_view text) {
  Ref ref;
  std::size_t i = 0;
  if (i < text.size() && text[i] == '$') {
    ref.column_absolute = true;
    ++i;
  }
  const std::size_t letters_start = i;
  while (i < text.size() && util::is_ascii_alpha(text[i])) ++i;
  if (i == letters_start || i - letters_start > 3) return std::nullopt;
  if (i < text.size() && text[i] == '$') {
    ref.row_absolute = true;
    ++i;
  }
  const std::size_t digits_start = i;
  while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
  if (i == digits_start || i != text.size() || i - digits_start > 7) return std::nullopt;
  std::int64_t column = 0;
  for (std::size_t k = letters_start; k < letters_start + (i - letters_start); ++k) {
    if (!util::is_ascii_alpha(text[k])) break;
    column = column * 26 + (util::ascii_upper(text[k]) - 'A' + 1);
  }
  ref.column = column - 1;
  ref.row = *util::parse_int(text.substr(digits_start)) - 1;
  if (ref.row < 0 || ref.column > 16383) return std::nullopt;
  return ref;
}

std::string format_ref(const Ref& ref) {
  if (ref.row < 0 || ref.column < 0 || ref.row > 1048575 || ref.column > 16383) return "#REF!";
  std::string out;
  if (ref.column_absolute) out.push_back('$');
  out.append(column_name(static_cast<std::uint32_t>(ref.column)));
  if (ref.row_absolute) out.push_back('$');
  out.append(std::to_string(ref.row + 1));
  return out;
}

// One A1 reference part, possibly with a sheet prefix: Sheet1!A1, 'My sheet'!$B$2.
struct RefPart {
  std::string sheet;  // as written, including quotes; empty when absent
  std::optional<Ref> ref;
};

RefPart split_sheet(std::string_view part) {
  RefPart out;
  const std::size_t bang = part.rfind('!');
  if (bang != std::string_view::npos) {
    out.sheet = std::string(part.substr(0, bang));
    part.remove_prefix(bang + 1);
  }
  out.ref = parse_ref(part);
  return out;
}

// Walk an A1 formula, handing every run of reference-like characters
// (letters, digits, $, ., !, quoted sheet names, ':') that is not a function
// name to `on_run`, and copying everything else.
template <class OnRun, class OnChar>
std::string walk_a1(std::string_view formula, OnRun on_run, OnChar on_char) {
  std::string out;
  std::size_t i = 0;
  while (i < formula.size()) {
    const char c = formula[i];
    if (c == '"') {
      i = copy_string(formula, i, out);
      continue;
    }
    if (word_char(c) || c == '\'') {
      const std::size_t start = i;
      while (i < formula.size()) {
        const char k = formula[i];
        if (k == '\'') {
          ++i;
          while (i < formula.size()) {
            if (formula[i] == '\'') {
              if (i + 1 < formula.size() && formula[i + 1] == '\'') {
                i += 2;
                continue;
              }
              ++i;
              break;
            }
            ++i;
          }
        } else if (word_char(k) || k == '!' || k == ':') {
          ++i;
        } else {
          break;
        }
      }
      std::string_view run = formula.substr(start, i - start);
      // A trailing ':' belongs to whatever follows.
      std::size_t trailing = 0;
      while (!run.empty() && run.back() == ':') {
        run.remove_suffix(1);
        ++trailing;
      }
      if (i < formula.size() && formula[i] == '(') {
        out.append(run);  // function name
      } else {
        on_run(run, out);
      }
      out.append(trailing, ':');
      continue;
    }
    on_char(c, out);
    ++i;
  }
  return out;
}

// Split a run at ':' (outside quotes).
std::vector<std::string_view> split_range(std::string_view run) {
  std::vector<std::string_view> parts;
  bool quoted = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i < run.size(); ++i) {
    if (run[i] == '\'') quoted = !quoted;
    if (run[i] == ':' && !quoted) {
      parts.push_back(run.substr(start, i - start));
      start = i + 1;
    }
  }
  parts.push_back(run.substr(start));
  return parts;
}

std::string unquote_sheet(std::string_view sheet) {
  if (sheet.size() >= 2 && sheet.front() == '\'' && sheet.back() == '\'') {
    return util::replace_all(sheet.substr(1, sheet.size() - 2), "''", "'");
  }
  return std::string(sheet);
}

std::string quote_sheet_if_needed(std::string_view sheet) {
  bool plain = !sheet.empty() && !util::is_ascii_digit(sheet[0]);
  for (const char c : sheet) plain = plain && (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '.');
  if (plain) return std::string(sheet);
  return "'" + util::replace_all(sheet, "'", "''") + "'";
}

}  // namespace

std::string shift_formula(std::string_view formula, std::int64_t rows, std::int64_t columns) {
  return walk_a1(
      formula,
      [&](std::string_view run, std::string& out) {
        const auto parts = split_range(run);
        std::vector<RefPart> refs;
        for (const auto part : parts) {
          refs.push_back(split_sheet(part));
          if (!refs.back().ref) {
            out.append(run);
            return;
          }
        }
        for (std::size_t k = 0; k < refs.size(); ++k) {
          if (k > 0) out.push_back(':');
          Ref ref = *refs[k].ref;
          if (!ref.row_absolute) ref.row += rows;
          if (!ref.column_absolute) ref.column += columns;
          if (!refs[k].sheet.empty()) out.append(refs[k].sheet + "!");
          out.append(format_ref(ref));
        }
      },
      [](char c, std::string& out) { out.push_back(c); });
}

std::string excel_to_odf_formula(std::string_view formula) {
  if (formula.starts_with('=')) formula.remove_prefix(1);
  std::string body = walk_a1(
      formula,
      [&](std::string_view run, std::string& out) {
        const auto parts = split_range(run);
        std::vector<RefPart> refs;
        for (const auto part : parts) {
          refs.push_back(split_sheet(part));
          if (!refs.back().ref) {
            out.append(run);
            return;
          }
        }
        out.push_back('[');
        for (std::size_t k = 0; k < refs.size(); ++k) {
          if (k > 0) out.push_back(':');
          if (!refs[k].sheet.empty()) {
            const std::string name = unquote_sheet(refs[k].sheet);
            out.push_back('$');
            out.append(quote_sheet_if_needed(name));
          }
          out.push_back('.');
          out.append(format_ref(*refs[k].ref));
        }
        out.push_back(']');
      },
      [](char c, std::string& out) { out.push_back(c == ',' ? ';' : c); });
  return "of:=" + body;
}

std::string odf_to_excel_formula(std::string_view formula) {
  if (const std::size_t colon = formula.find(':'); colon != std::string_view::npos && colon < 8 &&
                                                   formula.substr(0, colon).find_first_of("[.\"(") == std::string_view::npos) {
    formula.remove_prefix(colon + 1);  // "of:", "oooc:", "msoxl:"
  }
  if (formula.starts_with('=')) formula.remove_prefix(1);
  std::string out;
  std::size_t i = 0;
  while (i < formula.size()) {
    const char c = formula[i];
    if (c == '"') {
      i = copy_string(formula, i, out);
      continue;
    }
    if (c == '[') {
      // Find the closing bracket outside quoted sheet names.
      std::size_t j = i + 1;
      bool quoted = false;
      for (; j < formula.size(); ++j) {
        if (formula[j] == '\'') quoted = !quoted;
        if (formula[j] == ']' && !quoted) break;
      }
      if (j >= formula.size()) {
        out.append(formula.substr(i));
        break;
      }
      const std::string_view inner = formula.substr(i + 1, j - i - 1);
      std::string first_sheet;
      std::size_t k = 0;
      for (const std::string_view part : split_range(inner)) {
        // [$]Sheet.A1 or .A1; the sheet name may be quoted.
        std::size_t dot = std::string_view::npos;
        bool q = false;
        for (std::size_t p = 0; p < part.size(); ++p) {
          if (part[p] == '\'') q = !q;
          if (part[p] == '.' && !q) dot = p;
        }
        std::string_view sheet = dot == std::string_view::npos ? std::string_view() : part.substr(0, dot);
        std::string_view cell = dot == std::string_view::npos ? part : part.substr(dot + 1);
        if (sheet.starts_with('$')) sheet.remove_prefix(1);
        if (k > 0) out.push_back(':');
        const std::string name = unquote_sheet(sheet);
        if (!name.empty() && (k == 0 || name != first_sheet)) out.append(quote_sheet_if_needed(name) + "!");
        if (k == 0) first_sheet = name;
        out.append(cell);
        ++k;
      }
      i = j + 1;
      continue;
    }
    out.push_back(c == ';' ? ',' : c);
    ++i;
  }
  return out;
}

std::string r1c1_to_a1_formula(std::string_view formula, std::uint32_t row, std::uint32_t column) {
  std::string out;
  std::size_t i = 0;
  // Parse "n" or "[n]" after R or C; returns the absolute or relative value.
  const auto part = [&](std::size_t& at, std::int64_t base, bool& absolute) -> std::optional<std::int64_t> {
    absolute = false;
    if (at < formula.size() && formula[at] == '[') {
      const std::size_t close = formula.find(']', at);
      if (close == std::string_view::npos) return std::nullopt;
      const auto delta = util::parse_int(formula.substr(at + 1, close - at - 1));
      if (!delta) return std::nullopt;
      at = close + 1;
      return base + *delta;
    }
    std::size_t end = at;
    while (end < formula.size() && util::is_ascii_digit(formula[end])) ++end;
    if (end == at) return base;
    const auto value = util::parse_int(formula.substr(at, end - at));
    if (!value || *value < 1) return std::nullopt;
    at = end;
    absolute = true;
    return *value - 1;
  };
  while (i < formula.size()) {
    const char c = formula[i];
    if (c == '"') {
      i = copy_string(formula, i, out);
      continue;
    }
    const bool boundary = i == 0 || !(word_char(formula[i - 1]));
    if ((c == 'R' || c == 'r') && boundary) {
      std::size_t at = i + 1;
      bool row_abs = false;
      bool col_abs = false;
      const auto r = part(at, row, row_abs);
      if (r && at < formula.size() && (formula[at] == 'C' || formula[at] == 'c')) {
        ++at;
        const auto col = part(at, column, col_abs);
        const bool ends = at >= formula.size() || !(word_char(formula[at]) || formula[at] == '(');
        if (col && ends) {
          Ref ref;
          ref.row = *r;
          ref.column = *col;
          ref.row_absolute = row_abs;
          ref.column_absolute = col_abs;
          out.append(format_ref(ref));
          i = at;
          continue;
        }
      }
    }
    // Copy a whole word so that names containing R or C are left alone.
    if (word_char(c)) {
      const std::size_t start = i;
      while (i < formula.size() && word_char(formula[i])) ++i;
      out.append(formula.substr(start, i - start));
      continue;
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

std::string a1_to_r1c1_formula(std::string_view formula, std::uint32_t row, std::uint32_t column) {
  if (formula.starts_with('=')) formula.remove_prefix(1);
  return walk_a1(
      formula,
      [&](std::string_view run, std::string& out) {
        const auto parts = split_range(run);
        std::vector<RefPart> refs;
        for (const auto piece : parts) {
          refs.push_back(split_sheet(piece));
          if (!refs.back().ref) {
            out.append(run);
            return;
          }
        }
        for (std::size_t k = 0; k < refs.size(); ++k) {
          if (k > 0) out.push_back(':');
          const Ref& ref = *refs[k].ref;
          if (!refs[k].sheet.empty()) out.append(refs[k].sheet + "!");
          out.push_back('R');
          if (ref.row_absolute) {
            out.append(std::to_string(ref.row + 1));
          } else if (ref.row != row) {
            out.append(std::format("[{}]", ref.row - static_cast<std::int64_t>(row)));
          }
          out.push_back('C');
          if (ref.column_absolute) {
            out.append(std::to_string(ref.column + 1));
          } else if (ref.column != column) {
            out.append(std::format("[{}]", ref.column - static_cast<std::int64_t>(column)));
          }
        }
      },
      [](char c, std::string& out) { out.push_back(c); });
}

// --------------------------------------------------------------------------- OOXML strings

std::string ooxml_unescape(std::string_view text) {
  if (text.find("_x") == std::string_view::npos) return std::string(text);
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '_' && i + 6 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X') && text[i + 6] == '_') {
      if (const auto code = util::parse_int(text.substr(i + 2, 4), 16)) {
        const auto cp = static_cast<char32_t>(*code);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 13 < text.size() && text[i + 7] == '_' && text[i + 13] == '_') {
          if (const auto low = util::parse_int(text.substr(i + 9, 4), 16); low && *low >= 0xDC00 && *low <= 0xDFFF) {
            util::append_utf8(out, 0x10000 + ((cp - 0xD800) << 10) + (static_cast<char32_t>(*low) - 0xDC00));
            i += 14;
            continue;
          }
        }
        util::append_utf8(out, cp);
        i += 7;
        continue;
      }
    }
    out.push_back(text[i]);
    ++i;
  }
  return out;
}

void ooxml_escape_append(std::string& out, std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '_' && i + 6 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X') && text[i + 6] == '_' &&
        util::parse_int(text.substr(i + 2, 4), 16)) {
      out.append("_x005F_");
      ++i;
      continue;
    }
    if (c < 0x20 && c != '\t' && c != '\n') {
      out.append(std::format("_x{:04X}_", c));
      ++i;
      continue;
    }
    const std::size_t start = i;
    while (i < text.size()) {
      const auto k = static_cast<unsigned char>(text[i]);
      if ((k < 0x20 && k != '\t' && k != '\n') || k == '_') break;
      ++i;
    }
    if (i == start) {  // a '_' that is not an escape
      out.push_back('_');
      ++i;
      continue;
    }
    xml_escape_append(out, text.substr(start, i - start));
  }
}

std::string_view excel_error_text(std::uint8_t code) noexcept {
  switch (code) {
    case 0x00: return "#NULL!";
    case 0x07: return "#DIV/0!";
    case 0x0F: return "#VALUE!";
    case 0x17: return "#REF!";
    case 0x1D: return "#NAME?";
    case 0x24: return "#NUM!";
    case 0x2A: return "#N/A";
    case 0x2B: return "#GETTING_DATA";
    default: break;
  }
  return "#VALUE!";
}

}  // namespace cvt::table
