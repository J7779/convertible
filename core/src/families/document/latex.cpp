// Convertible core - document family: LaTeX (complete article writer, pragmatic reader).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <set>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

constexpr int max_group_depth = 48;

// =========================================================================== reading

std::string strip_comments(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  bool verbatim = false;
  while (i < text.size()) {
    // Comments are kept inside verbatim-like environments.
    for (const std::string_view env : {"verbatim", "lstlisting", "minted", "Verbatim", "comment"}) {
      const std::string begin = "\\begin{" + std::string(env) + "}";
      const std::string end = "\\end{" + std::string(env) + "}";
      if (!verbatim && text.substr(i).starts_with(begin)) {
        const std::size_t stop = text.find(end, i);
        const std::size_t to = stop == std::string_view::npos ? text.size() : stop + end.size();
        if (env == "comment") {
          i = to;
        } else {
          out.append(text.substr(i, to - i));
          i = to;
        }
        verbatim = false;
        break;
      }
    }
    if (i >= text.size()) break;
    const char c = text[i];
    if (c == '\\' && i + 1 < text.size()) {
      out.push_back(c);
      out.push_back(text[i + 1]);
      i += 2;
      continue;
    }
    if (c == '%') {
      // Drop to end of line, and the line break with leading blanks of the next line.
      while (i < text.size() && text[i] != '\n') ++i;
      if (i < text.size()) ++i;
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
      continue;
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

std::string accent(char mark, std::string_view base) {
  // Combining characters for the common accents.
  static const std::map<char, std::string_view> combining = {
      {'\'', "\xCC\x81"}, {'`', "\xCC\x80"}, {'^', "\xCC\x82"}, {'"', "\xCC\x88"}, {'~', "\xCC\x83"},
      {'=', "\xCC\x84"},  {'.', "\xCC\x87"}, {'u', "\xCC\x86"}, {'v', "\xCC\x8C"}, {'H', "\xCC\x8B"},
      {'c', "\xCC\xA7"},  {'k', "\xCC\xA8"}, {'r', "\xCC\x8A"}, {'d', "\xCC\xA3"}, {'b', "\xCC\xB1"},
  };
  std::string b(base);
  if (b == "\\i") b = "\xC4\xB1";
  if (b == "\\j") b = "\xC8\xB7";
  // Precomposed forms for the frequent Latin-1 cases.
  static const std::map<std::string, std::string> precomposed = {
      {"'e", "\xC3\xA9"}, {"`e", "\xC3\xA8"}, {"^e", "\xC3\xAA"}, {"\"e", "\xC3\xAB"}, {"'a", "\xC3\xA1"},
      {"`a", "\xC3\xA0"}, {"^a", "\xC3\xA2"}, {"\"a", "\xC3\xA4"}, {"~a", "\xC3\xA3"}, {"'i", "\xC3\xAD"},
      {"\"i", "\xC3\xAF"}, {"'o", "\xC3\xB3"}, {"^o", "\xC3\xB4"}, {"\"o", "\xC3\xB6"}, {"~o", "\xC3\xB5"},
      {"'u", "\xC3\xBA"}, {"`u", "\xC3\xB9"}, {"^u", "\xC3\xBB"}, {"\"u", "\xC3\xBC"}, {"~n", "\xC3\xB1"},
      {"cc", "\xC3\xA7"}, {"\"A", "\xC3\x84"}, {"\"O", "\xC3\x96"}, {"\"U", "\xC3\x9C"}, {"'E", "\xC3\x89"},
      {"\"\xC4\xB1", "\xC3\xAF"},
  };
  const auto it = precomposed.find(std::string(1, mark) + b);
  if (it != precomposed.end()) return it->second;
  const auto c = combining.find(mark);
  return c == combining.end() ? b : b + std::string(c->second);
}

const std::map<std::string, std::string, std::less<>>& symbols() {
  static const std::map<std::string, std::string, std::less<>> table = {
      {"ldots", "\xE2\x80\xA6"}, {"dots", "\xE2\x80\xA6"}, {"textellipsis", "\xE2\x80\xA6"}, {"LaTeX", "LaTeX"},
      {"TeX", "TeX"}, {"LaTeXe", "LaTeX2e"}, {"textbackslash", "\\"}, {"textasciitilde", "~"},
      {"textasciicircum", "^"}, {"textless", "<"}, {"textgreater", ">"}, {"textbar", "|"},
      {"textquotedbl", "\""}, {"textunderscore", "_"}, {"textendash", "\xE2\x80\x93"}, {"textemdash", "\xE2\x80\x94"},
      {"textquoteleft", "\xE2\x80\x98"}, {"textquoteright", "\xE2\x80\x99"}, {"textquotedblleft", "\xE2\x80\x9C"},
      {"textquotedblright", "\xE2\x80\x9D"}, {"textasciigrave", "`"}, {"textquotesingle", "'"}, {"copyright", "\xC2\xA9"}, {"textcopyright", "\xC2\xA9"},
      {"textregistered", "\xC2\xAE"}, {"texttrademark", "\xE2\x84\xA2"}, {"S", "\xC2\xA7"}, {"P", "\xC2\xB6"},
      {"dag", "\xE2\x80\xA0"}, {"ddag", "\xE2\x80\xA1"}, {"pounds", "\xC2\xA3"}, {"euro", "\xE2\x82\xAC"},
      {"textdegree", "\xC2\xB0"}, {"ss", "\xC3\x9F"}, {"ae", "\xC3\xA6"}, {"AE", "\xC3\x86"}, {"oe", "\xC5\x93"},
      {"OE", "\xC5\x92"}, {"o", "\xC3\xB8"}, {"O", "\xC3\x98"}, {"aa", "\xC3\xA5"}, {"AA", "\xC3\x85"},
      {"l", "\xC5\x82"}, {"L", "\xC5\x81"}, {"i", "\xC4\xB1"}, {"quad", " "}, {"qquad", " "}, {"enspace", " "},
      {"thinspace", "\xE2\x80\x89"}, {"textbullet", "\xE2\x80\xA2"}, {"checkmark", "\xE2\x9C\x93"},
      {"square", "\xE2\x98\x90"}, {"boxtimes", "\xE2\x98\x92"}, {"textsection", "\xC2\xA7"},
  };
  return table;
}

// Commands whose arguments are dropped entirely.
bool dropped_command(std::string_view name) {
  static const std::set<std::string_view> names = {
      "label", "vspace", "hspace", "vskip", "hskip", "setlength", "addtolength", "setcounter", "addtocounter",
      "pagestyle", "thispagestyle", "documentclass", "usepackage", "RequirePackage", "newcommand", "renewcommand",
      "providecommand", "def", "let", "hypersetup", "geometry", "bibliographystyle", "bibliography", "index",
      "noindent", "centering", "raggedright", "raggedleft", "maketitle", "tableofcontents", "listoffigures",
      "listoftables", "clearpage", "cleardoublepage", "smallskip", "medskip", "bigskip", "par", "indent",
      "tightlist", "hline", "toprule", "midrule", "bottomrule", "cline", "cmidrule", "endhead", "endfirsthead",
      "endfoot", "endlastfoot", "noalign", "linewidth", "textwidth", "protect", "nolinkurl", "small", "footnotesize",
      "scriptsize", "tiny", "large", "Large", "LARGE", "huge", "Huge", "normalsize", "selectlanguage",
      "phantomsection", "addcontentsline", "pandocbounded", "includegraphics", "frontmatter", "mainmatter",
      "backmatter", "appendix", "newpage", "pagebreak", "linebreak", "nopagebreak", "allowbreak", "relax",
      "makeatletter", "makeatother", "caption", "captionsetup", "newenvironment", "renewenvironment", "newtheorem",
      "setlist", "lstset", "definecolor", "color", "pagenumbering", "author", "title", "date"};
  return names.contains(name);
}

class TexParser {
 public:
  TexParser(std::string_view text, ir::Document& document) : s_(text), doc_(document) {}

  ir::Blocks parse_body() { return blocks({}, 0); }

  // Reads the preamble commands \title \author \date.
  void read_preamble(std::string_view preamble) {
    for (const std::string_view command : {"title", "author", "date"}) {
      const std::string needle = "\\" + std::string(command) + "{";
      const std::size_t at = preamble.find(needle);
      if (at == std::string_view::npos) continue;
      const std::size_t open = at + needle.size() - 1;
      const std::size_t close = matching_brace(preamble, open);
      if (close == std::string_view::npos) continue;
      TexParser inner(preamble.substr(open + 1, close - open - 1), doc_);
      const std::string value = collapse_whitespace(ir::plain_text(inner.inline_all(0)));
      if (command == "title") doc_.metadata.title = value;
      if (command == "author") doc_.metadata.author = util::replace_all(value, "\\and", ";");
      if (command == "date") doc_.metadata.date = value;
    }
  }

  static std::size_t matching_brace(std::string_view text, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
      if (text[i] == '\\') {
        ++i;
        continue;
      }
      if (text[i] == '{') ++depth;
      if (text[i] == '}' && --depth == 0) return i;
    }
    return std::string_view::npos;
  }

  // matching_brace() over the whole source, answered from a table built once
  // so that many unclosed braces do not make reading quadratic.
  std::size_t source_brace(std::size_t open) {
    constexpr std::size_t unknown = std::string_view::npos - 1;
    if (brace_match_.size() != s_.size()) {
      brace_match_.assign(s_.size(), unknown);
      std::vector<std::size_t> opened;
      for (std::size_t i = 0; i < s_.size(); ++i) {
        if (s_[i] == '\\') {
          ++i;
        } else if (s_[i] == '{') {
          brace_match_[i] = std::string_view::npos;
          opened.push_back(i);
        } else if (s_[i] == '}' && !opened.empty()) {
          brace_match_[opened.back()] = i;
          opened.pop_back();
        }
      }
    }
    if (open >= s_.size()) return std::string_view::npos;
    if (brace_match_[open] == unknown) return matching_brace(s_, open);
    return brace_match_[open];
  }

  ir::Inlines inline_all(int depth) {
    ir::Inlines out;
    while (pos_ < s_.size()) {
      if (inline_step(out, depth, {})) continue;
      // A block construct inside inline content: keep its text.
      const std::size_t before = pos_;
      if (s_[pos_] == '\\' && depth < max_group_depth) {
        const std::string name = command_name();
        if (name == "begin") {
          const std::string env(raw_argument());
          const std::string body = environment_body(env);
          append_text(out, " ");
          append_text(out, ir::plain_text(parse_group_blocks(body, depth)));
        } else if (name != "end" && name != "item" && name != "par") {
          optional_argument();
          for (ir::Inline& k : parse_group_text(raw_argument(), depth)) out.push_back(std::move(k));
        }
      } else {
        append_text(out, " ");
        ++pos_;
      }
      if (pos_ == before) ++pos_;
    }
    return out;
  }

 private:
  // ---- low level ----
  void skip_spaces() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }

  std::string command_name() {
    // at '\'
    std::size_t start = ++pos_;
    if (pos_ < s_.size() && util::is_ascii_alpha(s_[pos_])) {
      while (pos_ < s_.size() && (util::is_ascii_alpha(s_[pos_]) || s_[pos_] == '@')) ++pos_;
      std::string name(s_.substr(start, pos_ - start));
      if (pos_ < s_.size() && s_[pos_] == '*') ++pos_;  // starred variants
      return name;
    }
    if (pos_ < s_.size()) ++pos_;
    return std::string(s_.substr(start, pos_ - start));
  }

  // A {group} or a single token argument, raw.
  std::string_view raw_argument() {
    std::size_t save = pos_;
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n')) ++pos_;
    if (pos_ < s_.size() && s_[pos_] == '{') {
      const std::size_t close = source_brace(pos_);
      if (close == std::string_view::npos) {
        pos_ = s_.size();
        return {};
      }
      const std::string_view inner = s_.substr(pos_ + 1, close - pos_ - 1);
      pos_ = close + 1;
      return inner;
    }
    pos_ = save;
    return {};
  }

  std::string_view optional_argument() {
    const std::size_t save = pos_;
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n')) ++pos_;
    if (pos_ < s_.size() && s_[pos_] == '[') {
      int depth = 0;
      for (std::size_t i = pos_; i < s_.size(); ++i) {
        if (s_[i] == '{') ++depth;
        if (s_[i] == '}') --depth;
        if (s_[i] == ']' && depth == 0) {
          const std::string_view inner = s_.substr(pos_ + 1, i - pos_ - 1);
          pos_ = i + 1;
          return inner;
        }
      }
    }
    pos_ = save;
    return {};
  }

  ir::Inlines parse_group_text(std::string_view text, int depth) {
    if (depth > max_group_depth) return {ir::Text{std::string(text)}};
    TexParser inner(text, doc_);
    inner.notes_ = notes_;
    ir::Inlines out = inner.inline_all(depth + 1);
    notes_ = inner.notes_;
    return out;
  }

  ir::Blocks parse_group_blocks(std::string_view text, int depth) {
    if (depth > max_group_depth) return {ir::Paragraph{{ir::Text{std::string(text)}}}};
    TexParser inner(text, doc_);
    inner.notes_ = notes_;
    ir::Blocks out = inner.blocks({}, depth + 1);
    notes_ = inner.notes_;
    return out;
  }

  // ---- inline ----
  // Consumes one inline item. Returns false at a block boundary (blank line,
  // block command), leaving pos_ there.
  bool inline_step(ir::Inlines& out, int depth, std::string_view stop) {
    const char c = s_[pos_];
    if (!stop.empty() && s_.substr(pos_).starts_with(stop)) return false;
    if (c == '\\') {
      const std::size_t start = pos_;
      std::string name = command_name();
      if (name.size() == 1 && !util::is_ascii_alpha(name[0])) {
        switch (name[0]) {
          case '\\': {
            optional_argument();
            out.push_back(ir::LineBreak{});
            return true;
          }
          case '&': case '%': case '$': case '#': case '_': case '{': case '}': append_text(out, name); return true;
          case ' ': append_text(out, " "); return true;
          case ',': case ';': case ':': case '!': case '/': case '-': case '@': return true;
          case '(': {
            const std::size_t end = s_.find("\\)", pos_);
            ir::Code math{std::string(s_.substr(pos_, (end == std::string_view::npos ? s_.size() : end) - pos_))};
            math.math = ir::MathMode::Inline;
            out.push_back(std::move(math));
            pos_ = end == std::string_view::npos ? s_.size() : end + 2;
            return true;
          }
          case '[': {
            pos_ = start;
            return false;  // display math is a block
          }
          case '\'': case '`': case '^': case '"': case '~': case '=': case '.': {
            std::string_view base = raw_argument();
            if (base.empty() && pos_ < s_.size()) {
              if (s_[pos_] == '\\') {
                const std::size_t b = pos_;
                command_name();
                base = s_.substr(b, pos_ - b);
              } else {
                base = s_.substr(pos_, util::decode_utf8(s_, pos_).length);
                pos_ += base.size();
              }
            }
            append_text(out, accent(name[0], base));
            return true;
          }
          default: append_text(out, name); return true;
        }
      }
      if (name == "par") {
        pos_ = start;
        return false;
      }
      if (is_block_command(name)) {
        pos_ = start;
        return false;
      }
      inline_command(name, out, depth);
      return true;
    }
    if (c == '{') {
      const std::size_t close = source_brace(pos_);
      if (close == std::string_view::npos) {
        ++pos_;
        return true;
      }
      const std::string_view inner = s_.substr(pos_ + 1, close - pos_ - 1);
      pos_ = close + 1;
      // {\bf x} / {\em x} / {\it x} declarations.
      const std::string_view t = util::trim_left(inner);
      for (const auto& [decl, kind] : {std::pair<std::string_view, int>{"\\bf", 1}, {"\\bfseries", 1}, {"\\em", 2},
                                        {"\\it", 2}, {"\\itshape", 2}, {"\\tt", 3}, {"\\ttfamily", 3}}) {
        if (t.starts_with(decl) && (t.size() == decl.size() || !util::is_ascii_alpha(t[decl.size()]))) {
          ir::Inlines kids = parse_group_text(t.substr(decl.size()), depth);
          if (kind == 1) out.push_back(ir::Strong{std::move(kids)});
          else if (kind == 2) out.push_back(ir::Emphasis{std::move(kids)});
          else out.push_back(ir::Code{ir::plain_text(kids)});
          return true;
        }
      }
      for (ir::Inline& k : parse_group_text(inner, depth)) out.push_back(std::move(k));
      return true;
    }
    if (c == '}') {
      ++pos_;
      return true;
    }
    if (c == '$') {
      if (s_.substr(pos_).starts_with("$$")) return false;
      const std::size_t end = s_.find('$', pos_ + 1);
      ir::Code math{std::string(s_.substr(pos_ + 1, (end == std::string_view::npos ? s_.size() : end) - pos_ - 1))};
      math.math = ir::MathMode::Inline;
      out.push_back(std::move(math));
      pos_ = end == std::string_view::npos ? s_.size() : end + 1;
      return true;
    }
    if (c == '~') {
      append_text(out, "\xC2\xA0");
      ++pos_;
      return true;
    }
    if (c == '\n') {
      // A blank line ends the paragraph.
      std::size_t k = pos_ + 1;
      while (k < s_.size() && (s_[k] == ' ' || s_[k] == '\t' || s_[k] == '\r')) ++k;
      if (k < s_.size() && s_[k] == '\n') return false;
      append_text(out, "\n");
      pos_ = k;
      return true;
    }
    if (c == '-' && s_.substr(pos_).starts_with("---")) {
      append_text(out, "\xE2\x80\x94");
      pos_ += 3;
      return true;
    }
    if (c == '-' && s_.substr(pos_).starts_with("--")) {
      append_text(out, "\xE2\x80\x93");
      pos_ += 2;
      return true;
    }
    if (c == '`' && s_.substr(pos_).starts_with("``")) {
      append_text(out, "\xE2\x80\x9C");
      pos_ += 2;
      return true;
    }
    if (c == '\'' && s_.substr(pos_).starts_with("''")) {
      append_text(out, "\xE2\x80\x9D");
      pos_ += 2;
      return true;
    }
    if (c == '&') {
      if (!stop.empty()) return false;
      ++pos_;
      return true;
    }
    // Plain text run.
    std::size_t end = pos_ + 1;
    while (end < s_.size() && std::string_view("\\{}$~\n-`'&").find(s_[end]) == std::string_view::npos) ++end;
    append_text(out, s_.substr(pos_, end - pos_));
    pos_ = end;
    return true;
  }

  static bool is_block_command(std::string_view name) {
    static const std::set<std::string_view> names = {"section", "subsection", "subsubsection", "paragraph",
                                                     "subparagraph", "chapter", "part", "begin", "end", "item",
                                                     "newpage", "clearpage", "pagebreak", "bigskip", "medskip",
                                                     "smallskip", "maketitle", "tableofcontents", "hrule", "rule"};
    return names.contains(name);
  }

  void inline_command(const std::string& name, ir::Inlines& out, int depth) {
    const auto arg = [&] { return parse_group_text(raw_argument(), depth); };
    if (name == "emph" || name == "textit" || name == "textsl" || name == "mathit") {
      out.push_back(ir::Emphasis{arg()});
    } else if (name == "textbf" || name == "mathbf") {
      out.push_back(ir::Strong{arg()});
    } else if (name == "texttt" || name == "verb" || name == "lstinline" || name == "code" || name == "path") {
      if (name == "verb" || name == "lstinline") {
        optional_argument();
        if (pos_ < s_.size()) {
          const char delimiter = s_[pos_] == '{' ? '}' : s_[pos_];
          const std::size_t end = s_.find(delimiter, pos_ + 1);
          out.push_back(ir::Code{std::string(s_.substr(pos_ + 1, (end == std::string_view::npos ? s_.size() : end) - pos_ - 1))});
          pos_ = end == std::string_view::npos ? s_.size() : end + 1;
        }
        return;
      }
      out.push_back(ir::Code{ir::plain_text(arg())});
    } else if (name == "underline" || name == "uline") {
      out.push_back(ir::Underline{arg()});
    } else if (name == "sout" || name == "st" || name == "xout" || name == "strikeout") {
      out.push_back(ir::Strikethrough{arg()});
    } else if (name == "textsuperscript") {
      out.push_back(ir::Superscript{arg()});
    } else if (name == "textsubscript") {
      out.push_back(ir::Subscript{arg()});
    } else if (name == "hl" || name == "colorbox" || name == "highlight") {
      if (name == "colorbox") raw_argument();
      ir::Attributes attr;
      attr.add_class("mark");
      out.push_back(ir::Inline(ir::Strong{arg()}, std::move(attr)));
    } else if (name == "href") {
      const std::string_view raw = raw_argument();
      std::string url;
      for (std::size_t k = 0; k < raw.size(); ++k) {
        if (raw[k] == '\\' && k + 1 < raw.size() && !util::is_ascii_alpha(raw[k + 1])) ++k;
        url.push_back(raw[k]);
      }
      out.push_back(ir::Link{url, {}, arg()});
    } else if (name == "url") {
      std::string url(raw_argument());
      url = util::replace_all(util::replace_all(url, "\\#", "#"), "\\%", "%");
      out.push_back(ir::Link{url, {}, {ir::Text{url}}});
    } else if (name == "footnote") {
      optional_argument();
      const std::string_view body = raw_argument();
      const std::string label = std::to_string(++notes_);
      doc_.footnotes.push_back(ir::Footnote{label, parse_group_blocks(body, depth)});
      ir::Link link;
      link.note = label;
      link.url = "#fn-" + label;
      link.children = {ir::Text{label}};
      out.push_back(std::move(link));
    } else if (name == "includegraphics") {
      const std::string options(optional_argument());
      std::string file(raw_argument());
      std::string alt;
      if (const std::size_t at = options.find("alt={"); at != std::string::npos) {
        const std::size_t close = options.find('}', at);
        alt = collapse_whitespace(options.substr(at + 5, close == std::string::npos ? std::string::npos : close - at - 5));
      }
      out.push_back(ir::InlineImage{wiki::image_from_url(file), alt});
    } else if (name == "cite" || name == "citep" || name == "citet" || name == "autocite" || name == "parencite") {
      optional_argument();
      optional_argument();
      append_text(out, "[" + std::string(raw_argument()) + "]");
    } else if (name == "ref" || name == "eqref" || name == "pageref" || name == "autoref" || name == "cref" || name == "nameref") {
      const std::string target(raw_argument());
      out.push_back(ir::Link{"#" + target, {}, {ir::Text{target}}});
    } else if (name == "pandocbounded" || name == "mbox" || name == "textrm" || name == "textsf" || name == "textnormal" ||
               name == "textup" || name == "textsc" || name == "textmd" || name == "fbox" || name == "hbox" ||
               name == "makebox" || name == "text" || name == "foreignlanguage" || name == "textcolor" || name == "enquote") {
      if (name == "foreignlanguage" || name == "textcolor") raw_argument();
      if (name == "makebox") optional_argument();
      ir::Inlines kids = arg();
      if (name == "enquote") {
        append_text(out, "\xE2\x80\x9C");
        for (ir::Inline& k : kids) out.push_back(std::move(k));
        append_text(out, "\xE2\x80\x9D");
      } else {
        for (ir::Inline& k : kids) out.push_back(std::move(k));
      }
    } else if (name == "newline" || name == "linebreak") {
      out.push_back(ir::LineBreak{});
    } else if (const auto it = symbols().find(name); it != symbols().end()) {
      append_text(out, it->second);
      if (pos_ < s_.size() && s_[pos_] == '{' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '}') pos_ += 2;  // \LaTeX{}
    } else if (name == "def" || name == "let" || name == "gdef" || name == "edef") {
      // \def\name{body} / \let\a\b
      skip_spaces();
      if (pos_ < s_.size() && s_[pos_] == '\\') command_name();
      while (pos_ < s_.size() && s_[pos_] == '#') pos_ += 2;  // parameters #1#2
      if (name == "let") {
        skip_spaces();
        if (pos_ < s_.size() && s_[pos_] == '=') ++pos_;
        skip_spaces();
        if (pos_ < s_.size() && s_[pos_] == '\\') command_name();
      } else {
        raw_argument();
      }
    } else if (dropped_command(name)) {
      optional_argument();
      if (name != "noindent" && name != "centering" && name != "par" && name != "tightlist" && name != "hline" &&
          name != "toprule" && name != "midrule" && name != "bottomrule" && name != "endhead" && name != "maketitle" &&
          name != "protect" && name != "small" && name != "large" && name != "Large" && name != "footnotesize" &&
          name != "relax" && name != "makeatletter" && name != "makeatother" && name != "appendix" &&
          name != "endlastfoot" && name != "endfirsthead" && name != "endfoot" && name != "phantomsection") {
        raw_argument();
        if (name == "setcounter" || name == "setlength" || name == "addtolength" || name == "newcommand" ||
            name == "renewcommand" || name == "definecolor" || name == "addcontentsline" || name == "def") {
          raw_argument();
          if (name == "definecolor" || name == "addcontentsline") raw_argument();
        }
      }
    } else {
      // Unknown command: keep the text of its arguments.
      optional_argument();
      for (int k = 0; k < 2 && pos_ < s_.size() && s_[pos_] == '{'; ++k) {
        for (ir::Inline& x : arg()) out.push_back(std::move(x));
      }
    }
  }

  // ---- blocks ----
  std::string environment_body(const std::string& env) {
    const std::string end = "\\end{" + env + "}";
    // Nested environments of the same name.
    int depth = 1;
    std::size_t i = pos_;
    const std::string begin = "\\begin{" + env + "}";
    while (i < s_.size()) {
      const std::size_t next_end = s_.find(end, i);
      const std::size_t next_begin = s_.find(begin, i);
      if (next_end == std::string_view::npos) {
        std::string body(s_.substr(pos_));
        pos_ = s_.size();
        return body;
      }
      if (next_begin != std::string_view::npos && next_begin < next_end) {
        ++depth;
        i = next_begin + begin.size();
        continue;
      }
      if (--depth == 0) {
        std::string body(s_.substr(pos_, next_end - pos_));
        pos_ = next_end + end.size();
        return body;
      }
      i = next_end + end.size();
    }
    std::string body(s_.substr(pos_));
    pos_ = s_.size();
    return body;
  }

  ir::Blocks blocks(std::string_view /*until*/, int depth) {
    ir::Blocks out;
    ir::Inlines paragraph;
    const auto flush = [&] {
      normalize_inlines(paragraph);
      // Trim surrounding whitespace.
      while (!paragraph.empty()) {
        auto* t = paragraph.front().get_if<ir::Text>();
        if (t == nullptr) break;
        t->text.erase(0, t->text.find_first_not_of(" \t\n"));
        if (!t->text.empty()) break;
        paragraph.erase(paragraph.begin());
      }
      while (!paragraph.empty()) {
        auto* t = paragraph.back().get_if<ir::Text>();
        if (t == nullptr) break;
        while (!t->text.empty() && util::is_ascii_space(t->text.back())) t->text.pop_back();
        if (!t->text.empty()) break;
        paragraph.pop_back();
      }
      if (paragraph.empty()) return;
      if (paragraph.size() == 1 && paragraph.front().is<ir::InlineImage>()) {
        auto* image = paragraph.front().get_if<ir::InlineImage>();
        out.push_back(ir::ImageBlock{std::move(image->source), std::move(image->alt), {}, {}});
      } else {
        out.push_back(ir::Paragraph{std::move(paragraph)});
      }
      paragraph.clear();
    };
    while (pos_ < s_.size()) {
      // A group holding block content ({\def.. \begin{longtable}..}) is transparent.
      if (s_[pos_] == '{') {
        const std::size_t close = source_brace(pos_);
        if (close != std::string_view::npos) {
          const std::string_view inner = s_.substr(pos_ + 1, close - pos_ - 1);
          if (inner.find("\\begin{") != std::string_view::npos || inner.find("\n\n") != std::string_view::npos) {
            ++pos_;
            continue;
          }
        }
      }
      const std::size_t before_step = pos_;
      if (inline_step(paragraph, depth, {})) {
        if (pos_ == before_step) ++pos_;
        continue;
      }
      // A block boundary.
      if (s_[pos_] == '\n') {
        flush();
        skip_spaces();
        continue;
      }
      if (s_.substr(pos_).starts_with("$$") || s_.substr(pos_).starts_with("\\[")) {
        flush();
        const bool dollars = s_[pos_] == '$';
        const std::size_t end = s_.find(dollars ? "$$" : "\\]", pos_ + 2);
        ir::CodeBlock math{{}, std::string(util::trim(s_.substr(pos_ + 2, (end == std::string_view::npos ? s_.size() : end) - pos_ - 2)))};
        math.math = ir::MathMode::Display;
        out.push_back(std::move(math));
        pos_ = end == std::string_view::npos ? s_.size() : end + 2;
        continue;
      }
      const std::size_t start = pos_;
      const std::string name = command_name();
      flush();
      if (name == "section" || name == "subsection" || name == "subsubsection" || name == "paragraph" ||
          name == "subparagraph" || name == "chapter" || name == "part") {
        optional_argument();
        const std::string_view title = raw_argument();
        static const std::map<std::string_view, int> levels = {{"part", 1}, {"chapter", 1}, {"section", 1},
                                                               {"subsection", 2}, {"subsubsection", 3}, {"paragraph", 4},
                                                               {"subparagraph", 5}};
        int level = levels.at(name);
        if (has_chapters_ && name != "chapter" && name != "part") level = std::min(level + 1, 6);
        ir::Attributes attr;
        // \label right after the heading becomes its id.
        const std::size_t save = pos_;
        skip_spaces();
        if (s_.substr(pos_).starts_with("\\label{")) {
          pos_ += 6;
          attr.id = std::string(raw_argument());
        } else {
          pos_ = save;
        }
        out.push_back(ir::Block(ir::Heading{level, parse_group_text(title, depth)}, std::move(attr)));
      } else if (name == "begin") {
        environment(std::string(raw_argument()), out, depth);
      } else if (name == "end") {
        raw_argument();  // stray end
      } else if (name == "item") {
        pos_ = start;
        return out;  // the list parser takes over
      } else if (name == "newpage" || name == "clearpage" || name == "pagebreak") {
        out.push_back(ir::PageBreak{});
      } else if (name == "hrule" || name == "rule") {
        optional_argument();
        raw_argument();
        raw_argument();
        out.push_back(ir::HorizontalRule{});
      } else {
        // maketitle, tableofcontents, skips, \par
        optional_argument();
      }
    }
    flush();
    return out;
  }

  void list(const std::string& env, ir::Blocks& out, int depth) {
    const std::string body = environment_body(env);
    TexParser inner(body, doc_);
    inner.notes_ = notes_;
    inner.has_chapters_ = has_chapters_;
    ir::List list;
    list.ordered = env == "enumerate";
    list.definition = env == "description";
    // Items: split at top-level \item.
    inner.skip_spaces();
    while (inner.pos_ < inner.s_.size()) {
      if (inner.s_.substr(inner.pos_).starts_with("\\item")) {
        inner.pos_ += 5;
        const std::string label(inner.optional_argument());
        ir::Blocks item;
        if (list.definition) item.push_back(ir::Paragraph{parse_group_text(label, depth)});
        ir::TaskState task = ir::TaskState::None;
        if (!list.definition && !label.empty()) {
          if (label.find("square") != std::string::npos) task = ir::TaskState::Open;
          if (label.find("boxtimes") != std::string::npos || label.find("checkmark") != std::string::npos) task = ir::TaskState::Done;
        }
        if (depth < max_group_depth) {
          for (ir::Block& b : inner.blocks({}, depth + 1)) item.push_back(std::move(b));
        }
        list.items.push_back(std::move(item));
        if (task != ir::TaskState::None) {
          list.tasks.resize(list.items.size() - 1, ir::TaskState::None);
          list.tasks.push_back(task);
        }
      } else {
        // Text before the first \item (\tightlist, \def\labelenumi...).
        ir::Blocks ignored = inner.blocks({}, depth + 1);
        if (inner.pos_ < inner.s_.size() && !inner.s_.substr(inner.pos_).starts_with("\\item")) ++inner.pos_;
      }
    }
    notes_ = inner.notes_;
    if (!list.tasks.empty()) list.tasks.resize(list.items.size(), ir::TaskState::None);
    if (!list.items.empty()) out.push_back(std::move(list));
  }

  void table(const std::string& env, ir::Blocks& out, int depth) {
    // Optional position and the column specification.
    optional_argument();
    if (env == "tabularx" || env == "tabular*") raw_argument();
    const std::string spec(raw_argument());
    std::vector<ir::Alignment> aligns;
    for (std::size_t i = 0; i < spec.size(); ++i) {
      const char c = spec[i];
      if (c == 'l') aligns.push_back(ir::Alignment::Left);
      else if (c == 'r') aligns.push_back(ir::Alignment::Right);
      else if (c == 'c') aligns.push_back(ir::Alignment::Center);
      else if (c == 'p' || c == 'X' || c == 'm' || c == 'b') aligns.push_back(ir::Alignment::Left);
      if ((c == 'p' || c == 'm' || c == 'b' || c == '@' || c == '>' || c == '<' || c == '*') && i + 1 < spec.size() && spec[i + 1] == '{') {
        const std::size_t close = matching_brace(spec, i + 1);
        i = close == std::string::npos ? spec.size() : close;
      }
    }
    const std::string body = environment_body(env);
    ir::TableBlock table;
    table.alignments = aligns;
    std::size_t header_rows = 0;
    bool seen_top = false;
    // Rows at top-level "\\", cells at top-level '&'.
    std::vector<std::string> rows;
    std::string current;
    int braces = 0;
    for (std::size_t i = 0; i < body.size(); ++i) {
      const char c = body[i];
      if (c == '\\' && i + 1 < body.size() && body[i + 1] == '\\' && braces == 0) {
        rows.push_back(current);
        current.clear();
        ++i;
        // Skip an optional [length].
        if (i + 1 < body.size() && body[i + 1] == '[') {
          const std::size_t close = body.find(']', i + 1);
          if (close != std::string::npos) i = close;
        }
        continue;
      }
      if (c == '\\' && i + 1 < body.size()) {
        current.push_back(c);
        current.push_back(body[++i]);
        continue;
      }
      if (c == '{') ++braces;
      if (c == '}') --braces;
      current.push_back(c);
    }
    rows.push_back(current);
    for (std::string& row : rows) {
      // Rules and markers.
      std::string_view r = util::trim(row);
      bool rule = false;
      bool head_end = false;
      while (true) {
        bool changed = false;
        for (const std::string_view marker : {"\\hline", "\\toprule", "\\midrule", "\\bottomrule", "\\endhead",
                                               "\\endfirsthead", "\\endfoot", "\\endlastfoot", "\\noalign{}", "\\tightlist"}) {
          if (r.starts_with(marker)) {
            if (marker == "\\midrule" && seen_top) head_end = true;
            if (marker == "\\toprule" || (marker == "\\hline" && table.rows.empty() && !seen_top)) seen_top = true;
            else if (marker == "\\hline" && !table.rows.empty() && header_rows == 0 && table.rows.size() == 1) head_end = true;
            if (marker == "\\endhead" || marker == "\\endfirsthead") head_end = true;
            r = util::trim(r.substr(marker.size()));
            rule = true;
            changed = true;
          }
        }
        if (r.starts_with("\\cline") || r.starts_with("\\cmidrule")) {
          const std::size_t close = r.find('}');
          r = close == std::string_view::npos ? std::string_view{} : util::trim(r.substr(close + 1));
          changed = true;
        }
        if (!changed) break;
      }
      if (head_end && header_rows == 0) header_rows = table.rows.size();
      if (r.empty()) continue;
      (void)rule;
      ir::TableRow out_row;
      int cell_braces = 0;
      std::string cell;
      std::vector<std::string> cells;
      for (std::size_t i = 0; i < r.size(); ++i) {
        if (r[i] == '\\' && i + 1 < r.size()) {
          cell.push_back(r[i]);
          cell.push_back(r[++i]);
          continue;
        }
        if (r[i] == '{') ++cell_braces;
        if (r[i] == '}') --cell_braces;
        if (r[i] == '&' && cell_braces == 0) {
          cells.push_back(cell);
          cell.clear();
          continue;
        }
        cell.push_back(r[i]);
      }
      cells.push_back(cell);
      for (std::string& c : cells) {
        ir::TableCell out_cell;
        std::string_view t = util::trim(c);
        if (t.starts_with("\\multicolumn{")) {
          TexParser p(t, doc_);
          p.pos_ = 12;
          out_cell.colspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(util::trim(p.raw_argument())).value_or(1), 1, 1000));
          p.raw_argument();
          out_cell.content = parse_group_text(p.raw_argument(), depth);
        } else if (t.starts_with("\\multirow{")) {
          TexParser p(t, doc_);
          p.pos_ = 9;
          out_cell.rowspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(util::trim(p.raw_argument())).value_or(1), 1, 1000));
          p.raw_argument();
          out_cell.content = parse_group_text(p.raw_argument(), depth);
        } else {
          out_cell.content = parse_group_text(t, depth);
        }
        normalize_inlines(out_cell.content);
        out_row.cells.push_back(std::move(out_cell));
      }
      table.rows.push_back(std::move(out_row));
    }
    for (std::size_t i = 0; i < header_rows && i < table.rows.size(); ++i) table.rows[i].header = true;
    if (!table.rows.empty()) out.push_back(std::move(table));
  }

  void environment(const std::string& env, ir::Blocks& out, int depth) {
    if (depth > max_group_depth) {
      environment_body(env);
      return;
    }
    if (env == "itemize" || env == "enumerate" || env == "description" || env == "compactitem" || env == "compactenum") {
      list(env == "compactitem" ? "itemize" : env == "compactenum" ? "enumerate" : env, out, depth);
      return;
    }
    if (env == "verbatim" || env == "Verbatim" || env == "lstlisting" || env == "minted" || env == "alltt" || env == "Highlighting") {
      std::string language;
      if (env == "minted") {
        optional_argument();
        language = std::string(raw_argument());
      }
      if (env == "lstlisting" || env == "Verbatim" || env == "Highlighting") {
        const std::string options(optional_argument());
        if (const std::size_t at = options.find("language="); at != std::string::npos) {
          language = options.substr(at + 9, options.find_first_of(",]", at) - at - 9);
          language = util::to_lower(language);
        }
      }
      std::string code = environment_body(env);
      if (code.starts_with("\n")) code.erase(0, 1);
      while (code.ends_with('\n') || code.ends_with(' ')) code.pop_back();
      if (env == "Highlighting" || env == "alltt") {
        // Highlighting macros: keep their text.
        TexParser p(code, doc_);
        std::string flat;
        for (const std::string_view line : util::split(code, '\n')) {
          TexParser lp(line, doc_);
          if (!flat.empty()) flat.push_back('\n');
          flat += ir::plain_text(lp.inline_all(depth + 1));
        }
        code = flat;
      }
      out.push_back(ir::CodeBlock{language, code});
      return;
    }
    if (env == "Shaded" || env == "document" || env == "center" || env == "flushleft" || env == "flushright" ||
        env == "minipage" || env == "landscape" || env == "multicols" || env == "otherlanguage") {
      if (env == "minipage" || env == "multicols" || env == "otherlanguage") {
        optional_argument();
        raw_argument();
      }
      const std::string body = environment_body(env);
      for (ir::Block& b : parse_group_blocks(body, depth)) out.push_back(std::move(b));
      return;
    }
    if (env == "quote" || env == "quotation" || env == "verse" || env == "abstract") {
      const std::string body = environment_body(env);
      ir::Blocks inner = parse_group_blocks(body, depth);
      if (env == "abstract") {
        doc_.metadata.extra["description"] = ir::plain_text(inner);
        for (ir::Block& b : inner) out.push_back(std::move(b));
        return;
      }
      out.push_back(ir::BlockQuote{std::move(inner)});
      return;
    }
    if (env == "equation" || env == "equation*" || env == "align" || env == "align*" || env == "displaymath" ||
        env == "gather" || env == "gather*" || env == "multline" || env == "eqnarray" || env == "math") {
      ir::CodeBlock math{{}, std::string(util::trim(environment_body(env)))};
      math.math = ir::MathMode::Display;
      out.push_back(std::move(math));
      return;
    }
    if (env == "tabular" || env == "tabularx" || env == "longtable" || env == "tabular*" || env == "array" || env == "tabu") {
      table(env, out, depth);
      return;
    }
    if (env == "table" || env == "figure" || env == "table*" || env == "figure*") {
      optional_argument();
      const std::string body = environment_body(env);
      ir::Blocks inner = parse_group_blocks(body, depth);
      // \caption{...}
      std::string caption;
      const std::size_t at = body.find("\\caption{");
      if (at != std::string::npos) {
        const std::size_t close = matching_brace(body, at + 8);
        if (close != std::string::npos) caption = collapse_whitespace(ir::plain_text(parse_group_text(std::string_view(body).substr(at + 9, close - at - 9), depth)));
      }
      for (ir::Block& b : inner) {
        if (auto* image = b.get_if<ir::ImageBlock>(); image != nullptr && image->caption.empty()) image->caption = caption;
        if (auto* t = b.get_if<ir::TableBlock>(); t != nullptr && t->caption.empty() && !caption.empty()) t->caption = {ir::Text{caption}};
        if (auto* p = b.get_if<ir::Paragraph>(); p != nullptr && p->content.size() == 1 && p->content.front().is<ir::InlineImage>()) {
          auto* image = p->content.front().get_if<ir::InlineImage>();
          b = ir::ImageBlock{std::move(image->source), std::move(image->alt), caption, {}};
        }
        out.push_back(std::move(b));
      }
      return;
    }
    if (env == "comment") {
      environment_body(env);
      return;
    }
    // Unknown environments: their content.
    const std::string body = environment_body(env);
    for (ir::Block& b : parse_group_blocks(body, depth)) out.push_back(std::move(b));
  }

  std::string_view s_;
  std::vector<std::size_t> brace_match_;
  std::size_t pos_ = 0;
  ir::Document& doc_;
  int notes_ = 0;

 public:
  bool has_chapters_ = false;
};

