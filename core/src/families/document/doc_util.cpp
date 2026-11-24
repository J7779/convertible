// Convertible core - document family: the document utilities of document_api.hpp
// (outline, contents, notes, tables, depth, wrapping, pictures).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <utility>

#include "families/document/doc_common.hpp"
#include "families/document/document_api.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::document {

// =========================================================================== slugs and outline

std::string slugify(std::string_view text) {
  std::string out;
  std::size_t i = 0;
  while (i < text.size()) {
    const util::Utf8Decoded d = util::decode_utf8(text, i);
    const char32_t cp = d.code_point;
    i += std::max<std::size_t>(d.length, 1);
    if (cp < 0x80) {
      const char c = static_cast<char>(cp);
      if (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '-') {
        out.push_back(util::ascii_lower(c));
      } else if (c == ' ' || c == '\t' || c == '\n') {
        out.push_back('-');
      }
    } else if (d.ok && !(cp >= 0x2000 && cp <= 0x206F) && !(cp >= 0x3000 && cp <= 0x303F) && cp != 0xA0 &&
               !(cp >= 0x1F000 && cp <= 0x1FAFF) && !(cp >= 0x2600 && cp <= 0x27BF) && !(cp >= 0xFE00 && cp <= 0xFE0F)) {
      // Letters of other scripts are kept; simple Latin-1/Greek/Cyrillic upper case folded.
      char32_t folded = cp;
      if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) || (cp >= 0x391 && cp <= 0x3AB) || (cp >= 0x410 && cp <= 0x42F)) {
        folded = cp + 0x20;
      }
      util::append_utf8(out, folded);
    }
  }
  return out;
}

std::string unique_slug(std::string_view text, std::set<std::string>& used) {
  std::string base = slugify(text);
  if (base.empty()) base = "section";
  std::string candidate = base;
  for (int n = 1; used.contains(candidate); ++n) candidate = base + "-" + std::to_string(n);
  used.insert(candidate);
  return candidate;
}

namespace {

void collect_outline(const ir::Blocks& blocks, std::vector<OutlineEntry>& out, int depth) {
  if (depth > ir::max_document_depth) return;
  for (const ir::Block& block : blocks) {
    if (const auto* h = block.get_if<ir::Heading>()) {
      out.push_back({h->level, inline_line(h->content), block.attr.id});
    } else if (const auto* list = block.get_if<ir::List>()) {
      for (const ir::Blocks& item : list->items) collect_outline(item, out, depth + 1);
    } else if (const auto* quote = block.get_if<ir::BlockQuote>()) {
      collect_outline(quote->blocks, out, depth + 1);
    }
  }
}

}  // namespace

std::vector<OutlineEntry> heading_outline(const ir::Document& document) {
  std::vector<OutlineEntry> out;
  collect_outline(document.blocks, out, 0);
  return out;
}

void assign_heading_ids(ir::Document& document) {
  std::set<std::string> used;
  for_each_block(document.blocks, [&](ir::Block& block) {
    if (!block.attr.id.empty()) used.insert(block.attr.id);
  });
  for_each_block(document.blocks, [&](ir::Block& block) {
    if (const auto* h = block.get_if<ir::Heading>(); h != nullptr && block.attr.id.empty()) {
      block.attr.id = unique_slug(inline_line(h->content), used);
    }
  });
}

ir::Block table_of_contents(ir::Document& document, int max_level) {
  assign_heading_ids(document);
  const std::vector<OutlineEntry> outline = heading_outline(document);
  // Build nested lists with an explicit stack of (level, list).
  struct Frame {
    int level;
    ir::List list;
  };
  std::vector<Frame> stack;
  int base = 7;
  for (const OutlineEntry& e : outline) {
    if (e.level <= max_level) base = std::min(base, e.level);
  }
  stack.push_back({base, ir::List{}});
  const auto close_top = [&] {
    Frame top = std::move(stack.back());
    stack.pop_back();
    ir::List& parent = stack.back().list;
    if (parent.items.empty()) parent.items.emplace_back();
    parent.items.back().push_back(std::move(top.list));
  };
  for (const OutlineEntry& e : outline) {
    if (e.level > max_level) continue;
    while (stack.size() > 1 && e.level < stack.back().level) close_top();
    while (e.level > stack.back().level && stack.size() < 6) stack.push_back({stack.back().level + 1, ir::List{}});
    ir::Inlines label{ir::Link{"#" + e.id, {}, {ir::Text{e.text}}}};
    stack.back().list.items.push_back({ir::Paragraph{std::move(label)}});
  }
  while (stack.size() > 1) close_top();
  ir::Attributes attr;
  attr.classes.push_back("toc");
  return ir::Block(std::move(stack.front().list), std::move(attr));
}

