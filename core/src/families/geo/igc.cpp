// Convertible core - geo family: IGC flight recorder files (FAI glider logs).
//
// Reading: the date (HFDTE, both the old "HFDTEddmmyy" and the 2016
// "HFDTEDATE:ddmmyy,nn" forms), header fields (pilot, glider, competition
// id, recorder: Geo::metadata, dropped by strip_metadata), I-record
// extension columns (FXA, SIU, ENL, HRT -> heart rate, ...) and B-record
// fixes with GNSS / pressure altitude and midnight roll-over. Writing:
// a neutral A record, the date and one B record per timed point.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
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

std::optional<int> digits(std::string_view s) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string_view::npos) return std::nullopt;
  const auto v = util::parse_int(s);
  if (!v) return std::nullopt;
  return static_cast<int>(*v);
}

struct Extension {
  std::size_t start = 0;  // 0-based
  std::size_t length = 0;
  std::string code;
};

// "DDMMmmm" + N/S or "DDDMMmmm" + E/W.
std::optional<double> igc_angle(std::string_view text, int degree_digits, char hemisphere) {
  const auto deg = digits(text.substr(0, static_cast<std::size_t>(degree_digits)));
  const auto min = digits(text.substr(static_cast<std::size_t>(degree_digits), 5));
  if (!deg || !min) return std::nullopt;
  double v = *deg + *min / 60000.0;
  if (hemisphere == 'S' || hemisphere == 'W') v = -v;
  else if (hemisphere != 'N' && hemisphere != 'E') return std::nullopt;
  return v;
}

Result<ir::Geo> read_igc(ReadContext& ctx) {
  const std::string text = input_text(ctx.input);
  ir::Geo geo;
  std::optional<std::int64_t> day_ms;
  std::vector<Extension> extensions;
  ir::LineString line;
  std::int64_t last_seconds = -1;
  std::int64_t day_offset = 0;
  bool missing_date_warned = false;
  std::size_t index = 0;
  const auto lines = util::split_lines(text);
  for (std::string_view raw : lines) {
    if ((++index & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(lines.size())));
    const std::string_view l = util::trim_right(raw);
    if (l.empty()) continue;
    const char type = l[0];
    if (type == 'A') {
      geo.metadata.set("recorder", std::string(l.substr(1)));
    } else if (type == 'H') {
      if (l.size() >= 5 && l.substr(2, 3) == "DTE") {
        std::string_view value = l.substr(5);
        if (value.starts_with("DATE:")) value.remove_prefix(5);
        if (value.size() < 6) continue;
        const auto d = digits(value.substr(0, 2));
        const auto m = digits(value.substr(2, 2));
        const auto y = digits(value.substr(4, 2));
        if (d && m && y && *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31) {
          const int year = *y < 80 ? 2000 + *y : 1900 + *y;
          day_ms = days_from_civil(year, static_cast<unsigned>(*m), static_cast<unsigned>(*d)) * 86'400'000;
        }
      } else if (l.size() > 5) {
        const std::size_t colon = l.find(':');
        const std::string_view key = l.substr(2, (colon == std::string_view::npos ? l.size() : colon) - 2);
        const std::string_view value = colon == std::string_view::npos ? std::string_view() : util::trim(l.substr(colon + 1));
        if (!value.empty() && geo.metadata.size() < 64) {
          geo.metadata.set(util::to_lower(util::trim(key)), util::sanitize_utf8(value));
        }
      }
    } else if (type == 'I' && l.size() >= 3) {
      const auto n = digits(l.substr(1, 2));
      if (!n) continue;
      extensions.clear();
      for (int i = 0; i < *n && 3 + static_cast<std::size_t>(i) * 7 + 7 <= l.size(); ++i) {
        const std::string_view spec = l.substr(3 + static_cast<std::size_t>(i) * 7, 7);
        const auto start = digits(spec.substr(0, 2));
        const auto finish = digits(spec.substr(2, 2));
        if (!start || !finish || *start < 1 || *finish < *start) continue;
        extensions.push_back({static_cast<std::size_t>(*start - 1), static_cast<std::size_t>(*finish - *start + 1),
                              std::string(spec.substr(4, 3))});
      }
    } else if (type == 'B' && l.size() >= 35) {
      const auto hh = digits(l.substr(1, 2));
      const auto mm = digits(l.substr(3, 2));
      const auto ss = digits(l.substr(5, 2));
      const auto lat = igc_angle(l.substr(7, 7), 2, l[14]);
      const auto lon = igc_angle(l.substr(15, 8), 3, l[23]);
      if (!hh || !mm || !ss || !lat || !lon) {
        return fail(Status::Decode, "line {} is not a valid IGC fix (B record)", index);
      }
      const std::int64_t seconds = *hh * 3600 + *mm * 60 + *ss;
      if (last_seconds >= 0 && seconds < last_seconds - 3600) day_offset += 86'400'000;  // past midnight
      last_seconds = seconds;
      if (!day_ms && !missing_date_warned) {
        ctx.warn("The IGC file has no date (HFDTE); fixes were dated 1970-01-01.");
        missing_date_warned = true;
      }
      ir::Coord c;
      c.lat = *lat;
      c.lon = *lon;
      c.time_ms = day_ms.value_or(0) + day_offset + seconds * 1000;
      const auto parse_alt = [&](std::string_view s) -> std::optional<double> {
        const std::string_view t = util::trim(s);
        if (t.empty()) return std::nullopt;
        if (auto v = util::parse_int(t)) return static_cast<double>(*v);
        return std::nullopt;
      };
      const auto pressure = parse_alt(l.substr(25, 5));
      const auto gnss = parse_alt(l.substr(30, 5));
      if (gnss && (*gnss != 0 || !pressure || *pressure == 0)) {
        c.ele = *gnss;
      } else if (pressure) {
        c.ele = *pressure;
      }
      if (pressure && *pressure != 0) c.extra.emplace_back("pressure_altitude", *pressure);
      for (const Extension& e : extensions) {
        if (e.start + e.length > l.size()) continue;
        const auto v = parse_number(l.substr(e.start, e.length));
        if (!v) continue;
        if (e.code == "HRT") c.heart_rate = *v;
        else if (c.extra.size() < 32) c.extra.emplace_back(e.code, *v);
      }
      line.points.push_back(std::move(c));
    }
  }
  if (line.points.empty()) return fail(Status::Decode, "the IGC file holds no fixes (B records)");
  ir::Feature f;
  f.role = ir::FeatureRole::Track;
  const std::string glider = geo.metadata["gidgliderid"].as_string();
  f.geometry.shape = ir::MultiLineString{{std::move(line)}};
  if (!glider.empty()) f.properties.set("glider_id", glider);
  geo.features.push_back(std::move(f));
  return geo;
}

