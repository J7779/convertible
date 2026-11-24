// Convertible core - vector family: EPS and PostScript writer (DSC 3.0).
//
// Paths, fills (non-zero / even-odd), strokes, clipping, gradients (smooth
// shading), tiling patterns, pictures (Flate or LZW + ASCII85, JPEG copied
// through), text as outlines or as real text with the standard fonts.
// PostScript has no transparency: groups with opacity, masks, filters or
// blend modes and translucent paints are flattened - the page region they
// cover is rendered together with everything beneath it and placed as an
// opaque picture.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <string>

#include "engine/standard_options.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/ps_emit.hpp"
#include "families/vector/ps_internal.hpp"
#include "families/vector/ps_io.hpp"
#include "families/vector/render.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::vector {

namespace {

using emit::num;

constexpr double pt_per_px = 72.0 / 96.0;

/// Extra glyphs placed in the unused 128..159 slots of ISO Latin-1.
constexpr std::pair<int, char32_t> extra_slots[] = {
    {128, 0x2018}, {129, 0x2019}, {130, 0x201C}, {131, 0x201D}, {132, 0x2013}, {133, 0x2014},
    {134, 0x2022}, {135, 0x2026}, {136, 0x20AC}, {137, 0x2122}, {138, 0x2020}, {139, 0x2021},
    {140, 0x2030}, {141, 0x0152}, {142, 0x0153}, {143, 0x0160}, {144, 0x0161}, {145, 0x0178},
    {146, 0x017D}, {147, 0x017E}, {148, 0x0192}, {149, 0x2039}, {150, 0x203A}, {151, 0x201A},
    {152, 0x201E}, {153, 0xFB01}, {154, 0xFB02}, {155, 0x2212}};

std::optional<std::uint8_t> encode_char(char32_t cp) {
  if ((cp >= 32 && cp < 127) || (cp >= 160 && cp <= 255)) return static_cast<std::uint8_t>(cp);
  for (const auto& [slot, c] : extra_slots) {
    if (c == cp) return static_cast<std::uint8_t>(slot);
  }
  return std::nullopt;
}

std::string base_font_name(const FontSpec& font) {
  const std::string fam = util::to_lower(font.family);
  const bool bold = font.weight >= 600;
  const bool italic = font.italic;
  if (fam.find("mono") != std::string::npos || fam.find("courier") != std::string::npos ||
      fam.find("consol") != std::string::npos || fam.find("menlo") != std::string::npos) {
    return bold ? (italic ? "Courier-BoldOblique" : "Courier-Bold") : (italic ? "Courier-Oblique" : "Courier");
  }
  const bool serif = (fam.find("serif") != std::string::npos && fam.find("sans") == std::string::npos) ||
                     fam.find("times") != std::string::npos || fam.find("georgia") != std::string::npos ||
                     fam.find("garamond") != std::string::npos || fam.find("roman") != std::string::npos;
  if (serif) {
    return bold ? (italic ? "Times-BoldItalic" : "Times-Bold") : (italic ? "Times-Italic" : "Times-Roman");
  }
  return bold ? (italic ? "Helvetica-BoldOblique" : "Helvetica-Bold") : (italic ? "Helvetica-Oblique" : "Helvetica");
}

class Writer {
 public:
  Writer(const Scene& scene, WriteContext& ctx, const PsWriteOptions& opts) : scene_(scene), ctx_(ctx), o_(opts) {}

  Result<Bytes> run();

 private:
  const Scene& scene_;
  WriteContext& ctx_;
  PsWriteOptions o_;
  std::set<std::string> used_fonts_;     // base fonts for real text
  std::set<std::string> defined_fonts_;  // re-encoded fonts defined so far
  bool used_shading_ = false;
  bool used_flate_ = false;
  bool warned_flatten_ = false;
  const Page* page_ = nullptr;
  std::vector<std::size_t> path_;  // node indices from the page root
  Matrix acc_;                     // current user space -> page px

  std::string p(double v) const { return num(v, o_.precision); }
  void warn_once(bool& flag, std::string message) {
    if (flag) return;
    flag = true;
    ctx_.warn(std::move(message));
  }

