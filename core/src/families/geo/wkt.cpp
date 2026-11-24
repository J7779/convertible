// Convertible core - geo family: WKT / EWKT, WKB / EWKB and hex WKB.
//
// Formats: `wkt` (one geometry per statement; EWKT SRID prefixes set the
// CRS), `wkb` (binary; several concatenated geometries are read one after
// another, several features are written as one GEOMETRYCOLLECTION) and
// `wkb-hex` (one hexadecimal WKB/EWKB per line, as PostGIS prints them).
// Attributes cannot be stored, so the writers are lossy.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/wkt.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::geo {

namespace {

// --------------------------------------------------------------------------- WKT writing

struct Dims {
  bool z = false;
  bool m = false;
};

Dims dims_of(const ir::Geometry& g) {
  Dims d;
  for_each_coord(g, [&](const ir::Coord& c) {
    d.z = d.z || c.ele.has_value();
    d.m = d.m || c.m.has_value();
  });
  return d;
}

struct WktWriter {
  int decimals = -1;
  Dims dims;
  std::string out;

  void number(double v) { out += format_number(v, decimals); }
  void position(const ir::Coord& c) {
    number(c.lon);
    out += ' ';
    number(c.lat);
    if (dims.z) {
      out += ' ';
      number(c.ele.value_or(0.0));
    }
    if (dims.m) {
      out += ' ';
      number(c.m.value_or(0.0));
    }
  }
  void sequence(const std::vector<ir::Coord>& points) {
    out += '(';
    for (std::size_t i = 0; i < points.size(); ++i) {
      if (i) out += ", ";
      position(points[i]);
    }
    out += ')';
  }
  void rings(const ir::Polygon& p) {
    out += '(';
    for (std::size_t i = 0; i < p.rings.size(); ++i) {
      if (i) out += ", ";
      sequence(p.rings[i]);
    }
    out += ')';
  }
  void tag(std::string_view name) {
    out += name;
    if (dims.z && dims.m) out += " ZM";
    else if (dims.z) out += " Z";
    else if (dims.m) out += " M";
  }
  void geometry(const ir::Geometry& g, int depth) {
    std::visit(
        [&](const auto& s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, ir::Point>) {
            tag("POINT");
            out += " (";
            position(s.coord);
            out += ')';
          } else if constexpr (std::is_same_v<T, ir::LineString>) {
            tag("LINESTRING");
            if (s.points.empty()) {
              out += " EMPTY";
            } else {
              out += ' ';
              sequence(s.points);
            }
          } else if constexpr (std::is_same_v<T, ir::Polygon>) {
            tag("POLYGON");
            if (s.rings.empty()) {
              out += " EMPTY";
            } else {
              out += ' ';
              rings(s);
            }
          } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
            tag("MULTIPOINT");
            if (s.points.empty()) {
              out += " EMPTY";
              return;
            }
            out += " (";
            for (std::size_t i = 0; i < s.points.size(); ++i) {
              if (i) out += ", ";
              out += '(';
              position(s.points[i]);
              out += ')';
            }
            out += ')';
          } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
            tag("MULTILINESTRING");
            if (s.lines.empty()) {
              out += " EMPTY";
              return;
            }
            out += " (";
            for (std::size_t i = 0; i < s.lines.size(); ++i) {
              if (i) out += ", ";
              sequence(s.lines[i].points);
            }
            out += ')';
          } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
            tag("MULTIPOLYGON");
            if (s.polygons.empty()) {
              out += " EMPTY";
              return;
            }
            out += " (";
            for (std::size_t i = 0; i < s.polygons.size(); ++i) {
              if (i) out += ", ";
              rings(s.polygons[i]);
            }
            out += ')';
          } else {
            tag("GEOMETRYCOLLECTION");
            if (s.geometries.empty() || depth >= ir::max_geometry_depth) {
              out += " EMPTY";
              return;
            }
            out += " (";
            for (std::size_t i = 0; i < s.geometries.size(); ++i) {
              if (i) out += ", ";
              geometry(s.geometries[i], depth + 1);
            }
            out += ')';
          }
        },
        g.shape);
  }
};

// --------------------------------------------------------------------------- WKT reading

struct WktParser {
  std::string_view text;
  std::size_t pos = 0;

