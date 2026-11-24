// Convertible core - image family: DirectDraw Surface (DDS).
//
// Reader: the top mip level of 2D textures: uncompressed formats described
// by bit masks (RGB, RGBA, luminance, alpha), BC1/DXT1, BC2/DXT2-3,
// BC3/DXT4-5, BC4 (ATI1) and BC5 (ATI2), and the DX10 header for those
// formats plus R8G8B8A8 / B8G8R8A8 / B8G8R8X8. Premultiplied DXT2/DXT4 are
// un-premultiplied. Writer: uncompressed BGRA, BC1 or BC3, optionally with
// a full mip chain.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "util/bytes.hpp"

namespace cvt::image {

namespace {

constexpr std::uint32_t fourcc(const char (&s)[5]) {
  return static_cast<std::uint32_t>(s[0]) | static_cast<std::uint32_t>(s[1]) << 8 |
         static_cast<std::uint32_t>(s[2]) << 16 | static_cast<std::uint32_t>(s[3]) << 24;
}

enum class Block { None, BC1, BC2, BC3, BC4, BC5 };

inline Rgba rgb565(std::uint16_t c) {
  return {scale_to_8((c >> 11) & 31, 5), scale_to_8((c >> 5) & 63, 6), scale_to_8(c & 31, 5), 255};
}

/// Decode the colour part of a BC1/2/3 block into 16 RGBA pixels.
void decode_colour(const std::uint8_t* b, Rgba* out, bool bc1) {
  const auto c0 = static_cast<std::uint16_t>(b[0] | b[1] << 8);
  const auto c1 = static_cast<std::uint16_t>(b[2] | b[3] << 8);
  std::array<Rgba, 4> p{};
  p[0] = rgb565(c0);
  p[1] = rgb565(c1);
  if (c0 > c1 || !bc1) {
    for (int c = 0; c < 3; ++c) {
      p[2][static_cast<std::size_t>(c)] = static_cast<std::uint8_t>((2 * p[0][static_cast<std::size_t>(c)] + p[1][static_cast<std::size_t>(c)] + 1) / 3);
      p[3][static_cast<std::size_t>(c)] = static_cast<std::uint8_t>((p[0][static_cast<std::size_t>(c)] + 2 * p[1][static_cast<std::size_t>(c)] + 1) / 3);
    }
    p[2][3] = p[3][3] = 255;
  } else {
    for (int c = 0; c < 3; ++c) p[2][static_cast<std::size_t>(c)] = static_cast<std::uint8_t>((p[0][static_cast<std::size_t>(c)] + p[1][static_cast<std::size_t>(c)] + 1) / 2);
    p[2][3] = 255;
    p[3] = {0, 0, 0, 0};
  }
  const std::uint32_t bits = static_cast<std::uint32_t>(b[4]) | static_cast<std::uint32_t>(b[5]) << 8 |
                             static_cast<std::uint32_t>(b[6]) << 16 | static_cast<std::uint32_t>(b[7]) << 24;
  for (int i = 0; i < 16; ++i) out[i] = p[(bits >> (2 * i)) & 3];
}

/// Decode an 8-byte BC4-style channel block (also BC3 alpha) into 16 values.
void decode_channel(const std::uint8_t* b, std::uint8_t* out) {
  std::array<std::uint8_t, 8> v{};
  v[0] = b[0];
  v[1] = b[1];
  if (v[0] > v[1]) {
    for (int i = 1; i < 7; ++i) v[static_cast<std::size_t>(i + 1)] = static_cast<std::uint8_t>(((7 - i) * v[0] + i * v[1] + 3) / 7);
  } else {
    for (int i = 1; i < 5; ++i) v[static_cast<std::size_t>(i + 1)] = static_cast<std::uint8_t>(((5 - i) * v[0] + i * v[1] + 2) / 5);
    v[6] = 0;
    v[7] = 255;
  }
  std::uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) out[i] = v[(bits >> (3 * i)) & 7];
}

Result<ir::Image> read_dds(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("DDS ")) return fail(Status::Decode, "not a DDS file");
  const std::uint32_t size = r.u32le();
  r.skip(4);  // flags
  const std::uint32_t height = r.u32le();
  const std::uint32_t width = r.u32le();
  const std::uint32_t pitch = r.u32le();
  r.skip(4 + 4 + 44);  // depth, mip count, reserved
  r.skip(4);           // pixel format size
  const std::uint32_t pf_flags = r.u32le();
  const std::uint32_t four = r.u32le();
  const std::uint32_t bit_count = r.u32le();
  std::array<std::uint32_t, 4> masks{};
  for (auto& m : masks) m = r.u32le();
  r.skip(16 + 4);  // caps
  if (!r.ok() || size != 124) return fail(Status::Decode, "DDS header is invalid");
  CVT_TRY(check_size(ctx, width, height));
  // Block compression stores at least 4 bits per pixel.
  CVT_TRY(check_plausible(std::uint64_t{width} * height, ctx.input.size(), 4));
  Block block = Block::None;
  bool premultiplied = false;
  bool swizzle_bgra = false;
  bool force_rgba32 = false;
  if (pf_flags & 0x4) {  // DDPF_FOURCC
    if (four == fourcc("DXT1")) block = Block::BC1;
    else if (four == fourcc("DXT2") || four == fourcc("DXT3")) block = Block::BC2, premultiplied = four == fourcc("DXT2");
    else if (four == fourcc("DXT4") || four == fourcc("DXT5")) block = Block::BC3, premultiplied = four == fourcc("DXT4");
    else if (four == fourcc("ATI1") || four == fourcc("BC4U")) block = Block::BC4;
    else if (four == fourcc("ATI2") || four == fourcc("BC5U")) block = Block::BC5;
    else if (four == fourcc("DX10")) {
      const std::uint32_t dxgi = r.u32le();
      const std::uint32_t dimension = r.u32le();
      r.skip(4);
      const std::uint32_t array_size = r.u32le();
      r.skip(4);
      if (!r.ok()) return fail(Status::Decode, "DDS DX10 header is truncated");
      if (dimension != 3 && dimension != 0) return fail(Status::Decode, "only 2D DDS textures are supported");
      (void)array_size;
      switch (dxgi) {
        case 70: case 71: case 72: block = Block::BC1; break;
        case 73: case 74: case 75: block = Block::BC2; break;
        case 76: case 77: case 78: block = Block::BC3; break;
        case 79: case 80: case 81: block = Block::BC4; break;
        case 82: case 83: case 84: block = Block::BC5; break;
        case 27: case 28: case 29: force_rgba32 = true; break;
        case 87: case 90: case 91: force_rgba32 = true, swizzle_bgra = true; break;
        case 88: case 92: case 93: force_rgba32 = true, swizzle_bgra = true; break;
        default: return fail(Status::Decode, "DDS DXGI format {} is not supported", dxgi);
      }
    } else {
      return fail(Status::Decode, "DDS compression is not supported");
    }
  }
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  const ByteView data = r.peek_rest();
  if (block != Block::None) {
    const std::uint32_t bw = (width + 3) / 4;
    const std::uint32_t bh = (height + 3) / 4;
    const std::size_t block_bytes = block == Block::BC1 || block == Block::BC4 ? 8 : 16;
    const std::uint64_t needed = std::uint64_t{bw} * bh * block_bytes;
    if (needed > data.size()) return fail(Status::Decode, "DDS texture data is truncated");
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::array<Rgba, 16> px{};
    std::array<std::uint8_t, 16> ch{};
    std::array<std::uint8_t, 16> ch2{};
    for (std::uint32_t by = 0; by < bh; ++by) {
      if ((by & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(by) / bh));
      for (std::uint32_t bx = 0; bx < bw; ++bx, p += block_bytes) {
        switch (block) {
          case Block::BC1: decode_colour(p, px.data(), true); break;
          case Block::BC2:
            decode_colour(p + 8, px.data(), false);
            for (int i = 0; i < 16; ++i) px[static_cast<std::size_t>(i)][3] = static_cast<std::uint8_t>(((p[i / 2] >> (4 * (i % 2))) & 15) * 17);
            break;
          case Block::BC3:
            decode_colour(p + 8, px.data(), false);
            decode_channel(p, ch.data());
            for (int i = 0; i < 16; ++i) px[static_cast<std::size_t>(i)][3] = ch[static_cast<std::size_t>(i)];
            break;
          case Block::BC4:
            decode_channel(p, ch.data());
            for (int i = 0; i < 16; ++i) px[static_cast<std::size_t>(i)] = {ch[static_cast<std::size_t>(i)], ch[static_cast<std::size_t>(i)], ch[static_cast<std::size_t>(i)], 255};
            break;
          default:
            decode_channel(p, ch.data());
            decode_channel(p + 8, ch2.data());
            for (int i = 0; i < 16; ++i) {
              // Normal map: reconstruct blue from the unit length.
              const double nx = ch[static_cast<std::size_t>(i)] / 127.5 - 1.0;
              const double ny = ch2[static_cast<std::size_t>(i)] / 127.5 - 1.0;
              const double nz = std::sqrt(std::max(0.0, 1.0 - nx * nx - ny * ny));
              px[static_cast<std::size_t>(i)] = {ch[static_cast<std::size_t>(i)], ch2[static_cast<std::size_t>(i)],
                                                 static_cast<std::uint8_t>(std::lround((nz + 1.0) * 127.5)), 255};
            }
            break;
        }
        for (std::uint32_t y = 0; y < 4; ++y) {
          for (std::uint32_t x = 0; x < 4; ++x) {
            const std::uint32_t X = bx * 4 + x;
            const std::uint32_t Y = by * 4 + y;
            if (X < width && Y < height) std::memcpy(frame.rgba.data() + frame.offset(X, Y), px[y * 4 + x].data(), 4);
          }
        }
      }
    }
    if (premultiplied) unpremultiply(frame);
  } else {
    std::uint32_t bits = bit_count;
    if (force_rgba32) {
      bits = 32;
      masks = swizzle_bgra ? std::array<std::uint32_t, 4>{0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000}
                           : std::array<std::uint32_t, 4>{0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000};
    }
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return fail(Status::Decode, "DDS pixel size {} is not supported", bits);
    const bool luminance = (pf_flags & 0x20000) != 0;
    const bool alpha_only = (pf_flags & 0x2) != 0 && (pf_flags & 0x40) == 0 && !luminance;
    const bool has_alpha = (pf_flags & 0x1) != 0 || alpha_only || force_rgba32;
    const std::uint32_t bytes = bits / 8;
    std::uint64_t stride = std::uint64_t{width} * bytes;
    if (pitch >= stride && pitch < stride + 4) stride = pitch;  // honour small row padding
    if (stride * height > data.size()) return fail(Status::Decode, "DDS texture data is truncated");
    const auto extract = [](std::uint32_t v, std::uint32_t mask) -> std::uint8_t {
      if (mask == 0) return 0;
      const int shift = std::countr_zero(mask);
      const int count = std::popcount(mask >> shift);
      return scale_to_8((v & mask) >> shift, std::min(count, 16));
    };
    for (std::uint32_t y = 0; y < height; ++y) {
      const auto* row = reinterpret_cast<const std::uint8_t*>(data.data()) + stride * y;
      for (std::uint32_t x = 0; x < width; ++x) {
        std::uint32_t v = 0;
        for (std::uint32_t k = 0; k < bytes; ++k) v |= static_cast<std::uint32_t>(row[x * bytes + k]) << (8 * k);
        std::uint8_t* d = frame.rgba.data() + frame.offset(x, y);
        if (alpha_only) {
          d[0] = d[1] = d[2] = 255;
          d[3] = extract(v, masks[3] != 0 ? masks[3] : 0xFFu);
        } else if (luminance) {
          d[0] = d[1] = d[2] = extract(v, masks[0]);
          d[3] = has_alpha ? extract(v, masks[3]) : 255;
        } else {
          d[0] = extract(v, masks[0]);
          d[1] = extract(v, masks[1]);
          d[2] = extract(v, masks[2]);
          d[3] = has_alpha && masks[3] != 0 ? extract(v, masks[3]) : 255;
        }
      }
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

// --------------------------------------------------------------------------- BC encoding

inline std::uint16_t to565(const float* c) {
  const auto r = static_cast<std::uint32_t>(std::clamp(std::lround(c[0] * 31.0f / 255.0f), 0L, 31L));
  const auto g = static_cast<std::uint32_t>(std::clamp(std::lround(c[1] * 63.0f / 255.0f), 0L, 63L));
  const auto b = static_cast<std::uint32_t>(std::clamp(std::lround(c[2] * 31.0f / 255.0f), 0L, 31L));
  return static_cast<std::uint16_t>(r << 11 | g << 5 | b);
}

/// Encode 16 RGBA pixels as a 4-colour BC1 block (or 3-colour + transparent
/// when `binary_alpha` and some pixels are transparent).
void encode_colour(const Rgba* px, std::uint8_t* out, bool binary_alpha) {
  // Principal axis of the colours (opaque pixels only when alpha matters).
  float mean[3] = {0, 0, 0};
  int n = 0;
  bool any_clear = false;
  for (int i = 0; i < 16; ++i) {
    if (binary_alpha && px[i][3] < 128) {
      any_clear = true;
      continue;
    }
    for (int c = 0; c < 3; ++c) mean[c] += px[i][static_cast<std::size_t>(c)];
    ++n;
  }
  if (n == 0) {
    // Fully transparent block: c0 <= c1 with every index 3.
    std::memset(out, 0, 4);
    out[4] = out[5] = out[6] = out[7] = 0xFF;
    return;
  }
  for (float& m : mean) m /= static_cast<float>(n);
  float cov[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    if (binary_alpha && px[i][3] < 128) continue;
    const float d[3] = {px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2]};
    cov[0] += d[0] * d[0];
    cov[1] += d[0] * d[1];
    cov[2] += d[0] * d[2];
    cov[3] += d[1] * d[1];
    cov[4] += d[1] * d[2];
    cov[5] += d[2] * d[2];
  }
  float axis[3] = {1, 1, 1};
  for (int it = 0; it < 8; ++it) {
    const float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
    const float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
    const float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
    const float len = std::max({std::abs(x), std::abs(y), std::abs(z)});
    if (len <= 1e-6f) break;
    axis[0] = x / len;
    axis[1] = y / len;
    axis[2] = z / len;
  }
  float lo = std::numeric_limits<float>::infinity();
  float hi = -lo;
  for (int i = 0; i < 16; ++i) {
    if (binary_alpha && px[i][3] < 128) continue;
    const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
    lo = std::min(lo, t);
    hi = std::max(hi, t);
  }
  const float norm = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
  float e0[3];
  float e1[3];
  for (int c = 0; c < 3; ++c) {
    e0[c] = std::clamp(mean[c] + axis[c] * hi / std::max(norm, 1e-6f), 0.0f, 255.0f);
    e1[c] = std::clamp(mean[c] + axis[c] * lo / std::max(norm, 1e-6f), 0.0f, 255.0f);
  }
  std::uint16_t c0 = to565(e0);
  std::uint16_t c1 = to565(e1);
  const bool three = binary_alpha && any_clear;
  if (three ? c0 > c1 : c0 < c1) std::swap(c0, c1);
  if (!three && c0 == c1) {
    // A flat block: every index 0.
    out[0] = static_cast<std::uint8_t>(c0);
    out[1] = static_cast<std::uint8_t>(c0 >> 8);
    out[2] = static_cast<std::uint8_t>(c1);
    out[3] = static_cast<std::uint8_t>(c1 >> 8);
    std::memset(out + 4, 0, 4);
    return;
  }
  std::array<Rgba, 4> palette{};
  palette[0] = rgb565(c0);
  palette[1] = rgb565(c1);
  if (!three) {
    for (std::size_t c = 0; c < 3; ++c) {
      palette[2][c] = static_cast<std::uint8_t>((2 * palette[0][c] + palette[1][c] + 1) / 3);
      palette[3][c] = static_cast<std::uint8_t>((palette[0][c] + 2 * palette[1][c] + 1) / 3);
    }
  } else {
    for (std::size_t c = 0; c < 3; ++c) palette[2][c] = static_cast<std::uint8_t>((palette[0][c] + palette[1][c] + 1) / 2);
  }
  std::uint32_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    std::uint32_t best = 0;
    if (three && px[i][3] < 128) {
      best = 3;
    } else {
      int best_d = std::numeric_limits<int>::max();
      const int choices = three ? 3 : 4;
      for (int k = 0; k < choices; ++k) {
        int d = 0;
        for (std::size_t c = 0; c < 3; ++c) {
          const int e = static_cast<int>(px[i][c]) - palette[static_cast<std::size_t>(k)][c];
          d += e * e;
        }
        if (d < best_d) {
          best_d = d;
          best = static_cast<std::uint32_t>(k);
        }
      }
    }
    bits |= best << (2 * i);
  }
  out[0] = static_cast<std::uint8_t>(c0);
  out[1] = static_cast<std::uint8_t>(c0 >> 8);
  out[2] = static_cast<std::uint8_t>(c1);
  out[3] = static_cast<std::uint8_t>(c1 >> 8);
  for (int k = 0; k < 4; ++k) out[4 + k] = static_cast<std::uint8_t>(bits >> (8 * k));
}

void encode_alpha(const Rgba* px, std::uint8_t* out) {
  std::uint8_t lo = 255;
  std::uint8_t hi = 0;
  for (int i = 0; i < 16; ++i) {
    lo = std::min(lo, px[i][3]);
    hi = std::max(hi, px[i][3]);
  }
  out[0] = hi;
  out[1] = lo;
  std::array<int, 8> v{};
  v[0] = hi;
  v[1] = lo;
  for (int i = 1; i < 7; ++i) v[static_cast<std::size_t>(i + 1)] = ((7 - i) * hi + i * lo + 3) / 7;
  std::uint64_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    std::uint64_t best = 0;
    int best_d = 1 << 20;
    for (int k = 0; k < 8; ++k) {
      const int d = std::abs(v[static_cast<std::size_t>(k)] - px[i][3]);
      if (d < best_d) {
        best_d = d;
        best = static_cast<std::uint64_t>(k);
      }
    }
    if (hi == lo) best = 0;
    bits |= best << (3 * i);
  }
  for (int k = 0; k < 6; ++k) out[2 + k] = static_cast<std::uint8_t>(bits >> (8 * k));
}

