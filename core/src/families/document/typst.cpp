// Convertible core - document family: Typst markup (writer; basic reader).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

// ---- reading (common markup and functions) ------------------------------------------------------

std::size_t matching(std::string_view text, std::size_t open, char open_char, char close_char) {
  int depth = 0;
  bool quoted = false;
  for (std::size_t i = open; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\\') {
      ++i;
      continue;
    }
    if (c == '"' && open_char == '(') quoted = !quoted;
    if (quoted) continue;
    if (c == open_char) ++depth;
    if (c == close_char && --depth == 0) return i;
  }
  return std::string_view::npos;
}

std::string string_literal(std::string_view text) {
  const std::size_t open = text.find('"');
  if (open == std::string_view::npos) return {};
  std::string out;
  for (std::size_t i = open + 1; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size()) {
      out.push_back(text[++i]);
      continue;
    }
    if (text[i] == '"') break;
    out.push_back(text[i]);
  }
  return out;
}

InlineSyntax typst_inline(wiki::Context& context) {
  InlineSyntax syntax;
  syntax.escape = '\\';
  syntax.rules = {
      {"*", "*", InlineKind::Strong, false, true, true},
      {"_", "_", InlineKind::Emphasis, false, true, true},
      {"$", "$", InlineKind::Math, true, false, true},
  };
  syntax.hook = [&context](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    if (rest.starts_with("\\\n") || rest == "\\") {
      out.push_back(ir::LineBreak{});
      return rest.size() >= 2 ? 2 : 1;
    }
    if (rest.starts_with("`")) {
      std::size_t ticks = 0;
      while (ticks < rest.size() && rest[ticks] == '`') ++ticks;
      if (ticks == 2) {
        out.push_back(ir::Code{});
        return 2;
      }
      const std::string fence(ticks, '`');
      const std::size_t end = rest.find(fence, ticks);
      if (end == std::string_view::npos) return 0;
      std::string code(rest.substr(ticks, end - ticks));
      if (ticks >= 3) {
        // ```lang code```
        const std::size_t space = code.find_first_of(" \n");
        code = space == std::string::npos ? std::string() : std::string(util::trim(std::string_view(code).substr(space)));
      }
      out.push_back(ir::Code{std::move(code)});
      return end + ticks;
    }
    if (rest.starts_with("//")) {
      const std::size_t end = rest.find('\n');
      return end == std::string_view::npos ? rest.size() : end;
    }
    if (rest.starts_with("<") && rest.find('>') != std::string_view::npos && rest.find('>') < 64 &&
        rest.substr(1, rest.find('>') - 1).find(' ') == std::string_view::npos) {
      return rest.find('>') + 1;  // label
    }
    if (rest.starts_with("#")) {
      std::size_t name_end = 1;
      while (name_end < rest.size() && (util::is_ascii_alpha(rest[name_end]) || rest[name_end] == '.' || rest[name_end] == '-')) ++name_end;
      const std::string name(rest.substr(1, name_end - 1));
      if (name.empty()) return 0;
      std::size_t i = name_end;
      std::string arguments;
      if (i < rest.size() && rest[i] == '(') {
        const std::size_t close = matching(rest, i, '(', ')');
        if (close == std::string_view::npos) return 0;
        arguments = std::string(rest.substr(i + 1, close - i - 1));
        i = close + 1;
      }
      std::string_view body;
      bool has_body = false;
      if (i < rest.size() && rest[i] == '[') {
        const std::size_t close = matching(rest, i, '[', ']');
        if (close == std::string_view::npos) return 0;
        body = rest.substr(i + 1, close - i - 1);
        has_body = true;
        i = close + 1;
      }
      const auto kids = [&] { return parser.depth() < 20 ? parser.parse(body, parser.depth() + 1) : ir::Inlines{ir::Text{std::string(body)}}; };
      if (name == "emph") out.push_back(ir::Emphasis{kids()});
      else if (name == "strong") out.push_back(ir::Strong{kids()});
      else if (name == "strike") out.push_back(ir::Strikethrough{kids()});
      else if (name == "underline") out.push_back(ir::Underline{kids()});
      else if (name == "super") out.push_back(ir::Superscript{kids()});
      else if (name == "sub") out.push_back(ir::Subscript{kids()});
      else if (name == "highlight") {
        ir::Attributes attr;
        attr.add_class("mark");
        out.push_back(ir::Inline(ir::Strong{kids()}, std::move(attr)));
      } else if (name == "link") {
        const std::string url = string_literal(arguments);
        out.push_back(ir::Link{url, {}, has_body ? kids() : ir::Inlines{ir::Text{url}}});
      } else if (name == "footnote") {
        out.push_back(context.add_note(wiki::paragraph_blocks(kids())));
      } else if (name == "image") {
        std::string alt;
        if (const std::size_t at = arguments.find("alt:"); at != std::string::npos) alt = string_literal(std::string_view(arguments).substr(at));
        out.push_back(ir::InlineImage{wiki::image_from_url(string_literal(arguments)), alt});
      } else if (name == "raw") {
        out.push_back(ir::Code{string_literal(arguments)});
      } else if (name == "linebreak") {
        out.push_back(ir::LineBreak{});
      } else if (has_body) {
        for (ir::Inline& k : kids()) out.push_back(std::move(k));
      }
      return i;
    }
    return wiki::common_inline(text, pos, out);
  };
  return syntax;
}

