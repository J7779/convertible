// Convertible core - archive family: Apple disk images (.dmg, UDIF) with HFS+ / HFSX volumes, read only.
//
// A UDIF image ends with a 512-byte "koly" trailer that locates the data fork
// and an XML property list. The plist's resource-fork/blkx array holds one
// "mish" table per partition: a list of chunks (sector range, type, offset and
// length in the data fork). Chunk types: 0 and 2 zero-fill, 1 raw,
// 0x80000004 ADC, 0x80000005 zlib, 0x80000006 bzip2, 0x80000007 LZFSE,
// 0x80000008 LZMA (xz container), 0x7FFFFFFE comment, 0xFFFFFFFF end.
// The chunks form a virtual disk that is decompressed lazily, a chunk at a
// time with a small cache, only where the file system is actually read.
// The disk is searched for an HFS+/HFSX volume: bare, behind a GUID
// partition table, an Apple partition map or an MBR. Images without a koly
// trailer are read as raw disk images. APFS volumes, encrypted images and
// segmented images are refused with a precise message; HFS+ is read by
// hfs_plus.cpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/formats.hpp"
#include "families/archive/formats/hfs_plus.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::archive {

namespace {

namespace cz = compression;

constexpr std::size_t sector = 512;
constexpr std::size_t koly_size = 512;

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
bool starts(ByteView d, std::size_t at, std::string_view magic) {
  return d.size() >= at + magic.size() && std::memcmp(d.data() + at, magic.data(), magic.size()) == 0;
}

bool has_koly(ByteView d) { return d.size() >= koly_size && starts(d, d.size() - koly_size, "koly"); }

bool is_encrypted(ByteView d) {
  return starts(d, 0, "encrcdsa") || (d.size() >= 8 && starts(d, d.size() - 8, "cdsaencr"));
}

enum : std::uint32_t {
  chunk_zero = 0,
  chunk_raw = 1,
  chunk_ignore = 2,
  chunk_adc = 0x80000004u,
  chunk_zlib = 0x80000005u,
  chunk_bzip2 = 0x80000006u,
  chunk_lzfse = 0x80000007u,
  chunk_lzma = 0x80000008u,
  chunk_comment = 0x7FFFFFFEu,
  chunk_end = 0xFFFFFFFFu,
};

struct Chunk {
  std::uint64_t sector = 0;  // absolute first sector
  std::uint64_t count = 0;
  std::uint32_t type = 0;
  std::uint64_t offset = 0;  // in the file
  std::uint64_t length = 0;
};

// The plain bytes of an image without a trailer.
class RawDisk final : public hfs::Device {
 public:
  explicit RawDisk(ByteView data) : data_(data) {}
  [[nodiscard]] std::uint64_t size() const noexcept override { return data_.size(); }
  [[nodiscard]] Result<void> read(std::uint64_t offset, std::span<std::byte> out) override {
    if (offset > data_.size() || out.size() > data_.size() - offset) return fail(Status::Decode, "read beyond the end of the disk image");
    std::memcpy(out.data(), data_.data() + static_cast<std::size_t>(offset), out.size());
    return {};
  }

 private:
  ByteView data_;
};

std::string_view chunk_name(std::uint32_t type) {
  switch (type) {
    case chunk_adc: return "ADC";
    case chunk_zlib: return "zlib";
    case chunk_bzip2: return "bzip2";
    case chunk_lzfse: return "LZFSE";
    case chunk_lzma: return "LZMA";
    default: return "unknown";
  }
}

// The virtual disk described by the blkx tables.
class UdifDisk final : public hfs::Device {
 public:
  UdifDisk(ByteView file, std::vector<Chunk> chunks, std::uint64_t sectors, const Context& ctx)
      : file_(file), chunks_(std::move(chunks)), size_(sectors * sector), ctx_(&ctx) {}

