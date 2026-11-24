// Convertible core - archive family: ZIP reader.
//
// The central directory is authoritative (APPNOTE 4.3): the end record is
// searched from the back, Zip64 end records and extra fields replace the
// 16/32-bit fields, every local header is checked for its signature and
// bounds, and data descriptors (with or without signature, 32- or 64-bit)
// therefore need no parsing. A self-extractor stub or a Chrome extension
// header in front of the archive is tolerated: offsets are corrected by the
// distance between where the central directory says it is and where it
// actually ends up. Entries whose data ranges overlap are refused as a
// "zip bomb" (that is how non-recursive overlapping bombs reach their
// ratios); every decompressed byte is charged to the EntrySink.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>
#include <optional>
#include <vector>

#include "families/archive/compression.hpp"
#include "families/archive/formats/zip_internal.hpp"
#include "util/crc.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::archive::zip {

using util::ByteReader;
using util::ByteWriter;

namespace {

constexpr std::uint32_t u32_max = 0xFFFFFFFFu;
constexpr std::uint16_t u16_max = 0xFFFFu;

struct Entry {
  std::string name;
  std::uint16_t made_by = 0;
  std::uint16_t flags = 0;
  std::uint16_t method = 0;
  std::uint16_t time = 0;
  std::uint16_t date = 0;
  std::uint32_t crc = 0;
  std::uint32_t external = 0;
  std::uint64_t compressed = 0;
  std::uint64_t uncompressed = 0;
  std::uint64_t offset = 0;  // of the local header, in the input
  std::uint64_t data_start = 0;
  ByteView extra{};
};

struct Directory {
  std::uint64_t base = 0;      // added to every stored offset
  std::uint64_t cd_start = 0;  // actual position of the central directory
  std::uint64_t cd_size = 0;
  std::uint64_t count = 0;
  std::string comment;
};

std::uint32_t le32(ByteView v, std::size_t at) noexcept {
  return std::to_integer<std::uint32_t>(v[at]) | (std::to_integer<std::uint32_t>(v[at + 1]) << 8) |
         (std::to_integer<std::uint32_t>(v[at + 2]) << 16) | (std::to_integer<std::uint32_t>(v[at + 3]) << 24);
}

bool has_sig(ByteView data, std::uint64_t at, std::uint32_t sig) noexcept {
  return at <= data.size() && data.size() - at >= 4 && le32(data, static_cast<std::size_t>(at)) == sig;
}

Result<void> multi_volume() {
  return fail(Status::Decode,
              "this is one part of a split (multi-volume) ZIP archive, which is not supported; join the parts into one "
              "file first (for example with 'zip -s 0')");
}

Result<Directory> parse_directory(ByteView data) {
  const std::size_t eocd = find_eocd(data);
  if (eocd == static_cast<std::size_t>(-1)) {
    if (has_sig(data, 0, sig_local)) return fail(Status::Decode, "ZIP central directory not found (the archive is truncated)");
    return fail(Status::Decode, "not a ZIP archive (no end of central directory record)");
  }
  ByteReader r(data.subspan(eocd + 4));
  const std::uint16_t disk = r.u16le();
  const std::uint16_t cd_disk = r.u16le();
  const std::uint16_t disk_entries = r.u16le();
  const std::uint16_t entries = r.u16le();
  const std::uint32_t cd_size = r.u32le();
  const std::uint32_t cd_offset = r.u32le();
  const std::uint16_t comment_size = r.u16le();
  const ByteView comment = r.bytes(comment_size);
  if (!r.ok()) return fail(Status::Decode, "ZIP end record is truncated");

  Directory dir;
  dir.comment = decode_name(as_chars(comment), false, true);
  dir.count = entries;
  dir.cd_size = cd_size;
  std::uint64_t stated_offset = cd_offset;
  std::uint64_t cd_end = eocd;  // the directory ends where the next record begins
  bool split = disk != 0 || cd_disk != 0 || disk_entries != entries;

  if (eocd >= 20 && has_sig(data, eocd - 20, sig_zip64_locator)) {
    ByteReader loc(data.subspan(eocd - 16, 16));
    const std::uint32_t z_disk = loc.u32le();
    const std::uint64_t z_offset = loc.u64le();
    const std::uint32_t total_disks = loc.u32le();
    if (z_disk != 0 || total_disks > 1) CVT_TRY(multi_volume());
    // The Zip64 end record normally sits right before the locator; else trust its stored offset.
    std::uint64_t z_pos = eocd - 20;
    bool found = false;
    if (z_pos >= 56 && has_sig(data, z_pos - 56, sig_zip64_eocd)) {
      z_pos -= 56;
      found = true;
    } else if (has_sig(data, z_offset, sig_zip64_eocd) && z_offset < eocd) {
      z_pos = z_offset;
      found = true;
    }
    if (!found) return fail(Status::Decode, "ZIP64 end of central directory record not found");
    ByteReader z(data.subspan(static_cast<std::size_t>(z_pos) + 4));
    const std::uint64_t record_size = z.u64le();
    (void)z.u16le();  // made by
    (void)z.u16le();  // needed
    const std::uint32_t z_this = z.u32le();
    const std::uint32_t z_cd_disk = z.u32le();
    const std::uint64_t z_disk_entries = z.u64le();
    const std::uint64_t z_entries = z.u64le();
    const std::uint64_t z_cd_size = z.u64le();
    const std::uint64_t z_cd_offset = z.u64le();
    if (!z.ok() || record_size < 44) return fail(Status::Decode, "ZIP64 end of central directory record is damaged");
    split = z_this != 0 || z_cd_disk != 0 || z_disk_entries != z_entries;
    dir.count = z_entries;
    dir.cd_size = z_cd_size;
    stated_offset = z_cd_offset;
    cd_end = z_pos;
  } else if (entries == u16_max || cd_size == u32_max || cd_offset == u32_max) {
    // Saturated fields without Zip64 records: only plausible when they are real values.
    if (cd_offset == u32_max && cd_size == u32_max) return fail(Status::Decode, "ZIP64 records are missing");
  }
  if (split) CVT_TRY(multi_volume());
  if (dir.cd_size > cd_end) return fail(Status::Decode, "ZIP central directory size is out of range");

  if (stated_offset <= cd_end && dir.cd_size <= cd_end - stated_offset &&
      (dir.count == 0 || has_sig(data, stated_offset, sig_central))) {
    dir.cd_start = stated_offset;  // the usual case: offsets are exact
  } else {
    // Something precedes the archive (self-extractor stub, CRX header): shift every offset.
    const std::uint64_t actual = cd_end - dir.cd_size;
    if (actual < stated_offset) return fail(Status::Decode, "ZIP central directory offset is out of range");
    if (dir.count != 0 && !has_sig(data, actual, sig_central)) {
      return fail(Status::Decode, "ZIP central directory not found where the end record says");
    }
    dir.cd_start = actual;
    dir.base = actual - stated_offset;
  }
  // Every central record takes at least 46 bytes.
  if (dir.count > dir.cd_size / 46) return fail(Status::Decode, "ZIP central directory is too small for {} entries", dir.count);
  return dir;
}

Result<std::vector<Entry>> parse_entries(ByteView data, const Directory& dir, const Context& ctx) {
  CVT_TRY(ctx.limits.check_archive_entries(dir.count));
  std::vector<Entry> out;
  out.reserve(static_cast<std::size_t>(dir.count));
  ByteReader r(data.subspan(static_cast<std::size_t>(dir.cd_start), static_cast<std::size_t>(dir.cd_size)));
  for (std::uint64_t i = 0; i < dir.count; ++i) {
    if (r.u32le() != sig_central || !r.ok()) return fail(Status::Decode, "ZIP central directory record {} is damaged", i);
    Entry e;
    e.made_by = r.u16le();
    (void)r.u16le();  // version needed
    e.flags = r.u16le();
    e.method = r.u16le();
    e.time = r.u16le();
    e.date = r.u16le();
    e.crc = r.u32le();
    e.compressed = r.u32le();
    e.uncompressed = r.u32le();
    const std::uint16_t name_size = r.u16le();
    const std::uint16_t extra_size = r.u16le();
    const std::uint16_t comment_size = r.u16le();
    std::uint32_t disk = r.u16le();
    (void)r.u16le();  // internal attributes
    e.external = r.u32le();
    e.offset = r.u32le();
    const std::string_view raw_name = r.chars(name_size);
    e.extra = r.bytes(extra_size);
    (void)r.bytes(comment_size);
    if (!r.ok()) return fail(Status::Decode, "ZIP central directory is truncated");

    if (const ByteView z = find_extra(e.extra, extra_zip64); !z.empty() ||
        e.compressed == u32_max || e.uncompressed == u32_max || e.offset == u32_max || disk == u16_max) {
      ByteReader zr(z);
      if (e.uncompressed == u32_max) e.uncompressed = zr.u64le();
      if (e.compressed == u32_max) e.compressed = zr.u64le();
      if (e.offset == u32_max) e.offset = zr.u64le();
      if (disk == u16_max) disk = zr.u32le();
      if (!zr.ok()) return fail(Status::Decode, "ZIP64 extra field of '{}' is damaged", util::sanitize_utf8(raw_name));
    }
    if (disk != 0) CVT_TRY(multi_volume());

    // Name: UTF-8 flag, else a matching Info-ZIP Unicode Path field, else CP437.
    if (e.flags & flag_utf8) {
      e.name = decode_name(raw_name, true);
    } else if (const ByteView up = find_extra(e.extra, extra_unicode_path);
               up.size() > 5 && up[0] == std::byte{1} && le32(up, 1) == util::crc32(as_bytes(raw_name))) {
      e.name = decode_name(as_chars(up.subspan(5)), true);
    } else {
      e.name = decode_name(raw_name, false, true);
    }

    // Local header: signature, bounds, and where the data begins.
    if (e.offset > dir.cd_start || dir.base > dir.cd_start - e.offset) {
      return fail(Status::Decode, "ZIP local header of '{}' is out of range", e.name);
    }
    e.offset += dir.base;
    if (dir.cd_start - e.offset < 30 || !has_sig(data, e.offset, sig_local)) {
      return fail(Status::Decode, "ZIP local header of '{}' is missing", e.name);
    }
    const auto at = static_cast<std::size_t>(e.offset);
    const std::uint64_t local_name = std::to_integer<std::uint64_t>(data[at + 26]) | (std::to_integer<std::uint64_t>(data[at + 27]) << 8);
    const std::uint64_t local_extra = std::to_integer<std::uint64_t>(data[at + 28]) | (std::to_integer<std::uint64_t>(data[at + 29]) << 8);
    e.data_start = e.offset + 30 + local_name + local_extra;
    if (e.data_start > dir.cd_start || e.compressed > dir.cd_start - e.data_start) {
      return fail(Status::Decode, "ZIP data of '{}' runs past the central directory", e.name);
    }
    out.push_back(std::move(e));
  }

  // Overlapping entries are how non-recursive zip bombs multiply one deflate stream.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  ranges.reserve(out.size());
  for (const Entry& e : out) ranges.emplace_back(e.offset, e.data_start + e.compressed);
  std::ranges::sort(ranges);
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].first < ranges[i - 1].second) {
      return fail(Status::Limit, "ZIP entries share or overlap their data: refusing a likely zip bomb");
    }
  }
  return out;
}

