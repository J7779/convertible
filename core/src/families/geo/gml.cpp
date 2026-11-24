// Convertible core - geo family: GML simple features and GeoRSS.
//
// GML reading (2.1 and 3.x): feature collections (featureMember,
// featureMembers, wfs:member) or a single feature or geometry; Point,
// LineString, LinearRing, Polygon, Multi*, MultiCurve / MultiSurface,
// Curve / Surface with LineStringSegment / PolygonPatch, MultiGeometry;
// coordinates from pos, posList (with srsDimension), coordinates and coord.
// Axis order follows the srsName: geographic CRSs named by URN or URL
// ("urn:ogc:def:crs:EPSG::4326", "http://www.opengis.net/def/crs/EPSG/0/4326")
// are latitude first, the short "EPSG:4326" form and projected CRSs x first.
// Geometries in a projected CRS Convertible knows are converted to WGS 84 as
// they are read; others keep their coordinates and CRS. xlink references
// are never followed. Simple child elements become typed properties.
// GML writing: a GML 3.2 feature collection.
// GeoRSS reading: RSS 2.0, RSS 1.0 and Atom items with GeoRSS Simple
// (point, line, polygon, box, elev), GeoRSS GML (where) or W3C geo:lat/long.
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
#include "families/geo/xml_geo.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::geo {

namespace {

constexpr std::string_view gml32_ns = "http://www.opengis.net/gml/3.2";
constexpr std::string_view feature_ns = "urn:x-geo:features";

enum class AxisOrder { Auto, LonLat, LatLon };

// How to read the ordinates of positions in a given srsName.
struct Srs {
  std::string name;
  bool lat_first = false;
  std::optional<Crs> crs;  // known CRS; nullopt when unknown (coordinates are kept)
};

Srs srs_from(std::string_view name, AxisOrder forced, bool default_lat_first) {
  Srs s;
  s.name = std::string(util::trim(name));
  if (s.name.empty()) {
    s.crs = Crs{};
    s.lat_first = forced == AxisOrder::Auto ? default_lat_first : forced == AxisOrder::LatLon;
    return s;
  }
  s.crs = parse_crs(s.name);
  const bool long_form = util::istarts_with(s.name, "urn:") || util::istarts_with(s.name, "http://www.opengis.net/def/") ||
                         util::istarts_with(s.name, "https://www.opengis.net/def/");
  const bool geographic = s.crs && s.crs->kind == CrsKind::Geographic && util::to_lower(s.name).find("crs84") == std::string::npos;
  s.lat_first = geographic && long_form;
  if (forced != AxisOrder::Auto && (!s.crs || s.crs->kind == CrsKind::Geographic)) s.lat_first = forced == AxisOrder::LatLon;
  return s;
}

struct GmlReader {
  AxisOrder forced = AxisOrder::Auto;
  bool default_lat_first = false;  // GeoRSS GML: latitude first unless an srsName says otherwise
  std::string unknown_crs;
  bool xlinks = false;
};

std::size_t dimension_of(util::XmlNode node, std::size_t inherited) {
  for (util::XmlNode n = node; n; n = n.parent()) {
    if (const util::XmlAttribute a = n.attribute("srsDimension")) {
      const unsigned d = a.as_uint(0);
      if (d >= 2 && d <= 4) return d;
    }
    if (n == node && inherited != 0) break;
  }
  return inherited == 0 ? 2 : inherited;
}

Result<std::vector<ir::Coord>> numbers_to_coords(std::string_view text, std::size_t dim, const Srs& srs) {
  std::vector<double> values;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && util::is_ascii_space(text[i])) ++i;
    const std::size_t start = i;
    while (i < text.size() && !util::is_ascii_space(text[i])) ++i;
    if (i == start) break;
    const auto v = parse_number(text.substr(start, i - start));
    if (!v) return fail(Status::Decode, "invalid number in a GML coordinate list");
    values.push_back(*v);
  }
  if (dim < 2 || values.size() % dim != 0) return fail(Status::Decode, "a GML coordinate list does not match its dimension");
  std::vector<ir::Coord> out;
  out.reserve(values.size() / dim);
  for (std::size_t k = 0; k < values.size(); k += dim) {
    ir::Coord c;
    c.lon = srs.lat_first ? values[k + 1] : values[k];
    c.lat = srs.lat_first ? values[k] : values[k + 1];
    if (dim >= 3) c.ele = values[k + 2];
    out.push_back(c);
  }
  return out;
}

