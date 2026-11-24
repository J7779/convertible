// Convertible core - data family: RON (Rusty Object Notation), read only.
//
// Structs with or without a name (Point(x: 1, y: 2) -> {"x": 1, "y": 2}),
// tuples and tuple structs (-> arrays; a named one-field tuple such as
// Some(5) is its value), unit and enum variants (Red -> "Red", None -> null,
// Variant(1, 2) -> {"Variant": [1, 2]} as serde's externally tagged form),
// lists, maps with any key type (keys become text), strings including raw
// strings r#"..."#, chars, byte strings b"..." -> $binary, integers in every
// base, floats (inf, NaN), booleans, comments and #![enable(...)] attributes.
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

class RonParser {
 public:
  RonParser(std::string_view text, const ReadContext& ctx) : t_(text), policy_(read_policy(ctx.options)) {}

  Result<Json> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    CVT_TRY(skip());
    while (t_.substr(p_, 2) == "#!") {  // #![enable(...)]
      const std::size_t end = t_.find(']', p_);
      if (end == std::string_view::npos) return error("unterminated attribute");
      p_ = end + 1;
      CVT_TRY(skip());
    }
    CVT_TRY_ASSIGN(Json value, value_of(0));
    CVT_TRY(skip());
    if (p_ != t_.size()) return error("unexpected content after the value");
    return value;
  }

 private:
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept { return p_ + ahead < t_.size() ? t_[p_ + ahead] : '\0'; }
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("RON", t_, p_, message); }

  Result<void> skip() {
    while (p_ < t_.size()) {
      const char c = peek();
      if (util::is_ascii_space(c)) {
        ++p_;
      } else if (c == '/' && peek(1) == '/') {
        while (p_ < t_.size() && peek() != '\n') ++p_;
      } else if (c == '/' && peek(1) == '*') {
        int nesting = 0;
        while (p_ < t_.size()) {
          if (t_.substr(p_, 2) == "/*") {
            ++nesting;
            p_ += 2;
          } else if (t_.substr(p_, 2) == "*/") {
            p_ += 2;
            if (--nesting == 0) break;
          } else {
            ++p_;
          }
        }
        if (nesting != 0) return error("unterminated comment");
      } else {
        break;
      }
    }
    return {};
  }

  [[nodiscard]] static bool ident_start(char c) { return util::is_ascii_alpha(c) || c == '_'; }
  [[nodiscard]] static bool ident_char(char c) { return ident_start(c) || util::is_ascii_digit(c); }

  std::string identifier() {
    if (t_.substr(p_, 2) == "r#") p_ += 2;  // raw identifier
    const std::size_t start = p_;
    while (p_ < t_.size() && ident_char(peek())) ++p_;
    return std::string(t_.substr(start, p_ - start));
  }

  Result<std::string> string_literal(bool bytes) {
    if (peek() == 'r') {  // raw string r#"..."#
      ++p_;
      std::size_t hashes = 0;
      while (peek() == '#') {
        ++hashes;
        ++p_;
      }
      if (peek() != '"') return error("expected '\"' in a raw string");
      ++p_;
      const std::string close = "\"" + std::string(hashes, '#');
      const std::size_t end = t_.find(close, p_);
      if (end == std::string_view::npos) return error("unterminated raw string");
      std::string text(t_.substr(p_, end - p_));
      p_ = end + close.size();
      return text;
    }
    ++p_;  // "
    std::string out;
    while (true) {
      if (p_ >= t_.size()) return error("unterminated string");
      const char c = t_[p_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      const char e = peek();
      ++p_;
      switch (e) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '0': out.push_back('\0'); break;
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        case '\'': out.push_back('\''); break;
        case '\n':
          while (p_ < t_.size() && util::is_ascii_space(peek())) ++p_;
          break;
        case 'x': {
          const auto v = p_ + 2 <= t_.size() ? util::parse_int(t_.substr(p_, 2), 16) : std::nullopt;
          if (!v) return error("invalid \\x escape");
          p_ += 2;
          if (bytes) {
            out.push_back(static_cast<char>(*v));
          } else {
            util::append_utf8(out, static_cast<char32_t>(*v));
          }
          break;
        }
        case 'u': {
          if (peek() != '{') return error("invalid \\u escape");
          const std::size_t close = t_.find('}', p_);
          if (close == std::string_view::npos) return error("invalid \\u escape");
          const auto v = util::parse_int(t_.substr(p_ + 1, close - p_ - 1), 16);
          if (!v || *v < 0 || !util::is_scalar_value(static_cast<char32_t>(*v))) return error("invalid \\u escape");
          util::append_utf8(out, static_cast<char32_t>(*v));
          p_ = close + 1;
          break;
        }
        default: return error("invalid escape");
      }
    }
  }

  Result<Json> number() {
    const std::size_t start = p_;
    bool negative = false;
    if (peek() == '-' || peek() == '+') {
      negative = peek() == '-';
      ++p_;
    }
    if (t_.substr(p_, 3) == "inf") {
      p_ += 3;
      return Json(negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
    }
    if (t_.substr(p_, 3) == "NaN") {
      p_ += 3;
      return Json(std::numeric_limits<double>::quiet_NaN());
    }
    const std::size_t body = p_;
    while (p_ < t_.size() && (util::is_ascii_alpha(peek()) || util::is_ascii_digit(peek()) || peek() == '_' || peek() == '.' ||
                              ((peek() == '+' || peek() == '-') && (t_[p_ - 1] == 'e' || t_[p_ - 1] == 'E')))) {
      ++p_;
    }
    std::string text;
    for (const char c : t_.substr(body, p_ - body)) {
      if (c != '_') text.push_back(c);
    }
    // Type suffixes such as 5u8 or 1.5f32.
    for (const std::string_view suffix : {"u8", "u16", "u32", "u64", "u128", "i8", "i16", "i32", "i64", "i128", "f32", "f64"}) {
      if (text.ends_with(suffix) && text.size() > suffix.size()) {
        text.resize(text.size() - suffix.size());
        break;
      }
    }
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'o' || text[1] == 'b')) {
      auto value = integer_from_digits(std::string_view(text).substr(2), text[1] == 'x' ? 16 : text[1] == 'o' ? 8 : 2, negative);
      if (!value) {
        p_ = start;
        return error("invalid number");
      }
      return std::move(*value);
    }
    if (!decimal_parts(text)) {
      p_ = start;
      return error("invalid number");
    }
    Json value = number_from_literal((negative ? "-" : "") + text);
    if (text.find_first_of(".eE") != std::string::npos && value.is_integer()) value = Json(value.as_double());
    return value;
  }

  /// "( ... )" after an optional name: struct fields or tuple elements.
  Result<Json> parenthesised(const std::string& name, std::size_t depth) {
    ++p_;  // (
    CVT_TRY(skip());
    // Struct fields look like "ident :".
    const std::size_t save = p_;
    bool fields = false;
    if (ident_start(peek()) || t_.substr(p_, 2) == "r#") {
      (void)identifier();
      CVT_TRY(skip());
      fields = peek() == ':' && peek(1) != ':';
    }
    p_ = save;
    if (fields) {
      Json object = Json::object();
      while (true) {
        CVT_TRY(skip());
        if (peek() == ')') {
          ++p_;
          break;
        }
        const std::string field = identifier();
        if (field.empty()) return error("expected a field name");
        CVT_TRY(skip());
        if (peek() != ':') return error("expected ':' after a field name");
        ++p_;
        CVT_TRY_ASSIGN(Json v, value_of(depth + 1));
        CVT_TRY(put_member(object, field, std::move(v), policy_));
        CVT_TRY(skip());
        if (peek() == ',') {
          ++p_;
          continue;
        }
        if (peek() != ')') return error("expected ',' or ')'");
      }
      return object;
    }
    Json items = Json::array();
    while (true) {
      CVT_TRY(skip());
      if (peek() == ')') {
        ++p_;
        break;
      }
      CVT_TRY_ASSIGN(Json v, value_of(depth + 1));
      items.push_back(std::move(v));
      CVT_TRY(skip());
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() != ')') return error("expected ',' or ')'");
    }
    if (name.empty()) return items.empty() ? Json() : items;
    if (name == "Some" && items.size() == 1) return std::move(items[0]);
    if (items.size() == 1) return Json::object({{name, std::move(items[0])}});
    return Json::object({{name, std::move(items)}});
  }

  Result<Json> value_of(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the RON data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    CVT_TRY(skip());
    const char c = peek();
    if (p_ >= t_.size()) return error("expected a value");
    if (c == '"' || (c == 'r' && (peek(1) == '"' || (peek(1) == '#' && (peek(2) == '"' || peek(2) == '#'))))) {
      CVT_TRY_ASSIGN(std::string text, string_literal(false));
      return Json(std::move(text));
    }
    if (c == 'b' && (peek(1) == '"' || (peek(1) == 'r' && (peek(2) == '"' || peek(2) == '#')))) {
      ++p_;
      CVT_TRY_ASSIGN(std::string bytes, string_literal(true));
      return make_binary(as_bytes(bytes));
    }
    if (c == '\'') {
      ++p_;
      std::string out;
      if (peek() == '\\') {
        ++p_;
        const char e = peek();
        ++p_;
        out.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == '0' ? '\0' : e);
      } else {
        const util::Utf8Decoded d = util::decode_utf8(t_, p_);
        out.append(t_.substr(p_, d.length));
        p_ += d.length;
      }
      if (peek() != '\'') return error("unterminated character");
      ++p_;
      return Json(std::move(out));
    }
    if (c == '[') {
      ++p_;
      Json list = Json::array();
      while (true) {
        CVT_TRY(skip());
        if (peek() == ']') {
          ++p_;
          return list;
        }
        CVT_TRY_ASSIGN(Json v, value_of(depth + 1));
        list.push_back(std::move(v));
        CVT_TRY(skip());
        if (peek() == ',') {
          ++p_;
          continue;
        }
        if (peek() != ']') return error("expected ',' or ']'");
      }
    }
    if (c == '{') {
      ++p_;
      Json map = Json::object();
      while (true) {
        CVT_TRY(skip());
        if (peek() == '}') {
          ++p_;
          return map;
        }
        CVT_TRY_ASSIGN(Json key, value_of(depth + 1));
        CVT_TRY(skip());
        if (peek() != ':') return error("expected ':' in a map");
        ++p_;
        CVT_TRY_ASSIGN(Json v, value_of(depth + 1));
        CVT_TRY(put_member(map, key_from_value(key), std::move(v), policy_));
        CVT_TRY(skip());
        if (peek() == ',') {
          ++p_;
          continue;
        }
        if (peek() != '}') return error("expected ',' or '}'");
      }
    }
    if (c == '(') return parenthesised("", depth);
    if (util::is_ascii_digit(c) || c == '-' || c == '+' || c == '.') return number();
    if (ident_start(c)) {
      const std::size_t start = p_;
      const std::string name = identifier();
      if (name == "true") return Json(true);
      if (name == "false") return Json(false);
      if (name == "None") return Json();
      if (name == "inf" || name == "NaN") {
        p_ = start;
        return number();
      }
      CVT_TRY(skip());
      if (peek() == '(') return parenthesised(name, depth);
      return Json(name);  // unit struct or unit enum variant
    }
    return error(std::format("unexpected '{}'", c));
  }

  std::string_view t_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
};

Result<Json> parse_ron(std::string_view text, ReadContext& ctx) {
  RonParser parser(text, ctx);
  return parser.parse();
}

}  // namespace

void register_ron(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "ron",
      .name = "RON",
      .description = "Rusty Object Notation, a data format for Rust programs.",
      .category = Category::Data,
      .extensions = {"ron"},
      .mime = "application/ron",
      .popularity = 8,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"ron"}) ? 75 : 0; },
  });
  add_codec(registry, {.format_id = "ron",
                       .read = text_reader(parse_ron, "RON"),
                       .write = nullptr,
                       .read_options = common_read_options(true, false),
                       .lossy = true});
}

}  // namespace cvt::data
