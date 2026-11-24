// Convertible core - data family: tagged values, numbers, dates, text input,
// read policies and registration helpers shared by every format.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>

#include "engine/registry.hpp"
#include "families/data/data.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace tags = ir::data_tags;

// =========================================================================== tagged values

namespace {

bool is_hex_digit(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool valid_bigint_text(std::string_view text) noexcept {
  if (text.starts_with('-')) text.remove_prefix(1);
  if (text.empty()) return false;
  return std::ranges::all_of(text, [](char c) { return util::is_ascii_digit(c); });
}

/// JSON number grammar: -?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?
bool valid_json_number(std::string_view text) noexcept {
  std::size_t i = 0;
  const auto digits = [&] {
    const std::size_t start = i;
    while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
    return i - start;
  };
  if (i < text.size() && text[i] == '-') ++i;
  if (i >= text.size()) return false;
  if (text[i] == '0') {
    ++i;
  } else if (digits() == 0) {
    return false;
  }
  if (i < text.size() && text[i] == '.') {
    ++i;
    if (digits() == 0) return false;
  }
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    if (digits() == 0) return false;
  }
  return i == text.size();
}

}  // namespace

TagKind tag_kind(const Json& value) noexcept {
  if (!value.is_object()) return TagKind::None;
  const std::size_t size = value.size();
  if (size == 0 || size > 2) return TagKind::None;
  const auto members = value.items();
  if (!members[0].key().starts_with('$')) return TagKind::None;
  if (size == 1) {
    const std::string& key = members[0].key();
    const Json& payload = members[0].value();
    if (key == tags::binary) return payload.is_string() ? TagKind::Binary : TagKind::None;
    if (key == tags::date) return payload.is_string() ? TagKind::Date : TagKind::None;
    if (key == tags::bigint) {
      return payload.is_string() && valid_bigint_text(payload.as_string()) ? TagKind::BigInt : TagKind::None;
    }
    if (key == tags::decimal) {
      return payload.is_string() && valid_json_number(payload.as_string()) ? TagKind::Decimal : TagKind::None;
    }
    if (key == tags::float_value) {
      const std::string& text = payload.as_string();
      return text == "NaN" || text == "Infinity" || text == "-Infinity" ? TagKind::Float : TagKind::None;
    }
    if (key == tags::uid) return payload.get_uint64() && payload.is_integer() ? TagKind::Uid : TagKind::None;
    if (key == tags::undefined) return payload.is_bool() && payload.as_bool() ? TagKind::Undefined : TagKind::None;
    if (key == tags::object_id) {
      const std::string& text = payload.as_string();
      return payload.is_string() && text.size() == 24 && std::ranges::all_of(text, is_hex_digit) ? TagKind::ObjectId
                                                                                                  : TagKind::None;
    }
    if (key == tags::timestamp) {
      return payload.is_object() && payload.size() == 2 && payload["t"].get_uint64() &&
                     payload["i"].get_uint64() && payload["t"].as_uint64() <= 0xFFFFFFFFu &&
                     payload["i"].as_uint64() <= 0xFFFFFFFFu
                 ? TagKind::Timestamp
                 : TagKind::None;
    }
    if (key == tags::min_key) return payload.as_int64(0) == 1 ? TagKind::MinKey : TagKind::None;
    if (key == tags::max_key) return payload.as_int64(0) == 1 ? TagKind::MaxKey : TagKind::None;
    if (key == tags::code) return payload.is_string() ? TagKind::Code : TagKind::None;
    if (key == tags::symbol) return payload.is_string() ? TagKind::Symbol : TagKind::None;
    return TagKind::None;
  }
  const Json* const binary = value.find(tags::binary);
  if (binary != nullptr && binary->is_string()) {
    if (const Json* subtype = value.find(tags::subtype); subtype != nullptr) {
      return subtype->is_integer() && subtype->get_int64() && *subtype->get_int64() >= 0 &&
                     *subtype->get_int64() <= 255
                 ? TagKind::Binary
                 : TagKind::None;
    }
    if (const Json* ext = value.find(tags::ext); ext != nullptr) {
      return ext->is_integer() && ext->get_int64() && *ext->get_int64() >= -128 && *ext->get_int64() <= 127
                 ? TagKind::Ext
                 : TagKind::None;
    }
    return TagKind::None;
  }
  if (const Json* tag = value.find(tags::tag); tag != nullptr && value.contains(tags::tag_value)) {
    return tag->is_integer() && tag->get_uint64() ? TagKind::CborTag : TagKind::None;
  }
  if (const Json* regex = value.find(tags::regex); regex != nullptr && regex->is_string()) {
    const Json* options = value.find(tags::regex_options);
    return options != nullptr && options->is_string() ? TagKind::Regex : TagKind::None;
  }
  return TagKind::None;
}

Json make_binary(ByteView bytes) { return Json::object({{tags::binary, util::base64_encode(bytes)}}); }
Json make_binary_subtype(ByteView bytes, std::uint8_t subtype) {
  return Json::object({{tags::binary, util::base64_encode(bytes)}, {tags::subtype, static_cast<int>(subtype)}});
}
Json make_ext(std::int64_t type, ByteView bytes) {
  return Json::object({{tags::ext, type}, {tags::binary, util::base64_encode(bytes)}});
}
Json make_date(std::string iso8601) { return Json::object({{tags::date, std::move(iso8601)}}); }
Json make_bigint(std::string digits) { return Json::object({{tags::bigint, std::move(digits)}}); }
Json make_decimal(std::string text) { return Json::object({{tags::decimal, std::move(text)}}); }
Json make_uid(std::uint64_t uid) { return Json::object({{tags::uid, uid}}); }
Json make_cbor_tag(std::uint64_t tag, Json value) {
  return Json::object({{tags::tag, tag}, {tags::tag_value, std::move(value)}});
}
Json make_undefined() { return Json::object({{tags::undefined, true}}); }
Json make_object_id(ByteView twelve_bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  for (const std::byte b : twelve_bytes) {
    hex.push_back(digits[std::to_integer<unsigned>(b) >> 4]);
    hex.push_back(digits[std::to_integer<unsigned>(b) & 15]);
  }
  return Json::object({{tags::object_id, std::move(hex)}});
}
Json make_regex(std::string pattern, std::string options) {
  return Json::object({{tags::regex, std::move(pattern)}, {tags::regex_options, std::move(options)}});
}
Json make_symbol(std::string name) { return Json::object({{tags::symbol, std::move(name)}}); }
Json make_code(std::string code) { return Json::object({{tags::code, std::move(code)}}); }

