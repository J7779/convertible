// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "text/text.hpp"

#include <cmath>

#include "base/text_util.hpp"

namespace cvd::text {

FontSet& FontSet::instance() {
  static FontSet set;
  return set;
}

bool FontSet::install(const std::array<std::span<const std::uint8_t>, kFaceCount>& files) {
  bool ok = true;
  for (std::size_t i = 0; i < files.size(); ++i) ok = fonts_[i].load(files[i]) && ok;
  ready_ = ok;
  return ok;
}

namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026

Face fallback_face(Face face) {
  switch (face) {
    case Face::Mono:
    case Face::MonoMedium: return Face::Regular;
    default: return Face::Mono;
  }
}

}  // namespace

ShapedRun shape(std::string_view text, const Style& style) {
  ShapedRun run;
  const FontSet& set = FontSet::instance();
  if (!set.ready()) return run;
  std::string upper;
  if (style.uppercase) {
    upper = ascii_upper(text);
    text = upper;
  }
  const Font& primary = set.font(style.face);
  const float tracking_px = style.tracking * style.size;
  float pen = 0.0f;
  std::uint16_t prev = 0;
  Face prev_face = style.face;
  std::size_t pos = 0;
  run.glyphs.reserve(text.size());
  while (pos < text.size()) {
    const std::size_t start = pos;
    std::uint32_t cp = utf8_next(text, pos);
    if (cp == '\t') cp = ' ';
    if (cp == '\n' || cp == '\r') cp = ' ';
    Face face = style.face;
    const Font* font = &primary;
    std::uint16_t g = font->glyph_index(cp);
    if (g == 0 && cp > 0x7F) {
      const Face fb = fallback_face(style.face);
      const std::uint16_t g2 = set.font(fb).glyph_index(cp);
      if (g2 != 0) {
        face = fb;
        font = &set.font(fb);
        g = g2;
      }
    }
    if (style.tabular) g = font->substitute(g, "tnum");
    const float scale = style.size / static_cast<float>(font->units_per_em());
    if (prev != 0 && prev_face == face) pen += static_cast<float>(font->kerning(prev, g)) * scale;
    ShapedGlyph sg;
    sg.glyph = g;
    sg.face = face;
    sg.x = pen;
    sg.advance = static_cast<float>(font->advance(g)) * scale + tracking_px;
    sg.byte = static_cast<std::uint32_t>(start);
    pen += sg.advance;
    run.glyphs.push_back(sg);
    prev = g;
    prev_face = face;
  }
  if (!run.glyphs.empty()) pen -= tracking_px;  // no trailing tracking
  run.width = std::max(0.0f, pen);
  return run;
}

float measure(std::string_view text, const Style& style) { return shape(text, style).width; }

float ascent(const Style& style) {
  const Font& f = FontSet::instance().font(style.face);
  if (!f.valid()) return style.size * 0.8f;
  return static_cast<float>(f.ascender()) * style.size / static_cast<float>(f.units_per_em());
}

float descent(const Style& style) {
  const Font& f = FontSet::instance().font(style.face);
  if (!f.valid()) return style.size * 0.2f;
  return static_cast<float>(-f.descender()) * style.size / static_cast<float>(f.units_per_em());
}

float baseline_offset(const Style& style) {
  const float a = ascent(style);
  const float d = descent(style);
  return (style.line_height - (a + d)) * 0.5f + a;
}

std::string ellipsize_end(std::string_view text, const Style& style, float max_width) {
  if (measure(text, style) <= max_width) return std::string(text);
  const float ell = measure(kEllipsis, style);
  const ShapedRun run = shape(text, style);
  std::size_t cut = 0;
  for (const auto& g : run.glyphs) {
    if (g.x + g.advance + ell > max_width) break;
    cut = g.byte + 1;
  }
  // Move `cut` to the end of the code point that starts at the last fitting glyph.
  std::size_t end = 0;
  for (const auto& g : run.glyphs) {
    if (g.byte < cut) {
      std::size_t p = g.byte;
      utf8_next(text, p);
      end = p;
    }
  }
  std::string out(trim(text.substr(0, end)));
  out += kEllipsis;
  return out;
}

std::string ellipsize_middle(std::string_view text, const Style& style, float max_width) {
  if (measure(text, style) <= max_width) return std::string(text);
  const std::vector<std::uint32_t> cps = utf8_decode(text);
  if (cps.size() < 3) return ellipsize_end(text, style, max_width);
  // Keep up to ~40% for the tail, but at least the extension.
  std::size_t tail = std::max<std::size_t>(1, cps.size() * 2 / 5);
  const std::size_t dot = std::string_view(text).rfind('.');
  if (dot != std::string_view::npos) {
    const std::size_t ext_cps = utf8_length(text.substr(dot));
    tail = std::max(tail, std::min(ext_cps + 3, cps.size() - 1));
  }
  std::size_t head = cps.size() - tail;
  const auto build = [&](std::size_t h, std::size_t t) {
    std::string s;
    for (std::size_t i = 0; i < h; ++i) utf8_append(s, cps[i]);
    s += kEllipsis;
    for (std::size_t i = cps.size() - t; i < cps.size(); ++i) utf8_append(s, cps[i]);
    return s;
  };
  while (head + tail > 1) {
    const std::string candidate = build(head, tail);
    if (measure(candidate, style) <= max_width) return candidate;
    if (head > tail || (head > 0 && tail <= 4)) {
      --head;
    } else if (tail > 0) {
      --tail;
    } else {
      break;
    }
  }
  return std::string(kEllipsis);
}

