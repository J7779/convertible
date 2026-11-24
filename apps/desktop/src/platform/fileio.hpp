// Convertible desktop - portable file helpers taking UTF-8 paths.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cvd::fileio {

/// UTF-8 text <-> std::filesystem::path (wide on Windows).
std::filesystem::path to_path(std::string_view utf8);
std::string from_path(const std::filesystem::path& path);

std::optional<std::vector<std::uint8_t>> read_file(const std::string& path, std::size_t max_bytes = SIZE_MAX);
std::optional<std::string> read_text(const std::string& path);
bool write_file(const std::string& path, const std::uint8_t* data, std::size_t size);
bool write_text(const std::string& path, std::string_view text);

bool exists(const std::string& path);
bool is_directory(const std::string& path);
std::optional<std::uint64_t> file_size(const std::string& path);
bool make_directories(const std::string& path);
bool remove_all(const std::string& path);
/// Copies, replacing an existing destination.
bool copy_file(const std::string& from, const std::string& to);
/// Joins with the native separator.
std::string join(const std::string& dir, const std::string& name);
/// A path in `dir` named like `filename` that does not exist yet ("a.txt", "a (2).txt", ...).
std::string unique_path(const std::string& dir, const std::string& filename);

struct DirEntry {
  std::string name;
  std::string path;
  bool is_dir = false;
  std::uint64_t size = 0;
  bool hidden = false;
};
/// Lists a directory (no recursion); errors yield an empty list.
std::vector<DirEntry> list_directory(const std::string& path);

/// Fresh private directory for this session's outputs, under the system temp dir.
std::string make_session_temp_dir();
std::string home_directory();

}  // namespace cvd::fileio
