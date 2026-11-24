// Convertible core - archive family: HFS+ / HFSX volumes (Apple Technical Note TN1150), read only.
//
// The volume header at byte 1024 describes the special files as forks (eight
// extents each, more in the extents-overflow B-tree). The catalog B-tree
// holds one record per folder and file keyed by (parent id, name); the leaf
// nodes are chained, so a walk along that chain sees every record. Paths are
// built from the parent ids. Data forks are read through their extents;
// resource forks and extended attributes are ignored, except that files
// stored with HFS compression (com.apple.decmpfs, zlib inline or in the
// resource fork) are expanded. File hard links ("hlnk"/"hfs+" files) point to
// "iNode<n>" files in the private metadata folder; folder hard links are
// skipped. The private folders, the journal files and the usual system
// folders at the root (.Trashes, .fseventsd, .Spotlight-V100, ...) are not
// extracted. Every node, extent and size is checked; loops in the leaf chain
// or the folder hierarchy are refused.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/archive/formats/hfs_plus.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "families/archive/compression.hpp"
#include "util/text.hpp"

namespace cvt::archive::hfs {

namespace {

namespace cz = compression;

constexpr std::int64_t hfs_epoch = 2082844800;  // 1904-01-01 -> 1970-01-01
constexpr std::uint32_t root_folder = 2;
constexpr std::uint32_t uf_compressed = 0x20;
constexpr std::uint32_t s_ifmt = 0170000;
constexpr std::uint32_t s_iflnk = 0120000;
constexpr std::uint32_t s_ifreg = 0100000;
constexpr int max_depth = 512;

std::uint16_t be16(ByteView d, std::size_t at) {
  return static_cast<std::uint16_t>((std::to_integer<unsigned>(d[at]) << 8) | std::to_integer<unsigned>(d[at + 1]));
}
std::uint32_t be32(ByteView d, std::size_t at) {
  return (static_cast<std::uint32_t>(be16(d, at)) << 16) | be16(d, at + 2);
}
std::uint64_t be64(ByteView d, std::size_t at) {
  return (static_cast<std::uint64_t>(be32(d, at)) << 32) | be32(d, at + 4);
}
std::uint32_t le32(ByteView d, std::size_t at) {
  std::uint32_t v = 0;
  for (std::size_t i = 4; i-- > 0;) v = (v << 8) | std::to_integer<std::uint32_t>(d[at + i]);
  return v;
}
std::uint64_t le64(ByteView d, std::size_t at) {
  return static_cast<std::uint64_t>(le32(d, at)) | (static_cast<std::uint64_t>(le32(d, at + 4)) << 32);
}

struct Extent {
  std::uint32_t start = 0;
  std::uint32_t count = 0;
};

struct Fork {
  std::uint64_t logical = 0;
  std::uint32_t blocks = 0;
  std::array<Extent, 8> extents{};
};

Fork parse_fork(ByteView d, std::size_t at) {
  Fork f;
  f.logical = be64(d, at);
  f.blocks = be32(d, at + 12);
  for (std::size_t i = 0; i < 8; ++i) f.extents[i] = {be32(d, at + 16 + 8 * i), be32(d, at + 20 + 8 * i)};
  return f;
}

std::u16string utf16_name(ByteView d, std::size_t at, std::size_t chars) {
  std::u16string s(chars, u'\0');
  for (std::size_t i = 0; i < chars; ++i) s[i] = static_cast<char16_t>(be16(d, at + 2 * i));
  return s;
}

class Volume {
 public:
  Volume(Device& dev, std::uint32_t block_size, std::uint32_t total_blocks, const Context& ctx)
      : dev_(&dev), block_size_(block_size), total_blocks_(total_blocks), ctx_(&ctx) {}

