// Convertible core - image_modern family: demosaicing of colour filter array data.
//
// Bayer (any 2x2 arrangement):
//   * Adaptive (default): green by Hamilton-Adams directional estimates
//     (neighbour average plus same-colour Laplacian correction) blended by
//     inverse squared local gradient energy, then red/blue by colour-
//     difference interpolation (diagonals first, then the four neighbours,
//     with the same directional weights).
//   * Malvar-He-Cutler (2004) gradient-corrected linear filters.
//   * Bilinear.
// Other patterns (Fujifilm X-Trans 6x6 and any other repeating tile): green
// from the green pixels of the 3x3 neighbourhood, then red/blue from
// distance-weighted colour differences in a 5x5 window.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "families/image_modern/raw_model.hpp"

namespace cvt::image_modern::raw {

namespace {

// Mirror index (without repeating the edge sample, which keeps the parity of
// the position and so the Bayer phase).
std::uint32_t mirror(std::int64_t i, std::uint32_t n) {
  if (n == 1) return 0;
  const std::int64_t last = static_cast<std::int64_t>(n) - 1;
  for (int guard = 0; guard < 4 && (i < 0 || i > last); ++guard) {
    if (i < 0) i = -i;
    if (i > last) i = 2 * last - i;
  }
  return static_cast<std::uint32_t>(std::clamp<std::int64_t>(i, 0, last));
}

// Offset tables: index[d + 2][i] = mirror(i + d) for d in -2..2.
struct Offsets {
  std::array<std::vector<std::uint32_t>, 5> at;
  explicit Offsets(std::uint32_t n) {
    for (int d = -2; d <= 2; ++d) {
      auto& v = at[static_cast<std::size_t>(d + 2)];
      v.resize(n);
      for (std::uint32_t i = 0; i < n; ++i) v[i] = mirror(static_cast<std::int64_t>(i) + d, n);
    }
  }
  [[nodiscard]] std::uint32_t operator()(std::uint32_t i, int d) const noexcept {
    return at[static_cast<std::size_t>(d + 2)][i];
  }
};

class Bayer {
 public:
  Bayer(const std::vector<float>& plane, std::uint32_t w, std::uint32_t h, const Cfa& cfa, std::vector<float>& rgba)
      : p_(plane), w_(w), h_(h), cfa_(cfa), out_(rgba), xs_(w), ys_(h) {}

  [[nodiscard]] float raw(std::uint32_t x, std::uint32_t y, int dx, int dy) const noexcept {
    return p_[std::size_t{ys_(y, dy)} * w_ + xs_(x, dx)];
  }
  [[nodiscard]] float& out(std::uint32_t x, std::uint32_t y, int dx, int dy, int c) const noexcept {
    return out_[(std::size_t{ys_(y, dy)} * w_ + xs_(x, dx)) * 4 + static_cast<std::size_t>(c)];
  }
  [[nodiscard]] int color(std::uint32_t x, std::uint32_t y) const noexcept { return cfa_.at(x, y); }

  // Gradient energies for the directional decision.
  [[nodiscard]] float dh(std::uint32_t x, std::uint32_t y, int dy) const noexcept {
    return std::fabs(raw(x, y, -1, dy) - raw(x, y, 1, dy)) +
           std::fabs(raw(x, y, -2, dy) - 2.0f * raw(x, y, 0, dy) + raw(x, y, 2, dy));
  }
  [[nodiscard]] float dv(std::uint32_t x, std::uint32_t y, int dx) const noexcept {
    return std::fabs(raw(x, y, dx, -1) - raw(x, y, dx, 1)) +
           std::fabs(raw(x, y, dx, -2) - 2.0f * raw(x, y, dx, 0) + raw(x, y, dx, 2));
  }
  // Weight of the vertical direction (0..1).
  [[nodiscard]] float vertical_weight(std::uint32_t x, std::uint32_t y) const noexcept {
    const float hs = dh(x, y, -1) + dh(x, y, 0) + dh(x, y, 1);
    const float vs = dv(x, y, -1) + dv(x, y, 0) + dv(x, y, 1);
    const float wh = 1.0f / (1e-12f + hs * hs);
    const float wv = 1.0f / (1e-12f + vs * vs);
    return wv / (wh + wv);
  }

  void seed() {
    for (std::uint32_t y = 0; y < h_; ++y) {
      for (std::uint32_t x = 0; x < w_; ++x) {
        float* o = &out_[(std::size_t{y} * w_ + x) * 4];
        o[0] = o[1] = o[2] = 0.0f;
        o[color(x, y)] = p_[std::size_t{y} * w_ + x];
      }
    }
  }

