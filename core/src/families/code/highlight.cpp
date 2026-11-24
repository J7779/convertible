// Convertible core - code family: the table-driven lexer behind the syntax
// highlighter (see highlight.hpp). The language table is in languages.cpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/code/highlight.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::code {

namespace {

using TC = TokenClass;

// ---------------------------------------------------------------------------- word lists

struct WordList {
  std::vector<std::string_view> words;  // sorted
  explicit WordList(std::string_view list) {
    std::size_t start = 0;
    while (start < list.size()) {
      while (start < list.size() && util::is_ascii_space(list[start])) ++start;
      std::size_t end = start;
      while (end < list.size() && !util::is_ascii_space(list[end])) ++end;
      if (end > start) words.push_back(list.substr(start, end - start));
      start = end;
    }
    std::ranges::sort(words);
  }
  [[nodiscard]] bool contains(std::string_view word) const { return std::ranges::binary_search(words, word); }
};

/// A lexicon with its word lists split and sorted, built once per tokenize call.
struct Compiled {
  const Lexicon& lex;
  WordList keywords, types, constants, builtins;
  std::vector<std::string_view> line_comments;
  std::vector<std::pair<std::string_view, std::string_view>> block_comments;

  explicit Compiled(const Lexicon& l)
      : lex(l), keywords(l.keywords), types(l.types), constants(l.constants), builtins(l.builtins) {
    const WordList markers(l.line_comments);
    line_comments = markers.words;
    // Longest markers first, so "///" or "--" wins over "/" or "-".
    std::ranges::sort(line_comments, [](std::string_view a, std::string_view b) { return a.size() > b.size(); });
    const auto split = [](std::string_view list) {
      std::vector<std::string_view> parts;
      std::size_t start = 0;
      while (start < list.size()) {
        while (start < list.size() && list[start] == ' ') ++start;
        std::size_t end = list.find(' ', start);
        if (end == std::string_view::npos) end = list.size();
        if (end > start) parts.push_back(list.substr(start, end - start));
        start = end;
      }
      return parts;
    };
    const std::vector<std::string_view> pairs = split(l.block_comments);
    for (std::size_t i = 0; i + 1 < pairs.size(); i += 2) block_comments.emplace_back(pairs[i], pairs[i + 1]);
  }
};

// ---------------------------------------------------------------------------- the lexer

class Lexer {
 public:
  Lexer(std::string_view code, const Compiled& c, std::size_t base, std::vector<Token>& out)
      : s_(code), c_(c), lex_(c.lex), base_(base), out_(out) {}

  void run() {
    if (lex_.diff) {
      run_diff();
      return;
    }
    while (i_ < s_.size()) step();
  }

 private:
  // -- helpers -------------------------------------------------------------------
  [[nodiscard]] char at(std::size_t k) const { return k < s_.size() ? s_[k] : '\0'; }
  [[nodiscard]] bool starts(std::size_t k, std::string_view w) const { return s_.substr(k, w.size()) == w; }
  [[nodiscard]] bool istarts(std::size_t k, std::string_view w) const {
    return util::iequals(s_.substr(k, w.size()), w) && s_.size() - k >= w.size();
  }
  [[nodiscard]] bool ident_start(char ch) const {
    const auto u = static_cast<unsigned char>(ch);
    if (util::is_ascii_alpha(ch) || ch == '_' || u >= 0x80) return true;
    return !lex_.identifier_extra.empty() && lex_.identifier_extra.find(ch) != std::string_view::npos && ch != '-' &&
           ch != '?' && ch != '!';
  }
  [[nodiscard]] bool ident_char(char ch) const {
    const auto u = static_cast<unsigned char>(ch);
    if (util::is_ascii_alpha(ch) || util::is_ascii_digit(ch) || ch == '_' || u >= 0x80) return true;
    if (lex_.lisp && std::string_view("-?!*+<>=/:%&.").find(ch) != std::string_view::npos) return true;
    if (lex_.css && ch == '-') return true;
    return ch != '\0' && lex_.identifier_extra.find(ch) != std::string_view::npos;
  }
  [[nodiscard]] bool at_line_start(std::size_t k) const {
    while (k > 0) {
      const char p = s_[k - 1];
      if (p == '\n' || p == '\r') return true;
      if (p != ' ' && p != '\t') return false;
      --k;
    }
    return true;
  }
  [[nodiscard]] std::size_t line_end(std::size_t k) const {
    const std::size_t nl = s_.find('\n', k);
    return nl == std::string_view::npos ? s_.size() : nl;
  }

