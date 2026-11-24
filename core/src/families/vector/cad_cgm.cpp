// Convertible core - vector family: CGM (Computer Graphics Metafile, ISO/IEC
// 8632) reader, binary and clear-text encodings.
//
// Both encodings are decoded into the same element calls: a binary element
// reads typed parameters with the current precisions, a clear-text
// statement reads them from tokens. Descriptors (VDC type and extent,
// scaling, colour selection, value extent and table, width modes), the
// output primitives (polylines, polymarkers, polygons and polygon sets,
// rectangles, circles, arcs, ellipses, Béziers, text, cell arrays) and the
// line, edge, fill and text attributes are supported; every picture
// becomes a page. There is no CGM writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <memory>
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
constexpr std::uint64_t max_points = 20'000'000;

struct RealPrecision {
  bool fixed = true;
  int whole = 16;  // exponent bits for floating point
  int fraction = 16;
};

struct Precisions {
  int integer = 16;
  int index = 16;
  int color = 8;
  int color_index = 8;
  RealPrecision real;
  int vdc_integer = 16;
  RealPrecision vdc_real;
  bool vdc_real_type = false;
  bool direct_color = false;
  std::array<double, 6> color_extent{0, 255, 0, 255, 0, 255};
};

// --------------------------------------------------------------------------- parameter sources

class Params {
 public:
  explicit Params(const Precisions& p) : prec_(p) {}
  virtual ~Params() = default;
  Params(const Params&) = delete;
  Params& operator=(const Params&) = delete;

  [[nodiscard]] virtual bool more() const = 0;
  virtual double vdc() = 0;
  virtual std::int64_t integer() = 0;
  virtual std::int64_t index() = 0;
  virtual double real() = 0;
  virtual int enumeration(std::initializer_list<std::string_view> names) = 0;
  virtual std::int64_t color_index(int precision = 0) = 0;
  virtual std::array<double, 3> color_direct(int precision = 0) = 0;
  virtual std::string string() = 0;
  /// Start of a cell array row (binary rows are word aligned).
  virtual void align_row() {}
  [[nodiscard]] virtual bool binary() const = 0;

  Point point() {
    const double x = vdc();
    return {x, vdc()};
  }

 protected:
  const Precisions& prec_;
};

class BinaryParams final : public Params {
 public:
  BinaryParams(const Precisions& p, ByteView data) : Params(p), data_(data) {}

  [[nodiscard]] bool more() const override { return bit_ / 8 < data_.size() && ok_; }
  [[nodiscard]] bool binary() const override { return true; }

  std::uint64_t bits(int count) {
    std::uint64_t v = 0;
    for (int i = 0; i < count; ++i) {
      const std::size_t byte = bit_ / 8;
      if (byte >= data_.size()) {
        ok_ = false;
        ++bit_;
        continue;
      }
      const int shift = 7 - static_cast<int>(bit_ % 8);
      v = (v << 1) | ((static_cast<unsigned>(data_[byte]) >> shift) & 1u);
      ++bit_;
    }
    return v;
  }
  void align_byte() { bit_ = (bit_ + 7) / 8 * 8; }
  void align_row() override {
    align_byte();
    if ((bit_ / 8) % 2 == 1) bit_ += 8;
  }

  std::int64_t signed_value(int width) {
    align_byte();
    width = std::clamp(width, 8, 64) / 8 * 8;
    const std::uint64_t v = bits(width);
    if (width < 64 && (v >> (width - 1)) != 0) return static_cast<std::int64_t>(v) - (std::int64_t{1} << width);
    return static_cast<std::int64_t>(v);
  }
  std::uint64_t unsigned_value(int width) {
    if (width >= 8) align_byte();
    return bits(std::clamp(width, 1, 32));
  }
  double real_with(const RealPrecision& r) {
    align_byte();
    if (r.fixed) {
      const int whole = std::clamp(r.whole, 8, 32) / 8 * 8;
      const std::int64_t w = signed_value(whole);
      const std::uint64_t f = bits(std::clamp(r.fraction, 8, 32) / 8 * 8);
      return static_cast<double>(w) + static_cast<double>(f) / std::ldexp(1.0, std::clamp(r.fraction, 8, 32) / 8 * 8);
    }
    if (r.whole + r.fraction > 32) {
      const double d = std::bit_cast<double>(bits(64));
      return std::isfinite(d) ? d : 0.0;
    }
    const float f = std::bit_cast<float>(static_cast<std::uint32_t>(bits(32)));
    return std::isfinite(f) ? f : 0.0;
  }

  double vdc() override {
    return prec_.vdc_real_type ? real_with(prec_.vdc_real) : static_cast<double>(signed_value(prec_.vdc_integer));
  }
  std::int64_t integer() override { return signed_value(prec_.integer); }
  std::int64_t index() override { return signed_value(prec_.index); }
  double real() override { return real_with(prec_.real); }
  int enumeration(std::initializer_list<std::string_view> /*names*/) override {
    return static_cast<int>(signed_value(16));
  }
  std::int64_t color_index(int precision) override {
    return static_cast<std::int64_t>(unsigned_value(precision > 0 ? precision : prec_.color_index));
  }
  std::array<double, 3> color_direct(int precision) override {
    const int p = precision > 0 ? precision : prec_.color;
    std::array<double, 3> c{};
    for (double& v : c) v = static_cast<double>(unsigned_value(p));
    return c;
  }
  std::string string() override {
    align_byte();
    std::size_t length = unsigned_value(8);
    if (length == 255) length = unsigned_value(16) & 0x7FFF;
    std::string s;
    for (std::size_t i = 0; i < length && more(); ++i) s.push_back(static_cast<char>(bits(8)));
    return s;
  }
  [[nodiscard]] std::size_t bit_position() const { return bit_; }

 private:
  ByteView data_;
  std::size_t bit_ = 0;
  bool ok_ = true;
};

