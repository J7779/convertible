// Convertible core - geo family: Garmin FIT activity, course and track files.
//
// Reading: file header (12 or 14 bytes, optional header CRC), definition
// and data messages in either byte order, developer data (field
// descriptions give names; values become per-point extra data),
// compressed-timestamp record headers, chained files, and the file CRC
// (a mismatch is an error unless the CRC field is zero). Records become one
// track whose segments follow the laps; lap and session summaries become
// properties; file_id / device_info (manufacturer, product, serial number,
// creation time) go into Geo::metadata so that `strip_metadata` drops them.
// Course points become waypoints. Positions are semicircles (2^31 = 180 deg).
// Writing: a FIT 2.0 activity file - file_id (manufacturer "development",
// no serial number), timer events, one record per point, one lap per
// segment, a session and an activity message, with CRCs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
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
#include "util/text.hpp"

namespace cvt::geo {

namespace {

constexpr std::int64_t fit_epoch_s = 631065600;  // 1989-12-31T00:00:00Z
constexpr double semicircle = 180.0 / 2147483648.0;

std::uint16_t fit_crc(std::uint16_t crc, std::uint8_t byte) {
  static constexpr std::uint16_t table[16] = {0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
                                              0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400};
  std::uint16_t tmp = table[crc & 0xF];
  crc = static_cast<std::uint16_t>((crc >> 4) & 0x0FFF);
  crc = static_cast<std::uint16_t>(crc ^ tmp ^ table[byte & 0xF]);
  tmp = table[crc & 0xF];
  crc = static_cast<std::uint16_t>((crc >> 4) & 0x0FFF);
  crc = static_cast<std::uint16_t>(crc ^ tmp ^ table[(byte >> 4) & 0xF]);
  return crc;
}

std::uint16_t fit_crc(ByteView data) {
  std::uint16_t crc = 0;
  for (std::byte b : data) crc = fit_crc(crc, std::to_integer<std::uint8_t>(b));
  return crc;
}

// Sport enum (profile) <-> name.
constexpr std::pair<int, std::string_view> sports[] = {
    {0, "generic"},  {1, "running"},  {2, "cycling"},  {3, "transition"}, {4, "fitness_equipment"},
    {5, "swimming"}, {6, "basketball"}, {10, "training"}, {11, "walking"}, {13, "cross_country_skiing"},
    {14, "alpine_skiing"}, {15, "snowboarding"}, {16, "rowing"}, {17, "hiking"}, {19, "paddling"},
    {21, "e_biking"}, {37, "stand_up_paddleboarding"}, {41, "kayaking"}};

std::string sport_name(std::int64_t value) {
  for (const auto& [v, name] : sports) {
    if (v == value) return std::string(name);
  }
  return "sport " + std::to_string(value);
}

std::uint8_t sport_value(std::string_view name) {
  const std::string lower = util::to_lower(name);
  for (const auto& [v, n] : sports) {
    if (n == lower) return static_cast<std::uint8_t>(v);
  }
  if (lower == "biking" || lower == "bike" || lower == "ride") return 2;
  if (lower == "run") return 1;
  return 0;
}

// --------------------------------------------------------------------------- reading

struct FieldDef {
  std::uint8_t number = 0;
  std::uint8_t size = 0;
  std::uint8_t base = 0;
};

struct DevFieldDef {
  std::uint8_t number = 0;
  std::uint8_t size = 0;
  std::uint8_t developer = 0;
};

struct Definition {
  bool defined = false;
  bool big_endian = false;
  std::uint16_t global = 0;
  std::vector<FieldDef> fields;
  std::vector<DevFieldDef> dev_fields;
};

struct DevDescription {
  std::string name;
  std::string units;
  std::uint8_t base = 0;
  double scale = 1;
  double offset = 0;
};

// One decoded value: number (NaN when invalid) or text.
struct Value {
  double number = std::numeric_limits<double>::quiet_NaN();
  std::string text;
  bool valid() const { return !std::isnan(number) || !text.empty(); }
};

Value decode_value(ByteView raw, std::uint8_t base_type, bool big_endian) {
  Value v;
  const std::uint8_t base = base_type & 0x1F;
  util::ByteReader r(raw);
  const auto endian = big_endian ? std::endian::big : std::endian::little;
  switch (base) {
    case 0x00:  // enum
    case 0x02:  // uint8
    case 0x0D: {
      const std::uint8_t x = r.u8();
      if (r.ok() && x != 0xFF) v.number = x;
      break;
    }
    case 0x0A: {
      const std::uint8_t x = r.u8();
      if (r.ok() && x != 0) v.number = x;
      break;
    }
    case 0x01: {
      const std::int8_t x = r.i8();
      if (r.ok() && x != 0x7F) v.number = x;
      break;
    }
    case 0x03: {
      const auto x = r.read<std::int16_t>(endian);
      if (r.ok() && x != 0x7FFF) v.number = x;
      break;
    }
    case 0x04:
    case 0x0B: {
      const auto x = r.read<std::uint16_t>(endian);
      if (r.ok() && x != (base == 0x04 ? 0xFFFF : 0)) v.number = x;
      break;
    }
    case 0x05: {
      const auto x = r.read<std::int32_t>(endian);
      if (r.ok() && x != 0x7FFFFFFF) v.number = x;
      break;
    }
    case 0x06:
    case 0x0C: {
      const auto x = r.read<std::uint32_t>(endian);
      if (r.ok() && x != (base == 0x06 ? 0xFFFFFFFFu : 0u)) v.number = x;
      break;
    }
    case 0x08: {
      const auto x = r.read<std::uint32_t>(endian);
      if (r.ok() && x != 0xFFFFFFFFu) v.number = std::bit_cast<float>(x);
      break;
    }
    case 0x09: {
      const auto x = r.read<std::uint64_t>(endian);
      if (r.ok() && x != 0xFFFFFFFFFFFFFFFFull) v.number = std::bit_cast<double>(x);
      break;
    }
    case 0x0E: {
      const auto x = r.read<std::int64_t>(endian);
      if (r.ok() && x != 0x7FFFFFFFFFFFFFFFll) v.number = static_cast<double>(x);
      break;
    }
    case 0x0F:
    case 0x10: {
      const auto x = r.read<std::uint64_t>(endian);
      if (r.ok() && x != (base == 0x0F ? 0xFFFFFFFFFFFFFFFFull : 0ull)) v.number = static_cast<double>(x);
      break;
    }
    case 0x07: {
      std::string_view s = as_chars(raw);
      s = s.substr(0, std::min(s.find('\0'), s.size()));
      v.text = util::sanitize_utf8(s);
      break;
    }
    default: break;
  }
  return v;
}

struct Message {
  std::uint16_t global = 0;
  std::map<std::uint8_t, Value> fields;
  std::vector<std::pair<std::string, double>> dev_values;