  void skip_ws() {
    while (pos < text.size() && util::is_ascii_space(text[pos])) ++pos;
  }
  bool eat(char c) {
    skip_ws();
    if (pos < text.size() && text[pos] == c) {
      ++pos;
      return true;
    }
    return false;
  }
  bool peek(char c) {
    skip_ws();
    return pos < text.size() && text[pos] == c;
  }
  std::string word() {
    skip_ws();
    std::string w;
    while (pos < text.size() && (util::is_ascii_alpha(text[pos]) || text[pos] == '_')) w += util::ascii_upper(text[pos++]);
    return w;
  }
  // Peeks a keyword without consuming it.
  bool next_word_is(std::string_view keyword) {
    const std::size_t saved = pos;
    const std::string w = word();
    pos = saved;
    return w == keyword;
  }
  Result<double> number() {
    skip_ws();
    const std::size_t start = pos;
    while (pos < text.size() && !util::is_ascii_space(text[pos]) && text[pos] != ',' && text[pos] != ')' &&
           text[pos] != '(') {
      ++pos;
    }
    const auto v = parse_number(text.substr(start, pos - start));
    if (!v) return fail(Status::Decode, "invalid number in WKT at character {}", start + 1);
    return *v;
  }
  Result<ir::Coord> position(const Dims& declared, bool declared_any) {
    double values[4] = {0, 0, 0, 0};
    int count = 0;
    while (true) {
      skip_ws();
      if (pos >= text.size() || text[pos] == ',' || text[pos] == ')') break;
      if (count == 4) return fail(Status::Decode, "a WKT position has more than four ordinates");
      CVT_TRY_ASSIGN(values[count], number());
      ++count;
    }
    if (count < 2) return fail(Status::Decode, "a WKT position needs at least two numbers");
    ir::Coord c;
    c.lon = values[0];
    c.lat = values[1];
    if (declared_any) {
      int next = 2;
      if (declared.z && count > next) c.ele = values[next++];
      if (declared.m && count > next) c.m = values[next++];
    } else {
      if (count >= 3) c.ele = values[2];
      if (count >= 4) c.m = values[3];
    }
    return c;
  }
  Result<std::vector<ir::Coord>> sequence(const Dims& d, bool any) {
    if (next_word_is("EMPTY")) {
      (void)word();
      return std::vector<ir::Coord>{};
    }
    if (!eat('(')) return fail(Status::Decode, "expected '(' in WKT at character {}", pos + 1);
    std::vector<ir::Coord> out;
    do {
      // MULTIPOINT members may be written with their own parentheses.
      const bool wrapped = eat('(');
      CVT_TRY_ASSIGN(ir::Coord c, position(d, any));
      out.push_back(std::move(c));
      if (wrapped && !eat(')')) return fail(Status::Decode, "expected ')' in WKT at character {}", pos + 1);
    } while (eat(','));
    if (!eat(')')) return fail(Status::Decode, "expected ')' in WKT at character {}", pos + 1);
    return out;
  }
  Result<ir::Polygon> polygon(const Dims& d, bool any) {
    ir::Polygon p;
    if (next_word_is("EMPTY")) {
      (void)word();
      return p;
    }
    if (!eat('(')) return fail(Status::Decode, "expected '(' in WKT at character {}", pos + 1);
    do {
      CVT_TRY_ASSIGN(std::vector<ir::Coord> ring, sequence(d, any));
      p.rings.push_back(std::move(ring));
    } while (eat(','));
    if (!eat(')')) return fail(Status::Decode, "expected ')' in WKT at character {}", pos + 1);
    return p;
  }

