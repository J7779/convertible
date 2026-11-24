// Convertible core - office family: Microsoft "InfoTech Storage" containers.
//
// Compiled HTML Help (.chm, "ITSF") and Microsoft Reader books (.lit,
// "ITOLITLS") are both file systems in one file: a directory of named
// entries, each living in a "section". Section 0 is stored as is; the other
// sections are transformed streams named by ::DataSpace/NameList whose
// ::DataSpace/Storage/<name>/Transform/List says how they were made. LZX is
// the only transform read here (through the archive family's codec, one
// reset interval at a time as its ResetTable describes); a section that is
// DES-encrypted (Microsoft Reader DRM) is refused, never decrypted.
//
// The archive family exposes CHM as an archive of its user files only; the
// readers here also need the system files (#SYSTEM, #WINDOWS, #STRINGS,
// /manifest, /meta ...) and the LIT directory, hence this small reader.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "engine/limits.hpp"
#include "util/bytes.hpp"

namespace cvt::office::its {

struct Entry {
  std::uint64_t section = 0;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
};

/// Read a big-endian base-128 number ("ENCINT") at `pos` (advanced); nullopt
/// when it runs past `end` or is longer than 9 bytes.
[[nodiscard]] std::optional<std::uint64_t> read_encint(ByteView data, std::size_t& pos, std::size_t end);

class Storage {
 public:
  /// Parse a CHM (ITSF/ITSP/PMGL) directory.
  [[nodiscard]] static Result<Storage> open_chm(ByteView data, const Limits& limits);
  /// Parse a LIT (ITOLITLS/CAOL/IFCM/AOLL) directory.
  [[nodiscard]] static Result<Storage> open_lit(ByteView data, const Limits& limits);

  [[nodiscard]] const std::map<std::string, Entry>& entries() const noexcept { return entries_; }
  /// The stored name matching `name` exactly or, failing that, ignoring ASCII case.
  [[nodiscard]] std::optional<std::string> find(std::string_view name) const;
  [[nodiscard]] bool has(std::string_view name) const { return find(name).has_value(); }
  /// The bytes of an entry (looked up as `find` does). Status::Decode when it
  /// is missing, damaged or in a DRM-encrypted section (with a message
  /// saying so), Status::Limit beyond the decompression limit.
  [[nodiscard]] Result<Bytes> read(std::string_view name);
  /// Names of the content sections (index 0 is "Uncompressed").
  [[nodiscard]] const std::vector<std::string>& section_names() const noexcept { return section_names_; }

 private:
  Storage(ByteView data, const Limits& limits, bool lit) : data_(data), limits_(limits), lit_(lit) {}
  Result<void> add_entry(std::string name, Entry entry);
  Result<ByteView> stored(const Entry& entry) const;
  Result<void> read_section_names();
  Result<const Bytes*> section(std::uint64_t index);
  Result<Bytes> lzx(std::string_view storage, ByteView content, ByteView control, std::string_view guid);

  ByteView data_;
  Limits limits_;
  bool lit_ = false;
  std::uint64_t content_offset_ = 0;
  std::map<std::string, Entry> entries_;
  std::map<std::string, std::string> folded_;  // lower-case name -> stored name
  std::vector<std::string> section_names_;
  bool names_read_ = false;
  std::map<std::uint64_t, Bytes> sections_;
  std::uint64_t decompressed_ = 0;
};

}  // namespace cvt::office::its
