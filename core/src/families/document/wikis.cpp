// Convertible core - document family: MediaWiki, DokuWiki, Creole and Jira
// wiki markup, built on the shared wiki engine.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/html_internal.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

using wiki::Cell;
using wiki::Heading;
using wiki::ListItem;

// =========================================================================== shared helpers

// "<tag ...>inner</tag>" at text[pos] for a tag in `names`: (name, attributes, inner, total length).
struct TagMatch {
  std::string name;
  std::string attributes;
  std::string_view inner;
  std::size_t length = 0;
};

std::optional<TagMatch> match_html_pair(std::string_view text, std::size_t pos, std::initializer_list<std::string_view> names) {
  if (pos >= text.size() || text[pos] != '<') return std::nullopt;
  const std::size_t gt = text.find('>', pos);
  if (gt == std::string_view::npos) return std::nullopt;
  std::string_view open = text.substr(pos + 1, gt - pos - 1);
  if (open.ends_with("/")) return std::nullopt;
  std::size_t name_end = 0;
  while (name_end < open.size() && (util::is_ascii_alpha(open[name_end]) || util::is_ascii_digit(open[name_end]))) ++name_end;
  const std::string name = util::to_lower(open.substr(0, name_end));
  if (std::ranges::find(names, name) == names.end()) return std::nullopt;
  const std::string close = "</" + name + ">";
  // Case-insensitive search for the closing tag.
  const std::string lowered = util::to_lower(text.substr(gt + 1));
  const std::size_t end = lowered.find(close);
  if (end == std::string::npos) return std::nullopt;
  TagMatch match;
  match.name = name;
  match.attributes = std::string(util::trim(open.substr(name_end)));
  match.inner = text.substr(gt + 1, end);
  match.length = gt + 1 + end + close.size() - pos;
  return match;
}

std::string attribute_value(std::string_view attributes, std::string_view name) {
  const std::string lowered = util::to_lower(attributes);
  std::size_t at = lowered.find(std::string(name) + "=");
  if (at == std::string::npos) return {};
  at += name.size() + 1;
  if (at < attributes.size() && (attributes[at] == '"' || attributes[at] == '\'')) {
    const char quote = attributes[at];
    const std::size_t close = attributes.find(quote, at + 1);
    return std::string(attributes.substr(at + 1, close == std::string_view::npos ? std::string_view::npos : close - at - 1));
  }
  std::size_t end = at;
  while (end < attributes.size() && !util::is_ascii_space(attributes[end]) && attributes[end] != '|') ++end;
  return std::string(attributes.substr(at, end - at));
}

ir::Alignment alignment_from_style(std::string_view attributes) {
  const std::string lowered = util::to_lower(attributes);
  if (lowered.find("text-align: right") != std::string::npos || lowered.find("text-align:right") != std::string::npos ||
      lowered.find("align=\"right\"") != std::string::npos) {
    return ir::Alignment::Right;
  }
  if (lowered.find("text-align: center") != std::string::npos || lowered.find("text-align:center") != std::string::npos ||
      lowered.find("align=\"center\"") != std::string::npos) {
    return ir::Alignment::Center;
  }
  if (lowered.find("text-align: left") != std::string::npos || lowered.find("text-align:left") != std::string::npos ||
      lowered.find("align=\"left\"") != std::string::npos) {
    return ir::Alignment::Left;
  }
  return ir::Alignment::Default;
}

std::string align_style(ir::Alignment a) {
  switch (a) {
    case ir::Alignment::Left: return "text-align: left;";
    case ir::Alignment::Center: return "text-align: center;";
    case ir::Alignment::Right: return "text-align: right;";
    case ir::Alignment::Default: return {};
  }
  return {};
}

// Simple HTML-like inline tags shared by several wikis.
std::size_t html_inline(InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) {
  if (text[pos] != '<' || parser.depth() > 20) return 0;
  const std::string_view rest = text.substr(pos);
  for (const std::string_view br : {"<br>", "<br/>", "<br />", "<BR>", "<br/ >"}) {
    if (rest.starts_with(br)) {
      out.push_back(ir::LineBreak{});
      return br.size();
    }
  }
  if (rest.starts_with("<!--")) {
    const std::size_t end = rest.find("-->");
    return end == std::string_view::npos ? 0 : end + 3;
  }
  const auto match = match_html_pair(text, pos, {"b", "strong", "i", "em", "u", "ins", "s", "del", "strike", "sup", "sub",
                                                 "code", "tt", "kbd", "span", "small", "big", "mark", "nowiki", "math"});
  if (!match) return 0;
  const std::string& n = match->name;
  if (n == "nowiki") {
    std::string decoded = html::decode_references(match->inner, false);
    append_text(out, decoded);
  } else if (n == "code" || n == "tt" || n == "kbd") {
    std::string inner(match->inner);
    inner = util::replace_all(util::replace_all(inner, "<nowiki>", ""), "</nowiki>", "");
    out.push_back(ir::Code{html::decode_references(inner, false)});
  } else if (n == "math") {
    ir::Code math{std::string(util::trim(match->inner))};
    math.math = attribute_value(match->attributes, "display") == "block" ? ir::MathMode::Display : ir::MathMode::Inline;
    out.push_back(std::move(math));
  } else {
    ir::Inlines children = parser.parse(match->inner, parser.depth() + 1);
    if (n == "b" || n == "strong") out.push_back(ir::Strong{std::move(children)});
    else if (n == "i" || n == "em") out.push_back(ir::Emphasis{std::move(children)});
    else if (n == "u" || n == "ins") out.push_back(ir::Underline{std::move(children)});
    else if (n == "s" || n == "del" || n == "strike") out.push_back(ir::Strikethrough{std::move(children)});
    else if (n == "sup") out.push_back(ir::Superscript{std::move(children)});
    else if (n == "sub") out.push_back(ir::Subscript{std::move(children)});
    else if (n == "mark") {
      ir::Attributes attr;
      attr.add_class("mark");
      out.push_back(ir::Inline(ir::Strong{std::move(children)}, std::move(attr)));
    } else {
      for (ir::Inline& i : children) out.push_back(std::move(i));
    }
  }
  return match->length;
}

bool is_absolute_url(std::string_view url) {
  const std::size_t colon = url.find(':');
  if (colon == std::string_view::npos || colon == 0) return false;
  return std::ranges::all_of(url.substr(0, colon), [](char c) { return util::is_ascii_alpha(c) || c == '+' || c == '-' || c == '.'; });
}

std::string image_alt(const ir::InlineImage& image) { return image.alt; }

// Leading spaces of block content, as lines.
std::vector<std::string_view> block_lines(std::string_view text) { return util::split_lines(text); }

// =========================================================================== MediaWiki

std::optional<Heading> mediawiki_heading(std::string_view line) {
  const std::string_view t = util::trim(line);
  if (t.size() < 3 || t.front() != '=' || t.back() != '=') return std::nullopt;
  std::size_t open = 0;
  while (open < t.size() && t[open] == '=') ++open;
  std::size_t close = 0;
  while (close < t.size() && t[t.size() - 1 - close] == '=') ++close;
  const std::size_t level = std::min(open, close);
  if (level == 0 || level > 6 || 2 * level >= t.size()) return std::nullopt;
  return Heading{static_cast<int>(level), std::string(util::trim(t.substr(level, t.size() - 2 * level))), {}};
}

std::optional<ListItem> mediawiki_list(std::string_view line) {
  if (line.empty()) return std::nullopt;
  std::size_t n = 0;
  while (n < line.size() && (line[n] == '*' || line[n] == '#' || line[n] == ':' || line[n] == ';')) ++n;
  if (n == 0) return std::nullopt;
  ListItem item;
  item.depth = static_cast<int>(n);
  const char last = line[n - 1];
  item.ordered = last == '#';
  item.term = last == ';';
  item.definition = last == ':';
  item.text = std::string(util::trim(line.substr(n)));
  return item;
}

std::vector<Cell> mediawiki_cells(std::string_view content, std::string_view separator, bool header) {
  std::vector<Cell> cells;
  std::size_t start = 0;
  while (start <= content.size()) {
    std::size_t end = content.find(separator, start);
    if (end == std::string_view::npos) end = content.size();
    std::string_view part = content.substr(start, end - start);
    Cell cell;
    cell.header = header;
    // "attributes | content" unless the bar is inside [[...]].
    const std::size_t bar = part.find('|');
    const std::size_t link = part.find("[[");
    if (bar != std::string_view::npos && (link == std::string_view::npos || bar < link) &&
        part.substr(0, bar).find('=') != std::string_view::npos) {
      const std::string_view attributes = part.substr(0, bar);
      cell.colspan = static_cast<int>(util::parse_int(attribute_value(attributes, "colspan")).value_or(1));
      cell.rowspan = static_cast<int>(util::parse_int(attribute_value(attributes, "rowspan")).value_or(1));
      cell.align = alignment_from_style(attributes);
      part = part.substr(bar + 1);
    }
    cell.text = std::string(util::trim(part));
    cells.push_back(std::move(cell));
    if (end >= content.size()) break;
    start = end + separator.size();
  }
  return cells;
}

