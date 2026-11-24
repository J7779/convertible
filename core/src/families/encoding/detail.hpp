// Convertible core - encoding family: small helpers shared by the encoding sources.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::encoding::detail {

inline constexpr std::string_view lower_hex_digits = "0123456789abcdef";
inline constexpr std::string_view upper_hex_digits = "0123456789ABCDEF";

[[nodiscard]] constexpr std::uint8_t byte_at(ByteView data, std::size_t index) noexcept {
  return std::to_integer<std::uint8_t>(data[index]);
}

/// The byte value of a character (never negative, whatever `char` is).
[[nodiscard]] constexpr std::uint8_t byte_of(char c) noexcept { return static_cast<std::uint8_t>(c); }

/// Two hexadecimal digits.
inline void append_hex_byte(std::string& out, std::uint8_t value, bool uppercase) {
  const std::string_view digits = uppercase ? upper_hex_digits : lower_hex_digits;
  out.push_back(digits[value >> 4]);
  out.push_back(digits[value & 0x0Fu]);
}

/// `value` in hexadecimal, zero-padded to at least `min_digits` digits.
inline void append_hex(std::string& out, std::uint32_t value, int min_digits, bool uppercase) {
  const std::string_view digits = uppercase ? upper_hex_digits : lower_hex_digits;
  int count = 1;
  for (std::uint32_t rest = value >> 4; rest != 0; rest >>= 4) ++count;
  if (count < min_digits) count = min_digits;
  for (int shift = (count - 1) * 4; shift >= 0; shift -= 4) {
    out.push_back(digits[shift >= 32 ? 0u : (value >> shift) & 0x0Fu]);
  }
}

/// Maps a character to its digit value, or -1.
using DigitTable = std::array<std::int8_t, 256>;

/// The decoding table of an alphabet whose digit values are the positions of
/// its characters. With `fold_case`, letters match in either case.
[[nodiscard]] constexpr DigitTable make_digit_table(std::string_view alphabet, bool fold_case) noexcept {
  DigitTable table{};
  for (std::int8_t& entry : table) entry = -1;
  for (std::size_t i = 0; i < alphabet.size(); ++i) {
    const char c = alphabet[i];
    const auto value = static_cast<std::int8_t>(i);
    table[byte_of(c)] = value;
    if (fold_case && c >= 'A' && c <= 'Z') table[byte_of(static_cast<char>(c - 'A' + 'a'))] = value;
    if (fold_case && c >= 'a' && c <= 'z') table[byte_of(static_cast<char>(c - 'a' + 'A'))] = value;
  }
  return table;
}

/// Refuse, before any memory is spent, an input whose output could exceed
/// the output limit: `factor` is the largest number of output bytes one
/// input byte can turn into.
[[nodiscard]] inline Result<void> check_expansion(const Context& ctx, std::size_t input_bytes, unsigned factor) {
  if (factor != 0 && input_bytes > ctx.limits.max_output_bytes / factor) {
    return fail(Status::Limit, "the encoded output would exceed the limit of {} bytes", ctx.limits.max_output_bytes);
  }
  return {};
}

}  // namespace cvt::encoding::detail
