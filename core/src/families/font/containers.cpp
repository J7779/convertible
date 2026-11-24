// Convertible core - font family: WOFF 1, EOT and Mac resource-fork containers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/containers.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::font {

// --------------------------------------------------------------------------- WOFF 1

Result<ir::Font> read_woff(ByteView data, const Context& ctx) {
  util::ByteReader reader(data);
  if (!reader.consume("wOFF")) return fail(Status::Decode, "not a WOFF file");
  const std::uint32_t flavor = reader.u32be();
  const std::uint32_t length = reader.u32be();
  const std::uint16_t count = reader.u16be();
  const std::uint16_t reserved = reader.u16be();
  reader.skip(8);  // totalSfntSize, version
  const std::uint32_t meta_offset = reader.u32be();
  const std::uint32_t meta_length = reader.u32be();
  const std::uint32_t meta_orig = reader.u32be();
  const std::uint32_t priv_offset = reader.u32be();
  const std::uint32_t priv_length = reader.u32be();
  if (!reader.ok()) return fail(Status::Decode, "the WOFF header is truncated");
  if (reserved != 0) return fail(Status::Decode, "the WOFF header is invalid (reserved field set)");
  if (length != data.size()) ctx.warn("The WOFF length field does not match the file size.");
  if (count == 0 || count > max_tables) return fail(Status::Decode, "the WOFF file declares {} tables", count);
  if (std::uint64_t{count} * 20 > reader.remaining()) return fail(Status::Decode, "the WOFF table directory is truncated");
  ir::Font font;
  font.sfnt_version = flavor;
  std::uint64_t total = 0;
  bool cff = false;
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::uint32_t tag = reader.u32be();
    const std::uint32_t offset = reader.u32be();
    const std::uint32_t comp_length = reader.u32be();
    const std::uint32_t orig_length = reader.u32be();
    const std::uint32_t checksum = reader.u32be();
    if (!tag_is_valid(tag)) return fail(Status::Decode, "WOFF table {} has an invalid tag", i);
    if (std::uint64_t{offset} + comp_length > data.size()) {
      return fail(Status::Decode, "WOFF table '{}' extends past the end of the file", tag_text(tag));
    }
    if (comp_length > orig_length) return fail(Status::Decode, "WOFF table '{}' is larger compressed", tag_text(tag));
    total += orig_length;
    CVT_TRY(ctx.limits.check_decompressed(total));
    const ByteView stored = data.subspan(offset, comp_length);
    ir::FontTable table;
    table.tag = tag_text(tag);
    if (font.find(table.tag)) return fail(Status::Decode, "WOFF table '{}' appears twice", table.tag);
    if (comp_length < orig_length) {
      CVT_TRY_ASSIGN(table.bytes, util::zlib_decompress(stored, orig_length));
      if (table.bytes.size() != orig_length) {
        return fail(Status::Decode, "WOFF table '{}' decompressed to the wrong size", table.tag);
      }
    } else {
      table.bytes.assign(stored.begin(), stored.end());
    }
    if (table.tag != "head" && table_checksum(table.bytes) != checksum) {
      ctx.warn(std::format("WOFF table '{}' has a wrong checksum.", table.tag));
    }
    if (table.tag == "CFF " || table.tag == "CFF2") cff = true;
    font.tables.push_back(std::move(table));
  }
  if (meta_length > 0) {
    if (std::uint64_t{meta_offset} + meta_length > data.size()) {
      return fail(Status::Decode, "the WOFF metadata block is out of range");
    }
    CVT_TRY(ctx.limits.check_decompressed(total + meta_orig));
    auto meta = util::zlib_decompress(data.subspan(meta_offset, meta_length), meta_orig);
    if (meta && meta->size() == meta_orig) font.woff_metadata = std::move(*meta);
    else ctx.warn("The WOFF metadata block is damaged and was ignored.");
  }
  if (priv_length > 0) {
    if (std::uint64_t{priv_offset} + priv_length > data.size()) {
      return fail(Status::Decode, "the WOFF private data block is out of range");
    }
    const ByteView priv = data.subspan(priv_offset, priv_length);
    font.woff_private.assign(priv.begin(), priv.end());
  }
  font.flavor = (flavor == tag_otto || cff) ? ir::FontFlavor::CFF : ir::FontFlavor::TrueType;
  decode_names(font);
  return font;
}

