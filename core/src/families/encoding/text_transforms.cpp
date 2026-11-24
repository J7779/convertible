// Convertible core - encoding family: text-to-text transforms
// (percent-encoded text, Unicode escapes, JSON string literal, ROT13, ROT47).
//
// Adding another transform: write `foo_encode` (and `foo_decode` when it can
// be reversed) as functions over UTF-8 text, declare them in encoding.hpp,
// and add one `add_text_transform` block to `register_text_transforms` at the
// end of this file. A transform that is its own inverse registers the same
// function for both directions (see rot13).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/registry.hpp"
#include "families/encoding/detail.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

using detail::append_hex;
using detail::append_hex_byte;
using detail::byte_of;

constexpr bool is_url_unreserved(char c) noexcept {
  return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

constexpr bool is_url_reserved(char c) noexcept {
  constexpr std::string_view reserved = ":/?#[]@!$&'()*+,;=";
  return reserved.find(c) != std::string_view::npos;
}

constexpr bool is_high_surrogate(std::uint32_t value) noexcept { return value >= 0xD800 && value <= 0xDBFF; }
constexpr bool is_low_surrogate(std::uint32_t value) noexcept { return value >= 0xDC00 && value <= 0xDFFF; }

// "\uXXXX", as a surrogate pair above U+FFFF.
void append_u4(std::string& out, char32_t cp) {
  const auto unit = [&out](std::uint32_t value) {
    out.append("\\u");
    append_hex(out, value, 4, false);
  };
  if (cp < 0x10000) {
    unit(static_cast<std::uint32_t>(cp));
  } else {
    const std::uint32_t offset = static_cast<std::uint32_t>(cp) - 0x10000u;
    unit(0xD800u + (offset >> 10));
    unit(0xDC00u + (offset & 0x3FFu));
  }
}

// `count` hexadecimal digits starting at `position`, or -1.
std::int64_t read_hex(std::string_view text, std::size_t position, std::size_t count) {
  if (position > text.size() || count > text.size() - position) return -1;
  std::int64_t value = 0;
  for (std::size_t k = 0; k < count; ++k) {
    const int digit = util::hex_digit_value(text[position + k]);
    if (digit < 0) return -1;
    value = value * 16 + digit;
  }
  return value;
}

// One code point written in the numeric form of `style`; `utf8` holds its bytes.
void append_numeric_escape(std::string& out, char32_t cp, std::string_view utf8, UnicodeEscapeStyle style) {
  switch (style) {
    case UnicodeEscapeStyle::U4: append_u4(out, cp); break;
    case UnicodeEscapeStyle::UBraces:
      out.append("\\u{");
      append_hex(out, static_cast<std::uint32_t>(cp), 1, false);
      out.push_back('}');
      break;
    case UnicodeEscapeStyle::XBytes:
      for (const char c : utf8) {
        out.append("\\x");
        append_hex_byte(out, byte_of(c), false);
      }
      break;
    case UnicodeEscapeStyle::Python:
      if (cp < 0x10000) {
        append_u4(out, cp);
      } else {
        out.append("\\U");
        append_hex(out, static_cast<std::uint32_t>(cp), 8, false);
      }
      break;
  }
}

UnicodeEscapeStyle escape_style_of(const Options& options) {
  const std::string style = options.get_enum("style", "u4", {"u4", "u-braces", "x-bytes", "python"});
  if (style == "u-braces") return UnicodeEscapeStyle::UBraces;
  if (style == "x-bytes") return UnicodeEscapeStyle::XBytes;
  if (style == "python") return UnicodeEscapeStyle::Python;
  return UnicodeEscapeStyle::U4;
}

}  // namespace

// --------------------------------------------------------------------------- percent-encoded text

std::string url_text_encode(std::string_view text, bool component) {
  std::string out;
  out.reserve(text.size() * 3);
  for (const char c : text) {
    if (is_url_unreserved(c) || (!component && is_url_reserved(c))) {
      out.push_back(c);
    } else {
      out.push_back('%');
      append_hex_byte(out, byte_of(c), true);
    }
  }
  return out;
}

