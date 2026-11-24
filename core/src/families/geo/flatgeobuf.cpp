// Convertible core - geo family: FlatGeobuf.
//
// The file is "fgb" + version magic, a size-prefixed FlatBuffers Header
// (geometry type, dimensions, columns, feature count, index node size, CRS),
// an optional packed Hilbert R-tree (skipped on reading, its size computed
// from the feature count with overflow checks) and size-prefixed Feature
// tables (geometry as ends / xy / z / m arrays and parts, properties as
// column-typed binary). FlatBuffers are decoded in-house with every offset,
// vtable and vector checked against the buffer; nested parts are
// depth-limited. The writer emits aligned FlatBuffers without an index.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "families/geo/geo.hpp"
#include "families/geo/geo_ops.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::geo {

namespace {

constexpr std::uint8_t magic[8] = {0x66, 0x67, 0x62, 0x03, 0x66, 0x67, 0x62, 0x00};

enum : std::uint8_t { GtUnknown = 0, GtPoint, GtLineString, GtPolygon, GtMultiPoint, GtMultiLineString, GtMultiPolygon,
                      GtCollection, GtTin = 15, GtTriangle = 16 };
enum : std::uint8_t { CtByte = 0, CtUByte, CtBool, CtShort, CtUShort, CtInt, CtUInt, CtLong, CtULong, CtFloat, CtDouble,
                      CtString, CtJson, CtDateTime, CtBinary };

// --------------------------------------------------------------------------- FlatBuffers reading

struct Vec {
  std::size_t pos = 0;  // first element
  std::uint32_t length = 0;
};

class Table {
 public:
  Table() = default;
  Table(ByteView buf, std::size_t pos) : buf_(buf), pos_(pos) {
    if (pos + 4 > buf.size()) return;
    const auto soffset = static_cast<std::int32_t>(u32(pos));
    const std::int64_t vt = static_cast<std::int64_t>(pos) - soffset;
    if (vt < 0 || static_cast<std::uint64_t>(vt) + 4 > buf.size()) return;
    vt_ = static_cast<std::size_t>(vt);
    vt_size_ = u16(vt_);
    if (vt_size_ < 4 || vt_ + vt_size_ > buf.size()) return;
    valid_ = true;
  }
  [[nodiscard]] bool valid() const { return valid_; }

  // Position of field i, or 0 when absent / out of bounds.
  [[nodiscard]] std::size_t field(int i, std::size_t size) const {
    if (!valid_) return 0;
    const std::size_t entry = 4 + 2 * static_cast<std::size_t>(i);
    if (entry + 2 > vt_size_) return 0;
    const std::uint16_t off = u16(vt_ + entry);
    if (off == 0 || pos_ + off + size > buf_.size()) return 0;
    return pos_ + off;
  }
  template <class T>
  [[nodiscard]] T scalar(int i, T fallback) const {
    const std::size_t at = field(i, sizeof(T));
    if (at == 0) return fallback;
    util::ByteReader r(buf_.subspan(at, sizeof(T)));
    if constexpr (std::is_same_v<T, double>) return r.f64le();
    else return static_cast<T>(r.read<std::make_unsigned_t<T>>(std::endian::little));
  }
  // Target of the uoffset stored in field i.
  [[nodiscard]] std::optional<std::size_t> ref(int i) const {
    const std::size_t at = field(i, 4);
    if (at == 0) return std::nullopt;
    const std::uint64_t target = std::uint64_t{at} + u32(at);
    if (target >= buf_.size()) return std::nullopt;
    return static_cast<std::size_t>(target);
  }
  [[nodiscard]] std::optional<Vec> vector(int i, std::size_t element) const {
    const auto at = ref(i);
    if (!at || *at + 4 > buf_.size()) return std::nullopt;
    const std::uint32_t n = u32(*at);
    if (element != 0 && n > (buf_.size() - *at - 4) / element) return std::nullopt;
    return Vec{*at + 4, n};
  }
  [[nodiscard]] std::string string(int i) const {
    const auto v = vector(i, 1);
    if (!v) return {};
    return util::sanitize_utf8(as_chars(buf_.subspan(v->pos, v->length)));
  }
  [[nodiscard]] Table table(int i) const {
    const auto at = ref(i);
    return at ? Table(buf_, *at) : Table();
  }
  // Element k of a vector of tables.
  [[nodiscard]] Table table_at(const Vec& v, std::uint32_t k) const {
    const std::size_t slot = v.pos + std::size_t{k} * 4;
    if (slot + 4 > buf_.size()) return {};
    const std::uint64_t target = std::uint64_t{slot} + u32(slot);
    if (target >= buf_.size()) return {};
    return Table(buf_, static_cast<std::size_t>(target));
  }
  [[nodiscard]] double f64_at(const Vec& v, std::size_t k) const {
    util::ByteReader r(buf_.subspan(v.pos + k * 8, 8));
    return r.f64le();
  }
  [[nodiscard]] std::uint32_t u32_at(const Vec& v, std::size_t k) const { return u32(v.pos + k * 4); }
  [[nodiscard]] ByteView bytes(const Vec& v) const { return buf_.subspan(v.pos, v.length); }