  void adaptive() {
    seed();
    // Green at red and blue pixels.
    for (std::uint32_t y = 0; y < h_; ++y) {
      for (std::uint32_t x = 0; x < w_; ++x) {
        if (color(x, y) == 1) continue;
        const float c0 = raw(x, y, 0, 0);
        const float gh = 0.5f * (raw(x, y, -1, 0) + raw(x, y, 1, 0)) +
                         0.25f * (2.0f * c0 - raw(x, y, -2, 0) - raw(x, y, 2, 0));
        const float gv = 0.5f * (raw(x, y, 0, -1) + raw(x, y, 0, 1)) +
                         0.25f * (2.0f * c0 - raw(x, y, 0, -2) - raw(x, y, 0, 2));
        const float wv = vertical_weight(x, y);
        float g = wv * gv + (1.0f - wv) * gh;
        // Keep the estimate within a margin of the four green neighbours
        // (suppresses overshoot at hard edges).
        const float n1 = raw(x, y, -1, 0), n2 = raw(x, y, 1, 0), n3 = raw(x, y, 0, -1), n4 = raw(x, y, 0, 1);
        const float lo = std::min({n1, n2, n3, n4});
        const float hi = std::max({n1, n2, n3, n4});
        const float margin = 0.5f * (hi - lo);
        g = std::clamp(g, lo - margin, hi + margin);
        out(x, y, 0, 0, 1) = g;
      }
    }
    // Blue at red pixels and red at blue pixels, from the diagonals.
    for (std::uint32_t y = 0; y < h_; ++y) {
      for (std::uint32_t x = 0; x < w_; ++x) {
        const int c = color(x, y);
        if (c == 1) continue;
        const int other = 2 - c;
        float sum = 0.0f;
        for (int dy = -1; dy <= 1; dy += 2) {
          for (int dx = -1; dx <= 1; dx += 2) sum += out(x, y, dx, dy, other) - out(x, y, dx, dy, 1);
        }
        out(x, y, 0, 0, other) = out(x, y, 0, 0, 1) + 0.25f * sum;
      }
    }
    // Red and blue at green pixels, from the four neighbours.
    for (std::uint32_t y = 0; y < h_; ++y) {
      for (std::uint32_t x = 0; x < w_; ++x) {
        if (color(x, y) != 1) continue;
        const float wv = vertical_weight(x, y);
        const float g0 = out(x, y, 0, 0, 1);
        for (int c = 0; c <= 2; c += 2) {
          const float horizontal = 0.5f * ((out(x, y, -1, 0, c) - out(x, y, -1, 0, 1)) + (out(x, y, 1, 0, c) - out(x, y, 1, 0, 1)));
          const float vertical = 0.5f * ((out(x, y, 0, -1, c) - out(x, y, 0, -1, 1)) + (out(x, y, 0, 1, c) - out(x, y, 0, 1, 1)));
          out(x, y, 0, 0, c) = g0 + wv * vertical + (1.0f - wv) * horizontal;
        }
      }
    }
  }

  void malvar() {
    seed();
    for (std::uint32_t y = 0; y < h_; ++y) {
      for (std::uint32_t x = 0; x < w_; ++x) {
        const int c = color(x, y);
        const auto P = [&](int dx, int dy) { return raw(x, y, dx, dy); };
        const float c0 = P(0, 0);
        if (c != 1) {
          // Green at red/blue.
          const float g = (4.0f * c0 + 2.0f * (P(-1, 0) + P(1, 0) + P(0, -1) + P(0, 1)) -
                           (P(-2, 0) + P(2, 0) + P(0, -2) + P(0, 2))) / 8.0f;
          // The other chroma at red/blue.
          const float o = (6.0f * c0 + 2.0f * (P(-1, -1) + P(1, -1) + P(-1, 1) + P(1, 1)) -
                           1.5f * (P(-2, 0) + P(2, 0) + P(0, -2) + P(0, 2))) / 8.0f;
          out(x, y, 0, 0, 1) = g;
          out(x, y, 0, 0, 2 - c) = o;
        } else {
          // At green: the colour of the horizontal neighbours and of the vertical neighbours.
          const int hc = color(xs_(x, 1), y);
          const int vc = 2 - hc;
          const float row_kernel = (5.0f * c0 + 4.0f * (P(-1, 0) + P(1, 0)) - (P(-2, 0) + P(2, 0)) -
                                    (P(-1, -1) + P(1, -1) + P(-1, 1) + P(1, 1)) + 0.5f * (P(0, -2) + P(0, 2))) / 8.0f;
          const float col_kernel = (5.0f * c0 + 4.0f * (P(0, -1) + P(0, 1)) - (P(0, -2) + P(0, 2)) -
                                    (P(-1, -1) + P(1, -1) + P(-1, 1) + P(1, 1)) + 0.5f * (P(-2, 0) + P(2, 0))) / 8.0f;
          out(x, y, 0, 0, hc) = row_kernel;
          out(x, y, 0, 0, vc) = col_kernel;
        }
      }
    }
  }