struct Token {
  enum class Kind : std::uint8_t { Number, Word, String };
  Kind kind = Kind::Number;
  double number = 0;
  std::string text;
};

class TextParams final : public Params {
 public:
  TextParams(const Precisions& p, const std::vector<Token>& tokens) : Params(p), tokens_(tokens) {}
  [[nodiscard]] bool more() const override { return at_ < tokens_.size(); }
  [[nodiscard]] bool binary() const override { return false; }

  double number() {
    if (at_ >= tokens_.size()) return 0;
    const Token& t = tokens_[at_++];
    if (t.kind == Token::Kind::Number) return t.number;
    return util::parse_double(t.text).value_or(0.0);
  }
  double vdc() override { return number(); }
  std::int64_t integer() override { return static_cast<std::int64_t>(std::clamp(number(), -9e18, 9e18)); }
  std::int64_t index() override { return integer(); }
  double real() override { return number(); }
  int enumeration(std::initializer_list<std::string_view> names) override {
    if (at_ >= tokens_.size()) return 0;
    const Token& t = tokens_[at_++];
    if (t.kind == Token::Kind::Number) return static_cast<int>(t.number);
    int i = 0;
    for (const std::string_view n : names) {
      if (util::iequals(n, t.text)) return i;
      ++i;
    }
    return 0;
  }
  std::int64_t color_index(int /*precision*/) override { return integer(); }
  std::array<double, 3> color_direct(int /*precision*/) override {
    const double r = number(), g = number();
    return {r, g, number()};
  }
  std::string string() override {
    if (at_ >= tokens_.size()) return {};
    return tokens_[at_++].text;
  }

 private:
  const std::vector<Token>& tokens_;
  std::size_t at_ = 0;
};

// --------------------------------------------------------------------------- interpreter

struct Attributes {
  Color line_color{0, 0, 0, 1};
  double line_width = 1;
  int line_type = 1;
  LineCap line_cap = LineCap::Butt;
  LineJoin line_join = LineJoin::Miter;
  Color fill_color{0, 0, 0, 1};
  int interior = 0;  // 0 hollow, 1 solid, 2 pattern, 3 hatch, 4 empty
  Color edge_color{0, 0, 0, 1};
  double edge_width = 1;
  int edge_type = 1;
  bool edge_visible = false;
  Color text_color{0, 0, 0, 1};
  double char_height = 0;
  Point char_up{0, 1};
  Point char_base{1, 0};
  int halign = 0;
  int valign = 0;
  Color marker_color{0, 0, 0, 1};
  int marker_type = 3;
  double marker_size = 1;
  double char_expansion = 1;
};

class Interpreter {
 public:
  explicit Interpreter(ReadContext& ctx) : ctx_(ctx) {
    table_[0] = {1, 1, 1, 1};
    table_[1] = {0, 0, 0, 1};
    table_[2] = {1, 0, 0, 1};
    table_[3] = {0, 1, 0, 1};
    table_[4] = {0, 0, 1, 1};
    table_[5] = {1, 1, 0, 1};
    table_[6] = {0, 1, 1, 1};
    table_[7] = {1, 0, 1, 1};
  }

  Precisions prec;

  Result<void> element(int cls, int id, Params& p) {
    if ((++elements_ & 0x3FFF) == 0) CVT_TRY(ctx_.checkpoint(0.5));
    switch (cls) {
      case 0: return delimiter(id);
      case 1: return metafile_descriptor(id, p);
      case 2: return picture_descriptor(id, p);
      case 3: return control(id, p);
      case 4: return primitive(id, p);
      case 5: return attribute(id, p);
      default: return {};
    }
  }

  Result<Scene> finish() {
    if (in_picture_) end_picture();
    if (pages_.empty()) return fail(Status::Decode, "the CGM has no pictures");
    Scene scene;
    scene.source_units = metric_ ? "mm" : "px";
    scene.pages = std::move(pages_);
    return scene;
  }

 private:
  // ------------------------------------------------------------------ helpers

  [[nodiscard]] double extent_size() const {
    return std::max({std::abs(vdc_max_.x - vdc_min_.x), std::abs(vdc_max_.y - vdc_min_.y), 1e-9});
  }
  [[nodiscard]] double nominal() const { return extent_size() / 1000.0; }

  double width_value(Params& p, int mode) {
    if (mode == 0) return std::abs(p.vdc());
    const double v = std::abs(p.real());
    switch (mode) {
      case 1: return v * nominal();
      case 2: return v * extent_size();
      default: {
        // millimetres
        const double vdc_per_mm = metric_ ? 1.0 / metric_scale_ : 96.0 / 25.4 / (page_scale() > 0 ? page_scale() : 1.0);
        return v * vdc_per_mm;
      }
    }
  }

  [[nodiscard]] double page_scale() const {
    // px per VDC unit
    if (metric_) return metric_scale_ * px_per_mm;
    return 1000.0 / extent_size();
  }

  Color read_color(Params& p) {
    if (prec.direct_color) {
      const auto c = p.color_direct();
      const auto comp = [&](int i) {
        const double lo = prec.color_extent[static_cast<std::size_t>(2 * i)];
        const double hi = prec.color_extent[static_cast<std::size_t>(2 * i + 1)];
        return static_cast<float>(std::clamp((c[static_cast<std::size_t>(i)] - lo) / (hi - lo != 0 ? hi - lo : 1.0), 0.0, 1.0));
      };
      return {comp(0), comp(1), comp(2), 1};
    }
    return indexed(p.color_index());
  }
  [[nodiscard]] Color indexed(std::int64_t i) const {
    const auto it = table_.find(i);
    return it != table_.end() ? it->second : Color{0, 0, 0, 1};
  }

  std::vector<Point> points(Params& p) {
    std::vector<Point> out;
    while (p.more() && out.size() < 10'000'000) {
      const Point q = p.point();
      if (!std::isfinite(q.x) || !std::isfinite(q.y)) break;
      out.push_back(q);
    }
    return out;
  }

