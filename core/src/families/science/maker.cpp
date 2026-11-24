// Convertible core - science family: engineering / maker files.
//   G-code (RS-274 / 3D printer and CNC dialects) -> toolpath summary (Data)
//     and a top-down preview of the moves (Image);
//   Excellon drill files -> tool/hit summary (Data) and a drill map (Image);
//   Gerber RS-274X -> aperture/statistics summary (Data) and a rendered
//     layer (Image): circle/rectangle/obround/polygon apertures, draws, arcs,
//     flashes, regions and clear polarity (aperture macros are drawn as their
//     bounding circle with a warning).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <numbers>

#include "families/science/common.hpp"
#include "families/science/science.hpp"
#include "util/text.hpp"

namespace cvt::science {

namespace {

struct Point {
  double x = 0, y = 0;
};

struct Bounds {
  double min_x = 1e300, min_y = 1e300, max_x = -1e300, max_y = -1e300;
  void add(double x, double y, double r = 0) {
    min_x = std::min(min_x, x - r);
    min_y = std::min(min_y, y - r);
    max_x = std::max(max_x, x + r);
    max_y = std::max(max_y, y + r);
  }
  [[nodiscard]] bool valid() const { return min_x <= max_x && min_y <= max_y; }
};

/// A simple coverage canvas in drawing units, rendered to an RGBA frame.
class Canvas {
 public:
  Canvas(const Bounds& b, std::uint32_t max_size, double margin_fraction) {
    const double w = std::max(1e-9, b.max_x - b.min_x), h = std::max(1e-9, b.max_y - b.min_y);
    const double margin = std::max(w, h) * margin_fraction;
    min_x_ = b.min_x - margin;
    max_y_ = b.max_y + margin;
    const double full_w = w + 2 * margin, full_h = h + 2 * margin;
    scale_ = (max_size - 1) / std::max(full_w, full_h);
    width_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(full_w * scale_)) + 1);
    height_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(full_h * scale_)) + 1);
    value_.assign(static_cast<std::size_t>(width_) * height_, 0);
  }
  [[nodiscard]] std::uint32_t width() const { return width_; }
  [[nodiscard]] std::uint32_t height() const { return height_; }

  /// Paint a capsule (thick segment) with colour index `v` (0 = background).
  void segment(Point a, Point b, double radius, std::uint8_t v) {
    const double px_r = std::max(radius * scale_, 0.5);
    const double ax = (a.x - min_x_) * scale_, ay = (max_y_ - a.y) * scale_;
    const double bx = (b.x - min_x_) * scale_, by = (max_y_ - b.y) * scale_;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - px_r)));
    const int x1 = std::min(static_cast<int>(width_) - 1, static_cast<int>(std::ceil(std::max(ax, bx) + px_r)));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min(ay, by) - px_r)));
    const int y1 = std::min(static_cast<int>(height_) - 1, static_cast<int>(std::ceil(std::max(ay, by) + px_r)));
    const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        double t = len2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        const double cx = ax + t * dx - x, cy = ay + t * dy - y;
        if (cx * cx + cy * cy <= px_r * px_r) value_[static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)] = v;
      }
    }
  }
  void rect(Point c, double w, double h, std::uint8_t v) {
    polygon({{c.x - w / 2, c.y - h / 2}, {c.x + w / 2, c.y - h / 2}, {c.x + w / 2, c.y + h / 2}, {c.x - w / 2, c.y + h / 2}}, v);
  }
  /// Even-odd scanline fill.
  void polygon(const std::vector<Point>& pts, std::uint8_t v) {
    if (pts.size() < 3) return;
    std::vector<std::pair<double, double>> p;
    for (const Point& q : pts) p.emplace_back((q.x - min_x_) * scale_, (max_y_ - q.y) * scale_);
    for (std::uint32_t y = 0; y < height_; ++y) {
      const double sy = y + 0.5;
      std::vector<double> xs;
      for (std::size_t i = 0; i < p.size(); ++i) {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % p.size()];
        if ((a.second <= sy) != (b.second <= sy)) xs.push_back(a.first + (sy - a.second) * (b.first - a.first) / (b.second - a.second));
      }
      std::sort(xs.begin(), xs.end());
      for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
        const int from = std::max(0, static_cast<int>(std::ceil(xs[k] - 0.5)));
        const int to = std::min(static_cast<int>(width_) - 1, static_cast<int>(std::floor(xs[k + 1] - 0.5)));
        for (int x = from; x <= to; ++x) value_[static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)] = v;
      }
    }
  }
  [[nodiscard]] ir::Frame frame(const std::vector<std::array<std::uint8_t, 3>>& palette) const {
    ir::Frame f = ir::Frame::filled(width_, height_, {palette[0][0], palette[0][1], palette[0][2], 255});
    for (std::size_t i = 0; i < value_.size(); ++i) {
      const auto& c = palette[std::min<std::size_t>(value_[i], palette.size() - 1)];
      f.rgba[i * 4] = c[0];
      f.rgba[i * 4 + 1] = c[1];
      f.rgba[i * 4 + 2] = c[2];
    }
    return f;
  }

 private:
  double min_x_ = 0, max_y_ = 0, scale_ = 1;
  std::uint32_t width_ = 1, height_ = 1;
  std::vector<std::uint8_t> value_;
};

