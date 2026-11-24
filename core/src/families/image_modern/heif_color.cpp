// Convertible core - image_modern family: decoded HEIF pictures to RGBA
// rasters and rasters to the image IR (ITU-T H.273 code points, ranges,
// 10-bit samples, PQ/HLG high dynamic range).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "codec/colorspace.hpp"
#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/heif_internal.hpp"

namespace cvt::image_modern::heif {

namespace {

// --------------------------------------------------------------------------------- raster access

template <class T>
std::vector<T>& samples_of(Raster& raster) {
  if constexpr (sizeof(T) == 1) {
    return raster.p8;
  } else {
    return raster.p16;
  }
}

template <class T>
const std::vector<T>& samples_of(const Raster& raster) {
  if constexpr (sizeof(T) == 1) {
    return raster.p8;
  } else {
    return raster.p16;
  }
}

Raster like(const Raster& raster, std::uint32_t width, std::uint32_t height) {
  Raster out;
  out.width = width;
  out.height = height;
  out.wide = raster.wide;
  out.has_alpha = raster.has_alpha;
  out.source_bits = raster.source_bits;
  out.color = raster.color;
  out.identity_matrix = raster.identity_matrix;
  if (out.wide) {
    out.p16.resize(out.samples());
  } else {
    out.p8.resize(out.samples());
  }
  return out;
}

/// Copy pixels with a coordinate mapping: dst(x, y) = src(map(x, y)).
template <class T, class Map>
void remap(const Raster& src, Raster& dst, Map&& map) {
  const std::vector<T>& s = samples_of<T>(src);
  std::vector<T>& d = samples_of<T>(dst);
  for (std::uint32_t y = 0; y < dst.height; ++y) {
    for (std::uint32_t x = 0; x < dst.width; ++x) {
      const auto [sx, sy] = map(x, y);
      const std::size_t from = (std::size_t{sy} * src.width + sx) * 4;
      const std::size_t to = (std::size_t{y} * dst.width + x) * 4;
      for (std::size_t c = 0; c < 4; ++c) d[to + c] = s[from + c];
    }
  }
}

template <class Map>
Raster transformed(const Raster& src, std::uint32_t width, std::uint32_t height, Map&& map) {
  Raster dst = like(src, width, height);
  if (src.wide) {
    remap<std::uint16_t>(src, dst, map);
  } else {
    remap<std::uint8_t>(src, dst, map);
  }
  return dst;
}

// ------------------------------------------------------------------------------- colour maths

struct Coefficients {
  double kr = 0.299;
  double kb = 0.114;
};

/// Kr/Kb for a matrix code point (H.273 table 4); chromaticity-derived
/// matrices use the luminance coefficients of the primaries.
Coefficients coefficients(const Cicp& cicp) {
  switch (cicp.matrix) {
    case 1: return {0.2126, 0.0722};
    case 4: return {0.30, 0.11};
    case 7: return {0.212, 0.087};
    case 9:
    case 10: return {0.2627, 0.0593};
    case 12:
    case 13:
      switch (cicp.primaries) {
        case 1: return {0.2126, 0.0722};
        case 9: return {0.2627, 0.0593};
        case 11:
        case 12: return {0.2290, 0.0793};
        default: return {0.299, 0.114};
      }
    default: return {0.299, 0.114};  // 5, 6 and unspecified (as libavif and libheif do)
  }
}

std::optional<ir::ColorMatrix> standard_matrix(const Cicp& cicp) {
  switch (cicp.matrix) {
    case 1: return ir::ColorMatrix::BT709;
    case 2:
    case 5:
    case 6: return ir::ColorMatrix::BT601;
    case 9: return ir::ColorMatrix::BT2020;
    default: return std::nullopt;
  }
}

double pq_to_nits(double e) {
  constexpr double m1 = 2610.0 / 16384.0;
  constexpr double m2 = 2523.0 / 4096.0 * 128.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 / 4096.0 * 32.0;
  constexpr double c3 = 2392.0 / 4096.0 * 32.0;
  const double p = std::pow(std::clamp(e, 0.0, 1.0), 1.0 / m2);
  return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

double hlg_inverse_oetf(double e) {
  constexpr double a = 0.17883277;
  constexpr double b = 0.28466892;
  constexpr double c = 0.55991073;
  e = std::clamp(e, 0.0, 1.0);
  return e <= 0.5 ? e * e / 3.0 : (std::exp((e - c) / a) + b) / 12.0;
}

/// Reference white of HDR content in nits (ITU-R BT.2408): 1.0 in the IR.
constexpr double sdr_white_nits = 203.0;

double srgb_eotf(double e) {
  e = std::clamp(e, 0.0, 1.0);
  return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
}

using Matrix3 = std::array<double, 9>;

/// Linear RGB of the given primaries -> linear BT.709/sRGB; nullopt when the
/// primaries are (close enough to) BT.709 or unknown.
std::optional<Matrix3> to_bt709(std::uint16_t primaries) {
  switch (primaries) {
    case 9:
      return Matrix3{1.6604910, -0.5876411, -0.0728499, -0.1245505, 1.1328999, -0.0083494,
                     -0.0181508, -0.1005789, 1.1187297};
    case 11:
    case 12:
      return Matrix3{1.2249401, -0.2249404, 0.0, -0.0420569, 1.0420571, 0.0, -0.0196376, -0.0786361, 1.0982735};
    default: return std::nullopt;
  }
}

}  // namespace

// ------------------------------------------------------------------------------------- rasters

Result<Raster> make_raster(const Context& ctx, std::uint32_t width, std::uint32_t height, bool wide,
                           const std::uint16_t* fill16) {
  CVT_TRY(image::check_size(ctx, width, height));
  Raster raster;
  raster.width = width;
  raster.height = height;
  raster.wide = wide;
  const std::size_t count = raster.samples();
  if (wide) {
    raster.p16.assign(count, 0);
    for (std::size_t i = 0; i < count; i += 4) {
      for (std::size_t c = 0; c < 4; ++c) raster.p16[i + c] = fill16 != nullptr ? fill16[c] : (c == 3 ? 65535 : 0);
    }
  } else {
    raster.p8.assign(count, 0);
    for (std::size_t i = 0; i < count; i += 4) {
      for (std::size_t c = 0; c < 4; ++c) {
        raster.p8[i + c] = fill16 != nullptr ? static_cast<std::uint8_t>((fill16[c] + 128u) / 257u)
                                             : static_cast<std::uint8_t>(c == 3 ? 255 : 0);
      }
    }
  }
  return raster;
}

void promote_to_wide(Raster& raster) {
  if (raster.wide) return;
  raster.p16.resize(raster.p8.size());
  for (std::size_t i = 0; i < raster.p8.size(); ++i) raster.p16[i] = static_cast<std::uint16_t>(raster.p8[i] * 257u);
  raster.p8 = {};
  raster.wide = true;
}

Raster crop(const Raster& raster, const CropRect& rect) {
  return transformed(raster, rect.width, rect.height, [&](std::uint32_t x, std::uint32_t y) {
    return std::pair<std::uint32_t, std::uint32_t>{x + rect.left, y + rect.top};
  });
}

Raster rotate_ccw(const Raster& raster, int quarter_turns) {
  const int q = ((quarter_turns % 4) + 4) % 4;
  const std::uint32_t w = raster.width;
  const std::uint32_t h = raster.height;
  using P = std::pair<std::uint32_t, std::uint32_t>;
  switch (q) {
    case 1:  // destination is h x w; dst(x, y) = src(w - 1 - y, x)
      return transformed(raster, h, w, [&](std::uint32_t x, std::uint32_t y) { return P{w - 1 - y, x}; });
    case 2:
      return transformed(raster, w, h, [&](std::uint32_t x, std::uint32_t y) { return P{w - 1 - x, h - 1 - y}; });
    case 3:  // dst(x, y) = src(y, h - 1 - x)
      return transformed(raster, h, w, [&](std::uint32_t x, std::uint32_t y) { return P{y, h - 1 - x}; });
    default: return raster;
  }
}

Raster mirror(const Raster& raster, int axis) {
  const std::uint32_t w = raster.width;
  const std::uint32_t h = raster.height;
  using P = std::pair<std::uint32_t, std::uint32_t>;
  if (axis == 1) return transformed(raster, w, h, [&](std::uint32_t x, std::uint32_t y) { return P{w - 1 - x, y}; });
  return transformed(raster, w, h, [&](std::uint32_t x, std::uint32_t y) { return P{x, h - 1 - y}; });
}

namespace {

template <class T>
void blit_samples(Raster& canvas, const Raster& tile, std::int64_t x, std::int64_t y) {
  const std::int64_t x0 = std::max<std::int64_t>(x, 0);
  const std::int64_t y0 = std::max<std::int64_t>(y, 0);
  const std::int64_t x1 = std::min<std::int64_t>(x + tile.width, canvas.width);
  const std::int64_t y1 = std::min<std::int64_t>(y + tile.height, canvas.height);
  if (x0 >= x1 || y0 >= y1) return;
  const std::vector<T>& s = samples_of<T>(tile);
  std::vector<T>& d = samples_of<T>(canvas);
  const std::size_t run = static_cast<std::size_t>(x1 - x0) * 4;
  for (std::int64_t row = y0; row < y1; ++row) {
    const std::size_t from = (static_cast<std::size_t>(row - y) * tile.width + static_cast<std::size_t>(x0 - x)) * 4;
    const std::size_t to = (static_cast<std::size_t>(row) * canvas.width + static_cast<std::size_t>(x0)) * 4;
    std::copy_n(s.begin() + static_cast<std::ptrdiff_t>(from), run, d.begin() + static_cast<std::ptrdiff_t>(to));
  }
}

template <class T>
void composite_samples(Raster& canvas, const Raster& layer, std::int64_t x, std::int64_t y, double max) {
  const std::int64_t x0 = std::max<std::int64_t>(x, 0);
  const std::int64_t y0 = std::max<std::int64_t>(y, 0);
  const std::int64_t x1 = std::min<std::int64_t>(x + layer.width, canvas.width);
  const std::int64_t y1 = std::min<std::int64_t>(y + layer.height, canvas.height);
  const std::vector<T>& s = samples_of<T>(layer);
  std::vector<T>& d = samples_of<T>(canvas);
  for (std::int64_t row = y0; row < y1; ++row) {
    for (std::int64_t col = x0; col < x1; ++col) {
      const std::size_t from = (static_cast<std::size_t>(row - y) * layer.width + static_cast<std::size_t>(col - x)) * 4;
      const std::size_t to = (static_cast<std::size_t>(row) * canvas.width + static_cast<std::size_t>(col)) * 4;
      const double sa = s[from + 3] / max;
      const double da = d[to + 3] / max;
      const double oa = sa + da * (1.0 - sa);
      for (std::size_t c = 0; c < 3; ++c) {
        const double v = oa > 0 ? (s[from + c] * sa + d[to + c] * da * (1.0 - sa)) / oa : 0.0;
        d[to + c] = static_cast<T>(std::clamp(v + 0.5, 0.0, max));
      }
      d[to + 3] = static_cast<T>(std::clamp(oa * max + 0.5, 0.0, max));
    }
  }
}

}  // namespace

void blit(Raster& canvas, const Raster& tile, std::int64_t x, std::int64_t y) {
  if (canvas.wide) {
    blit_samples<std::uint16_t>(canvas, tile, x, y);
  } else {
    blit_samples<std::uint8_t>(canvas, tile, x, y);
  }
}

void composite(Raster& canvas, const Raster& layer, std::int64_t x, std::int64_t y) {
  if (canvas.wide) {
    composite_samples<std::uint16_t>(canvas, layer, x, y, 65535.0);
  } else {
    composite_samples<std::uint8_t>(canvas, layer, x, y, 255.0);
  }
}

// ------------------------------------------------------------------------- pictures to rasters

Result<Raster> picture_to_raster(const ir::VideoFrame& input, const PictureInfo& info, bool as_alpha,
                                 const Context& ctx) {
  if (!input.valid() || input.format == ir::PixelFormat::RGBA8) {
    return fail(Status::Decode, "the decoder returned an unusable picture");
  }
  const int bits = ir::bit_depth(input.format);
  const bool wide = bits > 8;
  const Cicp& cicp = info.cicp;
  CVT_TRY_ASSIGN(Raster raster, make_raster(ctx, input.width, input.height, wide));
  raster.source_bits = info.bit_depth;
  raster.color = cicp;
  raster.identity_matrix = cicp.matrix == 0 && !info.monochrome && !as_alpha;

  const std::uint32_t width = input.width;
  const std::uint32_t height = input.height;
  const double max_code = (1 << bits) - 1;
  const double scale = static_cast<double>(1 << (bits - 8));
  const double out_max = wide ? 65535.0 : 255.0;
  const bool full = cicp.full_range;
  const auto put = [&](std::size_t at, double v) {
    const double scaled = std::clamp(v, 0.0, 1.0) * out_max + 0.5;
    if (wide) {
      raster.p16[at] = static_cast<std::uint16_t>(scaled);
    } else {
      raster.p8[at] = static_cast<std::uint8_t>(scaled);
    }
  };
  const auto sample = [&](const ir::VideoFrame& f, std::size_t plane, std::uint32_t x, std::uint32_t y) -> double {
    const ir::Plane& p = f.planes[plane];
    const std::size_t at = p.stride * y;
    if (wide) {
      const std::size_t i = at + std::size_t{x} * 2;
      return static_cast<double>((p.data[i] | (p.data[i + 1] << 8)) & 0x3FF);
    }
    return static_cast<double>(p.data[at + x]);
  };
  const auto luma = [&](double v) { return full ? v / max_code : (v - 16.0 * scale) / (219.0 * scale); };

  if (as_alpha || info.monochrome) {
    // P010 keeps its samples MSB-aligned; convert so plane 0 holds plain values.
    const ir::VideoFrame* frame = &input;
    ir::VideoFrame plain;
    if (input.format == ir::PixelFormat::P010) {
      CVT_TRY_ASSIGN(plain, codec::convert_frame(input, ir::PixelFormat::I420P10, input.color));
      frame = &plain;
    }
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const double v = luma(sample(*frame, 0, x, y));
        const std::size_t at = (std::size_t{y} * width + x) * 4;
        put(at, v);
        put(at + 1, v);
        put(at + 2, v);
      }
    }
    return raster;
  }

