// Convertible core - code family: token-level minifiers and pretty-printers.
//
//   css-min / css-pretty        and css -> css (compress)
//   js-min / js-pretty          and js/mjs -> same (compress)
//   html-min                    any HTML file (raw bytes) -> minified HTML
//   sql-min / sql-pretty        and sql -> sql (pretty by default)
//   graphql-min / graphql-pretty and graphql -> graphql (compress)
//
// Everything works on tokens: strings, template literals, regular
// expressions, url(...) values and <pre>/<script>/<style> content are
// copied verbatim, and no identifier is ever renamed. Malformed input
// (an unterminated string or comment) is an error, never mangled output.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/code/minify.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/code/code.hpp"
#include "util/text.hpp"

namespace cvt::code {

namespace {

bool word_char(char c) {
  return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '$' || c == '\\' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// ============================================================================ JavaScript

struct JsToken {
  enum class Kind { Word, Number, String, Template, Regex, Punct, Comment };
  Kind kind = Kind::Punct;
  std::string_view text;
  bool newline_before = false;  // a line terminator separated it from the previous token
};

constexpr std::array<std::string_view, 50> js_puncts = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?" "?=", "=>", "==", "!=", "<=", ">=", "&&",
    "||",   "??",  "?.",  "++",  "--",  "+=",  "-=",  "*=",  "/=",  "%=",  "&=",  "|=", "^=", "**", "<<", ">>", "{",
    "}",    "(",   ")",   "[",   "]",   ";",   ",",   "<",   ">",   "+",   "-",   "*",  "%",  "&",  "|",  "^"};
constexpr std::string_view js_single = "!~?:=./@#";

bool regex_allowed_after(const JsToken* prev) {
  if (prev == nullptr) return true;
  switch (prev->kind) {
    case JsToken::Kind::Number:
    case JsToken::Kind::String:
    case JsToken::Kind::Template:
    case JsToken::Kind::Regex: return false;
    case JsToken::Kind::Word: {
      constexpr std::array<std::string_view, 15> keywords = {"return", "typeof", "instanceof", "in", "of",   "new",   "delete", "void",
                                                             "throw",  "case",   "do",         "else", "yield", "await", "extends"};
      return std::ranges::find(keywords, prev->text) != keywords.end();
    }
    case JsToken::Kind::Punct: return prev->text != ")" && prev->text != "]" && prev->text != "++" && prev->text != "--";
    case JsToken::Kind::Comment: return true;
  }
  return true;
}

class JsLexer {
 public:
  explicit JsLexer(std::string_view s) : s_(s) {}

  Result<std::vector<JsToken>> run() {
    std::vector<JsToken> out;
    if (s_.starts_with("#!")) {  // hashbang line
      const std::size_t end = s_.find('\n');
      out.push_back({JsToken::Kind::Comment, s_.substr(0, end), false});
      i_ = end == std::string_view::npos ? s_.size() : end;
    }
    bool newline = false;
    const JsToken* prev = nullptr;
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == '\n' || c == '\r' || s_.substr(i_, 3) == "\xE2\x80\xA8" || s_.substr(i_, 3) == "\xE2\x80\xA9") {
        newline = true;
        i_ += (c == '\n' || c == '\r') ? 1 : 3;
        continue;
      }
      if (util::is_ascii_space(c) || s_.substr(i_, 2) == "\xC2\xA0" || s_.substr(i_, 3) == "\xEF\xBB\xBF") {
        i_ += util::is_ascii_space(c) ? 1 : (s_[i_] == '\xC2' ? 2 : 3);
        continue;
      }
      const std::size_t start = i_;
      JsToken token;
      if (s_.substr(i_, 2) == "//") {
        i_ = std::min(s_.find('\n', i_), s_.size());
        token.kind = JsToken::Kind::Comment;
      } else if (s_.substr(i_, 2) == "/*") {
        const std::size_t end = s_.find("*/", i_ + 2);
        if (end == std::string_view::npos) return fail(Status::Decode, "unterminated comment at offset {}", i_);
        i_ = end + 2;
        token.kind = JsToken::Kind::Comment;
        if (s_.substr(start, i_ - start).find('\n') != std::string_view::npos) newline = true;
      } else if (c == '"' || c == '\'') {
        CVT_TRY(skip_string(c));
        token.kind = JsToken::Kind::String;
      } else if (c == '`') {
        CVT_TRY(skip_template());
        token.kind = JsToken::Kind::Template;
      } else if (util::is_ascii_digit(c) || (c == '.' && util::is_ascii_digit(at(i_ + 1)))) {
        // Digits, letters (0x.., 1e5, 10n) and at most one '.': "1.5.toFixed" is 1.5 then .toFixed.
        const bool radix = c == '0' && std::string_view("xXbBoO").find(at(i_ + 1)) != std::string_view::npos;
        bool dot = false;
        while (i_ < s_.size()) {
          const char ch = s_[i_];
          if (ch == '.') {
            if (dot || radix) break;
            dot = true;
            ++i_;
            continue;
          }
          if (!word_char(ch) || ch == '$' || ch == '\\') break;
          ++i_;
          if (!radix && (ch == 'e' || ch == 'E') && (at(i_) == '+' || at(i_) == '-')) ++i_;
        }
        token.kind = JsToken::Kind::Number;
      } else if (word_char(c) || (c == '#' && word_char(at(i_ + 1)))) {
        ++i_;
        while (i_ < s_.size() && word_char(s_[i_])) {
          if (s_[i_] == '\\') ++i_;  // \uXXXX escapes in identifiers
          ++i_;
        }
        i_ = std::min(i_, s_.size());
        token.kind = JsToken::Kind::Word;
      } else if (c == '/' && regex_allowed_after(prev) && skip_regex()) {
        token.kind = JsToken::Kind::Regex;
      } else {
        token.kind = JsToken::Kind::Punct;
        std::size_t length = 0;
        for (const std::string_view p : js_puncts) {
          if (s_.substr(i_, p.size()) == p) {
            length = p.size();
            break;
          }
        }
        if (length == 0) {
          if (js_single.find(c) == std::string_view::npos) {
            return fail(Status::Decode, "unexpected character '{}' at offset {}", c, i_);
          }
          length = 1;
        }
        i_ += length;
      }
      token.text = s_.substr(start, i_ - start);
      token.newline_before = newline;
      newline = false;
      out.push_back(token);
      if (token.kind != JsToken::Kind::Comment) previous_ = token;
      prev = previous_.text.empty() ? nullptr : &previous_;
    }
    return out;
  }

