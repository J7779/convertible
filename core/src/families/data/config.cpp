// Convertible core - data family: line-oriented configuration formats.
//
//   ini         [sections] with key = value / key: value lines; ';' and '#'
//               comments (inline ones after white space, option), quoted
//               values, indented continuation lines, key[] arrays, keys
//               without a value (null), optional nested [a.b] sections and
//               type detection. Keys before the first section stay at the top.
//   properties  Java .properties: '=', ':' or white space separators, '#'/'!'
//               comments, backslash escapes including \uXXXX, line
//               continuations; read as UTF-8 or (failing that) ISO-8859-1.
//               The writer escapes non-ASCII as \uXXXX by default.
//   env         dotenv files: KEY=value, `export`, quoted values with escapes
//               and line breaks, comments.
//   reg         Windows Registry Editor files (version 5.00 and REGEDIT4):
//               {"HKEY_...\\Path": {"Name": value, "@": default value}}; REG_SZ
//               -> string, REG_DWORD -> integer, REG_QWORD -> integer (or a
//               typed object when it would fit a DWORD), REG_BINARY -> $binary,
//               REG_MULTI_SZ -> array of strings, REG_EXPAND_SZ and other types
//               -> {"type": "REG_EXPAND_SZ", "value": ...}; deletions -> null.
// Nested data written to the flat formats is flattened with dotted keys.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/data.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace {

using namespace std::string_view_literals;

/// Lines with their starting offsets (LF, CRLF and CR all end a line).
struct Line {
  std::string_view text;
  std::size_t offset = 0;
};

std::vector<Line> split_lines_with_offsets(std::string_view text) {
  std::vector<Line> lines;
  std::size_t pos = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
  while (pos <= text.size()) {
    std::size_t end = text.find_first_of("\r\n", pos);
    if (end == std::string_view::npos) end = text.size();
    lines.push_back({text.substr(pos, end - pos), pos});
    if (end >= text.size()) break;
    pos = end + (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n' ? 2 : 1);
  }
  return lines;
}

/// Values written by the flat formats: strings as they are, others as JSON-ish text.
std::string scalar_string(const Json& value) {
  switch (tag_kind(value)) {
    case TagKind::None: break;
    case TagKind::BigInt:
    case TagKind::Decimal:
    case TagKind::Date: return std::string(tag_text(value));
    default: {
      const Json plain = plain_value(value);
      if (plain.is_string()) return plain.as_string();
      if (!plain.is_structured()) return scalar_string(plain);
      return util::dump_json(plain);
    }
  }
  switch (value.type()) {
    case Json::Type::Null: return {};
    case Json::Type::Bool: return value.as_bool() ? "true" : "false";
    case Json::Type::Int64:
    case Json::Type::UInt64: return format_integer(value);
    case Json::Type::Double: return format_double(value.as_double());
    case Json::Type::String: return value.as_string();
    default: {
      auto text = write_json_text(value, JsonStyle{.indent = -1});
      return text ? std::move(*text) : std::string();
    }
  }
}

bool is_leaf(const Json& value) { return !value.is_structured() || tag_kind(value) != TagKind::None; }

/// Flatten nested objects/arrays into dotted paths ("a.b", "list.0").
void flatten_into(const Json& value, std::string& path, std::string_view separator,
                  std::vector<std::pair<std::string, const Json*>>& out) {
  if (is_leaf(value) || value.empty()) {
    out.emplace_back(path, &value);
    return;
  }
  const std::size_t length = path.size();
  const auto child = [&](std::string_view key, const Json& element) {
    if (!path.empty()) path += separator;
    path += key;
    flatten_into(element, path, separator, out);
    path.resize(length);
  };
  if (value.is_array()) {
    for (std::size_t i = 0; i < value.size(); ++i) child(std::to_string(i), value[i]);
  } else {
    for (const auto& member : value.items()) child(member.key(), member.value());
  }
}

/// "true"/"false", JSON numbers -> typed values (used by `infer_types`).
Json infer(std::string text) {
  if (text == "true" || text == "True" || text == "TRUE") return Json(true);
  if (text == "false" || text == "False" || text == "FALSE") return Json(false);
  std::size_t i = 0;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) ++i;
  const std::size_t digits = i;
  while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
  if (i == digits || (text[digits] == '0' && i - digits > 1 && (i == text.size() || text[i] != '.'))) {
    return Json(std::move(text));
  }
  if (i < text.size() && text[i] == '.') {
    const std::size_t frac = ++i;
    while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
    if (i == frac) return Json(std::move(text));
  }
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    const std::size_t exp = i;
    while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
    if (i == exp) return Json(std::move(text));
  }
  if (i != text.size()) return Json(std::move(text));
  return number_from_literal(text);
}

// =========================================================================== INI

struct IniSettings {
  bool inline_comments = true;
  bool unquote = true;
  bool nested = false;
  bool infer_types = false;
  bool multiline = true;
  bool array_keys = true;
};

std::string unquote_value(std::string_view value, const IniSettings& settings) {
  if (!settings.unquote || value.size() < 2) return std::string(value);
  const char q = value.front();
  if ((q != '"' && q != '\'') || value.back() != q) return std::string(value);
  const std::string_view body = value.substr(1, value.size() - 2);
  if (q == '\'') return std::string(body);
  std::string out;
  for (std::size_t i = 0; i < body.size(); ++i) {
    if (body[i] == '\\' && i + 1 < body.size()) {
      const char e = body[++i];
      switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case '0': out.push_back('\0'); break;
        default: out.push_back(e);
      }
    } else {
      out.push_back(body[i]);
    }
  }
  return out;
}

