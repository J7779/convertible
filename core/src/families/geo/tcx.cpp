// Convertible core - geo family: Garmin Training Center XML (TCX).
//
// Reading: Activities (one track per Activity, one segment per Lap Track,
// lap summaries as properties, the sport), Courses (track plus CoursePoints
// as waypoints) and the Author block (Geo::metadata). Trackpoints carry
// time, position, altitude, heart rate, cadence and the ActivityExtension
// speed / watts / run cadence. Trackpoints without a position are skipped.
// Writing: an activity when the tracks have times (one Lap per segment with
// computed totals), otherwise a course.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
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
#include "families/geo/geo_ops.hpp"
#include "families/geo/xml_geo.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::geo {

namespace {

constexpr std::string_view tcx_ns = "http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2";
constexpr std::string_view ax_ns = "http://www.garmin.com/xmlschemas/ActivityExtension/v2";

std::optional<double> value_of(util::XmlNode node, std::string_view local) {
  const util::XmlNode child = node.child_local(local);
  if (!child) return std::nullopt;
  if (const util::XmlNode v = child.child_local("Value")) return xml::number(v);
  return xml::number(child);
}

std::optional<ir::Coord> read_trackpoint(util::XmlNode tp) {
  const util::XmlNode pos = tp.child_local("Position");
  if (!pos) return std::nullopt;
  const auto lat = xml::child_number(pos, "LatitudeDegrees");
  const auto lon = xml::child_number(pos, "LongitudeDegrees");
  if (!lat || !lon) return std::nullopt;
  ir::Coord c;
  c.lat = *lat;
  c.lon = *lon;
  if (auto alt = xml::child_number(tp, "AltitudeMeters")) c.ele = *alt;
  const std::string time = xml::child_text(tp, "Time");
  if (!time.empty()) c.time_ms = parse_time(time);
  if (auto hr = value_of(tp, "HeartRateBpm")) c.heart_rate = *hr;
  if (auto cad = xml::child_number(tp, "Cadence")) c.cadence = *cad;
  if (const util::XmlNode ext = tp.child_local("Extensions")) {
    for (util::XmlNode e : ext.descendants()) {
      if (!e.is_element()) continue;
      const std::string_view n = e.local_name();
      if (n == "Speed") {
        if (auto v = xml::number(e)) c.speed = *v;
      } else if (n == "Watts") {
        if (auto v = xml::number(e)) c.power = *v;
      } else if (n == "RunCadence" && !c.cadence) {
        if (auto v = xml::number(e)) c.cadence = *v;
      }
    }
  }
  return c;
}

void read_tracks(util::XmlNode parent, ir::MultiLineString& out) {
  for (util::XmlNode track : parent.children_local("Track")) {
    ir::LineString line;
    for (util::XmlNode tp : track.children_local("Trackpoint")) {
      if (auto c = read_trackpoint(tp)) line.points.push_back(std::move(*c));
    }
    if (!line.points.empty()) out.lines.push_back(std::move(line));
  }
}

Json lap_summary(util::XmlNode lap) {
  Json s = Json::object();
  if (const util::XmlAttribute start = lap.attribute("StartTime")) s.set("start_time", std::string(start.value()));
  const auto number = [&](std::string_view element, std::string_view key) {
    if (auto v = value_of(lap, element)) s.set(key, *v);
  };
  number("TotalTimeSeconds", "total_time_s");
  number("DistanceMeters", "distance_m");
  number("MaximumSpeed", "max_speed_mps");
  number("Calories", "calories");
  number("AverageHeartRateBpm", "avg_heart_rate");
  number("MaximumHeartRateBpm", "max_heart_rate");
  number("Cadence", "cadence");
  const std::string intensity = xml::child_text(lap, "Intensity");
  if (!intensity.empty()) s.set("intensity", intensity);
  const std::string trigger = xml::child_text(lap, "TriggerMethod");
  if (!trigger.empty()) s.set("trigger", trigger);
  return s;
}

Result<ir::Geo> read_tcx(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::XmlDocument doc, xml::parse(ctx.input, "TCX"));
  const util::XmlNode root = doc.root();
  if (root.local_name() != "TrainingCenterDatabase") {
    return fail(Status::Decode, "not a TCX file (root element <{}>)", root.local_name());
  }
  ir::Geo geo;
  if (const util::XmlNode author = root.child_local("Author")) {
    const std::string name = xml::child_text(author, "Name");
    if (!name.empty()) geo.metadata.set("creator", name);
  }
  for (util::XmlNode activity : root.child_local("Activities").children_local("Activity")) {
    CVT_TRY(ctx.checkpoint(0.5));
    ir::Feature f;
    f.role = ir::FeatureRole::Track;
    const std::string sport(activity.attribute("Sport").value());
    if (!sport.empty()) f.properties.set("sport", sport);
    const std::string id = xml::child_text(activity, "Id");
    if (!id.empty()) {
      f.properties.set("id", id);
      f.id = Json(id);
    }
    const std::string notes = xml::child_text(activity, "Notes");
    if (!notes.empty()) f.description = notes;
    ir::MultiLineString track;
    Json laps = Json::array();
    for (util::XmlNode lap : activity.children_local("Lap")) {
      read_tracks(lap, track);
      laps.push_back(lap_summary(lap));
    }
    if (!laps.empty()) f.properties.set("laps", std::move(laps));
    if (const util::XmlNode creator = activity.child_local("Creator")) {
      const std::string name = xml::child_text(creator, "Name");
      if (!name.empty()) geo.metadata.set("device", name);
    }
    f.geometry.shape = std::move(track);
    geo.features.push_back(std::move(f));
  }
  for (util::XmlNode course : root.child_local("Courses").children_local("Course")) {
    ir::Feature f;
    f.role = ir::FeatureRole::Track;
    f.name = xml::child_text(course, "Name");
    if (!f.name.empty()) f.properties.set("name", f.name);
    ir::MultiLineString track;
    read_tracks(course, track);
    Json laps = Json::array();
    for (util::XmlNode lap : course.children_local("Lap")) laps.push_back(lap_summary(lap));
    if (!laps.empty()) f.properties.set("laps", std::move(laps));
    f.geometry.shape = std::move(track);
    geo.features.push_back(std::move(f));
    for (util::XmlNode point : course.children_local("CoursePoint")) {
      ir::Feature w;
      w.role = ir::FeatureRole::Waypoint;
      const auto c = read_trackpoint(point);
      if (!c) continue;
      w.geometry.shape = ir::Point{*c};
      w.name = xml::child_text(point, "Name");
      if (!w.name.empty()) w.properties.set("name", w.name);
      const std::string type = xml::child_text(point, "PointType");
      if (!type.empty()) w.properties.set("type", type);
      geo.features.push_back(std::move(w));
    }
  }
  if (geo.features.empty()) return fail(Status::Decode, "the TCX file holds no activity or course");
  return geo;
}