std::string igc_angle_text(double v, int degree_digits, char positive, char negative) {
  const char hemisphere = v < 0 ? negative : positive;
  const double a = std::fabs(v);
  std::int64_t deg = static_cast<std::int64_t>(std::floor(a));
  std::int64_t thousandths = std::llround((a - static_cast<double>(deg)) * 60000.0);
  if (thousandths >= 60000) {
    ++deg;
    thousandths -= 60000;
  }
  return std::format("{:0{}}{:05}{}", deg, degree_digits, thousandths, hemisphere);
}

std::string igc_altitude(double v) {
  const auto a = std::clamp<std::int64_t>(std::llround(v), -9999, 99999);
  return a < 0 ? std::format("-{:04}", -a) : std::format("{:05}", a);
}

Result<Bytes> write_igc(const ir::Geo& geo, WriteContext& ctx) {
  std::vector<const ir::Coord*> fixes;
  std::size_t untimed = 0;
  for (const ir::Feature& f : geo.features) {
    for_each_coord(f.geometry, [&](const ir::Coord& c) {
      if (c.time_ms) fixes.push_back(&c);
      else ++untimed;
    });
  }
  if (fixes.empty()) return fail(Status::Encode, "IGC fixes need times; the data has none");
  if (untimed > 0) ctx.warn("Points without a time cannot be IGC fixes and were left out.");
  std::string out = "AXXX000\r\n";
  std::int64_t current_day = -1;
  for (const ir::Coord* c : fixes) {
    std::int64_t day = *c->time_ms / 86'400'000;
    std::int64_t ms = *c->time_ms % 86'400'000;
    if (ms < 0) {
      ms += 86'400'000;
      --day;
    }
    if (current_day < 0) {
      const CivilDate d = civil_from_days(day);
      out += std::format("HFDTEDATE:{:02}{:02}{:02},01\r\n", d.day, d.month, d.year % 100);
      current_day = day;
    }
    const std::int64_t s = ms / 1000;
    out += std::format("B{:02}{:02}{:02}", s / 3600, (s / 60) % 60, s % 60);
    out += igc_angle_text(c->lat, 2, 'N', 'S');
    out += igc_angle_text(c->lon, 3, 'E', 'W');
    out += c->ele ? 'A' : 'V';
    out += "00000";
    out += igc_altitude(c->ele.value_or(0.0));
    out += "\r\n";
  }
  return to_bytes(out);
}

int sniff_igc(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 4096);
  if (!head.starts_with('A')) return 0;
  if (head.find("\nHFDTE") != std::string_view::npos || head.find("\r\nHFDTE") != std::string_view::npos) return 90;
  return head.find("\nB") != std::string_view::npos && head.find("\nH") != std::string_view::npos ? 70 : 0;
}

}  // namespace

void register_igc(Registry& registry) {
  FormatInfo info;
  info.id = "igc";
  info.name = "IGC flight log";
  info.description = "FAI / IGC flight recorder files from gliders, paragliders and hang gliders.";
  info.category = Category::Geo;
  info.extensions = {"igc"};
  info.mime = "application/vnd.fai.igc";
  info.popularity = 20;
  info.sniff = sniff_igc;
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "igc";
  spec.read = read_igc;
  spec.write = write_igc;
  spec.lossy_write = true;
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
