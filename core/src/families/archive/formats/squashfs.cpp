// Convertible core - archive family: SquashFS 4.0 images (read only).
//
// Layout (all little endian): a 96-byte superblock, then data and fragment
// blocks, then the metadata tables (inodes, directories, fragments, ids,
// export, xattrs). Metadata lives in blocks of at most 8 KiB, each preceded by
// a 16-bit header (bit 15 = stored uncompressed). Inode references are
// (block position relative to the inode table << 16 | offset in the block).
//
// Read: directories (basic and extended), regular files (block lists, tail
// fragments, sparse blocks, uncompressed blocks), symbolic links, hard links
// (each name gets a copy), permissions and modification times. Devices,
// FIFOs and sockets are skipped with a warning; owners and extended
// attributes are ignored. Compressors: gzip (zlib), lzma (LZMA_Alone), lzo
// (LZO1X), xz, lz4 (raw block) and zstd, through the family's codecs.
// Hostile images are bounded: directory cycles and repeated directories are
// refused, every size is checked against the image and the limits before
// anything is allocated, and every decompressed byte is accounted.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "util/bytes.hpp"
#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/formats.hpp"
#include "util/text.hpp"

namespace cvt::archive {

namespace {

namespace cz = compression;

constexpr std::uint32_t magic = 0x73717368;  // "hsqs"
constexpr std::size_t superblock_size = 96;
constexpr std::size_t metadata_size = 8192;
constexpr std::uint32_t no_fragment = 0xFFFFFFFFu;
constexpr int max_depth = 256;

std::uint16_t le16(ByteView d, std::size_t at) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(d[at]) | (std::to_integer<unsigned>(d[at + 1]) << 8));
}
std::uint32_t le32(ByteView d, std::size_t at) {
  return static_cast<std::uint32_t>(le16(d, at)) | (static_cast<std::uint32_t>(le16(d, at + 2)) << 16);
}
std::uint64_t le64(ByteView d, std::size_t at) {
  return static_cast<std::uint64_t>(le32(d, at)) | (static_cast<std::uint64_t>(le32(d, at + 4)) << 32);
}

struct Superblock {
  std::uint32_t inode_count = 0;
  std::uint32_t block_size = 0;
  std::uint32_t fragment_count = 0;
  std::uint16_t compressor = 0;
  std::uint16_t flags = 0;
  std::uint64_t root_inode = 0;
  std::uint64_t bytes_used = 0;
  std::uint64_t inode_table = 0;
  std::uint64_t directory_table = 0;
  std::uint64_t fragment_table = 0;
};

std::string_view compressor_name(std::uint16_t id) {
  switch (id) {
    case 1: return "gzip";
    case 2: return "lzma";
    case 3: return "lzo";
    case 4: return "xz";
    case 5: return "lz4";
    case 6: return "zstd";
    default: return "unknown";
  }
}

Result<cz::Method> compressor_method(std::uint16_t id) {
  switch (id) {
    case 1: return cz::Method::Zlib;
    case 2: return cz::Method::LzmaAlone;
    case 3: return cz::Method::Lzo1x;
    case 4: return cz::Method::Xz;
    case 5: return cz::Method::Lz4Block;
    case 6: return cz::Method::Zstd;
    default: return fail(Status::Decode, "SquashFS compressor {} is unknown", id);
  }
}

class Image {
 public:
  Image(ByteView data, const Superblock& sb, cz::Method method, EntrySink& sink, const Context& ctx)
      : data_(data), sb_(sb), method_(method), sink_(&sink), ctx_(&ctx) {}

