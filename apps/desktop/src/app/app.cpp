// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "app/app.hpp"

#include <algorithm>
#include <cmath>

#include "app/automation.hpp"
#include "app/logo.hpp"
#include "base/resources.hpp"
#include "base/text_util.hpp"
#include "platform/fileio.hpp"

namespace cvd::app {

using namespace ui;

const char* screen_name(Screen s) {
  switch (s) {
    case Screen::Home: return "home";
    case Screen::Formats: return "formats";
    case Screen::Settings: return "settings";
    case Screen::Licenses: return "licenses";
  }
  return "home";
}

App::App(Platform& platform, AppOptions options) : platform_(platform), options_(std::move(options)) {
  install_bundled_fonts();
  Tokens::instance().load(resource_text("brand/tokens.json"));
  config_dir_ = options_.config_dir.empty() ? platform_.config_directory() : options_.config_dir;
  const std::string temp = options_.temp_dir.empty() ? fileio::make_session_temp_dir() : options_.temp_dir;
  state_ = std::make_unique<AppState>(temp, [this] { platform_.request_frame(); }, options_.workers);
  state_->settings = load_settings(config_dir_);
  ui_.host().get_clipboard = [this] { return platform_.clipboard_text(); };
  ui_.host().set_clipboard = [this](const std::string& s) { platform_.set_clipboard_text(s); };
  system_dark_ = platform_.system_dark_mode();
  system_reduce_motion_ = platform_.system_reduce_motion();
  resolve_theme();
  update_reduced_motion();
  if (options_.skip_intro) intro_phase_ = 2;
  if (!options_.open_files.empty()) add_files(options_.open_files);
  if (!options_.automation_script.empty()) {
    automation_ = std::make_unique<Automation>(*this, options_.automation_script, options_.automation_out);
  }
}

App::~App() {
  automation_.reset();
  const std::string temp = state_->temp_dir();
  state_->clear();
  state_.reset();
  // The session directory only ever holds this run's outputs.
  if (options_.temp_dir.empty() && temp.find("convertible-") != std::string::npos) fileio::remove_all(temp);
}

int App::automation_failures() const { return automation_ ? automation_->failures() : 0; }

void App::resolve_theme() {
  bool dark = system_dark_;
  if (state_->settings.theme == ThemeMode::Dark) dark = true;
  if (state_->settings.theme == ThemeMode::Light) dark = false;
  theme_ = Theme::build(Tokens::instance(), dark);
}

void App::update_reduced_motion() {
  const MotionMode m = state_->settings.motion;
  ui_.reduced_motion = m == MotionMode::Reduced || (m == MotionMode::System && system_reduce_motion_);
}

void App::apply_settings(const Settings& s) {
  const bool changed = !(s == state_->settings);
  state_->settings = s;
  resolve_theme();
  update_reduced_motion();
  if (changed) persist_settings();
  ui_.request_frame();
}

void App::persist_settings() { save_settings(config_dir_, state_->settings); }

// ---------------------------------------------------------------------------
// Events and frames
// ---------------------------------------------------------------------------

void App::on_event(const Event& in) {
  Event e = in;
  const float z = zoom();
  e.pos = e.pos / z;
  e.scroll = e.scroll / z;
  switch (e.type) {
    case EventType::Drop:
    case EventType::OpenFiles:
      drag_active_ = false;
      ui_.handle_event(e);
      if (intro_active()) skip_intro();
      add_files(e.paths);
      return;
    case EventType::DragEnter:
    case EventType::DragOver: drag_active_ = true; break;
    case EventType::DragLeave: drag_active_ = false; break;
    case EventType::AppearanceChanged:
      system_dark_ = platform_.system_dark_mode();
      system_reduce_motion_ = platform_.system_reduce_motion();
      resolve_theme();
      update_reduced_motion();
      break;
    case EventType::CloseRequested: running_ = false; break;
    case EventType::MouseDown:
    case EventType::KeyDown:
      if (intro_phase_ == 0) {
        skip_intro();
        return;  // the gesture only skips the intro
      }
      break;
    default: break;
  }
  ui_.handle_event(e);
  ui_.request_frame();
}

void App::skip_intro() {
  if (intro_phase_ == 0) {
    intro_phase_ = 1;
    intro_fade_start_ = now_;
  }
}

bool App::wants_frame() const {
  return ui_.needs_frame() || state_->any_busy() || intro_phase_ < 2 || !toasts_.empty() ||
         !completion_times_.empty() || (automation_ && !automation_->finished());
}

double App::wake_time() const {
  double t = ui_.wake_at();
  for (const Toast& toast : toasts_) {
    const double end = toast.shown_at + toast.duration;
    if (t == 0.0 || end < t) t = end;
  }
  return t;
}

std::vector<AccessibleItem> App::accessibility() const {
  std::vector<AccessibleItem> out;
  const float z = zoom();
  for (const auto& n : ui_.access_nodes()) {
    if (n.rect.empty()) continue;
    AccessibleItem a;
    a.role = static_cast<AccessibleItem::Role>(static_cast<int>(n.role));
    a.label = n.label;
    a.value = n.value;
    a.rect = {n.rect.x * z, n.rect.y * z, n.rect.w * z, n.rect.h * z};
    a.focused = n.focused;
    a.selected = n.selected;
    a.enabled = n.enabled;
    out.push_back(std::move(a));
  }
  return out;
}

CursorShape App::cursor() const {
  switch (ui_.cursor()) {
    case Cursor::Hand: return CursorShape::Hand;
    case Cursor::Text: return CursorShape::Text;
    case Cursor::ResizeH: return CursorShape::ResizeH;
    default: return CursorShape::Arrow;
  }
}

void App::on_frame(gfx::Bitmap& target, float scale, double now) {
  now_ = now;
  scale_ = scale;
  if (intro_start_ < 0.0) intro_start_ = now;
  if (automation_) automation_->before_frame(now);

  if (state_->poll(now)) ui_.request_frame();
  for (const auto& item : state_->items()) {
    if (item.status == ItemStatus::Done &&
        std::find(completed_seen_.begin(), completed_seen_.end(), item.id) == completed_seen_.end()) {
      completed_seen_.push_back(item.id);
      completion_times_.emplace_back(item.id, now);
    }
  }
  completion_times_.erase(std::remove_if(completion_times_.begin(), completion_times_.end(),
                                         [&](const auto& p) { return now - p.second > 0.6; }),
                          completion_times_.end());
  for (const std::string& n : state_->notices) toast(n, {}, {}, true);
  state_->notices.clear();

  const float z = zoom();
  const float s = scale * z;
  logical_size_ = {static_cast<float>(target.width) / s, static_cast<float>(target.height) / s};
  text::reset_glyph_stats();
  ui_.begin_frame(logical_size_, now, &theme_);
  build();
  ui_.end_frame();

  gfx::Canvas canvas(target);
  canvas.clear(theme_.color.background);
  canvas.scale(s, s);
  ui_.render(canvas);
  glyphs_last_frame_ = text::glyph_stats();

  if (automation_) automation_->after_frame(target);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void App::add_files(const std::vector<std::string>& paths) {
  const std::size_t before = state_->items().size();
  const int added = state_->add_files(paths);
  if (added > 0) {
    navigate(Screen::Home);
    if (before == 0 && added > 0) ui_.request_frame();
  }
  ui_.request_frame();
}

void App::choose_files() {
  const auto done = [this](std::vector<std::string> paths) {
    if (!paths.empty()) add_files(paths);
  };
  if (seams.dialog) {
    seams.dialog("open", "Choose files to convert", last_save_dir_, {}, done);
  } else if (platform_.has_native_dialogs()) {
    platform_.open_files_dialog("Choose files to convert", done);
  } else {
    browser_ = std::make_unique<FileBrowser>(FileBrowser::Mode::OpenFiles, "Choose files to convert",
                                             last_save_dir_.empty() ? platform_.documents_directory() : last_save_dir_,
                                             std::string(), done);
  }
}

void App::navigate(Screen s) {
  if (screen_ != s) {
    screen_ = s;
    close_picker();
    menu_.open = false;
  }
}

void App::open_picker(std::uint64_t item_id, const std::string& group_source) {
  picker_.open = true;
  picker_.item_id = item_id;
  picker_.group_source = group_source;
  picker_.highlight = 0;
  picker_.opened_at = now_;
  picker_.focus_search = true;
  picker_query.clear();
}

void App::close_picker() { picker_.open = false; }

void App::select_target(const std::string& target) {
  if (!picker_.group_source.empty()) {
    state_->set_target_for_source(picker_.group_source, target);
  } else {
    state_->set_target(picker_.item_id, target);
  }
  close_picker();
}

void App::convert_all() {
  const int n = state_->start_all();
  if (n > 0) ui_.request_frame();
}

void App::save_item(std::uint64_t id) {
  QueueItem* item = state_->find(id);
  if (!item || item->status != ItemStatus::Done) return;
  const std::string dir = last_save_dir_.empty() ? path_parent(item->path) : last_save_dir_;
  // Never suggest an existing file (e.g. the source itself when a text file is
  // re-encoded to .txt): accepting the suggestion must not overwrite anything.
  const std::string name = path_filename(fileio::unique_path(dir, item->output_name));
  const auto done = [this, id](std::optional<std::string> path) {
    if (!path) return;
    if (state_->save_to(id, *path)) {
      last_save_dir_ = path_parent(*path);
      toast("Saved " + path_filename(*path), "Show in " + platform_.file_manager_name(), *path);
    } else {
      toast("Couldn\xE2\x80\x99t save to " + path_filename(*path) + ". Check the folder is writable.", {}, {}, true);
    }
  };
  if (seams.dialog) {
    seams.dialog("save", "Save converted file", dir, name, [done](std::vector<std::string> r) {
      if (r.empty()) done(std::nullopt);
      else done(r.front());
    });
  } else if (platform_.has_native_dialogs()) {
    platform_.save_file_dialog("Save converted file", dir, name, done);
  } else {
    browser_ = std::make_unique<FileBrowser>(FileBrowser::Mode::SaveFile, "Save converted file", dir, name,
                                             [done](std::vector<std::string> r) {
                                               if (r.empty()) done(std::nullopt);
                                               else done(r.front());
                                             });
  }
}

void App::save_all() {
  if (state_->count(ItemStatus::Done) == 0) return;
  std::string dir = last_save_dir_;
  if (dir.empty()) {
    for (const auto& item : state_->items()) {
      if (item.status == ItemStatus::Done) {
        dir = path_parent(item.path);
        break;
      }
    }
  }
  const auto done = [this](std::optional<std::string> folder) {
    if (!folder) return;
    const int n = state_->save_all_to(*folder);
    last_save_dir_ = *folder;
    if (n > 0) {
      toast("Saved " + std::to_string(n) + (n == 1 ? " file to " : " files to ") + path_filename(*folder),
            "Show in " + platform_.file_manager_name(), *folder);
    } else {
      toast("Nothing was saved. Check the folder is writable.", {}, {}, true);
    }
  };
  if (seams.dialog) {
    seams.dialog("folder", "Save all converted files", dir, {}, [done](std::vector<std::string> r) {
      if (r.empty()) done(std::nullopt);
      else done(r.front());
    });
  } else if (platform_.has_native_dialogs()) {
    platform_.choose_folder_dialog("Save all converted files", dir, done);
  } else {
    browser_ = std::make_unique<FileBrowser>(FileBrowser::Mode::ChooseFolder, "Save all converted files to\xE2\x80\xA6",
                                             dir, std::string(), [done](std::vector<std::string> r) {
                                               if (r.empty()) done(std::nullopt);
                                               else done(r.front());
                                             });
  }
}

void App::reveal_item(std::uint64_t id) {
  const QueueItem* item = state_->find(id);
  if (!item) return;
  const std::string path = !item->saved_path.empty() ? item->saved_path : item->path;
  if (!reveal_path(path)) toast("Couldn\xE2\x80\x99t open the file manager.", {}, {}, true);
}

bool App::reveal_path(const std::string& path) {
  return seams.shell ? seams.shell("reveal", path) : platform_.reveal_in_file_manager(path);
}

bool App::open_path(const std::string& path) {
  return seams.shell ? seams.shell("open", path) : platform_.open_with_default_app(path);
}

void App::open_item(std::uint64_t id) {
  const QueueItem* item = state_->find(id);
  if (!item) return;
  const std::string path = !item->saved_path.empty() ? item->saved_path : item->output_path;
  if (path.empty() || !open_path(path)) {
    toast("No app is set up to open this kind of file.", {}, {}, true);
  }
}

void App::toast(std::string message, std::string action_label, std::string action_path, bool error) {
  Toast t;
  t.message = std::move(message);
  t.action_label = std::move(action_label);
  t.action_path = std::move(action_path);
  t.shown_at = now_;
  t.error = error;
  t.duration = error ? 6.0 : 4.5;
  t.id = next_toast_id_++;
  toasts_.push_back(std::move(t));
  while (toasts_.size() > 3) toasts_.pop_front();
  ui_.request_frame();
}

void App::open_menu(WidgetId owner, Rect anchor, std::vector<std::string> choices, int selected,
                    std::function<void(int)> on_pick) {
  menu_.open = true;
  menu_.owner = owner;
  menu_.anchor = anchor;
  menu_.choices = std::move(choices);
  menu_.selected = selected;
  menu_.highlight = std::max(0, selected);
  menu_.on_pick = std::move(on_pick);
  menu_.opened_at = now_;
}

// ---------------------------------------------------------------------------
// Frame building
// ---------------------------------------------------------------------------

void App::build() {
  const Vec2 size = ui_.size();
  Rect all{0.0f, 0.0f, size.x, size.y};
  // Soft brand glow at the top of the window.
  ui_.draw([size, dark = theme_.dark, accent = theme_.color.accent](gfx::Canvas& c) {
    const float r = std::max(size.x, size.y) * 0.75f;
    c.fill_rect({0, 0, size.x, size.y * 0.9f},
                gfx::Paint::radial({size.x * 0.18f, -size.y * 0.15f}, r,
                                   {{0.0f, accent.fade(dark ? 0.16f : 0.09f)}, {1.0f, accent.fade(0.0f)}}));
  });

  Rect top = cut_top(all, 64.0f);
  build_top_bar(top);

  // Content enters gently after the intro.
  float enter = 1.0f;
  if (intro_phase_ == 1) {
    enter = clampf(static_cast<float>((now_ - intro_fade_start_) / theme_.motion.slow), 0.0f, 1.0f);
    enter = theme_.motion.decelerate(enter);
  } else if (intro_phase_ == 0) {
    enter = 0.0f;
  }
  ui_.push_opacity(0.0f + enter);
  ui_.push_translate({0.0f, (1.0f - enter) * 12.0f});
  switch (screen_) {
    case Screen::Home: build_home(all); break;
    case Screen::Formats: build_formats(all); break;
    case Screen::Settings: build_settings(all); break;
    case Screen::Licenses: build_licenses(all); break;
  }
  ui_.pop_translate();
  ui_.pop_opacity();

  if (picker_.open) build_picker();
  if (menu_.open) build_menu();
  build_toasts();
  if (drag_active_ || ui_.drag_hovering()) build_drop_overlay();
  if (browser_) {
    if (!browser_->build(ui_, ui_.size())) browser_.reset();
  }
  handle_shortcuts();
  build_intro();
}

void App::handle_shortcuts() {
  Modifiers m;
  if (ui_.key_pressed(Key::Escape, false)) {
    if (picker_.open) {
      ui_.key_pressed(Key::Escape);
      close_picker();
    } else if (menu_.open) {
      ui_.key_pressed(Key::Escape);
      menu_.open = false;
    } else if (screen_ == Screen::Licenses) {
      ui_.key_pressed(Key::Escape);
      navigate(Screen::Settings);
    } else if (screen_ != Screen::Home) {
      ui_.key_pressed(Key::Escape);
      navigate(Screen::Home);
    }
  }
  const auto shortcut = [&](Key k) {
    Modifiers km;
    if (ui_.key_pressed(k, km, false) && km.primary()) {
      ui_.key_pressed(k);
      m = km;
      return true;
    }
    return false;
  };
  if (browser_) return;
  if (shortcut(Key::O)) choose_files();
  if (shortcut(Key::Enter)) convert_all();
  if (shortcut(Key::S)) save_all();
  if (shortcut(Key::Comma)) navigate(Screen::Settings);
  if (shortcut(Key::Digit1)) navigate(Screen::Home);
  if (shortcut(Key::Digit2)) navigate(Screen::Formats);
  if (shortcut(Key::Digit3)) navigate(Screen::Settings);
  const auto set_scale = [&](int pct) {
    Settings s = state_->settings;
    s.text_scale_percent = pct;
    apply_settings(s);
  };
  const int pct = state_->settings.text_scale_percent;
  if (shortcut(Key::Plus)) set_scale(pct >= 115 ? 130 : 115);
  if (shortcut(Key::Minus)) set_scale(pct <= 115 ? 100 : 115);
  if (shortcut(Key::Digit0)) set_scale(100);
  if (shortcut(Key::Q) || shortcut(Key::W)) {
#if !defined(__APPLE__)
    running_ = false;
#endif
  }
}

void App::build_intro() {
  if (intro_phase_ >= 2) return;
  const Theme& t = theme_;
  const Logo& logo = Logo::instance();
  const float duration = logo.intro_duration_ms();
  float t_ms = static_cast<float>((now_ - intro_start_) * 1000.0);
  if (ui_.reduced_motion && intro_phase_ == 0) {
    // Reduced motion: a plain 140 ms fade of the static mark.
    intro_phase_ = 1;
    intro_fade_start_ = now_;
  }
  if (intro_phase_ == 0 && t_ms >= duration) {
    intro_phase_ = 1;
    intro_fade_start_ = now_;
  }
  const float fade_dur = ui_.reduced_motion ? t.motion.fast : t.motion.slow;
  float overlay = 1.0f;
  if (intro_phase_ == 1) {
    const float u = clampf(static_cast<float>((now_ - intro_fade_start_) / fade_dur), 0.0f, 1.0f);
    overlay = 1.0f - t.motion.standard(u);
    if (u >= 1.0f) {
      intro_phase_ = 2;
      return;
    }
    t_ms = std::max(t_ms, duration);
  }
  ui_.request_frame();
  const Layer saved = ui_.layer();
  ui_.set_layer(Layer::Modal);
  if (intro_phase_ == 0) ui_.block_below(Layer::Modal);
  const Vec2 size = ui_.size();
  ui_.fill({0, 0, size.x, size.y}, 0.0f, t.color.background.fade(overlay));
  const float box = std::clamp(std::min(size.x, size.y) * 0.24f, 96.0f, 200.0f);
  const float shrink = intro_phase_ == 1 ? 1.0f - 0.08f * (1.0f - overlay) : 1.0f;
  const Rect r = centered({0, 0, size.x, size.y}, box * shrink, box * shrink);
  const bool light = !t.dark;
  ui_.describe(ui_.id("intro"), Role::Image, "Convertible", r);
  ui_.draw([r, t_ms, light, overlay](gfx::Canvas& c) {
    c.save();
    c.set_opacity(overlay);
    Logo::instance().draw_intro(c, r, t_ms, light);
    c.restore();
  });
  ui_.set_layer(saved);
}

void App::build_drop_overlay() {
  const Theme& t = theme_;
  const Layer saved = ui_.layer();
  ui_.set_layer(Layer::Modal);
  const float a = ui_.animate(ui_.id("drop-overlay"), 1.0f, t.motion.fast, t.motion.standard);
  const Vec2 size = ui_.size();
  ui_.fill({0, 0, size.x, size.y}, 0.0f, t.color.background.fade(0.95f * a));
  const Rect zone = Rect{0, 0, size.x, size.y}.inset(20.0f);
  ui_.fill(zone, t.radius_xxl, t.color.accent_subtle.fade(0.55f * a));
  ui_.stroke(zone, t.radius_xxl, 2.0f, t.color.accent.fade(a));
  const Rect logo_r = centered(zone, 84.0f, 84.0f).offset(0.0f, -36.0f);
  const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now_) * 4.0f);
  const bool light = !t.dark;
  ui_.draw([logo_r, light, pulse, a](gfx::Canvas& c) {
    c.save();
    c.set_opacity(a);
    const float k = 1.0f + 0.04f * pulse;
    const Rect rr = centered(logo_r, logo_r.w * k, logo_r.h * k);
    Logo::instance().draw_static(c, rr, light);
    c.restore();
  });
  ui_.request_frame();
  ui_.describe(ui_.id("drop-overlay-node"), Role::Group, "Drop to add files", zone);
  ui_.text_in("Drop to add files", t.type.title2, {zone.x, logo_r.bottom() + 20.0f, zone.w, 32.0f},
              t.color.text_primary.fade(a), 0.5f);
  ui_.text_in("They stay on this device", t.type.body, {zone.x, logo_r.bottom() + 54.0f, zone.w, 24.0f},
              t.color.text_secondary.fade(a), 0.5f);
  ui_.set_layer(saved);
}

