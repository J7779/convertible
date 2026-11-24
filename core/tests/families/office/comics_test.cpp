// Office family: comic archives in RAR, 7-Zip and tar through the archive
// family's readers, and the ZIP-based e-book wrappers next to the real ZIP codec.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/office/office_test_support.hpp"

namespace cvt {

using namespace test::office;

namespace {

struct ComicFixture {
  const char* file;
  const char* format;     // our comic format id
  const char* container;  // the archive family's reader it needs
};

constexpr std::array<ComicFixture, 3> comic_fixtures = {
    ComicFixture{"archives/comic.cbr", "cbr", "rar"},
    ComicFixture{"archives/comic.cb7", "cb7", "7z"},
    ComicFixture{"archives/comic_nested.cbt", "cbt", "tar"},
};

bool has_archive_reader(std::string_view format) {
  for (const Codec* codec : Registry::instance().codecs_for(format)) {
    if (codec->kind == Kind::Archive && codec->read) return true;
  }
  return false;
}

// cover (#eeeeee), Chapter 1: page1 (red), page2 (green), page10 (blue), Chapter 2: page1 (yellow).
constexpr std::array<std::array<int, 3>, 5> page_colours = {
    std::array<int, 3>{0xEE, 0xEE, 0xEE}, {0xCC, 0x33, 0x33}, {0x33, 0xCC, 0x33}, {0x33, 0x33, 0xCC}, {0xCC, 0xCC, 0x33}};

void check_pages(const ir::Image& image) {
  REQUIRE(image.frames.size() == page_colours.size());
  for (std::size_t i = 0; i < page_colours.size(); ++i) {
    INFO(i);
    for (std::size_t c = 0; c < 3; ++c) CHECK(static_cast<int>(image.frames[i].rgba[c]) == page_colours[i][c]);
  }
}

}  // namespace

TEST_CASE("office/comics: CBR, CB7 and CBT pages in reading order") {
  for (const ComicFixture& f : comic_fixtures) {
    INFO(f.file);
    if (!has_archive_reader(f.container)) continue;  // runs once the archive family is present
    const Bytes bytes = data(f.file);
    // Cover first, then natural order through nested folders; notes.txt and
    // ComicInfo.xml are not pages.
    const auto image = test::read_as<ir::Image>(f.format, bytes);
    REQUIRE_OK(image);
    check_pages(*image);
    const auto doc = test::read_as<ir::Document>(f.format, bytes);
    REQUIRE_OK(doc);
    CHECK(pictures(*doc).size() == 5);
    CHECK(count<ir::PageBreak>(*doc) == 4);
    CHECK(doc->metadata.title == "Widget Comics");
    // Every member through the Archive reader.
    const auto archive = test::read_as<ir::Archive>(f.format, bytes);
    REQUIRE_OK(archive);
    std::size_t files = 0;
    for (const ir::ArchiveEntry& e : archive->entries) files += e.is_directory ? 0 : 1;
    CHECK(files == 7);
    CHECK(test::fuzz_reader<ir::Document>(f.format, bytes, 200) > 0);
  }
}

TEST_CASE("office/comics: conversions of a CBR") {
  if (!has_archive_reader("rar")) return;
  const Registry& registry = Registry::instance();
  const Bytes cbr = data("archives/comic.cbr");
  const auto detected = detect_format(registry, cbr, "comic.cbr");
  REQUIRE(detected.has_value());
  CHECK(detected->format->id == "cbr");
  // To CBZ: repacked member by member (pictures untouched), still read in order.
  const auto cbz = test::convert(cbr, "", "cbz", {}, "comic.cbr");
  REQUIRE(cbz.ok());
  const auto names = zip_names(cbz.data);
  CHECK(std::ranges::find(names, "Chapter 1/page10.png") != names.end());
  const auto repacked = test::read_as<ir::Image>("cbz", cbz.data);
  REQUIRE_OK(repacked);
  check_pages(*repacked);
  const auto original = test::read_as<ir::Image>("cbr", cbr);
  REQUIRE_OK(original);
  CHECK(test::pixels_equal(*original, *repacked));
  // To e-books and PDF through the Document reader.
  const auto epub = test::convert(cbr, "cbr", "epub");
  REQUIRE(epub.ok());
  const auto book = test::read_as<ir::Document>("epub", epub.data);
  REQUIRE_OK(book);
  CHECK(pictures(*book).size() == 5);
  if (registry.has_writer("pdf")) CHECK(test::convert(cbr, "cbr", "pdf").ok());
  // A picture of every page.
  if (registry.has_writer("png")) CHECK(test::convert(cbr, "cbr", "png").ok());
  // Without the RAR reader the error says what is missing (simulated with
  // a registry that has only this family's codecs).
  Registry alone;
  for (const Codec& codec : registry.codecs()) {
    if (codec.format_id == "cbr") alone.add_codec(codec);
  }
  const Codec* reader = nullptr;
  for (const Codec* codec : alone.codecs_for("cbr")) {
    if (codec->kind == Kind::Document) reader = codec;
  }
  REQUIRE(reader != nullptr);
  ReadContext ctx;
  ctx.registry = &alone;
  ctx.input = cbr;
  const auto missing = reader->read(ctx);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().code == Status::UnsupportedConversion);
  CHECK(missing.error().message.find("RAR") != std::string::npos);
}

