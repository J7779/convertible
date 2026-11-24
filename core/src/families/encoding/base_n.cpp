// Convertible core - encoding family: Base32, Base58, Base36, Ascii85, Z85 and gzip+Base64.
//
// Adding another "base N" encoding: write `foo_encode` / `foo_decode` below
// (declare them in encoding.hpp), then add one `add_binary_encoding` block to
// `register_base_n` at the end of this file. Set `.expansion` to the largest
// number of output characters one input byte can produce, line breaks included.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/encoding/detail.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/base64.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

using detail::byte_at;
using detail::byte_of;
using detail::DigitTable;

// --------------------------------------------------------------------------- alphabets

constexpr std::string_view base32_standard_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr std::string_view base32_hex_alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
constexpr std::string_view base32_crockford_alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::string_view base58_alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr std::string_view base36_lower_alphabet = "0123456789abcdefghijklmnopqrstuvwxyz";
constexpr std::string_view base36_upper_alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::string_view z85_alphabet =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?&<>()[]{}@%$#";

constexpr DigitTable make_crockford_table() noexcept {
  DigitTable table = detail::make_digit_table(base32_crockford_alphabet, true);
  // Characters that are easily confused are read as the digit they resemble.
  table[byte_of('O')] = table[byte_of('o')] = 0;
  table[byte_of('I')] = table[byte_of('i')] = 1;
  table[byte_of('L')] = table[byte_of('l')] = 1;
  return table;
}

constexpr DigitTable base32_standard_table = detail::make_digit_table(base32_standard_alphabet, true);
constexpr DigitTable base32_hex_table = detail::make_digit_table(base32_hex_alphabet, true);
constexpr DigitTable base32_crockford_table = make_crockford_table();
constexpr DigitTable base58_table = detail::make_digit_table(base58_alphabet, false);
constexpr std::string_view base62_alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr DigitTable base62_table = detail::make_digit_table(base62_alphabet, false);
constexpr DigitTable base36_table = detail::make_digit_table(base36_lower_alphabet, true);
constexpr DigitTable z85_table = detail::make_digit_table(z85_alphabet, false);

constexpr std::string_view alphabet_of(Base32Alphabet alphabet) noexcept {
  switch (alphabet) {
    case Base32Alphabet::Standard: return base32_standard_alphabet;
    case Base32Alphabet::Hex: return base32_hex_alphabet;
    case Base32Alphabet::Crockford: return base32_crockford_alphabet;
  }
  return base32_standard_alphabet;
}

constexpr const DigitTable& table_of(Base32Alphabet alphabet) noexcept {
  switch (alphabet) {
    case Base32Alphabet::Standard: return base32_standard_table;
    case Base32Alphabet::Hex: return base32_hex_table;
    case Base32Alphabet::Crockford: return base32_crockford_table;
  }
  return base32_standard_table;
}

// --------------------------------------------------------------------------- layout helpers

// Break `text` into lines of `width` characters, each ending in LF.
std::string wrap_lines(std::string text, std::size_t width) {
  if (width == 0 || text.empty()) return text;
  std::string out;
  out.reserve(text.size() + text.size() / width + 1);
  for (std::size_t i = 0; i < text.size(); i += width) {
    out.append(text, i, width);
    out.push_back('\n');
  }
  return out;
}

void append_u32be(Bytes& out, std::uint32_t value, std::size_t count = 4) {
  for (std::size_t k = 0; k < count; ++k) out.push_back(static_cast<std::byte>((value >> (24 - 8 * k)) & 0xFFu));
}

// --------------------------------------------------------------------------- big-number bases

// One "whole input as a big integer" base. Digits are produced and consumed
// `chunk_digits` at a time so that the quadratic loops run over 32-bit limbs
// instead of single digits.
struct BigBase {
  std::string_view name;
  std::uint32_t base;
  std::uint32_t chunk_digits;
  std::uint32_t chunk_value;  // base ^ chunk_digits, below 2^32
  std::size_t max_digits;     // digits needed for big_number_max_bytes bytes
};

