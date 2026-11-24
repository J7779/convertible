// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/zip.hpp"

#include <algorithm>
#include <array>

#include "util/crc.hpp"
#include "util/text.hpp"

namespace cvt::util {

namespace {

constexpr std::uint32_t local_signature = 0x04034b50;
constexpr std::uint32_t central_signature = 0x02014b50;
constexpr std::uint32_t end_signature = 0x06054b50;
constexpr std::uint32_t end64_signature = 0x06064b50;
constexpr std::uint32_t end64_locator_signature = 0x07064b50;
constexpr std::size_t end_record_size = 22;
constexpr std::size_t central_record_size = 46;
constexpr std::size_t local_record_size = 30;
constexpr std::uint32_t u32_max = 0xFFFFFFFFu;
constexpr std::uint16_t u16_max = 0xFFFFu;
constexpr std::uint16_t flag_encrypted = 0x0001;
constexpr std::uint16_t flag_strong_encryption = 0x0040;
constexpr std::uint16_t flag_utf8 = 0x0800;
constexpr std::uint16_t made_by_unix = (3u << 8) | 63u;  // host system Unix, spec version 6.3

// IBM code page 437, upper half: the historical encoding of ZIP entry names.
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

std::string decode_name(std::string_view raw, bool utf8_flag) {
  std::string name;
  if (utf8_flag || is_valid_utf8(raw)) {
    name = sanitize_utf8(raw);
  } else {
    name.reserve(raw.size() * 2);
    for (const char c : raw) {
      const auto b = static_cast<std::uint8_t>(c);
      append_utf8(name, b < 0x80 ? static_cast<char32_t>(b) : static_cast<char32_t>(cp437_high[b - 0x80]));
    }
  }
  std::ranges::replace(name, '\\', '/');
  return name;
}

// Days since 1970-01-01 for a proleptic Gregorian date, and back.
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct Civil {
  std::int64_t year;
  unsigned month;
  unsigned day;
};
constexpr Civil civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {y + (m <= 2), m, d};
}

constexpr std::int64_t dos_epoch = 315532800;        // 1980-01-01T00:00:00Z
constexpr std::int64_t dos_last = 4354819198;        // 2107-12-31T23:59:58Z

bool valid_entry_name(std::string_view name) {
  if (name.empty() || name.size() > u16_max || name.front() == '/' || !is_valid_utf8(name)) return false;
  if (name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos) return false;
  if (name.size() >= 2 && is_ascii_alpha(name[0]) && name[1] == ':') return false;
  for (const std::string_view segment : split(name, '/')) {
    if (segment == "..") return false;
  }
  return true;
}

}  // namespace

DosDateTime unix_to_dos_time(std::int64_t unix_seconds) noexcept {
  const std::int64_t t = std::clamp(unix_seconds, dos_epoch, dos_last);
  const std::int64_t days = t / 86400;
  const auto seconds = static_cast<unsigned>(t % 86400);
  const Civil civil = civil_from_days(days);
  DosDateTime out;
  out.date = static_cast<std::uint16_t>(((static_cast<unsigned>(civil.year) - 1980u) << 9) | (civil.month << 5) | civil.day);
  out.time = static_cast<std::uint16_t>(((seconds / 3600) << 11) | (((seconds / 60) % 60) << 5) | ((seconds % 60) / 2));
  return out;
}

