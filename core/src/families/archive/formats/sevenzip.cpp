// Convertible core - archive family: the 7z format (registration; reader and writer live in sevenzip_*.cpp).
//
// Read: Copy, LZMA, LZMA2, PPMd (var.H), BZip2, Deflate, Deflate64,
// Zstandard (7-Zip-zstd), BCJ x86 / BCJ2 / PPC / IA64 / ARM / ARMT / ARM64 /
// SPARC / RISC-V / Delta filters, 7zAES, solid and non-solid folders,
// compressed and encrypted headers. Write: LZMA2 (default), LZMA, BZip2,
// Deflate, Copy (PPMd when an encoder exists), solid or not, optional AES-256
// with or without encrypted headers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/formats/formats.hpp"
#include "families/archive/formats/sevenzip_internal.hpp"

namespace cvt::archive {

void register_7z_format(Registry& registry) {
  ArchiveFormat f;
  f.info.id = "7z";
  f.info.name = "7-Zip archive";
  f.info.description =
      "The 7-Zip archive format: solid LZMA2 compression by default, with optional AES-256 encryption of the "
      "data and the file names.";
  f.info.category = Category::Archive;
  f.info.extensions = {"7z"};
  f.info.mime = "application/x-7z-compressed";
  f.info.popularity = 80;
  f.info.signatures = {Signature::at(0, "'7z' BC AF 27 1C")};
  f.read = sevenzip::read_7z;
  f.write = sevenzip::write_7z;
  f.read_options = {option_password()};
  f.write_options = standard_write_options();
  f.write_options.push_back(option_method("lzma2", {"lzma2", "lzma", "bzip2", "deflate", "copy", "ppmd"},
                                          "Compression method: LZMA2 (default, smallest for most data), LZMA, "
                                          "BZip2, Deflate, Copy (no compression) or PPMd (text; only when this "
                                          "build has a PPMd encoder, LZMA2 otherwise)."));
  f.write_options.push_back(option_solid(true));
  f.write_options.push_back(option_password());
  f.write_options.push_back(OptionSpec::boolean("encrypt_headers", "Encrypt file names", true,
                                                "With a password: also encrypt the archive header, so that "
                                                "the file names cannot be listed without the password.")
                                .advanced());
  register_archive_format(registry, std::move(f));
}

}  // namespace cvt::archive
