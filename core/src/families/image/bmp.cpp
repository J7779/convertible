// Convertible core - image family: Windows and OS/2 bitmaps (BMP, DIB).
//
// Reader: BITMAPCOREHEADER (OS/2 1.x), OS/2 2.x headers, BITMAPINFOHEADER
// and the V2/V3/V4/V5 extensions; 1, 2, 4, 8, 16, 24 and 32 bits per
// pixel; RLE4, RLE8 and RLE24 (OS/2); BI_BITFIELDS / ALPHABITFIELDS with
// arbitrary masks; embedded JPEG and PNG; bottom-up and top-down rows;
// embedded ICC profiles; resolution. Also used by ICO/CUR for their DIB
// entries (see bmp_codec.hpp).
//
// Writer: 24-bit, 32-bit with alpha (V5 header with masks), and palettised
// 1/4/8-bit (optionally RLE compressed), chosen automatically and losslessly
// unless a bit depth is forced.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/image/bmp_codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_api.hpp"
#include "families/image/image_ops.hpp"
#include "util/bytes.hpp"

namespace cvt::image {

namespace {

struct Mask {
  std::uint32_t mask = 0;
  int shift = 0;
  int bits = 0;
  [[nodiscard]] std::uint8_t extract(std::uint32_t value) const noexcept {
    if (mask == 0) return 0;
    const std::uint32_t v = (value & mask) >> shift;
    return scale_to_8(v, bits);
  }
};

Mask make_mask(std::uint32_t mask) {
  Mask m;
  m.mask = mask;
  if (mask == 0) return m;
  m.shift = std::countr_zero(mask);
  m.bits = std::min(16, std::popcount(mask >> m.shift));
  if (m.bits > 16) m.bits = 16;
  // Masks with gaps are unusual; use the contiguous low run.
  m.mask = static_cast<std::uint32_t>(((std::uint64_t{1} << m.bits) - 1) << m.shift);
  return m;
}

}  // namespace

Result<ir::Image> bmp::decode_dib(ByteView data, std::size_t pixel_offset, const Context& ctx, bool icon_mode) {
  util::ByteReader reader(data);
  const std::uint32_t header_size = reader.u32le();
  if (!reader.ok() || header_size < 12 || header_size > data.size()) return fail(Status::Decode, "BMP header is truncated");
  std::int64_t width = 0;
  std::int64_t height = 0;
  std::uint16_t planes = 1;
  std::uint16_t bpp = 0;
  std::uint32_t compression = 0;
  std::uint32_t colors_used = 0;
  std::int32_t x_ppm = 0;
  std::int32_t y_ppm = 0;
  std::array<std::uint32_t, 4> masks{};
  bool have_masks = false;
  bool core = header_size == 12;
  std::uint32_t profile_offset = 0;
  std::uint32_t profile_size = 0;
  std::uint32_t cs_type = 0;
  if (core) {
    width = reader.u16le();
    height = reader.u16le();
    planes = reader.u16le();
    bpp = reader.u16le();
  } else {
    if (header_size < 16) return fail(Status::Decode, "BMP header is invalid");
    width = reader.i32le();
    height = reader.i32le();
    planes = reader.u16le();
    bpp = reader.u16le();
    if (header_size >= 20) compression = reader.u32le();
    if (header_size >= 24) reader.skip(4);  // image size
    if (header_size >= 28) x_ppm = reader.i32le();
    if (header_size >= 32) y_ppm = reader.i32le();
    if (header_size >= 36) colors_used = reader.u32le();
    if (header_size >= 40) reader.skip(4);  // important colours
    const bool os2v2 = header_size == 64 || header_size == 16;
    if (os2v2 && compression == 3) {
      return fail(Status::Decode, "OS/2 Huffman-compressed bitmaps are not supported");
    }
    if (os2v2 && compression == 4) compression = 0x10004;  // OS/2 RLE24
    if (!os2v2 && header_size >= 52) {
      masks[0] = reader.u32le();
      masks[1] = reader.u32le();
      masks[2] = reader.u32le();
      have_masks = true;
    }
    if (!os2v2 && header_size >= 56) masks[3] = reader.u32le();
    if (!os2v2 && header_size >= 108) {
      cs_type = reader.u32le();
      reader.skip(36 + 12);  // endpoints, gamma
    }
    if (!os2v2 && header_size >= 124) {
      reader.skip(4);  // intent
      profile_offset = reader.u32le();
      profile_size = reader.u32le();
    }
  }
  if (!reader.ok()) return fail(Status::Decode, "BMP header is truncated");
  reader.seek(header_size);
  // BI_BITFIELDS masks may follow a 40-byte header.
  if ((compression == 3 || compression == 6) && !have_masks) {
    masks[0] = reader.u32le();
    masks[1] = reader.u32le();
    masks[2] = reader.u32le();
    if (compression == 6) masks[3] = reader.u32le();
    have_masks = true;
    if (!reader.ok()) return fail(Status::Decode, "BMP bit masks are truncated");
  }
  if (planes != 1 && !icon_mode) {
    // Some writers put garbage here; libraries ignore it.
  }
  const bool top_down = height < 0;
  if (top_down) height = -height;
  if (icon_mode) height /= 2;  // ICO DIBs store colour + AND mask heights
  if (width <= 0 || height <= 0 || width > 0x7FFFFFFF || height > 0x7FFFFFFF) {
    return fail(Status::Decode, "BMP image has invalid dimensions");
  }
  const auto w = static_cast<std::uint32_t>(width);
  const auto h = static_cast<std::uint32_t>(height);
  CVT_TRY(check_size(ctx, w, h));

  ir::Image image;
  if (x_ppm > 0 && y_ppm > 0) {
    image.dpi_x = x_ppm * 0.0254;
    image.dpi_y = y_ppm * 0.0254;
  }
  if (compression == 4 || compression == 5) {
    // Embedded JPEG / PNG: the pixel data is a complete file.
    if (pixel_offset == 0 || pixel_offset > data.size()) return fail(Status::Decode, "BMP pixel data is missing");
    CVT_TRY_ASSIGN(ir::Image inner, decode(compression == 4 ? "jpg" : "png", data.subspan(pixel_offset), ctx));
    inner.dpi_x = image.dpi_x;
    inner.dpi_y = image.dpi_y;
    return inner;
  }
  if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
    return fail(Status::Decode, "BMP files with {} bits per pixel are not supported", bpp);
  }
  // Colour table.
  std::vector<Rgba> palette;
  if (bpp <= 8) {
    std::uint32_t count = colors_used != 0 ? colors_used : (1u << bpp);
    count = std::min<std::uint32_t>(count, 1u << bpp);
    const std::size_t entry = core ? 3 : 4;
    palette.resize(count, Rgba{0, 0, 0, 255});
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint8_t b = reader.u8();
      const std::uint8_t g = reader.u8();
      const std::uint8_t r = reader.u8();
      if (entry == 4) reader.skip(1);
      if (!reader.ok()) {
        palette.resize(i);
        break;
      }
      palette[i] = {r, g, b, 255};
    }
    if (palette.empty()) return fail(Status::Decode, "BMP colour table is missing");
    palette.resize(std::size_t{1} << bpp, Rgba{0, 0, 0, 255});
  }
  if (compression == 0 && (bpp == 16 || bpp == 32) && !have_masks) {
    masks = bpp == 16 ? std::array<std::uint32_t, 4>{0x7C00, 0x03E0, 0x001F, 0}
                      : std::array<std::uint32_t, 4>{0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000};
    // The fourth byte of 32-bit pixels is alpha in icons and in many
    // bitmaps; an all-zero alpha channel is treated as opaque below.
  }
  if (compression == 0 && bpp == 32 && have_masks && masks[0] == 0 && masks[1] == 0 && masks[2] == 0) {
    masks = {0x00FF0000, 0x0000FF00, 0x000000FF, masks[3]};
  }
  const std::array<Mask, 4> m = {make_mask(masks[0]), make_mask(masks[1]), make_mask(masks[2]), make_mask(masks[3])};

  const std::size_t offset = pixel_offset != 0 ? pixel_offset : reader.position();
  if (offset > data.size()) return fail(Status::Decode, "BMP pixel data is missing");
  const ByteView pixels = data.subspan(offset);
  const bool run_length = compression == 1 || compression == 2 || compression == 0x10004;
  if (run_length) {
    CVT_TRY(check_plausible(std::uint64_t{w} * h, pixels.size(), 1024));
  } else {
    // Uncompressed rows: allow a file truncated to a quarter of its size.
    CVT_TRY(check_plausible(std::uint64_t{w} * h * bpp, pixels.size(), 32));
  }
  ir::Frame frame = ir::Frame::filled(w, h, {0, 0, 0, 255});
  const auto put_row = [&](std::uint32_t file_row) -> std::uint8_t* {
    const std::uint32_t y = top_down ? file_row : h - 1 - file_row;
    return frame.rgba.data() + static_cast<std::size_t>(y) * w * 4;
  };
  bool truncated = false;
  if (compression == 1 || compression == 2 || compression == 0x10004) {
    // RLE: pixels not covered stay transparent.
    std::fill(frame.rgba.begin(), frame.rgba.end(), std::uint8_t{0});
    const bool rle4 = compression == 2;
    const bool rle24 = compression == 0x10004;
    if ((rle4 && bpp != 4) || (compression == 1 && bpp != 8) || (rle24 && bpp != 24)) {
      return fail(Status::Decode, "BMP compression does not match its bit depth");
    }
    util::ByteReader rle(pixels);
    std::uint32_t x = 0;
    std::uint32_t row = 0;
    const auto set = [&](std::uint32_t px, Rgba c) {
      if (px >= w || row >= h) return;
      ::cvt::util::copy_bytes(put_row(row) + static_cast<std::size_t>(px) * 4, c.data(), 4);
    };
    while (row < h) {
      if ((row & 63) == 0 && x == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(row) / h));
      const std::uint8_t count = rle.u8();
      const std::uint8_t value = rle.u8();
      if (!rle.ok()) {
        truncated = true;
        break;
      }
      if (count > 0) {
        if (rle24) {
          const std::uint8_t g = rle.u8();
          const std::uint8_t r = rle.u8();
          for (std::uint32_t i = 0; i < count; ++i) set(x++, {r, g, value, 255});
        } else {
          for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t index = rle4 ? ((i & 1) ? (value & 15) : (value >> 4)) : value;
            set(x++, palette[index]);
          }
        }
        continue;
      }
      if (value == 0) {  // end of line
        x = 0;
        ++row;
      } else if (value == 1) {  // end of bitmap
        break;
      } else if (value == 2) {  // delta
        x += rle.u8();
        row += rle.u8();
      } else {  // absolute run
        if (rle24) {
          for (std::uint32_t i = 0; i < value; ++i) {
            const std::uint8_t b = rle.u8();
            const std::uint8_t g = rle.u8();
            const std::uint8_t r = rle.u8();
            set(x++, {r, g, b, 255});
          }
          if ((value * 3) & 1) rle.skip(1);
        } else if (rle4) {
          const std::size_t bytes = (value + 1u) / 2u;
          const ByteView packed = rle.bytes(bytes);
          if (rle.ok()) {
            for (std::uint32_t i = 0; i < value; ++i) {
              const auto b = std::to_integer<std::uint8_t>(packed[i / 2]);
              set(x++, palette[(i & 1) ? (b & 15) : (b >> 4)]);
            }
          }
          if (bytes & 1) rle.skip(1);
        } else {
          for (std::uint32_t i = 0; i < value; ++i) set(x++, palette[rle.u8()]);
          if (value & 1) rle.skip(1);
        }
        if (!rle.ok()) {
          truncated = true;
          break;
        }
      }
    }
  } else if (compression == 0 || compression == 3 || compression == 6) {
    const std::uint64_t stride = ((std::uint64_t{w} * bpp + 31) / 32) * 4;
    const bool use_alpha = m[3].mask != 0;
    bool any_alpha = false;
    for (std::uint32_t r = 0; r < h; ++r) {
      if ((r & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / h));
      const std::uint64_t start = stride * r;
      if (start >= pixels.size()) {
        truncated = true;
        break;
      }
      const auto* src = reinterpret_cast<const std::uint8_t*>(pixels.data()) + start;
      const std::uint64_t available = std::min<std::uint64_t>(stride, pixels.size() - start);
      if (available < (std::uint64_t{w} * bpp + 7) / 8) truncated = true;
      std::uint8_t* dst = put_row(r);
      for (std::uint32_t x = 0; x < w; ++x) {
        const std::uint64_t bit = std::uint64_t{x} * bpp;
        if ((bit + bpp + 7) / 8 > available) break;
        std::uint8_t* p = dst + static_cast<std::size_t>(x) * 4;
        if (bpp <= 8) {
          const std::uint8_t byte = src[bit / 8];
          const std::uint32_t index = (byte >> (8 - bpp - static_cast<int>(bit % 8))) & ((1u << bpp) - 1u);
          ::cvt::util::copy_bytes(p, palette[index].data(), 4);
        } else if (bpp == 24 && compression == 0) {
          p[0] = src[x * 3 + 2];
          p[1] = src[x * 3 + 1];
          p[2] = src[x * 3];
        } else {
          std::uint32_t v = 0;
          const std::size_t byte = bit / 8;
          for (int k = 0; k < bpp / 8; ++k) v |= static_cast<std::uint32_t>(src[byte + static_cast<std::size_t>(k)]) << (8 * k);
          p[0] = m[0].extract(v);
          p[1] = m[1].extract(v);
          p[2] = m[2].extract(v);
          if (use_alpha) {
            p[3] = m[3].extract(v);
            if (p[3] != 0) any_alpha = true;
          }
        }
      }
    }
    // An all-zero alpha channel means "no alpha" (very common in 32-bit files).
    if (use_alpha && !any_alpha) {
      for (std::size_t i = 3; i < frame.rgba.size(); i += 4) frame.rgba[i] = 255;
    }
  } else {
    return fail(Status::Decode, "BMP compression method {} is not supported", compression);
  }
  if (truncated) {
    if (icon_mode) return fail(Status::Decode, "icon bitmap is truncated");
    ctx.warn("The BMP file is truncated; missing rows were left black.");
  }
  // Embedded colour profile (V5, PROFILE_EMBEDDED = 'MBED').
  if (cs_type == 0x4D424544 && profile_size > 0 && profile_offset > 0 && profile_offset <= data.size() &&
      profile_size <= data.size() - profile_offset) {
    image.icc_profile = to_bytes(data.subspan(profile_offset, profile_size));
  }
  image.frames.push_back(std::move(frame));
  image.source_bit_depth = bpp <= 8 ? static_cast<std::uint8_t>(8) : static_cast<std::uint8_t>(8);
  return image;
}

