// Convertible core - data family: Hjson (human JSON, version 3).
//
// Reader: comments (#, //, /* */), quoteless keys and strings, '''multi-line
// strings''' with their indentation removed, single or double quotes,
// optional commas, and a root object without braces. A quoteless value is a
// number, true, false or null only when nothing but a separator or comment
// follows it on the line; otherwise the rest of the line is a string.
// Writer: one member per line, quoteless keys and strings wherever they read
// back unchanged, ''' blocks for multi-line text, JSON quotes otherwise.
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

class HjsonParser {
 public:
  HjsonParser(std::string_view text, const ReadContext& ctx) : t_(text), policy_(read_policy(ctx.options)) {}

  Result<Json> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    CVT_TRY(skip());
    if (p_ >= t_.size()) return Json::object();
    if (peek() == '{' || peek() == '[') {
      CVT_TRY_ASSIGN(Json value, value_of(0));
      CVT_TRY(skip());
      if (p_ < t_.size()) return error("unexpected content after the root value");
      return value;
    }
    // Braceless root object, or a single value.
    const std::size_t save = p_;
    if (key_ahead()) return object_body(0, false);
    p_ = save;
    CVT_TRY_ASSIGN(Json value, value_of(0));
    CVT_TRY(skip());
    if (p_ < t_.size()) return error("unexpected content after the root value");
    return value;
  }

 private:
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept { return p_ + ahead < t_.size() ? t_[p_ + ahead] : '\0'; }
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("Hjson", t_, p_, message); }

  Result<void> skip() {
    while (p_ < t_.size()) {
      const char c = peek();
      if (util::is_ascii_space(c)) {
        ++p_;
      } else if (c == '#' || (c == '/' && peek(1) == '/')) {
        while (p_ < t_.size() && peek() != '\n') ++p_;
      } else if (c == '/' && peek(1) == '*') {
        const std::size_t end = t_.find("*/", p_ + 2);
        if (end == std::string_view::npos) return error("unterminated comment");
        p_ = end + 2;
      } else {
        break;
      }
    }
    return {};
  }

  /// Is there "key:" at p_? (used to recognise a braceless root)
  bool key_ahead() {
    const std::size_t save = p_;
    auto key = parse_key();
    const bool ok = key.has_value() && (static_cast<void>(skip_inline()), peek() == ':');
    p_ = save;
    return ok;
  }

  void skip_inline() {
    while (peek() == ' ' || peek() == '\t') ++p_;
  }

  Result<std::string> quoted(char quote) {
    const std::size_t start = p_++;
    std::string out;
    while (true) {
      if (p_ >= t_.size()) {
        p_ = start;
        return error("unterminated string");
      }
      const char c = t_[p_++];
      if (c == quote) return out;
      if (c == '\n') return error("line break in a quoted string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      const char e = peek();
      ++p_;
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\'': out.push_back('\''); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          const auto unit = p_ + 4 <= t_.size() ? util::parse_int(t_.substr(p_, 4), 16) : std::nullopt;
          if (!unit) return error("invalid \\u escape");
          p_ += 4;
          auto cp = static_cast<char32_t>(*unit);
          if (cp >= 0xD800 && cp <= 0xDBFF && t_.substr(p_, 2) == "\\u") {
            const auto low = util::parse_int(t_.substr(p_ + 2, 4), 16);
            if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + static_cast<char32_t>(*low - 0xDC00);
              p_ += 6;
            }
          }
          util::append_utf8(out, util::is_scalar_value(cp) ? cp : util::replacement_character);
          break;
        }
        default: return error("invalid escape");
      }
    }
  }

  Result<std::string> multiline() {
    // Indentation of the opening ''' is removed from every line.
    const std::size_t line_begin = [&] {
      const std::size_t nl = p_ == 0 ? std::string_view::npos : t_.rfind('\n', p_ - 1);
      return nl == std::string_view::npos ? std::size_t{0} : nl + 1;
    }();
    const std::size_t indent = p_ - line_begin;
    p_ += 3;
    const std::size_t end = t_.find("'''", p_);
    if (end == std::string_view::npos) return error("unterminated ''' string");
    std::string_view body = t_.substr(p_, end - p_);
    p_ = end + 3;
    // The line break after the opening and before the closing quotes are not text.
    if (const std::size_t nl = body.find('\n'); nl != std::string_view::npos && util::trim(body.substr(0, nl)).empty()) {
      body.remove_prefix(nl + 1);
    }
    if (const std::size_t last = body.rfind('\n'); last != std::string_view::npos) {
      if (util::trim(body.substr(last + 1)).empty()) body = body.substr(0, last);
    } else if (util::trim(body).empty()) {
      body = {};
    }
    std::string out;
    std::size_t pos = 0;
    while (pos <= body.size() && !body.empty()) {
      std::size_t eol = body.find('\n', pos);
      if (eol == std::string_view::npos) eol = body.size();
      std::string_view line = body.substr(pos, eol - pos);
      std::size_t strip = 0;
      while (strip < indent && strip < line.size() && (line[strip] == ' ' || line[strip] == '\t')) ++strip;
      line.remove_prefix(strip);
      if (line.ends_with('\r')) line.remove_suffix(1);
      out += line;
      if (eol < body.size()) out.push_back('\n');
      pos = eol + 1;
    }
    return out;
  }

  Result<std::string> parse_key() {
    const char c = peek();
    if (c == '"' || c == '\'') return quoted(c);
    const std::size_t start = p_;
    while (p_ < t_.size()) {
      const char d = peek();
      if (d == ':' || util::is_ascii_space(d) || d == ',' || d == '{' || d == '}' || d == '[' || d == ']') break;
      ++p_;
    }
    if (p_ == start) return error("expected a key");
    return std::string(t_.substr(start, p_ - start));
  }

  Result<Json> object_body(std::size_t depth, bool braces) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the Hjson data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    Json object = Json::object();
    while (true) {
      CVT_TRY(skip());
      if (p_ >= t_.size()) {
        if (braces) return error("unterminated object");
        return object;
      }
      if (braces && peek() == '}') {
        ++p_;
        return object;
      }
      const std::size_t key_at = p_;
      CVT_TRY_ASSIGN(std::string key, parse_key());
      skip_inline();
      if (peek() != ':') return error("expected ':' after the key");
      ++p_;
      CVT_TRY(skip());
      CVT_TRY_ASSIGN(Json value, value_of(depth + 1));
      if (auto put = put_member(object, std::move(key), std::move(value), policy_); !put) {
        return syntax_error("Hjson", t_, key_at, put.error().message);
      }
      CVT_TRY(skip());
      if (peek() == ',') ++p_;
    }
  }

  Result<Json> array_body(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the Hjson data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    Json array = Json::array();
    while (true) {
      CVT_TRY(skip());
      if (p_ >= t_.size()) return error("unterminated array");
      if (peek() == ']') {
        ++p_;
        return array;
      }
      CVT_TRY_ASSIGN(Json value, value_of(depth + 1));
      array.push_back(std::move(value));
      CVT_TRY(skip());
      if (peek() == ',') ++p_;
    }
  }

  /// After a literal: only white space, a separator or a comment may follow.
  [[nodiscard]] bool literal_ends(std::size_t at) const {
    while (at < t_.size() && (t_[at] == ' ' || t_[at] == '\t' || t_[at] == '\r')) ++at;
    if (at >= t_.size()) return true;
    const char c = t_[at];
    return c == '\n' || c == ',' || c == '}' || c == ']' || c == '#' ||
           (c == '/' && at + 1 < t_.size() && (t_[at + 1] == '/' || t_[at + 1] == '*'));
  }

  Result<Json> value_of(std::size_t depth) {
    const char c = peek();
    if (c == '{') {
      ++p_;
      return object_body(depth, true);
    }
    if (c == '[') {
      ++p_;
      return array_body(depth);
    }
    if (t_.substr(p_, 3) == "'''") {
      CVT_TRY_ASSIGN(std::string text, multiline());
      return Json(std::move(text));
    }
    if (c == '"' || c == '\'') {
      CVT_TRY_ASSIGN(std::string text, quoted(c));
      return Json(std::move(text));
    }
    if (c == '}' || c == ']' || c == ',' || c == ':') return error(std::format("unexpected '{}'", c));
    // Literal or quoteless string.
    for (const auto& [word, value] : {std::pair<std::string_view, Json>{"true", Json(true)},
                                      {"false", Json(false)}, {"null", Json()}}) {
      if (t_.substr(p_, word.size()) == word && literal_ends(p_ + word.size())) {
        p_ += word.size();
        return value;
      }
    }
    if (util::is_ascii_digit(c) || c == '-') {
      std::size_t end = p_ + 1;
      while (end < t_.size() && (util::is_ascii_digit(t_[end]) || t_[end] == '.' || t_[end] == 'e' || t_[end] == 'E' ||
                                 t_[end] == '+' || t_[end] == '-')) {
        ++end;
      }
      const std::string_view number = t_.substr(p_, end - p_);
      if (literal_ends(end) && decimal_parts(number) && !(number.size() > 1 && number[0] == '0' && util::is_ascii_digit(number[1]))) {
        p_ = end;
        return number_from_literal(number);
      }
    }
    const std::size_t start = p_;
    while (p_ < t_.size() && t_[p_] != '\n') ++p_;
    return Json(std::string(util::trim(t_.substr(start, p_ - start))));
  }

  std::string_view t_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
};

