// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/office/office_test_support.hpp"

using namespace cvt;
using namespace cvt::test::office;

namespace {

Bytes rezip_with(ByteView epub, std::string_view name, std::string_view content) {
  auto zip = util::ZipReader::open(epub);
  util::ZipWriter out;
  util::ZipEntryOptions stored;
  stored.level = 0;
  for (std::size_t i = 0; zip && i < zip->size(); ++i) {
    auto bytes = zip->read(i);
    if (bytes) (void)out.add(zip->entries()[i].name, *bytes, zip->entries()[i].name == "mimetype" ? stored : util::ZipEntryOptions{});
  }
  (void)out.add(name, content);
  auto result = out.finish();
  return result ? std::move(*result) : Bytes{};
}

}  // namespace

TEST_CASE("office/epub: pandoc EPUB 3 and EPUB 2") {
  for (const char* name : {"pandoc/sample.epub", "pandoc/sample_epub2.epub"}) {
    INFO(name);
    const auto doc = test::read_as<ir::Document>("epub", data(name));
    REQUIRE_OK(doc);
    CHECK(sample_words_found(*doc) == std::size(sample_words));
    CHECK(has_heading(*doc, 1, "Quarterly Widget Report"));
    CHECK(has_heading(*doc, 2, "Inventory Table"));
    CHECK(doc->metadata.title == "Office Sample");
    CHECK(doc->metadata.language == "en-GB");
    CHECK(tables(*doc).size() == 1);
    CHECK(doc->footnotes.size() == 1);
    CHECK(pictures(*doc).size() == 1);
    CHECK(has_link(*doc, "https://example.com/widgets"));
  }
}

TEST_CASE("office/epub: LibreOffice EPUB with class-based styling") {
  const auto doc = test::read_as<ir::Document>("epub", data("libreoffice/sample.epub"));
  REQUIRE_OK(doc);
  CHECK(has_heading(*doc, 1, "Quarterly Widget Report"));  // recovered from the CSS (14 pt bold)
  CHECK(formatted<ir::Emphasis>(*doc, "consectetur"));
  CHECK(formatted<ir::Strong>(*doc, "eiusmod"));
  CHECK(tables(*doc).size() >= 1);
  const auto strict = test::read_as<ir::Document>("epub", data("libreoffice/sample.epub"), R"({"strict_headings":true})");
  REQUIRE_OK(strict);
  CHECK_FALSE(has_heading(*strict, 2, "Lists and Steps"));
}

TEST_CASE("office/epub: writer produces a valid, deterministic book") {
  const ir::Document original = rich_document();
  const auto bytes = test::write_from("epub", original);
  REQUIRE_OK(bytes);
  const auto names = zip_names(*bytes);
  REQUIRE(!names.empty());
  CHECK(names.front() == "mimetype");
  CHECK(std::ranges::find(names, "EPUB/nav.xhtml") != names.end());
  CHECK(std::ranges::find(names, "EPUB/toc.ncx") != names.end());
  CHECK(std::ranges::count_if(names, [](const std::string& n) { return n.ends_with(".xhtml") && n.find("chapter") != std::string::npos; }) >= 1);
  const std::string opf = zip_text(*bytes, "EPUB/content.opf");
  CHECK(opf.find("<dc:title>Rich Sample</dc:title>") != std::string::npos);
  CHECK(opf.find("Somebody") == std::string::npos);
  CHECK(opf.find("dcterms:modified") != std::string::npos);
  const auto again = test::write_from("epub", original);
  REQUIRE_OK(again);
  CHECK(*again == *bytes);
  const auto back = test::read_as<ir::Document>("epub", *bytes);
  REQUIRE_OK(back);
  CHECK(test::text_similarity(original, *back) >= 0.95);
  CHECK(has_heading(*back, 2, "Lists"));
  CHECK(back->footnotes.size() == 1);
  CHECK(has_link(*back, "https://example.com/a"));
}

TEST_CASE("office/epub: chapters split at top-level headings with pictures as files") {
  const auto source = test::read_as<ir::Document>("epub", data("pandoc/sample.epub"));
  REQUIRE_OK(source);
  const auto bytes = test::write_from("epub", *source);
  REQUIRE_OK(bytes);
  const auto names = zip_names(*bytes);
  CHECK(std::ranges::count_if(names, [](const std::string& n) { return n.starts_with("EPUB/images/"); }) == 1);
  CHECK(std::ranges::count_if(names, [](const std::string& n) { return n.starts_with("EPUB/chapter-"); }) == 2);
  const auto back = test::read_as<ir::Document>("epub", *bytes);
  REQUIRE_OK(back);
  CHECK(test::text_similarity(*source, *back) >= 0.97);
  CHECK(pictures(*back).size() == 1);
}