 private:
  [[nodiscard]] char at(std::size_t k) const { return k < s_.size() ? s_[k] : '\0'; }

  Result<void> skip_string(char quote) {
    const std::size_t start = i_++;
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == '\\') {
        i_ += 2;
        continue;
      }
      if (c == quote) {
        ++i_;
        return {};
      }
      if (c == '\n') break;
      ++i_;
    }
    return fail(Status::Decode, "unterminated string starting at offset {}", start);
  }

  Result<void> skip_template() {
    const std::size_t start = i_++;
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == '\\') {
        i_ += 2;
        continue;
      }
      if (c == '`') {
        ++i_;
        return {};
      }
      if (c == '$' && at(i_ + 1) == '{') {
        i_ += 2;
        CVT_TRY(skip_expression());
        continue;
      }
      ++i_;
    }
    return fail(Status::Decode, "unterminated template literal starting at offset {}", start);
  }

  // Inside ${ ... }: up to and including the matching '}'.
  Result<void> skip_expression() {
    int depth = 1;
    if (++nesting_ > 200) return fail(Status::Decode, "template literals are nested too deeply");
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == '"' || c == '\'') {
        CVT_TRY(skip_string(c));
      } else if (c == '`') {
        CVT_TRY(skip_template());
      } else if (s_.substr(i_, 2) == "//") {
        i_ = std::min(s_.find('\n', i_), s_.size());
      } else if (s_.substr(i_, 2) == "/*") {
        const std::size_t end = s_.find("*/", i_ + 2);
        if (end == std::string_view::npos) return fail(Status::Decode, "unterminated comment in a template literal");
        i_ = end + 2;
      } else if (c == '{') {
        ++depth;
        ++i_;
      } else if (c == '}') {
        ++i_;
        if (--depth == 0) {
          --nesting_;
          return {};
        }
      } else {
        ++i_;
      }
    }
    return fail(Status::Decode, "unterminated ${...} in a template literal");
  }

  // A regular expression literal; false (position unchanged) when the line ends first.
  bool skip_regex() {
    std::size_t k = i_ + 1;
    bool in_class = false;
    while (k < s_.size()) {
      const char c = s_[k];
      if (c == '\n' || c == '\r') return false;
      if (c == '\\') {
        k += 2;
        continue;
      }
      if (c == '[') in_class = true;
      if (c == ']') in_class = false;
      if (c == '/' && !in_class) break;
      ++k;
    }
    if (k >= s_.size()) return false;
    ++k;
    while (k < s_.size() && util::is_ascii_alpha(s_[k])) ++k;  // flags
    i_ = k;
    return true;
  }

  std::string_view s_;
  std::size_t i_ = 0;
  int nesting_ = 0;
  JsToken previous_{};  // the last token that was not a comment
};

bool is_punct(const JsToken& t, std::initializer_list<std::string_view> set) {
  return t.kind == JsToken::Kind::Punct && std::ranges::find(set, t.text) != set.end();
}

// Tokens after which no statement can end, so a following line break is never needed.
bool cannot_end_statement(const JsToken& t) {
  if (t.kind != JsToken::Kind::Punct) return false;
  return !is_punct(t, {")", "]", "}", "++", "--"});
}
// Tokens that cannot begin a statement, so automatic semicolon insertion never happens before them.
bool cannot_start_statement(const JsToken& t) {
  if (t.kind == JsToken::Kind::Word) return t.text == "instanceof" || t.text == "in";
  if (t.kind != JsToken::Kind::Punct) return false;
  return !is_punct(t, {"(", "[", "{", "+", "-", "++", "--", "!", "~", "...", "<", "#", "@"});
}

bool needs_space(const JsToken& prev, const JsToken& next) {
  const char a = prev.text.back(), b = next.text.front();
  if (word_char(a) && word_char(b)) return true;
  if ((a == '+' && b == '+') || (a == '-' && b == '-')) return true;
  if (a == '/' && (b == '/' || b == '*')) return true;
  if (prev.kind == JsToken::Kind::Number && b == '.' && prev.text.find_first_of(".eExXbBoO") == std::string_view::npos) {
    return true;
  }
  if (a == '<' && b == '!') return true;
  if (prev.text.ends_with("--") && b == '>') return true;
  return false;
}

bool license_comment(std::string_view text) { return text.starts_with("/*!") || text.find("@license") != std::string_view::npos || text.find("@preserve") != std::string_view::npos; }

// ============================================================================ CSS

struct CssToken {
  enum class Kind { Space, Comment, String, Url, Other };
  Kind kind;
  std::string_view text;
};

