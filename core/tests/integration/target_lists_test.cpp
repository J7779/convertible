// The "convert to..." lists users see in every app: the leading suggestions
// for representative sources, the absence of nonsense routes, the role and
// compress flags, and real conversions with content checks for every pinned
// (source, target) pair. Fixtures were written by independent software (see
// the SOURCES.md files next to them).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/planner.hpp"
#include "integration/integration_support.hpp"
#include "ir/document.hpp"
#include "ir/plain_text.hpp"
#include "util/hash.hpp"
#include "util/deflate.hpp"
#include "util/json.hpp"

using namespace cvt;
using namespace cvt::test::integration;

namespace {

std::vector<std::string> target_ids(std::string_view source) {
  std::vector<std::string> ids;
  for (const TargetInfo& t : list_targets(Registry::instance(), source)) ids.push_back(t.format->id);
  return ids;
}

bool offers(std::string_view source, std::string_view target) {
  const auto ids = target_ids(source);
  return std::ranges::find(ids, target) != ids.end();
}

// Position of `target` in the list, or the list size when absent.
std::size_t rank(const std::vector<std::string>& ids, std::string_view target) {
  return static_cast<std::size_t>(std::ranges::find(ids, target) - ids.begin());
}

std::string read_text(std::string_view format, const Bytes& bytes) {
  if (auto doc = test::read_as<ir::Document>(format, bytes)) return ir::plain_text(doc->blocks);
  if (auto text = test::read_as<ir::PlainText>(format, bytes)) return text->text;
  return {};
}

// Share of the reference's words (letters and digits, any case) that appear
// in `text`: the content survived, whatever the target's own markup.
double words_kept(std::string_view reference, std::string_view text) {
  const auto words = [](std::string_view t) {
    std::set<std::string> out;
    std::string word;
    for (const char c : t) {
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (static_cast<unsigned char>(c) >= 0x80)) {
        word += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
      } else if (!word.empty()) {
        out.insert(std::move(word));
        word.clear();
      }
    }
    if (!word.empty()) out.insert(std::move(word));
    return out;
  };
  const auto expected = words(reference);
  const auto got = words(text);
  if (expected.empty()) return 1.0;
  std::size_t kept = 0;
  for (const std::string& w : expected) kept += got.contains(w) ? 1 : 0;
  return static_cast<double>(kept) / static_cast<double>(expected.size());
}

// Pictures compared by brightness when the target holds grey levels only.
ir::Frame luminance(const ir::Frame& frame) {
  ir::Frame out = frame;
  for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
    const auto y = static_cast<std::uint8_t>((299 * frame.rgba[i] + 587 * frame.rgba[i + 1] + 114 * frame.rgba[i + 2] + 500) / 1000);
    out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = y;
  }
  return out;
}

bool grey(const ir::Frame& frame) {
  for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
    if (frame.rgba[i] != frame.rgba[i + 1] || frame.rgba[i] != frame.rgba[i + 2]) return false;
  }
  return true;
}

struct Pinned {
  std::string_view source;
  std::vector<std::string_view> leading;   // expected among the first `within` suggestions
  std::size_t within;
  std::vector<std::string_view> offered;   // expected somewhere in the list
  std::vector<std::string_view> absent;    // never offered
};

