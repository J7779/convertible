// Convertible core - geo family: TopoJSON.
//
// Reading: every object of the topology (a GeometryCollection or a single
// geometry; the object name becomes Feature::folder when there are
// several), arcs with or without the quantisation transform (delta-coded
// when quantised), negative arc indices (reversed arcs), points and
// multipoints, nested collections (depth-limited), ids and properties.
// Writing: one object; every distinct line or ring becomes one arc, and a
// ring or line that repeats an earlier one (in either direction) refers to
// the same arc. Coordinates are quantised (`quantization`, default 1e6
// positions per axis; 0 writes exact coordinates without a transform).
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
#include "families/geo/geo.hpp"
#include "families/geo/geo_ops.hpp"
#include "util/json.hpp"

namespace cvt::geo {

namespace {

struct Transform {
  bool present = false;
  double sx = 1, sy = 1, tx = 0, ty = 0;
};

struct Topology {
  Transform transform;
  std::vector<std::vector<ir::Coord>> arcs;
};

Result<ir::Coord> position(const Json& p, const Transform& t) {
  if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) {
    return fail(Status::Decode, "a TopoJSON position needs two numbers");
  }
  ir::Coord c;
  c.lon = p[0].as_double() * t.sx + t.tx;
  c.lat = p[1].as_double() * t.sy + t.ty;
  if (p.size() >= 3 && p[2].is_number()) c.ele = p[2].as_double();
  return c;
}

Result<std::vector<ir::Coord>> stitch(const Json& indices, const Topology& topo) {
  if (!indices.is_array()) return fail(Status::Decode, "TopoJSON arcs must be arrays of indices");
  std::vector<ir::Coord> out;
  for (const Json& index : indices.elements()) {
    const auto i = index.get_int64();
    if (!i) return fail(Status::Decode, "a TopoJSON arc index is not an integer");
    const bool reversed = *i < 0;
    const std::uint64_t k = reversed ? static_cast<std::uint64_t>(~*i) : static_cast<std::uint64_t>(*i);
    if (k >= topo.arcs.size()) return fail(Status::Decode, "TopoJSON arc index {} is out of range", *i);
    const auto& arc = topo.arcs[static_cast<std::size_t>(k)];
    if (arc.empty()) continue;
    const std::size_t start = out.empty() ? 0 : 1;  // shared end point
    if (reversed) {
      for (std::size_t j = arc.size() - start; j-- > 0;) out.push_back(arc[j]);
    } else {
      for (std::size_t j = start; j < arc.size(); ++j) out.push_back(arc[j]);
    }
  }
  return out;
}

Result<ir::Polygon> polygon(const Json& rings, const Topology& topo) {
  if (!rings.is_array()) return fail(Status::Decode, "TopoJSON polygon arcs must be an array of rings");
  ir::Polygon p;
  for (const Json& ring : rings.elements()) {
    CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, stitch(ring, topo));
    if (!pts.empty() && !(pts.front().lon == pts.back().lon && pts.front().lat == pts.back().lat)) pts.push_back(pts.front());
    p.rings.push_back(std::move(pts));
  }
  return p;
}

