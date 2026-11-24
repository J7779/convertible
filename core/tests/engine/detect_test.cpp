// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/detect.hpp"
#include "engine/fakes.hpp"

using namespace cvt;
using test::fake_format;

namespace {

std::string detect_id(const Registry& registry, std::string_view data, std::string_view filename = {}) {
  const auto detected = detect_format(registry, as_bytes(data), filename);
  return detected ? detected->format->id : std::string("(none)");
}

std::string real(std::string_view data, std::string_view filename = {}) {
  return detect_id(Registry::instance(), data, filename);
}

Registry sample_registry() {
  Registry registry;
  FormatInfo png = fake_format("png", Category::Image, 90);
  png.signatures = {Signature::at(0, "89 'PNG' 0D 0A 1A 0A")};
  registry.add_format(png);
  FormatInfo zip = fake_format("zip", Category::Archive, 90);
  zip.signatures = {Signature::at(0, "'PK' 03 04")};
  registry.add_format(zip);
  FormatInfo docx = fake_format("docx", Category::Document, 80);
  docx.signatures = {Signature::at(0, "'PK' 03 04")};
  docx.sniff = [](ByteView data, std::string_view) {
    return as_chars(data).find("word/") != std::string_view::npos ? 92 : 0;
  };
  registry.add_format(docx);
  registry.add_format(fake_format("cbz", Category::Archive, 20));        // extension only
  registry.add_format(fake_format("csv", Category::Spreadsheet, 80));    // extension only
  FormatInfo gz = fake_format("gz", Category::Archive, 70);
  gz.signatures = {Signature::at(0, "1F 8B 08")};
  registry.add_format(gz);
  FormatInfo targz = fake_format("tar.gz", Category::Archive, 60, {"tar.gz", "tgz"});
  targz.signatures = {Signature::at(0, "1F 8B 08")};
  registry.add_format(targz);
  FormatInfo txt = fake_format("txt", Category::Document, 90);
  registry.add_format(txt);
  FormatInfo weak = fake_format("weak", Category::Other, 100);
  weak.signatures = {Signature::at(0, "'B'")};
  registry.add_format(weak);
  FormatInfo strong = fake_format("strong", Category::Other, 1);
  strong.signatures = {Signature::at(0, "'BMAGIC'")};
  registry.add_format(strong);
  return registry;
}

}  // namespace

TEST_CASE("detect: file name extensions") {
  CHECK(filename_extensions("photo.PNG") == std::vector<std::string>{"png"});
  CHECK(filename_extensions("Backup.TAR.GZ") == std::vector<std::string>{"tar.gz", "gz"});
  CHECK(filename_extensions("dir.d/archive.zip") == std::vector<std::string>{"zip"});
  CHECK(filename_extensions("C:\\dir.x\\file.txt") == std::vector<std::string>{"txt"});
  CHECK(filename_extensions("README").empty());
  CHECK(filename_extensions(".gitignore").empty());
  CHECK(filename_extensions("..hidden.md") == std::vector<std::string>{"md"});
  CHECK(filename_extensions("trailing.").empty());
  CHECK(filename_extensions("").empty());
  CHECK(filename_extensions("a.b c").empty());  // not an extension
}

