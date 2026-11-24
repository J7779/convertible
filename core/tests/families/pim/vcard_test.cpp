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

bool has_value(const std::vector<ir::LabeledValue>& values, std::string_view value, std::string_view type,
               std::string_view label = {}) {
  return std::ranges::any_of(values, [&](const ir::LabeledValue& v) {
    return v.value == value && v.type == type && v.label == label;
  });
}

void check_same_cards(const ir::Contacts& a, const ir::Contacts& b, bool check_photo = true) {
  REQUIRE(a.cards.size() == b.cards.size());
  for (std::size_t i = 0; i < a.cards.size(); ++i) {
    const ir::Card& x = a.cards[i];
    const ir::Card& y = b.cards[i];
    INFO("card ", i, " ", x.full_name);
    CHECK(x.full_name == y.full_name);
    CHECK(x.family_name == y.family_name);
    CHECK(x.given_name == y.given_name);
    CHECK(x.additional_name == y.additional_name);
    CHECK(x.prefix == y.prefix);
    CHECK(x.suffix == y.suffix);
    CHECK(x.nickname == y.nickname);
    CHECK(x.organization == y.organization);
    CHECK(x.department == y.department);
    CHECK(x.title == y.title);
    CHECK(x.note == y.note);
    CHECK(x.birthday == y.birthday);
    CHECK(x.uid == y.uid);
    CHECK(x.categories == y.categories);
    REQUIRE(x.emails.size() == y.emails.size());
    for (std::size_t k = 0; k < x.emails.size(); ++k) {
      CHECK(x.emails[k].value == y.emails[k].value);
      CHECK(x.emails[k].type == y.emails[k].type);
      CHECK(x.emails[k].label == y.emails[k].label);
    }
    REQUIRE(x.phones.size() == y.phones.size());
    for (std::size_t k = 0; k < x.phones.size(); ++k) {
      CHECK(x.phones[k].value == y.phones[k].value);
      CHECK(x.phones[k].type == y.phones[k].type);
    }
    REQUIRE(x.addresses.size() == y.addresses.size());
    for (std::size_t k = 0; k < x.addresses.size(); ++k) {
      CHECK(x.addresses[k].street == y.addresses[k].street);
      CHECK(x.addresses[k].city == y.addresses[k].city);
      CHECK(x.addresses[k].postal_code == y.addresses[k].postal_code);
      CHECK(x.addresses[k].country == y.addresses[k].country);
      CHECK(x.addresses[k].extended == y.addresses[k].extended);
      CHECK(x.addresses[k].type == y.addresses[k].type);
    }
    CHECK(x.urls.size() == y.urls.size());
    if (check_photo) {
      CHECK(x.photo == y.photo);
      CHECK(x.photo_mime == y.photo_mime);
    }
  }
}

}  // namespace

