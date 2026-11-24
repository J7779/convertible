// Convertible core - font family: parsers and builders of the common sfnt tables.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/tables.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "families/font/cff.hpp"
#include "families/font/sfnt.hpp"
#include "util/text.hpp"

namespace cvt::font {

namespace {

constexpr std::array<char16_t, 128> mac_roman_high = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7,
    0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5,
    0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9,
    0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248,
    0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
    0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7,
    0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD,
    0x02DB, 0x02C7};

std::string decode_utf16be(ByteView raw) {
  std::u16string units;
  units.reserve(raw.size() / 2);
  for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
    units.push_back(static_cast<char16_t>((std::to_integer<unsigned>(raw[i]) << 8) | std::to_integer<unsigned>(raw[i + 1])));
  }
  return util::utf16_to_utf8(units);
}

std::string decode_mac_roman(ByteView raw) {
  std::u32string text;
  for (const std::byte b : raw) {
    const auto c = std::to_integer<unsigned>(b);
    text.push_back(c < 0x80 ? static_cast<char32_t>(c) : static_cast<char32_t>(mac_roman_high[c - 0x80]));
  }
  return util::utf32_to_utf8(text);
}

Bytes encode_utf16be(std::string_view text) {
  const std::u16string units = util::utf8_to_utf16(text);
  Bytes out;
  out.reserve(units.size() * 2);
  for (const char16_t unit : units) {
    out.push_back(static_cast<std::byte>(unit >> 8));
    out.push_back(static_cast<std::byte>(unit & 0xFF));
  }
  return out;
}

}  // namespace

// --------------------------------------------------------------------------- name

bool is_legal_name_id(std::uint16_t id) noexcept { return id == 0 || id == 7 || id == 13 || id == 14; }

Result<std::vector<NameRecord>> parse_name(ByteView table) {
  util::ByteReader reader(table);
  const std::uint16_t format = reader.u16be();
  const std::uint16_t count = reader.u16be();
  const std::uint16_t storage = reader.u16be();
  if (!reader.ok() || format > 1) return fail(Status::Decode, "the name table is malformed");
  if (std::uint64_t{count} * 12 > reader.remaining()) return fail(Status::Decode, "the name table is truncated");
  std::vector<NameRecord> records;
  records.reserve(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    NameRecord record;
    record.platform = reader.u16be();
    record.encoding = reader.u16be();
    record.language = reader.u16be();
    record.name_id = reader.u16be();
    const std::uint16_t length = reader.u16be();
    const std::uint16_t offset = reader.u16be();
    const std::uint64_t start = std::uint64_t{storage} + offset;
    if (start + length > table.size()) continue;  // skip broken records rather than failing the font
    const ByteView raw = table.subspan(static_cast<std::size_t>(start), length);
    record.raw.assign(raw.begin(), raw.end());
    if (record.platform == 0 || record.platform == 3) {
      record.text = decode_utf16be(raw);
    } else if (record.platform == 1 && record.encoding == 0) {
      record.text = decode_mac_roman(raw);
    }
    records.push_back(std::move(record));
  }
  return records;
}

Bytes build_name(std::vector<NameRecord> records) {
  std::ranges::sort(records, [](const NameRecord& a, const NameRecord& b) {
    return std::tie(a.platform, a.encoding, a.language, a.name_id) <
           std::tie(b.platform, b.encoding, b.language, b.name_id);
  });
  util::ByteWriter strings;
  std::map<Bytes, std::size_t> seen;
  std::vector<std::pair<std::uint16_t, std::uint16_t>> placed;
  for (const NameRecord& record : records) {
    const auto [it, inserted] = seen.try_emplace(record.raw, strings.size());
    if (inserted) strings.bytes(record.raw);
    placed.emplace_back(static_cast<std::uint16_t>(std::min<std::size_t>(record.raw.size(), 0xFFFF)),
                        static_cast<std::uint16_t>(std::min<std::size_t>(it->second, 0xFFFF)));
  }
  util::ByteWriter out;
  out.u16be(0).u16be(static_cast<std::uint16_t>(records.size()));
  out.u16be(static_cast<std::uint16_t>(6 + 12 * records.size()));
  for (std::size_t i = 0; i < records.size(); ++i) {
    const NameRecord& r = records[i];
    out.u16be(r.platform).u16be(r.encoding).u16be(r.language).u16be(r.name_id);
    out.u16be(placed[i].first).u16be(placed[i].second);
  }
  out.bytes(strings.view());
  return out.take();
}

std::string best_name(std::span<const NameRecord> records, std::uint16_t name_id) {
  const NameRecord* best = nullptr;
  int best_score = -1;
  for (const NameRecord& record : records) {
    if (record.name_id != name_id || record.text.empty()) continue;
    int score = 0;
    if (record.platform == 3 && record.language == 0x0409) score = 5;
    else if (record.platform == 3) score = 3;
    else if (record.platform == 0) score = 4;
    else if (record.platform == 1 && record.language == 0) score = 2;
    else score = 1;
    if (score > best_score) {
      best = &record;
      best_score = score;
    }
  }
  return best ? best->text : std::string{};
}

NameRecord make_name(std::uint16_t name_id, std::string_view text) {
  NameRecord record;
  record.name_id = name_id;
  record.text = std::string(text);
  record.raw = encode_utf16be(text);
  return record;
}

// --------------------------------------------------------------------------- cmap

