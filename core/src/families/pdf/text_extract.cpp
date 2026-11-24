// Convertible core - pdf family: text extraction and layout analysis.
//
// Glyphs are collected from the content streams with their positions, then
// grouped into lines, line segments (split at column gutters), tables
// (ruled grids and aligned columns), blocks and paragraphs, ordered with a
// recursive XY cut, and classified as headings, lists, code or body text.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "families/pdf/font_data.hpp"
#include "families/pdf/interp.hpp"
#include "families/pdf/pdf.hpp"
#include "families/pdf/pdf_image.hpp"
#include "families/pdf/syntax.hpp"
#include "util/text.hpp"

namespace cvt::pdf {

namespace {

bool is_lower_start(std::string_view text) {
  if (text.empty()) return false;
  const char c = text[0];
  if (c >= 'a' && c <= 'z') return true;
  // Latin-1 / Latin Extended lowercase letters (UTF-8 lead bytes C3..C5 with a lowercase trail).
  if (static_cast<unsigned char>(c) == 0xC3 && text.size() > 1) return static_cast<unsigned char>(text[1]) >= 0x9F;
  return false;
}

// ------------------------------------------------------------------ collection

struct Glyph {
  std::string text;  // UTF-8
  double x0 = 0, x1 = 0, base = 0, size = 0;
  bool bold = false, italic = false, mono = false;
  bool space = false;
  int angle = 0;  // quantised orientation bucket
  std::size_t order = 0;
  [[nodiscard]] double top() const { return base - size * 0.75; }
  [[nodiscard]] double bottom() const { return base + size * 0.25; }
};

struct Ruling {
  double x0, y0, x1, y1;  // horizontal: y0 == y1; vertical: x0 == x1
  [[nodiscard]] bool horizontal() const { return std::abs(y1 - y0) < std::abs(x1 - x0); }
};

struct PageImage {
  Rect box;
  ImageEvent event;
  Dict inline_dict;
  Object stream;
  std::size_t order = 0;
};

struct LinkArea {
  Rect box;
  std::string url;
};

class TextDevice final : public Device {
 public:
  TextDevice(PdfFile& file, const Matrix& to_page, const Rect& page_box, bool images)
      : file_(file), to_page_(to_page), page_box_(page_box), collect_images_(images) {}

  std::vector<Glyph> glyphs;
  std::vector<Ruling> rulings;
  std::vector<PageImage> images;

  void glyph(const GlyphEvent& ev, const GState& gs) override {
    if (artifact_depth_ > 0) return;
    if (ev.unicode.empty()) return;
    const Matrix m = Matrix::concat(ev.trm, to_page_);
    const double w = ev.font != nullptr ? ev.font->width(ev.code) : 0.5;
    const Point origin = m.apply(Point{0, 0});
    Point dir = m.apply_vector(Point{1, 0});
    const Point up = m.apply_vector(Point{0, 1});
    const double size = std::hypot(up.x, up.y);
    if (!(size > 0.5) || !std::isfinite(size) || size > 2000) return;
    double len = std::hypot(dir.x, dir.y);
    if (!(len > 0)) return;
    const double angle_rad = std::atan2(dir.y, dir.x);
    int angle = static_cast<int>(std::lround(angle_rad * 180.0 / 3.14159265358979323846));
    angle = ((angle % 360) + 360) % 360;
    // Buckets: 0, 90, 180, 270 within 3 degrees; others by 5 degrees.
    int bucket = (angle + 2) / 5 * 5 % 360;
    for (const int b : {0, 90, 180, 270, 360}) {
      if (std::abs(angle - b) <= 3) bucket = b % 360;
    }
    // Coordinates in the rotated reading frame.
    const double r = -bucket * 3.14159265358979323846 / 180.0;
    const auto rot = [&](Point p) { return Point{p.x * std::cos(r) - p.y * std::sin(r), p.x * std::sin(r) + p.y * std::cos(r)}; };
    const Point o = rot(origin);
    const Point e = rot(m.apply(Point{w, 0}));
    if (origin.x < page_box_.x0 - 50 || origin.x > page_box_.x1 + 50 || origin.y < page_box_.y0 - 50 ||
        origin.y > page_box_.y1 + 50) {
      return;  // outside the page
    }
    Glyph g;
    std::u32string text = ev.unicode;
    for (char32_t& c : text) {
      if (c == 0xA0 || c == 0x2002 || c == 0x2003 || c == 0x2009 || c == 0x200A) c = ' ';
      if (c < 0x20 && c != '\t') c = ' ';
    }
    g.text = util::utf32_to_utf8(text);
    g.space = util::trim(g.text).empty();
    if (g.space && !ev.space_code && text.size() == 1 && text[0] != ' ') g.space = true;
    g.x0 = o.x;
    g.x1 = std::max(e.x, o.x + 0.01);
    g.base = o.y;
    g.size = size;
    g.angle = bucket;
    g.bold = ev.font != nullptr && ev.font->bold();
    g.italic = ev.font != nullptr && ev.font->italic();
    g.mono = ev.font != nullptr && ev.font->monospace();
    if (gs.render_mode == 2 && gs.stroke.width > 0.05 * gs.font_size) g.bold = true;  // fill+stroke = fake bold
    g.order = counter_++;
    if (actual_text_depth_ > 0) {
      actual_glyphs_.push_back(std::move(g));
      return;
    }
    glyphs.push_back(std::move(g));
  }

  void fill(const Path& path, raster::FillRule, const GState& gs) override { add_rulings(path, gs, true); }
  void stroke(const Path& path, const GState& gs) override { add_rulings(path, gs, false); }

  void image(const ImageEvent& ev, const GState& gs) override {
    if (!collect_images_ || artifact_depth_ > 0 || images.size() > 500) return;
    const Matrix m = Matrix::concat(gs.ctm, to_page_);
    const Rect box = m.apply(Rect{0, 0, 1, 1});
    if (box.width() < 24 || box.height() < 24) return;  // icons, rules, dots
    PageImage pi;
    pi.box = box;
    pi.stream = ev.stream;
    if (ev.is_inline) {
      pi.inline_dict = *ev.dict;
      pi.event.inline_data = ev.inline_data;
      pi.event.is_inline = true;
    }
    pi.event.resources = nullptr;
    pi.order = counter_++;
    images.push_back(std::move(pi));
  }

  void begin_marked(const std::string& tag, const Dict& props) override {
    marked_.push_back(0);
    if (tag == "Artifact") {
      const std::string type = file_.get(props, "Type").as_name();
      // Pagination artifacts (headers, footers) are skipped; layout artifacts too.
      if (type != "Background") {
        ++artifact_depth_;
        marked_.back() |= 1;
      }
    }
    const Object actual = file_.get(props, "ActualText");
    if (actual.is_string()) {
      ++actual_text_depth_;
      marked_.back() |= 2;
      actual_texts_.push_back(decode_text_string(actual.as_string()));
      actual_start_.push_back(actual_glyphs_.size());
    }
  }
  void end_marked() override {
    if (marked_.empty()) return;
    const int flags = marked_.back();
    marked_.pop_back();
    if (flags & 1) --artifact_depth_;
    if (flags & 2) {
      --actual_text_depth_;
      const std::string text = actual_texts_.back();
      actual_texts_.pop_back();
      const std::size_t start = actual_start_.back();
      actual_start_.pop_back();
      if (start < actual_glyphs_.size()) {
        Glyph g = actual_glyphs_[start];
        for (std::size_t i = start; i < actual_glyphs_.size(); ++i) {
          g.x0 = std::min(g.x0, actual_glyphs_[i].x0);
          g.x1 = std::max(g.x1, actual_glyphs_[i].x1);
        }
        g.text = text;
        g.space = util::trim(text).empty();
        actual_glyphs_.resize(start);
        if (actual_text_depth_ > 0) actual_glyphs_.push_back(std::move(g));
        else if (!text.empty()) glyphs.push_back(std::move(g));
      }
      if (actual_text_depth_ == 0) {
        for (Glyph& g : actual_glyphs_) glyphs.push_back(std::move(g));
        actual_glyphs_.clear();
      }
    }
  }
  [[nodiscard]] bool execute_type3() const override { return false; }

