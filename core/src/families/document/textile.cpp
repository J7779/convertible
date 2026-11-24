// Convertible core - document family: Textile markup.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <set>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/html_internal.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

// Block signature "h1(#id){style}[lang]<>=. ": returns (tag, attributes, content).
struct Signature {
  std::string tag;
  std::string id;
  std::string classes;
  ir::Alignment align = ir::Alignment::Default;
  bool extended = false;  // ".." continues over blank lines
  std::string_view content;
};

std::optional<Signature> block_signature(std::string_view line) {
  std::size_t i = 0;
  while (i < line.size() && util::is_ascii_alpha(line[i])) ++i;
  if (i < line.size() && util::is_ascii_digit(line[i]) && i > 0) ++i;
  if (i == 0) return std::nullopt;
  Signature sig;
  sig.tag = std::string(line.substr(0, i));
  static constexpr std::array<std::string_view, 14> tags = {"h1", "h2", "h3", "h4", "h5", "h6", "p",
                                                            "bq", "bc", "pre", "notextile", "fn", "table", "div"};
  const bool footnote = sig.tag.starts_with("fn") && sig.tag.size() > 2;
  if (!footnote && std::ranges::find(tags, sig.tag) == tags.end()) {
    // fn12 has more digits.
    return std::nullopt;
  }
  // Attributes.
  while (i < line.size()) {
    const char c = line[i];
    if (c == '(' || c == '{' || c == '[') {
      const char close = c == '(' ? ')' : c == '{' ? '}' : ']';
      const std::size_t end = line.find(close, i);
      if (end == std::string_view::npos) return std::nullopt;
      if (c == '(') {
        std::string_view inside = line.substr(i + 1, end - i - 1);
        const std::size_t hash = inside.find('#');
        if (hash != std::string_view::npos) {
          sig.id = std::string(inside.substr(hash + 1));
          inside = inside.substr(0, hash);
        }
        sig.classes = std::string(inside);
      }
      i = end + 1;
    } else if (c == '<' || c == '>' || c == '=') {
      sig.align = c == '<' ? ir::Alignment::Left : c == '>' ? ir::Alignment::Right : ir::Alignment::Center;
      ++i;
    } else {
      break;
    }
  }
  if (i >= line.size() || line[i] != '.') return std::nullopt;
  ++i;
  if (i < line.size() && line[i] == '.') {
    sig.extended = true;
    ++i;
  }
  if (i < line.size() && line[i] != ' ') return std::nullopt;
  sig.content = i < line.size() ? line.substr(i + 1) : std::string_view{};
  return sig;
}

struct TextileState {
  std::set<std::string> notes;
};