ir::Document read_latex(std::string_view text) {
  ir::Document document;
  const std::string clean = strip_comments(text);
  std::string_view body = clean;
  const std::size_t begin = body.find("\\begin{document}");
  if (begin != std::string_view::npos) {
    TexParser preamble_parser(body.substr(0, begin), document);
    preamble_parser.read_preamble(body.substr(0, begin));
    body = body.substr(begin + 16);
    const std::size_t end = body.rfind("\\end{document}");
    if (end != std::string_view::npos) body = body.substr(0, end);
  } else {
    TexParser preamble_parser(body, document);
    preamble_parser.read_preamble(body);
  }
  TexParser parser(body, document);
  parser.has_chapters_ = body.find("\\chapter") != std::string_view::npos;
  document.blocks = parser.parse_body();
  if (!document.footnotes.empty()) number_footnotes(document);
  return document;
}

// =========================================================================== writing

std::string tex_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 16);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    switch (c) {
      case '\\': out.append("\\textbackslash{}"); break;
      case '{': out.append("\\{"); break;
      case '}': out.append("\\}"); break;
      case '#': out.append("\\#"); break;
      case '$': out.append("\\$"); break;
      case '%': out.append("\\%"); break;
      case '&': out.append("\\&"); break;
      case '_': out.append("\\_"); break;
      case '~': out.append("\\textasciitilde{}"); break;
      case '^': out.append("\\textasciicircum{}"); break;
      case '<': out.append("\\textless{}"); break;
      case '>': out.append("\\textgreater{}"); break;
      case '|': out.append("\\textbar{}"); break;
      case '-': out.append(i + 1 < text.size() && text[i + 1] == '-' ? "-{}" : "-"); break;
      case '`': out.append("\\textasciigrave{}"); break;
      case '\'': out.append(i + 1 < text.size() && text[i + 1] == '\'' ? "'{}" : "'"); break;
      case '"': out.append("\\textquotedbl{}"); break;
      case '\r': break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') break;
        out.push_back(c);
    }
  }
  // "\xC2\xA0" (no-break space) as a tie.
  return util::replace_all(out, "\xC2\xA0", "~");
}

