// Convertible core - vector family: HP-GL and HP-GL/2 plotter files.
//
// Reader: the vector subset of HP-GL/1 and HP-GL/2 (pens, line types and
// attributes, absolute/relative plotting, arcs, circles, rectangles,
// wedges, polygon mode fills, labels, scaling, rotation, polyline encoded
// PE), tolerating PCL/PJL wrappers. Plotter units are 0.025 mm, y up.
// Writer: PU/PD polylines with pens from the stroke colours, PW widths,
// optional polygon fills (HP-GL/2) and labels or outlined text.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/vector/cad_internal.hpp"
#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/vector_codecs.hpp"
#include "util/text.hpp"

namespace cvt::vector::cad {

namespace {

constexpr double pi = std::numbers::pi;
constexpr double mm_per_plu = 0.025;
constexpr double px_per_plu = px_per_mm * mm_per_plu;
constexpr std::uint64_t max_points = 20'000'000;

bool is_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// Position after the escape sequence starting at `pos` (ESC % ... letter,
// ESC <group> <param> values ... <upper-case letter>, or ESC <char>).
std::size_t skip_escape_sequence(std::string_view text, std::size_t pos) {
  ++pos;
  if (pos >= text.size()) return pos;
  const char c = text[pos++];
  if (c == '%') {
    while (pos < text.size() && (util::is_ascii_digit(text[pos]) || text[pos] == '-')) ++pos;
    return std::min(pos + 1, text.size());
  }
  if (c >= '!' && c <= '/') {
    if (pos < text.size()) ++pos;
    while (pos < text.size()) {
      while (pos < text.size() && (util::is_ascii_digit(text[pos]) || text[pos] == '.' || text[pos] == '+' ||
                                   text[pos] == '-')) {
        ++pos;
      }
      if (pos >= text.size()) return pos;
      const char t = text[pos++];
      if (!(t >= '`' && t <= '~')) return pos;  // an upper-case letter ends the sequence
    }
  }
  return pos;
}

// HP-GL/2 default palette (pens 0..7).
Color default_pen(int pen) {
  switch (pen) {
    case 0: return {1, 1, 1, 1};
    case 2: return {1, 0, 0, 1};
    case 3: return {0, 1, 0, 1};
    case 4: return {1, 1, 0, 1};
    case 5: return {0, 0, 1, 1};
    case 6: return {1, 0, 1, 1};
    case 7: return {0, 1, 1, 1};
    default: return {0, 0, 0, 1};
  }
}

// Fixed line types 1..8 as percentages of the pattern length (dash, gap, ...).
std::vector<double> fixed_line_type(int type) {
  switch (std::abs(type)) {
    case 1: return {0, 100};
    case 2: return {50, 50};
    case 3: return {70, 30};
    case 4: return {80, 10, 0, 10};
    case 5: return {70, 10, 10, 10};
    case 6: return {50, 10, 10, 10, 10, 10};
    case 7: return {70, 10, 0, 10, 0, 10};
    case 8: return {50, 10, 0, 10, 10, 10, 0, 10};
    default: return {};
  }
}

class Plotter {
 public:
  Plotter(ReadContext& ctx) : ctx_(ctx) { reset(); }

  Result<Scene> run(std::string_view data) {
    data_ = data;
    while (pos_ < data_.size()) {
      if ((++steps_ & 0x3FFF) == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(pos_) / static_cast<double>(data_.size())));
      const char c = data_[pos_];
      if (c == '\x1b') {
        skip_escape();
        continue;
      }
      if (c == '@' && data_.substr(pos_).starts_with("@PJL")) {
        while (pos_ < data_.size() && data_[pos_] != '\n') ++pos_;
        continue;
      }
      if (!is_letter(c)) {
        ++pos_;
        continue;
      }
      if (pos_ + 1 >= data_.size() || !is_letter(data_[pos_ + 1])) {
        ++pos_;
        continue;
      }
      const std::string cmd{util::ascii_upper(c), util::ascii_upper(data_[pos_ + 1])};
      pos_ += 2;
      CVT_TRY(command(cmd));
    }
    CVT_TRY(flush());
    end_page();
    if (pages_.empty()) return fail(Status::Decode, "the HP-GL file draws nothing");
    Scene scene;
    scene.source_units = "mm";
    scene.pages = std::move(pages_);
    return scene;
  }

 private:
  // ------------------------------------------------------------------ lexing

  void skip_escape() {
    ++pos_;  // ESC
    if (pos_ >= data_.size()) return;
    const char c = data_[pos_++];
    if (c == '%') {
      // ESC % [-]digits letter : enter HP-GL/2 (B), PCL (A) or universal exit (X).
      while (pos_ < data_.size() && (util::is_ascii_digit(data_[pos_]) || data_[pos_] == '-')) ++pos_;
      if (pos_ < data_.size()) {
        const char mode = data_[pos_++];
        if (mode == 'A') skip_pcl();
      }
      return;
    }
    if (c >= '!' && c <= '/') {
      if (pos_ < data_.size()) ++pos_;  // group character
      while (pos_ < data_.size()) {
        while (pos_ < data_.size() && (util::is_ascii_digit(data_[pos_]) || data_[pos_] == '.' || data_[pos_] == '+' ||
                                       data_[pos_] == '-')) {
          ++pos_;
        }
        if (pos_ >= data_.size()) return;
        const char t = data_[pos_++];
        if (t >= '@' && t <= '^') return;  // upper case terminates
        if (!(t >= '`' && t <= '~')) return;
      }
    }
  }

  // PCL mode: skip to the next ESC % ... B (back to HP-GL/2).
  void skip_pcl() {
    while (pos_ < data_.size()) {
      const std::size_t esc = data_.find("\x1b%", pos_);
      if (esc == std::string_view::npos) {
        pos_ = data_.size();
        return;
      }
      std::size_t p = esc + 2;
      while (p < data_.size() && (util::is_ascii_digit(data_[p]) || data_[p] == '-')) ++p;
      if (p < data_.size() && data_[p] == 'B') {
        pos_ = p + 1;
        return;
      }
      pos_ = esc + 2;
    }
  }

