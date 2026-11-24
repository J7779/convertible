// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <system_error>

namespace cvt::util {

namespace {

// Windows-1252 code points for bytes 0x80..0x9F. The five bytes the code page
// leaves undefined map to the matching C1 control, as browsers do.
constexpr std::array<char16_t, 32> cp1252_high = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

constexpr std::uint8_t u8_at(ByteView data, std::size_t i) noexcept {
  return std::to_integer<std::uint8_t>(data[i]);
}

template <class String>
std::string join_impl(const std::vector<String>& parts, std::string_view separator) {
  std::string out;
  std::size_t total = 0;
  for (const auto& part : parts) total += part.size() + separator.size();
  out.reserve(total);
  bool first = true;
  for (const auto& part : parts) {
    if (!first) out.append(separator);
    out.append(part);
    first = false;
  }
  return out;
}

void put_u16(Bytes& out, char16_t unit, bool little) {
  const auto lo = static_cast<std::byte>(unit & 0xFF);
  const auto hi = static_cast<std::byte>((unit >> 8) & 0xFF);
  if (little) {
    out.push_back(lo);
    out.push_back(hi);
  } else {
    out.push_back(hi);
    out.push_back(lo);
  }
}

void put_u32(Bytes& out, char32_t cp, bool little) {
  for (int i = 0; i < 4; ++i) {
    const int shift = little ? 8 * i : 8 * (3 - i);
    out.push_back(static_cast<std::byte>((cp >> shift) & 0xFF));
  }
}

}  // namespace

std::string to_lower(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), ascii_lower);
  return out;
}

std::string to_upper(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), ascii_upper);
  return out;
}

std::vector<std::string_view> split(std::string_view text, char separator, bool keep_empty) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t end = text.find(separator, start);
    const std::string_view piece = text.substr(start, end == std::string_view::npos ? end : end - start);
    if (keep_empty || !piece.empty()) parts.push_back(piece);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return parts;
}

std::vector<std::string_view> split_lines(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '\n' || c == '\r') {
      lines.push_back(text.substr(start, i - start));
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
      start = i + 1;
    }
    ++i;
  }
  if (start < text.size()) lines.push_back(text.substr(start));
  return lines;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  return join_impl(parts, separator);
}

std::string join(const std::vector<std::string_view>& parts, std::string_view separator) {
  return join_impl(parts, separator);
}

std::string replace_all(std::string_view text, std::string_view from, std::string_view to) {
  if (from.empty()) return std::string(text);
  std::string out;
  out.reserve(text.size());
  std::size_t start = 0;
  while (true) {
    const std::size_t hit = text.find(from, start);
    if (hit == std::string_view::npos) break;
    out.append(text.substr(start, hit - start));
    out.append(to);
    start = hit + from.size();
  }
  out.append(text.substr(start));
  return out;
}

std::optional<std::int64_t> parse_int(std::string_view text, int base) noexcept {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) return std::nullopt;
  }
  if (text.empty() || base < 2 || base > 36) return std::nullopt;
  std::int64_t value = 0;
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value, base);
  if (ec != std::errc{} || ptr != last) return std::nullopt;
  return value;
}

namespace {

// Validates the syntax ourselves, then lets strtod do the conversion using
// whatever decimal point the C locale currently has. Used where the standard
// library has no floating-point from_chars, and for out-of-range inputs.
std::optional<double> parse_double_portable(std::string_view text) noexcept {
  std::string buffer;
  buffer.reserve(text.size() + 4);
  std::size_t i = 0;
  if (text[i] == '-') buffer.push_back(text[i++]);
  const std::string_view rest = text.substr(i);
  if (iequals(rest, "inf") || iequals(rest, "infinity")) {
    return buffer.empty() ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
  }
  if (iequals(rest, "nan")) return std::numeric_limits<double>::quiet_NaN();
  bool digits = false;
  while (i < text.size() && is_ascii_digit(text[i])) {
    buffer.push_back(text[i++]);
    digits = true;
  }
  if (i < text.size() && text[i] == '.') {
    const std::lconv* conv = std::localeconv();
    buffer.append(conv != nullptr && conv->decimal_point != nullptr && *conv->decimal_point != '\0'
                      ? conv->decimal_point
                      : ".");
    ++i;
    while (i < text.size() && is_ascii_digit(text[i])) {
      buffer.push_back(text[i++]);
      digits = true;
    }
  }
  if (!digits) return std::nullopt;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    buffer.push_back(text[i++]);
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) buffer.push_back(text[i++]);
    bool exponent_digits = false;
    while (i < text.size() && is_ascii_digit(text[i])) {
      buffer.push_back(text[i++]);
      exponent_digits = true;
    }
    if (!exponent_digits) return std::nullopt;
  }
  if (i != text.size()) return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(buffer.c_str(), &end);  // saturates on overflow/underflow
  if (end != buffer.c_str() + buffer.size()) return std::nullopt;
  return value;
}

}  // namespace