  void emit(TC cls, std::size_t from, std::size_t to) {
    if (to <= from) return;
    if (!out_.empty() && out_.back().cls == cls && out_.back().offset + out_.back().length == base_ + from) {
      out_.back().length += to - from;
    } else {
      out_.push_back(Token{base_ + from, to - from, cls});
    }
    if (cls != TC::Plain || !util::is_ascii_space(s_[from])) last_significant_ = cls;
  }

  // -- one token -------------------------------------------------------------------
  void step() {
    const std::size_t start = i_;
    const char ch = s_[i_];

    if (lex_.markup && ch == '<' && markup_tag()) return;
    if (lex_.lua_long_brackets && lua_long(start)) return;
    if (comment()) return;
    if (lex_.hash_preprocessor && ch == '#' && at_line_start(i_)) {
      std::size_t end = line_end(i_);
      while (end > 0 && end < s_.size() && s_[end - 1] == '\\') end = line_end(end + 1);  // continued lines
      i_ = end;
      emit(TC::Preprocessor, start, i_);
      return;
    }
    if (string_literal()) return;
    if (lex_.dollar_variables && ch == '$' && dollar_variable()) return;
    if (lex_.at_annotations && ch == '@' && (ident_start(at(i_ + 1)) || at(i_ + 1) == '"')) {
      if (at(i_ + 1) == '"') {  // Objective-C @"string" / C# @"verbatim"
        ++i_;
        quoted('"', start, lex_.escape, true);
        return;
      }
      ++i_;
      while (i_ < s_.size() && (ident_char(s_[i_]) || s_[i_] == '.')) ++i_;
      emit(TC::Preprocessor, start, i_);
      return;
    }
    if (lex_.css && ch == '#' && util::hex_digit_value(at(i_ + 1)) >= 0 && brace_depth_ > 0) {
      ++i_;
      while (i_ < s_.size() && util::hex_digit_value(s_[i_]) >= 0) ++i_;
      emit(TC::Number, start, i_);
      return;
    }
    if (util::is_ascii_digit(ch) || (ch == '.' && util::is_ascii_digit(at(i_ + 1)) && !prev_ident())) {
      number();
      return;
    }
    if (ident_start(ch) || (lex_.lisp && ident_char(ch) && ch != '.' && ch != ':' && !util::is_ascii_digit(ch))) {
      identifier();
      return;
    }
    if (util::is_ascii_space(ch)) {
      while (i_ < s_.size() && util::is_ascii_space(s_[i_])) ++i_;
      emit(TC::Plain, start, i_);
      return;
    }
    if (std::string_view("+-*/%=<>!&|^~?:;,.()[]{}\\@#$").find(ch) != std::string_view::npos) {
      if (ch == '{') ++brace_depth_;
      if (ch == '}' && brace_depth_ > 0) --brace_depth_;
      lisp_head_ = ch == '(';
      after_definition_keyword_ = false;
      after_type_keyword_ = false;
      ++i_;
      emit(TC::Operator, start, i_);
      return;
    }
    ++i_;
    emit(TC::Plain, start, i_);
  }

  [[nodiscard]] bool prev_ident() const { return i_ > 0 && ident_char(s_[i_ - 1]); }

