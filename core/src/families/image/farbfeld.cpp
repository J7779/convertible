// Convertible core - image family: farbfeld (16-bit RGBA, big endian).
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

Result<ir::Image> read_farbfeld(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("farbfeld")) return fail(Status::Decode, "not a farbfeld file");
  const std::uint32_t width = r.u32be();
  const std::uint32_t height = r.u32be();
  if (!r.ok()) return fail(Status::Decode, "farbfeld header is truncated");
  CVT_TRY(check_size(ctx, width, height));
  const std::uint64_t pixels = std::uint64_t{width} * height;
  if (r.remaining() / 8 < pixels) return fail(Status::Decode, "farbfeld pixel data is truncated");
  std::vector<std::uint16_t> wide(static_cast<std::size_t>(pixels * 4));
  ir::Frame frame = ir::Frame::filled(width, height);
  for (std::size_t i = 0; i < wide.size(); ++i) {
    wide[i] = r.u16be();
    frame.rgba[i] = static_cast<std::uint8_t>((wide[i] * 255u + 32767u) / 65535u);
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  image.hdr = linear_from_srgb16(wide, width, height);
  image.source_bit_depth = 16;
  return image;
}

Result<Bytes> write_farbfeld(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "farbfeld");
  const ir::Image& image = *prepared;
  const ir::Frame& frame = image.frames.front();
  const bool from_hdr = image.hdr && image.hdr->width == frame.width && image.hdr->height == frame.height;
  util::ByteWriter out(frame.rgba.size() * 2 + 16);
  out.text("farbfeld").u32be(frame.width).u32be(frame.height);
  if (from_hdr) {
    for (const std::uint16_t v : srgb16_from_linear(*image.hdr)) out.u16be(v);
  } else {
    for (const std::uint8_t v : frame.rgba) out.u16be(static_cast<std::uint16_t>(v * 257u));
  }
  return out.take();
}

}  // namespace

void register_farbfeld(Registry& registry) {
  FormatInfo info;
  info.id = "ff";
  info.name = "farbfeld image";
  info.description = "Minimal 16-bit RGBA image format.";
  info.category = Category::Image;
  info.extensions = {"ff", "farbfeld"};
  info.mime = "image/x-farbfeld";
  info.popularity = 5;
  info.signatures = {Signature::at(0, "'farbfeld'")};
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "ff";
  spec.read = reader(read_farbfeld);
  spec.write = writer(write_farbfeld);
  spec.write_options = plain_writer_options();
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
