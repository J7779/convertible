// Convertible core - geo family: MapInfo Interchange Format (MIF / MID).
//
// `mif`: a lone .mif file (geometry and column definitions; the attribute
// values live in the separate .mid file, so they are missing).
// `mapinfo.zip`: a ZIP with a .mif and its .mid, read with attributes and
// written that way. Objects: Point, Line, Pline (Multiple), Region (rings
// grouped into polygons and holes by containment), Multipoint, Rect,
// RoundRect, Collection; Text, Arc and Ellipse are skipped with a note.
// Pen / Brush / Symbol colours become the feature style. Coordinates in
// "CoordSys Earth Projection 1, 104" are WGS 84; other systems are kept
// with a note. Native binary TAB files (.tab/.map/.dat/.id) are recognised
// but not decoded.
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
#include "engine/standard_options.hpp"
#include "families/geo/geo.hpp"
#include "families/geo/geo_ops.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::geo {

namespace {

struct MifColumn {
  std::string name;
  std::string type;  // lower-case: char, integer, smallint, decimal, float, date, logical, ...
};

struct MifHeader {
  char delimiter = '\t';
  std::optional<util::TextEncoding> encoding;
  bool wgs84 = true;
  std::string coordsys;
  std::vector<MifColumn> columns;
};

struct Lines {
  std::vector<std::string_view> lines;
  std::size_t at = 0;
  bool done() const { return at >= lines.size(); }
  std::string_view peek() const { return done() ? std::string_view() : util::trim(lines[at]); }
  std::string_view next() { return done() ? std::string_view() : util::trim(lines[at++]); }
};

std::vector<std::string_view> words(std::string_view line) { return util::split(line, ' ', false); }

std::string decode(std::string_view raw, std::optional<util::TextEncoding> encoding) {
  if (!encoding) {
    if (util::is_valid_utf8(raw)) return std::string(raw);
    return util::decode_text(as_bytes(raw), util::TextEncoding::Windows1252).text;
  }
  if (*encoding == util::TextEncoding::Utf8) return util::sanitize_utf8(raw);
  return util::decode_text(as_bytes(raw), *encoding).text;
}

Result<MifHeader> read_header(Lines& in) {
  MifHeader h;
  bool data = false;
  while (!in.done()) {
    const std::string_view line = in.next();
    if (line.empty()) continue;
    const auto w = words(line);
    const std::string key = util::to_lower(w[0]);
    if (key == "data") {
      data = true;
      break;
    }
    if (key == "delimiter" && line.size() > 9) {
      const std::size_t q = line.find('"');
      if (q != std::string_view::npos && q + 1 < line.size()) h.delimiter = line[q + 1];
    } else if (key == "charset") {
      const std::string cs = util::to_lower(line.substr(line.find(' ') + 1));
      if (cs.find("utf-8") != std::string::npos || cs.find("utf8") != std::string::npos) h.encoding = util::TextEncoding::Utf8;
      else if (cs.find("windowslatin1") != std::string::npos) h.encoding = util::TextEncoding::Windows1252;
      else if (cs.find("8859_1") != std::string::npos || cs.find("8859-1") != std::string::npos) h.encoding = util::TextEncoding::Latin1;
      // "Neutral" and others: detect.
    } else if (key == "coordsys") {
      h.coordsys = std::string(line);
      std::string compact;
      for (char c : util::to_lower(line)) {
        if (c != ' ') compact += c;
      }
      h.wgs84 = compact.starts_with("coordsysearthprojection1,104") || compact.starts_with("coordsysearthprojection1,0");
    } else if (key == "columns") {
      const auto n = w.size() > 1 ? util::parse_int(w[1]) : std::nullopt;
      if (!n || *n < 0 || *n > 4096) return fail(Status::Decode, "invalid MIF column count");
      for (std::int64_t i = 0; i < *n; ++i) {
        const auto cw = words(in.next());
        if (cw.size() < 2) return fail(Status::Decode, "invalid MIF column definition");
        std::string type = util::to_lower(cw[1]);
        type = type.substr(0, type.find('('));
        h.columns.push_back({std::string(cw[0]), type});
      }
    }
  }
  if (!data) return fail(Status::Decode, "the MIF file has no Data section");
  return h;
}

Result<ir::Coord> read_xy(std::string_view a, std::string_view b) {
  const auto x = parse_number(a);
  const auto y = parse_number(b);
  if (!x || !y) return fail(Status::Decode, "invalid MIF coordinate");
  ir::Coord c;
  c.lon = *x;
  c.lat = *y;
  return c;
}

Result<std::vector<ir::Coord>> read_points(Lines& in, std::uint64_t n) {
  if (n > in.lines.size() - std::min(in.at, in.lines.size())) {
    return fail(Status::Decode, "a MIF object declares more points than the file holds");
  }
  std::vector<ir::Coord> out;
  out.reserve(static_cast<std::size_t>(n));
  // Coordinates are one pair per line, but some writers put several pairs on a line.
  std::vector<std::string_view> pending;
  while (out.size() < n) {
    if (pending.size() < 2) {
      if (in.done()) return fail(Status::Decode, "a MIF object is truncated");
      for (auto v : words(in.next())) pending.push_back(v);
      continue;
    }
    CVT_TRY_ASSIGN(ir::Coord c, read_xy(pending[0], pending[1]));
    pending.erase(pending.begin(), pending.begin() + 2);
    out.push_back(c);
  }
  return out;
}

std::uint64_t count_of(std::string_view w) {
  const auto v = util::parse_int(w);
  return v && *v > 0 ? static_cast<std::uint64_t>(*v) : 0;
}

// Rings grouped by containment depth: even depth = outer, odd = hole.
ir::Geometry region(std::vector<std::vector<ir::Coord>> rings) {
  for (auto& r : rings) {
    if (!r.empty() && !(r.front().lon == r.back().lon && r.front().lat == r.back().lat)) r.push_back(r.front());
  }
  std::vector<std::vector<ir::Coord>> oriented;
  for (std::size_t i = 0; i < rings.size(); ++i) {
    if (rings[i].empty()) continue;
    int depth = 0;
    for (std::size_t j = 0; j < rings.size(); ++j) {
      if (i == j || rings[j].size() < 4) continue;
      // Point in polygon of rings[j].
      bool in = false;
      const ir::Coord& p = rings[i][0];
      const auto& r = rings[j];
      for (std::size_t a = 0, b = r.size() - 1; a < r.size(); b = a++) {
        if ((r[a].lat > p.lat) != (r[b].lat > p.lat) &&
            p.lon < (r[b].lon - r[a].lon) * (p.lat - r[a].lat) / (r[b].lat - r[a].lat) + r[a].lon) {
          in = !in;
        }
      }
      if (in) ++depth;
    }
    std::vector<ir::Coord> ring = rings[i];
    const bool outer = depth % 2 == 0;
    if ((signed_area(ring) > 0) != outer) std::reverse(ring.begin(), ring.end());  // outer CCW, holes CW
    oriented.push_back(std::move(ring));
  }
  return assemble_rings(std::move(oriented), false);
}

std::string pen_color(std::string_view line) {
  // Pen (width, pattern, colour) / Brush (pattern, forecolour, backcolour) / Symbol (shape, colour, size)
  const std::size_t open = line.find('(');
  const std::size_t close = line.find(')');
  if (open == std::string_view::npos || close == std::string_view::npos || close < open) return {};
  const auto parts = util::split(line.substr(open + 1, close - open - 1), ',');
  const std::string lower = util::to_lower(line.substr(0, open));
  const std::size_t index = lower.starts_with("pen") ? 2 : 1;
  if (parts.size() <= index) return {};
  if (lower.starts_with("brush") && util::trim(parts[0]) == "1") return {};  // pattern 1: hollow, no fill
  const auto value = util::parse_int(util::trim(parts[index]));
  if (!value || *value < 0 || *value > 0xFFFFFF) return {};
  return std::format("#{:06x}", *value);
}

Result<std::optional<ir::Geometry>> read_object(Lines& in, ir::Style& style, bool& skipped, int depth) {
  if (depth > 4) return fail(Status::Limit, "MIF collections are nested too deeply");
  const auto w = words(in.next());
  if (w.empty()) return std::optional<ir::Geometry>{};
  const std::string kind = util::to_lower(w[0]);
  std::optional<ir::Geometry> g;
  if (kind == "none") {
    g = ir::Geometry{ir::GeometryCollection{}};
  } else if (kind == "point" && w.size() >= 3) {
    CVT_TRY_ASSIGN(ir::Coord c, read_xy(w[1], w[2]));
    g = ir::Geometry{ir::Point{c}};
  } else if (kind == "line" && w.size() >= 5) {
    CVT_TRY_ASSIGN(ir::Coord a, read_xy(w[1], w[2]));
    CVT_TRY_ASSIGN(ir::Coord b, read_xy(w[3], w[4]));
    g = ir::Geometry{ir::LineString{{a, b}}};
  } else if (kind == "pline") {
    if (w.size() >= 2 && util::iequals(w[1], "multiple")) {
      const std::uint64_t sections = w.size() >= 3 ? count_of(w[2]) : 0;
      ir::MultiLineString ml;
      for (std::uint64_t s = 0; s < sections; ++s) {
        CVT_TRY_ASSIGN(auto pts, read_points(in, count_of(in.next())));
        ml.lines.push_back(ir::LineString{std::move(pts)});
      }
      g = ir::Geometry{std::move(ml)};
    } else {
      const std::uint64_t n = w.size() >= 2 ? count_of(w[1]) : count_of(in.next());
      CVT_TRY_ASSIGN(auto pts, read_points(in, n));
      g = ir::Geometry{ir::LineString{std::move(pts)}};
    }
  } else if (kind == "region" && w.size() >= 2) {
    const std::uint64_t n = count_of(w[1]);
    if (n > in.lines.size()) return fail(Status::Decode, "a MIF region declares too many rings");
    std::vector<std::vector<ir::Coord>> rings;
    for (std::uint64_t r = 0; r < n; ++r) {
      CVT_TRY_ASSIGN(auto pts, read_points(in, count_of(in.next())));
      rings.push_back(std::move(pts));
    }
    g = region(std::move(rings));
  } else if (kind == "multipoint" && w.size() >= 2) {
    CVT_TRY_ASSIGN(auto pts, read_points(in, count_of(w[1])));
    g = ir::Geometry{ir::MultiPoint{std::move(pts)}};
  } else if ((kind == "rect" || kind == "roundrect") && w.size() >= 5) {
    CVT_TRY_ASSIGN(ir::Coord a, read_xy(w[1], w[2]));
    CVT_TRY_ASSIGN(ir::Coord b, read_xy(w[3], w[4]));
    ir::Coord p2 = a, p4 = a;
    p2.lon = b.lon;
    p4.lat = b.lat;
    g = region({{a, p2, b, p4, a}});
  } else if (kind == "collection" && w.size() >= 2) {
    ir::GeometryCollection gc;
    const std::uint64_t n = count_of(w[1]);
    for (std::uint64_t i = 0; i < n && i < 64; ++i) {
      CVT_TRY_ASSIGN(auto inner, read_object(in, style, skipped, depth + 1));
      if (inner) gc.geometries.push_back(std::move(*inner));
    }
    g = ir::Geometry{std::move(gc)};
  } else if (kind == "text" || kind == "arc" || kind == "ellipse") {
    skipped = true;
    g = ir::Geometry{ir::GeometryCollection{}};
  } else {
    return fail(Status::Decode, "unknown MIF object '{}'", util::sanitize_utf8(w[0].substr(0, 30)));
  }
  // Style clauses and other indented lines until the next object.
  while (!in.done()) {
    const std::string_view next = in.peek();
    const std::string lower = util::to_lower(next.substr(0, std::min<std::size_t>(next.size(), 8)));
    if (lower.starts_with("pen")) {
      style.stroke = pen_color(next);
    } else if (lower.starts_with("brush")) {
      style.fill = pen_color(next);
    } else if (lower.starts_with("symbol")) {
      style.marker_color = pen_color(next);
    } else if (!(lower.starts_with("smooth") || lower.starts_with("center") || lower.starts_with("font") ||
                 lower.starts_with("spacing") || lower.starts_with("justify") || lower.starts_with("angle") ||
                 lower.starts_with("label") || lower.starts_with("\"") || next.empty())) {
      break;
    }
    in.next();
  }
  return g;
}

// MID: delimiter-separated values, strings in double quotes ("" escapes a quote).
std::vector<std::string> mid_fields(std::string_view line, char delimiter) {
  std::vector<std::string> out;
  std::string field;
  bool quoted = false, started = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          field += '"';
          ++i;
        } else {
          quoted = false;
        }
      } else {
        field += c;
      }
    } else if (c == '"' && !started) {
      quoted = started = true;
    } else if (c == delimiter) {
      out.push_back(std::move(field));
      field.clear();
      started = false;
    } else {
      field += c;
      started = true;
    }
  }
  out.push_back(std::move(field));
  return out;
}