std::vector<Point> arc_points(Point from, Point to, Point centre, bool clockwise) {
  const double r = std::hypot(from.x - centre.x, from.y - centre.y);
  double a0 = std::atan2(from.y - centre.y, from.x - centre.x);
  double a1 = std::atan2(to.y - centre.y, to.x - centre.x);
  if (clockwise) {
    while (a1 >= a0) a1 -= 2 * std::numbers::pi;
  } else {
    while (a1 <= a0) a1 += 2 * std::numbers::pi;
  }
  const int steps = std::clamp(static_cast<int>(std::fabs(a1 - a0) * 16), 2, 720);
  std::vector<Point> out;
  for (int i = 1; i <= steps; ++i) {
    const double a = a0 + (a1 - a0) * i / steps;
    out.push_back({centre.x + r * std::cos(a), centre.y + r * std::sin(a)});
  }
  out.back() = to;
  return out;
}

std::vector<OptionSpec> preview_options() {
  return {OptionSpec::integer("preview_size", "Preview size", 512, 16, 8192, "Longest side of the rendered picture, in pixels.")};
}

// --------------------------------------------------------------------------- G-code

struct Move {
  Point a, b;
  bool rapid;
  double z;
};

struct GcodeRun {
  std::vector<Move> moves;
  Json summary = Json::object();
};

std::vector<std::pair<char, double>> gcode_words(std::string_view line) {
  std::vector<std::pair<char, double>> out;
  std::size_t i = 0;
  while (i < line.size()) {
    const char c = line[i];
    if (c == ';') break;
    if (c == '(') {
      const std::size_t close = line.find(')', i);
      i = close == std::string_view::npos ? line.size() : close + 1;
      continue;
    }
    if (util::is_ascii_alpha(c)) {
      std::size_t j = i + 1;
      while (j < line.size() && (util::is_ascii_space(line[j]))) ++j;
      const std::size_t start = j;
      while (j < line.size() && (util::is_ascii_digit(line[j]) || line[j] == '.' || line[j] == '-' || line[j] == '+')) ++j;
      if (const auto v = to_double(line.substr(start, j - start))) out.emplace_back(util::ascii_upper(c), *v);
      i = j;
      continue;
    }
    ++i;
  }
  return out;
}

