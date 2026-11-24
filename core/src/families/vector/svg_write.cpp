// Convertible core - vector family: SVG writer (clean, optionally minified).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <string_view>

#include "families/image/image_api.hpp"
#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/svg.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::vector::svg {

namespace {

class Writer {
 public:
  Writer(const Context& ctx, const WriteOptions& options) : ctx_(ctx), o_(options) {}

  Result<std::string> run(const Scene& scene, std::size_t page_index) {
    if (scene.pages.empty()) return fail(Status::Encode, "the drawing has no pages");
    if (page_index >= scene.pages.size()) page_index = 0;
    if (scene.pages.size() > 1) ctx_.warn("SVG holds one page; only the first page was written.");
    const Page& page = scene.pages[page_index];
    const Group& root = page.root;
    const Matrix& rt = root.transform;
    const bool plain_root = root.opacity >= 1 && !root.clip && !root.mask && !root.filter &&
                            root.blend == BlendMode::Normal && !root.layer && root.visible && rt.b == 0 && rt.c == 0 &&
                            rt.a > 0 && rt.d > 0;
    std::string body;
    if (plain_root) {
      CVT_TRY(group_content(root, body, 1, true));
    } else {
      CVT_TRY(group(root, body, 1));
    }
    std::string out;
    if (!o_.minify) out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<svg xmlns=\"http://www.w3.org/2000/svg\"";
    if (uses_xlink_) out += " xmlns:xlink=\"http://www.w3.org/1999/xlink\"";
    if (uses_inkscape_) out += " xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\"";
    const double w = page.width > 0 ? page.width : 1, h = page.height > 0 ? page.height : 1;
    if (plain_root) {
      // The root transform becomes the viewBox.
      out += std::format(" width=\"{}\" height=\"{}\" viewBox=\"{} {} {} {}\"", num(w), num(h), fine(-rt.e / rt.a),
                         fine(-rt.f / rt.d), fine(w / rt.a), fine(h / rt.d));
      if (std::abs(rt.a - rt.d) > 1e-9 * std::max(rt.a, rt.d)) out += " preserveAspectRatio=\"none\"";
      out += '>';
    } else {
      out += std::format(" width=\"{}\" height=\"{}\" viewBox=\"0 0 {} {}\">", num(w), num(h), num(w), num(h));
    }
    nl(out);
    if (o_.keep_metadata) {
      if (const auto it = scene.metadata.find("title"); it != scene.metadata.end() && !it->second.empty()) {
        indent(out, 1);
        out += "<title>" + util::xml_escape(it->second) + "</title>";
        nl(out);
      }
      if (const auto it = scene.metadata.find("description"); it != scene.metadata.end() && !it->second.empty()) {
        indent(out, 1);
        out += "<desc>" + util::xml_escape(it->second) + "</desc>";
        nl(out);
      }
    }
    if (!defs_.empty()) {
      indent(out, 1);
      out += "<defs>";
      nl(out);
      out += defs_;
      indent(out, 1);
      out += "</defs>";
      nl(out);
    }
    out += body;
    out += "</svg>";
    nl(out);
    return out;
  }

 private:
  // ------------------------------------------------------------------ formatting
  void nl(std::string& s) const {
    if (!o_.minify) s += '\n';
  }
  void indent(std::string& s, int level) const {
    if (!o_.minify) s.append(static_cast<std::size_t>(level), ' ');
  }

  std::string num(double v) const {
    if (!std::isfinite(v)) v = 0;
    if (o_.precision >= 0) {
      const double scale = std::pow(10.0, o_.precision);
      v = std::round(v * scale) / scale;
    }
    if (v == 0) return "0";
    std::string s;
    if (o_.precision >= 0) {
      s = std::format("{:.{}f}", v, o_.precision);
      if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
      }
    } else {
      s = util::format_double(v);
    }
    if (o_.minify) {
      if (s.starts_with("0.")) s.erase(0, 1);
      else if (s.starts_with("-0.")) s.erase(1, 1);
    }
    if (s == "-0") s = "0";
    return s;
  }

  std::string color(const Color& c) const {
    const auto b = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    const int r = b(c.r), g = b(c.g), bl = b(c.b);
    if (o_.minify && r % 17 == 0 && g % 17 == 0 && bl % 17 == 0) {
      return std::format("#{:x}{:x}{:x}", r / 17, g / 17, bl / 17);
    }
    return std::format("#{:02x}{:02x}{:02x}", r, g, bl);
  }

