// Convertible core - font family: subsetting and optimisation.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/subset.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "families/font/build.hpp"
#include "families/font/cff.hpp"
#include "families/font/glyf.hpp"
#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"
#include "util/text.hpp"

namespace cvt::font {

// --------------------------------------------------------------------------- option parsing

Result<std::optional<std::set<std::uint32_t>>> parse_unicodes(std::string_view text) {
  std::set<std::uint32_t> out;
  const auto number = [](std::string_view item) -> std::optional<std::uint32_t> {
    item = util::trim(item);
    if (item.size() > 2 && (item[0] == 'U' || item[0] == 'u') && item[1] == '+') item.remove_prefix(2);
    else if (item.size() > 2 && item[0] == '0' && (item[1] == 'x' || item[1] == 'X')) item.remove_prefix(2);
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(item.data(), item.data() + item.size(), value, 16);
    if (item.empty() || error != std::errc{} || end != item.data() + item.size() || value > 0x10FFFF) return std::nullopt;
    return value;
  };
  std::string normalized(text);
  for (char& c : normalized) {
    if (c == ';' || c == ' ' || c == '\t' || c == '\n') c = ',';
  }
  for (const std::string_view raw : util::split(normalized, ',', false)) {
    const std::string_view item = util::trim(raw);
    if (item.empty()) continue;
    if (item == "*") return std::optional<std::set<std::uint32_t>>{};
    const std::size_t dash = item.find('-');
    const auto first = number(item.substr(0, dash));
    const auto last = dash == std::string_view::npos ? first : number(item.substr(dash + 1));
    if (!first || !last || *last < *first) {
      return fail(Status::InvalidArgument, "'{}' is not a code point or range (use U+0041 or U+0020-007E)", item);
    }
    for (std::uint32_t c = *first; c <= *last; ++c) out.insert(c);
  }
  return std::optional<std::set<std::uint32_t>>(std::move(out));
}

// --------------------------------------------------------------------------- GSUB closure

namespace {

// Covered glyphs in coverage-index order.
std::vector<std::uint16_t> coverage_glyphs(ByteView table, std::size_t offset) {
  std::vector<std::uint16_t> glyphs;
  const std::uint16_t format = get_u16(table, offset);
  const std::uint16_t count = get_u16(table, offset + 2);
  if (format == 1) {
    for (std::uint32_t i = 0; i < count && offset + 4 + 2 * i + 2 <= table.size(); ++i) {
      glyphs.push_back(get_u16(table, offset + 4 + 2 * i));
    }
  } else if (format == 2) {
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::size_t at = offset + 4 + 6 * i;
      if (at + 6 > table.size()) break;
      const std::uint16_t start = get_u16(table, at), end = get_u16(table, at + 2);
      for (std::uint32_t g = start; g <= end && glyphs.size() < 65536; ++g) glyphs.push_back(static_cast<std::uint16_t>(g));
    }
  }
  return glyphs;
}

void close_subtable(ByteView t, std::size_t at, std::uint16_t type, std::set<std::uint16_t>& glyphs, int depth) {
  if (at >= t.size() || depth > 2) return;
  const std::uint16_t format = get_u16(t, at);
  const auto has = [&](std::uint16_t g) { return glyphs.contains(g); };
  switch (type) {
    case 1: {
      const std::vector<std::uint16_t> covered = coverage_glyphs(t, at + get_u16(t, at + 2));
      for (std::size_t i = 0; i < covered.size(); ++i) {
        if (!has(covered[i])) continue;
        if (format == 1) glyphs.insert(static_cast<std::uint16_t>(covered[i] + get_u16(t, at + 4)));
        else if (format == 2 && i < get_u16(t, at + 4)) glyphs.insert(get_u16(t, at + 6 + 2 * i));
      }
      break;
    }
    case 2:
    case 3: {
      const std::vector<std::uint16_t> covered = coverage_glyphs(t, at + get_u16(t, at + 2));
      const std::uint16_t sets = get_u16(t, at + 4);
      for (std::size_t i = 0; i < covered.size() && i < sets; ++i) {
        if (!has(covered[i])) continue;
        const std::size_t seq = at + get_u16(t, at + 6 + 2 * i);
        const std::uint16_t n = get_u16(t, seq);
        for (std::uint32_t k = 0; k < n; ++k) glyphs.insert(get_u16(t, seq + 2 + 2 * k));
      }
      break;
    }
    case 4: {
      const std::vector<std::uint16_t> covered = coverage_glyphs(t, at + get_u16(t, at + 2));
      const std::uint16_t sets = get_u16(t, at + 4);
      for (std::size_t i = 0; i < covered.size() && i < sets; ++i) {
        if (!has(covered[i])) continue;
        const std::size_t set = at + get_u16(t, at + 6 + 2 * i);
        const std::uint16_t ligatures = get_u16(t, set);
        for (std::uint32_t l = 0; l < ligatures; ++l) {
          const std::size_t lig = set + get_u16(t, set + 2 + 2 * l);
          const std::uint16_t result = get_u16(t, lig);
          const std::uint16_t components = get_u16(t, lig + 2);
          bool all = true;
          for (std::uint32_t c = 1; c < components && all; ++c) all = has(get_u16(t, lig + 4 + 2 * (c - 1)));
          if (all) glyphs.insert(result);
        }
      }
      break;
    }
    case 7:
      if (format == 1) close_subtable(t, at + get_u32(t, at + 4), get_u16(t, at + 2), glyphs, depth + 1);
      break;
    case 8: {
      const std::vector<std::uint16_t> covered = coverage_glyphs(t, at + get_u16(t, at + 2));
      const std::uint16_t backtrack = get_u16(t, at + 4);
      const std::size_t lookahead_at = at + 6 + 2u * backtrack;
      const std::uint16_t lookahead = get_u16(t, lookahead_at);
      const std::size_t subst_at = lookahead_at + 2 + 2u * lookahead;
      const std::uint16_t count = get_u16(t, subst_at);
      for (std::size_t i = 0; i < covered.size() && i < count; ++i) {
        if (has(covered[i])) glyphs.insert(get_u16(t, subst_at + 2 + 2 * i));
      }
      break;
    }
    default:
      break;  // contextual lookups only reference other lookups, which are all visited
  }
}

}  // namespace