  // Decompress `size` stored bytes at `pos` into at most `limit` bytes.
  Result<Bytes> block(std::uint64_t pos, std::uint32_t size, bool compressed, std::uint64_t limit) {
    if (pos > data_.size() || size > data_.size() - pos) return fail(Status::Decode, "SquashFS block lies outside the image");
    const ByteView stored = data_.subspan(static_cast<std::size_t>(pos), size);
    if (!compressed) {
      if (stored.size() > limit) return fail(Status::Decode, "SquashFS block is larger than the block size");
      return to_bytes(stored);
    }
    if (!cz::can_decode(method_)) {
      return fail(Status::Decode, "SquashFS {} compression is not available in this build", compressor_name(sb_.compressor));
    }
    cz::DecodeOptions d;
    d.max_output = std::min<std::uint64_t>(limit, sink_->budget());
    auto out = cz::decompress(method_, stored, d);
    if (!out) {
      if (out.error().code == Status::Limit && limit > sink_->budget()) return fail(Status::Limit, "SquashFS image expands beyond the size limit");
      if (out.error().code == Status::Limit) return fail(Status::Decode, "SquashFS block is larger than the block size");
      return std::unexpected(std::move(out).error());
    }
    work_ += out->size();
    if (work_ > work_limit()) return fail(Status::Limit, "SquashFS image needs too much decompression work");
    return out;
  }

  // One metadata block (cached by position). Returns the block and the
  // position of the next one.
  Result<std::pair<const Bytes*, std::uint64_t>> metadata(std::uint64_t pos) {
    if (const auto found = meta_.find(pos); found != meta_.end()) return std::make_pair(&found->second.first, found->second.second);
    if (pos > data_.size() || data_.size() - pos < 2) return fail(Status::Decode, "SquashFS metadata lies outside the image");
    const std::uint16_t header = le16(data_, static_cast<std::size_t>(pos));
    const std::uint32_t size = header & 0x7FFFu;
    const bool compressed = (header & 0x8000u) == 0;
    if (size == 0) return fail(Status::Decode, "SquashFS metadata block is empty");
    CVT_TRY_ASSIGN(Bytes plain, block(pos + 2, size, compressed, metadata_size));
    CVT_TRY(sink_->charge(plain.size()));
    if (meta_.size() > 65536) meta_.clear();
    auto [it, inserted] = meta_.emplace(pos, std::make_pair(std::move(plain), pos + 2 + size));
    return std::make_pair(&it->second.first, it->second.second);
  }

  // Read `count` bytes of a metadata stream starting at block `pos`, offset `offset`.
  class Stream {
   public:
    Stream(Image& image, std::uint64_t pos, std::size_t offset) : image_(&image), pos_(pos), offset_(offset) {}
    Result<void> read(std::byte* out, std::size_t count) {
      while (count > 0) {
        CVT_TRY_ASSIGN(const auto blk, image_->metadata(pos_));
        const Bytes& b = *blk.first;
        if (offset_ >= b.size()) {
          if (offset_ > b.size()) return fail(Status::Decode, "SquashFS metadata offset is out of range");
          pos_ = blk.second;
          offset_ = 0;
          if (++hops_ > 100000) return fail(Status::Decode, "SquashFS metadata stream is too long");
          continue;
        }
        const std::size_t n = std::min(count, b.size() - offset_);
        ::cvt::util::copy_bytes(out, b.data() + offset_, n);
        out += n;
        offset_ += n;
        count -= n;
      }
      return {};
    }
    Result<Bytes> bytes(std::size_t count) {
      Bytes out(count);
      CVT_TRY(read(out.data(), count));
      return out;
    }
    Result<std::uint16_t> u16() {
      std::byte b[2];
      CVT_TRY(read(b, 2));
      return le16(ByteView(b, 2), 0);
    }
    Result<std::uint32_t> u32() {
      std::byte b[4];
      CVT_TRY(read(b, 4));
      return le32(ByteView(b, 4), 0);
    }
    Result<std::uint64_t> u64() {
      std::byte b[8];
      CVT_TRY(read(b, 8));
      return le64(ByteView(b, 8), 0);
    }

   private:
    Image* image_;
    std::uint64_t pos_;
    std::size_t offset_;
    std::size_t hops_ = 0;
  };

  Stream inode_stream(std::uint64_t ref) {
    return Stream(*this, sb_.inode_table + (ref >> 16), static_cast<std::size_t>(ref & 0xFFFFu));
  }

