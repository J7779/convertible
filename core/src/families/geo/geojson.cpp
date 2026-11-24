// Convertible core - geo family: GeoJSON (RFC 7946) and GeoJSON text
// sequences (RFC 8142 and newline-delimited GeoJSON).
//
// Reading accepts FeatureCollections, single Features and bare geometries,
// every geometry type including nested GeometryCollections (depth-limited),
// the pre-RFC `crs` member (projected data is converted to WGS 84), foreign
// members (kept in `extra`), simplestyle-spec properties (kept as the
// feature style) and the togeojson conventions for per-point data
// (`properties.coordinateProperties`, `coordTimes`, `_gpxType`).
// Writing follows RFC 7946: WGS 84, counter-clockwise outer rings, features
// crossing the antimeridian split, optional bbox members.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "util/text.hpp"

namespace cvt::geo {

namespace {

using namespace std::string_view_literals;

// --------------------------------------------------------------------------- positions

Result<ir::Coord> position(const Json& value) {
  if (!value.is_array() || value.size() < 2) return fail(Status::Decode, "a GeoJSON position needs two numbers");
  const auto lon = value[0].get_double();
  const auto lat = value[1].get_double();
  if (!lon || !lat || !std::isfinite(*lon) || !std::isfinite(*lat)) {
    return fail(Status::Decode, "a GeoJSON position holds a value that is not a number");
  }
  ir::Coord c;
  c.lon = *lon;
  c.lat = *lat;
  if (value.size() >= 3) {
    if (auto z = value[2].get_double(); z && std::isfinite(*z)) c.ele = *z;
  }
  if (value.size() >= 4) {
    if (auto m = value[3].get_double(); m && std::isfinite(*m)) c.m = *m;
  }
  return c;
}

Result<std::vector<ir::Coord>> positions(const Json& value) {
  if (!value.is_array()) return fail(Status::Decode, "GeoJSON coordinates must be an array");
  std::vector<ir::Coord> out;
  out.reserve(value.size());
  for (const Json& p : value.elements()) {
    CVT_TRY_ASSIGN(ir::Coord c, position(p));
    out.push_back(std::move(c));
  }
  return out;
}

Result<std::vector<std::vector<ir::Coord>>> rings(const Json& value) {
  if (!value.is_array()) return fail(Status::Decode, "GeoJSON polygon coordinates must be an array of rings");
  std::vector<std::vector<ir::Coord>> out;
  for (const Json& r : value.elements()) {
    CVT_TRY_ASSIGN(std::vector<ir::Coord> ring, positions(r));
    if (!ring.empty() && !(ring.front().lon == ring.back().lon && ring.front().lat == ring.back().lat)) {
      ring.push_back(ring.front());  // be forgiving about unclosed rings
    }
    out.push_back(std::move(ring));
  }
  return out;
}

Json position_json(const ir::Coord& c) {
  Json p = Json::array({c.lon, c.lat});
  if (c.ele) p.push_back(*c.ele);
  return p;
}

Json positions_json(const std::vector<ir::Coord>& points) {
  Json out = Json::array();
  out.reserve(points.size());
  for (const ir::Coord& c : points) out.push_back(position_json(c));
  return out;
}

Json polygon_json(const ir::Polygon& polygon) {
  Json out = Json::array();
  for (const auto& ring : polygon.rings) out.push_back(positions_json(ring));
  return out;
}

}  // namespace

// --------------------------------------------------------------------------- geometries

Result<ir::Geometry> geometry_from_geojson(const Json& value, int depth) {
  ir::Geometry g;
  if (value.is_null()) {
    g.shape = ir::GeometryCollection{};
    return g;
  }
  if (!value.is_object()) return fail(Status::Decode, "a GeoJSON geometry must be an object");
  if (depth > ir::max_geometry_depth) return fail(Status::Limit, "GeoJSON geometry collections are nested too deeply");
  const std::string_view type = value["type"].as_string();
  const Json& coordinates = value["coordinates"];
  if (type == "Point") {
    if (coordinates.is_array() && coordinates.empty()) {
      g.shape = ir::MultiPoint{};  // an empty point has no position
      return g;
    }
    CVT_TRY_ASSIGN(ir::Coord c, position(coordinates));
    g.shape = ir::Point{std::move(c)};
  } else if (type == "LineString") {
    CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, positions(coordinates));
    g.shape = ir::LineString{std::move(pts)};
  } else if (type == "MultiPoint") {
    CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, positions(coordinates));
    g.shape = ir::MultiPoint{std::move(pts)};
  } else if (type == "Polygon") {
    CVT_TRY_ASSIGN(auto r, rings(coordinates));
    g.shape = ir::Polygon{std::move(r)};
  } else if (type == "MultiLineString") {
    if (!coordinates.is_array()) return fail(Status::Decode, "GeoJSON coordinates must be an array");
    ir::MultiLineString multi;
    for (const Json& line : coordinates.elements()) {
      CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, positions(line));
      multi.lines.push_back(ir::LineString{std::move(pts)});
    }
    g.shape = std::move(multi);
  } else if (type == "MultiPolygon") {
    if (!coordinates.is_array()) return fail(Status::Decode, "GeoJSON coordinates must be an array");
    ir::MultiPolygon multi;
    for (const Json& polygon : coordinates.elements()) {
      CVT_TRY_ASSIGN(auto r, rings(polygon));
      multi.polygons.push_back(ir::Polygon{std::move(r)});
    }
    g.shape = std::move(multi);
  } else if (type == "GeometryCollection") {
    const Json& members = value["geometries"];
    if (!members.is_array()) return fail(Status::Decode, "a GeoJSON GeometryCollection needs a 'geometries' array");
    ir::GeometryCollection collection;
    for (const Json& member : members.elements()) {
      CVT_TRY_ASSIGN(ir::Geometry inner, geometry_from_geojson(member, depth + 1));
      collection.geometries.push_back(std::move(inner));
    }
    g.shape = std::move(collection);
  } else {
    return fail(Status::Decode, "unknown GeoJSON geometry type '{}'", util::sanitize_utf8(type.substr(0, 40)));
  }
  return g;
}

