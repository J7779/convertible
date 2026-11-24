// Convertible core - encoding family: Unicode normalisation, code point views
// and IDNA.
//
//   unicode-normalize  NFC / NFD / NFKC / NFKD (UAX #15), tables from unicode_gen.py
//   codepoints         text <-> "U+0048 U+0069" (or hex, decimal, HTML, CSS, JS notation)
//   utf8-bytes         one line per character with its UTF-8 bytes
//   idna               domain names <-> their ASCII (xn--) form, UTS #46 basics
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/encoding/unicode.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "families/encoding/text_tools.hpp"
#include "util/deflate.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

#include "families/encoding/unicode_tables.inc"

std::vector<std::uint32_t> inflate_words(std::span<const std::string_view> chunks, std::size_t count) {
  std::string text;
  for (const std::string_view chunk : chunks) text.append(chunk);
  std::vector<std::uint32_t> out;
  const auto compressed = util::hex_decode(text);
  if (!compressed) return out;
  const auto raw = util::zlib_decompress(*compressed, count * 4);
  if (!raw || raw->size() != count * 4) return out;
  out.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t v = 0;
    for (int b = 3; b >= 0; --b) v = (v << 8) | std::to_integer<std::uint32_t>((*raw)[4 * i + static_cast<std::size_t>(b)]);
    out[i] = v;
  }
  return out;
}

struct Decomposition {
  char32_t cp;
  bool compat;
  std::uint32_t offset;
  std::uint32_t length;
};

struct Data {
  std::vector<std::array<std::uint32_t, 3>> ccc;  // start, end, class
  std::vector<Decomposition> decompositions;      // sorted by cp
  std::vector<char32_t> pool;
  std::vector<std::pair<std::uint64_t, char32_t>> compositions;  // (first << 21 | second) -> composite, sorted
};

const Data& data() {
  static const Data instance = [] {
    Data d;
    const std::vector<std::uint32_t> ccc = inflate_words(ccc_z, ccc_count);
    for (std::size_t i = 0; i + 2 < ccc.size(); i += 3) d.ccc.push_back({ccc[i], ccc[i + 1], ccc[i + 2]});
    const std::vector<std::uint32_t> dec = inflate_words(decompositions_z, decompositions_count);
    for (std::size_t i = 0; i + 1 < dec.size();) {
      const std::uint32_t head = dec[i], length = dec[i + 1];
      if (i + 2 + length > dec.size()) break;
      d.decompositions.push_back({static_cast<char32_t>(head & 0x7FFFFFFFu), (head & 0x80000000u) != 0,
                                  static_cast<std::uint32_t>(d.pool.size()), length});
      for (std::uint32_t k = 0; k < length; ++k) d.pool.push_back(static_cast<char32_t>(dec[i + 2 + k]));
      i += 2 + length;
    }
    const std::vector<std::uint32_t> comp = inflate_words(compositions_z, compositions_count);
    for (std::size_t i = 0; i + 2 < comp.size(); i += 3) {
      d.compositions.emplace_back((std::uint64_t{comp[i]} << 21) | comp[i + 1], static_cast<char32_t>(comp[i + 2]));
    }
    std::ranges::sort(d.compositions);
    return d;
  }();
  return instance;
}

constexpr char32_t s_base = 0xAC00, l_base = 0x1100, v_base = 0x1161, t_base = 0x11A7;
constexpr char32_t l_count = 19, v_count = 21, t_count = 28, n_count = v_count * t_count, s_count = l_count * n_count;

std::uint32_t combining_class(char32_t cp) {
  const auto& ccc = data().ccc;
  const auto it = std::ranges::upper_bound(ccc, static_cast<std::uint32_t>(cp), {}, [](const auto& r) { return r[0]; });
  if (it == ccc.begin()) return 0;
  const auto& range = *std::prev(it);
  return cp <= range[1] ? range[2] : 0;
}

void decompose(char32_t cp, bool compat, std::u32string& out, int depth = 0) {
  if (cp >= s_base && cp < s_base + s_count) {
    const char32_t s = cp - s_base;
    out.push_back(l_base + s / n_count);
    out.push_back(v_base + (s % n_count) / t_count);
    if (s % t_count != 0) out.push_back(t_base + s % t_count);
    return;
  }
  const Data& d = data();
  const auto it = std::ranges::lower_bound(d.decompositions, cp, {}, &Decomposition::cp);
  if (depth < 32 && it != d.decompositions.end() && it->cp == cp && (compat || !it->compat)) {
    for (std::uint32_t k = 0; k < it->length; ++k) decompose(d.pool[it->offset + k], compat, out, depth + 1);
    return;
  }
  out.push_back(cp);
}

