// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "ui/ui.hpp"

#include <cmath>

namespace cvd::ui {

WidgetId hash_id(std::string_view text, WidgetId seed) {
  WidgetId h = seed;
  for (const char c : text) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h == 0 ? 1 : h;
}

Context::Context() { id_stack_.push_back(1469598103934665603ULL); }

WidgetId Context::pick(const std::vector<HitBox>& hits, Layer block, Vec2 m) {
  const HitBox* best = nullptr;
  for (const HitBox& h : hits) {
    if (static_cast<int>(h.layer) < static_cast<int>(block)) continue;
    if ((h.flags & kNoHover) || !h.rect.contains(m)) continue;
    if (!best || static_cast<int>(h.layer) >= static_cast<int>(best->layer)) best = &h;
  }
  return (best && !(best->flags & kDisabled)) ? best->id : 0;
}

// ---------------------------------------------------------------------------
// Ids
// ---------------------------------------------------------------------------

WidgetId Context::id(std::string_view name) const { return hash_id(name, id_stack_.back()); }

WidgetId Context::id(std::string_view name, std::uint64_t index) const {
  WidgetId h = hash_id(name, id_stack_.back());
  h ^= index + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
  return h == 0 ? 1 : h;
}

void Context::push_id(std::string_view name) { id_stack_.push_back(id(name)); }
void Context::push_id(std::uint64_t index) { id_stack_.push_back(id("#", index)); }
void Context::pop_id() {
  if (id_stack_.size() > 1) id_stack_.pop_back();
}

// ---------------------------------------------------------------------------
// Events and frames
// ---------------------------------------------------------------------------

bool Context::handle_event(const Event& e) {
  switch (e.type) {
    case EventType::MouseMove:
      mouse_ = e.pos;
      mouse_inside_ = true;
      mods_ = e.mods;
      return true;
    case EventType::MouseDown:
      mouse_ = e.pos;
      mouse_inside_ = true;
      mods_ = e.mods;
      if (e.button == MouseButton::Left) {
        pending_press_ = true;
        mouse_down_ = true;
        pending_clicks_ = e.clicks;
        press_pos_ = e.pos;
      } else if (e.button == MouseButton::Right) {
        pending_right_ = true;
      }
      return true;
    case EventType::MouseUp:
      mouse_ = e.pos;
      if (e.button == MouseButton::Left) {
        pending_release_ = true;
        mouse_down_ = false;
      }
      return true;
    case EventType::MouseLeave:
      mouse_inside_ = false;
      if (!mouse_down_) mouse_ = {-1000.0f, -1000.0f};
      return true;
    case EventType::Scroll:
      mouse_ = e.pos;
      pending_scroll_ += e.scroll;
      return true;
    case EventType::KeyDown:
      mods_ = e.mods;
      pending_keys_.push_back({e.key, e.mods, false});
      return true;
    case EventType::KeyUp:
      mods_ = e.mods;
      return false;
    case EventType::Text:
      pending_typed_ += e.text;
      return true;
    case EventType::DragEnter:
    case EventType::DragOver:
      drag_over_ = true;
      drag_pos_ = e.pos;
      mouse_ = e.pos;
      return true;
    case EventType::DragLeave:
    case EventType::Drop:
      drag_over_ = false;
      return true;
    case EventType::FocusLost:
      mouse_down_ = false;
      active_ = 0;
      mods_ = {};
      return true;
    default:
      return true;
  }
}

void Context::begin_frame(Vec2 size, double time_seconds, const Theme* theme) {
  dt_ = frame_ == 0 ? 0.0f : static_cast<float>(std::clamp(time_seconds - time_, 0.0, 0.1));
  time_ = time_seconds;
  ++frame_;
  theme_ = theme;
  size_ = size;
  needs_frame_ = false;
  wake_at_ = 0.0;

  mouse_pressed_ = pending_press_;
  mouse_released_ = pending_release_;
  right_pressed_ = pending_right_;
  press_clicks_ = pending_clicks_;
  scroll_accum_ = pending_scroll_;
  keys_ = std::move(pending_keys_);
  typed_ = std::move(pending_typed_);
  pending_keys_.clear();
  pending_typed_.clear();
  pending_press_ = pending_release_ = pending_right_ = false;
  pending_scroll_ = {};
  pending_clicks_ = 1;

  // Hover from the previous frame's layout and the current mouse position.
  hot_ = (mouse_inside_ || drag_over_) ? pick(prev_hits_, prev_block_layer_, mouse_) : 0;
  hot_scroll_ = 0;

  // Keyboard focus traversal.
  focus_changed_by_key_ = false;
  for (auto& k : keys_) {
    if (k.consumed || k.key != Key::Tab || k.mods.primary() || k.mods.alt) continue;
    k.consumed = true;
    if (prev_focus_order_.empty()) continue;
    auto it = std::find(prev_focus_order_.begin(), prev_focus_order_.end(), focused_);
    std::size_t idx = 0;
    if (it == prev_focus_order_.end()) {
      idx = k.mods.shift ? prev_focus_order_.size() - 1 : 0;
    } else {
      const auto cur = static_cast<std::size_t>(it - prev_focus_order_.begin());
      const std::size_t n = prev_focus_order_.size();
      idx = k.mods.shift ? (cur + n - 1) % n : (cur + 1) % n;
    }
    focused_ = prev_focus_order_[idx];
    focus_visible_ = true;
    focus_changed_by_key_ = true;
  }
  for (const auto& k : keys_) {
    if (k.key != Key::Tab && k.key != Key::None) {
      if (k.key == Key::Up || k.key == Key::Down || k.key == Key::Left || k.key == Key::Right ||
          k.key == Key::Enter || k.key == Key::Space || k.key == Key::Escape) {
        if (focused_ != 0) focus_visible_ = true;
      }
    }
  }
  if (mouse_pressed_) {
    focus_visible_ = false;
    if (hot_ == 0) focused_ = 0;
  }

  hits_.clear();
  focus_order_.clear();
  access_.clear();
  focused_seen_ = false;
  for (int i = 0; i < kLayerCount; ++i) {
    layers_[i].clear();
    clip_stack_[i].clear();
    translate_stack_[i].clear();
  }
  layer_ = Layer::Base;
  block_layer_ = Layer::Base;
  cursor_ = Cursor::Arrow;
  tooltip_text_.clear();
  scroll_stack_.clear();
  id_stack_.resize(1);
}

void Context::end_frame() {
  if (focused_ != 0 && !focused_seen_) focused_ = 0;
  if (mouse_released_) active_ = 0;
  if (!mouse_down_ && active_ != 0 && !mouse_released_) active_ = 0;

  // Tooltip.
  if (hot_ != hover_tracked_) {
    hover_tracked_ = hot_;
    hover_start_ = time_;
  }
  if (!tooltip_text_.empty() && tooltip_id_ == hot_ && hot_ != 0 && !mouse_down_) {
    const double wait = 0.55;
    if (time_ - hover_start_ >= wait) {
      const Theme& t = *theme_;
      const text::Style& st = t.type.caption;
      const float tw = std::min(text::measure(tooltip_text_, st), 320.0f);
      const float w = tw + 20.0f;
      const float h = st.line_height + 12.0f;
      float x = clampf(tooltip_anchor_.center().x - w * 0.5f, 8.0f, size_.x - w - 8.0f);
      float y = tooltip_anchor_.bottom() + 8.0f;
      if (y + h > size_.y - 8.0f) y = tooltip_anchor_.y - h - 8.0f;
      const Rect box{x, y, w, h};
      const float appear = clampf(static_cast<float>((time_ - hover_start_ - wait) / 0.12), 0.0f, 1.0f);
      if (appear < 1.0f) needs_frame_ = true;
      const Layer saved = layer_;
      layer_ = Layer::Tooltip;
      push_opacity(appear);
      shadow(box, t.radius_sm, 3);
      fill(box, t.radius_sm, t.dark ? t.color.surface_elevated : t.color.text_primary);
      if (t.dark) stroke(box, t.radius_sm, 1.0f, t.color.border);
      const std::string label = text::ellipsize_end(tooltip_text_, st, tw);
      text_in(label, st, box.inset(10.0f, 0.0f), t.dark ? t.color.text_primary : t.color.background, 0.0f);
      pop_opacity();
      layer_ = saved;
    } else {
      request_frame_at(hover_start_ + wait + 0.001);
    }
  }

  // Recompute hover against the new layout; ask for another frame when it moved.
  const WidgetId new_hot = (mouse_inside_ || drag_over_) ? pick(hits_, block_layer_, mouse_) : 0;
  if (new_hot != hot_) needs_frame_ = true;

  prev_hits_ = std::move(hits_);
  prev_focus_order_ = std::move(focus_order_);
  access_prev_ = std::move(access_);
  prev_block_layer_ = block_layer_;
  hits_.clear();
  focus_order_.clear();
  access_.clear();
}

void Context::render(gfx::Canvas& canvas) {
  for (auto& layer : layers_) {
    canvas.save();
    for (auto& cmd : layer) cmd(canvas);
    canvas.restore();
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Context::draw(DrawCommand cmd) { layers_[static_cast<int>(layer_)].push_back(std::move(cmd)); }

void Context::push_clip(const Rect& r) {
  const Vec2 t = current_translate();
  const Rect screen = r.offset(t.x, t.y);
  auto& stack = clip_stack_[static_cast<int>(layer_)];
  stack.push_back(stack.empty() ? screen : stack.back().intersect(screen));
  draw([r](gfx::Canvas& c) {
    c.save();
    c.clip_rect(r);
  });
}

void Context::pop_clip() {
  auto& stack = clip_stack_[static_cast<int>(layer_)];
  if (!stack.empty()) stack.pop_back();
  draw([](gfx::Canvas& c) { c.restore(); });
}

Rect Context::current_clip() const {
  const auto& stack = clip_stack_[static_cast<int>(layer_)];
  return stack.empty() ? Rect{0.0f, 0.0f, size_.x, size_.y} : stack.back();
}

void Context::push_opacity(float alpha) {
  draw([alpha](gfx::Canvas& c) {
    c.save();
    c.set_opacity(alpha);
  });
}

void Context::pop_opacity() {
  draw([](gfx::Canvas& c) { c.restore(); });
}

void Context::push_translate(Vec2 d) {
  auto& stack = translate_stack_[static_cast<int>(layer_)];
  stack.push_back((stack.empty() ? Vec2{} : stack.back()) + d);
  draw([d](gfx::Canvas& c) {
    c.save();
    c.translate(d.x, d.y);
  });
}

void Context::pop_translate() {
  auto& stack = translate_stack_[static_cast<int>(layer_)];
  if (!stack.empty()) stack.pop_back();
  draw([](gfx::Canvas& c) { c.restore(); });
}

Vec2 Context::current_translate() const {
  const auto& stack = translate_stack_[static_cast<int>(layer_)];
  return stack.empty() ? Vec2{} : stack.back();
}

void Context::fill(const Rect& r, float radius, Color c) {
  if (c.a <= 0.0f || r.empty()) return;
  draw([r, radius, c](gfx::Canvas& cv) { cv.fill_rrect(r, radius, c); });
}

void Context::fill_paint(const Rect& r, float radius, gfx::Paint paint) {
  if (r.empty()) return;
  draw([r, radius, paint = std::move(paint)](gfx::Canvas& cv) { cv.fill_rrect(r, radius, paint); });
}

void Context::stroke(const Rect& r, float radius, float width, Color c) {
  if (c.a <= 0.0f || r.empty()) return;
  draw([r, radius, width, c](gfx::Canvas& cv) { cv.stroke_rrect(r, radius, width, c); });
}

void Context::shadow(const Rect& r, float radius, int elevation) {
  const auto& layers = theme_->elevation[static_cast<std::size_t>(std::clamp(elevation, 0, 4))];
  for (const ShadowLayer& s : layers) {
    draw([r, radius, s](gfx::Canvas& cv) { cv.draw_shadow(r, radius, s.blur, s.spread, {s.x, s.y}, s.color); });
  }
}

void Context::text(std::string_view s, const text::Style& style, Vec2 baseline, Color c) {
  if (s.empty() || c.a <= 0.0f) return;
  draw([str = std::string(s), style, baseline, c](gfx::Canvas& cv) { text::draw_text(cv, str, style, baseline, c); });
}

void Context::text_in(std::string_view s, const text::Style& style, const Rect& r, Color c, float align) {
  if (s.empty()) return;
  const float w = text::measure(s, style);
  const float x = r.x + (r.w - w) * align;
  const float y = r.y + (r.h - style.line_height) * 0.5f + text::baseline_offset(style);
  text(s, style, {x, y}, c);
}

void Context::focus_ring(WidgetId wid, const Rect& r, float radius) {
  if (!focus_visible_ || focused_ != wid) return;
  const Theme& t = *theme_;
  const float o = t.focus_ring_offset + t.focus_ring_width;
  stroke(r.inset(-o), radius + o, t.focus_ring_width, t.color.focus_ring);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

Interaction Context::interact(WidgetId wid, const Rect& r, unsigned flags) {
  Interaction out;
  const Vec2 t = current_translate();
  const Rect screen = r.offset(t.x, t.y).intersect(current_clip());
  hits_.push_back({wid, screen, layer_, flags});
  const bool disabled = (flags & kDisabled) != 0;
  if ((flags & kFocusable) && !disabled) focus_order_.push_back(wid);
  if (focused_ == wid) {
    focused_seen_ = true;
    focused_rect_ = r.offset(t.x, t.y);
    focused_scroll_ = scroll_stack_.empty() ? 0 : scroll_stack_.back().id;
  }
  out.focused = focused_ == wid;
  if (disabled) return out;
  if (static_cast<int>(layer_) < static_cast<int>(prev_block_layer_)) return out;
  out.hovered = hot_ == wid && !screen.empty();
  if (mouse_pressed_ && out.hovered) {
    active_ = wid;
    if (flags & kFocusable) {
      focused_ = wid;
      focused_seen_ = true;
      focused_rect_ = r.offset(t.x, t.y);
      out.focused = true;
    } else {
      focused_ = 0;
    }
    out.double_clicked = press_clicks_ >= 2;
  }
  if (right_pressed_ && out.hovered) out.right_clicked = true;
  out.pressed = active_ == wid && mouse_down_;
  if (mouse_released_ && active_ == wid) {
    if (out.hovered) out.clicked = true;
  }
  if (out.focused && (flags & kKeyboardActivate)) {
    for (auto& k : keys_) {
      if (!k.consumed && (k.key == Key::Enter || k.key == Key::Space) && !k.mods.primary()) {
        k.consumed = true;
        out.clicked = true;
        focus_visible_ = true;
      }
    }
  }
  return out;
}

void Context::describe(WidgetId wid, Role role, std::string_view label, const Rect& r, std::string_view value,
                       bool selected, bool enabled) {
  const Vec2 t = current_translate();
  AccessNode n;
  n.id = wid;
  n.role = role;
  n.label = std::string(label);
  n.value = std::string(value);
  n.rect = r.offset(t.x, t.y).intersect(current_clip());
  n.focused = focused_ == wid;
  n.enabled = enabled;
  n.selected = selected;
  access_.push_back(std::move(n));
}

void Context::set_focus(WidgetId wid, bool visible) {
  focused_ = wid;
  focused_seen_ = true;  // assume it will be (or was) registered this frame
  if (visible) focus_visible_ = true;
}

void Context::tooltip(WidgetId wid, std::string_view text) {
  if (hot_ != wid || text.empty()) return;
  tooltip_id_ = wid;
  tooltip_text_ = std::string(text);
  for (const auto& h : hits_) {
    if (h.id == wid) tooltip_anchor_ = h.rect;
  }
}

bool Context::key_pressed(Key k, bool consume) {
  Modifiers m;
  return key_pressed(k, m, consume);
}

bool Context::key_pressed(Key k, Modifiers& mods_out, bool consume) {
  for (auto& kp : keys_) {
    if (!kp.consumed && kp.key == k) {
      if (consume) kp.consumed = true;
      mods_out = kp.mods;
      return true;
    }
  }
  return false;
}

bool Context::clicked_outside(const Rect& r) const {
  if (!mouse_pressed_) return false;
  const Vec2 t = current_translate();
  return !r.offset(t.x, t.y).contains(press_pos_);
}

void Context::block_below(Layer layer) {
  if (static_cast<int>(layer) > static_cast<int>(block_layer_)) block_layer_ = layer;
}

bool Context::hovering(const Rect& r) const {
  if (static_cast<int>(layer_) < static_cast<int>(prev_block_layer_)) return false;
  const Vec2 t = current_translate();
  return r.offset(t.x, t.y).intersect(current_clip()).contains(mouse_) && (mouse_inside_ || drag_over_);
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------

float Context::animate(WidgetId wid, float target, float duration, const CubicBezier& easing) {
  AnimState& a = anims_[wid];
  if (!a.initialised || reduced_motion) {
    a.initialised = true;
    a.tween.from = a.tween.to = target;
    a.tween.start_time = time_;
    a.tween.duration = 0.0f;
    return target;
  }
  if (target != a.tween.to) {
    a.tween.easing = easing;
    a.tween.retarget(target, time_, duration);
  }
  const float v = a.tween.value(time_);
  if (!a.tween.done(time_)) needs_frame_ = true;
  return v;
}

float Context::spring(WidgetId wid, float target, float stiffness, float damping) {
  AnimState& a = anims_[wid];
  if (!a.initialised || reduced_motion) {
    a.initialised = true;
    a.spring.snap(target);
    a.last_frame = frame_;
    return target;
  }
  a.spring.stiffness = stiffness;
  a.spring.damping = damping;
  a.spring.target = target;
  if (a.last_frame != frame_) {
    a.last_frame = frame_;
    if (a.spring.step(dt_)) needs_frame_ = true;
  } else if (!a.spring.settled()) {
    needs_frame_ = true;
  }
  return a.spring.value;
}

float Context::age(WidgetId wid) {
  auto it = first_seen_.find(wid);
  if (it == first_seen_.end()) it = first_seen_.emplace(wid, time_).first;
  return static_cast<float>(time_ - it->second);
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

float Context::begin_scroll(WidgetId sid, const Rect& viewport, float content_height) {
  ScrollState& st = scrolls_[sid];
  st.content = content_height;
  st.viewport = viewport.h;
  const float max_off = std::max(0.0f, content_height - viewport.h);
  const Vec2 t = current_translate();
  const Rect screen = viewport.offset(t.x, t.y).intersect(current_clip());

  // Wheel: the innermost scroll view under the mouse takes it.
  if (scroll_accum_.y != 0.0f && screen.contains(mouse_) &&
      static_cast<int>(layer_) >= static_cast<int>(prev_block_layer_) && max_off > 0.0f) {
    st.spring.target = clampf(st.spring.target + scroll_accum_.y, 0.0f, max_off);
    st.last_activity = time_;
    scroll_accum_ = {};
  }
  // Keep the keyboard-focused widget visible.
  if (focus_changed_by_key_ && focused_ != 0) {
    for (const auto& h : prev_hits_) {
      if (h.id != focused_) continue;
      // h.rect was clipped; use the unclipped focused rect when available.
      Rect fr = h.rect;
      if (fr.h <= 0.0f && focused_scroll_ == sid) fr = focused_rect_;
      if (screen.contains(fr.center()) && fr.y >= screen.y && fr.bottom() <= screen.bottom()) break;
      if (fr.y + fr.h * 0.5f < screen.y - 1000.0f || fr.y > screen.bottom() + 10000.0f) break;
      // Only adjust when the widget lies horizontally within this viewport.
      if (fr.center().x < screen.x || fr.center().x > screen.right()) break;
      const float margin = 16.0f;
      if (fr.y < screen.y) {
        st.spring.target = clampf(st.spring.value - (screen.y - fr.y) - margin, 0.0f, max_off);
      } else if (fr.bottom() > screen.bottom()) {
        st.spring.target = clampf(st.spring.value + (fr.bottom() - screen.bottom()) + margin, 0.0f, max_off);
      }
      st.last_activity = time_;
      break;
    }
  }
  st.spring.target = clampf(st.spring.target, 0.0f, max_off);
  if (st.spring.value > max_off) st.spring.value = max_off;
  if (st.spring.value < 0.0f) st.spring.value = 0.0f;
  st.spring.stiffness = 520.0f;
  st.spring.damping = 46.0f;
  if (reduced_motion) {
    st.spring.snap(st.spring.target);
  } else if (st.spring.step(dt_)) {
    needs_frame_ = true;
  }
  const float offset = std::round(st.spring.value);
  scroll_stack_.push_back({sid, viewport});
  push_clip(viewport);
  push_translate({0.0f, -offset});
  return offset;
}

void Context::end_scroll() {
  if (scroll_stack_.empty()) return;
  const ScrollFrame frame = scroll_stack_.back();
  scroll_stack_.pop_back();
  pop_translate();
  pop_clip();
  ScrollState& st = scrolls_[frame.id];
  const float max_off = std::max(0.0f, st.content - st.viewport);
  if (max_off <= 0.5f) return;
  const Rect& vp = frame.viewport;
  const float track_h = vp.h - 8.0f;
  const float thumb_h = std::max(28.0f, track_h * vp.h / st.content);
  const float thumb_y = vp.y + 4.0f + (track_h - thumb_h) * (st.spring.value / max_off);
  const Rect track{vp.right() - 12.0f, vp.y, 12.0f, vp.h};
  const Rect thumb{vp.right() - 9.0f, thumb_y, 6.0f, thumb_h};
  const WidgetId tid = hash_id("scrollbar", frame.id);
  const Interaction in = interact(tid, track, kNone);
  if (in.pressed) {
    if (!st.dragging_thumb) {
      st.dragging_thumb = true;
      st.drag_offset = mouse_.y - thumb_y;
      if (st.drag_offset < 0.0f || st.drag_offset > thumb_h) st.drag_offset = thumb_h * 0.5f;
    }
    const float frac = clampf((mouse_.y - st.drag_offset - vp.y - 4.0f) / std::max(1.0f, track_h - thumb_h), 0.0f, 1.0f);
    st.spring.snap(frac * max_off);
    st.last_activity = time_;
  } else {
    st.dragging_thumb = false;
  }
  const bool near = hovering(vp);
  const float recent = static_cast<float>(time_ - st.last_activity);
  const float target = (in.hovered || in.pressed) ? 1.0f : (recent < 0.9f || near ? 0.55f : 0.0f);
  if (recent < 0.9f) needs_frame_ = true;
  const float alpha = animate(hash_id("scrollbar-alpha", frame.id), target, theme_->motion.fast, theme_->motion.standard);
  const float width = animate(hash_id("scrollbar-w", frame.id), (in.hovered || in.pressed) ? 8.0f : 6.0f,
                              theme_->motion.fast, theme_->motion.standard);
  if (alpha > 0.01f) {
    const Rect th{vp.right() - 3.0f - width, thumb_y, width, thumb_h};
    fill(th, width * 0.5f, theme_->color.border_strong.fade(alpha));
  }
}

void Context::scroll_to(WidgetId sid, float offset) {
  ScrollState& st = scrolls_[sid];
  st.spring.target = std::max(0.0f, offset);
  st.last_activity = time_;
  needs_frame_ = true;
}

}  // namespace cvd::ui