std::int64_t dos_to_unix_time(std::uint16_t dos_date, std::uint16_t dos_time) noexcept {
  const std::int64_t year = 1980 + ((dos_date >> 9) & 0x7F);
  const unsigned month = std::clamp<unsigned>((dos_date >> 5) & 0x0F, 1, 12);
  const unsigned day = std::clamp<unsigned>(dos_date & 0x1F, 1, 31);
  const unsigned hour = std::min<unsigned>((dos_time >> 11) & 0x1F, 23);
  const unsigned minute = std::min<unsigned>((dos_time >> 5) & 0x3F, 59);
  const unsigned second = std::min<unsigned>((dos_time & 0x1F) * 2, 59);
  return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

std::optional<std::string> sanitize_entry_path(std::string_view raw) {
  std::string cleaned;
  cleaned.reserve(raw.size());
  for (const char c : raw) {
    if (c == '\\') {
      cleaned.push_back('/');
    } else if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) {
      cleaned.push_back(c);
    }
  }
  std::string_view rest = cleaned;
  std::string out;
  out.reserve(rest.size());
  while (!rest.empty()) {
    const std::size_t slash = rest.find('/');
    std::string_view segment = rest.substr(0, slash);
    rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
    // A "C:" prefix would select a drive (or, mid-path, an alternate data
    // stream) on Windows, wherever in the name it appears.
    while (segment.size() >= 2 && segment[1] == ':' && is_ascii_alpha(segment[0])) segment.remove_prefix(2);
    if (segment.empty() || segment == ".") continue;
    if (segment == "..") return std::nullopt;
    if (!out.empty()) out.push_back('/');
    out.append(segment);
  }
  if (out.empty()) return std::nullopt;
  return out;
}

// --------------------------------------------------------------------------- reader

Result<ZipReader> ZipReader::open(ByteView archive, const Limits& limits) {
  if (archive.size() < end_record_size) return fail(Status::Decode, "not a ZIP archive (too small)");

  // The end-of-central-directory record sits at the very end, followed only
  // by an optional comment of at most 65535 bytes.
  const std::size_t earliest =
      archive.size() > end_record_size + u16_max ? archive.size() - end_record_size - u16_max : 0;
  std::optional<std::size_t> end_position;
  for (std::size_t pos = archive.size() - end_record_size + 1; pos-- > earliest;) {
    ByteReader probe(archive.subspan(pos));
    if (probe.u32le() != end_signature) continue;
    probe.skip(16);
    const std::uint16_t comment_length = probe.u16le();
    if (probe.ok() && pos + end_record_size + comment_length <= archive.size()) {
      end_position = pos;
      break;
    }
  }
  if (!end_position) return fail(Status::Decode, "not a ZIP archive (end of central directory not found)");

  ByteReader end(archive.subspan(*end_position));
  end.skip(4);
  const std::uint16_t disk_number = end.u16le();
  const std::uint16_t directory_disk = end.u16le();
  end.skip(2);
  std::uint64_t entry_count = end.u16le();
  std::uint64_t directory_size = end.u32le();
  std::uint64_t directory_offset = end.u32le();
  if (disk_number != 0 || directory_disk != 0) {
    if (disk_number != u16_max) return fail(Status::Decode, "multi-volume ZIP archives are not supported");
  }

  bool zip64 = false;
  if (*end_position >= 20) {
    ByteReader locator(archive.subspan(*end_position - 20, 20));
    if (locator.u32le() == end64_locator_signature) {
      locator.skip(4);
      const std::uint64_t end64_offset = locator.u64le();
      if (end64_offset <= archive.size() && archive.size() - end64_offset >= 56) {
        ByteReader end64(archive.subspan(static_cast<std::size_t>(end64_offset)));
        if (end64.u32le() == end64_signature) {
          end64.skip(8 + 2 + 2 + 4 + 4 + 8);
          entry_count = end64.u64le();
          directory_size = end64.u64le();
          directory_offset = end64.u64le();
          zip64 = end64.ok();
        }
      }
      if (!zip64) return fail(Status::Decode, "corrupt ZIP64 end of central directory");
    }
  }

  if (directory_size > *end_position) return fail(Status::Decode, "corrupt ZIP central directory size");
  // Archives with data prepended (self-extractors) keep offsets relative to
  // the original start; recover the shift from where the directory must be.
  std::uint64_t shift = 0;
  if (!zip64) {
    const std::uint64_t actual_offset = *end_position - directory_size;
    if (directory_offset > actual_offset) return fail(Status::Decode, "corrupt ZIP central directory offset");
    shift = actual_offset - directory_offset;
    directory_offset = actual_offset;
  }
  if (directory_offset > archive.size() || directory_size > archive.size() - directory_offset) {
    return fail(Status::Decode, "ZIP central directory lies outside the file");
  }
  if (entry_count > directory_size / central_record_size) {
    return fail(Status::Decode, "ZIP entry count does not match the central directory size");
  }
  CVT_TRY(limits.check_archive_entries(entry_count));

  ZipReader zip;
  zip.archive_ = archive;
  zip.limits_ = limits;
  zip.entries_.reserve(static_cast<std::size_t>(entry_count));

  ByteReader dir(archive.subspan(static_cast<std::size_t>(directory_offset), static_cast<std::size_t>(directory_size)));
  for (std::uint64_t i = 0; i < entry_count; ++i) {
    if (dir.u32le() != central_signature) return fail(Status::Decode, "corrupt ZIP central directory entry {}", i);
    const std::uint16_t made_by = dir.u16le();
    dir.skip(2);
    const std::uint16_t flags = dir.u16le();
    ZipEntry entry;
    entry.method = dir.u16le();
    const std::uint16_t dos_time = dir.u16le();
    const std::uint16_t dos_date = dir.u16le();
    entry.crc32 = dir.u32le();
    entry.compressed_size = dir.u32le();
    entry.uncompressed_size = dir.u32le();
    const std::uint16_t name_length = dir.u16le();
    const std::uint16_t extra_length = dir.u16le();
    const std::uint16_t comment_length = dir.u16le();
    dir.skip(2 + 2);
    const std::uint32_t external_attributes = dir.u32le();
    entry.local_header_offset = dir.u32le();
    const std::string_view raw_name = dir.chars(name_length);
    ByteReader extra = dir.sub(extra_length);
    dir.skip(comment_length);
    if (!dir.ok()) return fail(Status::Decode, "truncated ZIP central directory");

    entry.mtime = dos_to_unix_time(dos_date, dos_time);
    while (extra.remaining() >= 4) {
      const std::uint16_t id = extra.u16le();
      const std::uint16_t size = extra.u16le();
      ByteReader field = extra.sub(size);
      if (!extra.ok()) break;
      if (id == 0x0001) {  // ZIP64 extended information
        if (entry.uncompressed_size == u32_max) entry.uncompressed_size = field.u64le();
        if (entry.compressed_size == u32_max) entry.compressed_size = field.u64le();
        if (entry.local_header_offset == u32_max) entry.local_header_offset = field.u64le();
        if (!field.ok()) return fail(Status::Decode, "corrupt ZIP64 extra field");
      } else if (id == 0x5455 && size >= 5) {  // extended timestamp (UTC)
        if (field.u8() & 0x01) entry.mtime = field.i32le();
      }
    }

    entry.name = decode_name(raw_name, (flags & flag_utf8) != 0);
    entry.encrypted = (flags & (flag_encrypted | flag_strong_encryption)) != 0;
    entry.is_directory = entry.name.ends_with('/') || (external_attributes & 0x10) != 0;
    if ((made_by >> 8) == 3) {  // created on Unix: the high word holds st_mode
      entry.unix_mode = external_attributes >> 16;
      if ((entry.unix_mode & 0170000u) == 0040000u) entry.is_directory = true;
    }
    entry.local_header_offset += shift;
    zip.entries_.push_back(std::move(entry));
  }
  return zip;
}

