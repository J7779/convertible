// Convertible core - image_modern family: bounded TIFF structure parser for RAW files.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/image_modern/raw_tiff.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>

namespace cvt::image_modern::raw {

namespace {

int type_size(std::uint16_t type) noexcept {
  switch (type) {
    case kByte: case kAscii: case kSByte: case kUndefined: return 1;
    case kShort: case kSShort: return 2;
    case kLong: case kSLong: case kFloat: case kIfd: return 4;
    case kRational: case kSRational: case kDouble: return 8;
    default: return 0;
  }
}

// Directories with more entries than this are not plausible in RAW files.
constexpr std::uint32_t max_entries = 2000;

}  // namespace

const Field* Ifd::find(std::uint16_t tag) const noexcept {
  const auto it = std::lower_bound(fields.begin(), fields.end(), tag,
                                   [](const Field& f, std::uint16_t t) { return f.tag < t; });
  return it != fields.end() && it->tag == tag ? &*it : nullptr;
}

std::uint64_t TiffData::uint(std::uint64_t at, int bytes) const noexcept {
  if (bytes <= 0 || bytes > 8 || !has(at, static_cast<std::uint64_t>(bytes))) return 0;
  std::uint64_t v = 0;
  for (int i = 0; i < bytes; ++i) {
    const auto b = std::to_integer<std::uint64_t>(data_[static_cast<std::size_t>(at) + static_cast<std::size_t>(i)]);
    v = little_ ? v | (b << (8 * i)) : (v << 8) | b;
  }
  return v;
}

std::optional<Ifd> TiffData::parse_ifd(std::uint64_t at, int depth) const {
  if (at == 0 || !has(at, 2)) return std::nullopt;
  const std::uint32_t count = u16(at);
  if (count == 0 || count > max_entries || !has(at + 2, std::uint64_t{count} * 12)) return std::nullopt;
  Ifd ifd;
  ifd.offset = at;
  ifd.depth = depth;
  ifd.fields.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint64_t e = at + 2 + std::uint64_t{i} * 12;
    Field f;
    f.tag = u16(e);
    f.type = u16(e + 2);
    f.count = u32(e + 4);
    const int size = type_size(f.type);
    if (size == 0) continue;  // unknown type: skip the entry
    const std::uint64_t total = std::uint64_t{f.count} * static_cast<std::uint64_t>(size);
    f.value_at = total <= 4 ? e + 8 : base_ + u32(e + 8);
    ifd.fields.push_back(f);
  }
  std::stable_sort(ifd.fields.begin(), ifd.fields.end(), [](const Field& a, const Field& b) { return a.tag < b.tag; });
  // Keep the first of duplicate tags.
  ifd.fields.erase(std::unique(ifd.fields.begin(), ifd.fields.end(),
                               [](const Field& a, const Field& b) { return a.tag == b.tag; }),
                   ifd.fields.end());
  if (ifd.fields.empty()) return std::nullopt;
  return ifd;
}

std::uint64_t TiffData::next_ifd(const Ifd& ifd) const noexcept {
  const std::uint64_t link = ifd.offset + 2 + std::uint64_t{u16(ifd.offset)} * 12;
  const std::uint32_t next = u32(link);
  return next == 0 ? 0 : base_ + next;
}

