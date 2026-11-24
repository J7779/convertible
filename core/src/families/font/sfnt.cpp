// Convertible core - font family: sfnt containers (tables, directories, collections).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/sfnt.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "families/font/tables.hpp"

namespace cvt::font {

std::string tag_text(std::uint32_t tag) {
  std::string text(4, ' ');
  for (int i = 0; i < 4; ++i) text[static_cast<std::size_t>(i)] = static_cast<char>((tag >> (24 - 8 * i)) & 0xFF);
  return text;
}

std::uint32_t tag_value(std::string_view tag) noexcept {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto c = i < tag.size() ? static_cast<std::uint8_t>(tag[i]) : std::uint8_t{' '};
    value = (value << 8) | c;
  }
  return value;
}

bool tag_is_valid(std::uint32_t tag) noexcept {
  for (int i = 0; i < 4; ++i) {
    const auto c = (tag >> (8 * i)) & 0xFF;
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

std::uint32_t table_checksum(ByteView data) noexcept {
  std::uint32_t sum = 0;
  const std::size_t full = data.size() / 4 * 4;
  for (std::size_t i = 0; i < full; i += 4) {
    sum += (std::to_integer<std::uint32_t>(data[i]) << 24) | (std::to_integer<std::uint32_t>(data[i + 1]) << 16) |
           (std::to_integer<std::uint32_t>(data[i + 2]) << 8) | std::to_integer<std::uint32_t>(data[i + 3]);
  }
  if (full < data.size()) {
    std::uint32_t last = 0;
    for (std::size_t i = full; i < full + 4; ++i) {
      last = (last << 8) | (i < data.size() ? std::to_integer<std::uint32_t>(data[i]) : 0u);
    }
    sum += last;
  }
  return sum;
}

std::uint16_t get_u16(ByteView data, std::size_t offset) noexcept {
  if (offset > data.size() || data.size() - offset < 2) return 0;
  return static_cast<std::uint16_t>((std::to_integer<unsigned>(data[offset]) << 8) |
                                    std::to_integer<unsigned>(data[offset + 1]));
}

std::int16_t get_i16(ByteView data, std::size_t offset) noexcept {
  return static_cast<std::int16_t>(get_u16(data, offset));
}

std::uint32_t get_u32(ByteView data, std::size_t offset) noexcept {
  if (offset > data.size() || data.size() - offset < 4) return 0;
  return (std::uint32_t{get_u16(data, offset)} << 16) | get_u16(data, offset + 2);
}

void put_u16(Bytes& data, std::size_t offset, std::uint16_t value) noexcept {
  if (offset > data.size() || data.size() - offset < 2) return;
  data[offset] = static_cast<std::byte>(value >> 8);
  data[offset + 1] = static_cast<std::byte>(value & 0xFF);
}

void put_u32(Bytes& data, std::size_t offset, std::uint32_t value) noexcept {
  put_u16(data, offset, static_cast<std::uint16_t>(value >> 16));
  put_u16(data, offset + 2, static_cast<std::uint16_t>(value & 0xFFFF));
}

Result<std::vector<DirectoryEntry>> read_directory(ByteView file, std::size_t face_offset,
                                                   std::uint32_t* sfnt_version) {
  util::ByteReader reader(file);
  if (!reader.seek(face_offset)) return fail(Status::Decode, "font face offset {} is outside the file", face_offset);
  const std::uint32_t version = reader.u32be();
  const std::uint16_t count = reader.u16be();
  reader.skip(6);
  if (!reader.ok()) return fail(Status::Decode, "the font header is truncated");
  if (version != tag_true_type && version != tag_otto && version != tag_true && version != tag_typ1) {
    return fail(Status::Decode, "not an sfnt font (version tag {:08X})", version);
  }
  if (count == 0) return fail(Status::Decode, "the font has no tables");
  if (count > max_tables) return fail(Status::Decode, "the font declares {} tables", count);
  if (sfnt_version) *sfnt_version = version;
  std::vector<DirectoryEntry> entries;
  entries.reserve(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    DirectoryEntry entry;
    entry.tag = reader.u32be();
    entry.checksum = reader.u32be();
    entry.offset = reader.u32be();
    entry.length = reader.u32be();
    if (!reader.ok()) return fail(Status::Decode, "the table directory is truncated");
    if (!tag_is_valid(entry.tag)) return fail(Status::Decode, "table {} has an invalid tag", i);
    const std::uint64_t end = std::uint64_t{entry.offset} + entry.length;
    if (end > file.size()) {
      return fail(Status::Decode, "table '{}' (offset {}, length {}) extends past the end of the file",
                  tag_text(entry.tag), entry.offset, entry.length);
    }
    entries.push_back(entry);
  }
  // Duplicate tags and partially overlapping tables are signs of a hostile
  // file. Tables sharing exactly the same bytes are tolerated.
  std::vector<DirectoryEntry> sorted = entries;
  std::ranges::sort(sorted, [](const DirectoryEntry& a, const DirectoryEntry& b) { return a.tag < b.tag; });
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i].tag == sorted[i - 1].tag) {
      return fail(Status::Decode, "table '{}' appears twice in the directory", tag_text(sorted[i].tag));
    }
  }
  std::ranges::sort(sorted, [](const DirectoryEntry& a, const DirectoryEntry& b) {
    return a.offset != b.offset ? a.offset < b.offset : a.length < b.length;
  });
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    const DirectoryEntry& previous = sorted[i - 1];
    const DirectoryEntry& current = sorted[i];
    if (previous.length == 0 || current.length == 0) continue;
    const bool same = previous.offset == current.offset && previous.length == current.length;
    if (!same && std::uint64_t{previous.offset} + previous.length > current.offset) {
      return fail(Status::Decode, "tables '{}' and '{}' overlap", tag_text(previous.tag), tag_text(current.tag));
    }
  }
  return entries;
}