namespace {

constexpr std::uint32_t max_unicode = 0x10FFFF;

void add_mapping(CharMap& map, std::uint32_t code, std::uint32_t glyph, std::uint32_t num_glyphs) {
  if (glyph == 0 || glyph >= num_glyphs || code > max_unicode) return;
  map.try_emplace(code, static_cast<std::uint16_t>(glyph));
}

Result<CharMap> parse_subtable(ByteView table, std::uint32_t offset, std::uint32_t num_glyphs, bool mac_roman) {
  CharMap map;
  util::ByteReader reader(table);
  if (!reader.seek(offset)) return fail(Status::Decode, "cmap subtable offset is out of range");
  const std::uint16_t format = reader.u16be();
  switch (format) {
    case 0: {
      reader.skip(4);
      const ByteView glyphs = reader.bytes(256);
      if (!reader.ok()) return fail(Status::Decode, "cmap format 0 is truncated");
      for (std::uint32_t c = 0; c < 256; ++c) {
        const std::uint32_t code = mac_roman && c >= 0x80 ? mac_roman_high[c - 0x80] : c;
        add_mapping(map, code, std::to_integer<std::uint32_t>(glyphs[c]), num_glyphs);
      }
      return map;
    }
    case 4: {
      const std::uint16_t length = reader.u16be();
      reader.skip(2);
      const std::uint16_t seg_x2 = reader.u16be();
      if (!reader.ok() || seg_x2 % 2 != 0) return fail(Status::Decode, "cmap format 4 header is malformed");
      const std::size_t base = offset;
      const std::size_t end_limit = std::min<std::size_t>(table.size(), base + std::max<std::size_t>(length, 16));
      const ByteView sub = table.subspan(base, end_limit - base);
      const std::size_t seg = seg_x2 / 2u;
      const std::size_t ends = 14, starts = 16 + seg_x2, deltas = 16 + 2u * seg_x2, ranges = 16 + 3u * seg_x2;
      if (ranges + seg_x2 > table.size() - base) return fail(Status::Decode, "cmap format 4 is truncated");
      const ByteView full = table.subspan(base);
      for (std::size_t i = 0; i < seg; ++i) {
        const std::uint32_t end_code = get_u16(full, ends + 2 * i);
        const std::uint32_t start_code = get_u16(full, starts + 2 * i);
        const std::uint16_t delta = get_u16(full, deltas + 2 * i);
        const std::uint16_t range = get_u16(full, ranges + 2 * i);
        if (start_code > end_code) continue;
        for (std::uint32_t c = start_code; c <= end_code; ++c) {
          if (c == 0xFFFF) break;
          std::uint32_t glyph = 0;
          if (range == 0) {
            glyph = (c + delta) & 0xFFFF;
          } else {
            const std::uint64_t at = ranges + 2 * i + range + 2ull * (c - start_code);
            if (at + 2 > full.size()) break;
            glyph = get_u16(full, static_cast<std::size_t>(at));
            if (glyph != 0) glyph = (glyph + delta) & 0xFFFF;
          }
          const std::uint32_t code = mac_roman && c >= 0x80 && c < 0x100 ? mac_roman_high[c - 0x80] : c;
          add_mapping(map, code, glyph, num_glyphs);
        }
      }
      (void)sub;
      return map;
    }
    case 6: {
      reader.skip(4);
      const std::uint16_t first = reader.u16be();
      const std::uint16_t count = reader.u16be();
      for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
        const std::uint16_t glyph = reader.u16be();
        if (!reader.ok()) break;
        std::uint32_t code = first + i;
        if (mac_roman && code >= 0x80 && code < 0x100) code = mac_roman_high[code - 0x80];
        add_mapping(map, code, glyph, num_glyphs);
      }
      return map;
    }
    case 10: {
      reader.skip(10);
      const std::uint32_t first = reader.u32be();
      const std::uint32_t count = reader.u32be();
      if (!reader.ok() || std::uint64_t{count} * 2 > reader.remaining()) {
        return fail(Status::Decode, "cmap format 10 is truncated");
      }
      for (std::uint32_t i = 0; i < count; ++i) add_mapping(map, first + i, reader.u16be(), num_glyphs);
      return map;
    }
    case 12:
    case 13: {
      reader.skip(10);
      const std::uint32_t groups = reader.u32be();
      if (!reader.ok() || std::uint64_t{groups} * 12 > reader.remaining()) {
        return fail(Status::Decode, "cmap format {} is truncated", format);
      }
      std::uint64_t total = 0;
      for (std::uint32_t g = 0; g < groups; ++g) {
        const std::uint32_t start = reader.u32be();
        const std::uint32_t end = reader.u32be();
        const std::uint32_t glyph = reader.u32be();
        if (start > end || end > max_unicode) continue;
        total += std::uint64_t{end} - start + 1;
        if (total > max_unicode + 1ull) return fail(Status::Decode, "cmap groups overlap excessively");
        for (std::uint32_t c = start; c <= end; ++c) {
          add_mapping(map, c, format == 12 ? glyph + (c - start) : glyph, num_glyphs);
        }
      }
      return map;
    }
    default:
      return fail(Status::Decode, "cmap format {} is not supported", format);
  }
}

}  // namespace