std::size_t mediawiki_block(std::span<const std::string_view> lines, std::size_t at, ir::Blocks& out, wiki::Reader& reader) {
  const std::string_view line = util::trim(lines[at]);
  // Anchors written before headings: <span id="x"></span>
  if (line.starts_with("<span id=\"") && line.ends_with("></span>") && at + 1 < lines.size()) {
    if (auto heading = mediawiki_heading(lines[at + 1])) {
      ir::Attributes attr;
      attr.id = attribute_value(line, "id");
      out.push_back(ir::Block(ir::Heading{heading->level, reader.inlines(heading->text)}, std::move(attr)));
      return 2;
    }
    return 1;
  }
  if (line == "<references />" || line == "<references/>" || line == "__TOC__" || line == "__NOTOC__" ||
      line == "<div class=\"figure\">" || line == "</div>" || (line.starts_with("[[Category:") && line.ends_with("]]"))) {
    return 1;
  }
  // Preformatted and highlighted code.
  for (const std::string_view tag : {"syntaxhighlight", "source", "pre"}) {
    const std::string open = "<" + std::string(tag);
    if (!util::istarts_with(line, open)) continue;
    const std::string close = "</" + std::string(tag) + ">";
    std::string content;
    std::size_t i = at;
    std::string_view first = lines[at];
    const std::size_t gt = first.find('>');
    if (gt == std::string_view::npos) return 0;
    const std::string attributes(first.substr(open.size(), gt - open.size()));
    std::string_view rest = first.substr(gt + 1);
    bool done = false;
    while (true) {
      const std::string lowered = util::to_lower(rest);
      const std::size_t end = lowered.find(close);
      if (end != std::string::npos) {
        content.append(rest.substr(0, end));
        done = true;
        break;
      }
      content.append(rest);
      if (++i >= lines.size()) break;
      content.push_back('\n');
      rest = lines[i];
    }
    if (!done) return 0;
    if (content.starts_with("\n")) content.erase(0, 1);
    if (content.ends_with("\n")) content.pop_back();
    ir::CodeBlock code;
    code.language = attribute_value(attributes, "lang");
    code.code = tag == "pre" ? html::decode_references(content, false) : content;
    if (tag == "pre" && code.code.starts_with("<nowiki>") && code.code.ends_with("</nowiki>")) {
      code.code = code.code.substr(8, code.code.size() - 17);
    }
    out.push_back(std::move(code));
    return i - at + 1;
  }
  if (util::istarts_with(line, "<blockquote")) {
    std::string content;
    std::size_t i = at;
    std::string_view rest = lines[at].substr(lines[at].find('>') + 1);
    while (true) {
      const std::size_t end = util::to_lower(rest).find("</blockquote>");
      if (end != std::string::npos) {
        content.append(rest.substr(0, end));
        break;
      }
      content.append(rest).push_back('\n');
      if (++i >= lines.size()) return 0;
      rest = lines[i];
    }
    const std::vector<std::string_view> inner = block_lines(content);
    out.push_back(ir::BlockQuote{reader.parse(inner, 1)});
    return i - at + 1;
  }
  // Tables {| ... |}
  if (line.starts_with("{|")) {
    ir::TableBlock table;
    std::size_t i = at + 1;
    std::vector<Cell> row;
    bool in_row = false;
    const auto end_row = [&] {
      if (row.empty()) return;
      ir::TableRow r;
      bool header = true;
      for (Cell& c : row) {
        if (!c.header) header = false;
        r.cells.push_back(ir::TableCell{reader.inlines(c.text), std::clamp(c.colspan, 1, 1000), std::clamp(c.rowspan, 1, 1000), c.align});
      }
      r.header = header && (table.rows.empty() || table.rows.back().header);
      table.rows.push_back(std::move(r));
      row.clear();
    };
    for (; i < lines.size(); ++i) {
      const std::string_view l = util::trim(lines[i]);
      if (l.starts_with("|}")) break;
      if (l.starts_with("|+")) {
        table.caption = reader.inlines(util::trim(l.substr(2)));
        continue;
      }
      if (l.starts_with("|-")) {
        end_row();
        in_row = true;
        continue;
      }
      if (l.starts_with("!")) {
        for (Cell& c : mediawiki_cells(l.substr(1), "!!", true)) row.push_back(std::move(c));
        continue;
      }
      if (l.starts_with("|")) {
        for (Cell& c : mediawiki_cells(l.substr(1), "||", false)) row.push_back(std::move(c));
        continue;
      }
      // Continuation of the last cell.
      if (!row.empty() && !l.empty()) row.back().text.append(" ").append(l);
    }
    (void)in_row;
    end_row();
    // Alignment per column when uniform.
    if (!table.rows.empty()) {
      const TableGrid grid = table_grid(table);
      std::vector<ir::Alignment> aligns(grid.columns, ir::Alignment::Default);
      bool any = false;
      for (std::size_t c = 0; c < grid.columns; ++c) {
        std::optional<ir::Alignment> common;
        for (const auto& r : grid.cells) {
          if (r[c] == nullptr) continue;
          if (!common) common = r[c]->align;
          if (*common != r[c]->align) common = ir::Alignment::Default;
        }
        if (common && *common != ir::Alignment::Default) {
          aligns[c] = *common;
          any = true;
        }
      }
      if (any) {
        table.alignments = aligns;
        for (ir::TableRow& r : table.rows) {
          for (ir::TableCell& c : r.cells) c.align = ir::Alignment::Default;
        }
      }
      out.push_back(std::move(table));
    }
    return std::min(i, lines.size() - 1) - at + 1;
  }
  return 0;
}

InlineSyntax mediawiki_inline(wiki::Context& context) {
  InlineSyntax syntax;
  syntax.rules = {
      {"'''", "'''", InlineKind::Strong, false, false, true},
      {"''", "''", InlineKind::Emphasis, false, false, true},
  };
  syntax.hook = [&context](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    if (rest.starts_with("'''''")) {
      const std::size_t end = text.find("'''''", pos + 5);
      if (end != std::string_view::npos && end > pos + 5 && parser.depth() < 20) {
        ir::Inlines inner = parser.parse(text.substr(pos + 5, end - pos - 5), parser.depth() + 1);
        out.push_back(ir::Strong{{ir::Emphasis{std::move(inner)}}});
        return end + 5 - pos;
      }
    }
    if (rest.starts_with("<ref")) {
      const std::size_t gt = rest.find('>');
      if (gt != std::string_view::npos && rest[gt - 1] == '/') return gt + 1;  // reused named reference
      const std::size_t end = rest.find("</ref>");
      if (gt != std::string_view::npos && end != std::string_view::npos && end > gt && parser.depth() < 20) {
        ir::Inlines body = parser.parse(rest.substr(gt + 1, end - gt - 1), parser.depth() + 1);
        out.push_back(context.add_note(wiki::paragraph_blocks(std::move(body))));
        return end + 6;
      }
    }
    if (const std::size_t n = html_inline(parser, text, pos, out)) return n;
    if (rest.starts_with("[[")) {
      const std::size_t end = rest.find("]]");
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(2, end - 2);
      const std::size_t bar = inner.find('|');
      const std::string target(util::trim(inner.substr(0, bar)));
      const std::string lowered = util::to_lower(target);
      if (lowered.starts_with("file:") || lowered.starts_with("image:")) {
        std::string alt;
        if (bar != std::string_view::npos) {
          for (const std::string_view part : util::split(inner.substr(bar + 1), '|')) {
            const std::string p(util::trim(part));
            if (p == "thumb" || p == "thumbnail" || p == "frame" || p == "frameless" || p == "border" || p == "left" ||
                p == "right" || p == "center" || p == "none" || p.ends_with("px") || p.starts_with("alt=") ||
                p.starts_with("link=") || p.starts_with("upright")) {
              if (p.starts_with("alt=")) alt = p.substr(4);
              continue;
            }
            alt = p;
          }
        }
        out.push_back(ir::InlineImage{wiki::image_from_url(target.substr(target.find(':') + 1)), alt});
        return end + 2;
      }
      if (lowered.starts_with("category:")) return end + 2;
      ir::Inlines label = bar == std::string_view::npos ? ir::Inlines{ir::Text{target}}
                                                        : parser.parse(inner.substr(bar + 1), parser.depth() + 1);
      out.push_back(ir::Link{target, {}, std::move(label)});
      return end + 2;
    }
    if (rest.starts_with("[") && rest.size() > 1) {
      const std::size_t end = rest.find(']');
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(1, end - 1);
      const std::size_t space = inner.find(' ');
      const std::string url(inner.substr(0, space));
      if (!is_absolute_url(url)) return 0;
      ir::Inlines label = space == std::string_view::npos ? ir::Inlines{ir::Text{url}}
                                                          : parser.parse(inner.substr(space + 1), parser.depth() + 1);
      out.push_back(ir::Link{url, {}, std::move(label)});
      return end + 1;
    }
    if (rest.starts_with("{{") ) {
      // Templates are not expanded; their text is dropped.
      int depth = 0;
      for (std::size_t k = 0; k + 1 < rest.size(); ++k) {
        if (rest.substr(k).starts_with("{{")) {
          ++depth;
          ++k;
        } else if (rest.substr(k).starts_with("}}")) {
          if (--depth == 0) return k + 2;
          ++k;
        }
      }
      return 0;
    }
    return wiki::common_inline(text, pos, out);
  };
  return syntax;
}

