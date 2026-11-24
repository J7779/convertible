// Convertible core - data family: JSON and its dialects.
//
//   json      RFC 8259. The reader is lenient by default (comments, trailing
//             commas, NaN/Infinity are accepted with a warning); `strict`
//             turns that off. Numbers are exact: integers beyond 64 bits and
//             decimals a double cannot hold become $bigint / $decimal.
//   jsonc     JSON with comments and trailing commas (VS Code settings).
//   json5     JSON5 1.0.0: identifiers as keys, single quotes, hex numbers,
//             +/-Infinity, NaN, leading/trailing decimal points, escapes such
//             as \x41 and line continuations, extra white space.
//   ndjson    newline-delimited JSON / JSON Lines: one value per line.
//   json-seq  RFC 7464 JSON text sequences (RS-prefixed records).
// Same-format conversion minifies unless indentation is asked for.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include "engine/registry.hpp"
#include "engine/sniffers.hpp"
#include "families/data/data.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace {

using namespace std::string_view_literals;

constexpr bool is_hex(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// =========================================================================== reader

struct Dialect {
  std::string_view name = "JSON";
  bool comments = false;
  bool trailing_commas = false;
  bool nan_inf = false;
  bool json5 = false;  // everything else JSON5 adds
};

class JsonParser {
 public:
  JsonParser(std::string_view text, Dialect dialect, const ReadPolicy& policy)
      : text_(text), dialect_(dialect), policy_(policy) {}

  /// One complete text: optional BOM, value, nothing but white space after.
  Result<Json> parse_document() {
    if (text_.starts_with("\xEF\xBB\xBF")) pos_ = 3;
    CVT_TRY(skip_space());
    if (pos_ >= text_.size()) return error("the text is empty; expected a value");
    CVT_TRY_ASSIGN(Json value, parse_value(0));
    CVT_TRY(skip_space());
    if (pos_ < text_.size()) return error("unexpected content after the value");
    return value;
  }

  /// Parse one value starting at `offset` within the same text (for the
  /// sequence formats); returns the value and leaves `pos()` after it.
  Result<Json> parse_at(std::size_t offset) {
    pos_ = offset;
    CVT_TRY(skip_space());
    return parse_value(0);
  }
  Result<void> skip_space_public() { return skip_space(); }
  [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
  [[nodiscard]] std::size_t lone_surrogates() const noexcept { return lone_surrogates_; }
  [[nodiscard]] bool used_extensions() const noexcept { return used_extensions_; }

 private:
  std::unexpected<Error> error(std::string_view message) const {
    return syntax_error(dialect_.name, text_, pos_, message);
  }
  std::unexpected<Error> error_at(std::size_t at, std::string_view message) const {
    return syntax_error(dialect_.name, text_, at, message);
  }

  [[nodiscard]] char peek() const noexcept { return pos_ < text_.size() ? text_[pos_] : '\0'; }

  /// JSON5 white space beyond ASCII: NBSP, BOM, LS, PS and the Zs category.
  [[nodiscard]] std::size_t unicode_space_length() const noexcept {
    const util::Utf8Decoded d = util::decode_utf8(text_, pos_);
    if (!d.ok) return 0;
    const char32_t c = d.code_point;
    const bool space = c == 0xA0 || c == 0xFEFF || c == 0x2028 || c == 0x2029 || c == 0x1680 ||
                       (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
    return space ? d.length : 0;
  }

  Result<void> skip_space() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
        continue;
      }
      if (dialect_.json5 && (c == '\v' || c == '\f')) {
        ++pos_;
        continue;
      }
      if (dialect_.json5 && static_cast<unsigned char>(c) >= 0x80) {
        if (const std::size_t length = unicode_space_length(); length > 0) {
          pos_ += length;
          continue;
        }
      }
      if (c == '/' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == '/' || text_[pos_ + 1] == '*')) {
        if (!dialect_.comments) return error("comments are not allowed in strict JSON");
        used_extensions_ = true;
        if (text_[pos_ + 1] == '/') {
          while (pos_ < text_.size() && text_[pos_] != '\n' && text_[pos_] != '\r') ++pos_;
        } else {
          const std::size_t end = text_.find("*/", pos_ + 2);
          if (end == std::string_view::npos) return error("unterminated /* comment");
          pos_ = end + 2;
        }
        continue;
      }
      break;
    }
    return {};
  }

  Result<Json> parse_value(std::size_t depth) {
    const char c = peek();
    switch (c) {
      case '{': return parse_object(depth);
      case '[': return parse_array(depth);
      case '"': return parse_string_value('"');
      case '\'':
        if (!dialect_.json5) return error("single-quoted strings are not JSON");
        return parse_string_value('\'');
      case 't': return parse_word("true", Json(true));
      case 'f': return parse_word("false", Json(false));
      case 'n': return parse_word("null", Json(nullptr));
      default: break;
    }
    if (c == '-' || c == '+' || c == '.' || util::is_ascii_digit(c) || c == 'N' || c == 'I') return parse_number();
    if (pos_ >= text_.size()) return error("unexpected end of text; expected a value");
    return error(std::format("unexpected character '{}'; expected a value", printable(pos_)));
  }

  [[nodiscard]] std::string printable(std::size_t at) const {
    const util::Utf8Decoded d = util::decode_utf8(text_, at);
    if (!d.ok) return "\\x" + std::format("{:02X}", static_cast<unsigned char>(text_[at]));
    if (d.code_point < 0x20) return std::format("U+{:04X}", static_cast<unsigned>(d.code_point));
    return std::string(text_.substr(at, d.length));
  }

  Result<Json> parse_word(std::string_view word, Json value) {
    if (text_.substr(pos_, word.size()) != word || is_identifier_continue(pos_ + word.size())) {
      return error(std::format("unexpected character '{}'; expected a value", printable(pos_)));
    }
    pos_ += word.size();
    return value;
  }

  [[nodiscard]] bool is_identifier_continue(std::size_t at) const noexcept {
    if (at >= text_.size()) return false;
    const char c = text_[at];
    return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '$';
  }

  Result<Json> parse_number() {
    const std::size_t start = pos_;
    bool negative = false;
    if (peek() == '-' || peek() == '+') {
      if (peek() == '+' && !dialect_.json5) return error("a number cannot start with '+' in JSON");
      negative = peek() == '-';
      ++pos_;
    }
    // Named values.
    if (peek() == 'I' || peek() == 'N') {
      const bool infinity = text_.substr(pos_, 8) == "Infinity";
      const bool nan = text_.substr(pos_, 3) == "NaN";
      const std::size_t length = infinity ? 8 : nan ? 3 : 0;
      if (length == 0 || is_identifier_continue(pos_ + length)) {
        return error(std::format("unexpected character '{}'; expected a value", printable(pos_)));
      }
      if (!dialect_.nan_inf && !dialect_.json5) {
        return error_at(start, "NaN and Infinity are not JSON (they are accepted unless the 'strict' option is set)");
      }
      used_extensions_ = used_extensions_ || !dialect_.json5;
      pos_ += length;
      if (nan) return Json(std::numeric_limits<double>::quiet_NaN());
      return Json(negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
    }
    // Hexadecimal (JSON5).
    if (dialect_.json5 && peek() == '0' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == 'x' || text_[pos_ + 1] == 'X')) {
      pos_ += 2;
      const std::size_t digits_start = pos_;
      while (pos_ < text_.size() && is_hex(text_[pos_])) ++pos_;
      if (pos_ == digits_start) return error("expected hexadecimal digits after 0x");
      if (is_identifier_continue(pos_)) return error("invalid character in a hexadecimal number");
      auto value = integer_from_digits(text_.substr(digits_start, pos_ - digits_start), 16, negative);
      if (!value) return error_at(start, "hexadecimal number is too long");
      return std::move(*value);
    }
    const std::size_t int_start = pos_;
    while (util::is_ascii_digit(peek())) ++pos_;
    const std::size_t int_digits = pos_ - int_start;
    if (int_digits > 1 && text_[int_start] == '0') return error_at(int_start, "numbers cannot have leading zeros");
    std::size_t frac_digits = 0;
    if (peek() == '.') {
      ++pos_;
      const std::size_t frac_start = pos_;
      while (util::is_ascii_digit(peek())) ++pos_;
      frac_digits = pos_ - frac_start;
      if (frac_digits == 0 && (!dialect_.json5 || int_digits == 0)) {
        return error("expected a digit after the decimal point");
      }
    }
    if (int_digits == 0) {
      if (!dialect_.json5 || frac_digits == 0) return error_at(start, "expected a digit");
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      const std::size_t exp_start = pos_;
      while (util::is_ascii_digit(peek())) ++pos_;
      if (pos_ == exp_start) return error("expected a digit in the exponent");
    }
    if (dialect_.json5 && is_identifier_continue(pos_)) return error("invalid character after a number");
    return number_from_literal(text_.substr(start, pos_ - start));
  }

  Result<std::uint32_t> hex4(std::size_t at) const {
    if (at + 4 > text_.size()) return error_at(at, "incomplete \\u escape");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const char c = text_[at + i];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return error_at(at + i, "invalid hexadecimal digit in a \\u escape");
      }
    }
    return value;
  }

  /// After "\u": a code unit, combined with a following low surrogate.
  Result<char32_t> unicode_escape() {
    CVT_TRY_ASSIGN(const std::uint32_t unit, hex4(pos_));
    pos_ += 4;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      if (text_.substr(pos_, 2) == "\\u") {
        auto low = hex4(pos_ + 2);
        if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
          pos_ += 6;
          return static_cast<char32_t>(0x10000 + ((unit - 0xD800) << 10) + (*low - 0xDC00));
        }
      }
      ++lone_surrogates_;
      return util::replacement_character;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      ++lone_surrogates_;
      return util::replacement_character;
    }
    return static_cast<char32_t>(unit);
  }

  Result<std::string> parse_string(char quote) {
    const std::size_t start = pos_;
    ++pos_;  // opening quote
    std::string out;
    while (true) {
      if (pos_ >= text_.size()) return error_at(start, "unterminated string");
      const char c = text_[pos_];
      if (c == quote) {
        ++pos_;
        return out;
      }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= text_.size()) return error_at(start, "unterminated string");
        const char e = text_[pos_++];
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
            CVT_TRY_ASSIGN(const char32_t cp, unicode_escape());
            util::append_utf8(out, cp);
            break;
          }
          default: {
            if (!dialect_.json5) return error_at(pos_ - 2, std::format("invalid escape '\\{}'", printable(pos_ - 1)));
            CVT_TRY(json5_escape(e, out));
          }
        }
        continue;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        if (c == '\n' || c == '\r') return error("line break inside a string (use \\n)");
        if (!dialect_.json5 || c != '\t') return error("control character inside a string (it must be escaped)");
      }
      // Copy a run of ordinary bytes (the text is valid UTF-8).
      const std::size_t run = pos_;
      while (pos_ < text_.size() && text_[pos_] != quote && text_[pos_] != '\\' &&
             static_cast<unsigned char>(text_[pos_]) >= 0x20) {
        ++pos_;
      }
      if (pos_ == run) {
        out.push_back(c);
        ++pos_;
      } else {
        out.append(text_.substr(run, pos_ - run));
      }
    }
  }

  Result<void> json5_escape(char e, std::string& out) {
    switch (e) {
      case '\'': out.push_back('\''); return {};
      case 'v': out.push_back('\v'); return {};
      case '0':
        if (util::is_ascii_digit(peek())) return error("octal escapes are not allowed");
        out.push_back('\0');
        return {};
      case 'x': {
        if (pos_ + 2 > text_.size() || !is_hex(text_[pos_]) ||
            !is_hex(text_[pos_ + 1])) {
          return error("invalid \\x escape");
        }
        const auto value = util::parse_int(text_.substr(pos_, 2), 16);
        pos_ += 2;
        util::append_utf8(out, static_cast<char32_t>(value.value_or(0)));
        return {};
      }
      case '\r':
        if (peek() == '\n') ++pos_;
        return {};
      case '\n': return {};
      default: break;
    }
    if (util::is_ascii_digit(e)) return error_at(pos_ - 2, "digits cannot be escaped");
    // Line separators continue the line; any other character stands for itself.
    --pos_;
    const util::Utf8Decoded d = util::decode_utf8(text_, pos_);
    if (d.ok && (d.code_point == 0x2028 || d.code_point == 0x2029)) {
      pos_ += d.length;
      return {};
    }
    out.append(text_.substr(pos_, d.length));
    pos_ += d.length;
    return {};
  }

  Result<Json> parse_string_value(char quote) {
    CVT_TRY_ASSIGN(std::string text, parse_string(quote));
    return Json(std::move(text));
  }

  /// JSON5 identifier key: ES5 IdentifierName (non-ASCII letters accepted
  /// leniently), with \uXXXX escapes.
  Result<std::string> parse_identifier() {
    std::string out;
    const std::size_t start = pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '\\') {
        if (text_.substr(pos_, 2) != "\\u") return error("invalid escape in an identifier");
        pos_ += 2;
        CVT_TRY_ASSIGN(const char32_t cp, unicode_escape());
        util::append_utf8(out, cp);
        continue;
      }
      if (util::is_ascii_alpha(c) || c == '_' || c == '$' || (util::is_ascii_digit(c) && !out.empty())) {
        out.push_back(c);
        ++pos_;
        continue;
      }
      if (static_cast<unsigned char>(c) >= 0x80 && unicode_space_length() == 0) {
        const util::Utf8Decoded d = util::decode_utf8(text_, pos_);
        out.append(text_.substr(pos_, d.length));
        pos_ += d.length;
        continue;
      }
      break;
    }
    if (out.empty()) return error_at(start, "expected a key");
    return out;
  }

  Result<Json> parse_object(std::size_t depth) {
    if (depth >= policy_.max_depth) {
      return fail(Status::Limit, "the {} data is nested more than {} levels deep (option max_depth)", dialect_.name,
                  policy_.max_depth);
    }
    ++pos_;  // {
    Json object = Json::object();
    CVT_TRY(skip_space());
    if (peek() == '}') {
      ++pos_;
      return object;
    }
    while (true) {
      std::string key;
      const std::size_t key_at = pos_;
      if (peek() == '"') {
        CVT_TRY_ASSIGN(key, parse_string('"'));
      } else if (peek() == '\'' && dialect_.json5) {
        CVT_TRY_ASSIGN(key, parse_string('\''));
      } else if (dialect_.json5) {
        CVT_TRY_ASSIGN(key, parse_identifier());
      } else if (peek() == '}') {
        return error("trailing comma before '}'");
      } else {
        return error(pos_ >= text_.size() ? "unexpected end of text; expected a key"
                                          : "expected a key in double quotes");
      }
      CVT_TRY(skip_space());
      if (peek() != ':') return error("expected ':' after the key");
      ++pos_;
      CVT_TRY(skip_space());
      CVT_TRY_ASSIGN(Json value, parse_value(depth + 1));
      if (auto put = put_member(object, std::move(key), std::move(value), policy_); !put) {
        return error_at(key_at, put.error().message);
      }
      CVT_TRY(skip_space());
      if (peek() == ',') {
        ++pos_;
        CVT_TRY(skip_space());
        if (peek() == '}') {
          if (!dialect_.trailing_commas && !dialect_.json5) return error("trailing comma before '}'");
          used_extensions_ = used_extensions_ || !dialect_.json5;
          ++pos_;
          return object;
        }
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        return object;
      }
      return error(pos_ >= text_.size() ? "unexpected end of text; expected ',' or '}'" : "expected ',' or '}'");
    }
  }

  Result<Json> parse_array(std::size_t depth) {
    if (depth >= policy_.max_depth) {
      return fail(Status::Limit, "the {} data is nested more than {} levels deep (option max_depth)", dialect_.name,
                  policy_.max_depth);
    }
    ++pos_;  // [
    Json array = Json::array();
    CVT_TRY(skip_space());
    if (peek() == ']') {
      ++pos_;
      return array;
    }
    while (true) {
      CVT_TRY_ASSIGN(Json value, parse_value(depth + 1));
      array.push_back(std::move(value));
      CVT_TRY(skip_space());
      if (peek() == ',') {
        ++pos_;
        CVT_TRY(skip_space());
        if (peek() == ']') {
          if (!dialect_.trailing_commas && !dialect_.json5) return error("trailing comma before ']'");
          used_extensions_ = used_extensions_ || !dialect_.json5;
          ++pos_;
          return array;
        }
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        return array;
      }
      return error(pos_ >= text_.size() ? "unexpected end of text; expected ',' or ']'" : "expected ',' or ']'");
    }
  }

  std::string_view text_;
  Dialect dialect_;
  ReadPolicy policy_;
  std::size_t pos_ = 0;
  std::size_t lone_surrogates_ = 0;
  bool used_extensions_ = false;
};

