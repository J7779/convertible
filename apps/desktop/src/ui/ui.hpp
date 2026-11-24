// Convertible desktop - the in-house immediate-mode UI toolkit.
//
// Every frame the views describe the whole interface: widgets register their
// rectangle (for hit-testing, focus order and the accessibility tree) and
// append drawing commands to a per-layer draw list. Interaction state (hover,
// press, focus, text editing, animations, scroll positions) is retained
// between frames, keyed by stable widget ids. Nothing here touches the OS:
// the platform layer feeds events in and presents the rendered bitmap.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/geometry.hpp"
#include "gfx/canvas.hpp"
#include "platform/events.hpp"
#include "text/text.hpp"
#include "ui/anim.hpp"
#include "ui/theme.hpp"

namespace cvd::ui {

using WidgetId = std::uint64_t;

WidgetId hash_id(std::string_view text, WidgetId seed = 1469598103934665603ULL);

enum class Layer { Base = 0, Overlay = 1, Modal = 2, Tooltip = 3 };
inline constexpr int kLayerCount = 4;

enum class Cursor { Arrow, Hand, Text, ResizeH };

enum class Role { Button, Toggle, Slider, TextField, Tab, MenuItem, ListItem, Label, Group, Image, Progress, Link };

struct AccessNode {
  WidgetId id = 0;
  Role role = Role::Label;
  std::string label;
  std::string value;
  Rect rect;       // on screen, clipped to the enclosing clip/scroll rectangle
  Rect full;       // unclipped (to tell scrolled-away from cut-off controls)
  bool scrolled = false;  // inside a scroll view
  Layer layer = Layer::Base;
  bool focused = false;
  bool enabled = true;
  bool selected = false;
};

/// Hooks the toolkit needs from the host (all optional).
struct Host {
  std::function<std::string()> get_clipboard;
  std::function<void(const std::string&)> set_clipboard;
};

struct Interaction {
  bool hovered = false;
  bool pressed = false;   // mouse held down on it
  bool clicked = false;   // activated by mouse release or keyboard
  bool focused = false;
  bool double_clicked = false;
  bool right_clicked = false;
};

enum InteractFlags : unsigned {
  kNone = 0,
  kFocusable = 1u << 0,
  kDisabled = 1u << 1,
  kKeyboardActivate = 1u << 2,  // Enter/Space activate when focused
  kNoHover = 1u << 3,
};

/// A draw command recorded during the frame and replayed at render time.
using DrawCommand = std::function<void(gfx::Canvas&)>;

class Context {
 public:
  Context();

  // ---- Frame ---------------------------------------------------------------
  /// Feeds an input event. Returns true when it should trigger a redraw.
  bool handle_event(const Event& e);
  void begin_frame(Vec2 size, double time_seconds, const Theme* theme);
  void end_frame();
  /// Replays all layers onto the canvas (which should already carry the DPI scale).
  void render(gfx::Canvas& canvas);
  /// True while an animation, tween or pending interaction needs more frames.
  [[nodiscard]] bool needs_frame() const { return needs_frame_; }
  void request_frame() { needs_frame_ = true; }
  /// Asks for a frame at a later time (caret blink, tooltip delay); 0 = none.
  void request_frame_at(double t) {
    if (wake_at_ == 0.0 || t < wake_at_) wake_at_ = t;
  }
  [[nodiscard]] double wake_at() const { return wake_at_; }

  [[nodiscard]] const Theme& theme() const { return *theme_; }
  [[nodiscard]] double time() const { return time_; }
  [[nodiscard]] float dt() const { return dt_; }
  [[nodiscard]] Vec2 size() const { return size_; }
  [[nodiscard]] Vec2 mouse() const { return mouse_; }
  [[nodiscard]] bool mouse_inside() const { return mouse_inside_; }
  [[nodiscard]] Cursor cursor() const { return cursor_; }
  void set_cursor(Cursor c) { cursor_ = c; }
  Host& host() { return host_; }

  bool reduced_motion = false;

  // ---- Ids -----------------------------------------------------------------
  WidgetId id(std::string_view name) const;
  WidgetId id(std::string_view name, std::uint64_t index) const;
  void push_id(std::string_view name);
  void push_id(std::uint64_t index);
  void pop_id();

  // ---- Drawing -------------------------------------------------------------
  void set_layer(Layer layer) { layer_ = layer; }
  [[nodiscard]] Layer layer() const { return layer_; }
  void draw(DrawCommand cmd);
  void push_clip(const Rect& r);
  void pop_clip();
  [[nodiscard]] Rect current_clip() const;
  void push_opacity(float alpha);
  void pop_opacity();
  void push_translate(Vec2 d);
  void pop_translate();
  [[nodiscard]] Vec2 current_translate() const;