std::optional<wiki::Heading> typst_heading(std::string_view line) {
  std::size_t marks = 0;
  while (marks < line.size() && line[marks] == '=') ++marks;
  if (marks == 0 || marks > 6 || marks >= line.size() || line[marks] != ' ') return std::nullopt;
  std::string text(util::trim(line.substr(marks)));
  std::string id;
  if (text.ends_with(">")) {
    const std::size_t lt = text.rfind('<');
    if (lt != std::string::npos) {
      id = text.substr(lt + 1, text.size() - lt - 2);
      text = std::string(util::trim(std::string_view(text).substr(0, lt)));
    }
  }
  return wiki::Heading{static_cast<int>(marks), text, id};
}

std::optional<wiki::ListItem> typst_list(std::string_view line) {
  const std::size_t indent = indentation(line);
  const std::string_view t = util::trim_left(line);
  if (t.size() < 2) return std::nullopt;
  wiki::ListItem item;
  item.depth = static_cast<int>(indent / 2) + 1;
  if ((t[0] == '-' || t[0] == '+') && t[1] == ' ') {
    item.ordered = t[0] == '+';
    item.text = std::string(util::trim(t.substr(2)));
    return item;
  }
  if (t[0] == '/' && t[1] == ' ') {
    const std::size_t colon = t.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    item.term = true;
    item.text = std::string(util::trim(t.substr(2, colon - 2))) + "\n" + std::string(util::trim(t.substr(colon + 1)));
    return item;
  }
  std::size_t digits = 0;
  while (digits < t.size() && util::is_ascii_digit(t[digits])) ++digits;
  if (digits > 0 && digits + 1 < t.size() && t[digits] == '.' && t[digits + 1] == ' ') {
    item.ordered = true;
    item.text = std::string(util::trim(t.substr(digits + 2)));
    return item;
  }
  return std::nullopt;
}

// Splits "a, b, [c, d]" at top-level commas.
std::vector<std::string_view> split_arguments(std::string_view text) {
  std::vector<std::string_view> out;
  int depth = 0;
  bool quoted = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\\') {
      ++i;
      continue;
    }
    if (c == '"') quoted = !quoted;
    if (quoted) continue;
    if (c == '(' || c == '[' || c == '{') ++depth;
    if (c == ')' || c == ']' || c == '}') --depth;
    if (c == ',' && depth == 0) {
      out.push_back(util::trim(text.substr(start, i - start)));
      start = i + 1;
    }
  }
  if (start < text.size() && !util::trim(text.substr(start)).empty()) out.push_back(util::trim(text.substr(start)));
  return out;
}

