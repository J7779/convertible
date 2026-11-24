// Convertible core - vector family: helpers shared by the smaller vector formats.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/vector/misc_common.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

#include "families/vector/geom.hpp"
#include "util/text.hpp"

namespace cvt::vector::misc {

namespace {

// --------------------------------------------------------------------------- path data lexer

struct Lexer {
  std::string_view s;
  std::size_t i = 0;

  static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ','; }
  static bool digit(char c) { return c >= '0' && c <= '9'; }

  void skip() {
    while (i < s.size() && space(s[i])) ++i;
  }
  [[nodiscard]] bool at_end() {
    skip();
    return i >= s.size();
  }
  [[nodiscard]] bool number_next() {
    skip();
    if (i >= s.size()) return false;
    const char c = s[i];
    return digit(c) || c == '-' || c == '+' || c == '.';
  }
  bool number(double& out) {
    skip();
    const std::size_t begin = i;
    std::size_t j = i;
    if (j < s.size() && (s[j] == '-' || s[j] == '+')) ++j;
    bool digits = false;
    while (j < s.size() && digit(s[j])) {
      ++j;
      digits = true;
    }
    if (j < s.size() && s[j] == '.') {
      ++j;
      while (j < s.size() && digit(s[j])) {
        ++j;
        digits = true;
      }
    }
    if (!digits) return false;
    if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
      std::size_t k = j + 1;
      if (k < s.size() && (s[k] == '-' || s[k] == '+')) ++k;
      if (k < s.size() && digit(s[k])) {
        while (k < s.size() && digit(s[k])) ++k;
        j = k;
      }
    }
    std::string_view token = s.substr(begin, j - begin);
    if (!token.empty() && token.front() == '+') token.remove_prefix(1);
    const auto value = util::parse_double(token);
    if (!value || !std::isfinite(*value)) return false;
    out = *value;
    i = j;
    return true;
  }
  bool flag(bool& out) {
    skip();
    if (i >= s.size() || (s[i] != '0' && s[i] != '1')) return false;
    out = s[i] == '1';
    ++i;
    return true;
  }
};

double round_to(double v, double scale) { return std::round(v * scale) / scale; }

// --------------------------------------------------------------------------- path data emitter

struct Emitter {
  std::string out;
  char last_cmd = 0;
  bool after_cmd = true;
  bool prev_has_dot = false;

  [[nodiscard]] static bool needs_separator(bool after, bool prev_dot, std::string_view token) {
    if (after) return false;
    if (token.front() == '-') return false;
    if (token.front() == '.' && prev_dot) return false;
    return true;
  }
  /// Characters needed to append command `c` (omitted when repeated) and the tokens.
  [[nodiscard]] std::size_t cost(char c, const std::vector<std::string>& tokens) const {
    bool after = after_cmd;
    bool dot = prev_has_dot;
    std::size_t n = 0;
    if (c != last_cmd || c == 'M' || c == 'm') {
      n += 1;
      after = true;
    }
    for (const std::string& t : tokens) {
      if (needs_separator(after, dot, t)) ++n;
      n += t.size();
      after = false;
      dot = t.find('.') != std::string::npos || t.find('e') != std::string::npos;
    }
    return n;
  }
  void emit(char c, const std::vector<std::string>& tokens) {
    if (c != last_cmd || c == 'M' || c == 'm') {
      out += c;
      after_cmd = true;
    }
    last_cmd = c;
    for (const std::string& t : tokens) {
      if (needs_separator(after_cmd, prev_has_dot, t)) out += ' ';
      out += t;
      after_cmd = false;
      prev_has_dot = t.find('.') != std::string::npos || t.find('e') != std::string::npos;
    }
  }
};

}  // namespace

// --------------------------------------------------------------------------- parse_path_data

