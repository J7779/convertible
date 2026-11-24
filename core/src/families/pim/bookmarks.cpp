// Convertible core - pim family: bookmarks and link files.
//
//   netscape-bookmarks  the Netscape bookmark file every browser imports and
//                       exports (HTML with <DL>/<DT>/<H3>/<A>), read and written
//   browser-bookmarks   Chrome / Edge / Brave "Bookmarks" JSON and Firefox JSON
//                       backups (read)
//   url-shortcut        Windows Internet Shortcut (.url)
//   webloc              macOS web location (.webloc, XML or binary property list)
//   desktop-link        freedesktop.org link (.desktop with Type=Link)
//
// Bookmark collections read into Data as a nested outline,
//   {"type": "bookmarks", "title": "...", "children": [
//      {"type": "folder", "title", "added", "modified", "children": [...]},
//      {"type": "bookmark", "title", "url", "added", "modified", "tags": [...], "description"}]}
// with ISO 8601 times, into a Document (nested lists of links) and into a
// Table (folder path, title, url, added, tags, description). Link files
// read into Data as {"type": "link", "url", "title"}.
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
#include "util/base64.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::pim {

namespace {

using namespace std::string_view_literals;

constexpr std::size_t max_depth = 64;

struct Node {
  bool folder = false;
  std::string title;
  std::string url;
  std::string added;     ///< ISO 8601
  std::string modified;  ///< ISO 8601
  std::vector<std::string> tags;
  std::string description;
  std::string icon;
  std::vector<Node> children;
};

std::string iso_from_unix(std::int64_t seconds) {
  if (seconds <= 0 || seconds > 32503680000LL) return {};
  return format_iso_datetime(seconds);
}

std::int64_t unix_from_iso(const std::string& iso) {
  if (const auto parsed = parse_iso_datetime(iso)) return parsed->utc_seconds();
  return 0;
}

// ---------------------------------------------------------------------------- Data / Document / Table

Json node_json(const Node& node, std::size_t depth) {
  Json out = Json::object({{"type", node.folder ? "folder" : "bookmark"}, {"title", node.title}});
  if (!node.folder) out["url"] = node.url;
  if (!node.added.empty()) out["added"] = node.added;
  if (!node.modified.empty()) out["modified"] = node.modified;
  if (!node.tags.empty()) out["tags"] = Json(node.tags);
  if (!node.description.empty()) out["description"] = node.description;
  if (!node.icon.empty()) out["icon"] = node.icon;
  if (node.folder) {
    Json children = Json::array();
    if (depth < max_depth) {
      for (const Node& child : node.children) children.push_back(node_json(child, depth + 1));
    }
    out["children"] = std::move(children);
  }
  return out;
}

Json tree_json(const Node& root) {
  Json out = Json::object({{"type", "bookmarks"}, {"title", root.title}});
  Json children = Json::array();
  for (const Node& child : root.children) children.push_back(node_json(child, 1));
  out["children"] = std::move(children);
  return out;
}

Node json_node(const Json& value, std::size_t depth) {
  Node node;
  const std::string& type = value["type"].as_string();
  node.folder = type == "folder" || (value.contains("children") && !value.contains("url"));
  node.title = value["title"].as_string();
  if (node.title.empty()) node.title = value["name"].as_string();
  node.url = value["url"].as_string();
  node.added = value["added"].as_string();
  node.modified = value["modified"].as_string();
  node.description = value["description"].as_string();
  node.icon = value["icon"].as_string();
  for (const Json& tag : value["tags"].elements()) node.tags.push_back(tag.as_string());
  if (depth < max_depth) {
    for (const Json& child : value["children"].elements()) node.children.push_back(json_node(child, depth + 1));
  }
  return node;
}

void document_items(const std::vector<Node>& nodes, std::vector<ir::Blocks>& items, std::size_t depth) {
  for (const Node& node : nodes) {
    ir::Blocks item;
    if (node.folder) {
      item.push_back(ir::build::paragraph({ir::build::strong(node.title)}));
      std::vector<ir::Blocks> children;
      if (depth < 32) document_items(node.children, children, depth + 1);
      if (!children.empty()) item.push_back(ir::List{false, 1, std::move(children)});
    } else {
      ir::Inlines content{ir::build::hyperlink(node.url, node.title.empty() ? node.url : node.title)};
      if (!node.description.empty()) content.push_back(ir::Text{" \xE2\x80\x94 " + node.description});
      item.push_back(ir::Paragraph{std::move(content)});
    }
    items.push_back(std::move(item));
  }
}

ir::Document tree_document(const Node& root) {
  ir::Document document;
  document.metadata.title = root.title;
  if (!root.title.empty()) document.blocks.push_back(ir::build::heading(1, root.title));
  std::vector<ir::Blocks> items;
  document_items(root.children, items, 0);
  if (!items.empty()) document.blocks.push_back(ir::List{false, 1, std::move(items)});
  return document;
}

void table_rows(const std::vector<Node>& nodes, const std::string& path, std::vector<std::vector<std::string>>& rows,
                std::size_t depth) {
  for (const Node& node : nodes) {
    if (node.folder) {
      if (depth < max_depth) table_rows(node.children, path.empty() ? node.title : path + "/" + node.title, rows, depth + 1);
      continue;
    }
    rows.push_back({path, node.title, node.url, node.added, util::join(node.tags, ","), node.description});
  }
}

ir::Table tree_table(const Node& root) {
  std::vector<std::vector<std::string>> rows;
  table_rows(root.children, {}, rows, 0);
  return string_table("Bookmarks", {"folder", "title", "url", "added", "tags", "description"}, rows);
}

Node table_tree(const ir::Table& table) {
  Node root;
  root.folder = true;
  root.title = "Bookmarks";
  const std::vector<std::vector<std::string>> rows = table_strings(table);
  if (rows.empty()) return root;
  const auto find = [&](std::initializer_list<std::string_view> names) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < rows[0].size(); ++i) {
      for (const std::string_view name : names) {
        if (util::iequals(rows[0][i], name)) return i;
      }
    }
    return std::nullopt;
  };
  const auto folder = find({"folder", "path"});
  const auto title = find({"title", "name"});
  const auto url = find({"url", "href", "link", "location"});
  const auto added = find({"added", "add_date", "date"});
  const auto tags = find({"tags", "keywords"});
  const auto description = find({"description", "note"});
  const auto get = [](const std::vector<std::string>& row, std::optional<std::size_t> index) {
    return index && *index < row.size() ? row[*index] : std::string();
  };
  for (std::size_t r = 1; r < rows.size(); ++r) {
    Node bookmark;
    bookmark.url = get(rows[r], url);
    if (bookmark.url.empty()) continue;
    bookmark.title = get(rows[r], title);
    bookmark.added = get(rows[r], added);
    bookmark.description = get(rows[r], description);
    const std::string split_text = get(rows[r], tags);  // split() returns views into it
    for (const std::string_view tag : util::split(split_text, ',', false)) bookmark.tags.emplace_back(util::trim(tag));
    Node* parent = &root;
    const std::string folder_text = get(rows[r], folder);  // split() returns views into it
    for (const std::string_view part : util::split(folder_text, '/', false)) {
      auto it = std::ranges::find_if(parent->children, [&](const Node& n) { return n.folder && n.title == part; });
      if (it == parent->children.end()) {
        Node child;
        child.folder = true;
        child.title = std::string(part);
        parent->children.push_back(std::move(child));
        it = parent->children.end() - 1;
      }
      parent = &*it;
    }
    parent->children.push_back(std::move(bookmark));
  }
  return root;
}

