// Convertible core - image family: TIFF and BigTIFF.
//
// Reader: little and big endian, classic and BigTIFF; strips and tiles,
// chunky and planar; uncompressed, PackBits, LZW (with horizontal and
// floating-point predictors), Deflate/AdobeDeflate, CCITT modified Huffman,
// Group 3 (1D/2D) and Group 4 for bilevel pictures, and JPEG ("new style",
// with JPEGTables); 1/2/4/8/16/32-bit integer and 16/32/64-bit float
// samples; bilevel, grey, palette, RGB, CMYK, YCbCr (with subsampling) and
// CIE L*a*b*; associated and unassociated alpha; orientation; every page of a
// multi-page file becomes a frame.
//
// Writer: uncompressed, LZW (+ predictor), Deflate (+ predictor), PackBits
// or JPEG; 8 or 16 bits per sample (16 for 16-bit sources); grey or RGB,
// with alpha; every frame becomes a page.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "families/image/jpeg_codec.hpp"
#include "families/image/tiff_ccitt.hpp"
#include "util/bytes.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::image {

namespace {

// --------------------------------------------------------------------------- tags

enum Tag : std::uint16_t {
  NewSubfileType = 254,
  ImageWidth = 256,
  ImageLength = 257,
  BitsPerSample = 258,
  Compression = 259,
  Photometric = 262,
  FillOrder = 266,
  ImageDescription = 270,
  Make = 271,
  Model = 272,
  StripOffsets = 273,
  Orientation = 274,
  SamplesPerPixel = 277,
  RowsPerStrip = 278,
  StripByteCounts = 279,
  XResolution = 282,
  YResolution = 283,
  PlanarConfiguration = 284,
  T4Options = 292,
  T6Options = 293,
  ResolutionUnit = 296,
  PageNumber = 297,
  Software = 305,
  DateTime = 306,
  Artist = 315,
  Predictor = 317,
  ColorMap = 320,
  TileWidth = 322,
  TileLength = 323,
  TileOffsets = 324,
  TileByteCounts = 325,
  InkSet = 332,
  ExtraSamples = 338,
  SampleFormat = 339,
  JpegTables = 347,
  YCbCrSubSampling = 530,
  ReferenceBlackWhite = 532,
  Xmp = 700,
  Copyright = 33432,
  IccProfile = 34675,
};

// --------------------------------------------------------------------------- file access

class File {
 public:
  File(ByteView data, bool little, bool big) : data_(data), little_(little), big_(big) {}
  [[nodiscard]] bool big() const noexcept { return big_; }
  [[nodiscard]] bool little() const noexcept { return little_; }
  [[nodiscard]] ByteView data() const noexcept { return data_; }
  [[nodiscard]] bool has(std::uint64_t at, std::uint64_t n) const noexcept {
    return at <= data_.size() && n <= data_.size() - at;
  }
  [[nodiscard]] std::uint64_t uint(std::uint64_t at, int bytes) const noexcept {
    if (!has(at, static_cast<std::uint64_t>(bytes))) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
      const auto b = std::to_integer<std::uint64_t>(data_[at + static_cast<std::uint64_t>(i)]);
      v = little_ ? v | (b << (8 * i)) : (v << 8) | b;
    }
    return v;
  }

 private:
  ByteView data_;
  bool little_;
  bool big_;
};

int type_size(std::uint16_t type) {
  switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: case 13: return 4;
    case 5: case 10: case 12: case 16: case 17: case 18: return 8;
    default: return 0;
  }
}

struct Entry {
  std::uint16_t type = 0;
  std::uint64_t count = 0;
  std::uint64_t value_at = 0;  // file position of the value(s)
};

using Ifd = std::map<std::uint16_t, Entry>;

/// Values of an entry as unsigned integers (rationals: numerators), at most `limit`.
std::vector<std::uint64_t> values(const File& f, const Ifd& ifd, std::uint16_t tag, std::uint64_t limit = 1u << 24) {
  const auto it = ifd.find(tag);
  if (it == ifd.end()) return {};
  const Entry& e = it->second;
  const int size = type_size(e.type);
  if (size == 0 || e.count == 0 || e.count > limit) return {};
  if (!f.has(e.value_at, e.count * static_cast<std::uint64_t>(size))) return {};
  std::vector<std::uint64_t> out(static_cast<std::size_t>(e.count));
  for (std::uint64_t i = 0; i < e.count; ++i) {
    const std::uint64_t at = e.value_at + i * static_cast<std::uint64_t>(size);
    if (e.type == 5 || e.type == 10) {
      out[i] = f.uint(at, 4);
    } else {
      out[i] = f.uint(at, size);
    }
  }
  return out;
}

std::uint64_t value(const File& f, const Ifd& ifd, std::uint16_t tag, std::uint64_t fallback) {
  const auto v = values(f, ifd, tag, 64);
  return v.empty() ? fallback : v[0];
}

double rational(const File& f, const Ifd& ifd, std::uint16_t tag) {
  const auto it = ifd.find(tag);
  if (it == ifd.end() || it->second.count < 1) return 0.0;
  const Entry& e = it->second;
  if (e.type == 5) {
    const std::uint64_t num = f.uint(e.value_at, 4);
    const std::uint64_t den = f.uint(e.value_at + 4, 4);
    return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
  }
  if (e.type == 11) return std::bit_cast<float>(static_cast<std::uint32_t>(f.uint(e.value_at, 4)));
  if (e.type == 12) return std::bit_cast<double>(f.uint(e.value_at, 8));
  return static_cast<double>(value(f, ifd, tag, 0));
}

ByteView bytes_of(const File& f, const Ifd& ifd, std::uint16_t tag, std::uint64_t limit = 64u << 20) {
  const auto it = ifd.find(tag);
  if (it == ifd.end()) return {};
  const Entry& e = it->second;
  const int size = type_size(e.type);
  const std::uint64_t n = e.count * static_cast<std::uint64_t>(size);
  if (size == 0 || n == 0 || n > limit || !f.has(e.value_at, n)) return {};
  return f.data().subspan(static_cast<std::size_t>(e.value_at), static_cast<std::size_t>(n));
}

std::string text_of(const File& f, const Ifd& ifd, std::uint16_t tag) {
  std::string s(as_chars(bytes_of(f, ifd, tag, 1u << 16)));
  while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
  return util::sanitize_utf8(s);
}

Result<Ifd> read_ifd(const File& f, std::uint64_t offset, std::uint64_t& next) {
  const int count_size = f.big() ? 8 : 2;
  const int entry_size = f.big() ? 20 : 12;
  const int inline_size = f.big() ? 8 : 4;
  if (!f.has(offset, static_cast<std::uint64_t>(count_size))) return fail(Status::Decode, "TIFF directory is out of bounds");
  const std::uint64_t count = f.uint(offset, count_size);
  if (count > 4096 || !f.has(offset + static_cast<std::uint64_t>(count_size), count * static_cast<std::uint64_t>(entry_size))) {
    return fail(Status::Decode, "TIFF directory is truncated");
  }
  Ifd ifd;
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::uint64_t at = offset + static_cast<std::uint64_t>(count_size) + i * static_cast<std::uint64_t>(entry_size);
    const auto tag = static_cast<std::uint16_t>(f.uint(at, 2));
    Entry e;
    e.type = static_cast<std::uint16_t>(f.uint(at + 2, 2));
    e.count = f.uint(at + 4, f.big() ? 8 : 4);
    const std::uint64_t field = at + 4 + (f.big() ? 8 : 4);
    const int size = type_size(e.type);
    if (size == 0) continue;
    const std::uint64_t total = e.count * static_cast<std::uint64_t>(size);
    if (e.count != 0 && total / e.count != static_cast<std::uint64_t>(size)) continue;
    e.value_at = total <= static_cast<std::uint64_t>(inline_size) ? field : f.uint(field, inline_size);
    ifd.emplace(tag, e);
  }
  next = f.uint(offset + static_cast<std::uint64_t>(count_size) + count * static_cast<std::uint64_t>(entry_size),
                f.big() ? 8 : 4);
  return ifd;
}

// --------------------------------------------------------------------------- decompression

