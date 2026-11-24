// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "ui/anim.hpp"

#include <algorithm>
#include <cmath>

namespace cvd::ui {

float CubicBezier::sample_x(float t) const {
  const float mt = 1.0f - t;
  return 3.0f * mt * mt * t * x1_ + 3.0f * mt * t * t * x2_ + t * t * t;
}

float CubicBezier::sample_y(float t) const {
  const float mt = 1.0f - t;
  return 3.0f * mt * mt * t * y1_ + 3.0f * mt * t * t * y2_ + t * t * t;
}

float CubicBezier::sample_dx(float t) const {
  const float mt = 1.0f - t;
  return 3.0f * mt * mt * x1_ + 6.0f * mt * t * (x2_ - x1_) + 3.0f * t * t * (1.0f - x2_);
}

float CubicBezier::operator()(float x) const {
  if (x <= 0.0f) return 0.0f;
  if (x >= 1.0f) return 1.0f;
  if (is_linear()) return x;
  // Newton-Raphson, falling back to bisection.
  float t = x;
  for (int i = 0; i < 8; ++i) {
    const float err = sample_x(t) - x;
    if (std::fabs(err) < 1e-6f) return sample_y(t);
    const float d = sample_dx(t);
    if (std::fabs(d) < 1e-6f) break;
    t -= err / d;
  }
  float lo = 0.0f, hi = 1.0f;
  t = x;
  for (int i = 0; i < 40; ++i) {
    const float v = sample_x(t);
    if (std::fabs(v - x) < 1e-6f) break;
    if (v < x) lo = t;
    else hi = t;
    t = (lo + hi) * 0.5f;
  }
  return sample_y(t);
}

bool Spring::step(float dt) {
  constexpr float kSub = 1.0f / 240.0f;
  dt = std::min(dt, 0.1f);
  while (dt > 0.0f) {
    const float h = std::min(dt, kSub);
    const float force = -stiffness * (value - target) - damping * velocity;
    velocity += force * h;
    value += velocity * h;
    dt -= h;
  }
  if (settled()) {
    value = target;
    velocity = 0.0f;
    return false;
  }
  return true;
}

bool Spring::settled() const { return std::fabs(value - target) < 0.0015f && std::fabs(velocity) < 0.01f; }

float Tween::value(double now) const {
  if (duration <= 0.0f) return to;
  const float t = static_cast<float>((now - start_time) / static_cast<double>(duration));
  if (t <= 0.0f) return from;
  if (t >= 1.0f) return to;
  return from + (to - from) * easing(t);
}

void Tween::retarget(float target, double now, float new_duration) {
  if (target == to) return;
  from = value(now);
  to = target;
  start_time = now;
  duration = new_duration;
}

float Track::value(float t, float base) const {
  if (keys_.empty()) return base;
  if (t <= keys_.front().t_ms) return keys_.front().value;
  for (std::size_t i = 0; i + 1 < keys_.size(); ++i) {
    const Keyframe& a = keys_[i];
    const Keyframe& b = keys_[i + 1];
    if (t < b.t_ms) {
      if (!a.has_easing) return a.value;  // hold
      const float span = b.t_ms - a.t_ms;
      const float u = span > 0.0f ? (t - a.t_ms) / span : 1.0f;
      return a.value + (b.value - a.value) * a.easing(u);
    }
  }
  return keys_.back().value;
}

}  // namespace cvd::ui