// ---------------------------------------------------------------------------- Netscape HTML

std::string decode_entities(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out.push_back(text[i]);
      continue;
    }
    const std::size_t semicolon = text.find(';', i);
    if (semicolon == std::string_view::npos || semicolon - i > 10) {
      out.push_back('&');
      continue;
    }
    const std::string_view name = text.substr(i + 1, semicolon - i - 1);
    std::optional<std::int64_t> code;
    if (name == "amp") code = '&';
    else if (name == "lt") code = '<';
    else if (name == "gt") code = '>';
    else if (name == "quot") code = '"';
    else if (name == "apos" || name == "#39") code = '\'';
    else if (name == "nbsp") code = 0xA0;
    else if (name.starts_with("#x") || name.starts_with("#X")) code = util::parse_int(name.substr(2), 16);
    else if (name.starts_with('#')) code = util::parse_int(name.substr(1));
    if (!code || *code <= 0 || *code > 0x10FFFF) {
      out.push_back('&');
      continue;
    }
    util::append_utf8(out, static_cast<char32_t>(*code));
    i = semicolon;
  }
  return out;
}

std::string escape_html(std::string_view text, bool attribute) {
  std::string out;
  for (const char c : text) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"' && attribute) out += "&quot;";
    else out.push_back(c);
  }
  return out;
}