void decode_names(ir::Font& font) {
  const ir::FontTable* name = font.find("name");
  if (!name) return;
  const auto records = parse_name(name->bytes);
  if (!records) return;
  ir::FontNames names;
  names.family = best_name(*records, 16);
  if (names.family.empty()) names.family = best_name(*records, 1);
  names.subfamily = best_name(*records, 17);
  if (names.subfamily.empty()) names.subfamily = best_name(*records, 2);
  names.full_name = best_name(*records, 4);
  names.postscript_name = best_name(*records, 6);
  names.version = best_name(*records, 5);
  font.names = std::move(names);
}

Result<ir::Font> read_sfnt_face(ByteView file, std::size_t face_offset, const Context& ctx) {
  std::uint32_t version = 0;
  CVT_TRY_ASSIGN(const std::vector<DirectoryEntry> entries, read_directory(file, face_offset, &version));
  ir::Font font;
  font.sfnt_version = version;
  bool cff = false;
  std::size_t bad_checksums = 0;
  for (const DirectoryEntry& entry : entries) {
    ir::FontTable table;
    table.tag = tag_text(entry.tag);
    const ByteView bytes = file.subspan(entry.offset, entry.length);
    table.bytes.assign(bytes.begin(), bytes.end());
    if (table.tag == "CFF " || table.tag == "CFF2") cff = true;
    if (table.tag != "head" && table_checksum(bytes) != entry.checksum) ++bad_checksums;
    font.tables.push_back(std::move(table));
  }
  if (bad_checksums > 0) {
    ctx.warn(std::format("{} table checksum(s) in the source font were wrong; they were recomputed.", bad_checksums));
  }
  font.flavor = (version == tag_otto || cff) ? ir::FontFlavor::CFF : ir::FontFlavor::TrueType;
  decode_names(font);
  return font;
}

Result<std::vector<std::uint32_t>> collection_offsets(ByteView file) {
  util::ByteReader reader(file);
  if (!reader.starts_with("ttcf")) return std::vector<std::uint32_t>{0};
  reader.skip(4);
  const std::uint32_t version = reader.u32be();
  const std::uint32_t count = reader.u32be();
  if (!reader.ok()) return fail(Status::Decode, "the collection header is truncated");
  if (version != 0x00010000u && version != 0x00020000u) {
    return fail(Status::Decode, "unsupported collection version {:08X}", version);
  }
  if (count == 0) return fail(Status::Decode, "the collection holds no fonts");
  if (count > max_faces || std::uint64_t{count} * 4 > reader.remaining()) {
    return fail(Status::Decode, "the collection declares {} fonts", count);
  }
  std::vector<std::uint32_t> offsets(count);
  for (std::uint32_t& offset : offsets) offset = reader.u32be();
  return offsets;
}

