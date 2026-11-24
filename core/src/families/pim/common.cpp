// Convertible core - pim family: helpers shared by the family's sources.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/pim/pim.hpp"
#include "util/text.hpp"

namespace cvt::pim {

namespace {

std::optional<int> digits(std::string_view text, std::size_t at, std::size_t count) {
  if (at + count > text.size()) return std::nullopt;
  int value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[at + i];
    if (!util::is_ascii_digit(c)) return std::nullopt;
    value = value * 10 + (c - '0');
  }
  return value;
}

bool valid_civil(std::int64_t year, int month, int day, int hour, int minute, int second) {
  return month >= 1 && month <= 12 && day >= 1 && static_cast<unsigned>(day) <= days_in_month(year, static_cast<unsigned>(month)) &&
         hour >= 0 && hour <= 24 && minute >= 0 && minute <= 59 && second >= 0 && second <= 60;
}

std::int64_t civil_seconds(std::int64_t year, int month, int day, int hour, int minute, int second) {
  return days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hour * 3600 +
         minute * 60 + second;
}

}  // namespace

OptionSpec source_encoding_option() {
  return OptionSpec::choice("source_encoding", "Source encoding", "auto",
                            {"auto", "utf-8", "utf-16le", "utf-16be", "utf-32le", "utf-32be", "latin1", "windows-1252"},
                            "Character encoding of the input; 'auto' detects Unicode and falls back to Windows-1252.")
      .advanced();
}

Result<std::string> decode_text(ByteView data, std::string_view encoding) {
  util::TextEncoding chosen = util::TextEncoding::Utf8;
  if (encoding.empty() || util::iequals(encoding, "auto")) {
    const util::EncodingGuess guess = util::guess_encoding(data);
    chosen = guess.encoding;
  } else {
    const auto parsed = util::parse_encoding(encoding);
    if (!parsed) return fail(Status::InvalidArgument, "unknown character encoding '{}'", util::sanitize_utf8(encoding));
    chosen = *parsed;
  }
  util::DecodedText decoded = util::decode_text(data, chosen);
  std::string text = std::move(decoded.text);
  if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
  return text;
}

std::string sniff_text(ByteView data, std::size_t limit) {
  const util::EncodingGuess guess = util::guess_encoding(data.first(std::min(data.size(), limit)));
  std::size_t take = std::min(data.size(), limit);
  // Keep UTF-16/32 code units whole.
  if (guess.encoding == util::TextEncoding::Utf16LE || guess.encoding == util::TextEncoding::Utf16BE) take &= ~std::size_t{1};
  if (guess.encoding == util::TextEncoding::Utf32LE || guess.encoding == util::TextEncoding::Utf32BE) take &= ~std::size_t{3};
  std::string text = util::decode_text(data.first(take), guess.encoding).text;
  if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
  return text;
}

// --------------------------------------------------------------------------- CSV

std::vector<std::vector<std::string>> parse_csv(std::string_view text, char delimiter) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false;
  bool field_started = false;
  std::size_t i = 0;
  const auto end_field = [&] {
    row.push_back(std::move(field));
    field.clear();
    field_started = false;
  };
  const auto end_row = [&] {
    end_field();
    if (!(row.size() == 1 && row[0].empty())) rows.push_back(std::move(row));
    row.clear();
  };
  while (i < text.size()) {
    const char c = text[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') {
          field.push_back('"');
          i += 2;
          continue;
        }
        quoted = false;
        ++i;
        continue;
      }
      field.push_back(c);
      ++i;
      continue;
    }
    if (c == '"' && !field_started) {
      quoted = true;
      field_started = true;
      ++i;
    } else if (c == delimiter) {
      end_field();
      ++i;
    } else if (c == '\r' || c == '\n') {
      end_row();
      i += (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ? 2 : 1;
    } else {
      field.push_back(c);
      field_started = true;
      ++i;
    }
  }
  if (field_started || !field.empty() || !row.empty()) end_row();
  return rows;
}

