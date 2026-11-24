// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// The searchable target-format picker popover.
#include <algorithm>
#include <cmath>

#include "app/app.hpp"
#include "base/text_util.hpp"

namespace cvd::app {

using namespace ui;

void App::build_picker() {
  const Theme& t = theme_;
  QueueItem* item = state_->find(picker_.item_id);
  if (!item) {
    close_picker();
    return;
  }
  const Layer saved = ui_.layer();
  ui_.set_layer(Layer::Overlay);
  const Vec2 size = ui_.size();
  const float w = std::min(380.0f, size.x - 24.0f);
  const float max_h = std::min(460.0f, size.y - 32.0f);
  const float age = static_cast<float>(now_ - picker_.opened_at);
  const float appear = ui_.reduced_motion ? 1.0f : t.motion.decelerate(clampf(age / t.motion.base, 0.0f, 1.0f));
  if (appear < 1.0f) ui_.request_frame();

  // Rows: section headers + entries.
  const auto groups = group_targets(item->targets, item->category, picker_query);
  struct Row {
    bool header;
    std::string category;
    const TargetInfo* target;
  };
  std::vector<Row> rows;
  const bool show_compress_hint = trim(picker_query).empty();
  for (const auto& g : groups) {
    rows.push_back({true, g.category, nullptr});
    for (const TargetInfo* tg : g.targets) rows.push_back({false, g.category, tg});
  }
  const float header_h = 30.0f;
  const float row_h = 40.0f;
  float content = 8.0f;
  for (const auto& r : rows) content += r.header ? header_h : row_h;
  if (rows.empty()) content += 80.0f;
  const float search_h = 64.0f;
  const float h = std::min(max_h, search_h + content + 8.0f);

  // Place below the anchor, or above when there is no room.
  float x = clampf(picker_.anchor.right() - w, 12.0f, size.x - w - 12.0f);
  float y = picker_.anchor.bottom() + 8.0f;
  bool above = false;
  if (y + h > size.y - 12.0f) {
    if (picker_.anchor.y - h - 8.0f >= 12.0f) {
      y = picker_.anchor.y - h - 8.0f;
      above = true;
    } else {
      y = std::max(12.0f, size.y - h - 12.0f);
    }
  }
  Rect panel_r{x, y + (above ? 1.0f : -1.0f) * (1.0f - appear) * 10.0f, w, h};
  ui_.push_opacity(appear);
  panel(ui_, ui_.id("picker-panel"), panel_r, t.radius_lg, 3);
  ui_.describe(ui_.id("picker-panel"), Role::Group, "Choose a target format", panel_r);

  // Search.
  Rect body = panel_r.inset(10.0f);
  Rect search = cut_top(body, search_h - 14.0f).inset(0.0f, 2.0f);
  const WidgetId sid = ui_.id("picker-search");
  if (picker_.focus_search) {
    ui_.set_focus(sid);
    picker_.focus_search = false;
  }
  TextFieldOptions so;
  so.placeholder = "Search formats";
  so.leading = Icon::Search;
  so.clear_button = true;
  if (text_field(ui_, sid, search, picker_query, "Search formats", so)) picker_.highlight = 0;
  cut_top(body, 4.0f);

  // Keyboard navigation over selectable rows.
  std::vector<int> selectable;
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (!rows[static_cast<std::size_t>(i)].header) selectable.push_back(i);
  }
  const int n = static_cast<int>(selectable.size());
  if (n > 0) {
    // Start on the current target.
    if (age < 0.001f || picker_.highlight < 0) {
      for (int k = 0; k < n; ++k) {
        const auto* tg = rows[static_cast<std::size_t>(selectable[static_cast<std::size_t>(k)])].target;
        if (tg->id == item->target && picker_.group_source.empty()) picker_.highlight = k;
      }
    }
    picker_.highlight = std::clamp(picker_.highlight, 0, n - 1);
  }
  bool scroll_to_highlight = false;
  if (ui_.key_pressed(Key::Down)) {
    picker_.highlight = std::min(n - 1, picker_.highlight + 1);
    scroll_to_highlight = true;
  }
  if (ui_.key_pressed(Key::Up)) {
    picker_.highlight = std::max(0, picker_.highlight - 1);
    scroll_to_highlight = true;
  }
  if (ui_.key_pressed(Key::PageDown)) {
    picker_.highlight = std::min(n - 1, picker_.highlight + 8);
    scroll_to_highlight = true;
  }
  if (ui_.key_pressed(Key::PageUp)) {
    picker_.highlight = std::max(0, picker_.highlight - 8);
    scroll_to_highlight = true;
  }
  std::string chosen;
  if (ui_.key_pressed(Key::Enter) && n > 0) {
    chosen = rows[static_cast<std::size_t>(selectable[static_cast<std::size_t>(picker_.highlight)])].target->id;
  }

