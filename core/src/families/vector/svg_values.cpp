// Convertible core - vector family: SVG/CSS value syntax.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/vector/svg_values.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>

#include "families/vector/geom.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::vector::svg {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

struct NamedColor {
  std::string_view name;
  std::uint32_t rgb;
};

constexpr NamedColor named_colors[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
    {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
    {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
    {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700},
    {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F},
    {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6},
    {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899},
    {"lightslategrey", 0x778899}, {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00},
    {"limegreen", 0x32CD32}, {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3},
    {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE},
    {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC}, {"mediumvioletred", 0xC71585},
    {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1}, {"moccasin", 0xFFE4B5},
    {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6}, {"olive", 0x808000},
    {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE},
    {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F},
    {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080},
    {"rebeccapurple", 0x663399}, {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1},
    {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57},
    {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB},
    {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA},
    {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080},
    {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE},
    {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00},
    {"yellowgreen", 0x9ACD32},
};

float hue_to_rgb(float p, float q, float t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1.0f / 6) return p + (q - p) * 6 * t;
  if (t < 0.5f) return q;
  if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
  return p;
}

}  // namespace

// ------------------------------------------------------------------ scanner

void Scanner::skip_space() {
  while (pos < text.size() && is_space(text[pos])) ++pos;
}

void Scanner::skip_space_comma() {
  skip_space();
  if (pos < text.size() && text[pos] == ',') {
    ++pos;
    skip_space();
  }
}

std::optional<double> Scanner::number() {
  skip_space();
  const std::size_t start = pos;
  std::size_t p = pos;
  if (p < text.size() && (text[p] == '+' || text[p] == '-')) ++p;
  bool digits = false;
  while (p < text.size() && util::is_ascii_digit(text[p])) ++p, digits = true;
  if (p < text.size() && text[p] == '.') {
    ++p;
    while (p < text.size() && util::is_ascii_digit(text[p])) ++p, digits = true;
  }
  if (!digits) return std::nullopt;
  if (p < text.size() && (text[p] == 'e' || text[p] == 'E')) {
    std::size_t q = p + 1;
    if (q < text.size() && (text[q] == '+' || text[q] == '-')) ++q;
    if (q < text.size() && util::is_ascii_digit(text[q])) {
      while (q < text.size() && util::is_ascii_digit(text[q])) ++q;
      p = q;
    }
  }
  std::string_view s = text.substr(start, p - start);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  const auto v = util::parse_double(s);
  if (!v || !std::isfinite(*v)) return std::nullopt;
  pos = p;
  return *v;
}

std::optional<bool> Scanner::flag() {
  skip_space();
  if (pos < text.size() && (text[pos] == '0' || text[pos] == '1')) return text[pos++] == '1';
  return std::nullopt;
}

std::vector<double> parse_numbers(std::string_view text) {
  std::vector<double> out;
  Scanner s{text};
  while (true) {
    s.skip_space_comma();
    if (s.at_end()) break;
    const auto v = s.number();
    if (!v) break;
    out.push_back(*v);
  }
  return out;
}

// ------------------------------------------------------------------ lengths

std::optional<Length> parse_length(std::string_view text) {
  text = util::trim(text);
  Scanner s{text};
  const auto v = s.number();
  if (!v) return std::nullopt;
  const std::string unit = util::to_lower(util::trim(text.substr(s.pos)));
  Length l{*v, Unit::None};
  if (unit.empty()) l.unit = Unit::None;
  else if (unit == "px") l.unit = Unit::Px;
  else if (unit == "pt") l.unit = Unit::Pt;
  else if (unit == "pc") l.unit = Unit::Pc;
  else if (unit == "mm") l.unit = Unit::Mm;
  else if (unit == "cm") l.unit = Unit::Cm;
  else if (unit == "in") l.unit = Unit::In;
  else if (unit == "em") l.unit = Unit::Em;
  else if (unit == "ex") l.unit = Unit::Ex;
  else if (unit == "rem") l.unit = Unit::Rem;
  else if (unit == "%") l.unit = Unit::Percent;
  else return std::nullopt;
  return l;
}

std::vector<Length> parse_lengths(std::string_view text) {
  std::vector<Length> out;
  for (std::string_view part : util::split(text, ',', false)) {
    std::size_t i = 0;
    part = util::trim(part);
    while (i < part.size()) {
      while (i < part.size() && is_space(part[i])) ++i;
      std::size_t j = i;
      while (j < part.size() && !is_space(part[j])) ++j;
      if (j > i) {
        const auto l = parse_length(part.substr(i, j - i));
        if (!l) return {};
        out.push_back(*l);
      }
      i = j;
    }
  }
  return out;
}

