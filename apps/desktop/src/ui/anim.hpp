// Convertible desktop - animation primitives: cubic-bezier easing, critically
// damped / underdamped springs, keyframe tracks and a timeline.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <string>
#include <vector>

namespace cvd::ui {

/// CSS-style cubic-bezier(x1, y1, x2, y2) timing function.
class CubicBezier {
 public:
  constexpr CubicBezier() = default;
  constexpr CubicBezier(float x1, float y1, float x2, float y2) : x1_(x1), y1_(y1), x2_(x2), y2_(y2) {}
  explicit CubicBezier(const std::array<float, 4>& p) : x1_(p[0]), y1_(p[1]), x2_(p[2]), y2_(p[3]) {}

  /// Eased progress for linear progress t in [0, 1].
  [[nodiscard]] float operator()(float t) const;
  [[nodiscard]] bool is_linear() const { return x1_ == y1_ && x2_ == y2_; }

 private:
  [[nodiscard]] float sample_x(float t) const;
  [[nodiscard]] float sample_y(float t) const;
  [[nodiscard]] float sample_dx(float t) const;
  float x1_ = 0.0f, y1_ = 0.0f, x2_ = 1.0f, y2_ = 1.0f;
};

/// A one-dimensional spring (mass 1) stepped with a fixed sub-step.
struct Spring {
  float stiffness = 380.0f;
  float damping = 34.0f;  // ~critically damped for stiffness 300
  float value = 0.0f;
  float velocity = 0.0f;
  float target = 0.0f;

  /// Advances by dt seconds; returns true while still moving.
  bool step(float dt);
  [[nodiscard]] bool settled() const;
  void snap(float v) {
    value = target = v;
    velocity = 0.0f;
  }
};

/// Time-based tween from a start value to a target with an easing.
struct Tween {
  float from = 0.0f;
  float to = 0.0f;
  double start_time = 0.0;
  float duration = 0.22f;  // seconds
  CubicBezier easing{0.2f, 0.0f, 0.0f, 1.0f};

  [[nodiscard]] float value(double now) const;
  [[nodiscard]] bool done(double now) const { return now >= start_time + duration; }
  void retarget(float target, double now, float new_duration);
};

/// A keyframe track as in assets/brand/animation.json: each keyframe's easing
/// shapes the interpolation towards the next keyframe; values hold before the
/// first and after the last keyframe.
struct Keyframe {
  float t_ms = 0.0f;
  float value = 0.0f;
  bool has_easing = false;
  CubicBezier easing;
};

class Track {
 public:
  Track() = default;
  explicit Track(std::vector<Keyframe> keys) : keys_(std::move(keys)) {}
  [[nodiscard]] float value(float t_ms, float base) const;
  [[nodiscard]] bool empty() const { return keys_.empty(); }
  [[nodiscard]] float end_time() const { return keys_.empty() ? 0.0f : keys_.back().t_ms; }
  [[nodiscard]] const std::vector<Keyframe>& keys() const { return keys_; }

 private:
  std::vector<Keyframe> keys_;
};

}  // namespace cvd::ui
