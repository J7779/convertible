// Convertible core - data family: Apple property lists.
//
//   plist           XML property list (read: XML, binary and OpenStep/ASCII
//                   forms alike; write: XML in Apple's layout)
//   bplist          binary property list "bplist00" (read any form; write binary)
//   openstep-plist  OpenStep / NeXTSTEP / GNUstep ASCII property lists and
//                   .strings files (read)
// Mapping: dict -> object (key order kept), array/set -> array, string,
// integer (64-bit signed and unsigned, 128-bit as $bigint), real, boolean,
// date -> $date (UTC, sub-second precision kept), data -> $binary,
// UID -> $uid. Property lists have no null: writers leave nulls out (option
// `null_values`).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/data.hpp"
#include "util/base64.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::data {

namespace {

/// Seconds between the Unix epoch and the Core Foundation epoch (2001-01-01).
constexpr std::int64_t cf_epoch = 978307200;

std::string date_from_cf(double seconds_since_2001) {
  if (!std::isfinite(seconds_since_2001) || std::abs(seconds_since_2001) > 1e15) {
    return format_datetime(from_unix(cf_epoch, 0));
  }
  const double whole = std::floor(seconds_since_2001);
  // A double of seconds since 2001 resolves about a microsecond today: keep microseconds.
  auto nanos = static_cast<std::int64_t>(std::llround((seconds_since_2001 - whole) * 1e6)) * 1000;
  auto seconds = static_cast<std::int64_t>(whole) + cf_epoch;
  if (nanos >= 1'000'000'000) {
    nanos -= 1'000'000'000;
    ++seconds;
  }
  return format_datetime(from_unix(seconds, static_cast<std::uint32_t>(nanos)));
}

std::optional<double> date_to_cf(std::string_view iso) {
  const auto parsed = parse_datetime(iso);
  if (!parsed || parsed->kind == DateTime::Kind::Time) return std::nullopt;
  return static_cast<double>(to_unix_seconds(*parsed) - cf_epoch) + parsed->nanos() / 1e9;
}

/// "2024-05-01T12:30:00Z": UTC with whole seconds, the only form XML
/// property list readers (Core Foundation, plistlib) accept.
std::string xml_plist_date(std::string_view iso, std::size_t& truncated) {
  const auto parsed = parse_datetime(iso);
  if (!parsed || parsed->kind == DateTime::Kind::Time) return std::string(iso);
  if (parsed->nanos() != 0) ++truncated;
  return format_datetime(from_unix(to_unix_seconds(*parsed), 0));
}

// =========================================================================== XML reader

Result<Json> xml_value(util::XmlNode node, std::size_t depth, const ReadPolicy& policy) {
  if (depth > policy.max_depth) {
    return fail(Status::Limit, "the property list is nested more than {} levels deep (option max_depth)",
                policy.max_depth);
  }
  const std::string_view name = node.name();
  if (name == "dict") {
    Json object = Json::object();
    std::string key;
    bool have_key = false;
    for (const util::XmlNode child : node.elements()) {
      if (!have_key) {
        if (child.name() != "key") return fail(Status::Decode, "invalid property list: <dict> expects <key>, found <{}>", child.name());
        key = child.text();
        have_key = true;
        continue;
      }
      CVT_TRY_ASSIGN(Json value, xml_value(child, depth + 1, policy));
      CVT_TRY(put_member(object, std::move(key), std::move(value), policy));
      have_key = false;
    }
    if (have_key) return fail(Status::Decode, "invalid property list: <key>{}</key> has no value", key);
    // plistlib / NSKeyedArchiver spelling of a UID.
    if (object.size() == 1 && object.contains("CF$UID") && object["CF$UID"].is_integer() && object["CF$UID"].get_uint64()) {
      return make_uid(object["CF$UID"].as_uint64());
    }
    return object;
  }
  if (name == "array") {
    Json array = Json::array();
    for (const util::XmlNode child : node.elements()) {
      CVT_TRY_ASSIGN(Json value, xml_value(child, depth + 1, policy));
      array.push_back(std::move(value));
    }
    return array;
  }
  if (name == "string") return Json(node.text());
  if (name == "true") return Json(true);
  if (name == "false") return Json(false);
  if (name == "integer") {
    std::string text(util::trim(node.text()));
    bool negative = false;
    std::string_view body = text;
    if (!body.empty() && (body.front() == '-' || body.front() == '+')) {
      negative = body.front() == '-';
      body.remove_prefix(1);
    }
    std::optional<Json> value;
    if (body.starts_with("0x") || body.starts_with("0X")) {
      value = integer_from_digits(body.substr(2), 16, negative);
    } else if (!body.empty() && std::ranges::all_of(body, [](char c) { return util::is_ascii_digit(c); })) {
      value = number_from_literal(text);
    }
    if (!value) return fail(Status::Decode, "invalid property list: '{}' is not an integer", text);
    return std::move(*value);
  }
  if (name == "real") {
    const std::string text = util::to_lower(util::trim(node.text()));
    if (text == "nan" || text == "+nan" || text == "-nan") return Json(std::numeric_limits<double>::quiet_NaN());
    if (text == "inf" || text == "+inf" || text == "infinity" || text == "+infinity") {
      return Json(std::numeric_limits<double>::infinity());
    }
    if (text == "-inf" || text == "-infinity") return Json(-std::numeric_limits<double>::infinity());
    const auto value = util::parse_double(text);
    if (!value) return fail(Status::Decode, "invalid property list: '{}' is not a real number", text);
    Json exact = number_from_literal(text);
    if (exact.is_integer()) return Json(*value);  // a <real> stays a double
    return exact;
  }
  if (name == "date") {
    const std::string text(util::trim(node.text()));
    const auto parsed = parse_datetime(text);
    if (!parsed) return fail(Status::Decode, "invalid property list: '{}' is not a date", text);
    return make_date(format_datetime(*parsed));
  }
  if (name == "data") {
    auto bytes = util::base64_decode(node.text());
    if (!bytes) return fail(Status::Decode, "invalid property list: <data> is not valid Base64");
    return make_binary(*bytes);
  }
  return fail(Status::Decode, "invalid property list: unknown element <{}>", name);
}

Result<Json> parse_xml_plist(ByteView input, const ReadContext& ctx) {
  const ReadPolicy policy = read_policy(ctx.options);
  util::XmlParseOptions options;
  options.preserve_whitespace = true;  // <string>  </string> keeps its spaces
  options.max_depth = policy.max_depth + 2;
  auto document = util::parse_xml(input, options);
  if (!document) return fail(std::move(document).error());
  util::XmlNode root = document->root();
  if (root.name() == "plist") {
    util::XmlNode value;
    for (const util::XmlNode child : root.elements()) {
      if (value) return fail(Status::Decode, "invalid property list: <plist> holds more than one value");
      value = child;
    }
    if (!value) return Json();
    return xml_value(value, 0, policy);
  }
  return xml_value(root, 0, policy);
}

// =========================================================================== binary reader

class BinaryPlistReader {
 public:
  BinaryPlistReader(ByteView data, const ReadContext& ctx) : data_(data), ctx_(ctx), policy_(read_policy(ctx.options)) {
    budget_ = std::max<std::uint64_t>(1'000'000, static_cast<std::uint64_t>(data.size()) * 32);
  }