char32_t compose_pair(char32_t a, char32_t b) {
  if (a >= l_base && a < l_base + l_count && b >= v_base && b < v_base + v_count) {
    return s_base + ((a - l_base) * v_count + (b - v_base)) * t_count;
  }
  if (a >= s_base && a < s_base + s_count && (a - s_base) % t_count == 0 && b > t_base && b < t_base + t_count) {
    return a + (b - t_base);
  }
  const auto& comp = data().compositions;
  const std::uint64_t key = (std::uint64_t{a} << 21) | b;
  const auto it = std::ranges::lower_bound(comp, key, {}, &std::pair<std::uint64_t, char32_t>::first);
  return it != comp.end() && it->first == key ? it->second : 0;
}

}  // namespace

std::string_view unicode_data_version() noexcept { return unicode_version; }

std::string normalize(std::string_view text, NormalForm form) {
  const std::u32string input = util::utf8_to_utf32(util::sanitize_utf8(text));
  const bool compat = form == NormalForm::Nfkc || form == NormalForm::Nfkd;
  std::u32string seq;
  seq.reserve(input.size() + input.size() / 4);
  for (const char32_t cp : input) decompose(cp, compat, seq);
  // Canonical ordering: stable sort of every run of non-starters by combining class.
  std::vector<std::uint32_t> classes(seq.size());
  for (std::size_t i = 0; i < seq.size(); ++i) classes[i] = combining_class(seq[i]);
  for (std::size_t i = 0; i < seq.size();) {
    if (classes[i] == 0) {
      ++i;
      continue;
    }
    std::size_t end = i;
    while (end < seq.size() && classes[end] != 0) ++end;
    if (end - i > 1) {
      std::vector<std::pair<std::uint32_t, char32_t>> run;
      for (std::size_t k = i; k < end; ++k) run.emplace_back(classes[k], seq[k]);
      std::ranges::stable_sort(run, {}, &std::pair<std::uint32_t, char32_t>::first);
      for (std::size_t k = i; k < end; ++k) {
        classes[k] = run[k - i].first;
        seq[k] = run[k - i].second;
      }
    }
    i = end;
  }
  if ((form == NormalForm::Nfc || form == NormalForm::Nfkc) && !seq.empty()) {
    // Canonical composition (UAX #15, section 3.11).
    std::size_t starter = 0;
    std::uint32_t last_class = classes[0] == 0 ? 0 : 256;
    std::size_t write = 1;
    for (std::size_t read = 1; read < seq.size(); ++read) {
      const char32_t ch = seq[read];
      const std::uint32_t cls = classes[read];
      const char32_t composite = (last_class < cls || last_class == 0) ? compose_pair(seq[starter], ch) : 0;
      if (composite != 0 && (last_class < cls || last_class == 0) && classes[starter] == 0) {
        seq[starter] = composite;
        continue;
      }
      if (cls == 0) starter = write;
      last_class = cls;
      classes[write] = cls;
      seq[write++] = ch;
    }
    seq.resize(write);
  }
  return util::utf32_to_utf8(seq);
}

std::string code_points(std::string_view text, std::string_view style) {
  std::string out;
  bool first = true;
  for (const char32_t cp : util::utf8_to_utf32(util::sanitize_utf8(text))) {
    const auto v = static_cast<std::uint32_t>(cp);
    if (style == "html") {
      out += std::format("&#x{:X};", v);
      continue;
    }
    if (style == "css") {
      out += std::format("\\{:X} ", v);
      continue;
    }
    if (style == "js") {
      out += std::format("\\u{{{:X}}}", v);
      continue;
    }
    if (!first) out.push_back(' ');
    first = false;
    if (style == "hex") {
      out += std::format("0x{:X}", v);
    } else if (style == "decimal") {
      out += std::to_string(v);
    } else {
      out += std::format("U+{:04X}", v);
    }
  }
  return out;
}

