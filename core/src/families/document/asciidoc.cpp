// Convertible core - document family: AsciiDoc markup (Asciidoctor dialect).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

struct PendingAttributes {
  std::vector<std::string> positional;
  std::vector<std::pair<std::string, std::string>> named;
  std::string title;
  std::string id;

  [[nodiscard]] std::string style() const { return positional.empty() ? std::string() : positional.front(); }
  [[nodiscard]] std::string get(std::string_view key) const {
    for (const auto& [k, v] : named) {
      if (k == key) return v;
    }
    return {};
  }
  void clear() { *this = {}; }
};

void parse_attribute_line(std::string_view inner, PendingAttributes& attrs) {
  // [source,python] [quote, Someone] [cols="<,>",options="header"] [#id.role]
  std::vector<std::string> parts;
  std::string current;
  bool quoted = false;
  for (const char c : inner) {
    if (c == '"') {
      quoted = !quoted;
      continue;
    }
    if (c == ',' && !quoted) {
      parts.push_back(std::string(util::trim(current)));
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  parts.push_back(std::string(util::trim(current)));
  for (std::string& part : parts) {
    const std::size_t eq = part.find('=');
    if (eq != std::string::npos) {
      attrs.named.emplace_back(std::string(util::trim(part.substr(0, eq))), std::string(util::trim(part.substr(eq + 1))));
      continue;
    }
    if (part.starts_with("#")) {
      const std::size_t dot = part.find('.');
      attrs.id = part.substr(1, dot == std::string::npos ? std::string::npos : dot - 1);
      part = dot == std::string::npos ? std::string() : part.substr(dot);
    }
    if (part.starts_with(".")) part.erase(0, 1);
    attrs.positional.push_back(std::move(part));
  }
}

std::optional<wiki::Heading> adoc_heading(std::string_view line) {
  std::size_t marks = 0;
  while (marks < line.size() && (line[marks] == '=' || line[marks] == '#')) ++marks;
  if (marks < 2 || marks > 6 || marks >= line.size() || line[marks] != ' ') return std::nullopt;
  if (line[0] == '#' && line.find('=') != std::string_view::npos && line.find('=') < marks) return std::nullopt;
  return wiki::Heading{static_cast<int>(marks) - 1, std::string(util::trim(line.substr(marks))), {}};
}

std::optional<wiki::ListItem> adoc_list(std::string_view line) {
  const std::string_view t = util::trim_left(line);
  std::size_t n = 0;
  if (!t.empty() && (t[0] == '*' || t[0] == '.' || t[0] == '-')) {
    const char m = t[0];
    while (n < t.size() && t[n] == m) ++n;
    if (n >= t.size() || t[n] != ' ' || (m == '-' && n > 1)) {
      n = 0;
    } else {
      wiki::ListItem item;
      item.depth = static_cast<int>(std::min<std::size_t>(n, 16));
      item.ordered = m == '.';
      std::string_view text = util::trim(t.substr(n));
      if (text.starts_with("[ ] ")) {
        item.task = ir::TaskState::Open;
        text.remove_prefix(4);
      } else if (text.starts_with("[x] ") || text.starts_with("[*] ")) {
        item.task = ir::TaskState::Done;
        text.remove_prefix(4);
      }
      item.text = std::string(text);
      return item;
    }
  }
  // Explicit numbers "1. item".
  std::size_t digits = 0;
  while (digits < t.size() && util::is_ascii_digit(t[digits])) ++digits;
  if (digits > 0 && digits < 10 && digits + 1 < t.size() && t[digits] == '.' && t[digits + 1] == ' ') {
    wiki::ListItem item;
    item.ordered = true;
    item.text = std::string(util::trim(t.substr(digits + 2)));
    return item;
  }
  // Definition list "term:: definition".
  const std::size_t sep = t.find("::");
  if (sep != std::string_view::npos && sep > 0 && line.substr(0, 1) != " " &&
      (sep + 2 == t.size() || t[sep + 2] == ' ') && !t.starts_with("image::") && t.find("://") == std::string_view::npos) {
    wiki::ListItem item;
    item.term = true;
    const std::string term(util::trim(t.substr(0, sep)));
    const std::string definition(util::trim(t.substr(sep + 2)));
    item.text = term + (definition.empty() ? "" : "\n" + definition);
    return item;
  }
  return std::nullopt;
}

struct AdocState {
  PendingAttributes pending;
};

ir::TableBlock adoc_table(std::span<const std::string_view> content, const PendingAttributes& attrs, wiki::Reader& reader) {
  // Column count and alignment from cols="..."
  std::vector<ir::Alignment> aligns;
  std::size_t columns = 0;
  const std::string cols = attrs.get("cols");
  if (!cols.empty()) {
    for (const std::string_view spec : util::split(cols, ',', false)) {
      std::size_t repeat = 1;
      std::string_view s = util::trim(spec);
      const std::size_t star = s.find('*');
      if (star != std::string_view::npos) {
        repeat = static_cast<std::size_t>(std::clamp<std::int64_t>(util::parse_int(s.substr(0, star)).value_or(1), 1, 100));
        s = s.substr(star + 1);
      }
      ir::Alignment a = ir::Alignment::Default;
      if (s.find('<') != std::string_view::npos) a = ir::Alignment::Left;
      if (s.find('>') != std::string_view::npos) a = ir::Alignment::Right;
      if (s.find('^') != std::string_view::npos) a = ir::Alignment::Center;
      for (std::size_t k = 0; k < repeat; ++k) aligns.push_back(a);
    }
    columns = aligns.size();
  }
  // Cells: "|" starts a cell, optionally preceded by a spec ("2+", ".2+", "^").
  struct RawCell {
    std::string text;
    int colspan = 1;
    int rowspan = 1;
  };
  std::vector<RawCell> cells;
  std::size_t first_line_cells = 0;
  bool header_by_blank = false;
  for (std::size_t li = 0; li < content.size(); ++li) {
    const std::string_view line = content[li];
    if (is_blank(line)) {
      if (li == 1 && first_line_cells > 0) header_by_blank = true;
      continue;
    }
    std::size_t i = 0;
    bool any = false;
    while (i <= line.size()) {
      // Find the next unescaped '|'.
      std::size_t bar = i;
      while (bar < line.size() && !(line[bar] == '|' && (bar == 0 || line[bar - 1] != '\\'))) ++bar;
      if (bar >= line.size()) {
        if (!cells.empty() && i < line.size()) cells.back().text.append(" ").append(util::trim(line.substr(i)));
        break;
      }
      const std::string_view before = line.substr(i, bar - i);
      // Text before the bar belongs to the previous cell unless it is a spec.
      std::string_view spec = util::trim(before);
      RawCell cell;
      bool is_spec = !spec.empty() && spec.size() <= 8 &&
                     std::ranges::all_of(spec, [](char c) { return util::is_ascii_digit(c) || std::string_view(".+*<>^aehlmsdv").find(c) != std::string_view::npos; });
      if (is_spec) {
        const std::size_t plus = spec.find('+');
        if (plus != std::string_view::npos) {
          const std::string_view number = spec.substr(0, plus);
          if (number.starts_with(".")) {
            cell.rowspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(number.substr(1)).value_or(1), 1, 1000));
          } else if (const std::size_t dot = number.find('.'); dot != std::string_view::npos) {
            cell.colspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(number.substr(0, dot)).value_or(1), 1, 1000));
            cell.rowspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(number.substr(dot + 1)).value_or(1), 1, 1000));
          } else {
            cell.colspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(number).value_or(1), 1, 1000));
          }
        }
      } else if (!spec.empty() && !cells.empty()) {
        cells.back().text.append(" ").append(spec);
      }
      std::size_t end = bar + 1;
      while (end < line.size() && !(line[end] == '|' && line[end - 1] != '\\')) ++end;
      // The next spec (digits followed by '+') belongs to the next cell.
      std::size_t text_end = end;
      if (end < line.size()) {
        std::size_t k = end;
        while (k > bar + 1 && line[k - 1] != ' ') --k;
        const std::string_view tail = line.substr(k, end - k);
        if (!tail.empty() && tail.size() <= 8 && tail.find('+') != std::string_view::npos &&
            std::ranges::all_of(tail, [](char c) { return util::is_ascii_digit(c) || c == '+' || c == '.'; })) {
          text_end = k;
        }
      }
      cell.text = util::replace_all(util::trim(line.substr(bar + 1, text_end - bar - 1)), "\\|", "|");
      cells.push_back(std::move(cell));
      any = true;
      i = text_end;
      if (text_end == end && end >= line.size()) break;
    }
    if (li == 0 && any) first_line_cells = cells.size();
  }
  if (columns == 0) columns = first_line_cells > 0 ? first_line_cells : 1;
  ir::TableBlock table;
  table.alignments = aligns;
  const bool header = attrs.get("options").find("header") != std::string::npos ||
                      attrs.get("%header").size() || header_by_blank;
  std::size_t used = 0;
  ir::TableRow row;
  for (RawCell& cell : cells) {
    row.cells.push_back(ir::TableCell{reader.inlines(cell.text), cell.colspan, cell.rowspan});
    used += static_cast<std::size_t>(cell.colspan);
    if (used >= columns) {
      row.header = header && table.rows.empty();
      table.rows.push_back(std::move(row));
      row = {};
      used = 0;
    }
  }
  if (!row.cells.empty()) table.rows.push_back(std::move(row));
  if (!attrs.title.empty()) table.caption = reader.inlines(attrs.title);
  return table;
}

