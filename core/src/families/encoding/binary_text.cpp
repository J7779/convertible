// Convertible core - encoding family: binary-to-text transformations.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <format>

#include "families/encoding/encoding.hpp"
#include "util/base64.hpp"
#include "util/crc.hpp"
#include "util/hash.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

constexpr std::string_view lower_digits = "0123456789abcdef";
constexpr std::string_view upper_digits = "0123456789ABCDEF";

constexpr std::uint8_t byte_at(ByteView data, std::size_t i) noexcept { return std::to_integer<std::uint8_t>(data[i]); }

void put_hex(std::string& out, std::uint8_t value, bool uppercase) {
  const std::string_view digits = uppercase ? upper_digits : lower_digits;
  out.push_back(digits[value >> 4]);
  out.push_back(digits[value & 0x0F]);
}

constexpr bool is_unreserved(std::uint8_t c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
         c == '_' || c == '~';
}

constexpr char uu_char(std::uint32_t six_bits) noexcept {
  return six_bits == 0 ? '`' : static_cast<char>(six_bits + 32);
}

}  // namespace

// --------------------------------------------------------------------------- hex

std::string hex_text(ByteView data, const HexStyle& style) {
  std::string out;
  out.reserve(data.size() * (2 + style.separator.size() + style.prefix.size()) + data.size() / 16 + 1);
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (i > 0) {
      if (style.bytes_per_line > 0 && i % style.bytes_per_line == 0) {
        out.push_back('\n');
      } else {
        out.append(style.separator);
      }
    }
    out.append(style.prefix);
    put_hex(out, byte_at(data, i), style.uppercase);
  }
  if (style.bytes_per_line > 0 && !data.empty()) out.push_back('\n');
  return out;
}

Result<Bytes> parse_hex_text(std::string_view text) {
  std::string digits;
  digits.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == ',' || c == ':' || c == '-' || c == ';' || c == '%') continue;
    if ((c == '0' || c == '\\') && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
      ++i;
      continue;
    }
    digits.push_back(c);
  }
  return util::hex_decode(digits);
}

// --------------------------------------------------------------------------- hexdump

std::string hexdump_text(ByteView data, std::size_t bytes_per_line, bool uppercase) {
  bytes_per_line = std::clamp<std::size_t>(bytes_per_line, 1, 256);
  // Bytes are shown in groups of two, each group preceded by one space.
  const std::size_t hex_width = bytes_per_line * 2 + (bytes_per_line + 1) / 2;
  std::string out;
  out.reserve((data.size() / bytes_per_line + 1) * (12 + hex_width + bytes_per_line + 3));
  for (std::size_t offset = 0; offset < data.size(); offset += bytes_per_line) {
    const std::size_t count = std::min(bytes_per_line, data.size() - offset);
    out += std::format("{:08x}:", offset);
    const std::size_t line_start = out.size();
    for (std::size_t i = 0; i < count; ++i) {
      if (i % 2 == 0) out.push_back(' ');
      put_hex(out, byte_at(data, offset + i), uppercase);
    }
    out.append(hex_width - (out.size() - line_start), ' ');
    out.append("  ");
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint8_t c = byte_at(data, offset + i);
      out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
    }
    out.push_back('\n');
  }
  return out;
}

Result<Bytes> parse_hexdump(std::string_view text) {
  Bytes out;
  std::size_t line_number = 0;
  for (std::string_view line : util::split_lines(text)) {
    ++line_number;
    if (util::trim(line).empty()) continue;
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) return fail(Status::Decode, "hex dump line {} has no offset column", line_number);
    line.remove_prefix(colon + 1);
    if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    // The hex column ends at the first run of two spaces (the gap before the
    // ASCII column, or the padding of a short last line).
    if (const std::size_t gap = line.find("  "); gap != std::string_view::npos) line = line.substr(0, gap);
    auto bytes = util::hex_decode(line);
    if (!bytes) return fail(Status::Decode, "hex dump line {} is malformed", line_number);
    append(out, *bytes);
  }
  return out;
}

// --------------------------------------------------------------------------- data: URI

std::string data_uri(ByteView data, std::string_view mime, bool base64) {
  std::string out = "data:";
  out.append(mime);
  if (base64) {
    out.append(";base64,");
    out.append(util::base64_encode(data));
  } else {
    out.push_back(',');
    out.append(percent_encode(data, false));
  }
  return out;
}

