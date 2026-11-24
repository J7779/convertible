// Convertible core - image family: Truevision TGA.
//
// Reader: colour-mapped (1, 9), true-colour (2, 10) and greyscale (3, 11)
// images, RLE or not; 8/15/16/24/32-bit pixels and 15/16/24/32-bit colour
// maps; every origin (left/right, top/bottom); the TGA 2.0 extension area's
// alpha type. Writer: 24-bit or 32-bit with alpha, grey for grey pictures,
// RLE by default, top-left origin.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "util/bytes.hpp"

namespace cvt::image {

namespace {

constexpr std::string_view footer_signature("TRUEVISION-XFILE.\0", 18);

Rgba colour_from(std::uint32_t v, int bits) {
  switch (bits) {
    case 15:
    case 16:
      return {scale_to_8((v >> 10) & 31, 5), scale_to_8((v >> 5) & 31, 5), scale_to_8(v & 31, 5),
              static_cast<std::uint8_t>(bits == 16 ? ((v & 0x8000) ? 255 : 0) : 255)};
    case 24: return {static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v), 255};
    case 32:
      return {static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v),
              static_cast<std::uint8_t>(v >> 24)};
    default: return {0, 0, 0, 255};
  }
}

std::uint32_t read_le(const std::uint8_t* p, int bytes) {
  std::uint32_t v = 0;
  for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
  return v;
}

