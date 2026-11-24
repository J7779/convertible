// Convertible core - image_modern family: reading RAW sample data from TIFF /
// DNG image directories (strips and tiles; uncompressed, lossless JPEG,
// Deflate with integer and floating-point predictors, lossy JPEG tiles).
// Written from the TIFF 6.0 specification, the DNG specification (1.4-1.7)
// and Adobe's TIFF technote 3 (floating-point predictor).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "families/image/image_api.hpp"
#include "families/image_modern/raw_ljpeg.hpp"
#include "families/image_modern/raw_model.hpp"
#include "util/deflate.hpp"

namespace cvt::image_modern::raw {

namespace {

enum : std::uint16_t {
  kImageWidth = 256, kImageLength = 257, kBitsPerSample = 258, kCompression = 259, kStripOffsets = 273,
  kSamplesPerPixel = 277, kRowsPerStrip = 278, kStripByteCounts = 279, kPlanarConfiguration = 284,
  kPredictor = 317, kTileWidth = 322, kTileLength = 323, kTileOffsets = 324, kTileByteCounts = 325,
  kSampleFormat = 339,
};

float half_to_float(std::uint16_t h) {
  const std::uint32_t sign = (h >> 15) & 1u;
  const std::uint32_t exponent = (h >> 10) & 0x1Fu;
  const std::uint32_t mantissa = h & 0x3FFu;
  float value = 0.0f;
  if (exponent == 0) {
    value = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa == 0 ? 65504.0f : 0.0f;  // infinities clamp, NaN becomes 0
  } else {
    value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
  }
  return sign != 0 ? -value : value;
}

// DNG 24-bit float: sign, 7-bit exponent (bias 63), 16-bit mantissa.
float fp24_to_float(std::uint32_t v) {
  const std::uint32_t sign = (v >> 23) & 1u;
  const std::uint32_t exponent = (v >> 16) & 0x7Fu;
  const std::uint32_t mantissa = v & 0xFFFFu;
  float value = 0.0f;
  if (exponent == 0) {
    value = std::ldexp(static_cast<float>(mantissa), -62 - 16);
  } else if (exponent == 127) {
    value = 0.0f;
  } else {
    value = std::ldexp(static_cast<float>(mantissa | 0x10000u), static_cast<int>(exponent) - 63 - 16);
  }
  return sign != 0 ? -value : value;
}

float sanitize(float v) { return std::isfinite(v) ? v : 0.0f; }

// One tile or strip: its position and size in the image.
struct Chunk {
  std::uint32_t x = 0, y = 0, width = 0, height = 0;  // nominal size (tiles may extend past the image)
  ByteView data;
};

struct Layout {
  std::uint32_t width = 0, height = 0;
  int spp = 1;
  int bits = 16;
  int sample_format = 1;
  int compression = 1;
  int predictor = 1;
  bool little = true;
  Packing packing = Packing::Auto;
};

// Write decoded integer rows of a chunk into the sensor buffer (clipped).
void place_u16(Sensor& sensor, const Chunk& chunk, const std::uint16_t* rows, std::size_t row_samples) {
  const auto spp = static_cast<std::size_t>(sensor.spp);
  const std::uint32_t w = std::min(chunk.width, sensor.width - chunk.x);
  const std::uint32_t h = std::min(chunk.height, sensor.height - chunk.y);
  for (std::uint32_t r = 0; r < h; ++r) {
    std::uint16_t* dst = sensor.u16.data() + (std::size_t{chunk.y + r} * sensor.width + chunk.x) * spp;
    std::memcpy(dst, rows + std::size_t{r} * row_samples, std::size_t{w} * spp * sizeof(std::uint16_t));
  }
}

void place_f32(Sensor& sensor, const Chunk& chunk, const float* rows, std::size_t row_samples) {
  const auto spp = static_cast<std::size_t>(sensor.spp);
  const std::uint32_t w = std::min(chunk.width, sensor.width - chunk.x);
  const std::uint32_t h = std::min(chunk.height, sensor.height - chunk.y);
  for (std::uint32_t r = 0; r < h; ++r) {
    float* dst = sensor.f32.data() + (std::size_t{chunk.y + r} * sensor.width + chunk.x) * spp;
    std::memcpy(dst, rows + std::size_t{r} * row_samples, std::size_t{w} * spp * sizeof(float));
  }
}

void undo_integer_predictor(std::uint16_t* rows, std::uint32_t width, std::uint32_t height, int spp, int predictor,
                            int bits) {
  std::size_t distance = static_cast<std::size_t>(spp);
  if (predictor == 34892) distance *= 2;
  if (predictor == 34893) distance *= 4;
  const std::size_t n = std::size_t{width} * static_cast<std::size_t>(spp);
  const std::uint32_t mask = bits >= 16 ? 0xFFFFu : ((1u << bits) - 1u);
  for (std::uint32_t y = 0; y < height; ++y) {
    std::uint16_t* row = rows + std::size_t{y} * n;
    for (std::size_t i = distance; i < n; ++i) row[i] = static_cast<std::uint16_t>((row[i] + row[i - distance]) & mask);
  }
}

// Floating-point predictor (TIFF technote 3): bytes of each row are stored
// as byte planes (most significant first) and horizontally differenced.
Result<void> undo_float_predictor(const Bytes& in, std::uint32_t width, std::uint32_t height, int spp, int bytes,
                                  int predictor, float* out) {
  const std::size_t n = std::size_t{width} * static_cast<std::size_t>(spp);
  const std::size_t row_bytes = n * static_cast<std::size_t>(bytes);
  if (in.size() < row_bytes * height) return fail(Status::Decode, "floating-point RAW tile is truncated");
  std::size_t distance = static_cast<std::size_t>(spp);
  if (predictor == 34894) distance *= 2;
  if (predictor == 34895) distance *= 4;
  std::vector<std::uint8_t> row(row_bytes);
  for (std::uint32_t y = 0; y < height; ++y) {
    std::memcpy(row.data(), in.data() + std::size_t{y} * row_bytes, row_bytes);
    for (std::size_t i = distance; i < row_bytes; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - distance]);
    for (std::size_t i = 0; i < n; ++i) {
      std::uint32_t v = 0;
      for (int b = 0; b < bytes; ++b) v = (v << 8) | row[static_cast<std::size_t>(b) * n + i];
      float f = 0.0f;
      if (bytes == 2) f = half_to_float(static_cast<std::uint16_t>(v));
      else if (bytes == 3) f = fp24_to_float(v);
      else f = std::bit_cast<float>(v);
      out[std::size_t{y} * n + i] = sanitize(f);
    }
  }
  return {};
}

// Plain floating-point samples in file byte order.
Result<void> unpack_float(ByteView data, std::uint32_t width, std::uint32_t height, int spp, int bytes, bool little,
                          float* out) {
  const std::size_t n = std::size_t{width} * static_cast<std::size_t>(spp);
  if (data.size() / static_cast<std::size_t>(bytes) / std::max<std::size_t>(n, 1) < height) {
    return fail(Status::Decode, "floating-point RAW data is truncated");
  }
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  for (std::size_t i = 0; i < n * height; ++i) {
    std::uint32_t v = 0;
    for (int b = 0; b < bytes; ++b) {
      const std::uint32_t byte = p[i * static_cast<std::size_t>(bytes) + static_cast<std::size_t>(b)];
      v = little ? v | (byte << (8 * b)) : (v << 8) | byte;
    }
    float f = 0.0f;
    if (bytes == 2) f = half_to_float(static_cast<std::uint16_t>(v));
    else if (bytes == 3) f = fp24_to_float(v);
    else f = std::bit_cast<float>(v);
    out[i] = sanitize(f);
  }
  return {};
}

Result<void> decode_chunk(const Layout& layout, const Chunk& chunk, Sensor& sensor, const Context& ctx) {
  const auto spp = static_cast<std::size_t>(layout.spp);
  const std::size_t row_samples = std::size_t{chunk.width} * spp;
  const std::size_t samples = row_samples * chunk.height;
  const bool floating = layout.sample_format == 3;

  switch (layout.compression) {
    case 1: {
      if (floating) {
        std::vector<float> rows(samples);
        CVT_TRY(unpack_float(chunk.data, chunk.width, chunk.height, layout.spp, layout.bits / 8, layout.little, rows.data()));
        place_f32(sensor, chunk, rows.data(), row_samples);
        return {};
      }
      std::vector<std::uint16_t> rows(samples);
      CVT_TRY(unpack_rows(chunk.data, chunk.width, chunk.height, layout.spp, layout.bits, layout.little, layout.packing,
                          rows.data(), row_samples));
      if (layout.predictor != 1) undo_integer_predictor(rows.data(), chunk.width, chunk.height, layout.spp, layout.predictor, layout.bits);
      place_u16(sensor, chunk, rows.data(), row_samples);
      return {};
    }
    case 7: {
      CVT_TRY_ASSIGN(LjpegFrame frame, decode_ljpeg(chunk.data, std::uint64_t{samples} + (1u << 20)));
      if (frame.samples.size() < samples) {
        // Some writers encode edge tiles at their clipped size.
        const std::uint32_t w = std::min(chunk.width, sensor.width - chunk.x);
        const std::uint32_t h = std::min(chunk.height, sensor.height - chunk.y);
        if (frame.samples.size() < std::size_t{w} * spp * h) return fail(Status::Decode, "lossless JPEG tile is too small");
        Chunk clipped = chunk;
        clipped.width = w;
        clipped.height = h;
        place_u16(sensor, clipped, frame.samples.data(), std::size_t{w} * spp);
        return {};
      }
      place_u16(sensor, chunk, frame.samples.data(), row_samples);
      return {};
    }
    case 8:
    case 32946: {
      const int bytes = floating ? layout.bits / 8 : (layout.bits + 7) / 8;
      const std::uint64_t expected = std::uint64_t{samples} * static_cast<std::uint64_t>(bytes);
      CVT_TRY(ctx.limits.check_decompressed(expected));
      CVT_TRY_ASSIGN(Bytes plain, util::zlib_decompress(chunk.data, expected + 16));
      if (floating) {
        std::vector<float> rows(samples);
        if (layout.predictor == 3 || layout.predictor == 34894 || layout.predictor == 34895) {
          CVT_TRY(undo_float_predictor(plain, chunk.width, chunk.height, layout.spp, bytes, layout.predictor, rows.data()));
        } else {
          CVT_TRY(unpack_float(plain, chunk.width, chunk.height, layout.spp, bytes, layout.little, rows.data()));
        }
        place_f32(sensor, chunk, rows.data(), row_samples);
        return {};
      }
      std::vector<std::uint16_t> rows(samples);
      CVT_TRY(unpack_rows(plain, chunk.width, chunk.height, layout.spp, layout.bits, layout.little, Packing::MsbPacked,
                          rows.data(), row_samples));
      if (layout.predictor != 1) undo_integer_predictor(rows.data(), chunk.width, chunk.height, layout.spp, layout.predictor, layout.bits);
      place_u16(sensor, chunk, rows.data(), row_samples);
      return {};
    }
    case 34892: {
      if (layout.bits != 8 || (layout.spp != 1 && layout.spp != 3)) {
        return fail(Status::Decode, "unsupported lossy JPEG RAW layout");
      }
      CVT_TRY_ASSIGN(ir::Image tile, image::decode("jpg", chunk.data, ctx));
      if (!tile.valid()) return fail(Status::Decode, "lossy JPEG RAW tile is empty");
      const ir::Frame& f = tile.frames.front();
      const std::uint32_t w = std::min({chunk.width, f.width, sensor.width - chunk.x});
      const std::uint32_t h = std::min({chunk.height, f.height, sensor.height - chunk.y});
      for (std::uint32_t r = 0; r < h; ++r) {
        std::uint16_t* dst = sensor.u16.data() + (std::size_t{chunk.y + r} * sensor.width + chunk.x) * spp;
        for (std::uint32_t c = 0; c < w; ++c) {
          const std::size_t at = f.offset(c, r);
          for (std::size_t s = 0; s < spp; ++s) dst[std::size_t{c} * spp + s] = f.rgba[at + s];
        }
      }
      return {};
    }
    default:
      return fail(Status::Decode, "RAW compression {} is not supported", layout.compression);
  }
}

}  // namespace

