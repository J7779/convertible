// Convertible core - archive family: Windows Imaging (.wim, .swm, .esd) reader.
//
// Reads the header, the blob table (every blob's SHA-1 is verified), and the
// metadata resource of one image (`wim_image`, default 1): the security data
// is skipped and the dentry tree is walked (UTF-16 names, attributes, last
// write times, wimlib's UNIX data tagged items for permissions, symbolic
// links stored as reparse points). Resources are stored or compressed in
// chunks with XPRESS (Huffman), LZX or LZMS through compression.hpp; solid
// resources (ESD / `wimcapture --solid`) are decoded as a whole and their
// blobs sliced out. Named data streams, security descriptors and junctions
// are left out; pipable WIMs and blobs that live in other parts of a split
// WIM are reported precisely.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "util/bytes.hpp"
#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/formats.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

namespace cvt::archive {

namespace wim {

namespace {

constexpr std::uint32_t flag_compression = 0x00000002;
constexpr std::uint32_t flag_xpress = 0x00020000;
constexpr std::uint32_t flag_lzx = 0x00040000;
constexpr std::uint32_t flag_lzms = 0x00080000;
constexpr std::uint32_t flag_xpress2 = 0x00200000;

constexpr std::uint8_t res_metadata = 0x02;
constexpr std::uint8_t res_compressed = 0x04;
constexpr std::uint8_t res_spanned = 0x08;
constexpr std::uint8_t res_solid = 0x10;
constexpr std::uint64_t solid_magic = 0x100000000ull;

constexpr std::uint32_t attr_directory = 0x10;
constexpr std::uint32_t attr_reparse = 0x400;
constexpr std::uint32_t tag_symlink = 0xA000000Cu;
constexpr std::uint32_t tag_unix_data = 0x337DD873u;
constexpr std::size_t max_depth = 512;

using Hash = std::array<std::uint8_t, 20>;

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
std::uint64_t align8(std::uint64_t v) { return (v + 7) & ~std::uint64_t{7}; }

Hash hash_at(ByteView d, std::size_t at) {
  Hash h{};
  for (std::size_t i = 0; i < 20; ++i) h[i] = std::to_integer<std::uint8_t>(d[at + i]);
  return h;
}
bool is_zero(const Hash& h) {
  return std::all_of(h.begin(), h.end(), [](std::uint8_t b) { return b == 0; });
}

std::string utf16le(ByteView d) {
  std::u16string wide;
  for (std::size_t i = 0; i + 1 < d.size(); i += 2) wide.push_back(static_cast<char16_t>(le16(d, i)));
  return util::utf16_to_utf8(wide);
}

std::int64_t filetime(std::uint64_t ft) {
  if (ft == 0) return 0;
  return static_cast<std::int64_t>(ft / 10000000u) - 11644473600LL;
}

struct ResHdr {
  std::uint64_t size = 0;  // stored size in the WIM
  std::uint8_t flags = 0;
  std::uint64_t offset = 0;
  std::uint64_t usize = 0;  // uncompressed size
};

ResHdr reshdr(ByteView d, std::size_t at) {
  ResHdr r;
  r.size = le64(d, at) & 0x00FFFFFFFFFFFFFFull;
  r.flags = std::to_integer<std::uint8_t>(d[at + 7]);
  r.offset = le64(d, at + 8);
  r.usize = le64(d, at + 16);
  return r;
}

struct Blob {
  ResHdr res;
  std::uint16_t part = 0;
  Hash hash{};
  std::optional<std::size_t> group;  // solid group, `res.offset` is then the offset inside it
};

struct SolidGroup {
  std::vector<ResHdr> resources;
  std::optional<Bytes> data;
};

class Wim {
 public:
  Wim(ByteView data, EntrySink& sink, const Context& ctx) : data_(data), sink_(sink), ctx_(ctx) {}