Json mid_value(const std::string& raw, const MifColumn& column) {
  const std::string_view t = util::trim(raw);
  if (column.type == "char") return Json(raw);
  if (t.empty()) return Json();
  if (column.type == "integer" || column.type == "smallint" || column.type == "largeint") {
    if (auto v = util::parse_int(t)) return Json(*v);
  } else if (column.type == "float" || column.type == "decimal") {
    if (auto v = parse_number(t)) return Json(*v);
  } else if (column.type == "logical") {
    return Json(t == "T" || t == "t" || t == "1" || util::iequals(t, "true"));
  }
  return Json(raw);
}

Result<ir::Geo> parse_mif(std::string_view mif, std::optional<std::string_view> mid, const Context& ctx) {
  const std::string mif_text(mif);
  Lines in;
  in.lines = util::split_lines(mif_text);
  CVT_TRY_ASSIGN(MifHeader h, read_header(in));
  ir::Geo geo;
  if (!h.wgs84) geo.crs = h.coordsys;
  std::vector<std::vector<std::string>> rows;
  std::string mid_text;
  if (mid) {
    mid_text = std::string(*mid);
    for (std::string_view line : util::split_lines(mid_text)) {
      if (rows.size() > 10'000'000) return fail(Status::Limit, "the MID file has too many rows");
      rows.push_back(mid_fields(line, h.delimiter));
    }
  }
  bool skipped = false;
  std::size_t index = 0;
  while (!in.done()) {
    if (in.peek().empty()) {
      in.next();
      continue;
    }
    if ((index & 1023) == 1023) CVT_TRY(ctx.checkpoint(static_cast<double>(in.at) / static_cast<double>(in.lines.size())));
    ir::Feature f;
    CVT_TRY_ASSIGN(auto g, read_object(in, f.style, skipped, 0));
    if (!g) continue;
    f.geometry = std::move(*g);
    if (index < rows.size()) {
      for (std::size_t c = 0; c < h.columns.size() && c < rows[index].size(); ++c) {
        f.properties.set(h.columns[c].name, mid_value(decode(rows[index][c], h.encoding), h.columns[c]));
      }
    }
    if (const Json* name = f.properties.find("name"); name != nullptr && name->is_string()) f.name = name->as_string();
    geo.features.push_back(std::move(f));
    ++index;
  }
  if (skipped) ctx.warn("MapInfo text, arc and ellipse objects were left out.");
  if (!mid && !h.columns.empty()) {
    ctx.warn("The attribute values of a MIF file are in its .mid file; convert a ZIP of both to keep them.");
  }
  return geo;
}