// ------------------------------------------------------------------ colours

std::optional<ColorValue> parse_color(std::string_view text) {
  const std::string t = util::to_lower(util::trim(text));
  if (t.empty()) return std::nullopt;
  if (t == "currentcolor") return ColorValue{{}, true};
  if (t == "transparent") return ColorValue{{0, 0, 0, 0}, false};
  if (t[0] == '#') {
    const std::string_view hex = std::string_view(t).substr(1);
    for (const char c : hex) {
      if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    const auto nib = [&](std::size_t i) { return static_cast<int>(*util::parse_int(hex.substr(i, 1), 16)); };
    const auto byte = [&](std::size_t i) { return static_cast<int>(*util::parse_int(hex.substr(i, 2), 16)); };
    if (hex.size() == 3 || hex.size() == 4) {
      return ColorValue{Color::rgb8(nib(0) * 17, nib(1) * 17, nib(2) * 17, hex.size() == 4 ? nib(3) * 17 : 255)};
    }
    if (hex.size() == 6 || hex.size() == 8) {
      return ColorValue{Color::rgb8(byte(0), byte(2), byte(4), hex.size() == 8 ? byte(6) : 255)};
    }
    return std::nullopt;
  }
  const std::size_t paren = t.find('(');
  if (paren != std::string::npos && t.back() == ')') {
    const std::string fn = std::string(util::trim(std::string_view(t).substr(0, paren)));
    std::string args = t.substr(paren + 1, t.size() - paren - 2);
    // Modern syntax "r g b / a": treat '/' as a separator.
    std::replace(args.begin(), args.end(), '/', ',');
    std::vector<std::pair<double, bool>> values;  // value, percent
    for (std::string_view part : util::split(args, ',', false)) {
      part = util::trim(part);
      std::size_t i = 0;
      while (i < part.size()) {
        while (i < part.size() && is_space(part[i])) ++i;
        std::size_t j = i;
        while (j < part.size() && !is_space(part[j])) ++j;
        if (j > i) {
          std::string_view tok = part.substr(i, j - i);
          bool percent = false;
          if (tok.ends_with('%')) {
            percent = true;
            tok.remove_suffix(1);
          } else if (tok.ends_with("deg")) {
            tok.remove_suffix(3);
          }
          const auto v = util::parse_double(tok);
          if (!v || !std::isfinite(*v)) return std::nullopt;
          values.emplace_back(*v, percent);
        }
        i = j;
      }
    }
    if (values.size() < 3 || values.size() > 4) return std::nullopt;
    float alpha = 1.0f;
    if (values.size() == 4) {
      alpha = static_cast<float>(values[3].second ? values[3].first / 100 : values[3].first);
      alpha = std::clamp(alpha, 0.0f, 1.0f);
    }
    if (fn == "rgb" || fn == "rgba") {
      Color c;
      float* ch[3] = {&c.r, &c.g, &c.b};
      for (std::size_t i = 0; i < 3; ++i) {
        const double v = values[i].second ? values[i].first / 100 : values[i].first / 255;
        *ch[i] = static_cast<float>(std::clamp(v, 0.0, 1.0));
      }
      c.a = alpha;
      return ColorValue{c};
    }
    if (fn == "hsl" || fn == "hsla") {
      float h = static_cast<float>(std::fmod(values[0].first, 360.0) / 360.0);
      if (h < 0) h += 1;
      const float s = std::clamp(static_cast<float>(values[1].first / 100), 0.0f, 1.0f);
      const float l = std::clamp(static_cast<float>(values[2].first / 100), 0.0f, 1.0f);
      const float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
      const float p = 2 * l - q;
      return ColorValue{Color{hue_to_rgb(p, q, h + 1.0f / 3), hue_to_rgb(p, q, h), hue_to_rgb(p, q, h - 1.0f / 3), alpha}};
    }
    return std::nullopt;
  }
  const auto it = std::lower_bound(std::begin(named_colors), std::end(named_colors), t,
                                   [](const NamedColor& a, std::string_view b) { return a.name < b; });
  if (it != std::end(named_colors) && it->name == t) {
    return ColorValue{Color::rgb8(static_cast<int>(it->rgb >> 16), static_cast<int>((it->rgb >> 8) & 0xFF),
                                  static_cast<int>(it->rgb & 0xFF))};
  }
  return std::nullopt;
}

// ------------------------------------------------------------------ transforms

std::optional<Matrix> parse_transform(std::string_view text) {
  Matrix m;
  std::size_t pos = 0;
  while (true) {
    while (pos < text.size() && (is_space(text[pos]) || text[pos] == ',')) ++pos;
    if (pos >= text.size()) break;
    std::size_t start = pos;
    while (pos < text.size() && (util::is_ascii_alpha(text[pos]))) ++pos;
    const std::string name = util::to_lower(text.substr(start, pos - start));
    while (pos < text.size() && is_space(text[pos])) ++pos;
    if (pos >= text.size() || text[pos] != '(') return std::nullopt;
    const std::size_t close = text.find(')', pos);
    if (close == std::string_view::npos) return std::nullopt;
    const std::vector<double> a = parse_numbers(text.substr(pos + 1, close - pos - 1));
    pos = close + 1;
    const double deg = std::numbers::pi / 180.0;
    Matrix t;
    if (name == "matrix" && a.size() == 6) {
      t = {a[0], a[1], a[2], a[3], a[4], a[5]};
    } else if (name == "translate" && (a.size() == 1 || a.size() == 2)) {
      t = Matrix::translate(a[0], a.size() == 2 ? a[1] : 0);
    } else if (name == "scale" && (a.size() == 1 || a.size() == 2)) {
      t = Matrix::scale(a[0], a.size() == 2 ? a[1] : a[0]);
    } else if (name == "rotate" && (a.size() == 1 || a.size() == 3)) {
      t = Matrix::rotate(a[0] * deg);
      if (a.size() == 3) t = Matrix::translate(a[1], a[2]) * t * Matrix::translate(-a[1], -a[2]);
    } else if (name == "skewx" && a.size() == 1) {
      t = Matrix::skew_x(a[0] * deg);
    } else if (name == "skewy" && a.size() == 1) {
      t = Matrix::skew_y(a[0] * deg);
    } else {
      return std::nullopt;
    }
    m = m * t;
  }
  return m;
}

// ------------------------------------------------------------------ path data

Path parse_path_data(std::string_view text, bool* error, std::size_t max_segments) {
  Path path;
  Scanner s{text};
  char cmd = 0;
  Point cur{}, start{}, last_ctrl{};
  char prev = 0;
  bool ok = true;
  std::size_t segments = 0;
  const auto fail_here = [&] {
    ok = false;
    if (error) *error = true;
  };
  while (ok) {
    s.skip_space_comma();
    if (s.at_end()) break;
    if (++segments > max_segments) {
      fail_here();
      break;
    }
    const char c = s.peek();
    if (util::is_ascii_alpha(c)) {
      cmd = c;
      ++s.pos;
    } else if (cmd == 0) {
      fail_here();
      break;
    } else if (cmd == 'M') {
      cmd = 'L';
    } else if (cmd == 'm') {
      cmd = 'l';
    } else if (cmd == 'Z' || cmd == 'z') {
      fail_here();
      break;
    }
    if (path.empty() && cmd != 'M' && cmd != 'm') {
      fail_here();
      break;
    }
    const bool rel = cmd >= 'a' && cmd <= 'z';
    const char up = util::ascii_upper(cmd);
    const auto num = [&]() -> std::optional<double> {
      s.skip_space_comma();
      return s.number();
    };
    const auto pt = [&]() -> std::optional<Point> {
      const auto x = num();
      if (!x) return std::nullopt;
      const auto y = num();
      if (!y) return std::nullopt;
      return rel ? Point{cur.x + *x, cur.y + *y} : Point{*x, *y};
    };
    switch (up) {
      case 'M': {
        const auto p = pt();
        if (!p) return fail_here(), path;
        path.move_to(*p);
        cur = start = *p;
        break;
      }
      case 'L': {
        const auto p = pt();
        if (!p) return fail_here(), path;
        path.line_to(*p);
        cur = *p;
        break;
      }
      case 'H': {
        const auto x = num();
        if (!x) return fail_here(), path;
        cur.x = rel ? cur.x + *x : *x;
        path.line_to(cur);
        break;
      }
      case 'V': {
        const auto y = num();
        if (!y) return fail_here(), path;
        cur.y = rel ? cur.y + *y : *y;
        path.line_to(cur);
        break;
      }
      case 'C': {
        const auto a = pt();
        const auto b = a ? pt() : std::nullopt;
        const auto e = b ? pt() : std::nullopt;
        if (!e) return fail_here(), path;
        path.cubic_to(*a, *b, *e);
        last_ctrl = *b;
        cur = *e;
        break;
      }
      case 'S': {
        const Point a = (util::ascii_upper(prev) == 'C' || util::ascii_upper(prev) == 'S') ? cur * 2 - last_ctrl : cur;
        const auto b = pt();
        const auto e = b ? pt() : std::nullopt;
        if (!e) return fail_here(), path;
        path.cubic_to(a, *b, *e);
        last_ctrl = *b;
        cur = *e;
        break;
      }
      case 'Q': {
        const auto a = pt();
        const auto e = a ? pt() : std::nullopt;
        if (!e) return fail_here(), path;
        path.quad_to(*a, *e);
        last_ctrl = *a;
        cur = *e;
        break;
      }
      case 'T': {
        const Point a = (util::ascii_upper(prev) == 'Q' || util::ascii_upper(prev) == 'T') ? cur * 2 - last_ctrl : cur;
        const auto e = pt();
        if (!e) return fail_here(), path;
        path.quad_to(a, *e);
        last_ctrl = a;
        cur = *e;
        break;
      }
      case 'A': {
        const auto rx = num();
        const auto ry = rx ? num() : std::nullopt;
        const auto rot = ry ? num() : std::nullopt;
        if (!rot) return fail_here(), path;
        s.skip_space_comma();
        const auto large = s.flag();
        s.skip_space_comma();
        const auto sweep = large ? s.flag() : std::nullopt;
        if (!sweep) return fail_here(), path;
        const auto e = pt();
        if (!e) return fail_here(), path;
        svg_arc_to(path, cur, *rx, *ry, *rot, *large, *sweep, *e);
        cur = *e;
        break;
      }
      case 'Z':
        path.close();
        cur = start;
        break;
      default: return fail_here(), path;
    }
    prev = cmd;
  }
  if (error) *error = !ok;
  return path;
}

// ------------------------------------------------------------------ preserveAspectRatio

AspectRatio parse_aspect_ratio(std::string_view text) {
  AspectRatio ar;
  std::vector<std::string_view> words;
  for (std::string_view w : util::split(util::trim(text), ' ', false)) words.push_back(w);
  if (!words.empty() && words[0] == "defer") words.erase(words.begin());
  if (words.empty()) return ar;
  const std::string align = std::string(words[0]);
  if (align == "none") {
    ar.x = ar.y = AspectRatio::Align::None;
  } else if (align.size() == 8) {
    const auto parse = [](std::string_view a) {
      if (a == "Min") return AspectRatio::Align::Min;
      if (a == "Max") return AspectRatio::Align::Max;
      return AspectRatio::Align::Mid;
    };
    ar.x = parse(std::string_view(align).substr(1, 3));
    ar.y = parse(std::string_view(align).substr(5, 3));
  }
  if (words.size() > 1 && words[1] == "slice") ar.slice = true;
  return ar;
}

Matrix view_box_transform(const Rect& vb, const AspectRatio& ar, double x, double y, double w, double h) {
  if (vb.empty() || !(vb.width() > 0) || !(vb.height() > 0)) return Matrix::translate(x, y);
  double sx = w / vb.width(), sy = h / vb.height();
  if (ar.x == AspectRatio::Align::None) {
    return Matrix::translate(x, y) * Matrix::scale(sx, sy) * Matrix::translate(-vb.x0, -vb.y0);
  }
  const double s = ar.slice ? std::max(sx, sy) : std::min(sx, sy);
  const double cw = vb.width() * s, ch = vb.height() * s;
  const auto offset = [](AspectRatio::Align a, double space) {
    switch (a) {
      case AspectRatio::Align::Min: return 0.0;
      case AspectRatio::Align::Max: return space;
      default: return space / 2;
    }
  };
  const double tx = x + offset(ar.x, w - cw), ty = y + offset(ar.y, h - ch);
  return Matrix::translate(tx, ty) * Matrix::scale(s, s) * Matrix::translate(-vb.x0, -vb.y0);
}

std::string url_reference(std::string_view text, std::string_view* rest) {
  text = util::trim(text);
  if (!util::istarts_with(text, "url(")) return {};
  const std::size_t close = text.find(')');
  if (close == std::string_view::npos) return {};
  std::string_view inner = util::trim(text.substr(4, close - 4));
  if (inner.size() >= 2 && (inner.front() == '"' || inner.front() == '\'') && inner.back() == inner.front()) {
    inner = inner.substr(1, inner.size() - 2);
  }
  if (rest) *rest = util::trim(text.substr(close + 1));
  const std::size_t hash = inner.find('#');
  if (hash == std::string_view::npos) return {};
  if (hash != 0) return {};  // references into other files are never followed
  return std::string(inner.substr(1));
}

// ------------------------------------------------------------------ CSS

namespace {

std::string_view strip_comments(std::string_view text, std::string& storage) {
  if (text.find("/*") == std::string_view::npos) return text;
  storage.clear();
  std::size_t i = 0;
  while (i < text.size()) {
    if (text.compare(i, 2, "/*") == 0) {
      const std::size_t end = text.find("*/", i + 2);
      if (end == std::string_view::npos) break;
      i = end + 2;
      storage += ' ';
    } else {
      storage += text[i++];
    }
  }
  return storage;
}

std::optional<CssSelector> parse_selector(std::string_view text) {
  CssSelector sel;
  std::vector<CssCompound> parts;
  std::vector<char> combinators;
  std::size_t i = 0;
  char pending = 0;
  text = util::trim(text);
  int ids = 0, classes = 0, types = 0;
  while (i < text.size()) {
    if (is_space(text[i]) || text[i] == '>' || text[i] == '+' || text[i] == '~') {
      char comb = ' ';
      while (i < text.size() && (is_space(text[i]) || text[i] == '>' || text[i] == '+' || text[i] == '~')) {
        if (text[i] != ' ' && !is_space(text[i])) comb = text[i];
        ++i;
      }
      if (comb == '+' || comb == '~') return std::nullopt;  // sibling combinators are not supported
      pending = comb;
      continue;
    }
    CssCompound c;
    bool any = false;
    while (i < text.size() && !is_space(text[i]) && text[i] != '>' && text[i] != '+' && text[i] != '~') {
      const char ch = text[i];
      const auto ident = [&](std::size_t from) {
        std::size_t j = from;
        while (j < text.size() && (util::is_ascii_alpha(text[j]) || util::is_ascii_digit(text[j]) || text[j] == '-' ||
                                   text[j] == '_' || static_cast<unsigned char>(text[j]) >= 0x80)) {
          ++j;
        }
        return j;
      };
      if (ch == '*') {
        c.type = "*";
        ++i;
      } else if (ch == '#') {
        const std::size_t j = ident(i + 1);
        c.id = std::string(text.substr(i + 1, j - i - 1));
        ++ids;
        i = j;
      } else if (ch == '.') {
        const std::size_t j = ident(i + 1);
        c.classes.emplace_back(text.substr(i + 1, j - i - 1));
        ++classes;
        i = j;
      } else if (ch == '[') {
        const std::size_t close = text.find(']', i);
        if (close == std::string_view::npos) return std::nullopt;
        std::string_view inside = text.substr(i + 1, close - i - 1);
        const std::size_t eq = inside.find('=');
        std::string name, value;
        if (eq == std::string_view::npos) {
          name = std::string(util::trim(inside));
        } else {
          name = std::string(util::trim(inside.substr(0, eq)));
          std::string_view v = util::trim(inside.substr(eq + 1));
          if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
          value = std::string(v);
          if (value.empty()) value = "\x01";
        }
        if (!name.empty() && (name.back() == '~' || name.back() == '|' || name.back() == '^' || name.back() == '$' ||
                              name.back() == '*')) {
          return std::nullopt;
        }
        c.attributes.emplace_back(name, value);
        ++classes;
        i = close + 1;
      } else if (ch == ':') {
        const std::size_t j = ident(i + 1);
        const std::string_view pseudo = text.substr(i + 1, j - i - 1);
        if (pseudo == "first-child") {
          c.first_child = true;
          ++classes;
        } else {
          return std::nullopt;  // dynamic pseudo-classes never match a static picture
        }
        i = j;
      } else if (util::is_ascii_alpha(ch) || ch == '_' || ch == '-') {
        const std::size_t j = ident(i);
        c.type = std::string(text.substr(i, j - i));
        ++types;
        i = j;
      } else {
        return std::nullopt;
      }
      any = true;
    }
    if (!any) return std::nullopt;
    if (!parts.empty()) combinators.push_back(pending ? pending : ' ');
    pending = 0;
    parts.push_back(std::move(c));
  }
  if (parts.empty()) return std::nullopt;
  std::reverse(parts.begin(), parts.end());
  std::reverse(combinators.begin(), combinators.end());
  sel.parts = std::move(parts);
  sel.combinators = std::move(combinators);
  sel.specificity = ids * 10000 + classes * 100 + types;
  return sel;
}

}  // namespace

std::vector<CssDeclaration> parse_declarations(std::string_view text) {
  std::string storage;
  text = strip_comments(text, storage);
  std::vector<CssDeclaration> out;
  for (std::string_view decl : util::split(text, ';', false)) {
    const std::size_t colon = decl.find(':');
    if (colon == std::string_view::npos) continue;
    CssDeclaration d;
    d.property = util::to_lower(util::trim(decl.substr(0, colon)));
    std::string_view value = util::trim(decl.substr(colon + 1));
    const std::size_t bang = value.rfind('!');
    if (bang != std::string_view::npos && util::iequals(util::trim(value.substr(bang + 1)), "important")) {
      d.important = true;
      value = util::trim(value.substr(0, bang));
    }
    d.value = std::string(value);
    if (!d.property.empty()) out.push_back(std::move(d));
  }
  return out;
}

void parse_stylesheet(std::string_view text, std::vector<CssRule>& rules, std::size_t& order) {
  std::string storage;
  text = strip_comments(text, storage);
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (is_space(text[i]) || text[i] == ';')) ++i;
    if (i >= text.size()) break;
    if (text.compare(i, 4, "<!--") == 0) {
      i += 4;
      continue;
    }
    if (text.compare(i, 3, "-->") == 0) {
      i += 3;
      continue;
    }
    if (text[i] == '@') {
      // Skip the at-rule: up to ';' or a balanced block.
      std::size_t j = i;
      while (j < text.size() && text[j] != ';' && text[j] != '{') ++j;
      if (j < text.size() && text[j] == '{') {
        int depth = 0;
        for (; j < text.size(); ++j) {
          if (text[j] == '{') ++depth;
          if (text[j] == '}' && --depth == 0) break;
        }
      }
      i = j + 1;
      continue;
    }
    const std::size_t open = text.find('{', i);
    if (open == std::string_view::npos) break;
    const std::size_t close = text.find('}', open);
    if (close == std::string_view::npos) break;
    CssRule rule;
    rule.order = order++;
    for (std::string_view s : util::split(text.substr(i, open - i), ',', false)) {
      if (auto sel = parse_selector(s)) rule.selectors.push_back(std::move(*sel));
    }
    rule.declarations = parse_declarations(text.substr(open + 1, close - open - 1));
    if (!rule.selectors.empty() && !rule.declarations.empty()) rules.push_back(std::move(rule));
    i = close + 1;
  }
}

