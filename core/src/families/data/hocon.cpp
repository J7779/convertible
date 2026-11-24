// Convertible core - data family: HOCON (Human-Optimized Config Object Notation), read only.
//
// Supported: the JSON superset (comments, optional root braces, '=' or ':'
// or nothing before '{', optional commas, newlines as separators), unquoted
// keys and values, dotted path keys, deep merging of repeated objects, value
// concatenation ("a b", "x ${y}", object and array concatenation),
// substitutions ${path} and optional ${?path} resolved against the document,
// += appends, and """triple-quoted""" strings. include statements are
// ignored with a warning - a conversion never reads other files - and
// substitutions are never looked up in the environment.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace {

// Placeholders that live only between parsing and resolution.
constexpr std::string_view concat_key = "\x01concat";
constexpr std::string_view ref_key = "\x01ref";
constexpr std::string_view optional_key = "\x01opt";

class HoconParser {
 public:
  HoconParser(std::string_view text, const ReadContext& ctx) : t_(text), policy_(read_policy(ctx.options)) {}

  Result<Json> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    CVT_TRY(skip(true));
    Json root;
    if (peek() == '[') {
      CVT_TRY_ASSIGN(root, array(0));
    } else if (peek() == '{') {
      ++p_;
      root = Json::object();
      CVT_TRY(object_body(root, '}', 0));
    } else {
      root = Json::object();
      CVT_TRY(object_body(root, '\0', 0));
    }
    CVT_TRY(skip(true));
    if (p_ < t_.size()) return error("unexpected content after the document");
    return root;
  }

  std::size_t includes_ = 0;

 private:
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept { return p_ + ahead < t_.size() ? t_[p_ + ahead] : '\0'; }
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("HOCON", t_, p_, message); }

  Result<void> skip(bool newlines) {
    while (p_ < t_.size()) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || (newlines && c == '\n')) {
        ++p_;
      } else if (c == '#' || (c == '/' && peek(1) == '/')) {
        while (p_ < t_.size() && peek() != '\n') ++p_;
      } else if (static_cast<unsigned char>(c) == 0xEF && t_.substr(p_, 3) == "\xEF\xBB\xBF") {
        p_ += 3;
      } else {
        break;
      }
    }
    return {};
  }

  [[nodiscard]] static bool forbidden(char c) {
    return std::string_view("$\"{}[]:=,+#`^?!@*&\\").find(c) != std::string_view::npos || c == '\n';
  }

  Result<std::string> quoted() {
    if (t_.substr(p_, 3) == "\"\"\"") {
      p_ += 3;
      const std::size_t end = t_.find("\"\"\"", p_);
      if (end == std::string_view::npos) return error("unterminated \"\"\" string");
      std::size_t close = end + 3;
      while (close < t_.size() && t_[close] == '"') ++close;  // extra quotes belong to the text
      std::string text(t_.substr(p_, close - 3 - p_));
      p_ = close;
      return text;
    }
    ++p_;
    std::string out;
    while (true) {
      if (p_ >= t_.size() || peek() == '\n') return error("unterminated string");
      const char c = t_[p_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      const char e = peek();
      ++p_;
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          const auto cp = p_ + 4 <= t_.size() ? util::parse_int(t_.substr(p_, 4), 16) : std::nullopt;
          if (!cp) return error("invalid \\u escape");
          p_ += 4;
          util::append_utf8(out, util::is_scalar_value(static_cast<char32_t>(*cp)) ? static_cast<char32_t>(*cp)
                                                                                   : util::replacement_character);
          break;
        }
        default: return error("invalid escape");
      }
    }
  }

  /// A path expression: unquoted and quoted segments separated by '.'.
  Result<std::vector<std::string>> path_until(std::string_view stops) {
    std::vector<std::string> parts{std::string()};
    bool any = false;
    while (p_ < t_.size()) {
      const char c = peek();
      if (c == '"') {
        CVT_TRY_ASSIGN(std::string text, quoted());
        parts.back() += text;
        any = true;
        continue;
      }
      if (c == '.') {
        parts.emplace_back();
        ++p_;
        continue;
      }
      if (stops.find(c) != std::string_view::npos || c == '\n' || c == '#' || (c == '/' && peek(1) == '/') ||
          (c == '+' && peek(1) == '=')) {
        break;
      }
      if (forbidden(c) && c != ' ' && c != '\t') return error(std::format("'{}' is not allowed in a key", c));
      parts.back().push_back(c);
      any = true;
      ++p_;
    }
    for (std::string& part : parts) {
      // Unquoted white space around segments is not part of the key.
      part = std::string(util::trim(part));
    }
    if (!any) return error("expected a key");
    return parts;
  }

  static Json ref(std::string path, bool optional) {
    return Json::object({{ref_key, std::move(path)}, {optional_key, optional}});
  }

  /// A value up to the end of its line (or ',', '}', ']'): a concatenation.
  Result<Json> value(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the HOCON data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    std::vector<Json> pieces;
    std::vector<bool> quoted_piece;
    while (p_ < t_.size()) {
      const char c = peek();
      if (c == '\n' || c == ',' || c == '}' || c == ']' || c == '#' || (c == '/' && peek(1) == '/')) break;
      if (c == ' ' || c == '\t' || c == '\r') {
        const std::size_t start = p_;
        while (peek() == ' ' || peek() == '\t' || peek() == '\r') ++p_;
        pieces.emplace_back(std::string(t_.substr(start, p_ - start)));
        quoted_piece.push_back(false);
        continue;
      }
      if (c == '{') {
        ++p_;
        Json object = Json::object();
        CVT_TRY(object_body(object, '}', depth + 1));
        pieces.push_back(std::move(object));
        quoted_piece.push_back(true);
        continue;
      }
      if (c == '[') {
        CVT_TRY_ASSIGN(Json list, array(depth + 1));
        pieces.push_back(std::move(list));
        quoted_piece.push_back(true);
        continue;
      }
      if (c == '"') {
        CVT_TRY_ASSIGN(std::string text, quoted());
        pieces.emplace_back(std::move(text));
        quoted_piece.push_back(true);
        continue;
      }
      if (c == '$' && peek(1) == '{') {
        p_ += 2;
        const bool optional = peek() == '?';
        if (optional) ++p_;
        const std::size_t end = t_.find('}', p_);
        if (end == std::string_view::npos) return error("unterminated substitution");
        std::string path(util::trim(t_.substr(p_, end - p_)));
        p_ = end + 1;
        pieces.push_back(ref(std::move(path), optional));
        quoted_piece.push_back(true);
        continue;
      }
      const std::size_t start = p_;
      while (p_ < t_.size() && !forbidden(peek()) && peek() != ' ' && peek() != '\t' && peek() != '\r' &&
             !(peek() == '/' && peek(1) == '/')) {
        ++p_;
      }
      if (p_ == start) return error(std::format("unexpected '{}' in a value", c));
      pieces.emplace_back(std::string(t_.substr(start, p_ - start)));
      quoted_piece.push_back(false);
    }
    // Trim unquoted white space at both ends.
    while (!pieces.empty() && !quoted_piece.back() && util::trim(pieces.back().as_string()).empty()) {
      pieces.pop_back();
      quoted_piece.pop_back();
    }
    while (!pieces.empty() && !quoted_piece.front() && util::trim(pieces.front().as_string()).empty()) {
      pieces.erase(pieces.begin());
      quoted_piece.erase(quoted_piece.begin());
    }
    if (pieces.empty()) return error("expected a value");
    if (pieces.size() == 1) {
      if (!quoted_piece[0]) return typed(pieces[0].as_string());
      return std::move(pieces[0]);
    }
    Json list = Json::array();
    for (Json& piece : pieces) list.push_back(std::move(piece));
    return Json::object({{concat_key, std::move(list)}});
  }

  static Json typed(const std::string& text) {
    if (text == "true" || text == "yes" || text == "on") return text == "true" ? Json(true) : Json(text);
    if (text == "false") return Json(false);
    if (text == "null") return Json();
    if (decimal_parts(text) && (util::is_ascii_digit(text[0]) || text[0] == '-') &&
        text.find_first_not_of("0123456789.eE+-") == std::string::npos) {
      return number_from_literal(text);
    }
    return Json(text);
  }

  Result<Json> array(std::size_t depth) {
    ++p_;  // [
    Json list = Json::array();
    while (true) {
      CVT_TRY(skip(true));
      if (p_ >= t_.size()) return error("unterminated array");
      if (peek() == ']') {
        ++p_;
        return list;
      }
      if (peek() == ',') return error("unexpected ','");
      CVT_TRY_ASSIGN(Json v, value(depth + 1));
      list.push_back(std::move(v));
      CVT_TRY(skip(false));
      if (peek() == ',') ++p_;
    }
  }

  static void merge_into(Json& target, Json value) {
    if (target.is_object() && value.is_object() && !target.contains(concat_key) && !value.contains(concat_key) &&
        !target.contains(ref_key) && !value.contains(ref_key)) {
      for (auto& member : value.items()) {
        if (Json* existing = target.find(member.key())) {
          merge_into(*existing, std::move(member.value()));
        } else {
          target.set(member.key(), std::move(member.value()));
        }
      }
      return;
    }
    target = std::move(value);
  }

  Result<void> object_body(Json& object, char close, std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the HOCON data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    while (true) {
      CVT_TRY(skip(true));
      if (p_ >= t_.size()) {
        if (close != '\0') return error("unterminated object");
        return {};
      }
      if (close != '\0' && peek() == close) {
        ++p_;
        return {};
      }
      if (peek() == ',') return error("unexpected ','");
      if (t_.substr(p_, 7) == "include" && (peek(7) == ' ' || peek(7) == '\t')) {
        ++includes_;
        while (p_ < t_.size() && peek() != '\n') ++p_;
        continue;
      }
      CVT_TRY_ASSIGN(std::vector<std::string> path, path_until(":={"));
      CVT_TRY(skip(false));
      bool append = false;
      if (peek() == ':' || peek() == '=') {
        ++p_;
      } else if (peek() == '+' && peek(1) == '=') {
        p_ += 2;
        append = true;
      } else if (peek() != '{') {
        return error("expected ':', '=' or '{' after the key");
      }
      CVT_TRY(skip(false));
      CVT_TRY_ASSIGN(Json v, value(depth + 1));
      // Walk / create the path.
      Json* node = &object;
      for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        Json* child = node->find(path[i]);
        if (child == nullptr || !child->is_object() || child->contains(concat_key) || child->contains(ref_key)) {
          node->set(path[i], Json::object());
          child = node->find(path[i]);
        }
        node = child;
      }
      const std::string& last = path.back();
      if (append) {
        Json* existing = node->find(last);
        if (existing != nullptr && existing->is_array()) {
          existing->push_back(std::move(v));
        } else {
          node->set(last, Json::array({std::move(v)}));
        }
      } else if (Json* existing = node->find(last)) {
        merge_into(*existing, std::move(v));
      } else {
        node->set(last, std::move(v));
      }
      CVT_TRY(skip(false));
      if (peek() == ',') ++p_;
    }
  }

  std::string_view t_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
};

