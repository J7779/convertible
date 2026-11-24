// Convertible core - hexadecimal encoding.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <string_view>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::util {

/// Value of a hex digit, or -1.
[[nodiscard]] constexpr int hex_digit_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/// Two hex digits per byte, no separators.
[[nodiscard]] std::string hex_encode(ByteView data, bool uppercase = false);

/// Decodes pairs of hex digits. ASCII whitespace between bytes is ignored.
/// Fails with Status::Decode on any other character or an odd digit count.
[[nodiscard]] Result<Bytes> hex_decode(std::string_view text);

}  // namespace cvt::util
