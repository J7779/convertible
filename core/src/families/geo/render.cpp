// Convertible core - geo family: map sketch renderer (see render.hpp).
//
// A small anti-aliased scanline rasteriser: shapes are filled with the
// even-odd rule using five sub-scanlines per pixel row and exact horizontal
// span coverage; strokes are the union (maximum coverage) of one quad per
// segment and a disc per vertex, so overlapping parts are not darkened.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/render.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "families/geo/geo.hpp"
#include "families/geo/geo_ops.hpp"

namespace cvt::geo {

std::optional<Rgba> parse_color(std::string_view text) {
  if (text.starts_with('#')) text.remove_prefix(1);
  std::string hex(text);
  if (hex.size() == 3) hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
  if (hex.size() != 6 && hex.size() != 8) return std::nullopt;
  std::uint32_t v = 0;
  const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), v, 16);
  if (ec != std::errc{} || end != hex.data() + hex.size()) return std::nullopt;
  if (hex.size() == 6) v = (v << 8) | 0xFF;
  return Rgba{static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8),
              static_cast<std::uint8_t>(v)};
}

namespace {

constexpr int subsamples = 5;

struct P {
  double x, y;
};

struct Edge {
  double xa, ya, xb, yb;  // ya < yb
};

// Calls emit(x, y, coverage) for every pixel the even-odd fill of `rings` touches.
template <class Emit>
void rasterize(const std::vector<std::vector<P>>& rings, int width, int height, Emit&& emit) {
  std::vector<Edge> edges;
  double min_x = 1e300, max_x = -1e300, min_y = 1e300, max_y = -1e300;
  for (const auto& ring : rings) {
    for (std::size_t i = 0; i < ring.size(); ++i) {
      const P& a = ring[i];
      const P& b = ring[(i + 1) % ring.size()];
      min_x = std::min(min_x, a.x);
      max_x = std::max(max_x, a.x);
      min_y = std::min(min_y, a.y);
      max_y = std::max(max_y, a.y);
      if (a.y == b.y || !std::isfinite(a.x) || !std::isfinite(b.x) || !std::isfinite(a.y) || !std::isfinite(b.y)) continue;
      edges.push_back(a.y < b.y ? Edge{a.x, a.y, b.x, b.y} : Edge{b.x, b.y, a.x, a.y});
    }
  }
  if (edges.empty()) return;
  const int x0 = std::max(0, static_cast<int>(std::floor(min_x)));
  const int x1 = std::min(width - 1, static_cast<int>(std::ceil(max_x)));
  const int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
  const int y1 = std::min(height - 1, static_cast<int>(std::ceil(max_y)));
  if (x0 > x1 || y0 > y1) return;
  std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.ya < b.ya; });
  std::vector<float> acc(static_cast<std::size_t>(x1 - x0 + 2));
  std::vector<const Edge*> active;
  std::vector<double> xs;
  std::size_t next = 0;
  const float weight = 1.0f / subsamples;
  for (int y = y0; y <= y1; ++y) {
    std::fill(acc.begin(), acc.end(), 0.0f);
    while (next < edges.size() && edges[next].ya < y + 1) active.push_back(&edges[next++]);
    std::erase_if(active, [&](const Edge* e) { return e->yb < y; });
    bool any = false;
    for (int s = 0; s < subsamples; ++s) {
      const double sy = y + (s + 0.5) / subsamples;
      xs.clear();
      for (const Edge* e : active) {
        if ((e->ya <= sy) != (e->yb <= sy)) xs.push_back(e->xa + (sy - e->ya) * (e->xb - e->xa) / (e->yb - e->ya));
      }
      std::sort(xs.begin(), xs.end());
      for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
        const double l = std::clamp(xs[i], static_cast<double>(x0), static_cast<double>(x1 + 1));
        const double r = std::clamp(xs[i + 1], static_cast<double>(x0), static_cast<double>(x1 + 1));
        if (r <= l) continue;
        any = true;
        const int il = static_cast<int>(std::floor(l));
        const int ir = static_cast<int>(std::floor(r));
        if (il == ir) {
          acc[static_cast<std::size_t>(il - x0)] += static_cast<float>(r - l) * weight;
        } else {
          acc[static_cast<std::size_t>(il - x0)] += static_cast<float>(il + 1 - l) * weight;
          for (int k = il + 1; k < ir; ++k) acc[static_cast<std::size_t>(k - x0)] += weight;
          if (ir <= x1) acc[static_cast<std::size_t>(ir - x0)] += static_cast<float>(r - ir) * weight;
        }
      }
    }
    if (!any) continue;
    for (int x = x0; x <= x1; ++x) {
      const float c = acc[static_cast<std::size_t>(x - x0)];
      if (c > 0.002f) emit(x, y, std::min(1.0f, c));
    }
  }
}

