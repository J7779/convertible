// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "base/text_util.hpp"

#include <cmath>
#include <cstdio>

#include "base/geometry.hpp"

namespace cvd {

// ---------------------------------------------------------------------------
// Colour helpers (declared in geometry.hpp)
// ---------------------------------------------------------------------------

namespace {
int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
}  // namespace

Color Color::hex(std::string_view text) {
  if (!text.empty() && text.front() == '#') text.remove_prefix(1);
  int v[8] = {};
  for (std::size_t i = 0; i < text.size() && i < 8; ++i) {
    v[i] = hex_digit(text[i]);
    if (v[i] < 0) return {1.0f, 0.0f, 1.0f, 1.0f};
  }
  if (text.size() == 3) return rgba8(v[0] * 17, v[1] * 17, v[2] * 17);
  if (text.size() == 6) return rgba8(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]);
  if (text.size() == 8) return rgba8(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5], v[6] * 16 + v[7]);
  return {1.0f, 0.0f, 1.0f, 1.0f};
}

std::uint32_t Color::premul_u32() const {
  const float al = clampf(a, 0.0f, 1.0f);
  const auto q = [](float v) { return static_cast<std::uint32_t>(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return (q(al) << 24) | (q(r * al) << 16) | (q(g * al) << 8) | q(b * al);
}

float Color::luminance() const {
  return 0.2126f * srgb_to_linear(r) + 0.7152f * srgb_to_linear(g) + 0.0722f * srgb_to_linear(b);
}

float contrast_ratio(Color a, Color b) {
  const float la = a.luminance();
  const float lb = b.luminance();
  const float hi = std::max(la, lb);
  const float lo = std::min(la, lb);
  return (hi + 0.05f) / (lo + 0.05f);
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

std::uint32_t utf8_next(std::string_view text, std::size_t& pos) {
  if (pos >= text.size()) return 0;
  const auto b0 = static_cast<unsigned char>(text[pos]);
  if (b0 < 0x80) {
    ++pos;
    return b0;
  }
  int extra = 0;
  std::uint32_t cp = 0;
  if ((b0 & 0xE0) == 0xC0) {
    extra = 1;
    cp = b0 & 0x1Fu;
  } else if ((b0 & 0xF0) == 0xE0) {
    extra = 2;
    cp = b0 & 0x0Fu;
  } else if ((b0 & 0xF8) == 0xF0) {
    extra = 3;
    cp = b0 & 0x07u;
  } else {
    ++pos;
    return 0xFFFD;
  }
  if (pos + static_cast<std::size_t>(extra) >= text.size()) {
    ++pos;
    return 0xFFFD;
  }
  for (int i = 1; i <= extra; ++i) {
    const auto b = static_cast<unsigned char>(text[pos + static_cast<std::size_t>(i)]);
    if ((b & 0xC0) != 0x80) {
      ++pos;
      return 0xFFFD;
    }
    cp = (cp << 6) | (b & 0x3Fu);
  }
  pos += static_cast<std::size_t>(extra) + 1;
  if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
      (cp >= 0xD800 && cp <= 0xDFFF)) {
    return 0xFFFD;
  }
  return cp;
}

std::size_t utf8_prev(std::string_view text, std::size_t pos) {
  if (pos == 0) return 0;
  if (pos > text.size()) pos = text.size();
  std::size_t p = pos - 1;
  int steps = 0;
  while (p > 0 && (static_cast<unsigned char>(text[p]) & 0xC0) == 0x80 && steps < 3) {
    --p;
    ++steps;
  }
  return p;
}

void utf8_append(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::vector<std::uint32_t> utf8_decode(std::string_view text) {
  std::vector<std::uint32_t> out;
  out.reserve(text.size());
  std::size_t pos = 0;
  while (pos < text.size()) out.push_back(utf8_next(text, pos));
  return out;
}

std::size_t utf8_length(std::string_view text) {
  std::size_t n = 0;
  std::size_t pos = 0;
  while (pos < text.size()) {
    utf8_next(text, pos);
    ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

std::string ascii_lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string ascii_upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

namespace {
char lower_char(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
}  // namespace

bool contains_ci(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (lower_char(haystack[i + j]) != lower_char(needle[j])) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) {
  if (prefix.size() > text.size()) return false;
  for (std::size_t j = 0; j < prefix.size(); ++j) {
    if (lower_char(text[j]) != lower_char(prefix[j])) return false;
  }
  return true;
}

std::string format_size(std::uint64_t bytes) {
  if (bytes == 1) return "1 byte";
  if (bytes < 1000) return std::to_string(bytes) + " bytes";
  const char* units[] = {"KB", "MB", "GB", "TB", "PB"};
  double value = static_cast<double>(bytes) / 1000.0;
  int unit = 0;
  while (value >= 999.95 && unit < 4) {
    value /= 1000.0;
    ++unit;
  }
  char buf[32];
  if (value < 9.995) {
    std::snprintf(buf, sizeof buf, "%.2f %s", value, units[unit]);
  } else if (value < 99.95) {
    std::snprintf(buf, sizeof buf, "%.1f %s", value, units[unit]);
  } else {
    std::snprintf(buf, sizeof buf, "%.0f %s", value, units[unit]);
  }
  return buf;
}

std::string format_size_change(std::uint64_t before, std::uint64_t after) {
  if (before == 0) return after == 0 ? "0%" : "new";
  const double change = (static_cast<double>(after) - static_cast<double>(before)) / static_cast<double>(before) * 100.0;
  const long long rounded = std::llround(change);
  if (rounded == 0) return "0%";
  char buf[48];
  if (rounded < 0) {
    std::snprintf(buf, sizeof buf, "\xE2\x88\x92%lld%%", -rounded);  // U+2212 minus sign
  } else {
    std::snprintf(buf, sizeof buf, "+%lld%%", rounded);
  }
  return buf;
}

std::string format_percent(double fraction) {
  const double f = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
  return std::to_string(static_cast<int>(std::floor(f * 100.0 + 1e-9))) + "%";
}

std::vector<std::string> split(std::string_view text, char sep) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (true) {
    const std::size_t p = text.find(sep, start);
    if (p == std::string_view::npos) {
      out.emplace_back(text.substr(start));
      break;
    }
    out.emplace_back(text.substr(start, p - start));
    start = p + 1;
  }
  return out;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

std::string path_filename(std::string_view path) {
  const std::size_t p = path.find_last_of("/\\");
  return std::string(p == std::string_view::npos ? path : path.substr(p + 1));
}

std::string path_stem(std::string_view filename) {
  const std::size_t p = filename.rfind('.');
  if (p == std::string_view::npos || p == 0) return std::string(filename);
  return std::string(filename.substr(0, p));
}

std::string path_extension(std::string_view filename) {
  const std::size_t p = filename.rfind('.');
  if (p == std::string_view::npos || p == 0 || p + 1 >= filename.size()) return {};
  return ascii_lower(filename.substr(p + 1));
}

std::string path_parent(std::string_view path) {
  const std::size_t p = path.find_last_of("/\\");
  if (p == std::string_view::npos) return {};
  if (p == 0) return std::string(path.substr(0, 1));
  return std::string(path.substr(0, p));
}

}  // namespace cvd