std::optional<Bytes> decode_data_uri(std::string_view uri, std::string* mime) {
  uri = util::trim(uri);
  if (!util::istarts_with(uri, "data:")) return std::nullopt;
  const std::size_t comma = uri.find(',');
  if (comma == std::string_view::npos) return std::nullopt;
  const std::string_view header = uri.substr(5, comma - 5);
  const std::string_view payload = uri.substr(comma + 1);
  bool base64 = false;
  for (std::string_view part : util::split(header, ';', false)) {
    if (util::iequals(util::trim(part), "base64")) base64 = true;
  }
  if (mime) *mime = util::to_lower(util::trim(header.substr(0, header.find(';'))));
  if (base64) {
    std::string clean;
    clean.reserve(payload.size());
    for (const char c : payload) {
      if (!is_space(c)) clean += c;
    }
    // Tolerate percent-encoded padding and missing padding.
    clean = util::replace_all(clean, "%3D", "=");
    while (clean.size() % 4 != 0) clean += '=';
    auto bytes = util::base64_decode(clean);
    if (!bytes) return std::nullopt;
    return std::move(*bytes);
  }
  Bytes out;
  for (std::size_t i = 0; i < payload.size(); ++i) {
    if (payload[i] == '%' && i + 2 < payload.size()) {
      const auto v = util::parse_int(payload.substr(i + 1, 2), 16);
      if (v) {
        out.push_back(static_cast<std::byte>(*v));
        i += 2;
        continue;
      }
    }
    out.push_back(static_cast<std::byte>(payload[i]));
  }
  return out;
}

}  // namespace cvt::vector::svg
