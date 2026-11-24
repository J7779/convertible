// Convertible core - image_modern family: GPU texture containers.
//
//   ktx   Khronos KTX 1.1 (read): uncompressed GL formats and the BCn, BPTC
//         (BC7), ETC1/ETC2/EAC and ASTC compressed formats.
//   ktx2  Khronos KTX 2.0 (read + write): the same Vulkan formats; no or
//         zlib supercompression (Zstandard and Basis Universal are not
//         supported). Writes uncompressed R8G8B8A8_SRGB.
//   pvr   PowerVR PVR v3 (read): byte/short/float channel layouts, ETC1/2,
//         EAC, DXT1-5, BC4/5/7 and ASTC (PVRTC is not supported).
//   astc  ARM .astc files (read, 2D LDR).
//   vtf   Valve Texture Format 7.0-7.5 (read): RGBA/BGRA/RGB/BGR/ABGR,
//         luminance, alpha, UV88, 16-bit and half-float RGBA, DXT1/3/5;
//         the largest mip level, every animation frame.
// Only the first array layer, cube face and depth slice of the largest mip
// level is read; that is the picture.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/misc.hpp"
#include "families/image_modern/misc_blocks.hpp"
#include "families/image_modern/misc_pixel.hpp"
#include "util/bytes.hpp"
#include "util/deflate.hpp"

