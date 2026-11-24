// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "ir/contacts.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace std::string_view_literals;

namespace {

bool has_value(const std::vector<ir::LabeledValue>& values, std::string_view value, std::string_view type = {},
               std::string_view label = {}) {
  return std::ranges::any_of(values, [&](const ir::LabeledValue& v) {
    return v.value == value && v.type == type && v.label == label;
  });
}

std::vector<std::string> values_of(const std::vector<ir::LabeledValue>& values) {
  std::vector<std::string> out;
  for (const ir::LabeledValue& v : values) out.push_back(v.value);
  return out;
}

/// A card with everything every contacts format can carry.
ir::Contacts sample_contacts() {
  ir::Contacts contacts;
  ir::Card ada;
  ada.full_name = "Ada Example";
  ada.given_name = "Ada";
  ada.family_name = "Example";
  ada.organization = "Example Org";
  ada.department = "Research";
  ada.title = "Chief Example Officer";
  ada.emails = {{"ada@example.org", "", ""}, {"ada.home@example.org", "", ""}};
  ada.phones = {{"+1 555 0100", "cell", ""}, {"+1 555 0101", "work", ""}, {"+1 555 0102", "home", ""}};
  ir::PostalAddress work;
  work.type = "work";
  work.street = "1 Example Street";
  work.city = "Sampletown";
  work.region = "EX";
  work.postal_code = "01234";
  work.country = "Exampleland";
  ada.addresses.push_back(work);
  ada.birthday = "1990-04-27";
  ada.note = "Line one, with comma\nLine two é \U0001F600";
  contacts.cards.push_back(ada);
  ir::Card sara;
  sara.full_name = "سارة المثال";
  sara.given_name = "سارة";
  sara.family_name = "المثال";
  sara.emails = {{"sara@example.org", "", ""}};
  contacts.cards.push_back(sara);
  return contacts;
}

void check_core_fields(const ir::Contacts& a, const ir::Contacts& b) {
  REQUIRE(a.cards.size() == b.cards.size());
  for (std::size_t i = 0; i < a.cards.size(); ++i) {
    const ir::Card& x = a.cards[i];
    const ir::Card& y = b.cards[i];
    INFO("card ", i);
    CHECK(x.full_name == y.full_name);
    CHECK(x.given_name == y.given_name);
    CHECK(x.family_name == y.family_name);
    CHECK(x.organization == y.organization);
    CHECK(x.department == y.department);
    CHECK(x.title == y.title);
    CHECK(x.birthday == y.birthday);
    CHECK(x.note == y.note);
    CHECK(values_of(x.emails) == values_of(y.emails));
    REQUIRE(x.phones.size() == y.phones.size());
    for (const ir::LabeledValue& phone : x.phones) CHECK(has_value(y.phones, phone.value, phone.type));
    REQUIRE(x.addresses.size() == y.addresses.size());
    for (std::size_t k = 0; k < x.addresses.size(); ++k) {
      CHECK(x.addresses[k].street == y.addresses[k].street);
      CHECK(x.addresses[k].city == y.addresses[k].city);
      CHECK(x.addresses[k].region == y.addresses[k].region);
      CHECK(x.addresses[k].postal_code == y.addresses[k].postal_code);
      CHECK(x.addresses[k].country == y.addresses[k].country);
      CHECK(x.addresses[k].type == y.addresses[k].type);
    }
  }
}

constexpr std::string_view contact_formats[] = {"vcf", "jcard", "xcard", "ldif", "google-contacts-csv",
                                                "outlook-contacts-csv"};

}  // namespace

TEST_CASE("pim/jcard: decodes an RFC 7095 document written with Python json") {
  const auto contacts = test::read_as<ir::Contacts>("jcard", test::fixture("pim/contacts/cards.jcard.json"));
  REQUIRE_OK(contacts);
  REQUIRE(contacts->cards.size() == 1);
  const ir::Card& c = contacts->cards[0];
  CHECK(c.full_name == "Ada Example");
  CHECK(c.family_name == "Example");
  CHECK(c.prefix == "Dr.");
  CHECK(c.organization == "Example Org");
  CHECK(c.department == "Research");
  CHECK(has_value(c.emails, "ada@example.org", "work"));
  CHECK(has_value(c.emails, "lab@example.org", "", "Lab"));
  CHECK(has_value(c.phones, "+1-555-0100", "home"));
  CHECK(c.addresses.at(0).street == "1 Example Street");
  CHECK(c.addresses.at(0).country == "Exampleland");
  CHECK(c.birthday == "1990-04-27");
  CHECK(c.categories == std::vector<std::string>{"Friends", "Work"});
  CHECK(c.note == "Unicode: éè 日本 \U0001F600, with; separators");
}

