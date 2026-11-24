// Convertible core - pdf family: XPS / OpenXPS reading.
//
// The fixed pages of an XPS package (ZIP + XAML-like XML) are translated
// into an in-memory PDF - paths, glyph runs with their embedded (and
// de-obfuscated) fonts, image brushes, canvases with transforms, clips and
// opacity - which the PDF text extraction and renderer then process.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

#include "engine/registry.hpp"
#include "families/pdf/builder.hpp"
#include "families/pdf/font_program.hpp"
#include "families/pdf/pdf.hpp"
#include "families/pdf/pdf_image.hpp"
#include "families/pdf/syntax.hpp"
#include "families/pdf/write_common.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"
#include "util/zip.hpp"

namespace cvt::pdf {

namespace {

std::string resolve_part(std::string_view base, std::string_view target) {
  std::string t(target);
  if (t.empty()) return t;
  std::vector<std::string> parts;
  if (t[0] != '/') {
    const std::size_t slash = base.rfind('/');
    const std::string dir = slash == std::string_view::npos ? std::string() : std::string(base.substr(0, slash));
    t = dir + "/" + t;
  }
  for (const std::string_view p : util::split(t, '/', false)) {
    if (p == ".") continue;
    if (p == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.emplace_back(p);
  }
  return util::join(parts, "/");
}

struct XpsColor {
  double r = 0, g = 0, b = 0, a = 1;
  bool valid = false;
};

XpsColor parse_color(std::string_view s) {
  XpsColor c;
  s = util::trim(s);
  if (s.starts_with("#")) {
    const std::string_view hex = s.substr(1);
    const auto v = util::parse_int(hex, 16);
    if (!v) return c;
    const auto u = static_cast<std::uint32_t>(*v);
    if (hex.size() == 8) {
      c.a = ((u >> 24) & 0xFF) / 255.0;
      c.r = ((u >> 16) & 0xFF) / 255.0;
      c.g = ((u >> 8) & 0xFF) / 255.0;
      c.b = (u & 0xFF) / 255.0;
    } else if (hex.size() == 6) {
      c.r = ((u >> 16) & 0xFF) / 255.0;
      c.g = ((u >> 8) & 0xFF) / 255.0;
      c.b = (u & 0xFF) / 255.0;
    } else {
      return c;
    }
    c.valid = true;
  } else if (s.starts_with("sc#")) {
    std::vector<double> v;
    for (const std::string_view p : util::split(s.substr(3), ',', false)) v.push_back(util::parse_double(util::trim(p)).value_or(0));
    const auto gamma = [](double x) {
      x = std::clamp(x, 0.0, 1.0);
      return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
    };
    if (v.size() == 4) {
      c = {gamma(v[1]), gamma(v[2]), gamma(v[3]), std::clamp(v[0], 0.0, 1.0), true};
    } else if (v.size() == 3) {
      c = {gamma(v[0]), gamma(v[1]), gamma(v[2]), 1, true};
    }
  }
  return c;
}

Matrix parse_matrix(std::string_view s) {
  std::vector<double> v;
  for (const std::string_view p : util::split(util::trim(s), ',', false)) v.push_back(util::parse_double(util::trim(p)).value_or(0));
  if (v.size() != 6) return {};
  return Matrix{v[0], v[1], v[2], v[3], v[4], v[5]};
}

// The XPS abbreviated geometry syntax -> PDF path operators.
std::string parse_geometry(std::string_view data, bool& even_odd) {
  std::string out;
  std::size_t i = 0;
  char cmd = 'M';
  double cx = 0, cy = 0, sx = 0, sy = 0;
  double last_cx = 0, last_cy = 0;  // last control point for S / T
  char last_cmd = 0;
  even_odd = true;  // XPS default FillRule is EvenOdd
  const auto skip = [&]() {
    while (i < data.size() && (data[i] == ' ' || data[i] == ',' || data[i] == '\n' || data[i] == '\r' || data[i] == '\t')) ++i;
  };
  const auto number = [&](double& v) {
    skip();
    const std::size_t start = i;
    if (i < data.size() && (data[i] == '-' || data[i] == '+')) ++i;
    while (i < data.size() && (util::is_ascii_digit(data[i]) || data[i] == '.' || data[i] == 'e' || data[i] == 'E' ||
                               ((data[i] == '-' || data[i] == '+') && i > start && (data[i - 1] == 'e' || data[i - 1] == 'E')))) {
      ++i;
    }
    if (i == start) return false;
    v = util::parse_double(data.substr(start, i - start)).value_or(0);
    return true;
  };
  const auto pt = [&](double x, double y) { return format_number(x) + " " + format_number(y); };
  std::size_t guard = 0;
  while (i < data.size() && ++guard < 1'000'000) {
    skip();
    if (i >= data.size()) break;
    if (util::is_ascii_alpha(data[i])) {
      cmd = data[i++];
      if (cmd == 'F') {
        double rule = 0;
        number(rule);
        even_odd = rule == 0;
        continue;
      }
      if (cmd == 'Z' || cmd == 'z') {
        out += "h\n";
        cx = sx;
        cy = sy;
        last_cmd = cmd;
        continue;
      }
    }
    const bool rel = cmd >= 'a' && cmd <= 'z';
    const char c = util::ascii_upper(cmd);
    double a = 0, b = 0;
    const double ox = rel ? cx : 0, oy = rel ? cy : 0;
    if (c == 'M' || c == 'L') {
      if (!number(a) || !number(b)) break;
      cx = ox + a;
      cy = oy + b;
      out += pt(cx, cy) + (c == 'M' ? " m\n" : " l\n");
      if (c == 'M') {
        sx = cx;
        sy = cy;
        cmd = rel ? 'l' : 'L';  // subsequent pairs are line-tos
      }
    } else if (c == 'H') {
      if (!number(a)) break;
      cx = ox + a;
      out += pt(cx, cy) + " l\n";
    } else if (c == 'V') {
      if (!number(a)) break;
      cy = oy + a;
      out += pt(cx, cy) + " l\n";
    } else if (c == 'C') {
      double x1, y1, x2, y2, x3, y3;
      if (!number(x1) || !number(y1) || !number(x2) || !number(y2) || !number(x3) || !number(y3)) break;
      out += pt(ox + x1, oy + y1) + " " + pt(ox + x2, oy + y2) + " " + pt(ox + x3, oy + y3) + " c\n";
      last_cx = ox + x2;
      last_cy = oy + y2;
      cx = ox + x3;
      cy = oy + y3;
    } else if (c == 'S') {
      double x2, y2, x3, y3;
      if (!number(x2) || !number(y2) || !number(x3) || !number(y3)) break;
      const bool smooth = util::ascii_upper(last_cmd) == 'C' || util::ascii_upper(last_cmd) == 'S';
      const double x1 = smooth ? 2 * cx - last_cx : cx, y1 = smooth ? 2 * cy - last_cy : cy;
      out += pt(x1, y1) + " " + pt(ox + x2, oy + y2) + " " + pt(ox + x3, oy + y3) + " c\n";
      last_cx = ox + x2;
      last_cy = oy + y2;
      cx = ox + x3;
      cy = oy + y3;
    } else if (c == 'Q') {
      double x1, y1, x2, y2;
      if (!number(x1) || !number(y1) || !number(x2) || !number(y2)) break;
      const double qx = ox + x1, qy = oy + y1, ex = ox + x2, ey = oy + y2;
      out += pt(cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy)) + " " + pt(ex + 2.0 / 3 * (qx - ex), ey + 2.0 / 3 * (qy - ey)) +
             " " + pt(ex, ey) + " c\n";
      cx = ex;
      cy = ey;
    } else if (c == 'A') {
      double rx, ry, rot, large, sweep, x, y;
      if (!number(rx) || !number(ry) || !number(rot) || !number(large) || !number(sweep) || !number(x) || !number(y)) break;
      // Elliptical arc -> cubic Beziers (SVG endpoint parametrisation).
      const double x1 = cx, y1 = cy, x2 = ox + x, y2 = oy + y;
      rx = std::abs(rx);
      ry = std::abs(ry);
      if (rx == 0 || ry == 0) {
        out += pt(x2, y2) + " l\n";
      } else {
        const double phi = rot * 3.14159265358979323846 / 180, cs = std::cos(phi), sn = std::sin(phi);
        const double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
        const double x1p = cs * dx + sn * dy, y1p = -sn * dx + cs * dy;
        double lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
        if (lambda > 1) {
          rx *= std::sqrt(lambda);
          ry *= std::sqrt(lambda);
        }
        double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
        num = std::max(0.0, num);
        double coef = std::sqrt(num / (rx * rx * y1p * y1p + ry * ry * x1p * x1p));
        if ((large != 0) == (sweep != 0)) coef = -coef;
        const double cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
        const double ccx = cs * cxp - sn * cyp + (x1 + x2) / 2, ccy = sn * cxp + cs * cyp + (y1 + y2) / 2;
        const auto angle = [](double ux, double uy, double vx, double vy) {
          double a2 = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
          return a2;
        };
        double t1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
        double dt = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
        if (sweep == 0 && dt > 0) dt -= 2 * 3.14159265358979323846;
        if (sweep != 0 && dt < 0) dt += 2 * 3.14159265358979323846;
        const int segs = std::max(1, static_cast<int>(std::ceil(std::abs(dt) / (3.14159265358979323846 / 2))));
        const double step = dt / segs;
        for (int k = 0; k < segs; ++k) {
          const double a0 = t1 + k * step, a1 = a0 + step;
          const double alpha = 4.0 / 3 * std::tan(step / 4);
          const auto at = [&](double t) {
            return Point{ccx + rx * std::cos(t) * cs - ry * std::sin(t) * sn, ccy + rx * std::cos(t) * sn + ry * std::sin(t) * cs};
          };
          const auto der = [&](double t) {
            return Point{-rx * std::sin(t) * cs - ry * std::cos(t) * sn, -rx * std::sin(t) * sn + ry * std::cos(t) * cs};
          };
          const Point p0 = at(a0), p3 = at(a1), d0 = der(a0), d3 = der(a1);
          out += pt(p0.x + alpha * d0.x, p0.y + alpha * d0.y) + " " + pt(p3.x - alpha * d3.x, p3.y - alpha * d3.y) + " " +
                 pt(p3.x, p3.y) + " c\n";
        }
      }
      cx = x2;
      cy = y2;
    } else {
      ++i;  // unknown command letter
    }
    last_cmd = cmd;
  }
  return out;
}

struct XpsConverter {
  util::ZipReader* zip = nullptr;
  const Context* ctx = nullptr;
  PdfBuilder builder{6};
  struct FontInfo {
    std::string resource;
    std::shared_ptr<const font::TrueType> tt;
    Bytes data;
    std::set<std::uint16_t> used;
    std::map<std::uint16_t, std::u32string> text;
  };
  std::map<std::string, FontInfo> fonts;
  std::map<std::string, Ref> images;  // part -> xobject
  Dict page_fonts, page_images, page_gs;
  int gs_counter = 0;
  std::string content;