Json geometry_to_geojson(const ir::Geometry& geometry) {
  return std::visit(
      [](const auto& shape) -> Json {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, ir::Point>) {
          return Json::object({{"type", "Point"}, {"coordinates", position_json(shape.coord)}});
        } else if constexpr (std::is_same_v<T, ir::LineString>) {
          return Json::object({{"type", "LineString"}, {"coordinates", positions_json(shape.points)}});
        } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
          return Json::object({{"type", "MultiPoint"}, {"coordinates", positions_json(shape.points)}});
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          return Json::object({{"type", "Polygon"}, {"coordinates", polygon_json(shape)}});
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          Json lines = Json::array();
          for (const auto& l : shape.lines) lines.push_back(positions_json(l.points));
          return Json::object({{"type", "MultiLineString"}, {"coordinates", std::move(lines)}});
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          Json polygons = Json::array();
          for (const auto& p : shape.polygons) polygons.push_back(polygon_json(p));
          return Json::object({{"type", "MultiPolygon"}, {"coordinates", std::move(polygons)}});
        } else {
          if (shape.geometries.empty()) return Json();
          Json members = Json::array();
          for (const auto& g : shape.geometries) {
            Json inner = geometry_to_geojson(g);
            if (inner.is_null()) inner = Json::object({{"type", "GeometryCollection"}, {"geometries", Json::array()}});
            members.push_back(std::move(inner));
          }
          return Json::object({{"type", "GeometryCollection"}, {"geometries", std::move(members)}});
        }
      },
      geometry.shape);
}

// --------------------------------------------------------------------------- per-point properties

