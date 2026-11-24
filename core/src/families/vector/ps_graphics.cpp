// Convertible core - vector family: PostScript graphics state, paths, painting and output.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include "families/vector/geom.hpp"
#include "families/vector/ps_internal.hpp"

namespace cvt::vector::ps {

namespace {

constexpr double deg = std::numbers::pi / 180.0;

bool is_similarity(const Matrix& m) {
  const double sx = std::hypot(m.a, m.b), sy = std::hypot(m.c, m.d);
  if (sx <= 0 || sy <= 0) return false;
  const double dotp = m.a * m.c + m.b * m.d;
  return std::abs(sx - sy) <= 1e-6 * std::max(sx, sy) && std::abs(dotp) <= 1e-6 * sx * sy;
}

Color clamp_color(double r, double g, double b) {
  const auto c = [](double v) { return static_cast<float>(std::clamp(std::isfinite(v) ? v : 0.0, 0.0, 1.0)); };
  return {c(r), c(g), c(b), 1.0f};
}

Color cmyk_to_rgb(double c, double m, double y, double k) {
  return clamp_color((1 - std::min(1.0, c)) * (1 - std::min(1.0, k)), (1 - std::min(1.0, m)) * (1 - std::min(1.0, k)),
                     (1 - std::min(1.0, y)) * (1 - std::min(1.0, k)));
}

Color hsb_to_rgb(double h, double s, double v) {
  h = std::clamp(h, 0.0, 1.0) * 6.0;
  s = std::clamp(s, 0.0, 1.0);
  v = std::clamp(v, 0.0, 1.0);
  const int i = static_cast<int>(std::floor(h)) % 6;
  const double f = h - std::floor(h);
  const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  switch (i) {
    case 0: return clamp_color(v, t, p);
    case 1: return clamp_color(q, v, p);
    case 2: return clamp_color(p, v, t);
    case 3: return clamp_color(p, q, v);
    case 4: return clamp_color(t, p, v);
    default: return clamp_color(v, p, q);
  }
}

double srgb_gamma(double v) {
  v = std::clamp(v, 0.0, 1.0);
  return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
}

Color lab_to_rgb(double l, double a, double b) {
  const double fy = (l + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
  const auto finv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
  const double x = 0.9642 * finv(fx), y = finv(fy), z = 0.8249 * finv(fz);  // D50
  // Bradford-adapted D50 XYZ -> linear sRGB.
  const double r = 3.1338561 * x - 1.6168667 * y - 0.4906146 * z;
  const double g = -0.9787684 * x + 1.9161415 * y + 0.0334540 * z;
  const double bl = 0.0719453 * x - 0.2289914 * y + 1.4052427 * z;
  return clamp_color(srgb_gamma(r), srgb_gamma(g), srgb_gamma(bl));
}

}  // namespace

// --------------------------------------------------------------------------- output

void Interpreter::start_page() {
  captures_.clear();
  Capture c;
  c.stack.emplace_back();
  captures_.push_back(std::move(c));
  drawn_on_page_ = 0;
  layer_stack_.clear();
  double llx = 0, lly = 0, urx = media_w_, ury = media_h_;
  if (opts_.eps && !opts_.bbox.empty() && opts_.bbox.width() > 0 && opts_.bbox.height() > 0) {
    llx = opts_.bbox.x0;
    lly = opts_.bbox.y0;
    urx = opts_.bbox.x1;
    ury = opts_.bbox.y1;
  } else if (!page_size_set_ && !opts_.bbox.empty() && opts_.bbox.width() > 0 && opts_.bbox.height() > 0) {
    // A PostScript document without a known media size: use its bounding box.
    llx = opts_.bbox.x0;
    lly = opts_.bbox.y0;
    urx = opts_.bbox.x1;
    ury = opts_.bbox.y1;
  }
  media_box_ = Rect{llx, lly, urx, ury};
  page_w_px_ = (urx - llx) * px_per_pt;
  page_h_px_ = (ury - lly) * px_per_pt;
  page_transform_ = Matrix{px_per_pt, 0, 0, -px_per_pt, -llx * px_per_pt, ury * px_per_pt};
}

void Interpreter::page_size_changed(double w, double h) {
  if (!(w > 0 && h > 0 && w < 1e6 && h < 1e6)) return;
  media_w_ = w;
  media_h_ = h;
  page_size_set_ = true;
  if (!opts_.eps && drawn_on_page_ == 0) start_page();
}

Rect Interpreter::page_rect_device() const { return media_box_; }

void Interpreter::close_top(Capture& capture) {
  if (capture.stack.size() <= 1) return;
  OpenGroup top_group = std::move(capture.stack.back());
  capture.stack.pop_back();
  if (top_group.group.children.empty() && !top_group.group.layer) return;
  capture.stack.back().group.children.push_back(Node{std::move(top_group.group)});
}

void Interpreter::sync_clip(Capture& capture) {
  std::vector<const ClipNode*> chain;
  for (const ClipNode* n = gstate_.clip.get(); n != nullptr; n = n->parent.get()) {
    chain.push_back(n);
    if (chain.size() > 10'000) break;
  }
  std::reverse(chain.begin(), chain.end());
  std::size_t k = 0;
  while (k < chain.size() && capture.base + k < capture.stack.size() &&
         capture.stack[capture.base + k].clip_id == chain[k]->id) {
    ++k;
  }
  while (capture.stack.size() > capture.base + k) close_top(capture);
  for (; k < chain.size(); ++k) {
    OpenGroup g;
    g.clip_id = chain[k]->id;
    auto clip = std::make_shared<ClipPath>();
    clip->items.push_back(ClipPath::Item{chain[k]->path, chain[k]->rule});
    g.group.clip = std::move(clip);
    capture.stack.push_back(std::move(g));
  }
}

void Interpreter::emit_node(Node node) {
  if (captures_.empty()) return;
  Capture& c = captures_.back();
  sync_clip(c);
  c.stack.back().group.children.push_back(std::move(node));
  ++drawn_on_page_;
  ++drawn_total_;
  charge(64);
}

void Interpreter::begin_capture() {
  if (captures_.size() >= 32) error("limitcheck", "patterns or forms nested too deeply");
  Capture c;
  c.stack.emplace_back();
  captures_.push_back(std::move(c));
}

Group Interpreter::end_capture() {
  if (captures_.size() <= 1) return {};
  Capture& c = captures_.back();
  while (c.stack.size() > 1) close_top(c);
  Group g = std::move(c.stack.front().group);
  captures_.pop_back();
  return g;
}

void Interpreter::finish_page(bool forced) {
  if (captures_.empty()) return;
  if (forced && drawn_on_page_ == 0 && !pages_.empty()) return;
  if (pages_.size() >= opts_.max_pages) fatal(Status::Limit, "the PostScript document has too many pages");
  while (captures_.size() > 1) end_capture();
  Capture& c = captures_.front();
  while (c.stack.size() > 1) close_top(c);
  Page page;
  page.width = page_w_px_;
  page.height = page_h_px_;
  page.root = std::move(c.stack.front().group);
  page.root.transform = page_transform_;
  pages_.push_back(std::move(page));
  captures_.clear();
}

Paint Interpreter::current_paint() {
  if (gstate_.space.family == Family::Pattern && gstate_.pattern.type == Type::Dict) {
    const Object pat = gstate_.pattern;
    const Object* type = dict_find(pat, "PatternType");
    const Object* mtx = dict_find(pat, ".PatternMatrix");
    Matrix m = mtx != nullptr && mtx->type == Type::Array ? to_matrix(*mtx) : Matrix{};
    if (type != nullptr && type->is_number() && type->number() == 2) {
      if (const Object* sh = dict_find(pat, "Shading")) {
        if (auto p = shading_paint(*sh, m)) return *p;
      }
      return Paint::solid({0.5f, 0.5f, 0.5f, 1});
    }
    const Object* impl = dict_find(pat, ".Impl");
    if (impl != nullptr && impl->type == Type::Integer && impl->ival >= 0 &&
        static_cast<std::size_t>(impl->ival) < pattern_groups_.size()) {
      auto pp = std::make_shared<PatternPaint>();
      const Object* bbox = dict_find(pat, "BBox");
      Rect tile;
      if (bbox != nullptr && bbox->type == Type::Array && bbox->size == 4) {
        const Object x0 = array_get(*bbox, 0), y0 = array_get(*bbox, 1), x1 = array_get(*bbox, 2),
                     y1 = array_get(*bbox, 3);
        if (x0.is_number() && y0.is_number() && x1.is_number() && y1.is_number()) {
          tile = Rect{std::min(x0.number(), x1.number()), std::min(y0.number(), y1.number()),
                      std::max(x0.number(), x1.number()), std::max(y0.number(), y1.number())};
        }
      }
      const Object* xs = dict_find(pat, "XStep");
      const Object* ys = dict_find(pat, "YStep");
      if (xs != nullptr && ys != nullptr && xs->is_number() && ys->is_number() && !tile.empty()) {
        const double sx = std::abs(xs->number()), sy = std::abs(ys->number());
        if (sx > 0 && sy > 0) tile = Rect::xywh(tile.x0, tile.y0, sx, sy);
      }
      if (tile.empty() || tile.width() <= 0 || tile.height() <= 0) return Paint::solid(gstate_.color);
      pp->tile = tile;
      pp->transform = m;
      const Object* paint_type = dict_find(pat, "PaintType");
      std::shared_ptr<const Group> content = pattern_groups_[static_cast<std::size_t>(impl->ival)];
      if (paint_type != nullptr && paint_type->is_number() && paint_type->number() == 2 && content) {
        // Uncoloured pattern: recolour every solid paint with the current colour.
        Group copy = *content;
        const Color col = gstate_.color;
        std::vector<Group*> todo{&copy};
        while (!todo.empty()) {
          Group* g = todo.back();
          todo.pop_back();
          for (Node& n : g->children) {
            if (auto* s = std::get_if<Shape>(&n.value)) {
              if (s->fill.type == Paint::Type::Solid) s->fill.color = col;
              if (s->stroke.paint.type == Paint::Type::Solid) s->stroke.paint.color = col;
            } else if (auto* t = std::get_if<Text>(&n.value)) {
              if (t->fill.type == Paint::Type::Solid) t->fill.color = col;
            } else if (auto* sub = std::get_if<Group>(&n.value)) {
              todo.push_back(sub);
            }
          }
        }
        content = std::make_shared<const Group>(std::move(copy));
      }
      pp->content = std::move(content);
      Paint p;
      p.type = Paint::Type::Pattern;
      p.pattern = std::move(pp);
      return p;
    }
    return Paint::solid(gstate_.color);
  }
  if (gstate_.space.none) return Paint::none();
  return Paint::solid(gstate_.color);
}

void Interpreter::emit_fill_paint(const Path& device_path, FillRule rule, const Paint& paint) {
  if (device_path.empty() || !paint.visible()) return;
  Shape s;
  s.path = device_path;
  s.fill = paint;
  s.fill_rule = rule;
  emit_node(Node{std::move(s)});
}

void Interpreter::emit_fill(const Path& device_path, FillRule rule) {
  if (device_path.empty()) return;
  // Paths with only moves paint nothing.
  bool drawable = false;
  for (const Verb v : device_path.verbs) {
    if (v != Verb::Move) {
      drawable = true;
      break;
    }
  }
  if (!drawable) return;
  emit_fill_paint(device_path, rule, current_paint());
}

Stroke Interpreter::current_stroke(Matrix* group_transform, bool* use_group) {
  Stroke st;
  st.paint = current_paint();
  st.cap = gstate_.cap;
  st.join = gstate_.join;
  st.miter_limit = std::max(1.0, gstate_.miter);
  const Matrix& m = gstate_.ctm;
  double scale = 1.0;
  *use_group = false;
  if (is_similarity(m)) {
    scale = std::hypot(m.a, m.b);
  } else if (m.invertible()) {
    *use_group = true;
    *group_transform = m;
  } else {
    scale = m.mean_scale();
  }
  st.width = gstate_.line_width * scale;
  if (gstate_.line_width <= 0) {
    st.hairline = true;
    st.width = 0.24;  // about one device pixel at 300 dpi
  }
  double sum = 0;
  for (const double d : gstate_.dash) sum += d;
  if (sum > 0) {
    st.dash = gstate_.dash;
    if (st.dash.size() % 2 != 0) {
      const std::vector<double> copy = st.dash;
      st.dash.insert(st.dash.end(), copy.begin(), copy.end());
    }
    for (double& d : st.dash) d *= scale;
    st.dash_offset = gstate_.dash_offset * scale;
  }
  return st;
}

void Interpreter::emit_stroke(const Path& device_path) {
  if (device_path.empty()) return;
  Matrix gm;
  bool use_group = false;
  Stroke st = current_stroke(&gm, &use_group);
  if (!st.paint.visible()) return;
  Shape s;
  s.stroke = std::move(st);
  if (use_group) {
    s.path = transformed(device_path, gm.inverted());
    Group g;
    g.transform = gm;
    g.children.push_back(Node{std::move(s)});
    emit_node(Node{std::move(g)});
  } else {
    s.path = device_path;
    emit_node(Node{std::move(s)});
  }
}

// --------------------------------------------------------------------------- colour

ColorSpace Interpreter::parse_color_space(const Object& desc, int depth) {
  if (depth > 8) error("limitcheck", "colour space nesting too deep");
  ColorSpace cs;
  cs.desc = desc;
  std::string family;
  Object arr;
  if (desc.type == Type::Name || desc.type == Type::String) {
    family = object_string(desc);
  } else if (desc.type == Type::Array && desc.size > 0) {
    arr = desc;
    family = object_string(array_get(desc, 0));
  } else {
    error("typecheck", "bad colour space");
  }
  if (family == "DeviceGray" || family == "CalGray" || family == "CIEBasedA" || family == "G") {
    cs.family = Family::Gray;
    cs.components = 1;
  } else if (family == "DeviceRGB" || family == "CalRGB" || family == "CIEBasedABC" || family == "RGB") {
    cs.family = Family::RGB;
    cs.components = 3;
  } else if (family == "DeviceCMYK" || family == "CIEBasedDEFG" || family == "CMYK") {
    cs.family = Family::CMYK;
    cs.components = 4;
  } else if (family == "CIEBasedDEF") {
    cs.family = Family::RGB;
    cs.components = 3;
  } else if (family == "Lab") {
    cs.family = Family::Lab;
    cs.components = 3;
  } else if (family == "ICCBased") {
    int n = 3;
    if (arr.type == Type::Array && arr.size > 1) {
      const Object d = array_get(arr, 1);
      if (d.type == Type::Dict) {
        if (const Object* nn = dict_find(d, "N"); nn != nullptr && nn->is_number()) n = static_cast<int>(nn->number());
      }
    }
    cs.family = n == 1 ? Family::Gray : (n == 4 ? Family::CMYK : Family::RGB);
    cs.components = n == 1 ? 1 : (n == 4 ? 4 : 3);
  } else if (family == "Pattern") {
    cs.family = Family::Pattern;
    cs.components = 0;
    if (arr.type == Type::Array && arr.size > 1) {
      cs.base = std::make_shared<ColorSpace>(parse_color_space(array_get(arr, 1), depth + 1));
      cs.components = cs.base->components;
    }
  } else if (family == "Indexed" || family == "I") {
    if (arr.type != Type::Array || arr.size < 4) error("rangecheck", "bad Indexed colour space");
    cs.family = Family::Indexed;
    cs.components = 1;
    cs.base = std::make_shared<ColorSpace>(parse_color_space(array_get(arr, 1), depth + 1));
    const Object hival = array_get(arr, 2);
    cs.hival = hival.is_number() ? std::clamp(static_cast<int>(hival.number()), 0, 4095) : 0;
    const Object lookup_obj = array_get(arr, 3);
    if (lookup_obj.type == Type::String) cs.lookup = std::string(string_view(lookup_obj));
    else if (lookup_obj.is_proc()) cs.lookup_proc = lookup_obj;
  } else if (family == "Separation" || family == "DeviceN") {
    if (arr.type != Type::Array || arr.size < 4) error("rangecheck", "bad Separation colour space");
    cs.family = family == "Separation" ? Family::Separation : Family::DeviceN;
    cs.components = 1;
    const Object names = array_get(arr, 1);
    if (cs.family == Family::DeviceN && names.type == Type::Array) cs.components = std::max<int>(1, static_cast<int>(names.size));
    if (cs.family == Family::Separation && object_string(names) == "None") cs.none = true;
    cs.base = std::make_shared<ColorSpace>(parse_color_space(array_get(arr, 2), depth + 1));
    cs.tint = array_get(arr, 3);
  } else if (family == "DevicePixel") {
    cs.family = Family::Gray;
    cs.components = 1;
  } else {
    error("undefined", std::format("unknown colour space {}", family));
  }
  return cs;
}

Color Interpreter::resolve_color(const ColorSpace& space, const std::vector<double>& comps, int depth) {
  const auto at = [&](std::size_t i) { return i < comps.size() ? comps[i] : 0.0; };
  if (depth > 8) return {0, 0, 0, 1};
  switch (space.family) {
    case Family::Gray: return clamp_color(at(0), at(0), at(0));
    case Family::RGB: return clamp_color(at(0), at(1), at(2));
    case Family::CMYK: return cmyk_to_rgb(at(0), at(1), at(2), at(3));
    case Family::Lab: return lab_to_rgb(at(0), at(1), at(2));
    case Family::Indexed: {
      if (!space.base) return {0, 0, 0, 1};
      const int idx = std::clamp(static_cast<int>(std::lround(at(0))), 0, space.hival);
      const int n = space.base->components;
      std::vector<double> base(static_cast<std::size_t>(std::max(n, 1)), 0.0);
      if (!space.lookup.empty()) {
        for (int k = 0; k < n; ++k) {
          const std::size_t pos = static_cast<std::size_t>(idx) * static_cast<std::size_t>(n) + static_cast<std::size_t>(k);
          base[static_cast<std::size_t>(k)] =
              pos < space.lookup.size() ? static_cast<unsigned char>(space.lookup[pos]) / 255.0 : 0.0;
        }
        if (space.base->family == Family::Lab) {
          // Lab lookups use the default ranges.
          base[0] *= 100.0;
          if (n > 2) {
            base[1] = base[1] * 200.0 - 100.0;
            base[2] = base[2] * 200.0 - 100.0;
          }
        }
      } else if (space.lookup_proc.type == Type::Array) {
        const std::size_t before = ostack_.size();
        push_int(idx);
        call(space.lookup_proc);
        for (int k = n - 1; k >= 0; --k) {
          if (ostack_.size() > before && ostack_.back().is_number()) base[static_cast<std::size_t>(k)] = ostack_.back().number();
          if (ostack_.size() > before) ostack_.pop_back();
        }
        if (ostack_.size() > before) ostack_.resize(before);
      }
      return resolve_color(*space.base, base, depth + 1);
    }
    case Family::Separation:
    case Family::DeviceN: {
      if (!space.base) return {0, 0, 0, 1};
      const int n = space.base->components;
      std::vector<double> base(static_cast<std::size_t>(std::max(n, 1)), 0.0);
      if (space.tint.type == Type::Array || space.tint.type == Type::Operator || space.tint.type == Type::Name) {
        const std::size_t before = ostack_.size();
        for (int k = 0; k < space.components; ++k) push_real(at(static_cast<std::size_t>(k)));
        try {
          call(space.tint);
        } catch (const PsError&) {
          ostack_.resize(before);
          return clamp_color(1 - at(0), 1 - at(0), 1 - at(0));
        }
        for (int k = n - 1; k >= 0; --k) {
          if (ostack_.size() > before && ostack_.back().is_number()) base[static_cast<std::size_t>(k)] = ostack_.back().number();
          if (ostack_.size() > before) ostack_.pop_back();
        }
        if (ostack_.size() > before) ostack_.resize(before);
      } else if (space.tint.type == Type::Dict) {
        base = eval_function(space.tint, at(0));
      }
      return resolve_color(*space.base, base, depth + 1);
    }
    case Family::Pattern:
      if (space.base) return resolve_color(*space.base, comps, depth + 1);
      return {0, 0, 0, 1};
  }
  return {0, 0, 0, 1};
}

void Interpreter::update_color() { gstate_.color = resolve_color(gstate_.space, gstate_.comps); }

void Interpreter::set_device_color(Family family, std::vector<double> comps) {
  ColorSpace cs;
  cs.family = family;
  cs.components = static_cast<int>(comps.size());
  cs.desc = make_name(family == Family::Gray ? "DeviceGray" : (family == Family::RGB ? "DeviceRGB" : "DeviceCMYK"));
  gstate_.space = std::move(cs);
  gstate_.comps = std::move(comps);
  gstate_.pattern = Object::null();
  update_color();
}

void Interpreter::set_color_space(const Object& desc) {
  ColorSpace cs = parse_color_space(desc);
  std::vector<double> comps;
  switch (cs.family) {
    case Family::Gray:
    case Family::Indexed: comps = {0.0}; break;
    case Family::RGB:
    case Family::Lab: comps = {0.0, 0.0, 0.0}; break;
    case Family::CMYK: comps = {0.0, 0.0, 0.0, 1.0}; break;
    case Family::Separation:
    case Family::DeviceN: comps.assign(static_cast<std::size_t>(cs.components), 1.0); break;
    case Family::Pattern: break;
  }
  gstate_.space = std::move(cs);
  gstate_.comps = std::move(comps);
  gstate_.pattern = Object::null();
  update_color();
}

void Interpreter::op_setgray() { set_device_color(Family::Gray, {pop_number()}); }
void Interpreter::op_currentgray() {
  const Color c = gstate_.color;
  push_real(0.3 * c.r + 0.59 * c.g + 0.11 * c.b);
}
void Interpreter::op_setrgbcolor() {
  const double b = pop_number(), g = pop_number(), r = pop_number();
  set_device_color(Family::RGB, {r, g, b});
}
void Interpreter::op_currentrgbcolor() {
  push_real(gstate_.color.r);
  push_real(gstate_.color.g);
  push_real(gstate_.color.b);
}
void Interpreter::op_sethsbcolor() {
  const double b = pop_number(), s = pop_number(), h = pop_number();
  const Color c = hsb_to_rgb(h, s, b);
  set_device_color(Family::RGB, {c.r, c.g, c.b});
}
void Interpreter::op_currenthsbcolor() {
  const double r = gstate_.color.r, g = gstate_.color.g, b = gstate_.color.b;
  const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
  double h = 0;
  const double s = mx > 0 ? (mx - mn) / mx : 0;
  if (mx > mn) {
    if (mx == r) h = (g - b) / (mx - mn);
    else if (mx == g) h = 2 + (b - r) / (mx - mn);
    else h = 4 + (r - g) / (mx - mn);
    h /= 6;
    if (h < 0) h += 1;
  }
  push_real(h);
  push_real(s);
  push_real(mx);
}
void Interpreter::op_setcmykcolor() {
  const double k = pop_number(), y = pop_number(), m = pop_number(), c = pop_number();
  set_device_color(Family::CMYK, {c, m, y, k});
}
void Interpreter::op_currentcmykcolor() {
  if (gstate_.space.family == Family::CMYK && gstate_.comps.size() == 4) {
    for (const double v : gstate_.comps) push_real(v);
    return;
  }
  const double r = gstate_.color.r, g = gstate_.color.g, b = gstate_.color.b;
  const double k = 1 - std::max({r, g, b});
  const double d = 1 - k;
  push_real(d > 0 ? (1 - r - k) / d : 0);
  push_real(d > 0 ? (1 - g - k) / d : 0);
  push_real(d > 0 ? (1 - b - k) / d : 0);
  push_real(k);
}
void Interpreter::op_setcolorspace() { set_color_space(pop()); }
void Interpreter::op_currentcolorspace() {
  if (gstate_.space.desc.type == Type::Array) {
    push(gstate_.space.desc);
    return;
  }
  Object arr = make_array(1);
  array_put(arr, 0, gstate_.space.desc.type == Type::Name ? gstate_.space.desc : make_name("DeviceGray"));
  push(arr);
}
void Interpreter::op_setcolor() {
  if (gstate_.space.family == Family::Pattern) {
    need(1);
    const Object pat = pop();
    std::vector<double> comps;
    if (gstate_.space.base) {
      for (int k = 0; k < gstate_.space.base->components; ++k) comps.insert(comps.begin(), pop_number());
      gstate_.color = resolve_color(*gstate_.space.base, comps);
    }
    gstate_.comps = std::move(comps);
    gstate_.pattern = pat.type == Type::Dict ? pat : Object::null();
    return;
  }
  const int n = std::max(1, gstate_.space.components);
  std::vector<double> comps(static_cast<std::size_t>(n));
  for (int k = n - 1; k >= 0; --k) comps[static_cast<std::size_t>(k)] = pop_number();
  gstate_.comps = std::move(comps);
  update_color();
}
void Interpreter::op_currentcolor() {
  for (const double v : gstate_.comps) push_real(v);
  if (gstate_.space.family == Family::Pattern) push(gstate_.pattern);
}
void Interpreter::op_setpattern() {
  const Object pat = pop_dict();
  if (gstate_.space.family != Family::Pattern) {
    ColorSpace cs;
    cs.family = Family::Pattern;
    cs.components = 0;
    cs.desc = make_name("Pattern");
    const Object* paint_type = dict_find(pat, "PaintType");
    if (paint_type != nullptr && paint_type->is_number() && paint_type->number() == 2) {
      // Uncoloured pattern: keep the current colour as the underlying one.
      cs.base = std::make_shared<ColorSpace>(gstate_.space);
    }
    gstate_.space = std::move(cs);
  }
  gstate_.pattern = pat;
}

// --------------------------------------------------------------------------- graphics state

void Interpreter::op_gsave() {
  if (gstack_.size() >= 10'000) error("limitcheck", "gsave nested too deeply");
  gstack_.push_back(SavedGState{gstate_, false});
}
void Interpreter::op_grestore() {
  if (gstack_.empty()) return;
  // A gsave belonging to an active save is never popped by grestore.
  if (!saves_.empty() && gstack_.size() <= saves_.back().gstack_depth) {
    gstate_ = saves_.back().state;
    return;
  }
  gstate_ = std::move(gstack_.back().state);
  gstack_.pop_back();
}
void Interpreter::op_grestoreall() {
  const std::size_t floor = saves_.empty() ? 0 : saves_.back().gstack_depth;
  if (gstack_.size() > floor) {
    gstate_ = gstack_[floor].state;
    gstack_.resize(floor);
  } else if (!saves_.empty()) {
    gstate_ = saves_.back().state;
  }
}
void Interpreter::op_initgraphics() {
  gstate_.ctm = Matrix{};
  gstate_.path.clear();
  gstate_.has_point = false;
  set_device_color(Family::Gray, {0.0});
  gstate_.line_width = 1;
  gstate_.cap = LineCap::Butt;
  gstate_.join = LineJoin::Miter;
  gstate_.miter = 10;
  gstate_.dash.clear();
  gstate_.dash_offset = 0;
  gstate_.clip.reset();
}
void Interpreter::op_gstate() {
  gstate_objects_.push_back(gstate_);
  charge(512);
  Object o;
  o.type = Type::GState;
  o.ref = static_cast<std::uint32_t>(gstate_objects_.size() - 1);
  push(o);
}
void Interpreter::op_currentgstate() {
  const Object g = pop_type(Type::GState);
  if (g.ref < gstate_objects_.size()) gstate_objects_[g.ref] = gstate_;
  push(g);
}
void Interpreter::op_setgstate() {
  const Object g = pop_type(Type::GState);
  if (g.ref < gstate_objects_.size()) gstate_ = gstate_objects_[g.ref];
}
void Interpreter::op_setlinewidth() { gstate_.line_width = std::abs(pop_number()); }
void Interpreter::op_currentlinewidth() { push_real(gstate_.line_width); }
void Interpreter::op_setlinecap() {
  const std::int64_t v = pop_int();
  gstate_.cap = v == 1 ? LineCap::Round : (v == 2 ? LineCap::Square : LineCap::Butt);
}
void Interpreter::op_currentlinecap() {
  push_int(gstate_.cap == LineCap::Round ? 1 : (gstate_.cap == LineCap::Square ? 2 : 0));
}
void Interpreter::op_setlinejoin() {
  const std::int64_t v = pop_int();
  gstate_.join = v == 1 ? LineJoin::Round : (v == 2 ? LineJoin::Bevel : LineJoin::Miter);
}
void Interpreter::op_currentlinejoin() {
  push_int(gstate_.join == LineJoin::Round ? 1 : (gstate_.join == LineJoin::Bevel ? 2 : 0));
}
void Interpreter::op_setmiterlimit() { gstate_.miter = std::max(1.0, pop_number()); }
void Interpreter::op_currentmiterlimit() { push_real(gstate_.miter); }
void Interpreter::op_setdash() {
  const double offset = pop_number();
  const Object arr = pop_array();
  std::vector<double> dash;
  double sum = 0;
  for (std::uint32_t i = 0; i < arr.size && i < 1000; ++i) {
    const Object e = array_get(arr, i);
    if (!e.is_number()) error("typecheck");
    const double v = e.number();
    if (v < 0) error("rangecheck");
    dash.push_back(v);
    sum += v;
  }
  if (!dash.empty() && sum <= 0) dash.clear();
  gstate_.dash = std::move(dash);
  gstate_.dash_offset = offset;
  gstate_.dash_array = arr;
}
void Interpreter::op_currentdash() {
  if (gstate_.dash_array.type == Type::Array) {
    push(gstate_.dash_array);
  } else {
    push(make_array(0));
  }
  push_real(gstate_.dash_offset);
}
void Interpreter::op_setflat() { gstate_.flat = std::clamp(pop_number(), 0.2, 100.0); }
void Interpreter::op_currentflat() { push_real(gstate_.flat); }
void Interpreter::op_setstrokeadjust() { gstate_.stroke_adjust = pop_bool(); }
void Interpreter::op_currentstrokeadjust() { push_bool(gstate_.stroke_adjust); }
void Interpreter::op_setoverprint() { gstate_.overprint = pop_bool(); }
void Interpreter::op_currentoverprint() { push_bool(gstate_.overprint); }
void Interpreter::op_setsmoothness() { gstate_.smoothness = pop_number(); }
void Interpreter::op_currentsmoothness() { push_real(gstate_.smoothness); }
void Interpreter::op_settransfer() { pop(); }
void Interpreter::op_currenttransfer() {
  Object p = make_array(0);
  p.exec = true;
  push(p);
}
void Interpreter::op_sethalftone() { pop(); }
void Interpreter::op_currenthalftone() {
  Object d = make_dict(4);
  dict_put(d, "HalftoneType", Object::integer(1));
  dict_put(d, "Frequency", Object::real(60));
  dict_put(d, "Angle", Object::real(45));
  Object p = make_array(0);
  p.exec = true;
  dict_put(d, "SpotFunction", p);
  push(d);
}
void Interpreter::op_setscreen() {
  pop();
  pop_number();
  pop_number();
}
void Interpreter::op_currentscreen() {
  push_real(60);
  push_real(45);
  Object p = make_array(0);
  p.exec = true;
  push(p);
}
void Interpreter::op_setcolortransfer() {
  need(4);
  for (int k = 0; k < 4; ++k) pop();
}
void Interpreter::op_currentcolortransfer() {
  for (int k = 0; k < 4; ++k) op_currenttransfer();
}
void Interpreter::op_setblackgeneration() { pop(); }
void Interpreter::op_currentblackgeneration() { op_currenttransfer(); }
void Interpreter::op_setundercolorremoval() { pop(); }
void Interpreter::op_currentundercolorremoval() { op_currenttransfer(); }
void Interpreter::op_setcolorrendering() { pop(); }
void Interpreter::op_currentcolorrendering() { push(make_dict(4)); }
void Interpreter::op_sethalftonephase() {
  pop_number();
  pop_number();
}
void Interpreter::op_wtranslation() {
  push_int(0);
  push_int(0);
}
void Interpreter::op_nulldevice() {}
void Interpreter::op_currentdevparams() {
  pop();
  push(make_dict(4));
}

// --------------------------------------------------------------------------- matrices

void Interpreter::op_matrix() {
  Object arr = make_array(6);
  store_matrix(arr, Matrix{});
  push(arr);
}
void Interpreter::op_initmatrix() { gstate_.ctm = Matrix{}; }
void Interpreter::op_identmatrix() {
  const Object arr = pop_array();
  store_matrix(arr, Matrix{});
  push(arr);
}
void Interpreter::op_defaultmatrix() { op_identmatrix(); }
void Interpreter::op_currentmatrix() {
  const Object arr = pop_array();
  store_matrix(arr, gstate_.ctm);
  push(arr);
}
void Interpreter::op_setmatrix() { gstate_.ctm = pop_matrix(); }
void Interpreter::op_translate() {
  need(1);
  if (top().type == Type::Array) {
    const Object arr = pop_array();
    const double ty = pop_number(), tx = pop_number();
    store_matrix(arr, Matrix::translate(tx, ty));
    push(arr);
    return;
  }
  const double ty = pop_number(), tx = pop_number();
  gstate_.ctm = gstate_.ctm * Matrix::translate(tx, ty);
}
void Interpreter::op_scale() {
  need(1);
  if (top().type == Type::Array) {
    const Object arr = pop_array();
    const double sy = pop_number(), sx = pop_number();
    store_matrix(arr, Matrix::scale(sx, sy));
    push(arr);
    return;
  }
  const double sy = pop_number(), sx = pop_number();
  gstate_.ctm = gstate_.ctm * Matrix::scale(sx, sy);
}
void Interpreter::op_rotate() {
  need(1);
  if (top().type == Type::Array) {
    const Object arr = pop_array();
    const double a = pop_number();
    store_matrix(arr, Matrix::rotate(a * deg));
    push(arr);
    return;
  }
  gstate_.ctm = gstate_.ctm * Matrix::rotate(pop_number() * deg);
}
void Interpreter::op_concat() { gstate_.ctm = gstate_.ctm * pop_matrix(); }
void Interpreter::op_concatmatrix() {
  const Object out = pop_array();
  const Matrix m2 = pop_matrix();
  const Matrix m1 = pop_matrix();
  store_matrix(out, m2 * m1);
  push(out);
}
void Interpreter::op_transform() {
  need(1);
  Matrix m = gstate_.ctm;
  if (top().type == Type::Array) m = pop_matrix();
  const double y = pop_number(), x = pop_number();
  const Point p = m.apply({x, y});
  push_real(p.x);
  push_real(p.y);
}
void Interpreter::op_itransform() {
  need(1);
  Matrix m = gstate_.ctm;
  if (top().type == Type::Array) m = pop_matrix();
  if (!m.invertible()) error("undefinedresult");
  const double y = pop_number(), x = pop_number();
  const Point p = m.inverted().apply({x, y});
  push_real(p.x);
  push_real(p.y);
}
void Interpreter::op_dtransform() {
  need(1);
  Matrix m = gstate_.ctm;
  if (top().type == Type::Array) m = pop_matrix();
  const double y = pop_number(), x = pop_number();
  const Point p = m.apply_vector({x, y});
  push_real(p.x);
  push_real(p.y);
}
void Interpreter::op_idtransform() {
  need(1);
  Matrix m = gstate_.ctm;
  if (top().type == Type::Array) m = pop_matrix();
  if (!m.invertible()) error("undefinedresult");
  const double y = pop_number(), x = pop_number();
  const Point p = m.inverted().apply_vector({x, y});
  push_real(p.x);
  push_real(p.y);
}
void Interpreter::op_invertmatrix() {
  const Object out = pop_array();
  const Matrix m = pop_matrix();
  if (!m.invertible()) error("undefinedresult");
  store_matrix(out, m.inverted());
  push(out);
}

// --------------------------------------------------------------------------- path construction

void Interpreter::path_moveto(Point device) {
  if (!std::isfinite(device.x) || !std::isfinite(device.y)) error("undefinedresult");
  Path& p = gstate_.path;
  if (!p.verbs.empty() && p.verbs.back() == Verb::Move) {
    p.points.back() = device;
  } else {
    if (p.verbs.size() > 5'000'000) error("limitcheck", "path too long");
    p.move_to(device);
  }
  gstate_.has_point = true;
  gstate_.point = device;
  gstate_.subpath_start = device;
}

void Interpreter::path_lineto(Point device) {
  if (!gstate_.has_point) error("nocurrentpoint");
  if (!std::isfinite(device.x) || !std::isfinite(device.y)) error("undefinedresult");
  Path& p = gstate_.path;
  if (p.verbs.empty() || p.verbs.back() == Verb::Close) p.move_to(gstate_.point);
  if (p.verbs.size() > 5'000'000) error("limitcheck", "path too long");
  p.line_to(device);
  gstate_.point = device;
}

void Interpreter::path_curveto(Point c1, Point c2, Point pt) {
  if (!gstate_.has_point) error("nocurrentpoint");
  if (!std::isfinite(pt.x) || !std::isfinite(pt.y) || !std::isfinite(c1.x) || !std::isfinite(c2.y)) {
    error("undefinedresult");
  }
  Path& p = gstate_.path;
  if (p.verbs.empty() || p.verbs.back() == Verb::Close) p.move_to(gstate_.point);
  if (p.verbs.size() > 5'000'000) error("limitcheck", "path too long");
  p.cubic_to(c1, c2, pt);
  gstate_.point = pt;
}

void Interpreter::arc_user(double cx, double cy, double r, double a1, double a2, bool ccw) {
  if (!std::isfinite(a1) || !std::isfinite(a2)) error("undefinedresult");
  if (ccw) {
    if (a2 < a1) a2 += std::ceil((a1 - a2) / 360.0) * 360.0;
  } else {
    if (a2 > a1) a2 -= std::ceil((a2 - a1) / 360.0) * 360.0;
  }
  double sweep = a2 - a1;
  sweep = std::clamp(sweep, -1440.0, 1440.0);
  const Matrix& m = gstate_.ctm;
  const Point start = m.apply({cx + r * std::cos(a1 * deg), cy + r * std::sin(a1 * deg)});
  if (gstate_.has_point) path_lineto(start);
  else path_moveto(start);
  const int n = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / 90.0 - 1e-9)));
  const double step = sweep / n * deg;
  const double k = 4.0 / 3.0 * std::tan(step / 4);
  double t = a1 * deg;
  for (int i = 0; i < n; ++i) {
    const double t2 = t + step;
    const double c0 = std::cos(t), s0 = std::sin(t), c1 = std::cos(t2), s1 = std::sin(t2);
    const Point p1{cx + r * (c0 - k * s0), cy + r * (s0 + k * c0)};
    const Point p2{cx + r * (c1 + k * s1), cy + r * (s1 - k * c1)};
    const Point p3{cx + r * c1, cy + r * s1};
    path_curveto(m.apply(p1), m.apply(p2), m.apply(p3));
    t = t2;
  }
}

void Interpreter::op_newpath() {
  gstate_.path.clear();
  gstate_.has_point = false;
}
void Interpreter::op_currentpoint() {
  if (!gstate_.has_point) error("nocurrentpoint");
  if (!gstate_.ctm.invertible()) error("undefinedresult");
  const Point p = gstate_.ctm.inverted().apply(gstate_.point);
  push_real(p.x);
  push_real(p.y);
}
void Interpreter::op_moveto() {
  const double y = pop_number(), x = pop_number();
  path_moveto(gstate_.ctm.apply({x, y}));
}
void Interpreter::op_rmoveto() {
  const double dy = pop_number(), dx = pop_number();
  if (!gstate_.has_point) error("nocurrentpoint");
  path_moveto(gstate_.point + gstate_.ctm.apply_vector({dx, dy}));
}
void Interpreter::op_lineto() {
  const double y = pop_number(), x = pop_number();
  path_lineto(gstate_.ctm.apply({x, y}));
}
void Interpreter::op_rlineto() {
  const double dy = pop_number(), dx = pop_number();
  if (!gstate_.has_point) error("nocurrentpoint");
  path_lineto(gstate_.point + gstate_.ctm.apply_vector({dx, dy}));
}
void Interpreter::op_curveto() {
  const double y3 = pop_number(), x3 = pop_number(), y2 = pop_number(), x2 = pop_number(), y1 = pop_number(),
               x1 = pop_number();
  const Matrix& m = gstate_.ctm;
  path_curveto(m.apply({x1, y1}), m.apply({x2, y2}), m.apply({x3, y3}));
}
void Interpreter::op_rcurveto() {
  const double y3 = pop_number(), x3 = pop_number(), y2 = pop_number(), x2 = pop_number(), y1 = pop_number(),
               x1 = pop_number();
  if (!gstate_.has_point) error("nocurrentpoint");
  const Matrix& m = gstate_.ctm;
  const Point o = gstate_.point;
  path_curveto(o + m.apply_vector({x1, y1}), o + m.apply_vector({x2, y2}), o + m.apply_vector({x3, y3}));
}
void Interpreter::op_arc() {
  const double a2 = pop_number(), a1 = pop_number(), r = pop_number(), y = pop_number(), x = pop_number();
  arc_user(x, y, r, a1, a2, true);
}
void Interpreter::op_arcn() {
  const double a2 = pop_number(), a1 = pop_number(), r = pop_number(), y = pop_number(), x = pop_number();
  arc_user(x, y, r, a1, a2, false);
}
namespace {
struct Tangent {
  bool ok = false;
  Point t1, t2, center;
  double a1 = 0, a2 = 0;
  bool ccw = true;
};
Tangent tangent_arc(Point p0, Point p1, Point p2, double r) {
  Tangent t;
  const Point d1 = p0 - p1, d2 = p2 - p1;
  const double l1 = length(d1), l2 = length(d2);
  if (l1 <= 0 || l2 <= 0 || r <= 0) return t;
  const Point u1 = d1 * (1 / l1), u2 = d2 * (1 / l2);
  const double cosang = std::clamp(dot(u1, u2), -1.0, 1.0);
  const double ang = std::acos(cosang);
  if (ang < 1e-9 || std::abs(ang - std::numbers::pi) < 1e-9) return t;
  const double dist = r / std::tan(ang / 2);
  t.t1 = p1 + u1 * dist;
  t.t2 = p1 + u2 * dist;
  Point bis = u1 + u2;
  const double bl = length(bis);
  if (bl <= 0) return t;
  bis = bis * (1 / bl);
  t.center = p1 + bis * (r / std::sin(ang / 2));
  t.a1 = std::atan2(t.t1.y - t.center.y, t.t1.x - t.center.x) / deg;
  t.a2 = std::atan2(t.t2.y - t.center.y, t.t2.x - t.center.x) / deg;
  t.ccw = cross(d1 * -1.0, d2) < 0 ? false : true;
  // Direction: from p0 towards p1 then turning to p2.
  t.ccw = cross(p1 - p0, p2 - p1) > 0;
  t.ok = true;
  return t;
}
}  // namespace
void Interpreter::op_arct() {
  const double r = pop_number(), y2 = pop_number(), x2 = pop_number(), y1 = pop_number(), x1 = pop_number();
  if (!gstate_.has_point) error("nocurrentpoint");
  if (!gstate_.ctm.invertible()) error("undefinedresult");
  const Point p0 = gstate_.ctm.inverted().apply(gstate_.point);
  const Tangent t = tangent_arc(p0, {x1, y1}, {x2, y2}, r);
  if (!t.ok) {
    path_lineto(gstate_.ctm.apply({x1, y1}));
    return;
  }
  arc_user(t.center.x, t.center.y, r, t.a1, t.a2, t.ccw);
}
void Interpreter::op_arcto() {
  const double r = pop_number(), y2 = pop_number(), x2 = pop_number(), y1 = pop_number(), x1 = pop_number();
  if (!gstate_.has_point) error("nocurrentpoint");
  if (!gstate_.ctm.invertible()) error("undefinedresult");
  const Point p0 = gstate_.ctm.inverted().apply(gstate_.point);
  const Tangent t = tangent_arc(p0, {x1, y1}, {x2, y2}, r);
  if (!t.ok) {
    path_lineto(gstate_.ctm.apply({x1, y1}));
    for (int k = 0; k < 2; ++k) {
      push_real(x1);
      push_real(y1);
    }
    return;
  }
  arc_user(t.center.x, t.center.y, r, t.a1, t.a2, t.ccw);
  push_real(t.t1.x);
  push_real(t.t1.y);
  push_real(t.t2.x);
  push_real(t.t2.y);
}
void Interpreter::op_closepath() {
  Path& p = gstate_.path;
  if (!gstate_.has_point || p.verbs.empty() || p.verbs.back() == Verb::Close) return;
  if (p.verbs.back() == Verb::Move) return;
  p.close();
  gstate_.point = gstate_.subpath_start;
}
void Interpreter::op_flattenpath() {
  const auto lines = flatten(gstate_.path, std::max(0.05, gstate_.flat * 0.25));
  Path out;
  for (const Polyline& pl : lines) {
    if (pl.points.empty()) continue;
    out.move_to(pl.points[0]);
    for (std::size_t i = 1; i < pl.points.size(); ++i) out.line_to(pl.points[i]);
    if (pl.closed) out.close();
  }
  gstate_.path = std::move(out);
}
void Interpreter::op_reversepath() {
  // Reverse each subpath (curves keep their control points, reversed).
  const Path& in = gstate_.path;
  struct Seg {
    Verb verb;
    std::vector<Point> pts;
  };
  Path out;
  std::size_t pi = 0;
  std::size_t i = 0;
  while (i < in.verbs.size()) {
    if (in.verbs[i] != Verb::Move) {
      ++i;
      continue;
    }
    const Point start = in.points[pi++];
    ++i;
    std::vector<Seg> segs;
    std::vector<Point> nodes{start};
    bool closed = false;
    while (i < in.verbs.size() && in.verbs[i] != Verb::Move) {
      const Verb v = in.verbs[i];
      Seg s{v, {}};
      const int n = v == Verb::Line ? 1 : v == Verb::Quad ? 2 : v == Verb::Cubic ? 3 : 0;
      for (int k = 0; k < n && pi < in.points.size(); ++k) s.pts.push_back(in.points[pi++]);
      if (v == Verb::Close) {
        closed = true;
        ++i;
        break;
      }
      nodes.push_back(s.pts.back());
      segs.push_back(std::move(s));
      ++i;
    }
    out.move_to(nodes.back());
    for (std::size_t k = segs.size(); k-- > 0;) {
      const Seg& s = segs[k];
      const Point to = nodes[k];
      if (s.verb == Verb::Line) out.line_to(to);
      else if (s.verb == Verb::Quad) out.quad_to(s.pts[0], to);
      else if (s.verb == Verb::Cubic) out.cubic_to(s.pts[1], s.pts[0], to);
    }
    if (closed) out.close();
  }
  gstate_.path = std::move(out);
}
void Interpreter::op_strokepath() {
  Matrix gm;
  bool use_group = false;
  Stroke st = current_stroke(&gm, &use_group);
  if (use_group) {
    const Path user = transformed(gstate_.path, gm.inverted());
    gstate_.path = transformed(stroke_outline(user, st, 0.05), gm);
  } else {
    gstate_.path = stroke_outline(gstate_.path, st, 0.05);
  }
}
void Interpreter::op_clippath() {
  Path p;
  if (gstate_.clip) {
    p = gstate_.clip->path;
  } else {
    const Rect r = page_rect_device();
    add_rect(p, r.x0, r.y0, r.width(), r.height());
  }
  gstate_.path = p;
  gstate_.has_point = !p.points.empty();
  if (gstate_.has_point) gstate_.point = p.points.back();
}
void Interpreter::op_pathbbox() {
  if (gstate_.path.empty()) error("nocurrentpoint");
  if (!gstate_.ctm.invertible()) error("undefinedresult");
  const Rect r = bounds(gstate_.path, gstate_.ctm.inverted());
  push_real(r.x0);
  push_real(r.y0);
  push_real(r.x1);
  push_real(r.y1);
}
void Interpreter::op_pathforall() {
  const Object close = pop_proc(), curve = pop_proc(), line = pop_proc(), move = pop_proc();
  if (!gstate_.ctm.invertible()) error("undefinedresult");
  const Matrix inv = gstate_.ctm.inverted();
  const Path p = gstate_.path;
  std::size_t pi = 0;
  Point last{};
  for (const Verb v : p.verbs) {
    switch (v) {
      case Verb::Move:
      case Verb::Line: {
        const Point q = inv.apply(p.points[pi++]);
        push_real(q.x);
        push_real(q.y);
        call(v == Verb::Move ? move : line);
        last = p.points[pi - 1];
        break;
      }
      case Verb::Quad: {
        const Point c = p.points[pi], e = p.points[pi + 1];
        pi += 2;
        const Point c1 = last + (c - last) * (2.0 / 3.0), c2 = e + (c - e) * (2.0 / 3.0);
        for (const Point q : {inv.apply(c1), inv.apply(c2), inv.apply(e)}) {
          push_real(q.x);
          push_real(q.y);
        }
        call(curve);
        last = e;
        break;
      }
      case Verb::Cubic: {
        for (int k = 0; k < 3; ++k) {
          const Point q = inv.apply(p.points[pi++]);
          push_real(q.x);
          push_real(q.y);
        }
        call(curve);
        last = p.points[pi - 1];
        break;
      }
      case Verb::Close: call(close); break;
    }
  }
}

// --------------------------------------------------------------------------- clipping and painting

void Interpreter::op_initclip() { gstate_.clip.reset(); }
namespace {
Path closed_copy(const Path& p) { return p; }
}  // namespace
void Interpreter::op_clip() {
  auto node = std::make_shared<ClipNode>();
  node->parent = gstate_.clip;
  node->path = closed_copy(gstate_.path);
  node->rule = FillRule::NonZero;
  node->id = ++clip_counter_;
  gstate_.clip = std::move(node);
}
void Interpreter::op_eoclip() {
  auto node = std::make_shared<ClipNode>();
  node->parent = gstate_.clip;
  node->path = closed_copy(gstate_.path);
  node->rule = FillRule::EvenOdd;
  node->id = ++clip_counter_;
  gstate_.clip = std::move(node);
}
namespace {
std::vector<Rect> rect_operands(Interpreter& in) {
  std::vector<Rect> out;
  in.need(1);
  if (in.top().type == Type::Array) {
    const Object arr = in.pop_array();
    for (std::uint32_t i = 0; i + 3 < arr.size && out.size() < 100000; i += 4) {
      const Object x = in.array_get(arr, i), y = in.array_get(arr, i + 1), w = in.array_get(arr, i + 2),
                   h = in.array_get(arr, i + 3);
      if (!x.is_number() || !y.is_number() || !w.is_number() || !h.is_number()) in.error("typecheck");
      out.push_back(Rect::xywh(x.number(), y.number(), w.number(), h.number()));
    }
    return out;
  }
  const double h = in.pop_number(), w = in.pop_number(), y = in.pop_number(), x = in.pop_number();
  out.push_back(Rect{x, y, x + w, y + h});
  return out;
}
Path rects_path(const std::vector<Rect>& rects, const Matrix& m) {
  Path p;
  for (const Rect& r : rects) {
    // Keep the drawing direction of the operands (w/h may be negative).
    p.move_to(m.apply({r.x0, r.y0}));
    p.line_to(m.apply({r.x1, r.y0}));
    p.line_to(m.apply({r.x1, r.y1}));
    p.line_to(m.apply({r.x0, r.y1}));
    p.close();
  }
  return p;
}
}  // namespace
void Interpreter::op_rectclip() {
  const std::vector<Rect> rects = rect_operands(*this);
  auto node = std::make_shared<ClipNode>();
  node->parent = gstate_.clip;
  node->path = rects_path(rects, gstate_.ctm);
  node->rule = FillRule::NonZero;
  node->id = ++clip_counter_;
  gstate_.clip = std::move(node);
  op_newpath();
}
void Interpreter::op_fill() {
  emit_fill(gstate_.path, FillRule::NonZero);
  op_newpath();
}
void Interpreter::op_eofill() {
  emit_fill(gstate_.path, FillRule::EvenOdd);
  op_newpath();
}
void Interpreter::op_stroke() {
  emit_stroke(gstate_.path);
  op_newpath();
}
void Interpreter::op_rectfill() {
  const std::vector<Rect> rects = rect_operands(*this);
  emit_fill(rects_path(rects, gstate_.ctm), FillRule::NonZero);
}
void Interpreter::op_rectstroke() {
  need(1);
  Matrix extra;
  bool has_matrix = false;
  if (top().type == Type::Array && top().size == 6 && ostack_.size() >= 2 &&
      (ostack_[ostack_.size() - 2].is_number() || ostack_[ostack_.size() - 2].type == Type::Array)) {
    // x y w h matrix  or  numarray matrix
    const Object m = top();
    bool numeric = true;
    for (std::uint32_t i = 0; i < 6; ++i) numeric = numeric && array_get(m, i).is_number();
    if (numeric && (ostack_[ostack_.size() - 2].type == Type::Array || ostack_.size() >= 5)) {
      extra = pop_matrix();
      has_matrix = true;
    }
  }
  const std::vector<Rect> rects = rect_operands(*this);
  if (has_matrix) {
    const Matrix saved = gstate_.ctm;
    gstate_.ctm = gstate_.ctm * extra;
    emit_stroke(rects_path(rects, gstate_.ctm));
    gstate_.ctm = saved;
  } else {
    emit_stroke(rects_path(rects, gstate_.ctm));
  }
}
void Interpreter::op_erasepage() {
  if (captures_.size() == 1) {
    Capture& c = captures_.front();
    c.stack.clear();
    c.stack.emplace_back();
    c.base = 1;
    drawn_on_page_ = 0;
  }
}
void Interpreter::op_showpage() {
  if (in_build_char_ || captures_.size() > 1) return;
  if (opts_.eps) {
    // An EPS file has exactly one page; later output is drawn on it too.
    return;
  }
  finish_page(false);
  start_page();
  op_initgraphics();
}
void Interpreter::op_copypage() {}
void Interpreter::op_setpagedevice() {
  const Object d = pop_dict();
  if (const Object* size = dict_find(d, "PageSize"); size != nullptr && size->type == Type::Array && size->size >= 2) {
    const Object w = array_get(*size, 0), h = array_get(*size, 1);
    if (w.is_number() && h.is_number()) page_size_changed(w.number(), h.number());
  }
  if (!opts_.eps) op_initgraphics();
}
void Interpreter::op_currentpagedevice() {
  Object d = make_dict(8);
  Object size = make_array(2);
  array_put(size, 0, Object::real(media_w_));
  array_put(size, 1, Object::real(media_h_));
  dict_put(d, "PageSize", size);
  Object hw = make_array(2);
  array_put(hw, 0, Object::real(media_w_));
  array_put(hw, 1, Object::real(media_h_));
  dict_put(d, "HWResolution", [&] {
    Object r = make_array(2);
    array_put(r, 0, Object::real(72));
    array_put(r, 1, Object::real(72));
    return r;
  }());
  dict_put(d, "MediaSize", hw);
  dict_put(d, "Orientation", Object::integer(0));
  push(d);
}

// --------------------------------------------------------------------------- user paths

void Interpreter::op_setbbox() {
  need(4);
  for (int k = 0; k < 4; ++k) pop_number();
}
void Interpreter::op_ucache() {}
void Interpreter::op_uappend() {
  const Object up = pop();
  if (up.type != Type::Array) error("typecheck");
  Object p = up;
  p.exec = true;
  call(p);
}
void Interpreter::op_upath() {
  pop_bool();
  Object p = make_array(0);
  p.exec = true;
  push(p);
}
void Interpreter::op_ufill() {
  need(1);
  if (top().type == Type::Array && top().size == 6 && ostack_.size() >= 2 && ostack_[ostack_.size() - 2].type == Type::Array) {
    pop();
  }
  op_gsave();
  op_newpath();
  op_uappend();
  op_fill();
  op_grestore();
}
void Interpreter::op_ueofill() {
  need(1);
  op_gsave();
  op_newpath();
  op_uappend();
  op_eofill();
  op_grestore();
}
void Interpreter::op_ustroke() {
  need(1);
  Matrix extra;
  bool has_matrix = false;
  if (top().type == Type::Array && top().size == 6 && ostack_.size() >= 2 && ostack_[ostack_.size() - 2].type == Type::Array) {
    extra = pop_matrix();
    has_matrix = true;
  }
  op_gsave();
  op_newpath();
  op_uappend();
  if (has_matrix) gstate_.ctm = gstate_.ctm * extra;
  op_stroke();
  op_grestore();
}
void Interpreter::op_infill() {
  need(1);
  if (top().type == Type::Array) pop();
  else {
    pop_number();
    pop_number();
  }
  push_bool(false);
}
void Interpreter::op_instroke() { op_infill(); }
void Interpreter::op_inufill() {
  pop();
  op_infill();
}

// --------------------------------------------------------------------------- forms and patterns

void Interpreter::op_execform() {
  const Object form = pop_dict();
  const Object* proc = dict_find(form, "PaintProc");
  if (proc == nullptr) error("undefined", "PaintProc");
  op_gsave();
  if (const Object* m = dict_find(form, "Matrix"); m != nullptr && m->type == Type::Array) {
    gstate_.ctm = gstate_.ctm * to_matrix(*m);
  }
  if (const Object* bbox = dict_find(form, "BBox"); bbox != nullptr && bbox->type == Type::Array && bbox->size == 4) {
    push(*bbox);
    push_int(0);
    push_int(4);
    op_getinterval();
    op_aload();
    pop();
    const double y1 = pop_number(), x1 = pop_number(), y0 = pop_number(), x0 = pop_number();
    push_real(x0);
    push_real(y0);
    push_real(x1 - x0);
    push_real(y1 - y0);
    op_rectclip();
  }
  op_newpath();
  push(form);
  try {
    call(*proc);
  } catch (...) {
    op_grestore();
    throw;
  }
  op_grestore();
}

void Interpreter::op_makepattern() {
  const Matrix m = pop_matrix();
  const Object pat = pop_dict();
  Object copy = make_dict(dict(pat).entries.size() + 4);
  const auto entries = dict(pat).entries;
  for (const auto& [k, v] : entries) dict_put(copy, k, v);
  const Matrix pm = gstate_.ctm * m;
  Object marr = make_array(6);
  store_matrix(marr, pm);
  dict_put(copy, ".PatternMatrix", marr);
  const Object* type = dict_find(pat, "PatternType");
  const int ptype = type != nullptr && type->is_number() ? static_cast<int>(type->number()) : 1;
  if (ptype == 1) {
    const Object* proc = dict_find(pat, "PaintProc");
    if (proc == nullptr) error("undefined", "PaintProc");
    if (pattern_groups_.size() >= 10'000) error("limitcheck", "too many patterns");
    const GState saved = gstate_;
    const std::vector<SavedGState> saved_stack_size;
    gstate_.ctm = Matrix{};
    gstate_.clip.reset();
    gstate_.path.clear();
    gstate_.has_point = false;
    set_device_color(Family::Gray, {0.0});
    begin_capture();
    push(copy);
    try {
      call(*proc);
    } catch (...) {
      end_capture();
      gstate_ = saved;
      throw;
    }
    Group content = end_capture();
    gstate_ = saved;
    pattern_groups_.push_back(std::make_shared<const Group>(std::move(content)));
    dict_put(copy, ".Impl", Object::integer(static_cast<std::int64_t>(pattern_groups_.size() - 1)));
  }
  push(copy);
}

void Interpreter::op_shfill() {
  const Object sh = pop_dict();
  do_shfill(sh, gstate_.ctm);
}

// --------------------------------------------------------------------------- font cache operators

void Interpreter::op_setcachedevice() {
  need(6);
  for (int k = 0; k < 4; ++k) pop_number();
  const double wy = pop_number(), wx = pop_number();
  build_char_width_ = {wx, wy};
}
void Interpreter::op_setcachedevice2() {
  need(10);
  for (int k = 0; k < 8; ++k) pop_number();
  const double wy = pop_number(), wx = pop_number();
  build_char_width_ = {wx, wy};
}
void Interpreter::op_setcharwidth() {
  const double wy = pop_number(), wx = pop_number();
  build_char_width_ = {wx, wy};
}
void Interpreter::op_setcachelimit() { pop_number(); }
void Interpreter::op_cachestatus() {
  for (int k = 0; k < 7; ++k) push_int(0);
}

// --------------------------------------------------------------------------- Illustrator layers

void Interpreter::op_ai_layer_begin() {
  // visible preview enabled printing dimmed hasMultiLayerMasks? colorIndex red green blue ... Lb
  std::vector<double> args;
  while (!ostack_.empty() && ostack_.back().is_number() && args.size() < 13) args.insert(args.begin(), pop_number());
  Capture& c = captures_.back();
  while (c.stack.size() > c.base) close_top(c);
  OpenGroup g;
  g.group.layer = true;
  g.group.visible = args.empty() || args[0] != 0;
  c.stack.push_back(std::move(g));
  c.base = c.stack.size();
}
void Interpreter::op_ai_layer_name() {
  const Object name = pop();
  Capture& c = captures_.back();
  if (c.base > 1 && c.base <= c.stack.size()) {
    c.stack[c.base - 1].group.layer_name = object_string(name);
  }
}
void Interpreter::op_ai_layer_end() {
  Capture& c = captures_.back();
  if (c.base <= 1) return;
  while (c.stack.size() > c.base) close_top(c);
  --c.base;
  close_top(c);
}

void Interpreter::install_graphics_operators() {
  using I = Interpreter;
  const std::pair<std::string_view, void (I::*)()> ops[] = {
      {"gsave", &I::op_gsave}, {"grestore", &I::op_grestore}, {"grestoreall", &I::op_grestoreall},
      {"initgraphics", &I::op_initgraphics}, {"gstate", &I::op_gstate}, {"currentgstate", &I::op_currentgstate},
      {"setgstate", &I::op_setgstate}, {"setlinewidth", &I::op_setlinewidth},
      {"currentlinewidth", &I::op_currentlinewidth}, {"setlinecap", &I::op_setlinecap},
      {"currentlinecap", &I::op_currentlinecap}, {"setlinejoin", &I::op_setlinejoin},
      {"currentlinejoin", &I::op_currentlinejoin}, {"setmiterlimit", &I::op_setmiterlimit},
      {"currentmiterlimit", &I::op_currentmiterlimit}, {"setdash", &I::op_setdash},
      {"currentdash", &I::op_currentdash}, {"setflat", &I::op_setflat}, {"currentflat", &I::op_currentflat},
      {"setstrokeadjust", &I::op_setstrokeadjust}, {"currentstrokeadjust", &I::op_currentstrokeadjust},
      {"setgray", &I::op_setgray}, {"currentgray", &I::op_currentgray}, {"setrgbcolor", &I::op_setrgbcolor},
      {"currentrgbcolor", &I::op_currentrgbcolor}, {"sethsbcolor", &I::op_sethsbcolor},
      {"currenthsbcolor", &I::op_currenthsbcolor}, {"setcmykcolor", &I::op_setcmykcolor},
      {"currentcmykcolor", &I::op_currentcmykcolor}, {"setcolorspace", &I::op_setcolorspace},
      {"currentcolorspace", &I::op_currentcolorspace}, {"setcolor", &I::op_setcolor},
      {"currentcolor", &I::op_currentcolor}, {"setpattern", &I::op_setpattern}, {"matrix", &I::op_matrix},
      {"initmatrix", &I::op_initmatrix}, {"identmatrix", &I::op_identmatrix},
      {"defaultmatrix", &I::op_defaultmatrix}, {"currentmatrix", &I::op_currentmatrix},
      {"setmatrix", &I::op_setmatrix}, {"translate", &I::op_translate}, {"scale", &I::op_scale},
      {"rotate", &I::op_rotate}, {"concat", &I::op_concat}, {"concatmatrix", &I::op_concatmatrix},
      {"transform", &I::op_transform}, {"itransform", &I::op_itransform}, {"dtransform", &I::op_dtransform},
      {"idtransform", &I::op_idtransform}, {"invertmatrix", &I::op_invertmatrix}, {"newpath", &I::op_newpath},
      {"currentpoint", &I::op_currentpoint}, {"moveto", &I::op_moveto}, {"rmoveto", &I::op_rmoveto},
      {"lineto", &I::op_lineto}, {"rlineto", &I::op_rlineto}, {"arc", &I::op_arc}, {"arcn", &I::op_arcn},
      {"arct", &I::op_arct}, {"arcto", &I::op_arcto}, {"curveto", &I::op_curveto}, {"rcurveto", &I::op_rcurveto},
      {"closepath", &I::op_closepath}, {"flattenpath", &I::op_flattenpath}, {"reversepath", &I::op_reversepath},
      {"strokepath", &I::op_strokepath}, {"clippath", &I::op_clippath}, {"pathbbox", &I::op_pathbbox},
      {"pathforall", &I::op_pathforall}, {"initclip", &I::op_initclip}, {"clip", &I::op_clip},
      {"eoclip", &I::op_eoclip}, {"rectclip", &I::op_rectclip}, {"fill", &I::op_fill}, {"eofill", &I::op_eofill},
      {"stroke", &I::op_stroke}, {"rectfill", &I::op_rectfill}, {"rectstroke", &I::op_rectstroke},
      {"erasepage", &I::op_erasepage}, {"showpage", &I::op_showpage}, {"copypage", &I::op_copypage},
      {"setpagedevice", &I::op_setpagedevice}, {"currentpagedevice", &I::op_currentpagedevice},
      {"ufill", &I::op_ufill}, {"ueofill", &I::op_ueofill}, {"ustroke", &I::op_ustroke},
      {"uappend", &I::op_uappend}, {"upath", &I::op_upath}, {"setbbox", &I::op_setbbox}, {"ucache", &I::op_ucache},
      {"infill", &I::op_infill}, {"ineofill", &I::op_infill}, {"instroke", &I::op_instroke},
      {"inufill", &I::op_inufill}, {"inueofill", &I::op_inufill}, {"inustroke", &I::op_inufill},
      {"setoverprint", &I::op_setoverprint}, {"currentoverprint", &I::op_currentoverprint},
      {"settransfer", &I::op_settransfer}, {"currenttransfer", &I::op_currenttransfer},
      {"sethalftone", &I::op_sethalftone}, {"currenthalftone", &I::op_currenthalftone},
      {"setscreen", &I::op_setscreen}, {"currentscreen", &I::op_currentscreen},
      {"setcolorscreen", &I::op_setcolortransfer}, {"setcolortransfer", &I::op_setcolortransfer},
      {"currentcolortransfer", &I::op_currentcolortransfer}, {"setblackgeneration", &I::op_setblackgeneration},
      {"currentblackgeneration", &I::op_currentblackgeneration},
      {"setundercolorremoval", &I::op_setundercolorremoval},
      {"currentundercolorremoval", &I::op_currentundercolorremoval},
      {"setcolorrendering", &I::op_setcolorrendering}, {"currentcolorrendering", &I::op_currentcolorrendering},
      {"setsmoothness", &I::op_setsmoothness}, {"currentsmoothness", &I::op_currentsmoothness},
      {"execform", &I::op_execform}, {"makepattern", &I::op_makepattern}, {"shfill", &I::op_shfill},
      {"wtranslation", &I::op_wtranslation}, {"sethalftonephase", &I::op_sethalftonephase},
      {"nulldevice", &I::op_nulldevice}, {"currentdevparams", &I::op_currentdevparams},
      {"setcachedevice", &I::op_setcachedevice}, {"setcachedevice2", &I::op_setcachedevice2},
      {"setcharwidth", &I::op_setcharwidth}, {"setcachelimit", &I::op_setcachelimit},
      {"cachestatus", &I::op_cachestatus}, {".ai_layer_begin", &I::op_ai_layer_begin},
      {".ai_layer_name", &I::op_ai_layer_name}, {".ai_layer_end", &I::op_ai_layer_end},
  };
  for (const auto& [name, fn] : ops) define_operator(name, fn);
}

}  // namespace cvt::vector::ps