  FontInfo* font(const std::string& part) {
    auto it = fonts.find(part);
    if (it != fonts.end()) return it->second.tt ? &it->second : nullptr;
    FontInfo info;
    info.resource = "F" + std::to_string(fonts.size());
    auto data = zip->read(part);
    if (data) {
      Bytes bytes = std::move(*data);
      // Obfuscated fonts: XOR the first 32 bytes with the GUID in the file name.
      const std::size_t slash = part.rfind('/');
      std::string name = slash == std::string::npos ? part : part.substr(slash + 1);
      if (util::iends_with(name, ".odttf") || util::iends_with(name, ".obfuscated")) {
        std::string hex;
        for (const char c : name.substr(0, name.find('.'))) {
          if (util::is_ascii_digit(c) || (util::ascii_lower(c) >= 'a' && util::ascii_lower(c) <= 'f')) hex.push_back(c);
        }
        if (hex.size() == 32 && bytes.size() >= 32) {
          std::uint8_t key[16];
          for (int k = 0; k < 16; ++k) key[k] = static_cast<std::uint8_t>(util::parse_int(hex.substr(static_cast<std::size_t>(2 * k), 2), 16).value_or(0));
          for (int k = 0; k < 16; ++k) {
            bytes[static_cast<std::size_t>(k)] ^= static_cast<std::byte>(key[15 - k]);
            bytes[static_cast<std::size_t>(k) + 16] ^= static_cast<std::byte>(key[15 - k]);
          }
        }
      }
      if (auto tt = font::TrueType::parse(bytes)) {
        info.tt = std::make_shared<const font::TrueType>(std::move(*tt));
        info.data = std::move(bytes);
      }
    }
    auto& slot = fonts[part] = std::move(info);
    return slot.tt ? &slot : nullptr;
  }

