// Convertible core - archive family: LHA / LZH archives (header levels 0 to 3), read only.
//
// Each entry starts with a header whose byte 20 holds its level:
//   level 0/1: size:u8 checksum:u8 method[5] packed:u32 size:u32 dos_time:u32
//              attribute:u8 level:u8 name_len:u8 name crc16:u16
//              (level 1 adds os:u8 and a chain of extended headers whose
//              sizes are counted in `packed`)
//   level 2:   total_size:u16 method[5] packed:u32 size:u32 unix_time:u32
//              reserved:u8 level:u8 crc16:u16 os:u8, then extended headers
//   level 3:   like level 2 with 32-bit header sizes
// Extended headers: 0x01 file name, 0x02 directory name (0xFF separated),
// 0x40 MS-DOS attributes, 0x41 Windows times, 0x42 64-bit sizes, 0x50 Unix
// mode, 0x54 Unix time; the rest are skipped. A single 0 byte ends the archive.
// Methods: -lh0-, -lz4- and -pm0- are stored, -lhd- marks a directory, -lh1-,
// -lh4- .. -lh7-, -lzs- and -lz5- go through the family's Lh / Lzs codecs.
// The CRC-16 of every file is verified. Symbolic links follow the LHa for
// UNIX convention "name|target".
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <optional>
#include <string>
#include <string_view>

#include "families/archive/archive_common.hpp"
#include "families/archive/checksums.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/legacy_util.hpp"
#include "util/text.hpp"