void typst_table(std::string_view arguments, ir::Blocks& out, wiki::Reader& reader) {
  ir::TableBlock table;
  std::size_t columns = 1;
  std::vector<std::string_view> header;
  std::vector<std::string_view> cells;
  for (const std::string_view arg : split_arguments(arguments)) {
    if (arg.starts_with("columns:")) {
      const std::string_view value = util::trim(arg.substr(8));
      if (const auto n = util::parse_int(value)) {
        columns = static_cast<std::size_t>(std::clamp<std::int64_t>(*n, 1, 1000));
      } else if (value.starts_with("(")) {
        columns = std::max<std::size_t>(1, split_arguments(value.substr(1, value.size() - 2)).size());
      }
    } else if (arg.starts_with("align:")) {
      std::string_view value = util::trim(arg.substr(6));
      if (value.starts_with("(")) value = value.substr(1, value.size() - 2);
      for (const std::string_view a : split_arguments(value)) {
        table.alignments.push_back(a == "left" ? ir::Alignment::Left : a == "right" ? ir::Alignment::Right
                                   : a == "center"                                  ? ir::Alignment::Center
                                                                                    : ir::Alignment::Default);
      }
    } else if (arg.starts_with("table.header(")) {
      for (const std::string_view h : split_arguments(arg.substr(13, arg.size() - 14))) header.push_back(h);
    } else if (arg.starts_with("[")) {
      cells.push_back(arg);
    }
  }
  const auto cell_content = [&](std::string_view c) {
    if (c.starts_with("[") && c.ends_with("]")) c = c.substr(1, c.size() - 2);
    return reader.inlines(c);
  };
  if (!header.empty()) {
    ir::TableRow row;
    row.header = true;
    for (const std::string_view h : header) row.cells.push_back(ir::TableCell{cell_content(h), 1, 1});
    table.rows.push_back(std::move(row));
  }
  for (std::size_t i = 0; i < cells.size(); i += columns) {
    ir::TableRow row;
    for (std::size_t c = i; c < std::min(cells.size(), i + columns); ++c) row.cells.push_back(ir::TableCell{cell_content(cells[c]), 1, 1});
    table.rows.push_back(std::move(row));
  }
  if (!table.rows.empty()) out.push_back(std::move(table));
}