  const bool subsampled = ir::chroma_shift_x(input.format) != 0 || ir::chroma_shift_y(input.format) != 0;
  const std::optional<ir::ColorMatrix> matrix = standard_matrix(cicp);
  if (!wide && matrix && (!subsampled || !info.nearest_chroma)) {
    // The shared converter (bilinear chroma at the signalled siting, exact rounding).
    ir::VideoFrame frame = input;
    frame.color.matrix = *matrix;
    frame.color.siting = info.siting;
    frame.color.range = full ? ir::ColorRange::Full : ir::ColorRange::Limited;
    CVT_TRY_ASSIGN(ir::VideoFrame rgba, codec::to_rgba(frame));
    raster.p8 = std::move(rgba.planes[0].data);
    raster.p8.resize(raster.samples());
    return raster;
  }

  // General path: chroma at full resolution (interpolated, or each sample
  // repeated over the pixels it covers), then the matrix (any Kr/Kb,
  // identity/GBR or YCgCo) per pixel.
  ir::VideoFrame converted;
  const ir::VideoFrame* frame = &input;
  ir::ColorInfo same = input.color;
  same.siting = info.siting;
  same.range = full ? ir::ColorRange::Full : ir::ColorRange::Limited;
  int shift_x = 0;
  int shift_y = 0;
  if (subsampled && !info.nearest_chroma) {
    ir::VideoFrame tagged = input;
    tagged.color = same;
    CVT_TRY_ASSIGN(converted, codec::convert_frame(tagged, wide ? ir::PixelFormat::I444P10 : ir::PixelFormat::I444, same));
    frame = &converted;
  } else if (input.format == ir::PixelFormat::NV12 || input.format == ir::PixelFormat::P010) {
    // Planar copies keep every chroma sample in place.
    ir::VideoFrame tagged = input;
    tagged.color = same;
    CVT_TRY_ASSIGN(converted, codec::convert_frame(tagged, wide ? ir::PixelFormat::I420P10 : ir::PixelFormat::I420, same));
    frame = &converted;
    shift_x = 1;
    shift_y = 1;
  } else {
    shift_x = ir::chroma_shift_x(input.format);
    shift_y = ir::chroma_shift_y(input.format);
  }
  const Coefficients k = coefficients(cicp);
  const double kg = 1.0 - k.kr - k.kb;
  const double half = static_cast<double>(1 << (bits - 1));
  const auto chroma = [&](double v) { return full ? (v - half) / max_code : (v - 128.0 * scale) / (224.0 * scale); };
  for (std::uint32_t y = 0; y < height; ++y) {
    if ((y & 255) == 0) CVT_TRY(ctx.checkpoint(0.5 * y / height));
    for (std::uint32_t x = 0; x < width; ++x) {
      const double ys = sample(*frame, 0, x, y);
      const double cbs = sample(*frame, 1, x >> shift_x, y >> shift_y);
      const double crs = sample(*frame, 2, x >> shift_x, y >> shift_y);
      double r = 0;
      double g = 0;
      double b = 0;
      if (cicp.matrix == 0) {  // identity: planes are G, B, R
        g = luma(ys);
        b = luma(cbs);
        r = luma(crs);
      } else if (cicp.matrix == 8) {  // YCgCo
        const double yy = luma(ys);
        const double cg = chroma(cbs);
        const double co = chroma(crs);
        const double t = yy - cg;
        g = yy + cg;
        b = t - co;
        r = t + co;
      } else {
        const double yy = luma(ys);
        const double cb = chroma(cbs);
        const double cr = chroma(crs);
        r = yy + 2.0 * (1.0 - k.kr) * cr;
        b = yy + 2.0 * (1.0 - k.kb) * cb;
        g = (yy - k.kr * r - k.kb * b) / kg;
      }
      const std::size_t at = (std::size_t{y} * width + x) * 4;
      put(at, r);
      put(at + 1, g);
      put(at + 2, b);
    }
  }
  return raster;
}