Result<std::vector<CssToken>> css_tokens(std::string_view s) {
  std::vector<CssToken> out;
  std::size_t i = 0;
  while (i < s.size()) {
    const std::size_t start = i;
    const char c = s[i];
    if (util::is_ascii_space(c)) {
      while (i < s.size() && util::is_ascii_space(s[i])) ++i;
      out.push_back({CssToken::Kind::Space, s.substr(start, i - start)});
    } else if (s.substr(i, 2) == "/*") {
      const std::size_t end = s.find("*/", i + 2);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated comment at offset {}", i);
      i = end + 2;
      out.push_back({CssToken::Kind::Comment, s.substr(start, i - start)});
    } else if (c == '"' || c == '\'') {
      ++i;
      while (i < s.size() && s[i] != c) {
        if (s[i] == '\\') ++i;
        if (s[i] == '\n') return fail(Status::Decode, "unterminated string at offset {}", start);
        ++i;
      }
      if (i >= s.size()) return fail(Status::Decode, "unterminated string at offset {}", start);
      ++i;
      out.push_back({CssToken::Kind::String, s.substr(start, i - start)});
    } else if (util::istarts_with(s.substr(i), "url(") && (i == 0 || !word_char(s[i - 1]))) {
      std::size_t k = i + 4;
      while (k < s.size() && util::is_ascii_space(s[k])) ++k;
      if (k < s.size() && (s[k] == '"' || s[k] == '\'')) {  // quoted: handled as name + string
        out.push_back({CssToken::Kind::Other, s.substr(i, 4)});
        i += 4;
        continue;
      }
      const std::size_t end = s.find(')', k);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated url( at offset {}", i);
      i = end + 1;
      out.push_back({CssToken::Kind::Url, s.substr(start, i - start)});
    } else {
      // A run of characters up to the next special one.
      ++i;
      while (i < s.size() && !util::is_ascii_space(s[i]) && s[i] != '"' && s[i] != '\'' && s.substr(i, 2) != "/*" &&
             std::string_view("{};:,>+~()").find(s[i]) == std::string_view::npos &&
             std::string_view("{};:,>+~()").find(s[i - 1]) == std::string_view::npos && !util::istarts_with(s.substr(i), "url(")) {
        ++i;
      }
      out.push_back({CssToken::Kind::Other, s.substr(start, i - start)});
    }
  }
  return out;
}

bool css_char(const CssToken* t, std::string_view set, bool back) {
  if (t == nullptr || t->text.empty() || t->kind != CssToken::Kind::Other) return false;
  return set.find(back ? t->text.back() : t->text.front()) != std::string_view::npos;
}

// ============================================================================ SQL and GraphQL tokens

struct Tok {
  enum class Kind { Word, Number, String, Punct, Comment, Hint };
  Kind kind;
  std::string_view text;
  bool newline_before = false;
};

Result<std::vector<Tok>> sql_tokens(std::string_view s) {
  std::vector<Tok> out;
  std::size_t i = 0;
  bool newline = false;
  while (i < s.size()) {
    const std::size_t start = i;
    const char c = s[i];
    if (util::is_ascii_space(c)) {
      newline = newline || c == '\n';
      ++i;
      continue;
    }
    Tok t{Tok::Kind::Punct, {}, newline};
    if (s.substr(i, 2) == "--" || c == '#') {
      i = std::min(s.find('\n', i), s.size());
      t.kind = Tok::Kind::Comment;
    } else if (s.substr(i, 2) == "/*") {
      const std::size_t end = s.find("*/", i + 2);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated comment at offset {}", i);
      i = end + 2;
      t.kind = (s.substr(start, 3) == "/*+" || s.substr(start, 3) == "/*!") ? Tok::Kind::Hint : Tok::Kind::Comment;
    } else if (c == '\'' || c == '"' || c == '`' || c == '[') {
      const char close = c == '[' ? ']' : c;
      ++i;
      while (true) {
        if (i >= s.size()) return fail(Status::Decode, "unterminated quoted text at offset {}", start);
        if (s[i] == close) {
          if (i + 1 < s.size() && s[i + 1] == close && close != ']') {
            i += 2;  // doubled quote
            continue;
          }
          ++i;
          break;
        }
        if (s[i] == '\\' && c == '\'') ++i;  // MySQL-style backslash escapes
        ++i;
      }
      t.kind = c == '\'' ? Tok::Kind::String : Tok::Kind::Word;
    } else if (util::is_ascii_digit(c) || (c == '.' && i + 1 < s.size() && util::is_ascii_digit(s[i + 1]))) {
      while (i < s.size() && (util::is_ascii_alpha(s[i]) || util::is_ascii_digit(s[i]) || s[i] == '.')) {
        ++i;
        if ((s[i - 1] == 'e' || s[i - 1] == 'E') && i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
      }
      t.kind = Tok::Kind::Number;
    } else if (word_char(c) || c == '@' || (c == ':' && i + 1 < s.size() && word_char(s[i + 1]))) {
      ++i;
      while (i < s.size() && (word_char(s[i]) || s[i] == '.' || s[i] == '@')) ++i;
      t.kind = Tok::Kind::Word;
    } else {
      constexpr std::array<std::string_view, 12> ops = {"<>", "<=", ">=", "!=", "||", "::", "->>", "->", ":=", "<<", ">>", "=="};
      std::size_t length = 1;
      for (const std::string_view op : ops) {
        if (s.substr(i, op.size()) == op) {
          length = op.size();
          break;
        }
      }
      i += length;
    }
    t.text = s.substr(start, i - start);
    newline = false;
    out.push_back(t);
  }
  return out;
}

Result<std::vector<Tok>> graphql_tokens(std::string_view s) {
  std::vector<Tok> out;
  std::size_t i = 0;
  while (i < s.size()) {
    const std::size_t start = i;
    const char c = s[i];
    if (util::is_ascii_space(c) || c == ',' || s.substr(i, 3) == "\xEF\xBB\xBF") {
      i += s.substr(i, 3) == "\xEF\xBB\xBF" ? 3 : 1;
      continue;
    }
    Tok t{Tok::Kind::Punct, {}, false};
    if (c == '#') {
      i = std::min(s.find('\n', i), s.size());
      t.kind = Tok::Kind::Comment;
    } else if (s.substr(i, 3) == "\"\"\"") {
      std::size_t k = i + 3;
      while (k < s.size() && s.substr(k, 3) != "\"\"\"") k += s.substr(k, 4) == "\\\"\"\"" ? 4 : 1;
      if (k >= s.size()) return fail(Status::Decode, "unterminated block string at offset {}", i);
      i = k + 3;
      t.kind = Tok::Kind::String;
    } else if (c == '"') {
      ++i;
      while (i < s.size() && s[i] != '"' && s[i] != '\n') i += s[i] == '\\' ? 2 : 1;
      if (i >= s.size() || s[i] != '"') return fail(Status::Decode, "unterminated string at offset {}", start);
      ++i;
      t.kind = Tok::Kind::String;
    } else if (util::is_ascii_digit(c) || c == '-') {
      ++i;
      while (i < s.size() && (util::is_ascii_digit(s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                              ((s[i] == '+' || s[i] == '-') && (s[i - 1] == 'e' || s[i - 1] == 'E')))) {
        ++i;
      }
      t.kind = Tok::Kind::Number;
    } else if (util::is_ascii_alpha(c) || c == '_') {
      while (i < s.size() && (util::is_ascii_alpha(s[i]) || util::is_ascii_digit(s[i]) || s[i] == '_')) ++i;
      t.kind = Tok::Kind::Word;
    } else if (s.substr(i, 3) == "...") {
      i += 3;
    } else if (std::string_view("!$&()/:=@[]{}|").find(c) != std::string_view::npos) {
      ++i;
    } else {
      return fail(Status::Decode, "unexpected character '{}' at offset {}", c, i);
    }
    t.text = s.substr(start, i - start);
    out.push_back(t);
  }
  return out;
}

bool value_token(const Tok& t) { return t.kind == Tok::Kind::Word || t.kind == Tok::Kind::Number || t.kind == Tok::Kind::String; }

}  // namespace

// ============================================================================ public functions

Result<std::string> minify_js(std::string_view js, const MinifyOptions& options) {
  CVT_TRY_ASSIGN(const std::vector<JsToken> tokens, JsLexer(js).run());
  std::string out;
  out.reserve(js.size());
  const JsToken* prev = nullptr;
  bool pending_newline = false;
  for (const JsToken& token : tokens) {
    pending_newline = pending_newline || token.newline_before;
    if (token.kind == JsToken::Kind::Comment) {
      const bool keep = token.text.starts_with("#!") || (options.keep_license && license_comment(token.text));
      if (!keep) continue;
      if (!out.empty() && out.back() != '\n') out.push_back('\n');
      out.append(token.text);
      out.push_back('\n');
      pending_newline = false;
      prev = nullptr;
      continue;
    }
    if (prev != nullptr) {
      if (pending_newline && !cannot_end_statement(*prev) && !cannot_start_statement(token)) {
        out.push_back('\n');
      } else if (needs_space(*prev, token)) {
        out.push_back(' ');
      }
    }
    out.append(token.text);
    prev = &token;
    pending_newline = false;
  }
  return out;
}

namespace {

bool operator_keyword(std::string_view w) {
  constexpr std::array<std::string_view, 22> words = {"return", "typeof", "instanceof", "in",     "of",     "new",
                                                      "delete", "void",   "throw",      "case",   "yield",  "await",
                                                      "else",   "do",     "const",      "let",    "var",    "export",
                                                      "import", "extends", "if",        "while"};
  return std::ranges::find(words, w) != words.end();
}

// True when `t` ends an operand, so a following + or - is binary.
bool ends_value(const JsToken& t) {
  switch (t.kind) {
    case JsToken::Kind::Number:
    case JsToken::Kind::String:
    case JsToken::Kind::Template:
    case JsToken::Kind::Regex: return true;
    case JsToken::Kind::Word: return !operator_keyword(t.text) && t.text != "for" && t.text != "switch" && t.text != "catch";
    case JsToken::Kind::Punct: return is_punct(t, {")", "]", "}", "++", "--"});
    case JsToken::Kind::Comment: return false;
  }
  return false;
}

}  // namespace

Result<std::string> pretty_js(std::string_view js, int indent) {
  CVT_TRY_ASSIGN(const std::vector<JsToken> tokens, JsLexer(js).run());
  std::string out;
  int depth = 0, parens = 0, ternary = 0;
  bool line_start = true, after_close = false, prev_binary = false;
  const JsToken* prev = nullptr;
  const auto newline = [&] {
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (!out.empty() && out.back() != '\n') out.push_back('\n');
    line_start = true;
  };
  const auto put = [&](std::string_view text) {
    if (line_start) {
      out.append(static_cast<std::size_t>(std::max(0, depth * indent)), ' ');
      line_start = false;
    }
    out.append(text);
  };
  constexpr std::array<std::string_view, 33> binary_ops = {
      "=",  "==", "===", "!=", "!==", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=",
      "&&=", "||=", "&&", "||", "<",   ">",  "<=", ">=", "=>", "*",  "/",  "%",   "**",  "<<",   ">>",  ">>>"};
  for (const JsToken& token : tokens) {
    if (token.kind == JsToken::Kind::Comment) {
      if (prev != nullptr && !token.newline_before && token.text.starts_with("//")) {
        // A trailing comment stays on its line, even after the line break a ';' produced.
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        out += " ";
        out.append(token.text);
        line_start = false;
      } else {
        newline();
        put(token.text);
      }
      newline();
      continue;
    }
    if (prev != nullptr && token.newline_before && !line_start && !cannot_end_statement(*prev) && !cannot_start_statement(token)) {
      newline();
    }
    if (is_punct(token, {"}"})) {
      depth = std::max(0, depth - 1);
      newline();
      put("}");
      prev = &token;
      after_close = true;
      prev_binary = false;
      continue;
    }
    if (after_close && !line_start) {
      const bool continues = is_punct(token, {")", "]", ",", ";", ".", "?.", "("}) ||
                             (token.kind == JsToken::Kind::Word &&
                              (token.text == "else" || token.text == "catch" || token.text == "finally" || token.text == "while"));
      if (!continues) newline();
    }
    after_close = false;
    bool binary = false;
    if (token.kind == JsToken::Kind::Punct) {
      binary = std::ranges::find(binary_ops, token.text) != binary_ops.end() || token.text == "??" ||
               ((token.text == "+" || token.text == "-" || token.text == "&" || token.text == "|" || token.text == "^") &&
                prev != nullptr && ends_value(*prev)) ||
               (token.text == "?" && prev != nullptr && ends_value(*prev)) || (token.text == ":" && ternary > 0);
    }
    if (!line_start && prev != nullptr) {
      bool space;
      if (needs_space(*prev, token)) {
        space = true;
      } else if (binary || prev_binary) {
        space = true;
      } else if (is_punct(token, {",", ";", ")", "]", ".", "?.", ":"}) || (is_punct(token, {"++", "--"}) && ends_value(*prev))) {
        space = false;
      } else if (is_punct(*prev, {"(", "[", ".", "?.", "!", "~", "...", "#", "@", "+", "-", "++", "--"})) {
        space = false;  // after an opening bracket, a member access or a prefix operator
      } else if (is_punct(token, {"(", "["}) && (is_punct(*prev, {")", "]"}) ||
                                                (prev->kind == JsToken::Kind::Word && !operator_keyword(prev->text) &&
                                                 prev->text != "for" && prev->text != "switch" && prev->text != "catch"))) {
        space = false;  // a call or an index
      } else if (token.kind == JsToken::Kind::Template && (prev->kind == JsToken::Kind::Word || is_punct(*prev, {")"}))) {
        space = false;  // a tagged template
      } else {
        space = true;
      }
      if (space) out.push_back(' ');
    }
    put(token.text);
    if (token.text == "?" && binary) ++ternary;
    if (token.text == ":" && binary) --ternary;
    prev_binary = binary;
    if (is_punct(token, {"("})) ++parens;
    if (is_punct(token, {")"})) parens = std::max(0, parens - 1);
    if (is_punct(token, {"{"})) {
      ++depth;
      newline();
    } else if (is_punct(token, {";"}) && parens == 0) {
      newline();
    }
    prev = &token;
  }
  newline();
  return out;
}

Result<std::string> minify_css(std::string_view css, const MinifyOptions& options) {
  CVT_TRY_ASSIGN(const std::vector<CssToken> tokens, css_tokens(css));
  std::string out;
  out.reserve(css.size());
  const CssToken* prev = nullptr;
  bool space = false;
  int parens = 0;
  int depth = 0;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const CssToken& t = tokens[i];
    if (t.kind == CssToken::Kind::Space) {
      space = true;
      continue;
    }
    if (t.kind == CssToken::Kind::Comment) {
      if (options.keep_license && t.text.starts_with("/*!")) {
        out.append(t.text);
        prev = nullptr;
      }
      space = space || prev != nullptr;  // a comment separates tokens like a space
      continue;
    }
    if (t.kind == CssToken::Kind::Other && t.text == ";") {
      // Drop the last semicolon of a block and repeated semicolons.
      std::size_t k = i + 1;
      while (k < tokens.size() && (tokens[k].kind == CssToken::Kind::Space || tokens[k].kind == CssToken::Kind::Comment)) ++k;
      if (k < tokens.size() && tokens[k].kind == CssToken::Kind::Other && (tokens[k].text == "}" || tokens[k].text == ";")) {
        space = false;
        continue;
      }
    }
    // A colon inside a block that is followed by ';' or '}' before any '{' separates
    // a property from its value: spaces around it can go. Elsewhere (selectors such as
    // "a :first-child") the space before a colon is significant.
    bool declaration_colon = false;
    if (t.kind == CssToken::Kind::Other && t.text == ":" && (depth > 0 || parens > 0)) {
      declaration_colon = parens > 0;
      if (!declaration_colon) {
        std::size_t k = i + 1;
        while (k < tokens.size() && !(tokens[k].kind == CssToken::Kind::Other &&
                                      (tokens[k].text == ";" || tokens[k].text == "}" || tokens[k].text == "{"))) {
          ++k;
        }
        declaration_colon = k == tokens.size() || tokens[k].text != "{";
      }
    }
    if (space && prev != nullptr) {
      const bool around = css_char(prev, parens > 0 ? "{};,(:" : "{};,>+~(:", true) ||
                          css_char(&t, parens > 0 ? "{};,)" : "{};,>+~)!", false) || declaration_colon;
      if (!around) out.push_back(' ');
    }
    if (t.kind == CssToken::Kind::Other && t.text == "(") ++parens;
    if (t.kind == CssToken::Kind::Other && t.text == ")") parens = std::max(0, parens - 1);
    if (t.kind == CssToken::Kind::Other && t.text == "{") ++depth;
    if (t.kind == CssToken::Kind::Other && t.text == "}") depth = std::max(0, depth - 1);
    out.append(t.text);
    prev = &t;
    space = false;
  }
  return out;
}

Result<std::string> pretty_css(std::string_view css, int indent) {
  CVT_TRY_ASSIGN(const std::string compact, minify_css(css, {true}));
  CVT_TRY_ASSIGN(const std::vector<CssToken> tokens, css_tokens(compact));
  std::string out;
  int depth = 0;
  int parens = 0;
  bool line_start = true;
  const auto pad = [&] {
    if (line_start) out.append(static_cast<std::size_t>(depth * indent), ' ');
    line_start = false;
  };
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const CssToken& t = tokens[i];
    if (t.kind == CssToken::Kind::Space) {
      if (!line_start) out.push_back(' ');
      continue;
    }
    if (t.kind == CssToken::Kind::Comment) {
      pad();
      out.append(t.text);
      out.push_back('\n');
      line_start = true;
      continue;
    }
    if (t.kind == CssToken::Kind::Other && parens == 0) {
      if (t.text == "{") {
        pad();
        out += " {\n";
        ++depth;
        line_start = true;
        continue;
      }
      if (t.text == "}") {
        if (!line_start) {
          // End an unterminated declaration; leave other block content (a custom property's
          // {...} value) exactly as it was.
          const std::size_t line = out.rfind('\n');
          const std::string_view last = std::string_view(out).substr(line == std::string::npos ? 0 : line + 1);
          out += last.find(':') != std::string_view::npos ? ";\n" : "\n";
        }
        depth = std::max(0, depth - 1);
        line_start = true;
        pad();
        out += "}\n";
        line_start = true;
        continue;
      }
      if (t.text == ";") {
        out += ";\n";
        line_start = true;
        continue;
      }
      if (t.text == ":" && depth > 0) {
        // In a declaration (no '{' before the next ';' or '}'), a space follows the colon.
        std::size_t k = i + 1;
        while (k < tokens.size() && tokens[k].text != ";" && tokens[k].text != "}" && tokens[k].text != "{") ++k;
        out += (k < tokens.size() && tokens[k].text == "{") ? ":" : ": ";
        continue;
      }
      if (t.text == ",") {
        out += depth == 0 || (i + 1 < tokens.size() && tokens[i + 1].text != "{") ? ", " : ",";
        continue;
      }
      if (t.text == ">" || t.text == "+" || t.text == "~") {
        out += " " + std::string(t.text) + " ";
        continue;
      }
    }
    if (t.text == "(") ++parens;
    if (t.text == ")") parens = std::max(0, parens - 1);
    pad();
    out.append(t.text);
  }
  // Tidy double spaces produced by combinator handling (indentation is kept).
  std::string tidy;
  bool indenting = true;
  for (const char c : out) {
    if (c == '\n') {
      indenting = true;
    } else if (c != ' ') {
      indenting = false;
    } else if (!indenting && !tidy.empty() && tidy.back() == ' ') {
      continue;
    }
    tidy.push_back(c);
  }
  return tidy;
}

