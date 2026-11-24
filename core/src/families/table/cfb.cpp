// Convertible core - Compound File Binary format reader and writer ([MS-CFB]).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/table/cfb.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "util/text.hpp"

namespace cvt::table {

namespace {

constexpr std::uint32_t free_sector = 0xFFFFFFFFu;
constexpr std::uint32_t end_of_chain = 0xFFFFFFFEu;
constexpr std::uint32_t fat_sector = 0xFFFFFFFDu;
constexpr std::uint32_t difat_sector = 0xFFFFFFFCu;
constexpr std::uint32_t no_stream = 0xFFFFFFFFu;
constexpr std::size_t header_size = 512;
constexpr std::size_t entry_size = 128;
constexpr std::size_t mini_sector_size = 64;
constexpr std::size_t max_depth = 64;

constexpr std::array<std::uint8_t, 8> signature = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

// CFB compares names by length first, then by upper-cased UTF-16 units.
char16_t upper_unit(char16_t c) {
  if (c >= u'a' && c <= u'z') return static_cast<char16_t>(c - u'a' + u'A');
  // Latin-1 supplement lower case letters (except the German sharp s and ÿ).
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return static_cast<char16_t>(c - 0x20);
  return c;
}

bool name_less(const std::u16string& a, const std::u16string& b) {
  if (a.size() != b.size()) return a.size() < b.size();
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char16_t x = upper_unit(a[i]);
    const char16_t y = upper_unit(b[i]);
    if (x != y) return x < y;
  }
  return false;
}

bool names_equal(std::string_view a, std::string_view b) { return util::iequals(a, b); }

}  // namespace

// --------------------------------------------------------------------------- reading

ByteView CfbReader::sector(std::uint32_t index) const noexcept {
  const std::uint64_t offset = (std::uint64_t{index} + 1) * sector_size_;
  if (offset >= data_.size()) return {};
  return data_.subspan(static_cast<std::size_t>(offset),
                       static_cast<std::size_t>(std::min<std::uint64_t>(sector_size_, data_.size() - offset)));
}

Result<std::vector<std::uint32_t>> CfbReader::chain(std::uint32_t start, const std::vector<std::uint32_t>& fat,
                                                    std::uint64_t max_length) const {
  std::vector<std::uint32_t> out;
  std::vector<bool> seen(fat.size(), false);
  std::uint32_t current = start;
  while (current != end_of_chain) {
    if (current >= fat.size()) return fail(Status::Decode, "compound file chain points outside the file (sector {})", current);
    if (seen[current]) return fail(Status::Decode, "compound file sector chain loops back on itself");
    seen[current] = true;
    out.push_back(current);
    if (out.size() > max_length) return fail(Status::Decode, "compound file sector chain is longer than its stream");
    current = fat[current];
  }
  return out;
}

