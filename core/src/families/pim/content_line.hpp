// Convertible core - pim family: content lines (RFC 5545 iCalendar, RFC 6350
// vCard, and their 1.0/2.1 predecessors), calendar times, and the property
// model shared by the vCard, jCard and xCard codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/errors.hpp"
#include "engine/limits.hpp"
#include "engine/options.hpp"
#include "ir/calendar.hpp"
#include "ir/contacts.hpp"

namespace cvt {
class Registry;
}  // namespace cvt

namespace cvt::pim {

// -- registration of the sources that pim.hpp does not name ---------------------------------

void register_calendar_csv(Registry& registry);     // calendar_csv.cpp
void register_contacts_formats(Registry& registry);  // contacts_formats.cpp

/// Writer options of every Contacts writer that can embed pictures:
/// drop_photos and strip_metadata.
[[nodiscard]] std::vector<OptionSpec> contacts_write_options_with_photos();

}  // namespace cvt::pim

namespace cvt::pim::cl {

using Params = std::vector<std::pair<std::string, std::string>>;

/// One unfolded content line `[group.]NAME;PARAM=value:VALUE`.
struct ContentLine {
  std::string group;  ///< vCard group ("item1"), empty when none
  std::string name;   ///< upper-case
  Params params;      ///< names upper-case; values unquoted, as written
  std::string value;  ///< raw value (escapes kept), after quoted-printable and charset decoding
};

/// Unfold and split text into content lines. With `legacy` (vCard 2.1 /
/// vCalendar 1.0) bare parameters (`TEL;HOME:`) become TYPE values and
/// QUOTED-PRINTABLE values (with '=' soft breaks) and CHARSET parameters
/// are decoded. Lines without a ':' are skipped. Limits the line count.
[[nodiscard]] Result<std::vector<ContentLine>> parse_lines(std::string_view text, bool legacy, const Limits& limits);

/// The first value of a parameter, or "" (case-insensitive name).
[[nodiscard]] std::string param(const ContentLine& line, std::string_view name);
[[nodiscard]] bool has_param(const ContentLine& line, std::string_view name);
/// All TYPE values, lower-case, split on commas.
[[nodiscard]] std::vector<std::string> types(const Params& params);

/// RFC 5545/6350 text escapes: "\n", "\N", "\,", "\;", "\\".
[[nodiscard]] std::string unescape(std::string_view raw);
[[nodiscard]] std::string escape(std::string_view text, bool semicolons = true, bool commas = true);
/// Split a raw value on an unescaped separator; the pieces keep their escapes.
[[nodiscard]] std::vector<std::string> split_raw(std::string_view raw, char separator);
/// Split on the separator and unescape every piece.
[[nodiscard]] std::vector<std::string> split_unescape(std::string_view raw, char separator);

/// Quoted-printable (RFC 2045) without the line structure rules of mail.
[[nodiscard]] std::string decode_qp(std::string_view text);
/// Encode for vCard 2.1. With `soft_breaks`, '=' soft breaks keep physical
/// lines under 76 octets (`first_line_used` = length of the "NAME;PARAMS:"
/// prefix); without, the value stays on one line, which more readers accept.
[[nodiscard]] std::string encode_qp(std::string_view text, std::size_t first_line_used, bool soft_breaks = true);
/// Decode bytes in a CHARSET to UTF-8 (UTF-8, ISO-8859-1, Windows-1252,
/// UTF-16, US-ASCII; anything else is treated as UTF-8 and repaired).
[[nodiscard]] std::string decode_charset(std::string_view bytes, std::string_view charset);

/// Quote a parameter value when it holds ':', ';' or ','.
[[nodiscard]] std::string param_value(std::string_view value);
/// "NAME;P=v:value" folded at 75 octets (on UTF-8 boundaries) with CRLF +
/// space continuations, ending in CRLF. `value` must already be escaped.
[[nodiscard]] std::string format_line(std::string_view name, const Params& params, std::string_view value);
/// Fold one logical line (no CRLF inside) at 75 octets; ends in CRLF.
[[nodiscard]] std::string fold(std::string_view line);

// -- iCalendar values -------------------------------------------------------------------

/// DATE ("20240501"), DATE-TIME ("20240501T103000", "...Z") and the ISO
/// extended forms some producers write ("2024-05-01T10:30:00Z").
[[nodiscard]] std::optional<ir::CalendarTime> parse_time(std::string_view value, std::string_view tzid = {});
/// "20240501" or "20240501T103000" / "...Z".
[[nodiscard]] std::string format_time(const ir::CalendarTime& time);
/// Civil time as seconds since the epoch (as if it were UTC).
[[nodiscard]] std::int64_t civil_seconds(const ir::CalendarTime& time);
/// A time `seconds` (civil) with the date_only / utc / tzid flags of `like`.
[[nodiscard]] ir::CalendarTime time_from_seconds(std::int64_t seconds, const ir::CalendarTime& like);
/// RFC 5545 DURATION ("-PT15M", "P1DT2H", "P2W") in seconds.
[[nodiscard]] std::optional<std::int64_t> parse_duration(std::string_view value);
[[nodiscard]] std::string format_duration(std::int64_t seconds);
/// ISO 8601 form for tables and data: "2024-05-01", "2024-05-01T10:30:00",
/// "2024-05-01T10:30:00Z".
[[nodiscard]] std::string iso_time(const ir::CalendarTime& time);

// -- vCard property model (shared by vcf, jCard and xCard) ------------------------------------

/// A vCard property with its value kept raw (text escapes in place), so that
/// structured components and list values can be told apart.
struct VProp {
  std::string group;
  std::string name;  ///< upper-case
  Params params;
  std::string raw;
};

/// Build a card from properties (VERSION, BEGIN and END ignored).
[[nodiscard]] ir::Card card_from_props(const std::vector<VProp>& props);

/// A property to write: components, each a list of values (unescaped).
struct WProp {
  WProp() = default;
  WProp(std::string group_name, std::string property_name, Params parameters,
        std::vector<std::vector<std::string>> values)
      : group(std::move(group_name)), name(std::move(property_name)), params(std::move(parameters)),
        components(std::move(values)) {}

  std::string group;
  std::string name;  ///< upper-case
  Params params;     ///< TYPE values lower-case
  std::vector<std::vector<std::string>> components;
  std::string value_type = "text";  ///< jCard/xCard value type: text, uri, date, ...
  bool binary = false;               ///< PHOTO with embedded bytes (components[0][0] is base64)
  std::string media_type;            ///< MIME type of embedded bytes, e.g. "image/jpeg"
};

/// "1990-04-27", "--04-27" from the basic, extended and date-time forms.
[[nodiscard]] std::string normalize_date(std::string_view value);

/// The properties of a card for vCard `version` ("2.1", "3.0", "4.0"),
/// without BEGIN/VERSION/END.
[[nodiscard]] std::vector<WProp> props_from_card(const ir::Card& card, std::string_view version, bool drop_photos);

}  // namespace cvt::pim::cl