// GML 2 <coordinates>: tuples separated by spaces, values by commas (cs / ts / decimal attributes honoured).
Result<std::vector<ir::Coord>> gml2_coordinates(util::XmlNode node, const Srs& srs) {
  std::string_view cs = node.attribute("cs").value();
  std::string_view ts = node.attribute("ts").value();
  std::string_view decimal = node.attribute("decimal").value();
  if (cs.empty()) cs = ",";
  if (decimal.empty()) decimal = ".";
  const std::string text = node.text();
  std::string normalised;
  for (char c : text) {
    if (!ts.empty() && c == ts.front()) normalised += ' ';
    else if (c == cs.front()) normalised += ',';
    else if (c == decimal.front()) normalised += '.';
    else normalised += c;
  }
  std::vector<ir::Coord> out;
  for (std::string_view tuple : util::split(util::trim(normalised), ' ', false)) {
    tuple = util::trim(tuple);
    if (tuple.empty()) continue;
    const auto parts = util::split(tuple, ',');
    if (parts.size() < 2) return fail(Status::Decode, "a GML coordinate tuple needs two values");
    const auto a = parse_number(parts[0]);
    const auto b = parse_number(parts[1]);
    if (!a || !b) return fail(Status::Decode, "invalid number in GML coordinates");
    ir::Coord c;
    c.lon = srs.lat_first ? *b : *a;
    c.lat = srs.lat_first ? *a : *b;
    if (parts.size() >= 3) {
      if (auto z = parse_number(parts[2])) c.ele = *z;
    }
    out.push_back(c);
  }
  return out;
}

Result<std::vector<ir::Coord>> sequence_of(util::XmlNode node, const Srs& srs) {
  const std::size_t dim = dimension_of(node, 0);
  std::vector<ir::Coord> out;
  for (util::XmlNode e : node.elements()) {
    const std::string_view n = e.local_name();
    if (n == "posList") {
      CVT_TRY_ASSIGN(auto pts, numbers_to_coords(e.text(), dimension_of(e, dim), srs));
      out.insert(out.end(), pts.begin(), pts.end());
    } else if (n == "pos") {
      const std::string text = e.text();
      const std::size_t count = util::split(util::trim(text), ' ', false).size();
      CVT_TRY_ASSIGN(auto pts, numbers_to_coords(text, std::max<std::size_t>(2, count), srs));
      out.insert(out.end(), pts.begin(), pts.end());
    } else if (n == "coordinates") {
      CVT_TRY_ASSIGN(auto pts, gml2_coordinates(e, srs));
      out.insert(out.end(), pts.begin(), pts.end());
    } else if (n == "coord") {
      ir::Coord c;
      const auto x = xml::child_number(e, "X");
      const auto y = xml::child_number(e, "Y");
      if (!x || !y) return fail(Status::Decode, "a GML coord needs X and Y");
      c.lon = srs.lat_first ? *y : *x;
      c.lat = srs.lat_first ? *x : *y;
      if (auto z = xml::child_number(e, "Z")) c.ele = *z;
      out.push_back(c);
    } else if (n == "pointProperty" || n == "pointRep") {
      if (const util::XmlNode p = e.child_local("Point")) {
        CVT_TRY_ASSIGN(auto pts, sequence_of(p, srs));
        out.insert(out.end(), pts.begin(), pts.end());
      }
    }
  }
  return out;
}

void close_ring(std::vector<ir::Coord>& ring) {
  if (!ring.empty() && !(ring.front().lon == ring.back().lon && ring.front().lat == ring.back().lat)) ring.push_back(ring.front());
}

Result<std::vector<ir::Coord>> ring_of(util::XmlNode boundary, const Srs& srs) {
  util::XmlNode ring = boundary.child_local("LinearRing");
  if (!ring) ring = boundary.child_local("Ring");
  std::vector<ir::Coord> pts;
  if (ring && ring.local_name() == "Ring") {
    // Ring of curve members.
    for (util::XmlNode member : ring.children_local("curveMember")) {
      for (util::XmlNode curve : member.elements()) {
        CVT_TRY_ASSIGN(auto part, sequence_of(curve.local_name() == "Curve" ? curve.child_local("segments").child_local("LineStringSegment") : curve, srs));
        if (!pts.empty() && !part.empty()) part.erase(part.begin());
        pts.insert(pts.end(), part.begin(), part.end());
      }
    }
  } else {
    CVT_TRY_ASSIGN(pts, sequence_of(ring ? ring : boundary, srs));
  }
  close_ring(pts);
  return pts;
}

