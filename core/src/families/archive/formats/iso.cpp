// Convertible core - archive family: ISO 9660 disc images (with Joliet, Rock Ridge, zisofs and UDF).
//
// Reading (ECMA-119, Joliet, IEEE P1281 SUSP / P1282 Rock Ridge, zisofs):
// volume descriptors from sector 16; directory records with multi-extent
// files and extended attribute records; level 1..3 names ("NAME.EXT;1");
// Joliet UCS-2 names; Rock Ridge names (NM), permissions (PX), times (TF),
// symbolic links (SL), relocated directories (CL / RE) and transparently
// compressed files (ZF, zisofs v1); recording times with their GMT offset.
// The richest tree wins: Rock Ridge, then Joliet, then UDF (udf_read.cpp),
// then plain ISO 9660 names (shown in lower case, as most systems mount
// them). The `iso_filesystem` option picks a tree explicitly.
// Writing lives in iso_write.cpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/formats.hpp"
#include "families/archive/formats/iso_internal.hpp"
#include "util/text.hpp"

namespace cvt::archive {

namespace iso {

namespace {

constexpr std::size_t max_depth = 512;
constexpr int max_continuations = 64;

std::uint32_t le32(ByteView d, std::size_t at) {
  return std::to_integer<std::uint32_t>(d[at]) | (std::to_integer<std::uint32_t>(d[at + 1]) << 8) |
         (std::to_integer<std::uint32_t>(d[at + 2]) << 16) | (std::to_integer<std::uint32_t>(d[at + 3]) << 24);
}
std::uint16_t le16(ByteView d, std::size_t at) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(d[at]) | (std::to_integer<unsigned>(d[at + 1]) << 8));
}
std::uint8_t u8(ByteView d, std::size_t at) { return std::to_integer<std::uint8_t>(d[at]); }

// 7-byte recording time of a directory record.
std::int64_t record_time(ByteView t) {
  if (t.size() < 7 || u8(t, 1) == 0) return 0;
  const auto offset = static_cast<std::int8_t>(u8(t, 6));
  const int minutes = offset >= -48 && offset <= 52 ? offset * 15 : 0;
  return unix_time(1900 + static_cast<std::int64_t>(u8(t, 0)), u8(t, 1), u8(t, 2), u8(t, 3), u8(t, 4), u8(t, 5), minutes);
}

// 17-byte "YYYYMMDDHHMMSScc" + offset time (volume descriptors, long-form TF).
std::int64_t decimal_time(ByteView t) {
  if (t.size() < 17) return 0;
  const std::string_view s = as_chars(t.first(16));
  if (!std::all_of(s.begin(), s.end(), util::is_ascii_digit)) return 0;
  const auto num = [&](std::size_t at, std::size_t len) {
    unsigned v = 0;
    for (std::size_t i = 0; i < len; ++i) v = v * 10 + static_cast<unsigned>(s[at + i] - '0');
    return v;
  };
  const auto offset = static_cast<std::int8_t>(u8(t, 16));
  const int minutes = offset >= -48 && offset <= 52 ? offset * 15 : 0;
  return unix_time(num(0, 4), num(4, 2), num(6, 2), num(8, 2), num(10, 2), num(12, 2), minutes);
}

struct Record {
  std::uint32_t extent = 0;
  std::uint32_t size = 0;
  std::uint8_t flags = 0;
  std::uint8_t xa_length = 0;
  ByteView name;
  ByteView system_use;
  std::int64_t mtime = 0;
  [[nodiscard]] bool is_dir() const noexcept { return (flags & 0x02u) != 0; }
  [[nodiscard]] bool is_dot() const noexcept {
    return name.size() == 1 && (name[0] == std::byte{0} || name[0] == std::byte{1});
  }
};

std::optional<Record> parse_record(ByteView r) {
  if (r.size() < 34) return std::nullopt;
  const std::size_t len = u8(r, 0);
  const std::size_t name_len = u8(r, 32);
  if (len < 33 + name_len || len > r.size()) return std::nullopt;
  Record rec;
  rec.xa_length = u8(r, 1);
  rec.extent = le32(r, 2);
  rec.size = le32(r, 10);
  rec.mtime = record_time(r.subspan(18, 7));
  rec.flags = u8(r, 25);
  rec.name = r.subspan(33, name_len);
  const std::size_t su = 33 + name_len + ((name_len & 1u) == 0 ? 1 : 0);
  rec.system_use = su < len ? r.subspan(su, len - su) : ByteView{};
  return rec;
}

// Rock Ridge / SUSP fields of one directory record.
struct RockRidge {
  bool sp = false;  // only on the root's "." record
  std::size_t skip = 0;
  bool has_name = false;
  std::string name;
  std::optional<std::uint32_t> mode;
  std::optional<std::int64_t> mtime;
  bool has_link = false;
  std::string link;
  bool link_continue = false;
  std::optional<std::uint32_t> child_link;
  bool relocated = false;
  bool zf = false;
  std::uint8_t zf_header = 0;
  std::uint8_t zf_log2 = 0;
  std::uint32_t zf_size = 0;
};

void append_link_component(RockRidge& rr, std::uint8_t flags, std::string_view text) {
  std::string part;
  if ((flags & 0x02u) != 0) {
    part = ".";
  } else if ((flags & 0x04u) != 0) {
    part = "..";
  } else if ((flags & 0x08u) != 0) {
    rr.link = "/";
    rr.link_continue = true;  // no separator after the root
    return;
  } else {
    part = util::sanitize_utf8(text);
  }
  if (!rr.link_continue && !rr.link.empty() && rr.link.back() != '/') rr.link += '/';
  rr.link += part;
  rr.link_continue = (flags & 0x01u) != 0;
}

void parse_entries(ByteView area, RockRidge& rr, std::optional<std::pair<std::uint64_t, std::uint64_t>>& next,
                   bool first_of_root) {
  std::size_t pos = 0;
  while (pos + 4 <= area.size()) {
    const ByteView e = area.subspan(pos);
    const std::string_view sig = as_chars(e.first(2));
    const std::size_t len = u8(e, 2);
    if (len < 4 || len > e.size()) break;
    const ByteView body = e.first(len);
    if (sig == "ST") break;
    if (sig == "SP" && first_of_root && len >= 7 && u8(body, 4) == 0xBE && u8(body, 5) == 0xEF) {
      rr.sp = true;
      rr.skip = u8(body, 6);
    } else if (sig == "CE" && len >= 28) {
      next = std::make_pair(static_cast<std::uint64_t>(le32(body, 4)) * sector + le32(body, 12), le32(body, 20));
    } else if (sig == "NM" && len >= 5) {
      const std::uint8_t flags = u8(body, 4);
      if ((flags & 0x06u) == 0) {
        rr.name += as_chars(body.subspan(5));
        rr.has_name = true;
      }
    } else if (sig == "PX" && len >= 12) {
      rr.mode = le32(body, 4);
    } else if (sig == "TF" && len >= 5) {
      const std::uint8_t flags = u8(body, 4);
      const std::size_t size = (flags & 0x80u) != 0 ? 17 : 7;
      std::size_t at = 5;
      for (int bit = 0; bit < 7; ++bit) {
        if ((flags & (1u << bit)) == 0) continue;
        if (at + size > len) break;
        if (bit == 1) rr.mtime = size == 17 ? decimal_time(body.subspan(at, 17)) : record_time(body.subspan(at, 7));
        at += size;
      }
    } else if (sig == "SL" && len >= 5) {
      rr.has_link = true;
      std::size_t at = 5;
      while (at + 2 <= len) {
        const std::uint8_t flags = u8(body, at);
        const std::size_t clen = u8(body, at + 1);
        if (at + 2 + clen > len) break;
        append_link_component(rr, flags, as_chars(body.subspan(at + 2, clen)));
        at += 2 + clen;
      }
    } else if (sig == "CL" && len >= 12) {
      rr.child_link = le32(body, 4);
    } else if (sig == "RE") {
      rr.relocated = true;
    } else if (sig == "ZF" && len >= 16 && as_chars(body.subspan(4, 2)) == "pz") {
      rr.zf = true;
      rr.zf_header = u8(body, 6);
      rr.zf_log2 = u8(body, 7);
      rr.zf_size = le32(body, 8);
    }
    pos += len;
  }
}

RockRidge parse_system_use(ByteView image, ByteView area, std::size_t skip, bool first_of_root) {
  RockRidge rr;
  area = skip < area.size() ? area.subspan(skip) : ByteView{};
  std::set<std::uint64_t> seen;
  for (int hop = 0; hop <= max_continuations; ++hop) {
    std::optional<std::pair<std::uint64_t, std::uint64_t>> next;
    parse_entries(area, rr, next, first_of_root && hop == 0);
    if (!next) break;
    const auto [at, length] = *next;
    if (at >= image.size() || length > image.size() - at || !seen.insert(at).second) break;
    area = image.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(length));
  }
  return rr;
}

