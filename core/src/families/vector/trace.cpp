// Convertible core - vector family: raster pictures into vector scenes.
//
// Tracing pipeline: (1) quantise the picture (image_ops) or threshold it,
// (2) build one binary mask per output layer - layers are stacked from the
// largest colour region upwards and each mask is the union of its colour and
// every colour drawn after it, so no gaps can open between regions, (3) follow
// the pixel-crack boundaries of each mask into closed loops (outer contours
// and holes, filled with the even-odd rule), (4) drop specks, (5) find corners
// on a simplified polygon and (6) fit cubic Béziers between corners through
// the edge midpoints (Schneider's least-squares method with Newton
// reparameterisation), within the requested tolerance.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/vector/trace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <vector>

#include "families/image/image_ops.hpp"
#include "families/vector/geom.hpp"

namespace cvt::vector {

namespace {

// ------------------------------------------------------------------ boundary loops

struct BitMask {
  int w = 0, h = 0;
  std::vector<std::uint8_t> bits;
  [[nodiscard]] bool at(int x, int y) const {
    return x >= 0 && y >= 0 && x < w && y < h && bits[static_cast<std::size_t>(y) * w + x] != 0;
  }
};

using Loop = std::vector<Point>;  // grid vertices, closed

// Boundary loops of the set pixels; the inside is on the right of the walking direction (y down).
std::vector<Loop> boundary_loops(const BitMask& m) {
  // Directed edges keyed by start vertex. Direction codes: 0 right, 1 down, 2 left, 3 up.
  const int vw = m.w + 1;
  const auto vid = [&](int x, int y) { return static_cast<std::size_t>(y) * vw + x; };
  std::vector<std::array<bool, 4>> out(static_cast<std::size_t>(vw) * (m.h + 1), {false, false, false, false});
  std::size_t edges = 0;
  for (int y = 0; y < m.h; ++y) {
    for (int x = 0; x < m.w; ++x) {
      if (!m.at(x, y)) continue;
      // Inside on the right: top edge goes right, right edge goes down, bottom goes left, left goes up.
      if (!m.at(x, y - 1)) out[vid(x, y)][0] = true, ++edges;
      if (!m.at(x + 1, y)) out[vid(x + 1, y)][1] = true, ++edges;
      if (!m.at(x, y + 1)) out[vid(x + 1, y + 1)][2] = true, ++edges;
      if (!m.at(x - 1, y)) out[vid(x, y + 1)][3] = true, ++edges;
    }
  }
  std::vector<Loop> loops;
  constexpr int dx[4] = {1, 0, -1, 0};
  constexpr int dy[4] = {0, 1, 0, -1};
  for (int y = 0; y <= m.h; ++y) {
    for (int x = 0; x <= m.w; ++x) {
      for (int start_dir = 0; start_dir < 4; ++start_dir) {
        if (!out[vid(x, y)][static_cast<std::size_t>(start_dir)]) continue;
        Loop loop;
        int cx = x, cy = y, dir = start_dir;
        std::size_t guard = 0;
        while (true) {
          out[vid(cx, cy)][static_cast<std::size_t>(dir)] = false;
          loop.push_back({static_cast<double>(cx), static_cast<double>(cy)});
          cx += dx[dir];
          cy += dy[dir];
          if (cx == x && cy == y) break;
          // Prefer turning right (keeps diagonal-touching pixels apart), then straight, then left.
          const int right = (dir + 1) % 4, left = (dir + 3) % 4;
          const auto& o = out[vid(cx, cy)];
          if (o[static_cast<std::size_t>(right)]) dir = right;
          else if (o[static_cast<std::size_t>(dir)]) {}
          else if (o[static_cast<std::size_t>(left)]) dir = left;
          else break;  // cannot happen for a valid boundary
          if (++guard > edges) break;
        }
        // Keep only the corners.
        Loop corners;
        const std::size_t n = loop.size();
        for (std::size_t i = 0; i < n; ++i) {
          const Point a = loop[(i + n - 1) % n], b = loop[i], c = loop[(i + 1) % n];
          if (cross(b - a, c - b) != 0) corners.push_back(b);
        }
        if (corners.size() >= 4) loops.push_back(std::move(corners));
      }
    }
  }
  return loops;
}

// ------------------------------------------------------------------ curve fitting

Point unit(Point p) {
  const double l = length(p);
  return l > 1e-12 ? p * (1.0 / l) : Point{0, 0};
}

struct Bezier {
  Point p0, p1, p2, p3;
};

Point bez(const Bezier& b, double t) { return eval_cubic(b.p0, b.p1, b.p2, b.p3, t); }

Point bez_d1(const Bezier& b, double t) {
  const double u = 1 - t;
  return (b.p1 - b.p0) * (3 * u * u) + (b.p2 - b.p1) * (6 * u * t) + (b.p3 - b.p2) * (3 * t * t);
}

Point bez_d2(const Bezier& b, double t) {
  return (b.p2 - b.p1 * 2 + b.p0) * (6 * (1 - t)) + (b.p3 - b.p2 * 2 + b.p1) * (6 * t);
}

Bezier generate(const std::vector<Point>& pts, std::size_t first, std::size_t last, const std::vector<double>& u,
                Point t1, Point t2) {
  const Point p0 = pts[first], p3 = pts[last];
  double c00 = 0, c01 = 0, c11 = 0, x0 = 0, x1 = 0;
  for (std::size_t i = 0; i < u.size(); ++i) {
    const double t = u[i], s = 1 - t;
    const double b0 = s * s * s, b1 = 3 * t * s * s, b2 = 3 * t * t * s, b3 = t * t * t;
    const Point a1 = t1 * b1, a2 = t2 * b2;
    c00 += dot(a1, a1);
    c01 += dot(a1, a2);
    c11 += dot(a2, a2);
    const Point tmp = pts[first + i] - (p0 * (b0 + b1) + p3 * (b2 + b3));
    x0 += dot(a1, tmp);
    x1 += dot(a2, tmp);
  }
  const double det = c00 * c11 - c01 * c01;
  double alpha1 = 0, alpha2 = 0;
  if (std::abs(det) > 1e-12) {
    alpha1 = (x0 * c11 - x1 * c01) / det;
    alpha2 = (c00 * x1 - c01 * x0) / det;
  }
  const double seg = length(p3 - p0);
  if (alpha1 < seg * 1e-6 || alpha2 < seg * 1e-6) alpha1 = alpha2 = seg / 3;
  return {p0, p0 + t1 * alpha1, p3 + t2 * alpha2, p3};
}

double max_error(const std::vector<Point>& pts, std::size_t first, const std::vector<double>& u, const Bezier& b,
                 std::size_t& split) {
  double worst = 0;
  split = first + u.size() / 2;
  for (std::size_t i = 1; i + 1 < u.size(); ++i) {
    const double d = length(bez(b, u[i]) - pts[first + i]);
    if (d > worst) {
      worst = d;
      split = first + i;
    }
  }
  return worst;
}

void fit_cubic(const std::vector<Point>& pts, std::size_t first, std::size_t last, Point t1, Point t2, double tolerance,
               std::vector<Bezier>& out, int depth) {
  const std::size_t n = last - first + 1;
  if (n == 2 || depth > 24) {
    const double d = length(pts[last] - pts[first]) / 3;
    out.push_back({pts[first], pts[first] + t1 * d, pts[last] + t2 * d, pts[last]});
    return;
  }
  // Chord-length parameterisation.
  std::vector<double> u(n, 0.0);
  for (std::size_t i = 1; i < n; ++i) u[i] = u[i - 1] + length(pts[first + i] - pts[first + i - 1]);
  if (u.back() <= 0) return;
  for (double& v : u) v /= u.back();
  Bezier b = generate(pts, first, last, u, t1, t2);
  std::size_t split = 0;
  double err = max_error(pts, first, u, b, split);
  if (err < tolerance) {
    out.push_back(b);
    return;
  }
  if (err < tolerance * 4) {
    for (int iter = 0; iter < 4; ++iter) {
      for (std::size_t i = 0; i < n; ++i) {
        const Point d = bez(b, u[i]) - pts[first + i];
        const Point d1 = bez_d1(b, u[i]), d2 = bez_d2(b, u[i]);
        const double num = dot(d, d1);
        const double den = dot(d1, d1) + dot(d, d2);
        if (std::abs(den) > 1e-12) u[i] = std::clamp(u[i] - num / den, 0.0, 1.0);
      }
      b = generate(pts, first, last, u, t1, t2);
      err = max_error(pts, first, u, b, split);
      if (err < tolerance) {
        out.push_back(b);
        return;
      }
    }
  }
  if (split <= first) split = first + 1;
  if (split >= last) split = last - 1;
  // Tangent from a wider window: neighbouring staircase samples are biased.
  const std::size_t k = std::min<std::size_t>({4, split - first, last - split});
  const Point center = unit(pts[split - k] - pts[split + k]);
  fit_cubic(pts, first, split, t1, center, tolerance, out, depth + 1);
  fit_cubic(pts, split, last, center * -1.0, t2, tolerance, out, depth + 1);
}

// Douglas-Peucker on an open polyline: indices of the kept points.
void simplify(const std::vector<Point>& pts, std::size_t a, std::size_t b, double tol, std::vector<bool>& keep) {
  if (b <= a + 1) return;
  const Point d = pts[b] - pts[a];
  const double len = length(d);
  double worst = -1;
  std::size_t at = a;
  for (std::size_t i = a + 1; i < b; ++i) {
    const double dist = len > 1e-12 ? std::abs(cross(d, pts[i] - pts[a])) / len : length(pts[i] - pts[a]);
    if (dist > worst) {
      worst = dist;
      at = i;
    }
  }
  if (worst > tol) {
    keep[at] = true;
    simplify(pts, a, at, tol, keep);
    simplify(pts, at, b, tol, keep);
  }
}

// One closed loop of grid corners into a smooth path.
void loop_to_path(const Loop& loop, const TraceOptions& o, Path& path) {
  // Edge midpoints of the staircase: a smooth, unbiased sample of the outline.
  std::vector<Point> mids;
  const std::size_t n = loop.size();
  const auto edge_len = [&](std::size_t i) { return length(loop[(i + 1) % n] - loop[i % n]); };
  for (std::size_t i = 0; i < n; ++i) {
    const Point a = loop[i], b = loop[(i + 1) % n];
    const double len = edge_len(i);
    if (len < 2) {
      mids.push_back(lerp(a, b, 0.5));
      continue;
    }
    // A long run keeps its ends; where it meets another long run the grid
    // vertex itself is the corner.
    const bool prev_long = edge_len(i + n - 1) >= 2, next_long = edge_len(i + 1) >= 2;
    const Point start = prev_long ? a : lerp(a, b, 0.5 / len);
    const Point end = next_long ? b : lerp(a, b, 1 - 0.5 / len);
    if (mids.empty() || !(mids.back() == start)) mids.push_back(start);
    if (!next_long) mids.push_back(end);
  }
  if (mids.size() > 1 && mids.front() == mids.back()) mids.pop_back();
  const std::size_t m = mids.size();
  if (m < 3) return;
  // Corners: simplify, then measure turns at the kept points.
  std::vector<Point> ring = mids;
  ring.push_back(mids[0]);
  std::vector<bool> keep(ring.size(), false);
  keep.front() = keep.back() = true;
  // Make the seam a natural break: start the simplification at the farthest point too.
  std::size_t far = 0;
  double far_d = 0;
  for (std::size_t i = 0; i < m; ++i) {
    const double d = length(mids[i] - mids[0]);
    if (d > far_d) {
      far_d = d;
      far = i;
    }
  }
  keep[far] = true;
  const double dp_tol = std::max(0.6, std::min(o.tolerance, 1.5));
  simplify(ring, 0, far, dp_tol, keep);
  simplify(ring, far, ring.size() - 1, dp_tol, keep);
  std::vector<std::size_t> key;
  for (std::size_t i = 0; i < m; ++i) {
    if (keep[i]) key.push_back(i);
  }
  const double corner_cos = std::cos(o.corner_angle * std::numbers::pi / 180.0);
  std::vector<std::size_t> corners;
  for (std::size_t k = 0; k < key.size(); ++k) {
    const Point prev = mids[key[(k + key.size() - 1) % key.size()]];
    const Point cur = mids[key[k]];
    const Point next = mids[key[(k + 1) % key.size()]];
    const Point d1 = unit(cur - prev), d2 = unit(next - cur);
    // Turn angle between successive directions; a sharp one is a corner.
    if (dot(d1, d2) <= corner_cos) corners.push_back(key[k]);
  }
  std::vector<Bezier> curves;
  if (corners.empty()) {
    // Smooth closed curve: start at the point farthest from the first one.
    std::vector<Point> rotated;
    for (std::size_t i = 0; i <= m; ++i) rotated.push_back(mids[(far + i) % m]);
    const Point s1 = unit(mids[(far + 4) % m] - mids[(far + m - 4) % m]);
    fit_cubic(rotated, 0, rotated.size() - 1, s1, s1 * -1.0, o.tolerance, curves, 0);
  } else {
    for (std::size_t c = 0; c < corners.size(); ++c) {
      const std::size_t a = corners[c], b = corners[(c + 1) % corners.size()];
      std::vector<Point> seq;
      std::size_t i = a;
      seq.push_back(mids[i]);
      do {
        i = (i + 1) % m;
        seq.push_back(mids[i]);
      } while (i != b);
      const std::size_t len = seq.size();
      const Point t1 = unit(seq[std::min<std::size_t>(3, len - 1)] - seq[0]);
      const Point t2 = unit(seq[len >= 4 ? len - 4 : 0] - seq[len - 1]);
      fit_cubic(seq, 0, len - 1, t1, t2, o.tolerance, curves, 0);
    }
  }
  if (curves.empty()) return;
  path.move_to(curves.front().p0);
  for (const Bezier& b : curves) {
    // Nearly straight curves become lines.
    const Point d = b.p3 - b.p0;
    const double len = length(d);
    const double dev1 = len > 1e-9 ? std::abs(cross(d, b.p1 - b.p0)) / len : length(b.p1 - b.p0);
    const double dev2 = len > 1e-9 ? std::abs(cross(d, b.p2 - b.p0)) / len : length(b.p2 - b.p0);
    if (dev1 < 0.05 && dev2 < 0.05) {
      path.line_to(b.p3);
    } else {
      path.cubic_to(b.p1, b.p2, b.p3);
    }
  }
  path.close();
}

Result<Path> trace_mask(const BitMask& mask, const TraceOptions& o, const Context& ctx) {
  const std::vector<Loop> loops = boundary_loops(mask);
  Path path;
  std::size_t done = 0;
  for (const Loop& loop : loops) {
    if ((++done & 255) == 0) CVT_TRY(ctx.checkpoint(0.5, "Tracing"));
    if (std::abs(signed_area(loop)) < o.speckle) continue;
    loop_to_path(loop, o, path);
  }
  return path;
}

}  // namespace

Result<Scene> image_to_scene(const ir::Image& image, const TraceOptions& options, const Context& ctx) {
  if (!image.valid()) return fail(Status::Encode, "the image has no pixels");
  const ir::Frame& frame = image.frames.front();
  Scene scene;
  Page page;
  page.width = frame.width;
  page.height = frame.height;
  if (options.mode == TraceOptions::Mode::Embed) {
    Picture picture;
    picture.pixels = std::make_shared<const ir::Frame>(frame);
    picture.bounds = Rect::xywh(0, 0, frame.width, frame.height);
    page.root.children.push_back(Node{std::move(picture)});
    scene.pages.push_back(std::move(page));
    return scene;
  }
  CVT_TRY(ctx.limits.check_pixels(frame.width, frame.height));
  const int w = static_cast<int>(frame.width), h = static_cast<int>(frame.height);
  const std::size_t pixels = static_cast<std::size_t>(w) * h;
  if (options.mode == TraceOptions::Mode::BlackWhite) {
    BitMask mask{w, h, std::vector<std::uint8_t>(pixels, 0)};
    for (std::size_t i = 0; i < pixels; ++i) {
      const std::uint8_t* p = &frame.rgba[i * 4];
      const double luma = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
      mask.bits[i] = (p[3] >= 128 && luma < options.threshold) ? 1 : 0;
    }
    CVT_TRY_ASSIGN(Path path, trace_mask(mask, options, ctx));
    if (!path.empty()) {
      Shape s;
      s.path = std::move(path);
      s.fill = Paint::solid({0, 0, 0, 1});
      s.fill_rule = FillRule::EvenOdd;
      page.root.children.push_back(Node{std::move(s)});
    }
    scene.pages.push_back(std::move(page));
    return scene;
  }
  // Colour: quantise, then stack layers from the largest region up.
  image::QuantizeOptions q;
  q.max_colors = std::clamp(options.colors, 2, 64);
  q.dither = image::Dither::None;
  q.keep_alpha = false;
  CVT_TRY_ASSIGN(const image::Quantized quantized, image::quantize(frame, q));
  std::vector<std::size_t> counts(quantized.palette.size(), 0);
  for (const std::uint8_t index : quantized.indices) {
    if (static_cast<int>(index) != quantized.transparent_index) ++counts[index];
  }
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < counts.size(); ++i) {
    if (counts[i] > 0) order.push_back(i);
  }
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return counts[a] > counts[b]; });
  std::vector<int> rank(quantized.palette.size(), -1);
  for (std::size_t r = 0; r < order.size(); ++r) rank[order[r]] = static_cast<int>(r);
  for (std::size_t r = 0; r < order.size(); ++r) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(order.size()), "Tracing"));
    BitMask mask{w, h, std::vector<std::uint8_t>(pixels, 0)};
    for (std::size_t i = 0; i < pixels; ++i) {
      const int k = rank[quantized.indices[i]];
      mask.bits[i] = (k >= static_cast<int>(r)) ? 1 : 0;
    }
    CVT_TRY_ASSIGN(Path path, trace_mask(mask, options, ctx));
    if (path.empty()) continue;
    const image::Rgba c = quantized.palette[order[r]];
    Shape s;
    s.path = std::move(path);
    s.fill = Paint::solid(Color::rgb8(c[0], c[1], c[2]));
    s.fill_rule = FillRule::EvenOdd;
    page.root.children.push_back(Node{std::move(s)});
  }
  scene.pages.push_back(std::move(page));
  return scene;
}

}  // namespace cvt::vector
