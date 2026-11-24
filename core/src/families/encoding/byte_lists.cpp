// Convertible core - encoding family: lists of byte values (octal, decimal).
//
// Adding another number base: write `foo_text` / `parse_foo_text` with the
// two helpers below (declare them in encoding.hpp) and add one
// `add_binary_encoding` block to `register_byte_lists` at the end of this file.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "engine/registry.hpp"
#include "families/encoding/detail.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

using namespace std::string_view_literals;

// Byte values in `base`, zero-padded to `min_digits`, joined by `separator`.
// A separator that ends in a line break also ends the text.
std::string byte_list(ByteView data, unsigned base, std::size_t min_digits, std::string_view separator) {
  std::string out;
  out.reserve(data.size() * (3 + separator.size()));
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (i > 0) out.append(separator);
    unsigned value = detail::byte_at(data, i);
    char digits[3];
    std::size_t count = 0;
    do {
      digits[count++] = static_cast<char>('0' + value % base);
      value /= base;
    } while (value != 0);
    while (count < min_digits) digits[count++] = '0';
    while (count > 0) out.push_back(digits[--count]);
  }
  if (!data.empty() && separator.ends_with('\n')) out.push_back('\n');
  return out;
}

Result<Bytes> parse_byte_list(std::string_view text, unsigned base, std::string_view name) {
  Bytes out;
  out.reserve(text.size() / 3);
  unsigned value = 0;
  bool in_number = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (util::is_ascii_space(c) || c == ',') {
      if (in_number) out.push_back(static_cast<std::byte>(value));
      value = 0;
      in_number = false;
      continue;
    }
    if (c < '0' || static_cast<unsigned>(c - '0') >= base) {
      return fail(Status::Decode, "invalid {} digit at offset {}", name, i);
    }
    value = value * base + static_cast<unsigned>(c - '0');
    // Checked after every digit, so the accumulator cannot overflow.
    if (value > 255) return fail(Status::Decode, "the {} value ending at offset {} is larger than 255", name, i);
    in_number = true;
  }
  if (in_number) out.push_back(static_cast<std::byte>(value));
  return out;
}

OptionSpec separator_option(std::string default_choice) {
  return OptionSpec::choice("separator", "Separator", std::move(default_choice), {"space", "comma", "newline"},
                            "What is written between two byte values.");
}

std::string_view separator_of(const Options& options, std::string_view fallback) {
  const std::string choice = options.get_enum("separator", fallback, {"space", "comma", "newline"});
  return choice == "comma" ? ", "sv : choice == "newline" ? "\n"sv : " "sv;
}

}  // namespace

std::string octal_text(ByteView data, std::string_view separator) { return byte_list(data, 8, 3, separator); }

std::string decimal_text(ByteView data, std::string_view separator) { return byte_list(data, 10, 1, separator); }

Result<Bytes> parse_octal_text(std::string_view text) { return parse_byte_list(text, 8, "octal"); }

Result<Bytes> parse_decimal_text(std::string_view text) { return parse_byte_list(text, 10, "decimal"); }

void register_byte_lists(Registry& registry) {
  add_binary_encoding(registry, {
      .id = "octal", .name = "Octal byte values", .description = "Every byte as three octal digits.",
      .extensions = {"oct", "octal"}, .popularity = 10,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        return octal_text(blob.bytes, separator_of(ctx.options, "space"));
      },
      .decode = [](std::string_view text, const ReadContext&) { return parse_octal_text(text); },
      .encode_options = {separator_option("space")},
      .expansion = 5,  // three digits, comma, space
  });
  add_binary_encoding(registry, {
      .id = "decimal", .name = "Decimal byte values", .description = "Every byte as a decimal number from 0 to 255.",
      .extensions = {"dec", "decimal"}, .popularity = 12,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        return decimal_text(blob.bytes, separator_of(ctx.options, "comma"));
      },
      .decode = [](std::string_view text, const ReadContext&) { return parse_decimal_text(text); },
      .encode_options = {separator_option("comma")},
      .expansion = 5,
  });
}

}  // namespace cvt::encoding
