// Convertible core - office family: Microsoft "InfoTech Storage" containers
// (CHM and LIT directories, sections and their LZX transform).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/office/its.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <format>
#include <span>
#include <utility>

#include "families/archive/compression.hpp"
#include "util/text.hpp"

namespace cvt::office::its {

namespace {

namespace cz = cvt::compression;

constexpr std::uint64_t frame_size = 0x8000;
constexpr std::string_view chm_lzx_guid = "{7FC28940-9D31-11D0-9B27-00A0C91E9C7C}";
constexpr std::string_view lit_lzx_guid = "{0A9007C6-4076-11D3-8789-0000F8105754}";
constexpr std::string_view lit_des_guid = "{67F6E4A2-60BF-11D3-8540-00C04F58C3CF}";

std::uint32_t le16(ByteView d, std::size_t at) {
  return std::to_integer<std::uint32_t>(d[at]) | (std::to_integer<std::uint32_t>(d[at + 1]) << 8);
}
std::uint32_t le32(ByteView d, std::size_t at) {
  return le16(d, at) | (le16(d, at + 2) << 16);
}
std::uint64_t le64(ByteView d, std::size_t at) {
  return static_cast<std::uint64_t>(le32(d, at)) | (static_cast<std::uint64_t>(le32(d, at + 4)) << 32);
}

// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" from a 16-byte binary GUID.
std::string guid_text(ByteView g) {
  const auto b = [&](std::size_t i) { return std::to_integer<unsigned>(g[i]); };
  return std::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}", le32(g, 0), le16(g, 4),
                     le16(g, 6), b(8), b(9), b(10), b(11), b(12), b(13), b(14), b(15));
}

std::string utf16_ascii(ByteView d) {
  std::string out;
  for (std::size_t i = 0; i + 1 < d.size(); i += 2) {
    const std::uint32_t c = le16(d, i);
    if (c == 0) break;
    out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return out;
}

}  // namespace

std::optional<std::uint64_t> read_encint(ByteView d, std::size_t& pos, std::size_t end) {
  std::uint64_t v = 0;
  for (int i = 0; i < 9; ++i) {
    if (pos >= end || pos >= d.size()) return std::nullopt;
    const auto b = std::to_integer<std::uint8_t>(d[pos++]);
    v = (v << 7) | (b & 0x7Fu);
    if ((b & 0x80u) == 0) return v;
  }
  return std::nullopt;
}

Result<void> Storage::add_entry(std::string name, Entry entry) {
  if (entries_.size() >= limits_.max_archive_entries + 256) return fail(Status::Limit, "the file lists too many entries");
  std::string folded = util::to_lower(name);
  folded_.try_emplace(std::move(folded), name);
  entries_.insert_or_assign(std::move(name), entry);
  return {};
}

std::optional<std::string> Storage::find(std::string_view name) const {
  if (entries_.contains(std::string(name))) return std::string(name);
  if (const auto it = folded_.find(util::to_lower(name)); it != folded_.end()) return it->second;
  return std::nullopt;
}

Result<ByteView> Storage::stored(const Entry& entry) const {
  const std::uint64_t at = content_offset_ + entry.offset;
  if (at < content_offset_ || at > data_.size() || entry.length > data_.size() - at) {
    return fail(Status::Decode, "the file is truncated (an entry lies beyond its end)");
  }
  return data_.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(entry.length));
}

// --------------------------------------------------------------------------- CHM

