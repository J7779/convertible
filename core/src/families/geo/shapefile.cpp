// Convertible core - geo family: Esri shapefiles as single-file exchanges.
//
// `shp.zip`: a ZIP holding one or more shapefile layers (.shp with its .shx,
// .dbf, .prj and .cpg). Every 2D, Z and M shape type is read, MultiPatch as
// polygons (triangle strips and fans become triangles); polygon rings are
// grouped into outer rings (clockwise) and their holes by containment.
// Attributes come from the .dbf (code page from the .cpg or the language
// byte), the coordinate system from the .prj (converted to WGS 84 when it
// is one Convertible knows). Writing produces one layer per geometry type
// (points, multipoints, lines, polygons) with .shx, .dbf, .prj and .cpg.
// `shp`: a lone .shp file, read as geometry only.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/geo/dbf.hpp"
#include "families/geo/geo.hpp"
#include "families/geo/geo_ops.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::geo {

namespace {

constexpr double no_data = -1e38;

// --------------------------------------------------------------------------- reading

struct ShapeReader {
  util::ByteReader r;
  std::size_t end = 0;  // end of the record content

  bool has(std::size_t bytes) const { return r.position() + bytes <= end; }
  ir::Coord xy() {
    ir::Coord c;
    c.lon = r.f64le();
    c.lat = r.f64le();
    return c;
  }
  // Optional Z / M arrays after the XY points.
  void z_values(std::vector<ir::Coord>& pts) {
    if (!has(16 + 8 * pts.size())) return;
    r.skip(16);
    for (ir::Coord& c : pts) c.ele = r.f64le();
  }
  void m_values(std::vector<ir::Coord>& pts) {
    if (!has(16 + 8 * pts.size())) return;
    r.skip(16);
    for (ir::Coord& c : pts) {
      const double m = r.f64le();
      if (m > no_data) c.m = m;
    }
  }
};

Result<ir::Geometry> read_shape(ByteView content, std::string& error) {
  ShapeReader s{util::ByteReader(content), content.size()};
  util::ByteReader& r = s.r;
  const std::uint32_t type = r.u32le();
  if (!r.ok()) return fail(Status::Decode, "truncated shapefile record");
  ir::Geometry g;
  const bool z = type == 11 || type == 13 || type == 15 || type == 18 || type == 31;
  const bool m = type == 21 || type == 23 || type == 25 || type == 28;
  switch (type) {
    case 0: g.shape = ir::GeometryCollection{}; return g;
    case 1:
    case 11:
    case 21: {
      ir::Coord c = s.xy();
      if (z && s.has(8)) c.ele = r.f64le();
      if ((z || m) && s.has(8)) {
        const double v = r.f64le();
        if (v > no_data) c.m = v;
      }
      if (!r.ok()) return fail(Status::Decode, "truncated shapefile point");
      g.shape = ir::Point{c};
      return g;
    }
    case 8:
    case 18:
    case 28: {
      r.skip(32);
      const std::uint32_t n = r.u32le();
      CVT_TRY(check_declared(n, 16, r.remaining(), "shapefile points"));
      std::vector<ir::Coord> pts;
      pts.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i) pts.push_back(s.xy());
      if (z) s.z_values(pts);
      if (z || m) s.m_values(pts);
      if (!r.ok()) return fail(Status::Decode, "truncated shapefile multipoint");
      g.shape = ir::MultiPoint{std::move(pts)};
      return g;
    }
    case 3:
    case 13:
    case 23:
    case 5:
    case 15:
    case 25:
    case 31: {
      r.skip(32);
      const std::uint32_t parts = r.u32le();
      const std::uint32_t n = r.u32le();
      CVT_TRY(check_declared(parts, 4, r.remaining(), "shapefile parts"));
      std::vector<std::uint32_t> starts(parts);
      for (auto& p : starts) p = r.u32le();
      std::vector<std::uint32_t> part_types;
      if (type == 31) {
        CVT_TRY(check_declared(parts, 4, r.remaining(), "shapefile part types"));
        part_types.resize(parts);
        for (auto& t : part_types) t = r.u32le();
      }
      CVT_TRY(check_declared(n, 16, r.remaining(), "shapefile points"));
      std::vector<ir::Coord> pts;
      pts.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i) pts.push_back(s.xy());
      if (z) s.z_values(pts);
      if (z || m) s.m_values(pts);
      if (!r.ok()) return fail(Status::Decode, "truncated shapefile shape");
      std::vector<std::vector<ir::Coord>> sequences;
      for (std::uint32_t p = 0; p < parts; ++p) {
        const std::uint32_t from = starts[p];
        const std::uint32_t to = p + 1 < parts ? starts[p + 1] : n;
        if (from > to || to > n) return fail(Status::Decode, "a shapefile part index is out of range");
        sequences.emplace_back(pts.begin() + from, pts.begin() + to);
      }
      if (type == 3 || type == 13 || type == 23) {
        if (sequences.size() == 1) {
          g.shape = ir::LineString{std::move(sequences.front())};
        } else {
          ir::MultiLineString ml;
          for (auto& seq : sequences) ml.lines.push_back(ir::LineString{std::move(seq)});
          g.shape = std::move(ml);
        }
        return g;
      }
      if (type != 31) return assemble_rings(std::move(sequences), true);
      // MultiPatch: triangle strips (0) and fans (1) become triangles; rings
      // (2 outer, 3 inner, 4 first, 5 ring) become polygons.
      ir::MultiPolygon mp;
      for (std::size_t p = 0; p < sequences.size(); ++p) {
        const auto& seq = sequences[p];
        const std::uint32_t pt = part_types[p];
        if (pt == 0 || pt == 1) {
          for (std::size_t i = 2; i < seq.size(); ++i) {
            const ir::Coord& a = pt == 0 ? seq[i - 2] : seq[0];
            std::vector<ir::Coord> tri = {a, seq[i - 1], seq[i], a};
            mp.polygons.push_back(ir::Polygon{{std::move(tri)}});
          }
        } else if (pt == 3 && !mp.polygons.empty()) {
          mp.polygons.back().rings.push_back(seq);
        } else if (pt <= 5) {
          mp.polygons.push_back(ir::Polygon{{seq}});
        } else {
          error = "unknown MultiPatch part type";
        }
      }
      for (auto& poly : mp.polygons) {
        for (auto& ring : poly.rings) {
          if (!ring.empty() && !(ring.front().lon == ring.back().lon && ring.front().lat == ring.back().lat)) {
            ring.push_back(ring.front());
          }
        }
      }
      g.shape = std::move(mp);
      orient_rings(g, true);
      return g;
    }
    default: return fail(Status::Decode, "unsupported shapefile shape type {}", type);
  }
}

