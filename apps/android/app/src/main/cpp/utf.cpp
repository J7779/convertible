// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "utf.hpp"

#include <cstdint>

namespace cvt_jni {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

void append_utf16(std::u16string& out, char32_t cp) {
  if (cp < 0x10000) {
    out.push_back(static_cast<char16_t>(cp));
  } else {
    cp -= 0x10000;
    out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
    out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
  }
}

void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

std::u16string utf8_to_utf16(std::string_view in) {
  std::u16string out;
  out.reserve(in.size());
  const auto* s = reinterpret_cast<const unsigned char*>(in.data());
  const std::size_t n = in.size();
  std::size_t i = 0;
  while (i < n) {
    const unsigned char b0 = s[i];
    if (b0 < 0x80) {
      out.push_back(static_cast<char16_t>(b0));
      ++i;
      continue;
    }
    std::size_t len = 0;
    char32_t cp = 0;
    char32_t min = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
      len = 2; cp = b0 & 0x1F; min = 0x80;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
      len = 3; cp = b0 & 0x0F; min = 0x800;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
      len = 4; cp = b0 & 0x07; min = 0x10000;
    } else {
      out.push_back(static_cast<char16_t>(kReplacement));
      ++i;
      continue;
    }
    std::size_t j = 1;
    for (; j < len && i + j < n; ++j) {
      const unsigned char b = s[i + j];
      if ((b & 0xC0) != 0x80) break;
      cp = (cp << 6) | (b & 0x3F);
    }
    if (j < len) {
      // Truncated or interrupted sequence: replace what was consumed, resync on the next byte.
      out.push_back(static_cast<char16_t>(kReplacement));
      i += j;
      continue;
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      out.push_back(static_cast<char16_t>(kReplacement));
    } else {
      append_utf16(out, cp);
    }
    i += len;
  }
  return out;
}

std::string utf16_to_utf8(std::u16string_view in) {
  std::string out;
  out.reserve(in.size() + in.size() / 2);
  const std::size_t n = in.size();
  for (std::size_t i = 0; i < n; ++i) {
    const char16_t u = in[i];
    if (u >= 0xD800 && u <= 0xDBFF) {
      if (i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
        const char32_t cp = 0x10000 + ((static_cast<char32_t>(u) - 0xD800) << 10) +
                            (static_cast<char32_t>(in[i + 1]) - 0xDC00);
        append_utf8(out, cp);
        ++i;
      } else {
        append_utf8(out, kReplacement);
      }
    } else if (u >= 0xDC00 && u <= 0xDFFF) {
      append_utf8(out, kReplacement);
    } else {
      append_utf8(out, u);
    }
  }
  return out;
}

}  // namespace cvt_jni
