// Convertible core - vector family: lossless JSON form of a scene.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/vector/scene_json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <set>

#include "util/base64.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::vector {

namespace {

// ------------------------------------------------------------------ numbers

std::string num(double v, int precision) {
  if (!std::isfinite(v)) return "0";
  if (precision >= 0) {
    const double scale = std::pow(10.0, precision);
    v = std::round(v * scale) / scale;
    if (v == 0) v = 0;  // no "-0"
  }
  std::string s = util::format_double(v);
  return s;
}

Json jnum(double v) { return Json(std::isfinite(v) ? v : 0.0); }

double get_num(const Json& j, std::string_view key, double fallback) {
  const Json* v = j.find(key);
  if (!v) return fallback;
  const auto d = v->get_double();
  return d && std::isfinite(*d) ? *d : fallback;
}

std::string get_str(const Json& j, std::string_view key) {
  const Json* v = j.find(key);
  if (!v || !v->is_string()) return {};
  return v->as_string();
}

bool get_bool(const Json& j, std::string_view key, bool fallback) {
  const Json* v = j.find(key);
  if (!v) return fallback;
  return v->get_bool().value_or(fallback);
}

Json color_json(const Color& c) { return Json::array({jnum(c.r), jnum(c.g), jnum(c.b), jnum(c.a)}); }

Color color_from(const Json& j) {
  Color c;
  if (!j.is_array() || j.size() < 3) return c;
  c.r = static_cast<float>(std::clamp(j[0].as_double(0), 0.0, 1.0));
  c.g = static_cast<float>(std::clamp(j[1].as_double(0), 0.0, 1.0));
  c.b = static_cast<float>(std::clamp(j[2].as_double(0), 0.0, 1.0));
  c.a = static_cast<float>(std::clamp(j.size() > 3 ? j[3].as_double(1) : 1.0, 0.0, 1.0));
  return c;
}

Json matrix_json(const Matrix& m) {
  return Json::array({jnum(m.a), jnum(m.b), jnum(m.c), jnum(m.d), jnum(m.e), jnum(m.f)});
}

Matrix matrix_from(const Json* j) {
  if (!j || !j->is_array() || j->size() != 6) return {};
  Matrix m{(*j)[0].as_double(1), (*j)[1].as_double(0), (*j)[2].as_double(0),
           (*j)[3].as_double(1), (*j)[4].as_double(0), (*j)[5].as_double(0)};
  for (const double v : {m.a, m.b, m.c, m.d, m.e, m.f}) {
    if (!std::isfinite(v)) return {};
  }
  return m;
}

Json rect_json(const Rect& r) { return Json::array({jnum(r.x0), jnum(r.y0), jnum(r.x1), jnum(r.y1)}); }

Rect rect_from(const Json* j) {
  if (!j || !j->is_array() || j->size() != 4) return {};
  return {(*j)[0].as_double(0), (*j)[1].as_double(0), (*j)[2].as_double(-1), (*j)[3].as_double(-1)};
}

constexpr std::array<std::string_view, 16> blend_names = {
    "normal",     "multiply",   "screen",     "overlay", "darken",     "lighten", "color-dodge", "color-burn",
    "hard-light", "soft-light", "difference", "exclusion", "hue", "saturation", "color", "luminosity"};

std::string_view blend_name(BlendMode m) { return blend_names[static_cast<std::size_t>(m)]; }
BlendMode blend_from(std::string_view s) {
  for (std::size_t i = 0; i < blend_names.size(); ++i) {
    if (blend_names[i] == s) return static_cast<BlendMode>(i);
  }
  return BlendMode::Normal;
}

constexpr std::array<std::string_view, 8> primitive_names = {"blur",  "offset", "flood", "merge",
                                                             "color-matrix", "drop-shadow", "blend", "composite"};

// ------------------------------------------------------------------ writer

struct Writer {
  Json defs = Json::object();
  std::map<const void*, std::string> keys;
  int counter = 0;

  std::string key_for(const void* p, char prefix) {
    auto it = keys.find(p);
    if (it != keys.end()) return it->second;
    std::string key = std::format("{}{}", prefix, counter++);
    keys.emplace(p, key);
    return key;
  }

