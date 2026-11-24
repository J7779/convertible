// Convertible core - data family: web key/value formats.
//
//   querystring   application/x-www-form-urlencoded / URL query strings.
//                 Nested keys follow `nested_keys`: "brackets" (a[b]=1,
//                 a[]=1&a[]=2, a[0][c]=1 - the PHP / Rails / qs convention),
//                 "dots" (a.b=1) or "none" (flat; repeated keys become arrays).
//   http-headers  "Name: value" header blocks; a request or status line is kept
//                 under ":start-line", repeated headers become arrays, folded
//                 continuation lines are joined. Anything after the blank line
//                 ending the headers is ignored (with a warning).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/data.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace {

std::string percent_decode(std::string_view text, std::size_t& malformed) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%') {
      const auto hex = [](char h) {
        return (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
      };
      const int high = i + 2 < text.size() ? hex(text[i + 1]) : -1;
      const int low = i + 2 < text.size() ? hex(text[i + 2]) : -1;
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>(high * 16 + low));
        i += 2;
      } else {
        ++malformed;
        out.push_back(c);
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

void percent_encode(std::string& out, std::string_view text, bool key) {
  static constexpr char hex[] = "0123456789ABCDEF";
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    if (util::is_ascii_alpha(ch) || util::is_ascii_digit(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '*' ||
        (key && (ch == '[' || ch == ']'))) {
      out.push_back(ch);
    } else if (ch == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
}

/// "a[b][]" -> {"a", "b", ""}; "a.b" -> {"a", "b"} in dots mode.
std::vector<std::string> split_key(std::string_view key, std::string_view mode) {
  std::vector<std::string> parts;
  if (mode == "dots") {
    for (const std::string_view part : util::split(key, '.')) parts.emplace_back(part);
    return parts;
  }
  if (mode == "none") return {std::string(key)};
  const std::size_t open = key.find('[');
  if (open == std::string_view::npos || open == 0 || !key.ends_with(']')) return {std::string(key)};
  parts.emplace_back(key.substr(0, open));
  std::size_t pos = open;
  while (pos < key.size()) {
    if (key[pos] != '[') return {std::string(key)};
    const std::size_t close = key.find(']', pos);
    if (close == std::string_view::npos) return {std::string(key)};
    parts.emplace_back(key.substr(pos + 1, close - pos - 1));
    pos = close + 1;
  }
  return parts;
}

bool is_index(std::string_view part) {
  return !part.empty() && part.size() <= 6 && std::ranges::all_of(part, [](char c) { return util::is_ascii_digit(c); });
}

Result<void> assign(Json& root, const std::vector<std::string>& parts, Json value, const ReadPolicy& policy) {
  if (parts.size() > policy.max_depth) return fail(Status::Limit, "a query-string key is nested too deeply");
  Json* node = &root;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const std::string& part = parts[i];
    const bool last = i + 1 == parts.size();
    const bool next_is_array = !last && (parts[i + 1].empty() || is_index(parts[i + 1]));
    if (node->is_array()) {
      if (part.empty()) {
        if (last) {
          node->push_back(std::move(value));
          return {};
        }
        // a[][x]=1: extend the last element unless it already has the key.
        Json* tail = node->empty() ? nullptr : node->at(node->size() - 1);
        if (tail == nullptr || !tail->is_structured() || (tail->is_object() && tail->contains(parts[i + 1]))) {
          node->push_back(next_is_array ? Json::array() : Json::object());
          tail = node->at(node->size() - 1);
        }
        node = tail;
        continue;
      }
      if (is_index(part)) {
        const auto index = static_cast<std::size_t>(util::parse_int(part).value_or(0));
        if (index > node->size() + 1000) return fail(Status::Limit, "a query-string array index is too large");
        Json& slot = (*node)[index];
        if (last) {
          slot = std::move(value);
          return {};
        }
        if (!slot.is_structured()) slot = next_is_array ? Json::array() : Json::object();
        node = &slot;
        continue;
      }
      // A named key inside what looked like an array: turn it into an object.
      Json object = Json::object();
      for (std::size_t k = 0; k < node->size(); ++k) object.set(std::to_string(k), std::move((*node)[k]));
      *node = std::move(object);
    }
    if (!node->is_object()) *node = Json::object();
    if (last) {
      if (Json* existing = node->find(part); existing != nullptr && policy.duplicates == ReadPolicy::Duplicates::Last &&
                                             parts.size() == 1) {
        // Repeated flat keys collect into an array (a=1&a=2).
        if (!existing->is_array()) {
          Json first = std::move(*existing);
          *existing = Json::array({std::move(first)});
        }
        existing->push_back(std::move(value));
        return {};
      }
      return put_member(*node, part, std::move(value), policy);
    }
    Json* child = node->find(part);
    if (child == nullptr || !child->is_structured()) {
      node->set(part, next_is_array ? Json::array() : Json::object());
      child = node->find(part);
    }
    node = child;
  }
  return {};
}

Result<Json> parse_querystring(std::string_view text, ReadContext& ctx) {
  const ReadPolicy policy = read_policy(ctx.options);
  const std::string mode = ctx.options.get_enum("nested_keys", "brackets", {"brackets", "dots", "none"});
  const bool semicolons = ctx.options.get_bool("semicolon_separator", false);
  std::string_view body = util::trim(text);
  if (body.starts_with("\xEF\xBB\xBF")) body.remove_prefix(3);
  if (const std::size_t q = body.find('?'); q != std::string_view::npos && body.substr(0, q).find('=') == std::string_view::npos) {
    body.remove_prefix(q + 1);  // a whole URL: keep the query part
  }
  if (const std::size_t hash = body.find('#'); hash != std::string_view::npos) body = body.substr(0, hash);
  Json root = Json::object();
  std::size_t malformed = 0;
  std::size_t pairs = 0;
  std::size_t start = 0;
  while (start <= body.size()) {
    std::size_t end = body.find_first_of(semicolons ? "&;" : "&", start);
    if (end == std::string_view::npos) end = body.size();
    const std::string_view pair = body.substr(start, end - start);
    start = end + 1;
    if (pair.empty()) {
      if (end == body.size()) break;
      continue;
    }
    CVT_TRY(ctx.limits.check_archive_entries(++pairs));
    const std::size_t eq = pair.find('=');
    const std::string key = percent_decode(pair.substr(0, eq), malformed);
    Json value = eq == std::string_view::npos ? Json() : Json(percent_decode(pair.substr(eq + 1), malformed));
    CVT_TRY(assign(root, split_key(key, mode), std::move(value), policy));
    if (end == body.size()) break;
  }
  if (malformed > 0) ctx.warn(std::format("{} malformed percent escape(s) were kept as they are.", malformed));
  // Percent-decoding may produce invalid UTF-8.
  std::size_t repaired = 0;
  std::vector<Json*> stack{&root};
  while (!stack.empty()) {
    Json* node = stack.back();
    stack.pop_back();
    if (node->is_string()) {
      std::size_t count = 0;
      std::string fixed = repair_utf8(node->as_string(), &count);
      if (count > 0) {
        *node = Json(std::move(fixed));
        repaired += count;
      }
    }
    for (Json& element : node->elements()) stack.push_back(&element);
    for (auto& member : node->items()) stack.push_back(&member.value());
  }
  if (repaired > 0) ctx.warn("Some values were not valid UTF-8 after decoding; they were repaired with U+FFFD.");
  return root;
}

std::string leaf_text(const Json& value) {
  switch (tag_kind(value)) {
    case TagKind::None: break;
    case TagKind::BigInt:
    case TagKind::Decimal:
    case TagKind::Date: return std::string(tag_text(value));
    default: {
      const Json plain = plain_value(value);
      return plain.is_string() ? plain.as_string() : util::dump_json(plain);
    }
  }
  switch (value.type()) {
    case Json::Type::Bool: return value.as_bool() ? "true" : "false";
    case Json::Type::Int64:
    case Json::Type::UInt64: return format_integer(value);
    case Json::Type::Double: return format_double(value.as_double());
    case Json::Type::String: return value.as_string();
    default: return {};
  }
}

void emit_pairs(std::string& out, const std::string& key, const Json& value, std::string_view mode,
                std::string_view arrays) {
  const bool leaf = !value.is_structured() || tag_kind(value) != TagKind::None;
  if (leaf) {
    if (!out.empty()) out.push_back('&');
    percent_encode(out, key, true);
    if (!value.is_null()) {
      out.push_back('=');
      percent_encode(out, leaf_text(value), false);
    }
    return;
  }
  const auto child_key = [&](std::string_view part, bool element) {
    if (key.empty()) return std::string(part);
    if (mode == "dots") return key + "." + std::string(part);
    if (mode == "none") return element && arrays == "repeat" ? key : key + "." + std::string(part);
    return key + "[" + std::string(part) + "]";
  };
  if (value.is_array()) {
    for (std::size_t i = 0; i < value.size(); ++i) {
      const Json& element = value[i];
      const bool element_leaf = !element.is_structured() || tag_kind(element) != TagKind::None;
      std::string next;
      if (arrays == "repeat" && element_leaf) {
        next = key;
      } else if (arrays == "brackets" && element_leaf && mode == "brackets") {
        next = key + "[]";
      } else {
        next = child_key(std::to_string(i), true);
      }
      emit_pairs(out, next, element, mode, arrays);
    }
    return;
  }
  for (const auto& member : value.items()) emit_pairs(out, child_key(member.key(), false), member.value(), mode, arrays);
}

Result<std::string> write_querystring(const Json& value, WriteContext& ctx) {
  const std::string mode = ctx.options.get_enum("nested_keys", "brackets", {"brackets", "dots", "none"});
  const std::string arrays = ctx.options.get_enum("array_format", "brackets", {"brackets", "indices", "repeat"});
  std::string out;
  if (!value.is_object() || tag_kind(value) != TagKind::None) {
    emit_pairs(out, "value", value, mode, arrays);
  } else {
    for (const Json::Member* member : members_of(value, ctx.options.get_bool("sort_keys", false))) {
      emit_pairs(out, member->key(), member->value(), mode, arrays);
    }
  }
  out.push_back('\n');
  return out;
}

int sniff_querystring(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"qs", "urlencoded", "form"})) return 70;
  (void)data;
  return 0;
}