  static std::string fine(double v) {
    std::string s = util::format_double(std::round(v * 1e6) / 1e6);
    if (s == "-0") s = "0";
    return s;
  }

  std::string matrix(const Matrix& m) const {
    if (m.b == 0 && m.c == 0 && m.a == 1 && m.d == 1) {
      return std::format("translate({}{}{})", num(m.e), m.f == 0 ? "" : " ", m.f == 0 ? "" : num(m.f));
    }
    if (m.b == 0 && m.c == 0 && m.e == 0 && m.f == 0) {
      return m.a == m.d ? std::format("scale({})", num(m.a)) : std::format("scale({} {})", num(m.a), num(m.d));
    }
    // Matrix entries need more precision than coordinates.
    return std::format("matrix({} {} {} {} {} {})", fine(m.a), fine(m.b), fine(m.c), fine(m.d), num(m.e), num(m.f));
  }

  // Path data; in minify mode each segment is written in its shorter form (absolute or relative).
  std::string path_data(const Path& path) const {
    std::string out;
    std::size_t i = 0;
    Point cur{}, start{};
    char last_cmd = 0;
    const auto append_numbers = [&](std::string& s, const std::vector<double>& values) {
      for (const double v : values) {
        const std::string n = num(v);
        if (!s.empty()) {
          const char prev = s.back();
          const bool prev_is_cmd = util::is_ascii_alpha(prev) && prev != 'e' && prev != 'E';
          if (!prev_is_cmd) {
            const bool need_space = !(o_.minify && (n[0] == '-' || (n[0] == '.' && last_number_has_dot(s))));
            if (need_space) s += ' ';
          }
        }
        s += n;
      }
    };
    const auto emit = [&](char abs_cmd, const std::vector<double>& abs_values, char rel_cmd,
                          const std::vector<double>& rel_values) {
      std::string a, r;
      a += (last_cmd == abs_cmd && o_.minify && abs_cmd != 'M') ? "" : std::string(1, abs_cmd);
      append_numbers(a, abs_values);
      if (o_.minify) {
        r += (last_cmd == rel_cmd && rel_cmd != 'm') ? "" : std::string(1, rel_cmd);
        append_numbers(r, rel_values);
      }
      const bool use_rel = o_.minify && r.size() < a.size();
      std::string& chosen = use_rel ? r : a;
      if (!out.empty() && !chosen.empty() && !util::is_ascii_alpha(chosen[0])) {
        const bool need_space = !(o_.minify && (chosen[0] == '-' || (chosen[0] == '.' && last_number_has_dot(out))));
        if (need_space) out += ' ';
      } else if (!out.empty() && !o_.minify) {
        out += ' ';
      }
      out += chosen;
      last_cmd = use_rel ? rel_cmd : abs_cmd;
    };
    for (const Verb v : path.verbs) {
      switch (v) {
        case Verb::Move: {
          const Point p = path.points[i++];
          emit('M', {p.x, p.y}, 'm', {p.x - cur.x, p.y - cur.y});
          // After a moveto, further coordinates are implicit linetos.
          last_cmd = last_cmd == 'M' ? 'L' : 'l';
          cur = start = p;
          break;
        }
        case Verb::Line: {
          const Point p = path.points[i++];
          if (o_.minify && num(p.y) == num(cur.y)) {
            emit('H', {p.x}, 'h', {p.x - cur.x});
          } else if (o_.minify && num(p.x) == num(cur.x)) {
            emit('V', {p.y}, 'v', {p.y - cur.y});
          } else {
            emit('L', {p.x, p.y}, 'l', {p.x - cur.x, p.y - cur.y});
          }
          cur = p;
          break;
        }
        case Verb::Quad: {
          const Point c = path.points[i], p = path.points[i + 1];
          i += 2;
          emit('Q', {c.x, c.y, p.x, p.y}, 'q', {c.x - cur.x, c.y - cur.y, p.x - cur.x, p.y - cur.y});
          cur = p;
          break;
        }
        case Verb::Cubic: {
          const Point c1 = path.points[i], c2 = path.points[i + 1], p = path.points[i + 2];
          i += 3;
          emit('C', {c1.x, c1.y, c2.x, c2.y, p.x, p.y}, 'c',
               {c1.x - cur.x, c1.y - cur.y, c2.x - cur.x, c2.y - cur.y, p.x - cur.x, p.y - cur.y});
          cur = p;
          break;
        }
        case Verb::Close:
          if (!out.empty() && !o_.minify) out += ' ';
          out += o_.minify ? 'z' : 'Z';
          last_cmd = 'z';
          cur = start;
          break;
      }
    }
    return out;
  }