  // Every extent of a fork, inline ones first, then the overflow records.
  Result<std::vector<Extent>> extents(const Fork& fork, std::uint32_t cnid, std::uint8_t type) const {
    std::vector<Extent> out;
    std::uint64_t covered = 0;
    for (const Extent& e : fork.extents) {
      if (e.count == 0) break;
      out.push_back(e);
      covered += e.count;
    }
    std::size_t guard = 0;
    while (covered < fork.blocks) {
      const auto found = overflow_.find({cnid, type, static_cast<std::uint32_t>(std::min<std::uint64_t>(covered, 0xFFFFFFFFu))});
      if (found == overflow_.end() || ++guard > 100000) return fail(Status::Decode, "HFS+ fork is missing extents");
      std::uint64_t added = 0;
      for (const Extent& e : found->second) {
        if (e.count == 0) break;
        out.push_back(e);
        added += e.count;
      }
      if (added == 0) return fail(Status::Decode, "HFS+ extent record is empty");
      covered += added;
    }
    for (const Extent& e : out) {
      if (e.start > total_blocks_ || e.count > total_blocks_ - e.start) return fail(Status::Decode, "HFS+ extent lies outside the volume");
    }
    if (fork.logical > covered * block_size_) return fail(Status::Decode, "HFS+ fork is larger than its extents");
    return out;
  }

  // Read `out.size()` bytes at `offset` of a fork with extents `ext`.
  Result<void> read(const std::vector<Extent>& ext, std::uint64_t offset, std::span<std::byte> out) {
    std::uint64_t base = 0;
    std::size_t done = 0;
    for (const Extent& e : ext) {
      const std::uint64_t len = static_cast<std::uint64_t>(e.count) * block_size_;
      if (done < out.size() && offset + done < base + len) {
        const std::uint64_t within = offset + done - base;
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size() - done, len - within));
        CVT_TRY(dev_->read(static_cast<std::uint64_t>(e.start) * block_size_ + within, out.subspan(done, n)));
        done += n;
      }
      base += len;
      if (done == out.size()) return {};
    }
    if (done != out.size()) return fail(Status::Decode, "HFS+ read beyond the end of a fork");
    return {};
  }

  // Visit every record of every leaf node of a B-tree stored in a fork.
  Result<void> leaf_records(const Fork& fork, std::uint32_t cnid, const std::function<Result<void>(ByteView)>& visit) {
    CVT_TRY_ASSIGN(const auto ext, extents(fork, cnid, 0));
    if (fork.logical < 512) return fail(Status::Decode, "HFS+ B-tree is too small");
    Bytes head(512);
    CVT_TRY(read(ext, 0, head));
    const std::uint32_t first_leaf = be32(head, 14 + 10);
    const std::uint16_t node_size = be16(head, 14 + 18);
    const std::uint32_t total_nodes = be32(head, 14 + 22);
    if (node_size < 512 || node_size > 32768 || (node_size & (node_size - 1)) != 0) {
      return fail(Status::Decode, "HFS+ B-tree node size is invalid");
    }
    const std::uint64_t nodes = std::min<std::uint64_t>(total_nodes, fork.logical / node_size);
    Bytes node(node_size);
    std::set<std::uint32_t> seen;
    for (std::uint32_t n = first_leaf; n != 0;) {
      if (n >= nodes || !seen.insert(n).second) return fail(Status::Decode, "HFS+ B-tree leaf chain is damaged");
      CVT_TRY(read(ext, static_cast<std::uint64_t>(n) * node_size, node));
      if (node[8] != std::byte{0xFF}) return fail(Status::Decode, "HFS+ B-tree leaf chain reaches a non-leaf node");
      const std::size_t records = be16(node, 10);
      if (14 + 2 * (records + 1) > node_size) return fail(Status::Decode, "HFS+ B-tree node is damaged");
      const std::size_t limit = node_size - 2 * (records + 1);
      for (std::size_t r = 0; r < records; ++r) {
        const std::size_t start = be16(node, node_size - 2 * (r + 1));
        const std::size_t end = be16(node, node_size - 2 * (r + 2));
        if (start < 14 || end <= start || end > limit) return fail(Status::Decode, "HFS+ B-tree record is damaged");
        CVT_TRY(visit(ByteView(node).subspan(start, end - start)));
      }
      if ((seen.size() & 63u) == 0) CVT_TRY(ctx_->checkpoint(0.3));
      n = be32(node, 0);
    }
    return {};
  }

