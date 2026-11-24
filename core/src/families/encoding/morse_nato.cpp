// Convertible core - encoding family: spelling text out (Morse code, NATO alphabet).
//
// Adding a character: append a row to `morse_codes` or `nato_words` (and, for
// alternative spellings that should only be understood, to `nato_aliases`).
// Adding another spelling alphabet: write `foo_encode` / `foo_decode` with
// the `spell` helper below, declare them in encoding.hpp and add one
// `add_text_transform` block to `register_morse_nato` at the end of this file.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <cstdint>
#include <format>
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

struct Spelling {
  char character;         // upper case
  std::string_view code;  // what is written for it
};

// International Morse code (ITU-R M.1677) plus the customary punctuation.
constexpr auto morse_codes = std::to_array<Spelling>({
    {'A', ".-"},     {'B', "-..."},    {'C', "-.-."},   {'D', "-.."},    {'E', "."},      {'F', "..-."},
    {'G', "--."},    {'H', "...."},    {'I', ".."},     {'J', ".---"},   {'K', "-.-"},    {'L', ".-.."},
    {'M', "--"},     {'N', "-."},      {'O', "---"},    {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},
    {'S', "..."},    {'T', "-"},       {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},
    {'Y', "-.--"},   {'Z', "--.."},    {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},  {'5', "....."},   {'6', "-...."},  {'7', "--..."},  {'8', "---.."},  {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"},  {'?', "..--.."}, {'\'', ".----."}, {'!', "-.-.--"}, {'/', "-..-."},
    {'(', "-.--."},  {')', "-.--.-"},  {'&', ".-..."},  {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},
    {'+', ".-.-."},  {'-', "-....-"},  {'_', "..--.-"}, {'"', ".-..-."}, {'$', "...-..-"}, {'@', ".--.-."},
});

// The ICAO / NATO spelling alphabet, with the ICAO spellings "Alfa" and "Juliett".
constexpr auto nato_words = std::to_array<Spelling>({
    {'A', "Alfa"},    {'B', "Bravo"},   {'C', "Charlie"}, {'D', "Delta"},  {'E', "Echo"},   {'F', "Foxtrot"},
    {'G', "Golf"},    {'H', "Hotel"},   {'I', "India"},   {'J', "Juliett"}, {'K', "Kilo"},  {'L', "Lima"},
    {'M', "Mike"},    {'N', "November"}, {'O', "Oscar"},  {'P', "Papa"},   {'Q', "Quebec"}, {'R', "Romeo"},
    {'S', "Sierra"},  {'T', "Tango"},   {'U', "Uniform"}, {'V', "Victor"}, {'W', "Whiskey"}, {'X', "X-ray"},
    {'Y', "Yankee"},  {'Z', "Zulu"},    {'0', "Zero"},    {'1', "One"},    {'2', "Two"},    {'3', "Three"},
    {'4', "Four"},    {'5', "Five"},    {'6', "Six"},     {'7', "Seven"},  {'8', "Eight"},  {'9', "Nine"},
});

// Spellings that are understood when decoding but never written.
constexpr auto nato_aliases = std::to_array<Spelling>({
    {'A', "Alpha"}, {'J', "Juliet"}, {'X', "Xray"}, {'W', "Whisky"}, {'9', "Niner"},
    {'3', "Tree"},  {'4', "Fower"},  {'5', "Fife"},
});

constexpr std::size_t longest_morse_code = 7;
constexpr std::size_t longest_nato_word = 8;

// Character -> code, for the ASCII range.
template <std::size_t N>
constexpr std::array<std::string_view, 128> make_lookup(const std::array<Spelling, N>& entries) noexcept {
  std::array<std::string_view, 128> table{};
  for (const Spelling& entry : entries) table[detail::byte_of(entry.character)] = entry.code;
  return table;
}

constexpr std::array<std::string_view, 128> morse_lookup = make_lookup(morse_codes);
constexpr std::array<std::string_view, 128> nato_lookup = make_lookup(nato_words);

// Spell `text` out: codes within a word are separated by a space, words by
// " / ", and line breaks are kept. Characters without a code are counted.
std::string spell(std::string_view text, const std::array<std::string_view, 128>& lookup, std::size_t reserve_factor,
                  std::size_t* skipped) {
  std::string out;
  out.reserve(text.size() * reserve_factor);
  std::size_t unknown = 0;
  bool line_has_content = false;
  bool word_gap = false;
  for (std::size_t i = 0; i < text.size();) {
    const util::Utf8Decoded decoded = util::decode_utf8(text, i);
    i += decoded.length;
    const char32_t cp = decoded.code_point;
    if (cp == '\n' || cp == '\r') {
      out.push_back(static_cast<char>(cp));
      line_has_content = false;
      word_gap = false;
      continue;
    }
    if (cp == ' ' || cp == '\t' || cp == 0xA0) {
      word_gap = line_has_content;
      continue;
    }
    const std::string_view code =
        cp < 0x80 ? lookup[static_cast<std::size_t>(util::ascii_upper(static_cast<char>(cp)))] : std::string_view{};
    if (code.empty()) {
      ++unknown;
      continue;
    }
    if (line_has_content) out.append(word_gap ? " / " : " ");
    out.append(code);
    line_has_content = true;
    word_gap = false;
  }
  if (skipped != nullptr) *skipped = unknown;
  return out;
}

// Builds the decoded text: letters are appended directly, a requested word
// gap turns into one space in front of the next letter of the same line.
class Unspelled {
 public:
  void letter(char c) {
    if (word_gap_ && line_has_content_) text_.push_back(' ');
    text_.push_back(c);
    word_gap_ = false;
    line_has_content_ = true;
  }
  void word_gap() { word_gap_ = true; }
  void line_break() {
    text_.push_back('\n');
    word_gap_ = false;
    line_has_content_ = false;
  }
  [[nodiscard]] std::string take() { return std::move(text_); }

 private:
  std::string text_;
  bool word_gap_ = false;
  bool line_has_content_ = false;
};

// "x-ray" and "XRAY" both become "xray".
std::string nato_key(std::string_view word) {
  std::string key;
  key.reserve(word.size());
  for (const char c : word) {
    if (c != '-') key.push_back(util::ascii_lower(c));
  }
  return key;
}

template <std::size_t N>
char find_nato_word(const std::array<Spelling, N>& entries, std::string_view key) {
  for (const Spelling& entry : entries) {
    if (nato_key(entry.code) == key) return entry.character;
  }
  return '\0';
}

void warn_skipped(const Context& ctx, std::size_t skipped, std::string_view what) {
  if (skipped > 0) ctx.warn(std::format("{} character(s) have no {} and were left out.", skipped, what));
}

}  // namespace

