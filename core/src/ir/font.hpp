// Convertible core - Font IR: an SFNT table collection.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

/// One glyph of a bitmap font (BDF, PCF, PSF, FNT, embedded bitmaps).
struct BitmapGlyph {
  /// Unicode scalar value, or -1 when the glyph is not encoded.
  std::int32_t codepoint = -1;
  /// Glyph name where the source has one (BDF STARTCHAR, PCF glyph names).
  std::string name;
  /// Size of the bitmap in pixels (0 x 0 for blank glyphs such as space).
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// Offset of the bitmap's lower-left corner from the glyph origin on the
  /// baseline, in pixels (y up), as BDF's BBX does.
  std::int32_t x_offset = 0;
  std::int32_t y_offset = 0;
  /// Horizontal advance in pixels.
  std::int32_t advance = 0;
  /// `width * height` bytes, rows top to bottom; 0 = background, 1 = ink.
  std::vector<std::uint8_t> pixels;
};

/// A bitmap font: one strike of pre-rendered glyphs at a fixed pixel size.
struct BitmapFont {
  /// Nominal pixel size (ascent + descent for most sources).
  std::uint32_t pixel_size = 0;
  /// Pixels above / below the baseline.
  std::int32_t ascent = 0;
  std::int32_t descent = 0;
  /// Resolution the strike was designed for (dots per inch).
  std::uint32_t dpi_x = 75;
  std::uint32_t dpi_y = 75;
  std::vector<BitmapGlyph> glyphs;
  /// Free-form properties (BDF properties such as FOUNDRY, COPYRIGHT).
  std::vector<std::pair<std::string, std::string>> properties;
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

  /// The sfnt version word of the source (0x00010000, 'OTTO', 'true',
  /// 'typ1'); 0 = derive from `flavor`.
  std::uint32_t sfnt_version = 0;
  /// Further faces when the source was a collection (TTC/OTC, a WOFF2
  /// collection, a suitcase with several fonts). This face is the one chosen
  /// with the reader's `face` option; the others follow in source order.
  std::vector<Font> other_faces = {};
  /// A bitmap strike, for bitmap-only sources (BDF, PCF, PSF, FNT/FON, OTB).
  /// When `tables` is empty the font has no outlines.
  std::optional<BitmapFont> bitmap = {};
  /// WOFF extended metadata (uncompressed XML) and private data blocks.
  Bytes woff_metadata = {};
  Bytes woff_private = {};

  /// The table with this tag, or nullptr.
  [[nodiscard]] const FontTable* find(std::string_view tag) const noexcept {
    for (const FontTable& table : tables) {
      if (table.tag == tag) return &table;
    }
    return nullptr;
  }
  /// Mutable access to the table with this tag, or nullptr.
  [[nodiscard]] FontTable* find(std::string_view tag) noexcept {
    for (FontTable& table : tables) {
      if (table.tag == tag) return &table;
    }
    return nullptr;
  }
  /// Insert or replace a table.
  void set_table(std::string_view tag, Bytes bytes) {
    if (FontTable* table = find(tag)) {
      table->bytes = std::move(bytes);
    } else {
      tables.push_back(FontTable{std::string(tag), std::move(bytes)});
    }
  }
  /// Remove a table if present; true when one was removed.
  bool remove_table(std::string_view tag) {
    for (auto it = tables.begin(); it != tables.end(); ++it) {
      if (it->tag == tag) {
        tables.erase(it);
        return true;
      }
    }
    return false;
  }
};

}  // namespace cvt::ir