Result<Bytes> write_woff(const ir::Font& font, int level, bool keep_private, const Context& ctx) {
  CVT_TRY_ASSIGN(const Bytes sfnt, write_sfnt(font));
  CVT_TRY_ASSIGN(const std::vector<DirectoryEntry> entries, read_directory(sfnt, 0));
  std::vector<DirectoryEntry> sorted = entries;
  std::ranges::sort(sorted, [](const DirectoryEntry& a, const DirectoryEntry& b) { return a.tag < b.tag; });
  struct Packed {
    DirectoryEntry entry;
    Bytes data;
  };
  std::vector<Packed> packed;
  std::size_t i = 0;
  for (const DirectoryEntry& entry : sorted) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i++) / static_cast<double>(sorted.size()), "Compressing tables"));
    const ByteView original = ByteView(sfnt).subspan(entry.offset, entry.length);
    Bytes stored(original.begin(), original.end());
    if (level > 0 && !original.empty()) {
      CVT_TRY_ASSIGN(Bytes compressed, util::zlib_compress(original, level));
      if (compressed.size() < original.size()) stored = std::move(compressed);
    }
    packed.push_back({entry, std::move(stored)});
  }
  const std::uint32_t header = 44 + 20u * static_cast<std::uint32_t>(packed.size());
  std::uint64_t offset = header;
  std::vector<std::uint32_t> offsets;
  for (const Packed& p : packed) {
    offsets.push_back(static_cast<std::uint32_t>(offset));
    offset = (offset + p.data.size() + 3) / 4 * 4;
  }
  Bytes meta;
  if (!font.woff_metadata.empty()) {
    CVT_TRY_ASSIGN(meta, util::zlib_compress(font.woff_metadata, std::max(level, 1)));
  }
  const std::uint64_t meta_offset = meta.empty() ? 0 : offset;
  if (!meta.empty()) offset = (offset + meta.size() + 3) / 4 * 4;
  const bool with_private = keep_private && !font.woff_private.empty();
  const std::uint64_t priv_offset = with_private ? offset : 0;
  std::uint64_t length = with_private ? offset + font.woff_private.size()
                                      : (meta.empty() ? offset : meta_offset + meta.size());
  if (length > 0xFFFFFFF0u) return fail(Status::Encode, "the font is too large for WOFF");
  std::uint64_t sfnt_size = 12 + 16ull * packed.size();
  for (const Packed& p : packed) sfnt_size += (p.entry.length + 3ull) / 4 * 4;
  const ir::FontTable* head = font.find("head");
  const std::uint32_t revision = head ? get_u32(head->bytes, 4) : 0x00010000u;
  util::ByteWriter out(static_cast<std::size_t>(length));
  out.text("wOFF").u32be(output_sfnt_version(font)).u32be(static_cast<std::uint32_t>(length));
  out.u16be(static_cast<std::uint16_t>(packed.size())).u16be(0).u32be(static_cast<std::uint32_t>(sfnt_size));
  out.u16be(static_cast<std::uint16_t>(revision >> 16)).u16be(static_cast<std::uint16_t>((revision & 0xFFFF) * 1000 / 65536));
  out.u32be(static_cast<std::uint32_t>(meta_offset)).u32be(static_cast<std::uint32_t>(meta.size()));
  out.u32be(static_cast<std::uint32_t>(meta.empty() ? 0 : font.woff_metadata.size()));
  out.u32be(static_cast<std::uint32_t>(priv_offset)).u32be(with_private ? static_cast<std::uint32_t>(font.woff_private.size()) : 0);
  for (std::size_t k = 0; k < packed.size(); ++k) {
    const Packed& p = packed[k];
    out.u32be(p.entry.tag).u32be(offsets[k]).u32be(static_cast<std::uint32_t>(p.data.size()));
    out.u32be(p.entry.length).u32be(p.entry.checksum);
  }
  for (const Packed& p : packed) {
    out.bytes(p.data);
    out.align(4);
  }
  if (!meta.empty()) {
    out.bytes(meta);
    if (with_private) out.align(4);
  }
  if (with_private) out.bytes(font.woff_private);
  return out.take();
}