namespace cvt::image_modern {

namespace {

using image::ImageCodecSpec;

// --------------------------------------------------------------------------- uncompressed layouts

enum class Raw {
  R8, RG8, RGB8, BGR8, RGBA8, BGRA8, ABGR8, BGRX8, L8, LA8, A8,
  R16, RG16, RGB16, RGBA16,
  R16F, RG16F, RGB16F, RGBA16F,
  R32F, RG32F, RGB32F, RGBA32F,
  RGB565, BGR565, RGBA4444, RGBA5551, BGRA5551, A2B10G10R10, B10G11R11F, E5B9G9R9,
};

int raw_bytes(Raw f) {
  switch (f) {
    case Raw::R8: case Raw::L8: case Raw::A8: return 1;
    case Raw::RG8: case Raw::LA8: case Raw::R16: case Raw::R16F: case Raw::RGB565: case Raw::BGR565:
    case Raw::RGBA4444: case Raw::RGBA5551: case Raw::BGRA5551: return 2;
    case Raw::RGB8: case Raw::BGR8: return 3;
    case Raw::RGBA8: case Raw::BGRA8: case Raw::ABGR8: case Raw::BGRX8: case Raw::RG16: case Raw::RG16F:
    case Raw::R32F: case Raw::A2B10G10R10: case Raw::B10G11R11F: case Raw::E5B9G9R9: return 4;
    case Raw::RGB16: case Raw::RGB16F: return 6;
    case Raw::RGBA16: case Raw::RGBA16F: case Raw::RG32F: return 8;
    case Raw::RGB32F: return 12;
    case Raw::RGBA32F: return 16;
  }
  return 4;
}

bool raw_is_float(Raw f) {
  switch (f) {
    case Raw::R16F: case Raw::RG16F: case Raw::RGB16F: case Raw::RGBA16F: case Raw::R32F: case Raw::RG32F:
    case Raw::RGB32F: case Raw::RGBA32F: case Raw::B10G11R11F: case Raw::E5B9G9R9: return true;
    default: return false;
  }
}
bool raw_is_16(Raw f) { return f == Raw::R16 || f == Raw::RG16 || f == Raw::RGB16 || f == Raw::RGBA16; }

/// Unsigned float with `mantissa` bits and a 5-bit exponent (B10G11R11).
float small_float(std::uint32_t bits, int mantissa) {
  const std::uint32_t e = bits >> mantissa;
  const std::uint32_t m = bits & ((1u << mantissa) - 1u);
  const float frac = static_cast<float>(m) / static_cast<float>(1u << mantissa);
  if (e == 0) return std::ldexp(frac, -14);
  if (e == 31) return 0.0f;
  return std::ldexp(1.0f + frac, static_cast<int>(e) - 15);
}

/// One pixel -> RGBA in 0..1 (float layouts: linear values, may exceed 1).
void raw_pixel(Raw f, const std::uint8_t* p, float* o) {
  const auto u8v = [](std::uint8_t v) { return v / 255.0f; };
  const auto u16v = [&](int i) { return load_le16(p + 2 * i) / 65535.0f; };
  const auto f16 = [&](int i) { return finite_or_zero(half_to_float(load_le16(p + 2 * i))); };
  const auto f32 = [&](int i) { return finite_or_zero(std::bit_cast<float>(load_le32(p + 4 * i))); };
  o[0] = o[1] = o[2] = 0.0f;
  o[3] = 1.0f;
  switch (f) {
    case Raw::R8: case Raw::L8: o[0] = o[1] = o[2] = u8v(p[0]); break;
    case Raw::A8: o[0] = o[1] = o[2] = 1.0f; o[3] = u8v(p[0]); break;
    case Raw::RG8: o[0] = u8v(p[0]); o[1] = u8v(p[1]); break;
    case Raw::LA8: o[0] = o[1] = o[2] = u8v(p[0]); o[3] = u8v(p[1]); break;
    case Raw::RGB8: for (int c = 0; c < 3; ++c) o[c] = u8v(p[c]); break;
    case Raw::BGR8: for (int c = 0; c < 3; ++c) o[c] = u8v(p[2 - c]); break;
    case Raw::RGBA8: for (int c = 0; c < 4; ++c) o[c] = u8v(p[c]); break;
    case Raw::BGRA8: o[0] = u8v(p[2]); o[1] = u8v(p[1]); o[2] = u8v(p[0]); o[3] = u8v(p[3]); break;
    case Raw::BGRX8: o[0] = u8v(p[2]); o[1] = u8v(p[1]); o[2] = u8v(p[0]); break;
    case Raw::ABGR8: o[3] = u8v(p[0]); o[2] = u8v(p[1]); o[1] = u8v(p[2]); o[0] = u8v(p[3]); break;
    case Raw::R16: o[0] = o[1] = o[2] = u16v(0); break;
    case Raw::RG16: o[0] = u16v(0); o[1] = u16v(1); break;
    case Raw::RGB16: for (int c = 0; c < 3; ++c) o[c] = u16v(c); break;
    case Raw::RGBA16: for (int c = 0; c < 4; ++c) o[c] = u16v(c); break;
    case Raw::R16F: o[0] = o[1] = o[2] = f16(0); break;
    case Raw::RG16F: o[0] = f16(0); o[1] = f16(1); break;
    case Raw::RGB16F: for (int c = 0; c < 3; ++c) o[c] = f16(c); break;
    case Raw::RGBA16F: for (int c = 0; c < 4; ++c) o[c] = f16(c); break;
    case Raw::R32F: o[0] = o[1] = o[2] = f32(0); break;
    case Raw::RG32F: o[0] = f32(0); o[1] = f32(1); break;
    case Raw::RGB32F: for (int c = 0; c < 3; ++c) o[c] = f32(c); break;
    case Raw::RGBA32F: for (int c = 0; c < 4; ++c) o[c] = f32(c); break;
    case Raw::RGB565: {
      const int v = load_le16(p);
      o[0] = ((v >> 11) & 31) / 31.0f;
      o[1] = ((v >> 5) & 63) / 63.0f;
      o[2] = (v & 31) / 31.0f;
      break;
    }
    case Raw::BGR565: {
      const int v = load_le16(p);
      o[2] = ((v >> 11) & 31) / 31.0f;
      o[1] = ((v >> 5) & 63) / 63.0f;
      o[0] = (v & 31) / 31.0f;
      break;
    }
    case Raw::RGBA4444: {
      const int v = load_le16(p);
      for (int c = 0; c < 4; ++c) o[c] = ((v >> (12 - 4 * c)) & 15) / 15.0f;
      break;
    }
    case Raw::RGBA5551: {
      const int v = load_le16(p);
      o[0] = ((v >> 11) & 31) / 31.0f;
      o[1] = ((v >> 6) & 31) / 31.0f;
      o[2] = ((v >> 1) & 31) / 31.0f;
      o[3] = static_cast<float>(v & 1);
      break;
    }
    case Raw::BGRA5551: {
      const int v = load_le16(p);
      o[3] = static_cast<float>((v >> 15) & 1);
      o[0] = ((v >> 10) & 31) / 31.0f;
      o[1] = ((v >> 5) & 31) / 31.0f;
      o[2] = (v & 31) / 31.0f;
      break;
    }
    case Raw::A2B10G10R10: {
      const std::uint32_t v = load_le32(p);
      o[0] = (v & 1023) / 1023.0f;
      o[1] = ((v >> 10) & 1023) / 1023.0f;
      o[2] = ((v >> 20) & 1023) / 1023.0f;
      o[3] = (v >> 30) / 3.0f;
      break;
    }
    case Raw::B10G11R11F: {
      const std::uint32_t v = load_le32(p);
      o[0] = small_float(v & 0x7FF, 6);
      o[1] = small_float((v >> 11) & 0x7FF, 6);
      o[2] = small_float(v >> 22, 5);
      break;
    }
    case Raw::E5B9G9R9: {
      const std::uint32_t v = load_le32(p);
      const float scale = std::ldexp(1.0f, static_cast<int>(v >> 27) - 15 - 9);
      o[0] = (v & 511) * scale;
      o[1] = ((v >> 9) & 511) * scale;
      o[2] = ((v >> 18) & 511) * scale;
      break;
    }
  }
}

/// Decode an uncompressed surface. Float layouts also fill `hdr`.
Result<ir::Image> decode_raw(Raw format, ByteView data, std::uint32_t width, std::uint32_t height,
                             std::uint64_t row_stride, const Context& ctx) {
  CVT_TRY(image::check_size(ctx, width, height));
  const std::uint64_t bpp = static_cast<std::uint64_t>(raw_bytes(format));
  if (row_stride == 0) row_stride = bpp * width;
  if (row_stride < bpp * width || data.size() < row_stride * (height - 1) + bpp * width) {
    return fail(Status::Decode, "the texture data is truncated");
  }
  const bool is_float = raw_is_float(format);
  const bool wide = raw_is_16(format);
  ir::Image image;
  ir::Frame frame = ir::Frame::filled(width, height);
  ir::HdrPixels hdr;
  if (is_float || wide) {
    hdr.width = width;
    hdr.height = height;
    hdr.rgba.resize(std::size_t{width} * height * 4);
  }
  const auto* src = reinterpret_cast<const std::uint8_t*>(data.data());
  for (std::uint32_t y = 0; y < height; ++y) {
    if ((y & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / height));
    for (std::uint32_t x = 0; x < width; ++x) {
      float px[4];
      raw_pixel(format, src + y * row_stride + x * bpp, px);
      std::uint8_t* out = frame.rgba.data() + frame.offset(x, y);
      for (int c = 0; c < 4; ++c) {
        if (is_float && c < 3) {
          out[c] = image::linear_to_srgb8(px[c]);
        } else {
          out[c] = static_cast<std::uint8_t>(std::lround(std::clamp(px[c], 0.0f, 1.0f) * 255.0f));
        }
        if (is_float || wide) {
          // Integer samples are sRGB-encoded: store them linear.
          hdr.rgba[frame.offset(x, y) + static_cast<std::size_t>(c)] =
              (c < 3 && !is_float) ? image::srgb_to_linear(std::clamp(px[c], 0.0f, 1.0f)) : px[c];
        }
      }
    }
  }
  image.frames.push_back(std::move(frame));
  if (is_float || wide) {
    image.hdr = std::move(hdr);
    image.source_bit_depth = is_float ? 32 : 16;
  }
  return image;
}

/// A texture format: either an uncompressed layout or a block format.
struct TexFormat {
  std::optional<Raw> raw;
  std::optional<BlockFormat> block;
  int astc_w = 4;
  int astc_h = 4;
  std::string unsupported;  // non-empty: name of an unsupported format
};

Result<ir::Image> decode_texture(const TexFormat& format, ByteView data, std::uint32_t width, std::uint32_t height,
                                 std::uint64_t row_stride, const Context& ctx) {
  if (!format.unsupported.empty()) {
    return fail(Status::UnsupportedConversion, "{} textures are not supported", format.unsupported);
  }
  if (format.raw) return decode_raw(*format.raw, data, width, height, row_stride, ctx);
  if (!format.block) return fail(Status::UnsupportedConversion, "this texture format is not supported");
  CVT_TRY(image::check_size(ctx, width, height));
  CVT_TRY_ASSIGN(ir::Frame frame, decode_surface(*format.block, data, width, height, ctx, format.astc_w, format.astc_h));
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

std::uint64_t texture_bytes(const TexFormat& format, std::uint32_t width, std::uint32_t height,
                            std::uint64_t row_stride = 0) {
  if (format.raw) {
    const std::uint64_t row = row_stride != 0 ? row_stride : std::uint64_t{width} * static_cast<std::uint64_t>(raw_bytes(*format.raw));
    return row * height;
  }
  if (format.block) return surface_bytes(*format.block, width, height, format.astc_w, format.astc_h);
  return 0;
}

constexpr std::array<std::array<int, 2>, 14> astc_footprints = {{
    {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12},
}};

TexFormat astc_format(std::size_t index) {
  TexFormat f;
  f.block = BlockFormat::ASTC;
  f.astc_w = astc_footprints[index][0];
  f.astc_h = astc_footprints[index][1];
  return f;
}
TexFormat raw_format(Raw r) {
  TexFormat f;
  f.raw = r;
  return f;
}
TexFormat block_format(BlockFormat b) {
  TexFormat f;
  f.block = b;
  return f;
}
TexFormat unsupported_format(std::string name) {
  TexFormat f;
  f.unsupported = std::move(name);
  return f;
}

// --------------------------------------------------------------------------- KTX 1

constexpr std::uint8_t ktx1_id[12] = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::uint8_t ktx2_id[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

TexFormat gl_format(std::uint32_t type, std::uint32_t format, std::uint32_t internal) {
  if (type == 0) {
    switch (internal) {
      case 0x83F0: case 0x83F1: case 0x8C4C: case 0x8C4D: return block_format(BlockFormat::BC1);
      case 0x83F2: case 0x8C4E: return block_format(BlockFormat::BC2);
      case 0x83F3: case 0x8C4F: return block_format(BlockFormat::BC3);
      case 0x8DBB: return block_format(BlockFormat::BC4);
      case 0x8DBC: return block_format(BlockFormat::BC4Signed);
      case 0x8DBD: return block_format(BlockFormat::BC5);
      case 0x8DBE: return block_format(BlockFormat::BC5Signed);
      case 0x8E8C: case 0x8E8D: return block_format(BlockFormat::BC7);
      case 0x8E8E: case 0x8E8F: return unsupported_format("BC6H");
      case 0x8D64: return block_format(BlockFormat::ETC1);
      case 0x9274: case 0x9275: return block_format(BlockFormat::ETC2);
      case 0x9276: case 0x9277: return block_format(BlockFormat::ETC2A1);
      case 0x9278: case 0x9279: return block_format(BlockFormat::ETC2EAC);
      case 0x9270: return block_format(BlockFormat::EACR11);
      case 0x9271: return block_format(BlockFormat::EACR11Signed);
      case 0x9272: return block_format(BlockFormat::EACRG11);
      case 0x9273: return block_format(BlockFormat::EACRG11Signed);
      case 0x8C00: case 0x8C01: case 0x8C02: case 0x8C03: return unsupported_format("PVRTC");
      default: break;
    }
    if (internal >= 0x93B0 && internal <= 0x93BD) return astc_format(internal - 0x93B0);
    if (internal >= 0x93D0 && internal <= 0x93DD) return astc_format(internal - 0x93D0);
    return unsupported_format("this compressed GL format");
  }
  constexpr std::uint32_t red = 0x1903, rg = 0x8227, rgb = 0x1907, bgr = 0x80E0, rgba = 0x1908, bgra = 0x80E1,
                          alpha = 0x1906, lum = 0x1909, lum_alpha = 0x190A;
  switch (type) {
    case 0x1401:  // UNSIGNED_BYTE
      switch (format) {
        case red: return raw_format(Raw::R8);
        case rg: return raw_format(Raw::RG8);
        case rgb: return raw_format(Raw::RGB8);
        case bgr: return raw_format(Raw::BGR8);
        case rgba: return raw_format(Raw::RGBA8);
        case bgra: return raw_format(Raw::BGRA8);
        case alpha: return raw_format(Raw::A8);
        case lum: return raw_format(Raw::L8);
        case lum_alpha: return raw_format(Raw::LA8);
        default: break;
      }
      break;
    case 0x1403:  // UNSIGNED_SHORT
      switch (format) {
        case red: case lum: return raw_format(Raw::R16);
        case rg: return raw_format(Raw::RG16);
        case rgb: return raw_format(Raw::RGB16);
        case rgba: return raw_format(Raw::RGBA16);
        default: break;
      }
      break;
    case 0x140B:  // HALF_FLOAT
    case 0x8D61:  // HALF_FLOAT_OES
      switch (format) {
        case red: case lum: return raw_format(Raw::R16F);
        case rg: return raw_format(Raw::RG16F);
        case rgb: return raw_format(Raw::RGB16F);
        case rgba: return raw_format(Raw::RGBA16F);
        default: break;
      }
      break;
    case 0x1406:  // FLOAT
      switch (format) {
        case red: case lum: return raw_format(Raw::R32F);
        case rg: return raw_format(Raw::RG32F);
        case rgb: return raw_format(Raw::RGB32F);
        case rgba: return raw_format(Raw::RGBA32F);
        default: break;
      }
      break;
    case 0x8363: return raw_format(Raw::RGB565);
    case 0x8033: return raw_format(Raw::RGBA4444);
    case 0x8034: return raw_format(Raw::RGBA5551);
    case 0x8368: return raw_format(Raw::A2B10G10R10);
    case 0x8C3B: return raw_format(Raw::B10G11R11F);
    case 0x8C3E: return raw_format(Raw::E5B9G9R9);
    default: break;
  }
  return unsupported_format("this uncompressed GL format");
}

Result<ir::Image> read_ktx(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (ctx.input.size() < 64 || std::memcmp(ctx.input.data(), ktx1_id, 12) != 0) return fail(Status::Decode, "not a KTX file");
  (void)r.skip(12);
  const std::uint32_t endian = r.u32le();
  if (endian != 0x04030201 && endian != 0x01020304) return fail(Status::Decode, "KTX endianness marker is invalid");
  const bool swap = endian == 0x01020304;
  const auto u32 = [&]() { return swap ? r.u32be() : r.u32le(); };
  const std::uint32_t gl_type = u32();
  const std::uint32_t type_size = u32();
  const std::uint32_t gl_format_value = u32();
  const std::uint32_t internal = u32();
  (void)u32();  // base internal format
  const std::uint32_t width = u32();
  std::uint32_t height = u32();
  const std::uint32_t depth = u32();
  (void)u32();  // array elements
  (void)u32();  // faces
  (void)u32();  // mip levels
  const std::uint32_t kv_bytes = u32();
  if (!r.ok()) return fail(Status::Decode, "KTX header is truncated");
  if (height == 0) height = 1;  // 1D texture
  if (depth > 1) ctx.warn("Only the first slice of the 3D KTX texture was read.");
  CVT_TRY(image::check_size(ctx, width, height));
  if (kv_bytes > r.remaining()) return fail(Status::Decode, "KTX key/value data is truncated");
  (void)r.skip(kv_bytes);
  const std::uint32_t image_size = u32();
  if (!r.ok()) return fail(Status::Decode, "KTX image data is missing");
  const TexFormat format = gl_format(gl_type, gl_format_value, internal);
  std::uint64_t row_stride = 0;
  if (format.raw) row_stride = (std::uint64_t{width} * static_cast<std::uint64_t>(raw_bytes(*format.raw)) + 3) / 4 * 4;
  const std::uint64_t needed = texture_bytes(format, width, height, row_stride);
  if (format.unsupported.empty() && (needed > r.remaining() || image_size < needed)) {
    return fail(Status::Decode, "KTX image data is truncated");
  }
  ByteView data = r.peek_rest();
  Bytes swapped;
  if (swap && format.raw && type_size > 1) {
    swapped.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(needed, data.size())));
    for (std::size_t i = 0; i + type_size <= swapped.size(); i += type_size) {
      std::reverse(swapped.begin() + static_cast<std::ptrdiff_t>(i), swapped.begin() + static_cast<std::ptrdiff_t>(i + type_size));
    }
    data = swapped;
  }
  return decode_texture(format, data, width, height, row_stride, ctx);
}

// --------------------------------------------------------------------------- KTX 2

TexFormat vk_format(std::uint32_t vk) {
  switch (vk) {
    case 9: case 15: return raw_format(Raw::R8);
    case 16: case 22: return raw_format(Raw::RG8);
    case 23: case 29: return raw_format(Raw::RGB8);
    case 30: case 36: return raw_format(Raw::BGR8);
    case 37: case 43: return raw_format(Raw::RGBA8);
    case 44: case 50: return raw_format(Raw::BGRA8);
    case 70: return raw_format(Raw::R16);
    case 77: return raw_format(Raw::RG16);
    case 84: return raw_format(Raw::RGB16);
    case 91: return raw_format(Raw::RGBA16);
    case 76: return raw_format(Raw::R16F);
    case 83: return raw_format(Raw::RG16F);
    case 90: return raw_format(Raw::RGB16F);
    case 97: return raw_format(Raw::RGBA16F);
    case 100: return raw_format(Raw::R32F);
    case 103: return raw_format(Raw::RG32F);
    case 106: return raw_format(Raw::RGB32F);
    case 109: return raw_format(Raw::RGBA32F);
    case 4: return raw_format(Raw::RGB565);
    case 5: return raw_format(Raw::BGR565);
    case 2: return raw_format(Raw::RGBA4444);
    case 6: return raw_format(Raw::RGBA5551);
    case 64: return raw_format(Raw::A2B10G10R10);
    case 122: return raw_format(Raw::B10G11R11F);
    case 123: return raw_format(Raw::E5B9G9R9);
    case 131: case 132: case 133: case 134: return block_format(BlockFormat::BC1);
    case 135: case 136: return block_format(BlockFormat::BC2);
    case 137: case 138: return block_format(BlockFormat::BC3);
    case 139: return block_format(BlockFormat::BC4);
    case 140: return block_format(BlockFormat::BC4Signed);
    case 141: return block_format(BlockFormat::BC5);
    case 142: return block_format(BlockFormat::BC5Signed);
    case 143: case 144: return unsupported_format("BC6H");
    case 145: case 146: return block_format(BlockFormat::BC7);
    case 147: case 148: return block_format(BlockFormat::ETC2);
    case 149: case 150: return block_format(BlockFormat::ETC2A1);
    case 151: case 152: return block_format(BlockFormat::ETC2EAC);
    case 153: return block_format(BlockFormat::EACR11);
    case 154: return block_format(BlockFormat::EACR11Signed);
    case 155: return block_format(BlockFormat::EACRG11);
    case 156: return block_format(BlockFormat::EACRG11Signed);
    case 0: return unsupported_format("Basis Universal (or undefined-format)");
    default: break;
  }
  if (vk >= 157 && vk <= 184) return astc_format((vk - 157) / 2);
  if (vk >= 1000066000 && vk <= 1000066013) return unsupported_format("ASTC HDR");
  if (vk >= 1000054000 && vk <= 1000054007) return unsupported_format("PVRTC");
  return unsupported_format("this Vulkan format");
}

Result<ir::Image> read_ktx2(ReadContext& ctx) {
  if (ctx.input.size() < 104 || std::memcmp(ctx.input.data(), ktx2_id, 12) != 0) {
    return fail(Status::Decode, "not a KTX2 file");
  }
  util::ByteReader r(ctx.input);
  (void)r.skip(12);
  const std::uint32_t vk = r.u32le();
  (void)r.u32le();  // type size
  const std::uint32_t width = r.u32le();
  std::uint32_t height = r.u32le();
  const std::uint32_t depth = r.u32le();
  (void)r.u32le();  // layers
  (void)r.u32le();  // faces
  const std::uint32_t levels = std::max(1u, r.u32le());
  const std::uint32_t scheme = r.u32le();
  (void)r.skip(16);  // DFD and key/value index
  (void)r.skip(16);  // supercompression global data
  const std::uint64_t offset = r.u64le();
  const std::uint64_t length = r.u64le();
  const std::uint64_t uncompressed = r.u64le();
  if (!r.ok() || levels > 32) return fail(Status::Decode, "KTX2 header is truncated");
  if (height == 0) height = 1;
  if (depth > 1) ctx.warn("Only the first slice of the 3D KTX2 texture was read.");
  CVT_TRY(image::check_size(ctx, width, height));
  if (offset > ctx.input.size() || length > ctx.input.size() - offset) return fail(Status::Decode, "KTX2 level data is truncated");
  const TexFormat format = vk_format(vk);
  if (!format.unsupported.empty()) return decode_texture(format, {}, width, height, 0, ctx);
  ByteView level = ctx.input.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
  Bytes inflated;
  switch (scheme) {
    case 0: break;
    case 3: {
      const std::uint64_t needed = texture_bytes(format, width, height);
      if (uncompressed < needed) return fail(Status::Decode, "KTX2 level is too small");
      CVT_TRY(ctx.limits.check_decompressed(uncompressed));
      CVT_TRY_ASSIGN(inflated, util::zlib_decompress(level, std::min(uncompressed, ctx.limits.max_decompressed_bytes)));
      level = inflated;
      break;
    }
    case 1: return fail(Status::UnsupportedConversion, "KTX2 Basis Universal (BasisLZ) textures are not supported");
    case 2: return fail(Status::UnsupportedConversion, "KTX2 Zstandard supercompression is not supported");
    default: return fail(Status::UnsupportedConversion, "unknown KTX2 supercompression scheme {}", scheme);
  }
  return decode_texture(format, level, width, height, 0, ctx);
}

Result<Bytes> write_ktx2(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const image::WriteImage prepared, image::prepare_write(source, ctx));
  image::warn_if_animated(*prepared, ctx, "KTX2");
  const ir::Frame& frame = prepared->frames.front();
  const std::string supercompression = ctx.options.get_enum("ktx2_supercompression", "none", {"none", "zlib"});
  const ByteView pixels = as_bytes(std::span<const std::uint8_t>(frame.rgba));
  Bytes packed;
  ByteView level = pixels;
  if (supercompression == "zlib") {
    CVT_TRY_ASSIGN(packed, util::zlib_compress(pixels, std::max(1, options::resolved_compression_level(ctx.options))));
    level = packed;
  }
  // Data format descriptor: one basic block, RGBSDA, BT.709 primaries, sRGB
  // transfer, four 8-bit samples (alpha linear).
  util::ByteWriter dfd;
  dfd.u32le(92).u32le(0).u16le(2).u16le(88);
  dfd.u8(1).u8(1).u8(2).u8(0);
  dfd.u32le(0);                 // texel block dimensions 1x1x1x1
  dfd.u32le(4).u32le(0);        // bytes per plane
  const std::uint8_t channels[4] = {0, 1, 2, 15 | 0x10};
  for (int c = 0; c < 4; ++c) {
    dfd.u16le(static_cast<std::uint16_t>(8 * c)).u8(7).u8(channels[c]);
    dfd.u32le(0).u32le(0).u32le(255);
  }
  const std::uint32_t dfd_offset = 104;
  const std::uint64_t data_offset = (dfd_offset + dfd.size() + 3) / 4 * 4;
  util::ByteWriter out;
  out.bytes(as_bytes(std::span<const std::uint8_t>(ktx2_id)));
  out.u32le(43).u32le(1).u32le(frame.width).u32le(frame.height).u32le(0).u32le(0).u32le(1).u32le(1);
  out.u32le(supercompression == "zlib" ? 3 : 0);
  out.u32le(dfd_offset).u32le(static_cast<std::uint32_t>(dfd.size())).u32le(0).u32le(0).u64le(0).u64le(0);
  out.u64le(data_offset).u64le(level.size()).u64le(pixels.size());
  out.bytes(dfd.view());
  while (out.size() < data_offset) out.u8(0);
  out.bytes(level);
  return out.take();
}

// --------------------------------------------------------------------------- PVR v3

TexFormat pvr_format(std::uint64_t pixel_format, std::uint32_t channel_type) {
  const std::uint32_t high = static_cast<std::uint32_t>(pixel_format >> 32);
  const std::uint32_t low = static_cast<std::uint32_t>(pixel_format);
  if (high == 0) {
    switch (low) {
      case 0: case 1: case 2: case 3: case 4: case 5: return unsupported_format("PVRTC");
      case 6: return block_format(BlockFormat::ETC1);
      case 7: return block_format(BlockFormat::BC1);
      case 8: case 9: return block_format(BlockFormat::BC2);
      case 10: case 11: return block_format(BlockFormat::BC3);
      case 12: return block_format(BlockFormat::BC4);
      case 13: return block_format(BlockFormat::BC5);
      case 14: return unsupported_format("BC6H");
      case 15: return block_format(BlockFormat::BC7);
      case 22: return block_format(BlockFormat::ETC2);
      case 23: return block_format(BlockFormat::ETC2EAC);
      case 24: return block_format(BlockFormat::ETC2A1);
      case 25: return block_format(BlockFormat::EACR11);
      case 26: return block_format(BlockFormat::EACRG11);
      default: break;
    }
    if (low >= 27 && low <= 40) return astc_format(low - 27);
    return unsupported_format("this PVR compressed format");
  }
  // Uncompressed: up to four channel names (low bytes) with bit counts (high bytes).
  std::string names;
  std::array<int, 4> bits{};
  for (int i = 0; i < 4; ++i) {
    const char name = static_cast<char>((low >> (8 * i)) & 0xFF);
    if (name == 0) break;
    names.push_back(name);
    bits[static_cast<std::size_t>(i)] = static_cast<int>((high >> (8 * i)) & 0xFF);
  }
  const bool all8 = std::all_of(bits.begin(), bits.begin() + static_cast<std::ptrdiff_t>(names.size()), [](int b) { return b == 8; });
  const bool all16 = std::all_of(bits.begin(), bits.begin() + static_cast<std::ptrdiff_t>(names.size()), [](int b) { return b == 16; });
  const bool all32 = std::all_of(bits.begin(), bits.begin() + static_cast<std::ptrdiff_t>(names.size()), [](int b) { return b == 32; });
  const bool unorm = channel_type == 0 || channel_type == 2 || channel_type == 4 || channel_type == 6;
  const bool is_float = channel_type == 12 || channel_type == 13;
  if (all8 && unorm) {
    if (names == "rgba") return raw_format(Raw::RGBA8);
    if (names == "bgra") return raw_format(Raw::BGRA8);
    if (names == "rgb") return raw_format(Raw::RGB8);
    if (names == "bgr") return raw_format(Raw::BGR8);
    if (names == "abgr") return raw_format(Raw::ABGR8);
    if (names == "rg") return raw_format(Raw::RG8);
    if (names == "r" || names == "l" || names == "i") return raw_format(Raw::L8);
    if (names == "la") return raw_format(Raw::LA8);
    if (names == "a") return raw_format(Raw::A8);
  }
  if (all16 && unorm) {
    if (names == "rgba") return raw_format(Raw::RGBA16);
    if (names == "rgb") return raw_format(Raw::RGB16);
    if (names == "rg") return raw_format(Raw::RG16);
    if (names == "r" || names == "l") return raw_format(Raw::R16);
  }
  if (all16 && is_float) {
    if (names == "rgba") return raw_format(Raw::RGBA16F);
    if (names == "rgb") return raw_format(Raw::RGB16F);
    if (names == "rg") return raw_format(Raw::RG16F);
    if (names == "r" || names == "l") return raw_format(Raw::R16F);
  }
  if (all32 && is_float) {
    if (names == "rgba") return raw_format(Raw::RGBA32F);
    if (names == "rgb") return raw_format(Raw::RGB32F);
    if (names == "rg") return raw_format(Raw::RG32F);
    if (names == "r" || names == "l") return raw_format(Raw::R32F);
  }
  return unsupported_format("this PVR pixel layout");
}

Result<ir::Image> read_pvr(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (r.u32le() != 0x03525650) return fail(Status::Decode, "not a PVR v3 file");
  (void)r.u32le();  // flags
  const std::uint64_t pixel_format = r.u64le();
  (void)r.u32le();  // colour space
  const std::uint32_t channel_type = r.u32le();
  const std::uint32_t height = r.u32le();
  const std::uint32_t width = r.u32le();
  const std::uint32_t depth = r.u32le();
  (void)r.u32le();  // surfaces
  (void)r.u32le();  // faces
  (void)r.u32le();  // mip levels
  const std::uint32_t meta = r.u32le();
  if (!r.ok() || meta > r.remaining()) return fail(Status::Decode, "PVR header is truncated");
  if (depth > 1) ctx.warn("Only the first slice of the 3D PVR texture was read.");
  (void)r.skip(meta);
  const TexFormat format = pvr_format(pixel_format, channel_type);
  return decode_texture(format, r.peek_rest(), width, height, 0, ctx);
}

// --------------------------------------------------------------------------- .astc

Result<ir::Image> read_astc(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (r.u32le() != 0x5CA1AB13) return fail(Status::Decode, "not an ASTC file");
  const int bw = r.u8();
  const int bh = r.u8();
  const int bd = r.u8();
  const std::uint32_t width = r.u24le();
  const std::uint32_t height = r.u24le();
  const std::uint32_t depth = r.u24le();
  if (!r.ok()) return fail(Status::Decode, "ASTC header is truncated");
  if (bd != 1) return fail(Status::UnsupportedConversion, "3D ASTC textures are not supported");
  if (depth > 1) ctx.warn("Only the first slice of the ASTC texture was read.");
  const auto valid = std::find(astc_footprints.begin(), astc_footprints.end(), std::array<int, 2>{bw, bh});
  if (valid == astc_footprints.end()) return fail(Status::Decode, "ASTC block size {}x{} is invalid", bw, bh);
  TexFormat format = astc_format(static_cast<std::size_t>(valid - astc_footprints.begin()));
  return decode_texture(format, r.peek_rest(), width, height, 0, ctx);
}

// Header checks for detection: compressed texture payloads can look like chains
// of MPEG audio frames to generic sniffers, so a validated header must win.
int sniff_pvr(ByteView data, std::string_view /*filename*/) {
  util::ByteReader r(data);
  if (r.u32le() != 0x03525650) return 0;
  (void)r.u32le();
  (void)r.u64le();
  (void)r.u32le();
  (void)r.u32le();
  const std::uint32_t height = r.u32le();
  const std::uint32_t width = r.u32le();
  const std::uint32_t depth = r.u32le();
  if (!r.ok() || width == 0 || height == 0 || depth == 0) return 0;
  return 96;
}

int sniff_astc(ByteView data, std::string_view /*filename*/) {
  util::ByteReader r(data);
  if (r.u32le() != 0x5CA1AB13) return 0;
  const int bw = r.u8();
  const int bh = r.u8();
  const int bd = r.u8();
  const std::uint32_t width = r.u24le();
  const std::uint32_t height = r.u24le();
  if (!r.ok() || width == 0 || height == 0 || bd < 1 || bd > 6) return 0;
  const auto valid = std::find(astc_footprints.begin(), astc_footprints.end(), std::array<int, 2>{bw, bh});
  return valid == astc_footprints.end() && bd == 1 ? 0 : 96;
}

// --------------------------------------------------------------------------- VTF

TexFormat vtf_format(std::int32_t f) {
  switch (f) {
    case 0: return raw_format(Raw::RGBA8);
    case 1: return raw_format(Raw::ABGR8);
    case 2: case 9: return raw_format(Raw::RGB8);
    case 3: case 10: return raw_format(Raw::BGR8);
    case 5: return raw_format(Raw::L8);
    case 6: return raw_format(Raw::LA8);
    case 8: return raw_format(Raw::A8);
    case 12: return raw_format(Raw::BGRA8);
    case 13: case 20: return block_format(BlockFormat::BC1);
    case 14: return block_format(BlockFormat::BC2);
    case 15: return block_format(BlockFormat::BC3);
    case 16: return raw_format(Raw::BGRX8);
    case 21: return raw_format(Raw::BGRA5551);
    case 22: return raw_format(Raw::RG8);
    case 24: return raw_format(Raw::RGBA16F);
    case 25: return raw_format(Raw::RGBA16);
    default: return unsupported_format("this VTF image format");
  }
}

Result<ir::Image> read_vtf(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume(std::string_view("VTF\0", 4))) return fail(Status::Decode, "not a VTF file");
  const std::uint32_t major = r.u32le();
  const std::uint32_t minor = r.u32le();
  const std::uint32_t header_size = r.u32le();
  const std::uint32_t width = r.u16le();
  const std::uint32_t height = r.u16le();
  const std::uint32_t flags = r.u32le();
  const std::uint32_t frames = std::max<std::uint32_t>(1, r.u16le());
  const std::uint32_t first_frame = r.u16le();
  (void)r.skip(4 + 12 + 4 + 4);
  const auto high_format = static_cast<std::int32_t>(r.u32le());
  const std::uint32_t mips = std::max<std::uint32_t>(1, r.u8());
  const auto low_format = static_cast<std::int32_t>(r.u32le());
  const std::uint32_t low_w = r.u8();
  const std::uint32_t low_h = r.u8();
  std::uint32_t depth = 1;
  if (major == 7 && minor >= 2) depth = std::max<std::uint32_t>(1, r.u16le());
  if (!r.ok() || major != 7 || minor > 5) return fail(Status::Decode, "VTF header is invalid");
  CVT_TRY(image::check_size(ctx, width, height, frames));
  if (mips > 16) return fail(Status::Decode, "VTF mip count is invalid");
  const TexFormat format = vtf_format(high_format);
  if (!format.unsupported.empty()) return decode_texture(format, {}, width, height, 0, ctx);
  const std::uint32_t faces = (flags & 0x4000) ? ((minor < 5 && first_frame == 0xFFFF) ? 7 : 6) : 1;

