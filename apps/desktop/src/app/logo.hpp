// Convertible desktop - the brand mark, its intro animation, the spinner and
// determinate progress, all evaluated from assets/brand/animation.json
// (embedded at build time): geometry from its path data, layers and keyframe
// tracks with their cubic-bezier easings.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/geometry.hpp"
#include "gfx/canvas.hpp"
#include "gfx/path.hpp"
#include "ui/anim.hpp"

namespace cvd::app {

class Logo {
 public:
  struct Layer {
    std::string id;
    std::string type;      // group | shape | stroke
    std::string geometry;  // seed | endSquare | centerline
    std::string paint;     // flux | iris | coral
    Vec2 pivot{512.0f, 512.0f};
    std::map<std::string, float, std::less<>> base;
    std::map<std::string, ui::Track, std::less<>> tracks;
    std::vector<std::string> children;

    [[nodiscard]] float value(std::string_view prop, float t_ms, float fallback) const;
  };
  struct Scene {
    float duration_ms = 0.0f;
    bool loop = false;
    std::vector<Layer> layers;
    [[nodiscard]] const Layer* find(std::string_view id) const;
  };

  /// Parses the storyboard; false when it is malformed.
  bool load(std::string_view json_text);
  /// The storyboard embedded in the executable.
  static const Logo& instance();

  [[nodiscard]] bool valid() const { return valid_; }
  [[nodiscard]] float intro_duration_ms() const { return intro_.duration_ms; }
  [[nodiscard]] float spinner_duration_ms() const { return spinner_.duration_ms; }
  [[nodiscard]] float centerline_length() const { return length_; }
  [[nodiscard]] const Scene& intro() const { return intro_; }
  [[nodiscard]] const Scene& spinner() const { return spinner_; }

  /// The finished mark (identical to logo.svg) fitted into `box`.
  void draw_static(gfx::Canvas& c, const Rect& box, bool on_light) const;
  /// One frame of the intro at `t_ms` (>= duration draws the static mark).
  void draw_intro(gfx::Canvas& c, const Rect& box, float t_ms, bool on_light) const;
  /// Indeterminate spinner at loop time `t_ms`; reduced motion pulses instead of drawing.
  void draw_spinner(gfx::Canvas& c, const Rect& box, float t_ms, bool on_light, bool reduced_motion) const;
  /// Determinate progress p in [0, 1]: the stroke trimmed to p; p = 1 is the finished mark.
  void draw_progress(gfx::Canvas& c, const Rect& box, float p, bool on_light) const;
  /// Square ripple used as the completion cue (t in ms since completion, 0..480).
  void draw_completion_ripple(gfx::Canvas& c, const Rect& box, float t_ms, bool on_light) const;

 private:
  void apply_box(gfx::Canvas& c, const Rect& box) const;
  [[nodiscard]] gfx::Paint paint(std::string_view name, bool on_light) const;
  void draw_scene(gfx::Canvas& c, const Scene& scene, float t_ms, bool on_light, float opacity_scale) const;
  void draw_layer(gfx::Canvas& c, const Scene& scene, const Layer& layer, float t_ms, bool on_light) const;
  void draw_trace(gfx::Canvas& c, float start, float end, const gfx::Paint& p) const;
  [[nodiscard]] bool is_full_mark_group(const Scene& scene, const Layer& group, float t) const;

  bool valid_ = false;
  std::vector<Vec2> centerline_;
  std::optional<gfx::PolylineMeasure> measure_;
  float length_ = 0.0f;
  float stroke_width_ = 200.0f;
  float miter_limit_ = 4.0f;
  Vec2 seed_c_{772.0f, 260.0f};
  float seed_r_ = 100.0f;
  Rect end_square_{664.0f, 664.0f, 200.0f, 200.0f};
  gfx::Path outline_;
  Vec2 flux_p0_{0.0f, 360.0f};
  Vec2 flux_p1_{0.0f, 664.0f};
  std::vector<gfx::GradientStop> flux_dark_;
  std::vector<gfx::GradientStop> flux_light_;
  Color iris_ = Color::hex("#7C6CFF");
  Color coral_ = Color::hex("#FF6452");
  Color iris_light_ = Color::hex("#6654F0");
  Color coral_light_ = Color::hex("#F04A36");
  std::map<std::string, ui::CubicBezier, std::less<>> easings_;
  Scene intro_;
  Scene spinner_;
  ui::CubicBezier pulse_easing_{0.65f, 0.0f, 0.35f, 1.0f};
};

}  // namespace cvd::app