// --------------------------------------------------------------------------- EOT

namespace {

constexpr std::uint32_t eot_flag_compressed = 0x00000004u;
constexpr std::uint32_t eot_flag_xor = 0x10000000u;

Bytes utf16le(std::string_view text) {
  Bytes out;
  for (const char16_t unit : util::utf8_to_utf16(text)) {
    out.push_back(static_cast<std::byte>(unit & 0xFF));
    out.push_back(static_cast<std::byte>(unit >> 8));
  }
  return out;
}

}  // namespace

bool looks_like_eot(ByteView data) noexcept {
  if (data.size() < 82) return false;
  util::ByteReader reader(data);
  const std::uint32_t eot_size = reader.u32le();
  const std::uint32_t font_size = reader.u32le();
  const std::uint32_t version = reader.u32le();
  if (get_u16(data, 34) != 0x4C50) return false;  // "LP" read little endian = 0x504C
  return eot_size <= data.size() + 16 && font_size < eot_size &&
         (version == 0x00010000u || version == 0x00020001u || version == 0x00020002u);
}

Result<ir::Font> read_eot(ByteView data, const Context& ctx) {
  util::ByteReader reader(data);
  const std::uint32_t eot_size = reader.u32le();
  const std::uint32_t font_size = reader.u32le();
  const std::uint32_t version = reader.u32le();
  const std::uint32_t flags = reader.u32le();
  reader.skip(10 + 1 + 1 + 4 + 2);
  const std::uint16_t magic = reader.u16le();
  reader.skip(16 + 8 + 4 + 16);
  if (!reader.ok() || magic != 0x504C) return fail(Status::Decode, "not an Embedded OpenType file");
  if (version != 0x00010000u && version != 0x00020001u && version != 0x00020002u) {
    return fail(Status::Decode, "unsupported EOT version {:08X}", version);
  }
  if (eot_size > data.size()) return fail(Status::Decode, "the EOT file is truncated");
  // Four length-prefixed UTF-16 names, each preceded by a padding word.
  for (int i = 0; i < 4; ++i) {
    reader.skip(2);
    const std::uint16_t size = reader.u16le();
    reader.skip(size);
  }
  if (version >= 0x00020001u) {
    reader.skip(2);
    const std::uint16_t root = reader.u16le();
    reader.skip(root);
  }
  if (version == 0x00020002u) {
    reader.skip(4 + 4 + 2);
    const std::uint16_t signature = reader.u16le();
    reader.skip(signature);
    reader.skip(4);
    const std::uint32_t eudc = reader.u32le();
    reader.skip(eudc);
  }
  if (!reader.ok()) return fail(Status::Decode, "the EOT header is truncated");
  if (font_size > reader.remaining()) return fail(Status::Decode, "the EOT font data is truncated");
  if (flags & eot_flag_compressed) {
    return fail(Status::Decode, "this EOT file uses MicroType Express compression, which is not supported");
  }
  const ByteView payload = reader.bytes(font_size);
  Bytes font_data(payload.begin(), payload.end());
  if (flags & eot_flag_xor) {
    for (std::byte& b : font_data) b ^= std::byte{0x50};
  }
  CVT_TRY_ASSIGN(ir::Font font, read_sfnt_face(font_data, 0, ctx));
  return font;
}