  static bool last_number_has_dot(const std::string& s) {
    for (std::size_t k = s.size(); k-- > 0;) {
      const char c = s[k];
      if (c == '.') return true;
      if (!util::is_ascii_digit(c)) return false;
    }
    return false;
  }

  std::string next_id(char prefix) {
    std::string id(1, prefix);
    std::uint32_t n = id_counter_++;
    constexpr std::string_view digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string tail;
    do {
      tail += digits[n % 36];
      n /= 36;
    } while (n > 0);
    std::reverse(tail.begin(), tail.end());
    return id + tail;
  }

  // ------------------------------------------------------------------ defs
  Result<std::string> gradient(const std::shared_ptr<const Gradient>& g) {
    if (const auto it = ids_.find(g.get()); it != ids_.end()) return it->second;
    const std::string id = next_id('g');
    ids_[g.get()] = id;
    std::string s;
    indent(s, 2);
    if (g->type == Gradient::Type::Linear) {
      s += std::format("<linearGradient id=\"{}\" gradientUnits=\"userSpaceOnUse\" x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\"",
                       id, num(g->p1.x), num(g->p1.y), num(g->p2.x), num(g->p2.y));
    } else {
      s += std::format("<radialGradient id=\"{}\" gradientUnits=\"userSpaceOnUse\" cx=\"{}\" cy=\"{}\" r=\"{}\"", id,
                       num(g->center.x), num(g->center.y), num(g->radius));
      if (g->focal != g->center) s += std::format(" fx=\"{}\" fy=\"{}\"", num(g->focal.x), num(g->focal.y));
      if (g->focal_radius > 0) s += std::format(" fr=\"{}\"", num(g->focal_radius));
    }
    if (!g->transform.is_identity(1e-12)) s += " gradientTransform=\"" + matrix(g->transform) + "\"";
    if (g->spread == Spread::Reflect) s += " spreadMethod=\"reflect\"";
    if (g->spread == Spread::Repeat) s += " spreadMethod=\"repeat\"";
    if (g->linear_rgb) s += " color-interpolation=\"linearRGB\"";
    s += '>';
    nl(s);
    for (const GradientStop& stop : g->stops) {
      indent(s, 3);
      s += std::format("<stop offset=\"{}\"", num(std::clamp(stop.offset, 0.0, 1.0)));
      if (!(stop.color.r == 0 && stop.color.g == 0 && stop.color.b == 0)) s += " stop-color=\"" + color(stop.color) + "\"";
      if (stop.color.a < 1) s += " stop-opacity=\"" + num(stop.color.a) + "\"";
      s += "/>";
      nl(s);
    }
    indent(s, 2);
    s += g->type == Gradient::Type::Linear ? "</linearGradient>" : "</radialGradient>";
    nl(s);
    defs_ += s;
    return id;
  }

  Result<std::string> pattern(const std::shared_ptr<const PatternPaint>& p) {
    if (const auto it = ids_.find(p.get()); it != ids_.end()) return it->second;
    const std::string id = next_id('p');
    ids_[p.get()] = id;
    std::string content;
    if (p->content) {
      Group shifted;
      shifted.transform = Matrix::translate(-p->tile.x0, -p->tile.y0) * p->content->transform;
      shifted.children = p->content->children;
      shifted.opacity = p->content->opacity;
      shifted.clip = p->content->clip;
      shifted.mask = p->content->mask;
      shifted.filter = p->content->filter;
      CVT_TRY(group(shifted, content, 3));
    }
    std::string s;
    indent(s, 2);
    s += std::format("<pattern id=\"{}\" patternUnits=\"userSpaceOnUse\" x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\"", id,
                     num(p->tile.x0), num(p->tile.y0), num(p->tile.width()), num(p->tile.height()));
    if (!p->transform.is_identity(1e-12)) s += " patternTransform=\"" + matrix(p->transform) + "\"";
    s += '>';
    nl(s);
    s += content;
    indent(s, 2);
    s += "</pattern>";
    nl(s);
    defs_ += s;
    return id;
  }

