// Convertible core - pim family: calendar CSV dialects of Google Calendar and Outlook.
//
// Google Calendar imports "Subject, Start Date, Start Time, End Date, End
// Time, All Day Event, Description, Location, Private" with US dates
// (MM/DD/YYYY) and 12-hour times; all-day end dates are inclusive. Outlook
// exports a longer column set ("Reminder on/off", "Categories", "Meeting
// Organizer", ...) whose all-day end date is exclusive. Neither carries time
// zones: times are written as the wall-clock times of the events.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/pim/content_line.hpp"
#include "families/pim/pim.hpp"
#include "util/text.hpp"

namespace cvt::pim {

namespace {

enum class Dialect { Google, Outlook };

Dialect dialect_of(std::string_view format_id) {
  return format_id == "outlook-calendar-csv" ? Dialect::Outlook : Dialect::Google;
}

/// Index of a header (case-insensitive, trimmed), or npos.
std::size_t column(const std::vector<std::string>& header, std::string_view name) {
  for (std::size_t i = 0; i < header.size(); ++i) {
    if (util::iequals(util::trim(header[i]), name)) return i;
  }
  return std::string::npos;
}

std::string cell(const std::vector<std::string>& row, std::size_t index) {
  return index < row.size() ? std::string(util::trim(row[index])) : std::string();
}

/// "05/01/2024", "5/1/24", "2024-05-01", "01.05.2024" -> days since epoch.
std::optional<std::int64_t> parse_date(std::string_view text) {
  text = util::trim(text);
  if (text.empty()) return std::nullopt;
  char separator = 0;
  for (const char c : text) {
    if (c == '/' || c == '-' || c == '.') {
      separator = c;
      break;
    }
  }
  if (separator == 0) return std::nullopt;
  const std::vector<std::string_view> parts = util::split(text, separator);
  if (parts.size() != 3) return std::nullopt;
  std::optional<std::int64_t> a = util::parse_int(util::trim(parts[0]));
  std::optional<std::int64_t> b = util::parse_int(util::trim(parts[1]));
  std::optional<std::int64_t> c = util::parse_int(util::trim(parts[2]));
  if (!a || !b || !c) return std::nullopt;
  std::int64_t year = 0;
  std::int64_t month = 0;
  std::int64_t day = 0;
  if (util::trim(parts[0]).size() == 4) year = *a, month = *b, day = *c;
  else if (separator == '.') day = *a, month = *b, year = *c;
  else month = *a, day = *b, year = *c;
  if (year < 100) year += year < 70 ? 2000 : 1900;
  if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, static_cast<unsigned>(month))) return std::nullopt;
  return days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
}

/// "10:00 AM", "10:00:00 PM", "22:30" -> seconds after midnight.
std::optional<std::int64_t> parse_clock_time(std::string_view text) {
  text = util::trim(text);
  if (text.empty()) return std::nullopt;
  bool pm = false;
  bool am = false;
  if (util::iends_with(text, "PM")) pm = true, text = util::trim(text.substr(0, text.size() - 2));
  else if (util::iends_with(text, "AM")) am = true, text = util::trim(text.substr(0, text.size() - 2));
  const std::vector<std::string_view> parts = util::split(text, ':');
  if (parts.size() < 2 || parts.size() > 3) return std::nullopt;
  const auto h = util::parse_int(parts[0]);
  const auto m = util::parse_int(parts[1]);
  const auto s = parts.size() == 3 ? util::parse_int(parts[2]) : std::optional<std::int64_t>(0);
  if (!h || !m || !s || *h < 0 || *h > 23 || *m < 0 || *m > 59 || *s < 0 || *s > 59) return std::nullopt;
  std::int64_t hour = *h;
  if ((am || pm) && (hour < 1 || hour > 12)) return std::nullopt;
  if (pm && hour != 12) hour += 12;
  if (am && hour == 12) hour = 0;
  return hour * 3600 + *m * 60 + *s;
}