Result<GcodeRun> parse_gcode(std::string_view text, const Context& ctx) {
  GcodeRun run;
  const auto lines = lines_of(text);
  double x = 0, y = 0, z = 0, e = 0, feed = 0;
  bool absolute = true, absolute_e = true, metric = true;
  int motion = 0;
  double rapid_len = 0, cut_len = 0, time_min = 0, extruded = 0;
  std::map<int, std::uint64_t> tools;
  std::map<std::string, std::uint64_t> gcodes, mcodes;
  Bounds bounds;
  std::vector<double> layers;
  Json temperatures = Json::object();
  std::uint64_t commands = 0;
  for (std::size_t li = 0; li < lines.size(); ++li) {
    if ((li & 8191) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(li) / static_cast<double>(lines.size())));
    const auto words = gcode_words(lines[li]);
    if (words.empty()) continue;
    ++commands;
    std::optional<double> nx, ny, nz, ne, ni, nj, nr;
    bool has_motion_word = false;
    for (const auto& [letter, value] : words) {
      switch (letter) {
        case 'G': {
          const int g = static_cast<int>(std::lround(value * 10));
          ++gcodes["G" + fmt_num(value)];
          if (g == 0 || g == 10 || g == 20 || g == 30) {
            motion = g / 10;
            has_motion_word = true;
          } else if (g == 200) metric = false;
          else if (g == 210) metric = true;
          else if (g == 900) absolute = true;
          else if (g == 910) absolute = false;
          else if (g == 920) {
            // Set position: handled after the coordinates are read.
            motion = -1;
          }
          break;
        }
        case 'M': {
          ++mcodes["M" + fmt_num(value)];
          if (value == 82) absolute_e = true;
          if (value == 83) absolute_e = false;
          break;
        }
        case 'X': nx = value; break;
        case 'Y': ny = value; break;
        case 'Z': nz = value; break;
        case 'E': ne = value; break;
        case 'I': ni = value; break;
        case 'J': nj = value; break;
        case 'R': nr = value; break;
        case 'F': feed = value; break;
        case 'T': ++tools[static_cast<int>(value)]; break;
        case 'S': {
          bool temp = false;
          for (const auto& w : words) temp = temp || (w.first == 'M' && (w.second == 104 || w.second == 109 || w.second == 140 || w.second == 190));
          if (temp) {
            for (const auto& w : words) {
              if (w.first == 'M') temperatures[(w.second == 140 || w.second == 190) ? "bed" : "hotend"] = json_number(value);
            }
          }
          break;
        }
        default: break;
      }
    }
    if (motion == -1) {  // G92
      if (nx) x = *nx;
      if (ny) y = *ny;
      if (nz) z = *nz;
      if (ne) e = *ne;
      motion = 1;
      continue;
    }
    (void)has_motion_word;
    if (!nx && !ny && !nz && !ne) continue;
    const double tx = nx ? (absolute ? *nx : x + *nx) : x;
    const double ty = ny ? (absolute ? *ny : y + *ny) : y;
    const double tz = nz ? (absolute ? *nz : z + *nz) : z;
    double de = 0;
    if (ne) {
      de = absolute_e ? *ne - e : *ne;
      e = absolute_e ? *ne : e + *ne;
    }
    const bool extruding = de > 1e-9;
    std::vector<Point> path;
    if ((motion == 2 || motion == 3) && (ni || nj || nr)) {
      Point centre{x + ni.value_or(0), y + nj.value_or(0)};
      if (nr && !ni && !nj) {
        const double d = std::hypot(tx - x, ty - y);
        const double h = std::sqrt(std::max(0.0, *nr * *nr - d * d / 4));
        const double mx = (x + tx) / 2, my = (y + ty) / 2;
        const double sign = (motion == 2) == (*nr > 0) ? 1 : -1;
        centre = {mx - sign * h * (ty - y) / std::max(d, 1e-12), my + sign * h * (tx - x) / std::max(d, 1e-12)};
      }
      path = arc_points({x, y}, {tx, ty}, centre, motion == 2);
    } else {
      path.push_back({tx, ty});
    }
    Point prev{x, y};
    double length = 0;
    for (const Point& p : path) {
      length += std::hypot(p.x - prev.x, p.y - prev.y);
      if (run.moves.size() < 2'000'000) run.moves.push_back({prev, p, motion == 0, tz});
      prev = p;
    }
    length = std::hypot(length, tz - z);
    if (motion == 0) {
      rapid_len += length;
    } else {
      cut_len += length;
      if (feed > 0) time_min += length / feed;
      bounds.add(x, y);
      for (const Point& p : path) bounds.add(p.x, p.y);
    }
    if (extruding) {
      extruded += de;
      if (layers.empty() || std::fabs(layers.back() - tz) > 1e-6) layers.push_back(tz);
    }
    x = tx;
    y = ty;
    z = tz;
  }
  if (commands == 0) return fail(Status::Decode, "no G-code commands found");
  Json& s = run.summary;
  s["units"] = metric ? "mm" : "inch";
  s["commands"] = commands;
  s["moves"] = static_cast<std::uint64_t>(run.moves.size());
  s["cutting_distance"] = json_number(std::round(cut_len * 1000) / 1000);
  s["rapid_distance"] = json_number(std::round(rapid_len * 1000) / 1000);
  s["estimated_minutes"] = json_number(std::round(time_min * 100) / 100);
  if (bounds.valid()) {
    s["bounds"] = Json::object({{"min", Json::array({json_number(bounds.min_x), json_number(bounds.min_y)})},
                                {"max", Json::array({json_number(bounds.max_x), json_number(bounds.max_y)})}});
  }
  if (extruded > 0) {
    s["filament_extruded"] = json_number(std::round(extruded * 1000) / 1000);
    s["layers"] = static_cast<std::uint64_t>(layers.size());
  }
  if (!temperatures.empty()) s["temperatures"] = std::move(temperatures);
  Json t = Json::object();
  for (const auto& [tool, n] : tools) t["T" + std::to_string(tool)] = n;
  if (!t.empty()) s["tools"] = std::move(t);
  Json g = Json::object();
  for (const auto& [k, n] : gcodes) g[k] = n;
  for (const auto& [k, n] : mcodes) g[k] = n;
  s["codes"] = std::move(g);
  return run;
}