  Json gradient(const std::shared_ptr<const Gradient>& g) {
    const auto it = keys.find(g.get());
    if (it != keys.end()) return Json(it->second);
    const std::string key = key_for(g.get(), 'g');
    Json j = Json::object();
    j["type"] = g->type == Gradient::Type::Linear ? "linear" : "radial";
    if (g->type == Gradient::Type::Linear) {
      j["p1"] = Json::array({jnum(g->p1.x), jnum(g->p1.y)});
      j["p2"] = Json::array({jnum(g->p2.x), jnum(g->p2.y)});
    } else {
      j["center"] = Json::array({jnum(g->center.x), jnum(g->center.y)});
      j["radius"] = jnum(g->radius);
      j["focal"] = Json::array({jnum(g->focal.x), jnum(g->focal.y)});
      j["focal_radius"] = jnum(g->focal_radius);
    }
    Json stops = Json::array();
    for (const GradientStop& s : g->stops) stops.push_back(Json::array({jnum(s.offset), color_json(s.color)}));
    j["stops"] = std::move(stops);
    j["spread"] = g->spread == Spread::Pad ? "pad" : g->spread == Spread::Reflect ? "reflect" : "repeat";
    if (!g->transform.is_identity(0)) j["transform"] = matrix_json(g->transform);
    if (g->linear_rgb) j["linear_rgb"] = true;
    defs[key] = std::move(j);
    return Json(key);
  }

  Json pattern(const std::shared_ptr<const PatternPaint>& p) {
    const auto it = keys.find(p.get());
    if (it != keys.end()) return Json(it->second);
    const std::string key = key_for(p.get(), 'p');
    defs[key] = nullptr;  // reserve the slot (keeps definition order stable)
    Json j = Json::object();
    j["tile"] = rect_json(p->tile);
    if (!p->transform.is_identity(0)) j["transform"] = matrix_json(p->transform);
    if (p->content) j["content"] = group(*p->content);
    defs[key] = std::move(j);
    return Json(key);
  }

  Json paint(const Paint& p) {
    if (p.type == Paint::Type::None) return nullptr;
    Json j = Json::object();
    switch (p.type) {
      case Paint::Type::None: break;
      case Paint::Type::Solid: j["color"] = color_json(p.color); break;
      case Paint::Type::Gradient:
        if (p.gradient) j["gradient"] = gradient(p.gradient);
        j["color"] = color_json(p.color);
        break;
      case Paint::Type::Pattern:
        if (p.pattern) j["pattern"] = pattern(p.pattern);
        j["color"] = color_json(p.color);
        break;
    }
    if (p.opacity != 1.0) j["opacity"] = jnum(p.opacity);
    return j;
  }

  Json stroke(const Stroke& s) {
    Json j = Json::object();
    j["paint"] = paint(s.paint);
    j["width"] = jnum(s.width);
    if (s.cap != LineCap::Butt) j["cap"] = s.cap == LineCap::Round ? "round" : "square";
    if (s.join != LineJoin::Miter) {
      constexpr std::array<std::string_view, 5> names = {"miter", "round", "bevel", "miter-clip", "arcs"};
      j["join"] = names[static_cast<std::size_t>(s.join)];
    }
    if (s.miter_limit != 4.0) j["miter_limit"] = jnum(s.miter_limit);
    if (!s.dash.empty()) {
      Json d = Json::array();
      for (const double v : s.dash) d.push_back(jnum(v));
      j["dash"] = std::move(d);
      if (s.dash_offset != 0) j["dash_offset"] = jnum(s.dash_offset);
    }
    if (s.non_scaling) j["non_scaling"] = true;
    if (s.hairline) j["hairline"] = true;
    return j;
  }

  Json clip(const std::shared_ptr<const ClipPath>& c) {
    const auto it = keys.find(c.get());
    if (it != keys.end()) return Json(it->second);
    const std::string key = key_for(c.get(), 'c');
    defs[key] = nullptr;
    Json j = Json::object();
    Json items = Json::array();
    for (const ClipPath::Item& item : c->items) {
      Json i = Json::object();
      i["d"] = path_to_string(item.path);
      if (item.rule == FillRule::EvenOdd) i["rule"] = "evenodd";
      items.push_back(std::move(i));
    }
    j["items"] = std::move(items);
    if (c->clip) j["clip"] = clip(c->clip);
    defs[key] = std::move(j);
    return Json(key);
  }

  Json mask(const std::shared_ptr<const Mask>& m) {
    const auto it = keys.find(m.get());
    if (it != keys.end()) return Json(it->second);
    const std::string key = key_for(m.get(), 'm');
    defs[key] = nullptr;
    Json j = Json::object();
    j["type"] = m->type == Mask::Type::Luminance ? "luminance" : "alpha";
    if (!m->region.empty()) j["region"] = rect_json(m->region);
    if (m->content) j["content"] = group(*m->content);
    defs[key] = std::move(j);
    return Json(key);
  }