InlineSyntax textile_inline(const TextileState& state) {
  InlineSyntax syntax;
  syntax.rules = {
      {"**", "**", InlineKind::Strong},      {"__", "__", InlineKind::Emphasis}, {"*", "*", InlineKind::Strong},
      {"_", "_", InlineKind::Emphasis},      {"??", "??", InlineKind::Emphasis}, {"-", "-", InlineKind::Strikethrough},
      {"+", "+", InlineKind::Underline},     {"^", "^", InlineKind::Superscript}, {"~", "~", InlineKind::Subscript},
  };
  syntax.hook = [&state](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    if (rest.starts_with("@") && rest.size() > 2 && !util::is_ascii_space(rest[1])) {
      const std::size_t end = rest.find('@', 1);
      if (end != std::string_view::npos && end > 1) {
        out.push_back(ir::Code{html::decode_references(rest.substr(1, end - 1), false)});
        return end + 1;
      }
    }
    if (rest.starts_with("==")) {
      const std::size_t end = rest.find("==", 2);
      if (end == std::string_view::npos) return 0;
      append_text(out, rest.substr(2, end - 2));
      return end + 2;
    }
    if (rest.starts_with("[") && rest.size() > 2) {
      // [1] footnote reference.
      const std::size_t close = rest.find(']');
      if (close != std::string_view::npos && close > 1) {
        const std::string label(rest.substr(1, close - 1));
        if (state.notes.contains(label)) {
          ir::Link link;
          link.note = label;
          link.url = "#fn-" + label;
          link.children = {ir::Text{label}};
          out.push_back(std::move(link));
          return close + 1;
        }
        // [^x^] bracketed delimiters for intra-word formatting.
        const char d = rest[1];
        if (std::string_view("*_-+^~@").find(d) != std::string_view::npos && close >= 3 && rest[close - 1] == d &&
            parser.depth() < 20) {
          const std::string_view inner = rest.substr(2, close - 3);
          ir::Inlines kids = d == '@' ? ir::Inlines{} : parser.parse(inner, parser.depth() + 1);
          switch (d) {
            case '*': out.push_back(ir::Strong{std::move(kids)}); break;
            case '_': out.push_back(ir::Emphasis{std::move(kids)}); break;
            case '-': out.push_back(ir::Strikethrough{std::move(kids)}); break;
            case '+': out.push_back(ir::Underline{std::move(kids)}); break;
            case '^': out.push_back(ir::Superscript{std::move(kids)}); break;
            case '~': out.push_back(ir::Subscript{std::move(kids)}); break;
            default: out.push_back(ir::Code{std::string(inner)}); break;
          }
          return close + 1;
        }
      }
    }
    if (rest.starts_with("\"") && parser.depth() < 20) {
      // "label":url
      const std::size_t close = rest.find("\":", 1);
      if (close != std::string_view::npos && close + 2 < rest.size() && !util::is_ascii_space(rest[close + 2])) {
        std::size_t end = close + 2;
        while (end < rest.size() && !util::is_ascii_space(rest[end])) ++end;
        while (end > close + 2 && std::string_view(".,;:!?)").find(rest[end - 1]) != std::string_view::npos) --end;
        const std::string url(rest.substr(close + 2, end - close - 2));
        const std::string_view label = rest.substr(1, close - 1);
        if (label.find('"') == std::string_view::npos) {
          ir::Inlines kids = label == "$" ? ir::Inlines{ir::Text{url}} : parser.parse(label, parser.depth() + 1);
          out.push_back(ir::Link{url, {}, std::move(kids)});
          return end;
        }
      }
    }
    if (rest.starts_with("!") && rest.size() > 2 && !util::is_ascii_space(rest[1])) {
      const std::size_t end = rest.find('!', 1);
      const std::string_view candidate = end == std::string_view::npos ? std::string_view{} : rest.substr(1, end - 1);
      const std::size_t paren = candidate.find('(');
      if (end != std::string_view::npos && candidate.substr(0, paren).find(' ') == std::string_view::npos &&
          (paren == std::string_view::npos || candidate.ends_with(")"))) {
        std::string_view inner = rest.substr(1, end - 1);
        std::string alt;
        if (inner.ends_with(")")) {
          const std::size_t open = inner.find('(');
          if (open != std::string_view::npos) {
            alt = std::string(inner.substr(open + 1, inner.size() - open - 2));
            inner = inner.substr(0, open);
          }
        }
        while (!inner.empty() && (inner.front() == '<' || inner.front() == '>' || inner.front() == '=')) inner.remove_prefix(1);
        out.push_back(ir::InlineImage{wiki::image_from_url(inner), alt});
        std::size_t used = end + 1;
        if (rest.substr(used).starts_with(":")) {
          // !img!:link
          std::size_t link_end = used + 1;
          while (link_end < rest.size() && !util::is_ascii_space(rest[link_end])) ++link_end;
          used = link_end;
        }
        return used;
      }
    }
    if (rest.starts_with("<")) {
      for (const std::string_view br : {"<br />", "<br/>", "<br>"}) {
        if (rest.starts_with(br)) {
          out.push_back(ir::LineBreak{});
          return br.size();
        }
      }
    }
    return wiki::common_inline(text, pos, out);
  };
  return syntax;
}

