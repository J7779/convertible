// Convertible core - Font IR: an SFNT table collection.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::ir {

enum class FontFlavor {
  TrueType,  ///< glyf/loca outlines (sfnt version 0x00010000 or 'true')
  CFF,       ///< CFF or CFF2 outlines (sfnt version 'OTTO')
};

/// One SFNT table, uncompressed.
struct FontTable {
  /// Four-character tag exactly as in the table directory, e.g. "head",
  /// "OS/2", "cvt " (trailing spaces included).
  std::string tag;
  /// Raw table bytes without padding.
  Bytes bytes;
};

/// Human-readable names decoded from the `name` table (UTF-8).
struct FontNames {
  std::string family;           ///< name ID 1 (or 16 when present)
  std::string subfamily;        ///< name ID 2 (or 17)
  std::string full_name;        ///< name ID 4
  std::string postscript_name;  ///< name ID 6
  std::string version;          ///< name ID 5
};

/// A single font face. TTF, OTF, WOFF and WOFF2 are all containers for the
/// same tables, so conversion between them is lossless table repackaging.
struct Font {
  FontFlavor flavor = FontFlavor::TrueType;
  /// All tables of the face, in the order of the source directory. Writers
  /// sort by tag and recompute checksums and `head.checkSumAdjustment`.
  std::vector<FontTable> tables;
  /// Convenience copy of the main names; nullopt when the reader did not
  /// decode the `name` table.
  std::optional<FontNames> names;

  /// The table with this tag, or nullptr.
  [[nodiscard]] const FontTable* find(std::string_view tag) const noexcept {
    for (const FontTable& table : tables) {
      if (table.tag == tag) return &table;
    }
    return nullptr;
  }
};

}  // namespace cvt::ir
