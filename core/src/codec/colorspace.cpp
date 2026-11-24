// Convertible core - conversion between RGBA and the YCbCr pixel formats.
//
// Everything works one row at a time on double-precision samples:
//
//   reader:  stored samples -> code values -> chroma interpolated to the luma
//            grid (bilinear, at the position given by the siting) -> R'G'B'
//            in 8-bit units (0..255, unrounded, unclamped)
//   writer:  R'G'B' -> Y'CbCr code values at full resolution -> chroma filtered
//            down with the kernel matching the siting -> rounded and stored
//
// The interpolation weights (1/4, 1/2, 3/4) and the sample values are exact
// in binary floating point, so a flat chroma plane stays exactly flat. The
// matrix arithmetic is accurate to about 1e-12 of a code, which makes the
// stored integers equal to the exactly computed and rounded values (see
// quantise() for ties). R'G'B' is gamma-encoded throughout: no transfer
// function is applied or removed.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "codec/colorspace.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

// Keep a * b + c as two IEEE operations so that every platform computes the
// same bits (a fused multiply-add rounds once instead of twice). GCC does not
// contract in ISO C++ mode.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

#if defined(__clang__) || defined(__GNUC__) || defined(_MSC_VER)
#define CVT_COLORSPACE_RESTRICT __restrict
#else
#define CVT_COLORSPACE_RESTRICT
#endif