TEST_CASE("office/epub: compress shrinks pictures") {
  const Bytes original = data("pandoc/photo.epub");
  const test::Converted small = test::convert(original, "epub", "epub", R"({"quality":50})");
  REQUIRE(small.ok());
  CHECK(small.data.size() < original.size() * 3 / 4);
  const auto a = test::read_as<ir::Document>("epub", original);
  const auto b = test::read_as<ir::Document>("epub", small.data);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  CHECK(test::text_similarity(*a, *b) >= 0.97);
}

TEST_CASE("office/epub: DRM-protected books are refused, font obfuscation is not DRM") {
  const Bytes book = data("pandoc/sample.epub");
  const auto adept = test::read_as<ir::Document>("epub", rezip_with(book, "META-INF/rights.xml", "<rights/>"));
  CHECK_STATUS(adept, Status::Decode);
  CHECK(adept.error().message.find("DRM") != std::string::npos);
  const std::string encrypted =
      "<encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" xmlns:enc=\"http://www.w3.org/2001/04/xmlenc#\">"
      "<enc:EncryptedData><enc:EncryptionMethod Algorithm=\"http://www.w3.org/2001/04/xmlenc#aes128-cbc\"/>"
      "<enc:CipherData><enc:CipherReference URI=\"EPUB/text/ch001.xhtml\"/></enc:CipherData></enc:EncryptedData></encryption>";
  CHECK_STATUS(test::read_as<ir::Document>("epub", rezip_with(book, "META-INF/encryption.xml", encrypted)), Status::Decode);
  const std::string fonts =
      "<encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" xmlns:enc=\"http://www.w3.org/2001/04/xmlenc#\">"
      "<enc:EncryptedData><enc:EncryptionMethod Algorithm=\"http://www.idpf.org/2008/embedding\"/>"
      "<enc:CipherData><enc:CipherReference URI=\"EPUB/fonts/a.otf\"/></enc:CipherData></enc:EncryptedData></encryption>";
  CHECK(test::read_as<ir::Document>("epub", rezip_with(book, "META-INF/encryption.xml", fonts)).has_value());
  CHECK(test::fuzz_reader<ir::Document>("epub", book, 64) > 0);
}

TEST_CASE("office/mobi: calibre MOBI 6 and KF8 books") {
  const auto mobi = test::read_as<ir::Document>("mobi", data("calibre/sample.mobi"));
  REQUIRE_OK(mobi);
  CHECK(sample_words_found(*mobi) >= std::size(sample_words) - 1);
  CHECK(mobi->metadata.title == "Office Sample");
  CHECK(lists(*mobi).size() >= 2);
  CHECK(tables(*mobi).size() == 1);
  CHECK(pictures(*mobi).size() == 1);
  const auto azw3 = test::read_as<ir::Document>("azw3", data("calibre/sample.azw3"));
  REQUIRE_OK(azw3);
  CHECK(sample_words_found(*azw3) == std::size(sample_words));
  CHECK(has_heading(*azw3, 1, "Quarterly Widget Report"));
  CHECK(has_heading(*azw3, 2, "Inventory Table"));
  CHECK(tables(*azw3).size() == 1);
  CHECK(pictures(*azw3).size() == 1);
  CHECK(Registry::instance().find("azw3")->sniff(data("calibre/sample.azw3"), "") >= 95);
  CHECK(Registry::instance().find("azw3")->sniff(data("calibre/sample.mobi"), "") == 0);
}

TEST_CASE("office/mobi: writer round trip, calibre-compatible structure") {
  const ir::Document original = rich_document();
  const auto bytes = test::write_from("mobi", original);
  REQUIRE_OK(bytes);
  CHECK(test::str(*bytes).substr(60, 8) == "BOOKMOBI");
  const auto back = test::read_as<ir::Document>("mobi", *bytes);
  REQUIRE_OK(back);
  CHECK(test::text_similarity(original, *back) >= 0.95);
  CHECK(back->metadata.title == "Rich Sample");
  CHECK(has_heading(*back, 1, "Main Title"));
  // A long book exercises record splitting and multibyte overlaps.
  ir::Document longer;
  std::string paragraph;
  for (int i = 0; i < 400; ++i) paragraph += "Ærøskøbing αβγδ 汉字测试 🙂 ";
  for (int i = 0; i < 20; ++i) longer.blocks.push_back(ir::Paragraph{{ir::Text{paragraph}}});
  const auto long_bytes = test::write_from("mobi", longer);
  REQUIRE_OK(long_bytes);
  const auto long_back = test::read_as<ir::Document>("mobi", *long_bytes);
  REQUIRE_OK(long_back);
  CHECK(test::text_similarity(*long_back, longer) >= 0.999);
}

