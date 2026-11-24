// Convertible core - geo family: KML 2.2 / 2.3 and KMZ.
//
// Reading: Placemarks with Point, LineString, LinearRing, Polygon,
// MultiGeometry, Model (its Location), gx:Track and gx:MultiTrack (when,
// gx:coord and the gx:SimpleArrayData sensor arrays); Folder / Document
// nesting becomes Feature::folder; ExtendedData Data and SchemaData /
// SimpleData become properties (typed by the Schema's SimpleField types,
// otherwise by their text); Style / StyleMap / styleUrl become Feature::style.
// NetworkLinks and overlays are ignored with a note - nothing is ever
// fetched. KMZ is a ZIP whose main document is doc.kml (or the first .kml);
// every other entry is kept as a resource.
// Writing: a Document with Folders, a Schema for typed properties, inline
// styles, and gx:Track for timed tracks (option).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/geo/geo.hpp"
#include "families/geo/xml_geo.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"
#include "util/zip.hpp"

namespace cvt::geo {

namespace {

constexpr std::string_view kml_ns = "http://www.opengis.net/kml/2.2";
constexpr std::string_view gx_ns = "http://www.google.com/kml/ext/2.2";

// --------------------------------------------------------------------------- colours

// KML colours are aabbggrr hex.
void parse_kml_color(std::string_view text, std::string& rgb, double& opacity) {
  text = util::trim(text);
  if (text.starts_with('#')) text.remove_prefix(1);
  if (text.size() != 8) return;
  std::uint32_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (ec != std::errc{} || end != text.data() + text.size()) return;
  const unsigned a = (value >> 24) & 0xFF, b = (value >> 16) & 0xFF, g = (value >> 8) & 0xFF, r = value & 0xFF;
  rgb = std::format("#{:02x}{:02x}{:02x}", r, g, b);
  opacity = a / 255.0;
}

std::string kml_color(std::string_view rgb, double opacity) {
  if (rgb.starts_with('#')) rgb.remove_prefix(1);
  if (rgb.size() != 6) return {};
  const auto alpha = static_cast<unsigned>(std::lround(std::clamp(opacity, 0.0, 1.0) * 255));
  return std::format("{:02x}{}{}{}", alpha, rgb.substr(4, 2), rgb.substr(2, 2), rgb.substr(0, 2));
}

void read_style(util::XmlNode style, ir::Style& out) {
  if (const util::XmlNode line = style.child_local("LineStyle")) {
    parse_kml_color(xml::child_text(line, "color"), out.stroke, out.stroke_opacity);
    if (auto w = xml::child_number(line, "width")) out.stroke_width = *w;
  }
  if (const util::XmlNode poly = style.child_local("PolyStyle")) {
    parse_kml_color(xml::child_text(poly, "color"), out.fill, out.fill_opacity);
    if (xml::child_text(poly, "fill") == "0") out.fill_opacity = 0.0;
  }
  if (const util::XmlNode icon = style.child_local("IconStyle")) {
    double ignored = 1.0;
    parse_kml_color(xml::child_text(icon, "color"), out.marker_color, ignored);
    if (auto s = xml::child_number(icon, "scale")) out.icon_scale = *s;
    if (const util::XmlNode i = icon.child_local("Icon")) out.icon = xml::child_text(i, "href");
  }
}

// --------------------------------------------------------------------------- reading

struct KmlReader {
  const Context* ctx = nullptr;
  ir::Geo geo;
  std::map<std::string, ir::Style, std::less<>> styles;
  std::map<std::string, std::string, std::less<>> style_maps;  // id -> normal style url
  // Schema id -> (field name -> type)
  std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> schemas;
  std::vector<std::string> links;  // NetworkLink targets (resolved only inside a KMZ)
  bool warned_overlays = false;
  std::size_t count = 0;
};

std::vector<ir::Coord> parse_coordinates(std::string_view text) {
  std::vector<ir::Coord> out;
  // Tuples are separated by white space; values inside a tuple by commas
  // (some writers put spaces after the commas, which is tolerated).
  std::string normalised;
  normalised.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == ',') {
      while (!normalised.empty() && util::is_ascii_space(normalised.back())) normalised.pop_back();
      normalised += ',';
      while (i + 1 < text.size() && util::is_ascii_space(text[i + 1])) ++i;
    } else {
      normalised += util::is_ascii_space(c) ? ' ' : c;
    }
  }
  for (std::string_view tuple : util::split(normalised, ' ', false)) {
    const auto parts = util::split(tuple, ',');
    if (parts.size() < 2) continue;
    const auto lon = parse_number(parts[0]);
    const auto lat = parse_number(parts[1]);
    if (!lon || !lat) continue;
    ir::Coord c;
    c.lon = *lon;
    c.lat = *lat;
    if (parts.size() >= 3) {
      if (auto alt = parse_number(parts[2])) c.ele = *alt;
    }
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<ir::Coord> ring_of(util::XmlNode boundary) {
  const util::XmlNode ring = boundary.child_local("LinearRing");
  std::vector<ir::Coord> pts = parse_coordinates(xml::child_text(ring ? ring : boundary, "coordinates"));
  if (!pts.empty() && !(pts.front().lon == pts.back().lon && pts.front().lat == pts.back().lat)) pts.push_back(pts.front());
  return pts;
}

// gx:Track: <when> and <gx:coord> lists plus optional sensor arrays.
ir::LineString read_track(util::XmlNode track) {
  ir::LineString line;
  std::vector<std::optional<std::int64_t>> times;
  for (util::XmlNode e : track.elements()) {
    const std::string_view n = e.local_name();
    if (n == "when") {
      times.push_back(parse_time(e.text()));
    } else if (n == "coord") {
      const std::string text = e.text();
      const auto parts = util::split(util::trim(text), ' ', false);
      if (parts.size() < 2) continue;
      const auto lon = parse_number(parts[0]);
      const auto lat = parse_number(parts[1]);
      if (!lon || !lat) continue;
      ir::Coord c;
      c.lon = *lon;
      c.lat = *lat;
      if (parts.size() >= 3) {
        if (auto alt = parse_number(parts[2])) c.ele = *alt;
      }
      line.points.push_back(std::move(c));
    }
  }
  for (std::size_t i = 0; i < line.points.size() && i < times.size(); ++i) line.points[i].time_ms = times[i];
  // Sensor arrays: ExtendedData/SchemaData/gx:SimpleArrayData name="heartrate"...
  if (const util::XmlNode extended = track.child_local("ExtendedData")) {
    for (util::XmlNode data : extended.descendants()) {
      if (!data.is_element() || data.local_name() != "SimpleArrayData") continue;
      const std::string name = util::to_lower(data.attribute("name").value());
      std::size_t i = 0;
      for (util::XmlNode v : data.children_local("value")) {
        if (i >= line.points.size()) break;
        ir::Coord& c = line.points[i++];
        const auto value = parse_number(v.text());
        if (!value) continue;
        if (name == "heartrate" || name == "heart_rate" || name == "hr") c.heart_rate = *value;
        else if (name == "cadence" || name == "cad") c.cadence = *value;
        else if (name == "power" || name == "watts") c.power = *value;
        else if (name == "speed") c.speed = *value;
        else if (name == "temperature" || name == "temp" || name == "atemp") c.temperature = *value;
        else if (c.extra.size() < 64) c.extra.emplace_back(name, *value);
      }
    }
  }
  return line;
}

Result<ir::Geometry> read_geometry(util::XmlNode node, int depth) {
  if (depth > ir::max_geometry_depth) return fail(Status::Limit, "KML MultiGeometry elements are nested too deeply");
  ir::Geometry g;
  const std::string_view n = node.local_name();
  if (n == "Point") {
    auto pts = parse_coordinates(xml::child_text(node, "coordinates"));
    if (pts.empty()) {
      g.shape = ir::MultiPoint{};
    } else {
      g.shape = ir::Point{std::move(pts.front())};
    }
  } else if (n == "LineString") {
    g.shape = ir::LineString{parse_coordinates(xml::child_text(node, "coordinates"))};
  } else if (n == "LinearRing") {
    g.shape = ir::Polygon{{ring_of(node)}};
  } else if (n == "Polygon") {
    ir::Polygon polygon;
    if (const util::XmlNode outer = node.child_local("outerBoundaryIs")) polygon.rings.push_back(ring_of(outer));
    for (util::XmlNode inner : node.children_local("innerBoundaryIs")) {
      // KML 2.2 allows one LinearRing per innerBoundaryIs, KML 2.3 several.
      bool any = false;
      for (util::XmlNode ring : inner.children_local("LinearRing")) {
        std::vector<ir::Coord> pts = parse_coordinates(xml::child_text(ring, "coordinates"));
        if (!pts.empty() && !(pts.front().lon == pts.back().lon && pts.front().lat == pts.back().lat)) {
          pts.push_back(pts.front());
        }
        polygon.rings.push_back(std::move(pts));
        any = true;
      }
      if (!any) polygon.rings.push_back(ring_of(inner));
    }
    g.shape = std::move(polygon);
  } else if (n == "Track") {
    g.shape = ir::MultiLineString{{read_track(node)}};
  } else if (n == "MultiTrack") {
    ir::MultiLineString multi;
    for (util::XmlNode t : node.children_local("Track")) multi.lines.push_back(read_track(t));
    g.shape = std::move(multi);
  } else if (n == "Model") {
    ir::Coord c;
    if (const util::XmlNode loc = node.child_local("Location")) {
      c.lon = xml::child_number(loc, "longitude").value_or(0.0);
      c.lat = xml::child_number(loc, "latitude").value_or(0.0);
      if (auto alt = xml::child_number(loc, "altitude")) c.ele = *alt;
    }
    g.shape = ir::Point{c};
  } else if (n == "MultiGeometry") {
    std::vector<ir::Geometry> members;
    for (util::XmlNode child : node.elements()) {
      const std::string_view cn = child.local_name();
      if (cn == "Point" || cn == "LineString" || cn == "LinearRing" || cn == "Polygon" || cn == "MultiGeometry" ||
          cn == "Track" || cn == "MultiTrack" || cn == "Model") {
        CVT_TRY_ASSIGN(ir::Geometry inner, read_geometry(child, depth + 1));
        members.push_back(std::move(inner));
      }
    }
    const auto all = [&](auto probe) {
      return !members.empty() &&
             std::all_of(members.begin(), members.end(), [&](const ir::Geometry& m) { return probe(m); });
    };
    if (all([](const ir::Geometry& m) { return std::holds_alternative<ir::Point>(m.shape); })) {
      ir::MultiPoint mp;
      for (auto& m : members) mp.points.push_back(std::get<ir::Point>(m.shape).coord);
      g.shape = std::move(mp);
    } else if (all([](const ir::Geometry& m) { return std::holds_alternative<ir::LineString>(m.shape); })) {
      ir::MultiLineString ml;
      for (auto& m : members) ml.lines.push_back(std::move(std::get<ir::LineString>(m.shape)));
      g.shape = std::move(ml);
    } else if (all([](const ir::Geometry& m) { return std::holds_alternative<ir::Polygon>(m.shape); })) {
      ir::MultiPolygon mp;
      for (auto& m : members) mp.polygons.push_back(std::move(std::get<ir::Polygon>(m.shape)));
      g.shape = std::move(mp);
    } else {
      g.shape = ir::GeometryCollection{std::move(members)};
    }
  } else {
    g.shape = ir::GeometryCollection{};
  }
  return g;
}

Json typed_value(std::string_view text, std::string_view type) {
  const std::string_view t = util::trim(text);
  if (type == "int" || type == "uint" || type == "short" || type == "ushort") {
    if (auto v = util::parse_int(t)) return Json(*v);
  } else if (type == "float" || type == "double") {
    if (auto v = parse_number(t)) return Json(*v);
  } else if (type == "bool") {
    if (t == "1" || t == "true") return Json(true);
    if (t == "0" || t == "false") return Json(false);
  } else if (type == "string") {
    return Json(std::string(text));
  }
  return text_to_json(text);
}

void read_extended_data(KmlReader& r, util::XmlNode extended, ir::Feature& f) {
  for (util::XmlNode e : extended.elements()) {
    const std::string_view n = e.local_name();
    if (n == "Data") {
      const std::string name(e.attribute("name").value());
      if (name.empty()) continue;
      f.properties.set(name, text_to_json(xml::child_text(e, "value")));
    } else if (n == "SchemaData") {
      std::string_view url = e.attribute("schemaUrl").value();
      if (url.starts_with('#')) url.remove_prefix(1);
      const auto schema = r.schemas.find(url);
      for (util::XmlNode d : e.children_local("SimpleData")) {
        const std::string name(d.attribute("name").value());
        if (name.empty()) continue;
        std::string type;
        if (schema != r.schemas.end()) {
          if (auto t = schema->second.find(name); t != schema->second.end()) type = t->second;
        }
        f.properties.set(name, typed_value(d.text(), type));
      }
    }
  }
}

Result<void> read_placemark(KmlReader& r, util::XmlNode node, const std::string& folder) {
  ir::Feature f;
  f.folder = folder;
  f.name = xml::child_text(node, "name");
  f.description = xml::child_text(node, "description");
  if (const util::XmlAttribute id = node.attribute("id")) f.id = Json(std::string(id.value()));
  if (!f.name.empty()) f.properties.set("name", f.name);
  if (!f.description.empty()) f.properties.set("description", f.description);
  if (const util::XmlNode extended = node.child_local("ExtendedData")) read_extended_data(r, extended, f);
  // Style: shared by url, then inline.
  std::string url = xml::child_text(node, "styleUrl");
  if (url.starts_with('#')) url.erase(0, 1);
  if (auto m = r.style_maps.find(url); m != r.style_maps.end()) url = m->second;
  if (auto s = r.styles.find(url); s != r.styles.end()) f.style = s->second;
  if (const util::XmlNode inline_style = node.child_local("Style")) read_style(inline_style, f.style);
  if (const util::XmlNode time = node.child_local("TimeStamp")) {
    const std::string when = xml::child_text(time, "when");
    if (!when.empty()) f.properties.set("timestamp", when);
  }
  bool found = false;
  for (util::XmlNode child : node.elements()) {
    const std::string_view n = child.local_name();
    if (n == "Point" || n == "LineString" || n == "LinearRing" || n == "Polygon" || n == "MultiGeometry" ||
        n == "Track" || n == "MultiTrack" || n == "Model") {
      CVT_TRY_ASSIGN(f.geometry, read_geometry(child, 0));
      if (n == "Track" || n == "MultiTrack") f.role = ir::FeatureRole::Track;
      found = true;
      break;
    }
  }
  if (!found) f.geometry.shape = ir::GeometryCollection{};
  if (auto* p = std::get_if<ir::Point>(&f.geometry.shape); p != nullptr && !p->coord.time_ms) {
    if (const Json* ts = f.properties.find("timestamp"); ts != nullptr) p->coord.time_ms = parse_time(ts->as_string());
  }
  r.geo.features.push_back(std::move(f));
  return {};
}

void collect_shared(KmlReader& r, util::XmlNode container) {
  for (util::XmlNode e : container.elements()) {
    const std::string_view n = e.local_name();
    if (n == "Style") {
      ir::Style style;
      read_style(e, style);
      r.styles[std::string(e.attribute("id").value())] = style;
    } else if (n == "StyleMap") {
      for (util::XmlNode pair : e.children_local("Pair")) {
        if (xml::child_text(pair, "key") == "normal") {
          std::string url = xml::child_text(pair, "styleUrl");
          if (url.starts_with('#')) url.erase(0, 1);
          r.style_maps[std::string(e.attribute("id").value())] = url;
        }
      }
    } else if (n == "Schema") {
      auto& fields = r.schemas[std::string(e.attribute("id").value())];
      for (util::XmlNode field : e.children_local("SimpleField")) {
        fields[std::string(field.attribute("name").value())] = util::to_lower(field.attribute("type").value());
      }
      // Older files refer to schemas by name.
      if (const util::XmlAttribute name = e.attribute("name")) r.schemas[std::string(name.value())] = fields;
    }
  }
}

Result<void> read_container(KmlReader& r, util::XmlNode container, const std::string& folder, int depth) {
  if (depth > 64) return fail(Status::Limit, "KML folders are nested too deeply");
  collect_shared(r, container);
  for (util::XmlNode e : container.elements()) {
    if ((++r.count & 255) == 0) CVT_TRY(r.ctx->checkpoint(0.5));
    const std::string_view n = e.local_name();
    if (n == "Placemark") {
      CVT_TRY(read_placemark(r, e, folder));
    } else if (n == "Folder" || n == "Document") {
      std::string name = xml::child_text(e, "name");
      std::string path = folder;
      if (n == "Folder") {
        if (name.empty()) name = "Folder";
        path = folder.empty() ? name : folder + "/" + name;
      }
      CVT_TRY(read_container(r, e, path, depth + 1));
    } else if (n == "NetworkLink") {
      util::XmlNode link = e.child_local("Link");
      if (!link) link = e.child_local("Url");
      if (r.links.size() < 1000) r.links.push_back(xml::child_text(link, "href"));
    } else if (n == "GroundOverlay" || n == "ScreenOverlay" || n == "PhotoOverlay") {
      if (!r.warned_overlays) r.ctx->warn("KML image overlays were ignored.");
      r.warned_overlays = true;
    }
  }
  return {};
}

Result<ir::Geo> parse_kml(ByteView input, const Context& ctx, std::vector<std::string>& links) {
  CVT_TRY_ASSIGN(util::XmlDocument doc, xml::parse(input, "KML"));
  util::XmlNode root = doc.root();
  if (root.local_name() != "kml") {
    // Some writers drop the <kml> wrapper and start with a Document.
    if (root.local_name() != "Document" && root.local_name() != "Folder") {
      return fail(Status::Decode, "not a KML file (root element <{}>)", root.local_name());
    }
  }
  KmlReader r;
  r.ctx = &ctx;
  if (root.local_name() == "kml") {
    // Placemarks may sit directly below <kml>, or in Documents / Folders.
    if (const util::XmlNode top = root.child_local("Document")) {
      r.geo.name = xml::child_text(top, "name");
      r.geo.description = xml::child_text(top, "description");
    }
    CVT_TRY(read_container(r, root, {}, 0));
  } else {
    r.geo.name = xml::child_text(root, "name");
    CVT_TRY(read_container(r, root, {}, 0));
  }
  if (const util::XmlNode author = root.child_local("Document").child_local("author")) {
    const std::string name = xml::child_text(author, "name");
    if (!name.empty()) r.geo.metadata.set("author", name);
  }
  links = std::move(r.links);
  return std::move(r.geo);
}

constexpr std::string_view links_ignored = "KML network links were ignored: linked content is never downloaded.";

Result<ir::Geo> read_kml(ReadContext& ctx) {
  std::vector<std::string> links;
  CVT_TRY_ASSIGN(ir::Geo geo, parse_kml(ctx.input, ctx, links));
  if (!links.empty()) ctx.warn(std::string(links_ignored));
  return geo;
}

Result<ir::Geo> read_kmz(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
  std::optional<std::size_t> main = zip.find("doc.kml");
  if (!main) {
    for (std::size_t i = 0; i < zip.size(); ++i) {
      const auto& e = zip.entries()[i];
      if (e.is_directory || !util::iends_with(e.name, ".kml")) continue;
      if (!main || (e.name.find('/') == std::string::npos && zip.entries()[*main].name.find('/') != std::string::npos)) {
        main = i;
      }
    }
  }
  if (!main) return fail(Status::Decode, "the KMZ archive contains no KML document");
  CVT_TRY_ASSIGN(const Bytes kml, zip.read(*main));
  std::vector<std::string> links;
  CVT_TRY_ASSIGN(ir::Geo geo, parse_kml(kml, ctx, links));
  // Links to other KML documents inside the archive (LIBKML writes one per
  // layer) are followed; anything else would need the network and is not.
  std::vector<std::size_t> used{*main};
  bool unresolved = false;
  for (std::size_t round = 0; round < links.size() && round < 256; ++round) {
    const auto path = util::sanitize_entry_path(links[round]);
    const auto index = path ? zip.find(*path) : std::nullopt;
    if (!index || std::find(used.begin(), used.end(), *index) != used.end()) {
      unresolved = unresolved || !index;
      continue;
    }
    used.push_back(*index);
    CVT_TRY_ASSIGN(const Bytes linked, zip.read(*index));
    std::vector<std::string> more;
    CVT_TRY_ASSIGN(ir::Geo part, parse_kml(linked, ctx, more));
    for (auto& f : part.features) geo.features.push_back(std::move(f));
    if (geo.name.empty()) geo.name = part.name;
    for (auto& l : more) {
      if (links.size() < 1000) links.push_back(std::move(l));
    }
  }
  if (unresolved) ctx.warn(std::string(links_ignored));
  for (std::size_t i = 0; i < zip.size(); ++i) {
    const auto& e = zip.entries()[i];
    if (std::find(used.begin(), used.end(), i) != used.end() || e.is_directory || e.encrypted) continue;
    const auto path = e.safe_path();
    if (!path) continue;
    auto data = zip.read(i);
    if (!data) {
      if (data.error().code == Status::Limit) return fail(data.error());
      ctx.warn("An entry of the KMZ archive could not be read and was skipped.");
      continue;
    }
    geo.resources.push_back(ir::GeoResource{*path, std::move(*data)});
  }
  return geo;
}

// --------------------------------------------------------------------------- writing

struct KmlWriter {
  util::XmlNode document;
  std::map<std::string, util::XmlNode, std::less<>> folders;
  std::map<std::string, std::string, std::less<>> field_types;  // schema fields (scalar, consistent types)
  bool gx_tracks = true;
  bool sensor_schema = false;
};

util::XmlNode folder_for(KmlWriter& w, const std::string& path) {
  if (path.empty()) return w.document;
  if (auto it = w.folders.find(path); it != w.folders.end()) return it->second;
  const std::size_t slash = path.rfind('/');
  util::XmlNode parent = slash == std::string::npos ? w.document : folder_for(w, path.substr(0, slash));
  util::XmlNode folder = parent.append_child("Folder");
  xml::add_text(folder, "name", slash == std::string::npos ? path : path.substr(slash + 1));
  w.folders[path] = folder;
  return folder;
}

std::string coordinates_text(const std::vector<ir::Coord>& points) {
  std::string out;
  for (const ir::Coord& c : points) {
    if (!out.empty()) out += ' ';
    out += format_number(c.lon);
    out += ',';
    out += format_number(c.lat);
    if (c.ele) {
      out += ',';
      out += format_number(*c.ele);
    }
  }
  return out;
}

void write_polygon(util::XmlNode parent, const ir::Polygon& polygon) {
  util::XmlNode node = parent.append_child("Polygon");
  for (std::size_t i = 0; i < polygon.rings.size(); ++i) {
    util::XmlNode boundary = node.append_child(i == 0 ? "outerBoundaryIs" : "innerBoundaryIs");
    xml::add_text(boundary.append_child("LinearRing"), "coordinates", coordinates_text(polygon.rings[i]));
  }
}

void write_track(KmlWriter& w, util::XmlNode parent, const std::vector<ir::Coord>& points) {
  util::XmlNode track = parent.append_child("gx:Track");
  for (const ir::Coord& c : points) xml::add_text(track, "when", c.time_ms ? format_time(*c.time_ms) : std::string());
  for (const ir::Coord& c : points) {
    std::string text = format_number(c.lon) + " " + format_number(c.lat);
    if (c.ele) text += " " + format_number(*c.ele);
    xml::add_text(track, "gx:coord", text);
  }
  struct Sensor {
    std::string_view name;
    std::optional<double> ir::Coord::*field;
  };
  static constexpr Sensor sensors[] = {{"heartrate", &ir::Coord::heart_rate},
                                       {"cadence", &ir::Coord::cadence},
                                       {"power", &ir::Coord::power},
                                       {"speed", &ir::Coord::speed},
                                       {"temperature", &ir::Coord::temperature}};
  util::XmlNode data;
  for (const Sensor& s : sensors) {
    const bool any = std::any_of(points.begin(), points.end(), [&](const ir::Coord& c) { return (c.*s.field).has_value(); });
    if (!any) continue;
    if (!data) {
      data = track.append_child("ExtendedData").append_child("SchemaData");
      data.set_attribute("schemaUrl", "#track_sensors");
      w.sensor_schema = true;
    }
    util::XmlNode array = data.append_child("gx:SimpleArrayData");
    array.set_attribute("name", s.name);
    for (const ir::Coord& c : points) {
      const auto& v = c.*s.field;
      xml::add_text(array, "gx:value", v ? format_number(*v) : std::string("0"));
    }
  }
}

bool all_timed(const std::vector<ir::Coord>& points) {
  return !points.empty() && std::all_of(points.begin(), points.end(), [](const ir::Coord& c) { return c.time_ms.has_value(); });
}

void write_geometry(KmlWriter& w, util::XmlNode parent, const ir::Geometry& g, int depth) {
  if (depth > ir::max_geometry_depth) return;
  std::visit(
      [&](const auto& shape) {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, ir::Point>) {
          xml::add_text(parent.append_child("Point"), "coordinates", coordinates_text({shape.coord}));
        } else if constexpr (std::is_same_v<T, ir::LineString>) {
          if (w.gx_tracks && all_timed(shape.points)) {
            write_track(w, parent, shape.points);
          } else {
            xml::add_text(parent.append_child("LineString"), "coordinates", coordinates_text(shape.points));
          }
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          write_polygon(parent, shape);
        } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
          util::XmlNode multi = parent.append_child("MultiGeometry");
          for (const ir::Coord& c : shape.points) xml::add_text(multi.append_child("Point"), "coordinates", coordinates_text({c}));
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          const bool timed = w.gx_tracks && !shape.lines.empty() &&
                             std::all_of(shape.lines.begin(), shape.lines.end(),
                                         [](const ir::LineString& l) { return all_timed(l.points); });
          if (timed) {
            util::XmlNode multi = parent.append_child("gx:MultiTrack");
            xml::add_text(multi, "gx:interpolate", "0");
            for (const auto& l : shape.lines) write_track(w, multi, l.points);
          } else {
            util::XmlNode multi = parent.append_child("MultiGeometry");
            for (const auto& l : shape.lines) {
              xml::add_text(multi.append_child("LineString"), "coordinates", coordinates_text(l.points));
            }
          }
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          util::XmlNode multi = parent.append_child("MultiGeometry");
          for (const auto& p : shape.polygons) write_polygon(multi, p);
        } else {
          if (shape.geometries.empty()) return;
          util::XmlNode multi = parent.append_child("MultiGeometry");
          for (const auto& inner : shape.geometries) write_geometry(w, multi, inner, depth + 1);
        }
      },
      g.shape);
}

