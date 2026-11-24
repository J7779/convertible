// Convertible core - document family: roff manual pages (man and mdoc macros).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/html_internal.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

// ---- roff escapes ---------------------------------------------------------------------------------

const std::map<std::string, std::string, std::less<>>& special_characters() {
  static const std::map<std::string, std::string, std::less<>> table = {
      {"bu", "\xE2\x80\xA2"}, {"em", "\xE2\x80\x94"}, {"en", "\xE2\x80\x93"}, {"hy", "-"}, {"mi", "-"},
      {"dq", "\""}, {"lq", "\xE2\x80\x9C"}, {"rq", "\xE2\x80\x9D"}, {"oq", "\xE2\x80\x98"}, {"cq", "\xE2\x80\x99"},
      {"aq", "'"}, {"ga", "`"}, {"ha", "^"}, {"ti", "~"}, {"rs", "\\"}, {"sl", "/"}, {"co", "\xC2\xA9"},
      {"rg", "\xC2\xAE"}, {"tm", "\xE2\x84\xA2"}, {"de", "\xC2\xB0"}, {"+-", "\xC2\xB1"}, {"mu", "\xC3\x97"},
      {"di", "\xC3\xB7"}, {"->", "\xE2\x86\x92"}, {"<-", "\xE2\x86\x90"}, {"<=", "\xE2\x89\xA4"}, {">=", "\xE2\x89\xA5"},
      {"!=", "\xE2\x89\xA0"}, {"Fo", "\xC2\xAB"}, {"Fc", "\xC2\xBB"}, {"lB", "["}, {"rB", "]"}, {"lC", "{"},
      {"rC", "}"}, {"ba", "|"}, {"or", "|"}, {"at", "@"}, {"sh", "#"}, {"Do", "$"}, {"ul", "_"}, {"ru", "_"},
      {"pl", "+"}, {"eq", "="}, {"ss", "\xC3\x9F"}, {"ae", "\xC3\xA6"}, {"oe", "\xC5\x93"}, {":a", "\xC3\xA4"},
      {":o", "\xC3\xB6"}, {":u", "\xC3\xBC"}, {"'e", "\xC3\xA9"}, {"`e", "\xC3\xA8"}, {"^e", "\xC3\xAA"},
      {"sq", "\xE2\x96\xA1"}, {"OK", "\xE2\x9C\x93"}, {"Am", "&"}, {"lt", "<"}, {"gt", ">"},
  };
  return table;
}

enum class Font : std::uint8_t { Roman, Bold, Italic, Mono };

struct Span {
  std::string text;
  Font font = Font::Roman;
};

// Decodes roff escapes into spans of text with fonts; `font` carries over lines.
void decode_line(std::string_view line, Font& font, std::vector<Span>& out, Font& previous) {
  std::string text;
  const auto flush = [&] {
    if (!text.empty()) out.push_back({std::move(text), font});
    text.clear();
  };
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c != '\\' || i + 1 >= line.size()) {
      text.push_back(c);
      continue;
    }
    const char e = line[++i];
    switch (e) {
      case 'f': {
        std::string name;
        if (i + 1 < line.size() && line[i + 1] == '[') {
          const std::size_t close = line.find(']', i + 2);
          name = std::string(line.substr(i + 2, (close == std::string_view::npos ? line.size() : close) - i - 2));
          i = close == std::string_view::npos ? line.size() : close;
        } else if (i + 1 < line.size() && line[i + 1] == '(') {
          name = std::string(line.substr(i + 2, 2));
          i += 3;
        } else if (i + 1 < line.size()) {
          name = std::string(1, line[i + 1]);
          i += 1;
        }
        flush();
        Font next = Font::Roman;
        if (name == "B" || name == "3" || name == "CB" || name == "BI") next = Font::Bold;
        else if (name == "I" || name == "2" || name == "CI") next = Font::Italic;
        else if (name == "CW" || name == "CR" || name == "C" || name == "CB") next = Font::Mono;
        else if (name == "P" || name.empty()) next = previous;
        previous = font;
        font = next;
        break;
      }
      case '(': {
        const std::string name(line.substr(i + 1, 2));
        const auto it = special_characters().find(name);
        text += it != special_characters().end() ? it->second : std::string();
        i += 2;
        break;
      }
      case '[': {
        const std::size_t close = line.find(']', i + 1);
        const std::string name(line.substr(i + 1, (close == std::string_view::npos ? line.size() : close) - i - 1));
        if (name.starts_with("u") && name.size() >= 5) {
          if (const auto cp = util::parse_int(name.substr(1), 16)) util::append_utf8(text, static_cast<char32_t>(*cp));
        } else if (const auto it = special_characters().find(name); it != special_characters().end()) {
          text += it->second;
        }
        i = close == std::string_view::npos ? line.size() : close;
        break;
      }
      case '*': {
        // Strings: \*(xx, \*[name], \*x
        if (i + 1 < line.size() && line[i + 1] == '(') {
          const std::string name(line.substr(i + 2, 2));
          if (name == "lq") text += "\xE2\x80\x9C";
          if (name == "rq") text += "\xE2\x80\x9D";
          if (name == "R") text += "\xC2\xAE";
          if (name == "Tm") text += "\xE2\x84\xA2";
          i += 3;
        } else if (i + 1 < line.size() && line[i + 1] == '[') {
          const std::size_t close = line.find(']', i + 2);
          i = close == std::string_view::npos ? line.size() : close;
        } else {
          ++i;
        }
        break;
      }
      case 'e': case '\\': text.push_back('\\'); break;
      case '-': text.push_back('-'); break;
      case '&': case ')': case '%': case 'c': case '/': case ',': case 'd': case 'u': break;
      case ' ': text.push_back(' '); break;
      case '~': case '0': text.append("\xC2\xA0"); break;
      case '|': case '^': break;
      case '.': text.push_back('.'); break;
      case '\'': text.push_back('\''); break;
      case '`': text.push_back('`'); break;
      case '"': i = line.size(); break;  // comment
      case 's': {
        // \s+1, \s-1, \s0, \s(12, \s[12]
        std::size_t k = i + 1;
        if (k < line.size() && (line[k] == '+' || line[k] == '-')) ++k;
        if (k < line.size() && line[k] == '(') k += 3;
        else if (k < line.size() && line[k] == '[') k = line.find(']', k) == std::string_view::npos ? line.size() : line.find(']', k) + 1;
        else if (k < line.size() && util::is_ascii_digit(line[k])) ++k;
        i = k - 1;
        break;
      }
      case 'm': case 'M': case 'F': case 'k': case 'n': case 'V': case 'Y': case 'g': case 'w': case 'h': case 'v': case 'l': case 'L': case 'x': case 'X': case 'D': case 'H': case 'S': case 'o': case 'b': case 'z': case 'Z': case 'A': case 'B': case 'N': case 'R': {
        // Escapes with an argument: skip it.
        if (i + 1 < line.size()) {
          const char d = line[i + 1];
          if (d == '[') {
            const std::size_t close = line.find(']', i + 2);
            i = close == std::string_view::npos ? line.size() : close;
          } else if (d == '(') {
            i += 3;
          } else if (d == '\'') {
            const std::size_t close = line.find('\'', i + 2);
            i = close == std::string_view::npos ? line.size() : close;
          } else {
            ++i;
          }
        }
        break;
      }
      default: text.push_back(e); break;
    }
  }
  flush();
}