  Result<void> count(std::size_t n) {
    points_ += n + 1;
    if (points_ > max_points) return fail(Status::Limit, "the CGM draws more than {} points", max_points);
    return {};
  }

  [[nodiscard]] std::vector<double> dash_for(int type, double width) const {
    const double u = std::max(width, nominal());
    switch (type) {
      case 2: return {6 * u, 3 * u};
      case 3: return {u, 2 * u};
      case 4: return {6 * u, 2 * u, u, 2 * u};
      case 5: return {6 * u, 2 * u, u, 2 * u, u, 2 * u};
      default: return {};
    }
  }

  Result<void> line(Path path) {
    if (path.empty()) return {};
    CVT_TRY(count(path.points.size()));
    Shape s;
    s.path = std::move(path);
    s.stroke.paint = Paint::solid(attr_.line_color);
    s.stroke.width = std::max(attr_.line_width, 1e-9);
    s.stroke.cap = attr_.line_cap;
    s.stroke.join = attr_.line_join;
    s.stroke.dash = dash_for(attr_.line_type, attr_.line_width);
    page_.children.push_back(Node{std::move(s)});
    return {};
  }

  Result<void> area(Path path, FillRule rule = FillRule::EvenOdd, bool closed_edges = true) {
    if (path.empty()) return {};
    CVT_TRY(count(path.points.size()));
    Shape s;
    s.path = std::move(path);
    s.fill_rule = rule;
    if (attr_.interior == 1 || attr_.interior == 2) {
      s.fill = Paint::solid(attr_.fill_color);
    } else if (attr_.interior == 3) {
      s.fill = Paint::solid(attr_.fill_color, 0.35);
      if (!hatch_warned_) {
        hatch_warned_ = true;
        ctx_.warn("CGM hatch fills were drawn as translucent solid fills.");
      }
    }
    if (attr_.edge_visible && closed_edges) {
      s.stroke.paint = Paint::solid(attr_.edge_color);
      s.stroke.width = std::max(attr_.edge_width, 1e-9);
      s.stroke.dash = dash_for(attr_.edge_type, attr_.edge_width);
    } else if (attr_.interior == 0) {  // hollow: the boundary in the fill colour
      s.stroke.paint = Paint::solid(attr_.fill_color);
      s.stroke.width = std::max(attr_.edge_width, nominal());
    }
    if (!s.fill.visible() && !s.stroke.paint.visible()) return {};
    page_.children.push_back(Node{std::move(s)});
    return {};
  }

  static Path polyline_path(const std::vector<Point>& pts, bool close) {
    Path path;
    for (std::size_t i = 0; i < pts.size(); ++i) {
      if (i == 0) path.move_to(pts[i]);
      else path.line_to(pts[i]);
    }
    if (close && pts.size() > 2) path.close();
    return path;
  }

  // ------------------------------------------------------------------ class 0

  Result<void> delimiter(int id) {
    if (id == 3) {
      if (in_picture_) end_picture();
      in_picture_ = true;
      page_ = Group{};
      attr_ = Attributes{};
      attr_.line_width = attr_.edge_width = nominal();
      background_.reset();
    } else if (id == 4) {
      attr_.line_width = line_width_mode_ == 1 ? nominal() : attr_.line_width;
      attr_.edge_width = edge_width_mode_ == 1 ? nominal() : attr_.edge_width;
    } else if (id == 5) {
      end_picture();
    }
    return {};
  }

  void end_picture() {
    if (!in_picture_) return;
    in_picture_ = false;
    Page page;
    const double k = page_scale();
    const double w = std::abs(vdc_max_.x - vdc_min_.x) * k, h = std::abs(vdc_max_.y - vdc_min_.y) * k;
    page.width = std::max(w, 1.0);
    page.height = std::max(h, 1.0);
    const double kx = vdc_max_.x >= vdc_min_.x ? k : -k;
    const double ky = vdc_max_.y >= vdc_min_.y ? k : -k;
    page.root.transform = Matrix{kx, 0, 0, -ky, -vdc_min_.x * kx, vdc_max_.y * ky};
    if (background_ && !(background_->r == 1 && background_->g == 1 && background_->b == 1)) {
      Shape bg;
      add_rect(bg.path, std::min(vdc_min_.x, vdc_max_.x), std::min(vdc_min_.y, vdc_max_.y),
               std::abs(vdc_max_.x - vdc_min_.x), std::abs(vdc_max_.y - vdc_min_.y));
      bg.fill = Paint::solid(*background_);
      page.root.children.push_back(Node{std::move(bg)});
    }
    for (Node& n : page_.children) page.root.children.push_back(std::move(n));
    page.name = std::format("Picture {}", pages_.size() + 1);
    page_ = Group{};
    pages_.push_back(std::move(page));
  }

  // ------------------------------------------------------------------ class 1

  RealPrecision read_real_precision(Params& p) {
    RealPrecision r;
    if (!p.binary()) return r;
    r.fixed = p.enumeration({"float", "fixed"}) == 1;
    r.whole = static_cast<int>(p.integer());
    r.fraction = static_cast<int>(p.integer());
    if (r.fixed) {
      r.whole = std::clamp(r.whole, 8, 32);
      r.fraction = std::clamp(r.fraction, 8, 32);
    }
    return r;
  }

  Result<void> metafile_descriptor(int id, Params& p) {
    switch (id) {
      case 3: prec.vdc_real_type = p.enumeration({"integer", "real"}) == 1; break;
      case 4: if (p.binary()) prec.integer = static_cast<int>(std::clamp<std::int64_t>(p.integer(), 8, 32)); break;
      case 5: if (p.binary()) prec.real = read_real_precision(p); break;
      case 6: if (p.binary()) prec.index = static_cast<int>(std::clamp<std::int64_t>(p.integer(), 8, 32)); break;
      case 7: if (p.binary()) prec.color = static_cast<int>(std::clamp<std::int64_t>(p.integer(), 1, 32)); break;
      case 8: if (p.binary()) prec.color_index = static_cast<int>(std::clamp<std::int64_t>(p.integer(), 1, 32)); break;
      case 10: {
        const auto lo = p.color_direct();
        const auto hi = p.color_direct();
        for (std::size_t i = 0; i < 3; ++i) {
          prec.color_extent[2 * i] = lo[i];
          prec.color_extent[2 * i + 1] = hi[i];
        }
        break;
      }
      default: break;
    }
    return {};
  }

