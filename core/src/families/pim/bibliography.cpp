// Convertible core - pim family: bibliographies.
//
//   bibtex       BibTeX and BibLaTeX (.bib): @string macros, # concatenation,
//                month macros, LaTeX accents <-> Unicode, name parsing
//   ris          RIS (Research Information Systems)
//   endnote-xml  EndNote XML export
//   enw          EndNote / refer tagged format (%A, %T, ...)
//   csl-json     Citation Style Language JSON
//   nbib         MEDLINE / PubMed (.nbib)
//   mods         Library of Congress MODS XML
//   refworks     RefWorks tagged format
//   isi          Web of Science / ISI export
//
// Every format reads into one record model carried as Data: an array of
// CSL-JSON items ("id", "type", "title", "author": [{"family", "given",
// "non-dropping-particle", "suffix"} | {"literal"}], "editor", "issued":
// {"date-parts": [[y, m, d]]}, "container-title", "volume", "issue", "page",
// "DOI", "URL", "ISBN", "ISSN", "publisher", "publisher-place", "abstract",
// "keyword", "note", "language", "edition", "collection-title", "number",
// "accessed"), plus "custom" for source fields without a CSL equivalent so
// that a format can round-trip its own extras. Each format also has a Table
// reader (one row per record) and a Document reader (a formatted reference
// list in an author-date style), and writers from Data.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/pim/bib_latex.hpp"
#include "families/pim/pim.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/table.hpp"
#include "util/json.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::pim {

namespace {

using namespace std::string_view_literals;

constexpr std::size_t max_records = 1'000'000;

// ============================================================================ record helpers

void set(Json& item, std::string_view key, std::string value) {
  value = std::string(util::trim(value));
  if (!value.empty()) item[key] = std::move(value);
}

std::string str(const Json& item, std::string_view key) {
  const Json& value = item[key];
  if (value.is_string()) return value.as_string();
  if (value.is_number()) return util::format_double(value.as_double());
  return {};
}

/// "Last, First", "Last, Jr., First" or "First Last" into a CSL name.
Json parse_plain_name(std::string_view text) {
  text = util::trim(text);
  std::vector<std::string_view> parts = util::split(text, ',');
  for (std::string_view& part : parts) part = util::trim(part);
  Json name = Json::object();
  if (parts.size() >= 3) {
    // "Last, Jr., First" (BibTeX, EndNote) or "Last, First, Jr." (RIS).
    const auto suffix_like = [](std::string_view word) {
      const std::string w = util::to_lower(word);
      return w == "jr" || w == "jr." || w == "sr" || w == "sr." || w == "ii" || w == "iii" || w == "iv" || w == "v";
    };
    const bool suffix_last = suffix_like(parts[2]) && !suffix_like(parts[1]);
    name["family"] = std::string(parts[0]);
    name["suffix"] = std::string(suffix_last ? parts[2] : parts[1]);
    name["given"] = std::string(suffix_last ? parts[1] : parts[2]);
  } else if (parts.size() == 2) {
    name["family"] = std::string(parts[0]);
    if (!parts[1].empty()) name["given"] = std::string(parts[1]);
  } else {
    const std::size_t space = text.rfind(' ');
    if (space == std::string_view::npos) {
      name["family"] = std::string(text);
    } else {
      name["family"] = std::string(text.substr(space + 1));
      name["given"] = std::string(util::trim(text.substr(0, space)));
    }
  }
  return name;
}

/// "Family, Given" (with particle and suffix) for tagged formats: the
/// suffix goes between family and given names, or last (RIS convention).
std::string name_text(const Json& name, bool suffix_last = false) {
  if (!name["literal"].as_string().empty()) return name["literal"].as_string();
  std::string family = name["family"].as_string();
  if (!name["non-dropping-particle"].as_string().empty()) family = name["non-dropping-particle"].as_string() + " " + family;
  std::string out = family;
  if (!suffix_last && !name["suffix"].as_string().empty()) out += ", " + name["suffix"].as_string();
  if (!name["given"].as_string().empty()) out += ", " + name["given"].as_string();
  if (suffix_last && !name["suffix"].as_string().empty()) out += ", " + name["suffix"].as_string();
  return out;
}

void add_name(Json& item, std::string_view role, Json name) {
  if (name.empty()) return;
  item[role].push_back(std::move(name));
}

/// Year, month, day from free text: "2020", "2020-05-01", "2020/05/01/", "2020 May 1", "May 2020".
std::vector<int> parse_date_parts(std::string_view text) {
  static constexpr std::array<std::string_view, 12> months = {"jan", "feb", "mar", "apr", "may", "jun",
                                                              "jul", "aug", "sep", "oct", "nov", "dec"};
  std::vector<int> numbers;
  int year = 0;
  int month = 0;
  int day = 0;
  std::string token;
  const auto flush = [&] {
    if (token.empty()) return;
    if (util::is_ascii_digit(token[0])) {
      const int value = static_cast<int>(util::parse_int(token).value_or(0));
      if (token.size() == 4 && year == 0) year = value;
      else if (month == 0 && value >= 1 && value <= 12 && year != 0) month = value;
      else if (day == 0 && value >= 1 && value <= 31) day = value;
    } else {
      for (std::size_t m = 0; m < months.size(); ++m) {
        if (util::istarts_with(token, months[m]) && month == 0) month = static_cast<int>(m) + 1;
      }
    }
    token.clear();
  };
  for (const char c : text) {
    if (util::is_ascii_digit(c) || util::is_ascii_alpha(c)) token.push_back(c);
    else flush();
  }
  flush();
  if (year == 0) return {};
  numbers.push_back(year);
  if (month != 0) numbers.push_back(month);
  if (month != 0 && day != 0) numbers.push_back(day);
  return numbers;
}

void set_issued(Json& item, std::string_view text) {
  const std::vector<int> parts = parse_date_parts(text);
  if (parts.empty()) return;
  Json date = Json::array();
  for (const int part : parts) date.push_back(part);
  item["issued"] = Json::object({{"date-parts", Json::array({std::move(date)})}});
}

std::vector<int> issued(const Json& item) {
  std::vector<int> out;
  for (const Json& part : item["issued"]["date-parts"][0].elements()) out.push_back(static_cast<int>(part.as_int64(0)));
  if (out.empty() && !item["issued"]["raw"].as_string().empty()) out = parse_date_parts(item["issued"]["raw"].as_string());
  return out;
}

std::string year_of(const Json& item) {
  const std::vector<int> parts = issued(item);
  return parts.empty() ? std::string() : std::to_string(parts[0]);
}

std::string iso_date(const Json& item, std::string_view key = "issued") {
  std::vector<int> parts;
  for (const Json& part : item[key]["date-parts"][0].elements()) parts.push_back(static_cast<int>(part.as_int64(0)));
  if (parts.empty()) return {};
  std::string out = std::format("{:04}", parts[0]);
  if (parts.size() > 1) out += std::format("-{:02}", parts[1]);
  if (parts.size() > 2) out += std::format("-{:02}", parts[2]);
  return out;
}

/// "12-34" page ranges from separate first and last pages.
std::string page_range(std::string_view first, std::string_view last) {
  if (first.empty()) return std::string(last);
  if (last.empty()) return std::string(first);
  return std::string(first) + "-" + std::string(last);
}
std::pair<std::string, std::string> split_pages(const std::string& pages) {
  const std::size_t dash = pages.find_first_of("-\xE2");
  if (dash == std::string::npos) return {pages, {}};
  std::size_t rest = dash + 1;
  while (rest < pages.size() && (pages[rest] == '-' || static_cast<unsigned char>(pages[rest]) >= 0x80)) ++rest;
  return {std::string(util::trim(pages.substr(0, dash))), std::string(util::trim(pages.substr(rest)))};
}

void add_keywords(Json& item, std::string_view text, char separator) {
  std::vector<std::string> keywords;
  if (const Json* existing = item.find("keyword")) {
    for (const std::string_view k : util::split(existing->as_string(), ',', false)) keywords.emplace_back(util::trim(k));
  }
  for (const std::string_view k : util::split(text, separator, false)) {
    if (!util::trim(k).empty()) keywords.emplace_back(util::trim(k));
  }
  if (!keywords.empty()) item["keyword"] = util::join(keywords, ", ");
}
std::vector<std::string> keywords_of(const Json& item) {
  std::vector<std::string> out;
  for (const std::string_view k : util::split(item["keyword"].as_string(), ',', false)) {
    if (!util::trim(k).empty()) out.emplace_back(util::trim(k));
  }
  return out;
}

// ---------------------------------------------------------------------------- type vocabularies

struct TypeName {
  std::string_view csl;
  std::string_view bibtex;
  std::string_view ris;
  std::string_view endnote;  ///< EndNote reference type name (XML / %0 / RefWorks RT)
  int endnote_number;
};

constexpr std::array<TypeName, 16> types = {{
    {"article-journal", "article", "JOUR", "Journal Article", 17},
    {"book", "book", "BOOK", "Book", 6},
    {"chapter", "incollection", "CHAP", "Book Section", 5},
    {"paper-conference", "inproceedings", "CPAPER", "Conference Paper", 47},
    {"thesis", "phdthesis", "THES", "Thesis", 32},
    {"report", "techreport", "RPRT", "Report", 27},
    {"webpage", "online", "ELEC", "Web Page", 12},
    {"article-newspaper", "article", "NEWS", "Newspaper Article", 23},
    {"article-magazine", "article", "MGZN", "Magazine Article", 19},
    {"dataset", "dataset", "DATA", "Dataset", 59},
    {"software", "software", "COMP", "Computer Program", 9},
    {"patent", "patent", "PAT", "Patent", 25},
    {"manuscript", "unpublished", "UNPB", "Unpublished Work", 34},
    {"entry-encyclopedia", "inreference", "ENCYC", "Encyclopedia", 53},
    {"motion_picture", "movie", "VIDEO", "Film or Broadcast", 21},
    {"document", "misc", "GEN", "Generic", 13},
}};

const TypeName& type_of(std::string_view csl) {
  for (const TypeName& type : types) {
    if (type.csl == csl) return type;
  }
  if (csl == "article") return types[0];
  return types.back();
}

std::string csl_from_bibtex(std::string_view bibtex) {
  const std::string t = util::to_lower(bibtex);
  if (t == "article") return "article-journal";
  if (t == "book" || t == "proceedings" || t == "collection" || t == "mvbook" || t == "booklet") return "book";
  if (t == "inbook" || t == "incollection" || t == "bookinbook" || t == "suppbook") return "chapter";
  if (t == "inproceedings" || t == "conference") return "paper-conference";
  if (t == "phdthesis" || t == "mastersthesis" || t == "thesis") return "thesis";
  if (t == "techreport" || t == "report") return "report";
  if (t == "online" || t == "electronic" || t == "www") return "webpage";
  if (t == "unpublished") return "manuscript";
  if (t == "manual") return "book";
  if (t == "patent") return "patent";
  if (t == "dataset") return "dataset";
  if (t == "software") return "software";
  if (t == "inreference") return "entry-encyclopedia";
  if (t == "movie" || t == "video") return "motion_picture";
  return "document";
}

std::string csl_from_ris(std::string_view ris) {
  for (const TypeName& type : types) {
    if (util::iequals(type.ris, ris)) return std::string(type.csl);
  }
  if (util::iequals(ris, "CONF")) return "paper-conference";
  if (util::iequals(ris, "WEB")) return "webpage";
  if (util::iequals(ris, "EJOUR") || util::iequals(ris, "JFULL")) return "article-journal";
  if (util::iequals(ris, "EBOOK")) return "book";
  if (util::iequals(ris, "ECHAP")) return "chapter";
  return "document";
}

std::string csl_from_endnote(std::string_view name) {
  for (const TypeName& type : types) {
    if (util::iequals(type.endnote, name)) return std::string(type.csl);
  }
  if (util::iequals(name, "Conference Proceedings")) return "paper-conference";
  if (util::iequals(name, "Edited Book")) return "book";
  if (util::iequals(name, "Electronic Article")) return "article-journal";
  if (util::iequals(name, "Electronic Source") || util::iequals(name, "Online Database")) return "webpage";
  if (util::iequals(name, "Dissertation")) return "thesis";
  return "document";
}

// ============================================================================ LaTeX

/// Text of a LaTeX command argument at `text[i]`: a {group} or one character.
std::string_view latex_argument(std::string_view text, std::size_t& i) {
  while (i < text.size() && text[i] == ' ') ++i;
  if (i >= text.size()) return {};
  if (text[i] == '{') {
    int depth = 0;
    const std::size_t start = i;
    for (; i < text.size(); ++i) {
      if (text[i] == '{') ++depth;
      else if (text[i] == '}' && --depth == 0) break;
    }
    const std::string_view inside = text.substr(start + 1, (i < text.size() ? i : text.size()) - start - 1);
    if (i < text.size()) ++i;
    return inside;
  }
  if (text[i] == '\\') {
    const std::size_t start = i++;
    while (i < text.size() && util::is_ascii_alpha(text[i])) ++i;
    return text.substr(start, i - start);
  }
  return text.substr(i++, 1);
}

struct Symbol {
  std::string_view command;
  std::string_view text;
};
constexpr std::array<Symbol, 38> latex_symbols = {{
    {"aa", "å"},          {"AA", "Å"},           {"ae", "æ"},          {"AE", "Æ"},           {"oe", "œ"},
    {"OE", "Œ"},          {"o", "ø"},            {"O", "Ø"},           {"ss", "ß"},           {"l", "ł"},
    {"L", "Ł"},           {"i", "ı"},            {"j", "ȷ"},           {"dh", "ð"},           {"DH", "Ð"},
    {"th", "þ"},          {"TH", "Þ"},           {"textendash", "–"},  {"textemdash", "—"},   {"textquoteleft", "‘"},
    {"textquoteright", "’"}, {"textquotedblleft", "“"}, {"textquotedblright", "”"}, {"S", "§"}, {"P", "¶"},
    {"copyright", "©"},   {"textregistered", "®"}, {"texttrademark", "™"}, {"euro", "€"},     {"pounds", "£"},
    {"dag", "†"},         {"ddag", "‡"},         {"ldots", "…"},       {"dots", "…"},         {"textbackslash", "\\"},
    {"textasciitilde", "~"}, {"textunderscore", "_"}, {"textdegree", "°"},
}};

std::string latex_to_unicode(std::string_view text, int depth = 0) {
  if (depth > 32) return std::string(text);
  std::string out;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '\\' && i + 1 < text.size()) {
      const char next = text[i + 1];
      if (!util::is_ascii_alpha(next)) {
        // \' \` \^ \" \~ \= \. accents; \& \% ... escapes.
        if (next == '\'' || next == '`' || next == '^' || next == '"' || next == '~' || next == '=' || next == '.') {
          i += 2;
          const std::string_view arg = latex_argument(text, i);
          char base = arg == "\\i" ? 'i' : arg == "\\j" ? 'j' : arg.empty() ? ' ' : arg[0];
          char32_t composed = 0;
          for (const latex::Accent& accent : latex::accents) {
            if (accent.command == next && accent.base == base) composed = accent.code_point;
          }
          if (composed != 0 && (arg.size() == 1 || arg == "\\i" || arg == "\\j")) {
            util::append_utf8(out, composed);
          } else {
            out += latex_to_unicode(arg, depth + 1);
          }
          continue;
        }
        if (next == '\\') out.push_back(' ');
        else if (next == ' ') out.push_back(' ');
        else if (next != '-') out.push_back(next);  // \- is a discretionary hyphen
        i += 2;
        continue;
      }
      std::size_t end = i + 1;
      while (end < text.size() && util::is_ascii_alpha(text[end])) ++end;
      const std::string_view command = text.substr(i + 1, end - i - 1);
      i = end;
      if (command.size() == 1 && std::string_view("cvuHkrdb").find(command[0]) != std::string_view::npos) {
        const std::string_view arg = latex_argument(text, i);
        const char base = arg == "\\i" ? 'i' : arg.empty() ? ' ' : arg[0];
        char32_t composed = 0;
        for (const latex::Accent& accent : latex::accents) {
          if (accent.command == command[0] && accent.base == base) composed = accent.code_point;
        }
        if (composed != 0 && (arg.size() == 1 || arg == "\\i")) util::append_utf8(out, composed);
        else out += latex_to_unicode(arg, depth + 1);
        continue;
      }
      bool known = false;
      for (const Symbol& symbol : latex_symbols) {
        if (symbol.command == command) {
          out += symbol.text;
          known = true;
          break;
        }
      }
      // A space after a control word is part of it.
      if (known) {
        if (i < text.size() && text[i] == ' ') ++i;
        if (i + 1 < text.size() && text[i] == '{' && text[i + 1] == '}') i += 2;
        continue;
      }
      // \emph{x}, \textit{x}, \url{x}, ...: keep the argument.
      while (i < text.size() && text[i] == ' ') ++i;
      if (i < text.size() && text[i] == '{') out += latex_to_unicode(latex_argument(text, i), depth + 1);
      continue;
    }
    if (c == '{' || c == '}') {
      ++i;
      continue;
    }
    if (c == '$') {  // math: keep the content
      ++i;
      continue;
    }
    if (c == '~') {
      out += "\xC2\xA0";
      ++i;
      continue;
    }
    if (c == '-' && text.substr(i).starts_with("---")) {
      out += "—";
      i += 3;
      continue;
    }
    if (c == '-' && text.substr(i).starts_with("--")) {
      out += "–";
      i += 2;
      continue;
    }
    if (text.substr(i).starts_with("``")) {
      out += "“";
      i += 2;
      continue;
    }
    if (text.substr(i).starts_with("''")) {
      out += "”";
      i += 2;
      continue;
    }
    // Line breaks and runs of white space become one space.
    if (util::is_ascii_space(c)) {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
      ++i;
      continue;
    }
    out.push_back(c);
    ++i;
  }
  return std::string(util::trim(out));
}

