// Convertible core - data family: Protocol Buffers wire format without a schema.
//
// Decoding turns a message into an object keyed by field number ("1", "2",
// ...; repeated fields become arrays). Without the .proto file the types are
// guessed, best effort:
//   varint           -> integer (values >= 2^63 as negative int64, the usual
//                       encoding of negative int32/int64)
//   64-bit / 32-bit  -> double / float when the bits look like an ordinary
//                       number, else the unsigned integer (option `fixed_as`)
//   length-delimited -> text when it is printable UTF-8, else a nested message
//                       when it parses completely as one, else $binary
//   groups           -> nested objects
// With `typed_keys` the wire type is kept in the key ("3:f32") so that
// encoding reproduces the same wire types. Encoding accepts keys "N" or
// "N:type" (type: varint, sint, f32, f64, float, double, bytes); integers
// become varints, doubles 64-bit fields, strings/binary/objects
// length-delimited fields and arrays repeated fields.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/bytes.hpp"

namespace cvt::data {

namespace {

struct ProtoSettings {
  std::string fixed_as = "auto";
  bool typed_keys = false;
  std::size_t max_depth = 64;
};

Result<std::uint64_t> varint(ByteView data, std::size_t& p) {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 70; shift += 7) {
    if (p >= data.size()) return fail(Status::Decode, "truncated varint at byte {}", p);
    const auto b = std::to_integer<std::uint64_t>(data[p++]);
    if (shift == 63 && b > 1) return fail(Status::Decode, "varint too long at byte {}", p);
    value |= (b & 0x7F) << shift;
    if ((b & 0x80) == 0) return value;
  }
  return fail(Status::Decode, "varint too long at byte {}", p);
}

bool plausible_double(double d) {
  if (d == 0.0) return true;
  if (!std::isfinite(d)) return false;
  const double magnitude = std::abs(d);
  return magnitude >= 1e-30 && magnitude <= 1e30;
}

bool printable_text(std::string_view text) {
  if (!util::is_valid_utf8(text)) return false;
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 && c != '\n' && c != '\r' && c != '\t') return false;
    if (u == 0x7F) return false;
  }
  return true;
}

class ProtoDecoder {
 public:
  ProtoDecoder(const ProtoSettings& settings, NodeBudget& budget) : settings_(settings), budget_(budget) {}

  /// Strict parse of a whole buffer as a message; nullopt when it is not one.
  Result<std::optional<Json>> message(ByteView data, std::size_t depth, std::uint64_t end_group = 0) {
    if (depth > settings_.max_depth) return std::optional<Json>();
    Json object = Json::object();
    std::size_t p = 0;
    while (p < data.size()) {
      auto key = varint(data, p);
      if (!key) return std::optional<Json>();
      const std::uint64_t field = *key >> 3;
      const auto wire = static_cast<int>(*key & 7);
      if (field == 0 || field > 536'870'911) return std::optional<Json>();
      if (wire == 4) {
        if (end_group != 0 && field == end_group) {
          consumed_ = p;
          return std::optional<Json>(std::move(object));
        }
        return std::optional<Json>();
      }
      CVT_TRY(budget_.take());
      Json value;
      std::string type;
      switch (wire) {
        case 0: {
          auto v = varint(data, p);
          if (!v) return std::optional<Json>();
          value = Json(static_cast<std::int64_t>(*v));  // two's complement: negative int32/int64
          type = "varint";
          break;
        }
        case 1: {
          if (data.size() - p < 8) return std::optional<Json>();
          util::ByteReader r(data.subspan(p, 8));
          const std::uint64_t bits = r.u64le();
          p += 8;
          const double d = std::bit_cast<double>(bits);
          const bool as_float = settings_.fixed_as == "float" || (settings_.fixed_as == "auto" && plausible_double(d));
          value = as_float ? Json(d) : Json(bits);
          type = as_float ? "double" : "f64";
          break;
        }
        case 5: {
          if (data.size() - p < 4) return std::optional<Json>();
          util::ByteReader r(data.subspan(p, 4));
          const std::uint32_t bits = r.u32le();
          p += 4;
          const auto f = static_cast<double>(std::bit_cast<float>(bits));
          const bool as_float = settings_.fixed_as == "float" || (settings_.fixed_as == "auto" && plausible_double(f));
          value = as_float ? Json(f) : Json(static_cast<std::int64_t>(bits));
          type = as_float ? "float" : "f32";
          break;
        }
        case 2: {
          auto size = varint(data, p);
          if (!size || *size > data.size() - p) return std::optional<Json>();
          const ByteView payload = data.subspan(p, static_cast<std::size_t>(*size));
          p += static_cast<std::size_t>(*size);
          CVT_TRY_ASSIGN(value, length_delimited(payload, depth));
          type = value.is_string() ? "string" : value.is_object() && tag_kind(value) == TagKind::None ? "message" : "bytes";
          break;
        }
        case 3: {
          ProtoDecoder inner(settings_, budget_);
          CVT_TRY_ASSIGN(auto group, inner.message(data.subspan(p), depth + 1, field));
          if (!group) return std::optional<Json>();
          p += inner.consumed_;
          value = std::move(*group);
          type = "group";
          break;
        }
        default: return std::optional<Json>();
      }
      std::string name = std::to_string(field);
      if (settings_.typed_keys) name += ":" + type;
      if (Json* existing = object.find(name)) {
        if (!existing->is_array() || !repeated_.contains(name)) {
          Json first = std::move(*existing);
          *existing = Json::array({std::move(first)});
          repeated_.insert(name);
        }
        existing->push_back(std::move(value));
      } else {
        object.set(name, std::move(value));
      }
    }
    if (end_group != 0) return std::optional<Json>();  // missing END_GROUP
    consumed_ = p;
    return std::optional<Json>(std::move(object));
  }