ir::Document read_textile(std::string_view text) {
  ir::Document document;
  TextileState state;
  std::vector<std::string_view> lines = util::split_lines(text);
  // Footnote definitions: "fnN. text" paragraphs.
  std::map<std::string, std::string> note_text;
  std::vector<std::string_view> body;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (auto sig = block_signature(lines[i]); sig && sig->tag.starts_with("fn") && sig->tag.size() > 2) {
      std::string content(sig->content);
      while (i + 1 < lines.size() && !is_blank(lines[i + 1])) content.append("\n").append(lines[++i]);
      const std::string label = sig->tag.substr(2);
      state.notes.insert(label);
      note_text[label] = std::move(content);
      continue;
    }
    body.push_back(lines[i]);
  }
  const InlineSyntax syntax = textile_inline(state);
  const auto inlines = [&](std::string_view t) {
    InlineParser parser(syntax);
    return parser.parse(util::trim(t));
  };

  ir::Blocks& out = document.blocks;
  std::vector<ir::Blocks*> targets{&out};
  std::size_t i = 0;
  std::vector<wiki::ListItem> items;
  const auto paragraph = [&](std::string_view t, ir::Attributes attr) {
    ir::Inlines content = inlines(t);
    if (content.empty()) return;
    if (content.size() == 1 && content.front().is<ir::InlineImage>()) {
      auto* image = content.front().get_if<ir::InlineImage>();
      out.push_back(ir::ImageBlock{std::move(image->source), std::move(image->alt), {}, {}});
      return;
    }
    out.push_back(ir::Block(ir::Paragraph{std::move(content)}, std::move(attr)));
  };
  while (i < body.size()) {
    const std::string_view line = body[i];
    if (is_blank(line)) {
      ++i;
      continue;
    }
    const std::string_view trimmed = util::trim(line);
    // Wrapper tags that pandoc emits around figures.
    if (trimmed == "<figure>" || trimmed == "</figure>" || trimmed == "<figcaption>" || trimmed == "</figcaption>" ||
        trimmed == "<div>" || trimmed == "</div>") {
      ++i;
      continue;
    }
    if (trimmed == "<hr />" || trimmed == "<hr/>" || trimmed == "<hr>" || is_repeated(trimmed, '-', 4)) {
      out.push_back(ir::HorizontalRule{});
      ++i;
      continue;
    }
    // Block-level HTML (definition lists, tables written as HTML).
    if (trimmed.starts_with("<dl") || trimmed.starts_with("<table") || trimmed.starts_with("<ul") ||
        trimmed.starts_with("<ol") || trimmed.starts_with("<blockquote") || trimmed.starts_with("<pre")) {
      const std::string tag(trimmed.substr(1, trimmed.find_first_of(" >") - 1));
      const std::string close = "</" + tag + ">";
      std::string html;
      std::size_t j = i;
      for (; j < body.size(); ++j) {
        html.append(body[j]).push_back('\n');
        if (body[j].find(close) != std::string_view::npos) break;
      }
      if (auto parsed = parse_html_text(html)) {
        for (ir::Block& b : parsed->blocks) out.push_back(std::move(b));
      }
      i = std::min(j, body.size() - 1) + 1;
      continue;
    }
    if (auto sig = block_signature(line)) {
      ir::Attributes attr;
      attr.id = sig->id;
      // Gather the block's lines: up to a blank line (extended blocks: up to the next signature).
      std::string content(sig->content);
      std::size_t j = i + 1;
      while (j < body.size()) {
        if (sig->extended) {
          if (block_signature(body[j]) && j > i + 1 && is_blank(body[j - 1])) break;
        } else if (is_blank(body[j])) {
          break;
        }
        content.append("\n").append(body[j]);
        ++j;
      }
      i = j;
      if (sig->tag.size() == 2 && sig->tag[0] == 'h') {
        out.push_back(ir::Block(ir::Heading{sig->tag[1] - '0', inlines(content)}, std::move(attr)));
      } else if (sig->tag == "bc" || sig->tag == "pre") {
        while (content.ends_with('\n')) content.pop_back();
        ir::CodeBlock code{sig->classes, html::decode_references(content, false)};
        out.push_back(std::move(code));
      } else if (sig->tag == "bq") {
        ir::BlockQuote quote;
        for (const std::string_view part : util::split(content, '\n')) (void)part;
        ir::Inlines q = inlines(content);
        if (!q.empty()) quote.blocks.push_back(ir::Paragraph{std::move(q)});
        out.push_back(std::move(quote));
      } else if (sig->tag == "notextile") {
        out.push_back(ir::Paragraph{{ir::Text{content}}});
      } else {
        paragraph(content, std::move(attr));
      }
      continue;
    }
    // Lists.
    const auto list_item = [](std::string_view l) -> std::optional<wiki::ListItem> {
      std::size_t n = 0;
      while (n < l.size() && (l[n] == '*' || l[n] == '#')) ++n;
      if (n == 0 || n >= l.size() || l[n] != ' ') return std::nullopt;
      wiki::ListItem item;
      item.depth = static_cast<int>(std::min<std::size_t>(n, 16));
      item.ordered = l[n - 1] == '#';
      item.text = std::string(util::trim(l.substr(n)));
      return item;
    };
    if (list_item(line) || (line.starts_with("- ") && line.find(":=") != std::string_view::npos)) {
      wiki::ReadSyntax list_syntax;
      list_syntax.list_item = [&list_item](std::string_view l) -> std::optional<wiki::ListItem> {
        if (auto item = list_item(l)) return item;
        if (l.starts_with("- ")) {
          const std::size_t sep = l.find(":=");
          if (sep == std::string_view::npos) return std::nullopt;
          wiki::ListItem term;
          term.term = true;
          term.text = std::string(util::trim(l.substr(2, sep - 2)));
          return term;
        }
        return std::nullopt;
      };
      list_syntax.inline_syntax = [&syntax](wiki::Context&) { return syntax; };
      list_syntax.list_item_continuation = false;
      std::vector<std::string_view> block;
      std::vector<std::string> expanded;
      while (i < body.size() && !is_blank(body[i]) && list_syntax.list_item(body[i])) {
        // "- term := definition" becomes a term line and a definition line.
        if (body[i].starts_with("- ") && !list_item(body[i])) {
          const std::size_t sep = body[i].find(":=");
          expanded.push_back(std::string(body[i].substr(0, sep)));
          expanded.push_back(":" + std::string(body[i].substr(sep + 2)));
        } else {
          expanded.emplace_back(body[i]);
        }
        ++i;
      }
      list_syntax.list_item = [&list_item](std::string_view l) -> std::optional<wiki::ListItem> {
        if (auto item = list_item(l)) return item;
        if (l.starts_with("- ")) {
          wiki::ListItem term;
          term.term = true;
          term.text = std::string(util::trim(l.substr(2)));
          return term;
        }
        if (l.starts_with(":")) {
          wiki::ListItem def;
          def.definition = true;
          def.text = std::string(util::trim(l.substr(1)));
          return def;
        }
        return std::nullopt;
      };
      for (const std::string& e : expanded) block.push_back(e);
      ir::Document scratch;
      wiki::Reader reader(list_syntax, scratch);
      for (ir::Block& b : reader.parse(block)) out.push_back(std::move(b));
      continue;
    }
    // Tables.
    if (trimmed.starts_with("|")) {
      ir::TableBlock table;
      while (i < body.size() && util::trim(body[i]).starts_with("|")) {
        const std::string_view row_text = util::trim(body[i]);
        ir::TableRow row;
        bool all_header = true;
        std::size_t k = 1;
        while (k < row_text.size()) {
          std::size_t end = row_text.find('|', k);
          if (end == std::string_view::npos) break;
          std::string_view cell_text = row_text.substr(k, end - k);
          ir::TableCell cell;
          bool header = false;
          // Cell attributes "_. ", "<. ", "\2. ", "/3. "
          const std::size_t dot = cell_text.find(". ");
          if (dot != std::string_view::npos && dot <= 8) {
            const std::string_view attrs = cell_text.substr(0, dot);
            bool valid = !attrs.empty();
            for (std::size_t a = 0; a < attrs.size() && valid; ++a) {
              const char c = attrs[a];
              if (c == '_') header = true;
              else if (c == '<') cell.align = ir::Alignment::Left;
              else if (c == '>') cell.align = ir::Alignment::Right;
              else if (c == '=') cell.align = ir::Alignment::Center;
              else if ((c == '\\' || c == '/') && a + 1 < attrs.size() && util::is_ascii_digit(attrs[a + 1])) {
                std::size_t d = a + 1;
                while (d < attrs.size() && util::is_ascii_digit(attrs[d])) ++d;
                const int span = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(attrs.substr(a + 1, d - a - 1)).value_or(1), 1, 1000));
                (c == '\\' ? cell.colspan : cell.rowspan) = span;
                a = d - 1;
              } else if (c != '^' && c != '~' && c != '-') {
                valid = false;
              }
            }
            if (valid) {
              cell_text = cell_text.substr(dot + 2);
            } else {
              header = false;
              cell.align = ir::Alignment::Default;
              cell.colspan = cell.rowspan = 1;
            }
          }
          if (!header) all_header = false;
          cell.content = inlines(cell_text);
          row.cells.push_back(std::move(cell));
          k = end + 1;
        }
        row.header = all_header && !row.cells.empty() && (table.rows.empty() || table.rows.back().header);
        if (!row.cells.empty()) table.rows.push_back(std::move(row));
        ++i;
      }
      wiki::infer_column_alignment(table);
      out.push_back(std::move(table));
      continue;
    }
    // Paragraph.
    std::string content(line);
    ++i;
    while (i < body.size() && !is_blank(body[i]) && !block_signature(body[i])) content.append("\n").append(body[i++]);
    paragraph(content, {});
  }
  for (const auto& [label, content] : note_text) {
    ir::Footnote note;
    note.label = label;
    note.blocks.push_back(ir::Paragraph{inlines(content)});
    document.footnotes.push_back(std::move(note));
  }
  if (!document.footnotes.empty()) number_footnotes(document);
  return document;
}

