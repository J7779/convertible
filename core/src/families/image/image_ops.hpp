// Convertible core - image family: the shared raster toolbox.
//
// Pixel operations every family that touches pictures needs: resampling,
// orientation, alpha flattening, colour quantisation with dithering, bit
// depth and greyscale reduction, premultiplication, sRGB <-> linear
// conversion and HDR tone mapping. Everything works on `ir::Frame` (8-bit
// sRGB RGBA, straight alpha) or `ir::HdrPixels` (linear float RGBA).
//
// This header is stable and dependency-light (only the image IR, the error
// model and the options type): other families include it directly, e.g. a
// PDF writer that downsamples embedded pictures or a GIF muxer that needs a
// palette:
//
//   #include "families/image/image_ops.hpp"
//   CVT_TRY_ASSIGN(ir::Frame thumb, image::resize(frame, 256, 192));
//   CVT_TRY_ASSIGN(image::Quantized q, image::quantize(frame, {.max_colors = 64}));
//
// All functions are deterministic (identical output on every platform for
// identical input), re-entrant and free of global mutable state. Functions
// that allocate in proportion to their arguments return `Result` and fail
// with Status::Limit when the requested size cannot be represented.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/image.hpp"

namespace cvt::image {

/// An opaque sRGB colour.
using Rgb = std::array<std::uint8_t, 3>;
/// An sRGB colour with straight alpha.
using Rgba = std::array<std::uint8_t, 4>;

// --------------------------------------------------------------------------- colour transfer

/// sRGB-encoded 8-bit value -> linear light 0..1 (exact IEC 61966-2-1 curve, table driven).
[[nodiscard]] float srgb_to_linear(std::uint8_t value) noexcept;
/// sRGB-encoded 0..1 -> linear light 0..1 (exact curve, any input precision).
[[nodiscard]] float srgb_to_linear(float encoded) noexcept;
/// Linear light -> sRGB-encoded 0..1 (input clamped to 0..1).
[[nodiscard]] float linear_to_srgb(float linear) noexcept;
/// Linear light -> sRGB-encoded 8-bit value, rounded to nearest (clamped).
[[nodiscard]] std::uint8_t linear_to_srgb8(float linear) noexcept;
/// Linear light -> sRGB-encoded 16-bit value, rounded to nearest (clamped).
[[nodiscard]] std::uint16_t linear_to_srgb16(float linear) noexcept;

/// 8-bit RGBA frame -> linear float pixels (alpha straight, 0..1).
[[nodiscard]] ir::HdrPixels to_linear(const ir::Frame& frame);
/// Linear float pixels with values in 0..1 -> 8-bit sRGB (clamped, no tone
/// mapping; use `tone_map` for real HDR content).
[[nodiscard]] ir::Frame from_linear(const ir::HdrPixels& pixels);

/// 16-bit sRGB-encoded samples (R, G, B, A per pixel; 65535 = 1.0) -> linear
/// float pixels, exact: `linear_to_srgb16(to_linear16(v)) == v` for every v.
[[nodiscard]] ir::HdrPixels linear_from_srgb16(std::span<const std::uint16_t> rgba16, std::uint32_t width,
                                               std::uint32_t height);
/// Linear float pixels -> 16-bit sRGB-encoded samples (alpha scaled to 0..65535).
[[nodiscard]] std::vector<std::uint16_t> srgb16_from_linear(const ir::HdrPixels& pixels);

// --------------------------------------------------------------------------- tone mapping

enum class ToneMapOperator {
  Auto,      ///< Clamp when every value is <= 1, otherwise Filmic
  Clamp,     ///< clip to 0..1 (exposure still applies)
  Reinhard,  ///< extended Reinhard on luminance, white point = brightest pixel
  Filmic,    ///< ACES-like filmic curve (Narkowicz fit) per channel
};

struct ToneMapOptions {
  ToneMapOperator op = ToneMapOperator::Auto;
  /// Exposure adjustment in stops applied before the curve (0 = none).
  float exposure_stops = 0.0f;
};

/// Map linear HDR pixels to an 8-bit sRGB frame. NaN and negative values
/// become 0; alpha is clamped.
[[nodiscard]] ir::Frame tone_map(const ir::HdrPixels& pixels, const ToneMapOptions& options = {});

// --------------------------------------------------------------------------- alpha

/// Premultiply / un-premultiply 8-bit RGBA in place (sRGB space, rounding to
/// nearest). `unpremultiply(premultiply(x))` loses precision only for very
/// transparent pixels, which is inherent to 8-bit storage.
void premultiply(ir::Frame& frame) noexcept;
void unpremultiply(ir::Frame& frame) noexcept;

/// Blend every pixel onto an opaque background colour (sRGB space, as
/// browsers composite) and make the frame opaque.
void flatten_alpha(ir::Frame& frame, Rgb background) noexcept;
void flatten_alpha(ir::Image& image, Rgb background) noexcept;

/// True when every pixel has alpha 255.
[[nodiscard]] bool is_opaque(const ir::Frame& frame) noexcept;
/// True when every pixel has R == G == B (alpha ignored).
[[nodiscard]] bool is_grayscale(const ir::Frame& frame) noexcept;
/// True when every alpha value is 0 or 255.
[[nodiscard]] bool has_binary_alpha(const ir::Frame& frame) noexcept;

/// Parse a colour typed by a user: "rrggbb", "#rrggbb", "#rgb", "rgb" or
/// one of the names white, black, red, green, blue, gray/grey, transparent
/// (= white for flattening). ASCII case-insensitive. InvalidArgument otherwise.
[[nodiscard]] Result<Rgb> parse_color(std::string_view text);

// --------------------------------------------------------------------------- resampling

enum class ResampleFilter {
  Auto,      ///< Lanczos3 for any scale change (area-correct when shrinking); Nearest never
  Nearest,   ///< point sampling (pixel art)
  Box,       ///< exact area average when shrinking, nearest when enlarging
  Triangle,  ///< bilinear (tent), widened when shrinking
  Mitchell,  ///< Mitchell-Netravali (B = C = 1/3), soft and ring-free
  Lanczos3,  ///< windowed sinc, radius 3: the sharpest; widened when shrinking
};

struct ResizeOptions {
  ResampleFilter filter = ResampleFilter::Auto;
  /// Filter in linear light (gamma-correct). Turning it off filters the
  /// sRGB-encoded values directly, which darkens fine detail.
  bool linear_light = true;
};

/// Resample a frame to `width` x `height` (both > 0). Colour is filtered in
/// linear light with premultiplied alpha, so there are no dark fringes
/// around transparent areas and no shift: pixel centres map onto pixel
/// centres. A same-size request returns a copy. Fails with Status::Limit
/// when the target size overflows memory arithmetic.
[[nodiscard]] Result<ir::Frame> resize(const ir::Frame& frame, std::uint32_t width, std::uint32_t height,
                                       const ResizeOptions& options = {});
/// The same for linear HDR pixels (values above 1 are kept).
[[nodiscard]] Result<ir::HdrPixels> resize(const ir::HdrPixels& pixels, std::uint32_t width, std::uint32_t height,
                                           const ResizeOptions& options = {});
/// Resize every frame (and the HDR pixels) of an image; scales the dpi.
[[nodiscard]] Result<void> resize(ir::Image& image, std::uint32_t width, std::uint32_t height,
                                  const ResizeOptions& options = {});

/// Apply the standard `scale`, `max_width` and `max_height` options
/// (`options::resolved_image_size`) to an image in place. Returns true when
/// the image was resized. Pictures are never enlarged by max_width/height.
[[nodiscard]] Result<bool> apply_size_options(ir::Image& image, const Options& options,
                                              const ResizeOptions& resize_options = {});

// --------------------------------------------------------------------------- orientation

/// Rotate clockwise by 90, 180 or 270 degrees (other values: copy).
[[nodiscard]] ir::Frame rotate(const ir::Frame& frame, int degrees_clockwise);
[[nodiscard]] ir::Frame flip_horizontal(const ir::Frame& frame);
[[nodiscard]] ir::Frame flip_vertical(const ir::Frame& frame);

/// Turn a frame stored with EXIF orientation `orientation` (1..8; anything
/// else is treated as 1) into its upright form:
///   1 as is, 2 mirror, 3 rotate 180, 4 flip, 5 transpose,
///   6 rotate 90 CW, 7 transverse, 8 rotate 270 CW.
[[nodiscard]] ir::Frame apply_orientation(const ir::Frame& frame, int orientation);
/// Same for linear pixels.
[[nodiscard]] ir::HdrPixels apply_orientation(const ir::HdrPixels& pixels, int orientation);
/// Apply to every frame and the HDR pixels; swaps dpi for transposing orientations.
void apply_orientation(ir::Image& image, int orientation);

// --------------------------------------------------------------------------- greyscale and bit depth

/// Replace colour by its luminance (sRGB-encoded relative luminance of the
/// BT.709 primaries computed in linear light), keeping alpha.
void to_grayscale(ir::Frame& frame) noexcept;

enum class Dither {
  None,            ///< nearest colour
  FloydSteinberg,  ///< error diffusion (serpentine), the best for photos
  Ordered,         ///< 8x8 Bayer matrix; stable across animation frames
};

/// Reduce every channel to `bits` (1..8) significant bits, expanded back to
/// 8 bits by bit replication (so 4-bit 0xA becomes 0xAA), with optional
/// dithering. Alpha is reduced too when `include_alpha`.
void reduce_bit_depth(ir::Frame& frame, int bits, Dither dither = Dither::None, bool include_alpha = false);

/// The smallest bit depth (1, 2, 4, 8) at which every value in the frame's
/// channels survives exactly (for lossless PNG/TIFF/PNM bit depth reduction):
/// value == replicate(value >> (8 - bits)).
[[nodiscard]] int lossless_bit_depth(const ir::Frame& frame, bool include_alpha) noexcept;

// --------------------------------------------------------------------------- quantisation

struct QuantizeOptions {
  /// 2..256 palette entries.
  int max_colors = 256;
  Dither dither = Dither::FloydSteinberg;
  /// 0..1: how much of the quantisation error is diffused / how strong the
  /// ordered pattern is. 1 is classic Floyd-Steinberg.
  float dither_strength = 1.0f;
  /// k-means refinement passes after median cut (0 = none). 1..100 of
  /// `quality` map onto speed in callers; 4 is a good default.
  int refine_iterations = 4;
  /// Keep alpha in the palette (semi-transparent entries allowed). When
  /// false, alpha is forced to 0 or 255 with a threshold of 128 and at most
  /// one fully transparent entry exists.
  bool keep_alpha = true;
  /// Reserve this exact colour as palette entry (e.g. a GIF transparent slot).
  /// Pixels with alpha 0 map onto it when it is transparent.
  std::optional<Rgba> reserved;
};

/// A palette image: one index per pixel into `palette`.
struct Quantized {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<Rgba> palette;
  std::vector<std::uint8_t> indices;
  /// Index of a fully transparent entry, or -1 when none exists.
  int transparent_index = -1;
  /// Mean squared error per channel value of the mapping (diagnostics).
  double mse = 0.0;