Result<PathParse> parse_path_data(std::string_view text, std::size_t max_segments) {
  PathParse result;
  Path& path = result.path;
  Lexer lx{text};
  char cmd = 0;
  Point cur{}, start{};
  Point last_ctrl{};
  char last_kind = 0;  // 'C' after a cubic, 'Q' after a quadratic
  bool need_move = true;
  const auto ensure_move = [&] {
    if (need_move) {
      path.move_to(cur);
      start = cur;
      need_move = false;
    }
  };
  while (!lx.at_end()) {
    if (path.verbs.size() > max_segments) {
      return fail(Status::Limit, "path data has more than {} segments", max_segments);
    }
    const char c = lx.s[lx.i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      cmd = c;
      ++lx.i;
    } else if (cmd == 0 || cmd == 'Z' || cmd == 'z' || !lx.number_next()) {
      result.error = true;
      break;
    } else if (cmd == 'M') {
      cmd = 'L';
    } else if (cmd == 'm') {
      cmd = 'l';
    }
    const bool rel = cmd >= 'a' && cmd <= 'z';
    const Point base = rel ? cur : Point{0, 0};
    double v[7] = {};
    const auto read = [&](int n) {
      for (int k = 0; k < n; ++k) {
        if (!lx.number(v[k])) return false;
      }
      return true;
    };
    bool ok = true;
    char kind = 0;
    switch (cmd) {
      case 'M':
      case 'm':
        if (!(ok = read(2))) break;
        cur = base + Point{v[0], v[1]};
        path.move_to(cur);
        start = cur;
        need_move = false;
        break;
      case 'L':
      case 'l':
        if (!(ok = read(2))) break;
        ensure_move();
        cur = base + Point{v[0], v[1]};
        path.line_to(cur);
        break;
      case 'H':
      case 'h':
        if (!(ok = read(1))) break;
        ensure_move();
        cur = Point{(rel ? cur.x : 0.0) + v[0], cur.y};
        path.line_to(cur);
        break;
      case 'V':
      case 'v':
        if (!(ok = read(1))) break;
        ensure_move();
        cur = Point{cur.x, (rel ? cur.y : 0.0) + v[0]};
        path.line_to(cur);
        break;
      case 'C':
      case 'c': {
        if (!(ok = read(6))) break;
        ensure_move();
        const Point c1 = base + Point{v[0], v[1]}, c2 = base + Point{v[2], v[3]}, p = base + Point{v[4], v[5]};
        path.cubic_to(c1, c2, p);
        last_ctrl = c2;
        cur = p;
        kind = 'C';
        break;
      }
      case 'S':
      case 's': {
        if (!(ok = read(4))) break;
        ensure_move();
        const Point c1 = last_kind == 'C' ? cur * 2.0 - last_ctrl : cur;
        const Point c2 = base + Point{v[0], v[1]}, p = base + Point{v[2], v[3]};
        path.cubic_to(c1, c2, p);
        last_ctrl = c2;
        cur = p;
        kind = 'C';
        break;
      }
      case 'Q':
      case 'q': {
        if (!(ok = read(4))) break;
        ensure_move();
        const Point c1 = base + Point{v[0], v[1]}, p = base + Point{v[2], v[3]};
        path.quad_to(c1, p);
        last_ctrl = c1;
        cur = p;
        kind = 'Q';
        break;
      }
      case 'T':
      case 't': {
        if (!(ok = read(2))) break;
        ensure_move();
        const Point c1 = last_kind == 'Q' ? cur * 2.0 - last_ctrl : cur;
        const Point p = base + Point{v[0], v[1]};
        path.quad_to(c1, p);
        last_ctrl = c1;
        cur = p;
        kind = 'Q';
        break;
      }
      case 'A':
      case 'a': {
        bool large = false, sweep = false;
        ok = lx.number(v[0]) && lx.number(v[1]) && lx.number(v[2]) && lx.flag(large) && lx.flag(sweep) &&
             lx.number(v[3]) && lx.number(v[4]);
        if (!ok) break;
        ensure_move();
        const Point p = base + Point{v[3], v[4]};
        svg_arc_to(path, cur, v[0], v[1], v[2], large, sweep, p);
        cur = p;
        break;
      }
      case 'Z':
      case 'z':
        if (!need_move) path.close();
        cur = start;
        need_move = true;
        break;
      default:
        ok = false;
        break;
    }
    if (!ok) {
      result.error = true;
      break;
    }
    last_kind = kind;
  }
  return result;
}

// --------------------------------------------------------------------------- numbers

std::string format_number(double value, int precision, bool compact) {
  if (!std::isfinite(value)) return "0";
  precision = std::clamp(precision, 0, 12);
  std::string s;
  if (std::abs(value) >= 1e15) {
    s = util::format_double(value);
  } else {
    std::array<char, 64> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::fixed, precision);
    s.assign(buf.data(), res.ptr);
    if (s.find('.') != std::string::npos) {
      while (!s.empty() && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
    }
  }
  if (s == "-0" || s.empty()) s = "0";
  if (compact) {
    if (s.starts_with("0.")) s.erase(0, 1);
    else if (s.starts_with("-0.")) s.erase(1, 1);
  }
  return s;
}