  Result<Json> parse() {
    if (data_.size() < 8 + 32 || as_chars(data_.first(7)) != "bplist0") {
      return fail(Status::Decode, "not a binary property list");
    }
    util::ByteReader trailer(data_.last(32));
    (void)trailer.bytes(6);
    offset_size_ = trailer.u8();
    ref_size_ = trailer.u8();
    const std::uint64_t count = trailer.u64be();
    const std::uint64_t top = trailer.u64be();
    const std::uint64_t table = trailer.u64be();
    const auto valid_size = [](std::uint8_t s) { return s == 1 || s == 2 || s == 4 || s == 8; };
    const std::uint64_t body_end = data_.size() - 32;
    if (!valid_size(offset_size_) || !valid_size(ref_size_) || count == 0 || top >= count || table < 8 ||
        table > body_end || count > (body_end - table) / offset_size_) {
      return fail(Status::Decode, "the binary property list has a damaged trailer");
    }
    count_ = count;
    table_ = table;
    in_progress_.assign(static_cast<std::size_t>(count), false);
    return object(top, 0);
  }

 private:
  [[nodiscard]] std::uint64_t read_be(std::uint64_t at, std::size_t size) const {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) value = (value << 8) | std::to_integer<std::uint64_t>(data_[static_cast<std::size_t>(at) + i]);
    return value;
  }

  Result<std::uint64_t> offset_of(std::uint64_t index) const {
    const std::uint64_t at = table_ + index * offset_size_;
    const std::uint64_t offset = read_be(at, offset_size_);
    if (offset < 8 || offset >= table_) return fail(Status::Decode, "binary property list object {} has a bad offset", index);
    return offset;
  }

  /// Length that follows a marker with low nibble 0xF: an int object.
  Result<std::uint64_t> length(std::uint8_t marker, std::uint64_t& at) const {
    const std::uint64_t low = marker & 0x0Fu;
    if (low != 0x0F) return low;
    if (at >= table_) return fail(Status::Decode, "truncated binary property list");
    const auto int_marker = std::to_integer<std::uint8_t>(data_[static_cast<std::size_t>(at)]);
    if ((int_marker & 0xF0u) != 0x10) return fail(Status::Decode, "binary property list: bad length marker");
    const std::size_t size = std::size_t{1} << (int_marker & 0x0Fu);
    if (size > 8 || at + 1 + size > table_) return fail(Status::Decode, "binary property list: bad length");
    const std::uint64_t value = read_be(at + 1, size);
    at += 1 + size;
    return value;
  }

  Result<void> need(std::uint64_t at, std::uint64_t bytes) const {
    if (bytes > table_ || at > table_ - bytes) return fail(Status::Decode, "truncated binary property list object");
    return {};
  }

  Result<std::uint64_t> ref_at(std::uint64_t at) const {
    const std::uint64_t ref = read_be(at, ref_size_);
    if (ref >= count_) return fail(Status::Decode, "binary property list reference {} is out of range", ref);
    return ref;
  }

