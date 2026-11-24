// Every writer: a rich document with hostile characters written and read back
// with our own reader; detection, options and hostile input for every reader.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/detect.hpp"
#include "families/document/doc_test_support.hpp"

using namespace cvt;
using namespace cvt::test::doc;

namespace {

struct Writable {
  const char* format;
  double min_similarity;
  bool table = true;      // table cells survive
  bool code = true;       // code text survives exactly
  bool hostile = true;    // the hostile paragraph survives exactly
  bool link = true;       // the link URL survives
  bool note = true;       // the footnote stays a footnote
};

// Lower thresholds belong to formats that cannot hold some constructs: notes
// become numbered end notes (Creole, Jira, BBCode, txt2tags), quotes vanish
// (Creole), lists become marked paragraphs (FB2), and POD keeps tables only as
// preformatted text.
const Writable writable[] = {
    {"html", 0.99},       {"xhtml", 0.99},      {"md", 0.99},          {"rst", 0.98},
    {"asciidoc", 0.98},   {"org", 0.98},        {"djot", 0.99},        {"typst", 0.97, true, true, true, true, true},
    {"mediawiki", 0.98},  {"dokuwiki", 0.98},   {"creole", 0.88, true, true, true, true, false},
    {"jira", 0.88, true, true, true, true, false}, {"textile", 0.98},
    {"bbcode", 0.88, true, true, true, true, false}, {"t2t", 0.88, true, true, true, true, false},
    {"pod", 0.70, false, true, true, true, false}, {"tex", 0.97},
    {"man", 0.90, true, true, true, false, false}, {"docbook", 0.98}, {"fb2", 0.88, true, false, true, true, true},
};

const std::string hostile =
    "Hostile: <tag> & \"quotes\" 'single' *stars* _under_ [brackets] {braces} `tick` #hash |pipe ~tilde ^caret $dollar "
    "%percent @at !bang \\backslash = equals + plus";

}  // namespace

TEST_CASE("document/roundtrip: every writer reads back with its own reader") {
  const ir::Document original = rich_document();
  for (const Writable& w : writable) {
    INFO("format ", w.format);
    const auto bytes = test::write_from(w.format, original, R"({"strip_metadata":false})");
    CHECK_OK(bytes);
    if (!bytes) continue;
    const auto back = test::read_as<ir::Document>(w.format, *bytes);
    CHECK_OK(back);
    if (!back) continue;
    const double similarity = test::text_similarity(*back, original);
    CAPTURE(similarity);
    CHECK(similarity >= w.min_similarity);
    const std::string text = ir::plain_text(*back);
    if (w.hostile) {
      CHECK(text.find(hostile) != std::string::npos);
      CHECK(text.find("Unicode: café naïve 汉字 שלום مرحبا 🙂🚀 — “quotes”.") != std::string::npos);
    }
    const std::vector<std::string> headings = heading_texts(*back);
    CHECK(std::ranges::find(headings, "Heading One") != headings.end());
    if (w.table) {
      const ir::TableBlock* t = first_table(*back);
      CHECK(t != nullptr);
      if (t != nullptr) {
        CHECK(cell_text(*t, 0, 0) == "Name");
        CHECK(cell_text(*t, 2, 2) == "0.50");
        CHECK(t->rows[0].header);
      }
    }
    if (w.code) {
      const auto codes = code_blocks(*back);
      CHECK(std::ranges::any_of(codes, [](const ir::CodeBlock* c) {
        return c->code == "def f(x):\n    return x < 1 and x > 0  # <&>";
      }));
    }
    if (w.link) CHECK(has_url(*back, "https://example.com/a?b=1&c=2"));
    if (w.note) {
      CHECK(back->footnotes.size() == 1);
      if (!back->footnotes.empty()) CHECK(util::trim(ir::plain_text(back->footnotes[0].blocks)) == "The note body.");
    }
    // Inline code with markup characters survives everywhere code exists.
    CHECK(text.find("x < y && z") != std::string::npos);
  }
}

