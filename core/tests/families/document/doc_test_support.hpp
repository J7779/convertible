// Helpers for the document family tests: structure queries on ir::Document.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "families/document/document_api.hpp"
#include "ir/document.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

namespace cvt::test::doc {

inline void walk_blocks(const ir::Blocks& blocks, const std::function<void(const ir::Block&, int)>& visit, int depth = 0) {
  for (const ir::Block& b : blocks) {
    visit(b, depth);
    if (const auto* l = b.get_if<ir::List>()) {
      for (const ir::Blocks& item : l->items) walk_blocks(item, visit, depth + 1);
    } else if (const auto* q = b.get_if<ir::BlockQuote>()) {
      walk_blocks(q->blocks, visit, depth + 1);
    }
  }
}

inline void walk_inlines(const ir::Inlines& inlines, const std::function<void(const ir::Inline&)>& visit) {
  for (const ir::Inline& i : inlines) {
    visit(i);
    std::visit([&](const auto& node) {
      if constexpr (requires { node.children; }) walk_inlines(node.children, visit);
    }, i.node);
  }
}

inline void all_inlines(const ir::Document& d, const std::function<void(const ir::Inline&)>& visit) {
  const auto on_block = [&](const ir::Block& b, int) {
    if (const auto* p = b.get_if<ir::Paragraph>()) walk_inlines(p->content, visit);
    if (const auto* h = b.get_if<ir::Heading>()) walk_inlines(h->content, visit);
    if (const auto* t = b.get_if<ir::TableBlock>()) {
      for (const auto& row : t->rows) {
        for (const auto& cell : row.cells) walk_inlines(cell.content, visit);
      }
    }
  };
  walk_blocks(d.blocks, on_block);
  for (const ir::Footnote& n : d.footnotes) walk_blocks(n.blocks, on_block);
}

/// Heading texts in order ("level:text").
inline std::vector<std::string> outline(const ir::Document& d) {
  std::vector<std::string> out;
  for (const auto& e : document::heading_outline(d)) out.push_back(std::to_string(e.level) + ":" + e.text);
  return out;
}

inline std::vector<std::string> heading_texts(const ir::Document& d) {
  std::vector<std::string> out;
  for (const auto& e : document::heading_outline(d)) out.push_back(e.text);
  return out;
}

inline const ir::TableBlock* first_table(const ir::Document& d) {
  const ir::TableBlock* found = nullptr;
  walk_blocks(d.blocks, [&](const ir::Block& b, int) {
    if (found == nullptr) found = b.get_if<ir::TableBlock>();
  });
  return found;
}

inline std::vector<const ir::List*> lists(const ir::Document& d) {
  std::vector<const ir::List*> out;
  walk_blocks(d.blocks, [&](const ir::Block& b, int) {
    if (const auto* l = b.get_if<ir::List>()) out.push_back(l);
  });
  return out;
}

inline std::vector<const ir::CodeBlock*> code_blocks(const ir::Document& d) {
  std::vector<const ir::CodeBlock*> out;
  walk_blocks(d.blocks, [&](const ir::Block& b, int) {
    if (const auto* c = b.get_if<ir::CodeBlock>()) out.push_back(c);
  });
  return out;
}

inline std::vector<std::string> link_urls(const ir::Document& d) {
  std::vector<std::string> out;
  all_inlines(d, [&](const ir::Inline& i) {
    if (const auto* l = i.get_if<ir::Link>(); l != nullptr && l->note.empty()) out.push_back(l->url);
  });
  return out;
}

inline bool has_url(const ir::Document& d, std::string_view url) {
  for (const std::string& u : link_urls(d)) {
    if (u == url) return true;
  }
  return false;
}

inline int count_blocks(const ir::Document& d, const std::function<bool(const ir::Block&)>& predicate) {
  int n = 0;
  walk_blocks(d.blocks, [&](const ir::Block& b, int) { n += predicate(b) ? 1 : 0; });
  return n;
}

inline int count_inlines(const ir::Document& d, const std::function<bool(const ir::Inline&)>& predicate) {
  int n = 0;
  all_inlines(d, [&](const ir::Inline& i) { n += predicate(i) ? 1 : 0; });
  return n;
}

inline std::string cell_text(const ir::TableBlock& t, std::size_t r, std::size_t c) {
  if (r >= t.rows.size() || c >= t.rows[r].cells.size()) return "<missing>";
  return std::string(util::trim(ir::plain_text(t.rows[r].cells[c].content)));
}

/// The reference: the synthetic source document.
inline const ir::Document& sample() {
  static const ir::Document doc = [] {
    auto parsed = document::parse_markdown(fixture_text("document/sample/sample.md"));
    return parsed ? std::move(*parsed) : ir::Document{};
  }();
  return doc;
}

/// A rich document exercising every construct, with hostile characters.
inline ir::Document rich_document() {
  using namespace ir::build;
  ir::Document d;
  d.metadata.title = "Rich <Test> & \"Document\"";
  d.metadata.language = "en";
  d.blocks.push_back(heading(1, "Heading One"));
  d.blocks.push_back(paragraph({text("Plain, "), strong("bold"), text(" and "), emphasis("italic"), text(" with "),
                                code("x < y && z"), text(" and "), hyperlink("https://example.com/a?b=1&c=2", "a link"),
                                text(".")}));
  d.blocks.push_back(paragraph("Hostile: <tag> & \"quotes\" 'single' *stars* _under_ [brackets] {braces} `tick` #hash "
                               "|pipe ~tilde ^caret $dollar %percent @at !bang \\backslash = equals + plus"));
  d.blocks.push_back(paragraph("Unicode: café naïve 汉字 שלום مرحبا 🙂🚀 — “quotes”."));
  d.blocks.push_back(heading(2, "Lists"));
  ir::List bullets;
  bullets.items.push_back({paragraph("First item")});
  bullets.items.push_back({paragraph("Second item"), list(false, {{paragraph("Nested one")}, {paragraph("Nested two")}})});
  bullets.items.push_back({paragraph("Third item")});
  d.blocks.push_back(std::move(bullets));
  d.blocks.push_back(list(true, {{paragraph("Step one")}, {paragraph("Step two")}, {paragraph("Step three")}}));
  d.blocks.push_back(heading(2, "Table"));
  ir::TableBlock t;
  t.rows.push_back(row({"Name", "Qty", "Price"}, true));
  t.rows.push_back(row({"Apple", "3", "1.25"}));
  t.rows.push_back(row({"Banana", "12", "0.50"}));
  t.alignments = {ir::Alignment::Left, ir::Alignment::Right, ir::Alignment::Center};
  d.blocks.push_back(std::move(t));
  d.blocks.push_back(heading(2, "Code"));
  d.blocks.push_back(code_block("def f(x):\n    return x < 1 and x > 0  # <&>", "python"));
  d.blocks.push_back(block_quote({paragraph("Quoted text here.")}));
  d.blocks.push_back(paragraph({text("A note"), ir::Link{"#fn-1", {}, {ir::Text{"1"}}, "1"}, text(" here.")}));
  d.footnotes.push_back(ir::Footnote{"1", {paragraph("The note body.")}});
  d.blocks.push_back(horizontal_rule());
  d.blocks.push_back(paragraph("Last paragraph."));
  return d;
}

}  // namespace cvt::test::doc
