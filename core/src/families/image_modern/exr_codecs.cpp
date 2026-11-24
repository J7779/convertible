// Convertible core - image_modern family: OpenEXR chunk codecs.
//
// Half floats, chunk layout arithmetic, and the simpler compressions:
// RLE, ZIPS/ZIP (byte predictor + zlib), PXR24 (24-bit float + zlib) and
// B44/B44A (fixed-rate 4x4 half blocks). PIZ lives in exr_piz.cpp and DWA
// in exr_dwa.cpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "util/bytes.hpp"
#include "families/image_modern/exr_internal.hpp"
#include "util/deflate.hpp"

namespace cvt::image_modern::exr {

// --------------------------------------------------------------------------- half floats

namespace {

std::array<float, 65536> make_half_table() {
  std::array<float, 65536> table{};
  for (std::uint32_t h = 0; h < 65536; ++h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = h & 0x3FFu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
      const float magnitude = std::ldexp(static_cast<float>(mantissa), -24);  // exact
      table[h] = sign != 0 ? -magnitude : magnitude;
      continue;
    }
    if (exponent == 31) {
      bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
      bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    table[h] = std::bit_cast<float>(bits);
  }
  return table;
}

}  // namespace

float half_to_float(std::uint16_t bits) noexcept {
  static const std::array<float, 65536> table = make_half_table();
  return table[bits];
}

std::uint16_t float_to_half(float value) noexcept {
  const std::uint32_t v = std::bit_cast<std::uint32_t>(value);
  std::uint32_t ui = v & ~0x80000000u;
  auto result = static_cast<std::uint16_t>((v >> 16) & 0x8000u);
  if (ui >= 0x38800000u) {
    if (ui >= 0x7F800000u) {
      result = static_cast<std::uint16_t>(result | 0x7C00u);
      if (ui == 0x7F800000u) return result;
      const std::uint32_t m = (ui & 0x7FFFFFu) >> 13;
      return static_cast<std::uint16_t>(result | m | (m == 0 ? 1u : 0u));
    }
    if (ui > 0x477FEFFFu) return static_cast<std::uint16_t>(result | 0x7C00u);
    ui -= 0x38000000u;
    ui = (ui + 0x00000FFFu + ((ui >> 13) & 1u)) >> 13;
    return static_cast<std::uint16_t>(result | ui);
  }
  if (ui < 0x33000001u) return result;  // zero or too small: signed zero
  const std::uint32_t e = ui >> 23;
  const std::uint32_t shift = 0x7Eu - e;
  const std::uint32_t m = 0x800000u | (ui & 0x7FFFFFu);
  const std::uint32_t r = m << (32 - shift);
  result = static_cast<std::uint16_t>(result | (m >> shift));
  if (r > 0x80000000u || (r == 0x80000000u && (result & 1u) != 0)) ++result;
  return result;
}

// --------------------------------------------------------------------------- layout

namespace {

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
  const std::int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/// Multiples of `s` in [a, a + n).
std::int32_t count_samples(std::int64_t a, std::int64_t n, std::int64_t s) noexcept {
  if (n <= 0) return 0;
  return static_cast<std::int32_t>(floor_div(a + n - 1, s) - floor_div(a - 1, s));
}

}  // namespace

void finish_layout(ChunkLayout& layout) {
  layout.raw_size = 0;
  for (ChunkChannel& channel : layout.channels) {
    channel.width = count_samples(layout.x0, layout.width, channel.x_sampling);
    channel.height = count_samples(layout.y0, layout.height, channel.y_sampling);
    layout.raw_size += std::uint64_t(static_cast<std::uint32_t>(channel.width)) *
                       static_cast<std::uint32_t>(channel.height) * pixel_size(channel.type);
  }
}

// --------------------------------------------------------------------------- ZIP / RLE helpers

Bytes zip_prepare(ByteView raw) {
  const std::size_t n = raw.size();
  Bytes tmp(n);
  const std::size_t half = (n + 1) / 2;
  for (std::size_t i = 0; i < n; ++i) tmp[(i & 1) == 0 ? i / 2 : half + i / 2] = raw[i];
  if (n == 0) return tmp;
  int previous = std::to_integer<int>(tmp[0]);
  for (std::size_t i = 1; i < n; ++i) {
    const int current = std::to_integer<int>(tmp[i]);
    tmp[i] = static_cast<std::byte>((current - previous + 128 + 256) & 0xFF);
    previous = current;
  }
  return tmp;
}

Bytes zip_reconstruct(ByteView data) {
  const std::size_t n = data.size();
  std::vector<std::uint8_t> tmp(n);
  for (std::size_t i = 0; i < n; ++i) tmp[i] = std::to_integer<std::uint8_t>(data[i]);
  for (std::size_t i = 1; i < n; ++i) tmp[i] = static_cast<std::uint8_t>(tmp[i - 1] + tmp[i] - 128);
  Bytes out(n);
  const std::size_t half = (n + 1) / 2;
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::byte>(tmp[(i & 1) == 0 ? i / 2 : half + i / 2]);
  return out;
}

Bytes rle_encode(ByteView data) {
  constexpr std::ptrdiff_t min_run = 3;
  constexpr std::ptrdiff_t max_run = 127;
  Bytes out;
  out.reserve(data.size() + data.size() / 64 + 2);
  const std::ptrdiff_t end = static_cast<std::ptrdiff_t>(data.size());
  std::ptrdiff_t run_start = 0;
  std::ptrdiff_t run_end = 1;
  while (run_start < end) {
    while (run_end < end && data[static_cast<std::size_t>(run_start)] == data[static_cast<std::size_t>(run_end)] &&
           run_end - run_start - 1 < max_run) {
      ++run_end;
    }
    if (run_end - run_start >= min_run) {
      out.push_back(static_cast<std::byte>(run_end - run_start - 1));
      out.push_back(data[static_cast<std::size_t>(run_start)]);
      run_start = run_end;
    } else {
      const auto at = [&](std::ptrdiff_t i) { return data[static_cast<std::size_t>(i)]; };
      while (run_end < end &&
             ((run_end + 1 >= end || at(run_end) != at(run_end + 1)) ||
              (run_end + 2 >= end || at(run_end + 1) != at(run_end + 2))) &&
             run_end - run_start < max_run) {
        ++run_end;
      }
      out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(-(run_end - run_start))));
      while (run_start < run_end) out.push_back(data[static_cast<std::size_t>(run_start++)]);
    }
    ++run_end;
  }
  return out;
}