  // Start of the high-resolution data.
  std::uint64_t start = 0;
  if (minor >= 3) {
    (void)r.seek(68);
    const std::uint32_t resources = r.u32le();
    if (!r.ok() || resources > 32) return fail(Status::Decode, "VTF resource table is invalid");
    (void)r.seek(80);
    bool found = false;
    for (std::uint32_t i = 0; i < resources; ++i) {
      const std::uint32_t tag = r.u24le();
      (void)r.u8();
      const std::uint32_t value = r.u32le();
      if (!r.ok()) return fail(Status::Decode, "VTF resource table is truncated");
      if (tag == 0x30) {
        start = value;
        found = true;
      }
    }
    if (!found) return fail(Status::Decode, "VTF file has no image data");
  } else {
    std::uint64_t low_size = 0;
    if (low_format != -1 && low_w > 0 && low_h > 0) {
      const TexFormat low = vtf_format(low_format);
      low_size = texture_bytes(low, low_w, low_h);
    }
    start = std::uint64_t{header_size} + low_size;
  }
  // Mips are stored smallest first; the full-size level comes last.
  std::uint64_t offset = start;
  std::uint64_t level_size = 0;
  for (std::uint32_t m = mips; m-- > 0;) {
    const std::uint32_t mw = std::max<std::uint32_t>(1, width >> m);
    const std::uint32_t mh = std::max<std::uint32_t>(1, height >> m);
    const std::uint32_t md = std::max<std::uint32_t>(1, depth >> m);
    level_size = texture_bytes(format, mw, mh);
    if (m > 0) offset += level_size * frames * faces * md;
  }
  const std::uint64_t frame_stride = level_size * faces * depth;
  if (offset > ctx.input.size() || frame_stride == 0) return fail(Status::Decode, "VTF image data is truncated");
  ir::Image image;
  for (std::uint32_t f = 0; f < frames; ++f) {
    const std::uint64_t at = offset + f * frame_stride;
    if (at + level_size > ctx.input.size()) {
      if (f == 0) return fail(Status::Decode, "VTF image data is truncated");
      ctx.warn("The VTF file is truncated; the remaining frames were skipped.");
      break;
    }
    CVT_TRY_ASSIGN(ir::Image one, decode_texture(format, ctx.input.subspan(static_cast<std::size_t>(at)), width, height, 0, ctx));
    if (frames > 1) one.frames[0].delay_ms = 100;
    if (high_format == 13 && (flags & 0x1000) == 0) {
      // Plain DXT1 carries no alpha in Source: the 3-colour mode's "transparent" texels are black.
      for (std::size_t i = 3; i < one.frames[0].rgba.size(); i += 4) one.frames[0].rgba[i] = 255;
    }
    if (f == 0) {
      image = std::move(one);
    } else {
      image.frames.push_back(std::move(one.frames[0]));
    }
  }
  if (image.frames.size() > 1) image.hdr.reset();
  return image;
}

