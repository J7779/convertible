// Convertible core - pim family: iCalendar (.ics, .ifb) and vCalendar 1.0 (.vcs).
//
// Reading builds a tree of components from the unfolded content lines and
// maps VEVENT / VTODO / VJOURNAL / VFREEBUSY to ir::Event, keeping VTIMEZONE
// and unknown components verbatim. Recurrences are preserved as RRULE /
// EXDATE / RDATE and can optionally be expanded into single occurrences
// within a date range (read options expand_recurrences, range_start,
// range_end, max_occurrences).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/pim/content_line.hpp"
#include "families/pim/pim.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

namespace cvt::pim {

namespace {

using cl::ContentLine;

constexpr std::size_t max_component_depth = 32;

struct Node {
  std::string name;
  std::vector<ContentLine> props;
  std::vector<Node> children;
};

Result<Node> build_tree(std::vector<ContentLine> lines, const Limits& limits) {
  Node root;
  std::vector<Node*> stack{&root};
  std::uint64_t components = 0;
  for (ContentLine& line : lines) {
    if (line.name == "BEGIN") {
      if (stack.size() > max_component_depth) return fail(Status::Limit, "calendar components are nested too deeply");
      if (++components > limits.max_archive_entries) {
        return fail(Status::Limit, "more than {} calendar components", limits.max_archive_entries);
      }
      Node child;
      child.name = util::to_upper(util::trim(line.value));
      stack.back()->children.push_back(std::move(child));
      stack.push_back(&stack.back()->children.back());
      continue;
    }
    if (line.name == "END") {
      const std::string name = util::to_upper(util::trim(line.value));
      // Close up to the matching component; ignore stray END lines.
      for (std::size_t depth = stack.size(); depth > 1; --depth) {
        if (stack[depth - 1]->name == name) {
          stack.resize(depth - 1);
          break;
        }
      }
      continue;
    }
    stack.back()->props.push_back(std::move(line));
  }
  return root;
}

/// Properties whose values are TEXT (escaped on the wire).
bool is_text_property(std::string_view name) {
  static constexpr std::array<std::string_view, 16> text = {
      "SUMMARY", "DESCRIPTION", "LOCATION", "COMMENT",   "CONTACT", "RESOURCES",   "CATEGORIES",    "TZNAME",
      "NAME",    "UID",         "PRODID",   "RELATED-TO", "CLASS",  "X-WR-CALNAME", "X-WR-CALDESC", "COLOR"};
  if (std::ranges::find(text, name) != text.end()) return true;
  return name.starts_with("X-") && name != "X-WR-TIMEZONE" && name != "X-MICROSOFT-CDO-ALLDAYEVENT";
}
bool is_list_property(std::string_view name) { return name == "CATEGORIES" || name == "RESOURCES"; }

ir::Property to_property(const ContentLine& line) {
  ir::Property out;
  out.name = line.group.empty() ? line.name : line.group + "." + line.name;
  out.params = line.params;
  out.value = is_text_property(line.name) ? (is_list_property(line.name) ? line.value : cl::unescape(line.value))
                                          : line.value;
  return out;
}

std::string property_line(const ir::Property& property) {
  const std::string value = is_text_property(property.name) && !is_list_property(property.name)
                                ? cl::escape(property.value)
                                : property.value;
  return cl::format_line(property.name, property.params, value);
}

ir::CalendarComponent to_component(const Node& node) {
  ir::CalendarComponent out;
  out.name = node.name;
  for (const ContentLine& line : node.props) out.properties.push_back(to_property(line));
  for (const Node& child : node.children) out.children.push_back(to_component(child));
  return out;
}

void write_component(std::string& out, const ir::CalendarComponent& component) {
  out += "BEGIN:" + component.name + "\r\n";
  for (const ir::Property& property : component.properties) out += property_line(property);
  for (const ir::CalendarComponent& child : component.children) write_component(out, child);
  out += "END:" + component.name + "\r\n";
}

/// "Name <mailto:addr>" from a CAL-ADDRESS and its CN.
std::string person(const ContentLine& line) {
  const std::string cn = cl::param(line, "CN");
  const std::string address(util::trim(line.value));
  if (cn.empty()) return address;
  return cn + " <" + address + ">";
}

/// CN and address back from "Name <mailto:addr>".
std::pair<std::string, std::string> split_person(std::string_view text) {
  const std::size_t open = text.rfind('<');
  if (open != std::string_view::npos && text.ends_with('>')) {
    return {std::string(util::trim(text.substr(0, open))), std::string(text.substr(open + 1, text.size() - open - 2))};
  }
  std::string address(util::trim(text));
  if (address.find(':') == std::string::npos && address.find('@') != std::string::npos) address = "mailto:" + address;
  return {{}, address};
}

std::optional<ir::CalendarTime> line_time(const ContentLine& line) {
  auto time = cl::parse_time(line.value, cl::param(line, "TZID"));
  if (time && util::iequals(cl::param(line, "VALUE"), "DATE") && !time->date_only) {
    time->date_only = true;
    time->hour = time->minute = time->second = 0;
    time->tzid.clear();
    time->utc = false;
  }
  return time;
}

// --------------------------------------------------------------------------- vCalendar 1.0 recurrence rules

/// Convert a vCalendar 1.0 RRULE ("W1 MO TH #10", "MP1 1+ FR #0",
/// "D2 19991231T000000Z") to RFC 5545 syntax; nullopt when not convertible.
std::optional<std::string> convert_v1_rrule(std::string_view rule) {
  std::vector<std::string_view> tokens = util::split(util::trim(rule), ' ', false);
  if (tokens.empty()) return std::nullopt;
  std::string_view head = tokens[0];
  std::string freq;
  std::string_view number;
  if (util::istarts_with(head, "MD")) freq = "MONTHLY", number = head.substr(2);
  else if (util::istarts_with(head, "MP")) freq = "MONTHLY", number = head.substr(2);
  else if (util::istarts_with(head, "YM")) freq = "YEARLY", number = head.substr(2);
  else if (util::istarts_with(head, "YD")) return std::nullopt;
  else if (util::istarts_with(head, "D")) freq = "DAILY", number = head.substr(1);
  else if (util::istarts_with(head, "W")) freq = "WEEKLY", number = head.substr(1);
  else return std::nullopt;
  const auto interval = util::parse_int(number);
  if (!interval || *interval < 1 || *interval > 1000) return std::nullopt;
  std::string out = "FREQ=" + freq;
  if (*interval != 1) out += std::format(";INTERVAL={}", *interval);
  std::vector<std::string> by;
  std::string end = ";COUNT=2";  // the vCalendar default duration is two occurrences
  std::string pending_ordinal;
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    const std::string_view token = tokens[i];
    if (token.starts_with('#')) {
      const auto count = util::parse_int(token.substr(1));
      if (!count || *count < 0) return std::nullopt;
      end = *count == 0 ? std::string() : std::format(";COUNT={}", *count);
      continue;
    }
    if (token.size() >= 8 && util::is_ascii_digit(token[0])) {
      if (!cl::parse_time(token)) return std::nullopt;
      end = ";UNTIL=" + std::string(token);
      continue;
    }
    if (util::istarts_with(head, "W") || util::istarts_with(head, "MP")) {
      if (token.size() == 2 && util::is_ascii_digit(token[0]) && (token[1] == '+' || token[1] == '-')) {
        pending_ordinal = token[1] == '-' ? "-" + std::string(1, token[0]) : std::string(1, token[0]);
        continue;
      }
      if (token.size() == 2 && util::is_ascii_alpha(token[0])) {
        by.push_back(pending_ordinal + util::to_upper(token));
        continue;
      }
      return std::nullopt;
    }
    if (util::istarts_with(head, "MD")) {
      if (util::iequals(token, "LD")) {
        by.emplace_back("-1");
        continue;
      }
      std::string_view digits = token;
      bool from_end = false;
      if (digits.ends_with('-')) from_end = true, digits.remove_suffix(1);
      if (digits.ends_with('+')) digits.remove_suffix(1);
      const auto day = util::parse_int(digits);
      if (!day || *day < 1 || *day > 31) return std::nullopt;
      by.push_back(from_end ? std::format("-{}", *day) : std::format("{}", *day));
      continue;
    }
    if (util::istarts_with(head, "YM")) {
      const auto month = util::parse_int(token);
      if (!month || *month < 1 || *month > 12) return std::nullopt;
      by.push_back(std::format("{}", *month));
      continue;
    }
    if (util::istarts_with(head, "D")) return std::nullopt;
  }
  if (!by.empty()) {
    if (util::istarts_with(head, "MD")) out += ";BYMONTHDAY=";
    else if (util::istarts_with(head, "YM")) out += ";BYMONTH=";
    else out += ";BYDAY=";
    out += util::join(by, ",");
  }
  return out + end;
}

// --------------------------------------------------------------------------- reading

struct ReadMode {
  bool legacy = false;  // vCalendar 1.0
};

ir::Alarm read_alarm(const Node& node, const ir::Event& event) {
  ir::Alarm alarm;
  for (const ContentLine& line : node.props) {
    if (line.name == "ACTION") alarm.action = util::to_upper(util::trim(line.value));
    else if (line.name == "DESCRIPTION") alarm.description = cl::unescape(line.value);
    else if (line.name == "TRIGGER") {
      if (const auto duration = cl::parse_duration(line.value)) {
        std::int64_t offset = *duration;
        if (util::iequals(cl::param(line, "RELATED"), "END") && event.end) {
          offset += cl::civil_seconds(*event.end) - cl::civil_seconds(event.start);
        }
        alarm.minutes_before = static_cast<int>(std::clamp<std::int64_t>(-offset / 60, -10'000'000, 10'000'000));
      } else if (const auto time = cl::parse_time(line.value)) {
        alarm.trigger_time = *time;
        alarm.minutes_before = static_cast<int>(
            std::clamp<std::int64_t>((cl::civil_seconds(event.start) - cl::civil_seconds(*time)) / 60, -10'000'000, 10'000'000));
      }
    }
  }
  return alarm;
}

/// vCalendar 1.0 AALARM / DALARM: "runtime;snooze;repeat;text".
ir::Alarm read_v1_alarm(const ContentLine& line, const ir::Event& event) {
  ir::Alarm alarm;
  alarm.action = line.name == "AALARM" ? "AUDIO" : "DISPLAY";
  const std::vector<std::string> parts = cl::split_unescape(line.value, ';');
  if (!parts.empty()) {
    if (const auto time = cl::parse_time(parts[0])) {
      alarm.minutes_before = static_cast<int>(
          std::clamp<std::int64_t>((cl::civil_seconds(event.start) - cl::civil_seconds(*time)) / 60, -10'000'000, 10'000'000));
    }
  }
  if (parts.size() >= 4) alarm.description = parts[3];
  return alarm;
}

ir::Event read_event(const Node& node, const ReadMode& mode, const Context& ctx) {
  ir::Event event;
  event.component = node.name;
  bool has_start = false;
  std::optional<std::int64_t> duration;
  std::vector<const ContentLine*> deferred_alarms;
  for (const ContentLine& line : node.props) {
    const std::string& name = line.name;
    if (name == "UID") event.uid = cl::unescape(line.value);
    else if (name == "SUMMARY") event.summary = cl::unescape(line.value);
    else if (name == "DESCRIPTION") event.description = util::replace_all(cl::unescape(line.value), "\r\n", "\n");
    else if (name == "LOCATION") event.location = cl::unescape(line.value);
    else if (name == "URL") event.url = std::string(util::trim(line.value));
    else if (name == "STATUS") event.status = util::to_upper(util::trim(line.value));
    else if (name == "RRULE") {
      if (mode.legacy) {
        if (auto converted = convert_v1_rrule(line.value)) {
          event.rrule = std::move(*converted);
        } else {
          event.extra.push_back({"X-VCALENDAR-RRULE", {}, line.value});
          ctx.warn("A vCalendar 1.0 recurrence rule could not be converted and was kept as text.");
        }
      } else {
        event.rrule = std::string(util::trim(line.value));
      }
    } else if (name == "ORGANIZER") event.organizer = person(line);
    else if (name == "ATTENDEE") event.attendees.push_back(person(line));
    else if (name == "CATEGORIES") {
      for (std::string& category : cl::split_unescape(line.value, mode.legacy ? ';' : ',')) {
        const std::string_view trimmed = util::trim(category);
        if (!trimmed.empty()) event.categories.emplace_back(trimmed);
      }
    } else if (name == "DTSTART" || name == "DTEND" || name == "DUE" || name == "CREATED" ||
               name == "LAST-MODIFIED" || name == "RECURRENCE-ID" || (mode.legacy && name == "DCREATED")) {
      const auto time = line_time(line);
      if (!time) {
        ctx.warn(std::format("An unreadable {} value was kept as an extra property.", name));
        event.extra.push_back(to_property(line));
        continue;
      }
      if (name == "DTSTART") event.start = *time, has_start = true;
      else if (name == "DTEND") event.end = *time;
      else if (name == "DUE") event.due = *time;
      else if (name == "CREATED" || name == "DCREATED") event.created = *time;
      else if (name == "LAST-MODIFIED") event.last_modified = *time;
      else event.recurrence_id = *time;
    } else if ((name == "EXDATE" || name == "RDATE") && !util::iequals(cl::param(line, "VALUE"), "PERIOD")) {
      bool ok = true;
      std::vector<ir::CalendarTime> times;
      for (const std::string& piece : cl::split_raw(line.value, mode.legacy ? ';' : ',')) {
        ContentLine single = line;
        single.value = piece;
        const auto time = line_time(single);
        if (!time) {
          ok = false;
          break;
        }
        times.push_back(*time);
      }
      if (!ok) {
        event.extra.push_back(to_property(line));
        continue;
      }
      auto& target = name == "EXDATE" ? event.exdates : event.rdates;
      target.insert(target.end(), times.begin(), times.end());
    } else if (name == "DURATION") {
      duration = cl::parse_duration(line.value);
      if (!duration) event.extra.push_back(to_property(line));
    } else if (mode.legacy && (name == "AALARM" || name == "DALARM")) {
      deferred_alarms.push_back(&line);
    } else {
      event.extra.push_back(to_property(line));
    }
  }
  if (!has_start) {
    // VTODO / VJOURNAL may lack DTSTART: anchor them at the due or creation time.
    if (event.due) event.start = *event.due;
    else if (event.created) event.start = *event.created;
    event.extra.push_back({"X-CONVERTIBLE-NO-DTSTART", {}, "TRUE"});
  }
  if (duration && !event.end && event.component != "VTODO") {
    ir::CalendarTime end = cl::time_from_seconds(cl::civil_seconds(event.start) + *duration, event.start);
    event.end = end;
  } else if (duration) {
    event.extra.push_back({"DURATION", {}, cl::format_duration(*duration)});
  }
  for (const ContentLine* line : deferred_alarms) event.alarms.push_back(read_v1_alarm(*line, event));
  for (const Node& child : node.children) {
    if (child.name == "VALARM") event.alarms.push_back(read_alarm(child, event));
    else event.components.push_back(to_component(child));
  }
  return event;
}

// --------------------------------------------------------------------------- recurrence expansion

struct Rule {
  std::string freq;
  std::int64_t interval = 1;
  std::optional<std::int64_t> count;
  std::optional<ir::CalendarTime> until;
  std::vector<std::pair<int, int>> byday;  // (ordinal or 0, weekday 0=SU..6=SA)
  std::vector<int> bymonthday;
  std::vector<int> bymonth;
  std::vector<int> bysetpos;
  int wkst = 1;  // MO
  bool unsupported = false;
};

int weekday_index(std::string_view code) {
  static constexpr std::array<std::string_view, 7> codes = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
  for (std::size_t i = 0; i < codes.size(); ++i) {
    if (util::iequals(code, codes[i])) return static_cast<int>(i);
  }
  return -1;
}

std::optional<Rule> parse_rule(std::string_view text) {
  Rule rule;
  const auto int_list = [](std::string_view value, int low, int high, std::vector<int>& out) {
    for (const std::string_view piece : util::split(value, ',', false)) {
      const auto parsed = util::parse_int(util::trim(piece));
      if (!parsed || *parsed < low || *parsed > high || *parsed == 0) return false;
      out.push_back(static_cast<int>(*parsed));
    }
    return true;
  };
  for (const std::string_view part : util::split(text, ';', false)) {
    const std::size_t eq = part.find('=');
    if (eq == std::string_view::npos) return std::nullopt;
    const std::string key = util::to_upper(util::trim(part.substr(0, eq)));
    const std::string_view value = util::trim(part.substr(eq + 1));
    if (key == "FREQ") rule.freq = util::to_upper(value);
    else if (key == "INTERVAL") {
      const auto n = util::parse_int(value);
      if (!n || *n < 1 || *n > 100000) return std::nullopt;
      rule.interval = *n;
    } else if (key == "COUNT") {
      const auto n = util::parse_int(value);
      if (!n || *n < 0) return std::nullopt;
      rule.count = *n;
    } else if (key == "UNTIL") {
      rule.until = cl::parse_time(value);
      if (!rule.until) return std::nullopt;
    } else if (key == "BYDAY") {
      for (const std::string_view piece : util::split(value, ',', false)) {
        const std::string_view token = util::trim(piece);
        if (token.size() < 2) return std::nullopt;
        const int weekday = weekday_index(token.substr(token.size() - 2));
        if (weekday < 0) return std::nullopt;
        int ordinal = 0;
        if (token.size() > 2) {
          const auto n = util::parse_int(token.substr(0, token.size() - 2));
          if (!n || *n == 0 || *n < -53 || *n > 53) return std::nullopt;
          ordinal = static_cast<int>(*n);
        }
        rule.byday.emplace_back(ordinal, weekday);
      }
    } else if (key == "BYMONTHDAY") {
      if (!int_list(value, -31, 31, rule.bymonthday)) return std::nullopt;
    } else if (key == "BYMONTH") {
      if (!int_list(value, 1, 12, rule.bymonth)) return std::nullopt;
    } else if (key == "BYSETPOS") {
      if (!int_list(value, -366, 366, rule.bysetpos)) return std::nullopt;
    } else if (key == "WKST") {
      rule.wkst = weekday_index(value);
      if (rule.wkst < 0) return std::nullopt;
    } else {
      rule.unsupported = true;  // BYHOUR, BYMINUTE, BYWEEKNO, BYYEARDAY, ...
    }
  }
  if (rule.freq != "DAILY" && rule.freq != "WEEKLY" && rule.freq != "MONTHLY" && rule.freq != "YEARLY") {
    return std::nullopt;
  }
  return rule;
}

/// Matching days (days since epoch) of one month for BYMONTHDAY / BYDAY.
std::vector<std::int64_t> month_days(const Rule& rule, std::int64_t year, unsigned month, unsigned default_day) {
  const unsigned length = days_in_month(year, month);
  const std::int64_t first = days_from_civil(year, month, 1);
  std::vector<std::int64_t> by_monthday;
  for (const int d : rule.bymonthday) {
    const int day = d > 0 ? d : static_cast<int>(length) + d + 1;
    if (day >= 1 && day <= static_cast<int>(length)) by_monthday.push_back(first + day - 1);
  }
  std::vector<std::int64_t> by_day;
  for (const auto& [ordinal, weekday] : rule.byday) {
    std::vector<std::int64_t> matches;
    for (unsigned d = 0; d < length; ++d) {
      if (weekday_from_days(first + d) == weekday) matches.push_back(first + d);
    }
    if (ordinal == 0) {
      by_day.insert(by_day.end(), matches.begin(), matches.end());
    } else {
      const int index = ordinal > 0 ? ordinal - 1 : static_cast<int>(matches.size()) + ordinal;
      if (index >= 0 && index < static_cast<int>(matches.size())) by_day.push_back(matches[static_cast<std::size_t>(index)]);
    }
  }
  std::vector<std::int64_t> out;
  if (!rule.bymonthday.empty() && !rule.byday.empty()) {
    for (const std::int64_t day : by_monthday) {
      if (std::ranges::find(by_day, day) != by_day.end()) out.push_back(day);
    }
  } else if (!rule.bymonthday.empty()) {
    out = std::move(by_monthday);
  } else if (!rule.byday.empty()) {
    out = std::move(by_day);
  } else if (default_day <= length) {
    out.push_back(first + default_day - 1);
  }
  return out;
}

bool month_allowed(const Rule& rule, std::int64_t day) {
  if (rule.bymonth.empty()) return true;
  const CivilDate date = civil_from_days(day);
  return std::ranges::find(rule.bymonth, static_cast<int>(date.month)) != rule.bymonth.end();
}

struct Range {
  std::optional<std::int64_t> start;  // civil seconds, inclusive
  std::optional<std::int64_t> end;    // civil seconds, exclusive
};

/// Occurrence start times (civil seconds) of a recurring event.
std::vector<std::int64_t> occurrences(const ir::Event& event, const Rule& rule, const Range& range,
                                      std::size_t max_occurrences, bool& truncated) {
  const std::int64_t start = cl::civil_seconds(event.start);
  const std::int64_t day0 = start >= 0 ? start / 86400 : (start - 86399) / 86400;
  const std::int64_t time_of_day = start - day0 * 86400;
  const CivilDate date0 = civil_from_days(day0);
  std::optional<std::int64_t> until;
  if (rule.until) until = cl::civil_seconds(*rule.until) + (rule.until->date_only ? 86399 : 0);
  std::vector<std::int64_t> out{start};
  std::int64_t emitted = 1;
  constexpr std::int64_t max_periods = 200'000;
  for (std::int64_t k = 0; k < max_periods; ++k) {
    std::vector<std::int64_t> days;
    if (rule.freq == "DAILY") {
      const std::int64_t day = day0 + k * rule.interval;
      bool ok = month_allowed(rule, day);
      const CivilDate date = civil_from_days(day);
      if (ok && !rule.bymonthday.empty()) {
        const auto length = static_cast<int>(days_in_month(date.year, date.month));
        ok = std::ranges::any_of(rule.bymonthday, [&](int d) {
          return (d > 0 ? d : length + d + 1) == static_cast<int>(date.day);
        });
      }
      if (ok && !rule.byday.empty()) {
        ok = std::ranges::any_of(rule.byday, [&](const auto& b) { return b.second == weekday_from_days(day); });
      }
      if (ok) days.push_back(day);
    } else if (rule.freq == "WEEKLY") {
      const std::int64_t week0 = day0 - ((weekday_from_days(day0) - rule.wkst + 7) % 7);
      const std::int64_t week = week0 + 7 * rule.interval * k;
      if (rule.byday.empty()) {
        days.push_back(week + (weekday_from_days(day0) - rule.wkst + 7) % 7);
      } else {
        for (const auto& b : rule.byday) days.push_back(week + (b.second - rule.wkst + 7) % 7);
      }
      std::erase_if(days, [&](std::int64_t d) { return !month_allowed(rule, d); });
    } else if (rule.freq == "MONTHLY") {
      const std::int64_t index = (date0.year * 12 + date0.month - 1) + k * rule.interval;
      const std::int64_t year = index / 12;
      const auto month = static_cast<unsigned>(index % 12 + 1);
      days = month_days(rule, year, month, date0.day);
      std::erase_if(days, [&](std::int64_t d) { return !month_allowed(rule, d); });
    } else {  // YEARLY
      const std::int64_t year = date0.year + k * rule.interval;
      if (year > 9999) break;
      const bool year_scope = rule.bymonth.empty() && rule.bymonthday.empty() && !rule.byday.empty();
      if (year_scope) {
        const std::int64_t first = days_from_civil(year, 1, 1);
        const std::int64_t length = is_leap_year(year) ? 366 : 365;
        for (const auto& [ordinal, weekday] : rule.byday) {
          std::vector<std::int64_t> matches;
          for (std::int64_t d = 0; d < length; ++d) {
            if (weekday_from_days(first + d) == weekday) matches.push_back(first + d);
          }
          if (ordinal == 0) {
            days.insert(days.end(), matches.begin(), matches.end());
          } else {
            const int index = ordinal > 0 ? ordinal - 1 : static_cast<int>(matches.size()) + ordinal;
            if (index >= 0 && index < static_cast<int>(matches.size())) days.push_back(matches[static_cast<std::size_t>(index)]);
          }
        }
      } else {
        std::vector<int> months = rule.bymonth;
        if (months.empty()) months.push_back(static_cast<int>(date0.month));
        for (const int month : months) {
          const std::vector<std::int64_t> in_month = month_days(rule, year, static_cast<unsigned>(month), date0.day);
          days.insert(days.end(), in_month.begin(), in_month.end());
        }
      }
    }
    std::ranges::sort(days);
    days.erase(std::unique(days.begin(), days.end()), days.end());
    if (!rule.bysetpos.empty()) {
      std::vector<std::int64_t> selected;
      for (const int position : rule.bysetpos) {
        const int index = position > 0 ? position - 1 : static_cast<int>(days.size()) + position;
        if (index >= 0 && index < static_cast<int>(days.size())) selected.push_back(days[static_cast<std::size_t>(index)]);
      }
      std::ranges::sort(selected);
      selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
      days = std::move(selected);
    }
    bool past_end = false;
    for (const std::int64_t day : days) {
      const std::int64_t when = day * 86400 + time_of_day;
      if (when <= start) continue;
      if ((until && when > *until) || (rule.count && emitted >= *rule.count)) {
        past_end = true;
        break;
      }
      if (range.end && when >= *range.end) {
        past_end = true;
        break;
      }
      if (out.size() >= max_occurrences) {
        truncated = true;
        past_end = true;
        break;
      }
      out.push_back(when);
      ++emitted;
    }
    if (past_end) break;
    // Stop scanning once the period starts beyond every bound we know of.
    if (k > 0 && days.empty() && rule.freq == "YEARLY" && date0.year + k * rule.interval > 9999) break;
  }
  return out;
}

Result<std::optional<std::int64_t>> range_bound(const Options& options, std::string_view key) {
  const std::string value = options.get_string(key, "");
  if (util::trim(value).empty()) return std::optional<std::int64_t>{};
  const auto time = cl::parse_time(value);
  if (!time) return fail(Status::InvalidArgument, "option '{}' must be an ISO 8601 date or date-time", key);
  return std::optional<std::int64_t>(cl::civil_seconds(*time));
}

Result<void> expand_recurrences(ir::Calendar& calendar, const ReadContext& ctx) {
  Range range;
  CVT_TRY_ASSIGN(range.start, range_bound(ctx.options, "range_start"));
  CVT_TRY_ASSIGN(range.end, range_bound(ctx.options, "range_end"));
  const auto max_occurrences = static_cast<std::size_t>(ctx.options.get_int("max_occurrences", 1000, 1, 100000));

  // Overridden occurrences: (uid, recurrence-id) pairs present in the source.
  std::set<std::pair<std::string, std::int64_t>> overrides;
  for (const ir::Event& event : calendar.events) {
    if (event.recurrence_id) overrides.emplace(event.uid, cl::civil_seconds(*event.recurrence_id));
  }
  std::vector<ir::Event> out;
  bool truncated = false;
  bool unsupported = false;
  for (std::size_t i = 0; i < calendar.events.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(calendar.events.size()), "Expanding"));
    const ir::Event& event = calendar.events[i];
    const std::int64_t length = event.end ? cl::civil_seconds(*event.end) - cl::civil_seconds(event.start) : 0;
    const auto in_range = [&](std::int64_t when) {
      const std::int64_t finish = when + std::max<std::int64_t>(length, 0);
      if (range.end && when >= *range.end) return false;
      if (range.start && finish <= *range.start && !(length == 0 && when == *range.start)) return false;
      return true;
    };
    if (event.rrule.empty() && event.rdates.empty()) {
      if (in_range(cl::civil_seconds(event.start))) out.push_back(event);
      continue;
    }
    std::vector<std::int64_t> times;
    if (!event.rrule.empty()) {
      const auto rule = parse_rule(event.rrule);
      if (!rule) {
        ctx.warn("A recurrence rule could not be read; the event was kept unexpanded.");
        out.push_back(event);
        continue;
      }
      unsupported = unsupported || rule->unsupported;
      times = occurrences(event, *rule, range, max_occurrences, truncated);
    } else {
      times.push_back(cl::civil_seconds(event.start));
    }
    for (const ir::CalendarTime& rdate : event.rdates) times.push_back(cl::civil_seconds(rdate));
    std::ranges::sort(times);
    times.erase(std::unique(times.begin(), times.end()), times.end());
    std::erase_if(times, [&](std::int64_t when) {
      return std::ranges::any_of(event.exdates, [&](const ir::CalendarTime& ex) {
        const std::int64_t value = cl::civil_seconds(ex);
        return ex.date_only ? (when >= value && when < value + 86400) : value == when;
      });
    });
    for (const std::int64_t when : times) {
      if (!in_range(when) || overrides.contains({event.uid, when})) continue;
      ir::Event occurrence = event;
      occurrence.rrule.clear();
      occurrence.exdates.clear();
      occurrence.rdates.clear();
      occurrence.start = cl::time_from_seconds(when, event.start);
      if (event.end) occurrence.end = cl::time_from_seconds(when + length, *event.end);
      occurrence.recurrence_id = occurrence.start;
      out.push_back(std::move(occurrence));
    }
  }
  if (truncated) ctx.warn(std::format("Recurrences were limited to {} occurrences per event.", max_occurrences));
  if (unsupported) ctx.warn("Some recurrence rule parts (BYHOUR, BYWEEKNO, BYYEARDAY, ...) are not expanded and were ignored.");
  std::stable_sort(out.begin(), out.end(), [](const ir::Event& a, const ir::Event& b) {
    return cl::civil_seconds(a.start) < cl::civil_seconds(b.start);
  });
  calendar.events = std::move(out);
  return {};
}

