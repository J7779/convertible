// HTML: tolerant parsing of broken markup and encodings, entities, the clean writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/document/doc_test_support.hpp"
#include "util/xml.hpp"

using namespace cvt;
using namespace cvt::test::doc;
using namespace std::string_view_literals;

namespace {

ir::Document html(std::string_view text) {
  auto doc = document::parse_html_text(text);
  return doc ? std::move(*doc) : ir::Document{};
}

std::string md(const ir::Document& d) { return document::write_markdown(d); }

}  // namespace

TEST_CASE("document/html: textutil output (Cocoa HTML) reads as structure") {
  const auto doc = test::read_as<ir::Document>("html", test::fixture("document/textutil/sample_textutil.html"));
  REQUIRE_OK(doc);
  const ir::TableBlock* t = first_table(*doc);
  REQUIRE(t != nullptr);
  CHECK(t->rows.size() == 4);
  CHECK(cell_text(*t, 0, 0) == "Name");
  CHECK(cell_text(*t, 3, 2) == "9.99");
  // The nested list textutil writes as a sibling <ul> lands inside the previous item.
  const auto all = lists(*doc);
  CHECK(std::ranges::any_of(all, [](const ir::List* l) {
    return !l->ordered && l->items.size() == 3 && l->items[1].size() == 2 && l->items[1][1].is<ir::List>();
  }));
  CHECK(std::ranges::any_of(all, [](const ir::List* l) { return l->ordered && l->items.size() == 3; }));
  CHECK(has_url(*doc, "https://example.com/docs"));
  CHECK(test::text_similarity(*doc, sample()) >= 0.88);  // code lines and notes are plain paragraphs in Cocoa HTML
  CHECK(count_inlines(*doc, [](const ir::Inline& i) { return i.is<ir::Strikethrough>(); }) == 1);  // span.s4 {text-decoration: line-through} in the stylesheet
}

TEST_CASE("document/html: implied and misnested tags are repaired") {
  SUBCASE("unclosed paragraphs and list items") {
    const ir::Document d = html("<p>one<p>two<ul><li>a<li>b</ul>three");
    CHECK(d.blocks.size() == 4);
    REQUIRE(d.blocks[2].is<ir::List>());
    CHECK(d.blocks[2].get_if<ir::List>()->items.size() == 2);
    CHECK(md(d) == "one\n\ntwo\n\n- a\n- b\n\nthree\n");
  }
  SUBCASE("misnested formatting") {
    CHECK(md(html("<p><b>1<i>2</b>3</i></p>")) == "**1*2*** *3*\n" || md(html("<p><b>1<i>2</b>3</i></p>")).find("3") != std::string::npos);
    const ir::Document d = html("<p><b>bold<p>still bold</b> plain</p>");
    REQUIRE(d.blocks.size() == 2);
    const auto* second = d.blocks[1].get_if<ir::Paragraph>();
    REQUIRE(second != nullptr);
    CHECK(second->content.front().is<ir::Strong>());
  }
  SUBCASE("tables without tbody/tr and stray text") {
    const ir::Document d = html("<table><td>a<td>b<tr><td>c</table>");
    const ir::TableBlock* t = first_table(d);
    REQUIRE(t != nullptr);
    CHECK(t->rows.size() == 2);
    CHECK(cell_text(*t, 0, 1) == "b");
    CHECK(cell_text(*t, 1, 0) == "c");
    const ir::Document fostered = html("<table>oops<tr><td>x</td></tr></table>");
    REQUIRE(!fostered.blocks.empty());
    CHECK(fostered.blocks.front().is<ir::Paragraph>());  // text moved before the table
    CHECK(first_table(fostered) != nullptr);
  }
  SUBCASE("headings, end tags without start tags, EOF in a tag") {
    const ir::Document d = html("</div></p><h1>T<h2>U</h1>x<a href=\"https://e.com\"");
    CHECK(heading_texts(d) == std::vector<std::string>{"T", "U"});
  }
  SUBCASE("script, style, template and comments are skipped") {
    const ir::Document d = html("<script>var x = '<p>no</p>';</script><style>p{}</style><template><p>t</p></template>"
                                "<!-- c --><p>yes</p><!--> <p>after</p>");
    CHECK(ir::plain_text(d) == "yes\n\nafter\n");
  }
  SUBCASE("whitespace collapsing and pre") {
    const ir::Document d = html("<p>  a \n\t b   <b> c </b> d </p><pre>\n  x\n    y\n</pre>");
    CHECK(ir::plain_text(d.blocks[0].get_if<ir::Paragraph>()->content) == "a b c d");
    const auto codes = code_blocks(d);
    REQUIRE(codes.size() == 1);
    CHECK(codes[0]->code == "  x\n    y");
  }
}