  void add_overflow(std::uint32_t cnid, std::uint8_t type, std::uint32_t start, const std::array<Extent, 8>& ext) {
    overflow_[{cnid, type, start}] = ext;
  }

  [[nodiscard]] std::uint32_t block_size() const noexcept { return block_size_; }

 private:
  Device* dev_;
  std::uint32_t block_size_;
  std::uint32_t total_blocks_;
  const Context* ctx_;
  std::map<std::tuple<std::uint32_t, std::uint8_t, std::uint32_t>, std::array<Extent, 8>> overflow_;
};

struct Item {
  std::uint32_t parent = 0;
  std::string name;
  bool folder = false;
  std::uint32_t mode = 0;
  std::uint32_t owner_flags = 0;
  std::uint32_t special = 0;  // iNode number of a hard link
  std::int64_t mtime = 0;
  std::uint32_t type = 0;     // Finder type and creator
  std::uint32_t creator = 0;
  Fork data;
  Fork rsrc;
};

constexpr std::uint32_t fourcc(const char (&s)[5]) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 8) | static_cast<unsigned char>(s[3]);
}

bool skipped_at_root(const std::string& name) {
  static constexpr std::string_view names[] = {
      std::string_view("\0\0\0\0HFS+ Private Data", 21), ".HFS+ Private Directory Data\r", ".journal", ".journal_info_block",
      ".Trashes", ".fseventsd", ".Spotlight-V100", ".TemporaryItems", ".DocumentRevisions-V100", ".HFS+ Private Directory Data"};
  return std::find(std::begin(names), std::end(names), std::string_view(name)) != std::end(names);
}

class Reader {
 public:
  Reader(Volume& volume, EntrySink& sink, const Context& ctx) : vol_(volume), sink_(sink), ctx_(ctx) {}

  Result<void> catalog(const Fork& fork) {
    return vol_.leaf_records(fork, 4, [this](ByteView rec) -> Result<void> {
      if (rec.size() < 8) return fail(Status::Decode, "HFS+ catalog record is damaged");
      const std::size_t key_len = be16(rec, 0);
      if (key_len < 6 || key_len + 2 + 2 > rec.size()) return fail(Status::Decode, "HFS+ catalog key is damaged");
      const std::uint32_t parent = be32(rec, 2);
      const std::size_t chars = be16(rec, 6);
      if (8 + 2 * chars > key_len + 2) return fail(Status::Decode, "HFS+ catalog name is damaged");
      const ByteView data = rec.subspan(2 + key_len);
      const std::uint16_t type = be16(data, 0);
      if (type != 1 && type != 2) return {};  // thread records
      if (items_.size() >= ctx_.limits.max_archive_entries + 64) {
        return fail(Status::Limit, "the disk image has more than {} entries", ctx_.limits.max_archive_entries);
      }
      if (data.size() < (type == 1 ? 88u : 248u)) return fail(Status::Decode, "HFS+ catalog record is truncated");
      Item item;
      item.parent = parent;
      item.name = name_to_utf8(utf16_name(rec, 8, chars));
      item.folder = type == 1;
      const std::uint32_t id = be32(data, 8);
      const std::uint32_t mod = be32(data, 16);
      item.mtime = mod == 0 ? 0 : static_cast<std::int64_t>(mod) - hfs_epoch;
      item.owner_flags = std::to_integer<std::uint32_t>(data[41]);
      item.mode = be16(data, 42);
      item.special = be32(data, 44);
      item.type = be32(data, 48);
      item.creator = be32(data, 52);
      if (!item.folder) {
        item.data = parse_fork(data, 88);
        item.rsrc = parse_fork(data, 168);
        if (item.owner_flags & uf_compressed) any_compressed_ = true;
      }
      if (parent == root_folder && skipped_at_root(item.name)) {
        if (item.name.starts_with(std::string_view("\0\0\0\0HFS+", 8))) private_files_ = id;
        hidden_.insert(id);
      }
      if (id < 16 && id != root_folder) return {};  // reserved ids
      items_[id] = std::move(item);
      return {};
    });
  }

