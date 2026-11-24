// Convertible core - font family: bridges from Font to Image (specimen), Data, Table,
// Document (report) and Archive (one file per face).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/font/build.hpp"
#include "families/font/font.hpp"
#include "families/font/raster.hpp"
#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"
#include "families/image/image_ops.hpp"
#include "ir/archive.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/table.hpp"
#include "util/text.hpp"

namespace cvt::font {

namespace {

std::string flavor_name(const ir::Font& font) {
  if (font.tables.empty()) return font.bitmap ? "bitmap" : "empty";
  if (font.find("CFF2")) return "CFF2 (variable PostScript outlines)";
  if (font.find("CFF ")) return "CFF (PostScript outlines)";
  if (font.find("glyf")) {
    const ir::FontTable* glyf = font.find("glyf");
    return glyf->bytes.empty() ? "bitmap (embedded strikes)" : "TrueType (quadratic outlines)";
  }
  return "other";
}

std::string embedding(std::uint16_t fs_type) {
  if (fs_type & 0x0002) return "restricted licence embedding";
  if (fs_type & 0x0004) return "preview and print embedding";
  if (fs_type & 0x0008) return "editable embedding";
  return "installable embedding";
}

std::string range_text(std::uint32_t a, std::uint32_t b) {
  return a == b ? std::format("U+{:04X}", a) : std::format("U+{:04X}-{:04X}", a, b);
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges_of(const std::vector<std::uint32_t>& codes) {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
  for (const std::uint32_t c : codes) {
    if (!out.empty() && out.back().second + 1 == c) out.back().second = c;
    else out.emplace_back(c, c);
  }
  return out;
}

std::vector<std::string> feature_tags(const ir::Font& font, std::string_view tag) {
  std::vector<std::string> out;
  const ir::FontTable* t = font.find(tag);
  if (!t) return out;
  const std::size_t list = get_u16(t->bytes, 6);
  const std::uint16_t count = get_u16(t->bytes, list);
  for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
    const std::size_t at = list + 2 + 6 * i;
    if (at + 4 > t->bytes.size()) break;
    std::string feature = tag_text(get_u32(t->bytes, at));
    if (std::ranges::find(out, feature) == out.end()) out.push_back(std::move(feature));
  }
  return out;
}

std::vector<std::uint32_t> mapped_codes(const ir::Font& font) {
  std::vector<std::uint32_t> codes;
  if (const ir::FontTable* cmap = font.find("cmap")) {
    if (auto map = parse_cmap(cmap->bytes, num_glyphs(font))) {
      for (const auto& [code, glyph] : *map) codes.push_back(code);
    }
  } else if (font.bitmap) {
    for (const ir::BitmapGlyph& g : font.bitmap->glyphs) {
      if (g.codepoint >= 0) codes.push_back(static_cast<std::uint32_t>(g.codepoint));
    }
    std::ranges::sort(codes);
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
  }
  return codes;
}

Json describe_face(const ir::Font& font) {
  Json face = Json::object();
  Json names = Json::object();
  Json records = Json::array();
  if (const ir::FontTable* name = font.find("name")) {
    if (auto parsed = parse_name(name->bytes)) {
      static constexpr std::pair<std::uint16_t, std::string_view> keys[] = {
          {0, "copyright"}, {1, "family"}, {2, "subfamily"}, {3, "unique_id"}, {4, "full_name"}, {5, "version"},
          {6, "postscript_name"}, {7, "trademark"}, {8, "manufacturer"}, {9, "designer"}, {10, "description"},
          {11, "vendor_url"}, {12, "designer_url"}, {13, "license"}, {14, "license_url"}, {16, "typographic_family"},
          {17, "typographic_subfamily"}};
      for (const auto& [id, key] : keys) {
        const std::string text = best_name(*parsed, id);
        if (!text.empty()) names.set(key, text);
      }
      for (const NameRecord& r : *parsed) {
        if (r.text.empty()) continue;
        records.push_back(Json::object({{"platform", r.platform}, {"encoding", r.encoding}, {"language", r.language},
                                        {"name_id", r.name_id}, {"text", r.text}}));
      }
    }
  } else if (font.names) {
    names.set("family", font.names->family);
  }
  face.set("names", std::move(names));
  face.set("outlines", flavor_name(font));
  Json metrics = Json::object();
  metrics.set("glyphs", static_cast<std::uint64_t>(!has_outline_data(font) && font.bitmap ? font.bitmap->glyphs.size() : num_glyphs(font)));
  if (!font.tables.empty()) {
    metrics.set("units_per_em", units_per_em(font));
    if (const ir::FontTable* hhea = font.find("hhea")) {
      metrics.set("ascender", get_i16(hhea->bytes, 4));
      metrics.set("descender", get_i16(hhea->bytes, 6));
      metrics.set("line_gap", get_i16(hhea->bytes, 8));
    }
    if (const ir::FontTable* os2 = font.find("OS/2"); os2 && os2->bytes.size() >= 78) {
      metrics.set("weight_class", get_u16(os2->bytes, 4));
      metrics.set("width_class", get_u16(os2->bytes, 6));
      metrics.set("embedding", embedding(get_u16(os2->bytes, 8)));
      if (get_u16(os2->bytes, 0) >= 2 && os2->bytes.size() >= 90) {
        metrics.set("x_height", get_i16(os2->bytes, 86));
        metrics.set("cap_height", get_i16(os2->bytes, 88));
      }
    }
    if (const ir::FontTable* post = font.find("post"); post && post->bytes.size() >= 16) {
      metrics.set("italic_angle", static_cast<std::int32_t>(get_u32(post->bytes, 4)) / 65536.0);
      metrics.set("fixed_pitch", get_u32(post->bytes, 12) != 0);
    }
  }
  if (font.bitmap) {
    metrics.set("pixel_size", font.bitmap->pixel_size);
    metrics.set("bitmap_ascent", font.bitmap->ascent);
    metrics.set("bitmap_descent", font.bitmap->descent);
  }
  face.set("metrics", std::move(metrics));
  const std::vector<std::uint32_t> codes = mapped_codes(font);
  Json coverage = Json::object();
  coverage.set("characters", static_cast<std::uint64_t>(codes.size()));
  Json ranges = Json::array();
  for (const auto& [a, b] : ranges_of(codes)) {
    if (ranges.size() >= 512) break;
    ranges.push_back(range_text(a, b));
  }
  coverage.set("ranges", std::move(ranges));
  if (const ir::FontTable* cmap = font.find("cmap")) {
    const std::size_t sequences = parse_cmap14(cmap->bytes).size();
    if (sequences > 0) coverage.set("variation_sequences", static_cast<std::uint64_t>(sequences));
  }
  face.set("coverage", std::move(coverage));
  Json tables = Json::array();
  for (const ir::FontTable& t : font.tables) {
    tables.push_back(Json::object({{"tag", t.tag}, {"length", static_cast<std::uint64_t>(t.bytes.size())},
                                   {"checksum", std::format("{:08X}", table_checksum(t.bytes))}}));
  }
  face.set("tables", std::move(tables));
  if (const ir::FontTable* fvar = font.find("fvar"); fvar && fvar->bytes.size() >= 16) {
    Json axes = Json::array();
    const std::size_t at = get_u16(fvar->bytes, 4);
    const std::uint16_t count = get_u16(fvar->bytes, 8), size = get_u16(fvar->bytes, 10);
    for (std::uint32_t i = 0; i < count && i < 64; ++i) {
      const std::size_t a = at + std::size_t{size} * i;
      if (a + 20 > fvar->bytes.size()) break;
      const auto fixed = [&](std::size_t o) { return static_cast<std::int32_t>(get_u32(fvar->bytes, a + o)) / 65536.0; };
      axes.push_back(Json::object({{"tag", tag_text(get_u32(fvar->bytes, a))}, {"min", fixed(4)}, {"default", fixed(8)}, {"max", fixed(12)}}));
    }
    face.set("variation_axes", std::move(axes));
  }
  Json features = Json::object();
  for (const std::string_view tag : {"GSUB", "GPOS"}) {
    const std::vector<std::string> list = feature_tags(font, tag);
    if (!list.empty()) features.set(tag, Json(list));
  }
  if (!features.empty()) face.set("features", std::move(features));
  if (!records.empty()) face.set("name_records", std::move(records));
  return face;
}

Result<AnyIR> font_to_data(AnyIR value, Context&) {
  const ir::Font font = std::move(value).get<ir::Font>();
  ir::Data data;
  data.value = describe_face(font);
  if (!font.other_faces.empty()) {
    Json faces = Json::array();
    for (const ir::Font& f : font.other_faces) faces.push_back(describe_face(f));
    data.value.set("other_faces", std::move(faces));
  }
  return data;
}

Result<AnyIR> font_to_table(AnyIR value, Context& ctx) {
  const ir::Font font = std::move(value).get<ir::Font>();
  ir::Sheet sheet;
  sheet.name = "Characters";
  sheet.has_header = true;
  sheet.rows.push_back({ir::Cell::string("Code point"), ir::Cell::string("Character"), ir::Cell::string("Glyph ID"),
                        ir::Cell::string("Glyph name"), ir::Cell::string("Advance width")});
  const auto character = [](std::uint32_t c) {
    if (c < 0x20 || (c >= 0x7F && c < 0xA0) || (c >= 0xD800 && c <= 0xDFFF)) return std::string{};
    return util::utf32_to_utf8(std::u32string(1, static_cast<char32_t>(c)));
  };
  if (has_outline_data(font) || (!font.bitmap && !font.tables.empty())) {
    CVT_TRY_ASSIGN(const GlyphSource source, GlyphSource::open(font));
    const std::vector<std::string> names = glyph_names(font);
    std::size_t i = 0;
    for (const auto& [code, glyph] : source.cmap()) {
      if ((i++ & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / source.cmap().size(), "Listing characters"));
      const double advance = source.advance(glyph);
      sheet.rows.push_back({ir::Cell::string(std::format("U+{:04X}", code)), ir::Cell::string(character(code)),
                            ir::Cell::numeric(glyph, std::to_string(glyph)),
                            ir::Cell::string(glyph < names.size() ? names[glyph] : std::string{}),
                            ir::Cell::numeric(advance, util::format_double(advance))});
    }
  } else if (font.bitmap) {
    for (std::size_t g = 0; g < font.bitmap->glyphs.size(); ++g) {
      const ir::BitmapGlyph& glyph = font.bitmap->glyphs[g];
      if (glyph.codepoint < 0) continue;
      const auto code = static_cast<std::uint32_t>(glyph.codepoint);
      sheet.rows.push_back({ir::Cell::string(std::format("U+{:04X}", code)), ir::Cell::string(character(code)),
                            ir::Cell::numeric(static_cast<double>(g), std::to_string(g)), ir::Cell::string(glyph.name),
                            ir::Cell::numeric(glyph.advance, std::to_string(glyph.advance))});
    }
  }
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

Result<SpecimenOptions> specimen_options(const Options& options) {
  SpecimenOptions s;
  const std::string text = options.get_string("sample_text", "");
  if (!text.empty()) {
    if (!util::is_valid_utf8(text)) return fail(Status::InvalidArgument, "option 'sample_text' is not valid UTF-8");
    s.sample_text = text;
  }
  s.font_size = static_cast<double>(options.get_int("font_size", 48, 6, 400));
  s.width = static_cast<std::uint32_t>(options.get_int("image_width", 1200, 100, 8000));
  CVT_TRY_ASSIGN(const image::Rgb fg, image::parse_color(options.get_string("foreground", "141414")));
  CVT_TRY_ASSIGN(const image::Rgb bg, image::parse_color(options.get_string("background", "ffffff")));
  s.foreground = {fg[0], fg[1], fg[2], 255};
  s.background = {bg[0], bg[1], bg[2], 255};
  if (util::iequals(util::trim(options.get_string("background", "")), "transparent")) s.background[3] = 0;
  s.glyph_grid = options.get_bool("glyph_grid", true);
  return s;
}

Result<AnyIR> font_to_image(AnyIR value, Context& ctx) {
  const ir::Font font = std::move(value).get<ir::Font>();
  CVT_TRY_ASSIGN(const SpecimenOptions options, specimen_options(ctx.options));
  CVT_TRY_ASSIGN(ir::Image image, render_specimen(font, options, ctx));
  return image;
}

ir::Inlines text(std::string s) { return ir::Inlines{ir::Text{std::move(s)}}; }

Result<AnyIR> font_to_document(AnyIR value, Context& ctx) {
  const ir::Font font = std::move(value).get<ir::Font>();
  const Json info = describe_face(font);
  ir::Document doc;
  std::string title = font.names ? font.names->full_name : std::string{};
  if (title.empty() && font.names) title = font.names->family;
  if (title.empty()) title = "Font";
  doc.metadata.title = title;
  doc.blocks.push_back(ir::Heading{1, text(title)});
  const auto row = [](std::string a, std::string b) {
    ir::TableRow r;
    r.cells.push_back(ir::TableCell{text(std::move(a))});
    r.cells.push_back(ir::TableCell{text(std::move(b))});
    return r;
  };
  const auto value_text = [](const Json& v) -> std::string {
    if (v.is_string()) return v.as_string();
    if (v.is_bool()) return v.as_bool() ? "yes" : "no";
    if (v.is_integer()) return std::to_string(v.as_int64());
    if (v.is_number()) return util::format_double(v.as_double());
    return {};
  };
  ir::TableBlock names;
  names.rows.push_back(row("Name", "Value"));
  names.rows.back().header = true;
  for (const auto& member : info["names"].items()) names.rows.push_back(row(member.key(), value_text(member.value())));
  doc.blocks.push_back(ir::Heading{2, text("Names")});
  doc.blocks.push_back(std::move(names));
  ir::TableBlock metrics;
  metrics.rows.push_back(row("Metric", "Value"));
  metrics.rows.back().header = true;
  metrics.rows.push_back(row("outlines", info["outlines"].as_string()));
  for (const auto& member : info["metrics"].items()) metrics.rows.push_back(row(member.key(), value_text(member.value())));
  doc.blocks.push_back(ir::Heading{2, text("Metrics")});
  doc.blocks.push_back(std::move(metrics));
  doc.blocks.push_back(ir::Heading{2, text("Character coverage")});
  std::vector<std::string> ranges;
  for (const Json& r : info["coverage"]["ranges"].elements()) ranges.push_back(r.as_string());
  doc.blocks.push_back(ir::Paragraph{text(std::format("{} characters: {}", info["coverage"]["characters"].as_uint64(),
                                                      util::join(ranges, ", ")))});
  if (info.contains("features")) {
    std::vector<std::string> list;
    for (const auto& member : info["features"].items()) {
      for (const Json& f : member.value().elements()) list.push_back(member.key() + " " + f.as_string());
    }
    doc.blocks.push_back(ir::Heading{2, text("OpenType features")});
    doc.blocks.push_back(ir::Paragraph{text(util::join(list, ", "))});
  }
  if (!font.tables.empty()) {
    ir::TableBlock tables;
    tables.rows.push_back(row("Table", "Bytes"));
    tables.rows.back().header = true;
    for (const ir::FontTable& t : font.tables) tables.rows.push_back(row(t.tag, std::to_string(t.bytes.size())));
    doc.blocks.push_back(ir::Heading{2, text("Tables")});
    doc.blocks.push_back(std::move(tables));
  }
  SpecimenOptions options;
  options.width = 900;
  options.font_size = 32;
  options.max_grid_glyphs = 256;
  if (auto specimen = render_specimen(font, options, ctx)) {
    doc.blocks.push_back(ir::Heading{2, text("Specimen")});
    doc.blocks.push_back(ir::ImageBlock{ir::ImageSource{std::move(*specimen)}, "Specimen of " + title, ""});
  } else {
    ctx.warn("The specimen could not be rendered: " + specimen.error().message);
  }
  return doc;
}

Result<AnyIR> font_to_archive(AnyIR value, Context& ctx) {
  const ir::Font font = std::move(value).get<ir::Font>();
  ir::Archive archive;
  std::size_t index = 0;
  for (const ir::Font* face : all_faces(font)) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(index) / (font.other_faces.size() + 1), "Splitting faces"));
    if (face->tables.empty()) continue;
    CVT_TRY_ASSIGN(Bytes bytes, write_sfnt(*face));
    std::string base = face->names ? face->names->postscript_name : std::string{};
    std::string safe;
    for (const char c : base) {
      if (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '-' || c == '_') safe.push_back(c);
    }
    if (safe.empty()) safe = std::format("face{}", index);
    const bool cff = face->find("CFF ") || face->find("CFF2");
    std::string path = safe + (cff ? ".otf" : ".ttf");
    for (const ir::ArchiveEntry& e : archive.entries) {
      if (e.path == path) path = std::format("{}-{}{}", safe, index, cff ? ".otf" : ".ttf");
    }
    ir::ArchiveEntry entry;
    entry.path = std::move(path);
    entry.bytes = std::move(bytes);
    archive.entries.push_back(std::move(entry));
    ++index;
  }
  if (archive.entries.empty()) return fail(Status::Encode, "the font has no faces with outlines");
  return archive;
}

}  // namespace