  /// Expand back to RGBA.
  [[nodiscard]] ir::Frame to_frame() const;
};

/// The frame's exact palette when it has at most `max_colors` distinct RGBA
/// values (colours of fully transparent pixels are merged into one entry),
/// in first-seen order; nullopt otherwise. Lossless.
[[nodiscard]] std::optional<Quantized> exact_palette(const ir::Frame& frame, int max_colors = 256);

/// Build a palette for one or more frames: median cut on a perceptually
/// weighted, premultiplied colour histogram, refined with k-means.
/// Deterministic. When the frames have no more than `max_colors` distinct
/// colours, the exact set is returned.
[[nodiscard]] std::vector<Rgba> build_palette(std::span<const ir::Frame* const> frames, const QuantizeOptions& options);

/// Map a frame onto a fixed palette with the requested dithering.
/// `palette` must have 1..256 entries.
[[nodiscard]] Result<Quantized> remap(const ir::Frame& frame, std::span<const Rgba> palette,
                                      const QuantizeOptions& options);

/// `exact_palette` when it fits, else `build_palette` + `remap`.
[[nodiscard]] Result<Quantized> quantize(const ir::Frame& frame, const QuantizeOptions& options = {});

/// Perceptually weighted squared distance between two colours (the metric
/// used by the quantiser; premultiplied, green weighted most).
[[nodiscard]] std::uint32_t color_distance(Rgba a, Rgba b) noexcept;

}  // namespace cvt::image
