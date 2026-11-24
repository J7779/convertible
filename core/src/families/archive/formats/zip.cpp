// Convertible core - archive family: ZIP and the ZIP-based package formats (jar, apk, ipa, xpi, whl, nupkg, crx, cbz).
//
// All of them share one reader (zip_read.cpp) and one writer (zip_write.cpp);
// the package formats differ only in how they are recognised. Office
// documents and e-books (docx, xlsx, pptx, odt, epub, ...) belong to their own
// families and are never claimed here; the generic ZIP sniff stays below
// their scores.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/sniffers.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/formats/formats.hpp"
#include "families/archive/formats/zip_internal.hpp"
#include "util/text.hpp"

namespace cvt::archive {

namespace {

bool starts_with(ByteView data, std::string_view magic) {
  return data.size() >= magic.size() && as_chars(data.first(magic.size())) == magic;
}

// Package formats are told apart by member names, which ZIP stores
// uncompressed in the local headers (front) and the central directory (back).
bool mentions(ByteView data, std::string_view member) {
  if (!starts_with(data, "PK\x03\x04")) return false;
  constexpr std::size_t window = 64 * 1024;
  const std::string_view text = as_chars(data);
  if (text.substr(0, window).find(member) != std::string_view::npos) return true;
  return text.size() > window && text.substr(text.size() - window).find(member) != std::string_view::npos;
}

// A ZIP that does not start with a local header (self-extractor stub, empty
// archive with a leading comment...): a consistent end record is enough.
int sniff_zip(std::span<const std::byte> data, std::string_view) {
  if (starts_with(data, "PK\x03\x04") || starts_with(data, "PK\x05\x06") || starts_with(data, "PK\x07\x08")) return 0;
  const std::size_t eocd = zip::find_eocd(data);
  if (eocd == static_cast<std::size_t>(-1) || eocd + 22 > data.size()) return 0;
  const auto le = [&](std::size_t at, int bytes) {
    std::uint64_t v = 0;
    for (int i = bytes; i-- > 0;) v = (v << 8) | std::to_integer<std::uint64_t>(data[at + static_cast<std::size_t>(i)]);
    return v;
  };
  const std::uint64_t entries = le(eocd + 10, 2);
  const std::uint64_t cd_size = le(eocd + 12, 4);
  if (entries == 0 || cd_size > eocd || cd_size < 46 * entries) return 0;
  const std::size_t cd = eocd - static_cast<std::size_t>(cd_size);
  return le(cd, 4) == 0x02014b50u ? 70 : 0;
}

struct Package {
  std::string_view id;
  std::string_view name;
  std::string_view description;
  std::vector<std::string> extensions;
  std::string_view mime;
  int popularity;
  std::vector<Signature> signatures;
  std::function<int(std::span<const std::byte>, std::string_view)> sniff;
  bool writable = true;
  Category category = Category::Archive;
};

void register_package(Registry& registry, Package p) {
  ArchiveFormat f;
  f.info.id = std::string(p.id);
  f.info.name = std::string(p.name);
  f.info.description = std::string(p.description);
  f.info.category = p.category;
  f.info.extensions = std::move(p.extensions);
  f.info.mime = std::string(p.mime);
  f.info.popularity = p.popularity;
  f.info.signatures = std::move(p.signatures);
  f.info.sniff = std::move(p.sniff);
  f.read = zip::read_zip;
  f.read_options = {option_password()};
  if (p.writable) {
    f.write = zip::write_zip;
    f.write_options = standard_write_options();
    f.write_options.push_back(option_method("deflate", {"deflate", "store", "bzip2", "lzma", "zstd", "xz"},
                                            "Compression method for the files: deflate (default, readable everywhere), "
                                            "store (no compression), or bzip2 / LZMA / Zstandard / xz (smaller, but "
                                            "only modern tools read them)."));
    f.write_options.push_back(option_password());
    f.write_options.push_back(option_encryption("aes256", {"aes256", "aes128", "zipcrypto"}));
  }
  register_archive_format(registry, std::move(f));
}

// The content decides through the shared ZIP classifier (the same one the
// detection database uses, so the confidences agree); the member names are
// the fallback for packages the classifier does not single out.
std::function<int(std::span<const std::byte>, std::string_view)> package_sniff(std::string id, std::vector<std::string> members,
                                                                            int score) {
  return [id = std::move(id), members = std::move(members), score](std::span<const std::byte> data, std::string_view name) {
    const sniff::Verdict verdict = sniff::classify(sniff::Family::Zip, data, name, id);
    int best = verdict.id == id ? verdict.confidence : 0;
    // A plain ZIP named after the package (the id is its extension).
    if (verdict.container && (verdict.id.empty() || verdict.base) && util::iends_with(name, "." + id)) {
      best = std::max(best, sniff::named_confidence);
    }
    if (!members.empty() && std::ranges::all_of(members, [&](const std::string& m) { return mentions(data, m); })) {
      best = std::max(best, score);
    }
    return best;
  };
}

}  // namespace

void register_zip_formats(Registry& registry) {
  const Signature local = Signature::at(0, "'PK' 03 04");
  register_package(registry, {"zip", "ZIP archive",
                              "The most widely used archive format; deflate by default, with optional bzip2, LZMA, "
                              "Zstandard or xz compression and AES or ZipCrypto encryption.",
                              {"zip", "zipx"}, "application/zip", 95,
                              {local, Signature::at(0, "'PK' 05 06"), Signature::at(0, "'PK' 07 08")}, sniff_zip, true});
  register_package(registry, {"jar", "Java archive", "A ZIP archive of Java classes and resources with a manifest.",
                              {"jar"}, "application/java-archive", 40, {}, package_sniff("jar", {"META-INF/MANIFEST.MF"}, 80), true});
  register_package(registry, {"apk", "Android package", "An Android application package (a signed ZIP archive).",
                              {"apk"}, "application/vnd.android.package-archive", 45, {},
                              package_sniff("apk", {"AndroidManifest.xml"}, 88), true});
  register_package(registry, {"ipa", "iOS application archive", "An iOS application bundle packed as a ZIP archive.",
                              {"ipa"}, "application/x-ios-app", 20, {}, package_sniff("ipa", {"Payload/", ".app/"}, 85), true});
  register_package(registry, {"xpi", "Firefox extension", "A Mozilla add-on package (a signed ZIP archive).", {"xpi"},
                              "application/x-xpinstall", 10, {},
                              [](std::span<const std::byte> data, std::string_view name) {
                                if (mentions(data, "META-INF/mozilla.rsa") || mentions(data, "install.rdf")) return 88;
                                return package_sniff("xpi", {}, 0)(data, name);
                              }, true});
  register_package(registry, {"whl", "Python wheel", "A built Python package (a ZIP archive with .dist-info metadata).",
                              {"whl"}, "application/x-wheel+zip", 15, {}, package_sniff("whl", {".dist-info/WHEEL"}, 85), true});
  register_package(registry, {"nupkg", "NuGet package", "A .NET package (a ZIP archive with a .nuspec manifest).",
                              {"nupkg", "snupkg"}, "application/x-nupkg", 10, {}, package_sniff("nupkg", {".nuspec"}, 85), true});
  register_package(registry, {"crx", "Chrome extension",
                              "A Chromium browser extension: a signed header ('Cr24') followed by a ZIP archive. "
                              "Read only (a valid package needs the publisher's signature).",
                              {"crx"}, "application/x-chrome-extension", 15, {Signature::at(0, "'Cr24'")},
                              [](std::span<const std::byte> data, std::string_view) {
                                if (!starts_with(data, "Cr24") || data.size() < 8) return 0;
                                const auto version = std::to_integer<int>(data[4]);
                                return (version == 2 || version == 3) && data[5] == std::byte{0} ? 90 : 0;
                              },
                              false});
  // Comic books: described here, read and written as archives of pages; the
  // office family adds the Document and Image codecs on the same id. The
  // content says "comic" only through ComicInfo.xml; otherwise the name decides.
  register_package(registry, {"cbz", "Comic book (ZIP)", "Comic book pages as images in a ZIP archive.", {"cbz"},
                              "application/vnd.comicbook+zip", 35, {}, package_sniff("cbz", {"ComicInfo.xml"}, 92), true,
                              Category::Ebook});
}

}  // namespace cvt::archive
