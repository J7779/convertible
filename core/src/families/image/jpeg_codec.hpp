// Convertible core - image family: JPEG codec interface for use inside the
// family (TIFF JPEG compression, MPO, the "mjpeg" video codec ...).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::image::jpeg {

// --------------------------------------------------------------------------- shared tables

/// Natural (row-major) index of the k-th coefficient in zig-zag order.
inline constexpr std::array<std::uint8_t, 64> zigzag = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/// Annex K quantisation tables (natural order).
inline constexpr std::array<std::uint8_t, 64> luma_quant = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,  14, 13, 16, 24,  40,  57,
    69, 56, 14, 17, 22,  29,  51,  87,  80, 62, 18, 22, 37,  56,  68,  109, 103, 77, 24, 35, 55,  64,
    81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
inline constexpr std::array<std::uint8_t, 64> chroma_quant = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

/// A Huffman table in DHT form: counts of codes of each length 1..16, then symbols.
struct HuffmanSpec {
  std::array<std::uint8_t, 16> counts{};
  std::vector<std::uint8_t> symbols;
};

/// The Annex K tables (used when a stream defines none, e.g. AVI MJPEG).
[[nodiscard]] const HuffmanSpec& standard_dc_luma();
[[nodiscard]] const HuffmanSpec& standard_ac_luma();
[[nodiscard]] const HuffmanSpec& standard_dc_chroma();
[[nodiscard]] const HuffmanSpec& standard_ac_chroma();

/// libjpeg's quality scaling: the table of `base` scaled for quality 1..100,
/// entries clamped to 1..255 (baseline-compatible).
[[nodiscard]] std::array<std::uint16_t, 64> scaled_quant(const std::array<std::uint8_t, 64>& base, int quality);

// --------------------------------------------------------------------------- decoding

enum class ColorModel { Gray, YCbCr, Rgb, Cmyk, Ycck };

/// One decoded component, cropped to its own sampled size.
struct Plane {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// Sampling factors from the frame header.
  int h = 1;
  int v = 1;
  /// `width * height` samples (8-bit sources) ...
  std::vector<std::uint8_t> samples;
  /// ... or `width * height` 12-bit samples (12-bit sources only).
  std::vector<std::uint16_t> samples12;
};

struct Planes {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int precision = 8;
  int max_h = 1;
  int max_v = 1;
  ColorModel model = ColorModel::YCbCr;
  /// True for Adobe CMYK/YCCK files, whose ink values are stored inverted.
  bool adobe_inverted = false;
  std::vector<Plane> components;
};

/// Everything a full decode returns besides the pixels.
struct Extras {
  Bytes exif;   ///< APP1 payload after "Exif\0\0"
  Bytes icc;    ///< reassembled APP2 ICC profile
  std::string xmp;
  std::string comment;
  double dpi_x = 0.0;
  double dpi_y = 0.0;
  bool progressive = false;
  bool arithmetic = false;
  bool has_mpf = false;  ///< an APP2 "MPF" segment (multi-picture file)
};

struct DecodeOptions {
  /// JPEGTables of a TIFF file (an abbreviated stream with DQT/DHT), decoded
  /// before `data`; may be empty.
  ByteView tables;
  /// Insert the Annex K Huffman tables when the stream defines none.
  bool default_huffman = true;
};

/// Decode to component planes (no colour conversion, no upsampling).
[[nodiscard]] Result<Planes> decode_planes(ByteView data, const Context& ctx, const DecodeOptions& options = {},
                                           Extras* extras = nullptr);

/// Convert planes to RGBA with centred ("fancy") chroma upsampling.
[[nodiscard]] Result<ir::Frame> planes_to_frame(const Planes& planes, const Context& ctx);

/// Full decode: pixels upright (EXIF orientation applied), metadata attached,
/// 12-bit sources also in `hdr`.
[[nodiscard]] Result<ir::Image> decode(ByteView data, const Context& ctx, const DecodeOptions& options = {});

// --------------------------------------------------------------------------- encoding

enum class Subsampling { Auto, S444, S422, S420 };

struct EncodeOptions {
  int quality = 90;
  Subsampling subsampling = Subsampling::Auto;
  bool progressive = false;
  /// Two-pass optimal Huffman tables (always on for progressive output).
  bool optimize = true;
  /// Write a single grey component.
  bool grayscale = false;
  double dpi_x = 0.0;
  double dpi_y = 0.0;
  /// Optional metadata segments (written only when non-empty).
  Bytes exif;
  Bytes icc;
  std::string xmp;
  std::string comment;
};

/// Encode an opaque RGBA frame (alpha is ignored; flatten first).
[[nodiscard]] Result<Bytes> encode(const ir::Frame& frame, const EncodeOptions& options, const Context& ctx);

/// Encode Y, Cb, Cr planes directly (full-range BT.601, chroma centred).
/// `subsampling` must match the chroma plane sizes: S444 (w x h), S422
/// ((w+1)/2 x h) or S420 ((w+1)/2 x (h+1)/2). Pass empty Cb/Cr for grey.
struct PlanarInput {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  Subsampling subsampling = Subsampling::S420;
  const std::uint8_t* y = nullptr;
  std::size_t y_stride = 0;
  const std::uint8_t* cb = nullptr;
  std::size_t cb_stride = 0;
  const std::uint8_t* cr = nullptr;
  std::size_t cr_stride = 0;
};
[[nodiscard]] Result<Bytes> encode_planes(const PlanarInput& input, const EncodeOptions& options, const Context& ctx);

}  // namespace cvt::image::jpeg