// zisofs (version 1) transparently compressed file.
Result<Bytes> unzisofs(ByteView data, std::uint64_t budget) {
  static constexpr std::uint8_t magic[8] = {0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07};
  if (data.size() < 16 || std::memcmp(data.data(), magic, 8) != 0) {
    return fail(Status::Decode, "ISO zisofs file header is damaged");
  }
  const std::uint64_t size = le32(data, 8);
  const std::size_t header = static_cast<std::size_t>(u8(data, 12)) * 4;
  const unsigned log2 = u8(data, 13);
  if (log2 < 15 || log2 > 17 || header < 16) return fail(Status::Decode, "ISO zisofs file header is damaged");
  if (size > budget) return fail(Status::Limit, "ISO compressed file of {} bytes exceeds the limit", size);
  const std::uint64_t block = std::uint64_t{1} << log2;
  const std::uint64_t blocks = (size + block - 1) / block;
  if (header + (blocks + 1) * 4 > data.size()) return fail(Status::Decode, "ISO zisofs block table is truncated");
  Bytes out;
  out.reserve(static_cast<std::size_t>(size));
  for (std::uint64_t i = 0; i < blocks; ++i) {
    const std::uint32_t start = le32(data, header + static_cast<std::size_t>(i) * 4);
    const std::uint32_t end = le32(data, header + static_cast<std::size_t>(i + 1) * 4);
    const std::uint64_t want = std::min<std::uint64_t>(block, size - out.size());
    if (end < start || end > data.size()) return fail(Status::Decode, "ISO zisofs block table is damaged");
    if (start == end) {
      out.insert(out.end(), static_cast<std::size_t>(want), std::byte{0});
      continue;
    }
    compression::DecodeOptions d;
    d.max_output = want;
    CVT_TRY_ASSIGN(const Bytes piece, compression::decompress(compression::Method::Zlib, data.subspan(start, end - start), d));
    if (piece.size() != want) return fail(Status::Decode, "ISO zisofs block has the wrong size");
    append(out, piece);
  }
  return out;
}