Result<std::string> url_text_decode(std::string_view text, bool plus_as_space) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '%') {
      const std::int64_t value = read_hex(text, i + 1, 2);
      if (value < 0) return fail(Status::Decode, "invalid percent escape at offset {}", i);
      out.push_back(static_cast<char>(static_cast<std::uint8_t>(value)));
      i += 2;
    } else if (c == '+' && plus_as_space) {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return util::sanitize_utf8(out);
}

// --------------------------------------------------------------------------- Unicode escapes

std::string unicode_escape(std::string_view text, UnicodeEscapeStyle style, bool escape_ascii_controls) {
  std::string out;
  out.reserve(text.size() + text.size() / 4);
  for (std::size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (byte_of(c) < 0x80) {
      const bool control = byte_of(c) < 0x20 || c == 0x7F;
      if (c == '\\') {
        out.append("\\\\");  // always, or the text could not be decoded again
      } else if (!control || !escape_ascii_controls) {
        out.push_back(c);
      } else if (c == '\n') {
        out.append("\\n");
      } else if (c == '\r') {
        out.append("\\r");
      } else if (c == '\t') {
        out.append("\\t");
      } else {
        append_numeric_escape(out, static_cast<char32_t>(byte_of(c)), text.substr(i, 1), style);
      }
      ++i;
      continue;
    }
    const util::Utf8Decoded decoded = util::decode_utf8(text, i);
    if (decoded.ok) {
      append_numeric_escape(out, decoded.code_point, text.substr(i, decoded.length), style);
    } else {
      append_numeric_escape(out, util::replacement_character, "\xEF\xBF\xBD", style);
    }
    i += decoded.length;
  }
  return out;
}

Result<std::string> unicode_unescape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '\\') {
      out.push_back(text[i++]);
      continue;
    }
    if (i + 1 >= text.size()) return fail(Status::Decode, "the text ends inside an escape sequence");
    const std::size_t start = i;
    const char kind = text[i + 1];
    i += 2;
    switch (kind) {
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'v': out.push_back('\v'); break;
      case '0': out.push_back('\0'); break;
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      case '\'': out.push_back('\''); break;
      case '/': out.push_back('/'); break;
      case 'x': {
        // Consecutive \xNN escapes spell UTF-8 bytes; when they do not form
        // valid UTF-8, each one is taken as a Latin-1 character instead.
        std::string run;
        i = start;
        while (i + 1 < text.size() && text[i] == '\\' && text[i + 1] == 'x') {
          const std::int64_t value = read_hex(text, i + 2, 2);
          if (value < 0) break;
          run.push_back(static_cast<char>(static_cast<std::uint8_t>(value)));
          i += 4;
        }
        if (run.empty()) return fail(Status::Decode, "invalid \\x escape at offset {}", start);
        if (util::is_valid_utf8(run)) {
          out.append(run);
        } else {
          for (const char c : run) util::append_utf8(out, static_cast<char32_t>(byte_of(c)));
        }
        break;
      }
      case 'u': {
        if (i < text.size() && text[i] == '{') {
          const std::size_t close = text.find('}', i);
          const std::size_t digits = close == std::string_view::npos ? 0 : close - i - 1;
          const std::int64_t value = digits >= 1 && digits <= 6 ? read_hex(text, i + 1, digits) : -1;
          if (value < 0 || value > 0x10FFFF) return fail(Status::Decode, "invalid \\u{{...}} escape at offset {}", start);
          util::append_utf8(out, static_cast<char32_t>(value));
          i = close + 1;
          break;
        }
        const std::int64_t unit = read_hex(text, i, 4);
        if (unit < 0) return fail(Status::Decode, "invalid \\u escape at offset {}", start);
        i += 4;
        auto cp = static_cast<std::uint32_t>(unit);
        if (is_high_surrogate(cp) && i + 1 < text.size() && text[i] == '\\' && text[i + 1] == 'u') {
          const std::int64_t low = read_hex(text, i + 2, 4);
          if (low >= 0 && is_low_surrogate(static_cast<std::uint32_t>(low))) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (static_cast<std::uint32_t>(low) - 0xDC00u);
            i += 6;
          }
        }
        util::append_utf8(out, static_cast<char32_t>(cp));  // a lone surrogate becomes U+FFFD
        break;
      }
      case 'U': {
        const std::int64_t value = read_hex(text, i, 8);
        if (value < 0 || value > 0x10FFFF) return fail(Status::Decode, "invalid \\U escape at offset {}", start);
        util::append_utf8(out, static_cast<char32_t>(value));
        i += 8;
        break;
      }
      default: return fail(Status::Decode, "unknown escape sequence at offset {}", start);
    }
  }
  return out;
}

