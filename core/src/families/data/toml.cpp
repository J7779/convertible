// Convertible core - data family: TOML 1.0.
//
// Reader: every value type (basic, literal and multi-line strings, decimal /
// hex / octal / binary integers with underscores, floats incl. inf and nan,
// booleans, offset and local date-times, local dates and times as $date),
// arrays, inline tables, dotted keys, [tables] and [[arrays of tables]], with
// the specification's rules against redefining keys and tables. Integers
// beyond 64 bits become $bigint rather than an error. The TOML 1.1 additions
// (\e and \xHH escapes, multi-line inline tables, times without seconds) are
// accepted.
//
// Writer: plain values first, then [tables] and [[arrays of tables]] in key
// order, so the layout is stable. TOML has no null: such members are left
// out (with a warning) or written per `null_values`.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/data.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace {

// =========================================================================== reader

enum class TableState : std::uint8_t { Implicit, Explicit, Dotted, Inline, ArrayOfTables };

/// A step of a path from the root: a key, or the last element of an array of tables.
struct PathStep {
  std::string key;
};

class TomlParser {
 public:
  TomlParser(std::string_view text, ReadContext& ctx) : t_(text), ctx_(ctx), policy_(read_policy(ctx.options)) {}

  Result<Json> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    root_ = Json::object();
    std::size_t lines = 0;
    while (true) {
      skip_ws();
      if (at_end()) break;
      const char c = peek();
      if (c == '#') {
        CVT_TRY(skip_comment());
      } else if (c == '\n' || c == '\r') {
        // handled by expect_newline
      } else if (c == '[') {
        CVT_TRY(parse_header());
      } else {
        CVT_TRY(parse_key_value_line());
      }
      CVT_TRY(expect_newline());
      if ((++lines & 4095) == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(p_) / static_cast<double>(t_.size())));
    }
    return std::move(root_);
  }

 private:
  [[nodiscard]] bool at_end() const noexcept { return p_ >= t_.size(); }
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
    return p_ + ahead < t_.size() ? t_[p_ + ahead] : '\0';
  }
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("TOML", t_, p_, message); }
  std::unexpected<Error> error_at(std::size_t at, std::string_view message) const {
    return syntax_error("TOML", t_, at, message);
  }

  void skip_ws() {
    while (peek() == ' ' || peek() == '\t') ++p_;
  }

  Result<void> skip_comment() {
    ++p_;  // '#'
    while (!at_end() && peek() != '\n') {
      const auto c = static_cast<unsigned char>(peek());
      if ((c < 0x20 && c != '\t' && !(c == '\r' && peek(1) == '\n')) || c == 0x7F) {
        return error("control character in a comment");
      }
      ++p_;
    }
    return {};
  }

  /// White space, an optional comment, then a newline or the end.
  Result<void> expect_newline() {
    skip_ws();
    if (peek() == '#') CVT_TRY(skip_comment());
    if (at_end()) return {};
    if (peek() == '\n') {
      ++p_;
      return {};
    }
    if (peek() == '\r' && peek(1) == '\n') {
      p_ += 2;
      return {};
    }
    return error("expected the end of the line");
  }

  /// White space, newlines and comments (inside arrays).
  Result<void> skip_ws_newlines() {
    while (!at_end()) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\n') {
        ++p_;
      } else if (c == '\r' && peek(1) == '\n') {
        p_ += 2;
      } else if (c == '#') {
        CVT_TRY(skip_comment());
      } else {
        break;
      }
    }
    return {};
  }

  // ------------------------------------------------------------------ keys

  [[nodiscard]] static bool bare_key_char(char c) noexcept {
    return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '-';
  }

  Result<std::string> parse_simple_key() {
    const char c = peek();
    if (c == '"') {
      if (t_.substr(p_, 3) == "\"\"\"") return error("a key cannot be a multi-line string");
      return parse_basic_string();
    }
    if (c == '\'') {
      if (t_.substr(p_, 3) == "'''") return error("a key cannot be a multi-line string");
      return parse_literal_string();
    }
    const std::size_t start = p_;
    while (bare_key_char(peek())) ++p_;
    if (p_ == start) return error(at_end() ? "unexpected end of file; expected a key" : "invalid character in a key");
    return std::string(t_.substr(start, p_ - start));
  }

  Result<std::vector<std::string>> parse_key() {
    std::vector<std::string> keys;
    while (true) {
      skip_ws();
      CVT_TRY_ASSIGN(std::string key, parse_simple_key());
      keys.push_back(std::move(key));
      skip_ws();
      if (peek() != '.') break;
      ++p_;
      if (keys.size() > policy_.max_depth) return fail(Status::Limit, "the TOML key has too many parts");
    }
    return keys;
  }

  // ------------------------------------------------------------------ tables

  static std::string path_id(const std::vector<std::string>& path) {
    std::string id;
    for (const std::string& part : path) {
      id += part;
      id.push_back('\0');
    }
    return id;
  }

  /// Walk `keys` from the root, descending into the last element of arrays
  /// of tables. Creates missing tables with `create_state`. Returns the
  /// concrete path ids (with array indices) through `concrete`.
  Result<Json*> walk(const std::vector<std::string>& keys, std::size_t count, TableState create_state,
                     std::vector<std::string>& concrete, bool for_dotted, std::size_t at) {
    Json* node = &root_;
    for (std::size_t i = 0; i < count; ++i) {
      const std::string& key = keys[i];
      concrete.push_back(key);
      Json* child = node->find(key);
      if (child == nullptr) {
        node->set(key, Json::object());
        child = node->find(key);
        states_[path_id(concrete)] = create_state;
      } else {
        const auto state = states_.find(path_id(concrete));
        if (child->is_array() && state != states_.end() && state->second == TableState::ArrayOfTables) {
          if (for_dotted) return error_at(at, std::format("cannot add keys to the array of tables '{}'", key));
          concrete.push_back(std::format("#{}", child->size() - 1));
          child = child->at(child->size() - 1);
        } else if (!child->is_object() || state == states_.end()) {
          return error_at(at, std::format("'{}' is already defined as a value, not a table", key));
        } else if (state->second == TableState::Inline) {
          return error_at(at, std::format("the inline table '{}' cannot be extended", key));
        } else if (for_dotted && state->second != TableState::Dotted) {
          return error_at(at, std::format("the table '{}' cannot be extended with dotted keys here", key));
        }
      }
      node = child;
    }
    return node;
  }

  Result<void> parse_header() {
    const std::size_t at = p_;
    const bool array = peek(1) == '[';
    p_ += array ? 2 : 1;
    CVT_TRY_ASSIGN(std::vector<std::string> keys, parse_key());
    skip_ws();
    if (array ? t_.substr(p_, 2) != "]]" : peek() != ']') {
      return error(array ? "expected ']]' to close the array-of-tables header" : "expected ']' to close the table header");
    }
    p_ += array ? 2 : 1;
    if (keys.size() > policy_.max_depth) {
      return fail(Status::Limit, "the TOML data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    std::vector<std::string> concrete;
    CVT_TRY_ASSIGN(Json* parent, walk(keys, keys.size() - 1, TableState::Implicit, concrete, false, at));
    const std::string& last = keys.back();
    concrete.push_back(last);
    const std::string id = path_id(concrete);
    Json* existing = parent->find(last);
    if (array) {
      if (existing == nullptr) {
        parent->set(last, Json::array());
        existing = parent->find(last);
        states_[id] = TableState::ArrayOfTables;
      } else if (const auto state = states_.find(id); state == states_.end() || state->second != TableState::ArrayOfTables) {
        return error_at(at, std::format("'{}' is already defined and is not an array of tables", last));
      }
      CVT_TRY(ctx_.limits.check_archive_entries(existing->size() + 1));
      existing->push_back(Json::object());
      concrete.push_back(std::format("#{}", existing->size() - 1));
      states_[path_id(concrete)] = TableState::Explicit;
    } else {
      if (existing == nullptr) {
        parent->set(last, Json::object());
        states_[id] = TableState::Explicit;
      } else {
        const auto state = states_.find(id);
        if (state == states_.end() || !existing->is_object()) {
          return error_at(at, std::format("'{}' is already defined as a value", last));
        }
        if (state->second != TableState::Implicit) {
          return error_at(at, std::format("the table [{}] is defined more than once", util::join(keys, ".")));
        }
        state->second = TableState::Explicit;
      }
    }
    current_ = std::move(concrete);
    return {};
  }

  /// Re-resolve the current table path (indices included).
  Json* current_table() {
    Json* node = &root_;
    for (const std::string& step : current_) {
      if (step.starts_with('#') && node->is_array()) {
        node = node->at(static_cast<std::size_t>(util::parse_int(std::string_view(step).substr(1)).value_or(0)));
      } else {
        node = node->find(step);
      }
      if (node == nullptr) return &root_;
    }
    return node;
  }

  Result<void> parse_key_value_line() {
    std::vector<std::string> base = current_;
    CVT_TRY(parse_key_value(current_table(), base, 0));
    return {};
  }

  /// key = value into `table` (whose concrete path is `base`).
  Result<void> parse_key_value(Json* table, std::vector<std::string> base, std::size_t depth) {
    const std::size_t at = p_;
    CVT_TRY_ASSIGN(std::vector<std::string> keys, parse_key());
    skip_ws();
    if (peek() != '=') return error("expected '=' after the key");
    ++p_;
    skip_ws();
    CVT_TRY_ASSIGN(Json value, parse_value(depth + keys.size(), base, keys));
    // Walk the dotted part inside `table`.
    Json* node = table;
    std::vector<std::string> concrete = std::move(base);
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
      concrete.push_back(keys[i]);
      Json* child = node->find(keys[i]);
      const std::string id = path_id(concrete);
      if (child == nullptr) {
        node->set(keys[i], Json::object());
        child = node->find(keys[i]);
        states_[id] = TableState::Dotted;
      } else {
        const auto state = states_.find(id);
        if (!child->is_object() || state == states_.end()) {
          return error_at(at, std::format("'{}' is already defined as a value, not a table", keys[i]));
        }
        if (state->second != TableState::Dotted) {
          return error_at(at, std::format("the table '{}' cannot be extended with dotted keys", keys[i]));
        }
      }
      node = child;
    }
    if (node->contains(keys.back())) {
      return error_at(at, std::format("the key '{}' is defined more than once", util::join(keys, ".")));
    }
    node->set(keys.back(), std::move(value));
    return {};
  }

  // ------------------------------------------------------------------ values

  Result<Json> parse_value(std::size_t depth, const std::vector<std::string>& base, const std::vector<std::string>& keys) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the TOML data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    const char c = peek();
    if (c == '"') {
      CVT_TRY_ASSIGN(std::string text, t_.substr(p_, 3) == "\"\"\"" ? parse_multiline_basic() : parse_basic_string());
      return Json(std::move(text));
    }
    if (c == '\'') {
      CVT_TRY_ASSIGN(std::string text, t_.substr(p_, 3) == "'''" ? parse_multiline_literal() : parse_literal_string());
      return Json(std::move(text));
    }
    if (c == '[') return parse_array(depth, base, keys);
    if (c == '{') return parse_inline_table(depth, base, keys);
    if (t_.substr(p_, 4) == "true" && !bare_key_char(peek(4))) {
      p_ += 4;
      return Json(true);
    }
    if (t_.substr(p_, 5) == "false" && !bare_key_char(peek(5))) {
      p_ += 5;
      return Json(false);
    }
    if (at_end() || c == '\n' || c == '\r' || c == '#') return error("expected a value");
    return parse_number_or_date();
  }

  Result<Json> parse_array(std::size_t depth, const std::vector<std::string>& base, const std::vector<std::string>& keys) {
    ++p_;
    Json array = Json::array();
    while (true) {
      CVT_TRY(skip_ws_newlines());
      if (peek() == ']') {
        ++p_;
        return array;
      }
      if (at_end()) return error("unterminated array");
      CVT_TRY_ASSIGN(Json value, parse_value(depth + 1, base, keys));
      array.push_back(std::move(value));
      CVT_TRY(skip_ws_newlines());
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == ']') {
        ++p_;
        return array;
      }
      return error("expected ',' or ']' in an array");
    }
  }

  Result<Json> parse_inline_table(std::size_t depth, std::vector<std::string> base, const std::vector<std::string>& keys) {
    ++p_;
    // Inline tables get a private path so their contents are frozen.
    base.push_back(std::format("{{{}", inline_counter_++));
    for (const std::string& key : keys) base.push_back(key);
    states_[path_id(base)] = TableState::Dotted;
    Json table = Json::object();
    bool first = true;
    while (true) {
      CVT_TRY(skip_ws_newlines());
      if (peek() == '}') {
        ++p_;
        break;
      }
      if (!first) {
        if (peek() != ',') return error("expected ',' or '}' in an inline table");
        ++p_;
        CVT_TRY(skip_ws_newlines());
        if (peek() == '}') {
          ++p_;
          break;
        }
      }
      first = false;
      if (at_end()) return error("unterminated inline table");
      CVT_TRY(parse_key_value(&table, base, depth + 1));
    }
    return table;
  }

  Result<std::uint32_t> hex_digits(std::size_t count) {
    if (p_ + count > t_.size()) return error("incomplete escape sequence");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const char c = t_[p_ + i];
      int digit = -1;
      if (c >= '0' && c <= '9') digit = c - '0';
      if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
      if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
      if (digit < 0) return error_at(p_ + i, "invalid hexadecimal digit in an escape");
      value = value * 16 + static_cast<std::uint32_t>(digit);
    }
    p_ += count;
    return value;
  }

  Result<void> escape(std::string& out) {
    ++p_;  // backslash
    const char e = peek();
    ++p_;
    switch (e) {
      case 'b': out.push_back('\b'); return {};
      case 't': out.push_back('\t'); return {};
      case 'n': out.push_back('\n'); return {};
      case 'f': out.push_back('\f'); return {};
      case 'r': out.push_back('\r'); return {};
      case 'e': out.push_back('\x1B'); return {};
      case '"': out.push_back('"'); return {};
      case '\\': out.push_back('\\'); return {};
      case 'x':
      case 'u':
      case 'U': {
        CVT_TRY_ASSIGN(const std::uint32_t cp, hex_digits(e == 'x' ? 2 : e == 'u' ? 4 : 8));
        if (!util::is_scalar_value(static_cast<char32_t>(cp))) {
          return error_at(p_ - 6, std::format("\\{} escape U+{:X} is not a Unicode scalar value", e, cp));
        }
        util::append_utf8(out, static_cast<char32_t>(cp));
        return {};
      }
      default: return error_at(p_ - 2, "invalid escape sequence");
    }
  }

  Result<void> check_string_char(char c, bool multiline) {
    const auto u = static_cast<unsigned char>(c);
    if ((u < 0x20 && c != '\t' && !(multiline && c == '\n')) || u == 0x7F) {
      if (multiline && c == '\r' && peek(1) == '\n') return {};
      return error(c == '\n' || c == '\r' ? "line break inside a single-line string"
                                          : "control characters must be escaped in strings");
    }
    return {};
  }

  Result<std::string> parse_basic_string() {
    const std::size_t start = p_;
    ++p_;
    std::string out;
    while (true) {
      if (at_end()) return error_at(start, "unterminated string");
      const char c = peek();
      if (c == '"') {
        ++p_;
        return out;
      }
      if (c == '\\') {
        CVT_TRY(escape(out));
        continue;
      }
      CVT_TRY(check_string_char(c, false));
      out.push_back(c);
      ++p_;
    }
  }

  Result<std::string> parse_literal_string() {
    const std::size_t start = p_;
    ++p_;
    std::string out;
    while (true) {
      if (at_end()) return error_at(start, "unterminated string");
      const char c = peek();
      if (c == '\'') {
        ++p_;
        return out;
      }
      CVT_TRY(check_string_char(c, false));
      out.push_back(c);
      ++p_;
    }
  }

  void skip_initial_newline() {
    if (peek() == '\n') {
      ++p_;
    } else if (peek() == '\r' && peek(1) == '\n') {
      p_ += 2;
    }
  }

  /// At a run of the closing quote character: 3..5 quotes close the string
  /// (the extra ones belong to the content). Returns the number of content
  /// quotes, or -1 when the run does not close.
  int closing_run(char quote) {
    std::size_t run = 0;
    while (peek(run) == quote) ++run;
    if (run < 3) return -1;
    if (run > 5) return -2;
    p_ += run;
    return static_cast<int>(run - 3);
  }

  Result<std::string> parse_multiline_basic() {
    const std::size_t start = p_;
    p_ += 3;
    skip_initial_newline();
    std::string out;
    while (true) {
      if (at_end()) return error_at(start, "unterminated multi-line string");
      const char c = peek();
      if (c == '"') {
        const int extra = closing_run('"');
        if (extra == -2) return error("too many quotes at the end of a multi-line string");
        if (extra >= 0) {
          out.append(static_cast<std::size_t>(extra), '"');
          return out;
        }
        out.push_back('"');
        ++p_;
        continue;
      }
      if (c == '\\') {
        // Line-ending backslash: trims white space up to the next non-blank.
        std::size_t look = p_ + 1;
        while (look < t_.size() && (t_[look] == ' ' || t_[look] == '\t')) ++look;
        if (look < t_.size() && (t_[look] == '\n' || (t_[look] == '\r' && look + 1 < t_.size() && t_[look + 1] == '\n'))) {
          p_ = look;
          while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || (peek() == '\r' && peek(1) == '\n'))) {
            p_ += peek() == '\r' ? 2 : 1;
          }
          continue;
        }
        CVT_TRY(escape(out));
        continue;
      }
      CVT_TRY(check_string_char(c, true));
      if (c == '\r') {
        out.push_back('\n');
        p_ += 2;
        continue;
      }
      out.push_back(c);
      ++p_;
    }
  }

  Result<std::string> parse_multiline_literal() {
    const std::size_t start = p_;
    p_ += 3;
    skip_initial_newline();
    std::string out;
    while (true) {
      if (at_end()) return error_at(start, "unterminated multi-line string");
      const char c = peek();
      if (c == '\'') {
        const int extra = closing_run('\'');
        if (extra == -2) return error("too many quotes at the end of a multi-line string");
        if (extra >= 0) {
          out.append(static_cast<std::size_t>(extra), '\'');
          return out;
        }
        out.push_back('\'');
        ++p_;
        continue;
      }
      CVT_TRY(check_string_char(c, true));
      if (c == '\r') {
        out.push_back('\n');
        p_ += 2;
        continue;
      }
      out.push_back(c);
      ++p_;
    }
  }

  /// Digits with single underscores between them.
  Result<std::string> digits_with_underscores(bool (*valid)(char), std::string_view what) {
    std::string out;
    const std::size_t start = p_;
    bool last_underscore = true;  // no leading underscore
    while (!at_end()) {
      const char c = peek();
      if (c == '_') {
        if (last_underscore) return error("misplaced '_' in a number");
        last_underscore = true;
      } else if (valid(c)) {
        out.push_back(c);
        last_underscore = false;
      } else {
        break;
      }
      ++p_;
    }
    if (out.empty()) return error_at(start, std::format("expected {} digits", what));
    if (last_underscore) return error("a number cannot end with '_'");
    return out;
  }

  static bool dec(char c) { return util::is_ascii_digit(c); }
  static bool hex(char c) { return dec(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
  static bool oct(char c) { return c >= '0' && c <= '7'; }
  static bool bin(char c) { return c == '0' || c == '1'; }

  Result<Json> parse_number_or_date() {
    const std::size_t start = p_;
    // Date or time?
    const auto digit_at = [&](std::size_t i) { return util::is_ascii_digit(peek(i)); };
    if (digit_at(0) && digit_at(1) && digit_at(2) && digit_at(3) && peek(4) == '-') return parse_datetime_value();
    if (digit_at(0) && digit_at(1) && peek(2) == ':') return parse_datetime_value();
    std::string literal;
    if (peek() == '+' || peek() == '-') literal.push_back(t_[p_++]);
    if (t_.substr(p_, 3) == "inf" || t_.substr(p_, 3) == "nan") {
      const bool nan = peek() == 'n';
      p_ += 3;
      if (bare_key_char(peek())) return error_at(start, "invalid value");
      if (nan) return Json(std::numeric_limits<double>::quiet_NaN());
      return Json(literal == "-" ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
    }
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'o' || peek(1) == 'b')) {
      if (!literal.empty()) return error_at(start, "hexadecimal, octal and binary integers cannot have a sign");
      const char kind = peek(1);
      p_ += 2;
      std::string digits;
      if (kind == 'x') {
        CVT_TRY_ASSIGN(digits, digits_with_underscores(hex, "hexadecimal"));
      } else if (kind == 'o') {
        CVT_TRY_ASSIGN(digits, digits_with_underscores(oct, "octal"));
      } else {
        CVT_TRY_ASSIGN(digits, digits_with_underscores(bin, "binary"));
      }
      if (bare_key_char(peek()) || peek() == '.') return error_at(start, "invalid character in an integer");
      auto value = integer_from_digits(digits, kind == 'x' ? 16 : kind == 'o' ? 8 : 2, false);
      if (!value) return error_at(start, "integer is too long");
      return std::move(*value);
    }
    if (!dec(peek())) return error_at(start, "expected a value");
    CVT_TRY_ASSIGN(const std::string integer, digits_with_underscores(dec, "decimal"));
    if (integer.size() > 1 && integer.front() == '0') return error_at(start, "leading zeros are not allowed");
    literal += integer;
    bool is_float = false;
    if (peek() == '.') {
      ++p_;
      if (!dec(peek())) return error("expected a digit after the decimal point");
      CVT_TRY_ASSIGN(const std::string fraction, digits_with_underscores(dec, "decimal"));
      literal += "." + fraction;
      is_float = true;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++p_;
      literal += 'e';
      if (peek() == '+' || peek() == '-') literal.push_back(t_[p_++]);
      CVT_TRY_ASSIGN(const std::string exponent, digits_with_underscores(dec, "exponent"));
      literal += exponent;
      is_float = true;
    }
    if (bare_key_char(peek()) || peek() == '.') return error_at(start, "invalid character in a number");
    Json value = number_from_literal(literal);
    if (!is_float && tag_kind(value) == TagKind::BigInt) {
      ctx_.warn("An integer beyond the 64-bit range of TOML was kept exactly as a big integer.");
    }
    return value;
  }

  Result<Json> parse_datetime_value() {
    const std::size_t start = p_;
    while (!at_end()) {
      const char c = peek();
      if (util::is_ascii_digit(c) || c == '-' || c == ':' || c == '.' || c == '+' || c == 'T' || c == 't' || c == 'Z' ||
          c == 'z') {
        ++p_;
      } else if (c == ' ' && util::is_ascii_digit(peek(1)) && util::is_ascii_digit(peek(2)) && peek(3) == ':' &&
                 p_ - start == 10) {
        ++p_;  // "1979-05-27 07:32:00"
      } else {
        break;
      }
    }
    const std::string_view text = t_.substr(start, p_ - start);
    const auto parsed = parse_datetime(text);
    if (!parsed) return error_at(start, std::format("invalid date or time '{}'", text));
    return make_date(format_datetime(*parsed));
  }

  std::string_view t_;
  ReadContext& ctx_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
  Json root_;
  std::vector<std::string> current_;
  std::unordered_map<std::string, TableState> states_;
  std::size_t inline_counter_ = 0;
};

