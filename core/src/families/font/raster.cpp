// Convertible core - font family: anti-aliased outline rasteriser and text layout.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/raster.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "families/font/build.hpp"
#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"
#include "util/text.hpp"

namespace cvt::font {

Rasterizer::Rasterizer(std::uint32_t width, std::uint32_t height)
    : width_(width), height_(height), stride_(std::size_t{width} + 3),
      accumulation_(stride_ * height + 4, 0.0f) {}

void Rasterizer::line(Point a, Point b) {
  if (a.y == b.y) return;
  float dir = 1.0f;
  if (a.y > b.y) {
    std::swap(a, b);
    dir = -1.0f;
  }
  const double w = width_;
  const double dxdy = (b.x - a.x) / (b.y - a.y);
  double x = a.x;
  if (a.y < 0) x -= a.y * dxdy;
  const auto y_start = static_cast<std::int64_t>(std::max(0.0, std::floor(a.y)));
  const auto y_end = static_cast<std::int64_t>(std::min<double>(height_, std::ceil(b.y)));
  for (std::int64_t y = y_start; y < y_end; ++y) {
    const std::size_t row = static_cast<std::size_t>(y) * stride_;
    const double dy = std::min<double>(static_cast<double>(y) + 1, b.y) - std::max<double>(static_cast<double>(y), a.y);
    const double x_next = x + dxdy * dy;
    const float d = static_cast<float>(dy) * dir;
    double x0 = std::clamp(std::min(x, x_next), 0.0, w);
    double x1 = std::clamp(std::max(x, x_next), 0.0, w);
    const double x0_floor = std::floor(x0);
    const auto x0i = static_cast<std::size_t>(x0_floor);
    const double x1_ceil = std::ceil(x1);
    const auto x1i = static_cast<std::size_t>(x1_ceil);
    float* acc = accumulation_.data() + row;
    if (x1i <= x0i + 1) {
      const double xmf = 0.5 * (x0 + x1) - x0_floor;
      acc[x0i] += d - d * static_cast<float>(xmf);
      acc[x0i + 1] += d * static_cast<float>(xmf);
    } else {
      const double s = 1.0 / (x1 - x0);
      const double x0f = x0 - x0_floor;
      const double a0 = 0.5 * s * (1.0 - x0f) * (1.0 - x0f);
      const double x1f = x1 - x1_ceil + 1.0;
      const double am = 0.5 * s * x1f * x1f;
      acc[x0i] += d * static_cast<float>(a0);
      if (x1i == x0i + 2) {
        acc[x0i + 1] += d * static_cast<float>(1.0 - a0 - am);
      } else {
        const double a1 = s * (1.5 - x0f);
        acc[x0i + 1] += d * static_cast<float>(a1 - a0);
        for (std::size_t xi = x0i + 2; xi + 1 < x1i; ++xi) acc[xi] += d * static_cast<float>(s);
        const double a2 = a1 + static_cast<double>(x1i - x0i - 3) * s;
        acc[x1i - 1] += d * static_cast<float>(1.0 - a2 - am);
      }
      acc[x1i] += d * static_cast<float>(am);
    }
    x = x_next;
  }
}

void Rasterizer::add(const Path& path, double scale, double origin_x, double baseline_y) {
  const auto map = [&](Point p) { return Point{origin_x + p.x * scale, baseline_y - p.y * scale}; };
  std::size_t pi = 0;
  Point start{}, current{};
  for (const Verb verb : path.verbs) {
    switch (verb) {
      case Verb::Move:
        if (!(current == start)) line(current, start);
        start = current = map(path.points[pi++]);
        break;
      case Verb::Line: {
        const Point p = map(path.points[pi++]);
        line(current, p);
        current = p;
        break;
      }
      case Verb::Quad: {
        const Point c = map(path.points[pi]), p = map(path.points[pi + 1]);
        pi += 2;
        const double dd = std::hypot(current.x - 2 * c.x + p.x, current.y - 2 * c.y + p.y);
        const int steps = std::clamp(static_cast<int>(std::ceil(std::sqrt(dd / 0.3))), 1, 256);
        Point prev = current;
        for (int i = 1; i <= steps; ++i) {
          const double t = static_cast<double>(i) / steps, u = 1 - t;
          const Point q{u * u * current.x + 2 * u * t * c.x + t * t * p.x, u * u * current.y + 2 * u * t * c.y + t * t * p.y};
          line(prev, q);
          prev = q;
        }
        current = p;
        break;
      }
      case Verb::Cubic: {
        const Point c1 = map(path.points[pi]), c2 = map(path.points[pi + 1]), p = map(path.points[pi + 2]);
        pi += 3;
        const double dd = std::max(std::hypot(current.x - 2 * c1.x + c2.x, current.y - 2 * c1.y + c2.y),
                                   std::hypot(c1.x - 2 * c2.x + p.x, c1.y - 2 * c2.y + p.y));
        const int steps = std::clamp(static_cast<int>(std::ceil(std::sqrt(dd * 0.75 / 0.3))), 1, 256);
        Point prev = current;
        for (int i = 1; i <= steps; ++i) {
          const double t = static_cast<double>(i) / steps, u = 1 - t;
          const double a = u * u * u, b = 3 * u * u * t, cc = 3 * u * t * t, dcoef = t * t * t;
          const Point q{a * current.x + b * c1.x + cc * c2.x + dcoef * p.x, a * current.y + b * c1.y + cc * c2.y + dcoef * p.y};
          line(prev, q);
          prev = q;
        }
        current = p;
        break;
      }
      case Verb::Close:
        line(current, start);
        current = start;
        break;
    }
  }
  if (!(current == start)) line(current, start);
}

std::vector<float> Rasterizer::coverage() const {
  std::vector<float> out(std::size_t{width_} * height_);
  for (std::uint32_t y = 0; y < height_; ++y) {
    float sum = 0;
    const float* acc = accumulation_.data() + std::size_t{y} * stride_;
    for (std::uint32_t x = 0; x < width_; ++x) {
      sum += acc[x];
      out[std::size_t{y} * width_ + x] = std::min(1.0f, std::abs(sum));
    }
  }
  return out;
}

void composite_mask(ir::Frame& frame, const std::vector<float>& coverage, std::uint32_t mask_width,
               std::uint32_t mask_height, std::int32_t x, std::int32_t y, std::array<std::uint8_t, 4> ink) {
  for (std::uint32_t my = 0; my < mask_height; ++my) {
    const std::int64_t fy = std::int64_t{y} + my;
    if (fy < 0 || fy >= frame.height) continue;
    for (std::uint32_t mx = 0; mx < mask_width; ++mx) {
      const std::int64_t fx = std::int64_t{x} + mx;
      if (fx < 0 || fx >= frame.width) continue;
      const float a = coverage[std::size_t{my} * mask_width + mx] * (ink[3] / 255.0f);
      if (a <= 0) continue;
      const std::size_t at = frame.offset(static_cast<std::uint32_t>(fx), static_cast<std::uint32_t>(fy));
      for (int c = 0; c < 3; ++c) {
        const float under = frame.rgba[at + static_cast<std::size_t>(c)];
        frame.rgba[at + static_cast<std::size_t>(c)] =
            static_cast<std::uint8_t>(std::lround(under + (ink[static_cast<std::size_t>(c)] - under) * a));
      }
      const float under_alpha = frame.rgba[at + 3] / 255.0f;
      frame.rgba[at + 3] = static_cast<std::uint8_t>(std::lround(255.0f * (a + under_alpha * (1 - a))));
    }
  }
}

// --------------------------------------------------------------------------- layout helpers

namespace {

struct FaceMetrics {
  double ascender = 800, descender = -200, line_gap = 0, upm = 1000;
};

FaceMetrics metrics_of(const ir::Font& font) {
  FaceMetrics m;
  m.upm = units_per_em(font);
  if (const ir::FontTable* hhea = font.find("hhea"); hhea && hhea->bytes.size() >= 10) {
    m.ascender = get_i16(hhea->bytes, 4);
    m.descender = get_i16(hhea->bytes, 6);
    m.line_gap = get_i16(hhea->bytes, 8);
  }
  if (m.ascender - m.descender <= 0) {
    m.ascender = 0.8 * m.upm;
    m.descender = -0.2 * m.upm;
  }
  return m;
}

// Draws text with either an outline GlyphSource or a bitmap strike.
class TextPainter {
 public:
  static Result<TextPainter> open(const ir::Font& font) {
    TextPainter painter;
    if (has_outline_data(font) || (!font.bitmap && !font.tables.empty())) {
      CVT_TRY_ASSIGN(GlyphSource source, GlyphSource::open(font));
      painter.source_ = std::move(source);
      painter.metrics_ = metrics_of(font);
    } else if (font.bitmap) {
      painter.bitmap_ = &*font.bitmap;
      const double size = std::max<double>(1, font.bitmap->ascent + font.bitmap->descent);
      painter.metrics_ = {static_cast<double>(font.bitmap->ascent), -static_cast<double>(font.bitmap->descent), 0, size};
    } else {
      return fail(Status::Decode, "the font has neither outlines nor bitmaps");
    }
    return painter;
  }