std::string_view scalar_type(const Json& v) {
  switch (v.type()) {
    case Json::Type::Bool: return "bool";
    case Json::Type::Int64:
    case Json::Type::UInt64: return "int";
    case Json::Type::Double: return "double";
    case Json::Type::String: return "string";
    default: return {};
  }
}

// Fields whose values are scalars of one type (ints may widen to double)
// across all features go into a typed Schema.
void infer_schema(KmlWriter& w, const ir::Geo& geo) {
  std::map<std::string, std::string, std::less<>> types;
  std::map<std::string, bool, std::less<>> bad;
  for (const ir::Feature& f : geo.features) {
    for (const auto& m : f.properties.items()) {
      const std::string key(m.key());
      if (key == "name" || key == "description") continue;
      const std::string_view t = scalar_type(m.value());
      if (m.value().is_null()) continue;
      if (t.empty()) {
        bad[key] = true;
        continue;
      }
      auto it = types.find(key);
      if (it == types.end()) {
        types[key] = std::string(t);
      } else if (it->second != t) {
        if ((it->second == "int" && t == "double") || (it->second == "double" && t == "int")) {
          it->second = "double";
        } else {
          bad[key] = true;
        }
      }
    }
  }
  for (auto& [k, t] : types) {
    if (!bad.contains(k)) w.field_types[k] = t;
  }
}

