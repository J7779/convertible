// Convertible core - vector family: Windows metafiles, device-independent bitmaps.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "families/image/image_api.hpp"
#include "families/vector/metafile_internal.hpp"

namespace cvt::vector::metafile {

namespace {

constexpr std::uint32_t bi_rgb = 0, bi_rle8 = 1, bi_rle4 = 2, bi_bitfields = 3, bi_jpeg = 4, bi_png = 5,
                        bi_alphabitfields = 6;

struct DibHeader {
  std::uint32_t header_size = 0;
  std::int64_t width = 0;
  std::int64_t height = 0;  // signed: negative = top-down
  std::uint32_t bpp = 0;
  std::uint32_t compression = bi_rgb;
  std::uint32_t colors_used = 0;
  std::uint32_t masks[4] = {0, 0, 0, 0};
  bool has_masks = false;
  std::size_t mask_bytes_after_header = 0;  // BITFIELDS masks stored after a 40-byte header
  bool core = false;
};

std::optional<DibHeader> parse_header(ByteView info) {
  util::ByteReader r(info);
  DibHeader h;
  h.header_size = r.u32le();
  if (!r.ok()) return std::nullopt;
  if (h.header_size == 12) {
    h.core = true;
    h.width = r.u16le();
    h.height = r.u16le();
    r.skip(2);
    h.bpp = r.u16le();
    if (!r.ok()) return std::nullopt;
    return h;
  }
  if (h.header_size < 16 || h.header_size > info.size()) return std::nullopt;
  h.width = r.i32le();
  h.height = r.i32le();
  r.skip(2);
  h.bpp = r.u16le();
  if (h.header_size >= 20) h.compression = r.u32le();
  if (h.header_size >= 36) {
    r.skip(12);  // size image, resolution
    h.colors_used = r.u32le();
  }
  if (!r.ok()) return std::nullopt;
  if (h.header_size == 64 && h.compression > bi_bitfields) h.compression = 0xFFFF;  // OS/2 2.x huffman etc.
  if (h.compression == bi_bitfields || h.compression == bi_alphabitfields) {
    h.has_masks = true;
    const std::size_t count = h.compression == bi_alphabitfields ? 4 : 3;
    if (h.header_size >= 52) {
      util::ByteReader m(info.subspan(40));
      for (std::size_t i = 0; i < 3; ++i) h.masks[i] = m.u32le();
      if (h.header_size >= 56) h.masks[3] = m.u32le();
      if (!m.ok()) return std::nullopt;
    } else {
      util::ByteReader m(info.subspan(std::min<std::size_t>(h.header_size, info.size())));
      for (std::size_t i = 0; i < count; ++i) h.masks[i] = m.u32le();
      if (!m.ok()) return std::nullopt;
      h.mask_bytes_after_header = count * 4;
    }
  } else if (h.header_size >= 56 && h.bpp >= 16 && h.compression == bi_rgb) {
    // V4/V5 headers carry masks even for BI_RGB; they are ignored then.
  }
  return h;
}

std::size_t table_entries(const DibHeader& h) {
  if (h.bpp <= 8) {
    const std::uint32_t full = 1u << h.bpp;
    if (h.colors_used == 0 || h.colors_used > full) return full;
    return h.colors_used;
  }
  return std::min<std::uint32_t>(h.colors_used, 256);
}

struct MaskShift {
  std::uint32_t mask = 0;
  int shift = 0;
  int bits = 0;
  [[nodiscard]] std::uint8_t extract(std::uint32_t v) const {
    if (mask == 0) return 255;
    const std::uint32_t x = (v & mask) >> shift;
    if (bits >= 8) return static_cast<std::uint8_t>(x >> (bits - 8));
    const std::uint32_t max = (1u << bits) - 1;
    return static_cast<std::uint8_t>((x * 255 + max / 2) / max);
  }
};

MaskShift make_mask(std::uint32_t mask) {
  MaskShift m;
  m.mask = mask;
  if (mask == 0) return m;
  m.shift = std::countr_zero(mask);
  m.bits = std::popcount(mask >> m.shift);
  return m;
}

Result<void> decode_rle(ByteView bits, int bpp, std::uint32_t width, std::uint32_t height,
                        std::vector<std::uint8_t>& indices) {
  // indices: width*height, row 0 = bottom row (as stored).
  util::ByteReader r(bits);
  std::uint32_t x = 0, y = 0;
  while (!r.at_end() && y < height) {
    const std::uint8_t count = r.u8();
    const std::uint8_t value = r.u8();
    if (!r.ok()) break;
    if (count > 0) {
      for (std::uint32_t i = 0; i < count && x < width; ++i, ++x) {
        indices[static_cast<std::size_t>(y) * width + x] =
            bpp == 8 ? value : static_cast<std::uint8_t>((i & 1) == 0 ? value >> 4 : value & 15);
      }
      continue;
    }
    if (value == 0) {  // end of line
      x = 0;
      ++y;
    } else if (value == 1) {  // end of bitmap
      break;
    } else if (value == 2) {  // delta
      x += r.u8();
      y += r.u8();
    } else {  // absolute run
      const std::uint32_t n = value;
      const std::size_t bytes = bpp == 8 ? n : (n + 1) / 2;
      const ByteView run = r.bytes(bytes);
      if (!r.ok()) break;
      for (std::uint32_t i = 0; i < n; ++i, ++x) {
        const auto b = static_cast<std::uint8_t>(run[bpp == 8 ? i : i / 2]);
        if (x < width) {
          indices[static_cast<std::size_t>(y) * width + x] =
              bpp == 8 ? b : static_cast<std::uint8_t>((i & 1) == 0 ? b >> 4 : b & 15);
        }
      }
      if (bytes & 1) r.skip(1);
    }
  }
  return {};
}

}  // namespace

std::optional<std::pair<std::uint32_t, std::uint32_t>> dib_size(ByteView info) {
  const auto h = parse_header(info);
  if (!h || h->width <= 0 || h->height == 0) return std::nullopt;
  const std::int64_t height = h->height < 0 ? -h->height : h->height;
  if (h->width > 0x7FFFFFFF || height > 0x7FFFFFFF) return std::nullopt;
  return std::pair{static_cast<std::uint32_t>(h->width), static_cast<std::uint32_t>(height)};
}

bool dib_bottom_up(ByteView info) {
  const auto h = parse_header(info);
  return !h || h->height > 0;
}

Result<FramePtr> decode_dib(const DibRequest& request, const Context& ctx, PixelBudget& budget) {
  const auto parsed = parse_header(request.info);
  if (!parsed) return fail(Status::Decode, "malformed bitmap header");
  const DibHeader& h = *parsed;
  if (h.width <= 0 || h.height == 0 || h.width > 0x7FFFFFFF || h.height < -0x7FFFFFFF || h.height > 0x7FFFFFFF) {
    return fail(Status::Decode, "bitmap has no pixels");
  }
  const auto width = static_cast<std::uint32_t>(h.width);
  const bool top_down = h.height < 0;
  const auto height = static_cast<std::uint32_t>(top_down ? -h.height : h.height);
  CVT_TRY(ctx.limits.check_pixels(width, height));
  const std::uint64_t pixels = std::uint64_t{width} * height;
  if (budget.used + pixels > ctx.limits.max_pixels) {
    return fail(Status::Limit, "bitmaps in the metafile exceed the limit of {} pixels", ctx.limits.max_pixels);
  }

  // Colour table.
  const std::size_t entries = table_entries(h);
  const std::size_t entry_size = request.palette_indices ? 2 : (h.core ? 3 : 4);
  const std::size_t table_offset = h.header_size + h.mask_bytes_after_header;
  std::vector<std::array<std::uint8_t, 4>> table;
  {
    if (table_offset > request.info.size()) return fail(Status::Decode, "bitmap colour table is truncated");
    util::ByteReader t(request.info.subspan(table_offset));
    for (std::size_t i = 0; i < entries; ++i) {
      if (request.palette_indices) {
        const std::uint16_t index = t.u16le();
        Color c{0, 0, 0, 1};
        if (request.palette != nullptr && index < request.palette->size()) c = (*request.palette)[index];
        table.push_back({static_cast<std::uint8_t>(c.r * 255.0f + 0.5f), static_cast<std::uint8_t>(c.g * 255.0f + 0.5f),
                         static_cast<std::uint8_t>(c.b * 255.0f + 0.5f), 255});
      } else {
        const std::uint8_t b = t.u8(), g = t.u8(), r = t.u8();
        if (!h.core) t.skip(1);
        table.push_back({r, g, b, 255});
      }
      if (!t.ok()) {
        if (h.bpp <= 8) return fail(Status::Decode, "bitmap colour table is truncated");
        table.pop_back();
        break;
      }
    }
  }
  ByteView bits = request.bits;
  if (bits.empty()) {
    const std::size_t offset = table_offset + entries * entry_size;
    if (offset > request.info.size()) return fail(Status::Decode, "bitmap bits are missing");
    bits = request.info.subspan(offset);
  }

  // Embedded PNG / JPEG.
  if (h.compression == bi_png || h.compression == bi_jpeg) {
    const std::string_view id = h.compression == bi_png ? "png" : "jpg";
    if (!image::can_decode(id, ctx)) return fail(Status::Decode, "no {} decoder is available for a bitmap", id);
    CVT_TRY_ASSIGN(ir::Image image, image::decode(id, bits, ctx));
    if (image.frames.empty() || !image.frames.front().valid()) return fail(Status::Decode, "empty embedded picture");
    budget.used += std::uint64_t{image.frames.front().width} * image.frames.front().height;
    return std::make_shared<const ir::Frame>(std::move(image.frames.front()));
  }

  const std::uint32_t bpp = h.bpp;
  if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
    return fail(Status::Decode, "unsupported bitmap depth {}", bpp);
  }
  const bool rle = h.compression == bi_rle8 || h.compression == bi_rle4;
  if (rle && ((h.compression == bi_rle8 && bpp != 8) || (h.compression == bi_rle4 && bpp != 4) || top_down)) {
    return fail(Status::Decode, "malformed run-length bitmap");
  }
  if (!rle && h.compression != bi_rgb && h.compression != bi_bitfields && h.compression != bi_alphabitfields) {
    return fail(Status::Decode, "unsupported bitmap compression {}", h.compression);
  }
  if (bpp <= 8 && table.empty()) {
    // Monochrome without a table: black and white.
    table.push_back({0, 0, 0, 255});
    table.push_back({255, 255, 255, 255});
  }

