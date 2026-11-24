// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "app/state.hpp"

#include <algorithm>
#include <cmath>

#include "base/text_util.hpp"
#include "platform/fileio.hpp"

namespace cvd::app {

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

Json Settings::to_json() const {
  Json j = Json::object();
  j.set("theme", theme == ThemeMode::Dark ? "dark" : theme == ThemeMode::Light ? "light" : "system");
  j.set("defaultPreset", preset_values()[static_cast<std::size_t>(std::clamp(default_preset, 0, 2))]);
  j.set("stripMetadata", strip_metadata);
  j.set("motion", motion == MotionMode::Reduced ? "reduced" : motion == MotionMode::Full ? "full" : "system");
  j.set("textScale", text_scale_percent);
  return j;
}

Settings Settings::from_json(const Json& j) {
  Settings s;
  const std::string& theme = j["theme"].str();
  s.theme = theme == "dark" ? ThemeMode::Dark : theme == "light" ? ThemeMode::Light : ThemeMode::System;
  const std::string& preset = j["defaultPreset"].str();
  for (std::size_t i = 0; i < preset_values().size(); ++i) {
    if (preset_values()[i] == preset) s.default_preset = static_cast<int>(i);
  }
  s.strip_metadata = j["stripMetadata"].as_bool(true);
  const std::string& motion = j["motion"].str();
  s.motion = motion == "reduced" ? MotionMode::Reduced : motion == "full" ? MotionMode::Full : MotionMode::System;
  const int scale = j["textScale"].as_int(100);
  s.text_scale_percent = scale >= 130 ? 130 : scale >= 115 ? 115 : 100;
  return s;
}

Settings load_settings(const std::string& directory) {
  const auto text = fileio::read_text(fileio::join(directory, "settings.json"));
  if (!text) return {};
  const auto j = Json::parse(*text);
  if (!j || !j->is_object()) return {};
  return Settings::from_json(*j);
}

bool save_settings(const std::string& directory, const Settings& settings) {
  if (!fileio::make_directories(directory)) return false;
  return fileio::write_text(fileio::join(directory, "settings.json"), settings.to_json().dump(2) + "\n");
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

Json coerce_option(const OptionSpec& spec, const Json& value) {
  switch (spec.type) {
    case OptionType::Int: {
      double v = value.is_number() ? value.as_number() : spec.default_value.as_number();
      v = std::round(v);
      if (spec.min) v = std::max(v, *spec.min);
      if (spec.max) v = std::min(v, *spec.max);
      return Json(v);
    }
    case OptionType::Float: {
      double v = value.is_number() ? value.as_number() : spec.default_value.as_number();
      if (spec.min) v = std::max(v, *spec.min);
      if (spec.max) v = std::min(v, *spec.max);
      return Json(v);
    }
    case OptionType::Bool: return Json(value.is_bool() ? value.as_bool() : spec.default_value.as_bool());
    case OptionType::Enum: {
      const std::string v = value.is_string() ? value.str() : spec.default_value.str();
      if (std::find(spec.choices.begin(), spec.choices.end(), v) != spec.choices.end()) return Json(v);
      return spec.default_value.is_string() ? spec.default_value : Json(spec.choices.empty() ? "" : spec.choices[0]);
    }
    case OptionType::String: return Json(value.is_string() ? value.str() : spec.default_value.as_string());
  }
  return value;
}

Json default_option_values(const std::vector<OptionSpec>& specs, const Settings& settings) {
  Json values = Json::object();
  for (const OptionSpec& s : specs) {
    Json v = s.default_value;
    if (s.key == "preset" && s.type == OptionType::Enum) {
      const std::string& want = preset_values()[static_cast<std::size_t>(std::clamp(settings.default_preset, 0, 2))];
      if (std::find(s.choices.begin(), s.choices.end(), want) != s.choices.end()) v = Json(want);
    }
    if (s.key == "strip_metadata" && s.type == OptionType::Bool) v = Json(settings.strip_metadata);
    values.set(s.key, coerce_option(s, v));
  }
  return values;
}

bool has_preset(const std::vector<OptionSpec>& specs) {
  for (const OptionSpec& s : specs) {
    if (s.key != "preset" || s.type != OptionType::Enum) continue;
    for (const auto& want : preset_values()) {
      if (std::find(s.choices.begin(), s.choices.end(), want) == s.choices.end()) return false;
    }
    return true;
  }
  return false;
}

int preset_index(const Json& values) {
  const std::string& v = values["preset"].str();
  for (std::size_t i = 0; i < preset_values().size(); ++i) {
    if (preset_values()[i] == v) return static_cast<int>(i);
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Queue helpers
// ---------------------------------------------------------------------------

const char* status_name(ItemStatus s) {
  switch (s) {
    case ItemStatus::Ready: return "ready";
    case ItemStatus::Queued: return "queued";
    case ItemStatus::Converting: return "converting";
    case ItemStatus::Done: return "done";
    case ItemStatus::Failed: return "failed";
    case ItemStatus::Cancelled: return "cancelled";
  }
  return "ready";
}

const TargetInfo* QueueItem::target_info() const {
  for (const auto& t : targets) {
    if (t.id == target) return &t;
  }
  return nullptr;
}

bool QueueItem::is_compress() const {
  const TargetInfo* t = target_info();
  return t && t->compress;
}

std::string default_target(const std::vector<TargetInfo>& targets) {
  for (const auto& t : targets) {
    if (!t.compress) return t.id;
  }
  return targets.empty() ? std::string() : targets.front().id;
}

bool target_matches(const TargetInfo& t, std::string_view query) {
  const std::string_view q = trim(query);
  if (q.empty()) return true;
  if (contains_ci(t.id, q) || contains_ci(t.name, q)) return true;
  std::string_view qq = q;
  if (!qq.empty() && qq.front() == '.') qq.remove_prefix(1);
  for (const auto& e : t.extensions) {
    if (starts_with_ci(e, qq)) return true;
  }
  return contains_ci(category_label(t.category), q);
}

std::vector<TargetGroup> group_targets(const std::vector<TargetInfo>& targets, const std::string& source_category,
                                       std::string_view query) {
  std::vector<TargetGroup> groups;
  const auto group_for = [&](const std::string& cat) -> TargetGroup& {
    for (auto& g : groups) {
      if (g.category == cat) return g;
    }
    groups.push_back({cat, {}});
    return groups.back();
  };
  // The source's own category always leads.
  for (const auto& t : targets) {
    if (t.category == source_category && target_matches(t, query)) group_for(t.category).targets.push_back(&t);
  }
  for (const auto& t : targets) {
    if (t.category != source_category && target_matches(t, query)) group_for(t.category).targets.push_back(&t);
  }
  return groups;
}

std::vector<const TargetInfo*> archive_targets(const std::vector<TargetInfo>& targets) {
  std::vector<const TargetInfo*> out;
  for (const auto& t : targets) {
    if (t.category == "archive" && !t.compress) out.push_back(&t);
  }
  return out;
}

std::string output_file_name(const std::string& source_name, const std::string& extension, bool same_format) {
  std::string stem = path_stem(source_name);
  if (stem.empty()) stem = "converted";
  // "photo.tar.gz" style double extensions: strip a trailing ".tar" too.
  if (path_extension(stem) == "tar") stem = path_stem(stem);
  if (same_format) stem += " (compressed)";
  return extension.empty() ? stem : stem + "." + extension;
}

// ---------------------------------------------------------------------------
// Job runner
// ---------------------------------------------------------------------------

JobRunner::JobRunner(int workers, std::function<void()> wake) : wake_(std::move(wake)) {
  if (workers <= 0) {
    const unsigned hc = std::thread::hardware_concurrency();
    workers = static_cast<int>(std::clamp(hc / 2u, 1u, 4u));
  }
  for (int i = 0; i < workers; ++i) {
    threads_.emplace_back([this](std::stop_token st) { worker_loop(st); });
  }
}

JobRunner::~JobRunner() {
  {
    std::lock_guard lock(mutex_);
    for (auto& [job, progress] : queue_) progress->cancel = true;
  }
  for (auto& t : threads_) t.request_stop();
  cv_.notify_all();
  threads_.clear();  // joins
}

std::shared_ptr<JobProgress> JobRunner::submit(Job job) {
  auto progress = std::make_shared<JobProgress>();
  {
    std::lock_guard lock(mutex_);
    queue_.emplace_back(std::move(job), progress);
  }
  cv_.notify_one();
  return progress;
}

void JobRunner::wait_idle() {
  std::unique_lock lock(mutex_);
  idle_cv_.wait(lock, [&] { return queue_.empty() && running_ == 0; });
}

void JobRunner::worker_loop(std::stop_token stop) {
  while (!stop.stop_requested()) {
    std::pair<Job, std::shared_ptr<JobProgress>> work;
    {
      std::unique_lock lock(mutex_);
      if (!cv_.wait(lock, stop, [&] { return !queue_.empty(); })) return;
      work = std::move(queue_.front());
      queue_.pop_front();
      ++running_;
    }
    auto& [job, progress] = work;
    progress->started = true;
    if (wake_) wake_();
    ConvertOutcome outcome;
    if (progress->cancel) {
      outcome.status = kCancelled;
      outcome.message = "Cancelled before it started.";
    } else {
      double last_wake = -1.0;
      outcome = convert_file(job.request, [&](double fraction, std::string_view stage) {
        progress->fraction = static_cast<float>(fraction);
        {
          std::lock_guard lock(progress->mutex);
          if (progress->stage != stage) progress->stage = std::string(stage);
        }
        if (wake_ && (fraction - last_wake >= 0.01 || fraction >= 1.0)) {
          last_wake = fraction;
          wake_();
        }
        return progress->cancel.load() || stop.stop_requested();
      });
    }
    {
      std::lock_guard lock(progress->mutex);
      progress->outcome = std::move(outcome);
    }
    progress->finished = true;
    {
      std::lock_guard lock(mutex_);
      --running_;
    }
    idle_cv_.notify_all();
    if (wake_) wake_();
  }
}

// ---------------------------------------------------------------------------
// AppState
// ---------------------------------------------------------------------------

AppState::AppState(std::string temp_dir, std::function<void()> wake, int workers)
    : temp_dir_(std::move(temp_dir)), runner_(workers, std::move(wake)) {}

AppState::~AppState() { cancel_all(); }

QueueItem* AppState::find(std::uint64_t id) {
  for (auto& it : items_) {
    if (it.id == id) return &it;
  }
  return nullptr;
}

const QueueItem* AppState::find(std::uint64_t id) const {
  for (const auto& it : items_) {
    if (it.id == id) return &it;
  }
  return nullptr;
}

void AppState::apply_target(QueueItem& item, const std::string& target) {
  item.target = target;
  item.options = options_for(item.effective_source(), target);
  item.values = default_option_values(item.options, settings);
}

int AppState::add_files(const std::vector<std::string>& paths) {
  int added = 0;
  for (const auto& path : paths) {
    if (path.empty()) continue;
    if (fileio::is_directory(path)) {
      notices.push_back("Folders can\xE2\x80\x99t be converted directly: " + path_filename(path));
      continue;
    }
    if (!fileio::exists(path)) {
      notices.push_back("Couldn\xE2\x80\x99t find " + path_filename(path));
      continue;
    }
    const bool duplicate = std::any_of(items_.begin(), items_.end(), [&](const QueueItem& q) { return q.path == path; });
    if (duplicate) continue;
    QueueItem item;
    item.id = next_id_++;
    item.path = path;
    item.name = path_filename(path);
    item.size = fileio::file_size(path).value_or(0);
    item.source = detect_file(path);
    if (const FormatInfo* f = Catalog::instance().find(item.source)) {
      item.category = f->category;
      item.source_label = ascii_upper(f->extensions.empty() ? f->id : f->extensions.front());
    } else {
      const std::string ext = path_extension(item.name);
      item.source_label = ext.empty() ? "FILE" : ascii_upper(ext);
    }
    // Unknown files still have the universal targets (base64, checksums, ...) via their raw bytes.
    item.targets = targets_for(item.effective_source());
    apply_target(item, default_target(item.targets));
    items_.push_back(std::move(item));
    ++added;
  }
  return added;
}

void AppState::remove(std::uint64_t id) {
  cancel(id);
  for (auto it = items_.begin(); it != items_.end(); ++it) {
    if (it->id == id) {
      if (!it->output_path.empty()) fileio::remove_all(path_parent(it->output_path));
      items_.erase(it);
      return;
    }
  }
}

void AppState::clear() {
  cancel_all();
  for (const auto& it : items_) {
    if (!it.output_path.empty()) fileio::remove_all(path_parent(it.output_path));
  }
  items_.clear();
}

bool AppState::set_target(std::uint64_t id, const std::string& target) {
  QueueItem* item = find(id);
  if (!item || item->busy()) return false;
  const bool known = std::any_of(item->targets.begin(), item->targets.end(), [&](const auto& t) { return t.id == target; });
  if (!known) return false;
  apply_target(*item, target);
  if (item->status != ItemStatus::Ready) {
    item->status = ItemStatus::Ready;
    item->progress = 0.0f;
  }
  return true;
}

int AppState::set_target_for_source(const std::string& source, const std::string& target) {
  int n = 0;
  for (auto& item : items_) {
    if (item.source == source && !item.busy() && item.status != ItemStatus::Done && set_target(item.id, target)) ++n;
  }
  return n;
}

bool AppState::set_option(std::uint64_t id, const std::string& key, const Json& value) {
  QueueItem* item = find(id);
  if (!item || item->busy()) return false;
  for (const auto& spec : item->options) {
    if (spec.key == key) {
      item->values.set(key, coerce_option(spec, value));
      return true;
    }
  }
  return false;
}

void AppState::set_preset(std::uint64_t id, int index) {
  if (index < 0 || index > 2) return;
  set_option(id, "preset", Json(preset_values()[static_cast<std::size_t>(index)]));
}

void AppState::reset_options(std::uint64_t id) {
  QueueItem* item = find(id);
  if (!item || item->busy()) return;
  item->values = default_option_values(item->options, settings);
}

std::vector<std::string> AppState::shared_sources() const {
  std::vector<std::string> out;
  for (const auto& item : items_) {
    if (item.source.empty() || item.status != ItemStatus::Ready) continue;
    if (std::find(out.begin(), out.end(), item.source) != out.end()) continue;
    const auto n = std::count_if(items_.begin(), items_.end(), [&](const QueueItem& q) {
      return q.source == item.source && q.status == ItemStatus::Ready;
    });
    if (n >= 2) out.push_back(item.source);
  }
  return out;
}

std::string AppState::options_json(const QueueItem& item) const {
  Json obj = Json::object();
  for (const auto& spec : item.options) {
    if (item.values.contains(spec.key)) obj.set(spec.key, item.values[spec.key]);
  }
  return obj.dump();
}

bool AppState::start(std::uint64_t id) {
  QueueItem* item = find(id);
  if (!item || item->busy() || item->target.empty()) return false;
  const TargetInfo* t = item->target_info();
  const std::string ext = t && !t->extensions.empty() ? t->extensions.front() : item->target;
  const std::string dir = fileio::join(temp_dir_, std::to_string(item->id) + "-" + std::to_string(next_id_++));
  fileio::make_directories(dir);
  item->output_name = output_file_name(item->name, ext, item->is_compress());
  item->output_path = fileio::join(dir, "output." + ext);
  item->saved_path.clear();
  item->status = ItemStatus::Queued;
  item->progress = 0.0f;
  item->stage = "Waiting";
  item->error_status = 0;
  item->error_message.clear();
  item->warnings.clear();
  item->output_size = 0;
  Job job;
  job.item_id = item->id;
  job.request.input_path = item->path;
  job.request.output_path = item->output_path;
  job.request.source = item->effective_source();
  job.request.target = item->target;
  job.request.options_json = options_json(*item);
  running_.emplace_back(item->id, runner_.submit(std::move(job)));
  return true;
}

int AppState::start_all() {
  int n = 0;
  for (auto& item : items_) {
    if (item.status == ItemStatus::Ready && start(item.id)) ++n;
  }
  return n;
}

void AppState::cancel(std::uint64_t id) {
  for (auto& [item_id, progress] : running_) {
    if (item_id == id) progress->cancel = true;
  }
}

void AppState::cancel_all() {
  for (auto& [item_id, progress] : running_) progress->cancel = true;
}

void AppState::convert_again(std::uint64_t id) {
  QueueItem* item = find(id);
  if (!item || item->busy()) return;
  item->status = ItemStatus::Ready;
  item->progress = 0.0f;
  item->stage.clear();
  item->error_status = 0;
  item->error_message.clear();
  item->warnings.clear();
}

bool AppState::poll(double now) {
  bool changed = false;
  for (auto it = running_.begin(); it != running_.end();) {
    QueueItem* item = find(it->first);
    auto& progress = *it->second;
    if (!item) {
      progress.cancel = true;
      if (progress.finished) {
        it = running_.erase(it);
      } else {
        ++it;
      }
      continue;
    }
    if (progress.started && item->status == ItemStatus::Queued) {
      item->status = ItemStatus::Converting;
      item->started_at = now;
      changed = true;
    }
    const float f = progress.fraction.load();
    if (f != item->progress) {
      item->progress = f;
      changed = true;
    }
    {
      std::lock_guard lock(progress.mutex);
      if (!progress.stage.empty() && progress.stage != item->stage) {
        item->stage = progress.stage;
        changed = true;
      }
    }
    if (progress.finished) {
      ConvertOutcome outcome;
      {
        std::lock_guard lock(progress.mutex);
        outcome = progress.outcome;
      }
      item->warnings = outcome.warnings;
      if (outcome.status == kOk) {
        item->status = ItemStatus::Done;
        item->progress = 1.0f;
        item->output_size = outcome.output_size;
        if (!outcome.extension.empty()) {
          item->output_name = output_file_name(item->name, outcome.extension, item->is_compress());
        }
      } else {
        item->status = outcome.status == kCancelled ? ItemStatus::Cancelled : ItemStatus::Failed;
        item->error_status = outcome.status;
        item->error_message = outcome.message;
        if (!item->output_path.empty()) fileio::remove_all(path_parent(item->output_path));
        item->output_path.clear();
      }
      changed = true;
      it = running_.erase(it);
      continue;
    }
    ++it;
  }
  return changed;
}

void AppState::wait_idle() { runner_.wait_idle(); }

bool AppState::any_busy() const {
  return std::any_of(items_.begin(), items_.end(), [](const QueueItem& q) { return q.busy(); });
}

int AppState::count(ItemStatus s) const {
  return static_cast<int>(std::count_if(items_.begin(), items_.end(), [&](const QueueItem& q) { return q.status == s; }));
}

bool AppState::save_to(std::uint64_t id, const std::string& path) {
  QueueItem* item = find(id);
  if (!item || item->status != ItemStatus::Done || item->output_path.empty()) return false;
  if (!fileio::copy_file(item->output_path, path)) return false;
  item->saved_path = path;
  return true;
}

int AppState::save_all_to(const std::string& directory) {
  int n = 0;
  if (!fileio::make_directories(directory)) return 0;
  for (auto& item : items_) {
    if (item.status != ItemStatus::Done) continue;
    const std::string dest = fileio::unique_path(directory, item.output_name);
    if (save_to(item.id, dest)) ++n;
  }
  return n;
}

}  // namespace cvd::app