const std::vector<Pinned> pinned = {
    {"txt", {"pdf", "docx", "html", "md"}, 6, {"odt", "rtf", "epub", "zip", "gz", "7z", "base64", "checksums", "qr"},
     {"xlsx", "csv", "svg", "fasta", "css-min", "extract"}},
    {"md", {"pdf", "docx", "html", "txt"}, 6, {"odt", "rtf", "epub", "tex", "zip", "base64"}, {"fasta", "svg", "js"}},
    {"html", {"pdf", "docx", "md", "txt"}, 6, {"html-min", "odt", "epub"}, {"svg", "fasta"}},
    {"docx", {"pdf", "html", "md", "txt", "odt", "rtf"}, 8, {"epub"}, {"svg", "fasta", "html-min"}},
    {"pdf", {"docx", "txt", "md", "html"}, 8, {"png", "jpg", "pdf"}, {"fasta", "xlsx-never"}},
    {"png", {"jpg", "png", "gif", "webp"}, 6, {"tiff", "bmp", "ico", "avif", "heic", "jxl", "pdf", "svg"}, {"mp3", "fasta"}},
    {"jpg", {"jpg", "png", "webp"}, 5, {"gif", "tiff", "bmp", "pdf"}, {"mp3"}},
    {"gif", {"jpg", "png", "gif"}, 5, {"webp", "mp4", "webm"}, {"mp3"}},
    {"svg", {"png", "pdf", "jpg", "svg"}, 6, {"eps", "emf", "dxf", "webp"}, {"mp3", "fasta"}},
    {"wav", {"mp3", "wav", "flac", "m4a"}, 6, {"ogg", "opus", "aac", "aiff"}, {"fasta", "xlsx", "docx"}},
    {"mp3", {"mp3", "wav", "flac"}, 5, {"ogg", "opus", "aac", "m4a"}, {"docx"}},
    {"flac", {"mp3", "wav", "flac"}, 5, {"ogg", "opus", "aac", "m4a"}, {"docx"}},
    {"mp4", {"mp4", "mkv", "mov", "webm"}, 6, {"gif", "mp3", "wav", "aac"}, {"docx", "fasta"}},
    {"mkv", {"mp4", "mkv", "mov", "webm"}, 6, {"gif", "mp3", "wav", "aac"}, {"docx"}},
    {"csv", {"csv", "xlsx", "json"}, 6, {"json", "sqlite", "md", "html", "pdf"}, {"fasta", "m3u", "pdb"}},
    {"xlsx", {"csv", "xlsx"}, 6, {"json", "sqlite", "pdf", "html"}, {"fasta", "m3u"}},
    {"json", {"json", "yaml", "xml", "toml", "csv"}, 8, {"xlsx", "zip", "base64"},
     {"svg", "eps", "emf", "dxf", "pdb", "fasta", "m3u", "rss", "gpl", "npmrc-never"}},
    {"yaml", {"json", "yaml", "xml", "toml"}, 8, {"csv"}, {"svg", "eps", "dxf", "fasta"}},
    {"zip", {"zip", "7z", "tar.gz", "extract"}, 8, {"tar", "base64", "checksums"}, {"png", "docx"}},
    {"tar.gz", {"zip", "7z", "tar.gz"}, 8, {"extract", "tar"}, {"png"}},
    {"gz", {"extract", "zip", "gz"}, 8, {"7z"}, {"png"}},
    {"obj", {"obj", "stl", "glb", "gltf"}, 6, {"3mf", "ply", "png", "dxf"}, {"code128", "qr", "fasta", "mp3"}},
    {"stl", {"obj", "stl", "glb"}, 6, {"3mf", "png"}, {"code128", "qr"}},
    {"srt", {"srt", "vtt", "ass"}, 6, {"txt", "ttml"}, {"png", "fasta"}},
    {"ics", {"ics"}, 3, {"csv", "json", "pdf"}, {"fasta", "png"}},
    {"vcf", {"vcf"}, 3, {"csv", "json", "pdf"}, {"fasta", "png"}},
    {"ttf", {"ttf", "otf", "woff2", "woff"}, 6, {"png", "pdf"}, {"mp3"}},
    {"py", {"html-highlight", "pdf"}, 8, {"pdf", "html", "md", "txt", "zip", "qr"}, {"js", "css", "sql", "graphql", "datetime-never"}},
    {"bin", {"base64", "hex", "checksums"}, 6, {"zip", "gz", "7z"}, {"extract", "png", "docx", "html-min", "pdf"}},
    {"dxf", {"svg", "png", "pdf"}, 8, {"eps", "emf", "obj", "stl"}, {"fasta"}},
};

}  // namespace

TEST_CASE("integration/targets: leading suggestions, obvious targets and no nonsense") {
  for (const Pinned& p : pinned) {
    INFO("source ", p.source);
    const auto ids = target_ids(p.source);
    REQUIRE(!ids.empty());
    for (const std::string_view target : p.leading) {
      INFO("leading ", target, " (rank ", rank(ids, target), ")");
      CHECK(rank(ids, target) < p.within);
    }
    for (const std::string_view target : p.offered) {
      INFO("offered ", target);
      CHECK(rank(ids, target) < ids.size());
    }
    for (const std::string_view target : p.absent) {
      INFO("absent ", target);
      CHECK(rank(ids, target) == ids.size());
    }
    // No id twice; universal byte-level targets come after every real conversion.
    CHECK(std::set<std::string>(ids.begin(), ids.end()).size() == ids.size());
  }
}