Result<std::vector<ir::Geometry>> read_shp(ByteView shp, const Context& ctx) {
  util::ByteReader r(shp);
  const std::uint32_t code = r.u32be();
  r.seek(24);
  const std::uint64_t declared = std::uint64_t{r.u32be()} * 2;
  const std::uint32_t version = r.u32le();
  if (!r.ok() || code != 9994 || version != 1000) return fail(Status::Decode, "not a shapefile (.shp)");
  const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(declared, shp.size()));
  if (declared > shp.size()) ctx.warn("The shapefile is shorter than its header says; the shapes present were read.");
  std::vector<ir::Geometry> out;
  std::size_t pos = 100;
  std::string error;
  while (pos + 8 <= length) {
    if ((out.size() & 1023) == 1023) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(length)));
    util::ByteReader h(shp.subspan(pos, 8));
    (void)h.u32be();  // record number
    const std::uint64_t content = std::uint64_t{h.u32be()} * 2;
    if (pos + 8 + content > length) return fail(Status::Decode, "a shapefile record runs past the end of the file");
    CVT_TRY_ASSIGN(ir::Geometry g, read_shape(shp.subspan(pos + 8, static_cast<std::size_t>(content)), error));
    out.push_back(std::move(g));
    pos += 8 + static_cast<std::size_t>(content);
  }
  if (!error.empty()) ctx.warn("Some MultiPatch parts of an unknown type were skipped.");
  return out;
}

struct Layer {
  std::string stem;
  std::optional<std::size_t> shp, dbf, prj, cpg;
};

std::string lower_extension(std::string_view name, std::string& stem) {
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos) return {};
  stem = std::string(name.substr(0, dot));
  return util::to_lower(name.substr(dot + 1));
}