Result<void> unpack_bits(ByteView in, std::uint8_t* out, std::size_t size) {
  std::size_t o = 0;
  std::size_t i = 0;
  while (o < size && i < in.size()) {
    const auto n = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(in[i++]));
    if (n >= 0) {
      const std::size_t count = static_cast<std::size_t>(n) + 1;
      for (std::size_t k = 0; k < count && o < size && i < in.size(); ++k) out[o++] = std::to_integer<std::uint8_t>(in[i++]);
    } else if (n != -128) {
      const std::size_t count = static_cast<std::size_t>(1 - n);
      if (i >= in.size()) break;
      const auto value = std::to_integer<std::uint8_t>(in[i++]);
      for (std::size_t k = 0; k < count && o < size; ++k) out[o++] = value;
    }
  }
  return {};
}

/// TIFF LZW (MSB-first codes, "early change").
Result<void> lzw_decode(ByteView in, std::uint8_t* out, std::size_t size) {
  std::vector<std::uint16_t> prefix(4096);
  std::vector<std::uint8_t> suffix(4096);
  std::vector<std::uint16_t> length(4096);
  for (int i = 0; i < 256; ++i) {
    suffix[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    length[static_cast<std::size_t>(i)] = 1;
  }
  int code_size = 9;
  int next = 258;
  int previous = -1;
  std::uint64_t acc = 0;
  int bits = 0;
  std::size_t i = 0;
  std::size_t o = 0;
  std::vector<std::uint8_t> stack(4096);
  while (o < size) {
    while (bits < code_size) {
      if (i >= in.size()) return {};
      acc = (acc << 8) | std::to_integer<std::uint8_t>(in[i++]);
      bits += 8;
    }
    const int code = static_cast<int>((acc >> (bits - code_size)) & ((1u << code_size) - 1u));
    bits -= code_size;
    if (code == 256) {
      code_size = 9;
      next = 258;
      previous = -1;
      continue;
    }
    if (code == 257) return {};
    if (previous < 0) {
      if (code > 255) return fail(Status::Decode, "TIFF LZW data is corrupt");
      out[o++] = static_cast<std::uint8_t>(code);
      previous = code;
      continue;
    }
    std::size_t sp = 0;
    int current = code;
    std::uint8_t first = 0;
    if (code > next || code >= 4096) return fail(Status::Decode, "TIFF LZW data is corrupt");
    if (code == next) {
      int c = previous;
      while (c > 255) c = prefix[static_cast<std::size_t>(c)];
      stack[sp++] = static_cast<std::uint8_t>(c);
      current = previous;
    }
    while (current > 255) {
      if (sp >= stack.size()) return fail(Status::Decode, "TIFF LZW data is corrupt");
      stack[sp++] = suffix[static_cast<std::size_t>(current)];
      current = prefix[static_cast<std::size_t>(current)];
    }
    if (sp >= stack.size()) return fail(Status::Decode, "TIFF LZW data is corrupt");
    stack[sp++] = static_cast<std::uint8_t>(current);
    first = static_cast<std::uint8_t>(current);
    while (sp > 0 && o < size) out[o++] = stack[--sp];
    if (next < 4096) {
      prefix[static_cast<std::size_t>(next)] = static_cast<std::uint16_t>(previous);
      suffix[static_cast<std::size_t>(next)] = first;
      ++next;
    }
    if (next >= 511 && code_size == 9) code_size = 10;
    if (next >= 1023 && code_size == 10) code_size = 11;
    if (next >= 2047 && code_size == 11) code_size = 12;
    previous = code;
  }
  return {};
}

std::uint8_t reverse_bits(std::uint8_t b) {
  b = static_cast<std::uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
  b = static_cast<std::uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
  b = static_cast<std::uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
  return b;
}

// --------------------------------------------------------------------------- page description

struct Page {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint16_t> bits;  // per sample
  std::uint32_t spp = 1;
  int bps = 1;  // bits of every sample (we require them to be equal)
  std::uint32_t compression = 1;
  std::uint32_t photometric = 1;
  std::uint32_t planar = 1;
  std::uint32_t predictor = 1;
  std::uint32_t fill_order = 1;
  std::uint32_t sample_format = 1;
  std::vector<std::uint64_t> extra;
  std::vector<std::uint64_t> offsets;
  std::vector<std::uint64_t> counts;
  bool tiled = false;
  std::uint32_t chunk_w = 0;
  std::uint32_t chunk_h = 0;
  std::vector<std::uint64_t> colormap;
  ByteView jpeg_tables;
  std::uint32_t sub_h = 2;
  std::uint32_t sub_v = 2;
  std::uint32_t orientation = 1;
  std::uint64_t t4 = 0;
  std::uint32_t ink_set = 1;
};

Result<Page> describe(const File& f, const Ifd& ifd, const Context& ctx) {
  Page p;
  p.width = static_cast<std::uint32_t>(std::min<std::uint64_t>(value(f, ifd, ImageWidth, 0), 0xFFFFFFFFu));
  p.height = static_cast<std::uint32_t>(std::min<std::uint64_t>(value(f, ifd, ImageLength, 0), 0xFFFFFFFFu));
  CVT_TRY(check_size(ctx, p.width, p.height));
  p.spp = static_cast<std::uint32_t>(value(f, ifd, SamplesPerPixel, 1));
  if (p.spp == 0 || p.spp > 16) return fail(Status::Decode, "TIFF has an unsupported number of samples ({})", p.spp);
  const auto bits = values(f, ifd, BitsPerSample, 16);
  p.bps = bits.empty() ? 1 : static_cast<int>(bits[0]);
  for (const auto b : bits) {
    if (static_cast<int>(b) != p.bps) return fail(Status::Decode, "TIFF samples of different sizes are not supported");
  }
  p.compression = static_cast<std::uint32_t>(value(f, ifd, Compression, 1));
  p.photometric = static_cast<std::uint32_t>(value(f, ifd, Photometric, p.spp >= 3 ? 2 : 1));
  p.planar = static_cast<std::uint32_t>(value(f, ifd, PlanarConfiguration, 1));
  p.predictor = static_cast<std::uint32_t>(value(f, ifd, Predictor, 1));
  p.fill_order = static_cast<std::uint32_t>(value(f, ifd, FillOrder, 1));
  p.sample_format = static_cast<std::uint32_t>(value(f, ifd, SampleFormat, 1));
  p.extra = values(f, ifd, ExtraSamples, 16);
  p.orientation = static_cast<std::uint32_t>(value(f, ifd, Orientation, 1));
  p.t4 = value(f, ifd, T4Options, 0);
  p.ink_set = static_cast<std::uint32_t>(value(f, ifd, InkSet, 1));
  p.tiled = ifd.contains(TileWidth) && ifd.contains(TileOffsets);
  const std::uint64_t max_chunks = 1u << 22;
  if (p.tiled) {
    p.chunk_w = static_cast<std::uint32_t>(std::min<std::uint64_t>(value(f, ifd, TileWidth, 0), 1u << 20));
    p.chunk_h = static_cast<std::uint32_t>(std::min<std::uint64_t>(value(f, ifd, TileLength, 0), 1u << 20));
    p.offsets = values(f, ifd, TileOffsets, max_chunks);
    p.counts = values(f, ifd, TileByteCounts, max_chunks);
    if (p.chunk_w == 0 || p.chunk_h == 0) return fail(Status::Decode, "TIFF tile size is invalid");
  } else {
    p.chunk_w = p.width;
    const std::uint64_t rows = value(f, ifd, RowsPerStrip, p.height);
    p.chunk_h = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(rows, 1, p.height));
    p.offsets = values(f, ifd, StripOffsets, max_chunks);
    p.counts = values(f, ifd, StripByteCounts, max_chunks);
  }
  if (p.offsets.empty()) return fail(Status::Decode, "TIFF page has no image data");
  if (p.counts.size() < p.offsets.size()) {
    // Missing byte counts (old writers): up to the next offset or the end of the file.
    std::vector<std::uint64_t> sorted = p.offsets;
    std::sort(sorted.begin(), sorted.end());
    p.counts.resize(p.offsets.size());
    for (std::size_t i = 0; i < p.offsets.size(); ++i) {
      const auto it = std::upper_bound(sorted.begin(), sorted.end(), p.offsets[i]);
      const std::uint64_t end = it == sorted.end() ? f.data().size() : *it;
      p.counts[i] = end > p.offsets[i] ? end - p.offsets[i] : 0;
    }
  }
  p.colormap = values(f, ifd, ColorMap, 3u << 16);
  p.jpeg_tables = bytes_of(f, ifd, JpegTables);
  const auto sub = values(f, ifd, YCbCrSubSampling, 2);
  if (sub.size() == 2) {
    p.sub_h = static_cast<std::uint32_t>(sub[0]);
    p.sub_v = static_cast<std::uint32_t>(sub[1]);
  }
  if (p.sub_h != 1 && p.sub_h != 2 && p.sub_h != 4) p.sub_h = 2;
  if (p.sub_v != 1 && p.sub_v != 2 && p.sub_v != 4) p.sub_v = 2;
  const bool float_ok = p.sample_format == 3 && (p.bps == 16 || p.bps == 24 || p.bps == 32 || p.bps == 64);
  const bool int_ok = p.sample_format != 3 && (p.bps == 1 || p.bps == 2 || p.bps == 4 || p.bps == 8 || p.bps == 16 ||
                                               p.bps == 32 || (p.bps > 0 && p.bps < 16));
  if (!float_ok && !int_ok) return fail(Status::Decode, "TIFF samples of {} bits are not supported", p.bps);
  return p;
}