Result<ir::Geometry> geometry(const Json& g, const Topology& topo, int depth) {
  if (depth > ir::max_geometry_depth) return fail(Status::Limit, "TopoJSON geometry collections are nested too deeply");
  ir::Geometry out;
  const std::string_view type = g["type"].as_string();
  const Json& arcs = g["arcs"];
  if (type == "Point") {
    CVT_TRY_ASSIGN(ir::Coord c, position(g["coordinates"], topo.transform));
    out.shape = ir::Point{c};
  } else if (type == "MultiPoint") {
    ir::MultiPoint mp;
    for (const Json& p : g["coordinates"].elements()) {
      CVT_TRY_ASSIGN(ir::Coord c, position(p, topo.transform));
      mp.points.push_back(c);
    }
    out.shape = std::move(mp);
  } else if (type == "LineString") {
    CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, stitch(arcs, topo));
    out.shape = ir::LineString{std::move(pts)};
  } else if (type == "MultiLineString") {
    ir::MultiLineString ml;
    for (const Json& line : arcs.elements()) {
      CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, stitch(line, topo));
      ml.lines.push_back(ir::LineString{std::move(pts)});
    }
    out.shape = std::move(ml);
  } else if (type == "Polygon") {
    CVT_TRY_ASSIGN(ir::Polygon p, polygon(arcs, topo));
    out.shape = std::move(p);
  } else if (type == "MultiPolygon") {
    ir::MultiPolygon mp;
    for (const Json& p : arcs.elements()) {
      CVT_TRY_ASSIGN(ir::Polygon poly, polygon(p, topo));
      mp.polygons.push_back(std::move(poly));
    }
    out.shape = std::move(mp);
  } else if (type == "GeometryCollection") {
    ir::GeometryCollection gc;
    for (const Json& member : g["geometries"].elements()) {
      CVT_TRY_ASSIGN(ir::Geometry inner, geometry(member, topo, depth + 1));
      gc.geometries.push_back(std::move(inner));
    }
    out.shape = std::move(gc);
  } else if (type.empty() || g["type"].is_null()) {
    out.shape = ir::GeometryCollection{};
  } else {
    return fail(Status::Decode, "unknown TopoJSON geometry type '{}'", type.substr(0, 40));
  }
  return out;
}

ir::Feature feature_of(const Json& g, ir::Geometry geom, const std::string& folder) {
  ir::Feature f;
  f.geometry = std::move(geom);
  f.folder = folder;
  if (const Json* p = g.find("properties"); p != nullptr && p->is_object()) f.properties = *p;
  if (const Json* id = g.find("id"); id != nullptr && (id->is_string() || id->is_number())) f.id = *id;
  if (const Json* name = f.properties.find("name"); name != nullptr && name->is_string()) f.name = name->as_string();
  return f;
}

Result<ir::Geo> read_topojson(ReadContext& ctx) {
  util::JsonParseOptions options;
  options.max_depth = 256;
  auto root = util::parse_json(input_text(ctx.input), options);
  if (!root) return fail(Status::Decode, "not valid TopoJSON: {}", root.error().message);
  if ((*root)["type"].as_string() != "Topology") return fail(Status::Decode, "not a TopoJSON topology");
  Topology topo;
  if (const Json* t = root->find("transform"); t != nullptr && t->is_object()) {
    const Json& scale = (*t)["scale"];
    const Json& translate = (*t)["translate"];
    if (scale.size() < 2 || translate.size() < 2) return fail(Status::Decode, "the TopoJSON transform is incomplete");
    topo.transform = {true, scale[0].as_double(1), scale[1].as_double(1), translate[0].as_double(), translate[1].as_double()};
  }
  const Json& arcs = (*root)["arcs"];
  topo.arcs.reserve(arcs.size());
  for (const Json& arc : arcs.elements()) {
    std::vector<ir::Coord> pts;
    pts.reserve(arc.size());
    double x = 0, y = 0;
    for (const Json& p : arc.elements()) {
      if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) {
        return fail(Status::Decode, "a TopoJSON arc position needs two numbers");
      }
      ir::Coord c;
      if (topo.transform.present) {
        x += p[0].as_double();
        y += p[1].as_double();
        c.lon = x * topo.transform.sx + topo.transform.tx;
        c.lat = y * topo.transform.sy + topo.transform.ty;
      } else {
        c.lon = p[0].as_double();
        c.lat = p[1].as_double();
      }
      if (p.size() >= 3 && p[2].is_number()) c.ele = p[2].as_double();
      pts.push_back(c);
    }
    topo.arcs.push_back(std::move(pts));
  }
  ir::Geo geo;
  const Json& objects = (*root)["objects"];
  const bool several = objects.size() > 1;
  for (const auto& member : objects.items()) {
    CVT_TRY(ctx.checkpoint(0.5));
    const std::string name(member.key());
    const Json& object = member.value();
    const std::string folder = several ? name : std::string();
    if (object["type"].as_string() == "GeometryCollection") {
      for (const Json& g : object["geometries"].elements()) {
        CVT_TRY_ASSIGN(ir::Geometry geom, geometry(g, topo, 1));
        geo.features.push_back(feature_of(g, std::move(geom), folder));
      }
    } else {
      CVT_TRY_ASSIGN(ir::Geometry geom, geometry(object, topo, 0));
      geo.features.push_back(feature_of(object, std::move(geom), folder));
    }
    if (!several) geo.name = name;
  }
  return geo;
}

