// Convertible core - geo family: location history exported by Google
// (Takeout "Records.json", semantic location history, and the newer
// on-device Timeline export). Read only.
//
//   Records.json          {"locations": [{latitudeE7, longitudeE7, timestamp | timestampMs,
//                          altitude, accuracy, velocity, ...}]}         -> one track
//   Semantic history      {"timelineObjects": [{placeVisit}, {activitySegment}]}
//                                                                       -> waypoints + tracks
//   Timeline export       {"semanticSegments": [{visit | activity | timelinePath}], "rawSignals"}
//                          or a top-level array of the same with "geo:lat,lon" strings
// The exported data is the user's own; nothing is added or sent anywhere.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
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
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::geo {

namespace {

std::optional<ir::Coord> e7(const Json& o, std::string_view lat_key, std::string_view lon_key) {
  const auto lat = o[lat_key].get_double();
  const auto lon = o[lon_key].get_double();
  if (!lat || !lon) return std::nullopt;
  ir::Coord c;
  c.lat = *lat / 1e7;
  c.lon = *lon / 1e7;
  return c;
}

// "40.2°, -29.45°", "geo:40.2,-29.45" or "40.2,-29.45".
std::optional<ir::Coord> lat_lng_text(std::string_view text) {
  text = util::trim(text);
  if (text.starts_with("geo:")) text.remove_prefix(4);
  std::string cleaned;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text.substr(i).starts_with("\xC2\xB0")) {  // degree sign
      ++i;
      continue;
    }
    cleaned += text[i];
  }
  const std::size_t comma = cleaned.find(',');
  if (comma == std::string::npos) return std::nullopt;
  const auto lat = parse_number(std::string_view(cleaned).substr(0, comma));
  const auto lon = parse_number(std::string_view(cleaned).substr(comma + 1));
  if (!lat || !lon || std::fabs(*lat) > 90 || std::fabs(*lon) > 180) return std::nullopt;
  ir::Coord c;
  c.lat = *lat;
  c.lon = *lon;
  return c;
}

std::optional<std::int64_t> time_of(const Json& o) {
  for (std::string_view key : {"timestamp", "time", "timestampMs", "startTimestamp"}) {
    const Json& v = o[key];
    if (v.is_string()) {
      if (auto t = parse_time(v.as_string())) return t;
      if (auto ms = util::parse_int(v.as_string())) return *ms;
    } else if (auto ms = v.get_int64()) {
      return *ms;
    }
  }
  return std::nullopt;
}

ir::Feature track_feature(ir::LineString line, Json properties) {
  ir::Feature f;
  f.role = ir::FeatureRole::Track;
  f.geometry.shape = ir::MultiLineString{{std::move(line)}};
  f.properties = std::move(properties);
  return f;
}

ir::Feature visit_feature(const ir::Coord& c, std::string name, Json properties) {
  ir::Feature f;
  f.role = ir::FeatureRole::Waypoint;
  f.geometry.shape = ir::Point{c};
  f.name = std::move(name);
  f.properties = std::move(properties);
  if (!f.name.empty()) f.properties.set("name", f.name);
  return f;
}

void read_records(const Json& locations, ir::Geo& geo) {
  ir::LineString line;
  for (const Json& l : locations.elements()) {
    auto c = e7(l, "latitudeE7", "longitudeE7");
    if (!c) continue;
    c->time_ms = time_of(l);
    if (auto alt = l["altitude"].get_double()) c->ele = *alt;
    if (auto v = l["velocity"].get_double()) c->speed = *v;
    if (auto a = l["accuracy"].get_double()) c->extra.emplace_back("accuracy", *a);
    line.points.push_back(std::move(*c));
  }
  if (!line.points.empty()) geo.features.push_back(track_feature(std::move(line), Json::object()));
}

