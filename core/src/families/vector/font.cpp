// Convertible core - vector family: TrueType outlines and simple text layout.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/vector/font.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "families/vector/geom.hpp"
#include "util/text.hpp"

namespace cvt::vector {

namespace bundled {
extern const unsigned char font_sans[];
extern const std::size_t font_sans_size;
extern const unsigned char font_sans_bold[];
extern const std::size_t font_sans_bold_size;
extern const unsigned char font_mono[];
extern const std::size_t font_mono_size;
}  // namespace bundled

namespace {

std::uint16_t be16(ByteView d, std::size_t at) {
  if (at + 2 > d.size()) return 0;
  return static_cast<std::uint16_t>((std::to_integer<unsigned>(d[at]) << 8) | std::to_integer<unsigned>(d[at + 1]));
}
std::int16_t bes16(ByteView d, std::size_t at) { return static_cast<std::int16_t>(be16(d, at)); }
std::uint32_t be32(ByteView d, std::size_t at) {
  if (at + 4 > d.size()) return 0;
  return (std::uint32_t{be16(d, at)} << 16) | be16(d, at + 2);
}
ByteView sub(ByteView d, std::uint64_t offset, std::uint64_t length) {
  if (offset > d.size() || length > d.size() - offset) return {};
  return d.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
}

}  // namespace

Result<TrueTypeFont> TrueTypeFont::parse(ByteView bytes, std::uint32_t index) {
  TrueTypeFont font;
  font.data_ = bytes;
  std::size_t base = 0;
  if (bytes.size() >= 12 && as_chars(bytes.first(4)) == "ttcf") {
    const std::uint32_t count = be32(bytes, 8);
    if (index >= count || index > 4096) return fail(Status::Decode, "font collection has no face {}", index);
    base = be32(bytes, 12 + 4 * static_cast<std::size_t>(index));
  }
  if (base + 12 > bytes.size()) return fail(Status::Decode, "font file is truncated");
  const std::uint32_t version = be32(bytes, base);
  if (version != 0x00010000 && version != 0x74727565 /* 'true' */ && version != 0x4F54544F /* 'OTTO' */) {
    return fail(Status::Decode, "not a TrueType font");
  }
  const std::uint16_t tables = be16(bytes, base + 4);
  ByteView head, maxp, hhea;
  for (std::uint16_t i = 0; i < tables; ++i) {
    const std::size_t rec = base + 12 + 16 * static_cast<std::size_t>(i);
    if (rec + 16 > bytes.size()) return fail(Status::Decode, "font table directory is truncated");
    const std::string_view tag = as_chars(bytes.subspan(rec, 4));
    const ByteView table = sub(bytes, be32(bytes, rec + 8), be32(bytes, rec + 12));
    if (tag == "head") head = table;
    else if (tag == "maxp") maxp = table;
    else if (tag == "hhea") hhea = table;
    else if (tag == "hmtx") font.hmtx_ = table;
    else if (tag == "loca") font.loca_ = table;
    else if (tag == "glyf") font.glyf_ = table;
    else if (tag == "cmap") font.cmap_ = table;
    else if (tag == "kern") font.kern_ = table;
  }
  if (head.size() < 54 || maxp.size() < 6 || hhea.size() < 36) return fail(Status::Decode, "font lacks required tables");
  if (font.glyf_.empty() || font.loca_.empty()) return fail(Status::Decode, "only TrueType (glyf) outlines are supported");
  font.units_per_em_ = be16(head, 18);
  if (font.units_per_em_ < 16) font.units_per_em_ = 1000;
  font.long_loca_ = bes16(head, 50) != 0;
  font.glyph_count_ = be16(maxp, 4);
  font.ascender_ = bes16(hhea, 4);
  font.descender_ = bes16(hhea, 6);
  font.metric_count_ = be16(hhea, 34);
  if (font.metric_count_ == 0 || std::size_t{font.metric_count_} * 4 > font.hmtx_.size()) {
    font.metric_count_ = static_cast<std::uint16_t>(std::min<std::size_t>(font.hmtx_.size() / 4, 0xFFFF));
  }
  // Pick the best Unicode cmap subtable.
  const ByteView cmap = font.cmap_;
  const std::uint16_t subtables = be16(cmap, 2);
  int best = -1;
  for (std::uint16_t i = 0; i < subtables; ++i) {
    const std::size_t rec = 4 + 8 * static_cast<std::size_t>(i);
    const std::uint16_t platform = be16(cmap, rec), encoding = be16(cmap, rec + 2);
    const std::uint32_t offset = be32(cmap, rec + 4);
    const std::uint16_t format = be16(cmap, offset);
    int score = -1;
    const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
    if (!unicode) continue;
    if (format == 12) score = 3;
    else if (format == 4) score = 2;
    else if (format == 6) score = 1;
    if (score > best) {
      best = score;
      font.cmap_subtable_ = offset;
      font.cmap_format_ = format;
    }
  }
  return font;
}

std::uint16_t TrueTypeFont::glyph_index(char32_t cp) const noexcept {
  const ByteView c = cmap_;
  const std::size_t t = cmap_subtable_;
  switch (cmap_format_) {
    case 4: {
      if (cp > 0xFFFF) return 0;
      const std::uint16_t segs = be16(c, t + 6) / 2;
      const std::size_t ends = t + 14, starts = ends + 2 * std::size_t{segs} + 2;
      const std::size_t deltas = starts + 2 * std::size_t{segs}, ranges = deltas + 2 * std::size_t{segs};
      // Binary search over end codes.
      std::size_t lo = 0, hi = segs;
      while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (be16(c, ends + 2 * mid) < cp) lo = mid + 1;
        else hi = mid;
      }
      if (lo >= segs) return 0;
      const std::uint16_t start = be16(c, starts + 2 * lo);
      if (cp < start) return 0;
      const std::uint16_t delta = be16(c, deltas + 2 * lo);
      const std::uint16_t range = be16(c, ranges + 2 * lo);
      if (range == 0) return static_cast<std::uint16_t>(cp + delta);
      const std::size_t at = ranges + 2 * lo + range + 2 * (cp - start);
      const std::uint16_t g = be16(c, at);
      return g == 0 ? 0 : static_cast<std::uint16_t>(g + delta);
    }
    case 6: {
      const std::uint16_t first = be16(c, t + 6), count = be16(c, t + 8);
      if (cp < first || cp >= std::uint32_t{first} + count) return 0;
      return be16(c, t + 10 + 2 * (cp - first));
    }
    case 12: {
      const std::uint32_t groups = be32(c, t + 12);
      std::uint32_t lo = 0, hi = std::min<std::uint32_t>(groups, static_cast<std::uint32_t>(c.size() / 12));
      while (lo < hi) {
        const std::uint32_t mid = (lo + hi) / 2;
        const std::size_t g = t + 16 + 12 * std::size_t{mid};
        const std::uint32_t start = be32(c, g), end = be32(c, g + 4);
        if (cp < start) hi = mid;
        else if (cp > end) lo = mid + 1;
        else return static_cast<std::uint16_t>(be32(c, g + 8) + (cp - start));
      }
      return 0;
    }
    default: return 0;
  }
}

