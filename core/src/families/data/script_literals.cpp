// Convertible core - data family: data written as program literals.
//
//   lua-table       Lua table constructors ({ key = value, [1] = x, "positional" }),
//                   optionally after `return` or `name =`; strings in all
//                   quotings incl. [[long brackets]], numbers, booleans, nil.
//                   Tables whose keys are 1..n become arrays.
//   php-serialized  PHP serialize(): N; b: i: d: s: a: O: (objects become
//                   their properties, private/protected name prefixes are
//                   dropped), E: enums, and R:/r: references. Arrays with keys
//                   0..n-1 become arrays. The writer produces arrays only.
//   python-literal  Python literals as ast.literal_eval accepts them: dict,
//                   list, tuple, set, str (all quotes and prefixes), bytes ->
//                   $binary, int (any size), float incl. inf/nan, True, False,
//                   None, adjacent string concatenation. The writer produces
//                   repr()-style text that literal_eval reads back.
// Nothing is ever evaluated: these are parsers of literal syntax only.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace {

/// Objects whose keys are exactly "0".."n-1" or "1".."n" (in order) -> arrays.
Json arrays_from_indexed(Json object, std::size_t first) {
  if (!object.is_object() || object.empty()) return object;
  std::size_t expected = first;
  for (const auto& member : object.items()) {
    if (member.key() != std::to_string(expected)) return object;
    ++expected;
  }
  Json array = Json::array();
  for (auto& member : object.items()) array.push_back(std::move(member.value()));
  return array;
}

// =========================================================================== Lua

class LuaParser {
 public:
  LuaParser(std::string_view text, const ReadContext& ctx) : t_(text), policy_(read_policy(ctx.options)) {}

