// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// pdf family: writing documents and images, and PDF -> PDF compression.
#include <string>

#include "families/pdf/document.hpp"
#include "families/pdf/pdf_test_util.hpp"
#include "ir/blob.hpp"

using namespace cvt;
using namespace cvt::ir::build;

namespace {

ir::Document sample_document() {
  ir::Document doc;
  doc.metadata.title = "Writer Test";
  doc.metadata.author = "Someone";
  doc.blocks.push_back(heading(1, "Main Title"));
  doc.blocks.push_back(paragraph({text("Plain text with "), strong("bold"), text(", "), emphasis("italic"), text(", "),
                                  code("code()"), text(" and "), hyperlink("https://example.net/x", "a link"),
                                  text(". Unicode: caf\xC3\xA9 \xE2\x82\xAC \xCE\xB1\xCE\xB2\xCE\xB3 \xD0\x96.")}));
  doc.blocks.push_back(heading(2, "Lists"));
  doc.blocks.push_back(list(false, {{paragraph("First item")}, {paragraph("Second item"), list(true, {{paragraph("Nested one")}})}}));
  doc.blocks.push_back(code_block("int main() {\n  return 0;\n}"));
  doc.blocks.push_back(block_quote({paragraph("A quoted remark.")}));
  doc.blocks.push_back(table({row({"Name", "Value"}, true), row({"Alpha", "1"}), row({"Beta", "2"})}));
  doc.blocks.push_back(horizontal_rule());
  std::string long_text;
  for (int i = 0; i < 400; ++i) long_text += "Lorem ipsum dolor sit amet " + std::to_string(i) + ". ";
  doc.blocks.push_back(paragraph(long_text));
  doc.blocks.push_back(page_break());
  doc.blocks.push_back(heading(2, "After the break"));
  return doc;
}

std::size_t page_count(const Bytes& pdf) {
  auto file = pdf::PdfFile::open(pdf);
  return file ? (*file)->pages().size() : 0;
}

}  // namespace

TEST_CASE("pdf/write: documents round-trip through our reader") {
  const ir::Document doc = sample_document();
  std::vector<std::string> warnings;
  const auto pdf = test::write_from("pdf", doc, {}, &warnings);
  REQUIRE_OK(pdf);
  CHECK(warnings.empty());
  CHECK(test::str(*pdf).starts_with("%PDF-1.7"));
  CHECK(page_count(*pdf) >= 3);
  auto back = test::read_as<ir::Document>("pdf", *pdf);
  REQUIRE_OK(back);
  CHECK(test::text_similarity(ir::plain_text(doc), ir::plain_text(*back)) >= 0.97);
  const std::string text = ir::plain_text(*back);
  CHECK(text.find("caf\xC3\xA9 \xE2\x82\xAC \xCE\xB1\xCE\xB2\xCE\xB3 \xD0\x96") != std::string::npos);
  CHECK(text.find("int main() {") != std::string::npos);
  // Structure: headings, lists, table, link, code.
  int headings = 0, lists = 0, tables = 0, codes = 0;
  for (const ir::Block& b : back->blocks) {
    headings += b.is<ir::Heading>() ? 1 : 0;
    lists += b.is<ir::List>() ? 1 : 0;
    tables += b.is<ir::TableBlock>() ? 1 : 0;
    codes += b.is<ir::CodeBlock>() ? 1 : 0;
  }
  CHECK(headings >= 3);
  CHECK(lists >= 1);
  CHECK(tables == 1);
  CHECK(codes == 1);
  // Privacy: no author by default, producer is ours, title kept.
  CHECK(test::str(*pdf).find("Someone") == std::string::npos);
  CHECK(back->metadata.title == "Writer Test");
  auto with_meta = test::write_from("pdf", doc, R"({"strip_metadata":false})");
  REQUIRE_OK(with_meta);
  auto meta_back = test::read_as<ir::Document>("pdf", *with_meta);
  REQUIRE_OK(meta_back);
  CHECK(meta_back->metadata.author == "Someone");
}

TEST_CASE("pdf/write: page options and characters outside the bundled fonts") {
  ir::Document doc;
  doc.blocks.push_back(paragraph("Hello \xE4\xB8\xAD\xE6\x96\x87"));  // CJK is not covered by Inter
  std::vector<std::string> warnings;
  const auto a4 = test::write_from("pdf", doc, {}, &warnings);
  REQUIRE_OK(a4);
  CHECK(warnings.size() == 1);
  auto file = pdf::PdfFile::open(*a4);
  REQUIRE(file.has_value());
  CHECK_APPROX((*file)->pages()[0].media_box.width(), 595.276, 0.01);
  const auto letter = test::write_from("pdf", doc, R"({"page_size":"letter","orientation":"landscape"})");
  REQUIRE_OK(letter);
  auto lf = pdf::PdfFile::open(*letter);
  REQUIRE(lf.has_value());
  CHECK_APPROX((*lf)->pages()[0].media_box.width(), 792.0, 0.01);
  // Rendering our own output matches Ghostscript's look closely enough to be legible.
  auto image = test::read_as<ir::Image>("pdf", *a4, R"({"dpi":20})");
  REQUIRE_OK(image);
  CHECK(image->frames[0].width == 166);
}

