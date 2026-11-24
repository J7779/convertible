// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "families/pim/content_line.hpp"
#include "ir/calendar.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace std::string_view_literals;

namespace {

const ir::Event* find_event(const ir::Calendar& calendar, std::string_view uid) {
  for (const ir::Event& event : calendar.events) {
    if (event.uid == uid) return &event;
  }
  return nullptr;
}

ir::CalendarTime at(int y, int mo, int d, int h = 0, int mi = 0, int s = 0, bool utc = false, std::string tzid = {}) {
  ir::CalendarTime t;
  t.year = y;
  t.month = mo;
  t.day = d;
  t.hour = h;
  t.minute = mi;
  t.second = s;
  t.utc = utc;
  t.tzid = std::move(tzid);
  return t;
}

ir::CalendarTime day(int y, int mo, int d) {
  ir::CalendarTime t = at(y, mo, d);
  t.date_only = true;
  return t;
}

std::vector<std::string> starts(const ir::Calendar& calendar, std::string_view uid) {
  std::vector<std::string> out;
  for (const ir::Event& event : calendar.events) {
    if (event.uid == uid) out.push_back(pim::cl::iso_time(event.start));
  }
  return out;
}

/// Field-level comparison of two calendars (order-insensitive on events, by UID).
void check_same_events(const ir::Calendar& a, const ir::Calendar& b) {
  REQUIRE(a.events.size() == b.events.size());
  for (const ir::Event& x : a.events) {
    INFO("event ", x.uid);
    const ir::Event* y = find_event(b, x.uid);
    REQUIRE(y != nullptr);
    CHECK(x.component == y->component);
    CHECK(x.summary == y->summary);
    CHECK(x.description == y->description);
    CHECK(x.location == y->location);
    CHECK(x.start == y->start);
    CHECK(x.end == y->end);
    CHECK(x.due == y->due);
    CHECK(x.rrule == y->rrule);
    CHECK(x.exdates.size() == y->exdates.size());
    CHECK(x.organizer == y->organizer);
    CHECK(x.attendees == y->attendees);
    CHECK(x.categories == y->categories);
    CHECK(x.status == y->status);
    CHECK(x.url == y->url);
    CHECK(x.alarms.size() == y->alarms.size());
  }
}

}  // namespace