  void path_ops(std::string& out, const Path& path);
  void group(std::string& out, const Group& g, bool in_proc);
  void node(std::string& out, const Node& n, bool in_proc);
  void clip(std::string& out, const ClipPath& c);
  void shape(std::string& out, const Shape& s, bool in_proc);
  void fill_with(std::string& out, const Path& path, FillRule rule, const Paint& paint, bool in_proc);
  void stroke_with(std::string& out, const Path& path, const Stroke& stroke, bool in_proc);
  void set_color(std::string& out, const Color& c);
  void gradient(std::string& out, const Gradient& g);
  std::string function_of(const std::vector<GradientStop>& stops, int repeats, bool reflect);
  void pattern(std::string& out, const PatternPaint& pp);
  void picture(std::string& out, const Picture& pic, bool in_proc);
  void image_data(std::string& out, const ir::Frame& frame, const Rect& box, bool smooth, bool in_proc,
                  const Bytes* jpeg);
  void text(std::string& out, const Text& t, bool in_proc);
  bool flatten(std::string& out, const Node& n);
  std::string page_body(const Page& page);
};

void Writer::path_ops(std::string& out, const Path& path) {
  std::size_t pi = 0;
  Point last{};
  int on_line = 0;
  auto sep = [&] {
    if (++on_line >= 6) {
      out += '\n';
      on_line = 0;
    } else {
      out += ' ';
    }
  };
  for (const Verb v : path.verbs) {
    switch (v) {
      case Verb::Move:
        last = path.points[pi++];
        out += p(last.x) + " " + p(last.y) + " m";
        break;
      case Verb::Line:
        last = path.points[pi++];
        out += p(last.x) + " " + p(last.y) + " l";
        break;
      case Verb::Quad: {
        const Point c = path.points[pi], e = path.points[pi + 1];
        pi += 2;
        const Point c1 = last + (c - last) * (2.0 / 3.0), c2 = e + (c - e) * (2.0 / 3.0);
        out += p(c1.x) + " " + p(c1.y) + " " + p(c2.x) + " " + p(c2.y) + " " + p(e.x) + " " + p(e.y) + " c";
        last = e;
        break;
      }
      case Verb::Cubic: {
        const Point a = path.points[pi], b = path.points[pi + 1], e = path.points[pi + 2];
        pi += 3;
        out += p(a.x) + " " + p(a.y) + " " + p(b.x) + " " + p(b.y) + " " + p(e.x) + " " + p(e.y) + " c";
        last = e;
        break;
      }
      case Verb::Close: out += "h"; break;
    }
    sep();
  }
  if (on_line != 0) out += '\n';
}

void Writer::set_color(std::string& out, const Color& c) {
  if (c.r == c.g && c.g == c.b) out += num(c.r, 4) + " g\n";
  else out += num(c.r, 4) + " " + num(c.g, 4) + " " + num(c.b, 4) + " rg\n";
}

std::string Writer::function_of(const std::vector<GradientStop>& stops_in, int repeats, bool reflect) {
  // A stitching function over [0, repeats] (one copy of the stops per period).
  std::vector<GradientStop> stops;
  for (int r = 0; r < repeats; ++r) {
    for (std::size_t i = 0; i < stops_in.size(); ++i) {
      GradientStop s = (reflect && r % 2 == 1) ? stops_in[stops_in.size() - 1 - i] : stops_in[i];
      if (reflect && r % 2 == 1) s.offset = 1.0 - s.offset;
      s.offset += r;
      stops.push_back(s);
    }
  }
  auto rgb = [&](const Color& c) { return num(c.r, 4) + " " + num(c.g, 4) + " " + num(c.b, 4); };
  auto seg = [&](const Color& a, const Color& b) {
    return "<< /FunctionType 2 /Domain [0 1] /C0 [" + rgb(a) + "] /C1 [" + rgb(b) + "] /N 1 >>";
  };
  if (stops.size() == 2) return seg(stops[0].color, stops[1].color);
  std::string fns, bounds, encode;
  for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
    fns += seg(stops[i].color, stops[i + 1].color) + "\n";
    encode += "0 1 ";
    if (i + 2 < stops.size()) bounds += num(stops[i + 1].offset, 6) + " ";
  }
  return "<< /FunctionType 3 /Domain [0 " + std::to_string(repeats) + "] /Functions [\n" + fns + "] /Bounds [" +
         bounds + "] /Encode [" + encode + "] >>";
}