  std::optional<Ref> image(const std::string& part) {
    if (const auto it = images.find(part); it != images.end()) return it->second;
    auto data = zip->read(part);
    if (!data) return std::nullopt;
    const std::string lower = util::to_lower(part);
    std::string format = "png";
    if (lower.ends_with(".jpg") || lower.ends_with(".jpeg")) format = "jpg";
    else if (lower.ends_with(".tif") || lower.ends_with(".tiff")) format = "tiff";
    else if (lower.ends_with(".wdp") || lower.ends_with(".jxr")) format = "jxr";
    auto decoded = decode_with_registry(ctx, format, *data);
    if (!decoded) return std::nullopt;
    ImageEncoding enc;
    enc.lossless = format != "jpg";
    auto ref = embed_image(builder, decoded->frames[0], enc, *ctx);
    if (!ref) return std::nullopt;
    images[part] = *ref;
    return *ref;
  }

  std::string set_alpha(double alpha) {
    if (alpha >= 0.999) return {};
    const std::string name = "G" + std::to_string(gs_counter++);
    Dict d;
    d.set("ca", alpha);
    d.set("CA", alpha);
    page_gs.set(name, Object(std::move(d)));
    return "/" + name + " gs\n";
  }

  void element(const util::XmlNode& node, const std::string& page_part, int depth) {
    if (depth > 64 || content.size() > (64u << 20)) return;
    const std::string_view name = node.local_name();
    if (name == "Canvas") {
      content += "q\n";
      if (const auto t = node.attribute("RenderTransform"); t) {
        const Matrix m = parse_matrix(t.value());
        content += format_number(m.a) + " " + format_number(m.b) + " " + format_number(m.c) + " " + format_number(m.d) +
                   " " + format_number(m.e) + " " + format_number(m.f) + " cm\n";
      }
      if (const auto c = node.attribute("Clip"); c) {
        bool eo = true;
        content += parse_geometry(c.value(), eo) + (eo ? "W* n\n" : "W n\n");
      }
      if (const auto o = node.attribute("Opacity"); o) content += set_alpha(o.as_double(1));
      for (const util::XmlNode child : node.elements()) element(child, page_part, depth + 1);
      content += "Q\n";
    } else if (name == "Path") {
      path(node, page_part);
    } else if (name == "Glyphs") {
      glyphs(node, page_part);
    }
  }