  const std::uint64_t stride = ((std::uint64_t{width} * bpp + 31) / 32) * 4;
  if (!rle && stride * height > bits.size()) {
    // Tolerate a missing final row padding, nothing more.
    if (stride * (height - 1) + (std::uint64_t{width} * bpp + 7) / 8 > bits.size()) {
      return fail(Status::Decode, "bitmap bits are truncated");
    }
  }
  budget.used += pixels;
  auto frame = std::make_shared<ir::Frame>(ir::Frame::filled(width, height, {0, 0, 0, 255}));
  const auto put = [&](std::uint32_t x, std::uint32_t stored_row, std::array<std::uint8_t, 4> c) {
    const std::uint32_t y = top_down ? stored_row : height - 1 - stored_row;
    const std::size_t at = frame->offset(x, y);
    std::memcpy(frame->rgba.data() + at, c.data(), 4);
  };
  const auto lookup = [&](std::uint32_t index) -> std::array<std::uint8_t, 4> {
    if (index < table.size()) return table[index];
    return {0, 0, 0, 255};
  };

  if (rle) {
    std::vector<std::uint8_t> indices(static_cast<std::size_t>(pixels), 0);
    CVT_TRY(decode_rle(bits, static_cast<int>(bpp), width, height, indices));
    for (std::uint32_t row = 0; row < height; ++row) {
      for (std::uint32_t x = 0; x < width; ++x) put(x, row, lookup(indices[static_cast<std::size_t>(row) * width + x]));
    }
    return FramePtr(std::move(frame));
  }