  bool comment() {
    const std::size_t start = i_;
    for (const auto& [open, close] : c_.block_comments) {
      if (!starts(i_, open)) continue;
      std::size_t depth = 1;
      std::size_t k = i_ + open.size();
      while (k < s_.size() && depth > 0) {
        if (lex_.nested_comments && starts(k, open)) {
          ++depth;
          k += open.size();
        } else if (starts(k, close)) {
          --depth;
          k += close.size();
        } else {
          ++k;
        }
      }
      i_ = std::min(k, s_.size());
      emit(TC::Comment, start, i_);
      return true;
    }
    for (std::string_view marker : c_.line_comments) {
      bool line_start_only = false;
      if (marker.starts_with('^')) {
        marker.remove_prefix(1);
        line_start_only = true;
      }
      if (marker.empty()) continue;
      const bool match = lex_.case_insensitive ? istarts(i_, marker) : starts(i_, marker);
      if (!match || (line_start_only && !at_line_start(i_))) continue;
      // A word marker such as REM must be followed by a non-word character.
      if (util::is_ascii_alpha(marker.back()) && ident_char(at(i_ + marker.size()))) continue;
      // '#' after an identifier character is not a comment in shells (a#b) or CSS.
      if (marker == "#" && lex_.dollar_variables && i_ > 0 && !util::is_ascii_space(s_[i_ - 1]) && s_[i_ - 1] != ';') {
        continue;
      }
      i_ = line_end(i_);
      emit(TC::Comment, start, i_);
      return true;
    }
    return false;
  }

  bool lua_long(std::size_t start) {
    std::size_t k = i_;
    bool is_comment = false;
    if (starts(k, "--[")) {
      is_comment = true;
      k += 2;
    }
    if (at(k) != '[') return false;
    std::size_t level = 0;
    std::size_t m = k + 1;
    while (at(m) == '=') {
      ++level;
      ++m;
    }
    if (at(m) != '[') return false;
    const std::string close = "]" + std::string(level, '=') + "]";
    const std::size_t end = s_.find(close, m + 1);
    i_ = end == std::string_view::npos ? s_.size() : end + close.size();
    emit(is_comment ? TC::Comment : TC::String, start, i_);
    return true;
  }

  // Scans a quoted literal whose opening delimiter ends before i_ (at i_ == quote).
  void quoted(char quote, std::size_t start, char escape, bool multiline) {
    ++i_;  // the opening quote
    while (i_ < s_.size()) {
      const char ch = s_[i_];
      if (escape != '\0' && ch == escape && quote != escape) {
        i_ = std::min(i_ + 2, s_.size());
        continue;
      }
      if (ch == quote) {
        if (escape == '\0' && at(i_ + 1) == quote) {  // doubled quote ('it''s')
          i_ += 2;
          continue;
        }
        ++i_;
        break;
      }
      if (ch == '\n' && !multiline) break;
      ++i_;
    }
    emit(TC::String, start, i_);
  }

  bool string_literal() {
    const std::size_t start = i_;
    const char ch = s_[i_];
    if (lex_.triple_quotes && (starts(i_, "\"\"\"") || starts(i_, "'''"))) {
      const std::string_view delim = s_.substr(i_, 3);
      std::size_t k = i_ + 3;
      while (k < s_.size() && !starts(k, delim)) k += (s_[k] == lex_.escape && lex_.escape != '\0') ? 2 : 1;
      i_ = std::min(k + 3, s_.size());
      emit(TC::String, start, i_);
      return true;
    }
    if (lex_.template_literals && ch == '`') {
      std::size_t k = i_ + 1;
      int depth = 0;
      while (k < s_.size()) {
        if (s_[k] == '\\') {
          k += 2;
          continue;
        }
        if (depth == 0 && s_[k] == '`') break;
        if (starts(k, "${")) {
          ++depth;
          k += 2;
          continue;
        }
        if (depth > 0 && s_[k] == '}') --depth;
        ++k;
      }
      i_ = std::min(k + 1, s_.size());
      emit(TC::String, start, i_);
      return true;
    }
    if (lex_.cpp_raw_strings && starts(i_, "R\"")) {
      const std::size_t open = s_.find('(', i_ + 2);
      if (open != std::string_view::npos && open - (i_ + 2) <= 16) {
        const std::string close = ")" + std::string(s_.substr(i_ + 2, open - i_ - 2)) + "\"";
        const std::size_t end = s_.find(close, open);
        i_ = end == std::string_view::npos ? s_.size() : end + close.size();
        emit(TC::String, start, i_);
        return true;
      }
    }
    if (lex_.rust_raw_strings && (ch == 'r' || ch == 'b')) {
      std::size_t k = i_ + 1;
      if (ch == 'b' && at(k) == 'r') ++k;
      std::size_t hashes = 0;
      while (at(k) == '#') {
        ++hashes;
        ++k;
      }
      if (at(k) == '"' && (ch == 'r' || at(i_ + 1) == 'r' || hashes == 0)) {
        if (ch == 'b' && at(i_ + 1) == '"') {  // b"bytes"
          ++i_;
          quoted('"', start, '\\', true);
          return true;
        }
        if (ch == 'r' || at(i_ + 1) == 'r') {
          const std::string close = "\"" + std::string(hashes, '#');
          const std::size_t end = s_.find(close, k + 1);
          i_ = end == std::string_view::npos ? s_.size() : end + close.size();
          emit(TC::String, start, i_);
          return true;
        }
      }
    }
    if (lex_.quotes.find(ch) != std::string_view::npos && ch != '\0') {
      // Backquoted strings (Go raw strings, shell command substitution) may span lines.
      quoted(ch, start, lex_.escape, lex_.multiline_strings || ch == '`');
      return true;
    }
    if (lex_.char_literals && ch == '\'') {
      // 'x', '\n', '\u{1F600}' or one UTF-8 character; anything else ('a lifetime) is not a literal.
      std::size_t k = i_ + 1;
      if (at(k) == '\\') {
        k += 2;
        while (k < s_.size() && k - i_ < 12 && s_[k] != '\'' && s_[k] != '\n') ++k;
      } else if (k < s_.size()) {
        k += static_cast<std::size_t>(util::decode_utf8(s_, k).length);
      }
      if (at(k) == '\'') {
        i_ = k + 1;
        emit(TC::String, start, i_);
        return true;
      }
    }
    return false;
  }