constexpr BigBase base58_numbers{"Base58", 58, 5, 656'356'768u, 89'500};
constexpr BigBase base36_numbers{"Base36", 36, 6, 2'176'782'336u, 101'412};
constexpr BigBase base62_numbers{"Base62", 62, 5, 916'132'832u, 88'100};

Result<std::string> big_number_encode(ByteView data, const BigBase& spec, std::string_view alphabet) {
  if (data.size() > big_number_max_bytes) {
    return fail(Status::Limit,
                "{} treats the file as one big number, which takes quadratic time; it is limited to {} bytes and "
                "this input has {}",
                spec.name, big_number_max_bytes, data.size());
  }
  std::size_t zeros = 0;
  while (zeros < data.size() && byte_at(data, zeros) == 0) ++zeros;

  // The remaining bytes as big-endian 32-bit limbs.
  const std::size_t count = data.size() - zeros;
  std::vector<std::uint32_t> limbs((count + 3) / 4, 0u);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t from_end = count - 1 - i;
    limbs[limbs.size() - 1 - from_end / 4] |= static_cast<std::uint32_t>(byte_at(data, zeros + i)) << (8 * (from_end % 4));
  }

  std::string reversed;
  reversed.reserve(count * 3 / 2 + zeros + spec.chunk_digits);
  std::size_t first = 0;  // limbs before this one are zero
  while (first < limbs.size()) {
    std::uint64_t remainder = 0;
    for (std::size_t i = first; i < limbs.size(); ++i) {
      const std::uint64_t value = (remainder << 32) | limbs[i];
      limbs[i] = static_cast<std::uint32_t>(value / spec.chunk_value);
      remainder = value % spec.chunk_value;
    }
    while (first < limbs.size() && limbs[first] == 0) ++first;
    for (std::uint32_t k = 0; k < spec.chunk_digits; ++k) {
      reversed.push_back(alphabet[static_cast<std::size_t>(remainder % spec.base)]);
      remainder /= spec.base;
    }
  }
  // The most significant chunk may have been written with leading zero digits.
  while (!reversed.empty() && reversed.back() == alphabet[0]) reversed.pop_back();
  reversed.append(zeros, alphabet[0]);
  std::ranges::reverse(reversed);
  return reversed;
}

Result<Bytes> big_number_decode(std::string_view text, const BigBase& spec, const DigitTable& table) {
  std::vector<std::uint8_t> digits;
  digits.reserve(std::min(text.size(), spec.max_digits));
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (util::is_ascii_space(c)) continue;
    const int value = table[byte_of(c)];
    if (value < 0) return fail(Status::Decode, "invalid {} character at offset {}", spec.name, i);
    if (digits.size() >= spec.max_digits) {
      return fail(Status::Limit,
                  "{} treats the text as one big number, which takes quadratic time; it is limited to {} digits "
                  "({} bytes of data)",
                  spec.name, spec.max_digits, big_number_max_bytes);
    }
    digits.push_back(static_cast<std::uint8_t>(value));
  }
  std::size_t zeros = 0;
  while (zeros < digits.size() && digits[zeros] == 0) ++zeros;

  // Little-endian limbs; each step is limbs = limbs * base^n + chunk.
  std::vector<std::uint32_t> limbs;
  limbs.reserve((digits.size() - zeros) / 5 + 2);
  std::size_t position = zeros;
  std::size_t take = (digits.size() - zeros) % spec.chunk_digits;
  if (take == 0) take = spec.chunk_digits;
  while (position < digits.size()) {
    std::uint32_t chunk = 0;
    std::uint32_t multiplier = 1;
    for (std::size_t k = 0; k < take; ++k) {
      chunk = chunk * spec.base + digits[position + k];
      multiplier *= spec.base;
    }
    position += take;
    take = spec.chunk_digits;
    std::uint64_t carry = chunk;
    for (std::uint32_t& limb : limbs) {
      const std::uint64_t value = static_cast<std::uint64_t>(limb) * multiplier + carry;
      limb = static_cast<std::uint32_t>(value & 0xFFFF'FFFFu);
      carry = value >> 32;
    }
    if (carry != 0) limbs.push_back(static_cast<std::uint32_t>(carry));
  }

  Bytes out(zeros, std::byte{0});
  out.reserve(zeros + limbs.size() * 4);
  bool started = false;
  for (std::size_t index = limbs.size(); index-- > 0;) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      const auto value = static_cast<std::uint8_t>((limbs[index] >> shift) & 0xFFu);
      if (value == 0 && !started) continue;
      started = true;
      out.push_back(static_cast<std::byte>(value));
    }
  }
  return out;
}