  // The fragment table entry `index`: (position, stored size word).
  Result<std::pair<std::uint64_t, std::uint32_t>> fragment_entry(std::uint32_t index) {
    if (index >= sb_.fragment_count) return fail(Status::Decode, "SquashFS fragment index is out of range");
    const std::uint64_t byte = static_cast<std::uint64_t>(index) * 16;
    const std::uint64_t slot = sb_.fragment_table + byte / metadata_size * 8;
    if (slot > data_.size() || data_.size() - slot < 8) return fail(Status::Decode, "SquashFS fragment table lies outside the image");
    const std::uint64_t meta = le64(data_, static_cast<std::size_t>(slot));
    Stream s(*this, meta, static_cast<std::size_t>(byte % metadata_size));
    CVT_TRY_ASSIGN(const std::uint64_t start, s.u64());
    CVT_TRY_ASSIGN(const std::uint32_t size, s.u32());
    return std::make_pair(start, size);
  }

  // The decompressed fragment block `index` (a small LRU cache).
  Result<const Bytes*> fragment(std::uint32_t index) {
    for (auto it = fragments_.begin(); it != fragments_.end(); ++it) {
      if (it->first == index) {
        fragments_.splice(fragments_.begin(), fragments_, it);
        return &fragments_.front().second;
      }
    }
    CVT_TRY_ASSIGN(const auto entry, fragment_entry(index));
    const std::uint32_t stored = entry.second & 0xFFFFFFu;
    const bool compressed = (entry.second & (1u << 24)) == 0;
    CVT_TRY_ASSIGN(Bytes plain, block(entry.first, stored, compressed, sb_.block_size));
    fragments_.emplace_front(index, std::move(plain));
    if (fragments_.size() > 4) fragments_.pop_back();
    return &fragments_.front().second;
  }

  [[nodiscard]] const Superblock& sb() const noexcept { return sb_; }
  [[nodiscard]] EntrySink& sink() noexcept { return *sink_; }
  [[nodiscard]] const Context& ctx() const noexcept { return *ctx_; }
  [[nodiscard]] std::uint64_t size() const noexcept { return data_.size(); }

 private:
  [[nodiscard]] std::uint64_t work_limit() const noexcept {
    const std::uint64_t l = ctx_->limits.max_decompressed_bytes;
    return l > (std::uint64_t{1} << 62) ? l : 2 * l + (64u << 20);
  }

  ByteView data_;
  Superblock sb_;
  cz::Method method_;
  EntrySink* sink_;
  const Context* ctx_;
  std::map<std::uint64_t, std::pair<Bytes, std::uint64_t>> meta_;
  std::list<std::pair<std::uint32_t, Bytes>> fragments_;
  std::uint64_t work_ = 0;
};

struct Inode {
  std::uint16_t type = 0;
  std::uint32_t mode = 0;
  std::int64_t mtime = 0;
  // directories
  std::uint32_t dir_block = 0;
  std::uint32_t dir_offset = 0;
  std::uint32_t dir_size = 0;
  // files
  std::uint64_t blocks_start = 0;
  std::uint64_t file_size = 0;
  std::uint32_t fragment = no_fragment;
  std::uint32_t fragment_offset = 0;
  std::vector<std::uint32_t> block_sizes;
  // symlinks
  std::string target;
};