namespace cvt::codec {
namespace {

using ir::ChromaSiting;
using ir::ColorMatrix;
using ir::ColorRange;
using ir::Plane;

/// Largest value of a 10-bit sample.
constexpr unsigned kPeak10 = 1023;

// ---------------------------------------------------------------------------
// Matrices and quantisation
// ---------------------------------------------------------------------------

struct LumaWeights {
  double kr;
  double kb;
};

[[nodiscard]] constexpr LumaWeights luma_weights(ColorMatrix matrix) noexcept {
  switch (matrix) {
    case ColorMatrix::BT601: return {0.299, 0.114};
    case ColorMatrix::BT709: return {0.2126, 0.0722};
    case ColorMatrix::BT2020: return {0.2627, 0.0593};
  }
  return {0.2126, 0.0722};
}

/// code = offset + scale * normalised value, with Y' in 0..1 and Cb/Cr in
/// -0.5..+0.5.
struct Quantisation {
  double y_offset;
  double y_scale;
  double c_offset;
  double c_scale;
  double peak;  ///< largest representable code, 2^bits - 1
};

[[nodiscard]] constexpr Quantisation quantisation(int bits, ColorRange range) noexcept {
  const double unit = static_cast<double>(1 << (bits - 8));
  const double peak = static_cast<double>((1 << bits) - 1);
  if (range == ColorRange::Full) {
    return {0.0, peak, 128.0 * unit, peak, peak};
  }
  return {16.0 * unit, 219.0 * unit, 128.0 * unit, 224.0 * unit, peak};
}

/// R'G'B' (8-bit units) from Y'CbCr code values:
///   luma = (Y - y_offset) * y_gain, u = Cb - c_offset, v = Cr - c_offset
///   R = luma + r_cr * v, G = luma + g_cb * u + g_cr * v, B = luma + b_cb * u
struct YuvToRgb {
  double y_offset;
  double c_offset;
  double y_gain;
  double r_cr;
  double g_cb;
  double g_cr;
  double b_cb;
};

[[nodiscard]] YuvToRgb yuv_to_rgb(const ColorInfo& color, int bits) noexcept {
  const LumaWeights k = luma_weights(color.matrix);
  const double kg = 1.0 - k.kr - k.kb;
  const Quantisation q = quantisation(bits, color.range);
  const double chroma = 255.0 / q.c_scale;
  YuvToRgb out{};
  out.y_offset = q.y_offset;
  out.c_offset = q.c_offset;
  out.y_gain = 255.0 / q.y_scale;
  out.r_cr = chroma * 2.0 * (1.0 - k.kr);
  out.b_cb = chroma * 2.0 * (1.0 - k.kb);
  out.g_cb = -chroma * 2.0 * (1.0 - k.kb) * k.kb / kg;
  out.g_cr = -chroma * 2.0 * (1.0 - k.kr) * k.kr / kg;
  return out;
}

/// Y'CbCr code values from R'G'B' (8-bit units), one row of a 3x3 matrix per
/// component plus the offsets.
struct RgbToYuv {
  double y_offset;
  double c_offset;
  double y_r, y_g, y_b;
  double cb_r, cb_g, cb_b;
  double cr_r, cr_g, cr_b;
};

[[nodiscard]] RgbToYuv rgb_to_yuv(const ColorInfo& color, int bits) noexcept {
  const LumaWeights k = luma_weights(color.matrix);
  const double kg = 1.0 - k.kr - k.kb;
  const Quantisation q = quantisation(bits, color.range);
  const double luma = q.y_scale / 255.0;
  const double chroma = q.c_scale / 255.0;
  const double cb_span = 2.0 * (1.0 - k.kb);  // Pb = (B' - Y') / cb_span
  const double cr_span = 2.0 * (1.0 - k.kr);  // Pr = (R' - Y') / cr_span
  RgbToYuv out{};
  out.y_offset = q.y_offset;
  out.c_offset = q.c_offset;
  out.y_r = luma * k.kr;
  out.y_g = luma * kg;
  out.y_b = luma * k.kb;
  out.cb_r = -chroma * k.kr / cb_span;
  out.cb_g = -chroma * kg / cb_span;
  out.cb_b = chroma * 0.5;
  out.cr_r = chroma * 0.5;
  out.cr_g = -chroma * kg / cr_span;
  out.cr_b = -chroma * k.kb / cr_span;
  return out;
}

// ---------------------------------------------------------------------------
// Sample access
// ---------------------------------------------------------------------------

/// Where one component (Y, Cb or Cr) lives inside a frame.
struct Channel {
  std::size_t plane = 0;   ///< index into VideoFrame::planes
  std::size_t offset = 0;  ///< byte offset of the first sample within a row
  std::size_t step = 1;    ///< bytes from one sample to the next
  std::size_t width = 0;   ///< samples per row
  std::size_t height = 0;  ///< rows
  bool wide = false;       ///< little-endian 16-bit words instead of bytes
  unsigned shift = 0;      ///< left shift of the stored value (6 for P010)
};

/// `component`: 0 = Y, 1 = Cb, 2 = Cr.
[[nodiscard]] Channel channel_of(PixelFormat format, std::size_t component, std::uint32_t width,
                                 std::uint32_t height) noexcept {
  const std::size_t bytes = ir::bytes_per_sample(format);
  const bool interleaved = component != 0 && (format == PixelFormat::NV12 || format == PixelFormat::P010);
  Channel ch;
  ch.plane = interleaved ? 1 : component;
  ch.offset = (interleaved && component == 2) ? bytes : 0;
  ch.step = interleaved ? 2 * bytes : bytes;
  ch.width = ir::plane_width(format, ch.plane, width);
  ch.height = ir::plane_height(format, ch.plane, height);
  ch.wide = bytes == 2;
  ch.shift = format == PixelFormat::P010 ? 6u : 0u;
  return ch;
}

[[nodiscard]] const std::uint8_t* row_ptr(const VideoFrame& frame, const Channel& ch, std::size_t row) noexcept {
  const Plane& plane = frame.planes[ch.plane];
  return plane.data.data() + row * plane.stride + ch.offset;
}

[[nodiscard]] std::uint8_t* row_ptr(VideoFrame& frame, const Channel& ch, std::size_t row) noexcept {
  Plane& plane = frame.planes[ch.plane];
  return plane.data.data() + row * plane.stride + ch.offset;
}

/// One row of stored samples as code values. 10-bit words are read
/// byte-wise (little-endian); anything above 1023 saturates.
void load_row(const std::uint8_t* src, const Channel& ch, double* CVT_COLORSPACE_RESTRICT dst) noexcept {
  const std::size_t n = ch.width;
  if (!ch.wide) {
    if (ch.step == 1) {
      for (std::size_t i = 0; i < n; ++i) dst[i] = static_cast<double>(src[i]);
    } else {
      const std::size_t step = ch.step;
      for (std::size_t i = 0; i < n; ++i) dst[i] = static_cast<double>(src[i * step]);
    }
    return;
  }
  const std::size_t step = ch.step;
  const unsigned shift = ch.shift;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t* s = src + i * step;
    const unsigned word = static_cast<unsigned>(s[0]) | (static_cast<unsigned>(s[1]) << 8);
    dst[i] = static_cast<double>(std::min(word >> shift, kPeak10));
  }
}

/// Half a code, plus a margin that is far above the arithmetic error and far
/// below anything that matters: a value that is mathematically an exact tie
/// (x.5) always rounds away from zero instead of following the last bit of
/// the floating-point result.
constexpr double kHalf = 0.5 + 1.0 / (1 << 30);

/// Clamp to 0..peak, then round to nearest with ties away from `centre`:
/// 0 for luma and RGB (plain "half up"), the zero-chroma code for Cb and Cr,
/// so that chroma rounds symmetrically about neutral as in ITU-T H.273.
[[nodiscard]] inline std::int32_t quantise(double value, double centre, double peak) noexcept {
  const double excursion = std::min(std::max(value, 0.0), peak) - centre;
  return static_cast<std::int32_t>(centre) + static_cast<std::int32_t>(excursion + (excursion < 0.0 ? -kHalf : kHalf));
}

/// The inverse of load_row: clamp, round and store one row of code values.
void store_row(const double* CVT_COLORSPACE_RESTRICT src, std::uint8_t* CVT_COLORSPACE_RESTRICT dst, const Channel& ch,
               double centre, double peak) noexcept {
  const std::size_t n = ch.width;
  if (!ch.wide) {
    if (ch.step == 1) {
      for (std::size_t i = 0; i < n; ++i) dst[i] = static_cast<std::uint8_t>(quantise(src[i], centre, peak));
    } else {
      const std::size_t step = ch.step;
      for (std::size_t i = 0; i < n; ++i) dst[i * step] = static_cast<std::uint8_t>(quantise(src[i], centre, peak));
    }
    return;
  }
  const std::size_t step = ch.step;
  const unsigned shift = ch.shift;
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned word = static_cast<unsigned>(quantise(src[i], centre, peak)) << shift;
    std::uint8_t* d = dst + i * step;
    d[0] = static_cast<std::uint8_t>(word & 0xFFu);
    d[1] = static_cast<std::uint8_t>(word >> 8);
  }
}

