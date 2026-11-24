// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Binary container classifiers. Every structure read here is described in a
// public specification: PKWARE APPNOTE (ZIP), MS-CFB (OLE2 compound files),
// ISO/IEC 14496-12 and the MP4 registration authority's brand list (ISO base
// media), RFC 8794 and the Matroska specification (EBML), RFC 3533 and the
// codec mappings of Xiph.org (Ogg), ISO/IEC 11172-3 / 13818-1/-3/-7 and
// ATSC A/52 (MPEG audio, transport and program streams, ADTS, AC-3), ITU-T
// H.264/H.265 Annex B, TIFF 6.0 / BigTIFF / DNG / GeoTIFF, POSIX ustar, the
// PE/COFF specification, the System V ABI (ELF), Apple's mach-o/loader.h,
// the JVM specification (class files), ECMA-119 / ECMA-167 (ISO 9660, UDF),
// the FAT/NTFS/ext/HFS+/APFS on-disk references, the UEFI specification
// (GPT), the SQLite file format, RFC 1952 (gzip) and SAM/BAM's BGZF, the ASF
// specification and ITU-T X.690 with RFC 5280/2986/5208/7292/5652 (DER).
#include "engine/sniffers.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/known_formats_sniff_util.hpp"
#include "util/deflate.hpp"