 private:
  void add_rulings(const Path& path, const GState& gs, bool filled) {
    if (rulings.size() > 20000) return;  // borders are often marked as artifacts: keep them
    const Matrix m = Matrix::concat(gs.ctm, to_page_);
    // Walk subpaths made of straight segments.
    std::size_t pi = 0;
    std::vector<Point> pts;
    const auto flush = [&]() {
      if (pts.size() >= 2) {
        if (filled) {
          Rect b = Rect::inverted_infinite();
          for (const Point& p : pts) b.include(p);
          if (b.height() <= 3 && b.width() > 3) rulings.push_back({b.x0, (b.y0 + b.y1) / 2, b.x1, (b.y0 + b.y1) / 2});
          else if (b.width() <= 3 && b.height() > 3) rulings.push_back({(b.x0 + b.x1) / 2, b.y0, (b.x0 + b.x1) / 2, b.y1});
          else if (b.width() > 3 && b.height() > 3 && pts.size() <= 5) {
            // Cell background or box: its edges act as rulings.
            rulings.push_back({b.x0, b.y0, b.x1, b.y0});
            rulings.push_back({b.x0, b.y1, b.x1, b.y1});
            rulings.push_back({b.x0, b.y0, b.x0, b.y1});
            rulings.push_back({b.x1, b.y0, b.x1, b.y1});
          }
        } else {
          for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            const Point a = pts[i], b = pts[i + 1];
            if (std::abs(a.y - b.y) < 1 && std::abs(a.x - b.x) > 2) rulings.push_back({std::min(a.x, b.x), a.y, std::max(a.x, b.x), a.y});
            else if (std::abs(a.x - b.x) < 1 && std::abs(a.y - b.y) > 2) rulings.push_back({a.x, std::min(a.y, b.y), a.x, std::max(a.y, b.y)});
          }
        }
      }
      pts.clear();
    };
    for (const PathOp op : path.ops) {
      switch (op) {
        case PathOp::Move:
          flush();
          pts.push_back(m.apply(path.points[pi++]));
          break;
        case PathOp::Line: pts.push_back(m.apply(path.points[pi++])); break;
        case PathOp::Cubic:
          pi += 3;
          pts.clear();  // curves are not rulings
          break;
        case PathOp::Close:
          if (!pts.empty() && !filled) pts.push_back(pts.front());
          flush();
          break;
      }
    }
    flush();
  }

