// Convertible core - data family: Amazon Ion 1.0.
//
// Reader: text and binary (detected by the binary version marker E0 01 00 EA).
//   null (all typed nulls) -> null; bool; int (any size, hex, binary,
//   underscores) -> integer or $bigint; decimal -> number (exact double) or
//   $decimal; float -> double; timestamp (all precisions) -> $date; string;
//   symbol -> string (unknown binary symbol IDs as "$<id>"); blob and clob ->
//   $binary; list and sexp -> array; struct -> object. Annotations are dropped
//   with a warning. Binary local symbol tables (also appended ones) are
//   honoured; shared imports cannot be resolved offline and yield "$<id>".
//   A stream of several top-level values is read as documents.
// Writer: Ion text (pretty or compact), floats as 1.5e0, $decimal as Ion
// decimals, $date as timestamps, $binary as {{blobs}}.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/base64.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace {

constexpr std::string_view system_symbols[] = {"",        "$ion",    "$ion_1_0", "$ion_symbol_table", "name",
                                               "version", "imports", "symbols",  "max_id",
                                               "$ion_shared_symbol_table"};

/// Timestamp text (Ion's partial precisions) -> $date.
std::optional<Json> ion_timestamp(std::string_view text, std::size_t& widened) {
  std::string iso(text);
  if (iso.ends_with('T') || iso.ends_with('t')) iso.pop_back();
  if (iso.size() == 4) {
    iso += "-01-01";
    ++widened;
  } else if (iso.size() == 7) {
    iso += "-01";
    ++widened;
  }
  bool unknown_offset = false;
  if (iso.ends_with("-00:00") && iso.size() > 10) {
    iso.resize(iso.size() - 6);
    unknown_offset = true;
  }
  auto parsed = parse_datetime(iso);
  if (!parsed) return std::nullopt;
  if (unknown_offset && parsed->kind == DateTime::Kind::Local) {
    parsed->kind = DateTime::Kind::Offset;  // UTC with an unknown local offset
    parsed->zulu = true;
  }
  return make_date(format_datetime(*parsed));
}

// =========================================================================== text reader

class IonTextParser {
 public:
  IonTextParser(std::string_view text, const ReadContext& ctx) : t_(text), ctx_(ctx), policy_(read_policy(ctx.options)) {}