bool truthy(std::string_view text) {
  return util::iequals(util::trim(text), "true") || util::iequals(util::trim(text), "yes") || util::trim(text) == "1";
}

std::string format_date(std::int64_t days) {
  const CivilDate date = civil_from_days(days);
  return std::format("{:02}/{:02}/{:04}", date.month, date.day, date.year);
}

std::string format_clock(std::int64_t seconds, bool with_seconds) {
  const std::int64_t hour = seconds / 3600;
  const std::int64_t minute = (seconds / 60) % 60;
  const std::int64_t hour12 = hour % 12 == 0 ? 12 : hour % 12;
  if (with_seconds) return std::format("{}:{:02}:{:02} {}", hour12, minute, seconds % 60, hour < 12 ? "AM" : "PM");
  return std::format("{:02}:{:02} {}", hour12, minute, hour < 12 ? "AM" : "PM");
}

ir::CalendarTime make_time(std::int64_t days, std::optional<std::int64_t> seconds, bool all_day) {
  ir::CalendarTime like;
  like.date_only = all_day;
  return cl::time_from_seconds(days * 86400 + (all_day ? 0 : seconds.value_or(0)), like);
}

std::int64_t day_of(const ir::CalendarTime& time) {
  const std::int64_t seconds = cl::civil_seconds(time);
  return seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
}

Result<AnyIR> read_calendar_csv(ReadContext& ctx) {
  const Dialect dialect = dialect_of(ctx.format_id);
  CVT_TRY_ASSIGN(const std::string text, decode_text(ctx.input, ctx.options.get_string("source_encoding", "auto")));
  std::vector<std::vector<std::string>> rows = parse_csv(text);
  if (rows.empty()) return fail(Status::Decode, "the calendar CSV file is empty");
  CVT_TRY(ctx.limits.check_archive_entries(rows.size()));
  const std::vector<std::string> header = rows.front();
  const std::size_t subject = column(header, "Subject");
  const std::size_t start_date = column(header, "Start Date");
  if (subject == std::string::npos || start_date == std::string::npos) {
    return fail(Status::Decode, "calendar CSV needs 'Subject' and 'Start Date' columns");
  }
  const std::size_t start_time = column(header, "Start Time");
  const std::size_t end_date = column(header, "End Date");
  const std::size_t end_time = column(header, "End Time");
  const std::size_t all_day = column(header, "All Day Event");
  const std::size_t description = column(header, "Description");
  const std::size_t location = column(header, "Location");
  const std::size_t is_private = column(header, "Private");
  const std::size_t categories = column(header, "Categories");
  const std::size_t organizer = column(header, "Meeting Organizer");
  const std::size_t required = column(header, "Required Attendees");
  const std::size_t optional_attendees = column(header, "Optional Attendees");
  const std::size_t reminder_on = column(header, "Reminder on/off");
  const std::size_t reminder_date = column(header, "Reminder Date");
  const std::size_t reminder_time = column(header, "Reminder Time");
  const std::size_t show_as = column(header, "Show time as");

  ir::Calendar calendar;
  std::size_t skipped = 0;
  for (std::size_t r = 1; r < rows.size(); ++r) {
    const std::vector<std::string>& row = rows[r];
    if ((r & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(rows.size())));
    const auto day = parse_date(cell(row, start_date));
    if (!day) {
      ++skipped;
      continue;
    }
    ir::Event event;
    event.summary = cell(row, subject);
    const auto start_seconds = parse_clock_time(cell(row, start_time));
    const bool whole_day = truthy(cell(row, all_day)) || (!start_seconds && cell(row, start_time).empty());
    event.start = make_time(*day, start_seconds, whole_day);
    const auto end_day = parse_date(cell(row, end_date));
    const auto end_seconds = parse_clock_time(cell(row, end_time));
    if (whole_day) {
      std::int64_t last = end_day.value_or(*day);
      // Google's all-day end date is inclusive; Outlook's is exclusive.
      std::int64_t exclusive = dialect == Dialect::Google ? last + 1 : std::max(last, *day + 1);
      if (dialect == Dialect::Outlook && last == *day) exclusive = *day + 1;
      event.end = make_time(exclusive, std::nullopt, true);
    } else if (end_day || end_seconds) {
      event.end = make_time(end_day.value_or(*day), end_seconds, false);
    }
    event.description = util::replace_all(cell(row, description), "\r\n", "\n");
    event.location = cell(row, location);
    if (truthy(cell(row, is_private))) event.extra.push_back({"CLASS", {}, "PRIVATE"});
    const std::string split_text = cell(row, categories);  // split() returns views into it
    for (const std::string_view category : util::split(split_text, ';', false)) {
      if (!util::trim(category).empty()) event.categories.emplace_back(util::trim(category));
    }
    event.organizer = cell(row, organizer);
    for (const std::size_t index : {required, optional_attendees}) {
      const std::string attendee_text = cell(row, index);  // split() returns views into it
      for (const std::string_view attendee : util::split(attendee_text, ';', false)) {
        if (!util::trim(attendee).empty()) event.attendees.emplace_back(util::trim(attendee));
      }
    }
    if (truthy(cell(row, reminder_on))) {
      ir::Alarm alarm;
      const auto when_day = parse_date(cell(row, reminder_date));
      const auto when_time = parse_clock_time(cell(row, reminder_time));
      if (when_day) {
        const std::int64_t when = *when_day * 86400 + when_time.value_or(0);
        alarm.minutes_before = static_cast<int>(
            std::clamp<std::int64_t>((cl::civil_seconds(event.start) - when) / 60, -10'000'000, 10'000'000));
      }
      event.alarms.push_back(alarm);
    }
    const std::string show = cell(row, show_as);
    if (show == "1" || util::iequals(show, "free")) event.extra.push_back({"TRANSP", {}, "TRANSPARENT"});
    calendar.events.push_back(std::move(event));
  }
  if (skipped > 0) ctx.warn(std::format("{} row(s) without a readable start date were skipped.", skipped));
  return calendar;
}