class Resolver {
 public:
  explicit Resolver(Json& root) : root_(root) {}

  Result<void> run() {
    for (int pass = 0; pass < 64; ++pass) {
      changed_ = false;
      pending_ = false;
      CVT_TRY(resolve(root_, 0));
      if (!pending_) return {};
      if (!changed_) break;
    }
    return fail(Status::Decode, "HOCON substitutions refer to each other in a cycle");
  }

 private:
  const Json* lookup(std::string_view path) const {
    const Json* node = &root_;
    for (const std::string_view part : util::split(path, '.')) {
      std::string key(util::trim(part));
      if (key.size() >= 2 && key.front() == '"' && key.back() == '"') key = key.substr(1, key.size() - 2);
      node = node->find(key);
      if (node == nullptr) return nullptr;
    }
    return node;
  }

  static bool unresolved(const Json& value) {
    std::vector<const Json*> stack{&value};
    while (!stack.empty()) {
      const Json* node = stack.back();
      stack.pop_back();
      if (node->is_object() && (node->contains(concat_key) || node->contains(ref_key))) return true;
      for (const Json& e : node->elements()) stack.push_back(&e);
      for (const auto& m : node->items()) stack.push_back(&m.value());
    }
    return false;
  }

  /// Resolve one piece; nullopt = "not yet" (depends on something unresolved).
  Result<std::optional<Json>> piece(const Json& value) {
    if (value.is_object() && value.contains(ref_key)) {
      const std::string& path = value[ref_key].as_string();
      const Json* target = lookup(path);
      if (target == nullptr) {
        if (value[optional_key].as_bool()) return std::optional<Json>(Json::object({{"\x01missing", true}}));
        return fail(Status::Decode, "the HOCON substitution ${{{}}} refers to a missing value (the environment is never "
                    "consulted)", path);
      }
      if (unresolved(*target)) return std::optional<Json>();
      return std::optional<Json>(*target);
    }
    if (unresolved(value)) return std::optional<Json>();
    return std::optional<Json>(value);
  }

