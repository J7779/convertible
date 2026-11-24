// Convertible core - encoding family: everyday text tools.
//
//   text-case    lower/UPPER/Title/Sentence case, camelCase, PascalCase,
//                snake_case, kebab-case, CONSTANT_CASE, sWAP cASE
//   slug         URL slugs ("Crème brûlée!" -> "creme-brulee")
//   lines        sort, de-duplicate, reverse, trim, drop empty lines
//   whitespace   tabs <-> spaces, trailing blanks, final newline
//   Classic ciphers (puzzles, not encryption): caesar (ROT-N), vigenere,
//   atbash, a1z26, plus braille (Unicode Grade 1 braille cells).
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
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "families/encoding/text_tools.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

// ---------------------------------------------------------------------------- simple case mapping

/// Upper/lower case of the alphabets most text uses (Latin, Greek, Cyrillic,
/// Armenian, full-width Latin). Characters outside are left unchanged.
char32_t to_upper_cp(char32_t c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c < 0x80) return c;
  if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
  if (c == 0xFF) return 0x178;
  if (c >= 0x100 && c <= 0x17F) {
    if (c == 0x131) return 'I';
    if (c == 0x17F) return 'S';
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c % 2 == 0) ? c - 1 : c;
    if (c == 0x138 || c == 0x149) return c;
    return (c % 2 == 1) ? c - 1 : c;
  }
  if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
  if (c == 0x3C2) return 0x3A3;  // final sigma
  if (c >= 0x3AC && c <= 0x3AF) {
    constexpr std::array<char32_t, 4> accented = {0x386, 0x388, 0x389, 0x38A};
    return accented[c - 0x3AC];
  }
  if (c >= 0x430 && c <= 0x44F) return c - 32;
  if (c >= 0x450 && c <= 0x45F) return c - 80;
  if (c >= 0x561 && c <= 0x586) return c - 48;
  if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
  return c;
}

char32_t to_lower_cp(char32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c < 0x80) return c;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
  if (c == 0x178) return 0xFF;
  if (c >= 0x100 && c <= 0x17F) {
    if (c == 0x130) return 'i';
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c % 2 == 1) ? c + 1 : c;
    if (c == 0x138 || c == 0x149 || c == 0x17F) return c;
    return (c % 2 == 0) ? c + 1 : c;
  }
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
  if (c == 0x386) return 0x3AC;
  if (c >= 0x388 && c <= 0x38A) return c + 37;
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  if (c >= 0x531 && c <= 0x556) return c + 48;
  if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
  return c;
}

bool is_upper_cp(char32_t c) { return to_lower_cp(c) != c; }
bool is_lower_cp(char32_t c) { return to_upper_cp(c) != c || c == 0xDF; }
bool is_letter_cp(char32_t c) {
  return is_upper_cp(c) || is_lower_cp(c) || (c >= 0x80 && c != 0xD7 && c != 0xF7 && !(c >= 0x2000 && c <= 0x2BFF) &&
                                              !(c >= 0x3000 && c <= 0x303F) && !(c >= 0xFF00 && c <= 0xFF20) &&
                                              !(c >= 0x80 && c <= 0xBF) && !(c >= 0x1F000));
}
bool is_digit_cp(char32_t c) { return c >= '0' && c <= '9'; }
bool is_word_cp(char32_t c) { return is_letter_cp(c) || is_digit_cp(c); }

std::u32string decode(std::string_view text) { return util::utf8_to_utf32(util::sanitize_utf8(text)); }
std::string encode(std::u32string_view text) { return util::utf32_to_utf8(text); }

std::string map_case(std::string_view text, char32_t (*f)(char32_t)) {
  std::u32string cps = decode(text);
  for (char32_t& c : cps) c = f(c);
  std::string out = encode(cps);
  // German sharp s has no single-character capital in common use.
  if (f == to_upper_cp) out = util::replace_all(out, "\xC3\x9F", "SS");
  return out;
}

