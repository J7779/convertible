// Convertible core - pim family: playlists.
//
//   m3u, m3u8   (extended) M3U, including HLS media and master playlists
//   pls         Winamp / Shoutcast PLS
//   xspf        XML Shareable Playlist Format
//   asx         Windows Media ASX
//   wpl         Windows Media Player playlist (SMIL)
//   itunes-xml  iTunes / Music library XML (read only)
//   cue         CUE sheets (FILE / TRACK / INDEX mm:ss:ff)
//
// Playlists have no IR of their own. Each format reads into a Table (one row
// per entry; columns location, title, artist, album, duration_s, track and
// a few format-specific ones) and into Data, and writes from both:
//
//   Table header: location | title | artist | album | duration_s | track | group
//   Data: {"type": "playlist", "title": "...",
//          "entries": [{"location", "title", "artist", "album", "duration_s", "track", "group",
//                       "attributes": {...}}]}
//   HLS:  {"type": "hls-media", "version", "target_duration", "media_sequence", "endlist",
//          "tags": [...], "segments": [{"uri", "duration_s", "title", "discontinuity", "key"}]}
//         {"type": "hls-master", "variants": [{"uri", "bandwidth", "resolution", "codecs", ...}],
//          "media": [{...EXT-X-MEDIA attributes}]}
//   CUE:  {"type": "cue", "title", "performer", "rem": {...}, "files": [{"file", "type",
//          "tracks": [{"number", "type", "title", "performer", "indexes": {"01": seconds}}]}]}
//
// Table writers accept common header aliases (path, url, file, name, creator,
// performer, length, seconds, ...), so a CSV or spreadsheet becomes a playlist.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/pim/pim.hpp"
#include "ir/data.hpp"
#include "ir/table.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::pim {

namespace {

using namespace std::string_view_literals;

struct Entry {
  std::string location;
  std::string title;
  std::string artist;
  std::string album;
  std::optional<double> duration;  ///< seconds
  std::string track;
  std::string group;
  std::vector<std::pair<std::string, std::string>> attributes;
};

struct Playlist {
  std::string title;
  std::vector<Entry> entries;
};

std::string number_text(double value) {
  if (std::abs(value - std::round(value)) < 1e-9) return std::to_string(static_cast<std::int64_t>(std::llround(value)));
  return util::format_double(std::round(value * 1000.0) / 1000.0);
}

std::optional<double> parse_seconds(std::string_view text) {
  text = util::trim(text);
  if (text.empty()) return std::nullopt;
  if (text.find(':') != std::string_view::npos) {
    double total = 0.0;
    for (const std::string_view part : util::split(text, ':')) {
      const auto value = util::parse_double(util::trim(part));
      if (!value || *value < 0) return std::nullopt;
      total = total * 60.0 + *value;
    }
    return total;
  }
  const auto value = util::parse_double(text);
  if (!value || !std::isfinite(*value)) return std::nullopt;
  return *value;
}

// ---------------------------------------------------------------------------- Table / Data mapping

const std::vector<std::string>& table_header() {
  static const std::vector<std::string> header = {"location", "title", "artist", "album", "duration_s", "track", "group"};
  return header;
}

ir::Table to_table(const Playlist& playlist) {
  ir::Sheet sheet;
  sheet.name = playlist.title.empty() ? std::string("Playlist") : playlist.title;
  sheet.has_header = true;
  std::vector<ir::Cell> head;
  for (const std::string& title : table_header()) head.push_back(ir::Cell::string(title));
  sheet.rows.push_back(std::move(head));
  const auto text = [](const std::string& value) { return value.empty() ? ir::Cell{} : ir::Cell::string(value); };
  for (const Entry& entry : playlist.entries) {
    sheet.rows.push_back({text(entry.location), text(entry.title), text(entry.artist), text(entry.album),
                          entry.duration ? ir::Cell::numeric(*entry.duration, number_text(*entry.duration)) : ir::Cell{},
                          text(entry.track), text(entry.group)});
  }
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

std::optional<std::size_t> column(const std::vector<std::string>& header, std::initializer_list<std::string_view> names) {
  for (std::size_t i = 0; i < header.size(); ++i) {
    std::string key;
    for (const char c : header[i]) {
      if (c != ' ' && c != '_' && c != '-') key.push_back(util::ascii_lower(c));
    }
    for (const std::string_view name : names) {
      if (key == name) return i;
    }
  }
  return std::nullopt;
}

Result<Playlist> from_table(const ir::Table& table) {
  Playlist playlist;
  if (table.sheets.empty() || table.sheets.front().rows.empty()) return playlist;
  const ir::Sheet& sheet = table.sheets.front();
  if (!sheet.name.empty() && sheet.name != "Playlist" && sheet.name != "Sheet1") playlist.title = sheet.name;
  std::vector<std::string> header;
  for (const ir::Cell& cell : sheet.rows.front()) header.push_back(cell.text);
  const auto location = column(header, {"location", "path", "url", "uri", "file", "filename", "src", "href"});
  if (!location) {
    return fail(Status::Encode, "the table needs a 'location' (or path, url, file) column to become a playlist");
  }
  const auto title = column(header, {"title", "name"});
  const auto artist = column(header, {"artist", "creator", "performer", "author"});
  const auto album = column(header, {"album"});
  const auto duration = column(header, {"durations", "duration", "length", "seconds", "time"});
  const auto track = column(header, {"track", "tracknum", "tracknumber", "number"});
  const auto group = column(header, {"group", "grouptitle", "category"});
  const auto get = [](const std::vector<ir::Cell>& row, std::optional<std::size_t> index) {
    return index && *index < row.size() ? row[*index].text : std::string();
  };
  for (std::size_t r = 1; r < sheet.rows.size(); ++r) {
    const std::vector<ir::Cell>& row = sheet.rows[r];
    Entry entry;
    entry.location = get(row, location);
    if (entry.location.empty()) continue;
    entry.title = get(row, title);
    entry.artist = get(row, artist);
    entry.album = get(row, album);
    entry.duration = parse_seconds(get(row, duration));
    entry.track = get(row, track);
    entry.group = get(row, group);
    playlist.entries.push_back(std::move(entry));
  }
  return playlist;
}

void set_text(Json& object, std::string_view key, const std::string& value) {
  if (!value.empty()) object[key] = value;
}

Json to_data(const Playlist& playlist) {
  Json root = Json::object({{"type", "playlist"}});
  set_text(root, "title", playlist.title);
  Json entries = Json::array();
  for (const Entry& entry : playlist.entries) {
    Json item = Json::object({{"location", entry.location}});
    set_text(item, "title", entry.title);
    set_text(item, "artist", entry.artist);
    set_text(item, "album", entry.album);
    if (entry.duration) item["duration_s"] = *entry.duration;
    set_text(item, "track", entry.track);
    set_text(item, "group", entry.group);
    if (!entry.attributes.empty()) {
      Json attributes = Json::object();
      for (const auto& [key, value] : entry.attributes) attributes[key] = value;
      item["attributes"] = std::move(attributes);
    }
    entries.push_back(std::move(item));
  }
  root["entries"] = std::move(entries);
  return root;
}

std::string json_text(const Json& value) {
  if (value.is_string()) return value.as_string();
  if (value.is_number()) return number_text(value.as_double());
  return {};
}

Playlist from_data(const Json& root) {
  Playlist playlist;
  playlist.title = json_text(root["title"]);
  const Json* entries = root.find("entries");
  if (entries == nullptr) entries = root.find("segments");
  if (entries == nullptr && root.is_array()) entries = &root;
  if (entries == nullptr) return playlist;
  for (const Json& item : entries->elements()) {
    Entry entry;
    entry.location = json_text(item["location"]);
    if (entry.location.empty()) entry.location = json_text(item["uri"]);
    if (entry.location.empty()) entry.location = json_text(item["url"]);
    if (entry.location.empty()) continue;
    entry.title = json_text(item["title"]);
    entry.artist = json_text(item["artist"]);
    entry.album = json_text(item["album"]);
    if (const auto seconds = item["duration_s"].get_double()) entry.duration = *seconds;
    entry.track = json_text(item["track"]);
    entry.group = json_text(item["group"]);
    for (const auto& [key, value] : item["attributes"].items()) entry.attributes.emplace_back(key, json_text(value));
    playlist.entries.push_back(std::move(entry));
  }
  return playlist;
}

// ============================================================================ M3U / HLS

/// `key="value" key2=value2` attribute lists of #EXTINF and #EXT-X- tags.
std::vector<std::pair<std::string, std::string>> attribute_list(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> out;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == ',')) ++i;
    const std::size_t eq = text.find('=', i);
    if (eq == std::string_view::npos) break;
    std::string key(util::trim(text.substr(i, eq - i)));
    i = eq + 1;
    std::string value;
    if (i < text.size() && text[i] == '"') {
      const std::size_t close = text.find('"', i + 1);
      value = std::string(text.substr(i + 1, close == std::string_view::npos ? std::string_view::npos : close - i - 1));
      i = close == std::string_view::npos ? text.size() : close + 1;
    } else {
      std::size_t end = i;
      while (end < text.size() && text[end] != ',' && text[end] != ' ') ++end;
      value = std::string(text.substr(i, end - i));
      i = end;
    }
    if (!key.empty()) out.emplace_back(std::move(key), std::move(value));
  }
  return out;
}