  Result<ir::Geometry> geometry(int depth) {
    if (depth > ir::max_geometry_depth) return fail(Status::Limit, "WKT geometry collections are nested too deeply");
    std::string type = word();
    if (type.empty()) return fail(Status::Decode, "expected a WKT geometry type at character {}", pos + 1);
    static constexpr std::string_view names[] = {"POINT",           "LINESTRING",   "POLYGON",
                                                 "MULTIPOINT",      "MULTILINESTRING", "MULTIPOLYGON",
                                                 "GEOMETRYCOLLECTION", "TRIANGLE", "TIN",
                                                 "POLYHEDRALSURFACE", "LINEARRING"};
    const auto known = [](std::string_view t) {
      return std::find(std::begin(names), std::end(names), t) != std::end(names);
    };
    Dims d;
    bool any = false;
    if (!known(type)) {
      for (std::string_view suffix : {"ZM", "Z", "M"}) {
        if (type.size() > suffix.size() && type.ends_with(suffix) && known(type.substr(0, type.size() - suffix.size()))) {
          type.resize(type.size() - suffix.size());
          d.z = suffix != "M";
          d.m = suffix != "Z";
          any = true;
          break;
        }
      }
      if (!any) return fail(Status::Decode, "unsupported WKT geometry type '{}'", type.substr(0, 40));
    }
    if (!any) {
      for (std::string_view tag : {"ZM", "Z", "M"}) {
        if (next_word_is(tag)) {
          (void)word();
          d.z = tag != "M";
          d.m = tag != "Z";
          any = true;
          break;
        }
      }
    }
    ir::Geometry g;
    if (type == "POINT") {
      if (next_word_is("EMPTY")) {
        (void)word();
        g.shape = ir::MultiPoint{};
        return g;
      }
      if (!eat('(')) return fail(Status::Decode, "expected '(' in WKT at character {}", pos + 1);
      CVT_TRY_ASSIGN(ir::Coord c, position(d, any));
      if (!eat(')')) return fail(Status::Decode, "expected ')' in WKT at character {}", pos + 1);
      g.shape = ir::Point{std::move(c)};
    } else if (type == "LINESTRING" || type == "LINEARRING") {
      CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, sequence(d, any));
      g.shape = ir::LineString{std::move(pts)};
    } else if (type == "POLYGON" || type == "TRIANGLE") {
      CVT_TRY_ASSIGN(ir::Polygon p, polygon(d, any));
      g.shape = std::move(p);
    } else if (type == "MULTIPOINT") {
      CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, sequence(d, any));
      g.shape = ir::MultiPoint{std::move(pts)};
    } else if (type == "MULTILINESTRING") {
      ir::MultiLineString ml;
      if (next_word_is("EMPTY")) {
        (void)word();
      } else {
        if (!eat('(')) return fail(Status::Decode, "expected '(' in WKT at character {}", pos + 1);
        do {
          CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, sequence(d, any));
          ml.lines.push_back(ir::LineString{std::move(pts)});
        } while (eat(','));
        if (!eat(')')) return fail(Status::Decode, "expected ')' in WKT at character {}", pos + 1);
      }
      g.shape = std::move(ml);
    } else if (type == "MULTIPOLYGON" || type == "TIN" || type == "POLYHEDRALSURFACE") {
      ir::MultiPolygon mp;
      if (next_word_is("EMPTY")) {
        (void)word();
      } else {
        if (!eat('(')) return fail(Status::Decode, "expected '(' in WKT at character {}", pos + 1);
        do {
          CVT_TRY_ASSIGN(ir::Polygon p, polygon(d, any));
          mp.polygons.push_back(std::move(p));
        } while (eat(','));
        if (!eat(')')) return fail(Status::Decode, "expected ')' in WKT at character {}", pos + 1);
      }
      g.shape = std::move(mp);
    } else {
      ir::GeometryCollection gc;
      if (next_word_is("EMPTY")) {
        (void)word();
      } else {
        if (!eat('(')) return fail(Status::Decode, "expected '(' in WKT at character {}", pos + 1);
        do {
          CVT_TRY_ASSIGN(ir::Geometry inner, geometry(depth + 1));
          gc.geometries.push_back(std::move(inner));
        } while (eat(','));
        if (!eat(')')) return fail(Status::Decode, "expected ')' in WKT at character {}", pos + 1);
      }
      g.shape = std::move(gc);
    }
    return g;
  }
};

// --------------------------------------------------------------------------- WKB

constexpr std::uint32_t ewkb_z = 0x80000000u, ewkb_m = 0x40000000u, ewkb_srid = 0x20000000u;

struct WkbWriter {
  util::ByteWriter out;
  bool le = true;
  Dims dims;

