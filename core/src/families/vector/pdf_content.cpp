// Convertible core - vector family: a scene page as PDF page content (see pdf_content.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/vector/pdf_content.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

#include "families/vector/geom.hpp"
#include "families/vector/ps_emit.hpp"
#include "families/vector/render.hpp"
#include "util/deflate.hpp"

namespace cvt::vector::pdf {

namespace {

using emit::num;

constexpr double pt_per_px = 72.0 / 96.0;

/// "a b c d e f cm\n"
std::string cm_ops(const Matrix& m, int precision) {
  const std::string a = emit::matrix_array(m, precision);
  return a.substr(1, a.size() - 2) + " cm\n";
}

std::string_view blend_name(BlendMode b) {
  switch (b) {
    case BlendMode::Normal: return "Normal";
    case BlendMode::Multiply: return "Multiply";
    case BlendMode::Screen: return "Screen";
    case BlendMode::Overlay: return "Overlay";
    case BlendMode::Darken: return "Darken";
    case BlendMode::Lighten: return "Lighten";
    case BlendMode::ColorDodge: return "ColorDodge";
    case BlendMode::ColorBurn: return "ColorBurn";
    case BlendMode::HardLight: return "HardLight";
    case BlendMode::SoftLight: return "SoftLight";
    case BlendMode::Difference: return "Difference";
    case BlendMode::Exclusion: return "Exclusion";
    case BlendMode::Hue: return "Hue";
    case BlendMode::Saturation: return "Saturation";
    case BlendMode::Color: return "Color";
    case BlendMode::Luminosity: return "Luminosity";
  }
  return "Normal";
}

class Builder {
 public:
  Builder(const Context& ctx, const PdfContentOptions& o) : ctx_(ctx), o_(o) {}

  Result<PdfPageContent> run(const Page& page);

 private:
  const Context& ctx_;
  PdfContentOptions o_;
  PdfPageContent page_;
  std::map<std::string, std::string> gs_cache_;
  std::map<std::string, int> counters_;
  Matrix base_;  // scene px -> PDF default user space
  Matrix acc_;   // current user space -> scene page px
  Rect page_px_;
  bool warned_filter_ = false;

  std::string p(double v) const { return num(v, o_.precision); }
  std::size_t add_object(std::string dict, std::optional<Bytes> stream = std::nullopt) {
    page_.objects.push_back(PdfObject{std::move(dict), std::move(stream)});
    return page_.objects.size() - 1;
  }
  static std::string ref(std::size_t index) { return "{obj " + std::to_string(index) + "}"; }
  std::string add_resource(const std::string& category, const std::string& prefix, std::string value) {
    const std::string name = prefix + std::to_string(counters_[prefix]++);
    page_.resources.push_back(PdfResource{category, name, std::move(value)});
    return name;
  }
  std::string ext_gstate(double fill_alpha, double stroke_alpha, BlendMode blend, std::optional<std::string> smask) {
    std::string d = "<< /Type /ExtGState";
    if (fill_alpha < 1) d += " /ca " + num(fill_alpha, 4);
    if (stroke_alpha < 1) d += " /CA " + num(stroke_alpha, 4);
    if (blend != BlendMode::Normal) d += " /BM /" + std::string(blend_name(blend));
    if (smask) d += " /SMask " + *smask;
    d += " >>";
    if (const auto it = gs_cache_.find(d); it != gs_cache_.end()) return it->second;
    const std::string name = add_resource("ExtGState", "GS", d);
    gs_cache_.emplace(d, name);
    return name;
  }
  /// The current page rectangle in the current user space (for unbounded forms).
  Rect user_page_rect() const {
    if (!acc_.invertible()) return page_px_;
    return transform_rect(page_px_, acc_.inverted());
  }