std::vector<double> TiffData::numbers(const Field& field, std::size_t limit) const {
  std::vector<double> out;
  const int size = type_size(field.type);
  const std::uint64_t n = std::min<std::uint64_t>(field.count, limit);
  if (size == 0 || !has(field.value_at, n * static_cast<std::uint64_t>(size))) return out;
  out.reserve(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::uint64_t at = field.value_at + i * static_cast<std::uint64_t>(size);
    switch (field.type) {
      case kByte: case kUndefined: case kAscii: out.push_back(static_cast<double>(uint(at, 1))); break;
      case kSByte: out.push_back(static_cast<double>(static_cast<std::int8_t>(uint(at, 1)))); break;
      case kShort: out.push_back(static_cast<double>(uint(at, 2))); break;
      case kSShort: out.push_back(static_cast<double>(static_cast<std::int16_t>(uint(at, 2)))); break;
      case kLong: case kIfd: out.push_back(static_cast<double>(uint(at, 4))); break;
      case kSLong: out.push_back(static_cast<double>(static_cast<std::int32_t>(uint(at, 4)))); break;
      case kRational: {
        const auto num = static_cast<double>(uint(at, 4));
        const auto den = static_cast<double>(uint(at + 4, 4));
        out.push_back(den == 0.0 ? 0.0 : num / den);
        break;
      }
      case kSRational: {
        const auto num = static_cast<double>(static_cast<std::int32_t>(uint(at, 4)));
        const auto den = static_cast<double>(static_cast<std::int32_t>(uint(at + 4, 4)));
        out.push_back(den == 0.0 ? 0.0 : num / den);
        break;
      }
      case kFloat: {
        const float v = std::bit_cast<float>(static_cast<std::uint32_t>(uint(at, 4)));
        out.push_back(std::isfinite(v) ? static_cast<double>(v) : 0.0);
        break;
      }
      case kDouble: {
        const double v = std::bit_cast<double>(uint(at, 8));
        out.push_back(std::isfinite(v) ? v : 0.0);
        break;
      }
      default: break;
    }
  }
  return out;
}

std::vector<double> TiffData::numbers(const Ifd& ifd, std::uint16_t tag, std::size_t limit) const {
  const Field* f = ifd.find(tag);
  return f ? numbers(*f, limit) : std::vector<double>{};
}

std::vector<std::uint64_t> TiffData::integers(const Field& field, std::size_t limit) const {
  std::vector<std::uint64_t> out;
  const int size = type_size(field.type);
  const std::uint64_t n = std::min<std::uint64_t>(field.count, limit);
  if (size == 0 || !has(field.value_at, n * static_cast<std::uint64_t>(size))) return out;
  out.reserve(static_cast<std::size_t>(n));
  const int width = (field.type == kRational || field.type == kSRational) ? 4 : (field.type == kDouble ? 0 : size);
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::uint64_t at = field.value_at + i * static_cast<std::uint64_t>(size);
    if (field.type == kFloat || field.type == kDouble) {
      const double v = field.type == kFloat ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(uint(at, 4))))
                                            : std::bit_cast<double>(uint(at, 8));
      out.push_back(std::isfinite(v) && v > 0 ? static_cast<std::uint64_t>(std::min(v, 1.8e19)) : 0);
    } else {
      out.push_back(uint(at, width));
    }
  }
  return out;
}

std::vector<std::uint64_t> TiffData::integers(const Ifd& ifd, std::uint16_t tag, std::size_t limit) const {
  const Field* f = ifd.find(tag);
  return f ? integers(*f, limit) : std::vector<std::uint64_t>{};
}

std::uint64_t TiffData::integer(const Ifd& ifd, std::uint16_t tag, std::uint64_t fallback) const {
  const Field* f = ifd.find(tag);
  if (f == nullptr || f->count == 0) return fallback;
  const auto v = integers(*f, 1);
  return v.empty() ? fallback : v.front();
}

double TiffData::number(const Ifd& ifd, std::uint16_t tag, double fallback) const {
  const Field* f = ifd.find(tag);
  if (f == nullptr || f->count == 0) return fallback;
  const auto v = numbers(*f, 1);
  return v.empty() ? fallback : v.front();
}

ByteView TiffData::bytes(const Field& field) const noexcept {
  const int size = type_size(field.type);
  const std::uint64_t total = std::uint64_t{field.count} * static_cast<std::uint64_t>(size);
  if (size == 0 || !has(field.value_at, total)) return {};
  return data_.subspan(static_cast<std::size_t>(field.value_at), static_cast<std::size_t>(total));
}