// --------------------------------------------------------------------------- sample access

/// Read sample `index` (counted in samples) of a packed row.
inline std::uint64_t read_sample(const std::uint8_t* row, std::uint64_t index, int bps, bool little) {
  if (bps == 8) return row[index];
  if (bps == 16) {
    const std::uint8_t* p = row + index * 2;
    return little ? (p[0] | p[1] << 8) : (p[0] << 8 | p[1]);
  }
  if (bps == 32 || bps == 64 || bps == 24) {
    const int n = bps / 8;
    const std::uint8_t* p = row + index * static_cast<std::uint64_t>(n);
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = little ? v | (std::uint64_t{p[i]} << (8 * i)) : (v << 8) | p[i];
    return v;
  }
  // Packed MSB-first bits.
  std::uint64_t v = 0;
  const std::uint64_t bit = index * static_cast<std::uint64_t>(bps);
  for (int i = 0; i < bps; ++i) {
    const std::uint64_t b = bit + static_cast<std::uint64_t>(i);
    v = (v << 1) | ((row[b >> 3] >> (7 - (b & 7))) & 1u);
  }
  return v;
}

float half_to_float(std::uint16_t h) {
  const std::uint32_t sign = (h >> 15) & 1u;
  const std::uint32_t exponent = (h >> 10) & 31u;
  const std::uint32_t mantissa = h & 1023u;
  float value = 0.0f;
  if (exponent == 0) {
    value = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa != 0 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
  } else {
    value = std::ldexp(static_cast<float>(mantissa + 1024), static_cast<int>(exponent) - 25);
  }
  return sign ? -value : value;
}

/// Normalised value of a raw sample (0..1 for integers; floats as stored).
inline double normalised(std::uint64_t raw, const Page& p) {
  if (p.sample_format == 3) {
    if (p.bps == 16) return half_to_float(static_cast<std::uint16_t>(raw));
    if (p.bps == 32) return std::bit_cast<float>(static_cast<std::uint32_t>(raw));
    if (p.bps == 64) return std::bit_cast<double>(raw);
    // 24-bit float: sign, 7-bit exponent (bias 63), 16-bit mantissa.
    const std::uint32_t sign = static_cast<std::uint32_t>(raw >> 23) & 1u;
    const int exponent = static_cast<int>((raw >> 16) & 127u);
    const auto mantissa = static_cast<double>(raw & 0xFFFFu);
    const double v = exponent == 0 ? std::ldexp(mantissa, -62 - 16) : std::ldexp(mantissa + 65536.0, exponent - 63 - 16);
    return sign ? -v : v;
  }
  const double max = std::ldexp(1.0, p.bps) - 1.0;
  if (p.sample_format == 2) {
    // Signed: shift to unsigned.
    const std::uint64_t mask = p.bps >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << p.bps) - 1;
    const std::uint64_t half = std::uint64_t{1} << (p.bps - 1);
    return static_cast<double>(((raw & mask) ^ half)) / max;
  }
  return static_cast<double>(raw) / max;
}

double decode_srgb(double v) {
  if (v <= 0.04045) return v / 12.92;
  return std::pow((v + 0.055) / 1.055, 2.4);
}

// --------------------------------------------------------------------------- page decoding

struct Decoded {
  ir::Frame frame;
  std::optional<ir::HdrPixels> hdr;
};

std::uint64_t row_bytes(std::uint64_t width, std::uint64_t samples, int bps) {
  return (width * samples * static_cast<std::uint64_t>(bps) + 7) / 8;
}

/// Undo the horizontal predictor on one decoded row (`row_len` bytes).
void undo_predictor(std::uint8_t* row, std::uint64_t pixels, std::uint32_t spp, const Page& p, bool little) {
  if (p.predictor == 2) {
    if (p.bps == 8) {
      for (std::uint64_t i = spp; i < pixels * spp; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - spp]);
    } else if (p.bps == 16) {
      for (std::uint64_t i = spp; i < pixels * spp; ++i) {
        std::uint8_t* c = row + i * 2;
        const std::uint8_t* q = row + (i - spp) * 2;
        const std::uint16_t prev = little ? static_cast<std::uint16_t>(q[0] | q[1] << 8) : static_cast<std::uint16_t>(q[0] << 8 | q[1]);
        const std::uint16_t cur = little ? static_cast<std::uint16_t>(c[0] | c[1] << 8) : static_cast<std::uint16_t>(c[0] << 8 | c[1]);
        const auto sum = static_cast<std::uint16_t>(cur + prev);
        if (little) {
          c[0] = static_cast<std::uint8_t>(sum);
          c[1] = static_cast<std::uint8_t>(sum >> 8);
        } else {
          c[0] = static_cast<std::uint8_t>(sum >> 8);
          c[1] = static_cast<std::uint8_t>(sum);
        }
      }
    } else if (p.bps == 32) {
      for (std::uint64_t i = spp; i < pixels * spp; ++i) {
        const auto prev = static_cast<std::uint32_t>(read_sample(row, i - spp, 32, little));
        const auto cur = static_cast<std::uint32_t>(read_sample(row, i, 32, little));
        const std::uint32_t sum = cur + prev;
        std::uint8_t* c = row + i * 4;
        for (int k = 0; k < 4; ++k) c[k] = static_cast<std::uint8_t>(sum >> (little ? 8 * k : 8 * (3 - k)));
      }
    }
  } else if (p.predictor == 3) {
    // Floating point: bytes were split into planes (most significant first)
    // and differenced across the whole row.
    const std::uint64_t n = pixels * spp;
    const int bytes = p.bps / 8;
    const std::uint64_t len = n * static_cast<std::uint64_t>(bytes);
    for (std::uint64_t i = spp; i < len; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - spp]);
    std::vector<std::uint8_t> tmp(row, row + len);
    for (std::uint64_t s = 0; s < n; ++s) {
      for (int b = 0; b < bytes; ++b) {
        // Rebuild in file byte order.
        const int file_byte = little ? bytes - 1 - b : b;
        row[s * static_cast<std::uint64_t>(bytes) + static_cast<std::uint64_t>(file_byte)] =
            tmp[static_cast<std::uint64_t>(b) * n + s];
      }
    }
  }
}