Result<Storage> Storage::open_chm(ByteView data, const Limits& limits) {
  Storage s(data, limits, false);
  if (data.size() < 0x58 || as_chars(data.first(4)) != "ITSF") return fail(Status::Decode, "not a CHM file");
  const std::uint32_t version = le32(data, 4);
  const std::uint32_t header_len = le32(data, 8);
  const std::uint64_t dir_offset = le64(data, 72);
  const std::uint64_t dir_length = le64(data, 80);
  if (dir_offset > data.size() || dir_length > data.size() - dir_offset || dir_length < 0x54) {
    return fail(Status::Decode, "the CHM directory is truncated");
  }
  s.content_offset_ = (version >= 3 && header_len >= 0x60 && data.size() >= 0x60) ? le64(data, 88) : dir_offset + dir_length;
  const ByteView dir = data.subspan(static_cast<std::size_t>(dir_offset), static_cast<std::size_t>(dir_length));
  if (as_chars(dir.first(4)) != "ITSP") return fail(Status::Decode, "the CHM directory header is damaged");
  const std::uint32_t itsp_len = le32(dir, 8);
  const std::uint32_t chunk_size = le32(dir, 16);
  const std::uint32_t chunks = le32(dir, 44);
  if (itsp_len > dir.size() || chunk_size < 32 || chunk_size > (1u << 20)) {
    return fail(Status::Decode, "the CHM directory header is damaged");
  }
  const ByteView area = dir.subspan(itsp_len);
  if (chunks > area.size() / chunk_size) return fail(Status::Decode, "the CHM directory is truncated");
  for (std::uint32_t c = 0; c < chunks; ++c) {
    const ByteView chunk = area.subspan(static_cast<std::size_t>(c) * chunk_size, chunk_size);
    if (as_chars(chunk.first(4)) != "PMGL") continue;  // index chunks (PMGI) are not needed
    const std::uint32_t free = le32(chunk, 4);
    if (free > chunk_size - 20) return fail(Status::Decode, "a CHM listing chunk is damaged");
    const std::size_t end = chunk_size - free;
    std::size_t pos = 20;
    while (pos < end) {
      const auto name_len = read_encint(chunk, pos, end);
      if (!name_len || *name_len > end - pos) return fail(Status::Decode, "a CHM listing entry is damaged");
      std::string name(as_chars(chunk.subspan(pos, static_cast<std::size_t>(*name_len))));
      pos += static_cast<std::size_t>(*name_len);
      const auto section = read_encint(chunk, pos, end);
      const auto offset = read_encint(chunk, pos, end);
      const auto length = read_encint(chunk, pos, end);
      if (!section || !offset || !length) return fail(Status::Decode, "a CHM listing entry is damaged");
      CVT_TRY(s.add_entry(std::move(name), Entry{*section, *offset, *length}));
    }
  }
  if (s.entries_.empty()) return fail(Status::Decode, "the CHM directory is empty");
  return s;
}

// --------------------------------------------------------------------------- LIT

