// Convertible desktop - view-model: the conversion queue, per-file targets
// and options, background jobs and user settings. No UI or OS code here, so
// all of it is unit-tested headlessly.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/binding.hpp"
#include "base/json.hpp"

namespace cvd::app {

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

enum class ThemeMode { System = 0, Dark = 1, Light = 2 };
enum class MotionMode { System = 0, Reduced = 1, Full = 2 };

struct Settings {
  ThemeMode theme = ThemeMode::System;
  int default_preset = 1;  // 0 high, 1 balanced, 2 small
  bool strip_metadata = true;
  MotionMode motion = MotionMode::System;
  int text_scale_percent = 100;  // 100, 115, 130

  [[nodiscard]] Json to_json() const;
  static Settings from_json(const Json& j);
  bool operator==(const Settings&) const = default;
};

/// Reads/writes settings.json in the given directory; missing or bad files give defaults.
Settings load_settings(const std::string& directory);
bool save_settings(const std::string& directory, const Settings& settings);

/// The schema's preset values in High/Balanced/Small order.
inline const std::vector<std::string>& preset_values() {
  static const std::vector<std::string> v = {"high", "balanced", "small"};
  return v;
}

// ---------------------------------------------------------------------------
// Option values
// ---------------------------------------------------------------------------

/// Default values for a schema, with the user's defaults applied to the shared keys.
Json default_option_values(const std::vector<OptionSpec>& specs, const Settings& settings);
/// Coerces and clamps a value to the spec (int rounding, min/max, enum membership).
Json coerce_option(const OptionSpec& spec, const Json& value);
/// True when the schema offers the preset enum with all three standard values.
bool has_preset(const std::vector<OptionSpec>& specs);
/// Index into preset_values() of the current value, -1 when not applicable.
int preset_index(const Json& values);

// ---------------------------------------------------------------------------
// Queue
// ---------------------------------------------------------------------------

enum class ItemStatus { Ready, Queued, Converting, Done, Failed, Cancelled };

const char* status_name(ItemStatus s);

struct QueueItem {
  std::uint64_t id = 0;
  std::string path;
  std::string name;
  std::uint64_t size = 0;
  std::string source;        // detected format id ("" when unknown)
  std::string source_label;  // e.g. "TXT"
  std::string category = "other";
  std::vector<TargetInfo> targets;
  std::string target;
  std::vector<OptionSpec> options;
  Json values = Json::object();
  bool advanced_open = false;

  ItemStatus status = ItemStatus::Ready;
  float progress = 0.0f;
  std::string stage;
  double started_at = 0.0;

  // Result.
  std::string output_path;  // temporary file holding the result
  std::string output_name;  // suggested file name
  std::string saved_path;   // where the user saved it
  std::uint64_t output_size = 0;
  int error_status = 0;
  std::string error_message;
  std::vector<std::string> warnings;

  [[nodiscard]] const TargetInfo* target_info() const;
  /// The format id handed to the core: unknown files are treated as raw bytes ("bin").
  [[nodiscard]] std::string effective_source() const { return source.empty() ? std::string("bin") : source; }
  [[nodiscard]] bool is_compress() const;
  [[nodiscard]] bool busy() const { return status == ItemStatus::Queued || status == ItemStatus::Converting; }
};

/// Picks the default target: the first non-compress target the core suggests.
std::string default_target(const std::vector<TargetInfo>& targets);

/// Ordering and grouping for the picker: entries in the source's category
/// first, then by the core's relevance order; groups follow first appearance.
struct TargetGroup {
  std::string category;
  std::vector<const TargetInfo*> targets;
};
std::vector<TargetGroup> group_targets(const std::vector<TargetInfo>& targets, const std::string& source_category,
                                       std::string_view query);
/// Search match: id, name, extensions (case-insensitive substring).
bool target_matches(const TargetInfo& t, std::string_view query);
/// Archive targets usable as "Compress into…" for any file.
std::vector<const TargetInfo*> archive_targets(const std::vector<TargetInfo>& targets);

/// Output file name: stem + "." + extension; " (compressed)" when the format is unchanged.
std::string output_file_name(const std::string& source_name, const std::string& extension, bool same_format);

/// A conversion request handed to the job runner.
struct Job {
  std::uint64_t item_id = 0;
  ConvertRequest request;
};

/// Shared progress for one running job (written by the worker, read by the UI).
struct JobProgress {
  std::atomic<float> fraction{0.0f};
  std::atomic<bool> cancel{false};
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  std::mutex mutex;
  std::string stage;
  ConvertOutcome outcome;
};

/// Test seam (automation only): parks workers inside the core's progress
/// callback so tests decide when a conversion advances, instead of racing it.
/// While held, every progress report waits for a permit (one report each),
/// for the job to be cancelled, or for the gate to open.
class ProgressGate {
 public:
  void hold();
  void open();
  /// Lets one parked (or the next) progress report through.
  void permit();
  /// Called by workers on every progress report.
  void pass(const std::atomic<bool>& cancel);
  /// Wakes parked workers so they re-check their cancel flag.
  void notify();
  /// Waits (up to `timeout_ms` of real time) until `pred()` holds; the
  /// predicate is re-checked whenever a worker parks, passes or finishes.
  bool wait(const std::function<bool()>& pred, int timeout_ms);
  [[nodiscard]] int parked() const;
  [[nodiscard]] bool held() const;
  [[nodiscard]] int permits() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool hold_ = false;
  int permits_ = 0;
  int parked_ = 0;
};

/// A small pool of worker threads running conversions through the core.
class JobRunner {
 public:
  explicit JobRunner(int workers = 0, std::function<void()> wake = {});
  ~JobRunner();
  JobRunner(const JobRunner&) = delete;
  JobRunner& operator=(const JobRunner&) = delete;