void insert_table_of_contents(ir::Document& document, int max_level) {
  const std::vector<OutlineEntry> outline = heading_outline(document);
  if (outline.empty()) return;
  ir::Block toc = table_of_contents(document, max_level);
  std::size_t position = 0;
  if (!document.blocks.empty()) {
    if (const auto* h = document.blocks.front().get_if<ir::Heading>(); h != nullptr && h->level == 1) {
      // A single leading title stays above the contents.
      std::size_t level1 = 0;
      for (const OutlineEntry& e : outline) level1 += e.level == 1 ? 1 : 0;
      if (level1 == 1) position = 1;
    }
  }
  ir::Attributes heading_attr;
  heading_attr.classes.push_back("toc-title");
  ir::Block heading(ir::Heading{2, {ir::Text{"Contents"}}}, std::move(heading_attr));
  document.blocks.insert(document.blocks.begin() + static_cast<std::ptrdiff_t>(position), std::move(toc));
  document.blocks.insert(document.blocks.begin() + static_cast<std::ptrdiff_t>(position), std::move(heading));
}

// =========================================================================== footnotes

namespace {

void visit_links(ir::Inlines& inlines, const std::function<void(ir::Link&)>& visit, int depth) {
  if (depth > ir::max_document_depth * 2) return;
  for (ir::Inline& item : inlines) {
    std::visit(
        [&](auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Link>) {
            visit(node);
            visit_links(node.children, visit, depth + 1);
          } else if constexpr (requires { node.children; }) {
            visit_links(node.children, visit, depth + 1);
          }
        },
        item.node);
  }
}

void for_each_link(ir::Document& document, const std::function<void(ir::Link&)>& visit) {
  // Notes can reference notes; walk the bodies too (in note order).
  for_each_inlines(document, [&](ir::Inlines& inlines) { visit_links(inlines, visit, 0); });
}

}  // namespace

void number_footnotes(ir::Document& document) {
  std::vector<std::string> order;
  std::set<std::string> seen;
  // First references in the main text, then in the notes themselves.
  const auto record = [&](ir::Link& link) {
    if (!link.note.empty() && seen.insert(link.note).second) order.push_back(link.note);
  };
  for_each_block(document.blocks, [&](ir::Block& block) {
    if (auto* p = block.get_if<ir::Paragraph>()) visit_links(p->content, record, 0);
    if (auto* h = block.get_if<ir::Heading>()) visit_links(h->content, record, 0);
    if (auto* t = block.get_if<ir::TableBlock>()) {
      for (ir::TableRow& row : t->rows) {
        for (ir::TableCell& cell : row.cells) visit_links(cell.content, record, 0);
      }
    }
  });
  for (std::size_t i = 0; i < document.footnotes.size() && i < 100000; ++i) {
    // Iterate over a snapshot index: notes referenced only from notes.
    if (!seen.contains(document.footnotes[i].label)) continue;
    for_each_block(document.footnotes[i].blocks, [&](ir::Block& block) {
      if (auto* p = block.get_if<ir::Paragraph>()) visit_links(p->content, record, 0);
    });
  }
  std::map<std::string, std::string> renamed;
  std::vector<ir::Footnote> notes;
  for (std::size_t i = 0; i < order.size(); ++i) {
    const std::string label = std::to_string(i + 1);
    renamed[order[i]] = label;
    ir::Footnote note;
    note.label = label;
    if (const ir::Footnote* found = find_footnote(document, order[i])) note.blocks = found->blocks;
    notes.push_back(std::move(note));
  }
  document.footnotes = std::move(notes);
  for_each_link(document, [&](ir::Link& link) {
    if (link.note.empty()) return;
    const auto it = renamed.find(link.note);
    if (it == renamed.end()) return;
    link.note = it->second;
    link.url = "#fn-" + it->second;
    link.children = {ir::Text{it->second}};
  });
}

