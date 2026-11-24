// Convertible core - image family: QOI ("Quite OK Image", specification 1.0).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "util/bytes.hpp"

namespace cvt::image {

namespace {

constexpr std::uint8_t op_index = 0x00;
constexpr std::uint8_t op_diff = 0x40;
constexpr std::uint8_t op_luma = 0x80;
constexpr std::uint8_t op_run = 0xC0;
constexpr std::uint8_t op_rgb = 0xFE;
constexpr std::uint8_t op_rgba = 0xFF;

inline std::size_t hash(const std::uint8_t* p) {
  return (p[0] * 3u + p[1] * 5u + p[2] * 7u + p[3] * 11u) % 64u;
}

Result<ir::Image> read_qoi(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("qoif")) return fail(Status::Decode, "not a QOI file");
  const std::uint32_t width = r.u32be();
  const std::uint32_t height = r.u32be();
  const std::uint8_t channels = r.u8();
  const std::uint8_t colorspace = r.u8();
  if (!r.ok()) return fail(Status::Decode, "QOI header is truncated");
  if ((channels != 3 && channels != 4) || colorspace > 1) return fail(Status::Decode, "QOI header is invalid");
  CVT_TRY(check_size(ctx, width, height));
  // The best case is a run of 62 pixels per byte.
  CVT_TRY(check_plausible(std::uint64_t{width} * height, ctx.input.size(), 62));
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  std::array<std::array<std::uint8_t, 4>, 64> seen{};
  std::uint8_t px[4] = {0, 0, 0, 255};
  const std::size_t total = static_cast<std::size_t>(width) * height;
  const auto* data = reinterpret_cast<const std::uint8_t*>(ctx.input.data());
  std::size_t pos = 14;
  const std::size_t end = ctx.input.size() >= 8 ? ctx.input.size() - 8 : 0;  // the 8-byte end marker
  int run = 0;
  bool truncated = false;
  for (std::size_t i = 0; i < total; ++i) {
    if ((i & 0xFFFFF) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(total)));
    if (run > 0) {
      --run;
    } else if (pos < end) {
      const std::uint8_t b = data[pos++];
      if (b == op_rgb) {
        if (pos + 3 > end) {
          truncated = true;
          break;
        }
        px[0] = data[pos];
        px[1] = data[pos + 1];
        px[2] = data[pos + 2];
        pos += 3;
      } else if (b == op_rgba) {
        if (pos + 4 > end) {
          truncated = true;
          break;
        }
        ::cvt::util::copy_bytes(px, data + pos, 4);
        pos += 4;
      } else if ((b & 0xC0) == op_index) {
        ::cvt::util::copy_bytes(px, seen[b].data(), 4);
      } else if ((b & 0xC0) == op_diff) {
        px[0] = static_cast<std::uint8_t>(px[0] + ((b >> 4) & 3) - 2);
        px[1] = static_cast<std::uint8_t>(px[1] + ((b >> 2) & 3) - 2);
        px[2] = static_cast<std::uint8_t>(px[2] + (b & 3) - 2);
      } else if ((b & 0xC0) == op_luma) {
        if (pos >= end) {
          truncated = true;
          break;
        }
        const std::uint8_t b2 = data[pos++];
        const int vg = (b & 0x3F) - 32;
        px[0] = static_cast<std::uint8_t>(px[0] + vg - 8 + ((b2 >> 4) & 0x0F));
        px[1] = static_cast<std::uint8_t>(px[1] + vg);
        px[2] = static_cast<std::uint8_t>(px[2] + vg - 8 + (b2 & 0x0F));
      } else {
        run = b & 0x3F;
      }
      ::cvt::util::copy_bytes(seen[hash(px)].data(), px, 4);
    } else {
      truncated = true;
      break;
    }
    ::cvt::util::copy_bytes(frame.rgba.data() + i * 4, px, 4);
  }
  if (truncated) ctx.warn("The QOI file is truncated; missing pixels were left black.");
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

Result<Bytes> write_qoi(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "QOI");
  const ir::Frame& frame = prepared->frames.front();
  const bool alpha = frame.has_transparency();
  util::ByteWriter out(frame.rgba.size() / 2 + 32);
  out.text("qoif").u32be(frame.width).u32be(frame.height).u8(alpha ? 4 : 3).u8(0);
  std::array<std::array<std::uint8_t, 4>, 64> seen{};
  std::uint8_t prev[4] = {0, 0, 0, 255};
  int run = 0;
  const std::size_t total = static_cast<std::size_t>(frame.width) * frame.height;
  for (std::size_t i = 0; i < total; ++i) {
    if ((i & 0xFFFFF) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(total)));
    const std::uint8_t* px = frame.rgba.data() + i * 4;
    if (::cvt::util::compare_bytes(px, prev, 4) == 0) {
      ++run;
      if (run == 62 || i + 1 == total) {
        out.u8(static_cast<std::uint8_t>(op_run | (run - 1)));
        run = 0;
      }
      continue;
    }
    if (run > 0) {
      out.u8(static_cast<std::uint8_t>(op_run | (run - 1)));
      run = 0;
    }
    const std::size_t h = hash(px);
    if (::cvt::util::compare_bytes(seen[h].data(), px, 4) == 0) {
      out.u8(static_cast<std::uint8_t>(op_index | h));
    } else {
      ::cvt::util::copy_bytes(seen[h].data(), px, 4);
      if (px[3] == prev[3]) {
        const int vr = static_cast<std::int8_t>(px[0] - prev[0]);
        const int vg = static_cast<std::int8_t>(px[1] - prev[1]);
        const int vb = static_cast<std::int8_t>(px[2] - prev[2]);
        const int vg_r = vr - vg;
        const int vg_b = vb - vg;
        if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
          out.u8(static_cast<std::uint8_t>(op_diff | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2)));
        } else if (vg_r > -9 && vg_r < 8 && vg > -33 && vg < 32 && vg_b > -9 && vg_b < 8) {
          out.u8(static_cast<std::uint8_t>(op_luma | (vg + 32)));
          out.u8(static_cast<std::uint8_t>((vg_r + 8) << 4 | (vg_b + 8)));
        } else {
          out.u8(op_rgb).u8(px[0]).u8(px[1]).u8(px[2]);
        }
      } else {
        out.u8(op_rgba).u8(px[0]).u8(px[1]).u8(px[2]).u8(px[3]);
      }
    }
    ::cvt::util::copy_bytes(prev, px, 4);
  }
  out.zeros(7).u8(1);
  return out.take();
}

}  // namespace

void register_qoi(Registry& registry) {
  FormatInfo info;
  info.id = "qoi";
  info.name = "QOI image";
  info.description = "Quite OK Image: fast lossless compression.";
  info.category = Category::Image;
  info.extensions = {"qoi"};
  info.mime = "image/qoi";
  info.popularity = 15;
  info.signatures = {Signature::at(0, "'qoif'")};
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "qoi";
  spec.read = reader(read_qoi);
  spec.write = writer(write_qoi);
  spec.write_options = plain_writer_options();
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