struct Tag {
  std::string name;  // upper case, "/DL" for end tags
  std::vector<std::pair<std::string, std::string>> attributes;  // names upper case
  std::size_t end = 0;  // index after '>'
};

std::optional<Tag> read_tag(std::string_view text, std::size_t at) {
  const std::size_t close = text.find('>', at);
  if (close == std::string_view::npos) return std::nullopt;
  std::string_view body = text.substr(at + 1, close - at - 1);
  Tag tag;
  tag.end = close + 1;
  std::size_t i = 0;
  if (i < body.size() && body[i] == '/') {
    tag.name.push_back('/');
    ++i;
  }
  while (i < body.size() && (util::is_ascii_alpha(body[i]) || util::is_ascii_digit(body[i]) || body[i] == '!')) {
    tag.name.push_back(util::ascii_upper(body[i++]));
  }
  while (i < body.size()) {
    while (i < body.size() && util::is_ascii_space(body[i])) ++i;
    const std::size_t name_start = i;
    while (i < body.size() && body[i] != '=' && !util::is_ascii_space(body[i])) ++i;
    std::string name = util::to_upper(body.substr(name_start, i - name_start));
    while (i < body.size() && util::is_ascii_space(body[i])) ++i;
    std::string value;
    if (i < body.size() && body[i] == '=') {
      ++i;
      while (i < body.size() && util::is_ascii_space(body[i])) ++i;
      if (i < body.size() && (body[i] == '"' || body[i] == '\'')) {
        const char quote = body[i];
        const std::size_t end = body.find(quote, i + 1);
        value = std::string(body.substr(i + 1, end == std::string_view::npos ? std::string_view::npos : end - i - 1));
        i = end == std::string_view::npos ? body.size() : end + 1;
      } else {
        const std::size_t start = i;
        while (i < body.size() && !util::is_ascii_space(body[i])) ++i;
        value = std::string(body.substr(start, i - start));
      }
    }
    if (!name.empty()) tag.attributes.emplace_back(std::move(name), decode_entities(value));
    if (i == name_start) ++i;
  }
  return tag;
}

std::string attribute(const Tag& tag, std::string_view name) {
  for (const auto& [key, value] : tag.attributes) {
    if (key == name) return value;
  }
  return {};
}

/// Text up to the next '<', entity-decoded and trimmed.
std::string text_until_tag(std::string_view text, std::size_t& at) {
  const std::size_t next = text.find('<', at);
  const std::string_view raw = text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at);
  at = next == std::string_view::npos ? text.size() : next;
  return std::string(util::trim(decode_entities(raw)));
}