std::optional<double> parse_double(std::string_view text) noexcept {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) return std::nullopt;
  }
  if (text.empty()) return std::nullopt;

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && !defined(_LIBCPP_VERSION)
  double value = 0.0;
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec == std::errc::result_out_of_range) return parse_double_portable(text);
  if (ec != std::errc{} || ptr != last) return std::nullopt;
  return value;
#else
  // libc++ only gained floating-point from_chars recently, and not for the
  // deployment targets we support.
  return parse_double_portable(text);
#endif
}

std::string format_double(double value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
  std::array<char, 64> buffer{};
  const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (ec != std::errc{}) return "0";
  return std::string(buffer.data(), ptr);
}

std::string sanitize_utf8(std::string_view text, std::size_t* replaced) {
  std::size_t count = 0;
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const Utf8Decoded d = decode_utf8(text, i);
    if (d.ok) {
      out.append(text.substr(i, d.length));
    } else {
      append_utf8(out, replacement_character);
      ++count;
    }
    i += d.length;
  }
  if (replaced != nullptr) *replaced = count;
  return out;
}

std::u32string utf8_to_utf32(std::string_view text) {
  std::u32string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const Utf8Decoded d = decode_utf8(text, i);
    out.push_back(d.code_point);
    i += d.length;
  }
  return out;
}

std::string utf32_to_utf8(std::u32string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char32_t cp : text) append_utf8(out, cp);
  return out;
}

std::u16string utf8_to_utf16(std::string_view text) {
  std::u16string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const Utf8Decoded d = decode_utf8(text, i);
    const char32_t cp = d.code_point;
    if (cp < 0x10000) {
      out.push_back(static_cast<char16_t>(cp));
    } else {
      const char32_t v = cp - 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
    }
    i += d.length;
  }
  return out;
}

std::string utf16_to_utf8(std::u16string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char16_t unit = text[i];
    if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
      const char32_t cp = 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) +
                          (static_cast<char32_t>(text[i + 1]) - 0xDC00);
      append_utf8(out, cp);
      ++i;
    } else {
      append_utf8(out, unit);  // lone surrogates become U+FFFD inside append_utf8
    }
  }
  return out;
}

std::wstring utf8_to_wide(std::string_view text) {
  std::wstring out;
  if constexpr (sizeof(wchar_t) == 2) {
    const std::u16string wide = utf8_to_utf16(text);
    out.reserve(wide.size());
    for (const char16_t unit : wide) out.push_back(static_cast<wchar_t>(unit));
  } else {
    const std::u32string wide = utf8_to_utf32(text);
    out.reserve(wide.size());
    for (const char32_t cp : wide) out.push_back(static_cast<wchar_t>(cp));
  }
  return out;
}

std::string wide_to_utf8(std::wstring_view text) {
  if constexpr (sizeof(wchar_t) == 2) {
    std::u16string units;
    units.reserve(text.size());
    for (const wchar_t unit : text) units.push_back(static_cast<char16_t>(unit));
    return utf16_to_utf8(units);
  } else {
    std::u32string units;
    units.reserve(text.size());
    for (const wchar_t unit : text) units.push_back(static_cast<char32_t>(unit));
    return utf32_to_utf8(units);
  }
}

std::size_t utf8_length(std::string_view text) noexcept {
  std::size_t count = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    i += decode_utf8(text, i).length;
    ++count;
  }
  return count;
}

std::string_view encoding_name(TextEncoding encoding) noexcept {
  switch (encoding) {
    case TextEncoding::Utf8: return "utf-8";
    case TextEncoding::Utf16LE: return "utf-16le";
    case TextEncoding::Utf16BE: return "utf-16be";
    case TextEncoding::Utf32LE: return "utf-32le";
    case TextEncoding::Utf32BE: return "utf-32be";
    case TextEncoding::Latin1: return "latin1";
    case TextEncoding::Windows1252: return "windows-1252";
  }
  return "utf-8";
}