ir::Inlines spans_to_inlines(const std::vector<Span>& spans) {
  ir::Inlines out;
  for (const Span& s : spans) {
    switch (s.font) {
      case Font::Bold: out.push_back(ir::Strong{{ir::Text{s.text}}}); break;
      case Font::Italic: out.push_back(ir::Emphasis{{ir::Text{s.text}}}); break;
      case Font::Mono: out.push_back(ir::Code{s.text}); break;
      case Font::Roman: append_text(out, s.text); break;
    }
  }
  // Merge adjacent equal formatting split over lines.
  ir::Inlines merged;
  for (ir::Inline& item : out) {
    if (!merged.empty()) {
      if (auto* a = merged.back().get_if<ir::Strong>(); a != nullptr && item.is<ir::Strong>()) {
        for (ir::Inline& k : item.get_if<ir::Strong>()->children) a->children.push_back(std::move(k));
        continue;
      }
      if (auto* a = merged.back().get_if<ir::Emphasis>(); a != nullptr && item.is<ir::Emphasis>()) {
        for (ir::Inline& k : item.get_if<ir::Emphasis>()->children) a->children.push_back(std::move(k));
        continue;
      }
      if (auto* a = merged.back().get_if<ir::Code>(); a != nullptr && item.is<ir::Code>()) {
        a->text += item.get_if<ir::Code>()->text;
        continue;
      }
    }
    merged.push_back(std::move(item));
  }
  normalize_inlines(merged);
  return merged;
}

// Splits macro arguments, honouring double quotes.
std::vector<std::string> macro_arguments(std::string_view text) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= text.size()) break;
    std::string arg;
    if (text[i] == '"') {
      ++i;
      while (i < text.size()) {
        if (text[i] == '"' && i + 1 < text.size() && text[i + 1] == '"') {
          arg.push_back('"');
          i += 2;
          continue;
        }
        if (text[i] == '"') {
          ++i;
          break;
        }
        arg.push_back(text[i++]);
      }
    } else {
      while (i < text.size() && text[i] != ' ' && text[i] != '\t') {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == ' ') {
          arg.append("\\ ");
          i += 2;
          continue;
        }
        arg.push_back(text[i++]);
      }
    }
    out.push_back(std::move(arg));
  }
  return out;
}

// ---- the page reader --------------------------------------------------------------------------------

class ManReader {
 public:
  explicit ManReader(ir::Document& document) : doc_(document) {}

  void read(std::string_view text) {
    const std::vector<std::string_view> lines = util::split_lines(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      std::string_view line = lines[i];
      if (line.starts_with(".\\\"") || line.starts_with("'\\\"") || line.starts_with("\\\"") || line == "." ||
          line.starts_with(".\\#")) {
        continue;
      }
      if ((line.starts_with(".") || line.starts_with("'")) && line.size() > 1) {
        std::size_t name_end = 1;
        while (name_end < line.size() && line[name_end] == ' ') ++name_end;
        const std::size_t name_start = name_end;
        while (name_end < line.size() && line[name_end] != ' ' && line[name_end] != '\t') ++name_end;
        const std::string name(line.substr(name_start, name_end - name_start));
        const std::string_view rest = util::trim(line.substr(name_end));
        if (name == "TS") {
          std::vector<std::string_view> table_lines;
          ++i;
          while (i < lines.size() && util::trim(lines[i]) != ".TE") table_lines.push_back(lines[i++]);
          flush_paragraph();
          tbl(table_lines);
          continue;
        }
        if (verbatim_ && !(name == "fi" || name == "EE" || name == "Ed" || name == "br" || name == "sp")) {
          // Font macros inside examples still apply; others are ignored.
          if (name == "B" || name == "I" || name == "BR" || name == "IR") {
            std::string joined;
            for (const std::string& a : macro_arguments(rest)) joined += a;
            add_verbatim_line(joined);
          }
          continue;
        }
        macro(name, rest, lines, i);
        continue;
      }
      if (verbatim_) {
        add_verbatim_line(line);
        continue;
      }
      if (util::trim(line).empty()) {
        flush_paragraph();
        continue;
      }
      if (pending_tag_) {
        // .TP: this line is the term.
        pending_tag_ = false;
        Font font = Font::Roman;
        Font previous = Font::Roman;
        std::vector<Span> spans;
        decode_line(line, font, spans, previous);
        start_item(spans_to_inlines(spans), true);
        continue;
      }
      if (link_url_) {
        link_text_ += (link_text_.empty() ? "" : " ") + std::string(line);
        continue;
      }
      add_text(line);
    }
    flush_paragraph();
    close_lists(0);
    for (ir::Block& b : blocks_) doc_.blocks.push_back(std::move(b));
  }