std::optional<std::int64_t> extended_mtime(ByteView extras) {
  const ByteView ut = find_extra(extras, extra_timestamp);
  if (ut.size() >= 5 && (std::to_integer<unsigned>(ut[0]) & 1u) != 0) {
    return static_cast<std::int64_t>(static_cast<std::int32_t>(le32(ut, 1)));
  }
  const ByteView ntfs = find_extra(extras, extra_ntfs);
  std::size_t pos = 4;
  while (ntfs.size() >= 4 && pos + 4 <= ntfs.size()) {
    const unsigned tag = std::to_integer<unsigned>(ntfs[pos]) | (std::to_integer<unsigned>(ntfs[pos + 1]) << 8);
    const std::size_t size = std::to_integer<std::size_t>(ntfs[pos + 2]) | (std::to_integer<std::size_t>(ntfs[pos + 3]) << 8);
    pos += 4;
    if (size > ntfs.size() - pos) break;
    if (tag == 1 && size >= 24) {
      const std::uint64_t ft = le32(ntfs, pos) | (static_cast<std::uint64_t>(le32(ntfs, pos + 4)) << 32);
      if (ft != 0) return static_cast<std::int64_t>(ft / 10000000u) - 11644473600;
    }
    pos += size;
  }
  return std::nullopt;
}