Result<std::string> minify_html(std::string_view html, const MinifyOptions&) {
  std::string out;
  out.reserve(html.size());
  std::size_t i = 0;
  bool space = false;
  const auto flush_space = [&] {
    if (space && !out.empty()) out.push_back(' ');
    space = false;
  };
  while (i < html.size()) {
    const char c = html[i];
    if (util::is_ascii_space(c)) {
      space = true;
      ++i;
      continue;
    }
    if (html.substr(i, 4) == "<!--") {
      const std::size_t end = html.find("-->", i + 4);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated HTML comment at offset {}", i);
      const std::string_view comment = html.substr(i, end + 3 - i);
      if (comment.starts_with("<!--[if") || comment.starts_with("<!--<![endif]") || comment.find("[endif]") != std::string_view::npos) {
        flush_space();
        out.append(comment);
      }
      i = end + 3;
      continue;
    }
    if (c == '<' && i + 1 < html.size() && (util::is_ascii_alpha(html[i + 1]) || html[i + 1] == '/' || html[i + 1] == '!' || html[i + 1] == '?')) {
      flush_space();
      // Copy the tag, collapsing whitespace between attributes but keeping quoted values verbatim.
      std::size_t k = i;
      std::string tag;
      bool tag_space = false;
      while (k < html.size() && html[k] != '>') {
        const char ch = html[k];
        if (ch == '"' || ch == '\'') {
          const std::size_t close = html.find(ch, k + 1);
          if (close == std::string_view::npos) return fail(Status::Decode, "unterminated attribute value at offset {}", k);
          if (tag_space && tag.back() != '=') tag.push_back(' ');
          tag_space = false;
          tag.append(html.substr(k, close + 1 - k));
          k = close + 1;
          continue;
        }
        if (util::is_ascii_space(ch)) {
          tag_space = true;
          ++k;
          continue;
        }
        if (tag_space && ch != '=' && tag.back() != '=' && ch != '/') tag.push_back(' ');
        if (tag_space && ch == '/' && html[k + 1] != '>') tag.push_back(' ');
        tag_space = false;
        tag.push_back(ch);
        ++k;
      }
      if (k >= html.size()) return fail(Status::Decode, "unterminated tag at offset {}", i);
      tag.push_back('>');
      out += tag;
      i = k + 1;
      // Raw-text and whitespace-sensitive elements: copy their content verbatim.
      std::string name;
      for (std::size_t n = 1; n < tag.size() && (util::is_ascii_alpha(tag[n]) || util::is_ascii_digit(tag[n])); ++n) {
        name.push_back(util::ascii_lower(tag[n]));
      }
      if (name == "pre" || name == "textarea" || name == "script" || name == "style" || name == "code" || name == "xmp" ||
          name == "plaintext") {
        const std::string close = "</" + name;
        std::size_t end = i;
        while (true) {
          end = html.find("</", end);
          if (end == std::string_view::npos || util::istarts_with(html.substr(end), close)) break;
          end += 2;
        }
        if (end == std::string_view::npos) end = html.size();
        out.append(html.substr(i, end - i));
        i = end;
      }
      continue;
    }
    flush_space();
    out.push_back(c);
    ++i;
  }
  return out;
}