  Result<std::vector<Json>> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    std::vector<Json> documents;
    while (true) {
      CVT_TRY(skip());
      if (at_end()) break;
      bool system = false;
      CVT_TRY_ASSIGN(Json value, value_with_annotations(0, false, &system));
      if (!system) documents.push_back(std::move(value));
    }
    if (annotations_ > 0) ctx_.warn(std::format("{} Ion annotation(s) were dropped.", annotations_));
    if (widened_ > 0) ctx_.warn(std::format("{} year- or month-precision timestamp(s) were widened to dates.", widened_));
    return documents;
  }

 private:
  [[nodiscard]] bool at_end() const noexcept { return p_ >= t_.size(); }
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept { return p_ + ahead < t_.size() ? t_[p_ + ahead] : '\0'; }
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("Ion", t_, p_, message); }

  Result<void> skip() {
    while (!at_end()) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
        ++p_;
      } else if (c == '/' && peek(1) == '/') {
        while (!at_end() && peek() != '\n') ++p_;
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

  [[nodiscard]] static bool identifier_start(char c) noexcept { return util::is_ascii_alpha(c) || c == '_' || c == '$'; }
  [[nodiscard]] static bool identifier_char(char c) noexcept { return identifier_start(c) || util::is_ascii_digit(c); }
  [[nodiscard]] static bool operator_char(char c) noexcept {
    return std::string_view("!#%&*+-./;<=>?@^`|~").find(c) != std::string_view::npos;
  }
  [[nodiscard]] bool stop(std::size_t at) const noexcept {
    if (at >= t_.size()) return true;
    const char c = t_[at];
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ']' || c == '}' || c == ')' ||
           c == '"' || c == '\'' || c == '{' || c == '[' || c == '(' || (c == '/' && (at + 1 < t_.size() && (t_[at + 1] == '/' || t_[at + 1] == '*')));
  }

  Result<std::uint32_t> hex(std::size_t digits) {
    if (p_ + digits > t_.size()) return error("incomplete escape");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < digits; ++i) {
      const char c = t_[p_ + i];
      const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (d < 0) return error("invalid hexadecimal escape");
      value = value * 16 + static_cast<std::uint32_t>(d);
    }
    p_ += digits;
    return value;
  }

  /// Body of a quoted string/symbol after the opening quote(s).
  Result<void> quoted(std::string& out, char quote, bool long_string) {
    while (true) {
      if (at_end()) return error("unterminated string");
      const char c = peek();
      if (long_string && t_.substr(p_, 3) == "'''") {
        p_ += 3;
        return {};
      }
      if (!long_string && c == quote) {
        ++p_;
        return {};
      }
      if (!long_string && (c == '\n' || c == '\r')) return error("line break in a quoted string");
      if (c == '\\') {
        ++p_;
        const char e = peek();
        ++p_;
        switch (e) {
          case 'a': out.push_back('\a'); break;
          case 'b': out.push_back('\b'); break;
          case 't': out.push_back('\t'); break;
          case 'n': out.push_back('\n'); break;
          case 'f': out.push_back('\f'); break;
          case 'r': out.push_back('\r'); break;
          case 'v': out.push_back('\v'); break;
          case '?': out.push_back('?'); break;
          case '0': out.push_back('\0'); break;
          case '\'': out.push_back('\''); break;
          case '"': out.push_back('"'); break;
          case '/': out.push_back('/'); break;
          case '\\': out.push_back('\\'); break;
          case '\n': break;
          case '\r':
            if (peek() == '\n') ++p_;
            break;
          case 'x':
          case 'u':
          case 'U': {
            CVT_TRY_ASSIGN(std::uint32_t cp, hex(e == 'x' ? 2 : e == 'u' ? 4 : 8));
            if (e == 'u' && cp >= 0xD800 && cp <= 0xDBFF && t_.substr(p_, 2) == "\\u") {
              p_ += 2;
              CVT_TRY_ASSIGN(const std::uint32_t low, hex(4));
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            if (!util::is_scalar_value(static_cast<char32_t>(cp))) return error("escape is not a Unicode scalar value");
            util::append_utf8(out, static_cast<char32_t>(cp));
            break;
          }
          default: return error("invalid escape sequence");
        }
        continue;
      }
      out.push_back(c);
      ++p_;
    }
  }

  /// One or more adjacent '''long''' strings.
  Result<std::string> long_strings() {
    std::string out;
    while (t_.substr(p_, 3) == "'''") {
      p_ += 3;
      CVT_TRY(quoted(out, '\'', true));
      const std::size_t save = p_;
      CVT_TRY(skip());
      if (t_.substr(p_, 3) != "'''") {
        p_ = save;
        break;
      }
    }
    return out;
  }

  Result<std::string> symbol_text() {
    if (peek() == '\'') {
      ++p_;
      std::string out;
      CVT_TRY(quoted(out, '\'', false));
      return out;
    }
    const std::size_t start = p_;
    while (identifier_char(peek())) ++p_;
    return std::string(t_.substr(start, p_ - start));
  }

  Result<Json> value_with_annotations(std::size_t depth, bool in_sexp, bool* system = nullptr) {
    // Annotations: symbol '::'.
    while (true) {
      const std::size_t save = p_;
      if (identifier_start(peek()) || (peek() == '\'' && t_.substr(p_, 3) != "'''")) {
        CVT_TRY_ASSIGN(std::string name, symbol_text());
        CVT_TRY(skip());
        if (t_.substr(p_, 2) == "::") {
          p_ += 2;
          CVT_TRY(skip());
          if (system != nullptr && depth == 0 && name == "$ion_symbol_table") *system = true;
          ++annotations_;
          continue;
        }
      }
      p_ = save;
      break;
    }
    if (system != nullptr && *system) {
      --annotations_;
      return value(depth, in_sexp);
    }
    CVT_TRY_ASSIGN(Json v, value(depth, in_sexp));
    if (system != nullptr && depth == 0 && v.is_string() && v.as_string() == "$ion_1_0" && last_was_symbol_) *system = true;
    return v;
  }

  Result<Json> value(std::size_t depth, bool in_sexp) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the Ion data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    last_was_symbol_ = false;
    const char c = peek();
    if (at_end()) return error("unexpected end of data; expected a value");
    if (c == '{' && peek(1) == '{') return lob();
    if (c == '{') return structure(depth);
    if (c == '[') return sequence(depth, ']', false);
    if (c == '(') return sequence(depth, ')', true);
    if (c == '"') {
      ++p_;
      std::string out;
      CVT_TRY(quoted(out, '"', false));
      return Json(std::move(out));
    }
    if (t_.substr(p_, 3) == "'''") {
      CVT_TRY_ASSIGN(std::string text, long_strings());
      return Json(std::move(text));
    }
    if (c == '\'') {
      CVT_TRY_ASSIGN(std::string text, symbol_text());
      last_was_symbol_ = true;
      return Json(std::move(text));
    }
    if (util::is_ascii_digit(c) || ((c == '-' || c == '+') && (util::is_ascii_digit(peek(1)) || t_.substr(p_ + 1, 3) == "inf"))) {
      return number_or_timestamp();
    }
    if (identifier_start(c)) {
      CVT_TRY_ASSIGN(std::string word, symbol_text());
      if (word == "true") return Json(true);
      if (word == "false") return Json(false);
      if (word == "nan") return Json(std::numeric_limits<double>::quiet_NaN());
      if (word == "null") {
        if (peek() == '.') {
          ++p_;
          while (util::is_ascii_alpha(peek())) ++p_;
        }
        return Json();
      }
      last_was_symbol_ = true;
      if (word.size() > 1 && word[0] == '$' && std::ranges::all_of(word.substr(1), [](char d) { return util::is_ascii_digit(d); })) {
        const auto sid = util::parse_int(std::string_view(word).substr(1));
        if (sid && *sid > 0 && *sid < 10) return Json(std::string(system_symbols[*sid]));
      }
      return Json(std::move(word));
    }
    if (in_sexp && operator_char(c)) {
      const std::size_t start = p_;
      while (operator_char(peek())) ++p_;
      return Json(std::string(t_.substr(start, p_ - start)));
    }
    return error(std::format("unexpected character '{}'", c));
  }

  Result<Json> number_or_timestamp() {
    const std::size_t start = p_;
    // +inf / -inf
    if ((peek() == '+' || peek() == '-') && t_.substr(p_ + 1, 3) == "inf" && stop(p_ + 4)) {
      const bool negative = peek() == '-';
      p_ += 4;
      return Json(negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
    }
    // Timestamps start with four digits followed by '-' or 'T'.
    if (util::is_ascii_digit(peek()) && util::is_ascii_digit(peek(1)) && util::is_ascii_digit(peek(2)) &&
        util::is_ascii_digit(peek(3)) && (peek(4) == '-' || peek(4) == 'T')) {
      while (!stop(p_)) ++p_;
      auto stamp = ion_timestamp(t_.substr(start, p_ - start), widened_);
      if (!stamp) {
        p_ = start;
        return error("invalid timestamp");
      }
      return std::move(*stamp);
    }
    while (!stop(p_)) ++p_;
    const std::string_view raw = t_.substr(start, p_ - start);
    std::string text;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '_') {
        if (i == 0 || i + 1 >= raw.size()) {
          p_ = start;
          return error("misplaced '_' in a number");
        }
        continue;
      }
      text.push_back(raw[i]);
    }
    std::string_view body = text;
    bool negative = false;
    if (body.starts_with('-')) {
      negative = true;
      body.remove_prefix(1);
    }
    if (body.starts_with("0x") || body.starts_with("0X") || body.starts_with("0b") || body.starts_with("0B")) {
      const int base = (body[1] == 'x' || body[1] == 'X') ? 16 : 2;
      auto value = integer_from_digits(body.substr(2), base, negative);
      if (!value) {
        p_ = start;
        return error("invalid integer");
      }
      return std::move(*value);
    }
    const bool is_float = body.find_first_of("eE") != std::string_view::npos;
    std::string json = text;
    for (char& ch : json) {
      if (ch == 'd' || ch == 'D') ch = 'e';
    }
    if (json.ends_with('.')) json += '0';
    if (const std::size_t dot = json.find('.'); dot != std::string::npos && dot + 1 < json.size() &&
                                                 (json[dot + 1] == 'e' || json[dot + 1] == 'E')) {
      json.insert(dot + 1, "0");
    }
    if (!decimal_parts(json) || (body.size() > 1 && body.front() == '0' && util::is_ascii_digit(body[1]))) {
      p_ = start;
      return error("invalid number");
    }
    if (is_float) return Json(util::parse_double(json).value_or(0.0));
    return number_from_literal(json);
  }

  Result<Json> lob() {
    p_ += 2;
    CVT_TRY(skip());
    if (peek() == '"' || t_.substr(p_, 3) == "'''") {
      std::string text;
      if (peek() == '"') {
        ++p_;
        CVT_TRY(quoted(text, '"', false));
      } else {
        CVT_TRY_ASSIGN(text, long_strings());
      }
      CVT_TRY(skip());
      if (t_.substr(p_, 2) != "}}") return error("expected '}}' after a clob");
      p_ += 2;
      // Clob characters are bytes 0-255.
      std::u32string cps = util::utf8_to_utf32(text);
      Bytes bytes;
      for (const char32_t cp : cps) bytes.push_back(static_cast<std::byte>(cp & 0xFF));
      return make_binary(bytes);
    }
    const std::size_t end = t_.find("}}", p_);
    if (end == std::string_view::npos) return error("unterminated blob");
    auto bytes = util::base64_decode(t_.substr(p_, end - p_));
    if (!bytes) return error("invalid Base64 in a blob");
    p_ = end + 2;
    return make_binary(*bytes);
  }

  Result<Json> sequence(std::size_t depth, char close, bool sexp) {
    ++p_;
    Json array = Json::array();
    while (true) {
      CVT_TRY(skip());
      if (at_end()) return error("unterminated list");
      if (peek() == close) {
        ++p_;
        return array;
      }
      CVT_TRY_ASSIGN(Json element, value_with_annotations(depth + 1, sexp));
      array.push_back(std::move(element));
      CVT_TRY(skip());
      if (!sexp) {
        if (peek() == ',') {
          ++p_;
          continue;
        }
        if (peek() != close) return error("expected ',' or ']' in a list");
      }
    }
  }

  Result<Json> structure(std::size_t depth) {
    ++p_;
    Json object = Json::object();
    while (true) {
      CVT_TRY(skip());
      if (at_end()) return error("unterminated struct");
      if (peek() == '}') {
        ++p_;
        return object;
      }
      std::string name;
      if (peek() == '"') {
        ++p_;
        CVT_TRY(quoted(name, '"', false));
      } else if (t_.substr(p_, 3) == "'''") {
        CVT_TRY_ASSIGN(name, long_strings());
      } else if (peek() == '\'' || identifier_start(peek())) {
        CVT_TRY_ASSIGN(name, symbol_text());
      } else {
        return error("expected a field name");
      }
      CVT_TRY(skip());
      if (peek() != ':') return error("expected ':' after a field name");
      ++p_;
      CVT_TRY(skip());
      CVT_TRY_ASSIGN(Json v, value_with_annotations(depth + 1, false));
      CVT_TRY(put_member(object, std::move(name), std::move(v), policy_));
      CVT_TRY(skip());
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() != '}') return error("expected ',' or '}' in a struct");
    }
  }

  std::string_view t_;
  const ReadContext& ctx_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
  std::size_t annotations_ = 0;
  std::size_t widened_ = 0;
  bool last_was_symbol_ = false;
};

