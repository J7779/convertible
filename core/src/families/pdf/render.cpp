// Convertible core - pdf family: rendering pages to RGBA.
//
// A RenderDevice paints the interpreter's output onto 8-bit premultiplied
// RGBA canvases: anti-aliased fills and strokes through the in-house
// rasteriser, clipping masks, glyph outlines (cached per size and subpixel
// phase), images with masks, axial/radial/function/mesh shadings, tiling and
// shading patterns, transparency groups, soft masks and blend modes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <unordered_map>

#include "families/pdf/interp.hpp"
#include "families/pdf/pdf.hpp"
#include "families/pdf/pdf_image.hpp"
#include "families/pdf/syntax.hpp"

namespace cvt::pdf {

namespace {

using raster::IntRect;
using raster::Mask;

struct Canvas {
  int width = 0, height = 0;
  std::vector<std::uint8_t> px;  // premultiplied RGBA
  void allocate(int w, int h) {
    width = w;
    height = h;
    px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
  }
  [[nodiscard]] std::uint8_t* at(int x, int y) {
    return px.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
  }
};

struct SoftMaskBuffer final : DeviceSoftMask {
  int width = 0, height = 0;
  std::vector<std::uint8_t> values;
  [[nodiscard]] std::uint8_t at(int x, int y) const {
    return values[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
  }
};

inline std::uint8_t mul255(int a, int b) { return static_cast<std::uint8_t>((a * b + 127) / 255); }
inline std::uint8_t to8(double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// Separable and non-separable blend functions on 0..1 values.
double blend_channel(BlendMode mode, double b, double s) {
  switch (mode) {
    case BlendMode::Multiply: return b * s;
    case BlendMode::Screen: return b + s - b * s;
    case BlendMode::Overlay: return s <= 0.5 && b <= 0.5 ? 2 * s * b : (b <= 0.5 ? 2 * s * b : 1 - 2 * (1 - s) * (1 - b));
    case BlendMode::Darken: return std::min(b, s);
    case BlendMode::Lighten: return std::max(b, s);
    case BlendMode::ColorDodge: return b == 0 ? 0 : (s >= 1 ? 1 : std::min(1.0, b / (1 - s)));
    case BlendMode::ColorBurn: return b >= 1 ? 1 : (s <= 0 ? 0 : 1 - std::min(1.0, (1 - b) / s));
    case BlendMode::HardLight: return s <= 0.5 ? 2 * s * b : 1 - 2 * (1 - s) * (1 - b);
    case BlendMode::SoftLight: {
      if (s <= 0.5) return b - (1 - 2 * s) * b * (1 - b);
      const double d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
      return b + (2 * s - 1) * (d - b);
    }
    case BlendMode::Difference: return std::abs(b - s);
    case BlendMode::Exclusion: return b + s - 2 * b * s;
    default: return s;
  }
}

double lum(const double c[3]) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }
void clip_color(double c[3]) {
  const double l = lum(c);
  const double n = std::min({c[0], c[1], c[2]}), x = std::max({c[0], c[1], c[2]});
  for (int i = 0; i < 3; ++i) {
    if (n < 0 && l - n != 0) c[i] = l + (c[i] - l) * l / (l - n);
    if (x > 1 && x - l != 0) c[i] = l + (c[i] - l) * (1 - l) / (x - l);
  }
}
void set_lum(double c[3], double l) {
  const double d = l - lum(c);
  for (int i = 0; i < 3; ++i) c[i] += d;
  clip_color(c);
}
double sat(const double c[3]) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }
void set_sat(double c[3], double s) {
  int imax = 0, imin = 0;
  for (int i = 1; i < 3; ++i) {
    if (c[i] > c[imax]) imax = i;
    if (c[i] < c[imin]) imin = i;
  }
  if (imax == imin) {
    c[0] = c[1] = c[2] = 0;
    return;
  }
  const int imid = 3 - imax - imin;
  if (c[imax] > c[imin]) {
    c[imid] = (c[imid] - c[imin]) * s / (c[imax] - c[imin]);
    c[imax] = s;
  } else {
    c[imid] = c[imax] = 0;
  }
  c[imin] = 0;
}

void blend_rgb(BlendMode mode, const double b[3], const double s[3], double out[3]) {
  double t[3] = {s[0], s[1], s[2]};
  switch (mode) {
    case BlendMode::Hue: {
      set_sat(t, sat(b));
      set_lum(t, lum(b));
      break;
    }
    case BlendMode::Saturation: {
      t[0] = b[0];
      t[1] = b[1];
      t[2] = b[2];
      set_sat(t, sat(s));
      set_lum(t, lum(b));
      break;
    }
    case BlendMode::Color: set_lum(t, lum(b)); break;
    case BlendMode::Luminosity:
      t[0] = b[0];
      t[1] = b[1];
      t[2] = b[2];
      set_lum(t, lum(s));
      break;
    default:
      for (int i = 0; i < 3; ++i) t[i] = blend_channel(mode, b[i], s[i]);
      break;
  }
  for (int i = 0; i < 3; ++i) out[i] = t[i];
}

// ---------------------------------------------------------------- pixel sources

/// Produces straight RGBA for a horizontal run of device pixels.
class Source {
 public:
  virtual ~Source() = default;
  virtual void row(int y, int x0, int x1, std::uint8_t* rgba) = 0;
  [[nodiscard]] virtual bool solid() const { return false; }
};

class SolidSource final : public Source {
 public:
  std::array<std::uint8_t, 4> color{0, 0, 0, 255};
  void row(int, int x0, int x1, std::uint8_t* rgba) override {
    for (int x = x0; x < x1; ++x, rgba += 4) std::copy(color.begin(), color.end(), rgba);
  }
  [[nodiscard]] bool solid() const override { return true; }
};

/// Shading colour evaluation (types 1-3 per pixel; mesh types are drawn separately).
class ShadingSource final : public Source {
 public:
  int type = 2;
  Matrix inverse;  // device -> shading space
  std::array<double, 6> coords{};
  std::array<double, 2> domain{0, 1};
  std::array<bool, 2> extend{false, false};
  std::vector<std::array<std::uint8_t, 4>> lut;  // over t
  // type 1
  std::shared_ptr<const Function> function;
  std::shared_ptr<const ColorSpace> space;
  std::array<double, 4> domain2{0, 1, 0, 1};
  bool has_background = false;
  std::array<std::uint8_t, 4> background{0, 0, 0, 0};

  [[nodiscard]] std::array<std::uint8_t, 4> color_at(double t) const {
    if (lut.empty()) return {0, 0, 0, 0};
    const double u = (t - domain[0]) / (domain[1] - domain[0]);
    const auto i = static_cast<std::size_t>(std::clamp(u, 0.0, 1.0) * static_cast<double>(lut.size() - 1) + 0.5);
    return lut[std::min(i, lut.size() - 1)];
  }