std::string_view strip_inline_comment(std::string_view value, const IniSettings& settings) {
  if (!settings.inline_comments) return value;
  char quote = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (quote != 0) {
      if (c == '\\' && quote == '"') {
        ++i;
      } else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if ((c == '"' || c == '\'') && util::trim(value.substr(0, i)).empty()) {
      quote = c;
      continue;
    }
    // ';' after white space starts a comment; '#' does not, so that values
    // such as colours (#fff) survive ('#' comments are whole lines).
    if (c == ';' && i > 0 && (value[i - 1] == ' ' || value[i - 1] == '\t')) return value.substr(0, i);
  }
  return value;
}

Result<Json> parse_ini(std::string_view text, ReadContext& ctx) {
  IniSettings settings;
  settings.inline_comments = ctx.options.get_bool("inline_comments", true);
  settings.unquote = ctx.options.get_bool("unquote", true);
  settings.nested = ctx.options.get_bool("nested_sections", false);
  settings.infer_types = ctx.options.get_bool("infer_types", false);
  settings.multiline = ctx.options.get_bool("multiline_values", true);
  const ReadPolicy policy = read_policy(ctx.options);
  Json root = Json::object();
  std::vector<std::string> section;  // path of the current section
  std::string last_key;
  bool have_last = false;
  const auto table = [&]() -> Json* {
    Json* node = &root;
    for (const std::string& part : section) {
      Json* child = node->find(part);
      if (child == nullptr || !child->is_object()) {
        node->set(part, Json::object());
        child = node->find(part);
      }
      node = child;
    }
    return node;
  };
  const auto value_of = [&](std::string raw) { return settings.infer_types ? infer(std::move(raw)) : Json(std::move(raw)); };
  std::size_t count = 0;
  for (const Line& line : split_lines_with_offsets(text)) {
    if ((++count & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(line.offset) / static_cast<double>(text.size())));
    const std::string_view trimmed = util::trim(line.text);
    if (trimmed.empty()) {
      have_last = false;
      continue;
    }
    if (trimmed.front() == ';' || trimmed.front() == '#') continue;
    const bool indented = line.text.front() == ' ' || line.text.front() == '\t';
    if (trimmed.front() == '[') {
      const std::string_view without_comment = util::trim(strip_inline_comment(trimmed, settings));
      if (!without_comment.ends_with(']')) return syntax_error("INI", text, line.offset, "a section header must end with ']'");
      const std::string name(util::trim(without_comment.substr(1, without_comment.size() - 2)));
      section.clear();
      if (settings.nested) {
        for (const std::string_view part : util::split(name, '.')) section.emplace_back(util::trim(part));
      } else {
        section.push_back(name);
      }
      if (section.size() > policy.max_depth) return fail(Status::Limit, "the INI section name is nested too deeply");
      Json* node = &root;
      for (const std::string& part : section) {
        Json* child = node->find(part);
        if (child == nullptr) {
          node->set(part, Json::object());
          child = node->find(part);
        } else if (!child->is_object()) {
          return syntax_error("INI", text, line.offset, std::format("section [{}] clashes with a key of the same name", name));
        }
        node = child;
      }
      have_last = false;
      continue;
    }
    const std::size_t separator = trimmed.find_first_of("=:");
    if (indented && have_last && settings.multiline && separator == std::string_view::npos) {
      // Continuation of the previous value.
      Json* node = table()->find(last_key);
      if (node != nullptr && node->is_string()) {
        std::string joined = node->as_string();
        joined.push_back('\n');
        joined += util::trim(strip_inline_comment(trimmed, settings));
        *node = Json(std::move(joined));
      }
      continue;
    }
    std::string key;
    Json value;
    if (separator == std::string_view::npos) {
      key = std::string(util::trim(strip_inline_comment(trimmed, settings)));
      value = Json();
    } else {
      key = std::string(util::trim(trimmed.substr(0, separator)));
      const std::string_view raw = util::trim(strip_inline_comment(trimmed.substr(separator + 1), settings));
      const bool quoted = raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'') && raw.back() == raw.front();
      std::string unquoted = unquote_value(raw, settings);
      value = quoted ? Json(std::move(unquoted)) : value_of(std::move(unquoted));
    }
    if (key.empty()) return syntax_error("INI", text, line.offset, "a key is missing before the separator");
    Json* node = table();
    if (settings.array_keys && key.size() > 2 && key.ends_with("[]")) {
      key.resize(key.size() - 2);
      Json* existing = node->find(key);
      if (existing == nullptr || !existing->is_array()) {
        node->set(key, Json::array());
        existing = node->find(key);
      }
      existing->push_back(std::move(value));
      have_last = false;
      continue;
    }
    if (auto put = put_member(*node, key, std::move(value), policy); !put) {
      return syntax_error("INI", text, line.offset, put.error().message);
    }
    last_key = std::move(key);
    have_last = true;
  }
  return root;
}