ir::Document read_mediawiki_text(std::string_view text) {
  wiki::ReadSyntax syntax;
  syntax.heading = mediawiki_heading;
  syntax.list_item = mediawiki_list;
  syntax.rule = [](std::string_view line) { return util::trim(line).starts_with("----") && is_repeated(line, '-', 4); };
  syntax.preformatted_line = [](std::string_view line) -> std::optional<std::string> {
    if (line.size() > 1 && line[0] == ' ' && !util::trim(line).empty()) return std::string(line.substr(1));
    return std::nullopt;
  };
  syntax.block = mediawiki_block;
  syntax.inline_syntax = mediawiki_inline;
  syntax.list_item_continuation = false;
  return wiki::read(text, syntax);
}

// Escape text so that no character starts markup.
std::string mediawiki_escape(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    const char prev = i > 0 ? text[i - 1] : '\0';
    const bool risky = c == '<' || c == '>' || c == '&' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|' ||
                       (c == '\'' && (next == '\'' || prev == '\'')) || (c == '~' && (next == '~' || prev == '~')) ||
                       (c == '_' && (next == '_' || prev == '_')) || (c == ':' && false);
    if (risky) {
      out.append("&#").append(std::to_string(static_cast<int>(static_cast<unsigned char>(c)))).append(";");
    } else {
      out.push_back(c);
    }
  }
  // Bare URLs would become links.
  out = util::replace_all(out, "://", "&#58;//");
  return out;
}

std::string mediawiki_line_start(const std::string& line) {
  if (line.empty()) return line;
  const char c = line.front();
  if (c == '*' || c == '#' || c == ':' || c == ';' || c == '=' || c == ' ' || (c == '-' && line.starts_with("----"))) {
    return "&#" + std::to_string(static_cast<int>(static_cast<unsigned char>(c))) + ";" + line.substr(1);
  }
  return line;
}

InlineMarkup mediawiki_markup(const ir::Document* document) {
  InlineMarkup m;
  m.escape = mediawiki_escape;
  m.strong = {"'''", "'''"};
  m.emphasis = {"''", "''"};
  m.strikethrough = {"<s>", "</s>"};
  m.underline = {"<u>", "</u>"};
  m.superscript = {"<sup>", "</sup>"};
  m.subscript = {"<sub>", "</sub>"};
  m.mark = {"<mark>", "</mark>"};
  m.line_break = "<br />";
  m.soft_break = " ";
  m.code = [](const ir::Code& code) -> std::string {
    if (code.math != ir::MathMode::None) {
      return std::string(code.math == ir::MathMode::Display ? "<math display=\"block\">" : "<math>") +
             html::escape_text(code.text) + "</math>";
    }
    if (code.raw_format == "wiki") return code.text;
    if (!code.raw_format.empty()) return {};
    return "<code><nowiki>" + html::escape_text(code.text) + "</nowiki></code>";
  };
  m.link = [](const ir::Link& link, const std::string& label) -> std::string {
    if (is_absolute_url(link.url)) {
      if (label == mediawiki_escape(link.url) && link.url.find(' ') == std::string::npos) return link.url;
      std::string url = link.url;
      std::erase_if(url, [](char c) { return c == ' ' || c == ']' || c == '[' || c == '\n'; });
      return "[" + url + " " + (label.empty() ? url : label) + "]";
    }
    std::string target = link.url;
    std::erase_if(target, [](char c) { return c == '[' || c == ']' || c == '|' || c == '{' || c == '}' || c == '\n'; });
    return "[[" + target + "|" + label + "]]";
  };
  m.image = [](const ir::InlineImage& image) -> std::string {
    const std::string url = image_url(image.source, false);
    if (url.empty()) return mediawiki_escape(image_alt(image));
    return "[[File:" + url + "|" + mediawiki_escape(image.alt) + "]]";
  };
  if (document != nullptr) {
    m.note = [document](const ir::Link&, std::size_t index) -> std::string {
      if (index == 0 || index > document->footnotes.size()) return {};
      const InlineMarkup inner = mediawiki_markup(nullptr);
      std::string body;
      for (const ir::Block& b : document->footnotes[index - 1].blocks) {
        if (const auto* p = b.get_if<ir::Paragraph>()) {
          if (!body.empty()) body.append("<br />");
          body.append(render_inlines(p->content, inner, document));
        }
      }
      return "<ref>" + body + "</ref>";
    };
  }
  return m;
}

std::string mediawiki_table(const ir::TableBlock& table, const std::function<std::string(const ir::Inlines&)>& render) {
  std::string out = "{| class=\"wikitable\"\n";
  if (!table.caption.empty()) out.append("|+ ").append(render(table.caption)).append("\n");
  const TableGrid grid = table_grid(table);
  for (std::size_t r = 0; r < table.rows.size(); ++r) {
    out.append("|-\n");
    std::size_t column = 0;
    for (const ir::TableCell& cell : table.rows[r].cells) {
      while (column < grid.columns && grid.cells[r][column] != &cell) ++column;
      out.append(table.rows[r].header ? "! " : "| ");
      std::string attributes;
      if (cell.colspan > 1) attributes += "colspan=\"" + std::to_string(cell.colspan) + "\" ";
      if (cell.rowspan > 1) attributes += "rowspan=\"" + std::to_string(cell.rowspan) + "\" ";
      const ir::Alignment align = cell.align != ir::Alignment::Default ? cell.align : table.column_alignment(column);
      if (align != ir::Alignment::Default) attributes += "style=\"" + align_style(align) + "\" ";
      if (!attributes.empty()) out.append(attributes).append("| ");
      std::string text = render(cell.content);
      out.append(text.empty() ? std::string() : text).append("\n");
    }
  }
  out.append("|}");
  return out;
}

Result<std::string> write_mediawiki(const ir::Document& document, WriteContext& /*ctx*/) {
  wiki::WriteSyntax syntax;
  syntax.raw_format = "wiki";
  syntax.heading = [](int level, const std::string& text, const ir::Attributes& attr) {
    std::string out;
    if (!attr.id.empty()) out = "<span id=\"" + html::escape_attribute(attr.id) + "\"></span>\n";
    const std::string marks = wiki::repeat("=", level);
    return out + marks + " " + text + " " + marks;
  };
  syntax.list_marker = [](const std::vector<bool>& path, int, ir::TaskState task) {
    std::string out;
    for (const bool ordered : path) out.push_back(ordered ? '#' : '*');
    out.push_back(' ');
    if (task == ir::TaskState::Done) out += "\xE2\x98\x91 ";
    if (task == ir::TaskState::Open) out += "\xE2\x98\x90 ";
    return out;
  };
  syntax.definition = [](const std::string& term, const std::vector<std::string>& definitions) {
    std::string out = "; " + term;
    for (const std::string& d : definitions) out += "\n: " + d;
    return out;
  };
  syntax.code_block = [](const ir::CodeBlock& code) -> std::string {
    if (code.math != ir::MathMode::None) return "<math display=\"block\">" + html::escape_text(code.code) + "</math>";
    if (!code.language.empty() && util::to_lower(code.code).find("</syntaxhighlight") == std::string::npos) {
      std::string lang = code.language;
      std::erase_if(lang, [](char c) { return !(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '+' || c == '-' || c == '_'); });
      return "<syntaxhighlight lang=\"" + lang + "\">\n" + code.code + "\n</syntaxhighlight>";
    }
    return "<pre>" + html::escape_text(code.code) + "</pre>";
  };
  syntax.quote = [](const std::string& inner) { return "<blockquote>\n" + inner + "\n</blockquote>"; };
  syntax.rule = "----";
  syntax.table = mediawiki_table;
  syntax.image_block = [](const ir::ImageBlock& image, const std::string& url) -> std::string {
    if (url.starts_with("data:")) return mediawiki_escape(image.caption.empty() ? image.alt : image.caption);
    if (!image.caption.empty()) return "[[File:" + url + "|thumb|" + mediawiki_escape(image.caption) + "]]";
    return "[[File:" + url + "|" + mediawiki_escape(image.alt) + "]]";
  };
  syntax.notes_section = [](const std::vector<std::string>&) { return std::string("<references />"); };
  syntax.item_break = "<br />";
  syntax.escape_line_start = mediawiki_line_start;
  wiki::Writer writer(document, syntax, mediawiki_markup(&document), 0);
  return writer.run();
}