  Result<Json> object(std::uint64_t index, std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the property list is nested more than {} levels deep (option max_depth)",
                  policy_.max_depth);
    }
    if (++produced_ > budget_) {
      return fail(Status::Limit, "the binary property list expands to more than {} values", budget_);
    }
    if (in_progress_[static_cast<std::size_t>(index)]) {
      return fail(Status::Decode, "the binary property list contains a cycle (object {} contains itself)", index);
    }
    if ((produced_ & 8191) == 0) CVT_TRY(ctx_.checkpoint(0.5));
    CVT_TRY_ASSIGN(std::uint64_t at, offset_of(index));
    const auto marker = std::to_integer<std::uint8_t>(data_[static_cast<std::size_t>(at)]);
    ++at;
    const std::uint8_t type = marker >> 4;
    const std::uint8_t low = marker & 0x0F;
    switch (type) {
      case 0x0:
        if (marker == 0x08) return Json(false);
        if (marker == 0x09) return Json(true);
        if (marker == 0x00) return Json();
        return fail(Status::Decode, "binary property list: unsupported marker 0x{:02X}", marker);
      case 0x1: {
        const std::size_t size = std::size_t{1} << low;
        if (size > 16) return fail(Status::Decode, "binary property list: integer of {} bytes", size);
        CVT_TRY(need(at, size));
        if (size == 16) {
          const ByteView bytes = data_.subspan(static_cast<std::size_t>(at), 16);
          const std::uint64_t high = read_be(at, 8);
          const std::uint64_t low_word = read_be(at + 8, 8);
          if (high == 0) return Json(low_word);
          if (high == ~std::uint64_t{0} && (low_word >> 63) != 0) return Json(static_cast<std::int64_t>(low_word));
          return make_bigint(twos_complement_to_decimal(bytes));
        }
        const std::uint64_t value = read_be(at, size);
        if (size == 8) return Json(static_cast<std::int64_t>(value));
        return Json(value);
      }
      case 0x2: {
        if (low == 2) {
          CVT_TRY(need(at, 4));
          return Json(static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(read_be(at, 4)))));
        }
        if (low == 3) {
          CVT_TRY(need(at, 8));
          return Json(std::bit_cast<double>(read_be(at, 8)));
        }
        return fail(Status::Decode, "binary property list: real of unsupported size");
      }
      case 0x3: {
        if (marker != 0x33) return fail(Status::Decode, "binary property list: bad date marker");
        CVT_TRY(need(at, 8));
        return make_date(date_from_cf(std::bit_cast<double>(read_be(at, 8))));
      }
      case 0x4: {
        CVT_TRY_ASSIGN(const std::uint64_t size, length(marker, at));
        CVT_TRY(need(at, size));
        return make_binary(data_.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(size)));
      }
      case 0x5: {
        CVT_TRY_ASSIGN(const std::uint64_t size, length(marker, at));
        CVT_TRY(need(at, size));
        const std::string_view raw = as_chars(data_.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(size)));
        if (util::is_valid_utf8(raw)) return Json(std::string(raw));
        return Json(util::decode_text(as_bytes(raw), util::TextEncoding::Latin1).text);
      }
      case 0x6: {
        CVT_TRY_ASSIGN(const std::uint64_t units, length(marker, at));
        if (units > table_) return fail(Status::Decode, "truncated binary property list object");
        CVT_TRY(need(at, units * 2));
        std::u16string text(static_cast<std::size_t>(units), u'\0');
        for (std::uint64_t i = 0; i < units; ++i) text[static_cast<std::size_t>(i)] = static_cast<char16_t>(read_be(at + i * 2, 2));
        return Json(util::utf16_to_utf8(text));
      }
      case 0x8: {
        const std::size_t size = std::size_t{low} + 1;
        if (size > 8) return fail(Status::Decode, "binary property list: UID of {} bytes", size);
        CVT_TRY(need(at, size));
        return make_uid(read_be(at, size));
      }
      case 0xA:
      case 0xB:
      case 0xC: {
        CVT_TRY_ASSIGN(const std::uint64_t size, length(marker, at));
        if (size > table_) return fail(Status::Decode, "truncated binary property list object");
        CVT_TRY(need(at, size * ref_size_));
        in_progress_[static_cast<std::size_t>(index)] = true;
        Json array = Json::array();
        for (std::uint64_t i = 0; i < size; ++i) {
          CVT_TRY_ASSIGN(const std::uint64_t ref, ref_at(at + i * ref_size_));
          CVT_TRY_ASSIGN(Json value, object(ref, depth + 1));
          array.push_back(std::move(value));
        }
        in_progress_[static_cast<std::size_t>(index)] = false;
        return array;
      }
      case 0xD: {
        CVT_TRY_ASSIGN(const std::uint64_t size, length(marker, at));
        if (size > table_) return fail(Status::Decode, "truncated binary property list object");
        CVT_TRY(need(at, size * 2 * ref_size_));
        in_progress_[static_cast<std::size_t>(index)] = true;
        Json object_value = Json::object();
        for (std::uint64_t i = 0; i < size; ++i) {
          CVT_TRY_ASSIGN(const std::uint64_t key_ref, ref_at(at + i * ref_size_));
          CVT_TRY_ASSIGN(const std::uint64_t value_ref, ref_at(at + (size + i) * ref_size_));
          CVT_TRY_ASSIGN(Json key, object(key_ref, depth + 1));
          CVT_TRY_ASSIGN(Json value, object(value_ref, depth + 1));
          std::string key_text = key.is_string() ? key.as_string() : util::dump_json(key);
          CVT_TRY(put_member(object_value, std::move(key_text), std::move(value), policy_));
        }
        in_progress_[static_cast<std::size_t>(index)] = false;
        return object_value;
      }
      default: break;
    }
    return fail(Status::Decode, "binary property list: unsupported marker 0x{:02X}", marker);
  }

  ByteView data_;
  const ReadContext& ctx_;
  ReadPolicy policy_;
  std::uint8_t offset_size_ = 0;
  std::uint8_t ref_size_ = 0;
  std::uint64_t count_ = 0;
  std::uint64_t table_ = 0;
  std::vector<bool> in_progress_;
  std::uint64_t produced_ = 0;
  std::uint64_t budget_ = 0;
};