 private:
  [[nodiscard]] std::uint32_t u32(std::size_t at) const {
    util::ByteReader r(buf_.subspan(at, 4));
    return r.u32le();
  }
  [[nodiscard]] std::uint16_t u16(std::size_t at) const {
    util::ByteReader r(buf_.subspan(at, 2));
    return r.u16le();
  }
  ByteView buf_{};
  std::size_t pos_ = 0, vt_ = 0;
  std::uint16_t vt_size_ = 0;
  bool valid_ = false;
};

Table root_of(ByteView buf) {
  if (buf.size() < 8) return {};
  util::ByteReader r(buf);
  const std::uint32_t root = r.u32le();
  if (root >= buf.size()) return {};
  return Table(buf, root);
}

struct Column {
  std::string name;
  std::uint8_t type = CtString;
};

struct Dims {
  bool z = false, m = false;
};

Result<std::vector<ir::Coord>> coords_of(const Table& g, std::size_t from, std::size_t to, const Dims& dims) {
  const auto xy = g.vector(1, 8);
  if (!xy) return std::vector<ir::Coord>{};
  if (xy->length % 2 != 0) return fail(Status::Decode, "a FlatGeobuf coordinate array has an odd length");
  const std::size_t n = xy->length / 2;
  if (from > to || to > n) return fail(Status::Decode, "a FlatGeobuf ring or part end is out of range");
  const auto z = dims.z ? g.vector(2, 8) : std::nullopt;
  const auto m = dims.m ? g.vector(3, 8) : std::nullopt;
  std::vector<ir::Coord> out;
  out.reserve(to - from);
  for (std::size_t i = from; i < to; ++i) {
    ir::Coord c;
    c.lon = g.f64_at(*xy, 2 * i);
    c.lat = g.f64_at(*xy, 2 * i + 1);
    if (z && i < z->length) c.ele = g.f64_at(*z, i);
    if (m && i < m->length) {
      const double v = g.f64_at(*m, i);
      if (!std::isnan(v)) c.m = v;
    }
    out.push_back(c);
  }
  return out;
}

Result<std::vector<std::vector<ir::Coord>>> parts_by_ends(const Table& g, const Dims& dims) {
  const auto xy = g.vector(1, 8);
  const std::size_t n = xy ? xy->length / 2 : 0;
  std::vector<std::vector<ir::Coord>> out;
  const auto ends = g.vector(0, 4);
  if (!ends || ends->length == 0) {
    CVT_TRY_ASSIGN(auto all, coords_of(g, 0, n, dims));
    out.push_back(std::move(all));
    return out;
  }
  std::size_t start = 0;
  for (std::uint32_t k = 0; k < ends->length; ++k) {
    const std::size_t end = g.u32_at(*ends, k);
    CVT_TRY_ASSIGN(auto part, coords_of(g, start, end, dims));
    out.push_back(std::move(part));
    start = end;
  }
  return out;
}

Result<ir::Geometry> fgb_geometry(const Table& g, std::uint8_t type, const Dims& dims, int depth) {
  if (depth > ir::max_geometry_depth) return fail(Status::Limit, "FlatGeobuf geometry parts are nested too deeply");
  ir::Geometry out;
  if (!g.valid()) {
    out.shape = ir::GeometryCollection{};
    return out;
  }
  if (type == GtUnknown) type = g.scalar<std::uint8_t>(6, GtUnknown);
  switch (type) {
    case GtPoint: {
      CVT_TRY_ASSIGN(auto pts, coords_of(g, 0, 1, dims));
      if (pts.empty()) out.shape = ir::MultiPoint{};
      else out.shape = ir::Point{pts.front()};
      return out;
    }
    case GtLineString: {
      CVT_TRY_ASSIGN(auto parts, parts_by_ends(g, dims));
      out.shape = ir::LineString{std::move(parts.front())};
      return out;
    }
    case GtMultiPoint: {
      CVT_TRY_ASSIGN(auto parts, parts_by_ends(g, dims));
      out.shape = ir::MultiPoint{std::move(parts.front())};
      return out;
    }
    case GtPolygon:
    case GtTriangle: {
      CVT_TRY_ASSIGN(auto rings, parts_by_ends(g, dims));
      out.shape = ir::Polygon{std::move(rings)};
      return out;
    }
    case GtMultiLineString: {
      CVT_TRY_ASSIGN(auto lines, parts_by_ends(g, dims));
      ir::MultiLineString ml;
      for (auto& l : lines) ml.lines.push_back(ir::LineString{std::move(l)});
      out.shape = std::move(ml);
      return out;
    }
    case GtMultiPolygon:
    case GtTin:
    case GtCollection: {
      const auto parts = g.vector(7, 4);
      std::vector<ir::Geometry> members;
      if (parts) {
        for (std::uint32_t k = 0; k < parts->length; ++k) {
          const Table part = g.table_at(*parts, k);
          CVT_TRY_ASSIGN(ir::Geometry inner, fgb_geometry(part, type == GtCollection ? GtUnknown : GtPolygon, dims, depth + 1));
          members.push_back(std::move(inner));
        }
      }
      if (type == GtCollection) {
        out.shape = ir::GeometryCollection{std::move(members)};
      } else {
        ir::MultiPolygon mp;
        for (auto& m : members) {
          if (auto* p = std::get_if<ir::Polygon>(&m.shape)) mp.polygons.push_back(std::move(*p));
        }
        out.shape = std::move(mp);
      }
      return out;
    }
    default: return fail(Status::Decode, "unsupported FlatGeobuf geometry type {}", type);
  }
}

Result<Json> fgb_properties(ByteView props, const std::vector<Column>& columns) {
  Json out = Json::object();
  util::ByteReader r(props);
  while (r.remaining() >= 2) {
    const std::uint16_t index = r.u16le();
    if (index >= columns.size()) return fail(Status::Decode, "a FlatGeobuf property refers to a missing column");
    const Column& c = columns[index];
    Json v;
    switch (c.type) {
      case CtByte: v = Json(static_cast<std::int64_t>(r.i8())); break;
      case CtUByte: v = Json(static_cast<std::int64_t>(r.u8())); break;
      case CtBool: v = Json(r.u8() != 0); break;
      case CtShort: v = Json(static_cast<std::int64_t>(r.i16le())); break;
      case CtUShort: v = Json(static_cast<std::int64_t>(r.u16le())); break;
      case CtInt: v = Json(static_cast<std::int64_t>(r.i32le())); break;
      case CtUInt: v = Json(static_cast<std::int64_t>(r.u32le())); break;
      case CtLong: v = Json(r.i64le()); break;
      case CtULong: v = Json(r.u64le()); break;
      case CtFloat: v = Json(static_cast<double>(r.f32le())); break;
      case CtDouble: v = Json(r.f64le()); break;
      case CtString:
      case CtJson:
      case CtDateTime:
      case CtBinary: {
        const std::uint32_t n = r.u32le();
        const ByteView bytes = r.bytes(n);
        if (!r.ok()) return fail(Status::Decode, "a FlatGeobuf property is truncated");
        if (c.type == CtJson) {
          auto parsed = util::parse_json(as_chars(bytes));
          v = parsed ? std::move(*parsed) : Json(util::sanitize_utf8(as_chars(bytes)));
        } else if (c.type == CtBinary) {
          v = Json(std::string("(binary)"));
        } else {
          v = Json(util::sanitize_utf8(as_chars(bytes)));
        }
        break;
      }
      default: return fail(Status::Decode, "unsupported FlatGeobuf column type {}", c.type);
    }
    if (!r.ok()) return fail(Status::Decode, "a FlatGeobuf property is truncated");
    out.set(c.name, std::move(v));
  }
  return out;
}

std::vector<Column> columns_of(const Table& t, int field) {
  std::vector<Column> out;
  const auto v = t.vector(field, 4);
  if (!v) return out;
  for (std::uint32_t k = 0; k < v->length && k < 65536; ++k) {
    const Table c = t.table_at(*v, k);
    if (!c.valid()) continue;
    out.push_back({c.string(0), c.scalar<std::uint8_t>(1, CtString)});
  }
  return out;
}

Result<ir::Geo> read_fgb(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  const ByteView head = r.bytes(8);
  if (!r.ok() || std::memcmp(head.data(), magic, 3) != 0 || std::to_integer<std::uint8_t>(head[3]) != 3) {
    return fail(Status::Decode, "not a FlatGeobuf file");
  }
  const std::uint32_t header_size = r.u32le();
  const ByteView header_buf = r.bytes(header_size);
  if (!r.ok()) return fail(Status::Decode, "the FlatGeobuf header is truncated");
  const Table header = root_of(header_buf);
  if (!header.valid()) return fail(Status::Decode, "the FlatGeobuf header is damaged");
  ir::Geo geo;
  geo.name = header.string(0);
  const std::uint8_t type = header.scalar<std::uint8_t>(2, GtUnknown);
  const Dims dims{header.scalar<std::uint8_t>(3, 0) != 0, header.scalar<std::uint8_t>(4, 0) != 0};
  const std::vector<Column> columns = columns_of(header, 7);
  const std::uint64_t count = header.scalar<std::uint64_t>(8, 0);
  const std::uint16_t node_size = header.scalar<std::uint16_t>(9, 16);
  const Table crs = header.table(10);
  if (crs.valid()) {
    const std::int32_t code = crs.scalar<std::int32_t>(1, 0);
    const std::string org = crs.string(0);
    if (code > 0 && (org.empty() || util::iequals(org, "EPSG"))) geo.crs = "EPSG:" + std::to_string(code);
    else if (!crs.string(4).empty()) geo.crs = crs.string(4);
  }
  if (const std::string d = header.string(12); !d.empty()) geo.description = d;
  // Skip the spatial index.
  if (node_size >= 2 && count > 0) {
    std::uint64_t n = count, total = count;
    while (n > 1) {
      n = (n + node_size - 1) / node_size;
      total += n;
    }
    if (total > r.remaining() / 40) return fail(Status::Decode, "the FlatGeobuf index is larger than the file");
    r.skip(static_cast<std::size_t>(total * 40));
  }
  if (count > r.remaining() / 8) return fail(Status::Decode, "the FlatGeobuf header declares more features than the file holds");
  while (r.remaining() >= 4) {
    if ((geo.features.size() & 1023) == 1023) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(r.position()) / static_cast<double>(ctx.input.size())));
    }
    const std::uint32_t size = r.u32le();
    const ByteView buf = r.bytes(size);
    if (!r.ok()) return fail(Status::Decode, "a FlatGeobuf feature is truncated");
    const Table feature = root_of(buf);
    if (!feature.valid()) return fail(Status::Decode, "a FlatGeobuf feature is damaged");
    ir::Feature f;
    CVT_TRY_ASSIGN(f.geometry, fgb_geometry(feature.table(0), type, dims, 0));
    std::vector<Column> own = columns_of(feature, 2);
    if (const auto props = feature.vector(1, 1)) {
      CVT_TRY_ASSIGN(f.properties, fgb_properties(feature.bytes(*props), own.empty() ? columns : own));
    }
    if (const Json* name = f.properties.find("name"); name != nullptr && name->is_string()) f.name = name->as_string();
    geo.features.push_back(std::move(f));
  }
  if (count != 0 && geo.features.size() != count) ctx.warn("The FlatGeobuf file holds a different number of features than its header says.");
  return geo;
}