// --------------------------------------------------------------------------- JSON string literal

std::string json_string_encode(std::string_view text, bool ascii_only) {
  std::string out;
  out.reserve(text.size() + text.size() / 8 + 2);
  out.push_back('"');
  for (std::size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (byte_of(c) < 0x80) {
      switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\b': out.append("\\b"); break;
        case '\f': out.append("\\f"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
          // DEL is legal in JSON, but "ASCII only" is asked for to get printable output.
          if (byte_of(c) < 0x20 || (ascii_only && c == 0x7F)) {
            append_u4(out, static_cast<char32_t>(byte_of(c)));
          } else {
            out.push_back(c);
          }
          break;
      }
      ++i;
      continue;
    }
    const util::Utf8Decoded decoded = util::decode_utf8(text, i);
    if (!decoded.ok) {
      // JSON must be valid Unicode.
      if (ascii_only) {
        append_u4(out, util::replacement_character);
      } else {
        util::append_utf8(out, util::replacement_character);
      }
    } else if (ascii_only) {
      append_u4(out, decoded.code_point);
    } else {
      out.append(text, i, decoded.length);
    }
    i += decoded.length;
  }
  out.push_back('"');
  return out;
}

Result<std::string> json_string_decode(std::string_view text) {
  const auto is_json_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (!text.empty() && is_json_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_json_space(text.back())) text.remove_suffix(1);
  if (text.empty() || text.front() != '"') {
    return fail(Status::Decode, "a JSON string literal must start with a double quote");
  }
  std::string out;
  out.reserve(text.size());
  std::size_t i = 1;
  for (;;) {
    if (i >= text.size()) return fail(Status::Decode, "the JSON string literal is not closed");
    const char c = text[i];
    if (c == '"') {
      if (i + 1 != text.size()) return fail(Status::Decode, "unexpected data after the JSON string at offset {}", i + 1);
      return out;
    }
    if (byte_of(c) < 0x20) return fail(Status::Decode, "unescaped control character in the JSON string at offset {}", i);
    if (c != '\\') {
      const util::Utf8Decoded decoded = util::decode_utf8(text, i);
      if (!decoded.ok) return fail(Status::Decode, "invalid UTF-8 in the JSON string at offset {}", i);
      out.append(text, i, decoded.length);
      i += decoded.length;
      continue;
    }
    if (i + 1 >= text.size()) return fail(Status::Decode, "the JSON string literal is not closed");
    const std::size_t start = i;
    const char kind = text[i + 1];
    i += 2;
    switch (kind) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'u': {
        const std::int64_t unit = read_hex(text, i, 4);
        if (unit < 0) return fail(Status::Decode, "invalid \\u escape in the JSON string at offset {}", start);
        i += 4;
        auto cp = static_cast<std::uint32_t>(unit);
        if (is_low_surrogate(cp)) return fail(Status::Decode, "unpaired surrogate in the JSON string at offset {}", start);
        if (is_high_surrogate(cp)) {
          const std::int64_t low = i + 1 < text.size() && text[i] == '\\' && text[i + 1] == 'u' ? read_hex(text, i + 2, 4) : -1;
          if (low < 0 || !is_low_surrogate(static_cast<std::uint32_t>(low))) {
            return fail(Status::Decode, "unpaired surrogate in the JSON string at offset {}", start);
          }
          cp = 0x10000u + ((cp - 0xD800u) << 10) + (static_cast<std::uint32_t>(low) - 0xDC00u);
          i += 6;
        }
        util::append_utf8(out, static_cast<char32_t>(cp));
        break;
      }
      default: return fail(Status::Decode, "invalid escape sequence in the JSON string at offset {}", start);
    }
  }
}

// --------------------------------------------------------------------------- rotations