void attach_alpha(Raster& color, const Raster& alpha, bool premultiplied) {
  if (color.width != alpha.width || color.height != alpha.height) return;
  const std::size_t pixels = std::size_t{color.width} * color.height;
  for (std::size_t i = 0; i < pixels; ++i) {
    const std::size_t at = i * 4;
    if (color.wide) {
      const std::uint32_t a = alpha.wide ? alpha.p16[at] : alpha.p8[at] * 257u;
      color.p16[at + 3] = static_cast<std::uint16_t>(a);
      if (premultiplied) {
        for (std::size_t c = 0; c < 3; ++c) {
          color.p16[at + c] =
              a == 0 ? 0 : static_cast<std::uint16_t>(std::min<std::uint32_t>(65535u, (color.p16[at + c] * 65535u + a / 2) / a));
        }
      }
    } else {
      const std::uint32_t a = alpha.wide ? (alpha.p16[at] * 255u + 32767u) / 65535u : alpha.p8[at];
      color.p8[at + 3] = static_cast<std::uint8_t>(a);
      if (premultiplied) {
        for (std::size_t c = 0; c < 3; ++c) {
          color.p8[at + c] =
              a == 0 ? 0 : static_cast<std::uint8_t>(std::min<std::uint32_t>(255u, (color.p8[at + c] * 255u + a / 2) / a));
        }
      }
    }
  }
  color.has_alpha = true;
}

