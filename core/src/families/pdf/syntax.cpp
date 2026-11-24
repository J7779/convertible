// Convertible core - pdf family: objects, tokenizer, parser, serializer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/syntax.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>

#include "families/pdf/font_data.hpp"
#include "util/text.hpp"

namespace cvt::pdf {

// --------------------------------------------------------------------------- Object

namespace {
const std::string& empty_string() {
  static const std::string value;
  return value;
}
const Array& empty_array() {
  static const Array value;
  return value;
}
const Dict& empty_dict() {
  static const Dict value;
  return value;
}
const Object& null_object() {
  static const Object value;
  return value;
}
}  // namespace

std::int64_t Object::as_int(std::int64_t fallback) const noexcept {
  if (const auto* i = std::get_if<std::int64_t>(&value_)) return *i;
  if (const auto* d = std::get_if<double>(&value_)) {
    if (std::isfinite(*d) && std::abs(*d) < 9.0e18) return static_cast<std::int64_t>(std::llround(*d));
  }
  return fallback;
}

double Object::as_number(double fallback) const noexcept {
  if (const auto* i = std::get_if<std::int64_t>(&value_)) return static_cast<double>(*i);
  if (const auto* d = std::get_if<double>(&value_)) return std::isfinite(*d) ? *d : fallback;
  return fallback;
}

const std::string& Object::as_name() const noexcept {
  if (const auto* n = std::get_if<Name>(&value_)) return n->value;
  return empty_string();
}
const std::string& Object::as_string() const noexcept {
  if (const auto* s = std::get_if<String>(&value_)) return s->bytes;
  return empty_string();
}
const Array& Object::as_array() const noexcept {
  if (const auto* a = std::get_if<std::shared_ptr<const Array>>(&value_)) return **a;
  return empty_array();
}
const Dict& Object::as_dict() const noexcept {
  if (const auto* d = std::get_if<std::shared_ptr<const Dict>>(&value_)) return **d;
  if (const auto* s = std::get_if<std::shared_ptr<const Stream>>(&value_)) return (*s)->dict;
  return empty_dict();
}
const Stream* Object::as_stream() const noexcept {
  if (const auto* s = std::get_if<std::shared_ptr<const Stream>>(&value_)) return s->get();
  return nullptr;
}
std::shared_ptr<const Stream> Object::stream_ptr() const noexcept {
  if (const auto* s = std::get_if<std::shared_ptr<const Stream>>(&value_)) return *s;
  return nullptr;
}
std::shared_ptr<const Dict> Object::dict_ptr() const noexcept {
  if (const auto* d = std::get_if<std::shared_ptr<const Dict>>(&value_)) return *d;
  return nullptr;
}
std::shared_ptr<const Array> Object::array_ptr() const noexcept {
  if (const auto* a = std::get_if<std::shared_ptr<const Array>>(&value_)) return *a;
  return nullptr;
}

const Object& Dict::get(std::string_view key) const noexcept {
  const Object* o = find(key);
  return o != nullptr ? *o : null_object();
}

// --------------------------------------------------------------------------- serializer

std::string format_number(double value) {
  if (!std::isfinite(value)) return "0";
  if (std::abs(value) < 1e-6) return "0";
  const double rounded = std::round(value);
  if (std::abs(value - rounded) < 1e-6 && std::abs(rounded) < 1e15) {
    char buffer[32];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, static_cast<std::int64_t>(rounded));
    return std::string(buffer, ec == std::errc{} ? end : buffer);
  }
  char buffer[64];
  const auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed, 5);
  std::string s(buffer, ec == std::errc{} ? end : buffer);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0" || s.empty()) s = "0";
  return s;
}

