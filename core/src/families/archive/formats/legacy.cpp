// Convertible core - archive family: registration of the legacy DOS-era archive formats.
//
// lzh (LHA/LZH, lha_read.cpp), arj (arj_read.cpp) and arc (ARC/PAK,
// arc_read.cpp) are read only: nobody needs new archives in these formats,
// but old collections and BBS-era downloads still turn up. ZOO and StuffIt
// are not supported.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/formats/formats.hpp"
#include "families/archive/formats/legacy_util.hpp"

namespace cvt::archive {

void register_legacy_formats(Registry& registry) {
  ArchiveFormat lzh;
  lzh.info.id = "lzh";
  lzh.info.name = "LHA archive";
  lzh.info.description = "LHA / LZH archive (header levels 0 to 3), common on Japanese systems, the Amiga and in old DOS software.";
  lzh.info.category = Category::Archive;
  lzh.info.extensions = {"lzh", "lha"};
  lzh.info.mime = "application/x-lzh-compressed";
  lzh.info.popularity = 15;
  lzh.info.signatures = {Signature::at(2, "'-lh' ?? '-'"), Signature::at(2, "'-lz' ?? '-'")};
  lzh.info.sniff = legacy::sniff_lha;
  lzh.read = legacy::read_lha;
  register_archive_format(registry, std::move(lzh));

  ArchiveFormat arj;
  arj.info.id = "arj";
  arj.info.name = "ARJ archive";
  arj.info.description = "ARJ archive from the DOS era (methods 0 to 4).";
  arj.info.category = Category::Archive;
  arj.info.extensions = {"arj"};
  arj.info.mime = "application/x-arj";
  arj.info.popularity = 8;
  arj.info.signatures = {Signature::at(0, "60 EA")};
  arj.info.sniff = legacy::sniff_arj;
  arj.read = legacy::read_arj;
  register_archive_format(registry, std::move(arj));

  ArchiveFormat arc;
  arc.info.id = "arc";
  arc.info.name = "ARC archive";
  arc.info.description = "SEA ARC / PKPAK archive from the DOS era (stored, packed, squeezed, crunched and squashed entries).";
  arc.info.category = Category::Archive;
  arc.info.extensions = {"arc", "ark", "pak"};
  arc.info.mime = "application/x-arc";
  arc.info.popularity = 5;
  arc.info.sniff = legacy::sniff_arc;
  arc.read = legacy::read_arc;
  register_archive_format(registry, std::move(arc));
}

}  // namespace cvt::archive