  Result<void> run(std::int64_t image) {
    CVT_TRY(read_header());
    CVT_TRY(read_blob_table());
    if (image < 1 || static_cast<std::uint64_t>(image) > metadata_.size()) {
      return fail(Status::InvalidArgument, "the WIM file has {} image(s); image {} does not exist", metadata_.size(), image);
    }
    const Blob& meta = metadata_[static_cast<std::size_t>(image - 1)];
    if (meta.part != part_) return fail(Status::Decode, "the metadata of image {} is in part {} of this split WIM", image, meta.part);
    if (meta.res.usize > std::max<std::uint64_t>(sink_.budget(), 0)) {
      return fail(Status::Limit, "WIM metadata of {} bytes exceeds the limit", meta.res.usize);
    }
    CVT_TRY_ASSIGN(const Bytes metadata, read_blob(meta, true));
    CVT_TRY(walk(metadata));
    if (missing_ != 0) {
      ctx_.warn(std::format("{} file(s) whose data is in another part of this split WIM were skipped.", missing_));
    }
    return {};
  }

 private:
  Result<void> read_header() {
    if (data_.size() >= 8 && as_chars(data_.first(8)) == std::string_view("WLPWM\0\0\0", 8)) {
      return fail(Status::Decode, "pipable WIM files are not supported");
    }
    if (data_.size() < 208 || as_chars(data_.first(8)) != std::string_view("MSWIM\0\0\0", 8)) {
      return fail(Status::Decode, "not a WIM file");
    }
    if (le32(data_, 8) < 208) return fail(Status::Decode, "WIM header is damaged");
    flags_ = le32(data_, 16);
    chunk_size_ = le32(data_, 20);
    part_ = le16(data_, 40);
    parts_ = le16(data_, 42);
    blob_table_ = reshdr(data_, 48);
    if ((flags_ & flag_compression) != 0) {
      if ((flags_ & flag_lzx) != 0) {
        method_ = compression::Method::LzxWim;
      } else if ((flags_ & flag_lzms) != 0) {
        method_ = compression::Method::Lzms;
      } else if ((flags_ & (flag_xpress | flag_xpress2)) != 0) {
        method_ = compression::Method::XpressHuffman;
      } else {
        return fail(Status::Decode, "WIM compression type is not known");
      }
      if (chunk_size_ == 0) chunk_size_ = 32768;
      if (!std::has_single_bit(chunk_size_) || chunk_size_ < 4096 || chunk_size_ > (1u << 26)) {
        return fail(Status::Decode, "WIM chunk size {} is not supported", chunk_size_);
      }
    }
    return {};
  }

  static std::optional<compression::Method> method_of(std::uint32_t type) {
    switch (type) {
      case 1: return compression::Method::XpressHuffman;
      case 2: return compression::Method::LzxWim;
      case 3: return compression::Method::Lzms;
      default: return std::nullopt;
    }
  }

  Result<ByteView> stored(const ResHdr& r) const {
    if (r.offset > data_.size() || r.size > data_.size() - r.offset) return fail(Status::Decode, "WIM resource is truncated");
    return data_.subspan(static_cast<std::size_t>(r.offset), static_cast<std::size_t>(r.size));
  }

  Result<Bytes> decode_chunk(ByteView packed, std::uint64_t size, compression::Method method, std::uint32_t chunk) const {
    if (packed.size() == size) return to_bytes(packed);  // stored chunk
    if (packed.size() > size) return fail(Status::Decode, "WIM chunk is larger than its data");
    if (!compression::can_decode(method)) {
      return fail(Status::Decode, "{} compressed WIM data cannot be read by this build", compression::method_name(method));
    }
    compression::DecodeOptions d;
    d.output_size = size;
    d.max_output = size;
    if (method == compression::Method::LzxWim) d.window_bits = std::max(15, std::countr_zero(chunk));
    CVT_TRY_ASSIGN(Bytes out, compression::decompress(method, packed, d));
    if (out.size() != size) return fail(Status::Decode, "WIM chunk decompressed to the wrong size");
    return out;
  }