Result<Decoded> decode_page(const File& f, const Page& p, const Context& ctx) {
  const std::uint32_t w = p.width;
  const std::uint32_t h = p.height;
  const bool planar = p.planar == 2 && p.spp > 1;
  const std::uint32_t across = (w + p.chunk_w - 1) / p.chunk_w;
  const std::uint32_t down = (h + p.chunk_h - 1) / p.chunk_h;
  const std::uint64_t per_plane = std::uint64_t{across} * down;
  const std::uint64_t needed = per_plane * (planar ? p.spp : 1);
  if (p.offsets.size() < needed) return fail(Status::Decode, "TIFF page lists too few strips or tiles");
  std::uint64_t compressed = 0;
  for (std::uint64_t i = 0; i < needed; ++i) {
    const std::uint64_t start = std::min<std::uint64_t>(p.offsets[i], f.data().size());
    compressed += std::min<std::uint64_t>(p.counts[i], f.data().size() - start);
  }
  const std::uint64_t ratio = p.compression == 1 ? 32 : p.compression == 32773 ? 256
                              : (p.compression >= 2 && p.compression <= 4) ? (1u << 20)
                                                                           : 4096;
  CVT_TRY(check_plausible(std::uint64_t{w} * h, compressed, ratio));

  ir::Frame frame = ir::Frame::filled(w, h, {0, 0, 0, 255});
  Decoded out;
  const bool little = f.little();

  // JPEG: every chunk is a JPEG stream that decodes straight to colour.
  if (p.compression == 7) {
    for (std::uint64_t i = 0; i < needed; ++i) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(i) / needed, "Decoding"));
      const std::uint64_t start = p.offsets[i];
      const std::uint64_t count = p.counts[i];
      if (!f.has(start, count) || count == 0) continue;
      jpeg::DecodeOptions options;
      options.tables = p.jpeg_tables;
      auto planes = jpeg::decode_planes(f.data().subspan(start, count), ctx, options);
      if (!planes) {
        ctx.warn("A JPEG-compressed part of the TIFF picture is damaged.");
        continue;
      }
      if (p.photometric == 2 && planes->components.size() == 3) planes->model = jpeg::ColorModel::Rgb;
      if (p.photometric == 6 && planes->components.size() == 3) planes->model = jpeg::ColorModel::YCbCr;
      CVT_TRY_ASSIGN(const ir::Frame tile, jpeg::planes_to_frame(*planes, ctx));
      const std::uint32_t x0 = static_cast<std::uint32_t>(i % across) * p.chunk_w;
      const std::uint32_t y0 = static_cast<std::uint32_t>(i / across) * p.chunk_h;
      for (std::uint32_t y = 0; y < tile.height && y0 + y < h; ++y) {
        const std::uint32_t n = std::min(tile.width, w - std::min(w, x0));
        std::memcpy(frame.rgba.data() + frame.offset(x0, y0 + y), tile.rgba.data() + static_cast<std::size_t>(y) * tile.width * 4,
                    static_cast<std::size_t>(n) * 4);
      }
    }
    out.frame = std::move(frame);
    return out;
  }
  if (p.compression == 6) return fail(Status::Decode, "old-style JPEG compression in TIFF is not supported");

  // Everything else decodes into raw sample rows first.
  const std::uint32_t chunk_spp = planar ? 1 : p.spp;
  const std::uint64_t raw_row = row_bytes(w, chunk_spp, p.bps);
  const std::uint64_t plane_bytes = raw_row * h;
  const std::uint32_t planes_count = planar ? p.spp : 1;
  if (plane_bytes * planes_count > static_cast<std::uint64_t>(SIZE_MAX / 2)) return fail(Status::Limit, "TIFF page is too large");
  CVT_TRY(ctx.limits.check_decompressed(plane_bytes * planes_count));
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(plane_bytes * planes_count), 0);
  const std::uint64_t chunk_row = row_bytes(p.chunk_w, chunk_spp, p.bps);
  const std::uint64_t chunk_size = chunk_row * p.chunk_h;
  if (chunk_size > (std::uint64_t{1} << 31)) return fail(Status::Decode, "TIFF strips or tiles are too large");
  std::vector<std::uint8_t> chunk(static_cast<std::size_t>(chunk_size));
  bool damaged = false;
  for (std::uint64_t i = 0; i < needed; ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / needed, "Decoding"));
    const std::uint64_t plane = i / per_plane;
    const std::uint64_t index = i % per_plane;
    const std::uint32_t x0 = static_cast<std::uint32_t>(index % across) * p.chunk_w;
    const std::uint32_t y0 = static_cast<std::uint32_t>(index / across) * p.chunk_h;
    const std::uint32_t rows = p.tiled ? p.chunk_h : std::min(p.chunk_h, h - y0);
    const std::size_t expected = static_cast<std::size_t>(chunk_row * rows);
    std::fill(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(expected), std::uint8_t{0});
    const std::uint64_t start = p.offsets[i];
    const std::uint64_t count = std::min<std::uint64_t>(p.counts[i], f.data().size() - std::min<std::uint64_t>(start, f.data().size()));
    if (start >= f.data().size() || count == 0) {
      damaged = true;
      continue;
    }
    ByteView src = f.data().subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(count));
    Bytes reversed;
    if (p.fill_order == 2 && (p.bps == 1 || (p.compression >= 2 && p.compression <= 4))) {
      reversed.resize(src.size());
      for (std::size_t k = 0; k < src.size(); ++k) reversed[k] = std::byte{reverse_bits(std::to_integer<std::uint8_t>(src[k]))};
      src = reversed;
    }
    switch (p.compression) {
      case 1:
        std::memcpy(chunk.data(), src.data(), std::min<std::size_t>(src.size(), expected));
        if (src.size() < expected) damaged = true;
        break;
      case 32773: CVT_TRY(unpack_bits(src, chunk.data(), expected)); break;
      case 5: {
        auto r = lzw_decode(src, chunk.data(), expected);
        if (!r) damaged = true;
        break;
      }
      case 8:
      case 32946: {
        util::Inflater inflater(util::DeflateFormat::Zlib);
        Bytes inflated;
        const auto progress = inflater.feed(src, inflated, expected);
        if (!progress && progress.error().code != Status::Limit) damaged = true;
        std::memcpy(chunk.data(), inflated.data(), std::min(inflated.size(), expected));
        if (inflated.size() < expected) damaged = true;
        break;
      }
      case 2:
      case 3:
      case 4: {
        if (p.bps != 1 || chunk_spp != 1) return fail(Status::Decode, "CCITT compression needs 1-bit pixels");
        const ccitt::Mode mode = p.compression == 2 ? ccitt::Mode::Huffman
                                 : p.compression == 3 ? ccitt::Mode::Group3
                                                      : ccitt::Mode::Group4;
        auto bilevel = ccitt::decode(src, p.chunk_w, rows, mode, (p.t4 & 1) != 0, (p.t4 & 4) != 0);
        if (!bilevel) {
          damaged = true;
          break;
        }
        for (std::uint32_t y = 0; y < rows; ++y) {
          for (std::uint32_t x = 0; x < p.chunk_w; ++x) {
            if ((*bilevel)[static_cast<std::size_t>(y) * p.chunk_w + x]) {
              chunk[static_cast<std::size_t>(y * chunk_row + x / 8)] |= static_cast<std::uint8_t>(0x80u >> (x % 8));
            }
          }
        }
        break;
      }
      default: return fail(Status::Decode, "TIFF compression {} is not supported", p.compression);
    }
    // Predictor, then copy the rows into place.
    for (std::uint32_t y = 0; y < rows; ++y) {
      std::uint8_t* row = chunk.data() + static_cast<std::size_t>(y * chunk_row);
      if (p.predictor > 1) undo_predictor(row, p.chunk_w, chunk_spp, p, little);
      if (y0 + y >= h) break;
      const std::uint64_t dst_offset = plane * plane_bytes + (y0 + y) * raw_row + row_bytes(x0, chunk_spp, p.bps);
      const std::uint64_t columns = std::min<std::uint64_t>(p.chunk_w, w - x0);
      const std::uint64_t n = row_bytes(columns, chunk_spp, p.bps);
      std::memcpy(raw.data() + dst_offset, row, static_cast<std::size_t>(n));
    }
  }
  if (damaged) ctx.warn("Parts of the TIFF image data are missing or damaged.");

  // Interpret the samples.
  const bool wide = p.bps > 8 || p.sample_format == 3;
  std::optional<ir::HdrPixels> hdr;
  if (wide) {
    hdr.emplace();
    hdr->width = w;
    hdr->height = h;
    hdr->rgba.assign(static_cast<std::size_t>(w) * h * 4, 1.0f);
  }
  // Alpha: the first extra sample that is associated (1) or unassociated (2) alpha.
  const std::uint32_t colour_samples = [&] {
    switch (p.photometric) {
      case 0:
      case 1:
      case 3: return 1u;
      case 5: return p.ink_set == 2 ? std::min(p.spp, 4u) : std::min(p.spp, 4u);
      default: return std::min(p.spp, 3u);
    }
  }();
  int alpha_index = -1;
  bool associated = false;
  for (std::size_t k = 0; k < p.extra.size(); ++k) {
    if ((p.extra[k] == 1 || p.extra[k] == 2) && colour_samples + k < p.spp) {
      alpha_index = static_cast<int>(colour_samples + k);
      associated = p.extra[k] == 1;
      break;
    }
  }
  if (alpha_index < 0 && p.extra.empty() && p.spp == colour_samples + 1 && (p.photometric == 2 || p.photometric <= 1)) {
    // Many writers omit ExtraSamples for RGBA / grey+alpha; treat the extra sample as unassociated alpha.
    alpha_index = static_cast<int>(colour_samples);
  }
  const std::uint64_t max_index = (std::uint64_t{1} << std::min(p.bps, 16)) - 1;
  if (p.photometric == 3 && p.colormap.size() < 3 * (max_index + 1)) {
    return fail(Status::Decode, "TIFF palette is missing or too short");
  }
  if (p.photometric == 6 && p.bps != 8) return fail(Status::Decode, "YCbCr TIFF needs 8-bit samples");
  if (p.photometric == 6 && (p.sub_h != 1 || p.sub_v != 1) && !planar) {
    // Subsampled YCbCr: data units of sub_h x sub_v luma samples plus Cb, Cr.
    const std::uint32_t units_x = (w + p.sub_h - 1) / p.sub_h;
    const std::uint32_t unit = p.sub_h * p.sub_v + 2;
    const std::uint64_t unit_row = std::uint64_t{units_x} * unit;
    std::vector<std::uint8_t> ycc(static_cast<std::size_t>(w) * h * 3);
    // Strips hold whole data-unit rows; the raw buffer was filled per strip
    // with the wrong row stride, so re-read straight from the strips.
    std::uint64_t strip_rows = p.chunk_h;
    std::uint64_t written = 0;
    for (std::uint64_t i = 0; i < needed && !p.tiled; ++i) {
      const std::uint64_t start = p.offsets[i];
      const std::uint64_t count = p.counts[i];
      if (!f.has(start, count)) continue;
      std::vector<std::uint8_t> data(static_cast<std::size_t>(unit_row * ((strip_rows + p.sub_v - 1) / p.sub_v)));
      ByteView src = f.data().subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(count));
      if (p.compression == 1) {
        std::memcpy(data.data(), src.data(), std::min(src.size(), data.size()));
      } else if (p.compression == 5) {
        (void)lzw_decode(src, data.data(), data.size());
      } else if (p.compression == 32773) {
        CVT_TRY(unpack_bits(src, data.data(), data.size()));
      } else if (p.compression == 8 || p.compression == 32946) {
        auto inflated = util::zlib_decompress(src, data.size());
        if (inflated) std::memcpy(data.data(), inflated->data(), std::min(inflated->size(), data.size()));
      }
      const std::uint64_t y_base = i * strip_rows;
      for (std::uint64_t uy = 0; uy * p.sub_v < strip_rows; ++uy) {
        for (std::uint32_t ux = 0; ux < units_x; ++ux) {
          const std::uint8_t* u = data.data() + uy * unit_row + std::uint64_t{ux} * unit;
          for (std::uint32_t dy = 0; dy < p.sub_v; ++dy) {
            for (std::uint32_t dx = 0; dx < p.sub_h; ++dx) {
              const std::uint64_t y = y_base + uy * p.sub_v + dy;
              const std::uint64_t x = std::uint64_t{ux} * p.sub_h + dx;
              if (x >= w || y >= h) continue;
              std::uint8_t* d = ycc.data() + (y * w + x) * 3;
              d[0] = u[dy * p.sub_h + dx];
              d[1] = u[p.sub_h * p.sub_v];
              d[2] = u[p.sub_h * p.sub_v + 1];
              ++written;
            }
          }
        }
      }
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
      const double y = ycc[i * 3];
      const double cb = ycc[i * 3 + 1] - 128.0;
      const double cr = ycc[i * 3 + 2] - 128.0;
      frame.rgba[i * 4] = static_cast<std::uint8_t>(std::clamp(y + 1.402 * cr + 0.5, 0.0, 255.0));
      frame.rgba[i * 4 + 1] = static_cast<std::uint8_t>(std::clamp(y - 0.344136 * cb - 0.714136 * cr + 0.5, 0.0, 255.0));
      frame.rgba[i * 4 + 2] = static_cast<std::uint8_t>(std::clamp(y + 1.772 * cb + 0.5, 0.0, 255.0));
    }
    (void)written;
    out.frame = std::move(frame);
    return out;
  }

  std::vector<double> sample(p.spp);
  for (std::uint32_t y = 0; y < h; ++y) {
    if ((y & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / h, "Converting"));
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint64_t palette_index = 0;
      for (std::uint32_t s = 0; s < p.spp; ++s) {
        std::uint64_t raw_value = 0;
        if (planar) {
          raw_value = read_sample(raw.data() + s * plane_bytes + y * raw_row, x, p.bps, little);
        } else {
          raw_value = read_sample(raw.data() + y * raw_row, std::uint64_t{x} * p.spp + s, p.bps, little);
        }
        if (s == 0) palette_index = raw_value;
        sample[s] = normalised(raw_value, p);
      }
      double r = 0, g = 0, b = 0, a = 1;
      switch (p.photometric) {
        case 0: r = g = b = 1.0 - sample[0]; break;
        case 1: r = g = b = sample[0]; break;
        case 3: {
          const std::size_t n = static_cast<std::size_t>(max_index + 1);
          const std::size_t i = static_cast<std::size_t>(std::min<std::uint64_t>(palette_index, max_index));
          r = static_cast<double>(p.colormap[i]) / 65535.0;
          g = static_cast<double>(p.colormap[n + i]) / 65535.0;
          b = static_cast<double>(p.colormap[2 * n + i]) / 65535.0;
          break;
        }
        case 5:
          if (p.spp >= 4) {
            const double k = sample[3];
            r = (1.0 - sample[0]) * (1.0 - k);
            g = (1.0 - sample[1]) * (1.0 - k);
            b = (1.0 - sample[2]) * (1.0 - k);
          } else {
            r = g = b = 1.0 - sample[0];
          }
          break;
        case 6: {  // YCbCr without subsampling
          const double yy = sample[0] * 255.0;
          const double cb = sample[std::min(1u, p.spp - 1)] * 255.0 - 128.0;
          const double cr = sample[std::min(2u, p.spp - 1)] * 255.0 - 128.0;
          r = (yy + 1.402 * cr) / 255.0;
          g = (yy - 0.344136 * cb - 0.714136 * cr) / 255.0;
          b = (yy + 1.772 * cb) / 255.0;
          break;
        }
        case 8:
        case 9: {  // CIE L*a*b* (8: signed a/b; 9: ICC unsigned a/b)
          const double L = sample[0] * 100.0;
          double av = p.spp > 1 ? sample[1] * 255.0 : 0.0;
          double bv = p.spp > 2 ? sample[2] * 255.0 : 0.0;
          if (p.photometric == 8 && p.bps == 8) {
            av = av >= 128.0 ? av - 256.0 : av;
            bv = bv >= 128.0 ? bv - 256.0 : bv;
          } else {
            av -= 128.0;
            bv -= 128.0;
          }
          const double fy = (L + 16.0) / 116.0;
          const double fx = fy + av / 500.0;
          const double fz = fy - bv / 200.0;
          const auto inv = [](double t) { return t > 6.0 / 29.0 ? t * t * t : 3.0 * (6.0 / 29.0) * (6.0 / 29.0) * (t - 4.0 / 29.0); };
          // D65 reference white, as libtiff and ImageMagick use for TIFF Lab.
          const double X = 0.95047 * inv(fx), Y = inv(fy), Z = 1.08883 * inv(fz);
          const double lr = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
          const double lg = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
          const double lb = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
          const auto enc = [](double v) {
            v = std::clamp(v, 0.0, 1.0);
            return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
          };
          r = enc(lr);
          g = enc(lg);
          b = enc(lb);
          break;
        }
        default:
          r = sample[0];
          g = p.spp > 1 ? sample[1] : sample[0];
          b = p.spp > 2 ? sample[2] : sample[0];
          break;
      }
      if (alpha_index >= 0) {
        a = std::clamp(sample[static_cast<std::size_t>(alpha_index)], 0.0, 1.0);
        if (associated && a > 0.0) {
          r /= a;
          g /= a;
          b /= a;
        }
      }
      std::uint8_t* d = frame.rgba.data() + frame.offset(x, y);
      const auto to8 = [](double v) {
        if (!(v > 0.0)) return std::uint8_t{0};
        if (v >= 1.0) return std::uint8_t{255};
        return static_cast<std::uint8_t>(v * 255.0 + 0.5);
      };
      d[0] = to8(r);
      d[1] = to8(g);
      d[2] = to8(b);
      d[3] = to8(a);
      if (hdr) {
        float* q = hdr->rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
        q[0] = static_cast<float>(decode_srgb(std::max(r, 0.0)));
        q[1] = static_cast<float>(decode_srgb(std::max(g, 0.0)));
        q[2] = static_cast<float>(decode_srgb(std::max(b, 0.0)));
        q[3] = static_cast<float>(a);
      }
    }
  }
  out.frame = std::move(frame);
  out.hdr = std::move(hdr);
  return out;
}