  std::vector<double> params() {
    std::vector<double> out;
    while (pos_ < data_.size()) {
      const char c = data_[pos_];
      if (c == ';' ) {
        ++pos_;
        break;
      }
      if (is_letter(c) || c == '\x1b') break;
      if (util::is_ascii_digit(c) || c == '-' || c == '+' || c == '.') {
        const std::size_t start = pos_;
        ++pos_;
        while (pos_ < data_.size() && (util::is_ascii_digit(data_[pos_]) || data_[pos_] == '.')) {
          ++pos_;
        }
        const auto v = util::parse_double(data_.substr(start, pos_ - start));
        if (v && std::isfinite(*v) && out.size() < 1'000'000) out.push_back(std::clamp(*v, -1e12, 1e12));
        continue;
      }
      ++pos_;  // separators: comma, space, other
    }
    return out;
  }

  // ------------------------------------------------------------------ state

  void reset() {
    pen_down_ = false;
    absolute_ = true;
    pen_ = 1;
    pens_.clear();
    widths_.clear();
    relative_width_ = false;
    dash_.clear();
    p1_ = {0, 0};
    p2_ = {10000, 7500};
    scaling_.reset();
    rotation_ = 0;
    char_height_.reset();
    sd_height_.reset();
    direction_ = 0;
    terminator_ = '\x03';
    label_origin_ = 1;
    fill_rule_ = FillRule::EvenOdd;
    fill_type_ = 1;
    shading_ = 100;
    cap_ = LineCap::Butt;
    join_ = LineJoin::Miter;
    polygon_mode_ = false;
    polygon_.clear();
  }

  struct Scaling {
    double ax = 1, bx = 0, ay = 1, by = 0;  // plu = a * user + b
  };

  [[nodiscard]] Point to_plu(Point user) const {
    if (!scaling_) return user;
    return {scaling_->ax * user.x + scaling_->bx, scaling_->ay * user.y + scaling_->by};
  }
  [[nodiscard]] Point to_plu_rel(Point user) const {
    if (!scaling_) return user;
    return {scaling_->ax * user.x, scaling_->ay * user.y};
  }
  [[nodiscard]] double user_length(double v) const { return scaling_ ? v * std::abs(scaling_->ax) : v; }

  [[nodiscard]] Color pen_color(int pen) const {
    const auto it = pens_.find(pen);
    return it != pens_.end() ? it->second : default_pen(pen);
  }
  [[nodiscard]] double pen_width_plu(int pen) const {
    const auto it = widths_.find(pen);
    const double mm = it != widths_.end() ? it->second : 0.35;
    if (relative_width_) {
      return mm / 100.0 * length(p2_ - p1_);  // the value is a percentage of the P1-P2 diagonal
    }
    return mm / mm_per_plu;
  }
  [[nodiscard]] bool ink() const { return pen_ != 0 || pens_.contains(0); }

  [[nodiscard]] Stroke stroke() const {
    Stroke s;
    s.paint = Paint::solid(pen_color(pen_));
    s.width = std::max(pen_width_plu(pen_), 0.1);
    s.cap = cap_;
    s.join = join_;
    s.dash = dash_;
    return s;
  }

  Group& page() { return current_; }

  Result<void> count(std::size_t n) {
    points_ += n + 1;
    if (points_ > max_points) return fail(Status::Limit, "the HP-GL file draws more than {} points", max_points);
    return {};
  }

  Result<void> flush() {
    if (path_.verbs.size() >= 2 && ink()) {
      CVT_TRY(count(path_.points.size()));
      Shape shape;
      shape.path = std::move(path_);
      shape.stroke = stroke();
      page().children.push_back(Node{std::move(shape)});
    }
    path_.clear();
    return {};
  }

  void end_page() {
    if (current_.children.empty()) return;
    Page page;
    const Rect box = content_bounds(current_, Matrix{});
    const Rect r = box.empty() ? Rect{0, 0, 1, 1} : box;
    page.width = std::max(r.width() * px_per_plu, 1.0);
    page.height = std::max(r.height() * px_per_plu, 1.0);
    page.root.transform = Matrix{px_per_plu, 0, 0, -px_per_plu, -r.x0 * px_per_plu, r.y1 * px_per_plu} *
                          Matrix::rotate(rotation_ * pi / 180);
    if (rotation_ != 0) {
      const Rect rotated = content_bounds(current_, Matrix::rotate(rotation_ * pi / 180));
      if (!rotated.empty()) {
        page.width = std::max(rotated.width() * px_per_plu, 1.0);
        page.height = std::max(rotated.height() * px_per_plu, 1.0);
        page.root.transform = Matrix{px_per_plu, 0, 0, -px_per_plu, -rotated.x0 * px_per_plu, rotated.y1 * px_per_plu} *
                              Matrix::rotate(rotation_ * pi / 180);
      }
    }
    page.root.children = std::move(current_.children);
    page.name = std::format("Page {}", pages_.size() + 1);
    current_ = Group{};
    pages_.push_back(std::move(page));
  }

  // A drawing move to `to` (plotter units) with the current pen state.
  Result<void> move(Point to) {
    if (polygon_mode_) {
      if (pen_down_) {
        if (polygon_.empty()) polygon_.move_to(at_);
        polygon_.line_to(to);
      } else {
        polygon_.move_to(to);
      }
      at_ = to;
      return {};
    }
    if (pen_down_) {
      if (path_.empty()) path_.move_to(at_);
      path_.line_to(to);
      if (path_.points.size() > 100'000) {
        const Point last = to;
        CVT_TRY(flush());
        path_.move_to(last);
      }
    } else if (!path_.empty()) {
      CVT_TRY(flush());
    }
    at_ = to;
    return {};
  }

  Result<void> coordinates(const std::vector<double>& p, bool relative) {
    for (std::size_t i = 0; i + 1 < p.size(); i += 2) {
      const Point target = relative ? at_ + to_plu_rel({p[i], p[i + 1]}) : to_plu({p[i], p[i + 1]});
      CVT_TRY(move(target));
    }
    return {};
  }

