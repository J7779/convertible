// Convertible core - geo family: Google encoded polylines (precision 5 and 6).
//
// One encoded polyline per line of text, each read as a LineString. The
// writer encodes every point sequence (lines, track segments, polygon
// rings; points are joined into one line per MultiPoint) on its own line.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstddef>
#include <cstdint>
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

Result<std::vector<ir::Coord>> decode_polyline(std::string_view text, double factor) {
  std::vector<ir::Coord> out;
  std::int64_t lat = 0, lon = 0;
  std::size_t i = 0;
  const auto next = [&](std::int64_t& value) -> Result<void> {
    std::int64_t result = 0;
    int shift = 0;
    while (true) {
      if (i >= text.size()) return fail(Status::Decode, "the encoded polyline is truncated");
      const int c = static_cast<unsigned char>(text[i++]);
      if (c < 63 || c > 126) return fail(Status::Decode, "invalid character in an encoded polyline");
      const int b = c - 63;
      if (shift > 60) return fail(Status::Decode, "an encoded polyline value is too long");
      result |= static_cast<std::int64_t>(b & 0x1f) << shift;
      shift += 5;
      if (b < 0x20) break;
    }
    const std::int64_t delta = (result & 1) ? ~(result >> 1) : (result >> 1);
    value += delta;
    return {};
  };
  while (i < text.size()) {
    CVT_TRY(next(lat));
    CVT_TRY(next(lon));
    ir::Coord c;
    c.lat = static_cast<double>(lat) / factor;
    c.lon = static_cast<double>(lon) / factor;
    if (std::fabs(c.lat) > 90.0 || std::fabs(c.lon) > 180.0 * 2) {
      return fail(Status::Decode, "an encoded polyline leaves the range of latitudes and longitudes");
    }
    out.push_back(c);
  }
  return out;
}

void encode_value(std::int64_t value, std::string& out) {
  std::uint64_t v = value < 0 ? ~(static_cast<std::uint64_t>(value) << 1) : static_cast<std::uint64_t>(value) << 1;
  while (v >= 0x20) {
    out += static_cast<char>((0x20 | (v & 0x1f)) + 63);
    v >>= 5;
  }
  out += static_cast<char>(v + 63);
}

std::string encode_polyline(const std::vector<ir::Coord>& points, double factor) {
  std::string out;
  std::int64_t lat = 0, lon = 0;
  for (const ir::Coord& c : points) {
    const auto la = static_cast<std::int64_t>(std::llround(c.lat * factor));
    const auto lo = static_cast<std::int64_t>(std::llround(c.lon * factor));
    encode_value(la - lat, out);
    encode_value(lo - lon, out);
    lat = la;
    lon = lo;
  }
  return out;
}

double factor_for(std::string_view format_id) { return format_id == "polyline6" ? 1e6 : 1e5; }

Result<ir::Geo> read_polyline(ReadContext& ctx) {
  const std::string text = input_text(ctx.input);
  ir::Geo geo;
  for (std::string_view line : util::split_lines(text)) {
    line = util::trim(line);
    if (line.empty()) continue;
    CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, decode_polyline(line, factor_for(ctx.format_id)));
    ir::Feature f;
    f.geometry.shape = ir::LineString{std::move(pts)};
    geo.features.push_back(std::move(f));
  }
  if (geo.features.empty()) return fail(Status::Decode, "the file holds no encoded polyline");
  return geo;
}

void sequences_of(const ir::Geometry& g, std::vector<const std::vector<ir::Coord>*>& out,
                  std::vector<std::vector<ir::Coord>>& owned, int depth = 0) {
  if (depth > ir::max_geometry_depth) return;
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ir::Point>) {
          owned.push_back({s.coord});
        } else if constexpr (std::is_same_v<T, ir::LineString> || std::is_same_v<T, ir::MultiPoint>) {
          out.push_back(&s.points);
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          for (const auto& r : s.rings) out.push_back(&r);
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          for (const auto& l : s.lines) out.push_back(&l.points);
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          for (const auto& p : s.polygons) {
            for (const auto& r : p.rings) out.push_back(&r);
          }
        } else {
          for (const auto& inner : s.geometries) sequences_of(inner, out, owned, depth + 1);
        }
      },
      g.shape);
}

Result<Bytes> write_polyline(const ir::Geo& geo, WriteContext& ctx) {
  const double factor = factor_for(ctx.format_id);
  std::string out;
  bool attributes = false;
  for (const ir::Feature& f : geo.features) {
    attributes = attributes || !f.properties.empty();
    std::vector<const std::vector<ir::Coord>*> seqs;
    std::vector<std::vector<ir::Coord>> owned;
    sequences_of(f.geometry, seqs, owned);
    for (const auto& o : owned) seqs.push_back(&o);
    for (const auto* s : seqs) {
      if (s->empty()) continue;
      out += encode_polyline(*s, factor);
      out += '\n';
    }
  }
  if (attributes) ctx.warn("Encoded polylines hold coordinates only; feature attributes were not written.");
  return to_bytes(out);
}

bool plausible(ByteView data, double factor) {
  const std::string_view head = text_head(data, 8192);
  const std::size_t eol = head.find_first_of("\r\n");
  const std::string_view line = util::trim(head.substr(0, eol));
  if (line.size() < 4) return false;
  for (char c : line) {
    if (c < 63 || c > 126) return false;
  }
  return decode_polyline(line, factor).has_value();
}

int sniff_polyline(ByteView data, std::string_view filename) {
  if (has_extension(filename, "polyline6")) return 0;
  return plausible(data, 1e5) ? 55 : 0;
}

int sniff_polyline6(ByteView data, std::string_view filename) {
  return has_extension(filename, "polyline6") && plausible(data, 1e6) ? 70 : 0;
}

}  // namespace

void register_polyline(Registry& registry) {
  for (const bool six : {false, true}) {
    FormatInfo info;
    info.id = six ? "polyline6" : "polyline";
    info.name = six ? "Encoded polyline (precision 6)" : "Encoded polyline";
    info.description = six ? "Encoded polylines with six decimals (OSRM, Valhalla), one per line."
                           : "Google's encoded polyline algorithm format (five decimals), one per line.";
    info.category = Category::Geo;
    info.extensions = {six ? "polyline6" : "polyline"};
    info.mime = "text/plain";
    info.popularity = six ? 12 : 20;
    info.sniff = six ? sniff_polyline6 : sniff_polyline;
    registry.add_format(std::move(info));
    GeoCodecSpec spec;
    spec.format_id = six ? "polyline6" : "polyline";
    spec.read = read_polyline;
    spec.write = write_polyline;
    spec.lossy_write = true;
    add_geo_codec(registry, std::move(spec));
  }
}

}  // namespace cvt::geo
