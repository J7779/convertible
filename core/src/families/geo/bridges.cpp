// Convertible core - geo family: bridges from Geo to the other kinds.
//
//   Geo -> Data      the GeoJSON object model (FeatureCollection)
//   Geo -> Table     one row per feature (properties as columns plus WKT or
//                    latitude/longitude), or one row per point with time and
//                    sensor data (GPS logs: CSV / spreadsheet exports)
//   Geo -> Document  a report: counts, extent, lengths and areas, track
//                    statistics, a map sketch and a properties table
//   Geo -> Image     a rendered map sketch (render.hpp)
// All of them apply the common write options first (precision, CRS,
// simplification, metadata stripping).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "families/geo/geo.hpp"
#include "families/geo/geo_ops.hpp"
#include "families/geo/render.hpp"
#include "families/geo/wkt.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/image.hpp"
#include "ir/table.hpp"

namespace cvt::geo {

namespace {

constexpr GeoWriterTraits bridge_traits{true, true, true, true};

// --------------------------------------------------------------------------- Table

ir::Cell cell_of(const Json& v) {
  switch (v.type()) {
    case Json::Type::Null: return ir::Cell{};
    case Json::Type::Bool: return ir::Cell::boolean(v.as_bool());
    case Json::Type::Int64:
    case Json::Type::UInt64:
    case Json::Type::Double: return ir::Cell::numeric(v.as_double(), json_to_text(v));
    default: return ir::Cell::string(json_to_text(v));
  }
}

ir::Cell number_cell(double v) { return ir::Cell::numeric(v, format_number(v)); }

bool all_tracks(const ir::Geo& geo) {
  return !geo.features.empty() && std::all_of(geo.features.begin(), geo.features.end(), [](const ir::Feature& f) {
    return f.role == ir::FeatureRole::Track || std::holds_alternative<ir::MultiLineString>(f.geometry.shape) ||
           std::holds_alternative<ir::LineString>(f.geometry.shape);
  }) && std::any_of(geo.features.begin(), geo.features.end(), [](const ir::Feature& f) { return has_times(f.geometry); });
}

ir::Sheet feature_rows(const ir::Geo& geo, std::string geometry_mode) {
  ir::Sheet sheet;
  sheet.name = geo.name.empty() ? "features" : geo.name;
  sheet.has_header = true;
  const bool all_points = std::all_of(geo.features.begin(), geo.features.end(), [](const ir::Feature& f) {
    return std::holds_alternative<ir::Point>(f.geometry.shape);
  });
  if (geometry_mode == "auto") geometry_mode = all_points ? "latlon" : "wkt";
  const bool wkt = geometry_mode == "wkt" || geometry_mode == "both";
  const bool latlon = geometry_mode == "latlon" || geometry_mode == "both";
  bool any_ele = false, any_time = false;
  for (const auto& f : geo.features) {
    if (const auto* p = std::get_if<ir::Point>(&f.geometry.shape)) {
      any_ele = any_ele || p->coord.ele;
      any_time = any_time || p->coord.time_ms;
    }
  }
  std::vector<std::string> keys;
  bool names = false;
  for (const auto& f : geo.features) {
    names = names || (!f.name.empty() && !f.properties.contains("name"));
    for (const auto& m : f.properties.items()) {
      const std::string k(m.key());
      if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    }
  }
  if (names && std::find(keys.begin(), keys.end(), "name") == keys.end()) keys.insert(keys.begin(), "name");
  std::vector<ir::Cell> header;
  if (wkt) header.push_back(ir::Cell::string("WKT"));
  if (latlon) {
    header.push_back(ir::Cell::string("latitude"));
    header.push_back(ir::Cell::string("longitude"));
    if (any_ele) header.push_back(ir::Cell::string("elevation"));
    if (any_time) header.push_back(ir::Cell::string("time"));
  }
  for (const auto& k : keys) header.push_back(ir::Cell::string(k));
  sheet.rows.push_back(std::move(header));
  for (const auto& f : geo.features) {
    std::vector<ir::Cell> row;
    if (wkt) row.push_back(ir::Cell::string(geometry_to_wkt(f.geometry)));
    if (latlon) {
      const auto* p = std::get_if<ir::Point>(&f.geometry.shape);
      row.push_back(p ? number_cell(p->coord.lat) : ir::Cell{});
      row.push_back(p ? number_cell(p->coord.lon) : ir::Cell{});
      if (any_ele) row.push_back(p && p->coord.ele ? number_cell(*p->coord.ele) : ir::Cell{});
      if (any_time) {
        row.push_back(p && p->coord.time_ms
                          ? ir::Cell::datetime(static_cast<double>(*p->coord.time_ms) / 1000.0, format_time(*p->coord.time_ms))
                          : ir::Cell{});
      }
    }
    for (const auto& k : keys) {
      if (k == "name" && !f.properties.contains("name")) {
        row.push_back(ir::Cell::string(f.name));
      } else {
        row.push_back(cell_of(f.properties[k]));
      }
    }
    sheet.rows.push_back(std::move(row));
  }
  return sheet;
}

ir::Sheet point_rows(const ir::Geo& geo) {
  ir::Sheet sheet;
  sheet.name = geo.name.empty() ? "points" : geo.name;
  sheet.has_header = true;
  bool ele = false, time = false, hr = false, cad = false, power = false, speed = false, temp = false;
  std::vector<std::string> extras;
  for (const auto& f : geo.features) {
    for_each_coord(f.geometry, [&](const ir::Coord& c) {
      ele = ele || c.ele;
      time = time || c.time_ms;
      hr = hr || c.heart_rate;
      cad = cad || c.cadence;
      power = power || c.power;
      speed = speed || c.speed;
      temp = temp || c.temperature;
      for (const auto& [name, value] : c.extra) {
        (void)value;
        if (std::find(extras.begin(), extras.end(), name) == extras.end() && extras.size() < 64) extras.push_back(name);
      }
    });
  }
  std::vector<ir::Cell> header = {ir::Cell::string("feature"), ir::Cell::string("segment"), ir::Cell::string("latitude"),
                                  ir::Cell::string("longitude")};
  if (ele) header.push_back(ir::Cell::string("elevation"));
  if (time) header.push_back(ir::Cell::string("time"));
  if (hr) header.push_back(ir::Cell::string("heart_rate"));
  if (cad) header.push_back(ir::Cell::string("cadence"));
  if (power) header.push_back(ir::Cell::string("power"));
  if (speed) header.push_back(ir::Cell::string("speed"));
  if (temp) header.push_back(ir::Cell::string("temperature"));
  for (const auto& e : extras) header.push_back(ir::Cell::string(e));
  sheet.rows.push_back(std::move(header));
  const auto opt = [](const std::optional<double>& v) { return v ? number_cell(*v) : ir::Cell{}; };
  for (std::size_t fi = 0; fi < geo.features.size(); ++fi) {
    const ir::Feature& f = geo.features[fi];
    const std::string name = feature_name(f).empty() ? std::to_string(fi + 1) : feature_name(f);
    std::vector<const std::vector<ir::Coord>*> seqs;
    std::vector<std::vector<ir::Coord>> owned;
    std::visit(
        [&](const auto& s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, ir::Point>) owned.push_back({s.coord});
          else if constexpr (std::is_same_v<T, ir::LineString> || std::is_same_v<T, ir::MultiPoint>) seqs.push_back(&s.points);
          else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
            for (const auto& l : s.lines) seqs.push_back(&l.points);
          } else if constexpr (std::is_same_v<T, ir::Polygon>) {
            for (const auto& r : s.rings) seqs.push_back(&r);
          } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
            for (const auto& p : s.polygons) {
              for (const auto& r : p.rings) seqs.push_back(&r);
            }
          } else {
            std::vector<ir::Coord> all;
            for_each_coord(ir::Geometry{s}, [&](const ir::Coord& c) { all.push_back(c); });
            owned.push_back(std::move(all));
          }
        },
        f.geometry.shape);
    for (const auto& o : owned) seqs.push_back(&o);
    for (std::size_t si = 0; si < seqs.size(); ++si) {
      for (const ir::Coord& c : *seqs[si]) {
        std::vector<ir::Cell> row = {ir::Cell::string(name), number_cell(static_cast<double>(si + 1)), number_cell(c.lat),
                                     number_cell(c.lon)};
        if (ele) row.push_back(opt(c.ele));
        if (time) {
          row.push_back(c.time_ms ? ir::Cell::datetime(static_cast<double>(*c.time_ms) / 1000.0, format_time(*c.time_ms))
                                  : ir::Cell{});
        }
        if (hr) row.push_back(opt(c.heart_rate));
        if (cad) row.push_back(opt(c.cadence));
        if (power) row.push_back(opt(c.power));
        if (speed) row.push_back(opt(c.speed));
        if (temp) row.push_back(opt(c.temperature));
        for (const auto& e : extras) {
          const auto it = std::find_if(c.extra.begin(), c.extra.end(), [&](const auto& kv) { return kv.first == e; });
          row.push_back(it == c.extra.end() ? ir::Cell{} : number_cell(it->second));
        }
        sheet.rows.push_back(std::move(row));
      }
    }
  }
  return sheet;
}