TEST_CASE("pim/ics: decodes a file written by Python icalendar") {
  std::vector<std::string> warnings;
  const auto calendar = test::read_as<ir::Calendar>("ics", test::fixture("pim/calendar/events_icalendar.ics"), {}, &warnings);
  REQUIRE_OK(calendar);
  CHECK(warnings.empty());
  CHECK(calendar->name == "Team calendar");
  CHECK(calendar->events.size() == 7);
  REQUIRE(calendar->timezones.size() == 1);
  CHECK(calendar->timezones[0].name == "VTIMEZONE");
  CHECK(calendar->timezones[0].children.size() == 2);

  const ir::Event* planning = find_event(*calendar, "planning-1@example.org");
  REQUIRE(planning != nullptr);
  CHECK(planning->summary == "Quarterly planning ✨");
  CHECK(planning->start == at(2024, 5, 6, 10, 0, 0, false, "Europe/Berlin"));
  CHECK(planning->end == at(2024, 5, 6, 11, 30, 0, false, "Europe/Berlin"));
  CHECK(planning->location == "Room 4, Example Tower; floor 2");
  CHECK(planning->description ==
        "Agenda:\n1. Review the roadmap for the next quarter, including every milestone that slipped\n2. Budget "
        "– 50% travel, 50% equipment");
  CHECK(planning->organizer == "Ada Example <mailto:ada@example.org>");
  CHECK(planning->attendees == std::vector<std::string>{"Bob Sample <mailto:bob@example.org>",
                                                        "Chloé Exemple <mailto:chloe@example.org>"});
  CHECK(planning->categories == std::vector<std::string>{"Work", "Planning"});
  CHECK(planning->status == "CONFIRMED");
  CHECK(planning->url == "https://example.org/meetings/planning");
  REQUIRE(planning->alarms.size() == 1);
  CHECK(planning->alarms[0].minutes_before == 15);
  CHECK(planning->alarms[0].description == "Planning starts soon");

  const ir::Event* birthday = find_event(*calendar, "birthday-2@example.org");
  REQUIRE(birthday != nullptr);
  CHECK(birthday->start == day(2024, 6, 10));
  CHECK(birthday->end == day(2024, 6, 11));
  CHECK(birthday->rrule == "FREQ=YEARLY");

  const ir::Event* rtl = find_event(*calendar, "rtl-3@example.org");
  REQUIRE(rtl != nullptr);
  CHECK(rtl->summary == "اجتماع الفريق");
  CHECK(rtl->start == at(2024, 5, 7, 14, 0, 0, true));

  const ir::Event* standup = find_event(*calendar, "standup-4@example.org");
  REQUIRE(standup != nullptr);
  CHECK(standup->rrule == "FREQ=WEEKLY;COUNT=6;BYDAY=MO,WE");
  REQUIRE(standup->exdates.size() == 1);
  CHECK(standup->exdates[0] == at(2024, 5, 8, 9, 0, 0, false, "Europe/Berlin"));

  const ir::Event* review = find_event(*calendar, "review-5@example.org");
  REQUIRE(review != nullptr);
  CHECK(review->end == at(2024, 5, 31, 17, 0, 0, true));  // from DURATION:PT1H

  const ir::Event* todo = find_event(*calendar, "todo-6@example.org");
  REQUIRE(todo != nullptr);
  CHECK(todo->component == "VTODO");
  CHECK(todo->due == at(2024, 5, 8, 17, 0, 0, true));
  CHECK(todo->status == "NEEDS-ACTION");

  const ir::Event* journal = find_event(*calendar, "journal-7@example.org");
  REQUIRE(journal != nullptr);
  CHECK(journal->component == "VJOURNAL");
  CHECK(journal->description == "Everyone agreed; lunch was good.");
}

TEST_CASE("pim/ics: decodes a file written by vobject, with folded and escaped text") {
  const auto calendar = test::read_as<ir::Calendar>("ics", test::fixture("pim/calendar/events_vobject.ics"));
  REQUIRE_OK(calendar);
  REQUIRE(calendar->events.size() == 2);
  const ir::Event* workshop = find_event(*calendar, "vobject-1@example.org");
  REQUIRE(workshop != nullptr);
  CHECK(workshop->summary == "Workshop: 日本語 and Русский");
  CHECK(workshop->description ==
        "A long description that certainly needs folding because it is longer than seventy-five octets, with "
        "commas, semicolons; and a backslash \\ too.");
  const ir::Event* holiday = find_event(*calendar, "vobject-2@example.org");
  REQUIRE(holiday != nullptr);
  CHECK(holiday->start == day(2024, 8, 1));
  CHECK(holiday->end == day(2024, 8, 3));
}

TEST_CASE("pim/ics: written files fold lines, escape text and read back identically") {
  const Bytes source = test::fixture("pim/calendar/events_icalendar.ics");
  const auto original = test::read_as<ir::Calendar>("ics", source);
  REQUIRE_OK(original);
  const auto written = test::write_from("ics", *original);
  REQUIRE_OK(written);
  const std::string text = test::str(*written);
  // Every physical line is at most 75 octets and ends in CRLF.
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find("\r\n", start);
    REQUIRE(end != std::string::npos);
    CHECK(end - start <= 75);
    start = end + 2;
  }
  CHECK(text.find("LOCATION:Room 4\\, Example Tower\\; floor 2\r\n") != std::string::npos);
  CHECK(text.find("BEGIN:VTIMEZONE") != std::string::npos);
  CHECK(text.find("PRODID:-//Example Org") == std::string::npos);  // strip_metadata defaults to true
  const auto again = test::read_as<ir::Calendar>("ics", *written);
  REQUIRE_OK(again);
  check_same_events(*original, *again);
  CHECK(again->timezones.size() == 1);
  // The same-format route goes through the engine and is stable.
  const test::Converted twice = test::convert(*written, "ics", "ics");
  REQUIRE(twice.ok());
  CHECK(twice.text() == text);
  // With strip_metadata off the source PRODID survives.
  const auto kept = test::write_from("ics", *original, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  CHECK(test::str(*kept).find("PRODID:-//Example Org//Fixture Calendar//EN") != std::string::npos);
}