  // Append an arc (plotter units) from the current point around `center`
  // sweeping `sweep` degrees (positive = counter-clockwise).
  Result<void> arc(Point center, double sweep) {
    const double r = length(at_ - center);
    if (!(r > 0) || !std::isfinite(sweep)) return {};
    sweep = std::clamp(sweep, -360.0, 360.0);
    const double a0 = std::atan2(at_.y - center.y, at_.x - center.x);
    const double a1 = a0 + sweep * pi / 180;
    Path piece;
    add_arc(piece, center, r, r, 0, a0, a1, sweep > 0, false);
    const Point end{center.x + r * std::cos(a1), center.y + r * std::sin(a1)};
    if (polygon_mode_) {
      if (pen_down_) append_piece(polygon_, piece);
      at_ = end;
      return {};
    }
    if (pen_down_) {
      append_piece(path_, piece);
    } else {
      CVT_TRY(flush());
    }
    at_ = end;
    return {};
  }

  static void append_piece(Path& path, const Path& piece) {
    if (piece.empty()) return;
    if (path.empty() || path.verbs.back() == Verb::Close) path.move_to(piece.points.front());
    else if (length(current_point(path) - piece.points.front()) > 1e-9) path.line_to(piece.points.front());
    for (std::size_t v = 1; v < piece.verbs.size(); ++v) path.verbs.push_back(piece.verbs[v]);
    path.points.insert(path.points.end(), piece.points.begin() + 1, piece.points.end());
  }

  Paint fill_paint() const {
    Color c = pen_color(pen_);
    double opacity = 1.0;
    if (fill_type_ == 10) {
      const double level = std::clamp(shading_, 0.0, 100.0) / 100.0;
      c = {static_cast<float>(1 - (1 - c.r) * level), static_cast<float>(1 - (1 - c.g) * level),
           static_cast<float>(1 - (1 - c.b) * level), 1};
    } else if (fill_type_ == 3 || fill_type_ == 4 || fill_type_ == 11) {
      opacity = 0.35;
    }
    return Paint::solid(c, opacity);
  }

  Result<void> emit_fill(Path path) {
    if (path.empty() || !ink()) return {};
    if (fill_type_ == 3 || fill_type_ == 4 || fill_type_ == 11) {
      ctx_.warn("HP-GL hatch and pattern fills were drawn as translucent solid fills.");
    }
    CVT_TRY(count(path.points.size()));
    Shape shape;
    shape.path = std::move(path);
    shape.fill = fill_paint();
    shape.fill_rule = fill_rule_;
    page().children.push_back(Node{std::move(shape)});
    return {};
  }

  Result<void> emit_edge(Path path) {
    if (path.empty() || !ink()) return {};
    CVT_TRY(count(path.points.size()));
    Shape shape;
    shape.path = std::move(path);
    shape.stroke = stroke();
    page().children.push_back(Node{std::move(shape)});
    return {};
  }

  // ------------------------------------------------------------------ labels

  [[nodiscard]] double cap_height() const {
    if (char_height_) return *char_height_;
    if (sd_height_) return *sd_height_;
    return 0.27 * 400;  // 0.27 cm
  }

  Result<void> label(std::string_view text) {
    CVT_TRY(flush());
    const double h = cap_height();
    if (!(h > 0) || !std::isfinite(h)) return {};
    FontSpec font;
    font.family = "sans-serif";
    font.size = h / cap_height_ratio;
    const double c = std::cos(direction_), s = std::sin(direction_);
    Point line_start = at_;
    std::string line;
    const auto draw_line = [&]() -> Result<void> {
      if (line.empty()) return {};
      const std::string utf8 = util::sanitize_utf8(line);
      const TextLayout layout = layout_text(utf8, font);
      const int lo = label_origin_ > 10 ? label_origin_ - 10 : label_origin_;
      const int column = (lo - 1) / 3, row = (lo - 1) % 3;
      const double dx = column == 0 ? 0 : column == 1 ? -layout.advance / 2 : -layout.advance;
      const double dy = row == 0 ? 0 : row == 1 ? -h / 2 : -h;
      Text t = make_text(utf8, font, {dx, -dy}, Paint::solid(pen_color(pen_)));
      CVT_TRY(count(t.outline.points.size()));
      Group g;
      g.transform = Matrix::translate(at_.x, at_.y) * Matrix::rotate(direction_) * Matrix::scale(1, -1);
      g.children.push_back(Node{std::move(t)});
      if (ink()) page().children.push_back(Node{std::move(g)});
      if (column == 0) at_ = at_ + Point{c, s} * layout.advance;
      line.clear();
      return {};
    };
    for (const char ch : text) {
      if (ch == '\r') {
        CVT_TRY(draw_line());
        at_ = line_start;
      } else if (ch == '\n') {
        CVT_TRY(draw_line());
        line_start = line_start + Point{s, -c} * (h * 2);
        at_ = line_start;
      } else if (static_cast<unsigned char>(ch) >= 0x20) {
        line.push_back(ch);
      }
    }
    return draw_line();
  }

  std::string_view label_text() {
    const std::size_t start = pos_;
    while (pos_ < data_.size() && data_[pos_] != terminator_) ++pos_;
    const std::string_view text = data_.substr(start, pos_ - start);
    if (pos_ < data_.size()) ++pos_;
    return text;
  }

  // ------------------------------------------------------------------ PE

