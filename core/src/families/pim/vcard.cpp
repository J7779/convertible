// Convertible core - pim family: vCard 2.1, 3.0 and 4.0 (.vcf), and the
// property model shared with jCard and xCard.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
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
#include "util/text.hpp"

namespace cvt::pim {

namespace cl {

namespace {

/// Apple wraps standard labels as "_$!<HomePage>!$_".
std::string clean_label(std::string_view label) {
  if (label.starts_with("_$!<") && label.ends_with(">!$_") && label.size() >= 8) {
    return std::string(label.substr(4, label.size() - 8));
  }
  return std::string(label);
}

/// The most specific TYPE: "home", "work", "cell", "fax", ...; "" when none.
std::string primary_type(const Params& params) {
  for (std::string type : types(params)) {
    if (type == "pref" || type == "internet" || type == "voice" || type == "x400" || type == "text" ||
        type.empty() || type.starts_with("x-")) {
      continue;
    }
    if (type == "mobile" || type == "iphone") type = "cell";
    return type;
  }
  return {};
}

std::string mime_from_type(std::string_view type) {
  const std::string lower = util::to_lower(type);
  if (lower.empty()) return {};
  if (lower.find('/') != std::string::npos) return lower;
  if (lower == "jpg") return "image/jpeg";
  return "image/" + lower;
}

std::string strip_whitespace(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (!util::is_ascii_space(c)) out.push_back(c);
  }
  return out;
}

}  // namespace

std::string normalize_date(std::string_view value) {
  value = util::trim(value);
  if (const std::size_t t = value.find('T'); t != std::string_view::npos && t >= 8) value = value.substr(0, t);
  const auto digits = [](std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char c) { return util::is_ascii_digit(c); });
  };
  if (value.size() == 8 && digits(value)) {
    return std::format("{}-{}-{}", value.substr(0, 4), value.substr(4, 2), value.substr(6, 2));
  }
  if (value.size() == 6 && value.starts_with("--") && digits(value.substr(2))) {
    return std::format("--{}-{}", value.substr(2, 2), value.substr(4, 2));
  }
  return std::string(value);
}

