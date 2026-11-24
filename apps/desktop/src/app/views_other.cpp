// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Formats browser, settings/about and the open-source licences screen.
#include <algorithm>
#include <cmath>

#include "app/app.hpp"
#include "app/logo.hpp"
#include "base/resources.hpp"
#include "base/text_util.hpp"

namespace cvd::app {

using namespace ui;

namespace {

Rect column(const Rect& r, float max_w = 880.0f, float pad = 24.0f) {
  const float w = std::min(max_w, r.w - pad * 2.0f);
  return {r.x + (r.w - w) * 0.5f, r.y, w, r.h};
}

/// How well a format matches the query: 0 = not at all; exact ids and
/// extensions first, then prefixes, names, categories and descriptions.
int format_score(const FormatInfo& f, std::string_view q) {
  std::string_view query = trim(q);
  if (query.empty()) return 1;
  std::string_view e = query;
  if (!e.empty() && e.front() == '.') e.remove_prefix(1);
  if (e.empty()) return 1;
  const auto equals_ci = [](std::string_view a, std::string_view b) { return a.size() == b.size() && starts_with_ci(a, b); };
  if (equals_ci(f.id, e)) return 100;
  for (const auto& ext : f.extensions) {
    if (equals_ci(ext, e)) return 95;
  }
  if (starts_with_ci(f.id, e)) return 80;
  for (const auto& ext : f.extensions) {
    if (starts_with_ci(ext, e)) return 70;
  }
  if (starts_with_ci(f.name, query)) return 60;
  if (contains_ci(f.id, query) || contains_ci(f.name, query)) return 40;
  if (contains_ci(category_label(f.category), query)) return 20;
  if (contains_ci(f.description, query)) return 10;
  return 0;
}

void pill(Context& ctx, const Rect& r, std::string_view label, bool on, Color on_color) {
  const Theme& t = ctx.theme();
  ctx.fill(r, r.h * 0.5f, on ? on_color.fade(t.dark ? 0.16f : 0.12f) : Color{0, 0, 0, 0});
  ctx.stroke(r, r.h * 0.5f, 1.0f, on ? on_color.fade(0.35f) : t.color.border);
  ctx.text_in(label, t.type.caption, r, on ? on_color : t.color.text_disabled, 0.5f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Formats browser
// ---------------------------------------------------------------------------

void App::build_formats(Rect r) {
  const Theme& t = theme_;
  Rect col = column(r);
  cut_top(col, 20.0f);
  const Catalog& cat = Catalog::instance();
  Rect head = cut_top(col, 40.0f);
  ui_.text_in("Formats", t.type.title1, head, t.color.text_primary);
  Rect sub = cut_top(col, 26.0f);
  ui_.text_in(std::to_string(cat.formats().size()) + " formats recognised. Any file can also become Base64, hex or a checksum report.",
              t.type.body, sub, t.color.text_secondary);
  cut_top(col, 16.0f);

  Rect search = cut_top(col, 44.0f);
  TextFieldOptions so;
  so.placeholder = "Search by name or extension, e.g. png, csv, audio";
  so.leading = Icon::Search;
  so.clear_button = true;
  text_field(ui_, ui_.id("formats-search"), search, formats_query_, "Search formats", so);
  cut_top(col, 12.0f);

  // Category filter chips (only categories that have formats).
  std::vector<std::string> cats;
  for (const auto& c : category_order()) {
    if (std::any_of(cat.formats().begin(), cat.formats().end(), [&](const FormatInfo& f) { return f.category == c; })) {
      cats.push_back(c);
    }
  }
  {
    float x = col.x;
    float y = col.y;
    const float h = 30.0f;
    const auto chip_btn = [&](const std::string& id, const std::string& label, Color dot) {
      const float w = text::measure(label, t.type.label) + (dot.a > 0 ? 34.0f : 24.0f);
      if (x + w > col.right()) {
        x = col.x;
        y += h + 8.0f;
      }
      const Rect cr{x, y, w, h};
      const WidgetId cid = ui_.id("cat", hash_id(id));
      const Interaction in = ui_.interact(cid, cr);
      const bool sel = formats_category_ == id;
      ui_.describe(cid, Role::Tab, label, cr, {}, sel);
      if (in.hovered) ui_.set_cursor(Cursor::Hand);
      if (in.clicked) formats_category_ = sel && !id.empty() ? std::string() : id;
      ui_.fill(cr, h * 0.5f, sel ? t.color.accent_subtle : (in.hovered ? t.color.surface_elevated : t.color.surface.fade(0.6f)));
      ui_.stroke(cr, h * 0.5f, 1.0f, sel ? t.color.accent.fade(0.6f) : t.color.border);
      float tx = cr.x + 12.0f;
      if (dot.a > 0) {
        ui_.fill({tx, cr.center().y - 3.0f, 6.0f, 6.0f}, 3.0f, dot);
        tx += 12.0f;
      }
      ui_.text_in(label, t.type.label, {tx, cr.y, cr.w, cr.h}, sel ? t.color.accent_text : t.color.text_primary);
      ui_.focus_ring(cid, cr, h * 0.5f);
      x += w + 8.0f;
    };
    chip_btn("", "All", Color{0, 0, 0, 0});
    for (const auto& c : cats) chip_btn(c, category_label(c), t.category(c));
    cut_top(col, (y - col.y) + h + 16.0f);
  }

  // List grouped by category, rebuilt only when the query or the category
  // filter changes (the catalogue has ~1,500 formats). With a query, each
  // group is ordered by match quality (ties keep the catalogue's popularity
  // order) and the group holding the best match comes first.
  const std::string key = std::string(trim(formats_query_)) + '\x1f' + formats_category_ + '\x1f' +
                          std::to_string(cat.formats().size());
  if (key != formats_rows_key_) {
    formats_rows_key_ = key;
    formats_rows_.clear();
    struct Group {
      std::string category;
      std::vector<std::pair<int, const FormatInfo*>> fs;
      int best = 0;
    };
    std::vector<Group> groups;
    for (const auto& c : category_order()) {
      if (!formats_category_.empty() && c != formats_category_) continue;
      Group g{c, {}, 0};
      for (const auto& f : cat.formats()) {
        if (f.category != c) continue;
        if (const int score = format_score(f, formats_query_)) g.fs.emplace_back(score, &f);
      }
      if (g.fs.empty()) continue;
      std::stable_sort(g.fs.begin(), g.fs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      g.best = g.fs.front().first;
      groups.push_back(std::move(g));
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.best > b.best; });
    int matched = 0;
    for (const auto& g : groups) {
      formats_rows_.push_back({true, g.category, nullptr, static_cast<int>(g.fs.size())});
      for (const auto& [score, f] : g.fs) formats_rows_.push_back({false, g.category, f, 0});
      matched += static_cast<int>(g.fs.size());
    }
    formats_matched_ = matched;
  }
  const auto& rows = formats_rows_;
  const float header_h = 44.0f;
  const float row_h = 56.0f;
  float content = 0.0f;
  for (const auto& row : rows) content += row.header ? header_h : row_h;
  content += 24.0f;
  if (rows.empty()) content = 120.0f;
  Rect list{col.x - 8.0f, col.y, col.w + 16.0f, r.bottom() - col.y};
  const float off = ui_.begin_scroll(ui_.id("formats-scroll"), list, content);
  float y = list.y;
  for (const auto& row : rows) {
    const float h = row.header ? header_h : row_h;
    if (y + h < list.y + off - 20.0f || y > list.y + off + list.h + 20.0f) {
      y += h;
      continue;
    }
    if (row.header) {
      const Rect hr{col.x, y + 12.0f, col.w, 24.0f};
      const Color c = t.category(row.category);
      ui_.fill({hr.x, hr.center().y - 4.0f, 8.0f, 8.0f}, 4.0f, c);
      ui_.text_in(category_label(row.category), t.type.overline, {hr.x + 18.0f, hr.y, 300.0f, hr.h}, t.color.text_secondary);
      ui_.text_in(std::to_string(row.count), t.type.caption, hr, t.color.text_secondary, 1.0f);
    } else {
      const FormatInfo& f = *row.format;
      const Rect rr{col.x, y, col.w, row_h - 4.0f};
      const WidgetId rid = ui_.id("format", hash_id(f.id));
      const Interaction in = ui_.interact(rid, rr, kFocusable);
      const std::string rw = std::string(f.readable ? "reads" : "detects") + (f.writable ? ", writes" : "");
      ui_.describe(rid, Role::ListItem, f.name, rr, rw);
      ui_.fill(rr, t.radius_md, in.hovered ? t.color.surface_elevated : t.color.surface.fade(t.dark ? 0.6f : 1.0f));
      ui_.focus_ring(rid, rr, t.radius_md);
      const Color tint = t.category(f.category);
      std::string exts;
      for (std::size_t i = 0; i < f.extensions.size() && i < 4; ++i) exts += (i ? "  ." : ".") + f.extensions[i];
      const std::string label = ascii_upper(f.id);
      const float cw = std::min(chip_width(ui_, label), 130.0f);
      chip(ui_, {rr.x + 12.0f, rr.center().y - 12.0f, cw, 24.0f}, text::ellipsize_end(label, t.type.format_label, cw - 26.0f),
           tint, false);
      Rect text_r{rr.x + 156.0f, rr.y + 7.0f, rr.w - 156.0f - 150.0f, 20.0f};
      ui_.text_in(text::ellipsize_end(f.name, t.type.body_strong, text_r.w), t.type.body_strong, text_r, t.color.text_primary);
      const std::string desc = f.description.empty() ? exts : f.description;
      ui_.text_in(text::ellipsize_end(desc, t.type.caption, text_r.w), t.type.caption,
                  {text_r.x, text_r.bottom() + 1.0f, text_r.w, 18.0f}, t.color.text_secondary);
      Rect pills{rr.right() - 140.0f, rr.center().y - 11.0f, 128.0f, 22.0f};
      pill(ui_, cut_left(pills, 60.0f), "Read", f.readable, t.color.success);
      cut_left(pills, 8.0f);
      pill(ui_, cut_left(pills, 60.0f), "Write", f.writable, t.color.accent_text);
    }
    y += h;
  }
  if (rows.empty()) {
    ui_.text_in("No format matches \xE2\x80\x9C" + std::string(trim(formats_query_)) + "\xE2\x80\x9D", t.type.body,
                {col.x, list.y + 40.0f, col.w, 24.0f}, t.color.text_secondary, 0.5f);
  }
  ui_.end_scroll();
}

// ---------------------------------------------------------------------------
// Settings and about
// ---------------------------------------------------------------------------

void App::build_settings(Rect r) {
  const Theme& t = theme_;
  Rect col0 = column(r, 720.0f);
  const float content_h = 1240.0f;
  Rect view{col0.x - 8.0f, r.y, col0.w + 16.0f, r.h};
  const float off = ui_.begin_scroll(ui_.id("settings-scroll"), view, content_h);
  (void)off;
  Rect col = col0;
  cut_top(col, 20.0f);
  ui_.text_in("Settings", t.type.title1, cut_top(col, 40.0f), t.color.text_primary);
  cut_top(col, 16.0f);

  Settings s = state_->settings;
  const auto section = [&](const char* title, float height) {
    Rect sec = cut_top(col, height);
    ui_.text_in(title, t.type.overline, cut_top(sec, 24.0f), t.color.text_secondary);
    const Rect card = sec;
    ui_.shadow(card, t.radius_lg, 1);
    ui_.fill(card, t.radius_lg, t.dark ? t.color.surface : t.color.surface);
    ui_.stroke(card, t.radius_lg, 1.0f, t.color.border.fade(t.dark ? 0.8f : 1.0f));
    cut_top(col, 24.0f);
    return card.inset(20.0f, 8.0f);
  };
  const auto setting_row = [&](Rect& card, const char* label, const char* detail, float h) {
    Rect row = cut_top(card, h);
    Rect text_r = cut_left(row, row.w * 0.48f);
    ui_.text_in(label, t.type.body_strong, {text_r.x, text_r.y + (detail ? 10.0f : 0.0f), text_r.w, detail ? 22.0f : text_r.h},
                t.color.text_primary);
    if (detail) {
      paragraph(ui_, detail, t.type.caption, {text_r.x, text_r.y + 34.0f, text_r.w - 12.0f, 32.0f}, t.color.text_secondary, 2);
    }
    return row;
  };

  // Appearance.
  {
    Rect card = section("Appearance", 24.0f + 3 * 68.0f + 16.0f);
    static const std::vector<std::string> themes = {"System", "Dark", "Light"};
    Rect row = setting_row(card, "Theme", nullptr, 68.0f);
    int sel = static_cast<int>(s.theme);
    if (segmented(ui_, ui_.id("set-theme"), centered(row, row.w, 40.0f), themes, sel, "Theme")) {
      s.theme = static_cast<ThemeMode>(sel);
    }
    ui_.fill({card.x, card.y, card.w, 1.0f}, 0.0f, t.color.border.fade(0.6f));
    static const std::vector<std::string> motions = {"System", "Reduced", "Full"};
    row = setting_row(card, "Motion", nullptr, 68.0f);
    sel = static_cast<int>(s.motion);
    if (segmented(ui_, ui_.id("set-motion"), centered(row, row.w, 40.0f), motions, sel, "Motion")) {
      s.motion = static_cast<MotionMode>(sel);
    }
    ui_.fill({card.x, card.y, card.w, 1.0f}, 0.0f, t.color.border.fade(0.6f));
    static const std::vector<std::string> sizes = {"100%", "115%", "130%"};
    row = setting_row(card, "Text size", nullptr, 68.0f);
    sel = s.text_scale_percent >= 130 ? 2 : s.text_scale_percent >= 115 ? 1 : 0;
    if (segmented(ui_, ui_.id("set-scale"), centered(row, row.w, 40.0f), sizes, sel, "Text size")) {
      s.text_scale_percent = sel == 2 ? 130 : sel == 1 ? 115 : 100;
    }
  }

  // Conversion defaults.
  {
    Rect card = section("Conversion defaults", 24.0f + 2 * 84.0f + 16.0f);
    static const std::vector<std::string> presets = {"High", "Balanced", "Small"};
    Rect row = setting_row(card, "Default quality", "Used when a format offers quality presets.", 84.0f);
    int sel = s.default_preset;
    if (segmented(ui_, ui_.id("set-preset"), centered(row, row.w, 40.0f), presets, sel, "Default quality")) {
      s.default_preset = sel;
    }
    ui_.fill({card.x, card.y, card.w, 1.0f}, 0.0f, t.color.border.fade(0.6f));
    row = setting_row(card, "Strip metadata", "Remove location, camera and author details where the format allows.", 84.0f);
    bool strip = s.strip_metadata;
    if (toggle(ui_, ui_.id("set-strip"), {row.right() - 50.0f, row.y, 50.0f, row.h}, strip, "Strip metadata")) {
      s.strip_metadata = strip;
    }
  }

  // Privacy.
  {
    const char* body =
        "Convertible works completely offline. It has no accounts, no analytics, no crash reporting and no network "
        "access. Recent conversions live in memory only and disappear when you quit; nothing is uploaded or kept.";
    // The card grows with the wrapped text (narrow windows, larger text).
    const float text_w = col.w - 40.0f - 38.0f;
    const float body_h = static_cast<float>(text::wrap(body, t.type.body, text_w).size()) * t.type.body.line_height;
    const float row_h = 30.0f + body_h + 24.0f;
    Rect card = section("Privacy", 24.0f + row_h + 16.0f);
    Rect row = cut_top(card, row_h).inset(0.0f, 10.0f);
    icon(ui_, Icon::Shield, {row.x, row.y + 2.0f, 24.0f, 24.0f}, t.color.success);
    Rect text_r{row.x + 38.0f, row.y, row.w - 38.0f, row.h};
    ui_.text_in("Your files never leave this device", t.type.body_strong, {text_r.x, text_r.y, text_r.w, 24.0f},
                t.color.text_primary);
    paragraph(ui_, body, t.type.body, {text_r.x, text_r.y + 30.0f, text_r.w, body_h}, t.color.text_secondary);
    ui_.describe(ui_.id("privacy-text"), Role::Label, body, row);
  }

  // About.
  {
    Rect card = section("About", 24.0f + 150.0f);
    Rect row = cut_top(card, 134.0f).inset(0.0f, 12.0f);
    const Rect logo_r{row.x, row.y + 6.0f, 56.0f, 56.0f};
    const bool light = !t.dark;
    ui_.draw([logo_r, light](gfx::Canvas& c) { Logo::instance().draw_static(c, logo_r, light); });
    Rect text_r{row.x + 76.0f, row.y, row.w - 76.0f, row.h};
    ui_.text_in("Convertible", t.type.title3, {text_r.x, text_r.y + 4.0f, text_r.w, 26.0f}, t.color.text_primary);
    text::Style mono = t.type.mono;
    const std::string versions = std::string("App ") + CVD_VERSION_STRING + "  \xC2\xB7  Core " + core_version();
    ui_.text_in(versions, mono, {text_r.x, text_r.y + 34.0f, text_r.w, 20.0f}, t.color.text_secondary);
    ui_.describe(ui_.id("versions"), Role::Label, versions, {text_r.x, text_r.y + 34.0f, text_r.w, 20.0f});
    ui_.text_in("Open source, MIT licence. Fonts: Inter and JetBrains Mono (SIL OFL 1.1).", t.type.caption,
                {text_r.x, text_r.y + 58.0f, text_r.w, 18.0f}, t.color.text_secondary);
    ButtonOptions bo;
    bo.kind = ButtonKind::Secondary;
    bo.style = &t.type.label;
    bo.trailing = Icon::ChevronRight;
    const float bw = button_width(ui_, "Open-source licences", bo);
    if (button(ui_, ui_.id("licences"), {text_r.x, text_r.y + 84.0f, bw, 36.0f}, "Open-source licences", bo)) {
      navigate(Screen::Licenses);
    }
  }
  ui_.end_scroll();
  if (!(s == state_->settings)) apply_settings(s);
}

void App::build_licenses(Rect r) {
  const Theme& t = theme_;
  Rect col = column(r, 760.0f);
  cut_top(col, 16.0f);
  Rect head = cut_top(col, 44.0f);
  ButtonOptions back;
  back.kind = ButtonKind::Ghost;
  back.icon = Icon::ChevronLeft;
  back.style = &t.type.label;
  if (button(ui_, ui_.id("back"), {head.x - 8.0f, head.y + 4.0f, button_width(ui_, "Settings", back), 36.0f}, "Settings", back)) {
    navigate(Screen::Settings);
  }
  ui_.text_in("Open-source licences", t.type.title2, cut_top(col, 40.0f), t.color.text_primary);
  cut_top(col, 8.0f);
  struct Doc {
    const char* title;
    const char* resource;
  };
  static const Doc docs[] = {{"Convertible (MIT)", "licenses/LICENSE"},
                             {"Inter typeface (SIL Open Font License 1.1)", "licenses/Inter-LICENSE.txt"},
                             {"JetBrains Mono typeface (SIL Open Font License 1.1)", "licenses/JetBrainsMono-LICENSE.txt"}};
  const text::Style& mono = t.type.mono.with_size(12.0f, 18.0f);
  const float text_w = col.w - 40.0f;
  struct Laid {
    const char* title;
    std::vector<std::string> lines;
  };
  // Wrap once per width.
  static float cached_w = -1.0f;
  static std::vector<Laid> laid;
  if (std::fabs(cached_w - text_w) > 0.5f) {
    laid.clear();
    for (const Doc& d : docs) laid.push_back({d.title, text::wrap(resource_text(d.resource), mono, text_w)});
    cached_w = text_w;
  }
  float content = 0.0f;
  for (const Laid& l : laid) content += 56.0f + static_cast<float>(l.lines.size()) * mono.line_height + 40.0f;
  Rect view{col.x - 8.0f, col.y, col.w + 16.0f, r.bottom() - col.y};
  const float off = ui_.begin_scroll(ui_.id("licences-scroll"), view, content);
  float y = view.y;
  for (const Laid& l : laid) {
    const float h = 56.0f + static_cast<float>(l.lines.size()) * mono.line_height + 16.0f;
    const Rect card{col.x, y, col.w, h};
    if (card.bottom() >= view.y + off - 10.0f && card.y <= view.y + off + view.h + 10.0f) {
      ui_.fill(card, t.radius_lg, t.color.surface);
      ui_.stroke(card, t.radius_lg, 1.0f, t.color.border.fade(0.8f));
      ui_.text_in(l.title, t.type.body_strong, {card.x + 20.0f, card.y + 14.0f, card.w - 40.0f, 24.0f}, t.color.text_primary);
      float ly = card.y + 52.0f;
      for (const auto& line : l.lines) {
        if (ly + mono.line_height >= view.y + off && ly <= view.y + off + view.h) {
          ui_.text(line, mono, {card.x + 20.0f, ly + text::baseline_offset(mono)}, t.color.text_secondary);
        }
        ly += mono.line_height;
      }
      ui_.describe(ui_.id("licence", hash_id(l.title)), Role::Label, l.title, card);
    }
    y += h + 24.0f;
  }
  ui_.end_scroll();
}

}  // namespace cvd::app