  void u32(std::uint32_t v) { le ? out.u32le(v) : out.u32be(v); }
  void f64(double v) { le ? out.f64le(v) : out.f64be(v); }
  void header(std::uint32_t base, std::optional<int> srid, bool top) {
    out.u8(le ? 1 : 0);
    if (srid) {
      std::uint32_t type = base;
      if (dims.z) type |= ewkb_z;
      if (dims.m) type |= ewkb_m;
      if (top) type |= ewkb_srid;
      u32(type);
      if (top) u32(static_cast<std::uint32_t>(*srid));
    } else {
      u32(base + (dims.z ? 1000u : 0u) + (dims.m ? 2000u : 0u));
    }
  }
  void position(const ir::Coord& c) {
    f64(c.lon);
    f64(c.lat);
    if (dims.z) f64(c.ele.value_or(0.0));
    if (dims.m) f64(c.m.value_or(0.0));
  }
  void sequence(const std::vector<ir::Coord>& pts) {
    u32(static_cast<std::uint32_t>(pts.size()));
    for (const ir::Coord& c : pts) position(c);
  }
  void polygon_body(const ir::Polygon& p) {
    u32(static_cast<std::uint32_t>(p.rings.size()));
    for (const auto& r : p.rings) sequence(r);
  }
  void point(const ir::Coord& c, std::optional<int> srid, bool top) {
    header(1, srid, top);
    position(c);
  }
  void geometry(const ir::Geometry& g, std::optional<int> srid, bool top, int depth) {
    std::visit(
        [&](const auto& s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, ir::Point>) {
            point(s.coord, srid, top);
          } else if constexpr (std::is_same_v<T, ir::LineString>) {
            header(2, srid, top);
            sequence(s.points);
          } else if constexpr (std::is_same_v<T, ir::Polygon>) {
            header(3, srid, top);
            polygon_body(s);
          } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
            header(4, srid, top);
            u32(static_cast<std::uint32_t>(s.points.size()));
            for (const ir::Coord& c : s.points) point(c, srid, false);
          } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
            header(5, srid, top);
            u32(static_cast<std::uint32_t>(s.lines.size()));
            for (const auto& l : s.lines) {
              header(2, srid, false);
              sequence(l.points);
            }
          } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
            header(6, srid, top);
            u32(static_cast<std::uint32_t>(s.polygons.size()));
            for (const auto& p : s.polygons) {
              header(3, srid, false);
              polygon_body(p);
            }
          } else {
            header(7, srid, top);
            const bool deep = depth >= ir::max_geometry_depth;
            u32(deep ? 0u : static_cast<std::uint32_t>(s.geometries.size()));
            if (!deep) {
              for (const auto& inner : s.geometries) geometry(inner, srid, false, depth + 1);
            }
          }
        },
        g.shape);
  }
};

struct WkbParser {
  util::ByteReader r;
  int* srid = nullptr;

  struct Header {
    bool le = true;
    std::uint32_t base = 0;
    Dims d;
  };

  Result<Header> header() {
    Header h;
    const std::uint8_t order = r.u8();
    if (!r.ok() || order > 1) return fail(Status::Decode, "invalid WKB byte order marker");
    h.le = order == 1;
    std::uint32_t type = h.le ? r.u32le() : r.u32be();
    if (!r.ok()) return fail(Status::Decode, "truncated WKB geometry header");
    if (type & (ewkb_z | ewkb_m | ewkb_srid)) {
      h.d.z = (type & ewkb_z) != 0;
      h.d.m = (type & ewkb_m) != 0;
      if (type & ewkb_srid) {
        const std::uint32_t s = h.le ? r.u32le() : r.u32be();
        if (srid != nullptr && *srid == 0) *srid = static_cast<int>(s);
      }
      type &= 0x0FFFFFFFu;
    }
    if (type >= 1000 && type < 4000) {
      const std::uint32_t group = type / 1000;
      h.d.z = h.d.z || group == 1 || group == 3;
      h.d.m = h.d.m || group == 2 || group == 3;
      type %= 1000;
    }
    if (type < 1 || type > 17) return fail(Status::Decode, "unsupported WKB geometry type {}", type);
    h.base = type;
    return h;
  }
  std::uint32_t count(const Header& h) { return h.le ? r.u32le() : r.u32be(); }
  double f64(const Header& h) { return h.le ? r.f64le() : r.f64be(); }
  std::size_t width(const Header& h) const { return 16u + (h.d.z ? 8u : 0u) + (h.d.m ? 8u : 0u); }

