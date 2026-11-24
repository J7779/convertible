// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "app/file_browser.hpp"

#include <algorithm>

#include "base/text_util.hpp"
#include "ui/widgets.hpp"

namespace cvd::app {

FileBrowser::FileBrowser(Mode mode, std::string title, const std::string& start_dir, std::string suggested_name,
                         Done done)
    : file_name(std::move(suggested_name)), mode_(mode), title_(std::move(title)), done_(std::move(done)) {
  navigate(start_dir.empty() || !fileio::is_directory(start_dir) ? fileio::home_directory() : start_dir);
}

void FileBrowser::refresh() {
  entries_ = fileio::list_directory(dir_);
  if (!show_hidden) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const auto& e) { return e.hidden; }),
                   entries_.end());
  }
  if (mode_ == Mode::ChooseFolder) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const auto& e) { return !e.is_dir; }),
                   entries_.end());
  }
  std::sort(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
    if (a.is_dir != b.is_dir) return a.is_dir;
    return ascii_lower(a.name) < ascii_lower(b.name);
  });
  selected_.assign(entries_.size(), false);
  cursor_ = 0;
  path_field_ = dir_;
}

void FileBrowser::navigate(const std::string& dir) {
  if (!fileio::is_directory(dir)) return;
  dir_ = fileio::from_path(fileio::to_path(dir).lexically_normal());
  if (dir_.size() > 1 && (dir_.back() == '/' || dir_.back() == '\\')) dir_.pop_back();
  refresh();
}

void FileBrowser::go_up() {
  const std::string parent = path_parent(dir_);
  if (!parent.empty() && parent != dir_) navigate(parent);
}

void FileBrowser::toggle(std::size_t index, bool extend) {
  if (index >= entries_.size()) return;
  cursor_ = index;
  if (entries_[index].is_dir && mode_ != Mode::ChooseFolder) {
    std::fill(selected_.begin(), selected_.end(), false);
    selected_[index] = true;
    return;
  }
  if (mode_ == Mode::OpenFiles && extend) {
    selected_[index] = !selected_[index];
  } else {
    std::fill(selected_.begin(), selected_.end(), false);
    selected_[index] = true;
  }
  if (mode_ == Mode::SaveFile && !entries_[index].is_dir) file_name = entries_[index].name;
}

void FileBrowser::activate(std::size_t index) {
  if (index >= entries_.size()) return;
  if (entries_[index].is_dir) {
    navigate(entries_[index].path);
    return;
  }
  toggle(index, false);
  accept();
}

std::vector<std::string> FileBrowser::selection() const {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (selected_[i]) out.push_back(entries_[i].path);
  }
  return out;
}

void FileBrowser::accept() {
  switch (mode_) {
    case Mode::OpenFiles: {
      std::vector<std::string> files;
      for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (selected_[i] && !entries_[i].is_dir) files.push_back(entries_[i].path);
      }
      if (files.empty()) {
        // A selected folder opens instead.
        for (std::size_t i = 0; i < entries_.size(); ++i) {
          if (selected_[i] && entries_[i].is_dir) {
            navigate(entries_[i].path);
            return;
          }
        }
        return;
      }
      finish(std::move(files));
      break;
    }
    case Mode::SaveFile: {
      const std::string name(trim(file_name));
      if (name.empty()) return;
      finish({fileio::join(dir_, name)});
      break;
    }
    case Mode::ChooseFolder: {
      for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (selected_[i]) {
          finish({entries_[i].path});
          return;
        }
      }
      finish({dir_});
      break;
    }
  }
}

void FileBrowser::cancel() { finish({}); }

void FileBrowser::finish(std::vector<std::string> result) {
  if (closed_) return;
  closed_ = true;
  if (done_) done_(std::move(result));
}