std::size_t adoc_block(AdocState& state, std::span<const std::string_view> lines, std::size_t at, ir::Blocks& out,
                       wiki::Reader& reader) {
  const std::string_view line = lines[at];
  const std::string_view trimmed = util::trim_right(line);
  ir::Document& document = *reader.context().document;
  PendingAttributes& attrs = state.pending;
  // Comments.
  if (trimmed == "////") {
    std::size_t i = at + 1;
    while (i < lines.size() && util::trim_right(lines[i]) != "////") ++i;
    return std::min(i, lines.size() - 1) - at + 1;
  }
  if (trimmed.starts_with("//") && !trimmed.starts_with("///")) return 1;
  // Document attributes.
  if (trimmed.starts_with(":") && trimmed.size() > 2) {
    const std::size_t close = trimmed.find(':', 1);
    if (close != std::string_view::npos && close > 1 && trimmed.substr(1, close - 1).find(' ') == std::string_view::npos) {
      const std::string key(trimmed.substr(1, close - 1));
      const std::string value(util::trim(trimmed.substr(close + 1)));
      if (key == "author") document.metadata.author = value;
      else if (key == "revdate" || key == "date") document.metadata.date = value;
      else if (key == "lang") document.metadata.language = value;
      else if (key == "description" || key == "keywords") document.metadata.extra[key] = value;
      return 1;
    }
  }
  // Block attributes, anchors and titles.
  if (trimmed.starts_with("[[") && trimmed.ends_with("]]")) {
    attrs.id = std::string(trimmed.substr(2, trimmed.size() - 4));
    return 1;
  }
  if (trimmed.starts_with("[") && trimmed.ends_with("]") && trimmed.find("](") == std::string_view::npos) {
    parse_attribute_line(trimmed.substr(1, trimmed.size() - 2), attrs);
    return 1;
  }
  if (trimmed.size() > 1 && trimmed[0] == '.' && trimmed[1] != '.' && trimmed[1] != ' ') {
    attrs.title = std::string(trimmed.substr(1));
    return 1;
  }
  // Section headings (with a pending anchor).
  if (auto heading = adoc_heading(trimmed)) {
    ir::Attributes a;
    a.id = attrs.id;
    attrs.clear();
    out.push_back(ir::Block(ir::Heading{heading->level, reader.inlines(heading->text)}, std::move(a)));
    return 1;
  }
  // Delimited blocks.
  const auto delimited = [&](std::string_view delimiter) -> std::optional<std::pair<std::vector<std::string_view>, std::size_t>> {
    if (trimmed.size() < 4 || !is_repeated(trimmed, delimiter.front(), 4) || !trimmed.starts_with(delimiter)) {
      if (!(delimiter == "--" && trimmed == "--") && !(delimiter == "```" && trimmed.starts_with("```"))) return std::nullopt;
    }
    std::vector<std::string_view> content;
    const std::string close = delimiter == "```" ? std::string("```") : std::string(trimmed);
    for (std::size_t i = at + 1; i < lines.size(); ++i) {
      if (util::trim_right(lines[i]) == close) return std::make_pair(content, i - at + 1);
      content.push_back(lines[i]);
    }
    return std::make_pair(content, lines.size() - at);
  };
  const std::string style = attrs.style();
  if (trimmed.starts_with("|===") && std::ranges::all_of(trimmed.substr(1), [](char c) { return c == '='; })) {
    std::vector<std::string_view> content;
    std::size_t i = at + 1;
    for (; i < lines.size() && util::trim_right(lines[i]) != trimmed; ++i) content.push_back(lines[i]);
    PendingAttributes table_attrs = attrs;
    attrs.clear();
    out.push_back(adoc_table(content, table_attrs, reader));
    return std::min(i, lines.size() - 1) - at + 1;
  }
  if (trimmed.size() >= 3 && is_repeated(trimmed, '\'', 3)) {
    attrs.clear();
    out.push_back(ir::HorizontalRule{});
    return 1;
  }
  if (auto block = delimited("----")) {
    ir::CodeBlock code;
    if (style == "source" && attrs.positional.size() > 1) code.language = attrs.positional[1];
    else if (style != "source" && !style.empty() && style != "listing") code.language = style;
    code.code = join_lines(block->first);
    attrs.clear();
    out.push_back(std::move(code));
    return block->second;
  }
  if (auto block = delimited("```")) {
    ir::CodeBlock code;
    code.language = std::string(util::trim(trimmed.substr(3)));
    code.code = join_lines(block->first);
    attrs.clear();
    out.push_back(std::move(code));
    return block->second;
  }
  if (auto block = delimited("....")) {
    out.push_back(ir::CodeBlock{{}, join_lines(block->first)});
    attrs.clear();
    return block->second;
  }
  if (auto block = delimited("++++")) {
    ir::CodeBlock raw{{}, join_lines(block->first)};
    raw.raw_format = "html";
    if (style == "stem" || style == "latexmath") {
      raw.raw_format.clear();
      raw.math = ir::MathMode::Display;
    }
    attrs.clear();
    out.push_back(std::move(raw));
    return block->second;
  }
  if (auto block = delimited("|===")) {
    if (!trimmed.starts_with("|===")) return 0;
    PendingAttributes table_attrs = attrs;
    attrs.clear();
    out.push_back(adoc_table(block->first, table_attrs, reader));
    return block->second;
  }
  for (const std::string_view delimiter : {"____", "====", "****", "--"}) {
    if (auto block = delimited(delimiter)) {
      PendingAttributes block_attrs = attrs;
      attrs.clear();
      ir::Blocks inner = reader.parse(block->first, 1);
      const std::string kind = util::to_lower(block_attrs.style());
      ir::Attributes a;
      a.id = block_attrs.id;
      if (kind == "note" || kind == "tip" || kind == "important" || kind == "warning" || kind == "caution") {
        a.add_class(ir::conventions::admonition);
        a.add_class(kind);
        out.push_back(ir::Block(ir::BlockQuote{std::move(inner)}, std::move(a)));
      } else if (delimiter == "____" || delimiter == "****" || kind == "quote") {
        out.push_back(ir::Block(ir::BlockQuote{std::move(inner)}, std::move(a)));
      } else {
        for (ir::Block& b : inner) out.push_back(std::move(b));
      }
      return block->second;
    }
  }
  // Block image.
  if (trimmed.starts_with("image::")) {
    const std::size_t bracket = trimmed.find('[', 7);
    if (bracket != std::string_view::npos && trimmed.ends_with("]")) {
      const std::string target(trimmed.substr(7, bracket - 7));
      const std::string_view inside = trimmed.substr(bracket + 1, trimmed.size() - bracket - 2);
      std::string alt(util::trim(inside.substr(0, inside.find(','))));
      if (alt.size() >= 2 && alt.front() == '"' && alt.back() == '"') alt = alt.substr(1, alt.size() - 2);
      ir::ImageBlock image{wiki::image_from_url(target), alt, attrs.title, {}};
      attrs.clear();
      out.push_back(std::move(image));
      return 1;
    }
  }
  if (trimmed == "'''" || trimmed == "---" || trimmed == "***" || trimmed == "- - -" || trimmed == "* * *") {
    attrs.clear();
    out.push_back(ir::HorizontalRule{});
    return 1;
  }
  if (trimmed == "<<<") {
    out.push_back(ir::PageBreak{});
    return 1;
  }
  // Admonition paragraphs "NOTE: text".
  for (const std::string_view kind : {"NOTE", "TIP", "IMPORTANT", "WARNING", "CAUTION"}) {
    if (trimmed.starts_with(std::string(kind) + ": ")) {
      std::string text(trimmed.substr(kind.size() + 2));
      std::size_t i = at + 1;
      while (i < lines.size() && !is_blank(lines[i])) text.append("\n").append(lines[i++]);
      ir::Attributes a;
      a.add_class(ir::conventions::admonition);
      a.add_class(util::to_lower(kind));
      out.push_back(ir::Block(ir::BlockQuote{{ir::Paragraph{reader.inlines(text)}}}, std::move(a)));
      return i - at;
    }
  }
  // A paragraph with a [quote] style.
  if (style == "quote" || style == "verse") {
    std::string text(trimmed);
    std::size_t i = at + 1;
    while (i < lines.size() && !is_blank(lines[i])) text.append("\n").append(lines[i++]);
    attrs.clear();
    out.push_back(ir::BlockQuote{{ir::Paragraph{reader.inlines(text)}}});
    return i - at;
  }
  return 0;
}