  ir::Coord position(const Header& h) {
    ir::Coord c;
    c.lon = f64(h);
    c.lat = f64(h);
    if (h.d.z) c.ele = f64(h);
    if (h.d.m) c.m = f64(h);
    return c;
  }
  Result<std::vector<ir::Coord>> sequence(const Header& h) {
    const std::uint32_t n = count(h);
    CVT_TRY(check_declared(n, width(h), r.remaining(), "WKB points"));
    std::vector<ir::Coord> out;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) out.push_back(position(h));
    if (!r.ok()) return fail(Status::Decode, "truncated WKB coordinates");
    return out;
  }
  Result<ir::Polygon> polygon_body(const Header& h) {
    const std::uint32_t n = count(h);
    CVT_TRY(check_declared(n, 4, r.remaining(), "WKB rings"));
    ir::Polygon p;
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(std::vector<ir::Coord> ring, sequence(h));
      p.rings.push_back(std::move(ring));
    }
    return p;
  }

  Result<ir::Geometry> geometry(int depth) {
    if (depth > ir::max_geometry_depth) return fail(Status::Limit, "WKB geometry collections are nested too deeply");
    CVT_TRY_ASSIGN(const Header h, header());
    ir::Geometry g;
    switch (h.base) {
      case 1: {
        ir::Coord c = position(h);
        if (!r.ok()) return fail(Status::Decode, "truncated WKB point");
        if (std::isnan(c.lon) && std::isnan(c.lat)) {
          g.shape = ir::MultiPoint{};  // POINT EMPTY
        } else {
          g.shape = ir::Point{std::move(c)};
        }
        return g;
      }
      case 2: {
        CVT_TRY_ASSIGN(std::vector<ir::Coord> pts, sequence(h));
        g.shape = ir::LineString{std::move(pts)};
        return g;
      }
      case 3:
      case 17: {
        CVT_TRY_ASSIGN(ir::Polygon p, polygon_body(h));
        g.shape = std::move(p);
        return g;
      }
      default: break;
    }
    const std::uint32_t n = count(h);
    CVT_TRY(check_declared(n, 9, r.remaining(), "WKB members"));
    std::vector<ir::Geometry> members;
    members.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(ir::Geometry inner, geometry(depth + 1));
      members.push_back(std::move(inner));
    }
    if (!r.ok()) return fail(Status::Decode, "truncated WKB geometry");
    const auto expect = [&](auto probe, std::string_view what) -> Result<void> {
      for (const auto& m : members) {
        if (!probe(m)) return fail(Status::Decode, "a WKB {} holds a member of another type", what);
      }
      return {};
    };
    switch (h.base) {
      case 4: {
        ir::MultiPoint mp;
        for (auto& m : members) {
          if (auto* p = std::get_if<ir::Point>(&m.shape)) mp.points.push_back(std::move(p->coord));
          else if (!std::holds_alternative<ir::MultiPoint>(m.shape)) return fail(Status::Decode, "a WKB MultiPoint holds a non-point");
        }
        g.shape = std::move(mp);
        break;
      }
      case 5: {
        CVT_TRY(expect([](const ir::Geometry& m) { return std::holds_alternative<ir::LineString>(m.shape); },
                       "MultiLineString"));
        ir::MultiLineString ml;
        for (auto& m : members) ml.lines.push_back(std::move(std::get<ir::LineString>(m.shape)));
        g.shape = std::move(ml);
        break;
      }
      case 6:
      case 15:
      case 16: {
        CVT_TRY(expect([](const ir::Geometry& m) { return std::holds_alternative<ir::Polygon>(m.shape); },
                       "MultiPolygon"));
        ir::MultiPolygon mp;
        for (auto& m : members) mp.polygons.push_back(std::move(std::get<ir::Polygon>(m.shape)));
        g.shape = std::move(mp);
        break;
      }
      case 7: g.shape = ir::GeometryCollection{std::move(members)}; break;
      default: return fail(Status::Decode, "unsupported WKB geometry type {}", h.base);
    }
    return g;
  }
};

}  // namespace

std::string geometry_to_wkt(const ir::Geometry& geometry, int decimals, bool with_z, bool with_m) {
  WktWriter w;
  w.decimals = decimals;
  w.dims = dims_of(geometry);
  w.dims.z = w.dims.z && with_z;
  w.dims.m = w.dims.m && with_m;
  w.geometry(geometry, 0);
  return std::move(w.out);
}

