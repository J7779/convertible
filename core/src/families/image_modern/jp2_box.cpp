// Convertible core - image_modern family: JP2/JPX boxes and the codestream SIZ
// marker (ISO/IEC 15444-1 Annex A.5.1 and Annex I, 15444-2 Annex M), in-house.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/image_modern/jp2_codec.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern::jp2 {

namespace {

using namespace std::string_view_literals;

constexpr std::string_view jp2_signature{"\x00\x00\x00\x0CjP  \x0D\x0A\x87\x0A", 12};
constexpr std::string_view xmp_uuid{"\xBE\x7A\xCF\xCB\x97\xA9\x42\xE8\x9C\x71\x99\x94\x91\xE3\xAF\xAC", 16};
constexpr std::string_view exif_uuid{"JpgTiffExif->JP2", 16};
/// Superboxes are at most a few levels deep in JP2/JPX; deeper nesting is hostile.
constexpr int max_depth = 6;

struct Box {
  std::string_view type;
  ByteView content;
  bool truncated = false;
};

/// Split `data` into boxes. Returns false when a box header is malformed (the
/// boxes before it are kept). A box running past the end is returned with
/// `truncated` set and ends the list.
bool split_boxes(ByteView data, std::vector<Box>& boxes) {
  util::ByteReader r(data);
  while (!r.at_end()) {
    if (r.remaining() < 8) return false;
    const std::size_t start = r.position();
    std::uint64_t length = r.u32be();
    const std::string_view type = r.chars(4);
    std::size_t header = 8;
    if (length == 1) {
      if (r.remaining() < 8) return false;
      length = r.u64be();
      header = 16;
    } else if (length == 0) {
      length = data.size() - start;
    }
    if (length < header) return false;
    const std::uint64_t available = data.size() - start;
    Box box;
    box.type = type;
    if (length > available) {
      box.content = data.subspan(start + header);
      box.truncated = true;
      boxes.push_back(box);
      return true;
    }
    box.content = data.subspan(start + header, static_cast<std::size_t>(length) - header);
    boxes.push_back(box);
    r.seek(start + static_cast<std::size_t>(length));
  }
  return true;
}

void parse_colr(ByteView content, FileInfo& info) {
  util::ByteReader r(content);
  ColourSpec spec;
  spec.method = r.u8();
  r.skip(2);  // precedence, approximation
  if (!r.ok()) return;
  if (spec.method == 1) {
    spec.enumerated = r.u32be();
    if (!r.ok()) return;
  } else if (spec.method == 2 || spec.method == 3) {
    const ByteView icc = r.rest();
    spec.icc.assign(icc.begin(), icc.end());
  }
  info.colours.push_back(std::move(spec));
}

Result<void> parse_pclr(ByteView content, FileInfo& info) {
  util::ByteReader r(content);
  Palette palette;
  palette.entries = r.u16be();
  const std::uint32_t columns = r.u8();
  if (!r.ok() || palette.entries == 0 || palette.entries > 1024 || columns == 0) {
    return fail(Status::Decode, "the JPEG 2000 palette box is invalid");
  }
  for (std::uint32_t c = 0; c < columns; ++c) {
    const std::uint8_t b = r.u8();
    Palette::Column column;
    column.precision = (b & 0x7Fu) + 1u;
    column.is_signed = (b & 0x80u) != 0;
    if (column.precision > 32) return fail(Status::Decode, "the JPEG 2000 palette has a {}-bit column", column.precision);
    palette.columns.push_back(column);
  }
  palette.values.resize(std::size_t{palette.entries} * columns);
  for (std::uint32_t e = 0; e < palette.entries; ++e) {
    for (std::uint32_t c = 0; c < columns; ++c) {
      const Palette::Column& column = palette.columns[c];
      const std::uint32_t bytes = (column.precision + 7) / 8;
      std::uint64_t raw = 0;
      for (std::uint32_t i = 0; i < bytes; ++i) raw = (raw << 8) | r.u8();
      raw &= (std::uint64_t{1} << column.precision) - 1;
      std::int64_t value = static_cast<std::int64_t>(raw);
      if (column.is_signed && column.precision < 64 && (raw >> (column.precision - 1)) != 0) {
        value -= std::int64_t{1} << column.precision;
      }
      palette.values[std::size_t{e} * columns + c] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
          value, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
    }
  }
  if (!r.ok()) return fail(Status::Decode, "the JPEG 2000 palette box is truncated");
  info.palette = std::move(palette);
  return {};
}

void parse_cmap(ByteView content, FileInfo& info) {
  util::ByteReader r(content);
  std::vector<ComponentMapping> mapping;
  while (r.remaining() >= 4) {
    ComponentMapping m;
    m.component = r.u16be();
    m.type = r.u8();
    m.column = r.u8();
    mapping.push_back(m);
  }
  info.mapping = std::move(mapping);
}

void parse_cdef(ByteView content, FileInfo& info) {
  util::ByteReader r(content);
  const std::uint16_t count = r.u16be();
  std::vector<ChannelDefinition> channels;
  for (std::uint16_t i = 0; i < count && r.remaining() >= 6; ++i) {
    ChannelDefinition c;
    c.channel = r.u16be();
    c.type = r.u16be();
    c.association = r.u16be();
    channels.push_back(c);
  }
  info.channels = std::move(channels);
}

/// Resolution in pixels per metre, converted to dots per inch; 0 when invalid.
struct Resolution {
  double x = 0.0;
  double y = 0.0;
};

Resolution parse_resolution(ByteView content) {
  util::ByteReader r(content);
  const double vn = r.u16be();
  const double vd = r.u16be();
  const double hn = r.u16be();
  const double hd = r.u16be();
  const int ve = r.i8();
  const int he = r.i8();
  if (!r.ok() || vd == 0 || hd == 0 || vn == 0 || hn == 0) return {};
  const double dpi_y = vn / vd * std::pow(10.0, ve) * 0.0254;
  const double dpi_x = hn / hd * std::pow(10.0, he) * 0.0254;
  if (!(dpi_x > 0.01 && dpi_x < 1e6 && dpi_y > 0.01 && dpi_y < 1e6)) return {};
  return {dpi_x, dpi_y};
}

struct Collector {
  FileInfo& info;
  const Context& ctx;
  bool have_colr = false;
  bool have_pclr = false;
  bool have_cmap = false;
  bool have_cdef = false;
  bool have_capture = false;
  bool have_display = false;
  bool fragmented = false;
  int codestreams = 0;
  double display_x = 0.0, display_y = 0.0;
};

Result<void> walk(ByteView data, Collector& c, int depth) {
  if (depth > max_depth) return fail(Status::Decode, "the JPEG 2000 boxes are nested too deeply");
  std::vector<Box> boxes;
  const bool clean = split_boxes(data, boxes);
  if (!clean) c.ctx.warn("Some JPEG 2000 boxes are damaged and were ignored.");
  for (const Box& box : boxes) {
    const std::string_view t = box.type;
    if (t == "jp2h" || t == "res " || t == "jpch" || t == "jplh" || t == "cgrp") {
      CVT_TRY(walk(box.content, c, depth + 1));
    } else if (t == "ihdr") {
      util::ByteReader r(box.content);
      const std::uint32_t height = r.u32be();
      const std::uint32_t width = r.u32be();
      const std::uint16_t nc = r.u16be();
      if (r.ok() && !c.info.has_ihdr) {
        c.info.has_ihdr = true;
        c.info.ihdr_width = width;
        c.info.ihdr_height = height;
        c.info.ihdr_components = nc;
      }
    } else if (t == "colr") {
      parse_colr(box.content, c.info);
      c.have_colr = true;
    } else if (t == "pclr") {
      if (!c.have_pclr) CVT_TRY(parse_pclr(box.content, c.info));
      c.have_pclr = true;
    } else if (t == "cmap") {
      if (!c.have_cmap) parse_cmap(box.content, c.info);
      c.have_cmap = true;
    } else if (t == "cdef") {
      if (!c.have_cdef) parse_cdef(box.content, c.info);
      c.have_cdef = true;
    } else if (t == "resc") {
      if (!c.have_capture) {
        const Resolution res = parse_resolution(box.content);
        if (res.x > 0) {
          c.info.dpi_x = res.x;
          c.info.dpi_y = res.y;
          c.have_capture = true;
        }
      }
    } else if (t == "resd") {
      if (!c.have_display) {
        const Resolution res = parse_resolution(box.content);
        if (res.x > 0) {
          c.display_x = res.x;
          c.display_y = res.y;
          c.have_display = true;
        }
      }
    } else if (t == "jp2c") {
      if (c.codestreams++ == 0) {
        c.info.codestream = box.content;
        if (box.truncated) c.ctx.warn("The JPEG 2000 file is truncated; the picture may be incomplete.");
      }
    } else if (t == "ftbl") {
      c.fragmented = true;
    } else if (t == "uuid" && box.content.size() >= 16) {
      const std::string_view id = as_chars(box.content.first(16));
      const ByteView payload = box.content.subspan(16);
      if (id == xmp_uuid && c.info.xmp.empty()) {
        c.info.xmp.assign(as_chars(payload));
      } else if (id == exif_uuid && c.info.exif.empty()) {
        ByteView tiff = payload;
        if (tiff.size() >= 6 && as_chars(tiff.first(6)) == "Exif\0\0"sv) tiff = tiff.subspan(6);
        c.info.exif.assign(tiff.begin(), tiff.end());
      }
    } else if (t == "xml " && c.info.xmp.empty()) {
      const std::string_view text = as_chars(box.content);
      if (text.find("x:xmpmeta") != std::string_view::npos || text.find("<?xpacket") != std::string_view::npos) {
        c.info.xmp.assign(text);
      }
    }
  }
  return {};
}

void put_box(util::ByteWriter& out, std::string_view type, ByteView content) {
  out.u32be(static_cast<std::uint32_t>(8 + content.size())).text(type).bytes(content);
}

}  // namespace