int sniff_mediawiki(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"wiki", "mediawiki", "mw"})) return 80;
  const std::string_view head = sniff_head(data);
  int score = 0;
  if (head.find("{| class=\"wikitable\"") != std::string_view::npos) score += 3;
  if (head.find("'''") != std::string_view::npos) score += 1;
  if (head.find("[[File:") != std::string_view::npos || head.find("[[Category:") != std::string_view::npos) score += 2;
  if (head.find("<ref>") != std::string_view::npos || head.find("<references") != std::string_view::npos) score += 2;
  if (head.find("\n== ") != std::string_view::npos || head.starts_with("== ")) score += 2;
  if (head.find("<syntaxhighlight") != std::string_view::npos) score += 2;
  return score >= 5 ? 66 : 0;
}

// =========================================================================== DokuWiki

std::optional<Heading> dokuwiki_heading(std::string_view line) {
  const std::string_view t = util::trim(line);
  if (t.size() < 5 || !t.starts_with("==") || !t.ends_with("==")) return std::nullopt;
  std::size_t open = 0;
  while (open < t.size() && t[open] == '=') ++open;
  std::size_t close = 0;
  while (close < t.size() && t[t.size() - 1 - close] == '=') ++close;
  if (open < 2 || open > 6 || open + close >= t.size()) return std::nullopt;
  return Heading{7 - static_cast<int>(open), std::string(util::trim(t.substr(open, t.size() - open - close))), {}};
}

std::optional<ListItem> dokuwiki_list(std::string_view line) {
  std::size_t spaces = 0;
  while (spaces < line.size() && (line[spaces] == ' ' || line[spaces] == '\t')) spaces += 1;
  if (spaces < 2 || spaces + 1 >= line.size()) return std::nullopt;
  const char marker = line[spaces];
  if ((marker != '*' && marker != '-') || (line[spaces + 1] != ' ' && line[spaces + 1] != '\t')) return std::nullopt;
  ListItem item;
  item.depth = static_cast<int>(indentation(line) / 2);
  item.ordered = marker == '-';
  item.text = std::string(util::trim(line.substr(spaces + 1)));
  return item;
}

std::optional<std::vector<Cell>> dokuwiki_row(std::string_view line) {
  const std::string_view t = util::trim_right(line);
  if (t.empty() || (t.front() != '^' && t.front() != '|')) return std::nullopt;
  std::vector<Cell> cells;
  std::size_t i = 0;
  while (i < t.size()) {
    const char separator = t[i];
    std::size_t end = i + 1;
    while (end < t.size() && t[end] != '^' && t[end] != '|') {
      // Do not split inside [[...]] or {{...}}.
      if (t.substr(end).starts_with("[[")) {
        const std::size_t close = t.find("]]", end);
        end = close == std::string_view::npos ? t.size() : close + 2;
        continue;
      }
      if (t.substr(end).starts_with("{{")) {
        const std::size_t close = t.find("}}", end);
        end = close == std::string_view::npos ? t.size() : close + 2;
        continue;
      }
      ++end;
    }
    if (end >= t.size()) break;  // the trailing separator
    const std::string_view raw = t.substr(i + 1, end - i - 1);
    if (raw.empty() && !cells.empty()) {
      ++cells.back().colspan;  // "||" extends the previous cell
    } else {
      Cell cell;
      cell.header = separator == '^';
      const std::size_t left = raw.size() - util::trim_left(raw).size();
      const std::size_t right = raw.size() - util::trim_right(raw).size();
      if (left >= 2 && right >= 2) cell.align = ir::Alignment::Center;
      else if (left >= 2) cell.align = ir::Alignment::Right;
      else if (right >= 2) cell.align = ir::Alignment::Left;
      cell.text = std::string(util::trim(raw));
      if (cell.text == ":::") cell.text.clear();  // row span marker
      cells.push_back(std::move(cell));
    }
    i = end;
  }
  if (cells.empty()) return std::nullopt;
  return cells;
}

std::size_t dokuwiki_block(std::span<const std::string_view> lines, std::size_t at, ir::Blocks& out, wiki::Reader&) {
  const std::string_view line = util::trim(lines[at]);
  if (line == "~~NOTOC~~" || line == "~~NOCACHE~~") return 1;
  for (const std::string_view tag : {"code", "file", "html"}) {
    const std::string open = "<" + std::string(tag);
    if (!line.starts_with(open) || (line.size() > open.size() && line[open.size()] != '>' && line[open.size()] != ' ')) continue;
    const std::string close = "</" + std::string(tag) + ">";
    const std::size_t gt = lines[at].find('>');
    if (gt == std::string_view::npos) return 0;
    const std::string_view attributes = util::trim(lines[at].substr(lines[at].find(open) + open.size(), gt - lines[at].find(open) - open.size()));
    std::string content;
    std::size_t i = at;
    std::string_view rest = lines[at].substr(gt + 1);
    bool done = false;
    while (true) {
      const std::size_t end = rest.find(close);
      if (end != std::string_view::npos) {
        content.append(rest.substr(0, end));
        done = true;
        break;
      }
      content.append(rest);
      if (++i >= lines.size()) break;
      content.push_back('\n');
      rest = lines[i];
    }
    if (!done) return 0;
    if (content.starts_with("\n")) content.erase(0, 1);
    if (content.ends_with("\n")) content.pop_back();
    ir::CodeBlock code;
    if (tag == "html") {
      code.raw_format = "html";
    } else {
      code.language = std::string(attributes.substr(0, attributes.find(' ')));
      if (code.language == "-") code.language.clear();
    }
    code.code = std::move(content);
    out.push_back(std::move(code));
    return i - at + 1;
  }
  return 0;
}

InlineSyntax dokuwiki_inline(wiki::Context& context) {
  InlineSyntax syntax;
  syntax.rules = {
      {"**", "**", InlineKind::Strong, false, false, true},
      {"//", "//", InlineKind::Emphasis, false, false, true},
      {"__", "__", InlineKind::Underline, false, false, true},
  };
  syntax.hook = [&context](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    if (rest.starts_with("%%")) {
      const std::size_t end = rest.find("%%", 2);
      if (end == std::string_view::npos) return 0;
      append_text(out, rest.substr(2, end - 2));
      return end + 2;
    }
    if (rest.starts_with("''")) {
      const std::size_t end = rest.find("''", 2);
      if (end == std::string_view::npos || end == 2) return 0;
      std::string code(rest.substr(2, end - 2));
      code = util::replace_all(code, "%%", "");
      out.push_back(ir::Code{std::move(code)});
      return end + 2;
    }
    if (rest.starts_with("\\\\") && (rest.size() == 2 || rest[2] == ' ' || rest[2] == '\n')) {
      out.push_back(ir::LineBreak{});
      return rest.size() > 2 ? 3 : 2;
    }
    if (rest.starts_with("((")) {
      const std::size_t end = rest.find("))");
      if (end == std::string_view::npos || parser.depth() > 20) return 0;
      ir::Inlines body = parser.parse(util::trim(rest.substr(2, end - 2)), parser.depth() + 1);
      out.push_back(context.add_note(wiki::paragraph_blocks(std::move(body))));
      return end + 2;
    }
    if (rest.starts_with("[[")) {
      const std::size_t end = rest.find("]]");
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(2, end - 2);
      const std::size_t bar = inner.find('|');
      const std::string target(util::trim(inner.substr(0, bar)));
      ir::Inlines label = bar == std::string_view::npos ? ir::Inlines{ir::Text{target}}
                                                        : parser.parse(inner.substr(bar + 1), parser.depth() + 1);
      out.push_back(ir::Link{target, {}, std::move(label)});
      return end + 2;
    }
    if (rest.starts_with("{{")) {
      const std::size_t end = rest.find("}}");
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(2, end - 2);
      const std::size_t bar = inner.find('|');
      std::string target(util::trim(inner.substr(0, bar)));
      target = target.substr(0, target.find('?'));
      const std::string alt = bar == std::string_view::npos ? std::string() : std::string(util::trim(inner.substr(bar + 1)));
      out.push_back(ir::InlineImage{wiki::image_from_url(target), alt});
      return end + 2;
    }
    if (const std::size_t n = html_inline(parser, text, pos, out)) return n;
    return wiki::common_inline(text, pos, out);
  };
  return syntax;
}

