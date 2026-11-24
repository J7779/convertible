// Convertible core - detection-only database of well-known file formats.
//
// This table makes `cvt_detect` and the format list useful for types no
// family implements (yet). It never overrides a family: when a family
// registers the same id, the family's description wins and this table only
// fills the fields the family left empty.
//
// HOW TO ADD A FORMAT: append one row to `known_formats` below.
//   id          lower-case canonical id (usually the main extension)
//   name        human name
//   category    Category::...
//   extensions  space separated, lower-case, no dots; the first is the default
//   mime        IANA media type, or "" when there is none
//   popularity  0..100 (png/pdf/zip ~95, common ~60-80, niche ~10-40)
//   signatures  up to 8 {offset, "pattern"} magic numbers; any one matching
//               identifies the format. Pattern grammar (checked at compile
//               time, see SignaturePattern in registry.hpp):
//                 89            hex byte          ??      any byte
//                 4? / ?F       one nibble        FF&E0   byte under a mask
//                 'RIFF'        ASCII text
//   sniff       optional function for formats that need a closer look
// A format that needs more than 8 signatures can simply have a second row
// with the same id. Formats without any signature are recognised by
// extension only.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/registry.hpp"
#include "families/families.hpp"
#include "util/text.hpp"

namespace cvt {

namespace {

struct KnownSignature {
  std::size_t offset = 0;
  SignaturePattern pattern{};
};

using Sniffer = int (*)(ByteView data, std::string_view filename);

struct KnownFormat {
  std::string_view id;
  std::string_view name;
  Category category = Category::Other;
  std::string_view extensions;
  std::string_view mime;
  int popularity = 0;
  KnownSignature signatures[8]{};
  Sniffer sniff = nullptr;
};

// --------------------------------------------------------------------------- sniffers

// ZIP-based formats carry no magic of their own (a generic "PK" signature
// here would make every unnamed ZIP look like the most popular of them); the
// entry names tell them apart. They are stored uncompressed in the local
// headers at the front and in the central directory at the back, so looking
// for a tell-tale member name near both ends is cheap and reliable.
bool zip_mentions(ByteView data, std::string_view member) {
  const std::string_view text = as_chars(data);
  if (!text.starts_with("PK\x03\x04")) return false;
  constexpr std::size_t window = 64 * 1024;
  if (text.substr(0, window).find(member) != std::string_view::npos) return true;
  return text.size() > window && text.substr(text.size() - window).find(member) != std::string_view::npos;
}

int sniff_docx(ByteView data, std::string_view) { return zip_mentions(data, "word/document") ? 92 : 0; }
int sniff_xlsx(ByteView data, std::string_view) { return zip_mentions(data, "xl/workbook") ? 92 : 0; }
int sniff_pptx(ByteView data, std::string_view) { return zip_mentions(data, "ppt/presentation") ? 92 : 0; }
int sniff_jar(ByteView data, std::string_view) { return zip_mentions(data, "META-INF/MANIFEST.MF") ? 80 : 0; }
int sniff_apk(ByteView data, std::string_view) { return zip_mentions(data, "AndroidManifest.xml") ? 88 : 0; }

// Matroska and WebM share the EBML magic; the DocType string tells them apart.
int sniff_ebml(ByteView data, std::string_view doctype) {
  const std::string_view text = as_chars(data);
  if (!text.starts_with("\x1A\x45\xDF\xA3")) return 0;
  return text.substr(0, 64).find(doctype) != std::string_view::npos ? 92 : 0;
}
int sniff_mkv(ByteView data, std::string_view) { return sniff_ebml(data, "matroska"); }
int sniff_webm(ByteView data, std::string_view) { return sniff_ebml(data, "webm"); }

// Java class files and "fat" Mach-O binaries both start with CA FE BA BE.
// What follows is a class-file version (major >= 45) or a small count of
// architectures.
std::uint32_t cafebabe_word(ByteView data) {
  util::ByteReader reader(data);
  if (reader.u32be() != 0xCAFEBABEu) return 0;
  const std::uint32_t word = reader.u32be();
  return reader.ok() ? word : 0;
}
int sniff_class(ByteView data, std::string_view) { return (cafebabe_word(data) & 0xFFFFu) >= 45 ? 85 : 0; }
int sniff_macho_fat(ByteView data, std::string_view) {
  const std::uint32_t count = cafebabe_word(data);
  return count >= 1 && count < 45 ? 85 : 0;
}

// Text-based markup: skip a BOM and leading whitespace, then look at how the
// document opens.
std::string_view text_head(ByteView data) {
  std::string_view text = as_chars(data).substr(0, 2048);
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  return util::trim_left(text);
}

bool head_contains(std::string_view head, std::string_view needle) {
  const auto it = std::ranges::search(head, needle, [](char a, char b) {
                    return util::ascii_lower(a) == util::ascii_lower(b);
                  }).begin();
  return it != head.end();
}

int sniff_xml(ByteView data, std::string_view) { return text_head(data).starts_with("<?xml") ? 70 : 0; }
int sniff_svg(ByteView data, std::string_view) {
  const std::string_view head = text_head(data);
  return head.starts_with("<") && head_contains(head, "<svg") ? 88 : 0;
}
int sniff_html(ByteView data, std::string_view) {
  const std::string_view head = text_head(data);
  if (!head.starts_with("<")) return 0;
  return util::istarts_with(head, "<!doctype html") || head_contains(head, "<html") ? 85 : 0;
}
int sniff_gpx(ByteView data, std::string_view) {
  const std::string_view head = text_head(data);
  return head.starts_with("<") && head_contains(head, "<gpx") ? 88 : 0;
}
int sniff_kml(ByteView data, std::string_view) {
  const std::string_view head = text_head(data);
  return head.starts_with("<") && head_contains(head, "<kml") ? 88 : 0;
}

// --------------------------------------------------------------------------- the table

using enum Category;

constexpr KnownFormat known_formats[] = {
    // ---- images ------------------------------------------------------------
    {"png", "PNG image", Image, "png", "image/png", 95, {{0, "89 'PNG' 0D 0A 1A 0A"}}},
    {"jpg", "JPEG image", Image, "jpg jpeg jpe jfif", "image/jpeg", 95, {{0, "FF D8 FF"}}},
    {"gif", "GIF image", Image, "gif", "image/gif", 85, {{0, "'GIF87a'"}, {0, "'GIF89a'"}}},
    {"webp", "WebP image", Image, "webp", "image/webp", 85, {{0, "'RIFF' ?? ?? ?? ?? 'WEBP'"}}},
    {"bmp", "Windows bitmap", Image, "bmp dib", "image/bmp", 60, {{0, "'BM'"}}},
    {"tiff", "TIFF image", Image, "tiff tif", "image/tiff", 65, {{0, "'II' 2A 00"}, {0, "'MM' 00 2A"}}},
    {"ico", "Windows icon", Image, "ico", "image/vnd.microsoft.icon", 50, {{0, "00 00 01 00"}}},
    {"heic", "HEIF/HEIC image", Image, "heic heif", "image/heic", 75,
     {{4, "'ftypheic'"}, {4, "'ftypheix'"}, {4, "'ftypmif1'"}, {4, "'ftypmsf1'"}, {4, "'ftyphevc'"}}},
    {"avif", "AVIF image", Image, "avif", "image/avif", 65, {{4, "'ftypavif'"}, {4, "'ftypavis'"}}},
    {"jxl", "JPEG XL image", Image, "jxl", "image/jxl", 35, {{0, "FF 0A"}, {0, "00 00 00 0C 'JXL ' 0D 0A 87 0A"}}},
    {"psd", "Adobe Photoshop document", Image, "psd", "image/vnd.adobe.photoshop", 55, {{0, "'8BPS'"}}},
    {"tga", "Truevision TGA image", Image, "tga", "image/x-tga", 30},
    {"svg", "SVG vector image", Vector, "svg", "image/svg+xml", 85, {}, sniff_svg},
    {"eps", "Encapsulated PostScript", Vector, "eps", "application/postscript", 30, {{0, "'%!PS-Adobe-' ?? '.' ?? ' EPSF'"}}},

    // ---- video -------------------------------------------------------------
    {"mp4", "MPEG-4 video", Video, "mp4 m4v", "video/mp4", 95,
     {{4, "'ftypisom'"}, {4, "'ftypiso2'"}, {4, "'ftypmp41'"}, {4, "'ftypmp42'"}, {4, "'ftypavc1'"}, {4, "'ftypM4V '"},
      {4, "'ftypdash'"}, {4, "'ftyp'"}}},
    {"mov", "QuickTime movie", Video, "mov qt", "video/quicktime", 80, {{4, "'ftypqt  '"}, {4, "'moov'"}}},
    {"mkv", "Matroska video", Video, "mkv mk3d", "video/x-matroska", 80, {}, sniff_mkv},
    {"webm", "WebM video", Video, "webm", "video/webm", 80, {}, sniff_webm},
    {"avi", "AVI video", Video, "avi", "video/x-msvideo", 65, {{0, "'RIFF' ?? ?? ?? ?? 'AVI '"}}},
    {"wmv", "Windows Media video", Video, "wmv asf", "video/x-ms-wmv", 45, {{0, "30 26 B2 75 8E 66 CF 11"}}},
    {"flv", "Flash video", Video, "flv", "video/x-flv", 30, {{0, "'FLV' 01"}}},
    {"mpg", "MPEG video", Video, "mpg mpeg", "video/mpeg", 45, {{0, "00 00 01 BA"}, {0, "00 00 01 B3"}}},
    {"3gp", "3GPP video", Video, "3gp 3g2", "video/3gpp", 30, {{4, "'ftyp3g'"}}},

    // ---- audio -------------------------------------------------------------
    {"mp3", "MP3 audio", Audio, "mp3", "audio/mpeg", 95,
     {{0, "'ID3'"}, {0, "FF FB"}, {0, "FF F3"}, {0, "FF F2"}, {0, "FF FA"}}},
    {"wav", "WAVE audio", Audio, "wav wave", "audio/wav", 85, {{0, "'RIFF' ?? ?? ?? ?? 'WAVE'"}}},
    {"flac", "FLAC audio", Audio, "flac", "audio/flac", 80, {{0, "'fLaC'"}}},
    {"ogg", "Ogg audio", Audio, "ogg oga", "audio/ogg", 65, {{0, "'OggS'"}}},
    {"opus", "Opus audio", Audio, "opus", "audio/opus", 55, {{28, "'OpusHead'"}}},
    {"m4a", "MPEG-4 audio", Audio, "m4a m4b", "audio/mp4", 80, {{4, "'ftypM4A '"}, {4, "'ftypM4B '"}}},
    {"aac", "AAC audio", Audio, "aac", "audio/aac", 60, {{0, "FF F1"}, {0, "FF F9"}}},
    {"aiff", "AIFF audio", Audio, "aiff aif aifc", "audio/aiff", 45,
     {{0, "'FORM' ?? ?? ?? ?? 'AIFF'"}, {0, "'FORM' ?? ?? ?? ?? 'AIFC'"}}},
    {"wma", "Windows Media audio", Audio, "wma", "audio/x-ms-wma", 35},
    {"mid", "MIDI music", Audio, "mid midi", "audio/midi", 40, {{0, "'MThd'"}}},

    // ---- archives and compression -------------------------------------------
    {"zip", "ZIP archive", Archive, "zip", "application/zip", 95, {{0, "'PK' 03 04"}, {0, "'PK' 05 06"}}},
    {"7z", "7-Zip archive", Archive, "7z", "application/x-7z-compressed", 80, {{0, "'7z' BC AF 27 1C"}}},
    {"rar", "RAR archive", Archive, "rar", "application/vnd.rar", 75, {{0, "'Rar!' 1A 07"}}},
    {"tar", "tar archive", Archive, "tar", "application/x-tar", 75, {{257, "'ustar'"}}},
    {"gz", "gzip compressed file", Archive, "gz", "application/gzip", 75, {{0, "1F 8B 08"}}},
    {"tar.gz", "gzip-compressed tar archive", Archive, "tar.gz tgz", "application/gzip", 70, {{0, "1F 8B 08"}}},
    {"bz2", "bzip2 compressed file", Archive, "bz2", "application/x-bzip2", 50, {{0, "'BZh'"}}},
    {"xz", "xz compressed file", Archive, "xz", "application/x-xz", 55, {{0, "FD '7zXZ' 00"}}},
    {"zst", "Zstandard compressed file", Archive, "zst", "application/zstd", 50, {{0, "28 B5 2F FD"}}},
    {"iso", "ISO 9660 disc image", Archive, "iso", "application/x-iso9660-image", 50, {{32769, "'CD001'"}}},
    {"cab", "Microsoft Cabinet archive", Archive, "cab", "application/vnd.ms-cab-compressed", 25, {{0, "'MSCF'"}}},
    {"deb", "Debian package", Archive, "deb", "application/vnd.debian.binary-package", 35,
     {{0, "'!<arch>' 0A 'debian-binary'"}}},
    {"jar", "Java archive", Archive, "jar", "application/java-archive", 40, {}, sniff_jar},
    {"apk", "Android package", Archive, "apk", "application/vnd.android.package-archive", 45, {},
     sniff_apk},
    {"dmg", "Apple disk image", Archive, "dmg", "application/x-apple-diskimage", 40},

    // ---- documents and e-books ------------------------------------------------
    {"pdf", "PDF document", Document, "pdf", "application/pdf", 98, {{0, "'%PDF-'"}}},
    {"docx", "Word document", Document, "docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
     95, {}, sniff_docx},
    {"doc", "Word 97-2003 document", Document, "doc", "application/msword", 60, {{0, "D0 CF 11 E0 A1 B1 1A E1"}}},
    {"odt", "OpenDocument text", Document, "odt", "application/vnd.oasis.opendocument.text", 55,
     {{30, "'mimetypeapplication/vnd.oasis.opendocument.text'"}}},
    {"rtf", "Rich Text Format", Document, "rtf", "application/rtf", 55, {{0, "'{\\rtf'"}}},
    {"html", "HTML page", Document, "html htm xhtml", "text/html", 90, {}, sniff_html},
    {"md", "Markdown", Document, "md markdown", "text/markdown", 85},
    {"tex", "LaTeX source", Document, "tex", "application/x-tex", 40},
    {"ps", "PostScript", Document, "ps", "application/postscript", 30, {{0, "'%!PS'"}}},
    {"djvu", "DjVu document", Document, "djvu djv", "image/vnd.djvu", 25, {{0, "'AT&TFORM'"}}},
    {"epub", "EPUB e-book", Ebook, "epub", "application/epub+zip", 80, {{30, "'mimetypeapplication/epub+zip'"}}},
    {"mobi", "Mobipocket e-book", Ebook, "mobi azw azw3", "application/x-mobipocket-ebook", 45, {{60, "'BOOKMOBI'"}}},
    {"fb2", "FictionBook e-book", Ebook, "fb2", "application/x-fictionbook+xml", 25},

    // ---- spreadsheets and presentations ----------------------------------------
    {"xlsx", "Excel workbook", Spreadsheet, "xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
     92, {}, sniff_xlsx},
    {"xls", "Excel 97-2003 workbook", Spreadsheet, "xls", "application/vnd.ms-excel", 55,
     {{0, "D0 CF 11 E0 A1 B1 1A E1"}}},
    {"ods", "OpenDocument spreadsheet", Spreadsheet, "ods", "application/vnd.oasis.opendocument.spreadsheet", 50,
     {{30, "'mimetypeapplication/vnd.oasis.opendocument.spreadsheet'"}}},
    {"csv", "Comma-separated values", Spreadsheet, "csv", "text/csv", 92},
    {"tsv", "Tab-separated values", Spreadsheet, "tsv tab", "text/tab-separated-values", 55},
    {"pptx", "PowerPoint presentation", Presentation, "pptx",
     "application/vnd.openxmlformats-officedocument.presentationml.presentation", 85, {}, sniff_pptx},
    {"ppt", "PowerPoint 97-2003 presentation", Presentation, "ppt", "application/vnd.ms-powerpoint", 45,
     {{0, "D0 CF 11 E0 A1 B1 1A E1"}}},
    {"odp", "OpenDocument presentation", Presentation, "odp", "application/vnd.oasis.opendocument.presentation", 40,
     {{30, "'mimetypeapplication/vnd.oasis.opendocument.presentation'"}}},

    // ---- structured data --------------------------------------------------------
    {"json", "JSON", Data, "json", "application/json", 92},
    {"xml", "XML", Data, "xml", "application/xml", 80, {}, sniff_xml},
    {"yaml", "YAML", Data, "yaml yml", "application/yaml", 80},
    {"toml", "TOML", Data, "toml", "application/toml", 60},
    {"ini", "INI configuration", Data, "ini cfg conf", "text/plain", 45},
    {"plist", "Apple property list", Data, "plist", "application/x-plist", 35, {{0, "'bplist00'"}}},
    {"sqlite", "SQLite database", Data, "sqlite sqlite3 db", "application/vnd.sqlite3", 60,
     {{0, "'SQLite format 3' 00"}}},

    // ---- fonts ------------------------------------------------------------------
    {"ttf", "TrueType font", Font, "ttf", "font/ttf", 80, {{0, "00 01 00 00 00"}, {0, "'true'"}}},
    {"otf", "OpenType font", Font, "otf", "font/otf", 75, {{0, "'OTTO'"}}},
    {"woff", "Web Open Font Format", Font, "woff", "font/woff", 65, {{0, "'wOFF'"}}},
    {"woff2", "Web Open Font Format 2", Font, "woff2", "font/woff2", 70, {{0, "'wOF2'"}}},

    // ---- subtitles ----------------------------------------------------------------
    {"srt", "SubRip subtitles", Subtitle, "srt", "application/x-subrip", 85},
    {"vtt", "WebVTT subtitles", Subtitle, "vtt", "text/vtt", 75, {{0, "'WEBVTT'"}, {3, "'WEBVTT'"}}},
    {"ass", "Advanced SubStation Alpha subtitles", Subtitle, "ass ssa", "text/x-ssa", 55},

    // ---- 3D models ------------------------------------------------------------------
    {"obj", "Wavefront OBJ model", Model3D, "obj", "model/obj", 75},
    {"stl", "STL model", Model3D, "stl", "model/stl", 75},
    {"ply", "Polygon File Format model", Model3D, "ply", "model/ply", 45, {{0, "'ply' 0A"}, {0, "'ply' 0D 0A"}}},
    {"gltf", "glTF model (JSON)", Model3D, "gltf", "model/gltf+json", 65},
    {"glb", "glTF model (binary)", Model3D, "glb", "model/gltf-binary", 65, {{0, "'glTF'"}}},
    {"fbx", "Autodesk FBX model", Model3D, "fbx", "application/octet-stream", 45, {{0, "'Kaydara FBX Binary'"}}},

    // ---- geographic -------------------------------------------------------------------
    {"geojson", "GeoJSON", Geo, "geojson", "application/geo+json", 70},
    {"gpx", "GPS Exchange Format", Geo, "gpx", "application/gpx+xml", 70, {}, sniff_gpx},
    {"kml", "Keyhole Markup Language", Geo, "kml", "application/vnd.google-earth.kml+xml", 65, {}, sniff_kml},

    // ---- calendars and contacts ----------------------------------------------------------
    {"ics", "iCalendar", Pim, "ics ical", "text/calendar", 80, {{0, "'BEGIN:VCALENDAR'"}}},
    {"vcf", "vCard", Pim, "vcf vcard", "text/vcard", 80, {{0, "'BEGIN:VCARD'"}}},

    // ---- source code ------------------------------------------------------------------------
    {"c", "C source", Code, "c h", "text/x-c", 50},
    {"cpp", "C++ source", Code, "cpp cc cxx hpp hh hxx", "text/x-c++", 50},
    {"py", "Python source", Code, "py", "text/x-python", 60},
    {"js", "JavaScript source", Code, "js mjs cjs", "text/javascript", 60},
    {"ts", "TypeScript source", Code, "ts tsx", "text/x-typescript", 50},
    {"java", "Java source", Code, "java", "text/x-java", 45},
    {"rs", "Rust source", Code, "rs", "text/x-rust", 40},
    {"go", "Go source", Code, "go", "text/x-go", 40},
    {"swift", "Swift source", Code, "swift", "text/x-swift", 40},
    {"cs", "C# source", Code, "cs", "text/x-csharp", 40},
    {"kt", "Kotlin source", Code, "kt kts", "text/x-kotlin", 40},
    {"rb", "Ruby source", Code, "rb", "text/x-ruby", 35},
    {"php", "PHP source", Code, "php", "application/x-php", 40},
    {"css", "Cascading Style Sheet", Code, "css", "text/css", 55},
    {"sh", "Shell script", Code, "sh bash zsh", "application/x-sh", 45, {{0, "'#!/bin/sh'"}, {0, "'#!/bin/bash'"}}},
    {"sql", "SQL script", Code, "sql", "application/sql", 45},

    // ---- executables and other binaries ---------------------------------------------------------
    {"exe", "Windows executable", Other, "exe dll sys", "application/vnd.microsoft.portable-executable", 60,
     {{0, "'MZ'"}}},
    {"elf", "ELF executable", Other, "elf so", "application/x-elf", 40, {{0, "7F 'ELF'"}}},
    {"macho", "Mach-O executable", Other, "dylib", "application/x-mach-binary", 35,
     {{0, "CF FA ED FE"}, {0, "CE FA ED FE"}, {0, "FE ED FA CF"}, {0, "FE ED FA CE"}}, sniff_macho_fat},
    {"class", "Java class file", Other, "class", "application/java-vm", 30, {}, sniff_class},
    {"wasm", "WebAssembly module", Other, "wasm", "application/wasm", 40, {{0, "00 'asm' 01 00 00 00"}}},
    {"torrent", "BitTorrent metainfo", Other, "torrent", "application/x-bittorrent", 30, {{0, "'d8:announce'"}}},
};

}  // namespace

void register_known_formats(Registry& registry) {
  for (const KnownFormat& known : known_formats) {
    FormatInfo info;
    info.id = std::string(known.id);
    info.name = std::string(known.name);
    info.category = known.category;
    for (const std::string_view extension : util::split(known.extensions, ' ', false)) {
      info.extensions.emplace_back(extension);
    }
    info.mime = std::string(known.mime);
    info.popularity = known.popularity;
    for (const KnownSignature& signature : known.signatures) {
      if (!signature.pattern.text.empty()) info.signatures.push_back(Signature::at(signature.offset, signature.pattern));
    }
    if (known.sniff != nullptr) info.sniff = known.sniff;
    registry.add_known_format(std::move(info));
  }
}

}  // namespace cvt