std::optional<Bytes> tagged_bytes(const Json& value) {
  const Json* const payload = value.find(tags::binary);
  if (payload == nullptr || !payload->is_string()) return std::nullopt;
  auto decoded = util::base64_decode(payload->as_string());
  if (!decoded) return std::nullopt;
  return std::move(*decoded);
}

std::string_view tag_text(const Json& value) noexcept {
  if (!value.is_object() || value.empty()) return {};
  for (const auto& member : value.items()) {
    if (member.key() != tags::regex_options && member.value().is_string()) return member.value().as_string();
  }
  return {};
}

Json plain_value(const Json& tagged) {
  switch (tag_kind(tagged)) {
    case TagKind::None: return tagged;
    case TagKind::Binary:
    case TagKind::Ext: return tagged[tags::binary];
    case TagKind::Date:
    case TagKind::BigInt:
    case TagKind::Decimal:
    case TagKind::ObjectId:
    case TagKind::Code:
    case TagKind::Symbol: return Json(tag_text(tagged));
    case TagKind::Float: {
      const std::string_view text = tag_text(tagged);
      if (text == "NaN") return std::numeric_limits<double>::quiet_NaN();
      return text == "Infinity" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
    }
    case TagKind::Uid: return tagged[tags::uid];
    case TagKind::CborTag: return tagged[tags::tag_value];
    case TagKind::Undefined: return nullptr;
    case TagKind::Regex:
      return Json(std::format("/{}/{}", tagged[tags::regex].as_string(), tagged[tags::regex_options].as_string()));
    case TagKind::Timestamp: {
      const Json& ts = tagged[tags::timestamp];
      return (ts["t"].as_uint64() << 32) | ts["i"].as_uint64();
    }
    case TagKind::MinKey: return Json("MinKey");
    case TagKind::MaxKey: return Json("MaxKey");
  }
  return tagged;
}

Json untag(const Json& value, bool keep_numbers) {
  const TagKind kind = tag_kind(value);
  if (kind != TagKind::None) {
    if (keep_numbers && (kind == TagKind::BigInt || kind == TagKind::Decimal || kind == TagKind::Float)) return value;
    if (kind == TagKind::CborTag) return untag(value[tags::tag_value], keep_numbers);
    return plain_value(value);
  }
  if (value.is_array()) {
    Json out = Json::array();
    out.reserve(value.size());
    for (const Json& element : value.elements()) out.push_back(untag(element, keep_numbers));
    return out;
  }
  if (value.is_object()) {
    Json out = Json::object();
    out.reserve(value.size());
    for (const auto& member : value.items()) out.set(member.key(), untag(member.value(), keep_numbers));
    return out;
  }
  return value;
}

// =========================================================================== numbers