// ---------------------------------------------------------------------------
// Chroma resampling
// ---------------------------------------------------------------------------

/// Position of the chroma samples along one axis.
enum class Phase {
  Full,      ///< not subsampled on this axis
  Cosited,   ///< chroma sample i sits on luma sample 2i
  Centered,  ///< chroma sample i sits midway between luma samples 2i and 2i+1
};

[[nodiscard]] constexpr Phase horizontal_phase(PixelFormat format, ChromaSiting siting) noexcept {
  if (ir::chroma_shift_x(format) == 0) return Phase::Full;
  return siting == ChromaSiting::Center ? Phase::Centered : Phase::Cosited;
}

[[nodiscard]] constexpr Phase vertical_phase(PixelFormat format, ChromaSiting siting) noexcept {
  if (ir::chroma_shift_y(format) == 0) return Phase::Full;
  return siting == ChromaSiting::TopLeft ? Phase::Cosited : Phase::Centered;
}

/// Linear interpolation of a subsampled row (`cw` samples) to `w` luma
/// columns, w == 2 * cw or 2 * cw - 1. Samples beyond the ends replicate.
void upsample_h(const double* CVT_COLORSPACE_RESTRICT src, std::size_t cw, double* CVT_COLORSPACE_RESTRICT dst,
                std::size_t w, Phase phase) noexcept {
  if (phase == Phase::Cosited) {
    // Even columns coincide with a sample, odd ones lie midway between two.
    const std::size_t last = cw - 1;
    for (std::size_t i = 0; i < last; ++i) {
      dst[2 * i] = src[i];
      dst[2 * i + 1] = 0.5 * (src[i] + src[i + 1]);
    }
    dst[2 * last] = src[last];
    if (2 * last + 1 < w) dst[2 * last + 1] = src[last];
    return;
  }
  // Centred: column 2i lies 1/4 of a chroma step left of sample i, column
  // 2i+1 lies 1/4 right of it.
  dst[0] = src[0];
  for (std::size_t i = 0; i + 1 < cw; ++i) {
    dst[2 * i + 1] = 0.75 * src[i] + 0.25 * src[i + 1];
    dst[2 * i + 2] = 0.25 * src[i] + 0.75 * src[i + 1];
  }
  if (w == 2 * cw) dst[w - 1] = src[cw - 1];
}

/// Filter a full-resolution row (`w` samples) down to `cw` = (w + 1) / 2
/// samples: a 2-tap box for centred chroma, [1 2 1] / 4 around the even
/// column for co-sited chroma. Missing neighbours replicate the edge.
void downsample_h(const double* CVT_COLORSPACE_RESTRICT src, std::size_t w, double* CVT_COLORSPACE_RESTRICT dst,
                  std::size_t cw, Phase phase) noexcept {
  if (phase == Phase::Centered) {
    const std::size_t pairs = w / 2;
    for (std::size_t i = 0; i < pairs; ++i) dst[i] = 0.5 * (src[2 * i] + src[2 * i + 1]);
    if (pairs < cw) dst[pairs] = src[w - 1];
    return;
  }
  dst[0] = 0.25 * (src[0] + 2.0 * src[0] + src[w > 1 ? 1 : 0]);
  std::size_t i = 1;
  for (; 2 * i + 1 < w; ++i) dst[i] = 0.25 * (src[2 * i - 1] + 2.0 * src[2 * i] + src[2 * i + 1]);
  if (i < cw) dst[i] = 0.25 * (src[2 * i - 1] + 2.0 * src[2 * i] + src[2 * i]);
}

