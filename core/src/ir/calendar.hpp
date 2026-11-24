// Convertible core - Calendar IR: events (the iCalendar VEVENT model).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ir/property.hpp"

namespace cvt::ir {

/// A calendar date or date-time, kept in its civil (wall-clock) form.
struct CalendarTime {
  int year = 1970;
  int month = 1;   ///< 1..12
  int day = 1;     ///< 1..31
  int hour = 0;    ///< 0..23
  int minute = 0;  ///< 0..59
  int second = 0;  ///< 0..60
  /// True for all-day values (DATE): the time fields are then zero and ignored.
  bool date_only = false;
  /// True when the value is in UTC ("Z" suffix).
  bool utc = false;
  /// IANA time zone name (TZID) for zoned local times; empty for UTC,
  /// all-day and floating values.
  std::string tzid;

  friend bool operator==(const CalendarTime&, const CalendarTime&) = default;
};

/// A reminder attached to an event.
struct Alarm {
  /// Minutes before the event start at which the alarm fires (negative =
  /// after the start).
  int minutes_before = 0;
  std::string description;
};

/// One event (VEVENT).
struct Event {
  std::string uid;          ///< globally unique id; writers generate one when empty
  std::string summary;      ///< title
  std::string description;  ///< long text; lines separated by '\n'
  std::string location;
  CalendarTime start;
  /// Exclusive end (DTEND); nullopt for instantaneous events. For all-day
  /// events this is the day after the last day.
  std::optional<CalendarTime> end;
  /// Recurrence rule exactly as in RRULE (e.g. "FREQ=WEEKLY;BYDAY=MO");
  /// empty for one-off events.
  std::string rrule;
  std::string organizer;               ///< "Name <mailto:addr>" or bare address
  std::vector<std::string> attendees;  ///< same form as `organizer`
  std::vector<std::string> categories;
  std::string status;  ///< "TENTATIVE", "CONFIRMED", "CANCELLED" or empty
  std::string url;
  std::optional<CalendarTime> created;
  std::optional<CalendarTime> last_modified;
  std::vector<Alarm> alarms;
  /// Properties not modelled above, preserved for round trips.
  std::vector<Property> extra;
};

/// A calendar (VCALENDAR).
struct Calendar {
  std::string name;  ///< display name (X-WR-CALNAME); may be empty
  std::vector<Event> events;
  /// Calendar-level properties not modelled above.
  std::vector<Property> extra;
};

}  // namespace cvt::ir