ir::Card card_from_props(const std::vector<VProp>& props) {
  ir::Card card;
  std::map<std::string, std::string> labels;  // group -> label
  for (const VProp& prop : props) {
    if (prop.name == "X-ABLABEL" && !prop.group.empty()) labels[util::to_lower(prop.group)] = clean_label(unescape(prop.raw));
  }
  const auto label_of = [&](const VProp& prop) {
    if (prop.group.empty()) return std::string();
    const auto it = labels.find(util::to_lower(prop.group));
    return it == labels.end() ? std::string() : it->second;
  };
  for (const VProp& prop : props) {
    const std::string& name = prop.name;
    if (name == "BEGIN" || name == "END" || name == "VERSION") continue;
    if (name == "X-ABLABEL" && !prop.group.empty()) continue;
    if (name == "FN") card.full_name = unescape(prop.raw);
    else if (name == "N") {
      std::vector<std::string> parts = split_unescape(prop.raw, ';');
      parts.resize(std::max<std::size_t>(parts.size(), 5));
      card.family_name = parts[0];
      card.given_name = parts[1];
      card.additional_name = parts[2];
      card.prefix = parts[3];
      card.suffix = parts[4];
    } else if (name == "NICKNAME") card.nickname = unescape(prop.raw);
    else if (name == "ORG") {
      std::vector<std::string> parts = split_unescape(prop.raw, ';');
      card.organization = parts[0];
      parts.erase(parts.begin());
      while (!parts.empty() && parts.back().empty()) parts.pop_back();
      card.department = util::join(parts, ";");
    } else if (name == "TITLE") card.title = unescape(prop.raw);
    else if (name == "ROLE") card.role = unescape(prop.raw);
    else if (name == "NOTE") card.note = util::replace_all(unescape(prop.raw), "\r\n", "\n");
    else if (name == "UID") card.uid = unescape(prop.raw);
    else if (name == "BDAY") card.birthday = normalize_date(unescape(prop.raw));
    else if (name == "ANNIVERSARY" || name == "X-ANNIVERSARY") card.anniversary = normalize_date(unescape(prop.raw));
    else if (name == "EMAIL") card.emails.push_back({std::string(util::trim(unescape(prop.raw))), primary_type(prop.params), label_of(prop)});
    else if (name == "TEL") {
      std::string number(util::trim(unescape(prop.raw)));
      if (util::istarts_with(number, "tel:")) number.erase(0, 4);
      card.phones.push_back({number, primary_type(prop.params), label_of(prop)});
    } else if (name == "URL") card.urls.push_back({std::string(util::trim(unescape(prop.raw))), primary_type(prop.params), label_of(prop)});
    else if (name == "ADR") {
      std::vector<std::string> parts = split_unescape(prop.raw, ';');
      parts.resize(std::max<std::size_t>(parts.size(), 7));
      ir::PostalAddress address;
      address.po_box = parts[0];
      address.extended = parts[1];
      address.street = parts[2];
      address.city = parts[3];
      address.region = parts[4];
      address.postal_code = parts[5];
      address.country = parts[6];
      address.type = primary_type(prop.params);
      address.label = label_of(prop);
      card.addresses.push_back(std::move(address));
    } else if (name == "CATEGORIES") {
      for (std::string& category : split_unescape(prop.raw, ',')) {
        if (!util::trim(category).empty()) card.categories.emplace_back(util::trim(category));
      }
    } else if (name == "PHOTO" && card.photo.empty() && card.photo_uri.empty()) {
      std::string encoding;
      std::string type;
      std::string media;
      for (const auto& [key, value] : prop.params) {
        if (util::iequals(key, "ENCODING")) encoding = value;
        if (util::iequals(key, "TYPE")) type = value;
        if (util::iequals(key, "MEDIATYPE")) media = value;
      }
      const std::string raw(util::trim(unescape(prop.raw)));
      if (util::iequals(encoding, "b") || util::iequals(encoding, "base64")) {
        if (auto bytes = util::base64_decode(strip_whitespace(raw))) {
          card.photo = std::move(*bytes);
          card.photo_mime = mime_from_type(media.empty() ? type : media);
          continue;
        }
      } else if (util::istarts_with(raw, "data:")) {
        const std::size_t comma = raw.find(',');
        const std::string_view header = std::string_view(raw).substr(5, comma == std::string::npos ? 0 : comma - 5);
        if (comma != std::string::npos && util::iends_with(header, ";base64")) {
          if (auto bytes = util::base64_decode(strip_whitespace(std::string_view(raw).substr(comma + 1)))) {
            card.photo = std::move(*bytes);
            card.photo_mime = util::to_lower(header.substr(0, header.size() - 7));
            continue;
          }
        }
      } else {
        card.photo_uri = raw;
        continue;
      }
      card.extra.push_back({name, prop.params, unescape(prop.raw)});
    } else {
      ir::Property extra;
      extra.name = prop.group.empty() ? name : util::to_upper(prop.group) + "." + name;
      extra.params = prop.params;
      extra.value = unescape(prop.raw);
      card.extra.push_back(std::move(extra));
    }
  }
  return card;
}