TEST_CASE("pim/vcf: decodes vCard 3.0 written by vobject") {
  const Bytes png = test::fixture("pim/contacts/dot_2x2.png");
  const auto contacts = test::read_as<ir::Contacts>("vcf", test::fixture("pim/contacts/cards_v30_vobject.vcf"));
  REQUIRE_OK(contacts);
  REQUIRE(contacts->cards.size() == 3);
  const ir::Card& ada = contacts->cards[0];
  CHECK(ada.full_name == "Dr. Ada Marie Example PhD");
  CHECK(ada.family_name == "Example");
  CHECK(ada.given_name == "Ada");
  CHECK(ada.additional_name == "Marie");
  CHECK(ada.prefix == "Dr.");
  CHECK(ada.suffix == "PhD");
  CHECK(ada.nickname == "Ada");
  CHECK(ada.organization == "Example Org");
  CHECK(ada.department == "Research");
  CHECK(ada.title == "Chief Example Officer");
  CHECK(has_value(ada.emails, "ada@example.org", "work"));
  CHECK(has_value(ada.emails, "ada.home@example.org", "home"));
  CHECK(has_value(ada.emails, "ada.lab@example.org", "", "Other"));
  CHECK(has_value(ada.phones, "+1 555 0100", "cell"));
  CHECK(has_value(ada.phones, "+1 555 0101", "work"));
  REQUIRE(ada.addresses.size() == 1);
  CHECK(ada.addresses[0].extended == "Suite 5");
  CHECK(ada.addresses[0].street == "1 Example Street");
  CHECK(ada.addresses[0].city == "Sampletown");
  CHECK(ada.addresses[0].region == "EX");
  CHECK(ada.addresses[0].postal_code == "01234");
  CHECK(ada.addresses[0].country == "Exampleland");
  CHECK(ada.addresses[0].type == "work");
  CHECK(ada.birthday == "1990-04-27");
  CHECK(ada.categories == std::vector<std::string>{"Friends", "Work"});
  CHECK(ada.note ==
        "Met at the example conference \U0001F389. This note is deliberately long so that it has to be folded across "
        "several physical lines, and it has commas, semicolons; and a second line.\nSecond line.");
  CHECK(ada.photo == png);
  CHECK(ada.photo_mime == "image/png");
  CHECK(ada.uid == "card-ada@example.org");
  CHECK(std::ranges::any_of(ada.extra, [](const ir::Property& p) { return p.name == "X-CUSTOM-FIELD" && p.value == "kept as is"; }));
  CHECK(contacts->cards[1].full_name == "سارة المثال");
  CHECK(contacts->cards[2].addresses[0].street == "2 Sample Road\nBuilding B");
  CHECK(contacts->cards[2].addresses[0].city == "Zürich");
}

TEST_CASE("pim/vcf: decodes vCard 2.1 with quoted-printable, charsets, bare types and a base64 photo") {
  const auto contacts = test::read_as<ir::Contacts>("vcf", test::fixture("pim/contacts/cards_v21.vcf"));
  REQUIRE_OK(contacts);
  REQUIRE(contacts->cards.size() == 2);
  const ir::Card& j = contacts->cards[0];
  CHECK(j.full_name == "Jürgen Müller");
  CHECK(j.family_name == "Müller");
  CHECK(has_value(j.phones, "+49 30 1234567", "cell"));
  CHECK(has_value(j.phones, "+49 30 7654321", "home"));
  CHECK(has_value(j.emails, "juergen@example.org", ""));
  REQUIRE(j.addresses.size() == 1);
  CHECK(j.addresses[0].street == "Beispielstraße 3");
  CHECK(j.addresses[0].type == "home");
  CHECK(j.note == "Line one\nLine two with a rather long text that needs soft line breaks in quoted printable");
  CHECK(j.photo == test::fixture("pim/contacts/dot_2x2.png"));
  CHECK(j.birthday == "1985-03-15");
  CHECK(contacts->cards[1].organization == "Example Org");
  CHECK(contacts->cards[1].department == "Support");
  CHECK(has_value(contacts->cards[1].phones, "+1 555 0199", "work"));
}

TEST_CASE("pim/vcf: decodes vCard 4.0 with data URIs, tel URIs and partial dates") {
  const auto contacts = test::read_as<ir::Contacts>("vcf", test::fixture("pim/contacts/cards_v40.vcf"));
  REQUIRE_OK(contacts);
  REQUIRE(contacts->cards.size() == 1);
  const ir::Card& c = contacts->cards[0];
  CHECK(c.full_name == "Chloé Exemple");
  CHECK(has_value(c.emails, "chloe@example.org", "work"));
  CHECK(has_value(c.phones, "+33-1-23-45-67-89", "cell"));
  CHECK(c.addresses.at(0).postal_code == "75001");
  CHECK(c.birthday == "--06-12");
  CHECK(c.anniversary == "2015-06-20");
  CHECK(c.photo == test::fixture("pim/contacts/dot_2x2.png"));
  CHECK(c.photo_mime == "image/png");
  CHECK(c.note == "Emoji \U0001F600 and escaped, commas; semicolons");
  CHECK(c.uid == "urn:uuid:4fbe8971-0bc3-424c-9c26-36c3e1eff6b1");
  CHECK(std::ranges::any_of(c.extra, [](const ir::Property& p) { return p.name == "GENDER" && p.value == "F"; }));
}