// =========================================================================== OpenStep reader

class OpenStepParser {
 public:
  OpenStepParser(std::string_view text, const ReadContext& ctx) : t_(text), policy_(read_policy(ctx.options)) {}

  Result<Json> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    CVT_TRY(skip());
    if (at_end()) return Json::object();
    // .strings files: a dictionary without braces.
    const std::size_t save = p_;
    if (peek() != '{' && peek() != '(' && peek() != '<') {
      auto key = parse_string();
      if (key) {
        CVT_TRY(skip());
        if (peek() == '=') {
          p_ = save;
          return parse_dict_body('\0', 0);
        }
      }
      p_ = save;
    }
    CVT_TRY_ASSIGN(Json value, parse_value(0));
    CVT_TRY(skip());
    if (!at_end()) return error("unexpected content after the property list");
    return value;
  }

 private:
  [[nodiscard]] bool at_end() const noexcept { return p_ >= t_.size(); }
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept { return p_ + ahead < t_.size() ? t_[p_ + ahead] : '\0'; }
  std::unexpected<Error> error(std::string_view message) const {
    return syntax_error("OpenStep property list", t_, p_, message);
  }

  Result<void> skip() {
    while (!at_end()) {
      const char c = peek();
      if (util::is_ascii_space(c)) {
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

  [[nodiscard]] static bool unquoted_char(char c) noexcept {
    return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '$' || c == '+' || c == '/' ||
           c == ':' || c == '.' || c == '-';
  }

  Result<std::string> parse_string() {
    if (peek() == '"' || peek() == '\'') {
      const char quote = peek();
      const std::size_t start = p_++;
      std::string out;
      while (true) {
        if (at_end()) {
          p_ = start;
          return error("unterminated string");
        }
        const char c = peek();
        if (c == quote) {
          ++p_;
          return out;
        }
        if (c != '\\') {
          out.push_back(c);
          ++p_;
          continue;
        }
        ++p_;
        const char e = peek();
        ++p_;
        switch (e) {
          case 'a': out.push_back('\a'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'v': out.push_back('\v'); break;
          case 'U':
          case 'u': {
            std::uint32_t value = 0;
            std::size_t digits = 0;
            while (digits < 4 && std::string_view("0123456789abcdefABCDEF").find(peek()) != std::string_view::npos) {
              value = value * 16 + static_cast<std::uint32_t>(util::parse_int(t_.substr(p_, 1), 16).value_or(0));
              ++p_;
              ++digits;
            }
            if (digits == 0) return error("invalid \\U escape");
            util::append_utf8(out, util::is_scalar_value(value) ? static_cast<char32_t>(value) : util::replacement_character);
            break;
          }
          default:
            if (e >= '0' && e <= '7') {
              std::uint32_t value = static_cast<std::uint32_t>(e - '0');
              for (int k = 0; k < 2 && peek() >= '0' && peek() <= '7'; ++k) value = value * 8 + static_cast<std::uint32_t>(t_[p_++] - '0');
              util::append_utf8(out, static_cast<char32_t>(value & 0xFF));
            } else {
              out.push_back(e);
            }
        }
      }
    }
    const std::size_t start = p_;
    while (unquoted_char(peek())) ++p_;
    if (p_ == start) return error(at_end() ? "unexpected end of the property list" : "expected a string");
    return std::string(t_.substr(start, p_ - start));
  }

  Result<Json> parse_dict_body(char close, std::size_t depth) {
    Json object = Json::object();
    while (true) {
      CVT_TRY(skip());
      if (close != '\0' && peek() == close) {
        ++p_;
        return object;
      }
      if (at_end()) {
        if (close == '\0') return object;
        return error("unterminated dictionary");
      }
      CVT_TRY_ASSIGN(std::string key, parse_string());
      CVT_TRY(skip());
      Json value;
      if (peek() == '=') {
        ++p_;
        CVT_TRY(skip());
        CVT_TRY_ASSIGN(value, parse_value(depth + 1));
        CVT_TRY(skip());
      } else {
        value = Json(key);  // .strings shorthand: "key";
      }
      if (peek() != ';') return error("expected ';' after a dictionary entry");
      ++p_;
      CVT_TRY(put_member(object, std::move(key), std::move(value), policy_));
    }
  }

  Result<Json> parse_value(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the property list is nested more than {} levels deep (option max_depth)",
                  policy_.max_depth);
    }
    const char c = peek();
    if (c == '{') {
      ++p_;
      return parse_dict_body('}', depth);
    }
    if (c == '(') {
      ++p_;
      Json array = Json::array();
      while (true) {
        CVT_TRY(skip());
        if (peek() == ')') {
          ++p_;
          return array;
        }
        CVT_TRY_ASSIGN(Json value, parse_value(depth + 1));
        array.push_back(std::move(value));
        CVT_TRY(skip());
        if (peek() == ',') {
          ++p_;
          continue;
        }
        if (peek() == ')') {
          ++p_;
          return array;
        }
        return error("expected ',' or ')' in an array");
      }
    }
    if (c == '<') {
      ++p_;
      if (peek() == '*') {  // GNUstep typed values
        ++p_;
        const char type = peek();
        ++p_;
        const std::size_t end = t_.find('>', p_);
        if (end == std::string_view::npos) return error("unterminated <* value");
        const std::string_view body = t_.substr(p_, end - p_);
        p_ = end + 1;
        switch (type) {
          case 'I': {
            Json value = number_from_literal(util::trim(body));
            if (!value.is_integer() && tag_kind(value) != TagKind::BigInt) return error("invalid <*I integer");
            return value;
          }
          case 'R': {
            if (!util::parse_double(util::trim(body))) return error("invalid <*R real");
            return number_from_literal(util::trim(body));
          }
          case 'B': return Json(util::trim(body) == "Y");
          case 'D': {
            std::string text(util::trim(body));
            // "2024-05-01 12:30:00 +0000"
            if (text.size() > 20 && text[10] == ' ' && text[19] == ' ') text = text.substr(0, 10) + "T" + text.substr(11, 8) + text.substr(20);
            const auto parsed = parse_datetime(text);
            if (!parsed) return error("invalid <*D date");
            return make_date(format_datetime(*parsed));
          }
          default: return error("unknown <* value type");
        }
      }
      Bytes bytes;
      int nibble = -1;
      while (!at_end() && peek() != '>') {
        const char h = peek();
        ++p_;
        if (util::is_ascii_space(h)) continue;
        const auto digit = util::parse_int(std::string_view(&h, 1), 16);
        if (!digit) return error("invalid hexadecimal digit in <data>");
        if (nibble < 0) {
          nibble = static_cast<int>(*digit);
        } else {
          bytes.push_back(static_cast<std::byte>(nibble * 16 + static_cast<int>(*digit)));
          nibble = -1;
        }
      }
      if (at_end()) return error("unterminated <data>");
      if (nibble >= 0) return error("odd number of hexadecimal digits in <data>");
      ++p_;
      return make_binary(bytes);
    }
    CVT_TRY_ASSIGN(std::string text, parse_string());
    return Json(std::move(text));
  }

  std::string_view t_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
};

// =========================================================================== reading any form

bool is_binary_plist(ByteView data) { return data.size() >= 8 && as_chars(data.first(7)) == "bplist0"; }

Result<Json> parse_any_plist(ByteView input, ReadContext& ctx) {
  if (is_binary_plist(input)) {
    BinaryPlistReader reader(input, ctx);
    return reader.parse();
  }
  const std::string_view head = text_head(input, 256);
  const bool xml = head.starts_with('<') && !head.starts_with("<*") &&
                   (head.starts_with("<?") || head.starts_with("<!") || head.starts_with("<plist") ||
                    head.starts_with("<dict") || head.starts_with("<array"));
  const std::string_view raw = as_chars(input);
  const bool utf16 = raw.starts_with("\xFF\xFE") || raw.starts_with("\xFE\xFF") ||
                     (raw.size() > 2 && (raw[0] == '\0' || raw[1] == '\0'));
  if (xml || (utf16 && ctx.format_id != "openstep-plist" && raw.find(std::string_view("<\0p", 3)) != std::string_view::npos)) {
    return parse_xml_plist(input, ctx);
  }
  CVT_TRY_ASSIGN(const std::string text, input_text(ctx, "property list"));
  if (const std::string_view t = text_head(as_bytes(text), 256); t.starts_with("<?xml") || t.starts_with("<plist")) {
    return parse_xml_plist(as_bytes(text), ctx);
  }
  OpenStepParser parser(text, ctx);
  return parser.parse();
}

Result<AnyIR> read_plist(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Json value, parse_any_plist(ctx.input, ctx));
  CVT_TRY(finish_read(value, ctx));
  return AnyIR(ir::Data{std::move(value)});
}