/// Words of an identifier-like line: split at non-word characters and at
/// case changes ("parseHTTPResponse2" -> parse, HTTP, Response2).
std::vector<std::u32string> words_of(std::u32string_view line) {
  std::vector<std::u32string> words;
  std::u32string current;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char32_t c = line[i];
    if (!is_word_cp(c) && c != '\'') {
      if (!current.empty()) words.push_back(std::move(current));
      current.clear();
      continue;
    }
    if (c == '\'') continue;  // don't -> dont
    if (!current.empty() && is_upper_cp(c)) {
      const char32_t prev = current.back();
      const bool next_lower = i + 1 < line.size() && is_lower_cp(line[i + 1]);
      if (is_lower_cp(prev) || is_digit_cp(prev) || (is_upper_cp(prev) && next_lower)) {
        words.push_back(std::move(current));
        current.clear();
      }
    }
    current.push_back(c);
  }
  if (!current.empty()) words.push_back(std::move(current));
  return words;
}

std::u32string lower_word(std::u32string word) {
  for (char32_t& c : word) c = to_lower_cp(c);
  return word;
}
std::u32string capitalized(std::u32string word) {
  word = lower_word(std::move(word));
  if (!word.empty()) word[0] = to_upper_cp(word[0]);
  return word;
}

std::string join_identifier(std::u32string_view line, std::string_view style) {
  const std::vector<std::u32string> words = words_of(line);
  std::u32string out;
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (style == "camel") {
      out += i == 0 ? lower_word(words[i]) : capitalized(words[i]);
    } else if (style == "pascal") {
      out += capitalized(words[i]);
    } else {
      if (i > 0) out.push_back(style == "kebab" ? U'-' : U'_');
      std::u32string w = lower_word(words[i]);
      if (style == "constant") {
        for (char32_t& c : w) c = to_upper_cp(c);
      }
      out += w;
    }
  }
  return encode(out);
}

std::string per_line(std::string_view text, const auto& convert) {
  std::string out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    const bool cr = line.ends_with('\r');
    if (cr) line.remove_suffix(1);
    out += convert(line);
    if (cr) out.push_back('\r');
    if (end == std::string_view::npos) break;
    out.push_back('\n');
    start = end + 1;
  }
  return out;
}

// ---------------------------------------------------------------------------- slugs

/// Latin letters with diacritics, ligatures and a few others, mapped to ASCII.
std::string_view transliterate(char32_t c) {
  struct Map {
    char32_t from;
    std::string_view to;
  };
  static constexpr Map table[] = {
      {0xC0, "A"},  {0xC1, "A"},  {0xC2, "A"},  {0xC3, "A"},  {0xC4, "A"},  {0xC5, "A"},  {0xC6, "AE"}, {0xC7, "C"},
      {0xC8, "E"},  {0xC9, "E"},  {0xCA, "E"},  {0xCB, "E"},  {0xCC, "I"},  {0xCD, "I"},  {0xCE, "I"},  {0xCF, "I"},
      {0xD0, "D"},  {0xD1, "N"},  {0xD2, "O"},  {0xD3, "O"},  {0xD4, "O"},  {0xD5, "O"},  {0xD6, "O"},  {0xD8, "O"},
      {0xD9, "U"},  {0xDA, "U"},  {0xDB, "U"},  {0xDC, "U"},  {0xDD, "Y"},  {0xDE, "TH"}, {0xDF, "ss"}, {0xE0, "a"},
      {0xE1, "a"},  {0xE2, "a"},  {0xE3, "a"},  {0xE4, "a"},  {0xE5, "a"},  {0xE6, "ae"}, {0xE7, "c"},  {0xE8, "e"},
      {0xE9, "e"},  {0xEA, "e"},  {0xEB, "e"},  {0xEC, "i"},  {0xED, "i"},  {0xEE, "i"},  {0xEF, "i"},  {0xF0, "d"},
      {0xF1, "n"},  {0xF2, "o"},  {0xF3, "o"},  {0xF4, "o"},  {0xF5, "o"},  {0xF6, "o"},  {0xF8, "o"},  {0xF9, "u"},
      {0xFA, "u"},  {0xFB, "u"},  {0xFC, "u"},  {0xFD, "y"},  {0xFE, "th"}, {0xFF, "y"},  {0x110, "D"}, {0x111, "d"},
      {0x126, "H"}, {0x127, "h"}, {0x131, "i"}, {0x132, "IJ"}, {0x133, "ij"}, {0x138, "k"}, {0x13F, "L"}, {0x140, "l"},
      {0x141, "L"}, {0x142, "l"}, {0x149, "n"}, {0x14A, "NG"}, {0x14B, "ng"}, {0x152, "OE"}, {0x153, "oe"}, {0x166, "T"},
      {0x167, "t"}, {0x17F, "s"}, {0x218, "S"}, {0x219, "s"}, {0x21A, "T"}, {0x21B, "t"},
  };
  for (const Map& m : table) {
    if (m.from == c) return m.to;
  }
  // Latin Extended-A letters with a diacritic: drop it (base letters by block position).
  if (c >= 0x100 && c <= 0x17F) {
    static constexpr std::string_view bases =
        "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlL"
        "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    static_assert(bases.size() == 128);
    return bases.substr(c - 0x100, 1);
  }
  return {};
}

}  // namespace