Result<AnyIR> read_calendar(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, decode_text(ctx.input, ctx.options.get_string("source_encoding", "auto")));
  const bool legacy_hint = ctx.format_id == "vcs";
  CVT_TRY_ASSIGN(std::vector<ContentLine> probe, cl::parse_lines(text, legacy_hint, ctx.limits));
  // A file announcing VERSION:1.0 is vCalendar whatever its extension.
  bool legacy = legacy_hint;
  for (const ContentLine& line : probe) {
    if (line.name == "VERSION") {
      legacy = util::trim(line.value) == "1.0";
      break;
    }
  }
  if (legacy != legacy_hint) {
    CVT_TRY_ASSIGN(probe, cl::parse_lines(text, legacy, ctx.limits));
  }
  CVT_TRY_ASSIGN(Node root, build_tree(std::move(probe), ctx.limits));
  const Node* vcalendar = nullptr;
  for (const Node& child : root.children) {
    if (child.name == "VCALENDAR") {
      vcalendar = &child;
      break;
    }
  }
  if (vcalendar == nullptr) return fail(Status::Decode, "no BEGIN:VCALENDAR found; this is not an iCalendar file");
  ir::Calendar calendar;
  const ReadMode mode{legacy};
  // Several VCALENDAR objects in one file are merged.
  for (const Node& cal : root.children) {
    if (cal.name != "VCALENDAR") continue;
    for (const ContentLine& line : cal.props) {
      if (line.name == "X-WR-CALNAME" && calendar.name.empty()) calendar.name = cl::unescape(line.value);
      else if (line.name == "VERSION") continue;
      else if (std::ranges::none_of(calendar.extra, [&](const ir::Property& p) { return p.name == line.name; }) ||
               line.name.starts_with("X-")) {
        calendar.extra.push_back(to_property(line));
      }
    }
    for (std::size_t i = 0; i < cal.children.size(); ++i) {
      const Node& child = cal.children[i];
      if ((i & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(cal.children.size())));
      if (child.name == "VEVENT" || child.name == "VTODO" || child.name == "VJOURNAL" || child.name == "VFREEBUSY") {
        calendar.events.push_back(read_event(child, mode, ctx));
      } else if (child.name == "VTIMEZONE") {
        calendar.timezones.push_back(to_component(child));
      } else {
        calendar.other_components.push_back(to_component(child));
      }
    }
  }
  if (ctx.options.get_bool("expand_recurrences", false)) CVT_TRY(expand_recurrences(calendar, ctx));
  return calendar;
}