namespace {

/// Significant digits (no leading/trailing zeros) and the decimal exponent of
/// the first digit, of a decimal literal "[-]ddd[.ddd][e[+-]ddd]".
struct Significand {
  std::string digits;
  std::int64_t exponent = 0;  // value = 0.d1d2d3... * 10^exponent
  bool zero = true;
};

Significand significand_of(std::string_view literal) {
  Significand out;
  std::size_t i = 0;
  if (i < literal.size() && (literal[i] == '-' || literal[i] == '+')) ++i;
  std::int64_t point = 0;  // digits before the decimal point
  bool seen_point = false;
  bool leading = true;
  std::int64_t leading_zeros_after_point = 0;
  for (; i < literal.size(); ++i) {
    const char c = literal[i];
    if (c == '.') {
      seen_point = true;
      continue;
    }
    if (!util::is_ascii_digit(c)) break;
    if (leading && c == '0') {
      if (seen_point) ++leading_zeros_after_point;
      continue;
    }
    leading = false;
    if (!seen_point) ++point;
    if (out.digits.size() < 1200) out.digits.push_back(c);  // more cannot matter for a double
  }
  std::int64_t exponent = 0;
  if (i < literal.size() && (literal[i] == 'e' || literal[i] == 'E')) {
    ++i;
    bool negative = false;
    if (i < literal.size() && (literal[i] == '+' || literal[i] == '-')) negative = literal[i++] == '-';
    for (; i < literal.size() && util::is_ascii_digit(literal[i]); ++i) {
      if (exponent < 100'000'000) exponent = exponent * 10 + (literal[i] - '0');
    }
    if (negative) exponent = -exponent;
  }
  while (!out.digits.empty() && out.digits.back() == '0') out.digits.pop_back();
  if (out.digits.empty()) return out;
  out.zero = false;
  out.exponent = (point > 0 ? point : -leading_zeros_after_point) + exponent;
  return out;
}

/// The exact value of a finite double as "<digits>e<exponent>".
std::string exact_decimal(double value) {
  if (value == 0.0 || !std::isfinite(value)) return "0";
  int binary_exponent = 0;
  const double fraction = std::frexp(std::abs(value), &binary_exponent);
  auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 53));
  int exponent = binary_exponent - 53;
  while (mantissa % 2 == 0 && exponent < 0) {
    mantissa /= 2;
    ++exponent;
  }
  // Base 10^9 limbs, least significant first.
  std::vector<std::uint32_t> limbs{static_cast<std::uint32_t>(mantissa % 1'000'000'000u),
                                   static_cast<std::uint32_t>(mantissa / 1'000'000'000u % 1'000'000'000u),
                                   static_cast<std::uint32_t>(mantissa / 1'000'000'000'000'000'000u)};
  const auto multiply = [&](std::uint32_t factor) {
    std::uint64_t carry = 0;
    for (std::uint32_t& limb : limbs) {
      const std::uint64_t product = static_cast<std::uint64_t>(limb) * factor + carry;
      limb = static_cast<std::uint32_t>(product % 1'000'000'000u);
      carry = product / 1'000'000'000u;
    }
    while (carry != 0) {
      limbs.push_back(static_cast<std::uint32_t>(carry % 1'000'000'000u));
      carry /= 1'000'000'000u;
    }
  };
  int decimal_exponent = 0;
  for (int i = 0; i < -exponent; ++i) multiply(5);
  if (exponent < 0) decimal_exponent = exponent;
  for (int i = 0; i < exponent; ++i) multiply(2);
  while (limbs.size() > 1 && limbs.back() == 0) limbs.pop_back();
  std::string digits = std::to_string(limbs.back());
  for (std::size_t i = limbs.size() - 1; i-- > 0;) {
    const std::string part = std::to_string(limbs[i]);
    digits.append(9 - part.size(), '0');
    digits += part;
  }
  return digits + "e" + std::to_string(decimal_exponent);
}

/// "+.5" -> "0.5", "5." -> "5.0", "+1E3" -> "1E3": the JSON spelling of a
/// decimal literal that is otherwise valid.
std::string json_spelling(std::string_view literal) {
  std::string out;
  std::size_t i = 0;
  if (i < literal.size() && (literal[i] == '+' || literal[i] == '-')) {
    if (literal[i] == '-') out.push_back('-');
    ++i;
  }
  // Integer part without redundant leading zeros.
  const std::size_t int_start = i;
  while (i < literal.size() && util::is_ascii_digit(literal[i])) ++i;
  std::string_view integer = literal.substr(int_start, i - int_start);
  while (integer.size() > 1 && integer.front() == '0') integer.remove_prefix(1);
  out.append(integer.empty() ? "0" : integer);
  if (i < literal.size() && literal[i] == '.') {
    ++i;
    const std::size_t frac_start = i;
    while (i < literal.size() && util::is_ascii_digit(literal[i])) ++i;
    const std::string_view fraction = literal.substr(frac_start, i - frac_start);
    out.push_back('.');
    out.append(fraction.empty() ? "0" : fraction);
  }
  if (i < literal.size() && (literal[i] == 'e' || literal[i] == 'E')) {
    out.push_back(literal[i++]);
    if (i < literal.size() && (literal[i] == '+' || literal[i] == '-')) out.push_back(literal[i++]);
    const std::size_t exp_start = i;
    while (i < literal.size() && util::is_ascii_digit(literal[i])) ++i;
    out.append(literal.substr(exp_start, i - exp_start));
    if (i == exp_start) out.push_back('0');
  }
  return out;
}

}  // namespace

Json number_from_literal(std::string_view literal) {
  const bool is_float = literal.find_first_of(".eE") != std::string_view::npos;
  if (!is_float) {
    bool negative = false;
    std::string_view digits = literal;
    if (!digits.empty() && (digits.front() == '-' || digits.front() == '+')) {
      negative = digits.front() == '-';
      digits.remove_prefix(1);
    }
    while (digits.size() > 1 && digits.front() == '0') digits.remove_prefix(1);
    std::uint64_t magnitude = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), magnitude);
    if (error == std::errc{} && end == digits.data() + digits.size() && !digits.empty()) {
      if (!negative) {
        if (magnitude <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return static_cast<std::int64_t>(magnitude);
        }
        return magnitude;
      }
      if (magnitude <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return -static_cast<std::int64_t>(magnitude);
      }
      if (magnitude == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1) {
        return std::numeric_limits<std::int64_t>::min();
      }
    }
    if (digits.empty()) return 0;
    return make_bigint((negative ? "-" : "") + std::string(digits));
  }
  std::string text = json_spelling(literal);
  const std::optional<double> parsed = util::parse_double(text);
  if (!parsed) return make_decimal(std::move(text));
  const double value = *parsed;
  const Significand wanted = significand_of(text);
  if (!std::isfinite(value)) return make_decimal(std::move(text));
  if (wanted.zero) return value;  // 0.0 / -0.0
  if (value == 0.0) return make_decimal(std::move(text));
  std::array<char, 64> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific);
  if (error != std::errc{}) return make_decimal(std::move(text));
  const Significand actual = significand_of(std::string_view(buffer.data(), end));
  if (actual.digits == wanted.digits && actual.exponent == wanted.exponent) return value;
  // Printed with 17 significant digits ("%.17g", as Core Foundation and many
  // C programs do): the text names this double.
  if (wanted.digits.size() <= 17) {
    const auto [end17, error17] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific, 16);
    if (error17 == std::errc{}) {
      const Significand seventeen = significand_of(std::string_view(buffer.data(), end17));
      if (seventeen.digits == wanted.digits && seventeen.exponent == wanted.exponent) return value;
    }
  }
  // The complete decimal expansion of this double (some writers print it).
  if (wanted.digits.size() > 17 && wanted.digits.size() <= 1100) {
    const Significand exact = significand_of(exact_decimal(value));
    if (exact.digits == wanted.digits && exact.exponent == wanted.exponent) return value;
  }
  return make_decimal(std::move(text));
}

