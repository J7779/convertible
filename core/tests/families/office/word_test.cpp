// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/office/office_test_support.hpp"
#include "families/table/cfb.hpp"

using namespace cvt;
using namespace cvt::test::office;

TEST_CASE("office/docx: pandoc document structure") {
  const auto doc = test::read_as<ir::Document>("docx", data("pandoc/sample.docx"));
  REQUIRE_OK(doc);
  CHECK(has_heading(*doc, 1, "Quarterly Widget Report"));
  CHECK(has_heading(*doc, 2, "Lists and Steps"));
  CHECK(has_heading(*doc, 3, "Unicode Corner"));
  CHECK(sample_words_found(*doc) == std::size(sample_words));
  const auto ls = lists(*doc);
  REQUIRE(ls.size() >= 3);
  CHECK_FALSE(ls[0]->ordered);
  CHECK(ls[0]->items.size() == 3);
  CHECK(ls[0]->items[1].size() == 2);  // nested list inside the second item
  bool ordered = false;
  for (const ir::List* l : ls) ordered = ordered || (l->ordered && l->items.size() == 3);
  CHECK(ordered);
  const auto ts = tables(*doc);
  REQUIRE(ts.size() == 1);
  CHECK(ts[0]->rows.size() == 4);
  CHECK(ts[0]->rows[0].cells.size() == 3);
  CHECK(has_link(*doc, "https://example.com/widgets"));
  CHECK(has_link(*doc, "https://example.org/page"));
  CHECK(formatted<ir::Strong>(*doc, "eiusmod"));
  CHECK(formatted<ir::Emphasis>(*doc, "consectetur"));
  CHECK(formatted<ir::Strikethrough>(*doc, "struck"));
  CHECK(formatted<ir::Superscript>(*doc, "2"));
  CHECK(formatted<ir::Code>(*doc, "widget_count"));
  REQUIRE(doc->footnotes.size() == 1);
  CHECK(ir::plain_text(doc->footnotes[0].blocks).find("The note text lives at the bottom.") != std::string::npos);
  CHECK(count<ir::CodeBlock>(*doc) == 1);
  CHECK(count<ir::BlockQuote>(*doc) == 1);
  const auto pics = pictures(*doc);
  REQUIRE(pics.size() == 1);
  CHECK(picture_bytes(pics[0]) == data("base/picture.png").size());
  // Privacy by default: no author.
  CHECK(doc->metadata.author.empty());
}

TEST_CASE("office/docx: LibreOffice document with merged and nested cells") {
  const auto doc = test::read_as<ir::Document>("docx", data("libreoffice/sample.docx"));
  REQUIRE_OK(doc);
  CHECK(has_heading(*doc, 1, "Quarterly Widget Report"));
  CHECK(sample_words_found(*doc) >= std::size(sample_words) - 1);
  const auto ts = tables(*doc);
  REQUIRE(ts.size() >= 2);
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
  CHECK(plain(*doc).find("Inner two") != std::string::npos);  // nested table flattened into its cell
  CHECK(formatted<ir::Underline>(*doc, "labore"));
  CHECK(pictures(*doc).size() == 1);
}

TEST_CASE("office/docx: textutil document") {
  const auto doc = test::read_as<ir::Document>("docx", data("textutil/sample.docx"));
  REQUIRE_OK(doc);
  CHECK(sample_words_found(*doc) >= std::size(sample_words) - 1);
  // textutil writes neither tables nor links into DOCX; its big bold title reads as a heading.
  CHECK(has_heading(*doc, 1, "Quarterly Widget Report"));
  CHECK(formatted<ir::Emphasis>(*doc, "consectetur"));
}

TEST_CASE("office/docx: comments, headers and footers are options") {
  const Bytes bytes = data("python/comments.docx");
  const auto plain_doc = test::read_as<ir::Document>("docx", bytes);
  REQUIRE_OK(plain_doc);
  CHECK(has_heading(*plain_doc, 1, "Review Notes"));
  CHECK(plain(*plain_doc).find("Check the numbers") == std::string::npos);
  CHECK(plain(*plain_doc).find("Header line") == std::string::npos);
  const auto lists_found = lists(*plain_doc);
  REQUIRE(lists_found.size() == 1);
  CHECK(lists_found[0]->ordered);
  const auto with = test::read_as<ir::Document>("docx", bytes, R"({"include_comments":true,"include_headers":true})");
  REQUIRE_OK(with);
  REQUIRE(with->footnotes.size() == 1);
  CHECK(ir::plain_text(with->footnotes[0].blocks).find("Check the numbers again.") != std::string::npos);
  // Comment authors are personal data: only with strip_metadata off.
  CHECK(ir::plain_text(with->footnotes[0].blocks).find("Reviewer") == std::string::npos);
  CHECK(plain(*with).find("Header line") != std::string::npos);
  CHECK(plain(*with).find("Footer line") != std::string::npos);
  const auto authors = test::read_as<ir::Document>("docx", bytes, R"({"include_comments":true,"strip_metadata":false})");
  REQUIRE_OK(authors);
  REQUIRE(authors->footnotes.size() == 1);
  CHECK(ir::plain_text(authors->footnotes[0].blocks).find("Reviewer") != std::string::npos);
}

