// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// MSG, TNEF, HWP 5, HWPX and VSDX inputs are assembled here from their
// specifications (no independent writer for them exists); the same bytes
// were checked with independent readers as recorded in SOURCES.md.
#include "families/office/office.hpp"
#include "families/office/office_test_support.hpp"
#include "families/table/cfb.hpp"

using namespace cvt;
using namespace cvt::test::office;

namespace {

Bytes utf16(std::string_view ascii) {
  Bytes out;
  for (char c : ascii) {
    out.push_back(static_cast<std::byte>(c));
    out.push_back(std::byte{0});
  }
  return out;
}

// A minimal Outlook message: subject, sender, recipients, plain and HTML
// bodies, one attachment referenced from the HTML by content id.
Bytes make_msg() {
  table::CfbWriter w;
  Bytes props(32);  // top-level property stream header
  // PR_CLIENT_SUBMIT_TIME (0x0039, PT_SYSTIME): 2020-01-02T03:04:05Z
  const std::uint64_t filetime = (1577934245ULL + 11644473600ULL) * 10000000ULL;
  util::ByteWriter entry;
  entry.u32le(0x00390040u).u32le(6).u64le(filetime);
  append(props, entry.view());
  (void)w.add_stream("__properties_version1.0", props);
  (void)w.add_stream("__nameid_version1.0/__substg1.0_00020102", Bytes{});  // named-property map (empty)
    (void)w.add_stream("__nameid_version1.0/__substg1.0_00030102", Bytes{});
    (void)w.add_stream("__nameid_version1.0/__substg1.0_00040102", Bytes{});
    (void)w.add_stream("__substg1.0_0037001F", utf16("Widget order"));
  (void)w.add_stream("__substg1.0_0C1A001F", utf16("Sample Sender"));
  (void)w.add_stream("__substg1.0_5D01001F", utf16("sender@example.com"));
  (void)w.add_stream("__substg1.0_0E04001F", utf16("Sample Receiver"));
  (void)w.add_stream("__substg1.0_1000001F", utf16("Plain body text."));
  (void)w.add_stream("__substg1.0_10130102",
                     test::bytes("<html><body><h2>Order</h2><p>Please send <b>four</b> gears.</p>"
                                 "<p><img src=\"cid:swatch\" alt=\"Swatch\"></p></body></html>"));
  const Bytes png = data("base/picture.png");
  (void)w.add_stream("__attach_version1.0_#00000000/__properties_version1.0", Bytes(8));
  (void)w.add_stream("__attach_version1.0_#00000000/__substg1.0_3707001F", utf16("swatch.png"));
  (void)w.add_stream("__attach_version1.0_#00000000/__substg1.0_3712001F", utf16("swatch"));
  (void)w.add_stream("__attach_version1.0_#00000000/__substg1.0_37010102", png);
  (void)w.add_stream("__attach_version1.0_#00000001/__properties_version1.0", Bytes(8));
  (void)w.add_stream("__attach_version1.0_#00000001/__substg1.0_3707001F", utf16("notes.txt"));
  (void)w.add_stream("__attach_version1.0_#00000001/__substg1.0_37010102", test::bytes("attached text"));
  auto file = w.finish();
  return file ? std::move(*file) : Bytes{};
}

void tnef_attribute(util::ByteWriter& out, std::uint8_t level, std::uint32_t id, ByteView data) {
  out.u8(level).u32le(id).u32le(static_cast<std::uint32_t>(data.size())).bytes(data);
  std::uint16_t sum = 0;
  for (std::byte b : data) sum = static_cast<std::uint16_t>(sum + std::to_integer<std::uint16_t>(b));
  out.u16le(sum);
}

Bytes make_tnef() {
  util::ByteWriter out;
  out.u32le(0x223E9F78u).u16le(0x0001);
  tnef_attribute(out, 1, 0x00078004u, test::bytes(std::string_view("Quarterly widgets\0", 18)));
  tnef_attribute(out, 1, 0x0002800Cu, test::bytes(std::string_view("TNEF body text.\0", 16)));
  tnef_attribute(out, 2, 0x00069002u, Bytes(14));
  tnef_attribute(out, 2, 0x00018010u, test::bytes(std::string_view("report.txt\0", 11)));
  tnef_attribute(out, 2, 0x0006800Fu, test::bytes("report contents"));
  return out.take();
}

Bytes make_hwp() {
  table::CfbWriter w;
  Bytes header(256);
  const std::string_view sig = "HWP Document File";
  for (std::size_t i = 0; i < sig.size(); ++i) header[i] = static_cast<std::byte>(sig[i]);
  header[36] = std::byte{1};  // compressed
  (void)w.add_stream("FileHeader", header);
  util::ByteWriter records;
  for (std::string_view line : {"First Hangul paragraph.", "Second paragraph."}) {
    Bytes text = utf16(line);
    text.push_back(std::byte{13});
    text.push_back(std::byte{0});
    records.u32le(67u | (static_cast<std::uint32_t>(text.size()) << 20)).bytes(text);
  }
  auto deflated = util::deflate_raw(records.view());
  (void)w.add_stream("BodyText/Section0", deflated ? *deflated : Bytes{});
  auto file = w.finish();
  return file ? std::move(*file) : Bytes{};
}

Bytes make_zip(std::initializer_list<std::pair<std::string_view, std::string_view>> parts) {
  util::ZipWriter zip;
  for (const auto& [name, text] : parts) (void)zip.add(name, text);
  auto out = zip.finish();
  return out ? std::move(*out) : Bytes{};
}

}  // namespace