// =========================================================================== writers

struct PlistWriteState {
  std::string null_mode = "omit";
  bool sort_keys = false;
  std::size_t nulls = 0;
  std::size_t converted = 0;
  std::size_t truncated_dates = 0;

  Result<bool> skip(const Json& value) {
    if (!value.is_null() && tag_kind(value) != TagKind::Undefined) return false;
    if (null_mode == "error") return fail(Status::Encode, "property lists have no null value (option null_values is \"error\")");
    ++nulls;
    return null_mode == "omit";
  }
};

class XmlPlistWriter {
 public:
  explicit XmlPlistWriter(PlistWriteState& state) : state_(state) {}

  Result<std::string> run(const Json& root) {
    out_ = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n";
    CVT_TRY_ASSIGN(const bool skip, state_.skip(root));
    if (!skip) CVT_TRY(write(root, 0));
    out_ += "</plist>\n";
    return std::move(out_);
  }

 private:
  void line(std::size_t depth, std::string_view text) {
    out_.append(depth, '\t');
    out_ += text;
    out_.push_back('\n');
  }
  void element(std::size_t depth, std::string_view name, std::string_view text) {
    out_.append(depth, '\t');
    out_ += std::format("<{}>{}</{}>\n", name, util::xml_escape(text), name);
  }