std::optional<TextEncoding> parse_encoding(std::string_view label) noexcept {
  // Compare ignoring case and the separators people sprinkle into labels.
  std::array<char, 24> buffer{};
  std::size_t n = 0;
  for (const char c : trim(label)) {
    if (c == '-' || c == '_' || c == ' ') continue;
    if (n == buffer.size()) return std::nullopt;
    buffer[n++] = ascii_lower(c);
  }
  const std::string_view key(buffer.data(), n);
  struct Alias {
    std::string_view name;
    TextEncoding encoding;
  };
  static constexpr std::array aliases = {
      Alias{"utf8", TextEncoding::Utf8},          Alias{"utf8bom", TextEncoding::Utf8},
      Alias{"utf16", TextEncoding::Utf16LE},      Alias{"utf16le", TextEncoding::Utf16LE},
      Alias{"ucs2", TextEncoding::Utf16LE},       Alias{"ucs2le", TextEncoding::Utf16LE},
      Alias{"unicode", TextEncoding::Utf16LE},    Alias{"utf16be", TextEncoding::Utf16BE},
      Alias{"ucs2be", TextEncoding::Utf16BE},     Alias{"utf32", TextEncoding::Utf32LE},
      Alias{"utf32le", TextEncoding::Utf32LE},    Alias{"ucs4", TextEncoding::Utf32LE},
      Alias{"utf32be", TextEncoding::Utf32BE},    Alias{"latin1", TextEncoding::Latin1},
      Alias{"iso88591", TextEncoding::Latin1},    Alias{"l1", TextEncoding::Latin1},
      Alias{"windows1252", TextEncoding::Windows1252}, Alias{"cp1252", TextEncoding::Windows1252},
      Alias{"win1252", TextEncoding::Windows1252},     Alias{"ansi", TextEncoding::Windows1252},
      Alias{"ascii", TextEncoding::Utf8},         Alias{"usascii", TextEncoding::Utf8},
  };
  for (const Alias& alias : aliases) {
    if (alias.name == key) return alias.encoding;
  }
  return std::nullopt;
}

std::optional<Bom> detect_bom(ByteView data) noexcept {
  const std::string_view text = as_chars(data);
  using namespace std::string_view_literals;
  // UTF-32LE must be tested before UTF-16LE: its mark starts with FF FE.
  if (text.starts_with("\xFF\xFE\x00\x00"sv)) return Bom{TextEncoding::Utf32LE, 4};
  if (text.starts_with("\x00\x00\xFE\xFF"sv)) return Bom{TextEncoding::Utf32BE, 4};
  if (text.starts_with("\xEF\xBB\xBF"sv)) return Bom{TextEncoding::Utf8, 3};
  if (text.starts_with("\xFF\xFE"sv)) return Bom{TextEncoding::Utf16LE, 2};
  if (text.starts_with("\xFE\xFF"sv)) return Bom{TextEncoding::Utf16BE, 2};
  return std::nullopt;
}

std::string_view bom_bytes(TextEncoding encoding) noexcept {
  using namespace std::string_view_literals;
  switch (encoding) {
    case TextEncoding::Utf8: return "\xEF\xBB\xBF"sv;
    case TextEncoding::Utf16LE: return "\xFF\xFE"sv;
    case TextEncoding::Utf16BE: return "\xFE\xFF"sv;
    case TextEncoding::Utf32LE: return "\xFF\xFE\x00\x00"sv;
    case TextEncoding::Utf32BE: return "\x00\x00\xFE\xFF"sv;
    case TextEncoding::Latin1:
    case TextEncoding::Windows1252: return {};
  }
  return {};
}