  MaskShift ms[4];
  bool use_masks = h.has_masks && (bpp == 16 || bpp == 32);
  if (use_masks) {
    for (int i = 0; i < 4; ++i) ms[i] = make_mask(h.masks[i]);
  } else if (bpp == 16) {
    ms[0] = make_mask(0x7C00);
    ms[1] = make_mask(0x03E0);
    ms[2] = make_mask(0x001F);
    use_masks = true;
  }
  bool any_alpha = false;
  for (std::uint32_t row = 0; row < height; ++row) {
    if ((row & 255) == 0) CVT_TRY(ctx.checkpoint(0.0));
    const std::uint64_t row_start = stride * row;
    util::ByteReader r(bits.subspan(std::min<std::uint64_t>(row_start, bits.size())));
    for (std::uint32_t x = 0; x < width; ++x) {
      std::array<std::uint8_t, 4> c{0, 0, 0, 255};
      switch (bpp) {
        case 1: case 2: case 4: case 8: {
          const std::uint64_t bit = std::uint64_t{x} * bpp;
          const std::uint64_t at = row_start + bit / 8;
          const auto byte = at < bits.size() ? static_cast<std::uint8_t>(bits[static_cast<std::size_t>(at)]) : 0;
          const unsigned shift = 8 - bpp - static_cast<unsigned>(bit % 8);
          c = lookup((byte >> shift) & ((1u << bpp) - 1));
          break;
        }
        case 16: {
          const std::uint32_t v = r.u16le();
          c = {ms[0].extract(v), ms[1].extract(v), ms[2].extract(v), 255};
          if (ms[3].mask != 0 && request.keep_alpha) c[3] = ms[3].extract(v);
          break;
        }
        case 24: {
          const std::uint8_t b = r.u8(), g = r.u8(), rr = r.u8();
          c = {rr, g, b, 255};
          break;
        }
        default: {
          const std::uint32_t v = r.u32le();
          if (use_masks) {
            c = {ms[0].extract(v), ms[1].extract(v), ms[2].extract(v), 255};
            if (ms[3].mask != 0 && request.keep_alpha) c[3] = ms[3].extract(v);
          } else {
            c = {static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v),
                 request.keep_alpha ? static_cast<std::uint8_t>(v >> 24) : std::uint8_t{255}};
          }
          if (c[3] != 0) any_alpha = true;
          break;
        }
      }
      put(x, row, c);
    }
  }
  if (bpp == 32 && request.keep_alpha) {
    if (!any_alpha) {
      // An all-zero alpha channel means "no alpha" in practice.
      for (std::size_t i = 3; i < frame->rgba.size(); i += 4) frame->rgba[i] = 255;
    } else if (request.premultiplied) {
      for (std::size_t i = 0; i < frame->rgba.size(); i += 4) {
        const std::uint32_t a = frame->rgba[i + 3];
        if (a == 0 || a == 255) continue;
        for (std::size_t k = 0; k < 3; ++k) {
          frame->rgba[i + k] = static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (frame->rgba[i + k] * 255 + a / 2) / a));
        }
      }
    }
  }
  return FramePtr(std::move(frame));
}

