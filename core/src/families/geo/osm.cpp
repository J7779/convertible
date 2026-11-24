// Convertible core - geo family: OpenStreetMap XML (.osm) and PBF (.osm.pbf).
//
// Both readers fill one model of nodes, ways and relations, from which
// features are built: tagged nodes become points; ways become lines, or
// polygons when closed and tagged as an area; multipolygon and boundary
// relations become (multi)polygons assembled from their member ways (joined
// end to end); route relations become multilines. Tags become properties
// and the OSM id the feature id ("node/12", "way/34", "relation/56").
// The PBF reader decodes the protobuf wire format in-house (blobs raw or
// zlib-compressed, string tables, dense nodes, delta-coded references) and
// bounds every length. Writing produces OSM XML 0.6 with fresh ids:
// nodes, ways, and multipolygon relations for polygons with holes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "families/geo/geo.hpp"
#include "families/geo/geo_ops.hpp"
#include "families/geo/protobuf.hpp"
#include "families/geo/xml_geo.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::geo {

namespace {

// --------------------------------------------------------------------------- model

struct OsmWay {
  std::int64_t id = 0;
  std::vector<std::int64_t> refs;
  Json tags = Json::object();
};

struct OsmMember {
  char type = 'n';  // n, w, r
  std::int64_t ref = 0;
  std::string role;
};

struct OsmRelation {
  std::int64_t id = 0;
  std::vector<OsmMember> members;
  Json tags = Json::object();
};

struct OsmData {
  std::unordered_map<std::int64_t, std::pair<double, double>> nodes;  // id -> (lon, lat)
  std::vector<std::pair<std::int64_t, Json>> tagged_nodes;
  std::vector<OsmWay> ways;
  std::vector<OsmRelation> relations;
  std::size_t missing = 0;
};

bool is_area(const Json& tags) {
  if (tags["area"].as_string() == "no") return false;
  if (tags["area"].as_string() == "yes") return true;
  for (std::string_view key : {"building", "landuse", "leisure", "amenity", "place", "shop", "tourism", "boundary",
                               "building:part", "man_made", "military", "aeroway", "historic", "parking"}) {
    if (tags.contains(key)) return true;
  }
  if (const Json* natural = tags.find("natural"); natural != nullptr) {
    const std::string& v = natural->as_string();
    return v != "coastline" && v != "tree_row" && v != "cliff" && v != "ridge";
  }
  if (const Json* water = tags.find("waterway"); water != nullptr) return water->as_string() == "riverbank";
  return false;
}

std::vector<ir::Coord> way_coords(OsmData& d, const std::vector<std::int64_t>& refs) {
  std::vector<ir::Coord> out;
  out.reserve(refs.size());
  for (std::int64_t ref : refs) {
    const auto it = d.nodes.find(ref);
    if (it == d.nodes.end()) {
      ++d.missing;
      continue;
    }
    ir::Coord c;
    c.lon = it->second.first;
    c.lat = it->second.second;
    out.push_back(c);
  }
  return out;
}

// Joins way node lists end to end into closed rings (reversing ways where needed).
std::vector<std::vector<std::int64_t>> join_rings(std::vector<std::vector<std::int64_t>> parts) {
  std::vector<std::vector<std::int64_t>> rings;
  while (!parts.empty()) {
    std::vector<std::int64_t> ring = std::move(parts.back());
    parts.pop_back();
    bool grown = true;
    while (!ring.empty() && ring.front() != ring.back() && grown) {
      grown = false;
      for (std::size_t i = 0; i < parts.size(); ++i) {
        auto& p = parts[i];
        if (p.empty()) continue;
        if (p.front() == ring.back()) {
          ring.insert(ring.end(), p.begin() + 1, p.end());
        } else if (p.back() == ring.back()) {
          ring.insert(ring.end(), p.rbegin() + 1, p.rend());
        } else {
          continue;
        }
        parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(i));
        grown = true;
        break;
      }
    }
    if (ring.size() >= 4 && ring.front() == ring.back()) rings.push_back(std::move(ring));
  }
  return rings;
}

ir::Feature osm_feature(std::string_view type, std::int64_t id, Json tags) {
  ir::Feature f;
  f.id = Json(std::string(type) + "/" + std::to_string(id));
  f.properties = std::move(tags);
  if (const Json* name = f.properties.find("name"); name != nullptr && name->is_string()) f.name = name->as_string();
  return f;
}

