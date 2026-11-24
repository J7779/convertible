// Convertible core - font family: SVG fonts (<font> elements inside an SVG document).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/font/build.hpp"
#include "families/font/font.hpp"
#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::font {

namespace {

// ---- SVG path data

class PathParser {
 public:
  explicit PathParser(std::string_view d) : d_(d) {}

  Result<Path> parse() {
    Path path;
    char command = 0;
    Point current{}, start{}, last_control{};
    char previous = 0;
    std::size_t guard = 0;
    while (true) {
      skip();
      if (pos_ >= d_.size()) break;
      if (++guard > 1'000'000) return fail(Status::Decode, "SVG path data is too long");
      const char c = d_[pos_];
      if (std::isalpha(static_cast<unsigned char>(c))) {
        command = c;
        ++pos_;
        if (command == 'Z' || command == 'z') {
          path.close();
          current = start;
          previous = 'Z';
          continue;
        }
      } else if (command == 0) {
        return fail(Status::Decode, "SVG path data does not start with a command");
      }
      const bool rel = std::islower(static_cast<unsigned char>(command)) != 0;
      const auto pt = [&](double x, double y) { return rel ? Point{current.x + x, current.y + y} : Point{x, y}; };
      switch (std::toupper(static_cast<unsigned char>(command))) {
        case 'M': {
          CVT_TRY_ASSIGN(const double x, number());
          CVT_TRY_ASSIGN(const double y, number());
          current = start = pt(x, y);
          path.move_to(current);
          command = rel ? 'l' : 'L';  // further pairs are lines
          break;
        }
        case 'L': {
          CVT_TRY_ASSIGN(const double x, number());
          CVT_TRY_ASSIGN(const double y, number());
          current = pt(x, y);
          path.line_to(current);
          break;
        }
        case 'H': {
          CVT_TRY_ASSIGN(const double x, number());
          current = {rel ? current.x + x : x, current.y};
          path.line_to(current);
          break;
        }
        case 'V': {
          CVT_TRY_ASSIGN(const double y, number());
          current = {current.x, rel ? current.y + y : y};
          path.line_to(current);
          break;
        }
        case 'C': case 'S': {
          Point c1{};
          const bool smooth = std::toupper(static_cast<unsigned char>(command)) == 'S';
          if (smooth) {
            const bool cubic_before = previous == 'C' || previous == 'S';
            c1 = cubic_before ? Point{2 * current.x - last_control.x, 2 * current.y - last_control.y} : current;
          } else {
            CVT_TRY_ASSIGN(const double x1, number());
            CVT_TRY_ASSIGN(const double y1, number());
            c1 = pt(x1, y1);
          }
          CVT_TRY_ASSIGN(const double x2, number());
          CVT_TRY_ASSIGN(const double y2, number());
          CVT_TRY_ASSIGN(const double x, number());
          CVT_TRY_ASSIGN(const double y, number());
          const Point c2 = pt(x2, y2), p = pt(x, y);
          path.cubic_to(c1, c2, p);
          last_control = c2;
          current = p;
          break;
        }
        case 'Q': case 'T': {
          Point control{};
          if (std::toupper(static_cast<unsigned char>(command)) == 'T') {
            const bool quad_before = previous == 'Q' || previous == 'T';
            control = quad_before ? Point{2 * current.x - last_control.x, 2 * current.y - last_control.y} : current;
          } else {
            CVT_TRY_ASSIGN(const double x1, number());
            CVT_TRY_ASSIGN(const double y1, number());
            control = pt(x1, y1);
          }
          CVT_TRY_ASSIGN(const double x, number());
          CVT_TRY_ASSIGN(const double y, number());
          const Point p = pt(x, y);
          path.quad_to(control, p);
          last_control = control;
          current = p;
          break;
        }
        case 'A': {
          CVT_TRY_ASSIGN(const double rx, number());
          CVT_TRY_ASSIGN(const double ry, number());
          CVT_TRY_ASSIGN(const double angle, number());
          CVT_TRY_ASSIGN(const double large, flag());
          CVT_TRY_ASSIGN(const double sweep, flag());
          CVT_TRY_ASSIGN(const double x, number());
          CVT_TRY_ASSIGN(const double y, number());
          const Point p = pt(x, y);
          arc(path, current, p, rx, ry, angle, large != 0, sweep != 0);
          current = p;
          break;
        }
        default:
          return fail(Status::Decode, "SVG path command '{}' is not supported", command);
      }
      previous = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
    }
    path.close();
    return path;
  }