bool is_hls(std::string_view text) {
  return text.find("#EXT-X-TARGETDURATION") != std::string_view::npos ||
         text.find("#EXT-X-STREAM-INF") != std::string_view::npos ||
         text.find("#EXT-X-MEDIA-SEQUENCE") != std::string_view::npos;
}

Playlist parse_m3u(std::string_view text) {
  Playlist playlist;
  Entry pending;
  bool has_pending = false;
  for (const std::string_view raw : util::split_lines(text)) {
    const std::string_view line = util::trim(raw);
    if (line.empty()) continue;
    if (line.starts_with('#')) {
      if (util::istarts_with(line, "#PLAYLIST:")) {
        playlist.title = std::string(util::trim(line.substr(10)));
      } else if (util::istarts_with(line, "#EXTINF:")) {
        has_pending = true;
        const std::string_view rest = line.substr(8);
        // Duration, then attributes, then ",title" (the comma after the last quoted value).
        std::size_t comma = std::string_view::npos;
        bool quoted = false;
        for (std::size_t i = 0; i < rest.size(); ++i) {
          if (rest[i] == '"') quoted = !quoted;
          if (rest[i] == ',' && !quoted) {
            comma = i;
            break;
          }
        }
        const std::string_view head = rest.substr(0, comma);
        pending.title = comma == std::string_view::npos ? std::string() : std::string(util::trim(rest.substr(comma + 1)));
        std::size_t space = 0;
        while (space < head.size() && !util::is_ascii_space(head[space])) ++space;
        const auto seconds = util::parse_double(head.substr(0, space));
        if (seconds && *seconds >= 0) pending.duration = *seconds;
        for (auto& [key, value] : attribute_list(head.substr(space))) {
          if (key == "group-title") pending.group = value;
          else pending.attributes.emplace_back(std::move(key), std::move(value));
        }
      } else if (util::istarts_with(line, "#EXTGRP:")) {
        has_pending = true;
        pending.group = std::string(util::trim(line.substr(8)));
      } else if (util::istarts_with(line, "#EXTALB:")) {
        has_pending = true;
        pending.album = std::string(util::trim(line.substr(8)));
      } else if (util::istarts_with(line, "#EXTART:")) {
        has_pending = true;
        pending.artist = std::string(util::trim(line.substr(8)));
      }
      continue;
    }
    pending.location = std::string(line);
    playlist.entries.push_back(std::move(pending));
    pending = Entry{};
    has_pending = false;
  }
  (void)has_pending;
  return playlist;
}