  Result<Json> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    if (t_.substr(p_).starts_with("#!")) {
      while (p_ < t_.size() && t_[p_] != '\n') ++p_;
    }
    CVT_TRY(skip());
    // Optional "return" or "local name =" / "name =".
    const std::size_t save = p_;
    if (word("return")) {
      CVT_TRY(skip());
    } else {
      p_ = save;
      if (word("local")) CVT_TRY(skip());
      const std::size_t name_start = p_;
      while (p_ < t_.size() && (util::is_ascii_alpha(t_[p_]) || util::is_ascii_digit(t_[p_]) || t_[p_] == '_' || t_[p_] == '.')) ++p_;
      CVT_TRY(skip());
      if (p_ > name_start && p_ < t_.size() && t_[p_] == '=' && (p_ + 1 >= t_.size() || t_[p_ + 1] != '=')) {
        ++p_;
        CVT_TRY(skip());
      } else {
        p_ = save;
      }
    }
    CVT_TRY_ASSIGN(Json value, expression(0));
    CVT_TRY(skip());
    if (p_ < t_.size() && t_[p_] == ';') ++p_;
    CVT_TRY(skip());
    if (p_ < t_.size()) return error("unexpected content after the value");
    return value;
  }

 private:
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("Lua", t_, p_, message); }

  bool word(std::string_view w) {
    if (t_.substr(p_, w.size()) != w) return false;
    const std::size_t end = p_ + w.size();
    if (end < t_.size() && (util::is_ascii_alpha(t_[end]) || util::is_ascii_digit(t_[end]) || t_[end] == '_')) return false;
    p_ = end;
    return true;
  }

  /// Level of a long bracket "[==[" at p_, or -1.
  [[nodiscard]] int long_bracket() const {
    if (p_ >= t_.size() || t_[p_] != '[') return -1;
    std::size_t i = p_ + 1;
    int level = 0;
    while (i < t_.size() && t_[i] == '=') {
      ++i;
      ++level;
    }
    return i < t_.size() && t_[i] == '[' ? level : -1;
  }

  Result<std::string> long_string(int level) {
    p_ += static_cast<std::size_t>(level) + 2;
    const std::string close = "]" + std::string(static_cast<std::size_t>(level), '=') + "]";
    const std::size_t end = t_.find(close, p_);
    if (end == std::string_view::npos) return error("unterminated long string");
    std::string_view body = t_.substr(p_, end - p_);
    if (body.starts_with("\r\n")) {
      body.remove_prefix(2);
    } else if (body.starts_with('\n')) {
      body.remove_prefix(1);
    }
    p_ = end + close.size();
    return std::string(body);
  }

  Result<void> skip() {
    while (p_ < t_.size()) {
      const char c = t_[p_];
      if (util::is_ascii_space(c)) {
        ++p_;
      } else if (c == '-' && p_ + 1 < t_.size() && t_[p_ + 1] == '-') {
        p_ += 2;
        const int level = long_bracket();
        if (level >= 0) {
          CVT_TRY(long_string(level));
        } else {
          while (p_ < t_.size() && t_[p_] != '\n') ++p_;
        }
      } else {
        break;
      }
    }
    return {};
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
      if (c == '\n') return error("line break in a string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (p_ >= t_.size()) return error("unterminated string");
      const char e = t_[p_++];
      switch (e) {
        case 'a': out.push_back('\a'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'v': out.push_back('\v'); break;
        case '\n': out.push_back('\n'); break;
        case 'z':
          while (p_ < t_.size() && util::is_ascii_space(t_[p_])) ++p_;
          break;
        case 'x': {
          const auto value = p_ + 2 <= t_.size() ? util::parse_int(t_.substr(p_, 2), 16) : std::nullopt;
          if (!value) return error("invalid \\x escape");
          out.push_back(static_cast<char>(*value));
          p_ += 2;
          break;
        }
        case 'u': {
          const std::size_t close = t_.find('}', p_);
          if (p_ >= t_.size() || t_[p_] != '{' || close == std::string_view::npos) return error("invalid \\u escape");
          const auto value = util::parse_int(t_.substr(p_ + 1, close - p_ - 1), 16);
          if (!value || *value < 0 || *value > 0x10FFFF) return error("invalid \\u escape");
          util::append_utf8(out, util::is_scalar_value(static_cast<char32_t>(*value)) ? static_cast<char32_t>(*value)
                                                                                      : util::replacement_character);
          p_ = close + 1;
          break;
        }
        default:
          if (util::is_ascii_digit(e)) {
            int value = e - '0';
            for (int k = 0; k < 2 && p_ < t_.size() && util::is_ascii_digit(t_[p_]); ++k) value = value * 10 + (t_[p_++] - '0');
            if (value > 255) return error("decimal escape too large");
            out.push_back(static_cast<char>(value));
          } else {
            out.push_back(e);
          }
      }
    }
  }

  Result<Json> number() {
    const std::size_t start = p_;
    bool negative = false;
    if (t_[p_] == '-') {
      negative = true;
      ++p_;
      CVT_TRY(skip());
    }
    const std::size_t body = p_;
    if (t_.substr(p_, 2) == "0x" || t_.substr(p_, 2) == "0X") {
      p_ += 2;
      const std::size_t digits = p_;
      while (p_ < t_.size() && std::string_view("0123456789abcdefABCDEF").find(t_[p_]) != std::string_view::npos) ++p_;
      if (p_ == digits) return error("invalid hexadecimal number");
      auto value = integer_from_digits(t_.substr(digits, p_ - digits), 16, negative);
      if (!value) return error("invalid number");
      return std::move(*value);
    }
    while (p_ < t_.size() && (util::is_ascii_digit(t_[p_]) || t_[p_] == '.' || t_[p_] == 'e' || t_[p_] == 'E' ||
                              ((t_[p_] == '+' || t_[p_] == '-') && (t_[p_ - 1] == 'e' || t_[p_ - 1] == 'E')))) {
      ++p_;
    }
    std::string text = (negative ? "-" : "") + std::string(t_.substr(body, p_ - body));
    if (text.ends_with('.')) text += "0";
    if (!text.empty() && (text.front() == '.' || text.starts_with("-."))) text.insert(text.front() == '-' ? 1 : 0, "0");
    if (!decimal_parts(text)) {
      p_ = start;
      return error("invalid number");
    }
    return number_from_literal(text);
  }

  Result<Json> expression(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the Lua table is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    CVT_TRY(skip());
    if (p_ >= t_.size()) return error("expected a value");
    const char c = t_[p_];
    if (c == '{') return table(depth);
    if (c == '"' || c == '\'') {
      CVT_TRY_ASSIGN(std::string text, quoted(c));
      return string_value(std::move(text));
    }
    if (const int level = long_bracket(); level >= 0) {
      CVT_TRY_ASSIGN(std::string text, long_string(level));
      return string_value(std::move(text));
    }
    if (word("true")) return Json(true);
    if (word("false")) return Json(false);
    if (word("nil")) return Json();
    if (word("math.huge")) return Json(std::numeric_limits<double>::infinity());
    if (c == '-' && t_.substr(p_ + 1).starts_with("math.huge")) {
      p_ += 10;
      return Json(-std::numeric_limits<double>::infinity());
    }
    if (t_.substr(p_).starts_with("0/0") || t_.substr(p_).starts_with("(0/0)")) {
      p_ += t_[p_] == '(' ? 5 : 3;
      return Json(std::numeric_limits<double>::quiet_NaN());
    }
    if (util::is_ascii_digit(c) || c == '-' || c == '.') return number();
    return error(std::format("unexpected '{}'", c));
  }

  Json string_value(std::string text) {
    if (util::is_valid_utf8(text)) return Json(std::move(text));
    return make_binary(as_bytes(text));
  }

  Result<Json> table(std::size_t depth) {
    ++p_;  // {
    Json object = Json::object();
    std::int64_t next_index = 1;
    bool all_positional = true;
    while (true) {
      CVT_TRY(skip());
      if (p_ >= t_.size()) return error("unterminated table");
      if (t_[p_] == '}') {
        ++p_;
        break;
      }
      std::string key;
      Json value;
      const std::size_t entry = p_;
      if (t_[p_] == '[' && long_bracket() < 0) {
        ++p_;
        CVT_TRY_ASSIGN(Json key_value, expression(depth + 1));
        CVT_TRY(skip());
        if (p_ >= t_.size() || t_[p_] != ']') return error("expected ']' after a table key");
        ++p_;
        CVT_TRY(skip());
        if (p_ >= t_.size() || t_[p_] != '=') return error("expected '=' after a table key");
        ++p_;
        key = key_from_value(key_value);
        all_positional = false;
        CVT_TRY_ASSIGN(value, expression(depth + 1));
      } else {
        // name = value, or a positional value.
        std::size_t i = p_;
        while (i < t_.size() && (util::is_ascii_alpha(t_[i]) || t_[i] == '_' || (i > p_ && util::is_ascii_digit(t_[i])))) ++i;
        std::size_t j = i;
        while (j < t_.size() && util::is_ascii_space(t_[j])) ++j;
        if (i > p_ && j < t_.size() && t_[j] == '=' && (j + 1 >= t_.size() || t_[j + 1] != '=')) {
          key = std::string(t_.substr(p_, i - p_));
          p_ = j + 1;
          all_positional = false;
          CVT_TRY_ASSIGN(value, expression(depth + 1));
        } else {
          CVT_TRY_ASSIGN(value, expression(depth + 1));
          key = std::to_string(next_index++);
        }
      }
      if (auto put = put_member(object, std::move(key), std::move(value), policy_); !put) {
        return syntax_error("Lua", t_, entry, put.error().message);
      }
      CVT_TRY(skip());
      if (p_ < t_.size() && (t_[p_] == ',' || t_[p_] == ';')) {
        ++p_;
        continue;
      }
      if (p_ < t_.size() && t_[p_] == '}') continue;
      return error("expected ',' or '}' in a table");
    }
    (void)all_positional;
    return arrays_from_indexed(std::move(object), 1);
  }

  std::string_view t_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
};

