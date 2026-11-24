// Convertible core - data family: UBJSON (Draft 12) and BJData (Draft 3).
//
// Both share one grammar of one-byte type markers; UBJSON is big endian,
// BJData little endian with extra unsigned types (u, m, M), half floats (h),
// bytes (B) and N-dimensional arrays ([$type#[dims]) which are read as
// nested arrays. Optimised containers ($ type, # count) are supported;
// high-precision numbers (H) become exact numbers or $bigint/$decimal.
// Following py-ubjson and other libraries, an optimised array of uint8 (or of
// BJData bytes) is binary data, and binary data is written that way.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/bytes.hpp"

namespace cvt::data {

namespace {

class UbjsonReader {
 public:
  UbjsonReader(ByteView data, bool little_endian, const ReadContext& ctx)
      : reader_(data), little_(little_endian), ctx_(ctx), policy_(read_policy(ctx.options)), budget_(data.size()),
        binary_arrays_(ctx.options.get_bool("uint8_arrays_as_binary", true)) {}

  Result<std::vector<Json>> read_all() {
    if (reader_.at_end()) return fail(Status::Decode, "the {} data is empty", name());
    std::vector<Json> documents;
    while (true) {
      skip_noops();
      if (reader_.at_end()) break;
      CVT_TRY_ASSIGN(Json value, value_of(next_marker(), 0));
      documents.push_back(std::move(value));
    }
    if (repaired_ > 0) ctx_.warn(std::format("{} string(s) were not valid UTF-8 and were repaired.", repaired_));
    return documents;
  }

 private:
  [[nodiscard]] std::string_view name() const { return little_ ? "BJData" : "UBJSON"; }
  std::unexpected<Error> truncated() const {
    return fail(Status::Decode, "the {} data is truncated at byte {}", name(), reader_.position());
  }
  std::unexpected<Error> bad(std::string_view what) const {
    return fail(Status::Decode, "invalid {} at byte {}: {}", name(), reader_.position(), what);
  }

  void skip_noops() {
    while (!reader_.at_end() && reader_.peek_u8() == 'N') (void)reader_.u8();
  }
  char next_marker() { return static_cast<char>(reader_.u8()); }

  template <class T>
  Result<T> fixed() {
    if (sizeof(T) > reader_.remaining()) return truncated();
    return reader_.read<T>(little_ ? std::endian::little : std::endian::big);
  }