  void row(int y, int x0, int x1, std::uint8_t* rgba) override {
    for (int x = x0; x < x1; ++x, rgba += 4) {
      const Point p = inverse.apply(Point{x + 0.5, y + 0.5});
      std::array<std::uint8_t, 4> c{0, 0, 0, 0};
      if (type == 1) {
        if (p.x >= domain2[0] && p.x <= domain2[1] && p.y >= domain2[2] && p.y <= domain2[3] && function && space) {
          const double in[2] = {p.x, p.y};
          double out[32] = {};
          function->eval(in, out);
          const Rgb v = space->to_rgb(out);
          c = {to8(v.r), to8(v.g), to8(v.b), 255};
        } else if (has_background) {
          c = background;
        }
      } else if (type == 2) {
        const double dx = coords[2] - coords[0], dy = coords[3] - coords[1];
        const double denom = dx * dx + dy * dy;
        double s = denom > 0 ? ((p.x - coords[0]) * dx + (p.y - coords[1]) * dy) / denom : 0;
        bool inside = true;
        if (s < 0) {
          if (extend[0]) s = 0;
          else inside = false;
        } else if (s > 1) {
          if (extend[1]) s = 1;
          else inside = false;
        }
        if (inside) c = color_at(domain[0] + s * (domain[1] - domain[0]));
        else if (has_background) c = background;
      } else {
        // Radial: find the largest s with point on circle(s).
        const double x0 = coords[0], y0 = coords[1], r0 = coords[2], x1 = coords[3], y1 = coords[4], r1 = coords[5];
        const double cdx = x1 - x0, cdy = y1 - y0, dr = r1 - r0;
        const double pdx = p.x - x0, pdy = p.y - y0;
        const double a = cdx * cdx + cdy * cdy - dr * dr;
        const double b = pdx * cdx + pdy * cdy + r0 * dr;
        const double cc = pdx * pdx + pdy * pdy - r0 * r0;
        double s = -1;
        bool found = false;
        const auto accept = [&](double cand) {
          if (r0 + cand * dr < 0) return false;
          if (cand < 0 && !extend[0]) return false;
          if (cand > 1 && !extend[1]) return false;
          return true;
        };
        if (std::abs(a) < 1e-12) {
          if (std::abs(b) > 1e-12) {
            const double cand = cc / (2 * b);
            if (accept(cand)) {
              s = cand;
              found = true;
            }
          }
        } else {
          const double disc = b * b - a * cc;
          if (disc >= 0) {
            const double root = std::sqrt(disc);
            const double s1 = (b + root) / a, s2 = (b - root) / a;
            const double hi = std::max(s1, s2), lo = std::min(s1, s2);
            if (accept(hi)) {
              s = hi;
              found = true;
            } else if (accept(lo)) {
              s = lo;
              found = true;
            }
          }
        }
        if (found) c = color_at(domain[0] + std::clamp(s, 0.0, 1.0) * (domain[1] - domain[0]));
        else if (has_background) c = background;
      }
      std::copy(c.begin(), c.end(), rgba);
    }
  }
};

/// A pre-rendered tile repeated over device space (tiling patterns).
class TileSource final : public Source {
 public:
  Canvas tile;             // premultiplied
  Matrix device_to_tile;   // device pixel -> tile pixel (before wrapping)
  bool uncolored = false;
  std::array<std::uint8_t, 3> color{0, 0, 0};
  void row(int y, int x0, int x1, std::uint8_t* rgba) override {
    for (int x = x0; x < x1; ++x, rgba += 4) {
      const Point p = device_to_tile.apply(Point{x + 0.5, y + 0.5});
      auto tx = static_cast<long long>(std::floor(p.x));
      auto ty = static_cast<long long>(std::floor(p.y));
      tx %= tile.width;
      ty %= tile.height;
      if (tx < 0) tx += tile.width;
      if (ty < 0) ty += tile.height;
      const std::uint8_t* s = tile.at(static_cast<int>(tx), static_cast<int>(ty));
      const int a = s[3];
      if (uncolored) {
        rgba[0] = color[0];
        rgba[1] = color[1];
        rgba[2] = color[2];
      } else if (a > 0) {
        for (int c = 0; c < 3; ++c) rgba[c] = static_cast<std::uint8_t>(std::min(255, s[c] * 255 / a));
      } else {
        rgba[0] = rgba[1] = rgba[2] = 0;
      }
      rgba[3] = static_cast<std::uint8_t>(a);
    }
  }
};

/// An image mapped by the CTM (unit square -> device).
class ImageSource final : public Source {
 public:
  const DecodedImage* image = nullptr;
  Matrix device_to_image;  // device -> image pixel space (u right, v down)
  bool smooth = false;
  bool stencil = false;
  std::array<std::uint8_t, 3> stencil_color{0, 0, 0};
  int samples = 1;  // supersampling per axis when minifying
  void row(int y, int x0, int x1, std::uint8_t* rgba) override {
    const int w = static_cast<int>(image->width), h = static_cast<int>(image->height);
    const auto fetch = [&](double u, double v, int out[4]) {
      int iu = static_cast<int>(std::floor(u)), iv = static_cast<int>(std::floor(v));
      iu = std::clamp(iu, 0, w - 1);
      iv = std::clamp(iv, 0, h - 1);
      const std::uint8_t* s = image->rgba.data() + (static_cast<std::size_t>(iv) * static_cast<std::size_t>(w) + static_cast<std::size_t>(iu)) * 4;
      for (int c = 0; c < 4; ++c) out[c] = s[c];
    };
    for (int x = x0; x < x1; ++x, rgba += 4) {
      int acc[4] = {0, 0, 0, 0};
      int n = 0;
      if (smooth && samples == 1) {
        const Point p = device_to_image.apply(Point{x + 0.5, y + 0.5});
        const double u = p.x - 0.5, v = p.y - 0.5;
        const double fu = u - std::floor(u), fv = v - std::floor(v);
        int a[4], b[4], c[4], d[4];
        fetch(std::floor(u), std::floor(v), a);
        fetch(std::floor(u) + 1, std::floor(v), b);
        fetch(std::floor(u), std::floor(v) + 1, c);
        fetch(std::floor(u) + 1, std::floor(v) + 1, d);
        // Interpolate premultiplied to avoid dark fringes.
        double out[4] = {0, 0, 0, 0};
        const double wts[4] = {(1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv};
        const int* px[4] = {a, b, c, d};
        for (int k = 0; k < 4; ++k) {
          const double alpha = px[k][3] / 255.0;
          for (int ch = 0; ch < 3; ++ch) out[ch] += wts[k] * px[k][ch] * alpha;
          out[3] += wts[k] * px[k][3];
        }
        for (int ch = 0; ch < 3; ++ch) acc[ch] = out[3] > 0 ? static_cast<int>(std::lround(out[ch] * 255.0 / out[3])) : 0;
        acc[3] = static_cast<int>(std::lround(out[3]));
        n = 1;
      } else {
        for (int sy = 0; sy < samples; ++sy) {
          for (int sx = 0; sx < samples; ++sx) {
            const Point p = device_to_image.apply(Point{x + (sx + 0.5) / samples, y + (sy + 0.5) / samples});
            int v[4];
            fetch(p.x, p.y, v);
            const int alpha = v[3];
            for (int ch = 0; ch < 3; ++ch) acc[ch] += v[ch] * alpha;
            acc[3] += alpha;
            ++n;
          }
        }
        if (acc[3] > 0) {
          for (int ch = 0; ch < 3; ++ch) acc[ch] = acc[ch] / acc[3];
        } else {
          acc[0] = acc[1] = acc[2] = 0;
        }
        acc[3] /= n;
      }
      if (stencil) {
        rgba[0] = stencil_color[0];
        rgba[1] = stencil_color[1];
        rgba[2] = stencil_color[2];
      } else {
        for (int ch = 0; ch < 3; ++ch) rgba[ch] = static_cast<std::uint8_t>(std::clamp(acc[ch], 0, 255));
      }
      rgba[3] = static_cast<std::uint8_t>(std::clamp(acc[3], 0, 255));
    }
  }
};

// ---------------------------------------------------------------- device

struct GlyphKey {
  const void* font;
  std::uint64_t code;
  std::int32_t a, b, c, d;  // matrix * 1024
  std::uint8_t fx, fy;      // subpixel phase (quarters)
  bool operator==(const GlyphKey&) const = default;
};
struct GlyphKeyHash {
  std::size_t operator()(const GlyphKey& k) const noexcept {
    std::size_t h = std::hash<const void*>()(k.font);
    const auto mix = [&](std::uint64_t v) { h ^= std::hash<std::uint64_t>()(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(k.code);
    mix(static_cast<std::uint32_t>(k.a));
    mix(static_cast<std::uint32_t>(k.b));
    mix(static_cast<std::uint32_t>(k.c));
    mix(static_cast<std::uint32_t>(k.d));
    mix(static_cast<std::uint64_t>(k.fx) << 8 | k.fy);
    return h;
  }
};

class RenderDevice final : public Device {
 public:
  RenderDevice(PdfFile& file, const Context& ctx, int width, int height, const Matrix& base, bool antialias)
      : file_(file), ctx_(ctx), base_(base), antialias_(antialias) {
    layers_.emplace_back();
    layers_.back().canvas.allocate(width, height);
    clip_stack_.push_back(std::make_shared<Mask>(Mask::full()));
    box_ = IntRect{0, 0, width, height};
  }

  Canvas& canvas() { return layers_.front().canvas; }
  Interpreter* interp = nullptr;

  void save() override { clip_stack_.push_back(clip_stack_.back()); }
  void restore() override {
    if (clip_stack_.size() > 1) clip_stack_.pop_back();
  }

  void fill(const Path& path, raster::FillRule rule, const GState& gs) override {
    const Path device = path.transformed(gs.ctm);
    const Mask mask = raster::fill_path(device, rule, box_, antialias_);
    paint_mask(mask, gs, true);
  }
  void stroke(const Path& path, const GState& gs) override {
    const Path outline = raster::stroke_outline(path, gs.stroke, gs.ctm);
    const Mask mask = raster::fill_path(outline, raster::FillRule::NonZero, box_, antialias_);
    // Lines thinner than a pixel are drawn one pixel wide with proportionally
    // less ink, which keeps their visual weight right.
    const double device_width = gs.stroke.width * gs.ctm.expansion();
    if (gs.stroke.width > 0 && device_width < 1.0 && antialias_) {
      GState light = gs;
      light.stroke_alpha = gs.stroke_alpha * std::max(device_width, 0.25);
      paint_mask(mask, light, false);
      return;
    }
    paint_mask(mask, gs, false);
  }
  void clip(const Path& path, raster::FillRule rule, const GState& gs) override {
    const Path device = path.transformed(gs.ctm);
    // Axis-aligned rectangles are clipped without anti-aliasing (crisp edges).
    const Mask mask = raster::fill_path(device, rule, box_, antialias_);
    auto next = std::make_shared<Mask>(raster::intersect(*clip_stack_.back(), mask));
    clip_stack_.back() = std::move(next);
  }

  void glyph(const GlyphEvent& ev, const GState& gs) override {
    const int mode = gs.render_mode;
    if (ev.font == nullptr || ev.font->kind() == PdfFont::Kind::Type3) return;
    const Path* outline = ev.font->outline(ev.code);
    if (outline == nullptr) return;
    const bool fill_glyph = mode == 0 || mode == 2 || mode == 4 || mode == 6;
    const bool stroke_glyph = mode == 1 || mode == 2 || mode == 5 || mode == 6;
    const bool clip_glyph = mode >= 4;
    if (clip_glyph) text_clip_.append(*outline, ev.trm);
    if (fill_glyph) {
      const Mask* mask = glyph_mask(ev, *outline);
      if (mask != nullptr) {
        // Glyph masks are cached relative to an integer origin.
        const Point o = ev.trm.apply(Point{0, 0});
        const int ox = static_cast<int>(std::floor(o.x)), oy = static_cast<int>(std::floor(o.y));
        Mask placed;
        placed.box = IntRect{mask->box.x0 + ox, mask->box.y0 + oy, mask->box.x1 + ox, mask->box.y1 + oy};
        const IntRect visible = placed.box.intersect(box_);
        if (!visible.empty()) {
          placed.box = visible;
          placed.coverage.resize(static_cast<std::size_t>(visible.width()) * static_cast<std::size_t>(visible.height()));
          for (int y = visible.y0; y < visible.y1; ++y) {
            const std::uint8_t* src = mask->row(y - oy) + (visible.x0 - ox - mask->box.x0);
            std::copy(src, src + visible.width(),
                      placed.coverage.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y - visible.y0) * static_cast<std::size_t>(visible.width())));
          }
          paint_mask(placed, gs, true);
        }
      }
    }
    if (stroke_glyph) {
      GState sgs = gs;
      sgs.ctm = ev.trm;
      // Line width is in user space: convert to glyph space.
      const Matrix glyph_to_user = Matrix::concat(ev.trm, gs.ctm.inverted());
      sgs.stroke.width = gs.stroke.width / std::max(glyph_to_user.expansion(), 1e-9);
      stroke(*outline, sgs);
    }
  }
  void end_text(const GState& gs) override {
    if (text_clip_.empty()) {
      return;
    }
    GState identity = gs;
    identity.ctm = Matrix{};
    clip(text_clip_, raster::FillRule::NonZero, identity);
    text_clip_.clear();
  }