  Result<void> write(const Json& value, std::size_t depth) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext: {
        const std::string& base64 = value[ir::data_tags::binary].as_string();
        out_.append(depth, '\t');
        out_ += "<data>\n";
        for (std::size_t pos = 0; pos < base64.size(); pos += 68) {
          out_.append(depth, '\t');
          out_ += std::string_view(base64).substr(pos, 68);
          out_.push_back('\n');
        }
        out_.append(depth, '\t');
        out_ += "</data>\n";
        return {};
      }
      case TagKind::Date: {
        const auto parsed = parse_datetime(tag_text(value));
        if (!parsed || parsed->kind == DateTime::Kind::Time) {
          element(depth, "string", tag_text(value));  // a time of day is no plist date
        } else {
          element(depth, "date", xml_plist_date(tag_text(value), state_.truncated_dates));
        }
        return {};
      }
      case TagKind::Uid:
        line(depth, "<dict>");
        element(depth + 1, "key", "CF$UID");
        element(depth + 1, "integer", format_integer(value[ir::data_tags::uid]));
        line(depth, "</dict>");
        return {};
      case TagKind::BigInt:
        ++state_.converted;
        element(depth, "string", tag_text(value));
        return {};
      case TagKind::Decimal: element(depth, "real", tag_text(value)); return {};
      case TagKind::Float: return write(plain_value(value), depth);
      default: break;
    }
    switch (value.type()) {
      case Json::Type::Null: element(depth, "string", ""); return {};
      case Json::Type::Bool: line(depth, value.as_bool() ? "<true/>" : "<false/>"); return {};
      case Json::Type::Int64:
      case Json::Type::UInt64: element(depth, "integer", format_integer(value)); return {};
      case Json::Type::Double: {
        const double d = value.as_double();
        element(depth, "real", std::isnan(d) ? "nan" : std::isinf(d) ? (d < 0 ? "-infinity" : "+infinity") : format_double(d));
        return {};
      }
      case Json::Type::String: element(depth, "string", value.as_string()); return {};
      case Json::Type::Array:
        if (value.empty()) {
          line(depth, "<array/>");
          return {};
        }
        line(depth, "<array>");
        for (const Json& element_value : value.elements()) {
          CVT_TRY_ASSIGN(const bool skip, state_.skip(element_value));
          if (!skip) CVT_TRY(write(element_value, depth + 1));
        }
        line(depth, "</array>");
        return {};
      case Json::Type::Object: {
        if (value.empty()) {
          line(depth, "<dict/>");
          return {};
        }
        line(depth, "<dict>");
        for (const Json::Member* member : members_of(value, state_.sort_keys)) {
          CVT_TRY_ASSIGN(const bool skip, state_.skip(member->value()));
          if (skip) continue;
          element(depth + 1, "key", member->key());
          CVT_TRY(write(member->value(), depth + 1));
        }
        line(depth, "</dict>");
        return {};
      }
    }
    return {};
  }

  PlistWriteState& state_;
  std::string out_;
};

class BinaryPlistWriter {
 public:
  explicit BinaryPlistWriter(PlistWriteState& state) : state_(state) {}

  Result<Bytes> run(const Json& root) {
    CVT_TRY_ASSIGN(const bool skip, state_.skip(root));
    const Json empty_dict = Json::object();
    CVT_TRY_ASSIGN(const std::uint64_t top, collect(skip ? empty_dict : root));
    const std::uint64_t count = objects_.size();
    ref_size_ = count < 256 ? 1 : count < 65536 ? 2 : count < (std::uint64_t{1} << 32) ? 4 : 8;
    util::ByteWriter out;
    out.text("bplist00");
    std::vector<std::uint64_t> offsets;
    offsets.reserve(objects_.size());
    for (const Item& item : objects_) {
      offsets.push_back(out.size());
      CVT_TRY(encode(item, out));
    }
    const std::uint64_t table = out.size();
    const std::uint8_t offset_size = table < 256 ? 1 : table < 65536 ? 2 : table < (std::uint64_t{1} << 32) ? 4 : 8;
    for (const std::uint64_t offset : offsets) put_be(out, offset, offset_size);
    out.zeros(6);
    out.u8(offset_size);
    out.u8(static_cast<std::uint8_t>(ref_size_));
    out.u64be(count);
    out.u64be(top);
    out.u64be(table);
    return out.take();
  }

 private:
  struct Item {
    const Json* value = nullptr;
    std::string string;  // for keys
    bool is_key = false;
    std::vector<std::uint64_t> refs;  // arrays: elements; dicts: keys then values
  };

  static void put_be(util::ByteWriter& out, std::uint64_t value, std::size_t size) {
    for (std::size_t i = size; i-- > 0;) out.u8(static_cast<std::uint8_t>(value >> (8 * i)));
  }

  std::uint64_t string_object(const std::string& text) {
    const auto found = strings_.find(text);
    if (found != strings_.end()) return found->second;
    Item item;
    item.string = text;
    item.is_key = true;
    objects_.push_back(std::move(item));
    strings_.emplace(text, objects_.size() - 1);
    return objects_.size() - 1;
  }

