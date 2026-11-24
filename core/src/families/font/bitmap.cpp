// Convertible core - font family: bitmap fonts (BDF, PCF, PSF, FNT/FON, EBDT/EBLC).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/bitmap.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "families/font/build.hpp"
#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"
#include "util/text.hpp"

namespace cvt::font {

namespace {

constexpr std::uint32_t max_glyph_side = 1024;
constexpr std::size_t max_bitmap_glyphs = 1u << 18;

Result<void> check_glyph_pixels(const Context& ctx, std::uint64_t& total, std::uint32_t w, std::uint32_t h) {
  if (w > max_glyph_side || h > max_glyph_side) return fail(Status::Decode, "a bitmap glyph of {}x{} pixels is too large", w, h);
  total += std::uint64_t{w} * h;
  return ctx.limits.check_pixels(total, 1);
}

std::string property(const ir::BitmapFont& font, std::string_view key) {
  for (const auto& [k, v] : font.properties) {
    if (k == key) return v;
  }
  return {};
}

std::optional<std::int64_t> to_int(std::string_view text) { return util::parse_int(util::trim(text)); }

constexpr std::array<char16_t, 32> cp1252_high = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D,
    0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
    0x0153, 0x009D, 0x017E, 0x0178};

std::int32_t cp1252_to_unicode(std::uint32_t code) {
  if (code >= 0x80 && code < 0xA0) return cp1252_high[code - 0x80];
  return static_cast<std::int32_t>(code);
}

}  // namespace

// --------------------------------------------------------------------------- BDF

Result<ir::BitmapFont> read_bdf(ByteView data, const Context& ctx) {
  const std::string_view text = as_chars(data);
  if (!text.starts_with("STARTFONT")) return fail(Status::Decode, "not a BDF font");
  ir::BitmapFont font;
  const std::vector<std::string_view> lines = util::split_lines(text);
  std::size_t i = 0;
  bool unicode = true;
  std::uint64_t pixels = 0;
  bool in_properties = false;
  while (i < lines.size()) {
    const std::string_view line = util::trim(lines[i++]);
    if (line.empty() || line.starts_with("COMMENT")) continue;
    const std::size_t space = line.find(' ');
    const std::string_view key = line.substr(0, space);
    const std::string_view value = space == std::string_view::npos ? std::string_view{} : util::trim(line.substr(space + 1));
    if (in_properties) {
      if (key == "ENDPROPERTIES") {
        in_properties = false;
        continue;
      }
      std::string v(value);
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = util::replace_all(v.substr(1, v.size() - 2), "\"\"", "\"");
      font.properties.emplace_back(std::string(key), v);
      if (key == "FONT_ASCENT") font.ascent = static_cast<std::int32_t>(to_int(v).value_or(0));
      if (key == "FONT_DESCENT") font.descent = static_cast<std::int32_t>(to_int(v).value_or(0));
      if (key == "PIXEL_SIZE") font.pixel_size = static_cast<std::uint32_t>(std::clamp<std::int64_t>(to_int(v).value_or(0), 0, 4096));
      if (key == "RESOLUTION_X") font.dpi_x = static_cast<std::uint32_t>(std::clamp<std::int64_t>(to_int(v).value_or(75), 1, 10000));
      if (key == "RESOLUTION_Y") font.dpi_y = static_cast<std::uint32_t>(std::clamp<std::int64_t>(to_int(v).value_or(75), 1, 10000));
      if (key == "CHARSET_REGISTRY") {
        const std::string upper = util::to_upper(v);
        unicode = upper.starts_with("ISO10646") || upper.starts_with("ISO8859") || upper.empty();
      }
      if (font.properties.size() > 4096) return fail(Status::Decode, "the BDF font has too many properties");
      continue;
    }
    if (key == "STARTPROPERTIES") {
      in_properties = true;
    } else if (key == "STARTCHAR") {
      ir::BitmapGlyph glyph;
      glyph.name = std::string(value);
      bool bitmap = false;
      std::uint32_t row = 0;
      while (i < lines.size()) {
        const std::string_view l = util::trim(lines[i++]);
        if (l == "ENDCHAR") break;
        if (bitmap) {
          if (row >= glyph.height) continue;
          const std::size_t bytes = (glyph.width + 7) / 8;
          for (std::uint32_t x = 0; x < glyph.width; ++x) {
            const std::size_t nibble_at = x / 4;
            if (nibble_at >= l.size() || nibble_at >= bytes * 2) break;
            const char c = l[nibble_at];
            int v = 0;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return fail(Status::Decode, "BDF glyph '{}' has a malformed bitmap row", glyph.name);
            glyph.pixels[std::size_t{row} * glyph.width + x] = static_cast<std::uint8_t>((v >> (3 - x % 4)) & 1);
          }
          ++row;
          continue;
        }
        const std::vector<std::string_view> parts = util::split(l, ' ', false);
        if (parts.empty()) continue;
        if (parts[0] == "ENCODING" && parts.size() >= 2) {
          const std::int64_t code = to_int(parts[1]).value_or(-1);
          glyph.codepoint = code >= 0 && code <= 0x10FFFF && unicode ? static_cast<std::int32_t>(code) : -1;
        } else if (parts[0] == "DWIDTH" && parts.size() >= 2) {
          glyph.advance = static_cast<std::int32_t>(std::clamp<std::int64_t>(to_int(parts[1]).value_or(0), -4096, 4096));
        } else if (parts[0] == "BBX" && parts.size() >= 5) {
          const auto w = to_int(parts[1]), h = to_int(parts[2]), xo = to_int(parts[3]), yo = to_int(parts[4]);
          if (!w || !h || !xo || !yo || *w < 0 || *h < 0) return fail(Status::Decode, "BDF glyph '{}' has a malformed BBX", glyph.name);
          CVT_TRY(check_glyph_pixels(ctx, pixels, static_cast<std::uint32_t>(std::min<std::int64_t>(*w, 1u << 20)),
                                     static_cast<std::uint32_t>(std::min<std::int64_t>(*h, 1u << 20))));
          glyph.width = static_cast<std::uint32_t>(*w);
          glyph.height = static_cast<std::uint32_t>(*h);
          glyph.x_offset = static_cast<std::int32_t>(std::clamp<std::int64_t>(*xo, -4096, 4096));
          glyph.y_offset = static_cast<std::int32_t>(std::clamp<std::int64_t>(*yo, -4096, 4096));
          glyph.pixels.assign(std::size_t{glyph.width} * glyph.height, 0);
        } else if (parts[0] == "BITMAP") {
          bitmap = true;
        }
      }
      font.glyphs.push_back(std::move(glyph));
      if (font.glyphs.size() > max_bitmap_glyphs) return fail(Status::Decode, "the BDF font has too many glyphs");
      if ((font.glyphs.size() & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size()), "Reading BDF"));
    } else if (key == "SIZE") {
      const std::vector<std::string_view> parts = util::split(value, ' ', false);
      if (!parts.empty() && font.pixel_size == 0) {
        font.pixel_size = static_cast<std::uint32_t>(std::clamp<std::int64_t>(to_int(parts[0]).value_or(0), 0, 4096));
      }
    } else if (key == "ENDFONT") {
      break;
    }
  }
  if (font.glyphs.empty()) return fail(Status::Decode, "the BDF font has no glyphs");
  if (font.ascent == 0 && font.descent == 0) {
    for (const ir::BitmapGlyph& g : font.glyphs) {
      font.ascent = std::max(font.ascent, g.y_offset + static_cast<std::int32_t>(g.height));
      font.descent = std::max(font.descent, -g.y_offset);
    }
  }
  if (font.pixel_size == 0) font.pixel_size = static_cast<std::uint32_t>(std::max(1, font.ascent + font.descent));
  return font;
}