void note_surrogates(const JsonParser& parser, const Context& ctx) {
  if (parser.lone_surrogates() > 0) {
    ctx.warn(std::format("{} unpaired UTF-16 surrogate escape(s) were replaced with U+FFFD.",
                         parser.lone_surrogates()));
  }
}

Result<Json> parse_json_text(std::string_view text, ReadContext& ctx) {
  Dialect dialect;
  const bool strict = ctx.options.get_bool("strict", false);
  dialect.comments = !strict;
  dialect.trailing_commas = !strict;
  dialect.nan_inf = !strict;
  JsonParser parser(text, dialect, read_policy(ctx.options));
  CVT_TRY_ASSIGN(Json value, parser.parse_document());
  if (parser.used_extensions()) {
    ctx.warn("The JSON text uses extensions (comments, trailing commas, NaN or Infinity) that strict JSON forbids.");
  }
  note_surrogates(parser, ctx);
  return value;
}

Result<Json> parse_jsonc_text(std::string_view text, ReadContext& ctx) {
  Dialect dialect{.name = "JSONC", .comments = true, .trailing_commas = true, .nan_inf = false, .json5 = false};
  JsonParser parser(text, dialect, read_policy(ctx.options));
  CVT_TRY_ASSIGN(Json value, parser.parse_document());
  note_surrogates(parser, ctx);
  return value;
}

