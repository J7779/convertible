// Convertible core - encoding family: punycode (RFC 3492) for internationalised names.
//
// Adding a related transform (for example another ASCII-compatible encoding
// of labels): write the label functions next to `punycode_encode_label`,
// reuse `transform_labels` to apply them to running text, declare the result
// in encoding.hpp and add one `add_text_transform` block to
// `register_punycode` at the end of this file.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "engine/registry.hpp"
#include "families/encoding/detail.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

// Parameters of RFC 3492 section 5.
constexpr std::uint32_t base = 36;
constexpr std::uint32_t t_min = 1;
constexpr std::uint32_t t_max = 26;
constexpr std::uint32_t skew = 38;
constexpr std::uint32_t damp = 700;
constexpr std::uint32_t initial_bias = 72;
constexpr std::uint32_t initial_n = 0x80;
constexpr std::uint32_t max_value = std::numeric_limits<std::uint32_t>::max();

constexpr std::string_view ace_prefix = "xn--";

constexpr char encode_digit(std::uint32_t digit) noexcept {
  return static_cast<char>(digit < 26 ? 'a' + digit : '0' + (digit - 26));
}

// 0..35, or `base` for a character that is not a digit.
constexpr std::uint32_t decode_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c - '0') + 26u;
  if (c >= 'a' && c <= 'z') return static_cast<std::uint32_t>(c - 'a');
  if (c >= 'A' && c <= 'Z') return static_cast<std::uint32_t>(c - 'A');
  return base;
}

constexpr std::uint32_t threshold(std::uint32_t k, std::uint32_t bias) noexcept {
  return k <= bias ? t_min : k >= bias + t_max ? t_max : k - bias;
}

constexpr std::uint32_t adapt(std::uint32_t delta, std::uint32_t point_count, bool first_time) noexcept {
  delta = first_time ? delta / damp : delta / 2;
  delta += delta / point_count;
  std::uint32_t k = 0;
  while (delta > ((base - t_min) * t_max) / 2) {
    delta /= base - t_min;
    k += base;
  }
  return k + (base - t_min + 1) * delta / (delta + skew);
}

// The characters a label is made of: letters, digits, hyphens and anything
// that is not ASCII. Everything else separates labels.
constexpr bool is_label_char(char c) noexcept {
  return detail::byte_of(c) >= 0x80 || util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '-';
}

// Copy `text`, replacing every label for which `convert` has a replacement.
// `convert(label, out)` appends the replacement and returns true, or returns
// false to keep the label as it is.
template <class Convert>
Result<std::string> transform_labels(std::string_view text, Convert convert) {
  std::string out;
  out.reserve(text.size() + text.size() / 4);
  std::size_t i = 0;
  while (i < text.size()) {
    if (!is_label_char(text[i])) {
      out.push_back(text[i++]);
      continue;
    }
    std::size_t end = i;
    while (end < text.size() && is_label_char(text[end])) ++end;
    const std::string_view label = text.substr(i, end - i);
    CVT_TRY_ASSIGN(const bool replaced, convert(label, out));
    if (!replaced) out.append(label);
    i = end;
  }
  return out;
}

}  // namespace

Result<std::string> punycode_encode_label(std::u32string_view label) {
  if (label.size() > punycode_max_label) {
    return fail(Status::Limit, "a label of {} characters is too long for punycode (the limit is {})", label.size(),
                punycode_max_label);
  }
  std::string out;
  out.reserve(label.size() * 2 + 1);
  for (const char32_t cp : label) {
    if (cp < initial_n) out.push_back(static_cast<char>(cp));
    if (!util::is_scalar_value(cp)) return fail(Status::Encode, "the label contains an invalid code point");
  }
  const auto basic_count = static_cast<std::uint32_t>(out.size());
  const auto total = static_cast<std::uint32_t>(label.size());
  if (basic_count > 0) out.push_back('-');

  std::uint32_t n = initial_n;
  std::uint32_t delta = 0;
  std::uint32_t bias = initial_bias;
  std::uint32_t handled = basic_count;
  while (handled < total) {
    // The smallest code point not handled yet.
    std::uint32_t next = max_value;
    for (const char32_t cp : label) {
      if (cp >= n && cp < next) next = static_cast<std::uint32_t>(cp);
    }
    const std::uint64_t increase = static_cast<std::uint64_t>(next - n) * (handled + 1);
    if (increase > max_value - delta) return fail(Status::Encode, "the label is too long for punycode");
    delta += static_cast<std::uint32_t>(increase);
    n = next;
    for (const char32_t cp : label) {
      if (cp < n && ++delta == 0) return fail(Status::Encode, "the label is too long for punycode");
      if (cp != n) continue;
      std::uint32_t q = delta;
      for (std::uint32_t k = base;; k += base) {
        const std::uint32_t t = threshold(k, bias);
        if (q < t) break;
        out.push_back(encode_digit(t + (q - t) % (base - t)));
        q = (q - t) / (base - t);
      }
      out.push_back(encode_digit(q));
      bias = adapt(delta, handled + 1, handled == basic_count);
      delta = 0;
      ++handled;
    }
    ++delta;
    ++n;
  }
  return out;
}