  void path(std::string& out, const Path& path);
  void color(std::string& out, const Color& c, bool stroke);
  std::string function_of(const std::vector<GradientStop>& stops, int repeats, bool reflect, bool alpha);
  std::string shading(const Gradient& g, bool alpha);
  bool fill(std::string& out, const Path& path, FillRule rule, const Paint& paint);
  void stroke(std::string& out, const Path& path, const Stroke& st);
  void pattern_paint(std::string& out, const PatternPaint& pp, bool stroking);
  void node(std::string& out, const Node& n);
  void group(std::string& out, const Group& g);
  void group_children(std::string& out, const Group& g);
  void clip(std::string& out, const ClipPath& c);
  std::size_t image_object(const ir::Frame& frame, bool smooth);
  void picture(std::string& out, const Picture& pic);
  bool raster_group(std::string& out, const Group& g);
  Result<void> check();
};

void Builder::path(std::string& out, const Path& path) {
  std::size_t pi = 0;
  Point last{};
  for (const Verb v : path.verbs) {
    switch (v) {
      case Verb::Move:
        last = path.points[pi++];
        out += p(last.x) + " " + p(last.y) + " m\n";
        break;
      case Verb::Line:
        last = path.points[pi++];
        out += p(last.x) + " " + p(last.y) + " l\n";
        break;
      case Verb::Quad: {
        const Point c = path.points[pi], e = path.points[pi + 1];
        pi += 2;
        const Point c1 = last + (c - last) * (2.0 / 3.0), c2 = e + (c - e) * (2.0 / 3.0);
        out += p(c1.x) + " " + p(c1.y) + " " + p(c2.x) + " " + p(c2.y) + " " + p(e.x) + " " + p(e.y) + " c\n";
        last = e;
        break;
      }
      case Verb::Cubic: {
        const Point a = path.points[pi], b = path.points[pi + 1], e = path.points[pi + 2];
        pi += 3;
        out += p(a.x) + " " + p(a.y) + " " + p(b.x) + " " + p(b.y) + " " + p(e.x) + " " + p(e.y) + " c\n";
        last = e;
        break;
      }
      case Verb::Close: out += "h\n"; break;
    }
  }
}

void Builder::color(std::string& out, const Color& c, bool stroke) {
  if (c.r == c.g && c.g == c.b) out += num(c.r, 4) + (stroke ? " G\n" : " g\n");
  else out += num(c.r, 4) + " " + num(c.g, 4) + " " + num(c.b, 4) + (stroke ? " RG\n" : " rg\n");
}

std::string Builder::function_of(const std::vector<GradientStop>& stops_in, int repeats, bool reflect, bool alpha) {
  std::vector<GradientStop> stops;
  for (int r = 0; r < repeats; ++r) {
    for (std::size_t i = 0; i < stops_in.size(); ++i) {
      GradientStop s = (reflect && r % 2 == 1) ? stops_in[stops_in.size() - 1 - i] : stops_in[i];
      if (reflect && r % 2 == 1) s.offset = 1.0 - s.offset;
      s.offset += r;
      stops.push_back(s);
    }
  }
  auto comps = [&](const Color& c) {
    return alpha ? num(c.a, 4) : num(c.r, 4) + " " + num(c.g, 4) + " " + num(c.b, 4);
  };
  auto seg = [&](const Color& a, const Color& b) {
    return "<< /FunctionType 2 /Domain [0 1] /C0 [" + comps(a) + "] /C1 [" + comps(b) + "] /N 1 >>";
  };
  if (stops.size() == 2) return seg(stops[0].color, stops[1].color);
  std::string fns, bounds, encode;
  for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
    fns += seg(stops[i].color, stops[i + 1].color) + " ";
    encode += "0 1 ";
    if (i + 2 < stops.size()) bounds += num(stops[i + 1].offset, 6) + " ";
  }
  return "<< /FunctionType 3 /Domain [0 " + std::to_string(repeats) + "] /Functions [" + fns + "] /Bounds [" + bounds +
         "] /Encode [" + encode + "] >>";
}

std::string Builder::shading(const Gradient& g, bool alpha) {
  const std::vector<GradientStop> stops = emit::normalized_stops(g);
  const std::string cs = alpha ? "/DeviceGray" : "/DeviceRGB";
  if (g.type == Gradient::Type::Linear) {
    Point p1 = g.p1, p2 = g.p2;
    int repeats = 1;
    if (g.spread != Spread::Pad) {
      repeats = 9;
      const Point d = p2 - p1;
      p1 = p1 - d * 4.0;
      p2 = p2 + d * 4.0;
    }
    return "<< /ShadingType 2 /ColorSpace " + cs + " /Coords [" + p(p1.x) + " " + p(p1.y) + " " + p(p2.x) + " " +
           p(p2.y) + "] /Domain [0 " + std::to_string(repeats) + "] /Extend [true true] /Function " +
           function_of(stops, repeats, g.spread == Spread::Reflect, alpha) + " >>";
  }
  return "<< /ShadingType 3 /ColorSpace " + cs + " /Coords [" + p(g.focal.x) + " " + p(g.focal.y) + " " +
         p(std::max(0.0, g.focal_radius)) + " " + p(g.center.x) + " " + p(g.center.y) + " " +
         p(std::max(0.0, g.radius)) + "] /Extend [true true] /Function " + function_of(stops, 1, false, alpha) + " >>";
}

void Builder::pattern_paint(std::string& out, const PatternPaint& pp, bool stroking) {
  std::string content;
  if (pp.content) {
    const Matrix saved = acc_;
    acc_ = acc_ * pp.transform;
    group_children(content, *pp.content);
    acc_ = saved;
  }
  // Pattern matrices map pattern space to the page's default user space.
  const Matrix m = base_ * acc_ * pp.transform;
  const std::size_t obj = add_object("<< /Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [" +
                                         p(pp.tile.x0) + " " + p(pp.tile.y0) + " " + p(pp.tile.x1) + " " +
                                         p(pp.tile.y1) + "] /XStep " + p(pp.tile.width()) + " /YStep " +
                                         p(pp.tile.height()) + " /Matrix " + emit::matrix_array(m, o_.precision) +
                                         " /Resources {resources} >>",
                                     to_bytes(content));
  const std::string name = add_resource("Pattern", "P", ref(obj));
  out += stroking ? "/Pattern CS /" + name + " SCN\n" : "/Pattern cs /" + name + " scn\n";
}

bool Builder::fill(std::string& out, const Path& pth, FillRule rule, const Paint& paint) {
  if (!paint.visible() || pth.empty()) return false;
  const std::string op = rule == FillRule::EvenOdd ? "f*" : "f";
  switch (paint.type) {
    case Paint::Type::Solid: {
      const double alpha = paint.opacity * paint.color.a;
      out += "q\n";
      if (alpha < 0.999) out += "/" + ext_gstate(alpha, 1, BlendMode::Normal, std::nullopt) + " gs\n";
      color(out, paint.color, false);
      path(out, pth);
      out += op + "\nQ\n";
      return true;
    }
    case Paint::Type::Gradient: {
      if (!paint.gradient) return false;
      const Gradient& g = *paint.gradient;
      out += "q\n";
      path(out, pth);
      out += rule == FillRule::EvenOdd ? "W* n\n" : "W n\n";
      bool translucent = paint.opacity < 0.999;
      for (const GradientStop& s : g.stops) translucent = translucent || s.color.a < 0.999f;
      if (translucent) {
        std::optional<std::string> smask;
        bool stop_alpha = false;
        for (const GradientStop& s : g.stops) stop_alpha = stop_alpha || s.color.a < 0.999f;
        if (stop_alpha) {
          // Luminosity soft mask painted with the stop alphas as grey levels.
          const std::string ash = add_resource("Shading", "Sh", shading(g, true));
          const Rect r = user_page_rect();
          const std::string form_content =
              (g.transform.is_identity() ? std::string() : cm_ops(g.transform, 9)) + "/" + ash + " sh\n";
          const std::size_t form = add_object(
              "<< /Type /XObject /Subtype /Form /BBox [" + p(r.x0) + " " + p(r.y0) + " " + p(r.x1) + " " + p(r.y1) +
                  "] /Group << /S /Transparency /CS /DeviceGray >> /Resources {resources} >>",
              to_bytes(form_content));
          smask = "<< /Type /Mask /S /Luminosity /G " + ref(form) + " >>";
        }
        out += "/" + ext_gstate(paint.opacity, 1, BlendMode::Normal, smask) + " gs\n";
      }
      if (!g.transform.is_identity()) out += cm_ops(g.transform, 9);
      const std::string sh = add_resource("Shading", "Sh", shading(g, false));
      out += "/" + sh + " sh\nQ\n";
      return true;
    }
    case Paint::Type::Pattern: {
      if (!paint.pattern) return false;
      out += "q\n";
      if (paint.opacity < 0.999) out += "/" + ext_gstate(paint.opacity, 1, BlendMode::Normal, std::nullopt) + " gs\n";
      pattern_paint(out, *paint.pattern, false);
      path(out, pth);
      out += op + "\nQ\n";
      return true;
    }
    case Paint::Type::None: return false;
  }
  return false;
}

void Builder::stroke(std::string& out, const Path& pth, const Stroke& st) {
  if (!st.paint.visible() || pth.empty()) return;
  out += "q\n";
  double width = st.width;
  if (st.non_scaling) width = st.width / std::max(1e-9, acc_.mean_scale());
  out += st.hairline ? std::string("0 w ") : p(width) + " w ";
  out += std::to_string(st.cap == LineCap::Round ? 1 : (st.cap == LineCap::Square ? 2 : 0)) + " J ";
  const int join = st.join == LineJoin::Round || st.join == LineJoin::Arcs ? 1 : (st.join == LineJoin::Bevel ? 2 : 0);
  out += std::to_string(join) + " j " + p(std::max(1.0, st.miter_limit)) + " M ";
  if (!st.dash.empty()) {
    out += "[";
    for (std::size_t i = 0; i < st.dash.size(); ++i) out += (i ? " " : "") + p(st.dash[i]);
    out += "] " + p(st.dash_offset) + " d\n";
  } else {
    out += "[] 0 d\n";
  }
  switch (st.paint.type) {
    case Paint::Type::Solid: {
      const double alpha = st.paint.opacity * st.paint.color.a;
      if (alpha < 0.999) out += "/" + ext_gstate(1, alpha, BlendMode::Normal, std::nullopt) + " gs\n";
      color(out, st.paint.color, true);
      path(out, pth);
      out += "S\n";
      break;
    }
    case Paint::Type::Gradient: {
      // Outline the stroke and fill it with the gradient.
      const Path outline = stroke_outline(pth, st, 0.05);
      Paint pnt = st.paint;
      out += "Q\n";
      fill(out, outline, FillRule::NonZero, pnt);
      return;
    }
    case Paint::Type::Pattern:
      if (st.paint.pattern) {
        if (st.paint.opacity < 0.999) out += "/" + ext_gstate(1, st.paint.opacity, BlendMode::Normal, std::nullopt) + " gs\n";
        pattern_paint(out, *st.paint.pattern, true);
        path(out, pth);
        out += "S\n";
      }
      break;
    case Paint::Type::None: break;
  }
  out += "Q\n";
}

void Builder::clip(std::string& out, const ClipPath& c) {
  if (c.clip) clip(out, *c.clip);
  if (c.items.empty()) {
    out += "0 0 0 0 re W n\n";
    return;
  }
  bool even_odd = true;
  for (const ClipPath::Item& item : c.items) {
    path(out, item.path);
    even_odd = even_odd && item.rule == FillRule::EvenOdd;
  }
  out += even_odd ? "W* n\n" : "W n\n";
}

std::size_t Builder::image_object(const ir::Frame& frame, bool smooth) {
  const std::string w = std::to_string(frame.width), h = std::to_string(frame.height);
  const std::string interp = smooth ? " /Interpolate true" : "";
  std::string smask;
  if (frame.has_transparency()) {
    Bytes alpha;
    alpha.reserve(static_cast<std::size_t>(frame.width) * frame.height);
    for (std::size_t i = 3; i < frame.rgba.size(); i += 4) alpha.push_back(std::byte{frame.rgba[i]});
    auto z = util::zlib_compress(alpha, o_.compression_level);
    const std::size_t sm = add_object("<< /Type /XObject /Subtype /Image /Width " + w + " /Height " + h +
                                          " /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode" +
                                          interp + " >>",
                                      z ? std::move(*z) : alpha);
    smask = " /SMask " + ref(sm);
  }
  // Colour samples (straight, not composited: the SMask carries alpha).
  bool gray = true;
  const std::size_t n = static_cast<std::size_t>(frame.width) * frame.height;
  for (std::size_t i = 0; i < n && gray; ++i) {
    const std::uint8_t* px = frame.rgba.data() + i * 4;
    gray = px[0] == px[1] && px[1] == px[2];
  }
  Bytes samples;
  samples.reserve(n * (gray ? 1 : 3));
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t* px = frame.rgba.data() + i * 4;
    for (int k = 0; k < (gray ? 1 : 3); ++k) samples.push_back(std::byte{px[k]});
  }
  auto z = util::zlib_compress(samples, o_.compression_level);
  return add_object("<< /Type /XObject /Subtype /Image /Width " + w + " /Height " + h + " /ColorSpace " +
                        (gray ? "/DeviceGray" : "/DeviceRGB") + " /BitsPerComponent 8 /Filter /FlateDecode" + interp +
                        smask + " >>",
                    z ? std::move(*z) : samples);
}