Result<Json> parse_json5_text(std::string_view text, ReadContext& ctx) {
  Dialect dialect{.name = "JSON5", .comments = true, .trailing_commas = true, .nan_inf = true, .json5 = true};
  JsonParser parser(text, dialect, read_policy(ctx.options));
  CVT_TRY_ASSIGN(Json value, parser.parse_document());
  note_surrogates(parser, ctx);
  return value;
}

Result<Json> parse_ndjson_text(std::string_view text, ReadContext& ctx) {
  Dialect dialect{.name = "NDJSON", .comments = false, .trailing_commas = false, .nan_inf = true, .json5 = false};
  JsonParser parser(text, dialect, read_policy(ctx.options));
  Json out = Json::array();
  std::size_t pos = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
  std::size_t records = 0;
  while (pos < text.size()) {
    std::size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view line = text.substr(pos, end - pos);
    if (!util::trim(line).empty()) {
      CVT_TRY_ASSIGN(Json value, parser.parse_at(pos));
      // The rest of the line must be blank.
      std::size_t after = parser.pos();
      while (after < end && (text[after] == ' ' || text[after] == '\t' || text[after] == '\r')) ++after;
      if (after < end) return syntax_error("NDJSON", text, after, "unexpected content after the value on this line");
      if (parser.pos() > end) return syntax_error("NDJSON", text, pos, "a value must fit on one line");
      out.push_back(std::move(value));
      if ((++records & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(text.size())));
    }
    pos = end + 1;
  }
  note_surrogates(parser, ctx);
  return out;
}

