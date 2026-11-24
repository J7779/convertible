// Markdown: CommonMark block and inline rules (our own cases), GFM and the
// extended flavour, and a writer whose output parses back to the same document.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/document/doc_test_support.hpp"

using namespace cvt;
using namespace cvt::test::doc;

namespace {

ir::Document md(std::string_view text, document::MarkdownFlavor flavor = document::MarkdownFlavor::Extended) {
  document::MarkdownReadOptions options;
  options.flavor = flavor;
  auto doc = document::parse_markdown(text, options);
  return doc ? std::move(*doc) : ir::Document{};
}

// The document rendered as HTML fragment: a compact structural fingerprint.
std::string html(std::string_view text, document::MarkdownFlavor flavor = document::MarkdownFlavor::Extended) {
  document::HtmlWriteOptions options;
  options.standalone = false;
  options.minify = true;
  return document::write_html(md(text, flavor), options);
}

}  // namespace

TEST_CASE("document/markdown: CommonMark blocks") {
  CHECK(html("# A\n## B ##\n####### not") == "<h1>A</h1><h2>B</h2><p>####### not</p>");
  CHECK(html("Title\n=====\nSub\n---") == "<h1>Title</h1><h2>Sub</h2>");
  CHECK(html("a\nb\n\nc") == "<p>a\nb</p><p>c</p>");
  CHECK(html("    code\n      more\n\npara") == "<pre><code>code\n  more</code></pre><p>para</p>");
  CHECK(html("```rust extra\nfn x() {}\n```") == "<pre><code class=\"language-rust\">fn x() {}</code></pre>");
  CHECK(html("~~~~\n```\n~~~~") == "<pre><code>```</code></pre>");
  CHECK(html("> q1\ncontinued lazily\n> > deeper") ==
        "<blockquote><p>q1\ncontinued lazily</p><blockquote><p>deeper</p></blockquote></blockquote>");
  CHECK(html("* * *\n---\n___") == "<hr><hr><hr>");
  CHECK(html("- a\n- b\n\n- c") == "<ul><li><p>a</p></li><li><p>b</p></li><li><p>c</p></li></ul>");  // loose
  CHECK(html("1. a\n2. b\n3) c") == "<ol><li>a</li><li>b</li></ol><ol start=\"3\"><li>c</li></ol>");
  CHECK(html("7. seven\n8. eight") == "<ol start=\"7\"><li>seven</li><li>eight</li></ol>");
  CHECK(html("- a\n  - b\n    - c") == "<ul><li>a<ul><li>b<ul><li>c</li></ul></li></ul></li></ul>");
  CHECK(html("-\tone\n-\ttwo") == "<ul><li>one</li><li>two</li></ul>");
  // A list item can contain several blocks; indentation decides membership.
  CHECK(html("- para\n\n      indented code\n\n  > quote") ==
        "<ul><li><p>para</p><pre><code>indented code</code></pre><blockquote><p>quote</p></blockquote></li></ul>");
  // Reference definitions vanish and resolve, case-insensitively.
  CHECK(html("[Foo Bar]\n\n[foo  BAR]: /url \"T\"") == "<p><a href=\"/url\" title=\"T\">Foo Bar</a></p>");
  // Paragraph interruption rules.
  CHECK(html("text\n2. not a list") == "<p>text\n2. not a list</p>");
  CHECK(html("text\n1. a list") == "<p>text</p><ol><li>a list</li></ol>");
}