std::size_t typst_block(std::span<const std::string_view> lines, std::size_t at, ir::Blocks& out, wiki::Reader& reader) {
  const std::string_view line = util::trim(lines[at]);
  ir::Document& document = *reader.context().document;
  if (line.starts_with("//")) return 1;
  if (line.starts_with("<") && line.ends_with(">") && line.find(' ') == std::string_view::npos) {
    // A label line after a heading.
    return 1;
  }
  if (line.starts_with("```")) {
    std::size_t ticks = 0;
    while (ticks < line.size() && line[ticks] == '`') ++ticks;
    const std::string fence(ticks, '`');
    std::vector<std::string_view> code;
    std::size_t i = at + 1;
    for (; i < lines.size() && util::trim(lines[i]) != fence; ++i) code.push_back(lines[i]);
    out.push_back(ir::CodeBlock{std::string(util::trim(line.substr(ticks))), join_lines(code)});
    return std::min(i, lines.size() - 1) - at + 1;
  }
  if (line.starts_with("$ ") && line.ends_with(" $") && line.size() > 4) {
    ir::CodeBlock math{{}, std::string(util::trim(line.substr(1, line.size() - 2)))};
    math.math = ir::MathMode::Display;
    out.push_back(std::move(math));
    return 1;
  }
  if (line.starts_with("#set ") || line.starts_with("#show ") || line.starts_with("#import ") || line.starts_with("#let ")) {
    if (line.starts_with("#set document(")) {
      const std::string args(line);
      if (const std::size_t at_title = args.find("title:"); at_title != std::string::npos) {
        document.metadata.title = string_literal(std::string_view(args).substr(at_title));
      }
    }
    // Skip multi-line arguments.
    std::string joined;
    std::size_t i = at;
    for (; i < lines.size(); ++i) {
      joined.append(lines[i]).push_back('\n');
      if (std::ranges::count(joined, '(') <= std::ranges::count(joined, ')')) break;
    }
    return std::min(i, lines.size() - 1) - at + 1;
  }
  if (line.starts_with("#line(") || line == "#divider()" || line.starts_with("#line(length")) {
    out.push_back(ir::HorizontalRule{});
    return 1;
  }
  if (line == "#pagebreak()") {
    out.push_back(ir::PageBreak{});
    return 1;
  }
  // Multi-line function calls: #quote, #figure, #table, #align, #block.
  if (line.starts_with("#")) {
    std::string joined;
    std::size_t i = at;
    for (; i < lines.size(); ++i) {
      joined.append(lines[i]).push_back('\n');
      const auto opens = std::ranges::count(joined, '(') + std::ranges::count(joined, '[');
      const auto closes = std::ranges::count(joined, ')') + std::ranges::count(joined, ']');
      if (opens <= closes) break;
      if (i - at > 10000) break;
    }
    const std::string_view text = util::trim(joined);
    std::size_t name_end = 1;
    while (name_end < text.size() && (util::is_ascii_alpha(text[name_end]) || text[name_end] == '.')) ++name_end;
    const std::string name(text.substr(1, name_end - 1));
    std::string arguments;
    std::size_t k = name_end;
    if (k < text.size() && text[k] == '(') {
      const std::size_t close = matching(text, k, '(', ')');
      if (close == std::string_view::npos) return 0;
      arguments = std::string(text.substr(k + 1, close - k - 1));
      k = close + 1;
    }
    std::string body;
    if (k < text.size() && text[k] == '[') {
      const std::size_t close = matching(text, k, '[', ']');
      if (close == std::string_view::npos) return 0;
      body = std::string(text.substr(k + 1, close - k - 1));
      k = close + 1;
    }
    if (k < text.size()) return 0;  // more text after the call: an inline construct
    const std::vector<std::string_view> body_lines = util::split_lines(body);
    if (name == "quote" || name == "block" || name == "align" || name == "pad" || name == "box") {
      ir::Blocks inner = reader.parse(body_lines, 1);
      if (name == "quote") out.push_back(ir::BlockQuote{std::move(inner)});
      else for (ir::Block& b : inner) out.push_back(std::move(b));
      return i - at + 1;
    }
    if (name == "table") {
      typst_table(arguments, out, reader);
      return i - at + 1;
    }
    if (name == "figure") {
      // figure(image("x", alt: "y"), caption: [...]) or figure(table(...))
      const std::vector<std::string_view> args = split_arguments(arguments);
      std::string caption;
      for (const std::string_view a : args) {
        if (a.starts_with("caption:")) {
          std::string_view c = util::trim(a.substr(8));
          if (c.starts_with("[") && c.ends_with("]")) c = c.substr(1, c.size() - 2);
          caption = collapse_whitespace(c);
        }
      }
      for (const std::string_view a : args) {
        if (a.starts_with("image(")) {
          std::string alt;
          if (const std::size_t at_alt = a.find("alt:"); at_alt != std::string_view::npos) alt = string_literal(a.substr(at_alt));
          out.push_back(ir::ImageBlock{wiki::image_from_url(string_literal(a)), alt, caption, {}});
          return i - at + 1;
        }
        std::string_view inner = a;
        if (inner.starts_with("align(")) {
          const std::size_t bracket = inner.find('[');
          if (bracket != std::string_view::npos) inner = inner.substr(bracket + 1, inner.size() - bracket - 2);
        }
        if (inner.starts_with("#table(") || inner.starts_with("table(")) {
          const std::size_t open = inner.find('(');
          const std::size_t close = matching(inner, open, '(', ')');
          if (close != std::string_view::npos) {
            typst_table(inner.substr(open + 1, close - open - 1), out, reader);
            if (!caption.empty() && !out.empty()) {
              if (auto* t = out.back().get_if<ir::TableBlock>()) t->caption = reader.inlines(caption);
            }
          }
          return i - at + 1;
        }
      }
      return i - at + 1;
    }
    return 0;
  }
  return 0;
}

ir::Document read_typst(std::string_view text) {
  wiki::ReadSyntax syntax;
  syntax.heading = typst_heading;
  syntax.list_item = typst_list;
  syntax.block = typst_block;
  syntax.inline_syntax = typst_inline;
  ir::Document document = wiki::read(text, syntax);
  // Labels on the line after a heading become its id.
  if (!document.footnotes.empty()) number_footnotes(document);
  return document;
}

// ---- writing ---------------------------------------------------------------------------------------