  Json filter(const std::shared_ptr<const Filter>& f) {
    const auto it = keys.find(f.get());
    if (it != keys.end()) return Json(it->second);
    const std::string key = key_for(f.get(), 'f');
    Json j = Json::object();
    if (!f->region.empty()) j["region"] = rect_json(f->region);
    Json prims = Json::array();
    for (const FilterPrimitive& p : f->primitives) {
      Json q = Json::object();
      q["type"] = primitive_names[static_cast<std::size_t>(p.type)];
      if (!p.in.empty()) q["in"] = p.in;
      if (!p.in2.empty()) q["in2"] = p.in2;
      if (!p.result.empty()) q["result"] = p.result;
      if (!p.merge_inputs.empty()) q["merge"] = Json(p.merge_inputs);
      if (p.std_dev_x != 0 || p.std_dev_y != 0) q["std_dev"] = Json::array({jnum(p.std_dev_x), jnum(p.std_dev_y)});
      if (p.dx != 0 || p.dy != 0) q["offset"] = Json::array({jnum(p.dx), jnum(p.dy)});
      if (p.type == FilterPrimitive::Type::Flood || p.type == FilterPrimitive::Type::DropShadow) {
        q["color"] = color_json(p.color);
      }
      if (!p.matrix.empty()) {
        Json m = Json::array();
        for (const double v : p.matrix) m.push_back(jnum(v));
        q["matrix"] = std::move(m);
      }
      if (p.type == FilterPrimitive::Type::Blend) q["mode"] = blend_name(p.blend);
      if (p.type == FilterPrimitive::Type::Composite) {
        q["operator"] = p.op;
        if (p.op == "arithmetic") q["k"] = Json::array({jnum(p.k1), jnum(p.k2), jnum(p.k3), jnum(p.k4)});
      }
      if (!p.linear_rgb) q["linear_rgb"] = false;
      prims.push_back(std::move(q));
    }
    j["primitives"] = std::move(prims);
    defs[key] = std::move(j);
    return Json(key);
  }

  Json node(const Node& n) {
    return std::visit([this](const auto& v) { return this->value(v); }, n.value);
  }

  Json value(const Shape& s) {
    Json j = Json::object();
    j["type"] = "shape";
    if (!s.id.empty()) j["id"] = s.id;
    j["d"] = path_to_string(s.path);
    if (s.fill.type != Paint::Type::None) j["fill"] = paint(s.fill);
    if (s.fill_rule == FillRule::EvenOdd) j["fill_rule"] = "evenodd";
    if (s.stroke.paint.type != Paint::Type::None) j["stroke"] = stroke(s.stroke);
    if (s.stroke_first) j["stroke_first"] = true;
    return j;
  }

  Json value(const Text& t) {
    Json j = Json::object();
    j["type"] = "text";
    if (!t.id.empty()) j["id"] = t.id;
    j["content"] = t.content;
    j["font"] = Json::object({{"family", Json(t.font.family)},
                              {"size", jnum(t.font.size)},
                              {"weight", Json(t.font.weight)},
                              {"italic", Json(t.font.italic)}});
    j["origin"] = Json::array({jnum(t.origin.x), jnum(t.origin.y)});
    j["advance"] = jnum(t.advance);
    j["d"] = path_to_string(t.outline);
    if (t.fill.type != Paint::Type::None) j["fill"] = paint(t.fill);
    if (t.stroke.paint.type != Paint::Type::None) j["stroke"] = stroke(t.stroke);
    return j;
  }

  Json value(const Picture& p) {
    Json j = Json::object();
    j["type"] = "image";
    if (!p.id.empty()) j["id"] = p.id;
    j["bounds"] = rect_json(p.bounds);
    if (!p.smooth) j["smooth"] = false;
    if (!p.encoded.empty() && !p.encoded_format.empty()) {
      j["format"] = p.encoded_format;
      j["data"] = util::base64_encode(p.encoded);
    }
    if (p.pixels && p.pixels->valid()) {
      j["width"] = Json(p.pixels->width);
      j["height"] = Json(p.pixels->height);
      if (p.encoded.empty()) {
        auto packed = util::zlib_compress(as_bytes(std::span<const std::uint8_t>(p.pixels->rgba)), 6);
        if (packed) j["rgba_zlib"] = util::base64_encode(*packed);
      }
    }
    return j;
  }

  Json value(const Group& g) { return group(g); }

  Json group(const Group& g) {
    Json j = Json::object();
    j["type"] = "group";
    if (!g.id.empty()) j["id"] = g.id;
    if (!g.transform.is_identity(0)) j["transform"] = matrix_json(g.transform);
    if (g.opacity != 1.0) j["opacity"] = jnum(g.opacity);
    if (g.blend != BlendMode::Normal) j["blend"] = blend_name(g.blend);
    if (g.clip) j["clip"] = clip(g.clip);
    if (g.mask) j["mask"] = mask(g.mask);
    if (g.filter) j["filter"] = filter(g.filter);
    if (g.layer) {
      j["layer"] = true;
      j["layer_name"] = g.layer_name;
    }
    if (!g.visible) j["visible"] = false;
    Json children = Json::array();
    for (const Node& n : g.children) children.push_back(node(n));
    j["children"] = std::move(children);
    return j;
  }
};

// ------------------------------------------------------------------ reader

struct Reader {
  const Json* defs = nullptr;
  std::uint64_t max_pixels = 0;
  int max_depth = 256;
  std::uint64_t pixels = 0;
  std::map<std::string, std::shared_ptr<const Gradient>, std::less<>> gradients;
  std::map<std::string, std::shared_ptr<const PatternPaint>, std::less<>> patterns;
  std::map<std::string, std::shared_ptr<const ClipPath>, std::less<>> clips;
  std::map<std::string, std::shared_ptr<const Mask>, std::less<>> masks;
  std::map<std::string, std::shared_ptr<const Filter>, std::less<>> filters;
  std::set<std::string, std::less<>> active;  // definitions being built (cycle guard)