ir::Document read_dokuwiki_text(std::string_view text) {
  wiki::ReadSyntax syntax;
  syntax.heading = dokuwiki_heading;
  syntax.list_item = dokuwiki_list;
  syntax.rule = [](std::string_view line) { return is_repeated(line, '-', 4); };
  syntax.table_row = dokuwiki_row;
  syntax.quote_line = [](std::string_view line) -> std::optional<std::string> {
    if (!line.starts_with(">")) return std::nullopt;
    std::string_view rest = line.substr(1);
    if (rest.starts_with(" ")) rest.remove_prefix(1);
    return std::string(rest);
  };
  syntax.preformatted_line = [](std::string_view line) -> std::optional<std::string> {
    if (line.starts_with("  ") && !dokuwiki_list(line) && !util::trim(line).empty()) return std::string(line.substr(2));
    if (line.starts_with("\t") && !dokuwiki_list(line)) return std::string(line.substr(1));
    return std::nullopt;
  };
  syntax.block = dokuwiki_block;
  syntax.inline_syntax = dokuwiki_inline;
  syntax.list_item_continuation = false;
  ir::Document document = wiki::read(text, syntax);
  // A paragraph made only of the alt text after a picture is pandoc's caption line.
  return document;
}

std::string dokuwiki_escape(std::string_view text) {
  static constexpr std::string_view specials = "*/_'[]{}<>()\\^|%~=";
  bool risky = text.find("://") != std::string_view::npos || text.find('&') != std::string_view::npos;
  for (const char c : text) {
    if (specials.find(c) != std::string_view::npos) risky = true;
  }
  if (!risky) return std::string(text);
  std::string flat(text);
  for (char& c : flat) {
    if (c == '\n') c = ' ';
  }
  if (flat.find("%%") == std::string::npos) return "%%" + flat + "%%";
  return "<nowiki>" + flat + "</nowiki>";
}

InlineMarkup dokuwiki_markup(const ir::Document* document) {
  InlineMarkup m;
  m.escape = dokuwiki_escape;
  m.strong = {"**", "**"};
  m.emphasis = {"//", "//"};
  m.underline = {"__", "__"};
  m.strikethrough = {"<del>", "</del>"};
  m.superscript = {"<sup>", "</sup>"};
  m.subscript = {"<sub>", "</sub>"};
  m.line_break = "\\\\ ";
  m.soft_break = " ";
  m.code = [](const ir::Code& code) -> std::string {
    if (code.raw_format == "dokuwiki") return code.text;
    if (!code.raw_format.empty()) return {};
    std::string text = code.text;
    for (char& c : text) {
      if (c == '\n') c = ' ';
    }
    if (text.find("%%") == std::string::npos && text.find("''") == std::string::npos) return "''%%" + text + "%%''";
    return "''" + dokuwiki_escape(text) + "''";
  };
  m.link = [](const ir::Link& link, const std::string& label) -> std::string {
    std::string url = link.url;
    std::erase_if(url, [](char c) { return c == '|' || c == ']' || c == '\n'; });
    return "[[" + url + "|" + label + "]]";
  };
  m.image = [](const ir::InlineImage& image) -> std::string {
    const std::string url = image_url(image.source, false);
    if (url.empty()) return dokuwiki_escape(image.alt);
    std::string alt = image.alt;
    std::erase_if(alt, [](char c) { return c == '}' || c == '|'; });
    return "{{" + url + (alt.empty() ? "" : "|" + alt) + "}}";
  };
  if (document != nullptr) {
    m.note = [document](const ir::Link&, std::size_t index) -> std::string {
      if (index == 0 || index > document->footnotes.size()) return {};
      const InlineMarkup inner = dokuwiki_markup(nullptr);
      std::string body;
      for (const ir::Block& b : document->footnotes[index - 1].blocks) {
        if (const auto* p = b.get_if<ir::Paragraph>()) {
          if (!body.empty()) body.append(" \\\\ ");
          body.append(render_inlines(p->content, inner, document));
        }
      }
      return "((" + body + "))";
    };
  }
  return m;
}

std::string dokuwiki_table(const ir::TableBlock& table, const std::function<std::string(const ir::Inlines&)>& render) {
  const TableGrid grid = table_grid(table);
  std::string out;
  for (std::size_t r = 0; r < grid.cells.size(); ++r) {
    const bool header = table.rows[r].header;
    std::string line;
    for (std::size_t c = 0; c < grid.columns; ++c) {
      const ir::TableCell* cell = grid.cells[r][c];
      const char separator = header ? '^' : '|';
      if (cell != nullptr && !grid.origin[r][c]) {
        // Covered slot: rows below get ":::", columns to the right an empty separator.
        if (c > 0 && grid.cells[r][c - 1] == cell) {
          line.push_back(separator);
          continue;
        }
        line.push_back(separator);
        line.append(" ::: ");
        continue;
      }
      line.push_back(separator);
      std::string text = cell != nullptr ? render(cell->content) : std::string();
      const ir::Alignment align = cell != nullptr && cell->align != ir::Alignment::Default ? cell->align : table.column_alignment(c);
      if (align == ir::Alignment::Right) line.append("  ").append(text).append(" ");
      else if (align == ir::Alignment::Center) line.append("  ").append(text).append("  ");
      else if (align == ir::Alignment::Left) line.append(" ").append(text).append("  ");
      else line.append(" ").append(text).append(" ");
    }
    line.push_back(header ? '^' : '|');
    if (!out.empty()) out.push_back('\n');
    out.append(line);
  }
  return out;
}

Result<std::string> write_dokuwiki(const ir::Document& document, WriteContext& /*ctx*/) {
  wiki::WriteSyntax syntax;
  syntax.raw_format = "dokuwiki";
  syntax.heading = [](int level, const std::string& text, const ir::Attributes&) {
    const std::string marks = wiki::repeat("=", 7 - std::min(level, 5));
    return marks + " " + text + " " + marks;
  };
  syntax.list_marker = [](const std::vector<bool>& path, int, ir::TaskState task) {
    std::string out(2 * path.size(), ' ');
    out.push_back(path.back() ? '-' : '*');
    out.push_back(' ');
    if (task == ir::TaskState::Done) out += "\xE2\x98\x91 ";
    if (task == ir::TaskState::Open) out += "\xE2\x98\x90 ";
    return out;
  };
  syntax.code_block = [](const ir::CodeBlock& code) -> std::string {
    std::string lang = code.math != ir::MathMode::None ? "latex" : code.language;
    std::erase_if(lang, [](char c) { return !(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '+' || c == '-' || c == '_'); });
    const std::string tag = code.code.find("</code>") == std::string::npos ? "code" : "file";
    return "<" + tag + (lang.empty() ? "" : " " + lang) + ">\n" + code.code + "\n</" + tag + ">";
  };
  syntax.quote = [](const std::string& inner) {
    std::string out;
    for (const std::string_view line : util::split(inner, '\n')) {
      if (!out.empty()) out.push_back('\n');
      out.append(">").append(line.empty() ? "" : " ").append(line);
    }
    return out;
  };
  syntax.rule = "----";
  syntax.table = dokuwiki_table;
  syntax.image_block = [](const ir::ImageBlock& image, const std::string& url) -> std::string {
    if (url.starts_with("data:")) return dokuwiki_escape(image.caption.empty() ? image.alt : image.caption);
    std::string alt = image.caption.empty() ? image.alt : image.caption;
    std::erase_if(alt, [](char c) { return c == '}' || c == '|'; });
    return "{{" + url + (alt.empty() ? "" : "|" + alt) + "}}";
  };
  syntax.item_break = " \\\\ ";
  syntax.escape_line_start = [](const std::string& line) -> std::string {
    if (line.starts_with(">") || line.starts_with("^") || line.starts_with("|") || line.starts_with("  ") ||
        line.starts_with("==") || line.starts_with("----")) {
      return "%%" + line.substr(0, 1) + "%%" + line.substr(1);
    }
    return line;
  };
  wiki::Writer writer(document, syntax, dokuwiki_markup(&document), 0);
  return writer.run();
}

int sniff_dokuwiki(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"dokuwiki", "dw"})) return 80;
  const std::string_view head = sniff_head(data);
  int score = 0;
  if (head.starts_with("======") || head.find("\n=====") != std::string_view::npos) score += 3;
  if (head.find("<code") != std::string_view::npos) score += 1;
  if (head.find("[[") != std::string_view::npos && head.find("|") != std::string_view::npos) score += 1;
  if (head.find("\n^") != std::string_view::npos) score += 2;
  if (head.find("((") != std::string_view::npos && head.find("))") != std::string_view::npos) score += 1;
  return score >= 5 ? 64 : 0;
}

// =========================================================================== Creole

std::optional<Heading> creole_heading(std::string_view line) {
  const std::string_view t = util::trim(line);
  if (t.size() < 2 || t.front() != '=') return std::nullopt;
  std::size_t level = 0;
  while (level < t.size() && t[level] == '=') ++level;
  if (level > 6) return std::nullopt;
  std::string_view text = t.substr(level);
  while (!text.empty() && text.back() == '=') text.remove_suffix(1);
  return Heading{static_cast<int>(level), std::string(util::trim(text)), {}};
}