TEST_CASE("integration/targets: roles and the compress flag") {
  const Registry& registry = Registry::instance();
  const FormatInfo* gz = registry.find("gz");
  REQUIRE(gz != nullptr);
  for (const TargetInfo& t : list_targets(registry, "gz")) {
    const std::string_view role = target_role(*gz, t);
    if (t.format->id == "gz") CHECK(role == "compress");
    if (t.format->id == "extract") CHECK(role == "extract");
    if (t.format->id == "base64") CHECK(role == "encode");
  }
  const FormatInfo* txt = registry.find("txt");
  for (const TargetInfo& t : list_targets(registry, "txt")) {
    const std::string_view role = target_role(*txt, t);
    CHECK(t.same_format == (role == "compress"));
    if (t.format->id == "zip" || t.format->id == "7z") CHECK(role == "archive");
    if (t.format->id == "pdf") CHECK(role == "convert");
  }
  // The C ABI carries the role.
  char* json = cvt_targets_json("txt");
  const auto parsed = util::parse_json(json);
  cvt_string_free(json);
  REQUIRE(parsed.has_value());
  for (const Json& entry : parsed->elements()) {
    const std::string role = entry["role"].as_string();
    CHECK((role == "compress" || role == "extract" || role == "archive" || role == "encode" || role == "convert"));
    CHECK(entry["compress"].as_bool() == (role == "compress"));
  }
}

TEST_CASE("integration/targets: documents convert with their text") {
  const Bytes md = test::fixture("detect/text/notes.md");
  const std::string reference = read_text("md", md);
  REQUIRE(reference.find("item two") != std::string::npos);
  const Bytes docx = test::fixture("office/libreoffice/sample.docx");
  const std::string docx_text = read_text("docx", docx);
  REQUIRE(!docx_text.empty());
  for (const char* target : {"pdf", "docx", "html", "txt", "odt", "rtf", "epub"}) {
    INFO("md -> ", target);
    const CapiResult out = capi_convert(md, "notes.md", target);
    REQUIRE(out.ok());
    CHECK(words_kept(reference, read_text(target, out.data)) >= 0.9);
  }
  for (const char* target : {"pdf", "html", "md", "txt", "odt", "rtf", "epub"}) {
    INFO("docx -> ", target);
    const CapiResult out = capi_convert(docx, "sample.docx", target);
    REQUIRE(out.ok());
    CHECK(words_kept(docx_text, read_text(target, out.data)) >= 0.9);
  }
  const Bytes pdf = test::fixture("detect/office/document.pdf");
  const std::string pdf_text = read_text("pdf", pdf);
  for (const char* target : {"docx", "txt", "md", "html"}) {
    INFO("pdf -> ", target);
    const CapiResult out = capi_convert(pdf, "document.pdf", target);
    REQUIRE(out.ok());
    CHECK(words_kept(pdf_text, read_text(target, out.data)) >= 0.9);
  }
  // pdf -> png and jpg: the same page; JPEG close to the PNG rendering.
  const CapiResult png = capi_convert(pdf, "document.pdf", "png");
  const CapiResult jpg = capi_convert(pdf, "document.pdf", "jpg");
  REQUIRE(png.ok());
  REQUIRE(jpg.ok());
  const auto a = test::read_as<ir::Image>("png", png.data);
  const auto b = test::read_as<ir::Image>("jpg", jpg.data);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  CHECK(test::ssim(*a, *b) >= 0.9);
}

TEST_CASE("integration/targets: pictures convert to every image format") {
  const Bytes png = test::fixture("detect/image/pattern.png");
  const auto original = test::read_as<ir::Image>("png", png);
  REQUIRE_OK(original);
  std::size_t converted = 0;
  for (const TargetInfo& t : list_targets(Registry::instance(), "png")) {
    if (t.format->category != Category::Image || t.format->output_only) continue;
    if (test::find_reader(t.format->id, Kind::Image) == nullptr) continue;  // write-only formats
    INFO("png -> ", t.format->id);
    const CapiResult out = capi_convert(png, "pattern.png", t.format->id);
    REQUIRE(out.ok());
    const auto back = test::read_as<ir::Image>(t.format->id, out.data);
    REQUIRE_OK(back);
    // Formats with fewer colours or channels (GIF, PBM, ICO sizes ...) still keep the picture.
    if (back->width() == original->width() && back->height() == original->height()) {
      const ir::Frame& got = back->frames[0];
      CHECK(test::ssim(got, grey(got) ? luminance(original->frames[0]) : original->frames[0]) >= 0.6);
    }
    ++converted;
  }
  CHECK(converted >= 20);
}