std::string write_m3u(const Playlist& playlist) {
  std::string out = "#EXTM3U\n";
  if (!playlist.title.empty()) out += "#PLAYLIST:" + util::replace_all(playlist.title, "\n", " ") + "\n";
  const auto clean = [](const std::string& value) { return util::replace_all(util::replace_all(value, "\r", " "), "\n", " "); };
  for (const Entry& entry : playlist.entries) {
    std::string attributes;
    for (const auto& [key, value] : entry.attributes) {
      if (util::xml_is_name(key)) attributes += std::format(" {}=\"{}\"", key, util::replace_all(clean(value), "\"", "'"));
    }
    if (!entry.group.empty()) attributes += std::format(" group-title=\"{}\"", util::replace_all(clean(entry.group), "\"", "'"));
    std::string title = clean(entry.title);
    if (title.empty() && !entry.artist.empty()) title = clean(entry.artist);
    out += std::format("#EXTINF:{}{},{}\n", entry.duration ? number_text(*entry.duration) : std::string("-1"), attributes, title);
    if (!entry.album.empty()) out += "#EXTALB:" + clean(entry.album) + "\n";
    if (!entry.artist.empty()) out += "#EXTART:" + clean(entry.artist) + "\n";
    out += clean(entry.location) + "\n";
  }
  return out;
}

Json parse_hls(std::string_view text) {
  const bool master = text.find("#EXT-X-STREAM-INF") != std::string_view::npos;
  Json root = Json::object({{"type", master ? "hls-master" : "hls-media"}});
  Json segments = Json::array();
  Json variants = Json::array();
  Json media = Json::array();
  Json tags = Json::array();
  Json pending = Json::object();
  Json key;
  bool discontinuity = false;
  for (const std::string_view raw : util::split_lines(text)) {
    const std::string_view line = util::trim(raw);
    if (line.empty() || line == "#EXTM3U") continue;
    if (line.starts_with('#')) {
      const std::size_t colon = line.find(':');
      const std::string_view tag = line.substr(1, colon == std::string_view::npos ? std::string_view::npos : colon - 1);
      const std::string_view value = colon == std::string_view::npos ? std::string_view() : line.substr(colon + 1);
      if (tag == "EXT-X-VERSION") root["version"] = util::parse_int(value).value_or(0);
      else if (tag == "EXT-X-TARGETDURATION") root["target_duration"] = util::parse_int(value).value_or(0);
      else if (tag == "EXT-X-MEDIA-SEQUENCE") root["media_sequence"] = util::parse_int(value).value_or(0);
      else if (tag == "EXT-X-PLAYLIST-TYPE") root["playlist_type"] = std::string(value);
      else if (tag == "EXT-X-ENDLIST") root["endlist"] = true;
      else if (tag == "EXT-X-DISCONTINUITY") discontinuity = true;
      else if (tag == "EXT-X-KEY") {
        key = Json::object();
        for (const auto& [k, v] : attribute_list(value)) key[util::to_lower(k)] = v;
      } else if (tag == "EXTINF") {
        const std::size_t comma = value.find(',');
        pending["duration_s"] = util::parse_double(value.substr(0, comma)).value_or(0.0);
        if (comma != std::string_view::npos && !util::trim(value.substr(comma + 1)).empty()) {
          pending["title"] = std::string(util::trim(value.substr(comma + 1)));
        }
      } else if (tag == "EXT-X-STREAM-INF") {
        pending = Json::object();
        for (const auto& [k, v] : attribute_list(value)) {
          const std::string name = util::to_lower(k);
          if (const auto number = util::parse_int(v); number && (name == "bandwidth" || name == "average-bandwidth")) {
            pending[name] = *number;
          } else {
            pending[name] = v;
          }
        }
      } else if (tag == "EXT-X-MEDIA") {
        Json item = Json::object();
        for (const auto& [k, v] : attribute_list(value)) item[util::to_lower(k)] = v;
        media.push_back(std::move(item));
      } else {
        tags.push_back(std::string(line));
      }
      continue;
    }
    pending["uri"] = std::string(line);
    if (master) {
      variants.push_back(std::move(pending));
    } else {
      if (discontinuity) pending["discontinuity"] = true;
      if (key.is_object()) pending["key"] = key;
      segments.push_back(std::move(pending));
      discontinuity = false;
    }
    pending = Json::object();
  }
  if (master) {
    root["variants"] = std::move(variants);
    if (!media.empty()) root["media"] = std::move(media);
  } else {
    root["segments"] = std::move(segments);
  }
  if (!tags.empty()) root["tags"] = std::move(tags);
  return root;
}

Playlist hls_playlist(const Json& hls) {
  Playlist playlist;
  const bool master = hls["type"].as_string() == "hls-master";
  for (const Json& item : (master ? hls["variants"] : hls["segments"]).elements()) {
    Entry entry;
    entry.location = item["uri"].as_string();
    entry.title = item["title"].as_string();
    if (master) {
      entry.title = item["resolution"].as_string();
      if (const auto bandwidth = item["bandwidth"].get_int64()) entry.attributes.emplace_back("bandwidth", std::to_string(*bandwidth));
    }
    if (const auto seconds = item["duration_s"].get_double()) entry.duration = *seconds;
    playlist.entries.push_back(std::move(entry));
  }
  return playlist;
}

std::string write_hls(const Json& root) {
  std::string out = "#EXTM3U\n";
  if (const auto version = root["version"].get_int64()) out += std::format("#EXT-X-VERSION:{}\n", *version);
  const auto attributes = [](const Json& object, std::initializer_list<std::string_view> skip) {
    std::vector<std::string> parts;
    for (const auto& [key, value] : object.items()) {
      if (std::ranges::find(skip, key) != skip.end()) continue;
      std::string name = util::to_upper(key);
      const bool numeric = value.is_number();
      const std::string text = numeric ? number_text(value.as_double()) : value.as_string();
      const bool quote = !numeric && name != "RESOLUTION" && name != "METHOD" && name != "TYPE" && name != "DEFAULT" &&
                         name != "AUTOSELECT" && name != "FORCED" && name != "FRAME-RATE" && name != "HDCP-LEVEL";
      parts.push_back(quote ? std::format("{}=\"{}\"", name, text) : std::format("{}={}", name, text));
    }
    return util::join(parts, ",");
  };
  if (root["type"].as_string() == "hls-master") {
    for (const Json& media : root["media"].elements()) out += "#EXT-X-MEDIA:" + attributes(media, {}) + "\n";
    for (const Json& variant : root["variants"].elements()) {
      out += "#EXT-X-STREAM-INF:" + attributes(variant, {"uri"}) + "\n" + variant["uri"].as_string() + "\n";
    }
    return out;
  }
  if (const auto target = root["target_duration"].get_int64()) out += std::format("#EXT-X-TARGETDURATION:{}\n", *target);
  if (const auto sequence = root["media_sequence"].get_int64()) out += std::format("#EXT-X-MEDIA-SEQUENCE:{}\n", *sequence);
  if (!root["playlist_type"].as_string().empty()) out += "#EXT-X-PLAYLIST-TYPE:" + root["playlist_type"].as_string() + "\n";
  for (const Json& tag : root["tags"].elements()) {
    if (tag.as_string().starts_with("#EXT")) out += tag.as_string() + "\n";
  }
  Json last_key;
  for (const Json& segment : root["segments"].elements()) {
    if (segment["discontinuity"].as_bool()) out += "#EXT-X-DISCONTINUITY\n";
    if (const Json* key = segment.find("key"); key != nullptr && !(*key == last_key)) {
      out += "#EXT-X-KEY:" + attributes(*key, {}) + "\n";
      last_key = *key;
    }
    out += std::format("#EXTINF:{},{}\n{}\n", number_text(segment["duration_s"].as_double()), segment["title"].as_string(),
                       segment["uri"].as_string());
  }
  if (root["endlist"].as_bool()) out += "#EXT-X-ENDLIST\n";
  return out;
}