Result<AnyIR> gcode_image(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const GcodeRun run, parse_gcode(input_text(ctx.input), ctx));
  Bounds b;
  for (const Move& m : run.moves) {
    if (m.rapid && !ctx.options.get_bool("show_travel", false)) continue;
    b.add(m.a.x, m.a.y);
    b.add(m.b.x, m.b.y);
  }
  if (!b.valid()) return fail(Status::Decode, "the G-code has no cutting or printing moves");
  const auto size = static_cast<std::uint32_t>(ctx.options.get_int("preview_size", 512));
  CVT_TRY(ctx.limits.check_pixels(size, size));
  Canvas canvas(b, size, 0.03);
  const double line = std::max(b.max_x - b.min_x, b.max_y - b.min_y) / size * 0.75;
  for (const Move& m : run.moves) {
    if (m.rapid) {
      if (ctx.options.get_bool("show_travel", false)) canvas.segment(m.a, m.b, line / 2, 2);
    } else {
      canvas.segment(m.a, m.b, line, 1);
    }
  }
  ir::Image image;
  image.frames.push_back(canvas.frame({{{255, 255, 255}, {20, 60, 160}, {200, 200, 200}}}));
  return image;
}

// --------------------------------------------------------------------------- Excellon

struct Drill {
  std::map<int, double> tools;  // number -> diameter
  std::vector<std::pair<int, Point>> hits;
  bool metric = false;
};

Result<Drill> parse_excellon(std::string_view text, const Context& ctx) {
  Drill d;
  int tool = 0;
  bool header = false;
  bool leading_zeros = true;  // LZ keeps leading zeros (trailing suppressed)
  int int_digits = 2, dec_digits = 4;
  bool incremental = false;
  Point pos{};
  const auto number = [&](std::string_view s) -> double {
    if (s.find('.') != std::string_view::npos) return to_double(s).value_or(0);
    bool neg = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
      neg = s[0] == '-';
      s.remove_prefix(1);
    }
    std::string digits(s);
    if (leading_zeros) {
      digits.resize(static_cast<std::size_t>(int_digits + dec_digits), '0');  // trailing zeros were suppressed
    } else {
      digits.insert(0, std::string(static_cast<std::size_t>(std::max(0, int_digits + dec_digits - static_cast<int>(digits.size()))), '0'));
    }
    const double v = static_cast<double>(to_int(digits).value_or(0)) / std::pow(10.0, dec_digits);
    return neg ? -v : v;
  };
  for (const auto raw : lines_of(text)) {
    const std::string line(util::trim(raw));
    if (line.empty() || line.starts_with(';')) continue;
    if (line == "M48") {
      header = true;
      continue;
    }
    if (line == "%" || line == "M95") {
      header = false;
      continue;
    }
    if (line.starts_with("METRIC") || line.starts_with("INCH")) {
      d.metric = line.starts_with("METRIC");
      if (d.metric) {
        int_digits = 3;
        dec_digits = 3;
      }
      leading_zeros = line.find("TZ") == std::string::npos;  // "TZ" = trailing zeros kept, leading suppressed
      if (line.find("LZ") != std::string::npos) leading_zeros = true;
      if (const std::size_t fmt = line.find("000."); fmt != std::string::npos) {
        const std::size_t dot = line.find('.', fmt);
        std::size_t zeros_before = 0, zeros_after = 0;
        for (std::size_t k = line.find_last_of(',', dot) + 1; k < dot; ++k) zeros_before += line[k] == '0';
        for (std::size_t k = dot + 1; k < line.size() && line[k] == '0'; ++k) ++zeros_after;
        if (zeros_before > 0 && zeros_after > 0) {
          int_digits = static_cast<int>(zeros_before);
          dec_digits = static_cast<int>(zeros_after);
        }
      }
      continue;
    }
    if (line == "M71") d.metric = true;
    if (line == "M72") d.metric = false;
    if (line == "G91") incremental = true;
    if (line == "G90") incremental = false;
    if (line.starts_with('T')) {
      std::size_t i = 1;
      while (i < line.size() && util::is_ascii_digit(line[i])) ++i;
      tool = static_cast<int>(to_int(std::string_view(line).substr(1, i - 1)).value_or(0));
      const std::size_t c = line.find('C', i);
      if (c != std::string::npos) {
        std::size_t j = c + 1;
        while (j < line.size() && (util::is_ascii_digit(line[j]) || line[j] == '.')) ++j;
        d.tools[tool] = to_double(std::string_view(line).substr(c + 1, j - c - 1)).value_or(0);
      }
      continue;
    }
    if (header) continue;
    if (line.starts_with('X') || line.starts_with('Y')) {
      const std::size_t xi = line.find('X'), yi = line.find('Y');
      const auto field = [&](std::size_t at) {
        std::size_t j = at + 1;
        while (j < line.size() && (util::is_ascii_digit(line[j]) || line[j] == '.' || line[j] == '-' || line[j] == '+')) ++j;
        return std::string_view(line).substr(at + 1, j - at - 1);
      };
      Point p = pos;
      if (xi != std::string::npos) p.x = number(field(xi)) + (incremental ? pos.x : 0);
      if (yi != std::string::npos) p.y = number(field(yi)) + (incremental ? pos.y : 0);
      pos = p;
      if (line.find("G85") != std::string::npos) continue;  // slots: start point only
      d.hits.emplace_back(tool, p);
      if (d.hits.size() > 5'000'000) return fail(Status::Limit, "too many drill hits");
    }
  }
  if (d.hits.empty() && d.tools.empty()) return fail(Status::Decode, "not an Excellon drill file");
  CVT_TRY(check_cells(ctx, d.hits.size()));
  return d;
}