  Result<void> resolve(Json& node, std::size_t depth) {
    if (depth > 4096) return fail(Status::Limit, "the HOCON data is nested too deeply");
    if (node.is_object() && (node.contains(ref_key) || node.contains(concat_key))) {
      std::vector<Json> parts;
      if (node.contains(ref_key)) {
        parts.push_back(node);
      } else {
        for (const Json& p : node[concat_key].elements()) parts.push_back(p);
      }
      std::vector<Json> values;
      for (const Json& part : parts) {
        CVT_TRY_ASSIGN(auto resolved, piece(part));
        if (!resolved) {
          pending_ = true;
          return {};
        }
        values.push_back(std::move(*resolved));
      }
      // Missing optional substitutions vanish.
      std::erase_if(values, [](const Json& v) { return v.is_object() && v.contains("\x01missing"); });
      Json result;
      if (values.empty()) {
        result = Json();
      } else if (values.size() == 1) {
        result = std::move(values[0]);
      } else if (std::ranges::all_of(values, [](const Json& v) { return v.is_array() || (v.is_string() && util::trim(v.as_string()).empty()); })) {
        result = Json::array();
        for (Json& v : values) {
          for (Json& e : v.elements()) result.push_back(std::move(e));
        }
      } else if (std::ranges::all_of(values, [](const Json& v) { return v.is_object() || (v.is_string() && util::trim(v.as_string()).empty()); })) {
        result = Json::object();
        for (Json& v : values) {
          for (auto& m : v.items()) result.set(m.key(), std::move(m.value()));
        }
      } else {
        std::string text;
        for (const Json& v : values) {
          if (v.is_string()) {
            text += v.as_string();
          } else if (v.is_null()) {
            text += "null";
          } else if (!v.is_structured()) {
            text += util::dump_json(v);
          } else {
            return fail(Status::Decode, "HOCON cannot concatenate an object or array with text");
          }
        }
        result = Json(std::move(text));
      }
      node = std::move(result);
      changed_ = true;
      return {};
    }
    for (Json& element : node.elements()) CVT_TRY(resolve(element, depth + 1));
    for (auto& member : node.items()) CVT_TRY(resolve(member.value(), depth + 1));
    return {};
  }