  Result<std::uint64_t> collect(const Json& value) {
    if (value.is_string()) return string_object(value.as_string());
    const std::uint64_t index = objects_.size();
    objects_.push_back(Item{&value, {}, false, {}});
    if (tag_kind(value) != TagKind::None) return index;
    if (value.is_array()) {
      std::vector<std::uint64_t> refs;
      for (const Json& element : value.elements()) {
        CVT_TRY_ASSIGN(const bool skip, state_.skip(element));
        if (skip) continue;
        CVT_TRY_ASSIGN(const std::uint64_t ref, collect(element));
        refs.push_back(ref);
      }
      objects_[index].refs = std::move(refs);
    } else if (value.is_object()) {
      std::vector<std::uint64_t> keys;
      std::vector<std::uint64_t> values;
      for (const Json::Member* member : members_of(value, state_.sort_keys)) {
        CVT_TRY_ASSIGN(const bool skip, state_.skip(member->value()));
        if (skip) continue;
        keys.push_back(string_object(member->key()));
        CVT_TRY_ASSIGN(const std::uint64_t ref, collect(member->value()));
        values.push_back(ref);
      }
      keys.insert(keys.end(), values.begin(), values.end());
      objects_[index].refs = std::move(keys);
    }
    return index;
  }

  static void marker_with_length(util::ByteWriter& out, std::uint8_t type, std::uint64_t length) {
    if (length < 15) {
      out.u8(static_cast<std::uint8_t>((type << 4) | length));
      return;
    }
    out.u8(static_cast<std::uint8_t>((type << 4) | 0x0F));
    integer(out, static_cast<std::int64_t>(length));
  }

  static void integer(util::ByteWriter& out, std::int64_t value) {
    if (value >= 0 && value < 256) {
      out.u8(0x10).u8(static_cast<std::uint8_t>(value));
    } else if (value >= 0 && value < 65536) {
      out.u8(0x11).u16be(static_cast<std::uint16_t>(value));
    } else if (value >= 0 && value <= 0xFFFFFFFFll) {
      out.u8(0x12).u32be(static_cast<std::uint32_t>(value));
    } else {
      out.u8(0x13).i64be(value);
    }
  }

  void string(util::ByteWriter& out, std::string_view text) const {
    const bool ascii = std::ranges::all_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (ascii) {
      marker_with_length(out, 0x5, text.size());
      out.text(text);
      return;
    }
    const std::u16string units = util::utf8_to_utf16(text);
    marker_with_length(out, 0x6, units.size());
    for (const char16_t unit : units) out.u16be(static_cast<std::uint16_t>(unit));
  }

  void refs(util::ByteWriter& out, const std::vector<std::uint64_t>& list) const {
    for (const std::uint64_t ref : list) put_be(out, ref, ref_size_);
  }

  Result<void> encode(const Item& item, util::ByteWriter& out) {
    if (item.is_key) {
      string(out, item.string);
      return {};
    }
    const Json& value = *item.value;
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext: {
        const auto bytes = tagged_bytes(value);
        if (!bytes) return fail(Status::Encode, "invalid Base64 in a binary value");
        marker_with_length(out, 0x4, bytes->size());
        out.bytes(*bytes);
        return {};
      }
      case TagKind::Date: {
        const auto seconds = date_to_cf(tag_text(value));
        if (!seconds) {
          string(out, tag_text(value));
          return {};
        }
        out.u8(0x33).f64be(*seconds);
        return {};
      }
      case TagKind::Uid: {
        const std::uint64_t uid = value[ir::data_tags::uid].as_uint64();
        const std::size_t size = uid < 256 ? 1 : uid < 65536 ? 2 : uid <= 0xFFFFFFFFu ? 4 : 8;
        out.u8(static_cast<std::uint8_t>(0x80 | (size - 1)));
        put_be(out, uid, size);
        return {};
      }
      case TagKind::BigInt: {
        bool negative = false;
        const auto magnitude = decimal_to_magnitude(tag_text(value), &negative);
        if (magnitude && magnitude->size() <= 15) {
          // 128-bit two's complement.
          std::array<std::uint8_t, 16> bytes{};
          for (std::size_t i = 0; i < magnitude->size(); ++i) bytes[16 - magnitude->size() + i] = std::to_integer<std::uint8_t>((*magnitude)[i]);
          if (negative) {
            for (auto& b : bytes) b = static_cast<std::uint8_t>(~b);
            for (std::size_t i = 16; i-- > 0;) {
              if (++bytes[i] != 0) break;
            }
          }
          out.u8(0x14);
          for (const std::uint8_t b : bytes) out.u8(b);
          return {};
        }
        ++state_.converted;
        string(out, tag_text(value));
        return {};
      }
      case TagKind::Decimal:
        out.u8(0x23).f64be(util::parse_double(tag_text(value)).value_or(0.0));
        ++state_.converted;
        return {};
      case TagKind::Float: out.u8(0x23).f64be(plain_value(value).as_double()); return {};
      default: break;
    }
    switch (value.type()) {
      case Json::Type::Null:
      case Json::Type::String: string(out, value.as_string()); return {};
      case Json::Type::Bool: out.u8(value.as_bool() ? 0x09 : 0x08); return {};
      case Json::Type::Int64: integer(out, value.as_int64()); return {};
      case Json::Type::UInt64: {
        const std::uint64_t v = value.as_uint64();
        if (v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          integer(out, static_cast<std::int64_t>(v));
        } else {
          out.u8(0x14).u64be(0).u64be(v);
        }
        return {};
      }
      case Json::Type::Double: out.u8(0x23).f64be(value.as_double()); return {};
      case Json::Type::Array:
        marker_with_length(out, 0xA, item.refs.size());
        refs(out, item.refs);
        return {};
      case Json::Type::Object:
        marker_with_length(out, 0xD, item.refs.size() / 2);
        refs(out, item.refs);
        return {};
    }
    return {};
  }

  PlistWriteState& state_;
  std::vector<Item> objects_;
  std::unordered_map<std::string, std::uint64_t> strings_;
  std::size_t ref_size_ = 1;
};