Result<Bytes> write_eot(const ir::Font& font, const EotOptions& options) {
  CVT_TRY_ASSIGN(Bytes sfnt, write_sfnt(font));
  const ir::FontTable* os2 = font.find("OS/2");
  std::vector<NameRecord> names;
  if (const ir::FontTable* name = font.find("name")) {
    if (auto parsed = parse_name(name->bytes)) names = std::move(*parsed);
  }
  const ByteView o = os2 ? ByteView(os2->bytes) : ByteView{};
  util::ByteWriter out;
  out.u32le(0).u32le(static_cast<std::uint32_t>(sfnt.size())).u32le(options.version);
  out.u32le(options.obfuscate ? eot_flag_xor : 0);
  for (std::size_t i = 0; i < 10; ++i) out.u8(i + 32 < o.size() ? std::to_integer<std::uint8_t>(o[32 + i]) : 0);
  out.u8(1);  // DEFAULT_CHARSET
  out.u8(o.size() > 63 && (get_u16(o, 62) & 1) ? 1 : 0);
  out.u32le(o.size() > 5 ? get_u16(o, 4) : 400);
  out.u16le(o.size() > 9 ? get_u16(o, 8) : 0);
  out.u16le(0x504C);
  for (std::size_t i = 0; i < 4; ++i) out.u32le(get_u32(o, 42 + 4 * i));
  out.u32le(get_u32(o, 78)).u32le(get_u32(o, 82));
  out.u32le(0);  // CheckSumAdjustment, patched below
  const std::size_t checksum_at = out.size() - 4;
  out.zeros(16);
  for (const std::uint16_t id : {1, 2, 5, 4}) {
    const Bytes text = utf16le(best_name(names, id));
    out.u16le(0).u16le(static_cast<std::uint16_t>(text.size())).bytes(text);
  }
  if (options.version >= 0x00020001u) out.u16le(0).u16le(0);  // empty root string
  if (options.version == 0x00020002u) {
    out.u32le(0).u32le(0).u16le(0).u16le(0).u32le(0).u32le(0);
  }
  // CheckSumAdjustment of the embedded font's head table.
  std::uint32_t adjustment = 0;
  if (auto entries = read_directory(sfnt, 0)) {
    for (const DirectoryEntry& e : *entries) {
      if (e.tag == tag_value("head")) adjustment = get_u32(sfnt, e.offset + 8);
    }
  }
  Bytes header = out.take();
  header[checksum_at] = static_cast<std::byte>(adjustment & 0xFF);
  header[checksum_at + 1] = static_cast<std::byte>((adjustment >> 8) & 0xFF);
  header[checksum_at + 2] = static_cast<std::byte>((adjustment >> 16) & 0xFF);
  header[checksum_at + 3] = static_cast<std::byte>(adjustment >> 24);
  if (options.obfuscate) {
    for (std::byte& b : sfnt) b ^= std::byte{0x50};
  }
  const std::uint64_t total = header.size() + sfnt.size();
  if (total > 0xFFFFFFF0u) return fail(Status::Encode, "the font is too large for EOT");
  const auto size = static_cast<std::uint32_t>(total);
  header[0] = static_cast<std::byte>(size & 0xFF);
  header[1] = static_cast<std::byte>((size >> 8) & 0xFF);
  header[2] = static_cast<std::byte>((size >> 16) & 0xFF);
  header[3] = static_cast<std::byte>(size >> 24);
  header.insert(header.end(), sfnt.begin(), sfnt.end());
  return header;
}

// --------------------------------------------------------------------------- resource forks

