// Convertible core - image family: ZSoft PCX.
//
// Reader: monochrome, CGA 4-colour, EGA 16-colour (1-bit x 4 planes or
// 4-bit), 256-colour (with the trailing palette), 24-bit (3 planes) and
// 32-bit (4 planes); RLE and the rare uncompressed variant.
// Writer: 8-bit palette for pictures with at most 256 colours, otherwise
// 24-bit (32-bit with alpha), RLE compressed.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "util/bytes.hpp"

namespace cvt::image {

namespace {

// The default 16-colour EGA palette (used when the header palette is empty).
constexpr Rgb ega[16] = {{0, 0, 0},       {0, 0, 170},     {0, 170, 0},     {0, 170, 170},
                         {170, 0, 0},     {170, 0, 170},   {170, 85, 0},    {170, 170, 170},
                         {85, 85, 85},    {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
                         {255, 85, 85},   {255, 85, 255},  {255, 255, 85},  {255, 255, 255}};

Result<ir::Image> read_pcx(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  const std::uint8_t manufacturer = r.u8();
  const std::uint8_t version = r.u8();
  const std::uint8_t encoding = r.u8();
  const std::uint8_t bpp = r.u8();
  const std::uint16_t xmin = r.u16le();
  const std::uint16_t ymin = r.u16le();
  const std::uint16_t xmax = r.u16le();
  const std::uint16_t ymax = r.u16le();
  const std::uint16_t hdpi = r.u16le();
  const std::uint16_t vdpi = r.u16le();
  const ByteView header_palette = r.bytes(48);
  r.skip(1);
  const std::uint8_t planes = r.u8();
  const std::uint16_t bytes_per_line = r.u16le();
  if (!r.ok() || ctx.input.size() < 128) return fail(Status::Decode, "PCX header is truncated");
  if (manufacturer != 0x0A || encoding > 1) return fail(Status::Decode, "not a PCX file");
  if (xmax < xmin || ymax < ymin) return fail(Status::Decode, "PCX image window is invalid");
  const std::uint32_t width = std::uint32_t{xmax} - xmin + 1;
  const std::uint32_t height = std::uint32_t{ymax} - ymin + 1;
  CVT_TRY(check_size(ctx, width, height));
  const bool layout_ok = (bpp == 1 && (planes == 1 || planes == 2 || planes == 3 || planes == 4)) ||
                         (bpp == 2 && planes == 1) || (bpp == 4 && planes == 1) ||
                         (bpp == 8 && (planes == 1 || planes == 3 || planes == 4));
  if (!layout_ok) return fail(Status::Decode, "PCX layout ({} bits x {} planes) is not supported", bpp, planes);
  if (std::uint64_t{bytes_per_line} * 8 < std::uint64_t{width} * bpp) return fail(Status::Decode, "PCX line length is too short");
  const std::uint64_t scan = std::uint64_t{bytes_per_line} * planes;
  CVT_TRY(check_plausible(scan * height, ctx.input.size(), encoding ? 64 : 2));
  // Decode all scan lines.
  std::vector<std::uint8_t> data(static_cast<std::size_t>(scan * height), 0);
  util::ByteReader body(ctx.input.subspan(128));
  bool truncated = false;
  if (encoding == 0) {
    const ByteView raw = body.bytes(std::min<std::size_t>(data.size(), body.remaining()));
    ::cvt::util::copy_bytes(data.data(), raw.data(), raw.size());
    truncated = raw.size() < data.size();
  } else {
    std::size_t at = 0;
    while (at < data.size()) {
      const std::uint8_t b = body.u8();
      if (!body.ok()) {
        truncated = true;
        break;
      }
      if ((b & 0xC0) == 0xC0) {
        const std::size_t count = b & 0x3Fu;
        const std::uint8_t value = body.u8();
        if (!body.ok()) {
          truncated = true;
          break;
        }
        for (std::size_t k = 0; k < count && at < data.size(); ++k) data[at++] = value;
      } else {
        data[at++] = b;
      }
    }
  }
  if (truncated) ctx.warn("The PCX file is truncated; missing rows were left black.");
  // Palettes.
  std::vector<Rgba> palette(256, Rgba{0, 0, 0, 255});
  bool header_empty = true;
  for (std::size_t i = 0; i < 16; ++i) {
    palette[i] = {std::to_integer<std::uint8_t>(header_palette[i * 3]), std::to_integer<std::uint8_t>(header_palette[i * 3 + 1]),
                  std::to_integer<std::uint8_t>(header_palette[i * 3 + 2]), 255};
    if (palette[i][0] || palette[i][1] || palette[i][2]) header_empty = false;
  }
  const std::uint32_t index_bits = bpp * (planes == 1 || bpp == 1 ? planes : 1);
  if (bpp == 8 && planes == 1) {
    if (ctx.input.size() >= 769 && std::to_integer<std::uint8_t>(ctx.input[ctx.input.size() - 769]) == 0x0C) {
      const ByteView p = ctx.input.last(768);
      for (std::size_t i = 0; i < 256; ++i) {
        palette[i] = {std::to_integer<std::uint8_t>(p[i * 3]), std::to_integer<std::uint8_t>(p[i * 3 + 1]),
                      std::to_integer<std::uint8_t>(p[i * 3 + 2]), 255};
      }
    } else {
      for (std::size_t i = 0; i < 256; ++i) palette[i] = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i), 255};
    }
  } else if (index_bits == 1 && planes == 1) {
    // Monochrome: 0 = black, 1 = white; the header palette is not used.
    palette[0] = {0, 0, 0, 255};
    palette[1] = {255, 255, 255, 255};
  } else if (index_bits <= 4 && (header_empty || version == 3)) {
    for (std::size_t i = 0; i < 16; ++i) palette[i] = {ega[i][0], ega[i][1], ega[i][2], 255};
  }
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint8_t* line = data.data() + scan * y;
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t* d = frame.rgba.data() + frame.offset(x, y);
      if (bpp == 8 && planes >= 3) {
        for (std::uint32_t c = 0; c < 3; ++c) d[c] = line[std::size_t{c} * bytes_per_line + x];
        if (planes == 4) d[3] = line[std::size_t{3} * bytes_per_line + x];
        continue;
      }
      std::uint32_t index = 0;
      if (planes == 1) {
        const std::uint64_t bit = std::uint64_t{x} * bpp;
        index = (line[bit / 8] >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1u);
      } else {
        for (std::uint32_t p = 0; p < planes; ++p) {
          const std::uint8_t byte = line[std::size_t{p} * bytes_per_line + x / 8];
          index |= ((byte >> (7 - x % 8)) & 1u) << p;
        }
      }
      ::cvt::util::copy_bytes(d, palette[index].data(), 4);
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  if (hdpi > 0 && vdpi > 0 && hdpi < 10000 && vdpi < 10000) {
    image.dpi_x = hdpi;
    image.dpi_y = vdpi;
  }
  return image;
}

void rle_line(util::ByteWriter& out, const std::uint8_t* data, std::size_t length) {
  std::size_t i = 0;
  while (i < length) {
    std::size_t run = 1;
    while (i + run < length && run < 63 && data[i + run] == data[i]) ++run;
    if (run > 1 || (data[i] & 0xC0) == 0xC0) {
      out.u8(static_cast<std::uint8_t>(0xC0 | run)).u8(data[i]);
    } else {
      out.u8(data[i]);
    }
    i += run;
  }
}

Result<Bytes> write_pcx(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "PCX");
  const ir::Frame& frame = prepared->frames.front();
  if (frame.width > 65535 || frame.height > 65535) return fail(Status::Encode, "PCX pictures are limited to 65535x65535 pixels");
  const bool alpha = !is_opaque(frame);
  std::optional<Quantized> palette;
  if (!alpha) palette = exact_palette(frame, 256);
  const std::uint8_t planes = palette ? 1 : (alpha ? 4 : 3);
  const std::uint16_t bytes_per_line = static_cast<std::uint16_t>((frame.width + 1) & ~1u);
  util::ByteWriter out;
  out.u8(0x0A).u8(5).u8(1).u8(8);
  out.u16le(0).u16le(0).u16le(static_cast<std::uint16_t>(frame.width - 1)).u16le(static_cast<std::uint16_t>(frame.height - 1));
  const auto dpi = [](double v) { return static_cast<std::uint16_t>(v > 0 && v < 65535 ? v + 0.5 : 72); };
  out.u16le(dpi(prepared->dpi_x)).u16le(dpi(prepared->dpi_y));
  out.zeros(48).u8(0).u8(planes).u16le(bytes_per_line).u16le(palette ? 1 : 1);
  out.u16le(0).u16le(0);
  out.zeros(54);
  std::vector<std::uint8_t> line(std::size_t{bytes_per_line} * planes);
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    if ((y & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / frame.height));
    std::fill(line.begin(), line.end(), std::uint8_t{0});
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      if (palette) {
        line[x] = palette->indices[std::size_t{y} * frame.width + x];
      } else {
        const std::uint8_t* p = frame.rgba.data() + frame.offset(x, y);
        for (std::uint32_t c = 0; c < planes; ++c) line[std::size_t{c} * bytes_per_line + x] = p[c];
      }
    }
    rle_line(out, line.data(), line.size());
  }
  if (palette) {
    out.u8(0x0C);
    for (std::size_t i = 0; i < 256; ++i) {
      const Rgba c = i < palette->palette.size() ? palette->palette[i] : Rgba{0, 0, 0, 255};
      out.u8(c[0]).u8(c[1]).u8(c[2]);
    }
  }
  return out.take();
}

int sniff_pcx(std::span<const std::byte> data, std::string_view /*filename*/) {
  if (data.size() < 128) return 0;
  const auto b = [&](std::size_t i) { return std::to_integer<std::uint32_t>(data[i]); };
  if (b(0) != 0x0A || b(1) > 5 || b(2) > 1) return 0;
  const std::uint32_t bpp = b(3);
  if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8) return 0;
  const std::uint32_t planes = b(65);
  if (planes == 0 || planes > 4) return 0;
  return 80;
}

}  // namespace

void register_pcx(Registry& registry) {
  FormatInfo info;
  info.id = "pcx";
  info.name = "PCX image";
  info.description = "ZSoft Paintbrush image.";
  info.category = Category::Image;
  info.extensions = {"pcx", "pcc"};
  info.mime = "image/vnd.zbrush.pcx";
  info.popularity = 15;
  info.sniff = sniff_pcx;
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "pcx";
  spec.read = reader(read_pcx);
  spec.write = writer(write_pcx);
  spec.write_options = plain_writer_options();
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