  void image(const ImageEvent& ev, const GState& gs) override {
    auto decoded = decode_image(file_, ev, &ctx_);
    if (!decoded) {
      if (decoded.error().code == Status::UnsupportedConversion) {
        ctx_.warn("An embedded JPEG/JPEG 2000 image could not be decoded (no decoder available); it was left blank.");
      }
      return;
    }
    const DecodedImage& img = *decoded;
    const Path unit = [] {
      Path p;
      p.rect(0, 0, 1, 1);
      return p;
    }();
    const Mask area = raster::fill_path(unit.transformed(gs.ctm), raster::FillRule::NonZero, box_, antialias_);
    if (area.empty()) return;
    // unit square: (0,0) bottom-left of the image; image rows go top-down.
    const Matrix to_unit = gs.ctm.inverted();
    const Matrix unit_to_pixels{static_cast<double>(img.width), 0, 0, -static_cast<double>(img.height), 0,
                                static_cast<double>(img.height)};
    ImageSource src;
    src.image = &img;
    src.device_to_image = Matrix::concat(to_unit, unit_to_pixels);
    // Minification: supersample.
    const double scale = std::max(img.width, img.height) / std::max(1.0, gs.ctm.max_expansion());
    src.samples = std::clamp(static_cast<int>(std::ceil(scale)), 1, 4);
    src.smooth = src.samples == 1 && (img.interpolate || scale < 0.5) && !img.stencil;
    src.stencil = img.stencil;
    if (img.stencil) {
      if (gs.fill_space && gs.fill_space->kind() == ColorSpace::Kind::Pattern && !gs.fill_pattern.is_null()) {
        // Stencil filled with a pattern: build a mask from the stencil and paint the pattern through it.
        Mask m = area;
        apply_source_alpha(m, src);
        paint_mask(m, gs, true);
        return;
      }
      const Rgb c = color_of(gs, true);
      src.stencil_color = {to8(c.r), to8(c.g), to8(c.b)};
    }
    composite(area, src, gs.fill_alpha, gs);
  }

  void shading(const Object& shading_ref, const GState& gs) override {
    const Object shading = file_.resolve(shading_ref);
    const Mask& clip = *clip_stack_.back();
    Mask region = clip.everywhere ? full_mask() : clip;
    draw_shading(shading, gs.ctm, region, gs, false);
  }