std::string text_case(std::string_view text, std::string_view style) {
  if (style == "lower") return map_case(text, to_lower_cp);
  if (style == "upper") return map_case(text, to_upper_cp);
  if (style == "swap") {
    std::u32string cps = decode(text);
    for (char32_t& c : cps) c = is_upper_cp(c) ? to_lower_cp(c) : to_upper_cp(c);
    return encode(cps);
  }
  if (style == "title") {
    std::u32string cps = decode(text);
    bool start = true;
    for (char32_t& c : cps) {
      if (is_word_cp(c)) {
        c = start ? to_upper_cp(c) : to_lower_cp(c);
        start = false;
      } else if (c != '\'' && c != 0x2019) {
        start = true;
      }
    }
    return encode(cps);
  }
  if (style == "sentence") {
    std::u32string cps = decode(text);
    bool start = true;
    for (char32_t& c : cps) {
      if (is_letter_cp(c)) {
        c = start ? to_upper_cp(c) : to_lower_cp(c);
        start = false;
      } else if (c == '.' || c == '!' || c == '?' || c == '\n') {
        start = true;
      }
    }
    return encode(cps);
  }
  return per_line(text, [style](std::string_view line) { return join_identifier(decode(line), style); });
}

std::string slugify(std::string_view text, char separator, std::size_t max_length) {
  std::string out;
  bool pending = false;
  for (const char32_t c : decode(text)) {
    std::string piece;
    if (c < 0x80) {
      if (util::is_ascii_alpha(static_cast<char>(c)) || util::is_ascii_digit(static_cast<char>(c))) {
        piece.push_back(util::ascii_lower(static_cast<char>(c)));
      }
    } else {
      piece = util::to_lower(transliterate(c));
    }
    if (piece.empty()) {
      if (c != '\'' && c != 0x2019) pending = !out.empty();  // apostrophes join (don't -> dont)
      continue;
    }
    if (pending) out.push_back(separator);
    pending = false;
    out += piece;
  }
  if (max_length > 0 && out.size() > max_length) {
    out.resize(max_length);
    while (!out.empty() && out.back() == separator) out.pop_back();
  }
  return out;
}

std::string transform_lines(std::string_view text, const LineOptions& options) {
  const bool final_newline = text.ends_with('\n');
  std::vector<std::string> lines;
  for (std::string_view line : util::split_lines(text)) {
    if (options.trim) line = util::trim(line);
    if (options.remove_empty && util::trim(line).empty()) continue;
    lines.emplace_back(line);
  }
  if (!final_newline && !lines.empty() && text.empty()) lines.clear();
  const auto key = [&options](const std::string& line) { return options.ignore_case ? util::to_lower(line) : line; };
  // Natural order: digit runs compare by value ("file2" < "file10").
  const auto natural_less = [&key](const std::string& a_line, const std::string& b_line) {
    const std::string a = key(a_line), b = key(b_line);
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
      if (util::is_ascii_digit(a[i]) && util::is_ascii_digit(b[j])) {
        std::size_t ei = i, ej = j;
        while (ei < a.size() && util::is_ascii_digit(a[ei])) ++ei;
        while (ej < b.size() && util::is_ascii_digit(b[ej])) ++ej;
        std::string_view na = std::string_view(a).substr(i, ei - i), nb = std::string_view(b).substr(j, ej - j);
        while (na.size() > 1 && na[0] == '0') na.remove_prefix(1);
        while (nb.size() > 1 && nb[0] == '0') nb.remove_prefix(1);
        if (na.size() != nb.size()) return na.size() < nb.size();
        if (na != nb) return na < nb;
        i = ei;
        j = ej;
      } else {
        if (a[i] != b[j]) return a[i] < b[j];
        ++i;
        ++j;
      }
    }
    return a.size() - i < b.size() - j;
  };
  if (options.sort == "asc" || options.sort == "desc") {
    std::ranges::stable_sort(lines, [&key](const std::string& a, const std::string& b) { return key(a) < key(b); });
  } else if (options.sort == "natural") {
    std::ranges::stable_sort(lines, natural_less);
  } else if (options.sort == "length") {
    std::ranges::stable_sort(lines, [](const std::string& a, const std::string& b) {
      return util::utf8_length(a) < util::utf8_length(b);
    });
  }
  if (options.sort == "desc") std::ranges::reverse(lines);
  if (options.unique) {
    std::vector<std::string> kept;
    std::vector<std::string> seen;  // sorted keys
    for (std::string& line : lines) {
      const std::string k = key(line);
      const auto at = std::ranges::lower_bound(seen, k);
      if (at != seen.end() && *at == k) continue;
      seen.insert(at, k);
      kept.push_back(std::move(line));
    }
    lines = std::move(kept);
  }
  if (options.reverse) std::ranges::reverse(lines);
  std::string out = util::join(lines, "\n");
  if (final_newline && !lines.empty()) out.push_back('\n');
  return out;
}

