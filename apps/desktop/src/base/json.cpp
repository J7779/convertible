// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "base/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cvd {

namespace {

const Json& null_json() {
  static const Json value;
  return value;
}

const std::string& empty_string() {
  static const std::string value;
  return value;
}

class Parser {
 public:
  explicit Parser(std::string_view text) : s_(text) {}

  std::optional<Json> run() {
    skip_ws();
    auto value = parse_value(0);
    if (!value) return std::nullopt;
    skip_ws();
    if (pos_ != s_.size()) return std::nullopt;
    return value;
  }

 private:
  void skip_ws() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }

  bool consume(std::string_view word) {
    if (s_.substr(pos_, word.size()) == word) {
      pos_ += word.size();
      return true;
    }
    return false;
  }

  std::optional<Json> parse_value(int depth) {
    if (depth > 256 || pos_ >= s_.size()) return std::nullopt;
    const char c = s_[pos_];
    if (c == '{') return parse_object(depth);
    if (c == '[') return parse_array(depth);
    if (c == '"') {
      auto str = parse_string();
      if (!str) return std::nullopt;
      return Json(std::move(*str));
    }
    if (consume("true")) return Json(true);
    if (consume("false")) return Json(false);
    if (consume("null")) return Json(nullptr);
    return parse_number();
  }

  std::optional<Json> parse_number() {
    const std::size_t start = pos_;
    if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
    if (pos_ >= s_.size() || !(s_[pos_] >= '0' && s_[pos_] <= '9')) return std::nullopt;
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        ++pos_;
      } else {
        break;
      }
    }
    // strtod: floating-point from_chars needs newer Apple runtimes than we target.
    // The app never changes the C locale, so '.' is the decimal point.
    const std::string text(s_.substr(start, pos_ - start));
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) return std::nullopt;
    return Json(value);
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  std::optional<std::uint32_t> parse_hex4() {
    if (pos_ + 4 > s_.size()) return std::nullopt;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
      else return std::nullopt;
    }
    return v;
  }

  std::optional<std::string> parse_string() {
    ++pos_;  // opening quote
    std::string out;
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) return std::nullopt;
      const char e = s_[pos_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          auto cp = parse_hex4();
          if (!cp) return std::nullopt;
          std::uint32_t code = *cp;
          if (code >= 0xD800 && code <= 0xDBFF) {
            if (pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
              pos_ += 2;
              auto lo = parse_hex4();
              if (!lo || *lo < 0xDC00 || *lo > 0xDFFF) return std::nullopt;
              code = 0x10000 + ((code - 0xD800) << 10) + (*lo - 0xDC00);
            } else {
              code = 0xFFFD;
            }
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            code = 0xFFFD;
          }
          append_utf8(out, code);
          break;
        }
        default: return std::nullopt;
      }
    }
    return std::nullopt;
  }

  std::optional<Json> parse_array(int depth) {
    ++pos_;
    Json::Array arr;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      return Json(std::move(arr));
    }
    while (true) {
      skip_ws();
      auto v = parse_value(depth + 1);
      if (!v) return std::nullopt;
      arr.push_back(std::move(*v));
      skip_ws();
      if (pos_ >= s_.size()) return std::nullopt;
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        return Json(std::move(arr));
      }
      return std::nullopt;
    }
  }

  std::optional<Json> parse_object(int depth) {
    ++pos_;
    Json::Object obj;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      return Json(std::move(obj));
    }
    while (true) {
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != '"') return std::nullopt;
      auto key = parse_string();
      if (!key) return std::nullopt;
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != ':') return std::nullopt;
      ++pos_;
      skip_ws();
      auto v = parse_value(depth + 1);
      if (!v) return std::nullopt;
      obj.emplace_back(std::move(*key), std::move(*v));
      skip_ws();
      if (pos_ >= s_.size()) return std::nullopt;
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        return Json(std::move(obj));
      }
      return std::nullopt;
    }
  }

  std::string_view s_;
  std::size_t pos_ = 0;
};

void append_number(std::string& out, double v) {
  if (!std::isfinite(v)) {
    out += "null";
    return;
  }
  if (v == std::floor(v) && std::fabs(v) < 9.0e15) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof buf, static_cast<long long>(v));
    out.append(buf, res.ptr);
    return;
  }
  char buf[64];
  const auto res = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, res.ptr);
}

}  // namespace

std::string json_quote(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out += '"';
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  out += '"';
  return out;
}

std::optional<Json> Json::parse(std::string_view text) { return Parser(text).run(); }