std::string tex_url(std::string_view url) {
  std::string out;
  for (const char c : url) {
    if (c == '#' || c == '%' || c == '\\' || c == '{' || c == '}' || c == '~' || c == '^' || c == '&' || c == '_' || c == '$') {
      if (c == '\\' || c == '{' || c == '}') {
        out.append("%").append(c == '\\' ? "5C" : c == '{' ? "7B" : "7D");
        continue;
      }
      out.push_back('\\');
    }
    if (static_cast<unsigned char>(c) < 0x20) continue;
    out.push_back(c);
  }
  return out;
}

bool listings_language(std::string_view language, std::string& out) {
  static const std::map<std::string_view, std::string_view> known = {
      {"python", "Python"}, {"py", "Python"}, {"c", "C"}, {"cpp", "C++"}, {"c++", "C++"}, {"java", "Java"},
      {"bash", "bash"}, {"sh", "sh"}, {"shell", "bash"}, {"html", "HTML"}, {"xml", "XML"}, {"sql", "SQL"},
      {"ruby", "Ruby"}, {"perl", "Perl"}, {"php", "PHP"}, {"tex", "TeX"}, {"latex", "TeX"}, {"haskell", "Haskell"},
      {"lisp", "Lisp"}, {"go", "Go"}, {"r", "R"}, {"matlab", "Matlab"}, {"fortran", "Fortran"}, {"pascal", "Pascal"},
      {"make", "make"}, {"lua", "Lua"}, {"scala", "Scala"}, {"erlang", "erlang"}, {"ml", "ML"}, {"ocaml", "ML"}};
  const auto it = known.find(lower(language));
  if (it == known.end()) return false;
  out = std::string(it->second);
  return true;
}

