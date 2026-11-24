// Convertible core - geo family: CSV / TSV with coordinate columns ("geocsv").
//
// Columns are recognised by their titles: latitude / longitude (lat, lng,
// lon, long, x, y, ...), a WKT or hex-WKB geometry column (WKT, geometry,
// the_geom, ...), and per-point data: elevation, time, heart rate, cadence,
// power, speed, temperature. A file with a time column and plain
// coordinates (a GPS log or a Strava-style export) becomes one track; any
// other file one feature per row with the remaining columns as typed
// properties. The delimiter (comma, semicolon, tab, pipe) is detected.
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
#include "families/geo/wkt.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::geo {

namespace {

// --------------------------------------------------------------------------- CSV text

char detect_delimiter(std::string_view header) {
  std::size_t best_count = 0;
  char best = ',';
  for (char d : {',', ';', '\t', '|'}) {
    std::size_t count = 0;
    bool quoted = false;
    for (char c : header) {
      if (c == '"') quoted = !quoted;
      if (!quoted && c == d) ++count;
    }
    if (count > best_count) {
      best_count = count;
      best = d;
    }
  }
  return best;
}

// RFC 4180 records (quoted fields may hold delimiters, quotes and line breaks).
Result<std::vector<std::vector<std::string>>> parse_csv(std::string_view text, char delimiter, const Context& ctx) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false, field_started = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') {
          field += '"';
          ++i;
        } else {
          quoted = false;
        }
      } else {
        field += c;
      }
      continue;
    }
    if (c == '"' && !field_started) {
      quoted = true;
      field_started = true;
    } else if (c == delimiter) {
      row.push_back(std::move(field));
      field.clear();
      field_started = false;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
      row.push_back(std::move(field));
      field.clear();
      field_started = false;
      if (!(row.size() == 1 && row[0].empty())) rows.push_back(std::move(row));
      row.clear();
      if ((rows.size() & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(text.size())));
    } else {
      field += c;
      field_started = true;
    }
  }
  if (quoted) return fail(Status::Decode, "the CSV file ends inside a quoted field");
  if (field_started || !row.empty()) {
    row.push_back(std::move(field));
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string csv_quote(std::string_view value, char delimiter) {
  if (value.find_first_of(std::string{'"', '\n', '\r', delimiter}) == std::string_view::npos &&
      (value.empty() || (value.front() != ' ' && value.back() != ' '))) {
    return std::string(value);
  }
  std::string out = "\"";
  for (char c : value) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

// --------------------------------------------------------------------------- columns

std::string normalise(std::string_view title) {
  std::string out;
  for (char c : util::trim(title)) {
    if (util::is_ascii_alpha(c) || util::is_ascii_digit(c)) out += util::ascii_lower(c);
  }
  return out;
}

bool one_of(std::string_view name, std::initializer_list<std::string_view> names) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

struct Columns {
  std::optional<std::size_t> lat, lon, geometry, ele, time, hr, cad, power, speed, temp;
};

Columns detect_columns(const std::vector<std::string>& header) {
  Columns c;
  std::optional<std::size_t> x, y;
  for (std::size_t i = 0; i < header.size(); ++i) {
    const std::string n = normalise(header[i]);
    if (!c.lat && one_of(n, {"lat", "latitude", "latdeg", "latitudedeg", "latdd", "decimallatitude", "pointy",
                             "positionlat", "latitudedegrees", "ylat"})) {
      c.lat = i;
    } else if (!c.lon && one_of(n, {"lon", "lng", "long", "longitude", "londeg", "longitudedeg", "londd",
                                    "decimallongitude", "pointx", "positionlong", "longitudedegrees", "xlon"})) {
      c.lon = i;
    } else if (!x && n == "x") {
      x = i;
    } else if (!y && n == "y") {
      y = i;
    } else if (!c.geometry && one_of(n, {"wkt", "geometry", "geom", "thegeom", "wktgeometry", "shape", "wkb",
                                         "geometrywkt", "geomwkt"})) {
      c.geometry = i;
    } else if (!c.ele && one_of(n, {"ele", "elevation", "alt", "altitude", "z", "altitudem", "elevationm",
                                    "enhancedaltitude", "altitudemeters"})) {
      c.ele = i;
    } else if (!c.time && one_of(n, {"time", "timestamp", "datetime", "utc", "timeutc", "recordedat", "date"})) {
      c.time = i;
    } else if (!c.hr && one_of(n, {"hr", "heartrate", "heartratebpm", "bpm", "heart"})) {
      c.hr = i;
    } else if (!c.cad && one_of(n, {"cad", "cadence", "cadencerpm"})) {
      c.cad = i;
    } else if (!c.power && one_of(n, {"power", "watts", "powerwatts"})) {
      c.power = i;
    } else if (!c.speed && one_of(n, {"speed", "enhancedspeed", "speedms", "velocity"})) {
      c.speed = i;
    } else if (!c.temp && one_of(n, {"temp", "temperature", "atemp"})) {
      c.temp = i;
    }
  }
  if (!c.lat && !c.lon && x && y) {
    c.lon = x;
    c.lat = y;
  }
  return c;
}

bool has_coordinates(const Columns& c) { return (c.lat && c.lon) || c.geometry; }

std::string_view cell(const std::vector<std::string>& row, std::optional<std::size_t> index) {
  if (!index || *index >= row.size()) return {};
  return util::trim(row[*index]);
}

std::optional<double> cell_number(const std::vector<std::string>& row, std::optional<std::size_t> index) {
  const std::string_view v = cell(row, index);
  if (v.empty()) return std::nullopt;
  return parse_number(v);
}

Result<std::optional<ir::Geometry>> geometry_cell(std::string_view v) {
  if (v.empty()) return std::optional<ir::Geometry>{};
  if (v.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos && v.size() >= 18) {
    CVT_TRY_ASSIGN(const Bytes bytes, util::hex_decode(v));
    CVT_TRY_ASSIGN(ir::Geometry g, geometry_from_wkb(bytes));
    return std::optional<ir::Geometry>(std::move(g));
  }
  CVT_TRY_ASSIGN(ir::Geometry g, geometry_from_wkt(v));
  return std::optional<ir::Geometry>(std::move(g));
}

void fill_sensors(ir::Coord& c, const std::vector<std::string>& row, const Columns& cols) {
  if (auto v = cell_number(row, cols.ele)) c.ele = *v;
  if (const std::string_view t = cell(row, cols.time); !t.empty()) {
    c.time_ms = parse_time(t);
    if (!c.time_ms) {
      // Unix seconds or milliseconds.
      if (auto n = parse_number(t)) c.time_ms = static_cast<std::int64_t>(*n > 1e11 ? *n : *n * 1000.0);
    }
  }
  if (auto v = cell_number(row, cols.hr)) c.heart_rate = *v;
  if (auto v = cell_number(row, cols.cad)) c.cadence = *v;
  if (auto v = cell_number(row, cols.power)) c.power = *v;
  if (auto v = cell_number(row, cols.speed)) c.speed = *v;
  if (auto v = cell_number(row, cols.temp)) c.temperature = *v;
}

Result<ir::Geo> read_geocsv(ReadContext& ctx) {
  const std::string text = input_text(ctx.input);
  const std::string_view first_line = std::string_view(text).substr(0, text.find_first_of("\r\n"));
  const char delimiter = detect_delimiter(first_line);
  CVT_TRY_ASSIGN(auto rows, parse_csv(text, delimiter, ctx));
  if (rows.empty()) return fail(Status::Decode, "the CSV file is empty");
  const std::vector<std::string> header = rows.front();
  const Columns cols = detect_columns(header);
  if (!has_coordinates(cols)) {
    return fail(Status::Decode, "no coordinate columns (latitude/longitude or WKT) were found in the CSV header");
  }
  const std::string mode = ctx.options.get_enum("csv_rows", "auto", {"auto", "features", "track"});
  const bool track = mode == "track" || (mode == "auto" && cols.time && !cols.geometry && rows.size() > 2);
  ir::Geo geo;
  ir::LineString line;
  std::size_t skipped = 0;
  for (std::size_t r = 1; r < rows.size(); ++r) {
    const auto& row = rows[r];
    std::optional<ir::Geometry> geometry;
    if (cols.geometry) {
      auto parsed = geometry_cell(cell(row, cols.geometry));
      if (!parsed) return fail(Status::Decode, "row {}: {}", r + 1, parsed.error().message);
      geometry = std::move(*parsed);
    }
    if (!geometry && cols.lat && cols.lon) {
      const auto lat = cell_number(row, cols.lat);
      const auto lon = cell_number(row, cols.lon);
      if (lat && lon) {
        ir::Coord c;
        c.lat = *lat;
        c.lon = *lon;
        fill_sensors(c, row, cols);
        if (track) {
          line.points.push_back(std::move(c));
          continue;
        }
        geometry = ir::Geometry{ir::Point{std::move(c)}};
      }
    }
    if (track) {
      ++skipped;
      continue;
    }
    ir::Feature f;
    f.geometry = geometry ? std::move(*geometry) : ir::Geometry{ir::GeometryCollection{}};
    for (std::size_t i = 0; i < header.size() && i < row.size(); ++i) {
      if (i == cols.lat || i == cols.lon || i == cols.geometry) continue;
      if (row[i].empty()) {
        f.properties.set(header[i], Json());
        continue;
      }
      f.properties.set(header[i], text_to_json(row[i]));
    }
    if (const Json* name = f.properties.find("name"); name != nullptr && name->is_string()) f.name = name->as_string();
    geo.features.push_back(std::move(f));
  }
  if (track) {
    if (skipped > 0) ctx.warn("Rows without coordinates were skipped.");
    ir::Feature f;
    f.role = ir::FeatureRole::Track;
    f.geometry.shape = ir::MultiLineString{{std::move(line)}};
    geo.features.push_back(std::move(f));
  }
  return geo;
}

// --------------------------------------------------------------------------- writing

char delimiter_from(const Options& options) {
  const std::string d = options.get_enum("csv_delimiter", "comma", {"comma", "semicolon", "tab"});
  return d == "semicolon" ? ';' : d == "tab" ? '\t' : ',';
}

Result<Bytes> write_geocsv(const ir::Geo& geo, WriteContext& ctx) {
  const char delimiter = delimiter_from(ctx.options);
  const std::string rows_mode = ctx.options.get_enum("rows", "features", {"features", "points"});
  std::string out;
  const auto line = [&](const std::vector<std::string>& cells) {
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (i) out += delimiter;
      out += csv_quote(cells[i], delimiter);
    }
    out += "\r\n";
  };
  if (rows_mode == "points") {
    // One row per coordinate, with the per-point data that exists.
    bool ele = false, time = false, hr = false, cad = false, power = false, speed = false, temp = false;
    for (const auto& f : geo.features) {
      for_each_coord(f.geometry, [&](const ir::Coord& c) {
        ele = ele || c.ele;
        time = time || c.time_ms;
        hr = hr || c.heart_rate;
        cad = cad || c.cadence;
        power = power || c.power;
        speed = speed || c.speed;
        temp = temp || c.temperature;
      });
    }
    std::vector<std::string> header = {"feature", "part", "latitude", "longitude"};
    if (ele) header.emplace_back("elevation");
    if (time) header.emplace_back("time");
    if (hr) header.emplace_back("heart_rate");
    if (cad) header.emplace_back("cadence");
    if (power) header.emplace_back("power");
    if (speed) header.emplace_back("speed");
    if (temp) header.emplace_back("temperature");
    line(header);
    const auto opt = [](const std::optional<double>& v) { return v ? format_number(*v) : std::string(); };
    for (std::size_t fi = 0; fi < geo.features.size(); ++fi) {
      const ir::Feature& f = geo.features[fi];
      const std::string name = feature_name(f).empty() ? std::to_string(fi + 1) : feature_name(f);
      // Parts: each line / ring / segment gets its own number.
      std::size_t part = 0;
      std::vector<std::vector<std::string>> rows;
      const auto emit_seq = [&](const std::vector<ir::Coord>& pts) {
        ++part;
        for (const ir::Coord& c : pts) {
          std::vector<std::string> cells = {name, std::to_string(part), format_number(c.lat), format_number(c.lon)};
          if (ele) cells.push_back(opt(c.ele));
          if (time) cells.push_back(c.time_ms ? format_time(*c.time_ms) : std::string());
          if (hr) cells.push_back(opt(c.heart_rate));
          if (cad) cells.push_back(opt(c.cadence));
          if (power) cells.push_back(opt(c.power));
          if (speed) cells.push_back(opt(c.speed));
          if (temp) cells.push_back(opt(c.temperature));
          line(cells);
        }
      };
      std::visit(
          [&](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, ir::Point>) emit_seq({s.coord});
            else if constexpr (std::is_same_v<T, ir::LineString> || std::is_same_v<T, ir::MultiPoint>) emit_seq(s.points);
            else if constexpr (std::is_same_v<T, ir::Polygon>) {
              for (const auto& r : s.rings) emit_seq(r);
            } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
              for (const auto& l : s.lines) emit_seq(l.points);
            } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
              for (const auto& p : s.polygons) {
                for (const auto& r : p.rings) emit_seq(r);
              }
            } else {
              std::vector<ir::Coord> all;
              for_each_coord(ir::Geometry{s}, [&](const ir::Coord& c) { all.push_back(c); });
              emit_seq(all);
            }
          },
          f.geometry.shape);
    }
    return to_bytes(out);
  }
  // One row per feature.
  std::string geometry_mode = ctx.options.get_enum("geometry_columns", "auto", {"auto", "wkt", "latlon", "both"});
  const bool all_points = std::all_of(geo.features.begin(), geo.features.end(), [](const ir::Feature& f) {
    return std::holds_alternative<ir::Point>(f.geometry.shape);
  });
  if (geometry_mode == "auto") geometry_mode = all_points ? "latlon" : "wkt";
  const bool wkt = geometry_mode == "wkt" || geometry_mode == "both";
  const bool latlon = geometry_mode == "latlon" || geometry_mode == "both";
  std::vector<std::string> keys;
  for (const auto& f : geo.features) {
    if (!feature_name(f).empty() && std::find(keys.begin(), keys.end(), "name") == keys.end()) keys.insert(keys.begin(), "name");
    for (const auto& m : f.properties.items()) {
      const std::string k(m.key());
      if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    }
  }
  std::vector<std::string> header;
  if (wkt) header.emplace_back("WKT");
  if (latlon) {
    header.emplace_back("latitude");
    header.emplace_back("longitude");
  }
  bool any_ele = false;
  for (const auto& f : geo.features) {
    if (const auto* p = std::get_if<ir::Point>(&f.geometry.shape)) any_ele = any_ele || p->coord.ele;
  }
  if (latlon && any_ele) header.emplace_back("elevation");
  for (const auto& k : keys) header.push_back(k);
  line(header);
  bool non_points = false;
  for (const auto& f : geo.features) {
    std::vector<std::string> cells;
    if (wkt) cells.push_back(geometry_to_wkt(f.geometry));
    if (latlon) {
      if (const auto* p = std::get_if<ir::Point>(&f.geometry.shape)) {
        cells.push_back(format_number(p->coord.lat));
        cells.push_back(format_number(p->coord.lon));
        if (any_ele) cells.push_back(p->coord.ele ? format_number(*p->coord.ele) : std::string());
      } else {
        non_points = true;
        cells.emplace_back();
        cells.emplace_back();
        if (any_ele) cells.emplace_back();
      }
    }
    for (const auto& k : keys) {
      if (k == "name" && !f.properties.contains("name")) {
        cells.push_back(feature_name(f));
      } else {
        cells.push_back(json_to_text(f.properties[k]));
      }
    }
    line(cells);
  }
  if (non_points && !wkt) ctx.warn("Lines and areas have no latitude/longitude columns; choose WKT geometry columns to keep them.");
  return to_bytes(out);
}

