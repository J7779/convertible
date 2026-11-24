// Convertible desktop - scripted automation of the real app (used by the
// end-to-end tests and for screenshots). A script is a JSON array of steps:
//
//   [{"op": "open", "files": ["a.txt", "b.txt"]},
//    {"op": "setTarget", "index": 0, "target": "base64"},
//    {"op": "convertAll"}, {"op": "waitIdle"},
//    {"op": "saveAll", "dir": "out"},
//    {"op": "screenshot", "path": "queue.png"},
//    {"op": "dump", "path": "state.json"},
//    {"op": "expect", "path": "items.0.status", "equals": "done"}]
//
// Relative input paths resolve against the script's folder, relative output
// paths against the output directory. See apps/desktop/README.md for all ops.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "gfx/canvas.hpp"
#include "platform/events.hpp"

namespace cvd::app {

class App;
class ProgressGate;

class Automation {
 public:
  Automation(App& app, const std::string& script_path, const std::string& out_dir);
  ~Automation();
  Automation(const Automation&) = delete;
  Automation& operator=(const Automation&) = delete;

  void before_frame(double now);
  void after_frame(const gfx::Bitmap& frame);
  [[nodiscard]] bool finished() const { return done_; }
  [[nodiscard]] int failures() const { return failures_; }
  [[nodiscard]] const std::vector<std::string>& log() const { return log_; }

  /// Resolves a dotted path ("items.0.status") in a JSON value.
  static const Json* lookup(const Json& root, const std::string& path);
  /// What the seams recorded (dialogs, shell calls), progress samples, log.
  [[nodiscard]] Json dump() const;

 private:
  bool step(const Json& cmd, double now);  // true when the step completed
  std::string in_path(const std::string& p) const;
  std::string out_path(const std::string& p) const;
  bool click_label(const Json& cmd);
  bool check(const Json& cmd, const Json& state);
  void fail(const std::string& message);
  void write_log() const;

  App& app_;
  std::vector<Json> steps_;
  std::size_t index_ = 0;
  std::string script_dir_;
  std::string out_dir_;
  bool done_ = false;
  int failures_ = 0;
  std::vector<std::string> log_;
  // Waiting state.
  bool waiting_ = false;
  double wait_until_ = 0.0;
  int frames_left_ = 0;
  std::string pending_shot_;
  int idle_frames_ = 0;
  bool yield_ = false;
  bool trace_ = false;
  int failure_shots_ = 0;
  // Seams and the progress gate.
  std::shared_ptr<ProgressGate> gate_;
  std::deque<std::vector<std::string>> dialog_replies_;
  Json dialogs_ = Json::array();
  Json shell_ = Json::array();
  bool shell_result_ = true;
  std::map<int, std::vector<double>> samples_;
  int steps_done_ = 0;
  std::deque<std::vector<Event>> event_batches_;
  Json focus_trail_ = Json::array();
  double real_deadline_ = 0.0;
};

}  // namespace cvd::app