namespace {

// Locate the resource fork inside a MacBinary or AppleDouble wrapper.
ByteView resource_fork_of(ByteView data) {
  if (data.size() >= 26 && get_u32(data, 0) == 0x00051607u) {  // AppleDouble
    const std::uint16_t entries = get_u16(data, 24);
    for (std::uint32_t i = 0; i < entries; ++i) {
      const std::size_t at = 26 + 12 * i;
      if (at + 12 > data.size()) break;
      if (get_u32(data, at) == 2) {
        const std::uint32_t offset = get_u32(data, at + 4), length = get_u32(data, at + 8);
        if (std::uint64_t{offset} + length <= data.size()) return data.subspan(offset, length);
      }
    }
    return {};
  }
  if (data.size() >= 128 && std::to_integer<int>(data[0]) == 0 && std::to_integer<int>(data[74]) == 0 &&
      std::to_integer<int>(data[1]) > 0 && std::to_integer<int>(data[1]) < 64 && get_u32(data, 65) == 0x46464958u) {
    // MacBinary with type 'FFIL' (font suitcase)
    const std::uint32_t data_length = get_u32(data, 83), resource_length = get_u32(data, 87);
    const std::uint64_t start = 128 + (std::uint64_t{data_length} + 127) / 128 * 128;
    if (start + resource_length <= data.size()) return data.subspan(static_cast<std::size_t>(start), resource_length);
    return {};
  }
  return data;
}

struct ResourceEntry {
  std::uint32_t type;
  std::uint16_t id;
  ByteView data;
};

Result<std::vector<ResourceEntry>> parse_resource_fork(ByteView fork) {
  if (fork.size() < 16) return fail(Status::Decode, "the resource fork is truncated");
  const std::uint32_t data_offset = get_u32(fork, 0), map_offset = get_u32(fork, 4);
  const std::uint32_t data_length = get_u32(fork, 8), map_length = get_u32(fork, 12);
  if (std::uint64_t{data_offset} + data_length > fork.size() || std::uint64_t{map_offset} + map_length > fork.size() ||
      map_length < 30) {
    return fail(Status::Decode, "the resource fork header is invalid");
  }
  const ByteView map = fork.subspan(map_offset, map_length);
  const std::uint16_t type_list = get_u16(map, 24);
  if (type_list + 2u > map.size()) return fail(Status::Decode, "the resource map is invalid");
  const std::uint32_t type_count = get_u16(map, type_list) + 1u;
  if (type_list + 2u + 8u * type_count > map.size()) return fail(Status::Decode, "the resource type list is truncated");
  std::vector<ResourceEntry> entries;
  for (std::uint32_t t = 0; t < type_count; ++t) {
    const std::size_t at = type_list + 2u + 8u * t;
    const std::uint32_t type = get_u32(map, at);
    const std::uint32_t count = get_u16(map, at + 4) + 1u;
    const std::size_t refs = type_list + std::size_t{get_u16(map, at + 6)};
    if (refs + 12ull * count > map.size()) return fail(Status::Decode, "the resource reference list is truncated");
    for (std::uint32_t r = 0; r < count; ++r) {
      const std::size_t ref = refs + 12u * r;
      const std::uint16_t id = get_u16(map, ref);
      const std::uint32_t offset = get_u32(map, ref + 4) & 0x00FFFFFFu;
      const std::uint64_t at_data = std::uint64_t{data_offset} + offset;
      if (at_data + 4 > fork.size()) return fail(Status::Decode, "a resource lies outside the fork");
      const std::uint32_t length = get_u32(fork, static_cast<std::size_t>(at_data));
      if (at_data + 4 + length > std::uint64_t{data_offset} + data_length) {
        return fail(Status::Decode, "a resource lies outside the fork");
      }
      entries.push_back({type, id, fork.subspan(static_cast<std::size_t>(at_data + 4), length)});
      if (entries.size() > 65536) return fail(Status::Decode, "the resource fork has too many resources");
    }
  }
  return entries;
}

}  // namespace

