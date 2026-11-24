// Convertible core - font family: format registration, readers, writers and the
// shared write pipeline (bitmap tracing, metadata stripping, subsetting, outline conversion).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/font/bitmap.hpp"
#include "families/font/build.hpp"
#include "families/font/cff.hpp"
#include "families/font/containers.hpp"
#include "families/font/font.hpp"
#include "families/font/raster.hpp"
#include "families/font/sfnt.hpp"
#include "families/font/subset.hpp"
#include "families/font/tables.hpp"
#include "families/font/type1.hpp"
#include "util/text.hpp"

namespace cvt::font {

// --------------------------------------------------------------------------- options and pipeline

std::vector<OptionSpec> subset_option_specs() {
  return {
      options::strip_metadata(),
      options::preset(),
      OptionSpec::text("unicodes", "Characters to keep", "",
                       "Keep only these code points or ranges, e.g. U+0020-007E, U+00E9 (empty keeps every glyph)."),
      OptionSpec::text("text", "Text to keep", "", "Keep only the glyphs needed to display this text."),
      OptionSpec::boolean("drop_hinting", "Remove hinting", false,
                          "Remove TrueType instructions and CFF hints (smaller; the preset 'small' does this too).")
          .advanced(),
      OptionSpec::boolean("drop_layout", "Remove layout tables", false,
                          "Remove GSUB, GPOS, GDEF and kern (ligatures, kerning and other OpenType features).")
          .advanced(),
      OptionSpec::boolean("drop_names", "Remove optional names", false,
                          "Keep only the essential and legal name records (family, style, copyright, trademark, licence).")
          .advanced(),
      OptionSpec::boolean("drop_glyph_names", "Remove glyph names", false,
                          "Store a compact post table without glyph names (TrueType outlines only).")
          .advanced(),
  };
}

namespace {

std::vector<OptionSpec> with(std::vector<OptionSpec> base, std::initializer_list<OptionSpec> extra) {
  base.insert(base.end(), extra.begin(), extra.end());
  return base;
}

OptionSpec tolerance_option() {
  return OptionSpec::number("curve_tolerance", "Curve tolerance", 1.0, 0.05, 20.0,
                            "Largest distance, in font units, between cubic curves and their quadratic approximation.")
      .advanced();
}

OptionSpec exact_curves_option() {
  return OptionSpec::boolean("exact_curves", "Exact curve points", false,
                             "Keep fractional control points when TrueType curves become CFF curves (larger files).")
      .advanced();
}

OptionSpec face_option() {
  return OptionSpec::integer("face", "Face", 0, 0, 4095, "Which font of a collection or suitcase to use first.");
}

bool has_outlines(const ir::Font& font) { return has_outline_data(font); }

Result<SubsetOptions> subset_options(const Options& options, bool& any) {
  SubsetOptions s;
  const std::string unicodes = options.get_string("unicodes", "");
  const std::string text = options.get_string("text", "");
  bool everything = false;
  if (!util::trim(unicodes).empty()) {
    CVT_TRY_ASSIGN(const auto parsed, parse_unicodes(unicodes));
    if (parsed) s.unicodes = *parsed;
    else everything = true;
  }
  if (!text.empty() && !everything) {
    if (!util::is_valid_utf8(text)) return fail(Status::InvalidArgument, "option 'text' is not valid UTF-8");
    std::set<std::uint32_t> all = s.unicodes.value_or(std::set<std::uint32_t>{});
    for (const char32_t c : util::utf8_to_utf32(text)) all.insert(static_cast<std::uint32_t>(c));
    all.insert(0x20);
    s.unicodes = std::move(all);
  }
  s.drop_hinting = options.get_bool("drop_hinting", false);
  if (!options.was_set("drop_hinting") && options::resolved_preset(options) == options::Preset::Small) s.drop_hinting = true;
  s.drop_layout = options.get_bool("drop_layout", false);
  s.drop_names = options.get_bool("drop_names", false);
  s.drop_glyph_names = options.get_bool("drop_glyph_names", false);
  any = s.unicodes.has_value() || s.drop_hinting || s.drop_layout || s.drop_names || s.drop_glyph_names;
  return s;
}

}  // namespace

Result<ir::Font> prepare_font(const ir::Font& font, OutlineTarget target, bool keep_all_faces, const WriteContext& ctx) {
  std::vector<const ir::Font*> faces = keep_all_faces ? all_faces(font) : std::vector<const ir::Font*>{&font};
  if (!keep_all_faces && !font.other_faces.empty()) {
    ctx.warn(std::format("The source holds {} fonts; only the first one was written (use ttc, woff2 or dfont to keep all).",
                         font.other_faces.size() + 1));
  }
  bool any_subset = false;
  CVT_TRY_ASSIGN(const SubsetOptions subset, subset_options(ctx.options, any_subset));
  const bool strip = options::resolved_strip_metadata(ctx.options);
  const bool same_format = !ctx.source_format_id.empty() && ctx.source_format_id == ctx.format_id;
  std::vector<ir::Font> prepared;
  for (std::size_t i = 0; i < faces.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(faces.size()), "Preparing font"));
    ir::Font face;
    if (faces[i]->bitmap && !has_outlines(*faces[i])) {
      CVT_TRY_ASSIGN(face, outline_font_from_bitmap(*faces[i]->bitmap, target == OutlineTarget::Cff, ctx));
      ctx.warn("The bitmap font was traced into outlines (one square per pixel).");
    } else if (faces[i]->tables.empty()) {
      return fail(Status::Encode, "the font has no glyph data");
    } else {
      face = *faces[i];
      face.other_faces.clear();
    }
    if (strip) strip_editor_tables(face);
    if (any_subset) {
      CVT_TRY(subset_font(face, subset, ctx));
    } else if (same_format && face.find("glyf") && target != OutlineTarget::Cff) {
      CVT_TRY(compact_glyf(face));
    }
    if (target == OutlineTarget::TrueType && !face.find("glyf")) {
      CVT_TRY_ASSIGN(face, to_truetype(face, ctx.options.get_float("curve_tolerance", 1.0, 0.05, 20.0), ctx));
      face.remove_table("DSIG");
    } else if (target == OutlineTarget::Cff && !face.find("CFF ")) {
      CVT_TRY_ASSIGN(face, to_cff(face, !ctx.options.get_bool("exact_curves", false), ctx));
      face.remove_table("DSIG");
    }
    prepared.push_back(std::move(face));
  }
  ir::Font result = std::move(prepared.front());
  for (std::size_t i = 1; i < prepared.size(); ++i) result.other_faces.push_back(std::move(prepared[i]));
  result.woff_metadata = font.woff_metadata;
  result.woff_private = strip ? Bytes{} : font.woff_private;
  const std::string metadata = ctx.options.get_string("woff_metadata", "");
  if (!metadata.empty()) result.woff_metadata = to_bytes(metadata);
  result.bitmap.reset();
  return result;
}

