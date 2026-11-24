// Convertible core - archive family: shared machinery of the container formats.
//
// Every archive reader and writer of the family goes through these helpers so
// that they all behave alike:
//
//   * options: the same keys (`compression_level`, `preset`, `method`,
//     `password`, `solid`, `strip_metadata`, `timestamps`) mean the same
//     thing for every format;
//   * privacy: writers never store owner names or ids (owner 0, empty
//     names), normalise permissions when `strip_metadata` is on, and never
//     invent timestamps from the clock (unknown times become `fixed_epoch`);
//   * safety: readers sanitise every path, drop symlinks that point outside
//     the archive and entries reached through a symlink, and account every
//     decompressed byte against the conversion's limits (total size, entry
//     count, compression ratio, nesting depth).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "engine/registry.hpp"
#include "ir/archive.hpp"
#include "util/bytes.hpp"

namespace cvt::archive {

/// The time written for entries whose modification time is unknown, and for
/// every entry under `timestamps = fixed`: 1980-01-01T00:00:00Z (the earliest
/// time ZIP can store, so every format agrees).
inline constexpr std::int64_t fixed_epoch = 315532800;

/// Formats nested inside formats (deb -> data.tar.xz -> tar, DMG -> HFS+,
/// RPM -> cpio) may go at most this deep.
inline constexpr int max_nesting_depth = 4;

/// Decompression-ratio guard: once more than `ratio_guard_floor` bytes have
/// been produced, output may not exceed `max_compression_ratio` times the
/// size of the input. Real data (even long runs of zeros in bzip2 or
/// Zstandard) stays far below this; crafted bombs do not.
inline constexpr std::uint64_t ratio_guard_floor = 64ull * 1024 * 1024;
inline constexpr std::uint64_t max_compression_ratio = 20000;

// --------------------------------------------------------------------------- options

/// `password`: text used to open (readers) or to encrypt (writers).
[[nodiscard]] OptionSpec option_password();
/// `method`: compression method of a writer, one of `choices`.
[[nodiscard]] OptionSpec option_method(std::string default_value, std::vector<std::string> choices,
                                       std::string description = {});
/// `solid`: compress all files as one stream (7z, tar.* are always solid).
[[nodiscard]] OptionSpec option_solid(bool default_value = true);
/// `timestamps`: "preserve" (default) keeps the modification times that came
/// with the entries; "fixed" writes `fixed_epoch` everywhere.
[[nodiscard]] OptionSpec option_timestamps();
/// `encryption`: how a writer encrypts when `password` is set.
[[nodiscard]] OptionSpec option_encryption(std::string default_value, std::vector<std::string> choices);
/// `entry`: path of the entry to extract (Archive -> Blob).
[[nodiscard]] OptionSpec option_entry();

/// compression_level, preset, strip_metadata, timestamps: what every
/// archive writer declares (plus its own extras).
[[nodiscard]] std::vector<OptionSpec> standard_write_options();

/// Everything a writer needs to know, resolved once from its context.
struct WritePolicy {
  /// 0..9. When the caller did not set `compression_level` and the
  /// conversion is format -> same format ("compress"), this is 9.
  int level = 6;
  bool strip_metadata = true;
  bool fixed_timestamps = false;
  bool same_format = false;
  /// Empty = no encryption.
  std::string password;
};
[[nodiscard]] WritePolicy resolve_write_policy(const WriteContext& ctx);

enum class EntryType { File, Directory, Symlink };

/// An entry as a writer should store it.
struct WriteEntry {
  const ir::ArchiveEntry* source = nullptr;
  std::string path;          ///< sanitised, no trailing '/'
  EntryType type = EntryType::File;
  std::int64_t mtime = fixed_epoch;
  std::uint32_t mode = 0644;  ///< permission bits only (& 07777)
  [[nodiscard]] ByteView data() const noexcept { return source != nullptr ? ByteView(source->bytes) : ByteView{}; }
  [[nodiscard]] const std::string& link_target() const noexcept { return source->symlink_target; }
};

struct PrepareOptions {
  bool symlinks = true;          ///< false: symlinks are dropped with a warning
  bool directories = true;       ///< false: directory entries are dropped silently
  bool add_parent_directories = false;  ///< emit missing parent directories before their children
};

/// Validate and normalise the entries of `archive` for writing: re-sanitise
/// paths, apply the policy to modes and times, resolve duplicates (the last
/// one wins, with a warning), and check the entry-count limit.
[[nodiscard]] Result<std::vector<WriteEntry>> prepare_entries(const ir::Archive& archive, const WritePolicy& policy,
                                                              const WriteContext& ctx, const PrepareOptions& options = {});

/// Permission bits written for an entry under the policy.
[[nodiscard]] std::uint32_t normalized_mode(EntryType type, std::uint32_t mode, bool strip_metadata) noexcept;

// --------------------------------------------------------------------------- reading

/// Collects the entries of an archive being read, enforcing limits and path
/// safety. Readers call `budget()`/`charge()` around every decompression and
/// `add()` for every entry, then `finish()`.
class EntrySink {
 public:
  /// `input_size` = size of the (compressed) archive, for the ratio guard.
  EntrySink(const Context& ctx, std::uint64_t input_size);