  void path(const util::XmlNode& node, const std::string& page_part) {
    std::string geometry;
    bool eo = true;
    if (const auto d = node.attribute("Data"); d) {
      geometry = parse_geometry(d.value(), eo);
    } else if (const util::XmlNode pg = node.find("Path.Data/PathGeometry"); pg) {
      if (const auto f = pg.attribute("Figures"); f) geometry = parse_geometry(f.value(), eo);
      if (pg.attribute("FillRule").value() == "NonZero") eo = false;
    }
    if (geometry.empty()) return;
    content += "q\n";
    if (const auto t = node.attribute("RenderTransform"); t) {
      const Matrix m = parse_matrix(t.value());
      content += format_number(m.a) + " " + format_number(m.b) + " " + format_number(m.c) + " " + format_number(m.d) +
                 " " + format_number(m.e) + " " + format_number(m.f) + " cm\n";
    }
    if (const auto c = node.attribute("Clip"); c) {
      bool ceo = true;
      content += parse_geometry(c.value(), ceo) + (ceo ? "W* n\n" : "W n\n");
    }
    double opacity = node.attribute("Opacity") ? node.attribute("Opacity").as_double(1) : 1;
    XpsColor fill = parse_color(node.attribute("Fill").value());
    const XpsColor stroke = parse_color(node.attribute("Stroke").value());
    // Element brushes.
    const util::XmlNode fill_el = node.child("Path.Fill");
    if (fill_el) {
      const util::XmlNode brush = fill_el.first_child();
      for (const util::XmlNode b : fill_el.elements()) {
        const std::string_view bn = b.local_name();
        if (bn == "SolidColorBrush") {
          fill = parse_color(b.attribute("Color").value());
          if (b.attribute("Opacity")) fill.a *= b.attribute("Opacity").as_double(1);
        } else if (bn == "ImageBrush") {
          image_brush(b, geometry, eo, page_part, opacity);
          content += "Q\n";
          return;
        } else if (bn == "LinearGradientBrush" || bn == "RadialGradientBrush") {
          // Approximated by the average of its stops.
          double r = 0, g = 0, bl = 0, a = 0;
          int n = 0;
          for (const util::XmlNode stop : b.descendants()) {
            if (stop.local_name() != "GradientStop") continue;
            const XpsColor c = parse_color(stop.attribute("Color").value());
            if (!c.valid) continue;
            r += c.r;
            g += c.g;
            bl += c.b;
            a += c.a;
            ++n;
          }
          if (n > 0) fill = XpsColor{r / n, g / n, bl / n, a / n, true};
        }
      }
      (void)brush;
    }
    const double thickness = node.attribute("StrokeThickness") ? node.attribute("StrokeThickness").as_double(1) : 1;
    std::string ops;
    if (fill.valid && fill.a > 0) {
      content += set_alpha(fill.a * opacity);
      content += format_number(fill.r) + " " + format_number(fill.g) + " " + format_number(fill.b) + " rg\n";
      content += geometry + (eo ? "f*\n" : "f\n");
    }
    if (stroke.valid && stroke.a > 0) {
      content += set_alpha(stroke.a * opacity);
      content += format_number(stroke.r) + " " + format_number(stroke.g) + " " + format_number(stroke.b) + " RG " +
                 format_number(thickness) + " w\n";
      const std::string join = std::string(node.attribute("StrokeLineJoin").value());
      content += join == "Round" ? "1 j\n" : (join == "Bevel" ? "2 j\n" : "0 j\n");
      const std::string cap = std::string(node.attribute("StrokeStartLineCap").value());
      content += cap == "Round" ? "1 J\n" : (cap == "Square" ? "2 J\n" : "0 J\n");
      content += geometry + "S\n";
    }
    content += "Q\n";
  }