void read_semantic(const Json& objects, ir::Geo& geo) {
  for (const Json& o : objects.elements()) {
    if (const Json* visit = o.find("placeVisit")) {
      const Json& loc = (*visit)["location"];
      auto c = e7(loc, "latitudeE7", "longitudeE7");
      if (!c) continue;
      Json props = Json::object();
      if (loc["address"].is_string()) props.set("address", loc["address"]);
      const Json& d = (*visit)["duration"];
      if (d["startTimestamp"].is_string()) props.set("start_time", d["startTimestamp"]);
      if (d["endTimestamp"].is_string()) props.set("end_time", d["endTimestamp"]);
      c->time_ms = time_of(d);
      geo.features.push_back(visit_feature(*c, loc["name"].as_string(), std::move(props)));
    } else if (const Json* segment = o.find("activitySegment")) {
      ir::LineString line;
      const Json& d = (*segment)["duration"];
      if (auto s = e7((*segment)["startLocation"], "latitudeE7", "longitudeE7")) {
        s->time_ms = parse_time(d["startTimestamp"].as_string());
        line.points.push_back(*s);
      }
      for (const Json& p : (*segment)["simplifiedRawPath"]["points"].elements()) {
        auto c = e7(p, "latE7", "lngE7");
        if (!c) continue;
        c->time_ms = time_of(p);
        line.points.push_back(*c);
      }
      for (const Json& p : (*segment)["waypointPath"]["waypoints"].elements()) {
        if (auto c = e7(p, "latE7", "lngE7")) line.points.push_back(*c);
      }
      if (auto e = e7((*segment)["endLocation"], "latitudeE7", "longitudeE7")) {
        e->time_ms = parse_time(d["endTimestamp"].as_string());
        line.points.push_back(*e);
      }
      // The start / end locations often repeat the first / last raw point.
      std::vector<ir::Coord> unique;
      for (auto& c : line.points) {
        if (!unique.empty() && unique.back().lat == c.lat && unique.back().lon == c.lon) {
          if (!unique.back().time_ms) unique.back().time_ms = c.time_ms;
          continue;
        }
        unique.push_back(std::move(c));
      }
      line.points = std::move(unique);
      Json props = Json::object();
      if ((*segment)["activityType"].is_string()) props.set("activity", (*segment)["activityType"]);
      if (d["startTimestamp"].is_string()) props.set("start_time", d["startTimestamp"]);
      if (d["endTimestamp"].is_string()) props.set("end_time", d["endTimestamp"]);
      if (!line.points.empty()) geo.features.push_back(track_feature(std::move(line), std::move(props)));
    }
  }
}

void read_timeline_segments(const Json& segments, ir::Geo& geo) {
  ir::LineString path;
  for (const Json& s : segments.elements()) {
    Json props = Json::object();
    if (s["startTime"].is_string()) props.set("start_time", s["startTime"]);
    if (s["endTime"].is_string()) props.set("end_time", s["endTime"]);
    if (const Json* visit = s.find("visit")) {
      const Json& top = (*visit)["topCandidate"];
      const Json& place = top.contains("placeLocation") ? top["placeLocation"] : top;
      std::optional<ir::Coord> c;
      if (place.is_string()) c = lat_lng_text(place.as_string());
      else c = lat_lng_text(place["latLng"].as_string());
      if (!c) continue;
      c->time_ms = parse_time(s["startTime"].as_string());
      if (top["semanticType"].is_string()) props.set("type", top["semanticType"]);
      geo.features.push_back(visit_feature(*c, {}, std::move(props)));
    } else if (const Json* activity = s.find("activity")) {
      ir::LineString line;
      const Json& start = (*activity)["start"];
      const Json& end = (*activity)["end"];
      auto a = lat_lng_text(start.is_string() ? start.as_string() : start["latLng"].as_string());
      auto b = lat_lng_text(end.is_string() ? end.as_string() : end["latLng"].as_string());
      if (a) {
        a->time_ms = parse_time(s["startTime"].as_string());
        line.points.push_back(*a);
      }
      if (b) {
        b->time_ms = parse_time(s["endTime"].as_string());
        line.points.push_back(*b);
      }
      const Json& type = (*activity)["topCandidate"]["type"];
      if (type.is_string()) props.set("activity", type);
      if (line.points.size() >= 2) geo.features.push_back(track_feature(std::move(line), std::move(props)));
    }
    if (const Json* points = s.find("timelinePath"); points != nullptr && points->is_array()) {
      for (const Json& p : points->elements()) {
        auto c = lat_lng_text(p["point"].as_string());
        if (!c) continue;
        c->time_ms = parse_time(p["time"].as_string());
        if (!c->time_ms) {
          // Older exports: minutes after the segment start.
          const auto base = parse_time(s["startTime"].as_string());
          if (auto offset = util::parse_int(p["durationMinutesOffsetFromStartTime"].as_string()); base && offset) {
            c->time_ms = *base + *offset * 60'000;
          }
        }
        path.points.push_back(std::move(*c));
      }
    }
  }
  if (!path.points.empty()) geo.features.push_back(track_feature(std::move(path), Json::object()));
}