  Result<std::string> clip(const std::shared_ptr<const ClipPath>& c) {
    if (const auto it = ids_.find(c.get()); it != ids_.end()) return it->second;
    std::string nested;
    if (c->clip) {
      CVT_TRY_ASSIGN(const std::string inner, clip(c->clip));
      nested = " clip-path=\"url(#" + inner + ")\"";
    }
    std::string body;
    for (const ClipPath::Item& item : c->items) {
      indent(body, 3);
      body += "<path d=\"" + path_data(item.path) + "\"";
      if (item.rule == FillRule::EvenOdd) body += " clip-rule=\"evenodd\"";
      body += "/>";
      nl(body);
    }
    // Identical clip paths (markers, repeated symbols) share one definition.
    const std::string key = nested + body;
    if (const auto it = clip_by_content_.find(key); it != clip_by_content_.end()) {
      ids_[c.get()] = it->second;
      return it->second;
    }
    const std::string id = next_id('c');
    ids_[c.get()] = id;
    clip_by_content_[key] = id;
    std::string s;
    indent(s, 2);
    s += "<clipPath id=\"" + id + "\"" + nested + ">";
    nl(s);
    s += body;
    indent(s, 2);
    s += "</clipPath>";
    nl(s);
    defs_ += s;
    return id;
  }

  Result<std::string> mask(const std::shared_ptr<const Mask>& m) {
    if (const auto it = ids_.find(m.get()); it != ids_.end()) return it->second;
    const std::string id = next_id('m');
    ids_[m.get()] = id;
    std::string content;
    if (m->content) CVT_TRY(group(*m->content, content, 3));
    const Rect r = m->region.empty() ? Rect{-1e6, -1e6, 1e6, 1e6} : m->region;
    std::string s;
    indent(s, 2);
    s += std::format("<mask id=\"{}\" maskUnits=\"userSpaceOnUse\" x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\"{}>", id,
                     num(r.x0), num(r.y0), num(r.width()), num(r.height()),
                     m->type == Mask::Type::Alpha ? " mask-type=\"alpha\"" : "");
    nl(s);
    s += content;
    indent(s, 2);
    s += "</mask>";
    nl(s);
    defs_ += s;
    return id;
  }