std::string textile_escape(std::string_view text) {
  std::string out = wiki::entity_escape(text, "*_-+^~@%?!\"|[]{}<>&=()");
  for (char& c : out) {
    if (c == '\n') c = ' ';
  }
  return out;
}

Result<std::string> write_textile(const ir::Document& document, WriteContext& /*ctx*/) {
  wiki::WriteSyntax syntax;
  syntax.raw_format = "textile";
  syntax.heading = [](int level, const std::string& text, const ir::Attributes& attr) {
    std::string out = "h" + std::to_string(level);
    if (!attr.id.empty()) {
      std::string id = attr.id;
      std::erase_if(id, [](char c) { return c == ')' || c == '(' || c == ' '; });
      out += "(#" + id + ")";
    }
    return out + ". " + text;
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
    std::string out;
    for (std::size_t k = 0; k < std::max<std::size_t>(definitions.size(), 1); ++k) {
      if (!out.empty()) out.push_back('\n');
      out += "- " + term + " := " + (k < definitions.size() ? definitions[k] : std::string());
    }
    return out;
  };
  syntax.code_block = [](const ir::CodeBlock& code) -> std::string {
    std::string lang = code.language;
    std::erase_if(lang, [](char c) { return !(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '+' || c == '-' || c == '_'); });
    const bool blank_lines = code.code.find("\n\n") != std::string::npos;
    return "bc" + (lang.empty() ? std::string() : "(" + lang + ")") + (blank_lines ? ".. " : ". ") + html::escape_text(code.code);
  };
  syntax.quote = [](const std::string& inner) { return "bq. " + util::replace_all(inner, "\n\n", " "); };
  syntax.rule = "<hr />";
  syntax.table = [](const ir::TableBlock& table, const std::function<std::string(const ir::Inlines&)>& render) {
    const TableGrid grid = table_grid(table);
    std::string out;
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
      if (!out.empty()) out.push_back('\n');
      std::size_t column = 0;
      out.push_back('|');
      for (const ir::TableCell& cell : table.rows[r].cells) {
        while (column < grid.columns && grid.cells[r][column] != &cell) ++column;
        std::string attrs;
        if (table.rows[r].header) attrs += "_";
        const ir::Alignment align = cell.align != ir::Alignment::Default ? cell.align : table.column_alignment(column);
        if (align == ir::Alignment::Left) attrs += "<";
        if (align == ir::Alignment::Right) attrs += ">";
        if (align == ir::Alignment::Center) attrs += "=";
        if (cell.colspan > 1) attrs += "\\" + std::to_string(cell.colspan);
        if (cell.rowspan > 1) attrs += "/" + std::to_string(cell.rowspan);
        if (!attrs.empty()) out.append(attrs).append(". ");
        out.append(render(cell.content)).push_back('|');
      }
    }
    return out;
  };
  syntax.image_block = [](const ir::ImageBlock& image, const std::string& url) -> std::string {
    if (url.starts_with("data:")) return textile_escape(image.caption.empty() ? image.alt : image.caption);
    std::string alt = image.caption.empty() ? image.alt : image.caption;
    std::erase_if(alt, [](char c) { return c == ')' || c == '(' || c == '!'; });
    return "!" + url + (alt.empty() ? "" : "(" + alt + ")") + "!";
  };
  syntax.notes_section = [](const std::vector<std::string>& bodies) {
    std::string out;
    for (std::size_t k = 0; k < bodies.size(); ++k) {
      if (!out.empty()) out.append("\n\n");
      out += "fn" + std::to_string(k + 1) + ". " + bodies[k];
    }
    return out;
  };
  syntax.item_break = "<br />";
  syntax.escape_line_start = [](const std::string& line) -> std::string {
    if (line.empty()) return line;
    if (block_signature(line) || line.front() == '*' || line.front() == '#' || line.front() == '|' ||
        line.starts_with("- ")) {
      return "&#" + std::to_string(static_cast<int>(static_cast<unsigned char>(line.front()))) + ";" + line.substr(1);
    }
    return line;
  };
  InlineMarkup m;
  m.escape = textile_escape;
  m.strong = {"*", "*"};
  m.emphasis = {"_", "_"};
  m.strikethrough = {"-", "-"};
  m.underline = {"+", "+"};
  m.superscript = {"[^", "^]"};
  m.subscript = {"[~", "~]"};
  m.line_break = "<br />";
  m.soft_break = " ";
  m.code = [](const ir::Code& code) -> std::string {
    if (code.raw_format == "textile") return code.text;
    if (!code.raw_format.empty()) return {};
    std::string text = html::escape_text(code.text);
    for (char& c : text) {
      if (c == '\n') c = ' ';
    }
    text = util::replace_all(text, "@", "&#64;");
    return "@" + text + "@";
  };
  m.link = [](const ir::Link& link, const std::string& label) -> std::string {
    std::string url = link.url;
    std::erase_if(url, [](char c) { return util::is_ascii_space(c); });
    if (label.empty() || label == textile_escape(link.url)) return "\"$\":" + url;
    return "\"" + util::replace_all(label, "\"", "&#34;") + "\":" + url;
  };
  m.image = [](const ir::InlineImage& image) -> std::string {
    const std::string url = image_url(image.source, false);
    if (url.empty()) return textile_escape(image.alt);
    std::string alt = image.alt;
    std::erase_if(alt, [](char c) { return c == ')' || c == '(' || c == '!'; });
    return "!" + url + (alt.empty() ? "" : "(" + alt + ")") + "!";
  };
  m.note = [](const ir::Link&, std::size_t index) { return "[" + std::to_string(index) + "]"; };
  wiki::Writer writer(document, syntax, m, 0);
  return writer.run();
}

int sniff_textile(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"textile"})) return 80;
  const std::string_view head = sniff_head(data);
  int score = 0;
  for (const std::string_view sig : {"h1. ", "h2. ", "h1(", "h2(", "bc. ", "bc(", "bq. ", "p. "}) {
    if (head.starts_with(sig) || head.find("\n" + std::string(sig)) != std::string_view::npos) score += 2;
  }
  if (head.find("\":http") != std::string_view::npos) score += 2;
  if (head.find("|_. ") != std::string_view::npos) score += 2;
  return score >= 6 ? 64 : 0;
}

}  // namespace

void register_textile(Registry& registry) {
  MarkupFormat textile;
  textile.id = "textile";
  textile.name = "Textile";
  textile.description = "Textile lightweight markup.";
  textile.extensions = {"textile"};
  textile.mime = "text/x-textile";
  textile.popularity = 15;
  textile.sniff = sniff_textile;
  textile.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_textile(text); };
  textile.write = write_textile;
  register_markup(registry, std::move(textile));
}

}  // namespace cvt::document