  double num(std::uint8_t field) const {
    const auto it = fields.find(field);
    return it == fields.end() ? std::numeric_limits<double>::quiet_NaN() : it->second.number;
  }
  bool has(std::uint8_t field) const { return !std::isnan(num(field)); }
  std::string text(std::uint8_t field) const {
    const auto it = fields.find(field);
    return it == fields.end() ? std::string() : it->second.text;
  }
};

struct FitData {
  std::vector<Message> messages;
};

Result<void> parse_file(ByteView file, FitData& out, const Context& ctx, std::size_t& consumed) {
  util::ByteReader h(file);
  const std::uint8_t header_size = h.u8();
  const std::uint8_t protocol = h.u8();
  (void)h.u16le();  // profile version
  const std::uint32_t data_size = h.u32le();
  const std::string_view magic = h.chars(4);
  if (!h.ok() || (header_size != 12 && header_size != 14) || magic != ".FIT") {
    return fail(Status::Decode, "not a FIT file");
  }
  if ((protocol >> 4) > 2) return fail(Status::Decode, "unsupported FIT protocol version {}", protocol >> 4);
  if (header_size == 14) {
    const std::uint16_t header_crc = h.u16le();
    if (header_crc != 0 && header_crc != fit_crc(file.first(12))) return fail(Status::Decode, "the FIT header checksum is wrong");
  }
  const std::uint64_t end = std::uint64_t{header_size} + data_size;
  if (end + 2 > file.size()) return fail(Status::Decode, "the FIT file is truncated");
  const std::uint16_t stored_crc = static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(file[end]) |
                                                              (std::to_integer<std::uint16_t>(file[end + 1]) << 8));
  if (stored_crc != 0 && stored_crc != fit_crc(file.first(static_cast<std::size_t>(end)))) {
    return fail(Status::Decode, "the FIT file checksum is wrong (the file is damaged)");
  }
  consumed = static_cast<std::size_t>(end + 2);
  std::array<Definition, 16> defs{};
  std::map<std::pair<std::uint8_t, std::uint8_t>, DevDescription> dev_descriptions;
  util::ByteReader r(file.subspan(header_size, data_size));
  std::uint32_t last_timestamp = 0;
  while (!r.at_end()) {
    if ((out.messages.size() & 4095) == 4095) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(r.position()) / static_cast<double>(data_size)));
    }
    const std::uint8_t header = r.u8();
    std::optional<std::uint32_t> compressed_time;
    std::uint8_t local = 0;
    if (header & 0x80) {
      local = (header >> 5) & 0x03;
      const std::uint32_t offset = header & 0x1F;
      std::uint32_t t = (last_timestamp & ~0x1Fu) + offset;
      if (offset < (last_timestamp & 0x1Fu)) t += 0x20;
      compressed_time = t;
      last_timestamp = t;
    } else if (header & 0x40) {
      local = header & 0x0F;
      Definition d;
      d.defined = true;
      (void)r.u8();
      d.big_endian = r.u8() == 1;
      d.global = d.big_endian ? r.u16be() : r.u16le();
      const std::uint8_t n = r.u8();
      for (std::uint8_t i = 0; i < n; ++i) {
        FieldDef f;
        f.number = r.u8();
        f.size = r.u8();
        f.base = r.u8();
        d.fields.push_back(f);
      }
      if (header & 0x20) {
        const std::uint8_t m = r.u8();
        for (std::uint8_t i = 0; i < m; ++i) {
          DevFieldDef f;
          f.number = r.u8();
          f.size = r.u8();
          f.developer = r.u8();
          d.dev_fields.push_back(f);
        }
      }
      if (!r.ok()) return fail(Status::Decode, "truncated FIT definition message");
      defs[local] = std::move(d);
      continue;
    } else {
      local = header & 0x0F;
    }
    const Definition& d = defs[local];
    if (!d.defined) return fail(Status::Decode, "a FIT data message uses an undefined message type");
    Message m;
    m.global = d.global;
    for (const FieldDef& f : d.fields) {
      const ByteView raw = r.bytes(f.size);
      if (!r.ok()) return fail(Status::Decode, "truncated FIT data message");
      // Arrays: the first element stands for the field.
      const std::size_t element = std::max<std::size_t>(1, std::min<std::size_t>(f.size, [&]() -> std::size_t {
        switch (f.base & 0x1F) {
          case 0x03: case 0x04: case 0x0B: return 2;
          case 0x05: case 0x06: case 0x08: case 0x0C: return 4;
          case 0x09: case 0x0E: case 0x0F: case 0x10: return 8;
          case 0x07: return f.size;
          default: return 1;
        }
      }()));
      if (raw.size() < element) continue;
      m.fields[f.number] = decode_value(raw.first(element), f.base, d.big_endian);
    }
    for (const DevFieldDef& f : d.dev_fields) {
      const ByteView raw = r.bytes(f.size);
      if (!r.ok()) return fail(Status::Decode, "truncated FIT developer data");
      const auto it = dev_descriptions.find({f.developer, f.number});
      if (it == dev_descriptions.end()) continue;
      const Value v = decode_value(raw, it->second.base, d.big_endian);
      if (!std::isnan(v.number) && m.dev_values.size() < 64) {
        m.dev_values.emplace_back(it->second.name, v.number / it->second.scale - it->second.offset);
      }
    }
    if (m.has(253)) last_timestamp = static_cast<std::uint32_t>(m.num(253));
    if (compressed_time && !m.has(253)) m.fields[253].number = *compressed_time;
    if (m.global == 206) {
      DevDescription desc;
      desc.name = m.text(3);
      desc.units = m.text(8);
      desc.base = static_cast<std::uint8_t>(m.has(2) ? m.num(2) : 0);
      if (m.has(6) && m.num(6) != 0) desc.scale = m.num(6);
      if (m.has(7)) desc.offset = m.num(7);
      if (desc.name.empty()) desc.name = "developer_" + std::to_string(static_cast<int>(m.num(1)));
      dev_descriptions[{static_cast<std::uint8_t>(m.has(0) ? m.num(0) : 0), static_cast<std::uint8_t>(m.has(1) ? m.num(1) : 0)}] =
          desc;
    }
    if (out.messages.size() >= 50'000'000) return fail(Status::Limit, "the FIT file holds too many messages");
    out.messages.push_back(std::move(m));
  }
  return {};
}

