// Convertible core - geo family: Geobuf (protocol-buffer encoded GeoJSON,
// geobuf.proto version 1).
//
// Data holds a shared key table, the precision (decimal digits of the
// integer coordinates) and dimensions, and a FeatureCollection, a Feature
// or a Geometry. Coordinates are delta coded per line or ring; polygon
// rings are stored without their closing point; multipolygons describe
// their structure in `lengths`. Property values are per feature. Custom
// (foreign) properties are skipped when reading.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "families/geo/geo.hpp"
#include "families/geo/protobuf.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::geo {

namespace {

enum GeobufType : std::uint32_t { GbPoint, GbMultiPoint, GbLineString, GbMultiLineString, GbPolygon, GbMultiPolygon, GbCollection };

struct Header {
  std::vector<std::string> keys;
  std::uint32_t dims = 2;
  double scale = 1e6;
};

Result<Json> read_value(ByteView data) {
  pbf::Reader r(data);
  std::uint32_t field = 0, wire = 0;
  Json v;
  while (r.next(field, wire)) {
    switch (field) {
      case 1: v = Json(util::sanitize_utf8(as_chars(r.bytes()))); break;
      case 2: v = Json(r.f64()); break;
      case 3: {
        const std::uint64_t n = r.varint();
        v = n <= static_cast<std::uint64_t>(INT64_MAX) ? Json(static_cast<std::int64_t>(n)) : Json(n);
        break;
      }
      case 4: v = Json(-static_cast<std::int64_t>(std::min<std::uint64_t>(r.varint(), INT64_MAX))); break;
      case 5: v = Json(r.varint() != 0); break;
      case 6: {
        auto parsed = util::parse_json(as_chars(r.bytes()));
        v = parsed ? std::move(*parsed) : Json();
        break;
      }
      default: r.skip(wire);
    }
  }
  if (!r.ok) return fail(Status::Decode, "damaged Geobuf value");
  return v;
}

std::vector<ir::Coord> line_part(const std::vector<std::int64_t>& coords, std::size_t start, std::size_t end,
                                 const Header& h, bool closed) {
  std::vector<ir::Coord> pts;
  std::int64_t prev[3] = {0, 0, 0};
  for (std::size_t i = start; i + h.dims <= end; i += h.dims) {
    ir::Coord c;
    for (std::uint32_t j = 0; j < h.dims && j < 3; ++j) prev[j] += coords[i + j];
    c.lon = static_cast<double>(prev[0]) / h.scale;
    c.lat = static_cast<double>(prev[1]) / h.scale;
    if (h.dims >= 3) c.ele = static_cast<double>(prev[2]) / h.scale;
    pts.push_back(c);
  }
  if (closed && !pts.empty()) pts.push_back(pts.front());
  return pts;
}

Result<std::vector<std::vector<ir::Coord>>> multi_line(const std::vector<std::int64_t>& coords,
                                                         const std::vector<std::uint32_t>& lengths, const Header& h,
                                                         bool closed) {
  std::vector<std::vector<ir::Coord>> out;
  if (lengths.empty()) {
    out.push_back(line_part(coords, 0, coords.size(), h, closed));
    return out;
  }
  std::uint64_t end = 0;
  for (std::uint32_t len : lengths) {
    const std::uint64_t start = end;
    end = start + std::uint64_t{len} * h.dims;
    if (end > coords.size()) return fail(Status::Decode, "Geobuf lengths exceed the coordinates");
    out.push_back(line_part(coords, static_cast<std::size_t>(start), static_cast<std::size_t>(end), h, closed));
  }
  return out;
}

Result<ir::Geometry> read_geometry(ByteView data, const Header& h, int depth) {
  if (depth > ir::max_geometry_depth) return fail(Status::Limit, "Geobuf geometry collections are nested too deeply");
  pbf::Reader r(data);
  std::uint32_t field = 0, wire = 0;
  std::uint32_t type = 0;
  std::vector<std::uint32_t> lengths;
  std::vector<std::int64_t> coords;
  std::vector<ByteView> members;
  while (r.next(field, wire)) {
    if (field == 1) type = static_cast<std::uint32_t>(r.varint());
    else if (field == 2) pbf::packed(r, wire, [&](pbf::Reader& x) { lengths.push_back(static_cast<std::uint32_t>(x.varint())); });
    else if (field == 3) pbf::packed(r, wire, [&](pbf::Reader& x) { coords.push_back(x.svarint()); });
    else if (field == 4 && wire == 2) members.push_back(r.bytes());
    else r.skip(wire);
  }
  if (!r.ok) return fail(Status::Decode, "damaged Geobuf geometry");
  ir::Geometry g;
  switch (type) {
    case GbPoint: {
      if (coords.size() < h.dims) return fail(Status::Decode, "a Geobuf point has no coordinates");
      ir::Coord c;
      c.lon = static_cast<double>(coords[0]) / h.scale;
      c.lat = static_cast<double>(coords[1]) / h.scale;
      if (h.dims >= 3) c.ele = static_cast<double>(coords[2]) / h.scale;
      g.shape = ir::Point{c};
      break;
    }
    case GbMultiPoint: g.shape = ir::MultiPoint{line_part(coords, 0, coords.size(), h, false)}; break;
    case GbLineString: g.shape = ir::LineString{line_part(coords, 0, coords.size(), h, false)}; break;
    case GbMultiLineString: {
      CVT_TRY_ASSIGN(auto lines, multi_line(coords, lengths, h, false));
      ir::MultiLineString ml;
      for (auto& l : lines) ml.lines.push_back(ir::LineString{std::move(l)});
      g.shape = std::move(ml);
      break;
    }
    case GbPolygon: {
      CVT_TRY_ASSIGN(auto rings, multi_line(coords, lengths, h, true));
      g.shape = ir::Polygon{std::move(rings)};
      break;
    }
    case GbMultiPolygon: {
      ir::MultiPolygon mp;
      if (lengths.empty()) {
        mp.polygons.push_back(ir::Polygon{{line_part(coords, 0, coords.size(), h, true)}});
      } else {
        std::uint64_t end = 0;
        std::size_t j = 1;
        for (std::uint32_t p = 0; p < lengths[0]; ++p) {
          if (j >= lengths.size()) return fail(Status::Decode, "Geobuf multipolygon lengths are inconsistent");
          ir::Polygon polygon;
          const std::uint32_t rings = lengths[j];
          for (std::uint32_t k = 0; k < rings; ++k) {
            if (j + 1 + k >= lengths.size()) return fail(Status::Decode, "Geobuf multipolygon lengths are inconsistent");
            const std::uint64_t start = end;
            end = start + std::uint64_t{lengths[j + 1 + k]} * h.dims;
            if (end > coords.size()) return fail(Status::Decode, "Geobuf lengths exceed the coordinates");
            polygon.rings.push_back(line_part(coords, static_cast<std::size_t>(start), static_cast<std::size_t>(end), h, true));
          }
          j += std::size_t{rings} + 1;
          mp.polygons.push_back(std::move(polygon));
        }
      }
      g.shape = std::move(mp);
      break;
    }
    case GbCollection: {
      ir::GeometryCollection gc;
      for (ByteView m : members) {
        CVT_TRY_ASSIGN(ir::Geometry inner, read_geometry(m, h, depth + 1));
        gc.geometries.push_back(std::move(inner));
      }
      g.shape = std::move(gc);
      break;
    }
    default: return fail(Status::Decode, "unknown Geobuf geometry type {}", type);
  }
  return g;
}

Result<ir::Feature> read_feature(ByteView data, const Header& h) {
  pbf::Reader r(data);
  std::uint32_t field = 0, wire = 0;
  ir::Feature f;
  bool geometry = false;
  std::vector<Json> values;
  std::vector<std::uint32_t> props;
  while (r.next(field, wire)) {
    if (field == 1 && wire == 2) {
      CVT_TRY_ASSIGN(f.geometry, read_geometry(r.bytes(), h, 0));
      geometry = true;
    } else if (field == 11 && wire == 2) {
      f.id = Json(util::sanitize_utf8(as_chars(r.bytes())));
    } else if (field == 12) {
      f.id = Json(r.svarint());
    } else if (field == 13 && wire == 2) {
      CVT_TRY_ASSIGN(Json v, read_value(r.bytes()));
      values.push_back(std::move(v));
    } else if (field == 14) {
      pbf::packed(r, wire, [&](pbf::Reader& x) { props.push_back(static_cast<std::uint32_t>(x.varint())); });
    } else {
      r.skip(wire);
    }
  }
  if (!r.ok) return fail(Status::Decode, "damaged Geobuf feature");
  if (!geometry) f.geometry.shape = ir::GeometryCollection{};
  for (std::size_t i = 0; i + 1 < props.size(); i += 2) {
    if (props[i] >= h.keys.size() || props[i + 1] >= values.size()) {
      return fail(Status::Decode, "a Geobuf property refers to a missing key or value");
    }
    f.properties.set(h.keys[props[i]], values[props[i + 1]]);
  }
  if (const Json* name = f.properties.find("name"); name != nullptr && name->is_string()) f.name = name->as_string();
  return f;
}

Result<ir::Geo> read_geobuf(ReadContext& ctx) {
  pbf::Reader r(ctx.input);
  Header h;
  std::uint32_t field = 0, wire = 0;
  std::uint32_t precision = 6;
  ir::Geo geo;
  ByteView collection, feature, geometry;
  while (r.next(field, wire)) {
    if (field == 1 && wire == 2) {
      if (h.keys.size() > 1'000'000) return fail(Status::Limit, "too many Geobuf keys");
      h.keys.push_back(util::sanitize_utf8(as_chars(r.bytes())));
    } else if (field == 2 && wire == 0) {
      h.dims = static_cast<std::uint32_t>(r.varint());
    } else if (field == 3 && wire == 0) {
      precision = static_cast<std::uint32_t>(r.varint());
    } else if (field == 4 && wire == 2) {
      collection = r.bytes();
    } else if (field == 5 && wire == 2) {
      feature = r.bytes();
    } else if (field == 6 && wire == 2) {
      geometry = r.bytes();
    } else {
      r.skip(wire);
    }
  }
  if (!r.ok) return fail(Status::Decode, "not a valid Geobuf file");
  if (h.dims < 2 || h.dims > 4 || precision > 15) return fail(Status::Decode, "invalid Geobuf dimensions or precision");
  h.scale = std::pow(10.0, precision);
  if (!collection.empty()) {
    pbf::Reader c(collection);
    std::size_t count = 0;
    while (c.next(field, wire)) {
      if (field == 1 && wire == 2) {
        if ((++count & 1023) == 0) CVT_TRY(ctx.checkpoint(0.5));
        CVT_TRY_ASSIGN(ir::Feature f, read_feature(c.bytes(), h));
        geo.features.push_back(std::move(f));
      } else {
        c.skip(wire);
      }
    }
    if (!c.ok) return fail(Status::Decode, "damaged Geobuf feature collection");
  } else if (!feature.empty()) {
    CVT_TRY_ASSIGN(ir::Feature f, read_feature(feature, h));
    geo.features.push_back(std::move(f));
  } else if (!geometry.empty()) {
    ir::Feature f;
    CVT_TRY_ASSIGN(f.geometry, read_geometry(geometry, h, 0));
    geo.features.push_back(std::move(f));
  } else {
    return fail(Status::Decode, "the Geobuf file holds no data");
  }
  return geo;
}

// --------------------------------------------------------------------------- writing

struct Encoder {
  double scale = 1e6;
  std::uint32_t dims = 2;