namespace cvt::sniff {

using namespace std::string_view_literals;
using namespace detail;

namespace {

constexpr std::size_t npos = std::string_view::npos;

// ============================================================================ ZIP

struct ZipEntry {
  std::string_view name;
  std::uint64_t local = 0;  // offset of the local header
  std::uint64_t compressed = 0;
  std::uint16_t method = 0;
};

constexpr std::size_t max_zip_entries = 20000;

// Visit the entries of a ZIP file: from the central directory when the end
// record is present, otherwise from the local headers at the front (a
// truncated file). `visit` returns false to stop. Returns false when the
// data is not a ZIP file at all.
template <class F>
bool zip_walk(const View& v, F&& visit) {
  if (!v.eq(0, "PK\x03\x04"sv) && !v.eq(0, "PK\x05\x06"sv) && !v.eq(0, "PK\x07\x08PK\x03\x04"sv)) return false;
  // End of central directory: the last 22 + 65535 bytes.
  std::size_t eocd = npos;
  if (v.size() >= 22) {
    const std::size_t low = v.size() > 22 + 65535 ? v.size() - 22 - 65535 : 0;
    for (std::size_t p = v.size() - 22 + 1; p-- > low;) {
      if (v.u8(p) == 'P' && v.eq(p, "PK\x05\x06"sv)) {
        eocd = p;
        break;
      }
    }
  }
  if (eocd != npos) {
    std::uint64_t count = v.u16le(eocd + 10);
    std::uint64_t cd_size = v.u32le(eocd + 12);
    std::uint64_t cd_offset = v.u32le(eocd + 16);
    if ((count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) && eocd >= 20 &&
        v.eq(eocd - 20, "PK\x06\x07"sv)) {
      const std::uint64_t z = v.u64le(eocd - 20 + 8);
      if (v.has64(z, 56) && v.eq(static_cast<std::size_t>(z), "PK\x06\x06"sv)) {
        count = v.u64le(static_cast<std::size_t>(z) + 32);
        cd_size = v.u64le(static_cast<std::size_t>(z) + 40);
        cd_offset = v.u64le(static_cast<std::size_t>(z) + 48);
      }
    }
    if (cd_offset <= eocd && cd_size <= eocd - cd_offset) {
      auto p = static_cast<std::size_t>(cd_offset);
      std::uint64_t seen = 0;
      while (seen < count && seen < max_zip_entries && v.has(p, 46) && v.eq(p, "PK\x01\x02"sv)) {
        const std::size_t n = v.u16le(p + 28);
        const std::size_t m = v.u16le(p + 30);
        const std::size_t k = v.u16le(p + 32);
        if (!v.has(p + 46, n)) break;
        const ZipEntry entry{v.str(p + 46, n), v.u32le(p + 42), v.u32le(p + 20), v.u16le(p + 10)};
        ++seen;
        if (!visit(entry)) return true;
        p += 46 + n + m + k;
      }
      if (seen > 0 || count == 0) return true;
    }
  }
  // No usable central directory: walk the local headers.
  std::size_t p = v.eq(0, "PK\x07\x08"sv) ? 4 : 0;
  for (std::size_t guard = 0; guard < max_zip_entries && v.has(p, 30) && v.eq(p, "PK\x03\x04"sv); ++guard) {
    const std::size_t n = v.u16le(p + 26);
    const std::size_t m = v.u16le(p + 28);
    const std::uint16_t flags = v.u16le(p + 6);
    const std::uint64_t compressed = v.u32le(p + 18);
    // A name cut off by the end of the sample still counts (it is the last entry).
    const ZipEntry entry{v.str(p + 30, n), p, compressed, v.u16le(p + 8)};
    if (!visit(entry)) return true;
    const std::size_t data = p + 30 + n + m;
    if (data > v.size()) break;
    if ((flags & 8u) != 0 && compressed == 0) {
      // Size unknown until the data descriptor: look for the next header nearby.
      const std::size_t next = v.find("PK\x03\x04"sv, data, data + (std::size_t{1} << 20));
      if (next == npos) break;
      p = next;
    } else {
      if (compressed > v.size() - data) break;
      p = data + static_cast<std::size_t>(compressed);
    }
  }
  return true;
}

// The stored or compressed bytes of an entry (clipped to the data).
ByteView zip_entry_data(const View& v, const ZipEntry& entry) {
  if (!v.has64(entry.local, 30) || !v.eq(static_cast<std::size_t>(entry.local), "PK\x03\x04"sv)) return {};
  const auto local = static_cast<std::size_t>(entry.local);
  const std::size_t start = local + 30 + v.u16le(local + 26) + v.u16le(local + 28);
  if (start > v.size()) return {};
  const std::size_t available = v.size() - start;
  const std::size_t count = entry.compressed < available ? static_cast<std::size_t>(entry.compressed) : available;
  return v.d.subspan(start, count);
}

// The first `limit` bytes of an entry's content (stored or deflated).
Bytes zip_entry_head(const View& v, const ZipEntry& entry, std::size_t limit) {
  const ByteView raw = zip_entry_data(v, entry);
  if (entry.method == 0) return Bytes(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(std::min(raw.size(), limit)));
  Bytes out;
  if (entry.method == 8) {
    util::Inflater inflater(util::Inflater::Format::Raw);
    (void)inflater.feed(raw, out, limit);  // on error `out` keeps what was decoded
  }
  return out;
}

bool is_image_name(std::string_view name) {
  for (const std::string_view ext : {".jpg"sv, ".jpeg"sv, ".png"sv, ".gif"sv, ".webp"sv, ".bmp"sv, ".avif"sv, ".jxl"sv}) {
    if (iends(name, ext)) return true;
  }
  return false;
}

struct MimeMapping {
  std::string_view mime;
  std::string_view id;
};
constexpr MimeMapping zip_mimetypes[] = {
    {"application/vnd.oasis.opendocument.text", "odt"},
    {"application/vnd.oasis.opendocument.text-template", "ott"},
    {"application/vnd.oasis.opendocument.text-master", "odm"},
    {"application/vnd.oasis.opendocument.text-web", "oth"},
    {"application/vnd.oasis.opendocument.spreadsheet", "ods"},
    {"application/vnd.oasis.opendocument.spreadsheet-template", "ots"},
    {"application/vnd.oasis.opendocument.presentation", "odp"},
    {"application/vnd.oasis.opendocument.presentation-template", "otp"},
    {"application/vnd.oasis.opendocument.graphics", "odg"},
    {"application/vnd.oasis.opendocument.graphics-template", "otg"},
    {"application/vnd.oasis.opendocument.chart", "odc"},
    {"application/vnd.oasis.opendocument.formula", "odf"},
    {"application/vnd.oasis.opendocument.image", "odi"},
    {"application/vnd.oasis.opendocument.base", "odb"},
    {"application/vnd.sun.xml.writer", "sxw"},
    {"application/vnd.sun.xml.writer.template", "stw"},
    {"application/vnd.sun.xml.writer.global", "sxg"},
    {"application/vnd.sun.xml.calc", "sxc"},
    {"application/vnd.sun.xml.calc.template", "stc"},
    {"application/vnd.sun.xml.impress", "sxi"},
    {"application/vnd.sun.xml.impress.template", "sti"},
    {"application/vnd.sun.xml.draw", "sxd"},
    {"application/vnd.sun.xml.draw.template", "std"},
    {"application/vnd.sun.xml.math", "sxm"},
    {"application/epub+zip", "epub"},
    {"image/openraster", "ora"},
    {"application/x-krita", "kra"},
    {"application/vnd.adobe.indesign-idml-package", "idml"},
    {"application/vnd.adobe.air-application-installer-package+zip", "air"},
    {"application/vnd.recordare.musicxml", "mxl"},
    {"application/hwp+zip", "hwpx"},
    {"application/vnd.etsi.asic-e+zip", "asice"},
    {"application/vnd.etsi.asic-s+zip", "asics"},
};

struct ZipFacts {
  std::string_view mimetype;
  bool content_types = false;
  bool word_main = false, xl_main = false, xl_binary = false, ppt_main = false, visio_main = false;
  bool word_dir = false, xl_dir = false, ppt_dir = false, visio_dir = false;
  bool vba = false;
  bool manifest_mf = false, class_file = false, android_manifest = false, classes_dex = false, resources_arsc = false;
  bool bundle_config = false, classes_jar = false, toc_pb = false, web_inf = false, application_xml = false;
  bool payload_app = false, install_rdf = false, mozilla_rsa = false, manifest_json = false, apk_member = false;
  bool kml = false, model_3d = false, usd_first = false, fdseq = false, nuspec = false;
  bool appx_manifest = false, appx_bundle = false, vsix_manifest = false, dist_info_wheel = false, egg_info = false;
  bool iwa_document = false, iwa_slide = false, iwa_calc = false, iwa_tables = false, keynote09 = false;
  bool sketch_document = false, sketch_meta = false;
  bool data_pkl = false, keras_config = false, keras_weights = false;
  bool mscx = false, gpif = false, procreate = false, pass_json = false, geogebra = false, h5p = false, dom_document = false;
  bool anki = false;
  bool container_xml = false, opf = false, description_xml = false, manifest_xml = false, lottie_animations = false;
  std::size_t entries = 0, npy = 0, images = 0;
  ZipEntry content_types_entry;
};

void zip_fact(ZipFacts& f, const ZipEntry& e, bool first) {
  const std::string_view n = e.name;
  ++f.entries;
  if (first && n == "mimetype") return;  // handled by the caller
  if (n == "[Content_Types].xml") {
    f.content_types = true;
    f.content_types_entry = e;
  }
  if (n.starts_with("word/")) f.word_dir = true;
  if (n.starts_with("xl/")) f.xl_dir = true;
  if (n.starts_with("ppt/")) f.ppt_dir = true;
  if (n.starts_with("visio/")) f.visio_dir = true;
  if (n.starts_with("word/document")) f.word_main = true;
  if (n.starts_with("xl/workbook")) f.xl_main = true;
  if (n == "xl/workbook.bin") f.xl_binary = true;
  if (n.starts_with("ppt/presentation")) f.ppt_main = true;
  if (n.starts_with("visio/document")) f.visio_main = true;
  if (n.ends_with("vbaProject.bin")) f.vba = true;
  if (n == "META-INF/MANIFEST.MF") f.manifest_mf = true;
  if (n.ends_with(".class")) f.class_file = true;
  if (n == "AndroidManifest.xml") f.android_manifest = true;
  if (n == "classes.dex") f.classes_dex = true;
  if (n == "resources.arsc") f.resources_arsc = true;
  if (n == "BundleConfig.pb") f.bundle_config = true;
  if (n == "classes.jar") f.classes_jar = true;
  if (n == "toc.pb") f.toc_pb = true;
  if (n.starts_with("WEB-INF/")) f.web_inf = true;
  if (n == "META-INF/application.xml") f.application_xml = true;
  if (n.starts_with("Payload/") && n.find(".app/") != npos) f.payload_app = true;
  if (n == "install.rdf") f.install_rdf = true;
  if (n == "META-INF/mozilla.rsa") f.mozilla_rsa = true;
  if (n == "manifest.json") f.manifest_json = true;
  if (n == "collection.anki2" || n == "collection.anki21") f.anki = true;
  if (n.find('/') == npos && iends(n, ".apk")) f.apk_member = true;
  if (n.find('/') == npos && iends(n, ".kml")) f.kml = true;
  if (iequal(n, "3D/3dmodel.model")) f.model_3d = true;
  if (first && (n.ends_with(".usdc") || n.ends_with(".usda") || n.ends_with(".usd"))) f.usd_first = true;
  if (n == "FixedDocumentSequence.fdseq" || n == "FixedDocSeq.fdseq") f.fdseq = true;
  if (n.find('/') == npos && n.ends_with(".nuspec")) f.nuspec = true;
  if (n == "AppxManifest.xml") f.appx_manifest = true;
  if (n == "AppxMetadata/AppxBundleManifest.xml") f.appx_bundle = true;
  if (n == "extension.vsixmanifest") f.vsix_manifest = true;
  if (n.ends_with(".dist-info/WHEEL")) f.dist_info_wheel = true;
  if (n.starts_with("EGG-INFO/")) f.egg_info = true;
  if (n == "Index/Document.iwa") f.iwa_document = true;
  if (n.starts_with("Index/Slide") || n.starts_with("Index/MasterSlide")) f.iwa_slide = true;
  if (n == "Index/CalculationEngine.iwa") f.iwa_calc = true;
  if (n.starts_with("Index/Tables/")) f.iwa_tables = true;
  if (n == "index.apxl" || n == "index.apxl.gz") f.keynote09 = true;
  if (n == "document.json") f.sketch_document = true;
  if (n == "meta.json") f.sketch_meta = true;
  if (n.ends_with("/data.pkl") || n == "data.pkl") f.data_pkl = true;
  if (n == "config.json") f.keras_config = true;
  if (n == "model.weights.h5") f.keras_weights = true;
  if (n.find('/') == npos && n.ends_with(".mscx")) f.mscx = true;
  if (n == "Content/score.gpif") f.gpif = true;
  if (n == "Document.archive") f.procreate = true;
  if (n == "pass.json") f.pass_json = true;
  if (n == "geogebra.xml") f.geogebra = true;
  if (n == "h5p.json") f.h5p = true;
  if (n == "DOMDocument.xml") f.dom_document = true;
  if (n == "META-INF/container.xml") f.container_xml = true;
  if (n.ends_with(".opf")) f.opf = true;
  if (n == "description.xml") f.description_xml = true;
  if (n == "META-INF/manifest.xml") f.manifest_xml = true;
  if (n.starts_with("animations/")) f.lottie_animations = true;
  if (n.ends_with(".npy")) ++f.npy;
  if (is_image_name(n)) ++f.images;
}

// The main content type of an OOXML package decides between document,
// template, macro-enabled and add-in variants.
struct OoxmlVariant {
  std::string_view content_type;
  std::string_view id;
};
constexpr OoxmlVariant ooxml_variants[] = {
    {"ms-word.document.macroEnabled.main+xml", "docm"},
    {"ms-word.template.macroEnabledTemplate.main+xml", "dotm"},
    {"wordprocessingml.template.main+xml", "dotx"},
    {"wordprocessingml.document.main+xml", "docx"},
    {"ms-excel.sheet.binary.macroEnabled.main", "xlsb"},
    {"ms-excel.sheet.macroEnabled.main+xml", "xlsm"},
    {"ms-excel.template.macroEnabled.main+xml", "xltm"},
    {"ms-excel.addin.macroEnabled.main+xml", "xlam"},
    {"spreadsheetml.template.main+xml", "xltx"},
    {"spreadsheetml.sheet.main+xml", "xlsx"},
    {"ms-powerpoint.presentation.macroEnabled.main+xml", "pptm"},
    {"ms-powerpoint.template.macroEnabled.main+xml", "potm"},
    {"ms-powerpoint.slideshow.macroEnabled.main+xml", "ppsm"},
    {"ms-powerpoint.addin.macroEnabled.main+xml", "ppam"},
    {"presentationml.template.main+xml", "potx"},
    {"presentationml.slideshow.main+xml", "ppsx"},
    {"presentationml.presentation.main+xml", "pptx"},
    {"ms-visio.drawing.macroEnabled.main+xml", "vsdm"},
    {"ms-visio.stencil.macroEnabled.main+xml", "vssm"},
    {"ms-visio.template.macroEnabled.main+xml", "vstm"},
    {"ms-visio.stencil.main+xml", "vssx"},
    {"ms-visio.template.main+xml", "vstx"},
    {"ms-visio.drawing.main+xml", "vsdx"},
};

bool in(std::string_view id, std::initializer_list<std::string_view> ids) {
  return std::ranges::find(ids, id) != ids.end();
}

Verdict classify_zip(const View& v, std::string_view filename, std::string_view wanted) {
  ZipFacts f;
  bool first = true;
  const bool is_zip = zip_walk(v, [&](const ZipEntry& e) {
    if (first && e.name == "mimetype" && e.method == 0) {
      f.mimetype = trim(as_chars(zip_entry_data(v, e)).substr(0, 128));
    }
    zip_fact(f, e, first);
    first = false;
    return true;
  });
  if (!is_zip) return nothing();

  if (!f.mimetype.empty()) {
    for (const MimeMapping& m : zip_mimetypes) {
      if (f.mimetype == m.mime) {
        if (m.id == "epub") return found(refine(filename, "epub", {"ibooks"}), 97);
        return found(m.id, 97);
      }
    }
  }
  if (f.bundle_config) return found("aab", 95);
  if (f.toc_pb && !f.android_manifest) return found("apks", 90);
  if (f.android_manifest) {
    if (f.classes_jar && !f.classes_dex) return found("aar", 92);
    return found("apk", 95);
  }
  if (f.payload_app) return found("ipa", 95);
  if (f.apk_member && f.manifest_json) return found("xapk", 92);
  if (f.fdseq) return found(has_ext(filename, "oxps") ? "oxps" : "xps", 92);
  if (f.nuspec) return found("nupkg", 95);
  if (f.vsix_manifest) return found("vsix", 95);
  if (f.appx_bundle) return found("msixbundle", 95);
  if (f.appx_manifest) return found("msix", 95);
  if (f.model_3d) return found("3mf", 95);

  // Office Open XML.
  const int groups = int{f.word_dir} + int{f.xl_dir} + int{f.ppt_dir} + int{f.visio_dir};
  if (groups == 1 && (f.content_types || f.word_main || f.xl_main || f.xl_binary || f.ppt_main || f.visio_main)) {
    std::string_view group_default = f.word_dir ? "docx" : f.xl_dir ? (f.xl_binary ? "xlsb" : "xlsx") : f.ppt_dir ? "pptx" : "vsdx";
    const bool relevant = f.word_dir   ? in(wanted, {"docx", "docm", "dotx", "dotm"})
                          : f.xl_dir   ? in(wanted, {"xlsx", "xlsm", "xltx", "xltm", "xlam", "xlsb"})
                          : f.ppt_dir  ? in(wanted, {"pptx", "pptm", "potx", "potm", "ppsx", "ppsm", "ppam"})
                                       : in(wanted, {"vsdx", "vsdm", "vssx", "vssm", "vstx", "vstm"});
    if (!relevant) return found(group_default, 95);  // the variant cannot be `wanted`: skip the inflation
    if (f.content_types) {
      const Bytes types = zip_entry_head(v, f.content_types_entry, 256 * 1024);
      const std::string_view text = as_chars(types);
      for (const OoxmlVariant& variant : ooxml_variants) {
        if (text.find(variant.content_type) == npos) continue;
        // Some writers (LibreOffice among them) label templates and shows
        // with the plain document type: then the name says which it is.
        if (variant.id == "docx") return found(refine(filename, "docx", {"dotx"}), 96);
        if (variant.id == "xlsx") return found(refine(filename, "xlsx", {"xltx"}), 96);
        if (variant.id == "pptx") return found(refine(filename, "pptx", {"potx", "ppsx"}), 96);
        if (variant.id == "vsdx") return found(refine(filename, "vsdx", {"vssx", "vstx"}), 96);
        return found(variant.id, 96);
      }
    }
    // No readable [Content_Types].xml (truncated or unusual): parts and name decide.
    if (f.vba) {
      group_default = f.word_dir ? refine(filename, "docm", {"dotm"})
                      : f.xl_dir ? refine(filename, "xlsm", {"xltm", "xlam", "xlsb"})
                      : f.ppt_dir ? refine(filename, "pptm", {"potm", "ppsm", "ppam"})
                                  : refine(filename, "vsdm", {"vssm", "vstm"});
      return found(group_default, 90);
    }
    group_default = f.word_dir ? refine(filename, group_default, {"dotx"})
                    : f.xl_dir ? refine(filename, group_default, {"xltx"})
                    : f.ppt_dir ? refine(filename, group_default, {"potx", "ppsx"})
                                : refine(filename, group_default, {"vssx", "vstx"});
    return found(group_default, 92);
  }

  if (f.container_xml && f.opf) return found("epub", 90);
  if (f.kml) return found("kmz", 95);
  if (f.usd_first) return found("usdz", 95);
  if (f.dist_info_wheel) return found("whl", 95);
  if (f.egg_info) return found("egg", 92);
  if (f.sketch_document && f.sketch_meta) return found("sketch", 95);
  if (f.iwa_document) {
    if (f.iwa_slide) return found("key", 92);
    if (f.iwa_calc || f.iwa_tables) return found("numbers", 92);
    return found(refine(filename, "pages", {"key", "numbers"}), 88);
  }
  if (f.keynote09) return found("key", 90);
  if (f.npy > 0 && f.npy == f.entries) return found("npz", 95);
  if (f.data_pkl) return found("pt", 92);
  if (f.keras_config && f.keras_weights) return found("keras", 95);
  if (f.mscx) return found("mscz", 95);
  if (f.gpif) return found("gp", 95);
  if (f.procreate) return found("procreate", 92);
  if (f.pass_json && f.manifest_json) return found("pkpass", 95);
  if (f.geogebra) return found("ggb", 95);
  if (f.h5p) return found("h5p", 95);
  if (f.anki) return found("apkg", 95);
  if (f.dom_document) return found("fla", 92);
  if (f.manifest_json && f.lottie_animations) return found("dotlottie", 92);
  if (f.install_rdf || f.mozilla_rsa) return found("xpi", 95);
  if (f.web_inf) return found("war", 92);
  if (f.application_xml) return found("ear", 92);
  if (f.description_xml && f.manifest_xml) return found("oxt", 90);
  if (f.manifest_mf || f.class_file) return found("jar", 88);
  return container_only();
}

// ============================================================================ OLE2 compound files

struct OleFacts {
  bool word = false, workbook = false, powerpoint = false, msg = false, visio = false, quill = false;
  bool hwp = false, catalog = false, max_scene = false, max_dll = false, revit = false, project = false;
  bool encrypted_package = false;
  std::array<std::uint8_t, 16> clsid{};
};

constexpr std::array<std::uint8_t, 16> clsid(std::uint32_t data1) {
  return {static_cast<std::uint8_t>(data1), static_cast<std::uint8_t>(data1 >> 8), static_cast<std::uint8_t>(data1 >> 16),
          static_cast<std::uint8_t>(data1 >> 24), 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
}

void ole_fact(OleFacts& f, std::string_view name) {
  if (iequal(name, "WordDocument")) f.word = true;
  if (iequal(name, "Workbook") || iequal(name, "Book")) f.workbook = true;
  if (name == "PowerPoint Document") f.powerpoint = true;
  if (name.starts_with("__substg1.0_") || name == "__properties_version1.0" || name == "__nameid_version1.0") f.msg = true;
  if (name == "VisioDocument") f.visio = true;
  if (name == "Quill") f.quill = true;
  if (name.ends_with("HwpSummaryInformation")) f.hwp = true;
  if (name == "Catalog") f.catalog = true;
  if (name == "Scene") f.max_scene = true;
  if (name == "ClassDirectory3" || name == "DllDirectory") f.max_dll = true;
  if (name == "BasicFileInfo") f.revit = true;
  if (name.size() == 6 && name.starts_with("   11")) f.project = true;
  if (name == "EncryptedPackage") f.encrypted_package = true;
}

Verdict classify_ole(const View& v, std::string_view filename) {
  if (!v.eq(0, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1"sv)) return nothing();
  const unsigned shift = v.u16le(30);
  if (shift != 9 && shift != 12) return container_only();
  const std::size_t sector = std::size_t{1} << shift;
  const std::size_t per_fat = sector / 4;
  const auto sector_offset = [&](std::uint32_t s) { return (static_cast<std::uint64_t>(s) + 1) << shift; };
  const std::uint32_t fat_sectors = v.u32le(44);
  // The FAT sector holding the entry of sector `s` (first 109 FAT sectors only:
  // enough for 109 * sector/4 * sector bytes, 6.8 MB with 512-byte sectors).
  const auto next_sector = [&](std::uint32_t s) -> std::uint32_t {
    const std::uint64_t index = s / per_fat;
    if (index >= 109 || index >= fat_sectors) return 0xFFFFFFFEu;
    const std::uint32_t fat = v.u32le(76 + static_cast<std::size_t>(index) * 4);
    const std::uint64_t at = sector_offset(fat) + (s % per_fat) * 4;
    if (!v.has64(at, 4)) return 0xFFFFFFFEu;
    return v.u32le(static_cast<std::size_t>(at));
  };
  OleFacts f;
  std::uint32_t dir = v.u32le(48);
  std::size_t entries = 0;
  bool root = true;
  for (std::size_t chain = 0; chain < 4096 && dir < 0xFFFFFFFAu; ++chain) {
    const std::uint64_t at = sector_offset(dir);
    for (std::size_t i = 0; i < sector / 128 && entries < 8192; ++i, ++entries) {
      const std::uint64_t e = at + i * 128;
      if (!v.has64(e, 128)) break;
      const auto entry = static_cast<std::size_t>(e);
      const std::size_t name_bytes = std::min<std::size_t>(v.u16le(entry + 64), 64);
      std::array<char, 32> name{};
      std::size_t length = 0;
      for (std::size_t k = 0; k + 1 < name_bytes && length < name.size(); k += 2) {
        const std::uint16_t c = v.u16le(entry + k);
        if (c == 0) break;
        name[length++] = c < 0x80 ? static_cast<char>(c) : '\x7F';
      }
      if (root) {
        for (std::size_t k = 0; k < 16; ++k) f.clsid[k] = v.u8(entry + 80 + k);
        root = false;
      }
      ole_fact(f, std::string_view(name.data(), length));
    }
    if (!v.has64(at, sector)) break;
    dir = next_sector(dir);
  }
  if (root) return container_only();  // directory unreadable (truncated)
  if (f.clsid == clsid(0x000C1084)) return found("msi", 97);
  if (f.clsid == clsid(0x000C1086)) return found("msp", 97);
  if (f.clsid == clsid(0x000C1082)) return found("mst", 97);
  if (f.encrypted_package) {
    const std::string_view id = refine(filename, {}, {"docx", "xlsx", "pptx", "docm", "xlsm", "pptm", "xlsb"});
    if (!id.empty()) return found(id, 90);
  }
  if (f.word) return found(refine(filename, "doc", {"dot"}), 95);
  if (f.workbook) return found(refine(filename, "xls", {"xla"}), 95);
  if (f.powerpoint) return found("ppt", 95);
  if (f.msg) return found(refine(filename, "msg", {"oft"}), 95);
  if (f.visio) return found(refine(filename, "vsd", {"vss", "vst"}), 95);
  if (f.quill) return found("pub", 92);
  if (f.hwp) return found("hwp", 95);
  if (f.max_scene && f.max_dll) return found("max", 92);
  if (f.revit) return found(refine(filename, "rvt", {"rfa", "rte", "rft"}), 92);
  if (f.project) return found("mpp", 88);
  if (f.catalog) return found("thumbsdb", 85);
  return base_reading("cfb", 70);
}

// ============================================================================ ISO base media

struct BrandMapping {
  std::string_view brand;
  std::string_view id;
};
constexpr BrandMapping major_brands[] = {
    {"qt  ", "mov"},  {"M4A ", "m4a"},  {"M4B ", "m4a"},  {"M4P ", "m4a"},  {"M4V ", "m4v"},  {"M4VH", "m4v"},
    {"M4VP", "m4v"},  {"heic", "heic"}, {"heix", "heic"}, {"heim", "heic"}, {"heis", "heic"}, {"hevc", "heic"},
    {"hevx", "heic"}, {"hevm", "heic"}, {"hevs", "heic"}, {"avif", "avif"}, {"avis", "avif"}, {"crx ", "cr3"},
    {"jp2 ", "jp2"},  {"jpx ", "jpx"},  {"jpm ", "jpm"},  {"mjp2", "mj2"},  {"mj2s", "mj2"},  {"F4V ", "f4v"},
    {"F4P ", "f4v"},  {"F4A ", "f4a"},  {"F4B ", "f4a"},  {"f4v ", "f4v"},  {"f4p ", "f4v"},  {"f4a ", "f4a"},  {"f4b ", "f4a"},  {"isml", "ismv"}, {"piff", "ismv"},
};
// Image brands may also appear as compatible brands after a structural major brand ("mif1").
constexpr BrandMapping compatible_brands[] = {
    {"avif", "avif"}, {"avis", "avif"}, {"heic", "heic"}, {"heix", "heic"}, {"heim", "heic"}, {"heis", "heic"},
    {"hevc", "heic"}, {"hevx", "heic"}, {"crx ", "cr3"},
};

std::string_view brand_id(std::string_view brand, const auto& table) {
  for (const BrandMapping& m : table) {
    if (m.brand == brand) return m.id;
  }
  if (brand.starts_with("3g2")) return "3g2";
  if (brand.starts_with("3gp") || brand.starts_with("3gg") || brand.starts_with("3ge") || brand.starts_with("3gs") ||
      brand.starts_with("3gr") || brand.starts_with("3gh") || brand.starts_with("3gm") || brand.starts_with("3gt")) {
    return "3gp";
  }
  return {};
}

bool box_type_plausible(const View& v, std::size_t at) {
  for (std::size_t i = 0; i < 4; ++i) {
    const std::uint8_t c = v.u8(at + i);
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

// Size of the box at `at` (header included), 0 when invalid. `end` limits it.
std::uint64_t box_size(const View& v, std::size_t at, std::uint64_t end, std::size_t& header) {
  header = 8;
  std::uint64_t size = v.u32be(at);
  if (size == 1) {
    size = v.u64be(at + 8);
    header = 16;
  } else if (size == 0) {
    size = end - at;
  }
  if (size < header || size > end - at) return 0;
  return size;
}

// Walk moov/trak/mdia/hdlr and report whether the file has video and sound tracks.
void bmff_tracks(const View& v, bool& video, bool& sound) {
  const std::uint64_t end = v.size();
  std::size_t header = 8;
  std::size_t at = 0;
  for (int top = 0; top < 64 && v.has(at, 8); ++top) {
    const std::uint64_t size = box_size(v, at, end, header);
    if (size == 0) return;
    if (v.eq(at + 4, "moov")) {
      const std::uint64_t moov_end = at + size;
      std::size_t t = at + header;
      for (int n = 0; n < 256 && t + 8 <= moov_end && v.has(t, 8); ++n) {
        std::size_t th = 8;
        const std::uint64_t tsize = box_size(v, t, moov_end, th);
        if (tsize == 0) return;
        if (v.eq(t + 4, "trak")) {
          // The handler type sits at a fixed place inside trak/mdia/hdlr; a
          // bounded scan of this trak for the hdlr box is simpler and linear.
          const std::size_t hdlr = v.find("hdlr", t + th, static_cast<std::size_t>(t + tsize));
          if (hdlr != npos && v.has(hdlr + 12, 4)) {
            if (v.eq(hdlr + 12, "vide")) video = true;
            if (v.eq(hdlr + 12, "soun")) sound = true;
          }
        }
        t += static_cast<std::size_t>(tsize);
      }
      return;
    }
    at += static_cast<std::size_t>(size);
  }
}

Verdict classify_bmff(const View& v, std::string_view filename) {
  std::size_t ftyp = npos;
  if (v.eq(4, "ftyp")) {
    ftyp = 0;
  } else if (v.eq(0, "\x00\x00\x00\x0CjP  \x0D\x0A\x87\x0A"sv) && v.eq(16, "ftyp")) {
    ftyp = 12;
  }
  if (ftyp == npos) {
    if (v.eq(4, "styp")) return found("m4s", 92);
    // Old QuickTime files start with a movie, data or padding atom instead of ftyp.
    for (const std::string_view atom : {"moov"sv, "mdat"sv, "wide"sv, "free"sv, "skip"sv, "pnot"sv}) {
      if (v.eq(4, atom)) {
        const std::uint64_t size = v.u32be(0);
        if (size < 8 && size != 0 && size != 1) return nothing();
        if (size >= 8 && size + 8 <= v.size() && !box_type_plausible(v, static_cast<std::size_t>(size) + 4)) return nothing();
        return found(refine(filename, "mov", {"mp4", "m4v", "m4a"}), atom == "moov" ? 90 : 82);
      }
    }
    return nothing();
  }
  const std::uint64_t box = v.u32be(ftyp);
  if (box < 16 || box > 4096) return nothing();
  const std::string_view major = v.str(ftyp + 8, 4);
  if (major.size() < 4) return container_only();
  std::string_view id = brand_id(major, major_brands);
  if (id.empty() || major == "mif1" || major == "msf1") {
    const std::size_t end = static_cast<std::size_t>(std::min<std::uint64_t>(ftyp + box, v.size()));
    for (std::size_t p = ftyp + 16; p + 4 <= end && id.empty(); p += 4) id = brand_id(v.str(p, 4), compatible_brands);
  }
  if (!id.empty()) {
    return found(id, 95);
  }
  if (major == "mif1" || major == "msf1" || major == "miaf") return found("heic", 85);
  // A structural brand (isom, mp41, mp42, avc1, dash ...): the name may say more.
  const std::string_view named = refine(filename, {}, {"mp4", "m4a", "m4b", "m4v", "m4p", "m4r", "mov", "qt", "3gp", "3g2",
                                                      "f4v", "f4a", "ismv", "isma", "m4s"});
  if (!named.empty()) {
    if (in(named, {"m4a", "m4b", "m4p", "m4r"})) return found("m4a", 90);
    if (named == "qt") return found("mov", 90);
    if (named == "isma") return found("ismv", 90);
    return found(named, 90);
  }
  bool video = false;
  bool sound = false;
  bmff_tracks(v, video, sound);
  if (sound && !video) return base_reading("m4a", 84);
  return base_reading("mp4", 84);
}

// ============================================================================ EBML (Matroska, WebM)

// Variable-length integer (RFC 8794): `marker` keeps the length marker (element ids).
bool ebml_vint(const View& v, std::size_t& at, std::uint64_t& value, bool marker, bool& unknown) {
  const std::uint8_t first = v.u8(at);
  if (first == 0 || !v.has(at, 1)) return false;
  std::size_t length = 1;
  while (length <= 8 && (first & (0x80u >> (length - 1))) == 0) ++length;
  if (length > 8 || !v.has(at, length)) return false;
  std::uint64_t result = marker ? first : (first & (0xFFu >> length));
  bool all_ones = (first & (0xFFu >> length)) == (0xFFu >> length);
  for (std::size_t i = 1; i < length; ++i) {
    const std::uint8_t b = v.u8(at + i);
    all_ones = all_ones && b == 0xFF;
    result = (result << 8) | b;
  }
  unknown = !marker && all_ones;
  value = result;
  at += length;
  return true;
}

Verdict classify_ebml(const View& v, std::string_view filename) {
  if (!v.eq(0, "\x1A\x45\xDF\xA3"sv)) return nothing();
  std::size_t at = 4;
  std::uint64_t size = 0;
  bool unknown = false;
  if (!ebml_vint(v, at, size, false, unknown) || unknown) return container_only();
  const std::size_t header_end = static_cast<std::size_t>(std::min<std::uint64_t>(at + size, v.size()));
  std::string_view doctype;
  for (std::size_t p = at; p < header_end;) {
    std::uint64_t id = 0;
    std::uint64_t length = 0;
    if (!ebml_vint(v, p, id, true, unknown) || !ebml_vint(v, p, length, false, unknown) || unknown) break;
    if (id == 0x4282) doctype = v.str(p, static_cast<std::size_t>(std::min<std::uint64_t>(length, 32)));
    if (length > v.size() - std::min(p, v.size())) break;
    p += static_cast<std::size_t>(length);
  }
  while (!doctype.empty() && doctype.back() == '\0') doctype.remove_suffix(1);
  if (doctype.empty()) return container_only();
  // Track types inside Segment/Tracks/TrackEntry/TrackType.
  bool video = false, audio = false, subtitle = false, tracks_seen = false;
  std::size_t p = at + static_cast<std::size_t>(std::min<std::uint64_t>(size, v.size()));
  std::uint64_t id = 0;
  std::uint64_t length = 0;
  if (ebml_vint(v, p, id, true, unknown) && id == 0x18538067 && ebml_vint(v, p, length, false, unknown)) {
    for (int n = 0; n < 64 && v.has(p, 2); ++n) {
      bool child_unknown = false;
      if (!ebml_vint(v, p, id, true, unknown) || !ebml_vint(v, p, length, false, child_unknown)) break;
      if (id == 0x1F43B675) break;  // first Cluster: the tracks come before it
      if (id == 0x1654AE6B && !child_unknown) {
        tracks_seen = true;
        const std::size_t tracks_end = static_cast<std::size_t>(std::min<std::uint64_t>(p + length, v.size()));
        // Scan TrackEntry children for TrackType (0x83, one-byte payload).
        std::size_t q = p;
        for (int k = 0; k < 4096 && q < tracks_end;) {
          std::uint64_t cid = 0;
          std::uint64_t clen = 0;
          bool cunknown = false;
          if (!ebml_vint(v, q, cid, true, cunknown) || !ebml_vint(v, q, clen, false, cunknown) || cunknown) break;
          ++k;
          if (cid == 0xAE) continue;  // descend into TrackEntry
          if (cid == 0x83 && clen == 1) {
            const std::uint8_t type = v.u8(q);
            video = video || type == 1;
            audio = audio || type == 2;
            subtitle = subtitle || type == 0x11;
          }
          if (clen > tracks_end - std::min(q, tracks_end)) break;
          q += static_cast<std::size_t>(clen);
        }
        break;
      }
      if (child_unknown || length > v.size() - std::min(p, v.size())) break;
      p += static_cast<std::size_t>(length);
    }
  }
  if (doctype == "webm") return found("webm", 95);
  if (doctype == "matroska") {
    if (tracks_seen && !video && audio) return found("mka", 95);
    if (tracks_seen && !video && !audio && subtitle) return found("mks", 95);
    return found(refine(filename, "mkv", {"mk3d"}), 95);
  }
  return container_only();
}

// ============================================================================ Ogg

Verdict classify_ogg(const View& v) {
  if (!v.eq(0, "OggS"sv)) return nothing();
  bool video = false, vorbis = false, opus = false, flac = false, speex = false, other_audio = false, any = false;
  std::size_t at = 0;
  for (int page = 0; page < 16 && v.has(at, 27) && v.eq(at, "OggS"sv); ++page) {
    if ((v.u8(at + 5) & 0x02) == 0) break;  // past the beginning-of-stream pages
    const std::size_t segments = v.u8(at + 26);
    std::size_t body = 0;
    for (std::size_t s = 0; s < segments; ++s) body += v.u8(at + 27 + s);
    const std::size_t packet = at + 27 + segments;
    const std::string_view head = v.str(packet, 8);
    any = true;
    if (head.starts_with("\x01vorbis"sv)) vorbis = true;
    else if (head.starts_with("OpusHead"sv)) opus = true;
    else if (head.starts_with("\x7F" "FLAC"sv)) flac = true;
    else if (head.starts_with("Speex   "sv)) speex = true;
    else if (head.starts_with("\x80theora"sv) || head.starts_with("BBCD\0"sv) || head.starts_with("OVP80"sv) ||
             head.starts_with("\x01video"sv) || head.starts_with("\x80" "daala"sv)) video = true;
    else if (head.starts_with("CELT    "sv) || head.starts_with("PCM     "sv) || head.starts_with("\x01" "audio"sv)) other_audio = true;
    at = packet + body;
  }
  if (!any) return container_only();
  if (video) return found("ogv", 95);
  const int kinds = int{vorbis} + int{opus} + int{flac} + int{speex} + int{other_audio};
  if (kinds == 1 && opus) return found("opus", 95);
  if (kinds == 1 && speex) return found("spx", 95);
  if (kinds == 1 && flac) return found("ogg-flac", 92);
  if (vorbis || kinds > 0) return found("ogg", 92);
  return base_reading("ogg", 80);
}

// ============================================================================ MPEG audio, ADTS, AC-3, TS, PS, elementary video

// Length of the MPEG audio frame whose header is at `at`, 0 when invalid. `layer`: 1..3.
std::size_t mpeg_audio_frame(const View& v, std::size_t at, int& layer) {
  const std::uint8_t b1 = v.u8(at + 1);
  const std::uint8_t b2 = v.u8(at + 2);
  if (!v.has(at, 4) || v.u8(at) != 0xFF || (b1 & 0xE0) != 0xE0) return 0;
  const int version = (b1 >> 3) & 3;  // 0: 2.5, 1: reserved, 2: 2, 3: 1
  const int layer_bits = (b1 >> 1) & 3;
  const int bitrate_index = b2 >> 4;
  const int rate_index = (b2 >> 2) & 3;
  if (version == 1 || layer_bits == 0 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) return 0;
  layer = 4 - layer_bits;
  static constexpr int v1[3][15] = {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
                                    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
                                    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}};
  static constexpr int v2[2][15] = {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
                                    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
  static constexpr int rates[3] = {44100, 48000, 32000};
  const int kbps = version == 3 ? v1[layer - 1][bitrate_index] : v2[layer == 1 ? 0 : 1][bitrate_index];
  int rate = rates[rate_index];
  if (version == 2) rate /= 2;
  if (version == 0) rate /= 4;
  const int padding = (b2 >> 1) & 1;
  if (layer == 1) return static_cast<std::size_t>((12 * kbps * 1000 / rate + padding) * 4);
  const int factor = (layer == 3 && version != 3) ? 72 : 144;
  return static_cast<std::size_t>(factor * kbps * 1000 / rate + padding);
}

std::size_t adts_frame(const View& v, std::size_t at) {
  if (!v.has(at, 7) || v.u8(at) != 0xFF || (v.u8(at + 1) & 0xF6) != 0xF0) return 0;
  if (((v.u8(at + 2) >> 2) & 0x0F) >= 13) return 0;
  const std::size_t length = (static_cast<std::size_t>(v.u8(at + 3) & 3) << 11) | (static_cast<std::size_t>(v.u8(at + 4)) << 3) |
                             (v.u8(at + 5) >> 5);
  return length >= 7 ? length : 0;
}

// Number of consecutive valid frames starting at `at` (up to `want`); `complete`
// tells whether the chain ended exactly at the end of the data.
template <class Frame>
int frame_chain(const View& v, std::size_t at, int want, Frame&& frame, bool& reached_end) {
  int count = 0;
  reached_end = false;
  while (count < want) {
    if (at == v.size()) {
      reached_end = true;
      break;
    }
    const std::size_t length = frame(at);
    if (length == 0) break;
    ++count;
    if (length > v.size() - at) {
      reached_end = true;  // last frame cut by the end of the sample
      break;
    }
    at += length;
  }
  return count;
}

Verdict classify_mpeg(const View& v, std::string_view filename) {
  // Transport streams: a 0x47 sync byte every 188 bytes (192 with a timecode prefix, 204 with FEC).
  for (const std::size_t stride : {std::size_t{188}, std::size_t{192}, std::size_t{204}}) {
    const std::size_t first = stride == 192 ? 4 : 0;
    if (v.u8(first) != 0x47 || !v.has(first, 1)) continue;
    int syncs = 0;
    std::size_t at = first;
    while (syncs < 6 && at < v.size() && v.u8(at) == 0x47) {
      ++syncs;
      at += stride;
    }
    if (syncs >= 3 || (syncs == 2 && at >= v.size())) {
      if (stride == 192) return found("m2ts", syncs >= 3 ? 93 : 80);
      return found("mpegts", syncs >= 3 ? 93 : 80);
    }
  }
  // Program streams and elementary MPEG video.
  if (v.eq(0, "\x00\x00\x01\xBA"sv)) {
    const std::uint8_t b = v.u8(4);
    if ((b & 0xC0) == 0x40) {
      if (has_ext(filename, "sub")) return found("vobsub", 90);
      return found(refine(filename, "mpg", {"vob"}), 92);
    }
    if ((b & 0xF0) == 0x20) return found("mpg", 92);
    return nothing();
  }
  if (v.eq(0, "\x00\x00\x01\xB3"sv)) {
    // An extension start code (00 00 01 B5) soon after the sequence header means MPEG-2.
    return found(v.find("\x00\x00\x01\xB5"sv, 4, 256) != npos ? "m2v" : "m1v", 90);
  }
  // H.264 / H.265 Annex B byte streams.
  std::size_t nal = npos;
  if (v.eq(0, "\x00\x00\x00\x01"sv)) nal = 4;
  else if (v.eq(0, "\x00\x00\x01"sv)) nal = 3;
  if (nal != npos && v.has(nal, 2)) {
    const std::uint8_t h = v.u8(nal);
    const std::uint8_t h2 = v.u8(nal + 1);
    if ((h & 0x81) == 0 && h2 == 0x01 && ((h >> 1) >= 32 && (h >> 1) <= 35)) return found("hevc", 90);
    if ((h & 0x80) == 0 && ((h & 0x1F) == 7 && (h & 0x60) != 0)) return found("h264", 90);
    if ((h & 0x80) == 0 && (h & 0x1F) == 9) return found("h264", 85);
  }
  // Audio: skip an ID3v2 tag first.
  std::size_t at = 0;
  bool id3 = false;
  if (v.eq(0, "ID3"sv) && v.has(0, 10)) {
    const std::size_t size = (static_cast<std::size_t>(v.u8(6) & 0x7F) << 21) | (static_cast<std::size_t>(v.u8(7) & 0x7F) << 14) |
                             (static_cast<std::size_t>(v.u8(8) & 0x7F) << 7) | (v.u8(9) & 0x7F);
    at = 10 + size + ((v.u8(5) & 0x10) != 0 ? 10 : 0);
    id3 = true;
    if (at >= v.size()) return id3 ? container_only() : nothing();
  }
  if (v.eq(at, "fLaC"sv)) return found("flac", 95);
  if (v.eq(at, "\x0B\x77"sv) && v.has(at, 6)) {
    const int bsid = v.u8(at + 5) >> 3;
    if (bsid <= 8) return found("ac3", 88);
    if (bsid >= 11 && bsid <= 16) return found("eac3", 88);
  }
  bool reached_end = false;
  const int adts = frame_chain(v, at, 4, [&](std::size_t p) { return adts_frame(v, p); }, reached_end);
  if (adts >= 3 || (adts >= 1 && (id3 || reached_end) && adts >= (id3 ? 1 : 2))) return found("aac", 92);
  int layer = 0;
  int first_layer = 0;
  const int frames = frame_chain(v, at, 4, [&](std::size_t p) {
    int l = 0;
    const std::size_t length = mpeg_audio_frame(v, p, l);
    if (first_layer == 0) first_layer = l;
    if (l != first_layer) return std::size_t{0};
    layer = l;
    return length;
  }, reached_end);
  if (frames >= 3 || (frames >= 1 && id3) || (frames == 2 && reached_end)) {
    const std::string_view id = layer == 3 ? "mp3" : layer == 2 ? "mp2" : "mp1";
    return found(id, frames >= 3 ? 93 : 88);
  }
  return id3 ? container_only() : nothing();
}

// ============================================================================ TIFF

Verdict classify_tiff(const View& v, std::string_view filename) {
  bool big = false;
  if (v.eq(0, "MM\x00\x2A"sv)) big = true;
  else if (!v.eq(0, "II\x2A\x00"sv)) {
    if (v.eq(0, "II\x2B\x00\x08\x00\x00\x00"sv) || v.eq(0, "MM\x00\x2B\x00\x08\x00\x00"sv)) return found("bigtiff", 92);
    if (v.eq(0, "IIRO"sv) || v.eq(0, "IIRS"sv) || v.eq(0, "MMOR"sv)) return found("orf", 95);
    if (v.eq(0, "IIU\x00"sv)) return found(refine(filename, "rw2", {"rwl"}), 95);
    return nothing();
  }
  if (v.eq(8, "CR\x02"sv)) return found("cr2", 97);
  const std::uint32_t ifd = v.u32(4, big);
  bool dng = false, geo = false;
  std::string_view make, description;
  if (v.has(ifd, 2)) {
    const std::size_t count = std::min<std::size_t>(v.u16(ifd, big), 512);
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t e = ifd + 2 + i * 12;
      if (!v.has(e, 12)) break;
      const std::uint16_t tag = v.u16(e, big);
      const std::uint16_t type = v.u16(e + 2, big);
      const std::uint32_t n = v.u32(e + 4, big);
      const auto ascii = [&]() -> std::string_view {
        if (type != 2 || n == 0) return {};
        const std::size_t length = std::min<std::size_t>(n, 256);
        return n <= 4 ? v.str(e + 8, length) : v.str(v.u32(e + 8, big), length);
      };
      if (tag == 0xC612) dng = true;
      if (tag == 33550 || tag == 33922 || tag == 34264 || tag == 34735) geo = true;
      if (tag == 0x010F) make = ascii();
      if (tag == 0x010E) description = ascii();
    }
  }
  if (dng) return found(refine(filename, "dng", {"gpr"}), 95);
  if (description.starts_with("<?xml") && description.find("<OME") != npos) return found("ome.tiff", 93);
  if (description.starts_with("Aperio")) return found("svs", 93);
  if (geo) return found("geotiff", 92);
  if (!has_ext(filename, "tif") && !has_ext(filename, "tiff") && !make.empty()) {
    struct MakeMapping {
      std::string_view make;
      std::string_view id;
    };
    static constexpr MakeMapping makes[] = {
        {"NIKON", "nef"},   {"SONY", "arw"},      {"PENTAX", "pef"},  {"RICOH IMAGING", "pef"}, {"SAMSUNG", "srw"},
        {"Hasselblad", "3fr"}, {"EPSON", "erf"},  {"Kodak", "dcr"},   {"EASTMAN KODAK", "dcr"}, {"Mamiya", "mef"},
        {"Leaf", "mos"},    {"Phase One", "iiq"}, {"Canon", "cr2"},
    };
    for (const MakeMapping& m : makes) {
      if (istarts(make, m.make)) {
        if (m.id == "nef") return found(refine(filename, "nef", {"nrw"}), 88);
        if (m.id == "arw") return found(refine(filename, "arw", {"sr2", "srf"}), 88);
        if (m.id == "dcr") return found(refine(filename, "dcr", {"kdc"}), 88);
        if (m.id == "3fr") return found(refine(filename, "3fr", {"fff"}), 88);
        return found(m.id, 88);
      }
    }
  }
  return base_reading("tiff", 80);
}

// ============================================================================ tar

bool tar_header(const View& v, std::size_t at) {
  if (!v.has(at, 512)) return v.eq(at + 257, "ustar"sv);
  if (v.eq(at + 257, "ustar"sv)) return true;
  if (v.u8(at) == 0) return false;
  // Pre-POSIX headers: verify the checksum (octal, bytes 148..155 counted as spaces).
  std::uint32_t sum = 0;
  for (std::size_t i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? 0x20u : v.u8(at + i);
  std::uint32_t stored = 0;
  bool digits = false;
  for (std::size_t i = 148; i < 156; ++i) {
    const char c = static_cast<char>(v.u8(at + i));
    if (c >= '0' && c <= '7') {
      stored = stored * 8 + static_cast<std::uint32_t>(c - '0');
      digits = true;
    } else if (c != ' ' && c != '\0') {
      return false;
    } else if (digits) {
      break;
    }
  }
  return digits && stored == sum;
}

Verdict classify_tar_at(const View& v, std::string_view filename) {
  if (!tar_header(v, 0)) return nothing();
  std::string_view name = v.str(0, 100);
  if (const std::size_t nul = name.find('\0'); nul != npos) name = name.substr(0, nul);
  if (name == "metadata.gz" || name == "data.tar.gz" || name == "checksums.yaml.gz") return found("gem", 95);
  if (name.ends_with(".ovf")) return found("ova", 95);
  (void)filename;
  return base_reading("tar", 84);
}

// ============================================================================ executables

std::string_view macho_filetype(std::uint32_t type) {
  switch (type) {
    case 1: return "o";
    case 2: return "macho";
    case 4: return "core";
    case 6:
    case 7:
    case 9: return "dylib";
    case 8: return "bundle";
    case 0xA: return "dsym";
    default: return "macho";
  }
}

Verdict classify_macho_header(const View& v, std::size_t at) {
  const std::uint32_t magic = v.u32be(at);
  bool big = false;
  if (magic == 0xFEEDFACEu || magic == 0xFEEDFACFu) big = true;
  else if (magic != 0xCEFAEDFEu && magic != 0xCFFAEDFEu) return nothing();
  if (!v.has(at, 16)) return found("macho", 80);
  return found(macho_filetype(v.u32(at + 12, big)), 92);
}

Verdict classify_exec(const View& v, std::string_view filename) {
  // PE / MZ.
  if (v.eq(0, "MZ"sv)) {
    const std::uint32_t pe = v.u32le(0x3C);
    if (v.has(pe, 24) && v.eq(pe, "PE\x00\x00"sv)) {
      const std::uint16_t characteristics = v.u16le(pe + 22);
      const std::size_t optional = pe + 24;
      const std::uint16_t magic = v.u16le(optional);
      const std::uint16_t subsystem = (magic == 0x10B || magic == 0x20B) ? v.u16le(optional + 68) : 0;
      if (subsystem >= 10 && subsystem <= 13) return found("efi", 95);
      if ((characteristics & 0x2000) != 0) return found(refine(filename, "dll", {"ocx", "cpl", "drv"}), 93);
      if (subsystem == 1) return found("sys", 92);
      return found(refine(filename, "exe", {"scr", "com"}), 93);
    }
    if (v.has(pe, 2) && pe >= 0x40 && v.eq(pe, "NE"sv)) return found(refine(filename, "exe", {"fon"}), 88);
    return nothing();
  }
  // ELF.
  if (v.eq(0, "\x7F" "ELF"sv)) {
    if (v.eq(8, "AI\x02"sv)) return found("appimage", 97);
    const bool wide = v.u8(4) == 2;
    const bool big = v.u8(5) == 2;
    if (!v.has(0, 18)) return nothing();
    const std::uint16_t type = v.u16(16, big);
    if (type == 1) return found(refine(filename, "o", {"ko"}), 92);
    if (type == 2) return found("elf", 92);
    if (type == 4) return found("core", 92);
    if (type == 3) {
      const std::uint64_t phoff = wide ? v.u64(32, big) : v.u32(28, big);
      const std::size_t phentsize = v.u16(wide ? 54 : 42, big);
      const std::size_t phnum = std::min<std::size_t>(v.u16(wide ? 56 : 44, big), 512);
      bool interp = false;
      bool known = phnum > 0 && phentsize >= 8;
      for (std::size_t i = 0; i < phnum && known; ++i) {
        const std::uint64_t at = phoff + static_cast<std::uint64_t>(i) * phentsize;
        if (!v.has64(at, 4)) {
          known = false;
          break;
        }
        if (v.u32(static_cast<std::size_t>(at), big) == 3) interp = true;
      }
      if (!known) return nothing();
      return found(interp ? "elf" : "so", 92);
    }
    return nothing();
  }
  // Mach-O and universal binaries; Java class files share CA FE BA BE.
  if (const Verdict m = classify_macho_header(v, 0); !m.id.empty()) return m;
  if (v.eq(0, "\xCA\xFE\xBA\xBE"sv) || v.eq(0, "\xCA\xFE\xBA\xBF"sv)) {
    const std::uint32_t word = v.u32be(4);
    if (!v.has(0, 8)) return nothing();
    if (v.u8(3) == 0xBE && (word & 0xFFFFu) >= 45 && (word >> 16) < 0x100) return found("class", 92);
    if (word >= 1 && word < 45) {
      const bool wide = v.u8(3) == 0xBF;
      const std::uint64_t offset = wide ? v.u64be(16) : v.u32be(16);
      if (offset > 0 && v.has64(offset, 16)) {
        const Verdict inner = classify_macho_header(v, static_cast<std::size_t>(offset));
        if (!inner.id.empty()) return found(inner.id, 93);
      }
      return found("macho", 88);
    }
    return nothing();
  }
  // COFF object files (no magic: a machine type and a plausible header).
  if (v.has(0, 20)) {
    if (v.eq(0, "\x00\x00\xFF\xFF"sv) && v.u16le(4) >= 1) return found("coff", 80);  // "bigobj" / anonymous object
    const std::uint16_t machine = v.u16le(0);
    const std::uint16_t sections = v.u16le(2);
    const std::uint32_t symbols = v.u32le(8);
    if ((machine == 0x014C || machine == 0x8664 || machine == 0xAA64 || machine == 0x01C4 || machine == 0x0200) &&
        sections >= 1 && sections <= 96 && v.u16le(16) == 0 && symbols < v.size() && (v.u16le(18) & 0x0002) == 0) {
      // The section table must fit and name sections like ".text".
      if (v.has(20, 40) && v.u8(20) == '.') return found("coff", 82);
    }
  }
  return nothing();
}

// ============================================================================ disk images

Verdict classify_disk(const View& v, std::string_view filename) {
  for (const std::size_t at : {std::size_t{32769}, std::size_t{34817}, std::size_t{36865}}) {
    if (v.eq(at, "CD001"sv) || v.eq(at, "BEA01"sv)) return found(refine(filename, "iso", {"toast"}), 95);
  }
  if (v.size() >= 512 && v.eq(v.size() - 512, "conectix"sv)) return found("vhd", 95);
  if (v.size() >= 512 && v.eq(v.size() - 512, "koly"sv)) return found("dmg", 95);
  if (v.eq(512, "EFI PART"sv) || v.eq(4096, "EFI PART"sv)) return base_reading("img", 84);
  if (v.eq(32, "NXSB"sv)) return base_reading("img", 84);
  if (v.eq(1024, "H+\x00\x04"sv) || v.eq(1024, "HX\x00\x05"sv)) return base_reading("img", 84);
  if (v.eq(3, "NTFS    "sv) || v.eq(3, "EXFAT   "sv)) return base_reading("img", 84);
  if (v.eq(0x10040, "_BHRfS_M"sv) || v.eq(0, "XFSB"sv)) return base_reading("img", 84);
  if (v.eq(1080, "\x53\xEF"sv) && v.has(0, 2048) && v.u32le(1024 + 76) <= 1) return base_reading("img", 80);
  const bool jump = (v.u8(0) == 0xEB && v.u8(2) == 0x90) || v.u8(0) == 0xE9;
  const std::uint16_t bytes_per_sector = v.u16le(11);
  if (jump && (bytes_per_sector == 512 || bytes_per_sector == 1024 || bytes_per_sector == 2048 || bytes_per_sector == 4096) &&
      (v.eq(54, "FAT"sv) || v.eq(82, "FAT32"sv))) {
    return base_reading("img", 84);
  }
  // A master boot record: boot signature and four sane partition entries.
  if (v.has(0, 512) && v.u8(510) == 0x55 && v.u8(511) == 0xAA) {
    bool sane = true;
    bool any = false;
    for (std::size_t i = 0; i < 4; ++i) {
      const std::size_t e = 446 + i * 16;
      const std::uint8_t status = v.u8(e);
      if (status != 0x00 && status != 0x80) sane = false;
      if (v.u8(e + 4) != 0) any = true;
    }
    if (sane && any) return base_reading("img", 78);
  }
  return nothing();
}

// ============================================================================ SQLite

Verdict classify_sqlite(const View& v) {
  if (!v.eq(0, "SQLite format 3\x00"sv)) return nothing();
  if (v.has(68, 4)) {
    const std::uint32_t application = v.u32be(68);
    if (application == 0x47504B47u || application == 0x47503130u || application == 0x47503131u) return found("gpkg", 97);
    if (application == 0x4D504258u) return found("mbtiles", 97);
  }
  return base_reading("sqlite", 84);
}

// ============================================================================ gzip

Verdict classify_gzip(const View& v, std::string_view filename) {
  if (!v.eq(0, "\x1F\x8B\x08"sv) || !v.has(0, 10)) return nothing();
  const std::uint8_t flags = v.u8(3);
  if ((flags & 0xE0) != 0) return nothing();
  std::size_t at = 10;
  bool bgzf = false;
  if ((flags & 0x04) != 0) {
    const std::size_t extra = v.u16le(at);
    bgzf = v.eq(at + 2, "BC"sv);
    at += 2 + extra;
  }
  std::string_view original;
  if ((flags & 0x08) != 0) {
    const std::size_t end = v.find("\0"sv, at, at + 1024);
    if (end == npos) return container_only();
    original = v.str(at, end - at);
    at = end + 1;
  }
  if ((flags & 0x10) != 0) {
    const std::size_t end = v.find("\0"sv, at, at + 65536);
    if (end == npos) return container_only();
    at = end + 1;
  }
  if ((flags & 0x02) != 0) at += 2;
  if (at > v.size()) return container_only();
  Bytes head;
  {
    util::Inflater inflater(util::Inflater::Format::Raw);
    const std::size_t take = std::min<std::size_t>(v.size() - at, 256 * 1024);
    (void)inflater.feed(v.d.subspan(at, take), head, 512);
  }
  const View h{head};
  if (h.size() >= 4) {
    if (h.eq(0, "BAM\x01"sv)) return found("bam", 97);
    if (h.eq(0, "BCF\x02"sv)) return found("bcf", 97);
    if (h.eq(0, "RDX2\n"sv) || h.eq(0, "RDX3\n"sv) || h.eq(0, "RDA2\n"sv) || h.eq(0, "RDA3\n"sv)) return found("rdata", 95);
    if (h.eq(0, "X\n"sv) || h.eq(0, "A\n"sv) || h.eq(0, "B\n"sv)) {
      if (h.has(0, 6) && h.u32be(2) >= 2 && h.u32be(2) <= 3) return found("rds", 92);
    }
    if (h.eq(0, "Vgm "sv)) return found("vgz", 95);
    if (h.eq(0, "\x01\x00\x00\x00"sv) && h.eq(40, " EMF"sv)) return found("emz", 95);
    if (h.eq(0, "\xD7\xCD\xC6\x9A"sv) || h.eq(0, "\x01\x00\x09\x00\x00\x03"sv) || h.eq(0, "\x02\x00\x09\x00\x00\x03"sv)) {
      return found("wmz", 92);
    }
    if (h.eq(344, "n+1\x00"sv)) return found("nii.gz", 95);
    if (tar_header(h, 0) && h.size() >= 512) {
      return found(refine(filename, "tar.gz", {"crate", "unitypackage"}), 95);
    }
    const std::string_view text = trim_left(h.chars());
    if (text.starts_with("<")) {
      if (text.find("<svg") != npos) return found("svgz", 95);
      if (text.find("<gnm:Workbook") != npos) return found("gnumeric", 95);
      if (text.find("<Ableton") != npos) return found("als", 95);
      if (text.find("<xournal") != npos) return found("xopp", 95);
      if (text.find("<PremiereData") != npos) return found("prproj", 95);
    }
  }
  if (original.ends_with(".tar")) return found("tar.gz", 90);
  if (bgzf) return base_reading("gz", 82);
  return base_reading("gz", 80);
}

// ============================================================================ ASF

Verdict classify_asf(const View& v, std::string_view filename) {
  constexpr std::string_view header = "\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C"sv;
  constexpr std::string_view stream_properties = "\x91\x07\xDC\xB7\xB7\xA9\xCF\x11\x8E\xE6\x00\xC0\x0C\x20\x53\x65"sv;
  constexpr std::string_view video_media = "\xC0\xEF\x19\xBC\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B"sv;
  constexpr std::string_view audio_media = "\x40\x9E\x69\xF8\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B"sv;
  if (!v.eq(0, header)) return nothing();
  const std::uint64_t size = v.u64le(16);
  const std::size_t end = static_cast<std::size_t>(std::min<std::uint64_t>(size, std::min<std::size_t>(v.size(), 1 << 20)));
  bool video = false;
  bool audio = false;
  for (std::size_t at = v.find(stream_properties, 30, end); at != npos; at = v.find(stream_properties, at + 16, end)) {
    if (v.eq(at + 24, video_media)) video = true;
    if (v.eq(at + 24, audio_media)) audio = true;
  }
  if (video) return found(refine(filename, "wmv", {"asf", "dvr-ms"}), 93);
  if (audio) return found(refine(filename, "wma", {"asf"}), 93);
  return base_reading("asf", 80);
}

// ============================================================================ DER

struct Tlv {
  std::uint8_t tag = 0;
  std::size_t content = 0;  // offset of the content
  std::size_t length = 0;
  bool ok = false;
};

Tlv der_at(const View& v, std::size_t at) {
  Tlv t;
  if (!v.has(at, 2)) return t;
  t.tag = v.u8(at);
  const std::uint8_t first = v.u8(at + 1);
  std::size_t header = 2;
  std::size_t length = first;
  if (first & 0x80) {
    const std::size_t count = first & 0x7F;
    if (count == 0 || count > 4 || !v.has(at + 2, count)) return t;
    length = 0;
    for (std::size_t i = 0; i < count; ++i) length = (length << 8) | v.u8(at + 2 + i);
    header += count;
  }
  t.content = at + header;
  t.length = length;
  t.ok = true;
  return t;
}
std::size_t der_next(const Tlv& t) { return t.content + t.length; }

Verdict classify_der(const View& v, std::string_view filename) {
  const Tlv outer = der_at(v, 0);
  if (!outer.ok || outer.tag != 0x30 || outer.length < 8) return nothing();
  // A file holds one DER value; allow a cut-off sample but not a much longer file.
  if (der_next(outer) < v.size() && v.size() - der_next(outer) > 2) return nothing();
  const Tlv a = der_at(v, outer.content);
  if (!a.ok) return container_only();
  if (a.tag == 0x06) {
    const std::string_view oid = v.str(a.content, a.length);
    if (oid == "\x2A\x86\x48\x86\xF7\x0D\x01\x07\x02"sv) return found(refine(filename, "p7b", {"p7s", "p7m", "cat", "mobileprovision"}), 92);
    if (oid == "\x2A\x86\x48\x86\xF7\x0D\x01\x07\x03"sv) return found("p7m", 90);
    return container_only();
  }
  if (a.tag == 0x02) {
    const bool zero = a.length == 1 && v.u8(a.content) == 0;
    const Tlv b = der_at(v, der_next(a));
    if (a.length == 1 && v.u8(a.content) == 3 && b.ok && b.tag == 0x30) return found("p12", 92);
    if (zero && b.ok && (b.tag == 0x30 || b.tag == 0x02)) return found("private-key", 88);
    if (a.length == 1 && v.u8(a.content) == 1 && b.ok && b.tag == 0x04) return found("private-key", 88);
    return container_only();
  }
  if (a.tag == 0x30) {
    // Signed structure: tbs, algorithm, signature bit string.
    const Tlv tbs_first = der_at(v, a.content);
    const Tlv alg = der_at(v, der_next(a));
    if (!tbs_first.ok) return container_only();
    if (tbs_first.tag == 0xA0) return found("der", 92);
    if (tbs_first.tag == 0x06 && alg.ok && alg.tag == 0x04) return found("private-key", 85);  // encrypted PKCS #8
    if (tbs_first.tag == 0x02) {
      const Tlv x = der_at(v, der_next(tbs_first));
      const Tlv y = der_at(v, der_next(x));
      const Tlv z = der_at(v, der_next(y));
      if (x.ok && y.ok && x.tag == 0x30 && y.tag == 0x30 && z.ok) {
        if (z.tag == 0xA0) return found("csr", 92);
        if (z.tag == 0x30) return found("der", 90);
        if (z.tag == 0x17 || z.tag == 0x18) return found("crl", 90);
      }
    }
    if (tbs_first.tag == 0x30) {
      const Tlv x = der_at(v, der_next(tbs_first));
      const Tlv y = der_at(v, der_next(x));
      if (x.ok && y.ok && x.tag == 0x30 && (y.tag == 0x17 || y.tag == 0x18)) return found("crl", 90);
    }
  }
  return container_only();
}

// ============================================================================ format-specific checks

Verdict check_png_animation(const View& v) {
  if (!v.eq(0, "\x89PNG\r\n\x1A\n"sv)) return nothing();
  std::size_t at = 8;
  for (int n = 0; n < 1024 && v.has(at, 8); ++n) {
    const std::uint32_t length = v.u32be(at);
    if (v.eq(at + 4, "acTL"sv)) return found("apng", 97);
    if (v.eq(at + 4, "IDAT"sv)) break;
    if (length > v.size()) break;
    at += 12 + static_cast<std::size_t>(length);
  }
  return container_only();
}

Verdict check_netpbm(const View& v) {
  if (v.u8(0) != 'P' || !v.has(0, 3) || !is_space(static_cast<char>(v.u8(2)))) return nothing();
  const char kind = static_cast<char>(v.u8(1));
  std::string_view id;
  switch (kind) {
    case '1': case '4': id = "pbm"; break;
    case '2': case '5': id = "pgm"; break;
    case '3': case '6': id = "ppm"; break;
    case '7': id = "pam"; break;
    case 'F': case 'f': id = "pfm"; break;
    default: return nothing();
  }
  if (kind == '7') return v.find("WIDTH"sv, 3, 256) != npos ? found(id, 92) : nothing();
  // Width (after optional comments) must follow.
  std::size_t at = 2;
  for (int guard = 0; guard < 64 && at < v.size(); ++guard) {
    const char c = static_cast<char>(v.u8(at));
    if (is_space(c)) {
      ++at;
    } else if (c == '#') {
      const std::size_t eol = v.find("\n"sv, at, at + 1024);
      if (eol == npos) return nothing();
      at = eol + 1;
    } else {
      return is_digit(c) || (kind != 'F' && kind != 'f' ? false : c == '-') ? found(id, 90) : nothing();
    }
  }
  return nothing();
}

Verdict check_icon(const View& v, std::string_view wanted) {
  if (v.eq(0, "\x00\x00\x01\x00"sv) && wanted == "dfont") {
    // Resource fork: data offset 256, then map offset, data length and map length (big-endian).
    const std::uint64_t data_length = v.u32be(8);
    const std::uint64_t map_offset = v.u32be(4);
    const std::uint64_t map_length = v.u32be(12);
    if (map_offset >= 256 && map_offset == 256 + data_length && map_offset + map_length == v.size()) return found("dfont", 92);
    return nothing();
  }
  if (v.u16le(0) != 0 || (v.u16le(2) != 1 && v.u16le(2) != 2) || !v.has(0, 6)) return nothing();
  const std::size_t count = v.u16le(4);
  if (count == 0 || count > 256) return nothing();
  const std::string_view id = v.u16le(2) == 1 ? "ico" : "cur";
  if (!v.has(6, 16)) return container_only();
  const std::uint32_t bytes = v.u32le(6 + 8);
  const std::uint32_t offset = v.u32le(6 + 12);
  if (v.u8(6 + 3) != 0 || bytes == 0 || offset < 6 + 16 * count) return nothing();
  if (id == "ico" && v.u16le(6 + 4) > 1) return nothing();
  return found(id, 92);
}

Verdict check_dbf(const View& v) {
  static constexpr std::uint8_t versions[] = {0x02, 0x03, 0x04, 0x05, 0x30, 0x31, 0x32, 0x43, 0x63,
                                              0x83, 0x8B, 0x8E, 0xCB, 0xF5, 0xFB};
  if (!v.has(0, 32) || std::ranges::find(versions, v.u8(0)) == std::end(versions)) return nothing();
  const int month = v.u8(2);
  const int day = v.u8(3);
  const std::size_t header = v.u16le(8);
  const std::size_t record = v.u16le(10);
  if (month < 1 || month > 12 || day < 1 || day > 31 || header < 33 || record == 0) return nothing();
  if (v.u8(0) == 0x30 || v.u8(0) == 0x31 || v.u8(0) == 0x32) {
    if (header < 33 + 263) return nothing();
  } else if ((header - 1) % 32 != 0) {
    return nothing();
  }
  if (v.has(header - 1, 1) && v.u8(0) != 0x30 && v.u8(0) != 0x31 && v.u8(0) != 0x32 && v.u8(header - 1) != 0x0D) return nothing();
  return found("dbf", 90);
}

Verdict check_stl(const View& v) {
  if (v.eq(0, "solid"sv)) {
    if (v.find("facet"sv, 5, 4096) != npos || v.find("endsolid"sv, 5, 4096) != npos) return found("stl", 95);
  }
  if (v.size() >= 84) {
    const std::uint64_t triangles = v.u32le(80);
    if (84 + triangles * 50 == v.size()) return found("stl", 92);
  }
  return nothing();
}

Verdict check_pyc(const View& v) {
  if (!v.has(0, 8) || v.u8(2) != 0x0D || v.u8(3) != 0x0A) return nothing();
  const std::uint16_t magic = v.u16le(0);
  if (magic == 62211 || (magic >= 20121 && magic <= 20200)) return found("pyc", 88);   // Python 2.7 / 1.5
  if (magic >= 3000 && magic <= 3800) {
    if (magic >= 3390 && v.u32le(4) > 3) return nothing();  // 3.7+: a flags word
    return found("pyc", 92);
  }
  return nothing();
}

Verdict check_pickle(const View& v) {
  if (v.u8(0) != 0x80 || v.u8(1) < 2 || v.u8(1) > 5 || !v.has(0, 3)) return nothing();
  if (v.u8(v.size() - 1) != '.') return nothing();
  return found("pkl", v.u8(1) >= 4 && v.u8(2) == 0x95 ? 90 : 82);
}

Verdict check_safetensors(const View& v) {
  const std::uint64_t header = v.u64le(0);
  if (!v.has(0, 9) || header < 2 || header > (std::uint64_t{100} << 20) || v.u8(8) != '{') return nothing();
  const std::size_t end = static_cast<std::size_t>(std::min<std::uint64_t>(8 + header, 8 + 65536));
  if (v.find("\"dtype\""sv, 8, end) != npos || v.find("\"__metadata__\""sv, 8, end) != npos) return found("safetensors", 95);
  return nothing();
}

Verdict check_onnx(const View& v) {
  // ModelProto: ir_version (field 1, varint) usually comes first.
  if (v.u8(0) != 0x08 || v.u8(1) == 0 || v.u8(1) > 20 || !v.has(0, 3)) return nothing();
  const std::uint8_t tag = v.u8(2);
  if (tag == 0x12 || tag == 0x1A || tag == 0x22 || tag == 0x28 || tag == 0x32 || tag == 0x3A || tag == 0x42) {
    return container_only();
  }
  return nothing();
}

Verdict check_torrent(const View& v) {
  if (v.u8(0) != 'd' || !is_digit(static_cast<char>(v.u8(1)))) return nothing();
  if (v.find("4:infod"sv, 1, 1 << 16) != npos && (v.eq(1, "8:announce"sv) || v.eq(1, "13:announce-list"sv) ||
                                                   v.eq(1, "7:comment"sv) || v.eq(1, "10:created by"sv) ||
                                                   v.eq(1, "13:creation date"sv) || v.eq(1, "8:encoding"sv) ||
                                                   v.eq(1, "4:info"sv) || v.eq(1, "9:url-list"sv))) {
    return found("torrent", 95);
  }
  return nothing();
}

Verdict check_3ds(const View& v) {
  if (v.u16le(0) != 0x4D4D || !v.has(0, 8)) return nothing();
  const std::uint64_t size = v.u32le(2);
  const std::uint16_t child = v.u16le(6);
  if (size >= 16 && size <= v.size() + 0 && (child == 0x0002 || child == 0x3D3D)) return found("3ds", 92);
  return nothing();
}

Verdict check_iges(const View& v) {
  if (!v.has(0, 80) || v.u8(72) != 'S') return nothing();
  const std::string_view sequence = v.str(73, 7);
  if (trim(sequence) == "1") return found("igs", 92);
  return nothing();
}

Verdict check_adx(const View& v) {
  if (v.u8(0) != 0x80 || v.u8(1) != 0x00 || !v.has(0, 6)) return nothing();
  const std::size_t offset = v.u16be(2);
  if (offset >= 4 && v.eq(offset - 2, "(c)CRI"sv)) return found("adx", 95);
  return nothing();
}

Verdict check_pict(const View& v) {
  if (!v.has(0, 528)) return nothing();
  if (v.eq(522, "\x00\x11\x02\xFF"sv) || v.eq(522, "\x11\x01"sv)) return found("pict", 90);
  return nothing();
}

Verdict check_xwd(const View& v) {
  for (const bool big : {true, false}) {
    const std::uint32_t header = v.u32(0, big);
    if (v.has(0, 100) && v.u32(4, big) == 7 && header >= 100 && header <= 1024 && v.u32(8, big) <= 2) return found("xwd", 88);
  }
  return nothing();
}

Verdict check_pcx(const View& v) {
  if (!v.has(0, 128) || v.u8(0) != 0x0A || v.u8(2) != 1) return nothing();
  const std::uint8_t version = v.u8(1);
  const std::uint8_t depth = v.u8(3);
  const std::uint8_t planes = v.u8(65);
  if ((version != 0 && version != 2 && version != 3 && version != 4 && version != 5) || v.u8(64) != 0) return nothing();
  if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return nothing();
  if (planes < 1 || planes > 4 || v.u16le(8) < v.u16le(4) || v.u16le(10) < v.u16le(6)) return nothing();
  return found("pcx", 88);
}

Verdict check_tga(const View& v) {
  if (v.size() >= 26 && v.eq(v.size() - 18, "TRUEVISION-XFILE.\0"sv)) return found("tga", 95);
  if (!v.has(0, 18)) return nothing();
  const std::uint8_t map_type = v.u8(1);
  const std::uint8_t type = v.u8(2);
  const std::uint8_t depth = v.u8(16);
  if (map_type > 1) return nothing();
  if (type != 1 && type != 2 && type != 3 && type != 9 && type != 10 && type != 11) return nothing();
  if (depth != 8 && depth != 15 && depth != 16 && depth != 24 && depth != 32) return nothing();
  if (v.u16le(12) == 0 || v.u16le(14) == 0) return nothing();
  if (map_type == 0 && (v.u16le(3) != 0 || v.u16le(5) != 0 || v.u8(7) != 0)) return nothing();
  return container_only();
}

Verdict check_bmp(const View& v) {
  if (!v.eq(0, "BM"sv) || !v.has(0, 18)) return nothing();
  const std::uint32_t dib = v.u32le(14);
  const std::uint32_t offset = v.u32le(10);
  if (dib != 12 && dib != 40 && dib != 52 && dib != 56 && dib != 64 && dib != 108 && dib != 124) return nothing();
  if (offset < 14 + dib) return nothing();
  return found("bmp", 92);
}

Verdict check_wem(const View& v) {
  if (!v.eq(0, "RIFF"sv) || !v.eq(8, "WAVE"sv) || !v.eq(12, "fmt "sv)) return nothing();
  return v.u16le(20) == 0xFFFE || v.u16le(20) == 0xFFFF ? found("wem", 96) : nothing();
}

Verdict check_gameboy(const View& v, std::string_view wanted) {
  if (!v.eq(0x104, "\xCE\xED\x66\x66\xCC\x0D\x00\x0B"sv)) return nothing();
  const std::uint8_t flag = v.u8(0x143);
  const std::string_view id = (flag == 0x80 || flag == 0xC0) ? "gbc" : "gb";
  (void)wanted;
  return found(id, 96);
}

Verdict check_size_named(const View& v, std::string_view wanted) {
  if (wanted == "hgt" && (v.size() == 2884802 || v.size() == 25934402)) return container_only();
  if (wanted == "d64" && (v.size() == 174848 || v.size() == 175531 || v.size() == 196608 || v.size() == 197376)) {
    return container_only();
  }
  if (wanted == "wbmp" && v.u8(0) == 0 && v.u8(1) == 0 && v.has(0, 4)) return container_only();
  return nothing();
}

Verdict check_illustrator(const View& v) {
  if (!v.eq(0, "%PDF-"sv) && !v.eq(0, "%!PS-Adobe"sv)) return nothing();
  const std::size_t window = std::size_t{1} << 18;
  if (v.find("<illustrator:"sv, 0, window) != npos || v.find("%%Creator: Adobe Illustrator"sv, 0, window) != npos ||
      v.find("%AI5_FileFormat"sv, 0, window) != npos) {
    return found("ai", 96);
  }
  return container_only();
}

Verdict check_multi_picture(const View& v) {
  if (!v.eq(0, "\xFF\xD8\xFF"sv)) return nothing();
  // The MP Format index lives in an APP2 segment of the first image.
  std::size_t at = 2;
  for (int n = 0; n < 32 && v.has(at, 4) && v.u8(at) == 0xFF; ++n) {
    const std::uint8_t marker = v.u8(at + 1);
    if (marker == 0xDA || marker == 0xD9) break;
    const std::size_t length = v.u16be(at + 2);
    // Phones also embed secondary images (depth and gain maps) this way, so
    // only the name can say the file is meant as a multi-picture object.
    if (marker == 0xE2 && v.eq(at + 4, "MPF\0"sv)) return container_only();
    at += 2 + length;
  }
  return nothing();
}

Verdict classify_misc(const View& v, std::string_view filename, std::string_view wanted) {
  if (wanted == "apng") return check_png_animation(v);
  if (wanted == "ai") return check_illustrator(v);
  if (wanted == "mpo") return check_multi_picture(v);
  if (in(wanted, {"pbm", "pgm", "ppm", "pam", "pfm"})) return check_netpbm(v);
  if (in(wanted, {"ico", "cur", "dfont"})) return check_icon(v, wanted);
  if (wanted == "dbf") return check_dbf(v);
  if (wanted == "stl") return check_stl(v);
  if (wanted == "pyc") return check_pyc(v);
  if (wanted == "pkl") return check_pickle(v);
  if (wanted == "safetensors") return check_safetensors(v);
  if (wanted == "onnx") return check_onnx(v);
  if (wanted == "torrent") return check_torrent(v);
  if (wanted == "3ds") return check_3ds(v);
  if (wanted == "igs") return check_iges(v);
  if (wanted == "adx") return check_adx(v);
  if (wanted == "pict") return check_pict(v);
  if (wanted == "xwd") return check_xwd(v);
  if (wanted == "pcx") return check_pcx(v);
  if (wanted == "tga") return check_tga(v);
  if (wanted == "bmp") return check_bmp(v);
  if (wanted == "wem") return check_wem(v);
  if (wanted == "gb" || wanted == "gbc") return check_gameboy(v, wanted);
  // Archives and models that are another format under a different name.
  if (wanted == "cbr") return v.eq(0, "Rar!\x1A\x07"sv) ? container_only() : nothing();
  if (wanted == "cb7") return v.eq(0, "7z\xBC\xAF\x27\x1C"sv) ? container_only() : nothing();
  if (wanted == "vrm") return v.eq(0, "glTF"sv) ? container_only() : nothing();
  // Magic numbers deep inside the file (kept out of the signature table, whose
  // patterns are meant for the first bytes).
  if (wanted == "mod") {
    for (const std::string_view tag : {"M.K."sv, "M!K!"sv, "FLT4"sv, "FLT8"sv, "4CHN"sv, "6CHN"sv, "8CHN"sv, "CD81"sv}) {
      if (v.eq(1080, tag)) return found("mod", 90);
    }
    return nothing();
  }
  if (wanted == "sms") return v.eq(0x7FF0, "TMR SEGA"sv) ? found("sms", 95) : nothing();
  if (wanted == "xiso") return v.eq(0x10000, "MICROSOFT*XBOX*MEDIA"sv) ? found("xiso", 95) : nothing();
  if (wanted == "photocd") return v.eq(2048, "PCD_IPI"sv) ? found("photocd", 95) : nothing();
  (void)filename;
  return check_size_named(v, wanted);
}

}  // namespace

// A one- or two-byte test that rejects most non-members before any parsing:
// detection asks every format about every file, so this keeps the common
// case to a few comparisons per format.
bool plausible(Family family, const View& v) {
  const std::uint8_t b = v.u8(0);
  switch (family) {
    case Family::None: return false;
    case Family::Zip: return b == 'P';
    case Family::Ole: return b == 0xD0;
    case Family::Bmff: return v.size() >= 8 && box_type_plausible(v, 4);
    case Family::Ebml: return b == 0x1A;
    case Family::Ogg: return b == 'O';
    case Family::Mpeg:
      return b == 0x47 || b == 0x00 || b == 'I' || b == 0xFF || b == 0x0B || b == 'f' || v.u8(4) == 0x47 || b == '#';
    case Family::Tiff: return b == 'I' || b == 'M';
    case Family::Markup:
    case Family::Json:
    case Family::Text: return b != 0 || v.u8(1) != 0;
    case Family::Tar: return v.size() >= 262;
    case Family::Exec: return b == 'M' || b == 0x7F || b == 0xCA || b == 0xCE || b == 0xCF || b == 0xFE || b == 0x4C || b == 0x64 || b == 0xC4 || b == 0x00;
    case Family::Disk: return v.size() >= 512;
    case Family::Sqlite: return b == 'S';
    case Family::Gzip: return b == 0x1F;
    case Family::Asf:
    case Family::Der: return b == 0x30;
    case Family::Misc: return true;
  }
  return false;
}

Verdict classify(Family family, ByteView data, std::string_view filename, std::string_view wanted) noexcept {
  if (data.empty()) return {};
  const View v{data};
  if (!plausible(family, v)) return {};
  switch (family) {
    case Family::None: return {};
    case Family::Zip: return classify_zip(v, filename, wanted);
    case Family::Ole: return classify_ole(v, filename);
    case Family::Bmff: return classify_bmff(v, filename);
    case Family::Ebml: return classify_ebml(v, filename);
    case Family::Ogg: return classify_ogg(v);
    case Family::Mpeg: return classify_mpeg(v, filename);
    case Family::Tiff: return classify_tiff(v, filename);
    case Family::Markup: return classify_markup(data, filename, wanted);
    case Family::Json: return classify_json(data, filename, wanted);
    case Family::Text: return classify_text(data, filename, wanted);
    case Family::Tar: return classify_tar_at(v, filename);
    case Family::Exec: return classify_exec(v, filename);
    case Family::Disk: return classify_disk(v, filename);
    case Family::Sqlite: return classify_sqlite(v);
    case Family::Gzip: return classify_gzip(v, filename);
    case Family::Asf: return classify_asf(v, filename);
    case Family::Der: return classify_der(v, filename);
    case Family::Misc: return classify_misc(v, filename, wanted);
  }
  return {};
}

}  // namespace cvt::sniff
