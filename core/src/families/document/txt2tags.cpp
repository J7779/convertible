// Convertible core - document family: txt2tags markup.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

std::optional<wiki::Heading> t2t_heading(std::string_view line) {
  const std::string_view t = util::trim(line);
  if (t.size() < 3) return std::nullopt;
  const char mark = t.front();
  if (mark != '=' && mark != '+') return std::nullopt;
  std::size_t open = 0;
  while (open < t.size() && t[open] == mark) ++open;
  std::string_view body = t;
  std::string id;
  // Optional [label] after the closing marks.
  if (body.ends_with("]")) {
    const std::size_t bracket = body.rfind('[');
    if (bracket != std::string_view::npos && bracket > 0 && body[bracket - 1] == mark) {
      id = std::string(body.substr(bracket + 1, body.size() - bracket - 2));
      body = body.substr(0, bracket);
    }
  }
  std::size_t close = 0;
  while (close < body.size() && body[body.size() - 1 - close] == mark) ++close;
  if (open != close || open > 5 || 2 * open >= body.size()) return std::nullopt;
  const std::string_view text = body.substr(open, body.size() - 2 * open);
  if (text.empty() || text.front() != ' ' || text.back() != ' ') return std::nullopt;
  return wiki::Heading{static_cast<int>(open), std::string(util::trim(text)), id};
}

std::optional<wiki::ListItem> t2t_list(std::string_view line) {
  std::size_t spaces = 0;
  while (spaces < line.size() && line[spaces] == ' ') ++spaces;
  if (spaces + 1 >= line.size()) return std::nullopt;
  const char marker = line[spaces];
  if ((marker != '-' && marker != '+' && marker != ':') || line[spaces + 1] != ' ') return std::nullopt;
  wiki::ListItem item;
  item.depth = static_cast<int>(spaces) + 1;
  item.ordered = marker == '+';
  item.term = marker == ':';
  item.text = std::string(util::trim(line.substr(spaces + 2)));
  return item;
}

std::optional<std::vector<wiki::Cell>> t2t_row(std::string_view line) {
  std::string_view t = util::trim_left(line);
  if (!t.starts_with("|")) return std::nullopt;
  const bool header = t.starts_with("||");
  t.remove_prefix(header ? 2 : 1);
  std::vector<wiki::Cell> cells;
  std::size_t start = 0;
  while (start < t.size()) {
    std::size_t end = t.find('|', start);
    if (end == std::string_view::npos) end = t.size();
    const std::string_view raw = t.substr(start, end - start);
    std::size_t bars = 0;
    while (end + bars < t.size() && t[end + bars] == '|') ++bars;
    if (!(end == t.size() && util::trim(raw).empty())) {
      wiki::Cell cell;
      cell.header = header;
      cell.colspan = bars > 1 ? static_cast<int>(bars) : 1;
      const std::size_t left = raw.size() - util::trim_left(raw).size();
      const std::size_t right = raw.size() - util::trim_right(raw).size();
      if (left > 1 && right > 1) cell.align = ir::Alignment::Center;
      else if (left > 1) cell.align = ir::Alignment::Right;
      else if (right > 1) cell.align = ir::Alignment::Left;
      cell.text = std::string(util::trim(raw));
      cells.push_back(std::move(cell));
    }
    start = end + std::max<std::size_t>(bars, 1);
  }
  if (cells.empty()) return std::nullopt;
  return cells;
}