Result<ParsedDataUri> parse_data_uri(std::string_view text) {
  text = util::trim(text);
  if (!util::istarts_with(text, "data:")) return fail(Status::Decode, "not a data: URI");
  text.remove_prefix(5);
  const std::size_t comma = text.find(',');
  if (comma == std::string_view::npos) return fail(Status::Decode, "data: URI has no ',' separator");
  std::string_view header = text.substr(0, comma);
  const std::string_view payload = text.substr(comma + 1);

  bool base64 = false;
  if (util::iends_with(header, ";base64")) {
    base64 = true;
    header.remove_suffix(7);
  }
  ParsedDataUri out;
  const std::string_view media_type = util::trim(header.substr(0, header.find(';')));
  out.mime = media_type.empty() ? std::string("text/plain") : util::to_lower(media_type);

  if (base64) {
    // Tolerate payloads that were percent-encoded on top of Base64.
    auto unescaped = percent_decode(payload, false);
    if (!unescaped) return std::unexpected(std::move(unescaped).error());
    auto bytes = util::base64_decode(as_chars(*unescaped));
    if (!bytes) return std::unexpected(std::move(bytes).error());
    out.bytes = std::move(*bytes);
  } else {
    auto bytes = percent_decode(payload, false);
    if (!bytes) return std::unexpected(std::move(bytes).error());
    out.bytes = std::move(*bytes);
  }
  return out;
}

// --------------------------------------------------------------------------- C source

std::string c_identifier(std::string_view text, bool avoid_reserved) {
  std::string out;
  out.reserve(text.size() + 1);
  for (const char c : text) {
    out.push_back(util::is_ascii_alpha(c) || util::is_ascii_digit(c) ? c : '_');
  }
  // Collapse nothing, trim nothing: predictable beats pretty. But an
  // identifier cannot be empty, all underscores, or start with a digit.
  if (out.find_first_not_of('_') == std::string::npos) return "data";
  if (util::is_ascii_digit(out.front())) out.insert(out.begin(), '_');
  // "__x" and "_X" are reserved for the C and C++ implementation.
  if (out.size() >= 2 && out[0] == '_' && (out[1] == '_' || (out[1] >= 'A' && out[1] <= 'Z'))) out.insert(out.begin(), 'v');
  if (avoid_reserved && is_reserved_identifier(out)) out.push_back('_');
  return out;
}

std::string c_header(ByteView data, std::string_view variable_name, std::size_t bytes_per_line,
                     std::string_view comment) {
  bytes_per_line = std::clamp<std::size_t>(bytes_per_line, 1, 256);
  const std::string name = c_identifier(variable_name);
  std::string out;
  out.reserve(data.size() * 6 + 128);
  if (!comment.empty()) {
    // Keep the comment well-formed whatever the file was called.
    out += "/* " + util::replace_all(util::sanitize_utf8(comment), "*/", "* /") + " */\n";
  }
  out += "unsigned char " + name + "[] = {";
  for (std::size_t i = 0; i < data.size(); ++i) {
    out.append(i % bytes_per_line == 0 ? "\n  " : " ");
    out.append("0x");
    put_hex(out, byte_at(data, i), false);
    if (i + 1 < data.size()) out.push_back(',');
  }
  out += "\n};\n";
  out += std::format("unsigned int {}_len = {};\n", name, data.size());
  return out;
}

// --------------------------------------------------------------------------- uuencode

std::string uuencode(ByteView data, std::string_view filename, const Progress* progress) {
  std::string name;
  for (const char c : filename) {
    if (static_cast<unsigned char>(c) > 0x20 && c != 0x7F) name.push_back(c);
  }
  if (name.empty()) name = "data.bin";

  std::string out = "begin 644 " + name + "\n";
  out.reserve(out.size() + data.size() / 45 * 62 + 80);
  for (std::size_t offset = 0; offset < data.size(); offset += 45) {
    if (progress != nullptr && offset % (45 * 23302) == 0 &&
        !progress->report(static_cast<double>(offset) / static_cast<double>(data.size()))) {
      return out;
    }
    const std::size_t count = std::min<std::size_t>(45, data.size() - offset);
    out.push_back(uu_char(static_cast<std::uint32_t>(count)));
    for (std::size_t i = 0; i < count; i += 3) {
      const auto at = [&](std::size_t k) -> std::uint32_t { return i + k < count ? byte_at(data, offset + i + k) : 0u; };
      const std::uint32_t n = (at(0) << 16) | (at(1) << 8) | at(2);
      out.push_back(uu_char((n >> 18) & 63));
      out.push_back(uu_char((n >> 12) & 63));
      out.push_back(uu_char((n >> 6) & 63));
      out.push_back(uu_char(n & 63));
    }
    out.push_back('\n');
  }
  out += "`\nend\n";
  return out;
}