namespace cvt::archive::legacy {

namespace {

namespace cz = compression;

struct LhMethod {
  enum class Kind { Stored, Directory, Lh, Lzs, Unsupported } kind = Kind::Unsupported;
  int window_bits = 0;
  std::uint32_t flags = 0;
};

LhMethod parse_method(std::string_view m) {
  using K = LhMethod::Kind;
  if (m == "-lh0-" || m == "-lz4-" || m == "-pm0-") return {K::Stored, 0, 0};
  if (m == "-lhd-") return {K::Directory, 0, 0};
  if (m == "-lh1-") return {K::Lh, 12, 1};
  if (m == "-lh4-") return {K::Lh, 12, 0};
  if (m == "-lh5-") return {K::Lh, 13, 0};
  if (m == "-lh6-") return {K::Lh, 15, 0};
  if (m == "-lh7-") return {K::Lh, 16, 0};
  if (m == "-lzs-") return {K::Lzs, 11, 0};
  if (m == "-lz5-") return {K::Lzs, 12, 0};
  return {};
}

bool plausible_method(ByteView m) {
  if (m.size() < 5 || m[0] != std::byte{'-'} || m[4] != std::byte{'-'}) return false;
  for (std::size_t i = 1; i < 4; ++i) {
    const auto c = u8(m, i);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

struct Header {
  std::string method;
  std::uint64_t packed = 0;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  std::uint32_t mode = 0;
  std::uint16_t crc = 0;
  std::string name;
  std::string dir;
  std::size_t data_pos = 0;
};

std::string join_dir(std::string_view raw) {
  std::string out;
  for (const char c : raw) out += (c == '\xFF' || c == '\\') ? '/' : c;
  return out;
}

// Parse extended headers starting at `pos` with first size `next`, each
// ending with a `width`-byte size of the next one. Returns the end position.
Result<std::size_t> extended(ByteView d, std::size_t pos, std::uint64_t next, std::size_t width, Header& h,
                             std::size_t limit) {
  std::size_t count = 0;
  while (next != 0) {
    if (++count > 1024) return fail(Status::Decode, "LHA header has too many extensions");
    if (next < 1 + width || next > limit || pos > limit - next) return fail(Status::Decode, "LHA extended header is damaged");
    const ByteView x = d.subspan(pos, static_cast<std::size_t>(next));
    const ByteView body = x.subspan(1, x.size() - 1 - width);
    switch (u8(x, 0)) {
      case 0x01:
        h.name = std::string(as_chars(body));
        break;
      case 0x02:
        h.dir = join_dir(as_chars(body));
        break;
      case 0x50:
        if (body.size() >= 2) h.mode = le16(body, 0);
        break;
      case 0x54:
        if (body.size() >= 4) h.mtime = le32(body, 0);
        break;
      case 0x42:
        if (body.size() >= 16) {
          h.packed = le32(body, 0) | (static_cast<std::uint64_t>(le32(body, 4)) << 32);
          h.size = le32(body, 8) | (static_cast<std::uint64_t>(le32(body, 12)) << 32);
        }
        break;
      default:
        break;
    }
    pos += static_cast<std::size_t>(next);
    next = width == 2 ? le16(x, x.size() - 2) : le32(x, x.size() - 4);
  }
  return pos;
}

// Parse the header at `pos`; nullopt at the end of the archive.
Result<std::optional<Header>> parse_header(ByteView d, std::size_t pos) {
  if (pos >= d.size() || d[pos] == std::byte{0}) return std::optional<Header>{};
  if (d.size() - pos < 22) return fail(Status::Decode, "LHA header is truncated");
  const int level = u8(d, pos + 20);
  Header h;
  if (!plausible_method(d.subspan(pos + 2, 5))) return fail(Status::Decode, "LHA header is damaged");
  h.method = std::string(as_chars(d.subspan(pos + 2, 5)));
  h.packed = le32(d, pos + 7);
  h.size = le32(d, pos + 11);
  switch (level) {
    case 0:
    case 1: {
      const std::size_t hsize = u8(d, pos);
      if (d.size() - pos < hsize + 2 || hsize < 22) return fail(Status::Decode, "LHA header is truncated");
      unsigned sum = 0;
      for (std::size_t i = 0; i < hsize; ++i) sum += u8(d, pos + 2 + i);
      if ((sum & 0xFFu) != u8(d, pos + 1)) return fail(Status::Decode, "LHA header checksum mismatch");
      h.mtime = dos_time(le16(d, pos + 17), le16(d, pos + 15));
      const std::size_t name_len = u8(d, pos + 21);
      if (22 + name_len + 2 > hsize + 2) return fail(Status::Decode, "LHA header is damaged");
      const std::string_view raw = as_chars(d.subspan(pos + 22, name_len));
      h.crc = le16(d, pos + 22 + name_len);
      const std::size_t slash = raw.find_last_of("\\\xFF/");
      if (slash != std::string_view::npos) {
        h.dir = join_dir(raw.substr(0, slash + 1));
        h.name = std::string(raw.substr(slash + 1));
      } else {
        h.name = std::string(raw);
      }
      std::size_t end = pos + 2 + hsize;
      if (level == 1) {
        const std::uint16_t next = le16(d, end - 2);
        CVT_TRY_ASSIGN(const std::size_t after, extended(d, end, next, 2, h, d.size()));
        const std::uint64_t ext = after - end;
        if (ext > h.packed) return fail(Status::Decode, "LHA extended headers exceed the entry");
        if (h.packed == le32(d, pos + 7)) h.packed -= ext;  // unless a 64-bit size header replaced it
        end = after;
      }
      h.data_pos = end;
      break;
    }
    case 2: {
      const std::size_t total = le16(d, pos);
      if (total < 26 || d.size() - pos < total) return fail(Status::Decode, "LHA header is truncated");
      h.mtime = le32(d, pos + 15);
      h.crc = le16(d, pos + 21);
      CVT_TRY(extended(d, pos + 26, le16(d, pos + 24), 2, h, pos + total));
      h.data_pos = pos + total;
      break;
    }
    case 3: {
      if (le16(d, pos) != 4 || d.size() - pos < 32) return fail(Status::Decode, "LHA level 3 header is damaged");
      const std::uint32_t total = le32(d, pos + 24);
      if (total < 32 || d.size() - pos < total) return fail(Status::Decode, "LHA header is truncated");
      h.mtime = le32(d, pos + 15);
      h.crc = le16(d, pos + 21);
      CVT_TRY(extended(d, pos + 32, le32(d, pos + 28), 4, h, pos + total));
      h.data_pos = pos + total;
      break;
    }
    default:
      return fail(Status::Decode, "LHA header level {} is not supported", level);
  }
  if (h.packed > d.size() - h.data_pos) return fail(Status::Decode, "LHA entry data is truncated");
  return std::optional<Header>(std::move(h));
}

}  // namespace

Result<ir::Archive> read_lha(ReadContext& ctx) {
  const ByteView d = ctx.input;
  EntrySink sink(ctx, d.size());
  std::size_t pos = 0;
  std::size_t count = 0;
  for (;;) {
    CVT_TRY_ASSIGN(const auto parsed, parse_header(d, pos));
    if (!parsed) break;
    const Header& h = *parsed;
    pos = h.data_pos + static_cast<std::size_t>(h.packed);
    if ((++count & 63u) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(d.size() + 1)));
    std::string name = h.name;
    std::string target;
    if (const std::size_t bar = name.find('|'); bar != std::string::npos) {
      target = name.substr(bar + 1);
      name.resize(bar);
    }
    ir::ArchiveEntry e;
    e.path = decode_name(h.dir + name, false, true);
    e.mtime = h.mtime;
    e.unix_mode = h.mode & 07777u;
    const LhMethod m = parse_method(h.method);
    if (!target.empty() || (h.mode & 0170000u) == 0120000u) {
      if (target.empty()) continue;
      e.symlink_target = decode_name(target, false, true);
      CVT_TRY(sink.add(std::move(e)));
      continue;
    }
    if (m.kind == LhMethod::Kind::Directory) {
      if (e.path.empty()) continue;
      e.is_directory = true;
      CVT_TRY(sink.add(std::move(e)));
      continue;
    }
    const ByteView packed = d.subspan(h.data_pos, static_cast<std::size_t>(h.packed));
    CVT_TRY(sink.charge(h.size));
    switch (m.kind) {
      case LhMethod::Kind::Stored:
        if (h.packed != h.size) return fail(Status::Decode, "LHA stored entry has the wrong size");
        e.bytes = to_bytes(packed);
        break;
      case LhMethod::Kind::Lh:
      case LhMethod::Kind::Lzs: {
        const cz::Method method = m.kind == LhMethod::Kind::Lh ? cz::Method::Lh : cz::Method::Lzs;
        if (!cz::can_decode(method)) {
          return fail(Status::Decode, "LHA method {} is not available in this build", h.method);
        }
        cz::DecodeOptions o;
        o.window_bits = m.window_bits;
        o.flags = m.flags;
        o.output_size = h.size;
        o.max_output = h.size;
        o.progress = ctx.progress;
        CVT_TRY_ASSIGN(e.bytes, cz::decompress(method, packed, o));
        break;
      }
      default:
        return fail(Status::Decode, "LHA method {} is not supported", h.method);
    }
    if (e.bytes.size() != h.size) return fail(Status::Decode, "LHA entry '{}' has the wrong size", e.path);
    if (crc16_arc(e.bytes) != h.crc) return fail(Status::Decode, "LHA entry '{}' fails its CRC check", e.path);
    CVT_TRY(sink.add(std::move(e)));
  }
  if (count == 0 && d.size() != 1) return fail(Status::Decode, "not an LHA archive");
  return sink.finish();
}

int sniff_lha(std::span<const std::byte> data, std::string_view filename) {
  if (data.size() < 22 || !plausible_method(data.subspan(2, 5))) return 0;
  if (u8(data, 2 + 1) != 'l' && u8(data, 2 + 1) != 'p') return 0;
  const int level = u8(data, 20);
  if (level > 3) return 0;
  const bool named = util::iends_with(filename, ".lzh") || util::iends_with(filename, ".lha");
  if (parse_method(as_chars(data.subspan(2, 5))).kind == LhMethod::Kind::Unsupported) return named ? 60 : 0;
  if (level <= 1) {
    const std::size_t hsize = u8(data, 0);
    if (data.size() >= hsize + 2) {
      unsigned sum = 0;
      for (std::size_t i = 0; i < hsize; ++i) sum += u8(data, 2 + i);
      if ((sum & 0xFFu) != u8(data, 1)) return 0;
    }
  }
  return named ? 92 : 85;
}

}  // namespace cvt::archive::legacy