void Writer::gradient(std::string& out, const Gradient& g) {
  used_shading_ = true;
  const std::vector<GradientStop> stops = emit::normalized_stops(g);
  std::string shading;
  if (g.type == Gradient::Type::Linear) {
    Point p1 = g.p1, p2 = g.p2;
    int repeats = 1;
    if (g.spread != Spread::Pad) {
      // Repeat the stops over enough periods to cover the area (approximately).
      repeats = 9;
      const Point d = p2 - p1;
      p1 = p1 - d * 4.0;
      p2 = p2 + d * 4.0;
    }
    std::string fn = function_of(stops, repeats, g.spread == Spread::Reflect);
    shading = "<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [" + p(p1.x) + " " + p(p1.y) + " " + p(p2.x) + " " +
              p(p2.y) + "] /Domain [0 " + std::to_string(repeats) + "] /Extend [true true] /Function " + fn + " >>";
    if (g.spread != Spread::Pad) {
      // With 9 periods centred on the original one, offset the domain so that period 4 is the original.
    }
  } else {
    if (g.spread != Spread::Pad) ctx_.warn("repeating radial gradients are drawn padded in PostScript");
    shading = "<< /ShadingType 3 /ColorSpace /DeviceRGB /Coords [" + p(g.focal.x) + " " + p(g.focal.y) + " " +
              p(std::max(0.0, g.focal_radius)) + " " + p(g.center.x) + " " + p(g.center.y) + " " +
              p(std::max(0.0, g.radius)) + "] /Extend [true true] /Function " + function_of(stops, 1, false) + " >>";
  }
  Color avg{0, 0, 0, 1};
  {
    double r = 0, gg = 0, b = 0;
    for (const GradientStop& s : stops) {
      r += s.color.r;
      gg += s.color.g;
      b += s.color.b;
    }
    const double n = static_cast<double>(stops.size());
    avg = Color{static_cast<float>(r / n), static_cast<float>(gg / n), static_cast<float>(b / n), 1};
  }
  const std::string m = g.transform.is_identity() ? std::string() : emit::matrix_array(g.transform, o_.precision) + " concat ";
  if (o_.level >= 3) {
    out += m + shading + " shfill\n";
  } else {
    out += "/shfill where { pop " + m + shading + " shfill } { ";
    std::string c;
    set_color(c, avg);
    c.pop_back();
    out += c + " clippath fill } ifelse\n";
  }
}

void Writer::pattern(std::string& out, const PatternPaint& pp) {
  std::string content;
  if (pp.content) {
    const Matrix saved = acc_;
    group(content, *pp.content, true);
    acc_ = saved;
  }
  out += "<< /PatternType 1 /PaintType 1 /TilingType 1 /BBox [" + p(pp.tile.x0) + " " + p(pp.tile.y0) + " " +
         p(pp.tile.x1) + " " + p(pp.tile.y1) + "] /XStep " + p(pp.tile.width()) + " /YStep " + p(pp.tile.height()) +
         "\n/PaintProc { pop\n" + content + "} >> " + emit::matrix_array(pp.transform, o_.precision) +
         " makepattern setpattern\n";
}

void Writer::fill_with(std::string& out, const Path& path, FillRule rule, const Paint& paint, bool in_proc) {
  if (!paint.visible() || path.empty()) return;
  const char* op = rule == FillRule::EvenOdd ? "F" : "f";
  switch (paint.type) {
    case Paint::Type::Solid:
      set_color(out, paint.color);
      path_ops(out, path);
      out += std::string(op) + "\n";
      break;
    case Paint::Type::Gradient:
      if (!paint.gradient) return;
      out += "q\n";
      path_ops(out, path);
      out += rule == FillRule::EvenOdd ? "W* n\n" : "W n\n";
      gradient(out, *paint.gradient);
      out += "Q\n";
      break;
    case Paint::Type::Pattern:
      if (!paint.pattern) return;
      out += "q\n";
      pattern(out, *paint.pattern);
      path_ops(out, path);
      out += std::string(op) + "\nQ\n";
      break;
    case Paint::Type::None: break;
  }
  (void)in_proc;
}

