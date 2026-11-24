// Convertible core - subtitle family: shared reading, timing and markup helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/subtitle/codepages.hpp"
#include "families/subtitle/subtitle.hpp"
#include "util/text.hpp"

namespace cvt::subtitle {

namespace {

using namespace std::string_view_literals;

/// Longest supported media time: 1000 hours. Larger values are clamped so
/// that arithmetic on hostile timestamps can never overflow.
constexpr std::int64_t max_time_ms = 1000LL * 3600 * 1000;

std::int64_t clamp_time(std::int64_t ms) { return std::clamp<std::int64_t>(ms, 0, max_time_ms); }

// ---------------------------------------------------------------------------- code pages

const codepages::CodePage* find_codepage(std::string_view name) {
  for (const codepages::CodePage& page : codepages::tables) {
    if (util::iequals(page.name, name)) return &page;
  }
  // Common aliases.
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 14> aliases = {{
      {"cp1250", "windows-1250"}, {"cp1251", "windows-1251"}, {"cp1253", "windows-1253"},
      {"cp1254", "windows-1254"}, {"cp1255", "windows-1255"}, {"cp1256", "windows-1256"},
      {"cp1257", "windows-1257"}, {"cp874", "windows-874"},   {"latin2", "iso-8859-2"},
      {"cyrillic", "iso-8859-5"}, {"greek", "iso-8859-7"},    {"ibm437", "cp437"},
      {"ibm850", "cp850"},        {"macintosh", "mac-roman"},
  }};
  for (const auto& [alias, canonical] : aliases) {
    if (util::iequals(alias, name)) return find_codepage(canonical);
  }
  return nullptr;
}

std::string decode_with(ByteView data, const codepages::CodePage& page) {
  std::string out;
  out.reserve(data.size() + data.size() / 4);
  for (const std::byte b : data) {
    const auto value = std::to_integer<std::uint8_t>(b);
    if (value < 0x80) {
      out.push_back(static_cast<char>(value));
    } else {
      const char16_t mapped = page.high[value - 0x80];
      util::append_utf8(out, mapped == 0 ? util::replacement_character : static_cast<char32_t>(mapped));
    }
  }
  return out;
}

/// Letters that are frequent in the languages a code page serves; a decode
/// producing many of them is likely right.
struct Profile {
  std::string_view page;
  std::u32string_view frequent;  // strong evidence (weight 3)
  std::u32string_view rare;      // letters that are implausible in this page's languages (weight -2)
};

constexpr std::array<Profile, 8> profiles = {{
    {"windows-1252", U"éèàùâêîôûçëïüäöñßáíóúãõåæøœÉÈÀÇÜÄÖÑ", U"ìòðþý¹³¾"},
    {"windows-1250", U"čěřšžýáíéůúňťďóąćęłńśźżőűŕĺľČŘŠŽŁŚŻ", U"ŕĺ"},
    {"windows-1251", U"оеаинтсрвлкмдпуяыьгзбчйхжшюцщэфъёОЕАИНТСРВЛКМДП", U""},
    {"koi8-r", U"оеаинтсрвлкмдпуяыьгзбчйхжшюцщэфъёОЕАИНТСРВЛКМДП", U""},
    {"windows-1253", U"αοιετσνηυρπκμλάέίόύήώςΑΟΙΕΤΣΝΗ", U""},
    {"windows-1254", U"ğışçöüĞİŞÇÖÜ", U""},
    {"windows-1255", U"יוהלארתבמשנעכדקפחצגזסטםןךףץ", U""},
    {"windows-1256", U"اليمنورتبهعدفكسقحجشصطزخضذثغظةىئءأإآ", U""},
}};

bool is_letter(char32_t c) {
  return (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x370 && c <= 0x3FF) ||
         (c >= 0x400 && c <= 0x4FF) || (c >= 0x5D0 && c <= 0x5EA) || (c >= 0x620 && c <= 0x64A) ||
         (c >= 0x0E01 && c <= 0x0E5B);
}

double score_decode(std::string_view decoded, const Profile& profile) {
  double score = 0.0;
  char32_t previous = 0;
  std::size_t i = 0;
  while (i < decoded.size()) {
    const util::Utf8Decoded d = util::decode_utf8(decoded, i);
    i += d.length == 0 ? 1 : d.length;
    const char32_t c = d.code_point;
    if (c < 0x80) {
      previous = c;
      continue;
    }
    if (c == util::replacement_character || (c >= 0x80 && c <= 0x9F)) {
      score -= 5.0;
    } else if (profile.frequent.find(c) != std::u32string_view::npos) {
      score += 3.0;
    } else if (profile.rare.find(c) != std::u32string_view::npos) {
      score -= 2.0;
    } else if (is_letter(c)) {
      score += 0.5;
    } else if (c == 0x2018 || c == 0x2019 || c == 0x201C || c == 0x201D || c == 0x2013 || c == 0x2014 ||
               c == 0x2026 || c == 0xA0 || c == 0xAB || c == 0xBB) {
      score += 0.25;
    } else {
      score -= 1.0;
    }
    // A letter that follows an ASCII letter is the norm; scripts must not mix with Latin inside a word.
    if (previous != 0 && previous < 0x80 && util::is_ascii_alpha(static_cast<char>(previous)) && c >= 0x370 &&
        is_letter(c)) {
      score -= 1.5;
    }
    previous = c;
  }
  return score;
}

// ---------------------------------------------------------------------------- markup

bool is_inline_tag(std::string_view name) { return name == "i" || name == "b" || name == "u"; }

/// Decode one character reference at text[i] == '&'. Returns the bytes to
/// append and the length consumed, or nullopt for a literal '&'.
std::optional<std::pair<std::string, std::size_t>> decode_reference(std::string_view text, std::size_t i) {
  const std::size_t semicolon = text.find(';', i);
  if (semicolon == std::string_view::npos || semicolon - i > 10) return std::nullopt;
  const std::string_view name = text.substr(i + 1, semicolon - i - 1);
  if (name.empty()) return std::nullopt;
  std::string out;
  if (name[0] == '#') {
    std::optional<std::int64_t> value;
    if (name.size() > 1 && (name[1] == 'x' || name[1] == 'X')) {
      value = util::parse_int(name.substr(2), 16);
    } else {
      value = util::parse_int(name.substr(1), 10);
    }
    if (!value || *value <= 0 || *value > 0x10FFFF) return std::nullopt;
    util::append_utf8(out, static_cast<char32_t>(*value));
    return std::pair{out, semicolon - i + 1};
  }
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> named = {{
      {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
      {"nbsp", "\xC2\xA0"}, {"lrm", "\xE2\x80\x8E"}, {"rlm", "\xE2\x80\x8F"}, {"shy", "\xC2\xAD"},
  }};
  for (const auto& [key, value] : named) {
    if (name == key) return std::pair{std::string(value), semicolon - i + 1};
  }
  return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------- encodings

std::optional<std::string> decode_codepage(ByteView data, std::string_view name) {
  const codepages::CodePage* page = find_codepage(name);
  if (page == nullptr) return std::nullopt;
  return decode_with(data, *page);
}

std::string guess_codepage(ByteView data) {
  // Score a bounded sample: long files say nothing new after 64 KiB.
  const ByteView sample = data.first(std::min<std::size_t>(data.size(), 64 * 1024));
  std::string best = "windows-1252";
  double best_score = -1e300;
  for (const Profile& profile : profiles) {
    std::string decoded;
    if (profile.page == "windows-1252") {
      decoded = util::decode_text(sample, util::TextEncoding::Windows1252).text;
    } else {
      const codepages::CodePage* page = find_codepage(profile.page);
      if (page == nullptr) continue;
      decoded = decode_with(sample, *page);
    }
    const double score = score_decode(decoded, profile);
    if (score > best_score + 1e-9) {
      best_score = score;
      best = std::string(profile.page);
    }
  }
  return best;
}

Result<std::string> decode_bytes(ByteView data, std::string_view encoding) {
  std::string text;
  if (encoding.empty() || util::iequals(encoding, "auto")) {
    const util::EncodingGuess guess = util::guess_encoding(data);
    if (guess.confident || guess.encoding != util::TextEncoding::Windows1252) {
      text = util::decode_text(data, guess.encoding).text;
    } else {
      const std::string page = guess_codepage(data);
      if (page == "windows-1252") {
        text = util::decode_text(data, util::TextEncoding::Windows1252).text;
      } else {
        text = *decode_codepage(data, page);
      }
    }
  } else if (const auto parsed = util::parse_encoding(encoding)) {
    text = util::decode_text(data, *parsed).text;
  } else if (auto decoded = decode_codepage(data, encoding)) {
    text = std::move(*decoded);
  } else {
    return fail(Status::InvalidArgument, "unknown character encoding '{}'", util::sanitize_utf8(encoding));
  }
  if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
  return text;
}

Result<std::string> decode_input(const ReadContext& ctx) {
  return decode_bytes(ctx.input, ctx.options.get_string("source_encoding", "auto"));
}

// ---------------------------------------------------------------------------- options

std::vector<OptionSpec> common_read_options() {
  std::vector<std::string> encodings = {"auto",     "utf-8",  "utf-16le", "utf-16be",
                                        "utf-32le", "utf-32be", "latin1",  "windows-1252"};
  for (const codepages::CodePage& page : codepages::tables) encodings.emplace_back(page.name);
  return {
      OptionSpec::choice("source_encoding", "Source encoding", "auto", std::move(encodings),
                         "Character encoding of text subtitles; 'auto' detects Unicode and guesses legacy code pages.")
          .advanced(),
      OptionSpec::integer("offset_ms", "Time shift (ms)", 0, -86'400'000, 86'400'000,
                          "Milliseconds added to every cue time (negative values make subtitles appear earlier)."),
      OptionSpec::number("fps_from", "Retime from frame rate", 0.0, 0.0, 1000.0,
                         "Frame rate the subtitles were timed for; together with 'Retime to frame rate' it "
                         "stretches all times (e.g. 25 -> 23.976). 0 = no retiming.")
          .advanced(),
      OptionSpec::number("fps_to", "Retime to frame rate", 0.0, 0.0, 1000.0,
                         "Frame rate of the target video; see 'Retime from frame rate'. 0 = no retiming.")
          .advanced(),
      OptionSpec::integer("min_duration_ms", "Minimum duration (ms)", 0, 0, 60'000,
                          "Extend cues shorter than this (never past the start of the next cue). 0 = off.")
          .advanced(),
      OptionSpec::integer("max_duration_ms", "Maximum duration (ms)", 0, 0, 3'600'000,
                          "Shorten cues longer than this. 0 = off.")
          .advanced(),
      OptionSpec::boolean("fix_overlaps", "Repair overlaps", false,
                          "End each cue before the next one starts; cues starting together are merged."),
      OptionSpec::boolean("strip_formatting", "Remove formatting", false,
                          "Drop italic, bold and underline markup and keep plain text only."),
      OptionSpec::integer("max_line_length", "Maximum line length", 0, 0, 1000,
                          "Re-wrap cue text so that no line is longer than this many characters. 0 = keep lines.")
          .advanced(),
      OptionSpec::boolean("merge_lines", "Merge lines", false, "Join the lines of each cue into one line.")
          .advanced(),
      OptionSpec::boolean("drop_empty", "Drop empty cues", true, "Remove cues without visible text.").advanced(),
  };
}

std::vector<OptionSpec> common_text_write_options(std::string_view default_line_ending) {
  return {
      OptionSpec::choice("line_ending", "Line ending", std::string(default_line_ending), {"lf", "crlf"},
                         "Line terminator of the output file.")
          .advanced(),
      OptionSpec::boolean("bom", "Byte order mark", false, "Start the output with a UTF-8 byte order mark.").advanced(),
  };
}

Bytes finish_text(std::string text, const WriteContext& ctx) {
  if (ctx.options.get_enum("line_ending", "lf", {"lf", "crlf"}) == "crlf") {
    text = util::convert_line_endings(text, util::LineEnding::CrLf);
  }
  if (ctx.options.get_bool("bom", false)) text.insert(0, "\xEF\xBB\xBF");
  return to_bytes(text);
}

// ---------------------------------------------------------------------------- processing

namespace {

/// Number of visible characters (code points) of IR text, tags excluded.
std::size_t visible_length(std::string_view ir_text) {
  std::size_t count = 0;
  for (const MarkupToken& token : tokenize_markup(ir_text)) {
    if (token.type == MarkupToken::Type::Text) count += util::utf8_length(token.text);
  }
  return count;
}

/// Re-wrap IR text into lines of at most `width` visible characters,
/// balancing two-line cues.
std::string rewrap(std::string_view ir_text, std::size_t width) {
  // Words keep their tags; tags are zero-width.
  std::vector<std::string> words;
  std::string current;
  for (const char c : ir_text) {
    if (c == ' ' || c == '\n' || c == '\t') {
      if (!current.empty()) words.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) words.push_back(std::move(current));
  if (words.empty()) return {};
  std::size_t total = 0;
  for (const std::string& word : words) total += visible_length(word) + 1;
  // Two balanced lines when the text fits in two lines.
  std::size_t target = width;
  if (total - 1 > width && total - 1 <= 2 * width) target = std::max<std::size_t>((total + 1) / 2, 1);
  const auto fill = [&words](std::size_t limit) {
    std::string out;
    std::size_t line_length = 0;
    for (const std::string& word : words) {
      const std::size_t length = visible_length(word);
      if (line_length > 0 && line_length + 1 + length > limit) {
        out.push_back('\n');
        line_length = 0;
      } else if (line_length > 0) {
        out.push_back(' ');
        ++line_length;
      }
      out.append(word);
      line_length += length;
    }
    return out;
  };
  std::string balanced = fill(target);
  if (target != width && std::ranges::count(balanced, '\n') > 1) return fill(width);
  return balanced;
}

}  // namespace

Result<AnyIR> finish_read(ir::Subtitles subs, const ReadContext& ctx) {
  const Options& o = ctx.options;
  const std::int64_t offset = o.get_int("offset_ms", 0, -86'400'000, 86'400'000);
  const double fps_from = o.get_float("fps_from", 0.0, 0.0, 1000.0);
  const double fps_to = o.get_float("fps_to", 0.0, 0.0, 1000.0);
  const double stretch = (fps_from > 0.0 && fps_to > 0.0) ? fps_from / fps_to : 1.0;
  const bool drop_empty = o.get_bool("drop_empty", true);
  const bool strip = o.get_bool("strip_formatting", false);
  const bool merge = o.get_bool("merge_lines", false);
  const auto max_line = static_cast<std::size_t>(o.get_int("max_line_length", 0, 0, 1000));
  const std::int64_t min_duration = o.get_int("min_duration_ms", 0, 0, 60'000);
  const std::int64_t max_duration = o.get_int("max_duration_ms", 0, 0, 3'600'000);
  const bool fix_overlaps = o.get_bool("fix_overlaps", false);

  std::vector<ir::Cue> cues;
  cues.reserve(subs.cues.size());
  for (std::size_t i = 0; i < subs.cues.size(); ++i) {
    if ((i & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(subs.cues.size())));
    ir::Cue cue = std::move(subs.cues[i]);
    std::int64_t start = clamp_time(cue.start_ms);
    std::int64_t end = clamp_time(std::max(cue.end_ms, cue.start_ms));
    if (stretch != 1.0) {
      start = clamp_time(std::llround(static_cast<double>(start) * stretch));
      end = clamp_time(std::llround(static_cast<double>(end) * stretch));
    }
    if (offset != 0) {
      if (end + offset <= 0 && end > 0) continue;  // shifted entirely before the start of the media
      start = clamp_time(start + offset);
      end = clamp_time(end + offset);
    }
    cue.start_ms = start;
    cue.end_ms = end;
    cue.text = balance_markup(cue.text);
    if (strip) cue.text = strip_markup(cue.text);
    if (merge) {
      std::string merged;
      for (const std::string_view line : util::split(cue.text, '\n', false)) {
        const std::string_view trimmed = util::trim(line);
        if (trimmed.empty()) continue;
        if (!merged.empty()) merged.push_back(' ');
        merged.append(trimmed);
      }
      cue.text = std::move(merged);
    }
    if (max_line > 0) {
      bool too_long = false;
      for (const std::string_view line : util::split(cue.text, '\n')) too_long = too_long || visible_length(line) > max_line;
      if (too_long) cue.text = rewrap(cue.text, max_line);
    }
    if (drop_empty && is_blank_text(cue.text)) continue;
    cues.push_back(std::move(cue));
  }
  std::stable_sort(cues.begin(), cues.end(),
                   [](const ir::Cue& a, const ir::Cue& b) { return a.start_ms < b.start_ms; });

  if (fix_overlaps && cues.size() > 1) {
    std::vector<ir::Cue> merged;
    merged.reserve(cues.size());
    for (ir::Cue& cue : cues) {
      if (!merged.empty() && merged.back().start_ms == cue.start_ms) {
        ir::Cue& previous = merged.back();
        previous.text += "\n" + cue.text;
        previous.end_ms = std::max(previous.end_ms, cue.end_ms);
        continue;
      }
      merged.push_back(std::move(cue));
    }
    for (std::size_t i = 0; i + 1 < merged.size(); ++i) {
      if (merged[i].end_ms > merged[i + 1].start_ms) merged[i].end_ms = merged[i + 1].start_ms;
    }
    cues = std::move(merged);
  }
  for (std::size_t i = 0; i < cues.size(); ++i) {
    ir::Cue& cue = cues[i];
    if (max_duration > 0 && cue.end_ms - cue.start_ms > max_duration) cue.end_ms = cue.start_ms + max_duration;
    if (min_duration > 0 && cue.end_ms - cue.start_ms < min_duration) {
      std::int64_t wanted = cue.start_ms + min_duration;
      if (i + 1 < cues.size()) wanted = std::min(wanted, std::max(cue.end_ms, cues[i + 1].start_ms));
      cue.end_ms = clamp_time(wanted);
    }
  }
  subs.cues = std::move(cues);
  return AnyIR(std::move(subs));
}

// ---------------------------------------------------------------------------- codecs

Codec make_text_codec(std::string format_id, TextParser parse, TextWriter write,
                      std::vector<OptionSpec> extra_read_options, std::vector<OptionSpec> extra_write_options,
                      std::string_view default_line_ending) {
  Codec codec;
  codec.format_id = std::move(format_id);
  codec.kind = Kind::Subtitles;
  if (parse) {
    codec.read = [parse = std::move(parse)](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const std::string text, decode_input(ctx));
      CVT_TRY_ASSIGN(ir::Subtitles subs, parse(text, ctx));
      return finish_read(std::move(subs), ctx);
    };
    codec.read_options = common_read_options();
    for (OptionSpec& spec : extra_read_options) codec.read_options.push_back(std::move(spec));
  }
  if (write) {
    codec.write = [write = std::move(write)](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(std::string text, write(value.get<ir::Subtitles>(), ctx));
      return finish_text(std::move(text), ctx);
    };
    codec.write_options = common_text_write_options(default_line_ending);
    for (OptionSpec& spec : extra_write_options) codec.write_options.push_back(std::move(spec));
  }
  return codec;
}

FormatInfo subtitle_format(std::string id, std::string name, std::string description,
                           std::vector<std::string> extensions, std::string mime, int popularity) {
  FormatInfo info;
  info.id = std::move(id);
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Subtitle;
  info.extensions = std::move(extensions);
  info.mime = std::move(mime);
  info.popularity = popularity;
  return info;
}

// ---------------------------------------------------------------------------- time

std::optional<std::int64_t> parse_clock(std::string_view text, bool minutes_first) {
  std::string compact;
  for (const char c : text) {
    if (c != ' ' && c != '\t') compact.push_back(c);
  }
  std::string_view s = compact;
  bool negative = false;
  if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
    negative = s.front() == '-';
    s.remove_prefix(1);
  }
  if (s.empty()) return std::nullopt;
  // Fraction after the last '.' or ','.
  std::string_view fraction;
  const std::size_t dot = s.find_last_of(".,");
  if (dot != std::string_view::npos) {
    fraction = s.substr(dot + 1);
    s = s.substr(0, dot);
    if (fraction.empty() || fraction.size() > 9) return std::nullopt;
  }
  std::vector<std::string_view> parts = util::split(s, ':');
  if (parts.empty() || parts.size() > 4) return std::nullopt;
  if (parts.size() == 4) {
    if (!fraction.empty()) return std::nullopt;
    fraction = parts.back();
    parts.pop_back();
    if (fraction.empty() || fraction.size() > 9) return std::nullopt;
  }
  std::vector<std::int64_t> values;
  for (const std::string_view part : parts) {
    if (part.empty() || part.size() > 9) return std::nullopt;
    const auto value = util::parse_int(part);
    if (!value || *value < 0) return std::nullopt;
    values.push_back(*value);
  }
  for (const char c : fraction) {
    if (!util::is_ascii_digit(c)) return std::nullopt;
  }
  std::int64_t hours = 0;
  std::int64_t minutes = 0;
  std::int64_t seconds = 0;
  if (values.size() == 3) {
    hours = values[0];
    minutes = values[1];
    seconds = values[2];
  } else if (values.size() == 2) {
    if (minutes_first || !fraction.empty()) {
      minutes = values[0];
      seconds = values[1];
    } else {
      hours = values[0];
      minutes = values[1];
    }
  } else {
    seconds = values[0];
  }
  if (hours > 1000 || minutes > 100000 || seconds > 10'000'000) return std::nullopt;
  std::int64_t ms = 0;
  if (!fraction.empty()) {
    std::string padded(fraction.substr(0, 3));
    while (padded.size() < 3) padded.push_back('0');
    ms = *util::parse_int(padded);
  }
  const std::int64_t total = ((hours * 60 + minutes) * 60 + seconds) * 1000 + ms;
  return negative ? -total : total;
}

std::string format_clock(std::int64_t ms, const ClockStyle& style) {
  ms = std::clamp<std::int64_t>(ms, 0, max_time_ms);
  std::int64_t fraction = ms % 1000;
  std::int64_t total_seconds = ms / 1000;
  if (style.fraction_digits == 2) {
    fraction = (fraction + 5) / 10;
    if (fraction == 100) {
      fraction = 0;
      ++total_seconds;
    }
  } else if (style.fraction_digits == 1) {
    fraction = (fraction + 50) / 100;
    if (fraction == 10) {
      fraction = 0;
      ++total_seconds;
    }
  } else if (style.fraction_digits == 0) {
    if (fraction >= 500) ++total_seconds;
    fraction = 0;
  }
  const std::int64_t hours = total_seconds / 3600;
  const std::int64_t minutes = (total_seconds / 60) % 60;
  const std::int64_t seconds = total_seconds % 60;
  std::string out;
  if (style.hour_digits > 0 || hours > 0) {
    out = std::format("{:0{}}:", hours, std::max(style.hour_digits, 1));
  }
  out += std::format("{:02}:{:02}", minutes, seconds);
  if (style.fraction_digits > 0) {
    out.push_back(style.fraction_separator);
    out += std::format("{:0{}}", fraction, std::min(style.fraction_digits, 3));
  }
  return out;
}

std::int64_t ms_to_frames(std::int64_t ms, double fps) {
  if (!(fps > 0.0)) return 0;
  return std::llround(static_cast<double>(ms) * fps / 1000.0);
}

std::int64_t frames_to_ms(std::int64_t frames, double fps) {
  if (!(fps > 0.0)) return 0;
  return clamp_time(std::llround(static_cast<double>(frames) * 1000.0 / fps));
}

// ---------------------------------------------------------------------------- markup

std::vector<MarkupToken> tokenize_markup(std::string_view ir_text) {
  std::vector<MarkupToken> tokens;
  std::string text;
  const auto flush = [&] {
    if (!text.empty()) tokens.push_back({MarkupToken::Type::Text, std::move(text)});
    text.clear();
  };
  std::size_t i = 0;
  while (i < ir_text.size()) {
    const char c = ir_text[i];
    if (c == '\n') {
      flush();
      tokens.push_back({MarkupToken::Type::LineBreak, {}});
      ++i;
      continue;
    }
    if (c == '<') {
      const std::string_view rest = ir_text.substr(i);
      if (rest.size() >= 3 && rest[2] == '>' && is_inline_tag(rest.substr(1, 1))) {
        flush();
        tokens.push_back({MarkupToken::Type::Open, std::string(rest.substr(1, 1))});
        i += 3;
        continue;
      }
      if (rest.size() >= 4 && rest[1] == '/' && rest[3] == '>' && is_inline_tag(rest.substr(2, 1))) {
        flush();
        tokens.push_back({MarkupToken::Type::Close, std::string(rest.substr(2, 1))});
        i += 4;
        continue;
      }
    }
    if (c != '\r') text.push_back(c);
    ++i;
  }
  flush();
  return tokens;
}

std::string html_to_ir(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '<') {
      const std::size_t close = text.find('>', i + 1);
      if (close == std::string_view::npos) {
        out.push_back('<');
        ++i;
        continue;
      }
      std::string_view tag = util::trim(text.substr(i + 1, close - i - 1));
      bool closing = false;
      if (!tag.empty() && tag.front() == '/') {
        closing = true;
        tag = util::trim_left(tag.substr(1));
      }
      std::size_t name_end = 0;
      while (name_end < tag.size() && (util::is_ascii_alpha(tag[name_end]) || util::is_ascii_digit(tag[name_end]))) {
        ++name_end;
      }
      const std::string name = util::to_lower(tag.substr(0, name_end));
      // Something like "a < b" is text, not a tag.
      const bool looks_like_tag = !name.empty() || (!tag.empty() && (util::is_ascii_digit(tag[0]) || tag[0] == '!'));
      if (!looks_like_tag && !closing) {
        out.push_back('<');
        ++i;
        continue;
      }
      std::string mapped;
      if (name == "i" || name == "em") mapped = "i";
      if (name == "b" || name == "strong") mapped = "b";
      if (name == "u") mapped = "u";
      if (!mapped.empty()) {
        out += closing ? "</" + mapped + ">" : "<" + mapped + ">";
      } else if (name == "br") {
        out.push_back('\n');
      }
      i = close + 1;
      continue;
    }
    if (c == '&') {
      if (const auto reference = decode_reference(text, i)) {
        out.append(reference->first);
        i += reference->second;
        continue;
      }
    }
    if (c != '\r') out.push_back(c);
    ++i;
  }
  return balance_markup(out);
}

std::string strip_markup(std::string_view ir_text) {
  std::string out;
  for (const MarkupToken& token : tokenize_markup(ir_text)) {
    if (token.type == MarkupToken::Type::Text) out.append(token.text);
    if (token.type == MarkupToken::Type::LineBreak) out.push_back('\n');
  }
  return out;
}

std::string ir_to_html(std::string_view ir_text) {
  std::string out;
  for (const MarkupToken& token : tokenize_markup(ir_text)) {
    switch (token.type) {
      case MarkupToken::Type::Text:
        for (const char c : token.text) {
          if (c == '&') out += "&amp;";
          else if (c == '<') out += "&lt;";
          else if (c == '>') out += "&gt;";
          else out.push_back(c);
        }
        break;
      case MarkupToken::Type::Open: out += "<" + token.text + ">"; break;
      case MarkupToken::Type::Close: out += "</" + token.text + ">"; break;
      case MarkupToken::Type::LineBreak: out.push_back('\n'); break;
    }
  }
  return out;
}

std::string balance_markup(std::string_view ir_text) {
  std::vector<std::string> open;
  std::string out;
  for (const MarkupToken& token : tokenize_markup(ir_text)) {
    switch (token.type) {
      case MarkupToken::Type::Text: out.append(token.text); break;
      case MarkupToken::Type::LineBreak: out.push_back('\n'); break;
      case MarkupToken::Type::Open:
        if (std::ranges::find(open, token.text) != open.end()) break;  // already open: ignore the duplicate
        open.push_back(token.text);
        out += "<" + token.text + ">";
        break;
      case MarkupToken::Type::Close: {
        const auto it = std::ranges::find(open, token.text);
        if (it == open.end()) break;
        open.erase(it);
        out += "</" + token.text + ">";
        break;
      }
    }
  }
  for (auto it = open.rbegin(); it != open.rend(); ++it) out += "</" + *it + ">";
  // Empty pairs such as "<i></i>" carry nothing.
  for (const std::string_view tag : {"i"sv, "b"sv, "u"sv}) {
    const std::string pair = std::format("<{}></{}>", tag, tag);
    for (std::size_t at = out.find(pair); at != std::string::npos; at = out.find(pair)) out.erase(at, pair.size());
  }
  // Trailing white space at line ends and of the whole cue.
  std::string cleaned;
  for (const std::string_view line : util::split(out, '\n')) {
    cleaned.append(util::trim_right(line));
    cleaned.push_back('\n');
  }
  while (!cleaned.empty() && (cleaned.back() == '\n')) cleaned.pop_back();
  while (!cleaned.empty() && cleaned.front() == '\n') cleaned.erase(0, 1);
  return cleaned;
}

// ---------------------------------------------------------------------------- text helpers

std::vector<std::string_view> text_lines(std::string_view text) {
  std::vector<std::string_view> lines = util::split_lines(text);
  for (std::string_view& line : lines) line = util::trim_right(line);
  return lines;
}

bool is_blank_text(std::string_view ir_text) {
  for (const MarkupToken& token : tokenize_markup(ir_text)) {
    if (token.type != MarkupToken::Type::Text) continue;
    std::size_t i = 0;
    while (i < token.text.size()) {
      const util::Utf8Decoded d = util::decode_utf8(token.text, i);
      i += d.length == 0 ? 1 : d.length;
      const char32_t c = d.code_point;
      if (c == ' ' || c == '\t' || c == 0xA0 || c == 0x200B || c == 0x200E || c == 0x200F || c == 0xFEFF) continue;
      return false;
    }
  }
  return true;
}

std::string sniff_prefix(ByteView data, std::size_t limit) {
  const ByteView head = data.first(std::min(data.size(), limit));
  const util::EncodingGuess guess = util::guess_encoding(head);
  std::size_t take = head.size();
  if (guess.encoding == util::TextEncoding::Utf16LE || guess.encoding == util::TextEncoding::Utf16BE) take &= ~std::size_t{1};
  if (guess.encoding == util::TextEncoding::Utf32LE || guess.encoding == util::TextEncoding::Utf32BE) take &= ~std::size_t{3};
  std::string text = util::decode_text(head.first(take), guess.encoding).text;
  if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
  // Leading blank lines do not matter to any sniffer.
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return text.substr(first);
}

}  // namespace cvt::subtitle