TEST_CASE("pim/xcard: decodes an RFC 6351 document written with ElementTree") {
  const auto contacts = test::read_as<ir::Contacts>("xcard", test::fixture("pim/contacts/cards.xcard.xml"));
  REQUIRE_OK(contacts);
  REQUIRE(contacts->cards.size() == 2);
  const ir::Card& c = contacts->cards[0];
  CHECK(c.full_name == "Ada Example");
  CHECK(c.given_name == "Ada");
  CHECK(c.prefix == "Dr.");
  CHECK(has_value(c.emails, "ada@example.org", "work"));
  CHECK(has_value(c.emails, "lab@example.org", "", "Lab"));
  CHECK(has_value(c.phones, "+1-555-0100", "cell"));
  CHECK(c.addresses.at(0).city == "Sampletown");
  CHECK(c.addresses.at(0).type == "home");
  CHECK(c.birthday == "1990-04-27");
  CHECK(c.note == "Café ☕ & <markup> stays text");
  CHECK(contacts->cards[1].organization == "Sample & Sons");
}

TEST_CASE("pim/ldif: decodes a Thunderbird-style LDIF file with base64 values and a photo") {
  const auto contacts = test::read_as<ir::Contacts>("ldif", test::fixture("pim/contacts/contacts.ldif"));
  REQUIRE_OK(contacts);
  REQUIRE(contacts->cards.size() == 2);
  const ir::Card& ada = contacts->cards[0];
  CHECK(ada.full_name == "Ada Example");
  CHECK(ada.nickname == "Ada");
  CHECK(values_of(ada.emails) == std::vector<std::string>{"ada@example.org", "ada.home@example.org"});
  CHECK(has_value(ada.phones, "+1 555 0101", "work"));
  CHECK(has_value(ada.phones, "+1 555 0102", "home"));
  CHECK(has_value(ada.phones, "+1 555 0100", "cell"));
  REQUIRE(ada.addresses.size() == 2);
  CHECK(ada.addresses[0].street == "1 Example Street");
  CHECK(ada.addresses[0].type == "work");
  CHECK(ada.addresses[1].street == "9 Home Lane");
  CHECK(ada.addresses[1].city == "Hometown");
  CHECK(ada.organization == "Example Org");
  CHECK(ada.department == "Research");
  CHECK(ada.birthday == "1990-04-27");
  CHECK(ada.note == "Multi-line note\nwith ümlauts");
  CHECK(has_value(ada.urls, "https://example.org/ada", "work"));
  const ir::Card& j = contacts->cards[1];
  CHECK(j.full_name == "Jürgen Müller");
  CHECK(j.photo == test::fixture("pim/contacts/dot_2x2.png"));
}

TEST_CASE("pim/contacts-csv: decodes Google and Outlook contact exports written with Python csv") {
  const auto google = test::read_as<ir::Contacts>("google-contacts-csv", test::fixture("pim/contacts/google_contacts.csv"));
  REQUIRE_OK(google);
  REQUIRE(google->cards.size() == 2);
  const ir::Card& ada = google->cards[0];
  CHECK(ada.full_name == "Dr. Ada Marie Example PhD");
  CHECK(ada.department == "Research");
  CHECK(ada.note == "Line one\nLine two, with comma");
  CHECK(ada.categories == std::vector<std::string>{"Friends"});
  CHECK(has_value(ada.emails, "ada@example.org", "work"));
  CHECK(has_value(ada.emails, "ada.home@example.org", "home"));
  CHECK(has_value(ada.emails, "ada.alt@example.org", "home"));
  CHECK(has_value(ada.phones, "+1 555 0100", "cell"));
  CHECK(ada.addresses.at(0).extended == "Suite 5");
  CHECK(has_value(ada.urls, "https://example.org/ada", "", "Profile"));
  CHECK(google->cards[1].note == "\U0001F600");

  const auto outlook = test::read_as<ir::Contacts>("outlook-contacts-csv", test::fixture("pim/contacts/outlook_contacts.csv"));
  REQUIRE_OK(outlook);
  REQUIRE(outlook->cards.size() == 2);
  const ir::Card& o = outlook->cards[0];
  CHECK(o.prefix == "Dr.");
  CHECK(o.full_name == "Dr. Ada Example");
  CHECK(values_of(o.emails) == std::vector<std::string>{"ada@example.org", "ada.home@example.org"});
  CHECK(has_value(o.phones, "+1 555 0101", "work"));
  CHECK(has_value(o.phones, "+1 555 0199", "fax"));
  CHECK(o.addresses.size() == 2);
  CHECK(o.birthday == "1990-04-27");
  CHECK(o.anniversary.empty());
  CHECK(o.note == "Note with \"quotes\", commas\nand a newline");
  CHECK(o.categories == std::vector<std::string>{"Friends", "Work"});
  CHECK(outlook->cards[1].given_name == "Jürgen");
}