std::string write_csv(const std::vector<std::vector<std::string>>& rows, char delimiter, std::string_view newline) {
  std::string out;
  for (const std::vector<std::string>& row : rows) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i > 0) out.push_back(delimiter);
      const std::string& value = row[i];
      const bool needs_quotes = value.find_first_of(std::string{delimiter} + "\"\r\n") != std::string::npos ||
                                (!value.empty() && (value.front() == ' ' || value.back() == ' '));
      if (!needs_quotes) {
        out.append(value);
        continue;
      }
      out.push_back('"');
      for (const char c : value) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
      }
      out.push_back('"');
    }
    out.append(newline);
  }
  return out;
}

// --------------------------------------------------------------------------- dates

std::optional<ParsedDateTime> parse_iso_datetime(std::string_view text) {
  text = util::trim(text);
  if (text.size() < 8) return std::nullopt;
  std::size_t pos = 0;
  std::int64_t year = 0;
  int month = 0;
  int day = 0;
  bool basic = false;  // 20240501 form
  {
    const auto y = digits(text, 0, 4);
    if (!y) return std::nullopt;
    year = *y;
    pos = 4;
    if (pos < text.size() && text[pos] == '-') {
      const auto m = digits(text, 5, 2);
      if (!m || text.size() < 10 || text[7] != '-') return std::nullopt;
      const auto d = digits(text, 8, 2);
      if (!d) return std::nullopt;
      month = *m;
      day = *d;
      pos = 10;
    } else {
      const auto m = digits(text, 4, 2);
      const auto d = digits(text, 6, 2);
      if (!m || !d) return std::nullopt;
      month = *m;
      day = *d;
      pos = 8;
      basic = true;
    }
  }
  ParsedDateTime out;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (pos == text.size()) {
    if (!valid_civil(year, month, day, 0, 0, 0)) return std::nullopt;
    out.date_only = true;
    out.local_seconds = civil_seconds(year, month, day, 0, 0, 0);
    return out;
  }
  if (text[pos] != 'T' && text[pos] != 't' && text[pos] != ' ') return std::nullopt;
  ++pos;
  const auto h = digits(text, pos, 2);
  if (!h) return std::nullopt;
  hour = *h;
  pos += 2;
  if (pos < text.size() && text[pos] == ':') ++pos;
  const auto mi = digits(text, pos, 2);
  if (!mi) return std::nullopt;
  minute = *mi;
  pos += 2;
  if (pos < text.size() && (text[pos] == ':' || util::is_ascii_digit(text[pos]))) {
    if (text[pos] == ':') ++pos;
    const auto s = digits(text, pos, 2);
    if (!s) return std::nullopt;
    second = *s;
    pos += 2;
    if (pos < text.size() && (text[pos] == '.' || text[pos] == ',')) {
      ++pos;
      while (pos < text.size() && util::is_ascii_digit(text[pos])) ++pos;
    }
  }
  (void)basic;
  if (!valid_civil(year, month, day, hour, minute, second)) return std::nullopt;
  out.local_seconds = civil_seconds(year, month, day, hour, minute, second);
  if (pos == text.size()) return out;
  const std::string_view zone = util::trim(text.substr(pos));
  if (zone == "Z" || zone == "z") {
    out.offset_minutes = 0;
    return out;
  }
  if (zone.size() >= 3 && (zone[0] == '+' || zone[0] == '-')) {
    const auto zh = digits(zone, 1, 2);
    if (!zh) return std::nullopt;
    std::size_t zp = 3;
    if (zp < zone.size() && zone[zp] == ':') ++zp;
    int zm = 0;
    if (zp < zone.size()) {
      const auto parsed = digits(zone, zp, 2);
      if (!parsed || zp + 2 != zone.size()) return std::nullopt;
      zm = *parsed;
    }
    const int sign = zone[0] == '-' ? -1 : 1;
    out.offset_minutes = sign * (*zh * 60 + zm);
    return out;
  }
  return std::nullopt;
}