// =========================================================================== binary reader

class IonBinaryReader {
 public:
  IonBinaryReader(ByteView data, const ReadContext& ctx)
      : data_(data), ctx_(ctx), policy_(read_policy(ctx.options)), budget_(data.size()) {
    reset_symbols();
  }

  Result<std::vector<Json>> parse() {
    std::vector<Json> documents;
    std::size_t p = 0;
    while (p < data_.size()) {
      if (p + 4 <= data_.size() && as_chars(data_.subspan(p, 4)) == std::string_view("\xE0\x01\x00\xEA", 4)) {
        reset_symbols();
        p += 4;
        continue;
      }
      bool keep = true;
      CVT_TRY_ASSIGN(Json value, read_value(p, data_.size(), 0, true, keep));
      if (keep) documents.push_back(std::move(value));
    }
    if (annotations_ > 0) ctx_.warn(std::format("{} Ion annotation(s) were dropped.", annotations_));
    return documents;
  }

 private:
  void reset_symbols() {
    symbols_.assign(std::begin(system_symbols), std::end(system_symbols));
  }

  std::unexpected<Error> bad(std::size_t at, std::string_view what) const {
    return fail(Status::Decode, "invalid Ion binary at byte {}: {}", at, what);
  }

  Result<std::uint64_t> var_uint(std::size_t& p, std::size_t end) const {
    std::uint64_t value = 0;
    for (int i = 0; i < 10; ++i) {
      if (p >= end) return bad(p, "truncated VarUInt");
      const auto b = std::to_integer<std::uint8_t>(data_[p++]);
      value = (value << 7) | (b & 0x7F);
      if ((b & 0x80) != 0) return value;
    }
    return bad(p, "VarUInt too long");
  }