// --------------------------------------------------------------------------- SIZ

std::uint64_t SizInfo::tiles() const noexcept {
  if (tile_w == 0 || tile_h == 0) return 0;
  const std::uint64_t across = (std::uint64_t{x1} - tile_x0 + tile_w - 1) / tile_w;
  const std::uint64_t down = (std::uint64_t{y1} - tile_y0 + tile_h - 1) / tile_h;
  return across * down;
}

std::uint64_t SizInfo::samples() const noexcept {
  std::uint64_t total = 0;
  for (const SizComponent& c : components) {
    const std::uint64_t w = (std::uint64_t{x1} + c.dx - 1) / c.dx - (std::uint64_t{x0} + c.dx - 1) / c.dx;
    const std::uint64_t h = (std::uint64_t{y1} + c.dy - 1) / c.dy - (std::uint64_t{y0} + c.dy - 1) / c.dy;
    total += w * h;
  }
  return total;
}

Result<SizInfo> parse_siz(ByteView codestream) {
  util::ByteReader r(codestream);
  if (r.u16be() != 0xFF4F || r.u16be() != 0xFF51) return fail(Status::Decode, "not a JPEG 2000 codestream");
  const std::uint16_t length = r.u16be();
  SizInfo s;
  s.rsiz = r.u16be();
  s.x1 = r.u32be();
  s.y1 = r.u32be();
  s.x0 = r.u32be();
  s.y0 = r.u32be();
  s.tile_w = r.u32be();
  s.tile_h = r.u32be();
  s.tile_x0 = r.u32be();
  s.tile_y0 = r.u32be();
  const std::uint16_t count = r.u16be();
  if (!r.ok()) return fail(Status::Decode, "the JPEG 2000 codestream header is truncated");
  if (count == 0 || count > 16384 || length != 38u + 3u * count) {
    return fail(Status::Decode, "the JPEG 2000 codestream header is invalid ({} components)", count);
  }
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::uint8_t ssiz = r.u8();
    SizComponent c;
    c.precision = (ssiz & 0x7Fu) + 1u;
    c.is_signed = (ssiz & 0x80u) != 0;
    c.dx = r.u8();
    c.dy = r.u8();
    if (c.precision > 38 || c.dx == 0 || c.dy == 0) {
      return fail(Status::Decode, "the JPEG 2000 component {} is invalid", i);
    }
    s.components.push_back(c);
  }
  if (!r.ok()) return fail(Status::Decode, "the JPEG 2000 codestream header is truncated");
  if (s.x1 <= s.x0 || s.y1 <= s.y0 || s.tile_w == 0 || s.tile_h == 0 || s.tile_x0 > s.x0 || s.tile_y0 > s.y0 ||
      std::uint64_t{s.tile_x0} + s.tile_w <= s.x0 || std::uint64_t{s.tile_y0} + s.tile_h <= s.y0) {
    return fail(Status::Decode, "the JPEG 2000 image or tile geometry is invalid");
  }
  // Tile indices are 16-bit (Isot), so a codestream has at most 65535 tiles.
  if (s.tiles() > 65535) return fail(Status::Decode, "the JPEG 2000 codestream declares {} tiles", s.tiles());
  return s;
}