void App::build_toasts() {
  const Theme& t = theme_;
  const Vec2 size = ui_.size();
  toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                               [&](const Toast& x) { return now_ - x.shown_at > x.duration + 0.3; }),
                toasts_.end());
  if (toasts_.empty()) return;
  const Layer saved = ui_.layer();
  ui_.set_layer(Layer::Overlay);
  const bool has_bar = screen_ == Screen::Home && !state_->empty();
  float y = size.y - (has_bar ? 96.0f : 24.0f);
  for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
    Toast& toast = *it;
    const float age = static_cast<float>(now_ - toast.shown_at);
    const float in = t.motion.decelerate(clampf(age / t.motion.base, 0.0f, 1.0f));
    const float out = clampf((static_cast<float>(toast.duration) + 0.3f - age) / 0.3f, 0.0f, 1.0f);
    const float a = std::min(in, out);
    const text::Style& st = t.type.body;
    const float action_w = toast.action_label.empty() ? 0.0f : text::measure(toast.action_label, t.type.body_strong) + 24.0f;
    const float max_text = std::min(520.0f, size.x - 120.0f - action_w);
    const std::string msg = text::ellipsize_middle(toast.message, st, max_text);
    const float w = text::measure(msg, st) + action_w + 40.0f + 36.0f;
    const float h = 48.0f;
    y -= h;
    Rect r{(size.x - w) * 0.5f, y + (1.0f - in) * 16.0f, w, h};
    ui_.push_opacity(a);
    ui_.push_id(toast.id);
    panel(ui_, ui_.id("toast"), r, h * 0.5f, 3);
    Rect inner = r.inset(16.0f, 0.0f);
    icon(ui_, toast.error ? Icon::Warning : Icon::Check, cut_left(inner, 20.0f).inset(0.0f, 14.0f),
         toast.error ? t.color.error : t.color.success);
    cut_left(inner, 10.0f);
    ui_.describe(ui_.id("toast-msg"), Role::Label, toast.message, r);
    if (icon_button(ui_, ui_.id("toast-close"), cut_right(inner, 28.0f).inset(0.0f, 10.0f), Icon::Close, "Dismiss")) {
      toast.duration = std::min(toast.duration, static_cast<double>(age));
    }
    if (!toast.action_label.empty()) {
      ButtonOptions bo;
      bo.kind = ButtonKind::Link;
      const Rect br = cut_right(inner, action_w);
      if (button(ui_, ui_.id("toast-action"), br, toast.action_label, bo)) {
        reveal_path(toast.action_path);
      }
    }
    ui_.text_in(msg, st, inner, t.color.text_primary);
    ui_.pop_id();
    ui_.pop_opacity();
    y -= 10.0f;
    if (a < 1.0f) ui_.request_frame();
  }
  ui_.set_layer(saved);
}