  Result<std::int64_t> var_int(std::size_t& p, std::size_t end, bool* negative_zero = nullptr) const {
    if (p >= end) return bad(p, "truncated VarInt");
    auto b = std::to_integer<std::uint8_t>(data_[p++]);
    const bool negative = (b & 0x40) != 0;
    std::uint64_t value = b & 0x3F;
    int count = 0;
    while ((b & 0x80) == 0) {
      if (p >= end || ++count > 9) return bad(p, "truncated VarInt");
      b = std::to_integer<std::uint8_t>(data_[p++]);
      value = (value << 7) | (b & 0x7F);
    }
    if (negative_zero != nullptr) *negative_zero = negative && value == 0;
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return bad(p, "VarInt too large");
    return negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
  }

  [[nodiscard]] std::string symbol(std::uint64_t sid) const {
    if (sid < symbols_.size() && !symbols_[sid].empty()) return symbols_[sid];
    return std::format("${}", sid);
  }

  /// Sign-magnitude integer of `size` bytes -> decimal digits with sign.
  [[nodiscard]] std::string signed_magnitude(std::size_t p, std::size_t size) const {
    if (size == 0) return "0";
    Bytes magnitude(data_.begin() + static_cast<std::ptrdiff_t>(p), data_.begin() + static_cast<std::ptrdiff_t>(p + size));
    const bool negative = (std::to_integer<unsigned>(magnitude[0]) & 0x80) != 0;
    magnitude[0] = static_cast<std::byte>(std::to_integer<unsigned>(magnitude[0]) & 0x7F);
    return magnitude_to_decimal(magnitude, negative);
  }