  [[nodiscard]] const FaceMetrics& metrics() const noexcept { return metrics_; }
  [[nodiscard]] bool has(std::uint32_t code) const {
    if (source_) return source_->cmap().contains(code);
    return find_bitmap(code) != nullptr;
  }
  /// Advance of a character in pixels at `size` px/em.
  [[nodiscard]] double advance(std::uint32_t code, double size) const {
    const double scale = size / metrics_.upm;
    if (source_) {
      const auto it = source_->cmap().find(code);
      return source_->advance(it != source_->cmap().end() ? it->second : 0) * scale;
    }
    const ir::BitmapGlyph* g = find_bitmap(code);
    return g ? g->advance * scale : size * 0.5;
  }
  /// Draw one character with its origin at (x, baseline).
  Result<void> draw(ir::Frame& frame, std::uint32_t code, double size, double x, double baseline,
                    std::array<std::uint8_t, 4> ink) const {
    const double scale = size / metrics_.upm;
    if (source_) {
      const auto it = source_->cmap().find(code);
      CVT_TRY_ASSIGN(const Path path, source_->outline(it != source_->cmap().end() ? it->second : 0));
      const Bounds b = path.control_bounds();
      if (b.empty) return {};
      const auto left = static_cast<std::int32_t>(std::floor(x + b.x_min * scale)) - 1;
      const auto top = static_cast<std::int32_t>(std::floor(baseline - b.y_max * scale)) - 1;
      const auto w = static_cast<std::uint32_t>(std::ceil((b.x_max - b.x_min) * scale)) + 3;
      const auto h = static_cast<std::uint32_t>(std::ceil((b.y_max - b.y_min) * scale)) + 3;
      if (w > 4096 || h > 4096) return {};
      Rasterizer r(w, h);
      r.add(path, scale, x - left, baseline - top);
      composite_mask(frame, r.coverage(), w, h, left, top, ink);
      return {};
    }
    const ir::BitmapGlyph* g = find_bitmap(code);
    if (!g) return {};
    const double pixel = scale;  // output pixels per bitmap pixel
    for (std::uint32_t row = 0; row < g->height; ++row) {
      for (std::uint32_t col = 0; col < g->width; ++col) {
        if (!g->pixels[std::size_t{row} * g->width + col]) continue;
        const double px = x + (g->x_offset + static_cast<double>(col)) * pixel;
        const double py = baseline - (g->y_offset + static_cast<double>(g->height - row)) * pixel;
        const auto x0 = static_cast<std::int64_t>(std::lround(px)), y0 = static_cast<std::int64_t>(std::lround(py));
        const auto x1 = static_cast<std::int64_t>(std::lround(px + pixel)), y1 = static_cast<std::int64_t>(std::lround(py + pixel));
        for (std::int64_t yy = std::max<std::int64_t>(y0, 0); yy < std::min<std::int64_t>(y1, frame.height); ++yy) {
          for (std::int64_t xx = std::max<std::int64_t>(x0, 0); xx < std::min<std::int64_t>(x1, frame.width); ++xx) {
            const std::size_t at = frame.offset(static_cast<std::uint32_t>(xx), static_cast<std::uint32_t>(yy));
            for (int c = 0; c < 4; ++c) frame.rgba[at + static_cast<std::size_t>(c)] = ink[static_cast<std::size_t>(c)];
          }
        }
      }
    }
    return {};
  }
  [[nodiscard]] std::vector<std::uint32_t> mapped(std::size_t limit) const {
    std::vector<std::uint32_t> codes;
    if (source_) {
      for (const auto& [code, glyph] : source_->cmap()) {
        if (code > 0x20 && codes.size() < limit) codes.push_back(code);
      }
    } else if (bitmap_) {
      for (const ir::BitmapGlyph& g : bitmap_->glyphs) {
        if (g.codepoint > 0x20 && codes.size() < limit) codes.push_back(static_cast<std::uint32_t>(g.codepoint));
      }
      std::ranges::sort(codes);
      codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
    }
    return codes;
  }