Result<compression::Method> codec_for(std::uint16_t method, const std::string& name) {
  using compression::Method;
  std::optional<Method> m;
  switch (method) {
    case 1: m = Method::Shrink; break;
    case 2:
    case 3:
    case 4:
    case 5: m = Method::Reduce; break;
    case 6: m = Method::Implode; break;
    case 8: m = Method::Deflate; break;
    case 9: m = Method::Deflate64; break;
    case 12: m = Method::Bzip2; break;
    case 14: m = Method::LzmaRaw; break;
    case 20:
    case 93: m = Method::Zstd; break;
    case 95: m = Method::Xz; break;
    case 98: m = Method::Ppmd8; break;
    default: break;
  }
  if (!m) return fail(Status::Decode, "'{}' uses ZIP compression method {}, which is not supported", name, method_description(method));
  if (!compression::can_decode(*m)) {
    return fail(Status::Decode, "'{}' uses ZIP compression method {}, which this build cannot decode", name,
                method_description(method));
  }
  return *m;
}

Result<Bytes> inflate_entry(const Entry& e, std::uint16_t method, ByteView packed, compression::DecodeOptions d) {
  if (method == method_store) {
    if (packed.size() != e.uncompressed) return fail(Status::Decode, "stored ZIP entry '{}' has the wrong size", e.name);
    return Bytes(packed.begin(), packed.end());
  }
  CVT_TRY_ASSIGN(const compression::Method m, codec_for(method, e.name));
  d.output_size = e.uncompressed;
  if (method >= 2 && method <= 5) d.level = method - 1;
  if (method == 6) d.flags = e.flags;
  if (method == 14) {
    // 2-byte LZMA SDK version, 2-byte property size, properties, then raw LZMA data.
    if (packed.size() < 4) return fail(Status::Decode, "LZMA header of '{}' is truncated", e.name);
    const std::size_t props = std::to_integer<std::size_t>(packed[2]) | (std::to_integer<std::size_t>(packed[3]) << 8);
    if (props < 5 || props > packed.size() - 4) return fail(Status::Decode, "LZMA header of '{}' is damaged", e.name);
    d.properties = packed.subspan(4, props);
    packed = packed.subspan(4 + props);
  }
  return compression::decompress(m, packed, d);
}

