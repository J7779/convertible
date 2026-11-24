// Convertible core - document family: the line-oriented wiki markup engine.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/document/wiki_engine.hpp"

#include <algorithm>

#include "families/document/html_internal.hpp"
#include "util/text.hpp"

namespace cvt::document::wiki {

namespace {
constexpr int max_list_depth = 16;
constexpr int max_quote_depth = 16;
}  // namespace

ir::Inline Context::add_note(ir::Blocks body) {
  ++note_counter;
  const std::string label = std::to_string(note_counter);
  if (document != nullptr) document->footnotes.push_back(ir::Footnote{label, std::move(body)});
  ir::Link link;
  link.note = label;
  link.url = "#fn-" + label;
  link.children = {ir::Text{label}};
  return link;
}

std::string repeat(std::string_view text, int count) {
  std::string out;
  for (int i = 0; i < count; ++i) out.append(text);
  return out;
}

std::string escape_chars(std::string_view text, std::string_view specials, std::string_view escape) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (specials.find(c) != std::string_view::npos) out.append(escape);
    out.push_back(c);
  }
  return out;
}

std::string entity_escape(std::string_view text, std::string_view specials) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (specials.find(c) != std::string_view::npos) {
      out.append("&#").append(std::to_string(static_cast<int>(static_cast<unsigned char>(c)))).append(";");
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::size_t bare_url_length(std::string_view text, std::size_t pos) {
  if (pos > 0) {
    const char before = text[pos - 1];
    if (util::is_ascii_alpha(before) || util::is_ascii_digit(before) || before == '/' || before == ':') return 0;
  }
  const std::string_view rest = text.substr(pos);
  std::size_t scheme = 0;
  for (const std::string_view prefix : {"https://", "http://", "ftp://", "mailto:"}) {
    if (rest.starts_with(prefix)) {
      scheme = prefix.size();
      break;
    }
  }
  if (scheme == 0) return 0;
  std::size_t i = scheme;
  while (i < rest.size() && !util::is_ascii_space(rest[i]) && rest[i] != '<' && rest[i] != '>' && rest[i] != '"' &&
         rest[i] != '[' && rest[i] != ']' && rest[i] != '|' && rest[i] != '{' && rest[i] != '}') {
    ++i;
  }
  while (i > scheme && std::string_view(".,;:!?)'*_").find(rest[i - 1]) != std::string_view::npos) {
    if (rest[i - 1] == ')' && std::ranges::count(rest.substr(0, i), '(') >= std::ranges::count(rest.substr(0, i), ')')) break;
    --i;
  }
  return i > scheme ? i : 0;
}

std::size_t decode_entity(std::string_view text, std::size_t pos, std::string& out) {
  if (pos >= text.size() || text[pos] != '&') return 0;
  const std::size_t semicolon = text.find(';', pos);
  if (semicolon == std::string_view::npos || semicolon - pos > 33 || semicolon == pos + 1) return 0;
  const std::string_view candidate = text.substr(pos, semicolon - pos + 1);
  for (std::size_t k = 1; k + 1 < candidate.size(); ++k) {
    const char c = candidate[k];
    if (!util::is_ascii_alpha(c) && !util::is_ascii_digit(c) && c != '#') return 0;
  }
  const std::string decoded = html::decode_references(candidate, false);
  if (decoded == candidate) return 0;
  out.append(decoded);
  return candidate.size();
}

std::size_t common_inline(std::string_view text, std::size_t pos, ir::Inlines& out) {
  if (text[pos] == '&') {
    std::string decoded;
    const std::size_t used = decode_entity(text, pos, decoded);
    if (used > 0) append_text(out, decoded);
    return used;
  }
  if (const std::size_t length = bare_url_length(text, pos); length > 0) {
    const std::string url(text.substr(pos, length));
    const std::string label = url.starts_with("mailto:") ? url.substr(7) : url;
    out.push_back(ir::Link{url, {}, {ir::Text{label}}});
    return length;
  }
  return 0;
}

ir::ImageSource image_from_url(std::string_view url) {
  if (auto embedded = parse_data_uri_image(url)) return std::move(*embedded);
  ir::EncodedImage reference;
  reference.url = std::string(util::trim(url));
  return reference;
}

void infer_column_alignment(ir::TableBlock& table) {
  if (table.rows.empty() || !table.alignments.empty()) return;
  const TableGrid grid = table_grid(table);
  std::vector<ir::Alignment> aligns(grid.columns, ir::Alignment::Default);
  bool any = false;
  for (std::size_t c = 0; c < grid.columns; ++c) {
    std::optional<ir::Alignment> common;
    bool conflict = false;
    for (const auto& row : grid.cells) {
      if (row[c] == nullptr || row[c]->align == ir::Alignment::Default) continue;
      if (!common) common = row[c]->align;
      if (*common != row[c]->align) conflict = true;
    }
    if (conflict) common.reset();
    if (common && *common != ir::Alignment::Default) {
      aligns[c] = *common;
      any = true;
    }
  }
  if (!any) return;
  table.alignments = std::move(aligns);
  for (ir::TableRow& row : table.rows) {
    for (ir::TableCell& cell : row.cells) cell.align = ir::Alignment::Default;
  }
}

ir::Blocks paragraph_blocks(ir::Inlines content) {
  ir::Blocks out;
  if (!content.empty()) out.push_back(ir::Paragraph{std::move(content)});
  return out;
}

// ================================================================================ reading

Reader::Reader(const ReadSyntax& syntax, ir::Document& document) : syntax_(syntax) {
  context_.document = &document;
  if (syntax_.inline_syntax) inline_syntax_ = syntax_.inline_syntax(context_);
}

ir::Inlines Reader::inlines(std::string_view text) {
  InlineParser parser(inline_syntax_);
  return parser.parse(text);
}

namespace {

struct ListFrame {
  ir::List list;
  int depth = 1;
  bool any_task = false;
};

}  // namespace

ir::Blocks Reader::parse(std::span<const std::string_view> lines, int depth) {
  ir::Blocks out;
  std::vector<std::string_view> paragraph;
  const auto flush = [&] {
    if (paragraph.empty()) return;
    std::string text;
    for (std::size_t k = 0; k < paragraph.size(); ++k) {
      if (k > 0) text.push_back('\n');
      text.append(util::trim(paragraph[k]));
    }
    ir::Inlines content = inlines(text);
    if (syntax_.paragraph_lines_are_breaks) {
      // Newlines are forced breaks in this dialect.
      ir::Inlines broken;
      for (ir::Inline& item : content) {
        if (auto* t = item.get_if<ir::Text>(); t != nullptr && t->text.find('\n') != std::string::npos) {
          std::size_t start = 0;
          for (std::size_t nl = t->text.find('\n'); nl != std::string::npos; nl = t->text.find('\n', start)) {
            append_text(broken, std::string_view(t->text).substr(start, nl - start));
            broken.push_back(ir::LineBreak{});
            start = nl + 1;
          }
          append_text(broken, std::string_view(t->text).substr(start));
        } else {
          broken.push_back(std::move(item));
        }
      }
      content = std::move(broken);
    }
    if (!content.empty()) {
      if (content.size() == 1) {
        if (auto* image = content.front().get_if<ir::InlineImage>()) {
          out.push_back(ir::ImageBlock{std::move(image->source), std::move(image->alt), {}, std::move(image->title)});
          paragraph.clear();
          return;
        }
      }
      out.push_back(ir::Paragraph{std::move(content)});
    }
    paragraph.clear();
  };
  const auto is_block_start = [&](std::string_view line) {
    return (syntax_.heading && syntax_.heading(line)) || (syntax_.rule && syntax_.rule(line)) ||
           (syntax_.table_row && syntax_.table_row(line)) || (syntax_.quote_line && syntax_.quote_line(line)) ||
           (syntax_.list_item && syntax_.list_item(line));
  };

  std::size_t i = 0;
  while (i < lines.size()) {
    const std::string_view line = lines[i];
    if (is_blank(line)) {
      flush();
      ++i;
      continue;
    }
    if (syntax_.block) {
      ir::Blocks produced;
      const std::size_t used = syntax_.block(lines, i, produced, *this);
      if (used > 0) {
        flush();
        for (ir::Block& b : produced) out.push_back(std::move(b));
        i += used;
        continue;
      }
    }
    if (syntax_.heading) {
      if (auto heading = syntax_.heading(line)) {
        flush();
        ir::Attributes attr;
        attr.id = heading->id;
        out.push_back(ir::Block(ir::Heading{std::clamp(heading->level, 1, 6), inlines(util::trim(heading->text))}, std::move(attr)));
        ++i;
        continue;
      }
    }
    if (syntax_.rule && syntax_.rule(line)) {
      flush();
      out.push_back(ir::HorizontalRule{});
      ++i;
      continue;
    }
    if (syntax_.table_row && syntax_.table_row(line)) {
      flush();
      ir::TableBlock table;
      while (i < lines.size()) {
        auto row = syntax_.table_row(lines[i]);
        if (!row) break;
        ir::TableRow out_row;
        bool all_header = !row->empty();
        for (Cell& cell : *row) {
          if (!cell.header) all_header = false;
          ir::TableCell c;
          c.content = inlines(util::trim(cell.text));
          c.colspan = std::clamp(cell.colspan, 1, 1000);
          c.rowspan = std::clamp(cell.rowspan, 1, 1000);
          c.align = cell.align;
          out_row.cells.push_back(std::move(c));
        }
        out_row.header = all_header && (table.rows.empty() || table.rows.back().header);
        table.rows.push_back(std::move(out_row));
        ++i;
      }
      infer_column_alignment(table);
      out.push_back(std::move(table));
      continue;
    }
    if (syntax_.quote_line && syntax_.quote_line(line)) {
      flush();
      std::vector<std::string> inner;
      while (i < lines.size()) {
        auto content = syntax_.quote_line(lines[i]);
        if (!content) break;
        inner.push_back(std::move(*content));
        ++i;
      }
      std::vector<std::string_view> views(inner.begin(), inner.end());
      ir::BlockQuote quote;
      if (depth < max_quote_depth) {
        quote.blocks = parse(views, depth + 1);
      } else {
        quote.blocks.push_back(ir::Paragraph{{ir::Text{join_lines(views)}}});
      }
      out.push_back(std::move(quote));
      continue;
    }
    if (paragraph.empty() && syntax_.preformatted_line && syntax_.preformatted_line(line)) {
      std::vector<std::string> code;
      while (i < lines.size()) {
        auto content = syntax_.preformatted_line(lines[i]);
        if (!content) break;
        code.push_back(std::move(*content));
        ++i;
      }
      out.push_back(ir::CodeBlock{{}, util::join(code, "\n")});
      continue;
    }
    if (syntax_.list_item && syntax_.list_item(line)) {
      flush();
      std::vector<ListItem> items;
      while (i < lines.size()) {
        auto item = syntax_.list_item(lines[i]);
        if (!item) {
          // Continuation of the previous item's text.
          if (syntax_.list_item_continuation && !items.empty() && !is_blank(lines[i]) && !is_block_start(lines[i]) &&
              !(syntax_.block && [&] {
                ir::Blocks probe;
                return syntax_.block(lines, i, probe, *this) > 0;
              }())) {
            items.back().text.append("\n").append(util::trim(lines[i]));
            ++i;
            continue;
          }
          break;
        }
        item->depth = std::clamp(item->depth, 1, max_list_depth);
        items.push_back(std::move(*item));
        ++i;
      }
      // Build nested lists.
      std::vector<ListFrame> stack;
      const auto close_top = [&] {
        ListFrame frame = std::move(stack.back());
        stack.pop_back();
        if (frame.any_task) frame.list.tasks.resize(frame.list.items.size(), ir::TaskState::None);
        if (stack.empty()) {
          out.push_back(std::move(frame.list));
        } else {
          ir::List& parent = stack.back().list;
          if (parent.items.empty()) {
            parent.items.emplace_back();
            if (stack.back().any_task) parent.tasks.push_back(ir::TaskState::None);
          }
          parent.items.back().push_back(std::move(frame.list));
        }
      };
      for (ListItem& item : items) {
        const bool definition = item.term || item.definition;
        while (!stack.empty() && stack.back().depth > item.depth) close_top();
        if (!stack.empty() && stack.back().depth == item.depth &&
            (stack.back().list.definition != definition || (!definition && stack.back().list.ordered != item.ordered))) {
          close_top();
        }
        if (stack.empty() || stack.back().depth < item.depth) {
          // Deeper markers or indentation open exactly one nested level.
          ListFrame frame;
          frame.depth = item.depth;
          frame.list.ordered = item.ordered;
          frame.list.definition = definition;
          stack.push_back(std::move(frame));
        }
        ListFrame& top = stack.back();
        if (item.term && item.text.find('\n') != std::string::npos) {
          // Continuation lines of a term are its definition.
          const std::size_t nl = item.text.find('\n');
          top.list.items.push_back({ir::Paragraph{inlines(util::trim(std::string_view(item.text).substr(0, nl)))}});
          top.list.items.back().push_back(ir::Paragraph{inlines(util::trim(std::string_view(item.text).substr(nl + 1)))});
          continue;
        }
        ir::Inlines content = inlines(util::trim(item.text));
        if (definition) {
          if (item.term || top.list.items.empty()) {
            top.list.items.push_back({ir::Paragraph{item.term ? std::move(content) : ir::Inlines{}}});
            if (item.term) continue;
          }
          top.list.items.back().push_back(ir::Paragraph{std::move(content)});
          continue;
        }
        top.list.items.push_back({ir::Paragraph{std::move(content)}});
        if (item.task != ir::TaskState::None) top.any_task = true;
        top.list.tasks.resize(top.list.items.size() - 1, ir::TaskState::None);
        top.list.tasks.push_back(item.task);
      }
      while (!stack.empty()) close_top();
      // Tasks vectors are only kept when some item has a box.
      for (ir::Block& block : out) {
        if (auto* list = block.get_if<ir::List>()) {
          if (std::ranges::all_of(list->tasks, [](ir::TaskState t) { return t == ir::TaskState::None; })) list->tasks.clear();
        }
      }
      continue;
    }
    paragraph.push_back(line);
    ++i;
  }
  flush();
  // Nested lists' task vectors.
  for_each_block(out, [](ir::Block& block) {
    if (auto* list = block.get_if<ir::List>()) {
      if (std::ranges::all_of(list->tasks, [](ir::TaskState t) { return t == ir::TaskState::None; })) list->tasks.clear();
    }
  });
  return out;
}

ir::Document read(std::string_view text, const ReadSyntax& syntax) {
  ir::Document document;
  Reader reader(syntax, document);
  const std::vector<std::string_view> lines = util::split_lines(text);
  document.blocks = reader.parse(lines);
  return document;
}

// ================================================================================ writing

Writer::Writer(const ir::Document& document, const WriteSyntax& syntax, InlineMarkup markup, int wrap_width)
    : doc_(document), syntax_(syntax), markup_(std::move(markup)), wrap_width_(wrap_width) {}

std::string Writer::inlines(const ir::Inlines& content) { return render_inlines(content, markup_, &doc_); }

std::string Writer::paragraph(const ir::Inlines& content) {
  std::string text = inlines(content);
  if (wrap_width_ > 0) text = wrap_text(text, static_cast<std::size_t>(wrap_width_));
  if (!syntax_.escape_line_start) return text;
  std::string out;
  bool first = true;
  for (const std::string_view line : util::split(text, '\n')) {
    if (!first) out.push_back('\n');
    out.append(syntax_.escape_line_start(std::string(line)));
    first = false;
  }
  return out;
}

std::string Writer::blocks(const ir::Blocks& list, int depth) {
  std::string out;
  for (const ir::Block& b : list) {
    std::string text = block(b, depth);
    if (text.empty()) continue;
    if (!out.empty()) out.append(syntax_.block_separator);
    out.append(text);
  }
  return out;
}

std::string Writer::list(const ir::List& list, std::vector<bool> path, int depth) {
  std::string out;
  if (list.definition && syntax_.definition) {
    for (const ir::Blocks& item : list.items) {
      if (item.empty()) continue;
      std::string term;
      if (const auto* p = item.front().get_if<ir::Paragraph>()) term = inlines(p->content);
      std::vector<std::string> definitions;
      for (std::size_t b = 1; b < item.size(); ++b) {
        std::string text = block(item[b], depth + 1);
        if (!text.empty()) definitions.push_back(util::replace_all(text, "\n", syntax_.item_break));
      }
      if (!out.empty()) out.push_back('\n');
      out.append(syntax_.definition(util::replace_all(term, "\n", " "), definitions));
    }
    return out;
  }
  path.push_back(list.ordered);
  int number = list.start;
  for (std::size_t i = 0; i < list.items.size(); ++i) {
    const ir::Blocks& item = list.items[i];
    std::string first_line;
    std::string trailing;
    bool first = true;
    for (std::size_t b = 0; b < item.size(); ++b) {
      const ir::Block& part = item[b];
      if (list.definition && b == 0) {
        if (const auto* p = part.get_if<ir::Paragraph>()) {
          ir::Inlines term{ir::Strong{p->content}};
          first_line = inlines(term);
          first = false;
          continue;
        }
      }
      if (const auto* nested = part.get_if<ir::List>()) {
        if (depth < 32) trailing.append("\n").append(this->list(*nested, path, depth + 1));
        continue;
      }
      std::string text = block(part, depth + 1);
      for (char& c : text) {
        if (c == '\n') c = ' ';
      }
      if (part.is<ir::Paragraph>()) text = util::replace_all(inlines(part.get_if<ir::Paragraph>()->content), "\n", " ");
      if (first) {
        first_line = text;
        first = false;
      } else {
        first_line.append(syntax_.item_break).append(text);
      }
    }
    if (!out.empty()) out.push_back('\n');
    out.append(syntax_.list_marker(path, number++, list.task(i))).append(first_line).append(trailing);
  }
  return out;
}

std::string Writer::block(const ir::Block& block, int depth) {
  if (depth > ir::max_document_depth * 2) return {};
  std::string out;
  std::visit(
      [&](const auto& node) {
        using T = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<T, ir::Paragraph>) {
          out = paragraph(node.content);
        } else if constexpr (std::same_as<T, ir::Heading>) {
          std::string text = inlines(node.content);
          for (char& c : text) {
            if (c == '\n') c = ' ';
          }
          out = syntax_.heading(std::clamp(node.level, 1, 6), std::string(util::trim(text)), block.attr);
        } else if constexpr (std::same_as<T, ir::List>) {
          out = list(node, {}, depth);
        } else if constexpr (std::same_as<T, ir::CodeBlock>) {
          if (!node.raw_format.empty() && node.raw_format != syntax_.raw_format && node.math == ir::MathMode::None) {
            out.clear();  // raw markup of another format has no meaning here
          } else if (!node.raw_format.empty() && node.raw_format == syntax_.raw_format) {
            out = node.code;
          } else {
            out = syntax_.code_block(node);
          }
        } else if constexpr (std::same_as<T, ir::BlockQuote>) {
          out = syntax_.quote(blocks(node.blocks, depth + 1));
        } else if constexpr (std::same_as<T, ir::TableBlock>) {
          out = syntax_.table(node, [this](const ir::Inlines& content) {
            std::string text = inlines(content);
            for (char& c : text) {
              if (c == '\n') c = ' ';
            }
            return text;
          });
        } else if constexpr (std::same_as<T, ir::ImageBlock>) {
          const std::string url = image_url(node.source, true);
          if (url.empty() || !syntax_.image_block) {
            const std::string& text = node.caption.empty() ? node.alt : node.caption;
            out = text.empty() ? std::string() : paragraph({ir::Text{text}});
          } else {
            out = syntax_.image_block(node, url);
          }
        } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
          out = syntax_.rule;
        } else if constexpr (std::same_as<T, ir::PageBreak>) {
          out.clear();
        }
      },
      block.node);
  return out;
}

std::string Writer::run() {
  std::string out = blocks(doc_.blocks, 0);
  if (syntax_.notes_section && !doc_.footnotes.empty()) {
    std::vector<std::string> bodies;
    for (const ir::Footnote& note : doc_.footnotes) {
      std::string body = blocks(note.blocks, 1);
      bodies.push_back(util::replace_all(body, "\n\n", " "));
    }
    if (!out.empty()) out.append(syntax_.block_separator);
    out.append(syntax_.notes_section(bodies));
  }
  while (out.ends_with('\n')) out.pop_back();
  if (!out.empty()) out.push_back('\n');
  return out;
}

}  // namespace cvt::document::wiki
