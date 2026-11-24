// Convertible core - vector family: SVG 1.1 / SVG 2 reader.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/image/image_api.hpp"
#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/svg.hpp"
#include "families/vector/svg_values.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::vector::svg {

namespace {

using util::XmlAttribute;
using util::XmlDocument;
using util::XmlNode;
using util::XmlParseOptions;
using util::parse_xml;

constexpr int max_depth = 200;
constexpr std::uint64_t max_nodes = 5'000'000;

using Props = std::map<std::string, std::string, std::less<>>;

bool is_inherited(std::string_view p) {
  static constexpr std::string_view names[] = {
      "clip-rule",        "color",          "color-interpolation", "color-interpolation-filters", "direction",
      "fill",             "fill-opacity",   "fill-rule",           "font-family",   "font-size",  "font-style",
      "font-weight",      "font-variant",   "font-stretch",        "image-rendering", "letter-spacing",
      "marker-start",     "marker-mid",     "marker-end",          "paint-order",   "shape-rendering",
      "stroke",           "stroke-dasharray", "stroke-dashoffset", "stroke-linecap", "stroke-linejoin",
      "stroke-miterlimit", "stroke-opacity", "stroke-width",       "text-anchor",   "text-rendering",
      "visibility",       "word-spacing",   "writing-mode",        "white-space",   "dominant-baseline"};
  return std::find(std::begin(names), std::end(names), p) != std::end(names);
}

bool is_presentation_attribute(std::string_view p) {
  if (is_inherited(p)) return true;
  static constexpr std::string_view names[] = {
      "opacity",      "clip-path",  "mask",          "filter",        "display",        "stop-color",
      "stop-opacity", "flood-color", "flood-opacity", "mix-blend-mode", "isolation",     "overflow",
      "baseline-shift", "text-decoration", "lighting-color", "mask-type", "vector-effect", "transform-origin"};
  return std::find(std::begin(names), std::end(names), p) != std::end(names);
}

double clamp01(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 1.0; }

std::optional<double> parse_number_or_percent(std::string_view text) {
  text = util::trim(text);
  bool percent = false;
  if (text.ends_with('%')) {
    percent = true;
    text.remove_suffix(1);
  }
  const auto v = util::parse_double(util::trim(text));
  if (!v || !std::isfinite(*v)) return std::nullopt;
  return percent ? *v / 100 : *v;
}

std::string_view attr(const XmlNode& n, std::string_view name) {
  const XmlAttribute a = n.attribute(name);
  return a ? a.value() : std::string_view{};
}

bool has_attr(const XmlNode& n, std::string_view name) { return static_cast<bool>(n.attribute(name)); }

std::string_view href_of(const XmlNode& n) {
  if (const XmlAttribute a = n.attribute("href")) return a.value();
  for (const XmlAttribute a : n.attributes()) {
    if (a.local_name() == "href") return a.value();
  }
  return {};
}

std::string local_ref(std::string_view href) {
  href = util::trim(href);
  if (href.size() > 1 && href[0] == '#') return std::string(href.substr(1));
  return {};
}

struct Viewport {
  double w = 100, h = 100;
};

struct State {
  Props inh;     // inherited properties (raw values)
  Props local;   // this element's own specified properties
  double font_size = 16;
  Viewport vp;
  Color current_color{0, 0, 0, 1};

  [[nodiscard]] std::string_view get(std::string_view name) const {
    if (is_inherited(name)) {
      const auto it = inh.find(name);
      return it == inh.end() ? std::string_view{} : std::string_view(it->second);
    }
    const auto it = local.find(name);
    return it == local.end() ? std::string_view{} : std::string_view(it->second);
  }
  [[nodiscard]] bool has(std::string_view name) const { return !get(name).empty(); }
};

struct Bbox {
  Rect rect;
  [[nodiscard]] Matrix unit_matrix() const {
    return {rect.width(), 0, 0, rect.height(), rect.x0, rect.y0};
  }
  [[nodiscard]] bool usable() const { return !rect.empty() && rect.width() > 0 && rect.height() > 0; }
};

// Bounding box of a node in its parent's coordinate space (geometry only).
void node_bounds(const Node& node, const Matrix& m, Rect& out) {
  if (const auto* s = std::get_if<Shape>(&node.value)) {
    out.add(bounds(s->path, m));
  } else if (const auto* t = std::get_if<Text>(&node.value)) {
    out.add(bounds(t->outline, m));
  } else if (const auto* p = std::get_if<Picture>(&node.value)) {
    out.add(transform_rect(p->bounds, m));
  } else if (const auto* g = std::get_if<Group>(&node.value)) {
    const Matrix gm = m * g->transform;
    for (const Node& child : g->children) node_bounds(child, gm, out);
  }
}

class Reader {
 public:
  Reader(const Context& ctx, const ReadOptions& options) : ctx_(ctx), options_(options) {}

  Result<Scene> run(ByteView bytes, int nesting);

 private:
  // ------------------------------------------------------------------ setup
  void index(const XmlNode& root) {
    std::vector<XmlNode> stack{root};
    while (!stack.empty()) {
      const XmlNode n = stack.back();
      stack.pop_back();
      if (!n.is_element()) continue;
      std::string_view id = attr(n, "id");
      if (id.empty()) id = attr(n, "xml:id");
      if (!id.empty() && !ids_.contains(id)) ids_.emplace(std::string(id), n);
      if (n.local_name() == "style") {
        const std::string_view type = attr(n, "type");
        if (type.empty() || util::iequals(type, "text/css")) parse_stylesheet(n.text(), rules_, rule_order_);
      }
      std::vector<XmlNode> kids;
      for (const XmlNode c : n.children()) kids.push_back(c);
      for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
    }
  }

  XmlNode lookup(std::string_view id) const {
    const auto it = ids_.find(id);
    return it == ids_.end() ? XmlNode{} : it->second;
  }

  // ------------------------------------------------------------------ CSS cascade
  bool compound_matches(const CssCompound& c, const XmlNode& n) const {
    if (!c.type.empty() && c.type != "*" && c.type != n.local_name()) return false;
    if (!c.id.empty() && attr(n, "id") != c.id) return false;
    if (!c.classes.empty()) {
      const std::string_view cls = attr(n, "class");
      for (const std::string& want : c.classes) {
        bool found = false;
        for (std::string_view word : util::split(cls, ' ', false)) {
          if (util::trim(word) == want) found = true;
        }
        if (!found) return false;
      }
    }
    for (const auto& [name, value] : c.attributes) {
      const XmlAttribute a = n.attribute(name);
      if (!a) return false;
      if (!value.empty() && value != "\x01" && a.value() != value) return false;
      if (value == "\x01" && !a.value().empty()) return false;
    }
    if (c.first_child) {
      for (XmlNode p = n.previous_sibling(); p; p = p.previous_sibling()) {
        if (p.is_element()) return false;
      }
    }
    return true;
  }

  bool selector_matches(const CssSelector& s, const XmlNode& n) const {
    if (!compound_matches(s.parts[0], n)) return false;
    XmlNode current = n;
    for (std::size_t i = 1; i < s.parts.size(); ++i) {
      const char comb = s.combinators[i - 1];
      bool matched = false;
      for (XmlNode p = current.parent(); p && p.is_element(); p = p.parent()) {
        if (compound_matches(s.parts[i], p)) {
          current = p;
          matched = true;
          break;
        }
        if (comb == '>') break;
      }
      if (!matched) return false;
    }
    return true;
  }

  // Specified properties of an element: presentation attributes < CSS < style attribute < !important.
  Props specified(const XmlNode& n) const {
    Props out;
    for (const XmlAttribute a : n.attributes()) {
      const std::string_view name = a.name();
      if (is_presentation_attribute(name)) out[std::string(name)] = std::string(util::trim(a.value()));
    }
    struct Hit {
      int specificity;
      std::size_t order;
      const CssDeclaration* decl;
    };
    std::vector<Hit> hits;
    for (const CssRule& rule : rules_) {
      int best = -1;
      for (const CssSelector& s : rule.selectors) {
        if (s.specificity > best && selector_matches(s, n)) best = s.specificity;
      }
      if (best < 0) continue;
      for (const CssDeclaration& d : rule.declarations) hits.push_back({best, rule.order, &d});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
      return std::tie(a.specificity, a.order) < std::tie(b.specificity, b.order);
    });
    for (const Hit& h : hits) {
      if (!h.decl->important) apply_declaration(out, *h.decl);
    }
    for (const CssDeclaration& d : parse_declarations(attr(n, "style"))) {
      if (!d.important) apply_declaration(out, d);
    }
    for (const Hit& h : hits) {
      if (h.decl->important) apply_declaration(out, *h.decl);
    }
    for (const CssDeclaration& d : parse_declarations(attr(n, "style"))) {
      if (d.important) apply_declaration(out, d);
    }
    return out;
  }

  static void apply_declaration(Props& out, const CssDeclaration& d) {
    if (d.property == "font") {
      // [style] [variant] [weight] size[/line-height] family
      std::vector<std::string_view> words;
      std::string_view v = d.value;
      std::size_t i = 0;
      while (i < v.size()) {
        while (i < v.size() && v[i] == ' ') ++i;
        std::size_t j = i;
        while (j < v.size() && v[j] != ' ') ++j;
        if (j > i) words.push_back(v.substr(i, j - i));
        i = j;
      }
      std::size_t k = 0;
      for (; k < words.size(); ++k) {
        const std::string_view w = words[k];
        if (w == "italic" || w == "oblique") out["font-style"] = std::string(w);
        else if (w == "bold" || w == "bolder" || w == "lighter" || (util::parse_int(w) && w.size() == 3))
          out["font-weight"] = std::string(w);
        else if (w == "normal" || w == "small-caps") continue;
        else break;
      }
      if (k < words.size()) {
        std::string_view size = words[k];
        if (const std::size_t slash = size.find('/'); slash != std::string_view::npos) size = size.substr(0, slash);
        out["font-size"] = std::string(size);
        ++k;
      }
      std::string family;
      for (; k < words.size(); ++k) {
        if (!family.empty()) family += ' ';
        family += words[k];
      }
      if (!family.empty()) out["font-family"] = family;
      return;
    }
    if (d.property == "marker") {
      out["marker-start"] = out["marker-mid"] = out["marker-end"] = d.value;
      return;
    }
    out[d.property] = d.value;
  }