Result<Json> parse_json_seq_text(std::string_view text, ReadContext& ctx) {
  Dialect dialect{.name = "JSON text sequence", .comments = false, .trailing_commas = false, .nan_inf = false,
                  .json5 = false};
  JsonParser parser(text, dialect, read_policy(ctx.options));
  Json out = Json::array();
  std::size_t pos = 0;
  std::size_t skipped = 0;
  while (pos < text.size()) {
    const std::size_t rs = text.find('\x1E', pos);
    if (rs == std::string_view::npos) {
      if (!util::trim(text.substr(pos)).empty()) {
        return syntax_error(dialect.name, text, pos, "expected a record separator (0x1E)");
      }
      break;
    }
    if (!util::trim(text.substr(pos, rs - pos)).empty()) {
      return syntax_error(dialect.name, text, pos, "content outside a record");
    }
    std::size_t end = text.find('\x1E', rs + 1);
    if (end == std::string_view::npos) end = text.size();
    // RFC 7464: a record that does not parse is skipped, not fatal.
    auto value = parser.parse_at(rs + 1);
    if (value) {
      std::size_t after = parser.pos();
      while (after < end && util::is_ascii_space(text[after])) ++after;
      if (after == end) {
        out.push_back(std::move(*value));
      } else {
        ++skipped;
      }
    } else if (value.error().code == Status::Limit) {
      return std::unexpected(std::move(value).error());
    } else {
      ++skipped;
    }
    pos = end;
  }
  if (skipped > 0) ctx.warn(std::format("{} malformed record(s) of the JSON text sequence were skipped.", skipped));
  note_surrogates(parser, ctx);
  return out;
}

