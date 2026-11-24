// Convertible core - archive family: UDF file system reader (ECMA-167 / OSTA UDF 1.02 .. 2.60).
//
// Supports what DVD and data discs use: the anchor at sector 256 (or at the
// end of the image), the main volume descriptor sequence, physical,
// sparable (read as physical, no defect remapping) and metadata partitions,
// the file set descriptor, file entries and extended file entries with
// short, long, extended and embedded (inline) allocation descriptors,
// allocation extent continuations, file identifier descriptors with OSTA
// CS0 names (8-bit and 16-bit), permissions, modification times with their
// zone offset and symbolic links. Virtual (VAT) partitions of incrementally
// written discs and named streams are not read. Only 2048-byte blocks.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "families/archive/formats/iso_internal.hpp"
#include "util/text.hpp"

namespace cvt::archive::iso {

namespace {

constexpr std::size_t max_depth = 512;
constexpr int max_ad_hops = 4096;

std::uint32_t le32(ByteView d, std::size_t at) {
  return std::to_integer<std::uint32_t>(d[at]) | (std::to_integer<std::uint32_t>(d[at + 1]) << 8) |
         (std::to_integer<std::uint32_t>(d[at + 2]) << 16) | (std::to_integer<std::uint32_t>(d[at + 3]) << 24);
}
std::uint16_t le16(ByteView d, std::size_t at) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(d[at]) | (std::to_integer<unsigned>(d[at + 1]) << 8));
}
std::uint64_t le64(ByteView d, std::size_t at) {
  return static_cast<std::uint64_t>(le32(d, at)) | (static_cast<std::uint64_t>(le32(d, at + 4)) << 32);
}
std::uint8_t u8(ByteView d, std::size_t at) { return std::to_integer<std::uint8_t>(d[at]); }

// Descriptor tag: identifier and a valid checksum.
bool tag_ok(ByteView d, std::uint16_t ident) {
  if (d.size() < 16 || le16(d, 0) != ident) return false;
  unsigned sum = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    if (i != 4) sum += u8(d, i);
  }
  return (sum & 0xFFu) == u8(d, 4);
}

// 12-byte ECMA-167 timestamp.
std::int64_t udf_time(ByteView t) {
  const std::uint16_t type_tz = le16(t, 0);
  int offset = 0;
  if ((type_tz >> 12) == 1) {
    int tz = type_tz & 0x0FFF;
    if (tz >= 0x800) tz -= 0x1000;
    if (tz != -2047 && tz >= -1440 && tz <= 1440) offset = tz;
  }
  const auto year = static_cast<std::int16_t>(le16(t, 2));
  if (year == 0) return 0;
  return unix_time(year, u8(t, 4), u8(t, 5), u8(t, 6), u8(t, 7), u8(t, 8), offset);
}

// OSTA compressed unicode (CS0).
std::string cs0(ByteView d) {
  if (d.empty()) return {};
  const std::uint8_t id = u8(d, 0);
  const ByteView body = d.subspan(1);
  if (id == 16 || id == 255) {
    std::u16string wide;
    for (std::size_t i = 0; i + 1 < body.size(); i += 2) {
      wide.push_back(static_cast<char16_t>((std::to_integer<unsigned>(body[i]) << 8) | std::to_integer<unsigned>(body[i + 1])));
    }
    return util::utf16_to_utf8(wide);
  }
  std::string out;
  for (const std::byte b : body) util::append_utf8(out, static_cast<char32_t>(std::to_integer<unsigned>(b)));
  return out;
}

struct LongAd {
  std::uint32_t length = 0;  // with the type in the top two bits
  std::uint32_t block = 0;
  std::uint16_t partition = 0;
};

LongAd long_ad(ByteView d, std::size_t at) { return {le32(d, at), le32(d, at + 4), le16(d, at + 8)}; }

struct PartitionMap {
  bool metadata = false;
  std::uint16_t number = 0;          // partition number of the physical partition behind it
  std::uint32_t metadata_file = 0;   // metadata partitions: file entry location
};