Result<Json> parse_toml_text(std::string_view text, ReadContext& ctx) {
  TomlParser parser(text, ctx);
  return parser.parse();
}

// =========================================================================== writer

bool bare_key(std::string_view key) {
  return !key.empty() && std::ranges::all_of(key, [](char c) {
    return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '-';
  });
}

void basic_string(std::string& out, std::string_view text, bool multiline) {
  out += multiline ? "\"\"\"\n" : "\"";
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += multiline ? "\n" : "\\n";
    } else if (c == '\t') {
      out += multiline ? "\t" : "\\t";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\b') {
      out += "\\b";
    } else if (c == '\f') {
      out += "\\f";
    } else if (c < 0x20 || c == 0x7F) {
      out += std::format("\\u{:04X}", static_cast<unsigned>(c));
    } else if (c >= 0x80) {
      const util::Utf8Decoded d = util::decode_utf8(text, i);
      if (d.ok) {
        out.append(text.substr(i, d.length));
      } else {
        out += "\\uFFFD";
      }
      i += d.length;
      continue;
    } else {
      out.push_back(static_cast<char>(c));
    }
    ++i;
  }
  out += multiline ? "\"\"\"" : "\"";
}

void write_key(std::string& out, std::string_view key) {
  if (bare_key(key)) {
    out += key;
  } else {
    basic_string(out, key, false);
  }
}