std::optional<Json> integer_from_digits(std::string_view digits, int base, bool negative) {
  if (digits.empty()) return std::nullopt;
  // Accumulate in 32-bit limbs (little endian) so any length works.
  std::vector<std::uint32_t> limbs{0};
  for (const char c : digits) {
    int digit = -1;
    if (c >= '0' && c <= '9') digit = c - '0';
    if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
    if (digit < 0 || digit >= base) return std::nullopt;
    std::uint64_t carry = static_cast<std::uint64_t>(digit);
    for (std::uint32_t& limb : limbs) {
      const std::uint64_t product = static_cast<std::uint64_t>(limb) * static_cast<std::uint64_t>(base) + carry;
      limb = static_cast<std::uint32_t>(product);
      carry = product >> 32;
    }
    if (carry != 0) limbs.push_back(static_cast<std::uint32_t>(carry));
    if (limbs.size() > 512) return std::nullopt;
  }
  while (limbs.size() > 1 && limbs.back() == 0) limbs.pop_back();
  if (limbs.size() <= 2) {
    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(limbs[0]) | (limbs.size() > 1 ? static_cast<std::uint64_t>(limbs[1]) << 32 : 0);
    if (!negative) {
      if (magnitude <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Json(static_cast<std::int64_t>(magnitude));
      }
      return Json(magnitude);
    }
    if (magnitude <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return Json(-static_cast<std::int64_t>(magnitude));
    }
    if (magnitude == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1) {
      return Json(std::numeric_limits<std::int64_t>::min());
    }
  }
  Bytes magnitude;
  for (auto it = limbs.rbegin(); it != limbs.rend(); ++it) {
    for (int shift = 24; shift >= 0; shift -= 8) magnitude.push_back(static_cast<std::byte>((*it >> shift) & 0xFF));
  }
  return make_bigint(magnitude_to_decimal(magnitude, negative));
}

std::string format_double(double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
  return util::dump_json(Json(value));
}

std::string format_integer(const Json& value) {
  if (value.is_unsigned()) return std::to_string(value.as_uint64());
  if (value.is_integer()) return std::to_string(value.as_int64());
  if (const auto whole = value.get_int64()) return std::to_string(*whole);
  return format_double(value.as_double());
}

std::optional<Bytes> decimal_to_magnitude(std::string_view digits, bool* negative) {
  bool minus = false;
  if (!digits.empty() && digits.front() == '-') {
    minus = true;
    digits.remove_prefix(1);
  }
  if (digits.empty() || digits.size() > 4096) return std::nullopt;
  std::vector<std::uint32_t> limbs{0};  // little-endian base 2^32
  for (const char c : digits) {
    if (!util::is_ascii_digit(c)) return std::nullopt;
    std::uint64_t carry = static_cast<std::uint64_t>(c - '0');
    for (std::uint32_t& limb : limbs) {
      const std::uint64_t product = static_cast<std::uint64_t>(limb) * 10u + carry;
      limb = static_cast<std::uint32_t>(product);
      carry = product >> 32;
    }
    if (carry != 0) limbs.push_back(static_cast<std::uint32_t>(carry));
  }
  Bytes out;
  for (auto it = limbs.rbegin(); it != limbs.rend(); ++it) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::byte>((*it >> shift) & 0xFF));
  }
  const auto first = std::ranges::find_if(out, [](std::byte b) { return b != std::byte{0}; });
  out.erase(out.begin(), first);
  if (negative != nullptr) *negative = minus && !out.empty();
  return out;
}

std::string magnitude_to_decimal(ByteView magnitude, bool negative) {
  // Repeated division of the big-endian byte string by 10^9.
  std::vector<std::uint32_t> words;  // big-endian base 2^8 as uint32 for convenience
  words.reserve(magnitude.size());
  for (const std::byte b : magnitude) words.push_back(std::to_integer<std::uint32_t>(b));
  std::vector<std::uint32_t> chunks;  // little-endian base 10^9
  std::size_t start = 0;
  while (start < words.size() && words[start] == 0) ++start;
  while (start < words.size()) {
    std::uint64_t remainder = 0;
    for (std::size_t i = start; i < words.size(); ++i) {
      const std::uint64_t current = (remainder << 8) | words[i];
      words[i] = static_cast<std::uint32_t>(current / 1'000'000'000u);
      remainder = current % 1'000'000'000u;
    }
    chunks.push_back(static_cast<std::uint32_t>(remainder));
    while (start < words.size() && words[start] == 0) ++start;
  }
  if (chunks.empty()) return "0";
  std::string out = negative ? "-" : "";
  out += std::to_string(chunks.back());
  for (std::size_t i = chunks.size() - 1; i-- > 0;) {
    const std::string part = std::to_string(chunks[i]);
    out.append(9 - part.size(), '0');
    out += part;
  }
  return out;
}

std::string twos_complement_to_decimal(ByteView bytes) {
  if (bytes.empty()) return "0";
  const bool negative = (std::to_integer<unsigned>(bytes[0]) & 0x80u) != 0;
  if (!negative) return magnitude_to_decimal(bytes, false);
  Bytes magnitude(bytes.begin(), bytes.end());
  for (std::byte& b : magnitude) b = ~b;
  for (std::size_t i = magnitude.size(); i-- > 0;) {  // + 1
    const unsigned v = std::to_integer<unsigned>(magnitude[i]) + 1;
    magnitude[i] = static_cast<std::byte>(v & 0xFF);
    if (v <= 0xFF) break;
  }
  return magnitude_to_decimal(magnitude, true);
}

// =========================================================================== dates

std::uint32_t DateTime::nanos() const noexcept {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 9; ++i) {
    value = value * 10 + (i < fraction.size() ? static_cast<std::uint32_t>(fraction[i] - '0') : 0u);
  }
  return value;
}

