// Convertible core - pim family: news feeds.
//
//   rss        RSS 0.91 / 0.92 / 2.0 and RSS 1.0 (RDF), read; RSS 2.0 written
//   atom       Atom 1.0 (RFC 4287)
//   json-feed  JSON Feed 1.0 / 1.1
//
// All three read into one Data model and write from it:
//   {"type": "feed", "format": "rss|atom|json-feed", "title", "link", "description",
//    "language", "updated", "author", "id",
//    "items": [{"id", "title", "link", "published", "updated", "author", "summary",
//               "content", "categories": [...], "enclosures": [{"url", "type", "length"}]}]}
// Dates are normalised to RFC 3339 (RFC 822 dates of RSS are converted).
// Readers for Document (the feed as headings and paragraphs, HTML reduced
// to text) and Table (one row per item) are registered as well.
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
#include "families/pim/pim.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/table.hpp"
#include "util/json.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::pim {

namespace {

using namespace std::string_view_literals;
using util::XmlNode;

constexpr std::string_view atom_ns = "http://www.w3.org/2005/Atom";

std::string text(XmlNode node) { return std::string(util::trim(node.text())); }

/// A date in any feed notation as RFC 3339; the input when unparseable.
std::string normalize_date(std::string_view value) {
  value = util::trim(value);
  if (value.empty()) return {};
  std::optional<ParsedDateTime> parsed = parse_iso_datetime(value);
  if (!parsed) parsed = parse_rfc822_datetime(value);
  if (!parsed) return std::string(value);
  if (parsed->date_only) return format_iso_date(parsed->local_seconds / 86400);
  return format_iso_datetime(parsed->utc_seconds(), parsed->offset_minutes ? std::optional<int>(*parsed->offset_minutes) : std::nullopt);
}

void set(Json& object, std::string_view key, const std::string& value) {
  if (!value.empty()) object[key] = value;
}

XmlNode child_ns(XmlNode node, std::string_view local, std::string_view ns_prefix_hint = {}) {
  for (const XmlNode child : node.elements()) {
    if (child.local_name() == local && (ns_prefix_hint.empty() || child.namespace_uri().find(ns_prefix_hint) != std::string_view::npos)) {
      return child;
    }
  }
  return {};
}

// ---------------------------------------------------------------------------- reading

Json rss_item(XmlNode item) {
  Json out = Json::object();
  set(out, "id", text(item.child_local("guid")));
  set(out, "title", text(item.child_local("title")));
  set(out, "link", text(item.child_local("link")));
  if (!out.contains("link") && item.attribute_local("about")) set(out, "link", std::string(item.attribute_local("about").value()));
  std::string published = text(item.child_local("pubDate"));
  if (published.empty()) published = text(child_ns(item, "date", "purl.org/dc"));
  set(out, "published", normalize_date(published));
  std::string author = text(child_ns(item, "creator", "purl.org/dc"));
  if (author.empty()) author = text(item.child_local("author"));
  set(out, "author", author);
  set(out, "summary", text(item.child_local("description")));
  set(out, "content", text(child_ns(item, "encoded", "purl.org/rss/1.0/modules/content")));
  Json categories = Json::array();
  for (const XmlNode category : item.children_local("category")) categories.push_back(text(category));
  for (const XmlNode subject : item.children_local("subject")) categories.push_back(text(subject));
  if (!categories.empty()) out["categories"] = std::move(categories);
  Json enclosures = Json::array();
  for (const XmlNode enclosure : item.children_local("enclosure")) {
    Json e = Json::object({{"url", std::string(enclosure.attribute_local("url").value())}});
    set(e, "type", std::string(enclosure.attribute_local("type").value()));
    if (const auto length = util::parse_int(enclosure.attribute_local("length").value())) e["length"] = *length;
    enclosures.push_back(std::move(e));
  }
  if (!enclosures.empty()) out["enclosures"] = std::move(enclosures);
  return out;
}

Result<Json> read_rss(XmlNode root, const Limits& limits) {
  Json feed = Json::object({{"type", "feed"}, {"format", "rss"}});
  const XmlNode channel = root.child_local("channel");
  if (!channel) return fail(Status::Decode, "the RSS feed has no <channel>");
  set(feed, "title", text(channel.child_local("title")));
  set(feed, "link", text(channel.child_local("link")));
  set(feed, "description", text(channel.child_local("description")));
  std::string language = text(channel.child_local("language"));
  if (language.empty()) language = text(child_ns(channel, "language", "purl.org/dc"));
  set(feed, "language", language);
  std::string updated = text(channel.child_local("lastBuildDate"));
  if (updated.empty()) updated = text(channel.child_local("pubDate"));
  if (updated.empty()) updated = text(child_ns(channel, "date", "purl.org/dc"));
  set(feed, "updated", normalize_date(updated));
  std::string author = text(channel.child_local("managingEditor"));
  if (author.empty()) author = text(child_ns(channel, "creator", "purl.org/dc"));
  set(feed, "author", author);
  Json items = Json::array();
  // RSS 2.0 puts items in the channel; RSS 1.0 (RDF) next to it.
  for (const XmlNode parent : {channel, root}) {
    for (const XmlNode item : parent.children_local("item")) {
      if (items.size() >= limits.max_archive_entries) return fail(Status::Limit, "too many feed items");
      items.push_back(rss_item(item));
    }
  }
  feed["items"] = std::move(items);
  return feed;
}

std::string atom_link(XmlNode node) {
  std::string alternate;
  for (const XmlNode link : node.children_local("link")) {
    const std::string_view rel = link.attribute_local("rel").value();
    if (rel.empty() || rel == "alternate") return std::string(link.attribute_local("href").value());
    if (alternate.empty() && rel != "self" && rel != "enclosure") alternate = link.attribute_local("href").value();
  }
  return alternate;
}

Json atom_entry(XmlNode entry) {
  Json out = Json::object();
  set(out, "id", text(entry.child_local("id")));
  set(out, "title", text(entry.child_local("title")));
  set(out, "link", atom_link(entry));
  set(out, "published", normalize_date(text(entry.child_local("published"))));
  set(out, "updated", normalize_date(text(entry.child_local("updated"))));
  set(out, "author", text(entry.child_local("author").child_local("name")));
  set(out, "summary", text(entry.child_local("summary")));
  set(out, "content", text(entry.child_local("content")));
  Json categories = Json::array();
  for (const XmlNode category : entry.children_local("category")) {
    std::string term(category.attribute_local("term").value());
    if (term.empty()) term = text(category);
    categories.push_back(std::move(term));
  }
  if (!categories.empty()) out["categories"] = std::move(categories);
  Json enclosures = Json::array();
  for (const XmlNode link : entry.children_local("link")) {
    if (link.attribute_local("rel").value() != "enclosure") continue;
    Json e = Json::object({{"url", std::string(link.attribute_local("href").value())}});
    set(e, "type", std::string(link.attribute_local("type").value()));
    if (const auto length = util::parse_int(link.attribute_local("length").value())) e["length"] = *length;
    enclosures.push_back(std::move(e));
  }
  if (!enclosures.empty()) out["enclosures"] = std::move(enclosures);
  return out;
}

Result<Json> read_atom(XmlNode root, const Limits& limits) {
  Json feed = Json::object({{"type", "feed"}, {"format", "atom"}});
  set(feed, "id", text(root.child_local("id")));
  set(feed, "title", text(root.child_local("title")));
  set(feed, "link", atom_link(root));
  set(feed, "description", text(root.child_local("subtitle")));
  set(feed, "language", std::string(root.attribute_local("lang").value()));
  set(feed, "updated", normalize_date(text(root.child_local("updated"))));
  set(feed, "author", text(root.child_local("author").child_local("name")));
  Json items = Json::array();
  for (const XmlNode entry : root.children_local("entry")) {
    if (items.size() >= limits.max_archive_entries) return fail(Status::Limit, "too many feed items");
    items.push_back(atom_entry(entry));
  }
  feed["items"] = std::move(items);
  return feed;
}

Result<Json> read_json_feed(std::string_view text_value, const Limits& limits) {
  CVT_TRY_ASSIGN(const Json root, util::parse_json(text_value, 64));
  if (!root["version"].as_string().starts_with("https://jsonfeed.org/version/")) {
    return fail(Status::Decode, "not a JSON Feed (no jsonfeed.org version)");
  }
  Json feed = Json::object({{"type", "feed"}, {"format", "json-feed"}});
  set(feed, "title", root["title"].as_string());
  set(feed, "link", root["home_page_url"].as_string());
  set(feed, "description", root["description"].as_string());
  set(feed, "language", root["language"].as_string());
  const auto author_of = [](const Json& object) {
    std::string name = object["author"]["name"].as_string();
    if (name.empty()) name = object["authors"][0]["name"].as_string();
    return name;
  };
  set(feed, "author", author_of(root));
  Json items = Json::array();
  for (const Json& item : root["items"].elements()) {
    if (items.size() >= limits.max_archive_entries) return fail(Status::Limit, "too many feed items");
    Json out = Json::object();
    set(out, "id", item["id"].is_string() ? item["id"].as_string() : item["id"].is_number() ? util::dump_json(item["id"]) : std::string());
    set(out, "title", item["title"].as_string());
    set(out, "link", item["url"].as_string());
    set(out, "published", normalize_date(item["date_published"].as_string()));
    set(out, "updated", normalize_date(item["date_modified"].as_string()));
    set(out, "author", author_of(item));
    set(out, "summary", item["summary"].as_string());
    std::string content = item["content_html"].as_string();
    if (content.empty()) content = item["content_text"].as_string();
    set(out, "content", content);
    Json categories = Json::array();
    for (const Json& tag : item["tags"].elements()) categories.push_back(tag.as_string());
    if (!categories.empty()) out["categories"] = std::move(categories);
    Json enclosures = Json::array();
    for (const Json& attachment : item["attachments"].elements()) {
      Json e = Json::object({{"url", attachment["url"].as_string()}});
      set(e, "type", attachment["mime_type"].as_string());
      if (const auto length = attachment["size_in_bytes"].get_int64()) e["length"] = *length;
      enclosures.push_back(std::move(e));
    }
    if (!enclosures.empty()) out["enclosures"] = std::move(enclosures);
    items.push_back(std::move(out));
  }
  feed["items"] = std::move(items);
  return feed;
}

Result<Json> read_feed(std::string_view format, const ReadContext& ctx) {
  if (format == "json-feed") {
    CVT_TRY_ASSIGN(const std::string decoded, decode_text(ctx.input, "auto"));
    return read_json_feed(decoded, ctx.limits);
  }
  util::XmlParseOptions options;
  options.replace_invalid_text = true;
  options.keep_unknown_entities = true;
  options.max_depth = 128;
  auto doc = util::parse_xml(ctx.input, options);
  if (!doc) return fail(doc.error().code, "the feed is not well-formed XML: {}", doc.error().message);
  const XmlNode root = doc->root();
  if (root.local_name() == "feed") return read_atom(root, ctx.limits);
  if (root.local_name() == "rss" || root.local_name() == "RDF") return read_rss(root, ctx.limits);
  return fail(Status::Decode, "not a news feed (root element <{}>)", root.local_name());
}

// ---------------------------------------------------------------------------- writing

std::string rfc822(const std::string& iso) {
  const auto parsed = parse_iso_datetime(iso);
  if (!parsed) return iso;
  return format_rfc822_datetime(parsed->utc_seconds(), parsed->offset_minutes.value_or(0));
}

std::string write_rss(const Json& feed) {
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  XmlNode rss = doc.append_child("rss");
  rss.set_attribute("version", "2.0");
  rss.set_attribute("xmlns:content", "http://purl.org/rss/1.0/modules/content/");
  rss.set_attribute("xmlns:dc", "http://purl.org/dc/elements/1.1/");
  XmlNode channel = rss.append_child("channel");
  channel.append_child("title").append_text(feed["title"].as_string());
  channel.append_child("link").append_text(feed["link"].as_string());
  channel.append_child("description").append_text(feed["description"].as_string());
  if (!feed["language"].as_string().empty()) channel.append_child("language").append_text(feed["language"].as_string());
  if (!feed["updated"].as_string().empty()) channel.append_child("lastBuildDate").append_text(rfc822(feed["updated"].as_string()));
  if (!feed["author"].as_string().empty()) channel.append_child("dc:creator").append_text(feed["author"].as_string());
  for (const Json& item : feed["items"].elements()) {
    XmlNode node = channel.append_child("item");
    const auto add = [&](std::string_view name, const std::string& value) {
      if (!value.empty()) node.append_child(name).append_text(value);
    };
    add("title", item["title"].as_string());
    add("link", item["link"].as_string());
    if (!item["id"].as_string().empty()) {
      XmlNode guid = node.append_child("guid");
      guid.append_text(item["id"].as_string());
      if (item["id"].as_string() != item["link"].as_string()) guid.set_attribute("isPermaLink", "false");
    }
    std::string published = item["published"].as_string();
    if (published.empty()) published = item["updated"].as_string();
    if (!published.empty()) add("pubDate", rfc822(published));
    add("dc:creator", item["author"].as_string());
    add("description", item["summary"].as_string());
    add("content:encoded", item["content"].as_string());
    for (const Json& category : item["categories"].elements()) add("category", category.as_string());
    for (const Json& enclosure : item["enclosures"].elements()) {
      XmlNode e = node.append_child("enclosure");
      e.set_attribute("url", enclosure["url"].as_string());
      e.set_attribute("length", std::to_string(enclosure["length"].as_int64(0)));
      e.set_attribute("type", enclosure["type"].as_string().empty() ? std::string("application/octet-stream") : enclosure["type"].as_string());
    }
  }
  return util::xml_to_string(doc);
}

std::string write_atom(const Json& feed) {
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  XmlNode root = doc.append_child("feed");
  root.set_attribute("xmlns", atom_ns);
  if (!feed["language"].as_string().empty()) root.set_attribute("xml:lang", feed["language"].as_string());
  // Atom requires id, title and updated; derive stable values when missing.
  std::string id = feed["id"].as_string();
  if (id.empty()) id = feed["link"].as_string();
  if (id.empty()) id = "urn:feed:" + feed["title"].as_string();
  root.append_child("id").append_text(id);
  root.append_child("title").append_text(feed["title"].as_string());
  if (!feed["description"].as_string().empty()) root.append_child("subtitle").append_text(feed["description"].as_string());
  std::string updated = feed["updated"].as_string();
  if (updated.empty() && !feed["items"].empty()) {
    for (const Json& item : feed["items"].elements()) {
      const std::string& candidate = item["updated"].as_string().empty() ? item["published"].as_string() : item["updated"].as_string();
      if (candidate > updated) updated = candidate;
    }
  }
  root.append_child("updated").append_text(updated.empty() ? std::string("1970-01-01T00:00:00Z") : updated);
  if (!feed["link"].as_string().empty()) {
    XmlNode link = root.append_child("link");
    link.set_attribute("rel", "alternate");
    link.set_attribute("href", feed["link"].as_string());
  }
  if (!feed["author"].as_string().empty()) root.append_child("author").append_child("name").append_text(feed["author"].as_string());
  std::size_t n = 0;
  for (const Json& item : feed["items"].elements()) {
    ++n;
    XmlNode entry = root.append_child("entry");
    std::string entry_id = item["id"].as_string();
    if (entry_id.empty()) entry_id = item["link"].as_string();
    if (entry_id.empty()) entry_id = std::format("{}#{}", id, n);
    entry.append_child("id").append_text(entry_id);
    entry.append_child("title").append_text(item["title"].as_string());
    if (!item["link"].as_string().empty()) {
      XmlNode link = entry.append_child("link");
      link.set_attribute("rel", "alternate");
      link.set_attribute("href", item["link"].as_string());
    }
    if (!item["published"].as_string().empty()) entry.append_child("published").append_text(item["published"].as_string());
    std::string entry_updated = item["updated"].as_string();
    if (entry_updated.empty()) entry_updated = item["published"].as_string();
    entry.append_child("updated").append_text(entry_updated.empty() ? std::string("1970-01-01T00:00:00Z") : entry_updated);
    if (!item["author"].as_string().empty()) entry.append_child("author").append_child("name").append_text(item["author"].as_string());
    if (!item["summary"].as_string().empty()) {
      XmlNode summary = entry.append_child("summary");
      summary.set_attribute("type", "html");
      summary.append_text(item["summary"].as_string());
    }
    if (!item["content"].as_string().empty()) {
      XmlNode content = entry.append_child("content");
      content.set_attribute("type", "html");
      content.append_text(item["content"].as_string());
    }
    for (const Json& category : item["categories"].elements()) entry.append_child("category").set_attribute("term", category.as_string());
    for (const Json& enclosure : item["enclosures"].elements()) {
      XmlNode link = entry.append_child("link");
      link.set_attribute("rel", "enclosure");
      link.set_attribute("href", enclosure["url"].as_string());
      if (!enclosure["type"].as_string().empty()) link.set_attribute("type", enclosure["type"].as_string());
      if (const auto length = enclosure["length"].get_int64()) link.set_attribute("length", std::to_string(*length));
    }
  }
  return util::xml_to_string(doc);
}

std::string write_json_feed(const Json& feed) {
  Json out = Json::object({{"version", "https://jsonfeed.org/version/1.1"}, {"title", feed["title"].as_string()}});
  set(out, "home_page_url", feed["link"].as_string());
  set(out, "description", feed["description"].as_string());
  set(out, "language", feed["language"].as_string());
  if (!feed["author"].as_string().empty()) out["authors"] = Json::array({Json::object({{"name", feed["author"].as_string()}})});
  Json items = Json::array();
  std::size_t n = 0;
  for (const Json& item : feed["items"].elements()) {
    ++n;
    std::string id = item["id"].as_string();
    if (id.empty()) id = item["link"].as_string();
    if (id.empty()) id = std::to_string(n);
    Json entry = Json::object({{"id", id}});
    set(entry, "url", item["link"].as_string());
    set(entry, "title", item["title"].as_string());
    set(entry, "content_html", item["content"].as_string());
    set(entry, "summary", item["summary"].as_string());
    if (!entry.contains("content_html")) entry["content_text"] = item["summary"].as_string();
    set(entry, "date_published", item["published"].as_string());
    set(entry, "date_modified", item["updated"].as_string());
    if (!item["author"].as_string().empty()) entry["authors"] = Json::array({Json::object({{"name", item["author"].as_string()}})});
    if (!item["categories"].empty()) entry["tags"] = item["categories"];
    if (!item["enclosures"].empty()) {
      Json attachments = Json::array();
      for (const Json& enclosure : item["enclosures"].elements()) {
        Json a = Json::object({{"url", enclosure["url"].as_string()},
                               {"mime_type", enclosure["type"].as_string().empty() ? std::string("application/octet-stream")
                                                                                    : enclosure["type"].as_string()}});
        if (const auto length = enclosure["length"].get_int64()) a["size_in_bytes"] = *length;
        attachments.push_back(std::move(a));
      }
      entry["attachments"] = std::move(attachments);
    }
    items.push_back(std::move(entry));
  }
  out["items"] = std::move(items);
  return util::dump_json(out, 2) + "\n";
}

// ---------------------------------------------------------------------------- Document / Table

/// HTML fragment to plain text: tags dropped, block tags become line breaks, entities decoded.
std::string html_text(std::string_view html) {
  std::string out;
  std::size_t i = 0;
  while (i < html.size()) {
    if (html[i] == '<') {
      const std::size_t close = html.find('>', i);
      if (close == std::string_view::npos) break;
      const std::string tag = util::to_lower(html.substr(i + 1, std::min<std::size_t>(close - i - 1, 4)));
      if (tag.starts_with("br") || tag.starts_with("p") || tag.starts_with("/p") || tag.starts_with("li") || tag.starts_with("div")) {
        out.push_back('\n');
      }
      i = close + 1;
      continue;
    }
    if (html[i] == '&') {
      const std::size_t semicolon = html.find(';', i);
      if (semicolon != std::string_view::npos && semicolon - i <= 8) {
        const std::string_view name = html.substr(i + 1, semicolon - i - 1);
        std::optional<std::int64_t> code;
        if (name == "amp") code = '&';
        else if (name == "lt") code = '<';
        else if (name == "gt") code = '>';
        else if (name == "quot") code = '"';
        else if (name == "apos") code = '\'';
        else if (name == "nbsp") code = ' ';
        else if (name.starts_with("#x")) code = util::parse_int(name.substr(2), 16);
        else if (name.starts_with('#')) code = util::parse_int(name.substr(1));
        if (code && *code > 0 && *code <= 0x10FFFF) {
          util::append_utf8(out, static_cast<char32_t>(*code));
          i = semicolon + 1;
          continue;
        }
      }
    }
    out.push_back(html[i++]);
  }
  // Collapse white space within lines and drop empty lines.
  std::vector<std::string> lines;
  for (const std::string_view line : util::split(out, '\n')) {
    std::string collapsed;
    for (const char c : line) {
      if (util::is_ascii_space(c)) {
        if (!collapsed.empty() && collapsed.back() != ' ') collapsed.push_back(' ');
      } else {
        collapsed.push_back(c);
      }
    }
    const std::string_view trimmed = util::trim(collapsed);
    if (!trimmed.empty()) lines.emplace_back(trimmed);
  }
  return util::join(lines, "\n");
}

ir::Document feed_document(const Json& feed) {
  using namespace ir::build;
  ir::Document document;
  document.metadata.title = feed["title"].as_string();
  document.metadata.language = feed["language"].as_string();
  document.metadata.author = feed["author"].as_string();
  document.metadata.date = feed["updated"].as_string();
  document.blocks.push_back(feed["link"].as_string().empty() ? heading(1, feed["title"].as_string())
                                                             : heading(1, {hyperlink(feed["link"].as_string(), feed["title"].as_string())}));
  if (!feed["description"].as_string().empty()) document.blocks.push_back(paragraph(html_text(feed["description"].as_string())));
  for (const Json& item : feed["items"].elements()) {
    const std::string& title = item["title"].as_string().empty() ? item["link"].as_string() : item["title"].as_string();
    document.blocks.push_back(item["link"].as_string().empty() ? heading(2, title) : heading(2, {hyperlink(item["link"].as_string(), title)}));
    std::vector<std::string> meta;
    if (!item["published"].as_string().empty()) meta.push_back(item["published"].as_string());
    if (!item["author"].as_string().empty()) meta.push_back(item["author"].as_string());
    if (!meta.empty()) document.blocks.push_back(paragraph({emphasis(util::join(meta, " \xC2\xB7 "))}));
    const std::string body = html_text(item["content"].as_string().empty() ? item["summary"].as_string() : item["content"].as_string());
    for (const std::string_view para : util::split(body, '\n', false)) document.blocks.push_back(paragraph(std::string(para)));
  }
  return document;
}

ir::Table feed_table(const Json& feed) {
  std::vector<std::vector<std::string>> rows;
  for (const Json& item : feed["items"].elements()) {
    std::vector<std::string> categories;
    for (const Json& category : item["categories"].elements()) categories.push_back(category.as_string());
    rows.push_back({item["title"].as_string(), item["link"].as_string(), item["published"].as_string(),
                    item["updated"].as_string(), item["author"].as_string(), html_text(item["summary"].as_string()),
                    util::join(categories, ", "), item["id"].as_string()});
  }
  ir::Table table = string_table(feed["title"].as_string().empty() ? std::string("Feed") : feed["title"].as_string(),
                                 {"title", "link", "published", "updated", "author", "summary", "categories", "id"}, rows);
  // Dates as DateTime cells where they parse.
  for (std::size_t r = 1; r < table.sheets[0].rows.size(); ++r) {
    for (const std::size_t c : {std::size_t{2}, std::size_t{3}}) {
      ir::Cell& cell = table.sheets[0].rows[r][c];
      if (const auto parsed = parse_iso_datetime(cell.text); parsed && !parsed->date_only) {
        cell = ir::Cell::datetime(static_cast<double>(parsed->utc_seconds()), cell.text);
      }
    }
  }
  return table;
}

// ---------------------------------------------------------------------------- registration

int sniff_feed(ByteView data, std::string_view target) {
  const std::string head = sniff_text(data, 2048);
  if (target == "json-feed") {
    return util::trim(head).starts_with('{') && head.find("jsonfeed.org/version") != std::string::npos ? 95 : 0;
  }
  if (!util::trim(head).starts_with('<')) return 0;
  if (target == "atom") return head.find("<feed") != std::string::npos && head.find("http://www.w3.org/2005/Atom") != std::string::npos ? 94 : 0;
  if (head.find("<rss") != std::string::npos) return 94;
  return head.find("<rdf:RDF") != std::string::npos && head.find("http://purl.org/rss/1.0/") != std::string::npos ? 94 : 0;
}

}  // namespace