TEST_CASE("office/mobi: DRM and damage") {
  Bytes book = data("calibre/sample.mobi");
  // Record 0 starts at the first record offset; its encryption field is at +12.
  const std::size_t r0 = (std::to_integer<std::size_t>(book[78]) << 24) | (std::to_integer<std::size_t>(book[79]) << 16) |
                         (std::to_integer<std::size_t>(book[80]) << 8) | std::to_integer<std::size_t>(book[81]);
  book[r0 + 13] = std::byte{2};
  const auto drm = test::read_as<ir::Document>("mobi", book);
  CHECK_STATUS(drm, Status::Decode);
  CHECK(drm.error().message.find("DRM") != std::string::npos);
  CHECK(test::fuzz_reader<ir::Document>("mobi", data("calibre/sample.mobi"), 64) > 0);
  CHECK(test::fuzz_reader<ir::Document>("azw3", data("calibre/sample.azw3"), 64) > 0);
}

TEST_CASE("office/palm: PalmDOC and TCR books from calibre") {
  const auto pdb = test::read_as<ir::Document>("palmdoc", data("calibre/sample_palmdoc.pdb"));
  REQUIRE_OK(pdb);
  CHECK(plain(*pdb).find("Quarterly Widget Report") != std::string::npos);
  CHECK(plain(*pdb).find("Sprocket") != std::string::npos);
  const auto tcr = test::read_as<ir::Document>("tcr", data("calibre/sample.tcr"));
  REQUIRE_OK(tcr);
  CHECK(plain(*tcr).find("Quarterly Widget Report") != std::string::npos);
  CHECK(test::fuzz_reader<ir::Document>("tcr", data("calibre/sample.tcr"), 64) > 0);
  CHECK(test::fuzz_reader<ir::Document>("palmdoc", data("calibre/sample_palmdoc.pdb"), 64) > 0);
}

TEST_CASE("office/fbz: zipped FictionBook through the FB2 codec") {
  const auto& registry = Registry::instance();
  if (!registry.has_reader("fb2")) return;  // the document family provides FB2
  const auto doc = test::read_as<ir::Document>("fbz", data("pandoc/sample.fbz"));
  REQUIRE_OK(doc);
  CHECK(plain(*doc).find("Quarterly Widget Report") != std::string::npos);
  if (registry.has_writer("fb2")) {
    const auto bytes = test::write_from("fbz", *doc);
    REQUIRE_OK(bytes);
    const auto back = test::read_as<ir::Document>("fbz", *bytes);
    REQUIRE_OK(back);
    CHECK(test::text_similarity(*doc, *back) >= 0.95);
  }
}

TEST_CASE("office/comics: CBZ and CBT pages in natural order") {
  for (const char* name : {"archives/comic.cbz", "archives/comic.cbt"}) {
    INFO(name);
    const std::string format = std::string(name).ends_with("cbz") ? "cbz" : "cbt";
    const auto doc = test::read_as<ir::Document>(format, data(name));
    REQUIRE_OK(doc);
    CHECK(count<ir::PageBreak>(*doc) == 2);
    const auto image = test::read_as<ir::Image>(format, data(name));
    REQUIRE_OK(image);
    REQUIRE(image->frames.size() == 3);
    // page1 red, page2 green, page10 blue
    CHECK(image->frames[0].rgba[0] == 0xCC);
    CHECK(image->frames[1].rgba[1] == 0xCC);
    CHECK(image->frames[2].rgba[2] == 0xCC);
  }
  // Pages -> CBZ -> pages.
  const auto image = test::read_as<ir::Image>("cbz", data("archives/comic.cbz"));
  REQUIRE_OK(image);
  const auto cbz = test::write_from("cbz", *image, R"({"page_format":"png"})");
  REQUIRE_OK(cbz);
  const auto again = test::read_as<ir::Image>("cbz", *cbz);
  REQUIRE_OK(again);
  CHECK(test::pixels_equal(*image, *again));
  // Comics convert to e-books and documents through the Document kind.
  CHECK(test::convert(data("archives/comic.cbz"), "", "epub", {}, "comic.cbz").ok());
  CHECK_STATUS(test::read_as<ir::Document>("cbz", test::bytes("PK\x03\x04 nonsense")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Document>("cbt", data("archives/comic.cbt"), 64) > 0);
}