// ------------------------------------------------------------------------------ rasters to IR

Result<ir::Frame> raster_to_frame(const Raster& raster, bool have_icc, std::optional<ir::HdrPixels>* hdr) {
  const std::size_t pixels = std::size_t{raster.width} * raster.height;
  const std::uint16_t transfer = raster.color.transfer;
  const bool high_range = transfer == transfer_pq || transfer == transfer_hlg;
  const std::optional<Matrix3> gamut = have_icc ? std::nullopt : to_bt709(raster.color.primaries);
  const bool special_transfer = transfer == 8 || transfer == 4 || transfer == 5;
  const double in_max = raster.wide ? 65535.0 : 255.0;
  const auto value = [&](std::size_t at) -> double {
    return raster.wide ? raster.p16[at] / in_max : raster.p8[at] / in_max;
  };
  const auto alpha8 = [&](std::size_t at) -> std::uint8_t {
    if (!raster.has_alpha) return 255;
    return raster.wide ? static_cast<std::uint8_t>((raster.p16[at] * 255u + 32767u) / 65535u) : raster.p8[at];
  };

  ir::Frame frame;
  frame.width = raster.width;
  frame.height = raster.height;
  frame.rgba.resize(pixels * 4);

  if (!high_range && !gamut && !special_transfer) {
    // Display-referred SDR (sRGB, BT.709, unspecified, ICC-described): keep the
    // encoded values; linear HDR pixels only when there is extra precision.
    for (std::size_t i = 0; i < pixels; ++i) {
      const std::size_t at = i * 4;
      for (std::size_t c = 0; c < 3; ++c) {
        frame.rgba[at + c] = raster.wide ? static_cast<std::uint8_t>((raster.p16[at + c] * 255u + 32767u) / 65535u)
                                         : raster.p8[at + c];
      }
      frame.rgba[at + 3] = alpha8(at + 3);
    }
    if (raster.wide && hdr != nullptr) {
      std::vector<std::uint16_t> rgba16 = raster.p16;
      if (!raster.has_alpha) {
        for (std::size_t i = 3; i < rgba16.size(); i += 4) rgba16[i] = 65535;
      }
      *hdr = image::linear_from_srgb16(rgba16, raster.width, raster.height);
    }
    return frame;
  }

  // Scene/display-linear path: decode the transfer function, convert the
  // primaries to BT.709, keep the linear values as HDR pixels.
  ir::HdrPixels linear;
  linear.width = raster.width;
  linear.height = raster.height;
  linear.rgba.resize(pixels * 4);
  for (std::size_t i = 0; i < pixels; ++i) {
    const std::size_t at = i * 4;
    std::array<double, 3> rgb{};
    for (std::size_t c = 0; c < 3; ++c) {
      const double e = value(at + c);
      switch (transfer) {
        case transfer_pq: rgb[c] = pq_to_nits(e) / sdr_white_nits; break;
        case transfer_hlg: rgb[c] = hlg_inverse_oetf(e); break;
        case 8: rgb[c] = e; break;
        case 4: rgb[c] = std::pow(std::clamp(e, 0.0, 1.0), 2.2); break;
        case 5: rgb[c] = std::pow(std::clamp(e, 0.0, 1.0), 2.8); break;
        default: rgb[c] = srgb_eotf(e); break;
      }
    }
    if (transfer == transfer_hlg) {
      // BT.2100 OOTF for a 1000-nit display (system gamma 1.2).
      const double ys = 0.2627 * rgb[0] + 0.6780 * rgb[1] + 0.0593 * rgb[2];
      const double gain = 1000.0 * std::pow(std::max(ys, 0.0), 0.2) / sdr_white_nits;
      for (double& v : rgb) v *= gain;
    }
    if (gamut) {
      const Matrix3& m = *gamut;
      const std::array<double, 3> in = rgb;
      for (std::size_t r = 0; r < 3; ++r) rgb[r] = m[r * 3] * in[0] + m[r * 3 + 1] * in[1] + m[r * 3 + 2] * in[2];
    }
    for (std::size_t c = 0; c < 3; ++c) linear.rgba[at + c] = static_cast<float>(std::max(rgb[c], 0.0));
    linear.rgba[at + 3] = raster.has_alpha ? static_cast<float>(value(at + 3)) : 1.0f;
  }
  if (high_range) {
    frame = image::tone_map(linear);
  } else {
    for (std::size_t i = 0; i < pixels; ++i) {
      const std::size_t at = i * 4;
      for (std::size_t c = 0; c < 3; ++c) frame.rgba[at + c] = image::linear_to_srgb8(linear.rgba[at + c]);
      frame.rgba[at + 3] = alpha8(at + 3);
    }
  }
  if (hdr != nullptr && (raster.wide || high_range)) *hdr = std::move(linear);
  return frame;
}

}  // namespace cvt::image_modern::heif