TEST_CASE("office/comics: CB7 and CBT repack to ZIP") {
  for (const ComicFixture& f : comic_fixtures) {
    INFO(f.file);
    if (std::string_view(f.format) == "cbr" || !has_archive_reader(f.container) || !Registry::instance().has_writer("zip")) {
      continue;
    }
    const auto zip = test::convert(data(f.file), f.format, "zip");
    REQUIRE(zip.ok());
    const auto names = zip_names(zip.data);
    CHECK(std::ranges::find(names, "Chapter 2/page1.png") != names.end());
    CHECK(std::ranges::find(names, "notes.txt") != names.end());
  }
}

TEST_CASE("office/comics: the CBZ codecs sit on the archive family's format") {
  const Registry& registry = Registry::instance();
  const FormatInfo* info = registry.find("cbz");
  REQUIRE(info != nullptr);
  CHECK(std::ranges::find(info->extensions, "cbz") != info->extensions.end());
  bool document = false;
  bool image = false;
  for (const Codec* codec : registry.codecs_for("cbz")) {
    document = document || (codec->kind == Kind::Document && codec->read);
    image = image || (codec->kind == Kind::Image && codec->read);
  }
  CHECK(document);
  CHECK(image);
  const auto detected = detect_format(registry, data("archives/comic.cbz"), "comic.cbz");
  REQUIRE(detected.has_value());
  CHECK(detected->format->id == "cbz");
}

TEST_CASE("office/fbz: zipped FictionBook next to the archive family's ZIP codec") {
  const Registry& registry = Registry::instance();
  if (!registry.has_reader("fb2")) return;
  const Bytes fbz = data("pandoc/sample.fbz");
  for (const char* name : {"sample.fbz", "sample.fb2.zip"}) {
    INFO(name);
    const auto detected = detect_format(registry, fbz, name);
    REQUIRE(detected.has_value());
    CHECK(detected->format->id == "fbz");
    const auto md = test::convert(fbz, "", "md", {}, name);
    REQUIRE(md.ok());
    CHECK(md.text().find("Quarterly Widget Report") != std::string::npos);
  }
  if (registry.has_writer("fb2")) {
    // Written by our ZIP writer, readable by the archive family's ZIP reader.
    const auto doc = test::read_as<ir::Document>("fbz", fbz);
    REQUIRE_OK(doc);
    const auto written = test::write_from("fbz", *doc);
    REQUIRE_OK(written);
    if (has_archive_reader("zip")) {
      const auto archive = test::read_as<ir::Archive>("zip", *written);
      REQUIRE_OK(archive);
      REQUIRE(archive->entries.size() == 1);
      CHECK(archive->entries[0].path.ends_with(".fb2"));
    }
  }
}

}  // namespace cvt
