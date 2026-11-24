// Convertible core - archive family: ARJ archives, read only.
//
// Every header is `60 EA size:u16 basic[size] crc32:u32` followed by
// extended headers (`size:u16 data crc32:u32`, ending with size 0). The basic
// header starts with a fixed part (first_hdr_size, versions, host OS, flags,
// method, file type, times, sizes, CRC-32 of the file, mode, ...) followed
// by the NUL-terminated name and comment. The first header describes the
// archive (its comment becomes the archive comment); a header of size 0 ends
// the archive. Host UNIX archives store Unix times and modes, the others MS-DOS
// times. Methods: 0 stored, 1..3 and 4 through the family's Arj codec.
// Garbled (encrypted) entries and multi-volume archives are refused with a
// precise message; chapters and volume labels are skipped. The CRC-32 of
// every header and every file is verified.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/legacy_util.hpp"
#include "util/crc.hpp"
#include "util/text.hpp"

namespace cvt::archive::legacy {

namespace {

namespace cz = compression;

constexpr std::size_t max_basic = 2600;

enum : std::uint8_t { garbled = 0x01, volume = 0x04, extfile = 0x08 };

struct Block {
  ByteView basic;
  std::size_t next = 0;  // position after the extended headers
};

// Read the header block at `pos` (signature already checked by the caller).
Result<std::optional<Block>> header_at(ByteView d, std::size_t pos) {
  if (d.size() - pos < 4 || le16(d, pos) != 0xEA60) return fail(Status::Decode, "ARJ header is damaged");
  const std::size_t size = le16(d, pos + 2);
  if (size == 0) return std::optional<Block>{};
  if (size > max_basic || size < 30) return fail(Status::Decode, "ARJ header has an invalid size");
  if (d.size() - pos - 4 < size + 4) return fail(Status::Decode, "ARJ header is truncated");
  Block b;
  b.basic = d.subspan(pos + 4, size);
  if (util::crc32(b.basic) != le32(d, pos + 4 + size)) return fail(Status::Decode, "ARJ header fails its CRC check");
  std::size_t p = pos + 4 + size + 4;
  for (std::size_t n = 0;; ++n) {
    if (n > 64 || d.size() - p < 2) return fail(Status::Decode, "ARJ extended header is damaged");
    const std::size_t ext = le16(d, p);
    p += 2;
    if (ext == 0) break;
    if (d.size() - p < ext + 4) return fail(Status::Decode, "ARJ extended header is truncated");
    p += ext + 4;
  }
  b.next = p;
  return std::optional<Block>(b);
}

std::pair<std::string_view, std::string_view> name_and_comment(ByteView basic) {
  const std::size_t first = u8(basic, 0);
  if (first > basic.size()) return {};
  std::string_view rest = as_chars(basic.subspan(first));
  const std::size_t nul = rest.find('\0');
  const std::string_view name = rest.substr(0, nul);
  std::string_view comment;
  if (nul != std::string_view::npos) {
    comment = rest.substr(nul + 1);
    comment = comment.substr(0, comment.find('\0'));
  }
  return {name, comment};
}

bool unix_host(std::uint8_t host) { return host == 2 || host == 8; }

std::optional<std::size_t> find_main_header(ByteView d) {
  // Usually at 0; self-extracting archives put a program in front.
  const std::size_t scan = std::min<std::size_t>(d.size(), 1u << 20);
  for (std::size_t p = 0; p + 4 <= scan; ++p) {
    if (d[p] != std::byte{0x60} || d[p + 1] != std::byte{0xEA}) continue;
    const auto h = header_at(d, p);
    if (h && *h && u8((*h)->basic, 6) == 2) return p;
    if (p == 0 && h && *h) return p;
  }
  return std::nullopt;
}

}  // namespace

Result<ir::Archive> read_arj(ReadContext& ctx) {
  const ByteView d = ctx.input;
  const auto start = find_main_header(d);
  if (!start) return fail(Status::Decode, "not an ARJ archive");
  CVT_TRY_ASSIGN(const auto main, header_at(d, *start));
  if (!main) return fail(Status::Decode, "ARJ archive has no main header");
  if (u8(main->basic, 4) & volume) return fail(Status::Decode, "multi-volume ARJ archives are not supported");
  if (u8(main->basic, 4) & garbled) return fail(Status::Decode, "encrypted (garbled) ARJ archives are not supported");
  EntrySink sink(ctx, d.size());
  std::string comment = decode_name(name_and_comment(main->basic).second, false, true);
  std::size_t pos = main->next;
  std::size_t count = 0;
  for (;;) {
    if (pos >= d.size()) break;  // tolerate a missing end header
    CVT_TRY_ASSIGN(const auto block, header_at(d, pos));
    if (!block) break;
    const ByteView b = block->basic;
    const std::uint8_t host = u8(b, 3);
    const std::uint8_t flags = u8(b, 4);
    const std::uint8_t method = u8(b, 5);
    const std::uint8_t type = u8(b, 6);
    const std::uint32_t time = le32(b, 8);
    const std::uint32_t packed = le32(b, 12);
    const std::uint32_t size = le32(b, 16);
    const std::uint32_t crc = le32(b, 20);
    const std::uint16_t mode = le16(b, 26);
    if (d.size() - block->next < packed) return fail(Status::Decode, "ARJ entry data is truncated");
    const ByteView data = d.subspan(block->next, packed);
    pos = block->next + packed;
    if ((++count & 63u) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(d.size() + 1)));
    if (flags & (volume | extfile)) return fail(Status::Decode, "multi-volume ARJ archives are not supported");
    if (type == 4 || type == 5 || type == 2) continue;  // volume and chapter labels
    const auto [raw_name, raw_comment] = name_and_comment(b);
    (void)raw_comment;
    ir::ArchiveEntry e;
    e.path = decode_name(raw_name, false, true);
    e.mtime = unix_host(host) ? static_cast<std::int64_t>(time)
                              : dos_time(static_cast<std::uint16_t>(time >> 16), static_cast<std::uint16_t>(time & 0xFFFFu));
    if (unix_host(host)) e.unix_mode = mode & 07777u;
    if (type == 3) {
      e.is_directory = true;
      CVT_TRY(sink.add(std::move(e)));
      continue;
    }
    if (flags & garbled) return fail(Status::Decode, "ARJ entry '{}' is encrypted (garbled), which is not supported", e.path);
    CVT_TRY(sink.charge(size));
    if (method == 0) {
      if (packed != size) return fail(Status::Decode, "ARJ stored entry has the wrong size");
      e.bytes = to_bytes(data);
    } else if (method >= 1 && method <= 4) {
      if (!cz::can_decode(cz::Method::Arj)) {
        return fail(Status::Decode, "ARJ method {} is not available in this build", method);
      }
      cz::DecodeOptions o;
      o.flags = method;
      o.output_size = size;
      o.max_output = size;
      o.progress = ctx.progress;
      CVT_TRY_ASSIGN(e.bytes, cz::decompress(cz::Method::Arj, data, o));
    } else {
      return fail(Status::Decode, "ARJ method {} is not supported", method);
    }
    if (e.bytes.size() != size) return fail(Status::Decode, "ARJ entry '{}' has the wrong size", e.path);
    if (util::crc32(e.bytes) != crc) return fail(Status::Decode, "ARJ entry '{}' fails its CRC check", e.path);
    CVT_TRY(sink.add(std::move(e)));
  }
  return sink.finish(std::move(comment));
}

int sniff_arj(std::span<const std::byte> data, std::string_view filename) {
  if (data.size() < 4 || data[0] != std::byte{0x60} || data[1] != std::byte{0xEA}) return 0;
  const auto h = header_at(data, 0);
  if (h && *h) return 95;
  const std::size_t size = le16(data, 2);
  const bool named = util::iends_with(filename, ".arj");
  return size >= 30 && size <= max_basic && named ? 70 : 0;
}

}  // namespace cvt::archive::legacy