Result<Bytes> write_bdf(const ir::BitmapFont& font) {
  if (font.glyphs.empty()) return fail(Status::Encode, "the bitmap font has no glyphs");
  std::int32_t x_min = 0, y_min = 0, x_max = 0, y_max = 0;
  bool first = true;
  std::int64_t total_width = 0;
  for (const ir::BitmapGlyph& g : font.glyphs) {
    total_width += g.advance;
    if (g.width == 0 || g.height == 0) continue;
    const std::int32_t x1 = g.x_offset + static_cast<std::int32_t>(g.width), y1 = g.y_offset + static_cast<std::int32_t>(g.height);
    if (first) {
      x_min = g.x_offset, y_min = g.y_offset, x_max = x1, y_max = y1;
      first = false;
    }
    x_min = std::min(x_min, g.x_offset);
    y_min = std::min(y_min, g.y_offset);
    x_max = std::max(x_max, x1);
    y_max = std::max(y_max, y1);
  }
  std::string family = property(font, "FAMILY_NAME");
  if (family.empty()) family = "Untitled";
  std::string xlfd_family;
  for (const char c : family) xlfd_family.push_back(c == '-' ? ' ' : c);
  const std::uint32_t size = font.pixel_size ? font.pixel_size : static_cast<std::uint32_t>(font.ascent + font.descent);
  std::string out;
  out += "STARTFONT 2.1\n";
  out += std::format("FONT -misc-{}-medium-r-normal--{}-{}-{}-{}-p-{}-iso10646-1\n", xlfd_family, size, size * 10,
                     font.dpi_x, font.dpi_y, font.glyphs.empty() ? 0 : total_width * 10 / static_cast<std::int64_t>(font.glyphs.size()));
  out += std::format("SIZE {} {} {}\n", size, font.dpi_x, font.dpi_y);
  out += std::format("FONTBOUNDINGBOX {} {} {} {}\n", x_max - x_min, y_max - y_min, x_min, y_min);
  std::vector<std::pair<std::string, std::string>> props;
  const auto quoted = [](std::string_view v) { return "\"" + util::replace_all(v, "\"", "\"\"") + "\""; };
  props.emplace_back("FAMILY_NAME", quoted(family));
  props.emplace_back("PIXEL_SIZE", std::to_string(size));
  props.emplace_back("FONT_ASCENT", std::to_string(font.ascent));
  props.emplace_back("FONT_DESCENT", std::to_string(font.descent));
  props.emplace_back("CHARSET_REGISTRY", quoted("ISO10646"));
  props.emplace_back("CHARSET_ENCODING", quoted("1"));
  for (const char* key : {"COPYRIGHT", "NOTICE", "FULL_NAME", "WEIGHT_NAME"}) {
    const std::string v = property(font, key);
    if (!v.empty()) props.emplace_back(key, quoted(v));
  }
  out += std::format("STARTPROPERTIES {}\n", props.size());
  for (const auto& [k, v] : props) out += k + " " + v + "\n";
  out += "ENDPROPERTIES\n";
  out += std::format("CHARS {}\n", font.glyphs.size());
  std::set<std::string> used;
  for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
    const ir::BitmapGlyph& g = font.glyphs[i];
    std::string name;
    for (const char c : g.name) {
      if (c > 0x20 && c < 0x7F) name.push_back(c);
    }
    if (name.empty()) name = g.codepoint >= 0 ? glyph_name_for_unicode(static_cast<std::uint32_t>(g.codepoint)) : std::format("glyph{}", i);
    if (used.contains(name)) name += std::format(".{}", i);
    used.insert(name);
    out += "STARTCHAR " + name + "\n";
    out += std::format("ENCODING {}\n", g.codepoint);
    out += std::format("SWIDTH {} 0\n", size ? g.advance * 1000 * 72 / static_cast<std::int64_t>(size * font.dpi_x) : 0);
    out += std::format("DWIDTH {} 0\n", g.advance);
    out += std::format("BBX {} {} {} {}\n", g.width, g.height, g.x_offset, g.y_offset);
    out += "BITMAP\n";
    const std::size_t bytes = (g.width + 7) / 8;
    for (std::uint32_t row = 0; row < g.height; ++row) {
      for (std::size_t b = 0; b < bytes; ++b) {
        unsigned v = 0;
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
          const std::size_t x = b * 8 + bit;
          if (x < g.width && g.pixels[std::size_t{row} * g.width + x]) v |= 0x80u >> bit;
        }
        out += std::format("{:02X}", v);
      }
      out += "\n";
    }
    out += "ENDCHAR\n";
  }
  out += "ENDFONT\n";
  return to_bytes(out);
}

// --------------------------------------------------------------------------- PCF

namespace {

enum : std::uint32_t {
  pcf_properties = 1, pcf_accelerators = 2, pcf_metrics = 4, pcf_bitmaps = 8, pcf_ink_metrics = 16,
  pcf_encodings = 32, pcf_swidths = 64, pcf_glyph_names = 128, pcf_bdf_accelerators = 256
};

struct PcfTable {
  std::uint32_t format = 0;
  ByteView data;
};

class PcfReader {
 public:
  PcfReader(ByteView data, std::uint32_t format) : r_(data), msb_((format & 4) != 0) {}
  std::uint32_t u32() { return msb_ ? r_.u32be() : r_.u32le(); }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  std::uint16_t u16() { return msb_ ? r_.u16be() : r_.u16le(); }
  std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
  std::uint8_t u8() { return r_.u8(); }
  util::ByteReader& raw() { return r_; }
  [[nodiscard]] bool ok() const { return r_.ok(); }

 private:
  util::ByteReader r_;
  bool msb_;
};

std::uint32_t le32(ByteView d) {
  util::ByteReader r(d);
  return r.u32le();
}

struct PcfMetric {
  std::int16_t lsb = 0, rsb = 0, width = 0, ascent = 0, descent = 0;
};

PcfMetric read_metric(PcfReader& r, bool compressed) {
  PcfMetric m;
  if (compressed) {
    m.lsb = static_cast<std::int16_t>(r.u8() - 0x80);
    m.rsb = static_cast<std::int16_t>(r.u8() - 0x80);
    m.width = static_cast<std::int16_t>(r.u8() - 0x80);
    m.ascent = static_cast<std::int16_t>(r.u8() - 0x80);
    m.descent = static_cast<std::int16_t>(r.u8() - 0x80);
  } else {
    m.lsb = r.i16();
    m.rsb = r.i16();
    m.width = r.i16();
    m.ascent = r.i16();
    m.descent = r.i16();
    (void)r.u16();
  }
  return m;
}

}  // namespace