TEST_CASE("document/markdown: CommonMark inlines") {
  CHECK(html("*em* **strong** ***both*** _a_ __b__") ==
        "<p><em>em</em> <strong>strong</strong> <em><strong>both</strong></em> <em>a</em> <strong>b</strong></p>");
  CHECK(html("snake_case_name and 2*3*4") == "<p>snake_case_name and 2<em>3</em>4</p>");
  CHECK(html("*(*foo*)*") == "<p><em>(<em>foo</em>)</em></p>");
  CHECK(html("**foo*") == "<p>*<em>foo</em></p>");
  CHECK(html("`` a ` b ``") == "<p><code>a ` b</code></p>");
  CHECK(html("\\*not em\\* \\_ \\\\") == "<p>*not em* _ \\</p>");
  CHECK(html("&amp; &copy; &#35; &#x41; &nope;") == "<p>&amp; \xC2\xA9 # A &amp;nope;</p>");
  CHECK(html("a  \nb\\\nc") == "<p>a<br>b<br>c</p>");
  CHECK(html("<https://e.com/x> <a@b.co>") ==
        "<p><a href=\"https://e.com/x\">https://e.com/x</a> <a href=\"mailto:a@b.co\">a@b.co</a></p>");
  CHECK(html("[a [nested] link](</my url> 'T') ![img](i.png \"t\")") ==
        "<p><a href=\"/my url\" title=\"T\">a [nested] link</a> <img src=\"i.png\" alt=\"img\" title=\"t\"></p>");
  CHECK(html("[link [inner](u1)](u2)") == "<p>[link <a href=\"u1\">inner</a>](u2)</p>");
  CHECK(html("a <span class=\"x\">raw</span> b") == "<p>a <span class=\"x\">raw</span> b</p>");
  CHECK(html("H<sub>2</sub>O and <u>under</u>") == "<p>H<sub>2</sub>O and <u>under</u></p>");
}

TEST_CASE("document/markdown: GFM and extended syntax") {
  CHECK(html("| a | b |\n|:-|-:|\n| 1 | 2 \\| 3 |\n| x |") ==
        "<table><thead><tr><th style=\"text-align:left\">a</th><th style=\"text-align:right\">b</th></tr></thead>"
        "<tbody><tr><td style=\"text-align:left\">1</td><td style=\"text-align:right\">2 | 3</td></tr>"
        "<tr><td style=\"text-align:left\">x</td><td style=\"text-align:right\"></td></tr></tbody></table>");
  CHECK(html("~~gone~~ www.example.com/x. and https://a.b/c?d") ==
        "<p><del>gone</del> <a href=\"http://www.example.com/x\">www.example.com/x</a>. and "
        "<a href=\"https://a.b/c?d\">https://a.b/c?d</a></p>");
  const ir::Document tasks = md("- [ ] open\n- [x] done\n- plain");
  REQUIRE(!tasks.blocks.empty());
  CHECK(tasks.blocks[0].get_if<ir::List>()->tasks ==
        std::vector<ir::TaskState>{ir::TaskState::Open, ir::TaskState::Done, ir::TaskState::None});
  const ir::Document notes = md("Text[^n] and[^n].\n\n[^n]: Note *body*.\n    continued\n\n[^unused]: x");
  REQUIRE(notes.footnotes.size() == 1);
  CHECK(notes.footnotes[0].label == "1");
  CHECK(ir::plain_text(notes.footnotes[0].blocks) == "Note body.\ncontinued\n");
  const ir::Document fm = md("---\ntitle: \"My: Title\"\nauthor:\n  - A\n  - B\ntags: [x, y]\n---\n\n# Hi {#custom .cls}\n\nTerm\n: Def one\n\n$x^2$ and\n\n$$\ny = 1\n$$");
  CHECK(fm.metadata.title == "My: Title");
  CHECK(fm.metadata.author == "A; B");
  CHECK(fm.metadata.extra.at("tags") == "x, y");
  CHECK(fm.metadata.extra.at("front_matter_format") == "yaml");
  REQUIRE(fm.blocks.size() >= 4);
  CHECK(fm.blocks[0].attr.id == "custom");
  CHECK(fm.blocks[0].attr.has_class("cls"));
  CHECK(fm.blocks[1].get_if<ir::List>()->definition);
  CHECK(fm.blocks[2].get_if<ir::Paragraph>()->content[0].get_if<ir::Code>()->math == ir::MathMode::Inline);
  CHECK(fm.blocks[3].get_if<ir::CodeBlock>()->math == ir::MathMode::Display);
  CHECK(fm.blocks[3].get_if<ir::CodeBlock>()->code == "y = 1");
  // GitHub alerts become admonitions.
  const ir::Document alert = md("> [!WARNING]\n> Careful.");
  CHECK(alert.blocks[0].attr.has_class("warning"));
  // CommonMark flavour: no tables, no strikethrough.
  CHECK(html("~~x~~", document::MarkdownFlavor::CommonMark) == "<p>~~x~~</p>");
}