struct Volumes {
  std::optional<std::size_t> primary;  // offset of the primary volume descriptor
  std::optional<std::size_t> joliet;   // offset of the Joliet supplementary descriptor
};

Volumes find_volumes(ByteView image) {
  Volumes v;
  for (std::size_t s = 16; s < 16 + 64; ++s) {
    const std::size_t at = s * sector;
    if (at + sector > image.size()) break;
    const ByteView d = image.subspan(at, sector);
    if (as_chars(d.subspan(1, 5)) != "CD001") break;
    const std::uint8_t type = u8(d, 0);
    if (type == 255) break;
    if (type == 1 && !v.primary) v.primary = at;
    if (type == 2 && !v.joliet && u8(d, 88) == '%' && u8(d, 89) == '/' &&
        (u8(d, 90) == '@' || u8(d, 90) == 'C' || u8(d, 90) == 'E')) {
      v.joliet = at;
    }
  }
  return v;
}

enum class Tree { RockRidge, Joliet, Plain };

std::string iso_name(ByteView raw, Tree tree) {
  std::string name;
  if (tree == Tree::Joliet) {
    std::u16string wide;
    for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
      wide.push_back(static_cast<char16_t>((std::to_integer<unsigned>(raw[i]) << 8) | std::to_integer<unsigned>(raw[i + 1])));
    }
    name = util::utf16_to_utf8(wide);
  } else {
    name = util::sanitize_utf8(as_chars(raw));
  }
  if (const std::size_t semi = name.rfind(';'); semi != std::string::npos) {
    const std::string_view version = std::string_view(name).substr(semi + 1);
    if (std::all_of(version.begin(), version.end(), util::is_ascii_digit)) name.resize(semi);
  }
  if (!name.empty() && name.back() == '.') name.pop_back();
  if (tree == Tree::Plain) name = util::to_lower(name);
  return name;
}