TEST_CASE("office/msg: headers, HTML body with inline picture, attachments") {
  const Bytes msg = make_msg();
  CHECK(Registry::instance().find("msg")->sniff(msg, "") >= 95);
  const auto doc = test::read_as<ir::Document>("msg", msg);
  REQUIRE_OK(doc);
  CHECK(has_heading(*doc, 1, "Widget order"));
  const std::string text = plain(*doc);
  CHECK(text.find("Sample Sender <sender@example.com>") != std::string::npos);
  CHECK(text.find("To: Sample Receiver") != std::string::npos);
  CHECK(text.find("2020-01-02T03:04:05Z") != std::string::npos);
  CHECK(has_heading(*doc, 2, "Order"));
  CHECK(formatted<ir::Strong>(*doc, "four"));
  const auto pics = pictures(*doc);
  REQUIRE(pics.size() == 1);
  CHECK(picture_bytes(pics[0]) == data("base/picture.png").size());
  CHECK(text.find("swatch.png, notes.txt") != std::string::npos);
  const auto archive = test::read_as<ir::Archive>("msg", msg);
  REQUIRE_OK(archive);
  REQUIRE(archive->entries.size() == 2);
  CHECK(archive->entries[0].path == "swatch.png");
  CHECK(archive->entries[1].path == "notes.txt");
  CHECK(test::str(archive->entries[1].bytes) == "attached text");
  CHECK(test::fuzz_reader<ir::Document>("msg", msg, 64) > 0);
}

TEST_CASE("office/msg: compressed RTF bodies") {
  // The example of [MS-OXRTFCP] section 3.1.1: "{\rtf1\ansi\ansicpg1252\pard hello world}\r\n".
  const Bytes compressed = test::bytes(std::string_view(
      "\x2D\x00\x00\x00\x2B\x00\x00\x00\x4C\x5A\x46\x75\xF1\xC5\xC7\xA7\x03\x00\x0A\x00\x72\x63\x70\x67\x31\x32\x35\x42"
      "\x32\x0A\xF3\x20\x68\x65\x6C\x09\x00\x20\x62\x77\x05\xB0\x6C\x64\x7D\x0A\x80\x0F\xA0",
      49));
  const auto rtf = office::decompress_compressed_rtf(compressed, 1024);
  REQUIRE_OK(rtf);
  CHECK(*rtf == "{\\rtf1\\ansi\\ansicpg1252\\pard hello world}\r\n");
}

TEST_CASE("office/tnef: subject, body and attachments") {
  const Bytes tnef = make_tnef();
  const auto doc = test::read_as<ir::Document>("tnef", tnef);
  REQUIRE_OK(doc);
  CHECK(has_heading(*doc, 1, "Quarterly widgets"));
  CHECK(plain(*doc).find("TNEF body text.") != std::string::npos);
  const auto archive = test::read_as<ir::Archive>("tnef", tnef);
  REQUIRE_OK(archive);
  REQUIRE(archive->entries.size() == 1);
  CHECK(archive->entries[0].path == "report.txt");
  CHECK(test::str(archive->entries[0].bytes) == "report contents");
  CHECK(test::fuzz_reader<ir::Document>("tnef", tnef, 64) > 0);
}

TEST_CASE("office/xps: reading order, headings, pictures and pages") {
  const auto doc = test::read_as<ir::Document>("xps", data("handmade/sample.xps"));
  REQUIRE_OK(doc);
  CHECK(has_heading(*doc, 1, "Quarterly Widget Report"));
  const std::string text = plain(*doc);
  CHECK(text.find("Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt.") != std::string::npos);
  CHECK(text.find("Text inside a moved canvas.") < text.find("Text below the picture."));
  CHECK(count<ir::PageBreak>(*doc) == 1);
  CHECK(pictures(*doc).size() == 1);
  CHECK(Registry::instance().find("xps")->sniff(data("handmade/sample.xps"), "") >= 90);
  CHECK(Registry::instance().find("oxps")->sniff(data("handmade/sample.xps"), "a.oxps") >= 90);
  CHECK(test::fuzz_reader<ir::Document>("xps", data("handmade/sample.xps"), 64) > 0);
}