std::string ini_value(const Json& value) {
  std::string text = scalar_string(value);
  const bool needs_quotes = text.empty() ? false
                            : text.front() == ' ' || text.back() == ' ' || text.front() == '"' || text.front() == '\'' ||
                                  text.find_first_of(";#\n\r\t") != std::string::npos;
  if (!needs_quotes) return text;
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

std::string ini_key(std::string_view key) {
  std::string out(key);
  for (char& c : out) {
    if (c == '=' || c == ':' || c == '\n' || c == '\r' || c == '[' || c == ']') c = '_';
  }
  if (!out.empty() && (out.front() == ';' || out.front() == '#')) out.insert(0, "_");
  return out;
}

Result<std::string> write_ini(const Json& value, WriteContext& ctx) {
  const std::string separator = ctx.options.get_enum("separator", " = ", {" = ", "=", ": "});
  const std::string arrays = ctx.options.get_enum("array_style", "repeat", {"repeat", "json", "indexed"});
  const std::string nulls = ctx.options.get_enum("null_values", "empty", {"empty", "bare_key", "omit"});
  const bool sort = ctx.options.get_bool("sort_keys", false);
  const std::string_view newline = ctx.options.get_enum("line_ending", "lf", {"lf", "crlf"}) == "crlf" ? "\r\n"sv : "\n"sv;
  std::string out;
  const auto pair_raw = [&](std::string_view key, const Json& v) {
    if (v.is_null()) {
      if (nulls == "omit") return;
      out += key;
      if (nulls == "empty") out += separator;
      out += newline;
      return;
    }
    out += key;
    out += separator;
    out += ini_value(v);
    out += newline;
  };
  const auto pair = [&](std::string_view key, const Json& v) { pair_raw(ini_key(key), v); };
  // Entries of one section: scalars, arrays per style, nested objects flattened.
  const auto entries = [&](const Json& object, bool top) {
    for (const Json::Member* member : members_of(object, sort)) {
      const Json& v = member->value();
      if (top && v.is_object() && tag_kind(v) == TagKind::None) continue;  // a section
      if (v.is_array() && tag_kind(v) == TagKind::None && arrays != "json" &&
          std::ranges::all_of(v.elements(), [](const Json& e) { return is_leaf(e); })) {
        for (std::size_t i = 0; i < v.size(); ++i) {
          if (arrays == "repeat") {
            pair_raw(ini_key(member->key()) + "[]", v[i]);
          } else {
            pair(std::format("{}.{}", member->key(), i), v[i]);
          }
        }
        continue;
      }
      if (is_leaf(v) || arrays == "json" || v.empty()) {
        pair(member->key(), v);
        continue;
      }
      std::vector<std::pair<std::string, const Json*>> flat;
      std::string path = member->key();
      flatten_into(v, path, ".", flat);
      for (const auto& [key, leaf] : flat) pair(key, *leaf);
    }
  };
  if (!value.is_object() || tag_kind(value) != TagKind::None) {
    ctx.warn("INI files hold sections of keys; the data was written as a single key \"value\".");
    pair("value", value);
    return out;
  }
  entries(value, true);
  for (const Json::Member* member : members_of(value, sort)) {
    const Json& v = member->value();
    if (!v.is_object() || tag_kind(v) != TagKind::None) continue;
    if (!out.empty()) out += newline;
    out += "[" + ini_key(member->key()) + "]";
    out += newline;
    entries(v, false);
  }
  return out;
}

int sniff_ini(ByteView data, std::string_view filename) {
  const bool ext = has_extension(filename, {"ini", "cfg", "conf", "inf", "cnf", "desktop", "gitconfig", "editorconfig"});
  const std::string_view head = text_head(data, 2048);
  if (head.empty()) return ext ? 60 : 0;
  if (head.starts_with('[') && head.find(']') != std::string_view::npos && head.find('\n') > head.find(']')) {
    if (head.starts_with("[[")) return ext ? 60 : 0;
    // "[section]" followed by "key = value" or "key=value".
    const std::size_t nl = head.find('\n');
    const std::string_view next = nl == std::string_view::npos ? std::string_view() : util::trim_left(head.substr(nl + 1));
    const std::size_t eq = next.find('=');
    const std::size_t end = next.find('\n');
    if (eq != std::string_view::npos && eq < end) {
      // `key = "text"` is how TOML writes strings; INI rarely quotes. Without
      // a name such a file is left to the TOML and text sniffers.
      if (!ext && next.substr(eq).starts_with("= \"")) return 0;
      return ext ? 80 : 64;
    }
    return ext ? 75 : 30;
  }
  return ext ? 70 : 0;
}

// =========================================================================== Java properties

Result<Json> parse_properties(std::string_view text, ReadContext& ctx) {
  const ReadPolicy policy = read_policy(ctx.options);
  const bool nested = ctx.options.get_bool("nested_keys", false);
  Json root = Json::object();
  const std::vector<Line> lines = split_lines_with_offsets(text);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    std::string_view line = util::trim_left(lines[index].text);
    const std::size_t offset = lines[index].offset;
    if (line.empty() || line.front() == '#' || line.front() == '!') continue;
    // Join continuation lines: an odd number of trailing backslashes.
    std::string logical;
    while (true) {
      std::size_t backslashes = 0;
      while (backslashes < line.size() && line[line.size() - 1 - backslashes] == '\\') ++backslashes;
      if (backslashes % 2 == 1 && index + 1 < lines.size()) {
        logical.append(line.substr(0, line.size() - 1));
        line = util::trim_left(lines[++index].text);
        continue;
      }
      if (backslashes % 2 == 1) line.remove_suffix(1);
      logical.append(line);
      break;
    }
    // Key up to the first unescaped separator.
    std::string key;
    std::size_t i = 0;
    const auto unescape = [&](std::string& out, bool in_key) -> Result<bool> {
      const char c = logical[i];
      if (c != '\\') {
        if (in_key && (c == '=' || c == ':' || c == ' ' || c == '\t' || c == '\f')) return false;
        out.push_back(c);
        ++i;
        return true;
      }
      if (i + 1 >= logical.size()) {
        ++i;
        return true;
      }
      const char e = logical[i + 1];
      i += 2;
      switch (e) {
        case 't': out.push_back('\t'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
          if (i + 4 > logical.size()) return syntax_error("properties", text, offset, "incomplete \\u escape");
          const auto unit = util::parse_int(std::string_view(logical).substr(i, 4), 16);
          if (!unit || std::string_view(logical).substr(i, 4).find_first_of("+-") != std::string_view::npos) {
            return syntax_error("properties", text, offset, "malformed \\uXXXX escape");
          }
          i += 4;
          auto cp = static_cast<char32_t>(*unit);
          if (cp >= 0xD800 && cp <= 0xDBFF && std::string_view(logical).substr(i, 2) == "\\u") {
            const auto low = util::parse_int(std::string_view(logical).substr(i + 2, 4), 16);
            if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + static_cast<char32_t>(*low - 0xDC00);
              i += 6;
            }
          }
          util::append_utf8(out, util::is_scalar_value(cp) ? cp : util::replacement_character);
          break;
        }
        default: out.push_back(e);
      }
      return true;
    };
    while (i < logical.size()) {
      CVT_TRY_ASSIGN(const bool more, unescape(key, true));
      if (!more) break;
    }
    while (i < logical.size() && (logical[i] == ' ' || logical[i] == '\t' || logical[i] == '\f')) ++i;
    if (i < logical.size() && (logical[i] == '=' || logical[i] == ':')) ++i;
    while (i < logical.size() && (logical[i] == ' ' || logical[i] == '\t' || logical[i] == '\f')) ++i;
    std::string value;
    while (i < logical.size()) CVT_TRY(unescape(value, false));
    if (!nested) {
      CVT_TRY(put_member(root, std::move(key), Json(std::move(value)), policy));
      continue;
    }
    Json* node = &root;
    const std::vector<std::string_view> parts = util::split(key, '.');
    if (parts.size() > policy.max_depth) return fail(Status::Limit, "a properties key has too many parts");
    for (std::size_t p = 0; p + 1 < parts.size(); ++p) {
      Json* child = node->find(parts[p]);
      if (child == nullptr || !child->is_object()) {
        if (child != nullptr) {
          // "a=1" and "a.b=2": keep the scalar under "" so nothing is lost.
          Json scalar = std::move(*child);
          *child = Json::object();
          child->set("", std::move(scalar));
        } else {
          node->set(parts[p], Json::object());
          child = node->find(parts[p]);
        }
      }
      node = child;
    }
    const std::string last(parts.back());
    if (Json* existing = node->find(last); existing != nullptr && existing->is_object()) {
      existing->set("", Json(std::move(value)));
    } else {
      CVT_TRY(put_member(*node, last, Json(std::move(value)), policy));
    }
  }
  return root;
}