 private:
  // ---- output helpers ----
  ir::Blocks& target() {
    if (!stack_.empty() && !stack_.back().list.items.empty()) return stack_.back().list.items.back();
    if (quote_depth_ > 0 && !quotes_.empty()) return quotes_.back();
    return blocks_;
  }

  void add_text(std::string_view line) {
    // "\c" at the end joins the next input line without a space.
    const bool join = line.ends_with("\\c");
    if (join) line.remove_suffix(2);
    std::vector<Span> spans;
    decode_line(line, font_, spans, previous_font_);
    if (!paragraph_.empty() && !join_next_) paragraph_.push_back({" ", Font::Roman});
    if (paragraph_.empty() && !pending_inlines_.empty() && !join_next_) paragraph_.push_back({" ", Font::Roman});
    for (Span& s : spans) paragraph_.push_back(std::move(s));
    join_next_ = join;
  }

  void add_inlines(ir::Inlines inlines) {
    flush_spans();
    if (!pending_inlines_.empty() && !join_next_) append_text(pending_inlines_, " ");
    join_next_ = false;
    for (ir::Inline& i : inlines) pending_inlines_.push_back(std::move(i));
    paragraph_text_started_ = true;
  }

  void flush_spans() {
    if (paragraph_.empty()) return;
    for (ir::Inline& i : spans_to_inlines(paragraph_)) pending_inlines_.push_back(std::move(i));
    paragraph_.clear();
    paragraph_text_started_ = true;
  }

  void flush_paragraph() {
    flush_spans();
    normalize_inlines(pending_inlines_);
    // Trim.
    if (!pending_inlines_.empty()) {
      if (auto* t = pending_inlines_.front().get_if<ir::Text>()) t->text.erase(0, t->text.find_first_not_of(' '));
      if (auto* t = pending_inlines_.back().get_if<ir::Text>()) {
        while (!t->text.empty() && t->text.back() == ' ') t->text.pop_back();
      }
      normalize_inlines(pending_inlines_);
    }
    if (!pending_inlines_.empty()) target().push_back(ir::Paragraph{std::move(pending_inlines_)});
    pending_inlines_.clear();
    paragraph_text_started_ = false;
  }

  void add_verbatim_line(std::string_view line) {
    Font font = Font::Roman;
    Font previous = Font::Roman;
    std::vector<Span> spans;
    decode_line(line, font, spans, previous);
    std::string text;
    for (const Span& s : spans) text += s.text;
    if (!verbatim_text_.empty() || verbatim_started_) verbatim_text_.push_back('\n');
    verbatim_text_ += util::replace_all(text, "\xC2\xA0", " ");
    verbatim_started_ = true;
  }

  struct ListFrame {
    ir::List list;
    int indent_level = 0;  // .RS depth it belongs to
  };

  void start_item(ir::Inlines tag, bool definition) {
    flush_paragraph();
    const std::string label = inline_line(tag);
    bool ordered = false;
    bool bullet = false;
    if (!definition) {
      bullet = label == "\xE2\x80\xA2" || label == "-" || label == "*" || label == "o" || label.empty() || label == "\xE2\x80\x93";
      std::string_view l = label;
      if (l.ends_with(".") || l.ends_with(")")) l.remove_suffix(1);
      ordered = !l.empty() && std::ranges::all_of(l, [](char c) { return util::is_ascii_digit(c); });
      if (!bullet && !ordered) definition = true;
    }
    // Find or open a list at the current .RS level.
    while (!stack_.empty() && stack_.back().indent_level > rs_depth_) close_lists(stack_.size() - 1);
    if (!stack_.empty() && stack_.back().indent_level == rs_depth_ &&
        (stack_.back().list.definition != definition || stack_.back().list.ordered != ordered)) {
      close_lists(stack_.size() - 1);
    }
    if (stack_.empty() || stack_.back().indent_level < rs_depth_) {
      if (stack_.size() >= 16) return;
      ListFrame frame;
      frame.indent_level = rs_depth_;
      frame.list.definition = definition;
      frame.list.ordered = ordered;
      if (ordered) frame.list.start = static_cast<int>(util::parse_int(std::string_view(label).substr(0, label.find_first_not_of("0123456789"))).value_or(1));
      stack_.push_back(std::move(frame));
    }
    ir::Blocks item;
    if (definition) item.push_back(ir::Paragraph{std::move(tag)});
    stack_.back().list.items.push_back(std::move(item));
  }

  void close_lists(std::size_t keep) {
    flush_paragraph();
    while (stack_.size() > keep) {
      ListFrame frame = std::move(stack_.back());
      stack_.pop_back();
      if (frame.list.items.empty()) continue;
      target().push_back(std::move(frame.list));
    }
  }

  void heading(int level, std::string_view rest) {
    flush_paragraph();
    close_lists(0);
    while (quote_depth_ > 0) end_quote();
    rs_depth_ = 0;
    std::string text;
    for (const std::string& a : macro_arguments(rest)) text += (text.empty() ? "" : " ") + a;
    Font font = Font::Roman;
    Font previous = Font::Roman;
    std::vector<Span> spans;
    decode_line(text, font, spans, previous);
    blocks_.push_back(ir::Heading{level, spans_to_inlines(spans)});
  }

  void end_quote() {
    if (quotes_.empty()) {
      quote_depth_ = 0;
      return;
    }
    ir::Blocks inner = std::move(quotes_.back());
    quotes_.pop_back();
    --quote_depth_;
    if (!inner.empty()) target().push_back(ir::BlockQuote{std::move(inner)});
  }