Result<AnyIR> geo_to_table(AnyIR value, Context& ctx) {
  CVT_TRY_ASSIGN(const ir::Geo geo, prepare_for_write(value.get<ir::Geo>(), ctx, bridge_traits));
  std::string rows = ctx.options.get_enum("table_rows", "auto", {"auto", "features", "points"});
  if (rows == "auto") rows = all_tracks(geo) ? "points" : "features";
  ir::Table table;
  if (rows == "points") {
    table.sheets.push_back(point_rows(geo));
  } else {
    table.sheets.push_back(feature_rows(geo, ctx.options.get_enum("geometry_columns", "auto", {"auto", "wkt", "latlon", "both"})));
  }
  return AnyIR(std::move(table));
}

// --------------------------------------------------------------------------- Data

Result<AnyIR> geo_to_data(AnyIR value, Context& ctx) {
  CVT_TRY_ASSIGN(const ir::Geo geo, prepare_for_write(value.get<ir::Geo>(), ctx, bridge_traits));
  GeoJsonStyle style;
  style.rfc7946 = geo.crs.empty();
  return AnyIR(ir::Data{geo_to_geojson(geo, style)});
}

// --------------------------------------------------------------------------- Image

Result<RenderOptions> render_options(const Options& options) {
  RenderOptions r;
  r.width = static_cast<std::uint32_t>(options.get_int("map_width", 1024, 16, 8192));
  r.height = static_cast<std::uint32_t>(options.get_int("map_height", 0, 0, 8192));
  r.graticule = options.get_bool("map_graticule", true);
  r.line_width = options.get_float("map_line_width", 2.5, 0.5, 50);
  const auto color = [&](std::string_view key, Rgba& out) -> Result<void> {
    const std::string text = options.get_string(key, "");
    if (text.empty()) return {};
    const auto parsed = parse_color(text);
    if (!parsed) return fail(Status::InvalidArgument, "option '{}' must be a colour such as #336699", key);
    out = *parsed;
    return {};
  };
  CVT_TRY(color("map_background", r.background));
  CVT_TRY(color("map_line_color", r.line));
  CVT_TRY(color("map_fill_color", r.fill));
  CVT_TRY(color("map_point_color", r.point));
  return r;
}