struct Canvas {
  int width = 0, height = 0;
  ir::Frame frame;

  void blend(int x, int y, const Rgba& color, float coverage) {
    const float a = coverage * static_cast<float>(color[3]) / 255.0f;
    if (a <= 0) return;
    const std::size_t at = frame.offset(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    for (int c = 0; c < 3; ++c) {
      const float d = frame.rgba[at + static_cast<std::size_t>(c)];
      frame.rgba[at + static_cast<std::size_t>(c)] =
          static_cast<std::uint8_t>(std::lround(d + (static_cast<float>(color[static_cast<std::size_t>(c)]) - d) * a));
    }
  }
};

// Stroke coverage of polylines, composited once (union of quads and discs).
void stroke(Canvas& canvas, const std::vector<std::vector<P>>& lines, double width, const Rgba& color, bool closed) {
  if (width <= 0) return;
  const double hw = width / 2;
  double min_x = 1e300, max_x = -1e300, min_y = 1e300, max_y = -1e300;
  for (const auto& l : lines) {
    for (const P& p : l) {
      min_x = std::min(min_x, p.x);
      max_x = std::max(max_x, p.x);
      min_y = std::min(min_y, p.y);
      max_y = std::max(max_y, p.y);
    }
  }
  const int x0 = std::max(0, static_cast<int>(std::floor(min_x - hw - 1)));
  const int x1 = std::min(canvas.width - 1, static_cast<int>(std::ceil(max_x + hw + 1)));
  const int y0 = std::max(0, static_cast<int>(std::floor(min_y - hw - 1)));
  const int y1 = std::min(canvas.height - 1, static_cast<int>(std::ceil(max_y + hw + 1)));
  if (x0 > x1 || y0 > y1) return;
  const auto mw = static_cast<std::size_t>(x1 - x0 + 1);
  std::vector<float> mask(mw * static_cast<std::size_t>(y1 - y0 + 1), 0.0f);
  const auto add = [&](const std::vector<P>& shape) {
    rasterize({shape}, canvas.width, canvas.height, [&](int x, int y, float c) {
      if (x < x0 || x > x1 || y < y0 || y > y1) return;
      float& m = mask[static_cast<std::size_t>(y - y0) * mw + static_cast<std::size_t>(x - x0)];
      m = std::max(m, c);
    });
  };
  const auto disc = [&](const P& c) {
    std::vector<P> shape;
    const int n = hw > 3 ? 16 : 10;
    for (int i = 0; i < n; ++i) {
      const double t = 2 * std::numbers::pi * i / n;
      shape.push_back({c.x + hw * std::cos(t), c.y + hw * std::sin(t)});
    }
    add(shape);
  };
  for (const auto& l : lines) {
    for (std::size_t i = 0; i + 1 < l.size() + (closed ? 1 : 0) && l.size() >= 2; ++i) {
      const P& a = l[i];
      const P& b = l[(i + 1) % l.size()];
      const double dx = b.x - a.x, dy = b.y - a.y;
      const double len = std::hypot(dx, dy);
      if (len < 1e-9) continue;
      const double nx = -dy / len * hw, ny = dx / len * hw;
      add({{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}});
    }
    if (width >= 2.0) {
      for (const P& p : l) disc(p);
    }
  }
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const float m = mask[static_cast<std::size_t>(y - y0) * mw + static_cast<std::size_t>(x - x0)];
      if (m > 0) canvas.blend(x, y, color, m);
    }
  }
}