Result<void> extract(const Entry& e, ByteView input, std::string_view password, EntrySink& sink, const Context& ctx,
                     std::uint64_t done, std::uint64_t total) {
  const std::uint8_t host = static_cast<std::uint8_t>(e.made_by >> 8);
  const std::uint32_t unix_bits = e.external >> 16;
  bool directory = e.name.ends_with('/') || e.name.ends_with('\\');
  bool symlink = false;
  std::uint32_t mode = 0;
  if ((host == host_unix || host == host_osx) && unix_bits != 0) {
    const std::uint32_t type = unix_bits & 0170000u;
    if (type == 0040000u) directory = true;
    if (type == 0120000u) symlink = true;
    mode = unix_bits & 07777u;
  } else if ((e.external & 0x10u) != 0) {
    directory = true;  // MS-DOS directory attribute
  }

  ir::ArchiveEntry entry;
  entry.path = e.name;
  entry.unix_mode = mode;
  entry.mtime = extended_mtime(e.extra).value_or(util::dos_to_unix_time(e.date, e.time));
  if (directory) {
    entry.is_directory = true;
    return sink.add(std::move(entry));
  }

  ByteView packed = input.subspan(static_cast<std::size_t>(e.data_start), static_cast<std::size_t>(e.compressed));
  std::uint16_t method = e.method;
  bool check_crc = true;
  bool zipcrypto = false;
  Bytes decrypted;
  if (e.flags & (flag_strong_encryption | flag_masked_headers)) {
    return fail(Status::Decode, "'{}' uses PKWARE strong encryption, which is not supported", e.name);
  }
  if (e.flags & flag_encrypted) {
    if (password.empty()) return fail(Status::Decode, "password required: '{}' is encrypted", e.name);
    if (method == method_aes) {
      CVT_TRY_ASSIGN(const AesInfo aes, parse_aes_extra(find_extra(e.extra, extra_aes)));
      CVT_TRY_ASSIGN(decrypted, aes_decrypt(packed, password, aes.strength));
      method = aes.method;
      check_crc = aes.vendor_version == 1;
    } else {
      if (packed.size() < zipcrypto_header_size) return fail(Status::Decode, "encrypted ZIP entry '{}' is truncated", e.name);
      decrypted.assign(packed.begin(), packed.end());
      ZipCrypto cipher(password);
      cipher.decrypt(decrypted);
      const auto check = std::to_integer<std::uint8_t>(decrypted[zipcrypto_header_size - 1]);
      const auto expected = static_cast<std::uint8_t>((e.flags & flag_descriptor) ? (e.time >> 8) : (e.crc >> 24));
      if (check != expected) return fail(Status::Decode, "wrong password for '{}'", e.name);
      decrypted.erase(decrypted.begin(), decrypted.begin() + zipcrypto_header_size);
      zipcrypto = true;
    }
    packed = decrypted;
  } else if (method == method_aes) {
    return fail(Status::Decode, "ZIP entry '{}' is marked AES but not encrypted", e.name);
  }

  if (e.uncompressed > sink.budget()) {
    return fail(Status::Limit, "'{}' would decompress to {} bytes, more than the remaining limit", e.name, e.uncompressed);
  }
  compression::DecodeOptions d;
  d.max_output = e.uncompressed;
  const double span = static_cast<double>(std::max<std::uint64_t>(total, 1));
  d.progress = ctx.progress.sub(static_cast<double>(done) / span, static_cast<double>(done + e.compressed) / span);
  auto content = inflate_entry(e, method, packed, d);
  if (!content) {
    if (zipcrypto && content.error().code == Status::Decode) {
      return fail(Status::Decode, "wrong password for '{}' (or damaged data)", e.name);
    }
    if (content.error().code == Status::Limit) {
      return fail(Status::Limit, "'{}' decompresses to more than its declared {} bytes", e.name, e.uncompressed);
    }
    return std::unexpected(std::move(content).error());
  }
  if (content->size() != e.uncompressed) {
    return fail(Status::Decode, "'{}' decompressed to {} bytes instead of {}", e.name, content->size(), e.uncompressed);
  }
  if (check_crc && util::crc32(*content) != e.crc) {
    if (zipcrypto) return fail(Status::Decode, "wrong password for '{}' (CRC mismatch)", e.name);
    return fail(Status::Decode, "CRC mismatch in '{}'", e.name);
  }
  CVT_TRY(sink.charge(content->size()));
  if (symlink) {
    entry.symlink_target = util::sanitize_utf8(as_chars(*content));
    if (entry.symlink_target.empty()) entry.bytes = std::move(*content);
  } else {
    entry.bytes = std::move(*content);
  }
  return sink.add(std::move(entry));
}

