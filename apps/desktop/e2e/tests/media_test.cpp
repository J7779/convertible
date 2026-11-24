// Real media and document conversions through the app, each one conditional
// on the catalogue: image quality and size, audio duration, video to an
// animation or a frame, documents to PDF (pages counted), CSV to a
// spreadsheet and back and to JSON (cell for cell), and the size-limit
// message. Targets are found by MIME type in the app's own catalogue (a
// documented field of the C ABI), never by a hard-coded id or position; the
// outputs are parsed by code written here (PNG, WAV, GIF and PDF structure;
// PDF streams are inflated with the core's own inflater).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "common.hpp"
#include "util/deflate.hpp"

using namespace e2e;

namespace {

Json catalogue(Session& s) {
  REQUIRE(s.ok(s.run_inline(script(R"JS([{"op": "dump", "path": "catalog.json", "catalog": true}])JS"))));
  return s.state("catalog.json")["catalog"];
}

const Json* format(const Json& cat, const std::string& id) {
  for (const Json& f : cat.items()) {
    if (f["id"].str() == id) return &f;
  }
  return nullptr;
}

/// The first of `targets` (core order) whose catalogue MIME type matches `pred`.
template <class Pred>
std::string target_where(const Json& cat, const std::vector<std::string>& targets, Pred pred) {
  for (const auto& t : targets) {
    const Json* f = format(cat, t);
    if (f && pred(*f)) return t;
  }
  return {};
}

std::string target_with_mime(const Json& cat, const std::vector<std::string>& targets, const std::string& mime) {
  return target_where(cat, targets, [&](const Json& f) { return f["mime"].str() == mime; });
}

struct Converted {
  Json item;  // state-dump item after the conversion
  Bytes data;
  std::string name;
};

/// Opens `input`, converts it to `target` (after `option_steps`) and saves it.
Converted convert(Session& s, const std::string& input, const std::string& target, const Json& option_steps,
                  const std::string& tag, bool expect_success = true) {
  Json steps = Json::array();
  Json files = Json::array();
  files.push_back(s.in(input));
  steps.push_back(step({{"op", "open"}, {"files", files}}));
  steps.push_back(step({{"op", "setTarget"}, {"index", 0}, {"target", target}}));
  for (const Json& o : option_steps.items()) steps.push_back(o);
  steps.push_back(step({{"op", "convertAll"}}));
  steps.push_back(step({{"op", "waitIdle"}}));
  steps.push_back(step({{"op", "frames"}, {"count", 3}}));
  if (expect_success) steps.push_back(step({{"op", "saveAll"}, {"dir", tag}, {"expect", 1}}));
  steps.push_back(step({{"op", "dump"}, {"path", tag + ".json"}}));
  REQUIRE(s.ok(s.run_inline(steps)));
  Converted c;
  const Json st = s.state(tag + ".json");
  c.item = at(st, "items.0");
  if (!expect_success) return c;
  REQUIRE_MESSAGE(c.item["status"].str() == "done", c.item["errorMessage"].str());
  const auto names = list_dir(s.out(tag));
  REQUIRE(names.size() == 1u);
  c.name = names[0];
  c.data = saved(s, tag + "/" + names[0]);
  return c;
}

std::uint32_t be32(const Bytes& b, std::size_t o) {
  return (static_cast<std::uint32_t>(b[o]) << 24) | (static_cast<std::uint32_t>(b[o + 1]) << 16) |
         (static_cast<std::uint32_t>(b[o + 2]) << 8) | b[o + 3];
}
std::uint32_t le32(const Bytes& b, std::size_t o) {
  return b[o] | (static_cast<std::uint32_t>(b[o + 1]) << 8) | (static_cast<std::uint32_t>(b[o + 2]) << 16) |
         (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
std::uint32_t le16(const Bytes& b, std::size_t o) { return b[o] | (static_cast<std::uint32_t>(b[o + 1]) << 8); }

void put_be32(Bytes& b, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
void put_le(Bytes& b, std::uint32_t v, int n) {
  for (int i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

/// PNG width and height from the IHDR chunk; nullopt when not a PNG.
std::optional<std::pair<std::uint32_t, std::uint32_t>> png_size(const Bytes& b) {
  static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (b.size() < 33 || std::memcmp(b.data(), sig, 8) != 0 || std::memcmp(b.data() + 12, "IHDR", 4) != 0) return std::nullopt;
  return std::make_pair(be32(b, 16), be32(b, 20));
}

/// A PNG whose header claims `w` x `h` pixels (the image data is a token).
Bytes png_header_only(std::uint32_t w, std::uint32_t h) {
  Bytes out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  const auto chunk = [&](const char* type, const Bytes& data) {
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    Bytes typed(type, type + 4);
    typed.insert(typed.end(), data.begin(), data.end());
    out.insert(out.end(), typed.begin(), typed.end());
    put_be32(out, crc32(typed));
  };
  Bytes ihdr;
  put_be32(ihdr, w);
  put_be32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk("IHDR", ihdr);
  chunk("IDAT", {0x78, 0x01, 0x01, 0x01, 0x00, 0xFE, 0xFF, 0x00, 0x00, 0x01, 0x00, 0x01});
  chunk("IEND", {});
  return out;
}

struct WavInfo {
  std::uint32_t rate = 0;
  std::uint32_t channels = 0;
  std::uint32_t bits = 0;
  std::uint64_t data = 0;
  [[nodiscard]] double seconds() const {
    const double frame = static_cast<double>(channels) * bits / 8.0;
    return rate && frame > 0 ? static_cast<double>(data) / frame / rate : 0.0;
  }
};

std::optional<WavInfo> parse_wav(const Bytes& b) {
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) return std::nullopt;
  WavInfo w;
  for (std::size_t p = 12; p + 8 <= b.size();) {
    const std::uint64_t size = le32(b, p + 4);
    const std::size_t body = p + 8;
    if (std::memcmp(b.data() + p, "fmt ", 4) == 0 && body + 16 <= b.size()) {
      w.channels = le16(b, body + 2);
      w.rate = le32(b, body + 4);
      w.bits = le16(b, body + 14);
    } else if (std::memcmp(b.data() + p, "data", 4) == 0) {
      w.data = std::min<std::uint64_t>(size, b.size() - body);
      break;
    }
    p = body + static_cast<std::size_t>(size) + (size & 1);
  }
  if (!w.rate || !w.channels) return std::nullopt;
  return w;
}

/// Number of frames (image descriptors) in a GIF; nullopt when malformed.
std::optional<int> gif_frames(const Bytes& b) {
  if (b.size() < 13 || std::memcmp(b.data(), "GIF8", 4) != 0) return std::nullopt;
  std::size_t p = 13;
  if (b[10] & 0x80) p += 3u * (1u << ((b[10] & 7) + 1));
  const auto skip_blocks = [&]() -> bool {
    while (p < b.size()) {
      const std::size_t n = b[p++];
      if (n == 0) return true;
      p += n;
    }
    return false;
  };
  int frames = 0;
  while (p < b.size()) {
    const std::uint8_t tag = b[p++];
    if (tag == 0x3B) return frames;
    if (tag == 0x21) {
      ++p;  // label
      if (!skip_blocks()) return std::nullopt;
    } else if (tag == 0x2C) {
      if (p + 9 > b.size()) return std::nullopt;
      const std::uint8_t packed = b[p + 8];
      p += 9;
      if (packed & 0x80) p += 3u * (1u << ((packed & 7) + 1));
      ++p;  // LZW minimum code size
      if (!skip_blocks()) return std::nullopt;
      ++frames;
    } else {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

/// Pages of a PDF: "/Type /Page" dictionaries, also inside compressed
/// object streams (every FlateDecode stream is inflated with the core's inflater).
int pdf_pages(const Bytes& pdf) {
  std::string text(pdf.begin(), pdf.end());
  std::string all = text;
  for (std::size_t p = 0; (p = text.find("stream", p)) != std::string::npos; p += 6) {
    if (p > 0 && text[p - 1] == 'd') continue;  // "endstream"
    std::size_t start = p + 6;
    if (start < text.size() && text[start] == '\r') ++start;
    if (start < text.size() && text[start] == '\n') ++start;
    const std::size_t end = text.find("endstream", start);
    if (end == std::string::npos) break;
    const auto* data = reinterpret_cast<const std::byte*>(text.data() + start);
    const auto raw = cvt::util::zlib_decompress(cvt::ByteView(data, end - start), 64u << 20);
    if (raw.has_value()) {
      all.append(reinterpret_cast<const char*>(raw->data()), raw->size());
    }
    p = end;
  }
  int pages = 0;
  for (std::size_t p = 0; (p = all.find("/Type", p)) != std::string::npos; ++p) {
    std::size_t q = p + 5;
    while (q < all.size() && (all[q] == ' ' || all[q] == '\n' || all[q] == '\r')) ++q;
    if (all.compare(q, 5, "/Page") == 0 && (q + 5 >= all.size() || all[q + 5] != 's')) ++pages;
  }
  return pages;
}

/// A short YUV4MPEG2 clip: `frames` frames of a moving gradient, 4:2:0.
Bytes make_y4m(int w, int h, int frames) {
  const std::string head = "YUV4MPEG2 W" + std::to_string(w) + " H" + std::to_string(h) + " F10:1 Ip A1:1 C420jpeg\n";
  Bytes out(head.begin(), head.end());
  for (int f = 0; f < frames; ++f) {
    const std::string fr = "FRAME\n";
    out.insert(out.end(), fr.begin(), fr.end());
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) out.push_back(static_cast<std::uint8_t>(16 + ((x * 6 + y * 2 + f * 20) % 220)));
    }
    for (int plane = 0; plane < 2; ++plane) {
      for (int i = 0; i < (w / 2) * (h / 2); ++i) out.push_back(static_cast<std::uint8_t>(plane ? 90 + f * 8 : 160 - f * 6));
    }
  }
  return out;
}

/// A ZIP archive with stored (uncompressed) entries.
Bytes stored_zip(const std::vector<std::pair<std::string, std::string>>& entries) {
  Bytes z;
  Bytes cd;
  for (const auto& [name, body] : entries) {
    const Bytes data(body.begin(), body.end());
    const std::uint32_t crc = crc32(data);
    const auto offset = static_cast<std::uint32_t>(z.size());
    put_le(z, 0x04034b50, 4); put_le(z, 20, 2); put_le(z, 0, 2); put_le(z, 0, 2); put_le(z, 0, 2); put_le(z, 0x21, 2);
    put_le(z, crc, 4); put_le(z, static_cast<std::uint32_t>(data.size()), 4); put_le(z, static_cast<std::uint32_t>(data.size()), 4);
    put_le(z, static_cast<std::uint32_t>(name.size()), 2); put_le(z, 0, 2);
    z.insert(z.end(), name.begin(), name.end());
    z.insert(z.end(), data.begin(), data.end());
    put_le(cd, 0x02014b50, 4); put_le(cd, 20, 2); put_le(cd, 20, 2); put_le(cd, 0, 2); put_le(cd, 0, 2); put_le(cd, 0, 2);
    put_le(cd, 0x21, 2); put_le(cd, crc, 4); put_le(cd, static_cast<std::uint32_t>(data.size()), 4);
    put_le(cd, static_cast<std::uint32_t>(data.size()), 4); put_le(cd, static_cast<std::uint32_t>(name.size()), 2);
    put_le(cd, 0, 2); put_le(cd, 0, 2); put_le(cd, 0, 2); put_le(cd, 0, 2); put_le(cd, 0, 4); put_le(cd, offset, 4);
    cd.insert(cd.end(), name.begin(), name.end());
  }
  const auto cd_offset = static_cast<std::uint32_t>(z.size());
  z.insert(z.end(), cd.begin(), cd.end());
  put_le(z, 0x06054b50, 4); put_le(z, 0, 2); put_le(z, 0, 2);
  put_le(z, static_cast<std::uint32_t>(entries.size()), 2); put_le(z, static_cast<std::uint32_t>(entries.size()), 2);
  put_le(z, static_cast<std::uint32_t>(cd.size()), 4); put_le(z, cd_offset, 4); put_le(z, 0, 2);
  return z;
}

/// A minimal WordprocessingML document with `paragraphs` paragraphs.
Bytes make_docx(int paragraphs) {
  std::string body;
  for (int i = 0; i < paragraphs; ++i) {
    body += "<w:p><w:r><w:t>Paragraph " + std::to_string(i + 1) +
            ": the quick brown fox jumps over the lazy dog, again and again.</w:t></w:r></w:p>";
  }
  return stored_zip({
      {"[Content_Types].xml",
       R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>)"},
      {"_rels/.rels",
       R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)"},
      {"word/document.xml",
       R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>)" +
           body + "</w:body></w:document>"},
  });
}

std::vector<std::vector<std::string>> csv_rows(const std::string& text) {
  std::vector<std::vector<std::string>> rows;
  std::size_t p = 0;
  while (p < text.size()) {
    std::size_t nl = text.find('\n', p);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(p, nl - p);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) {
      std::vector<std::string> cells;
      std::size_t q = 0;
      for (;;) {
        const std::size_t c = line.find(',', q);
        cells.push_back(line.substr(q, c == std::string::npos ? std::string::npos : c - q));
        if (c == std::string::npos) break;
        q = c + 1;
      }
      rows.push_back(cells);
    }
    p = nl + 1;
  }
  return rows;
}

bool same_cell(const std::string& expected, const Json& v) {
  if (v.is_string()) return v.str() == expected;
  if (v.is_number()) {
    char* end = nullptr;
    const double d = std::strtod(expected.c_str(), &end);
    return end && *end == '\0' && std::fabs(d - v.as_number()) < 1e-9;
  }
  return false;
}

const std::string kCsv = "name,qty,price\nalpha,1,2.5\nbeta,20,0.75\ngamma,300,10\n";

}  // namespace

TEST_CASE("e2e/media/image: a lossy target is smaller at lower quality and comes back as a PNG of the same size") {
  Session s("media-image");
  const Json cat = catalogue(s);
  const Json* png = format(cat, "png");
  if (!png || !(*png)["readable"].as_bool()) SKIP("no PNG reader in this catalogue");
  s.write_input("photo.png", make_png(160, 120, 5));
  const Discovery d = discover(s, s.in("photo.png"), "png");
  // The first image target (core order, not PNG) whose schema has a numeric quality.
  std::string target;
  Json quality;
  for (const auto& t : d.targets) {
    const Json* f = format(cat, t);
    if (!target.empty() || !f || (*f)["category"].str() != "image" || (*f)["mime"].str() == "image/png") continue;
    for (const Json& spec : d.by_target.at(t)["optionSpecs"].items()) {
      if (spec["key"].str() == "quality" && spec.contains("min") && spec.contains("max")) {
        target = t;
        quality = spec;
      }
    }
  }
  if (target.empty()) SKIP("no image target with a quality option");
  INFO("target ", target);
  const auto set_quality = [&](double v) {
    Json steps = Json::array();
    steps.push_back(step({{"op", "setOption"}, {"index", 0}, {"key", "quality"}, {"value", v}}));
    return steps;
  };
  const Converted hi = convert(s, "photo.png", target, set_quality(quality["max"].as_number()), "high");
  const Converted lo = convert(s, "photo.png", target, set_quality(std::max(quality["min"].as_number(), 5.0)), "low");
  CHECK(lo.data.size() < hi.data.size());
  // Back to PNG through the app: same dimensions.
  s.write_input("again-" + hi.name, hi.data);
  const Discovery back = discover(s, s.in("again-" + hi.name), "back");
  const std::string to_png = target_with_mime(cat, back.targets, "image/png");
  if (to_png.empty()) SKIP(target, " cannot be converted back to PNG");
  const Converted round = convert(s, "again-" + hi.name, to_png, Json::array(), "round");
  const auto size = png_size(round.data);
  REQUIRE(size.has_value());
  CHECK(size->first == 160u);
  CHECK(size->second == 120u);
}

TEST_CASE("e2e/media/audio: WAV to another audio format and back keeps the duration") {
  Session s("media-audio");
  const Json cat = catalogue(s);
  const Json* wav = format(cat, "wav");
  if (!wav || !(*wav)["readable"].as_bool()) SKIP("no WAV reader in this catalogue");
  const std::string wav_mime = (*wav)["mime"].str();
  s.write_input("tone.wav", make_wav(16000, 1.0, 440.0));
  const Discovery d = discover(s, s.in("tone.wav"), "wav");
  const std::string target = target_where(cat, d.targets, [&](const Json& f) {
    return f["category"].str() == "audio" && f["mime"].str() != wav_mime && f["writable"].as_bool();
  });
  if (target.empty()) SKIP("WAV has no other audio target");
  INFO("target ", target);
  const Converted out = convert(s, "tone.wav", target, Json::array(), "encoded");
  REQUIRE(out.data.size() > 16u);
  CHECK_FALSE(parse_wav(out.data).has_value());  // really another container
  s.write_input("back-" + out.name, out.data);
  const Discovery back = discover(s, s.in("back-" + out.name), "back");
  const std::string to_wav = target_with_mime(cat, back.targets, wav_mime);
  if (to_wav.empty()) SKIP(target, " cannot be decoded back to WAV");
  const Converted round = convert(s, "back-" + out.name, to_wav, Json::array(), "decoded");
  const auto info = parse_wav(round.data);
  REQUIRE(info.has_value());
  CHECK(info->channels >= 1u);
  // Codecs may pad a frame or two at either end; a second stays about a second.
  CHECK(info->seconds() > 0.9);
  CHECK(info->seconds() < 1.2);
}

TEST_CASE("e2e/media/video: a short clip becomes an animated GIF (or a still frame)") {
  Session s("media-video");
  const Json cat = catalogue(s);
  s.write_input("clip.y4m", make_y4m(32, 24, 10));
  const Discovery d = discover(s, s.in("clip.y4m"), "clip");
  const Json* src = format(cat, d.item["source"].str());
  if (!src || (*src)["category"].str() != "video") SKIP("YUV4MPEG2 video is not in this catalogue");
  std::string gif = target_with_mime(cat, d.targets, "image/gif");
  if (!gif.empty()) {
    const Converted out = convert(s, "clip.y4m", gif, Json::array(), "gif");
    const auto frames = gif_frames(out.data);
    REQUIRE_MESSAGE(frames.has_value(), "the GIF block structure does not parse");
    CHECK(*frames >= 2);
    CHECK(le16(out.data, 6) == 32u);
    CHECK(le16(out.data, 8) == 24u);
    return;
  }
  const std::string png = target_with_mime(cat, d.targets, "image/png");
  if (png.empty()) SKIP("the clip converts to neither GIF nor PNG");
  const Converted out = convert(s, "clip.y4m", png, Json::array(), "frame");
  const auto size = png_size(out.data);
  REQUIRE(size.has_value());
  CHECK(size->first == 32u);
  CHECK(size->second == 24u);
}

TEST_CASE("e2e/media/documents: Markdown and DOCX become PDF with the expected pages") {
  Session s("media-pdf");
  const Json cat = catalogue(s);
  std::string md;
  for (int i = 0; i < 140; ++i) {
    md += "Paragraph " + std::to_string(i + 1) +
          ". Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore.\n\n";
  }
  s.write_input("long.md", "# A long document\n\n" + md);
  s.write_input("short.docx", make_docx(6));
  int converted = 0;
  for (const auto& [input, min_pages] : std::vector<std::pair<std::string, int>>{{"long.md", 2}, {"short.docx", 1}}) {
    INFO("input ", input);
    const Discovery d = discover(s, s.in(input), input);
    const std::string pdf = target_with_mime(cat, d.targets, "application/pdf");
    if (pdf.empty()) {
      MESSAGE(input, " cannot become PDF in this catalogue");
      continue;
    }
    const Converted out = convert(s, input, pdf, Json::array(), "pdf-" + input);
    REQUIRE(out.data.size() > 64u);
    CHECK(std::string(out.data.begin(), out.data.begin() + 5) == "%PDF-");
    const std::string tail(out.data.end() - std::min<std::ptrdiff_t>(64, static_cast<std::ptrdiff_t>(out.data.size())), out.data.end());
    CHECK(tail.find("%%EOF") != std::string::npos);
    CHECK(pdf_pages(out.data) >= min_pages);
    ++converted;
  }
  if (converted == 0) SKIP("no document converts to PDF in this catalogue");
}

TEST_CASE("e2e/media/tables: CSV to a spreadsheet and back, and to JSON, cell for cell") {
  Session s("media-tables");
  const Json cat = catalogue(s);
  s.write_input("table.csv", kCsv);
  const Discovery d = discover(s, s.in("table.csv"), "csv");
  const Json* src = format(cat, d.item["source"].str());
  if (!src || !(*src)["readable"].as_bool()) SKIP("CSV is not readable in this catalogue");
  const std::string csv_mime = (*src)["mime"].str();
  const auto expected = csv_rows(kCsv);
  bool any = false;

  const std::string xlsx =
      target_with_mime(cat, d.targets, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
  if (!xlsx.empty()) {
    any = true;
    const Converted sheet = convert(s, "table.csv", xlsx, Json::array(), "sheet");
    CHECK(sheet.data.size() > 4u);
    CHECK((sheet.data[0] == 'P' && sheet.data[1] == 'K'));
    CHECK(parse_zip(sheet.data).has_value());
    s.write_input("back-" + sheet.name, sheet.data);
    const Discovery back = discover(s, s.in("back-" + sheet.name), "back");
    const std::string to_csv = target_with_mime(cat, back.targets, csv_mime);
    REQUIRE_FALSE(to_csv.empty());
    const Converted round = convert(s, "back-" + sheet.name, to_csv, Json::array(), "round");
    CHECK(csv_rows(to_string(round.data)) == expected);
  }

  const std::string json = target_with_mime(cat, d.targets, "application/json");
  if (!json.empty()) {
    any = true;
    const Converted out = convert(s, "table.csv", json, Json::array(), "json");
    const auto doc = Json::parse(to_string(out.data));
    REQUIRE(doc.has_value());
    REQUIRE(doc->is_array());
    const auto& rows = doc->items();
    if (!rows.empty() && rows[0].is_object()) {
      // Records keyed by the header.
      REQUIRE(rows.size() == expected.size() - 1);
      for (std::size_t r = 0; r < rows.size(); ++r) {
        for (std::size_t c = 0; c < expected[0].size(); ++c) {
          INFO("row ", r, " column ", expected[0][c]);
          CHECK(same_cell(expected[r + 1][c], rows[r][expected[0][c]]));
        }
      }
    } else {
      // Rows of cells, header included.
      REQUIRE(rows.size() == expected.size());
      for (std::size_t r = 0; r < rows.size(); ++r) {
        REQUIRE(rows[r].size() == expected[r].size());
        for (std::size_t c = 0; c < expected[r].size(); ++c) CHECK(same_cell(expected[r][c], rows[r][c]));
      }
    }
  }
  if (!any) SKIP("CSV converts to neither a spreadsheet nor JSON in this catalogue");
}

TEST_CASE("e2e/media/limits: an image too large to decode safely is refused with the size-limit message") {
  Session s("media-limit");
  const Json cat = catalogue(s);
  const Json* png = format(cat, "png");
  if (!png || !(*png)["readable"].as_bool()) SKIP("no PNG reader in this catalogue");
  // 60000 x 60000 pixels (3.6 billion): above the core's pixel limit, so it
  // must be refused before anything is allocated.
  s.write_input("huge.png", png_header_only(60000, 60000));
  const Discovery d = discover(s, s.in("huge.png"), "huge");
  const std::string target = target_where(cat, d.targets, [](const Json& f) {
    return f["category"].str() == "image" && f["mime"].str() != "image/png" && f["writable"].as_bool();
  });
  if (target.empty()) SKIP("no other image target for PNG");
  const Converted r = convert(s, "huge.png", target, Json::array(), "limit", false);
  CHECK(r.item["status"].str() == "failed");
  CHECK(r.item["errorStatus"].as_int() == 8);  // CVT_ERR_LIMIT
  CHECK(r.item["errorTitle"].str() == "This file is too large to convert safely");
  const Json st = s.state("limit.json");
  bool shown = false;
  for (const Json& n : st["accessibility"].items()) {
    shown = shown || n["label"].str().rfind("This file is too large to convert safely.", 0) == 0;
  }
  CHECK(shown);
}