  bool begin_group(const GroupInfo& group, const GState& gs) override {
    if (layers_.size() > 16) return false;
    // Only groups that need compositing get their own layer.
    if (gs.fill_alpha >= 1 && gs.blend == BlendMode::Normal && !gs.soft_mask && !group.knockout) return false;
    Layer layer;
    layer.canvas.allocate(canvas().width, canvas().height);
    layer.alpha = gs.fill_alpha;
    layer.blend = gs.blend;
    layer.soft_mask = gs.soft_mask;
    layers_.push_back(std::move(layer));
    return true;
  }
  void end_group(const GState&) override {
    if (layers_.size() < 2) return;
    Layer layer = std::move(layers_.back());
    layers_.pop_back();
    Canvas& dst = layers_.back().canvas;
    const auto* smask = dynamic_cast<const SoftMaskBuffer*>(layer.soft_mask.get());
    const int alpha = static_cast<int>(std::lround(std::clamp(layer.alpha, 0.0, 1.0) * 255));
    for (int y = 0; y < dst.height; ++y) {
      for (int x = 0; x < dst.width; ++x) {
        const std::uint8_t* s = layer.canvas.at(x, y);
        if (s[3] == 0) continue;
        int a = alpha;
        if (smask != nullptr) a = mul255(a, smask->at(x, y));
        if (a == 0) continue;
        const std::uint8_t straight[4] = {
            static_cast<std::uint8_t>(s[3] ? std::min(255, s[0] * 255 / s[3]) : 0),
            static_cast<std::uint8_t>(s[3] ? std::min(255, s[1] * 255 / s[3]) : 0),
            static_cast<std::uint8_t>(s[3] ? std::min(255, s[2] * 255 / s[3]) : 0), s[3]};
        blend_pixel(dst.at(x, y), straight, mul255(s[3], a), layer.blend);
      }
    }
  }

  std::shared_ptr<const DeviceSoftMask> make_soft_mask(const Dict& smask, const GState& gs) override {
    if (depth_ > 4) return nullptr;
    const Object group = file_.get(smask, "G");
    if (!group.is_stream()) return nullptr;
    const bool luminosity = !file_.get(smask, "S").is_name("Alpha");
    // Backdrop colour (luminosity masks).
    std::array<double, 3> backdrop{0, 0, 0};
    const Array& bc = file_.get(smask, "BC").as_array();
    const Dict& gdict = file_.get(group.as_dict(), "Group").as_dict();
    auto space = load_color_space(file_, gdict.get("CS"), nullptr);
    if (!bc.empty() && space) {
      std::vector<double> comps;
      for (const Object& o : bc) comps.push_back(file_.resolve(o).as_number());
      comps.resize(static_cast<std::size_t>(std::max(1, space->components())), 0.0);
      const Rgb v = space->to_rgb(comps.data());
      backdrop = {v.r, v.g, v.b};
    }
    const auto transfer = load_function(file_, file_.get(smask, "TR"));
    // Render the mask group on its own canvas.
    Layer layer;
    layer.canvas.allocate(canvas().width, canvas().height);
    layers_.push_back(std::move(layer));
    auto saved_clip = clip_stack_;
    clip_stack_.assign(1, std::make_shared<Mask>(Mask::full()));
    ++depth_;
    if (interp != nullptr) {
      GState mgs;
      mgs.ctm = gs.ctm;
      interp->run_form(group, mgs, 1, Object{});
    }
    --depth_;
    clip_stack_ = std::move(saved_clip);
    Layer rendered = std::move(layers_.back());
    layers_.pop_back();
    auto buffer = std::make_shared<SoftMaskBuffer>();
    buffer->width = rendered.canvas.width;
    buffer->height = rendered.canvas.height;
    buffer->values.resize(static_cast<std::size_t>(buffer->width) * static_cast<std::size_t>(buffer->height));
    std::array<std::uint8_t, 256> tr{};
    for (int i = 0; i < 256; ++i) {
      double v = i / 255.0;
      if (transfer) {
        double out[32] = {};
        transfer->eval(&v, out);
        v = out[0];
      }
      tr[static_cast<std::size_t>(i)] = to8(v);
    }
    for (int y = 0; y < buffer->height; ++y) {
      for (int x = 0; x < buffer->width; ++x) {
        const std::uint8_t* p = rendered.canvas.at(x, y);
        std::uint8_t value = 0;
        if (luminosity) {
          const double a = p[3] / 255.0;
          const double r = p[0] / 255.0 + backdrop[0] * (1 - a), g = p[1] / 255.0 + backdrop[1] * (1 - a),
                       b = p[2] / 255.0 + backdrop[2] * (1 - a);
          value = to8(0.3 * r + 0.59 * g + 0.11 * b);
        } else {
          value = p[3];
        }
        buffer->values[static_cast<std::size_t>(y) * static_cast<std::size_t>(buffer->width) + static_cast<std::size_t>(x)] = tr[value];
      }
    }
    return buffer;
  }

 private:
  struct Layer {
    Canvas canvas;
    double alpha = 1;
    BlendMode blend = BlendMode::Normal;
    std::shared_ptr<const DeviceSoftMask> soft_mask;
  };

  Mask full_mask() const {
    Mask m;
    m.box = box_;
    m.coverage.assign(static_cast<std::size_t>(box_.width()) * static_cast<std::size_t>(box_.height()), 255);
    return m;
  }

  static void blend_pixel(std::uint8_t* d, const std::uint8_t* s, int sa, BlendMode mode) {
    if (sa <= 0) return;
    if (mode == BlendMode::Normal || d[3] == 0) {
      const int inv = 255 - sa;
      for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>(mul255(s[c], sa) + mul255(d[c], inv));
      d[3] = static_cast<std::uint8_t>(sa + mul255(d[3], inv));
      return;
    }
    const double ab = d[3] / 255.0, as = sa / 255.0;
    double cb[3], cs[3], bl[3];
    for (int c = 0; c < 3; ++c) {
      cb[c] = ab > 0 ? d[c] / 255.0 / ab : 0;
      cs[c] = s[c] / 255.0;
    }
    blend_rgb(mode, cb, cs, bl);
    const double ar = as + ab - as * ab;
    for (int c = 0; c < 3; ++c) {
      const double mixed = (1 - ab) * cs[c] + ab * bl[c];
      const double premult = as * mixed + (1 - as) * ab * cb[c];
      d[c] = to8(premult);
    }
    d[3] = to8(ar);
  }

  Rgb color_of(const GState& gs, bool fill) const {
    const auto& space = fill ? gs.fill_space : gs.stroke_space;
    const auto& color = fill ? gs.fill_color : gs.stroke_color;
    if (!space) return {0, 0, 0};
    std::vector<double> comps = color;
    comps.resize(static_cast<std::size_t>(std::max(space->components(), 1)), 0.0);
    return space->to_rgb(comps.data());
  }

  void apply_source_alpha(Mask& m, Source& src) {
    std::vector<std::uint8_t> row(static_cast<std::size_t>(m.box.width()) * 4);
    for (int y = m.box.y0; y < m.box.y1; ++y) {
      src.row(y, m.box.x0, m.box.x1, row.data());
      std::uint8_t* cov = m.coverage.data() + static_cast<std::size_t>(y - m.box.y0) * static_cast<std::size_t>(m.box.width());
      for (int x = 0; x < m.box.width(); ++x) cov[x] = mul255(cov[x], row[static_cast<std::size_t>(x) * 4 + 3]);
    }
  }

  /// Composite `src` through `mask` x clip x soft mask with constant alpha.
  [[nodiscard]] bool exhausted() const override { return work_ > work_budget_; }