Result<ir::Geometry> geometry_from_wkt(std::string_view text, int* srid, std::size_t* consumed) {
  WktParser p{text, 0};
  p.skip_ws();
  if (p.text.substr(p.pos).size() >= 5 && util::istarts_with(p.text.substr(p.pos), "SRID=")) {
    p.pos += 5;
    const std::size_t start = p.pos;
    while (p.pos < text.size() && (util::is_ascii_digit(text[p.pos]) || text[p.pos] == '-')) ++p.pos;
    const auto value = util::parse_int(text.substr(start, p.pos - start));
    if (!value || !p.eat(';')) return fail(Status::Decode, "malformed EWKT SRID prefix");
    if (srid != nullptr) *srid = static_cast<int>(*value);
  }
  CVT_TRY_ASSIGN(ir::Geometry g, p.geometry(0));
  p.skip_ws();
  if (consumed != nullptr) {
    *consumed = p.pos;
  } else if (p.pos != text.size()) {
    return fail(Status::Decode, "unexpected text after the WKT geometry at character {}", p.pos + 1);
  }
  return g;
}

Bytes geometry_to_wkb(const ir::Geometry& geometry, bool little_endian, std::optional<int> srid) {
  WkbWriter w;
  w.le = little_endian;
  w.dims = dims_of(geometry);
  w.geometry(geometry, srid, true, 0);
  return w.out.take();
}

Result<ir::Geometry> geometry_from_wkb(ByteView data, std::size_t* consumed, int* srid) {
  WkbParser p{util::ByteReader(data), srid};
  CVT_TRY_ASSIGN(ir::Geometry g, p.geometry(0));
  if (!p.r.ok()) return fail(Status::Decode, "truncated WKB geometry");
  if (consumed != nullptr) *consumed = p.r.position();
  return g;
}

// --------------------------------------------------------------------------- formats

namespace {

void set_srid(ir::Geo& geo, int srid) {
  if (srid > 0 && geo.crs.empty() && srid != 4326) geo.crs = "EPSG:" + std::to_string(srid);
}

Result<ir::Geo> read_wkt(ReadContext& ctx) {
  const std::string text = input_text(ctx.input);
  ir::Geo geo;
  std::size_t pos = 0;
  while (true) {
    while (pos < text.size() && (util::is_ascii_space(text[pos]) || text[pos] == ';')) ++pos;
    if (pos >= text.size()) break;
    if ((geo.features.size() & 1023) == 1023) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(text.size())));
    int srid = 0;
    std::size_t used = 0;
    CVT_TRY_ASSIGN(ir::Geometry g, geometry_from_wkt(std::string_view(text).substr(pos), &srid, &used));
    if (used == 0) return fail(Status::Decode, "no progress reading WKT");
    pos += used;
    set_srid(geo, srid);
    ir::Feature f;
    f.geometry = std::move(g);
    geo.features.push_back(std::move(f));
  }
  if (geo.features.empty()) return fail(Status::Decode, "the WKT file holds no geometry");
  return geo;
}

std::optional<int> srid_for(const ir::Geo& geo, bool want) {
  if (!want) return std::nullopt;
  if (geo.crs.empty()) return 4326;
  if (const auto crs = parse_crs(geo.crs)) return crs->epsg;
  return std::nullopt;
}

void warn_attributes(const ir::Geo& geo, const WriteContext& ctx) {
  for (const auto& f : geo.features) {
    if (!f.properties.empty()) {
      ctx.warn("WKT and WKB hold geometry only; feature attributes were not written.");
      return;
    }
  }
}

Result<Bytes> write_wkt(const ir::Geo& geo, WriteContext& ctx) {
  warn_attributes(geo, ctx);
  const auto srid = srid_for(geo, ctx.options.get_bool("ewkt", false));
  std::string out;
  for (const ir::Feature& f : geo.features) {
    if (srid) out += "SRID=" + std::to_string(*srid) + ";";
    out += geometry_to_wkt(f.geometry, -1, true, true);
    out += '\n';
  }
  return to_bytes(out);
}