void Writer::stroke_with(std::string& out, const Path& path, const Stroke& st, bool in_proc) {
  if (!st.paint.visible() || path.empty()) return;
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
    case Paint::Type::Solid:
      set_color(out, st.paint.color);
      path_ops(out, path);
      out += "s\n";
      break;
    case Paint::Type::Gradient:
      if (!st.paint.gradient) return;
      out += "q\n";
      path_ops(out, path);
      out += "strokepath W n\n";
      gradient(out, *st.paint.gradient);
      out += "Q\n";
      break;
    case Paint::Type::Pattern:
      if (!st.paint.pattern) return;
      out += "q\n";
      pattern(out, *st.paint.pattern);
      path_ops(out, path);
      out += "s\nQ\n";
      break;
    case Paint::Type::None: break;
  }
  (void)in_proc;
}

void Writer::shape(std::string& out, const Shape& s, bool in_proc) {
  if (s.stroke_first) stroke_with(out, s.path, s.stroke, in_proc);
  fill_with(out, s.path, s.fill_rule, s.fill, in_proc);
  if (!s.stroke_first) stroke_with(out, s.path, s.stroke, in_proc);
}

void Writer::clip(std::string& out, const ClipPath& c) {
  if (c.clip) clip(out, *c.clip);
  if (c.items.empty()) {
    out += "0 0 m h W n\n";
    return;
  }
  bool all_even_odd = true;
  for (const ClipPath::Item& item : c.items) {
    path_ops(out, item.path);
    all_even_odd = all_even_odd && item.rule == FillRule::EvenOdd;
  }
  out += all_even_odd ? "W* n\n" : "W n\n";
}

void Writer::image_data(std::string& out, const ir::Frame& frame, const Rect& box, bool smooth, bool in_proc,
                        const Bytes* jpeg) {
  out += "q " + emit::matrix_array(Matrix{box.width(), 0, 0, box.height(), box.x0, box.y0}, o_.precision) + " concat\n";
  const std::string w = std::to_string(frame.width), h = std::to_string(frame.height);
  const std::string interp = smooth ? "true" : "false";
  if (jpeg != nullptr) {
    const int comps = emit::jpeg_components(*jpeg);
    const std::string space = comps == 1 ? "/DeviceGray" : "/DeviceRGB";
    const std::string decode = comps == 1 ? "[0 1]" : "[0 1 0 1 0 1]";
    if (in_proc) {
      out += w + " " + h + " " + interp + " " + decode + " " + space + " " + emit::ascii85(*jpeg) +
             " /DCTDecode cvt_simage\n";
    } else {
      out += w + " " + h + " " + interp + " " + decode + " " + space + " /DCTDecode cvt_image\n" +
             emit::ascii85(*jpeg) + "\n";
    }
    out += "Q\n";
    return;
  }
  int comps = 3;
  const Bytes samples = emit::picture_samples(frame, comps);
  Bytes packed;
  std::string filter;
  if (o_.level >= 3) {
    auto z = util::zlib_compress(samples, 9);
    if (z) {
      packed = std::move(*z);
      filter = "/FlateDecode";
      used_flate_ = true;
    }
  }
  if (filter.empty()) {
    packed = emit::lzw_encode(samples);
    filter = "/LZWDecode";
  }
  const std::string space = comps == 1 ? "/DeviceGray" : "/DeviceRGB";
  const std::string decode = comps == 1 ? "[0 1]" : "[0 1 0 1 0 1]";
  if (in_proc) {
    out += w + " " + h + " " + interp + " " + decode + " " + space + " " + emit::ascii85(packed) + " " + filter +
           " cvt_simage\n";
  } else {
    out += w + " " + h + " " + interp + " " + decode + " " + space + " " + filter + " cvt_image\n" +
           emit::ascii85(packed) + "\n";
  }
  out += "Q\n";
}

void Writer::picture(std::string& out, const Picture& pic, bool in_proc) {
  if (pic.bounds.empty()) return;
  std::uint32_t jw = 0, jh = 0;
  const int jcomps = pic.encoded_format == "jpg" ? emit::jpeg_components(pic.encoded, &jw, &jh) : 0;
  const bool opaque = !pic.pixels || !pic.pixels->has_transparency();
  if ((jcomps == 1 || jcomps == 3) && opaque && jw > 0 && jh > 0 && (!in_proc || pic.encoded.size() < 48000)) {
    ir::Frame size_only;
    size_only.width = jw;
    size_only.height = jh;
    image_data(out, size_only, pic.bounds, pic.smooth, in_proc, &pic.encoded);
    return;
  }
  ir::Frame storage;
  const ir::Frame* frame = emit::picture_frame(pic, ctx_, storage);
  if (frame == nullptr) {
    ctx_.warn("a picture could not be decoded and was left out");
    return;
  }
  image_data(out, *frame, pic.bounds, pic.smooth, in_proc, nullptr);
}