  std::int64_t q(double v) const { return std::llround(v * scale); }
  void add(std::vector<std::int64_t>& coords, const std::vector<ir::Coord>& pts, bool closed) const {
    std::int64_t prev[3] = {0, 0, 0};
    const std::size_t n = closed && pts.size() > 1 ? pts.size() - 1 : pts.size();
    for (std::size_t i = 0; i < n; ++i) {
      const std::int64_t v[3] = {q(pts[i].lon), q(pts[i].lat), q(pts[i].ele.value_or(0.0))};
      for (std::uint32_t j = 0; j < dims; ++j) {
        coords.push_back(v[j] - prev[j]);
        prev[j] = v[j];
      }
    }
  }
  static std::uint32_t count(const std::vector<ir::Coord>& pts, bool closed) {
    return static_cast<std::uint32_t>(closed && pts.size() > 1 ? pts.size() - 1 : pts.size());
  }

  Bytes geometry(const ir::Geometry& g, int depth) const {
    pbf::Writer w;
    std::vector<std::uint32_t> lengths;
    std::vector<std::int64_t> coords;
    std::uint32_t type = GbCollection;
    std::vector<Bytes> members;
    std::visit(
        [&](const auto& s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, ir::Point>) {
            type = GbPoint;
            const std::int64_t v[3] = {q(s.coord.lon), q(s.coord.lat), q(s.coord.ele.value_or(0.0))};
            for (std::uint32_t j = 0; j < dims; ++j) coords.push_back(v[j]);
          } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
            type = GbMultiPoint;
            add(coords, s.points, false);
          } else if constexpr (std::is_same_v<T, ir::LineString>) {
            type = GbLineString;
            add(coords, s.points, false);
          } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
            type = GbMultiLineString;
            for (const auto& l : s.lines) {
              lengths.push_back(count(l.points, false));
              add(coords, l.points, false);
            }
            if (s.lines.size() == 1) lengths.clear();
          } else if constexpr (std::is_same_v<T, ir::Polygon>) {
            type = GbPolygon;
            for (const auto& r : s.rings) {
              lengths.push_back(count(r, true));
              add(coords, r, true);
            }
            if (s.rings.size() == 1) lengths.clear();
          } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
            type = GbMultiPolygon;
            lengths.push_back(static_cast<std::uint32_t>(s.polygons.size()));
            for (const auto& p : s.polygons) {
              lengths.push_back(static_cast<std::uint32_t>(p.rings.size()));
              for (const auto& r : p.rings) {
                lengths.push_back(count(r, true));
                add(coords, r, true);
              }
            }
          } else {
            type = GbCollection;
            if (depth < ir::max_geometry_depth) {
              for (const auto& inner : s.geometries) members.push_back(geometry(inner, depth + 1));
            }
          }
        },
        g.shape);
    w.uint_field(1, type);
    if (!lengths.empty()) {
      pbf::Writer packed;
      for (std::uint32_t l : lengths) packed.varint(l);
      w.bytes_field(2, packed.take());
    }
    if (!coords.empty()) {
      pbf::Writer packed;
      for (std::int64_t c : coords) packed.varint(pbf::Writer::zigzag(c));
      w.bytes_field(3, packed.take());
    }
    for (const Bytes& m : members) w.bytes_field(4, m);
    return w.take();
  }
};

