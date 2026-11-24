// Convertible core - data family: Apache Avro object container files.
//
// Reader: the embedded schema (records, enums, arrays, maps, unions, fixed,
// named and recursive types, namespaces), the null and deflate codecs, and
// the logical types date, time-millis/micros, timestamp-millis/micros/nanos,
// local-timestamp-* (-> $date), decimal (-> number or $decimal) and uuid.
// The file becomes an array of its records; union values are plain values.
// Writer: a schema inferred from the data (records for objects whose keys are
// Avro names, maps otherwise, unions with null first where values differ,
// logical types for dates), deflate-compressed blocks, and a sync marker
// derived from the schema so that output is reproducible.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/bytes.hpp"
#include "util/deflate.hpp"
#include "util/hash.hpp"

namespace cvt::data {

namespace {

// =========================================================================== schema

struct AvroType {
  enum class Kind : std::uint8_t { Null, Boolean, Int, Long, Float, Double, Bytes, String, Record, Enum, Array, Map, Union, Fixed };
  Kind kind = Kind::Null;
  std::string name;
  std::string logical;
  std::int64_t scale = 0;
  std::uint64_t size = 0;
  std::vector<std::pair<std::string, std::size_t>> fields;
  std::vector<std::string> symbols;
  std::size_t items = 0;
  std::vector<std::size_t> branches;
};

class AvroSchema {
 public:
  Result<std::size_t> parse(const Json& schema, const std::string& space, std::size_t depth) {
    if (depth > 128) return fail(Status::Limit, "the Avro schema is nested too deeply");
    if (types_.size() > 100'000) return fail(Status::Limit, "the Avro schema is too large");
    if (schema.is_string()) return named_or_primitive(schema.as_string(), space);
    if (schema.is_array()) {
      AvroType u;
      u.kind = AvroType::Kind::Union;
      for (const Json& branch : schema.elements()) {
        CVT_TRY_ASSIGN(const std::size_t index, parse(branch, space, depth + 1));
        u.branches.push_back(index);
      }
      if (u.branches.empty()) return fail(Status::Decode, "the Avro schema has an empty union");
      types_.push_back(std::move(u));
      return types_.size() - 1;
    }
    if (!schema.is_object()) return fail(Status::Decode, "invalid Avro schema");
    const std::string type = schema["type"].as_string();
    if (schema["type"].is_structured()) return parse(schema["type"], space, depth + 1);
    if (type == "record" || type == "error" || type == "enum" || type == "fixed") {
      std::string name = schema["name"].as_string();
      std::string ns = schema.contains("namespace") ? schema["namespace"].as_string() : space;
      if (name.find('.') != std::string::npos) ns = name.substr(0, name.rfind('.'));
      const std::string full = name.find('.') != std::string::npos || ns.empty() ? name : ns + "." + name;
      const std::size_t index = types_.size();
      types_.emplace_back();
      names_[full] = index;
      AvroType t;
      t.name = full;
      if (type == "enum") {
        t.kind = AvroType::Kind::Enum;
        for (const Json& symbol : schema["symbols"].elements()) t.symbols.push_back(symbol.as_string());
      } else if (type == "fixed") {
        t.kind = AvroType::Kind::Fixed;
        t.size = schema["size"].as_uint64(0);
        t.logical = schema["logicalType"].as_string();
        t.scale = schema["scale"].as_int64(0);
      } else {
        t.kind = AvroType::Kind::Record;
        for (const Json& field : schema["fields"].elements()) {
          CVT_TRY_ASSIGN(const std::size_t field_type, parse(field["type"], ns, depth + 1));
          t.fields.emplace_back(field["name"].as_string(), field_type);
        }
      }
      types_[index] = std::move(t);
      return index;
    }
    AvroType t;
    if (type == "array") {
      CVT_TRY_ASSIGN(t.items, parse(schema["items"], space, depth + 1));
      t.kind = AvroType::Kind::Array;
    } else if (type == "map") {
      CVT_TRY_ASSIGN(t.items, parse(schema["values"], space, depth + 1));
      t.kind = AvroType::Kind::Map;
    } else {
      CVT_TRY_ASSIGN(const std::size_t base, named_or_primitive(type, space));
      t = types_[base];
    }
    t.logical = schema["logicalType"].as_string();
    t.scale = schema["scale"].as_int64(0);
    types_.push_back(std::move(t));
    return types_.size() - 1;
  }

  [[nodiscard]] const AvroType& at(std::size_t index) const { return types_[index]; }

 private:
  Result<std::size_t> named_or_primitive(const std::string& name, const std::string& space) {
    static constexpr std::pair<std::string_view, AvroType::Kind> primitives[] = {
        {"null", AvroType::Kind::Null},     {"boolean", AvroType::Kind::Boolean}, {"int", AvroType::Kind::Int},
        {"long", AvroType::Kind::Long},     {"float", AvroType::Kind::Float},     {"double", AvroType::Kind::Double},
        {"bytes", AvroType::Kind::Bytes},   {"string", AvroType::Kind::String}};
    for (const auto& [primitive, kind] : primitives) {
      if (name == primitive) {
        AvroType t;
        t.kind = kind;
        types_.push_back(std::move(t));
        return types_.size() - 1;
      }
    }
    if (const auto found = names_.find(name); found != names_.end()) return found->second;
    if (!space.empty()) {
      if (const auto found = names_.find(space + "." + name); found != names_.end()) return found->second;
    }
    return fail(Status::Decode, "the Avro schema refers to an unknown type '{}'", name);
  }

  std::vector<AvroType> types_;
  std::map<std::string, std::size_t, std::less<>> names_;
};

// =========================================================================== reader

class AvroDecoder {
 public:
  AvroDecoder(const AvroSchema& schema, ByteView data, NodeBudget& budget, std::size_t max_depth)
      : schema_(schema), reader_(data), budget_(budget), max_depth_(max_depth) {}