  Result<void> attributes(const Fork& fork) {
    if (!any_compressed_ || fork.logical == 0) return {};
    return vol_.leaf_records(fork, 8, [this](ByteView rec) -> Result<void> {
      if (rec.size() < 14) return {};
      const std::size_t key_len = be16(rec, 0);
      if (key_len < 12 || key_len + 2 > rec.size()) return {};
      const std::uint32_t file = be32(rec, 4);
      const std::size_t chars = be16(rec, 12);
      if (14 + 2 * chars > key_len + 2) return {};
      if (name_to_utf8(utf16_name(rec, 14, chars)) != "com.apple.decmpfs") return {};
      const ByteView data = rec.subspan(2 + key_len);
      if (data.size() < 16 || be32(data, 0) != 0x10) return {};  // only inline attribute data
      const std::uint32_t size = be32(data, 12);
      if (size > data.size() - 16) return fail(Status::Decode, "HFS+ attribute record is damaged");
      decmpfs_[file] = to_bytes(data.subspan(16, size));
      return {};
    });
  }

  Result<void> emit() {
    std::vector<std::pair<std::string, std::uint32_t>> order;
    for (const auto& [id, item] : items_) {
      if (id == root_folder) continue;
      const auto path = path_of(id);
      if (!path) continue;
      order.emplace_back(*path, id);
    }
    std::sort(order.begin(), order.end());
    for (const auto& [id, item] : items_) {
      if (private_files_ != 0 && item.parent == private_files_ && item.name.starts_with("iNode")) {
        const auto number = util::parse_int(std::string_view(item.name).substr(5));
        if (number && *number >= 0 && *number <= 0xFFFFFFFF) inodes_[static_cast<std::uint32_t>(*number)] = id;
      }
    }
    std::size_t done = 0;
    for (const auto& [path, id] : order) {
      if ((++done & 63u) == 0) CVT_TRY(ctx_.checkpoint(0.5 + 0.5 * static_cast<double>(done) / static_cast<double>(order.size())));
      CVT_TRY(entry(path, id, items_.at(id)));
    }
    return {};
  }