EncodingGuess guess_encoding(ByteView data) noexcept {
  if (const auto bom = detect_bom(data)) return {bom->encoding, bom->length, true};
  if (data.empty()) return {TextEncoding::Utf8, 0, true};

  // BOM-less UTF-16/32 text is dominated by NUL bytes in fixed positions.
  const std::size_t sample = std::min<std::size_t>(data.size(), 4096);
  std::array<std::size_t, 4> zeros{};
  for (std::size_t i = 0; i < sample; ++i) {
    if (data[i] == std::byte{0}) ++zeros[i % 4];
  }
  const std::size_t total_zeros = zeros[0] + zeros[1] + zeros[2] + zeros[3];
  if (total_zeros > 0 && data.size() >= 4) {
    const std::size_t quarter = sample / 4;
    const auto mostly = [&](std::size_t n) { return quarter > 0 && n * 10 >= quarter * 9; };
    const auto rarely = [&](std::size_t n) { return n * 10 <= quarter; };
    if (data.size() % 4 == 0) {
      if (mostly(zeros[1]) && mostly(zeros[2]) && mostly(zeros[3]) && rarely(zeros[0])) {
        return {TextEncoding::Utf32LE, 0, true};
      }
      if (mostly(zeros[0]) && mostly(zeros[1]) && mostly(zeros[2]) && rarely(zeros[3])) {
        return {TextEncoding::Utf32BE, 0, true};
      }
    }
    if (data.size() % 2 == 0) {
      const std::size_t even = zeros[0] + zeros[2];
      const std::size_t odd = zeros[1] + zeros[3];
      const std::size_t half = sample / 2;
      if (odd * 10 >= half * 6 && even * 10 <= half) return {TextEncoding::Utf16LE, 0, true};
      if (even * 10 >= half * 6 && odd * 10 <= half) return {TextEncoding::Utf16BE, 0, true};
    }
  }
  if (is_valid_utf8(as_chars(data))) return {TextEncoding::Utf8, 0, true};
  return {TextEncoding::Windows1252, 0, false};
}

DecodedText decode_text(ByteView data, TextEncoding encoding) {
  DecodedText out;
  // Look for this encoding's own mark only: "FF FE 00 00" is a UTF-32LE mark,
  // but also a UTF-16LE mark followed by U+0000.
  if (const std::string_view mark = bom_bytes(encoding); !mark.empty() && as_chars(data).starts_with(mark)) {
    data = data.subspan(mark.size());
  }

  switch (encoding) {
    case TextEncoding::Utf8: {
      out.text = sanitize_utf8(as_chars(data), &out.replaced);
      break;
    }
    case TextEncoding::Latin1:
    case TextEncoding::Windows1252: {
      out.text.reserve(data.size() + data.size() / 8);
      for (std::size_t i = 0; i < data.size(); ++i) {
        const std::uint8_t b = u8_at(data, i);
        if (encoding == TextEncoding::Windows1252 && b >= 0x80 && b <= 0x9F) {
          append_utf8(out.text, cp1252_high[b - 0x80]);
        } else {
          append_utf8(out.text, b);
        }
      }
      break;
    }
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE: {
      const bool little = encoding == TextEncoding::Utf16LE;
      const auto unit_at = [&](std::size_t i) {
        const auto a = static_cast<char32_t>(u8_at(data, i));
        const auto b = static_cast<char32_t>(u8_at(data, i + 1));
        return little ? (a | (b << 8)) : ((a << 8) | b);
      };
      out.text.reserve(data.size());
      std::size_t i = 0;
      while (i + 1 < data.size()) {
        const char32_t unit = unit_at(i);
        i += 2;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
          if (i + 1 < data.size()) {
            const char32_t low = unit_at(i);
            if (low >= 0xDC00 && low <= 0xDFFF) {
              append_utf8(out.text, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
              i += 2;
              continue;
            }
          }
          append_utf8(out.text, replacement_character);
          ++out.replaced;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
          append_utf8(out.text, replacement_character);
          ++out.replaced;
        } else {
          append_utf8(out.text, unit);
        }
      }
      if (i < data.size()) {  // dangling odd byte
        append_utf8(out.text, replacement_character);
        ++out.replaced;
      }
      break;
    }
    case TextEncoding::Utf32LE:
    case TextEncoding::Utf32BE: {
      const bool little = encoding == TextEncoding::Utf32LE;
      out.text.reserve(data.size() / 2);
      std::size_t i = 0;
      while (i + 3 < data.size()) {
        char32_t cp = 0;
        for (std::size_t k = 0; k < 4; ++k) {
          const auto b = static_cast<char32_t>(u8_at(data, i + k));
          cp |= b << (little ? 8 * k : 8 * (3 - k));
        }
        i += 4;
        if (!is_scalar_value(cp)) ++out.replaced;
        append_utf8(out.text, cp);
      }
      if (i < data.size()) {
        append_utf8(out.text, replacement_character);
        ++out.replaced;
      }
      break;
    }
  }
  return out;
}

