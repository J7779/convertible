// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "platform/fileio.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <system_error>

#include "base/text_util.hpp"

namespace cvd::fileio {

namespace fs = std::filesystem;

fs::path to_path(std::string_view utf8) {
  std::u8string u;
  u.reserve(utf8.size());
  for (const char c : utf8) u.push_back(static_cast<char8_t>(c));
  return fs::path(u);
}

std::string from_path(const fs::path& path) {
  const std::u8string u = path.u8string();
  return std::string(u.begin(), u.end());
}

std::optional<std::vector<std::uint8_t>> read_file(const std::string& path, std::size_t max_bytes) {
  std::ifstream in(to_path(path), std::ios::binary);
  if (!in) return std::nullopt;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) return std::nullopt;
  const std::size_t n = std::min(static_cast<std::size_t>(size), max_bytes);
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> data(n);
  if (n > 0 && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(n))) return std::nullopt;
  return data;
}

std::optional<std::string> read_text(const std::string& path) {
  auto data = read_file(path);
  if (!data) return std::nullopt;
  return std::string(data->begin(), data->end());
}

bool write_file(const std::string& path, const std::uint8_t* data, std::size_t size) {
  std::ofstream out(to_path(path), std::ios::binary | std::ios::trunc);
  if (!out) return false;
  if (size > 0) out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  return static_cast<bool>(out);
}

bool write_text(const std::string& path, std::string_view text) {
  return write_file(path, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

bool exists(const std::string& path) {
  std::error_code ec;
  return fs::exists(to_path(path), ec);
}

bool is_directory(const std::string& path) {
  std::error_code ec;
  return fs::is_directory(to_path(path), ec);
}

std::optional<std::uint64_t> file_size(const std::string& path) {
  std::error_code ec;
  const auto size = fs::file_size(to_path(path), ec);
  if (ec) return std::nullopt;
  return static_cast<std::uint64_t>(size);
}

bool make_directories(const std::string& path) {
  std::error_code ec;
  fs::create_directories(to_path(path), ec);
  return fs::is_directory(to_path(path), ec);
}

bool remove_all(const std::string& path) {
  std::error_code ec;
  fs::remove_all(to_path(path), ec);
  return !ec;
}

bool copy_file(const std::string& from, const std::string& to) {
  std::error_code ec;
  fs::copy_file(to_path(from), to_path(to), fs::copy_options::overwrite_existing, ec);
  return !ec;
}

std::string join(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  return from_path(to_path(dir) / to_path(name));
}

std::string unique_path(const std::string& dir, const std::string& filename) {
  std::string candidate = join(dir, filename);
  if (!exists(candidate)) return candidate;
  // Keep compound extensions such as ".tar.gz" together.
  std::string stem = filename;
  std::string ext;
  const std::size_t dot = filename.find('.', 1);
  if (dot != std::string::npos) {
    stem = filename.substr(0, dot);
    ext = filename.substr(dot);
  }
  for (int i = 2; i < 10000; ++i) {
    candidate = join(dir, stem + " (" + std::to_string(i) + ")" + ext);
    if (!exists(candidate)) return candidate;
  }
  return join(dir, filename);
}

std::vector<DirEntry> list_directory(const std::string& path) {
  std::vector<DirEntry> out;
  std::error_code ec;
  fs::directory_iterator it(to_path(path), fs::directory_options::skip_permission_denied, ec);
  if (ec) return out;
  for (const auto& entry : it) {
    DirEntry e;
    e.path = from_path(entry.path());
    e.name = from_path(entry.path().filename());
    std::error_code ec2;
    e.is_dir = entry.is_directory(ec2);
    if (!e.is_dir) {
      const auto sz = entry.file_size(ec2);
      e.size = ec2 ? 0 : static_cast<std::uint64_t>(sz);
    }
    e.hidden = !e.name.empty() && e.name[0] == '.';
    out.push_back(std::move(e));
    if (out.size() > 20000) break;
  }
  return out;
}

std::string make_session_temp_dir() {
  std::error_code ec;
  fs::path base = fs::temp_directory_path(ec);
  if (ec) base = fs::current_path(ec);
  std::random_device rd;
  for (int attempt = 0; attempt < 32; ++attempt) {
    const auto token = static_cast<unsigned long long>(rd()) ^
                       static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::path dir = base / ("convertible-" + std::to_string(token % 1000000000ULL));
    if (fs::create_directory(dir, ec)) {
      fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
      return from_path(dir);
    }
  }
  return from_path(base);
}

std::string home_directory() {
#if defined(_WIN32)
  if (const wchar_t* profile = _wgetenv(L"USERPROFILE")) return from_path(fs::path(profile));
  return "C:\\";
#else
  if (const char* home = std::getenv("HOME")) return home;
  return "/";
#endif
}

}  // namespace cvd::fileio