Result<ir::Font> read_sfnt(ByteView file, std::int64_t face, const Context& ctx) {
  CVT_TRY_ASSIGN(const std::vector<std::uint32_t> offsets, collection_offsets(file));
  CVT_TRY(ctx.limits.check_archive_entries(offsets.size()));
  if (face < 0 || static_cast<std::uint64_t>(face) >= offsets.size()) {
    return fail(Status::InvalidArgument, "face {} does not exist; the file holds {} face(s)", face, offsets.size());
  }
  std::vector<ir::Font> faces;
  faces.reserve(offsets.size());
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(offsets.size()), "Reading font"));
    CVT_TRY_ASSIGN(ir::Font one, read_sfnt_face(file, offsets[i], ctx));
    faces.push_back(std::move(one));
  }
  ir::Font primary = std::move(faces[static_cast<std::size_t>(face)]);
  for (std::size_t i = 0; i < faces.size(); ++i) {
    if (i != static_cast<std::size_t>(face)) primary.other_faces.push_back(std::move(faces[i]));
  }
  return primary;
}

std::uint32_t output_sfnt_version(const ir::Font& font) noexcept {
  if (font.find("CFF ") || font.find("CFF2")) return tag_otto;
  if (font.sfnt_version == tag_typ1) return tag_typ1;
  return font.flavor == ir::FontFlavor::CFF && !font.find("glyf") ? tag_otto : tag_true_type;
}

std::vector<const ir::FontTable*> physical_order(const ir::Font& font) {
  static constexpr std::array<std::string_view, 21> truetype_order = {
      "head", "hhea", "maxp", "OS/2", "hmtx", "LTSH", "VDMX", "hdmx", "cmap", "fpgm", "prep",
      "cvt ", "loca", "glyf", "kern", "name", "post", "gasp", "PCLT", "DSIG", "GDEF"};
  static constexpr std::array<std::string_view, 8> cff_order = {"head", "hhea", "maxp", "OS/2", "name",
                                                                  "cmap", "post", "CFF "};
  std::vector<const ir::FontTable*> order;
  std::vector<bool> used(font.tables.size(), false);
  const auto take = [&](std::string_view tag) {
    for (std::size_t i = 0; i < font.tables.size(); ++i) {
      if (!used[i] && font.tables[i].tag == tag) {
        used[i] = true;
        order.push_back(&font.tables[i]);
      }
    }
  };
  if (output_sfnt_version(font) == tag_otto) {
    for (const std::string_view tag : cff_order) take(tag);
  } else {
    for (const std::string_view tag : truetype_order) take(tag);
  }
  std::vector<const ir::FontTable*> rest;
  for (std::size_t i = 0; i < font.tables.size(); ++i) {
    if (!used[i]) rest.push_back(&font.tables[i]);
  }
  std::ranges::sort(rest, [](const ir::FontTable* a, const ir::FontTable* b) { return a->tag < b->tag; });
  order.insert(order.end(), rest.begin(), rest.end());
  return order;
}

namespace {

void write_offset_table(util::ByteWriter& out, std::uint32_t version, std::uint16_t count) {
  std::uint16_t entry_selector = 0;
  while ((2u << entry_selector) <= count) ++entry_selector;
  const auto search_range = static_cast<std::uint16_t>((1u << entry_selector) * 16u);
  out.u32be(version).u16be(count).u16be(search_range).u16be(entry_selector);
  out.u16be(static_cast<std::uint16_t>(count * 16u - search_range));
}

Result<void> check_tables(const ir::Font& font) {
  if (font.tables.empty()) return fail(Status::Encode, "the font has no outline tables to write");
  if (font.tables.size() > max_tables) return fail(Status::Encode, "too many tables ({})", font.tables.size());
  std::vector<std::string_view> tags;
  for (const ir::FontTable& table : font.tables) {
    if (table.tag.size() != 4 || !tag_is_valid(tag_value(table.tag))) {
      return fail(Status::Encode, "invalid table tag '{}'", table.tag);
    }
    if (table.bytes.size() > 0x7FFFFFF0u) return fail(Status::Encode, "table '{}' is too large", table.tag);
    tags.push_back(table.tag);
  }
  std::ranges::sort(tags);
  if (std::ranges::adjacent_find(tags) != tags.end()) return fail(Status::Encode, "duplicate table tags");
  return {};
}

}  // namespace