  // List.
  const WidgetId list_id = ui_.id("picker-list");
  if (scroll_to_highlight && n > 0) {
    float yy = 8.0f;
    const int target_row = selectable[static_cast<std::size_t>(picker_.highlight)];
    for (int i = 0; i < target_row; ++i) yy += rows[static_cast<std::size_t>(i)].header ? header_h : row_h;
    ui_.scroll_to(list_id, std::max(0.0f, yy - body.h * 0.4f));
  }
  if (age < 0.001f && n > 0) {
    float yy = 8.0f;
    const int target_row = selectable[static_cast<std::size_t>(picker_.highlight)];
    for (int i = 0; i < target_row; ++i) yy += rows[static_cast<std::size_t>(i)].header ? header_h : row_h;
    ui_.scroll_to(list_id, std::max(0.0f, yy - body.h * 0.4f));
  }
  ui_.begin_scroll(list_id, body, content);
  float yy = body.y + 4.0f;
  int sel_index = 0;
  for (const auto& r : rows) {
    if (r.header) {
      const Rect hr{body.x + 8.0f, yy, body.w - 16.0f, header_h};
      const Color c = t.category(r.category);
      ui_.fill({hr.x + 2.0f, hr.center().y - 3.0f + 2.0f, 6.0f, 6.0f}, 3.0f, c);
      std::string label = category_label(r.category);
      if (r.category == item->category && show_compress_hint) label += "  \xC2\xB7  same kind";
      ui_.text_in(label, t.type.overline, {hr.x + 16.0f, hr.y + 4.0f, hr.w - 16.0f, hr.h}, t.color.text_secondary);
      yy += header_h;
      continue;
    }
    const TargetInfo& tg = *r.target;
    const Rect rr{body.x, yy, body.w, row_h};
    const WidgetId rid = ui_.id("picker-row", hash_id(tg.id));
    const Interaction in = ui_.interact(rid, rr, kNone);
    const bool current = tg.id == item->target && picker_.group_source.empty();
    ui_.describe(rid, Role::MenuItem, tg.name, rr, tg.id, current);
    if (in.hovered) {
      ui_.set_cursor(Cursor::Hand);
      // Follow the pointer only when it moves, so keyboard navigation is not overridden.
      if (ui_.mouse() != picker_.last_mouse) picker_.highlight = sel_index;
    }
    if (in.clicked) chosen = tg.id;
    const bool hl = sel_index == picker_.highlight;
    if (hl) ui_.fill(rr.inset(2.0f, 1.0f), t.radius_sm, t.color.accent_subtle);
    const Color tint = t.category(tg.category);
    const std::string label = ascii_upper(tg.id);
    const float cw = std::min(chip_width(ui_, label), 124.0f);
    chip(ui_, {rr.x + 10.0f, rr.center().y - 12.0f, cw, 24.0f}, text::ellipsize_end(label, t.type.format_label, cw - 26.0f),
         tint, true);
    Rect name_r{rr.x + 10.0f + 130.0f, rr.y, rr.w - 150.0f - 30.0f, rr.h};
    std::string name = tg.name;
    if (tg.compress) name = "Compress \xC2\xB7 smaller " + ascii_upper(tg.id);
    ui_.text_in(text::ellipsize_end(name, t.type.body, name_r.w - (tg.lossy ? 44.0f : 0.0f)), t.type.body, name_r,
                hl ? t.color.text_primary : t.color.text_primary.fade(0.92f));
    if (tg.lossy) {
      const Rect lr{rr.right() - 84.0f, rr.center().y - 10.0f, 44.0f, 20.0f};
      ui_.fill(lr, 10.0f, t.color.warning_subtle);
      ui_.text_in("lossy", t.type.caption, lr, t.color.warning, 0.5f);
    }
    if (current) icon(ui_, Icon::Check, {rr.right() - 32.0f, rr.center().y - 9.0f, 18.0f, 18.0f}, t.color.accent_text);
    yy += row_h;
    ++sel_index;
  }
  if (rows.empty()) {
    const Rect er{body.x, body.y + 16.0f, body.w, 48.0f};
    ui_.text_in("No format matches \xE2\x80\x9C" + std::string(trim(picker_query)) + "\xE2\x80\x9D", t.type.body,
                er, t.color.text_secondary, 0.5f);
  }
  ui_.end_scroll();
  ui_.pop_opacity();
  picker_.last_mouse = ui_.mouse();

  if (!chosen.empty()) {
    select_target(chosen);
  } else if (ui_.clicked_outside(panel_r) && age > 0.05f) {
    // A click on the anchor itself is handled by its button (toggle).
    if (!picker_.anchor.contains(ui_.mouse_press_pos())) close_picker();
  }
  ui_.set_layer(saved);
}

}  // namespace cvd::app
