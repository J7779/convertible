// Convertible core - geo family: GPX 1.0 and 1.1 (GPS Exchange Format).
//
// Reads waypoints, routes and tracks with their segments, elevation, time,
// the descriptive fields (name, desc, cmt, sym, type, ...) and the common
// extensions: Garmin TrackPointExtension v1/v2 (hr, cad, atemp, wtemp,
// speed), Garmin/Strava power (PowerInWatts, power) and the GPX 1.0 speed
// element. Other numeric extension elements become per-point extra values.
// Document metadata (creator, author, time, links, keywords) goes into
// Geo::metadata so that `strip_metadata` removes it. Writes GPX 1.1.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "families/geo/geo.hpp"
#include "families/geo/xml_geo.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::geo {

namespace {

constexpr std::string_view gpx11_ns = "http://www.topografix.com/GPX/1/1";
constexpr std::string_view tpx_ns = "http://www.garmin.com/xmlschemas/TrackPointExtension/v2";
constexpr std::string_view pwr_ns = "http://www.garmin.com/xmlschemas/PowerExtension/v1";

// --------------------------------------------------------------------------- reading

// Sensor values go into the coordinate; other leaf elements into `props`
// (waypoints, typed) or the coordinate's extra values (numeric only).
void read_extension_values(util::XmlNode extensions, ir::Coord& c, Json* props, int depth = 0) {
  if (depth > 8) return;
  for (util::XmlNode e : extensions.elements()) {
    const std::string_view name = e.local_name();
    bool has_element_children = false;
    for (util::XmlNode inner : e.elements()) {
      (void)inner;
      has_element_children = true;
      break;
    }
    if (has_element_children) {
      read_extension_values(e, c, props, depth + 1);
      continue;
    }
    const auto value = parse_number(e.text());
    const bool sensor = util::iequals(name, "hr") || util::iequals(name, "heartrate") || util::iequals(name, "cad") ||
                        util::iequals(name, "cadence") || util::iequals(name, "atemp") || util::iequals(name, "wtemp") ||
                        util::iequals(name, "temp") || util::iequals(name, "temperature") ||
                        util::iequals(name, "speed") || util::iequals(name, "PowerInWatts") ||
                        util::iequals(name, "power") || util::iequals(name, "watts") ||
                        util::iequals(name, "HeartRateBpm") || util::iequals(name, "RunCadence");
    if (props != nullptr && !sensor) {
      if (props->size() < 256) props->set(name, text_to_json(util::trim(e.text())));
      continue;
    }
    if (!value) continue;
    if (util::iequals(name, "hr") || util::iequals(name, "heartrate") || util::iequals(name, "HeartRateBpm")) {
      c.heart_rate = *value;
    } else if (util::iequals(name, "cad") || util::iequals(name, "cadence") || util::iequals(name, "RunCadence")) {
      c.cadence = *value;
    } else if (util::iequals(name, "atemp") || util::iequals(name, "temp") || util::iequals(name, "temperature")) {
      c.temperature = *value;
    } else if (util::iequals(name, "wtemp")) {
      if (!c.temperature) c.temperature = *value;
    } else if (util::iequals(name, "speed")) {
      c.speed = *value;
    } else if (util::iequals(name, "PowerInWatts") || util::iequals(name, "power") || util::iequals(name, "watts")) {
      c.power = *value;
    } else if (c.extra.size() < 64) {
      c.extra.emplace_back(std::string(name), *value);
    }
  }
}

Result<ir::Coord> read_point(util::XmlNode node, Json* props = nullptr) {
  const auto lat = xml::attribute_number(node, "lat");
  const auto lon = xml::attribute_number(node, "lon");
  if (!lat || !lon) return fail(Status::Decode, "a GPX <{}> has no valid lat/lon attributes", node.local_name());
  ir::Coord c;
  c.lat = *lat;
  c.lon = *lon;
  if (auto ele = xml::child_number(node, "ele")) c.ele = *ele;
  const std::string time = xml::child_text(node, "time");
  if (!time.empty()) c.time_ms = parse_time(time);
  if (auto speed = xml::child_number(node, "speed")) c.speed = *speed;  // GPX 1.0
  if (const util::XmlNode ext = node.child_local("extensions")) read_extension_values(ext, c, props);
  return c;
}

// Descriptive child elements kept as properties.
void read_description(util::XmlNode node, ir::Feature& f) {
  for (std::string_view key : {"name", "cmt", "desc", "src", "sym", "type", "number", "fix", "sat", "hdop", "vdop",
                               "pdop", "magvar", "geoidheight"}) {
    const util::XmlNode child = node.child_local(key);
    if (!child) continue;
    const std::string text = std::string(util::trim(child.text()));
    if (key == "number" || key == "sat" || key == "hdop" || key == "vdop" || key == "pdop" || key == "magvar" ||
        key == "geoidheight") {
      f.properties.set(key, text_to_json(text));
    } else {
      f.properties.set(key, text);
    }
  }
  if (const util::XmlNode link = node.child_local("link")) {
    f.properties.set("link", std::string(link.attribute("href").value()));
  } else if (const util::XmlNode url = node.child_local("url")) {
    f.properties.set("link", std::string(util::trim(url.text())));
  }
  f.name = xml::child_text(node, "name");
  f.description = xml::child_text(node, "desc");
}

void read_metadata(util::XmlNode root, ir::Geo& geo) {
  util::XmlNode meta = root.child_local("metadata");
  const util::XmlNode source = meta ? meta : root;  // GPX 1.0 keeps these at the top level
  geo.name = xml::child_text(source, "name");
  geo.description = xml::child_text(source, "desc");
  if (const util::XmlAttribute creator = root.attribute("creator"); creator && !creator.value().empty()) {
    geo.metadata.set("creator", std::string(creator.value()));
  }
  if (const util::XmlNode author = source.child_local("author")) {
    const std::string name = author.child_local("name") ? xml::child_text(author, "name") : std::string(util::trim(author.text()));
    if (!name.empty()) geo.metadata.set("author", name);
    if (const util::XmlNode email = author.child_local("email")) {
      geo.metadata.set("email", std::string(email.attribute("id").value()) + "@" + std::string(email.attribute("domain").value()));
    }
  }
  if (const std::string email = xml::child_text(source, "email"); !email.empty()) geo.metadata.set("email", email);
  for (std::string_view key : {"time", "keywords"}) {
    const std::string text = xml::child_text(source, key);
    if (!text.empty()) geo.metadata.set(key, text);
  }
  if (const util::XmlNode link = source.child_local("link")) geo.metadata.set("link", std::string(link.attribute("href").value()));
  if (const util::XmlNode copyright = source.child_local("copyright")) {
    geo.metadata.set("copyright", std::string(copyright.attribute("author").value()));
  }
}

Result<ir::Geo> read_gpx(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::XmlDocument doc, xml::parse(ctx.input, "GPX"));
  const util::XmlNode root = doc.root();
  if (root.local_name() != "gpx") return fail(Status::Decode, "not a GPX file (root element <{}>)", root.local_name());
  ir::Geo geo;
  read_metadata(root, geo);
  std::size_t done = 0;
  for (util::XmlNode node : root.elements()) {
    if ((++done & 255) == 0) CVT_TRY(ctx.checkpoint(0.5));
    const std::string_view name = node.local_name();
    if (name == "wpt") {
      ir::Feature f;
      f.role = ir::FeatureRole::Waypoint;
      CVT_TRY_ASSIGN(ir::Coord c, read_point(node, &f.properties));
      f.geometry.shape = ir::Point{std::move(c)};
      read_description(node, f);
      geo.features.push_back(std::move(f));
    } else if (name == "rte") {
      ir::Feature f;
      f.role = ir::FeatureRole::Route;
      read_description(node, f);
      ir::LineString line;
      for (util::XmlNode p : node.children_local("rtept")) {
        CVT_TRY_ASSIGN(ir::Coord c, read_point(p));
        line.points.push_back(std::move(c));
      }
      f.geometry.shape = std::move(line);
      geo.features.push_back(std::move(f));
    } else if (name == "trk") {
      ir::Feature f;
      f.role = ir::FeatureRole::Track;
      read_description(node, f);
      ir::MultiLineString track;
      for (util::XmlNode seg : node.children_local("trkseg")) {
        ir::LineString line;
        for (util::XmlNode p : seg.children_local("trkpt")) {
          CVT_TRY_ASSIGN(ir::Coord c, read_point(p));
          line.points.push_back(std::move(c));
        }
        track.lines.push_back(std::move(line));
      }
      f.geometry.shape = std::move(track);
      geo.features.push_back(std::move(f));
    }
  }
  return geo;
}