// --------------------------------------------------------------------------- shared option specs

OptionSpec line_width_option(std::int64_t default_width) {
  return OptionSpec::integer("line_width", "Line width", default_width, 0, 1024,
                             "Characters per line; 0 writes a single unbroken line.");
}

// Whole groups of five bytes when the text is one line (see BinaryEncoding::chunk).
std::size_t base32_chunk(const Options& options) { return options.get_int("line_width", 0, 0, 1024) == 0 ? 5 : 0; }

OptionSpec padding_option() {
  return OptionSpec::boolean("padding", "Padding", true, "End the output with '=' padding characters.");
}

std::size_t line_width_of(const Options& options, std::int64_t fallback) {
  return static_cast<std::size_t>(options.get_int("line_width", fallback, 0, 1024));
}

}  // namespace

// --------------------------------------------------------------------------- Base32

std::string base32_encode(ByteView data, const Base32Style& style) {
  const std::string_view alphabet = alphabet_of(style.alphabet);
  const bool padding = style.padding && style.alphabet != Base32Alphabet::Crockford;
  std::string out;
  out.reserve((data.size() + 4) / 5 * 8);
  for (std::size_t offset = 0; offset < data.size(); offset += 5) {
    const std::size_t count = std::min<std::size_t>(5, data.size() - offset);
    std::uint64_t group = 0;
    for (std::size_t k = 0; k < 5; ++k) group = (group << 8) | (k < count ? byte_at(data, offset + k) : 0u);
    const std::size_t characters = (count * 8 + 4) / 5;  // 1 -> 2, 2 -> 4, 3 -> 5, 4 -> 7, 5 -> 8
    for (std::size_t k = 0; k < 8; ++k) {
      if (k < characters) {
        out.push_back(alphabet[static_cast<std::size_t>((group >> (35 - 5 * k)) & 31u)]);
      } else if (padding) {
        out.push_back('=');
      }
    }
  }
  return wrap_lines(std::move(out), style.line_width);
}

Result<Bytes> base32_decode(std::string_view text, Base32Alphabet alphabet) {
  const DigitTable& table = table_of(alphabet);
  const bool crockford = alphabet == Base32Alphabet::Crockford;
  Bytes out;
  out.reserve(text.size() / 8 * 5 + 5);
  std::uint32_t buffer = 0;
  int bits = 0;
  std::size_t digits = 0;
  std::size_t padding = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (util::is_ascii_space(c) || (crockford && c == '-')) continue;
    if (c == '=' && !crockford) {
      ++padding;
      continue;
    }
    if (padding > 0) return fail(Status::Decode, "Base32 data continues after the padding at offset {}", i);
    const int value = table[byte_of(c)];
    if (value < 0) return fail(Status::Decode, "invalid Base32 character at offset {}", i);
    buffer = ((buffer << 5) | static_cast<std::uint32_t>(value)) & 0xFFFFu;
    bits += 5;
    ++digits;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::byte>((buffer >> bits) & 0xFFu));
    }
  }
  const std::size_t tail = digits % 8;
  if (tail == 1 || tail == 3 || tail == 6) {
    return fail(Status::Decode, "{} Base32 characters cannot encode a whole number of bytes", digits);
  }
  if (padding > 0 && (digits + padding) % 8 != 0) return fail(Status::Decode, "the Base32 padding has the wrong length");
  return out;
}

// --------------------------------------------------------------------------- Base58 / Base36

Result<std::string> base58_encode(ByteView data) { return big_number_encode(data, base58_numbers, base58_alphabet); }