Result<Storage> Storage::open_lit(ByteView data, const Limits& limits) {
  Storage s(data, limits, true);
  if (data.size() < 0x28 || as_chars(data.first(8)) != "ITOLITLS") return fail(Status::Decode, "not a Microsoft Reader book");
  if (le32(data, 8) != 1) return fail(Status::Decode, "Microsoft Reader book version {} is not supported", le32(data, 8));
  const std::uint64_t header_len = le32(data, 12);
  const std::uint64_t pieces = le32(data, 16);
  const std::uint64_t secondary_len = le32(data, 20);
  if (header_len < 0x28 || pieces < 2 || pieces > 64) return fail(Status::Decode, "the book header is damaged");
  const std::uint64_t secondary_at = header_len + pieces * 16;
  if (secondary_at > data.size() || secondary_len > data.size() - secondary_at) {
    return fail(Status::Decode, "the book header is truncated");
  }
  // The secondary header: CAOL and ITSF blocks; ITSF holds the content offset.
  const ByteView secondary = data.subspan(static_cast<std::size_t>(secondary_at), static_cast<std::size_t>(secondary_len));
  if (secondary.size() < 8) return fail(Status::Decode, "the book header is damaged");
  bool have_offset = false;
  for (std::uint64_t at = le32(secondary, 4); at + 8 <= secondary.size();) {
    const std::string_view tag = as_chars(secondary.subspan(static_cast<std::size_t>(at), 4));
    if (tag == "ITSF") {
      if (at + 20 > secondary.size()) break;
      s.content_offset_ = le32(secondary, static_cast<std::size_t>(at) + 16);
      have_offset = true;
    } else if (tag != "CAOL") {
      break;
    }
    at += 48;
  }
  if (!have_offset || s.content_offset_ > data.size()) return fail(Status::Decode, "the book header lacks its content offset");
  // Piece 1 is the directory: IFCM header and AOLL listing chunks.
  const std::size_t piece = static_cast<std::size_t>(header_len) + 16;
  if (le32(data, piece + 4) != 0 || le32(data, piece + 12) != 0) return fail(Status::Decode, "the book directory is too large");
  const std::uint64_t dir_offset = le32(data, piece);
  const std::uint64_t dir_length = le32(data, piece + 8);
  if (dir_offset > data.size() || dir_length > data.size() - dir_offset || dir_length < 32) {
    return fail(Status::Decode, "the book directory is truncated");
  }
  const ByteView dir = data.subspan(static_cast<std::size_t>(dir_offset), static_cast<std::size_t>(dir_length));
  if (as_chars(dir.first(4)) != "IFCM") return fail(Status::Decode, "the book directory is damaged");
  const std::uint64_t chunk_size = le32(dir, 8);
  const std::uint64_t chunks = le32(dir, 24);
  if (chunk_size < 64 || chunk_size > (1u << 20) || chunks > (dir.size() - 32) / chunk_size) {
    return fail(Status::Decode, "the book directory is damaged");
  }
  for (std::uint64_t c = 0; c < chunks; ++c) {
    const ByteView chunk = dir.subspan(32 + static_cast<std::size_t>(c * chunk_size), static_cast<std::size_t>(chunk_size));
    if (as_chars(chunk.first(4)) != "AOLL") continue;  // AOLI index chunks are not needed
    const std::uint64_t quickref = le32(chunk, 4);
    if (quickref + 48 > chunk_size) return fail(Status::Decode, "a book directory chunk is damaged");
    const std::size_t end = static_cast<std::size_t>(chunk_size - quickref);
    std::size_t pos = 48;
    while (pos < end) {
      const auto name_len = read_encint(chunk, pos, end);
      if (name_len && *name_len == 0) break;  // padding
      if (!name_len || *name_len > end - pos) return fail(Status::Decode, "a book directory entry is damaged");
      std::string name(as_chars(chunk.subspan(pos, static_cast<std::size_t>(*name_len))));
      pos += static_cast<std::size_t>(*name_len);
      const auto section = read_encint(chunk, pos, end);
      const auto offset = read_encint(chunk, pos, end);
      const auto length = read_encint(chunk, pos, end);
      if (!section || !offset || !length) return fail(Status::Decode, "a book directory entry is damaged");
      if (name.empty()) continue;
      CVT_TRY(s.add_entry(std::move(name), Entry{*section, *offset, *length}));
    }
  }
  if (s.entries_.empty()) return fail(Status::Decode, "the book directory is empty");
  return s;
}

// --------------------------------------------------------------------------- sections

Result<void> Storage::read_section_names() {
  if (names_read_) return {};
  names_read_ = true;
  const auto key = find("::DataSpace/NameList");
  if (!key) {
    // Old CHM files: one compressed section.
    section_names_ = {"Uncompressed", "MSCompressed"};
    return {};
  }
  const Entry& entry = entries_.at(*key);
  if (entry.section != 0) return fail(Status::Decode, "the section name list is damaged");
  CVT_TRY_ASSIGN(const ByteView raw, stored(entry));
  if (raw.size() < 4) return fail(Status::Decode, "the section name list is damaged");
  const std::uint32_t count = le16(raw, 2);
  std::size_t pos = 4;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (pos + 2 > raw.size()) return fail(Status::Decode, "the section name list is truncated");
    const std::size_t length = le16(raw, pos);
    pos += 2;
    if (length * 2 > raw.size() - pos) return fail(Status::Decode, "the section name list is truncated");
    section_names_.push_back(utf16_ascii(raw.subspan(pos, length * 2)));
    pos += length * 2 + 2;
  }
  return {};
}

