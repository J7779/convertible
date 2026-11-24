// Helpers for the office family tests: fixtures and structure queries.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "families/document/document_api.hpp"
#include "ir/archive.hpp"
#include "ir/document.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"
#include "util/deflate.hpp"
#include "util/zip.hpp"

namespace cvt::test::office {

/// A fixture of tests/data/office, gunzipped when its name ends in ".gz".
inline Bytes data(std::string_view name) {
  Bytes bytes = fixture("office/" + std::string(name));
  if (name.ends_with(".gz")) {
    auto inflated = util::gzip_decompress(bytes);
    if (inflated) return std::move(*inflated);
  }
  return bytes;
}

inline void walk(const ir::Blocks& blocks, const std::function<void(const ir::Block&)>& visit, int depth = 0) {
  if (depth > 80) return;
  for (const ir::Block& b : blocks) {
    visit(b);
    if (const auto* l = b.get_if<ir::List>()) {
      for (const ir::Blocks& item : l->items) walk(item, visit, depth + 1);
    } else if (const auto* q = b.get_if<ir::BlockQuote>()) {
      walk(q->blocks, visit, depth + 1);
    }
  }
}

inline void walk_inlines(const ir::Inlines& inlines, const std::function<void(const ir::Inline&)>& visit) {
  for (const ir::Inline& i : inlines) {
    visit(i);
    std::visit(
        [&](const auto& node) {
          if constexpr (requires { node.children; }) walk_inlines(node.children, visit);
        },
        i.node);
  }
}

inline void each_inline(const ir::Document& d, const std::function<void(const ir::Inline&)>& visit) {
  const auto on_block = [&](const ir::Block& b) {
    if (const auto* p = b.get_if<ir::Paragraph>()) walk_inlines(p->content, visit);
    if (const auto* h = b.get_if<ir::Heading>()) walk_inlines(h->content, visit);
    if (const auto* t = b.get_if<ir::TableBlock>()) {
      for (const auto& row : t->rows) {
        for (const auto& cell : row.cells) walk_inlines(cell.content, visit);
      }
    }
  };
  walk(d.blocks, on_block);
  for (const ir::Footnote& n : d.footnotes) walk(n.blocks, on_block);
}

/// "level:text" for every heading.
inline std::vector<std::string> outline(const ir::Document& d) {
  std::vector<std::string> out;
  for (const auto& e : document::heading_outline(d)) out.push_back(std::to_string(e.level) + ":" + e.text);
  return out;
}

inline bool has_heading(const ir::Document& d, int level, std::string_view text) {
  for (const auto& e : document::heading_outline(d)) {
    if (e.level == level && e.text == text) return true;
  }
  return false;
}

template <class T>
inline std::size_t count(const ir::Document& d) {
  std::size_t n = 0;
  walk(d.blocks, [&](const ir::Block& b) { n += b.is<T>() ? 1 : 0; });
  return n;
}

inline std::vector<const ir::TableBlock*> tables(const ir::Document& d) {
  std::vector<const ir::TableBlock*> out;
  walk(d.blocks, [&](const ir::Block& b) {
    if (const auto* t = b.get_if<ir::TableBlock>()) out.push_back(t);
  });
  return out;
}

inline std::vector<const ir::List*> lists(const ir::Document& d) {
  std::vector<const ir::List*> out;
  walk(d.blocks, [&](const ir::Block& b) {
    if (const auto* l = b.get_if<ir::List>()) out.push_back(l);
  });
  return out;
}

/// Link targets in reading order.
inline std::vector<std::string> links(const ir::Document& d) {
  std::vector<std::string> out;
  each_inline(d, [&](const ir::Inline& i) {
    if (const auto* l = i.get_if<ir::Link>(); l && l->note.empty()) out.push_back(l->url);
  });
  return out;
}

inline bool has_link(const ir::Document& d, std::string_view url) {
  for (const std::string& l : links(d)) {
    if (l == url) return true;
  }
  return false;
}

/// Every picture (blocks and inline) in reading order.
inline std::vector<ir::ImageSource> pictures(const ir::Document& d) {
  std::vector<ir::ImageSource> out;
  walk(d.blocks, [&](const ir::Block& b) {
    if (const auto* i = b.get_if<ir::ImageBlock>()) out.push_back(i->source);
  });
  each_inline(d, [&](const ir::Inline& i) {
    if (const auto* img = i.get_if<ir::InlineImage>()) out.push_back(img->source);
  });
  return out;
}

inline std::size_t picture_bytes(const ir::ImageSource& s) {
  if (const auto* e = std::get_if<ir::EncodedImage>(&s)) return e->bytes.size();
  return 0;
}

/// True when some inline of this kind contains `text`.
template <class T>
inline bool formatted(const ir::Document& d, std::string_view text) {
  bool found = false;
  each_inline(d, [&](const ir::Inline& i) {
    if (const auto* node = i.get_if<T>()) {
      if constexpr (requires { node->children; }) {
        if (ir::plain_text(node->children).find(text) != std::string::npos) found = true;
      } else {
        if (node->text.find(text) != std::string::npos) found = true;
      }
    }
  });
  return found;
}

inline std::string plain(const ir::Document& d) { return ir::plain_text(d); }

/// The plain text of the shared synthetic sample (headings and paragraphs
/// every fixture made from base/sample.* contains).
inline constexpr std::string_view sample_words[] = {
    "Quarterly Widget Report", "Lorem ipsum dolor sit amet", "Lists and Steps", "Alpha bullet", "Nested gamma",
    "Prepare the frame", "Inventory Table", "Sprocket", "café", "αβγδ", "汉字测试", "שלום עולם", "🙂🚀",
    "second link"};

inline std::size_t sample_words_found(const ir::Document& d) {
  const std::string text = plain(d);
  std::size_t n = 0;
  for (std::string_view w : sample_words) n += text.find(w) != std::string::npos ? 1 : 0;
  return n;
}

/// A ZIP package's member names.
inline std::vector<std::string> zip_names(ByteView bytes) {
  std::vector<std::string> out;
  auto zip = util::ZipReader::open(bytes);
  if (!zip) return out;
  for (const auto& e : zip->entries()) out.push_back(e.name);
  return out;
}

inline std::string zip_text(ByteView bytes, std::string_view name) {
  auto zip = util::ZipReader::open(bytes);
  if (!zip) return {};
  auto data = zip->read(name);
  return data ? std::string(as_chars(*data)) : std::string();
}

/// A document exercising every block and inline type the writers handle.
inline ir::Document rich_document() {
  using namespace ir::build;
  ir::Document doc;
  doc.metadata.title = "Rich Sample";
  doc.metadata.author = "Somebody Example";
  doc.metadata.language = "en-GB";
  doc.blocks.push_back(heading(1, "Main Title"));
  doc.blocks.push_back(paragraph({text("Plain, "), strong("bold"), text(", "), emphasis("italic"), text(", "),
                                  underline({text("underlined")}), text(", "), strikethrough({text("struck")}), text(", "),
                                  code("mono_code"), text(", x"), superscript({text("2")}), text(" and H"),
                                  subscript({text("2")}), text("O. "), hyperlink("https://example.com/a", "a link"),
                                  text(".")}));
  ir::Link note;
  note.url = "#fn-1";
  note.note = "1";
  note.children.push_back(text("1"));
  doc.blocks.push_back(paragraph({text("Noted sentence."), note}));
  doc.footnotes.push_back(ir::Footnote{"1", {paragraph("The footnote body.")}});
  doc.blocks.push_back(heading(2, "Lists"));
  ir::List nested = ir::List{true, 1, {{paragraph("Inner one")}, {paragraph("Inner two")}}};
  doc.blocks.push_back(list(false, {{paragraph("Bullet one")}, {paragraph("Bullet two"), nested}, {paragraph("Bullet three")}}));
  doc.blocks.push_back(list(true, {{paragraph("Step one")}, {paragraph("Step two")}}, 1));
  doc.blocks.push_back(heading(2, "Table"));
  ir::TableBlock tbl;
  tbl.rows.push_back(row({"Name", "Count", "Price"}, true));
  tbl.rows.push_back(row({"Gear", "16", "0.75"}));
  ir::TableRow spanned;
  spanned.cells.push_back(ir::TableCell{{ir::Text{"Spanning two"}}, 2, 1});
  spanned.cells.push_back(ir::TableCell{{ir::Text{"9.99"}}, 1, 1});
  tbl.rows.push_back(std::move(spanned));
  doc.blocks.push_back(std::move(tbl));
  doc.blocks.push_back(heading(2, "Code and Quote"));
  doc.blocks.push_back(code_block("for part in parts:\n    check(part)"));
  doc.blocks.push_back(block_quote({paragraph("A quoted thought.")}));
  doc.blocks.push_back(page_break());
  doc.blocks.push_back(heading(2, "Unicode"));
  doc.blocks.push_back(paragraph("café αβγδ 汉字测试 שלום 🙂🚀"));
  return doc;
}

}  // namespace cvt::test::office