// =========================================================================== writer

bool is_json5_identifier(std::string_view key) {
  if (key.empty()) return false;
  static constexpr std::string_view reserved[] = {
      "break",  "case",     "catch",   "class", "const", "continue", "debugger", "default", "delete",
      "do",     "else",     "enum",    "export", "extends", "false",  "finally",  "for",     "function",
      "if",     "import",   "in",      "instanceof", "new", "null",   "return",   "super",   "switch",
      "this",   "throw",    "true",    "try",   "typeof", "var",     "void",     "while",   "with"};
  if (std::ranges::find(reserved, key) != std::end(reserved)) return false;
  for (std::size_t i = 0; i < key.size(); ++i) {
    const char c = key[i];
    if (!(util::is_ascii_alpha(c) || c == '_' || c == '$' || (i > 0 && util::is_ascii_digit(c)))) return false;
  }
  return true;
}

class JsonWriter {
 public:
  explicit JsonWriter(const JsonStyle& style) : style_(style) {}

  Result<std::string> run(const Json& value) {
    CVT_TRY(write(value, 0));
    if (style_.indent >= 0) out_.append(style_.newline);
    return std::move(out_);
  }

 private:
  void newline(std::size_t depth) {
    if (style_.indent < 0) return;
    out_.append(style_.newline);
    out_.append(depth * static_cast<std::size_t>(style_.indent), ' ');
  }

  void string(std::string_view text) {
    const std::size_t start = out_.size();
    append_json_string(out_, text, style_.ascii_only);
    if (style_.escape_slash) {
      std::string escaped;
      for (std::size_t i = start; i < out_.size(); ++i) {
        if (out_[i] == '/') escaped += "\\/";
        else escaped.push_back(out_[i]);
      }
      out_.resize(start);
      out_ += escaped;
    }
  }