void read_raw_signals(const Json& signals, ir::Geo& geo) {
  ir::LineString line;
  for (const Json& s : signals.elements()) {
    const Json& p = s["position"];
    if (!p.is_object()) continue;
    auto c = lat_lng_text(p["LatLng"].is_string() ? p["LatLng"].as_string() : p["latLng"].as_string());
    if (!c) continue;
    c->time_ms = parse_time(p["timestamp"].as_string());
    if (auto alt = p["altitudeMeters"].get_double()) c->ele = *alt;
    if (auto v = p["speedMetersPerSecond"].get_double()) c->speed = *v;
    line.points.push_back(std::move(*c));
  }
  if (!line.points.empty()) {
    Json props = Json::object({{"source", "raw signals"}});
    geo.features.push_back(track_feature(std::move(line), std::move(props)));
  }
}

Result<ir::Geo> read_takeout(ReadContext& ctx) {
  auto root = util::parse_json(input_text(ctx.input));
  if (!root) return fail(Status::Decode, "not valid location history JSON: {}", root.error().message);
  CVT_TRY(ctx.checkpoint(0.5));
  ir::Geo geo;
  if (root->is_array()) {
    read_timeline_segments(*root, geo);
  } else if (const Json* locations = root->find("locations"); locations != nullptr && locations->is_array()) {
    read_records(*locations, geo);
  } else if (const Json* objects = root->find("timelineObjects"); objects != nullptr && objects->is_array()) {
    read_semantic(*objects, geo);
  } else if (const Json* segments = root->find("semanticSegments"); segments != nullptr && segments->is_array()) {
    read_timeline_segments(*segments, geo);
    if (const Json* raw = root->find("rawSignals"); raw != nullptr && raw->is_array()) read_raw_signals(*raw, geo);
  } else {
    return fail(Status::Decode, "not a location history export");
  }
  if (geo.features.empty()) return fail(Status::Decode, "the location history holds no positions");
  return geo;
}

int sniff_takeout(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 4096);
  if (head.starts_with('{')) {
    if (head.find("\"locations\"") != std::string_view::npos && head.find("\"latitudeE7\"") != std::string_view::npos) return 95;
    if (head.find("\"timelineObjects\"") != std::string_view::npos) return 95;
    if (head.find("\"semanticSegments\"") != std::string_view::npos) return 95;
    return 0;
  }
  if (head.starts_with('[') && (head.find("\"timelinePath\"") != std::string_view::npos ||
                                head.find("\"geo:") != std::string_view::npos)) {
    return 85;
  }
  return 0;
}

}  // namespace

void register_takeout(Registry& registry) {
  FormatInfo info;
  info.id = "takeout-location";
  info.name = "Google location history";
  info.description = "Location history exported from Google Takeout or the Timeline app (read only).";
  info.category = Category::Geo;
  info.extensions = {"json"};
  info.mime = "application/json";
  info.popularity = 25;
  info.sniff = sniff_takeout;
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "takeout-location";
  spec.read = read_takeout;
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