  // ------------------------------------------------------------------ class 2

  Result<void> picture_descriptor(int id, Params& p) {
    switch (id) {
      case 1: {
        metric_ = p.enumeration({"abstract", "metric"}) == 1;
        // The scale factor is always a floating point real in the binary encoding.
        if (p.binary()) {
          auto& b = static_cast<BinaryParams&>(p);
          metric_scale_ = b.real_with(RealPrecision{false, 9, 23});
        } else {
          metric_scale_ = p.real();
        }
        if (!(metric_scale_ > 0) || !std::isfinite(metric_scale_)) metric_ = false;
        break;
      }
      case 2: prec.direct_color = p.enumeration({"indexed", "direct"}) == 1; break;
      case 3: line_width_mode_ = p.enumeration({"abstract", "scaled", "fractional", "mm"}); break;
      case 4: marker_size_mode_ = p.enumeration({"abstract", "scaled", "fractional", "mm"}); break;
      case 5: edge_width_mode_ = p.enumeration({"abstract", "scaled", "fractional", "mm"}); break;
      case 6: {
        const Point a = p.point(), b = p.point();
        if (std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(b.x) && std::isfinite(b.y) && a.x != b.x &&
            a.y != b.y) {
          vdc_min_ = a;
          vdc_max_ = b;
        }
        break;
      }
      case 7: background_ = read_color(p); break;
      default: break;
    }
    return {};
  }

  // ------------------------------------------------------------------ class 3

  Result<void> control(int id, Params& p) {
    if (id == 1 && p.binary()) prec.vdc_integer = static_cast<int>(std::clamp<std::int64_t>(p.integer(), 8, 32));
    if (id == 2 && p.binary()) prec.vdc_real = read_real_precision(p);
    return {};
  }

  // ------------------------------------------------------------------ class 5

  Result<void> attribute(int id, Params& p) {
    switch (id) {
      case 2: attr_.line_type = static_cast<int>(p.index()); break;
      case 3: attr_.line_width = width_value(p, line_width_mode_); break;
      case 4: attr_.line_color = read_color(p); break;
      case 6: attr_.marker_type = static_cast<int>(p.index()); break;
      case 7: attr_.marker_size = width_value(p, marker_size_mode_); break;
      case 8: attr_.marker_color = read_color(p); break;
      case 12: attr_.char_expansion = std::clamp(p.real(), 0.01, 100.0); break;
      case 14: attr_.text_color = read_color(p); break;
      case 15: attr_.char_height = std::abs(p.vdc()); break;
      case 16: {
        const double ux = p.vdc(), uy = p.vdc(), bx = p.vdc(), by = p.vdc();
        if (std::hypot(ux, uy) > 0 && std::hypot(bx, by) > 0) {
          attr_.char_up = {ux, uy};
          attr_.char_base = {bx, by};
        }
        break;
      }
      case 18:
        attr_.halign = p.enumeration({"normhoriz", "left", "ctr", "right", "conthoriz"});
        attr_.valign = p.enumeration({"normvert", "top", "cap", "half", "base", "bottom", "contvert"});
        break;
      case 22: attr_.interior = p.enumeration({"hollow", "solid", "pat", "hatch", "empty"}); break;
      case 23: attr_.fill_color = read_color(p); break;
      case 27: attr_.edge_type = static_cast<int>(p.index()); break;
      case 28: attr_.edge_width = width_value(p, edge_width_mode_); break;
      case 29: attr_.edge_color = read_color(p); break;
      case 30: attr_.edge_visible = p.enumeration({"off", "on"}) == 1; break;
      case 34: {
        std::int64_t i = p.color_index();
        int guard = 0;
        while (p.more() && ++guard <= 65536) {
          const auto c = p.color_direct();
          const auto comp = [&](std::size_t k) {
            const double lo = prec.color_extent[2 * k], hi = prec.color_extent[2 * k + 1];
            return static_cast<float>(std::clamp((c[k] - lo) / (hi - lo != 0 ? hi - lo : 1.0), 0.0, 1.0));
          };
          table_[i++] = Color{comp(0), comp(1), comp(2), 1};
        }
        break;
      }
      case 37: {
        const auto cap = p.index();
        attr_.line_cap = cap == 2 ? LineCap::Round : cap == 3 ? LineCap::Square : LineCap::Butt;
        break;
      }
      case 38: {
        const auto join = p.index();
        attr_.line_join = join == 2 ? LineJoin::Miter : join == 3 ? LineJoin::Round : join == 4 ? LineJoin::Bevel : LineJoin::Miter;
        break;
      }
      default: break;
    }
    return {};
  }

  // ------------------------------------------------------------------ class 4

