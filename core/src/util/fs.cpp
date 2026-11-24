// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/fs.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <span>
#include <system_error>

#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::util {

std::filesystem::path path_from_utf8(std::string_view utf8) {
  const std::string clean = is_valid_utf8(utf8) ? std::string(utf8) : sanitize_utf8(utf8);
  return std::filesystem::path(std::u8string(clean.begin(), clean.end()));
}

std::string path_to_utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

std::string filename_utf8(const std::filesystem::path& path) { return path_to_utf8(path.filename()); }

Result<Bytes> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
  std::error_code ec;
  const std::string shown = path_to_utf8(path);
  if (std::filesystem::is_directory(path, ec)) return fail(Status::Io, "'{}' is a directory", shown);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return fail(Status::Io, "cannot open '{}' for reading", shown);

  Bytes data;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (!ec) {
    if (size > max_bytes) return fail(Status::Limit, "'{}' is {} bytes, above the limit of {} bytes", shown, size, max_bytes);
    data.reserve(static_cast<std::size_t>(size));
  }
  std::array<char, 64 * 1024> chunk{};
  while (stream) {
    stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto got = static_cast<std::size_t>(stream.gcount());
    if (got == 0) break;
    if (data.size() + got > max_bytes) return fail(Status::Limit, "'{}' is above the limit of {} bytes", shown, max_bytes);
    append(data, std::string_view(chunk.data(), got));
  }
  if (stream.bad()) return fail(Status::Io, "error while reading '{}'", shown);
  return data;
}

Result<Bytes> read_file_prefix(const std::filesystem::path& path, std::size_t max_bytes) {
  std::error_code ec;
  const std::string shown = path_to_utf8(path);
  if (std::filesystem::is_directory(path, ec)) return fail(Status::Io, "'{}' is a directory", shown);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return fail(Status::Io, "cannot open '{}' for reading", shown);
  Bytes data(max_bytes);
  stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  data.resize(static_cast<std::size_t>(stream.gcount()));
  if (stream.bad()) return fail(Status::Io, "error while reading '{}'", shown);
  return data;
}

Result<void> write_file(const std::filesystem::path& path, ByteView data) {
  const std::string shown = path_to_utf8(path);
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) return fail(Status::Io, "cannot open '{}' for writing", shown);
  constexpr std::size_t slice = std::size_t{1} << 26;
  for (std::size_t offset = 0; offset < data.size() && stream; offset += slice) {
    const std::size_t count = std::min(slice, data.size() - offset);
    stream.write(reinterpret_cast<const char*>(data.data()) + offset, static_cast<std::streamsize>(count));
  }
  stream.flush();
  if (!stream) {
    stream.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return fail(Status::Io, "error while writing '{}'", shown);
  }
  return {};
}

Result<TempDir> TempDir::create(std::string_view prefix) {
  std::error_code ec;
  const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
  if (ec) return fail(Status::Io, "no temporary directory is available");

  std::random_device entropy;
  for (int attempt = 0; attempt < 16; ++attempt) {
    std::array<std::uint8_t, 12> random{};
    for (auto& byte : random) byte = static_cast<std::uint8_t>(entropy() & 0xFFu);
    std::string name(prefix);
    name.push_back('-');
    name.append(hex_encode(as_bytes(std::span<const std::uint8_t>(random))));
    const std::filesystem::path candidate = base / path_from_utf8(name);
    // create_directory reports false (without error) when the name exists,
    // so a pre-existing directory is never adopted.
    if (std::filesystem::create_directory(candidate, ec) && !ec) {
      std::filesystem::permissions(candidate, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
      return TempDir(candidate);
    }
  }
  return fail(Status::Io, "could not create a temporary directory");
}

TempDir::TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }

TempDir& TempDir::operator=(TempDir&& other) noexcept {
  if (this != &other) {
    remove();
    path_ = std::move(other.path_);
    other.path_.clear();
  }
  return *this;
}

TempDir::~TempDir() { remove(); }

void TempDir::remove() noexcept {
  if (path_.empty()) return;
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  path_.clear();
}

}  // namespace cvt::util