namespace {

// --------------------------------------------------------------------------- readers

std::int64_t face_of(const ReadContext& ctx) { return ctx.options.get_int("face", 0, 0, 4095); }

Result<AnyIR> read_sfnt_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Font font, read_sfnt(ctx.input, face_of(ctx), ctx));
  return font;
}

Result<AnyIR> read_otb(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Font font, read_sfnt(ctx.input, face_of(ctx), ctx));
  CVT_TRY_ASSIGN(ir::BitmapFont bitmap, read_embedded_bitmaps(font, ctx));
  font.bitmap = std::move(bitmap);
  return font;
}

Result<AnyIR> read_woff_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Font font, read_woff(ctx.input, ctx));
  return font;
}

Result<AnyIR> read_woff2_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Font font, read_woff2(ctx.input, face_of(ctx), ctx));
  return font;
}

Result<AnyIR> read_eot_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Font font, read_eot(ctx.input, ctx));
  return font;
}

Result<AnyIR> read_dfont_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Font font, read_resource_fonts(ctx.input, face_of(ctx), ctx));
  return font;
}

Result<AnyIR> read_cff_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const CffFont cff, parse_cff(ctx.input));
  if (cff.cff2) return fail(Status::Decode, "a bare CFF2 table cannot be read without its OpenType font");
  FontSpec spec;
  spec.family = cff.string_for(static_cast<int>(cff.top_value(3, {-1})[0]));
  if (spec.family.empty()) spec.family = cff.name.empty() ? "Untitled" : cff.name;
  spec.full_name = cff.string_for(static_cast<int>(cff.top_value(2, {-1})[0]));
  spec.postscript_name = cff.name;
  const std::string weight = cff.string_for(static_cast<int>(cff.top_value(4, {-1})[0]));
  if (!weight.empty()) spec.subfamily = weight;
  spec.copyright = cff.string_for(static_cast<int>(cff.top_value(1200, {-1})[0]));
  spec.trademark = cff.string_for(static_cast<int>(cff.top_value(1, {-1})[0]));
  if (spec.copyright.empty()) std::swap(spec.copyright, spec.trademark);
  spec.units_per_em = static_cast<std::uint16_t>(cff.units_per_em);
  spec.italic_angle = cff.top_value(1202, {0})[0];
  spec.fixed_pitch = cff.top_value(1201, {0})[0] != 0;
  const std::vector<double> bbox = cff.top_value(5);
  if (bbox.size() == 4 && bbox[3] > bbox[1]) {
    spec.ascender = static_cast<std::int16_t>(std::lround(bbox[3]));
    spec.descender = static_cast<std::int16_t>(std::lround(bbox[1]));
  }
  const std::vector<std::string> names = cff_glyph_names(ctx.input, static_cast<std::uint32_t>(cff.charstrings.size()));
  for (std::uint32_t g = 0; g < cff.charstrings.size(); ++g) {
    if ((g & 127) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(g) / static_cast<double>(cff.charstrings.size()), "Reading CFF"));
    CVT_TRY_ASSIGN(CffGlyph glyph, cff_glyph(cff, g));
    std::string name = g < names.size() && !names[g].empty() ? names[g] : std::format("glyph{:05}", g);
    if (g == 0) name = ".notdef";
    if (const auto code = unicode_from_glyph_name(name); code && g > 0) spec.cmap.try_emplace(*code, static_cast<std::uint16_t>(g));
    spec.glyph_names.push_back(std::move(name));
    spec.paths.push_back(std::move(glyph.path));
    spec.advances.push_back(glyph.width.value_or(0));
  }
  CVT_TRY_ASSIGN(ir::Font font, build_font(spec, true, ctx));
  font.set_table("CFF ", Bytes(ctx.input.begin(), ctx.input.end()));  // keep the original charstrings and hints
  return font;
}