std::uint16_t TrueTypeFont::advance(std::uint16_t glyph) const noexcept {
  if (metric_count_ == 0) return units_per_em_ / 2;
  const std::uint16_t g = std::min<std::uint16_t>(glyph, static_cast<std::uint16_t>(metric_count_ - 1));
  return be16(hmtx_, 4 * std::size_t{g});
}

std::int16_t TrueTypeFont::kerning(std::uint16_t left, std::uint16_t right) const noexcept {
  if (kern_.size() < 4 || be16(kern_, 0) != 0) return 0;
  const std::uint16_t tables = be16(kern_, 2);
  std::size_t at = 4;
  for (std::uint16_t i = 0; i < tables && at + 6 <= kern_.size(); ++i) {
    const std::uint16_t length = be16(kern_, at + 2);
    const std::uint16_t coverage = be16(kern_, at + 4);
    if ((coverage >> 8) == 0 && (coverage & 1) != 0) {
      const std::uint16_t pairs = be16(kern_, at + 6);
      const std::uint32_t key = (std::uint32_t{left} << 16) | right;
      std::size_t lo = 0, hi = pairs;
      while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const std::size_t rec = at + 14 + 6 * mid;
        const std::uint32_t k = be32(kern_, rec);
        if (k < key) lo = mid + 1;
        else if (k > key) hi = mid;
        else return bes16(kern_, rec + 4);
      }
    }
    if (length < 6) break;
    at += length;
  }
  return 0;
}

Path TrueTypeFont::outline(std::uint16_t glyph) const { return outline_at(glyph, 0); }