Result<CfbReader> CfbReader::open(ByteView data, const Limits& limits) {
  if (data.size() < header_size) return fail(Status::Decode, "too small to be a compound file");
  if (std::memcmp(data.data(), signature.data(), signature.size()) != 0) {
    return fail(Status::Decode, "not a compound file (OLE2 signature missing)");
  }
  util::ByteReader header(data.first(header_size));
  header.seek(26);
  const std::uint16_t major = header.u16le();
  const std::uint16_t byte_order = header.u16le();
  const std::uint16_t sector_shift = header.u16le();
  const std::uint16_t mini_shift = header.u16le();
  if (byte_order != 0xFFFE) return fail(Status::Decode, "compound file has an invalid byte order mark");
  if (sector_shift != 9 && sector_shift != 12) return fail(Status::Decode, "unsupported compound file sector size");
  if (mini_shift != 6) return fail(Status::Decode, "unsupported compound file mini sector size");
  (void)major;
  header.seek(44);
  const std::uint32_t fat_count = header.u32le();
  const std::uint32_t first_directory = header.u32le();
  (void)header.u32le();  // transaction signature
  const std::uint32_t mini_cutoff = header.u32le();
  const std::uint32_t first_mini_fat = header.u32le();
  const std::uint32_t mini_fat_count = header.u32le();
  const std::uint32_t first_difat = header.u32le();
  const std::uint32_t difat_count = header.u32le();

  CfbReader cfb;
  cfb.data_ = data;
  cfb.limits_ = limits;
  cfb.sector_size_ = 1u << sector_shift;
  cfb.mini_cutoff_ = mini_cutoff == 0 ? 4096 : mini_cutoff;
  const std::uint64_t ss = cfb.sector_size_;
  if (data.size() <= ss) return fail(Status::Decode, "the compound file has no sectors");
  cfb.sector_count_ = static_cast<std::uint32_t>(std::min<std::uint64_t>((data.size() - ss + ss - 1) / ss, 0xFFFFFFF0u));
  if (fat_count > cfb.sector_count_) return fail(Status::Decode, "compound file declares more FAT sectors than it has");

  // The FAT sectors: 109 in the header, the rest in the DIFAT chain.
  std::vector<std::uint32_t> fat_sectors;
  for (std::uint32_t i = 0; i < 109 && fat_sectors.size() < fat_count; ++i) {
    header.seek(76 + i * 4);
    fat_sectors.push_back(header.u32le());
  }
  {
    std::uint32_t current = first_difat;
    std::vector<bool> seen(cfb.sector_count_, false);
    for (std::uint32_t k = 0; k < difat_count && fat_sectors.size() < fat_count && current != end_of_chain; ++k) {
      if (current >= cfb.sector_count_ || seen[current]) return fail(Status::Decode, "damaged compound file DIFAT chain");
      seen[current] = true;
      util::ByteReader difat(cfb.sector(current));
      const std::uint32_t per_sector = cfb.sector_size_ / 4 - 1;
      for (std::uint32_t i = 0; i < per_sector && fat_sectors.size() < fat_count; ++i) fat_sectors.push_back(difat.u32le());
      difat.seek(per_sector * 4);
      current = difat.u32le();
    }
  }
  if (fat_sectors.size() < fat_count) return fail(Status::Decode, "compound file FAT is incomplete");
  cfb.fat_.reserve(static_cast<std::size_t>(fat_count) * (cfb.sector_size_ / 4));
  for (const std::uint32_t s : fat_sectors) {
    if (s >= cfb.sector_count_) return fail(Status::Decode, "compound file FAT sector {} is outside the file", s);
    util::ByteReader fat(cfb.sector(s));
    for (std::uint32_t i = 0; i < cfb.sector_size_ / 4; ++i) {
      const std::uint32_t value = fat.u32le();
      cfb.fat_.push_back(fat.ok() ? value : free_sector);
    }
  }
  if (cfb.fat_.size() > cfb.sector_count_) cfb.fat_.resize(cfb.sector_count_);

  // Directory.
  CVT_TRY_ASSIGN(const auto directory_sectors, cfb.chain(first_directory, cfb.fat_, cfb.sector_count_));
  std::vector<CfbEntry> raw;
  std::vector<std::uint8_t> raw_type;
  for (const std::uint32_t s : directory_sectors) {
    const ByteView bytes = cfb.sector(s);
    for (std::size_t offset = 0; offset + entry_size <= bytes.size(); offset += entry_size) {
      util::ByteReader e(bytes.subspan(offset, entry_size));
      std::u16string name(32, u'\0');
      for (char16_t& unit : name) unit = static_cast<char16_t>(e.u16le());
      const std::uint16_t name_bytes = e.u16le();
      const std::uint8_t type = e.u8();
      (void)e.u8();  // colour
      CfbEntry entry;
      entry.left = e.u32le();
      entry.right = e.u32le();
      entry.child = e.u32le();
      for (std::uint8_t& b : entry.clsid) b = e.u8();
      e.skip(4 + 8 + 8);
      entry.start_sector = e.u32le();
      entry.size = e.u64le();
      if (cfb.sector_size_ == 512) entry.size &= 0xFFFFFFFFu;  // version 3 keeps garbage in the high half
      const std::size_t units = name_bytes >= 2 && name_bytes <= 64 ? name_bytes / 2 - 1 : 0;
      name.resize(units);
      entry.name = util::utf16_to_utf8(name);
      raw.push_back(std::move(entry));
      raw_type.push_back(type);
      if (raw.size() > limits.max_archive_entries * 4 + 16) return fail(Status::Limit, "the compound file directory is too large");
    }
  }
  if (raw.empty() || raw_type[0] != 5) return fail(Status::Decode, "compound file has no root entry");

  // Mini stream and mini FAT.
  CfbEntry& root = raw[0];
  if (root.size > 0) {
    CVT_TRY(limits.check_decompressed(root.size));
    CVT_TRY_ASSIGN(const auto sectors, cfb.chain(root.start_sector, cfb.fat_, cfb.sector_count_));
    if (std::uint64_t{sectors.size()} * ss < root.size) return fail(Status::Decode, "the compound file mini stream is truncated");
    cfb.mini_stream_.reserve(static_cast<std::size_t>(root.size));
    for (const std::uint32_t s : sectors) {
      const ByteView bytes = cfb.sector(s);
      cfb.mini_stream_.insert(cfb.mini_stream_.end(), bytes.begin(), bytes.end());
      cfb.mini_stream_.resize(cfb.mini_stream_.size() + (ss - bytes.size()), std::byte{0});
    }
    cfb.mini_stream_.resize(static_cast<std::size_t>(root.size));
  }
  if (mini_fat_count > 0 && first_mini_fat != end_of_chain) {
    CVT_TRY_ASSIGN(const auto sectors, cfb.chain(first_mini_fat, cfb.fat_, cfb.sector_count_));
    for (const std::uint32_t s : sectors) {
      util::ByteReader r(cfb.sector(s));
      for (std::uint32_t i = 0; i < cfb.sector_size_ / 4; ++i) {
        const std::uint32_t value = r.u32le();
        cfb.mini_fat_.push_back(r.ok() ? value : free_sector);
      }
    }
  }

  // Walk the tree: storages' children are red-black trees of siblings.
  std::vector<bool> visited(raw.size(), false);
  const auto siblings_in_order = [&](std::uint32_t first) -> Result<std::vector<std::uint32_t>> {
    std::vector<std::uint32_t> out;
    std::vector<std::uint32_t> stack;
    std::uint32_t current = first;
    while (current != no_stream || !stack.empty()) {
      while (current != no_stream) {
        if (current >= raw.size()) return fail(Status::Decode, "compound file directory link {} is out of range", current);
        if (visited[current]) return fail(Status::Decode, "compound file directory tree is cyclic");
        visited[current] = true;
        stack.push_back(current);
        if (stack.size() > raw.size()) return fail(Status::Decode, "compound file directory tree is cyclic");
        current = raw[current].left;
      }
      current = stack.back();
      stack.pop_back();
      out.push_back(current);
      current = raw[current].right;
    }
    return out;
  };
  visited[0] = true;
  CfbEntry root_entry = raw[0];
  root_entry.type = CfbEntryType::Root;
  root_entry.path.clear();
  cfb.entries_.push_back(root_entry);
  struct Pending {
    std::uint32_t id;
    std::size_t parent;
    std::size_t depth;
  };
  std::vector<Pending> work;
  {
    CVT_TRY_ASSIGN(const auto kids, siblings_in_order(raw[0].child));
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) work.push_back({*it, 0, 1});
  }
  while (!work.empty()) {
    const Pending item = work.back();
    work.pop_back();
    const std::uint8_t type = raw_type[item.id];
    if (type != 1 && type != 2) continue;  // unused or unknown entries
    CfbEntry entry = raw[item.id];
    entry.type = type == 1 ? CfbEntryType::Storage : CfbEntryType::Stream;
    entry.parent = item.parent;
    const std::string& parent_path = cfb.entries_[item.parent].path;
    entry.path = parent_path.empty() ? entry.name : parent_path + "/" + entry.name;
    if (entry.type == CfbEntryType::Storage) entry.size = 0;
    CVT_TRY(limits.check_archive_entries(cfb.entries_.size()));
    cfb.entries_.push_back(std::move(entry));
    const std::size_t index = cfb.entries_.size() - 1;
    if (type == 1) {
      if (item.depth >= max_depth) return fail(Status::Decode, "compound file storages are nested too deeply");
      CVT_TRY_ASSIGN(const auto kids, siblings_in_order(raw[item.id].child));
      for (auto it = kids.rbegin(); it != kids.rend(); ++it) work.push_back({*it, index, item.depth + 1});
    }
  }
  return cfb;
}

