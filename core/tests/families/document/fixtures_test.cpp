// Every reader against the files pandoc produced from the one synthetic source
// document (core/tests/data/document/sample/sample.md): structure and text.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/document/doc_test_support.hpp"

using namespace cvt;
using namespace cvt::test::doc;

namespace {

enum Expect : unsigned {
  outline_ok = 1u << 0,     // the five section headings with their relative levels
  nested_list = 1u << 1,    // bullet list of 3 with a nested list of 2
  ordered_list = 1u << 2,   // ordered list of 3
  table_shape = 1u << 3,    // 4 rows x 3 columns, cell texts
  table_header = 1u << 4,   // first row is a header row
  code_python = 1u << 5,    // a code block tagged python
  code_text = 1u << 6,      // a code block with the function
  footnote = 1u << 7,       // one footnote with its text
  links = 1u << 8,          // the two links
  quote = 1u << 9,          // the block quote
  picture = 1u << 10,       // the picture
  all = 0xFFFFu,
};

struct Case {
  const char* file;
  const char* format;
  double min_similarity;
  unsigned expect;
};

// Thresholds: the reference plain text includes a 40-dash rule; formats without
// rules (DocBook, JATS) lose those characters. BBCode and man pages spell
// superscripts and notes as plain text ("x2", "[1]", a NOTES section).
const Case cases[] = {
    {"pandoc/sample.html", "html", 0.97, all},
    {"pandoc/sample.gfm.md", "md", 0.97, all},
    // pandoc's own Markdown uses its "simple table" syntax, which is not part of the supported flavours.
    {"pandoc/sample.pandoc.md", "md", 0.95, all & ~unsigned{table_shape | table_header}},
    {"pandoc/sample.commonmark.md", "md", 0.95, all & ~unsigned{footnote}},
    {"pandoc/sample.rst", "rst", 0.97, all},
    {"pandoc/sample.adoc", "adoc", 0.97, all},
    {"pandoc/sample.org", "org", 0.97, all},
    {"pandoc/sample.wiki", "wiki", 0.97, all},
    {"pandoc/sample.dokuwiki", "dokuwiki", 0.96, all},
    {"pandoc/sample.textile", "textile", 0.95, all},
    {"pandoc/sample.jira", "jira", 0.95, all & ~unsigned{footnote}},
    {"pandoc/sample.typ", "typst", 0.97, all},
    {"pandoc/sample.dj", "djot", 0.96, all},
    {"pandoc/sample.t2t", "t2t", 0.93, all & ~unsigned{table_header | code_python | footnote | picture | quote}},
    {"pandoc/sample.bbcode", "bbcode", 0.90, all & ~unsigned{footnote}},
    {"pandoc/sample.tex", "tex", 0.97, all & ~unsigned{code_python}},
    {"pandoc/sample.1", "man", 0.88, all & ~unsigned{code_python | footnote | picture | links}},
    {"pandoc/sample.dbk", "docbook", 0.95, all},
    {"pandoc/sample.fb2", "fb2", 0.93, all & ~unsigned{nested_list | ordered_list | code_python}},
    {"pandoc/sample.jats.xml", "jats", 0.95, all},
    {"pandoc/sample.tei.xml", "tei", 0.97, all},
    {"pandoc/sample.ipynb", "ipynb", 0.90, all & ~unsigned{footnote | table_shape | table_header}},
};

void check_case(const Case& c) {
  INFO("fixture ", c.file, " as ", c.format);
  const auto doc = test::read_as<ir::Document>(c.format, test::fixture(std::string("document/") + c.file));
  CHECK_OK(doc);
  if (!doc) return;
  const double similarity = test::text_similarity(*doc, sample());
  CAPTURE(similarity);
  CHECK(similarity >= c.min_similarity);
  if (c.expect & outline_ok) {
    const std::vector<std::string> headings = heading_texts(*doc);
    std::vector<std::string> expected = {"Lorem Ipsum Overview", "Lists and Structure", "Table Section", "Code and Quotes",
                                         "Unicode Corner"};
    std::vector<std::string> found;
    for (const std::string& h : headings) {
      if (std::ranges::find(expected, h) != expected.end()) found.push_back(h);
    }
    CHECK(found == expected);
    // Relative levels: the overview above the subsections, Unicode Corner deepest.
    const auto outline_entries = document::heading_outline(*doc);
    int overview = 0;
    int lists_level = 0;
    int corner = 0;
    for (const auto& e : outline_entries) {
      if (e.text == "Lorem Ipsum Overview") overview = e.level;
      if (e.text == "Lists and Structure") lists_level = e.level;
      if (e.text == "Unicode Corner") corner = e.level;
    }
    if (std::string_view(c.format) != "man") {
      CHECK(overview < lists_level);
      CHECK(lists_level < corner);
    }
  }
  const auto all_lists = lists(*doc);
  if (c.expect & nested_list) {
    bool found = false;
    for (const ir::List* l : all_lists) {
      if (l->ordered || l->definition || l->items.size() != 3) continue;
      for (const ir::Blocks& item : l->items) {
        for (const ir::Block& b : item) {
          if (const auto* nested = b.get_if<ir::List>(); nested != nullptr && nested->items.size() == 2) found = true;
        }
      }
    }
    CHECK(found);
  }
  if (c.expect & ordered_list) {
    CHECK(std::ranges::any_of(all_lists, [](const ir::List* l) { return l->ordered && l->items.size() == 3; }));
  }
  if (c.expect & table_shape) {
    const ir::TableBlock* t = first_table(*doc);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    CHECK(t->rows.size() == 4);
    const auto normalized = document::normalize_table(*t);
    CHECK(normalized.rows.front().cells.size() == 3);
    CHECK(cell_text(*t, 0, 0) == "Name");
    CHECK(cell_text(*t, 0, 2) == "Price");
    CHECK(cell_text(*t, 2, 0) == "Banana");
    CHECK(cell_text(*t, 3, 1) == "100");
    CHECK(cell_text(*t, 3, 2) == "9.99");
    if (c.expect & table_header) CHECK(t->rows.front().header);
  }
  const auto codes = code_blocks(*doc);
  if (c.expect & code_text) {
    CHECK(std::ranges::any_of(codes, [](const ir::CodeBlock* b) {
      return b->code.find("def greet(name):") != std::string::npos && b->code.find("return \"Hello, \" + name") != std::string::npos;
    }));
  }
  if (c.expect & code_python) CHECK(std::ranges::any_of(codes, [](const ir::CodeBlock* b) { return b->language == "python"; }));
  if (c.expect & footnote) {
    CHECK(doc->footnotes.size() == 1);
    if (doc->footnotes.size() != 1) return;
    CHECK(util::trim(ir::plain_text(doc->footnotes[0].blocks)) == "This is the footnote text.");
    CHECK(count_inlines(*doc, [](const ir::Inline& i) { return i.is<ir::Link>() && !i.get_if<ir::Link>()->note.empty(); }) == 1);
  }
  if (c.expect & links) {
    CHECK(has_url(*doc, "https://example.com/docs"));
    CHECK(has_url(*doc, "https://example.org/page"));
  }
  if (c.expect & quote) CHECK(count_blocks(*doc, [](const ir::Block& b) { return b.is<ir::BlockQuote>(); }) >= 1);
  if (c.expect & picture) {
    const int pictures = count_blocks(*doc, [](const ir::Block& b) { return b.is<ir::ImageBlock>(); }) +
                         count_inlines(*doc, [](const ir::Inline& i) { return i.is<ir::InlineImage>(); });
    CHECK(pictures >= 1);
  }
}

}  // namespace