  const Json* def(const Json& ref) {
    if (!ref.is_string() || !defs) return nullptr;
    const Json* d = defs->find(ref.as_string());
    return d && d->is_object() ? d : nullptr;
  }

  Result<std::shared_ptr<const Gradient>> gradient(const Json& ref) {
    if (!ref.is_string()) return fail(Status::Decode, "gradient reference is not a string");
    if (auto it = gradients.find(ref.as_string()); it != gradients.end()) return it->second;
    const Json* d = def(ref);
    if (!d) return fail(Status::Decode, "unknown gradient '{}'", ref.as_string());
    auto g = std::make_shared<Gradient>();
    g->type = get_str(*d, "type") == "radial" ? Gradient::Type::Radial : Gradient::Type::Linear;
    const auto pt = [&](std::string_view key, Point fallback) {
      const Json& a = (*d)[key];
      if (!a.is_array() || a.size() != 2) return fallback;
      return Point{a[0].as_double(fallback.x), a[1].as_double(fallback.y)};
    };
    g->p1 = pt("p1", {0, 0});
    g->p2 = pt("p2", {1, 0});
    g->center = pt("center", {0.5, 0.5});
    g->focal = pt("focal", g->center);
    g->radius = get_num(*d, "radius", 0.5);
    g->focal_radius = get_num(*d, "focal_radius", 0.0);
    for (const Json& s : (*d)["stops"].elements()) {
      if (!s.is_array() || s.size() != 2) continue;
      g->stops.push_back({std::clamp(s[0].as_double(0), 0.0, 1.0), color_from(s[1])});
    }
    const std::string spread = get_str(*d, "spread");
    g->spread = spread == "reflect" ? Spread::Reflect : spread == "repeat" ? Spread::Repeat : Spread::Pad;
    g->transform = matrix_from(d->find("transform"));
    g->linear_rgb = get_bool(*d, "linear_rgb", false);
    gradients.emplace(ref.as_string(), g);
    return std::shared_ptr<const Gradient>(g);
  }

  Result<std::shared_ptr<const PatternPaint>> pattern(const Json& ref, int depth) {
    if (!ref.is_string()) return fail(Status::Decode, "pattern reference is not a string");
    if (auto it = patterns.find(ref.as_string()); it != patterns.end()) return it->second;
    const Json* d = def(ref);
    if (!d) return fail(Status::Decode, "unknown pattern '{}'", ref.as_string());
    if (!active.insert(ref.as_string()).second) return fail(Status::Decode, "pattern '{}' refers to itself", ref.as_string());
    auto p = std::make_shared<PatternPaint>();
    p->tile = rect_from(d->find("tile"));
    p->transform = matrix_from(d->find("transform"));
    if (const Json* c = d->find("content")) {
      CVT_TRY_ASSIGN(Group g, group(*c, depth + 1));
      p->content = std::make_shared<const Group>(std::move(g));
    }
    active.erase(ref.as_string());
    patterns.emplace(ref.as_string(), p);
    return std::shared_ptr<const PatternPaint>(p);
  }

  Result<Paint> paint(const Json* j, int depth) {
    Paint p;
    if (!j || !j->is_object()) return p;
    p.color = color_from((*j)["color"]);
    p.opacity = std::clamp(get_num(*j, "opacity", 1.0), 0.0, 1.0);
    p.type = Paint::Type::Solid;
    if (const Json* g = j->find("gradient")) {
      CVT_TRY_ASSIGN(p.gradient, gradient(*g));
      p.type = Paint::Type::Gradient;
    } else if (const Json* pt = j->find("pattern")) {
      CVT_TRY_ASSIGN(p.pattern, pattern(*pt, depth));
      p.type = Paint::Type::Pattern;
    }
    return p;
  }