TEST_CASE("pim/ics: generated UIDs and DTSTAMP are deterministic") {
  ir::Calendar calendar;
  ir::Event event;
  event.summary = "No uid";
  event.start = at(2024, 1, 2, 3, 4, 5, true);
  calendar.events.push_back(event);
  const auto a = test::write_from("ics", calendar);
  const auto b = test::write_from("ics", calendar);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  CHECK(*a == *b);
  CHECK(test::str(*a).find("DTSTAMP:20240102T030405Z") != std::string::npos);
  const auto back = test::read_as<ir::Calendar>("ics", *a);
  REQUIRE_OK(back);
  CHECK(back->events[0].uid.size() == 32);
}

TEST_CASE("pim/ifb: free/busy objects round trip") {
  const Bytes source = test::fixture("pim/calendar/freebusy.ifb");
  const auto calendar = test::read_as<ir::Calendar>("ifb", source);
  REQUIRE_OK(calendar);
  REQUIRE(calendar->events.size() == 1);
  const ir::Event& fb = calendar->events[0];
  CHECK(fb.component == "VFREEBUSY");
  CHECK(std::ranges::any_of(fb.extra, [](const ir::Property& p) {
    return p.name == "FREEBUSY" && p.value == "20240506T080000Z/20240506T100000Z";
  }));
  const auto written = test::write_from("ifb", *calendar);
  REQUIRE_OK(written);
  const auto again = test::read_as<ir::Calendar>("ifb", *written);
  REQUIRE_OK(again);
  check_same_events(*calendar, *again);
  CHECK(test::convert(source, "", "ics", "", "busy.ifb").ok());
}

TEST_CASE("pim/vcs: decodes vCalendar 1.0 with quoted-printable, alarms and a v1 rule") {
  std::vector<std::string> warnings;
  const auto calendar = test::read_as<ir::Calendar>("vcs", test::fixture("pim/calendar/legacy.vcs"), {}, &warnings);
  REQUIRE_OK(calendar);
  CHECK(warnings.empty());
  REQUIRE(calendar->events.size() == 2);
  const ir::Event& meeting = calendar->events[0];
  CHECK(meeting.summary == "Café meeting");
  CHECK(meeting.description == "First line\nSecond line");
  CHECK(meeting.start == at(2024, 5, 6, 8, 0, 0, true));
  CHECK(meeting.categories == std::vector<std::string>{"BUSINESS", "MEETING"});
  CHECK(meeting.rrule == "FREQ=WEEKLY;BYDAY=MO,TH;COUNT=4");
  REQUIRE(meeting.alarms.size() == 1);
  CHECK(meeting.alarms[0].minutes_before == 15);
  CHECK(meeting.alarms[0].description == "Coffee first");
  CHECK(calendar->events[1].component == "VTODO");
  CHECK(calendar->events[1].due == at(2024, 5, 10, 17, 0, 0, true));
  // vcs converts to iCalendar through the engine.
  const test::Converted ics = test::convert(test::fixture("pim/calendar/legacy.vcs"), "", "ics", "", "legacy.vcs");
  REQUIRE(ics.ok());
  CHECK(ics.text().find("RRULE:FREQ=WEEKLY;BYDAY=MO,TH;COUNT=4") != std::string::npos);
}