  bool dollar_variable() {
    const std::size_t start = i_;
    const char next = at(i_ + 1);
    if (next == '{') {
      const std::size_t end = s_.substr(i_, 256).find('}');  // bounded: keeps the lexer linear
      if (end == std::string_view::npos) return false;
      i_ += end + 1;
    } else if (ident_char(next) && next != '\0') {
      i_ += 2;
      while (i_ < s_.size() && ident_char(s_[i_]) && s_[i_] != '$') ++i_;
    } else if (next != '\0' && std::string_view("?#@*!$-0123456789_").find(next) != std::string_view::npos) {
      i_ += 2;
    } else {
      return false;
    }
    emit(TC::Variable, start, i_);
    return true;
  }

  void number() {
    const std::size_t start = i_;
    while (i_ < s_.size()) {
      const char ch = s_[i_];
      if (util::is_ascii_alpha(ch) || util::is_ascii_digit(ch) || ch == '_' || ch == '.' || ch == '%' || ch == '\'') {
        // A digit separator ' only between digits (C++14); '.' only when not a range or method call.
        if (ch == '\'' && !(util::is_ascii_digit(at(i_ + 1)) && lex_.cpp_raw_strings)) break;
        if (ch == '.' && (at(i_ + 1) == '.' || (util::is_ascii_alpha(at(i_ + 1)) && at(i_ + 1) != 'e' && at(i_ + 1) != 'E'))) {
          break;
        }
        if (ch == '%' && !lex_.css) break;
        const char lower = util::ascii_lower(ch);
        ++i_;
        // Exponent signs: 1e-5, 0x1p+3.
        if ((lower == 'e' || lower == 'p') && (at(i_) == '+' || at(i_) == '-') && util::is_ascii_digit(at(i_ + 1))) ++i_;
        continue;
      }
      break;
    }
    emit(TC::Number, start, i_);
  }

