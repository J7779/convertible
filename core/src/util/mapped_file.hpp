// Convertible core - read-only view of a file's contents without copying it.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::util {

/// The bytes of a file, memory-mapped where the platform allows (POSIX
/// `mmap`, Windows `MapViewOfFile`) so that large inputs are paged in on
/// demand instead of being read into RAM. Where mapping is unavailable
/// (Emscripten) or fails, the file is read into a buffer instead; callers
/// cannot tell the difference.
///
/// The view stays valid until the object is destroyed or moved from.
/// The file must not be truncated by anyone while it is mapped; release the
/// mapping before overwriting the same path.
class MappedFile {
 public:
  /// Open `path` for reading. Fails with Status::Io when it cannot be opened
  /// and Status::Limit when it is larger than `max_bytes`.
  [[nodiscard]] static Result<MappedFile> open(const std::filesystem::path& path,
                                               std::uint64_t max_bytes = std::numeric_limits<std::uint64_t>::max());

  MappedFile() = default;
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  ~MappedFile();

  /// The whole file.
  [[nodiscard]] ByteView bytes() const noexcept { return view_; }
  [[nodiscard]] std::size_t size() const noexcept { return view_.size(); }
  /// True when the contents are mapped rather than held in a buffer.
  [[nodiscard]] bool is_mapped() const noexcept { return mapping_ != nullptr; }

  /// Release the mapping or buffer now; `bytes()` becomes empty.
  void close() noexcept;

 private:
  ByteView view_{};
  void* mapping_ = nullptr;      // base address of the mapping, when mapped
  std::size_t mapping_size_ = 0;
  Bytes buffer_;                 // contents, when not mapped
};

}  // namespace cvt::util
