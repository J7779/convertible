// Convertible core - image family: Wireless bitmap (WBMP, type 0).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "util/bytes.hpp"

namespace cvt::image {

namespace {

// Multi-byte integer: 7 bits per byte, high bit = more bytes follow.
bool read_varint(util::ByteReader& r, std::uint32_t& value) {
  value = 0;
  for (int i = 0; i < 5; ++i) {
    const std::uint8_t b = r.u8();
    if (!r.ok()) return false;
    value = (value << 7) | (b & 0x7Fu);
    if ((b & 0x80) == 0) return true;
  }
  return false;
}

void write_varint(util::ByteWriter& out, std::uint32_t value) {
  std::uint8_t bytes[5];
  int n = 0;
  do {
    bytes[n++] = static_cast<std::uint8_t>(value & 0x7F);
    value >>= 7;
  } while (value != 0);
  for (int i = n - 1; i >= 0; --i) out.u8(static_cast<std::uint8_t>(bytes[i] | (i > 0 ? 0x80 : 0)));
}

Result<ir::Image> read_wbmp(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  std::uint32_t type = 0;
  if (!read_varint(r, type) || type != 0) return fail(Status::Decode, "not a WBMP (type 0) file");
  const std::uint8_t fixed = r.u8();
  if (!r.ok() || (fixed & 0x9F) != 0) return fail(Status::Decode, "WBMP header is invalid");
  if (fixed & 0x60) {
    // Extension headers: skip them.
    std::uint8_t ext = 0;
    do {
      ext = r.u8();
    } while (r.ok() && (ext & 0x80));
  }
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  if (!read_varint(r, width) || !read_varint(r, height)) return fail(Status::Decode, "WBMP header is truncated");
  CVT_TRY(check_size(ctx, width, height));
  const std::size_t row = (width + 7) / 8;
  if (r.remaining() / row < height) return fail(Status::Decode, "WBMP pixel data is truncated");
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  for (std::uint32_t y = 0; y < height; ++y) {
    const ByteView bits = r.bytes(row);
    for (std::uint32_t x = 0; x < width; ++x) {
      if ((std::to_integer<std::uint8_t>(bits[x / 8]) >> (7 - x % 8)) & 1u) {  // 1 = white
        std::uint8_t* p = frame.rgba.data() + frame.offset(x, y);
        p[0] = p[1] = p[2] = 255;
      }
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  image.source_bit_depth = 1;
  image.source_grayscale = true;
  return image;
}

Result<Bytes> write_wbmp(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "WBMP");
  CVT_TRY_ASSIGN(ir::Frame frame, opaque_frame(prepared->frames.front(), ctx, "WBMP"));
  to_grayscale(frame);
  reduce_bit_depth(frame, 1, resolved_dither(ctx.options));
  util::ByteWriter out;
  write_varint(out, 0);
  out.u8(0);
  write_varint(out, frame.width);
  write_varint(out, frame.height);
  std::vector<std::uint8_t> row((frame.width + 7) / 8);
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    std::fill(row.begin(), row.end(), std::uint8_t{0});
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      if (frame.rgba[frame.offset(x, y)] >= 128) row[x / 8] |= static_cast<std::uint8_t>(0x80u >> (x % 8));
    }
    out.bytes(std::span<const std::uint8_t>(row));
  }
  return out.take();
}

int sniff_wbmp(std::span<const std::byte> data, std::string_view filename) {
  // No magic number: only plausible headers with a matching size, and
  // only together with the extension.
  if (!filename.ends_with(".wbmp") && !filename.ends_with(".WBMP")) return 0;
  util::ByteReader r(data);
  std::uint32_t type = 0;
  if (!read_varint(r, type) || type != 0 || r.u8() != 0) return 0;
  std::uint32_t w = 0;
  std::uint32_t h = 0;
  if (!read_varint(r, w) || !read_varint(r, h) || w == 0 || h == 0) return 0;
  return r.remaining() == std::uint64_t{(w + 7) / 8} * h ? 80 : 40;
}

}  // namespace

void register_wbmp(Registry& registry) {
  FormatInfo info;
  info.id = "wbmp";
  info.name = "Wireless bitmap";
  info.description = "Black-and-white image for early mobile phones (WAP).";
  info.category = Category::Image;
  info.extensions = {"wbmp"};
  info.mime = "image/vnd.wap.wbmp";
  info.popularity = 3;
  info.sniff = sniff_wbmp;
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "wbmp";
  spec.read = reader(read_wbmp);
  spec.write = writer(write_wbmp);
  spec.write_options = plain_writer_options();
  spec.write_options.push_back(dither_option());
  spec.write_options.push_back(background_option());
  spec.lossy_write = true;
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
