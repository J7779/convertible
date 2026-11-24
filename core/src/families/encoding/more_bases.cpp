// Convertible core - encoding family: further binary-to-text encodings.
//
//   base16        RFC 4648 Base16 (upper-case hex, strict)
//   base45        RFC 9285 (QR codes, EU digital certificates)
//   base62        digits and both letter cases (big-number, up to 64 KiB)
//   base64-mime   RFC 2045 Base64: 76 columns, CRLF line breaks
//   pem           Base64 armour with a BEGIN/END label of your choice
//   base91        basE91
//   base92        Base92
//   base122       Base122 (UTF-8 output, for embedding in HTML/JS)
//   bech32        Bech32 / Bech32m (BIP 173/350) data part with a chosen HRP
//   base58check   Base58 with a double-SHA-256 checksum
//   xxe           xxencode
//   hqx           BinHex 4.0
//   mime-words    RFC 2047 encoded words (text transform)
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/encoding/bases.hpp"
#include "families/encoding/detail.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/base64.hpp"
#include "util/hash.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

using detail::byte_at;
using detail::byte_of;
using detail::DigitTable;

// ---------------------------------------------------------------------------- helpers

std::string_view newline_of(const Options& options) {
  return options.get_enum("line_ending", "lf", {"lf", "crlf"}) == "crlf" ? std::string_view("\r\n") : std::string_view("\n");
}

std::string wrap(std::string_view text, std::size_t width, std::string_view newline) {
  if (width == 0) return std::string(text);
  std::string out;
  out.reserve(text.size() + text.size() / width * newline.size() + newline.size());
  for (std::size_t at = 0; at < text.size(); at += width) {
    out.append(text.substr(at, width));
    out.append(newline);
  }
  return out;
}

constexpr std::string_view base45_alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
constexpr DigitTable base45_table = detail::make_digit_table(base45_alphabet, false);