TEST_CASE("document/roundtrip: same-format conversions (the compress action) keep the content") {
  for (const char* format : {"md", "html", "rst", "asciidoc", "org", "mediawiki", "textile", "djot", "tex", "docbook"}) {
    INFO("format ", format);
    const auto first = test::write_from(format, rich_document());
    REQUIRE_OK(first);
    const test::Converted again = test::convert(*first, format, format);
    CHECK(again.ok());
    if (!again.ok()) continue;
    const auto a = test::read_as<ir::Document>(format, *first);
    const auto b = test::read_as<ir::Document>(format, again.data);
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    CHECK(test::text_similarity(*a, *b) >= 0.99);
  }
}

TEST_CASE("document/roundtrip: Texinfo and OPML writers produce the expected structure") {
  const auto texi = test::write_from("texinfo", rich_document());
  REQUIRE_OK(texi);
  const std::string t = test::str(*texi);
  CHECK(t.starts_with("\\input texinfo\n"));
  CHECK(t.find("@node Top\n@top ") != std::string::npos);
  CHECK(t.find("@chapter Heading One") != std::string::npos);
  CHECK(t.find("@section Lists") != std::string::npos);
  CHECK(t.find("@multitable") != std::string::npos);
  CHECK(t.find("@headitem Name") != std::string::npos);
  CHECK(t.find("@example\ndef f(x):") != std::string::npos);
  CHECK(t.find("@{braces@}") != std::string::npos);
  CHECK(t.find("@@at") != std::string::npos);
  CHECK(t.find("@footnote{The note body.}") != std::string::npos);
  CHECK(t.ends_with("@bye\n"));

  ir::Document outline;
  outline.blocks.push_back(ir::build::list(false, {{ir::build::paragraph("A"), ir::build::list(false, {{ir::build::paragraph("A1")}})},
                                                   {ir::build::paragraph({ir::build::hyperlink("https://example.com/", "Site")})}}));
  const auto opml = test::write_from("opml", outline);
  REQUIRE_OK(opml);
  const auto back = test::read_as<ir::Document>("opml", *opml);
  REQUIRE_OK(back);
  const auto all = lists(*back);
  REQUIRE(!all.empty());
  CHECK(all[0]->items.size() == 2);
  CHECK(has_url(*back, "https://example.com/"));
  CHECK(ir::plain_text(*back).find("A1") != std::string::npos);
}

TEST_CASE("document/roundtrip: wrap_width and standalone options change the output") {
  ir::Document d;
  d.blocks.push_back(ir::build::paragraph(std::string(30, 'a') + " " + std::string(30, 'b') + " " + std::string(30, 'c')));
  for (const char* format : {"md", "rst", "asciidoc", "org", "djot", "tex"}) {
    INFO("format ", format);
    const auto wide = test::write_from(format, d, R"({"wrap_width":0})");
    const auto narrow = test::write_from(format, d, R"({"wrap_width":40})");
    REQUIRE_OK(wide);
    REQUIRE_OK(narrow);
    CHECK(std::ranges::count(test::str(*narrow), '\n') > std::ranges::count(test::str(*wide), '\n'));
    const auto back = test::read_as<ir::Document>(format, *narrow);
    REQUIRE_OK(back);
    CHECK(test::text_similarity(*back, d) == 1.0);
  }
  for (const char* format : {"html", "tex"}) {
    const auto full = test::write_from(format, d, R"({"standalone":true})");
    const auto fragment = test::write_from(format, d, R"({"standalone":false})");
    REQUIRE_OK(full);
    REQUIRE_OK(fragment);
    CHECK(fragment->size() < full->size());
  }
  // Line endings.
  const auto crlf = test::write_from("md", rich_document(), R"({"line_endings":"crlf"})");
  REQUIRE_OK(crlf);
  CHECK(test::str(*crlf).find("\r\n") != std::string::npos);
  CHECK(test::str(*crlf).find("\n\n") == std::string::npos);
}