std::optional<std::size_t> CfbReader::find(std::string_view path) const noexcept {
  while (path.starts_with('/')) path.remove_prefix(1);
  for (std::size_t i = 1; i < entries_.size(); ++i) {
    if (names_equal(entries_[i].path, path)) return i;
  }
  return std::nullopt;
}

std::vector<std::size_t> CfbReader::children(std::size_t storage) const {
  std::vector<std::size_t> out;
  for (std::size_t i = 1; i < entries_.size(); ++i) {
    if (entries_[i].parent == storage) out.push_back(i);
  }
  return out;
}

Result<Bytes> CfbReader::read(std::size_t index) const {
  if (index >= entries_.size()) return fail(Status::Decode, "no such compound file entry");
  const CfbEntry& entry = entries_[index];
  if (entry.type != CfbEntryType::Stream) return fail(Status::Decode, "'{}' is a storage, not a stream", entry.path);
  if (entry.size == 0) return Bytes{};
  CVT_TRY(limits_.check_decompressed(handed_out_ + entry.size));
  Bytes out;
  if (entry.size < mini_cutoff_) {
    CVT_TRY_ASSIGN(const auto sectors, chain(entry.start_sector, mini_fat_, mini_stream_.size() / mini_sector_size + 1));
    if (std::uint64_t{sectors.size()} * mini_sector_size < entry.size) {
      return fail(Status::Decode, "compound file stream '{}' is truncated", entry.path);
    }
    out.reserve(static_cast<std::size_t>(entry.size));
    for (const std::uint32_t s : sectors) {
      const std::uint64_t offset = std::uint64_t{s} * mini_sector_size;
      if (offset + mini_sector_size > mini_stream_.size()) {
        return fail(Status::Decode, "compound file stream '{}' points outside the mini stream", entry.path);
      }
      out.insert(out.end(), mini_stream_.begin() + static_cast<std::ptrdiff_t>(offset),
                 mini_stream_.begin() + static_cast<std::ptrdiff_t>(offset + mini_sector_size));
    }
  } else {
    CVT_TRY_ASSIGN(const auto sectors, chain(entry.start_sector, fat_, entry.size / sector_size_ + 1));
    if (std::uint64_t{sectors.size()} * sector_size_ < entry.size) {
      return fail(Status::Decode, "compound file stream '{}' is truncated", entry.path);
    }
    out.reserve(static_cast<std::size_t>(entry.size));
    for (const std::uint32_t s : sectors) {
      const ByteView bytes = sector(s);
      out.insert(out.end(), bytes.begin(), bytes.end());
      out.resize(out.size() + (sector_size_ - bytes.size()), std::byte{0});
    }
  }
  out.resize(static_cast<std::size_t>(entry.size));
  handed_out_ += entry.size;
  return out;
}