class TexWriter {
 public:
  TexWriter(const ir::Document& document, bool standalone, bool toc, int wrap)
      : doc_(document), standalone_(standalone), toc_(toc), wrap_(wrap) {
    m_.escape = tex_escape;
    m_.strong = {"\\textbf{", "}"};
    m_.emphasis = {"\\emph{", "}"};
    m_.underline = {"\\uline{", "}"};
    m_.strikethrough = {"\\sout{", "}"};
    m_.superscript = {"\\textsuperscript{", "}"};
    m_.subscript = {"\\textsubscript{", "}"};
    m_.mark = {"\\colorbox{yellow}{", "}"};
    m_.line_break = "\\\\\n";
    m_.code = [](const ir::Code& code) -> std::string {
      if (code.math == ir::MathMode::Inline) return "\\(" + code.text + "\\)";
      if (code.math == ir::MathMode::Display) return "\\[" + code.text + "\\]";
      if (code.raw_format == "latex" || code.raw_format == "tex") return code.text;
      if (!code.raw_format.empty()) return {};
      std::string text = code.text;
      for (char& c : text) {
        if (c == '\n') c = ' ';
      }
      return "\\texttt{" + tex_escape(text) + "}";
    };
    m_.link = [](const ir::Link& link, const std::string& label) -> std::string {
      if (link.url.starts_with("#")) {
        std::string target = link.url.substr(1);
        std::erase_if(target, [](char c) { return c == '{' || c == '}' || c == '\\' || c == '#' || c == '%'; });
        return "\\hyperref[" + target + "]{" + label + "}";
      }
      if (label == tex_escape(link.url)) return "\\url{" + tex_url(link.url) + "}";
      return "\\href{" + tex_url(link.url) + "}{" + label + "}";
    };
    m_.image = [this](const ir::InlineImage& image) { return image_command(image.source, image.alt, false); };
    m_.note = [this](const ir::Link&, std::size_t index) -> std::string {
      if (index == 0 || index > doc_.footnotes.size()) return {};
      std::string body = blocks(doc_.footnotes[index - 1].blocks, 1);
      while (body.ends_with('\n')) body.pop_back();
      return "\\footnote{" + util::replace_all(body, "\n\n", "\\par ") + "}";
    };
  }