  // A complete non-solid resource.
  Result<Bytes> read_resource(const ResHdr& r) const {
    CVT_TRY_ASSIGN(const ByteView raw, stored(r));
    if ((r.flags & res_compressed) == 0) {
      if (r.size != r.usize) return fail(Status::Decode, "WIM resource sizes disagree");
      return to_bytes(raw);
    }
    if (!method_) return fail(Status::Decode, "compressed WIM resource in an uncompressed WIM");
    const std::uint64_t chunk = chunk_size_;
    const std::uint64_t chunks = (r.usize + chunk - 1) / chunk;
    if (chunks == 0) return Bytes{};
    const std::uint64_t entry = r.usize > 0xFFFFFFFFull ? 8 : 4;
    const std::uint64_t table = (chunks - 1) * entry;
    if (table > raw.size()) return fail(Status::Decode, "WIM chunk table is truncated");
    const ByteView body = raw.subspan(static_cast<std::size_t>(table));
    Bytes out;
    out.reserve(static_cast<std::size_t>(r.usize));
    for (std::uint64_t i = 0; i < chunks; ++i) {
      const auto at = [&](std::uint64_t k) -> std::uint64_t {
        const std::size_t p = static_cast<std::size_t>((k - 1) * entry);
        return entry == 8 ? le64(raw, p) : le32(raw, p);
      };
      const std::uint64_t start = i == 0 ? 0 : at(i);
      const std::uint64_t end = i + 1 == chunks ? body.size() : at(i + 1);
      if (start > end || end > body.size()) return fail(Status::Decode, "WIM chunk table is damaged");
      const std::uint64_t size = std::min(chunk, r.usize - i * chunk);
      CVT_TRY_ASSIGN(const Bytes piece,
                     decode_chunk(body.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start)), size,
                                  *method_, chunk_size_));
      append(out, piece);
      if ((i & 31u) == 31u) CVT_TRY(ctx_.checkpoint(0.5));
    }
    return out;
  }

  // A solid resource: its own header gives the size, chunk size and method; the chunk table holds sizes.
  Result<Bytes> read_solid(const ResHdr& r) const {
    CVT_TRY_ASSIGN(const ByteView raw, stored(r));
    if (raw.size() < 16) return fail(Status::Decode, "WIM solid resource is truncated");
    const std::uint64_t usize = le64(raw, 0);
    const std::uint32_t chunk = le32(raw, 8);
    const std::uint32_t type = le32(raw, 12);
    if (usize > sink_.budget()) return fail(Status::Limit, "WIM solid resource of {} bytes exceeds the limit", usize);
    if (!std::has_single_bit(chunk) || chunk < 4096 || chunk > (1u << 30)) return fail(Status::Decode, "WIM solid chunk size is not supported");
    const std::uint64_t chunks = (usize + chunk - 1) / chunk;
    if (chunks * 4 > raw.size() - 16) return fail(Status::Decode, "WIM solid chunk table is truncated");
    std::uint64_t pos = 16 + chunks * 4;
    Bytes out;
    out.reserve(static_cast<std::size_t>(usize));
    for (std::uint64_t i = 0; i < chunks; ++i) {
      const std::uint64_t packed = le32(raw, static_cast<std::size_t>(16 + i * 4));
      if (packed > raw.size() - pos) return fail(Status::Decode, "WIM solid resource is truncated");
      const std::uint64_t size = std::min<std::uint64_t>(chunk, usize - i * chunk);
      const ByteView piece = raw.subspan(static_cast<std::size_t>(pos), static_cast<std::size_t>(packed));
      pos += packed;
      if (type == 0 || packed == size) {
        if (packed != size) return fail(Status::Decode, "WIM solid chunk sizes disagree");
        append(out, piece);
        continue;
      }
      const auto method = method_of(type);
      if (!method) return fail(Status::Decode, "WIM solid compression type {} is not known", type);
      CVT_TRY_ASSIGN(const Bytes plain, decode_chunk(piece, size, *method, chunk));
      append(out, plain);
      if ((i & 7u) == 7u) CVT_TRY(ctx_.checkpoint(0.5));
    }
    return out;
  }