// --------------------------------------------------------------------------- Gerber

struct Aperture {
  char type = 'C';
  std::vector<double> params;
  [[nodiscard]] double radius() const {
    if (params.empty()) return 0;
    if (type == 'R' || type == 'O') return std::hypot(params[0], params.size() > 1 ? params[1] : params[0]) / 2;
    return params[0] / 2;
  }
};

struct GerberResult {
  Json summary = Json::object();
  std::vector<std::function<void(Canvas&)>> ops;
  Bounds bounds;
};

Result<GerberResult> parse_gerber(std::string_view text, const Context& ctx) {
  GerberResult out;
  std::map<int, Aperture> apertures;
  int int_x = 2, dec_x = 6, int_y = 2, dec_y = 6;
  bool trailing = false;  // omitted zeros: L (leading, default) or T
  bool metric = true;
  int current = 0;
  int interpolation = 1;
  bool region = false;
  bool clear = false;
  bool multi_quadrant = true;
  Point pos{};
  std::vector<Point> region_points;
  std::uint64_t draws = 0, flashes = 0, regions = 0, macros = 0;
  bool format_seen = false;
  const auto coordinate = [&](std::string_view s, int ints, int decs) -> double {
    if (s.find('.') != std::string_view::npos) return to_double(s).value_or(0);
    bool neg = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
      neg = s[0] == '-';
      s.remove_prefix(1);
    }
    std::string digits(s);
    if (trailing) digits.resize(static_cast<std::size_t>(ints + decs), '0');
    const double v = static_cast<double>(to_int(digits).value_or(0)) / std::pow(10.0, decs);
    return neg ? -v : v;
  };
  // Split into commands at '*' (extended commands inside %...%).
  std::size_t i = 0;
  std::uint64_t count = 0;
  while (i < text.size()) {
    if (++count % 65536 == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(text.size())));
    if (util::is_ascii_space(text[i])) {
      ++i;
      continue;
    }
    if (text[i] == '%') {
      const std::size_t end = text.find('%', i + 1);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated Gerber extended command");
      for (const auto part : util::split(text.substr(i + 1, end - i - 1), '*')) {
        std::string cmd;
        for (const char c : part) {
          if (c != '\n' && c != '\r') cmd.push_back(c);
        }
        if (cmd.starts_with("FS")) {
          format_seen = true;
          trailing = cmd.size() > 2 && cmd[2] == 'T';
          const std::size_t xp = cmd.find('X'), yp = cmd.find('Y');
          if (xp != std::string::npos && xp + 2 < cmd.size()) {
            int_x = cmd[xp + 1] - '0';
            dec_x = cmd[xp + 2] - '0';
          }
          if (yp != std::string::npos && yp + 2 < cmd.size()) {
            int_y = cmd[yp + 1] - '0';
            dec_y = cmd[yp + 2] - '0';
          }
          if (int_x < 0 || int_x > 7 || dec_x < 0 || dec_x > 7 || int_y < 0 || int_y > 7 || dec_y < 0 || dec_y > 7) {
            return fail(Status::Decode, "bad Gerber format specification");
          }
        } else if (cmd.starts_with("MO")) {
          metric = cmd.substr(2, 2) == "MM";
        } else if (cmd.starts_with("ADD")) {
          std::size_t k = 3;
          while (k < cmd.size() && util::is_ascii_digit(cmd[k])) ++k;
          const int code = static_cast<int>(to_int(std::string_view(cmd).substr(3, k - 3)).value_or(0));
          Aperture a;
          const std::size_t comma = cmd.find(',', k);
          const std::string name = cmd.substr(k, comma == std::string::npos ? std::string::npos : comma - k);
          if (name == "C" || name == "R" || name == "O" || name == "P") {
            a.type = name[0];
          } else {
            a.type = 'M';
            ++macros;
          }
          if (comma != std::string::npos) {
            for (const auto v : util::split(std::string_view(cmd).substr(comma + 1), 'X')) a.params.push_back(to_double(v).value_or(0));
          }
          apertures[code] = std::move(a);
        } else if (cmd.starts_with("LP")) {
          clear = cmd.size() > 2 && cmd[2] == 'C';
        } else if (cmd.starts_with("AM")) {
          // Aperture macro definitions: only their names matter here.
        }
      }
      i = end + 1;
      continue;
    }
    const std::size_t end = text.find('*', i);
    if (end == std::string_view::npos) break;
    std::string cmd;
    for (const char c : text.substr(i, end - i)) {
      if (c != '\n' && c != '\r' && c != ' ') cmd.push_back(c);
    }
    i = end + 1;
    if (cmd.empty() || cmd.starts_with("G04")) continue;
    if (cmd == "M02" || cmd == "M00") break;
    if (cmd == "G36") {
      region = true;
      region_points.clear();
      continue;
    }
    if (cmd == "G37") {
      region = false;
      if (region_points.size() >= 3) {
        ++regions;
        const auto pts = region_points;
        const std::uint8_t v = clear ? 0 : 1;
        out.ops.push_back([pts, v](Canvas& c) { c.polygon(pts, v); });
        for (const Point& p : pts) out.bounds.add(p.x, p.y);
      }
      region_points.clear();
      continue;
    }
    if (cmd == "G74") multi_quadrant = false;
    if (cmd == "G75") multi_quadrant = true;
    (void)multi_quadrant;
    // G01/G02/G03 may prefix coordinates.
    std::size_t k = 0;
    while (k + 2 < cmd.size() && cmd[k] == 'G') {
      const int g = static_cast<int>(to_int(std::string_view(cmd).substr(k + 1, 2)).value_or(-1));
      if (g == 1 || g == 2 || g == 3) interpolation = g;
      k += 3;
    }
    cmd.erase(0, k);
    if (cmd.starts_with("D") && !cmd.empty()) {
      const int d = static_cast<int>(to_int(std::string_view(cmd).substr(1)).value_or(0));
      if (d >= 10) {
        current = d;
        continue;
      }
    }
    std::optional<double> x, y, ci, cj;
    int dcode = 0;
    for (std::size_t p = 0; p < cmd.size();) {
      const char letter = cmd[p];
      std::size_t q = p + 1;
      while (q < cmd.size() && (util::is_ascii_digit(cmd[q]) || cmd[q] == '-' || cmd[q] == '+' || cmd[q] == '.')) ++q;
      const std::string_view v = std::string_view(cmd).substr(p + 1, q - p - 1);
      if (letter == 'X') x = coordinate(v, int_x, dec_x);
      else if (letter == 'Y') y = coordinate(v, int_y, dec_y);
      else if (letter == 'I') ci = coordinate(v, int_x, dec_x);
      else if (letter == 'J') cj = coordinate(v, int_y, dec_y);
      else if (letter == 'D') dcode = static_cast<int>(to_int(v).value_or(0));
      if (q == p + 1) ++q;
      p = q;
    }
    const Point target{x.value_or(pos.x), y.value_or(pos.y)};
    if (dcode == 0 && (x || y)) dcode = 1;  // deprecated modal D01
    const std::uint8_t v = clear ? 0 : 1;
    if (dcode == 2) {
      if (region && region_points.size() >= 3) {
        ++regions;
        const auto pts = region_points;
        out.ops.push_back([pts, v](Canvas& c) { c.polygon(pts, v); });
      }
      if (region) region_points = {target};
      pos = target;
      continue;
    }
    std::vector<Point> path;
    if (dcode == 1 && (interpolation == 2 || interpolation == 3)) {
      path = arc_points(pos, target, {pos.x + ci.value_or(0), pos.y + cj.value_or(0)}, interpolation == 2);
    } else {
      path = {target};
    }
    if (dcode == 1) {
      if (region) {
        if (region_points.empty()) region_points.push_back(pos);
        region_points.insert(region_points.end(), path.begin(), path.end());
      } else {
        const auto it = apertures.find(current);
        const double r = it == apertures.end() ? 0 : (it->second.type == 'C' ? it->second.radius() : it->second.radius());
        Point prev = pos;
        for (const Point& p : path) {
          out.ops.push_back([prev, p, r, v](Canvas& c) { c.segment(prev, p, r, v); });
          out.bounds.add(p.x, p.y, r);
          out.bounds.add(prev.x, prev.y, r);
          prev = p;
        }
        ++draws;
      }
    } else if (dcode == 3) {
      const auto it = apertures.find(current);
      if (it == apertures.end()) return fail(Status::Decode, "Gerber flash with undefined aperture D{}", current);
      const Aperture a = it->second;
      const Point at = target;
      if (a.type == 'R' && a.params.size() >= 2) {
        out.ops.push_back([a, at, v](Canvas& c) { c.rect(at, a.params[0], a.params[1], v); });
      } else if (a.type == 'O' && a.params.size() >= 2) {
        const double w = a.params[0], h = a.params[1];
        const double r = std::min(w, h) / 2;
        const Point p0 = w > h ? Point{at.x - (w / 2 - r), at.y} : Point{at.x, at.y - (h / 2 - r)};
        const Point p1 = w > h ? Point{at.x + (w / 2 - r), at.y} : Point{at.x, at.y + (h / 2 - r)};
        out.ops.push_back([p0, p1, r, v](Canvas& c) { c.segment(p0, p1, r, v); });
      } else if (a.type == 'P' && a.params.size() >= 2) {
        const int n = std::clamp(static_cast<int>(a.params[1]), 3, 64);
        const double rot = a.params.size() > 2 ? a.params[2] * std::numbers::pi / 180 : 0;
        std::vector<Point> pts;
        for (int s = 0; s < n; ++s) {
          const double ang = rot + 2 * std::numbers::pi * s / n;
          pts.push_back({at.x + a.params[0] / 2 * std::cos(ang), at.y + a.params[0] / 2 * std::sin(ang)});
        }
        out.ops.push_back([pts, v](Canvas& c) { c.polygon(pts, v); });
      } else {
        const double r = a.radius();
        out.ops.push_back([at, r, v](Canvas& c) { c.segment(at, at, r, v); });
      }
      if ((a.type == 'R' || a.type == 'O') && a.params.size() >= 2) {
        out.bounds.add(at.x - a.params[0] / 2, at.y - a.params[1] / 2);
        out.bounds.add(at.x + a.params[0] / 2, at.y + a.params[1] / 2);
      } else {
        out.bounds.add(at.x, at.y, a.radius());
      }
      ++flashes;
    }
    pos = target;
    if (out.ops.size() > 5'000'000) return fail(Status::Limit, "too many Gerber objects");
  }
  if (!format_seen && draws + flashes + regions == 0) return fail(Status::Decode, "not a Gerber file");
  if (macros > 0) ctx.warn(std::format("{} aperture macro(s) were drawn as their bounding circle.", macros));
  out.summary["units"] = metric ? "mm" : "inch";
  out.summary["apertures"] = static_cast<std::uint64_t>(apertures.size());
  out.summary["draws"] = draws;
  out.summary["flashes"] = flashes;
  out.summary["regions"] = regions;
  if (out.bounds.valid()) {
    out.summary["bounds"] = Json::object({{"min", Json::array({json_number(out.bounds.min_x), json_number(out.bounds.min_y)})},
                                          {"max", Json::array({json_number(out.bounds.max_x), json_number(out.bounds.max_y)})}});
  }
  return out;
}

}  // namespace