Result<AnyIR> read_type1_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Font font, read_type1(ctx.input, ctx));
  return font;
}

Result<AnyIR> read_afm_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Json json, read_afm(ctx.input));
  ir::Data data;
  data.value = std::move(json);
  return data;
}

ir::Font bitmap_only(ir::BitmapFont bitmap) {
  ir::Font font;
  ir::FontNames names;
  for (const auto& [key, value] : bitmap.properties) {
    if (key == "FAMILY_NAME") names.family = value;
    if (key == "FULL_NAME" || key == "FACE_NAME") names.full_name = value;
  }
  if (!names.family.empty()) font.names = names;
  font.bitmap = std::move(bitmap);
  return font;
}

Result<AnyIR> read_bdf_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::BitmapFont f, read_bdf(ctx.input, ctx));
  return bitmap_only(std::move(f));
}
Result<AnyIR> read_pcf_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::BitmapFont f, read_pcf(ctx.input, ctx));
  return bitmap_only(std::move(f));
}
Result<AnyIR> read_psf_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::BitmapFont f, read_psf(ctx.input, ctx));
  return bitmap_only(std::move(f));
}
Result<AnyIR> read_fnt_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::BitmapFont f, read_fnt(ctx.input, ctx));
  return bitmap_only(std::move(f));
}
Result<AnyIR> read_fon_codec(ReadContext& ctx) {
  CVT_TRY_ASSIGN(std::vector<ir::BitmapFont> fonts, read_fon(ctx.input, ctx));
  const auto face = static_cast<std::size_t>(face_of(ctx));
  if (face >= fonts.size()) return fail(Status::InvalidArgument, "face {} does not exist; the file holds {} font(s)", face, fonts.size());
  ir::Font primary = bitmap_only(std::move(fonts[face]));
  for (std::size_t i = 0; i < fonts.size(); ++i) {
    if (i != face) primary.other_faces.push_back(bitmap_only(std::move(fonts[i])));
  }
  return primary;
}

// --------------------------------------------------------------------------- writers

int woff_level(const Options& options) {
  return options.was_set("compression_level") ? static_cast<int>(options.get_int("compression_level", 9, 0, 9)) : 9;
}

Result<Bytes> write_ttf(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::TrueType, false, ctx));
  return write_sfnt(font);
}