// --------------------------------------------------------------------------- writing

std::string time_line(std::string_view name, const ir::CalendarTime& time) {
  cl::Params params;
  if (time.date_only) params.emplace_back("VALUE", "DATE");
  else if (!time.utc && !time.tzid.empty()) params.emplace_back("TZID", time.tzid);
  return cl::format_line(name, params, cl::format_time(time));
}

std::string person_line(std::string_view name, std::string_view text) {
  const auto [cn, address] = split_person(text);
  cl::Params params;
  if (!cn.empty()) params.emplace_back("CN", cn);
  return cl::format_line(name, params, address);
}

std::string generated_uid(const ir::Event& event, std::size_t index) {
  const std::string seed = std::format("{}|{}|{}|{}|{}", event.summary, cl::format_time(event.start),
                                       event.description, event.location, index);
  return util::sha1_hex(as_bytes(seed)).substr(0, 32);
}

const ir::Property* find_extra(const std::vector<ir::Property>& extra, std::string_view name) {
  for (const ir::Property& property : extra) {
    if (property.name == name) return &property;
  }
  return nullptr;
}

void write_event(std::string& out, const ir::Event& event, std::size_t index) {
  const std::string component = event.component.empty() ? std::string("VEVENT") : util::to_upper(event.component);
  out += "BEGIN:" + component + "\r\n";
  out += cl::format_line("UID", {}, cl::escape(event.uid.empty() ? generated_uid(event, index) : event.uid));
  if (find_extra(event.extra, "DTSTAMP") == nullptr) {
    // DTSTAMP is required; never invent "now": reuse a time from the event itself.
    ir::CalendarTime stamp = event.last_modified ? *event.last_modified : event.created ? *event.created : event.start;
    stamp.date_only = false;
    stamp.utc = true;
    stamp.tzid.clear();
    out += cl::format_line("DTSTAMP", {}, cl::format_time(stamp));
  }
  const bool no_start = find_extra(event.extra, "X-CONVERTIBLE-NO-DTSTART") != nullptr;
  if (!no_start) out += time_line("DTSTART", event.start);
  if (event.end) out += time_line(component == "VTODO" ? "DUE" : "DTEND", *event.end);
  if (event.due && !(event.end && component == "VTODO")) out += time_line("DUE", *event.due);
  if (event.recurrence_id) out += time_line("RECURRENCE-ID", *event.recurrence_id);
  if (!event.summary.empty()) out += cl::format_line("SUMMARY", {}, cl::escape(event.summary));
  if (!event.description.empty()) out += cl::format_line("DESCRIPTION", {}, cl::escape(event.description));
  if (!event.location.empty()) out += cl::format_line("LOCATION", {}, cl::escape(event.location));
  if (!event.rrule.empty()) out += cl::format_line("RRULE", {}, event.rrule);
  for (const ir::CalendarTime& time : event.exdates) out += time_line("EXDATE", time);
  for (const ir::CalendarTime& time : event.rdates) out += time_line("RDATE", time);
  if (!event.organizer.empty()) out += person_line("ORGANIZER", event.organizer);
  for (const std::string& attendee : event.attendees) out += person_line("ATTENDEE", attendee);
  if (!event.categories.empty()) {
    std::vector<std::string> escaped;
    for (const std::string& category : event.categories) escaped.push_back(cl::escape(category));
    out += cl::format_line("CATEGORIES", {}, util::join(escaped, ","));
  }
  if (!event.status.empty()) out += cl::format_line("STATUS", {}, event.status);
  if (!event.url.empty()) out += cl::format_line("URL", {}, event.url);
  if (event.created) out += cl::format_line("CREATED", {}, cl::format_time(*event.created));
  if (event.last_modified) out += cl::format_line("LAST-MODIFIED", {}, cl::format_time(*event.last_modified));
  for (const ir::Property& property : event.extra) {
    if (property.name == "X-CONVERTIBLE-NO-DTSTART") continue;
    out += property_line(property);
  }
  for (const ir::Alarm& alarm : event.alarms) {
    out += "BEGIN:VALARM\r\n";
    const std::string action = alarm.action.empty() ? std::string("DISPLAY") : util::to_upper(alarm.action);
    out += cl::format_line("ACTION", {}, action);
    if (alarm.trigger_time) {
      out += cl::format_line("TRIGGER", {{"VALUE", "DATE-TIME"}}, cl::format_time(*alarm.trigger_time));
    } else {
      out += cl::format_line("TRIGGER", {}, cl::format_duration(-static_cast<std::int64_t>(alarm.minutes_before) * 60));
    }
    std::string description = alarm.description;
    if (description.empty() && action == "DISPLAY") description = event.summary.empty() ? "Reminder" : event.summary;
    if (!description.empty()) out += cl::format_line("DESCRIPTION", {}, cl::escape(description));
    out += "END:VALARM\r\n";
  }
  for (const ir::CalendarComponent& child : event.components) write_component(out, child);
  out += "END:" + component + "\r\n";
}

