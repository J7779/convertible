// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/office/office_test_support.hpp"
#include "families/table/cfb.hpp"

using namespace cvt;
using namespace cvt::test::office;

namespace {

void check_sample_tables(const ir::Document& doc) {
  const auto ts = tables(doc);
  REQUIRE(ts.size() >= 1);
  bool colspan = false;
  bool rowspan = false;
  for (const auto& row : ts[0]->rows) {
    for (const auto& cell : row.cells) {
      colspan = colspan || cell.colspan == 2;
      rowspan = rowspan || cell.rowspan == 2;
    }
  }
  CHECK(colspan);
  CHECK(rowspan);
}

}  // namespace

TEST_CASE("office/rtf: LibreOffice document") {
  const auto doc = test::read_as<ir::Document>("rtf", data("libreoffice/sample.rtf"));
  REQUIRE_OK(doc);
  CHECK(has_heading(*doc, 1, "Quarterly Widget Report"));
  CHECK(has_heading(*doc, 2, "Lists and Steps"));
  CHECK(sample_words_found(*doc) >= std::size(sample_words) - 1);
  const auto ls = lists(*doc);
  REQUIRE(ls.size() >= 2);
  CHECK_FALSE(ls[0]->ordered);
  CHECK(ls[0]->items[1].size() == 2);
  check_sample_tables(*doc);
  CHECK(formatted<ir::Strong>(*doc, "eiusmod"));
  CHECK(formatted<ir::Underline>(*doc, "labore"));
  CHECK(formatted<ir::Superscript>(*doc, "2"));
  CHECK(has_link(*doc, "https://example.com/widgets"));
  CHECK(pictures(*doc).size() == 1);
}

TEST_CASE("office/rtf: pandoc and textutil documents") {
  const auto pandoc = test::read_as<ir::Document>("rtf", data("pandoc/sample.rtf"));
  REQUIRE_OK(pandoc);
  CHECK(sample_words_found(*pandoc) == std::size(sample_words));
  CHECK(has_heading(*pandoc, 1, "Quarterly Widget Report"));
  REQUIRE(pandoc->footnotes.size() == 1);
  CHECK(count<ir::CodeBlock>(*pandoc) == 1);
  const auto ls = lists(*pandoc);  // marker + tab paragraphs become lists
  REQUIRE(ls.size() >= 2);
  CHECK(ls[0]->items.size() == 3);
  CHECK(tables(*pandoc).size() == 1);
  const auto textutil = test::read_as<ir::Document>("rtf", data("textutil/sample.rtf"));
  REQUIRE_OK(textutil);
  CHECK(sample_words_found(*textutil) >= std::size(sample_words) - 1);
  CHECK(tables(*textutil).size() >= 1);
}

TEST_CASE("office/rtf: writer round trip and options") {
  const ir::Document original = rich_document();
  const auto bytes = test::write_from("rtf", original);
  REQUIRE_OK(bytes);
  CHECK(test::str(*bytes).starts_with("{\\rtf1\\ansi"));
  CHECK(test::str(*bytes).find("\\u27721?") != std::string::npos);  // 汉 as a Unicode escape
  const auto back = test::read_as<ir::Document>("rtf", *bytes);
  REQUIRE_OK(back);
  CHECK(test::text_similarity(original, *back) >= 0.97);
  CHECK(outline(*back) == outline(original));
  CHECK(formatted<ir::Strong>(*back, "bold"));
  CHECK(formatted<ir::Emphasis>(*back, "italic"));
  CHECK(formatted<ir::Strikethrough>(*back, "struck"));
  CHECK(formatted<ir::Superscript>(*back, "2"));
  CHECK(has_link(*back, "https://example.com/a"));
  CHECK(back->footnotes.size() == 1);
  CHECK(lists(*back).size() == 3);
  const auto ts = tables(*back);
  REQUIRE(ts.size() == 1);
  CHECK(ts[0]->rows[2].cells[0].colspan == 2);
  CHECK(count<ir::CodeBlock>(*back) == 1);
  const auto letter = test::write_from("rtf", original, R"({"page_size":"letter"})");
  REQUIRE_OK(letter);
  CHECK(test::str(*letter).find("\\paperw12240\\paperh15840") != std::string::npos);
  CHECK(test::str(*bytes).find("Somebody") == std::string::npos);
}

