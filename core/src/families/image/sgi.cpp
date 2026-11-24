// Convertible core - image family: SGI image (.sgi, .rgb, .rgba, .bw).
// Verbatim and RLE storage, 8 and 16 bits per channel, 1 to 4 channels.
// The writer uses RLE, 8 bits (16 for 16-bit sources).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "util/bytes.hpp"

namespace cvt::image {

namespace {

Result<ir::Image> read_sgi(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (r.u16be() != 474) return fail(Status::Decode, "not an SGI image");
  const std::uint8_t storage = r.u8();
  const std::uint8_t bpc = r.u8();
  const std::uint16_t dimension = r.u16be();
  const std::uint16_t xsize = r.u16be();
  std::uint16_t ysize = r.u16be();
  std::uint16_t zsize = r.u16be();
  if (!r.ok() || ctx.input.size() < 512) return fail(Status::Decode, "SGI header is truncated");
  if (storage > 1 || (bpc != 1 && bpc != 2) || dimension < 1 || dimension > 3) return fail(Status::Decode, "SGI header is invalid");
  if (dimension == 1) ysize = 1;
  if (dimension < 3) zsize = 1;
  if (zsize == 0 || zsize > 4) return fail(Status::Decode, "SGI images with {} channels are not supported", zsize);
  CVT_TRY(check_size(ctx, xsize, ysize));
  CVT_TRY(check_plausible(std::uint64_t{xsize} * ysize, ctx.input.size(), storage ? 128 : 4));
  const auto* data = reinterpret_cast<const std::uint8_t*>(ctx.input.data());
  const std::size_t size = ctx.input.size();
  const std::size_t pixels = std::size_t{xsize} * ysize;
  std::vector<std::uint16_t> channels(pixels * zsize, 0);
  bool damaged = false;
  if (storage == 0) {
    const std::uint64_t needed = 512 + std::uint64_t{pixels} * zsize * bpc;
    if (needed > size) damaged = true;
    for (std::size_t i = 0; i < pixels * zsize; ++i) {
      const std::size_t at = 512 + i * bpc;
      if (at + bpc > size) break;
      channels[i] = bpc == 1 ? data[at] : static_cast<std::uint16_t>(data[at] << 8 | data[at + 1]);
    }
  } else {
    const std::uint64_t tables = std::uint64_t{ysize} * zsize;
    if (512 + tables * 8 > size) return fail(Status::Decode, "SGI offset tables are truncated");
    util::ByteReader t(ctx.input.subspan(512));
    std::vector<std::uint32_t> starts(static_cast<std::size_t>(tables));
    std::vector<std::uint32_t> lengths(static_cast<std::size_t>(tables));
    for (auto& s : starts) s = t.u32be();
    for (auto& l : lengths) l = t.u32be();
    for (std::size_t row = 0; row < tables; ++row) {
      std::size_t at = starts[row];
      const std::size_t end = std::min<std::size_t>(size, std::size_t{starts[row]} + lengths[row]);
      std::uint16_t* out = channels.data() + row * xsize;
      std::size_t x = 0;
      while (x < xsize && at + bpc <= end) {
        const std::uint16_t control = bpc == 1 ? data[at] : static_cast<std::uint16_t>(data[at] << 8 | data[at + 1]);
        at += bpc;
        const std::size_t count = control & 0x7F;
        if (count == 0) break;
        if (control & 0x80) {
          for (std::size_t k = 0; k < count && x < xsize && at + bpc <= end; ++k, at += bpc) {
            out[x++] = bpc == 1 ? data[at] : static_cast<std::uint16_t>(data[at] << 8 | data[at + 1]);
          }
        } else {
          if (at + bpc > end) break;
          const std::uint16_t value = bpc == 1 ? data[at] : static_cast<std::uint16_t>(data[at] << 8 | data[at + 1]);
          at += bpc;
          for (std::size_t k = 0; k < count && x < xsize; ++k) out[x++] = value;
        }
      }
      if (x < xsize) damaged = true;
    }
  }
  if (damaged) ctx.warn("The SGI image data is truncated or damaged.");
  // Channel planes are stored bottom-up.
  const std::uint32_t max = bpc == 1 ? 255 : 65535;
  ir::Frame frame = ir::Frame::filled(xsize, ysize, {0, 0, 0, 255});
  std::vector<std::uint16_t> wide;
  if (bpc == 2) wide.assign(pixels * 4, 65535);
  for (std::uint32_t y = 0; y < ysize; ++y) {
    const std::uint32_t row = ysize - 1 - y;
    for (std::uint32_t x = 0; x < xsize; ++x) {
      std::uint16_t v[4];
      for (std::uint32_t c = 0; c < zsize; ++c) v[c] = channels[(std::size_t{c} * ysize + row) * xsize + x];
      std::uint16_t rgba[4];
      if (zsize <= 2) {
        rgba[0] = rgba[1] = rgba[2] = v[0];
        rgba[3] = zsize == 2 ? v[1] : static_cast<std::uint16_t>(max);
      } else {
        rgba[0] = v[0];
        rgba[1] = v[1];
        rgba[2] = v[2];
        rgba[3] = zsize == 4 ? v[3] : static_cast<std::uint16_t>(max);
      }
      std::uint8_t* d = frame.rgba.data() + frame.offset(x, y);
      for (int c = 0; c < 4; ++c) d[c] = scale_max_to_8(rgba[c], max);
      if (bpc == 2) std::memcpy(wide.data() + (std::size_t{y} * xsize + x) * 4, rgba, sizeof(rgba));
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  if (bpc == 2) image.hdr = linear_from_srgb16(wide, xsize, ysize);
  image.source_bit_depth = static_cast<std::uint8_t>(bpc * 8);
  image.source_grayscale = zsize <= 2;
  return image;
}

Result<Bytes> write_sgi(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "SGI");
  const ir::Image& image = *prepared;
  const ir::Frame& frame = image.frames.front();
  if (frame.width > 65535 || frame.height > 65535) return fail(Status::Encode, "SGI images are limited to 65535x65535 pixels");
  const bool alpha = !is_opaque(frame);
  const bool grey = is_grayscale(frame);
  const std::uint32_t z = (grey ? 1u : 3u) + (alpha ? 1u : 0u);
  const bool sixteen = image.hdr && image.hdr->width == frame.width && image.hdr->height == frame.height && image.source_bit_depth > 8;
  std::vector<std::uint16_t> wide;
  if (sixteen) wide = srgb16_from_linear(*image.hdr);
  const int bpc = sixteen ? 2 : 1;
  const std::uint32_t w = frame.width;
  const std::uint32_t h = frame.height;
  util::ByteWriter out;
  out.u16be(474).u8(1).u8(static_cast<std::uint8_t>(bpc)).u16be(z <= 1 ? 2 : 3).u16be(static_cast<std::uint16_t>(w));
  out.u16be(static_cast<std::uint16_t>(h)).u16be(static_cast<std::uint16_t>(z));
  out.u32be(0).u32be(sixteen ? 65535 : 255).u32be(0);
  out.zeros(80);  // name
  out.u32be(0);   // colormap: normal
  out.zeros(404);
  const std::size_t tables = std::size_t{h} * z;
  const std::size_t table_at = out.size();
  out.zeros(tables * 8);
  std::vector<std::uint32_t> starts(tables);
  std::vector<std::uint32_t> lengths(tables);
  std::vector<std::uint16_t> row(w);
  for (std::uint32_t c = 0; c < z; ++c) {
    for (std::uint32_t r = 0; r < h; ++r) {
      const std::uint32_t y = h - 1 - r;
      const std::size_t channel = grey ? (c == 0 ? 0 : 3) : (c < 3 ? c : 3);
      for (std::uint32_t x = 0; x < w; ++x) {
        const std::size_t at = frame.offset(x, y) + channel;
        row[x] = sixteen ? wide[at] : frame.rgba[at];
      }
      const std::size_t start = out.size();
      const auto put = [&](std::uint16_t v) {
        if (bpc == 1) {
          out.u8(static_cast<std::uint8_t>(v));
        } else {
          out.u16be(v);
        }
      };
      std::uint32_t x = 0;
      while (x < w) {
        std::uint32_t run = 1;
        while (x + run < w && run < 127 && row[x + run] == row[x]) ++run;
        if (run >= 3) {
          put(static_cast<std::uint16_t>(run));
          put(row[x]);
          x += run;
          continue;
        }
        std::uint32_t n = 0;
        while (x + n < w && n < 127 && !(x + n + 2 < w && row[x + n] == row[x + n + 1] && row[x + n] == row[x + n + 2])) ++n;
        put(static_cast<std::uint16_t>(0x80 | n));
        for (std::uint32_t k = 0; k < n; ++k) put(row[x + k]);
        x += n;
      }
      put(0);
      if (out.size() > 0xFFFFFFFFull) return fail(Status::Encode, "the SGI image would exceed 4 GiB");
      starts[std::size_t{c} * h + r] = static_cast<std::uint32_t>(start);
      lengths[std::size_t{c} * h + r] = static_cast<std::uint32_t>(out.size() - start);
    }
  }
  for (std::size_t i = 0; i < tables; ++i) {
    out.patch_u32be(table_at + i * 4, starts[i]);
    out.patch_u32be(table_at + tables * 4 + i * 4, lengths[i]);
  }
  return out.take();
}

}  // namespace

void register_sgi(Registry& registry) {
  FormatInfo info;
  info.id = "sgi";
  info.name = "SGI image";
  info.description = "Silicon Graphics raster image.";
  info.category = Category::Image;
  info.extensions = {"sgi", "rgb", "rgba", "bw", "int", "inta"};
  info.mime = "image/sgi";
  info.popularity = 8;
  info.signatures = {Signature::at(0, "01 DA 00 01"), Signature::at(0, "01 DA 01 01"), Signature::at(0, "01 DA 00 02"),
                     Signature::at(0, "01 DA 01 02")};
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "sgi";
  spec.read = reader(read_sgi);
  spec.write = writer(write_sgi);
  spec.write_options = plain_writer_options();
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