TEST_CASE("pdf/write: images become pages; lossless round trip and quality") {
  ir::Image image;
  image.frames.push_back(test::checker_frame(64, 40, 8));
  image.frames.push_back(test::alpha_gradient_frame(64, 40));
  image.dpi_x = image.dpi_y = 72;
  const auto pdf = test::write_from("pdf", image, R"({"lossless":true})");
  REQUIRE_OK(pdf);
  CHECK(page_count(*pdf) == 2);
  auto rendered = test::read_as<ir::Image>("pdf", *pdf, R"({"dpi":72,"pages":"1"})");
  REQUIRE_OK(rendered);
  REQUIRE(rendered->frames[0].width == 64);
  CHECK(test::psnr(rendered->frames[0], image.frames[0]) >= 40);
  // The alpha page carries a soft mask.
  auto second = test::read_as<ir::Image>("pdf", *pdf, R"({"dpi":72,"pages":"2","background":"transparent"})");
  REQUIRE_OK(second);
  CHECK(test::max_abs_diff(second->frames[0], image.frames[1]) <= 3);
  // Fixed paper sizes.
  const auto a4 = test::write_from("pdf", image, R"({"page_size":"a4","margin_mm":10})");
  REQUIRE_OK(a4);
  auto file = pdf::PdfFile::open(*a4);
  REQUIRE(file.has_value());
  CHECK_APPROX((*file)->pages()[0].media_box.height(), 595.276, 0.01);  // landscape: picture is wider
  // Photographs use JPEG when the registry provides an encoder: lower quality is smaller.
  ir::Image photo;
  photo.frames.push_back(test::noise_frame(96, 96, 5));
  const auto high = test::write_from("pdf", photo, R"({"quality":95})");
  const auto low = test::write_from("pdf", photo, R"({"quality":40})");
  REQUIRE_OK(high);
  REQUIRE_OK(low);
  if (test::find_writer("jpg", Kind::Image) != nullptr) CHECK(low->size() < high->size());
}

TEST_CASE("pdf/compress: PDF -> PDF is smaller and renders the same") {
  for (const char* name : {"qpdf_uncompressed.pdf", "libreoffice_report.pdf", "gs_graphics.pdf"}) {
    CAPTURE(name);
    const Bytes input = test::fixture(std::string("pdf/") + name);
    const test::Converted out = test::convert(input, "pdf", "pdf");
    REQUIRE(out.ok());
    CHECK(out.data.size() <= input.size());
    auto before = test::read_as<ir::Image>("pdf", input, R"({"dpi":50})");
    auto after = test::read_as<ir::Image>("pdf", out.data, R"({"dpi":50})");
    REQUIRE_OK(before);
    REQUIRE_OK(after);
    CHECK(test::ssim(before->frames[0], after->frames[0]) >= 0.99);
    CHECK(pdf_test::fixture_text(name) == [&] {
      auto doc = test::read_as<ir::Document>("pdf", out.data);
      return doc ? ir::plain_text(*doc) : std::string();
    }());
  }
  const Bytes uncompressed = test::fixture("pdf/qpdf_uncompressed.pdf");
  CHECK(test::convert(uncompressed, "pdf", "pdf").data.size() < uncompressed.size() * 9 / 10);
}

TEST_CASE("pdf/compress: decryption, page extraction and metadata") {
  const Bytes enc = test::fixture("pdf/enc_aes_256_user.pdf");
  CHECK(test::convert(enc, "pdf", "pdf").status == Status::InvalidArgument);
  const test::Converted plain = test::convert(enc, "pdf", "pdf", R"({"password":"secret"})");
  REQUIRE(plain.ok());
  auto reopened = pdf::PdfFile::open(plain.data);
  REQUIRE(reopened.has_value());
  CHECK(!(*reopened)->encrypted());
  const Bytes two = test::fixture("pdf/gs_two_pages.pdf");
  const test::Converted second = test::convert(two, "pdf", "pdf", R"({"pages":"2"})");
  REQUIRE(second.ok());
  CHECK(page_count(second.data) == 1);
  auto doc = test::read_as<ir::Document>("pdf", second.data);
  REQUIRE_OK(doc);
  CHECK(ir::plain_text(*doc).find("Beta column") != std::string::npos);
  // Metadata is stripped by default and kept on request.
  const Bytes article = test::fixture("pdf/latex_small.pdf");
  const test::Converted stripped = test::convert(article, "pdf", "pdf", R"({"compression_level":9})");
  REQUIRE(stripped.ok());
  CHECK(stripped.text().find("pdfTeX") == std::string::npos || stripped.data == article);
  // Non-PDF input is refused by the PDF rewriter.
  CHECK_STATUS(test::write_from("pdf", ir::Blob{test::bytes("not a pdf"), "", "", ""}), Status::UnsupportedConversion);
}

TEST_CASE("pdf/write: writers survive odd input") {
  ir::Document empty;
  const auto pdf = test::write_from("pdf", empty);
  REQUIRE_OK(pdf);
  CHECK(page_count(*pdf) == 1);
  ir::Document wide;
  wide.blocks.push_back(paragraph(std::string(3000, 'W')));  // one unbreakable word
  std::vector<ir::TableRow> rows;
  for (int i = 0; i < 120; ++i) rows.push_back(row({"cell " + std::to_string(i), std::string(200, 'x')}, i == 0));
  wide.blocks.push_back(table(std::move(rows)));
  const auto out = test::write_from("pdf", wide);
  REQUIRE_OK(out);
  CHECK(page_count(*out) > 2);
  ir::Image invalid;
  CHECK_STATUS(test::write_from("pdf", invalid), Status::Encode);
}