Result<Json> parse_hjson(std::string_view text, ReadContext& ctx) {
  HjsonParser parser(text, ctx);
  return parser.parse();
}

// =========================================================================== writer

bool quoteless_key(std::string_view key) {
  if (key.empty()) return false;
  if (key.find_first_of(",:[]{}\"'#") != std::string_view::npos || key.starts_with("//") || key.starts_with("/*")) return false;
  return std::ranges::none_of(key, [](char c) { return util::is_ascii_space(c) || static_cast<unsigned char>(c) < 0x20; });
}

bool quoteless_value(std::string_view text) {
  if (text.empty() || text != util::trim(text)) return false;
  if (std::ranges::any_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; })) return false;
  const char c = text.front();
  if (std::string_view("{}[],:\"'#").find(c) != std::string_view::npos || text.starts_with("//") || text.starts_with("/*") ||
      text.starts_with("'''")) {
    return false;
  }
  if (util::is_ascii_digit(c) || c == '-' || text.starts_with("true") || text.starts_with("false") || text.starts_with("null")) {
    return false;  // could read back as a literal
  }
  return true;
}

class HjsonWriter {
 public:
  HjsonWriter(int indent, bool sort) : indent_(indent), sort_(sort) {}
  Result<std::string> run(const Json& value) {
    CVT_TRY(write(value, 0));
    out_.push_back('\n');
    return std::move(out_);
  }