Json::Type Json::type() const {
  switch (value_.index()) {
    case 0: return Type::Null;
    case 1: return Type::Bool;
    case 2: return Type::Number;
    case 3: return Type::String;
    case 4: return Type::Array;
    default: return Type::Object;
  }
}

bool Json::as_bool(bool fallback) const {
  if (const auto* v = std::get_if<bool>(&value_)) return *v;
  return fallback;
}

double Json::as_number(double fallback) const {
  if (const auto* v = std::get_if<double>(&value_)) return *v;
  return fallback;
}

int Json::as_int(int fallback) const {
  if (const auto* v = std::get_if<double>(&value_)) {
    if (*v > 2147483647.0) return 2147483647;
    if (*v < -2147483648.0) return -2147483647 - 1;
    return static_cast<int>(std::llround(*v));
  }
  return fallback;
}

std::string Json::as_string(std::string_view fallback) const {
  if (const auto* v = std::get_if<std::string>(&value_)) return *v;
  return std::string(fallback);
}

const std::string& Json::str() const {
  if (const auto* v = std::get_if<std::string>(&value_)) return *v;
  return empty_string();
}

const Json::Array& Json::items() const {
  static const Array empty;
  if (const auto* v = std::get_if<std::shared_ptr<Array>>(&value_)) return **v;
  return empty;
}

const Json::Object& Json::members() const {
  static const Object empty;
  if (const auto* v = std::get_if<std::shared_ptr<Object>>(&value_)) return **v;
  return empty;
}

std::size_t Json::size() const {
  if (is_array()) return items().size();
  if (is_object()) return members().size();
  return 0;
}

const Json& Json::operator[](std::string_view key) const {
  for (const auto& [k, v] : members()) {
    if (k == key) return v;
  }
  return null_json();
}

const Json& Json::operator[](std::size_t index) const {
  const auto& arr = items();
  return index < arr.size() ? arr[index] : null_json();
}

bool Json::contains(std::string_view key) const {
  for (const auto& member : members()) {
    if (member.first == key) return true;
  }
  return false;
}

Json::Array& Json::mutable_array() {
  if (!is_array()) value_ = std::make_shared<Array>();
  auto& ptr = std::get<std::shared_ptr<Array>>(value_);
  if (ptr.use_count() > 1) ptr = std::make_shared<Array>(*ptr);
  return *ptr;
}

Json::Object& Json::mutable_object() {
  if (!is_object()) value_ = std::make_shared<Object>();
  auto& ptr = std::get<std::shared_ptr<Object>>(value_);
  if (ptr.use_count() > 1) ptr = std::make_shared<Object>(*ptr);
  return *ptr;
}

void Json::push_back(Json v) { mutable_array().push_back(std::move(v)); }

void Json::set(std::string_view key, Json v) {
  auto& obj = mutable_object();
  for (auto& member : obj) {
    if (member.first == key) {
      member.second = std::move(v);
      return;
    }
  }
  obj.emplace_back(std::string(key), std::move(v));
}

std::string Json::dump(int indent) const {
  std::string out;
  dump_to(out, indent, 0);
  return out;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
  const auto newline = [&](int d) {
    if (indent < 0) return;
    out += '\n';
    out.append(static_cast<std::size_t>(indent * d), ' ');
  };
  switch (type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += as_bool() ? "true" : "false"; break;
    case Type::Number: append_number(out, as_number()); break;
    case Type::String: out += json_quote(str()); break;
    case Type::Array: {
      const auto& arr = items();
      out += '[';
      for (std::size_t i = 0; i < arr.size(); ++i) {
        if (i) out += ',';
        newline(depth + 1);
        arr[i].dump_to(out, indent, depth + 1);
      }
      if (!arr.empty()) newline(depth);
      out += ']';
      break;
    }
    case Type::Object: {
      const auto& obj = members();
      out += '{';
      for (std::size_t i = 0; i < obj.size(); ++i) {
        if (i) out += ',';
        newline(depth + 1);
        out += json_quote(obj[i].first);
        out += indent < 0 ? ":" : ": ";
        obj[i].second.dump_to(out, indent, depth + 1);
      }
      if (!obj.empty()) newline(depth);
      out += '}';
      break;
    }
  }
}

bool Json::operator==(const Json& other) const {
  if (type() != other.type()) return false;
  switch (type()) {
    case Type::Null: return true;
    case Type::Bool: return as_bool() == other.as_bool();
    case Type::Number: return as_number() == other.as_number();
    case Type::String: return str() == other.str();
    case Type::Array: return items() == other.items();
    case Type::Object: return members() == other.members();
  }
  return false;
}

}  // namespace cvd