  Result<std::string> filter(const std::shared_ptr<const Filter>& f) {
    if (const auto it = ids_.find(f.get()); it != ids_.end()) return it->second;
    const std::string id = next_id('f');
    ids_[f.get()] = id;
    std::string s;
    indent(s, 2);
    const Rect r = f->region.empty() ? Rect{-1e6, -1e6, 1e6, 1e6} : f->region;
    s += std::format("<filter id=\"{}\" filterUnits=\"userSpaceOnUse\" x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\"", id,
                     num(r.x0), num(r.y0), num(r.width()), num(r.height()));
    if (!f->primitives.empty() && !f->primitives.front().linear_rgb) s += " color-interpolation-filters=\"sRGB\"";
    s += '>';
    nl(s);
    static constexpr std::string_view blend_names[] = {
        "normal",     "multiply",   "screen",     "overlay",   "darken", "lighten",    "color-dodge", "color-burn",
        "hard-light", "soft-light", "difference", "exclusion", "hue",    "saturation", "color",       "luminosity"};
    for (const FilterPrimitive& p : f->primitives) {
      std::string io;
      if (!p.in.empty()) io += " in=\"" + util::xml_escape(p.in, true) + "\"";
      if (!p.in2.empty()) io += " in2=\"" + util::xml_escape(p.in2, true) + "\"";
      if (!p.result.empty()) io += " result=\"" + util::xml_escape(p.result, true) + "\"";
      indent(s, 3);
      switch (p.type) {
        case FilterPrimitive::Type::GaussianBlur:
          s += std::format("<feGaussianBlur{} stdDeviation=\"{} {}\"/>", io, num(p.std_dev_x), num(p.std_dev_y));
          break;
        case FilterPrimitive::Type::Offset: s += std::format("<feOffset{} dx=\"{}\" dy=\"{}\"/>", io, num(p.dx), num(p.dy)); break;
        case FilterPrimitive::Type::Flood:
          s += std::format("<feFlood{} flood-color=\"{}\" flood-opacity=\"{}\"/>", io, color(p.color), num(p.color.a));
          break;
        case FilterPrimitive::Type::Merge:
          s += "<feMerge" + io + ">";
          for (const std::string& in : p.merge_inputs) {
            s += in.empty() ? "<feMergeNode/>" : "<feMergeNode in=\"" + util::xml_escape(in, true) + "\"/>";
          }
          s += "</feMerge>";
          break;
        case FilterPrimitive::Type::ColorMatrix: {
          s += "<feColorMatrix" + io + " type=\"matrix\" values=\"";
          for (std::size_t k = 0; k < p.matrix.size(); ++k) {
            if (k) s += ' ';
            s += util::format_double(std::round(p.matrix[k] * 1e6) / 1e6);
          }
          s += "\"/>";
          break;
        }
        case FilterPrimitive::Type::DropShadow:
          s += std::format("<feDropShadow{} dx=\"{}\" dy=\"{}\" stdDeviation=\"{} {}\" flood-color=\"{}\" flood-opacity=\"{}\"/>",
                           io, num(p.dx), num(p.dy), num(p.std_dev_x), num(p.std_dev_y), color(p.color), num(p.color.a));
          break;
        case FilterPrimitive::Type::Blend:
          s += std::format("<feBlend{} mode=\"{}\"/>", io, blend_names[static_cast<std::size_t>(p.blend)]);
          break;
        case FilterPrimitive::Type::Composite:
          s += "<feComposite" + io + " operator=\"" + util::xml_escape(p.op, true) + "\"";
          if (p.op == "arithmetic") {
            s += std::format(" k1=\"{}\" k2=\"{}\" k3=\"{}\" k4=\"{}\"", util::format_double(p.k1),
                             util::format_double(p.k2), util::format_double(p.k3), util::format_double(p.k4));
          }
          s += "/>";
          break;
      }
      nl(s);
    }
    indent(s, 2);
    s += "</filter>";
    nl(s);
    defs_ += s;
    return id;
  }

  // ------------------------------------------------------------------ attributes
  Result<std::string> paint_attrs(const Paint& p, std::string_view prefix, bool is_fill) {
    std::string s;
    const auto value = [&](const std::string& v) { s += std::format(" {}=\"{}\"", prefix, v); };
    double opacity = p.opacity;
    switch (p.type) {
      case Paint::Type::None:
        if (is_fill) value("none");
        return s;
      case Paint::Type::Solid:
        if (!(is_fill && p.color.r == 0 && p.color.g == 0 && p.color.b == 0)) value(color(p.color));
        opacity *= p.color.a;
        break;
      case Paint::Type::Gradient:
        if (p.gradient) {
          CVT_TRY_ASSIGN(const std::string id, gradient(p.gradient));
          value("url(#" + id + ")");
        } else {
          value(color(p.color));
        }
        break;
      case Paint::Type::Pattern:
        if (p.pattern) {
          CVT_TRY_ASSIGN(const std::string id, pattern(p.pattern));
          value("url(#" + id + ")");
        } else {
          value("none");
        }
        break;
    }
    if (opacity < 1) s += std::format(" {}-opacity=\"{}\"", prefix, num(std::clamp(opacity, 0.0, 1.0)));
    return s;
  }

  Result<std::string> stroke_attrs(const Stroke& st) {
    if (!st.paint.visible()) return std::string{};
    CVT_TRY_ASSIGN(std::string s, paint_attrs(st.paint, "stroke", false));
    const double width = st.hairline ? 1.0 : st.width;
    if (width != 1) s += " stroke-width=\"" + num(width) + "\"";
    if (st.cap == LineCap::Round) s += " stroke-linecap=\"round\"";
    if (st.cap == LineCap::Square) s += " stroke-linecap=\"square\"";
    if (st.join == LineJoin::Round) s += " stroke-linejoin=\"round\"";
    if (st.join == LineJoin::Bevel) s += " stroke-linejoin=\"bevel\"";
    if (st.join == LineJoin::MiterClip) s += " stroke-linejoin=\"miter-clip\"";
    if (st.join == LineJoin::Arcs) s += " stroke-linejoin=\"arcs\"";
    if (st.miter_limit != 4 && (st.join == LineJoin::Miter || st.join == LineJoin::MiterClip)) {
      s += " stroke-miterlimit=\"" + num(st.miter_limit) + "\"";
    }
    if (!st.dash.empty()) {
      s += " stroke-dasharray=\"";
      for (std::size_t i = 0; i < st.dash.size(); ++i) {
        if (i) s += ' ';
        s += num(st.dash[i]);
      }
      s += "\"";
      if (st.dash_offset != 0) s += " stroke-dashoffset=\"" + num(st.dash_offset) + "\"";
    }
    if (st.non_scaling || st.hairline) s += " vector-effect=\"non-scaling-stroke\"";
    return s;
  }