// --------------------------------------------------------------------------- writing

std::string tcx_sport(std::string_view sport) {
  const std::string s = util::to_lower(sport);
  if (s == "running" || s == "run") return "Running";
  if (s == "biking" || s == "cycling" || s == "ride" || s == "bike") return "Biking";
  return "Other";
}

void write_trackpoint(util::XmlNode track, const ir::Coord& c, double distance) {
  util::XmlNode tp = track.append_child("Trackpoint");
  if (c.time_ms) xml::add_text(tp, "Time", format_time(*c.time_ms));
  util::XmlNode pos = tp.append_child("Position");
  xml::add_text(pos, "LatitudeDegrees", format_number(c.lat));
  xml::add_text(pos, "LongitudeDegrees", format_number(c.lon));
  if (c.ele) xml::add_text(tp, "AltitudeMeters", format_number(*c.ele));
  xml::add_text(tp, "DistanceMeters", format_number(distance, 2));
  if (c.heart_rate) {
    xml::add_text(tp.append_child("HeartRateBpm"), "Value", format_number(std::round(*c.heart_rate), 0));
  }
  if (c.cadence) xml::add_text(tp, "Cadence", format_number(std::clamp(std::round(*c.cadence), 0.0, 254.0), 0));
  if (c.speed || c.power) {
    util::XmlNode tpx = tp.append_child("Extensions").append_child("ns3:TPX");
    if (c.speed) xml::add_text(tpx, "ns3:Speed", format_number(*c.speed));
    if (c.power) xml::add_text(tpx, "ns3:Watts", format_number(std::round(*c.power), 0));
  }
}