  Result<Json> timestamp(std::size_t p, std::size_t end) const {
    bool unknown = false;
    CVT_TRY_ASSIGN(const std::int64_t offset, var_int(p, end, &unknown));
    CVT_TRY_ASSIGN(const std::uint64_t year, var_uint(p, end));
    std::uint64_t parts[5] = {1, 1, 0, 0, 0};
    int count = 0;
    while (p < end && count < 5) {
      CVT_TRY_ASSIGN(parts[count], var_uint(p, end));
      ++count;
    }
    std::string fraction;
    if (p < end) {
      CVT_TRY_ASSIGN(const std::int64_t exponent, var_int(p, end));
      std::string digits = signed_magnitude(p, end - p);
      if (digits.starts_with('-') || exponent > 0 || exponent < -64) return bad(p, "invalid timestamp fraction");
      const auto places = static_cast<std::size_t>(-exponent);
      if (digits.size() < places) digits.insert(0, places - digits.size(), '0');
      if (digits.size() > places && digits.find_first_not_of('0') < digits.size() - places) {
        return bad(p, "timestamp fraction is not below one second");
      }
      fraction = digits.substr(digits.size() - places);
    }
    if (year < 1 || year > 9999 || parts[0] < 1 || parts[0] > 12 || parts[1] < 1 || parts[1] > 31 || parts[2] > 23 ||
        parts[3] > 59 || parts[4] > 60) {
      return bad(p, "timestamp out of range");
    }
    DateTime utc;
    utc.year = static_cast<std::int64_t>(year);
    utc.month = static_cast<int>(parts[0]);
    utc.day = static_cast<int>(parts[1]);
    utc.hour = static_cast<int>(parts[2]);
    utc.minute = static_cast<int>(parts[3]);
    utc.second = static_cast<int>(parts[4]);
    if (count <= 2) {
      utc.kind = DateTime::Kind::Date;
      return make_date(format_datetime(utc));
    }
    // Binary timestamps are UTC; express them in their local offset.
    const std::int64_t seconds = to_unix_seconds(utc) + (unknown ? 0 : offset * 60);
    DateTime local = from_unix(seconds, 0);
    local.fraction = fraction;
    local.zulu = unknown || offset == 0;
    local.offset_minutes = static_cast<int>(unknown ? 0 : offset);
    return make_date(format_datetime(local));
  }