  [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

  [[nodiscard]] Result<void> read(std::uint64_t offset, std::span<std::byte> out) override {
    if (offset > size_ || out.size() > size_ - offset) return fail(Status::Decode, "read beyond the end of the disk image");
    std::size_t done = 0;
    while (done < out.size()) {
      const std::uint64_t pos = offset + done;
      const std::uint64_t s = pos / sector;
      // The last chunk starting at or before `s`.
      auto it = std::upper_bound(chunks_.begin(), chunks_.end(), s, [](std::uint64_t v, const Chunk& c) { return v < c.sector; });
      std::uint64_t end_of_piece = 0;
      const Chunk* chunk = nullptr;
      if (it != chunks_.begin()) {
        const Chunk& c = *std::prev(it);
        if (s < c.sector + c.count) chunk = &c;
      }
      if (chunk == nullptr) {
        // A gap between chunks reads as zeros up to the next chunk.
        end_of_piece = it == chunks_.end() ? size_ : it->sector * sector;
      } else {
        end_of_piece = (chunk->sector + chunk->count) * sector;
      }
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size() - done, end_of_piece - pos));
      if (n == 0) return fail(Status::Internal, "DMG chunk map is inconsistent");
      const std::span<std::byte> dst = out.subspan(done, n);
      if (chunk == nullptr || chunk->type == chunk_zero || chunk->type == chunk_ignore) {
        std::fill(dst.begin(), dst.end(), std::byte{0});
      } else {
        const std::uint64_t within = pos - chunk->sector * sector;
        if (chunk->type == chunk_raw) {
          // A raw chunk shorter than its sectors reads as zeros at the end.
          const std::uint64_t have = chunk->length > within ? chunk->length - within : 0;
          const std::size_t copy = static_cast<std::size_t>(std::min<std::uint64_t>(have, n));
          std::memcpy(dst.data(), file_.data() + static_cast<std::size_t>(chunk->offset + within), copy);
          std::fill(dst.begin() + static_cast<std::ptrdiff_t>(copy), dst.end(), std::byte{0});
        } else {
          CVT_TRY_ASSIGN(const Bytes* plain, decoded(*chunk));
          std::memcpy(dst.data(), plain->data() + static_cast<std::size_t>(within), n);
        }
      }
      done += n;
    }
    return {};
  }

 private:
  Result<const Bytes*> decoded(const Chunk& c) {
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->first == &c) {
        cache_.splice(cache_.begin(), cache_, it);
        return &cache_.front().second;
      }
    }
    cz::Method method = cz::Method::Zlib;
    const ByteView packed = file_.subspan(static_cast<std::size_t>(c.offset), static_cast<std::size_t>(c.length));
    switch (c.type) {
      case chunk_adc: method = cz::Method::Adc; break;
      case chunk_zlib: method = cz::Method::Zlib; break;
      case chunk_bzip2: method = cz::Method::Bzip2; break;
      case chunk_lzfse: method = cz::Method::Lzfse; break;
      case chunk_lzma:
        method = starts(packed, 0, "\xFD" "7zXZ") ? cz::Method::Xz : cz::Method::LzmaAlone;
        break;
      default: return fail(Status::Decode, "DMG chunk type {:#x} is not supported", c.type);
    }
    if (!cz::can_decode(method)) {
      return fail(Status::Decode, "this DMG uses {} compression, which is not available in this build", chunk_name(c.type));
    }
    const std::uint64_t expected = c.count * sector;
    const std::uint64_t limit = ctx_->limits.max_decompressed_bytes;
    if (expected > limit) return fail(Status::Limit, "a DMG chunk expands beyond the size limit");
    cz::DecodeOptions d;
    d.max_output = expected;
    if (method == cz::Method::Adc || method == cz::Method::Lzfse) d.output_size = expected;
    auto plain = cz::decompress(method, packed, d);
    if (!plain) {
      if (plain.error().code == Status::Limit) return fail(Status::Decode, "a DMG chunk is larger than its sectors");
      return std::unexpected(std::move(plain).error());
    }
    if (plain->size() != expected) return fail(Status::Decode, "a DMG chunk is shorter than its sectors");
    work_ += expected;
    if (work_ > (limit > (std::uint64_t{1} << 62) ? limit : 2 * limit + (64u << 20))) {
      return fail(Status::Limit, "the disk image needs too much decompression work");
    }
    cached_ += plain->size();
    cache_.emplace_front(&c, std::move(*plain));
    while (cache_.size() > 1 && (cache_.size() > 8 || cached_ > (32u << 20))) {
      cached_ -= cache_.back().second.size();
      cache_.pop_back();
    }
    return &cache_.front().second;
  }

  ByteView file_;
  std::vector<Chunk> chunks_;
  std::uint64_t size_;
  const Context* ctx_;
  std::list<std::pair<const Chunk*, Bytes>> cache_;
  std::uint64_t cached_ = 0;
  std::uint64_t work_ = 0;
};

