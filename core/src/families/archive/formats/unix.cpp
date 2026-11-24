// Convertible core - archive family: the Unix archive formats (cpio, ar, deb, rpm, shar, warc, xar).
//
// This file holds their registration and small shared helpers; each format
// lives in its own unix_*.cpp file:
//   cpio  read: newc, crc (checksums verified), odc, old binary (both byte
//         orders), hard links, symlinks; write: newc.
//   ar    read: GNU (symbol table, '//' long names) and BSD ('#1/len');
//         write: GNU style. Thin archives are refused.
//   deb   read: data.tar.* entries plus the control files under DEBIAN/
//         (the layout `dpkg-deb -R` produces).
//   rpm   read: the cpio payload (gzip, bzip2, xz, lzma, zstd or none).
//   shar  write: a POSIX sh script that recreates the files.
//   warc  read: response and resource records of WARC 1.0 / 1.1 files, plain
//         or gzip-compressed per record.
//   xar   read: xar archives, macOS flat packages (.pkg) and .xip files.
// Nesting is fixed and shallow (deb -> compressed tar, rpm -> compressed
// cpio): no reader here recurses, so `max_nesting_depth` is never approached.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <cstring>

#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/formats.hpp"
#include "families/archive/formats/unix_internal.hpp"
#include "util/text.hpp"

namespace cvt::archive {

namespace unixfmt {

namespace {

// Days since 1970-01-01 of a proleptic Gregorian date.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9u)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::optional<unsigned> digits(std::string_view text, std::size_t at, std::size_t count) noexcept {
  if (at + count > text.size()) return std::nullopt;
  unsigned v = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[at + i];
    if (!util::is_ascii_digit(c)) return std::nullopt;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  return v;
}

}  // namespace

std::optional<std::int64_t> parse_iso_time(std::string_view text) noexcept {
  text = util::trim(text);
  const auto year = digits(text, 0, 4);
  const auto month = digits(text, 5, 2);
  const auto day = digits(text, 8, 2);
  if (!year || !month || !day || text[4] != '-' || text[7] != '-') return std::nullopt;
  if (*month < 1 || *month > 12 || *day < 1 || *day > 31) return std::nullopt;
  std::int64_t seconds = days_from_civil(*year, *month, *day) * 86400;
  if (text.size() > 10) {
    if (text[10] != 'T' && text[10] != 't' && text[10] != ' ') return std::nullopt;
    const auto hour = digits(text, 11, 2);
    const auto minute = digits(text, 14, 2);
    if (!hour || !minute || text[13] != ':' || *hour > 23 || *minute > 59) return std::nullopt;
    seconds += static_cast<std::int64_t>(*hour) * 3600 + static_cast<std::int64_t>(*minute) * 60;
    std::size_t pos = 16;
    if (pos < text.size() && text[pos] == ':') {
      const auto second = digits(text, 17, 2);
      if (!second || *second > 60) return std::nullopt;
      seconds += *second;
      pos = 19;
      if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && util::is_ascii_digit(text[pos])) ++pos;
      }
    }
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
      const auto oh = digits(text, pos + 1, 2);
      const std::size_t mm = pos + 3 < text.size() && text[pos + 3] == ':' ? pos + 4 : pos + 3;
      const auto om = digits(text, mm, 2);
      if (!oh) return std::nullopt;
      const std::int64_t offset = static_cast<std::int64_t>(*oh) * 3600 + static_cast<std::int64_t>(om.value_or(0)) * 60;
      seconds += text[pos] == '+' ? -offset : offset;
    }
  }
  return seconds;
}

std::optional<std::uint64_t> parse_field(std::string_view field, int base) noexcept {
  std::size_t i = 0;
  while (i < field.size() && field[i] == ' ') ++i;
  std::uint64_t v = 0;
  bool any = false;
  for (; i < field.size(); ++i) {
    const char c = field[i];
    int digit = -1;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    }
    if (digit < 0 || digit >= base) break;
    if (v > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(digit)) / static_cast<std::uint64_t>(base)) {
      return std::nullopt;
    }
    v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
    any = true;
  }
  for (; i < field.size(); ++i) {
    if (field[i] != ' ' && field[i] != '\0') return std::nullopt;
  }
  if (!any) return std::nullopt;
  return v;
}

}  // namespace unixfmt