  Result<Json> read_value(std::size_t& p, std::size_t end, std::size_t depth, bool top, bool& keep) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the Ion data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    CVT_TRY(budget_.take());
    while (true) {
      if (p >= end) return bad(p, "truncated value");
      const std::size_t at = p;
      const auto descriptor = std::to_integer<std::uint8_t>(data_[p++]);
      const std::uint8_t type = descriptor >> 4;
      std::uint64_t length = descriptor & 0x0F;
      if (length == 15) {
        if (type > 13) return bad(at, "invalid null descriptor");
        return Json();
      }
      if (type == 1) return Json(length == 1);
      if (length == 14 || (type == 13 && length == 1)) {
        CVT_TRY_ASSIGN(length, var_uint(p, end));
      }
      if (length > end - p) return bad(at, "value overruns its container");
      const std::size_t value_end = p + static_cast<std::size_t>(length);
      const std::size_t start = p;
      p = value_end;
      switch (type) {
        case 0: continue;  // NOP padding
        case 2:
        case 3: {
          if (length > 4096) return bad(at, "integer too long");
          std::string digits = magnitude_to_decimal(data_.subspan(start, static_cast<std::size_t>(length)), type == 3);
          if (type == 3 && digits == "-0") return bad(at, "negative zero integer");
          return number_from_literal(digits);
        }
        case 4: {
          util::ByteReader r(data_.subspan(start, static_cast<std::size_t>(length)));
          if (length == 0) return Json(0.0);
          if (length == 4) return Json(static_cast<double>(r.f32be()));
          if (length == 8) return Json(r.f64be());
          return bad(at, "float of unsupported size");
        }
        case 5: {
          if (length == 0) return Json(0.0);
          std::size_t q = start;
          CVT_TRY_ASSIGN(const std::int64_t exponent, var_int(q, value_end));
          std::string digits = signed_magnitude(q, value_end - q);
          const bool negative = digits.starts_with('-');
          if (negative) digits.erase(0, 1);
          return decimal_value(negative, digits, exponent);
        }
        case 6: return timestamp(start, value_end);
        case 7: {
          std::uint64_t sid = 0;
          for (std::size_t i = start; i < value_end; ++i) {
            if (sid > (std::numeric_limits<std::uint64_t>::max() >> 8)) return bad(at, "symbol ID too large");
            sid = (sid << 8) | std::to_integer<std::uint64_t>(data_[i]);
          }
          if (top && sid == 2) keep = false;  // $ion_1_0
          return Json(symbol(sid));
        }
        case 8: {
          std::size_t repaired = 0;
          return text_value(as_chars(data_.subspan(start, static_cast<std::size_t>(length))), repaired);
        }
        case 9:
        case 10: return make_binary(data_.subspan(start, static_cast<std::size_t>(length)));
        case 11:
        case 12: {
          Json array = Json::array();
          std::size_t q = start;
          while (q < value_end) {
            const auto next = std::to_integer<std::uint8_t>(data_[q]);
            if ((next >> 4) == 0 && (next & 0x0F) != 15) {  // padding inside a list
              ++q;
              std::uint64_t pad = next & 0x0F;
              if (pad == 14) {
                CVT_TRY_ASSIGN(pad, var_uint(q, value_end));
              }
              if (pad > value_end - q) return bad(q, "padding overruns its list");
              q += static_cast<std::size_t>(pad);
              continue;
            }
            bool keep_element = true;
            CVT_TRY_ASSIGN(Json element, read_value(q, value_end, depth + 1, false, keep_element));
            array.push_back(std::move(element));
          }
          return array;
        }
        case 13: {
          Json object = Json::object();
          std::size_t q = start;
          while (q < value_end) {
            CVT_TRY_ASSIGN(const std::uint64_t sid, var_uint(q, value_end));
            if (q >= value_end) return bad(q, "struct field without a value");
            const auto next = std::to_integer<std::uint8_t>(data_[q]);
            if ((next >> 4) == 0 && (next & 0x0F) != 15) {
              ++q;
              std::uint64_t pad = next & 0x0F;
              if (pad == 14) {
                CVT_TRY_ASSIGN(pad, var_uint(q, value_end));
              }
              if (pad > value_end - q) return bad(q, "padding overruns its struct");
              q += static_cast<std::size_t>(pad);
              continue;
            }
            bool keep_field = true;
            CVT_TRY_ASSIGN(Json field, read_value(q, value_end, depth + 1, false, keep_field));
            CVT_TRY(put_member(object, symbol(sid), std::move(field), policy_));
          }
          return object;
        }
        case 14: {
          std::size_t q = start;
          CVT_TRY_ASSIGN(const std::uint64_t annotation_length, var_uint(q, value_end));
          if (annotation_length == 0 || annotation_length > value_end - q) return bad(at, "bad annotation wrapper");
          const std::size_t annotations_end = q + static_cast<std::size_t>(annotation_length);
          bool symbol_table = false;
          while (q < annotations_end) {
            CVT_TRY_ASSIGN(const std::uint64_t sid, var_uint(q, annotations_end));
            if (sid == 3) symbol_table = true;
          }
          bool keep_inner = true;
          CVT_TRY_ASSIGN(Json inner, read_value(q, value_end, depth + 1, false, keep_inner));
          if (q != value_end) return bad(at, "annotation wrapper length mismatch");
          if (top && symbol_table && inner.is_object()) {
            CVT_TRY(load_symbol_table(inner));
            keep = false;
            return inner;
          }
          ++annotations_;
          return inner;
        }
        default: break;
      }
      return bad(at, "reserved type code");
    }
  }

  Result<void> load_symbol_table(const Json& table) {
    const Json& imports = table["imports"];
    const bool append = imports.is_string() && imports.as_string() == "$ion_symbol_table";
    if (!append) reset_symbols();
    if (imports.is_array()) {
      for (const Json& import : imports.elements()) {
        const std::uint64_t max_id = import["max_id"].as_uint64(0);
        if (max_id > 1'000'000 || symbols_.size() + max_id > budget_.limit) {
          return fail(Status::Limit, "the Ion symbol table imports too many symbols");
        }
        for (std::uint64_t i = 0; i < max_id; ++i) symbols_.emplace_back();  // unresolved shared symbols
      }
    }
    for (const Json& symbol_value : table["symbols"].elements()) {
      CVT_TRY(budget_.take());
      symbols_.push_back(symbol_value.is_string() ? symbol_value.as_string() : std::string());
    }
    return {};
  }

  ByteView data_;
  const ReadContext& ctx_;
  ReadPolicy policy_;
  NodeBudget budget_;
  std::vector<std::string> symbols_;
  std::size_t annotations_ = 0;
};