TEST_CASE("detect: signatures, sniffers, extensions and their interplay") {
  const Registry registry = sample_registry();
  const std::string png = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR";
  const std::string zip = std::string("PK\x03\x04") + std::string(40, '\0');
  const std::string docx = std::string("PK\x03\x04") + std::string(26, '\0') + "word/document.xml";

  CHECK(detect_id(registry, png) == "png");
  CHECK(detect_id(registry, png, "wrong.csv") == "png");          // a long magic beats an extension
  CHECK(detect_id(registry, zip) == "zip");                        // tie on the magic: more popular wins
  CHECK(detect_id(registry, zip, "report.docx") == "docx");        // the extension breaks the tie
  CHECK(detect_id(registry, docx) == "docx");                      // the sniffer recognises the content
  CHECK(detect_id(registry, docx, "renamed.zip") == "docx");
  CHECK(detect_id(registry, zip, "comic.cbz") == "cbz");           // specific extension beats generic container
  CHECK(detect_id(registry, "a,b\n1,2\n", "table.csv") == "csv");
  CHECK(detect_id(registry, "plain words\n", "table.png") == "txt");  // content contradicts the extension
  CHECK(detect_id(registry, "plain words\n") == "txt");            // text heuristic as last resort
  CHECK(detect_id(registry, "", "empty.png") == "png");            // nothing to inspect: trust the name
  CHECK(detect_id(registry, "") == "(none)");
  CHECK(detect_id(registry, std::string("\x00\x01\x02\xff\xfe", 5)) == "(none)");
  CHECK(detect_id(registry, std::string("\x1f\x8b\x08\0", 4), "site.tar.gz") == "tar.gz");
  CHECK(detect_id(registry, std::string("\x1f\x8b\x08\0", 4), "site.TGZ") == "tar.gz");
  CHECK(detect_id(registry, std::string("\x1f\x8b\x08\0", 4), "page.html.gz") == "gz");
  CHECK(detect_id(registry, std::string("\x1f\x8b\x08\0", 4)) == "gz");
  CHECK(detect_id(registry, "BMAGIC....") == "strong");            // longer magic beats popularity

  const auto candidates = detect_candidates(registry, as_bytes(docx), "report.docx");
  REQUIRE(candidates.size() >= 2);
  CHECK(candidates[0].format->id == "docx");
  CHECK(candidates[0].confidence == 97);  // sniff 92 + extension bonus
  CHECK(candidates[1].format->id == "zip");
  CHECK(candidates[1].confidence == 75);
  for (std::size_t i = 1; i < candidates.size(); ++i) CHECK(candidates[i - 1].confidence >= candidates[i].confidence);
}

TEST_CASE("detect: sniff results are clamped and never consulted for empty data") {
  Registry registry;
  FormatInfo wild = fake_format("wild");
  wild.sniff = [](ByteView data, std::string_view) { return data.empty() ? 100 : 5000; };
  registry.add_format(wild);
  FormatInfo negative = fake_format("negative");
  negative.sniff = [](ByteView, std::string_view) { return -50; };
  registry.add_format(negative);
  const auto hit = detect_format(registry, as_bytes("x"), {});
  REQUIRE(hit.has_value());
  CHECK(hit->format->id == "wild");
  CHECK(hit->confidence == 100);
  CHECK_FALSE(detect_format(registry, {}, {}).has_value());
}