Result<ir::Geo> build_features(OsmData& d, const Context& ctx) {
  ir::Geo geo;
  for (auto& [id, tags] : d.tagged_nodes) {
    const auto it = d.nodes.find(id);
    if (it == d.nodes.end()) continue;
    ir::Feature f = osm_feature("node", id, std::move(tags));
    ir::Coord c;
    c.lon = it->second.first;
    c.lat = it->second.second;
    f.geometry.shape = ir::Point{c};
    geo.features.push_back(std::move(f));
  }
  std::unordered_map<std::int64_t, std::size_t> way_index;
  for (std::size_t i = 0; i < d.ways.size(); ++i) way_index[d.ways[i].id] = i;
  std::unordered_set<std::int64_t> member_ways;
  for (const auto& r : d.relations) {
    for (const auto& m : r.members) {
      if (m.type == 'w') member_ways.insert(m.ref);
    }
  }
  std::size_t done = 0;
  for (auto& w : d.ways) {
    if ((++done & 4095) == 0) CVT_TRY(ctx.checkpoint(0.6));
    if (w.tags.empty() && member_ways.contains(w.id)) continue;  // only a part of a relation
    std::vector<ir::Coord> pts = way_coords(d, w.refs);
    if (pts.size() < 2) continue;
    const bool closed = w.refs.size() >= 4 && w.refs.front() == w.refs.back();
    ir::Feature f = osm_feature("way", w.id, w.tags);
    if (closed && is_area(w.tags)) {
      f.geometry.shape = ir::Polygon{{std::move(pts)}};
      orient_rings(f.geometry, true);
    } else {
      f.geometry.shape = ir::LineString{std::move(pts)};
    }
    geo.features.push_back(std::move(f));
  }
  for (auto& r : d.relations) {
    const std::string type = r.tags["type"].as_string();
    if (type == "multipolygon" || type == "boundary") {
      std::vector<std::vector<std::int64_t>> outer_parts, inner_parts;
      for (const auto& m : r.members) {
        if (m.type != 'w') continue;
        const auto it = way_index.find(m.ref);
        if (it == way_index.end()) continue;
        (m.role == "inner" ? inner_parts : outer_parts).push_back(d.ways[it->second].refs);
      }
      std::vector<std::vector<ir::Coord>> rings;
      for (bool inner : {false, true}) {
        for (auto& ring : join_rings(inner ? std::move(inner_parts) : std::move(outer_parts))) {
          std::vector<ir::Coord> pts = way_coords(d, ring);
          if (pts.size() < 4) continue;
          // Outer rings counter-clockwise, inner clockwise, so that assembly can tell them apart.
          if ((signed_area(pts) > 0) == inner) std::reverse(pts.begin(), pts.end());
          rings.push_back(std::move(pts));
        }
      }
      if (rings.empty()) continue;
      Json tags = r.tags;
      tags.erase("type");
      ir::Feature f = osm_feature("relation", r.id, std::move(tags));
      f.geometry = assemble_rings(std::move(rings), false);
      geo.features.push_back(std::move(f));
    } else if (type == "route" || type == "multilinestring") {
      ir::MultiLineString ml;
      for (const auto& m : r.members) {
        if (m.type != 'w') continue;
        const auto it = way_index.find(m.ref);
        if (it == way_index.end()) continue;
        std::vector<ir::Coord> pts = way_coords(d, d.ways[it->second].refs);
        if (pts.size() >= 2) ml.lines.push_back(ir::LineString{std::move(pts)});
      }
      if (ml.lines.empty()) continue;
      ir::Feature f = osm_feature("relation", r.id, r.tags);
      f.geometry.shape = std::move(ml);
      geo.features.push_back(std::move(f));
    }
  }
  if (d.missing > 0) ctx.warn("Some ways refer to nodes that are not in the file (an extract cut at its edge); they were left out.");
  return geo;
}

// --------------------------------------------------------------------------- XML