Result<Bytes> write_calendar(const AnyIR& value, WriteContext& ctx) {
  const ir::Calendar& calendar = value.get<ir::Calendar>();
  const bool strip = options::resolved_strip_metadata(ctx.options);
  std::string out = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n";
  const ir::Property* prodid = find_extra(calendar.extra, "PRODID");
  if (prodid != nullptr && !strip) out += property_line(*prodid);
  else out += "PRODID:-//Convertible//Calendar//EN\r\n";
  if (!calendar.name.empty()) out += cl::format_line("X-WR-CALNAME", {}, cl::escape(calendar.name));
  for (const ir::Property& property : calendar.extra) {
    if (property.name == "PRODID" || property.name == "VERSION" || property.name == "X-WR-CALNAME") continue;
    out += property_line(property);
  }
  for (const ir::CalendarComponent& timezone : calendar.timezones) write_component(out, timezone);
  for (std::size_t i = 0; i < calendar.events.size(); ++i) {
    if ((i & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(calendar.events.size())));
    write_event(out, calendar.events[i], i);
  }
  for (const ir::CalendarComponent& component : calendar.other_components) write_component(out, component);
  out += "END:VCALENDAR\r\n";
  return to_bytes(out);
}

// --------------------------------------------------------------------------- detection

int sniff_ics(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_text(data, 2048);
  if (!util::istarts_with(util::trim_left(head), "BEGIN:VCALENDAR")) return 0;
  if (head.find("VERSION:1.0") != std::string::npos) return 0;
  return 90;
}