Path TrueTypeFont::outline_at(std::uint16_t glyph, int depth) const {
  Path path;
  if (glyph >= glyph_count_ || depth > 8) return path;
  std::uint32_t start = 0, end = 0;
  if (long_loca_) {
    start = be32(loca_, 4 * std::size_t{glyph});
    end = be32(loca_, 4 * std::size_t{glyph} + 4);
  } else {
    start = 2u * be16(loca_, 2 * std::size_t{glyph});
    end = 2u * be16(loca_, 2 * std::size_t{glyph} + 2);
  }
  if (end <= start || end > glyf_.size()) return path;
  const ByteView g = glyf_.subspan(start, end - start);
  const std::int16_t contours = bes16(g, 0);
  if (contours >= 0) {
    const std::size_t nc = static_cast<std::size_t>(contours);
    std::vector<std::uint16_t> end_points(nc);
    for (std::size_t i = 0; i < nc; ++i) end_points[i] = be16(g, 10 + 2 * i);
    if (nc == 0) return path;
    const std::size_t npoints = std::size_t{end_points.back()} + 1;
    std::size_t at = 10 + 2 * nc;
    const std::uint16_t instructions = be16(g, at);
    at += 2 + instructions;
    std::vector<std::uint8_t> flags;
    flags.reserve(npoints);
    while (flags.size() < npoints) {
      if (at >= g.size()) return {};
      const std::uint8_t f = std::to_integer<std::uint8_t>(g[at++]);
      flags.push_back(f);
      if (f & 8) {
        if (at >= g.size()) return {};
        std::size_t repeat = std::to_integer<std::uint8_t>(g[at++]);
        while (repeat-- > 0 && flags.size() < npoints) flags.push_back(f);
      }
    }
    std::vector<Point> pts(npoints);
    int value = 0;
    for (std::size_t i = 0; i < npoints; ++i) {
      const std::uint8_t f = flags[i];
      if (f & 2) {
        if (at >= g.size()) return {};
        const int d = std::to_integer<std::uint8_t>(g[at++]);
        value += (f & 16) ? d : -d;
      } else if (!(f & 16)) {
        value += bes16(g, at);
        at += 2;
      }
      pts[i].x = value;
    }
    value = 0;
    for (std::size_t i = 0; i < npoints; ++i) {
      const std::uint8_t f = flags[i];
      if (f & 4) {
        if (at >= g.size()) return {};
        const int d = std::to_integer<std::uint8_t>(g[at++]);
        value += (f & 32) ? d : -d;
      } else if (!(f & 32)) {
        value += bes16(g, at);
        at += 2;
      }
      pts[i].y = value;
    }
    std::size_t first = 0;
    for (std::size_t c = 0; c < nc; ++c) {
      const std::size_t last = end_points[c];
      if (last < first || last >= npoints) break;
      const std::size_t n = last - first + 1;
      const auto on = [&](std::size_t k) { return (flags[first + k % n] & 1) != 0; };
      const auto pt = [&](std::size_t k) { return pts[first + k % n]; };
      // Start at an on-curve point (or the midpoint of two off-curve points).
      std::size_t s = 0;
      while (s < n && !on(s)) ++s;
      Point start_pt;
      std::size_t k = 0, stop = 0;
      if (s == n) {
        start_pt = lerp(pt(0), pt(1), 0.5);
        k = 1;
        stop = n + 1;
      } else {
        start_pt = pt(s);
        k = s + 1;
        stop = s + n;
      }
      path.move_to(start_pt);
      std::optional<Point> control;
      for (; k <= stop; ++k) {
        const Point p = (k == stop) ? start_pt : pt(k);
        const bool is_on = (k == stop) ? true : on(k);
        if (is_on) {
          if (control) {
            path.quad_to(*control, p);
            control.reset();
          } else {
            path.line_to(p);
          }
        } else {
          if (control) {
            const Point mid = lerp(*control, p, 0.5);
            path.quad_to(*control, mid);
          }
          control = p;
        }
      }
      path.close();
      first = last + 1;
    }
    return path;
  }
  // Composite glyph.
  std::size_t at = 10;
  while (at + 4 <= g.size()) {
    const std::uint16_t flags = be16(g, at);
    const std::uint16_t component = be16(g, at + 2);
    at += 4;
    double dx = 0, dy = 0;
    if (flags & 1) {
      dx = bes16(g, at);
      dy = bes16(g, at + 2);
      at += 4;
    } else {
      if (at + 2 > g.size()) break;
      dx = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(g[at]));
      dy = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(g[at + 1]));
      at += 2;
    }
    if (!(flags & 2)) dx = dy = 0;  // point matching is not supported
    Matrix m{1, 0, 0, 1, dx, dy};
    const auto f2dot14 = [&](std::size_t p) { return bes16(g, p) / 16384.0; };
    if (flags & 8) {
      m.a = m.d = f2dot14(at);
      at += 2;
    } else if (flags & 0x40) {
      m.a = f2dot14(at);
      m.d = f2dot14(at + 2);
      at += 4;
    } else if (flags & 0x80) {
      m.a = f2dot14(at);
      m.b = f2dot14(at + 2);
      m.c = f2dot14(at + 4);
      m.d = f2dot14(at + 6);
      at += 8;
    }
    const Path part = outline_at(component, depth + 1);
    Path moved = part;
    for (Point& p : moved.points) p = m.apply(p);
    path.verbs.insert(path.verbs.end(), moved.verbs.begin(), moved.verbs.end());
    path.points.insert(path.points.end(), moved.points.begin(), moved.points.end());
    if (!(flags & 0x20)) break;
  }
  return path;
}

