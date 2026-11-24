// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>
#include <vector>

#include "ir/calendar.hpp"
#include "ir/contacts.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/table.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/json.hpp"

using namespace cvt;

namespace {

Result<AnyIR> bridge(AnyIR value, Kind to) {
  const Kind from = value.kind();
  for (const Bridge& candidate : Registry::instance().bridges()) {
    if (candidate.from == from && candidate.to == to) {
      Context ctx;
      ctx.registry = &Registry::instance();
      return candidate.fn(std::move(value), ctx);
    }
  }
  return fail(Status::UnsupportedConversion, "no bridge");
}

ir::Calendar calendar_fixture() {
  auto calendar = test::read_as<ir::Calendar>("ics", test::fixture("pim/calendar/events_vobject.ics"));
  return calendar ? *calendar : ir::Calendar{};
}

ir::Contacts contacts_fixture() {
  auto contacts = test::read_as<ir::Contacts>("vcf", test::fixture("pim/contacts/cards_v30_vobject.vcf"));
  return contacts ? *contacts : ir::Contacts{};
}

}  // namespace

TEST_CASE("pim/bridges: calendar to table uses ISO times and typed cells") {
  const auto result = bridge(calendar_fixture(), Kind::Table);
  REQUIRE_OK(result);
  const ir::Table& table = result->get<ir::Table>();
  REQUIRE(table.sheets.size() == 1);
  const ir::Sheet& sheet = table.sheets[0];
  REQUIRE(sheet.rows.size() == 3);
  CHECK(sheet.rows[0][3].text == "Start");
  CHECK(sheet.rows[1][2].text == "Workshop: 日本語 and Русский");
  CHECK(sheet.rows[1][3].type == ir::CellType::DateTime);
  CHECK(sheet.rows[1][3].text == "2024-07-01T13:00:00Z");
  CHECK(sheet.rows[1][3].number == 1719838800.0);
  CHECK(sheet.rows[2][3].text == "2024-08-01");
  CHECK(sheet.rows[2][5].type == ir::CellType::Bool);
  CHECK(sheet.rows[2][5].number == 1.0);
}

TEST_CASE("pim/bridges: calendar to data keeps every modelled field") {
  const auto result = bridge(calendar_fixture(), Kind::Data);
  REQUIRE_OK(result);
  const Json expected = *util::parse_json(R"({
    "events": [
      {"type": "VEVENT", "uid": "vobject-1@example.org",
       "summary": "Workshop: 日本語 and Русский",
       "description": "A long description that certainly needs folding because it is longer than seventy-five octets, with commas, semicolons; and a backslash \\ too.",
       "start": "2024-07-01T13:00:00Z", "all_day": false, "end": "2024-07-01T17:00:00Z",
       "properties": [{"name": "DTSTAMP", "value": "20240401T080000Z"}]},
      {"type": "VEVENT", "uid": "vobject-2@example.org", "summary": "Holiday",
       "start": "2024-08-01", "all_day": true, "end": "2024-08-03",
       "properties": [{"name": "DTSTAMP", "value": "20240401T080000Z"}]}
    ]})");
  CHECK_MESSAGE(test::data_equal(result->get<ir::Data>().value, expected),
                test::data_diff(result->get<ir::Data>().value, expected));
}

TEST_CASE("pim/bridges: calendar to document is an agenda grouped by day") {
  auto calendar = test::read_as<ir::Calendar>("ics", test::fixture("pim/calendar/events_icalendar.ics"));
  REQUIRE_OK(calendar);
  const auto result = bridge(*calendar, Kind::Document);
  REQUIRE_OK(result);
  const std::string text = ir::plain_text(result->get<ir::Document>());
  CHECK(text.find("Team calendar") == 0);
  CHECK(text.find("Monday, 6 May 2024") != std::string::npos);
  CHECK(text.find("10:00–11:30 (Europe/Berlin)  Quarterly planning ✨ — Room 4, Example Tower; floor 2") !=
        std::string::npos);
  CHECK(text.find("All day  Ada Example's birthday") != std::string::npos);
  CHECK(text.find("Monday, 6 May 2024") < text.find("Tuesday, 7 May 2024"));
}

TEST_CASE("pim/bridges: contacts to table, data and document") {
  const ir::Contacts contacts = contacts_fixture();
  REQUIRE(contacts.cards.size() == 3);
  const auto table = bridge(contacts, Kind::Table);
  REQUIRE_OK(table);
  const ir::Sheet& sheet = table->get<ir::Table>().sheets.at(0);
  REQUIRE(sheet.rows.size() == 4);
  CHECK(sheet.rows[0][11].text == "E-mail");
  CHECK(sheet.rows[1][0].text == "Dr. Ada Marie Example PhD");
  CHECK(sheet.rows[1][11].text == "ada@example.org (work); ada.home@example.org (home); ada.lab@example.org (Other)");
  CHECK(sheet.rows[3][13].text == "2 Sample Road\nBuilding B, 8000 Zürich, Switzerland (home)");

  const auto data = bridge(contacts, Kind::Data);
  REQUIRE_OK(data);
  const Json& root = data->get<ir::Data>().value;
  CHECK(root["contacts"].size() == 3);
  CHECK(root["contacts"][0]["name"]["given"].as_string() == "Ada");
  CHECK(root["contacts"][0]["emails"][2]["label"].as_string() == "Other");
  CHECK(root["contacts"][0]["photo"]["mime"].as_string() == "image/png");
  CHECK(root["contacts"][1]["full_name"].as_string() == "سارة المثال");

  const auto document = bridge(contacts, Kind::Document);
  REQUIRE_OK(document);
  const ir::Document& doc = document->get<ir::Document>();
  CHECK(doc.blocks.size() >= 7);
  bool has_image = false;
  for (const ir::Block& block : doc.blocks) has_image = has_image || block.is<ir::ImageBlock>();
  CHECK(has_image);
  const std::string text = ir::plain_text(doc);
  CHECK(text.find("Organization: Example Org, Research") != std::string::npos);
  CHECK(text.find("Phone: +1 555 0100 (cell)") != std::string::npos);
}