std::optional<std::size_t> ZipReader::find(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].name == name) return i;
  }
  return std::nullopt;
}

Result<Bytes> ZipReader::read(std::string_view name) {
  const auto index = find(name);
  if (!index) return fail(Status::Decode, "ZIP archive has no entry named '{}'", name);
  return read(*index);
}

Result<Bytes> ZipReader::read(std::size_t index) {
  if (index >= entries_.size()) return fail(Status::InvalidArgument, "ZIP entry index {} out of range", index);
  const ZipEntry& entry = entries_[index];
  if (entry.encrypted) return fail(Status::Decode, "ZIP entry '{}' is encrypted", entry.name);
  if (entry.method != 0 && entry.method != 8) {
    return fail(Status::Decode, "ZIP entry '{}' uses unsupported compression method {}", entry.name, entry.method);
  }
  if (entry.uncompressed_size > limits_.max_decompressed_bytes ||
      decompressed_total_ > limits_.max_decompressed_bytes - entry.uncompressed_size) {
    return fail(Status::Limit, "ZIP contents exceed the decompression limit of {} bytes", limits_.max_decompressed_bytes);
  }
  if (entry.local_header_offset > archive_.size() || archive_.size() - entry.local_header_offset < local_record_size) {
    return fail(Status::Decode, "ZIP entry '{}' points outside the file", entry.name);
  }
  ByteReader local(archive_.subspan(static_cast<std::size_t>(entry.local_header_offset)));
  if (local.u32le() != local_signature) return fail(Status::Decode, "ZIP entry '{}' has a corrupt local header", entry.name);
  local.skip(22);
  const std::uint16_t name_length = local.u16le();
  const std::uint16_t extra_length = local.u16le();
  local.skip(static_cast<std::size_t>(name_length) + extra_length);
  if (!local.ok() || entry.compressed_size > local.remaining()) {
    return fail(Status::Decode, "ZIP entry '{}' is truncated", entry.name);
  }
  const ByteView stored = local.bytes(static_cast<std::size_t>(entry.compressed_size));

  Bytes data;
  if (entry.method == 0) {
    if (entry.compressed_size != entry.uncompressed_size) {
      return fail(Status::Decode, "ZIP entry '{}' has inconsistent sizes", entry.name);
    }
    data.assign(stored.begin(), stored.end());
  } else {
    auto inflated = inflate_raw(stored, entry.uncompressed_size);
    if (!inflated) {
      // Output larger than declared means the directory lied, not that a
      // user-configurable limit was hit.
      return fail(Status::Decode, "ZIP entry '{}' is corrupt: {}", entry.name, inflated.error().message);
    }
    data = std::move(*inflated);
    if (data.size() != entry.uncompressed_size) {
      return fail(Status::Decode, "ZIP entry '{}' has inconsistent sizes", entry.name);
    }
  }
  if (crc32(data) != entry.crc32) return fail(Status::Decode, "ZIP entry '{}' failed its checksum", entry.name);
  decompressed_total_ += data.size();
  return data;
}