  // Alternating font macros (.BR a b c -> bold a, roman b, bold c).
  ir::Inlines alternating(std::string_view rest, Font first, Font second) {
    ir::Inlines out;
    const std::vector<std::string> args = macro_arguments(rest);
    for (std::size_t k = 0; k < args.size(); ++k) {
      Font font = (k % 2 == 0) ? first : second;
      Font previous = font;
      std::vector<Span> spans;
      decode_line(args[k], font, spans, previous);
      for (Span& s : spans) s.font = s.font == Font::Roman ? ((k % 2 == 0) ? first : second) : s.font;
      for (ir::Inline& i : spans_to_inlines(spans)) out.push_back(std::move(i));
    }
    return out;
  }

  // ---- macros ----
  void macro(const std::string& name, std::string_view rest, const std::vector<std::string_view>& lines, std::size_t& i) {
    (void)lines;
    (void)i;
    if (name == "TH" || name == "Dt") {
      const std::vector<std::string> args = macro_arguments(rest);
      if (!args.empty()) {
        Font font = Font::Roman;
        Font previous = Font::Roman;
        std::vector<Span> spans;
        decode_line(args[0], font, spans, previous);
        doc_.metadata.title = inline_line(spans_to_inlines(spans));
        if (args.size() > 1 && !args[1].empty()) doc_.metadata.extra["section"] = args[1];
        if (name == "TH" && args.size() > 2 && !args[2].empty()) doc_.metadata.date = args[2];
      }
      return;
    }
    if (name == "Dd") {
      if (!rest.empty() && rest != "$Mdocdate$") doc_.metadata.date = std::string(util::trim(util::replace_all(rest, "$Mdocdate:", "")));
      return;
    }
    if (name == "SH" || name == "Sh") return heading(1, rest);
    if (name == "SS" || name == "Ss") return heading(2, rest);
    if (name == "PP" || name == "LP" || name == "P" || name == "Pp" || name == "sp" || name == "HP") {
      flush_paragraph();
      if (rs_depth_ == 0 && name != "sp" && name != "Pp") close_lists(0);
      if (name == "Pp" || name == "sp") return;
      return;
    }
    if (name == "br") {
      if (verbatim_) {
        return;
      }
      flush_spans();
      pending_inlines_.push_back(ir::LineBreak{});
      return;
    }
    if (name == "IP") {
      const std::vector<std::string> args = macro_arguments(rest);
      if (args.empty() || args[0].empty()) {
        // An indented paragraph continuing the current item.
        flush_paragraph();
        return;
      }
      Font font = Font::Roman;
      Font previous = Font::Roman;
      std::vector<Span> spans;
      decode_line(args[0], font, spans, previous);
      start_item(spans_to_inlines(spans), false);
      return;
    }
    if (name == "TP" || name == "TQ") {
      flush_paragraph();
      pending_tag_ = true;
      return;
    }
    if (name == "RS") {
      flush_paragraph();
      ++rs_depth_;
      if (stack_.empty() || stack_.back().list.items.empty()) {
        ++quote_depth_;
        quotes_.emplace_back();
        quote_rs_.push_back(rs_depth_);
      }
      return;
    }
    if (name == "RE") {
      flush_paragraph();
      while (!stack_.empty() && stack_.back().indent_level >= rs_depth_ && rs_depth_ > 0) close_lists(stack_.size() - 1);
      if (!quote_rs_.empty() && quote_rs_.back() == rs_depth_) {
        quote_rs_.pop_back();
        end_quote();
      }
      if (rs_depth_ > 0) --rs_depth_;
      return;
    }
    if (name == "nf" || name == "EX" || name == "Bd") {
      flush_paragraph();
      if (name == "Bd" && rest.find("-literal") == std::string_view::npos && rest.find("-unfilled") == std::string_view::npos) {
        ++quote_depth_;
        quotes_.emplace_back();
        quote_rs_.push_back(-1);
        return;
      }
      verbatim_ = true;
      verbatim_started_ = false;
      verbatim_text_.clear();
      return;
    }
    if (name == "fi" || name == "EE" || name == "Ed") {
      if (verbatim_) {
        verbatim_ = false;
        target().push_back(ir::CodeBlock{{}, verbatim_text_});
        verbatim_text_.clear();
      } else if (name == "Ed" && !quote_rs_.empty() && quote_rs_.back() == -1) {
        flush_paragraph();
        quote_rs_.pop_back();
        end_quote();
      }
      return;
    }
    if (name == "UR" || name == "MT") {
      flush_spans();
      link_url_ = true;
      link_target_ = (name == "MT" ? "mailto:" : "") + std::string(util::trim(rest));
      link_text_.clear();
      return;
    }
    if (name == "UE" || name == "ME") {
      link_url_ = false;
      Font font = Font::Roman;
      Font previous = Font::Roman;
      std::vector<Span> spans;
      decode_line(link_text_, font, spans, previous);
      ir::Inlines label = spans_to_inlines(spans);
      if (label.empty()) label = {ir::Text{link_target_.starts_with("mailto:") ? link_target_.substr(7) : link_target_}};
      add_inlines({ir::Link{link_target_, {}, std::move(label)}});
      // Trailing punctuation after .UE, or "\c" joining the next line.
      std::string_view trailing = util::trim(rest);
      if (trailing.ends_with("\\c")) {
        join_next_ = true;
        trailing.remove_suffix(2);
        trailing = util::trim(trailing);
      }
      if (!trailing.empty()) {
        Font f = Font::Roman;
        Font p = Font::Roman;
        std::vector<Span> sp;
        decode_line(trailing, f, sp, p);
        for (ir::Inline& x : spans_to_inlines(sp)) pending_inlines_.push_back(std::move(x));
      }
      return;
    }
    // Font macros.
    if (name == "B") return add_inlines({ir::Strong{alternating(rest, Font::Roman, Font::Roman)}});
    if (name == "I") return add_inlines({ir::Emphasis{alternating(rest, Font::Roman, Font::Roman)}});
    if (name == "BR") return add_inlines(alternating(rest, Font::Bold, Font::Roman));
    if (name == "RB") return add_inlines(alternating(rest, Font::Roman, Font::Bold));
    if (name == "IR") return add_inlines(alternating(rest, Font::Italic, Font::Roman));
    if (name == "RI") return add_inlines(alternating(rest, Font::Roman, Font::Italic));
    if (name == "BI") return add_inlines(alternating(rest, Font::Bold, Font::Italic));
    if (name == "IB") return add_inlines(alternating(rest, Font::Italic, Font::Bold));
    if (name == "SB" || name == "SM") return add_inlines(alternating(rest, name == "SB" ? Font::Bold : Font::Roman, Font::Roman));
    // mdoc.
    if (name == "Nm") {
      std::string text(util::trim(rest));
      if (text.empty()) text = page_name_;
      if (page_name_.empty()) page_name_ = text;
      return add_inlines({ir::Strong{{ir::Text{text}}}});
    }
    if (name == "Nd") return add_inlines({ir::Text{"\xE2\x80\x94 " + std::string(util::trim(rest))}});
    if (name == "Bl") {
      flush_paragraph();
      ListFrame frame;
      frame.indent_level = ++rs_depth_;
      frame.list.ordered = rest.find("-enum") != std::string_view::npos;
      frame.list.definition = rest.find("-tag") != std::string_view::npos || rest.find("-hang") != std::string_view::npos ||
                              rest.find("-ohang") != std::string_view::npos || rest.find("-inset") != std::string_view::npos ||
                              rest.find("-diag") != std::string_view::npos;
      mdoc_column_ = rest.find("-column") != std::string_view::npos;
      if (mdoc_column_) {
        mdoc_table_ = ir::TableBlock{};
      }
      if (stack_.size() < 16) stack_.push_back(std::move(frame));
      return;
    }
    if (name == "It") {
      flush_paragraph();
      if (mdoc_column_) {
        ir::TableRow row;
        std::string cells_text(rest);
        const std::string split_text = util::replace_all(cells_text, " Ta ", "\t");  // split() returns views into it
        for (const std::string_view cell : util::split(split_text, '\t')) {
          row.cells.push_back(ir::TableCell{mdoc_inline(util::trim(cell)), 1, 1});
        }
        mdoc_table_.rows.push_back(std::move(row));
        return;
      }
      if (stack_.empty()) return;
      ir::Blocks item;
      if (stack_.back().list.definition) item.push_back(ir::Paragraph{mdoc_inline(rest)});
      stack_.back().list.items.push_back(std::move(item));
      return;
    }
    if (name == "El") {
      flush_paragraph();
      if (mdoc_column_) {
        mdoc_column_ = false;
        if (!stack_.empty()) stack_.pop_back();
        if (rs_depth_ > 0) --rs_depth_;
        if (!mdoc_table_.rows.empty()) target().push_back(std::move(mdoc_table_));
        return;
      }
      if (!stack_.empty()) close_lists(stack_.size() - 1);
      if (rs_depth_ > 0) --rs_depth_;
      return;
    }
    if (name == "D1" || name == "Dl") {
      flush_paragraph();
      if (name == "Dl") {
        target().push_back(ir::CodeBlock{{}, ir::plain_text(mdoc_inline(rest))});
      } else {
        target().push_back(ir::BlockQuote{{ir::Paragraph{mdoc_inline(rest)}}});
      }
      return;
    }
    if (name == "Os" || name == "An" || name == "Re" || name == "Rs" || name == "Ns" || name == "Bk" || name == "Ek" ||
        name == "ft" || name == "ne" || name == "in" || name == "ti" || name == "ad" || name == "na" || name == "hy" ||
        name == "nh" || name == "ds" || name == "de" || name == "so" || name == "ll" || name == "PD" || name == "ta" ||
        name == "Sm" || name == "Fd" || name == "In" || name == "Lb") {
      return;
    }
    // Other mdoc inline macros, and unknown macros: their text.
    add_inlines(mdoc_inline(std::string(name) + " " + std::string(rest)));
  }