Result<Json> parse_lua(std::string_view text, ReadContext& ctx) {
  LuaParser parser(text, ctx);
  return parser.parse();
}

bool lua_identifier(std::string_view name) {
  static constexpr std::string_view reserved[] = {"and", "break", "do", "else", "elseif", "end", "false", "for",
                                                  "function", "goto", "if", "in", "local", "nil", "not", "or",
                                                  "repeat", "return", "then", "true", "until", "while"};
  if (name.empty() || util::is_ascii_digit(name[0]) || std::ranges::find(reserved, name) != std::end(reserved)) return false;
  return std::ranges::all_of(name, [](char c) { return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_'; });
}

void lua_string(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
          out += std::format("\\{:03}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

class LuaWriter {
 public:
  LuaWriter(int indent, bool sort) : indent_(indent), sort_(sort) {}
  std::string run(const Json& value) {
    out_ = "return ";
    write(value, 0);
    out_.push_back('\n');
    return std::move(out_);
  }
  std::size_t nulls_ = 0;

 private:
  void newline(std::size_t depth) {
    out_.push_back('\n');
    out_.append(depth * static_cast<std::size_t>(indent_), ' ');
  }
  void number(double d) {
    if (std::isnan(d)) {
      out_ += "(0/0)";
    } else if (std::isinf(d)) {
      out_ += d < 0 ? "-math.huge" : "math.huge";
    } else {
      out_ += format_double(d);
    }
  }
  void write(const Json& value, std::size_t depth) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary: {
        const auto bytes = tagged_bytes(value);
        lua_string(out_, bytes ? as_chars(*bytes) : std::string_view());
        return;
      }
      case TagKind::BigInt:
      case TagKind::Decimal: out_ += tag_text(value); return;
      case TagKind::Float: number(plain_value(value).as_double()); return;
      default: {
        const Json plain = plain_value(value);
        if (!plain.is_structured()) {
          write(plain, depth);
          return;
        }
        break;
      }
    }
    switch (value.type()) {
      case Json::Type::Null:
        ++nulls_;
        out_ += "nil";
        return;
      case Json::Type::Bool: out_ += value.as_bool() ? "true" : "false"; return;
      case Json::Type::Int64:
      case Json::Type::UInt64: out_ += format_integer(value); return;
      case Json::Type::Double: number(value.as_double()); return;
      case Json::Type::String: lua_string(out_, value.as_string()); return;
      case Json::Type::Array:
        if (value.empty()) {
          out_ += "{}";
          return;
        }
        out_.push_back('{');
        for (const Json& element : value.elements()) {
          newline(depth + 1);
          write(element, depth + 1);
          out_.push_back(',');
        }
        newline(depth);
        out_.push_back('}');
        return;
      case Json::Type::Object:
        if (value.empty()) {
          out_ += "{}";
          return;
        }
        out_.push_back('{');
        for (const Json::Member* member : members_of(value, sort_)) {
          newline(depth + 1);
          if (lua_identifier(member->key())) {
            out_ += member->key();
          } else {
            out_.push_back('[');
            lua_string(out_, member->key());
            out_.push_back(']');
          }
          out_ += " = ";
          write(member->value(), depth + 1);
          out_.push_back(',');
        }
        newline(depth);
        out_.push_back('}');
        return;
    }
  }
  int indent_ = 2;
  bool sort_ = false;
  std::string out_;
};

Result<std::string> write_lua(const Json& value, WriteContext& ctx) {
  LuaWriter writer(static_cast<int>(ctx.options.get_int("indent", 2, 0, 16)), ctx.options.get_bool("sort_keys", false));
  std::string out = writer.run(value);
  if (writer.nulls_ > 0) ctx.warn("Lua tables drop nil values: null members disappear when the table is loaded.");
  return out;
}

// =========================================================================== PHP serialize

class PhpParser {
 public:
  PhpParser(std::string_view text, const ReadContext& ctx)
      : t_(text), policy_(read_policy(ctx.options)), budget_(text.size()) {}

  Result<Json> parse() {
    CVT_TRY_ASSIGN(Json value, value_of(0));
    while (p_ < t_.size() && util::is_ascii_space(t_[p_])) ++p_;
    if (p_ != t_.size()) return error("unexpected data after the value");
    return value;
  }

  std::size_t objects_ = 0;

 private:
  std::unexpected<Error> error(std::string_view message) const {
    return fail(Status::Decode, "invalid PHP serialized data at byte {}: {}", p_, message);
  }

  Result<void> expect(char c) {
    if (p_ >= t_.size() || t_[p_] != c) return error(std::format("expected '{}'", c));
    ++p_;
    return {};
  }

  Result<std::string_view> until(char c) {
    const std::size_t end = t_.find(c, p_);
    if (end == std::string_view::npos) return error(std::format("expected '{}'", c));
    const std::string_view text = t_.substr(p_, end - p_);
    p_ = end + 1;
    return text;
  }

  Result<std::uint64_t> count(char terminator) {
    CVT_TRY_ASSIGN(const std::string_view digits, until(terminator));
    const auto n = util::parse_int(digits);
    if (!n || *n < 0) return error("invalid length");
    return static_cast<std::uint64_t>(*n);
  }

  Result<std::string> bytes_string() {
    CVT_TRY_ASSIGN(const std::uint64_t size, count(':'));
    CVT_TRY(expect('"'));
    if (size > t_.size() - p_) return error("string length runs past the end");
    std::string text(t_.substr(p_, static_cast<std::size_t>(size)));
    p_ += static_cast<std::size_t>(size);
    CVT_TRY(expect('"'));
    return text;
  }

  Json text_or_binary(std::string text) {
    if (util::is_valid_utf8(text)) return Json(std::move(text));
    return make_binary(as_bytes(text));
  }

  Result<Json> members(std::uint64_t n, std::size_t depth, bool object) {
    CVT_TRY(expect('{'));
    if (n > t_.size() - p_) return error("member count runs past the end");
    CVT_TRY(budget_.take(n));
    Json out = Json::object();
    for (std::uint64_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(Json key, value_of(depth + 1, false));
      CVT_TRY_ASSIGN(Json value, value_of(depth + 1));
      std::string name = key_from_value(key);
      if (object && name.starts_with('\0')) {
        // "\0Class\0name" (private) or "\0*\0name" (protected).
        const std::size_t second = name.find('\0', 1);
        name = second == std::string::npos ? name.substr(1) : name.substr(second + 1);
      }
      CVT_TRY(put_member(out, std::move(name), std::move(value), policy_));
    }
    CVT_TRY(expect('}'));
    return object ? out : arrays_from_indexed(std::move(out), 0);
  }

  Result<Json> value_of(std::size_t depth, bool record = true) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the PHP data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    if (p_ + 2 > t_.size()) return error("unexpected end of data");
    const char type = t_[p_];
    p_ += 1;
    Json value;
    switch (type) {
      case 'N': CVT_TRY(expect(';')); value = Json(); break;
      case 'b': {
        CVT_TRY(expect(':'));
        CVT_TRY_ASSIGN(const std::string_view text, until(';'));
        if (text != "0" && text != "1") return error("invalid boolean");
        value = Json(text == "1");
        break;
      }
      case 'i': {
        CVT_TRY(expect(':'));
        CVT_TRY_ASSIGN(const std::string_view text, until(';'));
        if (!decimal_parts(text) || text.find_first_of(".eE") != std::string_view::npos) return error("invalid integer");
        value = number_from_literal(text);
        break;
      }
      case 'd': {
        CVT_TRY(expect(':'));
        CVT_TRY_ASSIGN(const std::string_view text, until(';'));
        if (text == "NAN") {
          value = Json(std::numeric_limits<double>::quiet_NaN());
        } else if (text == "INF" || text == "-INF") {
          value = Json(text == "INF" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity());
        } else {
          if (!decimal_parts(text)) return error("invalid float");
          value = number_from_literal(text);
          if (value.is_integer()) value = Json(value.as_double());
        }
        break;
      }
      case 's':
      case 'S': {
        CVT_TRY(expect(':'));
        CVT_TRY_ASSIGN(std::string text, bytes_string());
        CVT_TRY(expect(';'));
        value = text_or_binary(std::move(text));
        break;
      }
      case 'a': {
        CVT_TRY(expect(':'));
        CVT_TRY_ASSIGN(const std::uint64_t n, count(':'));
        const std::size_t slot = values_.size();
        if (record) values_.emplace_back();
        CVT_TRY_ASSIGN(value, members(n, depth, false));
        if (record) values_[slot] = value;
        return value;
      }
      case 'O':
      case 'C': {
        CVT_TRY(expect(':'));
        CVT_TRY_ASSIGN(const std::string class_name, bytes_string());
        (void)class_name;
        CVT_TRY(expect(':'));
        ++objects_;
        if (type == 'C') {  // custom serialisation: raw payload
          CVT_TRY_ASSIGN(const std::uint64_t size, count(':'));
          CVT_TRY(expect('{'));
          if (size > t_.size() - p_) return error("payload runs past the end");
          value = text_or_binary(std::string(t_.substr(p_, static_cast<std::size_t>(size))));
          p_ += static_cast<std::size_t>(size);
          CVT_TRY(expect('}'));
          break;
        }
        CVT_TRY_ASSIGN(const std::uint64_t n, count(':'));
        const std::size_t slot = values_.size();
        values_.emplace_back();
        CVT_TRY_ASSIGN(value, members(n, depth, true));
        values_[slot] = value;
        return value;
      }
      case 'E': {
        CVT_TRY(expect(':'));
        CVT_TRY_ASSIGN(std::string text, bytes_string());
        CVT_TRY(expect(';'));
        value = text_or_binary(std::move(text));
        break;
      }
      case 'R':
      case 'r': {
        CVT_TRY(expect(':'));
        CVT_TRY_ASSIGN(const std::uint64_t index, count(';'));
        if (index == 0 || index > values_.size()) return error("reference out of range");
        CVT_TRY(budget_.take(16));
        value = values_[static_cast<std::size_t>(index - 1)];
        break;
      }
      default: return error(std::format("unknown type '{}'", type));
    }
    if (record) values_.push_back(value);
    return value;
  }

  std::string_view t_;
  ReadPolicy policy_;
  NodeBudget budget_;
  std::size_t p_ = 0;
  std::vector<Json> values_;
};

Result<Json> parse_php(std::string_view text, ReadContext& ctx) {
  PhpParser parser(text, ctx);
  CVT_TRY_ASSIGN(Json value, parser.parse());
  if (parser.objects_ > 0) ctx.warn(std::format("{} PHP object(s) were read as their properties (class names dropped).", parser.objects_));
  return value;
}

class PhpWriter {
 public:
  std::string run(const Json& value) {
    write(value);
    return std::move(out_);
  }
  std::size_t converted_ = 0;

 private:
  void string(std::string_view text) { out_ += std::format("s:{}:\"{}\";", text.size(), text); }
  void key(std::string_view k) {
    // Integer-looking keys become integer keys, as PHP itself does.
    if (const auto n = util::parse_int(k); n && std::to_string(*n) == k) {
      out_ += std::format("i:{};", *n);
    } else {
      string(k);
    }
  }
  void number(double d) {
    if (std::isnan(d)) {
      out_ += "d:NAN;";
    } else if (std::isinf(d)) {
      out_ += d < 0 ? "d:-INF;" : "d:INF;";
    } else {
      std::string text = util::format_double(d);
      out_ += "d:" + text + ";";
    }
  }
  void write(const Json& value) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary: {
        const auto bytes = tagged_bytes(value);
        string(bytes ? as_chars(*bytes) : std::string_view());
        return;
      }
      case TagKind::BigInt:
        ++converted_;
        string(tag_text(value));
        return;
      case TagKind::Decimal:
        out_ += "d:" + std::string(tag_text(value)) + ";";
        return;
      case TagKind::Float: number(plain_value(value).as_double()); return;
      default: {
        const Json plain = plain_value(value);
        if (!plain.is_structured()) {
          write(plain);
          return;
        }
        break;
      }
    }
    switch (value.type()) {
      case Json::Type::Null: out_ += "N;"; return;
      case Json::Type::Bool: out_ += value.as_bool() ? "b:1;" : "b:0;"; return;
      case Json::Type::Int64: out_ += std::format("i:{};", value.as_int64()); return;
      case Json::Type::UInt64:
        ++converted_;
        string(format_integer(value));
        return;
      case Json::Type::Double: number(value.as_double()); return;
      case Json::Type::String: string(value.as_string()); return;
      case Json::Type::Array:
        out_ += std::format("a:{}:{{", value.size());
        for (std::size_t i = 0; i < value.size(); ++i) {
          out_ += std::format("i:{};", i);
          write(value[i]);
        }
        out_ += "}";
        return;
      case Json::Type::Object:
        out_ += std::format("a:{}:{{", value.size());
        for (const auto& member : value.items()) {
          key(member.key());
          write(member.value());
        }
        out_ += "}";
        return;
    }
  }
  std::string out_;
};