  Result<void> text_at(Point at, const std::string& raw) {
    if (raw.empty()) return {};
    const std::string utf8 = util::is_valid_utf8(raw) ? raw : [&] {
      std::string s;
      for (const char c : raw) append_utf8(s, static_cast<unsigned char>(c));
      return s;
    }();
    double h = attr_.char_height > 0 ? attr_.char_height : nominal() * 10;
    FontSpec font;
    font.family = "sans-serif";
    font.size = h / cap_height_ratio;
    const TextLayout layout = layout_text(utf8, font);
    const double base_len = length(attr_.char_base), up_len = length(attr_.char_up);
    const Point bx = attr_.char_base * (1.0 / base_len), uy = attr_.char_up * (1.0 / up_len);
    const double expand = attr_.char_expansion * (base_len / up_len);
    const double advance = layout.advance * expand;
    const double dx = attr_.halign == 2 ? -advance / 2 : attr_.halign == 3 ? -advance : 0.0;
    double dy = 0;  // in y-down text space
    switch (attr_.valign) {
      case 1: case 2: dy = h; break;
      case 3: dy = h / 2; break;
      case 5: dy = -layout.descent; break;
      default: break;
    }
    Text t = make_text(utf8, font, {0, 0}, Paint::solid(attr_.text_color));
    CVT_TRY(count(t.outline.points.size()));
    Group g;
    // Text space (y down) -> VDC: x along the base vector, y against the up vector.
    g.transform = Matrix{bx.x, bx.y, -uy.x, -uy.y, at.x, at.y} * Matrix::translate(dx, dy) * Matrix::scale(expand, 1);
    g.children.push_back(Node{std::move(t)});
    page_.children.push_back(Node{std::move(g)});
    text_end_ = at + bx * advance;
    return {};
  }

  Result<void> marker(Point c) {
    const double r = std::max(attr_.marker_size * (marker_size_mode_ == 1 ? 3.0 : 1.0), nominal() * 2) * 0.5;
    Path path;
    Shape s;
    switch (attr_.marker_type) {
      case 1:  // dot
        add_ellipse(path, c.x, c.y, r * 0.3, r * 0.3);
        s.fill = Paint::solid(attr_.marker_color);
        break;
      case 4:  // circle
        add_ellipse(path, c.x, c.y, r, r);
        break;
      case 2:  // plus
        path.move_to({c.x - r, c.y});
        path.line_to({c.x + r, c.y});
        path.move_to({c.x, c.y - r});
        path.line_to({c.x, c.y + r});
        break;
      case 5:  // cross
        path.move_to({c.x - r, c.y - r});
        path.line_to({c.x + r, c.y + r});
        path.move_to({c.x - r, c.y + r});
        path.line_to({c.x + r, c.y - r});
        break;
      default:  // asterisk
        for (int i = 0; i < 3; ++i) {
          const double a = i * pi / 3 + pi / 2;
          path.move_to({c.x - r * std::cos(a), c.y - r * std::sin(a)});
          path.line_to({c.x + r * std::cos(a), c.y + r * std::sin(a)});
        }
        break;
    }
    if (!s.fill.visible()) {
      s.stroke.paint = Paint::solid(attr_.marker_color);
      s.stroke.width = std::max(nominal(), r * 0.15);
    }
    s.path = std::move(path);
    CVT_TRY(count(s.path.points.size()));
    page_.children.push_back(Node{std::move(s)});
    return {};
  }

  // Angle of vector v in the frame of conjugate diameters u, w.
  static double frame_angle(Point v, Point u, Point w) {
    const double det = u.x * w.y - u.y * w.x;
    if (std::abs(det) < 1e-300) return std::atan2(v.y, v.x);
    const double a = (v.x * w.y - v.y * w.x) / det;
    const double b = (u.x * v.y - u.y * v.x) / det;
    return std::atan2(b, a);
  }