  Result<void> non_finite(double value, std::size_t depth) {
    const std::string text = format_double(value);
    switch (style_.non_finite) {
      case JsonStyle::NonFinite::Tagged:
        if (style_.json5) {
          out_ += text;
          return {};
        }
        out_.push_back('{');
        newline(depth + 1);
        string(ir::data_tags::float_value);
        out_ += style_.indent < 0 ? ":" : ": ";
        string(text);
        newline(depth);
        out_.push_back('}');
        return {};
      case JsonStyle::NonFinite::Null: out_ += style_.json5 ? text : "null"; return {};
      case JsonStyle::NonFinite::Literal: out_ += text; return {};
      case JsonStyle::NonFinite::String: string(text); return {};
      case JsonStyle::NonFinite::Error:
        if (style_.json5) {
          out_ += text;
          return {};
        }
        return fail(Status::Encode, "{} cannot be written as JSON (option non_finite is \"error\")", text);
    }
    return {};
  }

  Result<void> write(const Json& value, std::size_t depth) {
    switch (value.type()) {
      case Json::Type::Null: out_ += "null"; return {};
      case Json::Type::Bool: out_ += value.as_bool() ? "true" : "false"; return {};
      case Json::Type::Int64:
      case Json::Type::UInt64: out_ += format_integer(value); return {};
      case Json::Type::Double: {
        const double d = value.as_double();
        if (!std::isfinite(d)) return non_finite(d, depth);
        out_ += format_double(d);
        return {};
      }
      case Json::Type::String: string(value.as_string()); return {};
      case Json::Type::Array: {
        if (value.empty()) {
          out_ += "[]";
          return {};
        }
        out_.push_back('[');
        bool first = true;
        for (const Json& element : value.elements()) {
          if (!first) out_.push_back(',');
          first = false;
          newline(depth + 1);
          CVT_TRY(write(element, depth + 1));
        }
        newline(depth);
        out_.push_back(']');
        return {};
      }
      case Json::Type::Object: break;
    }
    switch (tag_kind(value)) {
      case TagKind::BigInt:
      case TagKind::Decimal: out_ += tag_text(value); return {};
      case TagKind::Float: return non_finite(plain_value(value).as_double(), depth);
      default: break;
    }
    if (value.empty()) {
      out_ += "{}";
      return {};
    }
    out_.push_back('{');
    bool first = true;
    for (const Json::Member* member : members_of(value, style_.sort_keys)) {
      if (!first) out_.push_back(',');
      first = false;
      newline(depth + 1);
      if (style_.json5 && is_json5_identifier(member->key())) {
        out_ += member->key();
      } else {
        string(member->key());
      }
      out_ += style_.indent < 0 ? ":" : ": ";
      CVT_TRY(write(member->value(), depth + 1));
    }
    newline(depth);
    out_.push_back('}');
    return {};
  }

  const JsonStyle& style_;
  std::string out_;
};

JsonStyle style_from(const WriteContext& ctx, bool json5) {
  JsonStyle style;
  style.json5 = json5;
  const bool minify_default = same_format(ctx) && !ctx.options.was_set("indent");
  const bool minify = ctx.options.was_set("minify") ? ctx.options.get_bool("minify", false) : minify_default;
  style.indent = minify ? -1 : static_cast<int>(ctx.options.get_int("indent", 2, 0, 16));
  style.sort_keys = ctx.options.get_bool("sort_keys", false);
  style.ascii_only = ctx.options.get_bool("ascii_only", false);
  style.escape_slash = ctx.options.get_bool("escape_slash", false);
  const std::string non_finite =
      ctx.options.get_enum("non_finite", "tagged", {"tagged", "null", "literal", "string", "error"});
  style.non_finite = non_finite == "null"      ? JsonStyle::NonFinite::Null
                     : non_finite == "literal" ? JsonStyle::NonFinite::Literal
                     : non_finite == "string"  ? JsonStyle::NonFinite::String
                     : non_finite == "error"   ? JsonStyle::NonFinite::Error
                                               : JsonStyle::NonFinite::Tagged;
  return style;
}

/// Applies the `tagged_values` option before writing.
Result<Json> prepared(const Json& value, const WriteContext& ctx, std::string_view format_name) {
  const std::string mode = ctx.options.get_enum("tagged_values", "keep", {"keep", "plain", "error"});
  if (mode == "keep") return value;
  if (mode == "error") {
    std::vector<const Json*> stack{&value};
    while (!stack.empty()) {
      const Json* node = stack.back();
      stack.pop_back();
      const TagKind kind = tag_kind(*node);
      if (kind != TagKind::None && kind != TagKind::BigInt && kind != TagKind::Decimal && kind != TagKind::Float) {
        return fail(Status::Encode, "the data holds a value {} cannot express natively (option tagged_values is "
                    "\"error\")", format_name);
      }
      for (const Json& element : node->elements()) stack.push_back(&element);
      for (const auto& member : node->items()) stack.push_back(&member.value());
    }
    return value;
  }
  return untag(value, true);
}

