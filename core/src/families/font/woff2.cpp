// Convertible core - font family: WOFF2 (Brotli, glyf/loca and hmtx transforms, collections).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "families/archive/compression.hpp"
#include "families/font/containers.hpp"
#include "families/font/glyf.hpp"
#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"

namespace cvt::font {

namespace {

constexpr std::array<std::string_view, 63> known_tags = {
    "cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf", "loca", "prep",
    "CFF ", "VORG", "EBDT", "EBLC", "gasp", "hdmx", "kern", "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE",
    "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT", "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt",
    "avar", "bdat", "bloc", "bsln", "cvar", "fdsc", "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar",
    "mort", "morx", "opbd", "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill"};

// ---- variable-length integers

std::optional<std::uint32_t> read_base128(util::ByteReader& r) {
  std::uint32_t value = 0;
  for (int i = 0; i < 5; ++i) {
    const std::uint8_t b = r.u8();
    if (!r.ok()) return std::nullopt;
    if (i == 0 && b == 0x80) return std::nullopt;  // leading zeros
    if (value & 0xFE000000u) return std::nullopt;  // overflow
    value = (value << 7) | (b & 0x7F);
    if (!(b & 0x80)) return value;
  }
  return std::nullopt;
}

void write_base128(util::ByteWriter& out, std::uint32_t value) {
  std::array<std::uint8_t, 5> bytes{};
  int n = 0;
  do {
    bytes[static_cast<std::size_t>(n++)] = static_cast<std::uint8_t>(value & 0x7F);
    value >>= 7;
  } while (value != 0);
  for (int i = n - 1; i >= 0; --i) out.u8(static_cast<std::uint8_t>(bytes[static_cast<std::size_t>(i)] | (i > 0 ? 0x80 : 0)));
}

std::optional<std::uint16_t> read_255ushort(util::ByteReader& r) {
  const std::uint8_t code = r.u8();
  std::uint32_t value = 0;
  if (code == 253) value = r.u16be();
  else if (code == 255) value = r.u8() + 253u;
  else if (code == 254) value = r.u8() + 506u;
  else value = code;
  if (!r.ok()) return std::nullopt;
  return static_cast<std::uint16_t>(value);
}

void write_255ushort(util::ByteWriter& out, std::uint16_t value) {
  if (value < 253) out.u8(static_cast<std::uint8_t>(value));
  else if (value < 506) out.u8(255).u8(static_cast<std::uint8_t>(value - 253));
  else if (value < 762) out.u8(254).u8(static_cast<std::uint8_t>(value - 506));
  else out.u8(253).u16be(value);
}

// ---- directory

struct Woff2Entry {
  std::uint32_t tag = 0;
  std::uint8_t version = 0;
  std::uint32_t orig_length = 0;
  std::uint32_t stream_length = 0;
  bool transformed = false;
  std::uint64_t stream_offset = 0;
};

struct FaceRef {
  std::uint32_t flavor = 0;
  std::vector<std::uint16_t> tables;
};

// ---- glyf reconstruction

std::int32_t with_sign(int flag, std::int32_t value) { return (flag & 1) ? value : -value; }

Result<void> decode_triplets(util::ByteReader& flags, util::ByteReader& glyphs, std::vector<TtPoint>& points) {
  std::int32_t x = 0, y = 0;
  for (TtPoint& p : points) {
    const std::uint8_t full = flags.u8();
    if (!flags.ok()) return fail(Status::Decode, "WOFF2 glyph flags are truncated");
    p.on_curve = (full >> 7) == 0;
    const int flag = full & 0x7F;
    std::int32_t dx = 0, dy = 0;
    if (flag < 10) {
      dy = with_sign(flag, ((flag & 14) << 7) + glyphs.u8());
    } else if (flag < 20) {
      dx = with_sign(flag, (((flag - 10) & 14) << 7) + glyphs.u8());
    } else if (flag < 84) {
      const int b0 = flag - 20;
      const int b1 = glyphs.u8();
      dx = with_sign(flag, 1 + (b0 & 0x30) + (b1 >> 4));
      dy = with_sign(flag >> 1, 1 + ((b0 & 0x0C) << 2) + (b1 & 0x0F));
    } else if (flag < 120) {
      const int b0 = flag - 84;
      const int a = glyphs.u8(), b = glyphs.u8();
      dx = with_sign(flag, 1 + ((b0 / 12) << 8) + a);
      dy = with_sign(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + b);
    } else if (flag < 124) {
      const int a = glyphs.u8(), b = glyphs.u8(), c = glyphs.u8();
      dx = with_sign(flag, (a << 4) + (b >> 4));
      dy = with_sign(flag >> 1, ((b & 0x0F) << 8) + c);
    } else {
      const int a = glyphs.u8(), b = glyphs.u8(), c = glyphs.u8(), d = glyphs.u8();
      dx = with_sign(flag, (a << 8) + b);
      dy = with_sign(flag >> 1, (c << 8) + d);
    }
    if (!glyphs.ok()) return fail(Status::Decode, "WOFF2 glyph coordinates are truncated");
    x += dx;
    y += dy;
    p.x = x;
    p.y = y;
  }
  return {};
}

// Size of the composite component records at the start of `data`, and
// whether any of them asks for instructions.
std::optional<std::size_t> composite_size(ByteView data, bool& has_instructions) {
  util::ByteReader r(data);
  std::uint16_t flags = 0;
  has_instructions = false;
  std::size_t count = 0;
  do {
    flags = r.u16be();
    r.skip(2);
    r.skip((flags & composite::arg_words) ? 4 : 2);
    if (flags & composite::have_scale) r.skip(2);
    else if (flags & composite::xy_scale) r.skip(4);
    else if (flags & composite::two_by_two) r.skip(8);
    if (flags & composite::instructions) has_instructions = true;
    if (!r.ok() || ++count > 4096) return std::nullopt;
  } while (flags & composite::more);
  return r.position();
}

struct Reconstructed {
  Bytes glyf;
  Bytes loca;
  std::vector<std::int16_t> x_min;  // per glyph, 0 for empty glyphs
  std::int16_t index_format = 0;
};

Result<Reconstructed> reconstruct_glyf(ByteView data, const Context& ctx) {
  util::ByteReader r(data);
  const std::uint16_t reserved = r.u16be();
  const std::uint16_t options = r.u16be();
  const std::uint16_t count = r.u16be();
  const std::uint16_t index_format = r.u16be();
  std::array<std::uint32_t, 7> sizes{};
  for (std::uint32_t& s : sizes) s = r.u32be();
  if (!r.ok() || reserved != 0 || index_format > 1) return fail(Status::Decode, "the WOFF2 glyf header is invalid");
  std::array<ByteView, 7> streams;
  for (std::size_t i = 0; i < 7; ++i) {
    streams[i] = r.bytes(sizes[i]);
    if (!r.ok()) return fail(Status::Decode, "WOFF2 glyf stream {} is truncated", i);
  }
  ByteView overlap;
  if (options & 1) {
    overlap = r.bytes((count + 7u) >> 3);
    if (!r.ok()) return fail(Status::Decode, "the WOFF2 overlap bitmap is truncated");
  }
  util::ByteReader contours(streams[0]), npoints(streams[1]), flags(streams[2]), glyphs(streams[3]),
      composites(streams[4]), bboxes(streams[5]), instructions(streams[6]);
  const std::size_t bitmap_size = ((count + 31u) >> 5) << 2;
  const ByteView bbox_bitmap = bboxes.bytes(bitmap_size);
  if (!bboxes.ok()) return fail(Status::Decode, "the WOFF2 bbox bitmap is truncated");
  Reconstructed out;
  out.index_format = static_cast<std::int16_t>(index_format);
  out.x_min.assign(count, 0);
  util::ByteWriter glyf;
  std::vector<std::uint32_t> offsets;
  offsets.reserve(count + 1u);
  for (std::uint32_t g = 0; g < count; ++g) {
    if ((g & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(g) / count, "Decoding WOFF2 glyphs"));
    offsets.push_back(static_cast<std::uint32_t>(glyf.size()));
    const bool has_bbox = (std::to_integer<unsigned>(bbox_bitmap[g >> 3]) & (0x80u >> (g & 7))) != 0;
    const std::int16_t n = contours.i16be();
    if (!contours.ok()) return fail(Status::Decode, "the WOFF2 contour stream is truncated");
    TtGlyph glyph;
    if (n == 0) {
      if (has_bbox) return fail(Status::Decode, "empty WOFF2 glyph {} has a bounding box", g);
      continue;
    }
    if (n < -1) return fail(Status::Decode, "WOFF2 glyph {} has an invalid contour count", g);
    glyph.empty = false;
    glyph.num_contours = n;
    if (n == -1) {
      if (!has_bbox) return fail(Status::Decode, "composite WOFF2 glyph {} has no bounding box", g);
      bool want_instructions = false;
      const auto size = composite_size(composites.peek_rest(), want_instructions);
      if (!size) return fail(Status::Decode, "composite WOFF2 glyph {} is truncated", g);
      const ByteView component_bytes = composites.bytes(*size);
      glyph.x_min = bboxes.i16be();
      glyph.y_min = bboxes.i16be();
      glyph.x_max = bboxes.i16be();
      glyph.y_max = bboxes.i16be();
      if (!bboxes.ok()) return fail(Status::Decode, "the WOFF2 bbox stream is truncated");
      util::ByteWriter raw;
      raw.i16be(-1).i16be(glyph.x_min).i16be(glyph.y_min).i16be(glyph.x_max).i16be(glyph.y_max);
      raw.bytes(component_bytes);
      if (want_instructions) {
        const auto length = read_255ushort(glyphs);
        if (!length) return fail(Status::Decode, "the WOFF2 glyph stream is truncated");
        const ByteView code = instructions.bytes(*length);
        if (!instructions.ok()) return fail(Status::Decode, "the WOFF2 instruction stream is truncated");
        raw.u16be(*length).bytes(code);
      }
      out.x_min[g] = glyph.x_min;
      glyf.bytes(raw.view());
      glyf.align(4);
      continue;
    }
    std::uint32_t total = 0;
    for (std::int16_t c = 0; c < n; ++c) {
      const auto points = read_255ushort(npoints);
      if (!points) return fail(Status::Decode, "the WOFF2 point count stream is truncated");
      total += *points;
      if (total > 0xFFFF) return fail(Status::Decode, "WOFF2 glyph {} has too many points", g);
      if (total == 0) return fail(Status::Decode, "WOFF2 glyph {} has an empty contour", g);
      glyph.end_points.push_back(static_cast<std::uint16_t>(total - 1));
    }
    if (total > flags.remaining()) return fail(Status::Decode, "the WOFF2 flag stream is truncated");
    glyph.points.resize(total);
    CVT_TRY(decode_triplets(flags, glyphs, glyph.points));
    const auto length = read_255ushort(glyphs);
    if (!length) return fail(Status::Decode, "the WOFF2 glyph stream is truncated");
    const ByteView code = instructions.bytes(*length);
    if (!instructions.ok()) return fail(Status::Decode, "the WOFF2 instruction stream is truncated");
    glyph.instructions.assign(code.begin(), code.end());
    if (!overlap.empty()) glyph.overlap_simple = (std::to_integer<unsigned>(overlap[g >> 3]) & (0x80u >> (g & 7))) != 0;
    bool explicit_box = false;
    if (has_bbox) {
      glyph.x_min = bboxes.i16be();
      glyph.y_min = bboxes.i16be();
      glyph.x_max = bboxes.i16be();
      glyph.y_max = bboxes.i16be();
      if (!bboxes.ok()) return fail(Status::Decode, "the WOFF2 bbox stream is truncated");
      explicit_box = true;
    }
    const Bytes encoded = encode_glyph(glyph, !explicit_box);
    out.x_min[g] = get_i16(encoded, 2);
    glyf.bytes(encoded);
    glyf.align(4);
    if (glyf.size() > 0x7FFFFFFF) return fail(Status::Decode, "the WOFF2 glyf table is too large");
  }
  offsets.push_back(static_cast<std::uint32_t>(glyf.size()));
  if (index_format == 0 && offsets.back() > 0x1FFFE) {
    return fail(Status::Decode, "the WOFF2 glyf table does not fit a short loca table");
  }
  util::ByteWriter loca;
  for (const std::uint32_t offset : offsets) {
    if (index_format == 0) loca.u16be(static_cast<std::uint16_t>(offset / 2));
    else loca.u32be(offset);
  }
  out.glyf = glyf.take();
  out.loca = loca.take();
  return out;
}

Result<Bytes> reconstruct_hmtx(ByteView data, std::uint16_t count, std::uint16_t long_count,
                               const std::vector<std::int16_t>& x_min) {
  util::ByteReader r(data);
  const std::uint8_t flags = r.u8();
  if ((flags & 0xFC) != 0 || (flags & 3) == 0) return fail(Status::Decode, "WOFF2 hmtx transform flags are invalid");
  if (long_count == 0 || long_count > count) return fail(Status::Decode, "hhea.numberOfHMetrics is invalid");
  std::vector<std::uint16_t> advances(long_count);
  for (std::uint16_t& a : advances) a = r.u16be();
  std::vector<std::int16_t> lsbs(count);
  for (std::uint16_t g = 0; g < count; ++g) {
    const bool proportional = g < long_count;
    const bool omitted = proportional ? (flags & 1) : (flags & 2);
    lsbs[g] = omitted ? (g < x_min.size() ? x_min[g] : 0) : r.i16be();
  }
  if (!r.ok()) return fail(Status::Decode, "the WOFF2 hmtx data is truncated");
  util::ByteWriter out;
  for (std::uint16_t g = 0; g < count; ++g) {
    if (g < long_count) out.u16be(advances[g]);
    out.i16be(lsbs[g]);
  }
  return out.take();
}

}  // namespace

Result<ir::Font> read_woff2(ByteView data, std::int64_t face, const Context& ctx) {
  util::ByteReader r(data);
  if (!r.consume("wOF2")) return fail(Status::Decode, "not a WOFF2 file");
  const std::uint32_t flavor = r.u32be();
  const std::uint32_t length = r.u32be();
  const std::uint16_t count = r.u16be();
  const std::uint16_t reserved = r.u16be();
  r.skip(4);  // totalSfntSize
  const std::uint32_t compressed_size = r.u32be();
  r.skip(4);  // version
  const std::uint32_t meta_offset = r.u32be(), meta_length = r.u32be(), meta_orig = r.u32be();
  const std::uint32_t priv_offset = r.u32be(), priv_length = r.u32be();
  if (!r.ok()) return fail(Status::Decode, "the WOFF2 header is truncated");
  if (reserved != 0) return fail(Status::Decode, "the WOFF2 header is invalid (reserved field set)");
  if (length != data.size()) ctx.warn("The WOFF2 length field does not match the file size.");
  if (count == 0 || count > max_tables) return fail(Status::Decode, "the WOFF2 file declares {} tables", count);
  std::vector<Woff2Entry> entries(count);
  std::uint64_t stream_total = 0;
  for (Woff2Entry& e : entries) {
    const std::uint8_t flags = r.u8();
    e.tag = (flags & 63) == 63 ? r.u32be() : tag_value(known_tags[flags & 63]);
    e.version = static_cast<std::uint8_t>(flags >> 6);
    const auto orig = read_base128(r);
    if (!orig || !r.ok()) return fail(Status::Decode, "the WOFF2 table directory is truncated");
    if (!tag_is_valid(e.tag)) return fail(Status::Decode, "a WOFF2 table has an invalid tag");
    e.orig_length = *orig;
    const bool glyf_or_loca = e.tag == tag_value("glyf") || e.tag == tag_value("loca");
    e.transformed = glyf_or_loca ? e.version == 0 : e.version != 0;
    if (glyf_or_loca ? (e.version != 0 && e.version != 3)
                     : (e.version != 0 && !(e.tag == tag_value("hmtx") && e.version == 1))) {
      return fail(Status::Decode, "WOFF2 table '{}' uses unknown transform {}", tag_text(e.tag), e.version);
    }
    e.stream_length = e.orig_length;
    if (e.transformed) {
      const auto transformed = read_base128(r);
      if (!transformed) return fail(Status::Decode, "the WOFF2 table directory is truncated");
      e.stream_length = *transformed;
      if (e.tag == tag_value("loca") && e.stream_length != 0) {
        return fail(Status::Decode, "the transformed WOFF2 loca table must be empty");
      }
    }
    e.stream_offset = stream_total;
    stream_total += e.stream_length;
  }
  std::vector<FaceRef> faces;
  if (flavor == tag_ttcf) {
    const std::uint32_t version = r.u32be();
    const auto fonts = read_255ushort(r);
    if (!fonts || (version != 0x00010000u && version != 0x00020000u) || *fonts == 0) {
      return fail(Status::Decode, "the WOFF2 collection header is invalid");
    }
    for (std::uint16_t f = 0; f < *fonts; ++f) {
      FaceRef ref;
      const auto tables = read_255ushort(r);
      ref.flavor = r.u32be();
      if (!tables || !r.ok()) return fail(Status::Decode, "the WOFF2 collection directory is truncated");
      for (std::uint16_t t = 0; t < *tables; ++t) {
        const auto index = read_255ushort(r);
        if (!index || *index >= count) return fail(Status::Decode, "the WOFF2 collection refers to a missing table");
        ref.tables.push_back(*index);
      }
      faces.push_back(std::move(ref));
    }
  } else {
    FaceRef ref;
    ref.flavor = flavor;
    for (std::uint16_t t = 0; t < count; ++t) ref.tables.push_back(t);
    faces.push_back(std::move(ref));
  }
  const ByteView compressed = r.bytes(compressed_size);
  if (!r.ok()) return fail(Status::Decode, "the WOFF2 font data is truncated");
  CVT_TRY(ctx.limits.check_decompressed(stream_total));
  compression::DecodeOptions options;
  options.max_output = stream_total;
  options.progress = ctx.progress;
  CVT_TRY_ASSIGN(const Bytes stream, compression::decompress(compression::Method::Brotli, compressed, options));
  if (stream.size() != stream_total) return fail(Status::Decode, "the WOFF2 font data has the wrong size");
  const auto stream_of = [&](const Woff2Entry& e) {
    return ByteView(stream).subspan(static_cast<std::size_t>(e.stream_offset), e.stream_length);
  };
  if (face < 0 || static_cast<std::uint64_t>(face) >= faces.size()) {
    return fail(Status::InvalidArgument, "face {} does not exist; the file holds {} face(s)", face, faces.size());
  }
  // Rebuild each face; transformed glyf/loca/hmtx are reconstructed per face
  // (cached by table index so shared tables are decoded once).
  std::map<std::uint16_t, Reconstructed> glyf_cache;
  std::vector<ir::Font> fonts;
  for (const FaceRef& ref : faces) {
    ir::Font font;
    font.sfnt_version = ref.flavor;
    const auto find_entry = [&](std::string_view tag) -> std::optional<std::uint16_t> {
      for (const std::uint16_t i : ref.tables) {
        if (entries[i].tag == tag_value(tag)) return i;
      }
      return std::nullopt;
    };
    const auto glyf_index = find_entry("glyf");
    const auto loca_index = find_entry("loca");
    const Reconstructed* rebuilt = nullptr;
    if (glyf_index && entries[*glyf_index].transformed) {
      if (!loca_index || !entries[*loca_index].transformed) {
        return fail(Status::Decode, "a transformed WOFF2 glyf table needs a transformed loca table");
      }
      auto it = glyf_cache.find(*glyf_index);
      if (it == glyf_cache.end()) {
        CVT_TRY_ASSIGN(Reconstructed built, reconstruct_glyf(stream_of(entries[*glyf_index]), ctx));
        if (built.loca.size() != entries[*loca_index].orig_length) {
          return fail(Status::Decode, "the reconstructed WOFF2 loca table has the wrong size");
        }
        it = glyf_cache.emplace(*glyf_index, std::move(built)).first;
      }
      rebuilt = &it->second;
    } else if (loca_index && entries[*loca_index].transformed) {
      return fail(Status::Decode, "a transformed WOFF2 loca table needs a transformed glyf table");
    }
    for (const std::uint16_t i : ref.tables) {
      const Woff2Entry& e = entries[i];
      ir::FontTable table;
      table.tag = tag_text(e.tag);
      if (font.find(table.tag)) return fail(Status::Decode, "WOFF2 table '{}' appears twice in a face", table.tag);
      if (e.tag == tag_value("glyf") && rebuilt) {
        table.bytes = rebuilt->glyf;
      } else if (e.tag == tag_value("loca") && rebuilt) {
        table.bytes = rebuilt->loca;
      } else if (!e.transformed) {
        const ByteView bytes = stream_of(e);
        table.bytes.assign(bytes.begin(), bytes.end());
      } else {
        table.bytes.clear();  // hmtx: filled below once maxp/hhea are known
      }
      font.tables.push_back(std::move(table));
    }
    if (const auto hmtx_index = find_entry("hmtx"); hmtx_index && entries[*hmtx_index].transformed) {
      if (!rebuilt) return fail(Status::Decode, "a transformed WOFF2 hmtx table needs transformed glyf data");
      const ir::FontTable* hhea = font.find("hhea");
      if (!hhea) return fail(Status::Decode, "the WOFF2 font has hmtx but no hhea table");
      CVT_TRY_ASSIGN(Bytes hmtx, reconstruct_hmtx(stream_of(entries[*hmtx_index]), num_glyphs(font),
                                                  get_u16(hhea->bytes, 34), rebuilt->x_min));
      font.find("hmtx")->bytes = std::move(hmtx);
    }
    if (rebuilt) {
      if (ir::FontTable* head = font.find("head")) put_u16(head->bytes, 50, static_cast<std::uint16_t>(rebuilt->index_format));
    }
    const bool cff = font.find("CFF ") || font.find("CFF2");
    font.flavor = (ref.flavor == tag_otto || cff) ? ir::FontFlavor::CFF : ir::FontFlavor::TrueType;
    decode_names(font);
    fonts.push_back(std::move(font));
  }
  ir::Font primary = std::move(fonts[static_cast<std::size_t>(face)]);
  if (meta_length > 0) {
    if (std::uint64_t{meta_offset} + meta_length > data.size()) return fail(Status::Decode, "the WOFF2 metadata is out of range");
    compression::DecodeOptions meta_options;
    meta_options.max_output = meta_orig;
    auto meta = compression::decompress(compression::Method::Brotli, data.subspan(meta_offset, meta_length), meta_options);
    if (meta && meta->size() == meta_orig) primary.woff_metadata = std::move(*meta);
    else ctx.warn("The WOFF2 metadata block is damaged and was ignored.");
  }
  if (priv_length > 0) {
    if (std::uint64_t{priv_offset} + priv_length > data.size()) return fail(Status::Decode, "the WOFF2 private data is out of range");
    const ByteView priv = data.subspan(priv_offset, priv_length);
    primary.woff_private.assign(priv.begin(), priv.end());
  }
  for (std::size_t i = 0; i < fonts.size(); ++i) {
    if (i != static_cast<std::size_t>(face)) primary.other_faces.push_back(std::move(fonts[i]));
  }
  return primary;
}

// --------------------------------------------------------------------------- encoder

namespace {

void encode_triplet(std::int32_t dx, std::int32_t dy, bool on_curve, util::ByteWriter& flags, util::ByteWriter& data) {
  const int on = on_curve ? 0 : 128;
  const std::int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
  const int x_sign = dx < 0 ? 0 : 1, y_sign = dy < 0 ? 0 : 1;
  const int xy_sign = x_sign + 2 * y_sign;
  if (dx == 0 && ay < 1280) {
    flags.u8(static_cast<std::uint8_t>(on + ((ay & 0xF00) >> 7) + y_sign));
    data.u8(static_cast<std::uint8_t>(ay & 0xFF));
  } else if (dy == 0 && ax < 1280) {
    flags.u8(static_cast<std::uint8_t>(on + 10 + ((ax & 0xF00) >> 7) + x_sign));
    data.u8(static_cast<std::uint8_t>(ax & 0xFF));
  } else if (ax < 65 && ay < 65) {
    flags.u8(static_cast<std::uint8_t>(on + 20 + ((ax - 1) & 0x30) + (((ay - 1) & 0x30) >> 2) + xy_sign));
    data.u8(static_cast<std::uint8_t>((((ax - 1) & 0xF) << 4) | ((ay - 1) & 0xF)));
  } else if (ax < 769 && ay < 769) {
    flags.u8(static_cast<std::uint8_t>(on + 84 + 12 * (((ax - 1) & 0x300) >> 8) + (((ay - 1) & 0x300) >> 6) + xy_sign));
    data.u8(static_cast<std::uint8_t>((ax - 1) & 0xFF)).u8(static_cast<std::uint8_t>((ay - 1) & 0xFF));
  } else if (ax < 4096 && ay < 4096) {
    flags.u8(static_cast<std::uint8_t>(on + 120 + xy_sign));
    data.u8(static_cast<std::uint8_t>(ax >> 4)).u8(static_cast<std::uint8_t>(((ax & 0xF) << 4) | (ay >> 8)));
    data.u8(static_cast<std::uint8_t>(ay & 0xFF));
  } else {
    flags.u8(static_cast<std::uint8_t>(on + 124 + xy_sign));
    data.u8(static_cast<std::uint8_t>(ax >> 8)).u8(static_cast<std::uint8_t>(ax & 0xFF));
    data.u8(static_cast<std::uint8_t>(ay >> 8)).u8(static_cast<std::uint8_t>(ay & 0xFF));
  }
}

struct TransformedGlyf {
  Bytes data;
  std::uint32_t glyf_length = 0;  // size of the glyf table a decoder rebuilds
  std::uint32_t loca_length = 0;
  std::vector<std::int16_t> x_min;
  std::vector<bool> has_contours;
};

std::optional<TransformedGlyf> transform_glyf(const ir::Font& font) {
  auto slices = glyph_slices(font);
  if (!slices) return std::nullopt;
  const auto count = static_cast<std::uint16_t>(slices->size());
  util::ByteWriter contours, npoints, flags, glyphs, composites, bboxes, instructions;
  std::vector<std::uint8_t> bitmap(((count + 31u) >> 5) << 2, 0);
  std::vector<std::uint8_t> overlap((count + 7u) >> 3, 0);
  bool any_overlap = false;
  TransformedGlyf out;
  out.x_min.assign(count, 0);
  out.has_contours.assign(count, false);
  std::uint64_t rebuilt = 0;
  for (std::uint16_t g = 0; g < count; ++g) {
    const ByteView raw = (*slices)[g];
    auto parsed = parse_glyph(raw);
    if (!parsed) return std::nullopt;
    const TtGlyph& glyph = *parsed;
    if (glyph.empty || (glyph.num_contours == 0)) {
      contours.i16be(0);
      continue;
    }
    out.has_contours[g] = true;
    out.x_min[g] = glyph.x_min;
    if (glyph.is_composite()) {
      contours.i16be(-1);
      bool has_instructions = false;
      const auto size = composite_size(raw.subspan(10), has_instructions);
      if (!size) return std::nullopt;
      composites.bytes(raw.subspan(10, *size));
      if (has_instructions) {
        if (glyph.instructions.size() > 0xFFFF) return std::nullopt;
        write_255ushort(glyphs, static_cast<std::uint16_t>(glyph.instructions.size()));
        instructions.bytes(glyph.instructions);
      }
      bitmap[g >> 3] |= static_cast<std::uint8_t>(0x80u >> (g & 7));
      bboxes.i16be(glyph.x_min).i16be(glyph.y_min).i16be(glyph.x_max).i16be(glyph.y_max);
      std::uint64_t size_out = 10 + *size + (has_instructions ? 2 + glyph.instructions.size() : 0);
      rebuilt += (size_out + 3) / 4 * 4;
      continue;
    }
    if (glyph.points.empty()) {
      contours.i16be(0);
      out.has_contours[g] = false;
      continue;
    }
    contours.i16be(glyph.num_contours);
    std::uint32_t previous = 0;
    for (std::size_t c = 0; c < glyph.end_points.size(); ++c) {
      const std::uint32_t end = glyph.end_points[c] + 1u;
      if (end < previous) return std::nullopt;
      write_255ushort(npoints, static_cast<std::uint16_t>(end - previous));
      previous = end;
    }
    std::int32_t x = 0, y = 0;
    for (const TtPoint& p : glyph.points) {
      encode_triplet(p.x - x, p.y - y, p.on_curve, flags, glyphs);
      x = p.x;
      y = p.y;
    }
    write_255ushort(glyphs, static_cast<std::uint16_t>(glyph.instructions.size()));
    instructions.bytes(glyph.instructions);
    if (glyph.overlap_simple) {
      overlap[g >> 3] |= static_cast<std::uint8_t>(0x80u >> (g & 7));
      any_overlap = true;
    }
    // Store the box only when it differs from the one computed from the points.
    TtGlyph computed = glyph;
    const Bytes recomputed = encode_glyph(computed, true);
    const bool same_box = get_i16(recomputed, 2) == glyph.x_min && get_i16(recomputed, 4) == glyph.y_min &&
                          get_i16(recomputed, 6) == glyph.x_max && get_i16(recomputed, 8) == glyph.y_max;
    if (!same_box) {
      bitmap[g >> 3] |= static_cast<std::uint8_t>(0x80u >> (g & 7));
      bboxes.i16be(glyph.x_min).i16be(glyph.y_min).i16be(glyph.x_max).i16be(glyph.y_max);
    }
    rebuilt += (recomputed.size() + 3) / 4 * 4;
  }
  const ir::FontTable* head = font.find("head");
  std::uint16_t index_format = head ? get_u16(head->bytes, 50) : 1;
  if (index_format == 0 && rebuilt > 0x1FFFE) index_format = 1;
  util::ByteWriter result;
  result.u16be(0).u16be(any_overlap ? 1 : 0).u16be(count).u16be(index_format);
  Bytes box_stream;
  {
    const ByteView b = as_bytes(std::span<const std::uint8_t>(bitmap));
    box_stream.assign(b.begin(), b.end());
    append(box_stream, bboxes.view());
  }
  for (const std::size_t size : {contours.size(), npoints.size(), flags.size(), glyphs.size(), composites.size(),
                                 box_stream.size(), instructions.size()}) {
    result.u32be(static_cast<std::uint32_t>(size));
  }
  result.bytes(contours.view()).bytes(npoints.view()).bytes(flags.view()).bytes(glyphs.view());
  result.bytes(composites.view()).bytes(box_stream).bytes(instructions.view());
  if (any_overlap) result.bytes(std::span<const std::uint8_t>(overlap));
  out.data = result.take();
  out.glyf_length = static_cast<std::uint32_t>(rebuilt);
  out.loca_length = static_cast<std::uint32_t>((count + 1u) * (index_format ? 4u : 2u));
  return out;
}

struct UniqueTable {
  std::uint32_t tag = 0;
  Bytes bytes;          // as in the sfnt
  Bytes stream;         // what goes into the compressed stream
  std::uint32_t orig_length = 0;
  std::uint8_t version = 0;
  bool transformed = false;
};

}  // namespace

Result<Bytes> write_woff2(const ir::Font& font, const Woff2Options& options, const Context& ctx) {
  const std::vector<const ir::Font*> faces = all_faces(font);
  std::vector<UniqueTable> tables;
  std::map<std::pair<std::uint32_t, Bytes>, std::uint16_t> seen;
  std::vector<FaceRef> refs;
  // Size of the font a decoder rebuilds: headers, directories and every
  // (reconstructed) table padded to four bytes.
  std::uint64_t sfnt_total = faces.size() > 1 ? 12 + 4ull * faces.size() + 12 : 0;
  for (const ir::Font* face : faces) {
    CVT_TRY_ASSIGN(const Bytes sfnt, write_sfnt(*face));
    CVT_TRY_ASSIGN(std::vector<DirectoryEntry> entries, read_directory(sfnt, 0));
    std::ranges::sort(entries, [](const DirectoryEntry& a, const DirectoryEntry& b) { return a.tag < b.tag; });
    // loca must directly follow glyf.
    const auto loca = std::ranges::find_if(entries, [](const DirectoryEntry& e) { return e.tag == tag_value("loca"); });
    if (loca != entries.end()) {
      const DirectoryEntry moved = *loca;
      entries.erase(loca);
      const auto glyf = std::ranges::find_if(entries, [](const DirectoryEntry& e) { return e.tag == tag_value("glyf"); });
      entries.insert(glyf == entries.end() ? entries.end() : glyf + 1, moved);
    }
    sfnt_total += 12 + 16ull * entries.size();
    FaceRef ref;
    ref.flavor = output_sfnt_version(*face);
    std::optional<TransformedGlyf> transformed;
    const bool has_glyf = face->find("glyf") && face->find("loca");
    if (options.transform_glyf && has_glyf) transformed = transform_glyf(*face);
    bool hmtx_ok = false;
    std::uint8_t hmtx_flags = 0;
    Bytes hmtx_stream;
    if (transformed && options.transform_hmtx) {
      auto metrics = parse_hmtx(*face);
      const ir::FontTable* hhea = face->find("hhea");
      if (metrics && hhea && !metrics->empty()) {
        const std::uint16_t long_count = std::min<std::uint16_t>(get_u16(hhea->bytes, 34), static_cast<std::uint16_t>(metrics->size()));
        bool proportional_match = true, mono_match = true;
        for (std::size_t g = 0; g < metrics->size(); ++g) {
          const bool match = (*metrics)[g].lsb == (transformed->has_contours[g] ? transformed->x_min[g] : 0);
          if (g < long_count) proportional_match &= match;
          else mono_match &= match;
        }
        if (proportional_match) hmtx_flags |= 1;
        if (mono_match && long_count < metrics->size()) hmtx_flags |= 2;
        if (hmtx_flags != 0 && long_count > 0) {
          util::ByteWriter w;
          w.u8(hmtx_flags);
          for (std::uint16_t g = 0; g < long_count; ++g) w.u16be((*metrics)[g].advance);
          if (!(hmtx_flags & 1)) {
            for (std::uint16_t g = 0; g < long_count; ++g) w.i16be((*metrics)[g].lsb);
          }
          if (!(hmtx_flags & 2)) {
            for (std::size_t g = long_count; g < metrics->size(); ++g) w.i16be((*metrics)[g].lsb);
          }
          hmtx_stream = w.take();
          hmtx_ok = true;
        }
      }
    }
    for (const DirectoryEntry& e : entries) {
      const ByteView bytes = ByteView(sfnt).subspan(e.offset, e.length);
      Bytes key_bytes(bytes.begin(), bytes.end());
      if (e.tag == tag_value("head")) put_u32(key_bytes, 8, 0);
      const auto key = std::make_pair(e.tag, key_bytes);
      if (const auto it = seen.find(key); it != seen.end()) {
        ref.tables.push_back(it->second);
        continue;
      }
      UniqueTable t;
      t.tag = e.tag;
      t.bytes.assign(bytes.begin(), bytes.end());
      t.orig_length = e.length;
      t.stream = t.bytes;
      const bool glyf_or_loca = e.tag == tag_value("glyf") || e.tag == tag_value("loca");
      if (glyf_or_loca) {
        if (transformed) {
          t.transformed = true;
          t.version = 0;
          if (e.tag == tag_value("glyf")) {
            t.stream = transformed->data;
            t.orig_length = transformed->glyf_length;
          } else {
            t.stream.clear();
            t.orig_length = transformed->loca_length;
          }
        } else {
          t.version = 3;
        }
      } else if (e.tag == tag_value("hmtx") && hmtx_ok) {
        t.transformed = true;
        t.version = 1;
        t.stream = hmtx_stream;
      } else if (e.tag == tag_value("head") && transformed) {
        // The decoder rebuilds loca in the format the transform names.
        const std::uint16_t format = get_u16(transformed->data, 6);
        put_u16(t.stream, 50, format);
        put_u16(t.bytes, 50, format);
        // Bit 11: the font went through a lossless transform (required by WOFF2).
        put_u16(t.stream, 16, static_cast<std::uint16_t>(get_u16(t.stream, 16) | 0x0800));
      }
      const auto index = static_cast<std::uint16_t>(tables.size());
      if (tables.size() >= 0xFFFF) return fail(Status::Encode, "too many tables for WOFF2");
      seen.emplace(key, index);
      ref.tables.push_back(index);
      tables.push_back(std::move(t));
    }
    refs.push_back(std::move(ref));
  }
  for (const UniqueTable& t : tables) sfnt_total += (t.orig_length + 3ull) / 4 * 4;
  // Stream and compression.
  util::ByteWriter stream;
  for (const UniqueTable& t : tables) stream.bytes(t.stream);
  compression::EncodeOptions encode;
  encode.level = std::clamp(options.level, 0, 9);
  encode.progress = ctx.progress;
  CVT_TRY_ASSIGN(const Bytes compressed, compression::compress(compression::Method::Brotli, stream.view(), encode));
  Bytes meta;
  if (!font.woff_metadata.empty()) {
    CVT_TRY_ASSIGN(meta, compression::compress(compression::Method::Brotli, font.woff_metadata, encode));
  }
  util::ByteWriter directory;
  for (const UniqueTable& t : tables) {
    const std::string text = tag_text(t.tag);
    const auto known = std::ranges::find(known_tags, std::string_view(text));
    const std::uint8_t index = known == known_tags.end() ? 63 : static_cast<std::uint8_t>(known - known_tags.begin());
    directory.u8(static_cast<std::uint8_t>(index | (t.version << 6)));
    if (index == 63) directory.u32be(t.tag);
    write_base128(directory, t.orig_length);
    if (t.transformed) write_base128(directory, static_cast<std::uint32_t>(t.stream.size()));
  }
  const bool collection = faces.size() > 1;
  if (collection) {
    directory.u32be(0x00010000u);
    write_255ushort(directory, static_cast<std::uint16_t>(refs.size()));
    for (const FaceRef& ref : refs) {
      write_255ushort(directory, static_cast<std::uint16_t>(ref.tables.size()));
      directory.u32be(ref.flavor);
      for (const std::uint16_t i : ref.tables) write_255ushort(directory, i);
    }
  }
  const std::uint64_t data_start = 48 + directory.size();
  std::uint64_t end = data_start + compressed.size();
  std::uint64_t meta_offset = 0, priv_offset = 0;
  if (!meta.empty()) {
    end = (end + 3) / 4 * 4;
    meta_offset = end;
    end += meta.size();
  }
  const bool with_private = options.keep_private && !font.woff_private.empty();
  if (with_private) {
    end = (end + 3) / 4 * 4;
    priv_offset = end;
    end += font.woff_private.size();
  }
  end = (end + 3) / 4 * 4;  // decoders expect the file padded to a multiple of four
  if (end > 0xFFFFFFF0u) return fail(Status::Encode, "the font is too large for WOFF2");
  const ir::FontTable* head = font.find("head");
  const std::uint32_t revision = head ? get_u32(head->bytes, 4) : 0x00010000u;
  util::ByteWriter out(static_cast<std::size_t>(end));
  out.text("wOF2").u32be(collection ? tag_ttcf : refs[0].flavor).u32be(static_cast<std::uint32_t>(end));
  out.u16be(static_cast<std::uint16_t>(tables.size())).u16be(0);
  out.u32be(static_cast<std::uint32_t>(std::min<std::uint64_t>(sfnt_total, 0xFFFFFFFFu)));
  out.u32be(static_cast<std::uint32_t>(compressed.size()));
  out.u16be(static_cast<std::uint16_t>(revision >> 16)).u16be(static_cast<std::uint16_t>((revision & 0xFFFF) * 1000 / 65536));
  out.u32be(static_cast<std::uint32_t>(meta_offset)).u32be(static_cast<std::uint32_t>(meta.size()));
  out.u32be(meta.empty() ? 0 : static_cast<std::uint32_t>(font.woff_metadata.size()));
  out.u32be(static_cast<std::uint32_t>(priv_offset)).u32be(with_private ? static_cast<std::uint32_t>(font.woff_private.size()) : 0);
  out.bytes(directory.view()).bytes(compressed);
  if (!meta.empty()) {
    out.align(4);
    out.bytes(meta);
  }
  if (with_private) {
    out.align(4);
    out.bytes(font.woff_private);
  }
  out.align(4);
  return out.take();
}

}  // namespace cvt::font