std::string unicode_to_latex(std::string_view text, bool ascii) {
  std::string out;
  std::size_t i = 0;
  while (i < text.size()) {
    const util::Utf8Decoded d = util::decode_utf8(text, i);
    const std::size_t length = d.length == 0 ? 1 : d.length;
    const char32_t c = d.code_point;
    i += length;
    switch (c) {
      case '&': out += "\\&"; continue;
      case '%': out += "\\%"; continue;
      case '$': out += "\\$"; continue;
      case '#': out += "\\#"; continue;
      case '_': out += "\\_"; continue;
      case '{': out += "\\{"; continue;
      case '}': out += "\\}"; continue;
      case '~': out += "\\textasciitilde{}"; continue;
      case '\\': out += "\\textbackslash{}"; continue;
      case 0xA0: out += "~"; continue;
      case 0x2013: out += "--"; continue;
      case 0x2014: out += "---"; continue;
      default: break;
    }
    if (c < 0x80 || !ascii) {
      out.append(text.substr(i - length, length));
      continue;
    }
    bool done = false;
    for (const latex::Accent& accent : latex::accents) {
      if (accent.code_point == c) {
        const bool letter = util::is_ascii_alpha(accent.command);
        out += letter ? std::format("{{\\{} {}}}", accent.command, accent.base) : std::format("{{\\{}{}}}", accent.command, accent.base);
        done = true;
        break;
      }
    }
    if (!done) {
      std::string utf8;
      util::append_utf8(utf8, c);
      for (const Symbol& symbol : latex_symbols) {
        if (symbol.text == utf8 && symbol.command != "dots" && symbol.command != "textbackslash" && symbol.command != "textunderscore" && symbol.command != "textasciitilde") {
          out += "{\\" + std::string(symbol.command) + "}";
          done = true;
          break;
        }
      }
    }
    if (!done) out.append(text.substr(i - length, length));  // no LaTeX spelling: keep UTF-8
  }
  return out;
}

// ============================================================================ BibTeX parsing

/// Split a BibTeX name list on " and " at brace depth 0.
std::vector<std::string> split_names(std::string_view text) {
  std::vector<std::string> out;
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '{') ++depth;
    else if (text[i] == '}') --depth;
    else if (depth == 0 && util::is_ascii_space(text[i]) && i + 4 < text.size() && util::iequals(text.substr(i + 1, 3), "and") &&
             util::is_ascii_space(text[i + 4])) {
      out.emplace_back(util::trim(text.substr(start, i - start)));
      start = i + 5;
      i += 4;
    }
  }
  out.emplace_back(util::trim(text.substr(start)));
  std::erase_if(out, [](const std::string& s) { return s.empty(); });
  return out;
}

/// Words of a name at brace depth 0 (spaces and '~' separate).
std::vector<std::string> name_words(std::string_view text) {
  std::vector<std::string> out;
  std::string current;
  int depth = 0;
  for (const char c : text) {
    if (c == '{') ++depth;
    if (c == '}') --depth;
    if (depth == 0 && (util::is_ascii_space(c) || c == '~')) {
      if (!current.empty()) out.push_back(std::move(current));
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) out.push_back(std::move(current));
  return out;
}

/// "von" words start with a lower-case letter (after LaTeX is resolved).
bool lowercase_word(const std::string& word) {
  if (word.starts_with('{')) return false;  // {Van Gogh} is protected
  const std::string plain = latex_to_unicode(word);
  if (plain.empty()) return false;
  const util::Utf8Decoded d = util::decode_utf8(plain, 0);
  const char32_t c = d.code_point;
  return (c >= 'a' && c <= 'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7);
}

Json bibtex_name(std::string_view raw) {
  Json name = Json::object();
  // {Corporate Name}
  if (raw.starts_with('{') && raw.ends_with('}')) {
    int depth = 0;
    bool single = true;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '{') ++depth;
      if (raw[i] == '}' && --depth == 0 && i + 1 != raw.size()) single = false;
    }
    if (single) {
      name["literal"] = latex_to_unicode(raw);
      return name;
    }
  }
  std::vector<std::string> parts;
  {
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '{') ++depth;
      else if (raw[i] == '}') --depth;
      else if (raw[i] == ',' && depth == 0) {
        parts.emplace_back(util::trim(raw.substr(start, i - start)));
        start = i + 1;
      }
    }
    parts.emplace_back(util::trim(raw.substr(start)));
  }
  std::vector<std::string> first;
  std::vector<std::string> von;
  std::vector<std::string> last;
  std::string jr;
  if (parts.size() == 1) {
    // First von Last
    const std::vector<std::string> words = name_words(parts[0]);
    std::size_t i = 0;
    while (i + 1 < words.size() && !lowercase_word(words[i])) first.push_back(words[i++]);
    while (i + 1 < words.size() && lowercase_word(words[i])) von.push_back(words[i++]);
    while (i < words.size()) last.push_back(words[i++]);
  } else {
    // von Last, (Jr,) First
    const std::vector<std::string> words = name_words(parts[0]);
    std::size_t i = 0;
    while (i + 1 < words.size() && lowercase_word(words[i])) von.push_back(words[i++]);
    while (i < words.size()) last.push_back(words[i++]);
    if (parts.size() >= 3) {
      jr = parts[1];
      first = name_words(parts[2]);
    } else {
      first = name_words(parts[1]);
    }
  }
  const auto join = [](const std::vector<std::string>& words) {
    std::vector<std::string> converted;
    for (const std::string& word : words) converted.push_back(latex_to_unicode(word));
    return util::join(converted, " ");
  };
  if (!last.empty()) name["family"] = join(last);
  if (!first.empty()) name["given"] = join(first);
  if (!von.empty()) name["non-dropping-particle"] = join(von);
  if (!jr.empty()) name["suffix"] = latex_to_unicode(jr);
  return name;
}

class BibParser {
 public:
  BibParser(std::string_view text, const Limits& limits) : text_(text), limits_(limits) {
    static constexpr std::array<std::string_view, 12> months = {"jan", "feb", "mar", "apr", "may", "jun",
                                                                "jul", "aug", "sep", "oct", "nov", "dec"};
    for (std::size_t m = 0; m < months.size(); ++m) macros_[std::string(months[m])] = std::to_string(m + 1);
  }