Result<ir::Geo> read_shp_zip(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
  std::map<std::string, Layer, std::less<>> layers;  // key: lower-case stem
  std::vector<std::string> order;
  for (std::size_t i = 0; i < zip.size(); ++i) {
    const auto& e = zip.entries()[i];
    if (e.is_directory || e.name.starts_with("__MACOSX/") || e.name.find("/._") != std::string::npos ||
        e.name.starts_with("._")) {
      continue;
    }
    std::string stem;
    const std::string ext = lower_extension(e.name, stem);
    const std::string key = util::to_lower(stem);
    Layer& layer = layers[key];
    if (layer.stem.empty()) layer.stem = stem;
    if (ext == "shp") {
      layer.shp = i;
      order.push_back(key);
    } else if (ext == "dbf") {
      layer.dbf = i;
    } else if (ext == "prj") {
      layer.prj = i;
    } else if (ext == "cpg") {
      layer.cpg = i;
    }
  }
  if (order.empty()) return fail(Status::Decode, "the ZIP archive contains no shapefile (.shp)");
  ir::Geo geo;
  std::optional<std::string> crs;
  bool mixed_crs = false;
  for (const std::string& key : order) {
    const Layer& layer = layers[key];
    CVT_TRY_ASSIGN(const Bytes shp, zip.read(*layer.shp));
    CVT_TRY_ASSIGN(std::vector<ir::Geometry> shapes, read_shp(shp, ctx));
    dbf::Table table;
    if (layer.dbf) {
      std::optional<util::TextEncoding> encoding;
      if (layer.cpg) {
        CVT_TRY_ASSIGN(const Bytes cpg, zip.read(*layer.cpg));
        encoding = dbf::encoding_from_cpg(as_chars(cpg));
      }
      CVT_TRY_ASSIGN(const Bytes dbf_bytes, zip.read(*layer.dbf));
      CVT_TRY_ASSIGN(table, dbf::read(dbf_bytes, encoding, ctx));
    } else {
      ctx.warn("A shapefile layer has no .dbf file; its features have no attributes.");
    }
    std::string layer_crs;
    if (layer.prj) {
      CVT_TRY_ASSIGN(const Bytes prj, zip.read(*layer.prj));
      layer_crs = std::string(util::trim(as_chars(prj)));
    }
    if (crs && *crs != layer_crs) mixed_crs = true;
    crs = layer_crs;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
      ir::Feature f;
      f.geometry = std::move(shapes[i]);
      if (i < table.records.size()) f.properties = std::move(table.records[i]);
      if (const Json* name = f.properties.find("name"); name != nullptr && name->is_string()) f.name = name->as_string();
      if (order.size() > 1) f.folder = layer.stem;
      geo.features.push_back(std::move(f));
    }
  }
  if (mixed_crs) return fail(Status::Decode, "the shapefile layers use different coordinate systems");
  geo.crs = crs.value_or("");
  if (order.size() == 1) geo.name = layers[order.front()].stem;
  return geo;
}

Result<ir::Geo> read_lone_shp(ReadContext& ctx) {
  CVT_TRY_ASSIGN(std::vector<ir::Geometry> shapes, read_shp(ctx.input, ctx));
  ctx.warn("A .shp file alone holds only shapes: the attributes (.dbf) and coordinate system (.prj) are separate "
           "files. Convert a ZIP of all the files to keep them.");
  ir::Geo geo;
  for (auto& s : shapes) {
    ir::Feature f;
    f.geometry = std::move(s);
    geo.features.push_back(std::move(f));
  }
  return geo;
}

// --------------------------------------------------------------------------- writing

enum class LayerKind { Point, MultiPoint, Line, Polygon };

struct ShpWriter {
  LayerKind kind;
  bool z = false;
  util::ByteWriter shp, shx;
  BoundingBox box;
  double zmin = 0, zmax = 0, mmin = 0, mmax = 0;
  bool first_z = true, first_m = true;
  std::uint32_t records = 0;