// ============================================================================ PLS

Playlist parse_pls(std::string_view text) {
  Playlist playlist;
  std::map<std::int64_t, Entry> entries;
  for (const std::string_view raw : util::split_lines(text)) {
    const std::string_view line = util::trim(raw);
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string_view key = util::trim(line.substr(0, eq));
    const std::string value(util::trim(line.substr(eq + 1)));
    std::size_t digits = key.size();
    while (digits > 0 && util::is_ascii_digit(key[digits - 1])) --digits;
    const auto index = util::parse_int(key.substr(digits));
    if (util::iequals(key, "X-Playlist-Title") || util::iequals(key, "PlaylistName")) playlist.title = value;
    if (!index || *index < 0 || *index > 10'000'000 || entries.size() > 1'000'000) continue;
    const std::string_view name = key.substr(0, digits);
    Entry& entry = entries[*index];
    if (util::iequals(name, "File")) entry.location = value;
    else if (util::iequals(name, "Title")) entry.title = value;
    else if (util::iequals(name, "Length")) {
      const auto seconds = util::parse_double(value);
      if (seconds && *seconds >= 0) entry.duration = *seconds;
    }
  }
  for (auto& [index, entry] : entries) {
    if (!entry.location.empty()) playlist.entries.push_back(std::move(entry));
  }
  return playlist;
}

std::string write_pls(const Playlist& playlist) {
  std::string out = "[playlist]\n";
  if (!playlist.title.empty()) out += "X-Playlist-Title=" + util::replace_all(playlist.title, "\n", " ") + "\n";
  std::size_t n = 0;
  for (const Entry& entry : playlist.entries) {
    ++n;
    out += std::format("File{}={}\n", n, util::replace_all(entry.location, "\n", " "));
    std::string title = entry.title.empty() ? entry.artist : entry.title;
    if (!title.empty()) out += std::format("Title{}={}\n", n, util::replace_all(title, "\n", " "));
    out += std::format("Length{}={}\n", n, entry.duration ? std::to_string(std::llround(*entry.duration)) : std::string("-1"));
  }
  out += std::format("NumberOfEntries={}\nVersion=2\n", n);
  return out;
}

// ============================================================================ XML playlists

Result<util::XmlDocument> parse_lenient_xml(ByteView input) {
  util::XmlParseOptions options;
  options.replace_invalid_text = true;
  options.keep_unknown_entities = true;
  options.max_depth = 64;
  auto parsed = util::parse_xml(input, options);
  if (!parsed) return fail(parsed.error().code, "the playlist is not well-formed XML: {}", parsed.error().message);
  return parsed;
}

util::XmlNode child_ci(util::XmlNode node, std::string_view name) {
  for (const util::XmlNode child : node.elements()) {
    if (util::iequals(child.local_name(), name)) return child;
  }
  return {};
}
std::string attribute_ci(util::XmlNode node, std::string_view name) {
  for (const util::XmlAttribute attribute : node.attributes()) {
    if (util::iequals(attribute.local_name(), name)) return std::string(attribute.value());
  }
  return {};
}
std::string text_of(util::XmlNode node) { return std::string(util::trim(node.text())); }

Result<Playlist> parse_xspf(ByteView input) {
  CVT_TRY_ASSIGN(const util::XmlDocument doc, parse_lenient_xml(input));
  const util::XmlNode root = doc.root();
  if (root.local_name() != "playlist") return fail(Status::Decode, "not an XSPF playlist");
  Playlist playlist;
  playlist.title = text_of(root.child_local("title"));
  for (const util::XmlNode track : root.child_local("trackList").children_local("track")) {
    Entry entry;
    entry.location = text_of(track.child_local("location"));
    entry.title = text_of(track.child_local("title"));
    entry.artist = text_of(track.child_local("creator"));
    entry.album = text_of(track.child_local("album"));
    entry.track = text_of(track.child_local("trackNum"));
    if (const auto ms = util::parse_double(text_of(track.child_local("duration"))); ms && *ms >= 0) entry.duration = *ms / 1000.0;
    if (entry.location.empty()) entry.location = text_of(track.child_local("identifier"));
    playlist.entries.push_back(std::move(entry));
  }
  return playlist;
}

std::string write_xspf(const Playlist& playlist) {
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  util::XmlNode root = doc.append_child("playlist");
  root.set_attribute("version", "1");
  root.set_attribute("xmlns", "http://xspf.org/ns/0/");
  if (!playlist.title.empty()) root.append_child("title").append_text(playlist.title);
  util::XmlNode list = root.append_child("trackList");
  for (const Entry& entry : playlist.entries) {
    util::XmlNode track = list.append_child("track");
    track.append_child("location").append_text(entry.location);
    if (!entry.title.empty()) track.append_child("title").append_text(entry.title);
    if (!entry.artist.empty()) track.append_child("creator").append_text(entry.artist);
    if (!entry.album.empty()) track.append_child("album").append_text(entry.album);
    if (!entry.track.empty() && util::parse_int(entry.track)) track.append_child("trackNum").append_text(entry.track);
    if (entry.duration) track.append_child("duration").append_text(std::to_string(std::llround(*entry.duration * 1000.0)));
  }
  return util::xml_to_string(doc);
}