/// The two chroma rows, and their weights, that interpolate luma row `y`.
struct RowBlend {
  std::size_t a;
  std::size_t b;
  double weight_a;
  double weight_b;
};

[[nodiscard]] RowBlend vertical_blend(std::size_t y, std::size_t chroma_rows, Phase phase) noexcept {
  const std::size_t j = y / 2;
  const std::size_t below = std::min(j + 1, chroma_rows - 1);
  const bool odd = (y & 1u) != 0;
  if (phase == Phase::Cosited) {
    return odd ? RowBlend{j, below, 0.5, 0.5} : RowBlend{j, j, 1.0, 0.0};
  }
  return odd ? RowBlend{j, below, 0.75, 0.25} : RowBlend{j > 0 ? j - 1 : 0, j, 0.25, 0.75};
}

/// Receives the rows of one chroma component in luma-row order (already at
/// the target's chroma width), applies the vertical half of the downsampling
/// kernel and stores the finished rows.
class ChromaRowSink {
 public:
  ChromaRowSink(VideoFrame& out, const Channel& channel, Phase phase, std::size_t luma_rows, double centre,
                double peak)
      : out_(out), ch_(channel), phase_(phase), luma_rows_(luma_rows), centre_(centre), peak_(peak) {
    if (phase_ != Phase::Full) {
      even_.resize(ch_.width);
      mix_.resize(ch_.width);
      if (phase_ == Phase::Cosited) odd_.resize(ch_.width);
    }
  }

  void push(std::size_t y, const double* row) {
    if (phase_ == Phase::Full) {
      emit(y, row);
      return;
    }
    const std::size_t n = ch_.width;
    const bool last = y + 1 == luma_rows_;
    if ((y & 1u) == 0) {
      if (phase_ == Phase::Centered) {
        // Box over rows y and y + 1; a missing last row replicates this one.
        if (last) {
          emit(y / 2, row);
        } else {
          std::copy_n(row, n, even_.data());
        }
        return;
      }
      std::copy_n(row, n, even_.data());
      if (last) {
        // [1 2 1] / 4 with the missing row below replicated.
        blend3(y == 0 ? even_.data() : odd_.data(), even_.data(), even_.data());
        emit(y / 2, mix_.data());
      }
      return;
    }
    if (phase_ == Phase::Centered) {
      const double* even = even_.data();
      double* mix = mix_.data();
      for (std::size_t i = 0; i < n; ++i) mix[i] = 0.5 * (even[i] + row[i]);
    } else {
      // [1 2 1] / 4 over rows y - 2, y - 1 and y; above the top edge the
      // centre row replicates.
      blend3(y == 1 ? even_.data() : odd_.data(), even_.data(), row);
    }
    emit(y / 2, mix_.data());
    if (phase_ == Phase::Cosited) std::copy_n(row, n, odd_.data());
  }

 private:
  void blend3(const double* above, const double* centre, const double* below) noexcept {
    double* mix = mix_.data();
    const std::size_t n = ch_.width;
    for (std::size_t i = 0; i < n; ++i) mix[i] = 0.25 * (above[i] + 2.0 * centre[i] + below[i]);
  }

  void emit(std::size_t chroma_row, const double* row) noexcept {
    store_row(row, row_ptr(out_, ch_, chroma_row), ch_, centre_, peak_);
  }

  VideoFrame& out_;
  Channel ch_;
  Phase phase_;
  std::size_t luma_rows_;
  double centre_;
  double peak_;
  std::vector<double> even_;  ///< the latest even luma row
  std::vector<double> odd_;   ///< the latest odd luma row (co-sited only)
  std::vector<double> mix_;
};

// ---------------------------------------------------------------------------
// Row readers and writers
// ---------------------------------------------------------------------------

/// Reads a valid YCbCr frame row by row.
class YuvReader {
 public:
  explicit YuvReader(const VideoFrame& frame)
      : frame_(frame),
        luma_(channel_of(frame.format, 0, frame.width, frame.height)),
        cb_(channel_of(frame.format, 1, frame.width, frame.height)),
        cr_(channel_of(frame.format, 2, frame.width, frame.height)),
        phase_x_(horizontal_phase(frame.format, frame.color.siting)),
        phase_y_(vertical_phase(frame.format, frame.color.siting)),
        to_rgb_(yuv_to_rgb(frame.color, ir::bit_depth(frame.format))),
        scratch_(phase_y_ == Phase::Full ? 0 : cb_.width) {}

