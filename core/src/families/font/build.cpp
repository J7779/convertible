// Convertible core - font family: outline extraction, TrueType <-> CFF conversion
// and building complete fonts from outlines.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/build.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "families/font/glyf.hpp"
#include "families/font/sfnt.hpp"

namespace cvt::font {

bool is_truetype_only_table(std::string_view tag) noexcept {
  static constexpr std::array<std::string_view, 10> tags = {"glyf", "loca", "cvt ", "fpgm", "prep",
                                                             "hdmx", "LTSH", "VDMX", "cvar", "gvar"};
  return std::ranges::find(tags, tag) != tags.end();
}

bool is_variation_table(std::string_view tag) noexcept {
  static constexpr std::array<std::string_view, 9> tags = {"fvar", "gvar", "cvar", "avar", "HVAR",
                                                            "VVAR", "MVAR", "STAT", "CFF2"};
  return std::ranges::find(tags, tag) != tags.end();
}

bool is_variable(const ir::Font& font) noexcept { return font.find("fvar") != nullptr; }

bool has_outline_data(const ir::Font& font) noexcept {
  if (font.find("CFF ") || font.find("CFF2")) return true;
  const ir::FontTable* glyf = font.find("glyf");
  return glyf && !glyf->bytes.empty();
}

// --------------------------------------------------------------------------- GlyphSource

Result<GlyphSource> GlyphSource::open(const ir::Font& font) {
  GlyphSource source;
  source.count_ = num_glyphs(font);
  if (source.count_ == 0) return fail(Status::Decode, "the font has no glyphs (maxp missing or empty)");
  source.upm_ = font::units_per_em(font);
  if (font.find("glyf")) {
    CVT_TRY_ASSIGN(source.slices_, glyph_slices(font));
  } else if (const ir::FontTable* cff = font.find("CFF ") ? font.find("CFF ") : font.find("CFF2")) {
    CVT_TRY_ASSIGN(CffFont parsed, parse_cff(cff->bytes));
    source.count_ = std::min<std::uint32_t>(source.count_, static_cast<std::uint32_t>(parsed.charstrings.size()));
    source.cff_ = std::move(parsed);
  } else {
    return fail(Status::Decode, "the font has no outlines (neither glyf nor CFF)");
  }
  CVT_TRY_ASSIGN(source.metrics_, parse_hmtx(font));
  if (const ir::FontTable* cmap = font.find("cmap")) {
    if (auto map = parse_cmap(cmap->bytes, source.count_)) source.cmap_ = std::move(*map);
  }
  return source;
}

Result<Path> GlyphSource::outline(std::uint32_t glyph) const {
  if (glyph >= count_) return fail(Status::Decode, "glyph {} does not exist", glyph);
  if (cff_) {
    CVT_TRY_ASSIGN(CffGlyph g, cff_glyph(*cff_, glyph));
    return std::move(g.path);
  }
  CVT_TRY_ASSIGN(ResolvedGlyph g, resolve_glyph(slices_, glyph));
  return std::move(g.path);
}

double GlyphSource::advance(std::uint32_t glyph) const {
  if (glyph < metrics_.size()) return metrics_[glyph].advance;
  if (cff_) {
    if (auto g = cff_glyph(*cff_, glyph); g && g->width) return *g->width;
  }
  return 0;
}

Result<Outlines> extract_outlines(const ir::Font& font, const Context& ctx) {
  CVT_TRY_ASSIGN(const GlyphSource source, GlyphSource::open(font));
  Outlines out;
  out.units_per_em = source.units_per_em();
  out.cubic = font.find("glyf") == nullptr;
  out.paths.reserve(source.glyph_count());
  for (std::uint32_t g = 0; g < source.glyph_count(); ++g) {
    if ((g & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(g) / source.glyph_count(), "Reading outlines"));
    CVT_TRY_ASSIGN(Path path, source.outline(g));
    out.paths.push_back(std::move(path));
    out.advances.push_back(source.advance(g));
  }
  return out;
}

// --------------------------------------------------------------------------- metrics

namespace {

void set_hmtx(ir::Font& font, std::vector<HorizontalMetric> metrics) {
  std::uint16_t long_count = 0;
  font.set_table("hmtx", build_hmtx(metrics, long_count));
  if (ir::FontTable* hhea = font.find("hhea")) put_u16(hhea->bytes, 34, long_count);
}

struct GlyphBox {
  bool empty = true;
  std::int16_t x_min = 0, y_min = 0, x_max = 0, y_max = 0;
};

void apply_boxes(ir::Font& font, const std::vector<GlyphBox>& boxes) {
  std::vector<HorizontalMetric> metrics;
  if (auto parsed = parse_hmtx(font)) metrics = std::move(*parsed);
  metrics.resize(boxes.size());
  GlyphBox total;
  std::uint16_t advance_max = 0;
  std::int16_t min_lsb = std::numeric_limits<std::int16_t>::max(), min_rsb = min_lsb;
  std::int16_t max_extent = std::numeric_limits<std::int16_t>::min();
  for (std::size_t g = 0; g < boxes.size(); ++g) {
    const GlyphBox& b = boxes[g];
    advance_max = std::max(advance_max, metrics[g].advance);
    if (b.empty) {
      metrics[g].lsb = 0;
      continue;
    }
    metrics[g].lsb = b.x_min;
    min_lsb = std::min(min_lsb, b.x_min);
    min_rsb = std::min<std::int16_t>(min_rsb, static_cast<std::int16_t>(metrics[g].advance - b.x_max));
    max_extent = std::max(max_extent, b.x_max);
    if (total.empty) {
      total = b;
    } else {
      total.x_min = std::min(total.x_min, b.x_min);
      total.y_min = std::min(total.y_min, b.y_min);
      total.x_max = std::max(total.x_max, b.x_max);
      total.y_max = std::max(total.y_max, b.y_max);
    }
  }
  if (ir::FontTable* head = font.find("head"); head && head->bytes.size() >= 54) {
    put_u16(head->bytes, 36, static_cast<std::uint16_t>(total.x_min));
    put_u16(head->bytes, 38, static_cast<std::uint16_t>(total.y_min));
    put_u16(head->bytes, 40, static_cast<std::uint16_t>(total.x_max));
    put_u16(head->bytes, 42, static_cast<std::uint16_t>(total.y_max));
  }
  if (ir::FontTable* hhea = font.find("hhea"); hhea && hhea->bytes.size() >= 36) {
    put_u16(hhea->bytes, 10, advance_max);
    if (!total.empty) {
      put_u16(hhea->bytes, 12, static_cast<std::uint16_t>(min_lsb));
      put_u16(hhea->bytes, 14, static_cast<std::uint16_t>(min_rsb));
      put_u16(hhea->bytes, 16, static_cast<std::uint16_t>(max_extent));
    }
  }
  set_hmtx(font, std::move(metrics));
}

std::int16_t clamp16(double v) {
  return static_cast<std::int16_t>(std::clamp(v, -32768.0, 32767.0));
}

std::vector<GlyphBox> boxes_of(const std::vector<Path>& paths) {
  std::vector<GlyphBox> boxes(paths.size());
  for (std::size_t g = 0; g < paths.size(); ++g) {
    const Bounds b = paths[g].exact_bounds();
    if (b.empty) continue;
    boxes[g] = {false, clamp16(std::floor(b.x_min)), clamp16(std::floor(b.y_min)), clamp16(std::ceil(b.x_max)),
                clamp16(std::ceil(b.y_max))};
  }
  return boxes;
}

void drop_variations(ir::Font& font, const Context& ctx, std::string_view what) {
  bool dropped = false;
  for (const std::string_view tag : {"fvar", "gvar", "cvar", "avar", "HVAR", "VVAR", "MVAR", "STAT"}) {
    dropped |= font.remove_table(tag);
  }
  if (dropped) {
    ctx.warn(std::format("The variable font was instantiated at its default axis values for {} output; "
                         "the variation axes were removed.", what));
  }
}

}  // namespace

Result<void> refresh_truetype_metrics(ir::Font& font) {
  CVT_TRY_ASSIGN(const std::vector<ByteView> slices, glyph_slices(font));
  std::vector<GlyphBox> boxes(slices.size());
  std::uint32_t max_points = 0, max_contours = 0, max_composite_points = 0, max_composite_contours = 0;
  std::uint32_t max_elements = 0, max_depth = 0, max_instructions = 0;
  std::vector<int> depth(slices.size(), -1);
  const auto depth_of = [&](auto&& self, std::uint32_t g, int guard) -> int {
    if (g >= slices.size() || guard > 16) return 0;
    if (depth[g] >= 0) return depth[g];
    auto glyph = parse_glyph(slices[g]);
    int d = 0;
    if (glyph && glyph->is_composite()) {
      for (const TtComponent& c : glyph->components) d = std::max(d, 1 + self(self, c.glyph, guard + 1));
    }
    depth[g] = d;
    return d;
  };
  for (std::uint32_t g = 0; g < slices.size(); ++g) {
    CVT_TRY_ASSIGN(const TtGlyph glyph, parse_glyph(slices[g]));
    if (glyph.empty) continue;
    if (glyph.num_contours != 0) boxes[g] = {false, glyph.x_min, glyph.y_min, glyph.x_max, glyph.y_max};
    max_instructions = std::max<std::uint32_t>(max_instructions, static_cast<std::uint32_t>(glyph.instructions.size()));
    if (!glyph.is_composite()) {
      max_points = std::max<std::uint32_t>(max_points, static_cast<std::uint32_t>(glyph.points.size()));
      max_contours = std::max<std::uint32_t>(max_contours, static_cast<std::uint32_t>(glyph.end_points.size()));
    } else {
      CVT_TRY_ASSIGN(const ResolvedGlyph resolved, resolve_glyph(slices, g));
      max_composite_points = std::max<std::uint32_t>(max_composite_points, static_cast<std::uint32_t>(resolved.points.size()));
      max_composite_contours = std::max<std::uint32_t>(max_composite_contours, static_cast<std::uint32_t>(resolved.end_points.size()));
      max_elements = std::max<std::uint32_t>(max_elements, static_cast<std::uint32_t>(glyph.components.size()));
      max_depth = std::max<std::uint32_t>(max_depth, static_cast<std::uint32_t>(depth_of(depth_of, g, 0)));
    }
  }
  const ir::FontTable* old = font.find("maxp");
  Bytes maxp(32, std::byte{0});
  put_u32(maxp, 0, 0x00010000u);
  put_u16(maxp, 4, static_cast<std::uint16_t>(slices.size()));
  const auto c16 = [](std::uint32_t v) { return static_cast<std::uint16_t>(std::min<std::uint32_t>(v, 0xFFFF)); };
  put_u16(maxp, 6, c16(max_points));
  put_u16(maxp, 8, c16(max_contours));
  put_u16(maxp, 10, c16(max_composite_points));
  put_u16(maxp, 12, c16(max_composite_contours));
  if (old && old->bytes.size() >= 32 && get_u32(old->bytes, 0) == 0x00010000u) {
    for (std::size_t at = 14; at < 26; at += 2) put_u16(maxp, at, get_u16(old->bytes, at));
    max_instructions = std::max<std::uint32_t>(max_instructions, get_u16(old->bytes, 26));
  } else {
    put_u16(maxp, 14, 2);
  }
  if (get_u16(maxp, 14) == 0) put_u16(maxp, 14, 2);
  put_u16(maxp, 26, c16(max_instructions));
  put_u16(maxp, 28, c16(max_elements));
  put_u16(maxp, 30, c16(max_depth));
  font.set_table("maxp", std::move(maxp));
  apply_boxes(font, boxes);
  return {};
}

Result<void> refresh_cff_metrics(ir::Font& font) {
  CVT_TRY_ASSIGN(const GlyphSource source, GlyphSource::open(font));
  std::vector<Path> paths;
  paths.reserve(source.glyph_count());
  for (std::uint32_t g = 0; g < source.glyph_count(); ++g) {
    CVT_TRY_ASSIGN(Path path, source.outline(g));
    paths.push_back(std::move(path));
  }
  apply_boxes(font, boxes_of(paths));
  return {};
}

// --------------------------------------------------------------------------- conversions

Result<ir::Font> to_truetype(const ir::Font& font, double tolerance, const Context& ctx) {
  if (font.find("glyf") && !font.find("CFF ") && !font.find("CFF2")) return font;
  CVT_TRY_ASSIGN(const Outlines outlines, extract_outlines(font, ctx));
  const std::vector<std::string> names = glyph_names(font);
  std::vector<Bytes> glyphs;
  glyphs.reserve(outlines.paths.size());
  for (std::size_t g = 0; g < outlines.paths.size(); ++g) {
    if ((g & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(g) / outlines.paths.size(), "Converting outlines"));
    TtGlyph glyph;
    const Path quadratic = outlines.paths[g].to_quadratic(tolerance).reversed();
    path_to_contours(quadratic, glyph.points, glyph.end_points);
    if (glyph.points.size() > 0xFFFF) return fail(Status::Encode, "glyph {} has too many points for TrueType", g);
    for (const TtPoint& p : glyph.points) {
      if (p.x < -32768 || p.x > 32767 || p.y < -32768 || p.y > 32767) {
        return fail(Status::Encode, "glyph {} has coordinates outside the TrueType range", g);
      }
    }
    glyph.empty = glyph.points.empty();
    glyph.num_contours = static_cast<std::int16_t>(glyph.end_points.size());
    glyphs.push_back(encode_glyph(glyph));
  }
  ir::Font out = font;
  const bool was_cff2 = out.find("CFF2") != nullptr;
  out.remove_table("CFF ");
  out.remove_table("CFF2");
  out.remove_table("VORG");
  if (was_cff2 || is_variable(out)) drop_variations(out, ctx, "TrueType");
  GlyfLoca built = build_glyf_loca(glyphs);
  out.set_table("glyf", std::move(built.glyf));
  out.set_table("loca", std::move(built.loca));
  if (ir::FontTable* head = out.find("head")) put_u16(head->bytes, 50, static_cast<std::uint16_t>(built.index_format));
  const ir::FontTable* post = out.find("post");
  out.set_table("post", build_post(post ? ByteView(post->bytes) : ByteView{}, names));
  CVT_TRY(refresh_truetype_metrics(out));
  out.flavor = ir::FontFlavor::TrueType;
  out.sfnt_version = 0;
  return out;
}

Result<ir::Font> to_cff(const ir::Font& font, bool round_coordinates, const Context& ctx) {
  if (font.find("CFF ")) return font;
  CVT_TRY_ASSIGN(const Outlines outlines, extract_outlines(font, ctx));
  CffBuildInput input;
  input.glyph_names = glyph_names(font);
  input.units_per_em = outlines.units_per_em;
  input.round_coordinates = round_coordinates;
  input.advances = outlines.advances;
  const bool reverse = !font.find("CFF2");  // TrueType sources wind the other way
  input.outlines.reserve(outlines.paths.size());
  for (std::size_t g = 0; g < outlines.paths.size(); ++g) {
    if ((g & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(g) / outlines.paths.size(), "Converting outlines"));
    Path cubic = outlines.paths[g].to_cubic();
    input.outlines.push_back(reverse ? cubic.reversed() : std::move(cubic));
  }
  if (const ir::FontTable* name = font.find("name")) {
    if (auto records = parse_name(name->bytes)) {
      input.postscript_name = best_name(*records, 6);
      input.full_name = best_name(*records, 4);
      input.family_name = best_name(*records, 1);
      input.weight = best_name(*records, 2);
      input.copyright = best_name(*records, 0);
      input.notice = best_name(*records, 7);
      input.version = best_name(*records, 5);
    }
  }
  if (input.postscript_name.empty()) {
    for (const char c : input.family_name) {
      if (c > 0x20 && c < 0x7F && c != '[' && c != ']' && c != '(' && c != ')' && c != '/' && c != '%') {
        input.postscript_name.push_back(c);
      }
    }
    if (input.postscript_name.empty()) input.postscript_name = "Untitled";
  }
  if (const ir::FontTable* post = font.find("post"); post && post->bytes.size() >= 16) {
    input.italic_angle = static_cast<std::int32_t>(get_u32(post->bytes, 4)) / 65536.0;
    input.underline_position = get_i16(post->bytes, 8);
    input.underline_thickness = get_i16(post->bytes, 10);
    input.fixed_pitch = get_u32(post->bytes, 12) != 0;
  }
  CVT_TRY_ASSIGN(Bytes cff, build_cff(input));
  ir::Font out = font;
  const bool variable = is_variable(out);
  for (auto it = out.tables.begin(); it != out.tables.end();) {
    if (is_truetype_only_table(it->tag) || it->tag == "CFF2") it = out.tables.erase(it);
    else ++it;
  }
  if (variable) drop_variations(out, ctx, "CFF");
  out.set_table("CFF ", std::move(cff));
  Bytes maxp(6, std::byte{0});
  put_u32(maxp, 0, 0x00005000u);
  put_u16(maxp, 4, static_cast<std::uint16_t>(outlines.paths.size()));
  out.set_table("maxp", std::move(maxp));
  const ir::FontTable* post = out.find("post");
  out.set_table("post", build_post(post ? ByteView(post->bytes) : ByteView{}, {}));
  apply_boxes(out, boxes_of(input.outlines));
  if (ir::FontTable* head = out.find("head")) put_u16(head->bytes, 50, 0);
  out.flavor = ir::FontFlavor::CFF;
  out.sfnt_version = 0;
  return out;
}

// --------------------------------------------------------------------------- building

namespace {

std::string postscript_name_for(const FontSpec& spec) {
  if (!spec.postscript_name.empty()) return spec.postscript_name;
  std::string name;
  for (const char c : spec.family + "-" + spec.subfamily) {
    if (c > 0x20 && c < 0x7F && std::string_view("[](){}<>/% ").find(c) == std::string_view::npos) name.push_back(c);
  }
  if (name.size() > 63) name.resize(63);
  return name.empty() ? "Untitled" : name;
}

Bytes build_kern(const std::map<std::pair<std::uint16_t, std::uint16_t>, std::int16_t>& pairs) {
  const std::size_t count = std::min<std::size_t>(pairs.size(), (0xFFFF - 18) / 6);
  std::uint16_t entry_selector = 0;
  while ((2u << entry_selector) <= count) ++entry_selector;
  const auto search_range = static_cast<std::uint16_t>((1u << entry_selector) * 6u);
  util::ByteWriter out;
  out.u16be(0).u16be(1);
  out.u16be(0).u16be(static_cast<std::uint16_t>(14 + 6 * count)).u16be(1);
  out.u16be(static_cast<std::uint16_t>(count)).u16be(search_range).u16be(entry_selector);
  out.u16be(static_cast<std::uint16_t>(count * 6 - search_range));
  std::size_t written = 0;
  for (const auto& [key, value] : pairs) {
    if (written++ == count) break;
    out.u16be(key.first).u16be(key.second).i16be(value);
  }
  return out.take();
}

}  // namespace

Result<ir::Font> build_font(const FontSpec& spec, bool cff, const Context& ctx) {
  const std::size_t count = spec.glyph_names.size();
  if (count == 0 || spec.paths.size() != count || spec.advances.size() != count) {
    return fail(Status::Encode, "the font description is incomplete");
  }
  if (count > 65535) return fail(Status::Encode, "too many glyphs ({})", count);
  ir::Font font;
  const std::string ps_name = postscript_name_for(spec);
  const std::string full = spec.full_name.empty() ? spec.family + " " + spec.subfamily : spec.full_name;
  std::vector<NameRecord> names;
  const auto add = [&](std::uint16_t id, const std::string& text) {
    if (!text.empty()) names.push_back(make_name(id, text));
  };
  add(0, spec.copyright);
  add(1, spec.family);
  add(2, spec.subfamily);
  add(3, ps_name);
  add(4, full);
  add(5, spec.version);
  add(6, ps_name);
  add(7, spec.trademark);
  add(13, spec.license);
  add(14, spec.license_url);
  font.set_table("name", build_name(names));
  font.set_table("cmap", build_cmap(spec.cmap));

  std::vector<Path> paths;
  paths.reserve(count);
  for (std::size_t g = 0; g < count; ++g) {
    if ((g & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(g) / count, "Building outlines"));
    const bool reverse = cff ? !spec.postscript_winding : spec.postscript_winding;
    paths.push_back(reverse ? spec.paths[g].reversed() : spec.paths[g]);
  }
  Os2Basics os2;
  double total_width = 0;
  int counted = 0;
  for (const double a : spec.advances) {
    if (a > 0) {
      total_width += a;
      ++counted;
    }
  }
  os2.average_width = static_cast<std::int16_t>(counted ? std::lround(total_width / counted) : 0);
  os2.weight_class = spec.weight_class;
  os2.ascender = spec.ascender;
  os2.descender = spec.descender;
  os2.line_gap = spec.line_gap;
  os2.italic = spec.italic;
  os2.bold = spec.bold;
  Bounds all;
  for (const Path& p : paths) {
    const Bounds b = p.exact_bounds();
    if (!b.empty) {
      all.add({b.x_min, b.y_min});
      all.add({b.x_max, b.y_max});
    }
  }
  os2.win_ascent = static_cast<std::uint16_t>(std::max<double>(spec.ascender, all.empty ? 0 : std::ceil(all.y_max)));
  os2.win_descent = static_cast<std::uint16_t>(std::max<double>(-spec.descender, all.empty ? 0 : -std::floor(all.y_min)));
  const auto height_of = [&](std::uint32_t code) -> std::int16_t {
    const auto it = spec.cmap.find(code);
    if (it == spec.cmap.end() || it->second >= count) return 0;
    const Bounds b = paths[it->second].exact_bounds();
    return b.empty ? std::int16_t{0} : clamp16(std::round(b.y_max));
  };
  os2.x_height = height_of('x');
  os2.cap_height = height_of('H');
  if (!spec.cmap.empty()) {
    os2.first_char = static_cast<std::uint16_t>(std::min<std::uint32_t>(spec.cmap.begin()->first, 0xFFFF));
    os2.last_char = static_cast<std::uint16_t>(std::min<std::uint32_t>(spec.cmap.rbegin()->first, 0xFFFF));
  }
  font.set_table("OS/2", build_os2(os2));
  HeadBasics head;
  head.units_per_em = spec.units_per_em;
  head.mac_style = static_cast<std::uint16_t>((spec.bold ? 1 : 0) | (spec.italic ? 2 : 0));
  font.set_table("head", build_head(head));
  HheaBasics hhea;
  hhea.ascender = spec.ascender;
  hhea.descender = spec.descender;
  hhea.line_gap = spec.line_gap;
  font.set_table("hhea", build_hhea(hhea));
  std::vector<HorizontalMetric> metrics(count);
  for (std::size_t g = 0; g < count; ++g) {
    metrics[g].advance = static_cast<std::uint16_t>(std::clamp(std::round(spec.advances[g]), 0.0, 65535.0));
  }
  set_hmtx(font, metrics);
  Bytes post_header;
  {
    util::ByteWriter w;
    w.u32be(0x00030000u).u32be(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(spec.italic_angle * 65536))));
    w.i16be(spec.underline_position).i16be(spec.underline_thickness).u32be(spec.fixed_pitch ? 1 : 0);
    w.u32be(0).u32be(0).u32be(0).u32be(0);
    post_header = w.take();
  }
  if (!spec.kerning.empty()) {
    font.set_table("kern", build_kern(spec.kerning));
    if (spec.kerning.size() > (0xFFFF - 18) / 6) ctx.warn("Only the first 10919 kerning pairs fit the kern table.");
  }
  if (cff) {
    CffBuildInput input;
    input.postscript_name = ps_name;
    input.full_name = full;
    input.family_name = spec.family;
    input.weight = spec.subfamily;
    input.notice = spec.trademark.empty() ? spec.notice : spec.trademark;
    input.copyright = spec.copyright;
    input.version = spec.version;
    input.glyph_names = spec.glyph_names;
    input.outlines = paths;
    input.advances = spec.advances;
    input.units_per_em = spec.units_per_em;
    input.italic_angle = spec.italic_angle;
    input.underline_position = spec.underline_position;
    input.underline_thickness = spec.underline_thickness;
    input.fixed_pitch = spec.fixed_pitch;
    input.private_hints = spec.private_hints;
    CVT_TRY_ASSIGN(Bytes table, build_cff(input));
    font.set_table("CFF ", std::move(table));
    Bytes maxp(6, std::byte{0});
    put_u32(maxp, 0, 0x00005000u);
    put_u16(maxp, 4, static_cast<std::uint16_t>(count));
    font.set_table("maxp", std::move(maxp));
    font.set_table("post", post_header);
    font.flavor = ir::FontFlavor::CFF;
    apply_boxes(font, boxes_of(paths));
  } else {
    std::vector<Bytes> glyphs;
    glyphs.reserve(count);
    for (std::size_t g = 0; g < count; ++g) {
      TtGlyph glyph;
      path_to_contours(paths[g].to_quadratic(1.0), glyph.points, glyph.end_points);
      if (glyph.points.size() > 0xFFFF) return fail(Status::Encode, "glyph {} has too many points", g);
      glyph.empty = glyph.points.empty();
      glyph.num_contours = static_cast<std::int16_t>(glyph.end_points.size());
      glyphs.push_back(encode_glyph(glyph));
    }
    GlyfLoca built = build_glyf_loca(glyphs);
    font.set_table("glyf", std::move(built.glyf));
    font.set_table("loca", std::move(built.loca));
    if (ir::FontTable* h = font.find("head")) put_u16(h->bytes, 50, static_cast<std::uint16_t>(built.index_format));
    font.set_table("post", build_post(post_header, spec.glyph_names));
    Bytes maxp(32, std::byte{0});
    put_u32(maxp, 0, 0x00010000u);
    put_u16(maxp, 4, static_cast<std::uint16_t>(count));
    font.set_table("maxp", std::move(maxp));
    font.flavor = ir::FontFlavor::TrueType;
    CVT_TRY(refresh_truetype_metrics(font));
  }
  decode_names(font);
  return font;
}

}  // namespace cvt::font