namespace {

Result<ir::Image> read_bmp(ReadContext& ctx) {
  util::ByteReader reader(ctx.input);
  if (!reader.consume("BM")) {
    // Other OS/2 array/icon types are not bitmaps we can show.
    return fail(Status::Decode, "not a BMP file");
  }
  reader.skip(8);
  const std::uint32_t offset = reader.u32le();
  if (!reader.ok()) return fail(Status::Decode, "BMP header is truncated");
  // The DIB starts at byte 14; the pixel offset is relative to the file.
  if (offset != 0 && offset < 14) return fail(Status::Decode, "BMP pixel offset is invalid");
  return bmp::decode_dib(ctx.input.subspan(14), offset == 0 ? 0 : offset - 14, ctx, false);
}

// --------------------------------------------------------------------------- writing

struct Layout {
  int bpp = 24;
  bool alpha = false;
  bool rle = false;
  std::vector<Rgba> palette;
  std::vector<std::uint8_t> indices;
};

/// Length of the encoded run starting at `x`: equal values for RLE8, values
/// alternating between row[x] and row[x + 1] for RLE4.
std::uint32_t run_length(const std::uint8_t* row, std::uint32_t x, std::uint32_t width, bool rle4) {
  std::uint32_t run = 1;
  while (x + run < width && run < 255 && row[x + run] == row[x + (rle4 ? (run & 1) : 0)]) ++run;
  return run;
}

/// One row of RLE8 or RLE4 data (absolute runs for noisy stretches).
void put_rle(util::ByteWriter& out, const std::uint8_t* row, std::uint32_t width, bool rle4) {
  const std::uint32_t worthwhile = rle4 ? 5 : 3;
  std::uint32_t x = 0;
  while (x < width) {
    const std::uint32_t run = run_length(row, x, width, rle4);
    if (run >= worthwhile || width - x < 3) {
      const std::uint8_t value =
          rle4 ? static_cast<std::uint8_t>(row[x] << 4 | (run > 1 ? row[x + 1] : 0)) : row[x];
      out.u8(static_cast<std::uint8_t>(run)).u8(value);
      x += run;
      continue;
    }
    // Absolute stretch, ended where a worthwhile run begins.
    std::uint32_t n = 0;
    while (x + n < width && n < 254) {
      if (n >= 3 && run_length(row, x + n, width, rle4) >= worthwhile) break;
      ++n;
    }
    if (n < 3) {
      // Too short for absolute mode: single encoded pixels.
      for (std::uint32_t i = 0; i < n; ++i) {
        out.u8(1).u8(rle4 ? static_cast<std::uint8_t>(row[x + i] << 4) : row[x + i]);
      }
      x += n;
      continue;
    }
    out.u8(0).u8(static_cast<std::uint8_t>(n));
    std::uint32_t bytes = 0;
    if (rle4) {
      bytes = (n + 1) / 2;
      for (std::uint32_t i = 0; i < bytes; ++i) {
        const std::uint8_t hi = row[x + i * 2];
        const std::uint8_t lo = i * 2 + 1 < n ? row[x + i * 2 + 1] : 0;
        out.u8(static_cast<std::uint8_t>(hi << 4 | lo));
      }
    } else {
      bytes = n;
      for (std::uint32_t i = 0; i < n; ++i) out.u8(row[x + i]);
    }
    if (bytes & 1) out.u8(0);
    x += n;
  }
  out.u8(0).u8(0);
}

Result<Bytes> write_bmp(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "BMP");
  const ir::Frame& original = prepared->frames.front();
  const std::string depth = ctx.options.get_enum("bit_depth", "auto", {"auto", "1", "4", "8", "24", "32"});
  const bool rle = ctx.options.get_bool("rle", false);
  Layout layout;
  ir::Frame frame = original;
  const bool opaque = is_opaque(frame);
  if (depth == "32" || (depth == "auto" && !opaque)) {
    layout.bpp = 32;
    layout.alpha = true;
  } else if (depth == "24") {
    layout.bpp = 24;
  } else {
    // Palettised output: exact when it fits, else quantised with dithering.
    if (!opaque) {
      CVT_TRY_ASSIGN(frame, opaque_frame(frame, ctx, "A palette BMP"));
    }
    const int wanted = depth == "auto" ? 0 : std::stoi(depth);
    const int max_colors = wanted == 0 ? 256 : 1 << wanted;
    if (auto exact = exact_palette(frame, max_colors)) {
      layout.palette = exact->palette;
      layout.indices = std::move(exact->indices);
      const std::size_t n = layout.palette.size();
      layout.bpp = wanted != 0 ? wanted : n <= 2 ? 1 : n <= 16 ? 4 : 8;
    } else if (wanted == 0) {
      layout.bpp = 24;
    } else {
      QuantizeOptions qo;
      qo.max_colors = max_colors;
      qo.dither = resolved_dither(ctx.options);
      qo.keep_alpha = false;
      CVT_TRY_ASSIGN(Quantized q, quantize(frame, qo));
      layout.palette = q.palette;
      layout.indices = std::move(q.indices);
      layout.bpp = wanted;
      ctx.warn("The picture was reduced to a palette for the requested bit depth.");
    }
    if (rle && (layout.bpp == 8 || layout.bpp == 4)) layout.rle = true;
    if (rle && layout.bpp == 1) {
      layout.bpp = 4;
      layout.rle = true;
    }
  }
  const std::uint32_t w = frame.width;
  const std::uint32_t h = frame.height;
  if (w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) return fail(Status::Encode, "the picture is too large for BMP");
  // Pixel data (bottom-up).
  util::ByteWriter pixels;
  const std::uint64_t stride = ((std::uint64_t{w} * static_cast<std::uint64_t>(layout.bpp) + 31) / 32) * 4;
  if (!layout.rle) {
    if (stride * h > 0xFFFFFFFFull) return fail(Status::Encode, "the picture is too large for BMP");
    pixels.reserve(static_cast<std::size_t>(stride * h));
  }
  std::vector<std::uint8_t> row(static_cast<std::size_t>(stride));
  for (std::uint32_t r = 0; r < h; ++r) {
    if ((r & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / h));
    const std::uint32_t y = h - 1 - r;
    if (layout.rle) {
      const std::uint8_t* idx = layout.indices.data() + static_cast<std::size_t>(y) * w;
      put_rle(pixels, idx, w, layout.bpp == 4);
      continue;
    }
    std::fill(row.begin(), row.end(), std::uint8_t{0});
    for (std::uint32_t x = 0; x < w; ++x) {
      if (layout.bpp <= 8) {
        const std::uint8_t index = layout.indices[static_cast<std::size_t>(y) * w + x];
        const std::uint64_t bit = std::uint64_t{x} * static_cast<std::uint64_t>(layout.bpp);
        row[bit / 8] |= static_cast<std::uint8_t>(index << (8 - layout.bpp - static_cast<int>(bit % 8)));
      } else {
        const std::uint8_t* p = frame.rgba.data() + frame.offset(x, y);
        const std::size_t at = static_cast<std::size_t>(x) * (layout.bpp / 8);
        row[at] = p[2];
        row[at + 1] = p[1];
        row[at + 2] = p[0];
        if (layout.bpp == 32) row[at + 3] = p[3];
      }
    }
    pixels.bytes(std::span<const std::uint8_t>(row));
  }
  if (layout.rle) pixels.u8(0).u8(1);  // end of bitmap