void register_maker(Registry& registry) {
  add_format(registry, {.id = "gcode", .name = "G-code", .description = "CNC / 3D-printer toolpath program.",
                        .category = Category::Model3D, .extensions = {"gcode", "gco", "ngc", "nc", "tap", "cnc"},
                        .mime = "text/x-gcode", .popularity = 25,
                        .sniff = [](ByteView d, std::string_view name) {
                          const std::string_view h = head_text(d, 4096);
                          std::size_t hits = 0;
                          for (const auto line : lines_of(h)) {
                            const std::string_view l = util::trim(line);
                            if (l.starts_with("G0 ") || l.starts_with("G1 ") || l.starts_with("G00") || l.starts_with("G01") ||
                                l.starts_with("G21") || l.starts_with("G90") || l.starts_with("M104") || l.starts_with("G28")) {
                              ++hits;
                            }
                          }
                          if (hits == 0) return 0;
                          return has_extension(name, "gcode") || has_extension(name, "gco") || has_extension(name, "ngc") ? 92 : (hits >= 3 ? 70 : 30);
                        }});
  add_codec(registry, "gcode", Kind::Data,
            [](ReadContext& ctx) -> Result<AnyIR> {
              CVT_TRY_ASSIGN(GcodeRun run, parse_gcode(input_text(ctx.input), ctx));
              return ir::Data{std::move(run.summary)};
            },
            nullptr);
  std::vector<OptionSpec> gopts = preview_options();
  gopts.push_back(OptionSpec::boolean("show_travel", "Show travel moves", false, "Also draw rapid (non-cutting) moves in grey."));
  add_codec(registry, "gcode", Kind::Image, gcode_image, nullptr, gopts, {}, 30, 10, true);

  add_format(registry, {.id = "excellon", .name = "Excellon drill file", .description = "PCB drill tools and hit positions.",
                        .category = Category::Model3D, .extensions = {"drl", "xln", "exc", "drd"}, .mime = "application/x-excellon",
                        .popularity = 12,
                        .sniff = [](ByteView d, std::string_view) {
                          const std::string_view h = head_text(d, 2048);
                          return h.find("M48") != std::string_view::npos && (h.find("METRIC") != std::string_view::npos || h.find("INCH") != std::string_view::npos || h.find("T1C") != std::string_view::npos) ? 90 : 0;
                        }});
  add_codec(registry, "excellon", Kind::Data,
            [](ReadContext& ctx) -> Result<AnyIR> {
              CVT_TRY_ASSIGN(const Drill d, parse_excellon(input_text(ctx.input), ctx));
              Json tools = Json::object();
              for (const auto& [t, dia] : d.tools) {
                std::uint64_t n = 0;
                for (const auto& h : d.hits) n += h.first == t ? 1 : 0;
                tools["T" + std::to_string(t)] = Json::object({{"diameter", json_number(dia)}, {"hits", n}});
              }
              Bounds b;
              for (const auto& h : d.hits) b.add(h.second.x, h.second.y);
              Json out = Json::object({{"units", d.metric ? "mm" : "inch"}, {"tools", std::move(tools)},
                                       {"hits", static_cast<std::uint64_t>(d.hits.size())}});
              if (b.valid()) {
                out["bounds"] = Json::object({{"min", Json::array({json_number(b.min_x), json_number(b.min_y)})},
                                              {"max", Json::array({json_number(b.max_x), json_number(b.max_y)})}});
              }
              return ir::Data{std::move(out)};
            },
            nullptr);
  add_codec(registry, "excellon", Kind::Image,
            [](ReadContext& ctx) -> Result<AnyIR> {
              CVT_TRY_ASSIGN(const Drill d, parse_excellon(input_text(ctx.input), ctx));
              Bounds b;
              for (const auto& h : d.hits) {
                const auto it = d.tools.find(h.first);
                b.add(h.second.x, h.second.y, it == d.tools.end() ? 0 : it->second / 2);
              }
              if (!b.valid()) return fail(Status::Decode, "the drill file has no hits");
              const auto size = static_cast<std::uint32_t>(ctx.options.get_int("preview_size", 512));
              Canvas canvas(b, size, 0.05);
              for (const auto& h : d.hits) {
                const auto it = d.tools.find(h.first);
                canvas.segment(h.second, h.second, it == d.tools.end() ? 0 : it->second / 2, 1);
              }
              ir::Image image;
              image.frames.push_back(canvas.frame({{{255, 255, 255}, {0, 0, 0}}}));
              return image;
            },
            nullptr, preview_options(), {}, 30, 10, true);

  add_format(registry, {.id = "gbr", .name = "Gerber layer", .description = "RS-274X PCB artwork layer.",
                        .category = Category::Model3D, .extensions = {"gbr", "ger", "gtl", "gbl", "gto", "gbo", "gts", "gbs", "gko", "gm1"},
                        .mime = "application/vnd.gerber", .popularity = 15,
                        .sniff = [](ByteView d, std::string_view) {
                          const std::string_view h = head_text(d, 4096);
                          return h.find("%FS") != std::string_view::npos && h.find("%ADD") != std::string_view::npos ? 93 : (h.find("%FSLA") != std::string_view::npos ? 80 : 0);
                        }});
  add_codec(registry, "gbr", Kind::Data,
            [](ReadContext& ctx) -> Result<AnyIR> {
              CVT_TRY_ASSIGN(GerberResult r, parse_gerber(input_text(ctx.input), ctx));
              return ir::Data{std::move(r.summary)};
            },
            nullptr);
  std::vector<OptionSpec> gerber_opts = preview_options();
  gerber_opts.push_back(OptionSpec::text("layer_color", "Layer colour", "b87333", "Colour of copper/artwork (RRGGBB)."));
  add_codec(registry, "gbr", Kind::Image,
            [](ReadContext& ctx) -> Result<AnyIR> {
              CVT_TRY_ASSIGN(const GerberResult r, parse_gerber(input_text(ctx.input), ctx));
              if (!r.bounds.valid()) return fail(Status::Decode, "the Gerber file draws nothing");
              const auto size = static_cast<std::uint32_t>(ctx.options.get_int("preview_size", 512));
              Canvas canvas(r.bounds, size, 0.03);
              for (const auto& op : r.ops) op(canvas);
              const std::string colour = ctx.options.get_string("layer_color", "b87333");
              const auto v = util::parse_int(colour, 16);
              if (colour.size() != 6 || !v) return fail(Status::InvalidArgument, "option 'layer_color' must be six hexadecimal digits");
              ir::Image image;
              image.frames.push_back(canvas.frame({{{255, 255, 255},
                                                    {static_cast<std::uint8_t>(*v >> 16), static_cast<std::uint8_t>(*v >> 8),
                                                     static_cast<std::uint8_t>(*v)}}}));
              return image;
            },
            nullptr, gerber_opts, {}, 30, 10, true);
}

}  // namespace cvt::science