  Result<void> read_blob_table() {
    if ((blob_table_.flags & res_compressed) != 0 || blob_table_.size != blob_table_.usize) {
      return fail(Status::Decode, "compressed WIM blob tables are not supported");
    }
    CVT_TRY_ASSIGN(const ByteView table, stored(blob_table_));
    const std::size_t count = table.size() / 50;
    CVT_TRY(ctx_.limits.check_archive_entries(count / 2));
    bool group_open = false;
    for (std::size_t i = 0; i < count; ++i) {
      Blob b;
      b.res = reshdr(table, i * 50);
      b.part = le16(table, i * 50 + 24);
      b.hash = hash_at(table, i * 50 + 30);
      if ((b.res.flags & res_solid) != 0 && b.res.usize == solid_magic) {
        // A solid resource; consecutive ones form one group.
        if (!group_open) groups_.emplace_back();
        groups_.back().resources.push_back(b.res);
        group_open = true;
        continue;
      }
      group_open = false;
      if ((b.res.flags & res_solid) != 0) {
        if (groups_.empty()) return fail(Status::Decode, "WIM solid blob without a solid resource");
        b.group = groups_.size() - 1;
        b.res.usize = b.res.size;  // inside a solid resource the size field is the uncompressed size
      }
      if ((b.res.flags & res_metadata) != 0) {
        metadata_.push_back(b);
      } else if (!is_zero(b.hash)) {
        blobs_[b.hash] = b;
      }
    }
    return {};
  }

  Result<const Bytes*> group_data(std::size_t index) {
    SolidGroup& g = groups_[index];
    if (!g.data) {
      Bytes all;
      for (const ResHdr& r : g.resources) {
        CVT_TRY_ASSIGN(const Bytes part, read_solid(r));
        if (part.size() > sink_.budget() - std::min<std::uint64_t>(all.size(), sink_.budget())) {
          return fail(Status::Limit, "WIM solid resource exceeds the limit");
        }
        append(all, part);
      }
      g.data = std::move(all);
    }
    return &*g.data;
  }

  Result<Bytes> read_blob(const Blob& b, bool metadata) {
    if (b.res.usize > sink_.budget()) return fail(Status::Limit, "WIM file of {} bytes exceeds the limit", b.res.usize);
    if ((b.res.flags & res_spanned) != 0) return fail(Status::Decode, "WIM resources spanning parts are not supported");
    Bytes out;
    if (b.group) {
      CVT_TRY_ASSIGN(const Bytes* all, group_data(*b.group));
      if (b.res.offset > all->size() || b.res.usize > all->size() - b.res.offset) {
        return fail(Status::Decode, "WIM solid blob lies outside its resource");
      }
      out.assign(all->begin() + static_cast<std::ptrdiff_t>(b.res.offset),
                 all->begin() + static_cast<std::ptrdiff_t>(b.res.offset + b.res.usize));
    } else {
      CVT_TRY_ASSIGN(out, read_resource(b.res));
    }
    if (out.size() != b.res.usize) return fail(Status::Decode, "WIM resource has the wrong size");
    const auto digest = util::sha1(out);
    if (::cvt::util::compare_bytes(digest.data(), b.hash.data(), 20) != 0) {
      return fail(Status::Decode, "WIM {} fails its SHA-1 check", metadata ? "metadata" : "file data");
    }
    return out;
  }

  struct Dentry {
    std::uint64_t length = 0;
    std::uint32_t attributes = 0;
    std::uint64_t subdir = 0;
    std::int64_t mtime = 0;
    Hash hash{};
    std::uint32_t reparse_tag = 0;
    std::string name;
    std::optional<std::uint32_t> unix_mode;
    std::uint64_t next = 0;  // offset of the following sibling
    std::vector<Hash> unnamed;  // unnamed streams listed among the extra streams, in order
  };