std::optional<ListItem> creole_list(std::string_view line) {
  const std::string_view t = util::trim_left(line);
  std::size_t n = 0;
  while (n < t.size() && (t[n] == '*' || t[n] == '#')) ++n;
  if (n == 0 || n >= t.size() || (t[n] != ' ' && t[n] != '\t')) return std::nullopt;
  ListItem item;
  item.depth = static_cast<int>(n);
  item.ordered = t[n - 1] == '#';
  item.text = std::string(util::trim(t.substr(n)));
  return item;
}

std::optional<std::vector<Cell>> creole_row(std::string_view line) {
  const std::string_view t = util::trim(line);
  if (!t.starts_with("|")) return std::nullopt;
  std::vector<Cell> cells;
  std::size_t i = 1;
  while (i < t.size()) {
    std::size_t end = i;
    while (end < t.size() && t[end] != '|') {
      if (t.substr(end).starts_with("[[")) {
        const std::size_t close = t.find("]]", end);
        end = close == std::string_view::npos ? t.size() : close + 2;
      } else if (t.substr(end).starts_with("{{")) {
        const std::size_t close = t.find("}}", end);
        end = close == std::string_view::npos ? t.size() : close + 2;
      } else if (t[end] == '~' && end + 1 < t.size()) {
        end += 2;
      } else {
        ++end;
      }
    }
    std::string_view raw = t.substr(i, std::min(end, t.size()) - i);
    Cell cell;
    if (raw.starts_with("=")) {
      cell.header = true;
      raw.remove_prefix(1);
    }
    cell.text = std::string(util::trim(raw));
    if (!(end >= t.size() && cell.text.empty() && !cell.header)) cells.push_back(std::move(cell));
    i = end + 1;
  }
  if (cells.empty()) return std::nullopt;
  return cells;
}

std::size_t creole_block(std::span<const std::string_view> lines, std::size_t at, ir::Blocks& out, wiki::Reader&) {
  if (util::trim(lines[at]) != "{{{") return 0;
  std::vector<std::string_view> code;
  for (std::size_t i = at + 1; i < lines.size(); ++i) {
    if (util::trim_right(lines[i]) == "}}}") {
      std::vector<std::string> unindented;
      for (const std::string_view l : code) {
        // " }}}" inside a block is a literal "}}}".
        unindented.emplace_back(l.starts_with(" }}}") ? l.substr(1) : l);
      }
      out.push_back(ir::CodeBlock{{}, util::join(unindented, "\n")});
      return i - at + 1;
    }
    code.push_back(lines[i]);
  }
  return 0;
}

InlineSyntax creole_inline(wiki::Context&) {
  InlineSyntax syntax;
  syntax.escape = '~';
  syntax.rules = {
      {"**", "**", InlineKind::Strong, false, false, true},
      {"//", "//", InlineKind::Emphasis, false, false, true},
      {"##", "##", InlineKind::Code, true, false, true},
      {"^^", "^^", InlineKind::Superscript, false, false, true},
      {",,", ",,", InlineKind::Subscript, false, false, true},
      {"__", "__", InlineKind::Underline, false, false, true},
      {"--", "--", InlineKind::Strikethrough, false, true, false},
  };
  syntax.hook = [](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    if (rest.starts_with("{{{")) {
      std::size_t end = rest.find("}}}", 3);
      if (end == std::string_view::npos) return 0;
      while (end + 3 < rest.size() && rest[end + 3] == '}') ++end;  // "{{{ x }}}}" keeps the inner brace
      out.push_back(ir::Code{std::string(rest.substr(3, end - 3))});
      return end + 3;
    }
    if (rest.starts_with("\\\\")) {
      out.push_back(ir::LineBreak{});
      return 2;
    }
    if (rest.starts_with("[[")) {
      const std::size_t end = rest.find("]]");
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(2, end - 2);
      const std::size_t bar = inner.find('|');
      const std::string target(util::trim(inner.substr(0, bar)));
      ir::Inlines label = bar == std::string_view::npos ? ir::Inlines{ir::Text{target}}
                                                        : parser.parse(inner.substr(bar + 1), parser.depth() + 1);
      out.push_back(ir::Link{target, {}, std::move(label)});
      return end + 2;
    }
    if (rest.starts_with("{{")) {
      const std::size_t end = rest.find("}}");
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(2, end - 2);
      const std::size_t bar = inner.find('|');
      const std::string alt = bar == std::string_view::npos ? std::string() : std::string(util::trim(inner.substr(bar + 1)));
      out.push_back(ir::InlineImage{wiki::image_from_url(util::trim(inner.substr(0, bar))), alt});
      return end + 2;
    }
    if (const std::size_t length = wiki::bare_url_length(text, pos); length > 0) {
      const std::string url(text.substr(pos, length));
      out.push_back(ir::Link{url, {}, {ir::Text{url}}});
      return length;
    }
    return 0;
  };
  return syntax;
}

ir::Document read_creole_text(std::string_view text) {
  wiki::ReadSyntax syntax;
  syntax.heading = creole_heading;
  syntax.list_item = creole_list;
  syntax.rule = [](std::string_view line) { return util::trim(line) == "----"; };
  syntax.table_row = creole_row;
  syntax.block = creole_block;
  syntax.inline_syntax = creole_inline;
  return wiki::read(text, syntax);
}

std::string creole_escape(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    bool escape = c == '~' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|' || c == '\\' || c == '*' ||
                  c == '#' || c == '=' || (c == '/' && (next == '/' || (i > 0 && text[i - 1] == '/'))) ||
                  (c == '^' && next == '^') || (c == ',' && next == ',') || (c == '_' && next == '_') ||
                  (c == '-' && next == '-') || (c == ':' && next == '/');
    if (c == '\n') {
      out.push_back(' ');
      continue;
    }
    if (escape) out.push_back('~');
    out.push_back(c);
  }
  return out;
}

Result<std::string> write_creole(const ir::Document& document, WriteContext& /*ctx*/) {
  ir::Document copy = document;
  footnotes_to_endnotes(copy);
  wiki::WriteSyntax syntax;
  syntax.raw_format = "creole";
  syntax.heading = [](int level, const std::string& text, const ir::Attributes&) {
    const std::string marks = wiki::repeat("=", level);
    return marks + " " + text + " " + marks;
  };
  syntax.list_marker = [](const std::vector<bool>& path, int, ir::TaskState task) {
    std::string out;
    for (std::size_t k = 0; k < path.size(); ++k) out.push_back(path.back() ? '#' : '*');
    out.push_back(' ');
    if (task == ir::TaskState::Done) out += "\xE2\x98\x91 ";
    if (task == ir::TaskState::Open) out += "\xE2\x98\x90 ";
    return out;
  };
  syntax.code_block = [](const ir::CodeBlock& code) -> std::string {
    std::string body;
    for (const std::string_view line : util::split(code.code, '\n')) {
      if (!body.empty()) body.push_back('\n');
      if (util::trim_left(line).starts_with("}}}")) body.push_back(' ');
      body.append(line);
    }
    return "{{{\n" + body + "\n}}}";
  };
  syntax.quote = [](const std::string& inner) { return inner; };
  syntax.rule = "----";
  syntax.table = [](const ir::TableBlock& table, const std::function<std::string(const ir::Inlines&)>& render) {
    const auto cells = table_strings(table, render);
    std::string out;
    for (std::size_t r = 0; r < cells.size(); ++r) {
      if (!out.empty()) out.push_back('\n');
      for (const std::string& cell : cells[r]) out.append(table.rows[r].header ? "|= " : "| ").append(cell).append(" ");
      out.append("|");
    }
    return out;
  };
  syntax.image_block = [](const ir::ImageBlock& image, const std::string& url) -> std::string {
    if (url.starts_with("data:")) return creole_escape(image.caption.empty() ? image.alt : image.caption);
    std::string alt = image.caption.empty() ? image.alt : image.caption;
    std::erase_if(alt, [](char c) { return c == '}' || c == '|'; });
    return "{{" + url + "|" + alt + "}}";
  };
  syntax.item_break = " \\\\ ";
  InlineMarkup m;
  m.escape = creole_escape;
  m.strong = {"**", "**"};
  m.emphasis = {"//", "//"};
  m.superscript = {"^^", "^^"};
  m.subscript = {",,", ",,"};
  m.underline = {"__", "__"};
  m.strikethrough = {"--", "--"};
  m.line_break = "\\\\";
  m.soft_break = " ";
  m.code = [](const ir::Code& code) -> std::string {
    if (!code.raw_format.empty() && code.raw_format != "creole") return {};
    if (code.raw_format == "creole") return code.text;
    std::string text = code.text;
    for (char& c : text) {
      if (c == '\n') c = ' ';
    }
    if (text.find("}}}") != std::string::npos) return "##" + creole_escape(text) + "##";
    return "{{{" + text + "}}}";
  };
  m.link = [](const ir::Link& link, const std::string& label) -> std::string {
    std::string url = link.url;
    std::erase_if(url, [](char c) { return c == '|' || c == ']' || c == '\n'; });
    return "[[" + url + "|" + label + "]]";
  };
  m.image = [](const ir::InlineImage& image) -> std::string {
    const std::string url = image_url(image.source, false);
    if (url.empty()) return creole_escape(image.alt);
    std::string alt = image.alt;
    std::erase_if(alt, [](char c) { return c == '}' || c == '|'; });
    return "{{" + url + "|" + alt + "}}";
  };
  wiki::Writer writer(copy, syntax, m, 0);
  return writer.run();
}