// --------------------------------------------------------------------------- FlatBuffers writing

class Builder {
 public:
  struct Field {
    int index;
    std::size_t size;                 // 1, 2, 4 or 8 for scalars; 4 for references
    std::uint64_t value = 0;          // scalar bits
    std::function<std::size_t(Builder&)> child;  // writes the referenced object, returns its position
  };

  [[nodiscard]] std::size_t size() const { return out_.size(); }
  [[nodiscard]] Bytes take() { return out_.take(); }

  void pad_to(std::size_t alignment, std::size_t offset = 0) {
    while ((out_.size() + offset) % alignment != 0) out_.u8(0);
  }
  void patch_u32(std::size_t at, std::uint32_t v) { (void)out_.patch_u32le(at, v); }

  // Writes a table with its vtable in front of it; children follow it.
  std::size_t table(std::vector<Field> fields) {
    int max_index = -1;
    for (const auto& f : fields) max_index = std::max(max_index, f.index);
    const std::size_t vt_entries = static_cast<std::size_t>(max_index + 1);
    pad_to(2);
    const std::size_t vt = out_.size();
    out_.u16le(static_cast<std::uint16_t>(4 + 2 * vt_entries)).u16le(0);
    for (std::size_t i = 0; i < vt_entries; ++i) out_.u16le(0);
    pad_to(8, 0);
    // Make sure 8-byte fields can be aligned right after the 4-byte soffset.
    if (out_.size() % 8 == 0) out_.u32le(0);
    const std::size_t table_pos = out_.size();
    out_.i32le(static_cast<std::int32_t>(table_pos - vt));
    std::sort(fields.begin(), fields.end(), [](const Field& a, const Field& b) { return a.size > b.size; });
    std::vector<std::pair<std::size_t, const Field*>> refs;
    for (const Field& f : fields) {
      pad_to(f.size);
      const std::size_t at = out_.size();
      (void)out_.patch(vt + 4 + 2 * static_cast<std::size_t>(f.index), static_cast<std::uint16_t>(at - table_pos),
                       std::endian::little);
      switch (f.size) {
        case 1: out_.u8(static_cast<std::uint8_t>(f.value)); break;
        case 2: out_.u16le(static_cast<std::uint16_t>(f.value)); break;
        case 4: out_.u32le(static_cast<std::uint32_t>(f.value)); break;
        default: out_.u64le(f.value); break;
      }
      if (f.child) refs.emplace_back(at, &f);
    }
    (void)out_.patch(vt + 2, static_cast<std::uint16_t>(out_.size() - table_pos), std::endian::little);
    for (const auto& [slot, f] : refs) {
      const std::size_t child = f->child(*this);
      patch_u32(slot, static_cast<std::uint32_t>(child - slot));
    }
    return table_pos;
  }
  std::size_t string(std::string_view s) {
    pad_to(4);
    const std::size_t at = out_.size();
    out_.u32le(static_cast<std::uint32_t>(s.size())).text(s).u8(0);
    return at;
  }
  std::size_t f64_vector(const std::vector<double>& v) {
    pad_to(8, 4);
    const std::size_t at = out_.size();
    out_.u32le(static_cast<std::uint32_t>(v.size()));
    for (double d : v) out_.f64le(d);
    return at;
  }
  std::size_t u32_vector(const std::vector<std::uint32_t>& v) {
    pad_to(4);
    const std::size_t at = out_.size();
    out_.u32le(static_cast<std::uint32_t>(v.size()));
    for (std::uint32_t d : v) out_.u32le(d);
    return at;
  }
  std::size_t byte_vector(ByteView v) {
    pad_to(4);
    const std::size_t at = out_.size();
    out_.u32le(static_cast<std::uint32_t>(v.size())).bytes(v);
    return at;
  }
  // A vector of tables written by `make(k)`.
  std::size_t table_vector(std::size_t n, const std::function<std::size_t(Builder&, std::size_t)>& make) {
    pad_to(4);
    const std::size_t at = out_.size();
    out_.u32le(static_cast<std::uint32_t>(n));
    const std::size_t slots = out_.size();
    out_.zeros(4 * n);
    for (std::size_t k = 0; k < n; ++k) {
      const std::size_t child = make(*this, k);
      patch_u32(slots + 4 * k, static_cast<std::uint32_t>(child - (slots + 4 * k)));
    }
    return at;
  }
  // Root offset placeholder at the start of a buffer.
  void begin_root() { out_.u32le(0); }
  void set_root(std::size_t table_pos) { patch_u32(0, static_cast<std::uint32_t>(table_pos)); }