// Parse one "mish" table into chunks.
Result<void> parse_mish(ByteView m, std::uint64_t data_fork, std::uint64_t data_length, ByteView file, std::vector<Chunk>& out) {
  if (m.size() < 204 || !starts(m, 0, "mish")) return fail(Status::Decode, "DMG block table is damaged");
  const std::uint64_t first = be64(m, 8);
  const std::uint64_t data_offset = be64(m, 24);
  const std::uint32_t count = be32(m, 200);
  if (count > (m.size() - 204) / 40) return fail(Status::Decode, "DMG block table is truncated");
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t at = 204 + 40 * static_cast<std::size_t>(i);
    Chunk c;
    c.type = be32(m, at);
    if (c.type == chunk_end) break;
    if (c.type == chunk_comment) continue;
    const std::uint64_t rel = be64(m, at + 8);
    c.count = be64(m, at + 16);
    c.offset = be64(m, at + 24);
    c.length = be64(m, at + 32);
    if (c.count == 0) continue;
    if (rel > (std::uint64_t{1} << 40) || c.count > (std::uint64_t{1} << 40) || first > (std::uint64_t{1} << 40)) {
      return fail(Status::Decode, "DMG block table has an absurd sector range");
    }
    c.sector = first + rel;
    if (c.type != chunk_zero && c.type != chunk_ignore) {
      const std::uint64_t start = data_fork + data_offset;
      if (data_offset > data_length || c.offset > data_length - data_offset || c.length > data_length - data_offset - c.offset ||
          start + c.offset + c.length > file.size()) {
        return fail(Status::Decode, "DMG chunk lies outside the data fork");
      }
      c.offset += start;
    }
    if (c.type == chunk_raw && c.length > c.count * sector) return fail(Status::Decode, "DMG raw chunk is too long");
    out.push_back(c);
  }
  return {};
}

// The blkx tables of the XML property list.
Result<std::vector<Bytes>> blkx_tables(ByteView xml) {
  util::XmlParseOptions o;
  o.max_depth = 64;
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(xml, o));
  const util::XmlNode plist = doc.root();
  const auto value_of = [](util::XmlNode dict, std::string_view key) -> util::XmlNode {
    for (util::XmlNode n = dict.first_child(); n; n = n.next_sibling()) {
      if (n.is_element() && n.name() == "key" && n.text() == key) {
        for (util::XmlNode v = n.next_sibling(); v; v = v.next_sibling()) {
          if (v.is_element()) return v;
        }
      }
    }
    return {};
  };
  const util::XmlNode top = plist.child("dict");
  const util::XmlNode fork = value_of(top, "resource-fork");
  const util::XmlNode blkx = fork ? value_of(fork, "blkx") : util::XmlNode{};
  if (!blkx || blkx.name() != "array") return fail(Status::Decode, "the DMG property list has no block tables");
  std::vector<Bytes> out;
  for (util::XmlNode d = blkx.first_child(); d; d = d.next_sibling()) {
    if (!d.is_element() || d.name() != "dict") continue;
    const util::XmlNode data = value_of(d, "Data");
    if (!data) continue;
    CVT_TRY_ASSIGN(Bytes table, util::base64_decode(data.text()));
    out.push_back(std::move(table));
  }
  return out;
}