bool is_binary_ion(ByteView data) { return data.size() >= 4 && as_chars(data.first(4)) == std::string_view("\xE0\x01\x00\xEA", 4); }

Result<AnyIR> read_ion(ReadContext& ctx) {
  std::vector<Json> documents;
  if (is_binary_ion(ctx.input)) {
    IonBinaryReader reader(ctx.input, ctx);
    CVT_TRY_ASSIGN(documents, reader.parse());
  } else {
    CVT_TRY_ASSIGN(const std::string text, input_text(ctx, "Ion"));
    IonTextParser parser(text, ctx);
    CVT_TRY_ASSIGN(documents, parser.parse());
  }
  CVT_TRY_ASSIGN(Json value, select_documents(std::move(documents), ctx.options, "Ion"));
  CVT_TRY(finish_read(value, ctx));
  return AnyIR(ir::Data{std::move(value)});
}

// =========================================================================== text writer

bool ion_identifier(std::string_view name) {
  if (name.empty() || name == "true" || name == "false" || name == "null" || name == "nan") return false;
  if (!(util::is_ascii_alpha(name[0]) || name[0] == '_' || name[0] == '$')) return false;
  if (name[0] == '$' && name.size() > 1 && std::ranges::all_of(name.substr(1), [](char c) { return util::is_ascii_digit(c); })) {
    return false;
  }
  return std::ranges::all_of(name, [](char c) { return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '$'; });
}

void ion_string(std::string& out, std::string_view text, char quote) {
  out.push_back(quote);
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == static_cast<unsigned char>(quote) || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c < 0x20 || c == 0x7F) {
      out += std::format("\\x{:02x}", static_cast<unsigned>(c));
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
  out.push_back(quote);
}