// --------------------------------------------------------------------------- writing

struct Writer {
  util::XmlNode root;
  int pass = 0;  // 0: waypoints, 1: routes, 2: tracks (the order the schema requires)
  bool dropped_properties = false;
  bool polygons = false;
};

void write_point(util::XmlNode node, const ir::Coord& c) {
  node.set_attribute("lat", format_number(c.lat));
  node.set_attribute("lon", format_number(c.lon));
  if (c.ele) xml::add_text(node, "ele", format_number(*c.ele));
  if (c.time_ms) xml::add_text(node, "time", format_time(*c.time_ms));
}

void write_sensors(util::XmlNode node, const ir::Coord& c) {
  if (!(c.heart_rate || c.cadence || c.temperature || c.speed || c.power)) return;
  util::XmlNode ext = node.append_child("extensions");
  if (c.heart_rate || c.cadence || c.temperature || c.speed) {
    util::XmlNode tpx = ext.append_child("gpxtpx:TrackPointExtension");
    if (c.temperature) xml::add_text(tpx, "gpxtpx:atemp", format_number(*c.temperature));
    if (c.heart_rate) xml::add_text(tpx, "gpxtpx:hr", format_number(std::round(*c.heart_rate), 0));
    if (c.cadence) xml::add_text(tpx, "gpxtpx:cad", format_number(std::round(*c.cadence), 0));
    if (c.speed) xml::add_text(tpx, "gpxtpx:speed", format_number(*c.speed));
  }
  if (c.power) xml::add_text(ext, "pwr:PowerInWatts", format_number(std::round(*c.power), 0));
}