  [[nodiscard]] bool at_end() const { return reader_.at_end(); }
  [[nodiscard]] std::size_t position() const { return reader_.position(); }

  Result<std::int64_t> long_value() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 70; shift += 7) {
      if (reader_.at_end()) return truncated();
      const std::uint8_t b = reader_.u8();
      value |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) return unzigzag(value);
    }
    return fail(Status::Decode, "invalid Avro varint");
  }

  Result<ByteView> bytes() {
    CVT_TRY_ASSIGN(const std::int64_t size, long_value());
    if (size < 0 || static_cast<std::uint64_t>(size) > reader_.remaining()) return truncated();
    return reader_.bytes(static_cast<std::size_t>(size));
  }

  Result<Json> datum(std::size_t index, std::size_t depth) {
    if (depth > max_depth_) {
      return fail(Status::Limit, "the Avro data is nested more than {} levels deep (option max_depth)", max_depth_);
    }
    CVT_TRY(budget_.take());
    const AvroType& t = schema_.at(index);
    switch (t.kind) {
      case AvroType::Kind::Null: return Json();
      case AvroType::Kind::Boolean: {
        if (reader_.at_end()) return truncated();
        return Json(reader_.u8() != 0);
      }
      case AvroType::Kind::Int:
      case AvroType::Kind::Long: {
        CVT_TRY_ASSIGN(const std::int64_t v, long_value());
        return logical_integer(t, v);
      }
      case AvroType::Kind::Float:
        if (reader_.remaining() < 4) return truncated();
        return Json(static_cast<double>(reader_.f32le()));
      case AvroType::Kind::Double:
        if (reader_.remaining() < 8) return truncated();
        return Json(reader_.f64le());
      case AvroType::Kind::Bytes: {
        CVT_TRY_ASSIGN(const ByteView data, bytes());
        return logical_bytes(t, data);
      }
      case AvroType::Kind::String: {
        CVT_TRY_ASSIGN(const ByteView data, bytes());
        return text_value(as_chars(data), repaired_);
      }
      case AvroType::Kind::Fixed: {
        if (t.size > reader_.remaining()) return truncated();
        return logical_bytes(t, reader_.bytes(static_cast<std::size_t>(t.size)));
      }
      case AvroType::Kind::Enum: {
        CVT_TRY_ASSIGN(const std::int64_t symbol, long_value());
        if (symbol < 0 || static_cast<std::uint64_t>(symbol) >= t.symbols.size()) {
          return fail(Status::Decode, "Avro enum index {} out of range", symbol);
        }
        return Json(t.symbols[static_cast<std::size_t>(symbol)]);
      }
      case AvroType::Kind::Union: {
        CVT_TRY_ASSIGN(const std::int64_t branch, long_value());
        if (branch < 0 || static_cast<std::uint64_t>(branch) >= t.branches.size()) {
          return fail(Status::Decode, "Avro union index {} out of range", branch);
        }
        return datum(t.branches[static_cast<std::size_t>(branch)], depth + 1);
      }
      case AvroType::Kind::Record: {
        Json object = Json::object();
        for (const auto& [name, field] : t.fields) {
          CVT_TRY_ASSIGN(Json value, datum(field, depth + 1));
          object.set(name, std::move(value));
        }
        return object;
      }
      case AvroType::Kind::Array:
      case AvroType::Kind::Map: {
        const bool map = t.kind == AvroType::Kind::Map;
        Json out = map ? Json::object() : Json::array();
        while (true) {
          CVT_TRY_ASSIGN(std::int64_t count, long_value());
          if (count == 0) break;
          if (count < 0) {
            if (count == std::numeric_limits<std::int64_t>::min()) return fail(Status::Decode, "invalid Avro block count");
            count = -count;
            CVT_TRY_ASSIGN(const std::int64_t block_size, long_value());
            (void)block_size;
          }
          CVT_TRY(budget_.take(static_cast<std::uint64_t>(count)));
          for (std::int64_t i = 0; i < count; ++i) {
            if (map) {
              CVT_TRY_ASSIGN(const ByteView key, bytes());
              CVT_TRY_ASSIGN(Json value, datum(t.items, depth + 1));
              out.set(text_value(as_chars(key), repaired_).as_string(), std::move(value));
            } else {
              CVT_TRY_ASSIGN(Json value, datum(t.items, depth + 1));
              out.push_back(std::move(value));
            }
          }
        }
        return out;
      }
    }
    return Json();
  }