  PdfFile& file_;
  Matrix to_page_;
  Rect page_box_;
  bool collect_images_;
  std::size_t counter_ = 0;
  int artifact_depth_ = 0;
  int actual_text_depth_ = 0;
  std::vector<int> marked_;
  std::vector<std::string> actual_texts_;
  std::vector<std::size_t> actual_start_;
  std::vector<Glyph> actual_glyphs_;
};

// ------------------------------------------------------------------ layout structures

struct Segment {
  std::vector<Glyph> glyphs;
  double x0 = 0, x1 = 0, base = 0, size = 0, top = 0, bottom = 0;
  int angle = 0;
  void finish() {
    x0 = 1e18;
    x1 = -1e18;
    top = 1e18;
    bottom = -1e18;
    double weighted = 0, total = 0;
    for (const Glyph& g : glyphs) {
      x0 = std::min(x0, g.x0);
      x1 = std::max(x1, g.x1);
      if (!g.space) {
        top = std::min(top, g.top());
        bottom = std::max(bottom, g.bottom());
        weighted += g.size * static_cast<double>(g.text.size());
        total += static_cast<double>(g.text.size());
      }
    }
    size = total > 0 ? weighted / total : (glyphs.empty() ? 10 : glyphs[0].size);
    if (top > bottom) {
      top = base - size * 0.75;
      bottom = base + size * 0.25;
    }
  }
  [[nodiscard]] std::string plain() const;
};

[[nodiscard]] std::string join_glyphs(const std::vector<Glyph>& glyphs) {
  std::string out;
  for (std::size_t i = 0; i < glyphs.size(); ++i) {
    const Glyph& g = glyphs[i];
    if (i > 0 && !g.space && !glyphs[i - 1].space) {
      const Glyph& p = glyphs[i - 1];
      const double gap = g.x0 - p.x1;
      const double threshold = 0.16 * std::min(p.size, g.size);
      if (gap > threshold && !out.empty() && out.back() != ' ') out.push_back(' ');
    }
    if (g.space) {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
    } else {
      out += g.text;
    }
  }
  return out;
}

std::string Segment::plain() const { return std::string(util::trim(join_glyphs(glyphs))); }

struct Block {
  enum class Kind { Text, Table, Image } kind = Kind::Text;
  std::vector<Segment> lines;
  Rect box;
  // tables
  std::vector<std::vector<std::vector<Segment>>> cells;  // row -> col -> segments
  // images
  const PageImage* image = nullptr;
  std::size_t order = 0;
  [[nodiscard]] double size() const {
    double weighted = 0, total = 0;
    for (const Segment& s : lines) {
      weighted += s.size * static_cast<double>(s.glyphs.size());
      total += static_cast<double>(s.glyphs.size());
    }
    return total > 0 ? weighted / total : 0;
  }
};

bool is_bullet(std::string_view text) {
  static constexpr std::string_view kBullets[] = {"•", "◦", "▪", "●", "○", "■",
                                                   "□", "►", "‣", "⁃", "–", "—",
                                                   "-",      "*",      "·", "➢", "✓", "∙",
                                                   "◆", "▸", "→", "", "", "o"};
  for (const std::string_view b : kBullets) {
    if (text == b) return true;
  }
  return false;
}

// "1." "1)" "a." "a)" "(1)" "(a)" "iv." "IV." -> ordinal value or nullopt.
std::optional<int> list_number(std::string_view token) {
  if (token.size() < 2 || token.size() > 6) return std::nullopt;
  std::string_view t = token;
  bool paren_open = false;
  if (t.front() == '(') {
    paren_open = true;
    t.remove_prefix(1);
  }
  if (t.empty()) return std::nullopt;
  const char last = t.back();
  if (last != '.' && last != ')') return std::nullopt;
  if (paren_open && last != ')') return std::nullopt;
  t.remove_suffix(1);
  if (t.empty()) return std::nullopt;
  if (std::all_of(t.begin(), t.end(), [](char c) { return util::is_ascii_digit(c); })) {
    if (t.size() > 3) return std::nullopt;
    return static_cast<int>(util::parse_int(t).value_or(1));
  }
  if (t.size() == 1 && util::is_ascii_alpha(t[0])) return util::ascii_lower(t[0]) - 'a' + 1;
  static constexpr std::string_view kRoman[] = {"i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x", "xi", "xii"};
  const std::string lower = util::to_lower(t);
  for (std::size_t i = 0; i < std::size(kRoman); ++i) {
    if (lower == kRoman[i]) return static_cast<int>(i + 1);
  }
  return std::nullopt;
}

// ------------------------------------------------------------------ page analysis

struct PageLayout {
  std::vector<Block> blocks;  // in reading order
  double width = 0, height = 0;
};

std::vector<Segment> build_segments(std::vector<Glyph> glyphs) {
  std::vector<Segment> segments;
  if (glyphs.empty()) return segments;
  // Remove overprinted duplicates (fake bold, shadows).
  std::sort(glyphs.begin(), glyphs.end(), [](const Glyph& a, const Glyph& b) {
    if (a.angle != b.angle) return a.angle < b.angle;
    if (std::abs(a.base - b.base) > 0.01) return a.base < b.base;
    return a.x0 < b.x0;
  });
  std::vector<Glyph> unique;
  unique.reserve(glyphs.size());
  for (Glyph& g : glyphs) {
    bool dup = false;
    for (std::size_t k = unique.size(); k-- > 0 && unique.size() - k < 6;) {
      const Glyph& u = unique[k];
      if (u.text == g.text && u.angle == g.angle && std::abs(u.x0 - g.x0) < 0.15 * g.size &&
          std::abs(u.base - g.base) < 0.15 * g.size) {
        dup = true;
        break;
      }
    }
    if (!dup) unique.push_back(std::move(g));
  }
  // Lines: cluster by baseline.
  struct Line {
    std::vector<Glyph> glyphs;
    double base = 0, size = 0;
    int angle = 0;
  };
  std::vector<Line> lines;
  std::vector<std::size_t> open;  // indices of recent lines (same angle)
  for (Glyph& g : unique) {
    Line* target = nullptr;
    for (std::size_t k = open.size(); k-- > 0;) {
      Line& line = lines[open[k]];
      if (line.angle != g.angle) continue;
      const double tol = 0.3 * std::max(line.size, g.size);
      if (std::abs(line.base - g.base) <= tol) {
        target = &line;
        break;
      }
      // Superscripts / subscripts riding on the line.
      if (g.size < 0.85 * line.size && g.base < line.base && g.base > line.base - 0.6 * line.size) {
        target = &line;
        break;
      }
      if (g.size < 0.85 * line.size && g.base > line.base && g.base < line.base + 0.35 * line.size) {
        target = &line;
        break;
      }
    }
    if (target == nullptr) {
      lines.push_back(Line{{}, g.base, g.size, g.angle});
      open.push_back(lines.size() - 1);
      if (open.size() > 8) open.erase(open.begin());
      target = &lines.back();
    } else if (g.size > target->size * 1.1 && !g.space) {
      // A bigger glyph defines the line's baseline (the smaller ones were sup/sub).
      target->base = g.base;
      target->size = g.size;
    }
    target->glyphs.push_back(std::move(g));
  }
  // Merge superscript / subscript lines into the line they ride on.
  for (std::size_t i = 0; i < lines.size(); ++i) {
    Line& small = lines[i];
    if (small.glyphs.empty()) continue;
    double sx0 = 1e18, sx1 = -1e18;
    for (const Glyph& g : small.glyphs) {
      sx0 = std::min(sx0, g.x0);
      sx1 = std::max(sx1, g.x1);
    }
    for (std::size_t j = 0; j < lines.size(); ++j) {
      Line& big = lines[j];
      if (j == i || big.glyphs.empty() || big.angle != small.angle || big.size < small.size * 1.15) continue;
      const double d = big.base - small.base;  // > 0: small sits higher
      if (!((d > 0 && d < 0.65 * big.size) || (d < 0 && d > -0.4 * big.size))) continue;
      double bx0 = 1e18, bx1 = -1e18;
      for (const Glyph& g : big.glyphs) {
        bx0 = std::min(bx0, g.x0);
        bx1 = std::max(bx1, g.x1);
      }
      if (sx0 > bx1 + 0.5 * big.size || sx1 < bx0 - 0.5 * big.size) continue;
      for (Glyph& g : small.glyphs) big.glyphs.push_back(std::move(g));
      small.glyphs.clear();
      break;
    }
  }
  // Candidate gaps per line: clearly wider than the word spacing.
  struct Gap {
    std::size_t line;
    double x0, x1;
    bool strong;
  };
  std::vector<Gap> candidates;
  std::vector<double> word_gaps(lines.size(), 0);
  for (std::size_t li = 0; li < lines.size(); ++li) {
    Line& line = lines[li];
    if (line.glyphs.empty()) continue;
    std::sort(line.glyphs.begin(), line.glyphs.end(), [](const Glyph& a, const Glyph& b) { return a.x0 < b.x0; });
    std::vector<double> gaps;
    double reach = -1e18;
    for (const Glyph& g : line.glyphs) {
      if (g.space) continue;
      if (reach > -1e17) {
        const double gap = g.x0 - reach;
        if (gap > 0.1 * line.size) gaps.push_back(gap);
      }
      reach = std::max(reach, g.x1);
    }
    double word_gap = 0.25 * line.size;
    if (gaps.size() >= 3) {
      std::vector<double> sorted = gaps;
      std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
      word_gap = std::clamp(sorted[sorted.size() / 2], 0.15 * line.size, 0.6 * line.size);
    }
    word_gaps[li] = word_gap;
    reach = -1e18;
    for (const Glyph& g : line.glyphs) {
      if (g.space) continue;
      if (reach > -1e17) {
        const double gap = g.x0 - reach;
        const bool strong = gap > std::max(1.0 * line.size, 3.0 * word_gap);
        if (strong || (gap > 0.45 * line.size && gap > 1.6 * word_gap)) candidates.push_back(Gap{li, reach, g.x0, strong});
      }
      reach = std::max(reach, g.x1);
    }
  }
  // A weak gap becomes a split when other nearby lines have a gap at the same place
  // (column gutters, table columns).
  std::vector<std::vector<std::pair<double, double>>> splits(lines.size());
  for (const Gap& c : candidates) {
    bool split = c.strong;
    if (!split) {
      int support = 0;
      const Line& line = lines[c.line];
      for (const Gap& o : candidates) {
        if (o.line == c.line) continue;
        const Line& other = lines[o.line];
        if (other.angle != line.angle || std::abs(other.base - line.base) > 10 * line.size) continue;
        const double overlap = std::min(c.x1, o.x1) - std::max(c.x0, o.x0);
        if (overlap > 0.3 * std::min(c.x1 - c.x0, o.x1 - o.x0)) ++support;
        if (support >= 2) break;
      }
      split = support >= 2;
    }
    if (split) splits[c.line].emplace_back(c.x0, c.x1);
  }
  // Segments: split lines at the chosen gaps.
  for (std::size_t li = 0; li < lines.size(); ++li) {
    Line& line = lines[li];
    if (line.glyphs.empty()) continue;
    Segment current;
    current.angle = line.angle;
    current.base = line.base;
    double last_x1 = -1e18;
    const auto flush = [&]() {
      while (!current.glyphs.empty() && current.glyphs.back().space) current.glyphs.pop_back();
      while (!current.glyphs.empty() && current.glyphs.front().space) current.glyphs.erase(current.glyphs.begin());
      if (!current.glyphs.empty()) {
        current.finish();
        segments.push_back(std::move(current));
      }
      current = Segment{};
      current.angle = line.angle;
      current.base = line.base;
    };
    for (Glyph& g : line.glyphs) {
      if (!current.glyphs.empty() && !g.space) {
        bool cut = false;
        for (const auto& [a, b] : splits[li]) {
          if (last_x1 <= a + 0.01 && g.x0 >= b - 0.01) cut = true;
        }
        if (cut) {
          // Keep a bullet or number attached to its text.
          const std::string head = current.plain();
          bool marker = current.glyphs.size() <= 4 && (is_bullet(head) || list_number(head)) &&
                        g.x0 - last_x1 < 4 * line.size;
          // Section numbers ("1", "2.3") stay with a following title at the start of a line.
          const bool first_in_line = segments.empty() || segments.back().base != line.base ||
                                     segments.back().angle != line.angle;
          if (!marker && first_in_line && head.size() <= 8 && !head.empty() && util::is_ascii_digit(head[0]) &&
              std::all_of(head.begin(), head.end(), [](char c) { return util::is_ascii_digit(c) || c == '.'; }) &&
              util::is_ascii_alpha(g.text.empty() ? ' ' : g.text[0]) && g.x0 - last_x1 < 3 * line.size) {
            marker = true;
          }
          if (!marker) flush();
        }
      }
      if (!g.space) last_x1 = std::max(last_x1, g.x1);
      current.glyphs.push_back(std::move(g));
    }
    flush();
  }
  return segments;
}

// Ruled tables: grids formed by horizontal and vertical rulings.
void detect_ruled_tables(std::vector<Ruling> rulings, std::vector<Segment>& segments, std::vector<Block>& out) {
  if (rulings.size() < 4) return;
  std::vector<Ruling> hs, vs;
  for (const Ruling& r : rulings) (r.horizontal() ? hs : vs).push_back(r);
  if (hs.size() < 2 || vs.size() < 2) return;
  // Connected groups of rulings (by bounding box intersection).
  const std::size_t n = rulings.size();
  std::vector<int> parent(n);
  for (std::size_t i = 0; i < n; ++i) parent[i] = static_cast<int>(i);
  std::function<int(int)> find = [&](int x) { return parent[static_cast<std::size_t>(x)] == x ? x : parent[static_cast<std::size_t>(x)] = find(parent[static_cast<std::size_t>(x)]); };
  const auto touches = [](const Ruling& a, const Ruling& b) {
    const double tol = 2.0;
    return std::min(a.x0, a.x1) - tol <= std::max(b.x0, b.x1) && std::min(b.x0, b.x1) - tol <= std::max(a.x0, a.x1) &&
           std::min(a.y0, a.y1) - tol <= std::max(b.y0, b.y1) && std::min(b.y0, b.y1) - tol <= std::max(a.y0, a.y1);
  };
  if (n > 3000) return;
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      if (touches(rulings[i], rulings[j])) parent[static_cast<std::size_t>(find(static_cast<int>(i)))] = find(static_cast<int>(j));
    }
  }
  std::map<int, std::vector<Ruling>> groups;
  for (std::size_t i = 0; i < n; ++i) groups[find(static_cast<int>(i))].push_back(rulings[i]);
  for (auto& [root, group] : groups) {
    std::vector<double> xs, ys;
    Rect box = Rect::inverted_infinite();
    for (const Ruling& r : group) {
      box.include({r.x0, r.y0});
      box.include({r.x1, r.y1});
      if (r.horizontal()) ys.push_back(r.y0);
      else xs.push_back(r.x0);
    }
    const auto cluster = [](std::vector<double>& v) {
      std::sort(v.begin(), v.end());
      std::vector<double> out;
      for (const double x : v) {
        if (out.empty() || x - out.back() > 3) out.push_back(x);
      }
      v = out;
    };
    cluster(xs);
    cluster(ys);
    if (xs.size() < 2 || ys.size() < 2 || box.width() < 30 || box.height() < 10) continue;
    if (xs.size() == 2 && ys.size() == 2) {
      // A single box: a table only if it contains several separated text columns; treat as a frame otherwise.
      continue;
    }
    const std::size_t rows = ys.size() - 1, cols = xs.size() - 1;
    if (rows > 500 || cols > 64) continue;
    Block table;
    table.kind = Block::Kind::Table;
    table.box = box;
    table.cells.assign(rows, std::vector<std::vector<Segment>>(cols));
    std::size_t used = 0;
    for (std::size_t s = 0; s < segments.size();) {
      Segment& seg = segments[s];
      const double cy = (seg.top + seg.bottom) / 2;
      if (seg.angle != 0 || seg.x0 < box.x0 - 2 || seg.x1 > box.x1 + 2 || cy < box.y0 || cy > box.y1) {
        ++s;
        continue;
      }
      // Split the segment across column boundaries (cells can sit on one line).
      const auto row_it = std::upper_bound(ys.begin(), ys.end(), cy);
      const std::size_t row = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(row_it - ys.begin() - 1, 0, static_cast<std::ptrdiff_t>(rows) - 1));
      std::map<std::size_t, Segment> parts;
      for (Glyph& g : seg.glyphs) {
        const double cx = (g.x0 + g.x1) / 2;
        const auto col_it = std::upper_bound(xs.begin(), xs.end(), cx);
        const std::size_t col = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(col_it - xs.begin() - 1, 0, static_cast<std::ptrdiff_t>(cols) - 1));
        Segment& part = parts[col];
        part.angle = seg.angle;
        part.base = seg.base;
        part.glyphs.push_back(std::move(g));
      }
      for (auto& [col, part] : parts) {
        part.finish();
        table.cells[row][col].push_back(std::move(part));
      }
      segments.erase(segments.begin() + static_cast<std::ptrdiff_t>(s));
      ++used;
    }
    if (used == 0) continue;
    // Drop empty rows (between double rules) and empty columns.
    std::vector<std::vector<std::vector<Segment>>> kept;
    for (auto& row : table.cells) {
      bool any = false;
      for (auto& cell : row) any = any || !cell.empty();
      if (any) kept.push_back(std::move(row));
    }
    if (kept.empty()) continue;
    // Rows without rules between them: split a grid row into one row per
    // text line when the lines line up across columns.
    {
      std::vector<std::vector<std::vector<Segment>>> split_rows;
      for (auto& row : kept) {
        std::vector<double> bases;
        std::size_t filled = 0;
        for (const auto& cell : row) {
          filled += cell.empty() ? 0 : 1;
          for (const Segment& s : cell) bases.push_back(s.base);
        }
        std::sort(bases.begin(), bases.end());
        std::vector<double> lines_at;
        for (const double b : bases) {
          if (lines_at.empty() || b - lines_at.back() > 2.0) lines_at.push_back(b);
        }
        bool aligned = lines_at.size() > 1 && filled >= 2;
        if (aligned) {
          for (const double base : lines_at) {
            std::size_t cols_on_line = 0;
            for (const auto& cell : row) {
              cols_on_line += std::any_of(cell.begin(), cell.end(), [&](const Segment& s) { return std::abs(s.base - base) <= 2.0; }) ? 1 : 0;
            }
            if (cols_on_line * 2 < filled || cols_on_line < 2) aligned = false;
          }
        }
        if (!aligned) {
          split_rows.push_back(std::move(row));
          continue;
        }
        for (const double base : lines_at) {
          std::vector<std::vector<Segment>> line_row(row.size());
          for (std::size_t c = 0; c < row.size(); ++c) {
            for (const Segment& s : row[c]) {
              if (std::abs(s.base - base) <= 2.0) line_row[c].push_back(s);
            }
          }
          split_rows.push_back(std::move(line_row));
        }
      }
      kept = std::move(split_rows);
    }
    std::vector<bool> col_used(cols, false);
    for (const auto& row : kept) {
      for (std::size_t c = 0; c < cols; ++c) col_used[c] = col_used[c] || !row[c].empty();
    }
    for (auto& row : kept) {
      std::vector<std::vector<Segment>> cells;
      for (std::size_t c = 0; c < cols; ++c) {
        if (col_used[c]) cells.push_back(std::move(row[c]));
      }
      row = std::move(cells);
    }
    table.cells = std::move(kept);
    if (table.cells.size() == 1 && table.cells[0].size() == 1) {
      // A framed single cell is just text.
      for (Segment& s : table.cells[0][0]) segments.push_back(std::move(s));
      continue;
    }
    out.push_back(std::move(table));
  }
}