  void composite(const Mask& mask, Source& src, double alpha, const GState& gs) {
    if (mask.empty()) return;
    work_ += static_cast<std::uint64_t>(mask.everywhere ? box_.width() * box_.height() : mask.box.width() * mask.box.height());
    const Mask& clip = *clip_stack_.back();
    IntRect box = mask.everywhere ? box_ : mask.box;
    if (!clip.everywhere) box = box.intersect(clip.box);
    box = box.intersect(box_);
    if (box.empty()) return;
    Canvas& dst = layers_.back().canvas;
    const auto* smask = dynamic_cast<const SoftMaskBuffer*>(gs.soft_mask.get());
    const int ca = static_cast<int>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255));
    if (ca == 0) return;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(box.width()) * 4);
    const bool solid = src.solid();
    if (solid) src.row(box.y0, box.x0, box.x1, row.data());
    for (int y = box.y0; y < box.y1; ++y) {
      if (!solid) src.row(y, box.x0, box.x1, row.data());
      for (int x = box.x0; x < box.x1; ++x) {
        int cov = mask.at(x, y);
        if (cov == 0) continue;
        if (!clip.everywhere) {
          cov = mul255(cov, clip.at(x, y));
          if (cov == 0) continue;
        }
        if (smask != nullptr) cov = mul255(cov, smask->at(x, y));
        const std::uint8_t* s = row.data() + static_cast<std::size_t>(x - box.x0) * 4;
        const int sa = mul255(mul255(cov, ca), s[3]);
        blend_pixel(dst.at(x, y), s, sa, gs.blend);
      }
    }
  }

  void paint_mask(const Mask& mask, const GState& gs, bool fill) {
    if (mask.empty()) return;
    const double alpha = fill ? gs.fill_alpha : gs.stroke_alpha;
    const auto& space = fill ? gs.fill_space : gs.stroke_space;
    if (space && space->kind() == ColorSpace::Kind::Pattern) {
      const Object& pattern = fill ? gs.fill_pattern : gs.stroke_pattern;
      paint_pattern(mask, pattern, gs, fill, alpha);
      return;
    }
    SolidSource src;
    const Rgb c = color_of(gs, fill);
    src.color = {to8(c.r), to8(c.g), to8(c.b), 255};
    composite(mask, src, alpha, gs);
  }