Result<Bytes> Storage::lzx(std::string_view storage, ByteView content, ByteView control, std::string_view guid) {
  if (!cz::can_decode(cz::Method::Lzx)) return fail(Status::Decode, "LZX compressed content cannot be read by this build");
  if (control.size() < 24 || as_chars(control.subspan(4, 4)) != "LZXC") {
    return fail(Status::Decode, "the compression parameters are damaged");
  }
  std::uint64_t interval = 0;
  std::uint64_t window = 0;
  if (lit_) {
    // Microsoft Reader: the window in 32 KiB units; the stream resets once per window.
    window = static_cast<std::uint64_t>(le32(control, 12)) * frame_size;
    interval = window;
  } else {
    interval = le32(control, 12);
    window = le32(control, 16);
    if (le32(control, 8) >= 2) {
      interval *= frame_size;
      window *= frame_size;
    }
  }
  if (!std::has_single_bit(window) || window < (1u << 15) || window > (1u << 21) || interval < frame_size ||
      interval % frame_size != 0 || interval > (1u << 26)) {
    return fail(Status::Decode, "the compression parameters are not supported");
  }
  const std::string reset_name = std::string(storage) + "/Transform/" + std::string(guid) + "/InstanceData/ResetTable";
  const auto reset_key = find(reset_name);
  if (!reset_key) return fail(Status::Decode, "the compression reset table is missing");
  const Entry& reset_entry = entries_.at(*reset_key);
  if (reset_entry.section != 0) return fail(Status::Decode, "the compression reset table is damaged");
  CVT_TRY_ASSIGN(const ByteView reset, stored(reset_entry));
  if (reset.size() < 40) return fail(Status::Decode, "the compression reset table is damaged");
  const std::uint32_t count = le32(reset, 4);
  const std::uint32_t entry_size = le32(reset, 8);
  const std::uint32_t header = le32(reset, 12);
  const std::uint64_t uncompressed = le64(reset, 16);
  const std::uint64_t compressed = le64(reset, 24);
  if (entry_size != 8 || header > reset.size() || count > (reset.size() - header) / 8 || le64(reset, 32) != frame_size) {
    return fail(Status::Decode, "the compression reset table is damaged");
  }
  if (uncompressed > limits_.max_decompressed_bytes || decompressed_ + uncompressed > limits_.max_decompressed_bytes) {
    return fail(Status::Limit, "{} bytes of compressed content exceed the limit", uncompressed);
  }
  if ((uncompressed + frame_size - 1) / frame_size > count) return fail(Status::Decode, "the compression reset table is too short");
  if (compressed < content.size()) content = content.first(static_cast<std::size_t>(compressed));
  std::vector<std::uint64_t> offsets(count);
  for (std::uint32_t i = 0; i < count; ++i) offsets[i] = le64(reset, header + static_cast<std::size_t>(i) * 8);
  const int window_bits = std::countr_zero(window);
  const std::uint64_t per_interval = interval / frame_size;
  Bytes out;
  std::vector<std::uint32_t> frames;
  for (std::uint64_t start = 0, k = 0; start < uncompressed; start += interval, ++k) {
    const std::uint64_t size = std::min(interval, uncompressed - start);
    const std::uint64_t first = k * per_interval;
    const std::uint64_t next = first + per_interval;
    const std::uint64_t from = offsets[static_cast<std::size_t>(first)];
    const std::uint64_t to = next < offsets.size() ? offsets[static_cast<std::size_t>(next)] : content.size();
    if (from > to || to > content.size()) return fail(Status::Decode, "the compression reset table is damaged");
    frames.clear();
    for (std::uint64_t done = 0; done < size; done += frame_size) {
      frames.push_back(static_cast<std::uint32_t>(std::min(frame_size, size - done)));
    }
    cz::DecodeOptions d;
    d.window_bits = window_bits;
    d.block_sizes = frames;
    d.output_size = size;
    d.max_output = size;
    CVT_TRY_ASSIGN(Bytes part, cz::decompress(cz::Method::Lzx,
                                              content.subspan(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)), d));
    if (part.size() != size) return fail(Status::Decode, "compressed content decoded to the wrong size");
    out.insert(out.end(), part.begin(), part.end());
  }
  decompressed_ += out.size();
  return out;
}