  Result<void> polyline_encoded() {
    bool seven_bit = false;
    int fraction = 0;
    bool pen_up_next = false;
    bool absolute_next = false;
    CVT_TRY(flush());
    const auto decode = [&](std::int64_t& out) -> bool {
      std::uint64_t value = 0, factor = 1;
      int digits = 0;
      while (pos_ < data_.size()) {
        const auto b = static_cast<unsigned char>(data_[pos_]);
        if (seven_bit) {
          if (b >= 63 && b <= 94) {
            value += (b - 63) * factor;
            factor *= 32;
          } else if (b >= 95 && b <= 126) {
            value += (b - 95) * factor;
            ++pos_;
            out = (value & 1) != 0 ? -static_cast<std::int64_t>(value >> 1) : static_cast<std::int64_t>(value >> 1);
            return true;
          } else {
            return false;
          }
        } else {
          if (b >= 63 && b <= 126) {
            value += (b - 63) * factor;
            factor *= 64;
          } else if (b >= 191 && b <= 254) {
            value += (b - 191) * factor;
            ++pos_;
            out = (value & 1) != 0 ? -static_cast<std::int64_t>(value >> 1) : static_cast<std::int64_t>(value >> 1);
            return true;
          } else {
            return false;
          }
        }
        ++pos_;
        if (++digits > 10) return false;  // more digits than any coordinate needs
      }
      return false;
    };
    while (pos_ < data_.size()) {
      if ((++steps_ & 0x3FFF) == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(pos_) / static_cast<double>(data_.size())));
      const char c = data_[pos_];
      if (c == ';') {
        ++pos_;
        break;
      }
      if (c == '\x1b') break;
      if (c == ':') {
        ++pos_;
        std::int64_t pen = 0;
        if (!decode(pen)) break;
        CVT_TRY(flush());
        pen_ = static_cast<int>(std::clamp<std::int64_t>(pen, 0, 32767));
        continue;
      }
      if (c == '<') {
        ++pos_;
        pen_up_next = true;
        continue;
      }
      if (c == '>') {
        ++pos_;
        std::int64_t f = 0;
        if (!decode(f)) break;
        fraction = static_cast<int>(std::clamp<std::int64_t>(f, 0, 26));
        continue;
      }
      if (c == '=') {
        ++pos_;
        absolute_next = true;
        continue;
      }
      if (c == '7') {
        ++pos_;
        seven_bit = true;
        continue;
      }
      const auto b = static_cast<unsigned char>(c);
      if (b < 63 || (b > 126 && b < 191) || b > 254 || (seven_bit && b > 126)) {
        ++pos_;  // whitespace and other bytes are ignored
        continue;
      }
      std::int64_t x = 0, y = 0;
      if (!decode(x) || !decode(y)) break;
      const double scale = std::ldexp(1.0, -fraction);
      const Point user{static_cast<double>(x) * scale, static_cast<double>(y) * scale};
      const Point target = absolute_next ? to_plu(user) : at_ + to_plu_rel(user);
      pen_down_ = !pen_up_next;
      CVT_TRY(move(target));
      pen_up_next = false;
      absolute_next = false;
    }
    CVT_TRY(flush());
    pen_down_ = false;
    return {};
  }

  // ------------------------------------------------------------------ commands