FramePtr crop_frame(const FramePtr& frame, std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h) {
  if (!frame || !frame->valid()) return nullptr;
  if (w < 0) {
    x += w;
    w = -w;
  }
  if (h < 0) {
    y += h;
    h = -h;
  }
  const std::int64_t fw = frame->width, fh = frame->height;
  const std::int64_t x0 = std::clamp<std::int64_t>(x, 0, fw), y0 = std::clamp<std::int64_t>(y, 0, fh);
  const std::int64_t x1 = std::clamp<std::int64_t>(x + w, 0, fw), y1 = std::clamp<std::int64_t>(y + h, 0, fh);
  if (x1 <= x0 || y1 <= y0) return nullptr;
  if (x0 == 0 && y0 == 0 && x1 == fw && y1 == fh) return frame;
  auto out = std::make_shared<ir::Frame>(
      ir::Frame::filled(static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)));
  for (std::int64_t row = y0; row < y1; ++row) {
    const std::size_t src = frame->offset(static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(row));
    const std::size_t dst = out->offset(0, static_cast<std::uint32_t>(row - y0));
    std::memcpy(out->rgba.data() + dst, frame->rgba.data() + src, static_cast<std::size_t>(x1 - x0) * 4);
  }
  return out;
}

}  // namespace cvt::vector::metafile