Result<CharMap> parse_cmap(ByteView table, std::uint32_t num_glyphs) {
  util::ByteReader reader(table);
  reader.skip(2);
  const std::uint16_t count = reader.u16be();
  if (!reader.ok() || std::uint64_t{count} * 8 > reader.remaining()) {
    return fail(Status::Decode, "the cmap table is malformed");
  }
  struct Candidate {
    int score;
    std::uint16_t platform, encoding;
    std::uint32_t offset;
  };
  std::vector<Candidate> candidates;
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::uint16_t platform = reader.u16be();
    const std::uint16_t encoding = reader.u16be();
    const std::uint32_t offset = reader.u32be();
    if (offset >= table.size()) continue;
    const std::uint16_t format = get_u16(table, offset);
    if (format == 14) continue;
    int score = 0;
    if (platform == 3 && encoding == 10) score = 10;
    else if (platform == 0 && (encoding == 4 || encoding == 6)) score = 9;
    else if (platform == 3 && encoding == 1) score = 8;
    else if (platform == 0) score = 7;
    else if (platform == 3 && encoding == 0) score = 5;
    else if (platform == 1 && encoding == 0) score = 3;
    if (score > 0) candidates.push_back({score, platform, encoding, offset});
  }
  std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
  for (const Candidate& candidate : candidates) {
    auto parsed = parse_subtable(table, candidate.offset, num_glyphs, candidate.platform == 1);
    if (!parsed) continue;
    CharMap map = std::move(*parsed);
    if (candidate.platform == 3 && candidate.encoding == 0) {
      // Symbol fonts: make U+F020..U+F0FF reachable from Latin-1 text too.
      CharMap extra;
      for (const auto& [code, glyph] : map) {
        if (code >= 0xF020 && code <= 0xF0FF) extra.try_emplace(code - 0xF000, glyph);
      }
      map.merge(extra);
    }
    if (!map.empty() || &candidate == &candidates.back()) return map;
  }
  return CharMap{};
}

std::vector<VariationSequence> parse_cmap14(ByteView table) {
  std::vector<VariationSequence> out;
  const std::uint16_t count = get_u16(table, 2);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t offset = get_u32(table, 8 + 8 * i);
    if (get_u16(table, 4 + 8 * i) != 0 || get_u16(table, offset) != 14) continue;
    const ByteView sub = offset < table.size() ? table.subspan(offset) : ByteView{};
    const std::uint32_t records = get_u32(sub, 6);
    for (std::uint32_t r = 0; r < records && r < 1024; ++r) {
      const std::size_t at = 10 + 11 * r;
      const std::uint32_t selector = (std::uint32_t{get_u16(sub, at)} << 8) | std::to_integer<std::uint32_t>(at + 2 < sub.size() ? sub[at + 2] : std::byte{0});
      const std::uint32_t non_default = get_u32(sub, at + 7);
      if (non_default != 0 && non_default < sub.size()) {
        const std::uint32_t mappings = std::min<std::uint32_t>(get_u32(sub, non_default), 65536);
        for (std::uint32_t m = 0; m < mappings; ++m) {
          const std::size_t p = non_default + 4 + 5 * m;
          if (p + 5 > sub.size()) break;
          const std::uint32_t base = (std::uint32_t{get_u16(sub, p)} << 8) | std::to_integer<std::uint32_t>(sub[p + 2]);
          out.push_back({selector, base, get_u16(sub, p + 3)});
        }
      }
    }
  }
  return out;
}

namespace {

Bytes build_format4(const CharMap& map) {
  struct Segment {
    std::uint16_t start, end;
    std::int32_t delta;  // used when glyphs empty
    std::vector<std::uint16_t> glyphs;
  };
  std::vector<std::pair<std::uint32_t, std::uint16_t>> bmp;
  for (const auto& [code, glyph] : map) {
    if (code < 0xFFFF) bmp.emplace_back(code, glyph);
  }
  std::vector<Segment> segments;
  std::size_t i = 0;
  while (i < bmp.size()) {
    std::size_t run_end = i + 1;  // contiguous codes
    while (run_end < bmp.size() && bmp[run_end].first == bmp[run_end - 1].first + 1) ++run_end;
    // Split the contiguous run into sub-runs of constant delta.
    std::size_t j = i;
    std::vector<std::pair<std::size_t, std::size_t>> subs;
    while (j < run_end) {
      std::size_t k = j + 1;
      const std::int64_t delta = std::int64_t{bmp[j].second} - bmp[j].first;
      while (k < run_end && std::int64_t{bmp[k].second} - bmp[k].first == delta) ++k;
      subs.emplace_back(j, k);
      j = k;
    }
    std::size_t s = 0;
    while (s < subs.size()) {
      if (subs[s].second - subs[s].first >= 4 || subs.size() == 1) {
        const auto [a, b] = subs[s];
        segments.push_back({static_cast<std::uint16_t>(bmp[a].first), static_cast<std::uint16_t>(bmp[b - 1].first),
                            static_cast<std::int32_t>(std::int64_t{bmp[a].second} - bmp[a].first), {}});
        ++s;
      } else {
        const std::size_t a = subs[s].first;
        std::size_t b = subs[s].second;
        ++s;
        while (s < subs.size() && subs[s].second - subs[s].first < 4) b = subs[s++].second;
        Segment segment{static_cast<std::uint16_t>(bmp[a].first), static_cast<std::uint16_t>(bmp[b - 1].first), 0, {}};
        for (std::size_t g = a; g < b; ++g) segment.glyphs.push_back(bmp[g].second);
        segments.push_back(std::move(segment));
      }
    }
    i = run_end;
  }
  segments.push_back({0xFFFF, 0xFFFF, 1, {}});
  const std::size_t seg = segments.size();
  std::uint16_t entry_selector = 0;
  while ((2u << entry_selector) <= seg) ++entry_selector;
  const auto search_range = static_cast<std::uint16_t>(2u << entry_selector);
  std::size_t array_words = 0;
  for (const Segment& s : segments) array_words += s.glyphs.size();
  const std::size_t length = 16 + 8 * seg + 2 * array_words;
  if (length > 0xFFFF) return {};
  util::ByteWriter out(length);
  out.u16be(4).u16be(static_cast<std::uint16_t>(length)).u16be(0);
  out.u16be(static_cast<std::uint16_t>(seg * 2)).u16be(search_range).u16be(entry_selector);
  out.u16be(static_cast<std::uint16_t>(seg * 2 - search_range));
  for (const Segment& s : segments) out.u16be(s.end);
  out.u16be(0);
  for (const Segment& s : segments) out.u16be(s.start);
  for (const Segment& s : segments) out.u16be(static_cast<std::uint16_t>(s.delta & 0xFFFF));
  std::size_t array_offset = 0;
  for (std::size_t k = 0; k < seg; ++k) {
    if (segments[k].glyphs.empty()) {
      out.u16be(0);
    } else {
      const std::size_t remaining_ranges = seg - k;
      out.u16be(static_cast<std::uint16_t>(2 * (remaining_ranges + array_offset)));
      array_offset += segments[k].glyphs.size();
    }
  }
  for (const Segment& s : segments) {
    for (const std::uint16_t g : s.glyphs) out.u16be(g);
  }
  return out.take();
}

Bytes build_format12(const CharMap& map) {
  std::vector<std::array<std::uint32_t, 3>> groups;
  for (const auto& [code, glyph] : map) {
    if (!groups.empty() && groups.back()[1] + 1 == code &&
        groups.back()[2] + (groups.back()[1] - groups.back()[0]) + 1 == glyph) {
      groups.back()[1] = code;
    } else {
      groups.push_back({code, code, glyph});
    }
  }
  util::ByteWriter out;
  out.u16be(12).u16be(0).u32be(static_cast<std::uint32_t>(16 + 12 * groups.size())).u32be(0);
  out.u32be(static_cast<std::uint32_t>(groups.size()));
  for (const auto& g : groups) out.u32be(g[0]).u32be(g[1]).u32be(g[2]);
  return out.take();
}

}  // namespace