Result<ir::Geo> read_wkb(ReadContext& ctx) {
  ir::Geo geo;
  std::size_t pos = 0;
  while (pos < ctx.input.size()) {
    int srid = 0;
    std::size_t used = 0;
    CVT_TRY_ASSIGN(ir::Geometry g, geometry_from_wkb(ctx.input.subspan(pos), &used, &srid));
    pos += used;
    set_srid(geo, srid);
    ir::Feature f;
    f.geometry = std::move(g);
    geo.features.push_back(std::move(f));
  }
  if (geo.features.empty()) return fail(Status::Decode, "the WKB file is empty");
  return geo;
}

Result<Bytes> write_wkb(const ir::Geo& geo, WriteContext& ctx) {
  warn_attributes(geo, ctx);
  const bool le = ctx.options.get_enum("byte_order", "little", {"little", "big"}) == "little";
  const auto srid = srid_for(geo, ctx.options.get_enum("wkb_flavor", "iso", {"iso", "ewkb"}) == "ewkb");
  if (geo.features.size() == 1) return geometry_to_wkb(geo.features[0].geometry, le, srid);
  if (geo.features.size() > 1) ctx.warn("WKB holds one geometry; the features were written as one GEOMETRYCOLLECTION.");
  ir::Geometry all;
  ir::GeometryCollection gc;
  for (const ir::Feature& f : geo.features) gc.geometries.push_back(f.geometry);
  all.shape = std::move(gc);
  return geometry_to_wkb(all, le, srid);
}

Result<ir::Geo> read_wkb_hex(ReadContext& ctx) {
  const std::string text = input_text(ctx.input);
  ir::Geo geo;
  for (std::string_view line : util::split_lines(text)) {
    line = util::trim(line);
    if (line.starts_with("\\x")) line.remove_prefix(2);
    if (line.empty()) continue;
    CVT_TRY_ASSIGN(const Bytes bytes, util::hex_decode(line));
    int srid = 0;
    std::size_t used = 0;
    CVT_TRY_ASSIGN(ir::Geometry g, geometry_from_wkb(bytes, &used, &srid));
    if (used != bytes.size()) return fail(Status::Decode, "unexpected bytes after a hex WKB geometry");
    set_srid(geo, srid);
    ir::Feature f;
    f.geometry = std::move(g);
    geo.features.push_back(std::move(f));
  }
  if (geo.features.empty()) return fail(Status::Decode, "the file holds no hex WKB geometry");
  return geo;
}

Result<Bytes> write_wkb_hex(const ir::Geo& geo, WriteContext& ctx) {
  warn_attributes(geo, ctx);
  const bool le = ctx.options.get_enum("byte_order", "little", {"little", "big"}) == "little";
  const auto srid = srid_for(geo, ctx.options.get_enum("wkb_flavor", "ewkb", {"iso", "ewkb"}) == "ewkb");
  std::string out;
  for (const ir::Feature& f : geo.features) {
    out += util::hex_encode(geometry_to_wkb(f.geometry, le, srid), true);
    out += '\n';
  }
  return to_bytes(out);
}

int sniff_wkt(ByteView data, std::string_view) {
  std::string_view head = text_head(data, 256);
  if (util::istarts_with(head, "SRID=")) {
    const std::size_t semi = head.find(';');
    if (semi == std::string_view::npos) return 0;
    head = util::trim_left(head.substr(semi + 1));
  }
  for (std::string_view type : {"GEOMETRYCOLLECTION", "MULTIPOLYGON", "MULTILINESTRING", "MULTIPOINT", "POLYGON",
                                "LINESTRING", "POINT", "TRIANGLE", "TIN", "POLYHEDRALSURFACE"}) {
    if (!util::istarts_with(head, type)) continue;
    std::string_view rest = head.substr(type.size());
    while (!rest.empty() && (rest.front() == 'Z' || rest.front() == 'M' || rest.front() == 'z' || rest.front() == 'm')) {
      rest.remove_prefix(1);
    }
    rest = util::trim_left(rest);
    if (rest.starts_with('(') || util::istarts_with(rest, "EMPTY") || util::istarts_with(rest, "Z ") ||
        util::istarts_with(rest, "M ") || util::istarts_with(rest, "ZM")) {
      return 90;
    }
  }
  return 0;
}