void register_font_bridges(Registry& registry) {
  Bridge image;
  image.from = Kind::Font;
  image.to = Kind::Image;
  image.cost = 20;
  image.lossy = true;
  image.label = "render a font specimen";
  image.fn = font_to_image;
  image.options = {
      OptionSpec::text("sample_text", "Sample text", "", "Text shown in the specimen (default: a pangram and digits)."),
      OptionSpec::integer("font_size", "Font size", 48, 6, 400, "Size of the largest sample line, in pixels per em."),
      OptionSpec::integer("image_width", "Image width", 1200, 100, 8000, "Width of the specimen in pixels."),
      OptionSpec::text("foreground", "Text colour", "141414", "Colour of the glyphs (rrggbb or a colour name)."),
      OptionSpec::text("background", "Background colour", "ffffff", "Background colour (rrggbb, a name or 'transparent')."),
      OptionSpec::boolean("glyph_grid", "Glyph grid", true, "Append a grid of every character the font maps."),
  };
  registry.add_bridge(std::move(image));

  Bridge data;
  data.from = Kind::Font;
  data.to = Kind::Data;
  data.cost = 15;
  data.lossy = true;
  data.label = "describe a font (names, metrics, coverage, tables)";
  data.fn = font_to_data;
  registry.add_bridge(std::move(data));

  Bridge table;
  table.from = Kind::Font;
  table.to = Kind::Table;
  table.cost = 15;
  table.lossy = true;
  table.label = "list the character map of a font";
  table.fn = font_to_table;
  registry.add_bridge(std::move(table));

  Bridge document;
  document.from = Kind::Font;
  document.to = Kind::Document;
  document.cost = 12;  // preferred over the character table for document targets
  document.lossy = true;
  document.label = "font report with specimen";
  document.fn = font_to_document;
  registry.add_bridge(std::move(document));

  Bridge archive;
  archive.from = Kind::Font;
  archive.to = Kind::Archive;
  archive.cost = 30;
  archive.lossy = false;
  archive.label = "split a font collection into one file per face";
  archive.fn = font_to_archive;
  registry.add_bridge(std::move(archive));
}

}  // namespace cvt::font
