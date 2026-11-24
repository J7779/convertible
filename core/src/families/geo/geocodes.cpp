// Convertible core - geo family: geohashes and Open Location Codes (Plus Codes).
//
// One code per line, read as a Point at the centre of the code's cell with
// the code and the cell size (degrees) as properties. The writers encode
// every point (and the bounding-box centre of other geometries, with a note).
// Only full Plus Codes can be decoded; short codes need a reference place.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
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
#include "families/geo/geo_ops.hpp"
#include "util/text.hpp"

namespace cvt::geo {

namespace {

// --------------------------------------------------------------------------- geohash

constexpr std::string_view geohash_alphabet = "0123456789bcdefghjkmnpqrstuvwxyz";

struct Cell {
  double south, west, north, east;
};

std::optional<Cell> geohash_decode(std::string_view code) {
  if (code.empty() || code.size() > 22) return std::nullopt;
  Cell cell{-90, -180, 90, 180};
  bool even = true;  // longitude bit first
  for (char c : code) {
    const std::size_t v = geohash_alphabet.find(util::ascii_lower(c));
    if (v == std::string_view::npos) return std::nullopt;
    for (int bit = 4; bit >= 0; --bit) {
      const bool on = (v >> bit) & 1;
      if (even) {
        const double mid = (cell.west + cell.east) / 2;
        (on ? cell.west : cell.east) = mid;
      } else {
        const double mid = (cell.south + cell.north) / 2;
        (on ? cell.south : cell.north) = mid;
      }
      even = !even;
    }
  }
  return cell;
}

std::string geohash_encode(double lat, double lon, int length) {
  Cell cell{-90, -180, 90, 180};
  std::string out;
  bool even = true;
  int bits = 0;
  unsigned value = 0;
  while (static_cast<int>(out.size()) < length) {
    if (even) {
      const double mid = (cell.west + cell.east) / 2;
      value <<= 1;
      if (lon >= mid) {
        value |= 1;
        cell.west = mid;
      } else {
        cell.east = mid;
      }
    } else {
      const double mid = (cell.south + cell.north) / 2;
      value <<= 1;
      if (lat >= mid) {
        value |= 1;
        cell.south = mid;
      } else {
        cell.north = mid;
      }
    }
    even = !even;
    if (++bits == 5) {
      out += geohash_alphabet[value];
      bits = 0;
      value = 0;
    }
  }
  return out;
}

// --------------------------------------------------------------------------- Open Location Code

constexpr std::string_view olc_alphabet = "23456789CFGHJMPQRVWX";
constexpr int olc_pair_length = 10;
constexpr int olc_max_digits = 15;
constexpr std::int64_t olc_final_lat = 8000LL * 3125;  // 20^3 * 5^5
constexpr std::int64_t olc_final_lng = 8000LL * 1024;  // 20^3 * 4^5

double olc_lat_precision(int length) {
  if (length <= olc_pair_length) return std::pow(20.0, std::floor(length / -2.0 + 2));
  return std::pow(20.0, -3) / std::pow(5.0, length - olc_pair_length);
}

std::string olc_encode(double lat, double lon, int length) {
  length = std::clamp(length, 2, olc_max_digits);
  if (length < olc_pair_length && length % 2 == 1) ++length;
  lat = std::clamp(lat, -90.0, 90.0);
  if (lat == 90.0) lat -= olc_lat_precision(length);
  while (lon < -180.0) lon += 360.0;
  while (lon >= 180.0) lon -= 360.0;
  std::int64_t lat_val = static_cast<std::int64_t>(std::floor(std::round((lat + 90.0) * olc_final_lat * 1e6) / 1e6));
  std::int64_t lng_val = static_cast<std::int64_t>(std::floor(std::round((lon + 180.0) * olc_final_lng * 1e6) / 1e6));
  std::string code;
  if (length > olc_pair_length) {
    for (int i = 0; i < olc_max_digits - olc_pair_length; ++i) {
      const auto ndx = static_cast<std::size_t>((lat_val % 5) * 4 + (lng_val % 4));
      code.insert(code.begin(), olc_alphabet[ndx]);
      lat_val /= 5;
      lng_val /= 4;
    }
  } else {
    lat_val /= 3125;
    lng_val /= 1024;
  }
  for (int i = 0; i < olc_pair_length / 2; ++i) {
    code.insert(code.begin(), olc_alphabet[static_cast<std::size_t>(lng_val % 20)]);
    code.insert(code.begin(), olc_alphabet[static_cast<std::size_t>(lat_val % 20)]);
    lat_val /= 20;
    lng_val /= 20;
    if (i == 0) code.insert(code.begin(), '+');
  }
  // code is now 10 pair digits with '+' after eight, followed by the grid digits.
  if (length >= 8) return code.substr(0, static_cast<std::size_t>(length) + 1);
  std::string padded = code.substr(0, static_cast<std::size_t>(length));
  padded.append(static_cast<std::size_t>(8 - length), '0');
  padded += '+';
  return padded;
}

std::optional<Cell> olc_decode(std::string_view code) {
  const std::size_t plus = code.find('+');
  if (plus != 8 || code.find('+', plus + 1) != std::string_view::npos) return std::nullopt;
  std::string digits;
  for (char c : code) {
    if (c == '+') continue;
    if (c == '0') break;  // padding
    const char u = util::ascii_upper(c);
    if (olc_alphabet.find(u) == std::string_view::npos) return std::nullopt;
    digits += u;
  }
  if (digits.size() < 2 || digits.size() > olc_max_digits) return std::nullopt;
  // The first digit pair must stay within the globe.
  if (olc_alphabet.find(digits[0]) * 20 >= 180 || olc_alphabet.find(digits[1]) * 20 >= 360) return std::nullopt;
  double south = -90, west = -180, lat_res = 400, lng_res = 400;
  std::size_t i = 0;
  for (; i < digits.size() && i < olc_pair_length; i += 2) {
    lat_res /= 20;
    lng_res /= 20;
    south += static_cast<double>(olc_alphabet.find(digits[i])) * lat_res;
    if (i + 1 >= digits.size()) return std::nullopt;
    west += static_cast<double>(olc_alphabet.find(digits[i + 1])) * lng_res;
  }
  for (; i < digits.size(); ++i) {
    lat_res /= 5;
    lng_res /= 4;
    const auto v = static_cast<int>(olc_alphabet.find(digits[i]));
    south += (v / 4) * lat_res;
    west += (v % 4) * lng_res;
  }
  return Cell{south, west, south + lat_res, west + lng_res};
}

// --------------------------------------------------------------------------- formats

bool is_pluscode(std::string_view format_id) { return format_id == "pluscode"; }

Result<ir::Geo> read_codes(ReadContext& ctx) {
  const std::string text = input_text(ctx.input);
  const bool plus = is_pluscode(ctx.format_id);
  ir::Geo geo;
  std::size_t line_number = 0;
  for (std::string_view line : util::split_lines(text)) {
    ++line_number;
    line = util::trim(line);
    if (line.empty()) continue;
    // A code may be followed by a label: "8FVC9G8F+6X Zurich".
    const std::size_t space = line.find_first_of(" \t,;");
    const std::string_view code = line.substr(0, space);
    const std::string_view label = space == std::string_view::npos ? std::string_view() : util::trim(line.substr(space + 1));
    std::optional<Cell> cell;
    if (plus) {
      if (code.find('+') != std::string_view::npos && code.find('+') < 8) {
        return fail(Status::Decode, "line {}: '{}' is a short Plus Code; it needs a reference place to be decoded",
                    line_number, util::sanitize_utf8(code.substr(0, 20)));
      }
      cell = olc_decode(code);
    } else {
      cell = geohash_decode(code);
    }
    if (!cell) return fail(Status::Decode, "line {} is not a valid {}", line_number, plus ? "Plus Code" : "geohash");
    ir::Feature f;
    ir::Coord c;
    c.lat = (cell->south + cell->north) / 2;
    c.lon = (cell->west + cell->east) / 2;
    f.geometry.shape = ir::Point{c};
    f.properties.set("code", std::string(code));
    f.properties.set("cell_height", cell->north - cell->south);
    f.properties.set("cell_width", cell->east - cell->west);
    if (!label.empty()) {
      f.name = std::string(label);
      f.properties.set("name", f.name);
    }
    geo.features.push_back(std::move(f));
  }
  if (geo.features.empty()) return fail(Status::Decode, "the file holds no codes");
  return geo;
}

Result<Bytes> write_codes(const ir::Geo& geo, WriteContext& ctx) {
  const bool plus = is_pluscode(ctx.format_id);
  const int length = static_cast<int>(ctx.options.get_int("code_length", 11, plus ? 2 : 1, plus ? 15 : 12));
  std::string out;
  bool centroids = false;
  const auto emit = [&](const ir::Coord& c) {
    out += plus ? olc_encode(c.lat, c.lon, length) : geohash_encode(c.lat, c.lon, length);
    out += '\n';
  };
  for (const ir::Feature& f : geo.features) {
    if (const auto* p = std::get_if<ir::Point>(&f.geometry.shape)) {
      emit(p->coord);
    } else if (const auto* mp = std::get_if<ir::MultiPoint>(&f.geometry.shape)) {
      for (const ir::Coord& c : mp->points) emit(c);
    } else {
      const BoundingBox box = bounding_box(f.geometry);
      if (box.empty) continue;
      ir::Coord c;
      c.lat = (box.min_lat + box.max_lat) / 2;
      c.lon = (box.min_lon + box.max_lon) / 2;
      emit(c);
      centroids = true;
    }
  }
  if (centroids) ctx.warn("Lines and areas were written as the code of the centre of their bounding box.");
  return to_bytes(out);
}

int sniff_pluscode(ByteView data, std::string_view) {
  const std::string_view head = text_head(data, 1024);
  const std::string_view first = util::trim(head.substr(0, head.find_first_of("\r\n \t")));
  return first.size() >= 10 && first.size() <= 16 && olc_decode(first) ? 85 : 0;
}

int sniff_geohash(ByteView data, std::string_view filename) {
  const std::string_view head = text_head(data, 4096);
  const bool by_name = has_extension(filename, "geohash");
  bool digit = false;
  std::size_t lines = 0;
  for (std::string_view line : util::split_lines(head)) {
    line = util::trim(line);
    if (line.empty()) continue;
    const std::string_view code = line.substr(0, line.find_first_of(" \t,;"));
    if (code.size() < (by_name ? 1u : 6u) || code.size() > 12 || !geohash_decode(code)) return 0;
    digit = digit || code.find_first_of("0123456789") != std::string_view::npos;
    ++lines;
  }
  if (lines == 0) return 0;
  return by_name ? 80 : (digit ? 45 : 0);
}

}  // namespace

void register_geocodes(Registry& registry) {
  FormatInfo gh;
  gh.id = "geohash";
  gh.name = "Geohash list";
  gh.description = "Geohash codes of positions, one per line.";
  gh.category = Category::Geo;
  gh.extensions = {"geohash"};
  gh.mime = "text/plain";
  gh.popularity = 12;
  gh.sniff = sniff_geohash;
  registry.add_format(std::move(gh));
  GeoCodecSpec spec;
  spec.format_id = "geohash";
  spec.read = read_codes;
  spec.write = write_codes;
  spec.lossy_write = true;
  spec.write_options = {OptionSpec::integer("code_length", "Code length", 11, 1, 12,
                                            "Characters per geohash: 6 is about 1 km, 9 about 5 m, 11 about 15 cm.")};
  add_geo_codec(registry, std::move(spec));

  FormatInfo olc;
  olc.id = "pluscode";
  olc.name = "Plus Code list";
  olc.description = "Open Location Codes (Plus Codes) of positions, one per line.";
  olc.category = Category::Geo;
  olc.extensions = {"pluscode", "olc"};
  olc.mime = "text/plain";
  olc.popularity = 12;
  olc.sniff = sniff_pluscode;
  registry.add_format(std::move(olc));
  GeoCodecSpec plus;
  plus.format_id = "pluscode";
  plus.read = read_codes;
  plus.write = write_codes;
  plus.lossy_write = true;
  plus.write_options = {OptionSpec::integer("code_length", "Code length", 11, 2, 15,
                                            "Digits per code: 10 is about 14 m, 11 about 3 m, 12 about 60 cm.")};
  add_geo_codec(registry, std::move(plus));
}

}  // namespace cvt::geo