std::string transform_whitespace(std::string_view text, const WhitespaceOptions& options) {
  const std::size_t width = std::max<std::size_t>(1, options.tab_width);
  std::string out = per_line(text, [&](std::string_view line) {
    std::string result;
    if (options.tabs == "expand") {
      std::size_t column = 0;
      for (std::size_t i = 0; i < line.size();) {
        if (line[i] == '\t') {
          const std::size_t spaces = width - column % width;
          result.append(spaces, ' ');
          column += spaces;
          ++i;
          continue;
        }
        const auto length = static_cast<std::size_t>(util::decode_utf8(line, i).length);
        result.append(line.substr(i, length));
        ++column;
        i += length;
      }
    } else if (options.tabs == "unexpand") {
      // Only the indentation is converted; tabs inside a line would change its alignment meaning.
      std::size_t i = 0, column = 0;
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        column = line[i] == '\t' ? (column / width + 1) * width : column + 1;
        ++i;
      }
      result.append(column / width, '\t');
      result.append(column % width, ' ');
      result.append(line.substr(i));
    } else {
      result = std::string(line);
    }
    if (options.trim_trailing) {
      while (!result.empty() && (result.back() == ' ' || result.back() == '\t')) result.pop_back();
    }
    return result;
  });
  if (options.final_newline == "add" && !out.empty() && !out.ends_with('\n')) out.push_back('\n');
  if (options.final_newline == "remove") {
    while (out.ends_with('\n')) {
      out.pop_back();
      if (out.ends_with('\r')) out.pop_back();
    }
  }
  return out;
}

// ---------------------------------------------------------------------------- classic ciphers

std::string caesar(std::string_view text, int shift) {
  shift = ((shift % 26) + 26) % 26;
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>('a' + (c - 'a' + shift) % 26);
    if (c >= 'A' && c <= 'Z') c = static_cast<char>('A' + (c - 'A' + shift) % 26);
  }
  return out;
}

Result<std::string> vigenere(std::string_view text, std::string_view key, bool decrypt) {
  std::vector<int> shifts;
  for (const char c : key) {
    if (util::is_ascii_alpha(c)) shifts.push_back(util::ascii_lower(c) - 'a');
  }
  if (shifts.empty()) return fail(Status::InvalidArgument, "the Vigenère key must contain at least one letter A-Z");
  std::string out(text);
  std::size_t k = 0;
  for (char& c : out) {
    if (!util::is_ascii_alpha(c)) continue;
    const int shift = decrypt ? 26 - shifts[k % shifts.size()] : shifts[k % shifts.size()];
    const char base = c >= 'a' ? 'a' : 'A';
    c = static_cast<char>(base + (c - base + shift) % 26);
    ++k;
  }
  return out;
}

std::string atbash(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>('z' - (c - 'a'));
    if (c >= 'A' && c <= 'Z') c = static_cast<char>('Z' - (c - 'A'));
  }
  return out;
}

std::string a1z26_encode(std::string_view text) {
  std::string out;
  bool in_word = false;
  for (const char c : text) {
    if (util::is_ascii_alpha(c)) {
      if (in_word) out.push_back('-');
      out += std::to_string(util::ascii_lower(c) - 'a' + 1);
      in_word = true;
    } else {
      out.push_back(c);
      in_word = false;
    }
  }
  return out;
}