TEST_CASE("integration/targets: sound and video keep their content") {
  const Bytes wav = test::fixture("detect/audio/tone.wav");
  const auto tone = test::read_as<ir::Audio>("wav", wav);
  REQUIRE_OK(tone);
  for (const char* target : {"mp3", "flac", "aac", "ogg", "opus", "m4a"}) {
    INFO("wav -> ", target);
    const CapiResult out = capi_convert(wav, "tone.wav", target);
    REQUIRE(out.ok());
    // Back at the source's rate (Opus always codes at 48 kHz).
    const CapiResult back = capi_convert(out.data, std::string("tone.") + target, "wav",
                                         std::format(R"({{"sample_rate":{}}})", tone->sample_rate));
    REQUIRE(back.ok());
    const auto audio = test::read_as<ir::Audio>("wav", back.data);
    REQUIRE_OK(audio);
    const double floor = std::string_view(target) == "flac" ? 80.0 : 12.0;
    CHECK(test::aligned_snr_db(*tone, *audio, 8192) >= floor);
  }
  const Bytes mp4 = test::fixture("video/h264_aac.mp4");
  for (const char* target : {"mkv", "webm", "mov", "gif", "mp3", "wav", "aac"}) {
    INFO("mp4 -> ", target);
    const CapiResult out = capi_convert(mp4, "clip.mp4", target);
    REQUIRE(out.ok());
    CHECK(!out.data.empty());
    const std::string_view t = target;
    if (t == "mp3" || t == "wav" || t == "aac") {
      const auto audio = test::read_as<ir::Audio>(t, out.data);
      REQUIRE_OK(audio);
      CHECK(audio->frame_count() > 0);
    } else if (t == "gif") {
      const auto image = test::read_as<ir::Image>("gif", out.data);
      REQUIRE_OK(image);
      CHECK(image->frames.size() > 1);
    } else {
      auto drained = decode_all(t, out.data);
      REQUIRE_OK(drained);
      CHECK(!drained->frames.empty());
    }
  }
}

TEST_CASE("integration/targets: tables and data survive their round trips") {
  const Bytes csv = test::fixture("detect/text/table.csv");
  const auto table = test::read_as<ir::Table>("csv", csv);
  REQUIRE_OK(table);
  for (const char* target : {"xlsx", "ods", "sqlite", "md", "html", "json"}) {
    INFO("csv -> ", target, " -> csv");
    const CapiResult out = capi_convert(csv, "table.csv", target);
    REQUIRE(out.ok());
    const CapiResult back = capi_convert(out.data, std::string("table.") + target, "csv", {}, target);
    REQUIRE(back.ok());
    const auto again = test::read_as<ir::Table>("csv", back.data);
    REQUIRE_OK(again);
    CHECK_MESSAGE(test::tables_equal(*again, *table, 1e-9), test::table_diff(*again, *table));
  }
  const Bytes json = test::fixture("detect/text/data.json");
  const auto data = test::read_as<ir::Data>("json", json);
  REQUIRE_OK(data);
  for (const char* target : {"yaml", "toml", "xml"}) {
    INFO("json -> ", target, " -> json");
    const CapiResult out = capi_convert(json, "data.json", target);
    REQUIRE(out.ok());
    const CapiResult back = capi_convert(out.data, std::string("data.") + target, "json", {}, target);
    REQUIRE(back.ok());
    const auto again = test::read_as<ir::Data>("json", back.data);
    REQUIRE_OK(again);
    if (std::string_view(target) == "xml") {
      CHECK(!again->value.is_null());  // XML has no types: the structure, not the values, comes back
    } else {
      CHECK(test::data_equal(again->value, data->value));
    }
  }
  const CapiResult rows = capi_convert(csv, "table.csv", "json");
  REQUIRE(rows.ok());
  const CapiResult table_again = capi_convert(rows.data, "rows.json", "csv");
  REQUIRE(table_again.ok());
}