Bytes encode_value(const Json& v) {
  pbf::Writer w;
  switch (v.type()) {
    case Json::Type::String: w.string_field(1, v.as_string()); break;
    case Json::Type::Double: w.double_field(2, v.as_double()); break;
    case Json::Type::Int64:
      if (v.as_int64() >= 0) w.uint_field(3, static_cast<std::uint64_t>(v.as_int64()));
      else w.uint_field(4, static_cast<std::uint64_t>(-(v.as_int64() + 1)) + 1);
      break;
    case Json::Type::UInt64: w.uint_field(3, v.as_uint64()); break;
    case Json::Type::Bool: w.uint_field(5, v.as_bool() ? 1 : 0); break;
    default: w.string_field(6, util::dump_json(v)); break;
  }
  return w.take();
}

// Fewest decimals (up to 9) that represent every coordinate exactly.
std::uint32_t auto_precision(const ir::Geo& geo) {
  std::uint32_t needed = 0;
  bool done = false;
  for (const auto& f : geo.features) {
    for_each_coord(f.geometry, [&](const ir::Coord& c) {
      if (done) return;
      for (double v : {c.lon, c.lat, c.ele.value_or(0.0)}) {
        std::uint32_t d = needed;
        while (d < 9 && std::fabs(std::round(v * std::pow(10.0, d)) / std::pow(10.0, d) - v) > 1e-12 * std::max(1.0, std::fabs(v))) ++d;
        needed = d;
        if (needed >= 9) done = true;
      }
    });
  }
  return std::max<std::uint32_t>(needed, 1);
}

