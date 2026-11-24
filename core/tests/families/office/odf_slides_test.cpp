// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/office/office_test_support.hpp"

using namespace cvt;
using namespace cvt::test::office;

namespace {

// The python-pptx deck: five slides.
void check_deck(const ir::Document& doc, bool notes = true) {
  CHECK(has_heading(doc, 1, "Widget Deck"));
  CHECK(has_heading(doc, 1, "Agenda Points"));
  CHECK(has_heading(doc, 1, "Parts Table"));
  CHECK(has_heading(doc, 1, "Picture Slide"));
  CHECK(count<ir::PageBreak>(doc) == 4);
  const auto ls = lists(doc);
  REQUIRE(ls.size() >= 1);
  CHECK(ls[0]->items.size() == 2);
  CHECK(ls[0]->items[0].size() == 2);  // "Nested detail" one level deeper
  CHECK(plain(doc).find("Grouped text box") != std::string::npos);
  CHECK(plain(doc).find("Spring") != std::string::npos);
  CHECK(pictures(doc).size() == 1);
  CHECK((plain(doc).find("Remember to smile.") != std::string::npos) == notes);
}

}  // namespace

TEST_CASE("office/odt: pandoc and LibreOffice documents") {
  const auto pandoc = test::read_as<ir::Document>("odt", data("pandoc/sample.odt"));
  REQUIRE_OK(pandoc);
  CHECK(sample_words_found(*pandoc) == std::size(sample_words));
  CHECK(has_heading(*pandoc, 3, "Unicode Corner"));
  CHECK(pandoc->footnotes.size() == 1);
  CHECK(count<ir::CodeBlock>(*pandoc) == 1);
  CHECK(count<ir::BlockQuote>(*pandoc) == 1);
  CHECK(formatted<ir::Strong>(*pandoc, "eiusmod"));
  CHECK(formatted<ir::Code>(*pandoc, "widget_count"));
  const auto lo = test::read_as<ir::Document>("odt", data("libreoffice/sample.odt"));
  REQUIRE_OK(lo);
  CHECK(sample_words_found(*lo) >= std::size(sample_words) - 1);
  const auto ts = tables(*lo);
  REQUIRE(ts.size() >= 2);
  CHECK(ts[0]->rows[2].cells[0].colspan == 2);
  CHECK(ts[0]->rows[3].cells[0].rowspan == 2);
  CHECK(pictures(*lo).size() == 1);
  const auto textutil = test::read_as<ir::Document>("odt", data("textutil/sample.odt"));
  REQUIRE_OK(textutil);
  CHECK(sample_words_found(*textutil) >= std::size(sample_words) - 1);
  const auto flat = test::read_as<ir::Document>("fodt", data("libreoffice/sample.fodt"));
  REQUIRE_OK(flat);
  CHECK(test::text_similarity(*lo, *flat) >= 0.97);
  CHECK(pictures(*flat).size() == 1);  // embedded as base64
}

TEST_CASE("office/odt: writer round trips (ODT and flat ODT)") {
  const ir::Document original = rich_document();
  for (const char* format : {"odt", "fodt"}) {
    INFO(format);
    const auto bytes = test::write_from(format, original);
    REQUIRE_OK(bytes);
    const auto back = test::read_as<ir::Document>(format, *bytes);
    REQUIRE_OK(back);
    CHECK(test::text_similarity(original, *back) >= 0.97);
    CHECK(outline(*back) == outline(original));
    CHECK(formatted<ir::Strong>(*back, "bold"));
    CHECK(formatted<ir::Underline>(*back, "underlined"));
    CHECK(formatted<ir::Subscript>(*back, "2"));
    CHECK(formatted<ir::Code>(*back, "mono_code"));
    CHECK(back->footnotes.size() == 1);
    CHECK(lists(*back).size() == 3);
    CHECK(tables(*back)[0]->rows[2].cells[0].colspan == 2);
    CHECK(count<ir::CodeBlock>(*back) == 1);
    CHECK(count<ir::BlockQuote>(*back) == 1);
    CHECK(test::str(*bytes).find("Somebody") == std::string::npos);
  }
  const auto odt = test::write_from("odt", original);
  REQUIRE_OK(odt);
  CHECK(zip_names(*odt).front() == "mimetype");
  CHECK(Registry::instance().find("odt")->sniff(*odt, "") >= 95);
}