Result<Bytes> rle_decode(ByteView data, std::uint64_t expected) {
  Bytes out;
  out.reserve(static_cast<std::size_t>(expected));
  std::size_t pos = 0;
  while (pos < data.size()) {
    const auto count = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(data[pos++]));
    if (count < 0) {
      const auto n = static_cast<std::size_t>(-static_cast<int>(count));
      if (n > data.size() - pos || out.size() + n > expected) return fail(Status::Decode, "corrupt RLE data");
      out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(pos),
                 data.begin() + static_cast<std::ptrdiff_t>(pos + n));
      pos += n;
    } else {
      const auto n = static_cast<std::size_t>(count) + 1;
      if (pos >= data.size() || out.size() + n > expected) return fail(Status::Decode, "corrupt RLE data");
      out.insert(out.end(), n, data[pos++]);
    }
  }
  if (out.size() != expected) return fail(Status::Decode, "RLE data is truncated");
  return out;
}

Result<Bytes> zlib_exact(ByteView data, std::uint64_t expected) {
  auto out = util::zlib_decompress(data, expected);
  if (!out) return fail(Status::Decode, "corrupt zlib data: {}", out.error().message);
  if (out->size() != expected) return fail(Status::Decode, "zlib data is truncated");
  return out;
}

// --------------------------------------------------------------------------- PXR24