void footnotes_to_endnotes(ir::Document& document) {
  if (document.footnotes.empty()) return;
  ir::List list;
  list.ordered = true;
  for (ir::Footnote& note : document.footnotes) {
    ir::Blocks body = std::move(note.blocks);
    if (body.empty()) body.push_back(ir::Paragraph{});
    body.front().attr.id = "fn-" + note.label;
    list.items.push_back(std::move(body));
  }
  for_each_link(document, [](ir::Link& link) {
    if (link.note.empty()) return;
    ir::Inlines marker = std::move(link.children);
    link.children = {ir::Superscript{std::move(marker)}};
    link.note.clear();
  });
  document.footnotes.clear();
  document.blocks.push_back(ir::HorizontalRule{});
  ir::Attributes attr;
  attr.classes.push_back("footnotes");
  document.blocks.push_back(ir::Block(std::move(list), std::move(attr)));
}

// =========================================================================== transformations

void shift_headings(ir::Document& document, int delta) {
  const auto shift = [delta](ir::Block& block) {
    if (auto* h = block.get_if<ir::Heading>()) h->level = std::clamp(h->level + delta, 1, 6);
  };
  for_each_block(document.blocks, shift);
  for (ir::Footnote& note : document.footnotes) for_each_block(note.blocks, shift);
}

void strip_metadata(ir::Document& document) {
  document.metadata.author.clear();
  document.metadata.date.clear();
  document.metadata.extra.clear();
}

namespace {

void images_in_inlines(ir::Inlines& inlines, ImageHandling handling, int depth) {
  if (depth > ir::max_document_depth * 2) return;
  ir::Inlines out;
  out.reserve(inlines.size());
  for (ir::Inline& item : inlines) {
    if (auto* image = item.get_if<ir::InlineImage>()) {
      if (handling == ImageHandling::Drop) {
        if (!image->alt.empty()) append_text(out, image->alt);
        continue;
      }
      if (const auto* encoded = std::get_if<ir::EncodedImage>(&image->source); encoded != nullptr && !encoded->url.empty()) {
        out.push_back(std::move(item));  // already a reference
        continue;
      }
      if (!image->alt.empty()) append_text(out, image->alt);
      continue;
    }
    std::visit(
        [&](auto& node) {
          if constexpr (requires { node.children; }) images_in_inlines(node.children, handling, depth + 1);
        },
        item.node);
    out.push_back(std::move(item));
  }
  inlines = std::move(out);
}

void images_in_blocks(ir::Blocks& blocks, ImageHandling handling, int depth) {
  if (depth > ir::max_document_depth * 2) return;
  ir::Blocks out;
  out.reserve(blocks.size());
  for (ir::Block& block : blocks) {
    if (auto* image = block.get_if<ir::ImageBlock>()) {
      const auto* encoded = std::get_if<ir::EncodedImage>(&image->source);
      const bool reference = encoded != nullptr && !encoded->url.empty();
      if (handling == ImageHandling::Link && reference) {
        out.push_back(std::move(block));
        continue;
      }
      std::string text = !image->caption.empty() ? image->caption : image->alt;
      if (!text.empty()) out.push_back(ir::Paragraph{{ir::Text{std::move(text)}}});
      continue;
    }
    if (auto* p = block.get_if<ir::Paragraph>()) {
      images_in_inlines(p->content, handling, depth);
      if (p->content.empty()) continue;
    } else if (auto* h = block.get_if<ir::Heading>()) {
      images_in_inlines(h->content, handling, depth);
    } else if (auto* list = block.get_if<ir::List>()) {
      for (ir::Blocks& item : list->items) images_in_blocks(item, handling, depth + 1);
    } else if (auto* quote = block.get_if<ir::BlockQuote>()) {
      images_in_blocks(quote->blocks, handling, depth + 1);
    } else if (auto* table = block.get_if<ir::TableBlock>()) {
      for (ir::TableRow& row : table->rows) {
        for (ir::TableCell& cell : row.cells) images_in_inlines(cell.content, handling, depth);
      }
    }
    out.push_back(std::move(block));
  }
  blocks = std::move(out);
}

}  // namespace