Result<std::string> minify_sql(std::string_view sql) {
  CVT_TRY_ASSIGN(const std::vector<Tok> tokens, sql_tokens(sql));
  std::string out;
  const Tok* prev = nullptr;
  for (const Tok& t : tokens) {
    if (t.kind == Tok::Kind::Comment) continue;
    if (prev != nullptr) {
      const bool tight = (prev->kind == Tok::Kind::Punct && std::string_view("(,;").find(prev->text) != std::string_view::npos) ||
                         (t.kind == Tok::Kind::Punct && std::string_view("),;").find(t.text) != std::string_view::npos) ||
                         (t.kind == Tok::Kind::Punct && t.text == "(" && prev->kind == Tok::Kind::Word && !util::iequals(prev->text, "in") &&
                          !util::iequals(prev->text, "as") && !util::iequals(prev->text, "values") && !util::iequals(prev->text, "on") &&
                          !util::iequals(prev->text, "and") && !util::iequals(prev->text, "or") && !util::iequals(prev->text, "exists") &&
                          !util::iequals(prev->text, "not") && !util::iequals(prev->text, "from") && !util::iequals(prev->text, "join"));
      if (!tight && prev->text != "." && t.text != ".") out.push_back(' ');
    }
    out.append(t.text);
    prev = &t;
  }
  return out;
}