TEST_CASE("pim/ics: recurrence expansion honours COUNT, BYDAY, EXDATE, UNTIL and the range") {
  const Bytes source = test::fixture("pim/calendar/events_icalendar.ics");
  const auto expanded = test::read_as<ir::Calendar>("ics", source,
      R"({"expand_recurrences":true,"range_start":"2024-05-01","range_end":"2024-12-31"})");
  REQUIRE_OK(expanded);
  // Weekly on Monday and Wednesday, six occurrences counted before EXDATE removes 8 May.
  CHECK(starts(*expanded, "standup-4@example.org") ==
        std::vector<std::string>{"2024-05-06T09:00:00", "2024-05-13T09:00:00", "2024-05-15T09:00:00",
                                 "2024-05-20T09:00:00", "2024-05-22T09:00:00"});
  // Last Friday of the month until the end of September.
  CHECK(starts(*expanded, "review-5@example.org") ==
        std::vector<std::string>{"2024-05-31T16:00:00Z", "2024-06-28T16:00:00Z", "2024-07-26T16:00:00Z",
                                 "2024-08-30T16:00:00Z", "2024-09-27T16:00:00Z"});
  // A yearly birthday has one occurrence inside the range.
  CHECK(starts(*expanded, "birthday-2@example.org") == std::vector<std::string>{"2024-06-10"});
  for (const ir::Event& event : expanded->events) {
    CHECK(event.rrule.empty());
    if (event.uid == "standup-4@example.org") {
      CHECK(event.recurrence_id == event.start);
      REQUIRE(event.end.has_value());
      CHECK(pim::cl::civil_seconds(*event.end) - pim::cl::civil_seconds(event.start) == 15 * 60);
    }
  }
  // A narrow range keeps only what overlaps it.
  const auto narrow = test::read_as<ir::Calendar>("ics", source,
      R"({"expand_recurrences":true,"range_start":"2024-05-13","range_end":"2024-05-14"})");
  REQUIRE_OK(narrow);
  CHECK(starts(*narrow, "standup-4@example.org") == std::vector<std::string>{"2024-05-13T09:00:00"});
  CHECK(starts(*narrow, "planning-1@example.org").empty());
  // Unbounded yearly rule without a range end is capped.
  std::vector<std::string> warnings;
  const auto capped = test::read_as<ir::Calendar>("ics", source, R"({"expand_recurrences":true,"max_occurrences":3})",
                                                  &warnings);
  REQUIRE_OK(capped);
  CHECK(starts(*capped, "birthday-2@example.org") ==
        std::vector<std::string>{"2024-06-10", "2025-06-10", "2026-06-10"});
  CHECK(!warnings.empty());
  CHECK_STATUS(test::read_as<ir::Calendar>("ics", source, R"({"expand_recurrences":true,"range_end":"soon"})"),
               Status::InvalidArgument);
}

TEST_CASE("pim/ics: recurrence rules - monthly by day, yearly, BYSETPOS, RDATE and overrides") {
  const std::string text =
      "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:x\r\n"
      "BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20240115T100000\r\nRRULE:FREQ=MONTHLY;BYMONTHDAY=15,-1;COUNT=5\r\nEND:VEVENT\r\n"
      "BEGIN:VEVENT\r\nUID:b\r\nDTSTART:20240101T090000\r\nRRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1;COUNT=3\r\n"
      "END:VEVENT\r\n"
      "BEGIN:VEVENT\r\nUID:c\r\nDTSTART;VALUE=DATE:20240229\r\nRRULE:FREQ=YEARLY;COUNT=2\r\nRDATE;VALUE=DATE:20250301\r\n"
      "END:VEVENT\r\n"
      "BEGIN:VEVENT\r\nUID:d\r\nDTSTART:20240101T080000Z\r\nRRULE:FREQ=DAILY;INTERVAL=2;COUNT=4\r\nEND:VEVENT\r\n"
      "BEGIN:VEVENT\r\nUID:d\r\nRECURRENCE-ID:20240103T080000Z\r\nDTSTART:20240103T120000Z\r\nSUMMARY:moved\r\n"
      "END:VEVENT\r\n"
      "BEGIN:VEVENT\r\nUID:e\r\nDTSTART:20240111T100000\r\nRRULE:FREQ=YEARLY;BYMONTH=1;BYDAY=2TH;COUNT=2\r\nEND:VEVENT\r\n"
      "END:VCALENDAR\r\n";
  const auto calendar = test::read_as<ir::Calendar>("ics", as_bytes(text), R"({"expand_recurrences":true})");
  REQUIRE_OK(calendar);
  CHECK(starts(*calendar, "a") == std::vector<std::string>{"2024-01-15T10:00:00", "2024-01-31T10:00:00",
                                                           "2024-02-15T10:00:00", "2024-02-29T10:00:00",
                                                           "2024-03-15T10:00:00"});
  CHECK(starts(*calendar, "b") ==
        std::vector<std::string>{"2024-01-01T09:00:00", "2024-01-31T09:00:00", "2024-02-29T09:00:00"});
  // Feb 29 exists only in leap years; the RDATE adds one more day.
  CHECK(starts(*calendar, "c") == std::vector<std::string>{"2024-02-29", "2025-03-01", "2028-02-29"});
  CHECK(starts(*calendar, "d") == std::vector<std::string>{"2024-01-01T08:00:00Z", "2024-01-03T12:00:00Z",
                                                           "2024-01-05T08:00:00Z", "2024-01-07T08:00:00Z"});
  CHECK(starts(*calendar, "e") == std::vector<std::string>{"2024-01-11T10:00:00", "2025-01-09T10:00:00"});
}

