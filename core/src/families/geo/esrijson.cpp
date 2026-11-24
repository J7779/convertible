// Convertible core - geo family: Esri JSON (ArcGIS REST feature sets).
//
// Reading: FeatureSets ({geometryType, spatialReference, fields, features}),
// single features ({attributes, geometry}) and bare geometries: points
// (x, y, z, m), multipoints, polylines (paths) and polygons (rings, whose
// orientation tells outer boundaries - clockwise - from holes). The
// spatialReference (wkid / latestWkid / wkt) becomes the CRS. Writing: a
// FeatureSet with fields inferred from the properties, rings clockwise,
// and the spatial reference of the output CRS.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "families/geo/geo.hpp"
#include "families/geo/geo_ops.hpp"
#include "util/json.hpp"

namespace cvt::geo {

namespace {

Result<ir::Coord> esri_position(const Json& p, bool has_z, bool has_m) {
  if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) {
    return fail(Status::Decode, "an Esri JSON position needs two numbers");
  }
  ir::Coord c;
  c.lon = p[0].as_double();
  c.lat = p[1].as_double();
  std::size_t next = 2;
  if (has_z && p.size() > next && p[next].is_number()) c.ele = p[next++].as_double();
  if (has_m && p.size() > next && p[next].is_number()) c.m = p[next].as_double();
  if (!has_z && !has_m && p.size() >= 3 && p[2].is_number()) c.ele = p[2].as_double();
  return c;
}

Result<std::vector<ir::Coord>> esri_path(const Json& path, bool z, bool m) {
  if (!path.is_array()) return fail(Status::Decode, "an Esri JSON path or ring must be an array");
  std::vector<ir::Coord> out;
  out.reserve(path.size());
  for (const Json& p : path.elements()) {
    CVT_TRY_ASSIGN(ir::Coord c, esri_position(p, z, m));
    out.push_back(c);
  }
  return out;
}

Result<ir::Geometry> esri_geometry(const Json& g) {
  ir::Geometry out;
  if (!g.is_object()) {
    out.shape = ir::GeometryCollection{};
    return out;
  }
  const bool z = g["hasZ"].as_bool(false);
  const bool m = g["hasM"].as_bool(false);
  if (const Json* x = g.find("x")) {
    if (!x->is_number() || !g["y"].is_number()) {
      out.shape = ir::MultiPoint{};  // "x": "NaN" / null marks an empty point
      return out;
    }
    ir::Coord c;
    c.lon = x->as_double();
    c.lat = g["y"].as_double();
    if (g["z"].is_number()) c.ele = g["z"].as_double();
    if (g["m"].is_number()) c.m = g["m"].as_double();
    out.shape = ir::Point{c};
  } else if (const Json* points = g.find("points")) {
    CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, esri_path(*points, z, m));
    out.shape = ir::MultiPoint{std::move(pts)};
  } else if (const Json* paths = g.find("paths")) {
    ir::MultiLineString ml;
    for (const Json& path : paths->elements()) {
      CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, esri_path(path, z, m));
      ml.lines.push_back(ir::LineString{std::move(pts)});
    }
    if (ml.lines.size() == 1) {
      out.shape = std::move(ml.lines.front());
    } else {
      out.shape = std::move(ml);
    }
  } else if (const Json* rings = g.find("rings")) {
    std::vector<std::vector<ir::Coord>> all;
    for (const Json& ring : rings->elements()) {
      CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, esri_path(ring, z, m));
      all.push_back(std::move(pts));
    }
    out = assemble_rings(std::move(all), true);
  } else {
    out.shape = ir::GeometryCollection{};
  }
  return out;
}

std::string crs_of(const Json& sr) {
  if (!sr.is_object()) return {};
  for (std::string_view key : {"latestWkid", "wkid"}) {
    if (const Json* id = sr.find(key); id != nullptr && id->is_number()) {
      return "EPSG:" + std::to_string(id->as_int64());
    }
  }
  if (const Json* wkt = sr.find("wkt"); wkt != nullptr && wkt->is_string()) return wkt->as_string();
  return {};
}