void Builder::picture(std::string& out, const Picture& pic) {
  if (pic.bounds.empty()) return;
  std::size_t obj = 0;
  std::uint32_t jw = 0, jh = 0;
  const int jcomps = pic.encoded_format == "jpg" ? emit::jpeg_components(pic.encoded, &jw, &jh) : 0;
  const bool opaque = !pic.pixels || !pic.pixels->has_transparency();
  if ((jcomps == 1 || jcomps == 3) && opaque && jw > 0 && jh > 0) {
    obj = add_object("<< /Type /XObject /Subtype /Image /Width " + std::to_string(jw) + " /Height " +
                         std::to_string(jh) + " /ColorSpace " + (jcomps == 1 ? "/DeviceGray" : "/DeviceRGB") +
                         " /BitsPerComponent 8 /Filter /DCTDecode" + (pic.smooth ? " /Interpolate true" : "") + " >>",
                     pic.encoded);
  } else {
    ir::Frame storage;
    const ir::Frame* frame = emit::picture_frame(pic, ctx_, storage);
    if (frame == nullptr) {
      ctx_.warn("a picture could not be decoded and was left out of the PDF content");
      return;
    }
    obj = image_object(*frame, pic.smooth);
  }
  const std::string name = add_resource("XObject", "Im", ref(obj));
  const Rect& b = pic.bounds;
  out += "q " + p(b.width()) + " 0 0 " + p(-b.height()) + " " + p(b.x0) + " " + p(b.y0 + b.height()) + " cm /" + name +
         " Do Q\n";
}