namespace {

// The coordinate sequences of a geometry in the order the togeojson
// convention nests them: a LineString / MultiPoint is one sequence, a
// MultiLineString one per line, a Polygon one per ring.
void sequences(ir::Geometry& g, std::vector<std::vector<ir::Coord>*>& out, bool& nested) {
  if (auto* l = std::get_if<ir::LineString>(&g.shape)) {
    out.push_back(&l->points);
  } else if (auto* mp = std::get_if<ir::MultiPoint>(&g.shape)) {
    out.push_back(&mp->points);
  } else if (auto* ml = std::get_if<ir::MultiLineString>(&g.shape)) {
    nested = true;
    for (auto& line : ml->lines) out.push_back(&line.points);
  } else if (auto* p = std::get_if<ir::Polygon>(&g.shape)) {
    nested = true;
    for (auto& ring : p->rings) out.push_back(&ring);
  }
}

using Setter = void (*)(ir::Coord&, const Json&);

void apply_values(ir::Geometry& g, const Json& values, Setter set) {
  std::vector<std::vector<ir::Coord>*> seqs;
  bool nested = false;
  sequences(g, seqs, nested);
  if (seqs.empty() || !values.is_array()) return;
  if (!nested) {
    auto& pts = *seqs.front();
    for (std::size_t i = 0; i < pts.size() && i < values.size(); ++i) set(pts[i], values[i]);
    return;
  }
  // Nested data; a flat array is also accepted for one-line geometries.
  if (seqs.size() == 1 && !values.empty() && !values[0].is_array()) {
    auto& pts = *seqs.front();
    for (std::size_t i = 0; i < pts.size() && i < values.size(); ++i) set(pts[i], values[i]);
    return;
  }
  for (std::size_t s = 0; s < seqs.size() && s < values.size(); ++s) {
    auto& pts = *seqs[s];
    const Json& row = values[s];
    for (std::size_t i = 0; i < pts.size() && i < row.size(); ++i) set(pts[i], row[i]);
  }
}

void set_time(ir::Coord& c, const Json& v) {
  if (v.is_string()) {
    c.time_ms = parse_time(v.as_string());
  } else if (auto ms = v.get_int64()) {
    c.time_ms = *ms;
  }
}
void set_hr(ir::Coord& c, const Json& v) {
  if (auto d = v.get_double()) c.heart_rate = *d;
}
void set_cad(ir::Coord& c, const Json& v) {
  if (auto d = v.get_double()) c.cadence = *d;
}
void set_power(ir::Coord& c, const Json& v) {
  if (auto d = v.get_double()) c.power = *d;
}
void set_speed(ir::Coord& c, const Json& v) {
  if (auto d = v.get_double()) c.speed = *d;
}
void set_temp(ir::Coord& c, const Json& v) {
  if (auto d = v.get_double()) c.temperature = *d;
}

void read_coordinate_properties(ir::Feature& f) {
  Json& props = f.properties;
  if (const Json* times = props.find("coordTimes")) {
    apply_values(f.geometry, *times, set_time);
    props.erase("coordTimes");
  }
  if (const Json* cp = props.find("coordinateProperties"); cp != nullptr && cp->is_object()) {
    const Json copy = *cp;
    props.erase("coordinateProperties");
    for (const auto& member : copy.items()) {
      const std::string_view key = member.key();
      if (key == "times" || key == "coordTimes") apply_values(f.geometry, member.value(), set_time);
      else if (key == "heart" || key == "heartRates" || key == "heartRate") apply_values(f.geometry, member.value(), set_hr);
      else if (key == "cadences" || key == "cads" || key == "cadence") apply_values(f.geometry, member.value(), set_cad);
      else if (key == "powers" || key == "power") apply_values(f.geometry, member.value(), set_power);
      else if (key == "speeds" || key == "speed") apply_values(f.geometry, member.value(), set_speed);
      else if (key == "atemps" || key == "temperatures" || key == "temperature") apply_values(f.geometry, member.value(), set_temp);
    }
  }
  if (auto* p = std::get_if<ir::Point>(&f.geometry.shape)) {
    if (const Json* t = props.find("time"); t != nullptr && t->is_string()) p->coord.time_ms = parse_time(t->as_string());
  }
}

template <class Get>
Json collect_values(const ir::Geometry& g, Get get, bool& any) {
  std::vector<std::vector<ir::Coord>*> seqs;
  bool nested = false;
  sequences(const_cast<ir::Geometry&>(g), seqs, nested);
  const auto row = [&](const std::vector<ir::Coord>& pts) {
    Json out = Json::array();
    out.reserve(pts.size());
    for (const ir::Coord& c : pts) {
      Json v = get(c);
      if (!v.is_null()) any = true;
      out.push_back(std::move(v));
    }
    return out;
  };
  if (seqs.empty()) return Json();
  if (!nested) return row(*seqs.front());
  Json out = Json::array();
  for (const auto* s : seqs) out.push_back(row(*s));
  return out;
}

void write_coordinate_properties(const ir::Feature& f, Json& props) {
  if (const auto* p = std::get_if<ir::Point>(&f.geometry.shape)) {
    if (p->coord.time_ms && !props.contains("time")) props.set("time", format_time(*p->coord.time_ms));
    return;
  }
  Json cp = Json::object();
  const auto add = [&](std::string_view key, auto get) {
    bool any = false;
    Json values = collect_values(f.geometry, get, any);
    if (any) cp.set(key, std::move(values));
  };
  add("times", [](const ir::Coord& c) { return c.time_ms ? Json(format_time(*c.time_ms)) : Json(); });
  add("heartRates", [](const ir::Coord& c) { return c.heart_rate ? Json(*c.heart_rate) : Json(); });
  add("cadences", [](const ir::Coord& c) { return c.cadence ? Json(*c.cadence) : Json(); });
  add("powers", [](const ir::Coord& c) { return c.power ? Json(*c.power) : Json(); });
  add("speeds", [](const ir::Coord& c) { return c.speed ? Json(*c.speed) : Json(); });
  add("temperatures", [](const ir::Coord& c) { return c.temperature ? Json(*c.temperature) : Json(); });
  if (!cp.empty()) props.set("coordinateProperties", std::move(cp));
}

// --------------------------------------------------------------------------- styles

void read_simplestyle(ir::Feature& f) {
  Json& p = f.properties;
  const auto take_string = [&](std::string_view key, std::string& out) {
    if (const Json* v = p.find(key); v != nullptr && v->is_string()) {
      out = v->as_string();
      p.erase(key);
    }
  };
  const auto take_number = [&](std::string_view key, double& out) {
    if (const Json* v = p.find(key); v != nullptr && v->is_number()) {
      out = v->as_double();
      p.erase(key);
    }
  };
  take_string("stroke", f.style.stroke);
  take_number("stroke-opacity", f.style.stroke_opacity);
  take_number("stroke-width", f.style.stroke_width);
  take_string("fill", f.style.fill);
  take_number("fill-opacity", f.style.fill_opacity);
  take_string("marker-color", f.style.marker_color);
  take_string("marker-symbol", f.style.icon);
}

void write_simplestyle(const ir::Style& s, Json& p) {
  if (!s.stroke.empty()) p.set("stroke", s.stroke);
  if (s.stroke_opacity != 1.0) p.set("stroke-opacity", s.stroke_opacity);
  if (s.stroke_width != 0.0) p.set("stroke-width", s.stroke_width);
  if (!s.fill.empty()) p.set("fill", s.fill);
  if (s.fill_opacity != 1.0) p.set("fill-opacity", s.fill_opacity);
  if (!s.marker_color.empty()) p.set("marker-color", s.marker_color);
  if (!s.icon.empty()) p.set("marker-symbol", s.icon);
}

std::string_view role_name(ir::FeatureRole role) {
  switch (role) {
    case ir::FeatureRole::Waypoint: return "wpt";
    case ir::FeatureRole::Route: return "rte";
    case ir::FeatureRole::Track: return "trk";
    case ir::FeatureRole::Generic: break;
  }
  return {};
}

Json bbox_json(const BoundingBox& box) {
  return Json::array({box.min_lon, box.min_lat, box.max_lon, box.max_lat});
}

Json crs_member(const std::string& crs) {
  std::string name = crs;
  if (const auto code = epsg_code(crs)) name = "urn:ogc:def:crs:EPSG::" + std::to_string(*code);
  return Json::object({{"type", "name"}, {"properties", Json::object({{"name", name}})}});
}

std::string crs_from_member(const Json& crs) {
  if (!crs.is_object()) return {};
  const Json& props = crs["properties"];
  if (const Json* name = props.find("name"); name != nullptr && name->is_string()) return name->as_string();
  if (const Json* code = props.find("code"); code != nullptr && code->is_number()) {
    return "EPSG:" + std::to_string(code->as_int64());
  }
  return {};
}

}  // namespace