Result<Bytes> base58_decode(std::string_view text) { return big_number_decode(text, base58_numbers, base58_table); }

Result<std::string> base36_encode(ByteView data, bool uppercase) {
  return big_number_encode(data, base36_numbers, uppercase ? base36_upper_alphabet : base36_lower_alphabet);
}

Result<Bytes> base36_decode(std::string_view text) { return big_number_decode(text, base36_numbers, base36_table); }

Result<std::string> base62_encode(ByteView data) { return big_number_encode(data, base62_numbers, base62_alphabet); }

Result<Bytes> base62_decode(std::string_view text) { return big_number_decode(text, base62_numbers, base62_table); }

// --------------------------------------------------------------------------- Ascii85

std::string ascii85_encode(ByteView data, const Ascii85Style& style) {
  std::string out;
  out.reserve(data.size() / 4 * 5 + data.size() / 16 + 16);
  std::size_t column = 0;
  // Tokens (a delimiter or one character) are never split across lines.
  const auto put = [&](std::string_view token) {
    if (style.line_width > 0 && column > 0 && column + token.size() > style.line_width) {
      out.push_back('\n');
      column = 0;
    }
    out.append(token);
    column += token.size();
  };
  if (style.wrapper) put("<~");
  for (std::size_t offset = 0; offset < data.size(); offset += 4) {
    const std::size_t count = std::min<std::size_t>(4, data.size() - offset);
    std::uint32_t group = 0;
    for (std::size_t k = 0; k < 4; ++k) group = (group << 8) | (k < count ? byte_at(data, offset + k) : 0u);
    if (count == 4 && group == 0) {
      put("z");
      continue;
    }
    std::array<char, 5> characters{};
    for (std::size_t k = 5; k-- > 0;) {
      characters[k] = static_cast<char>('!' + group % 85);
      group /= 85;
    }
    // A partial group of n bytes is written as its first n + 1 characters.
    for (std::size_t k = 0; k < count + 1; ++k) put(std::string_view(&characters[k], 1));
  }
  if (style.wrapper) put("~>");
  if (style.line_width > 0 && !out.empty()) out.push_back('\n');
  return out;
}