TEST_CASE("pim/ics: content lines - unfolding, parameters and escapes") {
  const std::string text =
      "BEGIN:VCALENDAR\nVERSION:2.0\nBEGIN:VEVENT\nUID:x\nDTSTART:20240101\nSUMMARY:Long sum\n mary with\n\t tab\n"
      "ATTENDEE;CN=\"Doe, Jane\";ROLE=REQ-PARTICIPANT:mailto:jane@example.org\n"
      "DESCRIPTION:a\\nb\\, c\\; d\\\\ e\nX-UNKNOWN;FOO=bar:value\\, kept\nEND:VEVENT\nEND:VCALENDAR\n";
  const auto calendar = test::read_as<ir::Calendar>("ics", as_bytes(text));
  REQUIRE_OK(calendar);
  REQUIRE(calendar->events.size() == 1);
  const ir::Event& event = calendar->events[0];
  CHECK(event.summary == "Long summary with tab");
  CHECK(event.attendees == std::vector<std::string>{"Doe, Jane <mailto:jane@example.org>"});
  CHECK(event.description == "a\nb, c; d\\ e");
  CHECK(event.start.date_only);
  REQUIRE(!event.extra.empty());
  const auto written = test::write_from("ics", *calendar);
  REQUIRE_OK(written);
  CHECK(test::str(*written).find("ATTENDEE;CN=\"Doe, Jane\":mailto:jane@example.org") != std::string::npos);
  CHECK(test::str(*written).find("X-UNKNOWN;FOO=bar:value\\, kept") != std::string::npos);
}

TEST_CASE("pim/ics: detection and damaged input") {
  CHECK(test::convert(test::fixture("pim/calendar/events_icalendar.ics"), "", "ics").format_id == "ics");
  CHECK(test::convert(test::fixture("pim/calendar/legacy.vcs"), "", "ics", "", "").ok());
  CHECK_STATUS(test::read_as<ir::Calendar>("ics", as_bytes("BEGIN:VCARD\r\nEND:VCARD\r\n"sv)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Calendar>("ics", as_bytes(""sv)), Status::Decode);
  std::string deep = "BEGIN:VCALENDAR\r\n";
  for (int i = 0; i < 100; ++i) deep += "BEGIN:X\r\n";
  CHECK_STATUS(test::read_as<ir::Calendar>("ics", as_bytes(deep)), Status::Limit);
  // Hostile recurrence rules terminate.
  const std::string hostile =
      "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:h\r\nDTSTART:20240101T000000\r\n"
      "RRULE:FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  const auto none = test::read_as<ir::Calendar>("ics", as_bytes(hostile), R"({"expand_recurrences":true})");
  REQUIRE_OK(none);
  CHECK(none->events.size() == 1);  // only DTSTART itself
  for (const char* name : {"pim/calendar/events_icalendar.ics", "pim/calendar/events_vobject.ics",
                           "pim/calendar/freebusy.ifb"}) {
    CHECK(test::fuzz_reader<ir::Calendar>("ics", test::fixture(name)) > 0);
  }
  CHECK(test::fuzz_reader<ir::Calendar>("vcs", test::fixture("pim/calendar/legacy.vcs")) > 0);
}