// --------------------------------------------------------------------------- features

Result<ir::Feature> feature_from_geojson(const Json& value, const Context& ctx) {
  if (!value.is_object()) return fail(Status::Decode, "a GeoJSON feature must be an object");
  ir::Feature f;
  CVT_TRY_ASSIGN(f.geometry, geometry_from_geojson(value["geometry"]));
  const Json& props = value["properties"];
  if (props.is_object()) {
    f.properties = props;
  } else if (!props.is_null()) {
    ctx.warn("A GeoJSON feature has 'properties' that are not an object; they were kept under the key 'value'.");
    f.properties = Json::object({{"value", props}});
  }
  if (const Json* id = value.find("id"); id != nullptr && (id->is_string() || id->is_number())) f.id = *id;
  for (const auto& member : value.items()) {
    const std::string_view key = member.key();
    if (key == "type" || key == "id" || key == "geometry" || key == "properties" || key == "bbox") continue;
    f.extra.set(key, member.value());
  }
  read_coordinate_properties(f);
  read_simplestyle(f);
  if (const Json* role = f.properties.find("_gpxType"); role != nullptr && role->is_string()) {
    const std::string& r = role->as_string();
    if (r == "wpt") f.role = ir::FeatureRole::Waypoint;
    if (r == "rte") f.role = ir::FeatureRole::Route;
    if (r == "trk") f.role = ir::FeatureRole::Track;
    f.properties.erase("_gpxType");
    if (f.role == ir::FeatureRole::Track) {
      if (auto* line = std::get_if<ir::LineString>(&f.geometry.shape)) {
        f.geometry.shape = ir::MultiLineString{{std::move(*line)}};
      }
    }
  }
  if (const Json* name = f.properties.find("name"); name != nullptr && name->is_string()) f.name = name->as_string();
  if (const Json* d = f.properties.find("description"); d != nullptr && d->is_string()) f.description = d->as_string();
  return f;
}

