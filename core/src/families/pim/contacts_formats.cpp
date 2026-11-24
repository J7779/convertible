// Convertible core - pim family: jCard (RFC 7095), xCard (RFC 6351), LDIF
// (RFC 2849) address books, and the Google and Outlook contacts CSV layouts.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <tuple>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/pim/content_line.hpp"
#include "families/pim/pim.hpp"
#include "util/base64.hpp"
#include "util/json.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::pim {

namespace {

using cl::VProp;
using cl::WProp;

/// The raw (escaped) form of a structured value: components separated by
/// ';', list values inside a component by ','.
std::string raw_from(const std::vector<std::vector<std::string>>& components) {
  std::vector<std::string> parts;
  for (const auto& component : components) {
    std::vector<std::string> values;
    for (const std::string& value : component) values.push_back(cl::escape(value));
    parts.push_back(util::join(values, ","));
  }
  return util::join(parts, ";");
}

bool is_structured(std::string_view name) {
  return name == "N" || name == "ADR" || name == "ORG" || name == "GENDER" || name == "CLIENTPIDMAP";
}

std::string extended_date(std::string value) {
  const std::string normalized = cl::normalize_date(value);
  return normalized;
}

// =========================================================================== jCard

std::string json_scalar(const Json& value) {
  if (value.is_string()) return value.as_string();
  if (value.is_bool()) return value.as_bool() ? "true" : "false";
  if (value.is_number()) return util::dump_json(value);
  return {};
}

Result<std::vector<VProp>> jcard_props(const Json& card) {
  if (!card.is_array() || card.size() < 2 || card[0].as_string() != "vcard" || !card[1].is_array()) {
    return fail(Status::Decode, "a jCard must be [\"vcard\", [properties...]]");
  }
  std::vector<VProp> props;
  for (const Json& item : card[1].elements()) {
    if (!item.is_array() || item.size() < 4 || !item[0].is_string()) continue;
    VProp prop;
    prop.name = util::to_upper(item[0].as_string());
    for (const auto& [key, value] : item[1].items()) {
      if (key == "group") {
        prop.group = value.as_string();
        continue;
      }
      std::string text;
      if (value.is_array()) {
        std::vector<std::string> values;
        for (const Json& element : value.elements()) values.push_back(json_scalar(element));
        text = util::join(values, ",");
      } else {
        text = json_scalar(value);
      }
      prop.params.emplace_back(util::to_upper(key), text);
    }
    std::vector<std::vector<std::string>> components;
    if (item.size() == 4 && item[3].is_array()) {
      for (const Json& component : item[3].elements()) {
        std::vector<std::string> values;
        if (component.is_array()) {
          for (const Json& value : component.elements()) values.push_back(json_scalar(value));
        } else {
          values.push_back(json_scalar(component));
        }
        components.push_back(std::move(values));
      }
    } else {
      std::vector<std::string> values;
      for (std::size_t i = 3; i < item.size(); ++i) values.push_back(json_scalar(item[i]));
      components.push_back(std::move(values));
    }
    prop.raw = raw_from(components);
    props.push_back(std::move(prop));
  }
  return props;
}

Result<AnyIR> read_jcard(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, decode_text(ctx.input));
  CVT_TRY_ASSIGN(const Json root, util::parse_json(text));
  std::vector<const Json*> cards;
  if (root.is_array() && root.size() >= 1 && root[0].is_string()) {
    cards.push_back(&root);
  } else if (root.is_array()) {
    for (const Json& element : root.elements()) cards.push_back(&element);
  } else {
    return fail(Status::Decode, "a jCard file holds a JSON array");
  }
  CVT_TRY(ctx.limits.check_archive_entries(cards.size()));
  ir::Contacts contacts;
  for (const Json* card : cards) {
    CVT_TRY_ASSIGN(const std::vector<VProp> props, jcard_props(*card));
    contacts.cards.push_back(cl::card_from_props(props));
  }
  return contacts;
}

Json jcard_from_card(const ir::Card& card, bool drop_photos, bool strip) {
  Json properties = Json::array();
  properties.push_back(Json::array({"version", Json::object(), "text", "4.0"}));
  for (const WProp& prop : cl::props_from_card(card, "4.0", drop_photos)) {
    if (strip && (prop.name == "PRODID" || prop.name == "REV")) continue;
    Json params = Json::object();
    if (!prop.group.empty()) params["group"] = prop.group;
    for (const auto& [key, value] : prop.params) {
      if (util::iequals(key, "VALUE")) continue;
      const std::vector<std::string_view> values = util::split(value, ',', false);
      if (values.size() > 1) {
        Json list = Json::array();
        for (const std::string_view v : values) list.push_back(std::string(v));
        params[util::to_lower(key)] = std::move(list);
      } else {
        params[util::to_lower(key)] = value;
      }
    }
    Json item = Json::array({util::to_lower(prop.name), std::move(params), prop.value_type});
    if (prop.binary) {
      item.push_back("data:" + prop.media_type + ";base64," + prop.components[0][0]);
    } else if (is_structured(prop.name) && (prop.name != "ORG" || prop.components.size() > 1)) {
      Json structured = Json::array();
      for (const auto& component : prop.components) {
        if (component.size() == 1) structured.push_back(component[0]);
        else structured.push_back(Json(component));
      }
      item.push_back(std::move(structured));
    } else {
      for (const auto& component : prop.components) {
        for (const std::string& value : component) {
          item.push_back(prop.value_type == "date" ? extended_date(value) : value);
        }
      }
    }
    properties.push_back(std::move(item));
  }
  return Json::array({"vcard", std::move(properties)});
}

Result<Bytes> write_jcard(const AnyIR& value, WriteContext& ctx) {
  const ir::Contacts& contacts = value.get<ir::Contacts>();
  const bool drop = ctx.options.get_bool("drop_photos", false);
  const bool strip = options::resolved_strip_metadata(ctx.options);
  Json root;
  if (contacts.cards.size() == 1) {
    root = jcard_from_card(contacts.cards[0], drop, strip);
  } else {
    root = Json::array();
    for (const ir::Card& card : contacts.cards) root.push_back(jcard_from_card(card, drop, strip));
  }
  return to_bytes(util::dump_json(root, 2) + "\n");
}