 private:
  [[nodiscard]] const ir::BitmapGlyph* find_bitmap(std::uint32_t code) const {
    if (!bitmap_) return nullptr;
    for (const ir::BitmapGlyph& g : bitmap_->glyphs) {
      if (g.codepoint == static_cast<std::int32_t>(code)) return &g;
    }
    return nullptr;
  }
  std::optional<GlyphSource> source_;
  const ir::BitmapFont* bitmap_ = nullptr;
  FaceMetrics metrics_;
};

// Break text into lines that fit `width` pixels.
std::vector<std::u32string> wrap(const TextPainter& painter, const std::u32string& text, double size, double width) {
  std::vector<std::u32string> lines;
  std::u32string line;
  double used = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    std::size_t j = i;
    while (j < text.size() && text[j] != U' ') ++j;
    const std::u32string word = text.substr(i, j - i);
    double w = 0;
    for (const char32_t c : word) w += painter.advance(c, size);
    const double space = painter.advance(U' ', size);
    if (!line.empty() && used + space + w > width) {
      lines.push_back(line);
      line.clear();
      used = 0;
    }
    if (!line.empty()) {
      line += U' ';
      used += space;
    }
    line += word;
    used += w;
    i = j + 1;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

}  // namespace

Result<ir::Image> render_specimen(const ir::Font& font, const SpecimenOptions& options, const Context& ctx) {
  CVT_TRY_ASSIGN(const TextPainter painter, TextPainter::open(font));
  const FaceMetrics& m = painter.metrics();
  const double margin = std::max(8.0, options.font_size * 0.5);
  const double usable = options.width - 2 * margin;
  if (usable < 16) return fail(Status::InvalidArgument, "the specimen is too narrow");
  const std::u32string sample = util::utf8_to_utf32(options.sample_text);
  std::string title_text = font.names ? font.names->full_name : std::string{};
  if (title_text.empty() && font.names) title_text = font.names->family;
  const std::u32string title = util::utf8_to_utf32(title_text);
  const double line_factor = (m.ascender - m.descender + m.line_gap) / m.upm;
  struct Block {
    std::vector<std::u32string> lines;
    double size;
  };
  std::vector<Block> blocks;
  if (!title.empty()) blocks.push_back({wrap(painter, title, options.font_size * 0.75, usable), options.font_size * 0.75});
  for (const double factor : {0.5, 0.75, 1.0, 1.5}) {
    const double size = options.font_size * factor;
    blocks.push_back({wrap(painter, sample, size, usable), size});
  }
  double height = margin;
  for (const Block& b : blocks) height += static_cast<double>(b.lines.size()) * b.size * line_factor + b.size * 0.4;
  std::vector<std::uint32_t> grid;
  const double cell = options.font_size * 1.5;
  std::uint32_t columns = 1;
  if (options.glyph_grid) {
    grid = painter.mapped(options.max_grid_glyphs);
    columns = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(usable / cell));
    height += std::ceil(static_cast<double>(grid.size()) / columns) * cell;
  }
  height += margin;
  const auto image_height = static_cast<std::uint32_t>(std::ceil(height));
  CVT_TRY(ctx.limits.check_pixels(options.width, image_height));
  ir::Frame frame = ir::Frame::filled(options.width, image_height, options.background);
  double y = margin;
  std::size_t step = 0;
  for (const Block& b : blocks) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(step++) / static_cast<double>(blocks.size() + 1), "Rendering specimen"));
    for (const std::u32string& line : b.lines) {
      const double baseline = y + m.ascender / m.upm * b.size;
      double x = margin;
      for (const char32_t c : line) {
        CVT_TRY(painter.draw(frame, c, b.size, x, baseline, options.foreground));
        x += painter.advance(c, b.size);
      }
      y += b.size * line_factor;
    }
    y += b.size * 0.4;
  }
  for (std::size_t i = 0; i < grid.size(); ++i) {
    const double cx = margin + static_cast<double>(i % columns) * cell;
    const double cy = y + static_cast<double>(i / columns) * cell;
    const double size = options.font_size;
    const double x = cx + (cell - painter.advance(grid[i], size)) / 2;
    const double baseline = cy + cell * 0.5 + (m.ascender + m.descender) / 2 / m.upm * size;
    CVT_TRY(painter.draw(frame, grid[i], size, x, baseline, options.foreground));
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

Result<std::vector<float>> render_line(const ir::Font& font, std::u32string_view text, double pixel_size,
                                       std::uint32_t width, std::uint32_t height, double origin_x, double baseline_y) {
  CVT_TRY_ASSIGN(const GlyphSource source, GlyphSource::open(font));
  const double scale = pixel_size / source.units_per_em();
  Rasterizer r(width, height);
  double x = origin_x;
  for (const char32_t c : text) {
    const auto it = source.cmap().find(c);
    const std::uint32_t glyph = it != source.cmap().end() ? it->second : 0;
    CVT_TRY_ASSIGN(const Path path, source.outline(glyph));
    r.add(path, scale, x, baseline_y);
    x += source.advance(glyph) * scale;
  }
  return r.coverage();
}

Result<ir::BitmapFont> bitmap_from_outlines(const ir::Font& font, std::uint32_t pixel_size, const Context& ctx) {
  CVT_TRY_ASSIGN(const GlyphSource source, GlyphSource::open(font));
  const FaceMetrics m = metrics_of(font);
  const double scale = pixel_size / source.units_per_em();
  ir::BitmapFont strike;
  strike.pixel_size = pixel_size;
  strike.ascent = static_cast<std::int32_t>(std::lround(m.ascender * scale));
  strike.descent = static_cast<std::int32_t>(std::lround(-m.descender * scale));
  strike.dpi_x = strike.dpi_y = 72;
  std::uint64_t pixels = 0;
  std::size_t index = 0;
  const std::size_t total = source.cmap().size();
  const std::vector<std::string> names = glyph_names(font);
  for (const auto& [code, glyph] : source.cmap()) {
    if ((index++ & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(total), "Rasterising glyphs"));
    CVT_TRY_ASSIGN(const Path path, source.outline(glyph));
    ir::BitmapGlyph out;
    out.codepoint = static_cast<std::int32_t>(code);
    out.name = glyph < names.size() ? names[glyph] : std::format("uni{:04X}", code);
    out.advance = static_cast<std::int32_t>(std::lround(source.advance(glyph) * scale));
    const Bounds b = path.control_bounds();
    if (!b.empty) {
      const auto x0 = static_cast<std::int32_t>(std::floor(b.x_min * scale));
      const auto y0 = static_cast<std::int32_t>(std::floor(b.y_min * scale));
      const auto x1 = static_cast<std::int32_t>(std::ceil(b.x_max * scale));
      const auto y1 = static_cast<std::int32_t>(std::ceil(b.y_max * scale));
      const auto w = static_cast<std::uint32_t>(std::max(0, x1 - x0));
      const auto h = static_cast<std::uint32_t>(std::max(0, y1 - y0));
      pixels += std::uint64_t{w} * h;
      CVT_TRY(ctx.limits.check_pixels(pixels, 1));
      if (w > 0 && h > 0) {
        Rasterizer r(w, h);
        r.add(path, scale, -x0, y1);
        const std::vector<float> coverage = r.coverage();
        // Trim empty rows and columns so the box is tight.
        std::int32_t top = -1, bottom = -1, left = static_cast<std::int32_t>(w), right = -1;
        for (std::uint32_t yy = 0; yy < h; ++yy) {
          for (std::uint32_t xx = 0; xx < w; ++xx) {
            if (coverage[std::size_t{yy} * w + xx] >= 0.5f) {
              if (top < 0) top = static_cast<std::int32_t>(yy);
              bottom = static_cast<std::int32_t>(yy);
              left = std::min(left, static_cast<std::int32_t>(xx));
              right = std::max(right, static_cast<std::int32_t>(xx));
            }
          }
        }
        if (top >= 0) {
          out.width = static_cast<std::uint32_t>(right - left + 1);
          out.height = static_cast<std::uint32_t>(bottom - top + 1);
          out.x_offset = x0 + left;
          out.y_offset = y1 - bottom - 1;
          out.pixels.resize(std::size_t{out.width} * out.height);
          for (std::uint32_t yy = 0; yy < out.height; ++yy) {
            for (std::uint32_t xx = 0; xx < out.width; ++xx) {
              out.pixels[std::size_t{yy} * out.width + xx] =
                  coverage[(std::size_t{yy} + static_cast<std::size_t>(top)) * w + xx + static_cast<std::size_t>(left)] >= 0.5f ? 1 : 0;
            }
          }
        }
      }
    }
    strike.glyphs.push_back(std::move(out));
  }
  if (font.names) {
    strike.properties.emplace_back("FAMILY_NAME", font.names->family);
    strike.properties.emplace_back("FULL_NAME", font.names->full_name);
  }
  if (const ir::FontTable* name = font.find("name")) {
    if (auto records = parse_name(name->bytes)) {
      const std::string copyright = best_name(*records, 0);
      if (!copyright.empty()) strike.properties.emplace_back("COPYRIGHT", copyright);
    }
  }
  return strike;
}

}  // namespace cvt::font