InlineSyntax adoc_inline(wiki::Context& context) {
  InlineSyntax syntax;
  syntax.escape = '\\';
  syntax.rules = {
      {"**", "**", InlineKind::Strong, false, false, true}, {"__", "__", InlineKind::Emphasis, false, false, true},
      {"``", "``", InlineKind::Code, true, false, true},    {"##", "##", InlineKind::Mark, false, false, true},
      {"*", "*", InlineKind::Strong},                       {"_", "_", InlineKind::Emphasis},
      {"`", "`", InlineKind::Code, true},                   {"#", "#", InlineKind::Mark},
      {"^", "^", InlineKind::Superscript, false, false, true}, {"~", "~", InlineKind::Subscript, false, false, true},
  };
  syntax.hook = [&context](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    // Monospace: passthroughs inside are unwrapped ("`a++_++b`").
    if (rest.starts_with("``") || (rest.starts_with("`") && rest.size() > 2 && !util::is_ascii_space(rest[1]))) {
      const std::size_t n = rest.starts_with("``") ? 2 : 1;
      const std::size_t end = rest.find(std::string(n, '`'), n);
      if (end != std::string_view::npos && end > n) {
        std::string code(rest.substr(n, end - n));
        bool unwrapped = false;
        for (const std::string_view wrap : {"+++", "++"}) {
          if (unwrapped) break;
          for (std::size_t open = code.find(wrap); open != std::string::npos; open = code.find(wrap, open)) {
            const std::size_t close = code.find(wrap, open + wrap.size());
            if (close == std::string::npos) break;
            code.erase(close, wrap.size());
            code.erase(open, wrap.size());
            open = close - wrap.size();
            unwrapped = true;
          }
        }
        if (!unwrapped && code.size() >= 2 && code.front() == '+' && code.back() == '+') code = code.substr(1, code.size() - 2);
        out.push_back(ir::Code{std::move(code)});
        return end + n;
      }
    }
    // Passthroughs.
    if (rest.starts_with("+++") || rest.starts_with("++") || rest.starts_with("+")) {
      const std::size_t n = rest.starts_with("+++") ? 3 : rest.starts_with("++") ? 2 : 1;
      if (n == 1 && (rest.size() < 3 || util::is_ascii_space(rest[1]))) return 0;
      const std::size_t end = rest.find(std::string(n, '+'), n);
      if (end == std::string_view::npos || end == n) return 0;
      append_text(out, rest.substr(n, end - n));
      return end + n;
    }
    if (rest.starts_with("pass:")) {
      const std::size_t open = rest.find('[');
      const std::size_t close = rest.find(']', open);
      if (open == std::string_view::npos || close == std::string_view::npos || open > 10) return 0;
      append_text(out, rest.substr(open + 1, close - open - 1));
      return close + 1;
    }
    if (rest.starts_with("[") && parser.depth() < 20) {
      // [line-through]#x#, [.underline]#x#
      const std::size_t close = rest.find("]#");
      if (close != std::string_view::npos && close < 40) {
        const std::size_t end = rest.find('#', close + 2);
        if (end != std::string_view::npos) {
          const std::string role(util::trim(rest.substr(1, close - 1)));
          ir::Inlines kids = parser.parse(rest.substr(close + 2, end - close - 2), parser.depth() + 1);
          if (role.find("line-through") != std::string::npos) out.push_back(ir::Strikethrough{std::move(kids)});
          else if (role.find("underline") != std::string::npos) out.push_back(ir::Underline{std::move(kids)});
          else {
            ir::Attributes attr;
            attr.add_class("mark");
            out.push_back(ir::Inline(ir::Strong{std::move(kids)}, std::move(attr)));
          }
          return end + 1;
        }
      }
    }
    for (const std::string_view macro : {"footnote:", "stem:", "latexmath:", "image:", "link:", "mailto:", "kbd:"}) {
      if (!rest.starts_with(macro)) continue;
      const std::size_t open = rest.find('[', macro.size());
      if (open == std::string_view::npos) return 0;
      std::size_t close = open + 1;
      int depth = 1;
      while (close < rest.size()) {
        if (rest[close] == '\\' && close + 1 < rest.size()) {
          close += 2;
          continue;
        }
        if (rest[close] == '[') ++depth;
        if (rest[close] == ']' && --depth == 0) break;
        ++close;
      }
      if (close >= rest.size()) return 0;
      const std::string target(rest.substr(macro.size(), open - macro.size()));
      const std::string_view inside = rest.substr(open + 1, close - open - 1);
      if (macro == "footnote:") {
        ir::Inlines body = parser.parse(inside, parser.depth() + 1);
        out.push_back(context.add_note(wiki::paragraph_blocks(std::move(body))));
      } else if (macro == "stem:" || macro == "latexmath:") {
        ir::Code math{std::string(inside)};
        math.math = ir::MathMode::Inline;
        out.push_back(std::move(math));
      } else if (macro == "image:") {
        std::string alt(util::trim(inside.substr(0, inside.find(','))));
        out.push_back(ir::InlineImage{wiki::image_from_url(target), alt});
      } else if (macro == "kbd:") {
        out.push_back(ir::Code{std::string(inside)});
      } else {
        const std::string url = macro == "mailto:" ? "mailto:" + target : target;
        ir::Inlines label = inside.empty() ? ir::Inlines{ir::Text{target}} : parser.parse(inside, parser.depth() + 1);
        out.push_back(ir::Link{url, {}, std::move(label)});
      }
      return close + 1;
    }
    if (rest.starts_with("<<")) {
      const std::size_t end = rest.find(">>");
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(2, end - 2);
      const std::size_t comma = inner.find(',');
      const std::string id(util::trim(inner.substr(0, comma)));
      ir::Inlines label = comma == std::string_view::npos ? ir::Inlines{ir::Text{id}}
                                                          : parser.parse(util::trim(inner.substr(comma + 1)), parser.depth() + 1);
      out.push_back(ir::Link{"#" + id, {}, std::move(label)});
      return end + 2;
    }
    if (rest.starts_with(" +\n") || rest == " +") {
      out.push_back(ir::LineBreak{});
      return rest.size() >= 3 ? 3 : 2;
    }
    if (rest.starts_with("{")) {
      static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> attributes = {{
          {"{empty}", ""}, {"{nbsp}", "\xC2\xA0"}, {"{sp}", " "}, {"{plus}", "+"}, {"{amp}", "&"},
          {"{lt}", "<"}, {"{gt}", ">"}, {"{brvbar}", "|"}, {"{vbar}", "|"},
      }};
      for (const auto& [name, value] : attributes) {
        if (rest.starts_with(name)) {
          append_text(out, value);
          return name.size();
        }
      }
    }
    if (const std::size_t length = wiki::bare_url_length(text, pos); length > 0) {
      const std::string url(text.substr(pos, length));
      if (pos + length < text.size() && text[pos + length] == '[') {
        const std::size_t close = text.find(']', pos + length);
        if (close != std::string_view::npos) {
          const std::string_view inside = text.substr(pos + length + 1, close - pos - length - 1);
          ir::Inlines label = inside.empty() ? ir::Inlines{ir::Text{url}} : parser.parse(inside, parser.depth() + 1);
          out.push_back(ir::Link{url, {}, std::move(label)});
          return close + 1 - pos;
        }
      }
      out.push_back(ir::Link{url, {}, {ir::Text{url}}});
      return length;
    }
    return 0;
  };
  return syntax;
}

