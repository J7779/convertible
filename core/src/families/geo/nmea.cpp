// Convertible core - geo family: NMEA 0183 logs.
//
// Reading: GGA (position, altitude, fix quality, satellites, HDOP), RMC
// (position, date, speed), GLL, VTG (speed), GSA (dilution of precision)
// and WPL (waypoints) from any talker (GP, GN, GL, GA, GB, ...). Sentences
// of the same time are merged into one point; the date comes from RMC and
// time-of-day roll-over past midnight is handled. Sentences with a wrong
// checksum are skipped with a note. Writing: GGA + RMC per point and WPL
// per waypoint, with checksums.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "families/geo/geo.hpp"
#include "util/text.hpp"

namespace cvt::geo {

namespace {

constexpr double knots = 1852.0 / 3600.0;

std::uint8_t checksum(std::string_view body) {
  std::uint8_t sum = 0;
  for (char c : body) sum ^= static_cast<std::uint8_t>(c);
  return sum;
}

// "ddmm.mmmm" / "dddmm.mmmm" with a hemisphere letter.
std::optional<double> nmea_angle(std::string_view value, std::string_view hemisphere) {
  const auto v = parse_number(value);
  if (!v || value.empty() || hemisphere.empty()) return std::nullopt;
  const double deg = std::floor(*v / 100.0);
  double out = deg + (*v - deg * 100.0) / 60.0;
  const char h = hemisphere.front();
  if (h == 'S' || h == 'W') out = -out;
  else if (h != 'N' && h != 'E') return std::nullopt;
  return out;
}

// "hhmmss.sss" -> milliseconds of the day.
std::optional<std::int64_t> nmea_time(std::string_view value) {
  if (value.size() < 6) return std::nullopt;
  const auto h = util::parse_int(value.substr(0, 2));
  const auto m = util::parse_int(value.substr(2, 2));
  const auto s = parse_number(value.substr(4));
  if (!h || !m || !s || *h > 23 || *m > 59 || *s >= 61) return std::nullopt;
  return (*h * 3600 + *m * 60) * 1000 + static_cast<std::int64_t>(std::llround(*s * 1000));
}

std::optional<std::int64_t> nmea_date(std::string_view value) {
  if (value.size() != 6) return std::nullopt;
  const auto d = util::parse_int(value.substr(0, 2));
  const auto m = util::parse_int(value.substr(2, 2));
  const auto y = util::parse_int(value.substr(4, 2));
  if (!d || !m || !y || *m < 1 || *m > 12 || *d < 1 || *d > 31) return std::nullopt;
  const std::int64_t year = *y < 80 ? 2000 + *y : 1900 + *y;
  return days_from_civil(year, static_cast<unsigned>(*m), static_cast<unsigned>(*d)) * 86'400'000;
}

struct Fix {
  std::int64_t time_of_day = -1;
  std::optional<std::int64_t> date;
  ir::Coord coord;
  bool has_position = false;
};

Result<ir::Geo> read_nmea(ReadContext& ctx) {
  const std::string text = input_text(ctx.input);
  ir::Geo geo;
  std::vector<Fix> fixes;
  std::size_t bad = 0, index = 0;
  const auto lines = util::split_lines(text);
  for (std::string_view raw : lines) {
    if ((++index & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(lines.size())));
    std::string_view line = util::trim(raw);
    const std::size_t dollar = line.find_first_of("$!");
    if (dollar == std::string_view::npos) continue;
    line = line.substr(dollar);
    std::string_view body = line.substr(1);
    if (const std::size_t star = body.find('*'); star != std::string_view::npos) {
      const auto expected = util::parse_int(util::trim(body.substr(star + 1, 2)), 16);
      body = body.substr(0, star);
      if (!expected || *expected != checksum(body)) {
        ++bad;
        continue;
      }
    }
    const auto f = util::split(body, ',');
    if (f.empty() || f[0].size() < 5) continue;
    const std::string_view kind = f[0].substr(f[0].size() - 3);
    const auto field = [&](std::size_t i) -> std::string_view { return i < f.size() ? util::trim(f[i]) : std::string_view(); };
    // The fix this sentence belongs to: the last one when the time matches.
    const auto fix_for = [&](std::optional<std::int64_t> t) -> Fix& {
      if (!fixes.empty() && t && fixes.back().time_of_day == *t) return fixes.back();
      if (!fixes.empty() && !t && fixes.back().time_of_day < 0) return fixes.back();
      fixes.emplace_back();
      if (t) fixes.back().time_of_day = *t;
      return fixes.back();
    };
    if (kind == "GGA") {
      const auto t = nmea_time(field(1));
      const auto lat = nmea_angle(field(2), field(3));
      const auto lon = nmea_angle(field(4), field(5));
      if (!lat || !lon) continue;
      Fix& fx = fix_for(t);
      fx.coord.lat = *lat;
      fx.coord.lon = *lon;
      fx.has_position = true;
      if (auto alt = parse_number(field(9)); alt && !field(9).empty()) fx.coord.ele = *alt;
      if (auto sats = parse_number(field(7)); sats && !field(7).empty()) fx.coord.extra.emplace_back("satellites", *sats);
      if (auto hdop = parse_number(field(8)); hdop && !field(8).empty()) fx.coord.extra.emplace_back("hdop", *hdop);
    } else if (kind == "RMC") {
      const auto t = nmea_time(field(1));
      const auto lat = nmea_angle(field(3), field(4));
      const auto lon = nmea_angle(field(5), field(6));
      if (!lat || !lon) continue;
      Fix& fx = fix_for(t);
      fx.coord.lat = *lat;
      fx.coord.lon = *lon;
      fx.has_position = true;
      if (auto speed = parse_number(field(7)); speed && !field(7).empty()) fx.coord.speed = *speed * knots;
      fx.date = nmea_date(field(9));
    } else if (kind == "GLL") {
      const auto t = nmea_time(field(5));
      const auto lat = nmea_angle(field(1), field(2));
      const auto lon = nmea_angle(field(3), field(4));
      if (!lat || !lon) continue;
      Fix& fx = fix_for(t);
      fx.coord.lat = *lat;
      fx.coord.lon = *lon;
      fx.has_position = true;
    } else if (kind == "VTG") {
      if (!fixes.empty()) {
        if (auto kmh = parse_number(field(7)); kmh && !field(7).empty()) fixes.back().coord.speed = *kmh / 3.6;
      }
    } else if (kind == "WPL") {
      const auto lat = nmea_angle(field(1), field(2));
      const auto lon = nmea_angle(field(3), field(4));
      if (!lat || !lon) continue;
      ir::Feature w;
      w.role = ir::FeatureRole::Waypoint;
      ir::Coord c;
      c.lon = *lon;
      c.lat = *lat;
      w.geometry.shape = ir::Point{c};
      w.name = std::string(field(5));
      if (!w.name.empty()) w.properties.set("name", w.name);
      geo.features.push_back(std::move(w));
    }
  }
  if (bad > 0) ctx.warn(std::format("{} NMEA sentence(s) with a wrong checksum were skipped.", bad));
  // Dates: from RMC; fixes before the first dated one take its date.
  std::optional<std::int64_t> date;
  for (const Fix& fx : fixes) {
    if (fx.date) {
      date = fx.date;
      break;
    }
  }
  ir::LineString line;
  std::int64_t last_tod = -1;
  for (Fix& fx : fixes) {
    if (!fx.has_position) continue;
    if (fx.date) date = fx.date;
    if (fx.time_of_day >= 0) {
      if (last_tod >= 0 && fx.time_of_day < last_tod - 3'600'000 && !fx.date && date) *date += 86'400'000;
      last_tod = fx.time_of_day;
      if (date) fx.coord.time_ms = *date + fx.time_of_day;
    }
    line.points.push_back(std::move(fx.coord));
  }
  if (!line.points.empty()) {
    ir::Feature f;
    f.role = ir::FeatureRole::Track;
    f.geometry.shape = ir::MultiLineString{{std::move(line)}};
    geo.features.insert(geo.features.begin(), std::move(f));
  }
  if (geo.features.empty()) return fail(Status::Decode, "the NMEA log holds no positions");
  return geo;
}

std::string nmea_angle_text(double v, int degree_digits, char positive, char negative) {
  const double a = std::fabs(v);
  double deg = std::floor(a);
  double minutes = std::round((a - deg) * 60.0 * 10000.0) / 10000.0;
  if (minutes >= 60.0) {
    deg += 1;
    minutes -= 60.0;
  }
  return std::format("{:0{}}{:07.4f},{}", static_cast<int>(deg), degree_digits, minutes, v < 0 ? negative : positive);
}

std::string sentence(std::string_view body) { return std::format("${}*{:02X}\r\n", body, checksum(body)); }

Result<Bytes> write_nmea(const ir::Geo& geo, WriteContext& ctx) {
  std::string out;
  std::size_t untimed = 0;
  for (const ir::Feature& f : geo.features) {
    if (const auto* p = std::get_if<ir::Point>(&f.geometry.shape)) {
      std::string name;
      for (char c : feature_name(f)) {
        if (c != ',' && c != '*' && c != '$' && static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) < 127) name += c;
      }
      out += sentence(std::format("GPWPL,{},{},{}", nmea_angle_text(p->coord.lat, 2, 'N', 'S'),
                                  nmea_angle_text(p->coord.lon, 3, 'E', 'W'), name));
    }
  }
  for (const ir::Feature& f : geo.features) {
    if (std::holds_alternative<ir::Point>(f.geometry.shape)) continue;
    for_each_coord(f.geometry, [&](const ir::Coord& c) {
      if (!c.time_ms) {
        ++untimed;
        return;
      }
      std::string time, date;
      if (c.time_ms) {
        std::int64_t day = *c.time_ms / 86'400'000, ms = *c.time_ms % 86'400'000;
        if (ms < 0) {
          ms += 86'400'000;
          --day;
        }
        const CivilDate d = civil_from_days(day);
        time = std::format("{:02}{:02}{:02}.{:03}", ms / 3'600'000, (ms / 60'000) % 60, (ms / 1000) % 60, ms % 1000);
        date = std::format("{:02}{:02}{:02}", d.day, d.month, d.year % 100);
      }
      const std::string lat = nmea_angle_text(c.lat, 2, 'N', 'S');
      const std::string lon = nmea_angle_text(c.lon, 3, 'E', 'W');
      const std::string speed = c.speed ? format_number(*c.speed / knots, 2) : std::string("0.00");
      // GGA before RMC, as receivers emit them.
      out += sentence(std::format("GPGGA,{},{},{},1,04,1.0,{},M,,M,,", time, lat, lon,
                                  c.ele ? format_number(*c.ele, 3) : std::string()));
      out += sentence(std::format("GPRMC,{},A,{},{},{},,{},,", time, lat, lon, speed, date));
    });
  }
  if (untimed > 0) ctx.warn("Points without a time cannot be NMEA fixes and were left out.");
  return to_bytes(out);
}

int sniff_nmea(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 2048);
  if (head.size() < 7 || head[0] != '$') return 0;
  const std::string_view kind = head.substr(3, 3);
  if (kind != "GGA" && kind != "RMC" && kind != "GLL" && kind != "GSA" && kind != "GSV" && kind != "VTG" &&
      kind != "WPL" && kind != "ZDA") {
    return 0;
  }
  return head[6] == ',' ? 88 : 0;
}

}  // namespace

void register_nmea(Registry& registry) {
  FormatInfo info;
  info.id = "nmea";
  info.name = "NMEA 0183 log";
  info.description = "GPS receiver sentences (GGA, RMC, GLL, VTG, WPL) as logged by receivers and data loggers.";
  info.category = Category::Geo;
  info.extensions = {"nmea", "nma"};
  info.mime = "text/plain";
  info.popularity = 25;
  info.sniff = sniff_nmea;
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "nmea";
  spec.read = read_nmea;
  spec.write = write_nmea;
  spec.lossy_write = true;
  spec.traits = GeoWriterTraits{false, true, false, false};
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