 private:
  void skip() {
    while (pos_ < d_.size() && (util::is_ascii_space(d_[pos_]) || d_[pos_] == ',')) ++pos_;
  }
  Result<double> number() {
    skip();
    const std::size_t start = pos_;
    if (pos_ < d_.size() && (d_[pos_] == '-' || d_[pos_] == '+')) ++pos_;
    bool dot = false, digits = false;
    while (pos_ < d_.size()) {
      const char c = d_[pos_];
      if (util::is_ascii_digit(c)) {
        digits = true;
      } else if (c == '.' && !dot) {
        dot = true;
      } else if ((c == 'e' || c == 'E') && digits) {
        ++pos_;
        if (pos_ < d_.size() && (d_[pos_] == '-' || d_[pos_] == '+')) ++pos_;
        while (pos_ < d_.size() && util::is_ascii_digit(d_[pos_])) ++pos_;
        break;
      } else {
        break;
      }
      ++pos_;
    }
    const auto value = util::parse_double(d_.substr(start, pos_ - start));
    if (!digits || !value || !std::isfinite(*value)) return fail(Status::Decode, "SVG path data has a malformed number");
    return *value;
  }
  Result<double> flag() {
    skip();
    if (pos_ < d_.size() && (d_[pos_] == '0' || d_[pos_] == '1')) return static_cast<double>(d_[pos_++] - '0');
    return fail(Status::Decode, "SVG arc flag is malformed");
  }

  static void arc(Path& path, Point p0, Point p1, double rx, double ry, double angle, bool large, bool sweep) {
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx < 1e-9 || ry < 1e-9 || p0 == p1) {
      path.line_to(p1);
      return;
    }
    const double phi = angle * std::numbers::pi / 180.0;
    const double cs = std::cos(phi), sn = std::sin(phi);
    const double dx = (p0.x - p1.x) / 2, dy = (p0.y - p1.y) / 2;
    const double x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
    const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lambda > 1) {
      rx *= std::sqrt(lambda);
      ry *= std::sqrt(lambda);
    }
    const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
    if (large == sweep) coef = -coef;
    const double cx1 = coef * rx * y1 / ry, cy1 = -coef * ry * x1 / rx;
    const double cx = cs * cx1 - sn * cy1 + (p0.x + p1.x) / 2, cy = sn * cx1 + cs * cy1 + (p0.y + p1.y) / 2;
    const auto angle_of = [](double ux, double uy, double vx, double vy) {
      return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    const double theta = angle_of(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    double delta = angle_of((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && delta > 0) delta -= 2 * std::numbers::pi;
    if (sweep && delta < 0) delta += 2 * std::numbers::pi;
    const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / (std::numbers::pi / 2))));
    const double step = delta / segments;
    const double k = 4.0 / 3.0 * std::tan(step / 4);
    const auto at = [&](double t) {
      return Point{cx + rx * std::cos(t) * cs - ry * std::sin(t) * sn, cy + rx * std::cos(t) * sn + ry * std::sin(t) * cs};
    };
    const auto deriv = [&](double t) {
      return Point{-rx * std::sin(t) * cs - ry * std::cos(t) * sn, -rx * std::sin(t) * sn + ry * std::cos(t) * cs};
    };
    for (int i = 0; i < segments; ++i) {
      const double t0 = theta + step * i, t1 = t0 + step;
      const Point a = at(t0), b = at(t1), da = deriv(t0), db = deriv(t1);
      path.cubic_to({a.x + k * da.x, a.y + k * da.y}, {b.x - k * db.x, b.y - k * db.y}, i + 1 == segments ? p1 : b);
    }
  }

  std::string_view d_;
  std::size_t pos_ = 0;
};

util::XmlNode find_font(util::XmlNode root) {
  if (root.local_name() == "font") return root;
  for (util::XmlNode node : root.descendants()) {
    if (node.is_element() && node.local_name() == "font") return node;
  }
  return {};
}

int sniff_svg_font(std::span<const std::byte> data, std::string_view) {
  const std::string_view text = as_chars(data.first(std::min<std::size_t>(data.size(), 1 << 16)));
  if (text.find("<svg") == std::string_view::npos) return 0;
  if (text.find("<font") == std::string_view::npos) return 0;
  if (text.find("<glyph") == std::string_view::npos && text.find("<font-face") == std::string_view::npos) return 0;
  return 96;
}

