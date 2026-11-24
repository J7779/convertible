// Convertible core - codec_legacy family: MPEG-4 Part 2 / H.263 helpers
// shared by the decoder and the encoder (prediction must match bit for bit).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace cvt::legacy::mpeg4 {

/// A macroblock-aligned 4:2:0 picture.
struct Picture {
  std::array<std::vector<std::uint8_t>, 3> plane;
  std::array<int, 3> stride{};
  std::array<int, 3> rows{};

  void allocate(int w, int h) {
    stride = {w, w / 2, w / 2};
    rows = {h, h / 2, h / 2};
    for (std::size_t p = 0; p < 3; ++p) {
      plane[p].assign(static_cast<std::size_t>(stride[p]) * static_cast<std::size_t>(rows[p]), p == 0 ? 16 : 128);
    }
  }
  [[nodiscard]] std::uint8_t* at(int p, int x, int y) {
    const auto i = static_cast<std::size_t>(p);
    return plane[i].data() + static_cast<std::ptrdiff_t>(y) * stride[i] + x;
  }
  [[nodiscard]] const std::uint8_t* at(int p, int x, int y) const {
    const auto i = static_cast<std::size_t>(p);
    return plane[i].data() + static_cast<std::ptrdiff_t>(y) * stride[i] + x;
  }
};

/// One plane (or one field of it) of a reference picture.
struct PlaneView {
  const std::uint8_t* data = nullptr;
  int stride = 0;
  int width = 0;
  int height = 0;
};

[[nodiscard]] inline PlaneView view(const Picture& pic, int p, int field = -1) {
  const auto i = static_cast<std::size_t>(p);
  PlaneView v{pic.plane[i].data(), pic.stride[i], pic.stride[i], pic.rows[i]};
  if (field >= 0) {
    v.data += field * pic.stride[i];
    v.stride *= 2;
    v.height /= 2;
  }
  return v;
}

/// Copies the w x h area at (x, y) of `ref` into `out` (stride `w`), repeating
/// edge samples outside the plane (unrestricted motion vectors).
inline void fetch(const PlaneView& ref, int x, int y, int w, int h, std::uint8_t* out) {
  if (x >= 0 && y >= 0 && x + w <= ref.width && y + h <= ref.height) {
    for (int r = 0; r < h; ++r) {
      const std::uint8_t* s = ref.data + static_cast<std::ptrdiff_t>(y + r) * ref.stride + x;
      std::copy(s, s + w, out + r * w);
    }
    return;
  }
  for (int r = 0; r < h; ++r) {
    const int sy = std::clamp(y + r, 0, ref.height - 1);
    for (int c = 0; c < w; ++c) {
      const int sx = std::clamp(x + c, 0, ref.width - 1);
      out[r * w + c] = ref.data[static_cast<std::ptrdiff_t>(sy) * ref.stride + sx];
    }
  }
}

/// Half-sample prediction (H.263 / MPEG-4 rounding control `r`) of a w x h
/// block at half-sample position (x2, y2). `average` blends with `dst`
/// (B pictures: rounding up).
inline void mc_half(std::uint8_t* dst, int dst_stride, const PlaneView& ref, int x2, int y2, int w, int h, int r,
                    bool average) {
  std::uint8_t src[17 * 17];
  const int hx = x2 & 1;
  const int hy = y2 & 1;
  fetch(ref, x2 >> 1, y2 >> 1, w + 1, h + 1, src);
  const int s = w + 1;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::uint8_t* p = src + y * s + x;
      int v;
      if (hx == 0 && hy == 0) v = p[0];
      else if (hy == 0) v = (p[0] + p[1] + 1 - r) >> 1;
      else if (hx == 0) v = (p[0] + p[s] + 1 - r) >> 1;
      else v = (p[0] + p[1] + p[s] + p[s + 1] + 2 - r) >> 2;
      std::uint8_t& d = dst[y * dst_stride + x];
      d = static_cast<std::uint8_t>(average ? (d + v + 1) >> 1 : v);
    }
  }
}