ByteView TiffData::bytes(const Ifd& ifd, std::uint16_t tag) const noexcept {
  const Field* f = ifd.find(tag);
  return f ? bytes(*f) : ByteView{};
}

std::string TiffData::text(const Ifd& ifd, std::uint16_t tag, std::size_t limit) const {
  ByteView b = bytes(ifd, tag);
  if (b.size() > limit) b = b.first(limit);
  std::string s(as_chars(b));
  if (const std::size_t nul = s.find('\0'); nul != std::string::npos) s.resize(nul);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
  for (char& c : s) {
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = '?';
  }
  return s;
}

ByteView TiffData::range(std::uint64_t relative_offset, std::uint64_t length) const noexcept {
  const std::uint64_t at = base_ + relative_offset;
  if (at < relative_offset || !has(at, length)) return {};
  return data_.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(length));
}

std::optional<TiffHeader> parse_header(ByteView data, std::uint64_t base) {
  if (base > data.size() || data.size() - base < 8) return std::nullopt;
  const std::string_view h = as_chars(data.subspan(static_cast<std::size_t>(base), 8));
  TiffHeader header;
  if (h.substr(0, 2) == "II") {
    header.little = true;
  } else if (h.substr(0, 2) == "MM") {
    header.little = false;
  } else {
    return std::nullopt;
  }
  const TiffData t(data, base, header.little);
  header.magic = t.u16(base + 2);
  // 42 standard, 0x4F52 / 0x5352 Olympus ("RO" / "RS"), 0x55 Panasonic.
  if (header.magic != 42 && header.magic != 0x4F52 && header.magic != 0x5352 && header.magic != 0x55) {
    return std::nullopt;
  }
  const std::uint32_t first = t.u32(base + 4);
  if (first < 8) return std::nullopt;
  header.first_ifd = base + first;
  return header;
}

std::optional<TiffTree> parse_tree(ByteView data, std::uint64_t base, std::size_t max_ifds) {
  const auto header = parse_header(data, base);
  if (!header) return std::nullopt;
  TiffTree tree;
  tree.tiff = TiffData(data, base, header->little);
  tree.magic = header->magic;
  std::set<std::uint64_t> visited;

  // Depth-first: each main IFD followed by its SubIFDs.
  const auto add_subifds = [&](auto&& self, const Ifd& ifd, int depth) -> void {
    if (depth > 4) return;
    const Field* sub = ifd.find(330);
    if (sub == nullptr) return;
    for (const std::uint64_t rel : tree.tiff.integers(*sub, 32)) {
      if (tree.ifds.size() >= max_ifds) return;
      const std::uint64_t at = base + rel;
      if (!visited.insert(at).second) continue;
      auto child = tree.tiff.parse_ifd(at, depth);
      if (!child) continue;
      tree.ifds.push_back(*child);
      const Ifd copy = tree.ifds.back();
      self(self, copy, depth + 1);
    }
  };

  std::uint64_t at = header->first_ifd;
  while (at != 0 && tree.ifds.size() < max_ifds && visited.insert(at).second) {
    auto ifd = tree.tiff.parse_ifd(at, 0);
    if (!ifd) break;
    tree.ifds.push_back(*ifd);
    const Ifd copy = tree.ifds.back();
    if (tree.ifds.size() == 1) {
      if (const Field* exif = copy.find(34665)) {
        const auto rel = tree.tiff.integers(*exif, 1);
        if (!rel.empty() && visited.insert(base + rel[0]).second) tree.exif = tree.tiff.parse_ifd(base + rel[0], 1);
      }
    }
    add_subifds(add_subifds, copy, 1);
    at = tree.tiff.next_ifd(copy);
  }
  if (tree.ifds.empty()) return std::nullopt;
  return tree;
}

}  // namespace cvt::image_modern::raw