Result<ir::Geo> read_osm_xml(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::XmlDocument doc, xml::parse(ctx.input, "OpenStreetMap"));
  const util::XmlNode root = doc.root();
  if (root.local_name() != "osm") return fail(Status::Decode, "not an OpenStreetMap XML file");
  OsmData d;
  const auto tags_of = [](util::XmlNode e) {
    Json tags = Json::object();
    for (util::XmlNode t : e.children_local("tag")) {
      if (tags.size() < 1024) tags.set(t.attribute("k").value(), std::string(t.attribute("v").value()));
    }
    return tags;
  };
  std::size_t count = 0;
  for (util::XmlNode e : root.elements()) {
    if ((++count & 8191) == 0) CVT_TRY(ctx.checkpoint(0.3));
    const std::string_view n = e.local_name();
    const std::int64_t id = e.attribute("id").as_int64(0);
    if (n == "node") {
      if (e.attribute("visible").value() == "false") continue;
      const util::XmlAttribute lat = e.attribute("lat"), lon = e.attribute("lon");
      if (!lat || !lon) continue;
      d.nodes[id] = {lon.as_double(), lat.as_double()};
      Json tags = tags_of(e);
      if (!tags.empty()) d.tagged_nodes.emplace_back(id, std::move(tags));
    } else if (n == "way") {
      OsmWay w;
      w.id = id;
      for (util::XmlNode nd : e.children_local("nd")) w.refs.push_back(nd.attribute("ref").as_int64(0));
      w.tags = tags_of(e);
      d.ways.push_back(std::move(w));
    } else if (n == "relation") {
      OsmRelation r;
      r.id = id;
      for (util::XmlNode m : e.children_local("member")) {
        const std::string_view type = m.attribute("type").value();
        r.members.push_back({type.empty() ? 'n' : type.front(), m.attribute("ref").as_int64(0),
                             std::string(m.attribute("role").value())});
      }
      r.tags = tags_of(e);
      d.relations.push_back(std::move(r));
    }
  }
  CVT_TRY_ASSIGN(ir::Geo geo, build_features(d, ctx));
  if (const util::XmlAttribute generator = root.attribute("generator")) {
    geo.metadata.set("creator", std::string(generator.value()));
  }
  return geo;
}

struct OsmWriter {
  std::int64_t next = 1;  // positive ids: GDAL and other readers reject negative ones by default
  std::map<std::pair<double, double>, std::int64_t> node_ids;
  util::XmlNode root;

  std::int64_t node(const ir::Coord& c, const Json* tags) {
    const double lon = std::round(c.lon * 1e7) / 1e7, lat = std::round(c.lat * 1e7) / 1e7;
    if (tags == nullptr) {
      if (auto it = node_ids.find({lon, lat}); it != node_ids.end()) return it->second;
    }
    const std::int64_t id = next++;
    util::XmlNode n = root.append_child("node");
    n.set_attribute("id", id);
    n.set_attribute("visible", "true");
    n.set_attribute("lat", format_number(lat, 7));
    n.set_attribute("lon", format_number(lon, 7));
    if (tags != nullptr) {
      add_tags(n, *tags);
    } else {
      node_ids[{lon, lat}] = id;
    }
    return id;
  }
  static void add_tags(util::XmlNode e, const Json& tags) {
    for (const auto& m : tags.items()) {
      if (m.value().is_null()) continue;
      util::XmlNode t = e.append_child("tag");
      t.set_attribute("k", m.key());
      t.set_attribute("v", json_to_text(m.value()));
    }
  }
  std::int64_t way(const std::vector<ir::Coord>& pts, const Json* tags, bool closed) {
    std::vector<std::int64_t> refs;
    for (std::size_t i = 0; i < pts.size(); ++i) {
      if (closed && i + 1 == pts.size() && !refs.empty()) {
        refs.push_back(refs.front());
        break;
      }
      const std::int64_t id = node(pts[i], nullptr);
      if (refs.empty() || refs.back() != id) refs.push_back(id);
    }
    const std::int64_t id = next++;
    pending_ways.push_back({id, std::move(refs), tags != nullptr ? *tags : Json::object()});
    return id;
  }
  struct PendingWay {
    std::int64_t id;
    std::vector<std::int64_t> refs;
    Json tags;
  };
  std::vector<PendingWay> pending_ways;
  struct PendingRelation {
    std::int64_t id;
    std::vector<std::pair<std::int64_t, std::string>> members;
    Json tags;
  };
  std::vector<PendingRelation> pending_relations;
};