Result<ir::BitmapFont> read_pcf(ByteView data, const Context& ctx) {
  util::ByteReader header(data);
  if (!header.consume("\x01" "fcp")) return fail(Status::Decode, "not a PCF font");
  const std::uint32_t count = header.u32le();
  if (!header.ok() || count == 0 || count > 64) return fail(Status::Decode, "the PCF table count is invalid");
  std::map<std::uint32_t, PcfTable> tables;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t type = header.u32le(), format = header.u32le(), size = header.u32le(), offset = header.u32le();
    if (!header.ok()) return fail(Status::Decode, "the PCF table of contents is truncated");
    if (offset >= data.size() || size < 4) return fail(Status::Decode, "a PCF table is out of range");
    // bdftopcf may count padding past the end of the file in the last table.
    tables[type] = {format, data.subspan(offset, std::min<std::size_t>(size, data.size() - offset))};
  }
  const auto get = [&](std::uint32_t type) -> const PcfTable* {
    const auto it = tables.find(type);
    return it == tables.end() ? nullptr : &it->second;
  };
  const PcfTable* metrics_table = get(pcf_metrics);
  const PcfTable* bitmaps_table = get(pcf_bitmaps);
  if (!metrics_table || !bitmaps_table) return fail(Status::Decode, "the PCF font has no metrics or bitmaps");
  ir::BitmapFont font;
  // Properties.
  if (const PcfTable* t = get(pcf_properties)) {
    PcfReader r(t->data, le32(t->data));
    (void)r.raw().u32le();
    const std::uint32_t n = r.u32();
    if (n < 4096) {
      struct Prop { std::uint32_t name; bool is_string; std::uint32_t value; };
      std::vector<Prop> props;
      for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        Prop p{r.u32(), r.u8() != 0, r.u32()};
        props.push_back(p);
      }
      r.raw().skip((n & 3) == 0 ? 0 : 4 - (n & 3));
      const std::uint32_t string_size = r.u32();
      const ByteView strings = r.raw().bytes(string_size);
      const auto str = [&](std::uint32_t at) -> std::string {
        if (at >= strings.size()) return {};
        const std::string_view s = as_chars(strings.subspan(at));
        return std::string(s.substr(0, s.find('\0')));
      };
      if (r.ok()) {
        for (const Prop& p : props) {
          const std::string name = str(p.name);
          const std::string value = p.is_string ? str(p.value) : std::to_string(static_cast<std::int32_t>(p.value));
          font.properties.emplace_back(name, value);
          if (name == "PIXEL_SIZE") font.pixel_size = static_cast<std::uint32_t>(std::clamp<std::int64_t>(to_int(value).value_or(0), 0, 4096));
          if (name == "RESOLUTION_X") font.dpi_x = static_cast<std::uint32_t>(std::clamp<std::int64_t>(to_int(value).value_or(75), 1, 10000));
          if (name == "RESOLUTION_Y") font.dpi_y = static_cast<std::uint32_t>(std::clamp<std::int64_t>(to_int(value).value_or(75), 1, 10000));
        }
      }
    }
  }
  if (const PcfTable* t = get(pcf_bdf_accelerators) ? get(pcf_bdf_accelerators) : get(pcf_accelerators)) {
    PcfReader r(t->data, le32(t->data));
    (void)r.raw().u32le();
    r.raw().skip(8);
    font.ascent = r.i32();
    font.descent = r.i32();
    if (!r.ok()) font.ascent = font.descent = 0;
  }
  // Metrics.
  PcfReader mr(metrics_table->data, le32(metrics_table->data));
  const std::uint32_t mformat = mr.raw().u32le();
  const bool compressed = (mformat & 0xFFFFFF00u) == 0x100;
  const std::uint32_t glyph_count = compressed ? mr.u16() : mr.u32();
  if (glyph_count > max_bitmap_glyphs) return fail(Status::Decode, "the PCF font has too many glyphs");
  std::vector<PcfMetric> metrics(glyph_count);
  for (PcfMetric& m : metrics) m = read_metric(mr, compressed);
  if (!mr.ok()) return fail(Status::Decode, "the PCF metrics are truncated");
  // Bitmaps.
  PcfReader br(bitmaps_table->data, le32(bitmaps_table->data));
  const std::uint32_t bformat = br.raw().u32le();
  if (br.u32() != glyph_count) return fail(Status::Decode, "PCF bitmap and metric counts differ");
  std::vector<std::uint32_t> offsets(glyph_count);
  for (std::uint32_t& o : offsets) o = br.u32();
  std::array<std::uint32_t, 4> sizes{};
  for (std::uint32_t& s : sizes) s = br.u32();
  const std::uint32_t pad_index = bformat & 3;
  const ByteView bits = br.raw().bytes(std::min<std::size_t>(sizes[pad_index], br.raw().remaining()));
  if (!br.ok()) return fail(Status::Decode, "the PCF bitmaps are truncated");
  const std::uint32_t pad = 1u << pad_index;
  const bool msb_bit = (bformat & 8) != 0;
  const std::uint32_t unit = 1u << ((bformat >> 4) & 3);
  const bool msb_byte = (bformat & 4) != 0;
  std::uint64_t pixels = 0;
  font.glyphs.resize(glyph_count);
  for (std::uint32_t g = 0; g < glyph_count; ++g) {
    const PcfMetric& m = metrics[g];
    ir::BitmapGlyph& glyph = font.glyphs[g];
    const std::int32_t w = m.rsb - m.lsb, h = m.ascent + m.descent;
    glyph.advance = m.width;
    if (w <= 0 || h <= 0) continue;
    CVT_TRY(check_glyph_pixels(ctx, pixels, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)));
    glyph.width = static_cast<std::uint32_t>(w);
    glyph.height = static_cast<std::uint32_t>(h);
    glyph.x_offset = m.lsb;
    glyph.y_offset = -m.descent;
    glyph.pixels.assign(std::size_t{glyph.width} * glyph.height, 0);
    const std::uint32_t row_bytes = (glyph.width + 7) / 8;
    const std::uint32_t stride = (row_bytes + pad - 1) / pad * pad;
    for (std::uint32_t y = 0; y < glyph.height; ++y) {
      for (std::uint32_t x = 0; x < glyph.width; ++x) {
        std::uint64_t byte_index = std::uint64_t{offsets[g]} + std::uint64_t{y} * stride + x / 8;
        if (!msb_byte && unit > 1) {
          // swap bytes within each scan unit
          const std::uint64_t in_unit = byte_index % unit;
          byte_index = byte_index - in_unit + (unit - 1 - in_unit);
        }
        if (byte_index >= bits.size()) return fail(Status::Decode, "PCF glyph {} lies outside the bitmap data", g);
        const unsigned v = std::to_integer<unsigned>(bits[static_cast<std::size_t>(byte_index)]);
        const unsigned bit = msb_bit ? (v >> (7 - x % 8)) & 1 : (v >> (x % 8)) & 1;
        glyph.pixels[std::size_t{y} * glyph.width + x] = static_cast<std::uint8_t>(bit);
      }
    }
  }
  // Encodings.
  if (const PcfTable* t = get(pcf_encodings)) {
    PcfReader r(t->data, le32(t->data));
    (void)r.raw().u32le();
    const std::uint16_t min2 = r.u16(), max2 = r.u16(), min1 = r.u16(), max1 = r.u16();
    (void)r.u16();
    if (r.ok() && min2 <= max2 && min1 <= max1 && max2 < 256 && max1 < 256) {
      for (std::uint32_t b1 = min1; b1 <= max1; ++b1) {
        for (std::uint32_t b2 = min2; b2 <= max2; ++b2) {
          const std::uint16_t index = r.u16();
          if (!r.ok()) break;
          if (index < glyph_count && font.glyphs[index].codepoint < 0) {
            font.glyphs[index].codepoint = static_cast<std::int32_t>((b1 << 8) | b2);
          }
        }
      }
    }
  }
  if (const PcfTable* t = get(pcf_glyph_names)) {
    PcfReader r(t->data, le32(t->data));
    (void)r.raw().u32le();
    const std::uint32_t n = r.u32();
    if (n == glyph_count) {
      std::vector<std::uint32_t> name_offsets(n);
      for (std::uint32_t& o : name_offsets) o = r.u32();
      const std::uint32_t size = r.u32();
      const ByteView strings = r.raw().bytes(size);
      if (r.ok()) {
        for (std::uint32_t g = 0; g < n; ++g) {
          if (name_offsets[g] >= strings.size()) continue;
          const std::string_view s = as_chars(strings.subspan(name_offsets[g]));
          font.glyphs[g].name = std::string(s.substr(0, s.find('\0')));
        }
      }
    }
  }
  if (font.ascent == 0 && font.descent == 0) {
    for (const PcfMetric& m : metrics) {
      font.ascent = std::max<std::int32_t>(font.ascent, m.ascent);
      font.descent = std::max<std::int32_t>(font.descent, m.descent);
    }
  }
  if (font.pixel_size == 0) font.pixel_size = static_cast<std::uint32_t>(std::max(1, font.ascent + font.descent));
  // Drop glyphs that no encoding refers to, except glyph 0 (the default).
  return font;
}