  // Convenience drawing helpers (recorded, theme-aware).
  void fill(const Rect& r, float radius, Color c);
  void fill_paint(const Rect& r, float radius, gfx::Paint paint);
  void stroke(const Rect& r, float radius, float width, Color c);
  void shadow(const Rect& r, float radius, int elevation);
  void text(std::string_view s, const text::Style& style, Vec2 baseline, Color c);
  /// Single line vertically centred in `r`; align 0 = left, 0.5 = centre, 1 = right.
  void text_in(std::string_view s, const text::Style& style, const Rect& r, Color c, float align = 0.0f);
  /// Focus indicator for widget `id` (only while it has keyboard focus and focus is visible).
  void focus_ring(WidgetId id, const Rect& r, float radius);

  // ---- Interaction ---------------------------------------------------------
  Interaction interact(WidgetId id, const Rect& r, unsigned flags = kFocusable | kKeyboardActivate);
  /// Registers an accessibility node for the current frame.
  void describe(WidgetId id, Role role, std::string_view label, const Rect& r, std::string_view value = {},
                bool selected = false, bool enabled = true);
  [[nodiscard]] const std::vector<AccessNode>& access_nodes() const { return access_prev_; }
  /// Focusable widgets of the last frame that registered no accessibility node
  /// (their on-screen rectangles); empty when every control is described.
  [[nodiscard]] std::vector<Rect> undescribed_focusables() const;
  [[nodiscard]] std::size_t focusable_count() const { return prev_focus_order_.size(); }

  [[nodiscard]] WidgetId hot() const { return hot_; }
  [[nodiscard]] WidgetId active() const { return active_; }
  [[nodiscard]] WidgetId focused() const { return focused_; }
  [[nodiscard]] bool focus_visible() const { return focus_visible_; }
  void set_focus(WidgetId id, bool visible = false);
  void clear_focus() { focused_ = 0; }
  /// Tooltip for the hovered widget, shown after a short delay.
  void tooltip(WidgetId id, std::string_view text);

  struct KeyPress {
    Key key;
    Modifiers mods;
    bool consumed;
  };
  /// This frame's key presses in order (widgets mark the ones they handle).
  std::vector<KeyPress>& key_presses() { return keys_; }
  /// Keys pressed this frame that no widget consumed yet.
  bool key_pressed(Key k, bool consume = true);
  bool key_pressed(Key k, Modifiers& mods_out, bool consume = true);
  [[nodiscard]] const Modifiers& mods() const { return mods_; }
  [[nodiscard]] bool mouse_pressed_this_frame() const { return mouse_pressed_; }
  [[nodiscard]] bool mouse_down() const { return mouse_down_; }
  [[nodiscard]] Vec2 mouse_press_pos() const { return press_pos_; }
  /// A click anywhere this frame that was not on any widget of the given layer or above.
  [[nodiscard]] bool clicked_outside(const Rect& r) const;
  /// Blocks hover/clicks for layers below `layer` (modal sheets).
  void block_below(Layer layer);
  /// True when the mouse is over `r` and not covered by a higher layer.
  [[nodiscard]] bool hovering(const Rect& r) const;

  // ---- Animation -----------------------------------------------------------
  /// Eased tween towards `target`, keyed by id; returns the current value.
  float animate(WidgetId id, float target, float duration, const CubicBezier& easing);
  float animate(WidgetId id, float target) { return animate(id, target, theme_->motion.base, theme_->motion.standard); }
  /// Spring towards `target`.
  float spring(WidgetId id, float target, float stiffness = 380.0f, float damping = 34.0f);
  /// Seconds since `id` was first seen (for entrance animations).
  float age(WidgetId id);

  // ---- Scroll views --------------------------------------------------------
  /// Begins a vertical scroll region; content is drawn translated by -offset.
  /// Returns the scroll offset to apply to content layout.
  float begin_scroll(WidgetId id, const Rect& viewport, float content_height);
  void end_scroll();
  void scroll_to(WidgetId id, float offset);

  // ---- Text editing --------------------------------------------------------
  struct TextEdit {
    std::size_t caret = 0;
    std::size_t anchor = 0;
    float scroll_x = 0.0f;
    double blink_start = 0.0;
  };
  TextEdit& text_edit(WidgetId id) { return edits_[id]; }
  /// Text typed this frame (consumed by the focused text field).
  [[nodiscard]] const std::string& typed_text() const { return typed_; }
  void consume_typed() { typed_.clear(); }

  // ---- Drag and drop -------------------------------------------------------
  [[nodiscard]] bool drag_hovering() const { return drag_over_; }
  [[nodiscard]] Vec2 drag_pos() const { return drag_pos_; }

  // ---- Per-widget generic state ---------------------------------------------
  template <typename T>
  T& state(WidgetId id) {
    auto& slot = states_[id];
    if (!slot) slot = std::make_shared<Holder<T>>();
    return static_cast<Holder<T>*>(slot.get())->value;
  }

