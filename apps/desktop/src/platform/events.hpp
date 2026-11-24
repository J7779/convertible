// Convertible desktop - platform-neutral input events.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <vector>

#include "base/geometry.hpp"

namespace cvd {

enum class Key {
  None,
  Tab,
  Enter,
  Escape,
  Space,
  Backspace,
  Delete,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  PageUp,
  PageDown,
  A,
  C,
  F,
  N,
  O,
  Q,
  S,
  V,
  W,
  X,
  Z,
  Comma,
  Plus,   // also '=' on US layouts
  Minus,
  Digit0,
  Digit1,
  Digit2,
  Digit3,
  F1,
  Other
};

struct Modifiers {
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  bool super = false;  // Command on macOS, Windows key elsewhere

  /// The platform's primary shortcut modifier (Command on macOS, Ctrl elsewhere).
  [[nodiscard]] bool primary() const {
#if defined(__APPLE__)
    return super;
#else
    return ctrl;
#endif
  }
  [[nodiscard]] bool any() const { return shift || ctrl || alt || super; }
};

enum class EventType {
  MouseMove,
  MouseDown,
  MouseUp,
  MouseLeave,
  Scroll,
  KeyDown,
  KeyUp,
  Text,
  DragEnter,
  DragOver,
  DragLeave,
  Drop,
  OpenFiles,  // files handed to the app by the OS (open-with, command line)
  Resize,
  AppearanceChanged,
  CloseRequested,
  FocusLost
};

enum class MouseButton { Left = 0, Right = 1, Middle = 2 };

struct Event {
  EventType type = EventType::MouseMove;
  Vec2 pos{};               // logical (point) coordinates
  MouseButton button = MouseButton::Left;
  int clicks = 1;           // 2 for double click
  Vec2 scroll{};            // logical px, positive = content moves up/left
  bool precise = false;     // trackpad (pixel) scrolling
  Key key = Key::None;
  Modifiers mods;
  bool repeat = false;
  std::string text;                // UTF-8 for Text
  std::vector<std::string> paths;  // UTF-8 paths for Drop / OpenFiles
};

}  // namespace cvd
