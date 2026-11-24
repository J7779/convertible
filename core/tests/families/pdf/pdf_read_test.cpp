// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// pdf family: reading real files (structure, text extraction, security, recovery).
#include <string>

#include "families/pdf/pdf_test_util.hpp"
#include "families/pdf/document.hpp"
#include "ir/data.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace std::string_view_literals;

namespace {

ir::Document read_doc(std::string_view name, std::string_view options = {}) {
  const Bytes data = test::fixture(std::string("pdf/") + std::string(name));
  auto doc = test::read_as<ir::Document>("pdf", data, options);
  REQUIRE_OK(doc);
  return std::move(*doc);
}

std::string headings(const ir::Document& doc) {
  std::string out;
  for (const ir::Block& b : doc.blocks) {
    if (const auto* h = b.get_if<ir::Heading>()) out += std::to_string(h->level) + ":" + ir::plain_text(h->content) + "|";
  }
  return out;
}

template <class T>
const T* first_block(const ir::Document& doc) {
  for (const ir::Block& b : doc.blocks) {
    if (const auto* t = b.get_if<T>()) return t;
  }
  return nullptr;
}

bool has_link(const ir::Inlines& inlines, std::string_view url) {
  for (const ir::Inline& i : inlines) {
    if (const auto* l = i.get_if<ir::Link>()) {
      if (l->url == url) return true;
    }
    if (const auto* s = i.get_if<ir::Strong>(); s && has_link(s->children, url)) return true;
    if (const auto* e = i.get_if<ir::Emphasis>(); e && has_link(e->children, url)) return true;
  }
  return false;
}

bool doc_has_link(const ir::Document& doc, std::string_view url) {
  for (const ir::Block& b : doc.blocks) {
    if (const auto* p = b.get_if<ir::Paragraph>(); p && has_link(p->content, url)) return true;
  }
  return false;
}

// A minimal PDF assembled from object bodies (numbers 1..n) with a correct xref.
std::string build_pdf(const std::vector<std::string>& objects, std::string_view trailer = "/Root 1 0 R") {
  std::string out = "%PDF-1.4\n";
  std::vector<std::size_t> offsets;
  for (std::size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const std::size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
  for (const std::size_t o : offsets) {
    const std::string n = std::to_string(o);
    out += std::string(10 - n.size(), '0') + n + " 00000 n \n";
  }
  out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " " + std::string(trailer) + " >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return out;
}

std::string stream_object(std::string_view dict_extra, std::string_view data) {
  return "<< /Length " + std::to_string(data.size()) + " " + std::string(dict_extra) + " >>\nstream\n" +
         std::string(data) + "\nendstream";
}

std::vector<std::string> simple_page(std::string_view content, std::string_view resources = "") {
  return {"<< /Type /Catalog /Pages 2 0 R >>",
          "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
          "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> " +
              std::string(resources) + " >> >>",
          stream_object("", content),
          "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
}

}  // namespace

TEST_CASE("pdf/read: LaTeX article keeps headings, lists, table, link and styles") {
  const ir::Document doc = read_doc("latex_article.pdf");
  CHECK(headings(doc) == "1:1 Introduction|2:1.1 Details|1:2 Conclusion|");
  const ir::List* list = first_block<ir::List>(doc);
  REQUIRE(list != nullptr);
  CHECK(!list->ordered);
  REQUIRE(list->items.size() == 2);
  CHECK(ir::plain_text(list->items[1]) == "Second bullet item with more words\n");
  const ir::TableBlock* table = first_block<ir::TableBlock>(doc);
  REQUIRE(table != nullptr);
  REQUIRE(table->rows.size() == 3);
  REQUIRE(table->rows[2].cells.size() == 3);
  CHECK(ir::plain_text(table->rows[2].cells[0].content) == "Pears");
  CHECK(ir::plain_text(table->rows[2].cells[2].content) == "2.25");
  CHECK(doc_has_link(doc, "https://example.com/docs"));
  // Bold and italic runs survive.
  const std::string text = ir::plain_text(doc);
  CHECK(text.find("The bold words and the emphasised words keep their style.") != std::string::npos);
  bool strong = false;
  for (const ir::Block& b : doc.blocks) {
    if (const auto* p = b.get_if<ir::Paragraph>()) {
      for (const ir::Inline& i : p->content) {
        if (const auto* st = i.get_if<ir::Strong>()) strong = strong || util::trim(ir::plain_text(st->children)) == "bold words";
      }
    }
  }
  CHECK(strong);
  // Similarity against the independent pdftotext output and the LaTeX source words.
  const std::string reference = test::fixture_text("pdf/ref/latex_article.pdftotext.txt");
  CHECK(test::text_similarity(text, reference) >= 0.9);
}

TEST_CASE("pdf/read: CFF (Type 1C) rewrite extracts the same text") {
  const std::string a = pdf_test::fixture_text("latex_article.pdf");
  const std::string b = pdf_test::fixture_text("gs_cff_article.pdf");
  CHECK(test::text_similarity(a, b) >= 0.97);
}

TEST_CASE("pdf/read: two-column page is read column by column") {
  const std::string text = pdf_test::fixture_text("latex_twocolumn.pdf");
  const auto alpha = text.find("Alpha column text begins here");
  const auto end_left = text.find("Excepteur sint occaecat cupidatat non proident.");
  const auto beta = text.find("Beta column text starts the right column");
  REQUIRE(alpha != std::string::npos);
  REQUIRE(beta != std::string::npos);
  CHECK(alpha < end_left);
  CHECK(end_left < beta);
  CHECK(test::text_similarity(text, test::fixture_text("pdf/ref/latex_twocolumn.pdftotext.txt")) >= 0.85);
}

TEST_CASE("pdf/read: LibreOffice and WeasyPrint reports (TrueType/OpenType, ToUnicode)") {
  for (const char* name : {"libreoffice_report.pdf", "weasyprint_report.pdf"}) {
    CAPTURE(name);
    const ir::Document doc = read_doc(name);
    CHECK(headings(doc).starts_with("1:Quarterly Report|2:Highlights|"));
    const std::string text = ir::plain_text(doc);
    CHECK(text.find("caf\xC3\xA9, na\xC3\xAFve, Z\xC3\xBCrich, \xE2\x82\xAC") != std::string::npos);
    const ir::TableBlock* table = first_block<ir::TableBlock>(doc);
    REQUIRE(table != nullptr);
    REQUIRE(table->rows.size() == 4);
    CHECK(ir::plain_text(table->rows[3].cells[0].content) == "East");
    CHECK(ir::plain_text(table->rows[3].cells.back().content).ends_with("151"));
    CHECK(doc_has_link(doc, "https://example.org/report"));
    int lists = 0;
    for (const ir::Block& b : doc.blocks) lists += b.is<ir::List>() ? 1 : 0;
    CHECK(lists == 2);
    CHECK(first_block<ir::ImageBlock>(doc) != nullptr);
  }
  const std::string ours = pdf_test::fixture_text("libreoffice_report.pdf");
  CHECK(test::text_similarity(ours, test::fixture_text("pdf/ref/libreoffice_report.pdftotext.txt")) >= 0.85);
}

TEST_CASE("pdf/read: Type 3 and non-embedded standard fonts") {
  const std::string text = pdf_test::fixture_text("gs_type3_base14.pdf");
  CHECK(text.find("Helvetica text: Hello World 123") != std::string::npos);
  CHECK(text.find("Times bold line") != std::string::npos);
  CHECK(text.find("mono = code;") != std::string::npos);
}

TEST_CASE("pdf/read: xref streams, object streams, linearised and uncompressed files") {
  const std::string base = pdf_test::fixture_text("latex_small.pdf");
  CHECK(base.find("Encrypted sample text.") != std::string::npos);
  for (const char* name : {"qpdf_objstm.pdf", "qpdf_linearized.pdf", "qpdf_uncompressed.pdf"}) {
    CAPTURE(name);
    CHECK(pdf_test::fixture_text(name) == base);
  }
}

TEST_CASE("pdf/read: standard security handler (RC4 40/128, AES-128/256)") {
  const std::string base = pdf_test::fixture_text("latex_small.pdf");
  for (const char* name : {"enc_rc4_40.pdf", "enc_rc4_128.pdf", "enc_aes_128.pdf", "enc_aes_256.pdf"}) {
    CAPTURE(name);
    CHECK(pdf_test::fixture_text(name) == base);
  }
  for (const char* name : {"enc_aes_256_user.pdf", "enc_rc4_128_user.pdf"}) {
    CAPTURE(name);
    const Bytes data = test::fixture(std::string("pdf/") + name);
    CHECK_STATUS(test::read_as<ir::Document>("pdf", data), Status::InvalidArgument);
    CHECK_STATUS(test::read_as<ir::Document>("pdf", data, R"({"password":"wrong"})"), Status::InvalidArgument);
    CHECK(pdf_test::fixture_text(name, R"({"password":"secret"})") == base);
    CHECK(pdf_test::fixture_text(name, R"({"password":"owner"})") == base);
  }
}

TEST_CASE("pdf/read: options select pages and images") {
  const Bytes two = test::fixture("pdf/gs_two_pages.pdf");
  auto first = test::read_as<ir::Document>("pdf", two, R"({"pages":"1"})");
  auto second = test::read_as<ir::Document>("pdf", two, R"({"pages":"2"})");
  REQUIRE_OK(first);
  REQUIRE_OK(second);
  CHECK(ir::plain_text(*first).find("Encrypted sample text") != std::string::npos);
  CHECK(ir::plain_text(*first).find("Beta column") == std::string::npos);
  CHECK(ir::plain_text(*second).find("Beta column") != std::string::npos);
  CHECK_STATUS(test::read_as<ir::Document>("pdf", two, R"({"pages":"7"})"), Status::InvalidArgument);
  const ir::Document no_images = read_doc("libreoffice_report.pdf", R"({"include_images":false})");
  CHECK(first_block<ir::ImageBlock>(no_images) == nullptr);
}

TEST_CASE("pdf/read: metadata comes from the Info dictionary") {
  const ir::Document doc = read_doc("latex_article.pdf");
  CHECK(doc.metadata.title == "Sample Article");
  CHECK(doc.metadata.author.empty());
}

TEST_CASE("pdf/read: damaged cross-reference data is repaired") {
  const Bytes original = test::fixture("pdf/latex_small.pdf");
  const std::string base = pdf_test::fixture_text("latex_small.pdf");
  std::string text = test::str(original);
  // 1. Wrong offsets everywhere: prepend junk so every offset is off.
  {
    const std::string shifted = "garbage before the header\n" + text;
    auto doc = test::read_as<ir::Document>("pdf", as_bytes(shifted));
    REQUIRE_OK(doc);
    CHECK(ir::plain_text(*doc) == base);
  }
  // 2. No cross-reference table at all.
  {
    std::string broken = text;
    const auto x = broken.rfind("xref");
    REQUIRE(x != std::string::npos);
    broken.erase(x);
    std::vector<std::string> warnings;
    auto doc = test::read_as<ir::Document>("pdf", as_bytes(broken), {}, &warnings);
    REQUIRE_OK(doc);
    CHECK(ir::plain_text(*doc) == base);
    CHECK(!warnings.empty());
  }
  // 3. Corrupted startxref value.
  {
    std::string broken = text;
    const auto s = broken.rfind("startxref");
    broken.replace(s + 10, 3, "999");
    auto doc = test::read_as<ir::Document>("pdf", as_bytes(broken));
    REQUIRE_OK(doc);
    CHECK(ir::plain_text(*doc) == base);
  }
  // 4. Not a PDF / empty.
  CHECK_STATUS(test::read_as<ir::Document>("pdf", as_bytes("hello"sv)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Document>("pdf", as_bytes("%PDF-1.4\n%%EOF"sv)), Status::Decode);
}

TEST_CASE("pdf/read: hostile structures end quickly without crashing") {
  // Page tree cycle and self-referencing length.
  {
    const std::string pdf = build_pdf({"<< /Type /Catalog /Pages 2 0 R >>",
                                       "<< /Type /Pages /Kids [2 0 R 3 0 R] /Count 1 >>",
                                       "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Contents 4 0 R >>",
                                       "<< /Length 4 0 R >>\nstream\nBT ET\nendstream"});
    (void)test::read_as<ir::Document>("pdf", as_bytes(pdf));
    (void)test::read_as<ir::Image>("pdf", as_bytes(pdf), R"({"dpi":20})");
  }
}

TEST_CASE("pdf/read: recursive and exponential form XObjects are cut short") {
  {
    std::vector<std::string> objs = simple_page("/X1 Do", "/XObject << /X1 6 0 R >>");
    objs.push_back(stream_object("/Type /XObject /Subtype /Form /BBox [0 0 10 10] /Resources << /XObject << /X1 6 0 R /X2 7 0 R >> >>",
                                 "/X1 Do /X2 Do /X2 Do /X2 Do /X2 Do"));
    objs.push_back(stream_object("/Type /XObject /Subtype /Form /BBox [0 0 10 10] /Resources << /XObject << /X1 6 0 R >> >>",
                                 "/X1 Do /X1 Do /X1 Do 0 0 5 5 re f"));
    const std::string pdf = build_pdf(objs);
    auto image = test::read_as<ir::Image>("pdf", as_bytes(pdf), R"({"dpi":20})");
    CHECK(image.has_value());
    (void)test::read_as<ir::Document>("pdf", as_bytes(pdf));
  }
}

TEST_CASE("pdf/read: huge lengths, deep content nesting and absurd page sizes") {
  {
    std::string content(200000, '[');
    content += " BT /F1 12 Tf (x) Tj ET";
    std::vector<std::string> objs = simple_page(content);
    objs[3] = "<< /Length 99999999999 >>\nstream\n" + content + "\nendstream";
    objs[2] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 1000000000000 1000000000000] /Contents 4 0 R >>";
    const std::string pdf = build_pdf(objs);
    (void)test::read_as<ir::Document>("pdf", as_bytes(pdf));
    CHECK_STATUS(test::read_as<ir::Image>("pdf", as_bytes(pdf)), Status::Limit);
  }
}

TEST_CASE("pdf/read: truncated and corrupted real files") {
  CHECK(test::fuzz_reader<ir::Document>("pdf", test::fixture("pdf/latex_small.pdf"), 24) > 0);
  CHECK(test::fuzz_reader<ir::Document>("pdf", test::fixture("pdf/qpdf_objstm.pdf"), 12) > 0);
  CHECK(test::fuzz_reader<ir::Document>("pdf", test::fixture("pdf/enc_aes_128.pdf"), 8) > 0);
}

TEST_CASE("pdf/read: a minimal hand-built page with standard 14 font text") {
  const std::string pdf = build_pdf(simple_page("BT /F1 12 Tf 10 50 Td (Hello) Tj ( world) Tj ET"));
  auto doc = test::read_as<ir::Document>("pdf", as_bytes(pdf));
  REQUIRE_OK(doc);
  CHECK(ir::plain_text(*doc) == "Hello world\n");
}

TEST_CASE("pdf/detect: magic, sniffing and neighbours") {
  const Bytes data = test::fixture("pdf/latex_small.pdf");
  CHECK(test::convert(data, "", "pdf", "", "file.bin").ok());
  const test::Converted detected = test::convert(data, "", "pdf");
  CHECK(detected.ok());
  const std::string junk = "\x00\x01junk\n" + test::str(data);
  CHECK(test::convert(as_bytes(junk), "", "pdf", "", "x.pdf").ok());
  CHECK(Registry::instance().validate().empty());
  for (const char* id : {"pdf", "xps", "oxps", "fdf", "xfdf"}) {
    CAPTURE(id);
    CHECK(Registry::instance().find(id) != nullptr);
  }
}