Result<ir::Geo> read_mif(ReadContext& ctx) { return parse_mif(input_text(ctx.input), std::nullopt, ctx); }

Result<ir::Geo> read_mapinfo_zip(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
  std::optional<std::size_t> mif, mid;
  bool tab = false;
  for (std::size_t i = 0; i < zip.size(); ++i) {
    const std::string& name = zip.entries()[i].name;
    if (name.starts_with("__MACOSX/")) continue;
    if (util::iends_with(name, ".mif") && !mif) mif = i;
    if (util::iends_with(name, ".tab")) tab = true;
  }
  if (!mif) {
    if (tab) {
      return fail(Status::Decode, "native MapInfo TAB files (.tab/.map/.dat) are not supported; export the layer as MIF/MID");
    }
    return fail(Status::Decode, "the ZIP archive contains no MapInfo .mif file");
  }
  const std::string stem = zip.entries()[*mif].name.substr(0, zip.entries()[*mif].name.size() - 4);
  for (std::size_t i = 0; i < zip.size(); ++i) {
    const std::string& name = zip.entries()[i].name;
    if (name.size() == stem.size() + 4 && util::iequals(name.substr(0, stem.size()), stem) && util::iends_with(name, ".mid")) mid = i;
  }
  CVT_TRY_ASSIGN(const Bytes mif_bytes, zip.read(*mif));
  Bytes mid_bytes;
  if (mid) {
    CVT_TRY_ASSIGN(mid_bytes, zip.read(*mid));
  }
  CVT_TRY_ASSIGN(ir::Geo geo, parse_mif(input_text(mif_bytes), mid ? std::optional<std::string_view>(as_chars(mid_bytes)) : std::nullopt, ctx));
  const std::size_t slash = stem.rfind('/');
  geo.name = slash == std::string::npos ? stem : stem.substr(slash + 1);
  return geo;
}