bool Builder::raster_group(std::string& out, const Group& g) {
  Group inner = g;
  inner.transform = Matrix{};
  const Rect b = emit::node_bounds(Node{inner}, Matrix{});
  if (b.empty() || b.width() <= 0 || b.height() <= 0) return true;
  const double scale = o_.raster_dpi / 96.0 * std::max(1e-6, acc_.mean_scale());
  const auto w = static_cast<std::uint32_t>(std::clamp(std::ceil(b.width() * scale), 1.0, 16384.0));
  const auto h = static_cast<std::uint32_t>(std::clamp(std::ceil(b.height() * scale), 1.0, 16384.0));
  if (!ctx_.limits.check_pixels(w, h)) return false;
  const Matrix to_px =
      Matrix::scale(static_cast<double>(w) / b.width(), static_cast<double>(h) / b.height()) * Matrix::translate(-b.x0, -b.y0);
  auto frame = render_group(inner, to_px, w, h, ctx_);
  if (!frame) return false;
  const std::size_t obj = image_object(*frame, true);
  const std::string name = add_resource("XObject", "Im", ref(obj));
  out += "q " + p(b.width()) + " 0 0 " + p(-b.height()) + " " + p(b.x0) + " " + p(b.y0 + b.height()) + " cm /" + name +
         " Do Q\n";
  return true;
}

