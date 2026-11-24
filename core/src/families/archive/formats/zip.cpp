// Convertible core - archive family: ZIP and the ZIP-based package formats (jar, apk, ipa, xpi, whl, nupkg, crx, cbz).
//
// All of them share one reader (zip_read.cpp) and one writer (zip_write.cpp);
// the package formats differ only in how they are recognised. Office
// documents and e-books (docx, xlsx, pptx, odt, epub, ...) belong to their own
// families and are never claimed here; the generic ZIP sniff stays below
// their scores.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/formats/formats.hpp"
#include "families/archive/formats/zip_internal.hpp"

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
};

void register_package(Registry& registry, Package p) {
  ArchiveFormat f;
  f.info.id = std::string(p.id);
  f.info.name = std::string(p.name);
  f.info.description = std::string(p.description);
  f.info.category = Category::Archive;
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

std::function<int(std::span<const std::byte>, std::string_view)> member_sniff(std::vector<std::string> members, int score) {
  return [members = std::move(members), score](std::span<const std::byte> data, std::string_view) {
    for (const std::string& m : members) {
      if (!mentions(data, m)) return 0;
    }
    return score;
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
                              {"jar"}, "application/java-archive", 40, {local}, member_sniff({"META-INF/MANIFEST.MF"}, 80), true});
  register_package(registry, {"apk", "Android package", "An Android application package (a signed ZIP archive).",
                              {"apk"}, "application/vnd.android.package-archive", 45, {local},
                              member_sniff({"AndroidManifest.xml"}, 88), true});
  register_package(registry, {"ipa", "iOS application archive", "An iOS application bundle packed as a ZIP archive.",
                              {"ipa"}, "application/x-ios-app", 20, {local}, member_sniff({"Payload/", ".app/"}, 85), true});
  register_package(registry, {"xpi", "Firefox extension", "A Mozilla add-on package (a signed ZIP archive).", {"xpi"},
                              "application/x-xpinstall", 10, {local},
                              [](std::span<const std::byte> data, std::string_view) {
                                return mentions(data, "META-INF/mozilla.rsa") || mentions(data, "install.rdf") ? 82 : 0;
                              }, true});
  register_package(registry, {"whl", "Python wheel", "A built Python package (a ZIP archive with .dist-info metadata).",
                              {"whl"}, "application/x-wheel+zip", 15, {local}, member_sniff({".dist-info/WHEEL"}, 85), true});
  register_package(registry, {"nupkg", "NuGet package", "A .NET package (a ZIP archive with a .nuspec manifest).",
                              {"nupkg", "snupkg"}, "application/x-nupkg", 10, {local}, member_sniff({".nuspec"}, 85), true});
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
  register_package(registry, {"cbz", "Comic book archive (ZIP)", "Comic book pages as images in a ZIP archive.", {"cbz"},
                              "application/vnd.comicbook+zip", 20, {local}, nullptr, true});
}

}  // namespace cvt::archive