// Unruled tables: runs of lines with several aligned short segments.
void detect_aligned_tables(std::vector<Segment>& segments, std::vector<Block>& out) {
  std::vector<std::size_t> idx;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (segments[i].angle == 0) idx.push_back(i);
  }
  std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
    return segments[a].base < segments[b].base || (segments[a].base == segments[b].base && segments[a].x0 < segments[b].x0);
  });
  // Group into rows by baseline.
  struct Row {
    std::vector<std::size_t> segs;
    double base, size;
  };
  std::vector<Row> rows;
  for (const std::size_t i : idx) {
    const Segment& s = segments[i];
    if (!rows.empty() && std::abs(rows.back().base - s.base) < 0.4 * s.size) {
      rows.back().segs.push_back(i);
    } else {
      rows.push_back(Row{{i}, s.base, s.size});
    }
  }
  std::set<std::size_t> consumed;
  std::size_t r = 0;
  while (r < rows.size()) {
    if (rows[r].segs.size() < 2) {
      ++r;
      continue;
    }
    std::size_t end = r + 1;
    while (end < rows.size() && rows[end].segs.size() >= 2 &&
           rows[end].base - rows[end - 1].base < 2.6 * std::max(rows[end].size, rows[end - 1].size)) {
      ++end;
    }
    const std::size_t count = end - r;
    if (count >= 3) {
      // Column anchors from segment left edges.
      std::vector<double> lefts;
      double min_x = 1e18, max_x = -1e18, total_width = 0;
      std::size_t n_segs = 0;
      for (std::size_t k = r; k < end; ++k) {
        for (const std::size_t i : rows[k].segs) {
          lefts.push_back(segments[i].x0);
          min_x = std::min(min_x, segments[i].x0);
          max_x = std::max(max_x, segments[i].x1);
          total_width += segments[i].x1 - segments[i].x0;
          ++n_segs;
        }
      }
      std::sort(lefts.begin(), lefts.end());
      std::vector<double> anchors;
      std::vector<int> support;
      for (const double x : lefts) {
        if (!anchors.empty() && x - anchors.back() < 8) {
          ++support.back();
        } else {
          anchors.push_back(x);
          support.push_back(1);
        }
      }
      std::vector<double> columns;
      for (std::size_t a = 0; a < anchors.size(); ++a) {
        if (support[a] * 2 >= static_cast<int>(count)) columns.push_back(anchors[a]);
      }
      const double avg_width = n_segs > 0 ? total_width / static_cast<double>(n_segs) : 0;
      const double span = max_x - min_x;
      if (columns.size() >= 2 && span > 0 && avg_width < 0.42 * span) {
        Block table;
        table.kind = Block::Kind::Table;
        table.box = Rect{min_x, rows[r].base - rows[r].size, max_x, rows[end - 1].base + rows[end - 1].size * 0.3};
        // A header line right above the run whose text fits inside the table span
        // (centred header cells often fail the left-edge alignment).
        if (r > 0) {
          const Row& above = rows[r - 1];
          bool fits = rows[r].base - above.base < 2.6 * std::max(above.size, rows[r].size);
          for (const std::size_t i : above.segs) {
            fits = fits && !consumed.contains(i) && segments[i].x0 >= min_x - above.size && segments[i].x1 <= max_x + above.size;
          }
          if (fits) {
            std::vector<std::vector<Segment>> cells(columns.size());
            for (const std::size_t i : above.segs) {
              std::map<std::size_t, Segment> parts;
              // Assign whole words by their centre.
              const auto& glyphs = segments[i].glyphs;
              std::size_t w = 0;
              while (w < glyphs.size()) {
                std::size_t e = w + 1;
                while (e < glyphs.size() && !glyphs[e].space && glyphs[e].x0 - glyphs[e - 1].x1 < 0.16 * glyphs[e].size) ++e;
                double wx0 = 1e18, wx1 = -1e18;
                for (std::size_t k = w; k < e; ++k) {
                  wx0 = std::min(wx0, glyphs[k].x0);
                  wx1 = std::max(wx1, glyphs[k].x1);
                }
                const double cx = (wx0 + wx1) / 2;
                std::size_t col = 0;
                for (std::size_t c = 0; c < columns.size(); ++c) {
                  if (cx >= columns[c] - 8) col = c;
                }
                Segment& part = parts[col];
                part.angle = segments[i].angle;
                part.base = segments[i].base;
                for (std::size_t k = w; k < e; ++k) part.glyphs.push_back(glyphs[k]);
                w = e;
              }
              for (auto& [col, part] : parts) {
                while (!part.glyphs.empty() && part.glyphs.back().space) part.glyphs.pop_back();
                while (!part.glyphs.empty() && part.glyphs.front().space) part.glyphs.erase(part.glyphs.begin());
                if (part.glyphs.empty()) continue;
                part.finish();
                cells[col].push_back(std::move(part));
              }
              consumed.insert(i);
            }
            table.cells.push_back(std::move(cells));
            table.box.y0 = above.base - above.size;
          }
        }
        for (std::size_t k = r; k < end; ++k) {
          std::vector<std::vector<Segment>> cells(columns.size());
          for (const std::size_t i : rows[k].segs) {
            std::size_t col = 0;
            for (std::size_t c = 0; c < columns.size(); ++c) {
              if (segments[i].x0 >= columns[c] - 8) col = c;
            }
            cells[col].push_back(segments[i]);
            consumed.insert(i);
          }
          table.cells.push_back(std::move(cells));
        }
        out.push_back(std::move(table));
        r = end;
        continue;
      }
    }
    r = end;
  }
  if (consumed.empty()) return;
  std::vector<Segment> rest;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (!consumed.contains(i)) rest.push_back(std::move(segments[i]));
  }
  segments = std::move(rest);
}