Result<ir::Geo> read_esrijson(ReadContext& ctx) {
  util::JsonParseOptions options;
  options.max_depth = 256;
  auto root = util::parse_json(input_text(ctx.input), options);
  if (!root) return fail(Status::Decode, "not valid Esri JSON: {}", root.error().message);
  if (!root->is_object()) return fail(Status::Decode, "Esri JSON must be an object");
  ir::Geo geo;
  geo.crs = crs_of((*root)["spatialReference"]);
  const auto add = [&](const Json& feature) -> Result<void> {
    ir::Feature f;
    CVT_TRY_ASSIGN(f.geometry, esri_geometry(feature["geometry"]));
    if (const Json* a = feature.find("attributes"); a != nullptr && a->is_object()) f.properties = *a;
    if (geo.crs.empty()) geo.crs = crs_of(feature["geometry"]["spatialReference"]);
    if (const Json* name = f.properties.find("name"); name != nullptr && name->is_string()) f.name = name->as_string();
    geo.features.push_back(std::move(f));
    return {};
  };
  if (const Json* features = root->find("features"); features != nullptr && features->is_array()) {
    std::size_t i = 0;
    for (const Json& f : features->elements()) {
      if ((++i & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(features->size())));
      CVT_TRY(add(f));
    }
    if (const Json* name = root->find("displayFieldName"); name != nullptr && name->is_string()) {
      geo.extra.set("displayFieldName", *name);
    }
  } else if (root->contains("geometry") || root->contains("attributes")) {
    CVT_TRY(add(*root));
  } else if (root->contains("x") || root->contains("points") || root->contains("paths") || root->contains("rings")) {
    ir::Feature f;
    CVT_TRY_ASSIGN(f.geometry, esri_geometry(*root));
    geo.features.push_back(std::move(f));
  } else {
    return fail(Status::Decode, "not an Esri JSON feature set, feature or geometry");
  }
  return geo;
}

// --------------------------------------------------------------------------- writing

std::string_view geometry_type(const ir::Geometry& g) {
  switch (g.shape.index()) {
    case 0: return "esriGeometryPoint";
    case 3: return "esriGeometryMultipoint";
    case 1:
    case 4: return "esriGeometryPolyline";
    case 2:
    case 5: return "esriGeometryPolygon";
    default: return {};
  }
}

Json esri_point(const ir::Coord& c, bool z) {
  Json p = Json::array({c.lon, c.lat});
  if (z) p.push_back(c.ele.value_or(0.0));
  return p;
}

Json esri_seq(const std::vector<ir::Coord>& pts, bool z) {
  Json out = Json::array();
  for (const auto& c : pts) out.push_back(esri_point(c, z));
  return out;
}

Json esri_geometry_json(const ir::Geometry& source) {
  bool z = false;
  for_each_coord(source, [&](const ir::Coord& c) { z = z || c.ele.has_value(); });
  ir::Geometry g = source;
  orient_rings(g, false);  // Esri: outer rings clockwise
  Json out = Json::object();
  if (const auto* p = std::get_if<ir::Point>(&g.shape)) {
    out.set("x", p->coord.lon);
    out.set("y", p->coord.lat);
    if (p->coord.ele) out.set("z", *p->coord.ele);
    return out;
  }
  if (z) out.set("hasZ", true);
  if (const auto* mp = std::get_if<ir::MultiPoint>(&g.shape)) {
    out.set("points", esri_seq(mp->points, z));
  } else if (const auto* l = std::get_if<ir::LineString>(&g.shape)) {
    out.set("paths", Json::array({esri_seq(l->points, z)}));
  } else if (const auto* ml = std::get_if<ir::MultiLineString>(&g.shape)) {
    Json paths = Json::array();
    for (const auto& line : ml->lines) paths.push_back(esri_seq(line.points, z));
    out.set("paths", std::move(paths));
  } else if (const auto* poly = std::get_if<ir::Polygon>(&g.shape)) {
    Json rings = Json::array();
    for (const auto& r : poly->rings) rings.push_back(esri_seq(r, z));
    out.set("rings", std::move(rings));
  } else if (const auto* mpoly = std::get_if<ir::MultiPolygon>(&g.shape)) {
    Json rings = Json::array();
    for (const auto& p : mpoly->polygons) {
      for (const auto& r : p.rings) rings.push_back(esri_seq(r, z));
    }
    out.set("rings", std::move(rings));
  }
  return out;
}