int sniff_jcard(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_text(data, 512);
  std::string compact;
  for (const char c : head) {
    if (!util::is_ascii_space(c)) compact.push_back(c);
  }
  return compact.starts_with("[\"vcard\",") || compact.starts_with("[[\"vcard\",") ? 95 : 0;
}

// =========================================================================== xCard

constexpr std::string_view xcard_namespace = "urn:ietf:params:xml:ns:vcard-4.0";
constexpr std::array<std::string_view, 5> n_parts = {"surname", "given", "additional", "prefix", "suffix"};
constexpr std::array<std::string_view, 7> adr_parts = {"pobox", "ext", "street", "locality", "region", "code", "country"};

std::vector<std::string_view> structured_parts(std::string_view name) {
  if (name == "N") return {n_parts.begin(), n_parts.end()};
  if (name == "ADR") return {adr_parts.begin(), adr_parts.end()};
  if (name == "GENDER") return {"sex", "identity"};
  return {};
}

void xcard_property(util::XmlNode element, std::string_view group, std::vector<VProp>& out) {
  VProp prop;
  prop.group = std::string(group);
  prop.name = util::to_upper(element.local_name());
  std::vector<std::vector<std::string>> components;
  std::vector<std::string> values;
  for (util::XmlNode child : element.elements()) {
    if (child.local_name() == "parameters") {
      for (util::XmlNode param : child.elements()) {
        std::vector<std::string> param_values;
        for (util::XmlNode v : param.elements()) param_values.push_back(v.text());
        prop.params.emplace_back(util::to_upper(param.local_name()), util::join(param_values, ","));
      }
      continue;
    }
    values.push_back(child.text());
  }
  const std::vector<std::string_view> parts = structured_parts(prop.name);
  if (!parts.empty()) {
    for (const std::string_view part : parts) {
      std::vector<std::string> component;
      for (util::XmlNode child : element.children_local(part)) component.push_back(child.text());
      if (component.empty()) component.emplace_back();
      components.push_back(std::move(component));
    }
  } else if (prop.name == "ORG") {
    for (std::string& value : values) components.push_back({std::move(value)});
  } else {
    components.push_back(std::move(values));
  }
  prop.raw = raw_from(components);
  out.push_back(std::move(prop));
}

Result<AnyIR> read_xcard(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(ctx.input));
  ir::Contacts contacts;
  std::vector<util::XmlNode> cards;
  const util::XmlNode root = doc.root();
  if (root.local_name() == "vcard") cards.push_back(root);
  for (util::XmlNode child : root.elements()) {
    if (child.local_name() == "vcard") cards.push_back(child);
  }
  if (cards.empty()) return fail(Status::Decode, "no <vcard> element found; this is not an xCard document");
  CVT_TRY(ctx.limits.check_archive_entries(cards.size()));
  for (const util::XmlNode card : cards) {
    std::vector<VProp> props;
    for (util::XmlNode element : card.elements()) {
      if (element.local_name() == "group") {
        const std::string group(element.attribute("name").value());
        for (util::XmlNode inner : element.elements()) xcard_property(inner, group, props);
      } else {
        xcard_property(element, {}, props);
      }
    }
    contacts.cards.push_back(cl::card_from_props(props));
  }
  return contacts;
}

void write_xcard_property(util::XmlNode parent, const WProp& prop) {
  util::XmlNode element = parent.append_child(util::to_lower(prop.name));
  if (!element) return;  // a name XML cannot hold
  cl::Params params;
  for (const auto& p : prop.params) {
    if (!util::iequals(p.first, "VALUE")) params.push_back(p);
  }
  if (!params.empty()) {
    util::XmlNode parameters = element.append_child("parameters");
    for (const auto& [key, value] : params) {
      util::XmlNode param = parameters.append_child(util::to_lower(key));
      for (const std::string_view v : util::split(value, ',', false)) param.append_child("text").append_text(v);
    }
  }
  if (prop.binary) {
    element.append_child("uri").append_text("data:" + prop.media_type + ";base64," + prop.components[0][0]);
    return;
  }
  const std::vector<std::string_view> parts = structured_parts(prop.name);
  if (!parts.empty()) {
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i >= prop.components.size()) {
        element.append_child(parts[i]);
        continue;
      }
      for (const std::string& value : prop.components[i]) element.append_child(parts[i]).append_text(value);
    }
    return;
  }
  const std::string type = prop.value_type == "unknown" ? std::string("unknown") : prop.value_type;
  for (const auto& component : prop.components) {
    for (const std::string& value : component) element.append_child(type).append_text(value);
  }
}

Result<Bytes> write_xcard(const AnyIR& value, WriteContext& ctx) {
  const ir::Contacts& contacts = value.get<ir::Contacts>();
  const bool drop = ctx.options.get_bool("drop_photos", false);
  const bool strip = options::resolved_strip_metadata(ctx.options);
  util::XmlDocument doc;
  doc.set_declaration();
  util::XmlNode root = doc.append_child("vcards");
  root.set_attribute("xmlns", xcard_namespace);
  for (const ir::Card& card : contacts.cards) {
    util::XmlNode vcard = root.append_child("vcard");
    std::string open_group;
    util::XmlNode group_node;
    for (const WProp& prop : cl::props_from_card(card, "4.0", drop)) {
      if (strip && (prop.name == "PRODID" || prop.name == "REV")) continue;
      if (prop.group.empty()) {
        write_xcard_property(vcard, prop);
        continue;
      }
      if (prop.group != open_group || !group_node) {
        group_node = vcard.append_child("group");
        group_node.set_attribute("name", prop.group);
        open_group = prop.group;
      }
      write_xcard_property(group_node, prop);
    }
  }
  return to_bytes(util::xml_to_string(doc));
}