void dot(Canvas& canvas, const P& c, double radius, const Rgba& color) {
  std::vector<P> outer, inner;
  for (int i = 0; i < 20; ++i) {
    const double t = 2 * std::numbers::pi * i / 20;
    outer.push_back({c.x + (radius + 1.5) * std::cos(t), c.y + (radius + 1.5) * std::sin(t)});
    inner.push_back({c.x + radius * std::cos(t), c.y + radius * std::sin(t)});
  }
  const Rgba halo{255, 255, 255, 230};
  rasterize({outer}, canvas.width, canvas.height, [&](int x, int y, float cov) { canvas.blend(x, y, halo, cov); });
  rasterize({inner}, canvas.width, canvas.height, [&](int x, int y, float cov) { canvas.blend(x, y, color, cov); });
}

struct Projector {
  bool mercator = true;
  double scale = 1, ox = 0, oy = 0;
  int height = 0;

  static double merc(double lat) {
    const double clamped = std::clamp(lat, -85.0, 85.0);
    return std::log(std::tan(std::numbers::pi / 4 + clamped * std::numbers::pi / 360)) * 180 / std::numbers::pi;
  }
  double y_of(double lat) const { return mercator ? merc(lat) : lat; }
  P operator()(double lon, double lat) const {
    return {(lon - ox) * scale, static_cast<double>(height) - (y_of(lat) - oy) * scale};
  }
};

Rgba with_opacity(Rgba c, double opacity) {
  c[3] = static_cast<std::uint8_t>(std::lround(std::clamp(opacity, 0.0, 1.0) * c[3]));
  return c;
}

struct Painter {
  Canvas& canvas;
  const Projector& project;
  const RenderOptions& options;
  int pass = 0;  // 0 fills, 1 strokes, 2 points

  std::vector<P> to_pixels(const std::vector<ir::Coord>& pts) const {
    std::vector<P> out;
    out.reserve(pts.size());
    for (const ir::Coord& c : pts) out.push_back(project(c.lon, c.lat));
    return out;
  }

  void polygon(const ir::Polygon& p, const ir::Style& s) {
    std::vector<std::vector<P>> rings;
    for (const auto& r : p.rings) rings.push_back(to_pixels(r));
    if (pass == 0) {
      Rgba fill = options.fill;
      if (auto c = parse_color(s.fill)) fill = with_opacity(Rgba{(*c)[0], (*c)[1], (*c)[2], 255}, 0.35 * s.fill_opacity);
      rasterize(rings, canvas.width, canvas.height, [&](int x, int y, float cov) { canvas.blend(x, y, fill, cov); });
    } else if (pass == 1) {
      Rgba line = options.line;
      if (auto c = parse_color(s.stroke)) line = with_opacity(*c, s.stroke_opacity);
      stroke(canvas, rings, std::max(1.0, (s.stroke_width > 0 ? s.stroke_width : options.line_width) * 0.6), line, true);
    }
  }
  void line(const std::vector<ir::Coord>& pts, const ir::Style& s) {
    if (pass != 1) return;
    Rgba color = options.line;
    if (auto c = parse_color(s.stroke)) color = with_opacity(*c, s.stroke_opacity);
    stroke(canvas, {to_pixels(pts)}, s.stroke_width > 0 ? s.stroke_width : options.line_width, color, false);
  }
  void point(const ir::Coord& c, const ir::Style& s) {
    if (pass != 2) return;
    Rgba color = options.point;
    if (auto parsed = parse_color(s.marker_color)) color = *parsed;
    dot(canvas, project(c.lon, c.lat), options.point_radius, color);
  }
  void geometry(const ir::Geometry& g, const ir::Style& s, int depth) {
    if (depth > ir::max_geometry_depth) return;
    std::visit(
        [&](const auto& shape) {
          using T = std::decay_t<decltype(shape)>;
          if constexpr (std::is_same_v<T, ir::Point>) {
            point(shape.coord, s);
          } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
            for (const auto& c : shape.points) point(c, s);
          } else if constexpr (std::is_same_v<T, ir::LineString>) {
            line(shape.points, s);
          } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
            for (const auto& l : shape.lines) line(l.points, s);
          } else if constexpr (std::is_same_v<T, ir::Polygon>) {
            polygon(shape, s);
          } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
            for (const auto& p : shape.polygons) polygon(p, s);
          } else {
            for (const auto& inner : shape.geometries) geometry(inner, s, depth + 1);
          }
        },
        g.shape);
  }
};