Result<Bytes> write_otf(const AnyIR& value, WriteContext& ctx) {
  const ir::Font& source = value.get<ir::Font>();
  // CFF2 (variable) fonts are already OpenType/CFF: keep them as they are.
  const OutlineTarget target = source.find("CFF2") ? OutlineTarget::Keep : OutlineTarget::Cff;
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(source, target, false, ctx));
  return write_sfnt(font);
}

Result<Bytes> write_ttc(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::Keep, true, ctx));
  const std::vector<const ir::Font*> faces = all_faces(font);
  return write_collection(faces);
}

Result<Bytes> write_woff_codec(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::Keep, false, ctx));
  return write_woff(font, woff_level(ctx.options), !options::resolved_strip_metadata(ctx.options), ctx);
}

Result<Bytes> write_woff2_codec(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::Keep, true, ctx));
  Woff2Options options;
  options.level = woff_level(ctx.options);
  options.transform_glyf = ctx.options.get_bool("woff2_transform", true);
  options.transform_hmtx = options.transform_glyf;
  options.keep_private = !options::resolved_strip_metadata(ctx.options);
  return write_woff2(font, options, ctx);
}

Result<Bytes> write_eot_codec(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::TrueType, false, ctx));
  EotOptions options;
  const std::string version = ctx.options.get_enum("eot_version", "2.1", {"1", "2.1", "2.2"});
  options.version = version == "1" ? 0x00010000u : version == "2.2" ? 0x00020002u : 0x00020001u;
  options.obfuscate = ctx.options.get_bool("eot_obfuscate", false);
  return write_eot(font, options);
}

Result<Bytes> write_dfont_codec(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::Keep, true, ctx));
  return write_dfont(font);
}

Result<Bytes> write_cff_codec(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::Cff, false, ctx));
  const ir::FontTable* cff = font.find("CFF ");
  if (!cff) return fail(Status::Encode, "the font has no CFF outlines");
  return cff->bytes;
}

Result<Bytes> write_pfa(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::Cff, false, ctx));
  return write_type1(font, false, ctx);
}

Result<Bytes> write_pfb(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::Cff, false, ctx));
  return write_type1(font, true, ctx);
}

Result<Bytes> write_afm_codec(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Font font, prepare_font(value.get<ir::Font>(), OutlineTarget::Keep, false, ctx));
  return write_afm(font, ctx);
}

Result<ir::BitmapFont> strike_for(const ir::Font& font, const WriteContext& ctx) {
  if (font.bitmap && (!has_outlines(font) || !ctx.options.was_set("pixel_size"))) return *font.bitmap;
  const auto size = static_cast<std::uint32_t>(ctx.options.get_int("pixel_size", 16, 4, 512));
  if (!font.other_faces.empty()) ctx.warn("Only the first font of the collection was rasterised.");
  CVT_TRY_ASSIGN(ir::BitmapFont strike, bitmap_from_outlines(font, size, ctx));
  ctx.warn(std::format("The outlines were rasterised at {} pixels per em.", size));
  return strike;
}

Result<Bytes> write_bdf_codec(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::BitmapFont strike, strike_for(value.get<ir::Font>(), ctx));
  return write_bdf(strike);
}
Result<Bytes> write_pcf_codec(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::BitmapFont strike, strike_for(value.get<ir::Font>(), ctx));
  return write_pcf(strike);
}
Result<Bytes> write_psf_codec(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::BitmapFont strike, strike_for(value.get<ir::Font>(), ctx));
  return write_psf(strike, ctx);
}

// --------------------------------------------------------------------------- detection

int sniff_sfnt(ByteView data, bool want_cff, bool want_bitmap) {
  std::uint32_t version = 0;
  const auto entries = read_directory(data, 0, &version);
  if (!entries) return 0;
  bool glyf = false, cff = false, bitmap = false;
  for (const DirectoryEntry& e : *entries) {
    if (e.tag == tag_value("glyf") && e.length > 0) glyf = true;
    if (e.tag == tag_value("CFF ") || e.tag == tag_value("CFF2")) cff = true;
    if (e.tag == tag_value("EBDT") || e.tag == tag_value("bdat")) bitmap = true;
  }
  if (want_bitmap) return bitmap && !glyf && !cff ? 97 : 0;
  if (bitmap && !glyf && !cff) return 0;
  if (want_cff) return (cff || version == tag_otto) ? 96 : 0;
  return (!cff && version != tag_otto) ? 96 : 0;
}