Result<std::unique_ptr<hfs::Device>> open_disk(ByteView file, const Context& ctx) {
  if (is_encrypted(file)) return fail(Status::Decode, "encrypted disk images are not supported");
  if (!has_koly(file)) return std::unique_ptr<hfs::Device>(std::make_unique<RawDisk>(file));
  const ByteView k = file.subspan(file.size() - koly_size);
  if (be32(k, 4) != 4 || be32(k, 8) != koly_size) return fail(Status::Decode, "DMG trailer version is not supported");
  const std::uint64_t data_fork = be64(k, 24);
  const std::uint64_t data_length = be64(k, 32);
  const std::uint32_t segments = be32(k, 60);
  const std::uint64_t xml_offset = be64(k, 0xD8);
  const std::uint64_t xml_length = be64(k, 0xE0);
  const std::uint64_t sectors = be64(k, 0x1EC);
  if (segments > 1) return fail(Status::Decode, "segmented disk images (.dmgpart) are not supported");
  if (data_fork > file.size() || data_length > file.size() - data_fork) return fail(Status::Decode, "DMG data fork lies outside the file");
  if (xml_length == 0) {
    return fail(Status::Decode, "this DMG has no XML property list (old resource-fork images are not supported)");
  }
  if (xml_offset > file.size() || xml_length > file.size() - xml_offset) return fail(Status::Decode, "DMG property list lies outside the file");
  CVT_TRY_ASSIGN(const auto tables, blkx_tables(file.subspan(static_cast<std::size_t>(xml_offset), static_cast<std::size_t>(xml_length))));
  std::vector<Chunk> chunks;
  for (const Bytes& t : tables) CVT_TRY(parse_mish(t, data_fork, data_length, file, chunks));
  std::sort(chunks.begin(), chunks.end(), [](const Chunk& a, const Chunk& b) { return a.sector < b.sector; });
  std::uint64_t end = 0;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    if (i > 0 && chunks[i].sector < chunks[i - 1].sector + chunks[i - 1].count) return fail(Status::Decode, "DMG chunks overlap");
    end = std::max(end, chunks[i].sector + chunks[i].count);
  }
  if (chunks.empty()) return fail(Status::Decode, "the DMG has no data");
  const std::uint64_t total = std::max(end, std::min<std::uint64_t>(sectors, end + 64));
  return std::unique_ptr<hfs::Device>(std::make_unique<UdifDisk>(file, std::move(chunks), total, ctx));
}

struct Volume {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

// Find the HFS+ volume on the disk; `apfs` reports an APFS container seen on the way.
Result<std::optional<Volume>> find_volume(hfs::Device& disk, bool& apfs) {
  const auto probe = [&](std::uint64_t offset, std::uint64_t size) -> Result<bool> {
    if (size < 2048 || offset > disk.size() || size > disk.size() - offset) return false;
    Bytes h(512);
    CVT_TRY(disk.read(offset + 1024, h));
    if (hfs::is_volume_header(h)) return true;
    Bytes nx(64);
    CVT_TRY(disk.read(offset, nx));
    if (starts(nx, 32, "NXSB")) apfs = true;
    return false;
  };
  if (disk.size() < 2048) return fail(Status::Decode, "the disk image is too small");
  CVT_TRY_ASSIGN(const bool bare, probe(0, disk.size()));
  if (bare) return std::optional<Volume>(Volume{0, disk.size()});
  Bytes head(static_cast<std::size_t>(std::min<std::uint64_t>(disk.size(), 2048)));
  CVT_TRY(disk.read(0, head));
  // GUID partition table.
  if (starts(head, 512, "EFI PART")) {
    const std::uint64_t entries = le64(head, 512 + 72);
    const std::uint32_t count = std::min<std::uint32_t>(le32(head, 512 + 80), 256);
    const std::uint32_t entry_size = le32(head, 512 + 84);
    static constexpr std::uint8_t hfs_guid[16] = {0x00, 0x53, 0x46, 0x48, 0x00, 0x00, 0xAA, 0x11,
                                                  0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC};
    static constexpr std::uint8_t apfs_guid[16] = {0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11,
                                                   0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC};
    if (entry_size >= 128 && entry_size <= 4096 && entries < disk.size() / sector) {
      Bytes e(entry_size);
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t at = entries * sector + static_cast<std::uint64_t>(i) * entry_size;
        if (at + entry_size > disk.size()) break;
        CVT_TRY(disk.read(at, e));
        const std::uint64_t first = le64(e, 32);
        const std::uint64_t last = le64(e, 40);
        if (last < first || first > disk.size() / sector) continue;
        const std::uint64_t size = std::min<std::uint64_t>((last - first + 1) * sector, disk.size() - first * sector);
        if (std::memcmp(e.data(), apfs_guid, 16) == 0) apfs = true;
        if (std::memcmp(e.data(), hfs_guid, 16) == 0) {
          CVT_TRY_ASSIGN(const bool ok, probe(first * sector, size));
          if (ok) return std::optional<Volume>(Volume{first * sector, size});
        }
      }
    }
  }
  // Apple partition map.
  if (starts(head, 0, "ER") && starts(head, 512, "PM")) {
    const std::uint32_t count = std::min<std::uint32_t>(be32(head, 512 + 4), 64);
    Bytes e(512);
    for (std::uint32_t i = 1; i <= count; ++i) {
      if ((static_cast<std::uint64_t>(i) + 1) * sector > disk.size()) break;
      CVT_TRY(disk.read(static_cast<std::uint64_t>(i) * sector, e));
      if (!starts(e, 0, "PM")) break;
      const std::uint64_t start = be32(e, 8);
      const std::uint64_t blocks = be32(e, 12);
      std::string_view type = as_chars(ByteView(e).subspan(48, 32));
      type = type.substr(0, type.find('\0'));
      if (type == "Apple_APFS") apfs = true;
      if (type == "Apple_HFS" || type == "Apple_HFSX") {
        CVT_TRY_ASSIGN(const bool ok, probe(start * sector, blocks * sector));
        if (ok) return std::optional<Volume>(Volume{start * sector, blocks * sector});
      }
    }
  }
  // MBR.
  if (head.size() >= 512 && head[510] == std::byte{0x55} && head[511] == std::byte{0xAA}) {
    for (std::size_t i = 0; i < 4; ++i) {
      const std::size_t at = 446 + 16 * i;
      const auto type = std::to_integer<unsigned>(head[at + 4]);
      const std::uint64_t start = le32(head, at + 8);
      const std::uint64_t count = le32(head, at + 12);
      if (type == 0xAF) {
        CVT_TRY_ASSIGN(const bool ok, probe(start * sector, count * sector));
        if (ok) return std::optional<Volume>(Volume{start * sector, count * sector});
      }
    }
  }
  return std::optional<Volume>{};
}