void build_text_blocks(std::vector<Segment>& segments, std::vector<Block>& out) {
  std::sort(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) {
    if (a.angle != b.angle) return a.angle < b.angle;
    if (std::abs(a.base - b.base) > 0.5) return a.base < b.base;
    return a.x0 < b.x0;
  });
  std::vector<Block> blocks;
  for (Segment& seg : segments) {
    Block* best = nullptr;
    for (std::size_t k = blocks.size(); k-- > 0 && blocks.size() - k < 24;) {
      Block& b = blocks[k];
      const Segment& last = b.lines.back();
      if (last.angle != seg.angle) continue;
      const double dy = seg.base - last.base;
      const double size = std::max(seg.size, last.size);
      if (dy < 0.4 * size || dy > 1.75 * size) continue;
      if (std::max(seg.size, last.size) > 1.08 * std::min(seg.size, last.size)) continue;
      // A short bold line followed by regular text is a heading, not part of the paragraph.
      const auto bold_share = [](const Segment& s) {
        std::size_t bold = 0, all = 0;
        for (const Glyph& g : s.glyphs) {
          if (g.space) continue;
          ++all;
          bold += g.bold ? 1 : 0;
        }
        return all > 0 ? static_cast<double>(bold) / static_cast<double>(all) : 0.0;
      };
      if (b.lines.size() == 1 && bold_share(last) > 0.9 && bold_share(seg) < 0.5 &&
          (last.x1 - last.x0) < 0.75 * (seg.x1 - seg.x0)) {
        continue;
      }
      // Horizontal relation: overlap or same left edge.
      const double overlap = std::min(seg.x1, b.box.x1) - std::max(seg.x0, b.box.x0);
      const double narrower = std::min(seg.x1 - seg.x0, b.box.width());
      if (overlap < 0.3 * narrower && std::abs(seg.x0 - b.box.x0) > 2 * size) continue;
      // Regular spacing within the block.
      if (b.lines.size() >= 2) {
        const double prev_dy = last.base - b.lines[b.lines.size() - 2].base;
        if (dy > prev_dy * 1.4 + 0.5) continue;
      }
      best = &b;
      break;
    }
    if (best == nullptr) {
      Block b;
      b.box = Rect{seg.x0, seg.top, seg.x1, seg.bottom};
      b.order = seg.glyphs.empty() ? 0 : seg.glyphs.front().order;
      b.lines.push_back(std::move(seg));
      blocks.push_back(std::move(b));
    } else {
      best->box = best->box.unite(Rect{seg.x0, seg.top, seg.x1, seg.bottom});
      best->lines.push_back(std::move(seg));
    }
  }
  for (Block& b : blocks) out.push_back(std::move(b));
}

// Recursive XY cut for reading order.
void xy_cut(std::vector<Block*> items, std::vector<Block*>& out, int depth) {
  if (items.size() <= 1 || depth > 40) {
    std::sort(items.begin(), items.end(), [](const Block* a, const Block* b) {
      return a->box.y0 < b->box.y0 || (a->box.y0 == b->box.y0 && a->box.x0 < b->box.x0);
    });
    out.insert(out.end(), items.begin(), items.end());
    return;
  }
  const auto best_gap = [&](bool vertical_axis) {
    // Project on y (horizontal cut) or x (vertical cut).
    std::vector<std::pair<double, double>> spans;
    for (const Block* b : items) spans.emplace_back(vertical_axis ? b->box.x0 : b->box.y0, vertical_axis ? b->box.x1 : b->box.y1);
    std::sort(spans.begin(), spans.end());
    double reach = spans[0].second, gap = 0, at = 0;
    for (std::size_t i = 1; i < spans.size(); ++i) {
      if (spans[i].first - reach > gap) {
        gap = spans[i].first - reach;
        at = (spans[i].first + reach) / 2;
      }
      reach = std::max(reach, spans[i].second);
    }
    return std::pair<double, double>{gap, at};
  };
  const auto [hgap, hat] = best_gap(false);
  const auto [vgap, vat] = best_gap(true);
  double median_size = 10;
  {
    std::vector<double> sizes;
    for (const Block* b : items) {
      if (b->kind == Block::Kind::Text) sizes.push_back(b->size());
    }
    if (!sizes.empty()) {
      std::nth_element(sizes.begin(), sizes.begin() + static_cast<std::ptrdiff_t>(sizes.size() / 2), sizes.end());
      median_size = sizes[sizes.size() / 2];
    }
  }
  bool cut_vertical = false;
  if (vgap > 0.8 * median_size && hgap < 2.5 * median_size) cut_vertical = true;
  if (hgap <= 0 && vgap > 0) cut_vertical = true;
  if (hgap <= 0 && vgap <= 0) {
    std::sort(items.begin(), items.end(), [](const Block* a, const Block* b) {
      return a->box.y0 < b->box.y0 || (a->box.y0 == b->box.y0 && a->box.x0 < b->box.x0);
    });
    out.insert(out.end(), items.begin(), items.end());
    return;
  }
  std::vector<Block*> first, second;
  for (Block* b : items) {
    const double mid = cut_vertical ? (b->box.x0 + b->box.x1) / 2 : (b->box.y0 + b->box.y1) / 2;
    (mid < (cut_vertical ? vat : hat) ? first : second).push_back(b);
  }
  if (first.empty() || second.empty()) {
    std::sort(items.begin(), items.end(), [](const Block* a, const Block* b) { return a->box.y0 < b->box.y0; });
    out.insert(out.end(), items.begin(), items.end());
    return;
  }
  xy_cut(std::move(first), out, depth + 1);
  xy_cut(std::move(second), out, depth + 1);
}

PageLayout analyse_page(TextDevice& dev, double width, double height) {
  PageLayout layout;
  layout.width = width;
  layout.height = height;
  std::vector<Segment> segments = build_segments(std::move(dev.glyphs));
  std::vector<Block> blocks;
  detect_ruled_tables(dev.rulings, segments, blocks);
  detect_aligned_tables(segments, blocks);
  build_text_blocks(segments, blocks);
  for (const PageImage& img : dev.images) {
    Block b;
    b.kind = Block::Kind::Image;
    b.box = img.box;
    b.image = &img;
    b.order = img.order;
    blocks.push_back(std::move(b));
  }
  std::vector<Block*> pointers;
  for (Block& b : blocks) pointers.push_back(&b);
  std::vector<Block*> ordered;
  xy_cut(pointers, ordered, 0);
  for (Block* b : ordered) layout.blocks.push_back(std::move(*b));
  return layout;
}

