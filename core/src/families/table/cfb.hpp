// Convertible core - Compound File Binary format (OLE2 / "structured storage").
//
// The container of the Office 97-2003 binary formats (.xls, .doc, .ppt),
// MSI installers, Outlook .msg files, Visual Basic projects inside OOXML,
// encrypted OOXML packages and more, specified in [MS-CFB]. A compound file
// is a little file system inside a file: storages (folders) and streams
// (files) addressed by paths, stored in sectors chained through a FAT.
//
// This header is self-contained and family-neutral on purpose: any family
// that needs a compound file can include "families/table/cfb.hpp" and link
// nothing else (the implementation is cfb.cpp in the same directory).
//
// Reading
// -------
//   CVT_TRY_ASSIGN(const CfbReader cfb, CfbReader::open(bytes, ctx.limits));
//   for (const CfbEntry& entry : cfb.entries()) { ... entry.path ... }
//   CVT_TRY_ASSIGN(const Bytes stream, cfb.read("Workbook"));         // case-insensitive path
//   CVT_TRY_ASSIGN(const Bytes props, cfb.read("\x05SummaryInformation"));
//
// The reader trusts nothing: sector numbers are range-checked, FAT, mini
// FAT, DIFAT and directory chains are followed with cycle detection, stream
// sizes are checked against their chains, the directory tree is walked
// iteratively with a visited set (a cyclic or shared sibling tree is
// rejected), and `Limits::max_archive_entries` / `max_decompressed_bytes`
// bound the number of entries and the bytes handed out. Version 3
// (512-byte sectors) and version 4 (4096-byte sectors) files are supported;
// a slightly truncated final sector is tolerated (zero-filled), as many
// writers produce such files.
//
// Writing
// -------
//   CfbWriter writer;
//   CVT_TRY(writer.add_stream("Workbook", workbook_bytes));
//   CVT_TRY(writer.add_stream("ObjectPool/_1234/Ole10Native", data)); // storages are created as needed
//   CVT_TRY_ASSIGN(const Bytes file, writer.finish());
//
// The writer produces version 3 files (512-byte sectors, 4096-byte mini
// stream cutoff), lays every chain out contiguously, keeps sibling trees
// balanced (all black, which is a valid red-black tree), and is
// deterministic: no timestamps, no random CLSIDs. Streams up to 4 GiB.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "engine/limits.hpp"
#include "util/bytes.hpp"

namespace cvt::table {

enum class CfbEntryType : std::uint8_t { Storage = 1, Stream = 2, Root = 5 };

/// One directory entry of a compound file.
struct CfbEntry {
  /// Entry name, UTF-8 (control characters such as the leading 0x05 of
  /// "\x05SummaryInformation" are kept).
  std::string name;
  /// Full path from the root, '/' separated, without the root's own name
  /// ("Workbook", "Macros/VBA/dir"). Empty for the root entry.
  std::string path;
  CfbEntryType type = CfbEntryType::Stream;
  /// Stream size in bytes (0 for storages; the mini stream size for the root).
  std::uint64_t size = 0;
  /// Class id of storages (16 bytes, as stored).
  std::array<std::uint8_t, 16> clsid{};
  /// Index of the parent storage in `entries()`; nullopt for the root.
  std::optional<std::size_t> parent;
  // Internal: first sector of the stream and directory links.
  std::uint32_t start_sector = 0;
  std::uint32_t left = 0xFFFFFFFFu;
  std::uint32_t right = 0xFFFFFFFFu;
  std::uint32_t child = 0xFFFFFFFFu;
};

class CfbReader {
 public:
  /// Parse the header, FAT, mini FAT and directory of `data`, which must
  /// outlive the reader. Status::Decode for anything malformed,
  /// Status::Limit when the directory has more entries than allowed.
  [[nodiscard]] static Result<CfbReader> open(ByteView data, const Limits& limits = {});

  /// Reachable entries in a stable order: the root first, then a
  /// depth-first walk of the tree (siblings in name order).
  [[nodiscard]] const std::vector<CfbEntry>& entries() const noexcept { return entries_; }
  /// Index of the entry with this path (ASCII case-insensitive per segment,
  /// as CFB names compare), if any.
  [[nodiscard]] std::optional<std::size_t> find(std::string_view path) const noexcept;
  [[nodiscard]] bool contains(std::string_view path) const noexcept { return find(path).has_value(); }
  /// Indices of the direct children of a storage (or the root, index 0).
  [[nodiscard]] std::vector<std::size_t> children(std::size_t storage) const;
  /// The content of a stream. Status::Decode for storages, damaged chains
  /// or missing paths; Status::Limit when the bytes read so far (over all
  /// calls) would exceed `max_decompressed_bytes`.
  [[nodiscard]] Result<Bytes> read(std::size_t index) const;
  [[nodiscard]] Result<Bytes> read(std::string_view path) const;
  /// Bytes per sector: 512 (version 3) or 4096 (version 4).
  [[nodiscard]] std::uint32_t sector_size() const noexcept { return sector_size_; }

 private:
  CfbReader() = default;
  [[nodiscard]] Result<std::vector<std::uint32_t>> chain(std::uint32_t start, const std::vector<std::uint32_t>& fat,
                                                         std::uint64_t max_length) const;
  [[nodiscard]] ByteView sector(std::uint32_t index) const noexcept;

  ByteView data_{};
  Limits limits_{};
  std::uint32_t sector_size_ = 512;
  std::uint32_t mini_cutoff_ = 4096;
  std::uint32_t sector_count_ = 0;
  std::vector<std::uint32_t> fat_;
  std::vector<std::uint32_t> mini_fat_;
  Bytes mini_stream_;
  std::vector<CfbEntry> entries_;
  mutable std::uint64_t handed_out_ = 0;
};

class CfbWriter {
 public:
  /// Add a stream at `path` ('/' separated); missing storages on the way
  /// are created. Names are at most 31 UTF-16 code units and must not
  /// contain '/', '\\', ':' or '!'. Adding the same path twice replaces it.
  [[nodiscard]] Result<void> add_stream(std::string_view path, ByteView data);
  /// Create an (empty) storage explicitly, optionally with a class id.
  [[nodiscard]] Result<void> add_storage(std::string_view path, const std::array<std::uint8_t, 16>& clsid = {});
  /// Class id of the root storage (e.g. Excel's 00020820-0000-0000-C000-000000000046).
  void set_root_clsid(const std::array<std::uint8_t, 16>& clsid) noexcept { root_clsid_ = clsid; }
  /// Serialise the compound file.
  [[nodiscard]] Result<Bytes> finish() const;

 private:
  struct Node {
    std::u16string name;
    bool storage = false;
    Bytes data;
    std::array<std::uint8_t, 16> clsid{};
    std::vector<std::size_t> children;  // indices into nodes_
  };
  [[nodiscard]] Result<std::size_t> node_for(std::string_view path, bool storage);
  std::vector<Node> nodes_{Node{u"Root Entry", true, {}, {}, {}}};
  std::array<std::uint8_t, 16> root_clsid_{};
};

}  // namespace cvt::table
