// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Bibliographies: BibTeX/BibLaTeX, RIS, EndNote XML and tagged, CSL-JSON,
// MEDLINE, MODS, RefWorks and Web of Science, all through the CSL-JSON model.
#include "engine/detect.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/table.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/json.hpp"

using namespace cvt;

namespace {

std::string detected(std::string_view fixture_name, std::string_view filename) {
  const Bytes data = test::fixture(fixture_name);
  const auto detection = detect_format(Registry::instance(), data, filename);
  return detection ? detection->format->id : std::string("<nothing>");
}

/// The article every fixture describes (fields every format can hold).
void check_article(const Json& item, bool unicode_title = true) {
  CHECK(item["type"].as_string() == "article-journal");
  if (unicode_title) CHECK(item["title"].as_string() == "A synthetic study of ünïcödé handling");
  REQUIRE(item["author"].size() == 2);
  CHECK(item["author"][0]["family"].as_string() == "Example");
  CHECK(item["author"][0]["given"].as_string() == "Ada");
  const Json& second = item["author"][1];
  const std::string family = second["non-dropping-particle"].as_string().empty()
                                 ? second["family"].as_string()
                                 : second["non-dropping-particle"].as_string() + " " + second["family"].as_string();
  CHECK(family == "de la Fontaine");
  CHECK(second["given"].as_string() == "Jean-Luc");
  CHECK(item["volume"].as_string() == "12");
  CHECK(item["issue"].as_string() == "3");
  CHECK(item["page"].as_string() == "101-115");
  CHECK(item["DOI"].as_string() == "10.1234/example.2021.001");
  CHECK(item["issued"]["date-parts"][0][0].as_int64() == 2021);
}

void check_book(const Json& item) {
  CHECK(item["type"].as_string() == "book");
  CHECK(item["title"].as_string() == "Fictional Methods");
  CHECK(item["author"][0]["family"].as_string() == "Sample");
  CHECK(item["author"][0]["given"].as_string() == "Ben");
  CHECK(item["author"][0]["suffix"].as_string() == "Jr.");
  CHECK(item["publisher"].as_string() == "Example Press");
  CHECK(item["publisher-place"].as_string() == "Springfield");
  CHECK(item["issued"]["date-parts"][0][0].as_int64() == 2019);
}

}  // namespace

TEST_CASE("pim/bibtex: decodes files written by bibtexparser and pybtex") {
  const auto bib = test::read_as<ir::Data>("bib", test::fixture("pim/bibliography/bibtexparser.bib"));
  REQUIRE_OK(bib);
  REQUIRE(bib->value.size() == 2);
  check_article(bib->value[0]);
  CHECK(bib->value[0]["id"].as_string() == "example2021");
  CHECK(bib->value[0]["issued"]["date-parts"][0][1].as_int64() == 3);
  CHECK(bib->value[0]["keyword"].as_string() == "testing, unicode");
  const Json& book = bib->value[1];
  CHECK(book["author"][0]["suffix"].as_string() == "Jr.");
  CHECK(book["ISBN"].as_string() == "978-0-00-000000-0");
  const auto py = test::read_as<ir::Data>("bib", test::fixture("pim/bibliography/pybtex.bib"));
  REQUIRE_OK(py);
  const Json& proc = py->value[0];
  CHECK(proc["type"].as_string() == "paper-conference");
  CHECK(proc["author"][0]["literal"].as_string() == "Example Consortium");
  CHECK(proc["editor"][0]["family"].as_string() == "Example");
  CHECK(proc["title"].as_string() == "Proceedings paper on DNA of test data");
  CHECK(proc["container-title"].as_string() == "Proceedings of the Imaginary Conference");
}

TEST_CASE("pim/bibtex: macros, concatenation, LaTeX and name forms") {
  const auto bib = test::read_as<ir::Data>("bib", test::fixture("pim/bibliography/macros.bib"));
  REQUIRE_OK(bib);
  REQUIRE(bib->value.size() == 2);
  const Json& article = bib->value[0];
  CHECK(article["container-title"].as_string() == "Journal of Made-up Results (second series)");
  CHECK(article["title"].as_string() == "Café & crème — an em dash, emphasis and math x^2");
  CHECK(article["issued"]["date-parts"][0][2].as_int64() == 15);
  REQUIRE(article["author"].size() == 3);
  CHECK(article["author"][0]["non-dropping-particle"].as_string() == "de la");
  CHECK(article["author"][0]["family"].as_string() == "Fontaine");
  CHECK(article["author"][0]["given"].as_string() == "Jean-Luc");
  CHECK(article["author"][1]["family"].as_string() == "Müller");
  CHECK(article["author"][1]["given"].as_string() == "Jörg");
  CHECK(article["author"][2]["family"].as_string() == "Østergaard");
  CHECK(article["author"][2]["given"].as_string() == "Søren");
  CHECK(article["URL"].as_string() == "https://example.org/paper?id=1&x=2");
  CHECK(article["language"].as_string() == "french");
  const Json& online = bib->value[1];
  CHECK(online["type"].as_string() == "webpage");
  CHECK(online["author"][0]["given"].as_string() == "Ada");
  CHECK(online["title"].as_string() == "Quoted Title with “quotes”");
  CHECK(online["accessed"]["date-parts"][0][1].as_int64() == 1);
}