Result<Playlist> parse_asx(ByteView input) {
  CVT_TRY_ASSIGN(const util::XmlDocument doc, parse_lenient_xml(input));
  const util::XmlNode root = doc.root();
  if (!util::iequals(root.local_name(), "asx")) return fail(Status::Decode, "not an ASX playlist");
  Playlist playlist;
  playlist.title = text_of(child_ci(root, "title"));
  for (const util::XmlNode item : root.elements()) {
    if (!util::iequals(item.local_name(), "entry")) continue;
    Entry entry;
    entry.location = attribute_ci(child_ci(item, "ref"), "href");
    entry.title = text_of(child_ci(item, "title"));
    entry.artist = text_of(child_ci(item, "author"));
    entry.duration = parse_seconds(attribute_ci(child_ci(item, "duration"), "value"));
    if (!entry.location.empty()) playlist.entries.push_back(std::move(entry));
  }
  return playlist;
}

std::string write_asx(const Playlist& playlist) {
  std::string out = "<asx version=\"3.0\">\n";
  if (!playlist.title.empty()) out += "  <title>" + util::xml_escape(playlist.title) + "</title>\n";
  for (const Entry& entry : playlist.entries) {
    out += "  <entry>\n";
    if (!entry.title.empty()) out += "    <title>" + util::xml_escape(entry.title) + "</title>\n";
    if (!entry.artist.empty()) out += "    <author>" + util::xml_escape(entry.artist) + "</author>\n";
    out += "    <ref href=\"" + util::xml_escape(entry.location, true) + "\"/>\n";
    if (entry.duration) {
      const auto total = static_cast<std::int64_t>(std::llround(*entry.duration * 1000.0));
      out += std::format("    <duration value=\"{:02}:{:02}:{:02}.{:03}\"/>\n", total / 3600000, (total / 60000) % 60,
                         (total / 1000) % 60, total % 1000);
    }
    out += "  </entry>\n";
  }
  out += "</asx>\n";
  return out;
}

Result<Playlist> parse_wpl(ByteView input) {
  CVT_TRY_ASSIGN(const util::XmlDocument doc, parse_lenient_xml(input));
  const util::XmlNode root = doc.root();
  if (!util::iequals(root.local_name(), "smil")) return fail(Status::Decode, "not a WPL playlist");
  Playlist playlist;
  playlist.title = text_of(child_ci(child_ci(root, "head"), "title"));
  for (const util::XmlNode media : child_ci(child_ci(root, "body"), "seq").elements()) {
    if (!util::iequals(media.local_name(), "media")) continue;
    Entry entry;
    entry.location = attribute_ci(media, "src");
    if (!entry.location.empty()) playlist.entries.push_back(std::move(entry));
  }
  return playlist;
}

std::string write_wpl(const Playlist& playlist) {
  std::string out = "<?wpl version=\"1.0\"?>\n<smil>\n  <head>\n";
  out += "    <meta name=\"ItemCount\" content=\"" + std::to_string(playlist.entries.size()) + "\"/>\n";
  out += "    <title>" + util::xml_escape(playlist.title) + "</title>\n  </head>\n  <body>\n    <seq>\n";
  for (const Entry& entry : playlist.entries) out += "      <media src=\"" + util::xml_escape(entry.location, true) + "\"/>\n";
  out += "    </seq>\n  </body>\n</smil>\n";
  return out;
}

// ---------------------------------------------------------------------------- property lists (iTunes)

Json plist_value(util::XmlNode node, int depth) {
  if (depth > 64) return nullptr;
  const std::string_view name = node.local_name();
  if (name == "dict") {
    Json object = Json::object();
    std::string key;
    for (const util::XmlNode child : node.elements()) {
      if (child.local_name() == "key") key = child.text();
      else object[key] = plist_value(child, depth + 1);
    }
    return object;
  }
  if (name == "array") {
    Json array = Json::array();
    for (const util::XmlNode child : node.elements()) array.push_back(plist_value(child, depth + 1));
    return array;
  }
  if (name == "integer") return util::parse_int(util::trim(node.text())).value_or(0);
  if (name == "real") return util::parse_double(util::trim(node.text())).value_or(0.0);
  if (name == "true") return true;
  if (name == "false") return false;
  return node.text();  // string, date, data (Base64 kept as text)
}

std::string decode_file_url(std::string_view url) {
  // file://localhost/Music/a%20b.mp3 -> /Music/a b.mp3 (percent-decoded); others unchanged.
  std::string_view path = url;
  if (util::istarts_with(path, "file://localhost/")) path.remove_prefix(16);
  else if (util::istarts_with(path, "file:///")) path.remove_prefix(7);
  else return std::string(url);
  std::string out;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%' && i + 2 < path.size()) {
      if (const auto value = util::parse_int(path.substr(i + 1, 2), 16)) {
        out.push_back(static_cast<char>(*value));
        i += 2;
        continue;
      }
    }
    out.push_back(path[i]);
  }
  return util::sanitize_utf8(out);
}

Result<Json> parse_itunes(ByteView input) {
  CVT_TRY_ASSIGN(const util::XmlDocument doc, parse_lenient_xml(input));
  const util::XmlNode root = doc.root();
  if (root.local_name() != "plist") return fail(Status::Decode, "not an iTunes library (no <plist>)");
  const Json plist = plist_value(root.child_local("dict"), 0);
  const Json* tracks = plist.find("Tracks");
  if (tracks == nullptr || !tracks->is_object()) return fail(Status::Decode, "not an iTunes library (no Tracks dictionary)");
  Json out = Json::object({{"type", "itunes-library"}});
  Json track_list = Json::array();
  for (const auto& [id, track] : tracks->items()) {
    Json item = Json::object({{"id", util::parse_int(id).value_or(0)}});
    item["location"] = decode_file_url(track["Location"].as_string());
    if (!track["Name"].as_string().empty()) item["title"] = track["Name"].as_string();
    if (!track["Artist"].as_string().empty()) item["artist"] = track["Artist"].as_string();
    if (!track["Album"].as_string().empty()) item["album"] = track["Album"].as_string();
    if (const auto ms = track["Total Time"].get_double()) item["duration_s"] = *ms / 1000.0;
    if (const auto number = track["Track Number"].get_int64()) item["track"] = std::to_string(*number);
    if (!track["Genre"].as_string().empty()) item["group"] = track["Genre"].as_string();
    track_list.push_back(std::move(item));
  }
  out["entries"] = std::move(track_list);
  Json playlists = Json::array();
  for (const Json& playlist : plist["Playlists"].elements()) {
    Json items = Json::array();
    for (const Json& item : playlist["Playlist Items"].elements()) items.push_back(item["Track ID"].as_int64(0));
    playlists.push_back(Json::object({{"name", playlist["Name"].as_string()}, {"track_ids", std::move(items)}}));
  }
  out["playlists"] = std::move(playlists);
  return out;
}