std::optional<ParsedDateTime> parse_rfc822_datetime(std::string_view text) {
  // Tokenise on spaces and commas.
  std::vector<std::string_view> tokens;
  {
    std::size_t i = 0;
    while (i < text.size()) {
      while (i < text.size() && (util::is_ascii_space(text[i]) || text[i] == ',')) ++i;
      // Comments such as "(UTC)" are dropped.
      if (i < text.size() && text[i] == '(') {
        while (i < text.size() && text[i] != ')') ++i;
        if (i < text.size()) ++i;
        continue;
      }
      const std::size_t start = i;
      while (i < text.size() && !util::is_ascii_space(text[i]) && text[i] != ',' && text[i] != '(') ++i;
      if (i > start) tokens.push_back(text.substr(start, i - start));
    }
  }
  static constexpr std::array<std::string_view, 12> months = {"jan", "feb", "mar", "apr", "may", "jun",
                                                              "jul", "aug", "sep", "oct", "nov", "dec"};
  std::size_t t = 0;
  if (t < tokens.size() && tokens[t].size() >= 3 && util::is_ascii_alpha(tokens[t][0])) {
    // Weekday, unless it is already the month (asctime-like "May 01 2024").
    bool is_month = false;
    for (const std::string_view m : months) is_month = is_month || util::istarts_with(tokens[t], m);
    if (!is_month) ++t;
  }
  if (tokens.size() < t + 3) return std::nullopt;
  std::optional<std::int64_t> day_value;
  int month = 0;
  std::string_view first = tokens[t];
  std::string_view second_token = tokens[t + 1];
  const auto month_of = [&](std::string_view token) {
    for (std::size_t m = 0; m < months.size(); ++m) {
      if (util::istarts_with(token, months[m])) return static_cast<int>(m + 1);
    }
    return 0;
  };
  if (month_of(first) != 0) {  // "May 01 2024"
    month = month_of(first);
    day_value = util::parse_int(second_token);
  } else {
    day_value = util::parse_int(first);
    month = month_of(second_token);
  }
  if (!day_value || month == 0) return std::nullopt;
  const auto year_value = util::parse_int(tokens[t + 2]);
  if (!year_value) return std::nullopt;
  std::int64_t year = *year_value;
  if (tokens[t + 2].size() <= 2) year += year < 50 ? 2000 : 1900;
  else if (tokens[t + 2].size() == 3) year += 1900;
  int hour = 0;
  int minute = 0;
  int sec = 0;
  std::size_t next = t + 3;
  if (next < tokens.size() && tokens[next].find(':') != std::string_view::npos) {
    const std::vector<std::string_view> parts = util::split(tokens[next], ':');
    const auto ph = util::parse_int(parts[0]);
    const auto pm = parts.size() > 1 ? util::parse_int(parts[1]) : std::nullopt;
    const auto ps = parts.size() > 2 ? util::parse_int(parts[2].substr(0, 2)) : std::optional<std::int64_t>(0);
    if (!ph || !pm || !ps) return std::nullopt;
    hour = static_cast<int>(*ph);
    minute = static_cast<int>(*pm);
    sec = static_cast<int>(*ps);
    ++next;
  }
  if (*day_value < 1 || *day_value > 31 || !valid_civil(year, month, static_cast<int>(*day_value), hour, minute, sec)) {
    return std::nullopt;
  }
  ParsedDateTime out;
  out.local_seconds = civil_seconds(year, month, static_cast<int>(*day_value), hour, minute, sec);
  if (next < tokens.size()) {
    const std::string_view zone = tokens[next];
    if ((zone[0] == '+' || zone[0] == '-') && zone.size() == 5) {
      const auto zh = digits(zone, 1, 2);
      const auto zm = digits(zone, 3, 2);
      if (zh && zm) out.offset_minutes = (zone[0] == '-' ? -1 : 1) * (*zh * 60 + *zm);
    } else {
      static constexpr std::array<std::pair<std::string_view, int>, 11> names = {{{"UT", 0},
                                                                                  {"UTC", 0},
                                                                                  {"GMT", 0},
                                                                                  {"Z", 0},
                                                                                  {"EST", -300},
                                                                                  {"EDT", -240},
                                                                                  {"CST", -360},
                                                                                  {"CDT", -300},
                                                                                  {"MST", -420},
                                                                                  {"MDT", -360},
                                                                                  {"PST", -480}}};
      for (const auto& [name, offset] : names) {
        if (util::iequals(zone, name)) out.offset_minutes = offset;
      }
      if (util::iequals(zone, "PDT")) out.offset_minutes = -420;
    }
  }
  return out;
}