Result<Bytes> write_calendar_csv(const AnyIR& value, WriteContext& ctx) {
  const ir::Calendar& calendar = value.get<ir::Calendar>();
  const Dialect dialect = dialect_of(ctx.format_id);
  std::vector<std::vector<std::string>> rows;
  if (dialect == Dialect::Google) {
    rows.push_back({"Subject", "Start Date", "Start Time", "End Date", "End Time", "All Day Event", "Description",
                    "Location", "Private"});
  } else {
    rows.push_back({"Subject", "Start Date", "Start Time", "End Date", "End Time", "All day event", "Reminder on/off",
                    "Reminder Date", "Reminder Time", "Meeting Organizer", "Required Attendees", "Optional Attendees",
                    "Meeting Resources", "Billing Information", "Categories", "Description", "Location", "Mileage",
                    "Priority", "Private", "Sensitivity", "Show time as"});
  }
  bool zones_dropped = false;
  bool recurrence_dropped = false;
  for (const ir::Event& event : calendar.events) {
    if (event.component != "VEVENT" && !event.component.empty()) continue;
    zones_dropped = zones_dropped || event.start.utc || !event.start.tzid.empty();
    recurrence_dropped = recurrence_dropped || !event.rrule.empty();
    const bool all_day = event.start.date_only;
    const std::int64_t start_day = day_of(event.start);
    const std::int64_t start_tod = cl::civil_seconds(event.start) - start_day * 86400;
    std::int64_t end_day = start_day;
    std::int64_t end_tod = start_tod;
    if (event.end) {
      end_day = day_of(*event.end);
      end_tod = cl::civil_seconds(*event.end) - end_day * 86400;
    } else if (all_day) {
      end_day = start_day + 1;
    }
    if (all_day && dialect == Dialect::Google) end_day = std::max(start_day, end_day - 1);
    const bool is_private = std::ranges::any_of(event.extra, [](const ir::Property& p) {
      return p.name == "CLASS" && (util::iequals(p.value, "PRIVATE") || util::iequals(p.value, "CONFIDENTIAL"));
    });
    const bool seconds = dialect == Dialect::Outlook;
    std::vector<std::string> row = {event.summary,
                                    format_date(start_day),
                                    all_day ? std::string() : format_clock(start_tod, seconds),
                                    format_date(end_day),
                                    all_day ? std::string() : format_clock(end_tod, seconds),
                                    all_day ? "True" : "False"};
    if (dialect == Dialect::Google) {
      row.push_back(event.description);
      row.push_back(event.location);
      row.emplace_back(is_private ? "True" : "False");
    } else {
      const bool reminder = !event.alarms.empty();
      std::string reminder_date;
      std::string reminder_time;
      if (reminder) {
        const std::int64_t when = cl::civil_seconds(event.start) - static_cast<std::int64_t>(event.alarms[0].minutes_before) * 60;
        const std::int64_t when_day = when >= 0 ? when / 86400 : (when - 86399) / 86400;
        reminder_date = format_date(when_day);
        reminder_time = format_clock(when - when_day * 86400, true);
      }
      const bool transparent = std::ranges::any_of(event.extra, [](const ir::Property& p) {
        return p.name == "TRANSP" && util::iequals(p.value, "TRANSPARENT");
      });
      row.emplace_back(reminder ? "True" : "False");
      row.push_back(reminder_date);
      row.push_back(reminder_time);
      row.push_back(event.organizer);
      row.push_back(util::join(event.attendees, ";"));
      row.emplace_back();
      row.emplace_back();
      row.emplace_back();
      row.push_back(util::join(event.categories, ";"));
      row.push_back(event.description);
      row.push_back(event.location);
      row.emplace_back();
      row.emplace_back("Normal");
      row.emplace_back(is_private ? "True" : "False");
      row.emplace_back(is_private ? "Private" : "Normal");
      row.emplace_back(transparent ? "1" : "2");
    }
    rows.push_back(std::move(row));
  }
  if (zones_dropped) ctx.warn("Calendar CSV has no time zones; times were written as the events' own clock times.");
  if (recurrence_dropped) ctx.warn("Calendar CSV cannot hold recurrence rules; only the first occurrence was written.");
  return to_bytes(write_csv(rows));
}

