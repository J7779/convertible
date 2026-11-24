// Convertible core - file system helpers with UTF-8 paths on every platform.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::util {

/// Build a path from UTF-8 text. On Windows this converts to UTF-16, so
/// non-ASCII names work regardless of the active code page.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view utf8);
/// The path as UTF-8 text with the platform's native separators.
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);
/// The final path component as UTF-8 ("dir/photo.png" -> "photo.png").
[[nodiscard]] std::string filename_utf8(const std::filesystem::path& path);

/// Read a whole file. Fails with Status::Io if it cannot be read and
/// Status::Limit if it is larger than `max_bytes`.
[[nodiscard]] Result<Bytes> read_file(const std::filesystem::path& path,
                                      std::uint64_t max_bytes = std::numeric_limits<std::uint64_t>::max());
/// Read at most the first `max_bytes` bytes of a file (for format detection
/// without loading large files).
[[nodiscard]] Result<Bytes> read_file_prefix(const std::filesystem::path& path, std::size_t max_bytes);
/// Create or replace a file with exactly `data`. Parent directories are not
/// created. A partially written file is removed on failure.
[[nodiscard]] Result<void> write_file(const std::filesystem::path& path, ByteView data);

/// A freshly created, uniquely named directory inside the system temporary
/// directory, readable by the current user only. The directory and its
/// contents are deleted when the object is destroyed.
class TempDir {
 public:
  [[nodiscard]] static Result<TempDir> create(std::string_view prefix = "convertible");

  TempDir(TempDir&& other) noexcept;
  TempDir& operator=(TempDir&& other) noexcept;
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir();

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  explicit TempDir(std::filesystem::path path) noexcept : path_(std::move(path)) {}
  void remove() noexcept;
  std::filesystem::path path_;
};

}  // namespace cvt::util