// =========================================================================== Jira

std::optional<Heading> jira_heading(std::string_view line) {
  const std::string_view t = util::trim_left(line);
  if (t.size() < 4 || t[0] != 'h' || t[1] < '1' || t[1] > '6' || t[2] != '.' || (t.size() > 3 && t[3] != ' ')) {
    return std::nullopt;
  }
  Heading heading{t[1] - '0', std::string(util::trim(t.substr(3))), {}};
  if (heading.text.starts_with("{anchor:")) {
    const std::size_t close = heading.text.find('}');
    if (close != std::string::npos) {
      heading.id = heading.text.substr(8, close - 8);
      heading.text = std::string(util::trim(std::string_view(heading.text).substr(close + 1)));
    }
  }
  return heading;
}

std::optional<ListItem> jira_list(std::string_view line) {
  std::size_t n = 0;
  while (n < line.size() && (line[n] == '*' || line[n] == '#' || line[n] == '-')) ++n;
  if (n == 0 || n >= line.size() || line[n] != ' ') return std::nullopt;
  if (line[0] == '-' && n > 1) return std::nullopt;
  ListItem item;
  item.depth = static_cast<int>(n);
  item.ordered = line[n - 1] == '#';
  item.text = std::string(util::trim(line.substr(n)));
  return item;
}

std::optional<std::vector<Cell>> jira_row(std::string_view line) {
  const std::string_view t = util::trim(line);
  if (!t.starts_with("|")) return std::nullopt;
  std::vector<Cell> cells;
  std::size_t i = 0;
  while (i < t.size()) {
    const bool header = t.substr(i).starts_with("||");
    const std::size_t start = i + (header ? 2 : 1);
    std::size_t end = start;
    while (end < t.size() && t[end] != '|') {
      if (t[end] == '\\' && end + 1 < t.size()) {
        end += 2;
        continue;
      }
      if (t[end] == '[') {
        const std::size_t close = t.find(']', end);
        end = close == std::string_view::npos ? t.size() : close + 1;
        continue;
      }
      ++end;
    }
    if (start >= t.size()) break;
    Cell cell;
    cell.header = header;
    cell.text = std::string(util::trim(t.substr(start, std::min(end, t.size()) - start)));
    if (end < t.size() || !cell.text.empty()) cells.push_back(std::move(cell));
    i = end;
    if (i + 1 >= t.size()) break;
  }
  if (cells.empty()) return std::nullopt;
  return cells;
}

std::size_t jira_block(std::span<const std::string_view> lines, std::size_t at, ir::Blocks& out, wiki::Reader& reader) {
  const std::string_view line = util::trim(lines[at]);
  const auto delimited = [&](std::string_view open_prefix, std::string_view close) -> std::optional<std::pair<std::string, std::size_t>> {
    if (!line.starts_with(open_prefix)) return std::nullopt;
    const std::size_t brace = line.find('}');
    if (brace == std::string_view::npos) return std::nullopt;
    std::string content(line.substr(brace + 1));
    std::size_t i = at;
    while (true) {
      const std::size_t end = content.find(close);
      if (end != std::string::npos && (i > at || end > 0 || content.empty() || true)) {
        if (end != std::string::npos) {
          content.erase(end);
          return std::make_pair(content, i - at + 1);
        }
      }
      if (++i >= lines.size()) return std::nullopt;
      content.append("\n").append(lines[i]);
    }
  };
  if (line.starts_with("{code") || line.starts_with("{noformat")) {
    const bool noformat = line.starts_with("{noformat");
    auto block = delimited(noformat ? "{noformat" : "{code", noformat ? "{noformat}" : "{code}");
    if (!block) return 0;
    ir::CodeBlock code;
    const std::size_t brace = line.find('}');
    std::string_view params = line.substr(noformat ? 9 : 5, brace - (noformat ? 9 : 5));
    if (params.starts_with(":")) params.remove_prefix(1);
    for (const std::string_view p : util::split(params, '|', false)) {
      if (p.find('=') == std::string_view::npos) code.language = std::string(p);
      else if (p.starts_with("language=")) code.language = std::string(p.substr(9));
    }
    std::string text = block->first;
    if (text.starts_with("\n")) text.erase(0, 1);
    if (text.ends_with("\n")) text.pop_back();
    code.code = std::move(text);
    out.push_back(std::move(code));
    return block->second;
  }
  for (const std::string_view kind : {"quote", "panel", "note", "warning", "info", "tip"}) {
    const std::string open = "{" + std::string(kind);
    if (!line.starts_with(open)) continue;
    auto block = delimited(open, "{" + std::string(kind) + "}");
    if (!block) return 0;
    const std::vector<std::string_view> inner = util::split_lines(block->first);
    ir::Attributes attr;
    if (kind != "quote" && kind != "panel") {
      attr.add_class(ir::conventions::admonition);
      attr.add_class(kind);
    }
    out.push_back(ir::Block(ir::BlockQuote{reader.parse(inner, 1)}, std::move(attr)));
    return block->second;
  }
  return 0;
}

InlineSyntax jira_inline(wiki::Context&) {
  InlineSyntax syntax;
  syntax.escape = '\\';
  syntax.rules = {
      {"*", "*", InlineKind::Strong}, {"_", "_", InlineKind::Emphasis}, {"-", "-", InlineKind::Strikethrough},
      {"+", "+", InlineKind::Underline}, {"^", "^", InlineKind::Superscript}, {"~", "~", InlineKind::Subscript},
      {"??", "??", InlineKind::Emphasis},
  };
  syntax.hook = [](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    if (rest.starts_with("{{")) {
      const std::size_t end = rest.find("}}", 2);
      if (end == std::string_view::npos) return 0;
      std::string code;
      const std::string_view inner = rest.substr(2, end - 2);
      for (std::size_t k = 0; k < inner.size(); ++k) {
        if (inner[k] == '\\' && k + 1 < inner.size()) ++k;
        code.push_back(inner[k]);
      }
      out.push_back(ir::Code{std::move(code)});
      return end + 2;
    }
    // Braced delimiters for intra-word formatting: {*}x{*}
    if (rest.size() >= 3 && rest[0] == '{' && rest[2] == '}' && std::string_view("*_-+^~").find(rest[1]) != std::string_view::npos) {
      const std::string marker(rest.substr(0, 3));
      const std::size_t end = rest.find(marker, 3);
      if (end != std::string_view::npos && parser.depth() < 20) {
        ir::Inlines inner = parser.parse(rest.substr(3, end - 3), parser.depth() + 1);
        switch (rest[1]) {
          case '*': out.push_back(ir::Strong{std::move(inner)}); break;
          case '_': out.push_back(ir::Emphasis{std::move(inner)}); break;
          case '-': out.push_back(ir::Strikethrough{std::move(inner)}); break;
          case '+': out.push_back(ir::Underline{std::move(inner)}); break;
          case '^': out.push_back(ir::Superscript{std::move(inner)}); break;
          default: out.push_back(ir::Subscript{std::move(inner)}); break;
        }
        return end + 3;
      }
    }
    if (rest.starts_with("{anchor:") || rest.starts_with("{color")) {
      const std::size_t end = rest.find('}');
      return end == std::string_view::npos ? 0 : end + 1;
    }
    if (rest.starts_with("\\\\")) {
      out.push_back(ir::LineBreak{});
      return 2;
    }
    if (rest.starts_with("[")) {
      const std::size_t end = rest.find(']');
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(1, end - 1);
      if (inner.starts_with("~")) return 0;
      const std::size_t bar = inner.rfind('|');
      if (bar == std::string_view::npos) {
        if (!is_absolute_url(inner) && !inner.starts_with("#")) return 0;
        out.push_back(ir::Link{std::string(inner), {}, {ir::Text{std::string(inner)}}});
      } else {
        out.push_back(ir::Link{std::string(util::trim(inner.substr(bar + 1))), {},
                               parser.parse(inner.substr(0, bar), parser.depth() + 1)});
      }
      return end + 1;
    }
    if (rest.starts_with("!") && rest.size() > 2 && !util::is_ascii_space(rest[1])) {
      const std::size_t end = rest.find('!', 1);
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(1, end - 1);
      if (inner.find(' ') != std::string_view::npos && inner.find('|') == std::string_view::npos) return 0;
      const std::size_t bar = inner.find('|');
      std::string alt;
      if (bar != std::string_view::npos) {
        for (const std::string_view p : util::split(inner.substr(bar + 1), ',', false)) {
          const std::string_view t = util::trim(p);
          if (t.starts_with("alt=")) alt = std::string(t.substr(4));
        }
      }
      out.push_back(ir::InlineImage{wiki::image_from_url(inner.substr(0, bar)), alt});
      return end + 1;
    }
    return wiki::common_inline(text, pos, out);
  };
  return syntax;
}