void App::build_menu() {
  const Theme& t = theme_;
  const Layer saved = ui_.layer();
  ui_.set_layer(Layer::Overlay);
  const float row_h = 36.0f;
  const float w = std::max(menu_.anchor.w, 180.0f);
  const float h = std::min(static_cast<float>(menu_.choices.size()) * row_h + 12.0f, 320.0f);
  const Vec2 size = ui_.size();
  float y = menu_.anchor.bottom() + 6.0f;
  if (y + h > size.y - 12.0f) y = menu_.anchor.y - h - 6.0f;
  const Rect r{clampf(menu_.anchor.x, 12.0f, size.x - w - 12.0f), y, w, h};
  const float appear = ui_.animate(ui_.id("menu-appear", menu_.owner), 1.0f, t.motion.fast, t.motion.decelerate);
  ui_.push_opacity(appear);
  panel(ui_, ui_.id("menu-panel"), r, t.radius_md, 3);
  const int n = static_cast<int>(menu_.choices.size());
  if (ui_.key_pressed(Key::Down)) menu_.highlight = std::min(n - 1, menu_.highlight + 1);
  if (ui_.key_pressed(Key::Up)) menu_.highlight = std::max(0, menu_.highlight - 1);
  bool pick = ui_.key_pressed(Key::Enter) || ui_.key_pressed(Key::Space);
  int picked = pick ? menu_.highlight : -1;
  const Rect list = r.inset(6.0f);
  const float off = ui_.begin_scroll(ui_.id("menu-scroll"), list, static_cast<float>(n) * row_h);
  (void)off;
  for (int i = 0; i < n; ++i) {
    const Rect rr{list.x, list.y + static_cast<float>(i) * row_h, list.w, row_h};
    const WidgetId id = ui_.id("menu-item", static_cast<std::uint64_t>(i));
    const Interaction in = ui_.interact(id, rr, kNone);
    ui_.describe(id, Role::MenuItem, menu_.choices[static_cast<std::size_t>(i)], rr, {}, i == menu_.selected);
    if (in.hovered) {
      menu_.highlight = i;
      ui_.set_cursor(Cursor::Hand);
    }
    if (in.clicked) picked = i;
    if (i == menu_.highlight) ui_.fill(rr, t.radius_sm, t.color.accent_subtle);
    ui_.text_in(menu_.choices[static_cast<std::size_t>(i)], t.type.body, rr.inset(12.0f, 0.0f),
                i == menu_.highlight ? t.color.accent_text : t.color.text_primary);
    if (i == menu_.selected) {
      icon(ui_, Icon::Check, {rr.right() - 30.0f, rr.center().y - 9.0f, 18.0f, 18.0f}, t.color.accent_text);
    }
  }
  ui_.end_scroll();
  ui_.pop_opacity();
  if (picked >= 0) {
    if (menu_.on_pick) menu_.on_pick(picked);
    menu_.open = false;
    ui_.set_focus(menu_.owner, ui_.focus_visible());
  } else if (ui_.clicked_outside(r) && appear > 0.0f && now_ - menu_.opened_at > 0.05) {
    menu_.open = false;
  }
  ui_.set_layer(saved);
}