  void image_brush(const util::XmlNode& b, const std::string& geometry, bool eo, const std::string& page_part,
                   double opacity) {
    const std::string part = resolve_part(page_part, b.attribute("ImageSource").value());
    const auto ref = image(part);
    if (!ref) return;
    std::vector<double> vp;
    for (const std::string_view p : util::split(b.attribute("Viewport").value(), ',', false)) {
      vp.push_back(util::parse_double(util::trim(p)).value_or(0));
    }
    if (vp.size() != 4) return;
    const std::string name = "Im" + std::to_string(page_images.size());
    page_images.set(name, *ref);
    content += geometry + (eo ? "W* n\n" : "W n\n");
    if (const auto t = b.attribute("Transform"); t) {
      const Matrix m = parse_matrix(t.value());
      content += format_number(m.a) + " " + format_number(m.b) + " " + format_number(m.c) + " " + format_number(m.d) +
                 " " + format_number(m.e) + " " + format_number(m.f) + " cm\n";
    }
    content += set_alpha(opacity);
    // Image unit square -> viewport rectangle (y down in XPS).
    content += format_number(vp[2]) + " 0 0 " + format_number(-vp[3]) + " " + format_number(vp[0]) + " " +
               format_number(vp[1] + vp[3]) + " cm /" + name + " Do\n";
  }

