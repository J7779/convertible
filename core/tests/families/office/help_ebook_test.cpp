// Office family: Compiled HTML Help (.chm) and Microsoft Reader (.lit) as documents.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/office/office_test_support.hpp"
#include "util/text.hpp"

namespace cvt {

using namespace test::office;

namespace {

/// Every in-document link ("#id") points at a block id that exists.
bool internal_links_resolve(const ir::Document& d) {
  std::set<std::string> ids;
  walk(d.blocks, [&](const ir::Block& b) {
    if (!b.attr.id.empty()) ids.insert(b.attr.id);
  });
  for (const std::string& url : links(d)) {
    if (url.starts_with('#') && !ids.contains(url.substr(1))) return false;
  }
  return true;
}

bool has_id(const ir::Document& d, std::string_view id) {
  bool found = false;
  walk(d.blocks, [&](const ir::Block& b) { found = found || b.attr.id == id; });
  return found;
}

std::string calibre_text(std::string_view name) { return test::str(data(name)); }

// ----------------------------------------------------------------- a LIT file built from the format description

void put32(Bytes& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
void put_text(Bytes& out, std::string_view text) { append(out, text); }
void put_encint(Bytes& out, std::uint64_t v) {
  Bytes tmp;
  do {
    tmp.insert(tmp.begin(), static_cast<std::byte>(v & 0x7F));
    v >>= 7;
  } while (v != 0);
  for (std::size_t i = 0; i + 1 < tmp.size(); ++i) tmp[i] |= std::byte{0x80};
  append(out, tmp);
}
/// One character of binary markup (the format's UTF-8 style numbers).
void put_char(Bytes& out, std::uint32_t c) {
  std::string s;
  if (c < 0x80) {
    s.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    s.push_back(static_cast<char>(0xC0 | (c >> 6)));
    s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    s.push_back(static_cast<char>(0xE0 | (c >> 12)));
    s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
  append(out, s);
}
void put_sized(Bytes& out, std::string_view text) {
  put_char(out, static_cast<std::uint32_t>(text.size()));
  append(out, text);
}

/// A Microsoft Reader container whose files all live in the stored section:
/// header, two pieces, secondary header (CAOL + ITSF), IFCM directory with one
/// AOLL chunk, content.
Bytes make_lit(const std::vector<std::pair<std::string, Bytes>>& files) {
  constexpr std::uint32_t chunk_size = 2048;
  Bytes entries;
  Bytes content;
  for (const auto& [name, bytes] : files) {
    put_encint(entries, name.size());
    put_text(entries, name);
    put_encint(entries, 0);
    put_encint(entries, content.size());
    put_encint(entries, bytes.size());
    append(content, bytes);
  }
  Bytes chunk;
  put_text(chunk, "AOLL");
  put32(chunk, static_cast<std::uint32_t>(chunk_size - 48 - entries.size()));
  chunk.resize(48);
  append(chunk, entries);
  chunk.resize(chunk_size);
  Bytes dir;
  put_text(dir, "IFCM");
  put32(dir, 1);
  put32(dir, chunk_size);
  put32(dir, 0);
  put32(dir, 0xFFFFFFFF);
  put32(dir, 0xFFFFFFFF);
  put32(dir, 1);
  put32(dir, 0);
  append(dir, chunk);
  Bytes secondary;
  put32(secondary, 2);
  put32(secondary, 8);
  put_text(secondary, "CAOL");
  put32(secondary, 2);
  secondary.resize(8 + 48);
  put_text(secondary, "ITSF");
  put32(secondary, 4);
  put32(secondary, 0x20);
  put32(secondary, 1);
  const std::uint32_t header = 0x28;
  const std::uint32_t pieces_at = header;
  const std::uint32_t secondary_at = pieces_at + 2 * 16;
  const std::uint32_t secondary_len = static_cast<std::uint32_t>(secondary.size() + 8);
  const std::uint32_t dir_at = secondary_at + secondary_len;
  const std::uint32_t content_at = dir_at + static_cast<std::uint32_t>(dir.size());
  put32(secondary, content_at);
  put32(secondary, 0);
  Bytes out;
  put_text(out, "ITOLITLS");
  put32(out, 1);
  put32(out, header);
  put32(out, 2);
  put32(out, secondary_len);
  out.resize(header);
  for (const auto [at, size] : {std::pair<std::uint32_t, std::uint32_t>{0, 0}, {dir_at, static_cast<std::uint32_t>(dir.size())}}) {
    put32(out, at);
    put32(out, 0);
    put32(out, size);
    put32(out, 0);
  }
  append(out, secondary);
  append(out, dir);
  append(out, content);
  return out;
}

Bytes lit_manifest() {
  Bytes m;
  m.push_back(std::byte{1});
  put_text(m, "/");
  put32(m, 1);  // pages in reading order
  put32(m, 0);
  put_sized(m, "p1");
  put_sized(m, "page1.html");
  put_sized(m, "text/html");
  m.push_back(std::byte{0});
  for (int group = 1; group < 4; ++group) put32(m, 0);
  m.push_back(std::byte{0});
  return m;
}

// Binary markup helpers: <tag attr=value>, text, </tag>.
void open_tag(Bytes& b, std::uint32_t tag, const std::vector<std::pair<std::uint32_t, std::string>>& attributes = {},
              bool empty = false) {
  put_char(b, 0);
  put_char(b, empty ? 3 : 1);
  put_char(b, tag);
  for (const auto& [code, value] : attributes) {
    put_char(b, code);
    put_char(b, static_cast<std::uint32_t>(value.size() + 1));
    append(b, value);
  }
  put_char(b, 0);
}
void close_tag(Bytes& b) {
  put_char(b, 0);
  put_char(b, 2);
  put_char(b, 0x7F);
}

/// <html><body><h1 id="top">Hand Made</h1><p>One &lt;two&gt; <a href="\x02#top">back</a> <b>bold</b></p></body></html>
Bytes lit_page() {
  Bytes b;
  open_tag(b, 51);  // html
  open_tag(b, 16);  // body
  open_tag(b, 42, {{0x83EB, "top"}});
  append(b, "Hand Made");
  close_tag(b);
  open_tag(b, 75);  // p
  append(b, "One <two> & three ");
  open_tag(b, 3, {{0x0001, std::string("\x02") + "#top"}});
  append(b, "back");
  close_tag(b);
  append(b, " ");
  open_tag(b, 8);  // b
  append(b, "bold");
  close_tag(b);
  close_tag(b);
  close_tag(b);
  close_tag(b);
  return b;
}

Bytes handmade_lit(std::vector<std::pair<std::string, Bytes>> extra = {}, Bytes page = lit_page()) {
  std::vector<std::pair<std::string, Bytes>> files = {{"/manifest", lit_manifest()}, {"/data/p1/content", std::move(page)}};
  for (auto& f : extra) files.push_back(std::move(f));
  return make_lit(files);
}

}  // namespace

// =========================================================================== CHM

TEST_CASE("office/chm: topics in contents order, headings from the contents, links and pictures") {
  const Bytes chm = data("chmcmd/sample.chm");
  const auto doc = test::read_as<ir::Document>("chm", chm);
  REQUIRE_OK(doc);
  CHECK(outline(*doc) == std::vector<std::string>{"1:Introduction", "1:Products", "2:Widgets", "3:Pricing",
                                                  "2:Gears and Sprockets", "1:Appendix"});
  // Links between topics become in-document anchors (also the <a name> one).
  CHECK(has_link(*doc, "#pricing"));
  CHECK(has_link(*doc, "#topic-gears"));
  CHECK(has_link(*doc, "#topic-intro"));
  CHECK(has_link(*doc, "#topic-orphan"));
  CHECK(internal_links_resolve(*doc));
  // The topic only reachable through a link is included once, at the end.
  const std::string text = plain(*doc);
  CHECK(text.find("Nemo enim ipsam") != std::string::npos);
  CHECK(text.find("Nemo enim ipsam") == text.rfind("Nemo enim ipsam"));
  CHECK(text.find("Café prices") != std::string::npos);
  const auto pics = pictures(*doc);
  REQUIRE(pics.size() == 1);
  CHECK(picture_bytes(pics[0]) == data("base/chm/images/logo.png").size());
  REQUIRE(tables(*doc).size() == 1);
  CHECK(tables(*doc)[0]->rows.size() == 3);
  CHECK(formatted<ir::Strong>(*doc, "widget"));
  // Title and language from #SYSTEM (both are content, kept by default).
  CHECK(doc->metadata.title == "Widget Help Sample");
  CHECK(doc->metadata.language == "en-US");
  CHECK(doc->metadata.extra.empty());
  // Calibre reads the same text (it leaves out the contents' folder headings).
  const double similarity = test::text_similarity(text, calibre_text("calibre/sample_chm.txt"));
  INFO(similarity);
  CHECK(similarity >= 0.9);
}

TEST_CASE("office/chm: the locale's code page decodes pages without a charset") {
  const auto doc = test::read_as<ir::Document>("chm", data("chmcmd/cyrillic.chm"), R"({"strip_metadata":false})");
  REQUIRE_OK(doc);
  CHECK(outline(*doc) == std::vector<std::string>{"1:Шестерёнки"});
  CHECK(plain(*doc).find("Съешь же ещё этих мягких французских булок") != std::string::npos);
  CHECK(doc->metadata.title == "Справка");
  CHECK(doc->metadata.language == "ru-RU");
  CHECK(doc->metadata.extra.at("generator").starts_with("HHA Version"));
}

TEST_CASE("office/chm: options") {
  const Bytes chm = data("chmcmd/sample.chm");
  const auto plain_headings = test::read_as<ir::Document>("chm", chm, R"({"toc_titles":false})");
  REQUIRE_OK(plain_headings);
  CHECK(outline(*plain_headings) == std::vector<std::string>{"1:Introduction", "1:Widgets", "2:Pricing", "1:Gears",
                                                          "1:Appendix"});
  const auto paged = test::read_as<ir::Document>("chm", chm, R"({"chapter_breaks":true})");
  REQUIRE_OK(paged);
  CHECK(count<ir::PageBreak>(*paged) == 4);  // before the folder heading and the next three topics
  const auto unpaged = test::read_as<ir::Document>("chm", chm);
  REQUIRE_OK(unpaged);
  CHECK(count<ir::PageBreak>(*unpaged) == 0);
}

TEST_CASE("office/chm: one format id, two readers; conversions pick the right one") {
  const Registry& registry = Registry::instance();
  const FormatInfo* info = registry.find("chm");
  REQUIRE(info != nullptr);
  bool document = false;
  bool archive = false;
  for (const Codec* codec : registry.codecs_for("chm")) {
    document = document || (codec->kind == Kind::Document && codec->read);
    archive = archive || (codec->kind == Kind::Archive && codec->read);
  }
  CHECK(document);
  const Bytes chm = data("chmcmd/sample.chm");
  const auto detected = detect_format(registry, chm, "help.chm");
  REQUIRE(detected.has_value());
  CHECK(detected->format->id == "chm");
  CHECK(detect_format(registry, chm, "")->format->id == "chm");
  for (const char* target : {"md", "html", "epub"}) {
    INFO(target);
    const auto out = test::convert(chm, "", target, {}, "help.chm");
    REQUIRE(out.ok());
    if (std::string_view(target) != "epub") CHECK(out.text().find("Gears and Sprockets") != std::string::npos);
  }
  if (registry.has_writer("pdf")) CHECK(test::convert(chm, "chm", "pdf").ok());
  if (archive && registry.has_writer("zip")) {
    // The archive family's reader: the help file's own files.
    const auto zip = test::convert(chm, "chm", "zip");
    REQUIRE(zip.ok());
    const auto names = zip_names(zip.data);
    CHECK(std::ranges::find(names, "topics/widgets.htm") != names.end());
    CHECK(std::ranges::find(names, "images/logo.png") != names.end());
  }
}

TEST_CASE("office/chm: damaged and hostile files") {
  const Bytes chm = data("chmcmd/sample.chm");
  CHECK(test::fuzz_reader<ir::Document>("chm", chm, 300) > 0);
  CHECK_STATUS(test::read_as<ir::Document>("chm", test::bytes("ITSF")), Status::Decode);
  // Decompression limit.
  Limits small;
  small.max_decompressed_bytes = 1000;
  CHECK_STATUS(test::read_as<ir::Document>("chm", chm, {}, nullptr, small), Status::Limit);
  // An absurd directory chunk count.
  Bytes absurd = chm;
  const std::size_t dir = 0x78;  // ITSP of chmcmd files follows the 0x60 header and section 0 table
  if (absurd.size() > dir + 48 && test::str(ByteView(absurd).subspan(dir, 4)) == "ITSP") {
    absurd[dir + 44] = std::byte{0xFF};
    absurd[dir + 45] = std::byte{0xFF};
    absurd[dir + 46] = std::byte{0xFF};
    CHECK_STATUS(test::read_as<ir::Document>("chm", absurd), Status::Decode);
  }
}

// =========================================================================== LIT

TEST_CASE("office/lit: a Microsoft Reader book written by Calibre") {
  const Bytes lit = data("calibre/sample.lit");
  const auto doc = test::read_as<ir::Document>("lit", lit, R"({"strip_metadata":false})");
  REQUIRE_OK(doc);
  CHECK(outline(*doc) == std::vector<std::string>{"1:Quarterly Widget Report", "2:Lists and Steps", "2:Inventory Table",
                                                  "1:Appendix", "3:Unicode Corner"});
  // Tables with spans, nested lists, pictures, emphasis, code, quotes.
  REQUIRE(tables(*doc).size() == 1);
  const ir::TableBlock& table = *tables(*doc)[0];
  REQUIRE(table.rows.size() == 5);
  CHECK(table.rows[2].cells[0].colspan == 2);
  CHECK(table.rows[3].cells[0].rowspan == 2);
  CHECK(lists(*doc).size() >= 3);
  const auto pics = pictures(*doc);
  REQUIRE(pics.size() == 1);
  CHECK(picture_bytes(pics[0]) == data("base/picture.png").size());
  CHECK(formatted<ir::Emphasis>(*doc, "consectetur"));
  CHECK(formatted<ir::Strong>(*doc, "eiusmod"));
  CHECK(formatted<ir::Superscript>(*doc, "2"));
  CHECK(formatted<ir::Code>(*doc, "widget_count"));
  CHECK(count<ir::CodeBlock>(*doc) == 1);
  CHECK(count<ir::BlockQuote>(*doc) == 1);
  const std::string text = plain(*doc);
  for (std::string_view words : {"café, naïve, Ærøskøbing", "αβγδ", "汉字测试", "שלום עולם", "🙂🚀", "3 < 4 & 5 > 2"}) {
    INFO(words);
    CHECK(text.find(words) != std::string::npos);
  }
  // Links: external kept, between chapters to anchors.
  CHECK(has_link(*doc, "https://example.com/widgets"));
  CHECK(has_link(*doc, "#inventory"));
  CHECK(has_id(*doc, "inventory"));
  CHECK(internal_links_resolve(*doc));
  CHECK(count<ir::PageBreak>(*doc) == 3);
  // The package description.
  CHECK(doc->metadata.title == "Widget Almanac");
  CHECK(doc->metadata.author == "Jane Example");
  CHECK(doc->metadata.language == "eng");
  CHECK(doc->metadata.date.starts_with("2001-02-03"));
  CHECK(doc->metadata.extra.at("publisher") == "Example Press");
  CHECK(doc->metadata.extra.at("description") == "A synthetic sample book about widgets.");
  // Calibre's own reading of the same file.
  const double similarity = test::text_similarity(text, calibre_text("calibre/sample_lit.txt"));
  INFO(similarity);
  CHECK(similarity >= 0.9);
}

TEST_CASE("office/lit: privacy, options and conversions") {
  const Bytes lit = data("calibre/sample.lit");
  const auto doc = test::read_as<ir::Document>("lit", lit);
  REQUIRE_OK(doc);
  CHECK(doc->metadata.title == "Widget Almanac");
  CHECK(doc->metadata.author.empty());
  CHECK(doc->metadata.extra.empty());
  const auto flowing = test::read_as<ir::Document>("lit", lit, R"({"chapter_breaks":false})");
  REQUIRE_OK(flowing);
  CHECK(count<ir::PageBreak>(*flowing) == 0);
  const auto detected = detect_format(Registry::instance(), lit, "");
  REQUIRE(detected.has_value());
  CHECK(detected->format->id == "lit");
  for (const char* target : {"epub", "md", "docx"}) {
    INFO(target);
    const auto out = test::convert(lit, "", target, {}, "book.lit");
    CHECK(out.ok());
  }
  const auto md = test::convert(lit, "lit", "md");
  REQUIRE(md.ok());
  CHECK(md.text().find("Inventory Table") != std::string::npos);
}

TEST_CASE("office/lit: a container built from the format description") {
  const auto doc = test::read_as<ir::Document>("lit", handmade_lit());
  REQUIRE_OK(doc);
  CHECK(outline(*doc) == std::vector<std::string>{"1:Hand Made"});
  CHECK(plain(*doc).find("One <two> & three back bold") != std::string::npos);
  CHECK(has_link(*doc, "#top"));
  CHECK(formatted<ir::Strong>(*doc, "bold"));
}

TEST_CASE("office/lit: DRM-protected books are refused") {
  for (const char* marker : {"/DRMStorage/DRMBookplate", "/DRMStorage/Licenses/EUL"}) {
    INFO(marker);
    const auto doc = test::read_as<ir::Document>("lit", handmade_lit({{marker, test::bytes("x")}}));
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error().code == Status::Decode);
    CHECK(doc.error().message.find("DRM") != std::string::npos);
  }
  // A book whose content section is DES-encrypted ("sealed" with encryption):
  // the LZX transform of Calibre's file replaced by the DES one.
  Bytes sealed = data("calibre/sample.lit");
  const Bytes lzx = test::bytes(std::string_view("\xC6\x07\x90\x0A\x76\x40\xD3\x11\x87\x89\x00\x00\xF8\x10\x57\x54", 16));
  const Bytes des = test::bytes(std::string_view("\xA2\xE4\xF6\x67\xBF\x60\xD3\x11\x85\x40\x00\xC0\x4F\x58\xC3\xCF", 16));
  std::size_t replaced = 0;
  for (auto it = std::ranges::search(sealed, lzx).begin(); it != sealed.end();
       it = std::ranges::search(std::ranges::subrange(it, sealed.end()), lzx).begin()) {
    std::ranges::copy(des, it);
    ++replaced;
  }
  REQUIRE(replaced >= 1);
  const auto doc = test::read_as<ir::Document>("lit", sealed);
  REQUIRE_FALSE(doc.has_value());
  CHECK(doc.error().message.find("DRM") != std::string::npos);
}

TEST_CASE("office/lit: damaged and hostile files") {
  const Bytes lit = data("calibre/sample.lit");
  CHECK(test::fuzz_reader<ir::Document>("lit", lit, 300) > 0);
  const Bytes handmade = handmade_lit();
  CHECK(test::fuzz_reader<ir::Document>("lit", handmade, 400) > 0);
  Limits small;
  small.max_decompressed_bytes = 100;
  CHECK_STATUS(test::read_as<ir::Document>("lit", lit, {}, nullptr, small), Status::Limit);
  // Markup that closes more than it opens, lengths beyond the end, deep nesting.
  Bytes extra_close = lit_page();
  close_tag(extra_close);
  CHECK_STATUS(test::read_as<ir::Document>("lit", handmade_lit({}, extra_close)), Status::Decode);
  Bytes long_value;
  put_char(long_value, 0);
  put_char(long_value, 1);
  put_char(long_value, 75);
  put_char(long_value, 0x83EB);
  put_char(long_value, 0xFFF0);
  CHECK_STATUS(test::read_as<ir::Document>("lit", handmade_lit({}, long_value)), Status::Decode);
  Bytes deep;
  for (int i = 0; i < 5000; ++i) open_tag(deep, 31);  // div
  CHECK_STATUS(test::read_as<ir::Document>("lit", handmade_lit({}, deep)), Status::Decode);
  // No pages at all.
  CHECK_STATUS(test::read_as<ir::Document>("lit", make_lit({{"/manifest", Bytes{std::byte{0}}}})), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Document>("lit", test::bytes("ITOLITLS")), Status::Decode);
}

}  // namespace cvt