Result<Bytes> write_dds(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "DDS");
  const std::string format = ctx.options.get_enum("dds_format", "rgba", {"rgba", "bc1", "bc3"});
  const bool mipmaps = ctx.options.get_bool("mipmaps", false);
  ir::Frame level = prepared->frames.front();
  const std::uint32_t width = level.width;
  const std::uint32_t height = level.height;
  std::uint32_t levels = 1;
  if (mipmaps) {
    std::uint32_t w = width, h = height;
    while (w > 1 || h > 1) {
      w = std::max(1u, w / 2);
      h = std::max(1u, h / 2);
      ++levels;
    }
  }
  const bool bc = format != "rgba";
  util::ByteWriter out;
  out.text("DDS ").u32le(124);
  std::uint32_t flags = 0x1 | 0x2 | 0x4 | 0x1000;  // caps, height, width, pixel format
  flags |= bc ? 0x80000u : 0x8u;                    // linear size / pitch
  if (mipmaps) flags |= 0x20000;
  out.u32le(flags).u32le(height).u32le(width);
  const std::uint32_t top_size = bc ? std::max(1u, (width + 3) / 4) * std::max(1u, (height + 3) / 4) * (format == "bc1" ? 8u : 16u)
                                    : width * 4;
  out.u32le(top_size).u32le(0).u32le(levels);
  out.zeros(44);
  // Pixel format.
  out.u32le(32);
  if (bc) {
    out.u32le(0x4).text(format == "bc1" ? "DXT1" : "DXT5").zeros(20);
  } else {
    out.u32le(0x41).u32le(0).u32le(32).u32le(0x00FF0000).u32le(0x0000FF00).u32le(0x000000FF).u32le(0xFF000000);
  }
  out.u32le(0x1000 | (mipmaps ? 0x400008u : 0u)).zeros(16);
  const bool transparent_bc1 = format == "bc1" && !is_opaque(level);
  for (std::uint32_t l = 0; l < levels; ++l) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(l) / levels, "Encoding"));
    if (l > 0) {
      CVT_TRY_ASSIGN(level, resize(level, std::max(1u, level.width / 2), std::max(1u, level.height / 2), {ResampleFilter::Box}));
    }
    if (!bc) {
      for (std::size_t i = 0; i + 3 < level.rgba.size(); i += 4) {
        out.u8(level.rgba[i + 2]).u8(level.rgba[i + 1]).u8(level.rgba[i]).u8(level.rgba[i + 3]);
      }
      continue;
    }
    std::array<Rgba, 16> px{};
    std::uint8_t block[16];
    for (std::uint32_t by = 0; by < (level.height + 3) / 4; ++by) {
      for (std::uint32_t bx = 0; bx < (level.width + 3) / 4; ++bx) {
        for (std::uint32_t y = 0; y < 4; ++y) {
          for (std::uint32_t x = 0; x < 4; ++x) {
            const std::uint32_t X = std::min(bx * 4 + x, level.width - 1);
            const std::uint32_t Y = std::min(by * 4 + y, level.height - 1);
            std::memcpy(px[y * 4 + x].data(), level.rgba.data() + level.offset(X, Y), 4);
          }
        }
        if (format == "bc1") {
          encode_colour(px.data(), block, transparent_bc1);
          out.bytes(std::span<const std::uint8_t>(block, 8));
        } else {
          encode_alpha(px.data(), block);
          encode_colour(px.data(), block + 8, false);
          out.bytes(std::span<const std::uint8_t>(block, 16));
        }
      }
    }
  }
  return out.take();
}

}  // namespace

void register_dds(Registry& registry) {
  FormatInfo info;
  info.id = "dds";
  info.name = "DirectDraw Surface";
  info.description = "GPU texture (uncompressed or BC1-BC5 block compressed).";
  info.category = Category::Image;
  info.extensions = {"dds"};
  info.mime = "image/vnd-ms.dds";
  info.popularity = 20;
  info.signatures = {Signature::at(0, "'DDS ' 7C 00 00 00")};
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "dds";
  spec.read = reader(read_dds);
  spec.write = writer(write_dds);
  spec.write_options = plain_writer_options();
  spec.write_options.push_back(OptionSpec::choice("dds_format", "Texture format", "rgba", {"rgba", "bc1", "bc3"},
                                                  "Uncompressed BGRA (lossless), BC1/DXT1 (smallest, 1-bit alpha) or "
                                                  "BC3/DXT5 (smooth alpha)."));
  spec.write_options.push_back(OptionSpec::boolean("mipmaps", "Mipmaps", false,
                                                   "Also store every smaller level, as GPUs expect for textures."));
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
