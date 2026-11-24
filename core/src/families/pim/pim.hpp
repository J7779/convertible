// Convertible core - pim family: internal declarations shared by its sources.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/table.hpp"
#include "util/bytes.hpp"

namespace cvt {
class Registry;
}  // namespace cvt

namespace cvt::pim {

// -- registration, one function per area (each in its own source file) -------

void register_calendars(Registry& registry);    // calendar.cpp, calendar_csv.cpp
void register_contacts(Registry& registry);     // vcard.cpp, contacts_formats.cpp
void register_pim_bridges(Registry& registry);  // pim_bridges.cpp (Calendar/Contacts -> Table/Data/Document)
void register_playlists(Registry& registry);    // playlists.cpp
void register_bookmarks(Registry& registry);    // bookmarks.cpp
void register_feeds(Registry& registry);        // feeds.cpp
void register_bibliographies(Registry& registry);  // bibliography.cpp
void register_mail(Registry& registry);         // mail.cpp

// -- text input ----------------------------------------------------------------

/// The `source_encoding` read option shared by the text readers of the
/// family: "auto" (BOM, then UTF-8, then UTF-16/32 heuristics, else
/// Windows-1252) or any label `util::parse_encoding` accepts.
[[nodiscard]] OptionSpec source_encoding_option();

/// Decode a text file to UTF-8 without a byte order mark. `encoding` is the
/// value of the `source_encoding` option ("auto" or empty = detect).
/// Unknown labels are Status::InvalidArgument.
[[nodiscard]] Result<std::string> decode_text(ByteView data, std::string_view encoding = "auto");

/// The first `limit` bytes decoded leniently (BOM dropped, UTF-16 handled),
/// for use in `sniff` functions. Never fails.
[[nodiscard]] std::string sniff_text(ByteView data, std::size_t limit = 4096);

// -- CSV ---------------------------------------------------------------------------

/// RFC 4180 parsing: quoted fields with doubled quotes, embedded line
/// breaks, CRLF/LF/CR line ends. Rows are kept even when ragged. A trailing
/// empty line produces no row. Never fails (an unterminated quote runs to
/// the end of the text).
[[nodiscard]] std::vector<std::vector<std::string>> parse_csv(std::string_view text, char delimiter = ',');
/// Fields are quoted only when needed (delimiter, quote, CR, LF, leading or
/// trailing space). Every row ends with `newline`.
[[nodiscard]] std::string write_csv(const std::vector<std::vector<std::string>>& rows, char delimiter = ',',
                                    std::string_view newline = "\r\n");

// -- dates ----------------------------------------------------------------------------

/// Days since 1970-01-01 of a proleptic Gregorian date (any year).
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9u)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct CivilDate {
  std::int64_t year = 1970;
  unsigned month = 1;
  unsigned day = 1;
};
[[nodiscard]] constexpr CivilDate civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return CivilDate{y + (m <= 2 ? 1 : 0), m, d};
}
/// 0 = Sunday ... 6 = Saturday.
[[nodiscard]] constexpr int weekday_from_days(std::int64_t z) noexcept {
  return static_cast<int>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}
[[nodiscard]] constexpr bool is_leap_year(std::int64_t y) noexcept {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}
[[nodiscard]] constexpr unsigned days_in_month(std::int64_t y, unsigned m) noexcept {
  constexpr unsigned table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  return m == 2 && is_leap_year(y) ? 29 : table[m - 1];
}

/// A point in time read from text: seconds since the epoch of the civil
/// time as written, plus the UTC offset when the text carried one.
struct ParsedDateTime {
  std::int64_t local_seconds = 0;          ///< civil time as written, counted as if it were UTC
  std::optional<int> offset_minutes;       ///< east of UTC; nullopt = floating / unknown
  bool date_only = false;
  /// Seconds since the epoch in UTC (local time when there is no offset).
  [[nodiscard]] std::int64_t utc_seconds() const noexcept {
    return local_seconds - static_cast<std::int64_t>(offset_minutes.value_or(0)) * 60;
  }
};

/// ISO 8601 / RFC 3339 forms: "2024-05-01", "2024-05-01T12:30", "2024-05-01T12:30:00Z",
/// "2024-05-01 12:30:00+02:00", "20240501T123000Z", fractional seconds ignored.
[[nodiscard]] std::optional<ParsedDateTime> parse_iso_datetime(std::string_view text);
/// RFC 822 / 2822 / 5322 dates as used by mail and RSS: "Tue, 01 May 2024 12:30:00 +0200",
/// obsolete zone names (GMT, UT, EST, ...), two-digit years, missing seconds or weekday.
[[nodiscard]] std::optional<ParsedDateTime> parse_rfc822_datetime(std::string_view text);
/// "2024-05-01T12:30:00Z" (UTC), or with "+02:00" when `offset_minutes` is set.
[[nodiscard]] std::string format_iso_datetime(std::int64_t utc_seconds, std::optional<int> offset_minutes = 0);
/// "Wed, 01 May 2024 12:30:00 +0000".
[[nodiscard]] std::string format_rfc822_datetime(std::int64_t utc_seconds, int offset_minutes = 0);
/// "2024-05-01".
[[nodiscard]] std::string format_iso_date(std::int64_t days_since_epoch);

// -- tables ------------------------------------------------------------------------------

/// A one-sheet table whose first row holds `header` (has_header set) and
/// whose other rows are plain string cells (empty strings become Empty cells).
[[nodiscard]] ir::Table string_table(std::string sheet_name, std::vector<std::string> header,
                                     const std::vector<std::vector<std::string>>& rows);
/// The rows of a table's first sheet as display strings (header included).
[[nodiscard]] std::vector<std::vector<std::string>> table_strings(const ir::Table& table);

}  // namespace cvt::pim