std::string minify_path_data(const Path& path, int precision) {
  precision = std::clamp(precision, 0, 12);
  const double scale = std::pow(10.0, precision);
  const auto q = [&](Point p) { return Point{round_to(p.x, scale), round_to(p.y, scale)}; };
  const auto num = [&](double v) { return format_number(round_to(v, scale), precision, true); };
  const double half = 0.5 / scale;
  const auto same = [&](Point a, Point b) { return std::abs(a.x - b.x) < half && std::abs(a.y - b.y) < half; };

  Emitter em;
  Point cur{}, start{};
  Point last_ctrl{};
  char last_kind = 0;
  std::size_t i = 0;
  const auto choose = [&](char abs_cmd, std::vector<std::string> abs_tokens, std::vector<std::string> rel_tokens) {
    const char rel_cmd = static_cast<char>(abs_cmd - 'A' + 'a');
    if (em.cost(rel_cmd, rel_tokens) < em.cost(abs_cmd, abs_tokens)) {
      em.emit(rel_cmd, rel_tokens);
    } else {
      em.emit(abs_cmd, abs_tokens);
    }
  };
  for (const Verb verb : path.verbs) {
    const std::size_t n = verb == Verb::Move || verb == Verb::Line ? 1 : verb == Verb::Quad ? 2 : verb == Verb::Cubic ? 3 : 0;
    if (i + n > path.points.size()) break;
    switch (verb) {
      case Verb::Move: {
        const Point p = q(path.points[i]);
        choose('M', {num(p.x), num(p.y)}, {num(p.x - cur.x), num(p.y - cur.y)});
        cur = start = p;
        last_kind = 0;
        break;
      }
      case Verb::Line: {
        const Point p = q(path.points[i]);
        if (std::abs(p.y - cur.y) < half) {
          choose('H', {num(p.x)}, {num(p.x - cur.x)});
          cur.x = p.x;
        } else if (std::abs(p.x - cur.x) < half) {
          choose('V', {num(p.y)}, {num(p.y - cur.y)});
          cur.y = p.y;
        } else {
          choose('L', {num(p.x), num(p.y)}, {num(p.x - cur.x), num(p.y - cur.y)});
          cur = p;
        }
        last_kind = 0;
        break;
      }
      case Verb::Quad: {
        const Point c = q(path.points[i]), p = q(path.points[i + 1]);
        if (last_kind == 'Q' && same(c, q(cur * 2.0 - last_ctrl))) {
          choose('T', {num(p.x), num(p.y)}, {num(p.x - cur.x), num(p.y - cur.y)});
          last_ctrl = q(cur * 2.0 - last_ctrl);
        } else {
          choose('Q', {num(c.x), num(c.y), num(p.x), num(p.y)},
                 {num(c.x - cur.x), num(c.y - cur.y), num(p.x - cur.x), num(p.y - cur.y)});
          last_ctrl = c;
        }
        cur = p;
        last_kind = 'Q';
        break;
      }
      case Verb::Cubic: {
        const Point c1 = q(path.points[i]), c2 = q(path.points[i + 1]), p = q(path.points[i + 2]);
        if (last_kind == 'C' && same(c1, q(cur * 2.0 - last_ctrl))) {
          choose('S', {num(c2.x), num(c2.y), num(p.x), num(p.y)},
                 {num(c2.x - cur.x), num(c2.y - cur.y), num(p.x - cur.x), num(p.y - cur.y)});
        } else {
          choose('C', {num(c1.x), num(c1.y), num(c2.x), num(c2.y), num(p.x), num(p.y)},
                 {num(c1.x - cur.x), num(c1.y - cur.y), num(c2.x - cur.x), num(c2.y - cur.y), num(p.x - cur.x),
                  num(p.y - cur.y)});
        }
        last_ctrl = c2;
        cur = p;
        last_kind = 'C';
        break;
      }
      case Verb::Close:
        em.emit('z', {});
        em.last_cmd = 'z';
        cur = start;
        last_kind = 0;
        break;
    }
    i += n;
  }
  return std::move(em.out);
}

// --------------------------------------------------------------------------- colours