void add(Registry& registry, std::string id, std::string name, std::string description, std::vector<std::string> extensions,
         std::string mime, int popularity, std::function<int(std::span<const std::byte>, std::string_view)> sniff = {},
         std::vector<Signature> signatures = {}) {
  FormatInfo info;
  info.id = std::move(id);
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Font;
  info.extensions = std::move(extensions);
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.sniff = std::move(sniff);
  info.signatures = std::move(signatures);
  registry.add_format(std::move(info));
}

void codec(Registry& registry, std::string id, std::function<Result<AnyIR>(ReadContext&)> read,
           std::function<Result<Bytes>(const AnyIR&, WriteContext&)> write, std::vector<OptionSpec> write_options,
           std::vector<OptionSpec> read_options = {}, int write_cost = 10, Kind kind = Kind::Font, bool lossy = false) {
  Codec c;
  c.format_id = std::move(id);
  c.kind = kind;
  c.read = std::move(read);
  c.write = std::move(write);
  c.write_options = std::move(write_options);
  c.read_options = std::move(read_options);
  c.write_cost = write_cost;
  c.lossy = lossy;
  registry.add_codec(std::move(c));
}

}  // namespace

void register_sfnt_formats(Registry& registry) {
  add(registry, "ttf", "TrueType font", "Outline font with quadratic curves (sfnt).", {"ttf"}, "font/ttf", 80,
      [](std::span<const std::byte> d, std::string_view) { return sniff_sfnt(d, false, false); });
  add(registry, "otf", "OpenType font", "sfnt font with PostScript (CFF) outlines.", {"otf"}, "font/otf", 75,
      [](std::span<const std::byte> d, std::string_view) { return sniff_sfnt(d, true, false); });
  add(registry, "ttc", "TrueType collection", "Several sfnt fonts sharing tables in one file.", {"ttc", "otc"},
      "font/collection", 25, [](std::span<const std::byte> d, std::string_view) {
        const auto offsets = collection_offsets(d);
        return offsets && d.size() >= 4 && as_chars(d.first(4)) == "ttcf" && read_directory(d, offsets->front()) ? 97 : 0;
      });
  add(registry, "woff", "Web Open Font Format", "Compressed sfnt font for the web.", {"woff"}, "font/woff", 65);
  add(registry, "woff2", "Web Open Font Format 2", "Brotli-compressed sfnt font for the web.", {"woff2"}, "font/woff2", 70);
  add(registry, "eot", "Embedded OpenType font", "Font for old Internet Explorer.", {"eot"}, "application/vnd.ms-fontobject",
      15, [](std::span<const std::byte> d, std::string_view) { return looks_like_eot(d) ? 95 : 0; });
  add(registry, "dfont", "Mac data-fork font", "Classic Mac OS font suitcase in a data fork.", {"dfont", "suit"},
      "application/x-dfont", 4, [](std::span<const std::byte> d, std::string_view) { return looks_like_resource_fork(d) ? 92 : 0; });
  add(registry, std::string(id_cff), "Compact Font Format", "Bare CFF table: PostScript outlines without an sfnt wrapper.",
      {"cff"}, "application/font-cff", 3, [](std::span<const std::byte> d, std::string_view) {
        if (d.size() < 4 || std::to_integer<int>(d[0]) != 1 || std::to_integer<int>(d[2]) < 4) return 0;
        return parse_cff(d) ? 90 : 0;
      });
  add(registry, std::string(id_otb), "OpenType bitmap font", "sfnt font holding only embedded bitmaps (EBDT/EBLC).",
      {"otb"}, "font/otb", 3, [](std::span<const std::byte> d, std::string_view) { return sniff_sfnt(d, false, true); });

  const std::vector<OptionSpec> reader = {face_option()};
  codec(registry, "ttf", read_sfnt_codec, write_ttf, with(subset_option_specs(), {tolerance_option()}), reader);
  codec(registry, "otf", read_sfnt_codec, write_otf, with(subset_option_specs(), {exact_curves_option()}), reader);
  codec(registry, "ttc", read_sfnt_codec, write_ttc, subset_option_specs(), reader);
  codec(registry, "woff", read_woff_codec, write_woff_codec,
        with(subset_option_specs(), {options::compression_level(),
                                     OptionSpec::text("woff_metadata", "WOFF metadata", "",
                                                      "Extended metadata XML to embed (licence, vendor, description).")
                                         .advanced()}));
  codec(registry, "woff2", read_woff2_codec, write_woff2_codec,
        with(subset_option_specs(),
             {options::compression_level(),
              OptionSpec::boolean("woff2_transform", "Glyph transform", true,
                                  "Use the WOFF2 glyf/loca and hmtx transforms (smaller files).")
                  .advanced(),
              OptionSpec::text("woff_metadata", "WOFF metadata", "",
                               "Extended metadata XML to embed (licence, vendor, description).")
                  .advanced()}),
        reader);
  codec(registry, "eot", read_eot_codec, write_eot_codec,
        with(subset_option_specs(),
             {tolerance_option(),
              OptionSpec::choice("eot_version", "EOT version", "2.1", {"1", "2.1", "2.2"}, "Header version of the EOT file.")
                  .advanced(),
              OptionSpec::boolean("eot_obfuscate", "Obfuscate", false, "XOR the font data with 0x50 as EOT allows.")
                  .advanced()}));
  codec(registry, "dfont", read_dfont_codec, write_dfont_codec, subset_option_specs(), reader);
  codec(registry, std::string(id_cff), read_cff_codec, write_cff_codec, with(subset_option_specs(), {exact_curves_option()}));
  codec(registry, std::string(id_otb), read_otb, nullptr, {}, reader);
}