std::vector<WProp> props_from_card(const ir::Card& card, std::string_view version, bool drop_photos) {
  std::vector<WProp> out;
  const bool v4 = version == "4.0";
  int next_group = 1;
  const auto single = [](std::string value) { return std::vector<std::vector<std::string>>{{std::move(value)}}; };
  const auto typed = [](std::string_view type) {
    Params params;
    if (!type.empty()) params.emplace_back("TYPE", util::to_lower(type));
    return params;
  };
  const auto add_label = [&](WProp& prop, std::string_view label) {
    if (label.empty()) return;
    prop.group = std::format("item{}", next_group++);
    WProp ab;
    ab.group = prop.group;
    ab.name = "X-ABLABEL";
    ab.components = {{std::string(label)}};
    out.push_back(std::move(prop));
    out.push_back(std::move(ab));
  };

  std::string full_name = card.full_name;
  if (full_name.empty()) {
    std::vector<std::string> parts;
    for (const std::string* part : {&card.prefix, &card.given_name, &card.additional_name, &card.family_name, &card.suffix}) {
      if (!part->empty()) parts.push_back(*part);
    }
    full_name = util::join(parts, " ");
    if (full_name.empty()) full_name = card.organization;
    if (full_name.empty() && !card.emails.empty()) full_name = card.emails.front().value;
  }
  out.push_back({{}, "FN", {}, single(full_name)});
  out.push_back({{}, "N", {}, {{card.family_name}, {card.given_name}, {card.additional_name}, {card.prefix}, {card.suffix}}});
  if (!card.nickname.empty()) out.push_back({{}, "NICKNAME", {}, single(card.nickname)});
  if (!card.organization.empty() || !card.department.empty()) {
    WProp org{{}, "ORG", {}, {{card.organization}}};
    if (!card.department.empty()) {
      for (const std::string_view unit : util::split(card.department, ';')) org.components.push_back({std::string(unit)});
    }
    out.push_back(std::move(org));
  }
  if (!card.title.empty()) out.push_back({{}, "TITLE", {}, single(card.title)});
  if (!card.role.empty()) out.push_back({{}, "ROLE", {}, single(card.role)});
  for (const ir::LabeledValue& email : card.emails) {
    WProp prop{{}, "EMAIL", typed(email.type), single(email.value)};
    if (!email.label.empty()) add_label(prop, email.label);
    else out.push_back(std::move(prop));
  }
  for (const ir::LabeledValue& phone : card.phones) {
    WProp prop{{}, "TEL", typed(phone.type), single(phone.value)};
    if (!phone.label.empty()) add_label(prop, phone.label);
    else out.push_back(std::move(prop));
  }
  for (const ir::PostalAddress& address : card.addresses) {
    WProp prop{{},
               "ADR",
               typed(address.type),
               {{address.po_box},
                {address.extended},
                {address.street},
                {address.city},
                {address.region},
                {address.postal_code},
                {address.country}}};
    if (!address.label.empty()) add_label(prop, address.label);
    else out.push_back(std::move(prop));
  }
  for (const ir::LabeledValue& url : card.urls) {
    WProp prop{{}, "URL", typed(url.type), single(url.value)};
    prop.value_type = "uri";
    if (!url.label.empty()) add_label(prop, url.label);
    else out.push_back(std::move(prop));
  }
  const auto date_prop = [&](std::string name, const std::string& value) {
    WProp prop{{}, std::move(name), {}, single(value)};
    const bool full = value.size() == 10 && value[4] == '-' && value[7] == '-';
    const bool partial = value.size() == 7 && value.starts_with("--") && value[4] == '-';
    if (full || partial) {
      prop.value_type = "date";
      if (v4) prop.components = single(partial ? "--" + value.substr(2, 2) + value.substr(5, 2)
                                               : value.substr(0, 4) + value.substr(5, 2) + value.substr(8, 2));
    } else {
      prop.value_type = "text";
      if (v4) prop.params.emplace_back("VALUE", "text");
    }
    out.push_back(std::move(prop));
  };
  if (!card.birthday.empty()) date_prop("BDAY", card.birthday);
  if (!card.anniversary.empty()) date_prop(v4 ? "ANNIVERSARY" : "X-ANNIVERSARY", card.anniversary);
  if (!card.note.empty()) out.push_back({{}, "NOTE", {}, single(card.note)});
  if (!card.categories.empty()) out.push_back({{}, "CATEGORIES", {}, {card.categories}});
  if (!drop_photos && !card.photo.empty()) {
    WProp photo{{}, "PHOTO", {}, single(util::base64_encode(card.photo))};
    photo.binary = true;
    photo.value_type = "uri";
    photo.media_type = card.photo_mime.empty() ? std::string("image/jpeg") : card.photo_mime;
    out.push_back(std::move(photo));
  } else if (!drop_photos && !card.photo_uri.empty()) {
    WProp photo{{}, "PHOTO", {}, single(card.photo_uri)};
    photo.value_type = "uri";
    if (!v4) photo.params.emplace_back("VALUE", "uri");
    out.push_back(std::move(photo));
  }
  if (!card.uid.empty()) out.push_back({{}, "UID", {}, single(card.uid)});
  for (const ir::Property& property : card.extra) {
    WProp prop;
    std::string_view name = property.name;
    if (const std::size_t dot = name.find('.'); dot != std::string_view::npos) {
      prop.group = util::to_lower(name.substr(0, dot));
      name = name.substr(dot + 1);
    }
    prop.name = util::to_upper(name);
    prop.params = property.params;
    if (prop.name == "PHOTO" && drop_photos) continue;
    // Values were unescaped as a whole: keep ';' structure as written.
    for (const std::string_view component : util::split(property.value, ';')) prop.components.push_back({std::string(component)});
    if (prop.components.empty()) prop.components.push_back({std::string()});
    prop.value_type = "unknown";
    out.push_back(std::move(prop));
  }
  return out;
}

}  // namespace cl

