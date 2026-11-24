// Convertible core - archive family.
// Archives and compressed streams (zip, tar, gz, bz2, xz, zst, 7z, rar, iso, ...),
// the in-house compression codecs behind them (compression.hpp), and the
// Blob <-> Archive bridges.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/archive/bridges.hpp"
#include "families/archive/formats/formats.hpp"
#include "families/families.hpp"

namespace cvt {

void register_archive_family(Registry& registry) {
  archive::register_zip_formats(registry);
  archive::register_tar_formats(registry);
  archive::register_stream_formats(registry);
  archive::register_unix_formats(registry);
  archive::register_7z_format(registry);
  archive::register_rar_format(registry);
  archive::register_iso_formats(registry);
  archive::register_cab_formats(registry);
  archive::register_wim_format(registry);
  archive::register_squashfs_format(registry);
  archive::register_dmg_format(registry);
  archive::register_legacy_formats(registry);
  archive::register_bridges(registry);
}

}  // namespace cvt
