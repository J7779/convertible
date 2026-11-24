// Convertible desktop - the one interface every OS layer implements
// (Cocoa, Win32, X11) plus the headless implementation used for automation
// and tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/geometry.hpp"
#include "gfx/canvas.hpp"
#include "platform/events.hpp"

namespace cvd {

enum class CursorShape { Arrow, Hand, Text, ResizeH };

/// One element of the accessibility tree exposed to the OS screen reader.
struct AccessibleItem {
  enum class Role { Button, Toggle, Slider, TextField, Tab, MenuItem, ListItem, Label, Group, Image, Progress, Link };
  Role role = Role::Label;
  std::string label;
  std::string value;
  Rect rect;  // logical window coordinates
  bool focused = false;
  bool selected = false;
  bool enabled = true;
};

/// What the OS layer drives: it forwards events, asks for frames and presents
/// the returned bitmap.
class AppHandler {
 public:
  virtual ~AppHandler() = default;
  /// Input and window events (coordinates in logical points).
  virtual void on_event(const Event& e) = 0;
  /// Renders a frame into `target` (device pixels = logical size * scale).
  virtual void on_frame(gfx::Bitmap& target, float scale, double now_seconds) = 0;
  /// True when another frame should follow as soon as possible (animation).
  [[nodiscard]] virtual bool wants_frame() const = 0;
  /// A time (seconds, same clock as now()) at which a frame is wanted; 0 for none.
  [[nodiscard]] virtual double wake_time() const = 0;
  /// The cursor the UI wants.
  [[nodiscard]] virtual CursorShape cursor() const = 0;
  /// Called once per frame batch; returning false asks the loop to quit.
  [[nodiscard]] virtual bool keep_running() const = 0;
  /// The current accessibility tree (rebuilt every frame).
  [[nodiscard]] virtual std::vector<AccessibleItem> accessibility() const { return {}; }
  /// Window chrome colour matching the UI background, and whether it is dark.
  [[nodiscard]] virtual Color chrome_color() const { return {0.05f, 0.06f, 0.09f, 1.0f}; }
  [[nodiscard]] virtual bool chrome_dark() const { return true; }
};

struct FileFilter {
  std::string name;
  std::vector<std::string> extensions;  // without dots; empty = all files
};

using PathsCallback = std::function<void(std::vector<std::string>)>;
using PathCallback = std::function<void(std::optional<std::string>)>;

class Platform {
 public:
  virtual ~Platform() = default;

  /// Creates the main window (hidden platforms ignore it). Size in logical points.
  virtual bool create_window(const std::string& title, int width, int height) = 0;
  /// Runs the event loop until the window closes or the handler stops; returns the exit code.
  virtual int run(AppHandler& handler) = 0;
  /// Thread-safe: wakes the loop so it renders soon.
  virtual void request_frame() = 0;
  /// Thread-safe: runs `fn` on the UI thread.
  virtual void post(std::function<void()> fn) = 0;
  virtual void quit() = 0;

  [[nodiscard]] virtual double now() const = 0;
  [[nodiscard]] virtual float scale() const = 0;
  [[nodiscard]] virtual Vec2 window_size() const = 0;  // logical
  virtual void set_title(const std::string& title) = 0;

  // System appearance and accessibility.
  [[nodiscard]] virtual bool system_dark_mode() const = 0;
  [[nodiscard]] virtual bool system_reduce_motion() const = 0;

  // Clipboard (UTF-8 text).
  [[nodiscard]] virtual std::string clipboard_text() const = 0;
  virtual void set_clipboard_text(const std::string& text) = 0;

  // Native dialogs. Platforms without them return false from has_native_dialogs()
  // and the app shows its own file browser instead.
  [[nodiscard]] virtual bool has_native_dialogs() const = 0;
  virtual void open_files_dialog(const std::string& title, PathsCallback done) = 0;
  virtual void save_file_dialog(const std::string& title, const std::string& directory,
                                const std::string& suggested_name, PathCallback done) = 0;
  virtual void choose_folder_dialog(const std::string& title, const std::string& directory, PathCallback done) = 0;

  // Shell integration.
  virtual bool reveal_in_file_manager(const std::string& path) = 0;
  virtual bool open_with_default_app(const std::string& path) = 0;
  [[nodiscard]] virtual std::string file_manager_name() const = 0;

  /// Automation only: changes the (virtual) window size and scale; false when unsupported.
  virtual bool set_automation_size(int width, int height, float scale) {
    (void)width;
    (void)height;
    (void)scale;
    return false;
  }

  /// Per-user configuration directory for settings (created on demand).
  [[nodiscard]] virtual std::string config_directory() const = 0;
  /// Default folder suggestions for saving.
  [[nodiscard]] virtual std::string documents_directory() const = 0;
};

struct PlatformOptions {
  bool headless = false;
  int width = 1120;
  int height = 760;
  float scale = 0.0f;  // 0 = from the display
};

/// The platform layer of the current OS (or the headless one).
std::unique_ptr<Platform> create_platform(const PlatformOptions& options);
/// The headless implementation (all OSes).
std::unique_ptr<Platform> create_headless_platform(const PlatformOptions& options);

}  // namespace cvd