Result<Bytes> write_pcf(const ir::BitmapFont& font) {
  if (font.glyphs.empty()) return fail(Status::Encode, "the bitmap font has no glyphs");
  constexpr std::uint32_t format = 0x0E;  // MSB byte and bit order, 4-byte row padding
  const auto be32 = [](util::ByteWriter& w, std::uint32_t v) { w.u32be(v); };
  std::vector<std::pair<std::uint32_t, Bytes>> tables;
  // Properties.
  {
    std::vector<std::pair<std::string, std::string>> props = {
        {"FAMILY_NAME", property(font, "FAMILY_NAME").empty() ? std::string("Untitled") : property(font, "FAMILY_NAME")},
        {"PIXEL_SIZE", std::to_string(font.pixel_size)},
        {"FONT_ASCENT", std::to_string(font.ascent)},
        {"FONT_DESCENT", std::to_string(font.descent)},
        {"CHARSET_REGISTRY", "ISO10646"},
        {"CHARSET_ENCODING", "1"}};
    const std::string copyright = property(font, "COPYRIGHT");
    if (!copyright.empty()) props.emplace_back("COPYRIGHT", copyright);
    std::string strings;
    util::ByteWriter w;
    w.u32le(format);
    be32(w, static_cast<std::uint32_t>(props.size()));
    for (const auto& [name, value] : props) {
      const bool numeric = name == "PIXEL_SIZE" || name == "FONT_ASCENT" || name == "FONT_DESCENT";
      be32(w, static_cast<std::uint32_t>(strings.size()));
      strings += name;
      strings.push_back('\0');
      w.u8(numeric ? 0 : 1);
      if (numeric) {
        be32(w, static_cast<std::uint32_t>(to_int(value).value_or(0)));
      } else {
        be32(w, static_cast<std::uint32_t>(strings.size()));
        strings += value;
        strings.push_back('\0');
      }
    }
    if (props.size() & 3) w.zeros(4 - (props.size() & 3));
    be32(w, static_cast<std::uint32_t>(strings.size()));
    w.text(strings);
    w.align(4);
    tables.emplace_back(pcf_properties, w.take());
  }
  // Metrics and bitmaps.
  struct Box {
    std::int16_t lsb, rsb, width, ascent, descent;
  };
  std::vector<Box> boxes;
  for (const ir::BitmapGlyph& g : font.glyphs) {
    Box b{static_cast<std::int16_t>(g.x_offset), static_cast<std::int16_t>(g.x_offset + static_cast<std::int32_t>(g.width)),
          static_cast<std::int16_t>(g.advance), static_cast<std::int16_t>(g.y_offset + static_cast<std::int32_t>(g.height)),
          static_cast<std::int16_t>(-g.y_offset)};
    if (g.width == 0 || g.height == 0) b = {0, 0, static_cast<std::int16_t>(g.advance), 0, 0};
    boxes.push_back(b);
  }
  Box min_box = boxes[0], max_box = boxes[0];
  for (const Box& b : boxes) {
    min_box = {std::min(min_box.lsb, b.lsb), std::min(min_box.rsb, b.rsb), std::min(min_box.width, b.width),
               std::min(min_box.ascent, b.ascent), std::min(min_box.descent, b.descent)};
    max_box = {std::max(max_box.lsb, b.lsb), std::max(max_box.rsb, b.rsb), std::max(max_box.width, b.width),
               std::max(max_box.ascent, b.ascent), std::max(max_box.descent, b.descent)};
  }
  const auto write_box = [](util::ByteWriter& w, const Box& b) {
    w.i16be(b.lsb).i16be(b.rsb).i16be(b.width).i16be(b.ascent).i16be(b.descent).u16be(0);
  };
  const auto accelerators = [&]() {
    util::ByteWriter w;
    w.u32le(format);
    bool constant = true;
    for (const Box& b : boxes) constant &= b.width == boxes[0].width;
    w.u8(1).u8(0).u8(0).u8(constant ? 1 : 0).u8(1).u8(0).u8(0).u8(0);
    w.i32be(font.ascent).i32be(font.descent).i32be(max_box.rsb - max_box.width > 0 ? max_box.rsb - max_box.width : 0);
    write_box(w, min_box);
    write_box(w, max_box);
    return w.take();
  };
  tables.emplace_back(pcf_accelerators, accelerators());
  {
    util::ByteWriter w;
    w.u32le(format);
    be32(w, static_cast<std::uint32_t>(boxes.size()));
    for (const Box& b : boxes) write_box(w, b);
    tables.emplace_back(pcf_metrics, w.take());
  }
  {
    util::ByteWriter data;
    std::vector<std::uint32_t> offsets;
    for (const ir::BitmapGlyph& g : font.glyphs) {
      offsets.push_back(static_cast<std::uint32_t>(data.size()));
      if (g.width == 0 || g.height == 0) continue;
      const std::uint32_t stride = ((g.width + 7) / 8 + 3) / 4 * 4;
      for (std::uint32_t y = 0; y < g.height; ++y) {
        for (std::uint32_t b = 0; b < stride; ++b) {
          unsigned v = 0;
          for (std::uint32_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t x = b * 8 + bit;
            if (x < g.width && g.pixels[std::size_t{y} * g.width + x]) v |= 0x80u >> bit;
          }
          data.u8(static_cast<std::uint8_t>(v));
        }
      }
    }
    util::ByteWriter w;
    w.u32le(format);
    be32(w, static_cast<std::uint32_t>(font.glyphs.size()));
    for (const std::uint32_t o : offsets) be32(w, o);
    // Sizes for paddings 1, 2, 4, 8: only the one in use is exact; others are estimates readers ignore.
    for (int i = 0; i < 4; ++i) be32(w, static_cast<std::uint32_t>(data.size()));
    w.bytes(data.view());
    tables.emplace_back(pcf_bitmaps, w.take());
  }
  {
    std::map<std::uint32_t, std::uint16_t> codes;
    for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
      const std::int32_t c = font.glyphs[i].codepoint;
      if (c >= 0 && c <= 0xFFFF) codes.try_emplace(static_cast<std::uint32_t>(c), static_cast<std::uint16_t>(i));
    }
    std::uint32_t min1 = 255, max1 = 0, min2 = 255, max2 = 0;
    for (const auto& [c, i] : codes) {
      min1 = std::min(min1, c >> 8), max1 = std::max(max1, c >> 8);
      min2 = std::min(min2, c & 0xFF), max2 = std::max(max2, c & 0xFF);
    }
    if (codes.empty()) min1 = max1 = min2 = max2 = 0;
    util::ByteWriter w;
    w.u32le(format);
    w.u16be(static_cast<std::uint16_t>(min2)).u16be(static_cast<std::uint16_t>(max2));
    w.u16be(static_cast<std::uint16_t>(min1)).u16be(static_cast<std::uint16_t>(max1)).u16be(0);
    for (std::uint32_t b1 = min1; b1 <= max1; ++b1) {
      for (std::uint32_t b2 = min2; b2 <= max2; ++b2) {
        const auto it = codes.find((b1 << 8) | b2);
        w.u16be(it == codes.end() ? 0xFFFF : it->second);
      }
    }
    w.align(4);
    tables.emplace_back(pcf_encodings, w.take());
  }
  {
    util::ByteWriter w;
    w.u32le(format);
    be32(w, static_cast<std::uint32_t>(font.glyphs.size()));
    const std::uint32_t size = font.pixel_size ? font.pixel_size : 1;
    for (const ir::BitmapGlyph& g : font.glyphs) be32(w, static_cast<std::uint32_t>(g.advance * 72000 / static_cast<std::int64_t>(size * font.dpi_x)));
    tables.emplace_back(pcf_swidths, w.take());
  }
  {
    util::ByteWriter w;
    w.u32le(format);
    be32(w, static_cast<std::uint32_t>(font.glyphs.size()));
    std::string strings;
    for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
      be32(w, static_cast<std::uint32_t>(strings.size()));
      const ir::BitmapGlyph& g = font.glyphs[i];
      strings += !g.name.empty() ? g.name
                                 : (g.codepoint >= 0 ? glyph_name_for_unicode(static_cast<std::uint32_t>(g.codepoint)) : std::format("glyph{}", i));
      strings.push_back('\0');
    }
    be32(w, static_cast<std::uint32_t>(strings.size()));
    w.text(strings);
    w.align(4);
    tables.emplace_back(pcf_glyph_names, w.take());
  }
  tables.emplace_back(pcf_bdf_accelerators, accelerators());
  util::ByteWriter out;
  out.text("\x01" "fcp").u32le(static_cast<std::uint32_t>(tables.size()));
  std::uint32_t offset = 8 + 16u * static_cast<std::uint32_t>(tables.size());
  for (const auto& [type, bytes] : tables) {
    out.u32le(type).u32le(format).u32le(static_cast<std::uint32_t>(bytes.size())).u32le(offset);
    offset += static_cast<std::uint32_t>((bytes.size() + 3) / 4 * 4);
  }
  for (const auto& [type, bytes] : tables) {
    out.bytes(bytes);
    out.align(4);
  }
  return out.take();
}