void Writer::text(std::string& out, const Text& t, bool in_proc) {
  if (o_.text_as_text && t.fill.type == Paint::Type::Solid && !t.content.empty() &&
      t.stroke.paint.type == Paint::Type::None) {
    std::string encoded;
    bool ok = true;
    for (const char32_t cp : util::utf8_to_utf32(t.content)) {
      const auto c = encode_char(cp);
      if (!c) {
        ok = false;
        break;
      }
      encoded.push_back(static_cast<char>(*c));
    }
    if (ok) {
      const std::string base = base_font_name(t.font);
      used_fonts_.insert(base);
      const std::string fname = "Cvt-" + base;
      if (!defined_fonts_.contains(fname)) {
        out += "/" + fname + " /" + base + " cvt_reencode\n";
        defined_fonts_.insert(fname);
      }
      set_color(out, t.fill.color);
      out += "q [1 0 0 -1 " + p(t.origin.x) + " " + p(t.origin.y) + "] concat /" + fname + " " + p(t.font.size) +
             " selectfont 0 0 moveto " + emit::ps_string(encoded) + " show Q\n";
      return;
    }
  }
  if (t.outline.empty()) return;
  fill_with(out, t.outline, FillRule::NonZero, t.fill, in_proc);
  stroke_with(out, t.outline, t.stroke, in_proc);
}

bool Writer::flatten(std::string& out, const Node& n) {
  // Render the page region covered by `n` with everything beneath it.
  const Rect page_rect{0, 0, page_->width, page_->height};
  Rect region = emit::node_bounds(n, acc_).intersect(page_rect);
  if (region.empty() || region.width() <= 0 || region.height() <= 0) return true;
  const double scale = o_.raster_dpi / 96.0;
  region = Rect{std::floor(region.x0), std::floor(region.y0), std::ceil(region.x1), std::ceil(region.y1)}.intersect(
      Rect{0, 0, std::ceil(page_->width), std::ceil(page_->height)});
  const auto w = static_cast<std::uint32_t>(std::max(1.0, std::ceil(region.width() * scale)));
  const auto h = static_cast<std::uint32_t>(std::max(1.0, std::ceil(region.height() * scale)));
  if (!ctx_.limits.check_pixels(w, h)) return false;
  const Group prefix = emit::prefix_copy(page_->root, path_);
  const Matrix to_pixels = Matrix::scale(static_cast<double>(w) / region.width(), static_cast<double>(h) / region.height()) *
                           Matrix::translate(-region.x0, -region.y0);
  auto frame = render_group(prefix, to_pixels, w, h, ctx_);
  if (!frame) return false;
  warn_once(warned_flatten_,
            "PostScript has no transparency: translucent parts, masks, filters and blend modes were rasterised");
  // Place it in page space whatever the current transformation is.
  out += "q " + emit::matrix_array(acc_.inverted(), 9) + " concat\n";
  image_data(out, *frame, region, true, false, nullptr);
  out += "Q\n";
  return true;
}

void Writer::node(std::string& out, const Node& n, bool in_proc) {
  const bool needs_flatten = [&] {
    if (const auto* g = std::get_if<Group>(&n.value)) return g->visible && emit::group_needs_effects(*g);
    return emit::node_translucent(n);
  }();
  if (needs_flatten && !in_proc && page_ != nullptr) {
    if (flatten(out, n)) return;
    ctx_.warn("transparency could not be rasterised; it is drawn opaque");
  }
  if (const auto* g = std::get_if<Group>(&n.value)) {
    group(out, *g, in_proc);
  } else if (const auto* s = std::get_if<Shape>(&n.value)) {
    shape(out, *s, in_proc);
  } else if (const auto* t = std::get_if<Text>(&n.value)) {
    text(out, *t, in_proc);
  } else if (const auto* pic = std::get_if<Picture>(&n.value)) {
    picture(out, *pic, in_proc);
  }
}