struct TomlStyle {
  bool sort_keys = false;
  bool multiline_strings = true;
  std::string null_mode = "omit";
  std::size_t array_width = 80;
};

class TomlWriter {
 public:
  TomlWriter(const TomlStyle& style, const WriteContext& ctx) : style_(style), ctx_(ctx) {}

  Result<std::string> run(const Json& root) {
    CVT_TRY(write_table(root, {}, false));
    if (nulls_ > 0) ctx_.warn(std::format("TOML has no null; {} null value(s) were {}.", nulls_,
                                          style_.null_mode == "omit" ? "left out" : "written as empty strings"));
    if (big_ > 0) ctx_.warn(std::format("{} integer(s) beyond 64 bits were written as strings.", big_));
    if (binary_ > 0) ctx_.warn(std::format("{} binary value(s) were written as Base64 strings.", binary_));
    return std::move(out_);
  }

 private:
  [[nodiscard]] static bool is_table(const Json& value) { return value.is_object() && tag_kind(value) == TagKind::None; }
  [[nodiscard]] static bool is_array_of_tables(const Json& value) {
    return value.is_array() && !value.empty() && std::ranges::all_of(value.elements(), [](const Json& e) { return is_table(e); });
  }

  Result<bool> skip_null(const Json& value) {
    if (!value.is_null() && tag_kind(value) != TagKind::Undefined) return false;
    if (style_.null_mode == "error") return fail(Status::Encode, "TOML has no null value (option null_values is \"error\")");
    ++nulls_;
    return style_.null_mode == "omit";
  }