int sniff_geocsv(ByteView data, std::string_view filename) {
  const std::string_view head = text_head(data, 8192);
  const std::string_view first = head.substr(0, head.find_first_of("\r\n"));
  if (first.empty() || first.find('<') != std::string_view::npos || first.starts_with('{')) return 0;
  const char delimiter = detect_delimiter(first);
  std::vector<std::string> header;
  for (std::string_view part : util::split(first, delimiter)) {
    std::string_view t = util::trim(part);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
    header.emplace_back(t);
  }
  if (header.size() < 2) return 0;
  const Columns cols = detect_columns(header);
  if (!has_coordinates(cols)) return 0;
  return has_extension(filename, "geocsv") ? 95 : 86;
}

}  // namespace

void register_geocsv(Registry& registry) {
  FormatInfo info;
  info.id = "geocsv";
  info.name = "CSV with coordinates";
  info.description = "Comma- or tab-separated values with latitude/longitude or WKT geometry columns.";
  info.category = Category::Geo;
  info.extensions = {"geocsv", "csv", "tsv"};
  info.mime = "text/csv";
  info.popularity = 45;
  info.sniff = sniff_geocsv;
  registry.add_format(std::move(info));

  GeoCodecSpec spec;
  spec.format_id = "geocsv";
  spec.read = read_geocsv;
  spec.write = write_geocsv;
  spec.traits = GeoWriterTraits{false, true, true, true};
  spec.read_options = {OptionSpec::choice("csv_rows", "Rows are", "auto", {"auto", "features", "track"},
                                          "Read every row as its own feature, or all rows as the points of one track "
                                          "(auto: a track when there is a time column).")
                           .advanced()};
  spec.write_options = {
      OptionSpec::choice("geometry_columns", "Geometry columns", "auto", {"auto", "wkt", "latlon", "both"},
                         "Latitude/longitude columns (points), a WKT column (any geometry), or both; auto picks "
                         "latitude/longitude when every feature is a point."),
      OptionSpec::choice("rows", "Rows", "features", {"features", "points"},
                         "One row per feature with its attributes, or one row per point with time and sensor data."),
      OptionSpec::choice("csv_delimiter", "Delimiter", "comma", {"comma", "semicolon", "tab"},
                         "Character between the values.")
          .advanced(),
  };
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