Result<AnyIR> read_svg_font(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const util::XmlDocument document, util::parse_xml(ctx.input));
  const util::XmlNode font = find_font(document.root());
  if (!font) return fail(Status::Decode, "the SVG file contains no <font> element");
  FontSpec spec;
  const util::XmlNode face = font.child_local("font-face");
  const double upm = face ? face.attribute_local("units-per-em").as_double(1000) : 1000;
  spec.units_per_em = static_cast<std::uint16_t>(std::clamp(upm, 16.0, 16384.0));
  spec.ascender = static_cast<std::int16_t>(std::lround(std::clamp(face ? face.attribute_local("ascent").as_double(upm * 0.8) : upm * 0.8, -32768.0, 32767.0)));
  spec.descender = static_cast<std::int16_t>(std::lround(std::clamp(face ? face.attribute_local("descent").as_double(-upm * 0.2) : -upm * 0.2, -32768.0, 32767.0)));
  if (spec.descender > 0) spec.descender = static_cast<std::int16_t>(-spec.descender);
  if (face) {
    const std::string_view family = face.attribute_local("font-family").value();
    if (!family.empty()) spec.family = std::string(family);
    const int weight = face.attribute_local("font-weight").as_int(400);
    spec.weight_class = static_cast<std::uint16_t>(std::clamp(weight, 100, 900));
    spec.bold = weight >= 600;
    spec.italic = face.attribute_local("font-style").value() == "italic";
    spec.underline_position = static_cast<std::int16_t>(face.attribute_local("underline-position").as_int(-100));
    spec.underline_thickness = static_cast<std::int16_t>(face.attribute_local("underline-thickness").as_int(50));
  }
  if (spec.bold) spec.subfamily = spec.italic ? "Bold Italic" : "Bold";
  else if (spec.italic) spec.subfamily = "Italic";
  const double default_advance = font.attribute_local("horiz-adv-x").as_double(upm / 2);
  const auto add_glyph = [&](util::XmlNode node, std::string name) -> Result<std::uint16_t> {
    Path path;
    const std::string_view d = node.attribute_local("d").value();
    if (!d.empty()) {
      CVT_TRY_ASSIGN(path, PathParser(d).parse());
    }
    spec.glyph_names.push_back(std::move(name));
    spec.paths.push_back(std::move(path));
    spec.advances.push_back(node.attribute_local("horiz-adv-x").as_double(default_advance));
    return static_cast<std::uint16_t>(spec.glyph_names.size() - 1);
  };
  const util::XmlNode missing = font.child_local("missing-glyph");
  if (missing) {
    CVT_TRY(add_glyph(missing, ".notdef"));
  } else {
    spec.glyph_names.push_back(".notdef");
    spec.paths.emplace_back();
    spec.advances.push_back(default_advance);
  }
  std::set<std::string> used{".notdef"};
  std::map<std::string, std::uint16_t, std::less<>> by_name;
  std::size_t count = 0;
  for (util::XmlNode node : font.children_local("glyph")) {
    if (++count > 65000) return fail(Status::Decode, "the SVG font has too many glyphs");
    if ((count & 127) == 0) CVT_TRY(ctx.checkpoint(0.5, "Reading SVG glyphs"));
    const std::u32string unicode = util::utf8_to_utf32(node.attribute_local("unicode").value());
    std::string name(node.attribute_local("glyph-name").value());
    if (name == ".notdef" && !missing) {
      // Replace the placeholder .notdef with the real one.
      Path path;
      if (const std::string_view d = node.attribute_local("d").value(); !d.empty()) {
        CVT_TRY_ASSIGN(path, PathParser(d).parse());
      }
      spec.paths[0] = std::move(path);
      spec.advances[0] = node.attribute_local("horiz-adv-x").as_double(default_advance);
      continue;
    }
    if (name.empty() || used.contains(name)) {
      name = unicode.size() == 1 ? glyph_name_for_unicode(unicode[0]) : std::format("glyph{}", count);
    }
    if (used.contains(name)) name += std::format(".{}", count);
    used.insert(name);
    CVT_TRY_ASSIGN(const std::uint16_t gid, add_glyph(node, name));
    by_name.emplace(name, gid);
    if (unicode.size() == 1) spec.cmap.try_emplace(static_cast<std::uint32_t>(unicode[0]), gid);
    else if (unicode.size() > 1) ctx.warn("Ligature glyphs of the SVG font were kept without their substitution rules.");
  }
  for (util::XmlNode node : font.children_local("hkern")) {
    const std::u32string u1 = util::utf8_to_utf32(node.attribute_local("u1").value());
    const std::u32string u2 = util::utf8_to_utf32(node.attribute_local("u2").value());
    const auto k = static_cast<std::int16_t>(std::clamp(-node.attribute_local("k").as_double(0), -32768.0, 32767.0));
    const auto glyph_of = [&](const std::u32string& u, std::string_view g) -> std::optional<std::uint16_t> {
      if (u.size() == 1) {
        if (const auto it = spec.cmap.find(static_cast<std::uint32_t>(u[0])); it != spec.cmap.end()) return it->second;
      }
      if (const auto it = by_name.find(g); it != by_name.end()) return it->second;
      return std::nullopt;
    };
    const auto left = glyph_of(u1, node.attribute_local("g1").value());
    const auto right = glyph_of(u2, node.attribute_local("g2").value());
    if (left && right && k != 0) spec.kerning[{*left, *right}] = k;
  }
  spec.postscript_winding = false;
  if (spec.family.empty() || spec.family == "Untitled") {
    const std::string_view id = font.attribute_local("id").value();
    if (!id.empty()) spec.family = std::string(id);
  }
  CVT_TRY_ASSIGN(ir::Font out, build_font(spec, false, ctx));
  return out;
}