void Writer::group(std::string& out, const Group& g, bool in_proc) {
  if (!g.visible) return;
  const bool wrap = !g.transform.is_identity() || g.clip != nullptr;
  const Matrix saved = acc_;
  if (wrap) {
    out += "q\n";
    if (!g.transform.is_identity()) out += emit::matrix_array(g.transform, o_.precision) + " concat\n";
    if (g.clip) clip(out, *g.clip);
  }
  acc_ = acc_ * g.transform;
  for (std::size_t i = 0; i < g.children.size(); ++i) {
    if (!in_proc) path_.push_back(i);
    node(out, g.children[i], in_proc);
    if (!in_proc) path_.pop_back();
    if (out.size() > ctx_.limits.max_output_bytes) return;
  }
  acc_ = saved;
  if (wrap) out += "Q\n";
}

std::string Writer::page_body(const Page& page) {
  page_ = &page;
  std::string out;
  // Scene px (y down) -> PostScript points (y up).
  const Matrix to_ps{pt_per_px, 0, 0, -pt_per_px, 0, page.height * pt_per_px};
  out += emit::matrix_array(to_ps, 6) + " concat\n";
  acc_ = Matrix{};
  path_.clear();
  // The root group: its own transform/clip/effects.
  const Group& root = page.root;
  if (root.visible) {
    if (emit::group_needs_effects(root)) {
      Node wrapper{root};
      // A root with effects is flattened as a whole.
      path_.clear();
      Group outer;
      outer.children.push_back(wrapper);
      const Page* saved = page_;
      Page tmp = page;
      tmp.root = outer;
      page_ = &tmp;
      path_.push_back(0);
      if (!flatten(out, wrapper)) group(out, root, false);
      path_.clear();
      page_ = saved;
    } else {
      // Walk the root like any group but keep node paths relative to it.
      const bool wrap = !root.transform.is_identity() || root.clip != nullptr;
      if (wrap) {
        out += "q\n";
        if (!root.transform.is_identity()) out += emit::matrix_array(root.transform, o_.precision) + " concat\n";
        if (root.clip) clip(out, *root.clip);
      }
      acc_ = root.transform;
      for (std::size_t i = 0; i < root.children.size(); ++i) {
        path_ = {i};
        node(out, root.children[i], false);
      }
      if (wrap) out += "Q\n";
    }
  }
  page_ = nullptr;
  return out;
}