struct Icb {
  std::uint8_t file_type = 0;
  std::uint16_t ad_type = 0;
  std::uint64_t size = 0;
  std::uint32_t permissions = 0;
  std::int64_t mtime = 0;
  ByteView ads;
  std::uint16_t partition = 0;  // partition reference the ICB lives in (short ADs refer to it)
};

class Udf {
 public:
  Udf(ByteView image, EntrySink& sink, const Context& ctx) : image_(image), sink_(sink), ctx_(ctx) {}

  Result<void> run() {
    CVT_TRY(read_volume());
    CVT_TRY_ASSIGN(const ByteView fsd, block(fsd_.partition, fsd_.block));
    if (!tag_ok(fsd, 256)) return fail(Status::Decode, "UDF file set descriptor is damaged");
    const LongAd root = long_ad(fsd, 400);
    struct Dir {
      LongAd icb;
      std::string path;
      std::size_t depth;
    };
    std::vector<Dir> stack;
    stack.push_back({root, std::string(), 0});
    std::set<std::pair<std::uint16_t, std::uint32_t>> seen;
    std::size_t steps = 0;
    while (!stack.empty()) {
      Dir dir = std::move(stack.back());
      stack.pop_back();
      if ((++steps & 63u) == 0) CVT_TRY(ctx_.checkpoint(0.5));
      if (dir.depth > max_depth) return fail(Status::Decode, "UDF directory tree is nested too deeply");
      if (!seen.insert({dir.icb.partition, dir.icb.block}).second) continue;
      CVT_TRY_ASSIGN(const Icb icb, read_icb(dir.icb));
      if (icb.file_type != 4) return fail(Status::Decode, "UDF directory entry is not a directory");
      if (icb.size > image_.size()) return fail(Status::Decode, "UDF directory is larger than the image");
      CVT_TRY_ASSIGN(const Bytes listing, content(icb, image_.size()));
      std::size_t pos = 0;
      while (pos + 38 <= listing.size()) {
        const ByteView fid(listing.data() + pos, listing.size() - pos);
        if (!tag_ok(fid, 257)) return fail(Status::Decode, "UDF file identifier is damaged");
        const std::uint8_t characteristics = u8(fid, 18);
        const std::size_t name_len = u8(fid, 19);
        const LongAd child = long_ad(fid, 20);
        const std::size_t iu_len = le16(fid, 36);
        const std::size_t total = (38 + iu_len + name_len + 3) / 4 * 4;
        if (38 + iu_len + name_len > fid.size()) return fail(Status::Decode, "UDF file identifier is truncated");
        pos += total;
        if ((characteristics & 0x0Cu) != 0) continue;  // deleted or parent
        const std::string name = cs0(fid.subspan(38 + iu_len, name_len));
        if (name.empty()) continue;
        std::string path = dir.path.empty() ? name : dir.path + "/" + name;
        CVT_TRY_ASSIGN(const Icb entry_icb, read_icb(child));
        ir::ArchiveEntry entry;
        entry.path = path;
        entry.mtime = entry_icb.mtime;
        const std::uint32_t p = entry_icb.permissions;
        entry.unix_mode = (((p >> 10) & 7u) << 6) | (((p >> 5) & 7u) << 3) | (p & 7u);
        if (entry_icb.file_type == 4) {
          entry.is_directory = true;
          CVT_TRY(sink_.add(std::move(entry)));
          stack.push_back({child, std::move(path), dir.depth + 1});
        } else if (entry_icb.file_type == 12) {
          if (entry_icb.size > 65536) return fail(Status::Decode, "UDF symbolic link is too long");
          CVT_TRY_ASSIGN(const Bytes target, content(entry_icb, 65536));
          entry.symlink_target = link_target(target);
          if (entry.symlink_target.empty()) continue;
          CVT_TRY(sink_.add(std::move(entry)));
        } else if (entry_icb.file_type == 5 || entry_icb.file_type == 0) {
          if (entry_icb.size > sink_.budget()) {
            return fail(Status::Limit, "UDF file of {} bytes exceeds the limit", entry_icb.size);
          }
          CVT_TRY_ASSIGN(entry.bytes, content(entry_icb, sink_.budget()));
          CVT_TRY(sink_.charge(entry.bytes.size()));
          CVT_TRY(sink_.add(std::move(entry)));
        } else {
          ctx_.warn("Special files (devices, FIFOs, streams) on the disc were skipped.");
        }
      }
    }
    return {};
  }