double nice_step(double range) {
  static constexpr double steps[] = {1e-5, 2e-5, 5e-5, 1e-4, 2e-4, 5e-4, 1e-3, 2e-3, 5e-3, 0.01, 0.02, 0.05,
                                     0.1,  0.2,  0.5,  1,    2,    5,    10,   15,   30,   45,   90};
  for (double s : steps) {
    if (range / s <= 8) return s;
  }
  return 90;
}

}  // namespace

Result<ir::Image> render_map(const ir::Geo& geo, const RenderOptions& options, const Context& ctx) {
  const BoundingBox box = bounding_box(geo);
  double min_lon = box.empty ? -180 : box.min_lon, max_lon = box.empty ? 180 : box.max_lon;
  double min_lat = box.empty ? -60 : box.min_lat, max_lat = box.empty ? 70 : box.max_lat;
  Projector project;
  project.mercator = min_lat >= -85 && max_lat <= 85;
  // Keep a minimal extent so that a single point still gets a sensible view.
  const double min_extent = 0.005;
  if (max_lon - min_lon < min_extent) {
    const double c = (min_lon + max_lon) / 2;
    min_lon = c - min_extent / 2;
    max_lon = c + min_extent / 2;
  }
  if (max_lat - min_lat < min_extent) {
    const double c = (min_lat + max_lat) / 2;
    min_lat = c - min_extent / 2;
    max_lat = c + min_extent / 2;
  }
  const double y_min = project.y_of(min_lat), y_max = project.y_of(max_lat);
  const double dx = max_lon - min_lon, dy = std::max(1e-12, y_max - y_min);
  const std::uint32_t width = std::clamp<std::uint32_t>(options.width, 16, 8192);
  std::uint32_t height = options.height;
  if (height == 0) {
    height = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(std::llround(static_cast<double>(width) * dy / dx), 64, 2 * std::int64_t{width}));
  }
  height = std::clamp<std::uint32_t>(height, 16, 8192);
  CVT_TRY(ctx.limits.check_pixels(width, height));
  const double margin = 0.06 * std::min(width, height) + 4;
  project.scale = std::min((width - 2 * margin) / dx, (height - 2 * margin) / dy);
  project.ox = (min_lon + max_lon) / 2 - (width / 2.0) / project.scale;
  project.oy = (y_min + y_max) / 2 - (height / 2.0) / project.scale;
  project.height = static_cast<int>(height);

  Canvas canvas;
  canvas.width = static_cast<int>(width);
  canvas.height = static_cast<int>(height);
  canvas.frame = ir::Frame::filled(width, height, options.background);
  if (options.graticule) {
    // Visible extent in degrees.
    const double vx0 = project.ox, vx1 = project.ox + width / project.scale;
    const double vy0 = project.oy, vy1 = project.oy + height / project.scale;
    const double lat0 = project.mercator ? std::atan(std::sinh(vy0 * std::numbers::pi / 180)) * 180 / std::numbers::pi : vy0;
    const double lat1 = project.mercator ? std::atan(std::sinh(vy1 * std::numbers::pi / 180)) * 180 / std::numbers::pi : vy1;
    const double step = nice_step(std::max(vx1 - vx0, lat1 - lat0));
    std::vector<std::vector<P>> lines;
    for (double lon = std::ceil(vx0 / step) * step; lon <= vx1 && lines.size() < 200; lon += step) {
      lines.push_back({project(lon, lat0), project(lon, lat1)});
    }
    for (double lat = std::ceil(lat0 / step) * step; lat <= lat1 && lines.size() < 400; lat += step) {
      lines.push_back({project(vx0, lat), project(vx1, lat)});
    }
    for (const auto& l : lines) stroke(canvas, {l}, 1.0, options.graticule_color, false);
  }
  Painter painter{canvas, project, options, 0};
  for (painter.pass = 0; painter.pass < 3; ++painter.pass) {
    std::size_t index = 0;
    for (const ir::Feature& f : geo.features) {
      if ((++index & 255) == 0) {
        CVT_TRY(ctx.checkpoint((painter.pass + static_cast<double>(index) / static_cast<double>(geo.features.size())) / 3));
      }
      painter.geometry(f.geometry, f.style, 0);
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(canvas.frame));
  return image;
}

}  // namespace cvt::geo