Result<ir::Image> read_tiff(ReadContext& ctx) {
  const ByteView data = ctx.input;
  if (data.size() < 8) return fail(Status::Decode, "not a TIFF file");
  const std::string_view magic = as_chars(data.first(4));
  bool little = true;
  bool big = false;
  if (magic == std::string_view("II*\0", 4)) {
    little = true;
  } else if (magic == std::string_view("MM\0*", 4)) {
    little = false;
  } else if (magic == std::string_view("II+\0", 4)) {
    big = true;
  } else if (magic == std::string_view("MM\0+", 4)) {
    little = false;
    big = true;
  } else {
    return fail(Status::Decode, "not a TIFF file");
  }
  const File f(data, little, big);
  std::uint64_t offset = 0;
  if (big) {
    if (data.size() < 16 || f.uint(4, 2) != 8) return fail(Status::Decode, "BigTIFF header is invalid");
    offset = f.uint(8, 8);
  } else {
    offset = f.uint(4, 4);
  }
  ir::Image image;
  std::set<std::uint64_t> visited;
  bool dropped = false;
  while (offset != 0) {
    if (!visited.insert(offset).second) break;  // loop in the IFD chain
    if (visited.size() > ctx.limits.max_archive_entries) return fail(Status::Limit, "the TIFF has too many pages");
    std::uint64_t next = 0;
    auto ifd = read_ifd(f, offset, next);
    if (!ifd) {
      if (image.frames.empty()) return fail(ifd.error());
      ctx.warn("A TIFF page directory is damaged; the remaining pages were skipped.");
      break;
    }
    offset = next;
    // Skip thumbnails and transparency masks.
    const std::uint64_t subfile = value(f, *ifd, NewSubfileType, 0);
    if ((subfile & 1) || (subfile & 4)) continue;
    auto page = describe(f, *ifd, ctx);
    if (!page) {
      if (image.frames.empty()) return fail(page.error());
      ctx.warn(std::format("A TIFF page was skipped: {}", page.error().message));
      continue;
    }
    if (!image.frames.empty() && (page->width != image.width() || page->height != image.height())) {
      // Frames share one size; other pages cannot be represented.
      if (!(page->orientation >= 5 && page->width == image.height() && page->height == image.width())) {
        dropped = true;
        continue;
      }
    }
    CVT_TRY(ctx.limits.check_pixels(page->width, page->height, image.frames.size() + 1));
    auto decoded = decode_page(f, *page, ctx);
    if (!decoded) {
      if (image.frames.empty()) return fail(decoded.error());
      ctx.warn(std::format("A TIFF page could not be decoded: {}", decoded.error().message));
      continue;
    }
    ir::Frame frame = apply_orientation(decoded->frame, static_cast<int>(page->orientation));
    if (!image.frames.empty() && (frame.width != image.width() || frame.height != image.height())) {
      dropped = true;
      continue;
    }
    if (image.frames.empty()) {
      if (decoded->hdr) image.hdr = apply_orientation(*decoded->hdr, static_cast<int>(page->orientation));
      image.source_bit_depth = static_cast<std::uint8_t>(page->sample_format == 3 ? 32 : std::min(page->bps, 16));
      image.source_grayscale = page->photometric <= 1;
      const double xres = rational(f, *ifd, XResolution);
      const double yres = rational(f, *ifd, YResolution);
      const std::uint64_t unit = value(f, *ifd, ResolutionUnit, 2);
      if (xres > 0 && yres > 0 && (unit == 2 || unit == 3)) {
        image.dpi_x = unit == 3 ? xres * 2.54 : xres;
        image.dpi_y = unit == 3 ? yres * 2.54 : yres;
        if (page->orientation >= 5) std::swap(image.dpi_x, image.dpi_y);
      }
      image.icc_profile = to_bytes(bytes_of(f, *ifd, IccProfile));
      image.xmp = util::sanitize_utf8(as_chars(bytes_of(f, *ifd, Xmp)));
      const std::pair<std::uint16_t, const char*> text_tags[] = {
          {ImageDescription, "ImageDescription"}, {Make, "Make"},         {Model, "Model"},
          {Software, "Software"},                 {DateTime, "DateTime"}, {Artist, "Artist"},
          {Copyright, "Copyright"}};
      for (const auto& [tag, name] : text_tags) {
        std::string text = text_of(f, *ifd, tag);
        if (!text.empty()) image.metadata[name] = std::move(text);
      }
    }
    image.frames.push_back(std::move(frame));
  }
  if (image.frames.empty()) return fail(Status::Decode, "the TIFF file contains no picture");
  if (dropped) ctx.warn("Pages of a different size than the first page were skipped.");
  return image;
}

