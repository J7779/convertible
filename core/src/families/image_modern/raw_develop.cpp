// Convertible core - image_modern family: RAW development pipeline.
//
// linearisation table -> black level subtraction (repeat pattern plus
// per-row / per-column deltas) -> scaling to the white level -> white
// balance (as shot, grey world or none) with highlight clipping ->
// demosaic -> camera RGB to linear sRGB (DNG colour model: colour matrices
// or forward matrices interpolated between the two calibration illuminants
// by correlated colour temperature) -> baseline exposure -> default crop ->
// orientation -> tone mapping of the 8-bit rendition.
// Written from the DNG specification (chapter 6, "Mapping Raw Values to
// Linear Reference Values" and "Mapping Camera Color Space to CIE XYZ").
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "families/image/image_ops.hpp"
#include "families/image_modern/raw_model.hpp"

namespace cvt::image_modern::raw {

namespace {

// ----------------------------------------------------------------------------- 3x3 matrices

Matrix3 multiply(const Matrix3& a, const Matrix3& b) {
  Matrix3 r{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += a[static_cast<std::size_t>(i * 3 + k)] * b[static_cast<std::size_t>(k * 3 + j)];
      r[static_cast<std::size_t>(i * 3 + j)] = s;
    }
  }
  return r;
}

std::array<double, 3> transform_vector(const Matrix3& m, const std::array<double, 3>& v) {
  return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
          m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

std::optional<Matrix3> invert(const Matrix3& m) {
  const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (!std::isfinite(det) || std::fabs(det) < 1e-12) return std::nullopt;
  const double s = 1.0 / det;
  return Matrix3{(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s,
                 (f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s,
                 (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s};
}

Matrix3 identity() { return {1, 0, 0, 0, 1, 0, 0, 0, 1}; }

Matrix3 lerp(const Matrix3& a, const Matrix3& b, double w) {
  Matrix3 r{};
  for (std::size_t i = 0; i < 9; ++i) r[i] = w * a[i] + (1.0 - w) * b[i];
  return r;
}

// IEC 61966-2-1 sRGB primaries, D65.
constexpr Matrix3 xyz_from_srgb = {0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522,
                                   0.0721750, 0.0193339, 0.1191920, 0.9503041};
constexpr Matrix3 srgb_from_xyz = {3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108,
                                   0.0415560, 0.0556434,  -0.2040259, 1.0572252};
// Bradford chromatic adaptation D50 -> D65.
constexpr Matrix3 d65_from_d50 = {0.9555766, -0.0230393, 0.0631636, -0.0282895, 1.0099416,
                                  0.0210077, 0.0122982,  -0.0204830, 1.3299098};

// Correlated colour temperature of EXIF LightSource values (0 = unknown).
double illuminant_temperature(int code) {
  switch (code) {
    case 1: return 5500;   // daylight
    case 2: return 4150;   // fluorescent
    case 3: return 2850;   // tungsten
    case 4: return 5500;   // flash
    case 9: return 5500;   // fine weather
    case 10: return 6500;  // cloudy
    case 11: return 7500;  // shade
    case 12: return 6430;  // daylight fluorescent
    case 13: return 5000;  // day white fluorescent
    case 14: return 4150;  // cool white fluorescent
    case 15: return 3450;  // white fluorescent
    case 16: return 2940;  // warm white fluorescent
    case 17: return 2856;  // standard light A
    case 18: return 4874;  // standard light B
    case 19: return 6774;  // standard light C
    case 20: return 5503;  // D55
    case 21: return 6504;  // D65
    case 22: return 7504;  // D75
    case 23: return 5003;  // D50
    case 24: return 3200;  // ISO studio tungsten
    default: return 0;
  }
}

// McCamy's approximation of the correlated colour temperature of xy.
double cct_from_xy(double x, double y) {
  const double n = (x - 0.3320) / (0.1858 - y);
  const double t = 449.0 * n * n * n + 3525.0 * n * n + 6823.3 * n + 5520.33;
  return std::clamp(t, 1500.0, 50000.0);
}

std::array<double, 3> xyz_from_xy(double x, double y) {
  if (y <= 1e-6) return {0.9642, 1.0, 0.8249};
  return {x / y, 1.0, (1.0 - x - y) / y};
}

// The camera-to-sRGB matrix for white-balanced camera data, plus the camera
// neutral used for white balance.
struct ColorTransform {
  Matrix3 srgb_from_camera = identity();
  std::array<double, 3> neutral = {1, 1, 1};
  bool have_neutral = false;
  bool have_matrix = false;
};

ColorTransform color_transform(const ColorInfo& c) {
  ColorTransform out;
  const auto ab = c.analog_balance.value_or(std::array<double, 3>{1, 1, 1});
  const Matrix3 ab_m = {ab[0], 0, 0, 0, ab[1], 0, 0, 0, ab[2]};
  const auto cam_from_xyz_at = [&](double w) -> std::optional<Matrix3> {
    std::optional<Matrix3> cm;
    if (c.color_matrix1 && c.color_matrix2) cm = lerp(*c.color_matrix1, *c.color_matrix2, w);
    else if (c.color_matrix1) cm = c.color_matrix1;
    else if (c.color_matrix2) cm = c.color_matrix2;
    if (!cm) return std::nullopt;
    Matrix3 cc = identity();
    if (c.calibration1 && c.calibration2) cc = lerp(*c.calibration1, *c.calibration2, w);
    else if (c.calibration1 && !c.color_matrix2) cc = *c.calibration1;
    return multiply(multiply(ab_m, cc), *cm);
  };

  if (!c.neutral.empty() && c.neutral.size() >= 3) {
    out.neutral = {c.neutral[0], c.neutral[1], c.neutral[2]};
    out.have_neutral = true;
  } else if (c.wb_gains.size() >= 3 && c.wb_gains[0] > 0 && c.wb_gains[1] > 0 && c.wb_gains[2] > 0) {
    out.neutral = {1.0 / c.wb_gains[0], 1.0 / c.wb_gains[1], 1.0 / c.wb_gains[2]};
    out.have_neutral = true;
  }

  // Interpolation weight between the two calibration illuminants.
  double weight = 1.0;
  const double t1 = illuminant_temperature(c.illuminant1);
  const double t2 = illuminant_temperature(c.illuminant2);
  const bool dual = c.color_matrix1 && c.color_matrix2 && t1 > 0 && t2 > 0 && t1 != t2;
  if (!out.have_neutral && c.white_xy) {
    // Neutral from the as-shot white point (needs a matrix).
    const auto xyz = xyz_from_xy((*c.white_xy)[0], (*c.white_xy)[1]);
    if (dual) {
      const double t = cct_from_xy((*c.white_xy)[0], (*c.white_xy)[1]);
      weight = std::clamp((1.0 / t - 1.0 / t2) / (1.0 / t1 - 1.0 / t2), 0.0, 1.0);
    }
    if (const auto m = cam_from_xyz_at(weight)) {
      const auto n = transform_vector(*m, xyz);
      if (n[0] > 0 && n[1] > 0 && n[2] > 0) {
        const double mx = std::max({n[0], n[1], n[2]});
        out.neutral = {n[0] / mx, n[1] / mx, n[2] / mx};
        out.have_neutral = true;
      }
    }
  } else if (dual && out.have_neutral) {
    // Iterate: neutral -> xy (with the current matrix) -> temperature -> weight.
    weight = 0.5;
    for (int iteration = 0; iteration < 12; ++iteration) {
      const auto m = cam_from_xyz_at(weight);
      if (!m) break;
      const auto inv = invert(*m);
      if (!inv) break;
      const auto xyz = transform_vector(*inv, out.neutral);
      const double sum = xyz[0] + xyz[1] + xyz[2];
      if (!(sum > 1e-9)) break;
      const double t = cct_from_xy(xyz[0] / sum, xyz[1] / sum);
      const double next = std::clamp((1.0 / t - 1.0 / t2) / (1.0 / t1 - 1.0 / t2), 0.0, 1.0);
      if (std::fabs(next - weight) < 1e-6) {
        weight = next;
        break;
      }
      weight = next;
    }
  }

  // Forward matrices map white-balanced camera data to XYZ (D50).
  std::optional<Matrix3> fm;
  if (c.forward_matrix1 && c.forward_matrix2 && dual) fm = lerp(*c.forward_matrix1, *c.forward_matrix2, weight);
  else if (c.forward_matrix1) fm = c.forward_matrix1;
  else if (c.forward_matrix2) fm = c.forward_matrix2;
  if (fm) {
    out.srgb_from_camera = multiply(multiply(srgb_from_xyz, d65_from_d50), *fm);
    out.have_matrix = true;
    return out;
  }
  if (const auto m = cam_from_xyz_at(dual ? weight : 1.0)) {
    // Camera from sRGB, rows normalised so that white maps to the
    // (white-balanced) camera neutral (1, 1, 1).
    Matrix3 cam_from_srgb = multiply(*m, xyz_from_srgb);
    bool ok = true;
    for (int r = 0; r < 3; ++r) {
      const auto i = static_cast<std::size_t>(r * 3);
      const double sum = cam_from_srgb[i] + cam_from_srgb[i + 1] + cam_from_srgb[i + 2];
      if (!(std::fabs(sum) > 1e-9)) {
        ok = false;
        break;
      }
      for (std::size_t k = 0; k < 3; ++k) cam_from_srgb[i + k] /= sum;
    }
    if (ok) {
      if (const auto inv = invert(cam_from_srgb)) {
        out.srgb_from_camera = *inv;
        out.have_matrix = true;
      }
    }
  }
  return out;
}

// Estimate the saturation level of a sensor that does not record it: clipped
// photosites all read exactly the same maximum value (real, unclipped data
// is noisy), so a pile-up of identical values at the top of the histogram in
// the upper half of the range marks saturation. Otherwise the full range.
double estimate_white(const Sensor& s, double full) {
  if (s.u16.empty()) return full;
  std::uint16_t top = 0;
  for (const std::uint16_t v : s.u16) top = std::max(top, v);
  if (top == 0 || top < full * 0.5) return full;
  std::size_t at_top = 0;
  for (const std::uint16_t v : s.u16) at_top += v == top ? 1u : 0u;
  if (at_top >= 16 && at_top * 5000 >= s.u16.size()) return static_cast<double>(top);
  return full;
}

}  // namespace

Result<ir::Image> develop(Sensor sensor, const RawInfo& info, const DevelopOptions& options, const Context& ctx) {
  const int spp = sensor.spp;
  const bool floating = sensor.floating();
  const int bits = sensor.bits;
  const bool cfa = info.cfa.has_value();
  if (cfa && (spp != 1 || !info.cfa->valid())) return fail(Status::Decode, "invalid colour filter array description");
  if (!cfa && spp != 1 && spp != 3) return fail(Status::Decode, "RAW data with {} colour planes is not supported", spp);
  if (cfa) {
    for (std::uint32_t i = 0; i < info.cfa->width * info.cfa->height; ++i) {
      if (info.cfa->colors[i] > 2) return fail(Status::Decode, "colour filter arrays other than red/green/blue are not supported");
    }
  }

  // ---- active area
  std::uint32_t top = info.active[0], left = info.active[1], bottom = info.active[2], right = info.active[3];
  if (bottom == 0 && right == 0) {
    bottom = sensor.height;
    right = sensor.width;
  }
  bottom = std::min(bottom, sensor.height);
  right = std::min(right, sensor.width);
  if (top >= bottom || left >= right) {
    top = left = 0;
    bottom = sensor.height;
    right = sensor.width;
  }
  const std::uint32_t width = right - left;
  const std::uint32_t height = bottom - top;
  CVT_TRY(ctx.limits.check_pixels(width, height));
  const std::uint64_t pixels = std::uint64_t{width} * height;
  CVT_TRY(ctx.limits.check_decompressed(pixels * 4 * sizeof(float)));

  // ---- levels
  const Levels& lv = info.levels;
  const double full = floating ? 1.0 : static_cast<double>((1u << std::min(sensor.bits, 16)) - 1u);
  std::vector<double> white(static_cast<std::size_t>(spp), full);
  if (lv.linearization.empty()) {
    for (int s = 0; s < spp; ++s) {
      if (static_cast<std::size_t>(s) < lv.white.size()) white[static_cast<std::size_t>(s)] = lv.white[static_cast<std::size_t>(s)];
      else if (!lv.white.empty()) white[static_cast<std::size_t>(s)] = lv.white.front();
    }
  } else {
    const double table_max = static_cast<double>(*std::max_element(lv.linearization.begin(), lv.linearization.end()));
    for (int s = 0; s < spp; ++s) {
      double w = lv.white.empty() ? table_max : lv.white[std::min<std::size_t>(static_cast<std::size_t>(s), lv.white.size() - 1)];
      white[static_cast<std::size_t>(s)] = w > 0 ? w : table_max;
    }
  }
  if (lv.estimate_white && lv.white.empty()) {
    const double w = estimate_white(sensor, full);
    std::fill(white.begin(), white.end(), w);
  }
  const std::uint32_t br = std::max<std::uint32_t>(1, lv.black_rows);
  const std::uint32_t bc = std::max<std::uint32_t>(1, lv.black_cols);
  const bool have_black = lv.black.size() >= std::size_t{br} * bc * static_cast<std::size_t>(spp);

  // Normalised plane(s): CFA -> one float per pixel; linear -> 3 per pixel
  // directly into the output buffer.
  std::vector<float> plane(cfa || spp == 1 ? static_cast<std::size_t>(pixels) : 0);
  std::vector<float> rgba(static_cast<std::size_t>(pixels) * 4, 1.0f);
  const std::size_t lin_size = lv.linearization.size();
  for (std::uint32_t y = 0; y < height; ++y) {
    if ((y & 255) == 0) CVT_TRY(ctx.checkpoint(0.5 + 0.1 * y / height, "Developing RAW"));
    const double dv = y < lv.black_delta_v.size() ? lv.black_delta_v[y] : 0.0;
    for (std::uint32_t x = 0; x < width; ++x) {
      const double dh = x < lv.black_delta_h.size() ? lv.black_delta_h[x] : 0.0;
      const std::size_t src = (std::size_t{y + top} * sensor.width + (x + left)) * static_cast<std::size_t>(spp);
      for (int s = 0; s < spp; ++s) {
        double v = 0;
        if (sensor.floating()) {
          v = sensor.f32[src + static_cast<std::size_t>(s)];
        } else {
          const std::uint16_t raw = sensor.u16[src + static_cast<std::size_t>(s)];
          v = lin_size == 0 ? raw : lv.linearization[std::min<std::size_t>(raw, lin_size - 1)];
        }
        double black = dv + dh;
        if (have_black) {
          black += lv.black[((y % br) * bc + (x % bc)) * static_cast<std::size_t>(spp) + static_cast<std::size_t>(s)];
        }
        const double range = white[static_cast<std::size_t>(s)] - black;
        const float n = range > 1e-9 ? static_cast<float>((v - black) / range) : 0.0f;
        if (cfa || spp == 1) plane[std::size_t{y} * width + x] = n;
        else rgba[(std::size_t{y} * width + x) * 4 + static_cast<std::size_t>(s)] = n;
      }
    }
  }
  // DNG GainMap opcodes (lens shading) on the normalised data.
  for (const RawInfo::GainMap& gm : info.gain_maps) {
    const bool interleaved = !(cfa || spp == 1);
    const std::uint32_t bottom = std::min(gm.bottom, height), right = std::min(gm.right, width);
    const std::uint32_t row_pitch = std::max<std::uint32_t>(1, gm.row_pitch), col_pitch = std::max<std::uint32_t>(1, gm.col_pitch);
    const auto lookup = [&](double v, double h, std::uint32_t mp) {
      v = std::clamp(v, 0.0, static_cast<double>(gm.points_v - 1));
      h = std::clamp(h, 0.0, static_cast<double>(gm.points_h - 1));
      const auto v0 = static_cast<std::uint32_t>(v), h0 = static_cast<std::uint32_t>(h);
      const std::uint32_t v1 = std::min(v0 + 1, gm.points_v - 1), h1 = std::min(h0 + 1, gm.points_h - 1);
      const double fv = v - v0, fh = h - h0;
      const auto g = [&](std::uint32_t a, std::uint32_t b) {
        return static_cast<double>(gm.gains[(std::size_t{a} * gm.points_h + b) * gm.map_planes + mp]);
      };
      return (1 - fv) * ((1 - fh) * g(v0, h0) + fh * g(v0, h1)) + fv * ((1 - fh) * g(v1, h0) + fh * g(v1, h1));
    };
    for (std::uint32_t y = gm.top; y < bottom; y += row_pitch) {
      const double v = gm.spacing_v > 0 ? ((y + 0.5) / height - gm.origin_v) / gm.spacing_v : 0.0;
      for (std::uint32_t x = gm.left; x < right; x += col_pitch) {
        const double h = gm.spacing_h > 0 ? ((x + 0.5) / width - gm.origin_h) / gm.spacing_h : 0.0;
        for (std::uint32_t p = gm.plane; p < gm.plane + gm.planes; ++p) {
          const std::uint32_t mp = std::min(p - gm.plane, gm.map_planes - 1);
          const auto gain = static_cast<float>(lookup(v, h, mp));
          if (!interleaved) {
            if (p == 0) plane[std::size_t{y} * width + x] *= gain;
          } else if (p < 3) {
            rgba[(std::size_t{y} * width + x) * 4 + p] *= gain;
          }
        }
      }
    }
  }
  sensor.u16 = {};
  sensor.f32 = {};

  // ---- white balance
  const ColorTransform transform = color_transform(info.color);
  std::array<double, 3> gains = {1, 1, 1};
  DevelopOptions::WhiteBalance wb = options.white_balance;
  if (wb == DevelopOptions::WhiteBalance::Camera && !transform.have_neutral && (cfa || spp == 3)) {
    ctx.warn("The RAW file records no as-shot white balance; an automatic (grey world) balance was used.");
    wb = DevelopOptions::WhiteBalance::Auto;
  }
  if (cfa || spp == 3) {
    if (wb == DevelopOptions::WhiteBalance::Camera) {
      for (int c = 0; c < 3; ++c) {
        const double n = transform.neutral[static_cast<std::size_t>(c)];
        gains[static_cast<std::size_t>(c)] = n > 1e-6 ? 1.0 / n : 1.0;
      }
    } else if (wb == DevelopOptions::WhiteBalance::Auto) {
      std::array<double, 3> sum{};
      std::array<std::uint64_t, 3> count{};
      for (std::uint32_t y = 0; y < height; y += 1) {
        for (std::uint32_t x = 0; x < width; ++x) {
          for (int c = 0; c < 3; ++c) {
            float v = 0;
            int color = c;
            if (cfa) {
              if (c > 0) break;
              color = info.cfa->at(x, y);
              v = plane[std::size_t{y} * width + x];
            } else {
              v = rgba[(std::size_t{y} * width + x) * 4 + static_cast<std::size_t>(c)];
            }
            if (v > 0.0f && v < 0.95f) {
              sum[static_cast<std::size_t>(color)] += v;
              ++count[static_cast<std::size_t>(color)];
            }
          }
        }
      }
      std::array<double, 3> mean{};
      for (std::size_t c = 0; c < 3; ++c) mean[c] = count[c] > 0 ? sum[c] / static_cast<double>(count[c]) : 0.0;
      if (mean[0] > 0 && mean[1] > 0 && mean[2] > 0) gains = {mean[1] / mean[0], 1.0, mean[1] / mean[2]};
    }
    const double lowest = std::min({gains[0], gains[1], gains[2]});
    for (double& g : gains) g /= lowest;
  }
  // Apply gains and clip highlights so that saturated areas stay neutral.
  if (cfa) {
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        float& v = plane[std::size_t{y} * width + x];
        v = std::min(static_cast<float>(v * gains[static_cast<std::size_t>(info.cfa->at(x + left, y + top))]), 1.0f);
      }
    }
  } else if (spp == 3) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(pixels); ++i) {
      for (std::size_t c = 0; c < 3; ++c) rgba[i * 4 + c] = std::min(static_cast<float>(rgba[i * 4 + c] * gains[c]), 1.0f);
    }
  }
  CVT_TRY(ctx.checkpoint(0.65, "Demosaicing"));