Result<ir::Polygon> polygon_of(util::XmlNode node, const Srs& srs) {
  ir::Polygon p;
  for (util::XmlNode e : node.elements()) {
    const std::string_view n = e.local_name();
    if (n == "exterior" || n == "outerBoundaryIs") {
      CVT_TRY_ASSIGN(auto ring, ring_of(e, srs));
      p.rings.insert(p.rings.begin(), std::move(ring));
    } else if (n == "interior" || n == "innerBoundaryIs") {
      CVT_TRY_ASSIGN(auto ring, ring_of(e, srs));
      p.rings.push_back(std::move(ring));
    }
  }
  return p;
}

bool is_geometry_name(std::string_view n) {
  static constexpr std::string_view names[] = {
      "Point",        "LineString",  "LinearRing",      "Polygon",    "MultiPoint", "MultiLineString",
      "MultiPolygon", "MultiCurve",  "MultiSurface",    "Curve",      "Surface",    "MultiGeometry",
      "CompositeCurve", "CompositeSurface", "Envelope", "Box",        "OrientableSurface", "PolyhedralSurface",
      "TriangulatedSurface", "Tin"};
  return std::find(std::begin(names), std::end(names), n) != std::end(names);
}

Result<ir::Geometry> gml_geometry(GmlReader& r, util::XmlNode node, const Srs& inherited, int depth);
void to_wgs84(ir::Geometry& g, const Srs& srs, GmlReader& r);

// Members of a multi geometry: "*Member" with one child, "*Members" with several.
Result<std::vector<ir::Geometry>> members_of(GmlReader& r, util::XmlNode node, const Srs& srs, int depth) {
  std::vector<ir::Geometry> out;
  for (util::XmlNode e : node.elements()) {
    const std::string_view n = e.local_name();
    if (!(n.ends_with("Member") || n.ends_with("Members") || n == "patches" || n == "segments")) continue;
    if (e.attribute_local("href")) r.xlinks = true;
    for (util::XmlNode g : e.elements()) {
      const std::string_view gn = g.local_name();
      if (gn == "PolygonPatch" || gn == "Triangle" || gn == "Rectangle") {
        CVT_TRY_ASSIGN(ir::Polygon p, polygon_of(g, srs));
        out.push_back(ir::Geometry{std::move(p)});
        to_wgs84(out.back(), srs, r);
      } else if (gn == "LineStringSegment") {
        CVT_TRY_ASSIGN(auto pts, sequence_of(g, srs));
        out.push_back(ir::Geometry{ir::LineString{std::move(pts)}});
        to_wgs84(out.back(), srs, r);
      } else if (is_geometry_name(gn)) {
        CVT_TRY_ASSIGN(ir::Geometry inner, gml_geometry(r, g, srs, depth + 1));
        out.push_back(std::move(inner));
      }
    }
  }
  return out;
}

ir::Geometry combine(std::vector<ir::Geometry> members, int kind) {
  // kind: 0 points, 1 lines, 2 polygons, 3 anything
  ir::Geometry g;
  if (kind == 0) {
    ir::MultiPoint mp;
    for (auto& m : members) {
      if (auto* p = std::get_if<ir::Point>(&m.shape)) mp.points.push_back(p->coord);
    }
    g.shape = std::move(mp);
  } else if (kind == 1) {
    ir::MultiLineString ml;
    for (auto& m : members) {
      if (auto* l = std::get_if<ir::LineString>(&m.shape)) ml.lines.push_back(std::move(*l));
      else if (auto* mls = std::get_if<ir::MultiLineString>(&m.shape)) {
        for (auto& l2 : mls->lines) ml.lines.push_back(std::move(l2));
      }
    }
    g.shape = std::move(ml);
  } else if (kind == 2) {
    ir::MultiPolygon mp;
    for (auto& m : members) {
      if (auto* p = std::get_if<ir::Polygon>(&m.shape)) mp.polygons.push_back(std::move(*p));
      else if (auto* mps = std::get_if<ir::MultiPolygon>(&m.shape)) {
        for (auto& p2 : mps->polygons) mp.polygons.push_back(std::move(p2));
      }
    }
    g.shape = std::move(mp);
  } else {
    g.shape = ir::GeometryCollection{std::move(members)};
  }
  return g;
}