void write_style(util::XmlNode placemark, const ir::Style& s) {
  if (s.empty()) return;
  util::XmlNode style = placemark.append_child("Style");
  if (!s.marker_color.empty() || !s.icon.empty() || s.icon_scale != 0.0) {
    util::XmlNode icon = style.append_child("IconStyle");
    if (!s.marker_color.empty()) xml::add_text(icon, "color", kml_color(s.marker_color, 1.0));
    if (s.icon_scale != 0.0) xml::add_text(icon, "scale", format_number(s.icon_scale));
    if (!s.icon.empty()) xml::add_text(icon.append_child("Icon"), "href", s.icon);
  }
  if (!s.stroke.empty() || s.stroke_width != 0.0 || s.stroke_opacity != 1.0) {
    util::XmlNode line = style.append_child("LineStyle");
    xml::add_text(line, "color", kml_color(s.stroke.empty() ? "#ffffff" : s.stroke, s.stroke_opacity));
    if (s.stroke_width != 0.0) xml::add_text(line, "width", format_number(s.stroke_width));
  }
  if (!s.fill.empty() || s.fill_opacity != 1.0) {
    util::XmlNode poly = style.append_child("PolyStyle");
    xml::add_text(poly, "color", kml_color(s.fill.empty() ? "#ffffff" : s.fill, s.fill_opacity));
  }
}

