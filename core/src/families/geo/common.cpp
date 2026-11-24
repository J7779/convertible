// Convertible core - geo family: shared helpers (registration, options,
// numbers, time, detection).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
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

// --------------------------------------------------------------------------- numbers

std::string format_number(double value, int decimals) {
  if (!std::isfinite(value)) return "0";
  if (decimals < 0) {
    std::string text = util::format_double(value);
    if (text == "-0") text = "0";
    return text;
  }
  decimals = std::min(decimals, 17);
  char buffer[400];
  const auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed, decimals);
  if (ec != std::errc{}) return util::format_double(value);
  std::string text(buffer, end);
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
  }
  if (text == "-0") text = "0";
  return text;
}

std::optional<double> parse_number(std::string_view text) {
  text = util::trim(text);
  if (text.empty()) return std::nullopt;
  if (text.front() == '+') text.remove_prefix(1);
  auto value = util::parse_double(text);
  if (!value || !std::isfinite(*value)) return std::nullopt;
  return value;
}

// --------------------------------------------------------------------------- time

std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

CivilDate civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return CivilDate{y + (m <= 2), m, d};
}

namespace {

bool take_digits(std::string_view& text, std::size_t count, std::int64_t& out) {
  if (text.size() < count) return false;
  std::int64_t v = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (!util::is_ascii_digit(text[i])) return false;
    v = v * 10 + (text[i] - '0');
  }
  out = v;
  text.remove_prefix(count);
  return true;
}

}  // namespace

std::optional<std::int64_t> parse_time(std::string_view text) {
  text = util::trim(text);
  std::int64_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  bool negative_year = false;
  if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
    negative_year = text.front() == '-';
    text.remove_prefix(1);
  }
  if (!take_digits(text, 4, year)) return std::nullopt;
  if (negative_year) year = -year;
  // "2024-03-10", "20240310", and the "2024/03/10" GDAL writes in CSV.
  const char separator = text.empty() ? '\0' : text.front();
  const bool extended = separator == '-' || separator == '/';
  if (extended) text.remove_prefix(1);
  if (!take_digits(text, 2, month)) return std::nullopt;
  if (extended) {
    if (text.empty() || text.front() != separator) return std::nullopt;
    text.remove_prefix(1);
  }
  if (!take_digits(text, 2, day)) return std::nullopt;
  if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
  std::int64_t millis = 0;
  std::int64_t offset_minutes = 0;
  if (!text.empty() && (text.front() == 'T' || text.front() == 't' || text.front() == ' ')) {
    text.remove_prefix(1);
    if (!take_digits(text, 2, hour)) return std::nullopt;
    const bool colon = !text.empty() && text.front() == ':';
    if (colon) text.remove_prefix(1);
    if (!take_digits(text, 2, minute)) return std::nullopt;
    if (!text.empty() && (text.front() == ':' || (!colon && util::is_ascii_digit(text.front())))) {
      if (text.front() == ':') text.remove_prefix(1);
      if (!take_digits(text, 2, second)) return std::nullopt;
      if (!text.empty() && (text.front() == '.' || text.front() == ',')) {
        text.remove_prefix(1);
        std::size_t digits = 0;
        double fraction = 0, scale = 0.1;
        while (!text.empty() && util::is_ascii_digit(text.front())) {
          if (digits < 9) {
            fraction += (text.front() - '0') * scale;
            scale /= 10;
          }
          ++digits;
          text.remove_prefix(1);
        }
        if (digits == 0) return std::nullopt;
        millis = std::llround(fraction * 1000.0);
      }
    }
    if (hour > 24 || minute > 59 || second > 60) return std::nullopt;
    if (!text.empty()) {
      if (text.front() == 'Z' || text.front() == 'z') {
        text.remove_prefix(1);
      } else if (text.front() == '+' || text.front() == '-') {
        const int sign = text.front() == '-' ? -1 : 1;
        text.remove_prefix(1);
        std::int64_t oh = 0, om = 0;
        if (!take_digits(text, 2, oh)) return std::nullopt;
        if (!text.empty() && text.front() == ':') text.remove_prefix(1);
        if (!text.empty()) {
          if (!take_digits(text, 2, om)) return std::nullopt;
        }
        offset_minutes = sign * (oh * 60 + om);
      }
    }
  }
  if (!util::trim(text).empty()) return std::nullopt;
  const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const std::int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second - offset_minutes * 60;
  return seconds * 1000 + millis;
}