// Converts a geometry read in a known projected CRS to WGS 84.
void to_wgs84(ir::Geometry& g, const Srs& srs, GmlReader& r) {
  if (!srs.crs) {
    if (r.unknown_crs.empty()) r.unknown_crs = srs.name;
    return;
  }
  if (srs.crs->kind == CrsKind::Geographic) return;
  for_each_coord(g, [&](ir::Coord& c) {
    const XY ll = unproject(*srs.crs, c.lon, c.lat);
    c.lon = ll.x;
    c.lat = ll.y;
  });
}

Result<ir::Geometry> gml_geometry(GmlReader& r, util::XmlNode node, const Srs& inherited, int depth) {
  if (depth > ir::max_geometry_depth) return fail(Status::Limit, "GML geometries are nested too deeply");
  Srs srs = inherited;
  if (const util::XmlAttribute name = node.attribute("srsName")) srs = srs_from(name.value(), r.forced, r.default_lat_first);
  if (node.attribute_local("href") && !node.first_child()) r.xlinks = true;
  ir::Geometry g;
  const std::string_view n = node.local_name();
  if (n == "Point") {
    CVT_TRY_ASSIGN(auto pts, sequence_of(node, srs));
    if (pts.empty()) g.shape = ir::MultiPoint{};
    else g.shape = ir::Point{pts.front()};
  } else if (n == "LineString" || n == "LineStringSegment") {
    CVT_TRY_ASSIGN(auto pts, sequence_of(node, srs));
    g.shape = ir::LineString{std::move(pts)};
  } else if (n == "LinearRing") {
    CVT_TRY_ASSIGN(auto pts, sequence_of(node, srs));
    close_ring(pts);
    g.shape = ir::Polygon{{std::move(pts)}};
  } else if (n == "Polygon") {
    CVT_TRY_ASSIGN(ir::Polygon p, polygon_of(node, srs));
    g.shape = std::move(p);
  } else if (n == "Envelope" || n == "Box") {
    std::vector<ir::Coord> corners;
    if (node.child_local("lowerCorner")) {
      CVT_TRY_ASSIGN(auto lo, numbers_to_coords(node.child_local("lowerCorner").text(), 2, srs));
      CVT_TRY_ASSIGN(auto hi, numbers_to_coords(node.child_local("upperCorner").text(), 2, srs));
      corners.insert(corners.end(), lo.begin(), lo.end());
      corners.insert(corners.end(), hi.begin(), hi.end());
    } else {
      CVT_TRY_ASSIGN(corners, sequence_of(node, srs));
    }
    if (corners.size() < 2) return fail(Status::Decode, "a GML envelope needs two corners");
    const ir::Coord a = corners[0], b = corners[1];
    ir::Coord p1 = a, p2 = a, p3 = b, p4 = a;
    p2.lon = b.lon;
    p4.lat = b.lat;
    g.shape = ir::Polygon{{{p1, p2, p3, p4, p1}}};
  } else {
    CVT_TRY_ASSIGN(auto members, members_of(r, node, srs, depth));
    if (n == "MultiPoint") {
      g = combine(std::move(members), 0);
    } else if (n == "MultiLineString" || n == "MultiCurve" || n == "CompositeCurve") {
      g = combine(std::move(members), 1);
    } else if (n == "Curve") {
      g = combine(std::move(members), 1);
      auto& ml = std::get<ir::MultiLineString>(g.shape);
      if (ml.lines.size() == 1) {
        ir::LineString only = std::move(ml.lines.front());
        g.shape = std::move(only);
      }
    } else if (n == "MultiPolygon" || n == "MultiSurface" || n == "CompositeSurface" || n == "PolyhedralSurface" ||
               n == "TriangulatedSurface" || n == "Tin") {
      g = combine(std::move(members), 2);
    } else if (n == "Surface" || n == "OrientableSurface") {
      g = combine(std::move(members), 2);
      auto& mp = std::get<ir::MultiPolygon>(g.shape);
      if (mp.polygons.size() == 1) {
        ir::Polygon only = std::move(mp.polygons.front());
        g.shape = std::move(only);
      }
    } else {
      g = combine(std::move(members), 3);
    }
    return g;  // members were already converted
  }
  to_wgs84(g, srs, r);
  return g;
}