std::optional<Color> parse_android_color(std::string_view text) {
  text = util::trim(text);
  if (text.starts_with("@android:color/") || text.starts_with("@color/")) {
    const std::string_view name = text.substr(text.find('/') + 1);
    if (name == "white") return Color{1, 1, 1, 1};
    if (name == "black") return Color{0, 0, 0, 1};
    if (name == "transparent") return Color{0, 0, 0, 0};
    if (name == "darker_gray") return Color::rgb8(0xaa, 0xaa, 0xaa);
    return std::nullopt;
  }
  if (!text.starts_with('#')) return std::nullopt;
  text.remove_prefix(1);
  std::array<int, 8> h{};
  if (text.size() != 3 && text.size() != 4 && text.size() != 6 && text.size() != 8) return std::nullopt;
  for (std::size_t k = 0; k < text.size(); ++k) {
    const char c = text[k];
    if (c >= '0' && c <= '9') h[k] = c - '0';
    else if (c >= 'a' && c <= 'f') h[k] = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') h[k] = c - 'A' + 10;
    else return std::nullopt;
  }
  switch (text.size()) {
    case 3: return Color::rgb8(h[0] * 17, h[1] * 17, h[2] * 17);
    case 4: return Color::rgb8(h[1] * 17, h[2] * 17, h[3] * 17, h[0] * 17);
    case 6: return Color::rgb8(h[0] * 16 + h[1], h[2] * 16 + h[3], h[4] * 16 + h[5]);
    default: return Color::rgb8(h[2] * 16 + h[3], h[4] * 16 + h[5], h[6] * 16 + h[7], h[0] * 16 + h[1]);
  }
}

std::string android_color_string(const Color& color) {
  const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  constexpr char hex[] = "0123456789ABCDEF";
  std::string s = "#";
  const auto put = [&](int b) {
    s += hex[(b >> 4) & 15];
    s += hex[b & 15];
  };
  if (byte(color.a) != 255) put(byte(color.a));
  put(byte(color.r));
  put(byte(color.g));
  put(byte(color.b));
  return s;
}

// --------------------------------------------------------------------------- transforms

std::optional<Decomposed> decompose(const Matrix& m) {
  if (!std::isfinite(m.a) || !std::isfinite(m.b) || !std::isfinite(m.c) || !std::isfinite(m.d) ||
      !std::isfinite(m.e) || !std::isfinite(m.f)) {
    return std::nullopt;
  }
  Decomposed out;
  out.tx = m.e;
  out.ty = m.f;
  out.sx = std::hypot(m.a, m.b);
  if (out.sx < 1e-12) return std::nullopt;
  const double angle = std::atan2(m.b, m.a);
  out.sy = m.determinant() / out.sx;
  const double cs = std::cos(angle), sn = std::sin(angle);
  const double tol = 1e-9 * std::max({1.0, std::abs(m.c), std::abs(m.d), out.sx});
  if (std::abs(m.c + out.sy * sn) > tol || std::abs(m.d - out.sy * cs) > tol) return std::nullopt;
  out.rotation = angle * 180.0 / std::numbers::pi;
  if (std::abs(out.rotation) < 1e-12) out.rotation = 0.0;
  return out;
}

// --------------------------------------------------------------------------- trimming

namespace {

struct Measured {
  std::vector<std::vector<Point>> lines;
  std::vector<std::vector<double>> cumulative;
  double total = 0.0;
};

Measured measure(const std::vector<Polyline>& polylines) {
  Measured m;
  for (const Polyline& pl : polylines) {
    std::vector<Point> pts = pl.points;
    if (pl.closed && !pts.empty() && !(pts.back() == pts.front())) pts.push_back(pts.front());
    std::vector<double> cum(pts.size(), 0.0);
    for (std::size_t k = 1; k < pts.size(); ++k) cum[k] = cum[k - 1] + length(pts[k] - pts[k - 1]);
    m.total += cum.empty() ? 0.0 : cum.back();
    m.lines.push_back(std::move(pts));
    m.cumulative.push_back(std::move(cum));
  }
  return m;
}

/// Append the piece [a, b] (absolute lengths along the concatenation of `m`).
void piece(Path& out, const Measured& m, double a, double b) {
  if (!(b > a)) return;
  double offset = 0.0;
  for (std::size_t l = 0; l < m.lines.size(); ++l) {
    const auto& pts = m.lines[l];
    const auto& cum = m.cumulative[l];
    if (pts.empty()) continue;
    const double len = cum.back();
    const double lo = std::max(a - offset, 0.0), hi = std::min(b - offset, len);
    if (hi > lo) {
      const auto at = [&](double s) {
        const auto it = std::upper_bound(cum.begin(), cum.end(), s);
        std::size_t k = it == cum.begin() ? 0 : static_cast<std::size_t>(it - cum.begin()) - 1;
        if (k + 1 >= pts.size()) return pts.back();
        const double seg = cum[k + 1] - cum[k];
        return seg > 0 ? lerp(pts[k], pts[k + 1], (s - cum[k]) / seg) : pts[k];
      };
      out.move_to(at(lo));
      for (std::size_t k = 0; k < pts.size(); ++k) {
        if (cum[k] > lo && cum[k] < hi) out.line_to(pts[k]);
      }
      out.line_to(at(hi));
    }
    offset += len;
  }
}

}  // namespace