  Result<void> command(const std::string& cmd) {
    if (cmd == "LB") return label(label_text());
    if (cmd == "PE") return polyline_encoded();
    if (cmd == "DT") {
      // DT t[,mode]; the terminator is the next character.
      if (pos_ < data_.size() && data_[pos_] != ';') terminator_ = data_[pos_++];
      else terminator_ = '\x03';
      (void)params();
      return {};
    }
    if (cmd == "CO") {
      // Comment: CO "text"
      const std::size_t q = data_.find('"', pos_);
      if (q != std::string_view::npos && q < pos_ + 4) {
        const std::size_t e = data_.find('"', q + 1);
        pos_ = e == std::string_view::npos ? data_.size() : e + 1;
      }
      (void)params();
      return {};
    }
    if (cmd == "WD" || cmd == "BL") {
      (void)label_text();
      return {};
    }
    const std::vector<double> p = params();
    const auto arg = [&](std::size_t i, double fallback) { return i < p.size() ? p[i] : fallback; };
    if (cmd == "IN") {
      CVT_TRY(flush());
      reset();
      at_ = {0, 0};
    } else if (cmd == "DF") {
      CVT_TRY(flush());
      const Point keep = at_;
      const auto pens = pens_;
      reset();
      pens_ = pens;
      at_ = keep;
    } else if (cmd == "PU" || cmd == "PD") {
      if (cmd == "PU") {
        pen_down_ = false;
        if (!path_.empty() && !polygon_mode_) CVT_TRY(flush());
      } else {
        pen_down_ = true;
      }
      CVT_TRY(coordinates(p, !absolute_));
    } else if (cmd == "PA" || cmd == "PR") {
      absolute_ = cmd == "PA";
      CVT_TRY(coordinates(p, !absolute_));
    } else if (cmd == "SP") {
      CVT_TRY(flush());
      pen_ = static_cast<int>(std::clamp(arg(0, 0), 0.0, 32767.0));
    } else if (cmd == "PC") {
      CVT_TRY(flush());
      if (p.size() >= 4) {
        const auto ch = [&](std::size_t i) {
          const double lo = color_range_[2 * (i - 1)], hi = color_range_[2 * (i - 1) + 1];
          return static_cast<float>(std::clamp((p[i] - lo) / (hi - lo != 0 ? hi - lo : 1), 0.0, 1.0));
        };
        pens_[static_cast<int>(std::clamp(p[0], 0.0, 32767.0))] = Color{ch(1), ch(2), ch(3), 1};
      } else if (p.size() == 1) {
        pens_.erase(static_cast<int>(p[0]));
      } else {
        pens_.clear();
      }
    } else if (cmd == "CR") {
      if (p.size() >= 6) {
        for (std::size_t i = 0; i < 6; ++i) color_range_[i] = p[i];
      } else {
        color_range_ = {0, 255, 0, 255, 0, 255};
      }
    } else if (cmd == "NP") {
      // Number of pens: nothing to allocate.
    } else if (cmd == "PW") {
      CVT_TRY(flush());
      const double w = std::clamp(arg(0, 0.35), 0.0, 1000.0);
      if (p.size() >= 2) {
        widths_[static_cast<int>(std::clamp(p[1], 0.0, 32767.0))] = w;
      } else {
        widths_.clear();
        for (int i = 0; i < 256; ++i) widths_[i] = w;
      }
    } else if (cmd == "WU") {
      relative_width_ = arg(0, 0) != 0;
    } else if (cmd == "LT") {
      CVT_TRY(flush());
      dash_.clear();
      if (!p.empty()) {
        const std::vector<double> pattern = fixed_line_type(static_cast<int>(p[0]));
        double len = arg(1, 4);
        const bool absolute_len = arg(2, 0) != 0;
        len = absolute_len ? len / mm_per_plu : len / 100 * length(p2_ - p1_);
        if (!(len > 0) || !std::isfinite(len)) len = 0.04 * length(p2_ - p1_);
        for (const double v : pattern) dash_.push_back(v == 0 ? std::max(len * 0.005, 1.0) : v / 100 * len);
      }
    } else if (cmd == "LA") {
      CVT_TRY(flush());
      for (std::size_t i = 0; i + 1 < p.size(); i += 2) {
        const int kind = static_cast<int>(p[i]), value = static_cast<int>(p[i + 1]);
        if (kind == 1) cap_ = value == 2 ? LineCap::Square : value == 3 || value == 4 ? LineCap::Round : LineCap::Butt;
        if (kind == 2) join_ = value == 4 ? LineJoin::Round : value == 5 || value == 6 || value == 3 ? LineJoin::Bevel : LineJoin::Miter;
      }
      if (p.empty()) {
        cap_ = LineCap::Butt;
        join_ = LineJoin::Miter;
      }
    } else if (cmd == "IP") {
      if (p.size() >= 4) {
        p1_ = {p[0], p[1]};
        p2_ = {p[2], p[3]};
      } else if (p.size() >= 2) {
        const Point d = p2_ - p1_;
        p1_ = {p[0], p[1]};
        p2_ = p1_ + d;
      } else {
        p1_ = {0, 0};
        p2_ = {10000, 7500};
      }
    } else if (cmd == "SC") {
      if (p.size() < 4) {
        scaling_.reset();
      } else {
        Scaling s;
        const int type = static_cast<int>(arg(4, 0));
        if (type == 2) {
          s.ax = p[1];
          s.ay = p[3];
          s.bx = p1_.x - p[0] * p[1];
          s.by = p1_.y - p[2] * p[3];
        } else {
          const double dx = p[1] - p[0], dy = p[3] - p[2];
          if (dx == 0 || dy == 0) return {};
          s.ax = (p2_.x - p1_.x) / dx;
          s.ay = (p2_.y - p1_.y) / dy;
          double ox = p1_.x, oy = p1_.y;
          if (type == 1) {
            const double a = std::min(std::abs(s.ax), std::abs(s.ay));
            const double left = arg(5, 50) / 100, bottom = arg(6, 50) / 100;
            const double sx = std::copysign(a, s.ax), sy = std::copysign(a, s.ay);
            ox += (std::abs(p2_.x - p1_.x) - std::abs(dx * sx)) * left;
            oy += (std::abs(p2_.y - p1_.y) - std::abs(dy * sy)) * bottom;
            s.ax = sx;
            s.ay = sy;
          }
          s.bx = ox - p[0] * s.ax;
          s.by = oy - p[2] * s.ay;
        }
        if (std::isfinite(s.ax) && std::isfinite(s.ay) && std::isfinite(s.bx) && std::isfinite(s.by)) scaling_ = s;
      }
    } else if (cmd == "RO") {
      CVT_TRY(flush());
      const int r = static_cast<int>(arg(0, 0));
      if (current_.children.empty()) rotation_ = ((r % 360) + 360) % 360 / 90 * 90;
    } else if (cmd == "CI") {
      const double r = user_length(arg(0, 0));
      if (r > 0) {
        Path circle;
        add_ellipse(circle, at_.x, at_.y, r, r);
        if (polygon_mode_) {
          append_piece(polygon_, circle);
          polygon_.close();
        } else {
          CVT_TRY(flush());
          CVT_TRY(emit_edge(std::move(circle)));
        }
      }
    } else if (cmd == "AA" || cmd == "AR") {
      if (p.size() >= 3) {
        const Point c = cmd == "AA" ? to_plu({p[0], p[1]}) : at_ + to_plu_rel({p[0], p[1]});
        CVT_TRY(arc(c, p[2]));
      }
    } else if (cmd == "BZ" || cmd == "BR") {
      for (std::size_t i = 0; i + 5 < p.size(); i += 6) {
        const Point base = at_;
        const auto pt = [&](std::size_t k) {
          return cmd == "BZ" ? to_plu({p[i + k], p[i + k + 1]}) : base + to_plu_rel({p[i + k], p[i + k + 1]});
        };
        Path piece;
        piece.move_to(base);
        piece.cubic_to(pt(0), pt(2), pt(4));
        if (polygon_mode_) {
          if (pen_down_) append_piece(polygon_, piece);
        } else if (pen_down_) {
          append_piece(path_, piece);
        }
        at_ = pt(4);
      }
    } else if (cmd == "AT" || cmd == "RT") {
      if (p.size() >= 4) {
        const Point a = at_;
        const Point m = cmd == "AT" ? to_plu({p[0], p[1]}) : at_ + to_plu_rel({p[0], p[1]});
        const Point e = cmd == "AT" ? to_plu({p[2], p[3]}) : at_ + to_plu_rel({p[2], p[3]});
        const double d = 2 * (a.x * (m.y - e.y) + m.x * (e.y - a.y) + e.x * (a.y - m.y));
        if (std::abs(d) < 1e-12) {
          CVT_TRY(move(e));
        } else {
          const double a2 = dot(a, a), m2 = dot(m, m), e2 = dot(e, e);
          const Point c{(a2 * (m.y - e.y) + m2 * (e.y - a.y) + e2 * (a.y - m.y)) / d,
                        (a2 * (e.x - m.x) + m2 * (a.x - e.x) + e2 * (m.x - a.x)) / d};
          double sa = std::atan2(a.y - c.y, a.x - c.x), sm = std::atan2(m.y - c.y, m.x - c.x),
                 se = std::atan2(e.y - c.y, e.x - c.x);
          const auto ccw = [](double from, double to) {
            double v = to - from;
            while (v < 0) v += 2 * pi;
            return v;
          };
          double sweep = ccw(sa, se);
          if (ccw(sa, sm) > sweep) sweep -= 2 * pi;  // clockwise through the middle point
          CVT_TRY(arc(c, sweep * 180 / pi));
          at_ = e;
        }
      }
    } else if (cmd == "EA" || cmd == "ER" || cmd == "RA" || cmd == "RR") {
      if (p.size() >= 2) {
        const bool rel = cmd == "ER" || cmd == "RR";
        const Point e = rel ? at_ + to_plu_rel({p[0], p[1]}) : to_plu({p[0], p[1]});
        Path r;
        r.move_to(at_);
        r.line_to({e.x, at_.y});
        r.line_to(e);
        r.line_to({at_.x, e.y});
        r.close();
        CVT_TRY(flush());
        if (cmd[0] == 'E') CVT_TRY(emit_edge(std::move(r)));
        else CVT_TRY(emit_fill(std::move(r)));
      }
    } else if (cmd == "EW" || cmd == "WG") {
      if (p.size() >= 3) {
        const double r = user_length(p[0]);
        const double a0 = p[1] * pi / 180, a1 = (p[1] + std::clamp(p[2], -360.0, 360.0)) * pi / 180;
        Path w;
        w.move_to(at_);
        add_arc(w, at_, std::abs(r), std::abs(r), 0, a0, a1, p[2] > 0, true);
        w.close();
        CVT_TRY(flush());
        if (cmd == "EW") CVT_TRY(emit_edge(std::move(w)));
        else CVT_TRY(emit_fill(std::move(w)));
      }
    } else if (cmd == "PM") {
      const int mode = static_cast<int>(arg(0, 0));
      if (mode == 0) {
        CVT_TRY(flush());
        polygon_mode_ = true;
        polygon_.clear();
        polygon_.move_to(at_);
      } else if (mode == 1) {
        // Next subpolygon; fills close every subpolygon implicitly, edges
        // are drawn only where the pen went.
        polygon_.move_to(at_);
      } else {
        polygon_mode_ = false;
      }
    } else if (cmd == "FP") {
      fill_rule_ = arg(0, 0) == 1 ? FillRule::NonZero : FillRule::EvenOdd;
      Path poly = polygon_;
      CVT_TRY(emit_fill(std::move(poly)));
    } else if (cmd == "EP") {
      Path poly = polygon_;
      CVT_TRY(emit_edge(std::move(poly)));
    } else if (cmd == "FT") {
      fill_type_ = static_cast<int>(arg(0, 1));
      if (fill_type_ == 10) shading_ = arg(1, 100);
    } else if (cmd == "SI") {
      if (p.size() >= 2) char_height_ = std::abs(p[1]) * 400;  // cm -> plu
      else char_height_.reset();
    } else if (cmd == "SR") {
      if (p.size() >= 2) char_height_ = std::abs(p[1]) / 100 * std::abs(p2_.y - p1_.y);
      else char_height_ = 0.015 * std::abs(p2_.y - p1_.y);
    } else if (cmd == "SD" || cmd == "AD") {
      if (cmd == "SD") {
        for (std::size_t i = 0; i + 1 < p.size(); i += 2) {
          if (static_cast<int>(p[i]) == 4 && p[i + 1] > 0) sd_height_ = p[i + 1] * 25.4 / 72 / mm_per_plu * cap_height_ratio;
        }
      }
    } else if (cmd == "DI" || cmd == "DR") {
      double run = arg(0, 1), rise = arg(1, 0);
      if (cmd == "DR") {
        run *= p2_.x - p1_.x;
        rise *= p2_.y - p1_.y;
      }
      direction_ = (run == 0 && rise == 0) ? 0 : std::atan2(rise, run);
    } else if (cmd == "LO") {
      label_origin_ = static_cast<int>(std::clamp(arg(0, 1), 1.0, 19.0));
      if (label_origin_ == 10) label_origin_ = 1;
    } else if (cmd == "PG" || cmd == "FR") {
      CVT_TRY(flush());
      end_page();
    }
    // Everything else (PS, BP, IW, TR, VS, ...) has no visible effect here.
    return {};
  }