Result<Bytes> write_esrijson(const ir::Geo& geo, WriteContext& ctx) {
  std::string_view type;
  bool mixed = false, collections = false;
  for (const auto& f : geo.features) {
    const std::string_view t = geometry_type(f.geometry);
    if (t.empty()) {
      collections = collections || !geometry_empty(f.geometry);
      continue;
    }
    if (type.empty()) type = t;
    mixed = mixed || t != type;
  }
  if (mixed) ctx.warn("Esri feature sets hold one geometry type; the set mixes types, which some programs reject.");
  if (collections) ctx.warn("Esri JSON has no geometry collections; such features were written without geometry.");
  // Fields from the property types.
  Json fields = Json::array();
  std::vector<std::string> seen;
  for (const auto& f : geo.features) {
    for (const auto& m : f.properties.items()) {
      const std::string key(m.key());
      if (std::find(seen.begin(), seen.end(), key) != seen.end() || m.value().is_null()) continue;
      seen.push_back(key);
      std::string_view t = "esriFieldTypeString";
      if (m.value().is_integer()) t = "esriFieldTypeInteger";
      else if (m.value().is_float()) t = "esriFieldTypeDouble";
      else if (m.value().is_bool()) t = "esriFieldTypeSmallInteger";
      fields.push_back(Json::object({{"name", key}, {"type", t}, {"alias", key}}));
    }
  }
  Json features = Json::array();
  for (const auto& f : geo.features) {
    Json attributes = Json::object();
    for (const auto& m : f.properties.items()) {
      const Json& v = m.value();
      if (v.is_bool()) attributes.set(m.key(), v.as_bool() ? 1 : 0);
      else if (v.is_structured()) attributes.set(m.key(), util::dump_json(v));
      else attributes.set(m.key(), v);
    }
    Json feature = Json::object({{"attributes", std::move(attributes)}});
    if (!geometry_type(f.geometry).empty()) feature.set("geometry", esri_geometry_json(f.geometry));
    features.push_back(std::move(feature));
  }
  int wkid = 4326;
  if (const auto crs = parse_crs(geo.crs)) wkid = crs->epsg;
  Json root = Json::object();
  if (!type.empty()) root.set("geometryType", type);
  root.set("spatialReference", Json::object({{"wkid", wkid}, {"latestWkid", wkid}}));
  root.set("fields", std::move(fields));
  root.set("features", std::move(features));
  return to_bytes(util::dump_json(root) + "\n");
}

int sniff_esrijson(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 16384);
  if (!head.starts_with('{')) return 0;
  const bool esri_type = head.find("\"esriGeometry") != std::string_view::npos;
  const bool sr = head.find("\"spatialReference\"") != std::string_view::npos;
  const bool attributes = head.find("\"attributes\"") != std::string_view::npos;
  const bool rings = head.find("\"rings\"") != std::string_view::npos || head.find("\"paths\"") != std::string_view::npos;
  if (esri_type || (sr && (attributes || rings))) return 93;
  return 0;
}

}  // namespace

void register_esrijson(Registry& registry) {
  FormatInfo info;
  info.id = "esrijson";
  info.name = "Esri JSON";
  info.description = "ArcGIS REST API feature sets (attributes and geometries as Esri JSON).";
  info.category = Category::Geo;
  info.extensions = {"esrijson", "json"};
  info.mime = "application/json";
  info.popularity = 30;
  info.sniff = sniff_esrijson;
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "esrijson";
  spec.read = read_esrijson;
  spec.write = write_esrijson;
  spec.traits = GeoWriterTraits{true, false, false, false};
  spec.lossy_write = true;
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