Result<std::string> pretty_sql(std::string_view sql, std::string_view keyword_case, int indent) {
  CVT_TRY_ASSIGN(const std::vector<Tok> tokens, sql_tokens(sql));
  // Clause keywords that start a new line, and the words that may follow to form one clause.
  constexpr std::array<std::string_view, 24> clauses = {
      "select", "from",  "where",  "group", "order",  "having", "limit",  "offset", "union",  "intersect", "except", "values",
      "set",    "join",  "inner",  "left",  "right",  "full",   "cross",  "insert", "update", "delete",    "with",   "returning"};
  constexpr std::array<std::string_view, 96> keywords = {
      "select", "from", "where", "group", "by", "order", "having", "limit", "offset", "union", "all", "intersect", "except",
      "values", "set", "join", "inner", "left", "right", "full", "outer", "cross", "on", "using", "insert", "into", "update",
      "delete", "with", "as", "and", "or", "not", "in", "is", "null", "like", "between", "exists", "case", "when", "then",
      "else", "end", "distinct", "asc", "desc", "create", "table", "view", "index", "drop", "alter", "add", "column",
      "primary", "key", "foreign", "references", "default", "unique", "check", "constraint", "returning", "true", "false",
      "count", "sum", "avg", "min", "max", "cast", "coalesce", "if", "begin", "commit", "rollback", "transaction",
      "integer", "int", "varchar", "text", "boolean", "date", "timestamp", "numeric", "decimal", "real", "bigint", "char",
      "over", "partition", "window", "recursive", "lateral", "natural"};
  const auto lower = [](std::string_view w) { return util::to_lower(w); };
  const auto styled = [&](const Tok& t) -> std::string {
    if (t.kind != Tok::Kind::Word || keyword_case == "keep") return std::string(t.text);
    if (std::ranges::find(keywords, lower(t.text)) == keywords.end()) return std::string(t.text);
    return keyword_case == "lower" ? lower(t.text) : util::to_upper(t.text);
  };
  std::string out;
  int depth = 0;
  std::vector<int> clause_depth;  // parentheses that hold a sub-query
  int parens = 0;
  bool line_start = true;
  const Tok* prev = nullptr;
  const auto newline = [&](int extra) {
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (!out.empty() && out.back() != '\n') out.push_back('\n');
    out.append(static_cast<std::size_t>(std::max(0, (depth + extra) * indent)), ' ');
    line_start = true;
  };
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Tok& t = tokens[i];
    const std::string w = t.kind == Tok::Kind::Word ? lower(t.text) : std::string();
    if (t.kind == Tok::Kind::Comment) {
      if (!line_start) out.push_back(' ');
      out.append(t.text);
      newline(0);
      continue;
    }
    const auto join_modifier = [&](const Tok* p) {
      if (p == nullptr || p->kind != Tok::Kind::Word) return false;
      const std::string pw = lower(p->text);
      return pw == "left" || pw == "right" || pw == "full" || pw == "inner" || pw == "cross" || pw == "outer" || pw == "natural";
    };
    const bool clause = t.kind == Tok::Kind::Word && std::ranges::find(clauses, w) != clauses.end() &&
                        parens == static_cast<int>(clause_depth.size()) && !(join_modifier(prev) && (w == "join" || w == "outer"));
    const bool joined = join_modifier(prev) && (w == "join" || w == "outer");
    if (clause && !joined && prev != nullptr && !(lower(prev->text) == "(")) newline(0);
    if (t.kind == Tok::Kind::Word && (w == "and" || w == "or") && parens == static_cast<int>(clause_depth.size())) newline(1);
    if (!line_start && prev != nullptr) {
      constexpr std::array<std::string_view, 16> spaced_before_paren = {
          "in", "as", "values", "on", "and", "or", "exists", "not", "from", "join", "using", "over", "into", "with", "when", "then"};
      const bool tight = t.text == "," || t.text == ";" || t.text == ")" || t.text == "." || prev->text == "(" || prev->text == "." ||
                         (t.text == "(" && prev->kind == Tok::Kind::Word &&
                          std::ranges::find(spaced_before_paren, lower(prev->text)) == spaced_before_paren.end() &&
                          std::ranges::find(clauses, lower(prev->text)) == clauses.end());
      if (!tight) out.push_back(' ');
    }
    line_start = false;
    out += styled(t);
    if (t.text == "(") {
      ++parens;
      // A sub-query: indent it.
      if (i + 1 < tokens.size() && (lower(tokens[i + 1].text) == "select" || lower(tokens[i + 1].text) == "with")) {
        clause_depth.push_back(parens);
        ++depth;
        newline(0);
      }
    } else if (t.text == ")") {
      if (!clause_depth.empty() && clause_depth.back() == parens) {
        clause_depth.pop_back();
        --depth;
        out.pop_back();
        newline(0);
        out += ")";
      }
      parens = std::max(0, parens - 1);
    } else if (t.text == "," && parens == static_cast<int>(clause_depth.size())) {
      newline(1);
    } else if (t.text == ";") {
      out += "\n";
      line_start = true;
    }
    prev = &t;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
  out += "\n";
  return out;
}