  struct Entry {
    std::string type;
    std::string key;
    std::vector<std::pair<std::string, std::string>> fields;  // lower-case names, raw LaTeX values
  };

  Result<std::vector<Entry>> parse() {
    std::vector<Entry> entries;
    while (true) {
      const std::size_t at = text_.find('@', pos_);
      if (at == std::string_view::npos) break;
      pos_ = at + 1;
      std::string type = util::to_lower(identifier());
      skip_space();
      if (pos_ >= text_.size() || (text_[pos_] != '{' && text_[pos_] != '(')) continue;
      const char close = text_[pos_] == '{' ? '}' : ')';
      ++pos_;
      if (type == "comment") {
        skip_group(close);
        continue;
      }
      if (type == "preamble") {
        (void)value(close);
        skip_to(close);
        continue;
      }
      if (type == "string") {
        skip_space();
        const std::string name = util::to_lower(identifier());
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '=') {
          ++pos_;
          CVT_TRY_ASSIGN(std::string v, value(close));
          if (macros_.size() < 100000) macros_[name] = std::move(v);
        }
        skip_to(close);
        continue;
      }
      Entry entry;
      entry.type = type;
      skip_space();
      const std::size_t key_start = pos_;
      while (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != close && !util::is_ascii_space(text_[pos_])) ++pos_;
      entry.key = std::string(text_.substr(key_start, pos_ - key_start));
      while (true) {
        skip_space();
        if (pos_ >= text_.size()) break;
        if (text_[pos_] == close) {
          ++pos_;
          break;
        }
        if (text_[pos_] == ',') {
          ++pos_;
          continue;
        }
        const std::string field = util::to_lower(identifier());
        if (field.empty()) {
          ++pos_;  // stray character
          continue;
        }
        skip_space();
        if (pos_ >= text_.size() || text_[pos_] != '=') continue;
        ++pos_;
        CVT_TRY_ASSIGN(std::string v, value(close));
        entry.fields.emplace_back(field, std::move(v));
      }
      entries.push_back(std::move(entry));
      if (entries.size() > std::min<std::uint64_t>(limits_.max_archive_entries, max_records)) {
        return fail(Status::Limit, "too many bibliography entries");
      }
    }
    return entries;
  }

 private:
  void skip_space() {
    while (pos_ < text_.size() && (util::is_ascii_space(text_[pos_]))) ++pos_;
  }
  std::string identifier() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() && (util::is_ascii_alpha(text_[pos_]) || util::is_ascii_digit(text_[pos_]) ||
                                   text_[pos_] == '_' || text_[pos_] == '-' || text_[pos_] == ':' || text_[pos_] == '.' ||
                                   text_[pos_] == '+' || text_[pos_] == '/')) {
      ++pos_;
    }
    return std::string(text_.substr(start, pos_ - start));
  }
  void skip_group(char close) {
    int depth = 1;
    while (pos_ < text_.size() && depth > 0) {
      if (text_[pos_] == '{' || (close == ')' && text_[pos_] == '(')) ++depth;
      else if (text_[pos_] == '}' || (close == ')' && text_[pos_] == ')')) --depth;
      ++pos_;
    }
  }
  void skip_to(char close) {
    while (pos_ < text_.size() && text_[pos_] != close) ++pos_;
    if (pos_ < text_.size()) ++pos_;
  }
  /// A field value: parts joined by '#'.
  Result<std::string> value(char close) {
    std::string out;
    while (true) {
      skip_space();
      if (pos_ >= text_.size()) break;
      const char c = text_[pos_];
      if (c == '{') {
        const std::size_t start = ++pos_;
        int depth = 1;
        while (pos_ < text_.size() && depth > 0) {
          if (text_[pos_] == '\\') {
            pos_ += 2;
            continue;
          }
          if (text_[pos_] == '{') ++depth;
          else if (text_[pos_] == '}') --depth;
          ++pos_;
        }
        if (depth != 0) return fail(Status::Decode, "unbalanced braces in a BibTeX value");
        out.append(text_.substr(start, pos_ - start - 1));
      } else if (c == '"') {
        const std::size_t start = ++pos_;
        int depth = 0;
        while (pos_ < text_.size() && !(text_[pos_] == '"' && depth == 0)) {
          if (text_[pos_] == '\\') {
            pos_ += 2;
            continue;
          }
          if (text_[pos_] == '{') ++depth;
          else if (text_[pos_] == '}') --depth;
          ++pos_;
        }
        if (pos_ >= text_.size()) return fail(Status::Decode, "unterminated quoted BibTeX value");
        out.append(text_.substr(start, pos_ - start));
        ++pos_;
      } else if (util::is_ascii_digit(c)) {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && util::is_ascii_digit(text_[pos_])) ++pos_;
        out.append(text_.substr(start, pos_ - start));
      } else if (util::is_ascii_alpha(c)) {
        const std::string name = util::to_lower(identifier());
        const auto it = macros_.find(name);
        out.append(it == macros_.end() ? name : it->second);
        if (out.size() > 16 * 1024 * 1024) return fail(Status::Limit, "a BibTeX value expands beyond 16 MiB");
      } else {
        break;
      }
      skip_space();
      if (pos_ < text_.size() && text_[pos_] == '#') {
        ++pos_;
        continue;
      }
      break;
    }
    (void)close;
    return out;
  }

  std::string_view text_;
  const Limits& limits_;
  std::size_t pos_ = 0;
  std::map<std::string, std::string> macros_;
};

Json bib_item(const BibParser::Entry& entry) {
  Json item = Json::object({{"id", entry.key}, {"type", csl_from_bibtex(entry.type)}});
  if (entry.type == "mastersthesis") item["genre"] = "Master's thesis";
  if (entry.type == "phdthesis") item["genre"] = "PhD thesis";
  std::string year;
  std::string month;
  std::string day;
  Json custom = Json::object();
  for (const auto& [field, raw] : entry.fields) {
    if (field == "author" || field == "editor" || field == "translator") {
      std::vector<std::string> names = split_names(raw);
      for (const std::string& name : names) {
        if (util::iequals(name, "others")) continue;
        add_name(item, field, bibtex_name(name));
      }
      continue;
    }
    const std::string v = latex_to_unicode(raw);
    if (field == "title") set(item, "title", v);
    else if (field == "journal" || field == "journaltitle" || field == "booktitle") set(item, "container-title", v);
    else if (field == "year") year = v;
    else if (field == "month") month = v;
    else if (field == "day") day = v;
    else if (field == "date") set_issued(item, v);
    else if (field == "volume") set(item, "volume", v);
    else if (field == "number" || field == "issue") set(item, item["type"].as_string() == "article-journal" ? "issue" : "number", v);
    else if (field == "pages") set(item, "page", util::replace_all(util::replace_all(v, "–", "-"), "—", "-"));
    else if (field == "publisher" || field == "institution" || field == "school" || field == "organization") set(item, "publisher", v);
    else if (field == "address" || field == "location") set(item, "publisher-place", v);
    else if (field == "doi") set(item, "DOI", v);
    else if (field == "url") set(item, "URL", std::string(util::trim(raw)));
    else if (field == "isbn") set(item, "ISBN", v);
    else if (field == "issn") set(item, "ISSN", v);
    else if (field == "abstract") set(item, "abstract", v);
    else if (field == "keywords") add_keywords(item, v, ',');
    else if (field == "note" || field == "annote") set(item, "note", v);
    else if (field == "language" || field == "langid") set(item, "language", v);
    else if (field == "edition") set(item, "edition", v);
    else if (field == "series") set(item, "collection-title", v);
    else if (field == "urldate") {
      const std::vector<int> parts = parse_date_parts(v);
      if (!parts.empty()) {
        Json date = Json::array();
        for (const int part : parts) date.push_back(part);
        item["accessed"] = Json::object({{"date-parts", Json::array({std::move(date)})}});
      }
    } else if (field == "type") set(item, "genre", v);
    else custom[field] = v;
  }
  if (!year.empty() && !item.contains("issued")) set_issued(item, year + " " + month + " " + day);
  if (!custom.empty()) item["custom"] = std::move(custom);
  return item;
}

std::string bib_value(const std::string& text, bool ascii) { return "{" + unicode_to_latex(text, ascii) + "}"; }

std::string bib_names(const Json& names, bool ascii) {
  std::vector<std::string> out;
  for (const Json& name : names.elements()) {
    if (!name["literal"].as_string().empty()) {
      out.push_back("{" + unicode_to_latex(name["literal"].as_string(), ascii) + "}");
      continue;
    }
    std::string family = unicode_to_latex(name["family"].as_string(), ascii);
    if (family.find(' ') != std::string::npos) family = "{" + family + "}";
    if (!name["non-dropping-particle"].as_string().empty()) {
      family = unicode_to_latex(name["non-dropping-particle"].as_string(), ascii) + " " + family;
    }
    std::string text = family;
    if (!name["suffix"].as_string().empty()) text += ", " + unicode_to_latex(name["suffix"].as_string(), ascii);
    if (!name["given"].as_string().empty()) text += ", " + unicode_to_latex(name["given"].as_string(), ascii);
    out.push_back(std::move(text));
  }
  return util::join(out, " and ");
}

std::string citation_key(const Json& item, std::set<std::string>& used) {
  std::string key = str(item, "id");
  std::string clean;
  for (const char c : key) {
    if (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '-' || c == ':' || c == '.' || c == '/') clean.push_back(c);
  }
  if (clean.empty()) {
    std::string family = item["author"][0]["family"].as_string();
    for (const char c : family) {
      if (util::is_ascii_alpha(c)) clean.push_back(util::ascii_lower(c));
    }
    clean += year_of(item);
    if (clean.empty()) clean = "ref";
  }
  std::string unique = clean;
  for (int n = 2; used.contains(unique); ++n) unique = clean + std::string(1, static_cast<char>('a' + (n - 2) % 26)) + (n > 27 ? std::to_string(n) : "");
  used.insert(unique);
  return unique;
}

std::string write_bibtex(const Json& items, bool biblatex, bool ascii) {
  std::string out;
  std::set<std::string> used;
  for (const Json& item : items.elements()) {
    const std::string type = item["type"].as_string();
    std::string entry_type(type_of(type).bibtex);
    if (type == "thesis" && item["genre"].as_string().find("aster") != std::string::npos) entry_type = "mastersthesis";
    if (biblatex) {
      if (entry_type == "phdthesis" || entry_type == "mastersthesis") entry_type = "thesis";
      if (entry_type == "techreport") entry_type = "report";
    } else if (entry_type == "online" || entry_type == "dataset" || entry_type == "software" || entry_type == "inreference" ||
               entry_type == "movie") {
      entry_type = "misc";
    }
    out += "@" + entry_type + "{" + citation_key(item, used) + ",\n";
    std::vector<std::pair<std::string, std::string>> fields;
    const auto add = [&](std::string_view name, const std::string& value) {
      if (!value.empty()) fields.emplace_back(name, bib_value(value, ascii));
    };
    if (!item["author"].empty()) fields.emplace_back("author", "{" + bib_names(item["author"], ascii) + "}");
    if (!item["editor"].empty()) fields.emplace_back("editor", "{" + bib_names(item["editor"], ascii) + "}");
    add("title", str(item, "title"));
    const std::string container = str(item, "container-title");
    if (entry_type == "article") add(biblatex ? "journaltitle" : "journal", container);
    else add("booktitle", container);
    const std::vector<int> date = issued(item);
    if (biblatex) {
      add("date", iso_date(item));
    } else if (!date.empty()) {
      fields.emplace_back("year", "{" + std::to_string(date[0]) + "}");
      static constexpr std::array<std::string_view, 12> months = {"jan", "feb", "mar", "apr", "may", "jun",
                                                                  "jul", "aug", "sep", "oct", "nov", "dec"};
      if (date.size() > 1 && date[1] >= 1 && date[1] <= 12) fields.emplace_back("month", std::string(months[static_cast<std::size_t>(date[1] - 1)]));
    }
    add("volume", str(item, "volume"));
    add("number", str(item, "issue").empty() ? str(item, "number") : str(item, "issue"));
    add("pages", util::replace_all(str(item, "page"), "-", "--"));
    if (type == "thesis") add("school", str(item, "publisher"));
    else if (type == "report") add("institution", str(item, "publisher"));
    else add("publisher", str(item, "publisher"));
    add(biblatex ? "location" : "address", str(item, "publisher-place"));
    add("edition", str(item, "edition"));
    add("series", str(item, "collection-title"));
    add("doi", str(item, "DOI"));
    if (!str(item, "URL").empty()) fields.emplace_back("url", "{" + str(item, "URL") + "}");
    if (biblatex && item.contains("accessed")) add("urldate", iso_date(item, "accessed"));
    add("isbn", str(item, "ISBN"));
    add("issn", str(item, "ISSN"));
    add("abstract", str(item, "abstract"));
    add("keywords", util::join(keywords_of(item), ", "));
    add(biblatex ? "langid" : "language", str(item, "language"));
    add("note", str(item, "note"));
    for (const auto& [key, value] : item["custom"].items()) {
      if (util::xml_is_name(key)) add(key, value.as_string());
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
      out += "  " + fields[i].first + " = " + fields[i].second + (i + 1 < fields.size() ? ",\n" : "\n");
    }
    out += "}\n\n";
  }
  return out;
}