std::set<std::uint16_t> gsub_closure(const ir::Font& font, std::set<std::uint16_t> glyphs) {
  const ir::FontTable* gsub = font.find("GSUB");
  if (!gsub) return glyphs;
  const ByteView t = gsub->bytes;
  const std::size_t lookups = get_u16(t, 8);
  const std::uint16_t count = get_u16(t, lookups);
  for (int pass = 0; pass < 16; ++pass) {
    const std::size_t before = glyphs.size();
    for (std::uint32_t l = 0; l < count; ++l) {
      const std::size_t lookup = lookups + get_u16(t, lookups + 2 + 2 * l);
      const std::uint16_t type = get_u16(t, lookup);
      const std::uint16_t subtables = get_u16(t, lookup + 4);
      for (std::uint32_t s = 0; s < subtables; ++s) {
        close_subtable(t, lookup + get_u16(t, lookup + 6 + 2 * s), type, glyphs, 0);
      }
    }
    if (glyphs.size() == before) break;
  }
  return glyphs;
}

// --------------------------------------------------------------------------- helpers

void strip_editor_tables(ir::Font& font) {
  static constexpr std::array<std::string_view, 16> tags = {"FFTM", "PfEd", "TSI0", "TSI1", "TSI2", "TSI3",
                                                             "TSI5", "TSIV", "TSIP", "TSID", "TSIJ", "TSIB",
                                                             "TSIC", "TSIS", "Debg", "LINO"};
  for (const std::string_view tag : tags) font.remove_table(tag);
}

Result<void> compact_glyf(ir::Font& font) {
  if (!font.find("glyf")) return {};
  CVT_TRY_ASSIGN(const std::vector<ByteView> slices, glyph_slices(font));
  std::vector<Bytes> glyphs;
  glyphs.reserve(slices.size());
  for (const ByteView slice : slices) {
    CVT_TRY_ASSIGN(const TtGlyph glyph, parse_glyph(slice));
    glyphs.push_back(encode_glyph(glyph, false));
  }
  GlyfLoca built = build_glyf_loca(glyphs);
  font.set_table("glyf", std::move(built.glyf));
  font.set_table("loca", std::move(built.loca));
  if (ir::FontTable* head = font.find("head")) put_u16(head->bytes, 50, static_cast<std::uint16_t>(built.index_format));
  return {};
}