Result<Node> parse_netscape(std::string_view text, const Limits& limits) {
  Node root;
  root.folder = true;
  std::vector<Node*> stack{&root};
  Node* pending_folder = nullptr;
  Node* last = nullptr;
  std::uint64_t count = 0;
  std::size_t at = 0;
  bool saw_list = false;
  while ((at = text.find('<', at)) != std::string_view::npos) {
    const auto tag = read_tag(text, at);
    if (!tag) break;
    at = tag->end;
    if (tag->name == "H1" || tag->name == "TITLE") {
      const std::string title = text_until_tag(text, at);
      if (!title.empty() && (tag->name == "H1" || root.title.empty())) root.title = title;
    } else if (tag->name == "H3") {
      Node folder;
      folder.folder = true;
      folder.title = text_until_tag(text, at);
      folder.added = iso_from_unix(util::parse_int(attribute(*tag, "ADD_DATE")).value_or(0));
      folder.modified = iso_from_unix(util::parse_int(attribute(*tag, "LAST_MODIFIED")).value_or(0));
      stack.back()->children.push_back(std::move(folder));
      pending_folder = &stack.back()->children.back();
      last = pending_folder;
      if (++count > limits.max_archive_entries) return fail(Status::Limit, "too many bookmarks");
    } else if (tag->name == "DL") {
      saw_list = true;
      if (stack.size() >= max_depth) return fail(Status::Limit, "bookmark folders are nested too deeply");
      stack.push_back(pending_folder != nullptr ? pending_folder : stack.back());
      pending_folder = nullptr;
    } else if (tag->name == "/DL") {
      if (stack.size() > 1) stack.pop_back();
      pending_folder = nullptr;
    } else if (tag->name == "A") {
      Node bookmark;
      bookmark.url = attribute(*tag, "HREF");
      bookmark.title = text_until_tag(text, at);
      bookmark.added = iso_from_unix(util::parse_int(attribute(*tag, "ADD_DATE")).value_or(0));
      bookmark.modified = iso_from_unix(util::parse_int(attribute(*tag, "LAST_MODIFIED")).value_or(0));
      bookmark.icon = attribute(*tag, "ICON");
      const std::string split_text = attribute(*tag, "TAGS");  // split() returns views into it
      for (const std::string_view value : util::split(split_text, ',', false)) {
        bookmark.tags.emplace_back(util::trim(value));
      }
      stack.back()->children.push_back(std::move(bookmark));
      last = &stack.back()->children.back();
      pending_folder = nullptr;
      if (++count > limits.max_archive_entries) return fail(Status::Limit, "too many bookmarks");
    } else if (tag->name == "DD" && last != nullptr) {
      last->description = text_until_tag(text, at);
    }
  }
  if (!saw_list) return fail(Status::Decode, "not a bookmark file (no <DL> list)");
  // Stacked duplicates of the same container (a <DL> without an <H3>) are harmless.
  return root;
}

void write_netscape_nodes(std::string& out, const std::vector<Node>& nodes, int indent, std::size_t depth) {
  const std::string pad(static_cast<std::size_t>(indent) * 4, ' ');
  for (const Node& node : nodes) {
    const auto date = [](std::string_view name, const std::string& iso) {
      const std::int64_t seconds = unix_from_iso(iso);
      return seconds > 0 ? std::format(" {}=\"{}\"", name, seconds) : std::string();
    };
    if (node.folder) {
      out += pad + "<DT><H3" + date("ADD_DATE", node.added) + date("LAST_MODIFIED", node.modified) + ">" +
             escape_html(node.title, false) + "</H3>\n";
      out += pad + "<DL><p>\n";
      if (depth < max_depth) write_netscape_nodes(out, node.children, indent + 1, depth + 1);
      out += pad + "</DL><p>\n";
    } else {
      out += pad + "<DT><A HREF=\"" + escape_html(node.url, true) + "\"" + date("ADD_DATE", node.added) +
             date("LAST_MODIFIED", node.modified);
      if (!node.tags.empty()) out += " TAGS=\"" + escape_html(util::join(node.tags, ","), true) + "\"";
      if (!node.icon.empty() && node.icon.starts_with("data:")) out += " ICON=\"" + escape_html(node.icon, true) + "\"";
      out += ">" + escape_html(node.title, false) + "</A>\n";
      if (!node.description.empty()) out += pad + "<DD>" + escape_html(node.description, false) + "\n";
    }
  }
}

std::string write_netscape(const Node& root) {
  const std::string title = root.title.empty() ? std::string("Bookmarks") : root.title;
  std::string out =
      "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
      "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
      "<TITLE>" + escape_html(title, false) + "</TITLE>\n<H1>" + escape_html(title, false) + "</H1>\n<DL><p>\n";
  write_netscape_nodes(out, root.children, 1, 0);
  out += "</DL><p>\n";
  return out;
}

// ---------------------------------------------------------------------------- browser JSON

