// Convertible core - structured content sniffers for the known-formats database.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Many formats share one container and therefore one magic number: every
// OOXML document, JAR and EPUB is a ZIP file, Word, Excel and MSI files are
// OLE2 compound files, MP4, MOV, HEIC and CR3 are ISO base media files. A
// magic number can only say "ZIP"; the classifiers below look inside the
// container (entry names, stream names, brands, root elements, header fields)
// and name the specific format.
//
// Every classifier is a pure function of its input: bounds-checked on
// arbitrary and truncated data, linear in the bytes it inspects (and it
// inspects a bounded amount: headers, directories, the first lines of text),
// it never reads outside the span it is given, never throws and allocates
// little or nothing (the ZIP classifier inflates at most one small entry, the
// gzip classifier at most the first 512 bytes of the stream).
#pragma once

#include <cstdint>
#include <string_view>

#include "util/bytes.hpp"

namespace cvt::sniff {

/// The container or content family a classifier understands.
enum class Family : std::uint8_t {
  None,
  Zip,     ///< ZIP-based packages: OOXML, OpenDocument, EPUB, JAR/APK, KMZ, 3MF, iWork ...
  Ole,     ///< OLE2 compound files: doc/xls/ppt/msg/msi/vsd/pub/hwp ...
  Bmff,    ///< ISO base media files by `ftyp` brand: mp4/m4a/mov/3gp/heic/avif/cr3/jp2 ...
  Ebml,    ///< EBML DocType and track types: mkv/mka/mks/webm
  Ogg,     ///< Ogg by the codec of the first packets: ogg/opus/oga/spx/ogv
  Mpeg,    ///< MPEG audio frames (ID3 skipped), ADTS, AC-3, transport and program streams, elementary video
  Tiff,    ///< TIFF-based files by IFD tags: DNG, camera RAW, GeoTIFF, OME-TIFF, BigTIFF
  Markup,  ///< XML dialects by root element and namespace; HTML; binary property lists
  Json,    ///< JSON dialects by top-level keys; JSON Lines
  Text,    ///< text formats with tell-tale lines: shebangs, PEM, subtitles, playlists, source code ...
  Tar,     ///< tar by ustar magic or header checksum
  Exec,    ///< executables and libraries: PE, ELF, Mach-O (incl. universal), COFF, Java class
  Disk,    ///< disk and volume images by boot sector or superblock
  Sqlite,  ///< SQLite databases by application_id
  Gzip,    ///< gzip streams by header and the first decompressed bytes
  Asf,     ///< ASF (Windows Media) by stream types
  Der,     ///< DER-encoded certificates, keys and PKCS containers
  Misc,    ///< format-specific structural checks (the wanted id selects the check)
};

/// What a classifier concluded.
struct Verdict {
  /// The identified format id, or empty when the content is not specific.
  std::string_view id;
  /// 0..100; meaningful only when `id` is not empty.
  int confidence = 0;
  /// True when `id` is only the container's plain reading (generic XML, a
  /// plain MP4, a plain TIFF, an SQLite database without application id):
  /// a file name naming a more specific member of the family may refine it.
  bool base = false;
  /// True when the data is a structurally plausible member of the family
  /// (a ZIP file, an OLE2 file, text ...), whether or not it was identified.
  bool container = false;
};

/// Classify `data`. `filename` may be empty. `wanted` is the format id the
/// caller is asking about; classifiers use it only to skip work that cannot
/// change the answer for `wanted` (and, for Family::Misc, to choose the
/// check).
[[nodiscard]] Verdict classify(Family family, ByteView data, std::string_view filename,
                               std::string_view wanted) noexcept;

/// Confidence given to a format whose container is valid and whose file
/// extension names it when the content alone cannot tell (a ZIP named .cbz,
/// a TIFF named .nrw, text named .md). Every `base` verdict stays below it.
inline constexpr int named_confidence = 86;
/// The same for source code, where an extension is worth no more than the
/// extension-only confidence of the detector.
inline constexpr int weak_named_confidence = 78;

}  // namespace cvt::sniff
