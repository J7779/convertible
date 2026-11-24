// Convertible core - archive family: cpio reading (newc, crc, odc, old binary) and writing (newc).
//
// Layouts (POSIX.1 "cpio Archive/Interchange File Format" for odc, the SVR4
// "new ASCII" format for newc/crc, and the historical binary format):
//   newc/crc  "070701"/"070702" + 13 fields of 8 hex digits (110 bytes); the
//             name and the data are each padded to a multiple of 4 bytes.
//   odc       "070707" + octal fields (76 bytes); no padding.
//   binary    13 16-bit words in the writer's byte order (26 bytes), 32-bit
//             values most significant word first; name and data padded to 2.
// Hard links: newc writers store the data once, with the LAST link; the
// earlier links have size 0. The other formats repeat the data. Every link
// comes out as a regular file with the content.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>
#include <utility>

#include "families/archive/formats/unix_internal.hpp"
#include "util/text.hpp"

namespace cvt::archive::unixfmt {

namespace {

enum class Variant { Newc, Crc, Odc, BinLe, BinBe };

constexpr std::uint32_t type_mask = 0170000;
constexpr std::uint32_t type_dir = 0040000;
constexpr std::uint32_t type_file = 0100000;
constexpr std::uint32_t type_link = 0120000;

struct Header {
  Variant variant = Variant::Newc;
  std::size_t size = 0;  // bytes of the fixed header
  std::uint64_t dev = 0;
  std::uint64_t ino = 0;
  std::uint64_t mode = 0;
  std::uint64_t nlink = 0;
  std::uint64_t mtime = 0;
  std::uint64_t namesize = 0;
  std::uint64_t filesize = 0;
  std::uint64_t check = 0;
};

std::optional<Variant> variant_at(ByteView d) noexcept {
  if (d.size() >= 6) {
    const std::string_view magic = as_chars(d.first(6));
    if (magic == "070701") return Variant::Newc;
    if (magic == "070702") return Variant::Crc;
    if (magic == "070707") return Variant::Odc;
  }
  if (d.size() >= 2) {
    const auto b0 = std::to_integer<std::uint8_t>(d[0]);
    const auto b1 = std::to_integer<std::uint8_t>(d[1]);
    if (b0 == 0xC7 && b1 == 0x71) return Variant::BinLe;
    if (b0 == 0x71 && b1 == 0xC7) return Variant::BinBe;
  }
  return std::nullopt;
}

std::optional<Header> parse_header(ByteView d) noexcept {
  const auto variant = variant_at(d);
  if (!variant) return std::nullopt;
  Header h;
  h.variant = *variant;
  const std::string_view s = as_chars(d);
  switch (*variant) {
    case Variant::Newc:
    case Variant::Crc: {
      h.size = 110;
      if (d.size() < h.size) return std::nullopt;
      std::array<std::uint64_t, 13> f{};
      for (std::size_t i = 0; i < f.size(); ++i) {
        const auto v = parse_field(s.substr(6 + i * 8, 8), 16);
        if (!v) return std::nullopt;
        f[i] = *v;
      }
      h.ino = f[0];
      h.mode = f[1];
      h.nlink = f[4];
      h.mtime = f[5];
      h.filesize = f[6];
      h.dev = (f[7] << 32) | f[8];
      h.namesize = f[11];
      h.check = f[12];
      return h;
    }
    case Variant::Odc: {
      h.size = 76;
      if (d.size() < h.size) return std::nullopt;
      const auto field = [&](std::size_t at, std::size_t len) { return parse_field(s.substr(at, len), 8); };
      const auto dev = field(6, 6);
      const auto ino = field(12, 6);
      const auto mode = field(18, 6);
      const auto nlink = field(36, 6);
      const auto mtime = field(48, 11);
      const auto namesize = field(59, 6);
      const auto filesize = field(65, 11);
      if (!dev || !ino || !mode || !nlink || !mtime || !namesize || !filesize) return std::nullopt;
      h.dev = *dev;
      h.ino = *ino;
      h.mode = *mode;
      h.nlink = *nlink;
      h.mtime = *mtime;
      h.namesize = *namesize;
      h.filesize = *filesize;
      return h;
    }
    case Variant::BinLe:
    case Variant::BinBe: {
      h.size = 26;
      if (d.size() < h.size) return std::nullopt;
      util::ByteReader r(d.first(26));
      const std::endian order = *variant == Variant::BinLe ? std::endian::little : std::endian::big;
      std::array<std::uint64_t, 13> w{};
      for (auto& v : w) v = r.read<std::uint16_t>(order);
      h.dev = w[1];
      h.ino = w[2];
      h.mode = w[3];
      h.nlink = w[6];
      h.mtime = (w[8] << 16) | w[9];
      h.namesize = w[10];
      h.filesize = (w[11] << 16) | w[12];
      return h;
    }
  }
  return std::nullopt;
}

std::uint64_t align_up(std::uint64_t v, std::uint64_t a) noexcept { return (v + a - 1) / a * a; }

}  // namespace

bool is_cpio(ByteView data) noexcept {
  const auto h = parse_header(data);
  if (!h || h->namesize == 0 || h->namesize > 4096) return false;
  if (h->size + h->namesize > data.size()) return true;  // too short to see the name; the magic is enough
  // The name is NUL-terminated.
  return data[h->size + static_cast<std::size_t>(h->namesize) - 1] == std::byte{0};
}

Result<void> read_cpio(ByteView data, EntrySink& sink, const Context& ctx) {
  struct LinkGroup {
    std::vector<std::size_t> waiting;  // entries still without content
    std::optional<std::size_t> holder;  // entry holding the content
  };
  std::vector<ir::ArchiveEntry> entries;
  std::map<std::pair<std::uint64_t, std::uint64_t>, LinkGroup> links;
  std::size_t pos = 0;
  std::size_t steps = 0;
  bool trailer = false;
  std::size_t skipped_special = 0;
  while (pos < data.size()) {
    if ((++steps & 255u) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(data.size())));
    const ByteView rest = data.subspan(pos);
    const auto h = parse_header(rest);
    if (!h) {
      if (pos == 0) return fail(Status::Decode, "not a cpio archive");
      return fail(Status::Decode, "cpio header at offset {} is damaged", pos);
    }
    const bool newc = h->variant == Variant::Newc || h->variant == Variant::Crc;
    const bool binary = h->variant == Variant::BinLe || h->variant == Variant::BinBe;
    if (h->namesize == 0 || h->namesize > 65536) return fail(Status::Decode, "cpio entry name is damaged");
    std::uint64_t name_end = h->size + h->namesize;
    if (name_end > rest.size()) return fail(Status::Decode, "cpio archive is truncated");
    std::string_view raw_name = as_chars(rest.subspan(h->size, static_cast<std::size_t>(h->namesize)));
    if (const std::size_t nul = raw_name.find('\0'); nul != std::string_view::npos) raw_name = raw_name.substr(0, nul);
    const std::uint64_t data_start = newc ? align_up(name_end, 4) : (binary ? align_up(name_end, 2) : name_end);
    if (data_start > rest.size() || h->filesize > rest.size() - data_start) {
      return fail(Status::Decode, "cpio entry data is truncated");
    }
    const ByteView body = rest.subspan(static_cast<std::size_t>(data_start), static_cast<std::size_t>(h->filesize));
    const std::uint64_t next = newc ? align_up(data_start + h->filesize, 4)
                                    : (binary ? align_up(data_start + h->filesize, 2) : data_start + h->filesize);
    pos += static_cast<std::size_t>(std::min<std::uint64_t>(next, rest.size()));

    if (raw_name == "TRAILER!!!") {
      trailer = true;
      break;
    }
    if (h->variant == Variant::Crc && (h->mode & type_mask) == type_file) {
      std::uint32_t sum = 0;
      for (const std::byte b : body) sum += std::to_integer<std::uint32_t>(b);
      if (sum != static_cast<std::uint32_t>(h->check)) {
        return fail(Status::Decode, "cpio checksum mismatch for '{}'", util::sanitize_utf8(raw_name));
      }
    }
    ir::ArchiveEntry entry;
    entry.path = decode_name(raw_name, false, false);
    entry.mtime = static_cast<std::int64_t>(h->mtime);
    entry.unix_mode = static_cast<std::uint32_t>(h->mode & 07777u);
    const auto type = static_cast<std::uint32_t>(h->mode & type_mask);
    if (type == type_dir) {
      entry.is_directory = true;
    } else if (type == type_link) {
      entry.symlink_target = util::sanitize_utf8(as_chars(body));
      if (entry.symlink_target.empty()) continue;
    } else if (type == type_file || type == 0) {
      // Plain file (type 0: very old archives without type bits).
      CVT_TRY(sink.charge(body.size()));
      entry.bytes.assign(body.begin(), body.end());
    } else {
      ++skipped_special;
      continue;
    }
    // "." (the archive root) carries nothing.
    if (!ir::sanitize_archive_path(entry.path) && util::trim(raw_name).find_first_not_of("./") == std::string_view::npos) {
      continue;
    }
    CVT_TRY(ctx.limits.check_archive_entries(entries.size() + 1));
    const std::size_t index = entries.size();
    const bool file = entry.is_file();
    const bool has_data = !entry.bytes.empty();
    entries.push_back(std::move(entry));
    if (file && h->nlink > 1) {
      LinkGroup& group = links[{h->dev, h->ino}];
      if (has_data) {
        group.holder = index;
        for (const std::size_t w : group.waiting) {
          CVT_TRY(sink.charge(entries[index].bytes.size()));
          entries[w].bytes = entries[index].bytes;
        }
        group.waiting.clear();
      } else if (group.holder) {
        CVT_TRY(sink.charge(entries[*group.holder].bytes.size()));
        entries[index].bytes = entries[*group.holder].bytes;
      } else {
        group.waiting.push_back(index);
      }
    }
  }
  if (!trailer) ctx.warn("The cpio archive ends without its TRAILER!!! record; it may be truncated.");
  if (skipped_special != 0) ctx.warn(std::format("{} device file(s), FIFO(s) or socket(s) were skipped.", skipped_special));
  for (ir::ArchiveEntry& e : entries) CVT_TRY(sink.add(std::move(e)));
  return {};
}