// --------------------------------------------------------------------------- writer

Result<void> ZipWriter::add(std::string_view name, ByteView data, const ZipEntryOptions& options) {
  if (!valid_entry_name(name) || name.ends_with('/')) {
    return fail(Status::InvalidArgument, "invalid ZIP entry name '{}'", sanitize_utf8(name));
  }
  return add_record(std::string(name), data, options, false);
}

Result<void> ZipWriter::add_directory(std::string_view name, const ZipEntryOptions& options) {
  std::string full(name);
  if (!full.ends_with('/')) full.push_back('/');
  if (!valid_entry_name(full)) {
    return fail(Status::InvalidArgument, "invalid ZIP directory name '{}'", sanitize_utf8(name));
  }
  return add_record(std::move(full), {}, options, true);
}

Result<void> ZipWriter::add_record(std::string name, ByteView data, const ZipEntryOptions& options, bool directory) {
  Record record;
  record.uncompressed_size = data.size();
  record.crc = crc32(data);
  record.offset = out_.size();

  Bytes compressed;
  ByteView payload = data;
  if (!directory && options.level > 0 && !data.empty()) {
    auto deflated = deflate_raw(data, options.level);
    if (!deflated) return std::unexpected(std::move(deflated).error());
    if (deflated->size() < data.size()) {
      compressed = std::move(*deflated);
      payload = compressed;
      record.method = 8;
    }
  }
  record.compressed_size = payload.size();

  const DosDateTime stamp = unix_to_dos_time(options.mtime);
  record.dos_time = stamp.time;
  record.dos_date = stamp.date;
  const std::uint32_t mode = options.unix_mode != 0
                                 ? ((options.unix_mode & 0170000u) != 0 ? options.unix_mode
                                                                        : options.unix_mode | (directory ? 0040000u : 0100000u))
                                 : (directory ? 0040755u : 0100644u);
  record.external_attributes = (mode << 16) | (directory ? 0x10u : 0u);

  const bool is_ascii = std::ranges::all_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
  const bool zip64 = record.uncompressed_size >= u32_max || record.compressed_size >= u32_max;
  out_.u32le(local_signature);
  out_.u16le(static_cast<std::uint16_t>(zip64 ? 45 : 20));
  out_.u16le(static_cast<std::uint16_t>(is_ascii ? 0 : flag_utf8));
  out_.u16le(record.method);
  out_.u16le(record.dos_time).u16le(record.dos_date);
  out_.u32le(record.crc);
  out_.u32le(zip64 ? u32_max : static_cast<std::uint32_t>(record.compressed_size));
  out_.u32le(zip64 ? u32_max : static_cast<std::uint32_t>(record.uncompressed_size));
  out_.u16le(static_cast<std::uint16_t>(name.size()));
  out_.u16le(static_cast<std::uint16_t>(zip64 ? 20 : 0));
  out_.text(name);
  if (zip64) out_.u16le(0x0001).u16le(16).u64le(record.uncompressed_size).u64le(record.compressed_size);
  out_.bytes(payload);

  record.name = std::move(name);
  records_.push_back(std::move(record));
  return {};
}