// --------------------------------------------------------------------------- writing

struct ArcBuilder {
  std::vector<std::vector<ir::Coord>> arcs;
  std::map<std::vector<std::pair<double, double>>, std::int64_t> index;

  static std::vector<std::pair<double, double>> key(const std::vector<ir::Coord>& pts) {
    std::vector<std::pair<double, double>> k;
    k.reserve(pts.size());
    for (const ir::Coord& c : pts) k.emplace_back(c.lon, c.lat);
    return k;
  }
  // Returns the arc index, negative (~i) when the sequence is an earlier arc reversed.
  std::int64_t add(const std::vector<ir::Coord>& pts) {
    auto k = key(pts);
    if (auto it = index.find(k); it != index.end()) return it->second;
    std::reverse(k.begin(), k.end());
    if (auto it = index.find(k); it != index.end()) return ~it->second;
    std::reverse(k.begin(), k.end());
    const auto i = static_cast<std::int64_t>(arcs.size());
    index.emplace(std::move(k), i);
    arcs.push_back(pts);
    return i;
  }
};

struct Quantizer {
  bool on = false;
  double x0 = 0, y0 = 0, kx = 1, ky = 1;
  [[nodiscard]] std::int64_t qx(double x) const { return std::llround((x - x0) * kx); }
  [[nodiscard]] std::int64_t qy(double y) const { return std::llround((y - y0) * ky); }
};

Json point_json(const ir::Coord& c, const Quantizer& q) {
  Json p = q.on ? Json::array({q.qx(c.lon), q.qy(c.lat)}) : Json::array({c.lon, c.lat});
  if (c.ele) p.push_back(*c.ele);
  return p;
}

Json geometry_json(const ir::Geometry& g, ArcBuilder& arcs, const Quantizer& q, int depth) {
  const auto ring_arcs = [&](const ir::Polygon& p) {
    Json rings = Json::array();
    for (const auto& r : p.rings) rings.push_back(Json::array({arcs.add(r)}));
    return rings;
  };
  return std::visit(
      [&](const auto& s) -> Json {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ir::Point>) {
          return Json::object({{"type", "Point"}, {"coordinates", point_json(s.coord, q)}});
        } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
          Json pts = Json::array();
          for (const auto& c : s.points) pts.push_back(point_json(c, q));
          return Json::object({{"type", "MultiPoint"}, {"coordinates", std::move(pts)}});
        } else if constexpr (std::is_same_v<T, ir::LineString>) {
          return Json::object({{"type", "LineString"}, {"arcs", Json::array({arcs.add(s.points)})}});
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          Json lines = Json::array();
          for (const auto& l : s.lines) lines.push_back(Json::array({arcs.add(l.points)}));
          return Json::object({{"type", "MultiLineString"}, {"arcs", std::move(lines)}});
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          return Json::object({{"type", "Polygon"}, {"arcs", ring_arcs(s)}});
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          Json polygons = Json::array();
          for (const auto& p : s.polygons) polygons.push_back(ring_arcs(p));
          return Json::object({{"type", "MultiPolygon"}, {"arcs", std::move(polygons)}});
        } else {
          if (s.geometries.empty()) return Json::object({{"type", nullptr}});
          Json members = Json::array();
          if (depth < ir::max_geometry_depth) {
            for (const auto& inner : s.geometries) members.push_back(geometry_json(inner, arcs, q, depth + 1));
          }
          return Json::object({{"type", "GeometryCollection"}, {"geometries", std::move(members)}});
        }
      },
      g.shape);
}