  std::size_t consumed_ = 0;

 private:
  Result<Json> length_delimited(ByteView payload, std::size_t depth) {
    const std::string_view text = as_chars(payload);
    if (printable_text(text)) return Json(std::string(text));
    ProtoDecoder nested(settings_, budget_);
    CVT_TRY_ASSIGN(auto parsed, nested.message(payload, depth + 1));
    if (parsed && !parsed->empty()) return std::move(*parsed);
    return make_binary(payload);
  }

  const ProtoSettings& settings_;
  NodeBudget& budget_;
  std::set<std::string, std::less<>> repeated_;
};

Result<Json> parse_protobuf(ByteView input, ReadContext& ctx) {
  ProtoSettings settings;
  settings.fixed_as = ctx.options.get_enum("fixed_as", "auto", {"auto", "int", "float"});
  settings.typed_keys = ctx.options.get_bool("typed_keys", false);
  settings.max_depth = read_policy(ctx.options).max_depth;
  NodeBudget budget(input.size());
  ProtoDecoder decoder(settings, budget);
  CVT_TRY_ASSIGN(auto message, decoder.message(input, 0));
  if (!message) {
    return fail(Status::Decode, "the data is not a Protocol Buffers message (invalid field key or length)");
  }
  ctx.warn("Without a .proto schema the field types were guessed; numbers are field numbers.");
  return std::move(*message);
}

// =========================================================================== encoder

class ProtoEncoder {
 public:
  Result<Bytes> run(const Json& value) {
    if (!value.is_object() || tag_kind(value) != TagKind::None) {
      return fail(Status::Encode, "a Protocol Buffers message must be an object keyed by field numbers");
    }
    CVT_TRY(message(value, out_));
    return out_.take();
  }

 private:
  static void put_varint(util::ByteWriter& out, std::uint64_t v) {
    while (v >= 0x80) {
      out.u8(static_cast<std::uint8_t>(v | 0x80));
      v >>= 7;
    }
    out.u8(static_cast<std::uint8_t>(v));
  }

  Result<void> message(const Json& object, util::ByteWriter& out) {
    for (const auto& member : object.items()) {
      const std::string_view key = member.key();
      const std::size_t colon = key.find(':');
      const std::string_view number = key.substr(0, colon);
      const std::string_view type = colon == std::string_view::npos ? std::string_view() : key.substr(colon + 1);
      const auto field = util::parse_int(number);
      if (!field || *field < 1 || *field > 536'870'911) {
        return fail(Status::Encode, "'{}' is not a field number (Protocol Buffers keys are numbers such as \"1\")", key);
      }
      if (member.value().is_array() && tag_kind(member.value()) == TagKind::None) {
        for (const Json& element : member.value().elements()) {
          CVT_TRY(field_value(static_cast<std::uint64_t>(*field), type, element, out));
        }
      } else {
        CVT_TRY(field_value(static_cast<std::uint64_t>(*field), type, member.value(), out));
      }
    }
    return {};
  }