// name, cmt, desc, src, link, sym, type in the order of the GPX schema.
void write_description(util::XmlNode node, const ir::Feature& f, Writer& w, bool point) {
  const auto prop = [&](std::string_view key) -> std::string {
    const Json* v = f.properties.find(key);
    return v != nullptr ? json_to_text(*v) : std::string();
  };
  xml::add_text(node, "name", feature_name(f));
  xml::add_text(node, "cmt", prop("cmt"));
  std::string desc = f.description;
  if (desc.empty()) desc = prop("desc");
  if (desc.empty()) desc = prop("description");
  xml::add_text(node, "desc", desc);
  xml::add_text(node, "src", prop("src"));
  if (const std::string link = prop("link"); !link.empty()) node.append_child("link").set_attribute("href", link);
  if (point) xml::add_text(node, "sym", prop("sym"));
  xml::add_text(node, "type", prop("type"));
  for (const auto& member : f.properties.items()) {
    const std::string_view k = member.key();
    if (k != "name" && k != "cmt" && k != "desc" && k != "description" && k != "src" && k != "link" && k != "sym" &&
        k != "type" && k != "time" && k != "ele" && k != "number") {
      w.dropped_properties = true;
    }
  }
}

void add_waypoint(Writer& w, const ir::Feature& f, const ir::Coord& c) {
  if (w.pass != 0) return;
  util::XmlNode node = w.root.append_child("wpt");
  write_point(node, c);
  write_description(node, f, w, true);
}

void add_track(Writer& w, const ir::Feature& f, const std::vector<const std::vector<ir::Coord>*>& segments) {
  if (w.pass != 2) return;
  util::XmlNode node = w.root.append_child("trk");
  write_description(node, f, w, false);
  for (const auto* segment : segments) {
    util::XmlNode seg = node.append_child("trkseg");
    for (const ir::Coord& c : *segment) {
      util::XmlNode p = seg.append_child("trkpt");
      write_point(p, c);
      write_sensors(p, c);
    }
  }
}

void add_route(Writer& w, const ir::Feature& f, const std::vector<ir::Coord>& points) {
  if (w.pass != 1) return;
  util::XmlNode node = w.root.append_child("rte");
  write_description(node, f, w, false);
  for (const ir::Coord& c : points) {
    util::XmlNode p = node.append_child("rtept");
    write_point(p, c);
    write_sensors(p, c);
  }
}