// --------------------------------------------------------------------------- PSF

Result<ir::BitmapFont> read_psf(ByteView data, const Context& ctx) {
  util::ByteReader r(data);
  std::uint32_t count = 0, height = 0, width = 8, header = 0, charsize = 0;
  bool table = false, psf2 = false;
  if (r.consume("\x36\x04")) {
    const std::uint8_t mode = r.u8();
    charsize = r.u8();
    count = (mode & 1) ? 512 : 256;
    table = (mode & 6) != 0;
    height = charsize;
    header = 4;
  } else if (r.consume("\x72\xb5\x4a\x86")) {
    psf2 = true;
    (void)r.u32le();
    header = r.u32le();
    table = (r.u32le() & 1) != 0;
    count = r.u32le();
    charsize = r.u32le();
    height = r.u32le();
    width = r.u32le();
  } else {
    return fail(Status::Decode, "not a PC screen font");
  }
  if (!r.ok() || width == 0 || height == 0 || width > max_glyph_side || height > max_glyph_side || count == 0 ||
      count > 65536) {
    return fail(Status::Decode, "the PSF header is invalid");
  }
  const std::uint32_t row_bytes = (width + 7) / 8;
  if (charsize < row_bytes * height) return fail(Status::Decode, "the PSF glyph size is inconsistent");
  if (std::uint64_t{header} + std::uint64_t{count} * charsize > data.size()) return fail(Status::Decode, "the PSF glyphs are truncated");
  CVT_TRY(ctx.limits.check_pixels(std::uint64_t{width} * height, count));
  ir::BitmapFont font;
  font.pixel_size = height;
  font.ascent = static_cast<std::int32_t>(height - height / 4);
  font.descent = static_cast<std::int32_t>(height / 4);
  font.glyphs.resize(count);
  for (std::uint32_t g = 0; g < count; ++g) {
    ir::BitmapGlyph& glyph = font.glyphs[g];
    glyph.width = width;
    glyph.height = height;
    glyph.advance = static_cast<std::int32_t>(width);
    glyph.y_offset = -font.descent;
    glyph.pixels.resize(std::size_t{width} * height);
    const std::size_t base = header + std::size_t{g} * charsize;
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const unsigned v = std::to_integer<unsigned>(data[base + std::size_t{y} * row_bytes + x / 8]);
        glyph.pixels[std::size_t{y} * width + x] = static_cast<std::uint8_t>((v >> (7 - x % 8)) & 1);
      }
    }
  }
  if (table) {
    r.seek(header + std::size_t{count} * charsize);
    std::vector<std::vector<std::int32_t>> extra(count);
    for (std::uint32_t g = 0; g < count && !r.at_end(); ++g) {
      bool in_sequence = false;
      while (!r.at_end()) {
        if (psf2) {
          const std::uint8_t b = r.u8();
          if (b == 0xFF) break;
          if (b == 0xFE) {
            in_sequence = true;
            continue;
          }
          // UTF-8 code point
          int len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
          std::uint32_t cp = len == 1 ? b : len == 2 ? (b & 0x1Fu) : len == 3 ? (b & 0x0Fu) : (b & 0x07u);
          for (int k = 1; k < len; ++k) cp = (cp << 6) | (r.u8() & 0x3Fu);
          if (!in_sequence && cp <= 0x10FFFF) extra[g].push_back(static_cast<std::int32_t>(cp));
        } else {
          const std::uint16_t v = r.u16le();
          if (v == 0xFFFF) break;
          if (v == 0xFFFE) {
            in_sequence = true;
            continue;
          }
          if (!in_sequence) extra[g].push_back(v);
        }
        if (!r.ok()) break;
      }
    }
    // Each glyph gets its first code point; further code points that map to
    // the same picture become copies, so text in any of them renders.
    std::vector<ir::BitmapGlyph> copies;
    for (std::uint32_t g = 0; g < count; ++g) {
      font.glyphs[g].codepoint = extra[g].empty() ? -1 : extra[g][0];
      for (std::size_t k = 1; k < extra[g].size() && copies.size() < 65536; ++k) {
        ir::BitmapGlyph copy = font.glyphs[g];
        copy.codepoint = extra[g][k];
        copies.push_back(std::move(copy));
      }
    }
    font.glyphs.insert(font.glyphs.end(), copies.begin(), copies.end());
  } else {
    ctx.warn("The PSF font has no Unicode table; glyph positions were taken as code points.");
    for (std::uint32_t g = 0; g < count; ++g) font.glyphs[g].codepoint = static_cast<std::int32_t>(g);
  }
  return font;
}