  void paint_pattern(const Mask& mask, const Object& pattern_ref, const GState& gs, bool fill, double alpha) {
    const Object pattern = file_.resolve(pattern_ref);
    const Dict& pd = pattern.as_dict();
    const int type = static_cast<int>(file_.get(pd, "PatternType").as_int(1));
    Matrix pm;
    const Array& m = file_.get(pd, "Matrix").as_array();
    if (m.size() == 6) {
      pm = Matrix{file_.resolve(m[0]).as_number(1), file_.resolve(m[1]).as_number(), file_.resolve(m[2]).as_number(),
                  file_.resolve(m[3]).as_number(1), file_.resolve(m[4]).as_number(), file_.resolve(m[5]).as_number()};
    }
    const Matrix pattern_to_device = Matrix::concat(pm, pattern_base_);
    if (type == 2) {
      draw_shading(file_.get(pd, "Shading"), pattern_to_device, mask, gs, true, alpha);
      return;
    }
    if (!pattern.is_stream() || depth_ > 3) return;
    // Tiling pattern: render one cell at device resolution.
    const Array& bb = file_.get(pd, "BBox").as_array();
    if (bb.size() != 4) return;
    const Rect cell = Rect{file_.resolve(bb[0]).as_number(), file_.resolve(bb[1]).as_number(),
                           file_.resolve(bb[2]).as_number(), file_.resolve(bb[3]).as_number()}
                          .normalized();
    double xstep = std::abs(file_.get(pd, "XStep").as_number(cell.width()));
    double ystep = std::abs(file_.get(pd, "YStep").as_number(cell.height()));
    if (!(xstep > 0) || !(ystep > 0)) return;
    // Device size of one step.
    const Point vx = pattern_to_device.apply_vector(Point{xstep, 0});
    const Point vy = pattern_to_device.apply_vector(Point{0, ystep});
    const double sx = std::hypot(vx.x, vx.y), sy = std::hypot(vy.x, vy.y);
    if (sx < 0.25 || sy < 0.25) {
      // Sub-pixel tiles: approximate by the average colour (pattern too fine to resolve).
      SolidSource src;
      const Rgb c = color_of(gs, fill);
      src.color = {to8(c.r), to8(c.g), to8(c.b), 128};
      composite(mask, src, alpha, gs);
      return;
    }
    const int tw = static_cast<int>(std::clamp(std::ceil(sx), 1.0, 2048.0));
    const int th = static_cast<int>(std::clamp(std::ceil(sy), 1.0, 2048.0));
    // Tile space: pixel (i, j) of the tile; tile -> pattern space.
    const Matrix tile_to_pattern{xstep / tw, 0, 0, -ystep / th, cell.x0, cell.y0 + ystep};
    const Matrix pattern_to_tile = tile_to_pattern.inverted();
    auto src = std::make_unique<TileSource>();
    src->tile.allocate(tw, th);
    src->uncolored = file_.get(pd, "PaintType").as_int(1) == 2;
    if (src->uncolored) {
      const Rgb c = color_of_pattern_base(gs, fill);
      src->color = {to8(c.r), to8(c.g), to8(c.b)};
    }
    // Render the cell content (with neighbours wrapping) into the tile.
    {
      Layer layer;
      layer.canvas.allocate(tw, th);
      layers_.push_back(std::move(layer));
      const IntRect saved_box = box_;
      auto saved_clip = clip_stack_;
      box_ = IntRect{0, 0, tw, th};
      clip_stack_.assign(1, std::make_shared<Mask>(Mask::full()));
      ++depth_;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          GState pgs;
          pgs.ctm = Matrix::concat(Matrix::translate(dx * xstep, dy * ystep), pattern_to_tile);
          if (src->uncolored) {
            pgs.fill_space = device_gray();
            pgs.stroke_space = device_gray();
          }
          if (interp != nullptr) interp->run_form(pattern, pgs, 1, Object{});
          if (cell.width() <= xstep && cell.height() <= ystep) {
            dx = dy = 2;  // the cell fits in one step: no neighbours needed
          }
        }
      }
      --depth_;
      box_ = saved_box;
      clip_stack_ = std::move(saved_clip);
      src->tile = std::move(layers_.back().canvas);
      layers_.pop_back();
    }
    src->device_to_tile = Matrix::concat(pattern_to_device.inverted(), pattern_to_tile);
    composite(mask, *src, alpha, gs);
  }

  Rgb color_of_pattern_base(const GState& gs, bool fill) const {
    const auto& space = fill ? gs.fill_space : gs.stroke_space;
    const auto& color = fill ? gs.fill_color : gs.stroke_color;
    const auto base = space ? space->base() : nullptr;
    if (!base) return {0, 0, 0};
    std::vector<double> comps = color;
    comps.resize(static_cast<std::size_t>(std::max(base->components(), 1)), 0.0);
    return base->to_rgb(comps.data());
  }

  void draw_shading(const Object& shading_ref, const Matrix& to_device, const Mask& region, const GState& gs,
                    bool as_pattern, double alpha = 1.0) {
    const Object shading = file_.resolve(shading_ref);
    const Dict& sd = shading.as_dict();
    const int type = static_cast<int>(file_.get(sd, "ShadingType").as_int(0));
    auto space = load_color_space(file_, sd.get("ColorSpace"), nullptr);
    if (!space) space = device_gray();
    const auto function = load_function(file_, sd.get("Function"));
    std::array<std::uint8_t, 4> background{0, 0, 0, 0};
    bool has_background = false;
    const Array& bg = file_.get(sd, "Background").as_array();
    if (as_pattern && !bg.empty()) {
      std::vector<double> comps;
      for (const Object& o : bg) comps.push_back(file_.resolve(o).as_number());
      comps.resize(static_cast<std::size_t>(std::max(1, space->components())), 0.0);
      const Rgb v = space->to_rgb(comps.data());
      background = {to8(v.r), to8(v.g), to8(v.b), 255};
      has_background = true;
    }
    if (type >= 1 && type <= 3) {
      ShadingSource src;
      src.type = type;
      src.inverse = to_device.inverted();
      src.function = function;
      src.space = space;
      src.has_background = has_background;
      src.background = background;
      const auto nums = [&](std::string_view key) {
        std::vector<double> v;
        for (const Object& o : file_.get(sd, key).as_array()) v.push_back(file_.resolve(o).as_number());
        return v;
      };
      if (type == 1) {
        const auto d = nums("Domain");
        if (d.size() == 4) src.domain2 = {d[0], d[1], d[2], d[3]};
        const auto mm = nums("Matrix");
        if (mm.size() == 6) src.inverse = Matrix::concat(to_device.inverted(), Matrix{mm[0], mm[1], mm[2], mm[3], mm[4], mm[5]}.inverted());
      } else {
        const auto c = nums("Coords");
        if (c.size() < (type == 2 ? 4u : 6u)) return;
        for (std::size_t i = 0; i < c.size() && i < 6; ++i) src.coords[i] = c[i];
        const auto d = nums("Domain");
        if (d.size() == 2) src.domain = {d[0], d[1]};
        const Array& ext = file_.get(sd, "Extend").as_array();
        if (ext.size() == 2) src.extend = {file_.resolve(ext[0]).as_bool(), file_.resolve(ext[1]).as_bool()};
        if (!function) return;
        src.lut.resize(1024);
        for (std::size_t i = 0; i < src.lut.size(); ++i) {
          const double t = src.domain[0] + (src.domain[1] - src.domain[0]) * static_cast<double>(i) / 1023.0;
          double out[32] = {};
          function->eval(&t, out);
          const Rgb v = space->to_rgb(out);
          src.lut[i] = {to8(v.r), to8(v.g), to8(v.b), 255};
        }
      }
      composite(region, src, as_pattern ? alpha : gs.fill_alpha, gs);
      return;
    }
    if (type >= 4 && type <= 7 && shading.is_stream()) {
      draw_mesh(shading, sd, type, space, function, to_device, region, gs, as_pattern ? alpha : gs.fill_alpha);
    }
  }

  struct MeshVertex {
    Point p;
    std::array<double, 32> c{};
  };

  void draw_mesh(const Object& shading, const Dict& sd, int type, const std::shared_ptr<const ColorSpace>& space,
                 const std::shared_ptr<const Function>& function, const Matrix& to_device, const Mask& region,
                 const GState& gs, double alpha) {
    const Bytes data = file_.decode_or_empty(shading);
    const int bpcoord = static_cast<int>(file_.get(sd, "BitsPerCoordinate").as_int(8));
    const int bpcomp = static_cast<int>(file_.get(sd, "BitsPerComponent").as_int(8));
    const int bpflag = static_cast<int>(file_.get(sd, "BitsPerFlag").as_int(8));
    const int per_row = static_cast<int>(file_.get(sd, "VerticesPerRow").as_int(2));
    std::vector<double> decode;
    for (const Object& o : file_.get(sd, "Decode").as_array()) decode.push_back(file_.resolve(o).as_number());
    const int ncomp = function ? 1 : space->components();
    if (decode.size() < static_cast<std::size_t>(4 + 2 * ncomp) || bpcoord < 1 || bpcoord > 32 || bpcomp < 1 ||
        bpcomp > 16 || ncomp > 32) {
      return;
    }
    std::uint64_t bit = 0;
    const std::uint64_t total_bits = static_cast<std::uint64_t>(data.size()) * 8;
    const auto read = [&](int bits) -> std::uint64_t {
      std::uint64_t v = 0;
      for (int i = 0; i < bits; ++i, ++bit) {
        const std::size_t byte = static_cast<std::size_t>(bit / 8);
        const int b = byte < data.size() ? std::to_integer<int>(data[byte]) : 0;
        v = (v << 1) | static_cast<std::uint64_t>((b >> (7 - bit % 8)) & 1);
      }
      return v;
    };
    const auto align = [&]() { bit = (bit + 7) / 8 * 8; };
    const double cmax = std::pow(2.0, bpcoord) - 1, kmax = std::pow(2.0, bpcomp) - 1;
    const auto read_point = [&]() {
      const double x = decode[0] + static_cast<double>(read(bpcoord)) * (decode[1] - decode[0]) / cmax;
      const double y = decode[2] + static_cast<double>(read(bpcoord)) * (decode[3] - decode[2]) / cmax;
      return to_device.apply(Point{x, y});
    };
    const auto read_color = [&](std::array<double, 32>& c) {
      for (int i = 0; i < ncomp; ++i) {
        const auto k = static_cast<std::size_t>(4 + 2 * i);
        c[static_cast<std::size_t>(i)] = decode[k] + static_cast<double>(read(bpcomp)) * (decode[k + 1] - decode[k]) / kmax;
      }
    };
    std::vector<std::array<MeshVertex, 3>> triangles;
    const std::size_t max_triangles = 400000;
    if (type == 4) {
      std::vector<MeshVertex> pending;
      MeshVertex va, vb, vc;
      bool have = false;
      while (bit + static_cast<std::uint64_t>(bpflag + 2 * bpcoord) <= total_bits && triangles.size() < max_triangles) {
        const auto flag = read(bpflag);
        MeshVertex v;
        v.p = read_point();
        read_color(v.c);
        align();
        if (!have || flag == 0 || !pending.empty()) {
          pending.push_back(v);
          if (pending.size() == 3) {
            va = pending[0];
            vb = pending[1];
            vc = pending[2];
            pending.clear();
            have = true;
            triangles.push_back({va, vb, vc});
          }
        } else if (flag == 1) {
          va = vb;
          vb = vc;
          vc = v;
          triangles.push_back({va, vb, vc});
        } else {
          vb = vc;
          vc = v;
          triangles.push_back({va, vb, vc});
        }
      }
    } else if (type == 5) {
      std::vector<MeshVertex> prev, cur;
      if (per_row < 2) return;
      while (bit + static_cast<std::uint64_t>(2 * bpcoord) <= total_bits && triangles.size() < max_triangles) {
        MeshVertex v;
        v.p = read_point();
        read_color(v.c);
        align();
        cur.push_back(v);
        if (static_cast<int>(cur.size()) == per_row) {
          if (!prev.empty()) {
            for (int i = 0; i + 1 < per_row; ++i) {
              const auto k = static_cast<std::size_t>(i);
              triangles.push_back({prev[k], prev[k + 1], cur[k]});
              triangles.push_back({prev[k + 1], cur[k + 1], cur[k]});
            }
          }
          prev = std::move(cur);
          cur.clear();
        }
      }
    } else {
      // Coons (6) and tensor-product (7) patches, subdivided into triangles.
      std::array<Point, 16> last_pts{};
      std::array<std::array<double, 32>, 4> last_cols{};
      bool have_last = false;
      while (bit + static_cast<std::uint64_t>(bpflag) <= total_bits && triangles.size() < max_triangles) {
        const auto flag = read(bpflag);
        std::array<Point, 12> p{};
        std::array<std::array<double, 32>, 4> col{};
        const int new_points = flag == 0 ? 12 : 8;
        const int new_colors = flag == 0 ? 4 : 2;
        if (flag != 0 && !have_last) break;
        int start = 0;
        if (flag != 0) {
          // Shared edge from the previous patch.
          static constexpr int edges[4][4] = {{3, 4, 5, 6}, {6, 7, 8, 9}, {9, 10, 11, 0}, {0, 1, 2, 3}};
          const int e = static_cast<int>(std::min<std::uint64_t>(flag, 3));
          for (int i = 0; i < 4; ++i) p[static_cast<std::size_t>(i)] = last_pts[static_cast<std::size_t>(edges[e][i])];
          static constexpr int cedges[4][2] = {{1, 2}, {2, 3}, {3, 0}, {0, 1}};
          col[0] = last_cols[static_cast<std::size_t>(cedges[e][0])];
          col[1] = last_cols[static_cast<std::size_t>(cedges[e][1])];
          start = 4;
        }
        for (int i = start; i < start + new_points; ++i) p[static_cast<std::size_t>(i)] = read_point();
        if (type == 7) {
          for (int i = 0; i < 4; ++i) (void)read_point();  // interior points ignored (Coons approximation)
        }
        for (int i = 4 - new_colors; i < 4; ++i) read_color(col[static_cast<std::size_t>(i)]);
        align();
        if (bit > total_bits) break;
        for (std::size_t i = 0; i < 12; ++i) last_pts[i] = p[i];
        last_cols = col;
        have_last = true;
        // Boundary curves: C1 (p0..p3) bottom? Use the standard Coons parametrisation.
        const auto bez = [](Point a, Point b, Point c, Point d, double t) {
          const double u = 1 - t;
          return Point{u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
                       u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y};
        };
        const auto surface = [&](double u, double v) {
          const Point c1 = bez(p[0], p[11], p[10], p[9], u);   // v = 0
          const Point c2 = bez(p[3], p[4], p[5], p[6], u);     // v = 1
          const Point d1 = bez(p[0], p[1], p[2], p[3], v);     // u = 0
          const Point d2 = bez(p[9], p[8], p[7], p[6], v);     // u = 1
          return Point{(1 - v) * c1.x + v * c2.x + (1 - u) * d1.x + u * d2.x -
                           ((1 - u) * (1 - v) * p[0].x + u * (1 - v) * p[9].x + (1 - u) * v * p[3].x + u * v * p[6].x),
                       (1 - v) * c1.y + v * c2.y + (1 - u) * d1.y + u * d2.y -
                           ((1 - u) * (1 - v) * p[0].y + u * (1 - v) * p[9].y + (1 - u) * v * p[3].y + u * v * p[6].y)};
        };
        // corners: col[0] at p0 (u0 v0), col[1] at p3 (u0 v1), col[2] at p6 (u1 v1), col[3] at p9 (u1 v0)
        const auto colour = [&](double u, double v) {
          std::array<double, 32> c{};
          for (int i = 0; i < ncomp; ++i) {
            const auto k = static_cast<std::size_t>(i);
            c[k] = (1 - u) * (1 - v) * col[0][k] + (1 - u) * v * col[1][k] + u * v * col[2][k] + u * (1 - v) * col[3][k];
          }
          return c;
        };
        Rect b = Rect::inverted_infinite();
        for (const Point& q : p) b.include(q);
        const int n = static_cast<int>(std::clamp(std::max(b.width(), b.height()) / 4.0, 2.0, 32.0));
        for (int i = 0; i < n; ++i) {
          for (int j = 0; j < n; ++j) {
            const double u0 = static_cast<double>(i) / n, u1 = static_cast<double>(i + 1) / n;
            const double v0 = static_cast<double>(j) / n, v1 = static_cast<double>(j + 1) / n;
            const MeshVertex a{surface(u0, v0), colour(u0, v0)}, bb{surface(u1, v0), colour(u1, v0)},
                c{surface(u1, v1), colour(u1, v1)}, d{surface(u0, v1), colour(u0, v1)};
            triangles.push_back({a, bb, c});
            triangles.push_back({a, c, d});
          }
        }
      }
    }
    // Rasterise triangles with Gouraud shading (pixel centres).
    Canvas& dst = layers_.back().canvas;
    const Mask& clip = *clip_stack_.back();
    const auto* smask = dynamic_cast<const SoftMaskBuffer*>(gs.soft_mask.get());
    const int ca = static_cast<int>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255));
    for (const auto& t : triangles) {
      const Point a = t[0].p, b = t[1].p, c = t[2].p;
      const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
      if (std::abs(area) < 1e-9 || !std::isfinite(area)) continue;
      const int x0 = std::max(box_.x0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
      const int x1 = std::min(box_.x1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))) + 1);
      const int y0 = std::max(box_.y0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
      const int y1 = std::min(box_.y1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))) + 1);
      for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
          const double px = x + 0.5, py = y + 0.5;
          const double w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
          const double w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
          const double w2 = 1 - w0 - w1;
          if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
          int cov = region.at(x, y);
          if (cov == 0) continue;
          if (!clip.everywhere) cov = mul255(cov, clip.at(x, y));
          if (smask != nullptr) cov = mul255(cov, smask->at(x, y));
          if (cov == 0) continue;
          double comps[32] = {};
          for (int i = 0; i < ncomp; ++i) {
            const auto k = static_cast<std::size_t>(i);
            comps[i] = w0 * t[0].c[k] + w1 * t[1].c[k] + w2 * t[2].c[k];
          }
          Rgb v;
          if (function) {
            double out[32] = {};
            function->eval(comps, out);
            v = space->to_rgb(out);
          } else {
            v = space->to_rgb(comps);
          }
          const std::uint8_t s[4] = {to8(v.r), to8(v.g), to8(v.b), 255};
          blend_pixel(dst.at(x, y), s, mul255(cov, ca), gs.blend);
        }
      }
    }
  }

  const Mask* glyph_mask(const GlyphEvent& ev, const Path& outline) {
    const Matrix& m = ev.trm;
    const Point o = m.apply(Point{0, 0});
    if (!std::isfinite(o.x) || !std::isfinite(o.y) || std::abs(o.x) > 1e7 || std::abs(o.y) > 1e7) return nullptr;
    // Very large glyphs are not cached.
    const double size = m.max_expansion();
    const auto fx = static_cast<std::uint8_t>(std::clamp(static_cast<int>((o.x - std::floor(o.x)) * 4), 0, 3));
    const auto fy = static_cast<std::uint8_t>(std::clamp(static_cast<int>((o.y - std::floor(o.y)) * 4), 0, 3));
    GlyphKey key{ev.font, (static_cast<std::uint64_t>(ev.code.cid) << 32) | ev.code.code,
                 static_cast<std::int32_t>(std::lround(m.a * 1024)), static_cast<std::int32_t>(std::lround(m.b * 1024)),
                 static_cast<std::int32_t>(std::lround(m.c * 1024)), static_cast<std::int32_t>(std::lround(m.d * 1024)),
                 fx, fy};
    if (size < 200) {
      if (const auto it = glyph_cache_.find(key); it != glyph_cache_.end()) return &it->second;
    }
    // Render relative to the integer origin with the quantised subpixel phase.
    Matrix local = m;
    local.e = fx / 4.0 + 0.125;
    local.f = fy / 4.0 + 0.125;
    const Path device = outline.transformed(local);
    const Rect b = device.bounds();
    const IntRect box{static_cast<int>(std::floor(std::max(b.x0, -4096.0))) - 1,
                      static_cast<int>(std::floor(std::max(b.y0, -4096.0))) - 1,
                      static_cast<int>(std::ceil(std::min(b.x1, 4096.0))) + 1,
                      static_cast<int>(std::ceil(std::min(b.y1, 4096.0))) + 1};
    Mask mask = raster::fill_path(device, raster::FillRule::NonZero, box, antialias_);
    if (size >= 200) {
      scratch_glyph_ = std::move(mask);
      return &scratch_glyph_;
    }
    if (glyph_cache_.size() > 20000) glyph_cache_.clear();
    return &glyph_cache_.emplace(key, std::move(mask)).first->second;
  }

  PdfFile& file_;
  const Context& ctx_;
  Matrix base_;
  Matrix pattern_base_ = base_;
  bool antialias_ = true;
  std::vector<Layer> layers_;
  std::vector<std::shared_ptr<const Mask>> clip_stack_;
  IntRect box_;
  Path text_clip_;
  int depth_ = 0;
  std::unordered_map<GlyphKey, Mask, GlyphKeyHash> glyph_cache_;
  std::uint64_t work_ = 0;
  std::uint64_t work_budget_ = ~0ull;

 public:
  void set_work_budget(std::uint64_t pixels) { work_budget_ = pixels; }

 private:
  Mask scratch_glyph_;
};