Result<Bytes> Writer::run() {
  if (scene_.pages.empty()) return fail(Status::Encode, "the drawing has no page");
  std::size_t page_count = scene_.pages.size();
  if (o_.eps && page_count > 1) {
    ctx_.warn("EPS holds one page; only the first page was written");
    page_count = 1;
  }
  std::vector<std::string> bodies;
  double max_w = 0, max_h = 0;
  for (std::size_t i = 0; i < page_count; ++i) {
    CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(page_count), "Writing PostScript"));
    const Page& pg = scene_.pages[i];
    if (!(pg.width > 0 && pg.height > 0)) return fail(Status::Encode, "page {} has no size", i + 1);
    bodies.push_back(page_body(pg));
    max_w = std::max(max_w, pg.width * pt_per_px);
    max_h = std::max(max_h, pg.height * pt_per_px);
  }

  std::string out;
  out += o_.eps ? "%!PS-Adobe-3.0 EPSF-3.0\n" : "%!PS-Adobe-3.0\n";
  out += std::format("%%BoundingBox: 0 0 {} {}\n", static_cast<long long>(std::ceil(max_w - 1e-9)),
                     static_cast<long long>(std::ceil(max_h - 1e-9)));
  out += "%%HiResBoundingBox: 0 0 " + num(max_w, 4) + " " + num(max_h, 4) + "\n";
  const bool strip = options::resolved_strip_metadata(ctx_.options);
  if (!strip) {
    if (const auto it = scene_.metadata.find("title"); it != scene_.metadata.end() && !it->second.empty()) {
      std::string title;
      for (const char c : it->second) title.push_back(c >= 32 && c < 127 ? c : '?');
      out += "%%Title: " + emit::ps_string(title) + "\n";
    }
  }
  const int level = (o_.level >= 3 || used_flate_) ? 3 : 2;
  out += "%%LanguageLevel: " + std::to_string(level) + "\n";
  out += "%%DocumentData: Clean7Bit\n";
  out += "%%Pages: " + std::to_string(page_count) + "\n";
  if (!used_fonts_.empty()) {
    std::string list;
    bool first = true;
    for (const std::string& f : used_fonts_) {
      out += (first ? "%%DocumentNeededResources: font " : "%%+ font ") + f + "\n";
      first = false;
      list += f;
    }
  }
  out += "%%EndComments\n";
  out += "%%BeginProlog\n";
  out += "/cvt 40 dict def cvt begin\n"
         "/m /moveto load def /l /lineto load def /c /curveto load def /h /closepath load def\n"
         "/f /fill load def /F /eofill load def /s /stroke load def /n /newpath load def\n"
         "/W { clip } bind def /W* { eoclip } bind def /q /gsave load def /Q /grestore load def\n"
         "/w /setlinewidth load def /J /setlinecap load def /j /setlinejoin load def\n"
         "/M /setmiterlimit load def /d /setdash load def /g /setgray load def /rg /setrgbcolor load def\n"
         "/cvt_img 12 dict def\n"
         "/cvt_image { % width height interpolate decode colorspace filter -- data follows\n"
         "  cvt_img begin /fn exch def /cs exch def /dc exch def /ip exch def /hh exch def /ww exch def\n"
         "  /src currentfile /ASCII85Decode filter def cs setcolorspace\n"
         "  << /ImageType 1 /Width ww /Height hh /BitsPerComponent 8 /Decode dc /ImageMatrix [ww 0 0 hh 0 0]\n"
         "     /DataSource src fn filter /Interpolate ip >> image src flushfile end } bind def\n"
         "/cvt_simage { % width height interpolate decode colorspace string filter\n"
         "  cvt_img begin /fn exch def /str exch def /cs exch def /dc exch def /ip exch def /hh exch def\n"
         "  /ww exch def cs setcolorspace\n"
         "  << /ImageType 1 /Width ww /Height hh /BitsPerComponent 8 /Decode dc /ImageMatrix [ww 0 0 hh 0 0]\n"
         "     /DataSource str /ASCII85Decode filter fn filter /Interpolate ip >> image end } bind def\n"
         "";
  if (!used_fonts_.empty()) {
    out += "/cvt_encoding ISOLatin1Encoding 256 array copy def\ncvt_encoding 128 [";
    for (const auto& [slot, cp] : extra_slots) out += "/" + std::string(ps::unicode_glyph_name(cp));
    out += "] putinterval\n"
           "/cvt_reencode { findfont dup length dict begin { 1 index /FID ne { def } { pop pop } ifelse } forall\n"
           "  /Encoding cvt_encoding def currentdict end definefont pop } bind def\n";
  }
  out += "end\n%%EndProlog\n";
  out += "%%BeginSetup\n%%EndSetup\n";
  for (std::size_t i = 0; i < page_count; ++i) {
    const Page& pg = scene_.pages[i];
    const std::string n = std::to_string(i + 1);
    out += "%%Page: " + n + " " + n + "\n";
    const double w = pg.width * pt_per_px, h = pg.height * pt_per_px;
    if (!o_.eps) {
      out += std::format("%%PageBoundingBox: 0 0 {} {}\n", static_cast<long long>(std::ceil(w - 1e-9)),
                         static_cast<long long>(std::ceil(h - 1e-9)));
    }
    out += "%%BeginPageSetup\n";
    if (!o_.eps) {
      out += "/setpagedevice where { pop << /PageSize [" + num(w, 4) + " " + num(h, 4) + "] >> setpagedevice } if\n";
    }
    out += "cvt begin save\n%%EndPageSetup\n";
    out += bodies[i];
    out += "restore end\nshowpage\n%%PageTrailer\n";
  }
  out += "%%Trailer\n%%EOF\n";
  if (out.size() > ctx_.limits.max_output_bytes) return fail(Status::Limit, "the PostScript output is too large");
  return to_bytes(out);
}

}  // namespace

Result<Bytes> write_postscript(const Scene& scene, WriteContext& ctx, const PsWriteOptions& options) {
  Writer w(scene, ctx, options);
  return w.run();
}

}  // namespace cvt::vector