void add_geometry(Writer& w, const ir::Feature& f, const ir::Geometry& g, int depth) {
  if (depth > ir::max_geometry_depth) return;
  std::visit(
      [&](const auto& shape) {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, ir::Point>) {
          add_waypoint(w, f, shape.coord);
        } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
          for (const ir::Coord& c : shape.points) add_waypoint(w, f, c);
        } else if constexpr (std::is_same_v<T, ir::LineString>) {
          if (f.role == ir::FeatureRole::Route) {
            add_route(w, f, shape.points);
          } else {
            add_track(w, f, {&shape.points});
          }
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          std::vector<const std::vector<ir::Coord>*> segments;
          for (const auto& l : shape.lines) segments.push_back(&l.points);
          add_track(w, f, segments);
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          w.polygons = true;
          std::vector<const std::vector<ir::Coord>*> segments;
          for (const auto& r : shape.rings) segments.push_back(&r);
          add_track(w, f, segments);
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          w.polygons = true;
          std::vector<const std::vector<ir::Coord>*> segments;
          for (const auto& p : shape.polygons) {
            for (const auto& r : p.rings) segments.push_back(&r);
          }
          add_track(w, f, segments);
        } else {
          for (const auto& inner : shape.geometries) add_geometry(w, f, inner, depth + 1);
        }
      },
      g.shape);
}

Result<Bytes> write_gpx(const ir::Geo& geo, WriteContext& ctx) {
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  Writer w;
  w.root = doc.append_child("gpx");
  w.root.set_attribute("version", "1.1");
  // The creator attribute is required; it stays empty unless the source's is kept.
  w.root.set_attribute("creator", geo.metadata["creator"].as_string());
  w.root.set_attribute("xmlns", gpx11_ns);
  w.root.set_attribute("xmlns:gpxtpx", tpx_ns);
  w.root.set_attribute("xmlns:pwr", pwr_ns);
  util::XmlNode meta = w.root.append_child("metadata");
  xml::add_text(meta, "name", geo.name);
  xml::add_text(meta, "desc", geo.description);
  if (const std::string author = geo.metadata["author"].as_string(); !author.empty()) {
    xml::add_text(meta.append_child("author"), "name", author);
  }
  if (const std::string link = geo.metadata["link"].as_string(); !link.empty()) {
    meta.append_child("link").set_attribute("href", link);
  }
  if (const auto t = parse_time(geo.metadata["time"].as_string())) xml::add_text(meta, "time", format_time(*t));
  xml::add_text(meta, "keywords", geo.metadata["keywords"].as_string());
  if (!meta.first_child()) w.root.remove_child(meta);
  for (w.pass = 0; w.pass < 3; ++w.pass) {
    CVT_TRY(ctx.checkpoint(w.pass / 3.0));
    for (const ir::Feature& f : geo.features) add_geometry(w, f, f.geometry, 0);
  }
  if (w.dropped_properties) {
    ctx.warn("GPX has no place for arbitrary attributes; only name, description, comment, symbol, type and link were kept.");
  }
  if (w.polygons) ctx.warn("GPX has no polygons; polygon rings were written as track segments.");
  return xml::finish(doc);
}

int sniff_gpx(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 4096);
  if (!head.starts_with('<')) return 0;
  return head.find("<gpx") != std::string_view::npos ? 92 : 0;
}

}  // namespace

void register_gpx(Registry& registry) {
  FormatInfo info;
  info.id = "gpx";
  info.name = "GPS Exchange Format";
  info.description = "Waypoints, routes and tracks from GPS devices and apps (GPX 1.0 / 1.1).";
  info.category = Category::Geo;
  info.extensions = {"gpx"};
  info.mime = "application/gpx+xml";
  info.popularity = 72;
  info.sniff = sniff_gpx;
  registry.add_format(std::move(info));

  GeoCodecSpec spec;
  spec.format_id = "gpx";
  spec.read = read_gpx;
  spec.write = write_gpx;
  spec.traits = GeoWriterTraits{false, true, true, true};
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