  Result<void> primitive(int id, Params& p) {
    if (!in_picture_) {
      // Primitives outside a picture body: open an implicit picture.
      in_picture_ = true;
      page_ = Group{};
    }
    switch (id) {
      case 1: return line(polyline_path(points(p), false));
      case 2: {
        const std::vector<Point> pts = points(p);
        Path path;
        for (std::size_t i = 0; i + 1 < pts.size(); i += 2) {
          path.move_to(pts[i]);
          path.line_to(pts[i + 1]);
        }
        return line(std::move(path));
      }
      case 3: {
        for (const Point& c : points(p)) CVT_TRY(marker(c));
        return {};
      }
      case 4: {
        const Point at = p.point();
        (void)p.enumeration({"notfinal", "final"});
        return text_at(at, p.string());
      }
      case 5: {
        (void)p.vdc();
        (void)p.vdc();
        const Point at = p.point();
        (void)p.enumeration({"notfinal", "final"});
        return text_at(at, p.string());
      }
      case 6: {
        (void)p.enumeration({"notfinal", "final"});
        return text_at(text_end_, p.string());
      }
      case 7: return area(polyline_path(points(p), true));
      case 8: {
        Path path;
        bool open = false;
        while (p.more()) {
          const Point q = p.point();
          const int flag = p.enumeration({"invis", "vis", "closeinvis", "closevis"});
          if (!std::isfinite(q.x) || !std::isfinite(q.y)) break;
          if (!open) {
            path.move_to(q);
            open = true;
          } else {
            path.line_to(q);
          }
          if (flag >= 2) {
            path.close();
            open = false;
          }
          if (path.points.size() > 10'000'000) break;
        }
        return area(std::move(path));
      }
      case 9: return cell_array(p);
      case 11: {
        const Point a = p.point(), b = p.point();
        Path path;
        add_rect(path, std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y));
        return area(std::move(path));
      }
      case 12: {
        const Point c = p.point();
        const double r = std::abs(p.vdc());
        Path path;
        add_ellipse(path, c.x, c.y, r, r);
        return area(std::move(path));
      }
      case 13:
      case 14: {
        const Point a = p.point(), m = p.point(), e = p.point();
        const int close = id == 14 ? p.enumeration({"pie", "chord"}) : -1;
        const double d = 2 * (a.x * (m.y - e.y) + m.x * (e.y - a.y) + e.x * (a.y - m.y));
        Path path;
        if (std::abs(d) < 1e-12) {
          path.move_to(a);
          path.line_to(e);
          return line(std::move(path));
        }
        const double a2 = dot(a, a), m2 = dot(m, m), e2 = dot(e, e);
        const Point c{(a2 * (m.y - e.y) + m2 * (e.y - a.y) + e2 * (a.y - m.y)) / d,
                      (a2 * (e.x - m.x) + m2 * (a.x - e.x) + e2 * (m.x - a.x)) / d};
        const double r = length(a - c);
        const double sa = std::atan2(a.y - c.y, a.x - c.x), sm = std::atan2(m.y - c.y, m.x - c.x),
                     se = std::atan2(e.y - c.y, e.x - c.x);
        const auto ccw = [](double from, double to) {
          double v = to - from;
          while (v < 0) v += 2 * pi;
          return v;
        };
        const bool positive = ccw(sa, sm) <= ccw(sa, se);
        add_arc(path, c, r, r, 0, sa, se, positive, false);
        return close_arc(std::move(path), c, close);
      }
      case 15:
      case 16:
      case 20: {
        const Point c = p.point();
        const Point v0 = p.point(), v1 = p.point();
        const double r = std::abs(p.vdc());
        const int close = id == 16 ? p.enumeration({"pie", "chord"}) : -1;
        Path path;
        add_arc(path, c, r, r, 0, std::atan2(v0.y, v0.x), std::atan2(v1.y, v1.x), id != 20, false);
        return close_arc(std::move(path), c, close);
      }
      case 17: {
        const Point c = p.point(), d1 = p.point(), d2 = p.point();
        Path unit;
        add_ellipse(unit, 0, 0, 1, 1);
        const Point u = d1 - c, w = d2 - c;
        return area(transformed(unit, Matrix{u.x, u.y, w.x, w.y, c.x, c.y}));
      }
      case 18:
      case 19: {
        const Point c = p.point(), d1 = p.point(), d2 = p.point();
        const Point v0 = p.point(), v1 = p.point();
        const int close = id == 19 ? p.enumeration({"pie", "chord"}) : -1;
        const Point u = d1 - c, w = d2 - c;
        Path unit;
        add_arc(unit, {0, 0}, 1, 1, 0, frame_angle(v0, u, w), frame_angle(v1, u, w), true, false);
        return close_arc(transformed(unit, Matrix{u.x, u.y, w.x, w.y, c.x, c.y}), c, close);
      }
      case 26: {
        const int continuity = static_cast<int>(p.index());
        const std::vector<Point> pts = points(p);
        Path path;
        if (continuity == 2) {
          if (pts.size() >= 4) {
            path.move_to(pts[0]);
            for (std::size_t i = 1; i + 2 < pts.size(); i += 3) path.cubic_to(pts[i], pts[i + 1], pts[i + 2]);
          }
        } else {
          for (std::size_t i = 0; i + 3 < pts.size(); i += 4) {
            path.move_to(pts[i]);
            path.cubic_to(pts[i + 1], pts[i + 2], pts[i + 3]);
          }
        }
        return line(std::move(path));
      }
      default: return {};
    }
  }

  Result<void> close_arc(Path path, Point center, int close) {
    if (close < 0) return line(std::move(path));
    if (close == 0) path.line_to(center);
    path.close();
    return area(std::move(path));
  }

  Result<void> cell_array(Params& p) {
    const Point P = p.point(), Q = p.point(), R = p.point();
    const std::int64_t nx = p.integer(), ny = p.integer();
    const int local = static_cast<int>(p.integer());
    if (nx <= 0 || ny <= 0 || nx > 1'000'000 || ny > 1'000'000) return {};
    CVT_TRY(ctx_.limits.check_pixels(static_cast<std::uint64_t>(nx), static_cast<std::uint64_t>(ny)));
    pixels_ += static_cast<std::uint64_t>(nx) * static_cast<std::uint64_t>(ny);
    CVT_TRY(ctx_.limits.check_pixels(pixels_, 1));
    int mode = 1;
    if (p.binary()) mode = p.enumeration({"runlength", "packed"});
    const auto w = static_cast<std::uint32_t>(nx), h = static_cast<std::uint32_t>(ny);
    auto frame = std::make_shared<ir::Frame>(ir::Frame::filled(w, h, {0, 0, 0, 0}));
    const int precision = local > 0 ? local : 0;
    const auto next_color = [&]() -> Color {
      if (prec.direct_color) {
        const auto c = p.color_direct(precision);
        const auto comp = [&](std::size_t k) {
          const double lo = prec.color_extent[2 * k], hi = prec.color_extent[2 * k + 1];
          double v = c[k];
          if (p.binary() && precision > 0 && precision != prec.color) {
            v = lo + v / (std::ldexp(1.0, precision) - 1) * (hi - lo);
          }
          return static_cast<float>(std::clamp((v - lo) / (hi - lo != 0 ? hi - lo : 1.0), 0.0, 1.0));
        };
        return {comp(0), comp(1), comp(2), 1};
      }
      return indexed(p.color_index(precision));
    };
    const auto put = [&](std::uint32_t x, std::uint32_t y, Color c) {
      const std::size_t at = frame->offset(x, y);
      frame->rgba[at] = static_cast<std::uint8_t>(std::lround(c.r * 255));
      frame->rgba[at + 1] = static_cast<std::uint8_t>(std::lround(c.g * 255));
      frame->rgba[at + 2] = static_cast<std::uint8_t>(std::lround(c.b * 255));
      frame->rgba[at + 3] = 255;
    };
    for (std::uint32_t y = 0; y < h && p.more(); ++y) {
      if ((y & 63) == 0) CVT_TRY(ctx_.checkpoint(0.5));
      p.align_row();
      if (mode == 0) {
        std::uint32_t x = 0;
        while (x < w && p.more()) {
          const std::int64_t run = p.integer();
          const Color c = next_color();
          if (run <= 0) break;
          for (std::int64_t k = 0; k < run && x < w; ++k) put(x++, y, c);
        }
      } else {
        for (std::uint32_t x = 0; x < w && p.more(); ++x) put(x, y, next_color());
      }
    }
    Picture picture;
    picture.pixels = std::move(frame);
    picture.bounds = Rect::xywh(0, 0, static_cast<double>(w), static_cast<double>(h));
    picture.smooth = false;
    Group g;
    const Point ax = (R - P) * (1.0 / static_cast<double>(w)), ay = (Q - R) * (1.0 / static_cast<double>(h));
    g.transform = Matrix{ax.x, ax.y, ay.x, ay.y, P.x, P.y};
    g.children.push_back(Node{std::move(picture)});
    page_.children.push_back(Node{std::move(g)});
    return {};
  }

  ReadContext& ctx_;
  std::uint64_t elements_ = 0;
  std::uint64_t points_ = 0;
  std::uint64_t pixels_ = 0;
  std::vector<Page> pages_;
  Group page_;
  bool in_picture_ = false;
  bool metric_ = false;
  double metric_scale_ = 1.0;
  Point vdc_min_{0, 0}, vdc_max_{32767, 32767};
  int line_width_mode_ = 1, marker_size_mode_ = 1, edge_width_mode_ = 1;
  std::optional<Color> background_;
  std::map<std::int64_t, Color> table_;
  Attributes attr_;
  Point text_end_{0, 0};
  bool hatch_warned_ = false;
};