struct Pending {
  std::uint32_t extent;
  std::uint32_t size;
  std::string path;
  std::size_t depth;
};

class Reader {
 public:
  Reader(ByteView image, EntrySink& sink, const Context& ctx, Tree tree, std::size_t skip)
      : image_(image), sink_(sink), ctx_(ctx), tree_(tree), skip_(skip) {}

  Result<void> run(const Record& root) {
    std::vector<Pending> stack;
    stack.push_back({root.extent + root.xa_length, root.size, std::string(), 0});
    std::size_t steps = 0;
    while (!stack.empty()) {
      Pending dir = std::move(stack.back());
      stack.pop_back();
      if ((++steps & 63u) == 0) {
        CVT_TRY(ctx_.checkpoint(std::min(0.99, static_cast<double>(dir.extent) * sector / static_cast<double>(image_.size()))));
      }
      if (dir.depth > max_depth) return fail(Status::Decode, "ISO directory tree is nested too deeply");
      if (!visited_.insert(dir.extent).second) continue;  // loop or shared directory
      CVT_TRY(read_directory(dir, stack));
    }
    return {};
  }

 private:
  Result<ByteView> extent_bytes(std::uint64_t extent, std::uint64_t size) const {
    const std::uint64_t at = extent * sector;
    if (at > image_.size() || size > image_.size() - at) return fail(Status::Decode, "ISO image is truncated");
    return image_.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(size));
  }

  Result<void> read_directory(const Pending& dir, std::vector<Pending>& stack) {
    CVT_TRY_ASSIGN(const ByteView data, extent_bytes(dir.extent, dir.size));
    std::size_t pos = 0;
    // Multi-extent file being collected.
    std::vector<Record> parts;
    while (pos < data.size()) {
      const std::size_t len = u8(data, pos);
      if (len == 0) {
        pos = (pos / sector + 1) * sector;  // records never cross a sector; the rest is padding
        continue;
      }
      const auto rec = parse_record(data.subspan(pos, std::min(len, data.size() - pos)));
      if (!rec) return fail(Status::Decode, "ISO directory record at offset {} is damaged", dir.extent * sector + pos);
      pos += len;
      if (rec->is_dot()) continue;
      parts.push_back(*rec);
      if ((rec->flags & 0x80u) != 0 && !rec->is_dir()) {
        if (parts.size() > 65536) return fail(Status::Decode, "ISO file has too many extents");
        continue;
      }
      CVT_TRY(add_entry(dir, parts, stack));
      parts.clear();
    }
    if (!parts.empty()) CVT_TRY(add_entry(dir, parts, stack));
    return {};
  }

  Result<void> add_entry(const Pending& dir, const std::vector<Record>& parts, std::vector<Pending>& stack) {
    const Record& rec = parts.front();
    RockRidge rr;
    if (tree_ == Tree::RockRidge) rr = parse_system_use(image_, rec.system_use, skip_, false);
    if (rr.relocated) return {};  // shown where its CL entry is
    std::string name = rr.has_name ? util::sanitize_utf8(rr.name) : iso_name(rec.name, tree_);
    if (name.empty() || name == "." || name == "..") return {};
    ir::ArchiveEntry entry;
    entry.path = dir.path.empty() ? name : dir.path + "/" + name;
    entry.mtime = rr.mtime.value_or(rec.mtime);
    if (rr.mode) entry.unix_mode = *rr.mode & 07777u;
    const std::uint32_t type = rr.mode ? (*rr.mode & 0170000u) : 0;
    if (rr.child_link) {
      // A directory relocated by Rock Ridge: its "." record gives the size.
      CVT_TRY_ASSIGN(const ByteView head, extent_bytes(*rr.child_link, 34));
      const auto dot = parse_record(head);
      if (!dot) return fail(Status::Decode, "ISO relocated directory is damaged");
      entry.is_directory = true;
      std::string path = entry.path;
      CVT_TRY(sink_.add(std::move(entry)));
      stack.push_back({*rr.child_link, dot->size, std::move(path), dir.depth + 1});
      return {};
    }
    if (rec.is_dir()) {
      entry.is_directory = true;
      std::string path = entry.path;
      CVT_TRY(sink_.add(std::move(entry)));
      stack.push_back({rec.extent + rec.xa_length, rec.size, std::move(path), dir.depth + 1});
      return {};
    }
    if (rr.has_link || type == 0120000u) {
      if (rr.link.empty()) return {};
      entry.symlink_target = rr.link;
      return sink_.add(std::move(entry));
    }
    if (type != 0 && type != 0100000u) {
      ctx_.warn("Device files, sockets and FIFOs on the disc were skipped.");
      return {};
    }
    std::uint64_t total = 0;
    for (const Record& p : parts) total += p.size;
    if (total > sink_.budget()) return fail(Status::Limit, "ISO file of {} bytes exceeds the limit", total);
    Bytes bytes;
    bytes.reserve(static_cast<std::size_t>(total));
    for (const Record& p : parts) {
      CVT_TRY_ASSIGN(const ByteView data, extent_bytes(static_cast<std::uint64_t>(p.extent) + p.xa_length, p.size));
      append(bytes, data);
    }
    if (rr.zf) {
      CVT_TRY_ASSIGN(bytes, unzisofs(bytes, sink_.budget()));
    }
    CVT_TRY(sink_.charge(bytes.size()));
    entry.bytes = std::move(bytes);
    return sink_.add(std::move(entry));
  }

  ByteView image_;
  EntrySink& sink_;
  const Context& ctx_;
  Tree tree_;
  std::size_t skip_;
  std::set<std::uint32_t> visited_;
};

}  // namespace

