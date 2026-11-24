// Convertible core - image_modern family: the camera RAW processing model
// shared by the container readers (DNG and maker formats) and the
// development pipeline (levels, white balance, demosaic, colour, crop).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/image_modern/raw_tiff.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern::raw {

/// Colour filter array: colour index (0 = red, 1 = green, 2 = blue) per
/// position of a repeating `width` x `height` tile, anchored at the top-left
/// pixel of the decoded sensor buffer.
struct Cfa {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::array<std::uint8_t, 64> colors{};

  [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0 && width * height <= colors.size(); }
  [[nodiscard]] int at(std::uint32_t x, std::uint32_t y) const noexcept {
    return colors[(y % height) * width + (x % width)];
  }
  [[nodiscard]] bool bayer() const noexcept;
  /// The same pattern seen from pixel (dx, dy) as origin.
  [[nodiscard]] Cfa shifted(std::uint32_t dx, std::uint32_t dy) const noexcept;
  /// A 2x2 pattern from a name such as "RGGB".
  [[nodiscard]] static Cfa bayer_from(std::string_view rggb);
};

/// Decoded sensor samples before any processing: integer (`u16`) or
/// floating point (`f32`), `spp` interleaved samples per pixel.
struct Sensor {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int spp = 1;
  int bits = 16;  ///< significant bits of integer data; 32 for float
  std::vector<std::uint16_t> u16;
  std::vector<float> f32;

  [[nodiscard]] bool floating() const noexcept { return !f32.empty(); }
};

/// Black and white levels (DNG semantics: the black pattern repeats from the
/// top-left of the active area).
struct Levels {
  std::vector<std::uint16_t> linearization;
  std::uint32_t black_rows = 1;
  std::uint32_t black_cols = 1;
  std::vector<double> black;          ///< black_rows * black_cols * spp (empty = 0)
  std::vector<double> black_delta_h;  ///< per active-area column (may be empty)
  std::vector<double> black_delta_v;  ///< per active-area row (may be empty)
  std::vector<double> white;          ///< per sample (empty = full range of `bits`)
  /// Estimate the white level from a pile-up of the brightest values (maker
  /// formats that do not record it).
  bool estimate_white = false;
};

using Matrix3 = std::array<double, 9>;

/// Colour description of the camera.
struct ColorInfo {
  /// Camera neutral (DNG AsShotNeutral), one value per colour plane.
  std::vector<double> neutral;
  /// Alternative to `neutral`: as-shot multipliers per colour (R, G, B).
  std::vector<double> wb_gains;
  /// DNG AsShotWhiteXY, used when no neutral is given.
  std::optional<std::array<double, 2>> white_xy;
  /// XYZ -> camera matrices for two calibration illuminants (3x3, row-major).
  std::optional<Matrix3> color_matrix1, color_matrix2;
  std::optional<Matrix3> forward_matrix1, forward_matrix2;
  std::optional<Matrix3> calibration1, calibration2;
  std::optional<std::array<double, 3>> analog_balance;
  int illuminant1 = 0;
  int illuminant2 = 0;
  /// DNG BaselineExposure + BaselineExposureOffset, in stops.
  double baseline_exposure = 0.0;
};

/// Everything the development pipeline needs besides the samples.
struct RawInfo {
  /// Set for colour-filter-array data (spp must be 1).
  std::optional<Cfa> cfa;
  Levels levels;
  ColorInfo color;
  /// Active area in sensor pixels: top, left, bottom, right (0,0,0,0 = all).
  std::array<std::uint32_t, 4> active{};
  /// Default crop relative to the active area (size 0 = no crop).
  double crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
  int orientation = 1;
  /// DNG GainMap opcodes (OpcodeList2), applied to the normalised data.
  struct GainMap {
    std::uint32_t top = 0, left = 0, bottom = 0, right = 0;  ///< area in active-area pixels
    std::uint32_t plane = 0, planes = 1, row_pitch = 1, col_pitch = 1;
    std::uint32_t points_v = 0, points_h = 0, map_planes = 1;
    double spacing_v = 0, spacing_h = 0, origin_v = 0, origin_h = 0;  ///< relative to the image size
    std::vector<float> gains;  ///< points_v * points_h * map_planes
  };
  std::vector<GainMap> gain_maps;
  std::string make;
  std::string model;
};

/// User choices for development.
struct DevelopOptions {
  enum class Demosaic { Adaptive, Malvar, Bilinear } demosaic = Demosaic::Adaptive;
  enum class WhiteBalance { Camera, Auto, None } white_balance = WhiteBalance::Camera;
  enum class Tone { Clip, Filmic, Reinhard } tone = Tone::Clip;
  double exposure = 0.0;  ///< extra stops
};

/// Turn decoded sensor data into an image: linear sRGB in `hdr` plus an
/// 8-bit rendition in `frames[0]`, orientation applied.
[[nodiscard]] Result<ir::Image> develop(Sensor sensor, const RawInfo& info, const DevelopOptions& options,
                                        const Context& ctx);

/// Demosaic a normalised CFA plane (`width * height` floats) into `rgba`
/// (`width * height * 4` floats; alpha untouched).
void demosaic(const std::vector<float>& plane, std::uint32_t width, std::uint32_t height, const Cfa& cfa,
              DevelopOptions::Demosaic method, std::vector<float>& rgba);

// --------------------------------------------------------------------------- reading samples

/// How compression 1 data with an unusual bit depth is laid out.
enum class Packing {
  Auto,        ///< TIFF packing (MSB first, rows byte aligned), or 16-bit
               ///< containers when the byte count says so
  MsbPacked,   ///< TIFF packing
  Container16  ///< one sample per 16-bit word in file byte order
};

/// Read the samples of a TIFF/DNG image directory (strips or tiles;
/// uncompressed, lossless JPEG, Deflate with integer and floating-point
/// predictors, lossy JPEG tiles).
[[nodiscard]] Result<Sensor> read_samples(const TiffData& tiff, const Ifd& ifd, const Context& ctx,
                                          Packing packing = Packing::Auto);

/// Unpack `height` rows of `width * spp` samples of `bits` from `data`.
[[nodiscard]] Result<void> unpack_rows(ByteView data, std::uint32_t width, std::uint32_t height, int spp, int bits,
                                       bool little_endian, Packing packing, std::uint16_t* out,
                                       std::size_t out_stride);

// --------------------------------------------------------------------------- previews

/// An embedded preview picture.
struct Preview {
  enum class Kind { Jpeg, Rgb8 } kind = Kind::Jpeg;
  ByteView data;
  std::uint32_t width = 0;   ///< 0 when unknown
  std::uint32_t height = 0;
  int orientation = 1;       ///< applied when the decoded picture carries no EXIF
};

/// Dimensions from a JPEG's SOF marker.
[[nodiscard]] std::optional<std::array<std::uint32_t, 2>> jpeg_size(ByteView jpeg);
/// Add the previews of a TIFF structure (reduced-resolution IFDs, JPEG
/// interchange pointers) to `out`.
void collect_tiff_previews(const TiffTree& tree, int orientation, std::vector<Preview>& out);
/// Add a JPEG found at a range of the file.
void add_jpeg_preview(ByteView jpeg, int orientation, std::vector<Preview>& out);
/// Decode the largest preview. Fails when there is none.
[[nodiscard]] Result<ir::Image> decode_largest_preview(const std::vector<Preview>& previews, const Context& ctx);

}  // namespace cvt::image_modern::raw