  [[nodiscard]] const Channel& luma() const noexcept { return luma_; }
  [[nodiscard]] const Channel& cb() const noexcept { return cb_; }
  [[nodiscard]] const Channel& cr() const noexcept { return cr_; }
  [[nodiscard]] Phase phase_x() const noexcept { return phase_x_; }
  [[nodiscard]] Phase phase_y() const noexcept { return phase_y_; }

  /// Stored luma row `y` as code values (`luma().width` samples).
  void luma_row(std::size_t y, double* dst) const noexcept { load_row(row_ptr(frame_, luma_, y), luma_, dst); }

  /// Stored chroma row `row` of component `ch` (`ch.width` samples).
  void chroma_row(const Channel& ch, std::size_t row, double* dst) const noexcept {
    load_row(row_ptr(frame_, ch, row), ch, dst);
  }

  /// Chroma of component `ch` interpolated vertically to luma row `y`, still
  /// at the stored chroma width (`ch.width` samples).
  void chroma_line(const Channel& ch, std::size_t y, double* CVT_COLORSPACE_RESTRICT dst) noexcept {
    if (phase_y_ == Phase::Full) {
      chroma_row(ch, y, dst);
      return;
    }
    const RowBlend blend = vertical_blend(y, ch.height, phase_y_);
    chroma_row(ch, blend.a, dst);
    if (blend.a == blend.b) return;
    double* CVT_COLORSPACE_RESTRICT other = scratch_.data();
    chroma_row(ch, blend.b, other);
    const std::size_t n = ch.width;
    const double wa = blend.weight_a;
    const double wb = blend.weight_b;
    for (std::size_t i = 0; i < n; ++i) dst[i] = wa * dst[i] + wb * other[i];
  }

  /// Row `y` as R'G'B' in 8-bit units, unrounded and unclamped.
  void rgb_row(std::size_t y, double* CVT_COLORSPACE_RESTRICT r, double* CVT_COLORSPACE_RESTRICT g,
               double* CVT_COLORSPACE_RESTRICT b) {
    if (y_row_.empty()) {
      y_row_.resize(luma_.width);
      cb_line_.resize(cb_.width);
      cr_line_.resize(cr_.width);
      if (phase_x_ != Phase::Full) {
        cb_full_.resize(luma_.width);
        cr_full_.resize(luma_.width);
      }
    }
    const std::size_t w = luma_.width;
    luma_row(y, y_row_.data());
    chroma_line(cb_, y, cb_line_.data());
    chroma_line(cr_, y, cr_line_.data());
    const double* CVT_COLORSPACE_RESTRICT luma = y_row_.data();
    const double* CVT_COLORSPACE_RESTRICT cb = cb_line_.data();
    const double* CVT_COLORSPACE_RESTRICT cr = cr_line_.data();
    if (phase_x_ != Phase::Full) {
      upsample_h(cb_line_.data(), cb_.width, cb_full_.data(), w, phase_x_);
      upsample_h(cr_line_.data(), cr_.width, cr_full_.data(), w, phase_x_);
      cb = cb_full_.data();
      cr = cr_full_.data();
    }
    const YuvToRgb k = to_rgb_;
    for (std::size_t x = 0; x < w; ++x) {
      const double base = (luma[x] - k.y_offset) * k.y_gain;
      const double u = cb[x] - k.c_offset;
      const double v = cr[x] - k.c_offset;
      r[x] = base + k.r_cr * v;
      g[x] = base + k.g_cb * u + k.g_cr * v;
      b[x] = base + k.b_cb * u;
    }
  }

 private:
  const VideoFrame& frame_;
  Channel luma_;
  Channel cb_;
  Channel cr_;
  Phase phase_x_;
  Phase phase_y_;
  YuvToRgb to_rgb_;
  std::vector<double> scratch_;
  std::vector<double> y_row_;
  std::vector<double> cb_line_;
  std::vector<double> cr_line_;
  std::vector<double> cb_full_;
  std::vector<double> cr_full_;
};

/// Fills an allocated (tightly packed) YCbCr frame from R'G'B' rows that
/// arrive top to bottom.
class YuvWriter {
 public:
  YuvWriter(VideoFrame& out, const ColorInfo& color)
      : out_(out),
        luma_(channel_of(out.format, 0, out.width, out.height)),
        cb_ch_(channel_of(out.format, 1, out.width, out.height)),
        cr_ch_(channel_of(out.format, 2, out.width, out.height)),
        phase_x_(horizontal_phase(out.format, color.siting)),
        from_rgb_(rgb_to_yuv(color, ir::bit_depth(out.format))),
        peak_(quantisation(ir::bit_depth(out.format), color.range).peak),
        cb_sink_(out, cb_ch_, vertical_phase(out.format, color.siting), out.height, from_rgb_.c_offset, peak_),
        cr_sink_(out, cr_ch_, vertical_phase(out.format, color.siting), out.height, from_rgb_.c_offset, peak_),
        y_(luma_.width),
        cb_(luma_.width),
        cr_(luma_.width),
        cb_down_(phase_x_ == Phase::Full ? 0 : cb_ch_.width),
        cr_down_(phase_x_ == Phase::Full ? 0 : cr_ch_.width) {}