// --------------------------------------------------------------------------- writing

class LzwEncoder {
 public:
  explicit LzwEncoder(Bytes& out) : out_(out) { reset(); }
  void encode(std::span<const std::uint8_t> data) {
    emit(256);
    if (data.empty()) {
      emit(257);
      flush();
      return;
    }
    int prefix = data[0];
    for (std::size_t i = 1; i < data.size(); ++i) {
      const int c = data[i];
      const std::int32_t key = prefix << 8 | c;
      std::size_t slot = (static_cast<std::uint32_t>(key) * 2654435761u >> 18) & (table_size - 1);
      bool found = false;
      while (keys_[slot] != -1) {
        if (keys_[slot] == key) {
          found = true;
          break;
        }
        slot = (slot + 1) & (table_size - 1);
      }
      if (found) {
        prefix = codes_[slot];
        continue;
      }
      emit(prefix);
      keys_[slot] = key;
      codes_[slot] = static_cast<std::uint16_t>(next_++);
      // As libtiff: widen once the next entry no longer fits (the decoder's
      // "early change" makes up for its table lagging one entry behind).
      if (next_ == 4094) {
        emit(256);
        reset();
      } else if (next_ > (1 << code_size_) - 1) {
        ++code_size_;
      }
      prefix = c;
    }
    emit(prefix);
    ++next_;
    if (next_ == 4094) {
      emit(256);
      code_size_ = 9;
    } else if (next_ > (1 << code_size_) - 1) {
      ++code_size_;
    }
    emit(257);
    flush();
  }

