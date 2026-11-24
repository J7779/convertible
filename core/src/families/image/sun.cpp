// Convertible core - image family: Sun raster (.ras, .sun).
// 1, 8, 24 and 32-bit images, standard, RGB-ordered and byte-encoded (RLE)
// types, with or without a colour map. The writer stores 24-bit (32 with
// alpha) or an 8-bit colour map for pictures with few colours, RLE encoded.
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

Result<ir::Image> read_sun(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (r.u32be() != 0x59A66A95u) return fail(Status::Decode, "not a Sun raster file");
  const std::uint32_t width = r.u32be();
  const std::uint32_t height = r.u32be();
  const std::uint32_t depth = r.u32be();
  r.skip(4);  // length
  const std::uint32_t type = r.u32be();
  const std::uint32_t map_type = r.u32be();
  const std::uint32_t map_length = r.u32be();
  if (!r.ok()) return fail(Status::Decode, "Sun raster header is truncated");
  if (depth != 1 && depth != 8 && depth != 24 && depth != 32) return fail(Status::Decode, "Sun raster depth {} is not supported", depth);
  if (type > 3 || map_type > 1) return fail(Status::Decode, "Sun raster type {} is not supported", type);
  CVT_TRY(check_size(ctx, width, height));
  std::vector<Rgba> palette;
  if (map_type == 1 && map_length > 0) {
    if (map_length % 3 != 0 || map_length > 768) return fail(Status::Decode, "Sun raster colour map is invalid");
    const std::uint32_t n = map_length / 3;
    const ByteView map = r.bytes(map_length);
    if (!r.ok()) return fail(Status::Decode, "Sun raster colour map is truncated");
    palette.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      palette[i] = {std::to_integer<std::uint8_t>(map[i]), std::to_integer<std::uint8_t>(map[n + i]),
                    std::to_integer<std::uint8_t>(map[2 * n + i]), 255};
    }
  } else {
    r.skip(map_length);
  }
  const std::uint64_t stride = ((std::uint64_t{width} * depth + 15) / 16) * 2;
  CVT_TRY(check_plausible(stride * height, r.remaining(), type == 2 ? 128 : 4));
  std::vector<std::uint8_t> data(static_cast<std::size_t>(stride * height), 0);
  bool truncated = false;
  if (type == 2) {
    std::size_t at = 0;
    while (at < data.size()) {
      const std::uint8_t b = r.u8();
      if (!r.ok()) {
        truncated = true;
        break;
      }
      if (b != 0x80) {
        data[at++] = b;
        continue;
      }
      const std::uint8_t count = r.u8();
      if (count == 0) {
        data[at++] = 0x80;
      } else {
        const std::uint8_t value = r.u8();
        for (std::size_t k = 0; k <= count && at < data.size(); ++k) data[at++] = value;
      }
      if (!r.ok()) {
        truncated = true;
        break;
      }
    }
  } else {
    const ByteView raw = r.bytes(std::min<std::size_t>(data.size(), r.remaining()));
    std::memcpy(data.data(), raw.data(), raw.size());
    truncated = raw.size() < data.size();
  }
  if (truncated) ctx.warn("The Sun raster file is truncated; missing rows were left black.");
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  const bool rgb_order = type == 3;
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint8_t* row = data.data() + stride * y;
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t* d = frame.rgba.data() + frame.offset(x, y);
      if (depth == 1) {
        const bool set = (row[x / 8] >> (7 - x % 8)) & 1u;
        const Rgba c = palette.size() >= 2 ? palette[set ? 1 : 0] : (set ? Rgba{0, 0, 0, 255} : Rgba{255, 255, 255, 255});
        std::memcpy(d, c.data(), 4);
      } else if (depth == 8) {
        const std::uint8_t i = row[x];
        const Rgba c = !palette.empty() ? (i < palette.size() ? palette[i] : Rgba{0, 0, 0, 255}) : Rgba{i, i, i, 255};
        std::memcpy(d, c.data(), 4);
      } else {
        const std::size_t step = depth / 8;
        const std::uint8_t* p = row + std::size_t{x} * step + (depth == 32 ? 1 : 0);
        d[0] = rgb_order ? p[0] : p[2];
        d[1] = p[1];
        d[2] = rgb_order ? p[2] : p[0];
      }
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