  std::string id_attr(const std::string& id) const {
    if (id.empty() || o_.minify || !o_.keep_ids) return {};
    if (!util::is_valid_utf8(id)) return {};
    return " id=\"" + util::xml_escape(id, true) + "\"";
  }

  // ------------------------------------------------------------------ nodes
  Result<void> node(const Node& n, std::string& out, int level) {
    if (++count_ % 4096 == 0) CVT_TRY(ctx_.checkpoint(0.5, "Writing SVG"));
    if (const auto* s = std::get_if<Shape>(&n.value)) return shape(*s, out, level);
    if (const auto* t = std::get_if<Text>(&n.value)) return text(*t, out, level);
    if (const auto* p = std::get_if<Picture>(&n.value)) return picture(*p, out, level);
    if (const auto* g = std::get_if<Group>(&n.value)) return group(*g, out, level);
    return {};
  }

  Result<void> shape(const Shape& s, std::string& out, int level) {
    if (s.path.empty() || (!s.fill.visible() && !s.stroke.paint.visible())) return {};
    CVT_TRY_ASSIGN(const std::string fill, paint_attrs(s.fill, "fill", true));
    CVT_TRY_ASSIGN(const std::string stroke, stroke_attrs(s.stroke));
    indent(out, level);
    out += "<path" + id_attr(s.id) + " d=\"" + path_data(s.path) + "\"" + fill;
    if (s.fill_rule == FillRule::EvenOdd && s.fill.visible()) out += " fill-rule=\"evenodd\"";
    out += stroke;
    if (s.stroke_first && s.stroke.paint.visible() && s.fill.visible()) out += " paint-order=\"stroke\"";
    out += "/>";
    nl(out);
    return {};
  }

  Result<void> text(const Text& t, std::string& out, int level) {
    if (!t.fill.visible() && !t.stroke.paint.visible()) return {};
    bool real = o_.text_as_text && !t.content.empty() && t.advance > 0 && util::is_valid_utf8(t.content);
    if (real) {
      // Text whose layout a viewer could not reproduce (letter spacing, glyphs
      // placed individually) is written as outlines.
      const double natural = layout_text(t.content, t.font).advance;
      if (std::abs(natural - t.advance) > 0.02 * std::max(1.0, t.advance)) real = false;
    }
    CVT_TRY_ASSIGN(const std::string fill, paint_attrs(t.fill, "fill", true));
    CVT_TRY_ASSIGN(const std::string stroke, stroke_attrs(t.stroke));
    indent(out, level);
    if (!real) {
      if (t.outline.empty()) return {};
      out += "<path" + id_attr(t.id) + " d=\"" + path_data(t.outline) + "\"" + fill + stroke + "/>";
      nl(out);
      return {};
    }
    out += std::format("<text{} x=\"{}\" y=\"{}\" font-family=\"{}\" font-size=\"{}\"", id_attr(t.id), num(t.origin.x),
                       num(t.origin.y), util::xml_escape(t.font.family, true), num(t.font.size));
    if (t.font.weight != 400) out += std::format(" font-weight=\"{}\"", t.font.weight);
    if (t.font.italic) out += " font-style=\"italic\"";
    const bool spaces = t.content.find("  ") != std::string::npos || t.content.starts_with(' ') || t.content.ends_with(' ');
    if (spaces) out += " xml:space=\"preserve\"";
    out += fill + stroke + ">" + util::xml_escape(t.content) + "</text>";
    nl(out);
    return {};
  }

