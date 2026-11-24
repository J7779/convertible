// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// The known-formats database: integrity of every row, detection of real
// files written by independent software (core/tests/data/detect/), and
// robustness of the content sniffers on hostile input.
#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/detect.hpp"
#include "engine/known_formats_table.hpp"
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;

namespace {

std::vector<const known::KnownFormat*> all_rows() {
  std::vector<const known::KnownFormat*> rows;
  for (const std::span<const known::KnownFormat> part : known::all_parts()) {
    for (const known::KnownFormat& row : part) rows.push_back(&row);
  }
  return rows;
}

bool valid_id_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '+'; }

// "type/subtype" with a registered top-level type and token characters.
bool valid_mime(std::string_view mime) {
  static const std::set<std::string_view> types = {"application", "audio", "chemical", "font", "image", "message",
                                                   "model", "multipart", "text", "video"};
  const std::size_t slash = mime.find('/');
  if (slash == std::string_view::npos || !types.contains(mime.substr(0, slash))) return false;
  const std::string_view subtype = mime.substr(slash + 1);
  return !subtype.empty() && std::ranges::all_of(subtype, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' ||
           c == '_';
  });
}

Registry known_only() {
  Registry registry;
  register_known_formats(registry);
  registry.finalize();
  return registry;
}

std::string detected(const Registry& registry, ByteView data, std::string_view name) {
  const auto found = detect_format(registry, data, name);
  return found ? found->format->id : std::string("-");
}

std::string detected(ByteView data, std::string_view name) { return detected(Registry::instance(), data, name); }

struct ManifestRow {
  std::string file;
  std::string named;
  std::string unnamed;
};

std::vector<ManifestRow> manifest() {
  std::vector<ManifestRow> rows;
  for (const std::string_view line : util::split_lines(test::fixture_text("detect/manifest.tsv"))) {
    if (line.empty() || line.starts_with('#')) continue;
    const std::vector<std::string_view> cells = util::split(line, '\t');
    REQUIRE(cells.size() == 3);
    rows.push_back({std::string(cells[0]), std::string(cells[1]), std::string(cells[2])});
  }
  return rows;
}

std::string leaf(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

}  // namespace

// ============================================================================ table integrity

TEST_CASE("known formats: every row is well formed") {
  const auto rows = all_rows();
  CHECK(rows.size() >= 1000);
  std::set<std::string_view> ids;
  for (const known::KnownFormat* row : rows) {
    INFO("format ", row->id);
    CHECK(ids.insert(row->id).second);  // unique ids
    CHECK(!row->id.empty());
    CHECK(row->id.size() <= 32);
    CHECK(std::ranges::all_of(row->id, valid_id_char));
    CHECK(!row->name.empty());
    CHECK(!row->description.empty());
    CHECK(row->description.ends_with('.'));
    CHECK(row->popularity >= 1);
    CHECK(row->popularity <= 100);
    CHECK(row->mime.empty() || valid_mime(row->mime));
    CHECK(category_name(row->category) != std::string_view());
    const std::vector<std::string_view> extensions = util::split(row->extensions, ' ', false);
    CHECK(!extensions.empty());
    std::set<std::string_view> seen;
    for (const std::string_view extension : extensions) {
      CHECK(seen.insert(extension).second);
      CHECK(extension.size() <= 16);
      CHECK(extension.front() != '.');
      CHECK(std::ranges::all_of(extension, [](char c) { return valid_id_char(c) || c == '!'; }));
    }
    bool any_signature = false;
    for (const known::KnownSignature& signature : row->signatures) {
      if (signature.pattern.text.empty()) continue;
      any_signature = true;
      const Signature parsed = Signature::at(signature.offset, signature.pattern);
      CHECK(!parsed.bytes.empty());
      CHECK(parsed.significant_bits() >= 16);  // at least two constrained bytes
      CHECK(signature.offset + parsed.bytes.size() <= 0x10100);  // within the head of a file
    }
    // A row that is recognised by content says how; otherwise its extensions
    // must not collide (checked by Registry::validate below).
    (void)any_signature;
  }
}

TEST_CASE("known formats: the database alone and the shared registry validate") {
  const Registry registry = known_only();
  CHECK(registry.validate().empty());
  for (const std::string& problem : registry.validate()) MESSAGE(problem);
  CHECK(registry.formats().size() == all_rows().size());
  CHECK(Registry::instance().validate().empty());
  for (const std::string& problem : Registry::instance().validate()) MESSAGE(problem);
}