Result<std::string> a1z26_decode(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size();) {
    if (util::is_ascii_digit(text[i])) {
      std::size_t end = i;
      while (end < text.size() && util::is_ascii_digit(text[end])) ++end;
      const auto value = util::parse_int(text.substr(i, end - i));
      if (!value || *value < 1 || *value > 26) return fail(Status::Decode, "A1Z26 number out of range at offset {}", i);
      out.push_back(static_cast<char>('a' + *value - 1));
      i = end;
      if (i + 1 < text.size() && text[i] == '-' && util::is_ascii_digit(text[i + 1])) ++i;  // letter separator
      continue;
    }
    out.push_back(text[i++]);
  }
  return out;
}

namespace {

// Grade 1 (uncontracted) English braille, as Unicode braille patterns.
// Bits 0..5 are dots 1..6.
constexpr std::array<std::uint8_t, 26> braille_letters = {
    0x01, 0x03, 0x09, 0x19, 0x11, 0x0B, 0x1B, 0x13, 0x0A, 0x1A, 0x05, 0x07, 0x0D,
    0x1D, 0x15, 0x0F, 0x1F, 0x17, 0x0E, 0x1E, 0x25, 0x27, 0x3A, 0x2D, 0x3D, 0x35};
constexpr std::uint8_t braille_number = 0x3C;   // dots 3456
constexpr std::uint8_t braille_capital = 0x20;  // dot 6
struct BraillePunct {
  char c;
  std::uint8_t dots;
};
constexpr std::array<BraillePunct, 8> braille_punctuation = {
    {{',', 0x02}, {';', 0x06}, {':', 0x12}, {'.', 0x32}, {'!', 0x16}, {'?', 0x26}, {'\'', 0x04}, {'-', 0x24}}};

void append_cell(std::string& out, std::uint8_t dots) { util::append_utf8(out, static_cast<char32_t>(0x2800 + dots)); }

}  // namespace

std::string braille_encode(std::string_view text, std::size_t* skipped) {
  std::string out;
  bool number_mode = false;
  std::size_t missing = 0;
  for (const char32_t c : decode(text)) {
    if (c >= '0' && c <= '9') {
      if (!number_mode) append_cell(out, braille_number);
      number_mode = true;
      append_cell(out, braille_letters[c == '0' ? 9 : c - '1']);
      continue;
    }
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (letter && number_mode && c >= 'a' && c <= 'j') append_cell(out, 0x30);  // letter sign after a number
    number_mode = false;
    if (letter) {
      if (c <= 'Z') append_cell(out, braille_capital);
      append_cell(out, braille_letters[(c | 0x20) - 'a']);
    } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || (c >= 0x2800 && c <= 0x28FF)) {
      util::append_utf8(out, c == ' ' ? static_cast<char32_t>(0x2800) : c);
    } else if (const auto p = std::ranges::find_if(braille_punctuation, [c](const BraillePunct& b) {
                 return static_cast<char32_t>(b.c) == c;
               });
               p != braille_punctuation.end()) {
      append_cell(out, p->dots);
    } else {
      ++missing;
    }
  }
  if (skipped != nullptr) *skipped = missing;
  return out;
}

Result<std::string> braille_decode(std::string_view text) {
  std::string out;
  bool number_mode = false;
  bool capital = false;
  for (const char32_t c : decode(text)) {
    if (c < 0x2800 || c > 0x28FF) {
      if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
        util::append_utf8(out, c);
        number_mode = false;
        continue;
      }
      return fail(Status::Decode, "U+{:04X} is not a braille cell", static_cast<std::uint32_t>(c));
    }
    const auto dots = static_cast<std::uint8_t>(c - 0x2800);
    if (dots == 0) {
      out.push_back(' ');
      number_mode = false;
      continue;
    }
    if (dots == braille_number) {
      number_mode = true;
      continue;
    }
    if (dots == braille_capital) {
      capital = true;
      continue;
    }
    if (dots == 0x30) {  // letter sign
      number_mode = false;
      continue;
    }
    const auto letter = std::ranges::find(braille_letters, dots);
    if (letter != braille_letters.end()) {
      const auto index = static_cast<char>(letter - braille_letters.begin());
      if (number_mode && index < 10) {
        out.push_back(index == 9 ? '0' : static_cast<char>('1' + index));
      } else {
        out.push_back(static_cast<char>((capital ? 'A' : 'a') + index));
      }
      capital = false;
      continue;
    }
    const auto p = std::ranges::find_if(braille_punctuation, [dots](const BraillePunct& b) { return b.dots == dots; });
    if (p == braille_punctuation.end()) return fail(Status::Decode, "unknown braille cell U+{:04X}", static_cast<std::uint32_t>(c));
    out.push_back(p->c);
    number_mode = false;
  }
  return out;
}

