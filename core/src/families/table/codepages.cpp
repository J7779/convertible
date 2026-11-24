// Convertible core - table family: single-byte code page transcoding.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "families/table/codepages.hpp"
#include "util/text.hpp"

namespace cvt::table {

std::string decode_code_page(ByteView data, const CodePageInfo& page, std::size_t* replaced) {
  std::string out;
  out.reserve(data.size());
  std::size_t bad = 0;
  for (const std::byte b : data) {
    const auto value = std::to_integer<std::uint8_t>(b);
    if (value < 0x80) {
      out.push_back(static_cast<char>(value));
      continue;
    }
    const char16_t cp = (*page.upper)[value - 0x80u];
    if (cp == 0xFFFD) ++bad;
    util::append_utf8(out, cp);
  }
  if (replaced != nullptr) *replaced = bad;
  return out;
}

std::string encode_code_page(std::string_view utf8, const CodePageInfo& page, std::size_t* replaced) {
  std::string out;
  out.reserve(utf8.size());
  std::size_t bad = 0;
  std::size_t i = 0;
  while (i < utf8.size()) {
    const util::Utf8Decoded d = util::decode_utf8(utf8, i);
    i += d.length == 0 ? 1 : d.length;
    if (d.ok && d.code_point < 0x80) {
      out.push_back(static_cast<char>(d.code_point));
      continue;
    }
    bool found = false;
    if (d.ok) {
      for (std::size_t k = 0; k < page.upper->size(); ++k) {
        if (static_cast<char32_t>((*page.upper)[k]) == d.code_point && d.code_point != 0xFFFD) {
          out.push_back(static_cast<char>(0x80 + k));
          found = true;
          break;
        }
      }
    }
    if (!found) {
      out.push_back('?');
      ++bad;
    }
  }
  if (replaced != nullptr) *replaced = bad;
  return out;
}

}  // namespace cvt::table