bool FileBrowser::build(ui::Context& ctx, Vec2 size) {
  using namespace ui;
  if (closed_) return false;
  const Theme& t = ctx.theme();
  const Layer saved = ctx.layer();
  ctx.set_layer(Layer::Modal);
  ctx.block_below(Layer::Modal);
  const float appear = ctx.animate(ctx.id("fb-appear"), 1.0f, t.motion.base, t.motion.decelerate);
  ctx.fill({0, 0, size.x, size.y}, 0.0f, t.color.scrim.fade(appear));
  const float w = std::min(720.0f, size.x - 48.0f);
  const float h = std::min(560.0f, size.y - 48.0f);
  Rect sheet = centered({0, 0, size.x, size.y}, w, h);
  sheet.y += (1.0f - appear) * 16.0f;
  ctx.push_opacity(appear);
  panel(ctx, ctx.id("fb-panel"), sheet, t.radius_xl, 4);
  Rect body = sheet.inset(20.0f);
  Rect header = cut_top(body, 32.0f);
  ctx.text_in(title_, t.type.title3, header, t.color.text_primary);
  cut_top(body, 12.0f);

  // Path row: up button + editable path.
  Rect row = cut_top(body, 40.0f);
  if (icon_button(ctx, ctx.id("fb-up"), cut_left(row, 40.0f), Icon::Up, "Enclosing folder")) go_up();
  cut_left(row, 8.0f);
  if (icon_button(ctx, ctx.id("fb-home"), cut_left(row, 40.0f), Icon::Home, "Home folder")) {
    navigate(fileio::home_directory());
  }
  cut_left(row, 8.0f);
  TextFieldOptions po;
  po.monospace = true;
  po.leading = Icon::Folder;
  if (text_field(ctx, ctx.id("fb-path"), row, path_field_, "Folder", po)) {
    // Navigate as soon as the typed path is a folder.
    if (fileio::is_directory(path_field_) && path_field_ != dir_) {
      const std::string typed = path_field_;
      navigate(typed);
      path_field_ = typed;
    }
  }
  cut_top(body, 12.0f);

  // Footer.
  Rect footer = cut_bottom(body, 44.0f);
  cut_bottom(body, 12.0f);
  const char* ok_label = mode_ == Mode::OpenFiles ? "Open" : mode_ == Mode::SaveFile ? "Save" : "Choose";
  ButtonOptions primary;
  primary.kind = ButtonKind::Primary;
  const bool can_accept = mode_ == Mode::SaveFile ? !std::string(trim(file_name)).empty()
                                                  : (mode_ == Mode::ChooseFolder || !selection().empty());
  primary.enabled = can_accept;
  if (button(ctx, ctx.id("fb-ok"), cut_right(footer, 120.0f), ok_label, primary)) accept();
  cut_right(footer, 8.0f);
  if (button(ctx, ctx.id("fb-cancel"), cut_right(footer, 110.0f), "Cancel")) cancel();
  cut_right(footer, 16.0f);
  if (mode_ == Mode::SaveFile) {
    TextFieldOptions fo;
    fo.select_all_on_focus = true;
    text_field(ctx, ctx.id("fb-name"), footer, file_name, "File name", fo);
  } else {
    bool hidden = show_hidden;
    Rect sw = cut_left(footer, 52.0f);
    if (ui::toggle(ctx, ctx.id("fb-hidden"), sw, hidden, "Show hidden files")) {
      show_hidden = hidden;
      refresh();
    }
    ctx.text_in("Show hidden files", t.type.label, footer, t.color.text_secondary);
  }

  // List.
  ctx.fill(body, t.radius_md, t.dark ? t.color.surface_sunken : t.color.background);
  const float row_h = 36.0f;
  const float content = static_cast<float>(entries_.size()) * row_h + 8.0f;
  const WidgetId sid = ctx.id("fb-scroll");
  const float off = ctx.begin_scroll(sid, body, content);
  (void)off;
  const std::size_t first = static_cast<std::size_t>(std::max(0.0f, (off - 8.0f) / row_h));
  const std::size_t last = std::min(entries_.size(), first + static_cast<std::size_t>(body.h / row_h) + 3);
  for (std::size_t i = first; i < last; ++i) {
    const auto& e = entries_[i];
    const Rect rr{body.x + 4.0f, body.y + 4.0f + static_cast<float>(i) * row_h, body.w - 8.0f, row_h};
    const WidgetId rid = ctx.id("fb-row", i);
    const Interaction in = ctx.interact(rid, rr);
    ctx.describe(rid, Role::ListItem, e.name, rr, e.is_dir ? "folder" : "file", selected_[i]);
    if (in.clicked) toggle(i, ctx.mods().primary() || ctx.mods().shift);
    if (in.double_clicked) activate(i);
    if (in.focused && ctx.key_pressed(Key::Enter)) activate(i);
    const Color bg = selected_[i] ? t.color.accent_subtle : (in.hovered ? t.color.surface_elevated : Color{0, 0, 0, 0});
    ctx.fill(rr, t.radius_sm, bg);
    icon(ctx, e.is_dir ? Icon::Folder : Icon::File, {rr.x + 10.0f, rr.center().y - 9.0f, 18.0f, 18.0f},
         e.is_dir ? t.color.accent_text : t.color.text_secondary);
    Rect label{rr.x + 38.0f, rr.y, rr.w - 140.0f, rr.h};
    ctx.text_in(text::ellipsize_middle(e.name, t.type.body, label.w), t.type.body, label,
                selected_[i] ? t.color.accent_text : t.color.text_primary);
    if (!e.is_dir) {
      ctx.text_in(format_size(e.size), t.type.caption, {rr.right() - 100.0f, rr.y, 90.0f, rr.h}, t.color.text_secondary,
                  1.0f);
    }
    ctx.focus_ring(rid, rr, t.radius_sm);
  }
  if (entries_.empty()) {
    ctx.text_in("This folder is empty", t.type.body, {body.x, body.y + 20.0f, body.w, 24.0f}, t.color.text_secondary,
                0.5f);
  }
  ctx.end_scroll();
  ctx.pop_opacity();

  if (ctx.key_pressed(Key::Escape)) cancel();
  ctx.set_layer(saved);
  return !closed_;
}

}  // namespace cvd::app