constexpr std::string_view base91_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&()*+,./:;<=>?@[]^_`{|}~\"";
constexpr DigitTable base91_table = detail::make_digit_table(base91_alphabet, false);

constexpr std::string_view bech32_charset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
constexpr DigitTable bech32_table = detail::make_digit_table(bech32_charset, true);

constexpr std::string_view xx_alphabet = "+-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr DigitTable xx_table = detail::make_digit_table(xx_alphabet, false);

constexpr std::string_view binhex_alphabet = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
constexpr DigitTable binhex_table = detail::make_digit_table(binhex_alphabet, false);
static_assert(base45_alphabet.size() == 45 && base91_alphabet.size() == 91 && bech32_charset.size() == 32 &&
              xx_alphabet.size() == 64 && binhex_alphabet.size() == 64);

// Base92: value 0 is '!', 1..61 are '#'..'_', 62..90 are 'a'..'}'.
constexpr char base92_char(std::uint32_t value) noexcept {
  if (value == 0) return '!';
  if (value <= 61) return static_cast<char>('#' + value - 1);
  return static_cast<char>('a' + value - 62);
}
constexpr int base92_value(char c) noexcept {
  if (c == '!') return 0;
  if (c >= '#' && c <= '_') return c - '#' + 1;
  if (c >= 'a' && c <= '}') return c - 'a' + 62;
  return -1;
}

std::uint16_t crc16_xmodem(ByteView data, std::uint16_t crc = 0) {
  for (const std::byte b : data) {
    crc = static_cast<std::uint16_t>(crc ^ (std::to_integer<std::uint16_t>(b) << 8));
    for (int k = 0; k < 8; ++k) crc = static_cast<std::uint16_t>((crc & 0x8000u) ? (crc << 1) ^ 0x1021u : crc << 1);
  }
  return crc;
}

}  // namespace

// ---------------------------------------------------------------------------- Base16

std::string base16_encode(ByteView data) {
  std::string out;
  out.reserve(data.size() * 2);
  for (const std::byte b : data) detail::append_hex_byte(out, std::to_integer<std::uint8_t>(b), true);
  return out;
}

Result<Bytes> base16_decode(std::string_view text) {
  Bytes out;
  out.reserve(text.size() / 2);
  int high = -1;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (util::is_ascii_space(text[i])) continue;
    const int digit = util::hex_digit_value(text[i]);
    if (digit < 0) return fail(Status::Decode, "invalid Base16 character at offset {}", i);
    if (high < 0) {
      high = digit;
    } else {
      out.push_back(static_cast<std::byte>(high * 16 + digit));
      high = -1;
    }
  }
  if (high >= 0) return fail(Status::Decode, "Base16 text has an odd number of digits");
  return out;
}

// ---------------------------------------------------------------------------- Base45

std::string base45_encode(ByteView data) {
  std::string out;
  out.reserve(data.size() / 2 * 3 + 2);
  std::size_t i = 0;
  for (; i + 1 < data.size(); i += 2) {
    std::uint32_t n = byte_at(data, i) * 256u + byte_at(data, i + 1);
    out.push_back(base45_alphabet[n % 45]);
    n /= 45;
    out.push_back(base45_alphabet[n % 45]);
    out.push_back(base45_alphabet[n / 45]);
  }
  if (i < data.size()) {
    const std::uint32_t n = byte_at(data, i);
    out.push_back(base45_alphabet[n % 45]);
    out.push_back(base45_alphabet[n / 45]);
  }
  return out;
}

Result<Bytes> base45_decode(std::string_view text) {
  // Spaces are part of the alphabet; only line breaks are skipped.
  std::vector<int> values;
  values.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\n' || c == '\r') continue;
    const int v = base45_table[byte_of(c)];
    if (v < 0) return fail(Status::Decode, "invalid Base45 character at offset {}", i);
    values.push_back(v);
  }
  if (values.size() % 3 == 1) return fail(Status::Decode, "Base45 text has a dangling character");
  Bytes out;
  out.reserve(values.size() / 3 * 2 + 1);
  std::size_t i = 0;
  for (; i + 2 < values.size(); i += 3) {
    const std::uint32_t n = static_cast<std::uint32_t>(values[i] + values[i + 1] * 45 + values[i + 2] * 2025);
    if (n > 0xFFFF) return fail(Status::Decode, "Base45 group at character {} is out of range", i);
    out.push_back(static_cast<std::byte>(n >> 8));
    out.push_back(static_cast<std::byte>(n & 0xFF));
  }
  if (i < values.size()) {
    const std::uint32_t n = static_cast<std::uint32_t>(values[i] + values[i + 1] * 45);
    if (n > 0xFF) return fail(Status::Decode, "Base45 final pair is out of range");
    out.push_back(static_cast<std::byte>(n));
  }
  return out;
}

// ---------------------------------------------------------------------------- basE91

std::string base91_encode(ByteView data) {
  std::string out;
  out.reserve(data.size() * 16 / 13 + 2);
  std::uint32_t bits = 0;
  unsigned count = 0;
  for (const std::byte b : data) {
    bits |= std::to_integer<std::uint32_t>(b) << count;
    count += 8;
    if (count > 13) {
      std::uint32_t value = bits & 8191u;
      if (value > 88) {
        bits >>= 13;
        count -= 13;
      } else {
        value = bits & 16383u;
        bits >>= 14;
        count -= 14;
      }
      out.push_back(base91_alphabet[value % 91]);
      out.push_back(base91_alphabet[value / 91]);
    }
  }
  if (count > 0) {
    out.push_back(base91_alphabet[bits % 91]);
    if (count > 7 || bits > 90) out.push_back(base91_alphabet[bits / 91]);
  }
  return out;
}

Bytes base91_decode(std::string_view text) {
  Bytes out;
  out.reserve(text.size() * 14 / 16 + 1);
  int value = -1;
  std::uint32_t bits = 0;
  unsigned count = 0;
  for (const char c : text) {
    const int digit = base91_table[byte_of(c)];
    if (digit < 0) continue;
    if (value < 0) {
      value = digit;
      continue;
    }
    value += digit * 91;
    bits |= static_cast<std::uint32_t>(value) << count;
    count += (value & 8191) > 88 ? 13 : 14;
    do {
      out.push_back(static_cast<std::byte>(bits & 0xFF));
      bits >>= 8;
      count -= 8;
    } while (count > 7);
    value = -1;
  }
  if (value >= 0) out.push_back(static_cast<std::byte>((bits | static_cast<std::uint32_t>(value) << count) & 0xFF));
  return out;
}

// ---------------------------------------------------------------------------- Base92

std::string base92_encode(ByteView data) {
  if (data.empty()) return "~";
  std::string out;
  out.reserve(data.size() * 16 / 13 + 2);
  std::uint32_t bits = 0;  // pending bits, most significant first
  unsigned count = 0;
  for (const std::byte b : data) {
    bits = (bits << 8) | std::to_integer<std::uint32_t>(b);
    count += 8;
    if (count >= 13) {
      const std::uint32_t value = (bits >> (count - 13)) & 8191u;
      count -= 13;
      bits &= (1u << count) - 1;
      out.push_back(base92_char(value / 91));
      out.push_back(base92_char(value % 91));
    }
  }
  if (count > 0) {
    if (count < 7) {
      out.push_back(base92_char(bits << (6 - count)));
    } else {
      const std::uint32_t value = bits << (13 - count);
      out.push_back(base92_char(value / 91));
      out.push_back(base92_char(value % 91));
    }
  }
  return out;
}

Result<Bytes> base92_decode(std::string_view text) {
  text = util::trim(text);
  Bytes out;
  if (text == "~") return out;
  std::uint32_t bits = 0;
  unsigned count = 0;
  const auto push = [&](std::uint32_t value, unsigned width) {
    bits = (bits << width) | value;
    count += width;
    while (count >= 8) {
      out.push_back(static_cast<std::byte>((bits >> (count - 8)) & 0xFF));
      count -= 8;
      bits &= (1u << count) - 1;
    }
  };
  std::size_t i = 0;
  for (; i + 1 < text.size(); i += 2) {
    const int high = base92_value(text[i]);
    const int low = base92_value(text[i + 1]);
    if (high < 0 || low < 0) return fail(Status::Decode, "invalid Base92 character near offset {}", i);
    const auto value = static_cast<std::uint32_t>(high * 91 + low);
    if (value > 8191) return fail(Status::Decode, "Base92 pair at offset {} is out of range", i);
    push(value, 13);
  }
  if (i < text.size()) {
    const int last = base92_value(text[i]);
    if (last < 0 || last > 63) return fail(Status::Decode, "invalid final Base92 character");
    push(static_cast<std::uint32_t>(last), 6);
  }
  return out;  // leftover bits are padding
}

// ---------------------------------------------------------------------------- Base122

namespace {
constexpr std::array<std::uint8_t, 6> base122_illegal = {0, 10, 13, 34, 38, 92};
constexpr std::uint8_t base122_shortened = 0b111;
}  // namespace

std::string base122_encode(ByteView data) {
  std::string out;
  out.reserve(data.size() * 8 / 7 + 2);
  std::size_t index = 0;
  unsigned bit = 0;
  // The next 7 bits, most significant first; -1 at the end.
  const auto get7 = [&]() -> int {
    if (index >= data.size()) return -1;
    const unsigned first = byte_at(data, index);
    unsigned value = ((0xFEu >> bit) & first) << bit;
    value >>= 1;
    bit += 7;
    if (bit < 8) return static_cast<int>(value);
    bit -= 8;
    ++index;
    if (index >= data.size()) return static_cast<int>(value);
    const unsigned second = byte_at(data, index);
    unsigned part = ((0xFF00u >> bit) & second) & 0xFFu;
    part >>= 8 - bit;
    return static_cast<int>(value | part);
  };
  while (true) {
    const int bits = get7();
    if (bits < 0) break;
    const auto illegal = std::ranges::find(base122_illegal, static_cast<std::uint8_t>(bits));
    if (illegal == base122_illegal.end()) {
      out.push_back(static_cast<char>(bits));
      continue;
    }
    int next = get7();
    unsigned b1 = 0b11000010u;
    unsigned b2 = 0b10000000u;
    if (next < 0) {
      b1 |= (0b111u & base122_shortened) << 2;
      next = bits;
    } else {
      b1 |= (0b111u & static_cast<unsigned>(illegal - base122_illegal.begin())) << 2;
    }
    b1 |= (static_cast<unsigned>(next) & 0b01000000u) != 0 ? 1u : 0u;
    b2 |= static_cast<unsigned>(next) & 0b00111111u;
    out.push_back(static_cast<char>(b1));
    out.push_back(static_cast<char>(b2));
  }
  return out;
}

Result<Bytes> base122_decode(std::string_view text) {
  Bytes out;
  out.reserve(text.size() * 7 / 8 + 1);
  unsigned current = 0;
  unsigned bit_of_byte = 0;
  const auto push7 = [&](unsigned value) {
    value <<= 1;
    current |= (value >> bit_of_byte) & 0xFFu;
    bit_of_byte += 7;
    if (bit_of_byte >= 8) {
      out.push_back(static_cast<std::byte>(current & 0xFF));
      bit_of_byte -= 8;
      current = (value << (7 - bit_of_byte)) & 0xFFu;
    }
  };
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      push7(c);
      ++i;
      continue;
    }
    if ((c & 0xE0) != 0xC0 || i + 1 >= text.size() || (static_cast<unsigned char>(text[i + 1]) & 0xC0) != 0x80) {
      return fail(Status::Decode, "invalid Base122 sequence at offset {}", i);
    }
    const unsigned code = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
    const unsigned illegal = (code >> 8) & 7u;
    if (illegal != base122_shortened) {
      if (illegal >= base122_illegal.size()) return fail(Status::Decode, "invalid Base122 sequence at offset {}", i);
      push7(base122_illegal[illegal]);
    }
    push7(code & 127u);
    i += 2;
  }
  return out;
}

// ---------------------------------------------------------------------------- Bech32

namespace {

std::uint32_t bech32_polymod(const std::vector<std::uint8_t>& values) {
  constexpr std::array<std::uint32_t, 5> generator = {0x3b6a57b2u, 0x26508e6du, 0x1ea119fau, 0x3d4233ddu, 0x2a1462b3u};
  std::uint32_t chk = 1;
  for (const std::uint8_t v : values) {
    const std::uint32_t top = chk >> 25;
    chk = ((chk & 0x1ffffffu) << 5) ^ v;
    for (std::size_t i = 0; i < 5; ++i) {
      if ((top >> i) & 1u) chk ^= generator[i];
    }
  }
  return chk;
}

std::vector<std::uint8_t> bech32_hrp_expand(std::string_view hrp) {
  std::vector<std::uint8_t> out;
  out.reserve(hrp.size() * 2 + 1);
  for (const char c : hrp) out.push_back(static_cast<std::uint8_t>(byte_of(c) >> 5));
  out.push_back(0);
  for (const char c : hrp) out.push_back(static_cast<std::uint8_t>(byte_of(c) & 31));
  return out;
}

constexpr std::uint32_t bech32_constant(Bech32Variant variant) {
  return variant == Bech32Variant::Bech32 ? 1u : 0x2bc830a3u;
}

constexpr std::size_t bech32_max_bytes = 4096;

}  // namespace

Result<std::string> bech32_encode(std::string_view hrp, ByteView data, Bech32Variant variant) {
  if (hrp.empty() || hrp.size() > 83) return fail(Status::InvalidArgument, "the Bech32 prefix must have 1 to 83 characters");
  for (const char c : hrp) {
    if (byte_of(c) < 33 || byte_of(c) > 126 || (c >= 'A' && c <= 'Z')) {
      return fail(Status::InvalidArgument, "the Bech32 prefix must be lower-case printable ASCII");
    }
  }
  if (data.size() > bech32_max_bytes) {
    return fail(Status::Limit, "Bech32 is meant for short payloads; this encoder accepts up to {} bytes", bech32_max_bytes);
  }
  std::vector<std::uint8_t> values;
  std::uint32_t acc = 0;
  unsigned bits = 0;
  for (const std::byte b : data) {
    acc = (acc << 8) | std::to_integer<std::uint32_t>(b);
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      values.push_back(static_cast<std::uint8_t>((acc >> bits) & 31u));
    }
  }
  if (bits > 0) values.push_back(static_cast<std::uint8_t>((acc << (5 - bits)) & 31u));
  std::vector<std::uint8_t> check = bech32_hrp_expand(hrp);
  check.insert(check.end(), values.begin(), values.end());
  check.insert(check.end(), 6, 0);
  const std::uint32_t mod = bech32_polymod(check) ^ bech32_constant(variant);
  std::string out(hrp);
  out.push_back('1');
  for (const std::uint8_t v : values) out.push_back(bech32_charset[v]);
  for (int i = 0; i < 6; ++i) out.push_back(bech32_charset[(mod >> (5 * (5 - i))) & 31u]);
  return out;
}

Result<Bech32Decoded> bech32_decode(std::string_view text) {
  text = util::trim(text);
  bool lower = false, upper = false;
  for (const char c : text) {
    if (byte_of(c) < 33 || byte_of(c) > 126) return fail(Status::Decode, "Bech32 text contains invalid characters");
    lower = lower || (c >= 'a' && c <= 'z');
    upper = upper || (c >= 'A' && c <= 'Z');
  }
  if (lower && upper) return fail(Status::Decode, "Bech32 text mixes upper and lower case");
  const std::size_t separator = text.rfind('1');
  if (separator == std::string_view::npos || separator == 0 || text.size() - separator < 7) {
    return fail(Status::Decode, "not a Bech32 string (no '1' separator before a 6-character checksum)");
  }
  if (text.size() > bech32_max_bytes * 2) return fail(Status::Limit, "the Bech32 text is too long");
  Bech32Decoded out;
  out.hrp = util::to_lower(text.substr(0, separator));
  std::vector<std::uint8_t> values;
  for (std::size_t i = separator + 1; i < text.size(); ++i) {
    const int v = bech32_table[byte_of(text[i])];
    if (v < 0) return fail(Status::Decode, "invalid Bech32 character '{}'", text[i]);
    values.push_back(static_cast<std::uint8_t>(v));
  }
  std::vector<std::uint8_t> check = bech32_hrp_expand(out.hrp);
  check.insert(check.end(), values.begin(), values.end());
  const std::uint32_t mod = bech32_polymod(check);
  if (mod == bech32_constant(Bech32Variant::Bech32)) {
    out.variant = Bech32Variant::Bech32;
  } else if (mod == bech32_constant(Bech32Variant::Bech32m)) {
    out.variant = Bech32Variant::Bech32m;
  } else {
    return fail(Status::Decode, "Bech32 checksum mismatch");
  }
  values.resize(values.size() - 6);
  std::uint32_t acc = 0;
  unsigned bits = 0;
  for (const std::uint8_t v : values) {
    acc = (acc << 5) | v;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      out.data.push_back(static_cast<std::byte>((acc >> bits) & 0xFFu));
    }
  }
  if (bits >= 5 || (acc & ((1u << bits) - 1)) != 0) return fail(Status::Decode, "Bech32 data has invalid padding");
  return out;
}

// ---------------------------------------------------------------------------- Base58Check

Result<std::string> base58check_encode(ByteView data) {
  const auto first = util::sha256(data);
  const auto second = util::sha256(as_bytes(std::span<const std::uint8_t>(first.data(), first.size())));
  Bytes payload(data.begin(), data.end());
  for (std::size_t i = 0; i < 4; ++i) payload.push_back(static_cast<std::byte>(second[i]));
  return base58_encode(payload);
}

Result<Bytes> base58check_decode(std::string_view text) {
  CVT_TRY_ASSIGN(Bytes payload, base58_decode(text));
  if (payload.size() < 4) return fail(Status::Decode, "Base58Check text is too short to hold a checksum");
  const ByteView body = ByteView(payload).first(payload.size() - 4);
  const auto first = util::sha256(body);
  const auto second = util::sha256(as_bytes(std::span<const std::uint8_t>(first.data(), first.size())));
  for (std::size_t i = 0; i < 4; ++i) {
    if (std::to_integer<std::uint8_t>(payload[payload.size() - 4 + i]) != second[i]) {
      return fail(Status::Decode, "Base58Check checksum mismatch");
    }
  }
  payload.resize(payload.size() - 4);
  return payload;
}

// ---------------------------------------------------------------------------- PEM

std::string pem_encode(ByteView data, std::string_view label) {
  util::Base64Options style;
  style.line_width = 64;
  style.newline = "\n";
  std::string body = util::base64_encode(data, style);
  if (!body.empty()) body.push_back('\n');
  return std::format("-----BEGIN {}-----\n{}-----END {}-----\n", label, body, label);
}

Result<PemDecoded> pem_decode(std::string_view text) {
  const std::size_t begin = text.find("-----BEGIN ");
  if (begin == std::string_view::npos) return fail(Status::Decode, "no '-----BEGIN' line found");
  const std::size_t label_end = text.find("-----", begin + 11);
  if (label_end == std::string_view::npos) return fail(Status::Decode, "malformed BEGIN line");
  PemDecoded out;
  out.label = std::string(text.substr(begin + 11, label_end - begin - 11));
  const std::string end_marker = "-----END " + out.label + "-----";
  std::size_t body_start = text.find('\n', label_end);
  if (body_start == std::string_view::npos) return fail(Status::Decode, "the armoured block is empty");
  ++body_start;
  const std::size_t end = text.find(end_marker, body_start);
  if (end == std::string_view::npos) return fail(Status::Decode, "missing '{}'", end_marker);
  std::string_view body = text.substr(body_start, end - body_start);
  // RFC 1421 headers ("Proc-Type: ...") end at the first blank line.
  if (body.find(':') != std::string_view::npos && body.find(':') < body.find('\n')) {
    const std::size_t blank = body.find("\n\n") != std::string_view::npos ? body.find("\n\n") : body.find("\r\n\r\n");
    if (blank != std::string_view::npos) body = body.substr(blank + 2);
  }
  CVT_TRY_ASSIGN(out.data, util::base64_decode(body));
  return out;
}

// ---------------------------------------------------------------------------- xxencode

std::string xxencode(ByteView data, std::string_view filename) {
  std::string name;
  for (const char c : filename) {
    if (byte_of(c) > 0x20 && c != 0x7F) name.push_back(c);
  }
  if (name.empty()) name = "data";
  std::string out = "begin 644 " + name + "\n";
  for (std::size_t at = 0; at < data.size(); at += 45) {
    const std::size_t n = std::min<std::size_t>(45, data.size() - at);
    out.push_back(xx_alphabet[n]);
    for (std::size_t k = 0; k < n; k += 3) {
      const std::uint32_t group = (static_cast<std::uint32_t>(byte_at(data, at + k)) << 16) |
                                  (k + 1 < n ? static_cast<std::uint32_t>(byte_at(data, at + k + 1)) << 8 : 0u) |
                                  (k + 2 < n ? static_cast<std::uint32_t>(byte_at(data, at + k + 2)) : 0u);
      for (int shift = 18; shift >= 0; shift -= 6) out.push_back(xx_alphabet[(group >> shift) & 63u]);
    }
    out.push_back('\n');
  }
  out += "+\nend\n";
  return out;
}

Result<XxDecoded> xxdecode(std::string_view text) {
  XxDecoded out;
  const std::vector<std::string_view> lines = util::split_lines(text);
  std::size_t i = 0;
  while (i < lines.size() && !lines[i].starts_with("begin ")) ++i;
  if (i == lines.size()) return fail(Status::Decode, "no 'begin' line found");
  {
    const std::string_view header = util::trim(lines[i].substr(6));
    const std::size_t space = header.find(' ');
    if (space != std::string_view::npos) out.filename = std::string(util::trim(header.substr(space + 1)));
  }
  for (++i; i < lines.size(); ++i) {
    const std::string_view line = util::trim_right(lines[i]);
    if (line == "end") return out;
    if (line.empty()) continue;
    const int n = xx_table[byte_of(line[0])];
    if (n < 0) return fail(Status::Decode, "invalid xxencode line {}", i + 1);
    if (n == 0) continue;
    std::size_t produced = 0;
    for (std::size_t k = 1; produced < static_cast<std::size_t>(n); k += 4) {
      std::uint32_t group = 0;
      for (std::size_t m = 0; m < 4; ++m) {
        const char c = k + m < line.size() ? line[k + m] : '+';
        const int v = xx_table[byte_of(c)];
        if (v < 0) return fail(Status::Decode, "invalid xxencode character on line {}", i + 1);
        group = (group << 6) | static_cast<std::uint32_t>(v);
      }
      for (int shift = 16; shift >= 0 && produced < static_cast<std::size_t>(n); shift -= 8, ++produced) {
        out.bytes.push_back(static_cast<std::byte>((group >> shift) & 0xFFu));
      }
    }
  }
  return fail(Status::Decode, "the xxencoded data has no 'end' line");
}

// ---------------------------------------------------------------------------- BinHex 4.0

namespace {

void append_u32be(Bytes& out, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::byte>(v >> s));
}
void append_u16be(Bytes& out, std::uint16_t v) {
  out.push_back(static_cast<std::byte>(v >> 8));
  out.push_back(static_cast<std::byte>(v & 0xFF));
}

// RLE90: a run of 3 to 255 equal bytes becomes "byte 0x90 count"; a literal
// 0x90 is written as "0x90 0x00".
Bytes rle90_encode(ByteView data) {
  Bytes out;
  out.reserve(data.size() + data.size() / 64 + 4);
  std::size_t i = 0;
  while (i < data.size()) {
    const std::byte b = data[i];
    std::size_t run = 1;
    while (i + run < data.size() && data[i + run] == b && run < 255) ++run;
    out.push_back(b);
    if (b == std::byte{0x90}) out.push_back(std::byte{0x00});
    if (run >= 3) {
      out.push_back(std::byte{0x90});
      out.push_back(static_cast<std::byte>(run));
      i += run;
    } else {
      ++i;
    }
  }
  return out;
}

Result<Bytes> rle90_decode(ByteView data, std::size_t max_output) {
  Bytes out;
  out.reserve(std::min(data.size() * 2, max_output));
  for (std::size_t i = 0; i < data.size(); ++i) {
    const std::byte b = data[i];
    if (b != std::byte{0x90}) {
      out.push_back(b);
    } else {
      if (i + 1 >= data.size()) return fail(Status::Decode, "BinHex run-length marker at the end of the data");
      const auto count = std::to_integer<std::size_t>(data[++i]);
      if (count == 0) {
        out.push_back(std::byte{0x90});
      } else {
        if (out.empty()) return fail(Status::Decode, "BinHex run-length marker without a preceding byte");
        if (out.size() + count > max_output) return fail(Status::Limit, "the BinHex data expands beyond the limit");
        out.insert(out.end(), count - 1, out.back());
      }
    }
    if (out.size() > max_output) return fail(Status::Limit, "the BinHex data expands beyond the limit");
  }
  return out;
}

}  // namespace

std::string binhex_encode(ByteView data, std::string_view filename) {
  std::string name;
  for (const char c : filename) {
    if (byte_of(c) >= 0x20 && c != ':' && name.size() < 63) name.push_back(c);
  }
  if (name.empty()) name = "data";
  Bytes stream;
  Bytes header;
  header.push_back(static_cast<std::byte>(name.size()));
  for (const char c : name) header.push_back(static_cast<std::byte>(c));
  header.push_back(std::byte{0});
  for (const char c : std::string_view("????????")) header.push_back(static_cast<std::byte>(c));  // type, creator
  append_u16be(header, 0);                                                                        // Finder flags
  append_u32be(header, static_cast<std::uint32_t>(data.size()));
  append_u32be(header, 0);  // resource fork length
  stream = header;
  append_u16be(stream, crc16_xmodem(header));
  stream.insert(stream.end(), data.begin(), data.end());
  append_u16be(stream, crc16_xmodem(data));
  append_u16be(stream, 0);  // CRC of the empty resource fork
  const Bytes packed = rle90_encode(stream);

  std::string encoded = ":";
  for (std::size_t i = 0; i < packed.size(); i += 3) {
    const std::size_t n = std::min<std::size_t>(3, packed.size() - i);
    std::uint32_t group = 0;
    for (std::size_t k = 0; k < 3; ++k) group = (group << 8) | (k < n ? byte_at(packed, i + k) : 0u);
    for (std::size_t k = 0; k < n + 1; ++k) encoded.push_back(binhex_alphabet[(group >> (18 - 6 * k)) & 63u]);
  }
  encoded.push_back(':');
  std::string out = "(This file must be converted with BinHex 4.0)\n\n";
  for (std::size_t at = 0; at < encoded.size(); at += 64) {
    out.append(std::string_view(encoded).substr(at, 64));
    out.push_back('\n');
  }
  return out;
}

Result<BinHexDecoded> binhex_decode(std::string_view text, std::size_t max_output) {
  std::size_t start = text.find("(This file must be converted with BinHex");
  start = text.find(':', start == std::string_view::npos ? 0 : start);
  if (start == std::string_view::npos) return fail(Status::Decode, "no BinHex data (':' start marker) found");
  Bytes packed;
  packed.reserve(text.size() * 3 / 4);
  std::uint32_t group = 0;
  unsigned bits = 0;
  bool closed = false;
  for (std::size_t i = start + 1; i < text.size(); ++i) {
    const char c = text[i];
    if (c == ':') {
      closed = true;
      break;
    }
    if (util::is_ascii_space(c)) continue;
    const int v = binhex_table[byte_of(c)];
    if (v < 0) return fail(Status::Decode, "invalid BinHex character at offset {}", i);
    group = (group << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      packed.push_back(static_cast<std::byte>((group >> bits) & 0xFFu));
    }
  }
  if (!closed) return fail(Status::Decode, "the BinHex data has no closing ':'");
  CVT_TRY_ASSIGN(const Bytes stream, rle90_decode(packed, max_output + 1024));
  util::ByteReader reader(stream);
  const std::uint8_t name_length = reader.u8();
  if (name_length == 0 || name_length > 63) return fail(Status::Decode, "invalid BinHex file name length");
  BinHexDecoded out;
  out.filename = std::string(reader.chars(name_length));
  (void)reader.u8();  // version
  out.type = std::string(reader.chars(4));
  out.creator = std::string(reader.chars(4));
  (void)reader.u16be();  // Finder flags
  const std::uint32_t data_length = reader.u32be();
  const std::uint32_t resource_length = reader.u32be();
  if (!reader.ok()) return fail(Status::Decode, "truncated BinHex header");
  const std::size_t header_size = reader.position();
  const std::uint16_t header_crc = reader.u16be();
  if (!reader.ok() || crc16_xmodem(ByteView(stream).first(header_size)) != header_crc) {
    return fail(Status::Decode, "BinHex header CRC mismatch");
  }
  if (data_length > max_output || resource_length > max_output) return fail(Status::Limit, "BinHex fork exceeds the limit");
  if (reader.remaining() < std::uint64_t{data_length} + 2) return fail(Status::Decode, "truncated BinHex data fork");
  const ByteView data = reader.bytes(data_length);
  if (crc16_xmodem(data) != reader.u16be()) return fail(Status::Decode, "BinHex data fork CRC mismatch");
  if (reader.remaining() < std::uint64_t{resource_length} + 2) return fail(Status::Decode, "truncated BinHex resource fork");
  const ByteView resource = reader.bytes(resource_length);
  if (crc16_xmodem(resource) != reader.u16be()) return fail(Status::Decode, "BinHex resource fork CRC mismatch");
  out.data.assign(data.begin(), data.end());
  out.resource.assign(resource.begin(), resource.end());
  return out;
}

// ---------------------------------------------------------------------------- RFC 2047

std::string mime_words_encode(std::string_view text, bool q_encoding) {
  const std::string valid = util::sanitize_utf8(text);
  constexpr std::string_view prefix_b = "=?UTF-8?B?", prefix_q = "=?UTF-8?Q?", suffix = "?=";
  constexpr std::size_t max_word = 75;
  std::vector<std::string> words;
  std::string pending;  // raw bytes of the word being built
  const auto encoded_size = [q_encoding](std::string_view raw) {
    if (!q_encoding) return (raw.size() + 2) / 3 * 4;
    std::size_t n = 0;
    for (const char c : raw) n += (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == ' ') ? 1 : 3;
    return n;
  };
  const auto emit = [&]() {
    std::string word(q_encoding ? prefix_q : prefix_b);
    if (q_encoding) {
      for (const char c : pending) {
        if (c == ' ') {
          word.push_back('_');
        } else if (util::is_ascii_alpha(c) || util::is_ascii_digit(c)) {
          word.push_back(c);
        } else {
          word.push_back('=');
          detail::append_hex_byte(word, byte_of(c), true);
        }
      }
    } else {
      word += util::base64_encode(as_bytes(pending));
    }
    word += suffix;
    words.push_back(std::move(word));
    pending.clear();
  };
  const std::size_t room = max_word - prefix_b.size() - suffix.size();
  for (std::size_t i = 0; i < valid.size();) {
    const auto length = static_cast<std::size_t>(util::decode_utf8(valid, i).length);
    const std::string_view character = std::string_view(valid).substr(i, length);
    std::string candidate = pending;
    candidate.append(character);
    if (!pending.empty() && encoded_size(candidate) > room) emit();
    pending.append(character);
    i += length;
  }
  if (!pending.empty() || words.empty()) emit();
  return util::join(words, "\r\n ");
}

Result<std::string> mime_words_decode(std::string_view text) {
  std::string out;
  std::size_t i = 0;
  bool previous_was_word = false;
  std::string gap;  // whitespace after an encoded word, dropped if another word follows
  while (i < text.size()) {
    const std::size_t start = text.find("=?", i);
    if (start == std::string_view::npos) {
      out += gap;
      out.append(text.substr(i));
      break;
    }
    const std::size_t q1 = text.find('?', start + 2);
    const std::size_t q2 = q1 == std::string_view::npos ? q1 : text.find('?', q1 + 1);
    const std::size_t end = q2 == std::string_view::npos ? q2 : text.find("?=", q2 + 1);
    if (end == std::string_view::npos || q2 != q1 + 2) {
      out += gap;
      gap.clear();
      out.append(text.substr(i, start + 2 - i));
      i = start + 2;
      previous_was_word = false;
      continue;
    }
    const std::string_view between = text.substr(i, start - i);
    const bool only_space = std::ranges::all_of(between, [](char c) { return util::is_ascii_space(c); });
    if (!(previous_was_word && only_space)) {
      out += gap;
      out.append(between);
    }
    gap.clear();
    std::string_view charset = text.substr(start + 2, q1 - start - 2);
    if (const std::size_t star = charset.find('*'); star != std::string_view::npos) charset = charset.substr(0, star);  // RFC 2231 language
    const char kind = util::ascii_upper(text[q1 + 1]);
    const std::string_view payload = text.substr(q2 + 1, end - q2 - 1);
    Bytes raw;
    if (kind == 'B') {
      CVT_TRY_ASSIGN(raw, util::base64_decode(payload));
    } else if (kind == 'Q') {
      for (std::size_t k = 0; k < payload.size(); ++k) {
        const char c = payload[k];
        if (c == '_') {
          raw.push_back(std::byte{' '});
        } else if (c == '=' && k + 2 < payload.size() && util::hex_digit_value(payload[k + 1]) >= 0 &&
                   util::hex_digit_value(payload[k + 2]) >= 0) {
          raw.push_back(static_cast<std::byte>(util::hex_digit_value(payload[k + 1]) * 16 + util::hex_digit_value(payload[k + 2])));
          k += 2;
        } else {
          raw.push_back(static_cast<std::byte>(c));
        }
      }
    } else {
      return fail(Status::Decode, "unknown encoded-word encoding '{}'", text[q1 + 1]);
    }
    const auto encoding = util::parse_encoding(charset);
    if (!encoding) return fail(Status::Decode, "unsupported charset '{}' in an encoded word", charset);
    out += util::decode_text(raw, *encoding).text;
    i = end + 2;
    previous_was_word = true;
    // Collect whitespace that may separate this word from the next one.
    const std::size_t next = text.find_first_not_of(" \t\r\n", i);
    gap = std::string(text.substr(i, (next == std::string_view::npos ? text.size() : next) - i));
    i = next == std::string_view::npos ? text.size() : next;
  }
  return out;
}

// ---------------------------------------------------------------------------- registration

void register_more_bases(Registry& registry) {
  const OptionSpec line_ending = OptionSpec::choice("line_ending", "Line ending", "lf", {"lf", "crlf"},
                                                    "Line terminator used when the output is wrapped.")
                                     .advanced();
  add_binary_encoding(registry, {
      .id = "base16", .name = "Base16 text", .description = "RFC 4648 Base16: upper-case hexadecimal, two digits per byte.",
      .extensions = {"b16", "base16"}, .popularity = 20,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        const auto width = static_cast<std::size_t>(ctx.options.get_int("line_width", 0, 0, 1024));
        return wrap(base16_encode(blob.bytes), width, newline_of(ctx.options));
      },
      .decode = [](std::string_view text, const ReadContext&) { return base16_decode(text); },
      .encode_options = {OptionSpec::integer("line_width", "Line width", 0, 0, 1024,
                                             "Characters per line; 0 writes a single line."),
                         line_ending},
      .expansion = 6,
  });
  add_binary_encoding(registry, {
      .id = "base45", .name = "Base45 text", .description = "RFC 9285 Base45, the alphanumeric encoding used in QR codes.",
      .extensions = {"b45", "base45"}, .popularity = 12,
      .encode = [](const ir::Blob& blob, const WriteContext&) -> Result<std::string> { return base45_encode(blob.bytes); },
      .decode = [](std::string_view text, const ReadContext&) { return base45_decode(text); },
      .expansion = 2,
  });
  add_binary_encoding(registry, {
      .id = "base62", .name = "Base62 text",
      .description = "The file as one big number written with 0-9, A-Z and a-z; for files up to 64 KiB.",
      .extensions = {"b62", "base62"}, .popularity = 15,
      .encode = [](const ir::Blob& blob, const WriteContext&) { return base62_encode(blob.bytes); },
      .decode = [](std::string_view text, const ReadContext&) { return base62_decode(text); },
      .expansion = 2,
  });
  add_binary_encoding(registry, {
      .id = "base64-mime", .name = "Base64 text (MIME)",
      .description = "Base64 as in e-mail bodies (RFC 2045): 76 characters per line, CRLF line breaks.",
      .extensions = {"b64", "mime"}, .popularity = 25,
      .encode = [](const ir::Blob& blob, const WriteContext&) -> Result<std::string> {
        util::Base64Options style;
        style.line_width = 76;
        style.newline = "\r\n";
        std::string text = util::base64_encode(blob.bytes, style);
        if (!text.empty()) text += "\r\n";
        return text;
      },
      .decode = [](std::string_view text, const ReadContext&) { return util::base64_decode(text); },
      .expansion = 2,
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = as_chars(data).substr(0, 160);
        return text.size() > 80 && text.find("\r\n") == 76 && text.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") == 76 ? 80 : 0;
      },
  });
  add_binary_encoding(registry, {
      .id = "pem", .name = "PEM armoured Base64",
      .description = "Base64 between '-----BEGIN label-----' and '-----END label-----' lines, as for keys and certificates.",
      .extensions = {"pem", "asc"}, .mime = "application/x-pem-file", .popularity = 30,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        const std::string label = ctx.options.get_string("label", "DATA");
        if (label.empty() || label.size() > 64 ||
            !std::ranges::all_of(label, [](char c) { return byte_of(c) >= 0x20 && byte_of(c) < 0x7F && c != '-'; })) {
          return fail(Status::InvalidArgument, "option 'label' must be 1 to 64 printable ASCII characters without '-'");
        }
        return pem_encode(blob.bytes, label);
      },
      .decode = [](std::string_view text, const ReadContext&) -> Result<Bytes> {
        CVT_TRY_ASSIGN(PemDecoded decoded, pem_decode(text));
        return std::move(decoded.data);
      },
      .encode_options = {OptionSpec::text("label", "Label", "DATA",
                                          "Type written into the BEGIN/END lines, e.g. CERTIFICATE or PUBLIC KEY.")},
      .expansion = 2,
      .sniff = [](ByteView data, std::string_view) {
        return util::trim_left(as_chars(data).substr(0, 256)).starts_with("-----BEGIN ") ? 90 : 0;
      },
  });
  add_binary_encoding(registry, {
      .id = "base91", .name = "basE91 text", .description = "basE91: about 23% overhead, using 91 printable ASCII characters.",
      .extensions = {"b91", "base91"}, .popularity = 10,
      .encode = [](const ir::Blob& blob, const WriteContext&) -> Result<std::string> { return base91_encode(blob.bytes); },
      .decode = [](std::string_view text, const ReadContext&) -> Result<Bytes> { return base91_decode(text); },
      .expansion = 2,
  });
  add_binary_encoding(registry, {
      .id = "base92", .name = "Base92 text", .description = "Base92: 13 bits in two printable ASCII characters.",
      .extensions = {"b92", "base92"}, .popularity = 6,
      .encode = [](const ir::Blob& blob, const WriteContext&) -> Result<std::string> { return base92_encode(blob.bytes); },
      .decode = [](std::string_view text, const ReadContext&) { return base92_decode(text); },
      .expansion = 2,
  });
  add_binary_encoding(registry, {
      .id = "base122", .name = "Base122 text",
      .description = "Base122: 7 bits per UTF-8 character, a compact way to embed binary data in HTML.",
      .extensions = {"b122", "base122"}, .popularity = 5,
      .encode = [](const ir::Blob& blob, const WriteContext&) -> Result<std::string> { return base122_encode(blob.bytes); },
      .decode = [](std::string_view text, const ReadContext&) { return base122_decode(text); },
      .expansion = 2,
  });
  add_binary_encoding(registry, {
      .id = "bech32", .name = "Bech32 text",
      .description = "Bech32 or Bech32m (BIP 173/350): a prefix, '1', 5-bit data and a 6-character checksum.",
      .extensions = {"bech32"}, .popularity = 10,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        const bool m = ctx.options.get_enum("variant", "bech32", {"bech32", "bech32m"}) == "bech32m";
        CVT_TRY_ASSIGN(std::string text, bech32_encode(ctx.options.get_string("hrp", "data"), blob.bytes,
                                                       m ? Bech32Variant::Bech32m : Bech32Variant::Bech32));
        if (text.size() > 90) ctx.warn("The result is longer than the 90 characters BIP 173 allows for addresses.");
        return text;
      },
      .decode = [](std::string_view text, const ReadContext&) -> Result<Bytes> {
        CVT_TRY_ASSIGN(Bech32Decoded decoded, bech32_decode(text));
        return std::move(decoded.data);
      },
      .encode_options = {OptionSpec::text("hrp", "Prefix (HRP)", "data", "Human-readable part before the '1' separator."),
                         OptionSpec::choice("variant", "Checksum", "bech32", {"bech32", "bech32m"},
                                            "Bech32 (BIP 173) or Bech32m (BIP 350) checksum constant.")},
      .expansion = 2,
  });
  add_binary_encoding(registry, {
      .id = "base58check", .name = "Base58Check text",
      .description = "Base58 with a 4-byte double SHA-256 checksum, as in Bitcoin addresses and keys.",
      .extensions = {"b58c", "base58check"}, .popularity = 10,
      .encode = [](const ir::Blob& blob, const WriteContext&) { return base58check_encode(blob.bytes); },
      .decode = [](std::string_view text, const ReadContext&) { return base58check_decode(text); },
      .expansion = 2,
  });
  add_binary_encoding(registry, {
      .id = "xxe", .name = "Xxencoded text", .description = "xxencode: uuencode's layout with a mail-safe alphabet.",
      .extensions = {"xxe", "xx"}, .mime = "text/x-xxencode", .popularity = 6,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        const std::string name = ctx.options.get_string("filename");
        return xxencode(blob.bytes, name.empty() ? blob.filename : name);
      },
      .decode_blob = [](std::string_view text, const ReadContext&) -> Result<ir::Blob> {
        CVT_TRY_ASSIGN(XxDecoded decoded, xxdecode(text));
        ir::Blob blob;
        blob.bytes = std::move(decoded.bytes);
        blob.filename = std::move(decoded.filename);
        return blob;
      },
      .encode_options = {OptionSpec::text("filename", "File name", "",
                                          "Name stored in the 'begin' line; empty uses the source file name.")},
      .expansion = 2,
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = as_chars(data).substr(0, 1024);
        const std::size_t nl = text.find('\n');
        if (!text.starts_with("begin ") || nl == std::string_view::npos) return 0;
        const std::string_view body = text.substr(nl + 1, text.find("\nend") - nl - 1);
        const bool lower = std::ranges::any_of(body, [](char c) { return c >= 'a' && c <= 'z'; });
        const bool alphabet = std::ranges::all_of(body, [](char c) { return xx_table[byte_of(c)] >= 0 || c == '\n' || c == '\r'; });
        return lower && alphabet ? 90 : 0;
      },
  });
  add_binary_encoding(registry, {
      .id = "hqx", .name = "BinHex 4.0", .description = "Classic Mac OS BinHex: CRC-checked, run-length packed 6-bit text.",
      .extensions = {"hqx"}, .mime = "application/mac-binhex40", .popularity = 8,
      .encode = [](const ir::Blob& blob, const WriteContext&) -> Result<std::string> {
        return binhex_encode(blob.bytes, blob.filename);
      },
      .decode_blob = [](std::string_view text, const ReadContext& ctx) -> Result<ir::Blob> {
        CVT_TRY_ASSIGN(BinHexDecoded decoded, binhex_decode(text, ctx.limits.max_decompressed_bytes));
        if (!decoded.resource.empty()) {
          ctx.warn(std::format("The resource fork ({} bytes) was dropped; only the data fork is kept.", decoded.resource.size()));
        }
        ir::Blob blob;
        blob.bytes = std::move(decoded.data);
        blob.filename = std::move(decoded.filename);
        return blob;
      },
      .expansion = 2,
      .sniff = [](ByteView data, std::string_view) {
        return as_chars(data).substr(0, 512).find("(This file must be converted") != std::string_view::npos ? 95 : 0;
      },
  });
  add_text_transform(registry, {
      .id = "mime-words", .name = "MIME encoded words",
      .description = "Text as RFC 2047 encoded words (=?UTF-8?B?...?=), as used in e-mail subjects and names.",
      .extensions = {"mimewords"}, .popularity = 12,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        return mime_words_encode(text, ctx.options.get_enum("encoding", "b", {"b", "q"}) == "q");
      },
      .decode = [](std::string_view text, const ReadContext&) { return mime_words_decode(text); },
      .encode_options = {OptionSpec::choice("encoding", "Encoding", "b", {"b", "q"},
                                            "B (Base64) or Q (quoted-printable-like, readable for mostly ASCII text).")},
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = util::trim_left(as_chars(data).substr(0, 128));
        return text.starts_with("=?") && text.find("?=") != std::string_view::npos &&
                       (util::istarts_with(text.substr(text.find('?', 2) + 1), "B?") ||
                        util::istarts_with(text.substr(text.find('?', 2) + 1), "Q?"))
                   ? 85
                   : 0;
      },
      .expansion = 6,
  });
}

}  // namespace cvt::encoding