namespace detail {

[[nodiscard]] inline std::uint8_t clip8(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }

/// The MPEG-4 8-tap half-sample filter over n+1 samples `p[0..n]` (stride
/// `step`), mirrored at both ends of the block, for output position `i` (0..n-1).
[[nodiscard]] inline int qpel_tap(const std::uint8_t* p, int step, int n, int i, int r) {
  const auto at = [&](int k) {
    if (k < 0) k = -1 - k;          // p[-1] = p[0], p[-2] = p[1], ...
    if (k > n) k = 2 * n + 1 - k;   // p[n+1] = p[n], p[n+2] = p[n-1], ...
    return static_cast<int>(p[k * step]);
  };
  // (160, -48, 24, -8) / 256, evaluated as (20, -6, 3, -1) / 32 with the
  // rounding control applied at that precision (as every deployed decoder does).
  const int v = 20 * (at(i) + at(i + 1)) - 6 * (at(i - 1) + at(i + 2)) + 3 * (at(i - 2) + at(i + 3)) -
                (at(i - 3) + at(i + 4));
  return clip8((v + 16 - r) >> 5);
}

}  // namespace detail

/// Quarter-sample luma prediction (ISO/IEC 14496-2 7.6.2.2) of an n x n
/// block (n = 8 or 16) at quarter-sample position (x4, y4): horizontal
/// stage first (full / quarter / half / three-quarter), then the vertical
/// stage on its output; the 8-tap filters mirror at the block edges.
inline void mc_qpel(std::uint8_t* dst, int dst_stride, const PlaneView& ref, int x4, int y4, int n, int r, bool average) {
  std::uint8_t src[17 * 17];
  std::uint8_t hor[17 * 17];
  std::uint8_t out[16 * 16];
  const int fx = x4 & 3;
  const int fy = y4 & 3;
  const int s = n + 1;
  fetch(ref, x4 >> 2, y4 >> 2, s, s, src);
  // Horizontal stage over n+1 rows.
  for (int y = 0; y <= n; ++y) {
    const std::uint8_t* row = src + y * s;
    for (int x = 0; x < n; ++x) {
      int v;
      switch (fx) {
        case 0: v = row[x]; break;
        case 1: v = (row[x] + detail::qpel_tap(row, 1, n, x, r) + 1 - r) >> 1; break;
        case 2: v = detail::qpel_tap(row, 1, n, x, r); break;
        default: v = (row[x + 1] + detail::qpel_tap(row, 1, n, x, r) + 1 - r) >> 1; break;
      }
      hor[y * s + x] = static_cast<std::uint8_t>(v);
    }
  }
  // Vertical stage.
  for (int x = 0; x < n; ++x) {
    const std::uint8_t* col = hor + x;
    for (int y = 0; y < n; ++y) {
      int v;
      switch (fy) {
        case 0: v = col[y * s]; break;
        case 1: v = (col[y * s] + detail::qpel_tap(col, s, n, y, r) + 1 - r) >> 1; break;
        case 2: v = detail::qpel_tap(col, s, n, y, r); break;
        default: v = (col[(y + 1) * s] + detail::qpel_tap(col, s, n, y, r) + 1 - r) >> 1; break;
      }
      out[y * n + x] = static_cast<std::uint8_t>(v);
    }
  }
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      std::uint8_t& d = dst[y * dst_stride + x];
      d = average ? static_cast<std::uint8_t>((d + out[y * n + x] + 1) >> 1) : out[y * n + x];
    }
  }
}

/// Chroma vector (half-sample chroma units) for one luma vector in
/// half-sample units: luma / 2 rounded to the nearest half sample.
[[nodiscard]] constexpr int chroma_from_half(int v) noexcept { return (v >> 1) | (v & 1); }
/// Chroma vector for the sum of four luma half-sample vectors (H.263 Table 16 rounding).
[[nodiscard]] constexpr int chroma_from_sum4(int sum) noexcept {
  constexpr int round_table[16] = {0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2};
  return round_table[sum & 15] + ((sum >> 3) & ~1);
}

/// Inverse quantisation, H.263 method (quant_type 0) for AC coefficients.
[[nodiscard]] constexpr int dequant_h263(int level, int q) noexcept {
  if (level == 0) return 0;
  const int magnitude = q * (2 * std::abs(level) + 1) - ((q & 1) == 0 ? 1 : 0);
  return std::clamp(level < 0 ? -magnitude : magnitude, -2048, 2047);
}
/// Inverse quantisation, MPEG method (quant_type 1), before mismatch control.
[[nodiscard]] constexpr int dequant_mpeg(int level, int w, int q, bool intra) noexcept {
  if (level == 0) return 0;
  const int k = intra ? 0 : (level > 0 ? 1 : -1);
  return std::clamp(((2 * level + k) * w * q) / 16, -2048, 2047);
}

/// Division rounding half away from zero.
[[nodiscard]] constexpr int rounded_div(int a, int b) noexcept { return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b); }

}  // namespace cvt::legacy::mpeg4