ir::Document read_asciidoc(std::string_view text) {
  AdocState state;
  wiki::ReadSyntax syntax;
  syntax.list_item = adoc_list;
  syntax.preformatted_line = [](std::string_view line) -> std::optional<std::string> {
    if (line.starts_with(" ") && !util::trim(line).empty() && !adoc_list(line)) return std::string(line.substr(1));
    return std::nullopt;
  };
  syntax.block = [&state](std::span<const std::string_view> lines, std::size_t at, ir::Blocks& out, wiki::Reader& reader) {
    return adoc_block(state, lines, at, out, reader);
  };
  syntax.inline_syntax = adoc_inline;
  ir::Document document;
  std::vector<std::string_view> lines = util::split_lines(text);
  std::size_t start = 0;
  // Document header: "= Title", then an optional author line.
  while (start < lines.size() && (is_blank(lines[start]) || util::trim(lines[start]).starts_with("//"))) ++start;
  if (start < lines.size() && lines[start].starts_with("= ")) {
    document.metadata.title = trim_copy(lines[start].substr(2));
    ++start;
    if (start < lines.size() && !is_blank(lines[start]) && !lines[start].starts_with(":") && !lines[start].starts_with("[")) {
      std::string author(util::trim(lines[start]));
      const std::size_t lt = author.find('<');
      if (lt != std::string::npos) author = std::string(util::trim(std::string_view(author).substr(0, lt)));
      document.metadata.author = author;
      ++start;
    }
  }
  wiki::Reader reader(syntax, document);
  document.blocks = reader.parse(std::span<const std::string_view>(lines).subspan(start));
  if (!document.footnotes.empty()) number_footnotes(document);
  return document;
}