// ------------------------------------------------------------------------------- writing

namespace {

void put_hex(Bytes& out, std::uint64_t v) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  std::array<char, 8> text{};
  for (std::size_t i = 8; i-- > 0;) {
    text[i] = digits[v & 15u];
    v >>= 4;
  }
  append(out, std::string_view(text.data(), text.size()));
}

void put_entry(Bytes& out, std::uint64_t ino, std::uint32_t mode, std::uint64_t nlink, std::uint64_t mtime,
               std::string_view name, ByteView body) {
  append(out, std::string_view("070701"));
  put_hex(out, ino);
  put_hex(out, mode);
  put_hex(out, 0);  // uid
  put_hex(out, 0);  // gid
  put_hex(out, nlink);
  put_hex(out, mtime);
  put_hex(out, body.size());
  put_hex(out, 0);  // dev major
  put_hex(out, 0);  // dev minor
  put_hex(out, 0);  // rdev major
  put_hex(out, 0);  // rdev minor
  put_hex(out, name.size() + 1);
  put_hex(out, 0);  // check
  append(out, name);
  out.push_back(std::byte{0});
  out.resize(static_cast<std::size_t>(align_up(out.size(), 4)), std::byte{0});
  append(out, body);
  out.resize(static_cast<std::size_t>(align_up(out.size(), 4)), std::byte{0});
}

}  // namespace