void apply_image_handling(ir::Document& document, ImageHandling handling) {
  if (handling == ImageHandling::Embed) return;
  images_in_blocks(document.blocks, handling, 0);
  for (ir::Footnote& note : document.footnotes) images_in_blocks(note.blocks, handling, 0);
}

// =========================================================================== tables

TableGrid table_grid(const ir::TableBlock& table) {
  TableGrid grid;
  constexpr std::size_t max_columns = 1000;
  constexpr int max_span = 1000;
  // occupied[r] = columns already covered by row spans from above.
  std::vector<std::vector<const ir::TableCell*>> cells(table.rows.size());
  std::vector<std::vector<bool>> origin(table.rows.size());
  for (std::size_t r = 0; r < table.rows.size(); ++r) {
    std::size_t column = 0;
    for (const ir::TableCell& cell : table.rows[r].cells) {
      while (column < cells[r].size() && cells[r][column] != nullptr) ++column;
      if (column >= max_columns) break;
      const auto colspan = static_cast<std::size_t>(std::clamp(cell.colspan, 1, max_span));
      const auto rowspan = static_cast<std::size_t>(std::clamp(cell.rowspan, 1, max_span));
      const std::size_t last_row = std::min(table.rows.size(), r + rowspan);
      for (std::size_t rr = r; rr < last_row; ++rr) {
        const std::size_t end = std::min(max_columns, column + colspan);
        if (cells[rr].size() < end) {
          cells[rr].resize(end, nullptr);
          origin[rr].resize(end, false);
        }
        for (std::size_t cc = column; cc < end; ++cc) {
          if (cells[rr][cc] == nullptr) {
            cells[rr][cc] = &cell;
            origin[rr][cc] = rr == r && cc == column;
          }
        }
      }
      column += colspan;
    }
  }
  for (const auto& row : cells) grid.columns = std::max(grid.columns, row.size());
  for (std::size_t r = 0; r < cells.size(); ++r) {
    cells[r].resize(grid.columns, nullptr);
    origin[r].resize(grid.columns, false);
  }
  grid.cells = std::move(cells);
  grid.origin = std::move(origin);
  grid.header_rows = header_row_count(table);
  return grid;
}

ir::TableBlock normalize_table(const ir::TableBlock& table, bool repeat_spanned) {
  const TableGrid grid = table_grid(table);
  ir::TableBlock out;
  out.caption = table.caption;
  out.alignments = table.alignments;
  for (std::size_t r = 0; r < grid.cells.size(); ++r) {
    ir::TableRow row;
    row.header = table.rows[r].header;
    for (std::size_t c = 0; c < grid.columns; ++c) {
      ir::TableCell cell;
      if (const ir::TableCell* source = grid.cells[r][c]; source != nullptr && (grid.origin[r][c] || repeat_spanned)) {
        cell.content = source->content;
        cell.align = source->align;
      }
      row.cells.push_back(std::move(cell));
    }
    out.rows.push_back(std::move(row));
  }
  return out;
}

// =========================================================================== normalisation and depth

namespace {

bool inline_is_empty_container(const ir::Inline& item) {
  bool empty = false;
  std::visit(
      [&](const auto& node) {
        using T = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<T, ir::Link>) {
          empty = false;  // an empty link may still be an anchor or a note
        } else if constexpr (requires { node.children; }) {
          empty = node.children.empty();
        } else if constexpr (std::same_as<T, ir::Text>) {
          empty = node.text.empty();
        }
      },
      item.node);
  return empty && item.attr.id.empty();
}