// --------------------------------------------------------------------------- binary encoding

Result<Scene> read_binary(ReadContext& ctx) {
  Interpreter interp(ctx);
  util::ByteReader reader(ctx.input);
  Bytes params;
  bool seen_end = false;
  while (reader.remaining() >= 2 && !seen_end) {
    const std::uint16_t head = reader.u16be();
    const int cls = head >> 12, id = (head >> 5) & 0x7F;
    std::size_t len = head & 31;
    params.clear();
    bool more = false;
    if (len == 31) {
      do {
        const std::uint16_t word = reader.u16be();
        more = (word & 0x8000) != 0;
        const std::size_t part = word & 0x7FFF;
        if (reader.remaining() < part) return fail(Status::Decode, "CGM element is truncated");
        CVT_TRY(ctx.limits.check_decompressed(params.size() + part));
        const ByteView data = reader.bytes(part);
        params.insert(params.end(), data.begin(), data.end());
        if (part % 2 == 1) reader.skip(1);
      } while (more && reader.ok() && reader.remaining() >= 2);
    } else {
      if (reader.remaining() < len) return fail(Status::Decode, "CGM element is truncated");
      const ByteView data = reader.bytes(len);
      params.assign(data.begin(), data.end());
      if (len % 2 == 1) reader.skip(1);
    }
    if (!reader.ok()) return fail(Status::Decode, "CGM element is truncated");
    if (cls == 0 && id == 0) continue;  // no-op
    BinaryParams p(interp.prec, params);
    CVT_TRY(interp.element(cls, id, p));
    if (cls == 0 && id == 2) seen_end = true;
  }
  if (!seen_end) return fail(Status::Decode, "the CGM is truncated (no END METAFILE)");
  return interp.finish();
}

// --------------------------------------------------------------------------- clear-text encoding

struct Name {
  std::string_view name;
  int cls;
  int id;
};

constexpr std::array<Name, 79> clear_text_names = {{
    {"BEGMF", 0, 1},        {"ENDMF", 0, 2},          {"BEGPIC", 0, 3},        {"BEGPICBODY", 0, 4},
    {"ENDPIC", 0, 5},       {"MFVERSION", 1, 1},      {"MFDESC", 1, 2},        {"VDCTYPE", 1, 3},
    {"INTEGERPREC", 1, 4},  {"REALPREC", 1, 5},       {"INDEXPREC", 1, 6},     {"COLRPREC", 1, 7},
    {"COLRINDEXPREC", 1, 8}, {"MAXCOLRINDEX", 1, 9},  {"COLRVALUEEXT", 1, 10}, {"MFELEMLIST", 1, 11},
    {"FONTLIST", 1, 13},    {"CHARSETLIST", 1, 14},   {"CHARCODING", 1, 15},   {"SCALEMODE", 2, 1},
    {"COLRMODE", 2, 2},     {"LINEWIDTHMODE", 2, 3},  {"MARKERSIZEMODE", 2, 4}, {"EDGEWIDTHMODE", 2, 5},
    {"VDCEXT", 2, 6},       {"BACKCOLR", 2, 7},       {"VDCINTEGERPREC", 3, 1}, {"VDCREALPREC", 3, 2},
    {"AUXCOLR", 3, 3},      {"TRANSPARENCY", 3, 4},   {"CLIPRECT", 3, 5},      {"CLIP", 3, 6},
    {"LINE", 4, 1},         {"DISJTLINE", 4, 2},      {"MARKER", 4, 3},        {"TEXT", 4, 4},
    {"RESTRTEXT", 4, 5},    {"APNDTEXT", 4, 6},       {"POLYGON", 4, 7},       {"POLYGONSET", 4, 8},
    {"CELLARRAY", 4, 9},    {"GDP", 4, 10},           {"RECT", 4, 11},         {"CIRCLE", 4, 12},
    {"ARC3PT", 4, 13},      {"ARC3PTCLOSE", 4, 14},   {"ARCCTR", 4, 15},       {"ARCCTRCLOSE", 4, 16},
    {"ELLIPSE", 4, 17},     {"ELLIPARC", 4, 18},      {"ELLIPARCCLOSE", 4, 19}, {"ARCCTRREV", 4, 20},
    {"POLYBEZIER", 4, 26},  {"LINEINDEX", 5, 1},      {"LINETYPE", 5, 2},      {"LINEWIDTH", 5, 3},
    {"LINECOLR", 5, 4},     {"MARKERINDEX", 5, 5},    {"MARKERTYPE", 5, 6},    {"MARKERSIZE", 5, 7},
    {"MARKERCOLR", 5, 8},   {"TEXTFONTINDEX", 5, 10}, {"TEXTPREC", 5, 11},     {"CHAREXPAN", 5, 12},
    {"CHARSPACE", 5, 13},   {"TEXTCOLR", 5, 14},      {"CHARHEIGHT", 5, 15},   {"CHARORI", 5, 16},
    {"TEXTPATH", 5, 17},    {"TEXTALIGN", 5, 18},     {"INTSTYLE", 5, 22},     {"FILLCOLR", 5, 23},
    {"EDGETYPE", 5, 27},    {"EDGEWIDTH", 5, 28},     {"EDGECOLR", 5, 29},     {"EDGEVIS", 5, 30},
    {"COLRTABLE", 5, 34},   {"LINECAP", 5, 37},       {"LINEJOIN", 5, 38},
}};