void add_reader(Registry& registry, const char* id, image::ImageReader reader_fn) {
  ImageCodecSpec spec;
  spec.format_id = id;
  spec.read = image::reader(reader_fn);
  image::add_image_codec(registry, std::move(spec));
}

}  // namespace

void register_misc_texture(Registry& registry) {
  FormatInfo ktx;
  ktx.id = "ktx";
  ktx.name = "KTX texture";
  ktx.description = "Khronos texture container (OpenGL formats, BCn/ETC/ASTC compressed).";
  ktx.category = Category::Image;
  ktx.extensions = {"ktx"};
  ktx.mime = "image/ktx";
  ktx.popularity = 10;
  ktx.signatures = {Signature::at(0, "AB 'KTX 11' BB 0D 0A 1A 0A")};
  registry.add_format(std::move(ktx));

  FormatInfo ktx2;
  ktx2.id = "ktx2";
  ktx2.name = "KTX2 texture";
  ktx2.description = "Khronos texture container version 2 (Vulkan formats).";
  ktx2.category = Category::Image;
  ktx2.extensions = {"ktx2"};
  ktx2.mime = "image/ktx2";
  ktx2.popularity = 12;
  ktx2.signatures = {Signature::at(0, "AB 'KTX 20' BB 0D 0A 1A 0A")};
  registry.add_format(std::move(ktx2));

  FormatInfo pvr;
  pvr.id = "pvr";
  pvr.name = "PowerVR texture";
  pvr.description = "PowerVR SDK texture container, version 3.";
  pvr.category = Category::Image;
  pvr.extensions = {"pvr"};
  pvr.mime = "image/x-pvr";
  pvr.popularity = 6;
  pvr.signatures = {Signature::at(0, "'PVR' 03")};
  pvr.sniff = sniff_pvr;
  registry.add_format(std::move(pvr));

  FormatInfo astc;
  astc.id = "astc";
  astc.name = "ASTC texture";
  astc.description = "Adaptive Scalable Texture Compression image (ARM .astc file).";
  astc.category = Category::Image;
  astc.extensions = {"astc"};
  astc.mime = "image/astc";
  astc.popularity = 6;
  astc.signatures = {Signature::at(0, "13 AB A1 5C")};
  astc.sniff = sniff_astc;
  registry.add_format(std::move(astc));

  FormatInfo vtf;
  vtf.id = "vtf";
  vtf.name = "Valve texture";
  vtf.description = "Valve Texture Format used by Source engine games.";
  vtf.category = Category::Image;
  vtf.extensions = {"vtf"};
  vtf.mime = "image/x-vtf";
  vtf.popularity = 8;
  vtf.signatures = {Signature::at(0, "'VTF' 00 07 00 00 00")};
  registry.add_format(std::move(vtf));

  add_reader(registry, "ktx", read_ktx);
  add_reader(registry, "pvr", read_pvr);
  add_reader(registry, "astc", read_astc);
  add_reader(registry, "vtf", read_vtf);

  ImageCodecSpec ktx2_codec;
  ktx2_codec.format_id = "ktx2";
  ktx2_codec.read = image::reader(read_ktx2);
  ktx2_codec.write = image::writer(write_ktx2);
  ktx2_codec.write_options = image::with(
      image::lossless_writer_options(),
      {OptionSpec::choice("ktx2_supercompression", "Supercompression", "none", {"none", "zlib"},
                          "zlib makes smaller files; some older KTX2 readers only accept none")});
  image::add_image_codec(registry, std::move(ktx2_codec));
}

}  // namespace cvt::image_modern