namespace {

using cl::VProp;
using cl::WProp;

bool needs_qp(const WProp& prop) {
  for (const auto& component : prop.components) {
    for (const std::string& value : component) {
      for (const char c : value) {
        if (static_cast<unsigned char>(c) >= 0x80 || c == '\n' || c == '\r') return true;
      }
    }
  }
  return false;
}

std::string serialize_value(const WProp& prop, bool legacy) {
  std::vector<std::string> components;
  for (const auto& component : prop.components) {
    std::vector<std::string> values;
    for (const std::string& value : component) {
      // vCard 2.1 only needs ';' escaped, but escaping ',' too keeps readers
      // that apply 3.0 list rules from splitting text values.
      values.push_back(legacy ? util::replace_all(util::replace_all(util::replace_all(value, "\\", "\\\\"), ";", "\\;"),
                                                  ",", "\\,")
                              : cl::escape(value));
    }
    components.push_back(util::join(values, ","));
  }
  return util::join(components, ";");
}

std::string upper_subtype(std::string_view mime) {
  const std::size_t slash = mime.find('/');
  return util::to_upper(slash == std::string_view::npos ? mime : mime.substr(slash + 1));
}

void write_vcard_prop(std::string& out, const WProp& prop, std::string_view version) {
  const bool legacy = version == "2.1";
  const std::string name = prop.group.empty() ? prop.name : prop.group + "." + prop.name;
  if (prop.binary) {
    const std::string& data = prop.components[0][0];
    if (version == "4.0") {
      out += cl::fold(name + ":data:" + prop.media_type + ";base64," + data);
    } else if (legacy) {
      out += name + ";ENCODING=BASE64;TYPE=" + upper_subtype(prop.media_type) + ":\r\n";
      for (std::size_t i = 0; i < data.size(); i += 72) out += " " + data.substr(i, 72) + "\r\n";
      out += "\r\n";
    } else {
      out += cl::format_line(name, {{"ENCODING", "b"}, {"TYPE", upper_subtype(prop.media_type)}}, data);
    }
    return;
  }
  if (legacy) {
    std::string line = name;
    for (const auto& [key, value] : prop.params) {
      if (util::iequals(key, "TYPE")) {
        for (const std::string_view type : util::split(value, ',', false)) line += ";" + util::to_upper(type);
      } else {
        line += ";" + key + "=" + value;
      }
    }
    if (needs_qp(prop)) {
      line += ";CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:";
      out += line + cl::encode_qp(serialize_value(prop, true), line.size(), false) + "\r\n";
    } else {
      out += cl::fold(line + ":" + serialize_value(prop, true));
    }
    return;
  }
  cl::Params params = prop.params;
  if (version == "4.0" && prop.name == "TEL" &&
      std::ranges::none_of(params, [](const auto& p) { return util::iequals(p.first, "VALUE"); })) {
    params.emplace_back("VALUE", "text");
  }
  out += cl::format_line(name, params, serialize_value(prop, false));
}

Result<std::vector<std::vector<VProp>>> split_cards(std::string_view text, const Limits& limits) {
  CVT_TRY_ASSIGN(std::vector<cl::ContentLine> lines, cl::parse_lines(text, true, limits));
  std::vector<std::vector<VProp>> cards;
  int depth = 0;
  for (cl::ContentLine& line : lines) {
    if (line.name == "BEGIN" && util::iequals(util::trim(line.value), "VCARD")) {
      if (depth++ == 0) {
        cards.emplace_back();
        CVT_TRY(limits.check_archive_entries(cards.size()));
      }
      continue;
    }
    if (line.name == "END" && util::iequals(util::trim(line.value), "VCARD")) {
      if (depth > 0) --depth;
      continue;
    }
    if (depth != 1) continue;  // outside any card, or inside a nested AGENT card
    cards.back().push_back({std::move(line.group), std::move(line.name), std::move(line.params), std::move(line.value)});
  }
  return cards;
}

Result<AnyIR> read_vcf(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, decode_text(ctx.input, ctx.options.get_string("source_encoding", "auto")));
  CVT_TRY_ASSIGN(const auto cards, split_cards(text, ctx.limits));
  if (cards.empty()) return fail(Status::Decode, "no BEGIN:VCARD found; this is not a vCard file");
  ir::Contacts contacts;
  for (std::size_t i = 0; i < cards.size(); ++i) {
    if ((i & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(cards.size())));
    contacts.cards.push_back(cl::card_from_props(cards[i]));
  }
  return contacts;
}