Bytes build_cmap(const CharMap& map) {
  const bool wide = !map.empty() && map.rbegin()->first > 0xFFFF;
  Bytes f4 = build_format4(map);
  const Bytes f12 = (wide || f4.empty()) ? build_format12(map) : Bytes{};
  if (f4.empty()) {
    CharMap limited;
    for (const auto& [code, glyph] : map) {
      if (code < 0xFFFF && limited.size() < 8000) limited.emplace(code, glyph);
    }
    f4 = build_format4(limited);
  }
  const std::uint16_t count = f12.empty() ? 2 : 4;
  util::ByteWriter out;
  out.u16be(0).u16be(count);
  const std::uint32_t f4_at = 4 + 8u * count;
  const auto f12_at = static_cast<std::uint32_t>(f4_at + f4.size());
  out.u16be(0).u16be(3).u32be(f4_at);
  if (!f12.empty()) out.u16be(0).u16be(4).u32be(f12_at);
  out.u16be(3).u16be(1).u32be(f4_at);
  if (!f12.empty()) out.u16be(3).u16be(10).u32be(f12_at);
  out.bytes(f4).bytes(f12);
  return out.take();
}

// --------------------------------------------------------------------------- metrics

std::uint16_t num_glyphs(const ir::Font& font) noexcept {
  const ir::FontTable* maxp = font.find("maxp");
  return maxp ? get_u16(maxp->bytes, 4) : 0;
}

std::uint16_t units_per_em(const ir::Font& font) noexcept {
  const ir::FontTable* head = font.find("head");
  const std::uint16_t upm = head ? get_u16(head->bytes, 18) : 0;
  return upm >= 16 ? upm : 1000;
}

Result<std::vector<HorizontalMetric>> parse_hmtx(const ir::Font& font) {
  const std::uint16_t count = num_glyphs(font);
  const ir::FontTable* hhea = font.find("hhea");
  const ir::FontTable* hmtx = font.find("hmtx");
  std::vector<HorizontalMetric> metrics(count);
  if (!hhea || !hmtx) return metrics;
  std::uint16_t long_count = get_u16(hhea->bytes, 34);
  if (long_count == 0 && count > 0) return fail(Status::Decode, "hhea.numberOfHMetrics is zero");
  long_count = std::min(long_count, count);
  const ByteView data = hmtx->bytes;
  std::uint16_t last_advance = 0;
  for (std::uint16_t g = 0; g < count; ++g) {
    if (g < long_count) {
      last_advance = get_u16(data, 4u * g);
      metrics[g].advance = last_advance;
      metrics[g].lsb = get_i16(data, 4u * g + 2);
    } else {
      metrics[g].advance = last_advance;
      metrics[g].lsb = get_i16(data, 4u * long_count + 2u * (g - long_count));
    }
  }
  return metrics;
}

Bytes build_hmtx(std::span<const HorizontalMetric> metrics, std::uint16_t& number_of_hmetrics) {
  std::size_t long_count = metrics.size();
  while (long_count > 1 && metrics[long_count - 1].advance == metrics[long_count - 2].advance) --long_count;
  number_of_hmetrics = static_cast<std::uint16_t>(long_count);
  util::ByteWriter out(4 * metrics.size());
  for (std::size_t g = 0; g < metrics.size(); ++g) {
    if (g < long_count) out.u16be(metrics[g].advance);
    out.i16be(metrics[g].lsb);
  }
  return out.take();
}

// --------------------------------------------------------------------------- glyph names