std::string format_time(std::int64_t unix_ms, bool always_millis) {
  std::int64_t seconds = unix_ms / 1000;
  std::int64_t millis = unix_ms % 1000;
  if (millis < 0) {
    millis += 1000;
    seconds -= 1;
  }
  std::int64_t days = seconds / 86400;
  std::int64_t rest = seconds % 86400;
  if (rest < 0) {
    rest += 86400;
    days -= 1;
  }
  const CivilDate date = civil_from_days(days);
  std::string out = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", date.year, date.month, date.day, rest / 3600,
                                (rest / 60) % 60, rest % 60);
  if (millis != 0 || always_millis) out += std::format(".{:03}", millis);
  out += 'Z';
  return out;
}

// --------------------------------------------------------------------------- geometry helpers

std::string_view geometry_type_name(const ir::Geometry& geometry) noexcept {
  static constexpr std::string_view names[] = {"Point",           "LineString",   "Polygon",           "MultiPoint",
                                               "MultiLineString", "MultiPolygon", "GeometryCollection"};
  return names[geometry.shape.index()];
}

std::size_t coord_count(const ir::Geometry& geometry) {
  std::size_t n = 0;
  for_each_coord(geometry, [&](const ir::Coord&) { ++n; });
  return n;
}

bool geometry_empty(const ir::Geometry& geometry) { return coord_count(geometry) == 0; }

std::string feature_name(const ir::Feature& feature) {
  if (!feature.name.empty()) return feature.name;
  if (const Json* name = feature.properties.find("name"); name != nullptr && name->is_string()) {
    return name->as_string();
  }
  return {};
}

std::string json_to_text(const Json& value) {
  switch (value.type()) {
    case Json::Type::Null: return {};
    case Json::Type::Bool: return value.as_bool() ? "true" : "false";
    case Json::Type::Int64: return std::to_string(value.as_int64());
    case Json::Type::UInt64: return std::to_string(value.as_uint64());
    case Json::Type::Double: return format_number(value.as_double());
    case Json::Type::String: return value.as_string();
    case Json::Type::Array:
    case Json::Type::Object: return util::dump_json(value);
  }
  return {};
}

Json text_to_json(std::string_view text) {
  const std::string_view t = util::trim(text);
  if (t.empty()) return Json(std::string(text));
  if (t == "true") return Json(true);
  if (t == "false") return Json(false);
  // Leading zeros ("007") and '+' signs are kept as text: they are usually codes.
  const bool leading_zero = t.size() > 1 && t[0] == '0' && t[1] != '.';
  const bool negative_leading_zero = t.size() > 2 && t[0] == '-' && t[1] == '0' && t[2] != '.';
  if (t.size() == text.size() && !leading_zero && !negative_leading_zero && t.front() != '+') {
    bool integer = true;
    for (std::size_t i = 0; i < t.size(); ++i) {
      if (!(util::is_ascii_digit(t[i]) || (i == 0 && t[i] == '-'))) integer = false;
    }
    if (integer) {
      if (auto v = util::parse_int(t)) return Json(*v);
    }
    bool numeric = t.find_first_not_of("0123456789.-eE+") == std::string_view::npos;
    if (numeric) {
      if (auto v = util::parse_double(t); v && std::isfinite(*v)) return Json(*v);
    }
  }
  return Json(std::string(text));
}