 private:
  util::ByteWriter out_;
};

std::uint8_t geometry_code(const ir::Geometry& g) {
  switch (g.shape.index()) {
    case 0: return GtPoint;
    case 1: return GtLineString;
    case 2: return GtPolygon;
    case 3: return GtMultiPoint;
    case 4: return GtMultiLineString;
    case 5: return GtMultiPolygon;
    default: return GtCollection;
  }
}

struct Arrays {
  std::vector<std::uint32_t> ends;
  std::vector<double> xy, z, m;
  void add(const std::vector<ir::Coord>& pts, const Dims& d) {
    for (const ir::Coord& c : pts) {
      xy.push_back(c.lon);
      xy.push_back(c.lat);
      if (d.z) z.push_back(c.ele.value_or(0.0));
      if (d.m) m.push_back(c.m.value_or(std::numeric_limits<double>::quiet_NaN()));
    }
    ends.push_back(static_cast<std::uint32_t>(xy.size() / 2));
  }
};

std::size_t write_geometry(Builder& b, const ir::Geometry& g, const Dims& d, bool with_type, int depth) {
  Arrays a;
  std::vector<const ir::Geometry*> parts;
  std::vector<ir::Geometry> polygon_parts;
  bool use_ends = false;
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ir::Point>) a.add({s.coord}, d);
        else if constexpr (std::is_same_v<T, ir::LineString> || std::is_same_v<T, ir::MultiPoint>) a.add(s.points, d);
        else if constexpr (std::is_same_v<T, ir::Polygon>) {
          for (const auto& r : s.rings) a.add(r, d);
          use_ends = s.rings.size() > 1;
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          for (const auto& l : s.lines) a.add(l.points, d);
          use_ends = s.lines.size() > 1;
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          for (const auto& p : s.polygons) polygon_parts.push_back(ir::Geometry{p});
        } else {
          if (depth < ir::max_geometry_depth) {
            for (const auto& inner : s.geometries) parts.push_back(&inner);
          }
        }
      },
      g.shape);
  for (const auto& p : polygon_parts) parts.push_back(&p);
  const bool collection = std::holds_alternative<ir::GeometryCollection>(g.shape);
  std::vector<Builder::Field> fields;
  if (use_ends) fields.push_back({0, 4, 0, [&a](Builder& bb) { return bb.u32_vector(a.ends); }});
  if (!a.xy.empty()) fields.push_back({1, 4, 0, [&a](Builder& bb) { return bb.f64_vector(a.xy); }});
  if (d.z && !a.z.empty()) fields.push_back({2, 4, 0, [&a](Builder& bb) { return bb.f64_vector(a.z); }});
  if (d.m && !a.m.empty()) fields.push_back({3, 4, 0, [&a](Builder& bb) { return bb.f64_vector(a.m); }});
  if (with_type || collection) fields.push_back({6, 1, geometry_code(g), {}});
  if (!parts.empty()) {
    fields.push_back({7, 4, 0, [&parts, &d, collection, depth](Builder& bb) {
                        return bb.table_vector(parts.size(), [&](Builder& b2, std::size_t k) {
                          return write_geometry(b2, *parts[k], d, collection, depth + 1);
                        });
                      }});
  }
  return b.table(std::move(fields));
}