  /// Number of frames rendered so far.
  [[nodiscard]] std::uint64_t frame_index() const { return frame_; }

 private:
  struct HoldBase {
    virtual ~HoldBase() = default;
  };
  template <typename T>
  struct Holder : HoldBase {
    T value{};
  };

  struct HitBox {
    WidgetId id;
    Rect rect;
    Layer layer;
    unsigned flags;
  };
  static WidgetId pick(const std::vector<HitBox>& hits, Layer block, Vec2 m);

  struct ScrollState {
    Spring spring;
    float content = 0.0f;
    float viewport = 0.0f;
    double last_activity = -10.0;
    bool dragging_thumb = false;
    float drag_offset = 0.0f;
  };
  struct ScrollFrame {
    WidgetId id;
    Rect viewport;
  };
  struct AnimState {
    Tween tween;
    Spring spring;
    bool initialised = false;
    std::uint64_t last_frame = 0;
  };

  const Theme* theme_ = nullptr;
  Host host_;
  Vec2 size_{};
  double time_ = 0.0;
  float dt_ = 0.0f;
  std::uint64_t frame_ = 0;
  bool needs_frame_ = true;
  double wake_at_ = 0.0;

  // Input accumulated between frames.
  Vec2 mouse_{-1000.0f, -1000.0f};
  bool mouse_inside_ = false;
  bool mouse_down_ = false;
  bool mouse_pressed_ = false;
  bool mouse_released_ = false;
  bool right_pressed_ = false;
  int press_clicks_ = 1;
  Vec2 press_pos_{};
  Vec2 scroll_accum_{};
  std::vector<KeyPress> keys_;
  std::vector<KeyPress> pending_keys_;
  std::string typed_;
  std::string pending_typed_;
  Modifiers mods_;
  bool pending_press_ = false;
  bool pending_release_ = false;
  bool pending_right_ = false;
  int pending_clicks_ = 1;
  Vec2 pending_scroll_{};
  bool drag_over_ = false;
  Vec2 drag_pos_{};

  // Widget state.
  WidgetId hot_ = 0;
  WidgetId active_ = 0;
  WidgetId focused_ = 0;
  bool focus_visible_ = false;
  bool focus_changed_by_key_ = false;
  WidgetId hot_scroll_ = 0;
  WidgetId tooltip_id_ = 0;
  double hover_start_ = 0.0;
  WidgetId hover_tracked_ = 0;
  Layer block_layer_ = Layer::Base;
  Layer prev_block_layer_ = Layer::Base;
  std::vector<HitBox> hits_;
  std::vector<HitBox> prev_hits_;
  std::vector<WidgetId> focus_order_;
  std::vector<WidgetId> prev_focus_order_;
  std::vector<AccessNode> access_;
  std::vector<AccessNode> access_prev_;
  bool focused_seen_ = false;
  std::vector<std::uint64_t> id_stack_;
  std::unordered_map<WidgetId, ScrollState> scrolls_;
  std::vector<ScrollFrame> scroll_stack_;
  std::unordered_map<WidgetId, AnimState> anims_;
  std::unordered_map<WidgetId, double> first_seen_;
  std::unordered_map<WidgetId, TextEdit> edits_;
  std::unordered_map<WidgetId, std::shared_ptr<HoldBase>> states_;
  Rect focused_rect_{};
  WidgetId focused_scroll_ = 0;

  // Drawing.
  Layer layer_ = Layer::Base;
  std::vector<DrawCommand> layers_[kLayerCount];
  std::vector<Rect> clip_stack_[kLayerCount];
  std::vector<Vec2> translate_stack_[kLayerCount];
  Cursor cursor_ = Cursor::Arrow;
  std::string tooltip_text_;
  Rect tooltip_anchor_{};
};

/// Rect-cut layout helpers: carve pieces off the edges of a rectangle.
inline Rect cut_top(Rect& r, float h) {
  const Rect out{r.x, r.y, r.w, std::min(h, r.h)};
  r.y += out.h;
  r.h -= out.h;
  return out;
}
inline Rect cut_bottom(Rect& r, float h) {
  const Rect out{r.x, r.bottom() - std::min(h, r.h), r.w, std::min(h, r.h)};
  r.h -= out.h;
  return out;
}
inline Rect cut_left(Rect& r, float w) {
  const Rect out{r.x, r.y, std::min(w, r.w), r.h};
  r.x += out.w;
  r.w -= out.w;
  return out;
}
inline Rect cut_right(Rect& r, float w) {
  const Rect out{r.right() - std::min(w, r.w), r.y, std::min(w, r.w), r.h};
  r.w -= out.w;
  return out;
}
inline Rect centered(const Rect& r, float w, float h) {
  return {r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, w, h};
}

}  // namespace cvd::ui