Result<void> unpack_rows(ByteView data, std::uint32_t width, std::uint32_t height, int spp, int bits,
                         bool little_endian, Packing packing, std::uint16_t* out, std::size_t out_stride) {
  if (bits < 1 || bits > 16) return fail(Status::Decode, "{}-bit RAW samples are not supported", bits);
  const std::uint64_t n = std::uint64_t{width} * static_cast<std::uint64_t>(spp);
  const std::uint64_t packed_row = (n * static_cast<std::uint64_t>(bits) + 7) / 8;
  if (packing == Packing::Auto) {
    packing = (bits != 8 && bits != 16 && data.size() >= n * 2 * height && data.size() < n * 2 * height + n * 2)
                  ? Packing::Container16
                  : Packing::MsbPacked;
  }
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  if (packing == Packing::Container16 || bits == 16) {
    if (data.size() / 2 / std::max<std::uint64_t>(n, 1) < height) return fail(Status::Decode, "RAW strip data is truncated");
    const std::uint32_t mask = bits >= 16 ? 0xFFFFu : ((1u << bits) - 1u);
    for (std::uint32_t y = 0; y < height; ++y) {
      const std::uint8_t* row = p + std::size_t{y} * static_cast<std::size_t>(n) * 2;
      std::uint16_t* dst = out + std::size_t{y} * out_stride;
      for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t v = little_endian ? (row[i * 2] | (std::uint32_t{row[i * 2 + 1]} << 8))
                                              : ((std::uint32_t{row[i * 2]} << 8) | row[i * 2 + 1]);
        dst[i] = static_cast<std::uint16_t>(v & mask);
      }
    }
    return {};
  }
  if (packed_row == 0 || data.size() / packed_row < height) return fail(Status::Decode, "RAW strip data is truncated");
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint8_t* row = p + std::size_t{y} * static_cast<std::size_t>(packed_row);
    std::uint16_t* dst = out + std::size_t{y} * out_stride;
    if (bits == 8) {
      for (std::size_t i = 0; i < n; ++i) dst[i] = row[i];
      continue;
    }
    std::uint64_t acc = 0;
    int have = 0;
    std::size_t at = 0;
    const std::uint32_t mask = (1u << bits) - 1u;
    for (std::size_t i = 0; i < n; ++i) {
      while (have < bits) {
        acc = (acc << 8) | row[at++];
        have += 8;
      }
      have -= bits;
      dst[i] = static_cast<std::uint16_t>((acc >> have) & mask);
    }
  }
  return {};
}

