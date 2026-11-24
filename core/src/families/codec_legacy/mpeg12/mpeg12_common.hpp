// Convertible core - codec_legacy family: MPEG-1/MPEG-2 helpers shared by
// the decoder and the encoder (motion compensation must match bit for bit).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace cvt::legacy::mpeg12 {

/// A view of one plane of a reference: the whole frame or one field of it.
struct RefPlane {
  const std::uint8_t* data = nullptr;
  int stride = 0;
  int width = 0;
  int height = 0;
};

/// Half-sample motion compensated prediction of a bw x bh block (bw, bh <= 16)
/// whose top left is at half-sample position (x2, y2) of `ref`; samples
/// outside the reference repeat the edge. `average` blends with `dst`.
inline void predict(std::uint8_t* dst, int dst_stride, const RefPlane& ref, int x2, int y2, int bw, int bh,
                    bool average) {
  const int x0 = x2 >> 1;
  const int y0 = y2 >> 1;
  const int hx = x2 & 1;
  const int hy = y2 & 1;
  std::uint8_t tmp[17 * 17];
  const std::uint8_t* src;
  int src_stride;
  if (x0 >= 0 && y0 >= 0 && x0 + bw + hx <= ref.width && y0 + bh + hy <= ref.height) {
    src = ref.data + static_cast<std::ptrdiff_t>(y0) * ref.stride + x0;
    src_stride = ref.stride;
  } else {
    for (int y = 0; y <= bh; ++y) {
      const int sy = std::clamp(y0 + y, 0, ref.height - 1);
      for (int x = 0; x <= bw; ++x) {
        const int sx = std::clamp(x0 + x, 0, ref.width - 1);
        tmp[y * 17 + x] = ref.data[static_cast<std::ptrdiff_t>(sy) * ref.stride + sx];
      }
    }
    src = tmp;
    src_stride = 17;
  }
  for (int y = 0; y < bh; ++y) {
    const std::uint8_t* s0 = src + static_cast<std::ptrdiff_t>(y) * src_stride;
    const std::uint8_t* s1 = s0 + src_stride;
    std::uint8_t* d = dst + static_cast<std::ptrdiff_t>(y) * dst_stride;
    if (hx == 0 && hy == 0) {
      for (int x = 0; x < bw; ++x) d[x] = static_cast<std::uint8_t>(average ? (d[x] + s0[x] + 1) >> 1 : s0[x]);
      continue;
    }
    for (int x = 0; x < bw; ++x) {
      int v;
      if (hy == 0) v = (s0[x] + s0[x + 1] + 1) >> 1;
      else if (hx == 0) v = (s0[x] + s1[x] + 1) >> 1;
      else v = (s0[x] + s0[x + 1] + s1[x] + s1[x + 1] + 2) >> 2;
      d[x] = static_cast<std::uint8_t>(average ? (d[x] + v + 1) >> 1 : v);
    }
  }
}

/// Integer division rounding half away from zero (the "//" of the standard).
[[nodiscard]] constexpr int div2_round(int v) noexcept { return (v + (v > 0 ? 1 : (v < 0 ? -1 : 0))) / 2; }

/// Inverse quantisation (ISO 13818-2 7.4.2 / ISO 11172-2 2.4.4), saturated.
/// `qscale` is quantiser_scale (MPEG-2) or quantizer_scale (MPEG-1).
[[nodiscard]] constexpr int dequant_intra(bool mpeg2, int level, int w, int qscale) noexcept {
  int v;
  if (mpeg2) {
    v = (level * w * qscale) / 16;
  } else {
    v = (level * w * qscale) / 8;
    if ((v & 1) == 0 && v != 0) v -= v > 0 ? 1 : -1;
  }
  return std::clamp(v, -2048, 2047);
}
[[nodiscard]] constexpr int dequant_inter(bool mpeg2, int level, int w, int qscale) noexcept {
  const int sign = level > 0 ? 1 : -1;
  int v;
  if (mpeg2) {
    v = ((2 * level + sign) * w * qscale) / 32;
  } else {
    v = ((2 * level + sign) * w * qscale) / 16;
    if ((v & 1) == 0 && v != 0) v -= v > 0 ? 1 : -1;
  }
  return std::clamp(v, -2048, 2047);
}

}  // namespace cvt::legacy::mpeg12
