// Convertible desktop - the in-house software renderer.
//
// Draws into a premultiplied BGRA bitmap (one uint32 per pixel, 0xAARRGGBB
// in a little-endian word, the native layout of Win32 DIBs, X11 32-bit
// ZPixmaps and CoreGraphics' "premultiplied first, 32 little" images).
// Anti-aliasing uses exact area coverage accumulation (non-zero winding),
// rounded rectangles and shadows are evaluated analytically.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <vector>

#include "base/geometry.hpp"
#include "gfx/path.hpp"

namespace cvd::gfx {

struct IRect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
  [[nodiscard]] int width() const { return x1 - x0; }
  [[nodiscard]] int height() const { return y1 - y0; }
  [[nodiscard]] IRect intersect(const IRect& o) const {
    return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
  }
};

struct Bitmap {
  int width = 0;
  int height = 0;
  std::vector<std::uint32_t> pixels;

  Bitmap() = default;
  Bitmap(int w, int h) { resize(w, h); }
  void resize(int w, int h) {
    width = std::max(0, w);
    height = std::max(0, h);
    pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);
  }
  [[nodiscard]] std::uint32_t at(int x, int y) const {
    return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
  }
  std::uint32_t* row(int y) { return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width); }
  [[nodiscard]] const std::uint32_t* row(int y) const {
    return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
  }
};

/// 8-bit coverage mask positioned in device pixels (glyphs, blurred shapes).
struct Mask {
  int width = 0;
  int height = 0;
  int left = 0;  // offset of the mask's top-left from the pen position
  int top = 0;
  std::vector<std::uint8_t> alpha;
};

struct GradientStop {
  float offset = 0.0f;
  Color color;
};

struct Paint {
  enum class Kind { Solid, Linear, Radial };
  Kind kind = Kind::Solid;
  Color color;
  Vec2 p0{};       // linear start / radial centre (user space)
  Vec2 p1{};       // linear end
  float radius = 0.0f;
  std::vector<GradientStop> stops;

  Paint() = default;
  Paint(Color c) : color(c) {}  // NOLINT: implicit for convenience
  static Paint solid(Color c) { return Paint(c); }
  static Paint linear(Vec2 a, Vec2 b, std::vector<GradientStop> s) {
    Paint p;
    p.kind = Kind::Linear;
    p.p0 = a;
    p.p1 = b;
    p.stops = std::move(s);
    return p;
  }
  static Paint radial(Vec2 c, float r, std::vector<GradientStop> s) {
    Paint p;
    p.kind = Kind::Radial;
    p.p0 = c;
    p.radius = r;
    p.stops = std::move(s);
    return p;
  }
};

class Canvas {
 public:
  explicit Canvas(Bitmap& target);

  [[nodiscard]] Bitmap& target() { return *target_; }

  // State --------------------------------------------------------------------
  void save();
  void restore();
  void translate(float x, float y);
  void scale(float sx, float sy);
  void rotate(float radians);
  void concat(const Affine& t);
  void set_transform(const Affine& t);
  [[nodiscard]] const Affine& transform() const { return state_.ctm; }
  /// Intersects the clip with a rectangle in user space (its device bounding box).
  void clip_rect(const Rect& r);
  [[nodiscard]] IRect clip_bounds() const { return state_.clip; }
  /// Multiplies the global opacity.
  void set_opacity(float alpha);
  [[nodiscard]] float opacity() const { return state_.opacity; }

  // Drawing ------------------------------------------------------------------
  void clear(Color c);
  void fill_rect(const Rect& r, const Paint& paint);
  void fill_rrect(const Rect& r, float radius, const Paint& paint);
  void stroke_rrect(const Rect& r, float radius, float width, const Paint& paint);
  void fill_circle(Vec2 c, float radius, const Paint& paint);
  void fill_path(const Path& path, const Paint& paint);
  void stroke_path(const Path& path, const StrokeStyle& style, const Paint& paint);
  /// Strokes an already flattened polyline given in user space.
  void stroke_polyline(const std::vector<Vec2>& points, bool closed, const StrokeStyle& style, const Paint& paint);
  /// Fills polygons given in device space (non-zero winding).
  void fill_device_contours(const std::vector<Contour>& contours, const Paint& paint);
  /// CSS-style box shadow of a rounded rectangle: `blur` is the CSS blur radius.
  void draw_shadow(const Rect& r, float radius, float blur, float spread, Vec2 offset, Color color);
  /// Blends a coverage mask at an integer device position with a colour.
  void draw_mask(const Mask& mask, int device_x, int device_y, Color color);
  /// Draws a premultiplied image scaled into `dst` (user space), bilinear filtered.
  void draw_image(const Bitmap& image, const Rect& dst, float alpha = 1.0f);
  /// In-place blur of a device-space region (three box passes ≈ Gaussian).
  void blur_region(const Rect& r, float radius);

 private:
  struct State {
    Affine ctm;
    IRect clip;
    float opacity = 1.0f;
  };
  struct Shader;

  void rasterize(const std::vector<Contour>& contours, const Paint& paint);
  [[nodiscard]] IRect device_bounds(const Rect& r) const;

  Bitmap* target_;
  State state_;
  std::vector<State> stack_;
  std::vector<float> accum_;
  std::vector<float> coverage_;
};

/// Rasterises device-space polygons (non-zero winding) into a coverage mask
/// whose left/top hold the mask origin in the same space.
Mask rasterize_mask(const std::vector<Contour>& contours);

/// Box-blurs an 8-bit mask in place (three passes per axis).
void blur_mask(Mask& mask, float radius);

/// Converts premultiplied BGRA to straight RGBA bytes (for image files).
std::vector<std::uint8_t> to_rgba8(const Bitmap& bitmap);

}  // namespace cvd::gfx