  ReadContext& ctx_;
  std::string_view data_;
  std::size_t pos_ = 0;
  std::uint64_t steps_ = 0;
  std::uint64_t points_ = 0;
  std::vector<Page> pages_;
  Group current_;
  Path path_;
  Path polygon_;
  bool polygon_mode_ = false;
  Point at_{0, 0};
  bool pen_down_ = false;
  bool absolute_ = true;
  int pen_ = 1;
  std::map<int, Color> pens_;
  std::map<int, double> widths_;
  bool relative_width_ = false;
  std::array<double, 6> color_range_{0, 255, 0, 255, 0, 255};
  std::vector<double> dash_;
  Point p1_{0, 0}, p2_{10000, 7500};
  std::optional<Scaling> scaling_;
  int rotation_ = 0;
  std::optional<double> char_height_;
  std::optional<double> sd_height_;
  double direction_ = 0;
  char terminator_ = '\x03';
  int label_origin_ = 1;
  FillRule fill_rule_ = FillRule::EvenOdd;
  int fill_type_ = 1;
  double shading_ = 100;
  LineCap cap_ = LineCap::Butt;
  LineJoin join_ = LineJoin::Miter;
};

Result<Scene> read_hpgl(ReadContext& ctx) {
  Plotter plotter(ctx);
  return plotter.run(as_chars(ctx.input));
}

// --------------------------------------------------------------------------- writer

struct PenTable {
  std::vector<Color> colors;  // index 0 = pen 1
  bool hpgl1 = false;
  int pen_for(Color c) {
    if (hpgl1) {
      int best = 1;
      double best_d = 1e300;
      for (int pen = 1; pen <= 7; ++pen) {
        const Color d = default_pen(pen);
        const double dist = (d.r - c.r) * (d.r - c.r) + (d.g - c.g) * (d.g - c.g) + (d.b - c.b) * (d.b - c.b);
        if (dist < best_d) {
          best_d = dist;
          best = pen;
        }
      }
      return best;
    }
    c.a = 1;
    for (std::size_t i = 0; i < colors.size(); ++i) {
      if (colors[i] == c) return static_cast<int>(i) + 1;
    }
    if (colors.size() >= 255) {
      // Nearest existing pen.
      int best = 1;
      double best_d = 1e300;
      for (std::size_t i = 0; i < colors.size(); ++i) {
        const Color d = colors[i];
        const double dist = (d.r - c.r) * (d.r - c.r) + (d.g - c.g) * (d.g - c.g) + (d.b - c.b) * (d.b - c.b);
        if (dist < best_d) {
          best_d = dist;
          best = static_cast<int>(i) + 1;
        }
      }
      return best;
    }
    colors.push_back(c);
    return static_cast<int>(colors.size());
  }
};