std::string chrome_time(const Json& value) {
  // Microseconds since 1601-01-01, as a decimal string.
  const auto micros = util::parse_int(value.as_string());
  if (!micros || *micros <= 0) return {};
  return iso_from_unix(*micros / 1'000'000 - 11'644'473'600LL);
}

Node chrome_node(const Json& value, std::size_t depth) {
  Node node;
  node.folder = value["type"].as_string() == "folder";
  node.title = value["name"].as_string();
  node.url = value["url"].as_string();
  node.added = chrome_time(value["date_added"]);
  node.modified = chrome_time(value["date_modified"]);
  if (depth < max_depth) {
    for (const Json& child : value["children"].elements()) node.children.push_back(chrome_node(child, depth + 1));
  }
  return node;
}

Node firefox_node(const Json& value, std::size_t depth) {
  Node node;
  const std::string& type = value["type"].as_string();
  node.folder = type == "text/x-moz-place-container";
  node.title = value["title"].as_string();
  node.url = value["uri"].as_string();
  if (const auto added = value["dateAdded"].get_int64()) node.added = iso_from_unix(*added / 1'000'000);
  if (const auto modified = value["lastModified"].get_int64()) node.modified = iso_from_unix(*modified / 1'000'000);
  for (const std::string_view tag : util::split(value["tags"].as_string(), ',', false)) node.tags.emplace_back(tag);
  if (depth < max_depth) {
    for (const Json& child : value["children"].elements()) {
      if (child["type"].as_string() == "text/x-moz-place-separator") continue;
      node.children.push_back(firefox_node(child, depth + 1));
    }
  }
  return node;
}

Result<Node> parse_browser_json(std::string_view text) {
  CVT_TRY_ASSIGN(const Json root, util::parse_json(text, 256));
  Node tree;
  tree.folder = true;
  tree.title = "Bookmarks";
  if (const Json* roots = root.find("roots"); roots != nullptr && roots->is_object()) {
    for (const auto& [key, value] : roots->items()) {
      if (!value.is_object() || !value.contains("children")) continue;
      tree.children.push_back(chrome_node(value, 1));
    }
    return tree;
  }
  if (root["type"].as_string() == "text/x-moz-place-container") {
    Node firefox = firefox_node(root, 0);
    tree.children = std::move(firefox.children);
    return tree;
  }
  return fail(Status::Decode, "not a browser bookmark export (no \"roots\" or Firefox container)");
}

// ---------------------------------------------------------------------------- link files

std::string ini_value(std::string_view text, std::string_view section, std::string_view key) {
  bool in_section = false;
  for (const std::string_view raw : util::split_lines(text)) {
    const std::string_view line = util::trim(raw);
    if (line.starts_with('[')) {
      in_section = util::iequals(line, std::string("[") + std::string(section) + "]");
      continue;
    }
    if (!in_section) continue;
    const std::size_t eq = line.find('=');
    if (eq != std::string_view::npos && util::iequals(util::trim(line.substr(0, eq)), key)) {
      return std::string(util::trim(line.substr(eq + 1)));
    }
  }
  return {};
}

std::string file_stem(std::string_view filename) {
  const std::size_t dot = filename.rfind('.');
  return std::string(filename.substr(0, dot == std::string_view::npos ? filename.size() : dot));
}

/// Minimal binary property list reader (enough for web locations and small plists).
class BinaryPlist {
 public:
  explicit BinaryPlist(ByteView data) : data_(data) {}

  Result<Json> parse() {
    if (data_.size() < 40 || as_chars(data_.first(8)) != "bplist00") return fail(Status::Decode, "not a binary property list");
    const ByteView trailer = data_.last(32);
    offset_size_ = std::to_integer<std::uint8_t>(trailer[6]);
    ref_size_ = std::to_integer<std::uint8_t>(trailer[7]);
    count_ = be(trailer.subspan(8, 8));
    const std::uint64_t top = be(trailer.subspan(16, 8));
    table_ = be(trailer.subspan(24, 8));
    if (offset_size_ == 0 || offset_size_ > 8 || ref_size_ == 0 || ref_size_ > 8 || count_ == 0 || top >= count_ ||
        count_ > data_.size() || table_ > data_.size() || count_ * offset_size_ > data_.size() - table_) {
      return fail(Status::Decode, "damaged binary property list");
    }
    return object(top, 0);
  }

 private:
  static std::uint64_t be(ByteView bytes) {
    std::uint64_t value = 0;
    for (const std::byte b : bytes) value = (value << 8) | std::to_integer<std::uint8_t>(b);
    return value;
  }

  Result<std::uint64_t> offset_of(std::uint64_t index) const {
    if (index >= count_) return fail(Status::Decode, "binary property list reference out of range");
    const std::uint64_t value = be(data_.subspan(static_cast<std::size_t>(table_ + index * offset_size_), offset_size_));
    if (value >= data_.size()) return fail(Status::Decode, "binary property list offset out of range");
    return value;
  }

  Result<std::pair<std::uint64_t, std::uint64_t>> length(std::uint64_t at, std::uint8_t low) const {
    // Returns (length, position after the marker and length field).
    if (low != 0x0F) return std::pair{std::uint64_t{low}, at + 1};
    if (at + 2 > data_.size()) return fail(Status::Decode, "truncated binary property list");
    const auto marker = std::to_integer<std::uint8_t>(data_[static_cast<std::size_t>(at + 1)]);
    if ((marker & 0xF0) != 0x10) return fail(Status::Decode, "bad length in binary property list");
    const std::uint64_t bytes = 1ULL << (marker & 0x0F);
    if (bytes > 8 || at + 2 + bytes > data_.size()) return fail(Status::Decode, "truncated binary property list");
    return std::pair{be(data_.subspan(static_cast<std::size_t>(at + 2), static_cast<std::size_t>(bytes))), at + 2 + bytes};
  }

  Result<Json> object(std::uint64_t index, int depth) {
    if (depth > 32) return fail(Status::Limit, "binary property list nested too deeply");
    if (++visited_ > 100000) return fail(Status::Limit, "binary property list too large");
    CVT_TRY_ASSIGN(const std::uint64_t at, offset_of(index));
    const auto marker = std::to_integer<std::uint8_t>(data_[static_cast<std::size_t>(at)]);
    const std::uint8_t high = marker >> 4;
    const std::uint8_t low = marker & 0x0F;
    switch (high) {
      case 0x0: return marker == 0x09 ? Json(true) : marker == 0x08 ? Json(false) : Json(nullptr);
      case 0x1: {
        const std::uint64_t bytes = 1ULL << low;
        if (bytes > 8 || at + 1 + bytes > data_.size()) return fail(Status::Decode, "truncated integer");
        return Json(static_cast<std::int64_t>(be(data_.subspan(static_cast<std::size_t>(at + 1), static_cast<std::size_t>(bytes)))));
      }
      case 0x5:
      case 0x6:
      case 0x4: {
        CVT_TRY_ASSIGN(const auto info, length(at, low));
        const std::uint64_t bytes = high == 0x6 ? info.first * 2 : info.first;
        if (info.second + bytes > data_.size()) return fail(Status::Decode, "truncated string");
        const ByteView raw = data_.subspan(static_cast<std::size_t>(info.second), static_cast<std::size_t>(bytes));
        if (high == 0x4) return Json(util::base64_encode(raw));
        if (high == 0x5) return Json(util::sanitize_utf8(as_chars(raw)));
        return Json(util::decode_text(raw, util::TextEncoding::Utf16BE).text);
      }
      case 0xA:
      case 0xD: {
        CVT_TRY_ASSIGN(const auto info, length(at, low));
        const std::uint64_t refs = high == 0xD ? info.first * 2 : info.first;
        if (refs > count_ || info.second + refs * ref_size_ > data_.size()) return fail(Status::Decode, "truncated container");
        const auto ref = [&](std::uint64_t i) {
          return be(data_.subspan(static_cast<std::size_t>(info.second + i * ref_size_), ref_size_));
        };
        if (high == 0xA) {
          Json array = Json::array();
          for (std::uint64_t i = 0; i < info.first; ++i) {
            CVT_TRY_ASSIGN(Json item, object(ref(i), depth + 1));
            array.push_back(std::move(item));
          }
          return array;
        }
        Json dict = Json::object();
        for (std::uint64_t i = 0; i < info.first; ++i) {
          CVT_TRY_ASSIGN(const Json key, object(ref(i), depth + 1));
          CVT_TRY_ASSIGN(Json item, object(ref(info.first + i), depth + 1));
          dict[key.as_string()] = std::move(item);
        }
        return dict;
      }
      default: return Json(nullptr);  // reals, dates, UIDs, sets: not needed for links
    }
  }

  ByteView data_;
  std::uint8_t offset_size_ = 0;
  std::uint8_t ref_size_ = 0;
  std::uint64_t count_ = 0;
  std::uint64_t table_ = 0;
  std::uint64_t visited_ = 0;
};

Result<Json> read_link(std::string_view format, const ReadContext& ctx) {
  Json out = Json::object({{"type", "link"}});
  std::string url;
  std::string title;
  if (format == "webloc") {
    if (as_chars(ctx.input.first(std::min<std::size_t>(ctx.input.size(), 8))) == "bplist00") {
      BinaryPlist plist(ctx.input);
      CVT_TRY_ASSIGN(const Json value, plist.parse());
      url = value["URL"].as_string();
    } else {
      util::XmlParseOptions options;
      options.replace_invalid_text = true;
      auto doc = util::parse_xml(ctx.input, options);
      if (!doc) return fail(doc.error().code, "not a web location file: {}", doc.error().message);
      std::string key;
      for (const util::XmlNode child : doc->root().child_local("dict").elements()) {
        if (child.local_name() == "key") key = child.text();
        else if (key == "URL") url = std::string(util::trim(child.text()));
      }
    }
  } else {
    CVT_TRY_ASSIGN(const std::string text, decode_text(ctx.input, "auto"));
    if (format == "url-shortcut") {
      url = ini_value(text, "InternetShortcut", "URL");
    } else {
      url = ini_value(text, "Desktop Entry", "URL");
      title = ini_value(text, "Desktop Entry", "Name");
      if (const std::string comment = ini_value(text, "Desktop Entry", "Comment"); !comment.empty()) out["description"] = comment;
    }
  }
  if (url.empty()) return fail(Status::Decode, "the link file holds no URL");
  if (title.empty()) title = file_stem(ctx.filename);
  out["url"] = url;
  if (!title.empty()) out["title"] = title;
  return out;
}

/// The URL and title of a link, or of the first bookmark in a collection.
std::pair<std::string, std::string> first_link(const Json& value, int depth = 0) {
  if (depth > 64) return {};
  if (!value["url"].as_string().empty()) return {value["url"].as_string(), value["title"].as_string()};
  for (const Json& child : value["children"].elements()) {
    auto found = first_link(child, depth + 1);
    if (!found.first.empty()) return found;
  }
  for (const Json& child : value.elements()) {
    auto found = first_link(child, depth + 1);
    if (!found.first.empty()) return found;
  }
  return {};
}

Result<Bytes> write_link(std::string_view format, const Json& value) {
  const auto [url, title] = first_link(value);
  if (url.empty()) return fail(Status::Encode, "the data has no \"url\" to write as a link");
  const std::string clean_url = util::replace_all(util::replace_all(url, "\r", ""), "\n", "");
  const std::string clean_title = util::replace_all(util::replace_all(title, "\r", " "), "\n", " ");
  if (format == "url-shortcut") return to_bytes("[InternetShortcut]\r\nURL=" + clean_url + "\r\n");
  if (format == "desktop-link") {
    std::string out = "[Desktop Entry]\nVersion=1.0\nType=Link\nName=" + (clean_title.empty() ? clean_url : clean_title) + "\n";
    out += "URL=" + clean_url + "\nIcon=text-html\n";
    return to_bytes(out);
  }
  return to_bytes(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n<dict>\n\t<key>URL</key>\n\t<string>" + util::xml_escape(clean_url) +
      "</string>\n</dict>\n</plist>\n");
}

// ---------------------------------------------------------------------------- registration

Result<Node> read_tree(std::string_view format, const ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, decode_text(ctx.input, ctx.options.get_string("source_encoding", "auto")));
  if (format == "browser-bookmarks") return parse_browser_json(text);
  return parse_netscape(text, ctx.limits);
}