Result<TrueTypeFont> fallback_font(FallbackFace face) {
  switch (face) {
    case FallbackFace::Sans:
      return TrueTypeFont::parse(ByteView(reinterpret_cast<const std::byte*>(bundled::font_sans), bundled::font_sans_size));
    case FallbackFace::SansBold:
      return TrueTypeFont::parse(
          ByteView(reinterpret_cast<const std::byte*>(bundled::font_sans_bold), bundled::font_sans_bold_size));
    case FallbackFace::Mono:
      return TrueTypeFont::parse(ByteView(reinterpret_cast<const std::byte*>(bundled::font_mono), bundled::font_mono_size));
  }
  return fail(Status::Internal, "unknown bundled font");
}

FallbackFace choose_fallback(std::string_view family, int weight) {
  const std::string f = util::to_lower(family);
  for (const std::string_view mono : {"mono", "courier", "consol", "menlo", "monaco", "code", "fixed", "typewriter"}) {
    if (f.find(mono) != std::string::npos) return FallbackFace::Mono;
  }
  return weight >= 600 ? FallbackFace::SansBold : FallbackFace::Sans;
}

TextLayout layout_text(const TrueTypeFont& face, std::string_view utf8, double size, bool italic, double letter_spacing,
                       double word_spacing) {
  TextLayout layout;
  if (!(size > 0) || !std::isfinite(size)) return layout;
  const double scale = size / face.units_per_em();
  const double slant = italic ? 0.2 : 0.0;
  layout.ascent = face.ascender() * scale;
  layout.descent = -face.descender() * scale;
  double pen = 0;
  std::uint16_t previous = 0;
  std::size_t pos = 0;
  while (pos < utf8.size()) {
    const util::Utf8Decoded d = util::decode_utf8(utf8, pos);
    pos += std::max<std::size_t>(1, d.length);
    char32_t cp = d.code_point;
    if (cp == '\t' || cp == '\n' || cp == '\r') cp = ' ';
    const std::uint16_t glyph = face.glyph_index(cp);
    if (previous != 0 && glyph != 0) pen += face.kerning(previous, glyph) * scale;
    layout.positions.push_back(pen);
    const Path outline = face.outline(glyph);
    const Matrix m{scale, 0, slant * scale, -scale, pen, 0};
    append(layout.outline, outline, m);
    pen += face.advance(glyph) * scale + letter_spacing;
    if (cp == ' ') pen += word_spacing;
    previous = glyph;
  }
  layout.advance = pen;
  return layout;
}

TextLayout layout_text(std::string_view utf8, const FontSpec& font, double letter_spacing, double word_spacing) {
  auto face = fallback_font(choose_fallback(font.family, font.weight));
  if (!face) return {};
  return layout_text(*face, utf8, font.size, font.italic, letter_spacing, word_spacing);
}

Text make_text(std::string_view utf8, const FontSpec& font, Point origin, Paint fill) {
  Text text;
  text.content = std::string(utf8);
  text.font = font;
  text.origin = origin;
  TextLayout layout = layout_text(utf8, font);
  text.advance = layout.advance;
  text.outline = transformed(layout.outline, Matrix::translate(origin.x, origin.y));
  text.fill = std::move(fill);
  return text;
}

}  // namespace cvt::vector