Result<Bytes> CfbReader::read(std::string_view path) const {
  const auto index = find(path);
  if (!index) return fail(Status::Decode, "the compound file has no stream '{}'", path);
  return read(*index);
}

// --------------------------------------------------------------------------- writing

Result<std::size_t> CfbWriter::node_for(std::string_view path, bool storage) {
  while (path.starts_with('/')) path.remove_prefix(1);
  if (path.empty()) return fail(Status::InvalidArgument, "compound file paths must not be empty");
  std::size_t current = 0;
  const auto segments = util::split(path, '/', false);
  for (std::size_t k = 0; k < segments.size(); ++k) {
    const bool last = k + 1 == segments.size();
    const std::u16string name = util::utf8_to_utf16(segments[k]);
    if (name.empty() || name.size() > 31 || segments[k].find_first_of("\\:!") != std::string_view::npos) {
      return fail(Status::InvalidArgument, "invalid compound file entry name '{}'", segments[k]);
    }
    std::size_t found = nodes_.size();
    for (const std::size_t child : nodes_[current].children) {
      if (!name_less(nodes_[child].name, name) && !name_less(name, nodes_[child].name)) found = child;
    }
    const bool want_storage = !last || storage;
    if (found == nodes_.size()) {
      Node node;
      node.name = name;
      node.storage = want_storage;
      nodes_.push_back(std::move(node));
      nodes_[current].children.push_back(found);
    } else if (nodes_[found].storage != want_storage) {
      return fail(Status::InvalidArgument, "'{}' is already used for a {}", segments[k],
                  nodes_[found].storage ? "storage" : "stream");
    }
    current = found;
  }
  return current;
}