// --------------------------------------------------------------------------- files

bool has_jp2_signature(ByteView data) noexcept {
  return data.size() >= jp2_signature.size() && as_chars(data.first(jp2_signature.size())) == jp2_signature;
}

bool has_codestream_signature(ByteView data) noexcept {
  return data.size() >= 4 && as_chars(data.first(4)) == "\xFF\x4F\xFF\x51"sv;
}

std::string file_brand(ByteView data) {
  if (!has_jp2_signature(data) || data.size() < 24) return {};
  if (as_chars(data.subspan(16, 4)) != "ftyp") return {};
  return std::string(as_chars(data.subspan(20, 4)));
}

Result<FileInfo> parse_file(ByteView data, const Context& ctx) {
  if (!has_jp2_signature(data)) return fail(Status::Decode, "not a JP2 file (signature box missing)");
  FileInfo info;
  Collector c{info, ctx};
  std::vector<Box> top;
  split_boxes(data, top);
  for (const Box& box : top) {
    if (box.type == "ftyp") {
      util::ByteReader r(box.content);
      info.brand = std::string(r.chars(4));
      r.skip(4);
      while (r.remaining() >= 4) info.compatible.emplace_back(r.chars(4));
      break;
    }
  }
  if (info.brand == "jpm ") return fail(Status::Decode, "JPEG 2000 compound documents (JPM) are not supported");
  if (info.brand == "mjp2" || info.brand == "mj2s") {
    return fail(Status::Decode, "Motion JPEG 2000 files are videos, not single pictures");
  }
  CVT_TRY(walk(data, c, 0));
  if (c.codestreams == 0) {
    if (c.fragmented) return fail(Status::Decode, "fragmented JPX codestreams (fragment tables) are not supported");
    return fail(Status::Decode, "the JPEG 2000 file contains no codestream");
  }
  if (c.codestreams > 1) ctx.warn("The JPX file holds several codestreams; only the first was decoded.");
  if (!c.have_capture && c.have_display) {
    info.dpi_x = c.display_x;
    info.dpi_y = c.display_y;
  }
  return info;
}

