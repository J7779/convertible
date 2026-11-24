// Convertible core - vector family: Apple QuickDraw PICT (read only).
//
// Version 1 and 2 pictures, with or without the 512-byte file header:
// lines, rectangles, rounded rectangles, ovals, arcs/wedges, polygons and
// regions (by their bounding box) in their frame/paint/erase/fill variants,
// pen size and patterns (approximated by the pattern's average colour),
// RGB foreground/background colours, text (Long/DH/DV/DHDV text with the
// bundled fallback fonts), clip regions (bounding box), bitmaps and pixmaps
// (BitsRect, PackBitsRect 1-8 bit, DirectBitsRect 16/32 bit) as pictures
// and QuickTime JPEG pictures via the jpg codec. Comments are skipped.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/image_api.hpp"
#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/misc_common.hpp"
#include "families/vector/vector_codecs.hpp"
#include "util/bytes.hpp"

namespace cvt::vector::misc {

namespace {

constexpr std::size_t max_ops = 4'000'000;

// --------------------------------------------------------------------------- detection

std::uint16_t be16(std::span<const std::byte> d, std::size_t at) {
  return at + 2 <= d.size() ? static_cast<std::uint16_t>(std::to_integer<unsigned>(d[at]) << 8 | std::to_integer<unsigned>(d[at + 1]))
                            : 0xFFFF;
}

/// Offset of the picture (0 or 512) and its version, if the data looks like a PICT.
std::optional<std::pair<std::size_t, int>> locate(std::span<const std::byte> d) {
  for (const std::size_t base : {std::size_t{512}, std::size_t{0}}) {
    if (d.size() < base + 12) continue;
    const auto top = static_cast<std::int16_t>(be16(d, base + 2)), left = static_cast<std::int16_t>(be16(d, base + 4));
    const auto bottom = static_cast<std::int16_t>(be16(d, base + 6)), right = static_cast<std::int16_t>(be16(d, base + 8));
    if (bottom <= top || right <= left) continue;
    if (be16(d, base + 10) == 0x0011 && be16(d, base + 12) == 0x02FF) return std::pair{base, 2};
    if (be16(d, base + 10) == 0x1101) return std::pair{base, 1};
  }
  return std::nullopt;
}

int sniff_pict(std::span<const std::byte> data, std::string_view filename) {
  const auto found = locate(data);
  if (!found) return 0;
  if (found->second == 2) return be16(data, found->first + 14) == 0x0C00 ? 90 : 75;
  // Version 1 has only a two-byte marker: trust it more with the right extension.
  const std::string_view name = filename;
  const bool ext = name.ends_with(".pct") || name.ends_with(".pict") || name.ends_with(".pic") ||
                   name.ends_with(".PCT") || name.ends_with(".PICT");
  return ext ? 80 : 30;
}

// --------------------------------------------------------------------------- helpers

constexpr std::array<char16_t, 128> mac_roman = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7,
    0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5,
    0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9,
    0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248,
    0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
    0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7,
    0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD,
    0x02DB, 0x02C7};

std::string mac_roman_to_utf8(ByteView bytes) {
  std::string out;
  for (const std::byte b : bytes) {
    const auto c = std::to_integer<unsigned>(b);
    char32_t cp = c < 0x80 ? c : mac_roman[c - 0x80];
    if (cp < 0x20) continue;
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

using Pattern = std::array<std::uint8_t, 8>;

double density(const Pattern& p) {
  int bits = 0;
  for (const std::uint8_t b : p) bits += std::popcount(static_cast<unsigned>(b));
  return bits / 64.0;
}

Color mix(Color fg, Color bk, double d) {
  const auto f = static_cast<float>(d);
  return Color{fg.r * f + bk.r * (1 - f), fg.g * f + bk.g * (1 - f), fg.b * f + bk.b * (1 - f), 1.0f};
}

/// Old-style QuickDraw colour constants (FgColor / BkColor).
Color old_color(std::uint32_t value) {
  switch (value) {
    case 33: return Color{0, 0, 0, 1};
    case 30: return Color{1, 1, 1, 1};
    case 205: return Color{1, 0, 0, 1};
    case 341: return Color{0, 1, 0, 1};
    case 409: return Color{0, 0, 1, 1};
    case 273: return Color{0, 1, 1, 1};
    case 137: return Color{1, 0, 1, 1};
    case 69: return Color{1, 1, 0, 1};
    default: return Color{0, 0, 0, 1};
  }
}

/// PackBits (bytes, or 16-bit words when `word`); false when the data is malformed.
bool unpack_bits(ByteView in, std::vector<std::uint8_t>& out, std::size_t expected, bool word) {
  out.clear();
  std::size_t i = 0;
  const std::size_t unit = word ? 2 : 1;
  while (i < in.size() && out.size() < expected) {
    const auto n = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(in[i++]));
    if (n >= 0) {
      const std::size_t count = (static_cast<std::size_t>(n) + 1) * unit;
      if (i + count > in.size()) return false;
      for (std::size_t k = 0; k < count; ++k) out.push_back(std::to_integer<std::uint8_t>(in[i + k]));
      i += count;
    } else if (n != -128) {
      const std::size_t count = static_cast<std::size_t>(1 - n);
      if (i + unit > in.size()) return false;
      for (std::size_t k = 0; k < count; ++k) {
        for (std::size_t u = 0; u < unit; ++u) out.push_back(std::to_integer<std::uint8_t>(in[i + u]));
      }
      i += unit;
    }
  }
  if (out.size() < expected) return false;
  out.resize(expected);
  return true;
}

// --------------------------------------------------------------------------- the reader

class PictReader {
 public:
  explicit PictReader(ReadContext& ctx) : ctx_(ctx) {}

  Result<Scene> read() {
    const auto found = locate(ctx_.input);
    if (!found) return fail(Status::Decode, "not a PICT picture");
    base_ = found->first;
    version_ = found->second;
    r_ = util::ByteReader(ctx_.input);
    r_.seek(base_ + 2);
    const Rect frame = read_rect();
    Scene scene;
    scene.source_units = "pt";
    Page page;
    page.width = frame.width() * px_per_pt;
    page.height = frame.height() * px_per_pt;
    if (!(page.width > 0) || !(page.height > 0)) return fail(Status::Decode, "the PICT frame is empty");
    const Matrix to_page = Matrix::scale(px_per_pt, px_per_pt) * Matrix::translate(-frame.x0, -frame.y0);
    r_.seek(base_ + 10);
    if (version_ == 2) {
      r_.skip(4);  // 0011 02FF
    } else {
      r_.skip(2);  // 11 01
    }
    current_ = &page.root;
    std::size_t ops = 0;
    bool ended = false;
    while (!ended) {
      if (++ops > max_ops) return fail(Status::Limit, "the PICT picture has too many opcodes");
      if ((ops & 1023) == 0) {
        CVT_TRY(ctx_.checkpoint(static_cast<double>(r_.position()) / static_cast<double>(r_.size()), "Reading PICT"));
      }
      if (version_ == 2 && ((r_.position() - base_) & 1) != 0) r_.skip(1);
      if (r_.remaining() < (version_ == 2 ? 2u : 1u)) {
        ctx_.warn("The PICT picture is truncated; the part that was read is shown.");
        break;
      }
      const std::uint16_t op = version_ == 2 ? r_.u16be() : r_.u8();
      auto handled = opcode(op, ended, page);
      if (!handled) return fail(handled.error());
      if (!r_.ok()) {
        ctx_.warn("The PICT picture is truncated; the part that was read is shown.");
        break;
      }
    }
    page.root.transform = to_page;
    scene.pages.push_back(std::move(page));
    return scene;
  }

 private:
  ReadContext& ctx_;
  util::ByteReader r_;
  std::size_t base_ = 0;
  int version_ = 2;
  Group* current_ = nullptr;

  // Graphics state.
  Point pen_{0, 0};
  Point pen_size_{1, 1};
  Point origin_{0, 0};
  Color fg_{0, 0, 0, 1}, bk_{1, 1, 1, 1};
  Pattern pen_pat_{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  Pattern fill_pat_{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  Pattern bk_pat_{};
  Point oval_size_{0, 0};
  Rect last_rect_, last_rrect_, last_oval_, last_arc_rect_, last_region_;
  Path last_poly_;
  std::int16_t font_id_ = 0;
  std::string font_name_;
  std::map<std::int16_t, std::string> font_names_;
  double text_size_ = 12;
  std::uint8_t text_face_ = 0;
  Point text_loc_{0, 0};
  bool warned_invert_ = false, warned_region_ = false, warned_pattern_ = false, warned_text_ = false,
       warned_pixels_ = false;

  void warn_once(bool& flag, std::string_view message) {
    if (flag) return;
    flag = true;
    ctx_.warn(std::string(message));
  }

  Point read_point() {
    const double v = r_.i16be(), h = r_.i16be();
    return Point{h, v} - origin_;
  }
  Rect read_rect() {
    const double top = r_.i16be(), left = r_.i16be(), bottom = r_.i16be(), right = r_.i16be();
    Rect out{left - origin_.x, top - origin_.y, right - origin_.x, bottom - origin_.y};
    if (out.x1 < out.x0) std::swap(out.x0, out.x1);
    if (out.y1 < out.y0) std::swap(out.y0, out.y1);
    return out;
  }
  Pattern read_pattern() {
    Pattern p{};
    for (std::uint8_t& b : p) b = r_.u8();
    return p;
  }
  Color read_rgb() {
    const double r = r_.u16be() / 65535.0, g = r_.u16be() / 65535.0, b = r_.u16be() / 65535.0;
    return Color{static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), 1.0f};
  }
  Rect read_region() {
    const std::uint16_t size = r_.u16be();
    const Rect box = read_rect();
    if (size > 10) {
      warn_once(warned_region_, "Complex QuickDraw regions were approximated by their bounding boxes.");
      r_.skip(size - 10u);
    }
    return box;
  }
  Path read_poly() {
    const std::uint16_t size = r_.u16be();
    (void)read_rect();
    Path path;
    if (size < 10) return path;
    const std::size_t count = (size - 10u) / 4u;
    for (std::size_t k = 0; k < count && r_.ok(); ++k) {
      const Point p = read_point();
      if (k == 0) path.move_to(p);
      else path.line_to(p);
    }
    if (((size - 10u) % 4u) != 0) r_.skip((size - 10u) % 4u);
    return path;
  }

  Paint paint_for(int verb) {
    // verb: 0 frame, 1 paint, 2 erase, 3 invert, 4 fill
    const Pattern& pattern = verb == 2 ? bk_pat_ : verb == 4 ? fill_pat_ : pen_pat_;
    double d = density(pattern);
    if (verb == 2) return Paint::solid(mix(fg_, bk_, d));
    if (d != 0.0 && d != 1.0) warn_once(warned_pattern_, "QuickDraw patterns were approximated by their average colour.");
    return Paint::solid(mix(fg_, bk_, d));
  }

  void add_shape(Path path, int verb, FillRule rule = FillRule::NonZero) {
    if (path.empty()) return;
    if (verb == 3) {
      warn_once(warned_invert_, "QuickDraw invert operations cannot be represented and were skipped.");
      return;
    }
    Shape shape;
    shape.path = std::move(path);
    shape.fill = paint_for(verb);
    shape.fill_rule = rule;
    current_->children.push_back(Node{std::move(shape)});
  }

  /// Frame: the pen is dragged inside the outline. Rectangles are exact; curves use a stroke.
  void frame_path(const Path& outline, const Rect& box, bool exact_rect) {
    const double pw = pen_size_.x, ph = pen_size_.y;
    if (pw <= 0 || ph <= 0) return;
    if (exact_rect) {
      Path ring;
      add_rect(ring, box.x0, box.y0, box.width(), box.height());
      if (box.width() > 2 * pw && box.height() > 2 * ph) add_rect(ring, box.x0 + pw, box.y0 + ph, box.width() - 2 * pw, box.height() - 2 * ph);
      add_shape(std::move(ring), 0, FillRule::EvenOdd);
      return;
    }
    Shape shape;
    // Inset the outline by half the pen so that the stroke stays inside.
    const double sx = box.width() > pw ? (box.width() - pw) / box.width() : 1.0;
    const double sy = box.height() > ph ? (box.height() - ph) / box.height() : 1.0;
    const Point c{(box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2};
    shape.path = transformed(outline, Matrix::translate(c.x, c.y) * Matrix::scale(sx, sy) * Matrix::translate(-c.x, -c.y));
    shape.stroke.paint = paint_for(0);
    shape.stroke.width = (pw + ph) / 2;
    shape.stroke.join = LineJoin::Miter;
    current_->children.push_back(Node{std::move(shape)});
  }

  /// A QuickDraw line: the pen rectangle swept from `a` to `b`.
  void line(Point a, Point b) {
    const double pw = pen_size_.x, ph = pen_size_.y;
    pen_ = b;
    if (pw <= 0 || ph <= 0) return;
    std::array<Point, 8> pts = {a, a + Point{pw, 0}, a + Point{pw, ph}, a + Point{0, ph},
                                b, b + Point{pw, 0}, b + Point{pw, ph}, b + Point{0, ph}};
    // Convex hull (monotone chain) of the two pen rectangles.
    std::sort(pts.begin(), pts.end(), [](Point p, Point q) { return p.x < q.x || (p.x == q.x && p.y < q.y); });
    std::vector<Point> hull;
    for (int pass = 0; pass < 2; ++pass) {
      const std::size_t start = hull.size();
      for (std::size_t k = 0; k < pts.size(); ++k) {
        const Point p = pass == 0 ? pts[k] : pts[pts.size() - 1 - k];
        while (hull.size() >= start + 2 && cross(hull[hull.size() - 1] - hull[hull.size() - 2], p - hull[hull.size() - 2]) <= 0) {
          hull.pop_back();
        }
        hull.push_back(p);
      }
      hull.pop_back();
    }
    Path path;
    for (std::size_t k = 0; k < hull.size(); ++k) {
      if (k == 0) path.move_to(hull[k]);
      else path.line_to(hull[k]);
    }
    path.close();
    add_shape(std::move(path), 1);
  }

  void arc_shape(const Rect& box, double start, double extent, int verb) {
    const Point c{(box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2};
    const double rx = box.width() / 2, ry = box.height() / 2;
    // QuickDraw: 0 degrees at 12 o'clock, clockwise.
    const double a0 = (start - 90.0) * std::numbers::pi / 180.0;
    const double a1 = (start + extent - 90.0) * std::numbers::pi / 180.0;
    Path path;
    if (verb == 0) {
      add_arc(path, c, rx, ry, 0, a0, a1, extent >= 0, false);
      Shape shape;
      shape.path = std::move(path);
      shape.stroke.paint = paint_for(0);
      shape.stroke.width = (pen_size_.x + pen_size_.y) / 2;
      if (shape.stroke.width > 0) current_->children.push_back(Node{std::move(shape)});
      return;
    }
    path.move_to(c);
    add_arc(path, c, rx, ry, 0, a0, a1, extent >= 0, true);
    path.close();
    add_shape(std::move(path), verb);
  }

  void text(std::string_view raw) {
    const std::string utf8 = mac_roman_to_utf8(as_bytes(raw));
    if (utf8.empty()) return;
    warn_once(warned_text_, "PICT text was set with the bundled fallback fonts.");
    FontSpec font;
    font.size = std::clamp(text_size_ > 0 ? text_size_ : 12.0, 1.0, 2000.0);
    font.weight = (text_face_ & 1) != 0 ? 700 : 400;
    font.italic = (text_face_ & 2) != 0;
    std::string family = font_name_;
    if (family.empty()) {
      switch (font_id_) {
        case 4: case 22: family = "monospace"; break;
        case 2: case 20: family = "serif"; break;
        default: family = "sans-serif"; break;
      }
    }
    font.family = family;
    current_->children.push_back(Node{make_text(utf8, font, text_loc_, Paint::solid(fg_))});
  }

  std::string pascal_text() {
    const std::uint8_t count = r_.u8();
    const ByteView bytes = r_.bytes(count);
    return std::string(as_chars(bytes));
  }

  void set_clip(const Rect& box, Page& page) {
    Group group;
    auto clip = std::make_shared<ClipPath>();
    ClipPath::Item item;
    add_rect(item.path, box.x0, box.y0, box.width(), box.height());
    clip->items.push_back(std::move(item));
    group.clip = std::move(clip);
    page.root.children.push_back(Node{std::move(group)});
    current_ = &std::get<Group>(page.root.children.back().value);
  }

  // ---- pixel data

  Result<void> pixels(std::uint16_t op) {
    const bool direct = op == 0x9A || op == 0x9B;
    const bool region = op == 0x91 || op == 0x99 || op == 0x9B;
    const bool packed = op != 0x90 && op != 0x91;
    if (direct) r_.skip(4);  // baseAddr
    std::uint16_t row_bytes = r_.u16be();
    const bool pixmap = (row_bytes & 0x8000) != 0 || direct;
    row_bytes &= 0x3FFF;
    const Rect bounds = read_rect();
    int pixel_size = 1, pack_type = 0, component_count = 1;
    std::vector<Color> table;
    if (pixmap) {
      r_.skip(2);  // pmVersion
      pack_type = r_.u16be();
      r_.skip(4 + 4 + 4 + 2);  // packSize, hRes, vRes, pixelType
      pixel_size = r_.u16be();
      component_count = r_.u16be();
      r_.skip(2 + 4 + 4 + 4);  // cmpSize, planeBytes, pmTable, pmReserved
      if (!direct) {
        r_.skip(4 + 2);  // ctSeed, ctFlags
        const std::uint16_t size = r_.u16be();
        const std::size_t entries = std::min<std::size_t>(std::size_t{size} + 1, 256);
        table.assign(256, Color{0, 0, 0, 1});
        for (std::size_t k = 0; k < entries && r_.ok(); ++k) {
          const std::uint16_t index = r_.u16be();
          const Color c = read_rgb();
          table[(size == 0xFFFF ? k : index) & 0xFF] = c;  // flags bit: index = position
          if (k >= 256) break;
        }
        if (size >= 256) r_.skip((std::size_t{size} + 1 - 256) * 8);
      }
    } else {
      table = {Color{1, 1, 1, 1}, Color{0, 0, 0, 1}};
    }
    (void)read_rect();  // srcRect
    const Rect dst = read_rect();
    r_.skip(2);  // mode
    if (region) (void)read_region();
    if (!r_.ok()) return {};

    const double w = bounds.width(), h = bounds.height();
    if (!(w > 0) || !(h > 0)) return {};
    const auto width = static_cast<std::uint32_t>(w), height = static_cast<std::uint32_t>(h);
    CVT_TRY(ctx_.limits.check_pixels(width, height));
    if (pixel_size != 1 && pixel_size != 2 && pixel_size != 4 && pixel_size != 8 && pixel_size != 16 && pixel_size != 32) {
      return fail(Status::Decode, "unsupported PICT pixel size {}", pixel_size);
    }
    if (pack_type == 0) pack_type = pixel_size == 16 ? 3 : pixel_size == 32 ? 4 : 0;
    // Bytes of one unpacked row.
    std::size_t row = row_bytes;
    if (pixel_size == 32 && pack_type == 4) row = std::size_t{width} * static_cast<std::size_t>(std::max(component_count, 3));
    if (pixel_size == 32 && pack_type == 2) row = std::size_t{width} * 3;
    if (row == 0 || row > 4u * 65536u) return fail(Status::Decode, "bad PICT row size");
    const std::uint64_t min_row = (std::uint64_t{width} * static_cast<std::uint64_t>(pixel_size) + 7) / 8;
    if (pixel_size <= 16 && row < min_row) return fail(Status::Decode, "PICT row is shorter than the picture");

    ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
    std::vector<std::uint8_t> line;
    for (std::uint32_t y = 0; y < height; ++y) {
      if ((y & 63) == 0) CVT_TRY(ctx_.checkpoint(0.5, "Reading PICT pixels"));
      if (!packed || row_bytes < 8 || pack_type == 1 || (pixel_size == 32 && pack_type == 2)) {
        const ByteView raw = r_.bytes(row);
        if (!r_.ok()) return fail(Status::Decode, "PICT pixel data is truncated");
        line.assign(reinterpret_cast<const std::uint8_t*>(raw.data()), reinterpret_cast<const std::uint8_t*>(raw.data()) + raw.size());
      } else {
        const std::size_t count = row_bytes > 250 ? r_.u16be() : r_.u8();
        const ByteView raw = r_.bytes(count);
        if (!r_.ok()) return fail(Status::Decode, "PICT pixel data is truncated");
        if (!unpack_bits(raw, line, row, pixel_size == 16)) return fail(Status::Decode, "malformed PackBits data in a PICT picture");
      }
      for (std::uint32_t x = 0; x < width; ++x) {
        std::array<std::uint8_t, 4> px{0, 0, 0, 255};
        if (pixel_size <= 8) {
          const std::size_t bit = std::size_t{x} * static_cast<std::size_t>(pixel_size);
          const unsigned byte = line[bit / 8];
          const unsigned index = (byte >> (8 - pixel_size - static_cast<int>(bit % 8))) & ((1u << pixel_size) - 1u);
          const Color c = index < table.size() ? table[index] : Color{0, 0, 0, 1};
          px = {static_cast<std::uint8_t>(std::lround(c.r * 255)), static_cast<std::uint8_t>(std::lround(c.g * 255)),
                static_cast<std::uint8_t>(std::lround(c.b * 255)), 255};
        } else if (pixel_size == 16) {
          const unsigned v = static_cast<unsigned>(line[x * 2]) << 8 | line[x * 2 + 1];
          const auto five = [](unsigned c) { return static_cast<std::uint8_t>((c << 3) | (c >> 2)); };
          px = {five((v >> 10) & 31), five((v >> 5) & 31), five(v & 31), 255};
        } else if (pack_type == 4) {
          const std::size_t planes = static_cast<std::size_t>(std::max(component_count, 3));
          const std::size_t first = planes == 4 ? 1 : 0;
          px = {line[(first + 0) * width + x], line[(first + 1) * width + x], line[(first + 2) * width + x], 255};
        } else if (pack_type == 2) {
          px = {line[x * 3], line[x * 3 + 1], line[x * 3 + 2], 255};
        } else {
          if (std::size_t{x} * 4 + 3 >= line.size()) break;
          px = {line[x * 4 + 1], line[x * 4 + 2], line[x * 4 + 3], 255};
        }
        const std::size_t at = frame.offset(x, y);
        for (std::size_t k = 0; k < 4; ++k) frame.rgba[at + k] = px[k];
      }
    }
    if ((r_.position() - base_) & 1) r_.skip(1);
    Picture picture;
    picture.pixels = std::make_shared<const ir::Frame>(std::move(frame));
    picture.bounds = dst;
    picture.smooth = false;
    current_->children.push_back(Node{std::move(picture)});
    return {};
  }

  Result<void> quicktime(std::uint32_t length) {
    const ByteView data = r_.bytes(length);
    if (!r_.ok()) return {};
    util::ByteReader q(data);
    q.skip(2);  // version
    std::array<double, 9> m{};
    for (double& v : m) v = q.i32be() / 65536.0;
    const std::uint32_t matte_size = q.u32be();
    q.skip(8);  // matte rect
    q.skip(2);  // mode
    const double top = q.i16be(), left = q.i16be(), bottom = q.i16be(), right = q.i16be();
    q.skip(4);  // accuracy
    const std::uint32_t mask_size = q.u32be();
    if (matte_size > 0) {
      const std::uint32_t idsc = q.u32be();
      if (idsc < 4) return {};
      q.skip(idsc - 4u);
      q.skip(matte_size);
    }
    q.skip(mask_size);
    const std::size_t desc_start = q.position();
    const std::uint32_t desc_size = q.u32be();
    const std::uint32_t codec = q.u32be();
    q.seek(desc_start + 44);
    const std::uint32_t data_size = q.u32be();
    if (!q.ok() || desc_size < 86 || !q.seek(desc_start + desc_size)) return {};
    if (codec != 0x6A706567u) {  // 'jpeg'
      warn_once(warned_pixels_, "A QuickTime-compressed PICT picture uses a codec other than JPEG and was skipped.");
      return {};
    }
    const std::size_t available = q.remaining();
    const ByteView jpeg = data.subspan(q.position(), std::min<std::size_t>(data_size == 0 ? available : data_size, available));
    auto decoded = image::decode("jpg", jpeg, ctx_);
    if (!decoded || decoded->frames.empty()) {
      if (!decoded && (decoded.error().code == Status::Limit || decoded.error().code == Status::Cancelled)) {
        return fail(decoded.error());
      }
      warn_once(warned_pixels_, "A JPEG picture inside the PICT could not be decoded and was skipped.");
      return {};
    }
    Picture picture;
    const Matrix mm{m[0], m[1], m[3], m[4], m[6], m[7]};
    picture.bounds = transform_rect(Rect{left - origin_.x, top - origin_.y, right - origin_.x, bottom - origin_.y}, mm);
    picture.pixels = std::make_shared<const ir::Frame>(std::move(decoded->frames.front()));
    picture.encoded = Bytes(jpeg.begin(), jpeg.end());
    picture.encoded_format = "jpg";
    current_->children.push_back(Node{std::move(picture)});
    return {};
  }

  Result<void> opcode(std::uint16_t op, bool& ended, Page& page) {
    // Shape opcodes 0x30-0x8F: kind by high nibble, verb by low bits, "same" variants with bit 3.
    if (op >= 0x30 && op <= 0x8F) {
      const int kind = (op >> 4) & 0xF;  // 3 rect, 4 rrect, 5 oval, 6 arc, 7 poly, 8 region
      const int verb = op & 0x7;
      const bool same = (op & 0x8) != 0;
      switch (kind) {
        case 3: {
          if (!same) last_rect_ = read_rect();
          if (verb > 4) return {};
          Path p;
          add_rect(p, last_rect_.x0, last_rect_.y0, last_rect_.width(), last_rect_.height());
          if (verb == 0) frame_path(p, last_rect_, true);
          else add_shape(std::move(p), verb);
          return {};
        }
        case 4: {
          if (!same) last_rrect_ = read_rect();
          if (verb > 4) return {};
          Path p;
          add_rect(p, last_rrect_.x0, last_rrect_.y0, last_rrect_.width(), last_rrect_.height(), oval_size_.x / 2,
                   oval_size_.y / 2);
          if (verb == 0) frame_path(p, last_rrect_, false);
          else add_shape(std::move(p), verb);
          return {};
        }
        case 5: {
          if (!same) last_oval_ = read_rect();
          if (verb > 4) return {};
          Path p;
          add_ellipse(p, (last_oval_.x0 + last_oval_.x1) / 2, (last_oval_.y0 + last_oval_.y1) / 2, last_oval_.width() / 2,
                      last_oval_.height() / 2);
          if (verb == 0) frame_path(p, last_oval_, false);
          else add_shape(std::move(p), verb);
          return {};
        }
        case 6: {
          if (!same) last_arc_rect_ = read_rect();
          const double start = r_.i16be(), extent = r_.i16be();
          if (verb > 4) return {};
          arc_shape(last_arc_rect_, start, extent, verb);
          return {};
        }
        case 7: {
          if (!same) last_poly_ = read_poly();
          if (verb > 4) return {};
          if (verb == 0) {
            Shape shape;
            shape.path = transformed(last_poly_, Matrix::translate(pen_size_.x / 2, pen_size_.y / 2));
            shape.stroke.paint = paint_for(0);
            shape.stroke.width = (pen_size_.x + pen_size_.y) / 2;
            if (!shape.path.empty() && shape.stroke.width > 0) current_->children.push_back(Node{std::move(shape)});
          } else {
            Path closed = last_poly_;
            if (!closed.empty()) closed.close();
            add_shape(std::move(closed), verb);
          }
          return {};
        }
        case 8: {
          if (!same) last_region_ = read_region();
          if (verb > 4) return {};
          Path p;
          add_rect(p, last_region_.x0, last_region_.y0, last_region_.width(), last_region_.height());
          if (verb == 0) frame_path(p, last_region_, true);
          else add_shape(std::move(p), verb);
          return {};
        }
        default:
          return {};
      }
    }
    switch (op) {
      case 0x00: case 0x17: case 0x18: case 0x19: case 0x1C: case 0x1E: return {};
      case 0x01: set_clip(read_region(), page); return {};
      case 0x02: bk_pat_ = read_pattern(); return {};
      case 0x03: {
        font_id_ = r_.i16be();
        const auto it = font_names_.find(font_id_);
        font_name_ = it != font_names_.end() ? it->second : std::string();
        return {};
      }
      case 0x04: text_face_ = r_.u8(); return {};
      case 0x05: case 0x08: case 0x15: case 0x16: r_.skip(2); return {};
      case 0x06: r_.skip(4); return {};
      case 0x07: {
        const double v = r_.i16be(), h = r_.i16be();
        pen_size_ = {std::max(0.0, h), std::max(0.0, v)};
        return {};
      }
      case 0x09: pen_pat_ = read_pattern(); return {};
      case 0x0A: fill_pat_ = read_pattern(); return {};
      case 0x0B: {
        const double v = r_.i16be(), h = r_.i16be();
        oval_size_ = {std::abs(h), std::abs(v)};
        return {};
      }
      case 0x0C: {
        const double dh = r_.i16be(), dv = r_.i16be();
        origin_ = origin_ + Point{dh, dv};
        return {};
      }
      case 0x0D: text_size_ = r_.i16be(); return {};
      case 0x0E: fg_ = old_color(r_.u32be()); return {};
      case 0x0F: bk_ = old_color(r_.u32be()); return {};
      case 0x10: r_.skip(8); return {};
      case 0x11: r_.skip(1); return {};
      case 0x12: case 0x13: case 0x14: {
        // PixPat: type, pattern, and for type 1 a pixmap + colour table + data (approximated by the old pattern).
        const std::uint16_t type = r_.u16be();
        const Pattern pat = read_pattern();
        if (op == 0x12) bk_pat_ = pat;
        if (op == 0x13) pen_pat_ = pat;
        if (op == 0x14) fill_pat_ = pat;
        if (type == 1) {
          // Skip the pixmap, colour table and pixel data.
          std::uint16_t row_bytes = r_.u16be() & 0x3FFF;
          const Rect b = read_rect();
          r_.skip(2 + 2 + 4 + 4 + 4 + 2);
          const int pixel_size = r_.u16be();
          r_.skip(2 + 2 + 4 + 4 + 4);
          r_.skip(4 + 2);
          const std::uint16_t size = r_.u16be();
          r_.skip((std::size_t{size} + 1) * 8);
          const auto height = static_cast<std::size_t>(std::max(0.0, b.height()));
          (void)pixel_size;
          for (std::size_t y = 0; y < height && r_.ok(); ++y) {
            if (row_bytes < 8) {
              r_.skip(row_bytes);
            } else {
              r_.skip(row_bytes > 250 ? r_.u16be() : r_.u8());
            }
          }
        } else if (type == 2) {
          r_.skip(6);  // RGB colour
        }
        return {};
      }
      case 0x1A: fg_ = read_rgb(); return {};
      case 0x1B: bk_ = read_rgb(); return {};
      case 0x1D: case 0x1F: r_.skip(6); return {};
      case 0x20: {
        const Point a = read_point(), b = read_point();
        line(a, b);
        return {};
      }
      case 0x21: line(pen_, read_point()); return {};
      case 0x22: {
        const Point a = read_point();
        const double dh = r_.i8(), dv = r_.i8();
        line(a, a + Point{dh, dv});
        return {};
      }
      case 0x23: {
        const double dh = r_.i8(), dv = r_.i8();
        line(pen_, pen_ + Point{dh, dv});
        return {};
      }
      case 0x28: text_loc_ = read_point(); text(pascal_text()); return {};
      case 0x29: text_loc_.x += r_.u8(); text(pascal_text()); return {};
      case 0x2A: text_loc_.y += r_.u8(); text(pascal_text()); return {};
      case 0x2B: {
        text_loc_.x += r_.u8();
        text_loc_.y += r_.u8();
        text(pascal_text());
        return {};
      }
      case 0x2C: {
        const std::uint16_t length = r_.u16be();
        const std::size_t end = r_.position() + length;
        font_id_ = r_.i16be();
        font_name_ = mac_roman_to_utf8(as_bytes(pascal_text()));
        if (font_names_.size() < 1024) font_names_[font_id_] = font_name_;
        r_.seek(end);
        return {};
      }
      case 0x24: case 0x25: case 0x26: case 0x27: case 0x2D: case 0x2E: case 0x2F:
        r_.skip(r_.u16be());
        return {};
      case 0x90: case 0x91: case 0x98: case 0x99: case 0x9A: case 0x9B:
        return pixels(op);
      case 0xA0: r_.skip(2); return {};
      case 0xA1: {
        r_.skip(2);
        r_.skip(r_.u16be());
        return {};
      }
      case 0xFF: ended = true; return {};
      case 0x0C00: r_.skip(24); return {};
      case 0x8200: return quicktime(r_.u32be());
      case 0x8201: r_.skip(r_.u32be()); return {};
      default: break;
    }
    if ((op >= 0x92 && op <= 0x97) || (op >= 0x9C && op <= 0x9F) || (op >= 0xA2 && op <= 0xAF)) {
      r_.skip(r_.u16be());
    } else if (op >= 0xB0 && op <= 0xCF) {
      // no data
    } else if (op >= 0xD0 && op <= 0xFE) {
      r_.skip(r_.u32be());
    } else if (op >= 0x0100 && op <= 0x7FFF) {
      r_.skip(static_cast<std::size_t>(op >> 8) * 2);
    } else if (op >= 0x8000 && op <= 0x80FF) {
      // no data
    } else if (op >= 0x8100) {
      r_.skip(r_.u32be());
    } else {
      return fail(Status::Decode, "unknown PICT opcode 0x{:04X}", op);
    }
    return {};
  }
};

Result<Scene> read_pict(ReadContext& ctx) { return PictReader(ctx).read(); }

}  // namespace

void register_pict(Registry& registry) {
  VectorFormat format;
  format.info.id = "pict";
  format.info.name = "Apple PICT picture";
  format.info.description = "Classic Mac OS QuickDraw picture with vector shapes, text and bitmaps.";
  format.info.category = Category::Vector;
  format.info.extensions = {"pict", "pct", "pic"};
  format.info.mime = "image/x-pict";
  format.info.popularity = 10;
  format.info.sniff = sniff_pict;
  format.read = read_pict;
  format.lossy_read = true;
  format.read_cost = 15;
  register_vector_format(registry, std::move(format));
}

}  // namespace cvt::vector::misc
