// Convertible core - archive family: the "extract" target.
//
// Turns a compressed file or a one-entry archive back into its content:
// "notes.txt.gz" -> "notes.txt", a ZIP holding one picture -> that picture.
// It is an output-only format ("Extracted file") whose writer takes the
// Archive read directly from the source (Codec::direct_only: never an
// arbitrary file wrapped into an archive by a bridge) and names its output
// through WriteContext::output_filename, so the result carries the entry's
// own file name, extension and MIME type. An archive with several files is an
// Encode error that names them: convert it to another archive format, or let
// the application unpack it.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/archive/formats/formats.hpp"
#include "ir/archive.hpp"

namespace cvt::archive {

namespace {

constexpr std::size_t listed_names = 8;

Result<Bytes> write_extract(const AnyIR& value, WriteContext& ctx) {
  const ir::Archive& archive = value.get<ir::Archive>();
  std::vector<const ir::ArchiveEntry*> files;
  for (const ir::ArchiveEntry& entry : archive.entries) {
    if (entry.is_file()) files.push_back(&entry);
  }
  if (files.empty()) return fail(Status::Encode, "the archive holds no file to extract");
  if (files.size() > 1) {
    std::string names;
    for (std::size_t i = 0; i < files.size() && i < listed_names; ++i) {
      if (i > 0) names += ", ";
      names += files[i]->path;
    }
    if (files.size() > listed_names) names += std::format(" and {} more", files.size() - listed_names);
    return fail(Status::Encode, "the archive holds {} files ({}); extract takes out a single file", files.size(), names);
  }
  const ir::ArchiveEntry& entry = *files.front();
  CVT_TRY(ctx.checkpoint(0.5, "Extracting"));
  const std::size_t slash = entry.path.find_last_of('/');
  ctx.output_filename = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
  return entry.bytes;
}

}  // namespace

void register_extract(Registry& registry) {
  FormatInfo info;
  info.id = "extract";
  info.name = "Extracted file";
  info.description = "The file inside a compressed file or a one-entry archive, with its own name and format.";
  info.category = Category::Archive;
  info.extensions = {"bin"};
  info.mime = "application/octet-stream";
  info.popularity = 60;
  info.output_only = true;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "extract";
  codec.kind = Kind::Archive;
  codec.write = write_extract;
  codec.direct_only = true;
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::archive