namespace {

Result<Bytes> pxr24_decompress(const ChunkLayout& layout, ByteView packed) {
  std::uint64_t expected = 0;
  for (std::int32_t line = 0; line < layout.height; ++line) {
    for (const ChunkChannel& channel : layout.channels) {
      if (!ChunkLayout::has_line(channel, std::int64_t{layout.y0} + line)) continue;
      const std::uint64_t bytes = channel.type == PixelType::Half ? 2 : channel.type == PixelType::Float ? 3 : 4;
      expected += bytes * static_cast<std::uint32_t>(channel.width);
    }
  }
  CVT_TRY_ASSIGN(const Bytes tmp, zlib_exact(packed, expected));
  Bytes out(static_cast<std::size_t>(layout.raw_size));
  const auto* in = reinterpret_cast<const std::uint8_t*>(tmp.data());
  std::byte* write = out.data();
  for (std::int32_t line = 0; line < layout.height; ++line) {
    for (const ChunkChannel& channel : layout.channels) {
      if (!ChunkLayout::has_line(channel, std::int64_t{layout.y0} + line)) continue;
      const auto n = static_cast<std::size_t>(channel.width);
      std::uint32_t pixel = 0;
      switch (channel.type) {
        case PixelType::Uint: {
          const std::uint8_t* p0 = in;
          const std::uint8_t* p1 = p0 + n;
          const std::uint8_t* p2 = p1 + n;
          const std::uint8_t* p3 = p2 + n;
          for (std::size_t j = 0; j < n; ++j) {
            const std::uint32_t diff = (std::uint32_t{p0[j]} << 24) | (std::uint32_t{p1[j]} << 16) |
                                       (std::uint32_t{p2[j]} << 8) | p3[j];
            pixel += diff;
            store32(write, pixel);
            write += 4;
          }
          in += 4 * n;
          break;
        }
        case PixelType::Half: {
          const std::uint8_t* p0 = in;
          const std::uint8_t* p1 = p0 + n;
          for (std::size_t j = 0; j < n; ++j) {
            const std::uint32_t diff = (std::uint32_t{p0[j]} << 8) | p1[j];
            pixel += diff;
            store16(write, static_cast<std::uint16_t>(pixel));
            write += 2;
          }
          in += 2 * n;
          break;
        }
        case PixelType::Float: {
          const std::uint8_t* p0 = in;
          const std::uint8_t* p1 = p0 + n;
          const std::uint8_t* p2 = p1 + n;
          for (std::size_t j = 0; j < n; ++j) {
            const std::uint32_t diff = (std::uint32_t{p0[j]} << 24) | (std::uint32_t{p1[j]} << 16) |
                                       (std::uint32_t{p2[j]} << 8);
            pixel += diff;
            store32(write, pixel);
            write += 4;
          }
          in += 3 * n;
          break;
        }
      }
    }
  }
  return out;
}

// --------------------------------------------------------------------------- B44

/// exp(x / 8) for the "perceptually linear" B44 channels.
std::uint16_t b44_from_log(std::uint16_t x) noexcept {
  if ((x & 0x7C00u) == 0x7C00u) return 0;
  if (x >= 0x558Cu && x < 0x8000u) return 0x7BFFu;
  return float_to_half(std::exp(half_to_float(x) / 8.0f));
}

void unpack14(const std::uint8_t b[14], std::uint16_t s[16]) noexcept {
  s[0] = static_cast<std::uint16_t>((b[0] << 8) | b[1]);
  const std::uint32_t shift = b[2] >> 2;
  const std::uint32_t bias = 0x20u << shift;
  const auto step = [&](std::uint16_t base, std::uint32_t bits) {
    return static_cast<std::uint16_t>(std::uint32_t{base} + ((bits & 0x3Fu) << shift) - bias);
  };
  s[4] = step(s[0], (std::uint32_t{b[2]} << 4) | (b[3] >> 4));
  s[8] = step(s[4], (std::uint32_t{b[3]} << 2) | (b[4] >> 6));
  s[12] = step(s[8], b[4]);
  s[1] = step(s[0], b[5] >> 2);
  s[5] = step(s[4], (std::uint32_t{b[5]} << 4) | (b[6] >> 4));
  s[9] = step(s[8], (std::uint32_t{b[6]} << 2) | (b[7] >> 6));
  s[13] = step(s[12], b[7]);
  s[2] = step(s[1], b[8] >> 2);
  s[6] = step(s[5], (std::uint32_t{b[8]} << 4) | (b[9] >> 4));
  s[10] = step(s[9], (std::uint32_t{b[9]} << 2) | (b[10] >> 6));
  s[14] = step(s[13], b[10]);
  s[3] = step(s[2], b[11] >> 2);
  s[7] = step(s[6], (std::uint32_t{b[11]} << 4) | (b[12] >> 4));
  s[11] = step(s[10], (std::uint32_t{b[12]} << 2) | (b[13] >> 6));
  s[15] = step(s[14], b[13]);
  for (int i = 0; i < 16; ++i) {
    s[i] = (s[i] & 0x8000u) != 0 ? static_cast<std::uint16_t>(s[i] & 0x7FFFu) : static_cast<std::uint16_t>(~s[i]);
  }
}

void unpack3(const std::uint8_t b[3], std::uint16_t s[16]) noexcept {
  std::uint16_t v = static_cast<std::uint16_t>((b[0] << 8) | b[1]);
  v = (v & 0x8000u) != 0 ? static_cast<std::uint16_t>(v & 0x7FFFu) : static_cast<std::uint16_t>(~v);
  for (int i = 0; i < 16; ++i) s[i] = v;
}

Result<Bytes> b44_decompress(const ChunkLayout& layout, ByteView packed) {
  static const std::vector<std::uint16_t> exp_table = [] {
    std::vector<std::uint16_t> table(65536);
    for (std::uint32_t i = 0; i < 65536; ++i) table[i] = b44_from_log(static_cast<std::uint16_t>(i));
    return table;
  }();
  const auto* in = reinterpret_cast<const std::uint8_t*>(packed.data());
  const std::size_t in_size = packed.size();
  std::size_t pos = 0;
  // Channel planes, in channel order.
  std::vector<Bytes> planes(layout.channels.size());
  for (std::size_t c = 0; c < layout.channels.size(); ++c) {
    const ChunkChannel& channel = layout.channels[c];
    const auto nx = static_cast<std::size_t>(channel.width);
    const auto ny = static_cast<std::size_t>(channel.height);
    const std::size_t bytes = nx * ny * pixel_size(channel.type);
    if (bytes == 0) continue;
    Bytes& plane = planes[c];
    if (channel.type != PixelType::Half) {
      if (bytes > in_size - pos) return fail(Status::Decode, "B44 data is truncated");
      plane.assign(packed.begin() + static_cast<std::ptrdiff_t>(pos),
                   packed.begin() + static_cast<std::ptrdiff_t>(pos + bytes));
      pos += bytes;
      continue;
    }
    plane.resize(bytes);
    std::uint16_t s[16];
    for (std::size_t y = 0; y < ny; y += 4) {
      for (std::size_t x = 0; x < nx; x += 4) {
        if (in_size - pos < 3) return fail(Status::Decode, "B44 data is truncated");
        if (in[pos + 2] >= (13 << 2)) {
          unpack3(in + pos, s);
          pos += 3;
        } else {
          if (in_size - pos < 14) return fail(Status::Decode, "B44 data is truncated");
          unpack14(in + pos, s);
          pos += 14;
        }
        if (channel.p_linear) {
          for (std::uint16_t& v : s) v = exp_table[v];
        }
        for (std::size_t dy = 0; dy < 4 && y + dy < ny; ++dy) {
          for (std::size_t dx = 0; dx < 4 && x + dx < nx; ++dx) {
            store16(plane.data() + ((y + dy) * nx + x + dx) * 2, s[dy * 4 + dx]);
          }
        }
      }
    }
  }
  Bytes out(static_cast<std::size_t>(layout.raw_size));
  std::size_t write = 0;
  std::vector<std::size_t> rows(layout.channels.size(), 0);
  for (std::int32_t line = 0; line < layout.height; ++line) {
    for (std::size_t c = 0; c < layout.channels.size(); ++c) {
      const ChunkChannel& channel = layout.channels[c];
      if (!ChunkLayout::has_line(channel, std::int64_t{layout.y0} + line)) continue;
      const std::size_t row_bytes = static_cast<std::size_t>(channel.width) * pixel_size(channel.type);
      if (row_bytes == 0) continue;
      ::cvt::util::copy_bytes(out.data() + write, planes[c].data() + rows[c] * row_bytes, row_bytes);
      write += row_bytes;
      ++rows[c];
    }
  }
  return out;
}

}  // namespace