  /// Row `y` from R'G'B' in 8-bit units (any real value; clamping happens on
  /// the stored codes).
  void rgb_row(std::size_t y, const double* CVT_COLORSPACE_RESTRICT r, const double* CVT_COLORSPACE_RESTRICT g,
               const double* CVT_COLORSPACE_RESTRICT b) {
    const std::size_t w = luma_.width;
    double* CVT_COLORSPACE_RESTRICT luma = y_.data();
    double* CVT_COLORSPACE_RESTRICT cb = cb_.data();
    double* CVT_COLORSPACE_RESTRICT cr = cr_.data();
    const RgbToYuv k = from_rgb_;
    for (std::size_t x = 0; x < w; ++x) {
      luma[x] = k.y_offset + (k.y_r * r[x] + k.y_g * g[x] + k.y_b * b[x]);
      cb[x] = k.c_offset + (k.cb_r * r[x] + k.cb_g * g[x] + k.cb_b * b[x]);
      cr[x] = k.c_offset + (k.cr_r * r[x] + k.cr_g * g[x] + k.cr_b * b[x]);
    }
    store_row(luma, row_ptr(out_, luma_, y), luma_, 0.0, peak_);
    if (phase_x_ == Phase::Full) {
      cb_sink_.push(y, cb);
      cr_sink_.push(y, cr);
      return;
    }
    downsample_h(cb, w, cb_down_.data(), cb_ch_.width, phase_x_);
    downsample_h(cr, w, cr_down_.data(), cr_ch_.width, phase_x_);
    cb_sink_.push(y, cb_down_.data());
    cr_sink_.push(y, cr_down_.data());
  }

 private:
  VideoFrame& out_;
  Channel luma_;
  Channel cb_ch_;
  Channel cr_ch_;
  Phase phase_x_;
  RgbToYuv from_rgb_;
  double peak_;
  ChromaRowSink cb_sink_;
  ChromaRowSink cr_sink_;
  std::vector<double> y_;
  std::vector<double> cb_;
  std::vector<double> cr_;
  std::vector<double> cb_down_;
  std::vector<double> cr_down_;
};

void unpack_rgba(const std::uint8_t* CVT_COLORSPACE_RESTRICT src, std::size_t w, double* CVT_COLORSPACE_RESTRICT r,
                 double* CVT_COLORSPACE_RESTRICT g, double* CVT_COLORSPACE_RESTRICT b) noexcept {
  for (std::size_t x = 0; x < w; ++x) {
    r[x] = static_cast<double>(src[4 * x]);
    g[x] = static_cast<double>(src[4 * x + 1]);
    b[x] = static_cast<double>(src[4 * x + 2]);
  }
}

void pack_rgba(const double* CVT_COLORSPACE_RESTRICT r, const double* CVT_COLORSPACE_RESTRICT g,
               const double* CVT_COLORSPACE_RESTRICT b, std::size_t w, std::uint8_t* CVT_COLORSPACE_RESTRICT dst) noexcept {
  for (std::size_t x = 0; x < w; ++x) {
    dst[4 * x] = static_cast<std::uint8_t>(quantise(r[x], 0.0, 255.0));
    dst[4 * x + 1] = static_cast<std::uint8_t>(quantise(g[x], 0.0, 255.0));
    dst[4 * x + 2] = static_cast<std::uint8_t>(quantise(b[x], 0.0, 255.0));
    dst[4 * x + 3] = 255;
  }
}

// ---------------------------------------------------------------------------
// Validation and whole-frame helpers
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr bool known_format(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::RGBA8:
    case PixelFormat::I420:
    case PixelFormat::I422:
    case PixelFormat::I444:
    case PixelFormat::NV12:
    case PixelFormat::I420P10:
    case PixelFormat::I422P10:
    case PixelFormat::I444P10:
    case PixelFormat::P010: return true;
  }
  return false;
}