TEST_CASE("pim/bibtex: writing escapes accents and specials; biblatex dialect") {
  const auto csl = test::read_as<ir::Data>("csl-json", test::fixture("pim/bibliography/items.csl.json"));
  REQUIRE_OK(csl);
  const auto bibtex = test::write_from("bib", *csl);
  REQUIRE_OK(bibtex);
  const std::string text = test::str(*bibtex);
  CHECK(text.find("@article{example2021,") != std::string::npos);
  CHECK(text.find("title = {A synthetic study of {\\\"u}n{\\\"i}c{\\\"o}d{\\'e} handling}") != std::string::npos);
  CHECK(text.find("author = {Example, Ada and de la Fontaine, Jean-Luc}") != std::string::npos);
  CHECK(text.find("pages = {101--115}") != std::string::npos);
  CHECK(text.find("month = mar") != std::string::npos);
  const auto biblatex = test::write_from("bib", *csl, R"({"bib_dialect":"biblatex"})");
  REQUIRE_OK(biblatex);
  const std::string latex = test::str(*biblatex);
  CHECK(latex.find("journaltitle = {Journal of Made-up Results}") != std::string::npos);
  CHECK(latex.find("date = {2021-03}") != std::string::npos);
  CHECK(latex.find("ünïcödé") != std::string::npos);  // UTF-8 kept for BibLaTeX
  // Both read back to the same records.
  for (const Bytes* written : {&*bibtex, &*biblatex}) {
    const auto back = test::read_as<ir::Data>("bib", *written);
    REQUIRE_OK(back);
    check_article(back->value[0]);
    check_book(back->value[1]);
  }
}

TEST_CASE("pim/bibliography: tagged and XML formats decode to the same records") {
  struct Case {
    const char* format;
    const char* file;
    bool book;
    bool unicode;
  };
  const Case cases[] = {
      {"ris", "pim/bibliography/rispy.ris", true, true},
      {"enw", "pim/bibliography/endnote.enw", true, true},
      {"refworks", "pim/bibliography/refworks.txt", true, true},
      {"csl-json", "pim/bibliography/items.csl.json", true, true},
      {"nbib", "pim/bibliography/pubmed.nbib", false, false},
      {"isi", "pim/bibliography/wos.isi", false, false},
      {"endnote-xml", "pim/bibliography/endnote.xml", false, true},
      {"mods", "pim/bibliography/records.mods.xml", false, true},
  };
  for (const Case& c : cases) {
    INFO(c.format);
    const auto data = test::read_as<ir::Data>(c.format, test::fixture(c.file));
    REQUIRE_OK(data);
    REQUIRE(data->value.size() == (c.book ? 2u : 1u));
    check_article(data->value[0], c.unicode);
    if (c.book) check_book(data->value[1]);
  }
  const auto nbib = test::read_as<ir::Data>("nbib", test::fixture("pim/bibliography/pubmed.nbib"));
  REQUIRE_OK(nbib);
  CHECK(nbib->value[0]["title"].as_string() ==
        "A synthetic study of ünïcödé handling in long titles that wrap onto a continuation line.");
  CHECK(nbib->value[0]["language"].as_string() == "en");
  const auto isi = test::read_as<ir::Data>("isi", test::fixture("pim/bibliography/wos.isi"));
  REQUIRE_OK(isi);
  CHECK(isi->value[0]["title"].as_string() == "A synthetic study of unicode handling with a title that continues on the next line");
  CHECK(isi->value[0]["issued"]["date-parts"][0][1].as_int64() == 3);
}