std::span<const std::string_view> mac_glyph_names() noexcept {
  static constexpr std::array<std::string_view, 258> names = {
      ".notdef", ".null", "nonmarkingreturn", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent",
      "ampersand", "quotesingle", "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash",
      "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less",
      "equal", "greater", "question", "at", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
      "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright",
      "asciicircum", "underscore", "grave", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n",
      "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde",
      "Adieresis", "Aring", "Ccedilla", "Eacute", "Ntilde", "Odieresis", "Udieresis", "aacute", "agrave",
      "acircumflex", "adieresis", "atilde", "aring", "ccedilla", "eacute", "egrave", "ecircumflex", "edieresis",
      "iacute", "igrave", "icircumflex", "idieresis", "ntilde", "oacute", "ograve", "ocircumflex", "odieresis",
      "otilde", "uacute", "ugrave", "ucircumflex", "udieresis", "dagger", "degree", "cent", "sterling", "section",
      "bullet", "paragraph", "germandbls", "registered", "copyright", "trademark", "acute", "dieresis", "notequal",
      "AE", "Oslash", "infinity", "plusminus", "lessequal", "greaterequal", "yen", "mu", "partialdiff", "summation",
      "product", "pi", "integral", "ordfeminine", "ordmasculine", "Omega", "ae", "oslash", "questiondown",
      "exclamdown", "logicalnot", "radical", "florin", "approxequal", "Delta", "guillemotleft", "guillemotright",
      "ellipsis", "nonbreakingspace", "Agrave", "Atilde", "Otilde", "OE", "oe", "endash", "emdash", "quotedblleft",
      "quotedblright", "quoteleft", "quoteright", "divide", "lozenge", "ydieresis", "Ydieresis", "fraction",
      "currency", "guilsinglleft", "guilsinglright", "fi", "fl", "daggerdbl", "periodcentered", "quotesinglbase",
      "quotedblbase", "perthousand", "Acircumflex", "Ecircumflex", "Aacute", "Edieresis", "Egrave", "Iacute",
      "Icircumflex", "Idieresis", "Igrave", "Oacute", "Ocircumflex", "apple", "Ograve", "Uacute", "Ucircumflex",
      "Ugrave", "dotlessi", "circumflex", "tilde", "macron", "breve", "dotaccent", "ring", "cedilla", "hungarumlaut",
      "ogonek", "caron", "Lslash", "lslash", "Scaron", "scaron", "Zcaron", "zcaron", "brokenbar", "Eth", "eth",
      "Yacute", "yacute", "Thorn", "thorn", "minus", "multiply", "onesuperior", "twosuperior", "threesuperior",
      "onehalf", "onequarter", "threequarters", "franc", "Gbreve", "gbreve", "Idotaccent", "Scedilla", "scedilla",
      "Cacute", "cacute", "Ccaron", "ccaron", "dcroat"};
  return names;
}

std::span<const std::string_view> cff_standard_strings() noexcept {
  static constexpr std::array<std::string_view, 391> strings = {
      ".notdef", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quoteright",
      "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two",
      "three", "four", "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater",
      "question", "at", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
      "T", "U", "V", "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore",
      "quoteleft", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t",
      "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde", "exclamdown", "cent", "sterling",
      "fraction", "yen", "florin", "section", "currency", "quotesingle", "quotedblleft", "guillemotleft",
      "guilsinglleft", "guilsinglright", "fi", "fl", "endash", "dagger", "daggerdbl", "periodcentered", "paragraph",
      "bullet", "quotesinglbase", "quotedblbase", "quotedblright", "guillemotright", "ellipsis", "perthousand",
      "questiondown", "grave", "acute", "circumflex", "tilde", "macron", "breve", "dotaccent", "dieresis", "ring",
      "cedilla", "hungarumlaut", "ogonek", "caron", "emdash", "AE", "ordfeminine", "Lslash", "Oslash", "OE",
      "ordmasculine", "ae", "dotlessi", "lslash", "oslash", "oe", "germandbls", "onesuperior", "logicalnot", "mu",
      "trademark", "Eth", "onehalf", "plusminus", "Thorn", "onequarter", "divide", "brokenbar", "degree", "thorn",
      "threequarters", "twosuperior", "registered", "minus", "eth", "multiply", "threesuperior", "copyright",
      "Aacute", "Acircumflex", "Adieresis", "Agrave", "Aring", "Atilde", "Ccedilla", "Eacute", "Ecircumflex",
      "Edieresis", "Egrave", "Iacute", "Icircumflex", "Idieresis", "Igrave", "Ntilde", "Oacute", "Ocircumflex",
      "Odieresis", "Ograve", "Otilde", "Scaron", "Uacute", "Ucircumflex", "Udieresis", "Ugrave", "Yacute",
      "Ydieresis", "Zcaron", "aacute", "acircumflex", "adieresis", "agrave", "aring", "atilde", "ccedilla", "eacute",
      "ecircumflex", "edieresis", "egrave", "iacute", "icircumflex", "idieresis", "igrave", "ntilde", "oacute",
      "ocircumflex", "odieresis", "ograve", "otilde", "scaron", "uacute", "ucircumflex", "udieresis", "ugrave",
      "yacute", "ydieresis", "zcaron", "exclamsmall", "Hungarumlautsmall", "dollaroldstyle", "dollarsuperior",
      "ampersandsmall", "Acutesmall", "parenleftsuperior", "parenrightsuperior", "twodotenleader", "onedotenleader",
      "zerooldstyle", "oneoldstyle", "twooldstyle", "threeoldstyle", "fouroldstyle", "fiveoldstyle", "sixoldstyle",
      "sevenoldstyle", "eightoldstyle", "nineoldstyle", "commasuperior", "threequartersemdash", "periodsuperior",
      "questionsmall", "asuperior", "bsuperior", "centsuperior", "dsuperior", "esuperior", "isuperior", "lsuperior",
      "msuperior", "nsuperior", "osuperior", "rsuperior", "ssuperior", "tsuperior", "ff", "ffi", "ffl",
      "parenleftinferior", "parenrightinferior", "Circumflexsmall", "hyphensuperior", "Gravesmall", "Asmall",
      "Bsmall", "Csmall", "Dsmall", "Esmall", "Fsmall", "Gsmall", "Hsmall", "Ismall", "Jsmall", "Ksmall", "Lsmall",
      "Msmall", "Nsmall", "Osmall", "Psmall", "Qsmall", "Rsmall", "Ssmall", "Tsmall", "Usmall", "Vsmall", "Wsmall",
      "Xsmall", "Ysmall", "Zsmall", "colonmonetary", "onefitted", "rupiah", "Tildesmall", "exclamdownsmall",
      "centoldstyle", "Lslashsmall", "Scaronsmall", "Zcaronsmall", "Dieresissmall", "Brevesmall", "Caronsmall",
      "Dotaccentsmall", "Macronsmall", "figuredash", "hypheninferior", "Ogoneksmall", "Ringsmall", "Cedillasmall",
      "questiondownsmall", "oneeighth", "threeeighths", "fiveeighths", "seveneighths", "onethird", "twothirds",
      "zerosuperior", "foursuperior", "fivesuperior", "sixsuperior", "sevensuperior", "eightsuperior",
      "ninesuperior", "zeroinferior", "oneinferior", "twoinferior", "threeinferior", "fourinferior", "fiveinferior",
      "sixinferior", "seveninferior", "eightinferior", "nineinferior", "centinferior", "dollarinferior",
      "periodinferior", "commainferior", "Agravesmall", "Aacutesmall", "Acircumflexsmall", "Atildesmall",
      "Adieresissmall", "Aringsmall", "AEsmall", "Ccedillasmall", "Egravesmall", "Eacutesmall", "Ecircumflexsmall",
      "Edieresissmall", "Igravesmall", "Iacutesmall", "Icircumflexsmall", "Idieresissmall", "Ethsmall",
      "Ntildesmall", "Ogravesmall", "Oacutesmall", "Ocircumflexsmall", "Otildesmall", "Odieresissmall", "OEsmall",
      "Oslashsmall", "Ugravesmall", "Uacutesmall", "Ucircumflexsmall", "Udieresissmall", "Yacutesmall",
      "Thornsmall", "Ydieresissmall", "001.000", "001.001", "001.002", "001.003", "Black", "Bold", "Book", "Light",
      "Medium", "Regular", "Roman", "Semibold"};
  return strings;
}