/// Rejects frames that cannot be read safely: wrong layout, planes shorter
/// than their stride and row count imply, or so large that the size of an
/// RGBA copy or of a row buffer could not be computed or allocated.
[[nodiscard]] Result<void> check_frame(const VideoFrame& frame) {
  if (!known_format(frame.format) || !frame.valid()) {
    return fail(Status::InvalidArgument, "video frame is not valid (size, plane count, stride or plane length)");
  }
  constexpr std::uint64_t kMaxPixels = std::uint64_t{1} << 31;
  constexpr std::uint64_t kMaxBytes = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
  const std::uint64_t pixels = std::uint64_t{frame.width} * frame.height;
  const std::uint64_t row_buffer_bytes = std::uint64_t{frame.width} * sizeof(double);
  if (pixels > kMaxPixels || pixels > kMaxBytes / 4 || row_buffer_bytes > kMaxBytes) {
    return fail(Status::Limit, "video frame of {}x{} pixels is too large to convert", frame.width, frame.height);
  }
  // The same test as VideoFrame::valid(), arranged so that a huge stride
  // cannot wrap the multiplication.
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    const Plane& plane = frame.planes[p];
    const std::size_t row_bytes = ir::plane_row_bytes(frame.format, p, frame.width);
    const std::size_t rows = ir::plane_height(frame.format, p, frame.height);
    if (plane.stride < row_bytes || plane.data.size() < row_bytes ||
        rows - 1 > (plane.data.size() - row_bytes) / plane.stride) {
      return fail(Status::InvalidArgument, "video frame plane {} is shorter than its stride and height require", p);
    }
  }
  return {};
}

void carry_timing(const VideoFrame& from, VideoFrame& to) noexcept {
  to.pts_ms = from.pts_ms;
  to.duration_ms = from.duration_ms;
}

/// The same frame with every plane's stride reduced to its row size.
[[nodiscard]] VideoFrame packed_copy(const VideoFrame& frame) {
  VideoFrame out = VideoFrame::allocate(frame.format, frame.width, frame.height);
  out.color = frame.color;
  carry_timing(frame, out);
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    const std::size_t row_bytes = ir::plane_row_bytes(frame.format, p, frame.width);
    const std::size_t rows = ir::plane_height(frame.format, p, frame.height);
    const std::uint8_t* src = frame.planes[p].data.data();
    std::uint8_t* dst = out.planes[p].data.data();
    for (std::size_t row = 0; row < rows; ++row) {
      std::memcpy(dst + row * row_bytes, src + row * frame.planes[p].stride, row_bytes);
    }
  }
  return out;
}

/// YCbCr -> YCbCr with the same matrix: every plane is an affine function of
/// the matching source plane, so no trip through RGB is needed. Chroma is
/// resampled only along the axes on which its sample positions change.
[[nodiscard]] VideoFrame convert_planes(const VideoFrame& frame, PixelFormat format, const ColorInfo& color) {
  VideoFrame out = VideoFrame::allocate(format, frame.width, frame.height);
  out.color = color;
  carry_timing(frame, out);

  const Quantisation from = quantisation(ir::bit_depth(frame.format), frame.color.range);
  const Quantisation to = quantisation(ir::bit_depth(format), color.range);
  const std::size_t w = frame.width;
  const std::size_t h = frame.height;
  YuvReader reader(frame);

  // code_out = offset_out + (code_in - offset_in) * gain
  const auto rescale = [](const double* CVT_COLORSPACE_RESTRICT in, double* CVT_COLORSPACE_RESTRICT result,
                          std::size_t n, double offset_in, double gain, double offset_out) noexcept {
    for (std::size_t i = 0; i < n; ++i) result[i] = offset_out + (in[i] - offset_in) * gain;
  };

  {
    const Channel dst = channel_of(format, 0, frame.width, frame.height);
    const double gain = to.y_scale / from.y_scale;
    std::vector<double> row(w);
    std::vector<double> scaled(w);
    for (std::size_t y = 0; y < h; ++y) {
      reader.luma_row(y, row.data());
      rescale(row.data(), scaled.data(), w, from.y_offset, gain, to.y_offset);
      store_row(scaled.data(), row_ptr(out, dst, y), dst, 0.0, to.peak);
    }
  }

  const Phase src_x = reader.phase_x();
  const Phase src_y = reader.phase_y();
  const Phase dst_x = horizontal_phase(format, color.siting);
  const Phase dst_y = vertical_phase(format, color.siting);
  const double gain = to.c_scale / from.c_scale;

  for (std::size_t component = 1; component <= 2; ++component) {
    const Channel& src = component == 1 ? reader.cb() : reader.cr();
    const Channel dst = channel_of(format, component, frame.width, frame.height);
    std::vector<double> line(src.width);
    std::vector<double> full(src_x != dst_x && src_x != Phase::Full ? w : 0);
    std::vector<double> down(src_x != dst_x && dst_x != Phase::Full ? dst.width : 0);
    std::vector<double> scaled(dst.width);

    // `line` (source chroma width) -> `scaled` (target chroma width, target codes).
    const auto finish_row = [&]() noexcept {
      const double* in = line.data();
      if (src_x != dst_x) {
        if (src_x != Phase::Full) {
          upsample_h(in, src.width, full.data(), w, src_x);
          in = full.data();
        }
        if (dst_x != Phase::Full) {
          downsample_h(in, w, down.data(), dst.width, dst_x);
          in = down.data();
        }
      }
      rescale(in, scaled.data(), dst.width, from.c_offset, gain, to.c_offset);
    };

    if (src_y == dst_y) {
      // Same rows on both sides.
      for (std::size_t row = 0; row < dst.height; ++row) {
        reader.chroma_row(src, row, line.data());
        finish_row();
        store_row(scaled.data(), row_ptr(out, dst, row), dst, to.c_offset, to.peak);
      }
    } else {
      // Interpolate to every luma row, then filter down again if the target
      // is subsampled vertically.
      ChromaRowSink sink(out, dst, dst_y, h, to.c_offset, to.peak);
      for (std::size_t y = 0; y < h; ++y) {
        reader.chroma_line(src, y, line.data());
        finish_row();
        sink.push(y, scaled.data());
      }
    }
  }
  return out;
}

}  // namespace

