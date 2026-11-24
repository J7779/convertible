// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "app/logo.hpp"

#include <cmath>

#include "base/json.hpp"
#include "base/resources.hpp"
#include "ui/theme.hpp"

namespace cvd::app {

float Logo::Layer::value(std::string_view prop, float t_ms, float fallback) const {
  const auto bit = base.find(prop);
  const float b = bit == base.end() ? fallback : bit->second;
  const auto tit = tracks.find(prop);
  if (tit == tracks.end()) return b;
  return tit->second.value(t_ms, b);
}

const Logo::Layer* Logo::Scene::find(std::string_view id) const {
  for (const auto& l : layers) {
    if (l.id == id) return &l;
  }
  return nullptr;
}

const Logo& Logo::instance() {
  static const Logo logo = [] {
    Logo l;
    l.load(resource_text("brand/animation.json"));
    return l;
  }();
  return logo;
}

bool Logo::load(std::string_view json_text) {
  valid_ = false;
  const auto doc = Json::parse(json_text);
  if (!doc) return false;
  const Json& root = *doc;

  for (const auto& [name, pts] : root["easings"].members()) {
    if (pts.size() == 4) {
      easings_[name] = ui::CubicBezier(static_cast<float>(pts[0].as_number()), static_cast<float>(pts[1].as_number()),
                                       static_cast<float>(pts[2].as_number()), static_cast<float>(pts[3].as_number()));
    }
  }

  const Json& geo = root["geometry"];
  const auto centerline = gfx::Path::parse_svg(geo["centerline"]["d"].str());
  const auto outline = gfx::Path::parse_svg(geo["markOutline"]["d"].str());
  if (!centerline || !outline) return false;
  outline_ = *outline;
  const auto flat = centerline->flatten({}, 0.05f);
  if (flat.empty()) return false;
  centerline_ = flat.front().points;
  measure_.emplace(centerline_);
  length_ = measure_->length();
  stroke_width_ = static_cast<float>(geo["centerline"]["strokeWidth"].as_number(200));
  miter_limit_ = static_cast<float>(geo["centerline"]["miterLimit"].as_number(4));
  seed_c_ = {static_cast<float>(geo["seed"]["cx"].as_number(772)), static_cast<float>(geo["seed"]["cy"].as_number(260))};
  seed_r_ = static_cast<float>(geo["seed"]["r"].as_number(100));
  const Json& sq = geo["endSquare"];
  end_square_ = {static_cast<float>(sq["x"].as_number(664)), static_cast<float>(sq["y"].as_number(664)),
                 static_cast<float>(sq["width"].as_number(200)), static_cast<float>(sq["height"].as_number(200))};

  const Json& flux = root["paints"]["flux"];
  flux_p0_ = {static_cast<float>(flux["x1"].as_number(0)), static_cast<float>(flux["y1"].as_number(360))};
  flux_p1_ = {static_cast<float>(flux["x2"].as_number(0)), static_cast<float>(flux["y2"].as_number(664))};
  flux_dark_.clear();
  for (const Json& s : flux["stops"].items()) {
    flux_dark_.push_back({static_cast<float>(s["offset"].as_number()), Color::hex(s["color"].str())});
  }
  iris_ = Color::hex(root["paints"]["iris"]["color"].as_string("#7C6CFF"));
  coral_ = Color::hex(root["paints"]["coral"]["color"].as_string("#FF6452"));
  // Deeper tones for light backgrounds come from the design tokens.
  const Json& tokens = ui::Tokens::instance().root();
  flux_light_.clear();
  for (const Json& s : tokens["brand"]["gradient"]["stopsOnLight"].items()) {
    flux_light_.push_back({static_cast<float>(s["offset"].as_number()), Color::hex(s["color"].str())});
  }
  if (flux_light_.empty()) {
    flux_light_ = {{0.0f, Color::hex("#6654F0")}, {0.5f, Color::hex("#C94FA3")}, {1.0f, Color::hex("#F04A36")}};
  }
  if (tokens["brand"].contains("irisDeep")) iris_light_ = Color::hex(tokens["brand"]["irisDeep"].str());
  if (tokens["brand"].contains("coralDeep")) coral_light_ = Color::hex(tokens["brand"]["coralDeep"].str());

  const auto parse_scene = [&](const Json& j, Scene& scene) {
    scene.duration_ms = static_cast<float>(j["durationMs"].as_number(0));
    scene.loop = j["loop"].as_bool();
    scene.layers.clear();
    for (const Json& lj : j["layers"].items()) {
      Layer l;
      l.id = lj["id"].str();
      l.type = lj["type"].str();
      l.geometry = lj["geometry"].str();
      l.paint = lj.contains("fill") ? lj["fill"].str() : lj["stroke"].str();
      if (lj["pivot"].size() == 2) {
        l.pivot = {static_cast<float>(lj["pivot"][0].as_number()), static_cast<float>(lj["pivot"][1].as_number())};
      } else if (l.geometry == "seed") {
        l.pivot = seed_c_;
      }
      for (const auto& [k, v] : lj["base"].members()) l.base[k] = static_cast<float>(v.as_number());
      for (const Json& c : lj["children"].items()) l.children.push_back(c.str());
      for (const Json& tj : lj["tracks"].items()) {
        std::vector<ui::Keyframe> keys;
        for (const Json& kj : tj["keyframes"].items()) {
          ui::Keyframe k;
          k.t_ms = static_cast<float>(kj["t"].as_number());
          k.value = static_cast<float>(kj["value"].as_number());
          const std::string& e = kj["easing"].str();
          if (!e.empty()) {
            k.has_easing = true;
            const auto it = easings_.find(e);
            k.easing = it != easings_.end() ? it->second : ui::CubicBezier(0.0f, 0.0f, 1.0f, 1.0f);
          }
          keys.push_back(k);
        }
        l.tracks[tj["property"].str()] = ui::Track(std::move(keys));
      }
      scene.layers.push_back(std::move(l));
    }
  };
  parse_scene(root["intro"], intro_);
  parse_scene(root["spinner"], spinner_);
  if (const auto it = easings_.find("inOut"); it != easings_.end()) pulse_easing_ = it->second;
  valid_ = !intro_.layers.empty() && length_ > 0.0f;
  return valid_;
}

void Logo::apply_box(gfx::Canvas& c, const Rect& box) const {
  // The 1024 canvas is scaled uniformly and centred in the box.
  const float s = std::min(box.w, box.h) / 1024.0f;
  c.translate(box.x + (box.w - 1024.0f * s) * 0.5f, box.y + (box.h - 1024.0f * s) * 0.5f);
  c.scale(s, s);
}

gfx::Paint Logo::paint(std::string_view name, bool on_light) const {
  if (name == "iris") return gfx::Paint(on_light ? iris_light_ : iris_);
  if (name == "coral") return gfx::Paint(on_light ? coral_light_ : coral_);
  return gfx::Paint::linear(flux_p0_, flux_p1_, on_light ? flux_light_ : flux_dark_);
}

void Logo::draw_trace(gfx::Canvas& c, float start, float end, const gfx::Paint& p) const {
  if (!measure_ || end - start <= 1e-4f) return;
  const std::vector<Vec2> pts = measure_->trim(start, end);
  if (pts.size() < 2) return;
  gfx::StrokeStyle st;
  st.width = stroke_width_;
  st.cap = gfx::LineCap::Butt;
  st.join = gfx::LineJoin::Miter;
  st.miter_limit = miter_limit_;
  c.stroke_polyline(pts, false, st, p);
}

bool Logo::is_full_mark_group(const Scene& scene, const Layer& group, float t) const {
  bool stroke = false, seed = false;
  for (const auto& child : group.children) {
    const Layer* l = scene.find(child);
    if (!l) return false;
    if (l->value("opacity", t, 1.0f) < 0.999f || std::fabs(l->value("scale", t, 1.0f) - 1.0f) > 1e-4f) return false;
    if (l->type == "stroke" && l->geometry == "centerline" && l->value("trimStart", t, 0.0f) <= 0.0f &&
        l->value("trimEnd", t, 1.0f) >= 1.0f) {
      stroke = true;
    } else if (l->geometry == "seed") {
      seed = true;
    } else {
      return false;
    }
  }
  return stroke && seed;
}

void Logo::draw_layer(gfx::Canvas& c, const Scene& scene, const Layer& layer, float t, bool on_light) const {
  const float opacity = layer.value("opacity", t, 1.0f);
  if (opacity <= 0.001f) return;
  c.save();
  c.set_opacity(opacity);
  const float scale = layer.value("scale", t, 1.0f);
  const float rot = layer.value("rotationDeg", t, 0.0f);
  if (scale != 1.0f || rot != 0.0f) {
    c.translate(layer.pivot.x, layer.pivot.y);
    if (rot != 0.0f) c.rotate(rot * kPi / 180.0f);
    c.scale(scale, scale);
    c.translate(-layer.pivot.x, -layer.pivot.y);
  }
  if (layer.type == "group" && opacity < 1.0f && is_full_mark_group(scene, layer, t)) {
    // Group opacity applies to the composited group; a fully drawn stroke plus
    // seed is exactly the mark outline, so fill that once instead of
    // overlapping two translucent shapes.
    c.fill_path(outline_, paint("flux", on_light));
  } else if (layer.type == "group") {
    for (const auto& child : layer.children) {
      if (const Layer* l = scene.find(child)) draw_layer(c, scene, *l, t, on_light);
    }
  } else if (scale > 0.0005f) {
    const gfx::Paint p = paint(layer.paint, on_light);
    if (layer.type == "stroke" && layer.geometry == "centerline") {
      draw_trace(c, layer.value("trimStart", t, 0.0f), layer.value("trimEnd", t, 1.0f), p);
    } else if (layer.geometry == "seed") {
      gfx::Path circle;
      circle.add_circle(seed_c_, seed_r_);
      c.fill_path(circle, p);
    } else if (layer.geometry == "endSquare") {
      gfx::Path sq;
      sq.add_rect(end_square_);
      c.fill_path(sq, p);
    }
  }
  c.restore();
}

void Logo::draw_scene(gfx::Canvas& c, const Scene& scene, float t, bool on_light, float opacity_scale) const {
  // Children of groups are drawn by their group; everything else in listed order.
  std::vector<std::string_view> owned;
  for (const auto& l : scene.layers) {
    for (const auto& ch : l.children) owned.push_back(ch);
  }
  c.save();
  c.set_opacity(opacity_scale);
  for (const auto& l : scene.layers) {
    if (std::find(owned.begin(), owned.end(), l.id) != owned.end()) continue;
    draw_layer(c, scene, l, t, on_light);
  }
  c.restore();
}

void Logo::draw_static(gfx::Canvas& c, const Rect& box, bool on_light) const {
  if (!valid_) return;
  c.save();
  apply_box(c, box);
  c.fill_path(outline_, paint("flux", on_light));
  c.restore();
}

void Logo::draw_intro(gfx::Canvas& c, const Rect& box, float t_ms, bool on_light) const {
  if (!valid_) return;
  if (t_ms >= intro_.duration_ms) {
    draw_static(c, box, on_light);
    return;
  }
  c.save();
  apply_box(c, box);
  draw_scene(c, intro_, std::max(0.0f, t_ms), on_light, 1.0f);
  c.restore();
}

void Logo::draw_spinner(gfx::Canvas& c, const Rect& box, float t_ms, bool on_light, bool reduced_motion) const {
  if (!valid_) return;
  c.save();
  apply_box(c, box);
  if (reduced_motion) {
    // Runner and seed fully drawn, opacity pulsing 1 <-> 0.45 (800 ms each way).
    const float phase = std::fmod(std::max(0.0f, t_ms), 1600.0f);
    const float u = phase < 800.0f ? phase / 800.0f : 1.0f - (phase - 800.0f) / 800.0f;
    const float alpha = 1.0f - 0.55f * pulse_easing_(u);
    if (const Layer* track = spinner_.find("track")) draw_layer(c, spinner_, *track, 0.0f, on_light);
    c.set_opacity(alpha);
    c.fill_path(outline_, paint("flux", on_light));
  } else {
    const float t = spinner_.duration_ms > 0.0f ? std::fmod(std::max(0.0f, t_ms), spinner_.duration_ms) : 0.0f;
    draw_scene(c, spinner_, t, on_light, 1.0f);
  }
  c.restore();
}

void Logo::draw_progress(gfx::Canvas& c, const Rect& box, float p, bool on_light) const {
  if (!valid_) return;
  p = clampf(p, 0.0f, 1.0f);
  c.save();
  apply_box(c, box);
  if (p >= 0.9995f) {
    // Finished: exactly the static mark.
    c.fill_path(outline_, paint("flux", on_light));
  } else {
    if (const Layer* track = spinner_.find("track")) draw_layer(c, spinner_, *track, 0.0f, on_light);
    const gfx::Paint flux = paint("flux", on_light);
    draw_trace(c, 0.0f, p, flux);
    if (p > 0.0f) {
      gfx::Path circle;
      circle.add_circle(seed_c_, seed_r_);
      c.fill_path(circle, flux);
    }
  }
  c.restore();
}

void Logo::draw_completion_ripple(gfx::Canvas& c, const Rect& box, float t_ms, bool on_light) const {
  const Layer* ripple = intro_.find("rippleSquare");
  if (!valid_ || !ripple || t_ms < 0.0f || t_ms > 480.0f) return;
  // Replay the intro's square ripple track (1180..1720 ms) over 480 ms.
  const float start = ripple->tracks.count("scale") ? ripple->tracks.at("scale").keys().front().t_ms : 1180.0f;
  const float end = ripple->tracks.count("scale") ? ripple->tracks.at("scale").end_time() : 1720.0f;
  const float t = start + (end - start) * (t_ms / 480.0f);
  c.save();
  apply_box(c, box);
  draw_layer(c, intro_, *ripple, t, on_light);
  c.restore();
}

}  // namespace cvd::app