int sniff_xcard(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_text(data, 1024);
  if (head.find(xcard_namespace) != std::string::npos) return 95;
  return 0;
}

// =========================================================================== LDIF

struct LdifValue {
  std::string name;  // lower-case attribute without options
  std::string value;
  bool binary = false;
  Bytes bytes;
};

Result<std::vector<std::vector<LdifValue>>> parse_ldif(std::string_view text, const Limits& limits) {
  std::vector<std::string> lines;
  for (const std::string_view line : util::split_lines(text)) {
    if (!line.empty() && line[0] == ' ' && !lines.empty()) {
      lines.back().append(line.substr(1));
    } else {
      lines.emplace_back(line);
    }
  }
  std::vector<std::vector<LdifValue>> records;
  std::vector<LdifValue> current;
  const auto flush = [&]() -> Result<void> {
    if (!current.empty()) {
      records.push_back(std::move(current));
      CVT_TRY(limits.check_archive_entries(records.size()));
    }
    current.clear();
    return {};
  };
  for (const std::string& line : lines) {
    if (util::trim(line).empty()) {
      CVT_TRY(flush());
      continue;
    }
    if (line[0] == '#') continue;
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    LdifValue value;
    std::string name = util::to_lower(util::trim(std::string_view(line).substr(0, colon)));
    if (const std::size_t semicolon = name.find(';'); semicolon != std::string::npos) name.resize(semicolon);
    value.name = std::move(name);
    std::string_view rest = std::string_view(line).substr(colon + 1);
    if (rest.starts_with(':')) {
      std::string compact;
      for (const char c : rest.substr(1)) {
        if (!util::is_ascii_space(c)) compact.push_back(c);
      }
      auto decoded = util::base64_decode(compact);
      if (!decoded) continue;
      value.bytes = std::move(*decoded);
      value.value = util::sanitize_utf8(as_chars(value.bytes));
      value.binary = true;
    } else if (rest.starts_with('<')) {
      continue;  // URL references are never fetched
    } else {
      value.value = std::string(util::trim_left(rest));
    }
    if (value.name == "version" && current.empty()) continue;
    current.push_back(std::move(value));
  }
  CVT_TRY(flush());
  return records;
}

ir::Card card_from_ldif(const std::vector<LdifValue>& record) {
  ir::Card card;
  ir::PostalAddress work;
  work.type = "work";
  ir::PostalAddress home;
  home.type = "home";
  std::string birth_year;
  std::string birth_month;
  std::string birth_day;
  std::string dn;
  for (const LdifValue& v : record) {
    const std::string& n = v.name;
    const std::string& s = v.value;
    if (n == "dn") dn = s;
    else if (n == "objectclass" || n == "modifytimestamp" || n == "changetype") continue;
    else if (n == "cn" || n == "commonname") card.full_name = s;
    else if (n == "displayname") { if (card.full_name.empty()) card.full_name = s; }
    else if (n == "givenname" || n == "gn") card.given_name = s;
    else if (n == "sn" || n == "surname") card.family_name = s;
    else if (n == "mozillanickname" || n == "xmozillanickname" || n == "nickname") card.nickname = s;
    else if (n == "mail" || n == "mozillasecondemail" || n == "xmozillasecondemail") card.emails.push_back({s, "", ""});
    else if (n == "telephonenumber") card.phones.push_back({s, "work", ""});
    else if (n == "homephone") card.phones.push_back({s, "home", ""});
    else if (n == "mobile" || n == "cellphone") card.phones.push_back({s, "cell", ""});
    else if (n == "facsimiletelephonenumber" || n == "fax") card.phones.push_back({s, "fax", ""});
    else if (n == "pager" || n == "pagerphone") card.phones.push_back({s, "pager", ""});
    else if (n == "o") card.organization = s;
    else if (n == "ou") card.department = s;
    else if (n == "title") card.title = s;
    else if (n == "street" || n == "streetaddress" || n == "mozillaworkstreet") work.street = s;
    else if (n == "mozillaworkstreet2") work.extended = s;
    else if (n == "postaladdress") { if (work.street.empty()) work.street = util::replace_all(s, "$", "\n"); }
    else if (n == "l" || n == "locality") work.city = s;
    else if (n == "st") work.region = s;
    else if (n == "postalcode") work.postal_code = s;
    else if (n == "c" || n == "countryname") work.country = s;
    else if (n == "mozillahomestreet") home.street = s;
    else if (n == "mozillahomestreet2") home.extended = s;
    else if (n == "homepostaladdress") { if (home.street.empty()) home.street = util::replace_all(s, "$", "\n"); }
    else if (n == "mozillahomelocalityname") home.city = s;
    else if (n == "mozillahomestate") home.region = s;
    else if (n == "mozillahomepostalcode") home.postal_code = s;
    else if (n == "mozillahomecountryname") home.country = s;
    else if (n == "mozillaworkurl" || n == "workurl") card.urls.push_back({s, "work", ""});
    else if (n == "mozillahomeurl" || n == "homeurl") card.urls.push_back({s, "home", ""});
    else if (n == "labeleduri") card.urls.push_back({s, "", ""});
    else if (n == "description") card.note = util::replace_all(s, "\r\n", "\n");
    else if (n == "birthyear") birth_year = s;
    else if (n == "birthmonth") birth_month = s;
    else if (n == "birthday") birth_day = s;
    else if (n == "jpegphoto" || n == "photo") {
      if (v.binary && card.photo.empty()) {
        card.photo = v.bytes;
        card.photo_mime = "image/jpeg";
      }
    } else {
      card.extra.push_back({"X-LDIF-" + util::to_upper(n), {}, s});
    }
  }
  const auto pad = [](const std::string& text) { return text.size() == 1 ? "0" + text : text; };
  if (!birth_month.empty() && !birth_day.empty()) {
    card.birthday = (birth_year.empty() ? std::string("-") : birth_year) + "-" + pad(birth_month) + "-" + pad(birth_day);
  }
  if (card.full_name.empty()) {
    // "cn=Ada Example,mail=..." in the DN.
    for (const std::string_view rdn : util::split(dn, ',')) {
      const std::string_view trimmed = util::trim(rdn);
      if (util::istarts_with(trimmed, "cn=")) card.full_name = std::string(trimmed.substr(3));
    }
  }
  const auto filled = [](const ir::PostalAddress& a) {
    return !a.street.empty() || !a.city.empty() || !a.region.empty() || !a.postal_code.empty() || !a.country.empty() ||
           !a.extended.empty();
  };
  if (filled(work)) card.addresses.push_back(std::move(work));
  if (filled(home)) card.addresses.push_back(std::move(home));
  return card;
}