std::string format_iso_datetime(std::int64_t utc_seconds, std::optional<int> offset_minutes) {
  const std::int64_t local = utc_seconds + static_cast<std::int64_t>(offset_minutes.value_or(0)) * 60;
  std::int64_t days = local / 86400;
  std::int64_t rest = local % 86400;
  if (rest < 0) {
    rest += 86400;
    --days;
  }
  const CivilDate date = civil_from_days(days);
  std::string out = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", date.year, date.month, date.day, rest / 3600,
                                (rest / 60) % 60, rest % 60);
  if (!offset_minutes) return out;
  if (*offset_minutes == 0) return out + "Z";
  const int magnitude = *offset_minutes < 0 ? -*offset_minutes : *offset_minutes;
  out += std::format("{}{:02}:{:02}", *offset_minutes < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
  return out;
}

std::string format_rfc822_datetime(std::int64_t utc_seconds, int offset_minutes) {
  static constexpr std::array<std::string_view, 7> weekdays = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static constexpr std::array<std::string_view, 12> months = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const std::int64_t local = utc_seconds + static_cast<std::int64_t>(offset_minutes) * 60;
  std::int64_t days = local / 86400;
  std::int64_t rest = local % 86400;
  if (rest < 0) {
    rest += 86400;
    --days;
  }
  const CivilDate date = civil_from_days(days);
  const int magnitude = offset_minutes < 0 ? -offset_minutes : offset_minutes;
  return std::format("{}, {:02} {} {:04} {:02}:{:02}:{:02} {}{:02}{:02}", weekdays[static_cast<std::size_t>(weekday_from_days(days))],
                     date.day, months[date.month - 1], date.year, rest / 3600, (rest / 60) % 60, rest % 60,
                     offset_minutes < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
}

std::string format_iso_date(std::int64_t days_since_epoch) {
  const CivilDate date = civil_from_days(days_since_epoch);
  return std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day);
}

// --------------------------------------------------------------------------- tables

ir::Table string_table(std::string sheet_name, std::vector<std::string> header,
                       const std::vector<std::vector<std::string>>& rows) {
  ir::Sheet sheet;
  sheet.name = std::move(sheet_name);
  sheet.has_header = true;
  std::vector<ir::Cell> head;
  head.reserve(header.size());
  for (std::string& title : header) head.push_back(ir::Cell::string(std::move(title)));
  sheet.rows.push_back(std::move(head));
  for (const std::vector<std::string>& row : rows) {
    std::vector<ir::Cell> cells;
    cells.reserve(row.size());
    for (const std::string& value : row) cells.push_back(value.empty() ? ir::Cell{} : ir::Cell::string(value));
    sheet.rows.push_back(std::move(cells));
  }
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

std::vector<std::vector<std::string>> table_strings(const ir::Table& table) {
  std::vector<std::vector<std::string>> out;
  if (table.sheets.empty()) return out;
  for (const std::vector<ir::Cell>& row : table.sheets.front().rows) {
    std::vector<std::string> values;
    values.reserve(row.size());
    for (const ir::Cell& cell : row) values.push_back(cell.text);
    out.push_back(std::move(values));
  }
  return out;
}

}  // namespace cvt::pim