Result<ir::Image> read_tga(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  const std::uint8_t id_length = r.u8();
  const std::uint8_t map_type = r.u8();
  const std::uint8_t type = r.u8();
  const std::uint16_t map_first = r.u16le();
  const std::uint16_t map_length = r.u16le();
  const std::uint8_t map_bits = r.u8();
  r.skip(4);  // origin
  const std::uint16_t width = r.u16le();
  const std::uint16_t height = r.u16le();
  const std::uint8_t bpp = r.u8();
  const std::uint8_t descriptor = r.u8();
  if (!r.ok()) return fail(Status::Decode, "TGA header is truncated");
  const int base = type & 7;
  const bool rle = (type & 8) != 0;
  if ((base != 1 && base != 2 && base != 3) || type > 11) return fail(Status::Decode, "unsupported TGA image type {}", type);
  if (map_type > 1) return fail(Status::Decode, "TGA colour map type is invalid");
  if (base == 1 && (bpp != 8 && bpp != 16)) return fail(Status::Decode, "TGA colour-mapped images need 8 or 16 bits");
  if (base == 2 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) return fail(Status::Decode, "TGA pixel size {} is not supported", bpp);
  if (base == 3 && bpp != 8 && bpp != 16) return fail(Status::Decode, "TGA greyscale pixel size {} is not supported", bpp);
  CVT_TRY(check_size(ctx, width, height));
  r.skip(id_length);
  std::vector<Rgba> palette;
  if (map_type == 1) {
    if (map_bits != 15 && map_bits != 16 && map_bits != 24 && map_bits != 32) {
      return fail(Status::Decode, "TGA colour map entry size is invalid");
    }
    const int entry = (map_bits + 7) / 8;
    palette.resize(std::size_t{map_first} + map_length, Rgba{0, 0, 0, 255});
    for (std::uint32_t i = 0; i < map_length; ++i) {
      const ByteView e = r.bytes(static_cast<std::size_t>(entry));
      if (!r.ok()) return fail(Status::Decode, "TGA colour map is truncated");
      palette[map_first + i] = colour_from(read_le(as_u8(e).data(), entry), map_bits);
    }
  }
  // TGA 2.0 footer: alpha type 3 = straight alpha, 4 = premultiplied, 0-2 = ignore.
  int alpha_type = -1;
  if (ctx.input.size() >= 26 + 18 && as_chars(ctx.input.last(18)) == footer_signature) {
    util::ByteReader f(ctx.input.last(26));
    const std::uint32_t extension = f.u32le();
    if (extension != 0 && extension + 495 <= ctx.input.size()) alpha_type = std::to_integer<int>(ctx.input[extension + 494]);
  }
  const int pixel_bytes = (bpp + 7) / 8;
  const std::uint64_t pixels = std::uint64_t{width} * height;
  CVT_TRY(check_plausible(pixels, r.remaining(), rle ? 128 : 4));
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(pixels) * static_cast<std::size_t>(pixel_bytes), 0);
  bool truncated = false;
  if (!rle) {
    const ByteView data = r.bytes(std::min<std::size_t>(raw.size(), r.remaining()));
    std::memcpy(raw.data(), data.data(), data.size());
    truncated = data.size() < raw.size();
  } else {
    std::size_t at = 0;
    while (at < raw.size()) {
      const std::uint8_t header = r.u8();
      if (!r.ok()) {
        truncated = true;
        break;
      }
      const std::size_t count = std::size_t{header & 0x7Fu} + 1;
      if (header & 0x80) {
        const ByteView value = r.bytes(static_cast<std::size_t>(pixel_bytes));
        if (!r.ok()) {
          truncated = true;
          break;
        }
        for (std::size_t k = 0; k < count && at < raw.size(); ++k, at += static_cast<std::size_t>(pixel_bytes)) {
          std::memcpy(raw.data() + at, value.data(), static_cast<std::size_t>(pixel_bytes));
        }
      } else {
        const std::size_t n = std::min(count * static_cast<std::size_t>(pixel_bytes), raw.size() - at);
        const ByteView values = r.bytes(n);
        if (!r.ok()) {
          truncated = true;
          break;
        }
        std::memcpy(raw.data() + at, values.data(), n);
        at += n;
      }
    }
  }
  if (truncated) ctx.warn("The TGA file is truncated; missing pixels were left black.");
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  const bool right_to_left = (descriptor & 0x10) != 0;
  const bool top_down = (descriptor & 0x20) != 0;
  const int alpha_bits = descriptor & 15;
  const bool use_alpha = alpha_type < 0 ? (bpp == 32 && alpha_bits > 0) || bpp == 16 : alpha_type >= 3;
  bool any_alpha = false;
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::uint32_t y = top_down ? row : height - 1u - row;
    for (std::uint32_t col = 0; col < width; ++col) {
      const std::uint32_t x = right_to_left ? width - 1u - col : col;
      const std::uint8_t* p = raw.data() + (std::size_t{row} * width + col) * static_cast<std::size_t>(pixel_bytes);
      const std::uint32_t v = read_le(p, pixel_bytes);
      Rgba c{};
      if (base == 1) {
        c = v < palette.size() ? palette[v] : Rgba{0, 0, 0, 255};
      } else if (base == 3) {
        const auto g = static_cast<std::uint8_t>(bpp == 16 ? (v & 0xFF) : v);
        c = {g, g, g, static_cast<std::uint8_t>(bpp == 16 ? (v >> 8) : 255)};
      } else {
        c = colour_from(v, bpp == 15 ? 15 : bpp);
      }
      if (!use_alpha && base != 1) c[3] = 255;
      if (c[3] != 0) any_alpha = true;
      std::memcpy(frame.rgba.data() + frame.offset(x, y), c.data(), 4);
    }
  }
  // An all-zero alpha channel without an extension area is garbage, not transparency.
  if (!any_alpha && alpha_type < 0) {
    for (std::size_t i = 3; i < frame.rgba.size(); i += 4) frame.rgba[i] = 255;
  }
  if (alpha_type == 4) unpremultiply(frame);
  ir::Image image;
  image.frames.push_back(std::move(frame));
  image.source_grayscale = base == 3;
  return image;
}