  // mdoc callable macros within a line.
  ir::Inlines mdoc_inline(std::string_view text) {
    ir::Inlines out;
    const std::vector<std::string> args = macro_arguments(text);
    enum class Mode { Text, Flag, Arg, Bold, Italic, Code, Path, Xr, Link };
    Mode mode = Mode::Text;
    std::string open_close;  // pending closing delimiter for Dq/Sq/Pq/Op
    std::vector<std::string> closers;
    bool no_space = false;
    const auto put = [&](ir::Inline node) {
      if (!out.empty() && !no_space) append_text(out, " ");
      no_space = false;
      out.push_back(std::move(node));
    };
    for (std::size_t k = 0; k < args.size(); ++k) {
      const std::string& a = args[k];
      if (a == "Fl") { mode = Mode::Flag; continue; }
      if (a == "Ar" || a == "Va" || a == "Fa" || a == "Ev") { mode = Mode::Arg; continue; }
      if (a == "Cm" || a == "Sy" || a == "Ic" || a == "Nm" || a == "Dv" || a == "Er" || a == "Fn" || a == "Ft") { mode = Mode::Bold; continue; }
      if (a == "Em") { mode = Mode::Italic; continue; }
      if (a == "Li" || a == "Ql") { mode = Mode::Code; continue; }
      if (a == "Pa") { mode = Mode::Path; continue; }
      if (a == "Xr") { mode = Mode::Xr; continue; }
      if (a == "Lk" || a == "Mt") { mode = Mode::Link; continue; }
      if (a == "Ns") { no_space = true; continue; }
      if (a == "No") { mode = Mode::Text; continue; }
      if (a == "Dq" || a == "Do" || a == "Qq") { put(ir::Text{"\xE2\x80\x9C"}); no_space = true; closers.push_back("\xE2\x80\x9D"); continue; }
      if (a == "Sq" || a == "So") { put(ir::Text{"\xE2\x80\x98"}); no_space = true; closers.push_back("\xE2\x80\x99"); continue; }
      if (a == "Pq" || a == "Po") { put(ir::Text{"("}); no_space = true; closers.push_back(")"); continue; }
      if (a == "Op" || a == "Oo" || a == "Bq" || a == "Bo") { put(ir::Text{"["}); no_space = true; closers.push_back("]"); continue; }
      if (a == "Aq" || a == "Ao") { put(ir::Text{"<"}); no_space = true; closers.push_back(">"); continue; }
      if (a == "Dc" || a == "Sc" || a == "Pc" || a == "Oc" || a == "Bc" || a == "Ac") {
        if (!closers.empty()) {
          no_space = true;
          put(ir::Text{closers.back()});
          closers.pop_back();
        }
        continue;
      }
      // Punctuation attaches to the previous word.
      if (a.size() == 1 && std::string_view(".,:;)]?!").find(a[0]) != std::string_view::npos) {
        no_space = true;
        put(ir::Text{a});
        continue;
      }
      Font font = Font::Roman;
      Font previous = Font::Roman;
      std::vector<Span> spans;
      decode_line(a, font, spans, previous);
      std::string plain;
      for (const Span& s : spans) plain += s.text;
      switch (mode) {
        case Mode::Flag: put(ir::Strong{{ir::Text{"-" + plain}}}); break;
        case Mode::Arg: put(ir::Emphasis{{ir::Text{plain}}}); break;
        case Mode::Bold: put(ir::Strong{{ir::Text{plain}}}); break;
        case Mode::Italic: put(ir::Emphasis{{ir::Text{plain}}}); break;
        case Mode::Code: put(ir::Code{plain}); break;
        case Mode::Path: put(ir::Emphasis{{ir::Text{plain}}}); break;
        case Mode::Xr:
          if (k + 1 < args.size()) {
            put(ir::Strong{{ir::Text{plain}}});
            no_space = true;
            put(ir::Text{"(" + args[++k] + ")"});
          } else {
            put(ir::Strong{{ir::Text{plain}}});
          }
          mode = Mode::Text;
          break;
        case Mode::Link: {
          std::string label;
          while (k + 1 < args.size() && !(args[k + 1].size() == 1 && std::string_view(".,:;").find(args[k + 1][0]) != std::string_view::npos)) {
            label += (label.empty() ? "" : " ") + args[++k];
          }
          const std::string url = plain.find(':') == std::string::npos && plain.find('@') != std::string::npos ? "mailto:" + plain : plain;
          put(ir::Link{url, {}, {ir::Text{label.empty() ? plain : label}}});
          mode = Mode::Text;
          break;
        }
        case Mode::Text:
          for (ir::Inline& x : spans_to_inlines(spans)) put(std::move(x));
          break;
      }
    }
    while (!closers.empty()) {
      no_space = true;
      put(ir::Text{closers.back()});
      closers.pop_back();
    }
    normalize_inlines(out);
    return out;
  }