// ============================================================================ tagged formats (RIS, ENW, RefWorks, MEDLINE, ISI)

struct TaggedLine {
  std::string tag;
  std::string value;
};

/// Records of a tagged format. `tag_of` returns the tag and value of a line
/// or nullopt for continuation lines; `end` recognises end-of-record tags.
std::vector<std::vector<TaggedLine>> tagged_records(std::string_view text,
                                                   const std::function<std::optional<TaggedLine>(std::string_view)>& tag_of,
                                                   const std::function<bool(const TaggedLine&)>& is_end, bool blank_separates) {
  std::vector<std::vector<TaggedLine>> records;
  std::vector<TaggedLine> current;
  const auto flush = [&] {
    if (!current.empty()) records.push_back(std::move(current));
    current.clear();
  };
  for (const std::string_view raw : util::split_lines(text)) {
    if (util::trim(raw).empty()) {
      if (blank_separates) flush();
      continue;
    }
    if (const auto line = tag_of(raw)) {
      if (is_end(*line)) {
        flush();
        continue;
      }
      current.push_back(*line);
    } else if (!current.empty()) {
      current.back().value += (current.back().value.empty() ? "" : " ") + std::string(util::trim(raw));
    }
    if (records.size() > max_records) break;
  }
  flush();
  return records;
}

// ---------------------------------------------------------------------------- RIS

std::optional<TaggedLine> ris_tag(std::string_view line) {
  // "TY  - JOUR"; tolerate one or two spaces and a missing value.
  if (line.size() < 5 || !util::is_ascii_alpha(line[0]) || !(util::is_ascii_alpha(line[1]) || util::is_ascii_digit(line[1]))) {
    return std::nullopt;
  }
  const std::size_t dash = line.find('-', 2);
  if (dash == std::string_view::npos || dash > 5 || !util::trim(line.substr(2, dash - 2)).empty()) return std::nullopt;
  return TaggedLine{std::string(line.substr(0, 2)), std::string(util::trim(line.substr(dash + 1)))};
}

Json ris_item(const std::vector<TaggedLine>& lines) {
  Json item = Json::object();
  std::string start_page;
  std::string end_page;
  for (const TaggedLine& line : lines) {
    const std::string& t = line.tag;
    const std::string& v = line.value;
    if (t == "TY") item["type"] = csl_from_ris(v);
    else if (t == "ID") set(item, "id", v);
    else if (t == "AU" || t == "A1") add_name(item, "author", parse_plain_name(v));
    else if (t == "ED" || t == "A2" || t == "A3") add_name(item, "editor", parse_plain_name(v));
    else if (t == "TI" || t == "T1") set(item, "title", v);
    else if (t == "T2" || t == "JO" || t == "JF" || t == "BT" || t == "JA") {
      if (!item.contains("container-title")) set(item, "container-title", v);
    } else if (t == "T3") set(item, "collection-title", v);
    else if (t == "PY" || t == "Y1" || t == "DA") {
      if (!item.contains("issued") || t == "DA") set_issued(item, v);
    } else if (t == "VL") set(item, "volume", v);
    else if (t == "IS") set(item, "issue", v);
    else if (t == "SP") start_page = v;
    else if (t == "EP") end_page = v;
    else if (t == "DO") set(item, "DOI", v);
    else if (t == "UR" || t == "L2") {
      if (!item.contains("URL")) set(item, "URL", v);
    } else if (t == "PB") set(item, "publisher", v);
    else if (t == "CY" || t == "PP") set(item, "publisher-place", v);
    else if (t == "SN") set(item, v.find('-') != std::string::npos && v.size() <= 9 ? "ISSN" : "ISBN", v);
    else if (t == "AB" || t == "N2") set(item, "abstract", v);
    else if (t == "KW") add_keywords(item, v, ';');
    else if (t == "N1") set(item, "note", v);
    else if (t == "LA") set(item, "language", v);
    else if (t == "ET") set(item, "edition", v);
    else if (t != "ER") item["custom"]["RIS:" + t] = v;
  }
  set(item, "page", page_range(start_page, end_page));
  if (!item.contains("type")) item["type"] = "document";
  return item;
}

std::string write_ris(const Json& items) {
  std::string out;
  const auto line = [&](std::string_view tag, const std::string& value) {
    if (!value.empty()) out += std::string(tag) + "  - " + util::replace_all(util::replace_all(value, "\r", " "), "\n", " ") + "\r\n";
  };
  for (const Json& item : items.elements()) {
    line("TY", std::string(type_of(item["type"].as_string()).ris));
    line("ID", str(item, "id"));
    for (const Json& name : item["author"].elements()) line("AU", name_text(name, true));
    for (const Json& name : item["editor"].elements()) line("ED", name_text(name, true));
    line("TI", str(item, "title"));
    line(item["type"].as_string() == "article-journal" ? "JO" : "T2", str(item, "container-title"));
    line("T3", str(item, "collection-title"));
    const std::vector<int> date = issued(item);
    if (!date.empty()) {
      line("PY", std::to_string(date[0]));
      if (date.size() > 1) line("DA", std::format("{:04}/{:02}/{}", date[0], date[1], date.size() > 2 ? std::format("{:02}", date[2]) : ""));
    }
    line("VL", str(item, "volume"));
    line("IS", str(item, "issue").empty() ? str(item, "number") : str(item, "issue"));
    const auto [first, last] = split_pages(str(item, "page"));
    line("SP", first);
    line("EP", last);
    line("DO", str(item, "DOI"));
    line("UR", str(item, "URL"));
    line("PB", str(item, "publisher"));
    line("CY", str(item, "publisher-place"));
    line("SN", str(item, "ISSN").empty() ? str(item, "ISBN") : str(item, "ISSN"));
    line("ET", str(item, "edition"));
    line("AB", str(item, "abstract"));
    for (const std::string& keyword : keywords_of(item)) line("KW", keyword);
    line("N1", str(item, "note"));
    line("LA", str(item, "language"));
    for (const auto& [key, value] : item["custom"].items()) {
      if (key.starts_with("RIS:") && key.size() == 6) line(std::string_view(key).substr(4), value.as_string());
    }
    out += "ER  - \r\n\r\n";
  }
  return out;
}

// ---------------------------------------------------------------------------- EndNote tagged (.enw) and RefWorks

std::optional<TaggedLine> enw_tag(std::string_view line) {
  if (line.size() < 2 || line[0] != '%') return std::nullopt;
  return TaggedLine{std::string(line.substr(0, 2)), std::string(util::trim(line.substr(2)))};
}

Json enw_item(const std::vector<TaggedLine>& lines) {
  Json item = Json::object();
  for (const TaggedLine& line : lines) {
    const std::string& t = line.tag;
    const std::string& v = line.value;
    if (t == "%0") item["type"] = csl_from_endnote(v);
    else if (t == "%A") add_name(item, "author", parse_plain_name(v));
    else if (t == "%E") add_name(item, "editor", parse_plain_name(v));
    else if (t == "%T") set(item, "title", v);
    else if (t == "%J" || t == "%B") {
      if (!item.contains("container-title")) set(item, "container-title", v);
    } else if (t == "%S") set(item, "collection-title", v);
    else if (t == "%D") set_issued(item, v);
    else if (t == "%8") {
      const std::vector<int> parts = parse_date_parts(v);
      if (parts.size() > 1) set_issued(item, v);
    } else if (t == "%V") set(item, "volume", v);
    else if (t == "%N") set(item, "issue", v);
    else if (t == "%P") set(item, "page", v);
    else if (t == "%R") set(item, "DOI", v);
    else if (t == "%U") set(item, "URL", v);
    else if (t == "%I") set(item, "publisher", v);
    else if (t == "%C") set(item, "publisher-place", v);
    else if (t == "%@") set(item, v.find('-') != std::string::npos && v.size() <= 9 ? "ISSN" : "ISBN", v);
    else if (t == "%X") set(item, "abstract", v);
    else if (t == "%K") add_keywords(item, util::replace_all(v, " ; ", ";"), ';');
    else if (t == "%Z") set(item, "note", v);
    else if (t == "%G") set(item, "language", v);
    else if (t == "%7") set(item, "edition", v);
    else if (t == "%F") set(item, "id", v);
    else item["custom"]["ENW:" + t.substr(1)] = v;
  }
  if (!item.contains("type")) item["type"] = "document";
  return item;
}

std::string write_enw(const Json& items) {
  std::string out;
  const auto line = [&](std::string_view tag, const std::string& value) {
    if (!value.empty()) out += std::string(tag) + " " + util::replace_all(util::replace_all(value, "\r", " "), "\n", " ") + "\n";
  };
  for (const Json& item : items.elements()) {
    line("%0", std::string(type_of(item["type"].as_string()).endnote));
    for (const Json& name : item["author"].elements()) line("%A", name_text(name));
    for (const Json& name : item["editor"].elements()) line("%E", name_text(name));
    line("%T", str(item, "title"));
    line(item["type"].as_string() == "article-journal" ? "%J" : "%B", str(item, "container-title"));
    line("%S", str(item, "collection-title"));
    const std::vector<int> date = issued(item);
    if (!date.empty()) line("%D", std::to_string(date[0]));
    if (date.size() > 1) line("%8", iso_date(item));
    line("%V", str(item, "volume"));
    line("%N", str(item, "issue").empty() ? str(item, "number") : str(item, "issue"));
    line("%P", str(item, "page"));
    line("%R", str(item, "DOI"));
    line("%U", str(item, "URL"));
    line("%I", str(item, "publisher"));
    line("%C", str(item, "publisher-place"));
    line("%@", str(item, "ISSN").empty() ? str(item, "ISBN") : str(item, "ISSN"));
    line("%7", str(item, "edition"));
    line("%X", str(item, "abstract"));
    for (const std::string& keyword : keywords_of(item)) line("%K", keyword);
    line("%Z", str(item, "note"));
    line("%G", str(item, "language"));
    line("%F", str(item, "id"));
    out += "\n";
  }
  return out;
}

std::optional<TaggedLine> refworks_tag(std::string_view line) {
  if (line.size() < 3 || !util::is_ascii_alpha(line[0]) || !(util::is_ascii_alpha(line[1]) || util::is_ascii_digit(line[1])) ||
      line[2] != ' ') {
    return std::nullopt;
  }
  return TaggedLine{std::string(line.substr(0, 2)), std::string(util::trim(line.substr(3)))};
}