int sniff_ifb(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_text(data, 4096);
  if (!util::istarts_with(util::trim_left(head), "BEGIN:VCALENDAR")) return 0;
  if (head.find("BEGIN:VFREEBUSY") == std::string::npos || head.find("BEGIN:VEVENT") != std::string::npos) return 0;
  return 92;
}

int sniff_vcs(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_text(data, 2048);
  if (!util::istarts_with(util::trim_left(head), "BEGIN:VCALENDAR")) return 0;
  return head.find("VERSION:1.0") != std::string::npos ? 95 : 0;
}

std::vector<OptionSpec> calendar_read_options() {
  return {
      source_encoding_option(),
      OptionSpec::boolean("expand_recurrences", "Expand recurring events", false,
                          "Replace each recurring event by its single occurrences (RRULE, RDATE and EXDATE)."),
      OptionSpec::text("range_start", "Range start", "",
                       "With expansion: drop occurrences that end before this ISO 8601 date or date-time."),
      OptionSpec::text("range_end", "Range end", "",
                       "With expansion: drop occurrences that start at or after this ISO 8601 date or date-time."),
      OptionSpec::integer("max_occurrences", "Maximum occurrences", 1000, 1, 100000,
                          "With expansion: most occurrences generated per recurring event.")
          .advanced(),
  };
}

}  // namespace