Result<Bytes> write_kml_document(const ir::Geo& geo, const WriteContext& ctx) {
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  util::XmlNode kml = doc.append_child("kml");
  kml.set_attribute("xmlns", kml_ns);
  kml.set_attribute("xmlns:gx", gx_ns);
  KmlWriter w;
  w.gx_tracks = ctx.options.get_enum("kml_tracks", "gx", {"gx", "linestring"}) == "gx";
  w.document = kml.append_child("Document");
  xml::add_text(w.document, "name", geo.name);
  xml::add_text(w.document, "description", geo.description);
  infer_schema(w, geo);
  util::XmlNode schema;
  if (!w.field_types.empty()) {
    schema = w.document.append_child("Schema");
    schema.set_attribute("name", "properties");
    schema.set_attribute("id", "properties");
    for (const auto& [k, t] : w.field_types) {
      util::XmlNode field = schema.append_child("SimpleField");
      field.set_attribute("name", k);
      field.set_attribute("type", t);
    }
  }
  std::size_t index = 0;
  for (const ir::Feature& f : geo.features) {
    if ((++index & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(geo.features.size())));
    util::XmlNode placemark = folder_for(w, f.folder).append_child("Placemark");
    if (f.id.is_string() || f.id.is_number()) {
      const std::string id = json_to_text(f.id);
      if (util::xml_is_name(id)) placemark.set_attribute("id", id);
    }
    xml::add_text(placemark, "name", feature_name(f));
    std::string description = f.description;
    if (description.empty()) {
      if (const Json* d = f.properties.find("description"); d != nullptr && d->is_string()) description = d->as_string();
    }
    xml::add_text(placemark, "description", description);
    write_style(placemark, f.style);
    // Properties: typed ones in the Schema, the rest as Data.
    util::XmlNode extended;
    util::XmlNode schema_data;
    for (const auto& m : f.properties.items()) {
      const std::string key(m.key());
      if (key == "name" || key == "description" || m.value().is_null()) continue;
      if (!extended) extended = placemark.append_child("ExtendedData");
      if (w.field_types.contains(key)) {
        if (!schema_data) {
          schema_data = extended.append_child("SchemaData");
          schema_data.set_attribute("schemaUrl", "#properties");
        }
        util::XmlNode d = schema_data.append_child("SimpleData");
        d.set_attribute("name", key);
        d.append_text(json_to_text(m.value()));
      } else {
        util::XmlNode d = extended.append_child("Data");
        d.set_attribute("name", key);
        xml::add_text(d, "value", json_to_text(m.value()));
      }
    }
    write_geometry(w, placemark, f.geometry, 0);
  }
  if (w.sensor_schema) {
    util::XmlNode sensors = w.document.prepend_child("Schema");
    sensors.set_attribute("id", "track_sensors");
    for (std::string_view name : {"heartrate", "cadence", "power", "speed", "temperature"}) {
      util::XmlNode field = sensors.append_child("gx:SimpleArrayField");
      field.set_attribute("name", name);
      field.set_attribute("type", name == "heartrate" || name == "cadence" ? "int" : "float");
    }
  }
  return xml::finish(doc);
}