Result<AnyIR> read_ldif(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, decode_text(ctx.input, ctx.options.get_string("source_encoding", "auto")));
  CVT_TRY_ASSIGN(const auto records, parse_ldif(text, ctx.limits));
  ir::Contacts contacts;
  for (const auto& record : records) {
    const bool has_dn = std::ranges::any_of(record, [](const LdifValue& v) { return v.name == "dn"; });
    if (!has_dn) continue;
    contacts.cards.push_back(card_from_ldif(record));
  }
  if (contacts.cards.empty()) return fail(Status::Decode, "no LDIF entries (dn: lines) found");
  return contacts;
}

bool ldif_safe(std::string_view value) {
  if (value.empty()) return true;
  if (value.front() == ' ' || value.front() == ':' || value.front() == '<' || value.back() == ' ') return false;
  return std::ranges::all_of(value, [](char c) {
    const auto b = static_cast<unsigned char>(c);
    return b >= 0x01 && b < 0x80 && c != '\n' && c != '\r';
  });
}

void ldif_line(std::string& out, std::string_view name, std::string_view value) {
  std::string line(name);
  if (ldif_safe(value)) {
    line += ": ";
    line.append(value);
  } else {
    line += ":: " + util::base64_encode(as_bytes(value));
  }
  // Fold at 76 columns; continuation lines start with one space.
  std::size_t limit = 76;
  std::string_view rest = line;
  while (rest.size() > limit) {
    out.append(rest.substr(0, limit));
    out += "\n ";
    rest.remove_prefix(limit);
    limit = 75;
  }
  out.append(rest);
  out.push_back('\n');
}

std::string dn_escape(std::string_view value) {
  std::string out;
  for (const char c : value) {
    if (c == ',' || c == '+' || c == '"' || c == '\\' || c == '<' || c == '>' || c == ';' || c == '=') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

Result<Bytes> write_ldif(const AnyIR& value, WriteContext& ctx) {
  const ir::Contacts& contacts = value.get<ir::Contacts>();
  const bool drop = ctx.options.get_bool("drop_photos", false);
  std::string out = "version: 1\n";
  bool lost_addresses = false;
  for (const ir::Card& card : contacts.cards) {
    out.push_back('\n');
    std::string name = card.full_name;
    if (name.empty()) name = util::join(std::vector<std::string>{card.given_name, card.family_name}, " ");
    name = std::string(util::trim(name));
    std::string dn = "cn=" + dn_escape(name);
    if (!card.emails.empty()) dn += ",mail=" + dn_escape(card.emails.front().value);
    ldif_line(out, "dn", dn);
    for (const char* oc : {"top", "person", "organizationalPerson", "inetOrgPerson", "mozillaAbPersonAlpha"}) {
      ldif_line(out, "objectclass", oc);
    }
    if (!card.given_name.empty()) ldif_line(out, "givenName", card.given_name);
    if (!card.family_name.empty()) ldif_line(out, "sn", card.family_name);
    ldif_line(out, "cn", name);
    if (!card.nickname.empty()) ldif_line(out, "mozillaNickname", card.nickname);
    for (const ir::LabeledValue& email : card.emails) ldif_line(out, "mail", email.value);
    for (const ir::LabeledValue& phone : card.phones) {
      std::string_view attribute = "telephoneNumber";
      if (phone.type == "home") attribute = "homePhone";
      else if (phone.type == "cell") attribute = "mobile";
      else if (phone.type == "fax") attribute = "facsimileTelephoneNumber";
      else if (phone.type == "pager") attribute = "pager";
      ldif_line(out, attribute, phone.value);
    }
    bool wrote_work = false;
    bool wrote_home = false;
    for (const ir::PostalAddress& address : card.addresses) {
      const bool home = address.type == "home";
      if ((home && wrote_home) || (!home && wrote_work)) {
        lost_addresses = true;
        continue;
      }
      const auto field = [&](std::string_view work_name, std::string_view home_name, const std::string& v) {
        if (!v.empty()) ldif_line(out, home ? home_name : work_name, v);
      };
      field("street", "mozillaHomeStreet", address.street);
      field("mozillaWorkStreet2", "mozillaHomeStreet2", address.extended);
      field("l", "mozillaHomeLocalityName", address.city);
      field("st", "mozillaHomeState", address.region);
      field("postalCode", "mozillaHomePostalCode", address.postal_code);
      field("c", "mozillaHomeCountryName", address.country);
      (home ? wrote_home : wrote_work) = true;
    }
    if (!card.organization.empty()) ldif_line(out, "o", card.organization);
    if (!card.department.empty()) ldif_line(out, "ou", card.department);
    if (!card.title.empty()) ldif_line(out, "title", card.title);
    for (const ir::LabeledValue& url : card.urls) {
      ldif_line(out, url.type == "home" ? "mozillaHomeUrl" : url.type == "work" ? "mozillaWorkUrl" : "labeledURI", url.value);
    }
    if (!card.note.empty()) ldif_line(out, "description", card.note);
    if (card.birthday.size() == 10 && card.birthday[4] == '-') {
      if (card.birthday[0] != '-') ldif_line(out, "birthyear", card.birthday.substr(0, 4));
      ldif_line(out, "birthmonth", card.birthday.substr(5, 2));
      ldif_line(out, "birthday", card.birthday.substr(8, 2));
    }
    if (!drop && !card.photo.empty()) {
      out += "jpegPhoto:: ";
      const std::string encoded = util::base64_encode(card.photo);
      std::size_t used = 12;
      for (std::size_t i = 0; i < encoded.size();) {
        const std::size_t take = std::min(encoded.size() - i, 76 - used);
        out.append(encoded, i, take);
        i += take;
        if (i < encoded.size()) out += "\n ";
        used = 1;
      }
      out.push_back('\n');
    }
    for (const ir::Property& property : card.extra) {
      if (property.name.starts_with("X-LDIF-")) ldif_line(out, util::to_lower(property.name.substr(7)), property.value);
    }
  }
  if (lost_addresses) ctx.warn("LDIF holds one work and one home address per entry; further addresses were dropped.");
  return to_bytes(out);
}

int sniff_ldif(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_text(data, 1024);
  std::string_view text = util::trim_left(head);
  while (text.starts_with('#')) {
    const std::size_t newline = text.find('\n');
    if (newline == std::string_view::npos) return 0;
    text = util::trim_left(text.substr(newline + 1));
  }
  if (text.starts_with("version: 1")) {
    text = util::trim_left(text.substr(10));
  }
  return text.starts_with("dn:") || text.starts_with("dn::") ? 90 : 0;
}

// =========================================================================== contacts CSV

enum class CsvDialect { Google, Outlook };

struct Header {
  std::map<std::string, std::size_t> index;  // lower-case name -> column
  [[nodiscard]] std::optional<std::size_t> find(std::initializer_list<std::string_view> names) const {
    for (const std::string_view name : names) {
      if (const auto it = index.find(util::to_lower(name)); it != index.end()) return it->second;
    }
    return std::nullopt;
  }
};

std::string get(const std::vector<std::string>& row, const Header& header, std::initializer_list<std::string_view> names) {
  const auto at = header.find(names);
  if (!at || *at >= row.size()) return {};
  return std::string(util::trim(row[*at]));
}

std::vector<std::string> multi(std::string_view value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t sep = value.find(" ::: ", start);
    const std::string_view piece = util::trim(value.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start));
    if (!piece.empty()) out.emplace_back(piece);
    if (sep == std::string_view::npos) break;
    start = sep + 5;
  }
  return out;
}