// ------------------------------------------------------------------ inline building

struct LinkTarget {
  Rect box;
  std::string url;
};

class InlineBuilder {
 public:
  explicit InlineBuilder(const std::vector<LinkTarget>& links) : links_(links) {}

  void add_text(std::string_view text, bool bold, bool italic, bool sup, const std::string& link) {
    if (text.empty()) return;
    if (!runs_.empty()) {
      Run& last = runs_.back();
      if (last.bold == bold && last.italic == italic && last.sup == sup && last.link == link) {
        last.text += text;
        return;
      }
    }
    runs_.push_back(Run{std::string(text), bold, italic, sup, link});
  }
  // Append glyphs of one line; `join` is placed before (space or nothing).
  void add_line(const Segment& seg, double line_size, std::string_view join) {
    if (!join.empty() && !runs_.empty()) {
      const Run& last = runs_.back();
      add_text(join, last.bold, last.italic, false, last.link);
    }
    const std::vector<Glyph>& glyphs = seg.glyphs;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      const Glyph& g = glyphs[i];
      std::string piece;
      if (i > 0 && !g.space && !glyphs[i - 1].space) {
        const double gap = g.x0 - glyphs[i - 1].x1;
        if (gap > 0.16 * std::min(g.size, glyphs[i - 1].size)) piece = " ";
      }
      const bool sup = g.size < 0.8 * line_size && g.base < seg.base - 0.2 * line_size;
      const std::string link = link_at(g);
      if (!piece.empty()) {
        const Run* last = runs_.empty() ? nullptr : &runs_.back();
        add_text(piece, last ? last->bold : g.bold, last ? last->italic : g.italic, false, last ? last->link : link);
      }
      if (g.space) {
        const Run* last = runs_.empty() ? nullptr : &runs_.back();
        if (last != nullptr && !last->text.empty() && last->text.back() != ' ') {
          add_text(" ", last->bold, last->italic, false, last->link);
        }
        continue;
      }
      add_text(g.text, g.bold, g.italic, sup, link);
    }
  }
  [[nodiscard]] bool ends_with_hyphen() const {
    if (runs_.empty()) return false;
    const std::string& t = runs_.back().text;
    if (t.size() < 2 || t.back() != '-') return false;
    const char before = t[t.size() - 2];
    return util::is_ascii_alpha(before);
  }
  void drop_trailing_hyphen() {
    if (!runs_.empty() && !runs_.back().text.empty() && runs_.back().text.back() == '-') runs_.back().text.pop_back();
  }
  [[nodiscard]] ir::Inlines build() {
    ir::Inlines out;
    // Merge consecutive runs into link groups.
    std::size_t i = 0;
    while (i < runs_.size()) {
      if (!runs_[i].link.empty()) {
        ir::Link link;
        link.url = runs_[i].link;
        const std::string url = runs_[i].link;
        while (i < runs_.size() && runs_[i].link == url) {
          link.children.push_back(styled(runs_[i]));
          ++i;
        }
        out.push_back(std::move(link));
      } else {
        out.push_back(styled(runs_[i]));
        ++i;
      }
    }
    // Trim surrounding whitespace.
    if (!out.empty()) {
      if (auto* t = out.front().get_if<ir::Text>()) {
        while (!t->text.empty() && t->text.front() == ' ') t->text.erase(t->text.begin());
      }
      if (auto* t = out.back().get_if<ir::Text>()) {
        while (!t->text.empty() && t->text.back() == ' ') t->text.pop_back();
      }
    }
    return out;
  }
  [[nodiscard]] bool empty() const { return runs_.empty(); }
  [[nodiscard]] std::string plain() const {
    std::string out;
    for (const Run& r : runs_) out += r.text;
    return out;
  }

 private:
  struct Run {
    std::string text;
    bool bold, italic, sup;
    std::string link;
  };
  [[nodiscard]] static ir::Inline styled(const Run& r) {
    ir::Inline node = ir::Text{r.text};
    if (r.sup) node = ir::Superscript{{node}};
    if (r.italic) node = ir::Emphasis{{node}};
    if (r.bold) node = ir::Strong{{node}};
    return node;
  }
  [[nodiscard]] std::string link_at(const Glyph& g) const {
    const double cx = (g.x0 + g.x1) / 2, cy = g.base - g.size * 0.3;
    for (const LinkTarget& l : links_) {
      if (cx >= l.box.x0 && cx <= l.box.x1 && cy >= l.box.y0 && cy <= l.box.y1) return l.url;
    }
    return {};
  }
  const std::vector<LinkTarget>& links_;
  std::vector<Run> runs_;
};

std::string normalized_key(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (util::is_ascii_alpha(c) || util::is_ascii_digit(c)) out.push_back(util::ascii_lower(c));
    else if (static_cast<unsigned char>(c) >= 0x80) out.push_back(c);
  }
  return out;
}

// ------------------------------------------------------------------ document assembly

struct Analysed {
  std::vector<PageLayout> pages;
  std::vector<std::vector<LinkTarget>> links;
};

struct BlockInfo {
  std::string text;
  double size = 0;
  bool all_bold = false;
  bool all_mono = false;
  std::size_t chars = 0;
};

BlockInfo block_info(const Block& b) {
  BlockInfo info;
  std::size_t bold = 0, mono = 0;
  for (const Segment& s : b.lines) {
    if (!info.text.empty()) info.text.push_back(' ');
    info.text += s.plain();
    for (const Glyph& g : s.glyphs) {
      if (g.space) continue;
      ++info.chars;
      bold += g.bold ? 1 : 0;
      mono += g.mono ? 1 : 0;
    }
  }
  info.size = b.size();
  info.all_bold = info.chars > 0 && bold * 10 >= info.chars * 9;
  info.all_mono = info.chars > 0 && mono * 10 >= info.chars * 9;
  return info;
}

}  // namespace