  /// An integer value of the given marker (lengths and counts).
  Result<std::int64_t> integer_of(char marker) {
    switch (marker) {
      case 'i': {
        CVT_TRY_ASSIGN(const auto v, fixed<std::int8_t>());
        return v;
      }
      case 'U': {
        CVT_TRY_ASSIGN(const auto v, fixed<std::uint8_t>());
        return v;
      }
      case 'I': {
        CVT_TRY_ASSIGN(const auto v, fixed<std::int16_t>());
        return v;
      }
      case 'l': {
        CVT_TRY_ASSIGN(const auto v, fixed<std::int32_t>());
        return v;
      }
      case 'L': {
        CVT_TRY_ASSIGN(const auto v, fixed<std::int64_t>());
        return v;
      }
      default: break;
    }
    if (little_) {
      switch (marker) {
        case 'u': {
          CVT_TRY_ASSIGN(const auto v, fixed<std::uint16_t>());
          return v;
        }
        case 'm': {
          CVT_TRY_ASSIGN(const auto v, fixed<std::uint32_t>());
          return v;
        }
        case 'M': {
          CVT_TRY_ASSIGN(const auto v, fixed<std::uint64_t>());
          if (v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return bad("length too large");
          return static_cast<std::int64_t>(v);
        }
        default: break;
      }
    }
    return bad(std::format("expected an integer type for a length, found '{}'", marker));
  }

  Result<std::uint64_t> length() {
    if (reader_.at_end()) return truncated();
    CVT_TRY_ASSIGN(const std::int64_t v, integer_of(next_marker()));
    if (v < 0) return bad("negative length");
    return static_cast<std::uint64_t>(v);
  }

  Result<std::string> string_body() {
    CVT_TRY_ASSIGN(const std::uint64_t size, length());
    if (size > reader_.remaining()) return truncated();
    return std::string(reader_.chars(static_cast<std::size_t>(size)));
  }

  [[nodiscard]] static std::size_t fixed_size(char marker, bool little) {
    switch (marker) {
      case 'Z':
      case 'N':
      case 'T':
      case 'F': return 0;
      case 'i':
      case 'U':
      case 'C':
      case 'B': return 1;
      case 'I': return 2;
      case 'l':
      case 'd': return 4;
      case 'L':
      case 'D': return 8;
      case 'u':
      case 'h': return little ? 2 : 0;
      case 'm': return little ? 4 : 0;
      case 'M': return little ? 8 : 0;
      default: return 1;  // variable-size elements take at least a byte
    }
  }

  Result<Json> value_of(char marker, std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the {} data is nested more than {} levels deep (option max_depth)", name(),
                  policy_.max_depth);
    }
    CVT_TRY(budget_.take());
    switch (marker) {
      case 'Z': return Json();
      case 'T': return Json(true);
      case 'F': return Json(false);
      case 'i':
      case 'U':
      case 'I':
      case 'l':
      case 'L': {
        CVT_TRY_ASSIGN(const std::int64_t v, integer_of(marker));
        return Json(v);
      }
      case 'u':
      case 'm':
      case 'M':
        if (!little_) break;
        if (marker == 'M') {
          CVT_TRY_ASSIGN(const auto v, fixed<std::uint64_t>());
          if (v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return Json(static_cast<std::int64_t>(v));
          return Json(v);
        } else {
          CVT_TRY_ASSIGN(const std::int64_t v, integer_of(marker));
          return Json(v);
        }
      case 'h': {
        if (!little_) break;
        CVT_TRY_ASSIGN(const auto bits, fixed<std::uint16_t>());
        return Json(half_to_double(bits));
      }
      case 'd': {
        CVT_TRY_ASSIGN(const auto bits, fixed<std::uint32_t>());
        return Json(static_cast<double>(std::bit_cast<float>(bits)));
      }
      case 'D': {
        CVT_TRY_ASSIGN(const auto bits, fixed<std::uint64_t>());
        return Json(std::bit_cast<double>(bits));
      }
      case 'C': {
        CVT_TRY_ASSIGN(const auto c, fixed<std::uint8_t>());
        return text_value(std::string(1, static_cast<char>(c)), repaired_);
      }
      case 'B': {
        CVT_TRY_ASSIGN(const auto b, fixed<std::uint8_t>());
        return Json(static_cast<std::int64_t>(b));
      }
      case 'S': {
        CVT_TRY_ASSIGN(const std::string text, string_body());
        return text_value(text, repaired_);
      }
      case 'H': {
        CVT_TRY_ASSIGN(const std::string text, string_body());
        if (!decimal_parts(text)) return bad("invalid high-precision number");
        return number_from_literal(text);
      }
      case '[':
      case '{': return container(marker == '{', depth);
      default: break;
    }
    return bad(std::format("unknown type marker '{}'", marker == '\0' ? '?' : marker));
  }