/// Google label -> (type, custom label).
std::pair<std::string, std::string> google_label(std::string_view label) {
  std::string_view clean = util::trim(label);
  if (clean.starts_with("* ")) clean.remove_prefix(2);
  const std::string lower = util::to_lower(clean);
  if (lower.empty()) return {{}, {}};
  if (lower == "mobile" || lower == "cell") return {"cell", {}};
  if (lower == "home" || lower == "work" || lower == "other" || lower == "main" || lower == "pager") return {lower, {}};
  if (lower == "home fax" || lower == "work fax" || lower == "fax") return {"fax", {}};
  if (lower == "homepage" || lower == "home page") return {"home", {}};
  return {{}, std::string(clean)};
}

std::string google_label_of(const std::string& type, const std::string& label) {
  if (!label.empty()) return label;
  if (type == "cell") return "Mobile";
  if (type == "fax") return "Work Fax";
  if (type.empty()) return {};
  std::string out = type;
  out[0] = util::ascii_upper(out[0]);
  return out;
}

std::string join_name(const ir::Card& card) {
  std::vector<std::string> parts;
  for (const std::string* part : {&card.prefix, &card.given_name, &card.additional_name, &card.family_name, &card.suffix}) {
    if (!part->empty()) parts.push_back(*part);
  }
  return util::join(parts, " ");
}

/// "M/D/YYYY" (Outlook) or ISO -> ISO date; "" for Outlook's empty "0/0/00".
std::string csv_date(std::string_view text) {
  text = util::trim(text);
  if (text.empty() || text == "0/0/00" || text == "0/0/0000") return {};
  const std::vector<std::string_view> parts = util::split(text, '/');
  if (parts.size() == 3) {
    const auto m = util::parse_int(parts[0]);
    const auto d = util::parse_int(parts[1]);
    const auto y = util::parse_int(parts[2]);
    if (m && d && y && *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31) return std::format("{:04}-{:02}-{:02}", *y, *m, *d);
  }
  return cl::normalize_date(text);
}

std::string outlook_date(const std::string& iso) {
  if (iso.size() == 10 && iso[4] == '-' && iso[7] == '-' && iso[0] != '-') {
    const auto y = util::parse_int(iso.substr(0, 4));
    const auto m = util::parse_int(iso.substr(5, 2));
    const auto d = util::parse_int(iso.substr(8, 2));
    if (y && m && d) return std::format("{}/{}/{}", *m, *d, *y);
  }
  return iso;
}