void register_text_tools(Registry& registry) {
  const std::vector<std::string> cases = {"lower", "upper", "title",   "sentence", "camel",
                                          "pascal", "snake", "kebab", "constant", "swap"};
  add_text_transform(registry, {
      .id = "text-case", .name = "Text case conversion",
      .description = "Change letter case: lower, UPPER, Title, Sentence, camelCase, PascalCase, snake_case, kebab-case ...",
      .extensions = {"txt"}, .popularity = 30,
      .encode = [cases](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        return text_case(text, ctx.options.get_enum("case", "lower", cases));
      },
      .encode_options = {OptionSpec::choice("case", "Case", "lower", cases,
                                            "Target style; identifier styles (camel, pascal, snake, kebab, constant) "
                                            "turn every line into one identifier.")},
      .lossy = true,
      .sniff = [](ByteView, std::string_view) { return 0; },
      .expansion = 3,
  });
  add_text_transform(registry, {
      .id = "slug", .name = "URL slug", .description = "Lower-case ASCII words joined by hyphens, one slug per line.",
      .extensions = {"slug"}, .popularity = 20,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        const char separator = ctx.options.get_enum("separator", "-", {"-", "_"}) == "_" ? '_' : '-';
        const auto max_length = static_cast<std::size_t>(ctx.options.get_int("max_length", 0, 0, 1000));
        return per_line(text, [separator, max_length](std::string_view line) { return slugify(line, separator, max_length); });
      },
      .encode_options = {OptionSpec::choice("separator", "Separator", "-", {"-", "_"}, "Character between words."),
                         OptionSpec::integer("max_length", "Maximum length", 0, 0, 1000,
                                             "Cut longer slugs at this many characters; 0 = no limit.")},
      .lossy = true,
      .expansion = 2,
  });
  add_text_transform(registry, {
      .id = "lines", .name = "Sorted or de-duplicated lines",
      .description = "Sort, de-duplicate, reverse, trim or drop empty lines of a text.",
      .extensions = {"txt"}, .popularity = 25,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        LineOptions options;
        options.sort = ctx.options.get_enum("sort", "asc", {"none", "asc", "desc", "natural", "length"});
        options.unique = ctx.options.get_bool("unique", false);
        options.reverse = ctx.options.get_bool("reverse", false);
        options.trim = ctx.options.get_bool("trim", false);
        options.remove_empty = ctx.options.get_bool("remove_empty", false);
        options.ignore_case = ctx.options.get_bool("ignore_case", false);
        return transform_lines(text, options);
      },
      .encode_options = {OptionSpec::choice("sort", "Sort", "asc", {"none", "asc", "desc", "natural", "length"},
                                            "Order: keep, A-Z, Z-A, natural (file2 before file10) or by length."),
                         OptionSpec::boolean("unique", "Remove duplicates", false, "Keep only the first of equal lines."),
                         OptionSpec::boolean("reverse", "Reverse", false, "Reverse the order of the lines (after sorting)."),
                         OptionSpec::boolean("trim", "Trim lines", false, "Remove blanks at both ends of every line."),
                         OptionSpec::boolean("remove_empty", "Drop empty lines", false, "Remove lines that are blank."),
                         OptionSpec::boolean("ignore_case", "Ignore case", false,
                                             "Compare lines without regard to ASCII letter case.")},
      .lossy = true,
      .sniff = [](ByteView, std::string_view) { return 0; },
      .expansion = 2,
  });
  add_text_transform(registry, {
      .id = "whitespace", .name = "Tabs and spaces",
      .description = "Expand tabs to spaces or indent with tabs, trim trailing blanks, fix the final newline.",
      .extensions = {"txt"}, .popularity = 15,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        WhitespaceOptions options;
        options.tabs = ctx.options.get_enum("tabs", "expand", {"keep", "expand", "unexpand"});
        options.tab_width = static_cast<std::size_t>(ctx.options.get_int("tab_width", 4, 1, 16));
        options.trim_trailing = ctx.options.get_bool("trim_trailing", false);
        options.final_newline = ctx.options.get_enum("final_newline", "keep", {"keep", "add", "remove"});
        return transform_whitespace(text, options);
      },
      .encode_options = {OptionSpec::choice("tabs", "Tabs", "expand", {"keep", "expand", "unexpand"},
                                            "expand: tabs become spaces; unexpand: leading spaces become tabs."),
                         OptionSpec::integer("tab_width", "Tab width", 4, 1, 16, "Columns per tab stop."),
                         OptionSpec::boolean("trim_trailing", "Trim trailing blanks", false,
                                             "Remove spaces and tabs at the ends of lines."),
                         OptionSpec::choice("final_newline", "Final newline", "keep", {"keep", "add", "remove"},
                                            "Make sure the text ends with exactly one line break, or with none.")},
      .lossy = true,
      .sniff = [](ByteView, std::string_view) { return 0; },
      .expansion = 16,
  });

  // ---- classic ciphers ------------------------------------------------------------------
  add_text_transform(registry, {
      .id = "caesar", .name = "Caesar cipher (ROT-N)",
      .description = "Classic cipher, not encryption: every letter shifted N places in the alphabet.",
      .extensions = {"caesar"}, .popularity = 12,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        return caesar(text, static_cast<int>(ctx.options.get_int("shift", 3, 1, 25)));
      },
      .decode = [](std::string_view text, const ReadContext& ctx) -> Result<std::string> {
        return caesar(text, -static_cast<int>(ctx.options.get_int("shift", 3, 1, 25)));
      },
      .encode_options = {OptionSpec::integer("shift", "Shift", 3, 1, 25, "Places each letter moves (13 = ROT13).")},
      .decode_options = {OptionSpec::integer("shift", "Shift", 3, 1, 25, "Places each letter was moved.")},
      .expansion = 1,
  });
  add_text_transform(registry, {
      .id = "vigenere", .name = "Vigenère cipher",
      .description = "Classic cipher, not encryption: letters shifted by the letters of a repeating key.",
      .extensions = {"vigenere"}, .popularity = 8,
      .encode = [](std::string_view text, const WriteContext& ctx) {
        return vigenere(text, ctx.options.get_string("key", "KEY"), false);
      },
      .decode = [](std::string_view text, const ReadContext& ctx) {
        return vigenere(text, ctx.options.get_string("key", "KEY"), true);
      },
      .encode_options = {OptionSpec::text("key", "Key", "KEY", "Letters of the key; other characters are ignored.")},
      .decode_options = {OptionSpec::text("key", "Key", "KEY", "Letters of the key; other characters are ignored.")},
      .expansion = 1,
  });
  add_text_transform(registry, {
      .id = "atbash", .name = "Atbash cipher",
      .description = "Classic cipher, not encryption: the alphabet reversed (A<->Z, B<->Y ...).",
      .extensions = {"atbash"}, .popularity = 6,
      .encode = [](std::string_view text, const WriteContext&) -> Result<std::string> { return atbash(text); },
      .decode = [](std::string_view text, const ReadContext&) -> Result<std::string> { return atbash(text); },
      .expansion = 1,
  });
  add_text_transform(registry, {
      .id = "a1z26", .name = "A1Z26 cipher",
      .description = "Classic puzzle code, not encryption: letters as their positions 1-26 (\"hi\" -> \"8-9\").",
      .extensions = {"a1z26"}, .popularity = 6,
      .encode = [](std::string_view text, const WriteContext&) -> Result<std::string> { return a1z26_encode(text); },
      .decode = [](std::string_view text, const ReadContext&) { return a1z26_decode(text); },
      .lossy = true,  // letter case
      .expansion = 3,
  });
  add_text_transform(registry, {
      .id = "braille", .name = "Braille (Grade 1)",
      .description = "Uncontracted English braille written with Unicode braille cells (capital and number signs).",
      .extensions = {"brl"}, .popularity = 10,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        std::size_t skipped = 0;
        std::string out = braille_encode(text, &skipped);
        if (skipped > 0) ctx.warn(std::format("{} character(s) have no Grade 1 braille cell here and were left out.", skipped));
        return out;
      },
      .decode = [](std::string_view text, const ReadContext&) { return braille_decode(text); },
      .lossy = true,
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = as_chars(data).substr(0, 64);
        return text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xE2 &&
                       (static_cast<unsigned char>(text[1]) & 0xFC) == 0xA0
                   ? 80
                   : 0;
      },
      .expansion = 7,
  });
}

}  // namespace cvt::encoding