Result<Bytes> write_tga(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "TGA");
  const ir::Frame& frame = prepared->frames.front();
  if (frame.width > 65535 || frame.height > 65535) return fail(Status::Encode, "TGA pictures are limited to 65535x65535 pixels");
  const bool rle = ctx.options.get_bool("rle", true);
  const bool alpha = !is_opaque(frame);
  const bool grey = !alpha && is_grayscale(frame);
  const int bytes = grey ? 1 : alpha ? 4 : 3;
  util::ByteWriter out;
  out.u8(0).u8(0).u8(static_cast<std::uint8_t>((grey ? 3 : 2) | (rle ? 8 : 0)));
  out.u16le(0).u16le(0).u8(0);
  out.u16le(0).u16le(0).u16le(static_cast<std::uint16_t>(frame.width)).u16le(static_cast<std::uint16_t>(frame.height));
  out.u8(static_cast<std::uint8_t>(bytes * 8)).u8(static_cast<std::uint8_t>(0x20 | (alpha ? 8 : 0)));  // top-left origin
  const auto pixel = [&](std::uint32_t x, std::uint32_t y, std::uint8_t* dst) {
    const std::uint8_t* p = frame.rgba.data() + frame.offset(x, y);
    if (grey) {
      dst[0] = p[0];
    } else {
      dst[0] = p[2];
      dst[1] = p[1];
      dst[2] = p[0];
      if (alpha) dst[3] = p[3];
    }
  };
  std::vector<std::uint8_t> row(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(bytes));
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    if ((y & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / frame.height));
    for (std::uint32_t x = 0; x < frame.width; ++x) pixel(x, y, row.data() + std::size_t{x} * static_cast<std::size_t>(bytes));
    if (!rle) {
      out.bytes(std::span<const std::uint8_t>(row));
      continue;
    }
    // Packets never cross rows (TGA 2.0 recommendation).
    std::uint32_t x = 0;
    const auto same = [&](std::uint32_t a, std::uint32_t b) {
      return std::memcmp(row.data() + std::size_t{a} * static_cast<std::size_t>(bytes),
                         row.data() + std::size_t{b} * static_cast<std::size_t>(bytes), static_cast<std::size_t>(bytes)) == 0;
    };
    while (x < frame.width) {
      std::uint32_t run = 1;
      while (x + run < frame.width && run < 128 && same(x, x + run)) ++run;
      if (run >= 2) {
        out.u8(static_cast<std::uint8_t>(0x80 | (run - 1)));
        out.bytes(std::span<const std::uint8_t>(row).subspan(std::size_t{x} * static_cast<std::size_t>(bytes), static_cast<std::size_t>(bytes)));
        x += run;
        continue;
      }
      std::uint32_t n = 1;
      while (x + n < frame.width && n < 128 && !(x + n + 1 < frame.width && same(x + n, x + n + 1))) ++n;
      out.u8(static_cast<std::uint8_t>(n - 1));
      out.bytes(std::span<const std::uint8_t>(row).subspan(std::size_t{x} * static_cast<std::size_t>(bytes),
                                                           std::size_t{n} * static_cast<std::size_t>(bytes)));
      x += n;
    }
  }
  // TGA 2.0 footer without extension area.
  out.u32le(0).u32le(0).text(footer_signature);
  return out.take();
}

int sniff_tga(std::span<const std::byte> data, std::string_view filename) {
  if (data.size() >= 44 && as_chars(data.last(18)) == footer_signature) return 92;
  if (data.size() < 18) return 0;
  const auto b = [&](std::size_t i) { return std::to_integer<std::uint32_t>(data[i]); };
  const std::uint32_t type = b(2);
  const std::uint32_t bpp = b(16);
  const bool plausible = b(1) <= 1 && (type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11) &&
                         (bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32) && (b(12) | b(13) << 8) != 0 &&
                         (b(14) | b(15) << 8) != 0 && (b(17) & 0xC0) == 0;
  if (!plausible) return 0;
  const bool named = filename.ends_with(".tga") || filename.ends_with(".TGA") || filename.ends_with(".tpic") ||
                     filename.ends_with(".icb") || filename.ends_with(".vda") || filename.ends_with(".vst");
  return named ? 85 : 20;
}

}  // namespace

void register_tga(Registry& registry) {
  FormatInfo info;
  info.id = "tga";
  info.name = "Truevision TGA image";
  info.description = "Targa bitmap used by games and video tools.";
  info.category = Category::Image;
  info.extensions = {"tga", "icb", "vda", "vst", "tpic"};
  info.mime = "image/x-tga";
  info.popularity = 30;
  info.sniff = sniff_tga;
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "tga";
  spec.read = reader(read_tga);
  spec.write = writer(write_tga);
  spec.write_options = plain_writer_options();
  spec.write_options.push_back(OptionSpec::boolean("rle", "RLE compression", true,
                                                   "Run-length compress the pixels (smaller, supported by all common readers)."));
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