ir::Card google_card(const std::vector<std::string>& row, const Header& h) {
  ir::Card card;
  card.given_name = get(row, h, {"First Name", "Given Name"});
  card.additional_name = get(row, h, {"Middle Name", "Additional Name"});
  card.family_name = get(row, h, {"Last Name", "Family Name"});
  card.prefix = get(row, h, {"Name Prefix"});
  card.suffix = get(row, h, {"Name Suffix"});
  card.nickname = get(row, h, {"Nickname"});
  card.full_name = get(row, h, {"Name"});
  if (card.full_name.empty()) card.full_name = join_name(card);
  card.organization = get(row, h, {"Organization Name", "Organization 1 - Name"});
  card.title = get(row, h, {"Organization Title", "Organization 1 - Title"});
  card.department = get(row, h, {"Organization Department", "Organization 1 - Department"});
  card.birthday = cl::normalize_date(get(row, h, {"Birthday"}));
  card.note = util::replace_all(get(row, h, {"Notes"}), "\r\n", "\n");
  for (const std::string& label : multi(get(row, h, {"Labels", "Group Membership"}))) {
    if (!label.starts_with("* ")) card.categories.push_back(label);
  }
  for (int i = 1; i <= 50; ++i) {
    const std::string prefix = std::format("E-mail {} - ", i);
    if (!h.find({prefix + "Value"})) break;
    const auto [type, label] = google_label(get(row, h, {prefix + "Label", prefix + "Type"}));
    for (std::string& value : multi(get(row, h, {prefix + "Value"}))) card.emails.push_back({std::move(value), type, label});
  }
  for (int i = 1; i <= 50; ++i) {
    const std::string prefix = std::format("Phone {} - ", i);
    if (!h.find({prefix + "Value"})) break;
    const auto [type, label] = google_label(get(row, h, {prefix + "Label", prefix + "Type"}));
    for (std::string& value : multi(get(row, h, {prefix + "Value"}))) card.phones.push_back({std::move(value), type, label});
  }
  for (int i = 1; i <= 50; ++i) {
    const std::string prefix = std::format("Address {} - ", i);
    if (!h.find({prefix + "Street", prefix + "Formatted", prefix + "City"})) break;
    ir::PostalAddress address;
    std::tie(address.type, address.label) = google_label(get(row, h, {prefix + "Label", prefix + "Type"}));
    address.street = get(row, h, {prefix + "Street"});
    address.city = get(row, h, {prefix + "City"});
    address.po_box = get(row, h, {prefix + "PO Box"});
    address.region = get(row, h, {prefix + "Region"});
    address.postal_code = get(row, h, {prefix + "Postal Code"});
    address.country = get(row, h, {prefix + "Country"});
    address.extended = get(row, h, {prefix + "Extended Address"});
    if (address.street.empty() && address.city.empty() && address.postal_code.empty() && address.country.empty()) {
      address.street = get(row, h, {prefix + "Formatted"});
    }
    if (!address.street.empty() || !address.city.empty() || !address.region.empty() || !address.postal_code.empty() ||
        !address.country.empty() || !address.po_box.empty()) {
      card.addresses.push_back(std::move(address));
    }
  }
  for (int i = 1; i <= 50; ++i) {
    const std::string prefix = std::format("Website {} - ", i);
    if (!h.find({prefix + "Value"})) break;
    const auto [type, label] = google_label(get(row, h, {prefix + "Label", prefix + "Type"}));
    for (std::string& value : multi(get(row, h, {prefix + "Value"}))) card.urls.push_back({std::move(value), type, label});
  }
  return card;
}

ir::Card outlook_card(const std::vector<std::string>& row, const Header& h) {
  ir::Card card;
  card.given_name = get(row, h, {"First Name"});
  card.additional_name = get(row, h, {"Middle Name"});
  card.family_name = get(row, h, {"Last Name"});
  card.prefix = get(row, h, {"Title"});
  card.suffix = get(row, h, {"Suffix"});
  card.nickname = get(row, h, {"Nickname"});
  card.full_name = join_name(card);
  card.organization = get(row, h, {"Company"});
  card.department = get(row, h, {"Department"});
  card.title = get(row, h, {"Job Title"});
  card.birthday = csv_date(get(row, h, {"Birthday"}));
  card.anniversary = csv_date(get(row, h, {"Anniversary"}));
  card.note = util::replace_all(get(row, h, {"Notes"}), "\r\n", "\n");
  for (const std::string_view category : util::split(get(row, h, {"Categories"}), ';', false)) {
    if (!util::trim(category).empty()) card.categories.emplace_back(util::trim(category));
  }
  for (const std::string_view column : {"E-mail Address", "E-mail 2 Address", "E-mail 3 Address"}) {
    const std::string value = get(row, h, {column});
    if (!value.empty()) card.emails.push_back({value, "", ""});
  }
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 13> phones = {{
      {"Business Phone", "work"},
      {"Business Phone 2", "work"},
      {"Home Phone", "home"},
      {"Home Phone 2", "home"},
      {"Mobile Phone", "cell"},
      {"Business Fax", "fax"},
      {"Home Fax", "fax"},
      {"Other Fax", "fax"},
      {"Pager", "pager"},
      {"Primary Phone", ""},
      {"Company Main Phone", "main"},
      {"Car Phone", ""},
      {"Other Phone", ""},
  }};
  for (const auto& [column, type] : phones) {
    const std::string value = get(row, h, {column});
    if (!value.empty()) card.phones.push_back({value, std::string(type), ""});
  }
  for (const auto& [prefix, type] : {std::pair<std::string_view, std::string_view>{"Business", "work"}, {"Home", "home"},
                                     {"Other", ""}}) {
    const std::string p(prefix);
    ir::PostalAddress address;
    address.type = std::string(type);
    address.street = get(row, h, {p + " Street"});
    address.extended = get(row, h, {p + " Street 2"});
    address.city = get(row, h, {p + " City"});
    address.region = get(row, h, {p + " State"});
    address.postal_code = get(row, h, {p + " Postal Code"});
    address.country = get(row, h, {p + " Country/Region", p + " Country"});
    address.po_box = get(row, h, {p + " Address PO Box"});
    if (!address.street.empty() || !address.city.empty() || !address.postal_code.empty() || !address.country.empty()) {
      card.addresses.push_back(std::move(address));
    }
  }
  const std::string web = get(row, h, {"Web Page"});
  if (!web.empty()) card.urls.push_back({web, "", ""});
  return card;
}