  Result<Stroke> stroke(const Json* j, int depth) {
    Stroke s;
    if (!j || !j->is_object()) return s;
    CVT_TRY_ASSIGN(s.paint, paint(j->find("paint"), depth));
    s.width = std::max(0.0, get_num(*j, "width", 1.0));
    const std::string cap = get_str(*j, "cap");
    s.cap = cap == "round" ? LineCap::Round : cap == "square" ? LineCap::Square : LineCap::Butt;
    const std::string join = get_str(*j, "join");
    s.join = join == "round"        ? LineJoin::Round
             : join == "bevel"      ? LineJoin::Bevel
             : join == "miter-clip" ? LineJoin::MiterClip
             : join == "arcs"       ? LineJoin::Arcs
                                    : LineJoin::Miter;
    s.miter_limit = std::max(1.0, get_num(*j, "miter_limit", 4.0));
    for (const Json& v : (*j)["dash"].elements()) s.dash.push_back(std::max(0.0, v.as_double(0)));
    if (s.dash.size() % 2 == 1) s.dash.insert(s.dash.end(), s.dash.begin(), s.dash.end());
    s.dash_offset = get_num(*j, "dash_offset", 0.0);
    s.non_scaling = get_bool(*j, "non_scaling", false);
    s.hairline = get_bool(*j, "hairline", false);
    return s;
  }

  Result<std::shared_ptr<const ClipPath>> clip(const Json& ref, int depth) {
    if (!ref.is_string()) return fail(Status::Decode, "clip reference is not a string");
    if (auto it = clips.find(ref.as_string()); it != clips.end()) return it->second;
    const Json* d = def(ref);
    if (!d) return fail(Status::Decode, "unknown clip '{}'", ref.as_string());
    if (!active.insert(ref.as_string()).second) return fail(Status::Decode, "clip '{}' refers to itself", ref.as_string());
    if (depth > max_depth) return fail(Status::Limit, "clip paths are nested too deeply");
    auto c = std::make_shared<ClipPath>();
    for (const Json& item : (*d)["items"].elements()) {
      ClipPath::Item i;
      CVT_TRY_ASSIGN(i.path, path_from_string(get_str(item, "d")));
      i.rule = get_str(item, "rule") == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
      c->items.push_back(std::move(i));
    }
    if (const Json* inner = d->find("clip")) { CVT_TRY_ASSIGN(c->clip, clip(*inner, depth + 1)); }
    active.erase(ref.as_string());
    clips.emplace(ref.as_string(), c);
    return std::shared_ptr<const ClipPath>(c);
  }

  Result<std::shared_ptr<const Mask>> mask(const Json& ref, int depth) {
    if (!ref.is_string()) return fail(Status::Decode, "mask reference is not a string");
    if (auto it = masks.find(ref.as_string()); it != masks.end()) return it->second;
    const Json* d = def(ref);
    if (!d) return fail(Status::Decode, "unknown mask '{}'", ref.as_string());
    if (!active.insert(ref.as_string()).second) return fail(Status::Decode, "mask '{}' refers to itself", ref.as_string());
    auto m = std::make_shared<Mask>();
    m->type = get_str(*d, "type") == "alpha" ? Mask::Type::Alpha : Mask::Type::Luminance;
    m->region = rect_from(d->find("region"));
    if (const Json* c = d->find("content")) {
      CVT_TRY_ASSIGN(Group g, group(*c, depth + 1));
      m->content = std::make_shared<const Group>(std::move(g));
    }
    active.erase(ref.as_string());
    masks.emplace(ref.as_string(), m);
    return std::shared_ptr<const Mask>(m);
  }

  Result<std::shared_ptr<const Filter>> filter(const Json& ref) {
    if (!ref.is_string()) return fail(Status::Decode, "filter reference is not a string");
    if (auto it = filters.find(ref.as_string()); it != filters.end()) return it->second;
    const Json* d = def(ref);
    if (!d) return fail(Status::Decode, "unknown filter '{}'", ref.as_string());
    auto f = std::make_shared<Filter>();
    f->region = rect_from(d->find("region"));
    for (const Json& q : (*d)["primitives"].elements()) {
      FilterPrimitive p;
      const std::string type = get_str(q, "type");
      bool known = false;
      for (std::size_t i = 0; i < primitive_names.size(); ++i) {
        if (primitive_names[i] == type) {
          p.type = static_cast<FilterPrimitive::Type>(i);
          known = true;
        }
      }
      if (!known) continue;
      p.in = get_str(q, "in");
      p.in2 = get_str(q, "in2");
      p.result = get_str(q, "result");
      for (const Json& m : q["merge"].elements()) {
        if (m.is_string()) p.merge_inputs.push_back(m.as_string());
      }
      if (const Json* sd = q.find("std_dev"); sd && sd->size() == 2) {
        p.std_dev_x = std::max(0.0, (*sd)[0].as_double(0));
        p.std_dev_y = std::max(0.0, (*sd)[1].as_double(0));
      }
      if (const Json* off = q.find("offset"); off && off->size() == 2) {
        p.dx = (*off)[0].as_double(0);
        p.dy = (*off)[1].as_double(0);
      }
      p.color = color_from(q["color"]);
      for (const Json& v : q["matrix"].elements()) p.matrix.push_back(v.as_double(0));
      if (!p.matrix.empty() && p.matrix.size() != 20) p.matrix.clear();
      p.blend = blend_from(get_str(q, "mode"));
      if (const std::string op = get_str(q, "operator"); !op.empty()) p.op = op;
      if (const Json* k = q.find("k"); k && k->size() == 4) {
        p.k1 = (*k)[0].as_double(0);
        p.k2 = (*k)[1].as_double(0);
        p.k3 = (*k)[2].as_double(0);
        p.k4 = (*k)[3].as_double(0);
      }
      p.linear_rgb = get_bool(q, "linear_rgb", true);
      f->primitives.push_back(std::move(p));
    }
    filters.emplace(ref.as_string(), f);
    return std::shared_ptr<const Filter>(f);
  }