int sniff_calendar_csv(ByteView data, bool outlook) {
  const std::string head = sniff_text(data, 1024);
  const std::string_view first = std::string_view(head).substr(0, head.find_first_of("\r\n"));
  const std::string lower = util::to_lower(first);
  if (lower.find("subject") == std::string::npos || lower.find("start date") == std::string::npos) return 0;
  const bool has_outlook_columns = lower.find("reminder on/off") != std::string::npos;
  return has_outlook_columns == outlook ? 92 : 0;
}

}  // namespace

void register_calendar_csv(Registry& registry) {
  const auto add = [&registry](std::string id, std::string name, std::string description, int popularity, bool outlook) {
    FormatInfo info;
    info.id = id;
    info.name = std::move(name);
    info.description = std::move(description);
    info.category = Category::Pim;
    info.extensions = {"csv"};
    info.mime = "text/csv";
    info.popularity = popularity;
    info.sniff = [outlook](ByteView data, std::string_view) { return sniff_calendar_csv(data, outlook); };
    registry.add_format(std::move(info));

    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Calendar;
    codec.read = read_calendar_csv;
    codec.write = write_calendar_csv;
    codec.lossy = true;
    codec.read_cost = 15;
    codec.write_cost = 20;
    codec.read_options = {source_encoding_option()};
    registry.add_codec(std::move(codec));
  };
  add("google-calendar-csv", "Google Calendar CSV", "Events in the CSV layout Google Calendar imports.", 30, false);
  add("outlook-calendar-csv", "Outlook calendar CSV", "Events in the CSV layout Microsoft Outlook exports and imports.",
      25, true);
}

}  // namespace cvt::pim