FormatInfo format_info(std::string id, std::string name, std::string description, std::vector<std::string> extensions,
                       std::string mime, int popularity, std::function<int(ByteView, std::string_view)> sniff) {
  FormatInfo info;
  info.id = std::move(id);
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Data;
  info.extensions = std::move(extensions);
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.sniff = std::move(sniff);
  return info;
}

void register_collection(Registry& registry, const std::string& id, bool writable) {
  Codec data;
  data.format_id = id;
  data.kind = Kind::Data;
  data.read = [id](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(const Node tree, read_tree(id, ctx));
    return ir::Data{tree_json(tree)};
  };
  data.read_options = {source_encoding_option()};
  if (writable) {
    data.write = [](const AnyIR& value, WriteContext&) -> Result<Bytes> {
      Node root = json_node(value.get<ir::Data>().value, 0);
      root.folder = true;
      return to_bytes(write_netscape(root));
    };
  }
  registry.add_codec(std::move(data));

  Codec document;
  document.format_id = id;
  document.kind = Kind::Document;
  document.read = [id](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(const Node tree, read_tree(id, ctx));
    return tree_document(tree);
  };
  document.read_options = {source_encoding_option()};
  document.read_cost = 15;
  document.lossy = true;
  registry.add_codec(std::move(document));

  Codec table;
  table.format_id = id;
  table.kind = Kind::Table;
  table.read = [id](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(const Node tree, read_tree(id, ctx));
    return tree_table(tree);
  };
  table.read_options = {source_encoding_option()};
  table.read_cost = 15;
  if (writable) {
    table.write = [](const AnyIR& value, WriteContext&) -> Result<Bytes> {
      return to_bytes(write_netscape(table_tree(value.get<ir::Table>())));
    };
  }
  registry.add_codec(std::move(table));
}

}  // namespace