  Result<Node> node(const Json& j, int depth) {
    const std::string type = get_str(j, "type");
    if (type == "group") {
      CVT_TRY_ASSIGN(Group g, group(j, depth + 1));
      return Node{std::move(g)};
    }
    if (type == "shape") {
      Shape s;
      s.id = get_str(j, "id");
      CVT_TRY_ASSIGN(s.path, path_from_string(get_str(j, "d")));
      if (const Json* f = j.find("fill"); f && f->is_object()) { CVT_TRY_ASSIGN(s.fill, paint(f, depth)); }
      s.fill_rule = get_str(j, "fill_rule") == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
      if (const Json* st = j.find("stroke")) { CVT_TRY_ASSIGN(s.stroke, stroke(st, depth)); }
      s.stroke_first = get_bool(j, "stroke_first", false);
      return Node{std::move(s)};
    }
    if (type == "text") {
      Text t;
      t.id = get_str(j, "id");
      t.content = util::sanitize_utf8(get_str(j, "content"));
      const Json& font = j["font"];
      if (font.is_object()) {
        if (const std::string family = get_str(font, "family"); !family.empty()) t.font.family = family;
        t.font.size = std::max(0.0, get_num(font, "size", 16));
        t.font.weight = static_cast<int>(std::clamp(get_num(font, "weight", 400), 1.0, 1000.0));
        t.font.italic = get_bool(font, "italic", false);
      }
      const Json& o = j["origin"];
      if (o.is_array() && o.size() == 2) t.origin = {o[0].as_double(0), o[1].as_double(0)};
      t.advance = get_num(j, "advance", 0);
      CVT_TRY_ASSIGN(t.outline, path_from_string(get_str(j, "d")));
      t.fill = Paint{};
      if (const Json* f = j.find("fill"); f && f->is_object()) { CVT_TRY_ASSIGN(t.fill, paint(f, depth)); }
      if (const Json* st = j.find("stroke")) { CVT_TRY_ASSIGN(t.stroke, stroke(st, depth)); }
      return Node{std::move(t)};
    }
    if (type == "image") {
      Picture p;
      p.id = get_str(j, "id");
      p.bounds = rect_from(j.find("bounds"));
      p.smooth = get_bool(j, "smooth", true);
      p.encoded_format = get_str(j, "format");
      if (const std::string data = get_str(j, "data"); !data.empty()) {
        auto bytes = util::base64_decode(data);
        if (!bytes) return fail(Status::Decode, "image data is not valid Base64");
        p.encoded = std::move(*bytes);
      }
      const auto w = static_cast<std::uint64_t>(std::max(0.0, get_num(j, "width", 0)));
      const auto h = static_cast<std::uint64_t>(std::max(0.0, get_num(j, "height", 0)));
      if (const std::string packed = get_str(j, "rgba_zlib"); !packed.empty() && w > 0 && h > 0) {
        if (w > max_pixels / h || pixels + w * h > max_pixels) return fail(Status::Limit, "embedded pictures are too large");
        pixels += w * h;
        auto raw = util::base64_decode(packed);
        if (!raw) return fail(Status::Decode, "image data is not valid Base64");
        CVT_TRY_ASSIGN(Bytes rgba, util::zlib_decompress(*raw, w * h * 4));
        if (rgba.size() != w * h * 4) return fail(Status::Decode, "image pixel data has the wrong size");
        auto frame = std::make_shared<ir::Frame>();
        frame->width = static_cast<std::uint32_t>(w);
        frame->height = static_cast<std::uint32_t>(h);
        frame->rgba.resize(rgba.size());
        std::memcpy(frame->rgba.data(), rgba.data(), rgba.size());
        p.pixels = std::move(frame);
      }
      return Node{std::move(p)};
    }
    return fail(Status::Decode, "unknown scene node type '{}'", type);
  }