  void header(const std::vector<std::string>& path, bool array) {
    if (!out_.empty()) out_.push_back('\n');
    out_ += array ? "[[" : "[";
    for (std::size_t i = 0; i < path.size(); ++i) {
      if (i > 0) out_.push_back('.');
      write_key(out_, path[i]);
    }
    out_ += array ? "]]\n" : "]\n";
  }

  Result<void> write_table(const Json& table, std::vector<std::string> path, bool array_element) {
    const auto members = members_of(table, style_.sort_keys);
    bool has_plain = false;
    for (const Json::Member* member : members) {
      if (!is_table(member->value()) && !is_array_of_tables(member->value())) has_plain = true;
    }
    if (!path.empty() && (array_element || has_plain || members.empty())) header(path, array_element);
    for (const Json::Member* member : members) {
      const Json& value = member->value();
      if (is_table(value) || is_array_of_tables(value)) continue;
      CVT_TRY_ASSIGN(const bool skip, skip_null(value));
      if (skip) continue;
      write_key(out_, member->key());
      out_ += " = ";
      CVT_TRY(write_value(value, true));
      out_.push_back('\n');
    }
    for (const Json::Member* member : members) {
      const Json& value = member->value();
      std::vector<std::string> child = path;
      child.push_back(member->key());
      if (is_table(value)) {
        CVT_TRY(write_table(value, child, false));
      } else if (is_array_of_tables(value)) {
        for (const Json& element : value.elements()) CVT_TRY(write_table(element, child, true));
      }
    }
    return {};
  }