  /// Bytes that may still be decompressed: pass as `max_output` to decoders.
  [[nodiscard]] std::uint64_t budget() const noexcept;
  /// Account `bytes` of decompressed output; Status::Limit when the total or
  /// the ratio guard is exceeded.
  [[nodiscard]] Result<void> charge(std::uint64_t bytes);

  /// Add an entry whose `path` holds the name as stored (already valid
  /// UTF-8). The name is sanitised; an unusable one is skipped with a
  /// warning. Fails with Status::Limit beyond `max_archive_entries`.
  /// The bytes are NOT charged here (call `charge` when decompressing).
  [[nodiscard]] Result<void> add(ir::ArchiveEntry entry);

  /// Number of entries added so far.
  [[nodiscard]] std::size_t size() const noexcept { return archive_.entries.size(); }

  /// Apply the symlink rules and return the archive.
  [[nodiscard]] ir::Archive finish(std::string comment = {});

 private:
  const Context* ctx_;
  std::uint64_t input_size_;
  std::uint64_t total_ = 0;
  std::size_t skipped_ = 0;
  ir::Archive archive_;
};

/// Decode a stored name: valid UTF-8 (or `utf8` flag set) is kept, anything
/// else is read as IBM code page 437 (`cp437 = true`) or Latin-1.
[[nodiscard]] std::string decode_name(std::string_view raw, bool utf8, bool cp437 = true);
/// Encode a UTF-8 name as code page 437 when every character exists there.
[[nodiscard]] std::optional<std::string> encode_cp437(std::string_view utf8);

/// File name for the single entry of a stream compressed with a format
/// whose extensions are `extensions` ("photo.png.gz" -> "photo.png",
/// "backup.tgz" with mapping tgz->tar -> "backup.tar"), or "data" when nothing is known.
[[nodiscard]] std::string strip_compression_extension(std::string_view filename,
                                                      const std::vector<std::string>& extensions,
                                                      std::string_view replacement = {});

// --------------------------------------------------------------------------- registration

/// An archive format: FormatInfo plus an Archive-kind reader and/or writer.
struct ArchiveFormat {
  FormatInfo info;
  std::function<Result<ir::Archive>(ReadContext& ctx)> read;
  std::function<Result<Bytes>(const ir::Archive& archive, WriteContext& ctx)> write;
  std::vector<OptionSpec> read_options;
  std::vector<OptionSpec> write_options;
  int read_cost = 10;
  int write_cost = 10;
};

/// Register the format and its codec (only the directions provided).
void register_archive_format(Registry& registry, ArchiveFormat format);

}  // namespace cvt::archive