  std::shared_ptr<JobProgress> submit(Job job);
  /// Blocks until all submitted jobs have finished (tests and automation).
  void wait_idle();
  [[nodiscard]] int worker_count() const { return static_cast<int>(threads_.size()); }
  /// Jobs queued or running.
  [[nodiscard]] int pending() const;
  /// Installs the automation gate (before any job is submitted).
  void set_gate(std::shared_ptr<ProgressGate> gate);

 private:
  void worker_loop(std::stop_token stop);

  std::shared_ptr<ProgressGate> gate_;
  std::function<void()> wake_;
  mutable std::mutex mutex_;
  std::condition_variable_any cv_;
  std::condition_variable_any idle_cv_;
  std::deque<std::pair<Job, std::shared_ptr<JobProgress>>> queue_;
  int running_ = 0;
  std::vector<std::jthread> threads_;
};

/// The whole queue and its operations.
class AppState {
 public:
  explicit AppState(std::string temp_dir, std::function<void()> wake = {}, int workers = 0);
  ~AppState();

  Settings settings;

  // Files.
  /// Adds files (directories and duplicates are skipped); returns the number added.
  int add_files(const std::vector<std::string>& paths);
  void remove(std::uint64_t id);
  void clear();
  [[nodiscard]] const std::vector<QueueItem>& items() const { return items_; }
  [[nodiscard]] QueueItem* find(std::uint64_t id);
  [[nodiscard]] const QueueItem* find(std::uint64_t id) const;
  [[nodiscard]] bool empty() const { return items_.empty(); }
  /// Messages for files that could not be added (shown once, then cleared).
  std::vector<std::string> notices;

  // Targets and options.
  bool set_target(std::uint64_t id, const std::string& target);
  /// Sets the target of every idle item with the given source format.
  int set_target_for_source(const std::string& source, const std::string& target);
  bool set_option(std::uint64_t id, const std::string& key, const Json& value);
  void set_preset(std::uint64_t id, int preset_index);
  void reset_options(std::uint64_t id);
  /// Source formats shared by two or more idle items, in queue order.
  [[nodiscard]] std::vector<std::string> shared_sources() const;
  [[nodiscard]] std::string options_json(const QueueItem& item) const;

  // Conversion.
  bool start(std::uint64_t id);
  int start_all();
  void cancel(std::uint64_t id);
  void cancel_all();
  /// Back to Ready, keeping target and options.
  void convert_again(std::uint64_t id);
  /// Pulls progress and results from the workers; returns true when anything changed.
  bool poll(double now);
  void wait_idle();
  [[nodiscard]] bool any_busy() const;
  [[nodiscard]] int count(ItemStatus s) const;

  /// Saves a finished item's output to `path`; true on success.
  bool save_to(std::uint64_t id, const std::string& path);
  /// Saves every finished item into `directory` (unique names); returns how many.
  int save_all_to(const std::string& directory);

  [[nodiscard]] const std::string& temp_dir() const { return temp_dir_; }

  /// Automation seam: see ProgressGate.
  void set_progress_gate(std::shared_ptr<ProgressGate> gate);
  [[nodiscard]] const std::shared_ptr<ProgressGate>& progress_gate() const { return gate_; }
  [[nodiscard]] int pending_jobs() const { return runner_.pending(); }

 private:
  void apply_target(QueueItem& item, const std::string& target);

  std::string temp_dir_;
  std::vector<QueueItem> items_;
  std::vector<std::pair<std::uint64_t, std::shared_ptr<JobProgress>>> running_;
  std::uint64_t next_id_ = 1;
  std::shared_ptr<ProgressGate> gate_;
  JobRunner runner_;
};

}  // namespace cvd::app