Result<Scene> read_clear_text(ReadContext& ctx) {
  Interpreter interp(ctx);
  const std::string_view text = as_chars(ctx.input);
  std::size_t pos = 0;
  std::vector<Token> tokens;
  std::string name;
  std::uint64_t statements = 0;
  bool ended = false;
  while (pos < text.size()) {
    // One statement: name, parameters, terminator ';' or '/'.
    tokens.clear();
    name.clear();
    bool have_name = false;
    while (pos < text.size()) {
      const char c = text[pos];
      if (c == ';' || c == '/') {
        ++pos;
        break;
      }
      if (c == '%') {  // comment
        const std::size_t end = text.find('%', pos + 1);
        pos = end == std::string_view::npos ? text.size() : end + 1;
        continue;
      }
      if (c == '"' || c == '\'') {
        std::string s;
        ++pos;
        while (pos < text.size()) {
          if (text[pos] == c) {
            if (pos + 1 < text.size() && text[pos + 1] == c) {
              s.push_back(c);
              pos += 2;
              continue;
            }
            ++pos;
            break;
          }
          s.push_back(text[pos++]);
          if (s.size() > 1'000'000) break;
        }
        tokens.push_back({Token::Kind::String, 0, std::move(s)});
        continue;
      }
      if (util::is_ascii_digit(c) || c == '-' || c == '+' || c == '.') {
        const std::size_t start = pos++;
        while (pos < text.size() && (util::is_ascii_digit(text[pos]) || text[pos] == '.' || text[pos] == 'E' ||
                                     text[pos] == 'e' ||
                                     ((text[pos] == '-' || text[pos] == '+') && (text[pos - 1] == 'E' || text[pos - 1] == 'e')))) {
          ++pos;
        }
        const auto v = util::parse_double(text.substr(start, pos - start));
        if (tokens.size() < 20'000'000) tokens.push_back({Token::Kind::Number, v && std::isfinite(*v) ? *v : 0.0, {}});
        continue;
      }
      if (util::is_ascii_alpha(c) || c == '_') {
        const std::size_t start = pos;
        while (pos < text.size() && (util::is_ascii_alpha(text[pos]) || util::is_ascii_digit(text[pos]) ||
                                     text[pos] == '_' || text[pos] == '$')) {
          ++pos;
        }
        std::string word(text.substr(start, pos - start));
        if (!have_name) {
          name = util::to_upper(word);
          std::erase(name, '_');
          std::erase(name, '$');
          have_name = true;
        } else {
          tokens.push_back({Token::Kind::Word, 0, std::move(word)});
        }
        continue;
      }
      ++pos;  // whitespace, commas, parentheses
    }
    if (!have_name) continue;
    if ((++statements & 0xFFF) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(text.size())));
    for (const Name& n : clear_text_names) {
      if (n.name == name) {
        TextParams p(interp.prec, tokens);
        CVT_TRY(interp.element(n.cls, n.id, p));
        break;
      }
    }
    if (name == "ENDMF") {
      ended = true;
      break;
    }
  }
  if (!ended) return fail(Status::Decode, "the CGM is truncated (no ENDMF)");
  return interp.finish();
}

bool clear_text_start(std::span<const std::byte> data) {
  std::string_view t = as_chars(data.first(std::min<std::size_t>(data.size(), 256)));
  while (!t.empty() && util::is_ascii_space(t.front())) t.remove_prefix(1);
  while (t.starts_with("%")) {
    const std::size_t end = t.find('%', 1);
    if (end == std::string_view::npos) return false;
    t.remove_prefix(end + 1);
    while (!t.empty() && util::is_ascii_space(t.front())) t.remove_prefix(1);
  }
  return util::istarts_with(t, "BEGMF") || util::istarts_with(t, "BEG_MF");
}

bool binary_start(std::span<const std::byte> data) {
  if (data.size() < 4) return false;
  const auto head = static_cast<std::uint16_t>((static_cast<unsigned>(data[0]) << 8) | static_cast<unsigned>(data[1]));
  if ((head & 0xFFE0) != 0x0020) return false;  // class 0, element 1: BEGIN METAFILE
  const std::size_t len = head & 31;
  if (len == 31) return true;
  if (len == 0) return true;
  const auto slen = static_cast<std::size_t>(static_cast<unsigned>(data[2]));
  return slen + 1 <= len || slen == 255;
}

Result<Scene> read_cgm(ReadContext& ctx) {
  if (clear_text_start(ctx.input)) return read_clear_text(ctx);
  if (binary_start(ctx.input)) return read_binary(ctx);
  return fail(Status::Decode, "this is not a CGM file (no BEGIN METAFILE)");
}

int sniff_cgm(std::span<const std::byte> data, std::string_view /*filename*/) {
  if (clear_text_start(data)) return 95;
  if (binary_start(data)) return 85;
  return 0;
}

}  // namespace

void register_cgm(Registry& registry) {
  VectorFormat format;
  format.info.id = "cgm";
  format.info.name = "Computer Graphics Metafile";
  format.info.description = "ISO vector metafile used in technical documentation (binary and clear-text encodings).";
  format.info.category = Category::Vector;
  format.info.extensions = {"cgm"};
  format.info.mime = "image/cgm";
  format.info.popularity = 15;
  format.info.sniff = sniff_cgm;
  format.read = read_cgm;
  format.lossy_read = true;
  register_vector_format(registry, std::move(format));
}

}  // namespace cvt::vector::cad