Json refworks_item(const std::vector<TaggedLine>& lines) {
  Json item = Json::object();
  std::string start_page;
  std::string end_page;
  for (const TaggedLine& line : lines) {
    const std::string& t = line.tag;
    const std::string& v = line.value;
    if (t == "RT") item["type"] = csl_from_endnote(v);
    else if (t == "A1") add_name(item, "author", parse_plain_name(v));
    else if (t == "A2" || t == "ED") add_name(item, "editor", parse_plain_name(v));
    else if (t == "T1") set(item, "title", v);
    else if (t == "JF" || t == "T2" || t == "JO") {
      if (!item.contains("container-title")) set(item, "container-title", v);
    } else if (t == "T3") set(item, "collection-title", v);
    else if (t == "YR" || t == "FD") {
      if (!item.contains("issued") || t == "FD") set_issued(item, t == "FD" && item.contains("issued") ? year_of(item) + " " + v : v);
    } else if (t == "VO") set(item, "volume", v);
    else if (t == "IS") set(item, "issue", v);
    else if (t == "SP") start_page = v;
    else if (t == "OP") end_page = v;
    else if (t == "DO") set(item, "DOI", v);
    else if (t == "UL" || t == "LK") {
      if (!item.contains("URL")) set(item, "URL", v);
    } else if (t == "PB") set(item, "publisher", v);
    else if (t == "PP") set(item, "publisher-place", v);
    else if (t == "SN") set(item, v.find('-') != std::string::npos && v.size() <= 9 ? "ISSN" : "ISBN", v);
    else if (t == "AB") set(item, "abstract", v);
    else if (t == "K1") add_keywords(item, v, ';');
    else if (t == "NO") set(item, "note", v);
    else if (t == "LA") set(item, "language", v);
    else if (t == "ED") set(item, "edition", v);
    else if (t == "ID") set(item, "id", v);
    else item["custom"]["RW:" + t] = v;
  }
  set(item, "page", page_range(start_page, end_page));
  if (!item.contains("type")) item["type"] = "document";
  return item;
}

std::string write_refworks(const Json& items) {
  std::string out;
  const auto line = [&](std::string_view tag, const std::string& value) {
    if (!value.empty()) out += std::string(tag) + " " + util::replace_all(util::replace_all(value, "\r", " "), "\n", " ") + "\n";
  };
  for (const Json& item : items.elements()) {
    line("RT", std::string(type_of(item["type"].as_string()).endnote));
    line("ID", str(item, "id"));
    for (const Json& name : item["author"].elements()) line("A1", name_text(name));
    for (const Json& name : item["editor"].elements()) line("A2", name_text(name));
    line("T1", str(item, "title"));
    line("JF", str(item, "container-title"));
    line("T3", str(item, "collection-title"));
    const std::vector<int> date = issued(item);
    if (!date.empty()) line("YR", std::to_string(date[0]));
    if (date.size() > 1) {
      static constexpr std::array<std::string_view, 12> months = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      line("FD", std::string(months[static_cast<std::size_t>(std::clamp(date[1], 1, 12) - 1)]) +
                     (date.size() > 2 ? " " + std::to_string(date[2]) : std::string()));
    }
    line("VO", str(item, "volume"));
    line("IS", str(item, "issue").empty() ? str(item, "number") : str(item, "issue"));
    const auto [first, last] = split_pages(str(item, "page"));
    line("SP", first);
    line("OP", last);
    line("DO", str(item, "DOI"));
    line("UL", str(item, "URL"));
    line("PB", str(item, "publisher"));
    line("PP", str(item, "publisher-place"));
    line("SN", str(item, "ISSN").empty() ? str(item, "ISBN") : str(item, "ISSN"));
    line("AB", str(item, "abstract"));
    line("K1", util::join(keywords_of(item), "; "));
    line("NO", str(item, "note"));
    line("LA", str(item, "language"));
    out += "\n";
  }
  return out;
}

// ---------------------------------------------------------------------------- MEDLINE / NBIB

std::optional<TaggedLine> medline_tag(std::string_view line) {
  // "PMID- 12345", "TI  - Title", continuation lines start with six spaces.
  if (line.size() < 5 || line[0] == ' ' || line[4] != '-') return std::nullopt;
  return TaggedLine{std::string(util::trim(line.substr(0, 4))), std::string(util::trim(line.substr(5)))};
}

std::string medline_language(std::string_view code) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> languages = {{
      {"eng", "en"}, {"fre", "fr"}, {"ger", "de"}, {"spa", "es"}, {"ita", "it"},
      {"por", "pt"}, {"jpn", "ja"}, {"chi", "zh"}, {"rus", "ru"}, {"dut", "nl"},
  }};
  for (const auto& [three, two] : languages) {
    if (util::iequals(three, code)) return std::string(two);
  }
  return std::string(code);
}
std::string medline_language_code(std::string_view tag) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> languages = {{
      {"eng", "en"}, {"fre", "fr"}, {"ger", "de"}, {"spa", "es"}, {"ita", "it"},
      {"por", "pt"}, {"jpn", "ja"}, {"chi", "zh"}, {"rus", "ru"}, {"dut", "nl"},
  }};
  for (const auto& [three, two] : languages) {
    if (util::iequals(two, tag)) return std::string(three);
  }
  return std::string(tag);
}

Json medline_item(const std::vector<TaggedLine>& lines) {
  Json item = Json::object({{"type", "article-journal"}});
  bool full_authors = false;
  for (const TaggedLine& line : lines) full_authors = full_authors || line.tag == "FAU";
  for (const TaggedLine& line : lines) {
    const std::string& t = line.tag;
    const std::string& v = line.value;
    if (t == "PMID") {
      set(item, "id", "pmid:" + v);
      set(item, "PMID", v);
    } else if (t == "TI") set(item, "title", v);
    else if (t == "FAU") add_name(item, "author", parse_plain_name(v));
    else if (t == "AU" && !full_authors) {
      // "Example AB": family, then initials.
      const std::size_t space = v.rfind(' ');
      Json name = Json::object({{"family", space == std::string::npos ? v : v.substr(0, space)}});
      if (space != std::string::npos) name["given"] = v.substr(space + 1);
      add_name(item, "author", std::move(name));
    } else if (t == "JT") set(item, "container-title", v);
    else if (t == "TA") set(item, "container-title-short", v);
    else if (t == "DP") set_issued(item, v);
    else if (t == "VI") set(item, "volume", v);
    else if (t == "IP") set(item, "issue", v);
    else if (t == "PG") set(item, "page", v);
    else if (t == "AB") set(item, "abstract", v);
    else if ((t == "LID" || t == "AID") && v.ends_with("[doi]") && !item.contains("DOI")) {
      set(item, "DOI", std::string(util::trim(std::string_view(v).substr(0, v.size() - 5))));
    } else if (t == "LA") set(item, "language", medline_language(v));
    else if (t == "MH" || t == "OT") add_keywords(item, util::replace_all(v, ",", " "), ';');
    else if (t == "IS" && !item.contains("ISSN")) set(item, "ISSN", std::string(util::trim(v.substr(0, v.find(' ')))));
    else if (t == "PL") set(item, "publisher-place", v);
    else if (t == "PT" && util::iequals(v, "Book")) item["type"] = "book";
    else if (t == "PMC") set(item, "PMCID", v);
  }
  return item;
}

std::string medline_wrap(std::string_view tag, const std::string& value) {
  // Lines of at most 82 characters, continuation lines indented by six spaces.
  std::string out;
  std::string line = std::format("{:<4}- ", tag);
  std::size_t width = line.size();
  const std::string split_text = util::replace_all(value, "\n", " ");  // split() returns views into it
  for (const std::string_view word : util::split(split_text, ' ', false)) {
    if (width > 6 && width + 1 + word.size() > 82) {
      out += line + "\n";
      line = "      ";
      width = 6;
    } else if (width > 6) {
      line.push_back(' ');
      ++width;
    }
    line.append(word);
    width += word.size();
  }
  return out + line + "\n";
}

std::string write_medline(const Json& items) {
  std::string out;
  for (const Json& item : items.elements()) {
    const auto line = [&](std::string_view tag, const std::string& value) {
      if (!value.empty()) out += medline_wrap(tag, value);
    };
    line("PMID", str(item, "PMID"));
    line("TI", str(item, "title"));
    line("AB", str(item, "abstract"));
    for (const Json& name : item["author"].elements()) {
      line("FAU", name_text(name));
      std::string initials;
      for (const std::string_view given : util::split(name["given"].as_string(), ' ', false)) {
        if (!given.empty()) initials.push_back(given[0]);
      }
      line("AU", name["literal"].as_string().empty() ? name["family"].as_string() + (initials.empty() ? "" : " " + initials)
                                                     : name["literal"].as_string());
    }
    line("LA", medline_language_code(str(item, "language")));
    line("PT", item["type"].as_string() == "book" ? std::string("Book") : std::string("Journal Article"));
    const std::vector<int> date = issued(item);
    if (!date.empty()) {
      static constexpr std::array<std::string_view, 12> months = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      std::string dp = std::to_string(date[0]);
      if (date.size() > 1) dp += " " + std::string(months[static_cast<std::size_t>(std::clamp(date[1], 1, 12) - 1)]);
      if (date.size() > 2) dp += " " + std::to_string(date[2]);
      line("DP", dp);
    }
    line("TA", str(item, "container-title-short").empty() ? str(item, "container-title") : str(item, "container-title-short"));
    line("JT", str(item, "container-title"));
    line("VI", str(item, "volume"));
    line("IP", str(item, "issue"));
    line("PG", str(item, "page"));
    if (!str(item, "DOI").empty()) line("LID", str(item, "DOI") + " [doi]");
    line("IS", str(item, "ISSN"));
    line("PL", str(item, "publisher-place"));
    for (const std::string& keyword : keywords_of(item)) line("OT", keyword);
    out += "\n";
  }
  return out;
}

// ---------------------------------------------------------------------------- ISI / Web of Science

std::optional<TaggedLine> isi_tag(std::string_view line) {
  if (line.size() < 2 || line[0] == ' ') return std::nullopt;
  if (line.size() > 2 && line[2] != ' ') return std::nullopt;
  return TaggedLine{std::string(line.substr(0, 2)), std::string(util::trim(line.substr(std::min<std::size_t>(3, line.size()))))};
}

Json isi_item(const std::vector<TaggedLine>& lines) {
  Json item = Json::object({{"type", "article-journal"}});
  bool full_authors = false;
  std::string start_page;
  std::string end_page;
  std::string publication_date;
  for (const TaggedLine& line : lines) full_authors = full_authors || line.tag == "AF";
  for (const TaggedLine& line : lines) {
    const std::string& t = line.tag;
    const std::string& v = line.value;
    if (t == "PT") item["type"] = v == "B" ? "book" : v == "S" ? "chapter" : v == "P" ? "patent" : "article-journal";
    else if (t == "DT" && util::istarts_with(v, "Proceedings")) item["type"] = "paper-conference";
    else if ((t == "AF" && full_authors) || (t == "AU" && !full_authors)) add_name(item, "author", parse_plain_name(v));
    else if (t == "TI") set(item, "title", v);
    else if (t == "SO") set(item, "container-title", v);
    else if (t == "J9") set(item, "container-title-short", v);
    else if (t == "PY") set_issued(item, v);
    else if (t == "PD") publication_date = v;
    else if (t == "VL") set(item, "volume", v);
    else if (t == "IS") set(item, "issue", v);
    else if (t == "BP") start_page = v;
    else if (t == "EP") end_page = v;
    else if (t == "DI") set(item, "DOI", v);
    else if (t == "SN") set(item, "ISSN", v);
    else if (t == "BN") set(item, "ISBN", v);
    else if (t == "PU") set(item, "publisher", v);
    else if (t == "PI") set(item, "publisher-place", v);
    else if (t == "AB") set(item, "abstract", v);
    else if (t == "DE") add_keywords(item, v, ';');
    else if (t == "LA") set(item, "language", v);
    else if (t == "UT") set(item, "id", v);
  }
  if (!publication_date.empty() && item.contains("issued")) set_issued(item, year_of(item) + " " + publication_date);
  set(item, "page", page_range(start_page, end_page));
  return item;
}

