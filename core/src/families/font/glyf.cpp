// Convertible core - font family: TrueType glyf/loca outlines.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/glyf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"

namespace cvt::font {

namespace {

constexpr std::uint8_t on_curve_flag = 0x01;
constexpr std::uint8_t x_short = 0x02;
constexpr std::uint8_t y_short = 0x04;
constexpr std::uint8_t repeat_flag = 0x08;
constexpr std::uint8_t x_same = 0x10;
constexpr std::uint8_t y_same = 0x20;
constexpr std::uint8_t overlap_flag = 0x40;

constexpr std::size_t max_resolved_points = 1u << 20;
constexpr int max_component_depth = 16;

}  // namespace

Result<TtGlyph> parse_glyph(ByteView data) {
  TtGlyph glyph;
  if (data.empty()) return glyph;
  util::ByteReader reader(data);
  glyph.empty = false;
  glyph.num_contours = reader.i16be();
  glyph.x_min = reader.i16be();
  glyph.y_min = reader.i16be();
  glyph.x_max = reader.i16be();
  glyph.y_max = reader.i16be();
  if (!reader.ok()) return fail(Status::Decode, "glyph header is truncated");
  if (glyph.num_contours >= 0) {
    const auto contours = static_cast<std::size_t>(glyph.num_contours);
    if (contours * 2 > reader.remaining()) return fail(Status::Decode, "glyph contour list is truncated");
    glyph.end_points.resize(contours);
    int previous = -1;
    for (std::uint16_t& end : glyph.end_points) {
      end = reader.u16be();
      if (static_cast<int>(end) <= previous && !(contours > 0 && static_cast<int>(end) == previous)) {
        if (static_cast<int>(end) < previous) return fail(Status::Decode, "glyph contour end points are not ordered");
      }
      previous = end;
    }
    const std::size_t point_count = contours == 0 ? 0 : std::size_t{glyph.end_points.back()} + 1;
    const std::uint16_t instruction_length = reader.u16be();
    const ByteView instructions = reader.bytes(instruction_length);
    if (!reader.ok()) return fail(Status::Decode, "glyph instructions are truncated");
    glyph.instructions.assign(instructions.begin(), instructions.end());
    if (point_count > reader.remaining()) return fail(Status::Decode, "glyph point data is truncated");
    std::vector<std::uint8_t> flags;
    flags.reserve(point_count);
    while (flags.size() < point_count) {
      const std::uint8_t flag = reader.u8();
      if (!reader.ok()) return fail(Status::Decode, "glyph flags are truncated");
      flags.push_back(flag);
      if (flag & repeat_flag) {
        const std::uint8_t repeats = reader.u8();
        for (std::uint8_t r = 0; r < repeats && flags.size() < point_count; ++r) flags.push_back(flag);
      }
    }
    if (!flags.empty() && (flags[0] & overlap_flag)) glyph.overlap_simple = true;
    glyph.points.resize(point_count);
    std::int32_t value = 0;
    for (std::size_t i = 0; i < point_count; ++i) {
      const std::uint8_t f = flags[i];
      if (f & x_short) {
        const std::uint8_t d = reader.u8();
        value += (f & x_same) ? d : -static_cast<std::int32_t>(d);
      } else if (!(f & x_same)) {
        value += reader.i16be();
      }
      glyph.points[i].x = value;
      glyph.points[i].on_curve = (f & on_curve_flag) != 0;
    }
    value = 0;
    for (std::size_t i = 0; i < point_count; ++i) {
      const std::uint8_t f = flags[i];
      if (f & y_short) {
        const std::uint8_t d = reader.u8();
        value += (f & y_same) ? d : -static_cast<std::int32_t>(d);
      } else if (!(f & y_same)) {
        value += reader.i16be();
      }
      glyph.points[i].y = value;
    }
    if (!reader.ok()) return fail(Status::Decode, "glyph coordinates are truncated");
    return glyph;
  }
  // Composite glyph.
  std::uint16_t flags = 0;
  bool instructions = false;
  do {
    TtComponent c;
    flags = reader.u16be();
    c.flags = flags;
    c.glyph = reader.u16be();
    if (flags & composite::arg_words) {
      if (flags & composite::args_xy) {
        c.arg1 = reader.i16be();
        c.arg2 = reader.i16be();
      } else {
        c.arg1 = reader.u16be();
        c.arg2 = reader.u16be();
      }
    } else {
      if (flags & composite::args_xy) {
        c.arg1 = reader.i8();
        c.arg2 = reader.i8();
      } else {
        c.arg1 = reader.u8();
        c.arg2 = reader.u8();
      }
    }
    if (flags & composite::have_scale) {
      c.xx = c.yy = reader.i16be();
    } else if (flags & composite::xy_scale) {
      c.xx = reader.i16be();
      c.yy = reader.i16be();
    } else if (flags & composite::two_by_two) {
      c.xx = reader.i16be();
      c.xy = reader.i16be();
      c.yx = reader.i16be();
      c.yy = reader.i16be();
    }
    if (!reader.ok()) return fail(Status::Decode, "composite glyph is truncated");
    if (flags & composite::instructions) instructions = true;
    glyph.components.push_back(c);
    if (glyph.components.size() > 4096) return fail(Status::Decode, "composite glyph has too many components");
  } while (flags & composite::more);
  if (instructions) {
    const std::uint16_t length = reader.u16be();
    const ByteView bytes = reader.bytes(length);
    if (!reader.ok()) return fail(Status::Decode, "composite glyph instructions are truncated");
    glyph.instructions.assign(bytes.begin(), bytes.end());
  }
  return glyph;
}

Bytes encode_glyph(const TtGlyph& glyph, bool recompute_bounds) {
  if (glyph.empty || (glyph.num_contours == 0 && glyph.components.empty())) return {};
  util::ByteWriter out;
  if (!glyph.is_composite()) {
    if (glyph.points.empty()) return {};
    std::int16_t x_min = glyph.x_min, y_min = glyph.y_min, x_max = glyph.x_max, y_max = glyph.y_max;
    if (recompute_bounds) {
      x_min = y_min = std::numeric_limits<std::int16_t>::max();
      x_max = y_max = std::numeric_limits<std::int16_t>::min();
      for (const TtPoint& p : glyph.points) {
        x_min = static_cast<std::int16_t>(std::min<std::int32_t>(x_min, p.x));
        y_min = static_cast<std::int16_t>(std::min<std::int32_t>(y_min, p.y));
        x_max = static_cast<std::int16_t>(std::max<std::int32_t>(x_max, p.x));
        y_max = static_cast<std::int16_t>(std::max<std::int32_t>(y_max, p.y));
      }
    }
    out.i16be(static_cast<std::int16_t>(glyph.end_points.size())).i16be(x_min).i16be(y_min).i16be(x_max).i16be(y_max);
    for (const std::uint16_t end : glyph.end_points) out.u16be(end);
    out.u16be(static_cast<std::uint16_t>(glyph.instructions.size())).bytes(glyph.instructions);
    std::vector<std::uint8_t> flags;
    util::ByteWriter xs, ys;
    std::int32_t px = 0, py = 0;
    for (std::size_t i = 0; i < glyph.points.size(); ++i) {
      const TtPoint& p = glyph.points[i];
      std::uint8_t flag = p.on_curve ? on_curve_flag : 0;
      if (i == 0 && glyph.overlap_simple) flag |= overlap_flag;
      const std::int32_t dx = p.x - px, dy = p.y - py;
      if (dx == 0) {
        flag |= x_same;
      } else if (dx >= -255 && dx <= 255) {
        flag |= x_short;
        if (dx > 0) flag |= x_same;
        xs.u8(static_cast<std::uint8_t>(dx > 0 ? dx : -dx));
      } else {
        xs.i16be(static_cast<std::int16_t>(dx));
      }
      if (dy == 0) {
        flag |= y_same;
      } else if (dy >= -255 && dy <= 255) {
        flag |= y_short;
        if (dy > 0) flag |= y_same;
        ys.u8(static_cast<std::uint8_t>(dy > 0 ? dy : -dy));
      } else {
        ys.i16be(static_cast<std::int16_t>(dy));
      }
      flags.push_back(flag);
      px = p.x;
      py = p.y;
    }
    for (std::size_t i = 0; i < flags.size();) {
      std::size_t run = 1;
      while (i + run < flags.size() && flags[i + run] == flags[i] && run < 256) ++run;
      if (run > 1) {
        out.u8(flags[i] | repeat_flag).u8(static_cast<std::uint8_t>(run - 1));
      } else {
        out.u8(flags[i]);
      }
      i += run;
    }
    out.bytes(xs.view()).bytes(ys.view());
    return out.take();
  }
  out.i16be(-1).i16be(glyph.x_min).i16be(glyph.y_min).i16be(glyph.x_max).i16be(glyph.y_max);
  for (std::size_t i = 0; i < glyph.components.size(); ++i) {
    const TtComponent& c = glyph.components[i];
    std::uint16_t flags = c.flags & ~(composite::more | composite::instructions | composite::arg_words |
                                      composite::have_scale | composite::xy_scale | composite::two_by_two);
    if (i + 1 < glyph.components.size()) flags |= composite::more;
    if (i + 1 == glyph.components.size() && !glyph.instructions.empty()) flags |= composite::instructions;
    const bool xy = (c.flags & composite::args_xy) != 0;
    const bool words = xy ? (c.arg1 < -128 || c.arg1 > 127 || c.arg2 < -128 || c.arg2 > 127)
                          : (c.arg1 > 255 || c.arg2 > 255);
    if (words) flags |= composite::arg_words;
    if (c.xy != 0 || c.yx != 0) flags |= composite::two_by_two;
    else if (c.xx != c.yy) flags |= composite::xy_scale;
    else if (c.xx != 0x4000) flags |= composite::have_scale;
    out.u16be(flags).u16be(c.glyph);
    if (words) {
      out.u16be(static_cast<std::uint16_t>(c.arg1)).u16be(static_cast<std::uint16_t>(c.arg2));
    } else {
      out.u8(static_cast<std::uint8_t>(c.arg1)).u8(static_cast<std::uint8_t>(c.arg2));
    }
    if (flags & composite::two_by_two) out.i16be(c.xx).i16be(c.xy).i16be(c.yx).i16be(c.yy);
    else if (flags & composite::xy_scale) out.i16be(c.xx).i16be(c.yy);
    else if (flags & composite::have_scale) out.i16be(c.xx);
  }
  if (!glyph.instructions.empty()) out.u16be(static_cast<std::uint16_t>(glyph.instructions.size())).bytes(glyph.instructions);
  return out.take();
}

Result<std::vector<ByteView>> glyph_slices(const ir::Font& font) {
  const ir::FontTable* glyf = font.find("glyf");
  const ir::FontTable* loca = font.find("loca");
  const ir::FontTable* head = font.find("head");
  if (!glyf || !loca || !head) return fail(Status::Decode, "the font has no TrueType outlines (glyf/loca)");
  const std::uint32_t count = num_glyphs(font);
  const bool long_offsets = get_i16(head->bytes, 50) != 0;
  const std::size_t needed = (std::size_t{count} + 1) * (long_offsets ? 4 : 2);
  if (loca->bytes.size() < needed) return fail(Status::Decode, "the loca table is too short for {} glyphs", count);
  std::vector<ByteView> slices(count);
  const ByteView data = glyf->bytes;
  for (std::uint32_t g = 0; g < count; ++g) {
    std::uint64_t start = 0, end = 0;
    if (long_offsets) {
      start = get_u32(loca->bytes, 4u * g);
      end = get_u32(loca->bytes, 4u * g + 4);
    } else {
      start = 2ull * get_u16(loca->bytes, 2u * g);
      end = 2ull * get_u16(loca->bytes, 2u * g + 2);
    }
    if (start > end || end > data.size()) {
      if (start <= data.size() && end > data.size()) end = data.size();  // tolerate a short last glyph
      else return fail(Status::Decode, "loca entry {} is out of range", g);
    }
    slices[g] = data.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
  }
  return slices;
}

Path contours_to_path(std::span<const TtPoint> points, std::span<const std::uint16_t> end_points) {
  Path path;
  std::size_t start = 0;
  for (const std::uint16_t end_point : end_points) {
    const std::size_t end = end_point;
    if (end < start || end >= points.size()) break;
    const std::span<const TtPoint> c = points.subspan(start, end - start + 1);
    start = end + 1;
    const std::size_t n = c.size();
    if (n == 0) continue;
    const auto pt = [](const TtPoint& p) { return Point{static_cast<double>(p.x), static_cast<double>(p.y)}; };
    const auto mid = [&](const TtPoint& a, const TtPoint& b) {
      return Point{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0};
    };
    Point first{};
    std::size_t begin = 0, stop = n;
    if (c[0].on_curve) {
      first = pt(c[0]);
      begin = 1;
    } else if (c[n - 1].on_curve) {
      first = pt(c[n - 1]);
      stop = n - 1;
    } else {
      first = mid(c[0], c[n - 1]);
    }
    path.move_to(first);
    bool have_off = false;
    Point off{};
    for (std::size_t i = begin; i < stop; ++i) {
      const Point p = pt(c[i]);
      if (c[i].on_curve) {
        if (have_off) path.quad_to(off, p);
        else path.line_to(p);
        have_off = false;
      } else {
        if (have_off) path.quad_to(off, Point{(off.x + p.x) / 2, (off.y + p.y) / 2});
        off = p;
        have_off = true;
      }
    }
    if (have_off) path.quad_to(off, first);
    path.close();
  }
  return path;
}

void path_to_contours(const Path& input, std::vector<TtPoint>& points, std::vector<std::uint16_t>& end_points) {
  bool has_cubic = std::ranges::find(input.verbs, Verb::Cubic) != input.verbs.end();
  const Path path = has_cubic ? input.to_quadratic(1.0) : input;
  std::vector<TtPoint> contour;
  const auto round_point = [](Point p, bool on) {
    return TtPoint{static_cast<std::int32_t>(std::lround(p.x)), static_cast<std::int32_t>(std::lround(p.y)), on};
  };
  const auto flush = [&]() {
    if (contour.empty()) return;
    // Drop the closing duplicate of the first point.
    if (contour.size() > 1 && contour.back().on_curve && contour.back().x == contour.front().x &&
        contour.back().y == contour.front().y) {
      contour.pop_back();
    }
    // Drop consecutive duplicate on-curve points.
    std::vector<TtPoint> cleaned;
    for (const TtPoint& p : contour) {
      if (!cleaned.empty() && p.on_curve && cleaned.back().on_curve && cleaned.back().x == p.x &&
          cleaned.back().y == p.y) {
        continue;
      }
      cleaned.push_back(p);
    }
    // Drop on-curve points that are exactly the midpoint of two off-curve neighbours.
    const std::size_t n = cleaned.size();
    std::vector<bool> keep(n, true);
    if (n >= 3) {
      for (std::size_t i = 0; i < n; ++i) {
        const TtPoint& p = cleaned[i];
        const TtPoint& a = cleaned[(i + n - 1) % n];
        const TtPoint& b = cleaned[(i + 1) % n];
        if (p.on_curve && !a.on_curve && !b.on_curve && a.x + b.x == 2 * p.x && a.y + b.y == 2 * p.y &&
            keep[(i + n - 1) % n]) {
          keep[i] = false;
        }
      }
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (keep[i]) {
        points.push_back(cleaned[i]);
        ++kept;
      }
    }
    if (kept > 0) end_points.push_back(static_cast<std::uint16_t>(points.size() - 1));
    contour.clear();
  };
  std::size_t pi = 0;
  for (const Verb verb : path.verbs) {
    switch (verb) {
      case Verb::Move:
        flush();
        contour.push_back(round_point(path.points[pi++], true));
        break;
      case Verb::Line: contour.push_back(round_point(path.points[pi++], true)); break;
      case Verb::Quad:
        contour.push_back(round_point(path.points[pi], false));
        contour.push_back(round_point(path.points[pi + 1], true));
        pi += 2;
        break;
      case Verb::Cubic: pi += 3; break;  // removed above
      case Verb::Close: flush(); break;
    }
  }
  flush();
}

namespace {

Result<void> resolve(std::span<const ByteView> slices, std::uint32_t glyph_id, int depth,
                     std::vector<std::uint32_t>& stack, ResolvedGlyph& out) {
  if (glyph_id >= slices.size()) return fail(Status::Decode, "component glyph {} does not exist", glyph_id);
  if (depth > max_component_depth) return fail(Status::Decode, "composite glyphs are nested too deeply");
  if (std::ranges::find(stack, glyph_id) != stack.end()) {
    return fail(Status::Decode, "composite glyph {} refers to itself", glyph_id);
  }
  CVT_TRY_ASSIGN(const TtGlyph glyph, parse_glyph(slices[glyph_id]));
  if (!glyph.is_composite()) {
    if (out.points.size() + glyph.points.size() > max_resolved_points) {
      return fail(Status::Decode, "glyph {} has too many points", glyph_id);
    }
    const auto base = static_cast<std::uint32_t>(out.points.size());
    out.points.insert(out.points.end(), glyph.points.begin(), glyph.points.end());
    for (const std::uint16_t end : glyph.end_points) {
      out.end_points.push_back(static_cast<std::uint16_t>(std::min<std::uint32_t>(base + end, 0xFFFF)));
    }
    return {};
  }
  stack.push_back(glyph_id);
  for (const TtComponent& c : glyph.components) {
    ResolvedGlyph child;
    CVT_TRY(resolve(slices, c.glyph, depth + 1, stack, child));
    const double xx = c.xx / 16384.0, xy = c.xy / 16384.0, yx = c.yx / 16384.0, yy = c.yy / 16384.0;
    for (TtPoint& p : child.points) {
      const double x = p.x, y = p.y;
      p.x = static_cast<std::int32_t>(std::lround(xx * x + yx * y));
      p.y = static_cast<std::int32_t>(std::lround(xy * x + yy * y));
    }
    std::int32_t dx = 0, dy = 0;
    if (c.flags & composite::args_xy) {
      dx = c.arg1;
      dy = c.arg2;
      if ((c.flags & composite::scaled_offset) && !(c.flags & composite::unscaled_offset)) {
        dx = static_cast<std::int32_t>(std::lround(xx * c.arg1 + yx * c.arg2));
        dy = static_cast<std::int32_t>(std::lround(xy * c.arg1 + yy * c.arg2));
      }
    } else {
      const auto parent_index = static_cast<std::size_t>(c.arg1);
      const auto child_index = static_cast<std::size_t>(c.arg2);
      if (parent_index >= out.points.size() || child_index >= child.points.size()) {
        return fail(Status::Decode, "composite glyph {} matches a point that does not exist", glyph_id);
      }
      dx = out.points[parent_index].x - child.points[child_index].x;
      dy = out.points[parent_index].y - child.points[child_index].y;
    }
    if (out.points.size() + child.points.size() > max_resolved_points) {
      return fail(Status::Decode, "glyph {} has too many points", glyph_id);
    }
    const auto base = static_cast<std::uint32_t>(out.points.size());
    for (TtPoint p : child.points) {
      p.x += dx;
      p.y += dy;
      out.points.push_back(p);
    }
    for (const std::uint16_t end : child.end_points) {
      out.end_points.push_back(static_cast<std::uint16_t>(std::min<std::uint32_t>(base + end, 0xFFFF)));
    }
  }
  stack.pop_back();
  return {};
}

}  // namespace

Result<ResolvedGlyph> resolve_glyph(std::span<const ByteView> slices, std::uint32_t glyph) {
  ResolvedGlyph out;
  std::vector<std::uint32_t> stack;
  CVT_TRY(resolve(slices, glyph, 0, stack, out));
  // Paths are built per original contour; the end point list is exact even
  // when it exceeds 16 bits, because we build from the full list.
  std::vector<std::uint16_t> ends;
  out.path = Path{};
  std::size_t start = 0;
  std::vector<std::uint16_t> local_ends;
  for (std::size_t i = 0; i < out.end_points.size(); ++i) {
    const std::size_t end = out.end_points[i];
    if (end < start || end >= out.points.size()) break;
    local_ends.assign(1, static_cast<std::uint16_t>(end - start));
    out.path.append(contours_to_path(std::span(out.points).subspan(start, end - start + 1), local_ends));
    start = end + 1;
  }
  return out;
}

GlyfLoca build_glyf_loca(std::span<const Bytes> glyphs) {
  // Two-byte padding is enough for the short loca format; use it when the
  // table fits, four-byte alignment otherwise.
  std::uint64_t short_total = 0;
  for (const Bytes& glyph : glyphs) short_total += (glyph.size() + 1) / 2 * 2;
  const bool use_short = short_total / 2 <= 0xFFFF;
  const std::size_t alignment = use_short ? 2 : 4;
  GlyfLoca result;
  util::ByteWriter glyf;
  std::vector<std::uint32_t> offsets;
  offsets.reserve(glyphs.size() + 1);
  for (const Bytes& glyph : glyphs) {
    offsets.push_back(static_cast<std::uint32_t>(glyf.size()));
    glyf.bytes(glyph);
    glyf.align(alignment);
  }
  offsets.push_back(static_cast<std::uint32_t>(glyf.size()));
  result.glyf = glyf.take();
  result.index_format = use_short ? 0 : 1;
  util::ByteWriter loca;
  for (const std::uint32_t offset : offsets) {
    if (result.index_format == 0) loca.u16be(static_cast<std::uint16_t>(offset / 2));
    else loca.u32be(offset);
  }
  result.loca = loca.take();
  return result;
}

}  // namespace cvt::font