Result<const Bytes*> Storage::section(std::uint64_t index) {
  if (const auto it = sections_.find(index); it != sections_.end()) return &it->second;
  CVT_TRY(read_section_names());
  if (index >= section_names_.size()) return fail(Status::Decode, "content section {} is not known", index);
  const std::string storage = "::DataSpace/Storage/" + section_names_[static_cast<std::size_t>(index)];
  const auto read_stored = [this](const std::string& name) -> Result<ByteView> {
    const auto key = find(name);
    if (!key) return fail(Status::Decode, "the file lacks {}", name);
    const Entry& entry = entries_.at(*key);
    if (entry.section != 0) return fail(Status::Decode, "{} is damaged", name);
    return stored(entry);
  };
  CVT_TRY_ASSIGN(const ByteView content, read_stored(storage + "/Content"));
  CVT_TRY_ASSIGN(ByteView control, read_stored(storage + "/ControlData"));
  Bytes data;
  if (!lit_) {
    // CHM: the compressed section is LZX (its transform list names it as text).
    CVT_TRY_ASSIGN(data, lzx(storage, content, control, chm_lzx_guid));
  } else {
    CVT_TRY_ASSIGN(ByteView list, read_stored(storage + "/Transform/List"));
    data = to_bytes(content);
    if (list.size() < 16) return fail(Status::Decode, "the content section {} has no transform", section_names_[static_cast<std::size_t>(index)]);
    for (; list.size() >= 16; list = list.subspan(16)) {
      if (control.size() < 4) return fail(Status::Decode, "the compression parameters are truncated");
      const std::uint64_t used = (static_cast<std::uint64_t>(le32(control, 0)) + 1) * 4;
      if (used > control.size()) return fail(Status::Decode, "the compression parameters are truncated");
      const std::string guid = guid_text(list.first(16));
      if (guid == lit_des_guid) {
        return fail(Status::Decode,
                    "the book is protected by Microsoft Reader DRM (encrypted content); protected books cannot be converted");
      }
      if (guid != lit_lzx_guid) return fail(Status::Decode, "the book uses an unknown content transform {}", guid);
      CVT_TRY_ASSIGN(data, lzx(storage, data, control.first(static_cast<std::size_t>(used)), lit_lzx_guid));
      control = control.subspan(static_cast<std::size_t>(used));
    }
  }
  return &sections_.emplace(index, std::move(data)).first->second;
}

Result<Bytes> Storage::read(std::string_view name) {
  const auto key = find(name);
  if (!key) return fail(Status::Decode, "the file lacks {}", name);
  const Entry entry = entries_.at(*key);
  if (entry.length > limits_.max_decompressed_bytes) return fail(Status::Limit, "{} is too large", name);
  if (entry.section == 0) {
    CVT_TRY_ASSIGN(const ByteView view, stored(entry));
    return to_bytes(view);
  }
  CVT_TRY_ASSIGN(const Bytes* data, section(entry.section));
  if (entry.offset > data->size() || entry.length > data->size() - entry.offset) {
    return fail(Status::Decode, "{} lies beyond its content section", name);
  }
  const auto begin = data->begin() + static_cast<std::ptrdiff_t>(entry.offset);
  return Bytes(begin, begin + static_cast<std::ptrdiff_t>(entry.length));
}

}  // namespace cvt::office::its