bool has_udf(ByteView image) noexcept {
  for (std::size_t s = 16; s < 16 + 64; ++s) {
    const std::size_t at = s * sector;
    if (at + 6 > image.size()) return false;
    const std::string_view id = as_chars(image.subspan(at + 1, 5));
    if (id == "NSR02" || id == "NSR03") return true;
    if (id == "TEA01") return false;
    if (id != "CD001" && id != "BEA01" && id != "BOOT2" && id != "CDW02") return false;
  }
  return false;
}

Result<ir::Archive> read_iso(ReadContext& ctx) {
  const ByteView image = ctx.input;
  const std::string want = ctx.options.get_enum("iso_filesystem", "auto", {"auto", "iso9660", "joliet", "udf"});
  const Volumes volumes = find_volumes(image);
  const bool udf = has_udf(image);
  EntrySink sink(ctx, image.size());

  if (want == "udf" || (want == "auto" && udf && !volumes.primary)) {
    if (!udf) return fail(Status::Decode, "the disc image has no UDF file system");
    CVT_TRY(read_udf(image, sink, ctx));
    return sink.finish();
  }
  if (!volumes.primary) {
    if (image.size() < 17 * sector) return fail(Status::Decode, "not an ISO 9660 image (too short)");
    return fail(Status::Decode, "not an ISO 9660 image (no primary volume descriptor)");
  }
  const ByteView pvd = image.subspan(*volumes.primary, sector);
  if (le16(pvd, 128) != sector) return fail(Status::Decode, "ISO images with {}-byte blocks are not supported", le16(pvd, 128));
  const auto root = parse_record(pvd.subspan(156, 34));
  if (!root) return fail(Status::Decode, "ISO root directory record is damaged");

  // Rock Ridge is announced by an SP entry in the root's "." record.
  RockRidge root_rr;
  if (root->extent < image.size() / sector) {
    const ByteView first = image.subspan(static_cast<std::size_t>(root->extent) * sector);
    if (!first.empty()) {
      if (const auto dot = parse_record(first.first(std::min<std::size_t>(first.size(), u8(first, 0))))) {
        root_rr = parse_system_use(image, dot->system_use, 0, true);
      }
    }
  }

  Tree tree = Tree::Plain;
  Record start = *root;
  if (want == "joliet") {
    if (!volumes.joliet) return fail(Status::Decode, "the disc image has no Joliet tree");
    tree = Tree::Joliet;
  } else if (want == "iso9660") {
    tree = root_rr.sp ? Tree::RockRidge : Tree::Plain;
  } else if (root_rr.sp) {
    tree = Tree::RockRidge;
  } else if (volumes.joliet) {
    tree = Tree::Joliet;
  } else if (udf) {
    CVT_TRY(read_udf(image, sink, ctx));
    return sink.finish();
  }
  if (tree == Tree::Joliet) {
    const auto jroot = parse_record(image.subspan(*volumes.joliet + 156, 34));
    if (!jroot) return fail(Status::Decode, "ISO Joliet root directory record is damaged");
    start = *jroot;
  }
  Reader reader(image, sink, ctx, tree, root_rr.skip);
  CVT_TRY(reader.run(start));
  return sink.finish();
}

}  // namespace iso