// --------------------------------------------------------------------------- writing

std::string mif_number(double v) { return format_number(v); }

void write_points(std::string& out, const std::vector<ir::Coord>& pts) {
  for (const ir::Coord& c : pts) out += mif_number(c.lon) + " " + mif_number(c.lat) + "\n";
}

void write_region(std::string& out, const std::vector<const std::vector<ir::Coord>*>& rings) {
  out += std::format("Region {}\n", rings.size());
  for (const auto* r : rings) {
    out += std::format("  {}\n", r->size());
    write_points(out, *r);
  }
}

bool write_mif_object(std::string& out, const ir::Geometry& g) {
  bool ok = true;
  std::visit(
      [&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, ir::Point>) {
          out += "Point " + mif_number(s.coord.lon) + " " + mif_number(s.coord.lat) + "\n";
        } else if constexpr (std::is_same_v<T, ir::MultiPoint>) {
          out += std::format("Multipoint {}\n", s.points.size());
          write_points(out, s.points);
        } else if constexpr (std::is_same_v<T, ir::LineString>) {
          out += std::format("Pline {}\n", s.points.size());
          write_points(out, s.points);
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          out += std::format("Pline Multiple {}\n", s.lines.size());
          for (const auto& l : s.lines) {
            out += std::format("  {}\n", l.points.size());
            write_points(out, l.points);
          }
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          std::vector<const std::vector<ir::Coord>*> rings;
          for (const auto& r : s.rings) rings.push_back(&r);
          write_region(out, rings);
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          std::vector<const std::vector<ir::Coord>*> rings;
          for (const auto& p : s.polygons) {
            for (const auto& r : p.rings) rings.push_back(&r);
          }
          write_region(out, rings);
        } else {
          out += "none\n";
          ok = s.geometries.empty();
        }
      },
      g.shape);
  return ok;
}