Result<Bytes> write_geobuf(const ir::Geo& geo, WriteContext& ctx) {
  const auto option = ctx.options.get_int("geobuf_precision", -1, -1, 12);
  Encoder e;
  const std::uint32_t precision = option >= 0 ? static_cast<std::uint32_t>(option) : auto_precision(geo);
  e.scale = std::pow(10.0, precision);
  for (const auto& f : geo.features) {
    for_each_coord(f.geometry, [&](const ir::Coord& c) {
      if (c.ele) e.dims = 3;
    });
  }
  std::vector<std::string> keys;
  std::map<std::string, std::uint32_t, std::less<>> key_index;
  for (const auto& f : geo.features) {
    for (const auto& m : f.properties.items()) {
      if (key_index.emplace(std::string(m.key()), static_cast<std::uint32_t>(keys.size())).second) keys.emplace_back(m.key());
    }
  }
  pbf::Writer collection;
  std::size_t index = 0;
  for (const auto& f : geo.features) {
    if ((++index & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(geo.features.size())));
    pbf::Writer fw;
    fw.bytes_field(1, e.geometry(f.geometry, 0));
    if (f.id.is_string()) fw.string_field(11, f.id.as_string());
    else if (f.id.is_integer()) fw.sint_field(12, f.id.as_int64());
    pbf::Writer props;
    std::uint32_t value_index = 0;
    for (const auto& m : f.properties.items()) {
      fw.bytes_field(13, encode_value(m.value()));
      props.varint(key_index.find(m.key())->second);
      props.varint(value_index++);
    }
    const Bytes packed = props.take();
    if (!packed.empty()) fw.bytes_field(14, packed);
    collection.bytes_field(1, fw.take());
  }
  pbf::Writer data;
  for (const auto& k : keys) data.string_field(1, k);
  if (e.dims != 2) data.uint_field(2, e.dims);
  if (precision != 6) data.uint_field(3, precision);
  data.bytes_field(4, collection.take());
  return data.take();
}