Bytes build_file(const FileSpec& spec, ByteView codestream) {
  util::ByteWriter out(codestream.size() + 512 + spec.icc.size() + spec.exif.size() + spec.xmp.size());
  out.text(jp2_signature);
  {
    util::ByteWriter ftyp;
    if (spec.jpx) {
      ftyp.text("jpx ").u32be(0).text("jpx ").text("jp2 ").text("jpxb");
    } else {
      ftyp.text("jp2 ").u32be(0).text("jp2 ");
    }
    put_box(out, "ftyp", ftyp.view());
  }
  if (spec.jpx) {
    // Reader requirements: one-byte masks; feature 5 (unrestricted JPEG 2000
    // Part 1 codestream) is needed to understand and to display the file.
    util::ByteWriter rreq;
    rreq.u8(1).u8(0x80).u8(0x80).u16be(1).u16be(5).u8(0x80).u16be(0);
    put_box(out, "rreq", rreq.view());
  }

  util::ByteWriter header;
  {
    util::ByteWriter ihdr;
    bool uniform = true;
    for (const SizComponent& c : spec.components) {
      uniform = uniform && c.precision == spec.components.front().precision &&
                c.is_signed == spec.components.front().is_signed;
    }
    const auto bpc = [](const SizComponent& c) {
      return static_cast<std::uint8_t>((c.precision - 1) | (c.is_signed ? 0x80u : 0u));
    };
    ihdr.u32be(spec.height).u32be(spec.width).u16be(static_cast<std::uint16_t>(spec.components.size()));
    ihdr.u8(uniform ? bpc(spec.components.front()) : std::uint8_t{255}).u8(7).u8(0).u8(0);
    put_box(header, "ihdr", ihdr.view());
    if (!uniform) {
      util::ByteWriter bpcc;
      for (const SizComponent& c : spec.components) bpcc.u8(bpc(c));
      put_box(header, "bpcc", bpcc.view());
    }
  }
  {
    util::ByteWriter colr;
    if (!spec.icc.empty()) {
      colr.u8(2).u8(0).u8(0).bytes(spec.icc);
    } else {
      colr.u8(1).u8(0).u8(0).u32be(spec.enumerated);
    }
    put_box(header, "colr", colr.view());
  }
  if (spec.alpha_last && spec.components.size() >= 2) {
    util::ByteWriter cdef;
    const auto n = static_cast<std::uint16_t>(spec.components.size());
    cdef.u16be(n);
    for (std::uint16_t i = 0; i + 1 < n; ++i) cdef.u16be(i).u16be(0).u16be(static_cast<std::uint16_t>(i + 1));
    cdef.u16be(static_cast<std::uint16_t>(n - 1)).u16be(1).u16be(0);
    put_box(header, "cdef", cdef.view());
  }
  if (spec.dpi_x > 0 && spec.dpi_y > 0) {
    // Pixels per metre = N / 254 * 10^E, i.e. N = dpi * 10^(4 - E): exact for
    // dpi values with up to two decimals below 655 dpi.
    const auto encode = [](double dpi, std::uint16_t& n, std::int8_t& e) {
      for (int exponent = 2; exponent <= 8; ++exponent) {
        const double value = std::round(dpi * std::pow(10.0, 4 - exponent));
        if (value <= 65535.0) {
          n = static_cast<std::uint16_t>(std::max(1.0, value));
          e = static_cast<std::int8_t>(exponent);
          return;
        }
      }
      n = 65535;
      e = 8;
    };
    std::uint16_t vn = 0, hn = 0;
    std::int8_t ve = 0, he = 0;
    encode(spec.dpi_y, vn, ve);
    encode(spec.dpi_x, hn, he);
    util::ByteWriter resc;
    resc.u16be(vn).u16be(254).u16be(hn).u16be(254).i8(ve).i8(he);
    util::ByteWriter res;
    put_box(res, "resc", resc.view());
    put_box(header, "res ", res.view());
  }
  put_box(out, "jp2h", header.view());

  if (!spec.exif.empty()) {
    util::ByteWriter uuid;
    uuid.text(exif_uuid).bytes(spec.exif);
    put_box(out, "uuid", uuid.view());
  }
  if (!spec.xmp.empty()) {
    util::ByteWriter uuid;
    uuid.text(xmp_uuid).text(spec.xmp);
    put_box(out, "uuid", uuid.view());
  }
  if (std::uint64_t{codestream.size()} + 8 > 0xFFFFFFFFull) {
    out.u32be(1).text("jp2c").u64be(std::uint64_t{codestream.size()} + 16).bytes(codestream);
  } else {
    put_box(out, "jp2c", codestream);
  }
  return out.take();
}

}  // namespace cvt::image_modern::jp2