TEST_CASE("known formats: ids that existed before keep their meaning") {
  const Registry& registry = Registry::instance();
  const std::map<std::string_view, Category> stable = {
      {"png", Category::Image},   {"jpg", Category::Image},       {"mp4", Category::Video},     {"mov", Category::Video},
      {"mp3", Category::Audio},   {"aiff", Category::Audio},      {"mid", Category::Audio},     {"zip", Category::Archive},
      {"tar.gz", Category::Archive}, {"docx", Category::Document}, {"xlsx", Category::Spreadsheet},
      {"pptx", Category::Presentation}, {"sqlite", Category::Data}, {"ttf", Category::Font},    {"srt", Category::Subtitle},
      {"obj", Category::Model3D}, {"gpx", Category::Geo},         {"ics", Category::Pim},       {"vcf", Category::Pim},
      {"ts", Category::Code},     {"macho", Category::Other},     {"exe", Category::Other},     {"elf", Category::Other},
      {"mpg", Category::Video},   {"svg", Category::Vector},      {"md", Category::Document},   {"yaml", Category::Data},
  };
  for (const auto& [id, category] : stable) {
    INFO("format ", id);
    const FormatInfo* format = registry.find(id);
    REQUIRE(format != nullptr);
    CHECK(format->category == category);
    CHECK(format->extensions.front() == (id == "tar.gz" ? "tar.gz" : id == "macho" ? "macho" : id));
  }
}

TEST_CASE("known formats: every signature identifies its own format") {
  const Registry registry = known_only();
  for (const known::KnownFormat* row : all_rows()) {
    for (const known::KnownSignature& signature : row->signatures) {
      if (signature.pattern.text.empty()) continue;
      INFO("format ", row->id, " signature at ", signature.offset);
      const Signature parsed = Signature::at(signature.offset, signature.pattern);
      Bytes data(signature.offset + parsed.bytes.size() + 64, std::byte{0});
      std::copy(parsed.bytes.begin(), parsed.bytes.end(), data.begin() + static_cast<std::ptrdiff_t>(signature.offset));
      const auto candidates = detect_candidates(registry, data, {});
      const auto own = std::ranges::find_if(candidates, [&](const Detection& d) { return d.format->id == row->id; });
      CHECK(own != candidates.end());
    }
  }
}

TEST_CASE("known formats: a family description overrides the database row") {
  Registry registry;
  register_known_formats(registry);
  FormatInfo family;
  family.id = "heic";
  family.name = "HEIF image (family)";
  family.signatures = {Signature::at(4, "'ftypheic'")};
  registry.add_format(family);
  registry.finalize();
  const FormatInfo* heic = registry.find("heic");
  REQUIRE(heic != nullptr);
  CHECK(heic->name == "HEIF image (family)");
  CHECK(heic->mime == "image/heic");  // filled in from the database
  CHECK(heic->signatures.size() == 1);
  CHECK(!heic->sniff);  // the database's sniffer no longer applies
}

// ============================================================================ real files

TEST_CASE("known formats: real files are detected with and without their names") {
  const auto rows = manifest();
  CHECK(rows.size() >= 300);
  for (const ManifestRow& row : rows) {
    INFO("fixture ", row.file);
    const Bytes data = test::fixture("detect/" + row.file);
    REQUIRE(!data.empty());
    CHECK(detected(data, leaf(row.file)) == row.named);
    CHECK(detected(data, {}) == row.unnamed);
    // A path with directories and an upper-case extension changes nothing.
    std::string upper = "Some.Dir/" + leaf(row.file);
    const std::size_t dot = upper.rfind('.');
    if (dot != std::string::npos && dot > 9) {
      for (std::size_t i = dot; i < upper.size(); ++i) upper[i] = util::ascii_upper(upper[i]);
      CHECK(detected(data, upper) == row.named);
    }
  }
}

TEST_CASE("known formats: every fixture in the detect directory is in the manifest") {
  std::set<std::string> listed;
  for (const ManifestRow& row : manifest()) listed.insert(row.file);
  const std::filesystem::path base = std::filesystem::path(CVT_TEST_DATA_DIR) / "detect";
  std::size_t files = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(base)) {
    if (!entry.is_regular_file()) continue;
    const std::string relative = std::filesystem::relative(entry.path(), base).generic_string();
    if (relative == "manifest.tsv" || relative == "SOURCES.md" || relative == ".gitattributes") continue;
    ++files;
    INFO("file ", relative);
    CHECK(listed.contains(relative));
  }
  CHECK(files == listed.size());
}