TEST_CASE("office/rtf: hostile input") {
  std::string deep = "{\\rtf1 ";
  for (int i = 0; i < 100000; ++i) deep += "{";
  CHECK_STATUS(test::read_as<ir::Document>("rtf", test::bytes(deep)), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Document>("rtf", test::bytes("not rtf")), Status::Decode);
  // Unterminated destinations, stray braces and huge numbers are tolerated.
  const auto odd = test::read_as<ir::Document>("rtf", test::bytes("{\\rtf1 {\\*\\unknown x}}} \\u99999999999 a\\'zz{\\pict\\bin99999 "));
  CHECK(odd.has_value());
  CHECK(test::fuzz_reader<ir::Document>("rtf", data("pandoc/sample.rtf"), 128) > 0);
}

TEST_CASE("office/doc: Word 97 documents from LibreOffice and textutil") {
  const auto doc = test::read_as<ir::Document>("doc", data("libreoffice/sample.doc"));
  REQUIRE_OK(doc);
  CHECK(has_heading(*doc, 1, "Quarterly Widget Report"));
  CHECK(has_heading(*doc, 2, "Inventory Table"));
  CHECK(sample_words_found(*doc) >= std::size(sample_words) - 1);
  const auto ls = lists(*doc);
  REQUIRE(ls.size() >= 2);
  CHECK_FALSE(ls[0]->ordered);
  CHECK(ls[0]->items[1].size() == 2);
  CHECK(std::ranges::any_of(ls, [](const ir::List* l) { return l->ordered && l->items.size() == 3; }));
  check_sample_tables(*doc);
  CHECK(formatted<ir::Strong>(*doc, "eiusmod"));
  CHECK(formatted<ir::Emphasis>(*doc, "consectetur"));
  CHECK(formatted<ir::Underline>(*doc, "labore"));
  CHECK(formatted<ir::Superscript>(*doc, "2"));
  CHECK(has_link(*doc, "https://example.org/page"));
  const auto pics = pictures(*doc);
  REQUIRE(pics.size() == 1);
  CHECK(picture_bytes(pics[0]) > 0);
  CHECK(doc->metadata.title == "Office Sample");
  const auto textutil = test::read_as<ir::Document>("doc", data("textutil/sample.doc"));
  REQUIRE_OK(textutil);
  CHECK(sample_words_found(*textutil) >= std::size(sample_words) - 1);
}

TEST_CASE("office/doc: footnotes, code and quotes") {
  const auto doc = test::read_as<ir::Document>("doc", data("libreoffice/notes.doc"));
  REQUIRE_OK(doc);
  REQUIRE(doc->footnotes.size() == 1);
  CHECK(ir::plain_text(doc->footnotes[0].blocks).find("The note text lives at the bottom.") != std::string::npos);
  CHECK(count<ir::CodeBlock>(*doc) == 1);
  CHECK(formatted<ir::Strikethrough>(*doc, "struck"));
  const auto without = test::read_as<ir::Document>("doc", data("libreoffice/notes.doc"), R"({"include_notes":false})");
  REQUIRE_OK(without);
  CHECK(without->footnotes.empty());
  // Converts end to end; there is no DOC writer, DOCX is the way out.
  const test::Converted docx = test::convert(data("libreoffice/notes.doc"), "", "docx");
  CHECK(docx.ok());
  CHECK(Registry::instance().has_writer("doc") == false);
}

TEST_CASE("office/doc: protected and damaged files") {
  // A WordDocument stream whose FIB says "encrypted".
  Bytes word(1024);
  word[0] = std::byte{0xEC};
  word[1] = std::byte{0xA5};
  word[2] = std::byte{0xC1};
  word[0x0B] = std::byte{0x01};
  table::CfbWriter writer;
  REQUIRE_OK(writer.add_stream("WordDocument", word));
  const auto file = writer.finish();
  REQUIRE_OK(file);
  const auto result = test::read_as<ir::Document>("doc", *file);
  CHECK_STATUS(result, Status::Decode);
  CHECK(result.error().message.find("password") != std::string::npos);
  CHECK(Registry::instance().find("doc")->sniff(*file, "") >= 90);
  CHECK(Registry::instance().find("doc")->sniff(data("libreoffice/sample.ppt.gz"), "") == 0);
  CHECK(test::fuzz_reader<ir::Document>("doc", data("libreoffice/sample.doc"), 64) > 0);
}