void properties_escape(std::string& out, std::string_view text, bool key, bool ascii) {
  std::size_t i = 0;
  bool leading = true;
  while (i < text.size()) {
    const util::Utf8Decoded d = util::decode_utf8(text, i);
    const char32_t c = d.ok ? d.code_point : util::replacement_character;
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\f': out += "\\f"; break;
      case '=':
      case ':':
      case '#':
      case '!':
        if (key || leading) out.push_back('\\');
        out.push_back(static_cast<char>(c));
        break;
      case ' ':
        if (key || leading) out.push_back('\\');
        out.push_back(' ');
        break;
      default:
        if (c < 0x20 || c == 0x7F || (ascii && c > 0x7E)) {
          if (c > 0xFFFF) {
            const auto v = static_cast<std::uint32_t>(c) - 0x10000;
            out += std::format("\\u{:04X}\\u{:04X}", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
          } else {
            out += std::format("\\u{:04X}", static_cast<unsigned>(c));
          }
        } else {
          util::append_utf8(out, c);
        }
    }
    leading = false;
    i += d.length;
  }
}

Result<std::string> write_properties(const Json& value, WriteContext& ctx) {
  const bool ascii = ctx.options.get_bool("ascii_only", true);
  const std::string separator = ctx.options.get_enum("separator", "=", {"=", " = ", ": "});
  std::vector<std::pair<std::string, const Json*>> flat;
  std::string path;
  flatten_into(value, path, ".", flat);
  if (ctx.options.get_bool("sort_keys", false)) {
    std::ranges::stable_sort(flat, [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  std::string out;
  for (const auto& [key, leaf] : flat) {
    properties_escape(out, key.empty() ? "value" : key, true, ascii);
    out += separator;
    properties_escape(out, scalar_string(*leaf), false, ascii);
    out.push_back('\n');
  }
  return out;
}

int sniff_properties(ByteView /*data*/, std::string_view filename) {
  return has_extension(filename, {"properties"}) ? 75 : 0;
}

// =========================================================================== dotenv

Result<Json> parse_env(std::string_view text, ReadContext& ctx) {
  const ReadPolicy policy = read_policy(ctx.options);
  const bool expand = ctx.options.get_bool("expand_variables", false);
  Json root = Json::object();
  std::size_t p = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
  const auto skip_blank = [&] {
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
  };
  const auto skip_line = [&] {
    while (p < text.size() && text[p] != '\n') ++p;
    if (p < text.size()) ++p;
  };
  const auto expand_vars = [&](std::string value) {
    if (!expand) return value;
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '$' && i + 1 < value.size() && (value[i + 1] == '{' || util::is_ascii_alpha(value[i + 1]) || value[i + 1] == '_')) {
        std::size_t start = i + 1;
        std::size_t end = start;
        std::string name;
        if (value[start] == '{') {
          end = value.find('}', start);
          if (end == std::string::npos) {
            out.push_back(value[i]);
            continue;
          }
          name = value.substr(start + 1, end - start - 1);
          if (const std::size_t dash = name.find(":-"); dash != std::string::npos) name.resize(dash);
          ++end;
        } else {
          while (end < value.size() && (util::is_ascii_alpha(value[end]) || util::is_ascii_digit(value[end]) || value[end] == '_')) ++end;
          name = value.substr(start, end - start);
        }
        out += root[name].as_string();
        i = end - 1;
      } else {
        out.push_back(value[i]);
      }
    }
    return out;
  };
  while (p < text.size()) {
    skip_blank();
    if (p >= text.size()) break;
    if (text[p] == '\n' || text[p] == '\r' || text[p] == '#') {
      skip_line();
      continue;
    }
    const std::size_t line_at = p;
    if (text.substr(p, 7) == "export " || text.substr(p, 7) == "export\t") {
      p += 7;
      skip_blank();
    }
    const std::size_t key_start = p;
    while (p < text.size() && (util::is_ascii_alpha(text[p]) || util::is_ascii_digit(text[p]) || text[p] == '_' ||
                               text[p] == '.' || text[p] == '-')) {
      ++p;
    }
    std::string key(text.substr(key_start, p - key_start));
    skip_blank();
    if (key.empty() || p >= text.size() || text[p] != '=') {
      return syntax_error(".env", text, line_at, "expected KEY=value");
    }
    ++p;
    skip_blank();
    std::string value;
    if (p < text.size() && (text[p] == '"' || text[p] == '\'' || text[p] == '`')) {
      const char quote = text[p];
      const std::size_t start = p++;
      bool closed = false;
      while (p < text.size()) {
        const char c = text[p];
        if (c == quote) {
          ++p;
          closed = true;
          break;
        }
        if (c == '\\' && quote == '"' && p + 1 < text.size()) {
          const char e = text[p + 1];
          p += 2;
          switch (e) {
            case 'n': value.push_back('\n'); break;
            case 't': value.push_back('\t'); break;
            case 'r': value.push_back('\r'); break;
            default: value.push_back(e);
          }
          continue;
        }
        if (c == '\r' && p + 1 < text.size() && text[p + 1] == '\n') {
          ++p;
          continue;
        }
        value.push_back(c);
        ++p;
      }
      if (!closed) return syntax_error(".env", text, start, "unterminated quoted value");
      if (quote == '"') value = expand_vars(std::move(value));
      skip_blank();
      if (p < text.size() && text[p] == '#') {
        skip_line();
      } else if (p < text.size() && text[p] != '\n' && text[p] != '\r') {
        return syntax_error(".env", text, p, "unexpected text after the quoted value");
      } else {
        skip_line();
      }
    } else {
      const std::size_t start = p;
      while (p < text.size() && text[p] != '\n' && text[p] != '\r') {
        if (text[p] == '#' && p > start && (text[p - 1] == ' ' || text[p - 1] == '\t')) break;
        ++p;
      }
      value = expand_vars(std::string(util::trim(text.substr(start, p - start))));
      skip_line();
    }
    CVT_TRY(put_member(root, std::move(key), Json(std::move(value)), policy));
  }
  return root;
}

Result<std::string> write_env(const Json& value, WriteContext& ctx) {
  const bool upper = ctx.options.get_bool("uppercase_keys", false);
  const bool export_prefix = ctx.options.get_bool("export", false);
  std::vector<std::pair<std::string, const Json*>> flat;
  std::string path;
  flatten_into(value, path, "_", flat);
  std::string out;
  std::size_t renamed = 0;
  for (const auto& [raw_key, leaf] : flat) {
    std::string key = raw_key.empty() ? "VALUE" : raw_key;
    for (char& c : key) {
      if (!(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_')) {
        c = '_';
        ++renamed;
      }
      if (upper) c = util::ascii_upper(c);
    }
    if (util::is_ascii_digit(key.front())) key.insert(0, "_");
    const std::string text = scalar_string(*leaf);
    if (export_prefix) out += "export ";
    out += key;
    out.push_back('=');
    const bool plain = !text.empty() && std::ranges::all_of(text, [](char c) {
      return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || std::string_view("_-./:@,+%").find(c) != std::string_view::npos;
    });
    if (plain) {
      out += text;
    } else {
      out.push_back('"');
      for (const char c : text) {
        switch (c) {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\n': out += "\\n"; break;
          case '\r': out += "\\r"; break;
          case '$': out += "\\$"; break;
          case '`': out += "\\`"; break;
          default: out.push_back(c);
        }
      }
      out.push_back('"');
    }
    out.push_back('\n');
  }
  if (renamed > 0) ctx.warn("Some keys had characters that are not allowed in variable names; they became '_'.");
  return out;
}

int sniff_env(ByteView /*data*/, std::string_view filename) {
  const std::size_t slash = filename.find_last_of("/\\");
  const std::string_view name = slash == std::string_view::npos ? filename : filename.substr(slash + 1);
  if (name == ".env" || name.starts_with(".env.") || has_extension(name, {"env"})) return 75;
  return 0;
}

// =========================================================================== Windows registry

constexpr std::string_view reg_header5 = "Windows Registry Editor Version 5.00";
constexpr std::string_view reg_header4 = "REGEDIT4";

Result<Json> reg_hex_value(std::string_view type, std::string_view hex, bool unicode, std::size_t offset,
                           std::string_view text) {
  Bytes bytes;
  for (const std::string_view part : util::split(hex, ',', false)) {
    const std::string_view digits = util::trim(part);
    if (digits.empty()) continue;
    const auto byte = util::parse_int(digits, 16);
    if (!byte || digits.size() > 2 || *byte < 0) return syntax_error("REG", text, offset, "invalid byte in a hex list");
    bytes.push_back(static_cast<std::byte>(*byte));
  }
  const auto utf16 = [&](ByteView data) {
    std::u16string units;
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
      units.push_back(static_cast<char16_t>(std::to_integer<unsigned>(data[i]) | (std::to_integer<unsigned>(data[i + 1]) << 8)));
    }
    return util::utf16_to_utf8(units);
  };
  const auto strings = [&](ByteView data) {
    std::string decoded = unicode ? utf16(data) : util::decode_text(data, util::TextEncoding::Windows1252).text;
    return decoded;
  };
  const auto typed = [](std::string_view name, Json value) {
    return Json::object({{"type", name}, {"value", std::move(value)}});
  };
  if (type == "hex") return make_binary(bytes);
  if (type == "hex(2)") {
    std::string s = strings(bytes);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return typed("REG_EXPAND_SZ", Json(std::move(s)));
  }
  if (type == "hex(7)") {
    std::string s = strings(bytes);
    Json list = Json::array();
    std::size_t start = 0;
    while (start < s.size()) {
      const std::size_t end = s.find('\0', start);
      const std::string_view item = std::string_view(s).substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (item.empty()) break;
      list.push_back(Json(item));
      if (end == std::string::npos) break;
      start = end + 1;
    }
    return list;
  }
  if (type == "hex(b)" && bytes.size() == 8) {
    std::uint64_t v = 0;
    for (std::size_t i = 8; i-- > 0;) v = (v << 8) | std::to_integer<std::uint64_t>(bytes[i]);
    if (v <= 0xFFFFFFFFu) return typed("REG_QWORD", Json(v));
    return v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ? Json(static_cast<std::int64_t>(v)) : Json(v);
  }
  if (type == "hex(4)" && bytes.size() == 4) {
    std::uint32_t v = 0;
    for (std::size_t i = 4; i-- > 0;) v = (v << 8) | std::to_integer<std::uint32_t>(bytes[i]);
    return Json(static_cast<std::int64_t>(v));
  }
  if (type == "hex(1)") {
    std::string s = strings(bytes);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return typed("REG_SZ", Json(std::move(s)));
  }
  return typed(type, make_binary(bytes));
}

Result<std::string> reg_quoted(std::string_view text, std::size_t& p, std::string_view whole, std::size_t offset) {
  ++p;  // "
  std::string out;
  while (p < text.size() && text[p] != '"') {
    if (text[p] == '\\' && p + 1 < text.size()) {
      out.push_back(text[p + 1]);
      p += 2;
      continue;
    }
    out.push_back(text[p++]);
  }
  if (p >= text.size()) return syntax_error("REG", whole, offset, "unterminated string");
  ++p;
  return out;
}

Result<Json> parse_reg(std::string_view text, ReadContext& ctx) {
  const ReadPolicy policy = read_policy(ctx.options);
  const std::vector<Line> lines = split_lines_with_offsets(text);
  std::size_t index = 0;
  while (index < lines.size() && util::trim(lines[index].text).empty()) ++index;
  if (index >= lines.size()) return fail(Status::Decode, "the registry file is empty");
  const std::string_view header = util::trim(lines[index].text);
  const bool unicode = header == reg_header5;
  if (!unicode && header != reg_header4) {
    return syntax_error("REG", text, lines[index].offset, "missing the 'Windows Registry Editor Version 5.00' header");
  }
  ++index;
  Json root = Json::object();
  Json* key = nullptr;
  std::string key_name;
  for (; index < lines.size(); ++index) {
    std::string_view line = util::trim(lines[index].text);
    const std::size_t offset = lines[index].offset;
    if (line.empty() || line.front() == ';') continue;
    if (line.front() == '[') {
      if (!line.ends_with(']')) return syntax_error("REG", text, offset, "a key name must end with ']'");
      key_name = std::string(line.substr(1, line.size() - 2));
      if (key_name.starts_with('-')) {
        CVT_TRY(put_member(root, key_name, Json(), policy));
        key = nullptr;
        continue;
      }
      if (root.find(key_name) == nullptr) root.set(key_name, Json::object());
      key = root.find(key_name);
      continue;
    }
    if (key == nullptr) return syntax_error("REG", text, offset, "a value must follow a [key] line");
    // Join hex continuation lines.
    std::string logical(line);
    while (logical.ends_with('\\') && index + 1 < lines.size()) {
      logical.pop_back();
      logical += util::trim(lines[++index].text);
    }
    std::size_t p = 0;
    std::string name;
    if (logical.starts_with('@')) {
      name = "@";
      p = 1;
    } else if (logical.starts_with('"')) {
      CVT_TRY_ASSIGN(name, reg_quoted(logical, p, text, offset));
    } else {
      return syntax_error("REG", text, offset, "expected a value name in quotes or @");
    }
    while (p < logical.size() && (logical[p] == ' ' || logical[p] == '\t')) ++p;
    if (p >= logical.size() || logical[p] != '=') return syntax_error("REG", text, offset, "expected '=' after the value name");
    ++p;
    while (p < logical.size() && (logical[p] == ' ' || logical[p] == '\t')) ++p;
    const std::string_view data = std::string_view(logical).substr(p);
    Json value;
    if (data == "-") {
      value = Json();
    } else if (data.starts_with('"')) {
      std::size_t q = 0;
      CVT_TRY_ASSIGN(std::string s, reg_quoted(data, q, text, offset));
      value = Json(std::move(s));
    } else if (data.starts_with("dword:")) {
      const auto v = util::parse_int(util::trim(data.substr(6)), 16);
      if (!v || *v < 0 || *v > 0xFFFFFFFFll) return syntax_error("REG", text, offset, "invalid dword value");
      value = Json(*v);
    } else if (data.starts_with("hex")) {
      const std::size_t colon = data.find(':');
      if (colon == std::string_view::npos) return syntax_error("REG", text, offset, "expected ':' after hex");
      CVT_TRY_ASSIGN(value, reg_hex_value(util::to_lower(data.substr(0, colon)), data.substr(colon + 1), unicode, offset, text));
    } else {
      return syntax_error("REG", text, offset, "unknown value type");
    }
    CVT_TRY(put_member(*key, std::move(name), std::move(value), policy));
  }
  return root;
}

std::string reg_quote(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

void reg_hex(std::string& out, std::string_view prefix, ByteView bytes) {
  std::size_t column = prefix.size();
  out += prefix;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out += std::format("{:02x}", std::to_integer<unsigned>(bytes[i]));
    column += 2;
    if (i + 1 < bytes.size()) {
      out.push_back(',');
      ++column;
      if (column > 76) {
        out += "\\\r\n  ";
        column = 2;
      }
    }
  }
}

Bytes utf16le(std::string_view text, bool terminate) {
  Bytes out;
  for (const char16_t unit : util::utf8_to_utf16(text)) {
    out.push_back(static_cast<std::byte>(unit & 0xFF));
    out.push_back(static_cast<std::byte>(unit >> 8));
  }
  if (terminate) out.insert(out.end(), 2, std::byte{0});
  return out;
}

Result<std::string> reg_value_line(std::string_view name, const Json& value) {
  std::string out = name == "@" ? std::string("@") : reg_quote(name);
  out.push_back('=');
  if (value.is_null()) return out + "-";
  if (value.is_string()) return out + reg_quote(value.as_string());
  if (value.is_bool()) return out + std::format("dword:{:08x}", value.as_bool() ? 1 : 0);
  if (value.is_integer()) {
    if (const auto v = value.get_int64(); v && *v >= 0 && *v <= 0xFFFFFFFFll) return out + std::format("dword:{:08x}", *v);
    const std::uint64_t v = value.is_unsigned() ? value.as_uint64() : static_cast<std::uint64_t>(value.as_int64());
    Bytes bytes;
    for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    reg_hex(out, "hex(b):", bytes);
    return out;
  }
  if (tag_kind(value) == TagKind::Binary) {
    const auto bytes = tagged_bytes(value);
    if (!bytes) return fail(Status::Encode, "invalid Base64 in a binary value");
    reg_hex(out, "hex:", *bytes);
    return out;
  }
  if (value.is_array() && std::ranges::all_of(value.elements(), [](const Json& e) { return e.is_string(); })) {
    Bytes bytes;
    for (const Json& item : value.elements()) {
      const Bytes part = utf16le(item.as_string(), true);
      bytes.insert(bytes.end(), part.begin(), part.end());
    }
    bytes.insert(bytes.end(), 2, std::byte{0});
    reg_hex(out, "hex(7):", bytes);
    return out;
  }
  if (value.is_object() && value.size() == 2 && value["type"].is_string() && value.contains("value")) {
    const std::string type = value["type"].as_string();
    const Json& inner = value["value"];
    if (type == "REG_EXPAND_SZ" && inner.is_string()) {
      reg_hex(out, "hex(2):", utf16le(inner.as_string(), true));
      return out;
    }
    if (type == "REG_SZ" && inner.is_string()) return out + reg_quote(inner.as_string());
    if (type == "REG_QWORD" && inner.get_uint64()) {
      const std::uint64_t v = *inner.get_uint64();
      Bytes bytes;
      for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
      reg_hex(out, "hex(b):", bytes);
      return out;
    }
    if (type.starts_with("hex(") && tag_kind(inner) == TagKind::Binary) {
      const auto bytes = tagged_bytes(inner);
      if (bytes) {
        reg_hex(out, type + ":", *bytes);
        return out;
      }
    }
  }
  return out + reg_quote(scalar_string(value));
}

Result<std::string> write_reg_text(const Json& value, WriteContext& ctx) {
  std::string out(reg_header5);
  out += "\r\n";
  std::size_t converted = 0;
  if (!value.is_object() || tag_kind(value) != TagKind::None) {
    return fail(Status::Encode, "a registry file needs an object of keys such as {\"HKEY_CURRENT_USER\\\\Software\\\\X\": {...}}");
  }
  for (const auto& member : value.items()) {
    out += "\r\n";
    if (member.value().is_null()) {
      out += "[" + (member.key().starts_with('-') ? member.key() : "-" + member.key()) + "]\r\n";
      continue;
    }
    out += "[" + member.key() + "]\r\n";
    if (!member.value().is_object()) {
      CVT_TRY_ASSIGN(const std::string line, reg_value_line("@", member.value()));
      out += line + "\r\n";
      continue;
    }
    for (const auto& entry : member.value().items()) {
      if (entry.value().is_object() && tag_kind(entry.value()) == TagKind::None && !entry.value().contains("type")) ++converted;
      CVT_TRY_ASSIGN(const std::string line, reg_value_line(entry.key(), entry.value()));
      out += line + "\r\n";
    }
  }
  if (converted > 0) ctx.warn("Nested objects have no registry type and were written as JSON text.");
  return out;
}

Result<Bytes> write_reg(const Json& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, write_reg_text(value, ctx));
  if (ctx.options.get_enum("reg_encoding", "utf-16", {"utf-16", "utf-8"}) == "utf-8") return to_bytes(text);
  return util::encode_text(text, util::TextEncoding::Utf16LE, true).bytes;
}