Result<std::string> parse_code_points(std::string_view text) {
  std::string out;
  std::size_t i = 0;
  const auto append = [&out](std::uint64_t v, std::size_t at) -> Result<void> {
    if (v > 0x10FFFF || !util::is_scalar_value(static_cast<char32_t>(v))) {
      return fail(Status::Decode, "U+{:X} at offset {} is not a Unicode scalar value", v, at);
    }
    util::append_utf8(out, static_cast<char32_t>(v));
    return {};
  };
  while (i < text.size()) {
    const char c = text[i];
    if (util::is_ascii_space(c) || c == ',' || c == ';' || c == '}') {
      ++i;
      continue;
    }
    std::size_t start = i;
    int base = 16;
    if ((c == 'U' || c == 'u') && i + 1 < text.size() && text[i + 1] == '+') {
      start = i + 2;
    } else if (text.substr(i, 3) == "&#x" || text.substr(i, 3) == "&#X") {
      start = i + 3;
    } else if (text.substr(i, 2) == "&#") {
      start = i + 2;
      base = 10;
    } else if (text.substr(i, 3) == "\\u{") {
      start = i + 3;
    } else if (text.substr(i, 2) == "\\u" || text.substr(i, 2) == "\\U") {
      start = i + 2;
    } else if (c == '\\') {
      start = i + 1;
    } else if (text.substr(i, 2) == "0x" || text.substr(i, 2) == "0X") {
      start = i + 2;
    } else if (util::is_ascii_digit(c)) {
      base = 10;
    } else {
      return fail(Status::Decode, "unexpected '{}' at offset {}", c, i);
    }
    std::size_t end = start;
    while (end < text.size() && (base == 16 ? util::hex_digit_value(text[end]) >= 0 : util::is_ascii_digit(text[end]))) ++end;
    if (end == start || end - start > 8) return fail(Status::Decode, "malformed code point at offset {}", i);
    const auto value = util::parse_int(text.substr(start, end - start), base);
    if (!value) return fail(Status::Decode, "malformed code point at offset {}", i);
    CVT_TRY(append(static_cast<std::uint64_t>(*value), i));
    i = end;
    if (i < text.size() && text[i] == ';' && (text.substr(start - 2, 2) == "&#" || text.substr(start - 3, 3) == "&#x")) ++i;
  }
  return out;
}

std::string utf8_byte_table(std::string_view text) {
  const std::string valid = util::sanitize_utf8(text);
  std::string out;
  for (std::size_t i = 0; i < valid.size();) {
    const util::Utf8Decoded d = util::decode_utf8(valid, i);
    const std::string_view bytes = std::string_view(valid).substr(i, d.length);
    std::string shown;
    if (d.code_point < 0x20 || d.code_point == 0x7F || (d.code_point >= 0x80 && d.code_point < 0xA0)) {
      shown = "\xE2\x90\xA3";  // a visible placeholder for controls
    } else {
      shown = std::string(bytes);
    }
    out += std::format("U+{:04X}  {}  ", static_cast<std::uint32_t>(d.code_point), shown);
    for (std::size_t k = 0; k < bytes.size(); ++k) out += std::format("{}{:02X}", k > 0 ? " " : "", static_cast<unsigned char>(bytes[k]));
    out += "\n";
    i += d.length;
  }
  return out;
}

namespace {

Result<void> check_label(std::string_view label, bool allow_empty) {
  if (label.empty()) {
    if (allow_empty) return {};
    return fail(Status::Decode, "empty label in the domain name");
  }
  if (label.size() > 63) return fail(Status::Decode, "label '{}' is longer than 63 characters", label);
  for (const char c : label) {
    if (!(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '-' || c == '_')) {
      return fail(Status::Decode, "'{}' is not allowed in a host name label ('{}')", c, label);
    }
  }
  if (label.front() == '-' || label.back() == '-') return fail(Status::Decode, "label '{}' starts or ends with '-'", label);
  if (label.size() >= 4 && label.substr(2, 2) == "--" && !util::istarts_with(label, "xn--")) {
    return fail(Status::Decode, "label '{}' has '--' in positions 3 and 4", label);
  }
  return {};
}

std::string map_separators(std::string_view text) {
  std::string out = util::replace_all(text, "\xE3\x80\x82", ".");  // U+3002 ideographic full stop
  out = util::replace_all(out, "\xEF\xBC\x8E", ".");                // U+FF0E fullwidth full stop
  return util::replace_all(out, "\xEF\xBD\xA1", ".");               // U+FF61 halfwidth ideographic full stop
}

}  // namespace

Result<std::string> idna_to_ascii(std::string_view domain) {
  const std::string mapped = text_case(normalize(map_separators(util::trim(domain)), NormalForm::Nfkc), "lower");
  std::vector<std::string> labels;
  const std::vector<std::string_view> parts = util::split(mapped, '.', true);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const std::string_view label = parts[i];
    std::string ascii;
    if (std::ranges::any_of(label, [](char c) { return static_cast<unsigned char>(c) >= 0x80; })) {
      CVT_TRY_ASSIGN(const std::string encoded, punycode_encode_label(util::utf8_to_utf32(label)));
      ascii = "xn--" + encoded;
    } else {
      ascii = std::string(label);
    }
    CVT_TRY(check_label(ascii, i + 1 == parts.size() && parts.size() > 1));
    labels.push_back(std::move(ascii));
  }
  std::string out = util::join(labels, ".");
  if (out.size() > 253 + (out.ends_with('.') ? 1u : 0u)) return fail(Status::Decode, "the domain name is longer than 253 characters");
  return out;
}