TEST_CASE("pim/contacts: every contacts format round trips the common fields, and every pair converts") {
  const ir::Contacts sample = sample_contacts();
  for (const std::string_view format : contact_formats) {
    INFO("format ", format);
    const auto written = test::write_from(format, sample);
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Contacts>(format, *written);
    REQUIRE_OK(back);
    check_core_fields(sample, *back);
  }
  // Pairs through the engine: source format -> target format -> back to vcf.
  for (const std::string_view from : contact_formats) {
    const auto source = test::write_from(from, sample);
    REQUIRE_OK(source);
    for (const std::string_view to : contact_formats) {
      INFO(from, " -> ", to);
      const test::Converted converted = test::convert(*source, from, to);
      REQUIRE(converted.ok());
      const auto back = test::read_as<ir::Contacts>(to, converted.data);
      REQUIRE_OK(back);
      check_core_fields(sample, *back);
    }
  }
}

TEST_CASE("pim/contacts: photos survive jCard, xCard and LDIF unless dropped") {
  auto contacts = test::read_as<ir::Contacts>("vcf", test::fixture("pim/contacts/cards_v30_vobject.vcf"));
  REQUIRE_OK(contacts);
  const Bytes png = test::fixture("pim/contacts/dot_2x2.png");
  for (const std::string_view format : {"jcard"sv, "xcard"sv, "ldif"sv}) {
    INFO(format);
    const auto written = test::write_from(format, *contacts);
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Contacts>(format, *written);
    REQUIRE_OK(back);
    CHECK(back->cards[0].photo == png);
    const auto dropped = test::write_from(format, *contacts, R"({"drop_photos":true})");
    REQUIRE_OK(dropped);
    CHECK(test::read_as<ir::Contacts>(format, *dropped)->cards[0].photo.empty());
  }
  // jCard and xCard keep labels, categories and extra properties.
  for (const std::string_view format : {"jcard"sv, "xcard"sv}) {
    const auto back = test::read_as<ir::Contacts>(format, *test::write_from(format, *contacts));
    REQUIRE_OK(back);
    CHECK(has_value(back->cards[0].emails, "ada.lab@example.org", "", "Other"));
    CHECK(back->cards[0].categories == contacts->cards[0].categories);
    CHECK(back->cards[0].nickname == "Ada");
  }
}

TEST_CASE("pim/contacts: detection tells the formats apart") {
  CHECK(test::convert(test::fixture("pim/contacts/cards.jcard.json"), "", "vcf").ok());
  const auto detect = [](std::string_view fixture, std::string_view filename) {
    const test::Converted c = test::convert(test::fixture(fixture), "", "vcf", "", filename);
    return c.ok();
  };
  CHECK(detect("pim/contacts/cards.jcard.json", "cards.json"));
  CHECK(detect("pim/contacts/cards.xcard.xml", "cards.xml"));
  CHECK(detect("pim/contacts/contacts.ldif", "contacts.ldif"));
  CHECK(detect("pim/contacts/google_contacts.csv", "contacts.csv"));
  CHECK(detect("pim/contacts/outlook_contacts.csv", "contacts.csv"));
  // Each sniff claims its own file only.
  const Registry& registry = Registry::instance();
  const auto score = [&](std::string_view id, std::string_view fixture) {
    return registry.find(id)->sniff(test::fixture(fixture), {});
  };
  CHECK(score("google-contacts-csv", "pim/contacts/google_contacts.csv") > 0);
  CHECK(score("outlook-contacts-csv", "pim/contacts/google_contacts.csv") == 0);
  CHECK(score("outlook-contacts-csv", "pim/contacts/outlook_contacts.csv") > 0);
  CHECK(score("google-contacts-csv", "pim/contacts/outlook_contacts.csv") == 0);
  CHECK(score("jcard", "pim/contacts/cards.xcard.xml") == 0);
  CHECK(score("xcard", "pim/contacts/cards.xcard.xml") > 0);
}

TEST_CASE("pim/contacts: damaged input fails cleanly") {
  CHECK_STATUS(test::read_as<ir::Contacts>("jcard", as_bytes("[\"vcard\"]"sv)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Contacts>("jcard", as_bytes("{"sv)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Contacts>("xcard", as_bytes("<a/>"sv)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Contacts>("ldif", as_bytes("version: 1\n"sv)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Contacts>("google-contacts-csv", as_bytes("a,b\n1,2\n"sv)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Contacts>("jcard", test::fixture("pim/contacts/cards.jcard.json")) > 0);
  CHECK(test::fuzz_reader<ir::Contacts>("xcard", test::fixture("pim/contacts/cards.xcard.xml")) > 0);
  CHECK(test::fuzz_reader<ir::Contacts>("ldif", test::fixture("pim/contacts/contacts.ldif")) > 0);
  CHECK(test::fuzz_reader<ir::Contacts>("google-contacts-csv", test::fixture("pim/contacts/google_contacts.csv")) > 0);
  CHECK(test::fuzz_reader<ir::Contacts>("outlook-contacts-csv", test::fixture("pim/contacts/outlook_contacts.csv")) > 0);
}