Result<std::string> write_json(const Json& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const Json ready, prepared(value, ctx, "JSON"));
  return write_json_text(ready, style_from(ctx, false));
}

Result<std::string> write_json5(const Json& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const Json ready, prepared(value, ctx, "JSON5"));
  JsonStyle style = style_from(ctx, true);
  return write_json_text(ready, style);
}

Result<std::string> write_lines(const Json& value, WriteContext& ctx, std::string_view prefix) {
  CVT_TRY_ASSIGN(const Json ready, prepared(value, ctx, "NDJSON"));
  JsonStyle style = style_from(ctx, false);
  style.indent = -1;
  std::string out;
  const auto one = [&](const Json& record) -> Result<void> {
    CVT_TRY_ASSIGN(const std::string line, write_json_text(record, style));
    out += prefix;
    out += line;
    out.push_back('\n');
    return {};
  };
  if (ready.is_array()) {
    for (const Json& record : ready.elements()) CVT_TRY(one(record));
  } else {
    CVT_TRY(one(ready));
  }
  return out;
}

Result<std::string> write_ndjson(const Json& value, WriteContext& ctx) { return write_lines(value, ctx, ""); }
Result<std::string> write_json_seq(const Json& value, WriteContext& ctx) { return write_lines(value, ctx, "\x1E"); }

// =========================================================================== registration

std::vector<OptionSpec> json_write_options(bool pretty_options) {
  std::vector<OptionSpec> specs;
  if (pretty_options) {
    specs.push_back(OptionSpec::boolean("minify", "Minify", false,
                                        "Write everything on one line without spaces (the default when converting a "
                                        "file to its own format)."));
    specs.push_back(indent_option(2));
  }
  specs.push_back(sort_keys_option());
  specs.push_back(OptionSpec::boolean("ascii_only", "ASCII only", false,
                                      "Escape every non-ASCII character as \\uXXXX so the output is pure ASCII."));
  specs.push_back(OptionSpec::choice("non_finite", "NaN and infinity", "tagged",
                                     {"tagged", "null", "literal", "string", "error"},
                                     "How NaN and infinite numbers are written, since JSON has no spelling for "
                                     "them: {\"$float\": \"NaN\"}, null, the bare literal NaN (not valid JSON), a "
                                     "string, or an error.")
                      .advanced());
  specs.push_back(OptionSpec::choice("tagged_values", "Tagged values", "keep", {"keep", "plain", "error"},
                                     "Binary data, dates and other values JSON cannot express: keep them as tagged "
                                     "objects ({\"$binary\": ...}), write them as plain strings, or fail.")
                      .advanced());
  specs.push_back(OptionSpec::boolean("escape_slash", "Escape slashes", false,
                                      "Write '/' as '\\/' (safe inside HTML script elements).")
                      .advanced());
  return specs;
}

int sniff_json(ByteView data, std::string_view filename) {
  const std::string_view head = text_head(data);
  if (head.empty()) return 0;
  const char c = head.front();
  const bool shape = c == '{' || c == '[';
  if (has_extension(filename, {"json"})) return shape ? 72 : 60;
  if (!shape) return 0;
  // Many formats are JSON underneath (GeoJSON, glTF, notebooks, ...): when the
  // file name has an extension, stay below an extension-only match (78) so
  // that the specific format wins, but above plain text (60).
  const bool other_extension = filename.find('.') != std::string_view::npos;
  // Small enough to check completely: a document that parses is JSON.
  if (data.size() <= (std::size_t{1} << 20)) {
    util::JsonParseOptions options;
    options.max_depth = 64;
    if (util::parse_json(as_chars(data), options)) {
      // A JSON dialect the shared classifier recognises by its keys (a
      // notebook, a web manifest, ...) is that format, not plain JSON.
      const sniff::Verdict dialect = sniff::classify(sniff::Family::Json, data, filename, "json");
      if (!dialect.id.empty() && dialect.id != "json" && !dialect.base) return 70;
      return other_extension ? 70 : 85;
    }
  }
  // An object should continue with a quoted key or close.
  const std::string_view rest = util::trim_left(head.substr(1));
  if (c == '{') return rest.starts_with('"') || rest.starts_with('}') ? 62 : 0;
  return rest.empty() || rest.front() == '{' || rest.front() == '[' || rest.front() == '"' || rest.front() == ']' ||
                 rest.front() == '-' || util::is_ascii_digit(rest.front()) || rest.starts_with("true") ||
                 rest.starts_with("false") || rest.starts_with("null")
             ? 50
             : 0;
}