Result<std::string> idna_to_unicode(std::string_view domain) {
  std::vector<std::string> labels;
  const std::string lowered = util::to_lower(util::trim(domain));
  for (const std::string_view label : util::split(lowered, '.', true)) {
    if (util::istarts_with(label, "xn--")) {
      CVT_TRY_ASSIGN(const std::u32string decoded, punycode_decode_label(label.substr(4)));
      const std::string text = util::utf32_to_utf8(decoded);
      // A valid A-label decodes to text that is already normalised.
      if (normalize(text, NormalForm::Nfc) != text) return fail(Status::Decode, "'{}' decodes to text that is not in NFC", label);
      labels.push_back(text);
    } else {
      labels.emplace_back(label);
    }
  }
  return util::join(labels, ".");
}

void register_unicode(Registry& registry) {
  add_text_transform(registry, {
      .id = "unicode-normalize", .name = "Unicode-normalised text",
      .description = "Text in Unicode Normalization Form C, D, KC or KD (composed, decomposed, compatibility-folded).",
      .extensions = {"txt"}, .popularity = 15,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        const std::string form = ctx.options.get_enum("form", "nfc", {"nfc", "nfd", "nfkc", "nfkd"});
        return normalize(text, form == "nfd" ? NormalForm::Nfd : form == "nfkc" ? NormalForm::Nfkc
                                            : form == "nfkd" ? NormalForm::Nfkd : NormalForm::Nfc);
      },
      .encode_options = {OptionSpec::choice("form", "Normal form", "nfc", {"nfc", "nfd", "nfkc", "nfkd"},
                                            "NFC composes (the usual form), NFD decomposes, NFKC/NFKD also fold "
                                            "compatibility characters such as ﬁ, ² and full-width letters.")},
      .lossy = true,
      .sniff = [](ByteView, std::string_view) { return 0; },
      .expansion = 18,
  });
  add_text_transform(registry, {
      .id = "codepoints", .name = "Unicode code points",
      .description = "Text as a list of code points such as U+0048 U+0069 (or hex, decimal, HTML, CSS or JS notation).",
      .extensions = {"codepoints"}, .popularity = 15,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        return code_points(text, ctx.options.get_enum("style", "u-plus", {"u-plus", "hex", "decimal", "html", "css", "js"}));
      },
      .decode = [](std::string_view text, const ReadContext&) { return parse_code_points(text); },
      .encode_options = {OptionSpec::choice("style", "Notation", "u-plus", {"u-plus", "hex", "decimal", "html", "css", "js"},
                                            "U+0048, 0x48, 72, &#x48;, \\48 or \\u{48}.")},
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = as_chars(data).substr(0, 64);
        return text.starts_with("U+") && text.size() >= 6 && util::hex_digit_value(text[2]) >= 0 ? 80 : 0;
      },
      .expansion = 12,
  });
  add_text_transform(registry, {
      .id = "utf8-bytes", .name = "UTF-8 byte table",
      .description = "Every character on its own line with its code point and UTF-8 bytes.",
      .extensions = {"txt"}, .popularity = 10,
      .encode = [](std::string_view text, const WriteContext&) -> Result<std::string> { return utf8_byte_table(text); },
      .lossy = true,
      .sniff = [](ByteView, std::string_view) { return 0; },
      .expansion = 30,
  });
  add_text_transform(registry, {
      .id = "idna", .name = "Internationalised domain names",
      .description = "Domain names (one per line) converted to their ASCII xn-- form (UTS #46 basics), and back.",
      .extensions = {"idna"}, .popularity = 12,
      .encode = [](std::string_view text, const WriteContext&) -> Result<std::string> {
        std::string out;
        for (const std::string_view line : util::split_lines(text)) {
          if (util::trim(line).empty()) {
            out += "\n";
            continue;
          }
          CVT_TRY_ASSIGN(const std::string ascii, idna_to_ascii(line));
          out += ascii + "\n";
        }
        return out;
      },
      .decode = [](std::string_view text, const ReadContext&) -> Result<std::string> {
        std::string out;
        for (const std::string_view line : util::split_lines(text)) {
          CVT_TRY_ASSIGN(const std::string unicode, idna_to_unicode(line));
          out += unicode + "\n";
        }
        return out;
      },
      .expansion = 8,
  });
}

}  // namespace cvt::encoding