// --------------------------------------------------------------------------- Morse

std::string morse_encode(std::string_view text, std::size_t* skipped) { return spell(text, morse_lookup, 5, skipped); }

Result<std::string> morse_decode(std::string_view text) {
  Unspelled out;
  std::string code;
  const auto flush = [&]() -> Result<void> {
    if (code.empty()) return {};
    for (const Spelling& entry : morse_codes) {
      if (entry.code == code) {
        out.letter(entry.character);
        code.clear();
        return {};
      }
    }
    return fail(Status::Decode, "'{}' is not a Morse code sequence", code);
  };
  for (std::size_t i = 0; i < text.size();) {
    const std::size_t offset = i;
    const util::Utf8Decoded decoded = util::decode_utf8(text, i);
    i += decoded.length;
    const char32_t cp = decoded.code_point;
    const bool dot = cp == '.' || cp == 0x00B7 || cp == 0x2022;
    const bool dash = cp == '-' || cp == 0x2212 || cp == 0x2013 || cp == 0x2014;
    if (decoded.ok && (dot || dash)) {
      code.push_back(dot ? '.' : '-');
      if (code.size() > longest_morse_code) {
        return fail(Status::Decode, "the Morse sequence ending at offset {} is too long to be a character", offset);
      }
      continue;
    }
    CVT_TRY(flush());
    if (cp == ' ' || cp == '\t' || cp == '\r' || cp == 0xA0) continue;
    if (cp == '\n') {
      out.line_break();
    } else if (cp == '/' || cp == '|') {
      out.word_gap();
    } else {
      return fail(Status::Decode, "unexpected character in Morse code at offset {}", offset);
    }
  }
  CVT_TRY(flush());
  return out.take();
}

// --------------------------------------------------------------------------- NATO alphabet

std::string nato_encode(std::string_view text, std::size_t* skipped) { return spell(text, nato_lookup, 9, skipped); }

Result<std::string> nato_decode(std::string_view text) {
  Unspelled out;
  std::string word;
  const auto flush = [&]() -> Result<void> {
    if (word.empty()) return {};
    const std::string key = nato_key(word);
    char letter = find_nato_word(nato_words, key);
    if (letter == '\0') letter = find_nato_word(nato_aliases, key);
    if (letter == '\0') return fail(Status::Decode, "'{}' is not a word of the NATO alphabet", word);
    out.letter(letter);
    word.clear();
    return {};
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (util::is_ascii_alpha(c) || c == '-') {
      word.push_back(c);
      if (word.size() > longest_nato_word) {
        return fail(Status::Decode, "the word ending at offset {} is not part of the NATO alphabet", i);
      }
      continue;
    }
    CVT_TRY(flush());
    if (c == ' ' || c == '\t' || c == '\r' || c == ',') continue;
    if (c == '\n') {
      out.line_break();
    } else if (c == '/' || c == '|') {
      out.word_gap();
    } else {
      return fail(Status::Decode, "unexpected character in NATO alphabet text at offset {}", i);
    }
  }
  CVT_TRY(flush());
  return out.take();
}

// --------------------------------------------------------------------------- registration

void register_morse_nato(Registry& registry) {
  add_text_transform(registry, {
      .id = "morse", .name = "Morse code", .description = "Text spelled in dots and dashes of International Morse code.",
      .extensions = {"morse"}, .popularity = 15,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        // Worst case: "$ " alternating gives 7 code characters plus " / " per two bytes; 8 per byte is safe.
        CVT_TRY(detail::check_expansion(ctx, text.size(), 8));
        std::size_t skipped = 0;
        std::string morse = morse_encode(text, &skipped);
        warn_skipped(ctx, skipped, "Morse code");
        return morse;
      },
      .decode = [](std::string_view text, const ReadContext&) { return morse_decode(text); },
      .lossy = true,
  });
  add_text_transform(registry, {
      .id = "nato", .name = "NATO phonetic alphabet",
      .description = "Text spelled with the code words Alfa, Bravo, Charlie and so on.",
      .extensions = {"nato"}, .popularity = 10,
      .encode = [](std::string_view text, const WriteContext& ctx) -> Result<std::string> {
        // Worst case: "November" plus a separator for every letter.
        CVT_TRY(detail::check_expansion(ctx, text.size(), 9));
        std::size_t skipped = 0;
        std::string spelled = nato_encode(text, &skipped);
        warn_skipped(ctx, skipped, "NATO code word");
        return spelled;
      },
      .decode = [](std::string_view text, const ReadContext&) { return nato_decode(text); },
      .lossy = true,
  });
}

}  // namespace cvt::encoding