  std::uint32_t type() const {
    switch (kind) {
      case LayerKind::Point: return z ? 11 : 1;
      case LayerKind::MultiPoint: return z ? 18 : 8;
      case LayerKind::Line: return z ? 13 : 3;
      case LayerKind::Polygon: return z ? 15 : 5;
    }
    return 0;
  }
  void track(const ir::Coord& c) {
    box.add(c);
    const double e = c.ele.value_or(0.0);
    zmin = first_z ? e : std::min(zmin, e);
    zmax = first_z ? e : std::max(zmax, e);
    first_z = false;
    const double m = c.m.value_or(0.0);
    mmin = first_m ? m : std::min(mmin, m);
    mmax = first_m ? m : std::max(mmax, m);
    first_m = false;
  }
  static void bbox(util::ByteWriter& w, const BoundingBox& b) {
    w.f64le(b.empty ? 0 : b.min_lon).f64le(b.empty ? 0 : b.min_lat).f64le(b.empty ? 0 : b.max_lon).f64le(b.empty ? 0 : b.max_lat);
  }
  void record(const Bytes& content) {
    ++records;
    shx.u32be(static_cast<std::uint32_t>((100 + shp.size()) / 2)).u32be(static_cast<std::uint32_t>(content.size() / 2));
    shp.u32be(records).u32be(static_cast<std::uint32_t>(content.size() / 2));
    shp.bytes(content);
  }
  void add(const std::vector<std::vector<ir::Coord>>& parts) {
    util::ByteWriter c;
    c.u32le(type());
    std::vector<ir::Coord> all;
    for (const auto& p : parts) all.insert(all.end(), p.begin(), p.end());
    BoundingBox b;
    double lz = 0, hz = 0, lm = 0, hm = 0;
    bool first = true;
    for (const ir::Coord& pt : all) {
      b.add(pt);
      track(pt);
      const double e = pt.ele.value_or(0.0), m = pt.m.value_or(0.0);
      lz = first ? e : std::min(lz, e);
      hz = first ? e : std::max(hz, e);
      lm = first ? m : std::min(lm, m);
      hm = first ? m : std::max(hm, m);
      first = false;
    }
    if (kind == LayerKind::Point) {
      const ir::Coord& pt = all.front();
      c.f64le(pt.lon).f64le(pt.lat);
      if (z) c.f64le(pt.ele.value_or(0.0)).f64le(pt.m.value_or(0.0));
      record(c.take());
      return;
    }
    bbox(c, b);
    if (kind != LayerKind::MultiPoint) c.u32le(static_cast<std::uint32_t>(parts.size()));
    c.u32le(static_cast<std::uint32_t>(all.size()));
    if (kind != LayerKind::MultiPoint) {
      std::uint32_t start = 0;
      for (const auto& p : parts) {
        c.u32le(start);
        start += static_cast<std::uint32_t>(p.size());
      }
    }
    for (const ir::Coord& pt : all) c.f64le(pt.lon).f64le(pt.lat);
    if (z) {
      c.f64le(lz).f64le(hz);
      for (const ir::Coord& pt : all) c.f64le(pt.ele.value_or(0.0));
      c.f64le(lm).f64le(hm);
      for (const ir::Coord& pt : all) c.f64le(pt.m.value_or(0.0));
    }
    record(c.take());
  }
  void add_null() { record(Bytes{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}); }

  static Bytes header(std::uint32_t type, std::size_t file_bytes, const BoundingBox& b, double zl, double zh, double ml,
                      double mh) {
    util::ByteWriter h;
    h.u32be(9994).zeros(20).u32be(static_cast<std::uint32_t>(file_bytes / 2)).u32le(1000).u32le(type);
    bbox(h, b);
    h.f64le(zl).f64le(zh).f64le(ml).f64le(mh);
    return h.take();
  }
  Bytes shp_file() const {
    Bytes out = header(type(), 100 + shp.size(), box, zmin, zmax, mmin, mmax);
    out.insert(out.end(), shp.data().begin(), shp.data().end());
    return out;
  }
  Bytes shx_file() const {
    Bytes out = header(type(), 100 + shx.size(), box, zmin, zmax, mmin, mmax);
    out.insert(out.end(), shx.data().begin(), shx.data().end());
    return out;
  }
};

struct Pending {
  std::vector<std::vector<std::vector<ir::Coord>>> shapes;  // per record: parts
  std::vector<const Json*> rows;
  bool z = false;
};

void collect(const ir::Geometry& g, const Json& props, std::map<LayerKind, Pending>& layers, bool& split, int depth) {
  if (depth > ir::max_geometry_depth) return;
  const auto add = [&](LayerKind kind, std::vector<std::vector<ir::Coord>> parts) {
    Pending& p = layers[kind];
    for (const auto& part : parts) {
      for (const auto& c : part) p.z = p.z || c.ele.has_value() || c.m.has_value();
    }
    p.shapes.push_back(std::move(parts));
    p.rows.push_back(&props);
  };
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ir::Point>) {
          add(LayerKind::Point, {{s.coord}});
        } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
          if (!s.points.empty()) add(LayerKind::MultiPoint, {s.points});
        } else if constexpr (std::is_same_v<T, ir::LineString>) {
          if (!s.points.empty()) add(LayerKind::Line, {s.points});
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          std::vector<std::vector<ir::Coord>> parts;
          for (const auto& l : s.lines) parts.push_back(l.points);
          if (!parts.empty()) add(LayerKind::Line, std::move(parts));
        } else if constexpr (std::is_same_v<T, ir::Polygon> || std::is_same_v<T, ir::MultiPolygon>) {
          ir::Geometry oriented{s};
          orient_rings(oriented, false);  // shapefile: outer rings clockwise
          std::vector<std::vector<ir::Coord>> parts;
          if (const auto* p = std::get_if<ir::Polygon>(&oriented.shape)) {
            parts = p->rings;
          } else {
            for (const auto& poly : std::get<ir::MultiPolygon>(oriented.shape).polygons) {
              for (const auto& r : poly.rings) parts.push_back(r);
            }
          }
          if (!parts.empty()) add(LayerKind::Polygon, std::move(parts));
        } else {
          split = true;
          for (const auto& inner : s.geometries) collect(inner, props, layers, split, depth + 1);
        }
      },
      g.shape);
}