TEST_CASE("known formats: look-alike containers are told apart whatever their names") {
  const auto file = [](std::string_view name) { return test::fixture("detect/" + std::string(name)); };
  // ZIP family: the content wins over a misleading name; only a plain ZIP takes its name's word.
  CHECK(detected(file("office/document.docx"), "renamed.zip") == "docx");
  CHECK(detected(file("archive/archive.zip"), "report.docx") == "zip");
  CHECK(detected(file("exec/hello.jar"), "archive.zip") == "jar");
  CHECK(detected(file("office/spreadsheet.xlsx"), "book.docx") == "xlsx");
  CHECK(detected(file("office/document.odt"), "x.ods") == "odt");
  CHECK(detected(file("archive/comic_images.zip"), "comic.cbz") == "cbz");
  CHECK(detected(file("archive/archive.zip"), "places.kmz") == "zip");
  // RIFF and ISO base media: WAV vs AVI vs WebP, MP4 vs MOV vs HEIC.
  CHECK(detected(file("audio/tone.wav"), "x.avi") == "wav");
  CHECK(detected(file("video/clip.avi"), "x.webp") == "avi");
  CHECK(detected(file("image/pattern.webp"), "x.wav") == "webp");
  CHECK(detected(file("video/clip.mp4"), "x.heic") == "mp4");
  CHECK(detected(file("video/clip.mov"), "x.mp4") == "mov");
  CHECK(detected(file("image/mac.heic"), "x.mov") == "heic");
  CHECK(detected(file("audio/tone.m4a"), "x.mp4") == "m4a");
  // OLE2: Word vs Excel vs PowerPoint.
  CHECK(detected(file("office/document.doc"), "x.xls") == "doc");
  CHECK(detected(file("office/spreadsheet.xls"), "x.ppt") == "xls");
  CHECK(detected(file("office/presentation.ppt"), "x.doc") == "ppt");
  // XML dialects under a generic name.
  CHECK(detected(file("text/drawing.svg"), "x.xml") == "svg");
  CHECK(detected(file("geo/points.kml"), "x.xml") == "kml");
  CHECK(detected(file("geo/points.gpx"), "x.xml") == "gpx");
  CHECK(detected(file("text/feed.rss"), "x.xml") == "rss");
  CHECK(detected(file("text/data.xml"), "x.svg") == "xml");
  // Text tables and prose.
  CHECK(detected(file("text/table.csv"), {}) == "csv");
  CHECK(detected(file("text/table.tsv"), {}) == "tsv");
  CHECK(detected(file("text/plain.txt"), {}) == "txt");
  CHECK(detected(file("text/plain.txt"), "x.csv") == "csv");  // a text file named .csv is taken at its word
  // Java class files and universal binaries share CA FE BA BE.
  CHECK(detected(file("exec/Hello.class"), "x.dylib") == "class");
  CHECK(detected(file("exec/libfat.dylib"), "x.class") == "dylib");
}

// ============================================================================ robustness

TEST_CASE("known formats: empty, random and truncated input never crash a sniffer") {
  const Registry& registry = Registry::instance();
  CHECK(detected({}, {}) == "-");
  CHECK(detected({}, "clip.mkv") == "mkv");
  CHECK(detected({}, "notes.MD") == "md");
  // Every signature and every sniffer of every format, called directly (the
  // ranking around them is tested above; this is about memory safety).
  const std::vector<const FormatInfo*> formats = registry.formats();
  std::size_t hits = 0;
  const auto probe = [&](ByteView data, std::string_view name) {
    for (const FormatInfo* format : formats) {
      for (const Signature& signature : format->signatures) hits += signature.matches(data) ? 1 : 0;
      if (format->sniff) hits += format->sniff(data, name) > 0 ? 1 : 0;
    }
  };
  for (std::uint32_t seed = 1; seed <= 30; ++seed) {
    const Bytes noise = test::random_bytes(seed * 97 % 5000 + 1, seed);
    for (const std::string_view name : {"", "x.zip", "x.tif", "x.json"}) probe(noise, name);
  }
  // Every 1..16 byte prefix of every fixture, named and unnamed (prefixes
  // shared by several fixtures, such as ZIP headers, are probed once), and a
  // cut in the middle, where directories and tables are incomplete.
  std::set<std::string> seen;
  for (const ManifestRow& row : manifest()) {
    const Bytes data = test::fixture("detect/" + row.file);
    const std::string name = leaf(row.file);
    for (std::size_t cut = 1; cut <= 16 && cut <= data.size(); ++cut) {
      const ByteView prefix = ByteView(data).first(cut);
      if (!seen.insert(to_string(prefix) + '/' + name).second) continue;
      probe(prefix, name);
      if (seen.insert(to_string(prefix)).second) probe(prefix, {});
    }
    probe(ByteView(data).first(data.size() / 2), name);
    (void)detect_candidates(registry, ByteView(data).first(data.size() / 2), name);
  }
  CHECK(hits > 0);
}