Result<Bytes> write_cpio(const std::vector<WriteEntry>& entries, const Context& ctx) {
  Bytes out;
  std::uint64_t ino = 0;
  for (const WriteEntry& e : entries) {
    if ((++ino & 255u) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(ino) / static_cast<double>(entries.size())));
    ByteView body;
    std::uint32_t type = type_file;
    std::uint64_t nlink = 1;
    if (e.type == EntryType::Directory) {
      type = type_dir;
      nlink = 2;
    } else if (e.type == EntryType::Symlink) {
      type = type_link;
      body = as_bytes(e.link_target());
    } else {
      body = e.data();
    }
    if (body.size() > 0xFFFFFFFFull) return fail(Status::Encode, "'{}' is too large for cpio (4 GiB per file)", e.path);
    const std::uint64_t mtime = std::clamp<std::int64_t>(e.mtime, 0, 0xFFFFFFFFll);
    put_entry(out, ino, type | (e.mode & 07777u), nlink, static_cast<std::uint64_t>(mtime), e.path, body);
  }
  put_entry(out, 0, 0, 1, 0, "TRAILER!!!", {});
  out.resize(static_cast<std::size_t>(align_up(out.size(), 512)), std::byte{0});
  return out;
}

}  // namespace cvt::archive::unixfmt