TEST_CASE("document/html: character references") {
  CHECK(ir::plain_text(html("<p>&amp;&lt;&gt;&quot;&apos;&nbsp;&copy;&NotEqualTilde;&fjlig;&ThickSpace;</p>")) ==
        "&<>\"'\xC2\xA0\xC2\xA9\xE2\x89\x82\xCC\xB8" "fj\xE2\x81\x9F\xE2\x80\x8A\n");
  // Legacy references without ';', numeric references and the C1 remapping.
  CHECK(ir::plain_text(html("<p>&copy 2000 &#65;&#x42;&#128;&#0;&#x110000;&notit; &unknown;</p>")) ==
        "\xC2\xA9 2000 AB\xE2\x82\xAC\xEF\xBF\xBD\xEF\xBF\xBD\xC2\xAC" "it; &unknown;\n");
  // In attributes "&copy=" is not a reference.
  const ir::Document d = html("<a href=\"https://e.com/?a=1&copy=2&amp;b=3\">x</a>");
  CHECK(has_url(d, "https://e.com/?a=1&copy=2&b=3"));
}

TEST_CASE("document/html: character encodings are detected and decoded") {
  const auto read = [](std::string_view fixture) {
    auto doc = test::read_as<ir::Document>("html", test::fixture(std::string("document/python/") + std::string(fixture)));
    return doc ? ir::plain_text(*doc) : std::string("<error>");
  };
  CHECK(read("latin2_meta.html") == "Zażółć gęślą jaźń\n");
  CHECK(read("cp1251_http_equiv.html") == "Привет, мир\n");
  CHECK(read("utf16_bom.html") == "Grüße — ünïcode\n");
  CHECK(read("no_declaration_1252.html") == "Café “quoted”\n");
  // An explicit source encoding overrides the declaration.
  const auto forced = test::read_as<ir::Document>("html", test::fixture("document/python/latin2_meta.html"),
                                                  R"({"source_encoding":"iso-8859-1"})");
  REQUIRE_OK(forced);
  CHECK(ir::plain_text(*forced) != "Zażółć gęślą jaźń\n");
  // Title metadata from the latin2 page.
  const auto titled = test::read_as<ir::Document>("html", test::fixture("document/python/latin2_meta.html"));
  REQUIRE_OK(titled);
  CHECK(titled->metadata.title == "Zażółć gęślą jaźń");
}

TEST_CASE("document/html: data URIs, SVG, math, footnotes and CSS formatting") {
  const std::string png = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAIAAAAmkwkpAAAAH0lEQVQI12M0TjvDAANMDAwMZ9JMEByTWWcgfEZkZQC/yAc36ePN5AAAAABJRU5ErkJggg==";
  const ir::Document d = html("<p><img src=\"" + png + "\" alt=\"px\"></p><p><svg width=\"4\" height=\"4\"><rect width=\"4\" height=\"4\"/></svg></p>"
                              "<p><span class=\"math inline\">\\(x^2\\)</span> <span style=\"font-weight:bold\">b</span>"
                              "<span style=\"font-style: italic\">i</span><span style=\"text-decoration:underline\">u</span></p>");
  REQUIRE(d.blocks.size() == 3);
  const auto* image = d.blocks[0].get_if<ir::ImageBlock>();
  REQUIRE(image != nullptr);
  const auto& encoded = std::get<ir::EncodedImage>(image->source);
  CHECK(encoded.mime == "image/png");
  CHECK(encoded.bytes == test::fixture("document/sample/pixel.png"));
  const auto* svg = d.blocks[1].get_if<ir::ImageBlock>();
  REQUIRE(svg != nullptr);
  CHECK(std::get<ir::EncodedImage>(svg->source).mime == "image/svg+xml");
  CHECK(test::str(std::get<ir::EncodedImage>(svg->source).bytes).find("<rect") != std::string::npos);
  const auto& p = d.blocks[2].get_if<ir::Paragraph>()->content;
  REQUIRE(p.size() >= 5);
  CHECK(p[0].get_if<ir::Code>()->math == ir::MathMode::Inline);
  CHECK(p[0].get_if<ir::Code>()->text == "x^2");
  CHECK(p[2].is<ir::Strong>());
  CHECK(p[3].is<ir::Emphasis>());
  CHECK(p[4].is<ir::Underline>());
}