bool has_times(const ir::Geometry& geometry) {
  bool any = false;
  for_each_coord(geometry, [&](const ir::Coord& c) { any = any || c.time_ms.has_value(); });
  return any;
}

// --------------------------------------------------------------------------- detection

std::string_view text_head(ByteView data, std::size_t limit) noexcept {
  std::string_view text = as_chars(data.first(std::min(limit, data.size())));
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  return util::trim_left(text);
}

bool head_contains(ByteView data, std::string_view needle, std::size_t limit) noexcept {
  return as_chars(data.first(std::min(limit, data.size()))).find(needle) != std::string_view::npos;
}

bool zip_has_entry_with_extension(ByteView data, std::string_view suffix) {
  if (data.size() < 22 || as_chars(data.first(2)) != "PK") return false;
  Limits limits;
  limits.max_archive_entries = 10'000;
  auto zip = util::ZipReader::open(data, limits);
  if (!zip) return false;
  for (const auto& e : zip->entries()) {
    if (e.is_directory || e.name.starts_with("__MACOSX/") || e.name.starts_with("._")) continue;
    if (util::iends_with(e.name, suffix)) return true;
  }
  return false;
}

bool has_extension(std::string_view filename, std::string_view extension) noexcept {
  return filename.size() > extension.size() && filename[filename.size() - extension.size() - 1] == '.' &&
         util::iends_with(filename, extension);
}

util::XmlParseOptions xml_options() {
  util::XmlParseOptions options;
  options.max_depth = 256;
  options.max_entity_expansion = 64 * 1024;
  return options;
}

std::string input_text(ByteView data) {
  if (const auto bom = util::detect_bom(data)) {
    if (bom->encoding == util::TextEncoding::Utf8) return std::string(as_chars(data.subspan(bom->length)));
    return util::decode_text(data, bom->encoding).text;
  }
  return std::string(as_chars(data));
}

Result<void> check_declared(std::uint64_t count, std::uint64_t min_bytes_each, std::uint64_t remaining,
                            std::string_view what) {
  if (min_bytes_each != 0 && count > remaining / min_bytes_each) {
    return fail(Status::Decode, "the file declares {} {} but is too short to hold them", count, what);
  }
  return {};
}

// --------------------------------------------------------------------------- options

std::vector<OptionSpec> common_read_options() {
  return {OptionSpec::text("source_crs", "Source coordinate system", "",
                           "Coordinate reference system of the input (an EPSG code such as EPSG:32633) when the file "
                           "does not declare one or declares it wrongly; empty = as declared, else WGS 84.")
              .advanced()};
}

std::vector<OptionSpec> common_write_options(const GeoWriterTraits& traits) {
  std::vector<OptionSpec> specs = {
      options::quality(),
      options::preset(),
      OptionSpec::integer("precision", "Coordinate decimals", -1, -1, 15,
                          "Decimal places kept for coordinates (6 is about 10 cm in degrees); -1 keeps full "
                          "precision unless a lower quality asks for less."),
      OptionSpec::number("simplify_tolerance", "Simplify tolerance (m)", 0.0, 0.0, 1'000'000.0,
                         "Remove vertices that deviate less than this many metres from the simplified shape; 0 = "
                         "only when a lower quality asks for it."),
      OptionSpec::choice("simplify_method", "Simplification method", "douglas-peucker",
                         {"douglas-peucker", "visvalingam"},
                         "Douglas-Peucker keeps the farthest points; Visvalingam-Whyatt removes the smallest "
                         "triangles and gives smoother results.")
          .advanced(),
      options::strip_metadata(),
  };
  if (traits.has_time) {
    specs.push_back(OptionSpec::boolean("keep_time", "Keep timestamps", true, "Write the time of every track point."));
  }
  if (traits.has_sensors) {
    specs.push_back(OptionSpec::boolean("keep_sensors", "Keep sensor data", true,
                                        "Write heart rate, cadence, power, speed and temperature."));
  }
  if (traits.has_tracks) {
    specs.push_back(OptionSpec::choice("track_mode", "Tracks", "keep", {"keep", "merge", "split"},
                                       "Keep tracks as they are, merge all tracks and segments into one, or write "
                                       "every segment as its own track."));
  }
  if (traits.crs_capable) {
    specs.push_back(OptionSpec::text("target_crs", "Output coordinate system", "",
                                     "Write coordinates in this system: EPSG:4326 (default), EPSG:3857, a UTM zone "
                                     "(EPSG:326zz / 327zz, 258zz, 269zz), EPSG:27700, or \"utm\" for the zone of the "
                                     "data."));
  }
  return specs;
}