std::string_view standard_encoding(std::uint8_t code) noexcept {
  const auto strings = cff_standard_strings();
  if (code >= 32 && code <= 126) return strings[code - 31];
  static constexpr std::array<std::pair<std::uint8_t, std::uint16_t>, 54> high = {{
      {161, 96},  {162, 97},  {163, 98},  {164, 99},  {165, 100}, {166, 101}, {167, 102}, {168, 103}, {169, 104},
      {170, 105}, {171, 106}, {172, 107}, {173, 108}, {174, 109}, {175, 110}, {177, 111}, {178, 112}, {179, 113},
      {180, 114}, {182, 115}, {183, 116}, {184, 117}, {185, 118}, {186, 119}, {187, 120}, {188, 121}, {189, 122},
      {191, 123}, {193, 124}, {194, 125}, {195, 126}, {196, 127}, {197, 128}, {198, 129}, {199, 130}, {200, 131},
      {202, 132}, {203, 133}, {205, 134}, {206, 135}, {207, 136}, {208, 137}, {225, 138}, {227, 139}, {232, 140},
      {233, 141}, {234, 142}, {235, 143}, {241, 144}, {245, 145}, {248, 146}, {249, 147}, {250, 148}, {251, 149}}};
  for (const auto& [c, sid] : high) {
    if (c == code) return strings[sid];
  }
  return {};
}