std::string typst_escape(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (std::string_view("\\*_`$#@<>[]~\"'=-+/").find(c) != std::string_view::npos) {
      if (c == '/' && !(i + 1 < text.size() && (text[i + 1] == '/' || text[i + 1] == '*')) && !(i > 0 && text[i - 1] == '/')) {
        out.push_back(c);
        continue;
      }
      if ((c == '-' || c == '=' || c == '+') && !(i == 0 || text[i - 1] == '\n' || (i + 1 < text.size() && text[i + 1] == c))) {
        out.push_back(c);
        continue;
      }
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

std::string typst_string(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (c == '\n') {
      out.append("\\n");
      continue;
    }
    out.push_back(c);
  }
  return out + "\"";
}

class TypstWriter {
 public:
  explicit TypstWriter(const ir::Document& document) : doc_(document) {
    m_.escape = typst_escape;
    m_.strong = {"#strong[", "]"};
    m_.emphasis = {"#emph[", "]"};
    m_.strikethrough = {"#strike[", "]"};
    m_.underline = {"#underline[", "]"};
    m_.superscript = {"#super[", "]"};
    m_.subscript = {"#sub[", "]"};
    m_.mark = {"#highlight[", "]"};
    m_.line_break = "\\\n";
    m_.soft_break = " ";
    m_.code = [](const ir::Code& code) -> std::string {
      if (code.math != ir::MathMode::None) return code.math == ir::MathMode::Display ? "$ " + code.text + " $" : "$" + code.text + "$";
      if (code.raw_format == "typst") return code.text;
      if (!code.raw_format.empty()) return {};
      std::string text = code.text;
      for (char& c : text) {
        if (c == '\n') c = ' ';
      }
      if (text.find('`') == std::string::npos && !text.empty()) return "`" + text + "`";
      return "#raw(" + typst_string(text) + ")";
    };
    m_.link = [](const ir::Link& link, const std::string& label) -> std::string {
      if (link.url.starts_with("#")) return label;
      return "#link(" + typst_string(link.url) + ")[" + label + "]";
    };
    m_.image = [](const ir::InlineImage& image) -> std::string {
      const std::string url = image_url(image.source, false);
      if (url.empty()) return typst_escape(image.alt);
      return "#box(image(" + typst_string(url) + (image.alt.empty() ? "" : ", alt: " + typst_string(image.alt)) + "))";
    };
    m_.note = [this](const ir::Link&, std::size_t index) -> std::string {
      if (index == 0 || index > doc_.footnotes.size()) return {};
      std::string body;
      for (const ir::Block& b : doc_.footnotes[index - 1].blocks) {
        if (const auto* p = b.get_if<ir::Paragraph>()) {
          if (!body.empty()) body.append(" ");
          body.append(render_inlines(p->content, m_, &doc_));
        }
      }
      return "#footnote[" + body + "]";
    };
  }

  std::string run() {
    std::string out;
    if (!doc_.metadata.title.empty()) out += "#set document(title: " + typst_string(doc_.metadata.title) + ")\n";
    if (!doc_.metadata.language.empty()) {
      std::string lang = doc_.metadata.language.substr(0, doc_.metadata.language.find('-'));
      std::erase_if(lang, [](char c) { return !util::is_ascii_alpha(c); });
      if (!lang.empty()) out += "#set text(lang: " + typst_string(util::to_lower(lang)) + ")\n";
    }
    if (!out.empty()) out += "\n";
    out += blocks(doc_.blocks, 0);
    while (out.ends_with('\n')) out.pop_back();
    return out + "\n";
  }

 private:
  std::string inlines(const ir::Inlines& content) { return render_inlines(content, m_, &doc_); }

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

  std::string block(const ir::Block& block, int depth) {
    if (depth > ir::max_document_depth * 2) return {};
    std::string out;
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Paragraph>) {
            out = inlines(node.content);
          } else if constexpr (std::same_as<T, ir::Heading>) {
            std::string text = inlines(node.content);
            for (char& c : text) {
              if (c == '\n') c = ' ';
            }
            out = std::string(static_cast<std::size_t>(std::clamp(node.level, 1, 6)), '=') + " " + std::string(util::trim(text));
            if (!block.attr.id.empty()) {
              std::string id = block.attr.id;
              std::erase_if(id, [](char c) { return !(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '-' || c == '_'); });
              if (!id.empty()) out += " <" + id + ">";
            }
          } else if constexpr (std::same_as<T, ir::List>) {
            out = list(node, depth);
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            if (node.math != ir::MathMode::None) {
              out = "$ " + node.code + " $";
            } else if (!node.raw_format.empty()) {
              if (node.raw_format == "typst") out = node.code;
            } else {
              std::size_t longest = 2;
              std::size_t run = 0;
              for (const char c : node.code) {
                run = c == '`' ? run + 1 : 0;
                longest = std::max(longest, run);
              }
              const std::string fence(longest + 1, '`');
              std::string lang = node.language;
              std::erase_if(lang, [](char c) { return !(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '+' || c == '-' || c == '_'); });
              out = fence + lang + "\n" + node.code + "\n" + fence;
            }
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            out = "#quote(block: true)[\n" + blocks(node.blocks, depth + 1) + "\n]";
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            out = table(node);
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            const std::string url = image_url(node.source, false);
            if (url.empty()) {
              out = typst_escape(node.caption.empty() ? node.alt : node.caption);
              return;
            }
            out = "#figure(image(" + typst_string(url) + (node.alt.empty() ? "" : ", alt: " + typst_string(node.alt)) + ")";
            if (!node.caption.empty()) out += ",\n  caption: [" + typst_escape(node.caption) + "]";
            out += ")";
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            out = "#line(length: 100%)";
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            out = "#pagebreak()";
          }
        },
        block.node);
    return out;
  }

  std::string list(const ir::List& list, int depth) {
    std::string out;
    for (std::size_t i = 0; i < list.items.size(); ++i) {
      const ir::Blocks& item = list.items[i];
      std::string line;
      if (list.definition) {
        std::string term;
        if (!item.empty()) {
          if (const auto* p = item.front().get_if<ir::Paragraph>()) term = inlines(p->content);
        }
        ir::Blocks rest(item.begin() + std::min<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(item.size())), item.end());
        std::string body = blocks(rest, depth + 1);
        line = "/ " + util::replace_all(term, ":", "\\:") + ": " + util::replace_all(body, "\n\n", "\n  ");
      } else {
        line = list.ordered ? "+ " : "- ";
        if (list.task(i) == ir::TaskState::Done) line += "\xE2\x98\x91 ";
        if (list.task(i) == ir::TaskState::Open) line += "\xE2\x98\x90 ";
        std::string body = blocks(item, depth + 1);
        bool first = true;
        for (const std::string_view l : util::split(body, '\n')) {
          if (!first) line.append("\n").append(l.empty() ? "" : "  ");
          line.append(l);
          first = false;
        }
      }
      if (!out.empty()) out.append("\n");
      out.append(line);
    }
    if (list.ordered && list.start != 1) out = "#set enum(start: " + std::to_string(list.start) + ")\n" + out;
    return out;
  }

  std::string table(const ir::TableBlock& table) {
    const TableGrid grid = table_grid(table);
    if (grid.columns == 0) return {};
    std::string out = "#table(\n  columns: " + std::to_string(grid.columns) + ",\n";
    if (!table.alignments.empty()) {
      out += "  align: (";
      for (std::size_t c = 0; c < grid.columns; ++c) {
        const ir::Alignment a = table.column_alignment(c);
        out += a == ir::Alignment::Right ? "right" : a == ir::Alignment::Center ? "center" : "left";
        out += ",";
      }
      out += "),\n";
    }
    const std::size_t header = header_row_count(table);
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
      std::string row;
      for (const ir::TableCell& cell : table.rows[r].cells) {
        std::string content = "[" + inlines(cell.content) + "]";
        if (cell.colspan > 1 || cell.rowspan > 1) {
          content = "table.cell(" + std::string(cell.colspan > 1 ? "colspan: " + std::to_string(cell.colspan) + ", " : "") +
                    (cell.rowspan > 1 ? "rowspan: " + std::to_string(cell.rowspan) + ", " : "") + ")" + content;
        }
        row += content + ", ";
      }
      if (r < header) out += "  table.header(" + row + "),\n";
      else out += "  " + row + "\n";
    }
    out += ")";
    if (!table.caption.empty()) out = "#figure(\n" + out + ",\n  caption: [" + inlines(table.caption) + "])";
    return out;
  }

  const ir::Document& doc_;
  InlineMarkup m_;
};

int sniff_typst(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"typ"})) return 80;
  const std::string_view head = sniff_head(data);
  return (head.starts_with("#set ") || head.starts_with("#import ")) && head.find("\n= ") != std::string_view::npos ? 66 : 0;
}

}  // namespace

void register_typst(Registry& registry) {
  MarkupFormat typst;
  typst.id = "typst";
  typst.name = "Typst";
  typst.description = "Typst typesetting markup (written in full; read for common markup).";
  typst.extensions = {"typ"};
  typst.mime = "text/x-typst";
  typst.popularity = 20;
  typst.sniff = sniff_typst;
  typst.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_typst(text); };
  typst.read_lossy = true;  // scripting and styling are not evaluated
  typst.read_cost = 20;
  typst.write = [](const ir::Document& document, WriteContext&) -> Result<std::string> {
    TypstWriter writer(document);
    return writer.run();
  };
  register_markup(registry, std::move(typst));
}

}  // namespace cvt::document