 private:
  Result<ByteView> sector_at(std::uint64_t index) const {
    if (index >= image_.size() / sector) return fail(Status::Decode, "UDF image is truncated");
    return image_.subspan(static_cast<std::size_t>(index) * sector, sector);
  }

  Result<void> read_volume() {
    // Anchor volume descriptor pointer.
    std::optional<ByteView> anchor;
    const std::uint64_t count = image_.size() / sector;
    for (const std::uint64_t at : {std::uint64_t{256}, count >= 1 ? count - 1 : 0, count >= 257 ? count - 257 : 0}) {
      if (at >= count || at < 16) continue;
      const ByteView d = image_.subspan(static_cast<std::size_t>(at) * sector, sector);
      if (tag_ok(d, 2)) {
        anchor = d;
        break;
      }
    }
    if (!anchor) return fail(Status::Decode, "UDF anchor volume descriptor is missing");
    std::uint64_t length = le32(*anchor, 16);
    std::uint64_t location = le32(*anchor, 20);
    std::map<std::uint16_t, std::uint32_t> partition_start;
    std::optional<ByteView> lvd;
    bool done = false;
    for (int hops = 0; hops < 16 && !done; ++hops) {
      const std::uint64_t sectors = std::min<std::uint64_t>(length / sector, 4096);
      std::optional<std::pair<std::uint64_t, std::uint64_t>> next;
      for (std::uint64_t i = 0; i < sectors && !done; ++i) {
        CVT_TRY_ASSIGN(const ByteView d, sector_at(location + i));
        const std::uint16_t ident = le16(d, 0);
        if (!tag_ok(d, ident)) break;
        if (ident == 5) {
          partition_start[le16(d, 22)] = le32(d, 188);
        } else if (ident == 6) {
          lvd = d;
        } else if (ident == 3) {
          next = std::make_pair(le32(d, 20), le32(d, 24));
          break;
        } else if (ident == 8) {
          done = true;
        }
      }
      if (!next) break;
      length = next->first;
      location = next->second;
    }
    if (!lvd) return fail(Status::Decode, "UDF logical volume descriptor is missing");
    if (le32(*lvd, 212) != sector) return fail(Status::Decode, "UDF volumes with {}-byte blocks are not supported", le32(*lvd, 212));
    fsd_ = long_ad(*lvd, 248);
    const std::uint32_t map_table = le32(*lvd, 264);
    const std::uint32_t map_count = le32(*lvd, 268);
    if (map_table > sector - 440 || map_count > 64) return fail(Status::Decode, "UDF partition maps are damaged");
    std::size_t at = 440;
    for (std::uint32_t i = 0; i < map_count; ++i) {
      if (at + 2 > 440 + map_table) return fail(Status::Decode, "UDF partition maps are damaged");
      const std::uint8_t type = u8(*lvd, at);
      const std::size_t len = u8(*lvd, at + 1);
      if (len < 2 || at + len > 440 + map_table) return fail(Status::Decode, "UDF partition maps are damaged");
      PartitionMap map;
      if (type == 1 && len >= 6) {
        map.number = le16(*lvd, at + 4);
      } else if (type == 2 && len >= 64) {
        const std::string_view id = as_chars(lvd->subspan(at + 5, 23));
        map.number = le16(*lvd, at + 38);
        if (id.starts_with("*UDF Metadata Partition")) {
          map.metadata = true;
          map.metadata_file = le32(*lvd, at + 40);
        } else if (!id.starts_with("*UDF Sparable Partition")) {
          return fail(Status::Decode, "UDF '{}' partitions (incrementally written discs) are not supported",
                      util::sanitize_utf8(std::string(id.substr(0, id.find('\0')))));
        }
      } else {
        return fail(Status::Decode, "UDF partition map type {} is not known", type);
      }
      maps_.push_back(map);
      at += len;
    }
    for (const PartitionMap& map : maps_) {
      const auto found = partition_start.find(map.number);
      if (found == partition_start.end()) return fail(Status::Decode, "UDF partition {} is missing", map.number);
      starts_.push_back(found->second);
    }
    // Metadata partitions: load the metadata file, through which their blocks are addressed.
    for (std::size_t i = 0; i < maps_.size(); ++i) {
      if (!maps_[i].metadata) continue;
      std::optional<std::uint16_t> physical;
      for (std::size_t j = 0; j < maps_.size(); ++j) {
        if (!maps_[j].metadata && maps_[j].number == maps_[i].number) physical = static_cast<std::uint16_t>(j);
      }
      if (!physical) return fail(Status::Decode, "UDF metadata partition has no physical partition");
      CVT_TRY_ASSIGN(const Icb icb, read_icb({0, maps_[i].metadata_file, *physical}));
      if (icb.size > image_.size()) return fail(Status::Decode, "UDF metadata file is larger than the image");
      CVT_TRY_ASSIGN(Bytes data, content(icb, image_.size()));
      metadata_.emplace(static_cast<std::uint16_t>(i), std::move(data));
    }
    return {};
  }