Result<Inode> read_inode(Image& image, std::uint64_t ref) {
  auto s = image.inode_stream(ref);
  Inode in;
  CVT_TRY_ASSIGN(in.type, s.u16());
  CVT_TRY_ASSIGN(const std::uint16_t perm, s.u16());
  in.mode = perm & 07777u;
  CVT_TRY_ASSIGN(const std::uint32_t uid_gid, s.u32());
  (void)uid_gid;
  CVT_TRY_ASSIGN(const std::uint32_t mtime, s.u32());
  in.mtime = mtime;
  CVT_TRY_ASSIGN(const std::uint32_t number, s.u32());
  (void)number;
  const std::uint32_t block_size = image.sb().block_size;
  switch (in.type) {
    case 1: {  // basic directory
      CVT_TRY_ASSIGN(in.dir_block, s.u32());
      CVT_TRY_ASSIGN(const std::uint32_t links, s.u32());
      (void)links;
      CVT_TRY_ASSIGN(const std::uint16_t size, s.u16());
      CVT_TRY_ASSIGN(const std::uint16_t offset, s.u16());
      in.dir_size = size;
      in.dir_offset = offset;
      break;
    }
    case 8: {  // extended directory (the index that follows is not needed)
      CVT_TRY_ASSIGN(const std::uint32_t links, s.u32());
      (void)links;
      CVT_TRY_ASSIGN(in.dir_size, s.u32());
      CVT_TRY_ASSIGN(in.dir_block, s.u32());
      CVT_TRY_ASSIGN(const std::uint32_t parent, s.u32());
      (void)parent;
      CVT_TRY_ASSIGN(const std::uint16_t index_count, s.u16());
      (void)index_count;
      CVT_TRY_ASSIGN(const std::uint16_t offset, s.u16());
      in.dir_offset = offset;
      break;
    }
    case 2:
    case 9: {  // basic / extended file
      if (in.type == 2) {
        CVT_TRY_ASSIGN(const std::uint32_t start, s.u32());
        in.blocks_start = start;
        CVT_TRY_ASSIGN(in.fragment, s.u32());
        CVT_TRY_ASSIGN(in.fragment_offset, s.u32());
        CVT_TRY_ASSIGN(const std::uint32_t size, s.u32());
        in.file_size = size;
      } else {
        CVT_TRY_ASSIGN(in.blocks_start, s.u64());
        CVT_TRY_ASSIGN(in.file_size, s.u64());
        CVT_TRY_ASSIGN(const std::uint64_t sparse, s.u64());
        (void)sparse;
        CVT_TRY_ASSIGN(const std::uint32_t links, s.u32());
        (void)links;
        CVT_TRY_ASSIGN(in.fragment, s.u32());
        CVT_TRY_ASSIGN(in.fragment_offset, s.u32());
        CVT_TRY_ASSIGN(const std::uint32_t xattr, s.u32());
        (void)xattr;
      }
      std::uint64_t count = in.file_size / block_size;
      if (in.fragment == no_fragment && in.file_size % block_size != 0) ++count;
      // Every block takes at least its 4-byte size word in the inode table.
      if (count > image.size() / 4 + 1) return fail(Status::Decode, "SquashFS file declares more blocks than the image holds");
      in.block_sizes.resize(static_cast<std::size_t>(count));
      for (std::uint32_t& b : in.block_sizes) {
        CVT_TRY_ASSIGN(b, s.u32());
      }
      break;
    }
    case 3:
    case 10: {  // symlink
      CVT_TRY_ASSIGN(const std::uint32_t links, s.u32());
      (void)links;
      CVT_TRY_ASSIGN(const std::uint32_t size, s.u32());
      if (size > 4096) return fail(Status::Decode, "SquashFS symlink target is too long");
      CVT_TRY_ASSIGN(const Bytes target, s.bytes(size));
      in.target = util::sanitize_utf8(as_chars(target));
      break;
    }
    case 4: case 5: case 6: case 7: case 11: case 12: case 13: case 14:
      break;  // devices, FIFOs, sockets: skipped by the caller
    default:
      return fail(Status::Decode, "SquashFS inode type {} is unknown", in.type);
  }
  return in;
}