ir::Table itunes_table(const Json& library) {
  // One sheet with every track, then one per playlist.
  ir::Table table = to_table(from_data(library));
  table.sheets.front().name = "Library";
  std::map<std::int64_t, const Json*> by_id;
  for (const Json& track : library["entries"].elements()) by_id[track["id"].as_int64(0)] = &track;
  for (const Json& playlist : library["playlists"].elements()) {
    Json subset = Json::object({{"entries", Json::array()}});
    for (const Json& id : playlist["track_ids"].elements()) {
      if (const auto it = by_id.find(id.as_int64(0)); it != by_id.end()) subset["entries"].push_back(*it->second);
    }
    ir::Table sheet = to_table(from_data(subset));
    sheet.sheets.front().name = playlist["name"].as_string();
    table.sheets.push_back(std::move(sheet.sheets.front()));
  }
  return table;
}

// ============================================================================ CUE sheets

/// Split a CUE line into words, honouring double quotes.
std::vector<std::string> cue_words(std::string_view line) {
  std::vector<std::string> words;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && util::is_ascii_space(line[i])) ++i;
    if (i >= line.size()) break;
    if (line[i] == '"') {
      const std::size_t close = line.find('"', i + 1);
      words.emplace_back(line.substr(i + 1, close == std::string_view::npos ? std::string_view::npos : close - i - 1));
      i = close == std::string_view::npos ? line.size() : close + 1;
    } else {
      const std::size_t start = i;
      while (i < line.size() && !util::is_ascii_space(line[i])) ++i;
      words.emplace_back(line.substr(start, i - start));
    }
  }
  return words;
}

std::optional<double> cue_time(std::string_view text) {
  const std::vector<std::string_view> parts = util::split(text, ':');
  if (parts.size() != 3) return std::nullopt;
  const auto m = util::parse_int(parts[0]);
  const auto s = util::parse_int(parts[1]);
  const auto f = util::parse_int(parts[2]);
  if (!m || !s || !f || *m < 0 || *s < 0 || *s > 59 || *f < 0 || *f > 74) return std::nullopt;
  return static_cast<double>(*m) * 60.0 + static_cast<double>(*s) + static_cast<double>(*f) / 75.0;
}

std::string cue_time_text(double seconds) {
  const auto frames = static_cast<std::int64_t>(std::llround(std::max(seconds, 0.0) * 75.0));
  return std::format("{:02}:{:02}:{:02}", frames / 4500, (frames / 75) % 60, frames % 75);
}

Result<Json> parse_cue(std::string_view text) {
  Json root = Json::object({{"type", "cue"}});
  Json rem = Json::object();
  Json files = Json::array();
  Json* file = nullptr;
  Json* track = nullptr;
  bool any = false;
  for (const std::string_view raw : util::split_lines(text)) {
    const std::vector<std::string> words = cue_words(util::trim(raw));
    if (words.empty()) continue;
    const std::string command = util::to_upper(words[0]);
    const std::string arg = words.size() > 1 ? words[1] : std::string();
    if (command == "REM" && words.size() >= 3) {
      std::vector<std::string> rest(words.begin() + 2, words.end());
      (track != nullptr ? (*track)["rem"] : rem)[util::to_lower(words[1])] = util::join(rest, " ");
    } else if (command == "FILE") {
      files.push_back(Json::object({{"file", arg}, {"type", words.size() > 2 ? words[2] : std::string("WAVE")}, {"tracks", Json::array()}}));
      file = files.at(files.size() - 1);
      track = nullptr;
      any = true;
    } else if (command == "TRACK" && file != nullptr) {
      (*file)["tracks"].push_back(Json::object({{"number", util::parse_int(arg).value_or(0)},
                                                {"type", words.size() > 2 ? words[2] : std::string("AUDIO")}}));
      track = (*file)["tracks"].at((*file)["tracks"].size() - 1);
    } else if (command == "INDEX" && track != nullptr && words.size() >= 3) {
      if (const auto seconds = cue_time(words[2])) (*track)["indexes"][arg] = *seconds;
    } else if (command == "TITLE" || command == "PERFORMER" || command == "SONGWRITER" || command == "ISRC" ||
               command == "CATALOG" || command == "CDTEXTFILE") {
      (track != nullptr ? *track : root)[util::to_lower(command)] = arg;
    } else if (command == "PREGAP" || command == "POSTGAP") {
      if (track != nullptr) {
        if (const auto seconds = cue_time(arg)) (*track)[util::to_lower(command)] = *seconds;
      }
    } else if (command == "FLAGS" && track != nullptr) {
      std::vector<std::string> rest(words.begin() + 1, words.end());
      (*track)["flags"] = util::join(rest, " ");
    }
  }
  if (!any) return fail(Status::Decode, "not a CUE sheet (no FILE command)");
  if (!rem.empty()) root["rem"] = std::move(rem);
  root["files"] = std::move(files);
  return root;
}