Json tags_for(const ir::Feature& f) {
  Json tags = Json::object();
  if (!f.name.empty() && !f.properties.contains("name")) tags.set("name", f.name);
  for (const auto& m : f.properties.items()) tags.set(m.key(), m.value());
  return tags;
}

void add_polygons(OsmWriter& w, const std::vector<const ir::Polygon*>& polygons, const Json& tags) {
  if (polygons.size() == 1 && polygons[0]->rings.size() == 1) {
    w.way(polygons[0]->rings[0], &tags, true);
    return;
  }
  OsmWriter::PendingRelation r;
  r.id = w.next++;
  for (const ir::Polygon* p : polygons) {
    for (std::size_t i = 0; i < p->rings.size(); ++i) {
      const std::int64_t way = w.way(p->rings[i], nullptr, true);
      r.members.emplace_back(way, i == 0 ? "outer" : "inner");
    }
  }
  r.tags = tags;
  r.tags.set("type", "multipolygon");
  w.pending_relations.push_back(std::move(r));
}

void add_osm_geometry(OsmWriter& w, const ir::Geometry& g, const Json& tags, int depth) {
  if (depth > ir::max_geometry_depth) return;
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ir::Point>) {
          w.node(s.coord, &tags);
        } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
          for (const auto& c : s.points) w.node(c, &tags);
        } else if constexpr (std::is_same_v<T, ir::LineString>) {
          if (s.points.size() >= 2) w.way(s.points, &tags, false);
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          for (const auto& l : s.lines) {
            if (l.points.size() >= 2) w.way(l.points, &tags, false);
          }
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          add_polygons(w, {&s}, tags);
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          std::vector<const ir::Polygon*> polygons;
          for (const auto& p : s.polygons) polygons.push_back(&p);
          if (!polygons.empty()) add_polygons(w, polygons, tags);
        } else {
          for (const auto& inner : s.geometries) add_osm_geometry(w, inner, tags, depth + 1);
        }
      },
      g.shape);
}

Result<Bytes> write_osm(const ir::Geo& geo, WriteContext& ctx) {
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  OsmWriter w;
  w.root = doc.append_child("osm");
  w.root.set_attribute("version", "0.6");
  w.root.set_attribute("upload", "false");
  std::size_t index = 0;
  for (const ir::Feature& f : geo.features) {
    if ((++index & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(geo.features.size())));
    add_osm_geometry(w, f.geometry, tags_for(f), 0);
  }
  // Ways after all nodes and relations after all ways, as OSM files order them.
  for (const auto& p : w.pending_ways) {
    util::XmlNode way = w.root.append_child("way");
    way.set_attribute("id", p.id);
    way.set_attribute("visible", "true");
    for (std::int64_t ref : p.refs) way.append_child("nd").set_attribute("ref", ref);
    OsmWriter::add_tags(way, p.tags);
  }
  for (const auto& p : w.pending_relations) {
    util::XmlNode relation = w.root.append_child("relation");
    relation.set_attribute("id", p.id);
    relation.set_attribute("visible", "true");
    for (const auto& [ref, role] : p.members) {
      util::XmlNode m = relation.append_child("member");
      m.set_attribute("type", "way");
      m.set_attribute("ref", ref);
      m.set_attribute("role", role);
    }
    OsmWriter::add_tags(relation, p.tags);
  }
  return xml::finish(doc);
}

int sniff_osm(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 2048);
  return head.starts_with('<') && head.find("<osm") != std::string_view::npos ? 90 : 0;
}

// --------------------------------------------------------------------------- PBF (protobuf wire format)

using Pbf = pbf::Reader;
using pbf::packed;

struct Block {
  std::vector<std::string> strings;
  std::int64_t granularity = 100, lat_offset = 0, lon_offset = 0;
};

Json tags_from(const std::vector<std::uint32_t>& keys, const std::vector<std::uint32_t>& vals, const Block& b) {
  Json tags = Json::object();
  for (std::size_t i = 0; i < keys.size() && i < vals.size(); ++i) {
    if (keys[i] < b.strings.size() && vals[i] < b.strings.size() && tags.size() < 1024) {
      tags.set(b.strings[keys[i]], b.strings[vals[i]]);
    }
  }
  return tags;
}