Result<Bytes> write_topojson(const ir::Geo& geo, WriteContext& ctx) {
  const auto n = ctx.options.get_int("quantization", 1'000'000, 0, 1'000'000'000);
  Quantizer q;
  const BoundingBox box = bounding_box(geo);
  if (n >= 2 && !box.empty) {
    q.on = true;
    q.x0 = box.min_lon;
    q.y0 = box.min_lat;
    const double w = box.max_lon - box.min_lon, h = box.max_lat - box.min_lat;
    q.kx = w > 0 ? static_cast<double>(n - 1) / w : 1;
    q.ky = h > 0 ? static_cast<double>(n - 1) / h : 1;
  }
  ArcBuilder arcs;
  Json geometries = Json::array();
  for (const ir::Feature& f : geo.features) {
    Json g = geometry_json(f.geometry, arcs, q, 0);
    if (!f.id.is_null()) g.set("id", f.id);
    Json props = f.properties.is_object() ? f.properties : Json::object();
    if (!f.name.empty() && !props.contains("name")) props.set("name", f.name);
    if (!props.empty()) g.set("properties", std::move(props));
    geometries.push_back(std::move(g));
  }
  Json arcs_json = Json::array();
  for (const auto& arc : arcs.arcs) {
    Json a = Json::array();
    std::int64_t px = 0, py = 0;
    for (const ir::Coord& c : arc) {
      if (q.on) {
        const std::int64_t x = q.qx(c.lon), y = q.qy(c.lat);
        Json p = Json::array({x - px, y - py});
        if (c.ele) p.push_back(*c.ele);
        a.push_back(std::move(p));
        px = x;
        py = y;
      } else {
        a.push_back(point_json(c, q));
      }
    }
    arcs_json.push_back(std::move(a));
  }
  Json root = Json::object({{"type", "Topology"}});
  if (q.on) {
    root.set("transform", Json::object({{"scale", Json::array({1.0 / q.kx, 1.0 / q.ky})},
                                        {"translate", Json::array({q.x0, q.y0})}}));
  }
  if (!box.empty) root.set("bbox", Json::array({box.min_lon, box.min_lat, box.max_lon, box.max_lat}));
  const std::string name = geo.name.empty() ? std::string("features") : geo.name;
  root.set("objects", Json::object({{name, Json::object({{"type", "GeometryCollection"}, {"geometries", std::move(geometries)}})}}));
  root.set("arcs", std::move(arcs_json));
  return to_bytes(util::dump_json(root) + "\n");
}

int sniff_topojson(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 4096);
  if (!head.starts_with('{')) return 0;
  return head.find("\"Topology\"") != std::string_view::npos ? 93 : 0;
}

}  // namespace

void register_topojson(Registry& registry) {
  FormatInfo info;
  info.id = "topojson";
  info.name = "TopoJSON";
  info.description = "Geographic topology as JSON: shared, quantised arcs (D3, Mapshaper).";
  info.category = Category::Geo;
  info.extensions = {"topojson", "json"};
  info.mime = "application/topo+json";
  info.popularity = 35;
  info.sniff = sniff_topojson;
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "topojson";
  spec.read = read_topojson;
  spec.write = write_topojson;
  spec.write_options = {OptionSpec::integer("quantization", "Quantisation", 1'000'000, 0, 1'000'000'000,
                                            "Positions per axis of the quantised grid (1e5 is about 1 m for a city, "
                                            "1e6 the TopoJSON default); 0 keeps exact coordinates.")};
  spec.lossy_write = true;
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