  Result<void> field_value(std::uint64_t field, std::string_view type, const Json& value, util::ByteWriter& out) {
    const auto key = [&](int wire) { put_varint(out, (field << 3) | static_cast<std::uint64_t>(wire)); };
    const auto bytes = [&](ByteView payload) {
      key(2);
      put_varint(out, payload.size());
      out.bytes(payload);
    };
    if (value.is_null()) return {};
    if (type == "group") {
      key(3);
      CVT_TRY(message(value, out));
      key(4);
      return {};
    }
    if (type == "f32" || type == "float") {
      key(5);
      if (type == "float" || value.is_float()) {
        out.f32le(static_cast<float>(value.as_double()));
      } else {
        out.u32le(static_cast<std::uint32_t>(value.as_int64()));
      }
      return {};
    }
    if (type == "f64" || type == "double") {
      key(1);
      if (type == "double" || value.is_float()) {
        out.f64le(value.as_double());
      } else {
        out.u64le(value.is_unsigned() ? value.as_uint64() : static_cast<std::uint64_t>(value.as_int64()));
      }
      return {};
    }
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext: {
        const auto data = tagged_bytes(value);
        if (!data) return fail(Status::Encode, "invalid Base64 in a binary value");
        bytes(*data);
        return {};
      }
      case TagKind::Float:
        key(1);
        out.f64le(plain_value(value).as_double());
        return {};
      default: {
        const Json plain = plain_value(value);
        if (plain.is_string()) {
          bytes(as_bytes(plain.as_string()));
          return {};
        }
        return field_value(field, type, plain, out);
      }
    }
    switch (value.type()) {
      case Json::Type::Bool:
        key(0);
        put_varint(out, value.as_bool() ? 1 : 0);
        return {};
      case Json::Type::Int64:
        key(0);
        put_varint(out, type == "sint" ? zigzag(value.as_int64()) : static_cast<std::uint64_t>(value.as_int64()));
        return {};
      case Json::Type::UInt64:
        key(0);
        put_varint(out, value.as_uint64());
        return {};
      case Json::Type::Double:
        key(1);
        out.f64le(value.as_double());
        return {};
      case Json::Type::String: bytes(as_bytes(value.as_string())); return {};
      case Json::Type::Object: {
        util::ByteWriter nested;
        CVT_TRY(message(value, nested));
        bytes(nested.view());
        return {};
      }
      case Json::Type::Array: {
        // Arrays inside arrays: encode as a nested message of field 1 items.
        util::ByteWriter nested;
        for (const Json& element : value.elements()) CVT_TRY(field_value(1, {}, element, nested));
        bytes(nested.view());
        return {};
      }
      case Json::Type::Null: return {};
    }
    return {};
  }

  util::ByteWriter out_;
};

Result<Bytes> write_protobuf(const Json& value, WriteContext&) {
  ProtoEncoder encoder;
  return encoder.run(value);
}

}  // namespace

void register_protobuf(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "protobuf",
      .name = "Protocol Buffers message",
      .description = "A serialised Protocol Buffers message (binary wire format), decoded without a schema.",
      .category = Category::Data,
      .extensions = {"pb", "protobuf", "binpb"},
      .mime = "application/x-protobuf",
      .popularity = 20,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"pb", "protobuf", "binpb"}) ? 75 : 0; },
  });
  std::vector<OptionSpec> read_options = common_read_options(false, false);
  read_options.push_back(OptionSpec::choice("fixed_as", "Fixed-width fields", "auto", {"auto", "int", "float"},
                                            "How 32- and 64-bit fields are shown: guessed, always as unsigned "
                                            "integers, or always as floating-point numbers."));
  read_options.push_back(OptionSpec::boolean("typed_keys", "Keep wire types", false,
                                             "Add the wire type to each key (\"3:f32\") so that encoding again "
                                             "reproduces it exactly."));
  add_codec(registry, {.format_id = "protobuf",
                       .read = binary_reader(parse_protobuf),
                       .write = binary_writer(write_protobuf, "Protocol Buffers"),
                       .read_options = std::move(read_options),
                       .lossy = true,
                       .read_cost = 30,
                       .write_cost = 30});
}

}  // namespace cvt::data
