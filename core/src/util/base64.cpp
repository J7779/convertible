// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/base64.hpp"

#include <array>
#include <cstdint>

#include "util/text.hpp"

namespace cvt::util {

namespace {

constexpr std::string_view standard_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view url_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr std::array<std::int8_t, 256> make_decode_table() {
  std::array<std::int8_t, 256> table{};
  for (auto& entry : table) entry = -1;
  for (std::size_t i = 0; i < 64; ++i) {
    table[static_cast<unsigned char>(standard_alphabet[i])] = static_cast<std::int8_t>(i);
    table[static_cast<unsigned char>(url_alphabet[i])] = static_cast<std::int8_t>(i);
  }
  return table;
}
constexpr std::array<std::int8_t, 256> decode_table = make_decode_table();

}  // namespace

std::string base64_encode(ByteView data, const Base64Options& options) {
  const std::string_view alphabet = options.url_safe ? url_alphabet : standard_alphabet;
  std::string out;
  const std::size_t encoded = (data.size() + 2) / 3 * 4;
  out.reserve(encoded + (options.line_width > 0 ? (encoded / options.line_width + 1) * options.newline.size() : 0));

  std::size_t column = 0;
  const auto put = [&](char c) {
    if (options.line_width > 0 && column == options.line_width) {
      out.append(options.newline);
      column = 0;
    }
    out.push_back(c);
    ++column;
  };
  const auto at = [&](std::size_t i) { return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[i])); };

  std::size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    const std::uint32_t n = (at(i) << 16) | (at(i + 1) << 8) | at(i + 2);
    put(alphabet[(n >> 18) & 63]);
    put(alphabet[(n >> 12) & 63]);
    put(alphabet[(n >> 6) & 63]);
    put(alphabet[n & 63]);
  }
  const std::size_t rest = data.size() - i;
  if (rest == 1) {
    const std::uint32_t n = at(i) << 16;
    put(alphabet[(n >> 18) & 63]);
    put(alphabet[(n >> 12) & 63]);
    if (options.padding) {
      put('=');
      put('=');
    }
  } else if (rest == 2) {
    const std::uint32_t n = (at(i) << 16) | (at(i + 1) << 8);
    put(alphabet[(n >> 18) & 63]);
    put(alphabet[(n >> 12) & 63]);
    put(alphabet[(n >> 6) & 63]);
    if (options.padding) put('=');
  }
  return out;
}

Result<Bytes> base64_decode(std::string_view text) {
  Bytes out;
  out.reserve(text.size() / 4 * 3 + 3);
  std::uint32_t accumulator = 0;
  int bits = 0;
  std::size_t symbols = 0;
  std::size_t i = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (is_ascii_space(c)) continue;
    if (c == '=') break;
    const std::int8_t value = decode_table[static_cast<unsigned char>(c)];
    if (value < 0) {
      return fail(Status::Decode, "invalid Base64 character at offset {}", i);
    }
    accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    ++symbols;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::byte>((accumulator >> bits) & 0xFF));
    }
  }
  std::size_t padding = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '=') {
      ++padding;
    } else if (!is_ascii_space(c)) {
      return fail(Status::Decode, "unexpected data after Base64 padding at offset {}", i);
    }
  }
  const std::size_t tail = symbols % 4;
  if (tail == 1) return fail(Status::Decode, "truncated Base64 input");
  if (padding > 2 || (padding > 0 && (tail == 0 || tail + padding != 4))) {
    return fail(Status::Decode, "malformed Base64 padding");
  }
  return out;
}

}  // namespace cvt::util
