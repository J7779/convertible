// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Home screen: top bar, empty state, the queue of file cards and the action bar.
#include <algorithm>
#include <cmath>

#include "app/app.hpp"
#include "app/logo.hpp"
#include "base/text_util.hpp"

namespace cvd::app {

using namespace ui;

namespace {

constexpr float kColumnMax = 880.0f;
constexpr float kRowA = 76.0f;
constexpr float kCellH = 62.0f;

Rect column(const Rect& r, float max_w = kColumnMax, float pad = 24.0f) {
  const float w = std::min(max_w, r.w - pad * 2.0f);
  return {r.x + (r.w - w) * 0.5f, r.y, w, r.h};
}

std::string target_label(const TargetInfo* t) {
  if (!t) return "Choose";
  return ascii_upper(t->id);
}

void dashed_rrect(gfx::Canvas& c, const Rect& r, float radius, float width, float dash, float gap, Color color) {
  gfx::Path p;
  p.add_rrect(r, radius);
  const auto contours = p.flatten({}, 0.1f);
  if (contours.empty()) return;
  std::vector<Vec2> pts = contours.front().points;
  pts.push_back(pts.front());
  const gfx::PolylineMeasure m(pts);
  const float len = m.length();
  const int n = std::max(1, static_cast<int>(std::round(len / (dash + gap))));
  const float period = len / static_cast<float>(n);
  const float d = period * dash / (dash + gap);
  gfx::StrokeStyle st;
  st.width = width;
  st.cap = gfx::LineCap::Round;
  st.join = gfx::LineJoin::Round;
  for (int i = 0; i < n; ++i) {
    const float s = static_cast<float>(i) * period;
    c.stroke_polyline(m.trim(s / len, (s + d) / len), false, st, color);
  }
}

int option_columns(float width) { return width >= 700.0f ? 3 : (width >= 460.0f ? 2 : 1); }

std::vector<const OptionSpec*> visible_options(const QueueItem& item, bool advanced) {
  std::vector<const OptionSpec*> out;
  const bool preset = has_preset(item.options);
  for (const auto& o : item.options) {
    if (preset && o.key == "preset") continue;
    if (o.advanced == advanced) out.push_back(&o);
  }
  return out;
}

std::string choice_label(const std::string& v) {
  if (v.empty()) return v;
  std::string s = v;
  // "utf-8" -> "UTF-8", "lf" -> "LF"; words keep a capital first letter.
  const bool short_code = s.size() <= 4 || s.find('-') != std::string::npos ||
                          std::any_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
  if (short_code) return ascii_upper(s);
  s[0] = static_cast<char>(ascii_upper(s.substr(0, 1))[0]);
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------

void App::build_top_bar(Rect r) {
  const Theme& t = theme_;
  Rect bar = r.inset(24.0f, 0.0f);
  const Rect logo_r{bar.x, bar.center().y - 15.0f, 30.0f, 30.0f};
  const bool light = !t.dark;
  ui_.draw([logo_r, light](gfx::Canvas& c) { Logo::instance().draw_static(c, logo_r, light); });
  ui_.describe(ui_.id("brand"), Role::Image, "Convertible", logo_r);
  ui_.text_in("Convertible", t.type.title3, {bar.x + 40.0f, bar.y, 200.0f, bar.h}, t.color.text_primary);

  struct Tab {
    const char* label;
    Screen screen;
    Icon icon;
  };
  const Tab tabs[] = {{"Convert", Screen::Home, Icon::Refresh},
                      {"Formats", Screen::Formats, Icon::Grid},
                      {"Settings", Screen::Settings, Icon::Settings}};
  const bool compact = r.w < 640.0f;
  float x = bar.right();
  for (int i = 2; i >= 0; --i) {
    ButtonOptions bo;
    bo.kind = ButtonKind::Ghost;
    bo.style = &t.type.label;
    bo.radius = 18.0f;
    bo.selected = screen_ == tabs[i].screen || (tabs[i].screen == Screen::Settings && screen_ == Screen::Licenses);
    bo.icon = compact ? tabs[i].icon : Icon::None;
    if (compact) bo.tooltip = tabs[i].label;
    const std::string label = compact ? std::string() : tabs[i].label;
    const float w = compact ? 40.0f : button_width(ui_, label, bo) - 4.0f;
    x -= w;
    const Rect br{x, bar.center().y - 18.0f, w, 36.0f};
    if (button(ui_, ui_.id("tab", static_cast<std::uint64_t>(i)), br, label, bo)) navigate(tabs[i].screen);
    if (compact) ui_.describe(ui_.id("tab", static_cast<std::uint64_t>(i)), Role::Tab, tabs[i].label, br, {}, bo.selected);
    x -= 4.0f;
  }
  ui_.fill({r.x, r.bottom() - 1.0f, r.w, 1.0f}, 0.0f, t.color.border.fade(t.dark ? 0.7f : 1.0f));
}

// ---------------------------------------------------------------------------
// Home
// ---------------------------------------------------------------------------

void App::build_home(Rect r) {
  if (state_->empty()) {
    build_empty_state(r);
  } else {
    build_queue(r);
  }
}

void App::build_empty_state(Rect r) {
  const Theme& t = theme_;
  Rect col = column(r, 760.0f);
  const float avail = r.h - 48.0f;
  const float zone_h = std::clamp(avail - 110.0f, 260.0f, 420.0f);
  const float total = zone_h + 96.0f;
  const float top = r.y + std::max(24.0f, (avail - total) * 0.42f);
  const Rect zone{col.x, top, col.w, zone_h};

  const WidgetId zid = ui_.id("dropzone");
  const Interaction in = ui_.interact(zid, zone, kFocusable | kKeyboardActivate);
  ui_.describe(zid, Role::Button, "Choose files to convert", zone);
  if (in.hovered) ui_.set_cursor(Cursor::Hand);
  const float hover = ui_.animate(ui_.id("dz-hover"), in.hovered ? 1.0f : 0.0f, t.motion.base, t.motion.standard);
  const float age = ui_.age(zid);
  const float enter = t.motion.decelerate(clampf(age / t.motion.slower, 0.0f, 1.0f));
  if (enter < 1.0f) ui_.request_frame();

  ui_.fill(zone, t.radius_xxl, Color::lerp(t.color.surface.fade(t.dark ? 0.55f : 0.8f), t.color.accent_subtle.fade(0.7f), hover));
  const Color dash = Color::lerp(t.color.border_strong.fade(0.7f), t.color.accent, hover);
  ui_.draw([zone, radius = t.radius_xxl, dash](gfx::Canvas& c) { dashed_rrect(c, zone.inset(1.0f), radius, 1.5f, 7.0f, 7.0f, dash); });
  ui_.focus_ring(zid, zone, t.radius_xxl);

  // Mark, headline, button and hint, centred in the zone. Small windows (and
  // larger text) get a smaller mark and drop the hint rather than letting the
  // button cross the zone's edge.
  const text::Style& headline = zone.w < 520.0f ? t.type.title2 : t.type.title1;
  const float mark = zone.h >= 330.0f ? 84.0f : 56.0f;
  const float hint_h = 26.0f;
  const float body_h = mark + 22.0f + headline.line_height + 46.0f + 48.0f;
  const bool show_hint = body_h + hint_h + 32.0f <= zone.h;
  const float content_h = body_h + (show_hint ? hint_h : 0.0f);
  const float lift = -4.0f * hover;
  const float mark_y = zone.y + std::max(16.0f, (zone.h - content_h) * 0.45f);
  const Rect logo_r = centered({zone.x, mark_y + lift, zone.w, mark}, mark, mark);
  const bool light = !t.dark;
  ui_.draw([logo_r, light, enter](gfx::Canvas& c) {
    c.save();
    c.set_opacity(enter);
    Logo::instance().draw_static(c, logo_r, light);
    c.restore();
  });
  const float base_y = logo_r.bottom() + 22.0f;
  ui_.text_in("Drop files to convert", headline, {zone.x, base_y, zone.w, headline.line_height}, t.color.text_primary, 0.5f);
  ui_.text_in(text::ellipsize_end("Images, documents, audio, archives and more \xE2\x80\x94 as many as you like.", t.type.body,
                                  zone.w - 32.0f),
              t.type.body, {zone.x + 16.0f, base_y + headline.line_height + 6.0f, zone.w - 32.0f, 22.0f},
              t.color.text_secondary, 0.5f);
  ButtonOptions bo;
  bo.kind = ButtonKind::Primary;
  bo.icon = Icon::Plus;
  const float bw = button_width(ui_, "Choose files", bo) + 8.0f;
  const Rect btn{zone.center().x - bw * 0.5f, base_y + headline.line_height + 46.0f, bw, 48.0f};
  const bool clicked_button = button(ui_, ui_.id("choose"), btn, "Choose files", bo);
#if defined(__APPLE__)
  const char* hint = "or press \xE2\x8C\x98O";
#else
  const char* hint = "or press Ctrl+O";
#endif
  if (show_hint) ui_.text_in(hint, t.type.caption, {zone.x, btn.bottom() + 8.0f, zone.w, 18.0f}, t.color.text_secondary, 0.5f);
  if (clicked_button || in.clicked) choose_files();

  // Privacy statement.
  Rect privacy{col.x, zone.bottom() + 24.0f, col.w, 24.0f};
  const char* statement = "Your files never leave this device. Every conversion runs offline, right here.";
  const float sw = text::measure(statement, t.type.body) + 30.0f;
  const Rect pr = centered(privacy, std::min(sw, col.w), 24.0f);
  icon(ui_, Icon::Shield, {pr.x, pr.y + 2.0f, 20.0f, 20.0f}, t.color.success);
  ui_.text_in(text::ellipsize_end(statement, t.type.body, pr.w - 30.0f), t.type.body,
              {pr.x + 30.0f, pr.y, pr.w - 30.0f, pr.h}, t.color.text_secondary);
  ui_.describe(ui_.id("privacy"), Role::Label, statement, pr);

  // Catalogue size, linking to the formats browser.
  const Catalog& cat = Catalog::instance();
  const std::string count = std::to_string(cat.formats().size()) + " formats recognised \xC2\xB7 " +
                            std::to_string(cat.count_writable()) + " can be created";
  ButtonOptions link;
  link.kind = ButtonKind::Link;
  link.style = &t.type.label;
  const float lw = button_width(ui_, count, link);
  if (button(ui_, ui_.id("formats-link"), centered({col.x, privacy.bottom() + 10.0f, col.w, 24.0f}, lw, 24.0f), count, link)) {
    navigate(Screen::Formats);
  }
}

// ---------------------------------------------------------------------------
// Queue
// ---------------------------------------------------------------------------

float App::card_height(const QueueItem& item, float width) const {
  float h = kRowA;
  switch (item.status) {
    case ItemStatus::Ready: {
      const bool preset = has_preset(item.options);
      const auto basic = visible_options(item, false);
      const auto adv = visible_options(item, true);
      if (!preset && basic.empty() && adv.empty()) break;
      const int cols = option_columns(width - 32.0f);
      h += 44.0f;  // preset / options header row
      h += std::ceil(static_cast<float>(basic.size()) / static_cast<float>(cols)) * kCellH;
      if (item.advanced_open) h += std::ceil(static_cast<float>(adv.size()) / static_cast<float>(cols)) * kCellH;
      h += 10.0f;
      break;
    }
    case ItemStatus::Queued:
    case ItemStatus::Converting: h += 40.0f; break;
    case ItemStatus::Done: h += 52.0f + (item.warnings.empty() ? 0.0f : 24.0f); break;
    case ItemStatus::Failed: h += 74.0f; break;
    case ItemStatus::Cancelled: h += 48.0f; break;
  }
  return h;
}

void App::build_queue(Rect r) {
  const Theme& t = theme_;
  Rect col = column(r);
  // Header.
  Rect header = cut_top(col, 76.0f);
  header = header.inset(0.0f, 14.0f);
  const int n = static_cast<int>(state_->items().size());
  const std::string title = std::to_string(n) + (n == 1 ? " file" : " files");
  ui_.text_in(title, t.type.title2, {header.x, header.y, 300.0f, 30.0f}, t.color.text_primary);
  std::uint64_t total = 0;
  for (const auto& item : state_->items()) total += item.size;
  ui_.text_in(format_size(total) + " \xC2\xB7 nothing leaves this device", t.type.caption,
              {header.x, header.y + 30.0f, 400.0f, 18.0f}, t.color.text_secondary);
  Rect actions = header;
  {
    ButtonOptions bo;
    bo.kind = ButtonKind::Ghost;
    bo.style = &t.type.label;
    const bool busy = state_->any_busy();
    bo.enabled = !busy;
    const float w = button_width(ui_, "Clear", bo);
    if (button(ui_, ui_.id("clear-all"), cut_right(actions, w).inset(0.0f, 4.0f), "Clear", bo)) {
      state_->clear();
      completed_seen_.clear();
      close_picker();
    }
    cut_right(actions, 8.0f);
    ButtonOptions add;
    add.kind = ButtonKind::Secondary;
    add.icon = Icon::Plus;
    add.style = &t.type.label;
    const float aw = button_width(ui_, "Add files", add);
    if (button(ui_, ui_.id("add-files"), cut_right(actions, aw).inset(0.0f, 4.0f), "Add files", add)) choose_files();
  }

  // One control for every file that shares a source format.
  for (const std::string& source : state_->shared_sources()) {
    build_group_bar(cut_top(col, 56.0f), source);
    cut_top(col, 10.0f);
  }

  // Cards.
  const float bar_h = 84.0f;
  Rect list{col.x - 8.0f, col.y, col.w + 16.0f, r.bottom() - col.y - bar_h + 8.0f};
  float content = 8.0f;
  for (const auto& item : state_->items()) content += card_height(item, col.w) + 12.0f;
  content += 16.0f;
  const WidgetId sid = ui_.id("queue-scroll");
  ui_.begin_scroll(sid, list, content);
  float y = list.y + 8.0f;
  int index = 0;
  // Iterate by id: building a card may change the queue (remove).
  std::vector<std::uint64_t> ids;
  for (const auto& item : state_->items()) ids.push_back(item.id);
  for (const std::uint64_t id : ids) {
    QueueItem* item = state_->find(id);
    if (!item) continue;
    const float h = card_height(*item, col.w);
    const Rect card{col.x, y, col.w, h};
    build_card(*item, card, index++, false);
    y += h + 12.0f;
  }
  ui_.end_scroll();
  build_action_bar({r.x, r.bottom() - bar_h, r.w, bar_h});
}

void App::build_group_bar(Rect r, const std::string& source) {
  const Theme& t = theme_;
  int count = 0;
  const QueueItem* first = nullptr;
  for (const auto& item : state_->items()) {
    if (item.source == source && item.status == ItemStatus::Ready) {
      ++count;
      if (!first) first = &item;
    }
  }
  if (!first) return;
  ui_.push_id(source);
  ui_.fill(r, t.radius_lg, t.color.accent_subtle.fade(t.dark ? 0.6f : 0.8f));
  ui_.stroke(r, t.radius_lg, 1.0f, t.color.accent.fade(0.25f));
  Rect inner = r.inset(16.0f, 10.0f);
  const Color tint = t.category(first->category);
  const float cw = chip_width(ui_, first->source_label);
  chip(ui_, cut_left(inner, cw).inset(0.0f, 4.0f), first->source_label, tint, false);
  cut_left(inner, 10.0f);
  // Do all items share one target?
  std::string common = first->target;
  for (const auto& item : state_->items()) {
    if (item.source == source && item.status == ItemStatus::Ready && item.target != common) common.clear();
  }
  const std::string label = "All " + std::to_string(count) + " " + first->source_label + " files to";
  ui_.text_in(label, t.type.body_strong, cut_left(inner, text::measure(label, t.type.body_strong) + 12.0f), t.color.text_primary);
  const TargetInfo* ti = nullptr;
  for (const auto& tg : first->targets) {
    if (tg.id == common) ti = &tg;
  }
  ButtonOptions bo;
  bo.kind = ButtonKind::Secondary;
  bo.trailing = Icon::ChevronDown;
  bo.style = &t.type.label;
  bo.tooltip = "Set the target format for every file of this type";
  const std::string tl = common.empty() ? "Mixed" : target_label(ti);
  bo.access_label = label;
  bo.access_value = tl;
  const float bw = std::min(220.0f, button_width(ui_, tl, bo));
  const Rect br = cut_left(inner, bw);
  if (button(ui_, ui_.id("group-target"), br, tl, bo)) {
    open_picker(first->id, source);
    picker_.anchor = br.offset(ui_.current_translate().x, ui_.current_translate().y);
  }
  ui_.pop_id();
}

float App::build_card(QueueItem& item, Rect r, int index, bool measure_only) {
  (void)measure_only;
  const Theme& t = theme_;
  ui_.push_id(item.id);
  const WidgetId cid = ui_.id("card");
  // Entrance: fade and rise, staggered.
  const float age = ui_.age(cid) - static_cast<float>(std::min(index, 8)) * 0.04f;
  const float enter = ui_.reduced_motion ? 1.0f : t.motion.decelerate(clampf(age / t.motion.slow, 0.0f, 1.0f));
  if (enter < 1.0f) ui_.request_frame();
  ui_.push_opacity(enter);
  ui_.push_translate({0.0f, (1.0f - enter) * 14.0f});

  const Color tint = t.category(item.category);
  const bool hovered = ui_.hovering(r);
  const float hover = ui_.animate(ui_.id("card-hover"), hovered ? 1.0f : 0.0f, t.motion.fast, t.motion.standard);
  ui_.shadow(r, t.radius_lg, hover > 0.5f ? 2 : 1);
  ui_.fill(r, t.radius_lg, t.dark ? Color::lerp(t.color.surface, t.color.surface_elevated, 0.35f + 0.25f * hover) : t.color.surface);
  ui_.stroke(r, t.radius_lg, 1.0f, Color::lerp(t.color.border, t.color.border_strong, 0.25f * hover).fade(t.dark ? 0.8f : 1.0f));
  ui_.describe(cid, Role::Group, item.name, r, status_name(item.status));

  Rect row = Rect{r.x, r.y, r.w, kRowA}.inset(16.0f, 0.0f);
  // File tile: category tint with the format label.
  const Rect tile{row.x, row.center().y - 22.0f, 44.0f, 44.0f};
  ui_.fill(tile, t.radius_md, tint.fade(t.dark ? 0.16f : 0.12f));
  icon(ui_, Icon::File, tile.inset(11.0f), tint, 1.6f);
  cut_left(row, 58.0f);

  // Right-side controls depend on the state.
  Rect controls = row;
  const float right_w = std::min(row.w * 0.55f, 320.0f);
  controls = cut_right(row, right_w);
  Rect c = controls;
  const bool busy = item.busy();
  // Remove / cancel.
  {
    const Rect xr = cut_right(c, 36.0f);
    const Rect xb = centered(xr, 36.0f, 36.0f);
    if (busy) {
      if (icon_button(ui_, ui_.id("cancel"), xb, Icon::Close, "Cancel this conversion")) state_->cancel(item.id);
    } else if (icon_button(ui_, ui_.id("remove"), xb, Icon::Trash, "Remove from the list")) {
      const std::uint64_t id = item.id;
      ui_.pop_translate();
      ui_.pop_opacity();
      ui_.pop_id();
      if (picker_.open && picker_.item_id == id) close_picker();
      state_->remove(id);
      return r.h;
    }
    cut_right(c, 6.0f);
  }
  const TargetInfo* target = item.target_info();
  if (item.status == ItemStatus::Ready) {
    // Compress toggle when the core offers a same-format target.
    const TargetInfo* compress = nullptr;
    for (const auto& tg : item.targets) {
      if (tg.compress) compress = &tg;
    }
    if (compress) {
      const Rect cr = centered(cut_right(c, 40.0f), 36.0f, 36.0f);
      const bool on = item.target == compress->id;
      if (on) ui_.fill(cr, 18.0f, t.color.accent_subtle);
      if (icon_button(ui_, ui_.id("compress"), cr, Icon::Compress,
                      on ? "Compressing to a smaller " + ascii_upper(compress->id) : "Compress (same format, smaller)",
                      true, on ? t.color.accent_text : Color{0, 0, 0, 0})) {
        state_->set_target(item.id, on ? default_target(item.targets) : compress->id);
      }
      cut_right(c, 4.0f);
    }
    // Target picker.
    ButtonOptions bo;
    bo.kind = ButtonKind::Secondary;
    bo.trailing = Icon::ChevronDown;
    bo.style = &t.type.label;
    const std::string tip = target ? target->name : std::string("Choose a format");
    bo.tooltip = tip;
    const std::string tl = target_label(target);
    const std::string access = "Target format for " + item.name;
    bo.access_label = access;
    bo.access_value = tl;
    const float bw = std::min(c.w - 30.0f, std::max(110.0f, button_width(ui_, tl, bo) + 10.0f));
    const Rect br = centered(cut_right(c, bw), bw, 40.0f);
    if (target) {
      // Square dot in the target's category colour, echoing the mark.
      ui_.fill({br.x - 0.0f, br.y, 0.0f, 0.0f}, 0.0f, Color{0, 0, 0, 0});
    }
    const bool open = picker_.open && picker_.item_id == item.id && picker_.group_source.empty();
    bo.selected = open;
    if (button(ui_, ui_.id("target"), br, tl, bo)) {
      if (open) {
        close_picker();
      } else {
        open_picker(item.id);
        picker_.anchor = br.offset(ui_.current_translate().x, ui_.current_translate().y);
      }
    }
    if (target) {
      const Color tc = t.category(target->category);
      ui_.fill({br.x + 12.0f, br.center().y - 3.0f, 6.0f, 6.0f}, 1.0f, tc);
    }
    icon(ui_, Icon::ArrowRight, centered(cut_right(c, 28.0f), 18.0f, 18.0f), t.color.text_secondary);
  } else {
    // Static target chip plus progress / result mark.
    const Rect mark = centered(cut_right(c, 44.0f), 34.0f, 34.0f);
    const bool light = !t.dark;
    if (item.status == ItemStatus::Converting || item.status == ItemStatus::Queued) {
      const float shown = ui_.animate(ui_.id("progress"), item.progress, t.motion.base, t.motion.standard);
      const bool indeterminate = item.status == ItemStatus::Queued || item.progress <= 0.001f;
      const float spin_t = static_cast<float>(std::fmod(now_ * 1000.0, 1400.0 * 1000.0));
      const bool reduced = ui_.reduced_motion;
      ui_.draw([mark, shown, indeterminate, spin_t, light, reduced](gfx::Canvas& cv) {
        if (indeterminate) Logo::instance().draw_spinner(cv, mark, spin_t, light, reduced);
        else Logo::instance().draw_progress(cv, mark, shown, light);
      });
      ui_.describe(ui_.id("progress-node"), Role::Progress, "Converting " + item.name, mark, format_percent(item.progress));
      ui_.request_frame();
    } else if (item.status == ItemStatus::Done) {
      float since = -1.0f;
      for (const auto& [id, when] : completion_times_) {
        if (id == item.id) since = static_cast<float>((now_ - when) * 1000.0);
      }
      ui_.draw([mark, light, since](gfx::Canvas& cv) {
        Logo::instance().draw_progress(cv, mark, 1.0f, light);
        if (since >= 0.0f) Logo::instance().draw_completion_ripple(cv, mark, since, light);
      });
    } else {
      icon(ui_, item.status == ItemStatus::Failed ? Icon::Warning : Icon::Info, centered(mark, 24.0f, 24.0f),
           item.status == ItemStatus::Failed ? t.color.error : t.color.text_secondary);
    }
    cut_right(c, 6.0f);
    const std::string tl = target_label(target);
    const float cw = std::min(chip_width(ui_, tl), c.w - 30.0f);
    if (cw > 30.0f) {
      chip(ui_, centered(cut_right(c, cw), cw, 26.0f), tl, target ? t.category(target->category) : tint, true);
      icon(ui_, Icon::ArrowRight, centered(cut_right(c, 28.0f), 18.0f, 18.0f), t.color.text_secondary);
    }
  }

  // Name and meta.
  {
    const Rect name_r{row.x, row.center().y - 21.0f, row.w - 8.0f, 22.0f};
    ui_.text_in(text::ellipsize_middle(item.name, t.type.body_strong, name_r.w), t.type.body_strong, name_r,
                t.color.text_primary);
    std::string meta = format_size(item.size);
    const FormatInfo* f = Catalog::instance().find(item.source);
    meta += " \xC2\xB7 ";
    meta += f ? f->name : "Unrecognised file";
    const Rect meta_r{row.x, row.center().y + 1.0f, row.w - 8.0f, 20.0f};
    const float cw = chip_width(ui_, item.source_label);
    chip(ui_, {meta_r.x, meta_r.y + 1.0f, cw, 20.0f}, item.source_label, tint, false);
    ui_.text_in(text::ellipsize_end(meta, t.type.caption, meta_r.w - cw - 8.0f), t.type.caption,
                {meta_r.x + cw + 8.0f, meta_r.y, meta_r.w - cw - 8.0f, meta_r.h}, t.color.text_secondary);
  }

  // Row B.
  Rect b{r.x + 16.0f, r.y + kRowA, r.w - 32.0f, r.h - kRowA - 4.0f};
  switch (item.status) {
    case ItemStatus::Ready: build_card_options(item, b); break;
    case ItemStatus::Queued:
    case ItemStatus::Converting: {
      Rect line = cut_top(b, 32.0f);
      const float shown = ui_.animate(ui_.id("bar"), item.progress, t.motion.base, t.motion.standard);
      const std::string stage = item.status == ItemStatus::Queued ? "Waiting for a free worker\xE2\x80\xA6"
                                                                  : (item.stage.empty() ? "Converting" : item.stage);
      ui_.text_in(stage, t.type.caption, cut_left(line, line.w * 0.5f), t.color.text_secondary);
      text::Style pct = t.type.label;
      pct.tabular = true;
      ui_.text_in(format_percent(item.progress), pct, line, t.color.text_primary, 1.0f);
      const Rect track{r.x + 16.0f, r.bottom() - 14.0f, r.w - 32.0f, 4.0f};
      ui_.fill(track, 2.0f, t.color.border.fade(0.8f));
      ui_.fill_paint({track.x, track.y, track.w * clampf(shown, 0.0f, 1.0f), track.h}, 2.0f,
                     gfx::Paint::linear({track.x, 0.0f}, {track.right(), 0.0f},
                                        {{0.0f, t.color.gradient_start}, {0.5f, t.color.gradient_mid}, {1.0f, t.color.gradient_end}}));
      break;
    }
    case ItemStatus::Done: {
      Rect line = cut_top(b, 44.0f);
      // before -> after (saved %)
      text::Style num = t.type.body_strong;
      num.tabular = true;
      const std::string before = format_size(item.size);
      const std::string after = format_size(item.output_size);
      const std::string change = format_size_change(item.size, item.output_size);
      const bool smaller = item.output_size < item.size;
      float x = line.x;
      const auto put = [&](const std::string& s, const text::Style& st, Color col) {
        const float w = text::measure(s, st);
        ui_.text_in(s, st, {x, line.y, w + 2.0f, line.h}, col);
        x += w + 8.0f;
      };
      put(before, num, t.color.text_secondary);
      icon(ui_, Icon::ArrowRight, {x, line.center().y - 8.0f, 16.0f, 16.0f}, t.color.text_secondary);
      x += 24.0f;
      put(after, num, t.color.text_primary);
      const std::string badge = smaller ? change + " smaller" : change;
      const float bw = text::measure(badge, t.type.label) + 18.0f;
      const Color bc = smaller ? t.color.success : t.color.text_secondary;
      const Rect badge_r{x, line.center().y - 12.0f, bw, 24.0f};
      ui_.fill(badge_r, 12.0f, (smaller ? t.color.success_subtle : t.color.surface_sunken));
      ui_.text_in(badge, t.type.label, badge_r, bc, 0.5f);
      ui_.describe(ui_.id("result"), Role::Label, before + " to " + after + " (" + change + ")", line);
      // Actions.
      Rect acts = line;
      ButtonOptions save;
      save.kind = item.saved_path.empty() ? ButtonKind::Primary : ButtonKind::Secondary;
      save.icon = item.saved_path.empty() ? Icon::Save : Icon::Check;
      save.style = &t.type.label;
      const std::string save_tip = item.saved_path.empty() ? "Save " + item.output_name : "Saved to " + item.saved_path;
      save.tooltip = save_tip;
      const std::string save_label = item.saved_path.empty() ? "Save" : "Saved";
      const float sw = button_width(ui_, save_label, save);
      if (button(ui_, ui_.id("save"), centered(cut_right(acts, sw), sw, 36.0f), save_label, save)) save_item(item.id);
      cut_right(acts, 6.0f);
      if (!item.saved_path.empty()) {
        if (icon_button(ui_, ui_.id("reveal"), centered(cut_right(acts, 36.0f), 36.0f, 36.0f), Icon::Reveal,
                        "Show in " + platform_.file_manager_name())) {
          reveal_item(item.id);
        }
      }
      if (icon_button(ui_, ui_.id("open"), centered(cut_right(acts, 36.0f), 36.0f, 36.0f), Icon::File,
                      "Open with the default app")) {
        open_item(item.id);
      }
      if (icon_button(ui_, ui_.id("again"), centered(cut_right(acts, 36.0f), 36.0f, 36.0f), Icon::Refresh,
                      "Convert again with other settings")) {
        state_->convert_again(item.id);
      }
      if (!item.warnings.empty()) {
        Rect wl = cut_top(b, 22.0f);
        icon(ui_, Icon::Info, {wl.x, wl.y + 2.0f, 16.0f, 16.0f}, t.color.warning);
        std::string w = item.warnings.front();
        if (item.warnings.size() > 1) w += "  (+" + std::to_string(item.warnings.size() - 1) + " more)";
        ui_.text_in(text::ellipsize_end(w, t.type.caption, wl.w - 24.0f), t.type.caption,
                    {wl.x + 24.0f, wl.y, wl.w - 24.0f, wl.h}, t.color.text_secondary);
        std::string all;
        for (const auto& x2 : item.warnings) all += (all.empty() ? "" : " \xE2\x80\xA2 ") + x2;
        const WidgetId wid = ui_.id("warnings");
        ui_.interact(wid, wl, kNone);
        ui_.tooltip(wid, all);
        ui_.describe(wid, Role::Label, "Warnings: " + all, wl);
      }
      break;
    }
    case ItemStatus::Failed: {
      Rect top = cut_top(b, 26.0f);
      ui_.text_in(status_title(item.error_status), t.type.body_strong, top, t.color.error);
      std::string detail = item.error_message;
      const std::string hint = status_hint(item.error_status);
      if (!hint.empty()) detail = detail.empty() ? hint : detail + " " + hint;
      Rect line = cut_top(b, 40.0f);
      ButtonOptions again;
      again.kind = ButtonKind::Secondary;
      again.icon = Icon::Refresh;
      again.style = &t.type.label;
      const float aw = button_width(ui_, "Try again", again);
      if (button(ui_, ui_.id("retry"), centered(cut_right(line, aw), aw, 36.0f), "Try again", again)) {
        state_->convert_again(item.id);
      }
      paragraph(ui_, detail, t.type.caption, line.inset(0.0f, 2.0f).offset(0.0f, -2.0f).inset(0.0f, 0.0f), t.color.text_secondary, 2);
      ui_.describe(ui_.id("error"), Role::Label, status_title(item.error_status) + ". " + detail, top);
      break;
    }
    case ItemStatus::Cancelled: {
      Rect line = cut_top(b, 40.0f);
      ui_.text_in("Cancelled \xE2\x80\x94 nothing was written.", t.type.body, line, t.color.text_secondary);
      ButtonOptions again;
      again.kind = ButtonKind::Secondary;
      again.icon = Icon::Refresh;
      again.style = &t.type.label;
      const float aw = button_width(ui_, "Convert again", again);
      if (button(ui_, ui_.id("again"), centered(cut_right(line, aw), aw, 36.0f), "Convert again", again)) {
        state_->convert_again(item.id);
      }
      break;
    }
  }

  ui_.pop_translate();
  ui_.pop_opacity();
  ui_.pop_id();
  return r.h;
}

void App::build_card_options(QueueItem& item, Rect b) {
  const Theme& t = theme_;
  const bool preset = has_preset(item.options);
  const auto basic = visible_options(item, false);
  const auto adv = visible_options(item, true);
  if (!preset && basic.empty() && adv.empty()) return;
  ui_.fill({b.x, b.y, b.w, 1.0f}, 0.0f, t.color.border.fade(0.6f));
  Rect head = cut_top(b, 44.0f).inset(0.0f, 4.0f);
  if (preset) {
    ui_.text_in("Quality", t.type.label, cut_left(head, 64.0f), t.color.text_secondary);
    static const std::vector<std::string> labels = {"High", "Balanced", "Small"};
    int sel = preset_index(item.values);
    const float sw = std::min(300.0f, head.w * 0.6f);
    if (segmented(ui_, ui_.id("preset"), cut_left(head, sw), labels, sel, "Quality preset")) state_->set_preset(item.id, sel);
  } else {
    ui_.text_in("Options", t.type.label, cut_left(head, 80.0f), t.color.text_secondary);
  }
  if (!adv.empty()) {
    bool open = item.advanced_open;
    const float dw = 110.0f;
    if (disclosure(ui_, ui_.id("advanced"), cut_right(head, dw), "Advanced", open)) item.advanced_open = open;
  }
  ButtonOptions reset;
  reset.kind = ButtonKind::Link;
  reset.style = &t.type.caption;
  const bool is_default = item.values == default_option_values(item.options, state_->settings);
  if (!is_default) {
    cut_right(head, 12.0f);
    const float rw = button_width(ui_, "Reset", reset);
    if (button(ui_, ui_.id("reset"), cut_right(head, rw), "Reset", reset)) state_->reset_options(item.id);
  }

  const int cols = option_columns(b.w);
  const float cell_w = (b.w - static_cast<float>(cols - 1) * 16.0f) / static_cast<float>(cols);
  const auto grid = [&](const std::vector<const OptionSpec*>& specs) {
    for (std::size_t i = 0; i < specs.size(); ++i) {
      const OptionSpec& spec = *specs[i];
      const int cx = static_cast<int>(i) % cols;
      const int cy = static_cast<int>(i) / cols;
      Rect cell{b.x + static_cast<float>(cx) * (cell_w + 16.0f), b.y + static_cast<float>(cy) * kCellH, cell_w, kCellH - 6.0f};
      ui_.push_id(spec.key);
      Rect label_r = cut_top(cell, 20.0f);
      ui_.text_in(text::ellipsize_end(spec.label, t.type.label, label_r.w - 60.0f), t.type.label, label_r, t.color.text_secondary);
      const WidgetId info = ui_.id("info");
      if (!spec.description.empty()) {
        const float lw = std::min(text::measure(spec.label, t.type.label), label_r.w - 60.0f);
        const Rect ir{label_r.x + lw + 6.0f, label_r.y + 3.0f, 14.0f, 14.0f};
        ui_.interact(info, ir.inset(-4.0f), kNone);
        icon(ui_, Icon::Info, ir, t.color.text_secondary.fade(0.8f), 1.5f);
        ui_.tooltip(info, spec.description);
      }
      Rect ctl = cell.inset(0.0f, 1.0f);
      const Json& value = item.values[spec.key];
      switch (spec.type) {
        case OptionType::Int:
        case OptionType::Float: {
          if (numeric_control(spec) == NumericControl::Stepper) {
            build_stepper(item, spec, ctl);
            break;
          }
          double v = value.as_number();
          const double mn = spec.min.value_or(0.0);
          const double mx = spec.max.value_or(std::max(100.0, v * 2.0));
          const double step = spec.type == OptionType::Int ? 1.0 : (mx - mn) / 100.0;
          text::Style vs = t.type.label;
          vs.tabular = true;
          const std::string shown = spec.type == OptionType::Int ? std::to_string(static_cast<long long>(std::llround(v)))
                                                                  : std::to_string(std::round(v * 100.0) / 100.0).substr(0, 5);
          ui_.text_in(shown, vs, cut_right(ctl, 44.0f), t.color.text_primary, 1.0f);
          if (slider(ui_, ui_.id("slider"), ctl, v, mn, mx, step, spec.label)) state_->set_option(item.id, spec.key, Json(v));
          break;
        }
        case OptionType::Bool: {
          bool v = value.as_bool();
          const Rect sw{ctl.x - 2.0f, ctl.y, 48.0f, ctl.h};
          if (toggle(ui_, ui_.id("switch"), sw, v, spec.label)) state_->set_option(item.id, spec.key, Json(v));
          ui_.text_in(v ? "On" : "Off", t.type.caption, {sw.right() + 6.0f, ctl.y, 60.0f, ctl.h}, t.color.text_secondary);
          break;
        }
        case OptionType::Enum: {
          ButtonOptions bo;
          bo.kind = ButtonKind::Secondary;
          bo.trailing = Icon::ChevronDown;
          bo.style = &t.type.label;
          const std::string current = choice_label(value.str());
          bo.access_label = spec.label;
          bo.access_value = current;
          const WidgetId mid = ui_.id("menu");
          const Rect br{ctl.x, ctl.y, std::min(ctl.w, 220.0f), ctl.h};
          if (button(ui_, mid, br, current, bo)) {
            std::vector<std::string> labels;
            int sel = -1;
            for (std::size_t k = 0; k < spec.choices.size(); ++k) {
              labels.push_back(choice_label(spec.choices[k]));
              if (spec.choices[k] == value.str()) sel = static_cast<int>(k);
            }
            const std::uint64_t id = item.id;
            const std::string key = spec.key;
            const std::vector<std::string> choices = spec.choices;
            open_menu(mid, br.offset(ui_.current_translate().x, ui_.current_translate().y), labels, sel,
                      [this, id, key, choices](int picked) {
                        state_->set_option(id, key, Json(choices[static_cast<std::size_t>(picked)]));
                      });
          }
          break;
        }
        case OptionType::String: {
          std::string v = value.str();
          TextFieldOptions fo;
          fo.placeholder = spec.label;
          if (text_field(ui_, ui_.id("field"), ctl, v, spec.label, fo)) state_->set_option(item.id, spec.key, Json(v));
          break;
        }
      }
      ui_.pop_id();
    }
    b.y += std::ceil(static_cast<float>(specs.size()) / static_cast<float>(cols)) * kCellH;
  };
  grid(basic);
  if (item.advanced_open) grid(adv);
}

namespace {

std::string format_number(double v, bool integer) {
  if (integer) return std::to_string(static_cast<long long>(std::llround(v)));
  std::string s = std::to_string(std::round(v * 1000.0) / 1000.0);
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

std::optional<double> parse_number(const std::string& text) {
  // Locale-independent: digits, one optional sign and one '.' only.
  if (text.empty() || text.size() > 24) return std::nullopt;
  double whole = 0.0;
  double scale = 0.0;
  std::size_t i = 0;
  bool negative = false;
  if (text[0] == '-' || text[0] == '+') {
    negative = text[0] == '-';
    i = 1;
  }
  bool digits = false;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c >= '0' && c <= '9') {
      digits = true;
      if (scale > 0.0) {
        whole += (c - '0') / scale;
        scale *= 10.0;
      } else {
        whole = whole * 10.0 + (c - '0');
      }
    } else if (c == '.' && scale == 0.0) {
      scale = 10.0;
    } else {
      return std::nullopt;
    }
  }
  if (!digits) return std::nullopt;
  return negative ? -whole : whole;
}

}  // namespace

void App::build_stepper(QueueItem& item, const OptionSpec& spec, Rect ctl) {
  const bool integer = spec.type == OptionType::Int;
  const double v = item.values[spec.key].as_number();
  const double step = stepper_step(spec);
  const Rect minus = cut_left(ctl, 36.0f);
  const Rect plus = cut_right(ctl, 36.0f);
  ctl = ctl.inset(4.0f, 0.0f);
  const bool can_down = !spec.min || v > *spec.min;
  const bool can_up = !spec.max || v < *spec.max;
  const auto commit = [&](double next) {
    // Steps land on multiples of the step (3999 + 1000 -> 4000, not 4999).
    state_->set_option(item.id, spec.key, Json(next));
  };
  if (icon_button(ui_, ui_.id("dec"), centered(minus, 32.0f, 32.0f), Icon::ChevronLeft, "Decrease " + spec.label, can_down)) {
    commit(std::ceil(v / step - 1.0 - 1e-9) * step);
  }
  if (icon_button(ui_, ui_.id("inc"), centered(plus, 32.0f, 32.0f), Icon::ChevronRight, "Increase " + spec.label, can_up)) {
    commit(std::floor(v / step + 1.0 + 1e-9) * step);
  }
  const WidgetId fid = ui_.id("number");
  const bool editing = ui_.focused() == fid;
  auto draft = number_drafts_.find(fid);
  if (!editing && draft != number_drafts_.end()) {
    number_drafts_.erase(draft);
    draft = number_drafts_.end();
  }
  std::string text = draft != number_drafts_.end() ? draft->second : format_number(v, integer);
  TextFieldOptions fo;
  fo.placeholder = spec.min && *spec.min == 0.0 ? "0" : "";
  fo.monospace = true;
  fo.select_all_on_focus = true;
  if (text_field(ui_, fid, ctl, text, spec.label, fo)) {
    number_drafts_[fid] = text;
    if (const auto parsed = parse_number(text)) state_->set_option(item.id, spec.key, Json(*parsed));
  }
  if (ui_.focused() == fid) {
    const bool up = ui_.key_pressed(Key::Up);
    const bool down = !up && ui_.key_pressed(Key::Down);
    if (up || down) {
      commit(up ? std::floor(v / step + 1.0 + 1e-9) * step : std::ceil(v / step - 1.0 - 1e-9) * step);
      number_drafts_.erase(fid);
    }
  }
}

void App::build_action_bar(Rect r) {
  const Theme& t = theme_;
  ui_.fill(r, 0.0f, t.color.background.fade(0.94f));
  ui_.fill({r.x, r.y, r.w, 1.0f}, 0.0f, t.color.border.fade(t.dark ? 0.7f : 1.0f));
  ui_.interact(ui_.id("action-bar"), r, kNone);
  Rect col = column(r).inset(0.0f, 18.0f);
  const int ready = state_->count(ItemStatus::Ready);
  const int busy = state_->count(ItemStatus::Converting) + state_->count(ItemStatus::Queued);
  const int done = state_->count(ItemStatus::Done);
  const int failed = state_->count(ItemStatus::Failed);
  std::string summary;
  if (busy > 0) {
    summary = "Converting " + std::to_string(busy) + (busy == 1 ? " file\xE2\x80\xA6" : " files\xE2\x80\xA6");
  } else if (ready > 0) {
    summary = std::to_string(ready) + (ready == 1 ? " file ready" : " files ready");
  } else if (done > 0) {
    summary = std::to_string(done) + (done == 1 ? " file converted" : " files converted");
  } else {
    summary = "Nothing to convert";
  }
  if (failed > 0) summary += " \xC2\xB7 " + std::to_string(failed) + " failed";
  ui_.text_in(summary, t.type.body_strong, cut_left(col, col.w * 0.45f), t.color.text_primary);
  ui_.describe(ui_.id("summary"), Role::Label, summary, col);

  if (busy > 0) {
    ButtonOptions bo;
    bo.kind = ButtonKind::Danger;
    bo.icon = Icon::Stop;
    const float w = button_width(ui_, "Cancel all", bo);
    if (button(ui_, ui_.id("cancel-all"), cut_right(col, w), "Cancel all", bo)) state_->cancel_all();
    cut_right(col, 10.0f);
  }
  if (ready > 0) {
    ButtonOptions bo;
    bo.kind = ButtonKind::Primary;
    bo.icon = Icon::ArrowRight;
    const std::string label = ready == 1 ? "Convert" : "Convert all";
    const float w = button_width(ui_, label, bo) + 12.0f;
    if (button(ui_, ui_.id("convert-all"), cut_right(col, w), label, bo)) convert_all();
    cut_right(col, 10.0f);
  }
  if (done > 0) {
    ButtonOptions bo;
    bo.kind = ready > 0 ? ButtonKind::Secondary : ButtonKind::Primary;
    bo.icon = Icon::Save;
    const std::string label = done == 1 ? "Save" : "Save all";
    const float w = button_width(ui_, label, bo) + 8.0f;
    if (button(ui_, ui_.id("save-all"), cut_right(col, w), label, bo)) {
      if (done == 1) {
        for (const auto& item : state_->items()) {
          if (item.status == ItemStatus::Done) {
            save_item(item.id);
            break;
          }
        }
      } else {
        save_all();
      }
    }
  }
}

}  // namespace cvd::app