void register_bookmarks(Registry& registry) {
  registry.add_format(format_info("netscape-bookmarks", "Browser bookmarks (HTML)",
                                  "Netscape bookmark file, the HTML export of every browser.", {"html", "htm"}, "text/html",
                                  40, [](ByteView data, std::string_view) {
                                    const std::string head = util::to_upper(sniff_text(data, 512));
                                    return head.find("<!DOCTYPE NETSCAPE-BOOKMARK-FILE-1>") != std::string::npos ? 97 : 0;
                                  }));
  register_collection(registry, "netscape-bookmarks", true);

  registry.add_format(format_info("browser-bookmarks", "Browser bookmarks (JSON)",
                                  "Chrome / Edge \"Bookmarks\" file or Firefox JSON backup.", {"json"}, "application/json",
                                  20, [](ByteView data, std::string_view) {
                                    const std::string head = sniff_text(data, 4096);
                                    if (!util::trim(head).starts_with('{')) return 0;
                                    if (head.find("\"roots\"") != std::string::npos &&
                                        head.find("\"bookmark_bar\"") != std::string::npos) {
                                      return 93;
                                    }
                                    return head.find("\"text/x-moz-place-container\"") != std::string::npos ? 92 : 0;
                                  }));
  register_collection(registry, "browser-bookmarks", false);

  struct LinkFormat {
    std::string id;
    std::string name;
    std::string description;
    std::string extension;
    std::string mime;
    std::function<int(ByteView, std::string_view)> sniff;
  };
  const LinkFormat links[] = {
      {"url-shortcut", "Internet shortcut", "Windows Internet Shortcut file.", "url", "application/x-mswinurl",
       [](ByteView data, std::string_view) {
         return util::istarts_with(util::trim(sniff_text(data, 256)), "[InternetShortcut]") ? 95 : 0;
       }},
      {"webloc", "Web location", "macOS web location file (property list).", "webloc", "application/x-webloc",
       [](ByteView data, std::string_view filename) {
         const std::string head = sniff_text(data, 1024);
         // A binary property list says nothing about being a link; trust the name then.
         if (head.starts_with("bplist00")) return util::iends_with(filename, ".webloc") && head.find("URL") != std::string::npos ? 97 : 0;
         return head.find("<plist") != std::string::npos && head.find("<key>URL</key>") != std::string::npos ? 90 : 0;
       }},
      {"desktop-link", "Desktop link", "freedesktop.org desktop entry of type Link.", "desktop", "application/x-desktop",
       [](ByteView data, std::string_view) {
         const std::string head = sniff_text(data, 1024);
         return head.find("[Desktop Entry]") != std::string::npos && head.find("Type=Link") != std::string::npos ? 92 : 0;
       }},
  };
  for (const LinkFormat& link : links) {
    registry.add_format(format_info(link.id, link.name, link.description, {link.extension}, link.mime, 20, link.sniff));
    Codec codec;
    codec.format_id = link.id;
    codec.kind = Kind::Data;
    const std::string id = link.id;
    codec.read = [id](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(Json value, read_link(id, ctx));
      return ir::Data{std::move(value)};
    };
    codec.write = [id](const AnyIR& value, WriteContext&) -> Result<Bytes> { return write_link(id, value.get<ir::Data>().value); };
    registry.add_codec(std::move(codec));
  }
}

}  // namespace cvt::pim
