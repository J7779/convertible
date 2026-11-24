// Convertible core - document family: ir::Document -> Markdown.
//
// The output re-parses (with parse_markdown and the same flavour) to the same
// document: text is escaped where it would otherwise become markup, lines
// are never broken inside code spans, math or raw HTML, line starts that
// would open a block are escaped after wrapping, emphasis that cannot be
// expressed with delimiters (punctuation next to letters) falls back to HTML.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>

#include "families/document/doc_common.hpp"
#include "families/document/document_api.hpp"
#include "families/document/markdown_internal.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

// Spaces that must not become line breaks while wrapping.
constexpr char hard_space = '\x01';

bool is_alnum_byte(char c) {
  return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || static_cast<unsigned char>(c) >= 0x80;
}

std::string protect_spaces(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c == ' ') c = hard_space;
  }
  return out;
}

std::size_t longest_run(std::string_view text, char c) {
  std::size_t best = 0;
  std::size_t run = 0;
  for (const char x : text) {
    run = x == c ? run + 1 : 0;
    best = std::max(best, run);
  }
  return best;
}

class MdWriter {
 public:
  MdWriter(const ir::Document& document, const MarkdownWriteOptions& options) : doc_(document), options_(options) {
    nl_ = "\n";
    emph_ = options.emphasis == '_' ? '_' : '*';
    bullet_ = (options.bullet == '*' || options.bullet == '+') ? options.bullet : '-';
  }

  std::string run() {
    std::string out;
    out.append(front_matter());
    out.append(blocks(doc_.blocks, 0));
    // Footnotes.
    std::string notes;
    for (std::size_t i = 0; i < doc_.footnotes.size(); ++i) {
      const std::string label = "[^" + std::to_string(i + 1) + "]: ";
      std::string body = blocks(doc_.footnotes[i].blocks, 1);
      notes.append(label).append(indent_lines(body, "    ", false)).append("\n\n");
    }
    if (!notes.empty()) {
      if (!out.empty()) out.append("\n\n");
      out.append(notes);
    }
    // Reference link definitions.
    if (!references_.empty()) {
      if (!out.empty() && !out.ends_with("\n\n")) out.append(out.ends_with("\n") ? "\n" : "\n\n");
      for (const auto& [label, target] : reference_order_) {
        out.append("[").append(label).append("]: ").append(target).append("\n");
      }
    }
    while (out.ends_with("\n")) out.pop_back();
    if (!out.empty()) out.append("\n");
    return out;
  }

 private:
  bool gfm() const { return options_.flavor != MarkdownFlavor::CommonMark; }
  bool extended() const { return options_.flavor == MarkdownFlavor::Extended; }

  // ---- metadata --------------------------------------------------------------------

