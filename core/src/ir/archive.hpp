// Convertible core - Archive IR: a set of named file entries.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/bytes.hpp"
#include "util/zip.hpp"

namespace cvt::ir {

/// One file or directory inside an archive.
struct ArchiveEntry {
  /// UTF-8 path relative to the archive root, '/' separated and SANITISED:
  /// never absolute, no drive letters, no "." or ".." segments, no empty
  /// segments, no backslashes or NULs. Directories do NOT end with '/'.
  /// Readers must pass every stored name through `sanitize_archive_path`.
  std::string path;
  /// File content; empty for directories.
  Bytes bytes;
  /// True for explicit directory entries.
  bool is_directory = false;
  /// Modification time in seconds since the Unix epoch (UTC); 0 = unknown.
  std::int64_t mtime = 0;
  /// POSIX permission bits (e.g. 0644, 0755); 0 = unknown, writers then use
  /// 0644 for files and 0755 for directories.
  std::uint32_t unix_mode = 0;
};

/// The logical content of a container format.
struct Archive {
  /// Entries in stored order. Paths are unique in a well-formed archive, but
  /// consumers must tolerate duplicates (last one wins on extraction).
  std::vector<ArchiveEntry> entries;
};

/// Turn a stored entry name into a safe relative path, or nullopt when
/// nothing safe remains (empty name, or one that tries to escape the
/// extraction root via "..").
///
/// Backslashes become '/', leading slashes and drive letters ("C:") are
/// dropped, "." and empty segments are removed, control characters and
/// NULs are stripped, invalid UTF-8 must be repaired by the caller first.
[[nodiscard]] inline std::optional<std::string> sanitize_archive_path(std::string_view raw) {
  return util::sanitize_entry_path(raw);  // the single implementation lives with the ZIP container
}

}  // namespace cvt::ir