// =========================================================================== HTTP headers

bool token_char(char c) {
  return util::is_ascii_alpha(c) || util::is_ascii_digit(c) ||
         std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

Result<Json> parse_headers(std::string_view text, ReadContext& ctx) {
  Json root = Json::object();
  std::size_t pos = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
  std::string last;
  bool first = true;
  std::size_t count = 0;
  while (pos < text.size()) {
    std::size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    if (line.ends_with('\r')) line.remove_suffix(1);
    const std::size_t line_at = pos;
    pos = end + 1;
    if (line.empty()) {
      if (first) continue;
      if (!util::trim(text.substr(std::min(pos, text.size()))).empty()) {
        ctx.warn("The message body after the headers was not converted.");
      }
      break;
    }
    if ((line.front() == ' ' || line.front() == '\t') && !last.empty()) {
      Json* existing = root.find(last);
      Json* target = existing != nullptr && existing->is_array() ? existing->at(existing->size() - 1) : existing;
      if (target != nullptr) *target = Json(target->as_string() + " " + std::string(util::trim(line)));
      continue;
    }
    const std::size_t colon = line.find(':');
    const bool header = colon != std::string_view::npos && colon > 0 &&
                        std::ranges::all_of(line.substr(0, colon), token_char);
    if (!header) {
      if (first) {
        root.set(":start-line", Json(std::string(util::trim(line))));
        first = false;
        continue;
      }
      return syntax_error("HTTP headers", text, line_at, "expected 'Name: value'");
    }
    first = false;
    CVT_TRY(ctx.limits.check_archive_entries(++count));
    std::string name(line.substr(0, colon));
    Json value(std::string(util::trim(line.substr(colon + 1))));
    if (Json* existing = root.find(name)) {
      if (!existing->is_array()) {
        Json previous = std::move(*existing);
        *existing = Json::array({std::move(previous)});
      }
      existing->push_back(std::move(value));
    } else {
      root.set(name, std::move(value));
    }
    last = std::move(name);
  }
  return root;
}

Result<std::string> write_headers(const Json& value, WriteContext& ctx) {
  std::string out;
  if (!value.is_object() || tag_kind(value) != TagKind::None) {
    return fail(Status::Encode, "HTTP headers need an object of header names");
  }
  std::size_t renamed = 0;
  const auto line = [&](std::string_view name, const Json& v) {
    std::string clean;
    for (const char c : name) {
      clean.push_back(token_char(c) ? c : '-');
      if (!token_char(c)) ++renamed;
    }
    std::string text = leaf_text(v);
    if (v.is_structured() && tag_kind(v) == TagKind::None) text = util::dump_json(v);
    for (char& c : text) {
      if (c == '\r' || c == '\n') c = ' ';
    }
    out += clean.empty() ? "X" : clean;
    out += ": ";
    out += text;
    out += "\r\n";
  };
  if (const Json* start = value.find(":start-line"); start != nullptr && start->is_string()) {
    out += start->as_string() + "\r\n";
  }
  for (const auto& member : value.items()) {
    if (member.key() == ":start-line") continue;
    if (member.value().is_array()) {
      for (const Json& element : member.value().elements()) line(member.key(), element);
    } else {
      line(member.key(), member.value());
    }
  }
  if (renamed > 0) ctx.warn("Characters that are not allowed in header names were replaced with '-'.");
  return out;
}

int sniff_headers(ByteView data, std::string_view filename) {
  const std::string_view head = text_head(data, 256);
  if (head.starts_with("HTTP/1.") || head.starts_with("HTTP/2 ")) return 85;
  for (const std::string_view method : {"GET ", "POST ", "PUT ", "DELETE ", "HEAD ", "OPTIONS ", "PATCH "}) {
    if (head.starts_with(method)) {
      const std::size_t nl = head.find('\n');
      if (head.substr(0, nl).find(" HTTP/") != std::string_view::npos) return 85;
    }
  }
  return has_extension(filename, {"headers", "http"}) ? 70 : 0;
}

}  // namespace