Result<void> read_primitive_group(ByteView group, const Block& b, OsmData& d) {
  Pbf g(group);
  std::uint32_t field = 0, wire = 0;
  const auto lon_of = [&](std::int64_t v) { return 1e-9 * static_cast<double>(b.lon_offset + b.granularity * v); };
  const auto lat_of = [&](std::int64_t v) { return 1e-9 * static_cast<double>(b.lat_offset + b.granularity * v); };
  while (g.next(field, wire)) {
    if (field == 1 && wire == 2) {  // Node
      Pbf n(g.bytes());
      std::int64_t id = 0, lat = 0, lon = 0;
      std::vector<std::uint32_t> keys, vals;
      std::uint32_t f = 0, w = 0;
      while (n.next(f, w)) {
        if (f == 1) id = n.svarint();
        else if (f == 2) packed(n, w, [&](Pbf& x) { keys.push_back(static_cast<std::uint32_t>(x.varint())); });
        else if (f == 3) packed(n, w, [&](Pbf& x) { vals.push_back(static_cast<std::uint32_t>(x.varint())); });
        else if (f == 8) lat = n.svarint();
        else if (f == 9) lon = n.svarint();
        else n.skip(w);
      }
      if (!n.ok) return fail(Status::Decode, "damaged OSM PBF node");
      d.nodes[id] = {lon_of(lon), lat_of(lat)};
      Json tags = tags_from(keys, vals, b);
      if (!tags.empty()) d.tagged_nodes.emplace_back(id, std::move(tags));
    } else if (field == 2 && wire == 2) {  // DenseNodes
      Pbf dn(g.bytes());
      std::vector<std::int64_t> ids, lats, lons;
      std::vector<std::uint32_t> kv;
      std::uint32_t f = 0, w = 0;
      while (dn.next(f, w)) {
        if (f == 1) packed(dn, w, [&](Pbf& x) { ids.push_back(x.svarint()); });
        else if (f == 8) packed(dn, w, [&](Pbf& x) { lats.push_back(x.svarint()); });
        else if (f == 9) packed(dn, w, [&](Pbf& x) { lons.push_back(x.svarint()); });
        else if (f == 10) packed(dn, w, [&](Pbf& x) { kv.push_back(static_cast<std::uint32_t>(x.varint())); });
        else dn.skip(w);
      }
      if (!dn.ok || ids.size() != lats.size() || ids.size() != lons.size()) {
        return fail(Status::Decode, "damaged OSM PBF dense nodes");
      }
      std::int64_t id = 0, lat = 0, lon = 0;
      std::size_t k = 0;
      for (std::size_t i = 0; i < ids.size(); ++i) {
        id += ids[i];
        lat += lats[i];
        lon += lons[i];
        d.nodes[id] = {lon_of(lon), lat_of(lat)};
        Json tags = Json::object();
        while (k < kv.size() && kv[k] != 0) {
          if (k + 1 < kv.size() && kv[k] < b.strings.size() && kv[k + 1] < b.strings.size() && tags.size() < 1024) {
            tags.set(b.strings[kv[k]], b.strings[kv[k + 1]]);
          }
          k += 2;
        }
        ++k;  // the 0 separator
        if (!tags.empty()) d.tagged_nodes.emplace_back(id, std::move(tags));
      }
    } else if (field == 3 && wire == 2) {  // Way
      Pbf wy(g.bytes());
      OsmWay way;
      std::vector<std::uint32_t> keys, vals;
      std::int64_t ref = 0;
      std::uint32_t f = 0, w = 0;
      while (wy.next(f, w)) {
        if (f == 1) way.id = static_cast<std::int64_t>(wy.varint());
        else if (f == 2) packed(wy, w, [&](Pbf& x) { keys.push_back(static_cast<std::uint32_t>(x.varint())); });
        else if (f == 3) packed(wy, w, [&](Pbf& x) { vals.push_back(static_cast<std::uint32_t>(x.varint())); });
        else if (f == 8) packed(wy, w, [&](Pbf& x) {
          ref += x.svarint();
          way.refs.push_back(ref);
        });
        else wy.skip(w);
      }
      if (!wy.ok) return fail(Status::Decode, "damaged OSM PBF way");
      way.tags = tags_from(keys, vals, b);
      d.ways.push_back(std::move(way));
    } else if (field == 4 && wire == 2) {  // Relation
      Pbf rl(g.bytes());
      OsmRelation rel;
      std::vector<std::uint32_t> keys, vals, roles, types;
      std::vector<std::int64_t> ids;
      std::int64_t mem = 0;
      std::uint32_t f = 0, w = 0;
      while (rl.next(f, w)) {
        if (f == 1) rel.id = static_cast<std::int64_t>(rl.varint());
        else if (f == 2) packed(rl, w, [&](Pbf& x) { keys.push_back(static_cast<std::uint32_t>(x.varint())); });
        else if (f == 3) packed(rl, w, [&](Pbf& x) { vals.push_back(static_cast<std::uint32_t>(x.varint())); });
        else if (f == 8) packed(rl, w, [&](Pbf& x) { roles.push_back(static_cast<std::uint32_t>(x.varint())); });
        else if (f == 9) packed(rl, w, [&](Pbf& x) {
          mem += x.svarint();
          ids.push_back(mem);
        });
        else if (f == 10) packed(rl, w, [&](Pbf& x) { types.push_back(static_cast<std::uint32_t>(x.varint())); });
        else rl.skip(w);
      }
      if (!rl.ok) return fail(Status::Decode, "damaged OSM PBF relation");
      for (std::size_t i = 0; i < ids.size(); ++i) {
        OsmMember m;
        m.ref = ids[i];
        const std::uint32_t t = i < types.size() ? types[i] : 0;
        m.type = t == 1 ? 'w' : t == 2 ? 'r' : 'n';
        if (i < roles.size() && roles[i] < b.strings.size()) m.role = b.strings[roles[i]];
        rel.members.push_back(std::move(m));
      }
      rel.tags = tags_from(keys, vals, b);
      d.relations.push_back(std::move(rel));
    } else {
      g.skip(wire);
    }
  }
  if (!g.ok) return fail(Status::Decode, "damaged OSM PBF primitive group");
  return {};
}