namespace {

int sniff_iso(std::span<const std::byte> data, std::string_view) {
  const std::size_t at = 16 * iso::sector;
  if (data.size() < at + 6) return 0;
  const std::string_view id = as_chars(data.subspan(at + 1, 5));
  if (id == "CD001" && data.size() > at + 6 && std::to_integer<int>(data[at + 6]) == 1) return 90;  // the version byte follows the id
  if (id == "BEA01") return 85;
  return 0;
}

}  // namespace

void register_iso_formats(Registry& registry) {
  ArchiveFormat f;
  f.info.id = "iso";
  f.info.name = "ISO 9660 disc image";
  f.info.description =
      "CD/DVD image (ISO 9660 with Joliet and Rock Ridge names; UDF discs are read too). Written as ISO 9660 "
      "level 3 with Rock Ridge and Joliet, readable everywhere.";
  f.info.category = Category::Archive;
  f.info.extensions = {"iso"};
  f.info.mime = "application/x-iso9660-image";
  f.info.popularity = 50;
  f.info.signatures = {Signature::at(32769, "'CD001'"), Signature::at(32769, "'BEA01'")};
  f.info.sniff = sniff_iso;
  f.read = iso::read_iso;
  f.read_options = {OptionSpec::choice("iso_filesystem", "File system", "auto", {"auto", "iso9660", "joliet", "udf"},
                                       "Which directory tree of the disc to read: 'auto' takes the richest one "
                                       "(Rock Ridge, then Joliet, then UDF, then plain ISO 9660 names).")
                        .advanced()};
  f.write = iso::write_iso;
  f.write_options = standard_write_options();
  f.write_options.push_back(OptionSpec::text("volume_label", "Volume label", "CDROM",
                                             "Name of the disc (up to 32 letters, digits or underscores; Joliet keeps "
                                             "up to 16 characters as typed)."));
  f.read_cost = 10;
  f.write_cost = 12;
  register_archive_format(registry, std::move(f));
}

}  // namespace cvt::archive
