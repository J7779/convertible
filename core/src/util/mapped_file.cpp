// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/mapped_file.hpp"

#include <system_error>
#include <utility>

#include "util/fs.hpp"

#if defined(_WIN32)
#  include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  define CVT_HAVE_POSIX_MMAP 1
#endif

namespace cvt::util {

namespace {

// Platform mapping primitives: return the base address or nullptr.
#if defined(_WIN32)

void* map_whole_file(const std::filesystem::path& path, std::size_t size) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return nullptr;
  void* base = nullptr;
  LARGE_INTEGER actual{};
  if (GetFileSizeEx(file, &actual) && static_cast<std::uint64_t>(actual.QuadPart) == size) {
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping != nullptr) {
      base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
      CloseHandle(mapping);  // the view keeps the mapping object alive
    }
  }
  CloseHandle(file);
  return base;
}

void unmap(void* base, std::size_t /*size*/) noexcept { UnmapViewOfFile(base); }

#elif defined(CVT_HAVE_POSIX_MMAP)

void* map_whole_file(const std::filesystem::path& path, std::size_t size) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  void* base = nullptr;
  struct stat info {};
  if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && static_cast<std::uint64_t>(info.st_size) == size) {
    void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped != MAP_FAILED) base = mapped;
  }
  ::close(fd);  // the mapping stays valid after the descriptor is closed
  return base;
}

void unmap(void* base, std::size_t size) noexcept { ::munmap(base, size); }

#else

void* map_whole_file(const std::filesystem::path& /*path*/, std::size_t /*size*/) { return nullptr; }
void unmap(void* /*base*/, std::size_t /*size*/) noexcept {}

#endif

}  // namespace

Result<MappedFile> MappedFile::open(const std::filesystem::path& path, std::uint64_t max_bytes) {
  const std::string shown = path_to_utf8(path);
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) return fail(Status::Io, "'{}' is a directory", shown);
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) return fail(Status::Io, "cannot open '{}' for reading", shown);
  if (size > max_bytes) return fail(Status::Limit, "'{}' is {} bytes, above the limit of {} bytes", shown, size, max_bytes);

  MappedFile file;
  if (size > 0 && size <= std::numeric_limits<std::size_t>::max()) {
    if (void* base = map_whole_file(path, static_cast<std::size_t>(size))) {
      file.mapping_ = base;
      file.mapping_size_ = static_cast<std::size_t>(size);
      file.view_ = ByteView(static_cast<const std::byte*>(base), file.mapping_size_);
      return file;
    }
  }
  // Empty files cannot be mapped, some file systems refuse mappings, and
  // some platforms have none: read the contents instead.
  auto data = read_file(path, max_bytes);
  if (!data) return std::unexpected(std::move(data).error());
  file.buffer_ = std::move(*data);
  file.view_ = file.buffer_;
  return file;
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : mapping_(std::exchange(other.mapping_, nullptr)),
      mapping_size_(std::exchange(other.mapping_size_, 0)),
      buffer_(std::move(other.buffer_)) {
  view_ = mapping_ != nullptr ? ByteView(static_cast<const std::byte*>(mapping_), mapping_size_) : ByteView(buffer_);
  other.buffer_.clear();
  other.view_ = {};
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    close();
    mapping_ = std::exchange(other.mapping_, nullptr);
    mapping_size_ = std::exchange(other.mapping_size_, 0);
    buffer_ = std::move(other.buffer_);
    view_ = mapping_ != nullptr ? ByteView(static_cast<const std::byte*>(mapping_), mapping_size_) : ByteView(buffer_);
    other.buffer_.clear();
    other.view_ = {};
  }
  return *this;
}

MappedFile::~MappedFile() { close(); }

void MappedFile::close() noexcept {
  if (mapping_ != nullptr) unmap(mapping_, mapping_size_);
  mapping_ = nullptr;
  mapping_size_ = 0;
  buffer_.clear();
  buffer_.shrink_to_fit();
  view_ = {};
}

}  // namespace cvt::util