  void glyphs(const util::XmlNode& node, const std::string& page_part) {
    FontInfo* f = font(resolve_part(page_part, node.attribute("FontUri").value()));
    if (f == nullptr) return;
    const double size = node.attribute("FontRenderingEmSize").as_double(12);
    const double ox = node.attribute("OriginX").as_double(0), oy = node.attribute("OriginY").as_double(0);
    XpsColor fill = parse_color(node.attribute("Fill").value());
    if (!fill.valid) fill = XpsColor{0, 0, 0, 1, true};
    const std::u32string text = util::utf8_to_utf32(node.attribute("UnicodeString").value());
    std::vector<std::string_view> indices;
    if (const auto ind = node.attribute("Indices"); ind) indices = util::split(ind.value(), ';', true);
    const double upem = f->tt->units_per_em();
    std::string tj = "[";
    const std::size_t count = std::max(text.size(), indices.size());
    std::size_t ti = 0;
    for (std::size_t k = 0; k < count && k < 100000; ++k) {
      std::optional<std::uint16_t> gid;
      double advance = -1;
      std::size_t chars = 1;
      if (k < indices.size()) {
        std::string_view e = indices[k];
        if (!e.empty() && e.front() == '(') {
          const std::size_t close = e.find(')');
          if (close != std::string_view::npos) {
            const auto colon = e.find(':');
            chars = static_cast<std::size_t>(util::parse_int(e.substr(1, std::min(colon, close) - 1)).value_or(1));
            e.remove_prefix(close + 1);
          }
        }
        const auto parts = util::split(e, ',', true);
        if (!parts.empty() && !util::trim(parts[0]).empty()) {
          if (const auto g = util::parse_int(util::trim(parts[0]))) gid = static_cast<std::uint16_t>(*g);
        }
        if (parts.size() > 1 && !util::trim(parts[1]).empty()) advance = util::parse_double(util::trim(parts[1])).value_or(-1);
      }
      std::u32string unit;
      for (std::size_t c = 0; c < chars && ti < text.size(); ++c) unit.push_back(text[ti++]);
      if (!gid && !unit.empty()) gid = f->tt->glyph_for_unicode(unit[0]);
      if (!gid) continue;
      f->used.insert(*gid);
      if (!unit.empty() && !f->text.contains(*gid)) f->text[*gid] = unit;
      const double natural = f->tt->advance(*gid) * 1000.0 / upem;
      tj += std::format("<{:04X}>", *gid);
      if (advance >= 0) {
        const double adjust = natural - advance * 10.0;
        if (std::abs(adjust) > 0.01) tj += " " + format_number(adjust) + " ";
      }
    }
    tj += "] TJ\n";
    page_fonts.set(f->resource, Object::name(f->resource));  // placeholder, fixed at the end
    content += "q\n";
    if (const auto t = node.attribute("RenderTransform"); t) {
      const Matrix m = parse_matrix(t.value());
      content += format_number(m.a) + " " + format_number(m.b) + " " + format_number(m.c) + " " + format_number(m.d) +
                 " " + format_number(m.e) + " " + format_number(m.f) + " cm\n";
    }
    content += set_alpha(fill.a);
    content += "BT\n" + format_number(fill.r) + " " + format_number(fill.g) + " " + format_number(fill.b) + " rg\n";
    if (node.attribute("StyleSimulations").value().find("Bold") != std::string_view::npos) {
      content += "2 Tr " + format_number(size * 0.02) + " w " + format_number(fill.r) + " " + format_number(fill.g) +
                 " " + format_number(fill.b) + " RG\n";
    }
    const bool italic = node.attribute("StyleSimulations").value().find("Italic") != std::string_view::npos;
    content += "/" + f->resource + " " + format_number(size) + " Tf\n1 0 " + (italic ? std::string("-0.2") : std::string("0")) +
               " -1 " + format_number(ox) + " " + format_number(oy) + " Tm\n" + tj + "ET\nQ\n";
  }