TEST_CASE("detect: common real formats through the shared registry") {
  using namespace std::string_literals;
  CHECK(real("\x89PNG\r\n\x1a\n"s) == "png");
  CHECK(real("\xFF\xD8\xFF\xE0\0\x10JFIF"s) == "jpg");
  CHECK(real("GIF89a\x01\0\x01\0"s) == "gif");
  CHECK(real("RIFF\x24\0\0\0WEBPVP8 "s) == "webp");
  CHECK(real("RIFF\x24\0\0\0WAVEfmt "s) == "wav");
  CHECK(real("RIFF\x24\0\0\0AVI LIST"s) == "avi");
  CHECK(real("%PDF-1.7\n"s) == "pdf");
  CHECK(real("\0\0\0\x18" "ftypmp42\0\0\0\0"s) == "mp4");
  CHECK(real("\0\0\0\x18" "ftypheic\0\0\0\0"s) == "heic");
  CHECK(real("\0\0\0\x14" "ftypqt  \0\0\0\0"s) == "mov");
  CHECK(real("\x1A\x45\xDF\xA3\x9F\x42\x86\x81\x01\x42\x82\x84webm"s) == "webm");
  CHECK(real("\x1A\x45\xDF\xA3\x9F\x42\x86\x81\x01\x42\x82\x88matroska"s) == "mkv");
  CHECK(real("ID3\x04\0\0\0\0\0\0"s) == "mp3");
  CHECK(real("fLaC\0\0\0\x22"s) == "flac");
  CHECK(real("OggS\0\x02\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x01\x13OpusHead"s) == "opus");
  CHECK(real("7z\xBC\xAF\x27\x1C\0\x04"s) == "7z");
  CHECK(real("Rar!\x1A\x07\x01\0"s) == "rar");
  CHECK(real("SQLite format 3\0"s) == "sqlite");
  CHECK(real("\x7F" "ELF\x02\x01\x01\0"s) == "elf");
  CHECK(real("MZ\x90\0\x03\0"s, "setup.exe") == "exe");
  CHECK(real("\xCA\xFE\xBA\xBE\0\0\0\x34"s) == "class");   // Java class file, major version 52
  CHECK(real("\xCA\xFE\xBA\xBE\0\0\0\x02"s) == "macho");   // universal binary with two architectures
  CHECK(real("wOF2\0\x01\0\0"s) == "woff2");
  CHECK(real("\x1f\x8b\x08\0\0\0\0\0"s, "logs.tar.gz") == "tar.gz");
  CHECK(real("\x1f\x8b\x08\0\0\0\0\0"s, "logs.txt.gz") == "gz");

  const std::string zip_head = "PK\x03\x04\x14\0\0\0\x08\0"s + std::string(16, '\0') + "\x11\0\0\0"s;
  CHECK(real(zip_head + "random/name.bin", "a.zip") == "zip");
  CHECK(real(zip_head + "word/document.xml") == "docx");
  CHECK(real(zip_head + "xl/workbook.xml", "book.zip") == "xlsx");
  CHECK(real(zip_head + "ppt/presentation.xml", "slides.pptx") == "pptx");
  CHECK(real(zip_head + "random/name.bin", "slides.pptx") == "zip");  // the content contradicts the name
  CHECK(real(zip_head + "random/name.bin") == "zip");
  CHECK(real(zip_head + "random/name.bin", "comic.kmz") == "zip");    // unknown extension: the container

  CHECK(real("<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>") == "svg");
  CHECK(real("<?xml version=\"1.0\"?><gpx version=\"1.1\"/>", "track.gpx") == "gpx");
  CHECK(real("<?xml version=\"1.0\"?><root/>") == "xml");
  CHECK(real("<?xml version=\"1.0\"?><root/>", "feed.rss") == "xml");
  CHECK(real("<!DOCTYPE html><html><body></body></html>") == "html");
  CHECK(real("{\"a\": 1}", "data.json") == "json");
  CHECK(real("# Title\n\nSome *markdown*.\n", "README.md") == "md");
  CHECK(real("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n") == "ics");
  CHECK(real("Just some notes.\nNothing more.\n", "notes.txt") == "txt");
  CHECK(real("Just some notes.\nNothing more.\n") == "txt");
  CHECK(real("\xFF\xFEh\0i\0"s) == "txt");                           // UTF-16 with BOM
  CHECK(real("PK\x03\x04 pretending", "notes.txt") == "zip");        // binary content under a text name
  CHECK(real("\x01\x02\x03\x04\xF0\xF1\xF2"s) == "(none)");
  CHECK(real("\x01\x02\x03\x04\xF0\xF1\xF2"s, "blob.unknownext") == "(none)");
  CHECK(real("", "movie.mkv") == "mkv");
}

TEST_CASE("detect: hostile input never crashes a sniffer") {
  const Registry& registry = Registry::instance();
  for (std::uint32_t seed = 1; seed <= 300; ++seed) {
    const Bytes noise = test::random_bytes(seed * 7 % 600, seed);
    (void)detect_candidates(registry, noise, "x.docx");
  }
  // Every signature prefix on its own (truncated right after the magic).
  for (const FormatInfo* format : registry.formats()) {
    for (const Signature& signature : format->signatures) {
      Bytes head(signature.offset, std::byte{0});
      append(head, ByteView(signature.bytes));
      for (std::size_t cut = 0; cut <= head.size(); ++cut) {
        (void)detect_candidates(registry, ByteView(head).first(cut), "f." + format->extensions.front());
      }
      const auto found = detect_format(registry, head, {});
      CHECK_MESSAGE(found.has_value(), format->id);
    }
  }
}