Result<ir::Document> extract_document(PdfFile& file, const ExtractOptions& options, const Context& ctx) {
  ir::Document doc;
  // Metadata.
  const Dict& info = file.info().as_dict();
  doc.metadata.title = util::sanitize_utf8(decode_text_string(file.get(info, "Title").as_string()));
  doc.metadata.author = util::sanitize_utf8(decode_text_string(file.get(info, "Author").as_string()));
  const std::string subject = decode_text_string(file.get(info, "Subject").as_string());
  if (!subject.empty()) doc.metadata.extra["subject"] = util::sanitize_utf8(subject);
  const std::string keywords = decode_text_string(file.get(info, "Keywords").as_string());
  if (!keywords.empty()) doc.metadata.extra["keywords"] = util::sanitize_utf8(keywords);
  doc.metadata.date = pdf_date_to_iso(file.get(info, "CreationDate").as_string());
  const Object lang = file.get(file.catalog().as_dict(), "Lang");
  if (lang.is_string()) doc.metadata.language = util::sanitize_utf8(decode_text_string(lang.as_string()));

  // Outline titles for heading levels.
  std::map<std::string, int> outline_levels;
  {
    std::set<std::uint32_t> seen;
    std::function<void(const Object&, int)> walk = [&](const Object& first, int depth) {
      Object node_ref = first;
      for (int guard = 0; guard < 10000 && depth < 6; ++guard) {
        if (!node_ref.is_ref() || seen.contains(node_ref.as_ref().num)) break;
        seen.insert(node_ref.as_ref().num);
        const Object node = file.resolve(node_ref);
        const std::string title = normalized_key(decode_text_string(file.get(node.as_dict(), "Title").as_string()));
        if (!title.empty() && !outline_levels.contains(title)) outline_levels[title] = depth + 1;
        walk(node.as_dict().get("First"), depth + 1);
        node_ref = node.as_dict().get("Next");
      }
    };
    const Dict& outlines = file.get(file.catalog().as_dict(), "Outlines").as_dict();
    walk(outlines.get("First"), 0);
  }

  FontCache fonts;
  Analysed analysed;
  std::vector<std::unique_ptr<TextDevice>> devices;
  const auto& pages = file.pages();
  std::size_t done = 0;
  for (const int index : options.pages) {
    CVT_TRY(ctx.checkpoint(0.8 * static_cast<double>(done++) / static_cast<double>(options.pages.size()), "Reading text"));
    const Page& page = pages[static_cast<std::size_t>(index)];
    const PageTransform t = page_transform(page, 1.0);
    auto dev = std::make_unique<TextDevice>(file, t.matrix, Rect{0, 0, t.width, t.height}, options.images);
    Interpreter interp(file, *dev, fonts, &ctx);
    interp.limits.max_operators = 20'000'000;
    interp.run_page(page, Matrix{});
    if (ctx.progress.cancelled()) return fail(Status::Cancelled, "the conversion was cancelled");
    // Link annotations.
    std::vector<LinkTarget> links;
    for (const Object& annot_ref : file.get(page.dict, "Annots").as_array()) {
      const Object annot = file.resolve(annot_ref);
      const Dict& a = annot.as_dict();
      if (!file.get(a, "Subtype").is_name("Link")) continue;
      const Dict& action = file.get(a, "A").as_dict();
      std::string uri;
      if (file.get(action, "S").is_name("URI")) uri = file.get(action, "URI").as_string();
      if (uri.empty()) continue;
      const Array& rect = file.get(a, "Rect").as_array();
      if (rect.size() != 4) continue;
      const Rect r = t.matrix.apply(Rect{file.resolve(rect[0]).as_number(), file.resolve(rect[1]).as_number(),
                                         file.resolve(rect[2]).as_number(), file.resolve(rect[3]).as_number()});
      links.push_back(LinkTarget{r, util::sanitize_utf8(uri)});
      if (links.size() > 2000) break;
    }
    analysed.pages.push_back(analyse_page(*dev, t.width, t.height));
    analysed.links.push_back(std::move(links));
    devices.push_back(std::move(dev));
  }

  // Body size: most common glyph size weighted by characters.
  std::map<int, std::size_t> size_hist;
  for (const PageLayout& p : analysed.pages) {
    for (const Block& b : p.blocks) {
      for (const Segment& s : b.lines) {
        for (const Glyph& g : s.glyphs) {
          if (!g.space) size_hist[static_cast<int>(std::lround(g.size * 2))] += 1;
        }
      }
    }
  }
  double body = 10;
  std::size_t best_count = 0;
  for (const auto& [half_points, count] : size_hist) {
    if (count > best_count) {
      best_count = count;
      body = half_points / 2.0;
    }
  }

  // Repeated headers/footers and page numbers.
  std::map<std::string, int> edge_counts;
  const auto edge_key = [](const PageLayout& p, const Block& b) -> std::string {
    if (b.kind != Block::Kind::Text || b.lines.size() > 2) return {};
    const bool top = b.box.y1 < p.height * 0.1;
    const bool bottom = b.box.y0 > p.height * 0.9;
    if (!top && !bottom) return {};
    std::string text;
    for (const Segment& s : b.lines) text += s.plain();
    std::string key;
    for (const char c : text) {
      if (!util::is_ascii_digit(c)) key.push_back(c);  // page numbers vary
    }
    return (top ? "T:" : "B:") + key;
  };
  if (options.remove_headers_footers && analysed.pages.size() >= 3) {
    for (const PageLayout& p : analysed.pages) {
      std::set<std::string> seen;
      for (const Block& b : p.blocks) {
        const std::string k = edge_key(p, b);
        if (!k.empty() && seen.insert(k).second) ++edge_counts[k];
      }
    }
  }
  const auto is_page_furniture = [&](const PageLayout& p, const Block& b) {
    if (!options.remove_headers_footers) return false;
    const std::string k = edge_key(p, b);
    if (k.empty()) return false;
    // A lone page number.
    std::string text;
    for (const Segment& s : b.lines) text += s.plain();
    const std::string_view trimmed = util::trim(text);
    const auto digits = std::count_if(trimmed.begin(), trimmed.end(), [](char c) { return util::is_ascii_digit(c); });
    if (!trimmed.empty() && trimmed.size() <= 12 && digits > 0 &&
        static_cast<std::size_t>(digits) + 6 >= trimmed.size()) {
      return true;
    }
    const auto it = edge_counts.find(k);
    return it != edge_counts.end() && it->second * 2 >= static_cast<int>(analysed.pages.size()) && it->second >= 3;
  };

  // Heading sizes.
  std::set<int> heading_sizes;
  for (const PageLayout& p : analysed.pages) {
    for (const Block& b : p.blocks) {
      if (b.kind != Block::Kind::Text || b.lines.size() > 3) continue;
      const BlockInfo bi = block_info(b);
      if (bi.size >= body * 1.15 && bi.chars > 0 && bi.chars < 200) heading_sizes.insert(static_cast<int>(std::lround(bi.size * 2)));
    }
  }
  std::vector<int> levels(heading_sizes.rbegin(), heading_sizes.rend());
  if (levels.size() > 5) levels.resize(5);
  const auto size_level = [&](double size) {
    const int key = static_cast<int>(std::lround(size * 2));
    for (std::size_t i = 0; i < levels.size(); ++i) {
      if (key >= levels[i]) return static_cast<int>(i + 1);
    }
    return 0;
  };

  // Assemble blocks.
  bool continue_paragraph = false;
  for (std::size_t pi = 0; pi < analysed.pages.size(); ++pi) {
    CVT_TRY(ctx.checkpoint(0.8 + 0.2 * static_cast<double>(pi) / static_cast<double>(analysed.pages.size()), "Layout"));
    PageLayout& page = analysed.pages[pi];
    const auto& links = analysed.links[pi];
    bool first_on_page = true;
    for (Block& block : page.blocks) {
      if (block.kind == Block::Kind::Image) {
        if (!options.images) continue;
        const PageImage& img = *block.image;
        ImageEvent ev;
        if (img.event.is_inline) {
          ev = img.event;
          ev.dict = &img.inline_dict;
        } else {
          ev.dict = &img.stream.as_dict();
          ev.stream = img.stream;
        }
        ir::ImageSource source;
        bool ok = false;
        if (!ev.is_inline) {
          if (auto jpeg = passthrough_jpeg(file, img.stream)) {
            source = ir::EncodedImage{std::move(*jpeg), "image/jpeg"};
            ok = true;
          }
        }
        if (!ok) {
          auto decoded = decode_image(file, ev, &ctx);
          if (decoded && !decoded->stencil) {
            ir::Image picture;
            ir::Frame frame;
            frame.width = decoded->width;
            frame.height = decoded->height;
            frame.rgba = std::move(decoded->rgba);
            picture.frames.push_back(std::move(frame));
            picture.dpi_x = picture.dpi_y = 72.0 * decoded->width / std::max(1.0, block.box.width());
            source = std::move(picture);
            ok = true;
          }
        }
        if (ok) {
          doc.blocks.push_back(ir::ImageBlock{std::move(source), "", ""});
          continue_paragraph = false;
        }
        continue;
      }
      if (block.kind == Block::Kind::Table) {
        ir::TableBlock table;
        for (std::size_t r = 0; r < block.cells.size(); ++r) {
          ir::TableRow row;
          for (auto& cell_segments : block.cells[r]) {
            std::sort(cell_segments.begin(), cell_segments.end(), [](const Segment& a, const Segment& b) {
              return std::abs(a.base - b.base) > 1 ? a.base < b.base : a.x0 < b.x0;
            });
            InlineBuilder builder(links);
            for (std::size_t k = 0; k < cell_segments.size(); ++k) {
              const bool hyphen = k > 0 && builder.ends_with_hyphen();
              if (hyphen) builder.drop_trailing_hyphen();
              builder.add_line(cell_segments[k], cell_segments[k].size, k == 0 || hyphen ? "" : " ");
            }
            row.cells.push_back(ir::TableCell{builder.build(), 1, 1});
          }
          // Header row: first row entirely bold.
          if (r == 0) {
            bool bold = true, any = false;
            for (const auto& cell : block.cells[0]) {
              for (const Segment& s : cell) {
                for (const Glyph& g : s.glyphs) {
                  if (g.space) continue;
                  any = true;
                  bold = bold && g.bold;
                }
              }
            }
            row.header = any && bold;
          }
          table.rows.push_back(std::move(row));
        }
        // Equalise row lengths.
        std::size_t cols = 0;
        for (const auto& row : table.rows) cols = std::max(cols, row.cells.size());
        for (auto& row : table.rows) {
          while (row.cells.size() < cols) row.cells.push_back(ir::TableCell{{}, 1, 1});
        }
        if (!table.rows.empty()) doc.blocks.push_back(std::move(table));
        continue_paragraph = false;
        continue;
      }
      if (block.lines.empty() || is_page_furniture(page, block)) continue;
      const BlockInfo bi = block_info(block);
      if (bi.chars == 0) continue;
      // Headings.
      int level = 0;
      const std::string key = normalized_key(bi.text);
      const auto outline_it = outline_levels.find(key);
      const bool in_outline = outline_it != outline_levels.end() && block.lines.size() <= 3;
      if (block.lines.size() <= 3 && bi.chars < 200) {
        level = size_level(bi.size);
        if (level == 0 && bi.all_bold && block.lines.size() <= 2 && bi.chars < 100 && bi.size >= body * 0.95) {
          const std::string_view t = util::trim(bi.text);
          const bool sentence = !t.empty() && (t.back() == '.' || t.back() == ':' || t.back() == ',');
          if (!sentence) level = std::min(static_cast<int>(levels.size()) + 1, 6);
        }
        if (level == 0 && in_outline) level = std::min(static_cast<int>(levels.size()) + outline_it->second, 6);
      }
      if (level > 0) {
        InlineBuilder builder(links);
        for (std::size_t k = 0; k < block.lines.size(); ++k) builder.add_line(block.lines[k], block.lines[k].size, k == 0 ? "" : " ");
        ir::Inlines content = builder.build();
        // Headings carry their own weight: drop a uniform Strong wrapper.
        if (content.size() == 1) {
          if (auto* s = content[0].get_if<ir::Strong>()) {
            ir::Inlines inner = std::move(s->children);
            content = std::move(inner);
          }
        }
        doc.blocks.push_back(ir::Heading{level, std::move(content)});
        continue_paragraph = false;
        first_on_page = false;
        continue;
      }
      // Code blocks.
      if (bi.all_mono && block.lines.size() >= 2) {
        double char_width = 0;
        std::size_t n = 0;
        for (const Segment& s : block.lines) {
          for (const Glyph& g : s.glyphs) {
            if (!g.space && g.text.size() == 1) {
              char_width += g.x1 - g.x0;
              ++n;
            }
          }
        }
        char_width = n > 0 ? char_width / static_cast<double>(n) : block.lines[0].size * 0.6;
        std::string code;
        double left = 1e18;
        for (const Segment& s : block.lines) left = std::min(left, s.x0);
        for (std::size_t k = 0; k < block.lines.size(); ++k) {
          const Segment& s = block.lines[k];
          if (k > 0) code.push_back('\n');
          const int indent = char_width > 0 ? static_cast<int>(std::lround((s.x0 - left) / char_width)) : 0;
          code.append(static_cast<std::size_t>(std::clamp(indent, 0, 80)), ' ');
          // Keep the spacing inside the line by character positions.
          double cursor = s.x0;
          for (const Glyph& g : s.glyphs) {
            if (g.space) continue;
            const int gap = char_width > 0 ? static_cast<int>(std::lround((g.x0 - cursor) / char_width)) : 0;
            if (cursor != s.x0 && gap > 0) code.append(static_cast<std::size_t>(std::min(gap, 80)), ' ');
            code += g.text;
            cursor = g.x1;
          }
        }
        doc.blocks.push_back(ir::CodeBlock{"", code});
        continue_paragraph = false;
        first_on_page = false;
        continue;
      }
      // Lists and paragraphs.
      // The list marker at the start of a line: glyphs up to a space or a gap,
      // or a single bullet character.
      const auto marker = [](const Segment& s) {
        std::string token;
        std::size_t count = 0;
        for (std::size_t i = 0; i < s.glyphs.size() && count < 8; ++i) {
          const Glyph& g = s.glyphs[i];
          if (g.space) break;
          if (i > 0 && g.x0 - s.glyphs[i - 1].x1 > 0.16 * g.size) break;
          token += g.text;
          ++count;
          if (i == 0 && is_bullet(token)) break;
        }
        return std::pair<std::string, std::size_t>{token, count};
      };
      const auto first_token = [&](const Segment& s) { return marker(s).first; };
      double left = 1e18, right = -1e18;
      for (const Segment& s : block.lines) {
        left = std::min(left, s.x0);
        right = std::max(right, s.x1);
      }
      struct Para {
        std::vector<const Segment*> lines;
        bool list_item = false;
        bool ordered = false;
        int number = 1;
        double indent = 0;
      };
      std::vector<Para> paras;
      for (std::size_t k = 0; k < block.lines.size(); ++k) {
        const Segment& s = block.lines[k];
        const std::string token = first_token(s);
        const bool bullet = is_bullet(token) && s.glyphs.size() > 1;
        const auto number = list_number(token);
        bool starts_item = (bullet || number) && (k == 0 || s.x0 <= block.lines[k - 1].x0 + 2);
        if (number && k > 0 && !paras.empty() && !paras.back().list_item) starts_item = starts_item && s.x0 < left + 2;
        bool new_para = k == 0 || starts_item;
        if (!new_para && k > 0) {
          const Segment& prev = block.lines[k - 1];
          const double size = s.size;
          if (s.x0 > left + 1.0 * size && prev.x1 > right - 2 * size && !paras.back().list_item) new_para = true;  // indented first line
          if (prev.x1 < left + 0.7 * (right - left) && s.x0 < left + 0.5 * size && !paras.back().list_item &&
              block.lines.size() > 2) {
            new_para = true;  // previous line ended short
          }
        }
        if (new_para) {
          Para p;
          p.list_item = starts_item;
          p.ordered = number.has_value() && !bullet;
          p.number = number.value_or(1);
          p.indent = s.x0;
          paras.push_back(std::move(p));
        }
        paras.back().lines.push_back(&s);
      }
      for (std::size_t p = 0; p < paras.size(); ++p) {
        Para& para = paras[p];
        InlineBuilder builder(links);
        for (std::size_t k = 0; k < para.lines.size(); ++k) {
          Segment line = *para.lines[k];
          if (k == 0 && para.list_item) {
            // Strip the marker.
            std::size_t skip = marker(line).second;
            while (skip < line.glyphs.size() && line.glyphs[skip].space) ++skip;
            line.glyphs.erase(line.glyphs.begin(), line.glyphs.begin() + static_cast<std::ptrdiff_t>(skip));
          }
          const bool hyphen = k > 0 && builder.ends_with_hyphen() && !line.glyphs.empty() &&
                              util::is_ascii_alpha(line.glyphs.front().text.empty() ? ' ' : line.glyphs.front().text[0]) &&
                              is_lower_start(line.glyphs.front().text);
          if (hyphen) builder.drop_trailing_hyphen();
          builder.add_line(line, line.size, k == 0 || hyphen ? "" : " ");
        }
        if (builder.empty()) continue;
        ir::Inlines content = builder.build();
        if (para.list_item) {
          // Append to an existing list when the previous block is a list of the same kind.
          ir::Blocks item;
          item.push_back(ir::Paragraph{std::move(content)});
          ir::List* list = doc.blocks.empty() ? nullptr : doc.blocks.back().get_if<ir::List>();
          if (list != nullptr && list->ordered == para.ordered) {
            list->items.push_back(std::move(item));
          } else {
            ir::List fresh;
            fresh.ordered = para.ordered;
            fresh.start = para.ordered ? para.number : 1;
            fresh.items.push_back(std::move(item));
            doc.blocks.push_back(std::move(fresh));
          }
          continue_paragraph = false;
          continue;
        }
        // Paragraph continuing from the previous page.
        if (first_on_page && p == 0 && continue_paragraph && !doc.blocks.empty()) {
          if (auto* prev = doc.blocks.back().get_if<ir::Paragraph>()) {
            const std::string text = ir::plain_text(content);
            if (is_lower_start(text)) {
              std::string prev_text = ir::plain_text(prev->content);
              bool hyphenated = prev_text.size() > 1 && prev_text.back() == '-';
              if (hyphenated) {
                if (auto* t = prev->content.back().get_if<ir::Text>(); t && !t->text.empty() && t->text.back() == '-') {
                  t->text.pop_back();
                } else {
                  hyphenated = false;
                }
              }
              if (!hyphenated) prev->content.push_back(ir::Text{" "});
              for (ir::Inline& node : content) prev->content.push_back(std::move(node));
              const std::string all = ir::plain_text(prev->content);
              continue_paragraph = !all.empty() && std::string_view(".!?:\"").find(all.back()) == std::string_view::npos;
              first_on_page = false;
              continue;
            }
          }
        }
        const std::string text = ir::plain_text(content);
        continue_paragraph = !text.empty() && std::string_view(".!?:\")").find(text.back()) == std::string_view::npos;
        doc.blocks.push_back(ir::Paragraph{std::move(content)});
      }
      first_on_page = false;
    }
  }
  if (doc.blocks.empty() && !options.pages.empty()) {
    ctx.warn("No text was found in the PDF; it may contain only scanned images.");
  }
  for (const std::string& w : file.warnings()) ctx.warn(w);
  return doc;
}

}  // namespace cvt::pdf