namespace {

void serialize_name(std::string_view name, std::string& out) {
  out.push_back('/');
  static constexpr char hex[] = "0123456789ABCDEF";
  for (const char ch : name) {
    const auto c = static_cast<std::uint8_t>(ch);
    if (c < 0x21 || c > 0x7E || c == '#' || is_pdf_delimiter(c)) {
      out.push_back('#');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    } else {
      out.push_back(ch);
    }
  }
}

void serialize_string(const String& s, std::string& out) {
  // Literal form when it is mostly printable, hex otherwise.
  std::size_t binary = 0;
  for (const char ch : s.bytes) {
    const auto c = static_cast<std::uint8_t>(ch);
    if (c < 0x20 || c > 0x7E) ++binary;
  }
  if (s.hex || binary * 4 > s.bytes.size()) {
    static constexpr char hex[] = "0123456789ABCDEF";
    out.push_back('<');
    for (const char ch : s.bytes) {
      const auto c = static_cast<std::uint8_t>(ch);
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
    out.push_back('>');
    return;
  }
  out.push_back('(');
  for (const char ch : s.bytes) {
    const auto c = static_cast<std::uint8_t>(ch);
    switch (c) {
      case '(': out += "\\("; break;
      case ')': out += "\\)"; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20 || c > 0x7E) {
          char buffer[8];
          std::snprintf(buffer, sizeof buffer, "\\%03o", c);
          out += buffer;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back(')');
}

void serialize_impl(const Object& object, std::string& out, int depth) {
  if (depth > 256) {
    out += "null";
    return;
  }
  switch (object.type()) {
    case Object::Type::Null: out += "null"; break;
    case Object::Type::Bool: out += object.as_bool() ? "true" : "false"; break;
    case Object::Type::Int: out += std::to_string(object.as_int()); break;
    case Object::Type::Real: out += format_number(object.as_number()); break;
    case Object::Type::String: serialize_string(*object.string_object(), out); break;
    case Object::Type::Name: serialize_name(object.as_name(), out); break;
    case Object::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const Object& item : object.as_array()) {
        if (!first) out.push_back(' ');
        first = false;
        serialize_impl(item, out, depth + 1);
      }
      out.push_back(']');
      break;
    }
    case Object::Type::Dict:
    case Object::Type::Stream: {
      out += "<<";
      for (const auto& [key, value] : object.as_dict()) {
        serialize_name(key, out);
        const Object::Type t = value.type();
        if (t != Object::Type::Name && t != Object::Type::Array && t != Object::Type::Dict &&
            t != Object::Type::String) {
          out.push_back(' ');
        }
        serialize_impl(value, out, depth + 1);
      }
      out += ">>";
      break;
    }
    case Object::Type::Ref: {
      const Ref r = object.as_ref();
      out += std::to_string(r.num);
      out.push_back(' ');
      out += std::to_string(r.gen);
      out += " R";
      break;
    }
  }
}

}  // namespace

void serialize(const Object& object, std::string& out) { serialize_impl(object, out, 0); }

// --------------------------------------------------------------------------- lexer

void Lexer::skip_whitespace() noexcept {
  while (pos_ < data_.size()) {
    const std::uint8_t c = byte_at(pos_);
    if (is_pdf_space(c)) {
      ++pos_;
    } else if (c == '%') {
      while (pos_ < data_.size() && byte_at(pos_) != '\n' && byte_at(pos_) != '\r') ++pos_;
    } else {
      break;
    }
  }
}

Token Lexer::peek() {
  const std::size_t saved = pos_;
  Token t = next();
  pos_ = saved;
  return t;
}

namespace {
int hex_value(std::uint8_t c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

Token Lexer::next() {
  skip_whitespace();
  Token token;
  token.position = pos_;
  if (pos_ >= data_.size()) return token;
  const std::uint8_t c = byte_at(pos_);
  switch (c) {
    case '[': ++pos_; token.kind = Token::Kind::ArrayOpen; return token;
    case ']': ++pos_; token.kind = Token::Kind::ArrayClose; return token;
    case '{': ++pos_; token.kind = Token::Kind::BraceOpen; return token;
    case '}': ++pos_; token.kind = Token::Kind::BraceClose; return token;
    case '<':
      if (byte_at(pos_ + 1) == '<') {
        pos_ += 2;
        token.kind = Token::Kind::DictOpen;
        return token;
      } else {
        ++pos_;
        token.kind = Token::Kind::HexString;
        int high = -1;
        while (pos_ < data_.size()) {
          const std::uint8_t h = byte_at(pos_++);
          if (h == '>') break;
          const int v = hex_value(h);
          if (v < 0) continue;
          if (high < 0) {
            high = v;
          } else {
            token.text.push_back(static_cast<char>(high * 16 + v));
            high = -1;
          }
        }
        if (high >= 0) token.text.push_back(static_cast<char>(high * 16));
        return token;
      }
    case '>':
      if (byte_at(pos_ + 1) == '>') {
        pos_ += 2;
        token.kind = Token::Kind::DictClose;
        return token;
      }
      ++pos_;
      token.kind = Token::Kind::Error;
      return token;
    case ')':
      ++pos_;
      token.kind = Token::Kind::Error;
      return token;
    case '(': {
      ++pos_;
      token.kind = Token::Kind::String;
      int depth = 1;
      while (pos_ < data_.size()) {
        std::uint8_t ch = byte_at(pos_++);
        if (ch == '(') {
          ++depth;
        } else if (ch == ')') {
          if (--depth == 0) break;
        } else if (ch == '\\') {
          if (pos_ >= data_.size()) break;
          ch = byte_at(pos_++);
          switch (ch) {
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case '\r':
              if (byte_at(pos_) == '\n') ++pos_;
              continue;
            case '\n': continue;
            default:
              if (ch >= '0' && ch <= '7') {
                int value = ch - '0';
                for (int k = 0; k < 2 && byte_at(pos_) >= '0' && byte_at(pos_) <= '7' && pos_ < data_.size(); ++k) {
                  value = value * 8 + (byte_at(pos_++) - '0');
                }
                ch = static_cast<std::uint8_t>(value & 0xFF);
              }
          }
          token.text.push_back(static_cast<char>(ch));
          continue;
        } else if (ch == '\r') {
          // End-of-line in a literal string is a single \n.
          if (byte_at(pos_) == '\n') ++pos_;
          ch = '\n';
        }
        token.text.push_back(static_cast<char>(ch));
      }
      return token;
    }
    case '/': {
      ++pos_;
      token.kind = Token::Kind::Name;
      while (pos_ < data_.size() && is_pdf_regular(byte_at(pos_))) {
        const std::uint8_t ch = byte_at(pos_++);
        if (ch == '#' && hex_value(byte_at(pos_)) >= 0 && hex_value(byte_at(pos_ + 1)) >= 0 &&
            pos_ + 1 < data_.size()) {
          token.text.push_back(static_cast<char>(hex_value(byte_at(pos_)) * 16 + hex_value(byte_at(pos_ + 1))));
          pos_ += 2;
        } else {
          token.text.push_back(static_cast<char>(ch));
        }
      }
      return token;
    }
    default: break;
  }
  // Number or keyword.
  const std::size_t start = pos_;
  while (pos_ < data_.size() && is_pdf_regular(byte_at(pos_))) ++pos_;
  if (pos_ == start) {
    ++pos_;  // stray delimiter such as '}' or ')'
    token.kind = Token::Kind::Error;
    return token;
  }
  const std::string_view word(reinterpret_cast<const char*>(data_.data()) + start, pos_ - start);
  bool numeric = true;
  bool has_dot = false;
  bool has_digit = false;
  for (std::size_t i = 0; i < word.size(); ++i) {
    const char ch = word[i];
    if (ch >= '0' && ch <= '9') {
      has_digit = true;
    } else if (ch == '.') {
      has_dot = true;
    } else if ((ch == '-' || ch == '+') && (i == 0 || (i > 0 && (word[i - 1] == '-' || word[i - 1] == '+')))) {
      // leading sign (doubled signs such as "--5" occur in broken files)
    } else if (ch == '-' && i > 0) {
      // "12-3": some producers emit this; treat as the number before '-'.
      numeric = has_digit;
      if (numeric) {
        pos_ = start + i;
      }
      break;
    } else {
      numeric = false;
      break;
    }
  }
  if (numeric && has_digit) {
    std::string_view digits = word.substr(0, pos_ - start);
    while (digits.size() > 1 && (digits[0] == '+' || (digits[0] == '-' && (digits[1] == '-' || digits[1] == '+')))) {
      digits.remove_prefix(1);
    }
    if (!has_dot) {
      std::int64_t value = 0;
      std::string_view d = digits;
      bool negative = false;
      if (!d.empty() && d[0] == '-') {
        negative = true;
        d.remove_prefix(1);
      }
      const auto [ptr, ec] = std::from_chars(d.data(), d.data() + d.size(), value);
      if (ec == std::errc{} && ptr == d.data() + d.size()) {
        token.kind = Token::Kind::Integer;
        token.integer = negative ? -value : value;
        token.real = static_cast<double>(token.integer);
        return token;
      }
    }
    // Real: parse by hand (locale-free, tolerant of "-.5", "5.", "1.2.3").
    double value = 0.0;
    double scale = 0.0;
    bool negative = false;
    for (const char ch : digits) {
      if (ch == '-') {
        negative = true;
      } else if (ch == '.') {
        if (scale != 0.0) break;
        scale = 1.0;
      } else if (ch >= '0' && ch <= '9') {
        if (scale == 0.0) {
          value = value * 10.0 + (ch - '0');
        } else {
          scale *= 0.1;
          value += (ch - '0') * scale;
        }
      }
    }
    token.kind = Token::Kind::Real;
    token.real = negative ? -value : value;
    if (!std::isfinite(token.real)) token.real = 0.0;
    token.integer = static_cast<std::int64_t>(std::clamp(token.real, -9.0e18, 9.0e18));
    return token;
  }
  token.kind = Token::Kind::Keyword;
  token.text = std::string(word);
  return token;
}

// --------------------------------------------------------------------------- parser

namespace {

Object parse_after_token(Lexer& lexer, Token token, int depth, int max_depth, bool& ok, std::size_t& budget) {
  if (budget == 0) {
    ok = false;
    return {};
  }
  --budget;
  switch (token.kind) {
    case Token::Kind::Integer: {
      // Possibly "num gen R".
      if (token.integer >= 0 && token.integer <= 0x7FFFFFFF) {
        const std::size_t saved = lexer.position();
        const Token second = lexer.next();
        if (second.kind == Token::Kind::Integer && second.integer >= 0 && second.integer <= 65535) {
          const Token third = lexer.next();
          if (third.kind == Token::Kind::Keyword && third.text == "R") {
            return Ref{static_cast<std::uint32_t>(token.integer), static_cast<std::uint32_t>(second.integer)};
          }
        }
        lexer.seek(saved);
      }
      return token.integer;
    }
    case Token::Kind::Real: return token.real;
    case Token::Kind::String: return String{std::move(token.text), false};
    case Token::Kind::HexString: return String{std::move(token.text), true};
    case Token::Kind::Name: return Name{std::move(token.text)};
    case Token::Kind::Keyword:
      if (token.text == "true") return true;
      if (token.text == "false") return false;
      if (token.text == "null") return {};
      ok = false;
      return {};
    case Token::Kind::ArrayOpen: {
      Array array;
      if (depth >= max_depth) {
        ok = false;
        // Skip to the matching bracket without building anything.
        int level = 1;
        while (level > 0) {
          const Token t = lexer.next();
          if (t.kind == Token::Kind::End) break;
          if (t.kind == Token::Kind::ArrayOpen) ++level;
          if (t.kind == Token::Kind::ArrayClose) --level;
        }
        return array;
      }
      while (true) {
        Token t = lexer.next();
        if (t.kind == Token::Kind::ArrayClose || t.kind == Token::Kind::End) break;
        if (t.kind == Token::Kind::DictClose) {
          ok = false;
          break;
        }
        if (t.kind == Token::Kind::Keyword && (t.text == "endobj" || t.text == "stream" || t.text == "obj")) {
          lexer.seek(t.position);
          ok = false;
          break;
        }
        if (t.kind == Token::Kind::Error) continue;
        array.push_back(parse_after_token(lexer, std::move(t), depth + 1, max_depth, ok, budget));
        if (budget == 0) break;
      }
      return array;
    }
    case Token::Kind::DictOpen: {
      Dict dict;
      if (depth >= max_depth) {
        ok = false;
        int level = 1;
        while (level > 0) {
          const Token t = lexer.next();
          if (t.kind == Token::Kind::End) break;
          if (t.kind == Token::Kind::DictOpen) ++level;
          if (t.kind == Token::Kind::DictClose) --level;
        }
        return dict;
      }
      while (true) {
        Token key = lexer.next();
        if (key.kind == Token::Kind::DictClose || key.kind == Token::Kind::End) break;
        if (key.kind == Token::Kind::Keyword &&
            (key.text == "endobj" || key.text == "stream" || key.text == "obj" || key.text == "endstream")) {
          lexer.seek(key.position);
          ok = false;
          break;
        }
        if (key.kind != Token::Kind::Name) {
          ok = false;
          continue;  // skip junk between entries
        }
        Token value_token = lexer.next();
        if (value_token.kind == Token::Kind::DictClose) {
          dict.set(key.text, Object{});
          break;
        }
        if (value_token.kind == Token::Kind::Keyword &&
            (value_token.text == "endobj" || value_token.text == "stream")) {
          lexer.seek(value_token.position);
          ok = false;
          break;
        }
        Object value = parse_after_token(lexer, std::move(value_token), depth + 1, max_depth, ok, budget);
        // A null value means "absent" (PDF 32000 7.3.7).
        if (!value.is_null()) dict.set(key.text, std::move(value));
        if (budget == 0) break;
      }
      return dict;
    }
    default: ok = false; return {};
  }
}

}  // namespace

Object parse_object(Lexer& lexer, int max_depth, bool* ok) {
  bool good = true;
  std::size_t budget = 4'000'000;  // tokens per top-level object
  Token t = lexer.next();
  Object result = parse_after_token(lexer, std::move(t), 0, max_depth, good, budget);
  if (ok != nullptr) *ok = good;
  return result;
}

// --------------------------------------------------------------------------- text strings

std::string decode_text_string(std::string_view bytes) {
  std::string out;
  if (bytes.size() >= 2 && static_cast<std::uint8_t>(bytes[0]) == 0xFE && static_cast<std::uint8_t>(bytes[1]) == 0xFF) {
    std::u16string units;
    for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
      const char16_t u = static_cast<char16_t>((static_cast<std::uint8_t>(bytes[i]) << 8) |
                                               static_cast<std::uint8_t>(bytes[i + 1]));
      // Skip language escape sequences (U+001B ... U+001B).
      units.push_back(u);
    }
    std::u16string cleaned;
    bool in_escape = false;
    for (const char16_t u : units) {
      if (u == 0x1B) {
        in_escape = !in_escape;
        continue;
      }
      if (!in_escape) cleaned.push_back(u);
    }
    return util::utf16_to_utf8(cleaned);
  }
  if (bytes.size() >= 3 && static_cast<std::uint8_t>(bytes[0]) == 0xEF && static_cast<std::uint8_t>(bytes[1]) == 0xBB &&
      static_cast<std::uint8_t>(bytes[2]) == 0xBF) {
    return util::sanitize_utf8(bytes.substr(3));
  }
  for (const char ch : bytes) {
    const char32_t cp = font::pdfdoc_to_unicode(static_cast<std::uint8_t>(ch));
    if (cp != 0) util::append_utf8(out, cp);
  }
  return out;
}

std::string encode_text_string(std::string_view utf8) {
  bool ascii = true;
  for (const char ch : utf8) {
    const auto c = static_cast<std::uint8_t>(ch);
    if (c >= 0x80 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) ascii = false;
  }
  if (ascii) return std::string(utf8);
  const std::u16string units = util::utf8_to_utf16(utf8);
  std::string out = "\xFE\xFF";
  for (const char16_t u : units) {
    out.push_back(static_cast<char>(u >> 8));
    out.push_back(static_cast<char>(u & 0xFF));
  }
  return out;
}

}  // namespace cvt::pdf