  Result<Group> group(const Json& j, int depth) {
    if (depth > max_depth) return fail(Status::Limit, "groups are nested more than {} levels deep", max_depth);
    if (!j.is_object()) return fail(Status::Decode, "a scene group is not an object");
    Group g;
    g.id = get_str(j, "id");
    g.transform = matrix_from(j.find("transform"));
    g.opacity = std::clamp(get_num(j, "opacity", 1.0), 0.0, 1.0);
    g.blend = blend_from(get_str(j, "blend"));
    if (const Json* c = j.find("clip")) { CVT_TRY_ASSIGN(g.clip, clip(*c, depth)); }
    if (const Json* m = j.find("mask")) { CVT_TRY_ASSIGN(g.mask, mask(*m, depth)); }
    if (const Json* f = j.find("filter")) { CVT_TRY_ASSIGN(g.filter, filter(*f)); }
    g.layer = get_bool(j, "layer", false);
    g.layer_name = get_str(j, "layer_name");
    g.visible = get_bool(j, "visible", true);
    for (const Json& child : j["children"].elements()) {
      CVT_TRY_ASSIGN(Node n, node(child, depth));
      g.children.push_back(std::move(n));
    }
    return g;
  }
};

}  // namespace

std::string path_to_string(const Path& path, int precision) {
  std::string out;
  out.reserve(path.points.size() * 12);
  std::size_t i = 0;
  const auto pt = [&](const Point& p) {
    if (!out.empty() && out.back() != ' ' && !(out.back() >= 'A' && out.back() <= 'Z')) out += ' ';
    out += num(p.x, precision);
    out += ' ';
    out += num(p.y, precision);
  };
  for (const Verb verb : path.verbs) {
    switch (verb) {
      case Verb::Move:
        if (i >= path.points.size()) return out;
        out += 'M';
        pt(path.points[i++]);
        break;
      case Verb::Line:
        if (i >= path.points.size()) return out;
        out += 'L';
        pt(path.points[i++]);
        break;
      case Verb::Quad:
        if (i + 2 > path.points.size()) return out;
        out += 'Q';
        pt(path.points[i++]);
        pt(path.points[i++]);
        break;
      case Verb::Cubic:
        if (i + 3 > path.points.size()) return out;
        out += 'C';
        pt(path.points[i++]);
        pt(path.points[i++]);
        pt(path.points[i++]);
        break;
      case Verb::Close: out += 'Z'; break;
    }
  }
  return out;
}

Result<Path> path_from_string(std::string_view text) {
  Path path;
  std::size_t pos = 0;
  const auto skip = [&] {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == ',' || text[pos] == '\n')) ++pos;
  };
  const auto number = [&]() -> Result<double> {
    skip();
    const std::size_t start = pos;
    while (pos < text.size() && (util::is_ascii_digit(text[pos]) || text[pos] == '-' || text[pos] == '+' ||
                                 text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E')) {
      ++pos;
    }
    const auto v = util::parse_double(text.substr(start, pos - start));
    if (!v || !std::isfinite(*v)) return fail(Status::Decode, "malformed number in scene path data");
    return *v;
  };
  const auto point = [&]() -> Result<Point> {
    CVT_TRY_ASSIGN(const double x, number());
    CVT_TRY_ASSIGN(const double y, number());
    return Point{x, y};
  };
  while (true) {
    skip();
    if (pos >= text.size()) break;
    const char c = text[pos++];
    switch (c) {
      case 'M': {
        CVT_TRY_ASSIGN(const Point p, point());
        path.move_to(p);
        break;
      }
      case 'L': {
        if (path.empty()) return fail(Status::Decode, "scene path data does not start with M");
        CVT_TRY_ASSIGN(const Point p, point());
        path.line_to(p);
        break;
      }
      case 'Q': {
        if (path.empty()) return fail(Status::Decode, "scene path data does not start with M");
        CVT_TRY_ASSIGN(const Point a, point());
        CVT_TRY_ASSIGN(const Point b, point());
        path.quad_to(a, b);
        break;
      }
      case 'C': {
        if (path.empty()) return fail(Status::Decode, "scene path data does not start with M");
        CVT_TRY_ASSIGN(const Point a, point());
        CVT_TRY_ASSIGN(const Point b, point());
        CVT_TRY_ASSIGN(const Point e, point());
        path.cubic_to(a, b, e);
        break;
      }
      case 'Z':
        if (path.empty()) return fail(Status::Decode, "scene path data does not start with M");
        path.close();
        break;
      default: return fail(Status::Decode, "unexpected '{}' in scene path data", c);
    }
  }
  return path;
}