// The geometry inside a property element, if any.
util::XmlNode geometry_child(util::XmlNode property) {
  for (util::XmlNode e : property.elements()) {
    if (is_geometry_name(e.local_name())) return e;
  }
  return {};
}

bool has_element_children(util::XmlNode node) {
  for (util::XmlNode e : node.elements()) {
    (void)e;
    return true;
  }
  return false;
}

Result<ir::Feature> gml_feature(GmlReader& r, util::XmlNode node, const Srs& srs) {
  ir::Feature f;
  if (const util::XmlAttribute id = node.attribute_local("id")) f.id = Json(std::string(id.value()));
  else if (const util::XmlAttribute fid = node.attribute("fid")) f.id = Json(std::string(fid.value()));
  bool found = false;
  for (util::XmlNode e : node.elements()) {
    const std::string_view n = e.local_name();
    if (n == "boundedBy") continue;
    if (const util::XmlNode g = geometry_child(e)) {
      if (!found) {
        CVT_TRY_ASSIGN(f.geometry, gml_geometry(r, g, srs, 0));
        found = true;
      }
      continue;
    }
    if (has_element_children(e)) continue;
    if (e.attribute_local("nil").as_bool(false)) {
      f.properties.set(n, Json());
      continue;
    }
    if (f.properties.size() < 4096) f.properties.set(n, text_to_json(e.text()));
  }
  if (!found) f.geometry.shape = ir::GeometryCollection{};
  if (const Json* name = f.properties.find("name"); name != nullptr && name->is_string()) f.name = name->as_string();
  return f;
}

Srs collection_srs(const GmlReader& r, util::XmlNode root) {
  if (const util::XmlNode env = root.child_local("boundedBy").child_local("Envelope")) {
    if (const util::XmlAttribute name = env.attribute("srsName")) return srs_from(name.value(), r.forced, false);
  }
  return srs_from("", r.forced, false);
}

AxisOrder axis_option(const Options& options) {
  const std::string a = options.get_enum("axis_order", "auto", {"auto", "lonlat", "latlon"});
  return a == "lonlat" ? AxisOrder::LonLat : a == "latlon" ? AxisOrder::LatLon : AxisOrder::Auto;
}

Result<ir::Geo> read_gml(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::XmlDocument doc, xml::parse(ctx.input, "GML"));
  GmlReader r;
  r.forced = axis_option(ctx.options);
  const util::XmlNode root = doc.root();
  const Srs srs = collection_srs(r, root);
  ir::Geo geo;
  if (is_geometry_name(root.local_name())) {
    ir::Feature f;
    CVT_TRY_ASSIGN(f.geometry, gml_geometry(r, root, srs, 0));
    geo.features.push_back(std::move(f));
  } else {
    bool collection = false;
    std::size_t count = 0;
    for (util::XmlNode e : root.elements()) {
      const std::string_view n = e.local_name();
      if (n == "featureMember" || n == "member" || n == "featureMembers" || n == "members") {
        collection = true;
        for (util::XmlNode feature : e.elements()) {
          if ((++count & 1023) == 0) CVT_TRY(ctx.checkpoint(0.5));
          if (feature.local_name() == "FeatureCollection" || feature.local_name() == "additionalObjects") continue;
          CVT_TRY_ASSIGN(ir::Feature f, gml_feature(r, feature, srs));
          geo.features.push_back(std::move(f));
        }
      }
    }
    if (!collection) {
      CVT_TRY_ASSIGN(ir::Feature f, gml_feature(r, root, srs));
      geo.features.push_back(std::move(f));
    }
  }
  if (r.xlinks) ctx.warn("GML xlink references were not followed; the geometries they point to are missing.");
  if (!r.unknown_crs.empty()) geo.crs = r.unknown_crs;
  return geo;
}

// --------------------------------------------------------------------------- writing GML 3.2

struct GmlWriter {
  bool lat_first = true;
  std::string srs_name;
  std::size_t next_id = 1;
};

std::string pos_text(const std::vector<ir::Coord>& pts, const GmlWriter& w, bool z) {
  std::string out;
  for (const ir::Coord& c : pts) {
    if (!out.empty()) out += ' ';
    out += format_number(w.lat_first ? c.lat : c.lon);
    out += ' ';
    out += format_number(w.lat_first ? c.lon : c.lat);
    if (z) {
      out += ' ';
      out += format_number(c.ele.value_or(0.0));
    }
  }
  return out;
}

