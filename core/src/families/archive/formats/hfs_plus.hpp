// Convertible core - archive family: HFS+ / HFSX volume reader (used by the DMG reader).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/archive/archive_common.hpp"
#include "util/bytes.hpp"

namespace cvt::archive::hfs {

/// Random access to a disk or a partition. Reads past the end fail.
class Device {
 public:
  virtual ~Device() = default;
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
  [[nodiscard]] virtual Result<void> read(std::uint64_t offset, std::span<std::byte> out) = 0;
};

/// A window [offset, offset + size) of another device.
class Slice final : public Device {
 public:
  Slice(Device& base, std::uint64_t offset, std::uint64_t size) noexcept : base_(&base), offset_(offset), size_(size) {}
  [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }
  [[nodiscard]] Result<void> read(std::uint64_t offset, std::span<std::byte> out) override {
    if (offset > size_ || out.size() > size_ - offset) return fail(Status::Decode, "read beyond the end of the volume");
    return base_->read(offset_ + offset, out);
  }

 private:
  Device* base_;
  std::uint64_t offset_;
  std::uint64_t size_;
};

/// HFS+ names (UTF-16, decomposed) -> precomposed (NFC) UTF-8; '/' becomes ':'.
[[nodiscard]] std::string name_to_utf8(std::u16string_view name);

/// True when the 512 bytes at offset 1024 of a volume start an HFS+, HFSX or
/// HFS-wrapped HFS+ volume header.
[[nodiscard]] bool is_volume_header(ByteView header) noexcept;

/// Read every file, directory and symlink of the HFS+/HFSX volume on `volume`
/// into `sink`: data forks only (resource forks and extended attributes are
/// ignored; HFS compression via com.apple.decmpfs is expanded), BSD
/// permissions, content-modification times, file hard links. The private
/// metadata folders and the journal files are skipped.
[[nodiscard]] Result<void> read_volume(Device& volume, EntrySink& sink, const Context& ctx);

}  // namespace cvt::archive::hfs