  // One logical block of a partition (2048 bytes).
  Result<ByteView> block(std::uint16_t partition, std::uint32_t lb) const {
    if (partition >= maps_.size()) return fail(Status::Decode, "UDF partition reference {} is not known", partition);
    if (maps_[partition].metadata) {
      const Bytes& meta = metadata_.at(partition);
      const std::uint64_t at = static_cast<std::uint64_t>(lb) * sector;
      if (at + sector > meta.size()) return fail(Status::Decode, "UDF metadata block {} is out of range", lb);
      return ByteView(meta).subspan(static_cast<std::size_t>(at), sector);
    }
    return sector_at(static_cast<std::uint64_t>(starts_[partition]) + lb);
  }

  // `length` bytes starting at a logical block (contiguous on the medium).
  Result<void> read_extent(std::uint16_t partition, std::uint32_t lb, std::uint64_t length, Bytes& out) const {
    if (partition >= maps_.size()) return fail(Status::Decode, "UDF partition reference {} is not known", partition);
    if (maps_[partition].metadata) {
      const Bytes& meta = metadata_.at(partition);
      const std::uint64_t at = static_cast<std::uint64_t>(lb) * sector;
      if (at > meta.size() || length > meta.size() - at) return fail(Status::Decode, "UDF extent is out of range");
      out.insert(out.end(), meta.begin() + static_cast<std::ptrdiff_t>(at), meta.begin() + static_cast<std::ptrdiff_t>(at + length));
      return {};
    }
    const std::uint64_t at = (static_cast<std::uint64_t>(starts_[partition]) + lb) * sector;
    if (at > image_.size() || length > image_.size() - at) return fail(Status::Decode, "UDF image is truncated");
    append(out, image_.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(length)));
    return {};
  }

  Result<Icb> read_icb(const LongAd& ad) const {
    CVT_TRY_ASSIGN(const ByteView d, block(ad.partition, ad.block));
    Icb icb;
    icb.partition = ad.partition;
    std::size_t ea_at = 0;
    if (tag_ok(d, 261)) {
      icb.size = le64(d, 56);
      icb.mtime = udf_time(d.subspan(84, 12));
      ea_at = 168;
    } else if (tag_ok(d, 266)) {
      icb.size = le64(d, 56);
      icb.mtime = udf_time(d.subspan(92, 12));
      ea_at = 208;
    } else {
      return fail(Status::Decode, "UDF file entry is damaged");
    }
    icb.file_type = u8(d, 27);
    icb.ad_type = le16(d, 34) & 7u;
    icb.permissions = le32(d, 44);
    const std::uint64_t ea_len = le32(d, ea_at);
    const std::uint64_t ad_len = le32(d, ea_at + 4);
    const std::uint64_t ads = ea_at + 8 + ea_len;
    if (ads > sector || ad_len > sector - ads) return fail(Status::Decode, "UDF file entry is damaged");
    icb.ads = d.subspan(static_cast<std::size_t>(ads), static_cast<std::size_t>(ad_len));
    return icb;
  }

  // The content of a file or directory.
  Result<Bytes> content(const Icb& icb, std::uint64_t limit) const {
    if (icb.size > limit) return fail(Status::Limit, "UDF file of {} bytes exceeds the limit", icb.size);
    Bytes out;
    if (icb.ad_type == 3) {
      if (icb.size > icb.ads.size()) return fail(Status::Decode, "UDF embedded file is truncated");
      append(out, icb.ads.first(static_cast<std::size_t>(icb.size)));
      return out;
    }
    if (icb.ad_type > 2) return fail(Status::Decode, "UDF allocation descriptor type {} is not known", icb.ad_type);
    out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(icb.size, image_.size())));
    const std::size_t ad_size = icb.ad_type == 0 ? 8 : (icb.ad_type == 1 ? 16 : 20);
    ByteView ads = icb.ads;
    int hops = 0;
    std::size_t pos = 0;
    while (out.size() < icb.size) {
      if (pos + ad_size > ads.size()) break;
      const std::uint32_t raw = le32(ads, pos);
      const std::uint32_t type = raw >> 30;
      const std::uint64_t length = raw & 0x3FFFFFFFu;
      std::uint32_t lb = 0;
      std::uint16_t partition = icb.partition;
      if (icb.ad_type == 0) {
        lb = le32(ads, pos + 4);
      } else if (icb.ad_type == 1) {
        lb = le32(ads, pos + 4);
        partition = le16(ads, pos + 8);
      } else {
        lb = le32(ads, pos + 12);
        partition = le16(ads, pos + 16);
      }
      pos += ad_size;
      if (length == 0) break;
      if (type == 3) {
        // The allocation descriptors continue in an allocation extent descriptor.
        if (++hops > max_ad_hops) return fail(Status::Decode, "UDF allocation chain is too long");
        CVT_TRY_ASSIGN(const ByteView next, block(partition, lb));
        if (!tag_ok(next, 258)) return fail(Status::Decode, "UDF allocation extent is damaged");
        const std::uint32_t len = le32(next, 20);
        if (len > sector - 24) return fail(Status::Decode, "UDF allocation extent is damaged");
        ads = next.subspan(24, len);
        pos = 0;
        continue;
      }
      const std::uint64_t take = std::min<std::uint64_t>(length, icb.size - out.size());
      if (type == 0) {
        CVT_TRY(read_extent(partition, lb, take, out));
      } else {
        if (take > image_.size()) return fail(Status::Decode, "UDF sparse extent is implausibly large");
        out.insert(out.end(), static_cast<std::size_t>(take), std::byte{0});
      }
    }
    if (out.size() != icb.size) return fail(Status::Decode, "UDF file is shorter than its recorded size");
    return out;
  }

  static std::string link_target(ByteView d) {
    std::string out;
    std::size_t pos = 0;
    while (pos + 4 <= d.size()) {
      const std::uint8_t type = u8(d, pos);
      const std::size_t len = u8(d, pos + 1);
      if (pos + 4 + len > d.size()) return {};
      const ByteView id = d.subspan(pos + 4, len);
      pos += 4 + len;
      std::string part;
      if (type == 1 || type == 2) {
        out = "/";
        continue;
      }
      if (type == 3) {
        part = "..";
      } else if (type == 4) {
        part = ".";
      } else if (type == 5) {
        part = cs0(id);
      } else {
        return {};
      }
      if (!out.empty() && out.back() != '/') out += '/';
      out += part;
    }
    return out;
  }

  ByteView image_;
  EntrySink& sink_;
  const Context& ctx_;
  LongAd fsd_;
  std::vector<PartitionMap> maps_;
  std::vector<std::uint32_t> starts_;
  std::map<std::uint16_t, Bytes> metadata_;
};

}  // namespace

Result<void> read_udf(ByteView image, EntrySink& sink, const Context& ctx) {
  Udf udf(image, sink, ctx);
  return udf.run();
}

}  // namespace cvt::archive::iso