// Chrome extension: "Cr24", version 2 (key + signature) or 3 (protobuf header), then the ZIP.
Result<ByteView> skip_crx(ByteView data) {
  if (data.size() < 12 || as_chars(data.first(4)) != "Cr24") return data;
  const std::uint32_t version = le32(data, 4);
  std::uint64_t header = 0;
  if (version == 2) {
    if (data.size() < 16) return fail(Status::Decode, "CRX header is truncated");
    header = 16 + static_cast<std::uint64_t>(le32(data, 8)) + le32(data, 12);
  } else if (version == 3) {
    header = 12 + static_cast<std::uint64_t>(le32(data, 8));
  } else {
    return fail(Status::Decode, "CRX version {} is not supported", version);
  }
  if (header > data.size()) return fail(Status::Decode, "CRX header is truncated");
  return data.subspan(static_cast<std::size_t>(header));
}

}  // namespace

std::size_t find_eocd(ByteView data) noexcept {
  if (data.size() < 22) return static_cast<std::size_t>(-1);
  const std::size_t last = data.size() - 22;
  const std::size_t first = last > 65535 ? last - 65535 : 0;
  std::size_t fallback = static_cast<std::size_t>(-1);
  for (std::size_t pos = last + 1; pos-- > first;) {
    if (data[pos] != std::byte{'P'} || le32(data, pos) != sig_eocd) continue;
    const std::size_t comment = std::to_integer<std::size_t>(data[pos + 20]) | (std::to_integer<std::size_t>(data[pos + 21]) << 8);
    if (pos + 22 + comment == data.size()) return pos;
    if (pos + 22 + comment <= data.size() && fallback == static_cast<std::size_t>(-1)) fallback = pos;
  }
  return fallback;
}

Result<ir::Archive> read_zip(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const ByteView data, skip_crx(ctx.input));
  CVT_TRY_ASSIGN(const Directory dir, parse_directory(data));
  CVT_TRY_ASSIGN(const std::vector<Entry> entries, parse_entries(data, dir, ctx));
  const std::string password = ctx.options.get_string("password", "");
  EntrySink sink(ctx, ctx.input.size());
  std::uint64_t total = 0;
  for (const Entry& e : entries) total += e.compressed;
  std::uint64_t done = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(done) / static_cast<double>(std::max<std::uint64_t>(total, 1))));
    CVT_TRY(extract(entries[i], data, password, sink, ctx, done, total));
    done += entries[i].compressed;
  }
  return sink.finish(dir.comment);
}

}  // namespace cvt::archive::zip
