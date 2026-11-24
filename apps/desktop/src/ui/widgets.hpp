// Convertible desktop - standard widgets on top of ui::Context.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ui/ui.hpp"

namespace cvd::ui {

enum class Icon {
  None,
  Plus,
  Close,
  ChevronDown,
  ChevronRight,
  ChevronLeft,
  ChevronUp,
  Check,
  Search,
  File,
  Folder,
  Save,
  Reveal,
  Refresh,
  Settings,
  Grid,
  Warning,
  Info,
  Shield,
  Compress,
  Trash,
  ArrowRight,
  Stop,
  Home,
  Sun,
  Moon,
  Monitor,
  Up,
};

/// Draws a 24-unit line icon scaled into `r` (recorded).
void icon(Context& ctx, Icon which, const Rect& r, Color color, float stroke = 1.75f);
/// Draws an icon directly onto a canvas.
void draw_icon(gfx::Canvas& canvas, Icon which, const Rect& r, Color color, float stroke = 1.75f);

enum class ButtonKind { Primary, Secondary, Ghost, Danger, Link };

struct ButtonOptions {
  ButtonKind kind = ButtonKind::Secondary;
  Icon icon = Icon::None;
  Icon trailing = Icon::None;
  bool enabled = true;
  std::string_view tooltip;
  const text::Style* style = nullptr;  // default: theme bodyStrong / label
  float radius = -1.0f;                // default: theme radius_md
  bool selected = false;               // ghost/tab buttons: draw as current
};

/// Preferred width of a button with the given label and options.
float button_width(Context& ctx, std::string_view label, const ButtonOptions& opt);
bool button(Context& ctx, WidgetId id, const Rect& r, std::string_view label, const ButtonOptions& opt = {});
bool icon_button(Context& ctx, WidgetId id, const Rect& r, Icon which, std::string_view label, bool enabled = true,
                 Color tint = {0, 0, 0, 0});

/// Segmented control; returns true when the selection changed.
bool segmented(Context& ctx, WidgetId id, const Rect& r, std::span<const std::string> options, int& selected,
               std::string_view label);

/// On/off switch (track only); returns true when toggled.
bool toggle(Context& ctx, WidgetId id, const Rect& r, bool& value, std::string_view label);

/// Horizontal slider; returns true while the value changes.
bool slider(Context& ctx, WidgetId id, const Rect& r, double& value, double min, double max, double step,
            std::string_view label);

struct TextFieldOptions {
  std::string_view placeholder;
  Icon leading = Icon::None;
  bool monospace = false;
  bool clear_button = false;
  bool select_all_on_focus = false;
};
/// Single-line text input; returns true when the text changed.
bool text_field(Context& ctx, WidgetId id, const Rect& r, std::string& value, std::string_view label,
                const TextFieldOptions& opt = {});

/// Disclosure header with an animated chevron; returns true when toggled.
bool disclosure(Context& ctx, WidgetId id, const Rect& r, std::string_view label, bool& open);

/// Format chip: tinted pill with a round (source) or square (target) dot.
float chip_width(Context& ctx, std::string_view label);
void chip(Context& ctx, const Rect& r, std::string_view label, Color tint, bool square_dot);

/// Panel for popovers and sheets: shadow, surface, border; also blocks hover below it.
void panel(Context& ctx, WidgetId id, const Rect& r, float radius, int elevation);

/// Draws wrapped text inside a width; returns the height used.
float paragraph(Context& ctx, std::string_view text, const text::Style& style, const Rect& r, Color c,
                int max_lines = 0);

}  // namespace cvd::ui