std::string safe_stem(std::string_view name) {
  std::string out;
  for (char c : name) {
    if (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '-') out += c;
    else if (c == ' ' || c == '.') out += '_';
    if (out.size() >= 48) break;
  }
  return out.empty() ? "layer" : out;
}

Result<Bytes> write_shp_zip(const ir::Geo& geo, WriteContext& ctx) {
  std::map<LayerKind, Pending> layers;
  bool split = false;
  std::size_t empty = 0;
  for (const ir::Feature& f : geo.features) {
    if (geometry_empty(f.geometry)) {
      ++empty;
      continue;
    }
    collect(f.geometry, f.properties, layers, split, 0);
  }
  if (split) ctx.warn("Geometry collections were split: each member went to the layer of its type.");
  if (empty > 0) ctx.warn("Features without geometry were left out.");
  if (layers.empty()) return fail(Status::Encode, "there is no geometry to write as a shapefile");
  const bool keep_z = ctx.options.get_bool("shp_z", true);
  const auto parsed_crs = parse_crs(geo.crs);
  const std::string prj = parsed_crs ? crs_esri_wkt(*parsed_crs) : geo.crs;
  const std::string base = safe_stem(geo.name.empty() ? std::string("layer") : geo.name);
  util::ZipWriter zip;
  util::ZipEntryOptions zip_options;
  zip_options.level = options::resolved_compression_level(ctx.options);
  for (auto& [kind, pending] : layers) {
    ShpWriter w;
    w.kind = kind;
    w.z = pending.z && keep_z;
    for (auto& shape : pending.shapes) w.add(shape);
    std::string stem = base;
    if (layers.size() > 1) {
      static constexpr std::string_view suffix[] = {"points", "multipoints", "lines", "polygons"};
      stem += "_" + std::string(suffix[static_cast<int>(kind)]);
    }
    CVT_TRY_ASSIGN(const Bytes dbf_bytes, dbf::write(pending.rows, ctx));
    CVT_TRY(zip.add(stem + ".shp", w.shp_file(), zip_options));
    CVT_TRY(zip.add(stem + ".shx", w.shx_file(), zip_options));
    CVT_TRY(zip.add(stem + ".dbf", dbf_bytes, zip_options));
    if (!prj.empty()) CVT_TRY(zip.add(stem + ".prj", prj, zip_options));
    CVT_TRY(zip.add(stem + ".cpg", std::string_view("UTF-8"), zip_options));
  }
  return zip.finish();
}

int sniff_shp_zip(ByteView data, std::string_view) {
  return zip_has_entry_with_extension(data, ".shp") ? 96 : 0;
}

}  // namespace

void register_shapefile(Registry& registry) {
  FormatInfo info;
  info.id = "shp.zip";
  info.name = "Zipped shapefile";
  info.description = "Esri shapefile layers (.shp, .shx, .dbf, .prj, .cpg) in a ZIP archive.";
  info.category = Category::Geo;
  info.extensions = {"shp.zip", "zip"};
  info.mime = "application/x-shapefile+zip";
  info.popularity = 60;
  info.sniff = sniff_shp_zip;
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "shp.zip";
  spec.read = read_shp_zip;
  spec.write = write_shp_zip;
  spec.traits = GeoWriterTraits{true, false, false, false};
  spec.write_options = {
      OptionSpec::boolean("shp_z", "Elevation (Z) shapes", true,
                          "Write PointZ / PolylineZ / PolygonZ shapes when the data has elevations."),
      options::compression_level(),
  };
  add_geo_codec(registry, std::move(spec));

  FormatInfo shp;
  shp.id = "shp";
  shp.name = "Shapefile geometry";
  shp.description = "The .shp part of an Esri shapefile on its own (geometry without attributes).";
  shp.category = Category::Geo;
  shp.extensions = {"shp"};
  shp.mime = "application/x-esri-shape";
  shp.popularity = 40;
  shp.signatures = {Signature::at(0, "00 00 27 0A")};
  registry.add_format(std::move(shp));
  GeoCodecSpec lone;
  lone.format_id = "shp";
  lone.read = read_lone_shp;
  add_geo_codec(registry, std::move(lone));
}

}  // namespace cvt::geo