TEST_CASE("office/pptx: python-pptx, LibreOffice and pandoc decks") {
  const auto deck = test::read_as<ir::Document>("pptx", data("python/sample.pptx"));
  REQUIRE_OK(deck);
  check_deck(*deck);
  const auto quiet = test::read_as<ir::Document>("pptx", data("python/sample.pptx"), R"({"include_notes":false})");
  REQUIRE_OK(quiet);
  check_deck(*quiet, false);
  const auto impress = test::read_as<ir::Document>("pptx", data("libreoffice/sample_impress.pptx"));
  REQUIRE_OK(impress);
  check_deck(*impress);
  const auto pandoc = test::read_as<ir::Document>("pptx", data("pandoc/sample.pptx"));
  REQUIRE_OK(pandoc);
  CHECK(has_heading(*pandoc, 1, "Quarterly Widget Report"));
  CHECK(plain(*pandoc).find("Sprocket") != std::string::npos);
  CHECK(has_link(*pandoc, "https://example.com/widgets"));
}

TEST_CASE("office/ppt: PowerPoint 97 deck from LibreOffice") {
  const auto deck = test::read_as<ir::Document>("ppt", data("libreoffice/sample.ppt.gz"));
  REQUIRE_OK(deck);
  check_deck(*deck);
  CHECK(test::fuzz_reader<ir::Document>("ppt", data("libreoffice/sample.ppt.gz"), 32) > 0);
}

TEST_CASE("office/odp: LibreOffice deck and ODP writer") {
  const auto deck = test::read_as<ir::Document>("odp", data("libreoffice/sample.odp"));
  REQUIRE_OK(deck);
  check_deck(*deck);
  const auto bytes = test::write_from("odp", *deck);
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Document>("odp", *bytes);
  REQUIRE_OK(back);
  CHECK(test::text_similarity(*deck, *back) >= 0.95);
  CHECK(count<ir::PageBreak>(*back) == 4);
  CHECK(pictures(*back).size() == 1);
  CHECK(plain(*back).find("Remember to smile.") != std::string::npos);
}

TEST_CASE("office/pptx: writer round trip, slides from a document") {
  const auto deck = test::read_as<ir::Document>("pptx", data("python/sample.pptx"));
  REQUIRE_OK(deck);
  const auto bytes = test::write_from("pptx", *deck);
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Document>("pptx", *bytes);
  REQUIRE_OK(back);
  check_deck(*back);
  CHECK(test::text_similarity(*deck, *back) >= 0.95);
  // A plain document is split at its top-level headings.
  const auto from_doc = test::write_from("pptx", rich_document());
  REQUIRE_OK(from_doc);
  const auto slides = test::read_as<ir::Document>("pptx", *from_doc);
  REQUIRE_OK(slides);
  CHECK(has_heading(*slides, 1, "Lists"));
  CHECK(has_heading(*slides, 1, "Table"));
  CHECK(tables(*slides).size() == 1);
  CHECK(plain(*slides).find("The footnote body.") != std::string::npos);
  const auto names = zip_names(*from_doc);
  CHECK(std::ranges::find(names, "ppt/slides/slide1.xml") != names.end());
  CHECK(zip_text(*from_doc, "ppt/presentation.xml").find("cx=\"12192000\" cy=\"6858000\"") != std::string::npos);
  const auto again = test::write_from("pptx", rich_document());
  REQUIRE_OK(again);
  CHECK(*again == *from_doc);
}

TEST_CASE("office/slides: detection and robustness") {
  const auto& registry = Registry::instance();
  CHECK(registry.find("pptx")->sniff(data("python/sample.pptx"), "") >= 90);
  CHECK(registry.find("ppt")->sniff(data("libreoffice/sample.ppt.gz"), "") >= 90);
  CHECK(registry.find("odp")->sniff(data("libreoffice/sample.odp"), "") >= 95);
  CHECK(registry.find("ppsx")->sniff(data("python/sample.pptx"), "show.ppsx") >= 95);
  CHECK(registry.find("potx")->sniff(data("python/sample.pptx"), "") == 0);
  CHECK(test::fuzz_reader<ir::Document>("pptx", data("python/sample.pptx"), 32) > 0);
  CHECK(test::fuzz_reader<ir::Document>("odp", data("libreoffice/sample.odp"), 32) > 0);
  CHECK(test::fuzz_reader<ir::Document>("odt", data("pandoc/sample.odt"), 64) > 0);
  CHECK(test::fuzz_reader<ir::Document>("fodt", data("libreoffice/sample.fodt"), 32) > 0);
  const test::Converted pdf_route = test::convert(data("python/sample.pptx"), "", "odp");
  CHECK(pdf_route.ok());
}