void register_web_formats(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "querystring",
      .name = "URL query string",
      .description = "Form data and URL query parameters (application/x-www-form-urlencoded).",
      .category = Category::Data,
      .extensions = {"qs", "urlencoded", "form"},
      .mime = "application/x-www-form-urlencoded",
      .popularity = 20,
      .signatures = {},
      .sniff = sniff_querystring,
  });
  OptionSpec nested = OptionSpec::choice("nested_keys", "Nested keys", "brackets", {"brackets", "dots", "none"},
                                         "How nesting is spelled in keys: a[b][c] and a[] (PHP, Rails), a.b.c, or "
                                         "not at all.");
  std::vector<OptionSpec> read_options = common_read_options(true, false);
  read_options.push_back(nested);
  read_options.push_back(OptionSpec::boolean("semicolon_separator", "';' separates pairs", false,
                                             "Also split pairs at ';' (old HTML recommendation).")
                             .advanced());
  add_codec(registry, {.format_id = "querystring",
                       .read = text_reader(parse_querystring, "query string"),
                       .write = text_writer(write_querystring, "query string"),
                       .read_options = std::move(read_options),
                       .write_options = {nested, sort_keys_option(),
                                         OptionSpec::choice("array_format", "Lists", "brackets",
                                                            {"brackets", "indices", "repeat"},
                                                            "Lists as a[]=1&a[]=2, a[0]=1&a[1]=2, or a=1&a=2.")},
                       .lossy = true});

  registry.add_format(FormatInfo{
      .id = "http-headers",
      .name = "HTTP headers",
      .description = "A block of HTTP header lines (Name: value), optionally with the request or status line.",
      .category = Category::Data,
      .extensions = {"headers", "http"},
      .mime = "text/plain",
      .popularity = 15,
      .signatures = {},
      .sniff = sniff_headers,
  });
  add_codec(registry, {.format_id = "http-headers",
                       .read = text_reader(parse_headers, "HTTP headers", true),
                       .write = text_writer(write_headers, "HTTP headers"),
                       .read_options = common_read_options(false, false),
                       .lossy = true});
}

}  // namespace cvt::data
