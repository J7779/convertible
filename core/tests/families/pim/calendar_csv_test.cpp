// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
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

std::string iso(const ir::CalendarTime& time) { return pim::cl::iso_time(time); }

constexpr std::string_view calendar_formats[] = {"ics", "ifb", "google-calendar-csv", "outlook-calendar-csv"};

ir::Calendar sample_calendar() {
  ir::Calendar calendar;
  ir::Event timed;
  timed.uid = "a@example.org";
  timed.summary = "Planning, \"quoted\" ✨";
  timed.description = "Line one\nLine two";
  timed.location = "חדר 4";
  timed.start = *pim::cl::parse_time("20240506T100000");
  timed.end = *pim::cl::parse_time("20240506T113000");
  calendar.events.push_back(timed);
  ir::Event all_day;
  all_day.uid = "b@example.org";
  all_day.summary = "Holiday";
  all_day.start = *pim::cl::parse_time("20240801");
  all_day.end = *pim::cl::parse_time("20240803");
  calendar.events.push_back(all_day);
  ir::Event late;
  late.uid = "c@example.org";
  late.summary = "Late call";
  late.start = *pim::cl::parse_time("20240507T231500");
  late.end = *pim::cl::parse_time("20240508T001500");
  calendar.events.push_back(late);
  return calendar;
}

}  // namespace

TEST_CASE("pim/calendar-csv: decodes Google Calendar and Outlook CSV written with Python csv") {
  const auto google = test::read_as<ir::Calendar>("google-calendar-csv", test::fixture("pim/calendar/google_calendar.csv"));
  REQUIRE_OK(google);
  REQUIRE(google->events.size() == 3);
  CHECK(google->events[0].summary == "Quarterly planning");
  CHECK(iso(google->events[0].start) == "2024-05-06T10:00:00");
  CHECK(iso(*google->events[0].end) == "2024-05-06T11:30:00");
  CHECK(google->events[0].description == "Roadmap, budget \"and\" travel");
  CHECK(iso(google->events[1].start) == "2024-06-10");
  CHECK(iso(*google->events[1].end) == "2024-06-11");  // inclusive end date made exclusive
  CHECK(google->events[1].extra.size() == 1);          // CLASS:PRIVATE
  CHECK(iso(*google->events[2].end) == "2024-05-08T00:15:00");
  CHECK(google->events[2].summary == "Late call ☎");
  CHECK(google->events[2].description == "Line one\nline two");

  const auto outlook = test::read_as<ir::Calendar>("outlook-calendar-csv", test::fixture("pim/calendar/outlook_calendar.csv"));
  REQUIRE_OK(outlook);
  REQUIRE(outlook->events.size() == 2);
  const ir::Event& planning = outlook->events[0];
  CHECK(iso(planning.start) == "2024-05-06T10:00:00");
  CHECK(planning.organizer == "Ada Example");
  CHECK(planning.attendees == std::vector<std::string>{"Bob Sample", "Chloé Exemple"});
  CHECK(planning.categories == std::vector<std::string>{"Work", "Planning"});
  REQUIRE(planning.alarms.size() == 1);
  CHECK(planning.alarms[0].minutes_before == 15);
  CHECK(iso(outlook->events[1].start) == "2024-08-01");
  CHECK(iso(*outlook->events[1].end) == "2024-08-03");  // Outlook's end date is already exclusive
}

TEST_CASE("pim/calendar: every calendar format round trips, and every pair converts") {
  const ir::Calendar sample = sample_calendar();
  for (const std::string_view format : calendar_formats) {
    INFO("format ", format);
    const auto written = test::write_from(format, sample);
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Calendar>(format, *written);
    REQUIRE_OK(back);
    REQUIRE(back->events.size() == sample.events.size());
    for (std::size_t i = 0; i < sample.events.size(); ++i) {
      CHECK(back->events[i].summary == sample.events[i].summary);
      CHECK(back->events[i].description == sample.events[i].description);
      CHECK(back->events[i].location == sample.events[i].location);
      CHECK(back->events[i].start == sample.events[i].start);
      CHECK(back->events[i].end == sample.events[i].end);
    }
  }
  for (const std::string_view from : calendar_formats) {
    const auto source = test::write_from(from, sample);
    REQUIRE_OK(source);
    for (const std::string_view to : calendar_formats) {
      INFO(from, " -> ", to);
      const test::Converted converted = test::convert(*source, from, to);
      REQUIRE(converted.ok());
      const auto back = test::read_as<ir::Calendar>(to, converted.data);
      REQUIRE_OK(back);
      REQUIRE(back->events.size() == sample.events.size());
      for (std::size_t i = 0; i < sample.events.size(); ++i) {
        CHECK(back->events[i].summary == sample.events[i].summary);
        CHECK(back->events[i].start == sample.events[i].start);
        CHECK(back->events[i].end == sample.events[i].end);
      }
    }
  }
  // vcs is read-only and converts into each of them.
  for (const std::string_view to : calendar_formats) {
    CHECK(test::convert(test::fixture("pim/calendar/legacy.vcs"), "vcs", to).ok());
  }
}

TEST_CASE("pim/calendar-csv: warnings for what CSV cannot hold, and detection") {
  auto calendar = test::read_as<ir::Calendar>("ics", test::fixture("pim/calendar/events_icalendar.ics"));
  REQUIRE_OK(calendar);
  std::vector<std::string> warnings;
  const auto csv = test::write_from("google-calendar-csv", *calendar, {}, &warnings);
  REQUIRE_OK(csv);
  CHECK(warnings.size() == 2);  // time zones and recurrence rules
  const auto detect = [](std::string_view fixture) {
    return test::convert(test::fixture(fixture), "", "ics", "", "calendar.csv");
  };
  CHECK(detect("pim/calendar/google_calendar.csv").ok());
  CHECK(detect("pim/calendar/outlook_calendar.csv").ok());
  const Registry& registry = Registry::instance();
  CHECK(registry.find("google-calendar-csv")->sniff(test::fixture("pim/calendar/google_calendar.csv"), {}) > 0);
  CHECK(registry.find("outlook-calendar-csv")->sniff(test::fixture("pim/calendar/google_calendar.csv"), {}) == 0);
  CHECK(registry.find("outlook-calendar-csv")->sniff(test::fixture("pim/calendar/outlook_calendar.csv"), {}) > 0);
  CHECK_STATUS(test::read_as<ir::Calendar>("google-calendar-csv", as_bytes("a,b\n"sv)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Calendar>("google-calendar-csv", test::fixture("pim/calendar/google_calendar.csv")) > 0);
  CHECK(test::fuzz_reader<ir::Calendar>("outlook-calendar-csv", test::fixture("pim/calendar/outlook_calendar.csv")) > 0);
}