int sniff_geobuf(ByteView data, std::string_view filename) {
  if (data.size() < 4) return 0;
  const auto first = std::to_integer<std::uint8_t>(data[0]);
  // Data messages start with keys (0x0A), dimensions (0x10), precision (0x18) or the payload (0x22, 0x2A, 0x32).
  if (first != 0x0A && first != 0x10 && first != 0x18 && first != 0x22 && first != 0x2A && first != 0x32) return 0;
  if (!has_extension(filename, "geobuf") && !has_extension(filename, "pbf")) return 0;
  if (has_extension(filename, "osm.pbf")) return 0;
  return 70;
}

}  // namespace

void register_geobuf(Registry& registry) {
  FormatInfo info;
  info.id = "geobuf";
  info.name = "Geobuf";
  info.description = "Compact protocol-buffer encoding of GeoJSON (Mapbox geobuf).";
  info.category = Category::Geo;
  info.extensions = {"geobuf", "pbf"};
  info.mime = "application/x-protobuf";
  info.popularity = 15;
  info.sniff = sniff_geobuf;
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "geobuf";
  spec.read = read_geobuf;
  spec.write = write_geobuf;
  spec.write_options = {OptionSpec::integer("geobuf_precision", "Coordinate decimals", -1, -1, 12,
                                            "Decimal digits of the stored integer coordinates; -1 picks the fewest "
                                            "that keep every coordinate exact (at most 9).")
                            .advanced()};
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