Result<AnyIR> read_contacts_csv(ReadContext& ctx) {
  const CsvDialect dialect = ctx.format_id == "outlook-contacts-csv" ? CsvDialect::Outlook : CsvDialect::Google;
  CVT_TRY_ASSIGN(const std::string text, decode_text(ctx.input, ctx.options.get_string("source_encoding", "auto")));
  const std::vector<std::vector<std::string>> rows = parse_csv(text);
  if (rows.empty()) return fail(Status::Decode, "the contacts CSV file is empty");
  CVT_TRY(ctx.limits.check_archive_entries(rows.size()));
  Header header;
  for (std::size_t i = 0; i < rows[0].size(); ++i) header.index.emplace(util::to_lower(util::trim(rows[0][i])), i);
  if (!header.find({"First Name", "Given Name", "Name", "Last Name", "Family Name"})) {
    return fail(Status::Decode, "contacts CSV needs name columns such as 'First Name' and 'Last Name'");
  }
  ir::Contacts contacts;
  for (std::size_t r = 1; r < rows.size(); ++r) {
    if ((r & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(rows.size())));
    contacts.cards.push_back(dialect == CsvDialect::Google ? google_card(rows[r], header) : outlook_card(rows[r], header));
  }
  return contacts;
}

Result<Bytes> write_google_csv(const ir::Contacts& contacts) {
  std::size_t emails = 1;
  std::size_t phones = 1;
  std::size_t addresses = 1;
  std::size_t websites = 1;
  for (const ir::Card& card : contacts.cards) {
    emails = std::max(emails, card.emails.size());
    phones = std::max(phones, card.phones.size());
    addresses = std::max(addresses, card.addresses.size());
    websites = std::max(websites, card.urls.size());
  }
  std::vector<std::string> header = {"First Name",        "Middle Name",            "Last Name",        "Name Prefix",
                                     "Name Suffix",       "Nickname",               "Organization Name", "Organization Department",
                                     "Organization Title", "Birthday",              "Notes",            "Labels"};
  for (std::size_t i = 1; i <= emails; ++i) {
    header.push_back(std::format("E-mail {} - Label", i));
    header.push_back(std::format("E-mail {} - Value", i));
  }
  for (std::size_t i = 1; i <= phones; ++i) {
    header.push_back(std::format("Phone {} - Label", i));
    header.push_back(std::format("Phone {} - Value", i));
  }
  for (std::size_t i = 1; i <= addresses; ++i) {
    for (const char* part : {"Label", "Street", "City", "PO Box", "Region", "Postal Code", "Country", "Extended Address"}) {
      header.push_back(std::format("Address {} - {}", i, part));
    }
  }
  for (std::size_t i = 1; i <= websites; ++i) {
    header.push_back(std::format("Website {} - Label", i));
    header.push_back(std::format("Website {} - Value", i));
  }
  std::vector<std::vector<std::string>> rows{header};
  for (const ir::Card& card : contacts.cards) {
    std::vector<std::string> labels{"* myContacts"};
    labels.insert(labels.end(), card.categories.begin(), card.categories.end());
    std::vector<std::string> row = {card.given_name,   card.additional_name, card.family_name, card.prefix,
                                    card.suffix,       card.nickname,        card.organization, card.department,
                                    card.title,        card.birthday,        card.note,         util::join(labels, " ::: ")};
    if (card.given_name.empty() && card.family_name.empty() && !card.full_name.empty()) row[0] = card.full_name;
    for (std::size_t i = 0; i < emails; ++i) {
      const ir::LabeledValue* v = i < card.emails.size() ? &card.emails[i] : nullptr;
      row.push_back(v ? google_label_of(v->type, v->label) : std::string());
      row.push_back(v ? v->value : std::string());
    }
    for (std::size_t i = 0; i < phones; ++i) {
      const ir::LabeledValue* v = i < card.phones.size() ? &card.phones[i] : nullptr;
      row.push_back(v ? google_label_of(v->type, v->label) : std::string());
      row.push_back(v ? v->value : std::string());
    }
    for (std::size_t i = 0; i < addresses; ++i) {
      const ir::PostalAddress* a = i < card.addresses.size() ? &card.addresses[i] : nullptr;
      if (a == nullptr) {
        row.insert(row.end(), 8, std::string());
        continue;
      }
      row.push_back(google_label_of(a->type, a->label));
      for (const std::string* part : {&a->street, &a->city, &a->po_box, &a->region, &a->postal_code, &a->country, &a->extended}) {
        row.push_back(*part);
      }
    }
    for (std::size_t i = 0; i < websites; ++i) {
      const ir::LabeledValue* v = i < card.urls.size() ? &card.urls[i] : nullptr;
      row.push_back(v ? google_label_of(v->type, v->label) : std::string());
      row.push_back(v ? v->value : std::string());
    }
    rows.push_back(std::move(row));
  }
  return to_bytes(write_csv(rows));
}