void Builder::group_children(std::string& out, const Group& g) {
  for (const Node& n : g.children) node(out, n);
}

void Builder::group(std::string& out, const Group& g) {
  if (!g.visible) return;
  const Matrix saved = acc_;
  out += "q\n";
  if (!g.transform.is_identity()) out += cm_ops(g.transform, o_.precision);
  acc_ = acc_ * g.transform;
  if (g.clip) clip(out, *g.clip);
  bool done = false;
  if (g.filter) {
    done = raster_group(out, g);
    if (!done && !warned_filter_) {
      warned_filter_ = true;
      ctx_.warn("filter effects could not be rasterised for PDF; they were left out");
    }
  }
  if (!done) {
    const bool needs_form = g.opacity < 0.999 || g.blend != BlendMode::Normal || g.mask != nullptr;
    if (needs_form) {
      std::string inner;
      group_children(inner, g);
      Rect r = emit::group_content_bounds(g, Matrix{});
      if (r.empty()) r = user_page_rect();
      const std::size_t form = add_object("<< /Type /XObject /Subtype /Form /BBox [" + p(r.x0) + " " + p(r.y0) + " " +
                                              p(r.x1) + " " + p(r.y1) +
                                              "] /Group << /S /Transparency /CS /DeviceRGB >> /Resources {resources} >>",
                                          to_bytes(inner));
      std::optional<std::string> smask;
      if (g.mask && g.mask->content) {
        std::string mcontent;
        Rect mr = g.mask->region;
        if (!mr.empty()) mcontent += p(mr.x0) + " " + p(mr.y0) + " " + p(mr.width()) + " " + p(mr.height()) + " re W n\n";
        group_children(mcontent, *g.mask->content);
        if (mr.empty()) mr = user_page_rect();
        const std::size_t mform = add_object(
            "<< /Type /XObject /Subtype /Form /BBox [" + p(mr.x0) + " " + p(mr.y0) + " " + p(mr.x1) + " " + p(mr.y1) +
                "] /Group << /S /Transparency /CS /DeviceRGB >> /Resources {resources} >>",
            to_bytes(mcontent));
        smask = std::string("<< /Type /Mask /S /") + (g.mask->type == Mask::Type::Alpha ? "Alpha" : "Luminosity") +
                " /G " + ref(mform) + " >>";
      }
      const std::string gs = ext_gstate(g.opacity, g.opacity, g.blend, smask);
      const std::string fm = add_resource("XObject", "Fm", ref(form));
      out += "/" + gs + " gs /" + fm + " Do\n";
    } else {
      group_children(out, g);
    }
  }
  out += "Q\n";
  acc_ = saved;
}