void write_gml_geometry(util::XmlNode parent, const ir::Geometry& g, GmlWriter& w, bool top, int depth) {
  bool z = false;
  for_each_coord(g, [&](const ir::Coord& c) { z = z || c.ele.has_value(); });
  const auto start = [&](std::string_view name) {
    util::XmlNode n = parent.append_child(name);
    n.set_attribute("gml:id", "g" + std::to_string(w.next_id++));
    if (top) n.set_attribute("srsName", w.srs_name);
    n.set_attribute("srsDimension", z ? "3" : "2");
    return n;
  };
  const auto ring = [&](util::XmlNode polygon, std::string_view role, const std::vector<ir::Coord>& r) {
    xml::add_text(polygon.append_child(role).append_child("gml:LinearRing"), "gml:posList", pos_text(r, w, z));
  };
  const auto polygon = [&](util::XmlNode p, const ir::Polygon& poly) {
    for (std::size_t i = 0; i < poly.rings.size(); ++i) ring(p, i == 0 ? "gml:exterior" : "gml:interior", poly.rings[i]);
  };
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ir::Point>) {
          xml::add_text(start("gml:Point"), "gml:pos", pos_text({s.coord}, w, z));
        } else if constexpr (std::is_same_v<T, ir::LineString>) {
          xml::add_text(start("gml:LineString"), "gml:posList", pos_text(s.points, w, z));
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          polygon(start("gml:Polygon"), s);
        } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
          util::XmlNode m = start("gml:MultiPoint");
          for (const auto& c : s.points) {
            util::XmlNode p = m.append_child("gml:pointMember").append_child("gml:Point");
            p.set_attribute("gml:id", "g" + std::to_string(w.next_id++));
            xml::add_text(p, "gml:pos", pos_text({c}, w, z));
          }
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          util::XmlNode m = start("gml:MultiCurve");
          for (const auto& l : s.lines) {
            util::XmlNode ls = m.append_child("gml:curveMember").append_child("gml:LineString");
            ls.set_attribute("gml:id", "g" + std::to_string(w.next_id++));
            xml::add_text(ls, "gml:posList", pos_text(l.points, w, z));
          }
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          util::XmlNode m = start("gml:MultiSurface");
          for (const auto& p : s.polygons) {
            util::XmlNode poly = m.append_child("gml:surfaceMember").append_child("gml:Polygon");
            poly.set_attribute("gml:id", "g" + std::to_string(w.next_id++));
            polygon(poly, p);
          }
        } else {
          util::XmlNode m = start("gml:MultiGeometry");
          if (depth >= ir::max_geometry_depth) return;
          for (const auto& inner : s.geometries) {
            write_gml_geometry(m.append_child("gml:geometryMember"), inner, w, false, depth + 1);
          }
        }
      },
      g.shape);
}

std::string element_name(std::string_view key) {
  std::string out;
  for (char c : key) {
    out += (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '-' || c == '.') ? c : '_';
  }
  if (out.empty() || util::is_ascii_digit(out.front()) || out.front() == '-' || out.front() == '.') out.insert(out.begin(), '_');
  return out;
}

Result<Bytes> write_gml(const ir::Geo& geo, WriteContext& ctx) {
  GmlWriter w;
  const auto crs = parse_crs(geo.crs);
  if (crs && crs->kind != CrsKind::Geographic) {
    w.lat_first = false;
    w.srs_name = "urn:ogc:def:crs:EPSG::" + std::to_string(crs->epsg);
  } else {
    w.srs_name = "urn:ogc:def:crs:EPSG::4326";
  }
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  util::XmlNode root = doc.append_child("f:FeatureCollection");
  root.set_attribute("xmlns:gml", gml32_ns);
  root.set_attribute("xmlns:f", feature_ns);
  root.set_attribute("gml:id", "collection");
  std::size_t index = 0;
  for (const ir::Feature& f : geo.features) {
    if ((++index & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(geo.features.size())));
    util::XmlNode feature = root.append_child("gml:featureMember").append_child("f:feature");
    feature.set_attribute("gml:id", "f" + std::to_string(index));
    if (!geometry_empty(f.geometry)) write_gml_geometry(feature.append_child("f:geometry"), f.geometry, w, true, 0);
    if (!f.name.empty() && !f.properties.contains("name")) xml::add_text(feature, "f:name", f.name);
    for (const auto& m : f.properties.items()) {
      util::XmlNode p = feature.append_child("f:" + element_name(m.key()));
      if (m.value().is_null()) {
        p.set_attribute("xsi:nil", "true");
        root.set_attribute("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
      } else {
        p.append_text(json_to_text(m.value()));
      }
    }
  }
  return xml::finish(doc);
}

int sniff_gml(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 8192);
  if (!head.starts_with('<')) return 0;
  if (head.find("http://www.opengis.net/gml") == std::string_view::npos) return 0;
  if (head.find("<rss") != std::string_view::npos || head.find("<feed") != std::string_view::npos ||
      head.find("<kml") != std::string_view::npos) {
    return 0;
  }
  return 88;
}