  static std::string yaml_quote(std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
      if (c == '"' || c == '\\') out.push_back('\\');
      if (c == '\n') {
        out.append("\\n");
        continue;
      }
      out.push_back(c);
    }
    out.push_back('"');
    return out;
  }

  std::string front_matter() const {
    if (!options_.front_matter) return {};
    const ir::DocumentMetadata& m = doc_.metadata;
    const auto raw = m.extra.find("front_matter");
    const auto raw_format = m.extra.find("front_matter_format");
    if (raw != m.extra.end() && gfm()) {
      const bool toml = raw_format != m.extra.end() && raw_format->second == "toml";
      std::string body = raw->second;
      if (!body.empty() && !body.ends_with("\n")) body.push_back('\n');
      return std::string(toml ? "+++" : "---") + "\n" + body + (toml ? "+++" : "---") + "\n\n";
    }
    bool other = !m.author.empty() || !m.date.empty();
    for (const auto& [key, value] : m.extra) {
      if (key != "front_matter_format") other = true;
    }
    std::string first_heading;
    for (const ir::Block& b : doc_.blocks) {
      if (const auto* h = b.get_if<ir::Heading>()) {
        first_heading = inline_line(h->content);
        break;
      }
    }
    const bool title_needed = !m.title.empty() && m.title != first_heading;
    if (!extended() && !other && !title_needed) return {};
    if (m.title.empty() && m.language.empty() && !other) return {};
    if (!gfm()) return {};
    std::string out = "---\n";
    if (!m.title.empty()) out.append("title: ").append(yaml_quote(m.title)).append("\n");
    if (!m.author.empty()) out.append("author: ").append(yaml_quote(m.author)).append("\n");
    if (!m.date.empty()) out.append("date: ").append(yaml_quote(m.date)).append("\n");
    if (!m.language.empty()) out.append("lang: ").append(yaml_quote(m.language)).append("\n");
    for (const auto& [key, value] : m.extra) {
      if (key == "front_matter_format" || key.empty()) continue;
      bool simple_key = std::ranges::all_of(key, [](char c) {
        return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '-';
      });
      if (!simple_key) continue;
      out.append(key).append(": ").append(yaml_quote(value)).append("\n");
    }
    out.append("---\n\n");
    return out;
  }

  // ---- blocks -------------------------------------------------------------------------

  std::string blocks(const ir::Blocks& list, int depth) {
    std::string out;
    const ir::Block* previous = nullptr;
    for (const ir::Block& block : list) {
      std::string text = this->block(block, depth);
      if (text.empty()) continue;
      if (!out.empty()) {
        out.append("\n\n");
        // Two adjacent lists (or a list then indented content) would merge.
        if (previous != nullptr && previous->is<ir::List>() && block.is<ir::List>() &&
            previous->get_if<ir::List>()->ordered == block.get_if<ir::List>()->ordered &&
            !previous->get_if<ir::List>()->definition && !block.get_if<ir::List>()->definition) {
          out.append("<!-- -->\n\n");
        }
      }
      out.append(text);
      previous = &block;
    }
    return out;
  }

  static std::string indent_lines(std::string_view text, std::string_view prefix, bool first_too,
                                  std::string_view blank_prefix = {}) {
    std::string out;
    bool first = true;
    for (const std::string_view line : util::split(text, '\n')) {
      if (!first) out.push_back('\n');
      if (line.empty()) {
        out.append(blank_prefix);
      } else if (!first || first_too) {
        out.append(prefix);
        out.append(line);
        first = false;
        continue;
      } else {
        out.append(line);
      }
      first = false;
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
            out = paragraph(node.content);
          } else if constexpr (std::same_as<T, ir::Heading>) {
            out = heading(node, block.attr);
          } else if constexpr (std::same_as<T, ir::List>) {
            out = list(node, depth);
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            out = code_block(node);
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            std::string inner = blocks(node.blocks, depth + 1);
            if (block.attr.has_class(ir::conventions::admonition) && gfm()) {
              std::string kind = "NOTE";
              for (const std::string& c : block.attr.classes) {
                if (c != ir::conventions::admonition) kind = util::to_upper(c);
              }
              inner = "[!" + kind + "]" + (inner.empty() ? "" : "\n" + inner);
            }
            if (inner.empty()) inner = " ";
            out = indent_lines(inner, "> ", true, ">");
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            out = table(node);
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            const std::string url = image_url(node.source, options_.images == ImageHandling::Embed);
            const std::string& alt = node.caption.empty() ? node.alt : node.caption;
            if (url.empty()) {
              out = alt.empty() ? std::string() : finish_paragraph(escape_text(alt, false));
            } else {
              out = "![" + escape_text(alt, false) + "](" + destination(url) + title_part(node.title) + ")";
            }
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            out = "* * *";
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            out = "<div class=\"page-break\" style=\"break-after:page\"></div>";
          }
        },
        block.node);
    return out;
  }

  std::string paragraph(const ir::Inlines& content) {
    std::string text = inlines(content, false);
    return finish_paragraph(text);
  }

  // Wrap, trim, escape dangerous line starts, restore protected spaces.
  std::string finish_paragraph(const std::string& text) {
    std::string wrapped;
    if (options_.wrap_width > 0) {
      // Keep forced breaks; wrap each line.
      wrapped = wrap_text(text, static_cast<std::size_t>(options_.wrap_width));
    } else {
      wrapped = text;
    }
    std::string out;
    bool first = true;
    for (std::string_view line : util::split(wrapped, '\n')) {
      // Leading blanks would change meaning (indented code); trailing ones make breaks.
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
      std::string fixed(line);
      while (!fixed.empty() && fixed.back() == ' ' && !fixed.ends_with("\\")) fixed.pop_back();
      fixed = escape_line_start(fixed);
      if (!first) out.push_back('\n');
      out.append(fixed);
      first = false;
    }
    for (char& c : out) {
      if (c == hard_space) c = ' ';
    }
    return out;
  }

  std::string escape_line_start(const std::string& line) const {
    if (line.empty()) return line;
    const char c = line.front();
    const auto all_of = [&](char x) {
      return std::ranges::all_of(line, [x](char y) { return y == x || y == ' ' || y == '\t'; });
    };
    if (c == '#' || c == '>' || (c == ':' && extended())) return "\\" + line;
    if ((c == '-' || c == '+' || c == '*') && (line.size() == 1 || line[1] == ' ' || line[1] == '\t')) return "\\" + line;
    if (c == '=' && all_of('=')) return "\\" + line;
    if (c == '-' && all_of('-')) return "\\" + line;
    if (c == '+' && all_of('+')) return "\\" + line;
    if (c == '|' || (gfm() && line.find('|') != std::string::npos && line.find_first_not_of("|-: \t") == std::string::npos)) {
      return "\\" + line;
    }
    if (line.starts_with("```") || line.starts_with("~~~")) return "\\" + line;
    if (util::is_ascii_digit(c)) {
      std::size_t digits = 0;
      while (digits < line.size() && util::is_ascii_digit(line[digits])) ++digits;
      if (digits <= 9 && digits < line.size() && (line[digits] == '.' || line[digits] == ')') &&
          (digits + 1 == line.size() || line[digits + 1] == ' ' || line[digits + 1] == '\t')) {
        return line.substr(0, digits) + "\\" + line.substr(digits);
      }
    }
    return line;
  }

  std::string heading(const ir::Heading& heading, const ir::Attributes& attr) {
    std::string text = inlines(heading.content, false, true);
    for (char& c : text) {
      if (c == '\n' || c == hard_space) c = ' ';
    }
    text = std::string(util::trim(text));
    if (text.ends_with("#")) text.insert(text.size() - 1, "\\");
    if (!text.empty() && (text.front() == '#')) text = "\\" + text;
    const int level = std::clamp(heading.level, 1, 6);
    if (extended() && !attr.id.empty() && attr.id != slugify(inline_line(heading.content))) {
      text.append(" {#").append(attr.id).append("}");
    }
    if (options_.setext_headings && level <= 2 && !text.empty()) {
      return escape_line_start(text) + "\n" + std::string(std::max<std::size_t>(display_width(text), 3), level == 1 ? '=' : '-');
    }
    return std::string(static_cast<std::size_t>(level), '#') + (text.empty() ? "" : " " + text);
  }

  std::string code_block(const ir::CodeBlock& code) {
    if (code.math != ir::MathMode::None) {
      if (gfm()) return "$$\n" + code.code + "\n$$";
      return fenced(code.code, "math");
    }
    if (!code.raw_format.empty()) {
      if (code.raw_format == "html") return code.code;
      return fenced(code.code, "{=" + code.raw_format + "}");
    }
    return fenced(code.code, code.language);
  }

  static std::string fenced(const std::string& code, const std::string& info) {
    const bool backticks_in_info = info.find('`') != std::string::npos;
    const char fence_char = backticks_in_info ? '~' : '`';
    const std::size_t length = std::max<std::size_t>(3, longest_run(code, fence_char) + 1);
    const std::string fence(length, fence_char);
    std::string out = fence + info + "\n" + code;
    if (!code.empty() && !code.ends_with("\n")) out.push_back('\n');
    if (code.empty()) out.push_back('\n');
    out.append(fence);
    return out;
  }

  bool list_needs_loose(const ir::List& list) const {
    if (list.loose) return true;
    for (const ir::Blocks& item : list.items) {
      for (std::size_t i = 1; i < item.size(); ++i) {
        const bool paragraph_then_list = item[i - 1].is<ir::Paragraph>() && item[i].is<ir::List>();
        if (!paragraph_then_list) return true;
      }
    }
    return false;
  }

  std::string list(const ir::List& list, int depth) {
    if (list.definition) return definition_list(list, depth);
    const bool loose = list_needs_loose(list);
    std::string out;
    int number = list.start;
    for (std::size_t i = 0; i < list.items.size(); ++i) {
      std::string marker;
      if (list.ordered) {
        marker = std::to_string(number++) + ".";
      } else {
        marker = std::string(1, bullet_);
      }
      marker.push_back(' ');
      const std::string indent(marker.size(), ' ');
      std::string body;
      const ir::Blocks& item = list.items[i];
      std::string task;
      if (gfm() && list.task(i) != ir::TaskState::None) task = list.task(i) == ir::TaskState::Done ? "[x] " : "[ ] ";
      for (std::size_t b = 0; b < item.size(); ++b) {
        std::string text = block(item[b], depth + 1);
        if (text.empty()) continue;
        if (!body.empty()) body.append(loose ? "\n\n" : "\n");
        body.append(text);
      }
      if (!task.empty()) {
        if (!item.empty() && item.front().is<ir::Paragraph>()) {
          body = task + body;
        } else {
          body = task + "\n" + body;
        }
      }
      // A first line starting with a code fence or another marker is fine
      // after the list marker; an empty item is just the marker.
      if (!out.empty()) out.append(loose ? "\n\n" : "\n");
      if (body.empty()) {
        out.append(util::trim_right(marker));
      } else {
        out.append(marker).append(indent_lines(body, indent, false));
      }
    }
    return out;
  }

  std::string definition_list(const ir::List& list, int depth) {
    std::string out;
    for (const ir::Blocks& item : list.items) {
      if (item.empty()) continue;
      std::string term;
      if (const auto* p = item.front().get_if<ir::Paragraph>()) {
        term = inlines(p->content, false, true);
        for (char& c : term) {
          if (c == '\n' || c == hard_space) c = ' ';
        }
      }
      ir::Blocks definition(item.begin() + 1, item.end());
      std::string body = blocks(definition, depth + 1);
      if (!out.empty()) out.append("\n\n");
      if (extended()) {
        out.append(escape_line_start(std::string(util::trim(term))));
        if (!body.empty()) out.append("\n:   ").append(indent_lines(body, "    ", false));
      } else {
        out.append("**").append(util::trim(term)).append("**");
        if (!body.empty()) out.append("\n\n").append(body);
      }
    }
    return out;
  }

  // ---- tables ----------------------------------------------------------------------------

  bool pipe_table_possible(const ir::TableBlock& table) const {
    if (!gfm()) return false;
    for (const ir::TableRow& row : table.rows) {
      for (const ir::TableCell& cell : row.cells) {
        if (cell.colspan > 1 || cell.rowspan > 1) return false;
      }
    }
    return header_row_count(table) <= 1;
  }

  std::string table(const ir::TableBlock& table) {
    if (!pipe_table_possible(table)) {
      HtmlWriteOptions html;
      html.standalone = false;
      html.images = options_.images;
      ir::Document fragment;
      fragment.blocks.push_back(table);
      std::string out = write_html(fragment, html);
      while (out.ends_with("\n")) out.pop_back();
      return out;
    }
    const std::vector<std::vector<std::string>> cells = table_strings(table, [this](const ir::Inlines& content) {
      std::string text = inlines(content, true);
      for (char& c : text) {
        if (c == hard_space) c = ' ';
        if (c == '\n') c = ' ';
      }
      return std::string(util::trim(text));
    });
    std::size_t columns = 0;
    for (const auto& row : cells) columns = std::max(columns, row.size());
    if (columns == 0) return {};
    const std::size_t header_rows = header_row_count(table);
    std::vector<std::vector<std::string>> rows;
    if (header_rows == 0) rows.emplace_back(columns);  // empty header = no header
    for (const auto& row : cells) rows.push_back(row);
    std::vector<std::size_t> widths(columns, 3);
    for (const auto& row : rows) {
      for (std::size_t c = 0; c < row.size(); ++c) widths[c] = std::max(widths[c], display_width(row[c]));
    }
    std::string out;
    const auto write_row = [&](const std::vector<std::string>& row) {
      out.append("|");
      for (std::size_t c = 0; c < columns; ++c) {
        const std::string& text = c < row.size() ? row[c] : std::string();
        out.append(" ").append(pad_to(text, widths[c])).append(" |");
      }
      out.append("\n");
    };
    write_row(rows.front());
    out.append("|");
    for (std::size_t c = 0; c < columns; ++c) {
      const ir::Alignment align = table.column_alignment(c);
      std::string rule(widths[c], '-');
      if (align == ir::Alignment::Left || align == ir::Alignment::Center) rule.front() = ':';
      if (align == ir::Alignment::Right || align == ir::Alignment::Center) rule.back() = ':';
      out.append(" ").append(rule).append(" |");
    }
    out.append("\n");
    for (std::size_t r = 1; r < rows.size(); ++r) write_row(rows[r]);
    while (out.ends_with("\n")) out.pop_back();
    if (!table.caption.empty()) {
      const std::string caption = finish_paragraph(inlines(table.caption, false));
      out.append("\n\n").append(extended() ? "Table: " + caption : caption);
    }
    return out;
  }

  // ---- inlines ----------------------------------------------------------------------------

  std::string escape_text(std::string_view text, bool in_table) const {
    std::string out;
    out.reserve(text.size() + 8);
    for (std::size_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      switch (c) {
        case '\\':
        case '`':
        case '*':
        case '[':
        case ']':
        case '<':
          out.push_back('\\');
          out.push_back(c);
          break;
        case '_': {
          const bool intraword = i > 0 && i + 1 < text.size() && is_alnum_byte(text[i - 1]) && is_alnum_byte(text[i + 1]);
          if (!intraword) out.push_back('\\');
          out.push_back(c);
          break;
        }
        case '~':
        case '$':
          if (gfm()) out.push_back('\\');
          out.push_back(c);
          break;
        case '|':
          if (in_table || gfm()) out.push_back('\\');
          out.push_back(c);
          break;
        case '&':
          if (i + 1 < text.size() && (util::is_ascii_alpha(text[i + 1]) || text[i + 1] == '#')) out.push_back('\\');
          out.push_back(c);
          break;
        case '!':
          out.push_back(c);
          break;
        case '\r':
          break;
        default: out.push_back(c);
      }
    }
    return out;
  }

  static std::string destination(const std::string& url) {
    int depth = 0;
    bool balanced = true;
    bool needs_pointy = url.empty();
    for (const char c : url) {
      if (c == '(') ++depth;
      if (c == ')' && --depth < 0) balanced = false;
      if (c == ' ' || c == '<' || c == '>' || static_cast<unsigned char>(c) < 0x20) needs_pointy = true;
    }
    if (depth != 0) balanced = false;
    if (needs_pointy || !balanced) {
      std::string out = "<";
      for (const char c : url) {
        if (c == '<' || c == '>' || c == '\\') out.push_back('\\');
        if (c == '\n' || c == '\r') continue;
        out.push_back(c);
      }
      out.push_back('>');
      return protect_spaces(out);
    }
    std::string out;
    for (const char c : url) {
      if (c == '\\') out.push_back('\\');
      out.push_back(c);
    }
    return out;
  }

  static std::string title_part(const std::string& title) {
    if (title.empty()) return {};
    std::string out = " \"";
    for (const char c : title) {
      if (c == '"' || c == '\\') out.push_back('\\');
      out.push_back(c == '\n' ? ' ' : c);
    }
    out.push_back('"');
    return protect_spaces(out);
  }

  static std::string code_span(const std::string& text) {
    std::string content = text;
    for (char& c : content) {
      if (c == '\n') c = ' ';
    }
    const std::string ticks(longest_run(content, '`') + 1, '`');
    const bool pad = content.starts_with("`") || content.ends_with("`") ||
                     (content.starts_with(" ") && content.ends_with(" ") && content.find_first_not_of(' ') != std::string::npos);
    return protect_spaces(ticks + (pad ? " " : "") + content + (pad ? " " : "") + ticks);
  }

  static char first_char(const ir::Inline& item) {
    if (const auto* t = item.get_if<ir::Text>()) return t->text.empty() ? '\0' : t->text.front();
    if (item.is<ir::Code>()) return '`';
    return 'a';
  }

  // Emphasis delimiters fail next to letters when the content starts/ends
  // with punctuation; such spans are written as HTML instead.
  static bool delimiters_safe(const std::string& out, const std::string& content, char following) {
    if (content.empty()) return false;
    const char before = out.empty() ? ' ' : out.back();
    const char first = content.front();
    const char last = content.back();
    const auto punct = [](char c) { return markdown::is_ascii_punctuation(c) && c != hard_space; };
    if (is_alnum_byte(before) && punct(first)) return false;
    if (is_alnum_byte(following) && punct(last)) return false;
    if (first == ' ' || last == ' ' || first == hard_space || last == hard_space) return false;
    return true;
  }

  std::string inlines(const ir::Inlines& list, bool in_table, bool single_line = false, int depth = 0) {
    std::string out;
    if (depth > ir::max_document_depth * 2) return out;
    for (std::size_t index = 0; index < list.size(); ++index) {
      const ir::Inline& item = list[index];
      const char following = index + 1 < list.size() ? first_char(list[index + 1]) : ' ';
      std::visit(
          [&](const auto& node) {
            using T = std::remove_cvref_t<decltype(node)>;
            const auto delimited = [&](std::string_view delimiter, std::string_view html_tag, const ir::Inlines& children) {
              // Leading/trailing blanks go outside the delimiters.
              std::string content = inlines(children, in_table, single_line, depth + 1);
              std::size_t lead = 0;
              while (lead < content.size() && (content[lead] == ' ' || content[lead] == '\n')) ++lead;
              std::size_t trail = 0;
              while (trail < content.size() - lead &&
                     (content[content.size() - 1 - trail] == ' ' || content[content.size() - 1 - trail] == '\n')) {
                ++trail;
              }
              const std::string core = content.substr(lead, content.size() - lead - trail);
              out.append(content.substr(0, lead));
              if (core.empty()) {
                // nothing to emphasise
              } else if (!delimiter.empty() && delimiters_safe(out, core, trail > 0 ? ' ' : following) &&
                         (out.empty() || out.back() != delimiter.front()) && core.front() != delimiter.front() &&
                         core.back() != delimiter.back()) {
                out.append(delimiter).append(core).append(delimiter);
              } else {
                out.append("<").append(html_tag).append(">").append(core).append("</").append(html_tag).append(">");
              }
              out.append(content.substr(content.size() - trail));
            };
            if constexpr (std::same_as<T, ir::Text>) {
              std::string text = escape_text(node.text, in_table);
              if (single_line || in_table) {
                for (char& c : text) {
                  if (c == '\n') c = ' ';
                }
              }
              out.append(text);
            } else if constexpr (std::same_as<T, ir::Emphasis>) {
              delimited(std::string(1, emph_), "em", node.children);
            } else if constexpr (std::same_as<T, ir::Strong>) {
              if (item.attr.has_class("mark")) {
                delimited({}, "mark", node.children);
              } else {
                delimited(std::string(2, emph_), "strong", node.children);
              }
            } else if constexpr (std::same_as<T, ir::Strikethrough>) {
              delimited(gfm() ? "~~" : "", "del", node.children);
            } else if constexpr (std::same_as<T, ir::Underline>) {
              delimited({}, "u", node.children);
            } else if constexpr (std::same_as<T, ir::Superscript>) {
              delimited({}, "sup", node.children);
            } else if constexpr (std::same_as<T, ir::Subscript>) {
              delimited({}, "sub", node.children);
            } else if constexpr (std::same_as<T, ir::Code>) {
              if (node.math != ir::MathMode::None && gfm()) {
                const std::string_view d = node.math == ir::MathMode::Display ? "$$" : "$";
                std::string tex = node.text;
                for (char& c : tex) {
                  if (c == '\n') c = ' ';
                }
                // Inline math must not start/end with a space.
                tex = std::string(util::trim(tex));
                out.append(protect_spaces(std::string(d) + tex + std::string(d)));
              } else if (node.raw_format == "html") {
                out.append(protect_spaces(node.text));
              } else {
                out.append(code_span(node.text));
              }
            } else if constexpr (std::same_as<T, ir::Link>) {
              if (!node.note.empty()) {
                const std::size_t index = footnote_index(doc_, node.note);
                if (index > 0) {
                  out.append("[^").append(std::to_string(index)).append("]");
                  return;
                }
              }
              std::string label = inlines(node.children, in_table, single_line, depth + 1);
              const std::string plain = ir::plain_text(node.children);
              const bool absolute = node.url.find(':') != std::string::npos && node.url.find(' ') == std::string::npos &&
                                    node.url.find('<') == std::string::npos && node.url.find('>') == std::string::npos;
              if (node.title.empty() && absolute && (plain == node.url || "mailto:" + plain == node.url) &&
                  node.url.find('\n') == std::string::npos) {
                out.append("<").append(plain == node.url ? node.url : plain).append(">");
                return;
              }
              if (label.empty()) label = escape_text(node.url, in_table);
              if (options_.reference_links) {
                std::string target = destination(node.url) + title_part(node.title);
                for (char& c : target) {
                  if (c == hard_space) c = ' ';
                }
                const auto it = references_.find(target);
                std::string ref;
                if (it == references_.end()) {
                  ref = std::to_string(references_.size() + 1);
                  references_[target] = ref;
                  reference_order_.emplace_back(ref, target);
                } else {
                  ref = it->second;
                }
                out.append("[").append(label).append("][").append(ref).append("]");
              } else {
                out.append("[").append(label).append("](").append(destination(node.url)).append(title_part(node.title)).append(")");
              }
            } else if constexpr (std::same_as<T, ir::LineBreak>) {
              if (in_table || single_line) {
                out.append(gfm() || in_table ? "<br>" : " ");
              } else {
                out.append("\\\n");
              }
            } else if constexpr (std::same_as<T, ir::InlineImage>) {
              const std::string url = image_url(node.source, options_.images == ImageHandling::Embed);
              if (url.empty()) {
                out.append(escape_text(node.alt, in_table));
              } else {
                out.append("![").append(escape_text(node.alt, in_table)).append("](").append(destination(url));
                out.append(title_part(node.title)).append(")");
              }
            }
          },
          item.node);
    }
    return out;
  }

  const ir::Document& doc_;
  const MarkdownWriteOptions& options_;
  std::string nl_;
  char emph_ = '*';
  char bullet_ = '-';
  std::map<std::string, std::string> references_;
  std::vector<std::pair<std::string, std::string>> reference_order_;
};

}  // namespace

std::string write_markdown(const ir::Document& document, const MarkdownWriteOptions& options) {
  MdWriter writer(document, options);
  std::string out = writer.run();
  if (options.newline != "\n") {
    out = util::convert_line_endings(out, options.newline == "\r\n" ? util::LineEnding::CrLf : util::LineEnding::Cr);
  }
  return out;
}

}  // namespace cvt::document