int resolved_precision(const Options& options, bool projected) {
  const auto explicit_precision = options.get_int("precision", -1, -1, 15);
  if (explicit_precision >= 0) return static_cast<int>(explicit_precision);
  const int quality = options::resolved_quality(options);
  if (quality >= 90) return -1;
  if (quality >= 60) return projected ? 2 : 6;
  if (quality >= 30) return projected ? 1 : 5;
  return projected ? 0 : 4;
}

namespace {

double resolved_tolerance(const Options& options) {
  const double explicit_tolerance = options.get_float("simplify_tolerance", 0.0, 0.0, 1'000'000.0);
  if (explicit_tolerance > 0) return explicit_tolerance;
  const int quality = options::resolved_quality(options);
  if (quality >= 90) return 0.0;
  return 0.25 * std::pow(2.0, (90.0 - quality) / 10.0);
}

void strip_times(ir::Geo& geo) {
  for_each_coord(geo, [](ir::Coord& c) { c.time_ms.reset(); });
}

void strip_sensors(ir::Geo& geo) {
  for_each_coord(geo, [](ir::Coord& c) {
    c.heart_rate.reset();
    c.cadence.reset();
    c.power.reset();
    c.speed.reset();
    c.temperature.reset();
    c.extra.clear();
  });
}

}  // namespace

Result<void> normalize_after_read(ir::Geo& geo, const Context& ctx) {
  const std::string source = ctx.options.get_string("source_crs", "");
  if (!util::trim(source).empty()) {
    const auto crs = parse_crs(source);
    if (!crs) return fail(Status::InvalidArgument, "option 'source_crs': '{}' is not a supported coordinate system", source);
    geo.crs = crs->kind == CrsKind::Geographic ? std::string() : crs_id(*crs);
  }
  if (geo.crs.empty()) return {};
  const auto crs = parse_crs(geo.crs);
  if (!crs) {
    ctx.warn("The coordinate system of the input is not one Convertible can convert; coordinates were kept as they are.");
    return {};
  }
  if (crs->kind == CrsKind::Geographic) {
    geo.crs.clear();
    return {};
  }
  return reproject(geo, Crs{});
}