std::size_t t2t_block(std::span<const std::string_view> lines, std::size_t at, ir::Blocks& out, wiki::Reader&) {
  const std::string_view line = lines[at];
  if (line.starts_with("%")) return 1;  // comment or setting
  for (const std::string_view fence : {"```", "\"\"\"", "'''"}) {
    if (util::trim_right(line) != fence) continue;
    std::vector<std::string_view> content;
    for (std::size_t i = at + 1; i < lines.size(); ++i) {
      if (util::trim_right(lines[i]) == fence) {
        ir::CodeBlock code{{}, join_lines(content)};
        if (fence != "```") code.raw_format = fence == "'''" ? "t2t" : "t2t-raw";
        if (fence == "\"\"\"") {
          code.raw_format.clear();
          out.push_back(ir::Paragraph{{ir::Text{code.code}}});
        } else {
          out.push_back(std::move(code));
        }
        return i - at + 1;
      }
      content.push_back(lines[i]);
    }
    return 0;
  }
  if (line.starts_with("``` ")) {
    out.push_back(ir::CodeBlock{{}, std::string(line.substr(4))});
    return 1;
  }
  return 0;
}

InlineSyntax t2t_inline(wiki::Context&) {
  InlineSyntax syntax;
  syntax.rules = {
      {"**", "**", InlineKind::Strong, false, true, true},      {"//", "//", InlineKind::Emphasis, false, true, true},
      {"__", "__", InlineKind::Underline, false, true, true},   {"--", "--", InlineKind::Strikethrough, false, true, true},
      {"``", "``", InlineKind::Code, true, true, true},
  };
  syntax.hook = [](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    if (rest.starts_with("\"\"")) {
      const std::size_t end = rest.find("\"\"", 2);
      if (end == std::string_view::npos) return 0;
      append_text(out, rest.substr(2, end - 2));
      return end + 2;
    }
    if (rest.starts_with("''")) {
      const std::size_t end = rest.find("''", 2);
      if (end == std::string_view::npos) return 0;
      ir::Code raw{std::string(rest.substr(2, end - 2))};
      raw.raw_format = "html";
      out.push_back(std::move(raw));
      return end + 2;
    }
    if (rest.starts_with("<sup>") || rest.starts_with("<sub>")) {
      const bool sup = rest.starts_with("<sup>");
      const std::size_t end = rest.find(sup ? "</sup>" : "</sub>");
      if (end != std::string_view::npos && parser.depth() < 20) {
        ir::Inlines kids = parser.parse(rest.substr(5, end - 5), parser.depth() + 1);
        if (sup) out.push_back(ir::Superscript{std::move(kids)});
        else out.push_back(ir::Subscript{std::move(kids)});
        return end + 6;
      }
    }
    if (rest.starts_with("[") && rest.size() > 2) {
      const std::size_t end = rest.find(']');
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(1, end - 1);
      const std::size_t space = inner.rfind(' ');
      const auto image_name = [](std::string_view s) {
        const std::string lower = util::to_lower(s);
        return lower.ends_with(".png") || lower.ends_with(".jpg") || lower.ends_with(".jpeg") || lower.ends_with(".gif") ||
               lower.ends_with(".svg") || lower.ends_with(".webp");
      };
      if (space == std::string_view::npos) {
        if (inner.find('|') != std::string_view::npos) {
          const std::size_t bar = inner.find('|');
          if (image_name(inner.substr(0, bar))) {
            out.push_back(ir::InlineImage{wiki::image_from_url(inner.substr(0, bar)), std::string(inner.substr(bar + 1))});
            return end + 1;
          }
        }
        if (image_name(inner)) {
          out.push_back(ir::InlineImage{wiki::image_from_url(inner), {}});
          return end + 1;
        }
        return 0;
      }
      const std::string_view url = inner.substr(space + 1);
      if (url.find('.') == std::string_view::npos && url.find(':') == std::string_view::npos && !url.starts_with("#")) return 0;
      out.push_back(ir::Link{std::string(url), {}, parser.parse(inner.substr(0, space), parser.depth() + 1)});
      return end + 1;
    }
    return wiki::common_inline(text, pos, out);
  };
  return syntax;
}