Result<AnyIR> geo_to_image(AnyIR value, Context& ctx) {
  GeoWriterTraits traits = bridge_traits;
  traits.crs_capable = false;  // the sketch is drawn from WGS 84
  CVT_TRY_ASSIGN(const ir::Geo geo, prepare_for_write(value.get<ir::Geo>(), ctx, traits));
  CVT_TRY_ASSIGN(const RenderOptions options, render_options(ctx.options));
  CVT_TRY_ASSIGN(ir::Image image, render_map(geo, options, ctx));
  return AnyIR(std::move(image));
}

// --------------------------------------------------------------------------- Document

std::string distance_text(double metres) {
  return metres >= 1000 ? format_number(metres / 1000, 2) + " km" : format_number(metres, 1) + " m";
}

std::string area_text(double m2) {
  return m2 >= 1e6 ? format_number(m2 / 1e6, 3) + " km²" : format_number(m2, 0) + " m²";
}

std::string duration_text(double seconds) {
  const auto s = static_cast<std::int64_t>(std::llround(seconds));
  return std::format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60);
}

ir::TableRow row_of(std::vector<std::string> cells, bool header = false) { return ir::build::row(std::move(cells), header); }

Result<AnyIR> geo_to_document(AnyIR value, Context& ctx) {
  GeoWriterTraits traits = bridge_traits;
  traits.crs_capable = false;
  CVT_TRY_ASSIGN(const ir::Geo geo, prepare_for_write(value.get<ir::Geo>(), ctx, traits));
  using namespace ir::build;
  ir::Document doc;
  doc.metadata.title = geo.name.empty() ? "Geographic data" : geo.name;
  doc.blocks.push_back(heading(1, doc.metadata.title));
  if (!geo.description.empty()) doc.blocks.push_back(paragraph(geo.description));
  // Counts per geometry type.
  std::map<std::string, std::size_t> types;
  double length = 0, area = 0;
  std::size_t points = 0;
  for (const auto& f : geo.features) {
    ++types[std::string(geometry_type_name(f.geometry))];
    length += length_m(f.geometry);
    area += area_m2(f.geometry);
    points += coord_count(f.geometry);
  }
  std::string summary = std::format("{} feature{} with {} position{}", geo.features.size(),
                                    geo.features.size() == 1 ? "" : "s", points, points == 1 ? "" : "s");
  if (!types.empty()) {
    summary += " (";
    bool first = true;
    for (const auto& [t, n] : types) {
      summary += (first ? "" : ", ") + std::format("{} {}", n, t);
      first = false;
    }
    summary += ").";
  }
  doc.blocks.push_back(paragraph(summary));
  std::vector<ir::TableRow> facts;
  facts.push_back(row_of({"Measure", "Value"}, true));
  const BoundingBox box = bounding_box(geo);
  if (!box.empty) {
    facts.push_back(row_of({"Extent (west, south, east, north)",
                            std::format("{}, {}, {}, {}", format_number(box.min_lon, 6), format_number(box.min_lat, 6),
                                        format_number(box.max_lon, 6), format_number(box.max_lat, 6))}));
  }
  if (box.min_ele) {
    facts.push_back(row_of({"Elevation range", format_number(*box.min_ele, 1) + " – " + format_number(*box.max_ele, 1) + " m"}));
  }
  facts.push_back(row_of({"Coordinate system", geo.crs.empty() ? std::string("WGS 84 (EPSG:4326)") : geo.crs}));
  if (length > 0) facts.push_back(row_of({"Total length of lines and outlines", distance_text(length)}));
  if (area > 0) facts.push_back(row_of({"Total area of polygons", area_text(area)}));
  doc.blocks.push_back(table(std::move(facts)));
  // Track statistics.
  for (const auto& f : geo.features) {
    if (!has_times(f.geometry) && f.role != ir::FeatureRole::Track) continue;
    const TrackStats s = track_stats(f.geometry, 2.0);
    if (s.points < 2) continue;
    const std::string name = feature_name(f).empty() ? std::string("Track") : feature_name(f);
    doc.blocks.push_back(heading(2, name));
    std::vector<ir::TableRow> rows;
    rows.push_back(row_of({"Statistic", "Value"}, true));
    rows.push_back(row_of({"Points / segments", std::format("{} / {}", s.points, s.segments)}));
    rows.push_back(row_of({"Distance", distance_text(s.distance_m)}));
    if (s.start_ms) rows.push_back(row_of({"Start", format_time(*s.start_ms)}));
    if (s.end_ms) rows.push_back(row_of({"End", format_time(*s.end_ms)}));
    if (s.duration_s) rows.push_back(row_of({"Duration", duration_text(*s.duration_s)}));
    if (s.moving_time_s) rows.push_back(row_of({"Recording time", duration_text(*s.moving_time_s)}));
    if (s.average_speed_mps) {
      rows.push_back(row_of({"Average speed", format_number(*s.average_speed_mps * 3.6, 2) + " km/h"}));
    }
    if (s.max_speed_mps) rows.push_back(row_of({"Maximum speed", format_number(*s.max_speed_mps * 3.6, 2) + " km/h"}));
    if (s.min_ele) {
      rows.push_back(row_of({"Elevation gain / loss", format_number(s.elevation_gain_m, 1) + " m / " +
                                                          format_number(s.elevation_loss_m, 1) + " m"}));
    }
    if (s.average_heart_rate) {
      rows.push_back(row_of({"Heart rate (average / maximum)",
                             format_number(*s.average_heart_rate, 0) + " / " + format_number(*s.max_heart_rate, 0) + " bpm"}));
    }
    if (s.average_cadence) rows.push_back(row_of({"Average cadence", format_number(*s.average_cadence, 0)}));
    if (s.average_power) rows.push_back(row_of({"Average power", format_number(*s.average_power, 0) + " W"}));
    doc.blocks.push_back(table(std::move(rows)));
  }
  if (ctx.options.get_bool("include_map", true) && !geo.features.empty()) {
    RenderOptions r;
    r.width = 800;
    auto image = render_map(geo, r, ctx);
    if (image) doc.blocks.push_back(image_block(std::move(*image), "Map of the features", "Map"));
  }
  // Properties table (bounded so that a huge file still gives a readable report).
  constexpr std::size_t max_rows = 500, max_columns = 12;
  std::vector<std::string> keys;
  for (const auto& f : geo.features) {
    for (const auto& m : f.properties.items()) {
      const std::string k(m.key());
      if (std::find(keys.begin(), keys.end(), k) == keys.end() && keys.size() < max_columns) keys.push_back(k);
    }
  }
  if (!geo.features.empty()) {
    doc.blocks.push_back(heading(2, "Features"));
    std::vector<ir::TableRow> rows;
    std::vector<std::string> head = {"#", "Type"};
    for (const auto& k : keys) head.push_back(k);
    rows.push_back(row_of(head, true));
    for (std::size_t i = 0; i < geo.features.size() && i < max_rows; ++i) {
      const auto& f = geo.features[i];
      std::vector<std::string> cells = {std::to_string(i + 1), std::string(geometry_type_name(f.geometry))};
      for (const auto& k : keys) {
        std::string text = json_to_text(f.properties[k]);
        if (text.size() > 200) text = text.substr(0, 200) + "…";
        cells.push_back(std::move(text));
      }
      rows.push_back(row_of(std::move(cells)));
    }
    doc.blocks.push_back(table(std::move(rows)));
    if (geo.features.size() > max_rows) {
      doc.blocks.push_back(paragraph(std::format("Only the first {} of {} features are listed.", max_rows, geo.features.size())));
    }
  }
  return AnyIR(std::move(doc));
}