void register_type1_formats(Registry& registry) {
  add(registry, "pfb", "PostScript Type 1 font (binary)", "Adobe Type 1 font in PC binary segments.", {"pfb"},
      "application/x-font-type1", 15, {},
      {Signature::at(0, "80 01 ?? ?? ?? ?? '%!PS-AdobeFont'"), Signature::at(0, "80 01 ?? ?? ?? ?? '%!FontType1'")});
  add(registry, "pfa", "PostScript Type 1 font (ASCII)", "Adobe Type 1 font as PostScript text.", {"pfa"},
      "application/x-font-type1", 5);
  add(registry, "afm", "Adobe font metrics", "Glyph metrics of a PostScript font.", {"afm"}, "application/x-font-afm", 5);
  const std::vector<OptionSpec> type1_options = with(subset_option_specs(), {exact_curves_option()});
  codec(registry, "pfb", read_type1_codec, write_pfb, type1_options, {}, 20);
  codec(registry, "pfa", read_type1_codec, write_pfa, type1_options, {}, 20);
  codec(registry, "afm", read_afm_codec, nullptr, {}, {}, 10, Kind::Data);
  codec(registry, "afm", nullptr, write_afm_codec, subset_option_specs(), {}, 20, Kind::Font, true);
}

void register_bitmap_formats(Registry& registry) {
  add(registry, "bdf", "Bitmap Distribution Format font", "X11 bitmap font (text).", {"bdf"}, "application/x-font-bdf", 4);
  add(registry, "pcf", "Portable Compiled Format font", "X11 compiled bitmap font.", {"pcf"}, "application/x-font-pcf", 3);
  add(registry, "psf", "PC screen font", "Linux console bitmap font.", {"psf", "psfu"}, "application/x-font-linux-psf", 3);
  add(registry, "fnt", "Windows font resource", "Raw Windows bitmap font.", {"fnt"}, "application/x-font-fnt", 2,
      [](std::span<const std::byte> d, std::string_view) { return looks_like_fnt(d) ? 80 : 0; });
  add(registry, "fon", "Windows bitmap font", "Windows raster font resource (NE executable).", {"fon"},
      "application/x-font-fon", 4, [](std::span<const std::byte> d, std::string_view) { return looks_like_fon(d) ? 93 : 0; });
  const std::vector<OptionSpec> bitmap_write = {
      OptionSpec::integer("pixel_size", "Pixel size", 16, 4, 512,
                          "Pixels per em used when an outline font is rasterised into a bitmap font.")};
  codec(registry, "bdf", read_bdf_codec, write_bdf_codec, bitmap_write, {}, 20, Kind::Font, true);
  codec(registry, "pcf", read_pcf_codec, write_pcf_codec, bitmap_write, {}, 20, Kind::Font, true);
  codec(registry, "psf", read_psf_codec, write_psf_codec, bitmap_write, {}, 25, Kind::Font, true);
  codec(registry, "fnt", read_fnt_codec, nullptr, {});
  codec(registry, "fon", read_fon_codec, nullptr, {}, {face_option()});
}

}  // namespace cvt::font