  // Compute an element's state from its parent's.
  State derive(const State& parent, const XmlNode& n) const {
    State s;
    s.inh = parent.inh;
    s.vp = parent.vp;
    s.font_size = parent.font_size;
    s.current_color = parent.current_color;
    const Props spec = specified(n);
    for (const auto& [name, value] : spec) {
      if (value == "inherit") {
        if (!is_inherited(name)) {
          const auto it = parent.local.find(name);
          if (it != parent.local.end()) s.local[name] = it->second;
        }
        continue;
      }
      if (is_inherited(name)) {
        s.inh[name] = value;
      } else {
        s.local[name] = value;
      }
    }
    if (const auto it = spec.find("font-size"); it != spec.end() && it->second != "inherit") {
      const std::string fs = util::to_lower(it->second);
      static const std::map<std::string, double, std::less<>> keywords = {
          {"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18}, {"x-large", 24},
          {"xx-large", 32}};
      if (const auto k = keywords.find(fs); k != keywords.end()) {
        s.font_size = k->second;
      } else if (fs == "larger") {
        s.font_size = parent.font_size * 1.2;
      } else if (fs == "smaller") {
        s.font_size = parent.font_size / 1.2;
      } else if (const auto l = parse_length(fs)) {
        if (l->unit == Unit::Percent) s.font_size = parent.font_size * l->value / 100;
        else if (l->unit == Unit::Em) s.font_size = parent.font_size * l->value;
        else if (l->unit == Unit::Ex) s.font_size = parent.font_size * l->value / 2;
        else s.font_size = to_px(*l, parent, 'o');
      }
      if (!(s.font_size >= 0) || !std::isfinite(s.font_size)) s.font_size = parent.font_size;
      s.inh["font-size"] = std::string(util::format_double(s.font_size));
    }
    if (const auto it = spec.find("color"); it != spec.end()) {
      if (const auto c = parse_color(it->second); c && !c->current) s.current_color = c->color;
    }
    return s;
  }

  // State of an element computed from the root (for referenced content).
  State state_for(const XmlNode& n) const {
    std::vector<XmlNode> chain;
    for (XmlNode p = n; p && p.is_element(); p = p.parent()) chain.push_back(p);
    State s = root_state_;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) s = derive(s, *it);
    return s;
  }

  static double to_px(const Length& l, const State& s, char axis) {
    switch (l.unit) {
      case Unit::None:
      case Unit::Px: return l.value;
      case Unit::Pt: return l.value * 96.0 / 72.0;
      case Unit::Pc: return l.value * 16.0;
      case Unit::Mm: return l.value * 96.0 / 25.4;
      case Unit::Cm: return l.value * 96.0 / 2.54;
      case Unit::In: return l.value * 96.0;
      case Unit::Em: return l.value * s.font_size;
      case Unit::Ex: return l.value * s.font_size / 2;
      case Unit::Rem: return l.value * 16.0;
      case Unit::Percent:
        if (axis == 'x') return l.value / 100 * s.vp.w;
        if (axis == 'y') return l.value / 100 * s.vp.h;
        return l.value / 100 * std::sqrt((s.vp.w * s.vp.w + s.vp.h * s.vp.h) / 2);
    }
    return l.value;
  }

  double length_attr(const XmlNode& n, std::string_view name, const State& s, char axis, double fallback) const {
    const std::string_view v = attr(n, name);
    if (v.empty()) return fallback;
    const auto l = parse_length(v);
    if (!l) return fallback;
    return to_px(*l, s, axis);
  }

  double length_prop(std::string_view value, const State& s, char axis, double fallback) const {
    if (value.empty()) return fallback;
    const auto l = parse_length(value);
    if (!l) return fallback;
    return to_px(*l, s, axis);
  }

  // A length in objectBoundingBox units (fractions; percentages divided by 100).
  static double unit_attr(const XmlNode& n, std::string_view name, double fallback) {
    const std::string_view v = attr(n, name);
    if (v.empty()) return fallback;
    const auto l = parse_length(v);
    if (!l) return fallback;
    return l->unit == Unit::Percent ? l->value / 100 : l->value;
  }

  Matrix transform_of(const XmlNode& n, std::string_view name = "transform") const {
    const std::string_view v = attr(n, name);
    if (v.empty()) return {};
    const auto m = parse_transform(v);
    return m ? *m : Matrix{};
  }

  // ------------------------------------------------------------------ paint
  struct PaintRef {
    bool none = true;
    bool current = false;
    Color color;
    std::string url;
    std::optional<Color> fallback;
    bool fallback_none = false;
  };

  static PaintRef parse_paint(std::string_view value, const Color& current) {
    PaintRef p;
    value = util::trim(value);
    if (value.empty() || value == "none") return p;
    std::string_view rest;
    const std::string url = url_reference(value, &rest);
    if (util::istarts_with(value, "url(")) {
      p.none = false;
      p.url = url;
      if (rest == "none") p.fallback_none = true;
      else if (const auto c = parse_color(rest)) p.fallback = c->current ? current : c->color;
      return p;
    }
    if (value == "context-fill" || value == "context-stroke") return p;
    const auto c = parse_color(value);
    if (!c) {
      p.none = true;
      return p;
    }
    p.none = false;
    p.color = c->current ? current : c->color;
    return p;
  }

  // Attribute lookup through the href chain of gradients / patterns.
  std::string_view chain_attr(const XmlNode& n, std::string_view name) const {
    XmlNode cur = n;
    for (int i = 0; i < 16 && cur; ++i) {
      if (has_attr(cur, name)) return attr(cur, name);
      cur = lookup(local_ref(href_of(cur)));
    }
    return {};
  }

  XmlNode chain_with_children(const XmlNode& n, std::string_view child) const {
    XmlNode cur = n;
    for (int i = 0; i < 16 && cur; ++i) {
      for (const XmlNode c : cur.elements()) {
        if (c.local_name() == child) return cur;
      }
      cur = lookup(local_ref(href_of(cur)));
    }
    return {};
  }

  XmlNode chain_with_any_children(const XmlNode& n) const {
    XmlNode cur = n;
    for (int i = 0; i < 16 && cur; ++i) {
      for (const XmlNode c : cur.elements()) {
        (void)c;
        return cur;
      }
      cur = lookup(local_ref(href_of(cur)));
    }
    return {};
  }

  std::shared_ptr<const Gradient> gradient(const XmlNode& g, const Bbox& bbox, const State& user) {
    auto out = std::make_shared<Gradient>();
    const bool linear = g.local_name() == "linearGradient";
    out->type = linear ? Gradient::Type::Linear : Gradient::Type::Radial;
    const bool obb = chain_attr(g, "gradientUnits") != "userSpaceOnUse";
    if (obb && !bbox.usable()) return nullptr;
    const auto len = [&](std::string_view name, char axis, double def_obb, std::string_view def_user) -> double {
      std::string_view v = chain_attr(g, name);
      if (obb) {
        if (v.empty()) return def_obb;
        const auto l = parse_length(v);
        if (!l) return def_obb;
        return l->unit == Unit::Percent ? l->value / 100 : l->value;
      }
      if (v.empty()) v = def_user;
      const auto l = parse_length(v);
      return l ? to_px(*l, user, axis) : 0.0;
    };
    if (linear) {
      out->p1 = {len("x1", 'x', 0, "0%"), len("y1", 'y', 0, "0%")};
      out->p2 = {len("x2", 'x', 1, "100%"), len("y2", 'y', 0, "0%")};
    } else {
      out->center = {len("cx", 'x', 0.5, "50%"), len("cy", 'y', 0.5, "50%")};
      out->radius = len("r", 'o', 0.5, "50%");
      out->focal = {chain_attr(g, "fx").empty() ? out->center.x : len("fx", 'x', 0.5, "50%"),
                    chain_attr(g, "fy").empty() ? out->center.y : len("fy", 'y', 0.5, "50%")};
      out->focal_radius = chain_attr(g, "fr").empty() ? 0.0 : len("fr", 'o', 0.0, "0%");
      // SVG 1.1: a focal point outside the end circle moves onto it.
      const Point d = out->focal - out->center;
      const double dist = length(d);
      if (dist > out->radius * 0.999 && dist > 0) out->focal = out->center + d * (out->radius * 0.999 / dist);
    }
    const std::string_view spread = chain_attr(g, "spreadMethod");
    out->spread = spread == "reflect" ? Spread::Reflect : spread == "repeat" ? Spread::Repeat : Spread::Pad;
    Matrix gt;
    if (const std::string_view t = chain_attr(g, "gradientTransform"); !t.empty()) {
      if (const auto m = parse_transform(t)) gt = *m;
    }
    out->transform = obb ? bbox.unit_matrix() * gt : gt;
    const State gs = state_for(g);
    out->linear_rgb = gs.get("color-interpolation") == "linearRGB";
    if (const XmlNode holder = chain_with_children(g, "stop")) {
      double last = 0;
      for (const XmlNode stop : holder.elements()) {
        if (stop.local_name() != "stop") continue;
        const State ss = derive(state_for(holder), stop);
        double offset = 0;
        if (const auto o = parse_number_or_percent(attr(stop, "offset"))) offset = clamp01(*o);
        offset = std::max(offset, last);
        last = offset;
        Color c{0, 0, 0, 1};
        if (const std::string_view sc = ss.get("stop-color"); !sc.empty()) {
          if (const auto pc = parse_color(sc)) c = pc->current ? ss.current_color : pc->color;
        }
        if (const std::string_view so = ss.get("stop-opacity"); !so.empty()) {
          if (const auto o = parse_number_or_percent(so)) c.a = static_cast<float>(c.a * clamp01(*o));
        }
        out->stops.push_back({offset, c});
      }
    }
    if (out->stops.empty()) return nullptr;  // no stops: paint as none
    return out;
  }