Result<std::string> write_php(const Json& value, WriteContext& ctx) {
  PhpWriter writer;
  std::string out = writer.run(value);
  if (writer.converted_ > 0) ctx.warn("Integers beyond PHP's 64-bit range were written as strings.");
  return out;
}

int sniff_php(ByteView data, std::string_view filename) {
  const std::string_view head = as_chars(data).substr(0, 32);
  if (head.size() >= 4 && (head.starts_with("a:") || head.starts_with("O:")) && util::is_ascii_digit(head[2])) {
    const std::size_t colon = head.find(':', 2);
    if (colon != std::string_view::npos && colon + 1 < head.size() && (head[colon + 1] == '{' || head[colon + 1] == '"')) return 80;
  }
  return has_extension(filename, {"phpser", "phpserialized"}) ? 75 : 0;
}

// =========================================================================== Python literals

class PythonParser {
 public:
  PythonParser(std::string_view text, const ReadContext& ctx) : t_(text), policy_(read_policy(ctx.options)) {}

  Result<Json> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    CVT_TRY_ASSIGN(Json value, expression(0));
    skip();
    if (p_ != t_.size()) return error("unexpected content after the value");
    return value;
  }

 private:
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("Python literal", t_, p_, message); }

  void skip() {
    while (p_ < t_.size()) {
      const char c = t_[p_];
      if (util::is_ascii_space(c)) {
        ++p_;
      } else if (c == '#') {
        while (p_ < t_.size() && t_[p_] != '\n') ++p_;
      } else if (c == '\\' && p_ + 1 < t_.size() && (t_[p_ + 1] == '\n' || t_[p_ + 1] == '\r')) {
        p_ += 2;
      } else {
        break;
      }
    }
  }

  bool word(std::string_view w) {
    if (t_.substr(p_, w.size()) != w) return false;
    const std::size_t end = p_ + w.size();
    if (end < t_.size() && (util::is_ascii_alpha(t_[end]) || util::is_ascii_digit(t_[end]) || t_[end] == '_')) return false;
    p_ = end;
    return true;
  }

  /// One string literal (with prefix) at p_; `is_bytes` reports b'...'.
  Result<std::string> string_literal(bool& is_bytes) {
    bool raw = false;
    is_bytes = false;
    while (p_ < t_.size() && std::string_view("rRbBuU").find(t_[p_]) != std::string_view::npos) {
      if (t_[p_] == 'r' || t_[p_] == 'R') raw = true;
      if (t_[p_] == 'b' || t_[p_] == 'B') is_bytes = true;
      ++p_;
    }
    if (p_ >= t_.size() || (t_[p_] != '\'' && t_[p_] != '"')) return error("expected a string");
    const char quote = t_[p_];
    const bool triple = t_.substr(p_, 3) == std::string(3, quote);
    p_ += triple ? 3 : 1;
    std::string out;
    while (true) {
      if (p_ >= t_.size()) return error("unterminated string");
      if (triple ? t_.substr(p_, 3) == std::string(3, quote) : t_[p_] == quote) {
        p_ += triple ? 3 : 1;
        return out;
      }
      const char c = t_[p_++];
      if (c == '\n' && !triple) return error("line break in a string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (p_ >= t_.size()) return error("unterminated string");
      const char e = t_[p_++];
      if (raw) {
        out.push_back('\\');
        out.push_back(e);
        continue;
      }
      switch (e) {
        case '\n': break;
        case '\\': out.push_back('\\'); break;
        case '\'': out.push_back('\''); break;
        case '"': out.push_back('"'); break;
        case 'a': out.push_back('\a'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'v': out.push_back('\v'); break;
        case 'x':
        case 'u':
        case 'U': {
          const std::size_t digits = e == 'x' ? 2 : e == 'u' ? 4 : 8;
          if (is_bytes && e != 'x') {
            out.push_back('\\');
            out.push_back(e);
            break;
          }
          const auto value = p_ + digits <= t_.size() ? util::parse_int(t_.substr(p_, digits), 16) : std::nullopt;
          if (!value || *value < 0) return error("invalid escape");
          p_ += digits;
          if (is_bytes) {
            out.push_back(static_cast<char>(*value));
          } else {
            util::append_utf8(out, util::is_scalar_value(static_cast<char32_t>(*value)) ? static_cast<char32_t>(*value)
                                                                                        : util::replacement_character);
          }
          break;
        }
        default:
          if (e >= '0' && e <= '7') {
            int value = e - '0';
            for (int k = 0; k < 2 && p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '7'; ++k) value = value * 8 + (t_[p_++] - '0');
            if (is_bytes) {
              out.push_back(static_cast<char>(value & 0xFF));
            } else {
              util::append_utf8(out, static_cast<char32_t>(value));
            }
          } else {
            out.push_back('\\');
            out.push_back(e);
          }
      }
    }
  }

  Result<Json> number() {
    const std::size_t start = p_;
    bool negative = false;
    while (p_ < t_.size() && (t_[p_] == '-' || t_[p_] == '+')) {
      if (t_[p_] == '-') negative = !negative;
      ++p_;
      skip();
    }
    if (word("inf")) return Json(negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
    if (word("nan")) return Json(std::numeric_limits<double>::quiet_NaN());
    if (t_.substr(p_, 6) == "float(") {
      p_ += 6;
      skip();
      bool is_bytes = false;
      CVT_TRY_ASSIGN(const std::string text, string_literal(is_bytes));
      skip();
      CVT_TRY(expect(')'));
      const std::string lower = util::to_lower(text);
      if (lower == "nan") return Json(std::numeric_limits<double>::quiet_NaN());
      if (lower == "inf" || lower == "infinity" || lower == "+inf") {
        return Json(negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
      }
      if (lower == "-inf" || lower == "-infinity") return Json(-std::numeric_limits<double>::infinity());
      return error("unsupported float() argument");
    }
    const std::size_t body = p_;
    while (p_ < t_.size() && (util::is_ascii_alpha(t_[p_]) || util::is_ascii_digit(t_[p_]) || t_[p_] == '_' || t_[p_] == '.' ||
                              ((t_[p_] == '+' || t_[p_] == '-') && (t_[p_ - 1] == 'e' || t_[p_ - 1] == 'E')))) {
      ++p_;
    }
    std::string text;
    for (const char c : t_.substr(body, p_ - body)) {
      if (c != '_') text.push_back(c);
    }
    if (text.size() > 2 && text[0] == '0' && std::string_view("xXoObB").find(text[1]) != std::string_view::npos) {
      const int base = (text[1] == 'x' || text[1] == 'X') ? 16 : (text[1] == 'o' || text[1] == 'O') ? 8 : 2;
      auto value = integer_from_digits(std::string_view(text).substr(2), base, negative);
      if (!value) return error("invalid integer");
      return std::move(*value);
    }
    if (text.ends_with('.')) text += "0";
    if (text.starts_with('.')) text.insert(0, "0");
    if (!decimal_parts(text) || text.ends_with('j') || text.ends_with('J')) {
      p_ = start;
      return error("invalid number");
    }
    Json value = number_from_literal((negative ? "-" : "") + text);
    if (text.find_first_of(".eE") != std::string::npos && !value.is_float()) {
      // A Python float literal is a double (1e999 is infinity), whatever its digits.
      value = Json(util::parse_double((negative ? "-" : "") + text).value_or(0.0));
    }
    return value;
  }

  Result<void> expect(char c) {
    if (p_ >= t_.size() || t_[p_] != c) return error(std::format("expected '{}'", c));
    ++p_;
    return {};
  }

  Result<Json> sequence(char close, std::size_t depth) {
    Json array = Json::array();
    while (true) {
      skip();
      if (p_ >= t_.size()) return error("unterminated list");
      if (t_[p_] == close) {
        ++p_;
        return array;
      }
      CVT_TRY_ASSIGN(Json value, expression(depth + 1));
      array.push_back(std::move(value));
      skip();
      if (p_ < t_.size() && t_[p_] == ',') {
        ++p_;
        continue;
      }
      if (p_ < t_.size() && t_[p_] == close) continue;
      return error(std::format("expected ',' or '{}'", close));
    }
  }

  Result<Json> expression(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the Python literal is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    skip();
    if (p_ >= t_.size()) return error("expected a value");
    const char c = t_[p_];
    if (c == '[') {
      ++p_;
      return sequence(']', depth);
    }
    if (c == '(') {
      ++p_;
      skip();
      // A parenthesised value or a tuple.
      if (p_ < t_.size() && t_[p_] == ')') {
        ++p_;
        return Json::array();
      }
      CVT_TRY_ASSIGN(Json first, expression(depth + 1));
      skip();
      if (p_ < t_.size() && t_[p_] == ')') {
        ++p_;
        return first;
      }
      CVT_TRY(expect(','));
      CVT_TRY_ASSIGN(Json rest, sequence(')', depth));
      Json tuple = Json::array({std::move(first)});
      for (Json& element : rest.elements()) tuple.push_back(std::move(element));
      return tuple;
    }
    if (c == '{') {
      ++p_;
      skip();
      Json object = Json::object();
      Json set = Json::array();
      bool is_set = false;
      bool first = true;
      while (true) {
        skip();
        if (p_ >= t_.size()) return error("unterminated dict");
        if (t_[p_] == '}') {
          ++p_;
          return is_set ? set : object;
        }
        CVT_TRY_ASSIGN(Json key, expression(depth + 1));
        skip();
        if (first) is_set = p_ >= t_.size() || t_[p_] != ':';
        first = false;
        if (is_set) {
          set.push_back(std::move(key));
        } else {
          CVT_TRY(expect(':'));
          CVT_TRY_ASSIGN(Json value, expression(depth + 1));
          CVT_TRY(put_member(object, key_from_value(key), std::move(value), policy_));
        }
        skip();
        if (p_ < t_.size() && t_[p_] == ',') {
          ++p_;
          continue;
        }
        if (p_ < t_.size() && t_[p_] == '}') continue;
        return error("expected ',' or '}'");
      }
    }
    if (word("True")) return Json(true);
    if (word("False")) return Json(false);
    if (word("None")) return Json();
    if (t_.substr(p_, 5) == "set()") {
      p_ += 5;
      return Json::array();
    }
    const bool string_start = c == '\'' || c == '"' ||
                              ((std::string_view("rRbBuU").find(c) != std::string_view::npos) && p_ + 1 < t_.size() &&
                               (t_[p_ + 1] == '\'' || t_[p_ + 1] == '"' ||
                                (std::string_view("rRbB").find(t_[p_ + 1]) != std::string_view::npos && p_ + 2 < t_.size() &&
                                 (t_[p_ + 2] == '\'' || t_[p_ + 2] == '"'))));
    if (string_start) {
      std::string text;
      bool any_bytes = false;
      // Adjacent literals concatenate.
      while (true) {
        bool is_bytes = false;
        CVT_TRY_ASSIGN(const std::string part, string_literal(is_bytes));
        any_bytes = any_bytes || is_bytes;
        text += part;
        const std::size_t save = p_;
        skip();
        if (p_ < t_.size() && (t_[p_] == '\'' || t_[p_] == '"' || std::string_view("rRbBuU").find(t_[p_]) != std::string_view::npos)) {
          if (t_[p_] != '\'' && t_[p_] != '"' && (p_ + 1 >= t_.size() || (t_[p_ + 1] != '\'' && t_[p_ + 1] != '"' &&
                                                                          std::string_view("rRbB").find(t_[p_ + 1]) == std::string_view::npos))) {
            p_ = save;
            break;
          }
          continue;
        }
        p_ = save;
        break;
      }
      if (any_bytes) return make_binary(as_bytes(text));
      return Json(std::move(text));
    }
    if (util::is_ascii_digit(c) || c == '-' || c == '+' || c == '.' || t_.substr(p_).starts_with("inf") ||
        t_.substr(p_).starts_with("nan") || t_.substr(p_).starts_with("float(")) {
      return number();
    }
    return error(std::format("unexpected '{}' (only literals are accepted; nothing is evaluated)", c));
  }

  std::string_view t_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
};

Result<Json> parse_python(std::string_view text, ReadContext& ctx) {
  PythonParser parser(text, ctx);
  return parser.parse();
}

void python_string(std::string& out, std::string_view text, bool bytes) {
  const bool has_single = text.find('\'') != std::string_view::npos;
  const bool has_double = text.find('"') != std::string_view::npos;
  const char quote = has_single && !has_double ? '"' : '\'';
  if (bytes) out.push_back('b');
  out.push_back(quote);
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == static_cast<unsigned char>(quote) || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20 || c == 0x7F || (bytes && c >= 0x80)) {
      out += std::format("\\x{:02x}", static_cast<unsigned>(c));
    } else if (c >= 0x80) {
      const util::Utf8Decoded d = util::decode_utf8(text, i);
      if (d.ok) {
        out.append(text.substr(i, d.length));
      } else {
        out += "\\ufffd";
      }
      i += d.length;
      continue;
    } else {
      out.push_back(static_cast<char>(c));
    }
    ++i;
  }
  out.push_back(quote);
}

class PythonWriter {
 public:
  PythonWriter(int indent, bool sort) : indent_(indent), sort_(sort) {}
  std::size_t non_literal_ = 0;
  std::string run(const Json& value) {
    write(value, 0);
    out_.push_back('\n');
    return std::move(out_);
  }

 private:
  void newline(std::size_t depth) {
    if (indent_ < 0) return;
    out_.push_back('\n');
    out_.append(depth * static_cast<std::size_t>(indent_), ' ');
  }
  void number(double d) {
    if (std::isnan(d)) {
      out_ += "float('nan')";  // no literal exists; ast.literal_eval rejects this one
      ++non_literal_;
    } else if (std::isinf(d)) {
      out_ += d < 0 ? "-1e999" : "1e999";  // literal_eval reads these as infinities
    } else {
      out_ += format_double(d);
    }
  }
  void write(const Json& value, std::size_t depth) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext: {
        const auto bytes = tagged_bytes(value);
        python_string(out_, bytes ? as_chars(*bytes) : std::string_view(), true);
        return;
      }
      case TagKind::BigInt:
      case TagKind::Decimal: out_ += tag_text(value); return;
      case TagKind::Float: number(plain_value(value).as_double()); return;
      default: {
        const Json plain = plain_value(value);
        if (!plain.is_structured()) {
          write(plain, depth);
          return;
        }
        break;
      }
    }
    switch (value.type()) {
      case Json::Type::Null: out_ += "None"; return;
      case Json::Type::Bool: out_ += value.as_bool() ? "True" : "False"; return;
      case Json::Type::Int64:
      case Json::Type::UInt64: out_ += format_integer(value); return;
      case Json::Type::Double: number(value.as_double()); return;
      case Json::Type::String: python_string(out_, value.as_string(), false); return;
      case Json::Type::Array:
      case Json::Type::Object: {
        const bool object = value.is_object();
        if (value.empty()) {
          out_ += object ? "{}" : "[]";
          return;
        }
        out_.push_back(object ? '{' : '[');
        bool first = true;
        const auto separator = [&] {
          if (!first) out_ += indent_ < 0 ? ", " : ",";
          first = false;
          newline(depth + 1);
        };
        if (object) {
          for (const Json::Member* member : members_of(value, sort_)) {
            separator();
            python_string(out_, member->key(), false);
            out_ += ": ";
            write(member->value(), depth + 1);
          }
        } else {
          for (const Json& element : value.elements()) {
            separator();
            write(element, depth + 1);
          }
        }
        newline(depth);
        out_.push_back(object ? '}' : ']');
        return;
      }
    }
  }
  int indent_ = 4;
  bool sort_ = false;
  std::string out_;
};