void register_calendars(Registry& registry) {
  FormatInfo ics;
  ics.id = "ics";
  ics.name = "iCalendar";
  ics.description = "Calendar events, tasks and journal entries (RFC 5545).";
  ics.category = Category::Pim;
  ics.extensions = {"ics", "ical", "icalendar"};
  ics.mime = "text/calendar";
  ics.popularity = 80;
  ics.sniff = sniff_ics;
  registry.add_format(std::move(ics));

  FormatInfo ifb;
  ifb.id = "ifb";
  ifb.name = "iCalendar free/busy";
  ifb.description = "Free/busy time published as an iCalendar VFREEBUSY object.";
  ifb.category = Category::Pim;
  ifb.extensions = {"ifb"};
  ifb.mime = "text/calendar";
  ifb.popularity = 15;
  ifb.sniff = sniff_ifb;
  registry.add_format(std::move(ifb));

  FormatInfo vcs;
  vcs.id = "vcs";
  vcs.name = "vCalendar 1.0";
  vcs.description = "The predecessor of iCalendar, still exported by some phones.";
  vcs.category = Category::Pim;
  vcs.extensions = {"vcs"};
  vcs.mime = "text/x-vcalendar";
  vcs.popularity = 25;
  vcs.sniff = sniff_vcs;
  registry.add_format(std::move(vcs));

  for (const char* id : {"ics", "ifb"}) {
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Calendar;
    codec.read = read_calendar;
    codec.write = write_calendar;
    codec.read_options = calendar_read_options();
    codec.write_options = {options::strip_metadata()};
    registry.add_codec(std::move(codec));
  }
  Codec vcs_codec;
  vcs_codec.format_id = "vcs";
  vcs_codec.kind = Kind::Calendar;
  vcs_codec.read = read_calendar;
  vcs_codec.read_options = calendar_read_options();
  registry.add_codec(std::move(vcs_codec));

  register_calendar_csv(registry);
}

}  // namespace cvt::pim