  Result<std::shared_ptr<const PatternPaint>> pattern(const XmlNode& p, const Bbox& bbox, const State& user,
                                                      int depth) {
    if (!pattern_stack_.insert(p.internal_data()).second) return std::shared_ptr<const PatternPaint>{};
    const auto guard = [&] { pattern_stack_.erase(p.internal_data()); };
    const bool obb = chain_attr(p, "patternUnits") != "userSpaceOnUse";
    const bool content_obb = chain_attr(p, "patternContentUnits") == "objectBoundingBox";
    if ((obb || content_obb) && !bbox.usable()) {
      guard();
      return std::shared_ptr<const PatternPaint>{};
    }
    const auto len = [&](std::string_view name, char axis) -> double {
      const std::string_view v = chain_attr(p, name);
      if (v.empty()) return 0;
      const auto l = parse_length(v);
      if (!l) return 0;
      if (obb) {
        const double f = l->unit == Unit::Percent ? l->value / 100 : l->value;
        return axis == 'x' ? bbox.rect.x0 + f * bbox.rect.width() : axis == 'y' ? bbox.rect.y0 + f * bbox.rect.height()
               : axis == 'w' ? f * bbox.rect.width() : f * bbox.rect.height();
      }
      return to_px(*l, user, axis == 'w' ? 'x' : axis == 'h' ? 'y' : axis);
    };
    const double x = len("x", 'x'), y = len("y", 'y'), w = len("width", 'w'), h = len("height", 'h');
    if (!(w > 0) || !(h > 0)) {
      guard();
      return std::shared_ptr<const PatternPaint>{};
    }
    auto out = std::make_shared<PatternPaint>();
    out->tile = Rect::xywh(x, y, w, h);
    if (const std::string_view t = chain_attr(p, "patternTransform"); !t.empty()) {
      if (const auto m = parse_transform(t)) out->transform = *m;
    }
    Group content;
    const std::vector<double> vb = parse_numbers(chain_attr(p, "viewBox"));
    if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) {
      content.transform =
          view_box_transform(Rect::xywh(vb[0], vb[1], vb[2], vb[3]), parse_aspect_ratio(chain_attr(p, "preserveAspectRatio")), x, y, w, h);
    } else if (content_obb) {
      content.transform = Matrix::translate(x, y) * Matrix{bbox.rect.width(), 0, 0, bbox.rect.height(), 0, 0};
    } else {
      content.transform = Matrix::translate(x, y);
    }
    if (const XmlNode holder = chain_with_any_children(p)) {
      State s = state_for(holder);
      for (const XmlNode c : holder.elements()) {
        auto node = element(c, s, depth + 1);
        if (!node) {
          guard();
          return std::unexpected(node.error());
        }
        if (*node) content.children.push_back(std::move(**node));
      }
    }
    out->content = std::make_shared<const Group>(std::move(content));
    guard();
    return std::shared_ptr<const PatternPaint>(out);
  }

  Result<Paint> resolve_paint(std::string_view value, std::string_view opacity_value, const State& s,
                              const std::function<Bbox()>& bbox, int depth) {
    const PaintRef ref = parse_paint(value, s.current_color);
    Paint paint;
    if (ref.none) return paint;
    double opacity = 1.0;
    if (!opacity_value.empty()) {
      if (const auto o = parse_number_or_percent(opacity_value)) opacity = clamp01(*o);
    }
    if (ref.url.empty() && util::istarts_with(util::trim(value), "url(")) {
      if (ref.fallback) return Paint::solid(*ref.fallback, opacity);
      return paint;
    }
    if (!ref.url.empty()) {
      const XmlNode target = lookup(ref.url);
      const std::string_view kind = target ? target.local_name() : std::string_view{};
      if (kind == "linearGradient" || kind == "radialGradient") {
        auto g = gradient(target, bbox(), s);
        if (!g) return paint;
        if (g->stops.size() == 1) return Paint::solid(g->stops.front().color, opacity);
        paint.type = Paint::Type::Gradient;
        paint.gradient = std::move(g);
        paint.opacity = opacity;
        paint.color = paint.gradient->stops.front().color;
        return paint;
      }
      if (kind == "pattern") {
        CVT_TRY_ASSIGN(auto p, pattern(target, bbox(), s, depth));
        if (!p) return paint;
        paint.type = Paint::Type::Pattern;
        paint.pattern = std::move(p);
        paint.opacity = opacity;
        return paint;
      }
      if (ref.fallback) return Paint::solid(*ref.fallback, opacity);
      return paint;
    }
    return Paint::solid(ref.color, opacity);
  }

  // ------------------------------------------------------------------ strokes
  Result<Stroke> stroke_of(const State& s, const std::function<Bbox()>& bbox, int depth) {
    Stroke st;
    CVT_TRY_ASSIGN(st.paint, resolve_paint(s.get("stroke"), s.get("stroke-opacity"), s, bbox, depth));
    st.width = std::max(0.0, length_prop(s.get("stroke-width"), s, 'o', 1.0));
    const std::string_view cap = s.get("stroke-linecap");
    st.cap = cap == "round" ? LineCap::Round : cap == "square" ? LineCap::Square : LineCap::Butt;
    const std::string_view join = s.get("stroke-linejoin");
    st.join = join == "round"        ? LineJoin::Round
              : join == "bevel"      ? LineJoin::Bevel
              : join == "miter-clip" ? LineJoin::MiterClip
              : join == "arcs"       ? LineJoin::Arcs
                                     : LineJoin::Miter;
    if (const auto ml = util::parse_double(util::trim(s.get("stroke-miterlimit"))); ml && *ml >= 1) st.miter_limit = *ml;
    const std::string_view dash = s.get("stroke-dasharray");
    if (!dash.empty() && dash != "none") {
      double total = 0;
      bool bad = false;
      for (const Length& l : parse_lengths(dash)) {
        const double v = to_px(l, s, 'o');
        if (v < 0 || !std::isfinite(v)) bad = true;
        st.dash.push_back(v);
        total += v;
      }
      if (bad || !(total > 0)) st.dash.clear();
      if (st.dash.size() % 2 == 1) st.dash.insert(st.dash.end(), st.dash.begin(), st.dash.end());
      st.dash_offset = length_prop(s.get("stroke-dashoffset"), s, 'o', 0.0);
    }
    st.non_scaling = s.get("vector-effect") == "non-scaling-stroke";
    if (!(st.width > 0)) st.paint = Paint{};
    return st;
  }

  // ------------------------------------------------------------------ group features
  Result<std::shared_ptr<const ClipPath>> clip_path(const XmlNode& cp, const Bbox& bbox, int depth) {
    if (depth > max_depth) return fail(Status::Limit, "SVG clip paths are nested too deeply");
    if (!clip_stack_.insert(cp.internal_data()).second) return std::shared_ptr<const ClipPath>{};
    auto out = std::make_shared<ClipPath>();
    Matrix m = transform_of(cp);
    if (attr(cp, "clipPathUnits") == "objectBoundingBox") {
      if (!bbox.usable()) {
        clip_stack_.erase(cp.internal_data());
        // An empty clip region hides everything.
        return std::shared_ptr<const ClipPath>(out);
      }
      m = m * bbox.unit_matrix();
    }
    const State base = state_for(cp);
    std::vector<std::pair<XmlNode, Matrix>> items;
    for (const XmlNode c : cp.elements()) items.emplace_back(c, m);
    for (std::size_t i = 0; i < items.size() && i < 100000; ++i) {
      const auto [c, cm] = items[i];
      const State cs = derive(c.parent() == cp ? base : state_for(c.parent()), c);
      if (cs.get("display") == "none" || cs.get("visibility") == "hidden") continue;
      const Matrix child_m = cm * transform_of(c);
      const std::string_view name = c.local_name();
      if (name == "use") {
        const XmlNode target = lookup(local_ref(href_of(c)));
        if (target && items.size() < 10000) {
          const Matrix um = child_m * Matrix::translate(length_attr(c, "x", cs, 'x', 0), length_attr(c, "y", cs, 'y', 0));
          items.emplace_back(target, um);
        }
        continue;
      }
      Path path;
      if (name == "text") {
        CVT_TRY_ASSIGN(auto node, element(c, state_for(c.parent()), depth + 1));
        if (node) {
          collect_outlines(*node, {}, path);
        }
        // `element` already applied the text's transform attribute.
        ClipPath::Item item;
        item.path = transformed(path, cm);
        item.rule = FillRule::NonZero;
        out->items.push_back(std::move(item));
        continue;
      }
      path = shape_path(c, cs);
      if (path.empty()) continue;
      ClipPath::Item item;
      item.path = transformed(path, child_m);
      item.rule = cs.get("clip-rule") == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
      out->items.push_back(std::move(item));
    }
    // A clip-path on the clipPath element intersects.
    if (const std::string inner = url_reference(specified(cp)["clip-path"]); !inner.empty()) {
      if (const XmlNode next = lookup(inner); next && next.local_name() == "clipPath") {
        CVT_TRY_ASSIGN(auto nested, clip_path(next, bbox, depth + 1));
        if (nested) {
          // The nested clip is in the clipPath's user space (with its transform).
          out->clip = nested;
        }
      }
    }
    clip_stack_.erase(cp.internal_data());
    return std::shared_ptr<const ClipPath>(out);
  }

  static void collect_outlines(const Node& node, const Matrix& m, Path& out) {
    if (const auto* t = std::get_if<Text>(&node.value)) {
      append(out, t->outline, m);
    } else if (const auto* s = std::get_if<Shape>(&node.value)) {
      append(out, s->path, m);
    } else if (const auto* g = std::get_if<Group>(&node.value)) {
      for (const Node& c : g->children) collect_outlines(c, m * g->transform, out);
    }
  }

  Result<std::shared_ptr<const Mask>> mask(const XmlNode& mk, const Bbox& bbox, int depth) {
    if (!mask_stack_.insert(mk.internal_data()).second) return std::shared_ptr<const Mask>{};
    auto out = std::make_shared<Mask>();
    const bool obb = attr(mk, "maskUnits") != "userSpaceOnUse";
    const bool content_obb = attr(mk, "maskContentUnits") == "objectBoundingBox";
    const State ms = state_for(mk);
    if (obb) {
      if (!bbox.usable()) {
        mask_stack_.erase(mk.internal_data());
        out->region = Rect::xywh(0, 0, 0, 0);
        return std::shared_ptr<const Mask>(out);
      }
      const double x = unit_attr(mk, "x", -0.1), y = unit_attr(mk, "y", -0.1);
      const double w = unit_attr(mk, "width", 1.2), h = unit_attr(mk, "height", 1.2);
      out->region = Rect::xywh(bbox.rect.x0 + x * bbox.rect.width(), bbox.rect.y0 + y * bbox.rect.height(),
                               w * bbox.rect.width(), h * bbox.rect.height());
    } else {
      out->region = Rect::xywh(length_attr(mk, "x", ms, 'x', -0.1 * ms.vp.w), length_attr(mk, "y", ms, 'y', -0.1 * ms.vp.h),
                               length_attr(mk, "width", ms, 'x', 1.2 * ms.vp.w),
                               length_attr(mk, "height", ms, 'y', 1.2 * ms.vp.h));
    }
    const std::string_view type = ms.get("mask-type");
    out->type = type == "alpha" ? Mask::Type::Alpha : Mask::Type::Luminance;
    Group content;
    if (content_obb) {
      if (!bbox.usable()) {
        mask_stack_.erase(mk.internal_data());
        return std::shared_ptr<const Mask>(out);
      }
      content.transform = bbox.unit_matrix();
    }
    for (const XmlNode c : mk.elements()) {
      auto node = element(c, ms, depth + 1);
      if (!node) {
        mask_stack_.erase(mk.internal_data());
        return std::unexpected(node.error());
      }
      if (*node) content.children.push_back(std::move(**node));
    }
    if (const std::string inner = url_reference(ms.get("mask")); !inner.empty()) {
      if (const XmlNode next = lookup(inner); next && next.local_name() == "mask") {
        CVT_TRY_ASSIGN(auto nested, mask(next, bbox, depth + 1));
        content.mask = nested;
      }
    }
    out->content = std::make_shared<const Group>(std::move(content));
    mask_stack_.erase(mk.internal_data());
    return std::shared_ptr<const Mask>(out);
  }

  std::shared_ptr<const Filter> filter(const XmlNode& f, const Bbox& bbox) {
    auto out = std::make_shared<Filter>();
    const bool obb = attr(f, "filterUnits") != "userSpaceOnUse";
    const bool prim_obb = attr(f, "primitiveUnits") == "objectBoundingBox";
    const State fs = state_for(f);
    if (obb) {
      if (!bbox.usable()) return nullptr;
      const double x = unit_attr(f, "x", -0.1), y = unit_attr(f, "y", -0.1);
      const double w = unit_attr(f, "width", 1.2), h = unit_attr(f, "height", 1.2);
      out->region = Rect::xywh(bbox.rect.x0 + x * bbox.rect.width(), bbox.rect.y0 + y * bbox.rect.height(),
                               w * bbox.rect.width(), h * bbox.rect.height());
    } else {
      out->region = Rect::xywh(length_attr(f, "x", fs, 'x', -0.1 * fs.vp.w), length_attr(f, "y", fs, 'y', -0.1 * fs.vp.h),
                               length_attr(f, "width", fs, 'x', 1.2 * fs.vp.w),
                               length_attr(f, "height", fs, 'y', 1.2 * fs.vp.h));
    }
    const double sx = prim_obb && bbox.usable() ? bbox.rect.width() : 1.0;
    const double sy = prim_obb && bbox.usable() ? bbox.rect.height() : 1.0;
    const bool linear_default = fs.get("color-interpolation-filters") != "sRGB";
    for (const XmlNode p : f.elements()) {
      const State ps = derive(fs, p);
      FilterPrimitive prim;
      prim.in = std::string(attr(p, "in"));
      prim.in2 = std::string(attr(p, "in2"));
      prim.result = std::string(attr(p, "result"));
      const std::string_view cif = ps.get("color-interpolation-filters");
      prim.linear_rgb = cif.empty() || cif == "auto" ? linear_default : cif == "linearRGB";
      const std::string_view name = p.local_name();
      const auto flood = [&] {
        Color c{0, 0, 0, 1};
        if (const auto fc = parse_color(ps.get("flood-color"))) c = fc->current ? ps.current_color : fc->color;
        if (const auto fo = parse_number_or_percent(ps.get("flood-opacity"))) c.a = static_cast<float>(c.a * clamp01(*fo));
        return c;
      };
      const auto std_dev = [&](double def) {
        const std::vector<double> v = parse_numbers(attr(p, "stdDeviation"));
        const double a = v.empty() ? def : v[0];
        const double b = v.size() > 1 ? v[1] : a;
        prim.std_dev_x = std::max(0.0, a) * sx;
        prim.std_dev_y = std::max(0.0, b) * sy;
      };
      if (name == "feGaussianBlur") {
        prim.type = FilterPrimitive::Type::GaussianBlur;
        std_dev(0);
      } else if (name == "feOffset") {
        prim.type = FilterPrimitive::Type::Offset;
        prim.dx = util::parse_double(attr(p, "dx")).value_or(0) * sx;
        prim.dy = util::parse_double(attr(p, "dy")).value_or(0) * sy;
      } else if (name == "feFlood") {
        prim.type = FilterPrimitive::Type::Flood;
        prim.color = flood();
      } else if (name == "feMerge") {
        prim.type = FilterPrimitive::Type::Merge;
        for (const XmlNode m : p.elements()) {
          if (m.local_name() == "feMergeNode") prim.merge_inputs.emplace_back(attr(m, "in"));
        }
      } else if (name == "feDropShadow") {
        prim.type = FilterPrimitive::Type::DropShadow;
        std_dev(2);
        prim.dx = (has_attr(p, "dx") ? util::parse_double(attr(p, "dx")).value_or(2) : 2) * sx;
        prim.dy = (has_attr(p, "dy") ? util::parse_double(attr(p, "dy")).value_or(2) : 2) * sy;
        prim.color = flood();
      } else if (name == "feColorMatrix") {
        prim.type = FilterPrimitive::Type::ColorMatrix;
        const std::string_view type = attr(p, "type");
        const std::vector<double> v = parse_numbers(attr(p, "values"));
        if (type == "saturate") {
          const double s = v.empty() ? 1 : v[0];
          prim.matrix = {0.213 + 0.787 * s, 0.715 - 0.715 * s, 0.072 - 0.072 * s, 0, 0,
                         0.213 - 0.213 * s, 0.715 + 0.285 * s, 0.072 - 0.072 * s, 0, 0,
                         0.213 - 0.213 * s, 0.715 - 0.715 * s, 0.072 + 0.928 * s, 0, 0,
                         0, 0, 0, 1, 0};
        } else if (type == "hueRotate") {
          const double a = (v.empty() ? 0 : v[0]) * std::numbers::pi / 180;
          const double c = std::cos(a), s = std::sin(a);
          prim.matrix = {0.213 + c * 0.787 - s * 0.213, 0.715 - c * 0.715 - s * 0.715, 0.072 - c * 0.072 + s * 0.928, 0, 0,
                         0.213 - c * 0.213 + s * 0.143, 0.715 + c * 0.285 + s * 0.140, 0.072 - c * 0.072 - s * 0.283, 0, 0,
                         0.213 - c * 0.213 - s * 0.787, 0.715 - c * 0.715 + s * 0.715, 0.072 + c * 0.928 + s * 0.072, 0, 0,
                         0, 0, 0, 1, 0};
        } else if (type == "luminanceToAlpha") {
          prim.matrix = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.2125, 0.7154, 0.0721, 0, 0};
        } else if (v.size() == 20) {
          prim.matrix = v;
        } else {
          prim.matrix = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
        }
      } else if (name == "feBlend") {
        prim.type = FilterPrimitive::Type::Blend;
        const std::string_view mode = attr(p, "mode");
        static const std::map<std::string_view, BlendMode> modes = {
            {"multiply", BlendMode::Multiply}, {"screen", BlendMode::Screen}, {"darken", BlendMode::Darken},
            {"lighten", BlendMode::Lighten},   {"overlay", BlendMode::Overlay}, {"difference", BlendMode::Difference},
            {"exclusion", BlendMode::Exclusion}, {"color-dodge", BlendMode::ColorDodge},
            {"color-burn", BlendMode::ColorBurn}, {"hard-light", BlendMode::HardLight},
            {"soft-light", BlendMode::SoftLight}, {"hue", BlendMode::Hue}, {"saturation", BlendMode::Saturation},
            {"color", BlendMode::Color}, {"luminosity", BlendMode::Luminosity}};
        const auto it = modes.find(mode);
        prim.blend = it == modes.end() ? BlendMode::Normal : it->second;
      } else if (name == "feComposite") {
        prim.type = FilterPrimitive::Type::Composite;
        const std::string_view op = attr(p, "operator");
        prim.op = op.empty() ? "over" : std::string(op);
        prim.k1 = util::parse_double(attr(p, "k1")).value_or(0);
        prim.k2 = util::parse_double(attr(p, "k2")).value_or(0);
        prim.k3 = util::parse_double(attr(p, "k3")).value_or(0);
        prim.k4 = util::parse_double(attr(p, "k4")).value_or(0);
      } else {
        ctx_.warn(std::format("The SVG filter primitive <{}> is not supported and was skipped.", name));
        // Pass the input through under the primitive's result name.
        prim.type = FilterPrimitive::Type::Merge;
        prim.merge_inputs = {prim.in};
      }
      out->primitives.push_back(std::move(prim));
    }
    if (out->primitives.empty()) return nullptr;
    return out;
  }

  // ------------------------------------------------------------------ shapes
  Path shape_path(const XmlNode& n, const State& s) const {
    const std::string_view name = n.local_name();
    Path p;
    if (name == "path") {
      bool error = false;
      p = parse_path_data(attr(n, "d"), &error);
      if (error && !path_warned_) {
        path_warned_ = true;
        ctx_.warn("Some SVG path data was malformed; drawing stopped at the first error as SVG specifies.");
      }
    } else if (name == "rect") {
      const double x = length_attr(n, "x", s, 'x', 0), y = length_attr(n, "y", s, 'y', 0);
      const double w = length_attr(n, "width", s, 'x', 0), h = length_attr(n, "height", s, 'y', 0);
      double rx = length_attr(n, "rx", s, 'x', -1), ry = length_attr(n, "ry", s, 'y', -1);
      if (rx < 0 && ry >= 0) rx = ry;
      if (ry < 0 && rx >= 0) ry = rx;
      add_rect(p, x, y, w, h, std::max(rx, 0.0), std::max(ry, 0.0));
    } else if (name == "circle") {
      const double r = length_attr(n, "r", s, 'o', 0);
      add_ellipse(p, length_attr(n, "cx", s, 'x', 0), length_attr(n, "cy", s, 'y', 0), r, r);
    } else if (name == "ellipse") {
      double rx = length_attr(n, "rx", s, 'x', -1), ry = length_attr(n, "ry", s, 'y', -1);
      if (rx < 0) rx = ry;
      if (ry < 0) ry = rx;
      add_ellipse(p, length_attr(n, "cx", s, 'x', 0), length_attr(n, "cy", s, 'y', 0), rx, ry);
    } else if (name == "line") {
      p.move_to({length_attr(n, "x1", s, 'x', 0), length_attr(n, "y1", s, 'y', 0)});
      p.line_to({length_attr(n, "x2", s, 'x', 0), length_attr(n, "y2", s, 'y', 0)});
    } else if (name == "polyline" || name == "polygon") {
      const std::vector<double> v = parse_numbers(attr(n, "points"));
      for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
        if (i == 0) p.move_to({v[i], v[i + 1]});
        else p.line_to({v[i], v[i + 1]});
      }
      if (name == "polygon" && !p.empty()) p.close();
    }
    return p;
  }

  // ------------------------------------------------------------------ markers
  struct Vertex {
    Point p;
    Point in, out;  // directions (may be zero)
  };

  static std::vector<Vertex> path_vertices(const Path& path) {
    std::vector<Vertex> out;
    std::size_t i = 0;
    Point cur{}, start{};
    std::size_t start_index = 0;
    for (const Verb v : path.verbs) {
      switch (v) {
        case Verb::Move:
          cur = start = path.points[i++];
          start_index = out.size();
          out.push_back({cur, {}, {}});
          break;
        case Verb::Line: {
          const Point e = path.points[i++];
          if (!out.empty()) out.back().out = e - cur;
          out.push_back({e, e - cur, {}});
          cur = e;
          break;
        }
        case Verb::Quad: {
          const Point c = path.points[i], e = path.points[i + 1];
          i += 2;
          if (!out.empty()) out.back().out = (c == cur ? e : c) - cur;
          out.push_back({e, e - (c == e ? cur : c), {}});
          cur = e;
          break;
        }
        case Verb::Cubic: {
          const Point c1 = path.points[i], c2 = path.points[i + 1], e = path.points[i + 2];
          i += 3;
          if (!out.empty()) out.back().out = (c1 == cur ? (c2 == cur ? e : c2) : c1) - cur;
          out.push_back({e, e - (c2 == e ? (c1 == e ? cur : c1) : c2), {}});
          cur = e;
          break;
        }
        case Verb::Close: {
          if (!out.empty()) {
            out.back().out = start - cur;
            out.push_back({start, start - cur, {}});
            if (start_index < out.size()) {
              out.back().out = out[start_index].out;
              out[start_index].in = start - cur;
            }
          }
          cur = start;
          break;
        }
      }
    }
    return out;
  }

  Result<void> add_markers(const XmlNode& n, const State& s, const Path& path, double stroke_width, Group& into,
                           int depth) {
    const std::string_view name = n.local_name();
    if (name != "path" && name != "line" && name != "polyline" && name != "polygon") return {};
    const std::string start = url_reference(s.get("marker-start"));
    const std::string mid = url_reference(s.get("marker-mid"));
    const std::string end = url_reference(s.get("marker-end"));
    if (start.empty() && mid.empty() && end.empty()) return {};
    const std::vector<Vertex> vs = path_vertices(path);
    if (vs.empty()) return {};
    const auto angle_of = [](Point d) { return std::atan2(d.y, d.x); };
    for (std::size_t i = 0; i < vs.size(); ++i) {
      const bool first = i == 0, last = i + 1 == vs.size();
      const std::string& id = first ? start : last ? end : mid;
      if (id.empty()) continue;
      const XmlNode mk = lookup(id);
      if (!mk || mk.local_name() != "marker") continue;
      if (!marker_stack_.insert(mk.internal_data()).second) continue;
      const Vertex& v = vs[i];
      double angle = 0;
      const std::string_view orient = attr(mk, "orient");
      if (orient == "auto" || orient == "auto-start-reverse") {
        const bool has_in = length(v.in) > 0, has_out = length(v.out) > 0;
        if (has_in && has_out) {
          const double a = angle_of(v.in), b = angle_of(v.out);
          double d = b - a;
          while (d > std::numbers::pi) d -= 2 * std::numbers::pi;
          while (d < -std::numbers::pi) d += 2 * std::numbers::pi;
          angle = a + d / 2;
        } else if (has_out) {
          angle = angle_of(v.out);
        } else if (has_in) {
          angle = angle_of(v.in);
        }
        if (first && orient == "auto-start-reverse") angle += std::numbers::pi;
        if (first && has_out) angle = angle_of(v.out) + (orient == "auto-start-reverse" ? std::numbers::pi : 0);
        if (last && has_in) angle = angle_of(v.in);
      } else if (const auto deg = util::parse_double(util::trim(orient))) {
        angle = *deg * std::numbers::pi / 180;
      }
      State ms = state_for(mk);
      const double mw = length_attr(mk, "markerWidth", ms, 'x', 3);
      const double mh = length_attr(mk, "markerHeight", ms, 'y', 3);
      if (!(mw > 0) || !(mh > 0)) {
        marker_stack_.erase(mk.internal_data());
        continue;
      }
      const double scale = attr(mk, "markerUnits") == "userSpaceOnUse" ? 1.0 : stroke_width;
      Matrix vbt;
      const std::vector<double> vb = parse_numbers(attr(mk, "viewBox"));
      if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) {
        vbt = view_box_transform(Rect::xywh(vb[0], vb[1], vb[2], vb[3]), parse_aspect_ratio(attr(mk, "preserveAspectRatio")),
                                 0, 0, mw, mh);
        ms.vp = {vb[2], vb[3]};
      }
      const Point ref = vbt.apply({length_attr(mk, "refX", ms, 'x', 0), length_attr(mk, "refY", ms, 'y', 0)});
      Group outer;
      outer.transform = Matrix::translate(v.p.x, v.p.y) * Matrix::rotate(angle) * Matrix::scale(scale, scale) *
                        Matrix::translate(-ref.x, -ref.y);
      const std::string_view overflow = attr(mk, "overflow");
      if (overflow.empty() || overflow == "hidden" || overflow == "scroll") {
        auto clip = std::make_shared<ClipPath>();
        ClipPath::Item item;
        add_rect(item.path, 0, 0, mw, mh);
        clip->items.push_back(std::move(item));
        outer.clip = std::move(clip);
      }
      Group inner;
      inner.transform = vbt;
      for (const XmlNode c : mk.elements()) {
        auto node = element(c, ms, depth + 1);
        if (!node) {
          marker_stack_.erase(mk.internal_data());
          return std::unexpected(node.error());
        }
        if (*node) inner.children.push_back(std::move(**node));
      }
      marker_stack_.erase(mk.internal_data());
      outer.children.push_back(Node{std::move(inner)});
      into.children.push_back(Node{std::move(outer)});
    }
    return {};
  }

  // ------------------------------------------------------------------ text
  struct Glyph {
    char32_t cp = 0;
    std::size_t style = 0;  // index into text_styles_
    std::optional<double> x, y;
    double dx = 0, dy = 0;
  };
  struct TextStyle {
    FontSpec font;
    Paint fill;
    Stroke stroke;
    double letter_spacing = 0;
    double word_spacing = 0;
    double baseline_shift = 0;
    bool visible = true;
  };

  Result<std::size_t> text_style(const State& s, int depth) {
    TextStyle ts;
    std::string family(util::trim(s.get("font-family")));
    family = util::replace_all(family, "'", "");
    family = util::replace_all(family, "\"", "");
    ts.font.family = family.empty() ? "sans-serif" : family;
    ts.font.size = s.font_size;
    const std::string_view weight = s.get("font-weight");
    if (weight == "bold" || weight == "bolder") ts.font.weight = 700;
    else if (weight == "lighter") ts.font.weight = 300;
    else if (const auto w = util::parse_int(util::trim(weight))) ts.font.weight = static_cast<int>(std::clamp<std::int64_t>(*w, 1, 1000));
    const std::string_view style = s.get("font-style");
    ts.font.italic = style == "italic" || style == "oblique";
    const auto no_bbox = [] { return Bbox{}; };
    const std::function<Bbox()> bb = no_bbox;
    const std::string_view fill = s.has("fill") ? s.get("fill") : std::string_view("black");
    CVT_TRY_ASSIGN(ts.fill, resolve_paint(fill, s.get("fill-opacity"), s, bb, depth));
    CVT_TRY_ASSIGN(ts.stroke, stroke_of(s, bb, depth));
    if (const std::string_view ls = s.get("letter-spacing"); !ls.empty() && ls != "normal") {
      ts.letter_spacing = length_prop(ls, s, 'x', 0);
    }
    if (const std::string_view ws = s.get("word-spacing"); !ws.empty() && ws != "normal") {
      ts.word_spacing = length_prop(ws, s, 'x', 0);
    }
    const std::string_view bs = s.get("baseline-shift");
    if (bs == "super") ts.baseline_shift = -s.font_size * 0.33;
    else if (bs == "sub") ts.baseline_shift = s.font_size * 0.2;
    else if (!bs.empty() && bs != "baseline") {
      if (const auto l = parse_length(bs)) {
        ts.baseline_shift = -(l->unit == Unit::Percent ? l->value / 100 * s.font_size : to_px(*l, s, 'y'));
      }
    }
    ts.visible = s.get("visibility") != "hidden" && s.get("visibility") != "collapse";
    text_styles_.push_back(std::move(ts));
    return text_styles_.size() - 1;
  }

  Result<void> collect_glyphs(const XmlNode& n, const State& s, std::vector<Glyph>& glyphs, bool& last_space,
                              bool preserve, int depth) {
    if (depth > max_depth) return fail(Status::Limit, "SVG text is nested too deeply");
    CVT_TRY_ASSIGN(const std::size_t style, text_style(s, depth));
    std::vector<double> xs, ys, dxs, dys;
    const auto lens = [&](std::string_view name, char axis) {
      std::vector<double> out;
      for (const Length& l : parse_lengths(attr(n, name))) out.push_back(to_px(l, s, axis));
      return out;
    };
    xs = lens("x", 'x');
    ys = lens("y", 'y');
    dxs = lens("dx", 'x');
    dys = lens("dy", 'y');
    std::size_t index = 0;  // character index within this element
    const bool keep = preserve || attr(n, "xml:space") == "preserve" || s.get("white-space") == "pre";
    for (const XmlNode c : n.children()) {
      if (c.is_text()) {
        std::string text(c.value());
        std::size_t pos = 0;
        while (pos < text.size()) {
          const util::Utf8Decoded d = util::decode_utf8(text, pos);
          pos += std::max<std::size_t>(1, d.length);
          char32_t cp = d.code_point;
          if (!keep) {
            if (cp == '\n' || cp == '\r') continue;
            if (cp == '\t') cp = ' ';
            if (cp == ' ' && last_space) continue;
          } else if (cp == '\n' || cp == '\r' || cp == '\t') {
            cp = ' ';
          }
          Glyph g;
          g.cp = cp;
          g.style = style;
          if (index < xs.size()) g.x = xs[index];
          if (index < ys.size()) g.y = ys[index];
          if (index < dxs.size()) g.dx = dxs[index];
          if (index < dys.size()) g.dy = dys[index];
          glyphs.push_back(g);
          last_space = cp == ' ';
          ++index;
        }
      } else if (c.is_element()) {
        const std::string_view name = c.local_name();
        if (name != "tspan" && name != "a" && name != "textPath" && name != "tref") continue;
        const State cs = derive(s, c);
        if (cs.get("display") == "none") continue;
        const std::size_t before = glyphs.size();
        CVT_TRY(collect_glyphs(c, cs, glyphs, last_space, keep, depth + 1));
        // Positions given on this element apply to the first characters of the child too.
        for (std::size_t k = before; k < glyphs.size() && index < std::max({xs.size(), ys.size(), dxs.size(), dys.size()});
             ++k, ++index) {
          if (index < xs.size() && !glyphs[k].x) glyphs[k].x = xs[index];
          if (index < ys.size() && !glyphs[k].y) glyphs[k].y = ys[index];
          if (index < dxs.size()) glyphs[k].dx += dxs[index];
          if (index < dys.size()) glyphs[k].dy += dys[index];
        }
      }
    }
    return {};
  }

  Result<std::optional<Node>> text_element(const XmlNode& n, const State& s, int depth) {
    // textPath: lay the characters along a path.
    for (const XmlNode c : n.elements()) {
      if (c.local_name() == "textPath") return text_on_path(n, c, s, depth);
    }
    std::vector<Glyph> glyphs;
    bool last_space = true;
    CVT_TRY(collect_glyphs(n, s, glyphs, last_space, false, depth));
    // Drop trailing white space (leading is already collapsed).
    while (!glyphs.empty() && glyphs.back().cp == ' ' && attr(n, "xml:space") != "preserve") glyphs.pop_back();
    if (glyphs.empty()) return std::optional<Node>{};
    const std::string_view anchor = s.get("text-anchor");
    Group group;
    // Lay out: chunks start at every absolute position.
    struct Run {
      std::size_t style;
      std::u32string text;
      Point origin;
      std::size_t chunk;
    };
    std::vector<Run> runs;
    std::vector<std::pair<double, double>> chunk_extent;  // start x, end x
    Point pen{0, 0};
    std::size_t chunk = 0;
    bool first = true;
    for (const Glyph& g : glyphs) {
      const bool absolute = g.x || g.y;
      if (g.x) pen.x = *g.x;
      if (g.y) pen.y = *g.y;
      if (absolute && !first) ++chunk;
      if (absolute || first) chunk_extent.resize(chunk + 1, {pen.x, pen.x});
      pen.x += g.dx;
      pen.y += g.dy;
      const TextStyle& ts = text_styles_[g.style];
      const bool new_run = runs.empty() || runs.back().style != g.style || runs.back().chunk != chunk || absolute ||
                           g.dx != 0 || g.dy != 0;
      if (new_run) runs.push_back({g.style, {}, {pen.x, pen.y + ts.baseline_shift}, chunk});
      runs.back().text.push_back(g.cp);
      // Advance by this character.
      const std::string utf8 = util::utf32_to_utf8(std::u32string(1, g.cp));
      const TextLayout l = layout_text(utf8, ts.font, ts.letter_spacing, ts.word_spacing);
      pen.x += l.advance;
      chunk_extent[chunk].second = pen.x;
      first = false;
    }
    for (Run& r : runs) {
      const TextStyle& ts = text_styles_[r.style];
      if (!ts.visible) continue;
      const auto [x0, x1] = chunk_extent[r.chunk];
      double shift = 0;
      if (anchor == "middle") shift = -(x1 - x0) / 2;
      else if (anchor == "end") shift = -(x1 - x0);
      const std::string utf8 = util::utf32_to_utf8(r.text);
      Text t = make_text(utf8, ts.font, {r.origin.x + shift, r.origin.y}, ts.fill);
      // make_text lays out the whole run at once (kerning, spacing).
      if (ts.letter_spacing != 0 || ts.word_spacing != 0) {
        const TextLayout l = layout_text(utf8, ts.font, ts.letter_spacing, ts.word_spacing);
        t.outline = transformed(l.outline, Matrix::translate(t.origin.x, t.origin.y));
        t.advance = l.advance;
      }
      t.stroke = ts.stroke;
      if (!t.fill.visible() && !t.stroke.paint.visible()) continue;
      group.children.push_back(Node{std::move(t)});
    }
    if (group.children.empty()) return std::optional<Node>{};
    if (group.children.size() == 1) return std::optional<Node>{std::move(group.children.front())};
    return std::optional<Node>{Node{std::move(group)}};
  }

  Result<std::optional<Node>> text_on_path(const XmlNode& text, const XmlNode& tp, const State& s, int depth) {
    const XmlNode target = lookup(local_ref(href_of(tp)));
    if (!target) return std::optional<Node>{};
    Path path = shape_path(target, state_for(target));
    path = transformed(path, transform_of(target));
    const std::vector<Polyline> lines = flatten(path, 0.05);
    if (lines.empty() || lines[0].points.size() < 2) return std::optional<Node>{};
    const std::vector<Point>& pts = lines[0].points;
    std::vector<double> cum{0};
    for (std::size_t i = 1; i < pts.size(); ++i) cum.push_back(cum.back() + length(pts[i] - pts[i - 1]));
    const double total = cum.back();
    const State ts_state = derive(s, tp);
    std::vector<Glyph> glyphs;
    bool last_space = true;
    CVT_TRY(collect_glyphs(tp, ts_state, glyphs, last_space, false, depth + 1));
    (void)text;
    double offset = 0;
    if (const auto so = parse_length(attr(tp, "startOffset"))) {
      offset = so->unit == Unit::Percent ? so->value / 100 * total : to_px(*so, ts_state, 'x');
    }
    std::string content;
    Path outline;
    double pen = offset;
    const std::string_view anchor = ts_state.get("text-anchor");
    // Measure for the anchor.
    double width = 0;
    for (const Glyph& g : glyphs) {
      const TextStyle& st = text_styles_[g.style];
      width += layout_text(util::utf32_to_utf8(std::u32string(1, g.cp)), st.font, st.letter_spacing, st.word_spacing).advance;
    }
    if (anchor == "middle") pen -= width / 2;
    else if (anchor == "end") pen -= width;
    Paint fill;
    Stroke stroke;
    FontSpec font;
    for (const Glyph& g : glyphs) {
      const TextStyle& st = text_styles_[g.style];
      fill = st.fill;
      stroke = st.stroke;
      font = st.font;
      const std::string utf8 = util::utf32_to_utf8(std::u32string(1, g.cp));
      content += utf8;
      const TextLayout l = layout_text(utf8, st.font, st.letter_spacing, st.word_spacing);
      pen += g.dx;
      const double mid = pen + l.advance / 2;
      if (mid >= 0 && mid <= total) {
        const auto it = std::lower_bound(cum.begin(), cum.end(), mid);
        const std::size_t k = std::clamp<std::size_t>(static_cast<std::size_t>(it - cum.begin()), 1, pts.size() - 1);
        const double seg = cum[k] - cum[k - 1];
        const double f = seg > 0 ? (mid - cum[k - 1]) / seg : 0;
        const Point p = lerp(pts[k - 1], pts[k], f);
        const Point d = pts[k] - pts[k - 1];
        const double a = std::atan2(d.y, d.x);
        const Matrix m = Matrix::translate(p.x, p.y) * Matrix::rotate(a) * Matrix::translate(-l.advance / 2, g.dy);
        append(outline, l.outline, m);
      }
      pen += l.advance;
    }
    Text t;
    t.content = content;
    t.font = font;
    t.origin = pts.front();
    t.advance = width;
    t.outline = std::move(outline);
    t.fill = fill;
    t.stroke = stroke;
    return std::optional<Node>{Node{std::move(t)}};
  }

  // ------------------------------------------------------------------ images
  Result<std::optional<Node>> image_element(const XmlNode& n, const State& s, int depth) {
    const std::string_view href = href_of(n);
    std::string mime;
    const auto data = decode_data_uri(href, &mime);
    if (!data) {
      if (!href.empty()) ctx_.warn("An SVG image refers to an external file, which is never loaded (privacy).");
      return std::optional<Node>{};
    }
    CVT_TRY(ctx_.limits.check_decompressed(data->size()));
    const double x = length_attr(n, "x", s, 'x', 0), y = length_attr(n, "y", s, 'y', 0);
    double w = length_attr(n, "width", s, 'x', -1), h = length_attr(n, "height", s, 'y', -1);
    const AspectRatio ar = parse_aspect_ratio(attr(n, "preserveAspectRatio"));
    if (mime == "image/svg+xml" || (data->size() > 4 && as_chars(*data).find("<svg") != std::string_view::npos &&
                                    !image::sniff_format(*data))) {
      if (nesting_ >= 4) return std::optional<Node>{};
      Reader nested(ctx_, options_);
      nested.nesting_ = nesting_ + 1;
      auto scene = nested.run(*data, nesting_ + 1);
      if (!scene || scene->pages.empty()) {
        ctx_.warn("An embedded SVG picture could not be read.");
        return std::optional<Node>{};
      }
      Page& page = scene->pages.front();
      if (w < 0) w = page.width;
      if (h < 0) h = page.height;
      Group g;
      g.transform = view_box_transform(Rect::xywh(0, 0, page.width, page.height), ar, x, y, w, h);
      g.children.push_back(Node{std::move(page.root)});
      Group clip_group;
      auto clip = std::make_shared<ClipPath>();
      ClipPath::Item item;
      add_rect(item.path, x, y, w, h);
      clip->items.push_back(std::move(item));
      clip_group.clip = std::move(clip);
      clip_group.children.push_back(Node{std::move(g)});
      return std::optional<Node>{Node{std::move(clip_group)}};
    }
    std::string format;
    if (auto sniffed = image::sniff_format(*data)) format = *sniffed;
    else if (mime == "image/png") format = "png";
    else if (mime == "image/jpeg" || mime == "image/jpg") format = "jpg";
    else if (mime == "image/gif") format = "gif";
    else if (mime == "image/webp") format = "webp";
    auto decoded = image::decode(format, *data, ctx_);
    if (!decoded || !decoded->valid()) {
      ctx_.warn("An embedded SVG picture could not be decoded and was left out.");
      return std::optional<Node>{};
    }
    (void)depth;
    const ir::Frame& frame = decoded->frames.front();
    if (w < 0) w = frame.width;
    if (h < 0) h = frame.height;
    if (!(w > 0) || !(h > 0)) return std::optional<Node>{};
    Picture pic;
    pic.pixels = std::make_shared<const ir::Frame>(frame);
    if (format == "png" || format == "jpg" || format == "gif") {
      pic.encoded = std::move(*data);
      pic.encoded_format = format;
    }
    const std::string_view rendering = s.get("image-rendering");
    pic.smooth = !(rendering == "pixelated" || rendering == "optimizeSpeed" || rendering == "crisp-edges");
    const Matrix m = view_box_transform(Rect::xywh(0, 0, frame.width, frame.height), ar, x, y, w, h);
    const Rect placed = transform_rect(Rect::xywh(0, 0, frame.width, frame.height), m);
    pic.bounds = placed;
    if (ar.slice && ar.x != AspectRatio::Align::None) {
      Group g;
      auto clip = std::make_shared<ClipPath>();
      ClipPath::Item item;
      add_rect(item.path, x, y, w, h);
      clip->items.push_back(std::move(item));
      g.clip = std::move(clip);
      g.children.push_back(Node{std::move(pic)});
      return std::optional<Node>{Node{std::move(g)}};
    }
    return std::optional<Node>{Node{std::move(pic)}};
  }

  // ------------------------------------------------------------------ elements
  Result<void> container_children(const XmlNode& n, const State& s, Group& g, int depth) {
    for (const XmlNode c : n.elements()) {
      CVT_TRY_ASSIGN(auto node, element(c, s, depth + 1));
      if (node) g.children.push_back(std::move(*node));
    }
    return {};
  }

  // Wrap content with the element's transform, opacity, clip, mask and filter.
  Result<std::optional<Node>> finish(const XmlNode& n, const State& s, Node content, const Matrix& transform,
                                     int depth, Group* as_group = nullptr) {
    const std::string_view op = s.get("opacity");
    double opacity = 1.0;
    if (!op.empty()) {
      if (const auto o = parse_number_or_percent(op)) opacity = clamp01(*o);
    }
    const std::string clip_id = url_reference(s.get("clip-path"));
    const std::string mask_id = url_reference(s.get("mask"));
    const std::string filter_id = url_reference(s.get("filter"));
    const std::string_view blend = s.get("mix-blend-mode");
    const bool needs = opacity < 1.0 || !clip_id.empty() || !mask_id.empty() || !filter_id.empty() ||
                       (!blend.empty() && blend != "normal") || !transform.is_identity(0) || as_group;
    if (!needs) return std::optional<Node>{std::move(content)};
    Group g;
    if (as_group) {
      g = std::move(*as_group);
    } else {
      g.children.push_back(std::move(content));
    }
    g.transform = transform * g.transform;
    g.opacity = opacity;
    if (!blend.empty() && blend != "normal") {
      static const std::map<std::string_view, BlendMode> modes = {
          {"multiply", BlendMode::Multiply}, {"screen", BlendMode::Screen}, {"overlay", BlendMode::Overlay},
          {"darken", BlendMode::Darken}, {"lighten", BlendMode::Lighten}, {"color-dodge", BlendMode::ColorDodge},
          {"color-burn", BlendMode::ColorBurn}, {"hard-light", BlendMode::HardLight},
          {"soft-light", BlendMode::SoftLight}, {"difference", BlendMode::Difference},
          {"exclusion", BlendMode::Exclusion}, {"hue", BlendMode::Hue}, {"saturation", BlendMode::Saturation},
          {"color", BlendMode::Color}, {"luminosity", BlendMode::Luminosity}};
      if (const auto it = modes.find(blend); it != modes.end()) g.blend = it->second;
    }
    if (!clip_id.empty() || !mask_id.empty() || !filter_id.empty()) {
      Rect r;
      for (const Node& c : g.children) node_bounds(c, {}, r);
      const Bbox bbox{r};
      if (!clip_id.empty()) {
        const XmlNode cp = lookup(clip_id);
        if (cp && cp.local_name() == "clipPath") {
          CVT_TRY_ASSIGN(g.clip, clip_path(cp, bbox, depth + 1));
        } else {
          return std::optional<Node>{};  // a reference to a missing clip path hides the element (SVG 1.1)
        }
      }
      if (!mask_id.empty()) {
        const XmlNode mk = lookup(mask_id);
        if (mk && mk.local_name() == "mask") {
          CVT_TRY_ASSIGN(g.mask, mask(mk, bbox, depth + 1));
        } else {
          return std::optional<Node>{};
        }
      }
      if (!filter_id.empty()) {
        const XmlNode f = lookup(filter_id);
        if (f && f.local_name() == "filter") {
          g.filter = filter(f, bbox);
          if (!g.filter && !bbox.usable()) return std::optional<Node>{};
        } else {
          return std::optional<Node>{};
        }
      }
    }
    if (as_group) {
      if (const std::string_view id = attr(n, "id"); !id.empty() && g.id.empty()) g.id = std::string(id);
    }
    return std::optional<Node>{Node{std::move(g)}};
  }

  Result<std::optional<Node>> element(const XmlNode& n, const State& parent, int depth) {
    if (depth > max_depth) return fail(Status::Limit, "the SVG is nested more than {} levels deep", max_depth);
    if (++node_count_ > max_nodes) return fail(Status::Limit, "the SVG expands to more than {} elements", max_nodes);
    if ((node_count_ & 1023) == 0) CVT_TRY(ctx_.checkpoint(0.5, "Reading SVG"));
    const std::string_view ns = n.namespace_uri();
    if (!ns.empty() && ns != "http://www.w3.org/2000/svg") return std::optional<Node>{};
    const std::string_view name = n.local_name();
    static constexpr std::string_view never_rendered[] = {
        "defs", "clipPath", "mask", "pattern", "linearGradient", "radialGradient", "marker", "symbol", "filter",
        "style", "title", "desc", "metadata", "script", "font", "font-face", "cursor", "view", "animate",
        "animateTransform", "animateMotion", "set", "stop"};
    if (std::find(std::begin(never_rendered), std::end(never_rendered), name) != std::end(never_rendered)) {
      return std::optional<Node>{};
    }
    if (has_attr(n, "requiredExtensions") || has_attr(n, "requiredFeatures")) {
      // Unsupported extensions are not rendered; requiredFeatures is ignored (SVG 2).
      if (has_attr(n, "requiredExtensions") && !util::trim(attr(n, "requiredExtensions")).empty()) {
        return std::optional<Node>{};
      }
    }
    State s = derive(parent, n);
    const bool layer = (name == "g") && attr_local(n, "groupmode") == "layer";
    if (s.get("display") == "none" && !layer) return std::optional<Node>{};
    Matrix transform = transform_of(n);
    if (name == "g" || name == "a" || name == "switch") {
      Group g;
      if (name == "switch") {
        for (const XmlNode c : n.elements()) {
          if (has_attr(c, "requiredExtensions") && !util::trim(attr(c, "requiredExtensions")).empty()) continue;
          if (has_attr(c, "systemLanguage") && !util::istarts_with(util::trim(attr(c, "systemLanguage")), "en")) continue;
          CVT_TRY_ASSIGN(auto node, element(c, s, depth + 1));
          if (node) g.children.push_back(std::move(*node));
          break;
        }
      } else {
        CVT_TRY(container_children(n, s, g, depth));
      }
      if (n.attribute_local("groupmode") && attr_local(n, "groupmode") == "layer") {
        g.layer = true;
        g.layer_name = std::string(attr_local(n, "label"));
        if (g.layer_name.empty()) g.layer_name = std::string(attr(n, "id"));
        g.visible = s.get("display") != "none";
      }
      if (s.get("visibility") == "hidden" && g.children.empty()) return std::optional<Node>{};
      return finish(n, s, Node{}, transform, depth, &g);
    }
    if (name == "svg") {
      // Nested viewport.
      const double x = length_attr(n, "x", s, 'x', 0), y = length_attr(n, "y", s, 'y', 0);
      const double w = length_attr(n, "width", s, 'x', s.vp.w), h = length_attr(n, "height", s, 'y', s.vp.h);
      if (!(w > 0) || !(h > 0)) return std::optional<Node>{};
      return viewport_content(n, s, x, y, w, h, transform, depth);
    }
    if (name == "use") {
      const XmlNode target = lookup(local_ref(href_of(n)));
      if (!target) return std::optional<Node>{};
      if (!use_stack_.insert(target.internal_data()).second) {
        ctx_.warn("An SVG <use> element refers to itself; the loop was ignored.");
        return std::optional<Node>{};
      }
      for (XmlNode p = n; p; p = p.parent()) {
        if (p == target) {
          use_stack_.erase(target.internal_data());
          return std::optional<Node>{};
        }
      }
      const double x = length_attr(n, "x", s, 'x', 0), y = length_attr(n, "y", s, 'y', 0);
      transform = transform * Matrix::translate(x, y);
      Result<std::optional<Node>> content = std::optional<Node>{};
      const std::string_view tname = target.local_name();
      if (tname == "symbol" || tname == "svg") {
        const State ts = derive(s, target);
        const double w = length_attr(n, "width", s, 'x', length_attr(target, "width", ts, 'x', s.vp.w));
        const double h = length_attr(n, "height", s, 'y', length_attr(target, "height", ts, 'y', s.vp.h));
        if (w > 0 && h > 0) {
          content = viewport_content(target, ts, length_attr(target, "x", ts, 'x', 0), length_attr(target, "y", ts, 'y', 0),
                                     w, h, transform_of(target), depth + 1);
        }
      } else {
        content = element(target, s, depth + 1);
      }
      use_stack_.erase(target.internal_data());
      CVT_TRY_ASSIGN(auto node, std::move(content));
      if (!node) return std::optional<Node>{};
      return finish(n, s, std::move(*node), transform, depth);
    }
    if (name == "image") {
      if (s.get("visibility") == "hidden") return std::optional<Node>{};
      CVT_TRY_ASSIGN(auto node, image_element(n, s, depth));
      if (!node) return std::optional<Node>{};
      return finish(n, s, std::move(*node), transform, depth);
    }
    if (name == "text") {
      CVT_TRY_ASSIGN(auto node, text_element(n, s, depth));
      if (!node) return std::optional<Node>{};
      return finish(n, s, std::move(*node), transform, depth);
    }
    if (name == "foreignObject") {
      ctx_.warn("SVG foreignObject content (HTML) is not drawn.");
      return std::optional<Node>{};
    }
    if (name == "path" || name == "rect" || name == "circle" || name == "ellipse" || name == "line" ||
        name == "polyline" || name == "polygon") {
      Shape shape;
      shape.path = shape_path(n, s);
      if (shape.path.empty()) return std::optional<Node>{};
      const Path* geometry = &shape.path;
      const auto bbox = [geometry]() { return Bbox{bounds(*geometry)}; };
      const bool hidden = s.get("visibility") == "hidden" || s.get("visibility") == "collapse";
      const std::string_view fill = s.has("fill") ? s.get("fill") : std::string_view("black");
      CVT_TRY_ASSIGN(shape.fill, resolve_paint(fill, s.get("fill-opacity"), s, bbox, depth));
      shape.fill_rule = s.get("fill-rule") == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
      CVT_TRY_ASSIGN(shape.stroke, stroke_of(s, bbox, depth));
      if (name == "line" || name == "polyline") shape.fill = Paint{};
      const std::string_view order = s.get("paint-order");
      if (!order.empty()) {
        const std::size_t f = order.find("fill"), st = order.find("stroke");
        shape.stroke_first = st != std::string_view::npos && (f == std::string_view::npos || st < f);
      }
      shape.id = std::string(attr(n, "id"));
      Group with_markers;
      const double sw = shape.stroke.width > 0 ? shape.stroke.width : length_prop(s.get("stroke-width"), s, 'o', 1.0);
      const Path path_copy = shape.path;
      if (hidden) {
        shape.fill = Paint{};
        shape.stroke.paint = Paint{};
      }
      with_markers.children.push_back(Node{std::move(shape)});
      CVT_TRY(add_markers(n, s, path_copy, sw, with_markers, depth));
      if (with_markers.children.size() == 1) {
        return finish(n, s, std::move(with_markers.children.front()), transform, depth);
      }
      return finish(n, s, Node{}, transform, depth, &with_markers);
    }
    return std::optional<Node>{};
  }

  static std::string_view attr_local(const XmlNode& n, std::string_view local) {
    const XmlAttribute a = n.attribute_local(local);
    return a ? a.value() : std::string_view{};
  }

  Result<std::optional<Node>> viewport_content(const XmlNode& n, State s, double x, double y, double w, double h,
                                               const Matrix& transform, int depth) {
    Group inner;
    const std::vector<double> vb = parse_numbers(attr(n, "viewBox"));
    if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) {
      inner.transform =
          view_box_transform(Rect::xywh(vb[0], vb[1], vb[2], vb[3]), parse_aspect_ratio(attr(n, "preserveAspectRatio")), x, y, w, h);
      s.vp = {vb[2], vb[3]};
    } else {
      inner.transform = Matrix::translate(x, y);
      s.vp = {w, h};
    }
    CVT_TRY(container_children(n, s, inner, depth));
    Group outer;
    const std::string_view overflow = s.get("overflow");
    if (overflow.empty() || overflow == "hidden" || overflow == "scroll") {
      auto clip = std::make_shared<ClipPath>();
      ClipPath::Item item;
      add_rect(item.path, x, y, w, h);
      clip->items.push_back(std::move(item));
      outer.clip = std::move(clip);
    }
    outer.children.push_back(Node{std::move(inner)});
    return finish(n, s, Node{}, transform, depth, &outer);
  }

  const Context& ctx_;
  ReadOptions options_;
  std::map<std::string, XmlNode, std::less<>> ids_;
  std::vector<CssRule> rules_;
  std::size_t rule_order_ = 0;
  State root_state_;
  std::vector<TextStyle> text_styles_;
  std::set<const void*> use_stack_, clip_stack_, mask_stack_, pattern_stack_, marker_stack_;
  std::uint64_t node_count_ = 0;
  mutable bool path_warned_ = false;
  int nesting_ = 0;
};