ir::Document read_jira_text(std::string_view text) {
  wiki::ReadSyntax syntax;
  syntax.heading = jira_heading;
  syntax.list_item = jira_list;
  syntax.rule = [](std::string_view line) { return util::trim(line) == "----"; };
  syntax.table_row = jira_row;
  syntax.quote_line = [](std::string_view line) -> std::optional<std::string> {
    if (!line.starts_with("bq. ")) return std::nullopt;
    return std::string(line.substr(4));
  };
  syntax.block = jira_block;
  syntax.inline_syntax = jira_inline;
  syntax.list_item_continuation = false;
  return wiki::read(text, syntax);
}

std::string jira_escape(std::string_view text) {
  // A backslash cannot be escaped with itself ("\\" is a line break): a character reference.
  std::string out;
  for (const char c : text) {
    if (c == '\\') {
      out.append("&#92;");
      continue;
    }
    if (std::string_view("*_-+^~{}[]!|?#").find(c) != std::string_view::npos) out.push_back('\\');
    out.push_back(c);
  }
  for (char& c : out) {
    if (c == '\n') c = ' ';
  }
  return out;
}

Result<std::string> write_jira(const ir::Document& document, WriteContext& /*ctx*/) {
  ir::Document copy = document;
  footnotes_to_endnotes(copy);
  wiki::WriteSyntax syntax;
  syntax.raw_format = "jira";
  syntax.heading = [](int level, const std::string& text, const ir::Attributes& attr) {
    std::string out = "h" + std::to_string(level) + ". ";
    if (!attr.id.empty()) out += "{anchor:" + jira_escape(attr.id) + "}";
    return out + text;
  };
  syntax.list_marker = [](const std::vector<bool>& path, int, ir::TaskState task) {
    std::string out;
    for (const bool ordered : path) out.push_back(ordered ? '#' : '*');
    out.push_back(' ');
    if (task == ir::TaskState::Done) out += "(/) ";
    if (task == ir::TaskState::Open) out += "(x) ";
    return out;
  };
  syntax.code_block = [](const ir::CodeBlock& code) -> std::string {
    if (code.language.empty() || code.code.find("{code}") != std::string::npos) {
      return "{noformat}\n" + code.code + "\n{noformat}";
    }
    std::string lang = code.language;
    std::erase_if(lang, [](char c) { return !(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '+' || c == '-' || c == '_'); });
    return "{code:" + lang + "}\n" + code.code + "\n{code}";
  };
  syntax.quote = [](const std::string& inner) { return "{quote}\n" + inner + "\n{quote}"; };
  syntax.rule = "----";
  syntax.table = [](const ir::TableBlock& table, const std::function<std::string(const ir::Inlines&)>& render) {
    const auto cells = table_strings(table, render);
    std::string out;
    for (std::size_t r = 0; r < cells.size(); ++r) {
      if (!out.empty()) out.push_back('\n');
      const std::string_view sep = table.rows[r].header ? "||" : "|";
      for (const std::string& cell : cells[r]) out.append(sep).append(" ").append(cell.empty() ? " " : cell).append(" ");
      out.append(sep);
    }
    return out;
  };
  syntax.image_block = [](const ir::ImageBlock& image, const std::string& url) -> std::string {
    if (url.starts_with("data:")) return jira_escape(image.caption.empty() ? image.alt : image.caption);
    std::string alt = image.caption.empty() ? image.alt : image.caption;
    std::erase_if(alt, [](char c) { return c == '!' || c == ',' || c == '|'; });
    return "!" + url + (alt.empty() ? "" : "|alt=" + alt) + "!";
  };
  syntax.item_break = " \\\\ ";
  syntax.escape_line_start = [](const std::string& line) -> std::string {
    if (jira_heading(line) || line.starts_with("bq. ")) return "\\" + line;
    return line;
  };
  InlineMarkup m;
  m.escape = jira_escape;
  m.strong = {"*", "*"};
  m.emphasis = {"_", "_"};
  m.strikethrough = {"-", "-"};
  m.underline = {"+", "+"};
  m.superscript = {"^", "^"};
  m.subscript = {"~", "~"};
  m.line_break = "\\\\";
  m.soft_break = " ";
  m.code = [](const ir::Code& code) -> std::string {
    if (code.raw_format == "jira") return code.text;
    if (!code.raw_format.empty()) return {};
    std::string text = code.text;
    for (char& c : text) {
      if (c == '\n') c = ' ';
    }
    return "{{" + wiki::escape_chars(text, "{}\\") + "}}";
  };
  m.link = [](const ir::Link& link, const std::string& label) -> std::string {
    std::string url = link.url;
    std::erase_if(url, [](char c) { return c == '|' || c == ']' || c == '\n'; });
    if (label == jira_escape(link.url)) return "[" + url + "]";
    return "[" + label + "|" + url + "]";
  };
  m.image = [](const ir::InlineImage& image) -> std::string {
    const std::string url = image_url(image.source, false);
    if (url.empty()) return jira_escape(image.alt);
    std::string alt = image.alt;
    std::erase_if(alt, [](char c) { return c == '!' || c == ',' || c == '|'; });
    return "!" + url + (alt.empty() ? "" : "|alt=" + alt) + "!";
  };
  wiki::Writer writer(copy, syntax, m, 0);
  return writer.run();
}

int sniff_jira(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"jira", "confluence"})) return 80;
  const std::string_view head = sniff_head(data);
  int score = 0;
  if (head.starts_with("h1. ") || head.find("\nh1. ") != std::string_view::npos || head.find("\nh2. ") != std::string_view::npos) score += 3;
  if (head.find("{code") != std::string_view::npos || head.find("{noformat}") != std::string_view::npos) score += 2;
  if (head.find("\n||") != std::string_view::npos || head.starts_with("||")) score += 2;
  if (head.find("\nbq. ") != std::string_view::npos) score += 1;
  if (head.find("\ntable") != std::string_view::npos) score -= 1;
  // A file that is only a table: a "||heading||" row, then "|cell|" rows.
  const std::size_t nl = head.find('\n');
  const std::string_view first = util::trim(head.substr(0, nl));
  const std::string_view second = nl == std::string_view::npos ? std::string_view() : util::trim(head.substr(nl + 1, head.find('\n', nl + 1) - nl - 1));
  if (first.starts_with("||") && first.ends_with("||") && second.starts_with('|') && !second.starts_with("|-")) score += 7;
  return score >= 7 ? 74 : score >= 5 ? 66 : 0;
}

}  // namespace

void register_wikis(Registry& registry) {
  MarkupFormat mediawiki;
  mediawiki.id = "wiki";
  mediawiki.name = "MediaWiki markup";
  mediawiki.description = "Wikitext as used by Wikipedia and other MediaWiki sites.";
  mediawiki.extensions = {"wiki", "mediawiki", "mw"};
  mediawiki.mime = "text/x-wiki";
  mediawiki.popularity = 40;
  mediawiki.sniff = sniff_mediawiki;
  mediawiki.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_mediawiki_text(text); };
  mediawiki.write = write_mediawiki;
  register_markup(registry, std::move(mediawiki));

  MarkupFormat dokuwiki;
  dokuwiki.id = "dokuwiki";
  dokuwiki.name = "DokuWiki markup";
  dokuwiki.description = "Page markup of the DokuWiki wiki engine.";
  dokuwiki.extensions = {"dokuwiki", "dw"};
  dokuwiki.mime = "text/x-dokuwiki";
  dokuwiki.popularity = 20;
  dokuwiki.sniff = sniff_dokuwiki;
  dokuwiki.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_dokuwiki_text(text); };
  dokuwiki.write = write_dokuwiki;
  register_markup(registry, std::move(dokuwiki));

  MarkupFormat creole;
  creole.id = "creole";
  creole.name = "Creole wiki markup";
  creole.description = "The common wiki markup standard Creole 1.0 (with common additions).";
  creole.extensions = {"creole"};
  creole.mime = "text/x-creole";
  creole.popularity = 10;
  creole.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_creole_text(text); };
  creole.write = write_creole;
  creole.write_lossy = true;  // no quotes, notes become end notes
  register_markup(registry, std::move(creole));

  MarkupFormat jira;
  jira.id = "jira";
  jira.name = "Jira wiki markup";
  jira.description = "Text formatting notation of Jira and Confluence.";
  jira.extensions = {"jira", "confluence"};
  jira.mime = "text/x-jira";
  jira.popularity = 25;
  jira.sniff = sniff_jira;
  jira.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_jira_text(text); };
  jira.write = write_jira;
  jira.write_lossy = true;  // notes become end notes
  register_markup(registry, std::move(jira));
}

}  // namespace cvt::document