// --------------------------------------------------------------------------- GeoRSS

Result<std::vector<ir::Coord>> lat_lon_list(std::string_view text) {
  Srs srs;
  srs.lat_first = true;
  srs.crs = Crs{};
  std::string t(text);
  std::replace(t.begin(), t.end(), ',', ' ');
  return numbers_to_coords(t, 2, srs);
}

Result<std::optional<ir::Geometry>> georss_geometry(GmlReader& r, util::XmlNode item) {
  std::optional<ir::Geometry> g;
  std::optional<double> elev;
  std::optional<double> lat, lon;
  for (util::XmlNode e : item.elements()) {
    const std::string_view n = e.local_name();
    const std::string_view ns = e.namespace_uri();
    const bool georss = ns.find("georss") != std::string_view::npos;
    if (georss && n == "point") {
      CVT_TRY_ASSIGN(auto pts, lat_lon_list(e.text()));
      if (pts.size() != 1) return fail(Status::Decode, "a georss:point needs one latitude and longitude");
      g = ir::Geometry{ir::Point{pts.front()}};
    } else if (georss && n == "line") {
      CVT_TRY_ASSIGN(auto pts, lat_lon_list(e.text()));
      g = ir::Geometry{ir::LineString{std::move(pts)}};
    } else if (georss && n == "polygon") {
      CVT_TRY_ASSIGN(auto pts, lat_lon_list(e.text()));
      close_ring(pts);
      g = ir::Geometry{ir::Polygon{{std::move(pts)}}};
    } else if (georss && n == "box") {
      CVT_TRY_ASSIGN(auto pts, lat_lon_list(e.text()));
      if (pts.size() != 2) return fail(Status::Decode, "a georss:box needs two corners");
      ir::Coord p1 = pts[0], p2 = pts[0], p3 = pts[1], p4 = pts[0];
      p2.lon = pts[1].lon;
      p4.lat = pts[1].lat;
      g = ir::Geometry{ir::Polygon{{{p1, p2, p3, p4, p1}}}};
    } else if (georss && n == "where") {
      for (util::XmlNode inner : e.elements()) {
        if (!is_geometry_name(inner.local_name())) continue;
        CVT_TRY_ASSIGN(ir::Geometry geom, gml_geometry(r, inner, srs_from("", r.forced, true), 0));
        g = std::move(geom);
        break;
      }
    } else if (georss && n == "elev") {
      elev = parse_number(e.text());
    } else if (n == "lat" && ns.find("wgs84_pos") != std::string_view::npos) {
      lat = parse_number(e.text());
    } else if ((n == "long" || n == "lon") && ns.find("wgs84_pos") != std::string_view::npos) {
      lon = parse_number(e.text());
    } else if (n == "Point" && ns.find("wgs84_pos") != std::string_view::npos) {
      lat = xml::child_number(e, "lat");
      lon = xml::child_number(e, "long");
    }
  }
  if (!g && lat && lon) {
    ir::Coord c;
    c.lat = *lat;
    c.lon = *lon;
    g = ir::Geometry{ir::Point{c}};
  }
  if (g && elev) {
    for_each_coord(*g, [&](ir::Coord& c) { c.ele = *elev; });
  }
  return g;
}