 private:
  // Path of an item, or nullopt for items that are hidden or unreachable.
  std::optional<std::string> path_of(std::uint32_t id) {
    std::vector<std::uint32_t> chain;
    std::uint32_t cur = id;
    while (cur != root_folder) {
      if (hidden_.contains(cur) || chain.size() > max_depth) return std::nullopt;
      const auto it = items_.find(cur);
      if (it == items_.end()) return std::nullopt;
      if (cur != id && !it->second.folder) return std::nullopt;
      chain.push_back(cur);
      cur = it->second.parent;
    }
    std::string path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if (!path.empty()) path += '/';
      path += items_.at(*it).name;
    }
    return path;
  }

  Result<Bytes> read_fork(const Fork& fork, std::uint32_t cnid, std::uint8_t type, std::uint64_t max) {
    if (fork.logical > max) return fail(Status::Limit, "a file in the disk image is larger than the size limit");
    CVT_TRY_ASSIGN(const auto ext, vol_.extents(fork, cnid, type));
    Bytes out(static_cast<std::size_t>(fork.logical));
    CVT_TRY(vol_.read(ext, 0, out));
    return out;
  }

  Result<std::optional<Bytes>> decompress_file(std::uint32_t id, const Item& item) {
    const auto found = decmpfs_.find(id);
    if (found == decmpfs_.end()) return fail(Status::Decode, "compressed HFS+ file '{}' has no decmpfs attribute", item.name);
    const ByteView h = found->second;
    if (h.size() < 16 || le32(h, 0) != 0x636D7066u) return fail(Status::Decode, "HFS+ decmpfs header is damaged");
    const std::uint32_t kind = le32(h, 4);
    const std::uint64_t size = le64(h, 8);
    CVT_TRY(sink_.charge(size));
    const auto inflate = [&](ByteView block, std::uint64_t limit) -> Result<Bytes> {
      if (!block.empty() && (std::to_integer<unsigned>(block[0]) & 0x0Fu) == 0x0Fu) {
        if (block.size() - 1 > limit) return fail(Status::Decode, "HFS+ compressed block is too large");
        return to_bytes(block.subspan(1));
      }
      cz::DecodeOptions o;
      o.max_output = limit;
      return cz::decompress(cz::Method::Zlib, block, o);
    };
    Bytes out;
    // LZVN data goes to the LZFSE decoder as one "bvxn" block.
    const auto unlzvn = [&](ByteView block, std::uint64_t limit) -> Result<Bytes> {
      if (!block.empty() && block[0] == std::byte{0x06}) {
        if (block.size() - 1 > limit) return fail(Status::Decode, "HFS+ compressed block is too large");
        return to_bytes(block.subspan(1));
      }
      if (!cz::can_decode(cz::Method::Lzfse)) {
        return fail(Status::Decode, "file '{}' uses LZVN HFS compression, which is not available in this build", item.name);
      }
      if (block.size() > 0xFFFFFFFFu || limit > 0xFFFFFFFFu) return fail(Status::Decode, "HFS+ compressed block is too large");
      Bytes stream;
      const auto put32 = [&stream](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) stream.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
      };
      append(stream, std::string_view("bvxn"));
      put32(static_cast<std::uint32_t>(limit));
      put32(static_cast<std::uint32_t>(block.size()));
      append(stream, block);
      append(stream, std::string_view("bvx$"));
      cz::DecodeOptions o;
      o.max_output = limit;
      return cz::decompress(cz::Method::Lzfse, stream, o);
    };
    if ((kind == 7 || kind == 8) && !cz::can_decode(cz::Method::Lzfse)) {
      if (!warned_lzvn_) ctx_.warn("Files stored with LZVN HFS compression were skipped (no LZVN decoder in this build).");
      warned_lzvn_ = true;
      return std::optional<Bytes>{};
    }
    if (kind == 3) {
      CVT_TRY_ASSIGN(out, inflate(h.subspan(16), size));
    } else if (kind == 7) {
      CVT_TRY_ASSIGN(out, unlzvn(h.subspan(16), size));
    } else if (kind == 4 || kind == 8) {
      CVT_TRY_ASSIGN(const Bytes rsrc, read_fork(item.rsrc, id, 0xFF, sink_.budget() + size + (64u << 20)));
      if (rsrc.size() < 16) return fail(Status::Decode, "HFS+ compressed resource fork is damaged");
      const std::uint64_t data_off = be32(rsrc, 0);
      if (data_off > rsrc.size() || rsrc.size() - data_off < 8) return fail(Status::Decode, "HFS+ compressed resource fork is damaged");
      const std::uint64_t table = data_off + 4;
      const ByteView fork = ByteView(rsrc);
      // zlib: count, then (offset, length) pairs from `table`; LZVN: offsets
      // from `table`, the first one being the size of the offset table.
      std::vector<std::pair<std::uint64_t, std::uint64_t>> blocks;
      if (kind == 4) {
        const std::uint64_t count = le32(fork, static_cast<std::size_t>(table));
        if (count > (rsrc.size() - table - 4) / 8) return fail(Status::Decode, "HFS+ compressed resource fork is damaged");
        for (std::uint64_t b = 0; b < count; ++b) {
          blocks.emplace_back(table + le32(fork, static_cast<std::size_t>(table + 4 + 8 * b)),
                              le32(fork, static_cast<std::size_t>(table + 8 + 8 * b)));
        }
      } else {
        const std::uint64_t table_size = le32(fork, static_cast<std::size_t>(table));
        if (table_size < 8 || table_size % 4 != 0 || table_size > rsrc.size() - table) {
          return fail(Status::Decode, "HFS+ compressed resource fork is damaged");
        }
        for (std::uint64_t b = 0; b + 1 < table_size / 4; ++b) {
          const std::uint64_t from = le32(fork, static_cast<std::size_t>(table + 4 * b));
          const std::uint64_t to = le32(fork, static_cast<std::size_t>(table + 4 * b + 4));
          if (to < from) return fail(Status::Decode, "HFS+ compressed resource fork is damaged");
          blocks.emplace_back(table + from, to - from);
        }
      }
      out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(size, sink_.budget() + size)));
      for (const auto& [off, len] : blocks) {
        if (off > rsrc.size() || len > rsrc.size() - off) return fail(Status::Decode, "HFS+ compressed block lies outside the fork");
        if (out.size() >= size && len != 0) return fail(Status::Decode, "HFS+ compressed file is longer than declared");
        const ByteView block = fork.subspan(static_cast<std::size_t>(off), static_cast<std::size_t>(len));
        const std::uint64_t limit = std::min<std::uint64_t>(65536, size - out.size());
        if (kind == 4) {
          CVT_TRY_ASSIGN(const Bytes part, inflate(block, limit));
          append(out, part);
        } else {
          CVT_TRY_ASSIGN(const Bytes part, unlzvn(block, limit));
          append(out, part);
        }
      }
    } else {
      if (!warned_kind_) ctx_.warn("Files stored with LZFSE HFS compression were skipped.");
      warned_kind_ = true;
      return std::optional<Bytes>{};
    }
    if (out.size() != size) return fail(Status::Decode, "HFS+ compressed file '{}' has the wrong size", item.name);
    return std::optional<Bytes>(std::move(out));
  }

  Result<void> entry(const std::string& path, std::uint32_t id, const Item& link) {
    ir::ArchiveEntry e;
    e.path = path;
    e.mtime = link.mtime;
    e.unix_mode = link.mode & 07777u;
    if (link.folder) {
      e.is_directory = true;
      if ((link.mode & s_ifmt) == 0) e.unix_mode = 0;
      return sink_.add(std::move(e));
    }
    const Item* item = &link;
    if (link.type == fourcc("hlnk") && link.creator == fourcc("hfs+")) {
      // File hard link: the content lives in the private folder.
      if (const auto found = inodes_.find(link.special); found != inodes_.end()) {
        id = found->second;
        item = &items_.at(id);
      }
      if (item == &link) {
        ctx_.warn("A hard link whose target is missing was skipped.");
        return {};
      }
      e.unix_mode = item->mode & 07777u;
    } else if (link.type == fourcc("fdrp") && link.creator == fourcc("MACS")) {
      if (!warned_dir_link_) ctx_.warn("Folder hard links were skipped.");
      warned_dir_link_ = true;
      return {};
    }
    const std::uint32_t fmt = item->mode & s_ifmt;
    if (fmt == s_iflnk) {
      if (item->data.logical == 0 || item->data.logical > 4096) return {};
      CVT_TRY_ASSIGN(const Bytes target, read_fork(item->data, id, 0, 4096));
      e.symlink_target = util::sanitize_utf8(as_chars(target));
      return sink_.add(std::move(e));
    }
    if (fmt != 0 && fmt != s_ifreg) {
      if (!warned_special_) ctx_.warn("Device files, FIFOs and sockets were skipped.");
      warned_special_ = true;
      return {};
    }
    if (item->owner_flags & uf_compressed) {
      CVT_TRY_ASSIGN(auto content, decompress_file(id, *item));
      if (!content) return {};
      e.bytes = std::move(*content);
    } else {
      CVT_TRY(sink_.charge(item->data.logical));
      CVT_TRY_ASSIGN(e.bytes, read_fork(item->data, id, 0, sink_.budget() + item->data.logical));
    }
    return sink_.add(std::move(e));
  }

  Volume& vol_;
  EntrySink& sink_;
  const Context& ctx_;
  std::map<std::uint32_t, Item> items_;
  std::set<std::uint32_t> hidden_;
  std::unordered_map<std::uint32_t, Bytes> decmpfs_;
  std::unordered_map<std::uint32_t, std::uint32_t> inodes_;
  std::uint32_t private_files_ = 0;
  bool any_compressed_ = false;
  bool warned_kind_ = false;
  bool warned_lzvn_ = false;
  bool warned_dir_link_ = false;
  bool warned_special_ = false;
};

}  // namespace