  Result<void> picture(const Picture& p, std::string& out, int level) {
    if (p.bounds.empty()) return {};
    std::string mime;
    Bytes data;
    if (!p.encoded.empty() && (p.encoded_format == "png" || p.encoded_format == "jpg" || p.encoded_format == "gif")) {
      mime = p.encoded_format == "jpg" ? "image/jpeg" : "image/" + p.encoded_format;
      data = p.encoded;
    } else if (p.pixels && p.pixels->valid()) {
      ir::Image image;
      image.frames.push_back(*p.pixels);
      auto encoded = image::encode(o_.image_format, image, Options{}, ctx_);
      if (!encoded) {
        ctx_.warn(std::format("A picture could not be embedded: {}", encoded.error().message));
        return {};
      }
      data = std::move(*encoded);
      mime = o_.image_format == "jpg" ? "image/jpeg" : "image/" + o_.image_format;
    } else {
      return {};
    }
    uses_xlink_ = true;
    indent(out, level);
    out += std::format("<image{} x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" preserveAspectRatio=\"none\"", id_attr(p.id),
                       num(p.bounds.x0), num(p.bounds.y0), num(p.bounds.width()), num(p.bounds.height()));
    if (!p.smooth) out += " image-rendering=\"pixelated\"";
    out += " xlink:href=\"data:" + mime + ";base64," + util::base64_encode(data) + "\"/>";
    nl(out);
    return {};
  }

  Result<void> group_content(const Group& g, std::string& out, int level, bool top) {
    (void)top;
    for (const Node& c : g.children) CVT_TRY(node(c, out, level));
    return {};
  }

  Result<void> group(const Group& g, std::string& out, int level) {
    if (++depth_ > 500) return fail(Status::Limit, "the drawing is nested too deeply for SVG");
    std::string attrs;
    if (!g.transform.is_identity(1e-12)) attrs += " transform=\"" + matrix(g.transform) + "\"";
    if (g.opacity < 1) attrs += " opacity=\"" + num(std::clamp(g.opacity, 0.0, 1.0)) + "\"";
    if (g.blend != BlendMode::Normal) {
      static constexpr std::string_view names[] = {
          "normal",     "multiply",   "screen",     "overlay",   "darken", "lighten",    "color-dodge", "color-burn",
          "hard-light", "soft-light", "difference", "exclusion", "hue",    "saturation", "color",       "luminosity"};
      attrs += std::format(" style=\"mix-blend-mode:{}\"", names[static_cast<std::size_t>(g.blend)]);
    }
    if (g.clip) {
      CVT_TRY_ASSIGN(const std::string id, clip(g.clip));
      attrs += " clip-path=\"url(#" + id + ")\"";
    }
    if (g.mask) {
      CVT_TRY_ASSIGN(const std::string id, mask(g.mask));
      attrs += " mask=\"url(#" + id + ")\"";
    }
    if (g.filter) {
      CVT_TRY_ASSIGN(const std::string id, filter(g.filter));
      attrs += " filter=\"url(#" + id + ")\"";
    }
    if (g.layer) {
      uses_inkscape_ = true;
      attrs += " inkscape:groupmode=\"layer\"";
      if (!g.layer_name.empty()) attrs += " inkscape:label=\"" + util::xml_escape(util::sanitize_utf8(g.layer_name), true) + "\"";
    }
    if (!g.visible) attrs += " display=\"none\"";
    attrs += id_attr(g.id);
    // A plain group adds nothing: write its children in place.
    if (attrs.empty()) {
      for (const Node& c : g.children) CVT_TRY(node(c, out, level));
      --depth_;
      return {};
    }
    std::string inner;
    for (const Node& c : g.children) CVT_TRY(node(c, inner, level + 1));
    --depth_;
    if (inner.empty() && !g.layer) return {};
    indent(out, level);
    out += "<g" + attrs + ">";
    nl(out);
    out += inner;
    indent(out, level);
    out += "</g>";
    nl(out);
    return {};
  }

  const Context& ctx_;
  WriteOptions o_;
  std::string defs_;
  std::map<const void*, std::string> ids_;
  std::map<std::string, std::string> clip_by_content_;
  std::uint32_t id_counter_ = 0;
  std::uint64_t count_ = 0;
  int depth_ = 0;
  bool uses_xlink_ = false;
  bool uses_inkscape_ = false;
};

}  // namespace

Result<std::string> write_svg(const Scene& scene, const Context& ctx, const WriteOptions& options, std::size_t page) {
  Writer writer(ctx, options);
  return writer.run(scene, page);
}

}  // namespace cvt::vector::svg