Json feature_to_geojson(const ir::Feature& feature, const GeoJsonStyle& style) {
  ir::Geometry geometry = feature.geometry;
  if (style.rfc7946) {
    geometry = split_antimeridian(geometry);
    orient_rings(geometry, true);
  }
  Json out = Json::object({{"type", "Feature"}});
  if (!feature.id.is_null()) out.set("id", feature.id);
  if (style.bbox) {
    const BoundingBox box = bounding_box(geometry);
    if (!box.empty) out.set("bbox", bbox_json(box));
  }
  Json props = feature.properties.is_object() ? feature.properties : Json::object();
  if (!feature.name.empty() && !props.contains("name")) props.set("name", feature.name);
  if (!feature.description.empty() && !props.contains("description")) props.set("description", feature.description);
  if (style.gpx_type && feature.role != ir::FeatureRole::Generic) props.set("_gpxType", role_name(feature.role));
  if (style.simplestyle) write_simplestyle(feature.style, props);
  if (style.coordinate_properties) write_coordinate_properties(feature, props);
  out.set("properties", std::move(props));
  out.set("geometry", geometry_to_geojson(geometry));
  for (const auto& member : feature.extra.items()) {
    const std::string_view key = member.key();
    if (key == "type" || key == "id" || key == "geometry" || key == "properties" || key == "bbox") continue;
    out.set(key, member.value());
  }
  return out;
}

Result<ir::Geo> geo_from_geojson(const Json& root, const Context& ctx) {
  if (!root.is_object()) return fail(Status::Decode, "a GeoJSON document must be an object");
  ir::Geo geo;
  const std::string_view type = root["type"].as_string();
  if (const Json* crs = root.find("crs")) geo.crs = crs_from_member(*crs);
  if (type == "FeatureCollection") {
    const Json& features = root["features"];
    if (!features.is_array()) return fail(Status::Decode, "a GeoJSON FeatureCollection needs a 'features' array");
    geo.features.reserve(features.size());
    std::size_t index = 0;
    for (const Json& f : features.elements()) {
      if ((++index & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(features.size())));
      CVT_TRY_ASSIGN(ir::Feature feature, feature_from_geojson(f, ctx));
      geo.features.push_back(std::move(feature));
    }
    if (const Json* name = root.find("name"); name != nullptr && name->is_string()) geo.name = name->as_string();
    if (const Json* d = root.find("description"); d != nullptr && d->is_string()) geo.description = d->as_string();
    if (const Json* m = root.find("metadata"); m != nullptr && m->is_object()) geo.metadata = *m;
    for (const auto& member : root.items()) {
      const std::string_view key = member.key();
      if (key == "type" || key == "features" || key == "bbox" || key == "crs" || key == "name" ||
          key == "description" || key == "metadata") {
        continue;
      }
      geo.extra.set(key, member.value());
    }
  } else if (type == "Feature") {
    CVT_TRY_ASSIGN(ir::Feature feature, feature_from_geojson(root, ctx));
    geo.features.push_back(std::move(feature));
  } else {
    ir::Feature feature;
    CVT_TRY_ASSIGN(feature.geometry, geometry_from_geojson(root));
    geo.features.push_back(std::move(feature));
  }
  return geo;
}