Result<ir::Archive> read_dmg(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const auto disk, open_disk(ctx.input, ctx));
  bool apfs = false;
  CVT_TRY_ASSIGN(const auto volume, find_volume(*disk, apfs));
  if (!volume) {
    if (apfs) return fail(Status::Decode, "APFS volumes are not supported (only HFS+ and HFSX disk images can be read)");
    return fail(Status::Decode, "no HFS+ volume found in the disk image");
  }
  EntrySink sink(ctx, ctx.input.size());
  hfs::Slice slice(*disk, volume->offset, volume->size);
  CVT_TRY(hfs::read_volume(slice, sink, ctx));
  return sink.finish();
}

int sniff_dmg(std::span<const std::byte> data, std::string_view filename) {
  const bool named = util::iends_with(filename, ".dmg");
  if (has_koly(data)) {
    const ByteView k = data.subspan(data.size() - koly_size);
    if (be32(k, 4) == 4 && be32(k, 8) == koly_size) return 95;
    return named ? 80 : 0;
  }
  if (named && is_encrypted(data)) return 90;
  // Raw disk images: only with the name, and only when they hold HFS+.
  if (named && data.size() >= 1536 && hfs::is_volume_header(data.subspan(1024))) return 80;
  if (named && data.size() >= 1024 && starts(data, 512, "EFI PART")) return 70;
  if (named && starts(data, 0, "ER") && starts(data, 512, "PM")) return 70;
  return 0;
}

}  // namespace

void register_dmg_format(Registry& registry) {
  ArchiveFormat f;
  f.info.id = "dmg";
  f.info.name = "Apple disk image";
  f.info.description = "macOS disk image (UDIF: raw, zlib, bzip2, LZFSE, LZMA or ADC) with an HFS+ volume.";
  f.info.category = Category::Archive;
  f.info.extensions = {"dmg"};
  f.info.mime = "application/x-apple-diskimage";
  f.info.popularity = 40;
  f.info.sniff = sniff_dmg;
  f.read = read_dmg;
  f.read_cost = 14;
  register_archive_format(registry, std::move(f));
}

}  // namespace cvt::archive