Json scene_to_json(const Scene& scene) {
  Writer w;
  Json j = Json::object();
  j[scene_envelope_key] = scene_envelope_version;
  j["units"] = scene.source_units;
  if (!scene.metadata.empty()) {
    Json m = Json::object();
    for (const auto& [k, v] : scene.metadata) m[k] = v;
    j["metadata"] = std::move(m);
  }
  Json pages = Json::array();
  for (const Page& page : scene.pages) {
    Json p = Json::object();
    p["width"] = jnum(page.width);
    p["height"] = jnum(page.height);
    if (!page.name.empty()) p["name"] = page.name;
    p["root"] = w.group(page.root);
    pages.push_back(std::move(p));
  }
  j["defs"] = Json::object();
  j["pages"] = std::move(pages);
  if (scene.mesh) {
    const ir::Mesh& mesh = *scene.mesh;
    Json m = Json::object();
    if (!mesh.name.empty()) m["name"] = mesh.name;
    Json pos = Json::array();
    for (const ir::Vec3& v : mesh.positions) {
      pos.push_back(jnum(v.x));
      pos.push_back(jnum(v.y));
      pos.push_back(jnum(v.z));
    }
    m["positions"] = std::move(pos);
    m["indices"] = Json(mesh.indices);
    if (!mesh.face_sizes.empty()) m["face_sizes"] = Json(mesh.face_sizes);
    if (!mesh.colors.empty()) {
      Json cols = Json::array();
      for (const ir::Color& c : mesh.colors) {
        cols.push_back(jnum(c.r));
        cols.push_back(jnum(c.g));
        cols.push_back(jnum(c.b));
        cols.push_back(jnum(c.a));
      }
      m["colors"] = std::move(cols);
    }
    Json groups = Json::array();
    for (const ir::MeshGroup& g : mesh.groups) {
      groups.push_back(Json::object({{"name", Json(g.name)},
                                     {"first_face", Json(static_cast<std::uint64_t>(g.first_face))},
                                     {"face_count", Json(static_cast<std::uint64_t>(g.face_count))}}));
    }
    if (!mesh.groups.empty()) m["groups"] = std::move(groups);
    j["mesh"] = std::move(m);
  }
  j["defs"] = std::move(w.defs);
  return j;
}

bool is_scene_json(const Json& value) {
  if (!value.is_object()) return false;
  const Json* v = value.find(scene_envelope_key);
  return v && v->is_integer();
}

Result<Scene> scene_from_json(const Json& value, std::uint64_t max_pixels, int max_depth) {
  if (!is_scene_json(value)) {
    return fail(Status::Encode, "the data is not a vector drawing (only vector files read by Convertible can be "
                                "written as vector formats)");
  }
  if (value[scene_envelope_key].as_int64(0) > scene_envelope_version) {
    return fail(Status::Decode, "vector scene version {} is newer than this version understands",
                value[scene_envelope_key].as_int64(0));
  }
  Reader r;
  r.defs = &value["defs"];
  r.max_pixels = max_pixels;
  r.max_depth = max_depth;
  Scene scene;
  scene.source_units = get_str(value, "units");
  if (scene.source_units.empty()) scene.source_units = "px";
  for (const auto& member : value["metadata"].items()) {
    if (member.value().is_string()) scene.metadata[member.key()] = member.value().as_string();
  }
  for (const Json& p : value["pages"].elements()) {
    Page page;
    page.width = std::max(0.0, get_num(p, "width", 0));
    page.height = std::max(0.0, get_num(p, "height", 0));
    page.name = get_str(p, "name");
    if (const Json* root = p.find("root")) { CVT_TRY_ASSIGN(page.root, r.group(*root, 0)); }
    scene.pages.push_back(std::move(page));
  }
  if (const Json* m = value.find("mesh"); m && m->is_object()) {
    ir::Mesh mesh;
    mesh.name = get_str(*m, "name");
    const auto pos = (*m)["positions"].elements();
    for (std::size_t i = 0; i + 2 < pos.size(); i += 3) {
      mesh.positions.push_back({static_cast<float>(pos[i].as_double(0)), static_cast<float>(pos[i + 1].as_double(0)),
                                static_cast<float>(pos[i + 2].as_double(0))});
    }
    for (const Json& v : (*m)["indices"].elements()) mesh.indices.push_back(static_cast<std::uint32_t>(v.as_uint64(0)));
    for (const Json& v : (*m)["face_sizes"].elements()) {
      mesh.face_sizes.push_back(static_cast<std::uint32_t>(v.as_uint64(0)));
    }
    const auto cols = (*m)["colors"].elements();
    for (std::size_t i = 0; i + 3 < cols.size(); i += 4) {
      mesh.colors.push_back({static_cast<float>(cols[i].as_double(1)), static_cast<float>(cols[i + 1].as_double(1)),
                             static_cast<float>(cols[i + 2].as_double(1)), static_cast<float>(cols[i + 3].as_double(1))});
    }
    for (const Json& g : (*m)["groups"].elements()) {
      mesh.groups.push_back({get_str(g, "name"), {}, static_cast<std::size_t>(g["first_face"].as_uint64(0)),
                             static_cast<std::size_t>(g["face_count"].as_uint64(0))});
    }
    if (!mesh.valid()) return fail(Status::Decode, "the 3D mesh of the vector scene is inconsistent");
    scene.mesh = std::move(mesh);
  }
  return scene;
}

}  // namespace cvt::vector