void draw_annotations(PdfFile& file, Interpreter& interp, const Page& page, const Matrix& base) {
  for (const Object& annot_ref : file.get(page.dict, "Annots").as_array()) {
    const Object annot = file.resolve(annot_ref);
    const Dict& a = annot.as_dict();
    const std::int64_t flags = file.get(a, "F").as_int(0);
    if (flags & (2 | 32)) continue;  // hidden, no-view
    const std::string subtype = file.get(a, "Subtype").as_name();
    if (subtype == "Popup" || subtype == "Link") continue;
    const Object ap = file.get(file.get(a, "AP").as_dict(), "N");
    Object form = ap;
    if (ap.is_dict() && !ap.is_stream()) {
      const std::string state = file.get(a, "AS").as_name();
      form = file.get(ap.as_dict(), state);
    }
    if (!form.is_stream()) continue;
    const Array& rect_a = file.get(a, "Rect").as_array();
    if (rect_a.size() != 4) continue;
    const Rect rect = Rect{file.resolve(rect_a[0]).as_number(), file.resolve(rect_a[1]).as_number(),
                           file.resolve(rect_a[2]).as_number(), file.resolve(rect_a[3]).as_number()}
                          .normalized();
    const Dict& fd = form.as_dict();
    const Array& bb = file.get(fd, "BBox").as_array();
    if (bb.size() != 4) continue;
    Rect bbox = Rect{file.resolve(bb[0]).as_number(), file.resolve(bb[1]).as_number(), file.resolve(bb[2]).as_number(),
                     file.resolve(bb[3]).as_number()}
                    .normalized();
    Matrix fm;
    const Array& mm = file.get(fd, "Matrix").as_array();
    if (mm.size() == 6) {
      fm = Matrix{file.resolve(mm[0]).as_number(1), file.resolve(mm[1]).as_number(), file.resolve(mm[2]).as_number(),
                  file.resolve(mm[3]).as_number(1), file.resolve(mm[4]).as_number(), file.resolve(mm[5]).as_number()};
    }
    const Rect t = fm.apply(bbox);
    if (t.empty() || rect.empty()) continue;
    const Matrix fit{rect.width() / t.width(), 0, 0, rect.height() / t.height(),
                     rect.x0 - t.x0 * rect.width() / t.width(), rect.y0 - t.y0 * rect.height() / t.height()};
    GState gs;
    gs.ctm = Matrix::concat(fit, base);
    interp.run_form(form, gs, 1, page.resources);
  }
}

}  // namespace