  // tbl tables: options; format lines ending with '.'; data rows.
  void tbl(const std::vector<std::string_view>& lines) {
    std::size_t i = 0;
    char tab = '\t';
    if (i < lines.size() && util::trim(lines[i]).ends_with(";")) {
      const std::string options(lines[i]);
      if (const std::size_t at = options.find("tab("); at != std::string::npos && at + 5 < options.size()) tab = options[at + 4];
      ++i;
    }
    std::vector<ir::Alignment> aligns;
    while (i < lines.size()) {
      const std::string_view f = util::trim(lines[i++]);
      if (aligns.empty()) {
        for (const char c : f) {
          if (c == 'l' || c == 'L') aligns.push_back(ir::Alignment::Left);
          if (c == 'r' || c == 'R') aligns.push_back(ir::Alignment::Right);
          if (c == 'c' || c == 'C') aligns.push_back(ir::Alignment::Center);
          if (c == 'n' || c == 'N') aligns.push_back(ir::Alignment::Right);
        }
      }
      if (f.ends_with(".")) break;
    }
    ir::TableBlock table;
    table.alignments = aligns;
    std::size_t header_rows = 0;
    std::vector<std::string> cells;
    std::string current;
    bool in_block = false;
    const auto end_row = [&] {
      if (cells.empty()) return;
      ir::TableRow row;
      for (const std::string& c : cells) {
        Font font = Font::Roman;
        Font previous = Font::Roman;
        std::vector<Span> spans;
        decode_line(c, font, spans, previous);
        row.cells.push_back(ir::TableCell{spans_to_inlines(spans), 1, 1});
      }
      table.rows.push_back(std::move(row));
      cells.clear();
    };
    for (; i < lines.size(); ++i) {
      std::string_view l = lines[i];
      if (in_block) {
        const std::size_t end = l.find("T}");
        if (end != std::string_view::npos) {
          current += (current.empty() ? "" : " ") + std::string(l.substr(0, end));
          cells.push_back(current);
          current.clear();
          in_block = false;
          l = l.substr(end + 2);
          if (l.starts_with(std::string(1, tab))) l.remove_prefix(1);
          if (l.empty()) {
            if (i + 1 >= lines.size() || !lines[i + 1].starts_with("T{") || true) {
              // Row ends unless the line continues with more cells.
            }
            if (cells.size() >= std::max<std::size_t>(aligns.size(), 1)) end_row();
            continue;
          }
        } else {
          if (!l.starts_with(".")) current += (current.empty() ? "" : " ") + std::string(l);
          continue;
        }
      }
      if (util::trim(l) == "_" || util::trim(l) == "=") {
        end_row();
        if (header_rows == 0) header_rows = table.rows.size();
        continue;
      }
      if (l.starts_with(".")) continue;
      for (const std::string_view part : util::split(l, tab)) {
        if (part.starts_with("T{")) {
          in_block = true;
          current = std::string(util::trim(part.substr(2)));
          continue;
        }
        cells.emplace_back(part);
      }
      if (!in_block) end_row();
    }
    end_row();
    for (std::size_t r = 0; r < header_rows && r < table.rows.size(); ++r) table.rows[r].header = true;
    if (!table.rows.empty()) target().push_back(std::move(table));
  }