  Json& root_;
  bool changed_ = false;
  bool pending_ = false;
};

Result<Json> parse_hocon(std::string_view text, ReadContext& ctx) {
  HoconParser parser(text, ctx);
  CVT_TRY_ASSIGN(Json root, parser.parse());
  if (parser.includes_ > 0) {
    ctx.warn(std::format("{} include statement(s) were ignored: a conversion never reads other files.", parser.includes_));
  }
  Resolver resolver(root);
  CVT_TRY(resolver.run());
  return root;
}

}  // namespace

void register_hocon(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "hocon",
      .name = "HOCON",
      .description = "Human-Optimized Config Object Notation (Lightbend Config).",
      .category = Category::Data,
      .extensions = {"hocon", "conf"},
      .mime = "application/hocon",
      .popularity = 12,
      .signatures = {},
      .sniff = [](ByteView data, std::string_view filename) {
        if (has_extension(filename, {"hocon"})) return 75;
        if (!has_extension(filename, {"conf"})) return 0;
        // HOCON .conf files typically use "key { ... }" blocks or ${substitutions}.
        const std::string_view head = text_head(data, 4096);
        return head.find("${") != std::string_view::npos || head.find(" {\n") != std::string_view::npos ? 82 : 0;
      },
  });
  add_codec(registry, {.format_id = "hocon",
                       .read = text_reader(parse_hocon, "HOCON"),
                       .write = nullptr,
                       .read_options = common_read_options(false, false)});
}

}  // namespace cvt::data
