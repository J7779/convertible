// Convertible core - encoding family: HTML character references.
//
// The named references are the complete HTML5 list (html_entities.inc,
// written by unicode_gen.py from Python's html.entities.html5).
// Adding another markup-escaping transform: write `foo_encode` / `foo_decode`
// (declare them in encoding.hpp) and add one `add_text_transform` block to
// `register_html_entities` at the end of this file.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
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

struct NamedEntity {
  std::string_view name;  ///< with the trailing ';' where HTML5 requires one
  char32_t first;
  char32_t second;        ///< 0, or the second code point of a two-character reference
};
struct PreferredName {
  char32_t code_point;
  std::string_view name;
};

// All named character references of HTML5 and the name used when writing.
#include "families/encoding/html_entities.inc"

constexpr bool entities_are_sorted() noexcept {
  for (std::size_t i = 1; i < html5_entities.size(); ++i) {
    if (!(html5_entities[i - 1].name < html5_entities[i].name)) return false;
  }
  return true;
}
static_assert(entities_are_sorted(), "html5_entities must be sorted by name for the binary search");

const NamedEntity* find_entity(std::string_view name) {
  const auto found = std::ranges::lower_bound(html5_entities, name, {}, &NamedEntity::name);
  return found != html5_entities.end() && found->name == name ? &*found : nullptr;
}

std::string_view preferred_name(char32_t cp) {
  const auto found = std::ranges::lower_bound(preferred_names, cp, {}, &PreferredName::code_point);
  return found != preferred_names.end() && found->code_point == cp ? found->name : std::string_view{};
}

// Numeric references to 0x80..0x9F mean Windows-1252 characters in HTML
// (0 = no mapping, the code point is kept).
constexpr std::array<char16_t, 32> c1_replacements = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

// Longest reference body that is considered: HTML5 names are at most 32
// characters with the ';', numbers may carry leading zeros.
constexpr std::size_t max_reference_length = 32;

// The character a numeric reference body ("#38", "#x26") stands for.
std::optional<char32_t> resolve_reference(std::string_view body) {
  if (body.size() < 2 || body.front() != '#') return std::nullopt;
  body.remove_prefix(1);
  const bool hexadecimal = !body.empty() && (body.front() == 'x' || body.front() == 'X');
  if (hexadecimal) body.remove_prefix(1);
  if (body.empty()) return std::nullopt;
  std::uint32_t value = 0;
  for (const char c : body) {
    const int digit = hexadecimal ? util::hex_digit_value(c) : util::is_ascii_digit(c) ? c - '0' : -1;
    if (digit < 0) return std::nullopt;
    // Saturate: anything above U+10FFFF is equally invalid.
    value = std::min<std::uint32_t>(value * (hexadecimal ? 16u : 10u) + static_cast<std::uint32_t>(digit), 0x110000u);
  }
  if (value >= 0x80 && value <= 0x9F && c1_replacements[value - 0x80] != 0) return c1_replacements[value - 0x80];
  if (value == 0 || !util::is_scalar_value(value)) return util::replacement_character;
  return static_cast<char32_t>(value);
}

}  // namespace

std::string html_entities_encode(std::string_view text, bool non_ascii) { return html_entities_encode(text, non_ascii, false); }

std::string html_entities_encode(std::string_view text, bool non_ascii, bool named) {
  std::string out;
  out.reserve(text.size() + text.size() / 8);
  for (std::size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (detail::byte_of(c) < 0x80) {
      switch (c) {
        case '&': out.append("&amp;"); break;
        case '<': out.append("&lt;"); break;
        case '>': out.append("&gt;"); break;
        case '"': out.append("&quot;"); break;
        case '\'': out.append("&#39;"); break;
        default: out.push_back(c); break;
      }
      ++i;
      continue;
    }
    const util::Utf8Decoded decoded = util::decode_utf8(text, i);
    // A numeric reference to a C1 control means a Windows-1252 character in
    // HTML, so those few code points can only be written as themselves.
    const bool c1_control = decoded.code_point >= 0x80 && decoded.code_point <= 0x9F;
    const std::string_view name = named && decoded.ok ? preferred_name(decoded.code_point) : std::string_view{};
    if (!name.empty()) {
      out.push_back('&');
      out.append(name);
    } else if (non_ascii && decoded.ok && !c1_control) {
      out.append("&#x");
      detail::append_hex(out, static_cast<std::uint32_t>(decoded.code_point), 1, true);
      out.push_back(';');
    } else {
      out.append(text, i, decoded.length);
    }
    i += decoded.length;
  }
  return out;
}

std::string html_entities_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t ampersand = text.find('&', i);
    if (ampersand == std::string_view::npos) {
      out.append(text.substr(i));
      break;
    }
    out.append(text.substr(i, ampersand - i));
    const std::string_view candidate = text.substr(ampersand + 1, max_reference_length + 1);
    const std::size_t semicolon = candidate.find(';');
    const std::optional<char32_t> code_point =
        semicolon == std::string_view::npos ? std::nullopt : resolve_reference(candidate.substr(0, semicolon));
    if (code_point) {
      util::append_utf8(out, *code_point);
      i = ampersand + semicolon + 2;
      continue;
    }
    // Named references: the longest name that matches (HTML5 also knows a few
    // legacy names without the ';', such as "&copy" and "&amp").
    const NamedEntity* entity = nullptr;
    std::size_t length = 0;
    if (!candidate.empty() && candidate.front() != '#') {
      std::size_t limit = 0;
      while (limit < candidate.size() && limit < max_reference_length && candidate[limit] != ';' && candidate[limit] != '&' &&
             !util::is_ascii_space(candidate[limit]) && candidate[limit] != '<') {
        ++limit;
      }
      if (limit < candidate.size() && candidate[limit] == ';') entity = find_entity(candidate.substr(0, limit + 1));
      length = limit + 1;
      for (std::size_t k = limit; entity == nullptr && k >= 2; --k) {
        entity = find_entity(candidate.substr(0, k));
        length = k;
      }
    }
    if (entity != nullptr) {
      util::append_utf8(out, entity->first);
      if (entity->second != 0) util::append_utf8(out, entity->second);
      i = ampersand + 1 + length;
    } else {
      // Not a reference we know: the text is kept as it is.
      out.push_back('&');
      i = ampersand + 1;
    }
  }
  return out;
}

void register_html_entities(Registry& registry) {
  add_text_transform(registry, {
      .id = "html-entities", .name = "HTML-escaped text",
      .description = "Text with the characters that are special in HTML written as entities such as &amp;.",
      .extensions = {"htmlentities"}, .popularity = 30,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        // Worst case: one byte ('&') becomes five ("&amp;"); a named reference is at most 33 bytes for 2.
        CVT_TRY(detail::check_expansion(ctx, text.size(), ctx.options.get_bool("named", false) ? 17 : 5));
        return html_entities_encode(text, ctx.options.get_bool("non_ascii", false), ctx.options.get_bool("named", false));
      },
      .decode = [](std::string_view text, const ReadContext&) -> Result<std::string> {
        return html_entities_decode(text);
      },
      .encode_options = {OptionSpec::boolean("non_ascii", "Escape non-ASCII characters", false,
                                             "Write every character above U+007F as a numeric reference."),
                         OptionSpec::boolean("named", "Use entity names", false,
                                             "Write characters that have an HTML entity name with it (&eacute;, &rarr;).")},
  });
}

}  // namespace cvt::encoding