bool is_metadata_property(std::string_view name) {
  return name == "PRODID" || name == "REV" || name == "X-ABUID" || name.ends_with(".PRODID");
}

Result<Bytes> write_vcf(const AnyIR& value, WriteContext& ctx) {
  const ir::Contacts& contacts = value.get<ir::Contacts>();
  const std::string version = ctx.options.get_enum("vcard_version", "3.0", {"2.1", "3.0", "4.0"});
  const bool drop_photos = ctx.options.get_bool("drop_photos", false);
  const bool strip = options::resolved_strip_metadata(ctx.options);
  std::string out;
  for (std::size_t i = 0; i < contacts.cards.size(); ++i) {
    if ((i & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(contacts.cards.size())));
    out += "BEGIN:VCARD\r\nVERSION:" + version + "\r\n";
    for (const WProp& prop : cl::props_from_card(contacts.cards[i], version, drop_photos)) {
      if (strip && is_metadata_property(prop.name)) continue;
      write_vcard_prop(out, prop, version);
    }
    out += "END:VCARD\r\n";
  }
  return to_bytes(out);
}

int sniff_vcf(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_text(data, 1024);
  return util::istarts_with(util::trim_left(head), "BEGIN:VCARD") ? 92 : 0;
}

}  // namespace

std::vector<OptionSpec> contacts_write_options_with_photos() {
  return {OptionSpec::boolean("drop_photos", "Drop photos", false, "Leave contact pictures out of the output."),
          options::strip_metadata()};
}

void register_contacts(Registry& registry) {
  FormatInfo info;
  info.id = "vcf";
  info.name = "vCard";
  info.description = "Address-book cards (vCard 2.1, 3.0 and 4.0).";
  info.category = Category::Pim;
  info.extensions = {"vcf", "vcard"};
  info.mime = "text/vcard";
  info.popularity = 80;
  info.sniff = sniff_vcf;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "vcf";
  codec.kind = Kind::Contacts;
  codec.read = read_vcf;
  codec.write = write_vcf;
  codec.read_options = {source_encoding_option()};
  codec.write_options = {
      OptionSpec::choice("vcard_version", "vCard version", "3.0", {"2.1", "3.0", "4.0"},
                         "Version of the written cards: 3.0 is the most widely understood, 4.0 the newest, "
                         "2.1 for old phones."),
  };
  for (OptionSpec& spec : contacts_write_options_with_photos()) codec.write_options.push_back(std::move(spec));
  registry.add_codec(std::move(codec));

  register_contacts_formats(registry);
}

}  // namespace cvt::pim