ir::Document read_t2t(std::string_view text) {
  wiki::ReadSyntax syntax;
  syntax.heading = t2t_heading;
  syntax.list_item = t2t_list;
  syntax.rule = [](std::string_view line) {
    const std::string_view t = util::trim(line);
    return t.size() >= 20 && (is_repeated(t, '-', 20) || is_repeated(t, '=', 20) || is_repeated(t, '_', 20));
  };
  syntax.table_row = t2t_row;
  syntax.quote_line = [](std::string_view line) -> std::optional<std::string> {
    if (line.starts_with("\t")) return std::string(line.substr(1));
    return std::nullopt;
  };
  syntax.block = t2t_block;
  syntax.inline_syntax = t2t_inline;
  syntax.list_item_continuation = true;
  // The header: up to three lines (title, author, date) unless the first is empty.
  std::vector<std::string_view> lines = util::split_lines(text);
  ir::Document document;
  std::size_t start = 0;
  if (!lines.empty() && !is_blank(lines[0]) && !t2t_heading(lines[0]) && !lines[0].starts_with("%")) {
    // The header is always the first three lines (title, author, date).
    document.metadata.title = trim_copy(lines[0]);
    if (lines.size() > 1) document.metadata.author = trim_copy(lines[1]);
    if (lines.size() > 2) document.metadata.date = trim_copy(lines[2]);
    start = std::min<std::size_t>(3, lines.size());
  } else if (!lines.empty() && is_blank(lines[0])) {
    start = 1;
  }
  wiki::Reader reader(syntax, document);
  document.blocks = reader.parse(std::span<const std::string_view>(lines).subspan(start));
  return document;
}

std::string t2t_escape(std::string_view text) {
  // txt2tags has no escape character: risky runs are protected as raw text.
  std::string out;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    const bool pair = i + 1 < text.size() && text[i + 1] == c && std::string_view("*/_-`\"'").find(c) != std::string_view::npos;
    if (pair || c == '[' || c == ']' || c == '|' || (c == ':' && text.substr(i).starts_with("://"))) {
      std::size_t end = i + 1;
      while (end < text.size() && std::string_view("*/_-`\"'[]|:").find(text[end]) != std::string_view::npos) ++end;
      const std::string_view run = text.substr(i, end - i);
      if (run.find("\"\"") == std::string_view::npos) {
        out.append("\"\"").append(run).append("\"\"");
      } else {
        for (const char x : run) out.append("\"\"").append(1, x).append("\"\"");
      }
      i = end;
      continue;
    }
    out.push_back(c == '\n' ? ' ' : c);
    ++i;
  }
  return out;
}