bool looks_like_resource_fork(ByteView data) noexcept {
  const ByteView fork = resource_fork_of(data);
  if (fork.size() < 16 || get_u32(fork, 0) != 256) return false;
  auto entries = parse_resource_fork(fork);
  if (!entries) return false;
  return std::ranges::any_of(*entries, [](const ResourceEntry& e) {
    return e.type == tag_value("sfnt") || e.type == tag_value("FOND") || e.type == tag_value("NFNT");
  });
}

Result<ir::Font> read_resource_fonts(ByteView data, std::int64_t face, const Context& ctx) {
  const ByteView fork = resource_fork_of(data);
  CVT_TRY_ASSIGN(const std::vector<ResourceEntry> entries, parse_resource_fork(fork));
  std::vector<ir::Font> faces;
  bool bitmap_only = false;
  for (const ResourceEntry& e : entries) {
    if (e.type == tag_value("NFNT") || e.type == tag_value("FONT")) bitmap_only = true;
    if (e.type != tag_value("sfnt")) continue;
    CVT_TRY(ctx.limits.check_archive_entries(faces.size() + 1));
    CVT_TRY_ASSIGN(ir::Font f, read_sfnt_face(e.data, 0, ctx));
    faces.push_back(std::move(f));
  }
  if (faces.empty()) {
    return fail(Status::Decode, bitmap_only ? "the suitcase holds only classic bitmap fonts (NFNT), which are not supported"
                                            : "the resource fork holds no fonts");
  }
  if (face < 0 || static_cast<std::uint64_t>(face) >= faces.size()) {
    return fail(Status::InvalidArgument, "face {} does not exist; the suitcase holds {} face(s)", face, faces.size());
  }
  ir::Font primary = std::move(faces[static_cast<std::size_t>(face)]);
  for (std::size_t i = 0; i < faces.size(); ++i) {
    if (i != static_cast<std::size_t>(face)) primary.other_faces.push_back(std::move(faces[i]));
  }
  return primary;
}

Result<Bytes> write_dfont(const ir::Font& font) {
  std::vector<Bytes> resources;
  for (const ir::Font* face : all_faces(font)) {
    CVT_TRY_ASSIGN(Bytes sfnt, write_sfnt(*face));
    resources.push_back(std::move(sfnt));
  }
  util::ByteWriter data;
  std::vector<std::uint32_t> offsets;
  for (const Bytes& r : resources) {
    offsets.push_back(static_cast<std::uint32_t>(data.size()));
    data.u32be(static_cast<std::uint32_t>(r.size())).bytes(r);
  }
  if (data.size() > 0x00FFFFFFu) return fail(Status::Encode, "the fonts are too large for a resource fork");
  const auto data_length = static_cast<std::uint32_t>(data.size());
  const std::uint32_t data_offset = 256;
  const std::uint32_t map_offset = data_offset + data_length;
  const auto count = static_cast<std::uint16_t>(resources.size());
  const std::uint32_t map_length = 28 + 2 + 8 + 12u * count;
  util::ByteWriter out;
  out.u32be(data_offset).u32be(map_offset).u32be(data_length).u32be(map_length);
  out.zeros(240);
  out.bytes(data.view());
  out.u32be(data_offset).u32be(map_offset).u32be(data_length).u32be(map_length);
  out.u32be(0).u16be(0).u16be(0).u16be(28).u16be(static_cast<std::uint16_t>(map_length));
  out.u16be(0);  // one type
  out.text("sfnt").u16be(static_cast<std::uint16_t>(count - 1)).u16be(2 + 8);
  for (std::uint16_t i = 0; i < count; ++i) {
    out.u16be(static_cast<std::uint16_t>(256 + i)).i16be(-1).u8(0);
    out.u8(static_cast<std::uint8_t>(offsets[i] >> 16)).u16be(static_cast<std::uint16_t>(offsets[i] & 0xFFFF));
    out.u32be(0);
  }
  return out.take();
}

}  // namespace cvt::font