  Ref embed_font(FontInfo& f) {
    const Ref file = builder.add_stream([&] {
      Dict d;
      d.set("Length1", static_cast<std::int64_t>(f.data.size()));
      return d;
    }(), f.data);
    const double s = 1000.0 / f.tt->units_per_em();
    Dict fd;
    fd.set("Type", Object::name("FontDescriptor"));
    const std::string name = f.tt->postscript_name().empty() ? "XpsFont" : f.tt->postscript_name();
    fd.set("FontName", Object::name(name));
    fd.set("Flags", 32);
    const Rect bb = f.tt->bbox();
    fd.set("FontBBox", Array{Object(bb.x0 * s), Object(bb.y0 * s), Object(bb.x1 * s), Object(bb.y1 * s)});
    fd.set("ItalicAngle", 0);
    fd.set("Ascent", Object(f.tt->ascent() * s));
    fd.set("Descent", Object(f.tt->descent() * s));
    fd.set("CapHeight", Object(f.tt->cap_height() * s));
    fd.set("StemV", 80);
    fd.set("FontFile2", file);
    Array w;
    for (const std::uint16_t g : f.used) {
      w.push_back(static_cast<std::int64_t>(g));
      w.push_back(Array{Object(std::round(f.tt->advance(g) * s))});
    }
    Dict cid;
    cid.set("Type", Object::name("Font"));
    cid.set("Subtype", Object::name("CIDFontType2"));
    cid.set("BaseFont", Object::name(name));
    Dict info;
    info.set("Registry", Object::string("Adobe"));
    info.set("Ordering", Object::string("Identity"));
    info.set("Supplement", 0);
    cid.set("CIDSystemInfo", Object(std::move(info)));
    cid.set("FontDescriptor", builder.add(Object(std::move(fd))));
    cid.set("W", Object(std::move(w)));
    cid.set("CIDToGIDMap", Object::name("Identity"));
    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CMapName /Adobe-Identity-UCS def\n/CMapType 2 "
        "def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    std::vector<std::pair<std::uint16_t, std::u32string>> entries(f.text.begin(), f.text.end());
    for (std::size_t i = 0; i < entries.size(); i += 100) {
      const std::size_t n = std::min<std::size_t>(100, entries.size() - i);
      cmap += std::to_string(n) + " beginbfchar\n";
      for (std::size_t k = i; k < i + n; ++k) {
        cmap += std::format("<{:04X}> <", entries[k].first);
        for (const char16_t u : util::utf8_to_utf16(util::utf32_to_utf8(entries[k].second))) {
          cmap += std::format("{:04X}", static_cast<unsigned>(u));
        }
        cmap += ">\n";
      }
      cmap += "endbfchar\n";
    }
    cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    Dict t0;
    t0.set("Type", Object::name("Font"));
    t0.set("Subtype", Object::name("Type0"));
    t0.set("BaseFont", Object::name(name));
    t0.set("Encoding", Object::name("Identity-H"));
    t0.set("DescendantFonts", Array{Object(builder.add(Object(std::move(cid))))});
    t0.set("ToUnicode", builder.add_stream(Dict{}, as_bytes(cmap)));
    return builder.add(Object(std::move(t0)));
  }
};

Result<Bytes> xps_to_pdf(ByteView input, const Context& ctx) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(input, ctx.limits));
  // Locate the fixed document sequence through the package relationships.
  std::string fdseq;
  if (auto rels = zip.read("_rels/.rels")) {
    if (auto xml = util::parse_xml(*rels)) {
      for (const util::XmlNode r : xml->root().elements()) {
        const std::string_view type = r.attribute("Type").value();
        if (type.ends_with("/fixedrepresentation")) fdseq = resolve_part("", r.attribute("Target").value());
      }
    }
  }
  if (fdseq.empty()) {
    for (const auto& e : zip.entries()) {
      if (util::iends_with(e.name, ".fdseq")) fdseq = e.name;
    }
  }
  if (fdseq.empty()) return fail(Status::Decode, "not an XPS document (no fixed document sequence)");
  CVT_TRY_ASSIGN(Bytes seq_bytes, zip.read(fdseq));
  CVT_TRY_ASSIGN(util::XmlDocument seq, util::parse_xml(seq_bytes));
  std::vector<std::string> page_parts;
  for (const util::XmlNode ref : seq.root().elements()) {
    if (ref.local_name() != "DocumentReference") continue;
    const std::string doc_part = resolve_part(fdseq, ref.attribute("Source").value());
    auto doc_bytes = zip.read(doc_part);
    if (!doc_bytes) continue;
    auto doc = util::parse_xml(*doc_bytes);
    if (!doc) continue;
    for (const util::XmlNode pc : doc->root().elements()) {
      if (pc.local_name() == "PageContent") page_parts.push_back(resolve_part(doc_part, pc.attribute("Source").value()));
      if (page_parts.size() > 10000) break;
    }
  }
  if (page_parts.empty()) return fail(Status::Decode, "the XPS document has no pages");
  XpsConverter conv;
  conv.zip = &zip;
  conv.ctx = &ctx;
  const Ref pages_ref = conv.builder.reserve();
  struct PageRec {
    Ref ref;
    Ref content;
    double w = 0, h = 0;
    Dict fonts, images, gs;
  };
  std::vector<PageRec> recs;
  for (std::size_t i = 0; i < page_parts.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(page_parts.size()), "Reading XPS"));
    auto bytes = zip.read(page_parts[i]);
    if (!bytes) continue;
    auto xml = util::parse_xml(*bytes);
    if (!xml) continue;
    const util::XmlNode root = xml->root();
    PageRec rec;
    rec.w = std::clamp(root.attribute("Width").as_double(816), 1.0, 200000.0);
    rec.h = std::clamp(root.attribute("Height").as_double(1056), 1.0, 200000.0);
    conv.content = "0.75 0 0 -0.75 0 " + format_number(rec.h * 0.75) + " cm\n";
    conv.page_fonts = Dict{};
    conv.page_images = Dict{};
    conv.page_gs = Dict{};
    for (const util::XmlNode child : root.elements()) conv.element(child, page_parts[i], 0);
    rec.ref = conv.builder.reserve();
    rec.content = conv.builder.add_stream(Dict{}, as_bytes(conv.content));
    rec.fonts = conv.page_fonts;
    rec.images = conv.page_images;
    rec.gs = conv.page_gs;
    recs.push_back(std::move(rec));
  }
  if (recs.empty()) return fail(Status::Decode, "the XPS pages could not be read");
  std::map<std::string, Ref> font_refs;
  for (auto& [part, info] : conv.fonts) {
    if (info.tt && !info.used.empty()) font_refs[info.resource] = conv.embed_font(info);
  }
  Array kids;
  for (PageRec& rec : recs) {
    Dict res;
    Dict fonts;
    for (const auto& [name, unused] : rec.fonts) {
      if (const auto it = font_refs.find(name); it != font_refs.end()) fonts.set(name, it->second);
    }
    res.set("Font", Object(std::move(fonts)));
    if (!rec.images.empty()) res.set("XObject", Object(std::move(rec.images)));
    if (!rec.gs.empty()) res.set("ExtGState", Object(std::move(rec.gs)));
    Dict pd;
    pd.set("Type", Object::name("Page"));
    pd.set("Parent", pages_ref);
    pd.set("MediaBox", Array{Object(0), Object(0), Object(rec.w * 0.75), Object(rec.h * 0.75)});
    pd.set("Resources", Object(std::move(res)));
    pd.set("Contents", rec.content);
    conv.builder.set(rec.ref, Object(std::move(pd)));
    kids.push_back(rec.ref);
  }
  Dict pages;
  pages.set("Type", Object::name("Pages"));
  pages.set("Count", static_cast<std::int64_t>(kids.size()));
  pages.set("Kids", Object(std::move(kids)));
  conv.builder.set(pages_ref, Object(std::move(pages)));
  Dict catalog;
  catalog.set("Type", Object::name("Catalog"));
  catalog.set("Pages", pages_ref);
  const Ref root = conv.builder.add(Object(std::move(catalog)));
  return conv.builder.finish(root, std::nullopt, "1.7");
}