Result<Bytes> write_psf(const ir::BitmapFont& font, const Context& ctx) {
  if (font.glyphs.empty()) return fail(Status::Encode, "the bitmap font has no glyphs");
  std::int32_t width = 0;
  bool varying = false;
  for (const ir::BitmapGlyph& g : font.glyphs) {
    if (width && g.advance && g.advance != width) varying = true;
    width = std::max(width, g.advance);
  }
  if (varying) ctx.warn("PSF fonts have one cell width; proportional glyphs were placed in fixed cells.");
  // The cell covers the advance and every pixel of every glyph.
  std::int32_t left = 0, right = std::max(width, 1), top = font.ascent, bottom = font.descent;
  for (const ir::BitmapGlyph& g : font.glyphs) {
    if (g.width == 0 || g.height == 0) continue;
    left = std::min(left, g.x_offset);
    right = std::max(right, g.x_offset + static_cast<std::int32_t>(g.width));
    top = std::max(top, g.y_offset + static_cast<std::int32_t>(g.height));
    bottom = std::max(bottom, -g.y_offset);
  }
  const std::uint32_t height = static_cast<std::uint32_t>(std::clamp(top + bottom, 1, 1024));
  const std::uint32_t cell = static_cast<std::uint32_t>(std::clamp(right - left, 1, 255));
  const std::uint32_t row_bytes = (cell + 7) / 8;
  std::vector<const ir::BitmapGlyph*> glyphs;
  for (const ir::BitmapGlyph& g : font.glyphs) {
    if (g.codepoint >= 0) glyphs.push_back(&g);
  }
  if (glyphs.empty()) return fail(Status::Encode, "no glyph of the font has a code point");
  util::ByteWriter out;
  out.text("\x72\xb5\x4a\x86").u32le(0).u32le(32).u32le(1).u32le(static_cast<std::uint32_t>(glyphs.size()));
  out.u32le(row_bytes * height).u32le(height).u32le(cell);
  for (const ir::BitmapGlyph* g : glyphs) {
    std::vector<std::uint8_t> rows(std::size_t{row_bytes} * height, 0);
    for (std::uint32_t y = 0; y < g->height; ++y) {
      const std::int64_t cy = std::int64_t{top} - (g->y_offset + static_cast<std::int64_t>(g->height)) + y;
      if (cy < 0 || cy >= height) continue;
      for (std::uint32_t x = 0; x < g->width; ++x) {
        const std::int64_t cx = std::int64_t{g->x_offset} - left + x;
        if (cx < 0 || cx >= cell || !g->pixels[std::size_t{y} * g->width + x]) continue;
        rows[static_cast<std::size_t>(cy) * row_bytes + static_cast<std::size_t>(cx / 8)] |= static_cast<std::uint8_t>(0x80u >> (cx % 8));
      }
    }
    out.bytes(std::span<const std::uint8_t>(rows));
  }
  for (const ir::BitmapGlyph* g : glyphs) {
    std::string utf8;
    const auto cp = static_cast<std::uint32_t>(g->codepoint);
    if (cp < 0x80) utf8.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
      utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    out.text(utf8).u8(0xFF);
  }
  return out.take();
}

// --------------------------------------------------------------------------- Windows FNT / FON

bool looks_like_fnt(ByteView data) noexcept {
  if (data.size() < 118) return false;
  const std::uint16_t version = static_cast<std::uint16_t>(std::to_integer<unsigned>(data[0]) | (std::to_integer<unsigned>(data[1]) << 8));
  if (version != 0x0200 && version != 0x0300) return false;
  util::ByteReader r(data);
  r.skip(2);
  const std::uint32_t size = r.u32le();
  if (size > data.size() || size < 118) return false;
  for (std::size_t i = 6; i < 66; ++i) {
    const auto c = std::to_integer<unsigned>(data[i]);
    if (c != 0 && (c < 0x20 || c > 0x7E) && c < 0xA0) return false;
  }
  return true;
}

Result<ir::BitmapFont> read_fnt(ByteView data, const Context& ctx) {
  util::ByteReader r(data);
  const std::uint16_t version = r.u16le();
  const std::uint32_t size = r.u32le();
  const std::string_view copyright = r.chars(60);
  const std::uint16_t type = r.u16le();
  r.skip(2 + 2 + 2);  // points, resolutions
  const std::uint16_t ascent = r.u16le();
  r.skip(2 + 2 + 3);  // leading, italic/underline/strikeout
  r.skip(2);           // weight
  const std::uint8_t charset = r.u8();
  const std::uint16_t pix_width = r.u16le();
  const std::uint16_t pix_height = r.u16le();
  r.skip(1 + 2 + 2);
  const std::uint8_t first = r.u8();
  const std::uint8_t last = r.u8();
  r.skip(2 + 2 + 4);  // default, break, width bytes, device
  const std::uint32_t face_offset = r.u32le();
  if (!r.ok() || (version != 0x0200 && version != 0x0300)) return fail(Status::Decode, "not a Windows font resource");
  if (type & 1) return fail(Status::Decode, "vector Windows fonts are not supported");
  if (size > data.size()) ctx.warn("The FNT size field exceeds the data.");
  if (last < first || pix_height == 0 || pix_height > max_glyph_side) return fail(Status::Decode, "the FNT header is invalid");
  ir::BitmapFont font;
  font.pixel_size = pix_height;
  font.ascent = ascent;
  font.descent = static_cast<std::int32_t>(pix_height) - ascent;
  const std::string_view c = copyright.substr(0, copyright.find('\0'));
  if (!c.empty()) font.properties.emplace_back("COPYRIGHT", util::sanitize_utf8(c));
  if (face_offset > 0 && face_offset < data.size()) {
    const std::string_view face = as_chars(data.subspan(face_offset));
    font.properties.emplace_back("FAMILY_NAME", util::sanitize_utf8(face.substr(0, std::min<std::size_t>(face.find('\0'), 64))));
  }
  const std::size_t table = version == 0x0300 ? 148 : 118;
  const std::size_t entry = version == 0x0300 ? 6 : 4;
  std::uint64_t pixels = 0;
  for (std::uint32_t code = first; code <= last; ++code) {
    const std::size_t at = table + (code - first) * entry;
    if (at + entry > data.size()) return fail(Status::Decode, "the FNT character table is truncated");
    const auto w = static_cast<std::uint16_t>(std::to_integer<unsigned>(data[at]) | (std::to_integer<unsigned>(data[at + 1]) << 8));
    const std::uint32_t offset = version == 0x0300
        ? (std::to_integer<std::uint32_t>(data[at + 2]) | (std::to_integer<std::uint32_t>(data[at + 3]) << 8) |
           (std::to_integer<std::uint32_t>(data[at + 4]) << 16) | (std::to_integer<std::uint32_t>(data[at + 5]) << 24))
        : (std::to_integer<std::uint32_t>(data[at + 2]) | (std::to_integer<std::uint32_t>(data[at + 3]) << 8));
    const std::uint32_t width = pix_width ? pix_width : w;
    if (width > max_glyph_side) return fail(Status::Decode, "an FNT glyph is too wide");
    CVT_TRY(check_glyph_pixels(ctx, pixels, width, pix_height));
    ir::BitmapGlyph glyph;
    glyph.codepoint = charset == 0 ? cp1252_to_unicode(code) : static_cast<std::int32_t>(code);
    glyph.advance = static_cast<std::int32_t>(width);
    glyph.width = width;
    glyph.height = pix_height;
    glyph.y_offset = -font.descent;
    glyph.pixels.assign(std::size_t{width} * pix_height, 0);
    const std::uint32_t columns = (width + 7) / 8;
    if (std::uint64_t{offset} + std::uint64_t{columns} * pix_height > data.size()) {
      return fail(Status::Decode, "FNT glyph {} lies outside the file", code);
    }
    for (std::uint32_t col = 0; col < columns; ++col) {
      for (std::uint32_t y = 0; y < pix_height; ++y) {
        const unsigned v = std::to_integer<unsigned>(data[offset + std::size_t{col} * pix_height + y]);
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
          const std::uint32_t x = col * 8 + bit;
          if (x < width) glyph.pixels[std::size_t{y} * width + x] = static_cast<std::uint8_t>((v >> (7 - bit)) & 1);
        }
      }
    }
    font.glyphs.push_back(std::move(glyph));
  }
  if (charset != 0) ctx.warn("The FNT font uses a non-ANSI character set; codes were taken as code points.");
  return font;
}

bool looks_like_fon(ByteView data) noexcept {
  if (data.size() < 0x40 || get_u16(data, 0) != 0x4D5A) return false;
  const std::uint32_t ne = std::to_integer<std::uint32_t>(data[0x3C]) | (std::to_integer<std::uint32_t>(data[0x3D]) << 8) |
                           (std::to_integer<std::uint32_t>(data[0x3E]) << 16) | (std::to_integer<std::uint32_t>(data[0x3F]) << 24);
  if (std::uint64_t{ne} + 0x40 > data.size() || get_u16(data, ne) != 0x4E45) return false;
  const std::size_t table = ne + (std::to_integer<std::size_t>(data[ne + 0x24]) | (std::to_integer<std::size_t>(data[ne + 0x25]) << 8));
  for (std::size_t at = table + 2; at + 8 <= data.size();) {
    const std::uint16_t type = static_cast<std::uint16_t>(std::to_integer<unsigned>(data[at]) | (std::to_integer<unsigned>(data[at + 1]) << 8));
    if (type == 0) break;
    if (type == 0x8008) return true;
    const std::uint16_t count = static_cast<std::uint16_t>(std::to_integer<unsigned>(data[at + 2]) | (std::to_integer<unsigned>(data[at + 3]) << 8));
    at += 8 + 12u * count;
  }
  return false;
}