Result<std::u32string> punycode_decode_label(std::string_view label) {
  if (label.size() > punycode_max_label) {
    return fail(Status::Limit, "a punycode label of {} characters is too long (the limit is {})", label.size(),
                punycode_max_label);
  }
  std::u32string out;
  out.reserve(label.size());
  // Everything before the last hyphen is copied literally.
  const std::size_t delimiter = label.rfind('-');
  std::size_t position = 0;
  if (delimiter != std::string_view::npos && delimiter > 0) {
    for (std::size_t i = 0; i < delimiter; ++i) {
      if (detail::byte_of(label[i]) >= 0x80) return fail(Status::Decode, "punycode must be ASCII");
      out.push_back(static_cast<char32_t>(detail::byte_of(label[i])));
    }
    position = delimiter + 1;
  }

  std::uint32_t n = initial_n;
  std::uint32_t i = 0;
  std::uint32_t bias = initial_bias;
  while (position < label.size()) {
    const std::uint32_t old_i = i;
    std::uint32_t weight = 1;
    for (std::uint32_t k = base;; k += base) {
      if (position >= label.size()) return fail(Status::Decode, "the punycode label is truncated");
      const std::uint32_t digit = decode_digit(label[position++]);
      if (digit >= base) return fail(Status::Decode, "invalid punycode digit at offset {}", position - 1);
      if (digit > (max_value - i) / weight) return fail(Status::Decode, "the punycode label overflows");
      i += digit * weight;
      const std::uint32_t t = threshold(k, bias);
      if (digit < t) break;
      if (static_cast<std::uint64_t>(weight) * (base - t) > max_value) {
        return fail(Status::Decode, "the punycode label overflows");
      }
      weight *= base - t;
    }
    const auto length = static_cast<std::uint32_t>(out.size()) + 1;
    bias = adapt(i - old_i, length, old_i == 0);
    if (i / length > max_value - n) return fail(Status::Decode, "the punycode label overflows");
    n += i / length;
    i %= length;
    if (!util::is_scalar_value(static_cast<char32_t>(n))) {
      return fail(Status::Decode, "the punycode label decodes to an invalid code point");
    }
    out.insert(out.begin() + static_cast<std::ptrdiff_t>(i), static_cast<char32_t>(n));
    ++i;
  }
  return out;
}

Result<std::string> punycode_encode_text(std::string_view text) {
  return transform_labels(text, [](std::string_view label, std::string& out) -> Result<bool> {
    bool ascii = true;
    for (const char c : label) ascii = ascii && detail::byte_of(c) < 0x80;
    if (ascii) return false;
    // Names are case-insensitive: ASCII letters are folded, as IDNA does.
    CVT_TRY_ASSIGN(const std::string encoded, punycode_encode_label(util::utf8_to_utf32(util::to_lower(label))));
    out.append(ace_prefix);
    out.append(encoded);
    return true;
  });
}

Result<std::string> punycode_decode_text(std::string_view text) {
  return transform_labels(text, [](std::string_view label, std::string& out) -> Result<bool> {
    if (!util::istarts_with(label, ace_prefix)) return false;
    const std::string_view encoded = label.substr(ace_prefix.size());
    if (encoded.empty()) return fail(Status::Decode, "the label '{}' has no punycode after its prefix", label);
    auto decoded = punycode_decode_label(encoded);
    if (!decoded) {
      const std::string_view shown = label.substr(0, 64);
      return fail(decoded.error().code, "cannot decode the label '{}': {}", util::sanitize_utf8(shown),
                  decoded.error().message);
    }
    out.append(util::utf32_to_utf8(*decoded));
    return true;
  });
}

void register_punycode(Registry& registry) {
  add_text_transform(registry, {
      .id = "punycode", .name = "Punycode text",
      .description = "Internationalised domain names written in ASCII, such as xn--mnchen-3ya for m\u00fcnchen.",
      .extensions = {"punycode", "puny"}, .popularity = 15,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        CVT_TRY(detail::check_expansion(ctx, text.size(), 5));
        return punycode_encode_text(text);
      },
      .decode = [](std::string_view text, const ReadContext&) { return punycode_decode_text(text); },
      // Upper-case ASCII letters in the converted labels are folded to lower case.
      .lossy = true,
  });
}

}  // namespace cvt::encoding