Result<std::shared_ptr<PdfFile>> open_xps(ReadContext& ctx, Bytes& storage) {
  CVT_TRY_ASSIGN(storage, xps_to_pdf(ctx.input, ctx));
  OpenOptions options;
  options.limits = ctx.limits;
  return PdfFile::open(storage, options);
}

Result<AnyIR> read_xps_image(ReadContext& ctx) {
  Bytes storage;
  CVT_TRY_ASSIGN(auto file, open_xps(ctx, storage));
  RenderOptions options;
  CVT_TRY_ASSIGN(options.pages, parse_page_ranges(ctx.options.get_string("pages", ""), static_cast<int>(file->pages().size())));
  options.dpi = ctx.options.get_float("dpi", 150, 10, 1200);
  CVT_TRY_ASSIGN(ir::Image image, render_pages(*file, options, ctx));
  return image;
}

int sniff_xps(std::span<const std::byte> data, std::string_view) {
  if (data.size() < 4 || as_chars(data.first(2)) != "PK") return 0;
  const std::string_view text = as_chars(data);
  if (text.find("FixedDocumentSequence.fdseq") != std::string_view::npos || text.find(".fdseq") != std::string_view::npos) {
    return 92;
  }
  if (text.find(".fpage") != std::string_view::npos) return 90;
  return 0;
}

}  // namespace

void register_xps(Registry& registry) {
  for (const char* id : {"xps", "oxps"}) {
    FormatInfo info;
    info.id = id;
    info.name = std::string(id) == "xps" ? "XPS document" : "OpenXPS document";
    info.description = "XML Paper Specification: fixed-layout pages in a ZIP package.";
    info.category = Category::Document;
    info.extensions = {id};
    info.mime = std::string(id) == "xps" ? "application/vnd.ms-xpsdocument" : "application/oxps";
    info.popularity = std::string(id) == "xps" ? 30 : 15;
    info.sniff = [id](std::span<const std::byte> data, std::string_view filename) {
      const int score = sniff_xps(data, filename);
      if (score == 0) return 0;
      // Both share the structure: tell them apart by the extension.
      const bool oxps = util::iends_with(filename, ".oxps");
      return (std::string_view(id) == "oxps") == oxps ? score : score - 10;
    };
    // The office family owns XPS (its description, detection and the
    // Document reader with headings, pictures and page breaks); this family
    // adds the page renderer, and its description only fills gaps.
    registry.add_known_format(std::move(info));
    Codec img;
    img.format_id = id;
    img.kind = Kind::Image;
    img.read = read_xps_image;
    img.read_cost = 20;
    img.lossy = true;
    img.read_options = {OptionSpec::text("pages", "Pages", "", "Pages to convert, e.g. \"1-3,5\"; empty for all."),
                        OptionSpec::number("dpi", "Resolution (dpi)", 150, 10, 1200, "Rendering resolution.")};
    registry.add_codec(std::move(img));
  }
}

}  // namespace cvt::pdf