std::int64_t days_from_civil(std::int64_t y, int m, int d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<std::int64_t>(y - era * 400);
  const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civil_from_days(std::int64_t days, std::int64_t& y, int& m, int& d) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::int64_t doe = days - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = yoe + era * 400 + (m <= 2 ? 1 : 0);
}

namespace {

bool leap_year(std::int64_t y) noexcept { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(std::int64_t y, int m) noexcept {
  static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leap_year(y) ? 29 : days[m - 1];
}

bool fixed_digits(std::string_view text, std::size_t at, std::size_t count, int& out) noexcept {
  if (at + count > text.size()) return false;
  int value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (!util::is_ascii_digit(text[at + i])) return false;
    value = value * 10 + (text[at + i] - '0');
  }
  out = value;
  return true;
}

/// "hh:mm[:ss[.fff]]" at `at`; returns the position after it.
std::optional<std::size_t> parse_time_part(std::string_view text, std::size_t at, DateTime& out) noexcept {
  if (!fixed_digits(text, at, 2, out.hour) || at + 2 >= text.size() || text[at + 2] != ':' ||
      !fixed_digits(text, at + 3, 2, out.minute)) {
    return std::nullopt;
  }
  std::size_t i = at + 5;
  out.second = 0;
  if (i < text.size() && text[i] == ':') {
    if (!fixed_digits(text, i + 1, 2, out.second)) return std::nullopt;
    i += 3;
    if (i < text.size() && (text[i] == '.' || text[i] == ',')) {
      const std::size_t start = ++i;
      while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
      if (i == start || i - start > 64) return std::nullopt;
      out.fraction = std::string(text.substr(start, i - start));
    }
  }
  if (out.hour > 23 || out.minute > 59 || out.second > 60) return std::nullopt;
  return i;
}

}  // namespace

std::optional<DateTime> parse_datetime(std::string_view text) noexcept {
  DateTime out;
  // Time of day only.
  if (text.size() >= 5 && text[2] == ':') {
    const auto end = parse_time_part(text, 0, out);
    if (!end || *end != text.size()) return std::nullopt;
    out.kind = DateTime::Kind::Time;
    return out;
  }
  std::size_t i = 0;
  bool negative = false;
  bool extended = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    negative = text[i] == '-';
    extended = true;
    ++i;
  }
  const std::size_t year_start = i;
  std::int64_t year = 0;
  while (i < text.size() && util::is_ascii_digit(text[i])) {
    year = year * 10 + (text[i] - '0');
    if (++i - year_start > 9) return std::nullopt;
  }
  const std::size_t year_digits = i - year_start;
  if (year_digits < 4 || (!extended && year_digits != 4)) return std::nullopt;
  out.year = negative ? -year : year;
  if (i >= text.size() || text[i] != '-' || !fixed_digits(text, i + 1, 2, out.month) || i + 3 >= text.size() ||
      text[i + 3] != '-' || !fixed_digits(text, i + 4, 2, out.day)) {
    return std::nullopt;
  }
  if (out.month < 1 || out.month > 12 || out.day < 1 || out.day > days_in_month(out.year, out.month)) {
    return std::nullopt;
  }
  i += 6;
  if (i == text.size()) {
    out.kind = DateTime::Kind::Date;
    return out;
  }
  if (text[i] != 'T' && text[i] != 't' && text[i] != ' ') return std::nullopt;
  const auto end = parse_time_part(text, i + 1, out);
  if (!end) return std::nullopt;
  i = *end;
  if (i == text.size()) {
    out.kind = DateTime::Kind::Local;
    return out;
  }
  out.kind = DateTime::Kind::Offset;
  if (text[i] == 'Z' || text[i] == 'z') {
    out.zulu = true;
    return i + 1 == text.size() ? std::optional<DateTime>(out) : std::nullopt;
  }
  if (text[i] != '+' && text[i] != '-') return std::nullopt;
  const int sign = text[i] == '-' ? -1 : 1;
  int hours = 0;
  int minutes = 0;
  if (!fixed_digits(text, i + 1, 2, hours)) return std::nullopt;
  i += 3;
  if (i < text.size() && text[i] == ':') ++i;
  if (i < text.size()) {
    if (!fixed_digits(text, i, 2, minutes)) return std::nullopt;
    i += 2;
  }
  if (i != text.size() || hours > 23 || minutes > 59) return std::nullopt;
  out.offset_minutes = sign * (hours * 60 + minutes);
  out.zulu = out.offset_minutes == 0;  // +00:00 and -00:00 are UTC
  return out;
}