Path trim_first_contour(const Path& path, double start, double end, double tolerance) {
  std::vector<Polyline> lines = flatten(path, tolerance);
  if (lines.size() > 1) lines.resize(1);
  const Measured m = measure(lines);
  Path out;
  if (m.total <= 0.0) return out;
  start = std::clamp(start, 0.0, 1.0);
  end = std::clamp(end, 0.0, 1.0);
  if (start > end) {
    piece(out, m, start * m.total, m.total);
    piece(out, m, 0.0, end * m.total);
  } else {
    piece(out, m, start * m.total, end * m.total);
  }
  return out;
}

Path trim_path(const Path& path, double start, double end, double tolerance, bool per_contour) {
  const std::vector<Polyline> lines = flatten(path, tolerance);
  Path out;
  const auto trim_one = [&](const Measured& m) {
    if (m.total <= 0.0) return;
    if (start > end) {
      piece(out, m, start * m.total, m.total);
      piece(out, m, 0.0, end * m.total);
    } else {
      piece(out, m, start * m.total, end * m.total);
    }
  };
  start = std::clamp(start, 0.0, 1.0);
  end = std::clamp(end, 0.0, 1.0);
  if (per_contour) {
    for (const Polyline& pl : lines) trim_one(measure(std::vector<Polyline>{pl}));
  } else {
    trim_one(measure(lines));
  }
  return out;
}

// --------------------------------------------------------------------------- gradients

Color gradient_color_at(const std::vector<GradientStop>& stops, double t) {
  if (stops.empty()) return Color{0, 0, 0, 0};
  if (t <= stops.front().offset) return stops.front().color;
  if (t >= stops.back().offset) return stops.back().color;
  for (std::size_t k = 1; k < stops.size(); ++k) {
    if (t <= stops[k].offset) {
      const GradientStop& a = stops[k - 1];
      const GradientStop& b = stops[k];
      const double span = b.offset - a.offset;
      const float u = span > 0 ? static_cast<float>((t - a.offset) / span) : 1.0f;
      return Color{a.color.r + (b.color.r - a.color.r) * u, a.color.g + (b.color.g - a.color.g) * u,
                   a.color.b + (b.color.b - a.color.b) * u, a.color.a + (b.color.a - a.color.a) * u};
    }
  }
  return stops.back().color;
}

Color gradient_average(const Gradient& gradient) {
  constexpr int samples = 16;
  double r = 0, g = 0, b = 0, a = 0;
  for (int k = 0; k < samples; ++k) {
    const Color c = gradient_color_at(gradient.stops, (k + 0.5) / samples);
    r += c.r;
    g += c.g;
    b += c.b;
    a += c.a;
  }
  return Color{static_cast<float>(r / samples), static_cast<float>(g / samples), static_cast<float>(b / samples),
               static_cast<float>(a / samples)};
}

Group sweep_wedges(Point center, const std::vector<GradientStop>& stops, const Path& clip_path, FillRule rule,
                   double opacity, int wedges) {
  Group group;
  group.opacity = opacity;
  auto clip = std::make_shared<ClipPath>();
  clip->items.push_back(ClipPath::Item{clip_path, rule});
  group.clip = std::move(clip);
  const Rect box = bounds(clip_path);
  if (box.empty()) return group;
  double radius = 0.0;
  for (const Point corner : {Point{box.x0, box.y0}, Point{box.x1, box.y0}, Point{box.x0, box.y1}, Point{box.x1, box.y1}}) {
    radius = std::max(radius, length(corner - center));
  }
  radius = radius * 1.05 + 1.0;
  wedges = std::clamp(wedges, 4, 720);
  const double step = 2.0 * std::numbers::pi / wedges;
  for (int k = 0; k < wedges; ++k) {
    const double a0 = step * k;
    const double a1 = step * (k + 1) + step * 0.02;  // tiny overlap hides seams
    Shape wedge;
    wedge.path.move_to(center);
    wedge.path.line_to(center + Point{std::cos(a0), std::sin(a0)} * radius);
    wedge.path.line_to(center + Point{std::cos(a1), std::sin(a1)} * radius);
    wedge.path.close();
    wedge.fill = Paint::solid(gradient_color_at(stops, (k + 0.5) / wedges));
    group.children.push_back(Node{std::move(wedge)});
  }
  return group;
}

}  // namespace cvt::vector::misc