int sniff_ndjson(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"jsonl", "ndjson"})) return 90;
  // Two or more lines that each start with '{' and end with '}'.
  const std::string_view head = as_chars(data).substr(0, 4096);
  std::size_t lines = 0;
  std::size_t pos = 0;
  while (pos < head.size() && lines < 3) {
    const std::size_t end = head.find('\n', pos);
    if (end == std::string_view::npos) break;
    const std::string_view line = util::trim(head.substr(pos, end - pos));
    if (!line.empty()) {
      if (!line.starts_with('{') || !line.ends_with('}')) return 0;
      ++lines;
    }
    pos = end + 1;
  }
  return lines >= 2 ? 70 : 0;
}

int sniff_json5(ByteView /*data*/, std::string_view filename) {
  if (has_extension(filename, {"json5"})) return 75;
  return 0;
}

int sniff_jsonc(ByteView /*data*/, std::string_view filename) {
  if (has_extension(filename, {"jsonc"})) return 75;
  return 0;
}

}  // namespace

Result<std::string> write_json_text(const Json& value, const JsonStyle& style) {
  JsonWriter writer(style);
  return writer.run(value);
}

void register_json_formats(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "json",
      .name = "JSON",
      .description = "JavaScript Object Notation, the common interchange format for structured data.",
      .category = Category::Data,
      .extensions = {"json"},
      .mime = "application/json",
      .popularity = 92,
      .signatures = {},
      .sniff = sniff_json,
  });
  std::vector<OptionSpec> json_read = common_read_options();
  json_read.insert(json_read.begin(),
                   OptionSpec::boolean("strict", "Strict syntax", false,
                                       "Reject comments, trailing commas, NaN and Infinity instead of accepting them.")
                       .advanced());
  add_codec(registry, {.format_id = "json",
                       .read = text_reader(parse_json_text, "JSON"),
                       .write = text_writer(write_json, "JSON"),
                       .read_options = json_read,
                       .write_options = json_write_options(true)});
  // JSON dialects with ids of their own (detected by their keys in the
  // detection database): read as the JSON they are, so they convert to
  // everything JSON converts to.
  for (const char* dialect : {"har", "jsonld", "webmanifest"}) {
    add_codec(registry, {.format_id = dialect, .read = text_reader(parse_json_text, "JSON"), .read_options = json_read});
  }

  registry.add_format(FormatInfo{
      .id = "jsonc",
      .name = "JSON with comments",
      .description = "JSON that allows // and /* */ comments and trailing commas (editor settings files).",
      .category = Category::Data,
      .extensions = {"jsonc"},
      .mime = "application/json",
      .popularity = 35,
      .signatures = {},
      .sniff = sniff_jsonc,
  });
  add_codec(registry, {.format_id = "jsonc",
                       .read = text_reader(parse_jsonc_text, "JSONC"),
                       .write = text_writer(write_json, "JSONC"),
                       .read_options = common_read_options(),
                       .write_options = json_write_options(true)});

  registry.add_format(FormatInfo{
      .id = "json5",
      .name = "JSON5",
      .description = "JSON for humans: comments, unquoted keys, single quotes, hexadecimal numbers and more.",
      .category = Category::Data,
      .extensions = {"json5"},
      .mime = "application/json5",
      .popularity = 30,
      .signatures = {},
      .sniff = sniff_json5,
  });
  add_codec(registry, {.format_id = "json5",
                       .read = text_reader(parse_json5_text, "JSON5"),
                       .write = text_writer(write_json5, "JSON5"),
                       .read_options = common_read_options(),
                       .write_options = json_write_options(true)});

  registry.add_format(FormatInfo{
      .id = "jsonl",
      .name = "Newline-delimited JSON",
      .description = "One JSON value per line (JSON Lines), common for logs and data exports.",
      .category = Category::Data,
      .extensions = {"jsonl", "ndjson"},
      .mime = "application/x-ndjson",
      .popularity = 45,
      .signatures = {},
      .sniff = sniff_ndjson,
  });
  add_codec(registry, {.format_id = "jsonl",
                       .read = text_reader(parse_ndjson_text, "NDJSON"),
                       .write = text_writer(write_ndjson, "NDJSON"),
                       .read_options = common_read_options(),
                       .write_options = json_write_options(false)});

  registry.add_format(FormatInfo{
      .id = "json-seq",
      .name = "JSON text sequence",
      .description = "RFC 7464 sequence of JSON texts, each introduced by a record separator.",
      .category = Category::Data,
      .extensions = {"json-seq"},
      .mime = "application/json-seq",
      .popularity = 10,
      .signatures = {Signature::at(0, "1E '{'"), Signature::at(0, "1E '['")},
      .sniff = nullptr,
  });
  add_codec(registry, {.format_id = "json-seq",
                       .read = text_reader(parse_json_seq_text, "JSON text sequence"),
                       .write = text_writer(write_json_seq, "JSON text sequence"),
                       .read_options = common_read_options(),
                       .write_options = json_write_options(false)});
}

}  // namespace cvt::data