void normalize_inlines_depth(ir::Inlines& inlines, int depth) {
  if (depth > ir::max_document_depth * 2) return;
  ir::Inlines out;
  out.reserve(inlines.size());
  for (ir::Inline& item : inlines) {
    std::visit(
        [&](auto& node) {
          if constexpr (requires { node.children; }) normalize_inlines_depth(node.children, depth + 1);
        },
        item.node);
    if (inline_is_empty_container(item)) continue;
    if (const auto* text = item.get_if<ir::Text>(); text != nullptr && item.attr.empty()) {
      append_text(out, text->text);
      continue;
    }
    out.push_back(std::move(item));
  }
  inlines = std::move(out);
}

// Flatten inline content below `remaining` levels: formatting is dropped,
// text, code, breaks and pictures are kept. Iterative so arbitrarily deep
// input cannot overflow the stack.
void flatten_inlines(ir::Inlines& inlines, int remaining) {
  if (remaining > 0) {
    for (ir::Inline& item : inlines) {
      std::visit(
          [&](auto& node) {
            if constexpr (requires { node.children; }) flatten_inlines(node.children, remaining - 1);
          },
          item.node);
    }
    return;
  }
  ir::Inlines flat;
  std::vector<ir::Inline> stack(std::make_move_iterator(inlines.rbegin()), std::make_move_iterator(inlines.rend()));
  inlines.clear();
  while (!stack.empty()) {
    ir::Inline item = std::move(stack.back());
    stack.pop_back();
    bool container = false;
    std::visit(
        [&](auto& node) {
          if constexpr (requires { node.children; }) {
            container = true;
            for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) stack.push_back(std::move(*it));
            node.children.clear();
          }
        },
        item.node);
    if (container) continue;
    if (const auto* text = item.get_if<ir::Text>()) {
      append_text(flat, text->text);
    } else {
      flat.push_back(std::move(item));
    }
  }
  inlines = std::move(flat);
}

void enforce_blocks(ir::Blocks& blocks, int remaining);

void flatten_blocks(ir::Blocks& blocks) {
  // Everything below this level becomes a flat sequence of leaf blocks.
  ir::Blocks flat;
  std::vector<ir::Block> stack(std::make_move_iterator(blocks.rbegin()), std::make_move_iterator(blocks.rend()));
  blocks.clear();
  while (!stack.empty()) {
    ir::Block block = std::move(stack.back());
    stack.pop_back();
    if (auto* list = block.get_if<ir::List>()) {
      for (auto it = list->items.rbegin(); it != list->items.rend(); ++it) {
        for (auto b = it->rbegin(); b != it->rend(); ++b) stack.push_back(std::move(*b));
      }
      continue;
    }
    if (auto* quote = block.get_if<ir::BlockQuote>()) {
      for (auto it = quote->blocks.rbegin(); it != quote->blocks.rend(); ++it) stack.push_back(std::move(*it));
      continue;
    }
    flat.push_back(std::move(block));
  }
  blocks = std::move(flat);
}

void enforce_leaf(ir::Block& block, int remaining) {
  const int inline_budget = std::max(remaining, 1);
  if (auto* p = block.get_if<ir::Paragraph>()) flatten_inlines(p->content, inline_budget);
  if (auto* h = block.get_if<ir::Heading>()) flatten_inlines(h->content, inline_budget);
  if (auto* t = block.get_if<ir::TableBlock>()) {
    for (ir::TableRow& row : t->rows) {
      for (ir::TableCell& cell : row.cells) flatten_inlines(cell.content, inline_budget);
    }
    flatten_inlines(t->caption, inline_budget);
  }
}

void enforce_blocks(ir::Blocks& blocks, int remaining) {
  if (remaining <= 1) flatten_blocks(blocks);
  for (ir::Block& block : blocks) {
    if (auto* list = block.get_if<ir::List>()) {
      for (ir::Blocks& item : list->items) enforce_blocks(item, remaining - 1);
    } else if (auto* quote = block.get_if<ir::BlockQuote>()) {
      enforce_blocks(quote->blocks, remaining - 1);
    } else {
      enforce_leaf(block, remaining);
    }
  }
}

}  // namespace

void normalize_inlines(ir::Inlines& inlines) { normalize_inlines_depth(inlines, 0); }

