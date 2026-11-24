// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/hex.hpp"

#include <cstdint>

#include "util/text.hpp"

namespace cvt::util {

std::string hex_encode(ByteView data, bool uppercase) {
  const std::string_view digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (const std::byte b : data) {
    const auto v = std::to_integer<std::uint8_t>(b);
    out.push_back(digits[v >> 4]);
    out.push_back(digits[v & 0x0F]);
  }
  return out;
}

Result<Bytes> hex_decode(std::string_view text) {
  Bytes out;
  out.reserve(text.size() / 2);
  int high = -1;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (is_ascii_space(c)) continue;
    const int value = hex_digit_value(c);
    if (value < 0) return fail(Status::Decode, "invalid hexadecimal character at offset {}", i);
    if (high < 0) {
      high = value;
    } else {
      out.push_back(static_cast<std::byte>((high << 4) | value));
      high = -1;
    }
  }
  if (high >= 0) return fail(Status::Decode, "odd number of hexadecimal digits");
  return out;
}

}  // namespace cvt::util
