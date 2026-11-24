// Convertible core - pim family: bridges from calendars and contacts to
// tables (one row per event / card), data (JSON-shaped records) and
// documents (an agenda grouped by day, an address list).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/pim/content_line.hpp"
#include "families/pim/pim.hpp"
#include "ir/any.hpp"
#include "ir/calendar.hpp"
#include "ir/contacts.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/table.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::pim {

namespace {

using namespace ir::build;

// --------------------------------------------------------------------------- calendars

ir::Cell time_cell(const ir::CalendarTime& time) {
  if (time.date_only || time.utc) {
    return ir::Cell::datetime(static_cast<double>(cl::civil_seconds(time)), cl::iso_time(time));
  }
  return ir::Cell::string(cl::iso_time(time));
}

ir::Cell text_cell(const std::string& text) { return text.empty() ? ir::Cell{} : ir::Cell::string(text); }

Result<AnyIR> calendar_to_table(AnyIR value, Context& ctx) {
  const ir::Calendar calendar = std::move(value).get<ir::Calendar>();
  ir::Sheet sheet;
  sheet.name = calendar.name.empty() ? std::string("Events") : calendar.name;
  sheet.has_header = true;
  std::vector<ir::Cell> header;
  for (const char* title : {"Type", "UID", "Summary", "Start", "End", "All day", "Time zone", "Location", "Description",
                            "Recurrence", "Status", "Organizer", "Attendees", "Categories", "URL"}) {
    header.push_back(ir::Cell::string(title));
  }
  sheet.rows.push_back(std::move(header));
  for (std::size_t i = 0; i < calendar.events.size(); ++i) {
    if ((i & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(calendar.events.size())));
    const ir::Event& event = calendar.events[i];
    std::vector<ir::Cell> row;
    row.push_back(text_cell(event.component));
    row.push_back(text_cell(event.uid));
    row.push_back(text_cell(event.summary));
    row.push_back(time_cell(event.start));
    row.push_back(event.end ? time_cell(*event.end) : event.due ? time_cell(*event.due) : ir::Cell{});
    row.push_back(ir::Cell::boolean(event.start.date_only));
    row.push_back(text_cell(event.start.utc ? std::string("UTC") : event.start.tzid));
    row.push_back(text_cell(event.location));
    row.push_back(text_cell(event.description));
    row.push_back(text_cell(event.rrule));
    row.push_back(text_cell(event.status));
    row.push_back(text_cell(event.organizer));
    row.push_back(text_cell(util::join(event.attendees, "; ")));
    row.push_back(text_cell(util::join(event.categories, ", ")));
    row.push_back(text_cell(event.url));
    sheet.rows.push_back(std::move(row));
  }
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

Json time_list(const std::vector<ir::CalendarTime>& times) {
  Json out = Json::array();
  for (const ir::CalendarTime& time : times) out.push_back(cl::iso_time(time));
  return out;
}

Json properties_json(const std::vector<ir::Property>& properties) {
  Json out = Json::array();
  for (const ir::Property& property : properties) {
    Json params = Json::object();
    for (const auto& [key, value] : property.params) params[key] = value;
    Json item = Json::object({{"name", property.name}, {"value", property.value}});
    if (!params.empty()) item["params"] = std::move(params);
    out.push_back(std::move(item));
  }
  return out;
}

void set_text(Json& object, std::string_view key, const std::string& value) {
  if (!value.empty()) object[key] = value;
}

Json event_json(const ir::Event& event) {
  Json out = Json::object();
  out["type"] = event.component;
  set_text(out, "uid", event.uid);
  set_text(out, "summary", event.summary);
  set_text(out, "description", event.description);
  set_text(out, "location", event.location);
  out["start"] = cl::iso_time(event.start);
  out["all_day"] = event.start.date_only;
  if (!event.start.tzid.empty()) out["timezone"] = event.start.tzid;
  if (event.end) out["end"] = cl::iso_time(*event.end);
  if (event.due) out["due"] = cl::iso_time(*event.due);
  set_text(out, "rrule", event.rrule);
  if (!event.exdates.empty()) out["exdates"] = time_list(event.exdates);
  if (!event.rdates.empty()) out["rdates"] = time_list(event.rdates);
  if (event.recurrence_id) out["recurrence_id"] = cl::iso_time(*event.recurrence_id);
  set_text(out, "organizer", event.organizer);
  if (!event.attendees.empty()) out["attendees"] = Json(event.attendees);
  if (!event.categories.empty()) out["categories"] = Json(event.categories);
  set_text(out, "status", event.status);
  set_text(out, "url", event.url);
  if (event.created) out["created"] = cl::iso_time(*event.created);
  if (event.last_modified) out["last_modified"] = cl::iso_time(*event.last_modified);
  if (!event.alarms.empty()) {
    Json alarms = Json::array();
    for (const ir::Alarm& alarm : event.alarms) {
      Json item = Json::object({{"minutes_before", alarm.minutes_before}, {"action", alarm.action}});
      set_text(item, "description", alarm.description);
      alarms.push_back(std::move(item));
    }
    out["alarms"] = std::move(alarms);
  }
  if (!event.extra.empty()) out["properties"] = properties_json(event.extra);
  return out;
}

Result<AnyIR> calendar_to_data(AnyIR value, Context& /*ctx*/) {
  const ir::Calendar calendar = std::move(value).get<ir::Calendar>();
  Json root = Json::object();
  set_text(root, "name", calendar.name);
  Json events = Json::array();
  for (const ir::Event& event : calendar.events) events.push_back(event_json(event));
  root["events"] = std::move(events);
  if (!calendar.timezones.empty()) {
    Json zones = Json::array();
    for (const ir::CalendarComponent& zone : calendar.timezones) {
      for (const ir::Property& property : zone.properties) {
        if (property.name == "TZID") zones.push_back(property.value);
      }
    }
    root["timezones"] = std::move(zones);
  }
  return ir::Data{std::move(root)};
}

std::string day_heading(const ir::CalendarTime& time) {
  static constexpr std::array<std::string_view, 7> weekdays = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                                               "Thursday", "Friday", "Saturday"};
  static constexpr std::array<std::string_view, 12> months = {"January", "February", "March",     "April",
                                                              "May",     "June",     "July",      "August",
                                                              "September", "October", "November", "December"};
  const std::int64_t days = days_from_civil(time.year, static_cast<unsigned>(std::clamp(time.month, 1, 12)),
                                            static_cast<unsigned>(std::clamp(time.day, 1, 31)));
  return std::format("{}, {} {} {}", weekdays[static_cast<std::size_t>(weekday_from_days(days))], time.day,
                     months[static_cast<std::size_t>(std::clamp(time.month, 1, 12) - 1)], time.year);
}

std::string clock(const ir::CalendarTime& time) { return std::format("{:02}:{:02}", time.hour, time.minute); }

Result<AnyIR> calendar_to_document(AnyIR value, Context& /*ctx*/) {
  ir::Calendar calendar = std::move(value).get<ir::Calendar>();
  std::stable_sort(calendar.events.begin(), calendar.events.end(), [](const ir::Event& a, const ir::Event& b) {
    return cl::civil_seconds(a.start) < cl::civil_seconds(b.start);
  });
  ir::Document document;
  document.metadata.title = calendar.name.empty() ? std::string("Agenda") : calendar.name;
  document.blocks.push_back(heading(1, document.metadata.title));
  std::string current_day;
  std::vector<ir::Blocks> items;
  const auto flush = [&] {
    if (!items.empty()) document.blocks.push_back(list(false, std::move(items)));
    items.clear();
  };
  for (const ir::Event& event : calendar.events) {
    const std::string day = day_heading(event.start);
    if (day != current_day) {
      flush();
      document.blocks.push_back(heading(2, day));
      current_day = day;
    }
    std::string when;
    if (event.start.date_only) {
      when = "All day";
    } else {
      when = clock(event.start);
      if (event.end && !event.end->date_only) when += "–" + clock(*event.end);
      if (event.start.utc) when += " UTC";
      else if (!event.start.tzid.empty()) when += " (" + event.start.tzid + ")";
    }
    ir::Inlines line{text(when + "  "), strong(event.summary.empty() ? std::string("(untitled)") : event.summary)};
    if (event.component == "VTODO") line.insert(line.begin(), text("[task] "));
    if (!event.location.empty()) line.push_back(text(" — " + event.location));
    ir::Blocks item{paragraph(std::move(line))};
    if (!event.description.empty()) item.push_back(paragraph(event.description));
    std::vector<std::string> notes;
    if (!event.rrule.empty()) notes.push_back("Repeats: " + event.rrule);
    if (!event.organizer.empty()) notes.push_back("Organizer: " + event.organizer);
    if (!event.attendees.empty()) notes.push_back("Attendees: " + util::join(event.attendees, ", "));
    if (!notes.empty()) item.push_back(paragraph(ir::Inlines{emphasis(util::join(notes, "; "))}));
    items.push_back(std::move(item));
  }
  flush();
  if (calendar.events.empty()) document.blocks.push_back(paragraph("No events."));
  return document;
}

// --------------------------------------------------------------------------- contacts

std::string labeled(const ir::LabeledValue& value) {
  const std::string& tag = value.label.empty() ? value.type : value.label;
  return tag.empty() ? value.value : value.value + " (" + tag + ")";
}

std::string address_text(const ir::PostalAddress& address, std::string_view separator) {
  std::vector<std::string> parts;
  for (const std::string* part : {&address.po_box, &address.extended, &address.street}) {
    if (!part->empty()) parts.push_back(*part);
  }
  std::string city_line;
  for (const std::string* part : {&address.postal_code, &address.city, &address.region}) {
    if (part->empty()) continue;
    if (!city_line.empty()) city_line.push_back(' ');
    city_line += *part;
  }
  if (!city_line.empty()) parts.push_back(city_line);
  if (!address.country.empty()) parts.push_back(address.country);
  std::string out = util::join(parts, separator);
  const std::string& tag = address.label.empty() ? address.type : address.label;
  if (!tag.empty()) out += " (" + tag + ")";
  return out;
}

std::string display_name(const ir::Card& card) {
  if (!card.full_name.empty()) return card.full_name;
  std::vector<std::string> parts;
  for (const std::string* part : {&card.prefix, &card.given_name, &card.additional_name, &card.family_name, &card.suffix}) {
    if (!part->empty()) parts.push_back(*part);
  }
  if (!parts.empty()) return util::join(parts, " ");
  if (!card.organization.empty()) return card.organization;
  return card.emails.empty() ? std::string("(no name)") : card.emails.front().value;
}

Result<AnyIR> contacts_to_table(AnyIR value, Context& ctx) {
  const ir::Contacts contacts = std::move(value).get<ir::Contacts>();
  std::vector<std::vector<std::string>> rows;
  for (std::size_t i = 0; i < contacts.cards.size(); ++i) {
    if ((i & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(contacts.cards.size())));
    const ir::Card& card = contacts.cards[i];
    const auto join_values = [](const std::vector<ir::LabeledValue>& values) {
      std::vector<std::string> out;
      for (const ir::LabeledValue& v : values) out.push_back(labeled(v));
      return util::join(out, "; ");
    };
    std::vector<std::string> addresses;
    for (const ir::PostalAddress& address : card.addresses) addresses.push_back(address_text(address, ", "));
    rows.push_back({display_name(card), card.given_name, card.family_name, card.additional_name, card.prefix,
                    card.suffix, card.nickname, card.organization, card.department, card.title, card.role,
                    join_values(card.emails), join_values(card.phones), util::join(addresses, "; "),
                    join_values(card.urls), card.birthday, card.anniversary, util::join(card.categories, ", "), card.note,
                    card.uid});
  }
  return string_table("Contacts",
                      {"Full name", "Given name", "Family name", "Additional names", "Prefix", "Suffix", "Nickname",
                       "Organization", "Department", "Title", "Role", "E-mail", "Phone", "Address", "URL", "Birthday",
                       "Anniversary", "Categories", "Note", "UID"},
                      rows);
}

Json values_json(const std::vector<ir::LabeledValue>& values) {
  Json out = Json::array();
  for (const ir::LabeledValue& v : values) {
    Json item = Json::object({{"value", v.value}});
    set_text(item, "type", v.type);
    set_text(item, "label", v.label);
    out.push_back(std::move(item));
  }
  return out;
}

Result<AnyIR> contacts_to_data(AnyIR value, Context& /*ctx*/) {
  const ir::Contacts contacts = std::move(value).get<ir::Contacts>();
  Json cards = Json::array();
  for (const ir::Card& card : contacts.cards) {
    Json out = Json::object();
    out["full_name"] = display_name(card);
    Json name = Json::object();
    set_text(name, "family", card.family_name);
    set_text(name, "given", card.given_name);
    set_text(name, "additional", card.additional_name);
    set_text(name, "prefix", card.prefix);
    set_text(name, "suffix", card.suffix);
    if (!name.empty()) out["name"] = std::move(name);
    set_text(out, "nickname", card.nickname);
    set_text(out, "organization", card.organization);
    set_text(out, "department", card.department);
    set_text(out, "title", card.title);
    set_text(out, "role", card.role);
    if (!card.emails.empty()) out["emails"] = values_json(card.emails);
    if (!card.phones.empty()) out["phones"] = values_json(card.phones);
    if (!card.addresses.empty()) {
      Json addresses = Json::array();
      for (const ir::PostalAddress& a : card.addresses) {
        Json item = Json::object();
        set_text(item, "type", a.type);
        set_text(item, "label", a.label);
        set_text(item, "po_box", a.po_box);
        set_text(item, "extended", a.extended);
        set_text(item, "street", a.street);
        set_text(item, "city", a.city);
        set_text(item, "region", a.region);
        set_text(item, "postal_code", a.postal_code);
        set_text(item, "country", a.country);
        addresses.push_back(std::move(item));
      }
      out["addresses"] = std::move(addresses);
    }
    if (!card.urls.empty()) out["urls"] = values_json(card.urls);
    set_text(out, "birthday", card.birthday);
    set_text(out, "anniversary", card.anniversary);
    if (!card.categories.empty()) out["categories"] = Json(card.categories);
    set_text(out, "note", card.note);
    if (!card.photo.empty()) {
      out["photo"] = Json::object({{"mime", card.photo_mime}, {"base64", util::base64_encode(card.photo)}});
    } else if (!card.photo_uri.empty()) {
      out["photo"] = Json::object({{"uri", card.photo_uri}});
    }
    set_text(out, "uid", card.uid);
    if (!card.extra.empty()) out["properties"] = properties_json(card.extra);
    cards.push_back(std::move(out));
  }
  return ir::Data{Json::object({{"contacts", std::move(cards)}})};
}

Result<AnyIR> contacts_to_document(AnyIR value, Context& /*ctx*/) {
  ir::Contacts contacts = std::move(value).get<ir::Contacts>();
  ir::Document document;
  document.metadata.title = "Contacts";
  document.blocks.push_back(heading(1, "Contacts"));
  for (ir::Card& card : contacts.cards) {
    document.blocks.push_back(heading(2, display_name(card)));
    if (!card.photo.empty()) {
      document.blocks.push_back(image_block(ir::EncodedImage{std::move(card.photo), card.photo_mime}, display_name(card)));
    }
    std::vector<ir::Blocks> items;
    const auto add = [&items](std::string_view label, const std::string& text_value) {
      if (text_value.empty()) return;
      items.push_back({paragraph({strong(std::string(label) + ": "), text(text_value)})});
    };
    std::string organization = card.organization;
    if (!card.department.empty()) organization += organization.empty() ? card.department : ", " + card.department;
    add("Organization", organization);
    add("Title", card.title);
    for (const ir::LabeledValue& email : card.emails) add("E-mail", labeled(email));
    for (const ir::LabeledValue& phone : card.phones) add("Phone", labeled(phone));
    for (const ir::PostalAddress& address : card.addresses) add("Address", address_text(address, ", "));
    for (const ir::LabeledValue& url : card.urls) add("Web", labeled(url));
    add("Birthday", card.birthday);
    add("Nickname", card.nickname);
    add("Categories", util::join(card.categories, ", "));
    if (!items.empty()) document.blocks.push_back(list(false, std::move(items)));
    if (!card.note.empty()) document.blocks.push_back(paragraph(card.note));
  }
  return document;
}

void add_bridge(Registry& registry, Kind from, Kind to, int cost, bool lossy, std::string label,
                std::function<Result<AnyIR>(AnyIR, Context&)> fn) {
  Bridge bridge;
  bridge.from = from;
  bridge.to = to;
  bridge.cost = cost;
  bridge.lossy = lossy;
  bridge.label = std::move(label);
  bridge.fn = std::move(fn);
  registry.add_bridge(std::move(bridge));
}

}  // namespace

void register_pim_bridges(Registry& registry) {
  add_bridge(registry, Kind::Calendar, Kind::Table, 15, true, "list calendar events as a table", calendar_to_table);
  add_bridge(registry, Kind::Calendar, Kind::Data, 12, true, "calendar events as structured data", calendar_to_data);
  add_bridge(registry, Kind::Calendar, Kind::Document, 20, true, "render calendar as an agenda", calendar_to_document);
  add_bridge(registry, Kind::Contacts, Kind::Table, 15, true, "list contacts as a table", contacts_to_table);
  add_bridge(registry, Kind::Contacts, Kind::Data, 12, true, "contacts as structured data", contacts_to_data);
  add_bridge(registry, Kind::Contacts, Kind::Document, 20, true, "render contacts as an address list",
             contacts_to_document);
}

}  // namespace cvt::pim