Result<Scene> Reader::run(ByteView bytes, int nesting) {
  nesting_ = nesting;
  XmlParseOptions xml;
  xml.preserve_whitespace = true;
  xml.replace_invalid_text = true;
  xml.keep_unknown_entities = true;
  xml.max_depth = 512;
  CVT_TRY_ASSIGN(XmlDocument doc, parse_xml(bytes, xml));
  const XmlNode root = doc.root();
  if (!root || root.local_name() != "svg") return fail(Status::Decode, "not an SVG document (the root element is not <svg>)");
  index(root);
  root_state_.vp = {100, 100};
  // Page size.
  const std::vector<double> vb = parse_numbers(attr(root, "viewBox"));
  const bool has_vb = vb.size() == 4 && vb[2] > 0 && vb[3] > 0 && std::isfinite(vb[2]) && std::isfinite(vb[3]);
  const auto size = [&](std::string_view name, double from_vb) -> double {
    const std::string_view v = attr(root, name);
    const auto l = parse_length(v);
    if (!l || l->unit == Unit::Percent || !(l->value > 0)) return from_vb;
    return to_px(*l, root_state_, 'x');
  };
  double width = size("width", has_vb ? vb[2] : -1);
  double height = size("height", has_vb ? vb[3] : -1);
  if (has_vb && width > 0 && height < 0) height = width * vb[3] / vb[2];
  if (has_vb && height > 0 && width < 0) width = height * vb[2] / vb[3];
  State s = root_state_;
  s.vp = has_vb ? Viewport{vb[2], vb[3]} : Viewport{width > 0 ? width : 100, height > 0 ? height : 100};
  s = derive(s, root);
  if (has_vb) s.vp = {vb[2], vb[3]};
  Group content;
  CVT_TRY(container_children(root, s, content, 0));
  if (!(width > 0) || !(height > 0)) {
    // No size at all: use the drawing's extent.
    Rect r;
    for (const Node& n : content.children) node_bounds(n, {}, r);
    width = r.empty() ? 100 : std::max(1.0, r.x1);
    height = r.empty() ? 100 : std::max(1.0, r.y1);
    ctx_.warn("The SVG has no size; the drawing's extent was used.");
  }
  if (!std::isfinite(width) || !std::isfinite(height) || width > 1e7 || height > 1e7) {
    return fail(Status::Decode, "the SVG declares an unusable size");
  }
  Page page;
  page.width = width;
  page.height = height;
  if (has_vb) {
    content.transform = view_box_transform(Rect::xywh(vb[0], vb[1], vb[2], vb[3]),
                                           parse_aspect_ratio(attr(root, "preserveAspectRatio")), 0, 0, width, height);
  }
  // Root opacity / clip / filter.
  Matrix root_transform;
  auto wrapped = finish(root, s, Node{}, root_transform, 0, &content);
  if (!wrapped) return std::unexpected(wrapped.error());
  if (*wrapped) {
    if (auto* g = std::get_if<Group>(&(*wrapped)->value)) page.root = std::move(*g);
  }
  Scene scene;
  scene.source_units = "px";
  if (options_.keep_metadata) {
    for (const XmlNode c : root.elements()) {
      if (c.local_name() == "title" && !scene.metadata.contains("title")) {
        scene.metadata["title"] = std::string(util::trim(c.text()));
      } else if (c.local_name() == "desc" && !scene.metadata.contains("description")) {
        scene.metadata["description"] = std::string(util::trim(c.text()));
      }
    }
  }
  scene.pages.push_back(std::move(page));
  return scene;
}

}  // namespace

Result<Scene> read_svg(ByteView bytes, const Context& ctx, const ReadOptions& options) {
  Reader reader(ctx, options);
  return reader.run(bytes, 0);
}

}  // namespace cvt::vector::svg
