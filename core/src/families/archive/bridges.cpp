// Convertible core - archive family: Blob <-> Archive bridges.
//
//   Blob -> Archive   wrap any file as the single entry of an archive, so that
//                     every file can be zipped, gzipped, 7-zipped, ...
//   Archive -> Blob   take one file out of an archive: the only file, or the
//                     one named by the `entry` option.
//
// The Archive -> Blob bridge is deliberately expensive (see `extract_cost`):
// Blob writers (Base64, hex, checksums) must keep receiving the archive file
// itself through the engine's implicit raw reader, never its content.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <format>
#include <string>

#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/bridges.hpp"
#include "ir/archive.hpp"
#include "ir/blob.hpp"

namespace cvt::archive {

namespace {

constexpr int wrap_cost = 10;
// implicit raw read (1000) + writer < any archive reader + this + writer.
constexpr int extract_cost = 995;

std::string basename_of(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

}  // namespace

Result<ir::Archive> wrap_blob(ir::Blob blob, const Registry* registry) {
  ir::ArchiveEntry entry;
  std::string name = basename_of(blob.filename);
  auto safe = ir::sanitize_archive_path(name);
  if (!safe) {
    name = "data";
    if (registry != nullptr && !blob.format_id.empty()) {
      if (const FormatInfo* info = registry->find(blob.format_id); info != nullptr && !info->extensions.empty()) {
        name += "." + info->extensions.front();
      }
    }
    safe = name;
  }
  entry.path = *safe;
  entry.bytes = std::move(blob.bytes);
  ir::Archive archive;
  archive.entries.push_back(std::move(entry));
  return archive;
}

Result<ir::Blob> extract_entry(ir::Archive archive, std::string_view wanted) {
  std::size_t files = 0;
  ir::ArchiveEntry* chosen = nullptr;
  if (!wanted.empty()) {
    const auto target = ir::sanitize_archive_path(wanted);
    if (!target) return fail(Status::InvalidArgument, "option 'entry' is not a usable path");
    for (ir::ArchiveEntry& entry : archive.entries) {  // the last entry of a name wins, as on extraction
      if (entry.is_file() && entry.path == *target) chosen = &entry;
    }
    if (chosen == nullptr) return fail(Status::Encode, "the archive has no file named '{}'", *target);
  } else {
    for (ir::ArchiveEntry& entry : archive.entries) {
      if (!entry.is_file()) continue;
      ++files;
      chosen = &entry;
    }
    if (files == 0) return fail(Status::Encode, "the archive holds no files");
    if (files > 1) {
      return fail(Status::Encode, "the archive holds {} files; choose one with the 'entry' option", files);
    }
  }
  ir::Blob blob;
  blob.filename = basename_of(chosen->path);
  blob.bytes = std::move(chosen->bytes);
  return blob;
}

void register_bridges(Registry& registry) {
  Bridge wrap;
  wrap.from = Kind::Blob;
  wrap.to = Kind::Archive;
  wrap.cost = wrap_cost;
  wrap.lossy = false;
  wrap.label = "wrap file in archive";
  wrap.fn = [](AnyIR value, Context& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(ir::Archive archive, wrap_blob(std::move(value).get<ir::Blob>(), ctx.registry));
    return AnyIR(std::move(archive));
  };
  registry.add_bridge(std::move(wrap));

  Bridge extract;
  extract.from = Kind::Archive;
  extract.to = Kind::Blob;
  extract.cost = extract_cost;
  extract.lossy = true;
  extract.label = "extract one file";
  extract.options = {option_entry()};
  extract.fn = [](AnyIR value, Context& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(ir::Blob blob,
                   extract_entry(std::move(value).get<ir::Archive>(), ctx.options.get_string("entry", "")));
    return AnyIR(std::move(blob));
  };
  registry.add_bridge(std::move(extract));
}

}  // namespace cvt::archive