Result<Bytes> file_content(Image& image, const Inode& in) {
  const std::uint32_t block_size = image.sb().block_size;
  CVT_TRY(image.sink().charge(in.file_size));
  if (in.file_size > SIZE_MAX) return fail(Status::Limit, "SquashFS file is too large for this platform");
  Bytes out;
  out.reserve(static_cast<std::size_t>(in.file_size));
  std::uint64_t pos = in.blocks_start;
  for (const std::uint32_t word : in.block_sizes) {
    const std::uint64_t want = std::min<std::uint64_t>(block_size, in.file_size - out.size());
    const std::uint32_t stored = word & 0xFFFFFFu;
    if (stored == 0) {  // sparse block
      out.insert(out.end(), static_cast<std::size_t>(want), std::byte{0});
      continue;
    }
    const bool compressed = (word & (1u << 24)) == 0;
    CVT_TRY_ASSIGN(const Bytes plain, image.block(pos, stored, compressed, block_size));
    if (plain.size() < want) return fail(Status::Decode, "SquashFS data block is shorter than the file needs");
    out.insert(out.end(), plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(want));
    pos += stored;
  }
  if (in.fragment != no_fragment && out.size() < in.file_size) {
    CVT_TRY_ASSIGN(const Bytes* frag, image.fragment(in.fragment));
    const std::uint64_t tail = in.file_size - out.size();
    if (in.fragment_offset > frag->size() || tail > frag->size() - in.fragment_offset) {
      return fail(Status::Decode, "SquashFS fragment is shorter than the file's tail");
    }
    out.insert(out.end(), frag->begin() + in.fragment_offset,
               frag->begin() + static_cast<std::ptrdiff_t>(in.fragment_offset + tail));
  }
  if (out.size() != in.file_size) return fail(Status::Decode, "SquashFS file is shorter than its declared size");
  return out;
}

struct Walker {
  Image& image;
  std::set<std::uint64_t> active;  // directory inodes on the current path
  std::set<std::uint64_t> seen_dirs;
  std::size_t visited = 0;
  bool warned_special = false;

  Result<void> directory(const Inode& dir, const std::string& prefix, int depth) {
    if (depth > max_depth) return fail(Status::Decode, "SquashFS directories are nested too deeply");
    if (dir.dir_size < 3) return {};
    std::uint32_t remaining = dir.dir_size - 3;
    Image::Stream s(image, image.sb().directory_table + dir.dir_block, dir.dir_offset);
    while (remaining > 0) {
      if (remaining < 12) return fail(Status::Decode, "SquashFS directory listing is damaged");
      CVT_TRY_ASSIGN(const std::uint32_t count_minus_one, s.u32());
      CVT_TRY_ASSIGN(const std::uint32_t start, s.u32());
      CVT_TRY_ASSIGN(const std::uint32_t base, s.u32());
      (void)base;
      remaining -= 12;
      if (count_minus_one >= 256) return fail(Status::Decode, "SquashFS directory header is damaged");
      for (std::uint32_t i = 0; i <= count_minus_one; ++i) {
        if (remaining < 8) return fail(Status::Decode, "SquashFS directory listing is damaged");
        CVT_TRY_ASSIGN(const std::uint16_t offset, s.u16());
        CVT_TRY_ASSIGN(const std::uint16_t delta, s.u16());
        (void)delta;
        CVT_TRY_ASSIGN(const std::uint16_t type, s.u16());
        (void)type;
        CVT_TRY_ASSIGN(const std::uint16_t name_size, s.u16());
        remaining -= 8;
        const std::uint32_t name_len = static_cast<std::uint32_t>(name_size) + 1;
        if (name_len > remaining || name_len > 256) return fail(Status::Decode, "SquashFS directory entry is damaged");
        CVT_TRY_ASSIGN(const Bytes name, s.bytes(name_len));
        remaining -= name_len;
        if (++visited > image.ctx().limits.max_archive_entries * 2 + 16) {
          return fail(Status::Limit, "SquashFS image has more than {} entries", image.ctx().limits.max_archive_entries);
        }
        if ((visited & 255u) == 0) CVT_TRY(image.ctx().checkpoint(0.5));
        const std::uint64_t ref = (static_cast<std::uint64_t>(start) << 16) | offset;
        const std::string path = prefix + util::sanitize_utf8(as_chars(name));
        CVT_TRY(entry(ref, path, depth));
      }
    }
    return {};
  }