// ---- writing ----------------------------------------------------------------------------------

std::string adoc_escape(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (std::string_view("*_`#^~[]{}\\+<").find(c) != std::string_view::npos) out.push_back('\\');
    out.push_back(c == '\n' ? ' ' : c);
  }
  // "https://x" would become a link.
  return util::replace_all(out, "://", "\\://");
}

InlineMarkup adoc_markup(const ir::Document* document) {
  InlineMarkup m;
  m.escape = adoc_escape;
  m.strong = {"**", "**"};
  m.emphasis = {"__", "__"};
  m.mark = {"##", "##"};
  m.superscript = {"^", "^"};
  m.subscript = {"~", "~"};
  m.strikethrough = {"[.line-through]#", "#"};
  m.underline = {"[.underline]#", "#"};
  m.line_break = " +\n";
  m.soft_break = " ";
  m.code = [](const ir::Code& code) -> std::string {
    if (code.math != ir::MathMode::None) return "stem:[" + util::replace_all(code.text, "]", "\\]") + "]";
    if (code.raw_format == "adoc") return code.text;
    if (code.raw_format == "html") return "+++" + code.text + "+++";
    if (!code.raw_format.empty()) return {};
    std::string text = code.text;
    for (char& c : text) {
      if (c == '\n') c = ' ';
    }
    if (text.find("++") == std::string::npos && text.find('`') == std::string::npos) return "``++" + text + "++``";
    if (text.find("+++") == std::string::npos && text.find('`') == std::string::npos) return "``+++" + text + "+++``";
    return adoc_escape(text);
  };
  m.link = [](const ir::Link& link, const std::string& label) -> std::string {
    std::string url = link.url;
    std::erase_if(url, [](char c) { return util::is_ascii_space(c) || c == '['; });
    std::string text = util::replace_all(label, "]", "\\]");
    if (url.starts_with("#")) return "<<" + url.substr(1) + "," + text + ">>";
    if (url.starts_with("http://") || url.starts_with("https://") || url.starts_with("ftp://")) return url + "[" + text + "]";
    if (url.starts_with("mailto:")) return url + "[" + text + "]";
    return "link:" + url + "[" + text + "]";
  };
  m.image = [](const ir::InlineImage& image) -> std::string {
    const std::string url = image_url(image.source, false);
    if (url.empty()) return adoc_escape(image.alt);
    std::string alt = image.alt;
    std::erase_if(alt, [](char c) { return c == ']' || c == ','; });
    return "image:" + url + "[" + alt + "]";
  };
  if (document != nullptr) {
    m.note = [document](const ir::Link&, std::size_t index) -> std::string {
      if (index == 0 || index > document->footnotes.size()) return {};
      const InlineMarkup inner = adoc_markup(nullptr);
      std::string body;
      for (const ir::Block& b : document->footnotes[index - 1].blocks) {
        if (const auto* p = b.get_if<ir::Paragraph>()) {
          if (!body.empty()) body.append(" ");
          body.append(render_inlines(p->content, inner, document));
        }
      }
      return "footnote:[" + util::replace_all(body, "]", "\\]") + "]";
    };
  }
  return m;
}