std::string mid_text(const Json& v, const std::string& type, char delimiter) {
  if (v.is_null()) return type == "char" ? "\"\"" : "";
  if (type == "logical") return v.as_bool() ? "T" : "F";
  if (type == "char") {
    std::string s = json_to_text(v);
    std::string out = "\"";
    for (char c : s) {
      if (c == '"') out += '"';
      if (c == '\n' || c == '\r') c = ' ';
      out += c;
    }
    return out + "\"";
  }
  (void)delimiter;
  return json_to_text(v);
}

Result<Bytes> write_mapinfo_zip(const ir::Geo& geo, WriteContext& ctx) {
  // Columns from the property types.
  std::vector<MifColumn> columns;
  std::vector<std::size_t> widths;
  for (const auto& f : geo.features) {
    for (const auto& m : f.properties.items()) {
      const Json& v = m.value();
      std::string type = v.is_bool() ? "logical" : v.is_integer() ? "integer" : v.is_float() ? "float" : "char";
      if (v.is_integer() && (v.as_int64() > 2147483647 || v.as_int64() < -2147483647)) type = "float";
      auto it = std::find_if(columns.begin(), columns.end(), [&](const MifColumn& c) { return c.name == m.key(); });
      const std::size_t width = json_to_text(v).size();
      if (it == columns.end()) {
        columns.push_back({std::string(m.key()), v.is_null() ? std::string() : type});
        widths.push_back(width);
        continue;
      }
      const std::size_t i = static_cast<std::size_t>(it - columns.begin());
      widths[i] = std::max(widths[i], width);
      if (v.is_null() || it->type == type) continue;
      if (it->type.empty()) it->type = type;
      else if ((it->type == "integer" && type == "float") || (it->type == "float" && type == "integer")) it->type = "float";
      else it->type = "char";
    }
  }
  std::string mif = "Version 300\nCharset \"UTF-8\"\nDelimiter \",\"\nCoordSys Earth Projection 1, 104\n";
  mif += std::format("Columns {}\n", columns.size());
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].type.empty()) columns[i].type = "char";
    std::string name;
    for (char c : columns[i].name) name += (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_') ? c : '_';
    if (name.empty() || util::is_ascii_digit(name.front())) name.insert(name.begin(), '_');
    const std::string& t = columns[i].type;
    const std::string decl = t == "char" ? std::format("Char({})", std::clamp<std::size_t>(widths[i], 1, 254))
                             : t == "integer" ? std::string("Integer")
                             : t == "logical" ? std::string("Logical")
                                              : std::string("Float");
    mif += "  " + name + " " + decl + "\n";
  }
  mif += "Data\n\n";
  std::string mid;
  bool collections = false;
  for (const auto& f : geo.features) {
    collections = !write_mif_object(mif, f.geometry) || collections;
    for (std::size_t i = 0; i < columns.size(); ++i) {
      if (i) mid += ',';
      mid += mid_text(f.properties[columns[i].name], columns[i].type, ',');
    }
    mid += "\n";
  }
  if (collections) ctx.warn("MIF 300 has no geometry collections; such features were written without geometry.");
  const std::string stem = geo.name.empty() ? std::string("layer") : geo.name;
  std::string safe;
  for (char c : stem) safe += (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '-') ? c : '_';
  util::ZipWriter zip;
  util::ZipEntryOptions options;
  options.level = options::resolved_compression_level(ctx.options);
  CVT_TRY(zip.add(safe + ".mif", mif, options));
  CVT_TRY(zip.add(safe + ".mid", mid, options));
  return zip.finish();
}