Result<std::string> minify_graphql(std::string_view graphql) {
  CVT_TRY_ASSIGN(const std::vector<Tok> tokens, graphql_tokens(graphql));
  std::string out;
  const Tok* prev = nullptr;
  for (const Tok& t : tokens) {
    if (t.kind == Tok::Kind::Comment) continue;
    if (prev != nullptr && value_token(*prev) && value_token(t) && !(prev->kind == Tok::Kind::String || t.kind == Tok::Kind::String)) {
      out.push_back(' ');
    } else if (prev != nullptr && prev->kind == Tok::Kind::String && t.kind == Tok::Kind::String) {
      out.push_back(' ');
    }
    out.append(t.text);
    prev = &t;
  }
  return out;
}

Result<std::string> pretty_graphql(std::string_view graphql, int indent) {
  CVT_TRY_ASSIGN(const std::vector<Tok> tokens, graphql_tokens(graphql));
  std::string out;
  int depth = 0;
  int parens = 0;
  bool line_start = true;
  const Tok* prev = nullptr;
  const auto newline = [&] {
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (!out.empty() && out.back() != '\n') out.push_back('\n');
    line_start = true;
  };
  const auto put = [&](std::string_view text) {
    if (line_start) out.append(static_cast<std::size_t>(depth * indent), ' ');
    line_start = false;
    out.append(text);
  };
  for (const Tok& t : tokens) {
    if (t.kind == Tok::Kind::Comment) {
      newline();
      put(t.text);
      newline();
      continue;
    }
    if (t.text == "}") {
      --depth;
      if (depth < 0) depth = 0;
      newline();
      put("}");
      if (depth == 0) {
        newline();
        out.push_back('\n');
      }
      prev = &t;
      continue;
    }
    if (!line_start && prev != nullptr) {
      if (t.text == "{") {
        out.push_back(' ');
      } else if (parens > 0 && value_token(*prev) && (value_token(t) || t.text == "$")) {
        out += ", ";
      } else if (prev->text == ":" || prev->text == "=" || (value_token(*prev) && (value_token(t) || t.text == "@" || t.text == "$" || t.text == "...")) ||
                 t.text == "=" || t.text == "|" || prev->text == "|" || (prev->text == ")" && t.text != ")" && t.text != "!")) {
        out.push_back(' ');
      } else if (parens == 0 && prev->kind == Tok::Kind::Punct && prev->text == "]" && value_token(t)) {
        out.push_back(' ');
      } else if (parens > 0 && (prev->text == "!" || prev->text == "]") && (value_token(t) || t.text == "$")) {
        out += ", ";
      }
    }
    if (depth > 0 && parens == 0 && prev != nullptr && prev->text != "{" && prev->text != "..." && prev->text != "@" &&
        prev->text != ":" && prev->text != "=" && prev->text != "|" && prev->text != "$" &&
        (t.kind == Tok::Kind::Word || t.text == "...") && !line_start &&
        (value_token(*prev) || prev->text == ")" || prev->text == "}" || prev->text == "!" || prev->text == "]")) {
      // A new selection inside braces.
      while (!out.empty() && out.back() == ' ') out.pop_back();
      newline();
    }
    put(t.text);
    if (t.text == "(") ++parens;
    if (t.text == ")") --parens;
    if (t.text == "{") {
      ++depth;
      newline();
    }
    prev = &t;
  }
  newline();
  return out;
}

// ============================================================================ registration