 private:
  static constexpr std::size_t table_size = 1 << 14;
  void reset() {
    keys_.assign(table_size, -1);
    codes_.assign(table_size, 0);
    next_ = 258;
    code_size_ = 9;
  }
  void emit(int code) {
    acc_ = (acc_ << code_size_) | static_cast<std::uint32_t>(code);
    bits_ += code_size_;
    while (bits_ >= 8) {
      bits_ -= 8;
      out_.push_back(std::byte{static_cast<std::uint8_t>(acc_ >> bits_)});
    }
    acc_ &= (1u << bits_) - 1u;
  }
  void flush() {
    if (bits_ > 0) out_.push_back(std::byte{static_cast<std::uint8_t>(acc_ << (8 - bits_))});
    bits_ = 0;
    acc_ = 0;
  }
  Bytes& out_;
  std::vector<std::int32_t> keys_;
  std::vector<std::uint16_t> codes_;
  int next_ = 258;
  int code_size_ = 9;
  std::uint32_t acc_ = 0;
  int bits_ = 0;
};

void pack_bits(std::span<const std::uint8_t> row, Bytes& out) {
  std::size_t i = 0;
  while (i < row.size()) {
    std::size_t run = 1;
    while (i + run < row.size() && run < 128 && row[i + run] == row[i]) ++run;
    if (run >= 2) {
      out.push_back(std::byte{static_cast<std::uint8_t>(257 - run)});
      out.push_back(std::byte{row[i]});
      i += run;
      continue;
    }
    std::size_t n = 1;
    while (i + n < row.size() && n < 128 && !(i + n + 1 < row.size() && row[i + n] == row[i + n + 1])) ++n;
    out.push_back(std::byte{static_cast<std::uint8_t>(n - 1)});
    for (std::size_t k = 0; k < n; ++k) out.push_back(std::byte{row[i + k]});
    i += n;
  }
}

struct IfdWriter {
  struct Field {
    std::uint16_t tag;
    std::uint16_t type;
    std::uint32_t count;
    Bytes data;  // little-endian value bytes
  };
  std::vector<Field> fields;
  void shorts(std::uint16_t tag, std::initializer_list<std::uint16_t> v) {
    util::ByteWriter w;
    for (const auto x : v) w.u16le(x);
    fields.push_back({tag, 3, static_cast<std::uint32_t>(v.size()), w.take()});
  }
  void shorts(std::uint16_t tag, const std::vector<std::uint16_t>& v) {
    util::ByteWriter w;
    for (const auto x : v) w.u16le(x);
    fields.push_back({tag, 3, static_cast<std::uint32_t>(v.size()), w.take()});
  }
  void longs(std::uint16_t tag, const std::vector<std::uint32_t>& v) {
    util::ByteWriter w;
    for (const auto x : v) w.u32le(x);
    fields.push_back({tag, 4, static_cast<std::uint32_t>(v.size()), w.take()});
  }
  void rational(std::uint16_t tag, double v) {
    util::ByteWriter w;
    w.u32le(static_cast<std::uint32_t>(std::lround(v * 100))).u32le(100);
    fields.push_back({tag, 5, 1, w.take()});
  }
  void ascii(std::uint16_t tag, std::string_view text) {
    Bytes b = to_bytes(text);
    b.push_back(std::byte{0});
    fields.push_back({tag, 2, static_cast<std::uint32_t>(b.size()), std::move(b)});
  }
  void undefined(std::uint16_t tag, ByteView data, std::uint16_t type = 7) {
    fields.push_back({tag, type, static_cast<std::uint32_t>(data.size()), to_bytes(data)});
  }
};