/// ISI repeats a tag on continuation lines ("   ") for multi-valued fields.
std::vector<std::vector<TaggedLine>> isi_records(std::string_view text) {
  std::vector<std::vector<TaggedLine>> records;
  std::vector<TaggedLine> current;
  std::string last_tag;
  for (const std::string_view raw : util::split_lines(text)) {
    if (raw.starts_with("   ") && !current.empty()) {
      const std::string value(util::trim(raw));
      if (last_tag == "AU" || last_tag == "AF" || last_tag == "CR" || last_tag == "C1") current.push_back({last_tag, value});
      else current.back().value += " " + value;
      continue;
    }
    const auto line = isi_tag(raw);
    if (!line) continue;
    if (line->tag == "FN" || line->tag == "VR" || line->tag == "EF") continue;
    if (line->tag == "ER") {
      if (!current.empty()) records.push_back(std::move(current));
      current.clear();
      continue;
    }
    last_tag = line->tag;
    current.push_back(*line);
    if (records.size() > max_records) break;
  }
  if (!current.empty()) records.push_back(std::move(current));
  return records;
}

std::string write_isi(const Json& items) {
  std::string out = "FN Clarivate Analytics Web of Science\nVR 1.0\n";
  for (const Json& item : items.elements()) {
    const auto line = [&](std::string_view tag, const std::string& value) {
      if (!value.empty()) out += std::string(tag) + " " + util::replace_all(util::replace_all(value, "\r", " "), "\n", " ") + "\n";
    };
    const auto multi = [&](std::string_view tag, const std::vector<std::string>& values) {
      for (std::size_t i = 0; i < values.size(); ++i) out += (i == 0 ? std::string(tag) : std::string("  ")) + " " + values[i] + "\n";
    };
    const std::string type = item["type"].as_string();
    line("PT", type == "book" ? "B" : type == "chapter" ? "S" : type == "patent" ? "P" : "J");
    std::vector<std::string> short_names;
    std::vector<std::string> full_names;
    for (const Json& name : item["author"].elements()) {
      full_names.push_back(name_text(name));
      std::string initials;
      for (const std::string_view given : util::split(name["given"].as_string(), ' ', false)) {
        if (!given.empty()) initials.push_back(given[0]);
      }
      short_names.push_back(name["literal"].as_string().empty() ? name["family"].as_string() + (initials.empty() ? "" : ", " + initials)
                                                                : name["literal"].as_string());
    }
    multi("AU", short_names);
    multi("AF", full_names);
    line("TI", str(item, "title"));
    line("SO", str(item, "container-title"));
    line("LA", str(item, "language"));
    line("DT", type == "paper-conference" ? std::string("Proceedings Paper") : std::string("Article"));
    line("DE", util::join(keywords_of(item), "; "));
    line("AB", str(item, "abstract"));
    line("PU", str(item, "publisher"));
    line("PI", str(item, "publisher-place"));
    line("SN", str(item, "ISSN"));
    line("BN", str(item, "ISBN"));
    line("J9", str(item, "container-title-short"));
    const std::vector<int> date = issued(item);
    if (date.size() > 1) {
      static constexpr std::array<std::string_view, 12> months = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
      line("PD", std::string(months[static_cast<std::size_t>(std::clamp(date[1], 1, 12) - 1)]) +
                     (date.size() > 2 ? " " + std::to_string(date[2]) : std::string()));
    }
    if (!date.empty()) line("PY", std::to_string(date[0]));
    line("VL", str(item, "volume"));
    line("IS", str(item, "issue"));
    const auto [first, last] = split_pages(str(item, "page"));
    line("BP", first);
    line("EP", last);
    line("DI", str(item, "DOI"));
    line("UT", str(item, "id"));
    out += "ER\n\n";
  }
  out += "EF\n";
  return out;
}

// ============================================================================ XML formats

util::XmlNode child_named(util::XmlNode node, std::string_view name) {
  for (const util::XmlNode child : node.elements()) {
    if (util::iequals(child.local_name(), name)) return child;
  }
  return {};
}
std::string node_text(util::XmlNode node) {
  // EndNote wraps text in <style> runs; text() concatenates them.
  std::string text = node.text();
  std::string collapsed;
  for (const char c : text) {
    if (util::is_ascii_space(c)) {
      if (!collapsed.empty() && collapsed.back() != ' ') collapsed.push_back(' ');
    } else {
      collapsed.push_back(c);
    }
  }
  return std::string(util::trim(collapsed));
}

Json endnote_record(util::XmlNode record) {
  Json item = Json::object();
  const util::XmlNode ref_type = child_named(record, "ref-type");
  std::string type_name(ref_type.attribute("name").value());
  if (type_name.empty()) {
    const int number = static_cast<int>(util::parse_int(node_text(ref_type)).value_or(13));
    for (const TypeName& type : types) {
      if (type.endnote_number == number) type_name = type.endnote;
    }
  }
  item["type"] = csl_from_endnote(type_name);
  const util::XmlNode contributors = child_named(record, "contributors");
  for (const util::XmlNode author : child_named(contributors, "authors").elements()) add_name(item, "author", parse_plain_name(node_text(author)));
  for (const util::XmlNode editor : child_named(contributors, "secondary-authors").elements()) {
    add_name(item, "editor", parse_plain_name(node_text(editor)));
  }
  const util::XmlNode titles = child_named(record, "titles");
  set(item, "title", node_text(child_named(titles, "title")));
  set(item, "container-title", node_text(child_named(titles, "secondary-title")));
  if (!item.contains("container-title")) set(item, "container-title", node_text(child_named(child_named(record, "periodical"), "full-title")));
  set(item, "collection-title", node_text(child_named(titles, "tertiary-title")));
  set(item, "page", node_text(child_named(record, "pages")));
  set(item, "volume", node_text(child_named(record, "volume")));
  set(item, "issue", node_text(child_named(record, "number")));
  set(item, "edition", node_text(child_named(record, "edition")));
  const util::XmlNode dates = child_named(record, "dates");
  const std::string year = node_text(child_named(dates, "year"));
  const std::string pub_date = node_text(child_named(child_named(dates, "pub-dates"), "date"));
  if (!year.empty()) set_issued(item, pub_date.empty() ? year : year + " " + pub_date);
  const std::string isbn = node_text(child_named(record, "isbn"));
  set(item, isbn.find('-') != std::string::npos && isbn.size() <= 9 ? "ISSN" : "ISBN", isbn);
  set(item, "DOI", node_text(child_named(record, "electronic-resource-num")));
  set(item, "URL", node_text(child_named(child_named(child_named(record, "urls"), "related-urls"), "url")));
  set(item, "abstract", node_text(child_named(record, "abstract")));
  for (const util::XmlNode keyword : child_named(record, "keywords").elements()) add_keywords(item, node_text(keyword), ';');
  set(item, "publisher", node_text(child_named(record, "publisher")));
  set(item, "publisher-place", node_text(child_named(record, "pub-location")));
  set(item, "note", node_text(child_named(record, "notes")));
  set(item, "language", node_text(child_named(record, "language")));
  set(item, "id", node_text(child_named(record, "rec-number")));
  return item;
}

std::string write_endnote_xml(const Json& items) {
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  util::XmlNode records = doc.append_child("xml").append_child("records");
  std::size_t number = 0;
  for (const Json& item : items.elements()) {
    util::XmlNode record = records.append_child("record");
    const auto styled = [](util::XmlNode parent, std::string_view name, const std::string& value) {
      if (value.empty()) return;
      util::XmlNode style = parent.append_child(name).append_child("style");
      style.set_attribute("face", "normal");
      style.set_attribute("font", "default");
      style.set_attribute("size", "100%");
      style.append_text(value);
    };
    record.append_child("rec-number").append_text(std::to_string(++number));
    const TypeName& type = type_of(item["type"].as_string());
    util::XmlNode ref = record.append_child("ref-type");
    ref.set_attribute("name", type.endnote);
    ref.append_text(std::to_string(type.endnote_number));
    util::XmlNode contributors = record.append_child("contributors");
    if (!item["author"].empty()) {
      util::XmlNode authors = contributors.append_child("authors");
      for (const Json& name : item["author"].elements()) styled(authors, "author", name_text(name));
    }
    if (!item["editor"].empty()) {
      util::XmlNode editors = contributors.append_child("secondary-authors");
      for (const Json& name : item["editor"].elements()) styled(editors, "author", name_text(name));
    }
    util::XmlNode titles = record.append_child("titles");
    styled(titles, "title", str(item, "title"));
    styled(titles, "secondary-title", str(item, "container-title"));
    styled(titles, "tertiary-title", str(item, "collection-title"));
    if (item["type"].as_string() == "article-journal" && !str(item, "container-title").empty()) {
      styled(record.append_child("periodical"), "full-title", str(item, "container-title"));
    }
    styled(record, "pages", str(item, "page"));
    styled(record, "volume", str(item, "volume"));
    styled(record, "number", str(item, "issue"));
    styled(record, "edition", str(item, "edition"));
    if (!str(item, "abstract").empty()) styled(record, "abstract", str(item, "abstract"));
    if (!keywords_of(item).empty()) {
      util::XmlNode keywords = record.append_child("keywords");
      for (const std::string& keyword : keywords_of(item)) styled(keywords, "keyword", keyword);
    }
    const std::vector<int> date = issued(item);
    if (!date.empty()) {
      util::XmlNode dates = record.append_child("dates");
      styled(dates, "year", std::to_string(date[0]));
      if (date.size() > 1) styled(dates.append_child("pub-dates"), "date", iso_date(item));
    }
    styled(record, "pub-location", str(item, "publisher-place"));
    styled(record, "publisher", str(item, "publisher"));
    styled(record, "isbn", str(item, "ISSN").empty() ? str(item, "ISBN") : str(item, "ISSN"));
    styled(record, "electronic-resource-num", str(item, "DOI"));
    if (!str(item, "URL").empty()) styled(record.append_child("urls").append_child("related-urls"), "url", str(item, "URL"));
    styled(record, "notes", str(item, "note"));
    styled(record, "language", str(item, "language"));
  }
  util::XmlWriteOptions options;
  options.indent = "";
  return util::xml_to_string(doc, options);
}