void Builder::node(std::string& out, const Node& n) {
  if (const auto* g = std::get_if<Group>(&n.value)) {
    group(out, *g);
  } else if (const auto* s = std::get_if<Shape>(&n.value)) {
    if (s->stroke_first) stroke(out, s->path, s->stroke);
    fill(out, s->path, s->fill_rule, s->fill);
    if (!s->stroke_first) stroke(out, s->path, s->stroke);
  } else if (const auto* t = std::get_if<Text>(&n.value)) {
    fill(out, t->outline, FillRule::NonZero, t->fill);
    stroke(out, t->outline, t->stroke);
  } else if (const auto* pic = std::get_if<Picture>(&n.value)) {
    picture(out, *pic);
  }
}

Result<PdfPageContent> Builder::run(const Page& page) {
  if (!(page.width > 0 && page.height > 0)) return fail(Status::Encode, "the page has no size");
  page_.width_pt = page.width * pt_per_px;
  page_.height_pt = page.height * pt_per_px;
  page_px_ = Rect{0, 0, page.width, page.height};
  base_ = Matrix{pt_per_px, 0, 0, -pt_per_px, 0, page_.height_pt};
  acc_ = Matrix{};
  std::string out;
  out += cm_ops(base_, 6);
  group(out, page.root);
  if (out.size() > ctx_.limits.max_output_bytes) return fail(Status::Limit, "the PDF page content is too large");
  page_.content = to_bytes(out);
  return std::move(page_);
}

}  // namespace

std::string PdfPageContent::resources_dict() const {
  std::map<std::string, std::vector<const PdfResource*>> by_category;
  for (const PdfResource& r : resources) by_category[r.category].push_back(&r);
  std::string out = "<<";
  for (const auto& [category, list] : by_category) {
    out += " /" + category + " <<";
    for (const PdfResource* r : list) out += " /" + r->name + " " + r->value;
    out += " >>";
  }
  out += " >>";
  return out;
}

std::string resolve(std::string_view text, const std::function<std::string(std::size_t)>& ref,
                    std::string_view resources) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '{') {
      if (text.substr(i, 5) == "{obj ") {
        const std::size_t close = text.find('}', i);
        if (close != std::string_view::npos) {
          std::size_t index = 0;
          bool ok = close > i + 5;
          for (std::size_t k = i + 5; k < close && ok; ++k) {
            ok = text[k] >= '0' && text[k] <= '9';
            if (ok) index = index * 10 + static_cast<std::size_t>(text[k] - '0');
          }
          if (ok) {
            out += ref(index);
            i = close + 1;
            continue;
          }
        }
      } else if (text.substr(i, 11) == "{resources}") {
        out += resources;
        i += 11;
        continue;
      }
    }
    out.push_back(text[i++]);
  }
  return out;
}

Result<PdfPageContent> page_content(const Scene& scene, std::size_t page_index, const Context& ctx,
                                    const PdfContentOptions& options) {
  if (page_index >= scene.pages.size()) return fail(Status::Encode, "page {} does not exist", page_index + 1);
  Builder b(ctx, options);
  return b.run(scene.pages[page_index]);
}

}  // namespace cvt::vector::pdf