std::vector<OptionSpec> with_common(std::vector<OptionSpec> specific, bool crs) {
  GeoWriterTraits traits = bridge_traits;
  traits.crs_capable = crs;
  std::vector<OptionSpec> specs = common_write_options(traits);
  // The bridges do not offer the encoder-level quality knobs twice; keep only the geometry ones.
  std::erase_if(specs, [](const OptionSpec& s) { return s.key == "quality" || s.key == "preset"; });
  for (auto& s : specific) specs.push_back(std::move(s));
  return specs;
}

}  // namespace

void register_bridges(Registry& registry) {
  Bridge data;
  data.from = Kind::Geo;
  data.to = Kind::Data;
  data.cost = 10;
  data.label = "geographic features as GeoJSON-shaped data";
  data.fn = geo_to_data;
  data.options = with_common({}, true);
  registry.add_bridge(std::move(data));

  Bridge table;
  table.from = Kind::Geo;
  table.to = Kind::Table;
  table.cost = 15;
  table.lossy = true;
  table.label = "features or track points as table rows";
  table.fn = geo_to_table;
  table.options = with_common(
      {OptionSpec::choice("table_rows", "Rows", "auto", {"auto", "features", "points"},
                          "One row per feature with its attributes, or one row per point with time and sensor data "
                          "(auto: points for GPS tracks)."),
       OptionSpec::choice("geometry_columns", "Geometry columns", "auto", {"auto", "wkt", "latlon", "both"},
                          "How feature rows hold the geometry: latitude/longitude (points), WKT, or both.")},
      true);
  registry.add_bridge(std::move(table));

  Bridge document;
  document.from = Kind::Geo;
  document.to = Kind::Document;
  document.cost = 25;
  document.lossy = true;
  document.label = "summary report of geographic features";
  document.fn = geo_to_document;
  document.options = with_common({OptionSpec::boolean("include_map", "Include a map", true,
                                                      "Add a sketch map of the features to the report.")},
                                 false);
  registry.add_bridge(std::move(document));

  Bridge image;
  image.from = Kind::Geo;
  image.to = Kind::Image;
  image.cost = 30;
  image.lossy = true;
  image.label = "map sketch of geographic features";
  image.fn = geo_to_image;
  image.options = with_common(
      {OptionSpec::integer("map_width", "Map width", 1024, 16, 8192, "Width of the map picture in pixels."),
       OptionSpec::integer("map_height", "Map height", 0, 0, 8192, "Height in pixels; 0 follows the shape of the data."),
       OptionSpec::text("map_background", "Background colour", "#f7f6f2", "Colour behind the map, as #rrggbb."),
       OptionSpec::text("map_line_color", "Line colour", "#2b6cb0", "Colour of lines and outlines (features may set their own)."),
       OptionSpec::text("map_fill_color", "Fill colour", "#2b6cb040", "Colour of areas, as #rrggbb or #rrggbbaa."),
       OptionSpec::text("map_point_color", "Point colour", "#c53030", "Colour of point markers."),
       OptionSpec::number("map_line_width", "Line width", 2.5, 0.5, 50, "Width of lines in pixels."),
       OptionSpec::boolean("map_graticule", "Graticule", true, "Draw latitude / longitude lines behind the features.")},
      false);
  registry.add_bridge(std::move(image));
}

}  // namespace cvt::geo
