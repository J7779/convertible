// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Text classifiers: XML dialects (root element and namespace, as defined by
// each dialect's schema), HTML, JSON dialects (top-level members required by
// the GeoJSON RFC 7946, TopoJSON, glTF 2.0, nbformat, HAR 1.2, Lottie, W3C
// Web App Manifest, JSON-LD ...), and line-oriented text formats whose
// specifications fix their first line or record syntax (PEM RFC 7468, vCard
// RFC 6350, iCalendar RFC 5545, WebVTT, SubRip, SSA/ASS, M3U/HLS RFC 8216,
// RFC 5322 mail, mbox, unified diff, SAM, VCF, FASTA/FASTQ, PDB, CIF ...).
// The source-code heuristics at the end are deliberately conservative: they
// only fire on several independent tell-tale constructs and stay below every
// signature, so that a renamed or unnamed snippet is merely hinted at.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/known_formats_sniff_util.hpp"

namespace cvt::sniff::detail {

using namespace std::string_view_literals;

namespace {

constexpr std::size_t npos = std::string_view::npos;

// First line of `text`.
std::string_view first_line(std::string_view text) {
  const std::size_t eol = text.find('\n');
  std::string_view line = eol == npos ? text : text.substr(0, eol);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return line;
}

// The first line that is neither blank nor a comment starting with `comment`.
std::string_view first_meaningful_line(std::string_view text, std::string_view comment) {
  std::string_view result;
  for_each_line(text, 200, [&](std::string_view line) {
    const std::string_view t = trim(line);
    if (t.empty() || (!comment.empty() && t.starts_with(comment))) return true;
    result = t;
    return false;
  });
  return result;
}

}  // namespace

// ============================================================================ text head

void decode_head(ByteView data, TextHead& head) noexcept {
  const View v{data};
  head.length = 0;
  head.text = false;
  head.utf16 = false;
  std::size_t at = 0;
  int order = 0;  // 0: 8-bit, 1: UTF-16LE, 2: UTF-16BE
  if (v.eq(0, "\xEF\xBB\xBF"sv)) {
    at = 3;
  } else if (v.eq(0, "\xFF\xFE"sv) && !v.eq(0, "\xFF\xFE\x00\x00"sv)) {
    at = 2;
    order = 1;
  } else if (v.eq(0, "\xFE\xFF"sv)) {
    at = 2;
    order = 2;
  } else if (v.size() >= 4 && v.u8(0) != 0 && v.u8(1) == 0 && v.u8(2) != 0 && v.u8(3) == 0) {
    order = 1;  // ASCII-heavy UTF-16LE without a BOM ("<\0?\0x\0m\0l\0")
  } else if (v.size() >= 4 && v.u8(0) == 0 && v.u8(1) != 0 && v.u8(2) == 0 && v.u8(3) != 0) {
    order = 2;
  }
  std::size_t controls = 0;
  bool nul = false;
  // Binary data usually shows a NUL or many control bytes at once: stop
  // there instead of decoding the whole sample.
  if (order == 0) {
    const std::size_t count = std::min(v.size() - at, TextHead::capacity);
    for (std::size_t i = 0; i < count; ++i) {
      const auto c = static_cast<char>(v.u8(at + i));
      const auto u = static_cast<unsigned char>(c);
      if (c == '\0') {
        nul = true;
        break;
      }
      if (u < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\x1B' && ++controls > 4 && controls * 50 > i + 1) {
        break;
      }
      head.buffer[head.length++] = c;
    }
    head.complete = v.size() - at <= TextHead::capacity;
  } else {
    head.utf16 = true;
    std::size_t i = at;
    for (; i + 1 < v.size() && head.length < TextHead::capacity; i += 2) {
      const std::uint16_t c = order == 1 ? v.u16le(i) : v.u16be(i);
      if (c == 0) {
        nul = true;
        break;
      }
      if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f') ++controls;
      head.buffer[head.length++] = c < 0x80 ? static_cast<char>(c) : '\x80';
    }
    head.complete = i + 1 >= v.size();
  }
  head.text = head.length > 0 && !nul && controls * 50 <= head.length;
  if (head.text && order == 0) {
    // Reject 8-bit data that is not UTF-8 when it has many high bytes (binary).
    std::size_t high = 0;
    for (std::size_t i = 0; i < head.length; ++i) high += static_cast<unsigned char>(head.buffer[i]) >= 0x80 ? 1 : 0;
    if (high * 4 > head.length) {
      // Accept valid UTF-8 (a mostly non-Latin text).
      std::size_t i = 0;
      bool valid = true;
      while (i < head.length && valid) {
        const auto b = static_cast<unsigned char>(head.buffer[i]);
        std::size_t extra = b < 0x80 ? 0 : (b >> 5) == 0x6 ? 1 : (b >> 4) == 0xE ? 2 : (b >> 3) == 0x1E ? 3 : 9;
        if (extra == 9) valid = false;
        for (std::size_t k = 1; k <= extra && valid; ++k) {
          if (i + k >= head.length) break;  // cut by the sample
          valid = (static_cast<unsigned char>(head.buffer[i + k]) >> 6) == 0x2;
        }
        i += extra + 1;
      }
      head.text = valid;
    }
  }
}

// ============================================================================ markup

namespace {

struct Markup {
  bool declaration = false;
  std::string_view doctype;     // text after "<!DOCTYPE"
  std::string_view root;        // qualified name of the root element
  std::string_view local;       // local part of the root name
  std::string_view start_tag;   // the root start tag up to '>' (or the end of the sample)
  std::string_view rest;        // everything from the root start tag on
};

bool name_char(char c) { return is_alnum(c) || c == '_' || c == '-' || c == '.' || c == ':'; }

bool parse_markup(std::string_view text, Markup& m) {
  std::size_t at = 0;
  const auto skip_space = [&] {
    while (at < text.size() && is_space(text[at])) ++at;
  };
  for (int guard = 0; guard < 64; ++guard) {
    skip_space();
    if (at >= text.size() || text[at] != '<') return false;
    const std::string_view rest = text.substr(at);
    if (rest.starts_with("<?")) {
      if (rest.starts_with("<?xml") && at == 0) m.declaration = true;
      if (rest.starts_with("<?xml ") || rest.starts_with("<?xml\t") || rest.starts_with("<?xml\n")) m.declaration = true;
      const std::size_t end = rest.find("?>");
      if (end == npos) return false;
      at += end + 2;
    } else if (rest.starts_with("<!--")) {
      const std::size_t end = rest.find("-->");
      if (end == npos) return false;
      at += end + 3;
    } else if (istarts(rest, "<!DOCTYPE")) {
      std::size_t end = rest.find('>');
      const std::size_t bracket = rest.find('[');
      if (bracket != npos && (end == npos || bracket < end)) {
        const std::size_t close = rest.find("]>", bracket);
        end = close == npos ? npos : close + 1;
      }
      if (end == npos) return false;
      m.doctype = trim(rest.substr(9, end - 9));
      at += end + 1;
    } else {
      std::size_t n = 1;
      while (n < rest.size() && name_char(rest[n])) ++n;
      if (n == 1) return false;
      m.root = rest.substr(1, n - 1);
      const std::size_t colon = m.root.rfind(':');
      m.local = colon == npos ? m.root : m.root.substr(colon + 1);
      const std::size_t close = rest.find('>');
      m.start_tag = rest.substr(0, close == npos ? rest.size() : close + 1);
      m.rest = rest;
      return true;
    }
  }
  return false;
}

// True when the start tag declares a namespace URI starting with `uri`.
bool declares(const Markup& m, std::string_view uri) {
  for (std::size_t at = m.start_tag.find("xmlns"); at != npos; at = m.start_tag.find("xmlns", at + 5)) {
    std::size_t q = m.start_tag.find_first_of("\"'", at);
    if (q == npos) return false;
    const char quote = m.start_tag[q];
    const std::size_t end = m.start_tag.find(quote, q + 1);
    if (end == npos) return false;
    if (m.start_tag.substr(q + 1, end - q - 1).starts_with(uri)) return true;
  }
  return false;
}

bool attribute_contains(const Markup& m, std::string_view text) { return m.start_tag.find(text) != npos; }

struct RootMapping {
  std::string_view local;
  std::string_view ns;  // required namespace prefix of a declared URI, or empty
  std::string_view id;
  int confidence;
};

constexpr RootMapping roots[] = {
    {"svg", "", "svg", 92},
    {"kml", "", "kml", 92},
    {"gpx", "", "gpx", 92},
    {"TrainingCenterDatabase", "", "tcx", 95},
    {"COLLADA", "", "dae", 95},
    {"X3D", "", "x3d", 95},
    {"rss", "", "rss", 92},
    {"feed", "http://www.w3.org/2005/Atom", "atom", 95},
    {"plist", "", "plist", 84},
    {"xliff", "", "xlf", 95},
    {"FictionBook", "", "fb2", 95},
    {"tt", "http://www.w3.org/ns/ttml", "ttml", 95},
    {"math", "", "mml", 90},
    {"score-partwise", "", "musicxml", 95},
    {"score-timewise", "", "musicxml", 95},
    {"playlist", "http://xspf.org/ns/0/", "xspf", 95},
    {"stylesheet", "http://www.w3.org/1999/XSL/Transform", "xslt", 95},
    {"transform", "http://www.w3.org/1999/XSL/Transform", "xslt", 95},
    {"schema", "http://www.w3.org/2001/XMLSchema", "xsd", 95},
    {"definitions", "http://schemas.xmlsoap.org/wsdl/", "wsdl", 95},
    {"definitions", "http://www.omg.org/spec/BPMN/", "bpmn", 95},
    {"smil", "", "smil", 90},
    {"project", "http://maven.apache.org/POM/", "pom", 95},
    {"graphml", "", "graphml", 95},
    {"gexf", "", "gexf", 95},
    {"opml", "", "opml", 95},
    {"speak", "", "ssml", 92},
    {"vxml", "", "vxml", 95},
    {"package", "http://www.idpf.org/2007/opf", "opf", 95},
    {"ncx", "", "ncx", 95},
    {"osm", "", "osm", 92},
    {"LandXML", "", "landxml", 95},
    {"mzML", "", "mzml", 95},
    {"indexedmzML", "", "mzml", 95},
    {"mzXML", "", "mzxml", 95},
    {"sbml", "", "sbml", 95},
    {"phyloxml", "", "phyloxml", 95},
    {"XMI", "", "xmi", 92},
    {"mxfile", "", "drawio", 95},
    {"TEI", "", "tei", 92},
    {"VisioDocument", "", "vdx", 95},
    {"Project", "http://schemas.microsoft.com/project", "mspdi", 95},
    {"amf", "", "amf", 90},
    {"abiword", "", "abw", 95},
    {"MPD", "urn:mpeg:dash:schema:mpd", "mpd", 95},
    {"SmoothStreamingMedia", "", "ismc", 95},
    {"manifest", "http://ns.adobe.com/f4m/", "f4m", 95},
    {"metalink", "", "metalink", 95},
    {"xfdf", "", "xfdf", 95},
    {"PMML", "", "pmml", 95},
    {"Workbook", "urn:schemas-microsoft-com:office:spreadsheet", "xmlss", 95},
    {"wordDocument", "http://schemas.microsoft.com/office/word/2003/wordml", "wordml", 95},
    {"MMC_ConsoleFile", "", "msc", 95},
    {"qgis", "", "qgs", 95},
    {"museScore", "", "mscx", 95},
    {"CityModel", "", "gml", 92},
    {"FeatureCollection", "http://www.opengis.net/gml", "gml", 92},
    {"FeatureCollection", "http://www.opengis.net/wfs", "gml", 90},
    {"Ableton", "", "als", 92},
    {"xmpmeta", "adobe:ns:meta/", "xmp", 95},
    {"nzb", "", "nzb", 95},
    {"en-export", "", "enex", 95},
    {"SCRIBUSUTF8NEW", "", "sla", 95},
    {"SCRIBUSUTF8", "", "sla", 95},
    {"fcpxml", "", "fcpxml", 95},
};

}  // namespace

Verdict classify_markup(ByteView data, std::string_view filename, std::string_view wanted) noexcept {
  (void)wanted;
  const View v{data};
  if (v.eq(0, "bplist0"sv)) return base_reading("plist", 84);
  TextHead head;
  decode_head(data, head);
  if (!head.text) return nothing();
  const std::string_view text = head.view();
  Markup m;
  if (!parse_markup(text, m)) {
    const std::string_view t = trim_left(text);
    if (istarts(t, "<!doctype html")) return found("html", 88);
    return container_only();
  }
  // HTML (and XHTML when served as XML).
  if (iequal(m.local, "html") || istarts(m.doctype, "html")) {
    if (m.declaration && declares(m, "http://www.w3.org/1999/xhtml")) return found("xhtml", 93);
    if (has_ext(filename, "xhtml") || has_ext(filename, "xht")) return found("xhtml", 90);
    return found("html", 88);
  }
  if (iequal(m.local, "SAMI")) return found("smi", 95);
  if (iequal(m.local, "asx")) return found("asx", 95);
  if (m.rest.empty()) return container_only();
  if (text.find("<?wpl") != npos && iequal(m.local, "smil")) return found("wpl", 95);
  // OpenDocument flat XML: the mimetype attribute names the document kind.
  if (m.local == "document" && declares(m, "urn:oasis:names:tc:opendocument:xmlns:office")) {
    if (attribute_contains(m, "opendocument.text")) return found("fodt", 95);
    if (attribute_contains(m, "opendocument.spreadsheet")) return found("fods", 95);
    if (attribute_contains(m, "opendocument.presentation")) return found("fodp", 95);
    if (attribute_contains(m, "opendocument.graphics")) return found("fodg", 95);
    return found("fodt", 85);
  }
  // Interface Builder documents.
  if (m.local == "document" && attribute_contains(m, "com.apple.InterfaceBuilder3")) {
    return found(attribute_contains(m, "Storyboard") ? "storyboard" : "xib", 95);
  }
  if (m.local == "package" && declares(m, "http://schemas.microsoft.com/packaging/")) return found("nuspec", 95);
  if (m.local == "project" && declares(m, "http://audacity.sourceforge.net/xml/")) return found("aup", 95);
  if (m.local == "RDF") {
    if (declares(m, "http://purl.org/rss/1.0/")) return found("rss", 92);
    return found(refine(filename, "rdf", {"owl"}), 90);
  }
  if (m.local == "map" && attribute_contains(m, "tiledversion")) return found("tmx", 95);
  if (m.local == "eagle") return found(refine(filename, "brd", {"sch", "lbr"}), 92);
  if (m.local == "workbook" && attribute_contains(m, "source-build")) return found("twb", 95);
  if ((m.local == "Window" || m.local == "UserControl" || m.local == "Page" || m.local == "ResourceDictionary" ||
       m.local == "Application") &&
      declares(m, "http://schemas.microsoft.com/winfx/2006/xaml/presentation")) {
    return found("xaml", 95);
  }
  if (m.local == "Project" && declares(m, "http://schemas.microsoft.com/developer/msbuild/")) {
    return found(refine(filename, "csproj", {"vbproj", "vcxproj", "fsproj"}), 90);
  }
  for (const RootMapping& r : roots) {
    if (r.confidence == 0 || m.local != r.local) continue;
    if (!r.ns.empty() && !declares(m, r.ns)) continue;
    if (r.id == "plist") return base_reading("plist", r.confidence);  // webloc, mobileconfig ... refine it by name
    return found(r.id, r.confidence);
  }
  // DTD-only identification.
  if (istarts(m.doctype, "svg")) return found("svg", 90);
  if (m.doctype.find("DocBook") != npos || declares(m, "http://docbook.org/ns/docbook")) return found("docbook", 92);
  if (m.declaration) return base_reading("xml", 70);
  if (m.start_tag.find("xmlns") != npos) return base_reading("xml", 65);
  return container_only();
}

// ============================================================================ JSON

namespace {

struct JsonKeys {
  std::array<std::string_view, 64> keys{};
  std::size_t count = 0;
  std::string_view type_value;     // value of a top-level "type" string member
  bool object = false;
  bool array = false;
  bool valid = true;               // no syntax error within the sample
  bool closed = false;             // the top-level value ended within the sample
  std::size_t end = 0;             // offset just after the top-level value (when closed)
  [[nodiscard]] bool has(std::string_view key) const {
    for (std::size_t i = 0; i < count; ++i) {
      if (keys[i] == key) return true;
    }
    return false;
  }
};

// Skip a JSON string starting at text[at] == '"'; returns the index after it, or npos.
std::size_t skip_string(std::string_view text, std::size_t at) {
  for (std::size_t i = at + 1; i < text.size(); ++i) {
    if (text[i] == '\\') {
      ++i;
    } else if (text[i] == '"') {
      return i + 1;
    } else if (static_cast<unsigned char>(text[i]) < 0x20) {
      return npos;
    }
  }
  return npos;
}

// Skip any JSON value; returns the index after it, npos on error, text.size() when cut off.
std::size_t skip_value(std::string_view text, std::size_t at, bool& error) {
  while (at < text.size() && is_space(text[at])) ++at;
  if (at >= text.size()) return text.size();
  const char c = text[at];
  if (c == '"') {
    const std::size_t end = skip_string(text, at);
    return end == npos ? text.size() : end;
  }
  if (c == '{' || c == '[') {
    int depth = 0;
    for (std::size_t i = at; i < text.size(); ++i) {
      const char d = text[i];
      if (d == '"') {
        const std::size_t end = skip_string(text, i);
        if (end == npos) return text.size();
        i = end - 1;
      } else if (d == '{' || d == '[') {
        ++depth;
      } else if (d == '}' || d == ']') {
        if (--depth == 0) return i + 1;
      }
    }
    return text.size();
  }
  if (c == '-' || is_digit(c)) {
    std::size_t i = at + 1;
    while (i < text.size() && (is_digit(text[i]) || text[i] == '.' || text[i] == 'e' || text[i] == 'E' || text[i] == '+' ||
                               text[i] == '-')) {
      ++i;
    }
    return i;
  }
  for (const std::string_view literal : {"true"sv, "false"sv, "null"sv}) {
    if (text.substr(at).starts_with(literal)) return at + literal.size();
    if (literal.starts_with(text.substr(at))) return text.size();
  }
  error = true;
  return npos;
}

JsonKeys scan_json(std::string_view text) {
  JsonKeys k;
  std::size_t at = 0;
  while (at < text.size() && is_space(text[at])) ++at;
  if (at >= text.size()) {
    k.valid = false;
    return k;
  }
  if (text[at] == '[') {
    k.array = true;
    bool error = false;
    const std::size_t end = skip_value(text, at, error);
    k.valid = !error;
    k.closed = !error && end < text.size() + 1 && end <= text.size() && text[end - 1] == ']';
    k.end = end;
    // The first element must itself be a valid value.
    std::size_t first = at + 1;
    while (first < text.size() && is_space(text[first])) ++first;
    if (first < text.size() && text[first] != ']') {
      bool inner_error = false;
      (void)skip_value(text, first, inner_error);
      if (inner_error) k.valid = false;
    }
    return k;
  }
  if (text[at] != '{') {
    k.valid = false;
    return k;
  }
  k.object = true;
  ++at;
  for (int member = 0; member < 100000; ++member) {
    while (at < text.size() && is_space(text[at])) ++at;
    if (at >= text.size()) return k;
    if (text[at] == '}') {
      k.closed = true;
      k.end = at + 1;
      return k;
    }
    if (text[at] != '"') {
      k.valid = false;
      return k;
    }
    const std::size_t key_end = skip_string(text, at);
    if (key_end == npos) return k;
    const std::string_view key = text.substr(at + 1, key_end - at - 2);
    if (k.count < k.keys.size()) k.keys[k.count++] = key;
    at = key_end;
    while (at < text.size() && is_space(text[at])) ++at;
    if (at >= text.size()) return k;
    if (text[at] != ':') {
      k.valid = false;
      return k;
    }
    ++at;
    while (at < text.size() && is_space(text[at])) ++at;
    if (key == "type" && at < text.size() && text[at] == '"') {
      const std::size_t end = skip_string(text, at);
      if (end != npos) k.type_value = text.substr(at + 1, end - at - 2);
    }
    bool error = false;
    at = skip_value(text, at, error);
    if (error) {
      k.valid = false;
      return k;
    }
    while (at < text.size() && is_space(text[at])) ++at;
    if (at >= text.size()) return k;
    if (text[at] == ',') {
      ++at;
    } else if (text[at] != '}') {
      k.valid = false;
      return k;
    }
  }
  return k;
}

}  // namespace

Verdict classify_json(ByteView data, std::string_view filename, std::string_view wanted) noexcept {
  (void)wanted;
  TextHead head;
  decode_head(data, head);
  if (!head.text) return nothing();
  const std::string_view text = head.view();
  const JsonKeys k = scan_json(text);
  if (!k.valid || (!k.object && !k.array)) return container_only();
  // JSON Lines: a complete first value on its own line, followed by another value.
  if (k.closed) {
    std::size_t at = k.end;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r')) ++at;
    if (at < text.size() && text[at] == '\n') {
      while (at < text.size() && is_space(text[at])) ++at;
      if (at < text.size() && (text[at] == '{' || text[at] == '[')) return found("jsonl", 88);
    } else if (at < text.size() && !is_space(text[at])) {
      return container_only();  // trailing garbage after the value
    } else if (at < text.size() && text[at] == '\x1E') {
      return container_only();
    }
  }
  if (k.object) {
    static constexpr std::string_view geometry_types[] = {"FeatureCollection", "Feature", "Point", "LineString", "Polygon",
                                                          "MultiPoint", "MultiLineString", "MultiPolygon", "GeometryCollection"};
    if (k.type_value == "Topology" && (k.has("objects") || k.has("arcs"))) return found("topojson", 95);
    if (std::ranges::find(geometry_types, k.type_value) != std::end(geometry_types) &&
        (k.has("features") || k.has("geometry") || k.has("coordinates") || k.has("geometries"))) {
      return found("geojson", 93);
    }
    if (k.has("asset") && (k.has("scenes") || k.has("nodes") || k.has("meshes") || k.has("buffers") || k.has("accessors"))) {
      return found("gltf", 95);
    }
    if (k.has("nbformat") || (k.count >= 1 && k.keys[0] == "cells" && data.size() > 0 &&
                              as_chars(data).substr(data.size() > 512 ? data.size() - 512 : 0).find("\"nbformat\"") != npos)) {
      return found("ipynb", 95);
    }
    if (k.has("log") && k.count <= 2 && text.find("\"entries\"") != npos) return found("har", 92);
    if (k.has("v") && k.has("fr") && k.has("ip") && k.has("op") && (k.has("layers") || k.has("w"))) return found("lottie", 92);
    if (k.type_value == "excalidraw") return found("excalidraw", 97);
    if (k.has("@context")) return found("jsonld", 88);
    if (k.has("openapi") || k.has("swagger")) return found("openapi", 90);
    if (k.has("tiledversion")) return found("tmj", 95);
    if (k.has("doc_id") && k.has("url")) return found(refine(filename, "gdoc", {"gsheet", "gslides"}), 92);
    if ((k.has("name") || k.has("short_name")) && !k.has("manifest_version") &&
        (k.has("start_url") || k.has("display") || k.has("background_color") || k.has("theme_color"))) {
      return found("webmanifest", 85);
    }
  }
  return base_reading("json", 80);
}

// ============================================================================ line-oriented text

namespace {

bool is_timestamp(std::string_view s, char fraction) {
  // hh:mm:ss<fraction>mmm (hours may have 1-3 digits)
  std::size_t i = 0;
  while (i < s.size() && i < 3 && is_digit(s[i])) ++i;
  if (i == 0 || s.size() < i + 10) return false;
  return s[i] == ':' && is_digit(s[i + 1]) && is_digit(s[i + 2]) && s[i + 3] == ':' && is_digit(s[i + 4]) &&
         is_digit(s[i + 5]) && s[i + 6] == fraction && is_digit(s[i + 7]) && is_digit(s[i + 8]) && is_digit(s[i + 9]);
}

bool all_digits(std::string_view s) {
  return !s.empty() && std::ranges::all_of(s, [](char c) { return is_digit(c); });
}

std::string_view interpreter_id(std::string_view line) {
  // "#!/usr/bin/env -S python3 -u" -> "python3"
  std::string_view rest = trim(line.substr(2));
  std::size_t end = rest.find_first_of(" \t");
  std::string_view program = rest.substr(0, end);
  program = leaf(program);
  if (program == "env") {
    rest = end == npos ? std::string_view{} : trim(rest.substr(end));
    while (rest.starts_with("-")) {
      end = rest.find_first_of(" \t");
      rest = end == npos ? std::string_view{} : trim(rest.substr(end));
    }
    end = rest.find_first_of(" \t");
    program = leaf(rest.substr(0, end));
  }
  while (!program.empty() && (is_digit(program.back()) || program.back() == '.')) program.remove_suffix(1);
  struct Interpreter {
    std::string_view name;
    std::string_view id;
  };
  static constexpr Interpreter table[] = {
      {"sh", "sh"},         {"bash", "sh"},     {"dash", "sh"},       {"ksh", "sh"},       {"zsh", "sh"},
      {"ash", "sh"},        {"fish", "fish"},   {"python", "py"},     {"pypy", "py"},      {"perl", "pl"},
      {"ruby", "rb"},       {"node", "js"},     {"nodejs", "js"},     {"deno", "js"},      {"bun", "js"},
      {"php", "php"},       {"lua", "lua"},     {"luajit", "lua"},    {"tclsh", "tcl"},    {"wish", "tcl"},
      {"Rscript", "r"},     {"pwsh", "ps1"},    {"powershell", "ps1"}, {"awk", "awk"},     {"gawk", "awk"},
      {"osascript", "applescript"}, {"groovy", "groovy"}, {"swift", "swift"}, {"julia", "jl"}, {"elixir", "ex"},
      {"escript", "erl"},   {"raku", "raku"},   {"crystal", "cr"},    {"scala", "scala"},
      {"guile", "scm"},     {"racket", "rkt"},  {"ts-node", "ts"},    {"runghc", "hs"},    {"runhaskell", "hs"},
      {"ocaml", "ml"},      {"make", "makefile"}, {"dart", "dart"},
  };
  for (const Interpreter& i : table) {
    if (program == i.name) return i.id;
  }
  return {};
}

std::string_view pem_label_id(std::string_view label) {
  if (label == "CERTIFICATE" || label == "TRUSTED CERTIFICATE" || label == "X509 CERTIFICATE") return "crt";
  if (label == "CERTIFICATE REQUEST" || label == "NEW CERTIFICATE REQUEST") return "csr";
  if (label.ends_with("PRIVATE KEY")) return "private-key";
  if (label == "X509 CRL") return "crl";
  if (label == "PKCS7" || label == "CMS") return "p7b";
  if (label.starts_with("PGP ")) return "asc";
  return "pem";
}

// Consistent delimiter counts across the first lines: CSV or TSV.
std::string_view delimited(std::string_view text, bool complete) {
  struct Count {
    char delimiter;
    std::size_t expected = 0;
    bool consistent = true;
  };
  std::array<Count, 3> counts{{{','}, {'\t'}, {';'}}};
  std::size_t lines = 0;
  std::size_t statement_ends = 0;  // lines ending in ";" are code (SQL, C ...), not a table
  for_each_line(text, 40, [&](std::string_view line) {
    if (line.empty()) return true;
    // Skip a last line that the sample cut in the middle.
    if (!complete && line.data() + line.size() == text.data() + text.size()) return false;
    ++lines;
    if (line.back() == ';') ++statement_ends;
    for (Count& c : counts) {
      std::size_t n = 0;
      bool quoted = false;
      for (const char ch : line) {
        if (ch == '"') quoted = !quoted;
        if (!quoted && ch == c.delimiter) ++n;
      }
      if (lines == 1) c.expected = n;
      else if (n != c.expected) c.consistent = false;
    }
    return true;
  });
  if (lines < 2) return {};
  if (counts[1].consistent && counts[1].expected >= 1) return "tsv";
  const std::size_t needed_lines = 3;
  if (counts[0].consistent && counts[0].expected >= 1 && (lines >= needed_lines || counts[0].expected >= 2)) return "csv";
  if (counts[2].consistent && counts[2].expected >= 1 && statement_ends * 2 < lines &&
      (lines >= needed_lines || counts[2].expected >= 2)) {
    return "csv";
  }
  return {};
}

// Conservative source-language hints for unnamed snippets.
std::string_view source_language(std::string_view text) {
  int py = 0, c_include = 0, cpp = 0, java = 0, cs = 0, go = 0, rs = 0, js = 0, ts = 0, swift = 0, sql = 0, kt = 0;
  bool go_package = false;
  for_each_line(text, 120, [&](std::string_view raw) {
    const std::string_view line = trim(raw);
    if (line.starts_with("def ") && line.ends_with(":")) py += 2;
    if (line.starts_with("import ") && line.find(';') == npos && line.find('"') == npos && line.find('\'') == npos) ++py;
    if (line.starts_with("from ") && line.find(" import ") != npos) py += 2;
    if (line.starts_with("class ") && line.ends_with(":")) py += 2;
    if (line.starts_with("if __name__ == ")) py += 3;
    if (line.starts_with("#include <") || line.starts_with("#include \"")) ++c_include;
    if (line.find("std::") != npos || line.starts_with("namespace ") || line.starts_with("template <") ||
        line.starts_with("template<") || line.starts_with("using namespace ")) {
      ++cpp;
    }
    if (line.starts_with("package ") && line.ends_with(";")) java += 2;
    if (line.starts_with("import java.") || line.starts_with("public class ") || line.starts_with("public static void main")) java += 2;
    if (line.starts_with("using System")) cs += 3;
    if (line.starts_with("package ") && line.find(';') == npos && line.find('.') == npos) go_package = true;
    if (line.starts_with("func ") || line == "import (") go += 2;
    if (line.starts_with("fn ") || line.starts_with("pub fn ") || line.starts_with("use std::") || line.starts_with("impl ") ||
        line.starts_with("let mut ")) {
      rs += 2;
    }
    if (line.starts_with("function ") || line.find("require(") != npos || line.starts_with("module.exports") ||
        line.starts_with("export default") || line.starts_with("export function") || line.starts_with("const ")) {
      ++js;
    }
    if (line.starts_with("interface ") || line.find(": string") != npos || line.find(": number") != npos ||
        line.starts_with("export interface") || line.starts_with("type ")) {
      ++ts;
    }
    if (line == "import Foundation" || line == "import SwiftUI" || line == "import UIKit") swift += 3;
    if (line.starts_with("fun ") || line.starts_with("val ")) ++kt;
    if (istarts(line, "create table ") || istarts(line, "insert into ") || istarts(line, "select ") || istarts(line, "alter table ") ||
        istarts(line, "create index ") || istarts(line, "drop table ")) {
      ++sql;
    }
    return true;
  });
  if (go_package) go += 2;
  if (swift >= 3 && go < 4) return "swift";
  if (cs >= 3) return "cs";
  if (java >= 4) return "java";
  if (go >= 4) return "go";
  if (rs >= 4) return "rs";
  if (c_include >= 1 && cpp >= 1) return "cpp";
  if (c_include >= 2) return "c";
  if (py >= 4) return "py";
  if (sql >= 2) return "sql";
  if (kt >= 3 && js == 0) return "kt";
  if (js >= 3) return ts >= 2 ? "ts" : "js";
  return {};
}

// Everything a text rule looks at, decoded once per call.
struct Ctx {
  TextHead head;
  std::string_view text;
  std::string_view body;   // text without leading white space
  std::string_view line1;  // first line
  std::string_view name;   // last path component of the file name
  std::string_view filename;
};

using Rule = Verdict (*)(const Ctx&);

struct TextRule {
  std::string_view ids;  // space-separated ids the rule can produce
  Rule rule;
};

bool rule_produces(const TextRule& r, std::string_view id) {
  if (id.empty() || r.ids.find(id) == npos) return false;  // fast path: not even a substring
  std::string_view ids = r.ids;
  while (!ids.empty()) {
    const std::size_t space = ids.find(' ');
    if (ids.substr(0, space) == id) return true;
    if (space == npos) break;
    ids.remove_prefix(space + 1);
  }
  return false;
}

Verdict by_file_name(const Ctx& c) {
  const std::string_view n = c.name;
  if (n == "Makefile" || n == "makefile" || n == "GNUmakefile") return found("makefile", 95);
  if (n == "Dockerfile" || n == "Containerfile" || n.starts_with("Dockerfile.")) return found("dockerfile", 95);
  if (n == "CMakeLists.txt") return found("cmake", 95);
  if (n == "Gemfile" || n == "Rakefile" || n == "Podfile" || n == "Vagrantfile" || n == "Brewfile" || n == "Fastfile" ||
      n == "Guardfile") {
    return found("rb", 92);
  }
  if (n == "Jenkinsfile") return found("groovy", 92);
  if (n == "go.mod") return found("gomod", 95);
  if (n == ".gitignore") return found("gitignore", 95);
  if (n == ".gitattributes") return found("gitattributes", 95);
  if (n == ".dockerignore") return found("dockerignore", 95);
  if (n == ".editorconfig") return found("editorconfig", 95);
  if (n == ".htaccess") return found("htaccess", 95);
  if (n == ".env" || n.starts_with(".env.")) return found("env", 92);
  if (n == ".bashrc" || n == ".bash_profile" || n == ".profile" || n == ".zshrc" || n == ".zprofile") return found("sh", 92);
  return nothing();
}

Verdict by_shebang(const Ctx& c) {
  if (!c.line1.starts_with("#!")) return nothing();
  const std::string_view id = interpreter_id(c.line1);
  return id.empty() ? nothing() : found(id, 92);
}

Verdict by_pem(const Ctx& c) {
  const std::size_t pem = c.text.find("-----BEGIN ");
  if (pem == npos || (pem != 0 && c.text[pem - 1] != '\n')) return nothing();
  const std::size_t close = c.text.find("-----", pem + 11);
  if (close == npos || close - pem >= 80) return nothing();
  const std::string_view id = pem_label_id(c.text.substr(pem + 11, close - pem - 11));
  if ((id == "crt" || id == "private-key" || id == "csr" || id == "crl") && has_ext(c.filename, "pem")) return found("pem", 93);
  return found(id, 93);
}

Verdict by_ssh_key(const Ctx& c) {
  for (const std::string_view key : {"ssh-rsa AAAA"sv, "ssh-ed25519 AAAA"sv, "ssh-dss AAAA"sv, "ecdsa-sha2-nistp"sv,
                                     "sk-ssh-ed25519@openssh.com AAAA"sv, "sk-ecdsa-sha2-nistp256@openssh.com AAAA"sv}) {
    if (c.body.starts_with(key)) return found("ssh-pubkey", 92);
  }
  return nothing();
}

// Formats whose specification fixes the start of the file. Dispatched on
// the first character so that each call costs a handful of comparisons.
Verdict by_first_line(const Ctx& c) {
  const std::string_view b = c.body;
  const std::string_view l = c.line1;
  if (b.empty()) return nothing();
  if (l.starts_with(".TH ") || l.starts_with(".\\\"") || l.starts_with("'\\\"")) return found("roff", 88);
  switch (b[0]) {
    case '#':
      if (b.starts_with("#define ") && c.text.find("_width ") != npos && c.text.find("_bits[]") != npos) return found("xbm", 95);
      if (b.starts_with("#EXTM3U")) return found(c.text.find("#EXT-X-") != npos ? "m3u8" : refine(c.filename, "m3u", {"m3u8"}), 93);
      if (istarts(b, "#NEXUS")) return found("nex", 97);
      if (b.starts_with("# STOCKHOLM 1.0")) return found("sto", 97);
      if (b.starts_with("#VRML V2.0") || b.starts_with("#VRML V1.0")) return found("wrl", 97);
      if (b.starts_with("#X3D V")) return found("x3dv", 97);
      if (b.starts_with("#usda ")) return found("usda", 97);
      if (b.starts_with("# .PCD v")) return found("pcd", 92);
      if (b.starts_with("#version ") && b.size() > 9 && is_digit(b[9])) return found("glsl", 85);
      if (b.starts_with("#+TITLE:") || b.starts_with("#+title:")) return found("org", 90);
      if (b.starts_with("##fileformat=VCFv")) return found("vcf-variant", 97);
      if (b.starts_with("##gff-version")) return found("gff3", 97);
      if (b.starts_with("##TITLE=") && c.text.find("##JCAMP-DX=") != npos) return found("jdx", 95);
      break;
    case '<':
      if (b.starts_with("<?php")) return found("php", 93);
      if (b.starts_with("<REAPER_PROJECT")) return found("rpp", 97);
      if (b.starts_with("<region>") || b.starts_with("<group>") || b.starts_with("<control>")) return found("sfz", 88);
      break;
    case 'B':
    case 'b':
      if (istarts(b, "BEGIN:VCARD")) return found("vcf", 95);
      if (istarts(b, "BEGIN:VCALENDAR")) return found(c.text.find("VERSION:1.0") != npos ? "vcs" : "ics", 95);
      if (b.starts_with("Brain Vision Data Exchange Header File")) return found("vhdr", 97);
      break;
    case 'W':
      if (b.starts_with("WEBVTT")) return found("vtt", 95);
      if (b.starts_with("Windows Registry Editor Version 5.00")) return found("reg", 97);
      break;
    case 'R':
      if (b.starts_with("REGEDIT4")) return found("reg", 97);
      break;
    case '[':
      if (b.starts_with("[Script Info]")) return found("ass", 95);
      if (istarts(b, "[playlist]")) return found("pls", 95);
      if (b.starts_with("[InternetShortcut]")) return found("url", 95);
      if (b.starts_with("[Desktop Entry]")) return found("desktop", 95);
      if (istarts(b, "[version]") && ifind(c.text, "signature=") != npos && c.text.find('$') != npos) return found("inf", 90);
      if (b.starts_with("[Unit]") || b.starts_with("[Service]") || b.starts_with("[Socket]") || b.starts_with("[Timer]")) {
        return found("service", 85);
      }
      if (b.starts_with("[Event \"")) return found("pgn", 95);
      if (b.starts_with("[gd_scene")) return found("tscn", 97);
      if (b.starts_with("[gd_resource")) return found("tres", 97);
      break;
    case '%':
      if (b.starts_with("%YAML")) return found(c.text.find("tag:unity3d.com") != npos ? "unity" : "yaml", 95);
      if (b.starts_with("%%MatrixMarket")) return found("mtx", 97);
      if (b.starts_with("%FS")) return found("gbr", 88);
      break;
    case '-':
      if (b.starts_with("--- !tapi-tbd")) return found("tbd", 97);
      if (b.starts_with("--- ") && c.text.find("\n+++ ") != npos && c.text.find("\n@@ ") != npos) return found("diff", 92);
      if (b.starts_with("-module(")) return found("erl", 92);
      break;
    case 'd':
      if (b.starts_with("diff --git ") || b.starts_with("diff -u ")) return found("diff", 92);
      if (b.starts_with("digraph ") || b.starts_with("digraph{")) return found("gv", 88);
      if (b.starts_with("dn: ")) return found("ldif", 92);
      if (b.starts_with("defmodule ")) return found("ex", 88);
      break;
    case 'I':
      if (b.starts_with("Index: ")) return found("diff", 92);
      if (b.starts_with("ISO-10303-21;")) {
        if (c.text.find("FILE_SCHEMA (('IFC") != npos || c.text.find("FILE_SCHEMA(('IFC") != npos) return found("ifc", 97);
        return found("step", 95);
      }
      break;
    case '\\':
      if (b.starts_with("\\documentclass") || b.starts_with("\\begin{document}")) return found("tex", 90);
      if (b.starts_with("\\input texinfo")) return found("texi", 97);
      if (b.starts_with("\\version \"")) return found("ly", 92);
      break;
    case 'T':
      if (b.starts_with("TY  - ")) return found("ris", 95);
      break;
    case '@':
      for (const std::string_view entry : {"@article{"sv, "@book{"sv, "@inproceedings{"sv, "@misc{"sv, "@techreport{"sv,
                                           "@phdthesis{"sv, "@string{"sv, "@preamble{"sv, "@incollection{"sv, "@online{"sv}) {
        if (istarts(b, entry)) return found("bib", 90);
      }
      if (b.starts_with("@HD\tVN:") || b.starts_with("@SQ\tSN:")) return found("sam", 95);
      if (b.starts_with("@<TRIPOS>")) return found("mol2", 97);
      if (b.starts_with("@startuml") || b.starts_with("@startmindmap")) return found("puml", 97);
      if (istarts(b, "@echo off")) return found("bat", 92);
      break;
    case '0':
      if (b.starts_with("0 HEAD")) return found("ged", 97);
      break;
    case '(':
      if (b.starts_with("(;FF[") || b.starts_with("(;GM[") || b.starts_with("(;CA[")) return found("sgf", 93);
      if (b.starts_with("(kicad_pcb")) return found("kicad_pcb", 97);
      if (b.starts_with("(kicad_sch")) return found("kicad_sch", 97);
      if (b.starts_with("(* Content-type: application/vnd.wolfram.mathematica *)")) return found("nb", 95);
      if (b.starts_with("(ns ")) return found("clj", 88);
      break;
    case 'L':
      if (b.starts_with("LOCUS  ")) return found("gbk", 92);
      break;
    case 'H':
      if (b.starts_with("HIERARCHY")) return found("bvh", 95);
      break;
    case 'E':
      if (b.starts_with("EXP  0 ")) return found("e00", 95);
      break;
    case 'n':
    case 'N':
      if (istarts(b, "ncols") && ifind(c.text, "nrows") != npos && ifind(c.text, "cellsize") != npos) return found("asc-grid", 95);
      if (b.starts_with("Notebook[{")) return found("nb", 95);
      break;
    case 'P':
      if (b.starts_with("PROJCS[") || b.starts_with("PROJCRS[")) return found("prj", 92);
      break;
    case 'G':
      if (b.starts_with("GEOGCS[") || b.starts_with("GEOGCRS[")) return found("prj", 92);
      if (b.starts_with("G04 ") && c.text.find("%MO") != npos) return found("gbr", 88);
      break;
    case 'C':
      if (b.starts_with("COMPD_CS[")) return found("prj", 92);
      break;
    case '/':
      if (b.starts_with("// !$*UTF8*$!")) return found("pbxproj", 97);
      if (b.starts_with("//Maya ASCII")) return found("ma", 97);
      break;
    case 'M':
      if (b.starts_with("Microsoft Visual Studio Solution File")) return found("sln", 97);
      break;
    case 'c':
      if (b.starts_with("cmake_minimum_required(") || b.starts_with("cmake_minimum_required (")) return found("cmake", 92);
      break;
    case 's':
      if (b.starts_with("syntax = \"proto")) return found("proto", 93);
      if (b.starts_with("strict digraph")) return found("gv", 88);
      if (b.starts_with("sequenceDiagram")) return found("mmd", 88);
      break;
    case 'V':
      if (b.starts_with("VERSION .7") && c.text.find("FIELDS ") != npos) return found("pcd", 92);
      break;
    case 'v':
      if (b.starts_with("version: 1") && c.text.find("\ndn: ") != npos) return found("ldif", 92);
      break;
    case 'F':
      if (b.starts_with("Feature:")) return found("feature", 88);
      break;
    case 'X':
      if (b.starts_with("X:") && c.text.find("\nK:") != npos) return found("abc-music", 88);
      break;
    case 'g':
      if (b.starts_with("graph TD") || b.starts_with("graph LR")) return found("mmd", 88);
      if (b.starts_with("graph ") && b.find('{') != npos && b.find('{') < 80) return found("gv", 88);
      break;
    case 'f':
      if (b.starts_with("flowchart ")) return found("mmd", 88);
      break;
    case 'm':
      if (b.starts_with("module ") && l.ends_with(" where")) return found("hs", 88);
      break;
    default:
      break;
  }
  // Markers that may follow other lines.
  if (c.text.find("\n\\documentclass") != npos) return found("tex", 90);
  if ((c.text.find("DIVISION") != npos || c.text.find("division") != npos) &&
      ifind(first_meaningful_line(c.text, "*"), "IDENTIFICATION DIVISION") != npos) {
    return found("cob", 92);
  }
  return nothing();
}

Verdict by_mail(const Ctx& c) {
  const std::string_view text = c.text;
  if (c.body.starts_with("From ") && c.line1.size() > 5) {
    const std::size_t second = text.find('\n');
    if (second != npos) {
      const std::string_view next = first_line(text.substr(second + 1));
      const std::size_t colon = next.find(':');
      if (colon != npos && colon > 0 && colon < 40 && next.find(' ') > colon) return found("mbox", 92);
    }
  }
  int headers = 0;
  int known = 0;
  bool multipart_related = false;
  bool first_ok = true;
  std::string_view first;
  std::size_t index = 0;
  for_each_line(text, 40, [&](std::string_view line) {
    if (index++ == 0) first = line;
    if (line.empty()) return false;
    if (line[0] == ' ' || line[0] == '\t') return true;  // folded header
    const std::size_t colon = line.find(':');
    bool ok = colon != npos && colon > 0 && colon < 60;
    for (std::size_t i = 0; ok && i < colon; ++i) ok = is_alnum(line[i]) || line[i] == '-';
    if (!ok) {
      if (headers == 0) first_ok = false;
      return false;
    }
    ++headers;
    const std::string_view field = line.substr(0, colon);
    for (const std::string_view k : {"Received"sv, "Return-Path"sv, "From"sv, "To"sv, "Subject"sv, "Date"sv, "Message-ID"sv,
                                     "MIME-Version"sv, "Delivered-To"sv, "Reply-To"sv, "Cc"sv, "Content-Type"sv}) {
      if (iequal(field, k)) ++known;
    }
    if (iequal(field, "Content-Type") && ifind(line, "multipart/related") != npos) multipart_related = true;
    return true;
  });
  if (first_ok && headers >= 3 && known >= 3) return found(multipart_related ? "mhtml" : "eml", 88);
  if (all_digits(trim(first)) && has_ext(c.filename, "emlx")) return found("emlx", 90);
  return nothing();
}

Verdict by_subtitle_lines(const Ctx& c) {
  // SubRip: a counter line, then "hh:mm:ss,mmm --> hh:mm:ss,mmm".
  std::string_view counter;
  std::string_view timing;
  std::size_t seen = 0;
  for_each_line(c.text, 8, [&](std::string_view line) {
    const std::string_view t = trim(line);
    if (t.empty() && seen == 0) return true;
    ++seen;
    if (seen == 1) counter = t;
    if (seen == 2) timing = t;
    return seen < 2;
  });
  if (all_digits(counter) && is_timestamp(timing, ',') && timing.find("-->") != npos) return found("srt", 93);
  const std::string_view t = trim(c.line1);
  // YouTube SBV: "0:00:01.000,0:00:04.000"
  const std::size_t comma = t.find(',');
  if (comma != npos && comma >= 9 && t.size() >= 19) {
    const auto sbv_time = [](std::string_view s) {
      return s.size() >= 11 && s.size() <= 13 && is_digit(s[0]) && s.find(':') != npos && s[s.size() - 4] == '.';
    };
    if (sbv_time(t.substr(0, comma)) && sbv_time(t.substr(comma + 1))) return found("sbv", 88);
  }
  // MicroDVD: "{start}{end}text"
  if (t.size() > 4 && t[0] == '{' && is_digit(t[1])) {
    const std::size_t close = t.find('}');
    if (close != npos && close + 2 < t.size() && t[close + 1] == '{' && all_digits(t.substr(1, close - 1))) {
      return found("microdvd", 90);
    }
  }
  int tags = 0;
  for_each_line(c.text, 30, [&](std::string_view line) {
    if (line.size() >= 7 && line[0] == '[' && is_digit(line[1]) && is_digit(line[2]) && line[3] == ':' && is_digit(line[4])) ++tags;
    for (const std::string_view tag : {"[ti:"sv, "[ar:"sv, "[al:"sv, "[by:"sv, "[length:"sv}) {
      if (line.starts_with(tag)) ++tags;
    }
    return true;
  });
  if (tags >= 2) return found("lrc", 88);
  return nothing();
}

Verdict by_science_records(const Ctx& c) {
  const std::string_view text = c.text;
  const std::string_view body = c.body;
  if (body.starts_with("@")) {
    std::array<std::string_view, 4> record{};
    std::size_t n = 0;
    for_each_line(body, 4, [&](std::string_view line) {
      record[n++] = line;
      return true;
    });
    if (n == 4 && record[2].starts_with("+") && !record[1].empty() && record[1].size() == record[3].size() &&
        std::ranges::all_of(record[1], [](char ch) { return std::string_view("ACGTNacgtn.").find(ch) != npos; })) {
      return found("fastq", 92);
    }
  }
  if (body.starts_with(">")) {
    std::size_t sequence_lines = 0;
    bool ok = true;
    std::size_t index = 0;
    for_each_line(body, 20, [&](std::string_view line) {
      if (index++ == 0) return true;
      if (line.empty() || line.starts_with(">")) return true;
      if (!c.head.complete && line.data() + line.size() == text.data() + text.size()) return false;
      for (const char ch : line) {
        if (!is_alpha(ch) && ch != '*' && ch != '-') ok = false;
      }
      ++sequence_lines;
      return ok;
    });
    if (ok && sequence_lines >= 1) return found("fasta", 85);
  }
  int records = 0;
  for_each_line(text, 20, [&](std::string_view line) {
    for (const std::string_view r : {"HEADER"sv, "TITLE "sv, "COMPND"sv, "SOURCE"sv, "REMARK"sv, "ATOM  "sv, "HETATM"sv,
                                     "CRYST1"sv, "MODEL "sv, "EXPDTA"sv, "AUTHOR"sv}) {
      if (line.starts_with(r)) ++records;
    }
    return true;
  });
  if (records >= 3) return found("pdb", 88);
  if (first_meaningful_line(text, "#").starts_with("data_") && (text.find("\n_") != npos || text.find("\nloop_") != npos)) {
    return found("cif", 88);
  }
  std::array<std::string_view, 4> lines{};
  std::size_t n = 0;
  for_each_line(text, 4, [&](std::string_view line) {
    lines[n++] = line;
    return true;
  });
  if (n == 4 && (lines[3].find("V2000") != npos || lines[3].find("V3000") != npos)) {
    return found(text.find("$$$$") != npos ? "sdf" : "mol", 92);
  }
  return nothing();
}

Verdict by_geometry_lines(const Ctx& c) {
  const std::string_view text = c.text;
  const std::string_view t = trim(c.line1);
  if (t == "OFF" || t == "COFF" || t == "NOFF" || t == "STOFF") return found("off", 92);
  if (t == "0" && text.find("SECTION") != npos && text.find("SECTION") < 32) return found("dxf", 95);
  if (t == "999" && text.find("\nSECTION") != npos) return found("dxf", 92);
  int vertices = 0;
  int faces = 0;
  int materials = 0;
  int moves = 0;
  for_each_line(text, 200, [&](std::string_view line) {
    if (line.starts_with("v ") || line.starts_with("vt ") || line.starts_with("vn ")) ++vertices;
    if (line.starts_with("f ")) ++faces;
    if (line.starts_with("newmtl ")) ++materials;
    if (line.starts_with("G0 ") || line.starts_with("G1 ") || line.starts_with("G28") || line.starts_with("M104 ") ||
        line.starts_with("M109 ") || line.starts_with("G90") || line.starts_with("G21")) {
      ++moves;
    }
    return true;
  });
  if (materials >= 1 && vertices == 0) return found("mtl", 92);
  if (vertices >= 3 && faces >= 1) return found("obj", 90);
  if (moves >= 3 || (c.body.starts_with(";FLAVOR:") && moves >= 1)) return found("gcode", 88);
  return nothing();
}

Verdict by_records(const Ctx& c) {
  // Intel HEX and Motorola S-records: every line one record.
  std::size_t ihex = 0;
  std::size_t srec = 0;
  std::size_t lines = 0;
  bool file = false;
  bool track = false;
  for_each_line(c.text, 30, [&](std::string_view line) {
    const std::string_view t = trim(line);
    if (t.starts_with("FILE ")) file = true;
    if (t.starts_with("TRACK ")) track = true;
    if (line.empty() || lines >= 16) return true;
    if (!c.head.complete && line.data() + line.size() == c.text.data() + c.text.size()) return true;
    ++lines;
    const auto hex_tail = [](std::string_view s) { return !s.empty() && std::ranges::all_of(s, [](char ch) { return is_hex(ch); }); };
    if (line.size() >= 11 && line[0] == ':' && hex_tail(line.substr(1)) && line.size() % 2 == 1) ++ihex;
    if (line.size() >= 10 && line[0] == 'S' && is_digit(line[1]) && hex_tail(line.substr(2))) ++srec;
    return true;
  });
  if (lines >= 1 && ihex == lines) return found("ihex", 92);
  if (lines >= 2 && srec == lines && c.body.starts_with("S0")) return found("srec", 92);
  if (file && track) return found("cue", 92);
  return nothing();
}

Verdict by_code_markers(const Ctx& c) {
  const std::string_view b = c.body;
  if (b.starts_with("FROM ") || istarts(b, "# syntax=docker")) {
    if (c.text.find("\nRUN ") != npos || c.text.find("\nCOPY ") != npos || c.text.find("\nCMD ") != npos ||
        c.text.find("\nENTRYPOINT ") != npos) {
      return found("dockerfile", 85);
    }
  }
  // Objective-C or MATLAB for ".m" files.
  if (has_ext(c.filename, "m")) {
    if (c.text.find("#import") != npos || c.text.find("@interface") != npos || c.text.find("@implementation") != npos) {
      return found("m", 88);
    }
    if (b.starts_with("%") || b.starts_with("function ") || c.text.find("\nfunction ") != npos || c.text.find("\nend\n") != npos ||
        c.text.find("disp(") != npos) {
      return found("matlab", 88);
    }
  }
  return nothing();
}

Verdict by_delimiters(const Ctx& c) {
  // A delimiter layout is weak evidence: a name naming another text format wins.
  const std::string_view table = delimited(c.text, c.head.complete);
  return table.empty() ? nothing() : Verdict{table, 66, true, true};
}

Verdict by_source_language(const Ctx& c) {
  const std::string_view language = source_language(c.text);
  return language.empty() ? nothing() : Verdict{language, 62, true, true};
}

// In precedence order: earlier rules win when several match.
constexpr TextRule text_rules[] = {
    {"makefile dockerfile cmake rb groovy gomod gitignore gitattributes dockerignore editorconfig htaccess env sh", by_file_name},
    {"sh fish py pl rb js php lua tcl r ps1 awk applescript groovy swift jl ex erl raku cr scala scm rkt ts hs ml makefile dart",
     by_shebang},
    {"crt csr private-key crl p7b asc pem", by_pem},
    {"ssh-pubkey", by_ssh_key},
    {"xbm php vcf vcs ics vtt ass m3u m3u8 pls url desktop reg inf service unity yaml tbd diff tex texi ris bib roff ged pgn sgf "
     "vcf-variant gff3 sam gbk nex sto mol2 jdx vhdr mtx wrl x3dv usda ma bvh ifc step pcd e00 asc-grid prj kicad_pcb "
     "kicad_sch gbr pbxproj sln puml nb cmake proto bat glsl tscn tres rpp org ly erl ex clj hs cob feature ldif abc-music "
     "sfz gv mmd",
     by_first_line},
    {"mbox eml mhtml emlx", by_mail},
    {"srt sbv microdvd lrc", by_subtitle_lines},
    {"fastq fasta pdb cif sdf mol", by_science_records},
    {"off dxf mtl obj gcode", by_geometry_lines},
    {"ihex srec cue", by_records},
    {"dockerfile m matlab", by_code_markers},
    {"csv tsv", by_delimiters},
    {"swift cs java go rs cpp c py sql kt ts js", by_source_language},
};

}  // namespace

Verdict classify_text(ByteView data, std::string_view filename, std::string_view wanted) noexcept {
  Ctx c;
  decode_head(data, c.head);
  if (!c.head.text) return nothing();
  c.text = c.head.view();
  c.body = trim_left(c.text);
  c.line1 = first_line(c.text);
  c.name = leaf(filename);
  c.filename = filename;
  // Evaluate only the rule that can produce `wanted`; when it does, make sure
  // no earlier (stronger) rule claims the file first. Every other text format
  // costs one decode and one rule.
  std::size_t own = std::size(text_rules);
  for (std::size_t i = 0; i < std::size(text_rules); ++i) {
    if (rule_produces(text_rules[i], wanted)) {
      own = i;
      break;
    }
  }
  if (own == std::size(text_rules)) return container_only();
  // A rule may produce `wanted` only in a later rule too (rb by name, or by shebang).
  for (std::size_t i = own; i < std::size(text_rules); ++i) {
    if (!rule_produces(text_rules[i], wanted)) continue;
    const Verdict mine = text_rules[i].rule(c);
    if (mine.id != wanted) continue;
    for (std::size_t k = 0; k < i; ++k) {
      const Verdict earlier = text_rules[k].rule(c);
      if (!earlier.id.empty()) return earlier;
    }
    return mine;
  }
  return container_only();
}

}  // namespace cvt::sniff::detail
