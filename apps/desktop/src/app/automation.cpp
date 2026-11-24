// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "app/automation.hpp"

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

}  // namespace

Automation::Automation(App& app, const std::string& script_path, const std::string& out_dir)
    : app_(app), script_dir_(path_parent(script_path)), out_dir_(out_dir) {
  if (script_dir_.empty()) script_dir_ = ".";
  if (out_dir_.empty()) out_dir_ = script_dir_;
  fileio::make_directories(out_dir_);
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

void Automation::fail(const std::string& message) {
  ++failures_;
  log_.push_back("FAIL: " + message);
  std::fprintf(stderr, "automation: FAIL: %s\n", message.c_str());
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

bool Automation::click_label(const std::string& label, bool exact) {
  const ui::AccessNode* found = nullptr;
  for (const auto& n : app_.ui().access_nodes()) {
    if (n.rect.empty() || !n.enabled) continue;
    if (exact ? n.label == label : contains_ci(n.label, label)) {
      found = &n;
      if (n.role == ui::Role::Button || n.role == ui::Role::MenuItem) break;
    }
  }
  if (!found) return false;
  const float z = app_.zoom();
  const Vec2 c = found->rect.center() * z;
  Event e;
  e.type = EventType::MouseMove;
  e.pos = c;
  app_.on_event(e);
  e.type = EventType::MouseDown;
  app_.on_event(e);
  e.type = EventType::MouseUp;
  app_.on_event(e);
  return true;
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
      Event e;
      e.type = EventType::Drop;
      e.paths = files;
      app_.on_event(e);
    } else {
      app_.add_files(files);
    }
    return true;
  }
  if (op == "wait") {
    if (!waiting_) {
      waiting_ = true;
      wait_until_ = now + cmd["ms"].as_number(0) / 1000.0;
    }
    return now >= wait_until_;
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
        if (n.role == ui::Role::Button && n.label == ascii_upper(it->target)) {
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
  if (op == "waitIdle") {
    st.wait_idle();
    st.poll(now);
    // Let the UI show the finished state for a few frames.
    if (!waiting_) {
      waiting_ = true;
      idle_frames_ = cmd["frames"].as_int(2);
    }
    return idle_frames_-- <= 0;
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
    if (!fileio::write_text(p, app_.dump_state().dump(2) + "\n")) fail("cannot write " + p);
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
    if (cmd.contains("label")) {
      if (!click_label(cmd["label"].str(), cmd["exact"].as_bool(false))) fail("no element labelled " + cmd["label"].str());
    } else {
      Event e;
      e.pos = {static_cast<float>(cmd["x"].as_number()), static_cast<float>(cmd["y"].as_number())};
      e.type = EventType::MouseMove;
      app_.on_event(e);
      e.type = EventType::MouseDown;
      app_.on_event(e);
      e.type = EventType::MouseUp;
      app_.on_event(e);
    }
    return true;
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
  if (op == "key") {
    Event e;
    e.type = EventType::KeyDown;
    e.key = key_from_name(cmd["key"].str());
    e.mods.shift = cmd["shift"].as_bool();
    e.mods.alt = cmd["alt"].as_bool();
    if (cmd["primary"].as_bool()) {
#if defined(__APPLE__)
      e.mods.super = true;
#else
      e.mods.ctrl = true;
#endif
    }
    app_.on_event(e);
    e.type = EventType::KeyUp;
    app_.on_event(e);
    return true;
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
    e.type = phase == "leave" ? EventType::DragLeave : EventType::DragEnter;
    e.pos = {app_.ui().size().x * 0.5f, app_.ui().size().y * 0.5f};
    app_.on_event(e);
    return true;
  }
  if (op == "expect") {
    const Json state = app_.dump_state();
    const std::string path = cmd["path"].str();
    const Json* v = lookup(state, path);
    if (cmd.contains("equals")) {
      if (!v || !(*v == cmd["equals"])) fail("expected " + path + " == " + cmd["equals"].dump() + ", got " + (v ? v->dump() : "nothing"));
    } else if (cmd.contains("exists")) {
      if ((v != nullptr) != cmd["exists"].as_bool(true)) fail("expected " + path + (cmd["exists"].as_bool(true) ? " to exist" : " to be absent"));
    } else if (cmd.contains("min")) {
      if (!v || v->as_number() < cmd["min"].as_number()) fail("expected " + path + " >= " + cmd["min"].dump());
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