  std::string run() {
    std::string body = blocks(doc_.blocks, 0);
    if (!standalone_) return body;
    std::string out = "\\documentclass[11pt]{article}\n";
    out += "\\usepackage{iftex}\n";
    out += "\\ifPDFTeX\n  \\usepackage[T1]{fontenc}\n  \\usepackage[utf8]{inputenc}\n  \\usepackage{lmodern}\n";
    // Characters pdfLaTeX cannot typeset with the default fonts get a visible stand-in.
    for (const char32_t cp : unsupported_) {
      char hex[16];
      std::snprintf(hex, sizeof hex, "%04X", static_cast<unsigned>(cp));
      out += "  \\DeclareUnicodeCharacter{" + std::string(hex) + "}{\\textbf{?}}\n";
    }
    out += "\\else\n  \\usepackage{fontspec}\n\\fi\n";
    out += "\\usepackage{amsmath,amssymb}\n\\usepackage{graphicx}\n\\usepackage{longtable,booktabs,array}\n";
    out += "\\usepackage{multirow}\n\\usepackage{listings}\n\\usepackage[normalem]{ulem}\n\\usepackage{xcolor}\n";
    out += "\\usepackage[hidelinks]{hyperref}\n";
    out += "\\lstset{basicstyle=\\ttfamily\\small,breaklines=true,columns=fullflexible,keepspaces=true}\n";
    out += "\\setlength{\\parindent}{0pt}\n\\setlength{\\parskip}{6pt plus 2pt minus 1pt}\n";
    const ir::DocumentMetadata& meta = doc_.metadata;
    if (!meta.title.empty()) {
      out += "\\title{" + tex_escape(meta.title) + "}\n";
      out += "\\author{" + tex_escape(meta.author) + "}\n";
      // An explicit date: never the compilation date.
      out += "\\date{" + tex_escape(meta.date) + "}\n";
    }
    out += "\n\\begin{document}\n";
    if (!meta.title.empty()) out += "\\maketitle\n";
    if (toc_) out += "\\tableofcontents\n";
    out += "\n" + body + "\n\\end{document}\n";
    return out;
  }

