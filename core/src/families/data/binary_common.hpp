// Convertible core - data family: helpers shared by the binary formats.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "families/data/data.hpp"
#include "util/text.hpp"

namespace cvt::data {

/// IEEE 754 half precision -> double (exact).
[[nodiscard]] inline double half_to_double(std::uint16_t half) noexcept {
  const int exponent = (half >> 10) & 0x1F;
  const int mantissa = half & 0x3FF;
  double value = 0.0;
  if (exponent == 0) {
    value = std::ldexp(static_cast<double>(mantissa), -24);
  } else if (exponent != 31) {
    value = std::ldexp(static_cast<double>(mantissa + 1024), exponent - 25);
  } else {
    value = mantissa == 0 ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
  }
  return (half & 0x8000) != 0 ? -value : value;
}

/// The half-precision bits of `value` when it converts exactly (NaN and the
/// infinities included).
[[nodiscard]] inline std::optional<std::uint16_t> exact_half(double value) noexcept {
  if (std::isnan(value)) return std::uint16_t{0x7E00};
  const std::uint16_t sign = std::signbit(value) ? 0x8000 : 0;
  if (std::isinf(value)) return static_cast<std::uint16_t>(sign | 0x7C00);
  if (value == 0.0) return sign;
  const double magnitude = std::abs(value);
  int exponent = 0;
  const double fraction = std::frexp(magnitude, &exponent);  // magnitude = fraction * 2^exponent, fraction in [0.5, 1)
  // Normal halves: exponent in [-13, 16]; subnormals down to 2^-24.
  if (exponent >= -13 && exponent <= 16) {
    const double scaled = std::ldexp(fraction, 11);  // 11 significant bits
    if (scaled != std::floor(scaled)) return std::nullopt;
    const auto mantissa = static_cast<std::uint16_t>(static_cast<int>(scaled) - 1024);
    return static_cast<std::uint16_t>(sign | ((exponent + 14) << 10) | mantissa);
  }
  if (exponent < -13 && exponent >= -23) {
    const double scaled = std::ldexp(magnitude, 24);
    if (scaled != std::floor(scaled)) return std::nullopt;
    return static_cast<std::uint16_t>(sign | static_cast<std::uint16_t>(scaled));
  }
  return std::nullopt;
}

/// True when `value` survives a round trip through float.
[[nodiscard]] inline bool exact_float(double value) noexcept {
  if (std::isnan(value)) return true;
  const auto f = static_cast<float>(value);
  return static_cast<double>(f) == value || (std::isinf(value) && std::isinf(f));
}

/// A string read from a binary format: valid UTF-8 stays, anything else is
/// repaired (counted in `repaired`).
[[nodiscard]] inline Json text_value(std::string_view bytes, std::size_t& repaired) {
  if (util::is_valid_utf8(bytes)) return Json(std::string(bytes));
  ++repaired;
  return Json(repair_utf8(bytes));
}

/// Object keys from non-string map keys: integers as decimal text, others
/// as compact JSON.
[[nodiscard]] inline std::string key_from_value(const Json& key) {
  if (key.is_string()) return key.as_string();
  if (key.is_integer()) return format_integer(key);
  if (key.is_bool()) return key.as_bool() ? "true" : "false";
  if (key.is_null()) return "null";
  if (key.is_float()) return format_double(key.as_double());
  if (const TagKind kind = tag_kind(key); kind == TagKind::BigInt || kind == TagKind::Decimal || kind == TagKind::Date) {
    return std::string(tag_text(key));
  }
  if (tag_kind(key) == TagKind::Binary) return key[ir::data_tags::binary].as_string();
  return util::dump_json(key);
}

/// Decimal text -> (sign, coefficient digits without leading zeros, base-10 exponent).
struct DecimalParts {
  bool negative = false;
  std::string digits;  // "0" for zero
  std::int64_t exponent = 0;
};
[[nodiscard]] inline std::optional<DecimalParts> decimal_parts(std::string_view text) {
  DecimalParts out;
  std::size_t i = 0;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) out.negative = text[i++] == '-';
  bool any = false;
  bool point = false;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '.') {
      if (point) return std::nullopt;
      point = true;
      continue;
    }
    if (!util::is_ascii_digit(c)) break;
    any = true;
    out.digits.push_back(c);
    if (point) --out.exponent;
  }
  if (!any) return std::nullopt;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E' || text[i] == 'd' || text[i] == 'D')) {
    const auto exponent = util::parse_int(text.substr(i + 1 + (i + 1 < text.size() && text[i + 1] == '+' ? 1 : 0)));
    if (!exponent || *exponent > 1'000'000'000 || *exponent < -1'000'000'000) return std::nullopt;
    out.exponent += *exponent;
    i = text.size();
  }
  if (i != text.size()) return std::nullopt;
  const std::size_t first = out.digits.find_first_not_of('0');
  out.digits = first == std::string::npos ? "0" : out.digits.substr(first);
  return out;
}

/// Decimal text (JSON grammar) for a coefficient and a power of ten, in the
/// canonical spelling of $decimal: positional for fractions ("3.14",
/// "0.001"), an exponent otherwise ("12e3", "5e-100").
[[nodiscard]] inline std::string decimal_text(bool negative, std::string_view digits, std::int64_t exponent) {
  std::string out = negative ? "-" : "";
  const std::string coefficient = digits.empty() ? std::string("0") : std::string(digits);
  if (exponent < 0 && exponent >= -60) {
    const auto places = static_cast<std::size_t>(-exponent);
    if (coefficient.size() > places) {
      out += coefficient.substr(0, coefficient.size() - places) + "." + coefficient.substr(coefficient.size() - places);
    } else {
      out += "0." + std::string(places - coefficient.size(), '0') + coefficient;
    }
    return out;
  }
  out += coefficient;
  if (exponent != 0) out += std::format("e{}", exponent);
  return out;
}

/// Decimal value as a Json number when a double holds it exactly, else $decimal.
[[nodiscard]] inline Json decimal_value(bool negative, std::string_view digits, std::int64_t exponent) {
  return number_from_literal(decimal_text(negative, digits, exponent));
}

/// Zig-zag decoding / encoding (protobuf sint, Avro, Smile).
[[nodiscard]] constexpr std::int64_t unzigzag(std::uint64_t value) noexcept {
  return static_cast<std::int64_t>(value >> 1) ^ -static_cast<std::int64_t>(value & 1);
}
[[nodiscard]] constexpr std::uint64_t zigzag(std::int64_t value) noexcept {
  return (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
}

/// Shared budget against inputs that expand (shared references, aliases).
struct NodeBudget {
  std::uint64_t used = 0;
  std::uint64_t limit = 0;
  explicit NodeBudget(std::size_t input_size)
      : limit(std::max<std::uint64_t>(1'000'000, static_cast<std::uint64_t>(input_size) * 32)) {}
  [[nodiscard]] Result<void> take(std::uint64_t count = 1) {
    used += count;
    if (used > limit) return fail(Status::Limit, "the data expands to more than {} values", limit);
    return {};
  }
};

}  // namespace cvt::data