Json mods_record(util::XmlNode mods) {
  Json item = Json::object({{"type", "document"}});
  const util::XmlNode title_info = child_named(mods, "titleInfo");
  std::string title = node_text(child_named(title_info, "title"));
  const std::string subtitle = node_text(child_named(title_info, "subTitle"));
  if (!subtitle.empty()) title += ": " + subtitle;
  set(item, "title", title);
  for (const util::XmlNode name : mods.children_local("name")) {
    Json person = Json::object();
    for (const util::XmlNode part : name.children_local("namePart")) {
      const std::string_view type = part.attribute("type").value();
      if (type == "family") person["family"] = node_text(part);
      else if (type == "given") person["given"] = person.contains("given") ? person["given"].as_string() + " " + node_text(part) : node_text(part);
      else if (type == "termsOfAddress") person["suffix"] = node_text(part);
      else if (type.empty()) person = parse_plain_name(node_text(part));
    }
    if (name.attribute("type").value() == "corporate" && !person.contains("given")) person = Json::object({{"literal", node_text(name.child_local("namePart"))}});
    const std::string role = node_text(name.child_local("role").child_local("roleTerm"));
    add_name(item, role == "editor" || role == "edt" ? "editor" : "author", std::move(person));
  }
  const std::string genre = util::to_lower(node_text(child_named(mods, "genre")));
  const util::XmlNode origin = child_named(mods, "originInfo");
  set(item, "publisher", node_text(child_named(origin, "publisher")));
  set(item, "publisher-place", node_text(child_named(child_named(origin, "place"), "placeTerm")));
  set(item, "edition", node_text(child_named(origin, "edition")));
  std::string date = node_text(child_named(origin, "dateIssued"));
  for (const util::XmlNode related : mods.children_local("relatedItem")) {
    if (related.attribute("type").value() != "host") continue;
    set(item, "container-title", node_text(child_named(child_named(related, "titleInfo"), "title")));
    const std::string host_genre = util::to_lower(node_text(child_named(related, "genre")));
    item["type"] = host_genre.find("book") != std::string::npos ? "chapter"
                   : host_genre.find("conference") != std::string::npos ? "paper-conference"
                                                                         : "article-journal";
    const util::XmlNode part = child_named(related, "part");
    for (const util::XmlNode detail : part.children_local("detail")) {
      const std::string_view type = detail.attribute("type").value();
      if (type == "volume") set(item, "volume", node_text(child_named(detail, "number")));
      if (type == "issue" || type == "number") set(item, "issue", node_text(child_named(detail, "number")));
    }
    const util::XmlNode extent = child_named(part, "extent");
    set(item, "page", page_range(node_text(child_named(extent, "start")), node_text(child_named(extent, "end"))));
    if (date.empty()) date = node_text(child_named(part, "date"));
    if (date.empty()) date = node_text(child_named(child_named(related, "originInfo"), "dateIssued"));
  }
  if (item["type"].as_string() == "document") {
    if (genre.find("book") != std::string::npos) item["type"] = "book";
    else if (genre.find("thesis") != std::string::npos) item["type"] = "thesis";
    else if (genre.find("report") != std::string::npos) item["type"] = "report";
    else if (genre.find("web") != std::string::npos) item["type"] = "webpage";
  }
  set_issued(item, date);
  for (const util::XmlNode identifier : mods.children_local("identifier")) {
    const std::string type = util::to_lower(identifier.attribute("type").value());
    if (type == "doi") set(item, "DOI", node_text(identifier));
    else if (type == "isbn") set(item, "ISBN", node_text(identifier));
    else if (type == "issn") set(item, "ISSN", node_text(identifier));
    else if (type == "uri") set(item, "URL", node_text(identifier));
  }
  if (!item.contains("URL")) set(item, "URL", node_text(child_named(child_named(mods, "location"), "url")));
  set(item, "abstract", node_text(child_named(mods, "abstract")));
  for (const util::XmlNode subject : mods.children_local("subject")) {
    for (const util::XmlNode topic : subject.children_local("topic")) add_keywords(item, node_text(topic), ';');
  }
  set(item, "language", node_text(child_named(child_named(mods, "language"), "languageTerm")));
  set(item, "note", node_text(child_named(mods, "note")));
  const std::string_view id = mods.attribute("ID").value();
  if (!id.empty()) item["id"] = std::string(id);
  return item;
}

std::string write_mods(const Json& items) {
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  util::XmlNode collection = doc.append_child("modsCollection");
  collection.set_attribute("xmlns", "http://www.loc.gov/mods/v3");
  for (const Json& item : items.elements()) {
    util::XmlNode mods = collection.append_child("mods");
    if (!str(item, "id").empty() && util::xml_is_name(str(item, "id"))) mods.set_attribute("ID", str(item, "id"));
    mods.append_child("titleInfo").append_child("title").append_text(str(item, "title"));
    for (const std::string_view role : {"author"sv, "editor"sv}) {
      for (const Json& person : item[role].elements()) {
        util::XmlNode name = mods.append_child("name");
        if (!person["literal"].as_string().empty()) {
          name.set_attribute("type", "corporate");
          name.append_child("namePart").append_text(person["literal"].as_string());
        } else {
          name.set_attribute("type", "personal");
          util::XmlNode family = name.append_child("namePart");
          family.set_attribute("type", "family");
          std::string family_text = person["family"].as_string();
          if (!person["non-dropping-particle"].as_string().empty()) family_text = person["non-dropping-particle"].as_string() + " " + family_text;
          family.append_text(family_text);
          if (!person["given"].as_string().empty()) {
            util::XmlNode given = name.append_child("namePart");
            given.set_attribute("type", "given");
            given.append_text(person["given"].as_string());
          }
          if (!person["suffix"].as_string().empty()) {
            util::XmlNode suffix = name.append_child("namePart");
            suffix.set_attribute("type", "termsOfAddress");
            suffix.append_text(person["suffix"].as_string());
          }
        }
        util::XmlNode term = name.append_child("role").append_child("roleTerm");
        term.set_attribute("authority", "marcrelator");
        term.set_attribute("type", "text");
        term.append_text(std::string(role));
      }
    }
    const std::string type = item["type"].as_string();
    const bool hosted = type == "article-journal" || type == "chapter" || type == "paper-conference" ||
                        type == "article-newspaper" || type == "article-magazine";
    mods.append_child("typeOfResource").append_text("text");
    if (!hosted) mods.append_child("genre").append_text(type == "book" ? "book" : type == "thesis" ? "thesis" : type == "report" ? "report" : type);
    util::XmlNode origin = mods.append_child("originInfo");
    if (!str(item, "publisher").empty()) origin.append_child("publisher").append_text(str(item, "publisher"));
    if (!str(item, "publisher-place").empty()) origin.append_child("place").append_child("placeTerm").append_text(str(item, "publisher-place"));
    if (!str(item, "edition").empty()) origin.append_child("edition").append_text(str(item, "edition"));
    if (!iso_date(item).empty()) origin.append_child("dateIssued").append_text(iso_date(item));
    if (hosted) {
      util::XmlNode host = mods.append_child("relatedItem");
      host.set_attribute("type", "host");
      host.append_child("titleInfo").append_child("title").append_text(str(item, "container-title"));
      host.append_child("genre").append_text(type == "chapter" ? "book" : type == "paper-conference" ? "conference publication" : "periodical");
      util::XmlNode part = host.append_child("part");
      for (const auto& [detail_type, key] : {std::pair{"volume"sv, "volume"sv}, std::pair{"issue"sv, "issue"sv}}) {
        if (str(item, key).empty()) continue;
        util::XmlNode detail = part.append_child("detail");
        detail.set_attribute("type", detail_type);
        detail.append_child("number").append_text(str(item, key));
      }
      const auto [first, last] = split_pages(str(item, "page"));
      if (!first.empty()) {
        util::XmlNode extent = part.append_child("extent");
        extent.set_attribute("unit", "page");
        extent.append_child("start").append_text(first);
        if (!last.empty()) extent.append_child("end").append_text(last);
      }
    }
    for (const auto& [type_name, key] : {std::pair{"doi"sv, "DOI"sv}, std::pair{"isbn"sv, "ISBN"sv}, std::pair{"issn"sv, "ISSN"sv}}) {
      if (str(item, key).empty()) continue;
      util::XmlNode identifier = mods.append_child("identifier");
      identifier.set_attribute("type", type_name);
      identifier.append_text(str(item, key));
    }
    if (!str(item, "URL").empty()) mods.append_child("location").append_child("url").append_text(str(item, "URL"));
    if (!str(item, "abstract").empty()) mods.append_child("abstract").append_text(str(item, "abstract"));
    for (const std::string& keyword : keywords_of(item)) mods.append_child("subject").append_child("topic").append_text(keyword);
    if (!str(item, "language").empty()) mods.append_child("language").append_child("languageTerm").append_text(str(item, "language"));
    if (!str(item, "note").empty()) mods.append_child("note").append_text(str(item, "note"));
  }
  return util::xml_to_string(doc);
}

// ============================================================================ CSL-JSON

Result<Json> read_csl(std::string_view text) {
  CVT_TRY_ASSIGN(Json root, util::parse_json(text, 64));
  if (root.is_object() && root.contains("items")) root = std::move(root["items"]);
  if (root.is_object()) root = Json::array({std::move(root)});
  if (!root.is_array()) return fail(Status::Decode, "CSL-JSON must be an array of items");
  for (const Json& item : root.elements()) {
    if (!item.is_object() || !item.contains("type")) return fail(Status::Decode, "every CSL-JSON item needs a \"type\"");
  }
  return root;
}

// ============================================================================ document and table views

std::string initials_of(const std::string& given) {
  std::vector<std::string> out;
  for (const std::string_view part : util::split(given, ' ', false)) {
    std::vector<std::string> pieces;
    for (const std::string_view piece : util::split(part, '-', false)) {
      const util::Utf8Decoded d = util::decode_utf8(piece, 0);
      std::string initial;
      util::append_utf8(initial, d.code_point);
      pieces.push_back(initial + ".");
    }
    out.push_back(util::join(pieces, "-"));
  }
  return util::join(out, " ");
}

std::string author_list(const Json& names) {
  std::vector<std::string> out;
  for (const Json& name : names.elements()) {
    if (!name["literal"].as_string().empty()) {
      out.push_back(name["literal"].as_string());
      continue;
    }
    std::string family = name["family"].as_string();
    if (!name["non-dropping-particle"].as_string().empty()) family = name["non-dropping-particle"].as_string() + " " + family;
    std::string text = family;
    if (!name["given"].as_string().empty()) text += ", " + initials_of(name["given"].as_string());
    if (!name["suffix"].as_string().empty()) text += ", " + name["suffix"].as_string();
    out.push_back(std::move(text));
  }
  if (out.size() <= 1) return out.empty() ? std::string() : out[0];
  const std::string last = out.back();
  out.pop_back();
  return util::join(out, ", ") + ", & " + last;
}

/// One reference in an author-date style.
ir::Inlines reference_inlines(const Json& item) {
  using namespace ir::build;
  ir::Inlines out;
  std::string authors = author_list(item["author"]);
  if (authors.empty()) authors = author_list(item["editor"]);
  const std::string year = year_of(item);
  std::string head = authors.empty() ? str(item, "title") : authors;
  head += " (" + (year.empty() ? std::string("n.d.") : year) + "). ";
  out.push_back(text(head));
  const std::string type = item["type"].as_string();
  const std::string container = str(item, "container-title");
  const bool standalone = type == "book" || type == "report" || type == "thesis" || container.empty();
  if (!authors.empty()) {
    if (standalone) out.push_back(emphasis(str(item, "title") + "."));
    else out.push_back(text(str(item, "title") + "."));
  }
  if (!container.empty()) {
    out.push_back(text(" "));
    if (type == "chapter" || type == "paper-conference") out.push_back(text("In "));
    out.push_back(emphasis(container));
    std::string tail;
    if (!str(item, "volume").empty()) tail += ", " + str(item, "volume");
    if (!str(item, "issue").empty()) tail += "(" + str(item, "issue") + ")";
    if (!str(item, "page").empty()) tail += ", " + str(item, "page");
    out.push_back(text(tail + "."));
  }
  if (standalone && !str(item, "publisher").empty()) out.push_back(text(" " + str(item, "publisher") + "."));
  if (!str(item, "DOI").empty()) {
    const std::string url = "https://doi.org/" + str(item, "DOI");
    out.push_back(text(" "));
    out.push_back(hyperlink(url, url));
  } else if (!str(item, "URL").empty()) {
    out.push_back(text(" "));
    out.push_back(hyperlink(str(item, "URL"), str(item, "URL")));
  }
  return out;
}

ir::Document reference_document(const Json& items) {
  ir::Document document;
  document.metadata.title = "References";
  document.blocks.push_back(ir::build::heading(1, "References"));
  std::vector<const Json*> sorted;
  for (const Json& item : items.elements()) sorted.push_back(&item);
  const auto sort_key = [](const Json* item) {
    std::string key = (*item)["author"][0]["family"].as_string();
    if (key.empty()) key = (*item)["author"][0]["literal"].as_string();
    if (key.empty()) key = (*item)["title"].as_string();
    return util::to_lower(key) + " " + year_of(*item);
  };
  std::ranges::stable_sort(sorted, [&](const Json* a, const Json* b) { return sort_key(a) < sort_key(b); });
  for (const Json* item : sorted) document.blocks.push_back(ir::Paragraph{reference_inlines(*item)});
  return document;
}