std::string format_datetime(const DateTime& value) {
  std::string out;
  const auto date = [&] {
    if (value.year >= 0 && value.year <= 9999) {
      out += std::format("{:04}-{:02}-{:02}", value.year, value.month, value.day);
    } else {
      out += std::format("{}{:04}-{:02}-{:02}", value.year < 0 ? '-' : '+', value.year < 0 ? -value.year : value.year,
                         value.month, value.day);
    }
  };
  const auto time = [&] {
    out += std::format("{:02}:{:02}:{:02}", value.hour, value.minute, value.second);
    if (!value.fraction.empty()) {
      out.push_back('.');
      out += value.fraction;
    }
  };
  switch (value.kind) {
    case DateTime::Kind::Date: date(); break;
    case DateTime::Kind::Time: time(); break;
    case DateTime::Kind::Local:
      date();
      out.push_back('T');
      time();
      break;
    case DateTime::Kind::Offset:
      date();
      out.push_back('T');
      time();
      if (value.zulu) {
        out.push_back('Z');
      } else {
        const int magnitude = value.offset_minutes < 0 ? -value.offset_minutes : value.offset_minutes;
        out += std::format("{}{:02}:{:02}", value.offset_minutes < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
      }
      break;
  }
  return out;
}

std::int64_t to_unix_seconds(const DateTime& value) noexcept {
  const std::int64_t days = value.kind == DateTime::Kind::Time ? 0 : days_from_civil(value.year, value.month, value.day);
  std::int64_t seconds = days * 86400 + value.hour * 3600 + value.minute * 60 + value.second;
  if (value.kind == DateTime::Kind::Offset) seconds -= static_cast<std::int64_t>(value.offset_minutes) * 60;
  return seconds;
}

DateTime from_unix(std::int64_t seconds, std::uint32_t nanos) {
  DateTime out;
  out.kind = DateTime::Kind::Offset;
  out.zulu = true;
  // Keep the arithmetic in range: about +-292 billion years fit, clamp anyway.
  constexpr std::int64_t limit = 315'537'897'599LL * 100;  // far beyond any calendar a format uses
  seconds = std::clamp<std::int64_t>(seconds, -limit, limit);
  std::int64_t days = seconds / 86400;
  std::int64_t rest = seconds % 86400;
  if (rest < 0) {
    rest += 86400;
    --days;
  }
  civil_from_days(days, out.year, out.month, out.day);
  out.hour = static_cast<int>(rest / 3600);
  out.minute = static_cast<int>(rest % 3600 / 60);
  out.second = static_cast<int>(rest % 60);
  if (nanos > 0) {
    out.fraction = std::format("{:09}", nanos % 1'000'000'000u);
    while (!out.fraction.empty() && out.fraction.back() == '0') out.fraction.pop_back();
  }
  return out;
}

// =========================================================================== text input

Result<std::string> input_text(const ReadContext& ctx, std::string_view format_name, bool legacy_fallback) {
  const ByteView data = ctx.input;
  if (const auto bom = util::detect_bom(data)) {
    util::DecodedText decoded = util::decode_text(data, bom->encoding);
    if (decoded.replaced > 0 && bom->encoding != util::TextEncoding::Utf8) {
      return fail(Status::Decode, "the {} text contains malformed {} sequences", format_name,
                  util::encoding_name(bom->encoding));
    }
    if (decoded.replaced > 0) {
      ctx.warn(std::format("{} malformed UTF-8 sequence(s) were replaced with U+FFFD.", decoded.replaced));
    }
    return std::move(decoded.text);
  }
  const std::string_view text = as_chars(data);
  if (util::is_valid_utf8(text)) return std::string(text);
  const util::EncodingGuess guess = util::guess_encoding(data);
  if (guess.confident && guess.encoding != util::TextEncoding::Utf8) {
    util::DecodedText decoded = util::decode_text(data.subspan(guess.bom_length), guess.encoding);
    if (decoded.replaced == 0) return std::move(decoded.text);
  }
  if (legacy_fallback) {
    ctx.warn(std::format("The {} file is not UTF-8; it was read as Windows-1252.", format_name));
    return util::decode_text(data, util::TextEncoding::Windows1252).text;
  }
  std::size_t offset = 0;
  while (offset < text.size()) {
    const util::Utf8Decoded d = util::decode_utf8(text, offset);
    if (!d.ok) break;
    offset += d.length;
  }
  const TextPosition at = position_of(text, offset);
  return fail(Status::Decode, "invalid {} at line {}, column {}: the text is not valid UTF-8 (byte {})", format_name,
              at.line, at.column, offset);
}

TextPosition position_of(std::string_view text, std::size_t offset) noexcept {
  TextPosition at;
  offset = std::min(offset, text.size());
  std::size_t i = 0;
  if (text.starts_with("\xEF\xBB\xBF") && offset >= 3) i = 3;
  for (; i < offset; ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '\n' || (c == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n'))) {
      ++at.line;
      at.column = 1;
    } else if (c == '\r') {
      // CR of a CRLF: the LF ends the line.
    } else if ((c & 0xC0) != 0x80) {
      ++at.column;
    }
  }
  return at;
}

std::unexpected<Error> syntax_error(std::string_view format_name, std::string_view text, std::size_t offset,
                                    std::string_view message) {
  const TextPosition at = position_of(text, offset);
  return fail(Status::Decode, "invalid {} at line {}, column {}: {}", format_name, at.line, at.column, message);
}

void append_json_string(std::string& out, std::string_view text, bool ascii) {
  static constexpr char hex[] = "0123456789abcdef";
  out.push_back('"');
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20 || (ascii && c == 0x7F)) {
            out += "\\u00";
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
          } else {
            out.push_back(static_cast<char>(c));
          }
      }
      ++i;
      continue;
    }
    const util::Utf8Decoded d = util::decode_utf8(text, i);
    const char32_t cp = d.ok ? d.code_point : util::replacement_character;
    if (ascii) {
      const auto unit = [&](std::uint32_t u) {
        out += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4) out.push_back(hex[(u >> shift) & 15]);
      };
      if (cp >= 0x10000) {
        const std::uint32_t v = static_cast<std::uint32_t>(cp) - 0x10000;
        unit(0xD800 + (v >> 10));
        unit(0xDC00 + (v & 0x3FF));
      } else {
        unit(static_cast<std::uint32_t>(cp));
      }
    } else if (d.ok) {
      out.append(text.substr(i, d.length));
    } else {
      util::append_utf8(out, util::replacement_character);
    }
    i += d.length;
  }
  out.push_back('"');
}

std::string repair_utf8(std::string_view text, std::size_t* repaired) { return util::sanitize_utf8(text, repaired); }