struct OutColumn {
  std::string name;
  std::uint8_t type = CtString;
};

std::vector<OutColumn> infer_columns(const ir::Geo& geo) {
  std::vector<OutColumn> out;
  for (const auto& f : geo.features) {
    for (const auto& m : f.properties.items()) {
      const Json& v = m.value();
      std::uint8_t t = v.is_bool() ? CtBool : v.is_integer() ? CtLong : v.is_float() ? CtDouble
                                                               : v.is_structured() ? CtJson : CtString;
      auto it = std::find_if(out.begin(), out.end(), [&](const OutColumn& c) { return c.name == m.key(); });
      if (it == out.end()) {
        if (v.is_null()) t = 255;  // decided later
        out.push_back({std::string(m.key()), t});
        continue;
      }
      if (v.is_null() || it->type == t) continue;
      if (it->type == 255) it->type = t;
      else if ((it->type == CtLong && t == CtDouble) || (it->type == CtDouble && t == CtLong)) it->type = CtDouble;
      else it->type = CtString;
    }
  }
  for (auto& c : out) {
    if (c.type == 255) c.type = CtString;
  }
  return out;
}

Bytes encode_properties(const Json& props, const std::vector<OutColumn>& columns) {
  util::ByteWriter w;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const Json& v = props[columns[i].name];
    if (v.is_null()) continue;
    w.u16le(static_cast<std::uint16_t>(i));
    switch (columns[i].type) {
      case CtBool: w.u8(v.as_bool() ? 1 : 0); break;
      case CtLong: w.i64le(v.as_int64()); break;
      case CtDouble: w.f64le(v.as_double()); break;
      default: {
        const std::string text = columns[i].type == CtJson ? util::dump_json(v) : json_to_text(v);
        w.u32le(static_cast<std::uint32_t>(text.size())).text(text);
      }
    }
  }
  return w.take();
}