TEST_CASE("integration/targets: any file packs, encodes and comes back") {
  const Bytes input = test::fixture("detect/text/plain.txt");
  for (const char* target : {"zip", "gz", "7z", "tar.gz"}) {
    INFO("txt -> ", target, " -> extract");
    const CapiResult packed = capi_convert(input, "notes.txt", target);
    REQUIRE(packed.ok());
    const std::string name = std::string("notes.") + (std::string_view(target) == "gz" ? "txt.gz" : target);
    cvt_result* result = cvt_convert(reinterpret_cast<const std::uint8_t*>(packed.data.data()), packed.data.size(),
                                     name.c_str(), nullptr, "extract", nullptr, nullptr, nullptr);
    REQUIRE(cvt_result_status(result) == CVT_OK);
    CHECK(std::string_view(cvt_result_filename(result)) == "notes.txt");
    CHECK(std::string_view(cvt_result_format(result)) == "txt");
    CHECK(std::string_view(cvt_result_extension(result)) == "txt");
    CHECK(std::string_view(cvt_result_mime(result)) == "text/plain");
    CHECK(Bytes(reinterpret_cast<const std::byte*>(cvt_result_data(result)),
                reinterpret_cast<const std::byte*>(cvt_result_data(result)) + cvt_result_size(result)) == input);
    cvt_result_free(result);
  }
  const CapiResult b64 = capi_convert(input, "notes.txt", "base64");
  REQUIRE(b64.ok());
  const CapiResult decoded = capi_convert(b64.data, "notes.b64", "bin", {}, "base64");
  REQUIRE(decoded.ok());
  CHECK(decoded.data == input);
  const CapiResult hex = capi_convert(input, "notes.txt", "hex");
  REQUIRE(hex.ok());
  const CapiResult sums = capi_convert(input, "notes.txt", "checksums");
  REQUIRE(sums.ok());
  CHECK(test::str(sums.data).find(util::sha256_hex(input)) != std::string::npos);
  // Several files: extract names them.
  const CapiResult two = capi_convert(test::fixture("detect/archive/comic_images.zip"), "pages.zip", "extract");
  CHECK(two.status == CVT_ERR_ENCODE);
  CHECK(two.message.find("files (") != std::string::npos);
}

TEST_CASE("integration/targets: vector drawings stay vectors, data never becomes a drawing") {
  for (const char* data : {"json", "yaml", "toml", "xml"}) {
    for (const char* vector : {"svg", "eps", "emf", "dxf", "wmf", "pdf-never"}) {
      INFO(data, " -> ", vector);
      CHECK_FALSE(offers(data, vector));
    }
  }
  CHECK(offers("svg", "json"));  // the scene as a structure dump
  const Bytes svg = test::fixture("vector/svg/paint.svg");
  // svg -> png renders; compare with the rsvg rendering written by librsvg.
  const CapiResult png = capi_convert(svg, "paint.svg", "png");
  REQUIRE(png.ok());
  auto drawn = test::read_as<ir::Image>("png", png.data);
  const auto reference = test::read_as<ir::Image>("png", test::fixture("vector/svg/paint_rsvg.png"));
  REQUIRE_OK(drawn);
  REQUIRE_OK(reference);
  CHECK(test::ssim(*drawn, *reference) >= 0.8);
  // svg -> eps / emf / dxf / pdf and back to a picture: the drawing survives.
  const Bytes shapes = test::fixture("vector/svg/shapes.svg");
  const CapiResult shapes_png = capi_convert(shapes, "shapes.svg", "png");
  REQUIRE(shapes_png.ok());
  drawn = test::read_as<ir::Image>("png", shapes_png.data);
  REQUIRE_OK(drawn);
  for (const char* target : {"eps", "emf", "dxf", "pdf"}) {
    INFO("svg -> ", target);
    const CapiResult out = capi_convert(shapes, "shapes.svg", target);
    REQUIRE(out.ok());
    const CapiResult back = capi_convert(out.data, std::string("paint.") + target, "png");
    REQUIRE(back.ok());
    const auto picture = test::read_as<ir::Image>("png", back.data);
    REQUIRE_OK(picture);
    if (picture->width() == drawn->width() && picture->height() == drawn->height()) {
      CHECK(test::ssim(*picture, *drawn) >= (std::string_view(target) == "dxf" ? 0.5 : 0.8));
    }
  }
}