TEST_CASE("document/markdown: the writer round-trips and escapes") {
  const ir::Document rich = rich_document();
  for (const auto flavor : {document::MarkdownFlavor::Gfm, document::MarkdownFlavor::Extended}) {
    document::MarkdownWriteOptions options;
    options.flavor = flavor;
    const std::string text = document::write_markdown(rich, options);
    document::MarkdownReadOptions read;
    read.flavor = flavor;
    const auto back = document::parse_markdown(text, read);
    REQUIRE_OK(back);
    CHECK(test::text_similarity(*back, rich) == 1.0);
    CHECK(document::write_markdown(*back, options) == text);  // stable
  }
  // Text that looks like markup at line starts and inside paragraphs.
  ir::Document tricky;
  for (const char* line : {"# not a heading", "- not a list", "1. not a list", "> not a quote", "=====", "---",
                           "<div>not html</div>", "```", "    four spaces", "| a | b |", "[x]: /not-a-ref", "+ plus",
                           "*a* _b_ `c` [d](e) ![f](g) <h> &amp; \\ $m$ ~~s~~", "2) paren", ": colon"}) {
    tricky.blocks.push_back(ir::build::paragraph(line));
  }
  for (const int width : {0, 12}) {
    document::MarkdownWriteOptions options;
    options.flavor = document::MarkdownFlavor::Extended;
    options.wrap_width = width;
    const std::string text = document::write_markdown(tricky, options);
    const auto back = document::parse_markdown(text);
    REQUIRE_OK(back);
    INFO("markdown:\n", text);
    REQUIRE(back->blocks.size() == tricky.blocks.size());
    for (std::size_t i = 0; i < tricky.blocks.size(); ++i) {
      CHECK(back->blocks[i].is<ir::Paragraph>());
      CHECK(test::normalized_text(ir::plain_text(back->blocks[i].get_if<ir::Paragraph>()->content)) ==
            test::normalized_text(ir::plain_text(tricky.blocks[i].get_if<ir::Paragraph>()->content)));  // leading blanks cannot be expressed
    }
  }
}

TEST_CASE("document/markdown: writer options") {
  const ir::Document rich = rich_document();
  document::MarkdownWriteOptions options;
  options.setext_headings = true;
  CHECK(document::write_markdown(rich, options).find("Heading One\n===========") != std::string::npos);
  options = {};
  options.reference_links = true;
  const std::string refs = document::write_markdown(rich, options);
  CHECK(refs.find("[a link][1]") != std::string::npos);
  CHECK(refs.find("[1]: https://example.com/a?b=1&c=2") != std::string::npos);
  options = {};
  options.bullet = '*';
  CHECK(document::write_markdown(rich, options).find("* First item") != std::string::npos);
  options = {};
  options.flavor = document::MarkdownFlavor::CommonMark;
  const std::string cm = document::write_markdown(rich, options);
  CHECK(cm.find("<table>") != std::string::npos);  // no pipe tables in CommonMark
  // Through the engine: flavour option and the md -> md reflow.
  const test::Converted reflow = test::convert("A long line of words that wraps at the requested width.\n", "md", "md",
                                               R"({"wrap_width":20})");
  REQUIRE(reflow.ok());
  CHECK(reflow.text() == "A long line of words\nthat wraps at the\nrequested width.\n");
}

TEST_CASE("document/markdown: hostile input") {
  CHECK(test::fuzz_reader<ir::Document>("md", test::fixture("document/sample/sample.md")) > 0);
  std::string many_links;
  for (int i = 0; i < 3000; ++i) many_links += "[a](";
  CHECK_OK(document::parse_markdown(many_links));
  std::string emphasis;
  for (int i = 0; i < 3000; ++i) emphasis += "*a _b ";
  CHECK_OK(document::parse_markdown(emphasis));
  CHECK_OK(document::parse_markdown(std::string(100000, '>') + "x"));
  CHECK_OK(document::parse_markdown(std::string(50000, '-') + " x\n" + std::string(30000, ' ') + "- y"));
}