 private:
  void note_unicode(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
      const util::Utf8Decoded d = util::decode_utf8(text, i);
      i += std::max<std::size_t>(d.length, 1);
      const char32_t cp = d.code_point;
      // Latin-1 and Latin Extended-A are covered by T1 + inputenc.
      if (cp >= 0x80 && !(cp >= 0xA0 && cp <= 0x17F) && cp != 0x2013 && cp != 0x2014 && cp != 0x2018 && cp != 0x2019 &&
          cp != 0x201C && cp != 0x201D && cp != 0x2026 && cp != 0x20AC && cp != 0x2022 && unsupported_.size() < 2000) {
        unsupported_.insert(cp);
      }
    }
  }

  std::string inlines(const ir::Inlines& content) {
    std::string out = render_inlines(content, m_, &doc_);
    note_unicode(out);
    return out;
  }

  std::string image_command(const ir::ImageSource& source, std::string_view alt, bool block) {
    const std::string url = image_url(source, false);
    const std::string fallback = "\\fbox{" + tex_escape(alt.empty() ? std::string_view("image") : alt) + "}";
    note_unicode(alt);
    if (url.empty() || url.find("://") != std::string::npos) {
      // Embedded pictures cannot live in a .tex file; remote ones are not fetched.
      return fallback;
    }
    std::string file;
    for (const char c : url) {
      if (c != '{' && c != '}' && c != '\\' && c != '%' && c != '#') file.push_back(c);
    }
    const std::string size = block ? "width=0.8\\linewidth,keepaspectratio" : "height=1em";
    return "\\IfFileExists{" + file + "}{\\includegraphics[" + size + "]{" + file + "}}{" + fallback + "}";
  }

  std::string blocks(const ir::Blocks& list, int depth) {
    std::string out;
    for (const ir::Block& b : list) {
      std::string text = block(b, depth);
      if (text.empty()) continue;
      if (!out.empty()) out.append("\n\n");
      out.append(text);
    }
    return out;
  }

  std::string paragraph(const ir::Inlines& content) {
    std::string text = inlines(content);
    if (wrap_ > 0) text = wrap_text(text, static_cast<std::size_t>(wrap_));
    // A blank line inside would end the paragraph.
    while (text.find("\n\n") != std::string::npos) text = util::replace_all(text, "\n\n", "\n");
    return text;
  }

  std::string block(const ir::Block& block, int depth) {
    if (depth > ir::max_document_depth * 2) return {};
    std::string out;
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Paragraph>) {
            out = paragraph(node.content);
          } else if constexpr (std::same_as<T, ir::Heading>) {
            if (block.attr.has_class("toc-title")) return;
            static constexpr std::array<std::string_view, 6> commands = {"section", "subsection", "subsubsection",
                                                                         "paragraph", "subparagraph", "subparagraph"};
            std::string text = inlines(node.content);
            for (char& c : text) {
              if (c == '\n') c = ' ';
            }
            out = "\\" + std::string(commands[static_cast<std::size_t>(std::clamp(node.level, 1, 6) - 1)]) + "{" + text + "}";
            if (!block.attr.id.empty()) {
              std::string id = block.attr.id;
              std::erase_if(id, [](char c) { return c == '{' || c == '}' || c == '\\' || c == '#' || c == '%'; });
              out += "\\label{" + id + "}";
            }
          } else if constexpr (std::same_as<T, ir::List>) {
            if (block.attr.has_class("toc")) return;  // \tableofcontents replaces the generated list
            out = list(node, depth);
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            out = code_block(node);
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            std::string inner = blocks(node.blocks, depth + 1);
            if (block.attr.has_class(ir::conventions::admonition)) {
              std::string kind = "Note";
              for (const std::string& c : block.attr.classes) {
                if (c != ir::conventions::admonition && !c.empty()) kind = std::string(1, util::ascii_upper(c[0])) + c.substr(1);
              }
              inner = "\\textbf{" + tex_escape(kind) + ":} " + inner;
            }
            out = "\\begin{quote}\n" + inner + "\n\\end{quote}";
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            out = table(node);
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            out = "\\begin{figure}[htbp]\n\\centering\n" + image_command(node.source, node.alt, true);
            if (!node.caption.empty()) out += "\n\\caption{" + tex_escape(node.caption) + "}";
            note_unicode(node.caption);
            out += "\n\\end{figure}";
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            out = "\\noindent\\rule{\\linewidth}{0.4pt}";
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            out = "\\newpage";
          }
        },
        block.node);
    return out;
  }

  std::string code_block(const ir::CodeBlock& code) {
    if (code.math != ir::MathMode::None) return "\\[\n" + code.code + "\n\\]";
    if (!code.raw_format.empty()) return code.raw_format == "latex" || code.raw_format == "tex" ? code.code : std::string();
    const bool ascii = std::ranges::all_of(code.code, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    std::string language;
    if (ascii && code.code.find("\\end{lstlisting}") == std::string::npos) {
      const bool known = listings_language(code.language, language);
      return std::string("\\begin{lstlisting}") + (known ? "[language=" + language + "]" : "") + "\n" + code.code + "\n\\end{lstlisting}";
    }
    if (code.code.find("\\end{verbatim}") == std::string::npos) {
      note_unicode(code.code);
      return "\\begin{verbatim}\n" + code.code + "\n\\end{verbatim}";
    }
    // Neither environment can hold the text: typewriter lines.
    std::string out = "{\\ttfamily\\obeyspaces\n";
    for (const std::string_view line : util::split(code.code, '\n')) out += tex_escape(line) + "\\\\\n";
    note_unicode(code.code);
    return out + "}";
  }

  std::string list(const ir::List& list, int depth) {
    if (list.items.empty()) return {};
    std::string out;
    if (list.definition) {
      out = "\\begin{description}\n";
      for (const ir::Blocks& item : list.items) {
        std::string term;
        if (!item.empty()) {
          if (const auto* p = item.front().get_if<ir::Paragraph>()) term = inlines(p->content);
        }
        for (char& c : term) {
          if (c == '\n') c = ' ';
        }
        ir::Blocks rest(item.begin() + std::min<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(item.size())), item.end());
        out += "\\item[{" + term + "}] " + blocks(rest, depth + 1) + "\n";
      }
      return out + "\\end{description}";
    }
    const std::string env = list.ordered ? "enumerate" : "itemize";
    out = "\\begin{" + env + "}\n";
    if (list.ordered && list.start != 1) out += "\\setcounter{enumi}{" + std::to_string(list.start - 1) + "}\n";
    for (std::size_t i = 0; i < list.items.size(); ++i) {
      std::string marker = "\\item";
      if (list.task(i) == ir::TaskState::Done) marker = "\\item[$\\boxtimes$]";
      if (list.task(i) == ir::TaskState::Open) marker = "\\item[$\\square$]";
      out += marker + " " + blocks(list.items[i], depth + 1) + "\n";
    }
    return out + "\\end{" + env + "}";
  }

  std::string table(const ir::TableBlock& table) {
    const TableGrid grid = table_grid(table);
    if (grid.columns == 0) return {};
    std::string spec;
    for (std::size_t c = 0; c < grid.columns; ++c) {
      switch (table.column_alignment(c)) {
        case ir::Alignment::Right: spec.push_back('r'); break;
        case ir::Alignment::Center: spec.push_back('c'); break;
        default: spec.push_back('l'); break;
      }
    }
    std::string out = "\\begin{longtable}[]{@{}" + spec + "@{}}\n";
    if (!table.caption.empty()) out += "\\caption{" + inlines(table.caption) + "}\\\\\n";
    out += "\\toprule\n";
    const std::size_t header = header_row_count(table);
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
      std::string row;
      std::size_t column = 0;
      bool first = true;
      // Covered slots of row spans get empty cells.
      for (std::size_t c = 0; c < grid.columns;) {
        const ir::TableCell* cell = grid.cells[r][c];
        if (!first) row += " & ";
        first = false;
        if (cell == nullptr) {
          ++c;
          continue;
        }
        if (!grid.origin[r][c]) {
          // Part of a span from above (or from the left, which multicolumn covers).
          std::size_t width = 1;
          while (c + width < grid.columns && grid.cells[r][c + width] == cell) ++width;
          if (width > 1) row += "\\multicolumn{" + std::to_string(width) + "}{l}{}";
          c += width;
          continue;
        }
        std::string content = inlines(cell->content);
        for (char& ch : content) {
          if (ch == '\n') ch = ' ';
        }
        content = util::replace_all(content, "\\\\\n", " \\newline ");
        if (cell->rowspan > 1) content = "\\multirow{" + std::to_string(cell->rowspan) + "}{*}{" + content + "}";
        if (cell->colspan > 1) {
          const char a = spec[std::min(c, spec.size() - 1)];
          content = "\\multicolumn{" + std::to_string(cell->colspan) + "}{" + std::string(1, a) + "}{" + content + "}";
        }
        row += content;
        c += static_cast<std::size_t>(std::max(cell->colspan, 1));
        (void)column;
      }
      out += row + " \\\\\n";
      if (header > 0 && r + 1 == header) out += "\\midrule\n\\endhead\n";
    }
    out += "\\bottomrule\n\\end{longtable}";
    return out;
  }

  const ir::Document& doc_;
  bool standalone_;
  bool toc_;
  int wrap_;
  InlineMarkup m_;
  std::set<char32_t> unsupported_;
};