Result<void> read_primitive_block(ByteView data, OsmData& d) {
  Pbf m(data);
  Block b;
  std::vector<ByteView> groups;
  std::uint32_t field = 0, wire = 0;
  while (m.next(field, wire)) {
    if (field == 1 && wire == 2) {
      Pbf st(m.bytes());
      std::uint32_t f = 0, w = 0;
      while (st.next(f, w)) {
        if (f == 1 && w == 2) {
          const ByteView s = st.bytes();
          b.strings.push_back(util::sanitize_utf8(as_chars(s)));
        } else {
          st.skip(w);
        }
      }
      if (!st.ok) return fail(Status::Decode, "damaged OSM PBF string table");
    } else if (field == 2 && wire == 2) {
      groups.push_back(m.bytes());
    } else if (field == 17 && wire == 0) {
      b.granularity = static_cast<std::int64_t>(m.varint());
    } else if (field == 19 && wire == 0) {
      b.lat_offset = static_cast<std::int64_t>(m.varint());
    } else if (field == 20 && wire == 0) {
      b.lon_offset = static_cast<std::int64_t>(m.varint());
    } else {
      m.skip(wire);
    }
  }
  if (!m.ok) return fail(Status::Decode, "damaged OSM PBF block");
  if (b.granularity <= 0 || b.granularity > 1'000'000'000) return fail(Status::Decode, "invalid OSM PBF granularity");
  for (ByteView g : groups) CVT_TRY(read_primitive_group(g, b, d));
  return {};
}

Result<ir::Geo> read_osm_pbf(ReadContext& ctx) {
  OsmData d;
  util::ByteReader r(ctx.input);
  std::uint64_t inflated = 0;
  bool header_seen = false;
  while (!r.at_end()) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(r.position()) / static_cast<double>(ctx.input.size()) * 0.6));
    const std::uint32_t header_size = r.u32be();
    if (!r.ok() || header_size > 64 * 1024) return fail(Status::Decode, "invalid OSM PBF blob header size");
    const ByteView header = r.bytes(header_size);
    if (!r.ok()) return fail(Status::Decode, "the OSM PBF file is truncated");
    Pbf h(header);
    std::string type;
    std::uint64_t data_size = 0;
    std::uint32_t field = 0, wire = 0;
    while (h.next(field, wire)) {
      if (field == 1 && wire == 2) type = std::string(as_chars(h.bytes()));
      else if (field == 3 && wire == 0) data_size = h.varint();
      else h.skip(wire);
    }
    if (!h.ok || data_size > 32u * 1024 * 1024) return fail(Status::Decode, "invalid OSM PBF blob header");
    const ByteView blob_bytes = r.bytes(static_cast<std::size_t>(data_size));
    if (!r.ok()) return fail(Status::Decode, "the OSM PBF file is truncated");
    Pbf blob(blob_bytes);
    ByteView raw, zlib;
    std::uint64_t raw_size = 0;
    bool unsupported = false;
    while (blob.next(field, wire)) {
      if (field == 1 && wire == 2) raw = blob.bytes();
      else if (field == 2 && wire == 0) raw_size = blob.varint();
      else if (field == 3 && wire == 2) zlib = blob.bytes();
      else if (field >= 4 && field <= 7 && wire == 2) {
        (void)blob.bytes();
        unsupported = true;
      } else {
        blob.skip(wire);
      }
    }
    if (!blob.ok) return fail(Status::Decode, "damaged OSM PBF blob");
    Bytes content;
    if (!zlib.empty()) {
      if (raw_size > 32u * 1024 * 1024) return fail(Status::Decode, "an OSM PBF blob declares an absurd size");
      CVT_TRY(ctx.limits.check_decompressed(inflated + raw_size));
      CVT_TRY_ASSIGN(content, util::zlib_decompress(zlib, raw_size));
      inflated += content.size();
      raw = content;
    } else if (unsupported) {
      return fail(Status::Decode, "the OSM PBF file uses a compression Convertible does not support (only zlib)");
    }
    if (type == "OSMHeader") {
      header_seen = true;
      Pbf hb(raw);
      while (hb.next(field, wire)) {
        if (field == 4 && wire == 2) {
          const std::string feature(as_chars(hb.bytes()));
          if (feature != "OsmSchema-V0.6" && feature != "DenseNodes" && feature != "HistoricalInformation") {
            return fail(Status::Decode, "the OSM PBF file needs the unsupported feature '{}'",
                        util::sanitize_utf8(feature.substr(0, 60)));
          }
        } else {
          hb.skip(wire);
        }
      }
      if (!hb.ok) return fail(Status::Decode, "damaged OSM PBF header block");
    } else if (type == "OSMData") {
      if (!header_seen) return fail(Status::Decode, "the OSM PBF data comes before its header");
      CVT_TRY(read_primitive_block(raw, d));
    }
  }
  if (!header_seen) return fail(Status::Decode, "not an OSM PBF file");
  return build_features(d, ctx);
}