class IonWriter {
 public:
  IonWriter(int indent, bool sort) : indent_(indent), sort_(sort) {}

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
      out_ += "nan";
    } else if (std::isinf(d)) {
      out_ += d < 0 ? "-inf" : "+inf";
    } else {
      std::string text = format_double(d);
      if (text.find('e') == std::string::npos) text += "e0";
      out_ += text;
    }
  }

  void write(const Json& value, std::size_t depth) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext:
        out_ += "{{" + value[ir::data_tags::binary].as_string() + "}}";
        return;
      case TagKind::BigInt: out_ += tag_text(value); return;
      case TagKind::Decimal: {
        std::string text(tag_text(value));
        for (char& c : text) {
          if (c == 'e' || c == 'E') c = 'd';
        }
        if (text.find_first_of(".d") == std::string::npos) text += ".";
        out_ += text;
        return;
      }
      case TagKind::Float: number(plain_value(value).as_double()); return;
      case TagKind::Date: {
        const auto parsed = parse_datetime(tag_text(value));
        if (!parsed || parsed->kind == DateTime::Kind::Time) {
          ion_string(out_, tag_text(value), '"');
        } else if (parsed->kind == DateTime::Kind::Local) {
          out_ += format_datetime(*parsed) + "-00:00";
        } else {
          out_ += format_datetime(*parsed);
        }
        return;
      }
      case TagKind::Symbol: ion_string(out_, tag_text(value), '\''); return;
      case TagKind::Undefined: out_ += "null"; return;
      default: break;
    }
    switch (value.type()) {
      case Json::Type::Null: out_ += "null"; return;
      case Json::Type::Bool: out_ += value.as_bool() ? "true" : "false"; return;
      case Json::Type::Int64:
      case Json::Type::UInt64: out_ += format_integer(value); return;
      case Json::Type::Double: number(value.as_double()); return;
      case Json::Type::String: ion_string(out_, value.as_string(), '"'); return;
      case Json::Type::Array: {
        if (value.empty()) {
          out_ += "[]";
          return;
        }
        out_.push_back('[');
        bool first = true;
        for (const Json& element : value.elements()) {
          if (!first) out_.push_back(',');
          first = false;
          newline(depth + 1);
          write(element, depth + 1);
        }
        newline(depth);
        out_.push_back(']');
        return;
      }
      case Json::Type::Object: {
        if (value.empty()) {
          out_ += "{}";
          return;
        }
        out_.push_back('{');
        bool first = true;
        for (const Json::Member* member : members_of(value, sort_)) {
          if (!first) out_.push_back(',');
          first = false;
          newline(depth + 1);
          if (ion_identifier(member->key())) {
            out_ += member->key();
          } else {
            ion_string(out_, member->key(), '"');
          }
          out_ += indent_ < 0 ? ":" : ": ";
          write(member->value(), depth + 1);
        }
        newline(depth);
        out_.push_back('}');
        return;
      }
    }
  }

  int indent_ = 2;
  bool sort_ = false;
  std::string out_;
};

Result<std::string> write_ion(const Json& value, WriteContext& ctx) {
  const bool minify = ctx.options.was_set("minify") ? ctx.options.get_bool("minify", false)
                                                    : same_format(ctx) && !ctx.options.was_set("indent");
  IonWriter writer(minify ? -1 : static_cast<int>(ctx.options.get_int("indent", 2, 0, 16)), ctx.options.get_bool("sort_keys", false));
  return writer.run(value);
}

int sniff_ion(ByteView data, std::string_view filename) {
  if (is_binary_ion(data)) return 95;
  const std::string_view head = text_head(data, 64);
  if (head.starts_with("$ion_1_0")) return 90;
  return has_extension(filename, {"ion"}) ? 75 : 0;
}

}  // namespace

void register_ion(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "ion",
      .name = "Amazon Ion",
      .description = "Amazon's richly typed data format, in text or binary form.",
      .category = Category::Data,
      .extensions = {"ion"},
      .mime = "application/ion",
      .popularity = 10,
      .signatures = {Signature::at(0, "E0 01 00 EA")},
      .sniff = sniff_ion,
  });
  std::vector<OptionSpec> read_options = common_read_options(true, false);
  read_options.push_back(documents_option());
  read_options.push_back(document_index_option());
  add_codec(registry, {.format_id = "ion",
                       .read = read_ion,
                       .write = text_writer(write_ion, "Ion"),
                       .read_options = std::move(read_options),
                       .write_options = {
                           OptionSpec::boolean("minify", "Compact", false, "Write everything on one line."),
                           indent_option(2),
                           sort_keys_option(),
                       }});
}

}  // namespace cvt::data
