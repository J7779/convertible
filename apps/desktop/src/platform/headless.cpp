// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Headless platform: renders into an off-screen bitmap on a virtual clock.
// Used by --headless automation runs and by the unit tests.
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>

#include "platform/fileio.hpp"
#include "platform/platform.hpp"

namespace cvd {

namespace {

class HeadlessPlatform final : public Platform {
 public:
  explicit HeadlessPlatform(const PlatformOptions& o)
      : width_(o.width), height_(o.height), scale_(o.scale > 0.0f ? o.scale : 1.0f) {}

  bool create_window(const std::string&, int width, int height) override {
    width_ = width;
    height_ = height;
    return true;
  }

  int run(AppHandler& handler) override {
    // Virtual 60 Hz clock; a hard frame cap guards against runaway scripts.
    const int max_frames = 60 * 60 * 10;
    for (int frame = 0; frame < max_frames && !quit_; ++frame) {
      drain();
      bitmap_.resize(static_cast<int>(std::lround(static_cast<float>(width_) * scale_)),
                     static_cast<int>(std::lround(static_cast<float>(height_) * scale_)));
      handler.on_frame(bitmap_, scale_, time_);
      if (!handler.keep_running()) break;
      // Frame n is at exactly n/60 s (no accumulated rounding), so timed
      // automation steps land on the same frame on every machine.
      time_ = static_cast<double>(frame + 1) / 60.0;
    }
    return 0;
  }

  void request_frame() override {}
  void post(std::function<void()> fn) override {
    std::lock_guard lock(mutex_);
    posted_.push_back(std::move(fn));
  }
  void quit() override { quit_ = true; }
  [[nodiscard]] double now() const override { return time_; }
  [[nodiscard]] float scale() const override { return scale_; }
  [[nodiscard]] Vec2 window_size() const override {
    return {static_cast<float>(width_), static_cast<float>(height_)};
  }
  void set_title(const std::string&) override {}
  [[nodiscard]] bool system_dark_mode() const override { return true; }
  [[nodiscard]] bool system_reduce_motion() const override { return false; }
  [[nodiscard]] std::string clipboard_text() const override { return clipboard_; }
  void set_clipboard_text(const std::string& text) override { clipboard_ = text; }
  [[nodiscard]] bool has_native_dialogs() const override { return false; }
  void open_files_dialog(const std::string&, PathsCallback done) override { done({}); }
  void save_file_dialog(const std::string&, const std::string&, const std::string&, PathCallback done) override {
    done(std::nullopt);
  }
  void choose_folder_dialog(const std::string&, const std::string&, PathCallback done) override { done(std::nullopt); }
  bool reveal_in_file_manager(const std::string&) override { return true; }
  bool open_with_default_app(const std::string&) override { return true; }
  [[nodiscard]] std::string file_manager_name() const override { return "folder"; }
  [[nodiscard]] std::string config_directory() const override {
    return fileio::join(fileio::from_path(std::filesystem::temp_directory_path()), "convertible-headless-config");
  }
  [[nodiscard]] std::string documents_directory() const override { return fileio::home_directory(); }
  bool set_automation_size(int width, int height, float scale) override {
    width_ = std::max(200, width);
    height_ = std::max(200, height);
    if (scale > 0.0f) scale_ = scale;
    return true;
  }

 private:
  void drain() {
    std::deque<std::function<void()>> work;
    {
      std::lock_guard lock(mutex_);
      work.swap(posted_);
    }
    for (auto& fn : work) fn();
  }

  int width_;
  int height_;
  float scale_;
  double time_ = 0.0;
  bool quit_ = false;
  gfx::Bitmap bitmap_;
  std::string clipboard_;
  std::mutex mutex_;
  std::deque<std::function<void()>> posted_;
};

}  // namespace

std::unique_ptr<Platform> create_headless_platform(const PlatformOptions& options) {
  return std::make_unique<HeadlessPlatform>(options);
}

}  // namespace cvd