  Result<void> write_value(const Json& value, bool top) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Date: {
        const std::string text(tag_text(value));
        if (const auto parsed = parse_datetime(text)) {
          out_ += format_datetime(*parsed);
        } else {
          basic_string(out_, text, false);
        }
        return {};
      }
      case TagKind::BigInt:
        ++big_;
        basic_string(out_, tag_text(value), false);
        return {};
      case TagKind::Decimal: out_ += tag_text(value); return {};
      case TagKind::Float: return write_value(plain_value(value), top);
      case TagKind::Binary:
        ++binary_;
        basic_string(out_, value[ir::data_tags::binary].as_string(), false);
        return {};
      default: break;  // other tagged objects stay inline tables
    }
    switch (value.type()) {
      case Json::Type::Null:
        basic_string(out_, "", false);
        return {};
      case Json::Type::Bool: out_ += value.as_bool() ? "true" : "false"; return {};
      case Json::Type::Int64:
      case Json::Type::UInt64:
        if (value.is_unsigned()) {
          ++big_;
          basic_string(out_, format_integer(value), false);
          return {};
        }
        out_ += format_integer(value);
        return {};
      case Json::Type::Double: {
        const double d = value.as_double();
        if (std::isnan(d)) {
          out_ += "nan";
        } else if (std::isinf(d)) {
          out_ += d < 0 ? "-inf" : "inf";
        } else {
          out_ += format_double(d);
        }
        return {};
      }
      case Json::Type::String: {
        const std::string& text = value.as_string();
        basic_string(out_, text, top && style_.multiline_strings && text.find('\n') != std::string::npos &&
                                     text.find('\r') == std::string::npos);
        return {};
      }
      case Json::Type::Array: {
        // Try one line first; fall back to one element per line.
        const std::size_t start = out_.size();
        out_.push_back('[');
        bool first = true;
        std::size_t written = 0;
        for (const Json& element : value.elements()) {
          CVT_TRY_ASSIGN(const bool skip, skip_null(element));
          if (skip) continue;
          if (!first) out_ += ", ";
          first = false;
          CVT_TRY(write_value(element, false));
          ++written;
        }
        out_.push_back(']');
        const std::string_view line(out_.data() + start, out_.size() - start);
        if (top && written > 1 && (line.size() > style_.array_width || line.find('\n') != std::string_view::npos)) {
          out_.resize(start);
          out_ += "[\n";
          for (const Json& element : value.elements()) {
            CVT_TRY_ASSIGN(const bool skip, skip_null(element));
            if (skip) continue;
            out_ += "  ";
            CVT_TRY(write_value(element, false));
            out_ += ",\n";
          }
          out_.push_back(']');
        }
        return {};
      }
      case Json::Type::Object: {
        if (value.empty()) {
          out_ += "{}";
          return {};
        }
        out_ += "{ ";
        bool first = true;
        for (const Json::Member* member : members_of(value, style_.sort_keys)) {
          CVT_TRY_ASSIGN(const bool skip, skip_null(member->value()));
          if (skip) continue;
          if (!first) out_ += ", ";
          first = false;
          write_key(out_, member->key());
          out_ += " = ";
          CVT_TRY(write_value(member->value(), false));
        }
        out_ += " }";
        return {};
      }
    }
    return {};
  }

  const TomlStyle& style_;
  const WriteContext& ctx_;
  std::string out_;
  std::size_t nulls_ = 0;
  std::size_t big_ = 0;
  std::size_t binary_ = 0;
};