Result<std::vector<ir::BitmapFont>> read_fon(ByteView data, const Context& ctx) {
  util::ByteReader r(data);
  if (r.u16le() != 0x5A4D) return fail(Status::Decode, "not a Windows .fon file");
  r.seek(0x3C);
  const std::uint32_t ne = r.u32le();
  if (!r.ok() || std::uint64_t{ne} + 0x40 > data.size() || get_u16(data, ne) != 0x4E45) {
    return fail(Status::Decode, "the .fon file is not a 16-bit (NE) executable");
  }
  util::ByteReader nr(data);
  nr.seek(ne + 0x24);
  const std::uint16_t table_offset = nr.u16le();
  util::ByteReader t(data);
  if (!t.seek(std::size_t{ne} + table_offset)) return fail(Status::Decode, "the .fon resource table is out of range");
  const std::uint16_t shift = t.u16le();
  if (shift > 16) return fail(Status::Decode, "the .fon resource alignment is invalid");
  std::vector<ir::BitmapFont> fonts;
  for (int guard = 0; guard < 1024; ++guard) {
    const std::uint16_t type = t.u16le();
    if (!t.ok() || type == 0) break;
    const std::uint16_t count = t.u16le();
    t.skip(4);
    for (std::uint16_t i = 0; i < count; ++i) {
      const std::uint64_t offset = std::uint64_t{t.u16le()} << shift;
      const std::uint64_t length = std::uint64_t{t.u16le()} << shift;
      t.skip(8);
      if (!t.ok()) return fail(Status::Decode, "the .fon resource table is truncated");
      if (type != 0x8008) continue;
      if (offset >= data.size()) return fail(Status::Decode, "a .fon font resource is out of range");
      const std::uint64_t end = std::min<std::uint64_t>(offset + length, data.size());
      CVT_TRY_ASSIGN(ir::BitmapFont f, read_fnt(data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(end - offset)), ctx));
      fonts.push_back(std::move(f));
      CVT_TRY(ctx.limits.check_archive_entries(fonts.size()));
    }
  }
  if (fonts.empty()) return fail(Status::Decode, "the .fon file holds no fonts");
  return fonts;
}

// --------------------------------------------------------------------------- EBLC / EBDT

Result<ir::BitmapFont> read_embedded_bitmaps(const ir::Font& font, const Context& ctx) {
  const ir::FontTable* eblc = font.find("EBLC") ? font.find("EBLC") : font.find("bloc");
  const ir::FontTable* ebdt = font.find("EBDT") ? font.find("EBDT") : font.find("bdat");
  if (!eblc || !ebdt) return fail(Status::Decode, "the font has no embedded bitmaps");
  const ByteView loc = eblc->bytes, dat = ebdt->bytes;
  const std::uint32_t sizes = get_u32(loc, 4);
  if (sizes == 0 || sizes > 256 || 8 + 48ull * sizes > loc.size()) return fail(Status::Decode, "the EBLC table is invalid");
  // Use the first strike with 1-bit depth.
  std::size_t strike = 8;
  for (std::uint32_t s = 0; s < sizes; ++s) {
    if (std::to_integer<unsigned>(loc[8 + 48 * s + 46]) == 1) {
      strike = 8 + 48 * s;
      break;
    }
  }
  if (std::to_integer<unsigned>(loc[strike + 46]) != 1) {
    return fail(Status::Decode, "only 1-bit embedded bitmaps are supported");
  }
  const std::uint32_t array_offset = get_u32(loc, strike);
  const std::uint32_t subtables = get_u32(loc, strike + 8);
  ir::BitmapFont out;
  out.ascent = static_cast<std::int8_t>(std::to_integer<int>(loc[strike + 16]));
  out.descent = -static_cast<std::int8_t>(std::to_integer<int>(loc[strike + 17]));
  out.pixel_size = std::to_integer<std::uint32_t>(loc[strike + 45]);
  CharMap cmap;
  const std::uint32_t count = num_glyphs(font);
  if (const ir::FontTable* c = font.find("cmap")) {
    if (auto parsed = parse_cmap(c->bytes, count)) cmap = std::move(*parsed);
  }
  std::map<std::uint16_t, std::uint32_t> code_of;
  for (const auto& [code, glyph] : cmap) code_of.try_emplace(glyph, code);
  const std::vector<std::string> names = glyph_names(font);
  std::uint64_t pixels = 0;
  if (subtables > 65536 || std::uint64_t{array_offset} + 8ull * subtables > loc.size()) {
    return fail(Status::Decode, "the EBLC index is out of range");
  }
  for (std::uint32_t s = 0; s < subtables; ++s) {
    const std::size_t entry = array_offset + 8 * s;
    const std::uint16_t first = get_u16(loc, entry), last = get_u16(loc, entry + 2);
    const std::size_t sub = array_offset + std::size_t{get_u32(loc, entry + 4)};
    if (last < first || sub + 8 > loc.size()) return fail(Status::Decode, "an EBLC index subtable is invalid");
    const std::uint16_t index_format = get_u16(loc, sub), image_format = get_u16(loc, sub + 2);
    const std::uint32_t image_base = get_u32(loc, sub + 4);
    struct Item { std::uint16_t glyph; std::uint64_t offset, length; };
    std::vector<Item> items;
    std::array<std::uint8_t, 8> big{};  // metrics from index formats 2 and 5
    const auto read_big = [&](std::size_t at) {
      for (std::size_t k = 0; k < 8; ++k) big[k] = at + k < loc.size() ? std::to_integer<std::uint8_t>(loc[at + k]) : 0;
    };
    const std::uint32_t n = last - first + 1u;
    if (index_format == 1 || index_format == 3) {
      const std::size_t width = index_format == 1 ? 4 : 2;
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t at = sub + 8 + width * i;
        if (at + 2 * width > loc.size()) return fail(Status::Decode, "an EBLC offset array is truncated");
        const std::uint64_t a = width == 4 ? get_u32(loc, at) : get_u16(loc, at);
        const std::uint64_t b = width == 4 ? get_u32(loc, at + 4) : get_u16(loc, at + 2);
        if (b > a) items.push_back({static_cast<std::uint16_t>(first + i), image_base + a, b - a});
      }
    } else if (index_format == 2) {
      const std::uint32_t size = get_u32(loc, sub + 8);
      read_big(sub + 12);
      for (std::uint32_t i = 0; i < n; ++i) items.push_back({static_cast<std::uint16_t>(first + i), image_base + std::uint64_t{size} * i, size});
    } else if (index_format == 4) {
      const std::uint32_t glyphs = get_u32(loc, sub + 8);
      for (std::uint32_t i = 0; i < glyphs && i < 65536; ++i) {
        const std::size_t at = sub + 12 + 4 * i;
        if (at + 8 > loc.size()) break;
        const std::uint64_t a = get_u16(loc, at + 2), b = get_u16(loc, at + 6);
        if (b > a) items.push_back({get_u16(loc, at), image_base + a, b - a});
      }
    } else if (index_format == 5) {
      const std::uint32_t size = get_u32(loc, sub + 8);
      read_big(sub + 12);
      const std::uint32_t glyphs = get_u32(loc, sub + 20);
      for (std::uint32_t i = 0; i < glyphs && i < 65536; ++i) {
        const std::size_t at = sub + 24 + 2 * i;
        if (at + 2 > loc.size()) break;
        items.push_back({get_u16(loc, at), image_base + std::uint64_t{size} * i, size});
      }
    } else {
      return fail(Status::Decode, "EBLC index format {} is not supported", index_format);
    }
    for (const Item& item : items) {
      if (item.offset + item.length > dat.size()) return fail(Status::Decode, "an EBDT glyph is out of range");
      const ByteView img = dat.subspan(static_cast<std::size_t>(item.offset), static_cast<std::size_t>(item.length));
      ir::BitmapGlyph glyph;
      std::size_t data_at = 0;
      std::uint32_t h = 0, w = 0;
      std::int32_t bx = 0, by = 0, adv = 0;
      bool bit_aligned = false;
      switch (image_format) {
        case 1: case 2:
          if (img.size() < 5) return fail(Status::Decode, "an EBDT glyph is truncated");
          h = std::to_integer<std::uint32_t>(img[0]);
          w = std::to_integer<std::uint32_t>(img[1]);
          bx = static_cast<std::int8_t>(std::to_integer<int>(img[2]));
          by = static_cast<std::int8_t>(std::to_integer<int>(img[3]));
          adv = std::to_integer<std::int32_t>(img[4]);
          data_at = 5;
          bit_aligned = image_format == 2;
          break;
        case 6: case 7:
          if (img.size() < 8) return fail(Status::Decode, "an EBDT glyph is truncated");
          h = std::to_integer<std::uint32_t>(img[0]);
          w = std::to_integer<std::uint32_t>(img[1]);
          bx = static_cast<std::int8_t>(std::to_integer<int>(img[2]));
          by = static_cast<std::int8_t>(std::to_integer<int>(img[3]));
          adv = std::to_integer<std::int32_t>(img[4]);
          data_at = 8;
          bit_aligned = image_format == 7;
          break;
        case 5:
          h = big[0];
          w = big[1];
          bx = static_cast<std::int8_t>(big[2]);
          by = static_cast<std::int8_t>(big[3]);
          adv = big[4];
          bit_aligned = true;
          break;
        default:
          return fail(Status::Decode, "EBDT image format {} is not supported", image_format);
      }
      CVT_TRY(check_glyph_pixels(ctx, pixels, w, h));
      glyph.width = w;
      glyph.height = h;
      glyph.x_offset = bx;
      glyph.y_offset = by - static_cast<std::int32_t>(h);
      glyph.advance = adv;
      glyph.pixels.assign(std::size_t{w} * h, 0);
      const std::size_t stride_bits = bit_aligned ? w : (w + 7) / 8 * 8;
      for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
          const std::size_t bit = y * stride_bits + x;
          const std::size_t byte = data_at + bit / 8;
          if (byte >= img.size()) return fail(Status::Decode, "an EBDT glyph image is truncated");
          glyph.pixels[std::size_t{y} * w + x] = static_cast<std::uint8_t>((std::to_integer<unsigned>(img[byte]) >> (7 - bit % 8)) & 1);
        }
      }
      if (const auto it = code_of.find(item.glyph); it != code_of.end()) glyph.codepoint = static_cast<std::int32_t>(it->second);
      if (item.glyph < names.size()) glyph.name = names[item.glyph];
      out.glyphs.push_back(std::move(glyph));
    }
  }
  if (out.glyphs.empty()) return fail(Status::Decode, "the embedded bitmap strike is empty");
  if (out.pixel_size == 0) out.pixel_size = static_cast<std::uint32_t>(std::max(1, out.ascent + out.descent));
  if (font.names) out.properties.emplace_back("FAMILY_NAME", font.names->family);
  return out;
}