  const bool v5 = layout.bpp == 32;
  const std::uint32_t header_size = v5 ? 124 : 40;
  const auto palette_bytes = static_cast<std::uint32_t>(layout.bpp <= 8 ? (1u << layout.bpp) * 4 : 0);
  const std::uint32_t offset = 14 + header_size + palette_bytes;
  const std::uint64_t file_size = std::uint64_t{offset} + pixels.size();
  if (file_size > 0xFFFFFFFFull) return fail(Status::Encode, "the picture is too large for BMP");
  util::ByteWriter out(static_cast<std::size_t>(file_size));
  out.text("BM").u32le(static_cast<std::uint32_t>(file_size)).u32le(0).u32le(offset);
  out.u32le(header_size).i32le(static_cast<std::int32_t>(w)).i32le(static_cast<std::int32_t>(h)).u16le(1);
  out.u16le(static_cast<std::uint16_t>(layout.bpp));
  const std::uint32_t compression = layout.rle ? (layout.bpp == 8 ? 1u : 2u) : (v5 ? 3u : 0u);
  out.u32le(compression).u32le(static_cast<std::uint32_t>(pixels.size()));
  const auto ppm = [](double dpi) { return dpi > 0 ? static_cast<std::int32_t>(std::lround(dpi / 0.0254)) : 2835; };
  out.i32le(ppm(prepared->dpi_x)).i32le(ppm(prepared->dpi_y));
  out.u32le(layout.bpp <= 8 ? static_cast<std::uint32_t>(1u << layout.bpp) : 0u).u32le(0);
  if (v5) {
    out.u32le(0x00FF0000).u32le(0x0000FF00).u32le(0x000000FF).u32le(0xFF000000);
    out.u32le(0x73524742);  // 'sRGB'
    out.zeros(36 + 12);     // endpoints, gamma
    out.u32le(4);           // LCS_GM_IMAGES
    out.u32le(0).u32le(0).u32le(0);
  }
  if (layout.bpp <= 8) {
    for (std::size_t i = 0; i < (std::size_t{1} << layout.bpp); ++i) {
      const Rgba c = i < layout.palette.size() ? layout.palette[i] : Rgba{0, 0, 0, 255};
      out.u8(c[2]).u8(c[1]).u8(c[0]).u8(0);
    }
  }
  out.bytes(pixels.view());
  return out.take();
}