Result<Bytes> write_tcx(const ir::Geo& geo, WriteContext& ctx) {
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  util::XmlNode root = doc.append_child("TrainingCenterDatabase");
  root.set_attribute("xmlns", tcx_ns);
  root.set_attribute("xmlns:ns3", ax_ns);
  struct Source {
    const ir::Feature* feature;
    std::vector<const std::vector<ir::Coord>*> segments;
  };
  std::vector<Source> sources;
  std::vector<const ir::Feature*> waypoints;
  for (const ir::Feature& f : geo.features) {
    Source s{&f, {}};
    if (const auto* ml = std::get_if<ir::MultiLineString>(&f.geometry.shape)) {
      for (const auto& l : ml->lines) {
        if (!l.points.empty()) s.segments.push_back(&l.points);
      }
    } else if (const auto* l = std::get_if<ir::LineString>(&f.geometry.shape)) {
      if (!l->points.empty()) s.segments.push_back(&l->points);
    } else if (std::holds_alternative<ir::Point>(f.geometry.shape)) {
      waypoints.push_back(&f);
      continue;
    }
    if (s.segments.empty()) continue;
    sources.push_back(std::move(s));
  }
  if (sources.empty()) return fail(Status::Encode, "TCX needs a track or a line; there is none");
  // An activity when some line has a time at every point; untimed lines (routes) are then left out.
  const auto all_timed = [](const Source& s) {
    return std::all_of(s.segments.begin(), s.segments.end(), [](const auto* seg) {
      return std::all_of(seg->begin(), seg->end(), [](const ir::Coord& c) { return c.time_ms.has_value(); });
    });
  };
  const bool timed = std::any_of(sources.begin(), sources.end(), all_timed);
  if (timed) {
    const std::size_t before = sources.size();
    std::erase_if(sources, [&](const Source& s) { return !all_timed(s); });
    if (sources.size() < before) ctx.warn("Lines without times (routes) cannot be part of a TCX activity and were left out.");
  }
  std::size_t skipped = 0;
  if (timed) {
    util::XmlNode activities = root.append_child("Activities");
    for (const Source& s : sources) {
      util::XmlNode activity = activities.append_child("Activity");
      activity.set_attribute("Sport", tcx_sport(s.feature->properties["sport"].as_string()));
      xml::add_text(activity, "Id", format_time(*s.segments.front()->front().time_ms));
      double distance = 0;
      for (const auto* seg : s.segments) {
        CVT_TRY(ctx.checkpoint(0.5));
        util::XmlNode lap = activity.append_child("Lap");
        lap.set_attribute("StartTime", format_time(*seg->front().time_ms));
        double lap_distance = 0, max_speed = 0, hr_sum = 0, hr_max = 0;
        std::size_t hr_n = 0;
        for (std::size_t i = 1; i < seg->size(); ++i) {
          const double d = geodesic_distance((*seg)[i - 1], (*seg)[i]);
          lap_distance += d;
          const double dt = static_cast<double>(*(*seg)[i].time_ms - *(*seg)[i - 1].time_ms) / 1000.0;
          if (dt > 0) max_speed = std::max(max_speed, d / dt);
        }
        for (const ir::Coord& c : *seg) {
          if (c.heart_rate) {
            hr_sum += *c.heart_rate;
            hr_max = std::max(hr_max, *c.heart_rate);
            ++hr_n;
          }
        }
        xml::add_text(lap, "TotalTimeSeconds",
                      format_number(static_cast<double>(*seg->back().time_ms - *seg->front().time_ms) / 1000.0, 3));
        xml::add_text(lap, "DistanceMeters", format_number(lap_distance, 2));
        xml::add_text(lap, "MaximumSpeed", format_number(max_speed, 3));
        xml::add_text(lap, "Calories", "0");
        if (hr_n > 0) {
          xml::add_text(lap.append_child("AverageHeartRateBpm"), "Value", format_number(std::round(hr_sum / static_cast<double>(hr_n)), 0));
          xml::add_text(lap.append_child("MaximumHeartRateBpm"), "Value", format_number(std::round(hr_max), 0));
        }
        xml::add_text(lap, "Intensity", "Active");
        xml::add_text(lap, "TriggerMethod", "Manual");
        util::XmlNode track = lap.append_child("Track");
        for (std::size_t i = 0; i < seg->size(); ++i) {
          if (i > 0) distance += geodesic_distance((*seg)[i - 1], (*seg)[i]);
          write_trackpoint(track, (*seg)[i], distance);
        }
      }
    }
    if (!waypoints.empty()) ctx.warn("TCX activities have no waypoints; points were left out.");
  } else {
    util::XmlNode courses = root.append_child("Courses");
    for (const Source& s : sources) {
      util::XmlNode course = courses.append_child("Course");
      std::string name = feature_name(*s.feature);
      if (name.empty()) name = "Course";
      xml::add_text(course, "Name", name);
      double total = 0;
      for (const auto* seg : s.segments) {
        for (std::size_t i = 1; i < seg->size(); ++i) total += geodesic_distance((*seg)[i - 1], (*seg)[i]);
      }
      util::XmlNode lap = course.append_child("Lap");
      xml::add_text(lap, "TotalTimeSeconds", "0");
      xml::add_text(lap, "DistanceMeters", format_number(total, 2));
      const ir::Coord& first = s.segments.front()->front();
      const ir::Coord& last = s.segments.back()->back();
      util::XmlNode begin = lap.append_child("BeginPosition");
      xml::add_text(begin, "LatitudeDegrees", format_number(first.lat));
      xml::add_text(begin, "LongitudeDegrees", format_number(first.lon));
      util::XmlNode end = lap.append_child("EndPosition");
      xml::add_text(end, "LatitudeDegrees", format_number(last.lat));
      xml::add_text(end, "LongitudeDegrees", format_number(last.lon));
      xml::add_text(lap, "Intensity", "Active");
      util::XmlNode track = course.append_child("Track");
      double distance = 0;
      for (const auto* seg : s.segments) {
        for (std::size_t i = 0; i < seg->size(); ++i) {
          if (i > 0) distance += geodesic_distance((*seg)[i - 1], (*seg)[i]);
          write_trackpoint(track, (*seg)[i], distance);
        }
      }
      for (const ir::Feature* w : waypoints) {
        const ir::Coord& c = std::get<ir::Point>(w->geometry.shape).coord;
        if (!c.time_ms) {
          ++skipped;
          continue;
        }
        util::XmlNode point = course.append_child("CoursePoint");
        xml::add_text(point, "Name", feature_name(*w).substr(0, 10));
        xml::add_text(point, "Time", format_time(*c.time_ms));
        util::XmlNode pos = point.append_child("Position");
        xml::add_text(pos, "LatitudeDegrees", format_number(c.lat));
        xml::add_text(pos, "LongitudeDegrees", format_number(c.lon));
        xml::add_text(point, "PointType", "Generic");
      }
    }
    ctx.warn("The tracks have no times, so a TCX course without trackpoint times was written.");
    if (skipped > 0) ctx.warn("Waypoints without a time cannot be TCX course points and were left out.");
  }
  return xml::finish(doc);
}

int sniff_tcx(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 4096);
  return head.starts_with('<') && head.find("<TrainingCenterDatabase") != std::string_view::npos ? 92 : 0;
}

}  // namespace

void register_tcx(Registry& registry) {
  FormatInfo info;
  info.id = "tcx";
  info.name = "Training Center XML";
  info.description = "Garmin Training Center activities and courses with heart rate, cadence and power.";
  info.category = Category::Geo;
  info.extensions = {"tcx"};
  info.mime = "application/vnd.garmin.tcx+xml";
  info.popularity = 45;
  info.sniff = sniff_tcx;
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "tcx";
  spec.read = read_tcx;
  spec.write = write_tcx;
  spec.traits = GeoWriterTraits{false, true, true, true};
  spec.lossy_write = true;
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