Result<ir::Geo> read_georss(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::XmlDocument doc, xml::parse(ctx.input, "GeoRSS"));
  GmlReader r;
  r.forced = axis_option(ctx.options);
  r.default_lat_first = true;
  const util::XmlNode root = doc.root();
  ir::Geo geo;
  std::vector<util::XmlNode> items;
  const std::string_view rn = root.local_name();
  if (rn == "rss") {
    const util::XmlNode channel = root.child_local("channel");
    geo.name = xml::child_text(channel, "title");
    geo.description = xml::child_text(channel, "description");
    for (util::XmlNode i : channel.children_local("item")) items.push_back(i);
  } else if (rn == "feed") {
    geo.name = xml::child_text(root, "title");
    for (util::XmlNode i : root.children_local("entry")) items.push_back(i);
  } else if (rn == "RDF") {
    geo.name = xml::child_text(root.child_local("channel"), "title");
    for (util::XmlNode i : root.children_local("item")) items.push_back(i);
  } else {
    return fail(Status::Decode, "not an RSS or Atom feed (root element <{}>)", rn);
  }
  for (util::XmlNode item : items) {
    CVT_TRY_ASSIGN(auto g, georss_geometry(r, item));
    if (!g) continue;
    ir::Feature f;
    f.geometry = std::move(*g);
    f.name = xml::child_text(item, "title");
    if (!f.name.empty()) f.properties.set("name", f.name);
    std::string description = xml::child_text(item, "description");
    if (description.empty()) description = xml::child_text(item, "summary");
    if (description.empty()) description = xml::child_text(item, "content");
    f.description = description;
    if (!description.empty()) f.properties.set("description", description);
    if (const util::XmlNode link = item.child_local("link")) {
      std::string href(link.attribute("href").value());
      if (href.empty()) href = std::string(util::trim(link.text()));
      if (!href.empty()) f.properties.set("link", href);
    }
    for (std::string_view key : {"pubDate", "updated", "published", "date"}) {
      const std::string t = xml::child_text(item, key);
      if (!t.empty()) {
        f.properties.set("time", t);
        break;
      }
    }
    geo.features.push_back(std::move(f));
  }
  if (r.xlinks) ctx.warn("GML xlink references were not followed.");
  return geo;
}

int sniff_georss(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 8192);
  if (!head.starts_with('<')) return 0;
  const bool feed = head.find("<rss") != std::string_view::npos || head.find("<feed") != std::string_view::npos ||
                    head.find("RDF") != std::string_view::npos;
  const bool geo = head.find("http://www.georss.org/georss") != std::string_view::npos ||
                   head.find("http://www.w3.org/2003/01/geo/wgs84_pos#") != std::string_view::npos;
  return feed && geo ? 96 : 0;  // above a plain RSS or Atom feed
}

}  // namespace

void register_gml(Registry& registry) {
  const OptionSpec axis = OptionSpec::choice("axis_order", "Axis order", "auto", {"auto", "lonlat", "latlon"},
                                             "Order of geographic coordinates: as the coordinate system name says "
                                             "(auto), or forced longitude-first / latitude-first for files that "
                                             "get it wrong.")
                              .advanced();
  FormatInfo info;
  info.id = "gml";
  info.name = "Geography Markup Language";
  info.description = "OGC GML simple-feature collections (GML 2, 3.1, 3.2), as served by WFS.";
  info.category = Category::Geo;
  info.extensions = {"gml"};
  info.mime = "application/gml+xml";
  info.popularity = 40;
  info.sniff = sniff_gml;
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "gml";
  spec.read = read_gml;
  spec.write = write_gml;
  spec.traits = GeoWriterTraits{true, false, false, false};
  spec.read_options = {axis};
  spec.lossy_write = true;  // styles, roles and per-point data are not written
  add_geo_codec(registry, std::move(spec));

  FormatInfo rss;
  rss.id = "georss";
  rss.name = "GeoRSS feed";
  rss.description = "RSS or Atom feed whose items carry GeoRSS or W3C Basic Geo positions (read only).";
  rss.category = Category::Geo;
  rss.extensions = {"georss", "rss", "atom", "xml"};
  rss.mime = "application/rss+xml";
  rss.popularity = 25;
  rss.sniff = sniff_georss;
  registry.add_format(std::move(rss));
  GeoCodecSpec rss_spec;
  rss_spec.format_id = "georss";
  rss_spec.read = read_georss;
  rss_spec.read_options = {axis};
  add_geo_codec(registry, std::move(rss_spec));
}

}  // namespace cvt::geo