  static Result<std::optional<Dentry>> parse_dentry(ByteView m, std::uint64_t at) {
    if (at > m.size() || m.size() - at < 8) return fail(Status::Decode, "WIM directory entry is truncated");
    Dentry d;
    d.length = le64(m, static_cast<std::size_t>(at));
    if (d.length < 102) return std::optional<Dentry>{};  // end of directory
    if (d.length > m.size() - at) return fail(Status::Decode, "WIM directory entry is truncated");
    const ByteView e = m.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(d.length));
    d.attributes = le32(e, 8);
    d.subdir = le64(e, 16);
    d.mtime = filetime(le64(e, 56));
    d.hash = hash_at(e, 64);
    d.reparse_tag = le32(e, 88);
    const std::uint16_t extra = le16(e, 96);
    const std::uint64_t short_len = le16(e, 98);
    const std::uint64_t name_len = le16(e, 100);
    std::uint64_t pos = 102;
    if (name_len > e.size() - pos) return fail(Status::Decode, "WIM directory entry name is truncated");
    d.name = utf16le(e.subspan(static_cast<std::size_t>(pos), static_cast<std::size_t>(name_len)));
    pos += name_len + (name_len != 0 ? 2 : 0);
    pos += short_len + (short_len != 0 ? 2 : 0);
    // Tagged items (wimlib UNIX data) fill the rest of the entry.
    for (pos = align8(pos); pos + 8 <= e.size();) {
      const std::uint32_t tag = le32(e, static_cast<std::size_t>(pos));
      const std::uint64_t len = le32(e, static_cast<std::size_t>(pos + 4));
      if (len > e.size() - pos - 8) break;
      if (tag == tag_unix_data && len >= 16) d.unix_mode = le32(e, static_cast<std::size_t>(pos + 16));
      pos = align8(pos + 8 + len);
    }
    std::uint64_t next = align8(at + d.length);
    for (std::uint16_t i = 0; i < extra; ++i) {
      if (next > m.size() || m.size() - next < 38) return fail(Status::Decode, "WIM stream entry is truncated");
      const std::uint64_t len = le64(m, static_cast<std::size_t>(next));
      if (len < 38 || len > m.size() - next) return fail(Status::Decode, "WIM stream entry is damaged");
      if (le16(m, static_cast<std::size_t>(next + 36)) == 0) d.unnamed.push_back(hash_at(m, static_cast<std::size_t>(next + 16)));
      next = align8(next + len);
    }
    d.next = next;
    return std::optional<Dentry>{std::move(d)};
  }

  Result<Bytes> stream(const Hash& h, bool& missing) {
    missing = false;
    if (is_zero(h)) return Bytes{};
    const auto found = blobs_.find(h);
    if (found == blobs_.end()) {
      if (parts_ > 1) {
        missing = true;
        return Bytes{};
      }
      return fail(Status::Decode, "WIM file data is missing from the blob table");
    }
    if (found->second.part != part_) {
      missing = true;
      return Bytes{};
    }
    return read_blob(found->second, false);
  }

  Result<void> walk(const Bytes& meta) {
    const ByteView m(meta);
    if (m.size() < 8) return fail(Status::Decode, "WIM metadata is truncated");
    const std::uint64_t security = std::max<std::uint64_t>(le32(m, 0), 8);
    CVT_TRY_ASSIGN(const auto root, parse_dentry(m, align8(security)));
    if (!root) return fail(Status::Decode, "WIM image has no root directory");
    struct Dir {
      std::uint64_t first;
      std::string path;
      std::size_t depth;
    };
    std::vector<Dir> stack;
    if (root->subdir != 0) stack.push_back({root->subdir, std::string(), 0});
    std::set<std::uint64_t> seen;
    std::size_t steps = 0;
    bool skipped_special = false;
    while (!stack.empty()) {
      Dir dir = std::move(stack.back());
      stack.pop_back();
      if (dir.depth > max_depth) return fail(Status::Decode, "WIM directory tree is nested too deeply");
      if (!seen.insert(dir.first).second) continue;
      std::uint64_t at = dir.first;
      for (;;) {
        if ((++steps & 63u) == 0) CVT_TRY(ctx_.checkpoint(0.6));
        CVT_TRY_ASSIGN(const auto d, parse_dentry(m, at));
        if (!d) break;
        if (d->next <= at) return fail(Status::Decode, "WIM directory entry is damaged");
        at = d->next;
        if (d->name.empty()) continue;
        ir::ArchiveEntry entry;
        entry.path = dir.path.empty() ? d->name : dir.path + "/" + d->name;
        entry.mtime = d->mtime;
        if (d->unix_mode) entry.unix_mode = *d->unix_mode & 07777u;
        const bool reparse = (d->attributes & attr_reparse) != 0;
        if ((d->attributes & attr_directory) != 0 && !reparse) {
          entry.is_directory = true;
          std::string path = entry.path;
          CVT_TRY(sink_.add(std::move(entry)));
          if (d->subdir != 0) stack.push_back({d->subdir, std::move(path), dir.depth + 1});
          continue;
        }
        if (reparse) {
          bool missing = false;
          if (d->reparse_tag != tag_symlink) {
            skipped_special = true;
            continue;
          }
          // The reparse data is the default stream, or else the first unnamed extra stream.
          const Hash& rh = !is_zero(d->hash) || d->unnamed.empty() ? d->hash : d->unnamed.front();
          CVT_TRY_ASSIGN(const Bytes rp, stream(rh, missing));
          if (missing) {
            ++missing_;
            continue;
          }
          const std::string target = symlink_target(rp);
          if (target.empty()) continue;
          entry.symlink_target = target;
          CVT_TRY(sink_.add(std::move(entry)));
          continue;
        }
        bool missing = false;
        const Hash& h = is_zero(d->hash) && !d->unnamed.empty() ? d->unnamed.front() : d->hash;
        CVT_TRY_ASSIGN(entry.bytes, stream(h, missing));
        if (missing) {
          ++missing_;
          continue;
        }
        CVT_TRY(sink_.charge(entry.bytes.size()));
        CVT_TRY(sink_.add(std::move(entry)));
      }
    }
    if (skipped_special) ctx_.warn("Junctions and other special reparse points in the image were skipped.");
    return {};
  }