  void identifier() {
    const std::size_t start = i_;
    ++i_;
    while (i_ < s_.size() && ident_char(s_[i_])) ++i_;
    // Ruby/Elixir/Crystal predicate and bang methods (empty?, save!).
    if ((at(i_) == '?' || at(i_) == '!') && lex_.identifier_extra.find('?') != std::string_view::npos &&
        at(i_ + 1) != '=') {
      ++i_;
    }
    const std::string_view word = s_.substr(start, i_ - start);
    // String prefixes: r"..", b'..', f"..", rb"..", u8"..", L"..", @"..".
    if (word.size() <= 2 && (at(i_) == '"' || at(i_) == '\'') && lex_.quotes.find(at(i_)) != std::string_view::npos) {
      const bool prefix = std::ranges::all_of(word, [](char ch) { return std::string_view("rRbBuUfFL8").find(ch) != std::string_view::npos; });
      if (prefix) {
        const bool raw = word.find_first_of("rR") != std::string_view::npos;
        quoted(at(i_), start, raw ? '\0' : lex_.escape, lex_.multiline_strings);
        return;
      }
    }
    std::string lowered;
    std::string_view key = word;
    if (lex_.case_insensitive) {
      lowered = util::to_lower(word);
      key = lowered;
    }
    TC cls = TC::Plain;
    std::size_t k = i_;
    while (k < s_.size() && (s_[k] == ' ' || s_[k] == '\t')) ++k;
    const bool called = at(k) == '(';
    if (lex_.css && brace_depth_ > 0 && at(k) == ':' && at(k + 1) != ':') {
      cls = TC::Variable;  // a property name
    } else if (c_.keywords.contains(key)) {
      cls = TC::Keyword;
    } else if (c_.constants.contains(key)) {
      cls = TC::Constant;
    } else if (c_.types.contains(key)) {
      cls = TC::Type;
    } else if (lex_.lisp && lisp_head_) {
      cls = TC::Function;
    } else if (after_type_keyword_) {
      cls = TC::Type;
    } else if (called || c_.builtins.contains(key) || after_definition_keyword_) {
      cls = TC::Function;
    } else if (lex_.upper_constants && word.size() >= 2 && util::is_ascii_alpha(word[0]) &&
               std::ranges::none_of(word, [](char ch) { return ch >= 'a' && ch <= 'z'; }) &&
               std::ranges::any_of(word, [](char ch) { return ch >= 'A' && ch <= 'Z'; })) {
      cls = TC::Constant;
    } else if (lex_.capitalized_types && word[0] >= 'A' && word[0] <= 'Z') {
      cls = TC::Type;
    }
    lisp_head_ = false;
    after_definition_keyword_ = cls == TC::Keyword && is_definition_keyword(key);
    after_type_keyword_ = cls == TC::Keyword && is_type_keyword(key);
    emit(cls, start, i_);
  }

  static bool is_type_keyword(std::string_view word) {
    constexpr std::array<std::string_view, 12> words = {"class",  "struct",   "interface", "enum",   "trait", "union",
                                                        "record", "protocol", "extension", "object", "actor", "typedef"};
    return std::ranges::find(words, word) != words.end();
  }
  static bool is_definition_keyword(std::string_view word) {
    constexpr std::array<std::string_view, 10> words = {"def", "fn", "func", "function", "fun", "sub", "proc",
                                                        "defn", "defp", "procedure"};
    return std::ranges::find(words, word) != words.end();
  }

  // -- markup ---------------------------------------------------------------------------
  bool markup_tag() {
    const std::size_t start = i_;
    if (starts(i_, "<!--")) {
      const std::size_t end = s_.find("-->", i_ + 4);
      i_ = end == std::string_view::npos ? s_.size() : end + 3;
      emit(TC::Comment, start, i_);
      return true;
    }
    std::size_t k = i_ + 1;
    const bool closing = at(k) == '/';
    if (closing) ++k;
    if (!util::is_ascii_alpha(at(k)) && at(k) != '!') return false;
    emit(TC::Operator, i_, k);
    std::size_t name_end = k;
    while (name_end < s_.size() && (ident_char(s_[name_end]) || s_[name_end] == '-' || s_[name_end] == ':' ||
                                    s_[name_end] == '!' || s_[name_end] == '.')) {
      ++name_end;
    }
    const std::string name = util::to_lower(s_.substr(k, name_end - k));
    emit(TC::Tag, k, name_end);
    i_ = name_end;
    // Attributes up to '>'.
    while (i_ < s_.size() && s_[i_] != '>') {
      const char ch = s_[i_];
      const std::size_t from = i_;
      if (ch == '"' || ch == '\'') {
        quoted(ch, from, '\0', true);
      } else if (util::is_ascii_space(ch)) {
        while (i_ < s_.size() && util::is_ascii_space(s_[i_])) ++i_;
        emit(TC::Plain, from, i_);
      } else if (ch == '=' || ch == '/') {
        ++i_;
        emit(TC::Operator, from, i_);
      } else if (ch == '<') {
        break;  // malformed: a new tag begins
      } else {
        while (i_ < s_.size() && !util::is_ascii_space(s_[i_]) && s_[i_] != '=' && s_[i_] != '>' && s_[i_] != '/' &&
               s_[i_] != '"' && s_[i_] != '\'' && s_[i_] != '<') {
          ++i_;
        }
        if (i_ == from) ++i_;
        emit(TC::Attribute, from, i_);
      }
    }
    if (i_ < s_.size() && s_[i_] == '>') {
      emit(TC::Operator, i_, i_ + 1);
      ++i_;
    }
    // Embedded script and style content is lexed as JavaScript / CSS.
    if (!closing && (name == "script" || name == "style")) {
      const std::string close = "</" + name;
      std::size_t end = i_;
      while (true) {
        end = s_.find('<', end);
        if (end == std::string_view::npos || util::istarts_with(s_.substr(end), close)) break;
        ++end;
      }
      if (end == std::string_view::npos) end = s_.size();
      const Language* inner = find_language(name == "script" ? "js" : "css");
      if (inner != nullptr && end > i_) {
        const Compiled compiled(*inner->lexicon);
        Lexer nested(s_.substr(i_, end - i_), compiled, base_ + i_, out_);
        nested.run();
      }
      i_ = end;
    }
    return true;
  }