TEST_CASE("office/abw: AbiWord document") {
  const auto doc = test::read_as<ir::Document>("abw", data("abiword/sample.abw"));
  REQUIRE_OK(doc);
  CHECK(has_heading(*doc, 1, "Quarterly Widget Report"));
  CHECK(sample_words_found(*doc) >= std::size(sample_words) - 2);
  const auto ls = lists(*doc);
  REQUIRE(ls.size() >= 1);
  CHECK(ls[0]->items.size() >= 3);  // AbiWord merges the bullet and numbered lists of the source into one
  CHECK(tables(*doc).size() >= 1);
  CHECK(formatted<ir::Strong>(*doc, "eiusmod"));
  // The gzip-compressed variant.
  auto zabw = util::gzip_compress(data("abiword/sample.abw"));
  REQUIRE_OK(zabw);
  const auto z = test::read_as<ir::Document>("abw", *zabw);
  REQUIRE_OK(z);
  CHECK(test::text_similarity(*doc, *z) == 1.0);
  CHECK(test::fuzz_reader<ir::Document>("abw", data("abiword/sample.abw"), 32) > 0);
}

TEST_CASE("office/hwp: HWP 5 and HWPX text") {
  const auto hwp = test::read_as<ir::Document>("hwp", make_hwp());
  REQUIRE_OK(hwp);
  CHECK(plain(*hwp) == "First Hangul paragraph.\n\nSecond paragraph.\n");
  const Bytes hwpx = make_zip({{"mimetype", "application/hwp+zip"},
                               {"Contents/section0.xml",
                                "<hs:sec xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/section\" "
                                "xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\"><hp:p><hp:run><hp:t>Hello HWPX"
                                "</hp:t></hp:run></hp:p><hp:p><hp:run><hp:tbl><hp:tr><hp:tc><hp:subList><hp:p><hp:run><hp:t>Cell"
                                "</hp:t></hp:run></hp:p></hp:subList></hp:tc></hp:tr></hp:tbl></hp:run></hp:p></hs:sec>"}});
  const auto doc = test::read_as<ir::Document>("hwpx", hwpx);
  REQUIRE_OK(doc);
  CHECK(plain(*doc).find("Hello HWPX") != std::string::npos);
  CHECK(tables(*doc).size() == 1);
  CHECK(test::fuzz_reader<ir::Document>("hwp", make_hwp(), 64) > 0);
}

TEST_CASE("office/vsdx: shape text page by page") {
  const Bytes vsdx = make_zip(
      {{"visio/pages/pages.xml",
        "<Pages xmlns=\"http://schemas.microsoft.com/office/visio/2012/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><Page ID=\"0\" Name=\"Flow\">"
        "<Rel r:id=\"rId1\"/></Page></Pages>"},
       {"visio/pages/_rels/pages.xml.rels",
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
        "Type=\"http://schemas.microsoft.com/visio/2010/relationships/page\" Target=\"page1.xml\"/></Relationships>"},
       {"visio/pages/page1.xml",
        "<PageContents xmlns=\"http://schemas.microsoft.com/office/visio/2012/main\"><Shapes><Shape ID=\"1\"><Text>Start"
        "</Text></Shape><Shape ID=\"2\"><Text>Finish</Text></Shape></Shapes></PageContents>"}});
  const auto doc = test::read_as<ir::Document>("vsdx", vsdx);
  REQUIRE_OK(doc);
  CHECK(has_heading(*doc, 1, "Flow"));
  CHECK(plain(*doc).find("Start") < plain(*doc).find("Finish"));
}

TEST_CASE("office: CHM and LIT are recognised; registry stays valid") {
  const auto& registry = Registry::instance();
  CHECK(registry.find("chm") != nullptr);
  CHECK(registry.find("lit") != nullptr);
  // No consistency problem involves an office format.
  for (const std::string& problem : registry.validate()) {
    INFO(problem);
    for (const char* id : {"'docx'", "'doc'", "'rtf'", "'odt'", "'pptx'", "'epub'", "'mobi'", "'xps'", "'msg'", "'cbz'"}) {
      CHECK(problem.find(id) == std::string::npos);
    }
  }
  for (const char* id : {"docx", "doc", "rtf", "odt", "fodt", "wordml", "pptx", "ppt", "odp", "epub", "mobi", "azw3", "palmdoc",
                         "tcr", "cbz", "cbt", "xps", "msg", "tnef", "abw", "hwp", "hwpx", "vsdx"}) {
    INFO(id);
    CHECK(registry.has_reader(id));
  }
  for (const char* id : {"docx", "rtf", "odt", "fodt", "wordml", "pptx", "odp", "epub", "mobi", "cbz"}) {
    INFO(id);
    CHECK(registry.has_writer(id));
  }
}