Result<Bytes> write_tiff(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  const ir::Image& image = *prepared;
  const std::string compression_name =
      ctx.options.get_enum("compression", "lzw", {"none", "lzw", "deflate", "packbits", "jpeg"});
  const int level = options::resolved_compression_level(ctx.options);
  const bool jpeg_mode = compression_name == "jpeg";
  const std::string depth_option = ctx.options.get_enum("bit_depth", "auto", {"auto", "8", "16"});
  bool sixteen = !jpeg_mode && image.hdr.has_value() && image.frames.size() == 1 &&
                 (depth_option == "16" || (depth_option == "auto" && image.source_bit_depth > 8));
  if (image.hdr && (image.hdr->width != image.width() || image.hdr->height != image.height())) sixteen = false;
  bool grey = true;
  bool alpha = false;
  for (const ir::Frame& f : image.frames) {
    grey = grey && is_grayscale(f);
    alpha = alpha || !is_opaque(f);
  }
  if (jpeg_mode && alpha) {
    ctx.warn("JPEG-compressed TIFF has no transparency; transparent areas were blended onto the background.");
  }
  const std::uint16_t compression_tag = compression_name == "none"       ? 1
                                        : compression_name == "lzw"      ? 5
                                        : compression_name == "deflate"  ? 8
                                        : compression_name == "packbits" ? 32773
                                                                         : 7;
  const bool predictor = compression_tag == 5 || compression_tag == 8;
  util::ByteWriter out;
  out.text(std::string_view("II*\0", 4)).u32le(0);  // first IFD offset patched later
  std::size_t previous_next_field = 4;
  const auto pages = static_cast<std::uint16_t>(std::min<std::size_t>(image.frames.size(), 65535));
  for (std::size_t n = 0; n < image.frames.size(); ++n) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(n) / image.frames.size(), "Encoding"));
    ir::Frame frame = image.frames[n];
    const std::uint32_t w = frame.width;
    const std::uint32_t h = frame.height;
    bool page_alpha = alpha && !jpeg_mode;
    if (jpeg_mode && !is_opaque(frame)) {
      CVT_TRY_ASSIGN(const Rgb bg, resolved_background(ctx.options));
      flatten_alpha(frame, bg);
    }
    const std::uint32_t colour = grey ? 1 : 3;
    const std::uint32_t spp = colour + (page_alpha ? 1 : 0);
    const int bps = sixteen ? 16 : 8;
    const std::uint64_t row = std::uint64_t{w} * spp * static_cast<std::uint64_t>(bps / 8);
    if (row * h > 0xFFFFFFFFull) return fail(Status::Encode, "the picture is too large for classic TIFF");
    std::uint32_t rows_per_strip = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, (jpeg_mode ? 262144 : 65536) / std::max<std::uint64_t>(row, 1)));
    if (jpeg_mode) rows_per_strip = std::max<std::uint32_t>(16, (rows_per_strip + 15) / 16 * 16);
    rows_per_strip = std::min(rows_per_strip, h);
    std::vector<std::uint16_t> wide;
    if (sixteen) wide = srgb16_from_linear(*image.hdr);
    // Encode strips.
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> counts;
    std::vector<std::uint8_t> strip;
    for (std::uint32_t y0 = 0; y0 < h; y0 += rows_per_strip) {
      const std::uint32_t rows = std::min(rows_per_strip, h - y0);
      Bytes encoded;
      if (jpeg_mode) {
        ir::Frame part;
        part.width = w;
        part.height = rows;
        part.rgba.assign(frame.rgba.begin() + static_cast<std::ptrdiff_t>(frame.offset(0, y0)),
                         frame.rgba.begin() + static_cast<std::ptrdiff_t>(frame.offset(0, y0) + std::size_t{w} * rows * 4));
        jpeg::EncodeOptions jo;
        jo.quality = options::resolved_quality(ctx.options);
        jo.subsampling = jo.quality >= 90 ? jpeg::Subsampling::S444 : jpeg::Subsampling::S420;
        jo.grayscale = grey;
        CVT_TRY_ASSIGN(encoded, jpeg::encode(part, jo, ctx));
      } else {
        strip.assign(static_cast<std::size_t>(row * rows), 0);
        for (std::uint32_t y = 0; y < rows; ++y) {
          std::uint8_t* dst = strip.data() + static_cast<std::size_t>(y * row);
          for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t at = frame.offset(x, y0 + y);
            for (std::uint32_t s = 0; s < spp; ++s) {
              const std::size_t channel = s < colour ? s : 3;
              if (sixteen) {
                const std::uint16_t v = wide[at + channel];
                const std::size_t o = (static_cast<std::size_t>(x) * spp + s) * 2;
                dst[o] = static_cast<std::uint8_t>(v);
                dst[o + 1] = static_cast<std::uint8_t>(v >> 8);
              } else {
                dst[static_cast<std::size_t>(x) * spp + s] = frame.rgba[at + channel];
              }
            }
          }
          if (predictor) {
            // Horizontal differencing, right to left.
            if (sixteen) {
              for (std::uint64_t i = std::uint64_t{w} * spp - 1; i >= spp; --i) {
                const std::uint16_t cur = static_cast<std::uint16_t>(dst[i * 2] | dst[i * 2 + 1] << 8);
                const std::uint16_t prev = static_cast<std::uint16_t>(dst[(i - spp) * 2] | dst[(i - spp) * 2 + 1] << 8);
                const auto d = static_cast<std::uint16_t>(cur - prev);
                dst[i * 2] = static_cast<std::uint8_t>(d);
                dst[i * 2 + 1] = static_cast<std::uint8_t>(d >> 8);
              }
            } else {
              for (std::uint64_t i = std::uint64_t{w} * spp - 1; i >= spp; --i) {
                dst[i] = static_cast<std::uint8_t>(dst[i] - dst[i - spp]);
              }
            }
          }
        }
        switch (compression_tag) {
          case 1: encoded.assign(reinterpret_cast<const std::byte*>(strip.data()), reinterpret_cast<const std::byte*>(strip.data() + strip.size())); break;
          case 5: {
            LzwEncoder lzw(encoded);
            lzw.encode(strip);
            break;
          }
          case 8: {
            CVT_TRY_ASSIGN(encoded, util::zlib_compress(as_bytes(std::span<const std::uint8_t>(strip)), level));
            break;
          }
          default:
            for (std::uint32_t y = 0; y < rows; ++y) {
              pack_bits(std::span<const std::uint8_t>(strip).subspan(static_cast<std::size_t>(y * row), static_cast<std::size_t>(row)), encoded);
            }
            break;
        }
      }
      out.align(2);
      if (out.size() + encoded.size() > 0xFFFFFFFFull) return fail(Status::Encode, "the TIFF would exceed 4 GiB");
      offsets.push_back(static_cast<std::uint32_t>(out.size()));
      counts.push_back(static_cast<std::uint32_t>(encoded.size()));
      out.bytes(encoded);
    }
    // Directory.
    IfdWriter ifd;
    ifd.longs(NewSubfileType, {pages > 1 ? 2u : 0u});
    ifd.longs(ImageWidth, {w});
    ifd.longs(ImageLength, {h});
    ifd.shorts(BitsPerSample, std::vector<std::uint16_t>(spp, static_cast<std::uint16_t>(bps)));
    ifd.shorts(Compression, {compression_tag});
    ifd.shorts(Photometric, {static_cast<std::uint16_t>(jpeg_mode && !grey ? 6 : grey ? 1 : 2)});
    if (prepared.keep_metadata()) {
      if (const auto it = image.metadata.find("ImageDescription"); it != image.metadata.end()) ifd.ascii(ImageDescription, it->second);
    }
    ifd.longs(StripOffsets, offsets);
    ifd.shorts(SamplesPerPixel, {static_cast<std::uint16_t>(spp)});
    ifd.longs(RowsPerStrip, {rows_per_strip});
    ifd.longs(StripByteCounts, counts);
    const double dpi_x = image.dpi_x > 0 ? image.dpi_x : 72.0;
    const double dpi_y = image.dpi_y > 0 ? image.dpi_y : 72.0;
    ifd.rational(XResolution, dpi_x);
    ifd.rational(YResolution, dpi_y);
    ifd.shorts(PlanarConfiguration, {1});
    ifd.shorts(ResolutionUnit, {2});
    if (pages > 1) ifd.shorts(PageNumber, {static_cast<std::uint16_t>(n), pages});
    if (predictor) ifd.shorts(Predictor, {2});
    if (page_alpha) ifd.shorts(ExtraSamples, {2});
    if (jpeg_mode && !grey) {
      const bool full = options::resolved_quality(ctx.options) >= 90;
      ifd.shorts(YCbCrSubSampling, {static_cast<std::uint16_t>(full ? 1 : 2), static_cast<std::uint16_t>(full ? 1 : 2)});
      util::ByteWriter ref;
      for (const std::uint32_t v : {0u, 255u, 128u, 255u, 128u, 255u}) ref.u32le(v).u32le(1);
      ifd.fields.push_back({ReferenceBlackWhite, 5, 6, ref.take()});
    }
    if (prepared.keep_metadata() && n == 0) {
      if (!image.xmp.empty()) ifd.undefined(Xmp, as_bytes(image.xmp), 1);
      if (!image.icc_profile.empty()) ifd.undefined(IccProfile, image.icc_profile);
    }
    std::sort(ifd.fields.begin(), ifd.fields.end(), [](const auto& a, const auto& b) { return a.tag < b.tag; });
    // Out-of-line values first, then the directory.
    std::vector<std::uint32_t> value_offsets(ifd.fields.size(), 0);
    for (std::size_t i = 0; i < ifd.fields.size(); ++i) {
      if (ifd.fields[i].data.size() > 4) {
        out.align(2);
        value_offsets[i] = static_cast<std::uint32_t>(out.size());
        out.bytes(ifd.fields[i].data);
      }
    }
    out.align(2);
    const auto ifd_offset = static_cast<std::uint32_t>(out.size());
    out.patch_u32le(previous_next_field, ifd_offset);
    out.u16le(static_cast<std::uint16_t>(ifd.fields.size()));
    for (std::size_t i = 0; i < ifd.fields.size(); ++i) {
      const auto& field = ifd.fields[i];
      out.u16le(field.tag).u16le(field.type).u32le(field.count);
      if (field.data.size() > 4) {
        out.u32le(value_offsets[i]);
      } else {
        Bytes inline_value = field.data;
        inline_value.resize(4, std::byte{0});
        out.bytes(inline_value);
      }
    }
    previous_next_field = out.size();
    out.u32le(0);
    if (out.size() > 0xFFFFFFFFull) return fail(Status::Encode, "the TIFF would exceed 4 GiB");
  }
  return out.take();
}

int sniff_tiff(std::span<const std::byte> data, std::string_view /*filename*/) {
  if (data.size() < 8) return 0;
  const std::string_view magic = as_chars(data.first(4));
  if (magic == std::string_view("II*\0", 4) || magic == std::string_view("MM\0*", 4)) return 0;  // signatures cover it
  if (magic == std::string_view("II+\0", 4) || magic == std::string_view("MM\0+", 4)) return 92;
  return 0;
}

}  // namespace

void register_tiff(Registry& registry) {
  FormatInfo info;
  info.id = "tiff";
  info.name = "TIFF image";
  info.description = "Tagged image file: lossless, multi-page, high bit depth.";
  info.category = Category::Image;
  info.extensions = {"tiff", "tif"};
  info.mime = "image/tiff";
  info.popularity = 65;
  info.signatures = {Signature::at(0, "'II' 2A 00"), Signature::at(0, "'MM' 00 2A"), Signature::at(0, "'II' 2B 00"),
                     Signature::at(0, "'MM' 00 2B")};
  info.sniff = sniff_tiff;
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "tiff";
  spec.read = reader(read_tiff);
  spec.write = writer(write_tiff);
  spec.write_options = lossless_writer_options();
  spec.write_options.push_back(OptionSpec::choice("compression", "Compression", "lzw",
                                                  {"none", "lzw", "deflate", "packbits", "jpeg"},
                                                  "LZW and Deflate are lossless and widely supported; JPEG is lossy "
                                                  "and much smaller for photos."));
  spec.write_options.push_back(options::quality());
  spec.write_options.back().description = "JPEG quality when the JPEG compression is chosen.";
  spec.write_options.back() = spec.write_options.back().advanced();
  spec.write_options.push_back(OptionSpec::choice("bit_depth", "Bit depth", "auto", {"auto", "8", "16"},
                                                  "Bits per sample; auto keeps 16 bits for 16-bit sources.")
                                   .advanced());
  spec.write_options.push_back(background_option());
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