PlistWriteState state_from(const WriteContext& ctx) {
  PlistWriteState state;
  state.null_mode = ctx.options.get_enum("null_values", "omit", {"omit", "empty_string", "error"});
  state.sort_keys = ctx.options.get_bool("sort_keys", false);
  return state;
}

void report(const PlistWriteState& state, const WriteContext& ctx) {
  if (state.nulls > 0) {
    ctx.warn(std::format("Property lists have no null; {} null value(s) were {}.", state.nulls,
                         state.null_mode == "omit" ? "left out" : "written as empty strings"));
  }
  if (state.truncated_dates > 0) {
    ctx.warn(std::format("{} date(s) were rounded down to whole seconds (XML property lists hold no fractions).",
                         state.truncated_dates));
  }
  if (state.converted > 0) {
    ctx.warn(std::format("{} number(s) beyond the property list's range were written as text or rounded.",
                         state.converted));
  }
}

Result<std::string> write_xml_plist(const Json& value, WriteContext& ctx) {
  PlistWriteState state = state_from(ctx);
  XmlPlistWriter writer(state);
  CVT_TRY_ASSIGN(std::string out, writer.run(value));
  report(state, ctx);
  return out;
}

Result<Bytes> write_binary_plist(const Json& value, WriteContext& ctx) {
  PlistWriteState state = state_from(ctx);
  BinaryPlistWriter writer(state);
  CVT_TRY_ASSIGN(Bytes out, writer.run(value));
  report(state, ctx);
  return out;
}

int sniff_xml_plist(ByteView data, std::string_view filename) {
  const std::string_view head = text_head(data, 1024);
  if (head.starts_with('<') && (head.find("<!DOCTYPE plist") != std::string_view::npos ||
                                head.find("<plist") != std::string_view::npos)) {
    return 92;
  }
  return has_extension(filename, {"plist"}) && head.starts_with("<?xml") ? 80 : 0;
}

int sniff_openstep(ByteView data, std::string_view filename) {
  if (!has_extension(filename, {"plist", "strings"})) return 0;
  const std::string_view head = text_head(data, 256);
  if (head.starts_with('{') || head.starts_with('(') || head.starts_with('"') || head.starts_with("/*") ||
      head.starts_with("//")) {
    return 80;
  }
  return has_extension(filename, {"strings"}) ? 70 : 0;
}

std::vector<OptionSpec> plist_write_options() {
  return {
      sort_keys_option(),
      OptionSpec::choice("null_values", "Null values", "omit", {"omit", "empty_string", "error"},
                         "Property lists have no null: leave such values out, write empty strings, or fail."),
  };
}

}  // namespace

void register_plist(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "plist",
      .name = "Apple property list (XML)",
      .description = "Apple's XML property list format, used for settings and app metadata.",
      .category = Category::Data,
      .extensions = {"plist"},
      .mime = "application/x-plist",
      .popularity = 40,
      .signatures = {Signature::at(0, "'<?xml version=\"1.0\" encoding=\"UTF-8\"?>' 0A '<!DOCTYPE plist'")},
      .sniff = sniff_xml_plist,
  });
  registry.add_format(FormatInfo{
      .id = "bplist",
      .name = "Apple property list (binary)",
      .description = "Apple's compact binary property list format (bplist00).",
      .category = Category::Data,
      .extensions = {"plist", "bplist"},
      .mime = "application/x-bplist",
      .popularity = 35,
      .signatures = {Signature::at(0, "'bplist00'")},
      .sniff = nullptr,
  });
  registry.add_format(FormatInfo{
      .id = "openstep-plist",
      .name = "OpenStep property list",
      .description = "The old-style ASCII property list of NeXTSTEP, OpenStep and GNUstep, and .strings files.",
      .category = Category::Data,
      .extensions = {"plist", "strings"},
      .mime = "text/plain",
      .popularity = 15,
      .signatures = {},
      .sniff = sniff_openstep,
  });
  std::vector<OptionSpec> read_options = common_read_options(true, false);
  add_codec(registry, {.format_id = "plist",
                       .read = read_plist,
                       .write = text_writer(write_xml_plist, "property list"),
                       .read_options = read_options,
                       .write_options = plist_write_options()});
  add_codec(registry, {.format_id = "bplist",
                       .read = read_plist,
                       .write = binary_writer(write_binary_plist, "binary property list"),
                       .read_options = read_options,
                       .write_options = plist_write_options()});
  add_codec(registry, {.format_id = "openstep-plist",
                       .read = read_plist,
                       .write = nullptr,
                       .read_options = read_options});
}

}  // namespace cvt::data
