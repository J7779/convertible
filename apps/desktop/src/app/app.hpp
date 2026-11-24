// Convertible desktop - the application: owns the view-model and the UI
// context, builds every screen each frame and talks to the platform layer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/file_browser.hpp"
#include "app/state.hpp"
#include "platform/platform.hpp"
#include "ui/ui.hpp"
#include "ui/widgets.hpp"

namespace cvd::app {

class Automation;

enum class Screen { Home, Formats, Settings, Licenses };

const char* screen_name(Screen s);

struct AppOptions {
  bool skip_intro = false;
  std::string config_dir;  // empty = platform default
  std::string temp_dir;    // empty = fresh session dir
  int workers = 0;
  std::vector<std::string> open_files;  // files from the command line / OS
  std::string automation_script;        // path to a JSON script
  std::string automation_out;           // output directory for screenshots/dumps
};

struct Toast {
  std::string message;
  std::string action_label;
  std::string action_path;  // revealed when the action is clicked
  double shown_at = 0.0;
  double duration = 4.5;
  bool error = false;
  std::uint64_t id = 0;
};

class App : public AppHandler {
 public:
  App(Platform& platform, AppOptions options);
  ~App() override;

  // AppHandler
  void on_event(const Event& e) override;
  void on_frame(gfx::Bitmap& target, float scale, double now) override;
  [[nodiscard]] bool wants_frame() const override;
  [[nodiscard]] double wake_time() const override;
  [[nodiscard]] CursorShape cursor() const override;
  [[nodiscard]] bool keep_running() const override { return running_; }
  [[nodiscard]] std::vector<AccessibleItem> accessibility() const override;
  [[nodiscard]] Color chrome_color() const override { return theme_.color.background; }
  [[nodiscard]] bool chrome_dark() const override { return theme_.dark; }

  // ---- Actions (used by the views, automation and tests) -----------------
  void add_files(const std::vector<std::string>& paths);
  void choose_files();
  void navigate(Screen s);
  [[nodiscard]] Screen screen() const { return screen_; }
  void open_picker(std::uint64_t item_id, const std::string& group_source = {});
  void close_picker();
  [[nodiscard]] bool picker_open() const { return picker_.open; }
  void select_target(const std::string& target);
  void convert_all();
  void save_item(std::uint64_t id);
  void save_all();
  void reveal_item(std::uint64_t id);
  void open_item(std::uint64_t id);
  void apply_settings(const Settings& s);
  void skip_intro();
  void quit() { running_ = false; }
  void toast(std::string message, std::string action_label = {}, std::string action_path = {}, bool error = false);

  [[nodiscard]] AppState& state() { return *state_; }
  [[nodiscard]] ui::Context& ui() { return ui_; }
  [[nodiscard]] Platform& platform() { return platform_; }
  [[nodiscard]] const ui::Theme& theme() const { return theme_; }
  [[nodiscard]] bool dark() const { return theme_.dark; }
  [[nodiscard]] float zoom() const { return static_cast<float>(state_->settings.text_scale_percent) / 100.0f; }
  [[nodiscard]] double now() const { return now_; }
  [[nodiscard]] bool intro_active() const { return intro_phase_ < 2; }
  /// Failed automation expectations (0 without automation).
  [[nodiscard]] int automation_failures() const;

  /// JSON snapshot of everything visible (automation / tests).
  [[nodiscard]] Json dump_state() const;

  std::string picker_query;  // search text of the open picker

  /// Test seams, installed by automation so end-to-end runs never open OS
  /// dialogs, file managers or other apps. Empty = use the platform.
  struct Seams {
    /// kind: "open" (files), "save" (one file) or "folder"; reply with the chosen paths (empty = cancelled).
    std::function<void(const std::string& kind, const std::string& title, const std::string& directory,
                       const std::string& suggested_name, std::function<void(std::vector<std::string>)> reply)>
        dialog;
    /// action: "reveal" or "open"; returns what the platform call would.
    std::function<bool(const std::string& action, const std::string& path)> shell;
  } seams;

 private:
  friend class Automation;

  void resolve_theme();
  void update_reduced_motion();
  void build();
  void build_top_bar(Rect r);
  void build_home(Rect r);
  void build_empty_state(Rect r);
  void build_queue(Rect r);
  float build_card(QueueItem& item, Rect r, int index, bool measure_only);
  float card_height(const QueueItem& item, float width) const;
  void build_card_options(QueueItem& item, Rect r);
  void build_group_bar(Rect r, const std::string& source);
  void build_action_bar(Rect r);
  void build_picker();
  void build_formats(Rect r);
  void build_settings(Rect r);
  void build_licenses(Rect r);
  void build_drop_overlay();
  void build_intro();
  void build_toasts();
  void handle_shortcuts();
  void persist_settings();
  bool reveal_path(const std::string& path);
  bool open_path(const std::string& path);

  Platform& platform_;
  AppOptions options_;
  std::unique_ptr<AppState> state_;
  ui::Context ui_;
  ui::Theme theme_;
  bool running_ = true;
  double now_ = 0.0;
  Vec2 logical_size_{1120.0f, 760.0f};
  float scale_ = 1.0f;
  Screen screen_ = Screen::Home;
  std::string config_dir_;
  bool system_dark_ = true;
  bool system_reduce_motion_ = false;

  // Intro: 0 playing, 1 fading out, 2 finished.
  int intro_phase_ = 0;
  double intro_start_ = -1.0;
  double intro_fade_start_ = 0.0;

  struct Picker {
    bool open = false;
    std::uint64_t item_id = 0;
    std::string group_source;  // non-empty: applies to every file of this format
    Rect anchor;
    int highlight = 0;
    double opened_at = 0.0;
    bool focus_search = false;
    Vec2 last_mouse{-1.0f, -1.0f};
  } picker_;

  struct Menu {
    bool open = false;
    ui::WidgetId owner = 0;
    Rect anchor;
    std::vector<std::string> choices;
    int selected = -1;
    int highlight = 0;
    std::function<void(int)> on_pick;
    double opened_at = 0.0;
  } menu_;
  void open_menu(ui::WidgetId owner, Rect anchor, std::vector<std::string> choices, int selected,
                 std::function<void(int)> on_pick);
  void build_menu();

  std::string formats_query_;
  std::string formats_category_;
  std::deque<Toast> toasts_;
  std::uint64_t next_toast_id_ = 1;
  std::vector<std::uint64_t> completed_seen_;
  std::vector<std::pair<std::uint64_t, double>> completion_times_;
  std::unique_ptr<FileBrowser> browser_;
  std::unique_ptr<Automation> automation_;
  bool drag_active_ = false;
  int formats_matched_ = 0;
  text::GlyphStats glyphs_last_frame_;
  std::string last_save_dir_;
};

}  // namespace cvd::app