  Result<void> entry(std::uint64_t ref, const std::string& path, int depth) {
    CVT_TRY_ASSIGN(const Inode in, read_inode(image, ref));
    ir::ArchiveEntry e;
    e.path = path;
    e.mtime = in.mtime;
    e.unix_mode = in.mode;
    switch (in.type) {
      case 1:
      case 8: {
        if (active.contains(ref)) return fail(Status::Decode, "SquashFS directory contains itself");
        if (!seen_dirs.insert(ref).second) return fail(Status::Decode, "SquashFS directory appears twice");
        e.is_directory = true;
        CVT_TRY(image.sink().add(std::move(e)));
        active.insert(ref);
        CVT_TRY(directory(in, path + "/", depth + 1));
        active.erase(ref);
        return {};
      }
      case 2:
      case 9: {
        CVT_TRY_ASSIGN(e.bytes, file_content(image, in));
        return image.sink().add(std::move(e));
      }
      case 3:
      case 10:
        if (in.target.empty()) return {};
        e.symlink_target = in.target;
        return image.sink().add(std::move(e));
      default:
        if (!warned_special) image.ctx().warn("Device files, FIFOs and sockets were skipped.");
        warned_special = true;
        return {};
    }
  }
};

Result<Superblock> parse_superblock(ByteView data) {
  if (data.size() < superblock_size || le32(data, 0) != magic) return fail(Status::Decode, "not a SquashFS image");
  Superblock sb;
  sb.inode_count = le32(data, 4);
  sb.block_size = le32(data, 12);
  sb.fragment_count = le32(data, 16);
  sb.compressor = le16(data, 20);
  const std::uint16_t block_log = le16(data, 22);
  sb.flags = le16(data, 24);
  const std::uint16_t major = le16(data, 28);
  const std::uint16_t minor = le16(data, 30);
  sb.root_inode = le64(data, 32);
  sb.bytes_used = le64(data, 40);
  sb.inode_table = le64(data, 64);
  sb.directory_table = le64(data, 72);
  sb.fragment_table = le64(data, 80);
  if (major != 4) return fail(Status::Decode, "SquashFS version {}.{} is not supported (only 4.0)", major, minor);
  if (block_log < 12 || block_log > 20 || sb.block_size != (1u << block_log)) {
    return fail(Status::Decode, "SquashFS block size is invalid");
  }
  if (sb.bytes_used > data.size()) return fail(Status::Decode, "SquashFS image is truncated");
  if (sb.inode_table >= sb.bytes_used || sb.directory_table >= sb.bytes_used || sb.directory_table < sb.inode_table) {
    return fail(Status::Decode, "SquashFS table positions are invalid");
  }
  return sb;
}

Result<ir::Archive> read_squashfs(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const Superblock sb, parse_superblock(ctx.input));
  CVT_TRY_ASSIGN(const cz::Method method, compressor_method(sb.compressor));
  EntrySink sink(ctx, ctx.input.size());
  Image image(ctx.input.first(static_cast<std::size_t>(sb.bytes_used)), sb, method, sink, ctx);
  CVT_TRY_ASSIGN(const Inode root, read_inode(image, sb.root_inode));
  if (root.type != 1 && root.type != 8) return fail(Status::Decode, "SquashFS root is not a directory");
  Walker walker{image, {}, {}, 0, false};
  walker.active.insert(sb.root_inode);
  walker.seen_dirs.insert(sb.root_inode);
  CVT_TRY(walker.directory(root, "", 0));
  return sink.finish();
}

int sniff_squashfs(std::span<const std::byte> data, std::string_view) {
  if (data.size() < superblock_size || le32(data, 0) != magic) return 0;
  const auto sb = parse_superblock(data);
  if (sb) return 95;
  // A valid header on a truncated file: still SquashFS.
  return le16(data, 28) == 4 ? 85 : 0;
}

}  // namespace

void register_squashfs_format(Registry& registry) {
  ArchiveFormat f;
  f.info.id = "squashfs";
  f.info.name = "SquashFS image";
  f.info.description = "Compressed read-only Linux file system image (SquashFS 4.0), as used by live CDs, snaps and AppImages.";
  f.info.category = Category::Archive;
  f.info.extensions = {"sqfs", "squashfs", "snap"};
  f.info.mime = "application/vnd.squashfs";
  f.info.popularity = 25;
  f.info.signatures = {Signature::at(0, "'hsqs'")};
  f.info.sniff = sniff_squashfs;
  f.read = read_squashfs;
  f.read_cost = 12;
  register_archive_format(registry, std::move(f));
}

}  // namespace cvt::archive