// =========================================================================== policies

ReadPolicy read_policy(const Options& options) {
  ReadPolicy policy;
  policy.max_depth = static_cast<std::size_t>(options.get_int("max_depth", 256, 1, 2048));
  const std::string duplicates = options.get_enum("duplicate_keys", "last", {"last", "first", "error"});
  policy.duplicates = duplicates == "first"   ? ReadPolicy::Duplicates::First
                      : duplicates == "error" ? ReadPolicy::Duplicates::Error
                                              : ReadPolicy::Duplicates::Last;
  return policy;
}

Result<void> put_member(Json& object, std::string key, Json value, const ReadPolicy& policy) {
  if (!object.is_object()) object = Json::object();
  switch (policy.duplicates) {
    case ReadPolicy::Duplicates::Last: object.set(key, std::move(value)); break;
    case ReadPolicy::Duplicates::First: object.insert(key, std::move(value)); break;
    case ReadPolicy::Duplicates::Error:
      if (!object.insert(key, std::move(value))) {
        return fail(Status::Decode, "duplicate key \"{}\" (option duplicate_keys is \"error\")", key);
      }
      break;
  }
  return {};
}

namespace {

std::string_view tag_description(TagKind kind) {
  switch (kind) {
    case TagKind::Binary: return "binary data";
    case TagKind::Ext: return "an extension type";
    case TagKind::Date: return "a date or time";
    case TagKind::Uid: return "a UID";
    case TagKind::CborTag: return "a tagged value";
    case TagKind::Undefined: return "an undefined value";
    case TagKind::ObjectId: return "an ObjectId";
    case TagKind::Regex: return "a regular expression";
    case TagKind::Timestamp: return "a BSON timestamp";
    case TagKind::MinKey:
    case TagKind::MaxKey: return "a min/max key";
    case TagKind::Code: return "JavaScript code";
    case TagKind::Symbol: return "a symbol";
    default: return "a special value";
  }
}

}  // namespace

Result<void> finish_read(Json& value, const Context& ctx) {
  const std::string special = ctx.options.get_enum("special_values", "tagged", {"tagged", "plain", "error"});
  const std::string big = ctx.options.get_enum("big_numbers", "exact", {"exact", "double", "string", "error"});
  const auto max_depth = static_cast<std::size_t>(ctx.options.get_int("max_depth", 256, 1, 2048));
  std::vector<std::pair<Json*, std::size_t>> stack{{&value, 0}};
  std::size_t converted = 0;
  while (!stack.empty()) {
    auto [node, depth] = stack.back();
    stack.pop_back();
    const TagKind kind = tag_kind(*node);
    if (kind == TagKind::Float) {
      *node = plain_value(*node);
      continue;
    }
    if (kind == TagKind::BigInt || kind == TagKind::Decimal) {
      if (big == "exact") continue;
      const std::string text(tag_text(*node));
      if (big == "error") {
        return fail(Status::Decode, "the number {} cannot be held exactly as a 64-bit integer or double "
                    "(option big_numbers is \"error\")", text);
      }
      if (big == "string") {
        *node = Json(text);
      } else {
        *node = util::parse_double(text).value_or(0.0);
      }
      ++converted;
      continue;
    }
    if (kind != TagKind::None) {
      if (special == "error") {
        return fail(Status::Decode, "the input contains {}, which has no JSON equivalent (option special_values is "
                    "\"error\")", tag_description(kind));
      }
      if (special == "plain") {
        *node = plain_value(*node);
        ++converted;
        // A CBOR tag's content may itself hold tagged values.
        if (node->is_structured()) stack.emplace_back(node, depth);
        continue;
      }
      if (kind == TagKind::CborTag) {
        if (Json* inner = node->find(ir::data_tags::tag_value)) stack.emplace_back(inner, depth + 1);
      }
      continue;
    }
    if (node->is_structured()) {
      if (depth >= max_depth) {
        return fail(Status::Limit, "the data is nested more than {} levels deep (option max_depth)", max_depth);
      }
      if (node->is_array()) {
        for (Json& element : node->elements()) {
          if (element.is_structured()) stack.emplace_back(&element, depth + 1);
        }
      } else {
        for (auto& member : node->items()) {
          if (member.value().is_structured()) stack.emplace_back(&member.value(), depth + 1);
        }
      }
    }
  }
  (void)converted;
  return {};
}

std::size_t json_depth(const Json& value) {
  std::size_t deepest = 0;
  std::vector<std::pair<const Json*, std::size_t>> stack{{&value, 0}};
  while (!stack.empty()) {
    const auto [node, depth] = stack.back();
    stack.pop_back();
    if (!node->is_structured()) continue;
    deepest = std::max(deepest, depth + 1);
    if (node->is_array()) {
      for (const Json& element : node->elements()) {
        if (element.is_structured()) stack.emplace_back(&element, depth + 1);
      }
    } else {
      for (const auto& member : node->items()) {
        if (member.value().is_structured()) stack.emplace_back(&member.value(), depth + 1);
      }
    }
  }
  return deepest;
}

Result<void> check_write_depth(const Json& value, std::string_view format_name) {
  if (json_depth(value) > max_write_depth) {
    return fail(Status::Encode, "the data is nested more than {} levels deep, too deep to write as {}",
                max_write_depth, format_name);
  }
  return {};
}

std::vector<const Json::Member*> members_of(const Json& object, bool sort) {
  std::vector<const Json::Member*> out;
  out.reserve(object.size());
  for (const auto& member : object.items()) out.push_back(&member);
  if (sort) {
    std::ranges::stable_sort(out, [](const Json::Member* a, const Json::Member* b) { return a->key() < b->key(); });
  }
  return out;
}