ir::Table reference_table(const Json& items) {
  std::vector<std::vector<std::string>> rows;
  for (const Json& item : items.elements()) {
    std::vector<std::string> authors;
    for (const Json& name : item["author"].elements()) authors.push_back(name_text(name));
    rows.push_back({str(item, "id"), item["type"].as_string(), util::join(authors, "; "), str(item, "title"),
                    str(item, "container-title"), year_of(item), str(item, "volume"),
                    str(item, "issue").empty() ? str(item, "number") : str(item, "issue"), str(item, "page"),
                    str(item, "DOI"), str(item, "URL"), str(item, "publisher")});
  }
  ir::Table table = string_table("References",
                                 {"id", "type", "authors", "title", "container", "year", "volume", "issue", "pages", "doi",
                                  "url", "publisher"},
                                 rows);
  for (std::size_t r = 1; r < table.sheets[0].rows.size(); ++r) {
    ir::Cell& year = table.sheets[0].rows[r][5];
    if (const auto value = util::parse_int(year.text)) year = ir::Cell::numeric(static_cast<double>(*value), year.text);
  }
  return table;
}

// ============================================================================ registration

Result<Json> read_items(std::string_view format, const ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, decode_text(ctx.input, ctx.options.get_string("source_encoding", "auto")));
  Json items = Json::array();
  const auto finish = [&](std::vector<std::vector<TaggedLine>> records, const std::function<Json(const std::vector<TaggedLine>&)>& convert,
                          std::string_view what) -> Result<Json> {
    if (records.empty() && !util::trim(text).empty()) return fail(Status::Decode, "no {} records found", what);
    if (records.size() > ctx.limits.max_archive_entries) return fail(Status::Limit, "too many records");
    for (const auto& record : records) items.push_back(convert(record));
    return items;
  };
  if (format == "bib") {
    BibParser parser(text, ctx.limits);
    CVT_TRY_ASSIGN(const std::vector<BibParser::Entry> entries, parser.parse());
    if (entries.empty() && !util::trim(text).empty() && text.find('@') == std::string::npos) {
      return fail(Status::Decode, "no BibTeX entries found");
    }
    for (const BibParser::Entry& entry : entries) items.push_back(bib_item(entry));
    return items;
  }
  if (format == "ris") {
    return finish(tagged_records(text, ris_tag, [](const TaggedLine& l) { return l.tag == "ER"; }, false), ris_item, "RIS");
  }
  if (format == "enw") {
    return finish(tagged_records(text, enw_tag, [](const TaggedLine&) { return false; }, true), enw_item, "EndNote");
  }
  if (format == "refworks") {
    return finish(tagged_records(text, refworks_tag, [](const TaggedLine&) { return false; }, true), refworks_item, "RefWorks");
  }
  if (format == "nbib") {
    return finish(tagged_records(text, medline_tag, [](const TaggedLine&) { return false; }, true), medline_item, "MEDLINE");
  }
  if (format == "isi") return finish(isi_records(text), isi_item, "Web of Science");
  if (format == "csl-json") return read_csl(text);
  // XML formats
  util::XmlParseOptions options;
  options.replace_invalid_text = true;
  options.keep_unknown_entities = true;
  options.max_depth = 128;
  auto doc = util::parse_xml(ctx.input, options);
  if (!doc) return fail(doc.error().code, "not well-formed XML: {}", doc.error().message);
  const util::XmlNode root = doc->root();
  if (format == "endnote-xml") {
    for (const util::XmlNode record : child_named(root, "records").elements()) {
      if (items.size() >= ctx.limits.max_archive_entries) return fail(Status::Limit, "too many records");
      items.push_back(endnote_record(record));
    }
    if (items.empty() && root.local_name() != "xml") return fail(Status::Decode, "not an EndNote XML export");
    return items;
  }
  if (root.local_name() == "mods") {
    items.push_back(mods_record(root));
    return items;
  }
  if (root.local_name() != "modsCollection") return fail(Status::Decode, "not a MODS document (root <{}>)", root.local_name());
  for (const util::XmlNode mods : root.children_local("mods")) {
    if (items.size() >= ctx.limits.max_archive_entries) return fail(Status::Limit, "too many records");
    items.push_back(mods_record(mods));
  }
  return items;
}

Result<std::string> write_items(std::string_view format, const Json& value, const WriteContext& ctx) {
  Json items = value;
  if (items.is_object() && items.contains("items")) items = items["items"];
  if (items.is_object()) items = Json::array({items});
  if (!items.is_array()) return fail(Status::Encode, "a bibliography is written from an array of CSL-JSON items");
  if (format == "bib") {
    const bool biblatex = ctx.options.get_enum("bib_dialect", "bibtex", {"bibtex", "biblatex"}) == "biblatex";
    const bool escape = ctx.options.was_set("latex_escape") ? ctx.options.get_bool("latex_escape", true) : !biblatex;
    return write_bibtex(items, biblatex, escape);
  }
  if (format == "ris") return write_ris(items);
  if (format == "enw") return write_enw(items);
  if (format == "refworks") return write_refworks(items);
  if (format == "nbib") return write_medline(items);
  if (format == "isi") return write_isi(items);
  if (format == "endnote-xml") return write_endnote_xml(items);
  if (format == "mods") return write_mods(items);
  return util::dump_json(items, 2) + "\n";
}

struct Format {
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> extensions;
  std::string mime;
  int popularity;
  std::function<int(ByteView, std::string_view)> sniff;
};

void register_format(Registry& registry, const Format& format) {
  FormatInfo info;
  info.id = format.id;
  info.name = format.name;
  info.description = format.description;
  info.category = Category::Data;
  info.extensions = format.extensions;
  info.mime = format.mime;
  info.popularity = format.popularity;
  info.sniff = format.sniff;
  registry.add_format(std::move(info));

  const std::string id = format.id;
  Codec data;
  data.format_id = id;
  data.kind = Kind::Data;
  data.read = [id](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(Json items, read_items(id, ctx));
    return ir::Data{std::move(items)};
  };
  data.write = [id](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    CVT_TRY_ASSIGN(std::string text, write_items(id, value.get<ir::Data>().value, ctx));
    return to_bytes(text);
  };
  data.read_options = {source_encoding_option()};
  if (id == "bib") {
    data.write_options = {
        OptionSpec::choice("bib_dialect", "Dialect", "bib", {"bib", "biblatex"},
                           "Classic BibTeX fields (journal, year, address) or BibLaTeX ones (journaltitle, date, location)."),
        OptionSpec::boolean("latex_escape", "Escape accents", true,
                            "Write accented letters as LaTeX commands ({\\'e}) by default on for BibTeX and off for BibLaTeX, which reads UTF-8.")
            .advanced(),
    };
  }
  registry.add_codec(std::move(data));

  Codec table;
  table.format_id = id;
  table.kind = Kind::Table;
  table.lossy = true;
  table.read_cost = 15;
  table.read = [id](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(const Json items, read_items(id, ctx));
    return reference_table(items);
  };
  table.read_options = {source_encoding_option()};
  registry.add_codec(std::move(table));

  Codec document;
  document.format_id = id;
  document.kind = Kind::Document;
  document.lossy = true;
  document.read_cost = 15;
  document.read = [id](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(const Json items, read_items(id, ctx));
    return reference_document(items);
  };
  document.read_options = {source_encoding_option()};
  registry.add_codec(std::move(document));
}

std::string head_of(ByteView data) { return sniff_text(data, 4096); }

/// True when one of the first non-empty lines starts with `prefix`.
bool first_line_starts(const std::string& head, std::string_view prefix) {
  for (const std::string_view raw : util::split_lines(head)) {
    const std::string_view line = util::trim_right(raw);
    if (util::trim(line).empty()) continue;
    return line.starts_with(prefix);
  }
  return false;
}

}  // namespace

void register_bibliographies(Registry& registry) {
  register_format(registry, {"bib", "BibTeX bibliography", "BibTeX / BibLaTeX database of references.", {"bib", "bibtex"},
                             "application/x-bibtex", 45, [](ByteView data, std::string_view) {
                               const std::string head = head_of(data);
                               const std::size_t at = head.find('@');
                               if (at == std::string::npos) return 0;
                               std::size_t end = at + 1;
                               while (end < head.size() && util::is_ascii_alpha(head[end])) ++end;
                               if (end == at + 1 || end >= head.size()) return 0;
                               std::size_t brace = end;
                               while (brace < head.size() && head[brace] == ' ') ++brace;
                               return brace < head.size() && (head[brace] == '{' || head[brace] == '(') ? 85 : 0;
                             }});
  register_format(registry, {"ris", "RIS citations", "Research Information Systems tagged citations.", {"ris"},
                             "application/x-research-info-systems", 40, [](ByteView data, std::string_view) {
                               const std::string head = head_of(data);
                               if (first_line_starts(head, "TY  - ")) return 92;
                               // Some writers number the records ("1.") before the TY line.
                               return head.find("\nTY  - ") != std::string::npos && head.find("\nER  -") != std::string::npos ? 88 : 0;
                             }});
  register_format(registry, {"endnote-xml", "EndNote XML", "EndNote library exported as XML.", {"xml"}, "application/xml", 25,
                             [](ByteView data, std::string_view) {
                               const std::string head = head_of(data);
                               return head.find("<xml>") != std::string::npos && head.find("<records>") != std::string::npos &&
                                              head.find("<record>") != std::string::npos
                                          ? 90
                                          : 0;
                             }});
  register_format(registry, {"enw", "EndNote tagged citation", "EndNote / refer tagged citations (%A, %T, ...).", {"enw"},
                             "application/x-endnote-refer", 25, [](ByteView data, std::string_view) {
                               const std::string head = head_of(data);
                               return first_line_starts(head, "%0 ") ? 90 : 0;
                             }});
  register_format(registry, {"csl-json", "CSL-JSON citations", "Citation Style Language JSON items.", {"json"},
                             "application/vnd.citationstyles.csl+json", 25, [](ByteView data, std::string_view) {
                               const std::string head = head_of(data);
                               const std::string_view trimmed = util::trim(head);
                               if (!trimmed.starts_with('[') && !trimmed.starts_with('{')) return 0;
                               const bool csl_type = head.find("\"article-journal\"") != std::string::npos ||
                                                     head.find("\"paper-conference\"") != std::string::npos ||
                                                     head.find("\"chapter\"") != std::string::npos ||
                                                     head.find("\"book\"") != std::string::npos;
                               return csl_type && head.find("\"type\"") != std::string::npos &&
                                              (head.find("\"issued\"") != std::string::npos ||
                                               head.find("\"container-title\"") != std::string::npos ||
                                               head.find("\"family\"") != std::string::npos)
                                          ? 88
                                          : 0;
                             }});
  register_format(registry, {"nbib", "MEDLINE / PubMed citations", "MEDLINE-format citations as exported by PubMed.",
                             {"nbib", "medline"}, "application/nbib", 25, [](ByteView data, std::string_view) {
                               const std::string head = head_of(data);
                               return first_line_starts(head, "PMID-") ? 93 : 0;
                             }});
  register_format(registry, {"mods", "MODS XML", "Metadata Object Description Schema records.", {"mods", "xml"},
                             "application/mods+xml", 15, [](ByteView data, std::string_view) {
                               const std::string head = head_of(data);
                               return head.find("http://www.loc.gov/mods/") != std::string::npos ? 92 : 0;
                             }});
  register_format(registry, {"refworks", "RefWorks tagged citations", "RefWorks tagged export format.", {"txt"},
                             "text/plain", 10, [](ByteView data, std::string_view) {
                               const std::string head = head_of(data);
                               return first_line_starts(head, "RT ") && head.find("\nA1 ") != std::string::npos ? 88 : 0;
                             }});
  register_format(registry, {"isi", "Web of Science export", "ISI / Clarivate Web of Science tagged records.", {"isi", "ciw"},
                             "application/x-inst-for-scientific-info", 15, [](ByteView data, std::string_view) {
                               const std::string head = head_of(data);
                               return first_line_starts(head, "FN ") && head.find("\nPT ") != std::string::npos ? 93 : 0;
                             }});
}

}  // namespace cvt::pim