Playlist cue_playlist(const Json& cue) {
  Playlist playlist;
  playlist.title = cue["title"].as_string();
  for (const Json& file : cue["files"].elements()) {
    const auto& tracks = file["tracks"].elements();
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      const Json& track = tracks[i];
      Entry entry;
      entry.location = file["file"].as_string();
      entry.title = track["title"].as_string();
      entry.artist = track["performer"].as_string().empty() ? cue["performer"].as_string() : track["performer"].as_string();
      entry.album = cue["title"].as_string();
      entry.track = std::format("{:02}", track["number"].as_int64(0));
      const double start = track["indexes"]["01"].as_double(0.0);
      entry.attributes.emplace_back("start_s", number_text(start));
      if (i + 1 < tracks.size()) {
        const double next = tracks[i + 1]["indexes"]["01"].as_double(start);
        if (next > start) entry.duration = next - start;
      }
      playlist.entries.push_back(std::move(entry));
    }
  }
  return playlist;
}

std::string quote_cue(std::string_view value) {
  std::string out(value);
  std::ranges::replace(out, '"', '\'');
  std::ranges::replace(out, '\n', ' ');
  return "\"" + out + "\"";
}

std::string write_cue_from_data(const Json& cue) {
  std::string out;
  for (const auto& [key, value] : cue["rem"].items()) out += "REM " + util::to_upper(key) + " " + json_text(value) + "\n";
  for (const std::string_view key : {"catalog"sv, "cdtextfile"sv}) {
    if (!cue[key].as_string().empty()) out += util::to_upper(key) + " " + quote_cue(cue[key].as_string()) + "\n";
  }
  if (!cue["performer"].as_string().empty()) out += "PERFORMER " + quote_cue(cue["performer"].as_string()) + "\n";
  if (!cue["title"].as_string().empty()) out += "TITLE " + quote_cue(cue["title"].as_string()) + "\n";
  for (const Json& file : cue["files"].elements()) {
    out += "FILE " + quote_cue(file["file"].as_string()) + " " +
           (file["type"].as_string().empty() ? std::string("WAVE") : file["type"].as_string()) + "\n";
    for (const Json& track : file["tracks"].elements()) {
      out += std::format("  TRACK {:02} {}\n", track["number"].as_int64(0),
                         track["type"].as_string().empty() ? std::string("AUDIO") : track["type"].as_string());
      for (const std::string_view key : {"title"sv, "performer"sv, "songwriter"sv}) {
        if (!track[key].as_string().empty()) out += "    " + util::to_upper(key) + " " + quote_cue(track[key].as_string()) + "\n";
      }
      if (!track["isrc"].as_string().empty()) out += "    ISRC " + track["isrc"].as_string() + "\n";
      if (!track["flags"].as_string().empty()) out += "    FLAGS " + track["flags"].as_string() + "\n";
      for (const auto& [key, value] : track["rem"].items()) out += "    REM " + util::to_upper(key) + " " + json_text(value) + "\n";
      if (const auto pregap = track["pregap"].get_double()) out += "    PREGAP " + cue_time_text(*pregap) + "\n";
      for (const auto& [index, seconds] : track["indexes"].items()) {
        out += "    INDEX " + index + " " + cue_time_text(seconds.as_double()) + "\n";
      }
      if (const auto postgap = track["postgap"].get_double()) out += "    POSTGAP " + cue_time_text(*postgap) + "\n";
    }
  }
  return out;
}

/// A CUE sheet from playlist rows: tracks of one file share its FILE command;
/// start times come from the "start_s" attribute or accumulate durations.
Json cue_from_playlist(const Playlist& playlist) {
  Json root = Json::object({{"type", "cue"}});
  if (!playlist.title.empty()) root["title"] = playlist.title;
  Json files = Json::array();
  std::string current;
  double position = 0.0;
  std::int64_t number = 0;
  for (const Entry& entry : playlist.entries) {
    if (files.empty() || entry.location != current) {
      files.push_back(Json::object({{"file", entry.location}, {"type", "WAVE"}, {"tracks", Json::array()}}));
      current = entry.location;
      position = 0.0;
    }
    double start = position;
    for (const auto& [key, value] : entry.attributes) {
      if (key == "start_s") start = util::parse_double(value).value_or(position);
    }
    Json track = Json::object({{"number", ++number}, {"type", "AUDIO"}});
    if (!entry.title.empty()) track["title"] = entry.title;
    if (!entry.artist.empty()) track["performer"] = entry.artist;
    track["indexes"] = Json::object({{"01", start}});
    files[files.size() - 1]["tracks"].push_back(std::move(track));
    position = start + entry.duration.value_or(0.0);
  }
  root["files"] = std::move(files);
  return root;
}

// ============================================================================ registration

struct Format {
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> extensions;
  std::string mime;
  int popularity;
  std::function<int(ByteView, std::string_view)> sniff;
};

/// The canonical Data value of a file of `format` (Playlist shape, or the
/// HLS / CUE / iTunes shapes).
Result<Json> read_data(std::string_view format, ByteView input, const ReadContext& ctx) {
  if (format == "xspf") {
    CVT_TRY_ASSIGN(const Playlist playlist, parse_xspf(input));
    return to_data(playlist);
  }
  if (format == "asx") {
    CVT_TRY_ASSIGN(const Playlist playlist, parse_asx(input));
    return to_data(playlist);
  }
  if (format == "wpl") {
    CVT_TRY_ASSIGN(const Playlist playlist, parse_wpl(input));
    return to_data(playlist);
  }
  if (format == "itunes-xml") return parse_itunes(input);
  CVT_TRY_ASSIGN(const std::string text, decode_text(input, ctx.options.get_string("source_encoding", "auto")));
  if (format == "cue") return parse_cue(text);
  if (format == "pls") {
    const Playlist playlist = parse_pls(text);
    if (playlist.entries.empty() && util::trim(text).size() > 0 && text.find("[playlist]") == std::string::npos &&
        text.find("[Playlist]") == std::string::npos) {
      return fail(Status::Decode, "not a PLS playlist (no [playlist] section)");
    }
    return to_data(playlist);
  }
  // m3u / m3u8
  if (is_hls(text)) return parse_hls(text);
  const Playlist playlist = parse_m3u(text);
  if (playlist.entries.empty() && !util::trim(text).empty() && !util::trim(text).starts_with("#")) {
    return fail(Status::Decode, "not an M3U playlist");
  }
  return to_data(playlist);
}

Playlist playlist_of(const Json& data) {
  const std::string& type = data["type"].as_string();
  if (type == "hls-media" || type == "hls-master") return hls_playlist(data);
  if (type == "cue") return cue_playlist(data);
  return from_data(data);
}