void register_feeds(Registry& registry) {
  struct Format {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> extensions;
    std::string mime;
    int popularity;
  };
  const Format formats[] = {
      {"rss", "RSS feed", "Really Simple Syndication news feed (RSS 0.9x, 1.0 and 2.0).", {"rss", "xml"},
       "application/rss+xml", 45},
      {"atom", "Atom feed", "Atom syndication feed (RFC 4287).", {"atom", "xml"}, "application/atom+xml", 35},
      {"json-feed", "JSON Feed", "JSON Feed syndication format (jsonfeed.org).", {"json"}, "application/feed+json", 15},
  };
  for (const Format& format : formats) {
    FormatInfo info;
    info.id = format.id;
    info.name = format.name;
    info.description = format.description;
    info.category = Category::Data;
    info.extensions = format.extensions;
    info.mime = format.mime;
    info.popularity = format.popularity;
    info.sniff = [id = format.id](ByteView data, std::string_view) { return sniff_feed(data, id); };
    registry.add_format(std::move(info));

    const std::string id = format.id;
    Codec data;
    data.format_id = id;
    data.kind = Kind::Data;
    data.read = [id](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(Json feed, read_feed(id, ctx));
      return ir::Data{std::move(feed)};
    };
    data.write = [id](const AnyIR& value, WriteContext&) -> Result<Bytes> {
      const Json& feed = value.get<ir::Data>().value;
      if (!feed.is_object()) return fail(Status::Encode, "a feed is written from an object with \"title\" and \"items\"");
      if (id == "atom") return to_bytes(write_atom(feed));
      if (id == "json-feed") return to_bytes(write_json_feed(feed));
      return to_bytes(write_rss(feed));
    };
    registry.add_codec(std::move(data));

    Codec document;
    document.format_id = id;
    document.kind = Kind::Document;
    document.lossy = true;
    document.read_cost = 15;
    document.read = [id](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const Json feed, read_feed(id, ctx));
      return feed_document(feed);
    };
    registry.add_codec(std::move(document));

    Codec table;
    table.format_id = id;
    table.kind = Kind::Table;
    table.lossy = true;
    table.read_cost = 15;
    table.read = [id](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const Json feed, read_feed(id, ctx));
      return feed_table(feed);
    };
    registry.add_codec(std::move(table));
  }
}

}  // namespace cvt::pim