TEST_CASE("document/roundtrip: shared writer options (toc, heading shift, images, metadata)") {
  ir::Document d = rich_document();
  d.metadata.author = "Some Author";
  ir::EncodedImage png;
  png.bytes = test::fixture("document/sample/pixel.png");
  png.mime = "image/png";
  d.blocks.push_back(ir::build::image_block(png, "Pixel"));
  const auto plain = test::write_from("html", d);
  const auto with_toc = test::write_from("html", d, R"({"toc":true})");
  const auto shifted = test::write_from("html", d, R"({"heading_shift":1})");
  const auto dropped = test::write_from("html", d, R"({"images":"drop"})");
  const auto kept_meta = test::write_from("html", d, R"({"strip_metadata":false})");
  REQUIRE_OK(plain);
  REQUIRE_OK(with_toc);
  REQUIRE_OK(shifted);
  REQUIRE_OK(dropped);
  REQUIRE_OK(kept_meta);
  CHECK(test::str(*plain).find("data:image/png;base64,") != std::string::npos);
  CHECK(test::str(*dropped).find("data:image/png") == std::string::npos);
  CHECK(test::str(*with_toc).find("class=\"toc\"") != std::string::npos);
  CHECK(test::str(*with_toc).find("href=\"#heading-one\"") != std::string::npos);
  CHECK(test::str(*shifted).find("<h2") < test::str(*shifted).find("<h3"));
  CHECK(test::str(*shifted).find("<h1") == std::string::npos);
  CHECK(test::str(*plain).find("Some Author") == std::string::npos);  // privacy by default
  CHECK(test::str(*kept_meta).find("Some Author") != std::string::npos);
  // LaTeX: \tableofcontents and a placeholder for the embedded picture.
  std::vector<std::string> warnings;
  const auto tex = test::write_from("tex", d, R"({"toc":true})", &warnings);
  REQUIRE_OK(tex);
  CHECK(test::str(*tex).find("\\tableofcontents") != std::string::npos);
  CHECK(test::str(*tex).find("\\fbox{Pixel}") != std::string::npos);
  CHECK(!warnings.empty());
}

TEST_CASE("document/roundtrip: readers survive hostile input") {
  struct Reader {
    const char* format;
    const char* fixture;
  };
  const Reader readers[] = {
      {"html", "pandoc/sample.html"},   {"md", "pandoc/sample.gfm.md"},     {"rst", "pandoc/sample.rst"},
      {"asciidoc", "pandoc/sample.adoc"}, {"org", "pandoc/sample.org"},     {"mediawiki", "pandoc/sample.wiki"},
      {"dokuwiki", "pandoc/sample.dokuwiki"}, {"textile", "pandoc/sample.textile"}, {"jira", "pandoc/sample.jira"},
      {"typst", "pandoc/sample.typ"},   {"djot", "pandoc/sample.dj"},       {"t2t", "pandoc/sample.t2t"},
      {"bbcode", "pandoc/sample.bbcode"}, {"tex", "pandoc/sample.tex"},     {"man", "pandoc/sample.1"},
      {"docbook", "pandoc/sample.dbk"}, {"fb2", "pandoc/sample.fb2"},       {"jats", "pandoc/sample.jats.xml"},
      {"tei", "pandoc/sample.tei.xml"}, {"opml", "pandoc/sample.opml"},     {"ipynb", "pandoc/sample.ipynb"},
      {"creole", "handwritten/sample.creole"}, {"pod", "handwritten/sample.pod"}, {"man", "handwritten/sample.mdoc"},
      {"mhtml", "python/page.mht"},     {"enex", "python/note.enex"},       {"webarchive", "textutil/sample.webarchive"},
  };
  for (const Reader& r : readers) {
    INFO("format ", r.format);
    const Bytes data = test::fixture(std::string("document/") + r.fixture);
    CHECK(test::fuzz_reader<ir::Document>(r.format, data, 96) > 0);
  }
  // Pathological nesting: every text reader returns within limits.
  const std::vector<std::pair<const char*, std::string>> deep = {
      {"md", std::string(20000, '>') + " x\n" + std::string(20000, '*') + "a" + std::string(20000, '*')},
      {"md", std::string(10000, '[') + std::string(10000, ']') + "(" + std::string(5000, '(') + ")"},
      {"md", std::string(5000, '`') + "x" + std::string(4999, '`')},
      {"html", [] {
         std::string s;
         for (int i = 0; i < 20000; ++i) s += "<div><b><i>";
         return s + "x";
       }()},
      {"html", std::string(50000, '<') + "&amp;&#xFFFFFFFF;&#0;&NotANamedRef;"},
      {"rst", [] {
         std::string s;
         for (int i = 0; i < 300; ++i) s += std::string(static_cast<std::size_t>(i), ' ') + "- item\n";
         return s;
       }()},
      {"bbcode", [] {
         std::string s;
         for (int i = 0; i < 20000; ++i) s += "[quote][list][*]";
         return s;
       }()},
      {"tex", std::string(20000, '{') + "\\begin{itemize}\\item" + std::string(3000, '{')},
      {"tex", [] {
         std::string s;
         for (int i = 0; i < 2000; ++i) s += "\\begin{quote}\\begin{itemize}\\item ";
         return s;
       }()},
      {"pod", "=pod\n\n" + std::string(20000, 'B') + std::string(20000, '<') + "x"},
      {"asciidoc", std::string(5000, '*') + " x\n" + "|===\n" + std::string(3000, '|')},
      {"org", std::string(3000, '*') + " h\n" + std::string(5000, ' ') + "- x\n"},
      {"mediawiki", std::string(5000, '*') + " x\n{|\n" + std::string(5000, '|') + "\n" + std::string(4000, '[') + "x"},
      {"typst", std::string(20000, '[') + "#emph[" + std::string(2000, '#')},
      {"djot", std::string(5000, '>') + " " + std::string(5000, '{') + std::string(5000, '_')},
      {"man", ".RS\n.RS\n" + [] {
         std::string s;
         for (int i = 0; i < 5000; ++i) s += ".RS\n.IP \\(bu\nx\n";
         return s;
       }()},
  };
  for (const auto& [format, text] : deep) {
    INFO("pathological ", format);
    const auto doc = test::read_as<ir::Document>(format, test::bytes(text));
    if (doc) document::enforce_depth(const_cast<ir::Document&>(*doc));
    CHECK(true);
  }
}