int sniff_reg(ByteView data, std::string_view filename) {
  const std::string_view raw = as_chars(data);
  if (raw.starts_with("\xFF\xFEW\0i\0n\0d\0o\0w\0s\0 \0R\0e\0g\0"sv)) return 95;
  const std::string_view head = text_head(data, 64);
  if (head.starts_with(reg_header5) || head.starts_with(reg_header4)) return 95;
  return has_extension(filename, {"reg"}) ? 40 : 0;
}

}  // namespace

void register_config_formats(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "ini",
      .name = "INI configuration",
      .description = "Sections of key = value settings (INI, CFG and CONF files).",
      .category = Category::Data,
      .extensions = {"ini", "cfg", "conf", "inf", "cnf", "desktop", "gitconfig", "editorconfig"},
      .mime = "text/plain",
      .popularity = 50,
      .signatures = {},
      .sniff = sniff_ini,
  });
  std::vector<OptionSpec> ini_read = common_read_options(true, false);
  ini_read.push_back(OptionSpec::boolean("inline_comments", "Inline comments", true,
                                         "Treat ';' after white space as the start of a comment ('#' starts comment lines only)."));
  ini_read.push_back(OptionSpec::boolean("unquote", "Remove quotes", true,
                                         "Strip matching quotes around values (and decode \\n, \\t, \\\" in double quotes)."));
  ini_read.push_back(OptionSpec::boolean("nested_sections", "Nested sections", false,
                                         "Read [a.b] as section b inside section a."));
  ini_read.push_back(OptionSpec::boolean("infer_types", "Detect numbers and booleans", false,
                                         "Turn unquoted values such as 42, 1.5 and true into numbers and booleans."));
  ini_read.push_back(OptionSpec::boolean("multiline_values", "Continuation lines", true,
                                         "Indented lines without '=' continue the previous value.")
                         .advanced());
  add_codec(registry,
            {.format_id = "ini",
             .read = text_reader(parse_ini, "INI", true),
             .write = text_writer(write_ini, "INI"),
             .read_options = std::move(ini_read),
             .write_options = {
                 sort_keys_option(),
                 OptionSpec::choice("separator", "Separator", " = ", {" = ", "=", ": "}, "Between key and value."),
                 OptionSpec::choice("array_style", "Lists", "repeat", {"repeat", "json", "indexed"},
                                    "Lists as repeated key[] = lines, as JSON text, or as key.0, key.1 entries."),
                 OptionSpec::choice("null_values", "Null values", "empty", {"empty", "bare_key", "omit"},
                                    "Null as an empty value, as a key without '=', or left out."),
                 OptionSpec::choice("line_ending", "Line ending", "lf", {"lf", "crlf"}, "Line terminator.").advanced(),
             },
             .lossy = true});

  registry.add_format(FormatInfo{
      .id = "properties",
      .name = "Java properties",
      .description = "Java .properties key = value settings and resource bundles.",
      .category = Category::Data,
      .extensions = {"properties"},
      .mime = "text/x-java-properties",
      .popularity = 35,
      .signatures = {},
      .sniff = sniff_properties,
  });
  std::vector<OptionSpec> properties_read = common_read_options(true, false);
  properties_read.push_back(OptionSpec::boolean("nested_keys", "Nest dotted keys", false,
                                                "Read a.b.c = 1 as nested objects instead of one flat key."));
  add_codec(registry, {.format_id = "properties",
                       .read = text_reader(parse_properties, "properties", true),
                       .write = text_writer(write_properties, "properties"),
                       .read_options = std::move(properties_read),
                       .write_options = {
                           sort_keys_option(),
                           OptionSpec::boolean("ascii_only", "Escape non-ASCII", true,
                                               "Write characters beyond ASCII as \\uXXXX, readable by every Java version."),
                           OptionSpec::choice("separator", "Separator", "=", {"=", " = ", ": "}, "Between key and value."),
                       },
                       .lossy = true});

  registry.add_format(FormatInfo{
      .id = "env",
      .name = "Environment file",
      .description = "dotenv KEY=value files used to configure applications.",
      .category = Category::Data,
      .extensions = {"env"},
      .mime = "text/plain",
      .popularity = 40,
      .signatures = {},
      .sniff = sniff_env,
  });
  std::vector<OptionSpec> env_read = common_read_options(true, false);
  env_read.push_back(OptionSpec::boolean("expand_variables", "Expand variables", false,
                                         "Replace ${NAME} and $NAME with values defined earlier in the file."));
  add_codec(registry, {.format_id = "env",
                       .read = text_reader(parse_env, ".env"),
                       .write = text_writer(write_env, ".env"),
                       .read_options = std::move(env_read),
                       .write_options = {
                           OptionSpec::boolean("uppercase_keys", "Upper-case names", false,
                                               "Write variable names in upper case."),
                           OptionSpec::boolean("export", "Export prefix", false, "Prefix every line with 'export '."),
                       },
                       .lossy = true});

  registry.add_format(FormatInfo{
      .id = "reg",
      .name = "Windows registry file",
      .description = "Registry keys and values exported by the Windows Registry Editor.",
      .category = Category::Data,
      .extensions = {"reg"},
      .mime = "text/x-ms-regedit",
      .popularity = 25,
      .signatures = {},
      .sniff = sniff_reg,
  });
  add_codec(registry, {.format_id = "reg",
                       .read = text_reader(parse_reg, "REG", true),
                       .write = binary_writer(write_reg, "REG"),
                       .read_options = common_read_options(true, false),
                       .write_options = {
                           OptionSpec::choice("reg_encoding", "Encoding", "utf-16", {"utf-16", "utf-8"},
                                              "UTF-16 with a byte order mark is what the Registry Editor writes."),
                       }});
}

}  // namespace cvt::data