Result<VideoFrame> to_rgba(const VideoFrame& frame) {
  CVT_TRY(check_frame(frame));
  if (frame.format == PixelFormat::RGBA8) return packed_copy(frame);

  VideoFrame out = VideoFrame::allocate(PixelFormat::RGBA8, frame.width, frame.height);
  out.color = frame.color;
  carry_timing(frame, out);

  const std::size_t w = frame.width;
  const std::size_t h = frame.height;
  YuvReader reader(frame);
  std::vector<double> r(w);
  std::vector<double> g(w);
  std::vector<double> b(w);
  std::uint8_t* dst = out.planes[0].data.data();
  for (std::size_t y = 0; y < h; ++y) {
    reader.rgb_row(y, r.data(), g.data(), b.data());
    pack_rgba(r.data(), g.data(), b.data(), w, dst + y * w * 4);
  }
  return out;
}

Result<VideoFrame> from_rgba(const VideoFrame& rgba, PixelFormat format, const ColorInfo& color) {
  CVT_TRY(check_frame(rgba));
  if (rgba.format != PixelFormat::RGBA8) {
    return fail(Status::InvalidArgument, "from_rgba needs an RGBA8 frame");
  }
  if (!known_format(format)) return fail(Status::InvalidArgument, "unknown target pixel format");
  if (format == PixelFormat::RGBA8) return packed_copy(rgba);

  VideoFrame out = VideoFrame::allocate(format, rgba.width, rgba.height);
  out.color = color;
  carry_timing(rgba, out);

  const std::size_t w = rgba.width;
  const std::size_t h = rgba.height;
  YuvWriter writer(out, color);
  std::vector<double> r(w);
  std::vector<double> g(w);
  std::vector<double> b(w);
  const std::uint8_t* src = rgba.planes[0].data.data();
  const std::size_t stride = rgba.planes[0].stride;
  for (std::size_t y = 0; y < h; ++y) {
    unpack_rgba(src + y * stride, w, r.data(), g.data(), b.data());
    writer.rgb_row(y, r.data(), g.data(), b.data());
  }
  return out;
}

Result<VideoFrame> convert_frame(const VideoFrame& frame, PixelFormat format, const ColorInfo& color) {
  CVT_TRY(check_frame(frame));
  if (!known_format(format)) return fail(Status::InvalidArgument, "unknown target pixel format");
  if (frame.format == PixelFormat::RGBA8) return from_rgba(frame, format, color);
  if (format == PixelFormat::RGBA8) return to_rgba(frame);
  if (frame.format == format && frame.color == color) return packed_copy(frame);
  if (frame.color.matrix == color.matrix) return convert_planes(frame, format, color);

  VideoFrame out = VideoFrame::allocate(format, frame.width, frame.height);
  out.color = color;
  carry_timing(frame, out);

  const std::size_t w = frame.width;
  const std::size_t h = frame.height;
  YuvReader reader(frame);
  YuvWriter writer(out, color);
  std::vector<double> r(w);
  std::vector<double> g(w);
  std::vector<double> b(w);
  for (std::size_t y = 0; y < h; ++y) {
    reader.rgb_row(y, r.data(), g.data(), b.data());
    writer.rgb_row(y, r.data(), g.data(), b.data());
  }
  return out;
}

}  // namespace cvt::codec