Result<std::string> write_python(const Json& value, WriteContext& ctx) {
  const bool minify = ctx.options.get_bool("minify", false);
  PythonWriter writer(minify ? -1 : static_cast<int>(ctx.options.get_int("indent", 4, 0, 16)), ctx.options.get_bool("sort_keys", false));
  std::string out = writer.run(value);
  if (writer.non_literal_ > 0) ctx.warn("NaN has no Python literal; it was written as float('nan'), which ast.literal_eval rejects.");
  return out;
}

}  // namespace

void register_script_literals(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "lua-table",
      .name = "Lua table",
      .description = "Data written as a Lua table constructor (configuration and save files).",
      .category = Category::Data,
      .extensions = {"lua"},
      .mime = "text/x-lua",
      .popularity = 10,
      .signatures = {},
      .sniff = [](ByteView data, std::string_view filename) {
        const std::string_view head = text_head(data, 64);
        if (!has_extension(filename, {"lua"})) return 0;
        return head.starts_with("return {") || head.starts_with("{") || head.starts_with("return{") ? 70 : 0;
      },
  });
  add_codec(registry, {.format_id = "lua-table",
                       .read = text_reader(parse_lua, "Lua"),
                       .write = text_writer(write_lua, "Lua"),
                       .read_options = common_read_options(true, false),
                       .write_options = {indent_option(2), sort_keys_option()}});

  registry.add_format(FormatInfo{
      .id = "php-serialized",
      .name = "PHP serialized data",
      .description = "Values stored with PHP's serialize() function.",
      .category = Category::Data,
      .extensions = {"phpser", "phpserialized"},
      .mime = "application/vnd.php.serialized",
      .popularity = 12,
      .signatures = {},
      .sniff = sniff_php,
  });
  add_codec(registry, {.format_id = "php-serialized",
                       .read = text_reader(parse_php, "PHP serialized data", true),
                       .write = text_writer(write_php, "PHP serialized data"),
                       .read_options = common_read_options(true, false)});

  registry.add_format(FormatInfo{
      .id = "python-literal",
      .name = "Python literal",
      .description = "Data written as Python literals (dict, list, str, int, ...), as repr() prints them.",
      .category = Category::Data,
      .extensions = {"pyon", "pylit"},
      .mime = "text/x-python",
      .popularity = 10,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"pyon", "pylit"}) ? 75 : 0; },
  });
  add_codec(registry, {.format_id = "python-literal",
                       .read = text_reader(parse_python, "Python literal"),
                       .write = text_writer(write_python, "Python literal"),
                       .read_options = common_read_options(true, false),
                       .write_options = {OptionSpec::boolean("minify", "Compact", false, "Write everything on one line."),
                                         indent_option(4), sort_keys_option()}});
}

}  // namespace cvt::data