Result<std::string> write_asciidoc(const ir::Document& document, WriteContext& ctx) {
  wiki::WriteSyntax syntax;
  syntax.raw_format = "adoc";
  syntax.heading = [](int level, const std::string& text, const ir::Attributes& attr) {
    std::string out;
    if (!attr.id.empty()) out = "[[" + attr.id + "]]\n";
    return out + wiki::repeat("=", std::min(level + 1, 6)) + " " + text;
  };
  syntax.list_marker = [](const std::vector<bool>& path, int, ir::TaskState task) {
    std::string out = wiki::repeat(path.back() ? "." : "*", static_cast<int>(path.size())) + " ";
    if (task == ir::TaskState::Done) out += "[x] ";
    if (task == ir::TaskState::Open) out += "[ ] ";
    return out;
  };
  syntax.definition = [](const std::string& term, const std::vector<std::string>& definitions) {
    std::string out = term + "::";
    for (std::size_t k = 0; k < definitions.size(); ++k) out += (k == 0 ? "\n  " : " +\n  ") + definitions[k];
    return out;
  };
  syntax.code_block = [](const ir::CodeBlock& code) -> std::string {
    std::size_t length = 4;
    for (const std::string_view line : util::split(code.code, '\n')) {
      if (is_repeated(line, '-', 4)) length = std::max(length, util::trim(line).size() + 1);
    }
    const std::string fence(length, '-');
    if (code.math != ir::MathMode::None) return "[stem]\n++++\n" + code.code + "\n++++";
    std::string lang = code.language;
    std::erase_if(lang, [](char c) { return c == ',' || c == ']' || util::is_ascii_space(c); });
    return (lang.empty() ? std::string("[listing]") : "[source," + lang + "]") + "\n" + fence + "\n" + code.code + "\n" + fence;
  };
  syntax.quote = [](const std::string& inner) { return "____\n" + inner + "\n____"; };
  syntax.rule = "'''";
  syntax.table = [](const ir::TableBlock& table, const std::function<std::string(const ir::Inlines&)>& render) {
    const TableGrid grid = table_grid(table);
    std::string cols;
    for (std::size_t c = 0; c < grid.columns; ++c) {
      if (!cols.empty()) cols.push_back(',');
      switch (table.column_alignment(c)) {
        case ir::Alignment::Left: cols.push_back('<'); break;
        case ir::Alignment::Right: cols.push_back('>'); break;
        case ir::Alignment::Center: cols.push_back('^'); break;
        case ir::Alignment::Default: cols.push_back('1'); break;
      }
    }
    std::string out;
    if (!table.caption.empty()) out += "." + render(table.caption) + "\n";
    out += "[cols=\"" + cols + "\"" + (header_row_count(table) > 0 ? ",options=\"header\"" : "") + "]\n|===\n";
    for (const ir::TableRow& row : table.rows) {
      for (std::size_t k = 0; k < row.cells.size(); ++k) {
        const ir::TableCell& cell = row.cells[k];
        if (k > 0) out.push_back(' ');
        if (cell.colspan > 1 && cell.rowspan > 1) out += std::to_string(cell.colspan) + "." + std::to_string(cell.rowspan) + "+";
        else if (cell.colspan > 1) out += std::to_string(cell.colspan) + "+";
        else if (cell.rowspan > 1) out += "." + std::to_string(cell.rowspan) + "+";
        out += "|" + util::replace_all(render(cell.content), "|", "\\|");
      }
      out += "\n";
    }
    return out + "|===";
  };
  syntax.image_block = [](const ir::ImageBlock& image, const std::string& url) -> std::string {
    if (url.starts_with("data:")) return adoc_escape(image.caption.empty() ? image.alt : image.caption);
    std::string out;
    if (!image.caption.empty()) out = "." + image.caption + "\n";
    std::string alt = image.alt;
    std::erase_if(alt, [](char c) { return c == ']' || c == ','; });
    return out + "image::" + url + "[" + alt + "]";
  };
  syntax.item_break = " +\n";
  syntax.escape_line_start = [](const std::string& line) -> std::string {
    if (line.empty()) return line;
    if (adoc_heading(line) || adoc_list(line) || line.starts_with(":") || line.starts_with("[") ||
        line.starts_with(".") || line.starts_with("//") || line.starts_with("'''") || line.starts_with("<<<") ||
        line.starts_with("----") || line.starts_with("____") || line.starts_with("====") || line.starts_with("|===") ||
        line.starts_with("= ")) {
      return "{empty}" + line;
    }
    for (const std::string_view kind : {"NOTE: ", "TIP: ", "IMPORTANT: ", "WARNING: ", "CAUTION: "}) {
      if (line.starts_with(kind)) return "{empty}" + line;
    }
    return line;
  };
  wiki::Writer writer(document, syntax, adoc_markup(&document), static_cast<int>(ctx.options.get_int("wrap_width", 0, 0, 1000)));
  // Quotes and admonitions keep their kind.
  std::string out;
  const ir::DocumentMetadata& meta = document.metadata;
  if (!meta.title.empty()) {
    out += "= " + meta.title + "\n";
    if (!meta.author.empty()) out += ":author: " + meta.author + "\n";
    if (!meta.date.empty()) out += ":revdate: " + meta.date + "\n";
    if (!meta.language.empty()) out += ":lang: " + meta.language + "\n";
    out += "\n";
  }
  return out + writer.run();
}

int sniff_asciidoc(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"adoc", "asciidoc"})) return 80;
  const std::string_view head = sniff_head(data);
  int score = 0;
  if (head.starts_with("= ")) score += 2;
  if (head.find("\n== ") != std::string_view::npos) score += 2;
  if (head.find("[source,") != std::string_view::npos) score += 3;
  if (head.find("\n|===") != std::string_view::npos) score += 3;
  if (head.find("\n:toc:") != std::string_view::npos || head.find("\n:author:") != std::string_view::npos) score += 2;
  return score >= 5 ? 68 : 0;
}

}  // namespace

void register_asciidoc(Registry& registry) {
  MarkupFormat adoc;
  adoc.id = "adoc";
  adoc.name = "AsciiDoc";
  adoc.description = "AsciiDoc documentation markup (Asciidoctor syntax).";
  adoc.extensions = {"adoc", "asciidoc"};
  adoc.mime = "text/asciidoc";
  adoc.popularity = 30;
  adoc.sniff = sniff_asciidoc;
  adoc.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_asciidoc(text); };
  adoc.write = write_asciidoc;
  adoc.features = feature_wrap;
  register_markup(registry, std::move(adoc));
}

}  // namespace cvt::document