int sniff_osm_pbf(ByteView data, std::string_view) {
  if (data.size() < 20) return 0;
  const std::string_view head = as_chars(data.first(std::min<std::size_t>(data.size(), 32)));
  return head.find("OSMHeader") != std::string_view::npos && head.find("OSMHeader") <= 8 ? 96 : 0;
}

}  // namespace

void register_osm(Registry& registry) {
  FormatInfo info;
  info.id = "osm";
  info.name = "OpenStreetMap XML";
  info.description = "OpenStreetMap nodes, ways and relations as XML (API 0.6, editor exports).";
  info.category = Category::Geo;
  info.extensions = {"osm"};
  info.mime = "application/vnd.openstreetmap.data+xml";
  info.popularity = 40;
  info.sniff = sniff_osm;
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "osm";
  spec.read = read_osm_xml;
  spec.write = write_osm;
  spec.lossy_write = true;
  add_geo_codec(registry, std::move(spec));

  FormatInfo pbf;
  pbf.id = "osm.pbf";
  pbf.name = "OpenStreetMap PBF";
  pbf.description = "OpenStreetMap data in the compressed protocol-buffer format (planet and regional extracts).";
  pbf.category = Category::Geo;
  pbf.extensions = {"osm.pbf", "pbf"};
  pbf.mime = "application/vnd.openstreetmap.data+pbf";
  pbf.popularity = 35;
  pbf.sniff = sniff_osm_pbf;
  registry.add_format(std::move(pbf));
  GeoCodecSpec pbf_spec;
  pbf_spec.format_id = "osm.pbf";
  pbf_spec.read = read_osm_pbf;
  add_geo_codec(registry, std::move(pbf_spec));
}

}  // namespace cvt::geo