Result<Bytes> write_kml(const ir::Geo& geo, WriteContext& ctx) {
  if (!geo.resources.empty()) ctx.warn("Embedded files of the source cannot be stored in a KML file; write KMZ to keep them.");
  return write_kml_document(geo, ctx);
}

Result<Bytes> write_kmz(const ir::Geo& geo, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const Bytes kml, write_kml_document(geo, ctx));
  util::ZipWriter zip;
  util::ZipEntryOptions options;
  options.level = options::resolved_compression_level(ctx.options);
  CVT_TRY(zip.add("doc.kml", kml, options));
  for (const ir::GeoResource& r : geo.resources) {
    const auto path = util::sanitize_entry_path(r.path);
    if (!path || *path == "doc.kml") continue;
    CVT_TRY(zip.add(*path, r.data, options));
  }
  return zip.finish();
}

int sniff_kml(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 4096);
  if (!head.starts_with('<')) return 0;
  return head.find("<kml") != std::string_view::npos ? 92 : 0;
}

int sniff_kmz(ByteView data, std::string_view) { return zip_has_entry_with_extension(data, ".kml") ? 95 : 0; }

}  // namespace

void register_kml(Registry& registry) {
  FormatInfo info;
  info.id = "kml";
  info.name = "Keyhole Markup Language";
  info.description = "Placemarks, paths and polygons for Google Earth and other globes (KML 2.2 / 2.3).";
  info.category = Category::Geo;
  info.extensions = {"kml"};
  info.mime = "application/vnd.google-earth.kml+xml";
  info.popularity = 68;
  info.sniff = sniff_kml;
  registry.add_format(std::move(info));

  const auto kml_options = [] {
    return std::vector<OptionSpec>{OptionSpec::choice(
        "kml_tracks", "Timed tracks", "gx", {"gx", "linestring"},
        "Write tracks whose points all have a time as gx:Track (keeps time and sensor data) or as plain LineStrings.")};
  };
  GeoCodecSpec spec;
  spec.format_id = "kml";
  spec.read = read_kml;
  spec.write = write_kml;
  spec.traits = GeoWriterTraits{false, true, true, true};
  spec.write_options = kml_options();
  add_geo_codec(registry, std::move(spec));

  FormatInfo kmz;
  kmz.id = "kmz";
  kmz.name = "Zipped KML";
  kmz.description = "A KML document compressed in a ZIP archive with its icons and images.";
  kmz.category = Category::Geo;
  kmz.extensions = {"kmz"};
  kmz.mime = "application/vnd.google-earth.kmz";
  kmz.popularity = 55;
  kmz.sniff = sniff_kmz;
  registry.add_format(std::move(kmz));

  GeoCodecSpec kmz_spec;
  kmz_spec.format_id = "kmz";
  kmz_spec.read = read_kmz;
  kmz_spec.write = write_kmz;
  kmz_spec.traits = GeoWriterTraits{false, true, true, true};
  kmz_spec.write_options = kml_options();
  kmz_spec.write_options.push_back(options::compression_level());
  add_geo_codec(registry, std::move(kmz_spec));
}

}  // namespace cvt::geo