  // -- diff -----------------------------------------------------------------------------
  void run_diff() {
    while (i_ < s_.size()) {
      const std::size_t start = i_;
      std::size_t end = line_end(i_);
      if (end < s_.size()) ++end;  // keep the line break with its line
      const std::string_view line = s_.substr(start, end - start);
      TC cls = TC::Plain;
      if (line.starts_with("+++") || line.starts_with("---") || line.starts_with("diff ") ||
          line.starts_with("index ") || line.starts_with("***") || line.starts_with("Index: ")) {
        cls = TC::Heading;
      } else if (line.starts_with("@@")) {
        cls = TC::Preprocessor;
      } else if (line.starts_with("+") || line.starts_with(">")) {
        cls = TC::Inserted;
      } else if (line.starts_with("-") || line.starts_with("<")) {
        cls = TC::Deleted;
      } else if (line.starts_with("!")) {
        cls = TC::Keyword;
      }
      i_ = end;
      emit(cls, start, end);
    }
  }

  std::string_view s_;
  const Compiled& c_;
  const Lexicon& lex_;
  std::size_t base_;
  std::vector<Token>& out_;
  std::size_t i_ = 0;
  std::size_t brace_depth_ = 0;
  bool lisp_head_ = false;
  bool after_definition_keyword_ = false;
  bool after_type_keyword_ = false;
  TC last_significant_ = TC::Plain;
};

}  // namespace

std::string_view token_class_name(TokenClass cls) noexcept {
  switch (cls) {
    case TC::Plain: return "plain";
    case TC::Keyword: return "keyword";
    case TC::Type: return "type";
    case TC::String: return "string";
    case TC::Number: return "number";
    case TC::Comment: return "comment";
    case TC::Operator: return "operator";
    case TC::Preprocessor: return "preprocessor";
    case TC::Function: return "function";
    case TC::Constant: return "constant";
    case TC::Variable: return "variable";
    case TC::Tag: return "tag";
    case TC::Attribute: return "attribute";
    case TC::Inserted: return "inserted";
    case TC::Deleted: return "deleted";
    case TC::Heading: return "heading";
  }
  return "plain";
}

std::vector<Token> tokenize(std::string_view code, const Lexicon& lexicon) {
  std::vector<Token> out;
  out.reserve(code.size() / 4 + 1);
  const Compiled compiled(lexicon);
  Lexer lexer(code, compiled, 0, out);
  lexer.run();
  return out;
}

std::vector<Token> tokenize(std::string_view code, std::string_view language) {
  const Language* found = find_language(language);
  if (found == nullptr) {
    if (code.empty()) return {};
    return {Token{0, code.size(), TokenClass::Plain}};
  }
  return tokenize(code, *found->lexicon);
}

}  // namespace cvt::code