  static std::string symlink_target(ByteView rp) {
    if (rp.size() < 12) return {};
    const std::size_t sub_off = le16(rp, 0);
    const std::size_t sub_len = le16(rp, 2);
    const std::size_t print_off = le16(rp, 4);
    const std::size_t print_len = le16(rp, 6);
    const ByteView buffer = rp.subspan(12);
    std::string target;
    if (print_len != 0 && print_off + print_len <= buffer.size()) {
      target = utf16le(buffer.subspan(print_off, print_len));
    } else if (sub_off + sub_len <= buffer.size()) {
      target = utf16le(buffer.subspan(sub_off, sub_len));
    }
    std::replace(target.begin(), target.end(), '\\', '/');
    return target;
  }

  ByteView data_;
  EntrySink& sink_;
  const Context& ctx_;
  std::uint32_t flags_ = 0;
  std::uint32_t chunk_size_ = 32768;
  std::uint16_t part_ = 1;
  std::uint16_t parts_ = 1;
  ResHdr blob_table_;
  std::optional<compression::Method> method_;
  std::vector<Blob> metadata_;
  std::map<Hash, Blob> blobs_;
  std::vector<SolidGroup> groups_;
  std::size_t missing_ = 0;
};

Result<ir::Archive> read_wim(ReadContext& ctx) {
  EntrySink sink(ctx, ctx.input.size());
  Wim wim(ctx.input, sink, ctx);
  CVT_TRY(wim.run(ctx.options.get_int("wim_image", 1, 1, 65535)));
  return sink.finish();
}

int sniff_wim(std::span<const std::byte> data, std::string_view) {
  if (data.size() < 208) return 0;
  const std::string_view magic = as_chars(data.first(8));
  if (magic != std::string_view("MSWIM\0\0\0", 8)) return 0;
  return le32(data, 8) >= 208 ? 95 : 0;
}

}  // namespace

}  // namespace wim

void register_wim_format(Registry& registry) {
  ArchiveFormat f;
  f.info.id = "wim";
  f.info.name = "Windows Imaging archive";
  f.info.description =
      "Windows image file (WIM / ESD: XPRESS, LZX or LZMS compressed, solid or not). One image is read (option "
      "'wim_image'). Read only.";
  f.info.category = Category::Archive;
  f.info.extensions = {"wim", "swm", "esd"};
  f.info.mime = "application/x-ms-wim";
  f.info.popularity = 15;
  f.info.signatures = {Signature::at(0, "'MSWIM' 00 00 00")};
  f.info.sniff = wim::sniff_wim;
  f.read = wim::read_wim;
  f.read_options = {OptionSpec::integer("wim_image", "Image", 1, 1, 65535,
                                        "Which image of a WIM file holding several to extract (1 = the first).")};
  register_archive_format(registry, std::move(f));
}

}  // namespace cvt::archive