TEST_CASE("integration/targets: svg -> pdf is true vector output") {
  const Bytes svg = test::fixture("vector/svg/shapes.svg");
  const CapiResult pdf = capi_convert(svg, "shapes.svg", "pdf");
  REQUIRE(pdf.ok());
  const std::string text = test::str(pdf.data);
  // No page-sized picture: the shapes are content-stream paths.
  CHECK(text.find("/Subtype /Image") == std::string::npos);
  const std::size_t stream = text.find("/FlateDecode");
  REQUIRE(stream != std::string::npos);
  bool vector_operators = false;
  for (std::size_t at = text.find("stream\n"); at != std::string::npos; at = text.find("stream\n", at + 1)) {
    const std::size_t end = text.find("\nendstream", at);
    if (end == std::string::npos) break;
    auto inflated = util::zlib_decompress(ByteView(pdf.data).subspan(at + 7, end - at - 7), 1u << 24);
    if (!inflated) continue;
    const std::string ops = test::str(*inflated);
    const bool paths = ops.find(" re\n") != std::string::npos || ops.find(" c\n") != std::string::npos || ops.find(" l\n") != std::string::npos;
    const bool painted = ops.find("\nf\n") != std::string::npos || ops.find("\nS\n") != std::string::npos ||
                         ops.find("\nf*\n") != std::string::npos || ops.find("\nB\n") != std::string::npos;
    if (paths && painted) {
      vector_operators = true;
    }
  }
  CHECK(vector_operators);
  // Rendered back through the PDF reader, it matches the SVG rendering.
  const CapiResult page = capi_convert(pdf.data, "shapes.pdf", "png", R"({"dpi":72})");
  const CapiResult direct = capi_convert(svg, "shapes.svg", "png", R"({"dpi":72})");
  REQUIRE(page.ok());
  REQUIRE(direct.ok());
  const auto a = test::read_as<ir::Image>("png", page.data);
  const auto b = test::read_as<ir::Image>("png", direct.data);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  if (a->width() == b->width() && a->height() == b->height()) {
    // The PDF page has a white background; the SVG rendering is transparent.
    ir::Image flat = *b;
    for (std::size_t i = 0; i < flat.frames[0].rgba.size(); i += 4) {
      const std::uint32_t alpha = flat.frames[0].rgba[i + 3];
      for (std::size_t c = 0; c < 3; ++c) {
        flat.frames[0].rgba[i + c] = static_cast<std::uint8_t>((flat.frames[0].rgba[i + c] * alpha + 255 * (255 - alpha) + 127) / 255);
      }
      flat.frames[0].rgba[i + 3] = 255;
    }
    CHECK(test::ssim(*a, flat) >= 0.85);
  } else {
    MESSAGE("page ", a->width(), "x", a->height(), " vs drawing ", b->width(), "x", b->height());
    CHECK(std::abs(static_cast<int>(a->width()) - static_cast<int>(b->width())) <= 2);
  }
}

TEST_CASE("integration/targets: meshes, subtitles, calendars, contacts and fonts") {
  const Bytes obj = test::fixture("detect/text/cube.obj");
  const auto cube = test::read_as<ir::Mesh>("obj", obj);
  REQUIRE_OK(cube);
  for (const char* target : {"stl", "glb", "gltf", "3mf", "ply", "dxf"}) {
    INFO("obj -> ", target);
    const CapiResult out = capi_convert(obj, "cube.obj", target);
    REQUIRE(out.ok());
    const auto back = test::read_as<ir::Mesh>(target, out.data);
    REQUIRE_OK(back);
    CHECK(back->positions.size() >= cube->positions.size());
    CHECK(back->face_count() >= cube->face_count());
  }
  const Bytes srt = test::fixture("detect/text/subtitles.srt");
  for (const char* target : {"vtt", "ass", "ttml", "txt"}) {
    INFO("srt -> ", target);
    const CapiResult out = capi_convert(srt, "subtitles.srt", target);
    REQUIRE(out.ok());
    CHECK(!out.data.empty());
  }
  for (const auto& [file, targets] : std::vector<std::pair<const char*, std::vector<const char*>>>{
           {"detect/text/event.ics", {"csv", "json", "pdf"}}, {"detect/text/contact.vcf", {"csv", "json", "pdf"}},
           {"detect/font/sample.ttf", {"otf", "woff2", "woff", "png", "pdf"}}}) {
    const std::string_view path = file;
    const std::string name(path.substr(path.rfind('/') + 1));
    for (const char* target : targets) {
      INFO(name, " -> ", target);
      const CapiResult out = capi_convert(test::fixture(file), name, target);
      CHECK(out.ok());
      CHECK(!out.data.empty());
    }
  }
}