std::string rot13(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>('a' + (c - 'a' + 13) % 26);
    if (c >= 'A' && c <= 'Z') c = static_cast<char>('A' + (c - 'A' + 13) % 26);
  }
  return out;
}

std::string rot47(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= '!' && c <= '~') c = static_cast<char>('!' + (c - '!' + 47) % 94);
  }
  return out;
}

// --------------------------------------------------------------------------- registration

void register_text_transforms(Registry& registry) {
  // ---- percent-encoded text ---------------------------------------------------
  add_text_transform(registry, {
      .id = "url-text", .name = "Percent-encoded text",
      .description = "Text made safe for use inside a URL by writing special characters as %XX.",
      .extensions = {"urltext"}, .popularity = 25,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        CVT_TRY(detail::check_expansion(ctx, text.size(), 3));
        return url_text_encode(text, ctx.options.get_bool("component", true));
      },
      .decode = [](std::string_view text, const ReadContext& ctx) {
        return url_text_decode(text, ctx.options.get_bool("plus_as_space", false));
      },
      .encode_options = {OptionSpec::boolean(
          "component", "Encode reserved characters", true,
          "Also escape the characters that structure a URL, such as / ? & and =; turn off to encode a whole URL.")},
      .decode_options = {OptionSpec::boolean("plus_as_space", "+ means space", false,
                                             "Treat '+' in the input as a space (form encoding).")},
  });

  // ---- Unicode escapes -----------------------------------------------------------
  add_text_transform(registry, {
      .id = "unicode-escapes", .name = "Unicode-escaped text",
      .description = "ASCII-only text in which other characters are written as escapes such as \\u00e9.",
      .extensions = {"uescape"}, .popularity = 20,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        // Worst case: one byte becomes six ("\u0001") or, per UTF-8 byte, four ("\xNN").
        CVT_TRY(detail::check_expansion(ctx, text.size(), 6));
        return unicode_escape(text, escape_style_of(ctx.options), ctx.options.get_bool("escape_ascii_controls", true));
      },
      .decode = [](std::string_view text, const ReadContext&) { return unicode_unescape(text); },
      .encode_options = {OptionSpec::choice("style", "Escape style", "u4", {"u4", "u-braces", "x-bytes", "python"},
                                            "Notation: \\uXXXX with surrogate pairs, \\u{...}, \\xNN per UTF-8 byte, "
                                            "or Python's \\uXXXX and \\UXXXXXXXX."),
                         OptionSpec::boolean("escape_ascii_controls", "Escape control characters", true,
                                             "Also write line breaks, tabs and other control characters as escapes.")},
  });

  // ---- JSON string literal ----------------------------------------------------------
  add_text_transform(registry, {
      .id = "json-string", .name = "JSON string literal",
      .description = "The whole text as one quoted and escaped JSON string.",
      .extensions = {"jsonstr"}, .mime = "application/json", .popularity = 25,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        CVT_TRY(detail::check_expansion(ctx, text.size(), 6));
        return json_string_encode(text, ctx.options.get_bool("ascii_only", false));
      },
      .decode = [](std::string_view text, const ReadContext&) { return json_string_decode(text); },
      .encode_options = {OptionSpec::boolean("ascii_only", "ASCII only", false,
                                             "Write every non-ASCII character as a \\uXXXX escape.")},
  });

  // ---- rotations ------------------------------------------------------------------------
  add_text_transform(registry, {
      .id = "rot13", .name = "ROT13 text", .description = "Every letter replaced by the one 13 places further on.",
      .extensions = {"rot13"}, .popularity = 15,
      .encode = [](std::string_view text, const WriteContext&) -> Result<std::string> { return rot13(text); },
      .decode = [](std::string_view text, const ReadContext&) -> Result<std::string> { return rot13(text); },
  });
  add_text_transform(registry, {
      .id = "rot47", .name = "ROT47 text",
      .description = "Every printable ASCII character replaced by the one 47 places further on.",
      .extensions = {"rot47"}, .popularity = 8,
      .encode = [](std::string_view text, const WriteContext&) -> Result<std::string> { return rot47(text); },
      .decode = [](std::string_view text, const ReadContext&) -> Result<std::string> { return rot47(text); },
  });
}

}  // namespace cvt::encoding