int sniff_bmp(std::span<const std::byte> data, std::string_view /*filename*/) {
  if (data.size() < 26 || as_chars(data.first(2)) != "BM") return 0;
  util::ByteReader r(data.subspan(14));
  const std::uint32_t size = r.u32le();
  if (size == 12 || size == 16 || size == 40 || size == 52 || size == 56 || size == 64 || size == 108 || size == 124) {
    return 90;
  }
  return 0;
}

}  // namespace

void register_bmp(Registry& registry) {
  FormatInfo info;
  info.id = "bmp";
  info.name = "Windows bitmap";
  info.description = "Uncompressed (or RLE) Windows and OS/2 bitmap.";
  info.category = Category::Image;
  info.extensions = {"bmp", "dib", "rle"};
  info.mime = "image/bmp";
  info.popularity = 60;
  info.signatures = {Signature::at(0, "'BM'")};
  info.sniff = sniff_bmp;
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "bmp";
  spec.read = reader(read_bmp);
  spec.write = writer(write_bmp);
  spec.write_options = plain_writer_options();
  spec.write_options.push_back(OptionSpec::choice("bit_depth", "Bit depth", "auto", {"auto", "1", "4", "8", "24", "32"},
                                                  "Bits per pixel. Auto writes 32 bits with transparency, a "
                                                  "palette when the picture has few colours, else 24 bits.")
                                   .advanced());
  spec.write_options.push_back(OptionSpec::boolean("rle", "RLE compression", false,
                                                   "Run-length compress palette pictures (not every program reads it).")
                                   .advanced());
  spec.write_options.push_back(dither_option());
  spec.write_options.push_back(background_option());
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