class HpglWriter {
 public:
  HpglWriter(WriteContext& ctx, bool hpgl2, bool fills, bool labels)
      : ctx_(ctx), hpgl2_(hpgl2), fills_(fills && hpgl2), labels_(labels) {
    pens_.hpgl1 = !hpgl2;
  }

  Result<std::string> write(const Scene& scene) {
    std::string body;
    for (std::size_t i = 0; i < scene.pages.size(); ++i) {
      page_h_ = scene.pages[i].height;
      CVT_TRY(group(scene.pages[i].root, Matrix{}, 0));
      if (i + 1 < scene.pages.size()) out_ += "PU;PG;";
    }
    std::string head = hpgl2_ ? "BP;IN;" : "IN;";
    if (hpgl2_) {
      head += std::format("NP{};", std::max<std::size_t>(pens_.colors.size() + 1, 2));
      for (std::size_t i = 0; i < pens_.colors.size(); ++i) {
        const Color c = pens_.colors[i];
        const auto b = [](float v) { return std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
        head += std::format("PC{},{},{},{};", i + 1, b(c.r), b(c.g), b(c.b));
      }
      head += "WU0;";
    }
    head += "SP1;";
    return head + out_ + (hpgl2_ ? "PU;SP0;PG;" : "PU;SP0;");
  }

 private:
  [[nodiscard]] Matrix to_plu(const Matrix& m) const {
    const double k = 1.0 / px_per_plu;
    return Matrix{k, 0, 0, -k, 0, page_h_ * k} * m;
  }

  void select(int pen, double width_mm, bool dashed) {
    if (pen != pen_) {
      out_ += std::format("SP{};", pen);
      pen_ = pen;
    }
    if (hpgl2_ && std::abs(width_mm - width_) > 1e-6) {
      out_ += std::format("PW{};", number(width_mm, 3));
      width_ = width_mm;
    }
    if (dashed != dashed_) {
      out_ += dashed ? "LT2;" : "LT;";
      dashed_ = dashed;
    }
  }

  Result<void> polylines(const Path& path, double tolerance) {
    for (const Polyline& pl : flatten(path, tolerance)) {
      if (pl.points.empty()) continue;
      points_ += pl.points.size();
      if ((points_ & 0xFFFF) == 0) CVT_TRY(ctx_.checkpoint(0.5));
      const auto pt = [](Point p) { return std::format("{},{}", std::lround(p.x), std::lround(p.y)); };
      out_ += "PU" + pt(pl.points.front()) + ";";
      std::size_t n = 0;
      std::string pd;
      const auto add = [&](Point p) {
        pd += (n % 128 == 0 ? (n == 0 ? std::string("PD") : std::string(";PD")) : std::string(",")) + pt(p);
        ++n;
      };
      for (std::size_t i = 1; i < pl.points.size(); ++i) add(pl.points[i]);
      if (pl.closed && pl.points.size() > 2) add(pl.points.front());
      if (n == 0) add(pl.points.front());
      out_ += pd + ";";
    }
    return {};
  }

  Result<void> fill(const Path& path, FillRule rule, double tolerance) {
    bool first = true;
    for (const Polyline& pl : flatten(path, tolerance)) {
      if (pl.points.size() < 3) continue;
      const auto pt = [](Point p) { return std::format("{},{}", std::lround(p.x), std::lround(p.y)); };
      out_ += "PU" + pt(pl.points.front()) + ";";
      out_ += first ? "PM0;" : "";
      first = false;
      out_ += "PD";
      for (std::size_t i = 1; i < pl.points.size(); ++i) out_ += (i > 1 ? "," : "") + pt(pl.points[i]);
      out_ += ";PM1;";
      points_ += pl.points.size();
    }
    if (!first) out_ += std::format("PM2;FP{};", rule == FillRule::NonZero ? 1 : 0);
    return {};
  }

  Result<void> group(const Group& g, const Matrix& parent, int depth) {
    if (depth > 256) return fail(Status::Encode, "the drawing is nested too deeply");
    if (!g.visible) return {};
    const Matrix m = parent * g.transform;
    const double tolerance = 2.0;  // plotter units (0.05 mm)
    for (const Node& node : g.children) {
      if (const auto* shape = std::get_if<Shape>(&node.value)) {
        const Path path = transformed(shape->path, to_plu(m));
        if (shape->fill.visible() && fills_) {
          select(pens_.pen_for(shape->fill.color), width_ < 0 ? 0.35 : width_, false);
          CVT_TRY(fill(path, shape->fill_rule, tolerance));
        } else if (shape->fill.visible() && !shape->stroke.paint.visible()) {
          if (!hpgl2_) note("HP-GL/1 has no polygon fills; filled shapes were written as outlines.");
          select(pens_.pen_for(shape->fill.color), 0.35, false);
          CVT_TRY(polylines(path, tolerance));
        }
        if (shape->stroke.paint.visible()) {
          const double scale = shape->stroke.non_scaling ? 1.0 : m.mean_scale();
          const double width_mm = shape->stroke.hairline ? 0.1 : shape->stroke.width * scale / px_per_mm;
          select(pens_.pen_for(shape->stroke.paint.color), std::clamp(width_mm, 0.0, 100.0), !shape->stroke.dash.empty());
          CVT_TRY(polylines(path, tolerance));
        }
      } else if (const auto* text = std::get_if<Text>(&node.value)) {
        CVT_TRY(write_text(*text, m, tolerance));
      } else if (const auto* child = std::get_if<Group>(&node.value)) {
        CVT_TRY(group(*child, m, depth + 1));
      } else if (std::get_if<Picture>(&node.value) != nullptr) {
        note("Raster pictures cannot be plotted and were left out.");
      }
    }
    return {};
  }

  Result<void> write_text(const Text& text, const Matrix& m, double tolerance) {
    const Color color = text.fill.visible() ? text.fill.color : text.stroke.paint.color;
    std::string ascii;
    for (const char32_t cp : util::utf8_to_utf32(text.content)) ascii.push_back(cp >= 0x20 && cp < 0x7F ? static_cast<char>(cp) : '?');
    if (!labels_ || util::trim(ascii).empty()) {
      const Path outline = transformed(text.outline, to_plu(m));
      if (fills_) {
        select(pens_.pen_for(color), width_ < 0 ? 0.35 : width_, false);
        return fill(outline, FillRule::NonZero, tolerance / 2);
      }
      select(pens_.pen_for(color), 0.1, false);
      return polylines(outline, tolerance / 2);
    }
    const Matrix t = to_plu(m);
    const Point origin = t.apply(text.origin);
    const Point x = t.apply_vector({1, 0});
    const Point up = t.apply_vector({0, -1});
    const double cap = text.font.size * cap_height_ratio * length(up) * mm_per_plu / 10;  // cm
    const double width = cap * 0.7;
    select(pens_.pen_for(color), width_ < 0 ? 0.35 : width_, false);
    out_ += std::format("SI{},{};DI{},{};PU{},{};LB{}\x03;", number(width, 4), number(cap, 4), number(x.x, 6),
                        number(x.y, 6), std::lround(origin.x), std::lround(origin.y), ascii);
    return {};
  }

  void note(std::string_view message) {
    const std::string m(message);
    if (std::find(notes_.begin(), notes_.end(), m) == notes_.end()) {
      notes_.push_back(m);
      ctx_.warn(m);
    }
  }

  WriteContext& ctx_;
  bool hpgl2_;
  bool fills_;
  bool labels_;
  PenTable pens_;
  std::string out_;
  double page_h_ = 0;
  int pen_ = 1;
  double width_ = -1;
  bool dashed_ = false;
  std::uint64_t points_ = 0;
  std::vector<std::string> notes_;
};

Result<Bytes> write_hpgl(const Scene& scene, WriteContext& ctx) {
  if (scene.pages.empty()) return fail(Status::Encode, "the drawing has no pages");
  const bool hpgl2 = ctx.options.get_enum("dialect", "hpgl2", {"hpgl", "hpgl2"}) == "hpgl2";
  const bool fills = ctx.options.get_bool("fills", true);
  const bool labels = ctx.options.get_enum("text", "label", {"label", "outline"}) == "label";
  HpglWriter writer(ctx, hpgl2, fills, labels);
  CVT_TRY_ASSIGN(std::string text, writer.write(scene));
  CVT_TRY(ctx.limits.check_output(text.size()));
  return to_bytes(text);
}

// --------------------------------------------------------------------------- detection

int sniff_hpgl(std::span<const std::byte> data, std::string_view /*filename*/) {
  std::string_view text = as_chars(data.first(std::min<std::size_t>(data.size(), 2048)));
  std::size_t pos = 0;
  bool wrapped = false;
  // PJL / PCL wrappers.
  while (pos < text.size()) {
    if (text[pos] == '\x1b') {
      wrapped = true;
      pos = skip_escape_sequence(text, pos);
      continue;
    }
    if (text.substr(pos).starts_with("@PJL")) {
      wrapped = true;
      while (pos < text.size() && text[pos] != '\n') ++pos;
      continue;
    }
    if (util::is_ascii_space(text[pos]) || text[pos] == ';') {
      ++pos;
      continue;
    }
    break;
  }
  static constexpr std::array<std::string_view, 40> known = {
      "IN", "DF", "BP", "PS", "IP", "SC", "SP", "PU", "PD", "PA", "PR", "PC", "NP", "PW", "WU", "LT", "LA", "CI", "AA", "AR",
      "AT", "EA", "ER", "RA", "RR", "EW", "WG", "PM", "FP", "EP", "FT", "LB", "DT", "SI", "SR", "DI", "DR", "PE", "RO", "TR"};
  int recognised = 0, first_ok = -1;
  std::size_t p = pos;
  for (int i = 0; i < 16 && p + 1 < text.size(); ++i) {
    while (p < text.size() && (util::is_ascii_space(text[p]) || text[p] == ';')) ++p;
    if (p + 1 >= text.size()) break;
    const std::string cmd{util::ascii_upper(text[p]), util::ascii_upper(text[p + 1])};
    if (!is_letter(text[p]) || !is_letter(text[p + 1])) break;
    const bool ok = std::find(known.begin(), known.end(), cmd) != known.end();
    if (first_ok < 0) first_ok = ok ? 1 : 0;
    if (!ok) break;
    ++recognised;
    p += 2;
    if (cmd == "LB") {
      while (p < text.size() && text[p] != '\x03') ++p;
      ++p;
      continue;
    }
    while (p < text.size() && (util::is_ascii_digit(text[p]) || text[p] == ',' || text[p] == '.' || text[p] == '-' ||
                               text[p] == '+' || text[p] == ' ')) {
      ++p;
    }
    if (p < text.size() && text[p] != ';' && !is_letter(text[p]) && !util::is_ascii_space(text[p])) break;
  }
  if (first_ok != 1) return 0;
  if (recognised >= 3) return wrapped ? 80 : 85;
  if (recognised >= 2) return 50;
  return 0;
}

}  // namespace