// --------------------------------------------------------------------------- dispatch

Result<Bytes> decompress_chunk(Compression compression, const ChunkLayout& layout, ByteView packed) {
  if (packed.size() >= layout.raw_size) {
    // Stored data: every OpenEXR compressor keeps the raw bytes when
    // compressing would not make them smaller.
    return Bytes(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(layout.raw_size));
  }
  switch (compression) {
    case Compression::None: return fail(Status::Decode, "uncompressed chunk is truncated");
    case Compression::Rle: {
      CVT_TRY_ASSIGN(const Bytes data, rle_decode(packed, layout.raw_size));
      return zip_reconstruct(data);
    }
    case Compression::Zips:
    case Compression::Zip: {
      CVT_TRY_ASSIGN(const Bytes data, zlib_exact(packed, layout.raw_size));
      return zip_reconstruct(data);
    }
    case Compression::Piz: return piz_decompress(layout, packed);
    case Compression::Pxr24: return pxr24_decompress(layout, packed);
    case Compression::B44:
    case Compression::B44a: return b44_decompress(layout, packed);
    case Compression::Dwaa:
    case Compression::Dwab: return dwa_decompress(layout, packed);
  }
  return fail(Status::Decode, "unknown OpenEXR compression");
}

Result<Bytes> compress_chunk(Compression compression, const ChunkLayout& layout, ByteView raw, int zlib_level) {
  switch (compression) {
    case Compression::None: return Bytes(raw.begin(), raw.end());
    case Compression::Rle: return rle_encode(zip_prepare(raw));
    case Compression::Zips:
    case Compression::Zip: return util::zlib_compress(zip_prepare(raw), zlib_level);
    case Compression::Piz: return piz_compress(layout, raw);
    default: break;
  }
  return fail(Status::Internal, "this OpenEXR compression cannot be written");
}

}  // namespace cvt::image_modern::exr