  void bilinear() {
    seed();
    for (std::uint32_t y = 0; y < h_; ++y) {
      for (std::uint32_t x = 0; x < w_; ++x) {
        const int c0 = color(x, y);
        std::array<float, 3> sum{};
        std::array<int, 3> count{};
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const int c = color(xs_(x, dx), ys_(y, dy));
            sum[static_cast<std::size_t>(c)] += raw(x, y, dx, dy);
            ++count[static_cast<std::size_t>(c)];
          }
        }
        for (int c = 0; c < 3; ++c) {
          if (c == c0) continue;
          const auto i = static_cast<std::size_t>(c);
          out(x, y, 0, 0, c) = count[i] > 0 ? sum[i] / static_cast<float>(count[i]) : 0.0f;
        }
      }
    }
  }

 private:
  const std::vector<float>& p_;
  std::uint32_t w_, h_;
  const Cfa& cfa_;
  std::vector<float>& out_;
  Offsets xs_, ys_;
};

// Any repeating pattern (X-Trans and others), with bounds checks.
void generic(const std::vector<float>& p, std::uint32_t w, std::uint32_t h, const Cfa& cfa, std::vector<float>& out) {
  const auto at = [&](std::int64_t x, std::int64_t y) { return std::size_t(y) * w + std::size_t(x); };
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      float* o = &out[at(x, y) * 4];
      o[0] = o[1] = o[2] = 0.0f;
      o[cfa.at(x, y)] = p[at(x, y)];
    }
  }
  // Green: mean of green pixels within radius 1 (2 if none), weighted by
  // closeness of their value to the local same-colour context.
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      if (cfa.at(x, y) == 1) continue;
      for (int radius = 1; radius <= 2; ++radius) {
        float sum = 0.0f, weight = 0.0f;
        for (int dy = -radius; dy <= radius; ++dy) {
          for (int dx = -radius; dx <= radius; ++dx) {
            const std::int64_t xx = std::int64_t{x} + dx, yy = std::int64_t{y} + dy;
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            if (cfa.at(static_cast<std::uint32_t>(xx), static_cast<std::uint32_t>(yy)) != 1) continue;
            const float wt = 1.0f / static_cast<float>(dx * dx + dy * dy);
            sum += wt * p[at(xx, yy)];
            weight += wt;
          }
        }
        if (weight > 0.0f) {
          out[at(x, y) * 4 + 1] = sum / weight;
          break;
        }
      }
    }
  }
  // Red and blue from colour differences.
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const int c0 = cfa.at(x, y);
      const float g0 = out[at(x, y) * 4 + 1];
      for (int c = 0; c <= 2; c += 2) {
        if (c == c0) continue;
        float sum = 0.0f, weight = 0.0f;
        for (int radius = 1; radius <= 3 && weight == 0.0f; ++radius) {
          for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
              const std::int64_t xx = std::int64_t{x} + dx, yy = std::int64_t{y} + dy;
              if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
              if (cfa.at(static_cast<std::uint32_t>(xx), static_cast<std::uint32_t>(yy)) != c) continue;
              const float wt = 1.0f / static_cast<float>(dx * dx + dy * dy);
              sum += wt * (p[at(xx, yy)] - out[at(xx, yy) * 4 + 1]);
              weight += wt;
            }
          }
        }
        out[at(x, y) * 4 + static_cast<std::size_t>(c)] = weight > 0.0f ? g0 + sum / weight : g0;
      }
    }
  }
}

}  // namespace

bool Cfa::bayer() const noexcept {
  if (width != 2 || height != 2) return false;
  int count[3] = {0, 0, 0};
  for (std::size_t i = 0; i < 4; ++i) {
    if (colors[i] > 2) return false;
    ++count[colors[i]];
  }
  // Greens on one diagonal, red and blue on the other.
  return count[0] == 1 && count[1] == 2 && count[2] == 1 && (colors[0] == 1) == (colors[3] == 1);
}

Cfa Cfa::shifted(std::uint32_t dx, std::uint32_t dy) const noexcept {
  Cfa out = *this;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) out.colors[y * width + x] = static_cast<std::uint8_t>(at(x + dx, y + dy));
  }
  return out;
}

Cfa Cfa::bayer_from(std::string_view rggb) {
  Cfa cfa;
  cfa.width = 2;
  cfa.height = 2;
  for (std::size_t i = 0; i < 4 && i < rggb.size(); ++i) {
    cfa.colors[i] = rggb[i] == 'R' ? 0 : (rggb[i] == 'G' ? 1 : 2);
  }
  return cfa;
}

void demosaic(const std::vector<float>& plane, std::uint32_t width, std::uint32_t height, const Cfa& cfa,
              DevelopOptions::Demosaic method, std::vector<float>& rgba) {
  if (cfa.bayer() && width >= 4 && height >= 4) {
    Bayer bayer(plane, width, height, cfa, rgba);
    switch (method) {
      case DevelopOptions::Demosaic::Adaptive: bayer.adaptive(); return;
      case DevelopOptions::Demosaic::Malvar: bayer.malvar(); return;
      case DevelopOptions::Demosaic::Bilinear: bayer.bilinear(); return;
    }
  }
  generic(plane, width, height, cfa, rgba);
}

}  // namespace cvt::image_modern::raw