int sniff_latex(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"tex", "latex", "ltx"})) return 80;
  const std::string_view head = sniff_head(data);
  if (head.find("\\documentclass") != std::string_view::npos || head.find("\\begin{document}") != std::string_view::npos) return 88;
  int score = 0;
  for (const std::string_view command : {"\\section{", "\\begin{", "\\end{", "\\textbf{", "\\emph{", "\\usepackage"}) {
    if (head.find(command) != std::string_view::npos) ++score;
  }
  return score >= 4 ? 66 : 0;
}

}  // namespace

void register_latex(Registry& registry) {
  MarkupFormat tex;
  tex.id = "tex";
  tex.name = "LaTeX source";
  tex.description = "LaTeX document: written as a complete, compilable article; read for its structure and text.";
  tex.extensions = {"tex", "latex", "ltx"};
  tex.mime = "application/x-tex";
  tex.popularity = 40;
  tex.sniff = sniff_latex;
  tex.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_latex(text); };
  tex.read_lossy = true;  // macros are not expanded
  tex.read_cost = 20;
  tex.write = [](const ir::Document& document, WriteContext& ctx) -> Result<std::string> {
    // The contents list of the shared toc option is replaced by \tableofcontents.
    const bool toc = ctx.options.get_bool("toc", false);
    for (const ir::Block& b : document.blocks) {
      if (const auto* image = b.get_if<ir::ImageBlock>()) {
        if (const auto* e = std::get_if<ir::EncodedImage>(&image->source); e == nullptr || e->url.empty()) {
          ctx.warn("Embedded pictures cannot be stored in a LaTeX file; a framed placeholder with the alternative text was written.");
          break;
        }
      }
    }
    TexWriter writer(document, ctx.options.get_bool("standalone", true), toc,
                     static_cast<int>(ctx.options.get_int("wrap_width", 0, 0, 1000)));
    return writer.run();
  };
  tex.features = feature_standalone | feature_wrap;
  register_markup(registry, std::move(tex));
}

}  // namespace cvt::document