 private:
  void newline(std::size_t depth) {
    out_.push_back('\n');
    out_.append(depth * static_cast<std::size_t>(indent_), ' ');
  }

  void string(std::string_view text, std::size_t depth) {
    if (quoteless_value(text)) {
      out_ += text;
      return;
    }
    if (text.find('\n') != std::string_view::npos && text.find("'''") == std::string_view::npos &&
        std::ranges::none_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x20 && c != '\n'; }) &&
        !text.starts_with(' ') && !text.ends_with('\n')) {
      // On its own line, so that the text lines up with the quotes.
      if (out_.ends_with(": ")) out_.pop_back();
      newline(depth + 1);
      out_ += "'''";
      std::size_t pos = 0;
      while (pos <= text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        newline(depth + 1);
        out_ += text.substr(pos, eol - pos);
        pos = eol + 1;
      }
      newline(depth + 1);
      out_ += "'''";
      return;
    }
    append_json_string(out_, text);
  }

  Result<void> write(const Json& value, std::size_t depth) {
    if (tag_kind(value) == TagKind::BigInt || tag_kind(value) == TagKind::Decimal) {
      out_ += tag_text(value);
      return {};
    }
    switch (value.type()) {
      case Json::Type::Null: out_ += "null"; return {};
      case Json::Type::Bool: out_ += value.as_bool() ? "true" : "false"; return {};
      case Json::Type::Int64:
      case Json::Type::UInt64: out_ += format_integer(value); return {};
      case Json::Type::Double: {
        CVT_TRY_ASSIGN(std::string text, write_json_text(value, JsonStyle{.indent = -1}));
        out_ += text;
        return {};
      }
      case Json::Type::String: string(value.as_string(), depth); return {};
      case Json::Type::Array:
        if (value.empty()) {
          out_ += "[]";
          return {};
        }
        out_.push_back('[');
        for (const Json& element : value.elements()) {
          newline(depth + 1);
          CVT_TRY(write(element, depth + 1));
        }
        newline(depth);
        out_.push_back(']');
        return {};
      case Json::Type::Object:
        if (value.empty()) {
          out_ += "{}";
          return {};
        }
        out_.push_back('{');
        for (const Json::Member* member : members_of(value, sort_)) {
          newline(depth + 1);
          if (quoteless_key(member->key())) {
            out_ += member->key();
          } else {
            append_json_string(out_, member->key());
          }
          out_ += ": ";
          CVT_TRY(write(member->value(), depth + 1));
        }
        newline(depth);
        out_.push_back('}');
        return {};
    }
    return {};
  }

  int indent_ = 2;
  bool sort_ = false;
  std::string out_;
};

Result<std::string> write_hjson(const Json& value, WriteContext& ctx) {
  HjsonWriter writer(static_cast<int>(ctx.options.get_int("indent", 2, 1, 16)), ctx.options.get_bool("sort_keys", false));
  return writer.run(value);
}

}  // namespace

void register_hjson(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "hjson",
      .name = "Hjson",
      .description = "Human JSON: a relaxed JSON syntax for configuration files.",
      .category = Category::Data,
      .extensions = {"hjson"},
      .mime = "application/hjson",
      .popularity = 12,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"hjson"}) ? 75 : 0; },
  });
  add_codec(registry, {.format_id = "hjson",
                       .read = text_reader(parse_hjson, "Hjson"),
                       .write = text_writer(write_hjson, "Hjson"),
                       .read_options = common_read_options(),
                       .write_options = {indent_option(2), sort_keys_option()}});
}

}  // namespace cvt::data