namespace {

bool starts_with(ByteView data, std::string_view magic) {
  return data.size() >= magic.size() && as_chars(data.first(magic.size())) == magic;
}

int sniff_ar(std::span<const std::byte> data, std::string_view) {
  if (!starts_with(data, "!<arch>\n")) return 0;
  if (starts_with(data, "!<arch>\ndebian-binary")) return 0;  // deb
  return 90;
}

int sniff_rpm(std::span<const std::byte> data, std::string_view) {
  if (data.size() < 104 || !starts_with(data, "\xED\xAB\xEE\xDB")) return 0;
  const auto major = std::to_integer<int>(data[4]);
  if (major < 3 || major > 4) return 0;
  return starts_with(data.subspan(96), "\x8E\xAD\xE8\x01") ? 96 : 0;
}

int sniff_xar(std::span<const std::byte> data, std::string_view) {
  if (data.size() < 28 || !starts_with(data, "xar!")) return 0;
  const auto header_size = (std::to_integer<unsigned>(data[4]) << 8) | std::to_integer<unsigned>(data[5]);
  const auto version = (std::to_integer<unsigned>(data[6]) << 8) | std::to_integer<unsigned>(data[7]);
  return header_size >= 28 && version == 1 ? 92 : 0;
}

ArchiveFormat make(std::string id, std::string name, std::string description, std::vector<std::string> extensions,
                   std::string mime, int popularity) {
  ArchiveFormat f;
  f.info.id = std::move(id);
  f.info.name = std::move(name);
  f.info.description = std::move(description);
  f.info.category = Category::Archive;
  f.info.extensions = std::move(extensions);
  f.info.mime = std::move(mime);
  f.info.popularity = popularity;
  return f;
}

Result<ir::Archive> read_cpio_format(ReadContext& ctx) {
  EntrySink sink(ctx, ctx.input.size());
  CVT_TRY(unixfmt::read_cpio(ctx.input, sink, ctx));
  return sink.finish();
}

Result<Bytes> write_cpio_format(const ir::Archive& archive, WriteContext& ctx) {
  const WritePolicy policy = resolve_write_policy(ctx);
  CVT_TRY_ASSIGN(const auto entries, prepare_entries(archive, policy, ctx,
                                                     {.symlinks = true, .directories = true, .add_parent_directories = true}));
  return unixfmt::write_cpio(entries, ctx);
}

// The standard set (like tar, these formats do not compress: compression_level
// and preset are accepted so that every archive target takes the same options).
std::vector<OptionSpec> plain_write_options() { return standard_write_options(); }

}  // namespace

void register_unix_formats(Registry& registry) {
  {
    ArchiveFormat f = make("cpio", "cpio archive",
                           "Unix cpio archive (newc, crc, odc or old binary), as used by initramfs images and RPM payloads.",
                           {"cpio"}, "application/x-cpio", 25);
    f.info.signatures = {Signature::at(0, "'070701'"), Signature::at(0, "'070702'"), Signature::at(0, "'070707'")};
    f.info.sniff = [](std::span<const std::byte> data, std::string_view) { return unixfmt::is_cpio(data) ? 80 : 0; };
    f.read = read_cpio_format;
    f.write = write_cpio_format;
    f.write_options = plain_write_options();
    register_archive_format(registry, std::move(f));
  }
  {
    ArchiveFormat f = make("ar", "ar archive",
                           "Unix ar archive (GNU or BSD flavour), the container of static libraries; it holds a flat "
                           "list of files without directories.",
                           {"ar", "a"}, "application/x-archive", 20);
    f.info.sniff = sniff_ar;
    f.read = unixfmt::read_ar;
    f.write = unixfmt::write_ar;
    f.write_options = plain_write_options();
    register_archive_format(registry, std::move(f));
  }
  {
    ArchiveFormat f = make("deb", "Debian package",
                           "A Debian/Ubuntu software package. Read as its installed files plus the package control "
                           "files under DEBIAN/ (the layout of `dpkg-deb -R`).",
                           {"deb", "udeb"}, "application/vnd.debian.binary-package", 35);
    f.info.signatures = {Signature::at(0, "'!<arch>' 0A 'debian-binary'")};
    f.read = unixfmt::read_deb;
    f.read_cost = 12;
    register_archive_format(registry, std::move(f));
  }
  {
    ArchiveFormat f = make("rpm", "RPM package",
                           "A Red Hat / Fedora / SUSE software package; read as the files of its payload.", {"rpm"},
                           "application/x-rpm", 30);
    f.info.signatures = {Signature::at(0, "ED AB EE DB")};
    f.info.sniff = sniff_rpm;
    f.read = unixfmt::read_rpm;
    f.read_cost = 12;
    register_archive_format(registry, std::move(f));
  }
  {
    ArchiveFormat f = make("shar", "Shell archive",
                           "A POSIX shell script that recreates the files when run with sh (text as here-documents, "
                           "binary files as base64). Write only.",
                           {"shar"}, "application/x-shar", 5);
    f.write = unixfmt::write_shar;
    f.write_options = plain_write_options();
    register_archive_format(registry, std::move(f));
  }
  {
    ArchiveFormat f = make("warc", "Web archive (WARC)",
                           "An ISO 28500 web archive (WARC 1.0 / 1.1, plain or gzip-compressed); read as the pages "
                           "and files it captured, named after their URLs.",
                           {"warc", "warc.gz"}, "application/warc", 15);
    f.info.signatures = {Signature::at(0, "'WARC/1.'")};
    f.info.sniff = [](std::span<const std::byte> data, std::string_view) { return unixfmt::sniff_warc(data); };
    f.read = unixfmt::read_warc;
    f.read_cost = 12;
    register_archive_format(registry, std::move(f));
  }
  {
    ArchiveFormat f = make("xar", "xar archive",
                           "eXtensible ARchive, the container of macOS installer packages (.pkg) and .xip files.",
                           {"xar", "pkg", "xip"}, "application/x-xar", 20);
    f.info.signatures = {Signature::at(0, "'xar!'")};
    f.info.sniff = sniff_xar;
    f.read = unixfmt::read_xar;
    f.read_cost = 12;
    register_archive_format(registry, std::move(f));
  }
}

}  // namespace cvt::archive