Result<ir::Geo> prepare_for_write(const ir::Geo& source, const Context& ctx, const GeoWriterTraits& traits) {
  ir::Geo geo = source;
  const Options& options = ctx.options;
  if (traits.has_tracks) {
    const std::string mode = options.get_enum("track_mode", "keep", {"keep", "merge", "split"});
    if (mode == "merge") merge_tracks(geo);
    if (mode == "split") split_tracks(geo);
  }
  if (traits.has_time && !options.get_bool("keep_time", true)) strip_times(geo);
  if (traits.has_sensors && !options.get_bool("keep_sensors", true)) strip_sensors(geo);
  if (!traits.has_time) {
    // Formats without per-point time keep nothing to strip; nothing to do.
  }
  if (options::resolved_strip_metadata(options)) {
    geo.metadata = Json::object();
  }

  // Coordinate system: the IR is WGS 84 unless a reader could not convert.
  const auto current = parse_crs(geo.crs);
  if (!current && !geo.crs.empty()) {
    ctx.warn("Coordinates are in a coordinate system Convertible cannot convert; they were written unchanged.");
  }
  bool projected = current && current->kind != CrsKind::Geographic;
  const double tolerance = resolved_tolerance(options);
  if (tolerance > 0) {
    const SimplifyMethod method = options.get_enum("simplify_method", "douglas-peucker",
                                                   {"douglas-peucker", "visvalingam"}) == "visvalingam"
                                      ? SimplifyMethod::Visvalingam
                                      : SimplifyMethod::DouglasPeucker;
    simplify(geo, tolerance, method, projected || (!current && !geo.crs.empty()));
  }
  if (traits.crs_capable) {
    const std::string target_text = std::string(util::trim(options.get_string("target_crs", "")));
    if (!target_text.empty() && current) {
      Crs target;
      if (util::iequals(target_text, "utm")) {
        if (projected) CVT_TRY(reproject(geo, Crs{}));
        const BoundingBox box = bounding_box(geo);
        target = box.empty ? utm_crs_for(0, 0) : utm_crs_for((box.min_lon + box.max_lon) / 2, (box.min_lat + box.max_lat) / 2);
      } else {
        const auto parsed = parse_crs(target_text);
        if (!parsed) {
          return fail(Status::InvalidArgument, "option 'target_crs': '{}' is not a supported coordinate system",
                      target_text);
        }
        target = *parsed;
      }
      CVT_TRY(reproject(geo, target));
      projected = target.kind != CrsKind::Geographic;
    } else if (!target_text.empty() && !current) {
      ctx.warn("The coordinates could not be converted to the requested coordinate system.");
    }
  } else if (projected) {
    // Formats that only hold WGS 84.
    CVT_TRY(reproject(geo, Crs{}));
    projected = false;
  }
  const int decimals = resolved_precision(options, projected);
  if (decimals >= 0) round_coordinates(geo, decimals);
  return geo;
}

// --------------------------------------------------------------------------- registration

void add_geo_codec(Registry& registry, GeoCodecSpec spec) {
  if (spec.read) {
    Codec reader;
    reader.format_id = spec.format_id;
    reader.kind = Kind::Geo;
    reader.read_cost = spec.read_cost;
    reader.read_options = common_read_options();
    for (auto& o : spec.read_options) reader.read_options.push_back(o);
    GeoReadFn read = spec.read;
    reader.read = [read](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(ir::Geo geo, read(ctx));
      CVT_TRY(normalize_after_read(geo, ctx));
      return AnyIR(std::move(geo));
    };
    if (spec.write) {
      // Readers and writers of one format share a Codec entry when neither is lossy.
      if (!spec.lossy_write) {
        reader.write_cost = spec.write_cost;
        reader.write_options = common_write_options(spec.traits);
        for (auto& o : spec.write_options) reader.write_options.push_back(o);
        GeoWriteFn write = spec.write;
        const GeoWriterTraits traits = spec.traits;
        reader.write = [write, traits](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
          CVT_TRY_ASSIGN(const ir::Geo geo, prepare_for_write(value.get<ir::Geo>(), ctx, traits));
          return write(geo, ctx);
        };
        registry.add_codec(std::move(reader));
        return;
      }
    }
    registry.add_codec(std::move(reader));
  }
  if (spec.write) {
    Codec writer;
    writer.format_id = spec.format_id;
    writer.kind = Kind::Geo;
    writer.write_cost = spec.write_cost;
    writer.lossy = spec.lossy_write;
    writer.write_options = common_write_options(spec.traits);
    for (auto& o : spec.write_options) writer.write_options.push_back(o);
    GeoWriteFn write = spec.write;
    const GeoWriterTraits traits = spec.traits;
    writer.write = [write, traits](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(const ir::Geo geo, prepare_for_write(value.get<ir::Geo>(), ctx, traits));
      return write(geo, ctx);
    };
    registry.add_codec(std::move(writer));
  }
}

}  // namespace cvt::geo