std::string path_data(const Path& path) {
  std::string d;
  std::size_t pi = 0;
  const auto n = [](double v) { return util::format_double(std::round(v * 100) / 100); };
  for (const Verb verb : path.verbs) {
    switch (verb) {
      case Verb::Move: d += "M" + n(path.points[pi].x) + " " + n(path.points[pi].y); ++pi; break;
      case Verb::Line: d += "L" + n(path.points[pi].x) + " " + n(path.points[pi].y); ++pi; break;
      case Verb::Quad:
        d += "Q" + n(path.points[pi].x) + " " + n(path.points[pi].y) + " " + n(path.points[pi + 1].x) + " " + n(path.points[pi + 1].y);
        pi += 2;
        break;
      case Verb::Cubic:
        d += "C" + n(path.points[pi].x) + " " + n(path.points[pi].y) + " " + n(path.points[pi + 1].x) + " " +
             n(path.points[pi + 1].y) + " " + n(path.points[pi + 2].x) + " " + n(path.points[pi + 2].y);
        pi += 3;
        break;
      case Verb::Close: d += "Z"; break;
    }
  }
  return d;
}

Result<Bytes> write_svg_font(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::Keep, false, ctx));
  CVT_TRY_ASSIGN(const GlyphSource source, GlyphSource::open(font));
  const std::vector<std::string> names = glyph_names(font);
  std::map<std::uint16_t, std::vector<std::uint32_t>> codes;
  for (const auto& [code, glyph] : source.cmap()) codes[glyph].push_back(code);
  const std::string family = font.names ? font.names->family : std::string("Untitled");
  std::string ps = font.names ? font.names->postscript_name : std::string{};
  if (ps.empty()) ps = "font";
  const ir::FontTable* hhea = font.find("hhea");
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out += "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\">\n<defs>\n";
  out += std::format("<font id=\"{}\" horiz-adv-x=\"{}\">\n", util::xml_escape(ps, true), std::lround(source.advance(0)));
  out += std::format("<font-face font-family=\"{}\" units-per-em=\"{}\" ascent=\"{}\" descent=\"{}\"/>\n",
                     util::xml_escape(family, true), std::lround(source.units_per_em()),
                     hhea ? get_i16(hhea->bytes, 4) : 800, hhea ? get_i16(hhea->bytes, 6) : -200);
  for (std::uint32_t g = 0; g < source.glyph_count(); ++g) {
    if ((g & 127) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(g) / source.glyph_count(), "Writing SVG glyphs"));
    CVT_TRY_ASSIGN(const Path path, source.outline(g));
    const std::string d = path_data(path);
    if (g == 0) {
      out += std::format("<missing-glyph horiz-adv-x=\"{}\" d=\"{}\"/>\n", std::lround(source.advance(0)), d);
      continue;
    }
    const auto it = codes.find(static_cast<std::uint16_t>(g));
    const std::vector<std::uint32_t> list = it == codes.end() ? std::vector<std::uint32_t>{0} : it->second;
    for (const std::uint32_t code : list) {
      std::string attr = std::format("glyph-name=\"{}\"", util::xml_escape(names[g], true));
      if (code != 0) {
        attr += std::format(" unicode=\"&#x{:X};\"", code);
      }
      out += std::format("<glyph {} horiz-adv-x=\"{}\" d=\"{}\"/>\n", attr, std::lround(source.advance(g)), d);
    }
  }
  out += "</font>\n</defs>\n</svg>\n";
  return to_bytes(out);
}

}  // namespace

void register_svg_font(Registry& registry) {
  FormatInfo info;
  info.id = std::string(id_svg_font);
  info.name = "SVG font";
  info.description = "Glyph outlines in an SVG <font> element (SVG 1.1 fonts).";
  info.category = Category::Font;
  info.extensions = {"svg"};
  info.mime = "image/svg+xml";
  info.popularity = 3;
  info.sniff = sniff_svg_font;
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = std::string(id_svg_font);
  codec.kind = Kind::Font;
  codec.read = read_svg_font;
  codec.write = write_svg_font;
  codec.write_cost = 30;
  codec.write_options = subset_option_specs();
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::font