  ir::Document& doc_;
  ir::Blocks blocks_;
  std::vector<ListFrame> stack_;
  std::vector<ir::Blocks> quotes_;
  std::vector<int> quote_rs_;
  int quote_depth_ = 0;
  int rs_depth_ = 0;
  std::vector<Span> paragraph_;
  ir::Inlines pending_inlines_;
  bool paragraph_text_started_ = false;
  Font font_ = Font::Roman;
  Font previous_font_ = Font::Roman;
  bool verbatim_ = false;
  bool verbatim_started_ = false;
  std::string verbatim_text_;
  bool pending_tag_ = false;
  bool join_next_ = false;
  bool link_url_ = false;
  std::string link_target_;
  std::string link_text_;
  std::string page_name_;
  bool mdoc_column_ = false;
  ir::TableBlock mdoc_table_;
};

ir::Document read_man(std::string_view text) {
  ir::Document document;
  ManReader reader(document);
  reader.read(text);
  // A trailing NOTES section of numbered notes (pandoc's convention) stays as text.
  return document;
}

// ---- writing --------------------------------------------------------------------------------------------

std::string roff_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '\\') {
      out.append("\\e");
    } else if (c == '\n') {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  // No-break spaces.
  return util::replace_all(out, "\xC2\xA0", "\\ ");
}

// A text line must not start with '.' or '\'' (requests).
std::string roff_line(std::string_view line) {
  std::string out(util::trim_left(line));
  if (out.starts_with(".") || out.starts_with("'")) out.insert(0, "\\&");
  return out;
}

class ManWriter {
 public:
  explicit ManWriter(const ir::Document& document) : doc_(document) {
    m_.escape = roff_escape;
    m_.strong = {"\\fB", "\\fR"};
    m_.emphasis = {"\\fI", "\\fR"};
    m_.underline = {"\\fI", "\\fR"};
    m_.strikethrough = {"[", "]"};
    m_.superscript = {"^", ""};
    m_.subscript = {"_", ""};
    m_.line_break = "\n.br\n";
    m_.soft_break = " ";
    m_.code = [](const ir::Code& code) -> std::string {
      if (!code.raw_format.empty()) return code.raw_format == "man" ? code.text : std::string();
      std::string text = code.text;
      for (char& c : text) {
        if (c == '\n') c = ' ';
      }
      return "\\fB" + roff_escape(text) + "\\fR";
    };
    m_.link = [](const ir::Link& link, const std::string& label) -> std::string {
      if (link.url.starts_with("#")) return label;
      std::string url = link.url;
      std::erase_if(url, [](char c) { return util::is_ascii_space(c); });
      const bool mail = url.starts_with("mailto:");
      return "\\c\n" + std::string(mail ? ".MT " + url.substr(7) : ".UR " + url) + "\n" + label + "\n" + (mail ? ".ME" : ".UE") + " \\c\n";
    };
    m_.image = [](const ir::InlineImage& image) { return "[" + roff_escape(image.alt.empty() ? "image" : image.alt) + "]"; };
    m_.note = [](const ir::Link&, std::size_t index) { return "[" + std::to_string(index) + "]"; };
  }

  std::string run() {
    std::string title = doc_.metadata.title.empty() ? std::string("UNTITLED") : doc_.metadata.title;
    for (char& c : title) {
      if (c == '"') c = '\'';
    }
    std::string section = "1";
    if (const auto it = doc_.metadata.extra.find("section"); it != doc_.metadata.extra.end()) section = it->second;
    std::string date = doc_.metadata.date;
    std::erase(date, '"');
    std::string out = ".TH \"" + roff_escape(title) + "\" \"" + roff_escape(section) + "\" \"" + roff_escape(date) + "\" \"\" \"\"\n";
    out += blocks(doc_.blocks, 0);
    if (!doc_.footnotes.empty()) {
      out += ".SH NOTES\n";
      for (std::size_t i = 0; i < doc_.footnotes.size(); ++i) {
        out += ".IP \"[" + std::to_string(i + 1) + "]\" 5\n" + blocks(doc_.footnotes[i].blocks, 1);
      }
    }
    // Collapse the line structure: no empty lines (they mean vertical space).
    std::string cleaned;
    for (const std::string_view line : util::split(out, '\n')) {
      if (util::trim(line).empty()) continue;
      cleaned.append(line).push_back('\n');
    }
    return cleaned;
  }

 private:
  std::string text(const ir::Inlines& content) {
    // Source lines are kept short; roff fills them anyway.
    std::string rendered = wrap_text(render_inlines(content, m_, &doc_), 78);
    std::string out;
    for (const std::string_view line : util::split(rendered, '\n')) {
      if (line.empty()) continue;
      if (line.starts_with(".UR") || line.starts_with(".UE") || line.starts_with(".MT") || line.starts_with(".ME") ||
          line.starts_with(".br")) {
        out.append(line).push_back('\n');
      } else {
        out.append(roff_line(line)).push_back('\n');
      }
    }
    return out;
  }

  std::string blocks(const ir::Blocks& list, int depth) {
    std::string out;
    bool first = true;
    for (const ir::Block& b : list) {
      // A paragraph right after a heading needs no .PP.
      out += block(b, depth, first || after_heading_);
      after_heading_ = b.is<ir::Heading>();
      first = false;
    }
    return out;
  }