bool is_volume_header(ByteView h) noexcept {
  if (h.size() < 512) return false;
  const std::uint16_t sig = be16(h, 0);
  if (sig == 0x482B || sig == 0x4858) return be16(h, 2) == 4 || be16(h, 2) == 5;
  return sig == 0x4244 && be16(h, 0x7C) == 0x482B;  // HFS wrapper around HFS+
}

Result<void> read_volume(Device& volume, EntrySink& sink, const Context& ctx) {
  if (volume.size() < 1536) return fail(Status::Decode, "the HFS+ volume is truncated");
  Bytes h(512);
  CVT_TRY(volume.read(1024, h));
  std::uint64_t offset = 0;
  if (be16(h, 0) == 0x4244) {
    if (be16(h, 0x7C) != 0x482B) return fail(Status::Decode, "HFS (Mac OS Standard) volumes are not supported, only HFS+");
    // HFS wrapper: the embedded HFS+ volume starts at drAlBlSt * 512 + start * drAlBlkSiz.
    const std::uint64_t alloc_size = be32(h, 20);
    const std::uint64_t first = be16(h, 28);
    const std::uint64_t start = be16(h, 0x7E);
    offset = first * 512 + start * alloc_size;
    if (offset + 1536 > volume.size()) return fail(Status::Decode, "the embedded HFS+ volume lies outside the image");
    CVT_TRY(volume.read(offset + 1024, h));
  }
  Slice inner(volume, offset, volume.size() - offset);
  const std::uint16_t sig = be16(h, 0);
  if ((sig != 0x482B && sig != 0x4858) || (be16(h, 2) != 4 && be16(h, 2) != 5)) {
    return fail(Status::Decode, "no HFS+ volume header found");
  }
  const std::uint32_t attributes = be32(h, 4);
  if (attributes & 0x8000u) ctx.warn("The HFS+ volume is marked as software-locked.");
  const std::uint32_t block_size = be32(h, 40);
  const std::uint32_t total_blocks = be32(h, 44);
  if (block_size < 512 || block_size > (1u << 20) || (block_size & (block_size - 1)) != 0) {
    return fail(Status::Decode, "HFS+ block size {} is invalid", block_size);
  }
  if (static_cast<std::uint64_t>(total_blocks) * block_size > inner.size() + block_size) {
    ctx.warn("The HFS+ volume is larger than the disk image; it may be truncated.");
  }
  Volume vol(inner, block_size, total_blocks, ctx);
  const Fork extents_file = parse_fork(h, 192);
  const Fork catalog_file = parse_fork(h, 272);
  const Fork attributes_file = parse_fork(h, 352);
  if (extents_file.logical != 0) {
    CVT_TRY(vol.leaf_records(extents_file, 3, [&vol](ByteView rec) -> Result<void> {
      if (rec.size() < 12 + 64 || be16(rec, 0) != 10) return fail(Status::Decode, "HFS+ extents record is damaged");
      const std::uint8_t fork_type = std::to_integer<std::uint8_t>(rec[2]);
      const std::uint32_t file = be32(rec, 4);
      const std::uint32_t start = be32(rec, 8);
      std::array<Extent, 8> ext{};
      for (std::size_t i = 0; i < 8; ++i) ext[i] = {be32(rec, 12 + 8 * i), be32(rec, 16 + 8 * i)};
      vol.add_overflow(file, fork_type, start, ext);
      return {};
    }));
  }
  Reader reader(vol, sink, ctx);
  CVT_TRY(reader.catalog(catalog_file));
  CVT_TRY(reader.attributes(attributes_file));
  return reader.emit();
}

}  // namespace cvt::archive::hfs