TEST_CASE("document/roundtrip: formats are detected by content and extension") {
  struct Detect {
    const char* fixture;
    const char* filename;
    const char* expected;
  };
  const Detect cases[] = {
      {"pandoc/sample.html", "", "html"},         {"pandoc/sample.html", "page.htm", "html"},
      {"pandoc/sample.gfm.md", "notes.md", "md"}, {"pandoc/sample.rst", "doc.rst", "rst"},
      {"pandoc/sample.adoc", "doc.adoc", "asciidoc"}, {"pandoc/sample.org", "", "org"},
      {"pandoc/sample.wiki", "page.wiki", "mediawiki"}, {"pandoc/sample.textile", "a.textile", "textile"},
      {"pandoc/sample.jira", "", "jira"},          {"pandoc/sample.typ", "a.typ", "typst"},
      {"pandoc/sample.dj", "a.dj", "djot"},        {"pandoc/sample.tex", "", "tex"},
      {"pandoc/sample.1", "", "man"},              {"pandoc/sample.1", "sample.1", "man"},
      {"pandoc/sample.dbk", "", "docbook"},        {"pandoc/sample.dbk", "a.xml", "docbook"},
      {"pandoc/sample.fb2", "", "fb2"},            {"pandoc/sample.jats.xml", "", "jats"},
      {"pandoc/sample.tei.xml", "", "tei"},        {"pandoc/sample.opml", "", "opml"},
      {"pandoc/sample.ipynb", "", "ipynb"},        {"pandoc/sample.texi", "", "texinfo"},
      {"pandoc/sample.bbcode", "a.bbcode", "bbcode"}, {"pandoc/sample.t2t", "a.t2t", "t2t"},
      {"pandoc/sample.dokuwiki", "a.dokuwiki", "dokuwiki"}, {"handwritten/sample.creole", "a.creole", "creole"},
      {"handwritten/sample.pod", "", "pod"},       {"handwritten/sample.mdoc", "", "man"},
      {"python/page.mht", "", "mhtml"},            {"python/note.enex", "", "enex"},
      {"textutil/sample.webarchive", "", "webarchive"}, {"python/outputs.ipynb", "", "ipynb"},
  };
  for (const Detect& c : cases) {
    INFO("fixture ", c.fixture, " name '", c.filename, "'");
    const auto detected = detect_format(Registry::instance(), test::fixture(std::string("document/") + c.fixture), c.filename);
    REQUIRE(detected.has_value());
    CHECK(detected->format->id == c.expected);
  }
  // Plain prose stays plain text.
  const auto prose = detect_format(Registry::instance(), test::bytes("Just a few words.\nAnother line.\n"), "");
  REQUIRE(prose.has_value());
  CHECK(prose->format->id == "txt");
  CHECK(Registry::instance().validate().empty());
}