  std::string block(const ir::Block& block, int depth, bool first_in_item) {
    if (depth > ir::max_document_depth * 2) return {};
    std::string out;
    // Right after .SH/.SS a new paragraph starts anyway; .PP there is redundant.
    const std::string pp = (after_heading_ && depth == 0) ? std::string() : std::string(".PP\n");
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Paragraph>) {
            out = (first_in_item && (depth > 0 || after_heading_) ? std::string() : std::string(".PP\n")) + text(node.content);
          } else if constexpr (std::same_as<T, ir::Heading>) {
            std::string t = render_inlines(node.content, m_, &doc_);
            for (char& c : t) {
              if (c == '\n') c = ' ';
            }
            out = (node.level <= 1 ? ".SH " : ".SS ") + std::string("\"") + util::replace_all(std::string(util::trim(t)), "\"", "\\(dq") + "\"\n";
          } else if constexpr (std::same_as<T, ir::List>) {
            int number = node.start;
            for (std::size_t i = 0; i < node.items.size(); ++i) {
              const ir::Blocks& item = node.items[i];
              if (node.definition) {
                std::string term;
                if (!item.empty()) {
                  if (const auto* p = item.front().get_if<ir::Paragraph>()) term = text(p->content);
                }
                out += ".TP\n" + (term.empty() ? std::string("\\&\n") : term);
                ir::Blocks rest(item.begin() + std::min<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(item.size())), item.end());
                out += nested(rest, depth);
              } else {
                std::string marker = node.ordered ? "\"" + std::to_string(number++) + ".\" 4" : std::string("\\(bu 2");
                if (node.task(i) == ir::TaskState::Done) marker = "\"[x]\" 4";
                if (node.task(i) == ir::TaskState::Open) marker = "\"[ ]\" 4";
                out += ".IP " + marker + "\n" + nested(item, depth);
              }
            }
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            if (!node.raw_format.empty()) {
              if (node.raw_format == "man") out = node.code + "\n";
              return;
            }
            out = pp + ".EX\n";
            for (const std::string_view line : util::split(node.code, '\n')) {
              std::string l = roff_escape(line);
              if (l.starts_with(".") || l.starts_with("'")) l.insert(0, "\\&");
              if (l.empty()) l = "\\&";
              out += l + "\n";
            }
            out += ".EE\n";
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            out = ".RS\n" + blocks(node.blocks, depth + 1) + ".RE\n";
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            out = table(node, pp);
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            const std::string& alt = node.caption.empty() ? node.alt : node.caption;
            out = pp + roff_line("[" + roff_escape(alt.empty() ? std::string("image") : alt) + "]") + "\n";
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            out = pp + "\\l'\\n(.lu'\n";
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            out.clear();
          }
        },
        block.node);
    return out;
  }

  // Blocks of a list item: the first paragraph follows the .IP/.TP line, the
  // rest is indented with .RS/.RE.
  std::string nested(const ir::Blocks& item, int depth) {
    std::string out;
    bool first = true;
    bool indented = false;
    for (const ir::Block& b : item) {
      if (first && b.is<ir::Paragraph>()) {
        out += text(b.get_if<ir::Paragraph>()->content);
      } else if (b.is<ir::List>()) {
        if (!indented) {
          out += ".RS\n";
          indented = true;
        }
        out += block(b, depth + 1, false);
      } else {
        out += block(b, depth + 1, false);
      }
      first = false;
    }
    if (indented) out += ".RE\n";
    return out;
  }

  std::string table(const ir::TableBlock& table, const std::string& pp) {
    const auto cells = table_strings(table, [this](const ir::Inlines& c) {
      std::string t = render_inlines(c, m_, &doc_);
      for (char& ch : t) {
        if (ch == '\n') ch = ' ';
      }
      return t;
    });
    if (cells.empty()) return {};
    std::size_t columns = 0;
    for (const auto& row : cells) columns = std::max(columns, row.size());
    std::string format;
    for (std::size_t c = 0; c < columns; ++c) {
      const ir::Alignment a = table.column_alignment(c);
      format += (c > 0 ? " " : "") + std::string(a == ir::Alignment::Right ? "r" : a == ir::Alignment::Center ? "c" : "l");
    }
    std::string out = pp + ".TS\ntab(@);\n" + format + ".\n";
    const std::size_t header = header_row_count(table);
    for (std::size_t r = 0; r < cells.size(); ++r) {
      for (std::size_t c = 0; c < columns; ++c) {
        if (c > 0) out += "@";
        std::string cell = c < cells[r].size() ? cells[r][c] : std::string();
        out += "T{\n" + roff_line(cell.empty() ? "\\&" : cell) + "\nT}";
      }
      out += "\n";
      if (header > 0 && r + 1 == header) out += "_\n";
    }
    return out + ".TE\n";
  }

  const ir::Document& doc_;
  InlineMarkup m_;
  bool after_heading_ = false;
};

int sniff_man(ByteView data, std::string_view filename) {
  const std::string_view head = sniff_head(data, 2048);
  std::size_t i = 0;
  // Skip comment lines.
  while (i < head.size() && (head.substr(i).starts_with(".\\\"") || head.substr(i).starts_with("'\\\""))) {
    const std::size_t nl = head.find('\n', i);
    if (nl == std::string_view::npos) return 0;
    i = nl + 1;
  }
  const std::string_view rest = head.substr(i);
  const bool content = rest.starts_with(".TH ") || rest.starts_with(".Dd ") || rest.starts_with(".Dd\n") ||
                       head.find("\n.TH ") != std::string_view::npos || head.find("\n.SH ") != std::string_view::npos;
  if (!content) return 0;
  return has_extension(filename, {"man", "1", "2", "3", "4", "5", "6", "7", "8", "9", "mdoc"}) ? 85 : 72;
}

}  // namespace

void register_man(Registry& registry) {
  MarkupFormat man;
  man.id = "man";
  man.name = "Manual page";
  man.description = "Unix manual page in roff (man and mdoc macros).";
  man.extensions = {"man", "1", "2", "3", "4", "5", "6", "7", "8", "9", "mdoc"};
  man.mime = "text/troff";
  man.popularity = 20;
  man.sniff = sniff_man;
  man.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_man(text); };
  man.read_lossy = true;
  man.write = [](const ir::Document& document, WriteContext&) -> Result<std::string> {
    ManWriter writer(document);
    return writer.run();
  };
  man.write_lossy = true;  // no pictures, strike/sub/sup as plain text, notes as a NOTES section
  register_markup(registry, std::move(man));
}

}  // namespace cvt::document