void register_hpgl(Registry& registry) {
  VectorFormat format;
  format.info.id = "hpgl";
  format.info.name = "HP-GL plotter file";
  format.info.description = "Hewlett-Packard Graphics Language (HP-GL and HP-GL/2) for pen plotters and cutters.";
  format.info.category = Category::Vector;
  format.info.extensions = {"plt", "hpgl", "hgl", "hp2", "hpg"};
  format.info.mime = "application/vnd.hp-hpgl";
  format.info.popularity = 25;
  format.info.sniff = sniff_hpgl;
  format.read = read_hpgl;
  format.write = write_hpgl;
  format.lossy_read = true;
  format.lossy_write = true;
  format.can_embed_images = false;
  format.write_options = {
      OptionSpec::choice("dialect", "Dialect", "hpgl2", {"hpgl2", "hpgl"},
                         "hpgl2 adds pen colours, widths and polygon fills; hpgl (HP-GL/1) is plain PU/PD for old plotters and cutters."),
      OptionSpec::boolean("fills", "Fills", true, "Fill closed areas with polygon mode (HP-GL/2 only)."),
      OptionSpec::choice("text", "Text", "label", {"label", "outline"},
                         "label writes plotter labels (LB); outline draws the glyph outlines."),
  };
  register_vector_format(registry, std::move(format));
}

}  // namespace cvt::vector::cad