namespace {

constexpr std::array<std::string_view, 11> layout_tags = {"GSUB", "GPOS", "GDEF", "kern", "JSTF", "BASE",
                                                           "MATH", "morx", "mort", "kerx", "feat"};

Bytes rewrite_gvar(ByteView gvar, const std::vector<std::uint32_t>& order, const std::vector<bool>& keep_data) {
  if (gvar.size() < 20) return {};
  const std::uint16_t glyph_count = get_u16(gvar, 12);
  const bool long_offsets = (get_u16(gvar, 14) & 1) != 0;
  const std::uint32_t data_at = get_u32(gvar, 16);
  const std::uint32_t shared_at = get_u32(gvar, 8);
  const std::uint16_t shared_count = get_u16(gvar, 6), axes = get_u16(gvar, 4);
  const auto offset_of = [&](std::uint32_t g) -> std::uint64_t {
    if (g > glyph_count) return 0;
    return long_offsets ? get_u32(gvar, 20 + 4 * g) : 2ull * get_u16(gvar, 20 + 2 * g);
  };
  const std::uint64_t shared_size = 2ull * axes * shared_count;
  if (std::uint64_t{shared_at} + shared_size > gvar.size()) return {};
  util::ByteWriter data;
  std::vector<std::uint32_t> offsets;
  for (std::size_t i = 0; i < order.size(); ++i) {
    offsets.push_back(static_cast<std::uint32_t>(data.size()));
    const std::uint32_t g = order[i];
    if (!keep_data[i] || g >= glyph_count) continue;
    const std::uint64_t start = data_at + offset_of(g), end = data_at + offset_of(g + 1);
    if (start <= end && end <= gvar.size()) data.bytes(gvar.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start)));
    data.align(2);
  }
  offsets.push_back(static_cast<std::uint32_t>(data.size()));
  util::ByteWriter out;
  const std::uint32_t header = 20 + 4u * static_cast<std::uint32_t>(offsets.size());
  out.u16be(1).u16be(0).u16be(axes).u16be(shared_count).u32be(header);
  out.u16be(static_cast<std::uint16_t>(order.size())).u16be(1);
  out.u32be(header + static_cast<std::uint32_t>(shared_size));
  for (const std::uint32_t o : offsets) out.u32be(o);
  out.bytes(gvar.subspan(shared_at, static_cast<std::size_t>(shared_size)));
  out.bytes(data.view());
  return out.take();
}

Bytes remap_kern(ByteView kern, const std::map<std::uint16_t, std::uint16_t>& map) {
  if (get_u16(kern, 0) != 0 || get_u16(kern, 2) != 1 || (get_u16(kern, 8) >> 8) != 0) return {};
  const std::uint16_t pairs = get_u16(kern, 10);
  std::map<std::pair<std::uint16_t, std::uint16_t>, std::int16_t> out;
  for (std::uint32_t i = 0; i < pairs; ++i) {
    const std::size_t at = 18 + 6 * i;
    if (at + 6 > kern.size()) break;
    const auto l = map.find(get_u16(kern, at)), r = map.find(get_u16(kern, at + 2));
    if (l != map.end() && r != map.end()) out[{l->second, r->second}] = get_i16(kern, at + 4);
  }
  const std::size_t count = out.size();
  std::uint16_t selector = 0;
  while ((2u << selector) <= count) ++selector;
  const auto range = static_cast<std::uint16_t>((1u << selector) * 6u);
  util::ByteWriter w;
  w.u16be(0).u16be(1).u16be(0).u16be(static_cast<std::uint16_t>(14 + 6 * count)).u16be(get_u16(kern, 8));
  w.u16be(static_cast<std::uint16_t>(count)).u16be(count ? range : 0).u16be(selector);
  w.u16be(static_cast<std::uint16_t>(count ? count * 6 - range : 0));
  for (const auto& [key, value] : out) w.u16be(key.first).u16be(key.second).i16be(value);
  return w.take();
}

}  // namespace

// --------------------------------------------------------------------------- subsetting