TEST_CASE("office/docx: writer round trip keeps structure") {
  const ir::Document original = rich_document();
  const auto bytes = test::write_from("docx", original);
  REQUIRE_OK(bytes);
  const auto names = zip_names(*bytes);
  CHECK(std::ranges::find(names, "word/footnotes.xml") != names.end());
  CHECK(std::ranges::find(names, "word/numbering.xml") != names.end());
  const auto back = test::read_as<ir::Document>("docx", *bytes);
  REQUIRE_OK(back);
  CHECK(test::text_similarity(original, *back) >= 0.97);
  CHECK(outline(*back) == outline(original));
  CHECK(formatted<ir::Strong>(*back, "bold"));
  CHECK(formatted<ir::Emphasis>(*back, "italic"));
  CHECK(formatted<ir::Underline>(*back, "underlined"));
  CHECK(formatted<ir::Strikethrough>(*back, "struck"));
  CHECK(formatted<ir::Superscript>(*back, "2"));
  CHECK(formatted<ir::Subscript>(*back, "2"));
  CHECK(formatted<ir::Code>(*back, "mono_code"));
  CHECK(has_link(*back, "https://example.com/a"));
  REQUIRE(back->footnotes.size() == 1);
  CHECK(ir::plain_text(back->footnotes[0].blocks).find("The footnote body.") != std::string::npos);
  const auto ls = lists(*back);
  REQUIRE(ls.size() == 3);
  CHECK_FALSE(ls[0]->ordered);
  CHECK(ls[1]->ordered);  // nested ordered list
  const auto ts = tables(*back);
  REQUIRE(ts.size() == 1);
  CHECK(ts[0]->rows[0].header);
  CHECK(ts[0]->rows[2].cells[0].colspan == 2);
  CHECK(count<ir::CodeBlock>(*back) == 1);
  CHECK(count<ir::BlockQuote>(*back) == 1);
  CHECK(count<ir::PageBreak>(*back) == 1);
  // Title kept, author not written by default.
  CHECK(back->metadata.title == "Rich Sample");
  CHECK(test::str(*bytes).find("Somebody") == std::string::npos);
  CHECK(zip_text(*bytes, "docProps/core.xml").find("Somebody") == std::string::npos);
  const auto kept = test::write_from("docx", original, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  CHECK(zip_text(*kept, "docProps/core.xml").find("Somebody Example") != std::string::npos);
}

TEST_CASE("office/docx: writer output is deterministic and honours page options") {
  const ir::Document doc = rich_document();
  const auto a = test::write_from("docx", doc);
  const auto b = test::write_from("docx", doc);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  CHECK(*a == *b);
  const auto letter = test::write_from("docx", doc, R"({"page_size":"letter","orientation":"landscape","margin_mm":10})");
  REQUIRE_OK(letter);
  const std::string xml = zip_text(*letter, "word/document.xml");
  CHECK(xml.find("w:w=\"15840\" w:h=\"12240\" w:orient=\"landscape\"") != std::string::npos);
  CHECK(xml.find("w:top=\"567\"") != std::string::npos);
  CHECK_STATUS(test::write_from("docx", doc, R"({"page_size":"tabloid"})"), Status::InvalidArgument);
}

TEST_CASE("office/docx: pictures are embedded with their size, dropped on request") {
  using namespace ir::build;
  ir::Document doc;
  ir::EncodedImage png;
  png.bytes = data("base/picture.png");
  png.mime = "image/png";
  doc.blocks.push_back(image_block(png, "Gradient swatch"));
  const auto bytes = test::write_from("docx", doc);
  REQUIRE_OK(bytes);
  const auto media_names = zip_names(*bytes);
  CHECK(std::ranges::find(media_names, "word/media/image1.png") != media_names.end());
  // 16x12 pixels at 96 dpi = 152400 x 114300 EMU.
  CHECK(zip_text(*bytes, "word/document.xml").find("cx=\"152400\" cy=\"114300\"") != std::string::npos);
  const auto back = test::read_as<ir::Document>("docx", *bytes);
  REQUIRE_OK(back);
  const auto pics = pictures(*back);
  REQUIRE(pics.size() == 1);
  CHECK(picture_bytes(pics[0]) == png.bytes.size());
  std::vector<std::string> warnings;
  const auto dropped = test::write_from("docx", doc, R"({"images":"drop"})", &warnings);
  REQUIRE_OK(dropped);
  CHECK(std::ranges::none_of(zip_names(*dropped), [](const std::string& n) { return n.starts_with("word/media/"); }));
  CHECK(warnings.size() == 1);
}

TEST_CASE("office/docx: compress shrinks picture-heavy documents and keeps the text") {
  const Bytes original = data("pandoc/photo.docx");
  const test::Converted compressed = test::convert(original, "docx", "docx", R"({"quality":60})");
  REQUIRE(compressed.ok());
  CHECK(compressed.data.size() < original.size() * 3 / 4);
  const auto before = test::read_as<ir::Document>("docx", original);
  const auto after = test::read_as<ir::Document>("docx", compressed.data);
  REQUIRE_OK(before);
  REQUIRE_OK(after);
  CHECK(test::text_similarity(*before, *after) >= 0.99);
  CHECK(pictures(*after).size() == 1);
  // The default (high quality) never makes a JPEG worse.
  const test::Converted same = test::convert(original, "docx", "docx");
  REQUIRE(same.ok());
  CHECK(same.data.size() <= original.size());
  // max_image_size downscales.
  const test::Converted small = test::convert(original, "docx", "docx", R"({"max_image_size":64})");
  REQUIRE(small.ok());
  CHECK(small.data.size() < compressed.data.size());
}

TEST_CASE("office/wordml: Word 2003 XML from textutil and round trip") {
  const auto doc = test::read_as<ir::Document>("wordml", data("textutil/sample.wordml"));
  REQUIRE_OK(doc);
  CHECK(sample_words_found(*doc) >= std::size(sample_words) - 1);
  CHECK(formatted<ir::Strong>(*doc, "eiusmod"));
  const ir::Document original = rich_document();
  const auto bytes = test::write_from("wordml", original);
  REQUIRE_OK(bytes);
  CHECK(test::str(*bytes).find("<?mso-application progid=\"Word.Document\"?>") != std::string::npos);
  const auto back = test::read_as<ir::Document>("wordml", *bytes);
  REQUIRE_OK(back);
  CHECK(test::text_similarity(original, *back) >= 0.97);
  CHECK(outline(*back) == outline(original));
  CHECK(back->footnotes.size() == 1);
  CHECK(lists(*back).size() == 3);
}

TEST_CASE("office/wordml: flat OPC packages read like DOCX") {
  // Build the flat form of our own DOCX: every part inline.
  const auto docx = test::write_from("docx", rich_document());
  REQUIRE_OK(docx);
  auto zip = util::ZipReader::open(*docx);
  REQUIRE_OK(zip);
  std::string flat = "<?xml version=\"1.0\" standalone=\"yes\"?>\n<?mso-application progid=\"Word.Document\"?>\n"
                     "<pkg:package xmlns:pkg=\"http://schemas.microsoft.com/office/2006/xmlPackage\">";
  for (std::size_t i = 0; i < zip->size(); ++i) {
    const std::string name = zip->entries()[i].name;
    if (name == "[Content_Types].xml") continue;
    auto bytes = zip->read(i);
    REQUIRE_OK(bytes);
    std::string text(as_chars(*bytes));
    if (text.starts_with("<?xml")) text.erase(0, text.find("?>") + 2);
    flat += "<pkg:part pkg:name=\"/" + name + "\"><pkg:xmlData>" + text + "</pkg:xmlData></pkg:part>";
  }
  flat += "</pkg:package>";
  const auto doc = test::read_as<ir::Document>("wordml", test::bytes(flat));
  REQUIRE_OK(doc);
  CHECK(outline(*doc) == outline(rich_document()));
  CHECK(doc->footnotes.size() == 1);
  const test::Converted detected = test::convert(flat, "", "md", {}, "flat.xml");
  CHECK(detected.ok());
  CHECK(detected.format_id == "md");
}

TEST_CASE("office/docx: detection of the DOCX family") {
  const Bytes docx = data("pandoc/sample.docx");
  CHECK(test::convert(docx, "", "txt", {}, "file.bin").ok());
  const test::Converted plain_text = test::convert(docx, "", "txt");
  REQUIRE(plain_text.ok());
  CHECK(plain_text.text().find("Quarterly Widget Report") != std::string::npos);
  const auto& registry = Registry::instance();
  const FormatInfo* info = registry.find("docx");
  REQUIRE(info != nullptr);
  CHECK(info->sniff(docx, "a.docx") >= 90);
  CHECK(registry.find("dotx")->sniff(docx, "a.dotx") == 0);  // the content type says "document"
  CHECK(registry.find("docm")->sniff(docx, "a.docx") == 0);
  CHECK(info->sniff(data("pandoc/sample.odt"), "a.odt") == 0);
}

TEST_CASE("office/docx: encrypted and damaged packages fail cleanly") {
  table::CfbWriter writer;
  REQUIRE_OK(writer.add_stream("EncryptionInfo", test::bytes("info")));
  REQUIRE_OK(writer.add_stream("EncryptedPackage", test::bytes("secret")));
  const auto encrypted = writer.finish();
  REQUIRE_OK(encrypted);
  const auto result = test::read_as<ir::Document>("docx", *encrypted);
  CHECK_STATUS(result, Status::Decode);
  CHECK(result.error().message.find("password") != std::string::npos);
  const Bytes valid = data("pandoc/sample.docx");
  CHECK_STATUS(test::read_as<ir::Document>("docx", ByteView(valid).first(valid.size() / 2)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Document>("docx", data("textutil/sample.docx"), 64) > 0);
  CHECK(test::fuzz_reader<ir::Document>("wordml", data("textutil/sample.wordml"), 64) > 0);
}

TEST_CASE("office/docx: hostile XML and recursion are bounded") {
  // Billion laughs inside document.xml.
  std::string laughs = "<?xml version=\"1.0\"?><!DOCTYPE w:document [<!ENTITY a \"aaaaaaaaaa\">";
  for (int i = 1; i < 10; ++i) {
    laughs += "<!ENTITY " + std::string(1, static_cast<char>('a' + i)) + " \"";
    for (int k = 0; k < 10; ++k) laughs += "&" + std::string(1, static_cast<char>('a' + i - 1)) + ";";
    laughs += "\">";
  }
  laughs += "]><w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p><w:r><w:t>&j;</w:t>"
            "</w:r></w:p></w:body></w:document>";
  util::ZipWriter zip;
  REQUIRE_OK(zip.add("word/document.xml", laughs));
  // Deeply nested tables.
  std::string deep = "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>";
  for (int i = 0; i < 100; ++i) deep += "<w:tbl><w:tr><w:tc>";
  deep += "<w:p><w:r><w:t>deep</w:t></w:r></w:p>";
  for (int i = 0; i < 100; ++i) deep += "</w:tc></w:tr></w:tbl>";
  deep += "</w:body></w:document>";
  const auto bomb = zip.finish();
  REQUIRE_OK(bomb);
  CHECK_FALSE(test::read_as<ir::Document>("docx", *bomb).has_value());
  util::ZipWriter zip2;
  REQUIRE_OK(zip2.add("word/document.xml", deep));
  const auto deep_zip = zip2.finish();
  REQUIRE_OK(deep_zip);
  CHECK(test::read_as<ir::Document>("docx", *deep_zip).has_value());
  // A relationship cycle (a part relating to itself) does not loop.
  util::ZipWriter zip3;
  REQUIRE_OK(zip3.add("word/document.xml", "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p/></w:body></w:document>"));
  REQUIRE_OK(zip3.add("word/_rels/document.xml.rels",
                      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"r1\" "
                      "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes\" Target=\"document.xml\"/>"
                      "</Relationships>"));
  const auto cyclic = zip3.finish();
  REQUIRE_OK(cyclic);
  CHECK(test::read_as<ir::Document>("docx", *cyclic).has_value());
  // A decompression bomb is stopped by the limits.
  Limits limits;
  limits.max_decompressed_bytes = 1000;
  CHECK_STATUS(test::read_as<ir::Document>("docx", data("pandoc/sample.docx"), {}, nullptr, limits), Status::Limit);
}

TEST_CASE("office/docx: pictures can be extracted as an archive") {
  const auto archive = test::read_as<ir::Archive>("docx", data("pandoc/photo.docx"));
  REQUIRE_OK(archive);
  REQUIRE(archive->entries.size() == 1);
  CHECK(archive->entries[0].path == "image-001.jpg");
  CHECK(archive->entries[0].bytes == data("base/photo.jpg"));
}