Result<Bytes> write_fgb(const ir::Geo& geo, WriteContext& ctx) {
  Dims dims;
  std::optional<std::uint8_t> type;
  bool mixed = false;
  for (const auto& f : geo.features) {
    for_each_coord(f.geometry, [&](const ir::Coord& c) {
      dims.z = dims.z || c.ele.has_value();
      dims.m = dims.m || c.m.has_value();
    });
    const std::uint8_t t = geometry_code(f.geometry);
    if (type && *type != t) mixed = true;
    type = t;
  }
  const std::uint8_t header_type = (!type || mixed) ? static_cast<std::uint8_t>(GtUnknown) : *type;
  const std::vector<OutColumn> columns = infer_columns(geo);
  const BoundingBox box = bounding_box(geo);
  int epsg = 4326;
  if (const auto crs = parse_crs(geo.crs)) epsg = crs->epsg;
  util::ByteWriter out;
  for (std::uint8_t b : magic) out.u8(b);
  {
    Builder b;
    b.begin_root();
    std::vector<Builder::Field> fields;
    const std::string name = geo.name;
    if (!name.empty()) fields.push_back({0, 4, 0, [&name](Builder& bb) { return bb.string(name); }});
    std::vector<double> envelope;
    if (!box.empty) envelope = {box.min_lon, box.min_lat, box.max_lon, box.max_lat};
    if (!envelope.empty()) fields.push_back({1, 4, 0, [&envelope](Builder& bb) { return bb.f64_vector(envelope); }});
    fields.push_back({2, 1, header_type, {}});
    if (dims.z) fields.push_back({3, 1, 1, {}});
    if (dims.m) fields.push_back({4, 1, 1, {}});
    if (!columns.empty()) {
      fields.push_back({7, 4, 0, [&columns](Builder& bb) {
                          return bb.table_vector(columns.size(), [&](Builder& b2, std::size_t k) {
                            const std::string& cname = columns[k].name;
                            return b2.table({{0, 4, 0, [&cname](Builder& b3) { return b3.string(cname); }},
                                             {1, 1, columns[k].type, {}}});
                          });
                        }});
    }
    fields.push_back({8, 8, geo.features.size(), {}});
    fields.push_back({9, 2, 0, {}});  // no spatial index
    fields.push_back({10, 4, 0, [epsg](Builder& bb) {
                        return bb.table({{0, 4, 0, [](Builder& b3) { return b3.string("EPSG"); }},
                                         {1, 4, static_cast<std::uint32_t>(epsg), {}}});
                      }});
    if (!geo.description.empty()) {
      const std::string& d = geo.description;
      fields.push_back({12, 4, 0, [&d](Builder& bb) { return bb.string(d); }});
    }
    b.set_root(b.table(std::move(fields)));
    b.pad_to(8);
    const Bytes header = b.take();
    out.u32le(static_cast<std::uint32_t>(header.size())).bytes(header);
  }
  std::size_t index = 0;
  for (const auto& f : geo.features) {
    if ((++index & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(geo.features.size())));
    Builder b;
    b.begin_root();
    const Bytes props = encode_properties(f.properties, columns);
    std::vector<Builder::Field> fields;
    if (!geometry_empty(f.geometry)) {
      const ir::Geometry& g = f.geometry;
      fields.push_back({0, 4, 0, [&g, &dims, header_type](Builder& bb) {
                          return write_geometry(bb, g, dims, header_type == GtUnknown, 0);
                        }});
    }
    if (!props.empty()) fields.push_back({1, 4, 0, [&props](Builder& bb) { return bb.byte_vector(props); }});
    b.set_root(b.table(std::move(fields)));
    b.pad_to(8);
    const Bytes feature = b.take();
    out.u32le(static_cast<std::uint32_t>(feature.size())).bytes(feature);
  }
  return out.take();
}

}  // namespace

void register_flatgeobuf(Registry& registry) {
  FormatInfo info;
  info.id = "fgb";
  info.name = "FlatGeobuf";
  info.description = "Binary geographic features with an optional packed spatial index, built on FlatBuffers.";
  info.category = Category::Geo;
  info.extensions = {"fgb"};
  info.mime = "application/flatgeobuf";
  info.popularity = 30;
  info.signatures = {Signature::at(0, "'fgb' 03 'fgb' ??")};
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "fgb";
  spec.read = read_fgb;
  spec.write = write_fgb;
  spec.traits = GeoWriterTraits{true, false, false, false};
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