Result<Bytes> write_sfnt(const ir::Font& font) {
  CVT_TRY(check_tables(font));
  const auto count = static_cast<std::uint16_t>(font.tables.size());
  const std::vector<const ir::FontTable*> order = physical_order(font);
  std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> placed;  // tag -> offset, checksum
  std::uint64_t offset = 12 + 16ull * count;
  for (const ir::FontTable* table : order) {
    Bytes bytes = table->bytes;
    if (table->tag == "head") put_u32(bytes, 8, 0);
    placed[table->tag] = {static_cast<std::uint32_t>(offset), table_checksum(bytes)};
    offset += (table->bytes.size() + 3) / 4 * 4;
    if (offset > 0xFFFFFFF0u) return fail(Status::Encode, "the font is too large for the sfnt format");
  }
  util::ByteWriter out(static_cast<std::size_t>(offset));
  write_offset_table(out, output_sfnt_version(font), count);
  for (const auto& [tag, where] : placed) {
    const ir::FontTable* table = font.find(tag);
    out.u32be(tag_value(tag)).u32be(where.second).u32be(where.first);
    out.u32be(static_cast<std::uint32_t>(table->bytes.size()));
  }
  std::size_t head_at = 0;
  bool has_head = false;
  for (const ir::FontTable* table : order) {
    if (table->tag == "head") {
      head_at = out.size();
      has_head = table->bytes.size() >= 12;
    }
    out.bytes(table->bytes);
    out.align(4);
  }
  Bytes result = out.take();
  if (has_head) {
    put_u32(result, head_at + 8, 0);
    put_u32(result, head_at + 8, 0xB1B0AFBAu - table_checksum(result));
  }
  return result;
}

Result<Bytes> write_collection(std::span<const ir::Font* const> faces) {
  if (faces.empty()) return fail(Status::Encode, "a collection needs at least one font");
  if (faces.size() > max_faces) return fail(Status::Encode, "too many faces for a collection");
  for (const ir::Font* face : faces) CVT_TRY(check_tables(*face));
  // Each distinct table body is stored once; head tables differ by their
  // checkSumAdjustment, which is zero in a collection's shared head.
  std::vector<Bytes> blobs;
  std::map<Bytes, std::size_t> blob_index;
  std::vector<std::vector<std::pair<std::string, std::size_t>>> directories;
  for (const ir::Font* face : faces) {
    std::vector<std::pair<std::string, std::size_t>> directory;
    for (const ir::FontTable* table : physical_order(*face)) {
      Bytes bytes = table->bytes;
      if (table->tag == "head") put_u32(bytes, 8, 0);
      auto [it, inserted] = blob_index.try_emplace(bytes, blobs.size());
      if (inserted) blobs.push_back(std::move(bytes));
      directory.emplace_back(table->tag, it->second);
    }
    std::ranges::sort(directory, [](const auto& a, const auto& b) { return a.first < b.first; });
    directories.push_back(std::move(directory));
  }
  std::uint64_t header_size = 12 + 4ull * faces.size() + 12;  // version 2: DSIG tag, length, offset
  std::vector<std::uint64_t> directory_offsets;
  std::uint64_t at = header_size;
  for (const auto& directory : directories) {
    directory_offsets.push_back(at);
    at += 12 + 16ull * directory.size();
  }
  std::vector<std::uint64_t> blob_offsets;
  for (const Bytes& blob : blobs) {
    at = (at + 3) / 4 * 4;
    blob_offsets.push_back(at);
    at += blob.size();
  }
  if (at > 0xFFFFFFF0u) return fail(Status::Encode, "the collection is too large");
  util::ByteWriter out(static_cast<std::size_t>(at + 4));
  out.text("ttcf").u32be(0x00020000u).u32be(static_cast<std::uint32_t>(faces.size()));
  for (const std::uint64_t offset : directory_offsets) out.u32be(static_cast<std::uint32_t>(offset));
  out.u32be(0).u32be(0).u32be(0);
  for (std::size_t f = 0; f < faces.size(); ++f) {
    write_offset_table(out, output_sfnt_version(*faces[f]), static_cast<std::uint16_t>(directories[f].size()));
    for (const auto& [tag, index] : directories[f]) {
      out.u32be(tag_value(tag)).u32be(table_checksum(blobs[index]));
      out.u32be(static_cast<std::uint32_t>(blob_offsets[index])).u32be(static_cast<std::uint32_t>(blobs[index].size()));
    }
  }
  for (const Bytes& blob : blobs) {
    out.align(4);
    out.bytes(blob);
  }
  out.align(4);
  return out.take();
}

std::vector<const ir::Font*> all_faces(const ir::Font& font) {
  std::vector<const ir::Font*> faces{&font};
  for (const ir::Font& other : font.other_faces) faces.push_back(&other);
  return faces;
}

}  // namespace cvt::font