Result<void> subset_font(ir::Font& font, const SubsetOptions& options, const Context& ctx) {
  if (font.tables.empty()) return {};
  const std::uint16_t count = num_glyphs(font);
  if (options.drop_layout) {
    for (const std::string_view tag : layout_tags) font.remove_table(tag);
  }
  if (options.drop_names) {
    if (ir::FontTable* name = font.find("name")) {
      if (auto records = parse_name(name->bytes)) {
        const bool has_windows = std::ranges::any_of(*records, [](const NameRecord& r) { return r.platform == 3; });
        std::vector<NameRecord> kept;
        for (NameRecord& r : *records) {
          const bool essential = r.name_id <= 7 || r.name_id == 13 || r.name_id == 14;
          if (!essential) continue;
          if (has_windows && r.platform != 3 && !is_legal_name_id(r.name_id)) continue;
          kept.push_back(std::move(r));
        }
        name->bytes = build_name(std::move(kept));
      }
    }
  }
  const bool truetype = font.find("glyf") != nullptr;
  const ir::FontTable* cff_table = font.find("CFF ");
  // ---- which glyphs to keep
  std::vector<bool> keep(count, true);
  CharMap cmap;
  if (const ir::FontTable* table = font.find("cmap")) {
    if (auto parsed = parse_cmap(table->bytes, count)) cmap = std::move(*parsed);
  }
  bool subsetting = options.unicodes.has_value();
  CharMap kept_cmap;
  if (subsetting) {
    std::set<std::uint16_t> glyphs{0};
    for (const std::uint32_t code : *options.unicodes) {
      if (const auto it = cmap.find(code); it != cmap.end()) {
        glyphs.insert(it->second);
        kept_cmap.emplace(code, it->second);
      }
    }
    if (options.layout_closure && font.find("GSUB")) glyphs = gsub_closure(font, std::move(glyphs));
    if (truetype) {
      CVT_TRY_ASSIGN(const std::vector<ByteView> slices, glyph_slices(font));
      std::vector<std::uint16_t> queue(glyphs.begin(), glyphs.end());
      while (!queue.empty()) {
        const std::uint16_t g = queue.back();
        queue.pop_back();
        if (g >= slices.size()) continue;
        auto glyph = parse_glyph(slices[g]);
        if (!glyph || !glyph->is_composite()) continue;
        for (const TtComponent& c : glyph->components) {
          if (glyphs.insert(c.glyph).second) queue.push_back(c.glyph);
        }
      }
    }
    keep.assign(count, false);
    for (const std::uint16_t g : glyphs) {
      if (g < count) keep[g] = true;
    }
  }
  const bool has_layout = std::ranges::any_of(layout_tags, [&](std::string_view tag) {
    return tag != "kern" && font.find(tag) != nullptr;
  });
  const bool renumber = subsetting && !has_layout && !font.find("CFF2");
  std::vector<std::uint32_t> order;  // output glyph -> source glyph
  for (std::uint32_t g = 0; g < count; ++g) {
    if (!renumber || keep[g]) order.push_back(g);
  }
  std::map<std::uint16_t, std::uint16_t> mapping;
  for (std::size_t i = 0; i < order.size(); ++i) mapping[static_cast<std::uint16_t>(order[i])] = static_cast<std::uint16_t>(i);
  std::vector<bool> keep_out(order.size());
  for (std::size_t i = 0; i < order.size(); ++i) keep_out[i] = keep[order[i]];
  const std::vector<std::string> names = glyph_names(font);
  std::vector<HorizontalMetric> metrics;
  CVT_TRY_ASSIGN(metrics, parse_hmtx(font));
  const bool changed_glyphs = subsetting || options.drop_hinting;
  // ---- outlines
  if (truetype && changed_glyphs) {
    CVT_TRY_ASSIGN(const std::vector<ByteView> slices, glyph_slices(font));
    std::vector<Bytes> glyphs;
    glyphs.reserve(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      if ((i & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / order.size(), "Subsetting"));
      if (!keep_out[i]) {
        glyphs.emplace_back();
        continue;
      }
      const ByteView raw = slices[order[i]];
      if (!renumber && !options.drop_hinting) {
        glyphs.emplace_back(raw.begin(), raw.end());
        continue;
      }
      CVT_TRY_ASSIGN(TtGlyph glyph, parse_glyph(raw));
      if (options.drop_hinting) glyph.instructions.clear();
      for (TtComponent& c : glyph.components) {
        const auto it = mapping.find(c.glyph);
        c.glyph = it != mapping.end() ? it->second : 0;
      }
      glyphs.push_back(encode_glyph(glyph, false));
    }
    GlyfLoca built = build_glyf_loca(glyphs);
    font.set_table("glyf", std::move(built.glyf));
    font.set_table("loca", std::move(built.loca));
    if (ir::FontTable* head = font.find("head")) put_u16(head->bytes, 50, static_cast<std::uint16_t>(built.index_format));
  } else if (cff_table && changed_glyphs) {
    CVT_TRY_ASSIGN(const CffFont parsed, parse_cff(cff_table->bytes));
    std::vector<Bytes> replacement(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (!keep_out[i]) {
        replacement[i] = Bytes{std::byte{14}};  // endchar: an empty glyph of the default width
      } else if (options.drop_hinting) {
        CVT_TRY_ASSIGN(const CffGlyph glyph, cff_glyph(parsed, order[i]));
        const std::size_t fd = parsed.fd_select.empty() ? 0 : parsed.fd_select[order[i]];
        const CffPrivate& priv = parsed.privates[std::min(fd, parsed.privates.size() - 1)];
        const double advance = order[i] < metrics.size() ? metrics[order[i]].advance : priv.default_width;
        const std::optional<double> width =
            advance == priv.default_width ? std::nullopt : std::optional<double>(advance - priv.nominal_width);
        replacement[i] = cff_charstring(glyph.path, width, false);
      }
    }
    CVT_TRY_ASSIGN(Bytes rebuilt, rebuild_cff(parsed, order, replacement, !options.drop_hinting));
    font.set_table("CFF ", std::move(rebuilt));
  } else if (font.find("CFF2") && subsetting) {
    ctx.warn("CFF2 outlines cannot be subset; every glyph outline was kept (the character map was reduced).");
  }
  // ---- hinting tables
  if (options.drop_hinting) {
    for (const std::string_view tag : {"fpgm", "prep", "cvt ", "hdmx", "LTSH", "VDMX"}) font.remove_table(tag);
  }
  // ---- glyph-indexed tables
  if (subsetting) {
    if (renumber) {
      std::vector<HorizontalMetric> sub;
      for (const std::uint32_t g : order) sub.push_back(g < metrics.size() ? metrics[g] : HorizontalMetric{});
      std::uint16_t long_count = 0;
      font.set_table("hmtx", build_hmtx(sub, long_count));
      if (ir::FontTable* hhea = font.find("hhea")) put_u16(hhea->bytes, 34, long_count);
      CharMap remapped;
      for (const auto& [code, glyph] : kept_cmap) remapped.emplace(code, mapping.at(glyph));
      kept_cmap = std::move(remapped);
      if (const ir::FontTable* kern = font.find("kern")) {
        Bytes remapped_kern = remap_kern(kern->bytes, mapping);
        if (remapped_kern.empty()) font.remove_table("kern");
        else font.set_table("kern", std::move(remapped_kern));
      }
      bool dropped = false;
      for (const std::string_view tag : {"hdmx", "LTSH", "VORG", "vhea", "vmtx", "EBDT", "EBLC", "EBSC", "CBDT",
                                         "CBLC", "sbix", "COLR", "CPAL", "SVG ", "HVAR", "VVAR"}) {
        dropped |= font.remove_table(tag);
      }
      if (dropped) ctx.warn("Glyph-indexed tables that cannot be renumbered (colour, bitmap or vertical metrics) were removed.");
    }
    if (const ir::FontTable* gvar = font.find("gvar")) {
      Bytes rewritten = rewrite_gvar(gvar->bytes, order, keep_out);
      if (rewritten.empty()) return fail(Status::Decode, "the gvar table is malformed");
      font.set_table("gvar", std::move(rewritten));
    }
    font.set_table("cmap", build_cmap(kept_cmap));
    if (ir::FontTable* maxp = font.find("maxp")) put_u16(maxp->bytes, 4, static_cast<std::uint16_t>(order.size()));
  }
  // ---- glyph names
  if (const ir::FontTable* post = font.find("post")) {
    if (options.drop_glyph_names || cff_table) {
      if (options.drop_glyph_names) font.set_table("post", build_post(post->bytes, {}));
    } else if (renumber && get_u32(post->bytes, 0) == 0x00020000u) {
      std::vector<std::string> sub;
      for (const std::uint32_t g : order) sub.push_back(g < names.size() ? names[g] : std::string{});
      font.set_table("post", build_post(post->bytes, sub));
    }
  }
  // ---- metrics
  if (truetype && changed_glyphs) {
    CVT_TRY(refresh_truetype_metrics(font));
    if (options.drop_hinting) {
      if (ir::FontTable* maxp = font.find("maxp"); maxp && maxp->bytes.size() >= 32) {
        put_u16(maxp->bytes, 14, 1);
        for (std::size_t at = 16; at <= 26; at += 2) put_u16(maxp->bytes, at, 0);
      }
    }
  } else if (cff_table && changed_glyphs) {
    CVT_TRY(refresh_cff_metrics(font));
  }
  if (changed_glyphs || options.drop_names || options.drop_layout) font.remove_table("DSIG");
  decode_names(font);
  return {};
}

}  // namespace cvt::font