Json geo_to_geojson(const ir::Geo& geo, const GeoJsonStyle& style) {
  Json out = Json::object({{"type", "FeatureCollection"}});
  if (!geo.name.empty()) out.set("name", geo.name);
  if (!geo.description.empty()) out.set("description", geo.description);
  if (!geo.crs.empty()) out.set("crs", crs_member(geo.crs));
  if (!geo.metadata.empty()) out.set("metadata", geo.metadata);
  if (style.bbox) {
    const BoundingBox box = bounding_box(geo);
    if (!box.empty) out.set("bbox", bbox_json(box));
  }
  for (const auto& member : geo.extra.items()) {
    const std::string_view key = member.key();
    if (key == "type" || key == "features" || key == "bbox" || key == "crs" || key == "name") continue;
    out.set(key, member.value());
  }
  Json features = Json::array();
  features.reserve(geo.features.size());
  for (const ir::Feature& f : geo.features) features.push_back(feature_to_geojson(f, style));
  out.set("features", std::move(features));
  return out;
}

// --------------------------------------------------------------------------- codecs

namespace {

util::JsonParseOptions parse_options() {
  util::JsonParseOptions options;
  options.max_depth = 256;
  options.allow_nan_inf = false;
  return options;
}

GeoJsonStyle style_from(const Options& options) {
  GeoJsonStyle style;
  style.rfc7946 = options.get_bool("rfc7946", true);
  style.bbox = options.get_bool("bbox", false);
  return style;
}

Result<ir::Geo> read_geojson(ReadContext& ctx) {
  const std::string text = input_text(ctx.input);
  auto parsed = util::parse_json(text, parse_options());
  if (!parsed) return fail(Status::Decode, "not valid GeoJSON: {}", parsed.error().message);
  return geo_from_geojson(*parsed, ctx);
}

Result<Bytes> write_geojson(const ir::Geo& geo, WriteContext& ctx) {
  const Json doc = geo_to_geojson(geo, style_from(ctx.options));
  util::JsonWriteOptions options;
  options.indent = ctx.options.get_bool("pretty", false) ? 2 : -1;
  std::string text = util::dump_json(doc, options);
  text += '\n';
  return to_bytes(text);
}

// GeoJSON text sequences: one feature per line (newline-delimited) or per
// record (RFC 8142: each record starts with the RS character 0x1E).
Result<ir::Geo> read_geojsonseq(ReadContext& ctx) {
  const std::string text = input_text(ctx.input);
  ir::Geo geo;
  const bool rs = text.find('\x1E') != std::string::npos;
  std::vector<std::string_view> records = rs ? util::split(text, '\x1E') : util::split_lines(text);
  std::size_t index = 0;
  for (std::string_view record : records) {
    if ((++index & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(records.size())));
    record = util::trim(record);
    if (record.empty()) continue;
    auto parsed = util::parse_json(record, parse_options());
    if (!parsed) {
      if (rs) {
        // RFC 8142: a truncated record is skipped with a note.
        ctx.warn("A GeoJSON text sequence record could not be parsed and was skipped.");
        continue;
      }
      return fail(Status::Decode, "line {} is not valid GeoJSON: {}", index, parsed.error().message);
    }
    CVT_TRY_ASSIGN(ir::Geo part, geo_from_geojson(*parsed, ctx));
    if (geo.crs.empty()) geo.crs = part.crs;
    for (auto& f : part.features) geo.features.push_back(std::move(f));
  }
  if (geo.features.empty() && util::trim(text).empty()) return fail(Status::Decode, "the GeoJSON sequence is empty");
  return geo;
}

Result<Bytes> write_geojsonseq(const ir::Geo& geo, WriteContext& ctx) {
  const GeoJsonStyle style = style_from(ctx.options);
  const bool rs = ctx.options.get_bool("record_separator", false);
  std::string out;
  for (const ir::Feature& f : geo.features) {
    if (rs) out += '\x1E';
    out += util::dump_json(feature_to_geojson(f, style));
    out += '\n';
  }
  return to_bytes(out);
}

// Is the first JSON value of the text an object that ends on its own line
// with another one after it (a newline-delimited sequence)?
bool looks_like_sequence(std::string_view head) {
  const std::size_t newline = head.find('\n');
  if (newline == std::string_view::npos) return false;
  const std::string_view first = util::trim(head.substr(0, newline));
  const std::string_view rest = util::trim_left(head.substr(newline + 1));
  return first.starts_with('{') && first.ends_with('}') && rest.starts_with('{');
}

bool mentions_geojson_type(std::string_view head) {
  for (std::string_view type : {"\"Feature\""sv, "\"FeatureCollection\""sv}) {
    if (head.find(type) != std::string_view::npos) return true;
  }
  return false;
}

bool mentions_geometry(std::string_view head) {
  if (head.find("\"coordinates\"") == std::string_view::npos && head.find("\"geometries\"") == std::string_view::npos) {
    return false;
  }
  for (std::string_view type : {"\"Point\""sv, "\"LineString\""sv, "\"Polygon\""sv, "\"MultiPoint\""sv,
                                "\"MultiLineString\""sv, "\"MultiPolygon\""sv, "\"GeometryCollection\""sv}) {
    if (head.find(type) != std::string_view::npos) return true;
  }
  return false;
}

int sniff_geojson(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 16384);
  if (!head.starts_with('{')) return 0;
  if (head.find("\"Topology\"") != std::string_view::npos) return 0;
  if (looks_like_sequence(head)) return 0;
  if (mentions_geojson_type(head)) return 92;
  if (mentions_geometry(head)) return 85;
  return 0;
}