  // ---- demosaic / expand
  if (cfa) {
    demosaic(plane, width, height, info.cfa->shifted(left, top), options.demosaic, rgba);
  } else if (spp == 1) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(pixels); ++i) {
      rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = plane[i];
    }
  }
  plane = {};
  CVT_TRY(ctx.checkpoint(0.85, "Converting colour"));

  // ---- colour, exposure
  if (spp != 1 || cfa) {
    if (!transform.have_matrix) {
      ctx.warn("The RAW file has no colour profile for its camera; colours are approximate (camera RGB shown as sRGB).");
    }
  }
  const double exposure = std::exp2(info.color.baseline_exposure + options.exposure);
  Matrix3 m = (spp == 1 && !cfa) ? identity() : transform.srgb_from_camera;
  for (double& v : m) v *= exposure;
  for (std::size_t i = 0; i < static_cast<std::size_t>(pixels); ++i) {
    float* p = &rgba[i * 4];
    const double r = std::max(0.0f, p[0]), g = std::max(0.0f, p[1]), b = std::max(0.0f, p[2]);
    p[0] = std::max(0.0f, static_cast<float>(m[0] * r + m[1] * g + m[2] * b));
    p[1] = std::max(0.0f, static_cast<float>(m[3] * r + m[4] * g + m[5] * b));
    p[2] = std::max(0.0f, static_cast<float>(m[6] * r + m[7] * g + m[8] * b));
    p[3] = 1.0f;
  }

  // ---- default crop (in place: rows move towards the start)
  std::uint32_t cx = 0, cy = 0, cw = width, ch = height;
  if (info.crop_w > 0 && info.crop_h > 0) {
    const auto ox = static_cast<std::uint32_t>(std::clamp(std::floor(info.crop_x), 0.0, static_cast<double>(width - 1)));
    const auto oy = static_cast<std::uint32_t>(std::clamp(std::floor(info.crop_y), 0.0, static_cast<double>(height - 1)));
    const auto w = static_cast<std::uint32_t>(std::clamp(std::round(info.crop_w), 1.0, static_cast<double>(width - ox)));
    const auto h = static_cast<std::uint32_t>(std::clamp(std::round(info.crop_h), 1.0, static_cast<double>(height - oy)));
    cx = ox;
    cy = oy;
    cw = w;
    ch = h;
  }
  if (cw != width || ch != height) {
    for (std::uint32_t y = 0; y < ch; ++y) {
      std::memmove(&rgba[std::size_t{y} * cw * 4], &rgba[(std::size_t{y + cy} * width + cx) * 4], std::size_t{cw} * 4 * sizeof(float));
    }
    rgba.resize(std::size_t{cw} * ch * 4);
    rgba.shrink_to_fit();
  }

  ir::Image image;
  ir::HdrPixels hdr;
  hdr.width = cw;
  hdr.height = ch;
  hdr.rgba = std::move(rgba);
  image::ToneMapOptions tone;
  tone.op = options.tone == DevelopOptions::Tone::Filmic   ? image::ToneMapOperator::Filmic
            : options.tone == DevelopOptions::Tone::Reinhard ? image::ToneMapOperator::Reinhard
                                                             : image::ToneMapOperator::Clamp;
  image.frames.push_back(image::tone_map(hdr, tone));
  image.hdr = std::move(hdr);
  image.source_bit_depth = static_cast<std::uint8_t>(floating ? 32 : std::clamp(bits, 1, 16));
  image.source_grayscale = spp == 1 && !cfa;
  if (!info.make.empty()) image.metadata["Make"] = info.make;
  if (!info.model.empty()) image.metadata["Model"] = info.model;
  if (info.orientation >= 2 && info.orientation <= 8) image::apply_orientation(image, info.orientation);
  CVT_TRY(ctx.checkpoint(1.0, "Developing RAW"));
  return image;
}

}  // namespace cvt::image_modern::raw