Result<Bytes> write_sun(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "Sun raster");
  CVT_TRY_ASSIGN(const ir::Frame frame, opaque_frame(prepared->frames.front(), ctx, "Sun raster"));
  const std::optional<Quantized> palette = exact_palette(frame, 256);
  const std::uint32_t depth = palette ? 8 : 24;
  const std::uint32_t w = frame.width;
  const std::uint32_t h = frame.height;
  const std::uint64_t stride = ((std::uint64_t{w} * depth + 15) / 16) * 2;
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(stride * h), 0);
  for (std::uint32_t y = 0; y < h; ++y) {
    std::uint8_t* row = raw.data() + stride * y;
    for (std::uint32_t x = 0; x < w; ++x) {
      if (palette) {
        row[x] = palette->indices[std::size_t{y} * w + x];
      } else {
        const std::uint8_t* p = frame.rgba.data() + frame.offset(x, y);
        row[x * 3] = p[2];
        row[x * 3 + 1] = p[1];
        row[x * 3 + 2] = p[0];
      }
    }
  }
  // Byte-encoded RLE on request (ImageMagick 7 reads RLE 24-bit data with
  // red and blue swapped, so the standard type is the default).
  const bool rle = ctx.options.get_bool("rle", false);
  util::ByteWriter body;
  if (!rle) body.bytes(std::span<const std::uint8_t>(raw));
  std::size_t i = rle ? 0 : raw.size();
  while (i < raw.size()) {
    std::size_t run = 1;
    while (i + run < raw.size() && run < 256 && raw[i + run] == raw[i]) ++run;
    if (run >= 3 || raw[i] == 0x80) {
      if (raw[i] == 0x80 && run < 3) {
        for (std::size_t k = 0; k < run; ++k) body.u8(0x80).u8(0);
      } else {
        body.u8(0x80).u8(static_cast<std::uint8_t>(run - 1)).u8(raw[i]);
      }
    } else {
      for (std::size_t k = 0; k < run; ++k) body.u8(raw[i]);
    }
    i += run;
  }
  util::ByteWriter out;
  const std::uint32_t map_length = palette ? 768 : 0;
  out.u32be(0x59A66A95u).u32be(w).u32be(h).u32be(depth).u32be(static_cast<std::uint32_t>(body.size()));
  out.u32be(rle ? 2 : 1).u32be(palette ? 1 : 0).u32be(map_length);
  if (palette) {
    for (std::size_t c = 0; c < 3; ++c) {
      for (std::size_t k = 0; k < 256; ++k) out.u8(k < palette->palette.size() ? palette->palette[k][c] : 0);
    }
  }
  out.bytes(body.view());
  return out.take();
}

}  // namespace

void register_sun(Registry& registry) {
  FormatInfo info;
  info.id = "ras";
  info.name = "Sun raster image";
  info.description = "Sun Microsystems raster image.";
  info.category = Category::Image;
  info.extensions = {"ras", "sun", "im1", "im8", "im24", "im32"};
  info.mime = "image/x-sun-raster";
  info.popularity = 5;
  info.signatures = {Signature::at(0, "59 A6 6A 95")};
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "ras";
  spec.read = reader(read_sun);
  spec.write = writer(write_sun);
  spec.write_options = plain_writer_options();
  spec.write_options.push_back(background_option());
  spec.write_options.push_back(OptionSpec::boolean("rle", "RLE compression", false,
                                                   "Byte-encode runs (smaller; some readers mishandle it).")
                                   .advanced());
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
