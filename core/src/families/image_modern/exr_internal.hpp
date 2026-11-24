// Convertible core - image_modern family: OpenEXR internals shared by the
// container code (exr.cpp) and the chunk codecs (exr_codecs.cpp, exr_piz.cpp,
// exr_dwa.cpp).
//
// Every chunk codec works on the uncompressed layout OpenEXR defines for a
// chunk: for each line of the chunk, top to bottom, for each channel in
// channel-list order (alphabetical), the channel's samples of that line
// (lines that the channel's y sampling skips hold nothing), little-endian.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern::exr {

// --------------------------------------------------------------------------- basic types

enum class PixelType : std::uint8_t { Uint = 0, Half = 1, Float = 2 };

[[nodiscard]] constexpr std::uint32_t pixel_size(PixelType type) noexcept { return type == PixelType::Half ? 2 : 4; }

enum class Compression : std::uint8_t {
  None = 0,
  Rle = 1,
  Zips = 2,
  Zip = 3,
  Piz = 4,
  Pxr24 = 5,
  B44 = 6,
  B44a = 7,
  Dwaa = 8,
  Dwab = 9,
};

/// Scan lines per chunk of a scan-line image.
[[nodiscard]] constexpr std::int32_t lines_per_chunk(Compression compression) noexcept {
  switch (compression) {
    case Compression::None:
    case Compression::Rle:
    case Compression::Zips: return 1;
    case Compression::Zip:
    case Compression::Pxr24: return 16;
    case Compression::Piz:
    case Compression::B44:
    case Compression::B44a:
    case Compression::Dwaa: return 32;
    case Compression::Dwab: return 256;
  }
  return 1;
}

/// IEEE 754 binary16 <-> binary32. `float_to_half` rounds to nearest even,
/// overflows to infinity and keeps NaN payload bits like OpenEXR does.
[[nodiscard]] float half_to_float(std::uint16_t bits) noexcept;
[[nodiscard]] std::uint16_t float_to_half(float value) noexcept;

// --------------------------------------------------------------------------- chunks

/// One channel as seen by a chunk codec.
struct ChunkChannel {
  std::string name;
  PixelType type = PixelType::Half;
  bool p_linear = false;
  std::int32_t x_sampling = 1;
  std::int32_t y_sampling = 1;
  /// Samples of this channel in the chunk, per line and number of lines.
  std::int32_t width = 0;
  std::int32_t height = 0;
};

/// The pixel rectangle a chunk covers and the channels stored in it.
struct ChunkLayout {
  std::int32_t x0 = 0;
  std::int32_t y0 = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::vector<ChunkChannel> channels;
  /// Size of the uncompressed chunk in bytes.
  std::uint64_t raw_size = 0;

  /// True when the channel has samples on absolute line `y`.
  [[nodiscard]] static bool has_line(const ChunkChannel& channel, std::int64_t y) noexcept {
    const std::int64_t s = channel.y_sampling;
    return ((y % s) + s) % s == 0;
  }
};

/// Fill in `width`, `height` of every channel and `raw_size` for the
/// rectangle (the rectangle and sampling must already be validated).
void finish_layout(ChunkLayout& layout);

/// Decompress one chunk into its uncompressed layout (exactly
/// `layout.raw_size` bytes). `packed` holding `raw_size` bytes or more is
/// stored data, whatever the compression. Fails with Status::Decode on
/// corrupt data.
[[nodiscard]] Result<Bytes> decompress_chunk(Compression compression, const ChunkLayout& layout, ByteView packed);

/// Compress one uncompressed chunk (None, Rle, Zips, Zip, Piz only). The
/// caller stores `raw` instead when the result is not smaller.
[[nodiscard]] Result<Bytes> compress_chunk(Compression compression, const ChunkLayout& layout, ByteView raw,
                                           int zlib_level);

// --------------------------------------------------------------------------- shared helpers

/// The byte predictor + interleave used by ZIP, ZIPS, RLE and the DWA DC
/// stream: `zip_reconstruct` undoes what `zip_prepare` does.
[[nodiscard]] Bytes zip_prepare(ByteView raw);
[[nodiscard]] Bytes zip_reconstruct(ByteView data);

/// OpenEXR's byte RLE. `rle_decode` fails unless exactly `expected` bytes
/// come out.
[[nodiscard]] Bytes rle_encode(ByteView data);
[[nodiscard]] Result<Bytes> rle_decode(ByteView data, std::uint64_t expected);

/// zlib-decompress exactly `expected` bytes.
[[nodiscard]] Result<Bytes> zlib_exact(ByteView data, std::uint64_t expected);

/// The canonical Huffman coder shared by PIZ and DWA (the 20-byte header,
/// packed code-length table and run-length symbol of OpenEXR).
[[nodiscard]] Bytes huf_compress(std::span<const std::uint16_t> values);
[[nodiscard]] Result<void> huf_decompress(ByteView data, std::span<std::uint16_t> out);

[[nodiscard]] Result<Bytes> piz_decompress(const ChunkLayout& layout, ByteView packed);
[[nodiscard]] Result<Bytes> piz_compress(const ChunkLayout& layout, ByteView raw);
[[nodiscard]] Result<Bytes> dwa_decompress(const ChunkLayout& layout, ByteView packed);

// --------------------------------------------------------------------------- little-endian access

[[nodiscard]] inline std::uint16_t load16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(p[0]) |
                                    (std::to_integer<std::uint16_t>(p[1]) << 8));
}
[[nodiscard]] inline std::uint32_t load32(const std::byte* p) noexcept {
  return std::to_integer<std::uint32_t>(p[0]) | (std::to_integer<std::uint32_t>(p[1]) << 8) |
         (std::to_integer<std::uint32_t>(p[2]) << 16) | (std::to_integer<std::uint32_t>(p[3]) << 24);
}
[[nodiscard]] inline std::uint64_t load64(const std::byte* p) noexcept {
  return std::uint64_t{load32(p)} | (std::uint64_t{load32(p + 4)} << 32);
}
inline void store16(std::byte* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::byte>(v & 0xFFu);
  p[1] = static_cast<std::byte>(v >> 8);
}
inline void store32(std::byte* p, std::uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}

}  // namespace cvt::image_modern::exr