TEST_CASE("document/fixtures: pandoc renditions of the sample read with the expected structure") {
  REQUIRE(!sample().blocks.empty());
  CHECK(sample().metadata.title == "Sample Document");
  for (const Case& c : cases) check_case(c);
}

TEST_CASE("document/fixtures: the reference document itself") {
  const ir::Document& d = sample();
  CHECK(outline(d) == std::vector<std::string>{"1:Lorem Ipsum Overview", "2:Lists and Structure", "2:Table Section",
                                               "2:Code and Quotes", "3:Unicode Corner"});
  REQUIRE(d.footnotes.size() == 1);
  const ir::TableBlock* t = first_table(d);
  REQUIRE(t != nullptr);
  CHECK(t->alignments == std::vector<ir::Alignment>{ir::Alignment::Left, ir::Alignment::Right, ir::Alignment::Center});
}

TEST_CASE("document/fixtures: hand-written Creole, POD and mdoc (verified with pandoc, podchecker, mandoc)") {
  SUBCASE("creole") {
    const auto doc = test::read_as<ir::Document>("creole", test::fixture("document/handwritten/sample.creole"));
    REQUIRE_OK(doc);
    CHECK(heading_texts(*doc) == std::vector<std::string>{"Lorem Ipsum Overview", "Lists and Structure", "Table Section",
                                                          "Code and Quotes", "Unicode Corner"});
    const ir::TableBlock* t = first_table(*doc);
    REQUIRE(t != nullptr);
    CHECK(t->rows.size() == 4);
    CHECK(t->rows[0].header);
    CHECK(cell_text(*t, 3, 2) == "9.99");
    CHECK(has_url(*doc, "https://example.com/docs"));
    CHECK(code_blocks(*doc).size() == 1);
    CHECK(test::text_similarity(ir::plain_text(*doc), "Final paragraph") < 1.0);
    const std::string text = ir::plain_text(*doc);
    CHECK(text.find("an escaped ** star pair.") != std::string::npos);
    CHECK(count_inlines(*doc, [](const ir::Inline& i) { return i.is<ir::LineBreak>(); }) == 1);
  }
  SUBCASE("pod") {
    const auto doc = test::read_as<ir::Document>("pod", test::fixture("document/handwritten/sample.pod"));
    REQUIRE_OK(doc);
    CHECK(doc->metadata.title == "sample");
    CHECK(outline(*doc) == std::vector<std::string>{"1:NAME", "1:Lorem Ipsum Overview", "2:Lists and Structure",
                                                    "2:Code and Quotes", "3:Unicode Corner"});
    const auto all = lists(*doc);
    CHECK(std::ranges::any_of(all, [](const ir::List* l) { return l->ordered && l->items.size() == 3; }));
    CHECK(std::ranges::any_of(all, [](const ir::List* l) { return l->definition && l->items.size() == 1; }));
    CHECK(has_url(*doc, "https://example.com/docs"));
    const auto codes = code_blocks(*doc);
    REQUIRE(codes.size() == 1);
    CHECK(codes[0]->code == "def greet(name):\n    return \"Hello, \" + name");
    const std::string text = ir::plain_text(*doc);
    CHECK(text.find("1 < 2 > 0 and $a <=> $b.") != std::string::npos);
  }
  SUBCASE("mdoc") {
    const auto doc = test::read_as<ir::Document>("man", test::fixture("document/handwritten/sample.mdoc"));
    REQUIRE_OK(doc);
    CHECK(doc->metadata.title == "SAMPLE");
    CHECK(heading_texts(*doc) == std::vector<std::string>{"NAME", "SYNOPSIS", "DESCRIPTION", "Lists", "EXAMPLES", "SEE ALSO"});
    const auto all = lists(*doc);
    CHECK(std::ranges::any_of(all, [](const ir::List* l) { return l->definition && l->items.size() == 2; }));
    CHECK(std::ranges::any_of(all, [](const ir::List* l) { return l->ordered && l->items.size() == 3; }));
    const auto codes = code_blocks(*doc);
    REQUIRE(codes.size() == 1);
    CHECK(codes[0]->code == "sample -v input.txt");
    CHECK(has_url(*doc, "https://example.com/docs"));
    CHECK(ir::plain_text(*doc).find("cat(1)") != std::string::npos);
  }
}
