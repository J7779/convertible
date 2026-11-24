// Convertible core - font family: sfnt containers (tables, directories, collections).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/limits.hpp"
#include "ir/font.hpp"
#include "util/bytes.hpp"

namespace cvt::font {

inline constexpr std::uint32_t tag_true_type = 0x00010000u;
inline constexpr std::uint32_t tag_otto = 0x4F54544Fu;  // 'OTTO'
inline constexpr std::uint32_t tag_true = 0x74727565u;  // 'true'
inline constexpr std::uint32_t tag_typ1 = 0x74797031u;  // 'typ1'
inline constexpr std::uint32_t tag_ttcf = 0x74746366u;  // 'ttcf'

/// Largest number of tables accepted in one face (real fonts have < 60).
inline constexpr std::uint32_t max_tables = 1024;
/// Largest number of faces accepted in one collection.
inline constexpr std::uint32_t max_faces = 4096;

/// A four-character tag as text ("cvt " keeps its space).
[[nodiscard]] std::string tag_text(std::uint32_t tag);
[[nodiscard]] std::uint32_t tag_value(std::string_view tag) noexcept;
/// True when a tag consists of printable ASCII (0x20..0x7E).
[[nodiscard]] bool tag_is_valid(std::uint32_t tag) noexcept;

/// The sfnt table checksum (sum of big-endian uint32 words, zero padded).
[[nodiscard]] std::uint32_t table_checksum(ByteView data) noexcept;

/// Big-endian helpers for in-place table edits (bounds checked; out-of-range
/// reads give 0, writes are ignored).
[[nodiscard]] std::uint16_t get_u16(ByteView data, std::size_t offset) noexcept;
[[nodiscard]] std::int16_t get_i16(ByteView data, std::size_t offset) noexcept;
[[nodiscard]] std::uint32_t get_u32(ByteView data, std::size_t offset) noexcept;
void put_u16(Bytes& data, std::size_t offset, std::uint16_t value) noexcept;
void put_u32(Bytes& data, std::size_t offset, std::uint32_t value) noexcept;

/// Location of one table in an sfnt file.
struct DirectoryEntry {
  std::uint32_t tag = 0;
  std::uint32_t checksum = 0;
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
};

/// Parse and validate the table directory of the face starting at
/// `face_offset` (0 for a plain font). Every entry is checked against the
/// file: in bounds, no duplicate tags, no partial overlap between tables.
[[nodiscard]] Result<std::vector<DirectoryEntry>> read_directory(ByteView file, std::size_t face_offset,
                                                                 std::uint32_t* sfnt_version = nullptr);

/// Options for reading sfnt data.
struct SfntReadOptions {
  /// Verify table checksums and only warn about mismatches (never fatal:
  /// many shipping fonts have wrong checksums).
  bool check_checksums = true;
};

/// Read one face of a plain sfnt file (TrueType, OpenType, 'true', 'typ1').
[[nodiscard]] Result<ir::Font> read_sfnt_face(ByteView file, std::size_t face_offset, const Context& ctx);
/// Read a plain sfnt font or a TTC/OTC collection. `face` selects the face
/// that becomes the primary one; the rest go to `other_faces`.
[[nodiscard]] Result<ir::Font> read_sfnt(ByteView file, std::int64_t face, const Context& ctx);
/// Offsets of the faces of a TTC (or {0} for a plain font).
[[nodiscard]] Result<std::vector<std::uint32_t>> collection_offsets(ByteView file);

/// The sfnt version word to write for a font.
[[nodiscard]] std::uint32_t output_sfnt_version(const ir::Font& font) noexcept;

/// Recommended physical order of table data (OpenType spec); the directory
/// itself is always sorted by tag.
[[nodiscard]] std::vector<const ir::FontTable*> physical_order(const ir::Font& font);

/// Serialise a face as a plain sfnt file: sorted directory, 4-byte aligned
/// tables, correct checksums and head.checkSumAdjustment.
[[nodiscard]] Result<Bytes> write_sfnt(const ir::Font& font);
/// Serialise several faces as a TTC (version 2 header without DSIG);
/// identical tables are stored once.
[[nodiscard]] Result<Bytes> write_collection(std::span<const ir::Font* const> faces);

/// Fill `font.names` from its name table.
void decode_names(ir::Font& font);

/// The faces of a font: itself first, then `other_faces`.
[[nodiscard]] std::vector<const ir::Font*> all_faces(const ir::Font& font);

}  // namespace cvt::font