namespace {

void add_text_writer(Registry& registry, std::string format_id, std::vector<OptionSpec> options,
                     std::function<Result<std::string>(std::string_view, const Options&)> fn, bool lossy) {
  Codec codec;
  codec.format_id = std::move(format_id);
  codec.kind = Kind::PlainText;
  codec.write_options = std::move(options);
  codec.lossy = lossy;
  codec.write = [fn = std::move(fn)](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    CVT_TRY(ctx.checkpoint(0.0, "Formatting"));
    CVT_TRY_ASSIGN(const std::string text, fn(value.get<ir::PlainText>().text, ctx.options));
    CVT_TRY(ctx.limits.check_output(text.size()));
    return to_bytes(text);
  };
  registry.add_codec(std::move(codec));
}

void add_target(Registry& registry, std::string id, std::string name, std::string description, std::string extension,
                std::string mime, int popularity) {
  FormatInfo info;
  info.id = std::move(id);
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Code;
  info.extensions = {std::move(extension)};
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.sniff = [](ByteView, std::string_view) { return 0; };  // a target, never detected
  registry.add_format(std::move(info));
}

OptionSpec style_option(std::string_view fallback) {
  return OptionSpec::choice("style", "Style", std::string(fallback), {"minify", "pretty", "keep"},
                            "minify: remove comments and whitespace; pretty: re-indent; keep: write the text unchanged.");
}
OptionSpec license_option() {
  return OptionSpec::boolean("keep_license", "Keep licence comments", true, "Keep /*! ... */ comments (licences, banners).");
}
OptionSpec indent_option() { return OptionSpec::integer("indent", "Indent", 2, 1, 8, "Spaces per indentation level."); }

int indent_of(const Options& options) { return static_cast<int>(options.get_int("indent", 2, 1, 8)); }

}  // namespace

void register_minify(Registry& registry) {
  using Fn = std::function<Result<std::string>(std::string_view, const Options&)>;
  const Fn css_min = [](std::string_view t, const Options& o) { return minify_css(t, {o.get_bool("keep_license", true)}); };
  const Fn css_pretty = [](std::string_view t, const Options& o) { return pretty_css(t, indent_of(o)); };
  const Fn js_min = [](std::string_view t, const Options& o) { return minify_js(t, {o.get_bool("keep_license", true)}); };
  const Fn js_pretty = [](std::string_view t, const Options& o) { return pretty_js(t, indent_of(o)); };
  const Fn sql_min = [](std::string_view t, const Options&) { return minify_sql(t); };
  const Fn sql_pretty = [](std::string_view t, const Options& o) {
    return pretty_sql(t, o.get_enum("keyword_case", "upper", {"upper", "lower", "keep"}), indent_of(o));
  };
  const Fn gql_min = [](std::string_view t, const Options&) { return minify_graphql(t); };
  const Fn gql_pretty = [](std::string_view t, const Options& o) { return pretty_graphql(t, indent_of(o)); };
  const auto by_style = [](Fn minify, Fn pretty, std::string_view fallback) -> Fn {
    return [minify, pretty, fallback = std::string(fallback)](std::string_view t, const Options& o) -> Result<std::string> {
      const std::string style = o.get_enum("style", fallback, {"minify", "pretty", "keep"});
      if (style == "keep") return std::string(t);
      return style == "pretty" ? pretty(t, o) : minify(t, o);
    };
  };
  const OptionSpec keyword_case = OptionSpec::choice("keyword_case", "Keyword case", "upper", {"upper", "lower", "keep"},
                                                     "Letter case of SQL keywords.");

  // Same-format conversion of the source formats ("compress").
  add_text_writer(registry, "css", {style_option("minify"), license_option(), indent_option()}, by_style(css_min, css_pretty, "minify"), false);
  for (const char* id : {"js", "mjs"}) {
    add_text_writer(registry, id, {style_option("minify"), license_option(), indent_option()}, by_style(js_min, js_pretty, "minify"), false);
  }
  add_text_writer(registry, "sql", {style_option("pretty"), keyword_case, indent_option()}, by_style(sql_min, sql_pretty, "pretty"), false);
  add_text_writer(registry, "graphql", {style_option("minify"), indent_option()}, by_style(gql_min, gql_pretty, "minify"), false);

  // Explicit targets.
  add_target(registry, "css-min", "Minified CSS", "CSS without comments and redundant whitespace.", "min.css", "text/css", 30);
  add_text_writer(registry, "css-min", {license_option()}, css_min, false);
  add_target(registry, "css-pretty", "Formatted CSS", "CSS re-indented with one declaration per line.", "css", "text/css", 20);
  add_text_writer(registry, "css-pretty", {indent_option()}, css_pretty, false);
  add_target(registry, "js-min", "Minified JavaScript",
             "JavaScript without comments and redundant whitespace (no renaming, safe line breaks kept).", "min.js",
             "text/javascript", 30);
  add_text_writer(registry, "js-min", {license_option()}, js_min, false);
  add_target(registry, "js-pretty", "Formatted JavaScript", "JavaScript re-indented by braces and statements.", "js",
             "text/javascript", 20);
  add_text_writer(registry, "js-pretty", {indent_option()}, js_pretty, false);
  add_target(registry, "sql-min", "Minified SQL", "SQL on one line without comments.", "sql", "application/sql", 15);
  add_text_writer(registry, "sql-min", {}, sql_min, false);
  add_target(registry, "sql-pretty", "Formatted SQL", "SQL with one clause per line and upper-case keywords.", "sql",
             "application/sql", 20);
  add_text_writer(registry, "sql-pretty", {keyword_case, indent_option()}, sql_pretty, false);
  add_target(registry, "graphql-min", "Minified GraphQL", "GraphQL without comments, commas and redundant whitespace.",
             "graphql", "application/graphql", 10);
  add_text_writer(registry, "graphql-min", {}, gql_min, false);
  add_target(registry, "graphql-pretty", "Formatted GraphQL", "GraphQL with one field per line.", "graphql",
             "application/graphql", 10);
  add_text_writer(registry, "graphql-pretty", {indent_option()}, gql_pretty, false);

  // HTML: a Blob writer, because HTML is read as a Document by the document family
  // and minifying must see the markup itself.
  add_target(registry, "html-min", "Minified HTML",
             "HTML without comments and with collapsed whitespace; pre, textarea, script and style are kept as they are.",
             "html", "text/html", 25);
  Codec html;
  html.format_id = "html-min";
  html.kind = Kind::Blob;
  html.write_options = {license_option()};
  html.write = [](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    const ir::Blob& blob = value.get<ir::Blob>();
    const util::EncodingGuess guess = util::guess_encoding(blob.bytes);
    if (!util::looks_like_text(ByteView(blob.bytes).first(std::min<std::size_t>(blob.bytes.size(), 4096)))) {
      return fail(Status::Encode, "the input is not text, so it cannot be HTML");
    }
    const std::string text = util::decode_text(blob.bytes, guess.encoding).text;
    CVT_TRY_ASSIGN(const std::string minified, minify_html(text, {ctx.options.get_bool("keep_license", true)}));
    return to_bytes(minified);
  };
  registry.add_codec(std::move(html));
}

}  // namespace cvt::code