int sniff_wkb(ByteView data, std::string_view) {
  if (data.size() < 9 || data.size() > (64u << 20)) return 0;
  const auto b0 = std::to_integer<std::uint8_t>(data[0]);
  if (b0 > 1) return 0;
  std::size_t pos = 0;
  int geometries = 0;
  while (pos < data.size() && geometries < 1000) {
    std::size_t used = 0;
    if (!geometry_from_wkb(data.subspan(pos), &used) || used == 0) return 0;
    pos += used;
    ++geometries;
  }
  return pos == data.size() ? 80 : 0;
}

int sniff_wkb_hex(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 65536);
  const std::size_t eol = head.find_first_of("\r\n");
  std::string_view line = util::trim(head.substr(0, eol));
  if (line.starts_with("\\x")) line.remove_prefix(2);
  if (line.size() < 18 || line.size() % 2 != 0 || !(line.starts_with("00") || line.starts_with("01"))) return 0;
  if (line.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos) return 0;
  const auto bytes = util::hex_decode(line);
  if (!bytes) return 0;
  std::size_t used = 0;
  if (!geometry_from_wkb(*bytes, &used) || used != bytes->size()) return 0;
  return 88;
}

}  // namespace

void register_wkt(Registry& registry) {
  FormatInfo wkt;
  wkt.id = "wkt";
  wkt.name = "Well-Known Text geometry";
  wkt.description = "OGC Well-Known Text (and PostGIS EWKT) geometries, one per line.";
  wkt.category = Category::Geo;
  wkt.extensions = {"wkt", "ewkt"};
  wkt.mime = "text/plain";
  wkt.popularity = 40;
  wkt.sniff = sniff_wkt;
  registry.add_format(std::move(wkt));
  GeoCodecSpec spec;
  spec.format_id = "wkt";
  spec.read = read_wkt;
  spec.write = write_wkt;
  spec.lossy_write = true;
  spec.traits = GeoWriterTraits{true, false, false, false};
  spec.write_options = {OptionSpec::boolean("ewkt", "EWKT SRID prefix", false,
                                            "Start every geometry with the PostGIS \"SRID=n;\" prefix.")};
  add_geo_codec(registry, std::move(spec));

  FormatInfo wkb;
  wkb.id = "wkb";
  wkb.name = "Well-Known Binary geometry";
  wkb.description = "OGC Well-Known Binary (and PostGIS EWKB) geometry.";
  wkb.category = Category::Geo;
  wkb.extensions = {"wkb", "ewkb"};
  wkb.mime = "application/octet-stream";
  wkb.popularity = 30;
  wkb.sniff = sniff_wkb;
  registry.add_format(std::move(wkb));
  const auto binary_options = [](std::string_view flavor) {
    return std::vector<OptionSpec>{
        OptionSpec::choice("byte_order", "Byte order", "little", {"little", "big"},
                           "Little-endian (NDR) or big-endian (XDR) numbers.")
            .advanced(),
        OptionSpec::choice("wkb_flavor", "Flavour", std::string(flavor), {"iso", "ewkb"},
                           "ISO WKB type codes, or PostGIS EWKB with the SRID embedded."),
    };
  };
  GeoCodecSpec wkb_spec;
  wkb_spec.format_id = "wkb";
  wkb_spec.read = read_wkb;
  wkb_spec.write = write_wkb;
  wkb_spec.lossy_write = true;
  wkb_spec.traits = GeoWriterTraits{true, false, false, false};
  wkb_spec.write_options = binary_options("iso");
  add_geo_codec(registry, std::move(wkb_spec));

  FormatInfo hex;
  hex.id = "wkb-hex";
  hex.name = "Hex-encoded WKB geometry";
  hex.description = "Well-Known Binary geometries as hexadecimal text, one per line (PostGIS output).";
  hex.category = Category::Geo;
  hex.extensions = {"hexwkb", "wkbhex"};
  hex.mime = "text/plain";
  hex.popularity = 20;
  hex.sniff = sniff_wkb_hex;
  registry.add_format(std::move(hex));
  GeoCodecSpec hex_spec;
  hex_spec.format_id = "wkb-hex";
  hex_spec.read = read_wkb_hex;
  hex_spec.write = write_wkb_hex;
  hex_spec.lossy_write = true;
  hex_spec.traits = GeoWriterTraits{true, false, false, false};
  hex_spec.write_options = binary_options("ewkb");
  add_geo_codec(registry, std::move(hex_spec));
}

}  // namespace cvt::geo