// ---------------------------------------------------------------------------
// State dump
// ---------------------------------------------------------------------------

Json App::dump_state() const {
  Json root = Json::object();
  root.set("screen", screen_name(screen_));
  root.set("theme", theme_.dark ? "dark" : "light");
  root.set("reducedMotion", ui_.reduced_motion);
  root.set("intro", intro_phase_ == 0 ? "playing" : intro_phase_ == 1 ? "fading" : "done");
  Json size = Json::object();
  size.set("width", static_cast<double>(logical_size_.x));
  size.set("height", static_cast<double>(logical_size_.y));
  size.set("scale", static_cast<double>(scale_));
  size.set("zoom", static_cast<double>(zoom()));
  root.set("window", size);
  root.set("coreVersion", core_version());
  root.set("appVersion", CVD_VERSION_STRING);
  root.set("settings", state_->settings.to_json());
  Json items = Json::array();
  for (const auto& item : state_->items()) {
    Json j = Json::object();
    j.set("id", static_cast<double>(item.id));
    j.set("name", item.name);
    j.set("size", static_cast<double>(item.size));
    j.set("source", item.source);
    j.set("category", item.category);
    j.set("target", item.target);
    j.set("targets", static_cast<double>(item.targets.size()));
    j.set("status", status_name(item.status));
    j.set("progress", static_cast<double>(item.progress));
    j.set("stage", item.stage);
    j.set("options", item.values);
    j.set("sourceLabel", item.source_label);
    j.set("advancedOpen", item.advanced_open);
    Json target_ids = Json::array();
    for (const auto& t : item.targets) target_ids.push_back(t.id);
    j.set("targetIds", target_ids);
    Json specs = Json::array();
    static const char* types[] = {"int", "float", "bool", "enum", "string"};
    for (const auto& o : item.options) {
      Json spec = Json::object();
      spec.set("key", o.key);
      spec.set("label", o.label);
      spec.set("type", types[static_cast<int>(o.type)]);
      spec.set("default", o.default_value);
      if (o.min) spec.set("min", *o.min);
      if (o.max) spec.set("max", *o.max);
      Json ch = Json::array();
      for (const auto& c : o.choices) ch.push_back(c);
      spec.set("choices", ch);
      spec.set("advanced", o.advanced);
      specs.push_back(spec);
    }
    j.set("optionSpecs", specs);
    j.set("outputPath", item.output_path);
    j.set("hasPreset", has_preset(item.options));
    std::string compress;
    for (const auto& t : item.targets) {
      if (t.compress) compress = t.id;
    }
    j.set("compressTarget", compress);
    if (item.status == ItemStatus::Done) {
      j.set("outputName", item.output_name);
      j.set("outputSize", static_cast<double>(item.output_size));
      j.set("savedPath", item.saved_path);
    }
    if (item.error_status != 0) {
      j.set("errorStatus", item.error_status);
      j.set("errorTitle", status_title(item.error_status));
      j.set("errorMessage", item.error_message);
    }
    Json warnings = Json::array();
    for (const auto& w : item.warnings) warnings.push_back(w);
    j.set("warnings", warnings);
    items.push_back(j);
  }
  root.set("items", items);
  Json picker = Json::object();
  picker.set("open", picker_.open);
  picker.set("query", picker_query);
  picker.set("highlight", picker_.highlight);
  picker.set("groupSource", picker_.group_source);
  if (picker_.open) {
    if (const QueueItem* item = state_->find(picker_.item_id)) {
      Json groups = Json::array();
      for (const auto& g : group_targets(item->targets, item->category, picker_query)) {
        Json group = Json::object();
        group.set("category", g.category);
        Json ids = Json::array();
        for (const TargetInfo* t : g.targets) ids.push_back(t->id);
        group.set("targets", ids);
        groups.push_back(group);
      }
      picker.set("groups", groups);
      picker.set("sourceCategory", item->category);
    }
  }
  root.set("picker", picker);
  Json toasts = Json::array();
  for (const auto& toast : toasts_) toasts.push_back(toast.message);
  root.set("toasts", toasts);
  Json nodes = Json::array();
  static const char* roles[] = {"button", "toggle", "slider", "textfield", "tab", "menuitem",
                                "listitem", "label", "group", "image", "progress", "link"};
  for (const auto& n : ui_.access_nodes()) {
    if (n.rect.empty()) continue;
    Json j = Json::object();
    j.set("role", roles[static_cast<int>(n.role)]);
    j.set("label", n.label);
    if (!n.value.empty()) j.set("value", n.value);
    Json rect = Json::array();
    rect.push_back(static_cast<double>(std::round(n.rect.x)));
    rect.push_back(static_cast<double>(std::round(n.rect.y)));
    rect.push_back(static_cast<double>(std::round(n.rect.w)));
    rect.push_back(static_cast<double>(std::round(n.rect.h)));
    j.set("rect", rect);
    Json full = Json::array();
    for (const float v : {n.full.x, n.full.y, n.full.w, n.full.h}) full.push_back(static_cast<double>(std::round(v)));
    if (!(full == rect)) j.set("full", full);
    if (n.scrolled) j.set("scrolled", true);
    if (n.layer != Layer::Base) j.set("layer", static_cast<int>(n.layer));
    if (n.focused) j.set("focused", true);
    if (n.selected) j.set("selected", true);
    if (!n.enabled) j.set("enabled", false);
    nodes.push_back(j);
  }
  root.set("accessibility", nodes);

  // Diagnostics for the end-to-end tests.
  root.set("time", now_);
  root.set("introMs", intro_start_ < 0.0 ? 0.0 : (now_ - intro_start_) * 1000.0);
  root.set("dropOverlay", drag_active_ || ui_.drag_hovering());
  root.set("focusVisible", ui_.focus_visible());
  root.set("tempDir", state_->temp_dir());
  root.set("configDir", config_dir_);
  Json glyphs = Json::object();
  glyphs.set("shaped", static_cast<double>(glyphs_last_frame_.shaped));
  glyphs.set("missing", static_cast<double>(glyphs_last_frame_.missing));
  Json missing_cps = Json::array();
  for (const std::uint32_t cp : glyphs_last_frame_.missing_codepoints) missing_cps.push_back(static_cast<double>(cp));
  glyphs.set("missingCodepoints", missing_cps);
  root.set("glyphs", glyphs);
  Json a11y = Json::object();
  a11y.set("focusable", static_cast<double>(ui_.focusable_count()));
  Json undescribed = Json::array();
  for (const Rect& r : ui_.undescribed_focusables()) {
    Json rect = Json::array();
    for (const float v : {r.x, r.y, r.w, r.h}) rect.push_back(static_cast<double>(std::round(v)));
    undescribed.push_back(rect);
  }
  a11y.set("undescribed", undescribed);
  root.set("a11y", a11y);
  Json formats = Json::object();
  formats.set("query", formats_query_);
  formats.set("category", formats_category_);
  formats.set("matched", formats_matched_);
  formats.set("total", static_cast<double>(Catalog::instance().formats().size()));
  root.set("formats", formats);
  Json menu = Json::object();
  menu.set("open", menu_.open);
  Json choices = Json::array();
  for (const auto& c : menu_.choices) choices.push_back(c);
  menu.set("choices", choices);
  menu.set("highlight", menu_.highlight);
  root.set("menu", menu);
  Json browser = Json::object();
  browser.set("open", browser_ != nullptr);
  if (browser_) {
    const FileBrowser::Mode m = browser_->mode();
    browser.set("mode", m == FileBrowser::Mode::OpenFiles ? "open" : m == FileBrowser::Mode::SaveFile ? "save" : "folder");
    browser.set("directory", browser_->directory());
    browser.set("fileName", browser_->file_name);
    Json entries = Json::array();
    for (const auto& e : browser_->entries()) entries.push_back(e.name);
    browser.set("entries", entries);
  }
  root.set("browser", browser);
  if (automation_) root.set("automation", automation_->dump());
  return root;
}

}  // namespace cvd::app