Result<std::string> write_playlist(std::string_view format, const Playlist& playlist, const Json* data) {
  if (format == "pls") return write_pls(playlist);
  if (format == "xspf") return write_xspf(playlist);
  if (format == "asx") return write_asx(playlist);
  if (format == "wpl") return write_wpl(playlist);
  if (format == "cue") return write_cue_from_data(data != nullptr && (*data)["type"].as_string() == "cue" ? *data : cue_from_playlist(playlist));
  if (data != nullptr) {
    const std::string& type = (*data)["type"].as_string();
    if (type == "hls-media" || type == "hls-master") return write_hls(*data);
  }
  return write_m3u(playlist);
}

void register_playlist(Registry& registry, const Format& format, bool writable) {
  FormatInfo info;
  info.id = format.id;
  info.name = format.name;
  info.description = format.description;
  info.category = Category::Data;
  info.extensions = format.extensions;
  info.mime = format.mime;
  info.popularity = format.popularity;
  info.sniff = format.sniff;
  registry.add_format(std::move(info));

  const std::string id = format.id;
  Codec data;
  data.format_id = id;
  data.kind = Kind::Data;
  data.read = [id](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(Json value, read_data(id, ctx.input, ctx));
    return ir::Data{std::move(value)};
  };
  data.read_options = {source_encoding_option()};
  if (writable) {
    data.write = [id](const AnyIR& value, WriteContext&) -> Result<Bytes> {
      const Json& root = value.get<ir::Data>().value;
      CVT_TRY_ASSIGN(std::string text, write_playlist(id, playlist_of(root), &root));
      return to_bytes(text);
    };
  }
  registry.add_codec(std::move(data));

  Codec table;
  table.format_id = id;
  table.kind = Kind::Table;
  table.read = [id](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(const Json value, read_data(id, ctx.input, ctx));
    if (id == "itunes-xml") return itunes_table(value);
    return to_table(playlist_of(value));
  };
  table.read_options = {source_encoding_option()};
  table.read_cost = 12;
  if (writable) {
    table.write = [id](const AnyIR& value, WriteContext&) -> Result<Bytes> {
      CVT_TRY_ASSIGN(const Playlist playlist, from_table(value.get<ir::Table>()));
      CVT_TRY_ASSIGN(std::string text, write_playlist(id, playlist, nullptr));
      return to_bytes(text);
    };
  }
  registry.add_codec(std::move(table));
}

std::string head_of(ByteView data) { return sniff_text(data, 2048); }

int sniff_m3u(ByteView data, bool extended_only) {
  const std::string head = head_of(data);
  if (head.starts_with("#EXTM3U")) return extended_only ? 90 : 88;
  return 0;
}

}  // namespace

void register_playlists(Registry& registry) {
  register_playlist(registry,
                    {"m3u", "M3U playlist", "Media playlist (extended M3U with #EXTINF).", {"m3u"}, "audio/x-mpegurl", 50,
                     [](ByteView data, std::string_view) { return sniff_m3u(data, false); }},
                    true);
  register_playlist(registry,
                    {"m3u8", "M3U8 playlist", "UTF-8 M3U playlist, including HLS media and master playlists.", {"m3u8"},
                     "application/vnd.apple.mpegurl", 50,
                     [](ByteView data, std::string_view) {
                       const std::string head = head_of(data);
                       if (!head.starts_with("#EXTM3U")) return 0;
                       return is_hls(head) ? 92 : 86;
                     }},
                    true);
  register_playlist(registry,
                    {"pls", "PLS playlist", "Winamp / Shoutcast playlist.", {"pls"}, "audio/x-scpls", 30,
                     [](ByteView data, std::string_view) {
                       return util::istarts_with(util::trim(head_of(data)), "[playlist]") ? 92 : 0;
                     }},
                    true);
  register_playlist(registry,
                    {"xspf", "XSPF playlist", "XML Shareable Playlist Format.", {"xspf"}, "application/xspf+xml", 25,
                     [](ByteView data, std::string_view) {
                       const std::string head = head_of(data);
                       return head.find("http://xspf.org/ns/0/") != std::string::npos ? 95 : 0;
                     }},
                    true);
  register_playlist(registry,
                    {"asx", "ASX playlist", "Windows Media advanced stream redirector.", {"asx", "wax", "wvx"},
                     "video/x-ms-asf", 15,
                     [](ByteView data, std::string_view) {
                       const std::string head = util::to_lower(util::trim(head_of(data)));
                       return head.starts_with("<asx") ? 92 : 0;
                     }},
                    true);
  register_playlist(registry,
                    {"wpl", "Windows Media playlist", "Windows Media Player playlist (SMIL).", {"wpl"},
                     "application/vnd.ms-wpl", 15,
                     [](ByteView data, std::string_view) {
                       const std::string head = head_of(data);
                       return head.starts_with("<?wpl") ? 95 : 0;
                     }},
                    true);
  register_playlist(registry,
                    {"itunes-xml", "iTunes library", "iTunes / Music library export (property list XML).", {"xml"},
                     "application/xml", 15,
                     [](ByteView data, std::string_view) {
                       const std::string head = head_of(data);
                       return head.find("<plist") != std::string::npos && head.find("<key>Tracks</key>") != std::string::npos &&
                                      head.find("<key>Music Folder</key>") != std::string::npos
                                  ? 93
                              : head.find("<plist") != std::string::npos && head.find("<key>Tracks</key>") != std::string::npos
                                  ? 85
                                  : 0;
                     }},
                    false);
  register_playlist(registry,
                    {"cue", "CUE sheet", "Track layout of a CD image or album file.", {"cue"}, "application/x-cue", 30,
                     [](ByteView data, std::string_view) {
                       const std::string head = head_of(data);
                       for (const std::string_view raw : util::split_lines(head)) {
                         const std::string line = util::to_upper(util::trim(raw));
                         if (line.empty() || line.starts_with("REM ")) continue;
                         return line.starts_with("FILE ") || line.starts_with("PERFORMER ") || line.starts_with("TITLE ") ||
                                        line.starts_with("CATALOG ")
                                    ? 85
                                    : 0;
                       }
                       return 0;
                     }},
                    true);
}

}  // namespace cvt::pim