Result<std::string> write_toml(const Json& value, WriteContext& ctx) {
  TomlStyle style;
  style.sort_keys = ctx.options.get_bool("sort_keys", false);
  style.multiline_strings = ctx.options.get_bool("multiline_strings", true);
  style.null_mode = ctx.options.get_enum("null_values", "omit", {"omit", "empty_string", "error"});
  if (style.null_mode == "empty_string") style.null_mode = "string";
  Json root;
  const Json* table = &value;
  if (!value.is_object() || tag_kind(value) != TagKind::None) {
    const std::string key = value.is_array() ? "items" : "value";
    ctx.warn(std::format("A TOML document is a table; the data was placed under the key \"{}\".", key));
    root = Json::object();
    root.set(key, value);
    table = &root;
  }
  TomlWriter writer(style, ctx);
  return writer.run(*table);
}

int sniff_toml(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"toml"})) return 75;
  const std::string_view head = text_head(data);
  // [section] lines and key = "value" pairs are shared with INI; TOML is only
  // claimed with a TOML-specific construct such as [[array]] or a dotted header.
  if (head.starts_with("[[") ) return 45;
  return 0;
}

}  // namespace

void register_toml(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "toml",
      .name = "TOML",
      .description = "Tom's Obvious Minimal Language, a configuration file format with typed values.",
      .category = Category::Data,
      .extensions = {"toml"},
      .mime = "application/toml",
      .popularity = 62,
      .signatures = {},
      .sniff = sniff_toml,
  });
  add_codec(registry,
            {.format_id = "toml",
             .read = text_reader(parse_toml_text, "TOML"),
             .write = text_writer(write_toml, "TOML"),
             .read_options = common_read_options(false),
             .write_options = {
                 sort_keys_option(),
                 OptionSpec::choice("null_values", "Null values", "omit", {"omit", "empty_string", "error"},
                                    "TOML has no null: leave such members out, write empty strings, or fail."),
                 OptionSpec::boolean("multiline_strings", "Multi-line strings", true,
                                     "Write text containing line breaks as \"\"\" blocks.")
                     .advanced(),
             }});
}

}  // namespace cvt::data