Result<std::string> write_t2t(const ir::Document& document, WriteContext& /*ctx*/) {
  ir::Document copy = document;
  footnotes_to_endnotes(copy);
  wiki::WriteSyntax syntax;
  syntax.raw_format = "t2t";
  syntax.heading = [](int level, const std::string& text, const ir::Attributes& attr) {
    const std::string marks = wiki::repeat("=", std::min(level, 5));
    std::string id;
    if (!attr.id.empty()) {
      id = attr.id;
      std::erase_if(id, [](char c) { return !(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '-' || c == '_'); });
    }
    return marks + " " + text + " " + marks + (id.empty() ? "" : "[" + id + "]");
  };
  syntax.list_marker = [](const std::vector<bool>& path, int, ir::TaskState task) {
    std::string out(path.size() - 1, ' ');
    out.push_back(path.back() ? '+' : '-');
    out.push_back(' ');
    if (task == ir::TaskState::Done) out += "\xE2\x98\x91 ";
    if (task == ir::TaskState::Open) out += "\xE2\x98\x90 ";
    return out;
  };
  syntax.definition = [](const std::string& term, const std::vector<std::string>& definitions) {
    std::string out = ": " + term;
    for (const std::string& d : definitions) out += "\n" + d;
    return out;
  };
  syntax.code_block = [](const ir::CodeBlock& code) { return "```\n" + code.code + "\n```"; };
  syntax.quote = [](const std::string& inner) {
    std::string out;
    for (const std::string_view line : util::split(inner, '\n')) {
      if (!out.empty()) out.push_back('\n');
      if (!line.empty()) out.append("\t").append(line);
    }
    return out;
  };
  syntax.rule = "--------------------";
  syntax.table = [](const ir::TableBlock& table, const std::function<std::string(const ir::Inlines&)>& render) {
    const auto cells = table_strings(table, render);
    std::string out;
    for (std::size_t r = 0; r < cells.size(); ++r) {
      if (!out.empty()) out.push_back('\n');
      out.append(table.rows[r].header ? "|| " : "| ");
      for (std::size_t c = 0; c < cells[r].size(); ++c) {
        if (c > 0) out.append(" | ");
        out.append(cells[r][c].empty() ? " " : cells[r][c]);
      }
      out.append(" |");
    }
    return out;
  };
  syntax.image_block = [](const ir::ImageBlock& image, const std::string& url) -> std::string {
    if (url.starts_with("data:") || url.find(' ') != std::string::npos) return t2t_escape(image.alt);
    return "[" + url + "]";
  };
  syntax.item_break = " ";
  syntax.escape_line_start = [](const std::string& line) -> std::string {
    if (line.empty()) return line;
    if (t2t_heading(line) || t2t_list(line) || line.front() == '%' || line.front() == '|' || line.front() == '\t') {
      return "\"\"" + line.substr(0, 1) + "\"\"" + line.substr(1);
    }
    return line;
  };
  InlineMarkup m;
  m.escape = t2t_escape;
  m.strong = {"**", "**"};
  m.emphasis = {"//", "//"};
  m.underline = {"__", "__"};
  m.strikethrough = {"--", "--"};
  m.superscript = {"<sup>", "</sup>"};
  m.subscript = {"<sub>", "</sub>"};
  m.line_break = " ";
  m.soft_break = " ";
  m.code = [](const ir::Code& code) -> std::string {
    if (!code.raw_format.empty()) return code.raw_format == "html" ? "''" + code.text + "''" : std::string();
    std::string text = code.text;
    for (char& c : text) {
      if (c == '\n') c = ' ';
    }
    if (text.find("``") != std::string::npos || text.empty()) return t2t_escape(text);
    return "``" + text + "``";
  };
  m.link = [](const ir::Link& link, const std::string& label) -> std::string {
    std::string url = link.url;
    std::erase_if(url, [](char c) { return util::is_ascii_space(c) || c == ']'; });
    if (label == t2t_escape(link.url)) return url;
    std::string text = label;
    std::erase_if(text, [](char c) { return c == ']'; });
    return "[" + text + " " + url + "]";
  };
  m.image = [](const ir::InlineImage& image) -> std::string {
    const std::string url = image_url(image.source, false);
    if (url.empty() || url.find(' ') != std::string::npos) return t2t_escape(image.alt);
    return "[" + url + "]";
  };
  wiki::Writer writer(copy, syntax, m, 0);
  std::string body = writer.run();
  // Header area: title, (empty) author and date lines, then a blank line.
  std::string header = copy.metadata.title.empty() ? "\n" : copy.metadata.title + "\n";
  if (!copy.metadata.title.empty()) {
    header += copy.metadata.author + "\n" + copy.metadata.date + "\n";
  }
  return header + "\n" + body;
}

int sniff_t2t(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"t2t"})) return 80;
  const std::string_view head = sniff_head(data);
  return head.find("\n%!target") != std::string_view::npos || head.find("\n%!encoding") != std::string_view::npos ? 70 : 0;
}

}  // namespace

void register_txt2tags(Registry& registry) {
  MarkupFormat t2t;
  t2t.id = "t2t";
  t2t.name = "txt2tags";
  t2t.description = "txt2tags lightweight markup.";
  t2t.extensions = {"t2t"};
  t2t.mime = "text/x-txt2tags";
  t2t.popularity = 8;
  t2t.sniff = sniff_t2t;
  t2t.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_t2t(text); };
  t2t.write = write_t2t;
  t2t.write_lossy = true;  // no notes (end notes), no line breaks
  register_markup(registry, std::move(t2t));
}

}  // namespace cvt::document