Result<Bytes> write_outlook_csv(const ir::Contacts& contacts, const WriteContext& ctx) {
  const std::vector<std::string> header = {
      "First Name",        "Middle Name",         "Last Name",           "Title",           "Suffix",
      "Nickname",          "Company",             "Department",          "Job Title",       "Business Street",
      "Business Street 2", "Business City",       "Business State",      "Business Postal Code",
      "Business Country/Region", "Home Street",   "Home Street 2",       "Home City",       "Home State",
      "Home Postal Code",  "Home Country/Region", "Other Street",        "Other City",      "Other State",
      "Other Postal Code", "Other Country/Region", "Business Phone",     "Business Phone 2", "Business Fax",
      "Home Phone",        "Home Phone 2",        "Mobile Phone",        "Pager",           "Other Phone",
      "E-mail Address",    "E-mail 2 Address",    "E-mail 3 Address",    "Birthday",        "Anniversary",
      "Notes",             "Web Page",            "Categories"};
  std::map<std::string, std::size_t> at;
  for (std::size_t i = 0; i < header.size(); ++i) at[header[i]] = i;
  std::vector<std::vector<std::string>> rows{header};
  bool lost = false;
  for (const ir::Card& card : contacts.cards) {
    std::vector<std::string> row(header.size());
    const auto set = [&](const std::string& column, const std::string& value) {
      std::string& slot = row[at[column]];
      if (!slot.empty()) return false;
      slot = value;
      return true;
    };
    set("First Name", card.given_name.empty() && card.family_name.empty() ? card.full_name : card.given_name);
    set("Middle Name", card.additional_name);
    set("Last Name", card.family_name);
    set("Title", card.prefix);
    set("Suffix", card.suffix);
    set("Nickname", card.nickname);
    set("Company", card.organization);
    set("Department", card.department);
    set("Job Title", card.title);
    set("Birthday", outlook_date(card.birthday));
    set("Anniversary", outlook_date(card.anniversary));
    set("Notes", card.note);
    set("Categories", util::join(card.categories, ";"));
    if (!card.urls.empty()) set("Web Page", card.urls.front().value);
    lost = lost || card.urls.size() > 1 || card.emails.size() > 3;
    const std::array<std::string, 3> email_columns = {"E-mail Address", "E-mail 2 Address", "E-mail 3 Address"};
    for (std::size_t i = 0; i < card.emails.size() && i < 3; ++i) set(email_columns[i], card.emails[i].value);
    for (const ir::LabeledValue& phone : card.phones) {
      bool placed = false;
      if (phone.type == "work") placed = set("Business Phone", phone.value) || set("Business Phone 2", phone.value);
      else if (phone.type == "home") placed = set("Home Phone", phone.value) || set("Home Phone 2", phone.value);
      else if (phone.type == "cell") placed = set("Mobile Phone", phone.value);
      else if (phone.type == "fax") placed = set("Business Fax", phone.value);
      else if (phone.type == "pager") placed = set("Pager", phone.value);
      if (!placed) placed = set("Other Phone", phone.value);
      lost = lost || !placed;
    }
    bool used_business = false;
    bool used_home = false;
    bool used_other = false;
    for (const ir::PostalAddress& address : card.addresses) {
      std::string prefix;
      if (address.type == "work" && !used_business) prefix = "Business", used_business = true;
      else if (address.type == "home" && !used_home) prefix = "Home", used_home = true;
      else if (!used_other) prefix = "Other", used_other = true;
      else {
        lost = true;
        continue;
      }
      set(prefix + " Street", address.street);
      if (prefix != "Other") set(prefix + " Street 2", address.extended);
      set(prefix + " City", address.city);
      set(prefix + " State", address.region);
      set(prefix + " Postal Code", address.postal_code);
      set(prefix + " Country/Region", address.country);
    }
    rows.push_back(std::move(row));
  }
  if (lost) ctx.warn("Some e-mail addresses, phone numbers, addresses or web pages did not fit Outlook's fixed columns.");
  return to_bytes(write_csv(rows));
}

Result<Bytes> write_contacts_csv(const AnyIR& value, WriteContext& ctx) {
  const ir::Contacts& contacts = value.get<ir::Contacts>();
  if (ctx.format_id == "outlook-contacts-csv") return write_outlook_csv(contacts, ctx);
  return write_google_csv(contacts);
}

int sniff_contacts_csv(ByteView data, bool outlook) {
  const std::string head = sniff_text(data, 4096);
  const std::string first = util::to_lower(std::string_view(head).substr(0, head.find_first_of("\r\n")));
  const bool google = first.find("e-mail 1 - value") != std::string::npos ||
                      (first.find("given name") != std::string::npos && first.find("family name") != std::string::npos);
  const bool looks_outlook = !google && first.find("first name") != std::string::npos &&
                             (first.find("e-mail address") != std::string::npos || first.find("job title") != std::string::npos);
  if (outlook) return looks_outlook ? 90 : 0;
  return google ? 92 : 0;
}

void add_format(Registry& registry, std::string id, std::string name, std::string description,
                std::vector<std::string> extensions, std::string mime, int popularity,
                std::function<int(ByteView, std::string_view)> sniff) {
  FormatInfo info;
  info.id = std::move(id);
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Pim;
  info.extensions = std::move(extensions);
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.sniff = std::move(sniff);
  registry.add_format(std::move(info));
}

void add_codec(Registry& registry, std::string id, std::function<Result<AnyIR>(ReadContext&)> read,
               std::function<Result<Bytes>(const AnyIR&, WriteContext&)> write, bool lossy, bool with_encoding) {
  Codec codec;
  codec.format_id = std::move(id);
  codec.kind = Kind::Contacts;
  codec.read = std::move(read);
  codec.write = std::move(write);
  codec.lossy = lossy;
  if (lossy) codec.write_cost = 20;
  if (with_encoding) codec.read_options = {source_encoding_option()};
  codec.write_options = contacts_write_options_with_photos();
  registry.add_codec(std::move(codec));
}

}  // namespace

void register_contacts_formats(Registry& registry) {
  add_format(registry, "jcard", "jCard", "vCard data as JSON (RFC 7095).", {"jcard", "json"}, "application/vcard+json",
             20, sniff_jcard);
  add_codec(registry, "jcard", read_jcard, write_jcard, false, false);

  add_format(registry, "xcard", "xCard", "vCard data as XML (RFC 6351).", {"xcard", "xml"}, "application/vcard+xml", 15,
             sniff_xcard);
  add_codec(registry, "xcard", read_xcard, write_xcard, false, false);

  add_format(registry, "ldif", "LDIF address book", "LDAP Data Interchange Format entries, as exported by Thunderbird.",
             {"ldif", "ldi"}, "text/x-ldif", 30, sniff_ldif);
  add_codec(registry, "ldif", read_ldif, write_ldif, true, true);

  add_format(registry, "google-contacts-csv", "Google Contacts CSV", "Contacts in the CSV layout of Google Contacts.",
             {"csv"}, "text/csv", 35, [](ByteView data, std::string_view) { return sniff_contacts_csv(data, false); });
  add_codec(registry, "google-contacts-csv", read_contacts_csv, write_contacts_csv, true, true);

  add_format(registry, "outlook-contacts-csv", "Outlook contacts CSV", "Contacts in the CSV layout of Microsoft Outlook.",
             {"csv"}, "text/csv", 30, [](ByteView data, std::string_view) { return sniff_contacts_csv(data, true); });
  add_codec(registry, "outlook-contacts-csv", read_contacts_csv, write_contacts_csv, true, true);
}

}  // namespace cvt::pim