TEST_CASE("pim/vcf: every version round trips; options select the version and drop photos") {
  const auto source = test::read_as<ir::Contacts>("vcf", test::fixture("pim/contacts/cards_v30_vobject.vcf"));
  REQUIRE_OK(source);
  for (const char* version : {"2.1", "3.0", "4.0"}) {
    INFO("version ", version);
    const auto written = test::write_from("vcf", *source, std::string(R"({"vcard_version":")") + version + "\"}");
    REQUIRE_OK(written);
    const std::string text = test::str(*written);
    CHECK(text.find(std::string("VERSION:") + version + "\r\n") != std::string::npos);
    const auto back = test::read_as<ir::Contacts>("vcf", *written);
    REQUIRE_OK(back);
    check_same_cards(*source, *back);
  }
  const auto v21 = test::write_from("vcf", *source, R"({"vcard_version":"2.1"})");
  REQUIRE_OK(v21);
  CHECK(test::str(*v21).find("ENCODING=QUOTED-PRINTABLE") != std::string::npos);
  CHECK(test::str(*v21).find("TEL;CELL:+1 555 0100") != std::string::npos);
  const auto v4 = test::write_from("vcf", *source, R"({"vcard_version":"4.0"})");
  REQUIRE_OK(v4);
  CHECK(test::str(*v4).find("PHOTO:data:image/png;base64,") != std::string::npos);
  CHECK(test::str(*v4).find("BDAY:19900427") != std::string::npos);
  const auto no_photo = test::write_from("vcf", *source, R"({"drop_photos":true})");
  REQUIRE_OK(no_photo);
  CHECK(test::str(*no_photo).find("PHOTO") == std::string::npos);
  CHECK(no_photo->size() < test::write_from("vcf", *source)->size());
  // Grouped labels survive as Apple-style item groups.
  CHECK(test::str(*test::write_from("vcf", *source)).find("item1.X-ABLABEL:Other") != std::string::npos);
}

TEST_CASE("pim/vcf: legacy and 4.0 fixtures survive conversion through the engine") {
  for (const char* name : {"pim/contacts/cards_v21.vcf", "pim/contacts/cards_v40.vcf"}) {
    INFO(name);
    const Bytes source = test::fixture(name);
    const auto original = test::read_as<ir::Contacts>("vcf", source);
    REQUIRE_OK(original);
    const test::Converted converted = test::convert(source, "", "vcf");
    REQUIRE(converted.ok());
    CHECK(converted.format_id == "vcf");
    const auto back = test::read_as<ir::Contacts>("vcf", converted.data);
    REQUIRE_OK(back);
    check_same_cards(*original, *back);
  }
}

TEST_CASE("pim/vcf: encodings, detection and damaged input") {
  // A Latin-1 file without a charset parameter.
  const auto latin = test::read_as<ir::Contacts>("vcf", as_bytes("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Ren\xE9\r\nEND:VCARD\r\n"sv),
                                                 R"({"source_encoding":"latin1"})");
  REQUIRE_OK(latin);
  CHECK(latin->cards[0].full_name == "René");
  const Bytes utf16 = test::bytes("\xFF\xFE" "B\0E\0G\0I\0N\0:\0V\0C\0A\0R\0D\0\r\0\n\0F\0N\0:\0X\0\r\0\n\0E\0N\0D\0:\0V\0C\0A\0R\0D\0"sv);
  const auto wide = test::read_as<ir::Contacts>("vcf", utf16);
  REQUIRE_OK(wide);
  CHECK(wide->cards[0].full_name == "X");
  CHECK(test::convert(test::fixture("pim/contacts/cards_v21.vcf"), "", "vcf").format_id == "vcf");
  CHECK_STATUS(test::read_as<ir::Contacts>("vcf", as_bytes("hello"sv)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Contacts>("vcf", as_bytes(""sv), R"({"source_encoding":"klingon"})"), Status::InvalidArgument);
  for (const char* name : {"pim/contacts/cards_v30_vobject.vcf", "pim/contacts/cards_v21.vcf", "pim/contacts/cards_v40.vcf"}) {
    CHECK(test::fuzz_reader<ir::Contacts>("vcf", test::fixture(name)) > 0);
  }
}
