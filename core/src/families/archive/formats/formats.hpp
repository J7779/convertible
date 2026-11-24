// Convertible core - archive family: registration entry points of the container formats.
// Each function lives in the source file named next to it and registers its
// formats (FormatInfo, Archive codecs) on the registry.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::archive {

void register_zip_formats(Registry& registry);        // formats/zip.cpp: zip + zip-based aliases
void register_tar_formats(Registry& registry);        // formats/tar.cpp: tar and tar.* compounds
void register_stream_formats(Registry& registry);     // formats/streams.cpp: gz, bz2, xz, lzma, zst, lz4, br, lz, z, zz, sz
void register_unix_formats(Registry& registry);       // formats/unix.cpp: cpio, ar, deb, rpm, shar, warc, xar
void register_7z_format(Registry& registry);          // formats/sevenzip.cpp
void register_rar_format(Registry& registry);         // formats/rar.cpp
void register_iso_formats(Registry& registry);        // formats/iso.cpp: iso (ISO 9660 / Joliet / Rock Ridge / UDF)
void register_cab_formats(Registry& registry);        // formats/cab.cpp: cab, chm, msi (optional)
void register_wim_format(Registry& registry);         // formats/wim.cpp
void register_squashfs_format(Registry& registry);    // formats/squashfs.cpp
void register_dmg_format(Registry& registry);         // formats/dmg.cpp: dmg (UDIF + HFS+)
void register_legacy_formats(Registry& registry);     // formats/legacy.cpp: lha/lzh, arj, arc, zoo, sit

}  // namespace cvt::archive