EncodedText encode_text(std::string_view utf8, TextEncoding encoding, bool with_bom) {
  EncodedText out;
  if (with_bom) append(out.bytes, bom_bytes(encoding));
  out.bytes.reserve(out.bytes.size() + utf8.size() * (encoding == TextEncoding::Utf8 ? 1 : 2));

  std::size_t i = 0;
  while (i < utf8.size()) {
    const Utf8Decoded d = decode_utf8(utf8, i);
    i += d.length;
    char32_t cp = d.code_point;
    if (!d.ok) ++out.replaced;

    switch (encoding) {
      case TextEncoding::Utf8: {
        std::string tmp;
        append_utf8(tmp, cp);
        append(out.bytes, tmp);
        break;
      }
      case TextEncoding::Utf16LE:
      case TextEncoding::Utf16BE: {
        const bool little = encoding == TextEncoding::Utf16LE;
        if (cp < 0x10000) {
          put_u16(out.bytes, static_cast<char16_t>(cp), little);
        } else {
          const char32_t v = cp - 0x10000;
          put_u16(out.bytes, static_cast<char16_t>(0xD800 + (v >> 10)), little);
          put_u16(out.bytes, static_cast<char16_t>(0xDC00 + (v & 0x3FF)), little);
        }
        break;
      }
      case TextEncoding::Utf32LE:
      case TextEncoding::Utf32BE:
        put_u32(out.bytes, cp, encoding == TextEncoding::Utf32LE);
        break;
      case TextEncoding::Latin1:
        if (cp > 0xFF) {
          cp = U'?';
          if (d.ok) ++out.replaced;
        }
        out.bytes.push_back(static_cast<std::byte>(cp));
        break;
      case TextEncoding::Windows1252: {
        std::optional<std::uint8_t> byte;
        if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
          byte = static_cast<std::uint8_t>(cp);
        } else {
          for (std::size_t k = 0; k < cp1252_high.size(); ++k) {
            if (static_cast<char32_t>(cp1252_high[k]) == cp) {
              byte = static_cast<std::uint8_t>(0x80 + k);
              break;
            }
          }
        }
        if (!byte) {
          byte = static_cast<std::uint8_t>('?');
          if (d.ok) ++out.replaced;
        }
        out.bytes.push_back(static_cast<std::byte>(*byte));
        break;
      }
    }
  }
  return out;
}

bool looks_like_text(ByteView sample) noexcept {
  if (sample.empty()) return false;
  if (const auto bom = detect_bom(sample)) {
    if (bom->encoding != TextEncoding::Utf8) return sample.size() > bom->length;
    sample = sample.subspan(bom->length);
    if (sample.empty()) return true;
  }
  const std::size_t limit = std::min<std::size_t>(sample.size(), 64 * 1024);
  std::string_view text = as_chars(sample.first(limit));
  std::size_t controls = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    const Utf8Decoded d = decode_utf8(text, i);
    if (!d.ok) {
      // A multi-byte sequence cut off by the sampling window is fine.
      if (limit < sample.size() && i + 4 > text.size()) break;
      return false;
    }
    const char32_t cp = d.code_point;
    if (cp == 0) return false;
    if (cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r' && cp != '\f' && cp != 0x1B) ++controls;
    i += d.length;
  }
  // Tolerate the odd control character, but not a file full of them.
  return controls * 50 <= text.size();
}

std::string convert_line_endings(std::string_view text, LineEnding ending) {
  const std::string_view eol = ending == LineEnding::Lf ? "\n" : ending == LineEnding::CrLf ? "\r\n" : "\r";
  std::string out;
  out.reserve(text.size() + text.size() / 16);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
      out.append(eol);
    } else if (c == '\n') {
      out.append(eol);
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::optional<LineEnding> detect_line_ending(std::string_view text) noexcept {
  std::size_t lf = 0;
  std::size_t crlf = 0;
  std::size_t cr = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++crlf;
        ++i;
      } else {
        ++cr;
      }
    } else if (text[i] == '\n') {
      ++lf;
    }
  }
  if (lf + crlf + cr == 0) return std::nullopt;
  if (crlf >= lf && crlf >= cr) return LineEnding::CrLf;
  if (lf >= cr) return LineEnding::Lf;
  return LineEnding::Cr;
}

}  // namespace cvt::util