Result<ir::Frame> render_page(PdfFile& file, int page_index, const RenderOptions& options, const Context& ctx) {
  const Page& page = file.pages()[static_cast<std::size_t>(page_index)];
  const double scale = options.dpi / 72.0;
  const PageTransform t = page_transform(page, scale);
  const auto width = static_cast<std::uint64_t>(std::max(1.0, std::ceil(t.width - 1e-6)));
  const auto height = static_cast<std::uint64_t>(std::max(1.0, std::ceil(t.height - 1e-6)));
  if (width > 100000 || height > 100000) return fail(Status::Limit, "page {} is too large to render", page_index + 1);
  CVT_TRY(ctx.limits.check_pixels(width, height));
  FontCache fonts;
  RenderDevice device(file, ctx, static_cast<int>(width), static_cast<int>(height), t.matrix, options.antialias);
  // Painting more than ~400 full pages' worth of pixels means a hostile or
  // pathological file: stop there.
  device.set_work_budget(std::max<std::uint64_t>(width * height * 400, 1u << 24));
  Interpreter interp(file, device, fonts, &ctx);
  device.interp = &interp;
  interp.run_page(page, t.matrix);
  if (options.annotations && !interp.aborted()) draw_annotations(file, interp, page, t.matrix);
  if (ctx.progress.cancelled()) return fail(Status::Cancelled, "the conversion was cancelled");
  ir::Frame frame;
  frame.width = static_cast<std::uint32_t>(width);
  frame.height = static_cast<std::uint32_t>(height);
  frame.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  const Canvas& c = device.canvas();
  const auto bg = options.background;
  for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
    const int a = c.px[i + 3];
    // Composite over the background (straight alpha output).
    const int inv = 255 - a;
    const int out_a = a + mul255(bg[3], inv);
    for (int ch = 0; ch < 3; ++ch) {
      const int premult = c.px[i + static_cast<std::size_t>(ch)] + mul255(mul255(bg[static_cast<std::size_t>(ch)], bg[3]), inv);
      frame.rgba[i + static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>(out_a > 0 ? std::min(255, premult * 255 / out_a) : 0);
    }
    frame.rgba[i + 3] = static_cast<std::uint8_t>(out_a);
  }
  return frame;
}

Result<ir::Image> render_pages(PdfFile& file, const RenderOptions& options, const Context& ctx) {
  ir::Image image;
  // All frames of an ir::Image share one size: pages of other sizes are
  // scaled into the first page's size (keeping aspect, centred).
  std::uint64_t total_pixels = 0;
  for (std::size_t i = 0; i < options.pages.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(options.pages.size()), "Rendering"));
    CVT_TRY_ASSIGN(ir::Frame frame, render_page(file, options.pages[i], options, ctx));
    total_pixels += static_cast<std::uint64_t>(frame.width) * frame.height;
    if (total_pixels > ctx.limits.max_pixels) return fail(Status::Limit, "rendering exceeds the pixel limit");
    if (!image.frames.empty() && (frame.width != image.frames[0].width || frame.height != image.frames[0].height)) {
      ir::Frame fitted = ir::Frame::filled(image.frames[0].width, image.frames[0].height, options.background);
      const double s = std::min(static_cast<double>(fitted.width) / frame.width, static_cast<double>(fitted.height) / frame.height);
      const auto nw = static_cast<std::uint32_t>(std::max(1.0, frame.width * s)), nh = static_cast<std::uint32_t>(std::max(1.0, frame.height * s));
      const std::uint32_t ox = (fitted.width - nw) / 2, oy = (fitted.height - nh) / 2;
      for (std::uint32_t y = 0; y < nh; ++y) {
        for (std::uint32_t x = 0; x < nw; ++x) {
          const auto sx = static_cast<std::uint32_t>(std::min<double>(frame.width - 1, (x + 0.5) / s));
          const auto sy = static_cast<std::uint32_t>(std::min<double>(frame.height - 1, (y + 0.5) / s));
          std::copy_n(frame.rgba.begin() + static_cast<std::ptrdiff_t>(frame.offset(sx, sy)), 4,
                      fitted.rgba.begin() + static_cast<std::ptrdiff_t>(fitted.offset(x + ox, y + oy)));
        }
      }
      frame = std::move(fitted);
      ctx.warn("Pages of different sizes were scaled to the size of the first page.");
    }
    image.frames.push_back(std::move(frame));
  }
  image.dpi_x = image.dpi_y = options.dpi;
  for (const std::string& w : file.warnings()) ctx.warn(w);
  return image;
}

}  // namespace cvt::pdf