int sniff_geojsonseq(ByteView data, std::string_view) {
  const std::string_view raw = as_chars(data.first(std::min<std::size_t>(data.size(), 16384)));
  if (raw.starts_with('\x1E') && raw.find('{') == 1) return 95;
  const std::string_view head = text_head(data, 16384);
  if (looks_like_sequence(head) && (mentions_geojson_type(head) || mentions_geometry(head))) return 94;
  return 0;
}

}  // namespace

void register_geojson(Registry& registry) {
  FormatInfo info;
  info.id = "geojson";
  info.name = "GeoJSON";
  info.description = "Geographic features as JSON (RFC 7946).";
  info.category = Category::Geo;
  info.extensions = {"geojson", "json"};
  info.mime = "application/geo+json";
  info.popularity = 75;
  info.sniff = sniff_geojson;
  registry.add_format(std::move(info));

  GeoCodecSpec spec;
  spec.format_id = "geojson";
  spec.read = read_geojson;
  spec.write = write_geojson;
  spec.traits = GeoWriterTraits{false, true, true, true};
  spec.write_options = {
      OptionSpec::boolean("pretty", "Indent", false, "Write one value per line with indentation instead of compact JSON."),
      OptionSpec::boolean("bbox", "Bounding boxes", false, "Add a bbox member to the collection and to every feature.")
          .advanced(),
      OptionSpec::boolean("rfc7946", "RFC 7946 geometry rules", true,
                          "Orient rings counter-clockwise (holes clockwise) and split shapes that cross the "
                          "antimeridian, as RFC 7946 requires.")
          .advanced(),
  };
  add_geo_codec(registry, std::move(spec));

  FormatInfo seq;
  seq.id = "geojsonseq";
  seq.name = "GeoJSON text sequence";
  seq.description = "One GeoJSON feature per line (newline-delimited GeoJSON, RFC 8142 sequences).";
  seq.category = Category::Geo;
  seq.extensions = {"geojsonl", "geojsons", "geojsonseq", "ndgeojson"};
  seq.mime = "application/geo+json-seq";
  seq.popularity = 30;
  seq.sniff = sniff_geojsonseq;
  registry.add_format(std::move(seq));

  GeoCodecSpec seq_spec;
  seq_spec.format_id = "geojsonseq";
  seq_spec.read = read_geojsonseq;
  seq_spec.write = write_geojsonseq;
  seq_spec.traits = GeoWriterTraits{false, true, true, true};
  seq_spec.write_options = {
      OptionSpec::boolean("record_separator", "RFC 8142 record separators", false,
                          "Start every feature with the ASCII record separator (application/geo+json-seq) "
                          "instead of writing plain newline-delimited GeoJSON."),
      OptionSpec::boolean("bbox", "Bounding boxes", false, "Add a bbox member to every feature.").advanced(),
      OptionSpec::boolean("rfc7946", "RFC 7946 geometry rules", true,
                          "Orient rings counter-clockwise and split shapes that cross the antimeridian.")
          .advanced(),
  };
  add_geo_codec(registry, std::move(seq_spec));
}

}  // namespace cvt::geo