Result<Sensor> read_samples(const TiffData& tiff, const Ifd& ifd, const Context& ctx, Packing packing) {
  Layout layout;
  layout.width = static_cast<std::uint32_t>(std::min<std::uint64_t>(tiff.integer(ifd, kImageWidth), 0xFFFFFFFFu));
  layout.height = static_cast<std::uint32_t>(std::min<std::uint64_t>(tiff.integer(ifd, kImageLength), 0xFFFFFFFFu));
  layout.spp = static_cast<int>(tiff.integer(ifd, kSamplesPerPixel, 1));
  layout.bits = static_cast<int>(tiff.integer(ifd, kBitsPerSample, 1));
  layout.sample_format = static_cast<int>(tiff.integer(ifd, kSampleFormat, 1));
  layout.compression = static_cast<int>(tiff.integer(ifd, kCompression, 1));
  layout.predictor = static_cast<int>(tiff.integer(ifd, kPredictor, 1));
  layout.little = tiff.little();
  layout.packing = packing;
  if (layout.width == 0 || layout.height == 0) return fail(Status::Decode, "RAW image has no size");
  if (layout.spp < 1 || layout.spp > 4) return fail(Status::Decode, "RAW images with {} samples per pixel are not supported", layout.spp);
  if (tiff.integer(ifd, kPlanarConfiguration, 1) != 1 && layout.spp > 1) {
    return fail(Status::Decode, "planar RAW data is not supported");
  }
  if (layout.sample_format == 3) {
    if (layout.bits != 16 && layout.bits != 24 && layout.bits != 32) {
      return fail(Status::Decode, "{}-bit floating-point RAW data is not supported", layout.bits);
    }
  } else if (layout.bits < 1 || layout.bits > 16) {
    return fail(Status::Decode, "{}-bit RAW samples are not supported", layout.bits);
  }
  const int p = layout.predictor;
  if (p != 1 && p != 2 && p != 3 && (p < 34892 || p > 34895)) return fail(Status::Decode, "TIFF predictor {} is not supported", p);
  CVT_TRY(ctx.limits.check_pixels(layout.width, layout.height));

  // Chunks: tiles or strips.
  std::vector<Chunk> chunks;
  const bool tiled = ifd.has(kTileOffsets);
  std::vector<std::uint64_t> offsets = tiff.integers(ifd, tiled ? kTileOffsets : kStripOffsets, 1u << 22);
  std::vector<std::uint64_t> counts = tiff.integers(ifd, tiled ? kTileByteCounts : kStripByteCounts, 1u << 22);
  if (offsets.empty()) return fail(Status::Decode, "RAW image has no data");
  if (counts.size() < offsets.size()) {
    // A single strip without a byte count: assume it runs to the end of the file.
    if (offsets.size() == 1 && counts.empty()) counts.push_back(tiff.data().size());
    else return fail(Status::Decode, "RAW image byte counts are missing");
  }
  std::uint64_t total_bytes = 0;
  if (tiled) {
    const std::uint64_t tw = tiff.integer(ifd, kTileWidth);
    const std::uint64_t th = tiff.integer(ifd, kTileLength);
    if (tw == 0 || th == 0 || tw > 65536 || th > 65536) return fail(Status::Decode, "invalid RAW tile size");
    const std::uint64_t across = (layout.width + tw - 1) / tw;
    const std::uint64_t down = (layout.height + th - 1) / th;
    if (across * down > offsets.size()) return fail(Status::Decode, "RAW image has too few tiles");
    for (std::uint64_t i = 0; i < across * down; ++i) {
      Chunk c;
      c.x = static_cast<std::uint32_t>((i % across) * tw);
      c.y = static_cast<std::uint32_t>((i / across) * th);
      c.width = static_cast<std::uint32_t>(tw);
      c.height = static_cast<std::uint32_t>(th);
      chunks.push_back(c);
    }
  } else {
    std::uint64_t rows = tiff.integer(ifd, kRowsPerStrip, layout.height);
    if (rows == 0 || rows > layout.height) rows = layout.height;
    const std::uint64_t strips = (layout.height + rows - 1) / rows;
    if (strips > offsets.size()) return fail(Status::Decode, "RAW image has too few strips");
    for (std::uint64_t i = 0; i < strips; ++i) {
      Chunk c;
      c.x = 0;
      c.y = static_cast<std::uint32_t>(i * rows);
      c.width = layout.width;
      c.height = static_cast<std::uint32_t>(std::min<std::uint64_t>(rows, layout.height - c.y));
      chunks.push_back(c);
    }
  }
  // Data ranges: offsets are absolute in plain files; relative to the
  // structure base in embedded ones.
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    const std::uint64_t at = tiff.base() + offsets[i];
    std::uint64_t length = counts[i];
    if (at >= tiff.data().size()) return fail(Status::Decode, "RAW data offset is outside the file");
    length = std::min<std::uint64_t>(length, tiff.data().size() - at);
    chunks[i].data = tiff.data().subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(length));
    total_bytes += length;
  }
  // Reject sizes the data cannot possibly describe (lossless JPEG needs at
  // least one bit per sample; Deflate is bounded by the decompression limit).
  const std::uint64_t samples = std::uint64_t{layout.width} * layout.height * static_cast<std::uint64_t>(layout.spp);
  if (layout.compression == 1 || layout.compression == 7 || layout.compression == 34892) {
    const std::uint64_t ratio = layout.compression == 1 ? 1 : (layout.compression == 7 ? 9 : 2000);
    if (samples > (total_bytes + 64) * 8 * ratio) return fail(Status::Decode, "RAW image is larger than its data");
  }
  CVT_TRY(ctx.limits.check_decompressed(samples * (layout.sample_format == 3 ? 4 : 2)));

  Sensor sensor;
  sensor.width = layout.width;
  sensor.height = layout.height;
  sensor.spp = layout.spp;
  if (layout.sample_format == 3) {
    sensor.bits = 32;
    sensor.f32.assign(static_cast<std::size_t>(samples), 0.0f);
  } else {
    sensor.bits = layout.compression == 34892 ? 8 : layout.bits;
    sensor.u16.assign(static_cast<std::size_t>(samples), 0);
  }
  if (layout.compression == 7 && layout.sample_format == 3) return fail(Status::Decode, "floating-point lossless JPEG is invalid");
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    if ((i & 15) == 0) CVT_TRY(ctx.checkpoint(0.5 * static_cast<double>(i) / static_cast<double>(chunks.size()), "Decoding RAW data"));
    CVT_TRY(decode_chunk(layout, chunks[i], sensor, ctx));
  }
  return sensor;
}

}  // namespace cvt::image_modern::raw