namespace {

struct NamedCode {
  std::string_view name;
  std::uint32_t code;
};

constexpr NamedCode extra_names[] = {
    {"dotlessi", 0x0131}, {"Lslash", 0x0141}, {"lslash", 0x0142}, {"OE", 0x0152}, {"oe", 0x0153},
    {"Scaron", 0x0160}, {"scaron", 0x0161}, {"Ydieresis", 0x0178}, {"Zcaron", 0x017D}, {"zcaron", 0x017E},
    {"florin", 0x0192}, {"circumflex", 0x02C6}, {"caron", 0x02C7}, {"breve", 0x02D8}, {"dotaccent", 0x02D9},
    {"ring", 0x02DA}, {"ogonek", 0x02DB}, {"tilde", 0x02DC}, {"hungarumlaut", 0x02DD}, {"endash", 0x2013},
    {"emdash", 0x2014}, {"quoteleft", 0x2018}, {"quoteright", 0x2019}, {"quotesinglbase", 0x201A},
    {"quotedblleft", 0x201C}, {"quotedblright", 0x201D}, {"quotedblbase", 0x201E}, {"dagger", 0x2020},
    {"daggerdbl", 0x2021}, {"bullet", 0x2022}, {"ellipsis", 0x2026}, {"perthousand", 0x2030},
    {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A}, {"fraction", 0x2044}, {"Euro", 0x20AC},
    {"trademark", 0x2122}, {"minus", 0x2212}, {"fi", 0xFB01}, {"fl", 0xFB02}, {"Gbreve", 0x011E},
    {"gbreve", 0x011F}, {"Idotaccent", 0x0130}, {"Scedilla", 0x015E}, {"scedilla", 0x015F}, {"Cacute", 0x0106},
    {"cacute", 0x0107}, {"Ccaron", 0x010C}, {"ccaron", 0x010D}, {"dcroat", 0x0111}, {"Omega", 0x03A9},
    {"Delta", 0x2206}, {"pi", 0x03C0}, {"partialdiff", 0x2202}, {"summation", 0x2211}, {"product", 0x220F},
    {"integral", 0x222B}, {"radical", 0x221A}, {"infinity", 0x221E}, {"approxequal", 0x2248},
    {"notequal", 0x2260}, {"lessequal", 0x2264}, {"greaterequal", 0x2265}, {"lozenge", 0x25CA},
    {"nonbreakingspace", 0x00A0}, {"franc", 0x20A3}, {"nbspace", 0x00A0}, {"uni00A0", 0x00A0},
    {"quotesingle", 0x0027}};

constexpr std::array<std::string_view, 95> ascii_names = {
    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quotesingle", "parenleft",
    "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two", "three", "four",
    "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at", "A",
    "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "grave", "a", "b", "c", "d",
    "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "braceleft", "bar", "braceright", "asciitilde"};

constexpr std::array<std::string_view, 95> latin1_names = {
    "exclamdown", "cent", "sterling", "currency", "yen", "brokenbar", "section", "dieresis", "copyright",
    "ordfeminine", "guillemotleft", "logicalnot", "uni00AD", "registered", "macron", "degree", "plusminus",
    "twosuperior", "threesuperior", "acute", "mu", "paragraph", "periodcentered", "cedilla", "onesuperior",
    "ordmasculine", "guillemotright", "onequarter", "onehalf", "threequarters", "questiondown", "Agrave", "Aacute",
    "Acircumflex", "Atilde", "Adieresis", "Aring", "AE", "Ccedilla", "Egrave", "Eacute", "Ecircumflex", "Edieresis",
    "Igrave", "Iacute", "Icircumflex", "Idieresis", "Eth", "Ntilde", "Ograve", "Oacute", "Ocircumflex", "Otilde",
    "Odieresis", "multiply", "Oslash", "Ugrave", "Uacute", "Ucircumflex", "Udieresis", "Yacute", "Thorn",
    "germandbls", "agrave", "aacute", "acircumflex", "atilde", "adieresis", "aring", "ae", "ccedilla", "egrave",
    "eacute", "ecircumflex", "edieresis", "igrave", "iacute", "icircumflex", "idieresis", "eth", "ntilde", "ograve",
    "oacute", "ocircumflex", "otilde", "odieresis", "divide", "oslash", "ugrave", "uacute", "ucircumflex",
    "udieresis", "yacute", "thorn", "ydieresis"};

std::optional<std::uint32_t> parse_hex(std::string_view text) {
  std::uint32_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  for (const char c : text) {
    if (c >= 'a' && c <= 'f') return std::nullopt;  // AGL requires upper-case hex
  }
  return value;
}

}  // namespace

std::optional<std::uint32_t> unicode_from_glyph_name(std::string_view name) {
  if (name.empty() || name.find('.') != std::string_view::npos || name.find('_') != std::string_view::npos) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < ascii_names.size(); ++i) {
    if (ascii_names[i] == name) return static_cast<std::uint32_t>(0x20 + i);
  }
  for (std::size_t i = 0; i < latin1_names.size(); ++i) {
    if (latin1_names[i] == name) return static_cast<std::uint32_t>(0xA1 + i);
  }
  for (const NamedCode& entry : extra_names) {
    if (entry.name == name) return entry.code;
  }
  if (name.size() == 7 && name.starts_with("uni")) {
    const auto value = parse_hex(name.substr(3));
    if (value && (*value < 0xD800 || *value > 0xDFFF)) return value;
  }
  if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u') {
    const auto value = parse_hex(name.substr(1));
    if (value && *value <= max_unicode && (*value < 0xD800 || *value > 0xDFFF)) return value;
  }
  return std::nullopt;
}

std::string glyph_name_for_unicode(std::uint32_t codepoint) {
  if (codepoint >= 0x20 && codepoint <= 0x7E) return std::string(ascii_names[codepoint - 0x20]);
  if (codepoint >= 0xA1 && codepoint <= 0xFF && codepoint != 0xAD) return std::string(latin1_names[codepoint - 0xA1]);
  for (const NamedCode& entry : extra_names) {
    if (entry.code == codepoint && entry.name != "nbspace" && entry.name != "uni00A0" &&
        entry.name != "quotesingle") {
      return std::string(entry.name);
    }
  }
  if (codepoint <= 0xFFFF) return std::format("uni{:04X}", codepoint);
  return std::format("u{:X}", codepoint);
}

std::vector<std::string> post_glyph_names(ByteView post, std::uint32_t count) {
  std::vector<std::string> names(count);
  const std::uint32_t version = get_u32(post, 0);
  const auto standard = mac_glyph_names();
  if (version == 0x00010000u) {
    for (std::uint32_t g = 0; g < count && g < standard.size(); ++g) names[g] = std::string(standard[g]);
  } else if (version == 0x00020000u) {
    util::ByteReader reader(post);
    reader.skip(32);
    const std::uint16_t declared = reader.u16be();
    if (!reader.ok() || std::uint64_t{declared} * 2 > reader.remaining()) return names;
    std::vector<std::uint16_t> indices(declared);
    for (std::uint16_t& index : indices) index = reader.u16be();
    std::vector<std::string> extra;
    while (!reader.at_end() && extra.size() < 65536) {
      const std::uint8_t length = reader.u8();
      const std::string_view text = reader.chars(length);
      if (!reader.ok()) break;
      extra.emplace_back(text);
    }
    for (std::uint32_t g = 0; g < count && g < declared; ++g) {
      const std::uint16_t index = indices[g];
      if (index < 258) names[g] = std::string(standard[index]);
      else if (index - 258u < extra.size()) names[g] = extra[index - 258u];
    }
  }
  return names;
}