Result<void> CfbWriter::add_stream(std::string_view path, ByteView data) {
  if (data.size() > 0xFFFFFFFFull) return fail(Status::Encode, "compound file streams are limited to 4 GiB");
  CVT_TRY_ASSIGN(const std::size_t index, node_for(path, false));
  nodes_[index].data.assign(data.begin(), data.end());
  return {};
}

Result<void> CfbWriter::add_storage(std::string_view path, const std::array<std::uint8_t, 16>& clsid) {
  CVT_TRY_ASSIGN(const std::size_t index, node_for(path, true));
  nodes_[index].clsid = clsid;
  return {};
}

Result<Bytes> CfbWriter::finish() const {
  constexpr std::uint32_t ss = 512;
  constexpr std::uint32_t cutoff = 4096;
  const std::size_t count = nodes_.size();

  // Sibling trees: balanced binary trees over the CFB name order.
  std::vector<std::uint32_t> left(count, no_stream);
  std::vector<std::uint32_t> right(count, no_stream);
  std::vector<std::uint32_t> child(count, no_stream);
  for (std::size_t i = 0; i < count; ++i) {
    if (!nodes_[i].storage) continue;
    std::vector<std::size_t> kids = nodes_[i].children;
    std::ranges::sort(kids, [&](std::size_t a, std::size_t b) { return name_less(nodes_[a].name, nodes_[b].name); });
    const std::function<std::uint32_t(std::size_t, std::size_t)> build = [&](std::size_t lo, std::size_t hi) -> std::uint32_t {
      if (lo >= hi) return no_stream;
      const std::size_t mid = lo + (hi - lo) / 2;
      const auto id = static_cast<std::uint32_t>(kids[mid]);
      left[id] = build(lo, mid);
      right[id] = build(mid + 1, hi);
      return id;
    };
    child[i] = build(0, kids.size());
  }

  // Mini stream for small streams.
  std::vector<std::uint32_t> start(count, end_of_chain);
  Bytes mini_stream;
  std::vector<std::uint32_t> mini_fat;
  for (std::size_t i = 1; i < count; ++i) {
    const Node& node = nodes_[i];
    if (node.storage || node.data.empty() || node.data.size() >= cutoff) continue;
    const auto first = static_cast<std::uint32_t>(mini_stream.size() / mini_sector_size);
    const auto sectors = static_cast<std::uint32_t>((node.data.size() + mini_sector_size - 1) / mini_sector_size);
    start[i] = first;
    for (std::uint32_t k = 0; k < sectors; ++k) mini_fat.push_back(k + 1 < sectors ? first + k + 1 : end_of_chain);
    mini_stream.insert(mini_stream.end(), node.data.begin(), node.data.end());
    mini_stream.resize(static_cast<std::size_t>(first + sectors) * mini_sector_size, std::byte{0});
  }

  // Regular sectors: large streams, the mini stream, the mini FAT, the directory, then FAT and DIFAT.
  std::vector<std::uint32_t> fat;
  const auto allocate = [&](std::uint64_t bytes) -> std::uint32_t {
    if (bytes == 0) return end_of_chain;
    const auto first = static_cast<std::uint32_t>(fat.size());
    const auto sectors = static_cast<std::uint32_t>((bytes + ss - 1) / ss);
    for (std::uint32_t k = 0; k < sectors; ++k) fat.push_back(k + 1 < sectors ? first + k + 1 : end_of_chain);
    return first;
  };
  for (std::size_t i = 1; i < count; ++i) {
    if (!nodes_[i].storage && nodes_[i].data.size() >= cutoff) start[i] = allocate(nodes_[i].data.size());
  }
  const std::uint32_t mini_stream_start = allocate(mini_stream.size());
  const std::uint32_t mini_fat_start = allocate(mini_fat.size() * 4);
  const std::uint32_t mini_fat_sectors = static_cast<std::uint32_t>((mini_fat.size() * 4 + ss - 1) / ss);
  const std::size_t directory_entries = (count + 3) / 4 * 4;
  const std::uint32_t directory_start = allocate(directory_entries * entry_size);
  // FAT sectors must cover every sector, themselves and the DIFAT included.
  std::uint32_t fat_sectors = 0;
  std::uint32_t difat_sectors = 0;
  while (true) {
    const std::uint64_t total = fat.size() + fat_sectors + difat_sectors;
    const auto needed = static_cast<std::uint32_t>((total + ss / 4 - 1) / (ss / 4));
    const std::uint32_t needed_difat = needed > 109 ? (needed - 109 + 126) / 127 : 0;
    if (needed == fat_sectors && needed_difat == difat_sectors) break;
    fat_sectors = needed;
    difat_sectors = needed_difat;
  }
  const auto fat_first = static_cast<std::uint32_t>(fat.size());
  for (std::uint32_t k = 0; k < fat_sectors; ++k) fat.push_back(fat_sector);
  const auto difat_first = static_cast<std::uint32_t>(fat.size());
  for (std::uint32_t k = 0; k < difat_sectors; ++k) fat.push_back(difat_sector);
  const auto total_sectors = static_cast<std::uint32_t>(fat.size());
  fat.resize(std::size_t{fat_sectors} * (ss / 4), free_sector);

  util::ByteWriter out(header_size + std::size_t{total_sectors} * ss);
  // Header.
  out.bytes(std::span<const std::uint8_t>(signature));
  out.zeros(16);           // CLSID
  out.u16le(0x003E);       // minor version
  out.u16le(0x0003);       // major version 3
  out.u16le(0xFFFE);       // byte order
  out.u16le(9);            // sector shift
  out.u16le(6);            // mini sector shift
  out.zeros(6);
  out.u32le(0);            // directory sectors (0 in version 3)
  out.u32le(fat_sectors);
  out.u32le(directory_start);
  out.u32le(0);            // transaction signature
  out.u32le(cutoff);
  out.u32le(mini_fat.empty() ? end_of_chain : mini_fat_start);
  out.u32le(mini_fat_sectors);
  out.u32le(difat_sectors > 0 ? difat_first : end_of_chain);
  out.u32le(difat_sectors);
  for (std::uint32_t k = 0; k < 109; ++k) out.u32le(k < fat_sectors ? fat_first + k : free_sector);

  const auto pad = [&] { out.align(ss); };
  // Large streams.
  for (std::size_t i = 1; i < count; ++i) {
    if (!nodes_[i].storage && nodes_[i].data.size() >= cutoff) {
      out.bytes(nodes_[i].data);
      pad();
    }
  }
  out.bytes(mini_stream);
  pad();
  for (const std::uint32_t value : mini_fat) out.u32le(value);
  if (!mini_fat.empty()) {
    for (std::size_t k = mini_fat.size(); k % (ss / 4) != 0; ++k) out.u32le(free_sector);
  }
  // Directory.
  for (std::size_t i = 0; i < directory_entries; ++i) {
    if (i >= count) {
      out.zeros(64);
      out.u16le(0);
      out.u8(0);
      out.u8(0);
      out.u32le(no_stream);
      out.u32le(no_stream);
      out.u32le(no_stream);
      out.zeros(16 + 4 + 8 + 8);
      out.u32le(0);
      out.u64le(0);
      continue;
    }
    const Node& node = nodes_[i];
    for (std::size_t k = 0; k < 32; ++k) out.u16le(k < node.name.size() ? node.name[k] : 0);
    out.u16le(static_cast<std::uint16_t>((node.name.size() + 1) * 2));
    out.u8(i == 0 ? 5 : node.storage ? 1 : 2);
    out.u8(1);  // black
    out.u32le(left[i]);
    out.u32le(right[i]);
    out.u32le(child[i]);
    const auto& clsid = i == 0 ? root_clsid_ : node.clsid;
    out.bytes(std::span<const std::uint8_t>(clsid));
    out.u32le(0);  // state bits
    out.zeros(16);  // no timestamps
    if (i == 0) {
      out.u32le(mini_stream.empty() ? end_of_chain : mini_stream_start);
      out.u64le(mini_stream.size());
    } else if (node.storage) {
      out.u32le(0);
      out.u64le(0);
    } else {
      out.u32le(start[i]);
      out.u64le(node.data.size());
    }
  }
  // FAT and DIFAT.
  for (const std::uint32_t value : fat) out.u32le(value);
  for (std::uint32_t d = 0; d < difat_sectors; ++d) {
    for (std::uint32_t k = 0; k < 127; ++k) {
      const std::uint32_t index = 109 + d * 127 + k;
      out.u32le(index < fat_sectors ? fat_first + index : free_sector);
    }
    out.u32le(d + 1 < difat_sectors ? difat_first + d + 1 : end_of_chain);
  }
  return out.take();
}

}  // namespace cvt::table