  std::size_t repaired_ = 0;

 private:
  std::unexpected<Error> truncated() const { return fail(Status::Decode, "the Avro data block is truncated"); }

  static Json logical_integer(const AvroType& t, std::int64_t v) {
    const auto instant = [&](std::int64_t per_second, bool local) {
      std::int64_t seconds = v / per_second;
      std::int64_t rest = v % per_second;
      if (rest < 0) {
        rest += per_second;
        --seconds;
      }
      DateTime d = from_unix(seconds, static_cast<std::uint32_t>(rest * (1'000'000'000 / per_second)));
      if (local) d.kind = DateTime::Kind::Local;
      return make_date(format_datetime(d));
    };
    if (t.logical == "date") {
      DateTime d = from_unix(v * 86400);
      d.kind = DateTime::Kind::Date;
      return make_date(format_datetime(d));
    }
    if (t.logical == "time-millis" || t.logical == "time-micros") {
      const std::int64_t per_second = t.logical == "time-millis" ? 1000 : 1'000'000;
      if (v < 0 || v >= 86400 * per_second) return Json(v);
      DateTime d = from_unix(v / per_second, static_cast<std::uint32_t>(v % per_second * (1'000'000'000 / per_second)));
      d.kind = DateTime::Kind::Time;
      return make_date(format_datetime(d));
    }
    if (t.logical == "timestamp-millis") return instant(1000, false);
    if (t.logical == "timestamp-micros") return instant(1'000'000, false);
    if (t.logical == "timestamp-nanos") return instant(1'000'000'000, false);
    if (t.logical == "local-timestamp-millis") return instant(1000, true);
    if (t.logical == "local-timestamp-micros") return instant(1'000'000, true);
    if (t.logical == "local-timestamp-nanos") return instant(1'000'000'000, true);
    return Json(v);
  }

  static Json logical_bytes(const AvroType& t, ByteView data) {
    if (t.logical == "decimal" && data.size() <= 64) {
      std::string digits = twos_complement_to_decimal(data);
      const bool negative = digits.starts_with('-');
      if (negative) digits.erase(0, 1);
      return decimal_value(negative, digits, -t.scale);
    }
    return make_binary(data);
  }

  const AvroSchema& schema_;
  util::ByteReader reader_;
  NodeBudget& budget_;
  std::size_t max_depth_ = 256;
};

Result<Json> parse_avro(ByteView input, ReadContext& ctx) {
  util::ByteReader reader(input);
  if (!reader.consume(std::string_view("Obj\x01", 4))) return fail(Status::Decode, "not an Avro object container file");
  NodeBudget budget(input.size());
  const std::size_t max_depth = read_policy(ctx.options).max_depth;
  // Header metadata: a map of bytes.
  AvroSchema meta_schema;
  CVT_TRY_ASSIGN(const std::size_t meta_type, meta_schema.parse(Json::object({{"type", "map"}, {"values", "bytes"}}), "", 0));
  AvroDecoder header(meta_schema, reader.peek_rest(), budget, max_depth);
  CVT_TRY_ASSIGN(const Json meta, header.datum(meta_type, 0));
  (void)reader.bytes(header.position());
  if (reader.remaining() < 16) return fail(Status::Decode, "truncated Avro header");
  const ByteView sync = reader.bytes(16);
  const auto schema_bytes = tagged_bytes(meta["avro.schema"]);
  if (!schema_bytes) return fail(Status::Decode, "the Avro file has no schema");
  CVT_TRY_ASSIGN(const Json schema_json, util::parse_json(as_chars(*schema_bytes)));
  const std::string codec = meta.contains("avro.codec") ? to_string(tagged_bytes(meta["avro.codec"]).value_or(Bytes{})) : "null";
  if (codec != "null" && codec != "deflate") {
    return fail(Status::Decode, "the Avro codec '{}' is not supported (only null and deflate)", codec);
  }
  AvroSchema schema;
  CVT_TRY_ASSIGN(const std::size_t root, schema.parse(schema_json, "", 0));
  Json records = Json::array();
  std::uint64_t decompressed = 0;
  std::size_t repaired = 0;
  while (!reader.at_end()) {
    AvroDecoder block_header(meta_schema, reader.peek_rest(), budget, max_depth);
    CVT_TRY_ASSIGN(const std::int64_t count, block_header.long_value());
    CVT_TRY_ASSIGN(const std::int64_t size, block_header.long_value());
    (void)reader.bytes(block_header.position());
    if (count < 0 || size < 0 || static_cast<std::uint64_t>(size) > reader.remaining()) {
      return fail(Status::Decode, "damaged Avro data block");
    }
    ByteView block = reader.bytes(static_cast<std::size_t>(size));
    Bytes inflated;
    if (codec == "deflate") {
      CVT_TRY_ASSIGN(inflated, util::inflate_raw(block, ctx.limits.max_decompressed_bytes - std::min(decompressed, ctx.limits.max_decompressed_bytes)));
      decompressed += inflated.size();
      CVT_TRY(ctx.limits.check_decompressed(decompressed));
      block = inflated;
    }
    AvroDecoder decoder(schema, block, budget, max_depth);
    for (std::int64_t i = 0; i < count; ++i) {
      CVT_TRY_ASSIGN(Json record, decoder.datum(root, 0));
      records.push_back(std::move(record));
    }
    repaired += decoder.repaired_;
    if (reader.remaining() < 16 || as_chars(reader.bytes(16)) != as_chars(sync)) {
      return fail(Status::Decode, "the Avro sync marker after a data block does not match");
    }
    CVT_TRY(ctx.checkpoint(1.0 - static_cast<double>(reader.remaining()) / static_cast<double>(input.size())));
  }
  if (repaired > 0) ctx.warn(std::format("{} string(s) were not valid UTF-8 and were repaired.", repaired));
  return records;
}

// =========================================================================== schema inference and writer

bool avro_name(std::string_view name) {
  if (name.empty() || !(util::is_ascii_alpha(name[0]) || name[0] == '_')) return false;
  return std::ranges::all_of(name, [](char c) { return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_'; });
}

class SchemaInference {
 public:
  Json infer(const Json& value) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext: return Json("bytes");
      case TagKind::Date: {
        const auto parsed = parse_datetime(tag_text(value));
        if (!parsed) return Json("string");
        switch (parsed->kind) {
          case DateTime::Kind::Date: return Json::object({{"type", "int"}, {"logicalType", "date"}});
          case DateTime::Kind::Time: return Json::object({{"type", "long"}, {"logicalType", "time-micros"}});
          case DateTime::Kind::Local: return Json::object({{"type", "long"}, {"logicalType", "local-timestamp-micros"}});
          case DateTime::Kind::Offset: return Json::object({{"type", "long"}, {"logicalType", "timestamp-micros"}});
        }
        return Json("string");
      }
      case TagKind::Float: return Json("double");
      default:
        ++converted_;
        return Json("string");
    }
    switch (value.type()) {
      case Json::Type::Null: return Json("null");
      case Json::Type::Bool: return Json("boolean");
      case Json::Type::Int64: return Json("long");
      case Json::Type::UInt64:
        ++converted_;
        return Json("string");
      case Json::Type::Double: return Json("double");
      case Json::Type::String: return Json("string");
      case Json::Type::Array: {
        Json items = Json("null");
        bool first = true;
        for (const Json& element : value.elements()) {
          items = first ? infer(element) : merge(items, infer(element));
          first = false;
        }
        return Json::object({{"type", "array"}, {"items", items}});
      }
      case Json::Type::Object: {
        if (!std::ranges::all_of(value.items(), [](const auto& m) { return avro_name(m.key()); }) || value.empty()) {
          Json values = Json("null");
          bool first = true;
          for (const auto& member : value.items()) {
            values = first ? infer(member.value()) : merge(values, infer(member.value()));
            first = false;
          }
          return Json::object({{"type", "map"}, {"values", values}});
        }
        Json fields = Json::array();
        for (const auto& member : value.items()) {
          fields.push_back(Json::object({{"name", member.key()}, {"type", infer(member.value())}}));
        }
        return Json::object({{"type", "record"}, {"name", "R"}, {"fields", fields}});
      }
    }
    return Json("null");
  }

  /// Combine two schemas into one that accepts values of both.
  Json merge(const Json& a, const Json& b) {
    std::vector<Json> branches;
    const auto add = [&](const Json& schema) {
      if (schema.is_array()) {
        for (const Json& s : schema.elements()) add_branch(branches, s);
      } else {
        add_branch(branches, schema);
      }
    };
    add(a);
    add(b);
    if (branches.size() == 1) return branches.front();
    // null first so that it can be the default.
    std::ranges::stable_partition(branches, [](const Json& s) { return s.is_string() && s.as_string() == "null"; });
    Json out = Json::array();
    for (Json& branch : branches) out.push_back(std::move(branch));
    return out;
  }

  /// Give every record a unique name and make missing fields optional.
  void finalize(Json& schema, const std::string& hint) {
    if (schema.is_array()) {
      for (Json& branch : schema.elements()) finalize(branch, hint);
      return;
    }
    if (!schema.is_object()) return;
    const std::string type = schema["type"].as_string();
    if (type == "record") {
      schema.set("name", unique(hint));
      for (Json& field : schema["fields"].elements()) {
        finalize(field["type"], hint + "_" + field["name"].as_string());
        if (field["type"].is_array() && field["type"][0].is_string() && field["type"][0].as_string() == "null") {
          field.set("default", Json());
        }
      }
    } else if (type == "array") {
      finalize(schema["items"], hint);
    } else if (type == "map") {
      finalize(schema["values"], hint);
    }
  }

  std::size_t converted_ = 0;

 private:
  static std::string kind_of(const Json& schema) {
    if (schema.is_string()) return schema.as_string();
    return schema["type"].as_string() + "/" + schema["logicalType"].as_string();
  }

  void add_branch(std::vector<Json>& branches, const Json& schema) {
    const std::string kind = kind_of(schema);
    for (Json& existing : branches) {
      if (kind_of(existing) != kind) continue;
      if (kind == "record/") {
        // Union of fields; a field missing on either side becomes optional.
        Json fields = Json::array();
        const Json& left = existing["fields"];
        const Json& right = schema["fields"];
        const auto find = [](const Json& list, const std::string& name) -> const Json* {
          for (const Json& f : list.elements()) {
            if (f["name"].as_string() == name) return &f;
          }
          return nullptr;
        };
        for (const Json& f : left.elements()) {
          const Json* other = find(right, f["name"].as_string());
          Json type = other != nullptr ? merge(f["type"], (*other)["type"]) : merge(f["type"], Json("null"));
          fields.push_back(Json::object({{"name", f["name"]}, {"type", type}}));
        }
        for (const Json& f : right.elements()) {
          if (find(left, f["name"].as_string()) != nullptr) continue;
          fields.push_back(Json::object({{"name", f["name"]}, {"type", merge(f["type"], Json("null"))}}));
        }
        existing.set("fields", fields);
      } else if (kind == "array/") {
        existing.set("items", merge(existing["items"], schema["items"]));
      } else if (kind == "map/") {
        existing.set("values", merge(existing["values"], schema["values"]));
      }
      return;
    }
    // A record and a map of the same values cannot both be told apart cheaply;
    // that is fine: unions may hold one of each.
    branches.push_back(schema);
  }

  std::string unique(const std::string& hint) {
    std::string base;
    for (const char c : hint) base.push_back(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' ? c : '_');
    if (base.empty() || util::is_ascii_digit(base[0])) base.insert(0, "R");
    std::string name = base;
    for (int n = 2; used_.contains(name); ++n) name = base + std::to_string(n);
    used_.insert(name);
    return name;
  }

  std::set<std::string, std::less<>> used_;
};

class AvroEncoder {
 public:
  explicit AvroEncoder(const AvroSchema& schema) : schema_(schema) {}

  void put_long(std::int64_t value) {
    std::uint64_t v = zigzag(value);
    while (v >= 0x80) {
      out_.u8(static_cast<std::uint8_t>(v | 0x80));
      v >>= 7;
    }
    out_.u8(static_cast<std::uint8_t>(v));
  }
  void put_bytes(ByteView data) {
    put_long(static_cast<std::int64_t>(data.size()));
    out_.bytes(data);
  }

  [[nodiscard]] bool matches(std::size_t index, const Json& value) const {
    const AvroType& t = schema_.at(index);
    const TagKind kind = tag_kind(value);
    switch (t.kind) {
      case AvroType::Kind::Null: return value.is_null();
      case AvroType::Kind::Boolean: return value.is_bool();
      case AvroType::Kind::Int:
      case AvroType::Kind::Long:
        if (!t.logical.empty()) {
          if (kind != TagKind::Date) return false;
          const auto parsed = parse_datetime(tag_text(value));
          if (!parsed) return false;
          if (t.logical == "date") return parsed->kind == DateTime::Kind::Date;
          if (t.logical.starts_with("time-")) return parsed->kind == DateTime::Kind::Time;
          if (t.logical.starts_with("local-")) return parsed->kind == DateTime::Kind::Local;
          return parsed->kind == DateTime::Kind::Offset;
        }
        return value.is_integer() && !value.is_unsigned();
      case AvroType::Kind::Float:
      case AvroType::Kind::Double: return value.is_float() || kind == TagKind::Float;
      case AvroType::Kind::Bytes:
      case AvroType::Kind::Fixed: return kind == TagKind::Binary || kind == TagKind::Ext;
      case AvroType::Kind::String:
        return value.is_string() || value.is_unsigned() ||
               (kind != TagKind::None && kind != TagKind::Binary && kind != TagKind::Ext && kind != TagKind::Float &&
                !(kind == TagKind::Date && parse_datetime(tag_text(value))));
      case AvroType::Kind::Record: return value.is_object() && kind == TagKind::None;
      case AvroType::Kind::Map: return value.is_object() && kind == TagKind::None;
      case AvroType::Kind::Array: return value.is_array();
      case AvroType::Kind::Enum: return value.is_string();
      case AvroType::Kind::Union: return false;
    }
    return false;
  }

  Result<void> datum(std::size_t index, const Json& value) {
    const AvroType& t = schema_.at(index);
    switch (t.kind) {
      case AvroType::Kind::Null: return {};
      case AvroType::Kind::Boolean: out_.u8(value.as_bool() ? 1 : 0); return {};
      case AvroType::Kind::Int:
      case AvroType::Kind::Long: {
        if (t.logical.empty()) {
          put_long(value.as_int64());
          return {};
        }
        const auto parsed = parse_datetime(tag_text(value));
        if (!parsed) return fail(Status::Encode, "invalid date value");
        if (t.logical == "date") {
          put_long(days_from_civil(parsed->year, parsed->month, parsed->day));
        } else {
          put_long(to_unix_seconds(*parsed) * 1'000'000 + parsed->nanos() / 1000);
        }
        return {};
      }
      case AvroType::Kind::Float: out_.f32le(static_cast<float>(plain_value(value).as_double())); return {};
      case AvroType::Kind::Double: out_.f64le(plain_value(value).as_double()); return {};
      case AvroType::Kind::Bytes:
      case AvroType::Kind::Fixed: {
        const auto data = tagged_bytes(value);
        if (!data) return fail(Status::Encode, "invalid Base64 in a binary value");
        if (t.kind == AvroType::Kind::Bytes) {
          put_bytes(*data);
        } else {
          out_.bytes(*data);
        }
        return {};
      }
      case AvroType::Kind::String: {
        std::string text = value.is_string() ? value.as_string() : value.is_unsigned() ? format_integer(value) : std::string(tag_text(value));
        if (!value.is_string() && !value.is_unsigned() && plain_value(value).is_string()) text = plain_value(value).as_string();
        put_bytes(as_bytes(text));
        return {};
      }
      case AvroType::Kind::Enum: {
        const auto found = std::ranges::find(t.symbols, value.as_string());
        put_long(found - t.symbols.begin());
        return {};
      }
      case AvroType::Kind::Union: {
        for (std::size_t b = 0; b < t.branches.size(); ++b) {
          if (matches(t.branches[b], value)) {
            put_long(static_cast<std::int64_t>(b));
            return datum(t.branches[b], value);
          }
        }
        return fail(Status::Encode, "a value does not fit the inferred Avro schema");
      }
      case AvroType::Kind::Record:
        for (const auto& [name, field] : t.fields) {
          const Json* member = value.find(name);
          static const Json null_value;
          CVT_TRY(datum(field, member != nullptr ? *member : null_value));
        }
        return {};
      case AvroType::Kind::Array:
        if (!value.empty()) {
          put_long(static_cast<std::int64_t>(value.size()));
          for (const Json& element : value.elements()) CVT_TRY(datum(t.items, element));
        }
        put_long(0);
        return {};
      case AvroType::Kind::Map:
        if (!value.empty()) {
          put_long(static_cast<std::int64_t>(value.size()));
          for (const auto& member : value.items()) {
            put_bytes(as_bytes(member.key()));
            CVT_TRY(datum(t.items, member.value()));
          }
        }
        put_long(0);
        return {};
    }
    return {};
  }

  Bytes take() { return out_.take(); }
  [[nodiscard]] std::size_t size() const { return out_.size(); }

 private:
  const AvroSchema& schema_;
  util::ByteWriter out_;
};

Result<Bytes> write_avro(const Json& value, WriteContext& ctx) {
  std::vector<const Json*> records;
  if (value.is_array()) {
    for (const Json& element : value.elements()) records.push_back(&element);
  } else {
    records.push_back(&value);
  }
  SchemaInference inference;
  Json schema_json = Json("null");
  for (std::size_t i = 0; i < records.size(); ++i) {
    Json next = inference.infer(*records[i]);
    if (i == 0) {
      schema_json = std::move(next);
    } else if (!next.equal_ordered(schema_json)) {  // records usually share one shape
      schema_json = inference.merge(schema_json, next);
    }
    if ((i & 1023) == 0) CVT_TRY(ctx.checkpoint(0.3 * static_cast<double>(i) / static_cast<double>(records.size())));
  }
  inference.finalize(schema_json, "Record");
  if (inference.converted_ > 0) {
    ctx.warn(std::format("{} value(s) without an Avro type (big numbers and special values) were written as strings.",
                         inference.converted_));
  }
  const std::string schema_text = util::dump_json(schema_json);
  AvroSchema schema;
  CVT_TRY_ASSIGN(const std::size_t root, schema.parse(schema_json, "", 0));
  const std::string codec = ctx.options.get_enum("avro_codec", "deflate", {"deflate", "null"});
  const int level = options::resolved_compression_level(ctx.options, 9);

  util::ByteWriter out;
  out.text(std::string_view("Obj\x01", 4));
  AvroEncoder header(schema);
  header.put_long(2);
  header.put_bytes(as_bytes(std::string_view("avro.codec")));
  header.put_bytes(as_bytes(codec));
  header.put_bytes(as_bytes(std::string_view("avro.schema")));
  header.put_bytes(as_bytes(schema_text));
  header.put_long(0);
  out.bytes(header.take());
  const auto digest = util::md5(as_bytes(schema_text));
  out.bytes(std::span<const std::uint8_t>(digest.data(), digest.size()));

  std::size_t index = 0;
  while (index < records.size()) {
    AvroEncoder block(schema);
    std::size_t count = 0;
    while (index < records.size() && count < 4096 && block.size() < (1u << 20)) {
      CVT_TRY(block.datum(root, *records[index]));
      ++index;
      ++count;
    }
    Bytes data = block.take();
    if (codec == "deflate") {
      CVT_TRY_ASSIGN(data, util::deflate_raw(data, level));
    }
    AvroEncoder block_header(schema);
    block_header.put_long(static_cast<std::int64_t>(count));
    block_header.put_long(static_cast<std::int64_t>(data.size()));
    out.bytes(block_header.take());
    out.bytes(data);
    out.bytes(std::span<const std::uint8_t>(digest.data(), digest.size()));
    CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(records.size())));
  }
  return out.take();
}

}  // namespace

void register_avro(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "avro",
      .name = "Apache Avro",
      .description = "Avro object container file: records with an embedded schema.",
      .category = Category::Data,
      .extensions = {"avro"},
      .mime = "application/avro",
      .popularity = 25,
      .signatures = {Signature::at(0, "'Obj' 01")},
      .sniff = nullptr,
  });
  add_codec(registry, {.format_id = "avro",
                       .read = binary_reader(parse_avro),
                       .write = binary_writer(write_avro, "Avro"),
                       .read_options = common_read_options(false, false),
                       .write_options = {
                           OptionSpec::choice("avro_codec", "Compression", "deflate", {"deflate", "null"},
                                              "Compress the data blocks with deflate, or store them as they are."),
                           options::compression_level(9),
                       }});
}

}  // namespace cvt::data