Bytes build_post(ByteView source, std::span<const std::string> names) {
  util::ByteWriter out;
  const bool has_header = source.size() >= 32;
  out.u32be(names.empty() ? 0x00030000u : 0x00020000u);
  if (has_header) {
    out.bytes(source.subspan(4, 12));  // italicAngle, underlinePosition, underlineThickness, isFixedPitch
  } else {
    out.u32be(0).i16be(-100).i16be(50).u32be(0);
  }
  out.u32be(0).u32be(0).u32be(0).u32be(0);  // memory hints are meaningless today
  if (names.empty()) return out.take();
  const auto standard = mac_glyph_names();
  std::map<std::string_view, std::uint16_t> standard_index;
  for (std::size_t i = 0; i < standard.size(); ++i) standard_index.emplace(standard[i], static_cast<std::uint16_t>(i));
  out.u16be(static_cast<std::uint16_t>(names.size()));
  std::vector<std::string_view> extra;
  std::map<std::string_view, std::uint16_t> extra_index;
  for (const std::string& name : names) {
    if (const auto it = standard_index.find(name); it != standard_index.end()) {
      out.u16be(it->second);
      continue;
    }
    auto [it, inserted] = extra_index.try_emplace(name, static_cast<std::uint16_t>(258 + extra.size()));
    if (inserted) extra.push_back(name);
    out.u16be(it->second);
  }
  for (const std::string_view name : extra) {
    const std::string_view clipped = name.substr(0, 255);
    out.u8(static_cast<std::uint8_t>(clipped.size())).text(clipped);
  }
  return out.take();
}

namespace {

std::string sanitize_ps_name(std::string_view name) {
  std::string out;
  for (const char c : name) {
    if (c > 0x20 && c < 0x7F && std::string_view("[](){}<>/%").find(c) == std::string_view::npos) out.push_back(c);
  }
  if (out.size() > 63) out.resize(63);
  return out;
}

}  // namespace

std::vector<std::string> glyph_names(const ir::Font& font) {
  const std::uint32_t count = num_glyphs(font);
  std::vector<std::string> names;
  if (const ir::FontTable* cff = font.find("CFF ")) names = cff_glyph_names(cff->bytes, count);
  if (names.empty()) {
    if (const ir::FontTable* post = font.find("post")) names = post_glyph_names(post->bytes, count);
  }
  names.resize(count);
  std::map<std::uint16_t, std::uint32_t> reverse;
  if (const ir::FontTable* cmap = font.find("cmap")) {
    if (auto map = parse_cmap(cmap->bytes, count)) {
      for (const auto& [code, glyph] : *map) reverse.try_emplace(glyph, code);
    }
  }
  std::set<std::string> used;
  for (std::uint32_t g = 0; g < count; ++g) {
    std::string name = sanitize_ps_name(names[g]);
    if (g == 0) name = ".notdef";
    if (name.empty()) {
      const auto it = reverse.find(static_cast<std::uint16_t>(g));
      name = it != reverse.end() ? glyph_name_for_unicode(it->second) : std::format("glyph{:05}", g);
    }
    if (used.contains(name)) {
      std::string base = name;
      for (int suffix = 1; used.contains(name); ++suffix) name = std::format("{}.{}", base, suffix);
    }
    used.insert(name);
    names[g] = std::move(name);
  }
  return names;
}

// --------------------------------------------------------------------------- builders

Bytes build_os2(const Os2Basics& b) {
  util::ByteWriter out(96);
  const std::int16_t em = static_cast<std::int16_t>(std::max(1, b.ascender - b.descender));
  out.u16be(4).i16be(b.average_width).u16be(b.weight_class).u16be(5).u16be(0);
  out.i16be(static_cast<std::int16_t>(em * 65 / 100)).i16be(static_cast<std::int16_t>(em * 60 / 100));
  out.i16be(0).i16be(static_cast<std::int16_t>(em * 14 / 100));
  out.i16be(static_cast<std::int16_t>(em * 65 / 100)).i16be(static_cast<std::int16_t>(em * 60 / 100));
  out.i16be(0).i16be(static_cast<std::int16_t>(em * 48 / 100));
  out.i16be(static_cast<std::int16_t>(em * 5 / 100)).i16be(static_cast<std::int16_t>(em * 26 / 100));
  out.i16be(0);
  out.zeros(10);  // PANOSE: any
  out.u32be(b.unicode_range1).u32be(0).u32be(0).u32be(0);
  out.text("NONE");
  std::uint16_t selection = 0;
  if (b.italic) selection |= 1;
  if (b.bold) selection |= 1 << 5;
  if (!b.italic && !b.bold) selection |= 1 << 6;
  selection |= 1 << 7;  // USE_TYPO_METRICS
  out.u16be(selection).u16be(b.first_char).u16be(b.last_char);
  out.i16be(b.ascender).i16be(b.descender).i16be(b.line_gap).u16be(b.win_ascent).u16be(b.win_descent);
  out.u32be(b.code_page1).u32be(0);
  out.i16be(b.x_height).i16be(b.cap_height).u16be(0).u16be(0x20).u16be(2);
  return out.take();
}

Bytes build_head(const HeadBasics& b) {
  util::ByteWriter out(54);
  out.u32be(0x00010000u).u32be(b.revision).u32be(0).u32be(0x5F0F3CF5u).u16be(3).u16be(b.units_per_em);
  out.u64be(0).u64be(0);  // created / modified: deliberately not recorded
  out.i16be(b.x_min).i16be(b.y_min).i16be(b.x_max).i16be(b.y_max).u16be(b.mac_style).u16be(8).i16be(2);
  out.i16be(b.index_to_loc_format).i16be(0);
  return out.take();
}

Bytes build_hhea(const HheaBasics& b) {
  util::ByteWriter out(36);
  out.u32be(0x00010000u).i16be(b.ascender).i16be(b.descender).i16be(b.line_gap).u16be(b.advance_width_max);
  out.i16be(b.min_lsb).i16be(b.min_rsb).i16be(b.x_max_extent).i16be(1).i16be(0).i16be(0);
  out.zeros(8).i16be(0).u16be(b.number_of_hmetrics);
  return out.take();
}

}  // namespace cvt::font
