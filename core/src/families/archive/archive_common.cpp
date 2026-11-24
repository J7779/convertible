// Convertible core - archive family: shared container machinery (see archive_common.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/archive/archive_common.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <format>
#include <limits>
#include <utility>

#include "engine/standard_options.hpp"
#include "util/text.hpp"

namespace cvt::archive {

namespace {

// IBM code page 437, upper half.
constexpr std::array<char16_t, 128> cp437_high = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
    0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
    0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
    0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
    0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
    0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
    0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

// True when a symlink stored at `link_path` with this target stays inside the archive root.
bool symlink_target_is_safe(std::string_view link_path, std::string_view target) {
  if (target.empty()) return false;
  if (target.front() == '/' || target.front() == '\\') return false;
  if (target.size() >= 2 && target[1] == ':' && util::is_ascii_alpha(target[0])) return false;
  if (target.find('\0') != std::string_view::npos) return false;
  // Depth of the directory holding the link.
  int depth = static_cast<int>(std::ranges::count(link_path, '/'));
  for (const std::string_view segment : util::split(target, '/', false)) {
    if (segment == "..") {
      if (--depth < 0) return false;
    } else if (segment != ".") {
      if (segment.find('\\') != std::string_view::npos) return false;
      ++depth;
    }
  }
  return true;
}

}  // namespace

// --------------------------------------------------------------------------- options

OptionSpec option_password() {
  return OptionSpec::text("password", "Password", "",
                          "Password that opens an encrypted archive, or encrypts the archive being written. "
                          "Empty means no encryption.");
}

OptionSpec option_method(std::string default_value, std::vector<std::string> choices, std::string description) {
  if (description.empty()) description = "Compression method used for the files.";
  return OptionSpec::choice("method", "Method", std::move(default_value), std::move(choices), std::move(description))
      .advanced();
}

OptionSpec option_solid(bool default_value) {
  return OptionSpec::boolean("solid", "Solid", default_value,
                             "Compress all files as one stream: smaller, but a single file cannot be extracted "
                             "without decompressing those before it.")
      .advanced();
}

OptionSpec option_timestamps() {
  return OptionSpec::choice("timestamps", "Timestamps", "preserve", {"preserve", "fixed"},
                            "'preserve' keeps the modification times the files came with; 'fixed' writes "
                            "1980-01-01 for every file so that identical content gives identical archives.")
      .advanced();
}

OptionSpec option_encryption(std::string default_value, std::vector<std::string> choices) {
  return OptionSpec::choice("encryption", "Encryption", std::move(default_value), std::move(choices),
                            "Cipher used when a password is given.")
      .advanced();
}

OptionSpec option_entry() {
  return OptionSpec::text("entry", "Entry", "",
                          "Path of the file to take out of the archive; may be left empty when the archive holds "
                          "exactly one file.");
}

std::vector<OptionSpec> standard_write_options() {
  return {options::compression_level(9), options::preset(), options::strip_metadata(), option_timestamps()};
}

WritePolicy resolve_write_policy(const WriteContext& ctx) {
  WritePolicy policy;
  policy.same_format = !ctx.source_format_id.empty() && ctx.source_format_id == ctx.format_id;
  if (ctx.options.was_set(options::key_compression_level) || ctx.options.was_set(options::key_preset) ||
      !policy.same_format) {
    policy.level = options::resolved_compression_level(ctx.options, 9);
  } else {
    policy.level = 9;  // "compress": recompress harder than the default
  }
  policy.strip_metadata = options::resolved_strip_metadata(ctx.options);
  policy.fixed_timestamps = ctx.options.get_enum("timestamps", "preserve", {"preserve", "fixed"}) == "fixed";
  policy.password = ctx.options.get_string("password", "");
  return policy;
}

std::uint32_t normalized_mode(EntryType type, std::uint32_t mode, bool strip_metadata) noexcept {
  mode &= 07777u;
  switch (type) {
    case EntryType::Directory:
      if (strip_metadata || mode == 0) return 0755;
      return mode;
    case EntryType::Symlink:
      return 0777;
    case EntryType::File:
      if (mode == 0) return 0644;
      if (strip_metadata) return (mode & 0111u) != 0 ? 0755 : 0644;
      return mode;
  }
  return 0644;
}

Result<std::vector<WriteEntry>> prepare_entries(const ir::Archive& archive, const WritePolicy& policy,
                                                const WriteContext& ctx, const PrepareOptions& options) {
  CVT_TRY(ctx.limits.check_archive_entries(archive.entries.size()));
  std::vector<WriteEntry> out;
  out.reserve(archive.entries.size());
  std::map<std::string, std::size_t, std::less<>> index;
  std::size_t dropped_links = 0;
  std::size_t duplicates = 0;
  std::size_t bad_names = 0;

  const auto emit = [&](WriteEntry entry) {
    if (const auto found = index.find(entry.path); found != index.end()) {
      // A directory entry next to a file of the same name is not a duplicate worth warning about.
      if (!(entry.type == EntryType::Directory && out[found->second].type == EntryType::Directory)) ++duplicates;
      out[found->second] = std::move(entry);
      return;
    }
    index.emplace(entry.path, out.size());
    out.push_back(std::move(entry));
  };

  for (const ir::ArchiveEntry& source : archive.entries) {
    const auto path = ir::sanitize_archive_path(source.path);
    if (!path) {
      ++bad_names;
      continue;
    }
    WriteEntry entry;
    entry.source = &source;
    entry.path = *path;
    entry.type = source.is_directory ? EntryType::Directory
                                     : (source.is_symlink() ? EntryType::Symlink : EntryType::File);
    if (entry.type == EntryType::Symlink && !options.symlinks) {
      ++dropped_links;
      continue;
    }
    if (entry.type == EntryType::Directory && !options.directories) continue;
    entry.mtime = (policy.fixed_timestamps || source.mtime == 0) ? fixed_epoch : source.mtime;
    entry.mode = normalized_mode(entry.type, source.unix_mode, policy.strip_metadata);
    if (options.add_parent_directories) {
      std::size_t slash = entry.path.find('/');
      while (slash != std::string::npos) {
        std::string parent = entry.path.substr(0, slash);
        if (!index.contains(parent)) {
          WriteEntry dir;
          dir.path = std::move(parent);
          dir.type = EntryType::Directory;
          dir.mtime = entry.mtime;
          dir.mode = 0755;
          emit(std::move(dir));
        }
        slash = entry.path.find('/', slash + 1);
      }
    }
    emit(std::move(entry));
  }
  if (bad_names != 0) ctx.warn(std::format("{} entr{} with unusable names were skipped.", bad_names, bad_names == 1 ? "y" : "ies"));
  if (dropped_links != 0) {
    ctx.warn(std::format("The target format cannot store symbolic links; {} link(s) were left out.", dropped_links));
  }
  if (duplicates != 0) ctx.warn(std::format("{} duplicate path(s): the last entry of each name was kept.", duplicates));
  CVT_TRY(ctx.limits.check_archive_entries(out.size()));
  return out;
}

// --------------------------------------------------------------------------- reading

EntrySink::EntrySink(const Context& ctx, std::uint64_t input_size) : ctx_(&ctx), input_size_(input_size) {}

std::uint64_t EntrySink::budget() const noexcept {
  const std::uint64_t limit = ctx_->limits.max_decompressed_bytes;
  std::uint64_t remaining = limit > total_ ? limit - total_ : 0;
  const std::uint64_t ratio_cap =
      input_size_ > ratio_guard_floor / max_compression_ratio ? input_size_ * max_compression_ratio : ratio_guard_floor;
  const std::uint64_t ratio_remaining = ratio_cap > total_ ? ratio_cap - total_ : 0;
  remaining = std::min(remaining, std::max(ratio_remaining, std::uint64_t{0}));
  return remaining;
}

Result<void> EntrySink::charge(std::uint64_t bytes) {
  if (bytes > std::numeric_limits<std::uint64_t>::max() - total_) {
    return fail(Status::Limit, "decompressed size overflows");
  }
  total_ += bytes;
  CVT_TRY(ctx_->limits.check_decompressed(total_));
  if (total_ > ratio_guard_floor && total_ / std::max<std::uint64_t>(input_size_, 1) > max_compression_ratio) {
    return fail(Status::Limit, "the archive expands more than {}:1 ({} bytes from {}): refusing a likely decompression bomb",
                max_compression_ratio, total_, input_size_);
  }
  return {};
}

Result<void> EntrySink::add(ir::ArchiveEntry entry) {
  CVT_TRY(ctx_->limits.check_archive_entries(archive_.entries.size() + 1));
  const auto path = ir::sanitize_archive_path(entry.path);
  if (!path && entry.is_directory && entry.path.find_first_not_of("./") == std::string::npos) {
    return {};  // the archive root itself ("." or "./"), nothing to extract
  }
  if (!path) {
    if (++skipped_ <= 3) {
      ctx_->warn(std::format("Skipped an entry with an unsafe or empty name ('{}').", util::sanitize_utf8(entry.path)));
    }
    return {};
  }
  entry.path = *path;
  if (entry.is_directory) {
    entry.bytes.clear();
    entry.symlink_target.clear();
  }
  archive_.entries.push_back(std::move(entry));
  return {};
}

ir::Archive EntrySink::finish(std::string comment) {
  // 1. Links that leave the archive root are dropped.
  std::set<std::string, std::less<>> links;
  std::size_t unsafe_links = 0;
  std::erase_if(archive_.entries, [&](const ir::ArchiveEntry& entry) {
    if (!entry.is_symlink()) return false;
    if (!symlink_target_is_safe(entry.path, entry.symlink_target)) {
      ++unsafe_links;
      return true;
    }
    links.insert(entry.path);
    return false;
  });
  // 2. Entries whose path runs through a link would be written outside the
  //    place they claim to be: drop them.
  std::size_t through_links = 0;
  if (!links.empty()) {
    std::erase_if(archive_.entries, [&](const ir::ArchiveEntry& entry) {
      std::size_t slash = entry.path.find('/');
      while (slash != std::string::npos) {
        if (links.contains(std::string_view(entry.path).substr(0, slash))) {
          ++through_links;
          return true;
        }
        slash = entry.path.find('/', slash + 1);
      }
      return false;
    });
  }
  if (unsafe_links != 0) {
    ctx_->warn(std::format("{} symbolic link(s) pointing outside the archive were left out.", unsafe_links));
  }
  if (through_links != 0) {
    ctx_->warn(std::format("{} entr{} stored below a symbolic link were left out.", through_links,
                           through_links == 1 ? "y" : "ies"));
  }
  if (skipped_ > 3) ctx_->warn(std::format("{} entries with unsafe or empty names were skipped in all.", skipped_));
  archive_.comment = std::move(comment);
  return std::move(archive_);
}

std::string decode_name(std::string_view raw, bool utf8, bool cp437) {
  if (utf8 || util::is_valid_utf8(raw)) return util::sanitize_utf8(raw);
  std::string name;
  name.reserve(raw.size() * 2);
  for (const char c : raw) {
    const auto b = static_cast<std::uint8_t>(c);
    if (b < 0x80) {
      name.push_back(c);
    } else {
      util::append_utf8(name, cp437 ? static_cast<char32_t>(cp437_high[b - 0x80]) : static_cast<char32_t>(b));
    }
  }
  return name;
}

std::optional<std::string> encode_cp437(std::string_view utf8) {
  std::string out;
  out.reserve(utf8.size());
  std::size_t pos = 0;
  while (pos < utf8.size()) {
    const util::Utf8Decoded decoded = util::decode_utf8(utf8, pos);
    if (!decoded.ok) return std::nullopt;
    pos += decoded.length;
    if (decoded.code_point < 0x80) {
      out.push_back(static_cast<char>(decoded.code_point));
      continue;
    }
    const auto found = std::ranges::find(cp437_high, static_cast<char16_t>(decoded.code_point));
    if (decoded.code_point > 0xFFFF || found == cp437_high.end()) return std::nullopt;
    out.push_back(static_cast<char>(0x80 + (found - cp437_high.begin())));
  }
  return out;
}

std::string strip_compression_extension(std::string_view filename, const std::vector<std::string>& extensions,
                                        std::string_view replacement) {
  std::string name(filename);
  if (const std::size_t slash = name.find_last_of("/\\"); slash != std::string::npos) name.erase(0, slash + 1);
  for (const std::string& ext : extensions) {
    if (name.size() > ext.size() + 1 && util::iends_with(name, ext) && name[name.size() - ext.size() - 1] == '.') {
      name.resize(name.size() - ext.size() - 1);
      name += replacement;
      return name;
    }
  }
  if (name.empty()) return "data" + std::string(replacement);
  return name + (replacement.empty() ? ".out" : std::string(replacement));
}

// --------------------------------------------------------------------------- registration

void register_archive_format(Registry& registry, ArchiveFormat format) {
  const std::string id = format.info.id;
  registry.add_format(std::move(format.info));
  if (!format.read && !format.write) return;
  Codec codec;
  codec.format_id = id;
  codec.kind = Kind::Archive;
  if (format.read) {
    codec.read = [read = std::move(format.read)](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(ir::Archive archive, read(ctx));
      return AnyIR(std::move(archive));
    };
  }
  if (format.write) {
    codec.write = [write = std::move(format.write)](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      return write(value.get<ir::Archive>(), ctx);
    };
  }
  codec.read_cost = format.read_cost;
  codec.write_cost = format.write_cost;
  codec.read_options = std::move(format.read_options);
  codec.write_options = std::move(format.write_options);
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::archive