// --------------------------------------------------------------------------- bitmap -> outlines

Result<ir::Font> outline_font_from_bitmap(const ir::BitmapFont& bitmap, bool cff, const Context& ctx) {
  if (bitmap.glyphs.empty()) return fail(Status::Encode, "the bitmap font has no glyphs");
  const std::int32_t extent = std::max(1, bitmap.ascent + bitmap.descent);
  const std::int32_t unit = std::max(1, 1024 / extent);
  FontSpec spec;
  spec.family = property(bitmap, "FAMILY_NAME");
  if (spec.family.empty()) spec.family = "Bitmap";
  spec.copyright = property(bitmap, "COPYRIGHT");
  spec.units_per_em = static_cast<std::uint16_t>(std::clamp(unit * extent, 16, 16384));
  spec.ascender = static_cast<std::int16_t>(bitmap.ascent * unit);
  spec.descender = static_cast<std::int16_t>(-bitmap.descent * unit);
  spec.postscript_winding = false;
  spec.fixed_pitch = true;
  std::int32_t common = -1;
  for (const ir::BitmapGlyph& g : bitmap.glyphs) {
    if (common < 0) common = g.advance;
    if (g.advance != common) spec.fixed_pitch = false;
  }
  spec.glyph_names.push_back(".notdef");
  spec.paths.emplace_back();
  spec.advances.push_back(std::max(common, 1) * static_cast<double>(unit));
  std::set<std::string> used{".notdef"};
  std::size_t index = 0;
  for (const ir::BitmapGlyph& g : bitmap.glyphs) {
    if ((index++ & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(bitmap.glyphs.size()), "Tracing pixels"));
    if (g.codepoint < 0 || spec.cmap.contains(static_cast<std::uint32_t>(g.codepoint))) continue;
    if (spec.glyph_names.size() >= 65535) break;
    Path path;
    // Rectangles: horizontal runs, merged downwards while identical.
    std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> runs(g.height);
    for (std::uint32_t y = 0; y < g.height; ++y) {
      std::uint32_t x = 0;
      while (x < g.width) {
        while (x < g.width && !g.pixels[std::size_t{y} * g.width + x]) ++x;
        const std::uint32_t start = x;
        while (x < g.width && g.pixels[std::size_t{y} * g.width + x]) ++x;
        if (x > start) runs[y].emplace_back(start, x);
      }
    }
    for (std::uint32_t y = 0; y < g.height; ++y) {
      for (const auto& run : runs[y]) {
        std::uint32_t bottom = y + 1;
        while (bottom < g.height) {
          auto& next = runs[bottom];
          const auto it = std::ranges::find(next, run);
          if (it == next.end()) break;
          next.erase(it);
          ++bottom;
        }
        // Pixel rows count down from the top of the bitmap.
        const double x0 = (g.x_offset + static_cast<double>(run.first)) * unit;
        const double x1 = (g.x_offset + static_cast<double>(run.second)) * unit;
        const double y_top = (g.y_offset + static_cast<double>(g.height - y)) * unit;
        const double y_bottom = (g.y_offset + static_cast<double>(g.height - bottom)) * unit;
        path.move_to({x0, y_bottom});
        path.line_to({x0, y_top});
        path.line_to({x1, y_top});
        path.line_to({x1, y_bottom});
        path.close();
      }
    }
    std::string name = g.name;
    if (name.empty() || used.contains(name)) name = glyph_name_for_unicode(static_cast<std::uint32_t>(g.codepoint));
    if (used.contains(name)) name += std::format(".{}", index);
    used.insert(name);
    spec.cmap.emplace(static_cast<std::uint32_t>(g.codepoint), static_cast<std::uint16_t>(spec.glyph_names.size()));
    spec.glyph_names.push_back(std::move(name));
    spec.paths.push_back(std::move(path));
    spec.advances.push_back(static_cast<double>(g.advance) * unit);
  }
  return build_font(spec, cff, ctx);
}

}  // namespace cvt::font
