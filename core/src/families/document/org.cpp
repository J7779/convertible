// Convertible core - document family: Emacs Org-mode markup.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

bool is_image_path(std::string_view target) {
  const std::string lower = util::to_lower(target);
  for (const std::string_view ext : {".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".bmp", ".tif", ".tiff"}) {
    if (lower.ends_with(ext)) return true;
  }
  return false;
}

std::optional<wiki::Heading> org_heading(std::string_view line) {
  std::size_t stars = 0;
  while (stars < line.size() && line[stars] == '*') ++stars;
  if (stars == 0 || stars >= line.size() || line[stars] != ' ') return std::nullopt;
  std::string_view text = util::trim(line.substr(stars));
  // TODO keywords and priority cookies are not part of the title.
  for (const std::string_view keyword : {"TODO ", "DONE "}) {
    if (text.starts_with(keyword)) text.remove_prefix(keyword.size());
  }
  if (text.starts_with("[#") && text.size() > 4 && text[3] == ']') text = util::trim(text.substr(4));
  // Tags ":a:b:" at the end.
  if (text.ends_with(":")) {
    const std::size_t space = text.rfind(' ');
    if (space != std::string_view::npos && text.substr(space + 1).starts_with(":")) text = util::trim(text.substr(0, space));
  }
  return wiki::Heading{static_cast<int>(std::min<std::size_t>(stars, 6)), std::string(text), {}};
}

std::optional<wiki::ListItem> org_list(std::string_view line) {
  const std::size_t indent = indentation(line);
  const std::string_view t = util::trim_left(line);
  std::size_t marker = 0;
  bool ordered = false;
  if (t.starts_with("- ") || t.starts_with("+ ") || (indent > 0 && t.starts_with("* "))) {
    marker = 1;
  } else {
    std::size_t digits = 0;
    while (digits < t.size() && digits < 9 && util::is_ascii_digit(t[digits])) ++digits;
    if (digits == 0 && !t.empty() && util::is_ascii_alpha(t[0]) && t.size() > 1 && (t[1] == '.' || t[1] == ')')) digits = 1;
    if (digits > 0 && digits + 1 < t.size() && (t[digits] == '.' || t[digits] == ')') && t[digits + 1] == ' ') {
      marker = digits + 1;
      ordered = true;
    }
  }
  if (marker == 0) return std::nullopt;
  wiki::ListItem item;
  item.depth = static_cast<int>(indent / 2) + 1;
  item.ordered = ordered;
  std::string_view text = util::trim(t.substr(marker));
  if (text.starts_with("[ ] ")) {
    item.task = ir::TaskState::Open;
    text.remove_prefix(4);
  } else if (text.starts_with("[X] ") || text.starts_with("[x] ") || text.starts_with("[-] ")) {
    item.task = ir::TaskState::Done;
    text.remove_prefix(4);
  }
  if (!ordered) {
    const std::size_t sep = text.find(" :: ");
    if (sep != std::string_view::npos || text.ends_with(" ::")) {
      item.term = true;
      const std::string term(util::trim(text.substr(0, sep == std::string_view::npos ? text.size() - 3 : sep)));
      const std::string definition = sep == std::string_view::npos ? std::string() : std::string(util::trim(text.substr(sep + 4)));
      item.text = term + (definition.empty() ? "" : "\n" + definition);
      return item;
    }
  }
  item.text = std::string(text);
  return item;
}

std::string block_name(std::string_view line) {
  const std::string lowered = util::to_lower(util::trim(line));
  if (!lowered.starts_with("#+begin_")) return {};
  const std::size_t end = lowered.find_first_of(" \t", 8);
  return lowered.substr(8, end == std::string::npos ? std::string::npos : end - 8);
}

std::vector<std::string> split_org_row(std::string_view line) {
  std::string_view t = util::trim(line);
  if (t.starts_with("|")) t.remove_prefix(1);
  if (t.ends_with("|")) t.remove_suffix(1);
  std::vector<std::string> cells;
  for (const std::string_view c : util::split(t, '|')) cells.emplace_back(util::trim(c));
  return cells;
}

std::size_t org_block(std::span<const std::string_view> lines, std::size_t at, ir::Blocks& out, wiki::Reader& reader) {
  const std::string_view line = lines[at];
  const std::string_view trimmed = util::trim(line);
  ir::Document& document = *reader.context().document;
  // Headings with a property drawer (CUSTOM_ID).
  if (auto heading = org_heading(line)) {
    std::size_t used = 1;
    ir::Attributes attr;
    if (at + 1 < lines.size() && util::trim(lines[at + 1]) == ":PROPERTIES:") {
      std::size_t i = at + 2;
      for (; i < lines.size() && util::trim(lines[i]) != ":END:"; ++i) {
        const std::string_view p = util::trim(lines[i]);
        if (util::istarts_with(p, ":CUSTOM_ID:")) attr.id = std::string(util::trim(p.substr(11)));
      }
      used = std::min(i, lines.size() - 1) - at + 1;
    }
    out.push_back(ir::Block(ir::Heading{heading->level, reader.inlines(heading->text)}, std::move(attr)));
    return used;
  }
  // Keywords.
  if (trimmed.starts_with("#+") && trimmed.find(':') != std::string_view::npos && block_name(trimmed).empty() &&
      !util::istarts_with(trimmed, "#+end_")) {
    const std::size_t colon = trimmed.find(':');
    const std::string key = util::to_lower(trimmed.substr(2, colon - 2));
    const std::string value(util::trim(trimmed.substr(colon + 1)));
    if (key == "title") document.metadata.title = value;
    else if (key == "author") document.metadata.author = value;
    else if (key == "date") document.metadata.date = value;
    else if (key == "language") document.metadata.language = value;
    else if (key == "caption" && at + 1 < lines.size()) {
      // A caption for the following picture.
      ir::Inlines next = reader.inlines(util::trim(lines[at + 1]));
      if (next.size() == 1 && next.front().is<ir::InlineImage>()) {
        auto* image = next.front().get_if<ir::InlineImage>();
        out.push_back(ir::ImageBlock{std::move(image->source), image->alt.empty() ? value : image->alt, value, {}});
        return 2;
      }
    } else if (key == "description" || key == "keywords" || key == "subtitle") {
      document.metadata.extra[key] = value;
    }
    return 1;
  }
  if (trimmed.starts_with(":PROPERTIES:")) {
    std::size_t i = at + 1;
    while (i < lines.size() && util::trim(lines[i]) != ":END:") ++i;
    return std::min(i, lines.size() - 1) - at + 1;
  }
  if (trimmed == "#" || trimmed.starts_with("# ")) return 1;  // comment
  // Blocks.
  if (const std::string name = block_name(trimmed); !name.empty()) {
    const std::string end = "#+end_" + name;
    std::size_t i = at + 1;
    std::vector<std::string_view> content;
    for (; i < lines.size(); ++i) {
      if (util::to_lower(util::trim(lines[i])).starts_with(end)) break;
      content.push_back(lines[i]);
    }
    if (i >= lines.size()) return 0;
    const std::string_view parameters = util::trim(trimmed.substr(8 + name.size()));
    if (name == "src" || name == "example") {
      // Strip the common indentation; ",*" escapes leading stars.
      std::size_t common = std::string::npos;
      for (const std::string_view l : content) {
        if (!is_blank(l)) common = std::min(common, indentation(l));
      }
      std::vector<std::string> code;
      for (const std::string_view l : content) {
        std::string s(strip_indent(l, common == std::string::npos ? 0 : common));
        if (s.starts_with(",*") || s.starts_with(",#+")) s.erase(0, 1);
        code.push_back(std::move(s));
      }
      ir::CodeBlock block;
      if (name == "src") block.language = std::string(parameters.substr(0, parameters.find(' ')));
      block.code = util::join(code, "\n");
      out.push_back(std::move(block));
    } else if (name == "export") {
      ir::CodeBlock raw{{}, join_lines(content)};
      raw.raw_format = std::string(parameters.substr(0, parameters.find(' ')));
      out.push_back(std::move(raw));
    } else if (name == "comment") {
      // dropped
    } else if (name == "verse") {
      ir::Inlines text;
      for (std::size_t k = 0; k < content.size(); ++k) {
        if (k > 0) text.push_back(ir::LineBreak{});
        for (ir::Inline& i2 : reader.inlines(util::trim(content[k]))) text.push_back(std::move(i2));
      }
      ir::Attributes attr;
      attr.add_class(ir::conventions::line_block);
      out.push_back(ir::Block(ir::BlockQuote{{ir::Block(ir::Paragraph{std::move(text)}, std::move(attr))}}));
    } else {
      ir::BlockQuote quote{reader.parse(content, 1)};
      ir::Attributes attr;
      if (name != "quote" && name != "center") {
        attr.add_class(ir::conventions::admonition);
        attr.add_class(name);
      }
      if (name == "center") {
        for (ir::Block& b : quote.blocks) out.push_back(std::move(b));
      } else {
        out.push_back(ir::Block(std::move(quote), std::move(attr)));
      }
    }
    return i - at + 1;
  }
  // Tables, with "|---+---|" separating the header.
  if (trimmed.starts_with("|")) {
    std::vector<std::vector<std::string>> rows;
    std::size_t header_rows = 0;
    std::size_t i = at;
    for (; i < lines.size() && util::trim(lines[i]).starts_with("|"); ++i) {
      const std::string_view row = util::trim(lines[i]);
      if (row.starts_with("|-")) {
        if (header_rows == 0 && !rows.empty()) header_rows = rows.size();
        continue;
      }
      rows.push_back(split_org_row(row));
    }
    ir::TableBlock table;
    for (std::size_t r = 0; r < rows.size(); ++r) {
      ir::TableRow row;
      row.header = r < header_rows && header_rows < rows.size();
      for (const std::string& cell : rows[r]) row.cells.push_back(ir::TableCell{reader.inlines(cell), 1, 1});
      table.rows.push_back(std::move(row));
    }
    if (!table.rows.empty()) out.push_back(std::move(table));
    return i - at;
  }
  // Footnote definitions "[fn:label] text".
  if (line.starts_with("[fn:")) {
    const std::size_t close = line.find(']');
    if (close != std::string_view::npos && close > 4) {
      std::string text(util::trim(line.substr(close + 1)));
      std::size_t i = at + 1;
      while (i < lines.size() && !is_blank(lines[i]) && !lines[i].starts_with("[fn:") && !org_heading(lines[i])) {
        text.append("\n").append(util::trim(lines[i]));
        ++i;
      }
      document.footnotes.push_back(ir::Footnote{std::string(line.substr(4, close - 4)), {ir::Paragraph{reader.inlines(text)}}});
      return i - at;
    }
  }
  return 0;
}

InlineSyntax org_inline(wiki::Context& context) {
  InlineSyntax syntax;
  syntax.rules = {
      {"*", "*", InlineKind::Strong},      {"/", "/", InlineKind::Emphasis},        {"_", "_", InlineKind::Underline},
      {"+", "+", InlineKind::Strikethrough}, {"=", "=", InlineKind::Code, true},     {"~", "~", InlineKind::Code, true},
  };
  syntax.hook = [&context](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    if (rest.starts_with("[[")) {
      const std::size_t end = rest.find("]]");
      if (end == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(2, end - 2);
      const std::size_t split = inner.find("][");
      std::string target(inner.substr(0, split));
      if (target.starts_with("file:")) target.erase(0, 5);
      if (split == std::string_view::npos && is_image_path(target)) {
        out.push_back(ir::InlineImage{wiki::image_from_url(target), {}});
        return end + 2;
      }
      ir::Inlines label = split == std::string_view::npos ? ir::Inlines{ir::Text{std::string(inner)}}
                                                          : parser.parse(inner.substr(split + 2), parser.depth() + 1);
      if (target.starts_with("#") || target.starts_with("*")) target = "#" + (target.starts_with("*") ? slugify(target.substr(1)) : target.substr(1));
      out.push_back(ir::Link{target, {}, std::move(label)});
      return end + 2;
    }
    if (rest.starts_with("[fn:")) {
      const std::size_t close = rest.find(']');
      if (close == std::string_view::npos) return 0;
      const std::string_view inner = rest.substr(4, close - 4);
      const std::size_t colon = inner.find(':');
      if (colon != std::string_view::npos) {
        // Inline definition [fn:label:text] or [fn::text].
        ir::Inlines body = parser.parse(inner.substr(colon + 1), parser.depth() + 1);
        out.push_back(context.add_note(wiki::paragraph_blocks(std::move(body))));
        return close + 1;
      }
      ir::Link link;
      link.note = std::string(inner);
      link.url = "#fn-" + link.note;
      link.children = {ir::Text{link.note}};
      out.push_back(std::move(link));
      return close + 1;
    }
    if ((rest.starts_with("^{") || rest.starts_with("_{")) && pos > 0 && !util::is_ascii_space(text[pos - 1])) {
      const std::size_t close = rest.find('}');
      if (close != std::string_view::npos && parser.depth() < 20) {
        ir::Inlines kids = parser.parse(rest.substr(2, close - 2), parser.depth() + 1);
        if (rest[0] == '^') out.push_back(ir::Superscript{std::move(kids)});
        else out.push_back(ir::Subscript{std::move(kids)});
        return close + 1;
      }
    }
    if (rest.starts_with("\\\\") && (rest.size() == 2 || rest[2] == '\n')) {
      out.push_back(ir::LineBreak{});
      return rest.size() == 2 ? 2 : 3;
    }
    if (rest.starts_with("\\(")) {
      const std::size_t end = rest.find("\\)");
      if (end == std::string_view::npos) return 0;
      ir::Code math{std::string(rest.substr(2, end - 2))};
      math.math = ir::MathMode::Inline;
      out.push_back(std::move(math));
      return end + 2;
    }
    if (rest.starts_with("\\")) {
      // Org entities for markup characters.
      static constexpr std::array<std::pair<std::string_view, std::string_view>, 12> entities = {{
          {"\\ast{}", "*"}, {"\\slash{}", "/"}, {"\\under{}", "_"}, {"\\plus{}", "+"}, {"\\equal{}", "="},
          {"\\tilde{}", "~"}, {"\\vert{}", "|"}, {"\\lbrack{}", "["}, {"\\rbrack{}", "]"},
          {"\\backslash{}", "\\"}, {"\\dollar{}", "$"}, {"\\asciicirc{}", "^"},
      }};
      for (const auto& [name, value] : entities) {
        if (rest.starts_with(name)) {
          append_text(out, value);
          return name.size();
        }
      }
    }
    if (rest.starts_with("$") && rest.size() > 2 && !util::is_ascii_space(rest[1])) {
      const std::size_t end = rest.find('$', 1);
      if (end != std::string_view::npos && !util::is_ascii_space(rest[end - 1]) &&
          (end + 1 >= rest.size() || !util::is_ascii_alpha(rest[end + 1]))) {
        ir::Code math{std::string(rest.substr(1, end - 1))};
        math.math = ir::MathMode::Inline;
        out.push_back(std::move(math));
        return end + 1;
      }
    }
    return wiki::common_inline(text, pos, out);
  };
  return syntax;
}

ir::Document read_org(std::string_view text) {
  wiki::ReadSyntax syntax;
  syntax.list_item = org_list;
  syntax.rule = [](std::string_view line) { return is_repeated(line, '-', 5); };
  syntax.block = org_block;
  syntax.inline_syntax = org_inline;
  ir::Document document = wiki::read(text, syntax);
  if (!document.footnotes.empty()) number_footnotes(document);
  // Zero-width spaces that protect line starts are not content.
  for_each_inlines(document, [](ir::Inlines& inlines) {
    for (ir::Inline& item : inlines) {
      if (auto* t = item.get_if<ir::Text>()) {
        if (t->text.starts_with("\xE2\x80\x8B")) t->text.erase(0, 3);
        t->text = util::replace_all(t->text, "\n\xE2\x80\x8B", "\n");
      }
    }
  });
  return document;
}

// ---- writing ----------------------------------------------------------------------------------

std::string org_escape(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    switch (c) {
      case '*': out.append("\\ast{}"); break;
      case '/': out.append(i + 1 < text.size() && text[i + 1] == '/' ? "/" : "\\slash{}"); break;
      case '_': out.append("\\under{}"); break;
      case '+': out.append("\\plus{}"); break;
      case '=': out.append("\\equal{}"); break;
      case '~': out.append("\\tilde{}"); break;
      case '|': out.append("\\vert{}"); break;
      case '[': out.append("\\lbrack{}"); break;
      case ']': out.append("\\rbrack{}"); break;
      case '\\': out.append("\\backslash{}"); break;
      case '$': out.append("\\dollar{}"); break;
      case '^': out.append("\\asciicirc{}"); break;
      default: out.push_back(c);
    }
  }
  // "//" (URLs) stay readable; the doubled slash is not emphasis.
  return out;
}

Result<std::string> write_org(const ir::Document& document, WriteContext& ctx) {
  wiki::WriteSyntax syntax;
  syntax.raw_format = "org";
  syntax.heading = [](int level, const std::string& text, const ir::Attributes& attr) {
    std::string out = wiki::repeat("*", level) + " " + text;
    if (!attr.id.empty()) out += "\n:PROPERTIES:\n:CUSTOM_ID: " + attr.id + "\n:END:";
    return out;
  };
  syntax.list_marker = [](const std::vector<bool>& path, int number, ir::TaskState task) {
    std::string out(2 * (path.size() - 1), ' ');
    out += path.back() ? std::to_string(number) + ". " : "- ";
    if (task == ir::TaskState::Done) out += "[X] ";
    if (task == ir::TaskState::Open) out += "[ ] ";
    return out;
  };
  syntax.definition = [](const std::string& term, const std::vector<std::string>& definitions) {
    std::string out = "- " + term + " ::";
    for (std::size_t k = 0; k < definitions.size(); ++k) out += (k == 0 ? " " : " ") + definitions[k];
    return out;
  };
  syntax.code_block = [](const ir::CodeBlock& code) -> std::string {
    std::string body;
    for (const std::string_view line : util::split(code.code, '\n')) {
      if (!body.empty()) body.push_back('\n');
      if (line.starts_with("*") || line.starts_with("#+")) body.push_back(',');
      body.append(line);
    }
    if (code.math != ir::MathMode::None) return "\\[\n" + code.code + "\n\\]";
    std::string lang = code.language;
    std::erase_if(lang, [](char c) { return util::is_ascii_space(c); });
    if (lang.empty()) return "#+begin_example\n" + body + "\n#+end_example";
    return "#+begin_src " + lang + "\n" + body + "\n#+end_src";
  };
  syntax.quote = [](const std::string& inner) { return "#+begin_quote\n" + inner + "\n#+end_quote"; };
  syntax.rule = "-----";
  syntax.table = [](const ir::TableBlock& table, const std::function<std::string(const ir::Inlines&)>& render) {
    const auto cells = table_strings(table, render);
    std::size_t columns = 0;
    for (const auto& row : cells) columns = std::max(columns, row.size());
    std::vector<std::size_t> widths(columns, 1);
    for (const auto& row : cells) {
      for (std::size_t c = 0; c < row.size(); ++c) widths[c] = std::max(widths[c], display_width(row[c]));
    }
    std::string out;
    const std::size_t header = header_row_count(table);
    for (std::size_t r = 0; r < cells.size(); ++r) {
      if (!out.empty()) out.push_back('\n');
      out.push_back('|');
      for (std::size_t c = 0; c < columns; ++c) {
        out.append(" ").append(pad_to(c < cells[r].size() ? cells[r][c] : std::string(), widths[c])).append(" |");
      }
      if (header > 0 && r + 1 == header) {
        out.append("\n|");
        for (std::size_t c = 0; c < columns; ++c) out.append(std::string(widths[c] + 2, '-')).append(c + 1 < columns ? "+" : "|");
      }
    }
    return out;
  };
  syntax.image_block = [](const ir::ImageBlock& image, const std::string& url) -> std::string {
    if (url.starts_with("data:")) return org_escape(image.caption.empty() ? image.alt : image.caption);
    std::string out;
    if (!image.caption.empty()) out = "#+caption: " + org_escape(image.caption) + "\n";
    return out + "[[file:" + url + "]]";
  };
  syntax.notes_section = [](const std::vector<std::string>& bodies) {
    std::string out;
    for (std::size_t k = 0; k < bodies.size(); ++k) {
      if (!out.empty()) out.append("\n\n");
      out += "[fn:" + std::to_string(k + 1) + "] " + bodies[k];
    }
    return out;
  };
  syntax.item_break = " \\\\ ";
  syntax.escape_line_start = [](const std::string& line) -> std::string {
    if (line.starts_with("#") || line.starts_with("-") || line.starts_with(":") ||
        (!line.empty() && util::is_ascii_digit(line.front()))) {
      return "\xE2\x80\x8B" + line;  // a zero-width space keeps the line from starting a block
    }
    return line;
  };
  InlineMarkup m;
  m.escape = org_escape;
  m.strong = {"*", "*"};
  m.emphasis = {"/", "/"};
  m.underline = {"_", "_"};
  m.strikethrough = {"+", "+"};
  m.superscript = {"^{", "}"};
  m.subscript = {"_{", "}"};
  m.line_break = "\\\\\n";
  m.soft_break = " ";
  m.code = [](const ir::Code& code) -> std::string {
    if (code.math != ir::MathMode::None) return "\\(" + code.text + "\\)";
    if (code.raw_format == "org") return code.text;
    if (!code.raw_format.empty()) return {};
    std::string text = code.text;
    for (char& c : text) {
      if (c == '\n') c = ' ';
    }
    if (text.find('~') == std::string::npos) return "~" + text + "~";
    if (text.find('=') == std::string::npos) return "=" + text + "=";
    return org_escape(text);
  };
  m.link = [](const ir::Link& link, const std::string& label) -> std::string {
    std::string url = link.url;
    std::erase_if(url, [](char c) { return c == '[' || c == ']' || c == '\n'; });
    if (label == org_escape(link.url)) return "[[" + url + "]]";
    return "[[" + url + "][" + label + "]]";
  };
  m.image = [](const ir::InlineImage& image) -> std::string {
    const std::string url = image_url(image.source, false);
    if (url.empty()) return org_escape(image.alt);
    return "[[file:" + url + "]]";
  };
  m.note = [](const ir::Link&, std::size_t index) { return "[fn:" + std::to_string(index) + "]"; };
  wiki::Writer writer(document, syntax, m, static_cast<int>(ctx.options.get_int("wrap_width", 0, 0, 1000)));
  std::string out;
  const ir::DocumentMetadata& meta = document.metadata;
  if (!meta.title.empty()) out += "#+title: " + meta.title + "\n";
  if (!meta.author.empty()) out += "#+author: " + meta.author + "\n";
  if (!meta.date.empty()) out += "#+date: " + meta.date + "\n";
  if (!meta.language.empty()) out += "#+language: " + meta.language + "\n";
  if (!out.empty()) out += "\n";
  return out + writer.run();
}

int sniff_org(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"org"})) return 80;
  const std::string_view head = sniff_head(data);
  const std::string lowered = util::to_lower(head.substr(0, 2048));
  int score = 0;
  if (lowered.starts_with("#+title:") || lowered.find("\n#+title:") != std::string::npos) score += 4;
  if (lowered.find("#+begin_src") != std::string::npos) score += 3;
  if (lowered.find(":properties:") != std::string::npos) score += 2;
  if (head.starts_with("* ") || head.find("\n* ") != std::string_view::npos) score += 1;
  return score >= 5 ? 70 : 0;
}

}  // namespace

void register_org(Registry& registry) {
  MarkupFormat org;
  org.id = "org";
  org.name = "Org-mode";
  org.description = "Emacs Org-mode outline markup.";
  org.extensions = {"org"};
  org.mime = "text/org";
  org.popularity = 25;
  org.sniff = sniff_org;
  org.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_org(text); };
  org.write = write_org;
  org.features = feature_wrap;
  register_markup(registry, std::move(org));
}

}  // namespace cvt::document
