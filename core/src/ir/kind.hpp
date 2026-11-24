// Convertible core - the set of intermediate representations.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace cvt {

/// Which intermediate representation (IR) a value holds. Every codec reads a
/// format into, or writes a format from, exactly one kind; bridges convert
/// between kinds. The numeric values index `AnyIR`'s variant and planner
/// tables: append new kinds at the end, never reorder.
enum class Kind {
  Blob,       ///< ir::Blob      - opaque bytes; every format is implicitly readable as this
  Image,      ///< ir::Image     - raster frames
  Audio,      ///< ir::Audio     - PCM samples
  Document,   ///< ir::Document  - rich text tree
  Data,       ///< ir::Data      - JSON-like structured data
  Table,      ///< ir::Table     - workbook of sheets of cells
  Archive,    ///< ir::Archive   - named file entries
  Subtitles,  ///< ir::Subtitles - timed text cues
  Mesh,       ///< ir::Mesh      - 3D geometry
  Font,       ///< ir::Font      - SFNT table collection
  Geo,        ///< ir::Geo       - geographic features
  Calendar,   ///< ir::Calendar  - events
  Contacts,   ///< ir::Contacts  - address-book cards
  PlainText,  ///< ir::PlainText - plain Unicode text
  Video,      ///< ir::Video     - streaming video with audio (move-only)
};

inline constexpr std::size_t kind_count = 15;

inline constexpr std::array<Kind, kind_count> all_kinds = {
    Kind::Blob,    Kind::Image,     Kind::Audio, Kind::Document, Kind::Data, Kind::Table,    Kind::Archive,
    Kind::Subtitles, Kind::Mesh,    Kind::Font,  Kind::Geo,      Kind::Calendar, Kind::Contacts, Kind::PlainText,
    Kind::Video};

[[nodiscard]] constexpr std::string_view kind_name(Kind kind) noexcept {
  switch (kind) {
    case Kind::Blob: return "Blob";
    case Kind::Image: return "Image";
    case Kind::Audio: return "Audio";
    case Kind::Document: return "Document";
    case Kind::Data: return "Data";
    case Kind::Table: return "Table";
    case Kind::Archive: return "Archive";
    case Kind::Subtitles: return "Subtitles";
    case Kind::Mesh: return "Mesh";
    case Kind::Font: return "Font";
    case Kind::Geo: return "Geo";
    case Kind::Calendar: return "Calendar";
    case Kind::Contacts: return "Contacts";
    case Kind::PlainText: return "PlainText";
    case Kind::Video: return "Video";
  }
  return "Unknown";
}

[[nodiscard]] constexpr std::optional<Kind> parse_kind(std::string_view name) noexcept {
  for (const Kind kind : all_kinds) {
    if (kind_name(kind) == name) return kind;
  }
  return std::nullopt;
}

}  // namespace cvt