std::vector<std::string> wrap(std::string_view text, const Style& style, float width, int max_lines) {
  std::vector<std::string> lines;
  const float space = measure(" ", style);
  for (const std::string& para : split(text, '\n')) {
    std::string line;
    float line_w = 0.0f;
    for (const std::string& word : split(para, ' ')) {
      const float ww = measure(word, style);
      if (!line.empty() && line_w + space + ww > width) {
        lines.push_back(line);
        line.clear();
        line_w = 0.0f;
      }
      if (line.empty() && ww > width) {
        // A single overlong word: break by characters.
        std::size_t pos = 0;
        std::string piece;
        while (pos < word.size()) {
          const std::size_t s = pos;
          utf8_next(word, pos);
          const std::string next = piece + word.substr(s, pos - s);
          if (!piece.empty() && measure(next, style) > width) {
            lines.push_back(piece);
            piece = word.substr(s, pos - s);
          } else {
            piece = next;
          }
        }
        line = piece;
        line_w = measure(line, style);
        continue;
      }
      if (!line.empty()) {
        line += ' ';
        line_w += space;
      }
      line += word;
      line_w += ww;
    }
    lines.push_back(line);
  }
  if (max_lines > 0 && static_cast<int>(lines.size()) > max_lines) {
    lines.resize(static_cast<std::size_t>(max_lines));
    lines.back() = ellipsize_end(lines.back() + kEllipsis.data(), style, width);
  }
  return lines;
}

std::size_t hit_test(std::string_view text, const Style& style, float x) {
  const ShapedRun run = shape(text, style);
  for (const auto& g : run.glyphs) {
    if (x < g.x + g.advance * 0.5f) return g.byte;
  }
  return text.size();
}

float caret_x(std::string_view text, const Style& style, std::size_t index) {
  const ShapedRun run = shape(text, style);
  for (const auto& g : run.glyphs) {
    if (g.byte >= index) return g.x;
  }
  return run.width;
}

// ---------------------------------------------------------------------------
// Glyph cache and drawing
// ---------------------------------------------------------------------------

GlyphCache& GlyphCache::instance() {
  static GlyphCache cache;
  return cache;
}

const gfx::Mask& GlyphCache::get(Face face, std::uint16_t glyph, float size_px, int subpixel) {
  const auto size_q = static_cast<std::uint64_t>(std::clamp(static_cast<int>(std::lround(size_px * 4.0f)), 1, 8191));
  const std::uint64_t key = (static_cast<std::uint64_t>(face) << 40) | (static_cast<std::uint64_t>(glyph) << 16) |
                            (size_q << 3) | static_cast<std::uint64_t>(subpixel & 7);
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second;
  if (cache_.size() > 6000) cache_.clear();
  const Font& font = FontSet::instance().font(face);
  const float scale = static_cast<float>(size_q) / 4.0f / static_cast<float>(font.units_per_em());
  const gfx::Path outline = font.outline(glyph);
  const Affine t{scale, 0.0f, 0.0f, -scale, static_cast<float>(subpixel) / 4.0f, 0.0f};
  gfx::Mask mask = gfx::rasterize_mask(outline.flatten(t, 0.06f));
  // Slight gamma lift so thin stems keep their weight on both themes.
  static const std::array<std::uint8_t, 256> lift = [] {
    std::array<std::uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i) {
      table[static_cast<std::size_t>(i)] =
          static_cast<std::uint8_t>(std::lround(std::pow(static_cast<float>(i) / 255.0f, 0.86f) * 255.0f));
    }
    return table;
  }();
  for (auto& a : mask.alpha) a = lift[a];
  return cache_.emplace(key, std::move(mask)).first->second;
}

void draw_text(gfx::Canvas& canvas, std::string_view text, const Style& style, Vec2 origin, Color color) {
  if (text.empty() || color.a <= 0.0f) return;
  const ShapedRun run = shape(text, style);
  const Affine& ctm = canvas.transform();
  const float scale = ctm.mean_scale();
  const float size_px = style.size * scale;
  const Vec2 dev = ctm.apply(origin);
  const float baseline_y = std::round(dev.y);
  const gfx::IRect clip = canvas.clip_bounds();
  if (baseline_y - size_px * 1.5f > static_cast<float>(clip.y1) || baseline_y + size_px < static_cast<float>(clip.y0)) {
    return;
  }
  GlyphCache& cache = GlyphCache::instance();
  for (const auto& g : run.glyphs) {
    const float gx = dev.x + g.x * scale;
    if (gx > static_cast<float>(clip.x1)) break;
    if (gx + g.advance * scale < static_cast<float>(clip.x0)) continue;
    const float fl = std::floor(gx);
    int sub = static_cast<int>(std::lround((gx - fl) * 4.0f));
    int ix = static_cast<int>(fl);
    if (sub == 4) {
      sub = 0;
      ++ix;
    }
    const gfx::Mask& m = cache.get(g.face, g.glyph, size_px, sub);
    canvas.draw_mask(m, ix, static_cast<int>(baseline_y), color);
  }
}

}  // namespace cvd::text