  Result<Json> container(bool object, std::size_t depth) {
    char type = 0;
    std::int64_t count = -1;
    std::vector<std::uint64_t> dims;
    if (!reader_.at_end() && reader_.peek_u8() == '$') {
      (void)reader_.u8();
      if (reader_.at_end()) return truncated();
      type = next_marker();
      if (reader_.at_end() || reader_.peek_u8() != '#') return bad("a typed container needs a count");
    }
    if (!reader_.at_end() && reader_.peek_u8() == '#') {
      (void)reader_.u8();
      if (!object && little_ && !reader_.at_end() && reader_.peek_u8() == '[') {
        // BJData N-dimensional array: the count is an array of dimensions.
        (void)reader_.u8();
        const bool binary = binary_arrays_;
        binary_arrays_ = false;  // the shape is a list of numbers
        auto shape_result = container(false, depth + 1);
        binary_arrays_ = binary;
        CVT_TRY_ASSIGN(Json shape, std::move(shape_result));
        std::uint64_t total = 1;
        for (const Json& dim : shape.elements()) {
          const auto n = dim.get_uint64();
          if (!n) return bad("invalid array dimension");
          dims.push_back(*n);
          if (*n != 0 && total > std::numeric_limits<std::uint64_t>::max() / *n) return bad("array too large");
          total *= *n;
        }
        if (dims.empty()) return bad("empty array shape");
        count = static_cast<std::int64_t>(std::min<std::uint64_t>(total, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
      } else {
        CVT_TRY_ASSIGN(const std::uint64_t n, length());
        count = static_cast<std::int64_t>(std::min<std::uint64_t>(n, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
      }
      const std::size_t element = type != 0 ? fixed_size(type, little_) : 1;
      const auto n = static_cast<std::uint64_t>(count);
      if (element > 0 && n > reader_.remaining() / element) return truncated();
      CVT_TRY(budget_.take(n));
    }
    // Binary: an optimised array of uint8 / bytes.
    if (!object && binary_arrays_ && (type == 'U' || type == 'B') && count >= 0 && dims.size() <= 1) {
      return make_binary(reader_.bytes(static_cast<std::size_t>(count)));
    }
    Json out = object ? Json::object() : Json::array();
    for (std::int64_t i = 0; count < 0 || i < count; ++i) {
      if (count < 0) {
        skip_noops();
        if (reader_.at_end()) return truncated();
        if (reader_.peek_u8() == (object ? '}' : ']')) {
          (void)reader_.u8();
          break;
        }
      }
      std::string key;
      if (object) {
        CVT_TRY_ASSIGN(const std::string raw, string_body());
        key = text_value(raw, repaired_).as_string();
      }
      char marker = type;
      if (marker == 0) {
        if (count >= 0) skip_noops();
        if (reader_.at_end()) return truncated();
        marker = next_marker();
      }
      CVT_TRY_ASSIGN(Json value, value_of(marker, depth + 1));
      if (object) {
        CVT_TRY(put_member(out, std::move(key), std::move(value), policy_));
      } else {
        out.push_back(std::move(value));
      }
    }
    if (dims.size() > 1) return reshape(std::move(out), dims);
    return out;
  }

  /// Row-major flat array -> nested arrays of the given shape.
  static Json reshape(Json flat, const std::vector<std::uint64_t>& dims) {
    std::vector<Json> level;
    level.reserve(flat.size());
    for (Json& element : flat.elements()) level.push_back(std::move(element));
    for (std::size_t d = dims.size(); d-- > 1;) {
      const auto width = static_cast<std::size_t>(dims[d]);
      std::vector<Json> next;
      for (std::size_t i = 0; width > 0 && i + width <= level.size(); i += width) {
        Json row = Json::array();
        for (std::size_t k = 0; k < width; ++k) row.push_back(std::move(level[i + k]));
        next.push_back(std::move(row));
      }
      level = std::move(next);
    }
    Json out = Json::array();
    for (Json& element : level) out.push_back(std::move(element));
    return out;
  }

  util::ByteReader reader_;
  bool little_ = false;
  const ReadContext& ctx_;
  ReadPolicy policy_;
  NodeBudget budget_;
  bool binary_arrays_ = true;
  std::size_t repaired_ = 0;
};

Result<Json> parse_ubjson_common(ByteView input, ReadContext& ctx, bool little) {
  UbjsonReader reader(input, little, ctx);
  CVT_TRY_ASSIGN(std::vector<Json> documents, reader.read_all());
  return select_documents(std::move(documents), ctx.options, little ? "BJData" : "UBJSON");
}

Result<Json> parse_ubjson(ByteView input, ReadContext& ctx) { return parse_ubjson_common(input, ctx, false); }
Result<Json> parse_bjdata(ByteView input, ReadContext& ctx) { return parse_ubjson_common(input, ctx, true); }

// =========================================================================== writer

class UbjsonWriter {
 public:
  explicit UbjsonWriter(bool little) : little_(little) {}

  Result<Bytes> run(const Json& value) {
    CVT_TRY(write(value));
    return out_.take();
  }

 private:
  template <class T>
  void put(char marker, T value) {
    out_.u8(static_cast<std::uint8_t>(marker));
    out_.write(value, little_ ? std::endian::little : std::endian::big);
  }

  void integer(std::int64_t v) {
    if (v >= -128 && v <= 127) {
      put('i', static_cast<std::int8_t>(v));
    } else if (v >= 0 && v <= 255) {
      put('U', static_cast<std::uint8_t>(v));
    } else if (v >= -32768 && v <= 32767) {
      put('I', static_cast<std::int16_t>(v));
    } else if (little_ && v >= 0 && v <= 65535) {
      put('u', static_cast<std::uint16_t>(v));
    } else if (v >= std::numeric_limits<std::int32_t>::min() && v <= std::numeric_limits<std::int32_t>::max()) {
      put('l', static_cast<std::int32_t>(v));
    } else if (little_ && v >= 0 && v <= 0xFFFFFFFFll) {
      put('m', static_cast<std::uint32_t>(v));
    } else {
      put('L', v);
    }
  }

  void high_precision(std::string_view text) {
    out_.u8('H');
    integer(static_cast<std::int64_t>(text.size()));
    out_.text(text);
  }

  void string(std::string_view text, bool with_marker) {
    const std::string valid = util::is_valid_utf8(text) ? std::string(text) : repair_utf8(text);
    if (with_marker) out_.u8('S');
    integer(static_cast<std::int64_t>(valid.size()));
    out_.text(valid);
  }

  Result<void> write(const Json& value) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext: {
        const auto bytes = tagged_bytes(value);
        if (!bytes) return fail(Status::Encode, "invalid Base64 in a binary value");
        out_.u8('[').u8('$').u8('U').u8('#');
        integer(static_cast<std::int64_t>(bytes->size()));
        out_.bytes(*bytes);
        return {};
      }
      case TagKind::BigInt:
      case TagKind::Decimal: high_precision(tag_text(value)); return {};
      case TagKind::Float: break;
      case TagKind::Date: string(tag_text(value), true); return {};
      case TagKind::Undefined: out_.u8('Z'); return {};
      default: break;
    }
    if (tag_kind(value) == TagKind::Float) return write(plain_value(value));
    switch (value.type()) {
      case Json::Type::Null: out_.u8('Z'); return {};
      case Json::Type::Bool: out_.u8(value.as_bool() ? 'T' : 'F'); return {};
      case Json::Type::Int64: integer(value.as_int64()); return {};
      case Json::Type::UInt64:
        if (little_) {
          put('M', value.as_uint64());
        } else {
          high_precision(format_integer(value));
        }
        return {};
      case Json::Type::Double: {
        const double d = value.as_double();
        if (exact_float(d)) {
          put('d', std::bit_cast<std::uint32_t>(static_cast<float>(d)));
        } else {
          put('D', std::bit_cast<std::uint64_t>(d));
        }
        return {};
      }
      case Json::Type::String: string(value.as_string(), true); return {};
      case Json::Type::Array:
        out_.u8('[');
        for (const Json& element : value.elements()) CVT_TRY(write(element));
        out_.u8(']');
        return {};
      case Json::Type::Object:
        out_.u8('{');
        for (const auto& member : value.items()) {
          string(member.key(), false);
          CVT_TRY(write(member.value()));
        }
        out_.u8('}');
        return {};
    }
    return {};
  }

  bool little_ = false;
  util::ByteWriter out_;
};

Result<Bytes> write_ubjson(const Json& value, WriteContext&) { return UbjsonWriter(false).run(value); }
Result<Bytes> write_bjdata(const Json& value, WriteContext&) { return UbjsonWriter(true).run(value); }

std::vector<OptionSpec> read_options() {
  std::vector<OptionSpec> specs = common_read_options(true, false);
  specs.push_back(documents_option());
  specs.push_back(document_index_option());
  specs.push_back(OptionSpec::boolean("uint8_arrays_as_binary", "Byte arrays are binary", true,
                                      "Read optimised arrays of uint8 as binary data (the usual library convention).")
                      .advanced());
  return specs;
}

}  // namespace

void register_ubjson(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "ubjson",
      .name = "Universal Binary JSON",
      .description = "UBJSON, a binary form of JSON with typed containers.",
      .category = Category::Data,
      .extensions = {"ubj", "ubjson"},
      .mime = "application/ubjson",
      .popularity = 12,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"ubj", "ubjson"}) ? 75 : 0; },
  });
  add_codec(registry, {.format_id = "ubjson",
                       .read = binary_reader(parse_ubjson),
                       .write = binary_writer(write_ubjson, "UBJSON"),
                       .read_options = read_options()});
  registry.add_format(FormatInfo{
      .id = "bjdata",
      .name = "Binary JData",
      .description = "BJData, a little-endian extension of UBJSON for scientific data.",
      .category = Category::Data,
      .extensions = {"bjd", "bjdata", "jdb"},
      .mime = "application/jdata-binary",
      .popularity = 8,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"bjd", "bjdata", "jdb"}) ? 75 : 0; },
  });
  add_codec(registry, {.format_id = "bjdata",
                       .read = binary_reader(parse_bjdata),
                       .write = binary_writer(write_bjdata, "BJData"),
                       .read_options = read_options()});
}

}  // namespace cvt::data
