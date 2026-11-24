// Convertible core - data family: CSON (CoffeeScript Object Notation), read only.
//
// Indentation-based objects (key: value lines, nested blocks), braces and
// brackets that may span lines with or without commas, objects inside
// arrays (consecutive key lines form one object; a lone ',' starts the next),
// unquoted keys, single/double/triple-quoted strings (triple quotes are
// dedented), numbers in JavaScript notation, true/false/yes/no/on/off,
// null/undefined, and # / ### comments. Nothing is evaluated.
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

class CsonParser {
 public:
  CsonParser(std::string_view text, const ReadContext& ctx) : t_(text), policy_(read_policy(ctx.options)) {}

  Result<Json> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    CVT_TRY(skip_all());
    if (p_ >= t_.size()) return Json::object();
    Json value;
    if (key_ahead()) {
      CVT_TRY_ASSIGN(value, block_object(column(), 0));
    } else {
      CVT_TRY_ASSIGN(value, inline_value(0));
    }
    CVT_TRY(skip_all());
    if (p_ < t_.size()) return error("unexpected content");
    return value;
  }

 private:
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept { return p_ + ahead < t_.size() ? t_[p_ + ahead] : '\0'; }
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("CSON", t_, p_, message); }
  [[nodiscard]] std::size_t column() const {
    const std::size_t nl = p_ == 0 ? std::string_view::npos : t_.rfind('\n', p_ - 1);
    return p_ - (nl == std::string_view::npos ? 0 : nl + 1);
  }

  /// Blanks and a comment on this line.
  Result<void> skip_line() {
    while (peek() == ' ' || peek() == '\t' || peek() == '\r') ++p_;
    if (t_.substr(p_, 3) == "###") {
      const std::size_t end = t_.find("###", p_ + 3);
      if (end == std::string_view::npos) return error("unterminated ### comment");
      p_ = end + 3;
      return skip_line();
    }
    if (peek() == '#') {
      while (p_ < t_.size() && peek() != '\n') ++p_;
    }
    return {};
  }

  Result<void> skip_all() {
    while (true) {
      CVT_TRY(skip_line());
      if (peek() != '\n') return {};
      ++p_;
    }
  }

  [[nodiscard]] static bool ident_char(char c) { return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '$'; }

  /// key followed by ':' (and not '::') on this line?
  [[nodiscard]] bool key_ahead() const {
    std::size_t i = p_;
    if (i >= t_.size()) return false;
    if (t_[i] == '"' || t_[i] == '\'') {
      const char q = t_[i++];
      while (i < t_.size() && t_[i] != q && t_[i] != '\n') i += t_[i] == '\\' ? 2 : 1;
      if (i >= t_.size() || t_[i] != q) return false;
      ++i;
    } else {
      const std::size_t start = i;
      while (i < t_.size() && (ident_char(t_[i]) || t_[i] == '-' || t_[i] == '.')) ++i;
      if (i == start) return false;
    }
    while (i < t_.size() && (t_[i] == ' ' || t_[i] == '\t')) ++i;
    return i < t_.size() && t_[i] == ':' && (i + 1 >= t_.size() || t_[i + 1] != ':');
  }

  Result<std::string> quoted() {
    const char q = peek();
    if (t_.substr(p_, 3) == std::string(3, q)) {
      const std::size_t open_column = column();
      p_ += 3;
      const std::size_t end = t_.find(std::string(3, q), p_);
      if (end == std::string_view::npos) return error("unterminated triple-quoted string");
      std::string_view body = t_.substr(p_, end - p_);
      p_ = end + 3;
      if (const std::size_t nl = body.find('\n'); nl != std::string_view::npos && util::trim(body.substr(0, nl)).empty()) {
        body.remove_prefix(nl + 1);
      }
      if (const std::size_t last = body.rfind('\n'); last != std::string_view::npos && util::trim(body.substr(last + 1)).empty()) {
        body = body.substr(0, last);
      }
      // Remove the smallest indentation of the non-empty lines.
      std::size_t common = std::string_view::npos;
      for (const std::string_view line : util::split(body, '\n')) {
        if (util::trim(line).empty()) continue;
        std::size_t n = 0;
        while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) ++n;
        common = std::min(common, n);
      }
      (void)open_column;
      if (common == std::string_view::npos) common = 0;
      std::string out;
      bool first = true;
      for (const std::string_view line : util::split(body, '\n')) {
        if (!first) out.push_back('\n');
        first = false;
        out += line.size() >= common ? line.substr(common) : std::string_view();
      }
      return out;
    }
    ++p_;
    std::string out;
    while (true) {
      if (p_ >= t_.size() || peek() == '\n') return error("unterminated string");
      const char c = t_[p_++];
      if (c == q) return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      const char e = peek();
      ++p_;
      switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'v': out.push_back('\v'); break;
        case '0': out.push_back('\0'); break;
        case 'u': {
          const auto cp = p_ + 4 <= t_.size() ? util::parse_int(t_.substr(p_, 4), 16) : std::nullopt;
          if (!cp) return error("invalid \\u escape");
          p_ += 4;
          util::append_utf8(out, util::is_scalar_value(static_cast<char32_t>(*cp)) ? static_cast<char32_t>(*cp)
                                                                                   : util::replacement_character);
          break;
        }
        case 'x': {
          const auto cp = p_ + 2 <= t_.size() ? util::parse_int(t_.substr(p_, 2), 16) : std::nullopt;
          if (!cp) return error("invalid \\x escape");
          p_ += 2;
          util::append_utf8(out, static_cast<char32_t>(*cp));
          break;
        }
        default: out.push_back(e);
      }
    }
  }

  Result<std::string> key() {
    if (peek() == '"' || peek() == '\'') return quoted();
    const std::size_t start = p_;
    while (p_ < t_.size() && (ident_char(peek()) || peek() == '-' || peek() == '.')) ++p_;
    return std::string(t_.substr(start, p_ - start));
  }

  Result<Json> scalar_word() {
    const std::size_t start = p_;
    while (p_ < t_.size() && (ident_char(peek()) || peek() == '.' || peek() == '-' || peek() == '+')) ++p_;
    const std::string_view word = t_.substr(start, p_ - start);
    if (word == "true" || word == "yes" || word == "on") return Json(true);
    if (word == "false" || word == "no" || word == "off") return Json(false);
    if (word == "null" || word == "undefined") return Json();
    if (word == "Infinity" || word == "+Infinity") return Json(std::numeric_limits<double>::infinity());
    if (word == "-Infinity") return Json(-std::numeric_limits<double>::infinity());
    if (word == "NaN") return Json(std::numeric_limits<double>::quiet_NaN());
    std::string_view body = word;
    bool negative = false;
    if (body.starts_with('-') || body.starts_with('+')) {
      negative = body.front() == '-';
      body.remove_prefix(1);
    }
    if (body.size() > 2 && body[0] == '0' && std::string_view("xXoObB").find(body[1]) != std::string_view::npos) {
      const int base = (body[1] == 'x' || body[1] == 'X') ? 16 : (body[1] == 'o' || body[1] == 'O') ? 8 : 2;
      if (auto value = integer_from_digits(body.substr(2), base, negative)) return std::move(*value);
    }
    std::string literal(word);
    if (literal.starts_with('.')) literal.insert(0, "0");
    if (decimal_parts(literal)) return number_from_literal(literal);
    p_ = start;
    return error(std::format("unexpected '{}'", word.empty() ? std::string(1, peek()) : std::string(word)));
  }

  Result<Json> inline_value(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the CSON data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    const char c = peek();
    if (c == '"' || c == '\'') {
      CVT_TRY_ASSIGN(std::string text, quoted());
      return Json(std::move(text));
    }
    if (c == '[') return array(depth);
    if (c == '{') return braced_object(depth);
    return scalar_word();
  }

  Result<Json> array(std::size_t depth) {
    ++p_;
    Json list = Json::array();
    while (true) {
      CVT_TRY(skip_all());
      if (p_ >= t_.size()) return error("unterminated array");
      if (peek() == ']') {
        ++p_;
        return list;
      }
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (key_ahead()) {
        CVT_TRY_ASSIGN(Json object, block_object(column(), depth + 1));
        list.push_back(std::move(object));
        continue;
      }
      CVT_TRY_ASSIGN(Json v, inline_value(depth + 1));
      list.push_back(std::move(v));
    }
  }

  Result<Json> braced_object(std::size_t depth) {
    ++p_;
    Json object = Json::object();
    while (true) {
      CVT_TRY(skip_all());
      if (p_ >= t_.size()) return error("unterminated object");
      if (peek() == '}') {
        ++p_;
        return object;
      }
      if (peek() == ',') {
        ++p_;
        continue;
      }
      CVT_TRY_ASSIGN(std::string name, key());
      if (name.empty()) return error("expected a key");
      CVT_TRY(skip_line());
      if (peek() != ':') return error("expected ':' after a key");
      ++p_;
      CVT_TRY(skip_all());
      CVT_TRY_ASSIGN(Json v, inline_value(depth + 1));
      CVT_TRY(put_member(object, std::move(name), std::move(v), policy_));
    }
  }

  /// Lines "key: value" at exactly `indent`.
  Result<Json> block_object(std::size_t indent, std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the CSON data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    Json object = Json::object();
    while (true) {
      const std::size_t key_at = p_;
      CVT_TRY_ASSIGN(std::string name, key());
      CVT_TRY(skip_line());
      if (peek() != ':') return error("expected ':' after a key");
      ++p_;
      CVT_TRY(skip_line());
      Json v;
      if (p_ >= t_.size() || peek() == '\n') {
        CVT_TRY(skip_all());
        const std::size_t child = column();
        if (p_ >= t_.size() || child <= indent) {
          v = Json();
        } else if (key_ahead()) {
          CVT_TRY_ASSIGN(v, block_object(child, depth + 1));
        } else {
          CVT_TRY_ASSIGN(v, inline_value(depth + 1));
        }
      } else if (key_ahead()) {
        CVT_TRY_ASSIGN(v, block_object(column(), depth + 1));  // "a: b: 1"
      } else {
        CVT_TRY_ASSIGN(v, inline_value(depth + 1));
      }
      if (auto put = put_member(object, std::move(name), std::move(v), policy_); !put) {
        return syntax_error("CSON", t_, key_at, put.error().message);
      }
      CVT_TRY(skip_all());
      if (p_ >= t_.size() || peek() == ']' || peek() == ',' || peek() == '}') return object;
      const std::size_t col = column();
      if (col < indent) return object;
      if (col > indent) return error("bad indentation");
      if (!key_ahead()) return object;
    }
  }

  std::string_view t_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
};

Result<Json> parse_cson(std::string_view text, ReadContext& ctx) {
  CsonParser parser(text, ctx);
  return parser.parse();
}

}  // namespace

void register_cson(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "cson",
      .name = "CSON",
      .description = "CoffeeScript Object Notation, used for Atom editor settings and packages.",
      .category = Category::Data,
      .extensions = {"cson"},
      .mime = "text/x-cson",
      .popularity = 6,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"cson"}) ? 75 : 0; },
  });
  add_codec(registry, {.format_id = "cson",
                       .read = text_reader(parse_cson, "CSON"),
                       .write = nullptr,
                       .read_options = common_read_options(true, false)});
}

}  // namespace cvt::data