TEST_CASE("pim/bibliography: every writable pair round-trips the records") {
  const char* formats[] = {"bib", "ris", "enw", "refworks", "csl-json", "nbib", "isi", "endnote-xml", "mods"};
  const auto source = test::read_as<ir::Data>("csl-json", test::fixture("pim/bibliography/items.csl.json"));
  REQUIRE_OK(source);
  for (const char* from : formats) {
    const auto written = test::write_from(from, *source);
    REQUIRE_OK(written);
    for (const char* to : formats) {
      INFO(from, " -> ", to);
      const test::Converted converted = test::convert(*written, from, to);
      REQUIRE(converted.ok());
      const auto back = test::read_as<ir::Data>(to, converted.data);
      REQUIRE_OK(back);
      REQUIRE(back->value.size() == 2);
      check_article(back->value[0]);
      // MEDLINE and Web of Science describe journal articles only.
      const bool article_only = std::string_view(from) == "nbib" || std::string_view(to) == "nbib" ||
                                std::string_view(from) == "isi" || std::string_view(to) == "isi";
      if (!article_only) {
        CHECK(back->value[1]["type"].as_string() == "book");
        CHECK(back->value[1]["title"].as_string() == "Fictional Methods");
        CHECK(back->value[1]["author"][0]["suffix"].as_string() == "Jr.");
      }
    }
  }
  // csl-json -> csl-json is exact.
  const test::Converted same = test::convert(test::fixture("pim/bibliography/items.csl.json"), "csl-json", "csl-json");
  REQUIRE(same.ok());
  const auto copy = test::read_as<ir::Data>("csl-json", same.data);
  REQUIRE_OK(copy);
  CHECK_MESSAGE(test::data_equal(source->value, copy->value), test::data_diff(source->value, copy->value));
}

TEST_CASE("pim/bibliography: reference list and table views") {
  const auto document = test::read_as<ir::Document>("ris", test::fixture("pim/bibliography/rispy.ris"));
  REQUIRE_OK(document);
  const std::string text = ir::plain_text(*document);
  CHECK(text.find("Example, A., & de la Fontaine, J.-L. (2021). A synthetic study of ünïcödé handling. Journal of Made-up "
                  "Results, 12(3), 101-115. https://doi.org/10.1234/example.2021.001") != std::string::npos);
  CHECK(text.find("Sample, B., Jr. (2019). Fictional Methods. Example Press.") != std::string::npos);
  const auto table = test::read_as<ir::Table>("bib", test::fixture("pim/bibliography/bibtexparser.bib"));
  REQUIRE_OK(table);
  CHECK(table->sheets[0].rows[1][2].text == "Example, Ada; de la Fontaine, Jean-Luc");
  CHECK(table->sheets[0].rows[1][5].number == 2021.0);
}

TEST_CASE("pim/bibliography: detection and damaged input") {
  CHECK(detected("pim/bibliography/bibtexparser.bib", "") == "bib");
  CHECK(detected("pim/bibliography/macros.bib", "refs.bib") == "bib");
  CHECK(detected("pim/bibliography/rispy.ris", "") == "ris");
  CHECK(detected("pim/bibliography/endnote.enw", "") == "enw");
  CHECK(detected("pim/bibliography/refworks.txt", "export.txt") == "refworks");
  CHECK(detected("pim/bibliography/items.csl.json", "items.json") == "csl-json");
  CHECK(detected("pim/bibliography/pubmed.nbib", "") == "nbib");
  CHECK(detected("pim/bibliography/wos.isi", "savedrecs.txt") == "isi");
  CHECK(detected("pim/bibliography/endnote.xml", "lib.xml") == "endnote-xml");
  CHECK(detected("pim/bibliography/records.mods.xml", "x.xml") == "mods");
  struct Case {
    const char* format;
    const char* file;
  };
  const Case cases[] = {{"bib", "pim/bibliography/macros.bib"},       {"bib", "pim/bibliography/bibtexparser.bib"},
                        {"ris", "pim/bibliography/rispy.ris"},           {"enw", "pim/bibliography/endnote.enw"},
                        {"refworks", "pim/bibliography/refworks.txt"},   {"csl-json", "pim/bibliography/items.csl.json"},
                        {"nbib", "pim/bibliography/pubmed.nbib"},        {"isi", "pim/bibliography/wos.isi"},
                        {"endnote-xml", "pim/bibliography/endnote.xml"}, {"mods", "pim/bibliography/records.mods.xml"}};
  for (const Case& c : cases) {
    INFO(c.format);
    CHECK(test::fuzz_reader<ir::Data>(c.format, test::fixture(c.file)) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Data>("bib", test::bytes("@article{x, title = {a {b}")), Status::Decode);
  // A macro that doubles itself cannot blow up memory.
  std::string bomb = "@string{a = \"xxxxxxxxxxxxxxxx\"}\n";
  for (int i = 0; i < 30; ++i) bomb += std::format("@string{{a = a # a}}\n");
  bomb += "@misc{k, title = a}\n";
  CHECK_STATUS(test::read_as<ir::Data>("bib", test::bytes(bomb)), Status::Limit);
}
