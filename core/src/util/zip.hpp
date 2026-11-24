// Convertible core - ZIP container reader and writer (DEFLATE via util/deflate).
//
// Used directly by the archive family and indirectly by every ZIP-based
// format (docx, xlsx, pptx, odt, epub, 3mf, kmz, ...).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "engine/limits.hpp"
#include "util/bytes.hpp"
#include "util/deflate.hpp"

namespace cvt::util {

/// Turn a stored entry name into a safe relative path, or nullopt when
/// nothing safe remains (empty name, or one that tries to escape the
/// extraction root via "..").
///
/// Backslashes become '/', leading slashes are dropped, a drive-letter
/// prefix ("C:") is removed from the start of every path segment, "." and
/// empty segments are removed (so a directory name loses its trailing '/'),
/// control characters and NULs are stripped. Invalid UTF-8 must be repaired
/// by the caller first (`ZipEntry::name` already is).
///
/// The result can be appended to an extraction directory on any platform
/// without leaving it. (Reserved device names such as "NUL" or "COM1" are
/// not treated specially; code that creates files on Windows opens them with
/// the "\\?\" prefix or checks for them.)
[[nodiscard]] std::optional<std::string> sanitize_entry_path(std::string_view raw);

/// One central-directory record.
struct ZipEntry {
  std::string name;                    ///< as stored, converted to valid UTF-8; '/' separators; NOT sanitised
  std::uint64_t compressed_size = 0;
  std::uint64_t uncompressed_size = 0;
  std::uint32_t crc32 = 0;
  std::uint16_t method = 0;            ///< 0 = stored, 8 = deflate; others cannot be read
  bool is_directory = false;
  bool encrypted = false;              ///< encrypted entries cannot be read
  std::int64_t mtime = 0;              ///< Unix seconds (UTC); the DOS timestamp is interpreted as UTC
  std::uint32_t unix_mode = 0;         ///< POSIX permission/type bits when recorded, else 0
  std::uint64_t local_header_offset = 0;

  /// `name` as a path that is safe to create below an extraction root
  /// (see `sanitize_entry_path`; directories come without the trailing '/'),
  /// or nullopt when the name is unusable. Never extract to `name` itself.
  [[nodiscard]] std::optional<std::string> safe_path() const { return sanitize_entry_path(name); }
};

/// Random-access reader over a ZIP archive held in memory.
///
/// The reader borrows the archive bytes: they must outlive it. Decompression
/// is bounded by the `Limits` given to `open()`: the entry count is checked
/// up front, and the running total of bytes returned by `read()` may not
/// exceed `max_decompressed_bytes`.
///
/// The central directory is authoritative: sizes and checksums are taken
/// from it, so entries written in streaming mode (general-purpose bit 3,
/// zeros in the local header and a data descriptor after the data) read like
/// any other. ZIP64 archives are supported; multi-volume and encrypted ones
/// are not.
class ZipReader {
 public:
  /// Parse the central directory. Fails with Status::Decode for anything that
  /// is not a well-formed (optionally ZIP64) archive, Status::Limit when it
  /// has more than `limits.max_archive_entries` entries.
  [[nodiscard]] static Result<ZipReader> open(ByteView archive, const Limits& limits = {});

  [[nodiscard]] const std::vector<ZipEntry>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  /// Index of the entry with exactly this name, if present (first match).
  [[nodiscard]] std::optional<std::size_t> find(std::string_view name) const noexcept;
  [[nodiscard]] bool contains(std::string_view name) const noexcept { return find(name).has_value(); }

  /// Decompress one entry and verify its CRC-32.
  [[nodiscard]] Result<Bytes> read(std::size_t index);
  /// Decompress the entry with this name; Status::Decode if it is missing.
  [[nodiscard]] Result<Bytes> read(std::string_view name);

  /// Total bytes handed out by `read()` so far.
  [[nodiscard]] std::uint64_t decompressed_total() const noexcept { return decompressed_total_; }

 private:
  ZipReader() = default;
  ByteView archive_{};
  std::vector<ZipEntry> entries_;
  Limits limits_{};
  std::uint64_t decompressed_total_ = 0;
};

/// Per-entry settings for `ZipWriter::add`.
struct ZipEntryOptions {
  int level = default_compression_level;  ///< 0 = store uncompressed, 1..9 = deflate
  std::int64_t mtime = 0;                 ///< Unix seconds (UTC); 0 = 1980-01-01, which keeps output reproducible
  std::uint32_t unix_mode = 0;            ///< POSIX mode bits; 0 = 0644 for files, 0755 for directories
};

/// Builds a ZIP archive in memory. Entries are written in the order added
/// (formats such as EPUB and ODF require `mimetype` first and stored).
/// Output is deterministic for identical inputs. ZIP64 records are emitted
/// only when an archive needs them.
class ZipWriter {
 public:
  ZipWriter() = default;

  /// Add a file. `name` uses '/' separators and must be non-empty, relative,
  /// free of ".." segments and at most 65535 bytes of valid UTF-8.
  /// Data that does not shrink is stored instead of deflated.
  [[nodiscard]] Result<void> add(std::string_view name, ByteView data, const ZipEntryOptions& options = {});
  [[nodiscard]] Result<void> add(std::string_view name, std::string_view text, const ZipEntryOptions& options = {}) {
    return add(name, as_bytes(text), options);
  }
  /// Add an explicit directory entry (a trailing '/' is appended if missing).
  [[nodiscard]] Result<void> add_directory(std::string_view name, const ZipEntryOptions& options = {});

  [[nodiscard]] std::size_t entry_count() const noexcept { return records_.size(); }

  /// Write the central directory and return the archive. The writer is empty
  /// afterwards and can be reused.
  [[nodiscard]] Result<Bytes> finish();

 private:
  struct Record {
    std::string name;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t offset = 0;
    std::uint32_t crc = 0;
    std::uint16_t method = 0;
    std::uint16_t dos_time = 0;
    std::uint16_t dos_date = 0;
    std::uint32_t external_attributes = 0;
  };
  [[nodiscard]] Result<void> add_record(std::string name, ByteView data, const ZipEntryOptions& options,
                                        bool directory);

  ByteWriter out_;
  std::vector<Record> records_;
};

/// Convert between Unix time (UTC) and the DOS date/time pair used by ZIP.
/// Times outside 1980..2107 are clamped.
struct DosDateTime {
  std::uint16_t date = 0x0021;  ///< 1980-01-01
  std::uint16_t time = 0;
};
[[nodiscard]] DosDateTime unix_to_dos_time(std::int64_t unix_seconds) noexcept;
[[nodiscard]] std::int64_t dos_to_unix_time(std::uint16_t dos_date, std::uint16_t dos_time) noexcept;

}  // namespace cvt::util