Result<ParsedUu> uudecode(std::string_view text) {
  const std::vector<std::string_view> lines = util::split_lines(text);
  std::size_t index = 0;
  while (index < lines.size() && !lines[index].starts_with("begin ")) ++index;
  if (index == lines.size()) return fail(Status::Decode, "no uuencode 'begin' line found");

  ParsedUu out;
  // "begin <mode> <name>"
  const std::string_view header = lines[index].substr(6);
  if (const std::size_t space = header.find(' '); space != std::string_view::npos) {
    out.filename = util::sanitize_utf8(util::trim(header.substr(space + 1)));
  }
  ++index;

  const auto value = [](char c) -> std::uint32_t { return (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) - 32u) & 63u; };
  bool ended = false;
  for (; index < lines.size(); ++index) {
    const std::string_view line = lines[index];
    if (line == "end") {
      ended = true;
      break;
    }
    if (line.empty()) continue;
    if (static_cast<unsigned char>(line.front()) < 32 || static_cast<unsigned char>(line.front()) > 96) {
      return fail(Status::Decode, "malformed uuencode line {}", index + 1);
    }
    const std::size_t count = value(line.front());
    if (count == 0) continue;
    if (count > 45) return fail(Status::Decode, "malformed uuencode line {}", index + 1);
    const std::size_t needed = (count * 4 + 2) / 3;  // characters that carry data
    if (line.size() - 1 < needed) return fail(Status::Decode, "truncated uuencode line {}", index + 1);
    std::size_t produced = 0;
    for (std::size_t i = 1; produced < count; i += 4) {
      const auto at = [&](std::size_t k) -> std::uint32_t {
        if (i + k >= line.size()) return 0u;  // some encoders drop trailing padding
        const auto c = static_cast<unsigned char>(line[i + k]);
        return c < 32 || c > 96 ? 64u : value(line[i + k]);
      };
      const std::uint32_t a = at(0), b = at(1), c = at(2), d = at(3);
      if ((a | b | c | d) & 64u) return fail(Status::Decode, "invalid character in uuencode line {}", index + 1);
      const std::uint32_t n = (a << 18) | (b << 12) | (c << 6) | d;
      for (int shift = 16; shift >= 0 && produced < count; shift -= 8, ++produced) {
        out.bytes.push_back(static_cast<std::byte>((n >> shift) & 0xFF));
      }
    }
  }
  if (!ended) return fail(Status::Decode, "uuencode data has no 'end' line");
  return out;
}

// --------------------------------------------------------------------------- percent-encoding

std::string percent_encode(ByteView data, bool space_as_plus) {
  std::string out;
  out.reserve(data.size() * 3);
  for (std::size_t i = 0; i < data.size(); ++i) {
    const std::uint8_t c = byte_at(data, i);
    if (is_unreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else if (c == ' ' && space_as_plus) {
      out.push_back('+');
    } else {
      out.push_back('%');
      put_hex(out, c, true);
    }
  }
  return out;
}

Result<Bytes> percent_decode(std::string_view text, bool plus_as_space) {
  Bytes out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '%') {
      const int high = i + 1 < text.size() ? util::hex_digit_value(text[i + 1]) : -1;
      const int low = i + 2 < text.size() ? util::hex_digit_value(text[i + 2]) : -1;
      if (high < 0 || low < 0) return fail(Status::Decode, "invalid percent escape at offset {}", i);
      out.push_back(static_cast<std::byte>((high << 4) | low));
      i += 2;
    } else if (c == '+' && plus_as_space) {
      out.push_back(std::byte{' '});
    } else if (c == '\n' || c == '\r') {
      continue;  // line breaks are never data in a percent-encoded stream
    } else {
      out.push_back(static_cast<std::byte>(c));
    }
  }
  return out;
}

// --------------------------------------------------------------------------- binary digits

std::string binary_text(ByteView data, bool separate_bytes, std::size_t bytes_per_line) {
  std::string out;
  out.reserve(data.size() * 9 + 1);
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (i > 0) {
      if (bytes_per_line > 0 && i % bytes_per_line == 0) {
        out.push_back('\n');
      } else if (separate_bytes) {
        out.push_back(' ');
      }
    }
    const std::uint8_t value = byte_at(data, i);
    for (int bit = 7; bit >= 0; --bit) out.push_back(((value >> bit) & 1) != 0 ? '1' : '0');
  }
  if (bytes_per_line > 0 && !data.empty()) out.push_back('\n');
  return out;
}

Result<Bytes> parse_binary_text(std::string_view text) {
  Bytes out;
  out.reserve(text.size() / 8);
  std::uint32_t value = 0;
  int bits = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (util::is_ascii_space(c)) continue;
    if (c != '0' && c != '1') return fail(Status::Decode, "invalid binary digit at offset {}", i);
    value = (value << 1) | static_cast<std::uint32_t>(c - '0');
    if (++bits == 8) {
      out.push_back(static_cast<std::byte>(value));
      value = 0;
      bits = 0;
    }
  }
  if (bits != 0) return fail(Status::Decode, "the number of binary digits is not a multiple of 8");
  return out;
}

// --------------------------------------------------------------------------- checksums

std::string checksum_report(ByteView data, std::string_view filename, bool uppercase) {
  const auto style = [uppercase](std::string text) { return uppercase ? util::to_upper(text) : text; };
  std::string out;
  if (!filename.empty()) out += "File: " + util::sanitize_utf8(filename) + "\n";
  out += std::format("Size: {} bytes\n", data.size());
  out += "CRC-32: " + style(std::format("{:08x}", util::crc32(data))) + "\n";
  out += "Adler-32: " + style(std::format("{:08x}", util::adler32(data))) + "\n";
  out += "MD5: " + style(util::md5_hex(data)) + "\n";
  out += "SHA-1: " + style(util::sha1_hex(data)) + "\n";
  out += "SHA-256: " + style(util::sha256_hex(data)) + "\n";
  return out;
}

}  // namespace cvt::encoding