std::vector<OptionSpec> common_read_options(bool with_duplicates, bool with_numbers) {
  std::vector<OptionSpec> specs;
  specs.push_back(OptionSpec::choice(
      "special_values", "Special values", "tagged", {"tagged", "plain", "error"},
      "How values without a JSON equivalent (binary data, dates, ...) are kept: as tagged objects such as "
      "{\"$binary\": ...} that other formats understand, as plain strings and numbers, or rejected."));
  if (with_numbers) {
    specs.push_back(OptionSpec::choice("big_numbers", "Big numbers", "exact", {"exact", "double", "string", "error"},
                                       "Numbers beyond 64-bit integers or double precision: kept exactly as "
                                       "{\"$bigint\"}/{\"$decimal\"}, rounded to a double, kept as text, or rejected.")
                        .advanced());
  }
  if (with_duplicates) {
    specs.push_back(OptionSpec::choice("duplicate_keys", "Duplicate keys", "last", {"last", "first", "error"},
                                       "Which value a repeated key keeps, or whether it is an error.")
                        .advanced());
  }
  specs.push_back(OptionSpec::integer("max_depth", "Maximum nesting", 256, 1, 2048,
                                      "Deepest nesting of arrays and objects accepted.")
                      .advanced());
  return specs;
}

OptionSpec indent_option(int default_indent) {
  return OptionSpec::integer("indent", "Indentation", default_indent, 0, 16, "Spaces per nesting level.");
}

OptionSpec sort_keys_option() {
  return OptionSpec::boolean("sort_keys", "Sort keys", false, "Write object keys in sorted order instead of the "
                                                              "order they were read in.");
}

OptionSpec documents_option() {
  return OptionSpec::choice("documents", "Documents", "auto", {"auto", "array", "first", "index"},
                            "For inputs holding several documents: auto (one document as is, several as an array), "
                            "always an array, only the first, or the one at 'document_index'.");
}

OptionSpec document_index_option() {
  return OptionSpec::integer("document_index", "Document index", 0, 0, 1'000'000,
                             "0-based document to keep when 'documents' is \"index\".")
      .advanced();
}

Result<Json> select_documents(std::vector<Json> documents, const Options& options, std::string_view format_name) {
  const std::string mode = options.get_enum("documents", "auto", {"auto", "array", "first", "index"});
  if (mode == "array" || (mode == "auto" && documents.size() > 1)) {
    Json out = Json::array();
    out.reserve(documents.size());
    for (Json& document : documents) out.push_back(std::move(document));
    return out;
  }
  if (mode == "index") {
    const auto index = static_cast<std::size_t>(options.get_int("document_index", 0, 0, 1'000'000));
    if (index >= documents.size()) {
      return fail(Status::InvalidArgument, "document_index {} is out of range: the {} input has {} document(s)",
                  index, format_name, documents.size());
    }
    return std::move(documents[index]);
  }
  if (documents.empty()) return Json();
  return std::move(documents.front());
}

// --------------------------------------------------------------------------- registration

void add_codec(Registry& registry, CodecDef def) {
  Codec codec;
  codec.format_id = std::move(def.format_id);
  codec.kind = Kind::Data;
  codec.read = std::move(def.read);
  codec.write = std::move(def.write);
  codec.read_options = std::move(def.read_options);
  codec.write_options = std::move(def.write_options);
  codec.lossy = def.lossy;
  codec.read_cost = def.read_cost;
  codec.write_cost = def.write_cost;
  registry.add_codec(std::move(codec));
}

std::function<Result<AnyIR>(ReadContext&)> text_reader(TextParser parser, std::string format_name,
                                                       bool legacy_fallback) {
  return [parser, format_name = std::move(format_name), legacy_fallback](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(const std::string text, input_text(ctx, format_name, legacy_fallback));
    CVT_TRY_ASSIGN(Json value, parser(text, ctx));
    CVT_TRY(finish_read(value, ctx));
    return AnyIR(ir::Data{std::move(value)});
  };
}

std::function<Result<AnyIR>(ReadContext&)> binary_reader(BinaryParser parser) {
  return [parser](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(Json value, parser(ctx.input, ctx));
    CVT_TRY(finish_read(value, ctx));
    return AnyIR(ir::Data{std::move(value)});
  };
}

std::function<Result<Bytes>(const AnyIR&, WriteContext&)> text_writer(TextWriter writer, std::string format_name) {
  return [writer, format_name = std::move(format_name)](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    const ir::Data& data = value.get<ir::Data>();
    CVT_TRY(check_write_depth(data.value, format_name));
    CVT_TRY_ASSIGN(const std::string text, writer(data.value, ctx));
    CVT_TRY(ctx.limits.check_output(text.size()));
    return to_bytes(text);
  };
}

std::function<Result<Bytes>(const AnyIR&, WriteContext&)> binary_writer(BinaryWriter writer, std::string format_name) {
  return [writer, format_name = std::move(format_name)](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    const ir::Data& data = value.get<ir::Data>();
    CVT_TRY(check_write_depth(data.value, format_name));
    CVT_TRY_ASSIGN(Bytes bytes, writer(data.value, ctx));
    CVT_TRY(ctx.limits.check_output(bytes.size()));
    return bytes;
  };
}

bool has_extension(std::string_view filename, std::initializer_list<std::string_view> exts) noexcept {
  const std::size_t dot = filename.rfind('.');
  if (dot == std::string_view::npos) return false;
  const std::string_view extension = filename.substr(dot + 1);
  return std::ranges::any_of(exts, [&](std::string_view e) { return util::iequals(e, extension); });
}

std::string_view text_head(ByteView data, std::size_t limit) noexcept {
  std::string_view text = as_chars(data).substr(0, limit);
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  return util::trim_left(text);
}

}  // namespace cvt::data