int sniff_mif(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 2048);
  if (!util::istarts_with(head, "Version")) return 0;
  const std::string lower = util::to_lower(head);
  return lower.find("\ncolumns") != std::string::npos || lower.find("\ndata") != std::string::npos ? 90 : 0;
}

int sniff_mapinfo_zip(ByteView data, std::string_view) {
  return zip_has_entry_with_extension(data, ".mif") || zip_has_entry_with_extension(data, ".tab") ? 94 : 0;
}

}  // namespace

void register_mapinfo(Registry& registry) {
  FormatInfo mif;
  mif.id = "mif";
  mif.name = "MapInfo Interchange Format";
  mif.description = "MapInfo MIF geometry file (its attributes are in a separate .mid file).";
  mif.category = Category::Geo;
  mif.extensions = {"mif"};
  mif.mime = "application/x-mapinfo-mif";
  mif.popularity = 20;
  mif.sniff = sniff_mif;
  registry.add_format(std::move(mif));
  GeoCodecSpec lone;
  lone.format_id = "mif";
  lone.read = read_mif;
  add_geo_codec(registry, std::move(lone));

  FormatInfo zip;
  zip.id = "mapinfo.zip";
  zip.name = "Zipped MapInfo layer";
  zip.description = "A MapInfo layer (MIF + MID) in a ZIP archive.";
  zip.category = Category::Geo;
  zip.extensions = {"mapinfo.zip", "zip"};
  zip.mime = "application/zip";
  zip.popularity = 20;
  zip.sniff = sniff_mapinfo_zip;
  registry.add_format(std::move(zip));
  GeoCodecSpec spec;
  spec.format_id = "mapinfo.zip";
  spec.read = read_mapinfo_zip;
  spec.write = write_mapinfo_zip;
  spec.write_options = {options::compression_level()};
  spec.lossy_write = true;
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