Result<Bytes> ZipWriter::finish() {
  const std::uint64_t directory_offset = out_.size();
  for (const Record& record : records_) {
    const bool big_sizes = record.uncompressed_size >= u32_max || record.compressed_size >= u32_max;
    const bool big_offset = record.offset >= u32_max;
    const std::uint16_t extra_size =
        static_cast<std::uint16_t>((big_sizes ? 16 : 0) + (big_offset ? 8 : 0));
    const bool is_ascii =
        std::ranges::all_of(record.name, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    out_.u32le(central_signature);
    out_.u16le(made_by_unix);
    out_.u16le(static_cast<std::uint16_t>(extra_size > 0 ? 45 : 20));
    out_.u16le(static_cast<std::uint16_t>(is_ascii ? 0 : flag_utf8));
    out_.u16le(record.method);
    out_.u16le(record.dos_time).u16le(record.dos_date);
    out_.u32le(record.crc);
    out_.u32le(big_sizes ? u32_max : static_cast<std::uint32_t>(record.compressed_size));
    out_.u32le(big_sizes ? u32_max : static_cast<std::uint32_t>(record.uncompressed_size));
    out_.u16le(static_cast<std::uint16_t>(record.name.size()));
    out_.u16le(static_cast<std::uint16_t>(extra_size > 0 ? extra_size + 4 : 0));
    out_.u16le(0);  // comment length
    out_.u16le(0);  // disk number
    out_.u16le(0);  // internal attributes
    out_.u32le(record.external_attributes);
    out_.u32le(big_offset ? u32_max : static_cast<std::uint32_t>(record.offset));
    out_.text(record.name);
    if (extra_size > 0) {
      out_.u16le(0x0001).u16le(extra_size);
      if (big_sizes) out_.u64le(record.uncompressed_size).u64le(record.compressed_size);
      if (big_offset) out_.u64le(record.offset);
    }
  }
  const std::uint64_t directory_size = out_.size() - directory_offset;
  const std::uint64_t count = records_.size();

  if (count >= u16_max || directory_size >= u32_max || directory_offset >= u32_max) {
    const std::uint64_t end64_offset = out_.size();
    out_.u32le(end64_signature);
    out_.u64le(44);  // size of the remainder of this record
    out_.u16le(made_by_unix).u16le(45);
    out_.u32le(0).u32le(0);
    out_.u64le(count).u64le(count);
    out_.u64le(directory_size).u64le(directory_offset);
    out_.u32le(end64_locator_signature);
    out_.u32le(0);
    out_.u64le(end64_offset);
    out_.u32le(1);
  }
  out_.u32le(end_signature);
  out_.u16le(0).u16le(0);
  out_.u16le(static_cast<std::uint16_t>(std::min<std::uint64_t>(count, u16_max)));
  out_.u16le(static_cast<std::uint16_t>(std::min<std::uint64_t>(count, u16_max)));
  out_.u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(directory_size, u32_max)));
  out_.u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(directory_offset, u32_max)));
  out_.u16le(0);  // comment length
  records_.clear();
  return out_.take();
}

}  // namespace cvt::util