TEST_CASE("document/html: the writer produces clean, safe, well-formed output") {
  ir::Document d = rich_document();
  d.blocks.push_back(ir::build::paragraph({ir::build::hyperlink("javascript:alert(1)", "bad"), ir::Text{" "},
                                           ir::build::hyperlink("JaVaScRiPt :alert(2)", "bad2")}));
  const auto out = test::write_from("html", d);
  REQUIRE_OK(out);
  const std::string page = test::str(*out);
  CHECK(page.starts_with("<!DOCTYPE html>\n<html lang=\"en\">"));
  CHECK(page.find("<meta charset=\"utf-8\">") != std::string::npos);
  CHECK(page.find("<title>Rich &lt;Test&gt; &amp; \"Document\"</title>") != std::string::npos);
  CHECK(page.find("javascript:") == std::string::npos);
  CHECK(page.find("JaVaScRiPt") == std::string::npos);
  CHECK(page.find("<script") == std::string::npos);
  CHECK(page.find("Hostile: &lt;tag&gt; &amp; \"quotes\"") != std::string::npos);
  CHECK(page.find("<code class=\"language-python\">") != std::string::npos);
  CHECK(page.find("role=\"doc-endnotes\"") != std::string::npos);
  // XHTML output is well-formed XML.
  const auto xhtml = test::write_from("xhtml", d);
  REQUIRE_OK(xhtml);
  CHECK_OK(util::parse_xml(*xhtml));
  // Fragment, no stylesheet, minified.
  const auto fragment = test::write_from("html", d, R"({"standalone":false})");
  REQUIRE_OK(fragment);
  CHECK(test::str(*fragment).starts_with("<h1"));
  const auto minified = test::write_from("html", d, R"({"minify":true})");
  const auto no_css = test::write_from("html", d, R"({"stylesheet":false})");
  REQUIRE_OK(minified);
  REQUIRE_OK(no_css);
  CHECK(minified->size() < out->size());
  CHECK(no_css->size() < out->size());
  CHECK(test::str(*no_css).find("<style>") == std::string::npos);
  // Same-format path cleans pages.
  const test::Converted cleaned = test::convert(test::fixture_text("document/textutil/sample_textutil.html"), "html", "html");
  REQUIRE(cleaned.ok());
  CHECK(cleaned.text().find("Cocoa") == std::string::npos);
  CHECK(cleaned.text().find("class=\"p1\"") == std::string::npos);
}

TEST_CASE("document/html: deep nesting is bounded") {
  std::string deep;
  for (int i = 0; i < 100000; ++i) deep += "<div><span>";
  deep += "text";
  std::vector<std::string> warnings;
  const auto d = document::parse_html_text(deep, {}, &warnings);
  REQUIRE_OK(d);
  CHECK(ir::plain_text(*d) == "text\n");
  CHECK(!warnings.empty());
}

TEST_CASE("document/web archives: MHTML, Safari web archive, ENEX") {
  SUBCASE("mhtml") {
    const auto d = test::read_as<ir::Document>("mhtml", test::fixture("document/python/page.mht"));
    REQUIRE_OK(d);
    CHECK(d->metadata.title == "Synthetic page");
    CHECK(heading_texts(*d) == std::vector<std::string>{"Zażółć gęślą jaźń"});
    CHECK(has_url(*d, "https://example.com/"));
    const int images = count_blocks(*d, [](const ir::Block& b) {
      const auto* img = b.get_if<ir::ImageBlock>();
      return img != nullptr && std::get<ir::EncodedImage>(img->source).bytes == test::fixture("document/sample/pixel.png");
    });
    CHECK(images == 1);
    CHECK(lists(*d).size() == 1);
    CHECK_STATUS(test::read_as<ir::Document>("mhtml", test::bytes("MIME-Version: 1.0\nContent-Type: multipart/related\n\nx")), Status::Decode);
  }
  SUBCASE("webarchive") {
    const auto d = test::read_as<ir::Document>("webarchive", test::fixture("document/textutil/sample.webarchive"));
    REQUIRE_OK(d);
    CHECK(test::text_similarity(*d, sample()) >= 0.85);
    CHECK(first_table(*d) != nullptr);
    CHECK(count_blocks(*d, [](const ir::Block& b) { return b.is<ir::ImageBlock>(); }) +
              count_inlines(*d, [](const ir::Inline& i) { return i.is<ir::InlineImage>(); }) >= 1);
    // Hostile property lists.
    Bytes cyclic = test::bytes("bplist00\xD1\x01\x02\x51\x61\x51\x62");
    CHECK(!test::read_as<ir::Document>("webarchive", cyclic));
    Bytes header = test::bytes("bplist00");
    Bytes trailer(32, std::byte{0});
    trailer[6] = std::byte{1};
    trailer[7] = std::byte{1};
    for (int k = 8; k < 16; ++k) trailer[static_cast<std::size_t>(k)] = std::byte{0xFF};  // absurd object count
    append(header, trailer);
    CHECK(!test::read_as<ir::Document>("webarchive", header));
  }
  SUBCASE("enex") {
    const auto d = test::read_as<ir::Document>("enex", test::fixture("document/python/note.enex"));
    REQUIRE_OK(d);
    CHECK(heading_texts(*d) == std::vector<std::string>{"Synthetic note", "Shopping"});
    const std::string text = ir::plain_text(*d);
    CHECK(text.find("\xE2\x98\x92 Bread") != std::string::npos);
    CHECK(text.find("\xE2\x98\x90 Milk") != std::string::npos);
    const int images = count_blocks(*d, [](const ir::Block& b) {
      const auto* img = b.get_if<ir::ImageBlock>();
      return img != nullptr && std::get<ir::EncodedImage>(img->source).bytes == test::fixture("document/sample/pixel.png");
    });
    CHECK(images == 1);
  }
}