void normalize_document(ir::Document& document) {
  for_each_inlines(document, [](ir::Inlines& inlines) { normalize_inlines_depth(inlines, 0); });
}

void enforce_depth(ir::Document& document, int max_depth) {
  max_depth = std::max(max_depth, 2);
  enforce_blocks(document.blocks, max_depth);
  for (ir::Footnote& note : document.footnotes) enforce_blocks(note.blocks, max_depth - 1);
}

// =========================================================================== width and wrapping

namespace {

bool is_wide(char32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
         (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
         (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
         (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
         (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x1FA70 && cp <= 0x1FAFF) || (cp >= 0x1F680 && cp <= 0x1F6FF) ||
         (cp >= 0x20000 && cp <= 0x3FFFD);
}

bool is_zero_width(char32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) || (cp >= 0x0591 && cp <= 0x05BD) ||
         (cp >= 0x0610 && cp <= 0x061A) || (cp >= 0x064B && cp <= 0x065F) || (cp >= 0x0E31 && cp <= 0x0E3A && cp != 0x0E32 && cp != 0x0E33) ||
         (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x200B && cp <= 0x200F) ||
         (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x20D0 && cp <= 0x20FF) ||
         (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0xFEFF ||
         (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

}  // namespace

std::size_t display_width(std::string_view utf8) {
  std::size_t width = 0;
  std::size_t i = 0;
  while (i < utf8.size()) {
    const util::Utf8Decoded d = util::decode_utf8(utf8, i);
    i += std::max<std::size_t>(d.length, 1);
    if (d.code_point < 0x20) continue;
    if (is_zero_width(d.code_point)) continue;
    width += is_wide(d.code_point) ? 2 : 1;
  }
  return width;
}

std::string wrap_text(std::string_view text, std::size_t width, std::string_view first_prefix, std::string_view prefix,
                      std::string_view newline) {
  std::string out;
  bool first_line = true;
  for (const std::string_view paragraph_line : util::split(text, '\n')) {
    if (!first_line) out.append(newline);
    const std::string_view line_prefix = first_line ? first_prefix : prefix;
    out.append(line_prefix);
    first_line = false;
    std::size_t column = display_width(line_prefix);
    bool line_has_word = false;
    std::size_t i = 0;
    while (i < paragraph_line.size()) {
      while (i < paragraph_line.size() && paragraph_line[i] == ' ') ++i;
      if (i >= paragraph_line.size()) break;
      std::size_t end = paragraph_line.find(' ', i);
      if (end == std::string_view::npos) end = paragraph_line.size();
      const std::string_view word = paragraph_line.substr(i, end - i);
      const std::size_t word_width = display_width(word);
      if (line_has_word && width > 0 && column + 1 + word_width > width) {
        out.append(newline);
        out.append(prefix);
        column = display_width(prefix);
        line_has_word = false;
      }
      if (line_has_word) {
        out.push_back(' ');
        ++column;
      }
      out.append(word);
      column += word_width;
      line_has_word = true;
      i = end;
    }
  }
  // No trailing spaces on any line.
  std::string cleaned;
  cleaned.reserve(out.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i] == ' ') {
      std::size_t j = i;
      while (j < out.size() && out[j] == ' ') ++j;
      if (j == out.size() || out[j] == '\n' || out[j] == '\r') {
        i = j - 1;
        continue;
      }
    }
    cleaned.push_back(out[i]);
  }
  return cleaned;
}

// =========================================================================== pictures

std::string sniff_image_mime(ByteView data) {
  const std::string_view s = as_chars(data);
  if (s.starts_with("\x89PNG")) return "image/png";
  if (s.starts_with("\xFF\xD8\xFF")) return "image/jpeg";
  if (s.starts_with("GIF8")) return "image/gif";
  if (s.size() >= 12 && s.starts_with("RIFF") && s.substr(8, 4) == "WEBP") return "image/webp";
  if (s.starts_with("BM")) return "image/bmp";
  if (s.starts_with(std::string_view("II*\0", 4)) || s.starts_with(std::string_view("MM\0*", 4))) return "image/tiff";
  if (s.starts_with(std::string_view("\0\0\1\0", 4))) return "image/x-icon";
  if (s.size() >= 12 && s.substr(4, 4) == "ftyp") {
    const std::string_view brand = s.substr(8, 4);
    if (brand == "avif" || brand == "avis") return "image/avif";
    if (brand == "heic" || brand == "heix" || brand == "mif1") return "image/heic";
  }
  if (s.starts_with("\xFF\x0A") || s.starts_with(std::string_view("\0\0\0\x0CJXL ", 8))) return "image/jxl";
  const std::string_view head = util::trim_left(s.substr(0, 512));
  if (head.starts_with("<svg") || ((head.starts_with("<?xml") || head.starts_with("<!--")) && s.substr(0, 1024).find("<svg") != std::string_view::npos)) {
    return "image/svg+xml";
  }
  return {};
}

std::optional<ir::EncodedImage> encoded_image(const ir::ImageSource& source) {
  if (const auto* encoded = std::get_if<ir::EncodedImage>(&source)) {
    if (encoded->bytes.empty()) return std::nullopt;
    ir::EncodedImage copy = *encoded;
    if (copy.mime.empty()) copy.mime = sniff_image_mime(copy.bytes);
    if (copy.mime.empty()) copy.mime = "application/octet-stream";
    return copy;
  }
  const ir::Image& image = std::get<ir::Image>(source);
  if (image.frames.empty() || !image.frames.front().valid()) return std::nullopt;
  ir::EncodedImage out;
  out.bytes = encode_png(image.frames.front());
  if (out.bytes.empty()) return std::nullopt;
  out.mime = "image/png";
  return out;
}

std::string image_data_uri(const ir::ImageSource& source) {
  const auto encoded = encoded_image(source);
  if (!encoded) return {};
  return "data:" + encoded->mime + ";base64," + util::base64_encode(encoded->bytes);
}

std::string image_url(const ir::ImageSource& source, bool allow_data_uri) {
  if (const auto* encoded = std::get_if<ir::EncodedImage>(&source); encoded != nullptr && !encoded->url.empty()) {
    return encoded->url;
  }
  return allow_data_uri ? image_data_uri(source) : std::string();
}

std::optional<ir::EncodedImage> parse_data_uri_image(std::string_view uri) {
  uri = util::trim(uri);
  if (!util::istarts_with(uri, "data:")) return std::nullopt;
  const std::size_t comma = uri.find(',');
  if (comma == std::string_view::npos) return std::nullopt;
  std::string_view header = uri.substr(5, comma - 5);
  const std::string_view payload = uri.substr(comma + 1);
  bool base64 = false;
  if (util::iends_with(header, ";base64")) {
    base64 = true;
    header.remove_suffix(7);
  }
  ir::EncodedImage image;
  const std::size_t semicolon = header.find(';');
  image.mime = lower(util::trim(header.substr(0, semicolon)));
  if (base64) {
    std::string cleaned;
    cleaned.reserve(payload.size());
    for (std::size_t i = 0; i < payload.size(); ++i) {
      if (payload[i] == '%' && i + 2 < payload.size() && payload[i + 1] == '3' &&
          (payload[i + 2] == 'D' || payload[i + 2] == 'd')) {
        cleaned.push_back('=');
        i += 2;
      } else if (!util::is_ascii_space(payload[i])) {
        cleaned.push_back(payload[i]);
      }
    }
    auto decoded = util::base64_decode(cleaned);
    if (!decoded) return std::nullopt;
    image.bytes = std::move(*decoded);
  } else {
    std::string bytes;
    for (std::size_t i = 0; i < payload.size(); ++i) {
      if (payload[i] == '%' && i + 2 < payload.size()) {
        const auto value = util::parse_int(payload.substr(i + 1, 2), 16);
        if (value) {
          bytes.push_back(static_cast<char>(*value));
          i += 2;
          continue;
        }
      }
      bytes.push_back(payload[i]);
    }
    image.bytes = to_bytes(bytes);
  }
  if (image.mime.empty() || image.mime == "application/octet-stream") image.mime = sniff_image_mime(image.bytes);
  return image;
}

}  // namespace cvt::document
