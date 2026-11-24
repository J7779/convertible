// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "app/automation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "app/app.hpp"
#include "base/text_util.hpp"
#include "gfx/image_io.hpp"
#include "platform/fileio.hpp"

namespace cvd::app {

namespace {

Key key_from_name(const std::string& n) {
  static const std::pair<const char*, Key> keys[] = {
      {"Tab", Key::Tab},       {"Enter", Key::Enter},     {"Return", Key::Enter},   {"Escape", Key::Escape},
      {"Space", Key::Space},   {"Backspace", Key::Backspace}, {"Delete", Key::Delete}, {"Left", Key::Left},
      {"Right", Key::Right},   {"Up", Key::Up},           {"Down", Key::Down},      {"Home", Key::Home},
      {"End", Key::End},       {"PageUp", Key::PageUp},   {"PageDown", Key::PageDown}, {"A", Key::A},
      {"C", Key::C},           {"F", Key::F},             {"N", Key::N},            {"O", Key::O},
      {"Q", Key::Q},           {"S", Key::S},             {"V", Key::V},            {"W", Key::W},
      {"X", Key::X},           {"Z", Key::Z},             {"Comma", Key::Comma},    {"Plus", Key::Plus},
      {"Minus", Key::Minus},   {"0", Key::Digit0},        {"1", Key::Digit1},       {"2", Key::Digit2},
      {"3", Key::Digit3}};
  for (const auto& [name, key] : keys) {
    if (n == name) return key;
  }
  return Key::Other;
}

double real_seconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

Modifiers mods_from(const Json& cmd) {
  Modifiers m;
  m.shift = cmd["shift"].as_bool();
  m.alt = cmd["alt"].as_bool();
  if (cmd["primary"].as_bool()) {
#if defined(__APPLE__)
    m.super = true;
#else
    m.ctrl = true;
#endif
  }
  return m;
}

/// A click as real input arrives: press in one frame, release in the next,
/// then one more frame before anything else (so clicks never merge).
std::vector<std::vector<Event>> click_batches(Vec2 pos, const Json& cmd) {
  Event e;
  e.pos = pos;
  e.mods = mods_from(cmd);
  std::vector<std::vector<Event>> batches;
  e.type = EventType::MouseMove;
  std::vector<Event> first = {e};
  const int clicks = std::max(1, cmd["clicks"].as_int(1));
  for (int i = 1; i <= clicks; ++i) {
    e.clicks = i;
    e.type = EventType::MouseDown;
    if (i == 1) first.push_back(e);
    else batches.push_back({e});
    if (i == 1) batches.push_back(first);
    e.type = EventType::MouseUp;
    batches.push_back({e});
  }
  batches.push_back({});
  return batches;
}

const char* role_name(ui::Role r) {
  static const char* roles[] = {"button", "toggle", "slider", "textfield", "tab", "menuitem",
                                "listitem", "label", "group", "image", "progress", "link"};
  return roles[static_cast<int>(r)];
}

Vec2 point_or_centre(App& app, const Json& cmd) {
  if (cmd.contains("x")) return {static_cast<float>(cmd["x"].as_number()), static_cast<float>(cmd["y"].as_number())};
  return Vec2{app.ui().size().x * 0.5f, app.ui().size().y * 0.5f} * app.zoom();
}

}  // namespace

Automation::Automation(App& app, const std::string& script_path, const std::string& out_dir)
    : app_(app), script_dir_(path_parent(script_path)), out_dir_(out_dir) {
  if (script_dir_.empty()) script_dir_ = ".";
  if (out_dir_.empty()) out_dir_ = script_dir_;
  fileio::make_directories(out_dir_);
  // Shell calls never leave the app under automation: they are recorded.
  app_.seams.shell = [this](const std::string& action, const std::string& path) {
    Json j = Json::object();
    j.set("action", action);
    j.set("path", path);
    j.set("exists", fileio::exists(path));
    shell_.push_back(j);
    log_.push_back(action + " " + path);
    return shell_result_;
  };
  const auto text = fileio::read_text(script_path);
  if (!text) {
    fail("cannot read automation script " + script_path);
    done_ = true;
    return;
  }
  const auto doc = Json::parse(*text);
  if (!doc || !doc->is_array()) {
    fail("automation script is not a JSON array");
    done_ = true;
    return;
  }
  steps_ = doc->items();
  trace_ = std::getenv("CVD_AUTOMATION_TRACE") != nullptr;
}

Automation::~Automation() {
  if (gate_) gate_->open();
  app_.seams = {};
  write_log();
}

void Automation::write_log() const {
  std::string text;
  for (const auto& line : log_) text += line + "\n";
  text += "failures " + std::to_string(failures_) + "\n";
  fileio::write_text(fileio::join(out_dir_, "automation.log"), text);
}

Json Automation::dump() const {
  Json j = Json::object();
  j.set("dialogs", dialogs_);
  j.set("shell", shell_);
  Json samples = Json::object();
  for (const auto& [index, values] : samples_) {
    Json arr = Json::array();
    for (const double v : values) arr.push_back(v);
    samples.set(std::to_string(index), arr);
  }
  j.set("samples", samples);
  j.set("failures", failures_);
  j.set("step", static_cast<double>(index_));
  if (gate_) {
    Json g = Json::object();
    g.set("held", gate_->held());
    g.set("parked", gate_->parked());
    j.set("gate", g);
  }
  return j;
}

void Automation::fail(const std::string& message) {
  ++failures_;
  log_.push_back("FAIL: step " + std::to_string(index_) + ": " + message);
  std::fprintf(stderr, "automation: FAIL: step %zu: %s\n", index_, message.c_str());
  // Keep a picture of the first failures next to the outputs.
  if (failure_shots_ < 3 && pending_shot_.empty()) {
    const std::string dir = fileio::join(out_dir_, "failures");
    fileio::make_directories(dir);
    pending_shot_ = fileio::join(dir, "failure-" + std::to_string(++failure_shots_) + ".png");
  }
}

std::string Automation::in_path(const std::string& p) const {
  if (p.empty()) return p;
  if (fileio::to_path(p).is_absolute()) return p;
  return fileio::join(script_dir_, p);
}

std::string Automation::out_path(const std::string& p) const {
  if (fileio::to_path(p).is_absolute()) return p;
  return fileio::join(out_dir_, p);
}

const Json* Automation::lookup(const Json& root, const std::string& path) {
  const Json* cur = &root;
  for (const std::string& part : split(path, '.')) {
    if (part.empty()) continue;
    if (cur->is_array()) {
      char* end = nullptr;
      const long idx = std::strtol(part.c_str(), &end, 10);
      if (end == part.c_str() || idx < 0 || static_cast<std::size_t>(idx) >= cur->size()) return nullptr;
      cur = &(*cur)[static_cast<std::size_t>(idx)];
    } else if (cur->is_object()) {
      if (!cur->contains(part)) return nullptr;
      cur = &(*cur)[part];
    } else {
      return nullptr;
    }
  }
  return cur;
}

bool Automation::click_label(const Json& cmd) {
  // Matches by label (substring, or exact with "exact": true), optionally by
  // role; "nth" picks among several matches in tree order. Buttons and menu
  // items win over other roles unless a role is given.
  const std::string& label = cmd["label"].str();
  const bool exact = cmd["exact"].as_bool(false);
  const std::string& role = cmd["role"].str();
  std::vector<const ui::AccessNode*> matches;
  for (const auto& n : app_.ui().access_nodes()) {
    if (n.rect.empty() || !n.enabled) continue;
    if (!(exact ? n.label == label : contains_ci(n.label, label))) continue;
    if (!role.empty() && role != role_name(n.role)) continue;
    matches.push_back(&n);
  }
  if (role.empty()) {
    std::vector<const ui::AccessNode*> buttons;
    for (const auto* n : matches) {
      if (n->role == ui::Role::Button || n->role == ui::Role::MenuItem) buttons.push_back(n);
    }
    if (!buttons.empty()) matches = buttons;
  }
  // "last": true picks the last match (e.g. the action bar's "Convert", not the "Convert" tab).
  const int nth = cmd["last"].as_bool() ? static_cast<int>(matches.size()) - 1 : cmd["nth"].as_int(0);
  if (nth < 0 || static_cast<std::size_t>(nth) >= matches.size()) return false;
  const auto* node = matches[static_cast<std::size_t>(nth)];
  // "dx"/"dy" (0..1) pick a point inside the element, e.g. along a slider.
  const Vec2 p{node->rect.x + node->rect.w * static_cast<float>(cmd["dx"].as_number(0.5)),
               node->rect.y + node->rect.h * static_cast<float>(cmd["dy"].as_number(0.5))};
  for (auto& b : click_batches(p * app_.zoom(), cmd)) event_batches_.push_back(std::move(b));
  return true;
}

bool Automation::check(const Json& cmd, const Json& state) {
  const std::string path = cmd["path"].str();
  const Json* v = lookup(state, path);
  if (cmd.contains("equals")) return v && *v == cmd["equals"];
  if (cmd.contains("notEquals")) return !v || !(*v == cmd["notEquals"]);
  if (cmd.contains("exists")) return (v != nullptr) == cmd["exists"].as_bool(true);
  bool ok = v != nullptr;
  if (cmd.contains("min")) ok = ok && v->is_number() && v->as_number() >= cmd["min"].as_number();
  if (cmd.contains("max")) ok = ok && v->is_number() && v->as_number() <= cmd["max"].as_number();
  if (cmd.contains("length")) ok = ok && v->size() == static_cast<std::size_t>(cmd["length"].as_int());
  if (cmd.contains("contains")) {
    bool found = false;
    if (v && v->is_string()) found = v->str().find(cmd["contains"].str()) != std::string::npos;
    if (v && v->is_array()) {
      for (const Json& e : v->items()) found = found || e == cmd["contains"];
    }
    ok = ok && found;
  }
  return ok;
}

void Automation::before_frame(double now) {
  while (!done_) {
    if (index_ >= steps_.size()) {
      done_ = true;
      log_.push_back("done");
      app_.quit();
      return;
    }
    if (trace_ && !waiting_) std::fprintf(stderr, "automation: step %zu %s\n", index_, steps_[index_].dump().c_str());
    if (!step(steps_[index_], now)) return;  // waiting
    ++index_;
    waiting_ = false;
    if (yield_) {
      // Screenshots capture the frame that is about to be rendered.
      yield_ = false;
      return;
    }
  }
}

void Automation::after_frame(const gfx::Bitmap& frame) {
  if (!pending_shot_.empty()) {
    fileio::make_directories(path_parent(pending_shot_));
    if (!gfx::write_image(frame, pending_shot_)) fail("cannot write screenshot " + pending_shot_);
    else log_.push_back("screenshot " + pending_shot_);
    pending_shot_.clear();
  }
}

bool Automation::step(const Json& cmd, double now) {
  const std::string& op = cmd["op"].str();
  AppState& st = app_.state();
  const auto item_at = [&](const Json& c) -> QueueItem* {
    const int idx = c["index"].as_int(0);
    if (idx < 0 || static_cast<std::size_t>(idx) >= st.items().size()) return nullptr;
    return st.find(st.items()[static_cast<std::size_t>(idx)].id);
  };

  if (op == "open") {
    std::vector<std::string> files;
    for (const Json& f : cmd["files"].items()) files.push_back(in_path(f.str()));
    if (cmd["drop"].as_bool(true)) {
      // Drops land anywhere on the window: the centre unless a point is given.
      Event e;
      e.type = EventType::Drop;
      e.paths = files;
      e.pos = point_or_centre(app_, cmd);
      app_.on_event(e);
    } else {
      app_.add_files(files);
    }
    return true;
  }
  if (op == "wait") {
    // Advances the clock by `ms` (virtual time when headless).
    if (!waiting_) {
      waiting_ = true;
      wait_until_ = now + cmd["ms"].as_number(0) / 1000.0;
    }
    return now + 1e-9 >= wait_until_;
  }
  if (op == "at") {
    // Runs the clock up to `ms` after the first frame; the next step sees that frame.
    const double t0 = app_.intro_start_ < 0.0 ? now : app_.intro_start_;
    return (now - t0) * 1000.0 + 1e-6 >= cmd["ms"].as_number(0);
  }
  if (op == "frames") {
    if (!waiting_) {
      waiting_ = true;
      frames_left_ = cmd["count"].as_int(1);
    }
    return frames_left_-- <= 0;
  }
  if (op == "skipIntro") {
    app_.skip_intro();
    return true;
  }
  if (op == "setTarget") {
    if (cmd.contains("source")) {
      if (st.set_target_for_source(cmd["source"].str(), cmd["target"].str()) == 0) fail("setTarget for source failed");
    } else if (QueueItem* it = item_at(cmd); !it || !st.set_target(it->id, cmd["target"].str())) {
      fail("setTarget " + cmd["target"].str() + " failed");
    }
    return true;
  }
  if (op == "setOption") {
    QueueItem* it = item_at(cmd);
    if (!it || !st.set_option(it->id, cmd["key"].str(), cmd["value"])) fail("setOption " + cmd["key"].str() + " failed");
    return true;
  }
  if (op == "setPreset") {
    if (QueueItem* it = item_at(cmd)) {
      const std::string p = cmd["preset"].str();
      for (std::size_t i = 0; i < preset_values().size(); ++i) {
        if (preset_values()[i] == p) st.set_preset(it->id, static_cast<int>(i));
      }
    }
    return true;
  }
  if (op == "advanced") {
    if (QueueItem* it = item_at(cmd)) it->advanced_open = cmd["open"].as_bool(true);
    return true;
  }
  if (op == "openPicker") {
    if (QueueItem* it = item_at(cmd)) {
      // Anchor at the card's target button when it is on screen.
      app_.open_picker(it->id, cmd["source"].str());
      Rect anchor{app_.ui().size().x - 360.0f, 120.0f, 140.0f, 40.0f};
      for (const auto& n : app_.ui().access_nodes()) {
        if (n.role == ui::Role::Button && n.label == "Target format for " + it->name) {
          anchor = n.rect;
          break;
        }
      }
      app_.picker_.anchor = anchor;
    }
    return true;
  }
  if (op == "closePicker") {
    app_.close_picker();
    return true;
  }
  if (op == "search") {
    app_.picker_query = cmd["text"].str();
    return true;
  }
  if (op == "convert") {
    if (QueueItem* it = item_at(cmd)) st.start(it->id);
    return true;
  }
  if (op == "convertAll") {
    app_.convert_all();
    return true;
  }
  if (op == "cancel") {
    if (QueueItem* it = item_at(cmd)) st.cancel(it->id);
    return true;
  }
  if (op == "cancelAll") {
    st.cancel_all();
    return true;
  }
  if (op == "gate") {
    // Progress gate: "hold" parks conversions at their progress reports, "open" releases them.
    if (!gate_) {
      gate_ = std::make_shared<ProgressGate>();
      st.set_progress_gate(gate_);
    }
    if (cmd["mode"].str() == "hold") gate_->hold();
    else gate_->open();
    return true;
  }
  if (op == "gateWait") {
    // Real-time wait (workers run in real time) until `parked` of them sit in a progress report.
    if (!gate_) {
      fail("gateWait without a gate");
      return true;
    }
    const int want = cmd["parked"].as_int(1);
    if (!gate_->wait([&] { return gate_->parked() >= want; }, cmd["timeoutMs"].as_int(30000))) {
      fail("gateWait: only " + std::to_string(gate_->parked()) + " of " + std::to_string(want) + " workers parked");
    }
    st.poll(now);
    return true;
  }
  if (op == "stepProgress") {
    // One frame per step: let `stride` progress reports through, wait until
    // the worker parks again (or finishes) and record the item's progress.
    const int idx = cmd["index"].as_int(0);
    QueueItem* it = item_at(cmd);
    if (!gate_ || !gate_->held() || !it) {
      fail("stepProgress needs a held gate and an item");
      return true;
    }
    if (!waiting_) {
      waiting_ = true;
      steps_done_ = 0;
      if (!cmd["append"].as_bool()) samples_[idx].clear();
    }
    st.poll(now);
    samples_[idx].push_back(static_cast<double>(it->progress));
    if (!it->busy()) return true;
    if (cmd.contains("steps") && steps_done_ >= cmd["steps"].as_int()) return true;
    if (steps_done_ >= cmd["maxSteps"].as_int(100000)) {
      fail("stepProgress: more than maxSteps progress steps");
      return true;
    }
    for (int k = 0; k < std::max(1, cmd["stride"].as_int(1)); ++k) gate_->permit();
    ++steps_done_;
    const bool ok = gate_->wait([&] { return gate_->permits() == 0 && gate_->parked() >= st.pending_jobs(); },
                                cmd["timeoutMs"].as_int(30000));
    if (!ok) {
      fail("stepProgress: the worker did not report progress in time");
      return true;
    }
    return false;
  }
  if (op == "waitIdle") {
    if (!waiting_ && gate_ && gate_->held()) {
      fail("waitIdle while the progress gate is held (open it first)");
      gate_->open();
    }
    st.wait_idle();
    st.poll(now);
    // Let the UI show the finished state for a few frames.
    if (!waiting_) {
      waiting_ = true;
      idle_frames_ = cmd["frames"].as_int(2);
    }
    return idle_frames_-- <= 0;
  }
  if (op == "waitFor") {
    // Renders frames until the condition on the state dump holds (bounded in real time).
    if (!waiting_) {
      waiting_ = true;
      real_deadline_ = real_seconds() + cmd["timeoutMs"].as_number(30000) / 1000.0;
    }
    st.poll(now);
    if (check(cmd, app_.dump_state())) return true;
    if (real_seconds() > real_deadline_) {
      fail("waitFor " + cmd["path"].str() + " timed out");
      return true;
    }
    return false;
  }
  if (op == "dialogs") {
    // "seam": file dialogs are answered from dialogReply steps (and recorded)
    // instead of opening the platform's dialog or the in-house browser.
    if (cmd["mode"].str() == "seam") {
      app_.seams.dialog = [this](const std::string& kind, const std::string& title, const std::string& directory,
                                 const std::string& name, std::function<void(std::vector<std::string>)> reply) {
        Json j = Json::object();
        j.set("kind", kind);
        j.set("title", title);
        j.set("directory", directory);
        j.set("name", name);
        dialogs_.push_back(j);
        std::vector<std::string> answer;
        if (!dialog_replies_.empty()) {
          answer = dialog_replies_.front();
          dialog_replies_.pop_front();
        }
        log_.push_back("dialog " + kind + (answer.empty() ? " cancelled" : " -> " + answer.front()));
        reply(std::move(answer));
      };
    } else {
      app_.seams.dialog = nullptr;
    }
    return true;
  }
  if (op == "dialogReply") {
    std::vector<std::string> paths;
    for (const Json& f : cmd["paths"].items()) paths.push_back(out_path(f.str()));
    dialog_replies_.push_back(std::move(paths));
    return true;
  }
  if (op == "shellResult") {
    shell_result_ = cmd["value"].as_bool(true);
    return true;
  }
  if (op == "save") {
    QueueItem* it = item_at(cmd);
    if (!it || !st.save_to(it->id, out_path(cmd["path"].str()))) fail("save failed");
    return true;
  }
  if (op == "saveAll") {
    const int n = st.save_all_to(out_path(cmd["dir"].as_string(".")));
    if (cmd.contains("expect") && n != cmd["expect"].as_int()) fail("saveAll saved " + std::to_string(n));
    log_.push_back("saved " + std::to_string(n));
    return true;
  }
  if (op == "screenshot") {
    pending_shot_ = out_path(cmd["path"].as_string("screenshot.png"));
    yield_ = true;
    return true;
  }
  if (op == "dump") {
    const std::string p = out_path(cmd["path"].as_string("state.json"));
    Json state = app_.dump_state();
    if (cmd["catalog"].as_bool()) {
      Json formats = Json::array();
      for (const auto& f : Catalog::instance().formats()) {
        Json j = Json::object();
        j.set("id", f.id);
        j.set("name", f.name);
        j.set("description", f.description);
        j.set("category", f.category);
        Json exts = Json::array();
        for (const auto& e : f.extensions) exts.push_back(e);
        j.set("extensions", exts);
        j.set("readable", f.readable);
        j.set("writable", f.writable);
        formats.push_back(j);
      }
      state.set("catalog", formats);
    }
    fileio::make_directories(path_parent(p));
    if (!fileio::write_text(p, state.dump(2) + "\n")) fail("cannot write " + p);
    return true;
  }
  if (op == "navigate") {
    const std::string& s = cmd["screen"].str();
    app_.navigate(s == "formats" ? Screen::Formats : s == "settings" ? Screen::Settings : s == "licenses" ? Screen::Licenses : Screen::Home);
    return true;
  }
  if (op == "theme" || op == "settings") {
    Settings s = app_.state().settings;
    if (op == "theme") {
      const std::string& v = cmd["value"].str();
      s.theme = v == "dark" ? ThemeMode::Dark : v == "light" ? ThemeMode::Light : ThemeMode::System;
    } else {
      Json merged = s.to_json();
      for (const auto& [k, v] : cmd["values"].members()) merged.set(k, v);
      s = Settings::from_json(merged);
    }
    app_.apply_settings(s);
    return true;
  }
  if (op == "resize") {
    if (!app_.platform().set_automation_size(cmd["width"].as_int(1120), cmd["height"].as_int(760),
                                             static_cast<float>(cmd["scale"].as_number(0.0)))) {
      log_.push_back("resize ignored by this platform");
    }
    return true;
  }
  if (op == "click") {
    // Press, release and settle over three frames (see click_batches).
    if (!waiting_) {
      waiting_ = true;
      event_batches_.clear();
      if (cmd.contains("label")) {
        if (!click_label(cmd)) fail("no element labelled " + cmd["label"].str());
      } else {
        for (auto& b : click_batches({static_cast<float>(cmd["x"].as_number()), static_cast<float>(cmd["y"].as_number())}, cmd)) {
          event_batches_.push_back(std::move(b));
        }
      }
    }
    if (event_batches_.empty()) return true;
    for (const Event& e : event_batches_.front()) app_.on_event(e);
    event_batches_.pop_front();
    return false;
  }
  if (op == "hover") {
    Event e;
    e.type = EventType::MouseMove;
    if (cmd.contains("label")) {
      for (const auto& n : app_.ui().access_nodes()) {
        if (contains_ci(n.label, cmd["label"].str()) && !n.rect.empty()) {
          e.pos = n.rect.center() * app_.zoom();
          break;
        }
      }
    } else {
      e.pos = {static_cast<float>(cmd["x"].as_number()), static_cast<float>(cmd["y"].as_number())};
    }
    app_.on_event(e);
    return true;
  }
  if (op == "scroll") {
    Event e;
    e.type = EventType::MouseMove;
    e.pos = point_or_centre(app_, cmd);
    app_.on_event(e);
    e.type = EventType::Scroll;
    e.scroll = {0.0f, static_cast<float>(cmd["dy"].as_number())};
    app_.on_event(e);
    return true;
  }
  if (op == "key") {
    Event e;
    e.type = EventType::KeyDown;
    e.key = key_from_name(cmd["key"].str());
    e.mods = mods_from(cmd);
    // One press per frame, as a person (or key repeat) would deliver them.
    if (!waiting_) {
      waiting_ = true;
      frames_left_ = std::max(1, cmd["repeat"].as_int(1));
    }
    if (frames_left_-- <= 0) return true;
    e.type = EventType::KeyDown;
    app_.on_event(e);
    e.type = EventType::KeyUp;
    app_.on_event(e);
    return false;
  }
  if (op == "focusCycle") {
    // Presses Tab `count` times (one per frame) and writes the focused
    // control's name after each press to `path` (a JSON array).
    if (!waiting_) {
      waiting_ = true;
      frames_left_ = cmd["count"].as_int(10);
      focus_trail_ = Json::array();
    } else {
      std::string label;
      for (const auto& n : app_.ui().access_nodes()) {
        if (n.focused) label = n.label;
      }
      focus_trail_.push_back(label);
    }
    if (frames_left_-- <= 0) {
      const std::string p = out_path(cmd["path"].as_string("focus.json"));
      if (!fileio::write_text(p, focus_trail_.dump(2) + "\n")) fail("cannot write " + p);
      return true;
    }
    Event e;
    e.key = Key::Tab;
    e.type = EventType::KeyDown;
    app_.on_event(e);
    e.type = EventType::KeyUp;
    app_.on_event(e);
    return false;
  }
  if (op == "tabTo") {
    // Keyboard-only navigation: Tab (one press per frame, Shift+Tab with
    // "shift") until the focused control's name contains `label`.
    if (!waiting_) {
      waiting_ = true;
      frames_left_ = cmd["max"].as_int(40);
    }
    const bool exact = cmd["exact"].as_bool(false);
    const std::string& role = cmd["role"].str();
    for (const auto& n : app_.ui().access_nodes()) {
      if (!n.focused || (!role.empty() && role != role_name(n.role))) continue;
      if (exact ? n.label == cmd["label"].str() : contains_ci(n.label, cmd["label"].str())) return true;
    }
    if (frames_left_-- <= 0) {
      fail("tabTo: no focusable control named " + cmd["label"].str());
      return true;
    }
    Event e;
    e.key = Key::Tab;
    e.mods.shift = cmd["shift"].as_bool();
    e.type = EventType::KeyDown;
    app_.on_event(e);
    e.type = EventType::KeyUp;
    app_.on_event(e);
    return false;
  }
  if (op == "type") {
    Event e;
    e.type = EventType::Text;
    e.text = cmd["text"].str();
    app_.on_event(e);
    return true;
  }
  if (op == "drag") {
    Event e;
    const std::string& phase = cmd["phase"].str();
    e.type = phase == "leave" ? EventType::DragLeave : phase == "over" ? EventType::DragOver : EventType::DragEnter;
    e.pos = point_or_centre(app_, cmd);
    app_.on_event(e);
    return true;
  }
  if (op == "expect") {
    if (cmd.contains("path")) {
      const Json state = app_.dump_state();
      if (!check(cmd, state)) {
        const Json* v = lookup(state, cmd["path"].str());
        fail("expected " + cmd.dump() + ", got " + (v ? v->dump() : "nothing"));
      }
    }
    if (cmd.contains("file")) {
      const std::string fp = out_path(cmd["file"].str());
      const auto data = fileio::read_text(fp);
      if (!data) {
        fail("expected file " + fp);
      } else if (cmd.contains("content") && *data != cmd["content"].str()) {
        fail("file " + fp + " differs: " + json_quote(data->substr(0, 120)));
      }
    }
    return true;
  }
  if (op == "log") {
    std::fprintf(stderr, "automation: %s\n", cmd["message"].str().c_str());
    log_.push_back(cmd["message"].str());
    return true;
  }
  if (op == "quit") {
    index_ = steps_.size();
    return true;
  }
  fail("unknown op '" + op + "'");
  return true;
}

}  // namespace cvd::app