std::int64_t fit_time_ms(double fit_seconds) { return (static_cast<std::int64_t>(fit_seconds) + fit_epoch_s) * 1000; }

Json summary(const Message& m) {
  Json s = Json::object();
  const auto time = [&](std::uint8_t field, std::string_view key) {
    if (m.has(field)) s.set(key, format_time(fit_time_ms(m.num(field))));
  };
  const auto scaled = [&](std::uint8_t field, std::string_view key, double scale) {
    if (m.has(field)) s.set(key, m.num(field) / scale);
  };
  time(2, "start_time");
  time(253, "end_time");
  scaled(7, "total_elapsed_time_s", 1000);
  scaled(8, "total_timer_time_s", 1000);
  scaled(9, "total_distance_m", 100);
  return s;
}

Result<ir::Geo> read_fit(ReadContext& ctx) {
  FitData data;
  std::size_t pos = 0;
  while (pos + 12 <= ctx.input.size()) {
    std::size_t used = 0;
    const auto parsed = parse_file(ctx.input.subspan(pos), data, ctx, used);
    if (!parsed) {
      if (pos == 0) return fail(parsed.error());
      ctx.warn("Data after the end of the FIT file was ignored.");
      break;
    }
    pos += used;
  }
  ir::Geo geo;
  std::vector<ir::Coord> points;
  struct Lap {
    double start = std::numeric_limits<double>::quiet_NaN();
    Json json;
  };
  std::vector<Lap> laps;
  Json session;
  std::string course_name;
  std::optional<std::int64_t> sport;
  Json devices = Json::array();
  for (const Message& m : data.messages) {
    switch (m.global) {
      case 0: {  // file_id
        if (m.has(1)) geo.metadata.set("manufacturer", static_cast<std::int64_t>(m.num(1)));
        if (m.has(2)) geo.metadata.set("product", static_cast<std::int64_t>(m.num(2)));
        if (m.has(3)) geo.metadata.set("serial_number", static_cast<std::int64_t>(m.num(3)));
        if (m.has(4)) geo.metadata.set("time", format_time(fit_time_ms(m.num(4))));
        if (!m.text(8).empty()) geo.metadata.set("product_name", m.text(8));
        break;
      }
      case 23: {  // device_info
        Json d = Json::object();
        if (m.has(2)) d.set("manufacturer", static_cast<std::int64_t>(m.num(2)));
        if (m.has(4)) d.set("product", static_cast<std::int64_t>(m.num(4)));
        if (m.has(3)) d.set("serial_number", static_cast<std::int64_t>(m.num(3)));
        if (m.has(5)) d.set("software_version", m.num(5) / 100);
        if (!d.empty() && devices.size() < 64) devices.push_back(std::move(d));
        break;
      }
      case 20: {  // record
        if (!m.has(0) || !m.has(1)) break;
        ir::Coord c;
        c.lat = m.num(0) * semicircle;
        c.lon = m.num(1) * semicircle;
        if (m.has(253)) c.time_ms = fit_time_ms(m.num(253));
        if (m.has(78)) c.ele = m.num(78) / 5 - 500;
        else if (m.has(2)) c.ele = m.num(2) / 5 - 500;
        if (m.has(3)) c.heart_rate = m.num(3);
        if (m.has(4)) c.cadence = m.num(4);
        if (m.has(7)) c.power = m.num(7);
        if (m.has(73)) c.speed = m.num(73) / 1000;
        else if (m.has(6)) c.speed = m.num(6) / 1000;
        if (m.has(13)) c.temperature = m.num(13);
        c.extra = m.dev_values;
        points.push_back(std::move(c));
        break;
      }
      case 19: {  // lap
        Lap lap;
        lap.start = m.num(2);
        lap.json = summary(m);
        if (m.has(15)) lap.json.set("avg_heart_rate", m.num(15));
        if (m.has(16)) lap.json.set("max_heart_rate", m.num(16));
        if (m.has(17)) lap.json.set("avg_cadence", m.num(17));
        if (m.has(19)) lap.json.set("avg_power", m.num(19));
        if (m.has(11)) lap.json.set("total_calories", m.num(11));
        if (laps.size() < 100'000) laps.push_back(std::move(lap));
        break;
      }
      case 18: {  // session
        session = summary(m);
        if (m.has(5)) sport = static_cast<std::int64_t>(m.num(5));
        if (m.has(16)) session.set("avg_heart_rate", m.num(16));
        if (m.has(17)) session.set("max_heart_rate", m.num(17));
        if (m.has(11)) session.set("total_calories", m.num(11));
        if (m.has(26)) session.set("num_laps", m.num(26));
        break;
      }
      case 12: {  // sport
        if (m.has(0)) sport = static_cast<std::int64_t>(m.num(0));
        break;
      }
      case 31: course_name = m.text(5); break;
      case 32: {  // course_point
        if (!m.has(2) || !m.has(3)) break;
        ir::Feature f;
        f.role = ir::FeatureRole::Waypoint;
        ir::Coord c;
        c.lat = m.num(2) * semicircle;
        c.lon = m.num(3) * semicircle;
        if (m.has(1)) c.time_ms = fit_time_ms(m.num(1));
        f.geometry.shape = ir::Point{c};
        f.name = m.text(6);
        if (!f.name.empty()) f.properties.set("name", f.name);
        geo.features.push_back(std::move(f));
        break;
      }
      default: break;
    }
  }
  if (!devices.empty()) geo.metadata.set("devices", std::move(devices));
  if (points.empty() && geo.features.empty()) return fail(Status::Decode, "the FIT file holds no positions");
  if (!points.empty()) {
    // Segments follow the laps: a new segment starts at each lap's start time.
    std::vector<double> starts;
    for (std::size_t i = 1; i < laps.size(); ++i) {
      if (!std::isnan(laps[i].start)) starts.push_back(static_cast<double>(fit_time_ms(laps[i].start)));
    }
    std::sort(starts.begin(), starts.end());
    ir::MultiLineString track;
    std::size_t current = 0;
    track.lines.emplace_back();
    for (ir::Coord& c : points) {
      if (c.time_ms) {
        const auto segment = static_cast<std::size_t>(
            std::upper_bound(starts.begin(), starts.end(), static_cast<double>(*c.time_ms)) - starts.begin());
        while (current < segment) {
          ++current;
          track.lines.emplace_back();
        }
      }
      track.lines.back().points.push_back(std::move(c));
    }
    std::erase_if(track.lines, [](const ir::LineString& l) { return l.points.empty(); });
    ir::Feature f;
    f.role = ir::FeatureRole::Track;
    f.geometry.shape = std::move(track);
    f.name = course_name;
    if (!course_name.empty()) f.properties.set("name", course_name);
    if (sport) f.properties.set("sport", sport_name(*sport));
    if (!laps.empty()) {
      Json lap_array = Json::array();
      for (auto& l : laps) lap_array.push_back(std::move(l.json));
      f.properties.set("laps", std::move(lap_array));
    }
    if (!session.is_null() && !session.empty()) f.properties.set("session", std::move(session));
    geo.features.insert(geo.features.begin(), std::move(f));
  }
  return geo;
}

// --------------------------------------------------------------------------- writing

struct FitWriter {
  util::ByteWriter data;

  void define(std::uint8_t local, std::uint16_t global, const std::vector<FieldDef>& fields) {
    data.u8(static_cast<std::uint8_t>(0x40 | local)).u8(0).u8(0).u16le(global).u8(static_cast<std::uint8_t>(fields.size()));
    for (const FieldDef& f : fields) data.u8(f.number).u8(f.size).u8(f.base);
  }
  void begin(std::uint8_t local) { data.u8(local); }
};

std::uint32_t fit_seconds(std::int64_t unix_ms) {
  const std::int64_t s = unix_ms / 1000 - fit_epoch_s;
  return static_cast<std::uint32_t>(std::clamp<std::int64_t>(s, 0, 0xFFFFFFFE));
}

std::int32_t semicircles(double degrees) {
  return static_cast<std::int32_t>(std::clamp(std::llround(degrees / semicircle), -2147483647LL, 2147483646LL));
}

template <class T>
T clamp_to(double v, double lo, double hi) {
  return static_cast<T>(std::llround(std::clamp(v, lo, hi)));
}

Result<Bytes> write_fit(const ir::Geo& geo, WriteContext& ctx) {
  // Collect the segments: tracks first, otherwise any lines.
  std::vector<std::vector<ir::Coord>> segments;
  std::string sport;
  std::size_t tracks = 0;
  for (int pass = 0; pass < 2 && segments.empty(); ++pass) {
    for (const ir::Feature& f : geo.features) {
      if (pass == 0 && f.role != ir::FeatureRole::Track) continue;
      bool used = false;
      if (const auto* ml = std::get_if<ir::MultiLineString>(&f.geometry.shape)) {
        for (const auto& l : ml->lines) {
          if (!l.points.empty()) segments.push_back(l.points);
        }
        used = true;
      } else if (const auto* l = std::get_if<ir::LineString>(&f.geometry.shape)) {
        if (!l->points.empty()) segments.push_back(l->points);
        used = true;
      }
      if (used) {
        ++tracks;
        if (sport.empty()) sport = f.properties["sport"].as_string();
      }
    }
  }
  if (segments.empty()) return fail(Status::Encode, "a FIT activity needs a track or a line; there is none");
  if (tracks > 1) ctx.warn("A FIT activity holds one track; all tracks were written as laps of one activity.");
  // Times: every point needs one.
  bool any_time = false, all_time = true;
  for (const auto& s : segments) {
    for (const auto& c : s) {
      any_time = any_time || c.time_ms;
      all_time = all_time && c.time_ms;
    }
  }
  std::int64_t synthetic = (fit_epoch_s + 1) * 1000;
  if (!any_time) ctx.warn("The track has no times; consecutive one-second times were made up for the FIT records.");
  else if (!all_time) ctx.warn("Points without a time got the time of the point before them.");
  std::int64_t last = 0;
  for (auto& s : segments) {
    for (auto& c : s) {
      if (!any_time) {
        c.time_ms = synthetic;
        synthetic += 1000;
      } else if (!c.time_ms) {
        c.time_ms = last;
      }
      last = *c.time_ms;
    }
  }
  bool ele = false, hr = false, cad = false, power = false, speed = false, temp = false;
  for (const auto& s : segments) {
    for (const auto& c : s) {
      ele = ele || c.ele;
      hr = hr || c.heart_rate;
      cad = cad || c.cadence;
      power = power || c.power;
      speed = speed || c.speed;
      temp = temp || c.temperature;
    }
  }
  const std::uint8_t sport_id = sport_value(sport);
  const std::uint32_t start = fit_seconds(*segments.front().front().time_ms);
  const std::uint32_t end = fit_seconds(*segments.back().back().time_ms);
  FitWriter w;
  // file_id
  w.define(0, 0, {{0, 1, 0x00}, {1, 2, 0x84}, {2, 2, 0x84}, {4, 4, 0x86}});
  w.begin(0);
  w.data.u8(4).u16le(255).u16le(0).u32le(start);
  // event (timer start / stop)
  w.define(1, 21, {{253, 4, 0x86}, {0, 1, 0x00}, {1, 1, 0x00}});
  w.begin(1);
  w.data.u32le(start).u8(0).u8(0);
  // record
  std::vector<FieldDef> rec = {{253, 4, 0x86}, {0, 4, 0x85}, {1, 4, 0x85}, {5, 4, 0x86}};
  if (ele) {
    rec.push_back({2, 2, 0x84});
    rec.push_back({78, 4, 0x86});
  }
  if (hr) rec.push_back({3, 1, 0x02});
  if (cad) rec.push_back({4, 1, 0x02});
  if (speed) {
    rec.push_back({6, 2, 0x84});
    rec.push_back({73, 4, 0x86});
  }
  if (power) rec.push_back({7, 2, 0x84});
  if (temp) rec.push_back({13, 1, 0x01});
  w.define(2, 20, rec);
  double distance = 0;
  struct LapInfo {
    std::uint32_t start, end;
    double distance;
    std::optional<double> avg_hr, max_hr;
    std::int32_t start_lat, start_lon, end_lat, end_lon;
  };
  std::vector<LapInfo> lap_info;
  std::size_t written = 0, total = 0;
  for (const auto& s : segments) total += s.size();
  double hr_sum_all = 0;
  std::size_t hr_n_all = 0;
  double hr_max_all = 0;
  for (const auto& s : segments) {
    LapInfo lap{fit_seconds(*s.front().time_ms), fit_seconds(*s.back().time_ms), 0, {}, {}, semicircles(s.front().lat),
                semicircles(s.front().lon), semicircles(s.back().lat), semicircles(s.back().lon)};
    double hr_sum = 0;
    std::size_t hr_n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
      if ((++written & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(written) / static_cast<double>(total)));
      const ir::Coord& c = s[i];
      if (i > 0) {
        const double d = geodesic_distance(s[i - 1], c);
        distance += d;
        lap.distance += d;
      }
      w.begin(2);
      w.data.u32le(fit_seconds(*c.time_ms)).i32le(semicircles(c.lat)).i32le(semicircles(c.lon));
      w.data.u32le(clamp_to<std::uint32_t>(distance * 100, 0, 4294967294.0));
      if (ele) {
        if (c.ele) {
          w.data.u16le(clamp_to<std::uint16_t>((*c.ele + 500) * 5, 0, 65534));
          w.data.u32le(clamp_to<std::uint32_t>((*c.ele + 500) * 5, 0, 4294967294.0));
        } else {
          w.data.u16le(0xFFFF).u32le(0xFFFFFFFF);
        }
      }
      if (hr) w.data.u8(c.heart_rate ? clamp_to<std::uint8_t>(*c.heart_rate, 0, 254) : 0xFF);
      if (cad) w.data.u8(c.cadence ? clamp_to<std::uint8_t>(*c.cadence, 0, 254) : 0xFF);
      if (speed) {
        if (c.speed) {
          w.data.u16le(clamp_to<std::uint16_t>(*c.speed * 1000, 0, 65534));
          w.data.u32le(clamp_to<std::uint32_t>(*c.speed * 1000, 0, 4294967294.0));
        } else {
          w.data.u16le(0xFFFF).u32le(0xFFFFFFFF);
        }
      }
      if (power) w.data.u16le(c.power ? clamp_to<std::uint16_t>(*c.power, 0, 65534) : 0xFFFF);
      if (temp) w.data.i8(c.temperature ? clamp_to<std::int8_t>(*c.temperature, -127, 126) : 0x7F);
      if (c.heart_rate) {
        hr_sum += *c.heart_rate;
        ++hr_n;
        lap.max_hr = lap.max_hr ? std::max(*lap.max_hr, *c.heart_rate) : *c.heart_rate;
      }
    }
    if (hr_n > 0) {
      lap.avg_hr = hr_sum / static_cast<double>(hr_n);
      hr_sum_all += hr_sum;
      hr_n_all += hr_n;
      hr_max_all = std::max(hr_max_all, *lap.max_hr);
    }
    lap_info.push_back(lap);
  }
  w.begin(1);
  w.data.u32le(end).u8(0).u8(9);  // timer stop_all
  // laps
  std::vector<FieldDef> lap_fields = {{253, 4, 0x86}, {2, 4, 0x86}, {7, 4, 0x86}, {8, 4, 0x86}, {9, 4, 0x86},
                                      {3, 4, 0x85},   {4, 4, 0x85}, {5, 4, 0x85}, {6, 4, 0x85}, {0, 1, 0x00},
                                      {1, 1, 0x00},   {254, 2, 0x84}, {25, 1, 0x00}, {15, 1, 0x02}, {16, 1, 0x02}};
  w.define(3, 19, lap_fields);
  for (std::size_t i = 0; i < lap_info.size(); ++i) {
    const LapInfo& l = lap_info[i];
    w.begin(3);
    const std::uint32_t elapsed = (l.end - l.start) * 1000;
    w.data.u32le(l.end).u32le(l.start).u32le(elapsed).u32le(elapsed);
    w.data.u32le(clamp_to<std::uint32_t>(l.distance * 100, 0, 4294967294.0));
    w.data.i32le(l.start_lat).i32le(l.start_lon).i32le(l.end_lat).i32le(l.end_lon);
    w.data.u8(9).u8(1).u16le(static_cast<std::uint16_t>(i)).u8(sport_id);
    w.data.u8(l.avg_hr ? clamp_to<std::uint8_t>(*l.avg_hr, 0, 254) : 0xFF);
    w.data.u8(l.max_hr ? clamp_to<std::uint8_t>(*l.max_hr, 0, 254) : 0xFF);
  }
  // session
  w.define(4, 18, {{253, 4, 0x86}, {2, 4, 0x86}, {7, 4, 0x86}, {8, 4, 0x86}, {9, 4, 0x86}, {5, 1, 0x00},
                   {6, 1, 0x00}, {0, 1, 0x00}, {1, 1, 0x00}, {25, 2, 0x84}, {26, 2, 0x84}, {254, 2, 0x84},
                   {16, 1, 0x02}, {17, 1, 0x02}});
  w.begin(4);
  const std::uint32_t elapsed = (end - start) * 1000;
  w.data.u32le(end).u32le(start).u32le(elapsed).u32le(elapsed);
  w.data.u32le(clamp_to<std::uint32_t>(distance * 100, 0, 4294967294.0));
  w.data.u8(sport_id).u8(0).u8(8).u8(1).u16le(0).u16le(static_cast<std::uint16_t>(lap_info.size())).u16le(0);
  w.data.u8(hr_n_all ? clamp_to<std::uint8_t>(hr_sum_all / static_cast<double>(hr_n_all), 0, 254) : 0xFF);
  w.data.u8(hr_n_all ? clamp_to<std::uint8_t>(hr_max_all, 0, 254) : 0xFF);
  // activity
  w.define(5, 34, {{253, 4, 0x86}, {0, 4, 0x86}, {1, 2, 0x84}, {2, 1, 0x00}, {3, 1, 0x00}, {4, 1, 0x00}});
  w.begin(5);
  w.data.u32le(end).u32le(elapsed).u16le(1).u8(0).u8(26).u8(1);

  const Bytes body = w.data.take();
  util::ByteWriter file(body.size() + 16);
  file.u8(14).u8(0x20).u16le(2132).u32le(static_cast<std::uint32_t>(body.size())).text(".FIT");
  file.u16le(fit_crc(file.view()));
  file.bytes(body);
  file.u16le(fit_crc(file.view()));
  return file.take();
}

}  // namespace

void register_fit(Registry& registry) {
  FormatInfo info;
  info.id = "fit";
  info.name = "Garmin FIT";
  info.description = "Flexible and Interoperable Data Transfer activity files from sports watches and bike computers.";
  info.category = Category::Geo;
  info.extensions = {"fit"};
  info.mime = "application/vnd.ant.fit";
  info.popularity = 50;
  info.signatures = {Signature::at(8, "'.FIT'")};
  registry.add_format(std::move(info));
  GeoCodecSpec spec;
  spec.format_id = "fit";
  spec.read = read_fit;
  spec.write = write_fit;
  spec.traits = GeoWriterTraits{false, true, true, true};
  spec.lossy_write = true;  // attributes other than the sport are not stored
  add_geo_codec(registry, std::move(spec));
}

}  // namespace cvt::geo