Result<Bytes> ascii85_decode(std::string_view text) {
  text = util::trim(text);
  const bool wrapped = text.starts_with("<~");
  if (wrapped) text.remove_prefix(2);

  Bytes out;
  out.reserve(text.size() / 5 * 4 + 4);
  std::uint64_t group = 0;
  std::size_t count = 0;
  bool ended = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (util::is_ascii_space(c)) continue;
    if (c == '~') {
      if (i + 1 >= text.size() || text[i + 1] != '>') {
        return fail(Status::Decode, "'~' at offset {} is not part of the '~>' end marker", i);
      }
      if (!util::trim(text.substr(i + 2)).empty()) return fail(Status::Decode, "data follows the '~>' end marker");
      ended = true;
      break;
    }
    if (c == 'z') {
      if (count != 0) return fail(Status::Decode, "'z' inside an Ascii85 group at offset {}", i);
      append_u32be(out, 0);
      continue;
    }
    if (c < '!' || c > 'u') return fail(Status::Decode, "invalid Ascii85 character at offset {}", i);
    group = group * 85 + static_cast<std::uint64_t>(c - '!');
    if (++count == 5) {
      if (group > 0xFFFF'FFFFu) return fail(Status::Decode, "Ascii85 group ending at offset {} is out of range", i);
      append_u32be(out, static_cast<std::uint32_t>(group));
      group = 0;
      count = 0;
    }
  }
  if (wrapped && !ended) return fail(Status::Decode, "the Ascii85 data has no '~>' end marker");
  if (count == 1) return fail(Status::Decode, "the Ascii85 data ends in the middle of a group");
  if (count > 1) {
    for (std::size_t k = count; k < 5; ++k) group = group * 85 + 84;
    if (group > 0xFFFF'FFFFu) return fail(Status::Decode, "the last Ascii85 group is out of range");
    append_u32be(out, static_cast<std::uint32_t>(group), count - 1);
  }
  return out;
}

// --------------------------------------------------------------------------- Z85

Result<std::string> z85_encode(ByteView data) {
  if (data.size() % 4 != 0) {
    return fail(Status::Encode,
                "Z85 can only encode data whose size is a multiple of 4 bytes; this input has {} bytes", data.size());
  }
  std::string out;
  out.reserve(data.size() / 4 * 5);
  for (std::size_t offset = 0; offset < data.size(); offset += 4) {
    std::uint32_t group = 0;
    for (std::size_t k = 0; k < 4; ++k) group = (group << 8) | byte_at(data, offset + k);
    std::array<char, 5> characters{};
    for (std::size_t k = 5; k-- > 0;) {
      characters[k] = z85_alphabet[group % 85];
      group /= 85;
    }
    out.append(characters.data(), characters.size());
  }
  return out;
}

Result<Bytes> z85_decode(std::string_view text) {
  Bytes out;
  out.reserve(text.size() / 5 * 4);
  std::uint64_t group = 0;
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (util::is_ascii_space(c)) continue;
    const int value = z85_table[byte_of(c)];
    if (value < 0) return fail(Status::Decode, "invalid Z85 character at offset {}", i);
    group = group * 85 + static_cast<std::uint64_t>(value);
    if (++count == 5) {
      if (group > 0xFFFF'FFFFu) return fail(Status::Decode, "Z85 group ending at offset {} is out of range", i);
      append_u32be(out, static_cast<std::uint32_t>(group));
      group = 0;
      count = 0;
    }
  }
  if (count != 0) return fail(Status::Decode, "Z85 text must have a multiple of 5 characters");
  return out;
}

// --------------------------------------------------------------------------- gzip + Base64

Result<std::string> gzip_base64_encode(ByteView data, int compression_level, std::size_t line_width) {
  CVT_TRY_ASSIGN(const Bytes compressed, util::gzip_compress(data, compression_level));
  util::Base64Options style;
  style.line_width = line_width;
  std::string text = util::base64_encode(compressed, style);
  if (line_width > 0 && !text.empty()) text.push_back('\n');
  return text;
}

Result<Bytes> gzip_base64_decode(std::string_view text, std::uint64_t max_output) {
  CVT_TRY_ASSIGN(const Bytes compressed, util::base64_decode(text));
  if (compressed.empty()) return fail(Status::Decode, "the Base64 text contains no gzip data");
  return util::gzip_decompress(compressed, max_output);
}

// --------------------------------------------------------------------------- registration

void register_base_n(Registry& registry) {
  // ---- Base32 ----------------------------------------------------------------
  const auto encode_base32 = [](Base32Alphabet alphabet) {
    return [alphabet](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
      return base32_encode(blob.bytes, {.alphabet = alphabet,
                                        .padding = ctx.options.get_bool("padding", true),
                                        .line_width = line_width_of(ctx.options, 0)});
    };
  };
  const auto decode_base32 = [](Base32Alphabet alphabet) {
    return [alphabet](std::string_view text, const ReadContext&) { return base32_decode(text, alphabet); };
  };
  add_binary_encoding(registry, {
      .id = "base32", .name = "Base32 text", .description = "Binary data as the letters A-Z and digits 2-7 (RFC 4648).",
      .extensions = {"b32", "base32"}, .popularity = 30,
      .encode = encode_base32(Base32Alphabet::Standard), .decode = decode_base32(Base32Alphabet::Standard),
      .encode_options = {padding_option(), line_width_option(0)},
      .expansion = 4,  // 8 characters per 5 bytes, doubled by one-character lines
      .chunk = base32_chunk,
  });
  add_binary_encoding(registry, {
      .id = "base32hex", .name = "Base32 text (extended hex alphabet)",
      .description = "Base32 with the digits 0-9 and letters A-V, which keeps the sort order of the data (RFC 4648).",
      .extensions = {"b32hex", "base32hex"}, .popularity = 10,
      .encode = encode_base32(Base32Alphabet::Hex), .decode = decode_base32(Base32Alphabet::Hex),
      .encode_options = {padding_option(), line_width_option(0)},
      .expansion = 4,
      .chunk = base32_chunk,
  });
  add_binary_encoding(registry, {
      .id = "base32-crockford", .name = "Base32 text (Crockford alphabet)",
      .description = "Base32 without the easily confused letters I, L, O and U, for codes that people read aloud or type.",
      .extensions = {"b32crockford", "crockford"}, .popularity = 12,
      .encode = [](const ir::Blob& blob, const WriteContext&) -> Result<std::string> {
        return base32_encode(blob.bytes, {.alphabet = Base32Alphabet::Crockford, .padding = false});
      },
      .decode = decode_base32(Base32Alphabet::Crockford),
      .expansion = 2,
  });

  // ---- Base58 / Base36 -------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "base58", .name = "Base58 text",
      .description = "The Bitcoin alphabet: letters and digits without 0, O, I and l; for files up to 64 KiB.",
      .extensions = {"b58", "base58"}, .popularity = 25,
      .encode = [](const ir::Blob& blob, const WriteContext&) { return base58_encode(blob.bytes); },
      .decode = [](std::string_view text, const ReadContext&) { return base58_decode(text); },
      .expansion = 2,
  });
  add_binary_encoding(registry, {
      .id = "base36", .name = "Base36 text",
      .description = "The file as one big number written with the digits 0-9 and letters a-z; for files up to 64 KiB.",
      .extensions = {"b36", "base36"}, .popularity = 10,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) {
        return base36_encode(blob.bytes, ctx.options.get_bool("uppercase", false));
      },
      .decode = [](std::string_view text, const ReadContext&) { return base36_decode(text); },
      .encode_options = {OptionSpec::boolean("uppercase", "Uppercase letters", false, "Write the letters A-Z in upper case.")},
      .expansion = 2,
  });

  // ---- Ascii85 / Z85 ------------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "ascii85", .name = "Ascii85 text",
      .description = "Adobe's Base85 encoding as used in PostScript and PDF: five characters for four bytes.",
      .extensions = {"a85", "ascii85"}, .popularity = 25,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        return ascii85_encode(blob.bytes, {.wrapper = ctx.options.get_bool("wrapper", true),
                                           .line_width = line_width_of(ctx.options, 0)});
      },
      .decode = [](std::string_view text, const ReadContext&) { return ascii85_decode(text); },
      .encode_options = {OptionSpec::boolean("wrapper", "Delimiters", true,
                                             "Surround the data with the <~ and ~> markers."),
                         line_width_option(0)},
      .expansion = 3,  // 5 characters per 4 bytes, doubled by one-character lines
  });
  add_binary_encoding(registry, {
      .id = "z85", .name = "Z85 text",
      .description = "ZeroMQ's Base85 variant, safe inside source code and JSON; the size must be a multiple of 4 bytes.",
      .extensions = {"z85"}, .popularity = 10,
      .encode = [](const ir::Blob& blob, const WriteContext&) { return z85_encode(blob.bytes); },
      .decode = [](std::string_view text, const ReadContext&) { return z85_decode(text); },
      .expansion = 2,
  });

  // ---- gzip + Base64 --------------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "gzip-base64", .name = "Gzip-compressed Base64 text",
      .description = "The file compressed with gzip and then written as Base64.",
      .extensions = {"gz.b64", "gzb64"}, .popularity = 20,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        CVT_TRY(ctx.checkpoint(0.0, "Compressing"));
        return gzip_base64_encode(blob.bytes, options::resolved_compression_level(ctx.options),
                                  line_width_of(ctx.options, 76));
      },
      .decode = [](std::string_view text, const ReadContext& ctx) {
        return gzip_base64_decode(text, ctx.limits.max_decompressed_bytes);
      },
      .encode_options = {line_width_option(76), options::compression_level()},
      .expansion = 3,  // incompressible data: 4 characters per 3 bytes, doubled by one-character lines
  });
}

}  // namespace cvt::encoding
