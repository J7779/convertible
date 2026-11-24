// Convertible core - data family: FlexBuffers (schema-less FlatBuffers), read only.
//
// Every value type: null, signed/unsigned integers and floats (inline and
// indirect), bool, key, string, blob -> $binary, maps (keys in their stored
// order, which FlexBuffers keeps sorted), untyped vectors, typed vectors
// (int, uint, float, key, string, bool) and fixed-size vectors of 2, 3 or 4.
// Offsets point backwards and are checked; shared sub-values are limited by a
// node budget and self-references by the nesting limit.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <limits>
#include <string>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/bytes.hpp"

namespace cvt::data {

namespace {

enum FlexType : std::uint8_t {
  Null = 0, Int = 1, UInt = 2, Float = 3, Key = 4, String = 5, IndirectInt = 6, IndirectUInt = 7, IndirectFloat = 8,
  Map = 9, Vector = 10, VectorInt = 11, VectorUInt = 12, VectorFloat = 13, VectorKey = 14, VectorStringDeprecated = 15,
  VectorInt2 = 16, VectorUInt2 = 17, VectorFloat2 = 18, VectorInt3 = 19, VectorUInt3 = 20, VectorFloat3 = 21,
  VectorInt4 = 22, VectorUInt4 = 23, VectorFloat4 = 24, Blob = 25, Bool = 26, VectorBool = 36
};

class FlexReader {
 public:
  FlexReader(ByteView data, const ReadContext& ctx)
      : data_(data), policy_(read_policy(ctx.options)), budget_(data.size()) {}

  Result<Json> parse() {
    if (data_.size() < 3) return fail(Status::Decode, "the FlexBuffer is too short");
    const std::size_t root_width = byte_at(data_.size() - 1);
    const std::uint8_t packed = byte_at(data_.size() - 2);
    if (!valid_width(root_width) || data_.size() < 2 + root_width) {
      return fail(Status::Decode, "the FlexBuffer root has an invalid width");
    }
    return value(data_.size() - 2 - root_width, root_width, packed, 0);
  }

 private:
  static bool valid_width(std::size_t w) { return w == 1 || w == 2 || w == 4 || w == 8; }
  [[nodiscard]] std::uint8_t byte_at(std::size_t at) const { return std::to_integer<std::uint8_t>(data_[at]); }

  std::unexpected<Error> bad(std::size_t at, std::string_view what) const {
    return fail(Status::Decode, "invalid FlexBuffer at byte {}: {}", at, what);
  }

  Result<std::uint64_t> read_uint(std::size_t at, std::size_t width) const {
    if (!valid_width(width) || at > data_.size() || width > data_.size() - at) return bad(at, "read out of range");
    std::uint64_t value = 0;
    for (std::size_t i = width; i-- > 0;) value = (value << 8) | byte_at(at + i);
    return value;
  }

  Result<std::int64_t> read_int(std::size_t at, std::size_t width) const {
    CVT_TRY_ASSIGN(std::uint64_t value, read_uint(at, width));
    if (width < 8 && (value >> (width * 8 - 1)) != 0) value |= ~std::uint64_t{0} << (width * 8);
    return static_cast<std::int64_t>(value);
  }

  Result<double> read_float(std::size_t at, std::size_t width) const {
    CVT_TRY_ASSIGN(const std::uint64_t bits, read_uint(at, width));
    if (width == 4) return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits)));
    if (width == 8) return std::bit_cast<double>(bits);
    return bad(at, "float of unsupported width");
  }

  /// The position an offset stored at `at` (width bytes) points to.
  Result<std::size_t> indirect(std::size_t at, std::size_t width) const {
    CVT_TRY_ASSIGN(const std::uint64_t offset, read_uint(at, width));
    if (offset > at) return bad(at, "offset points before the buffer");
    return at - static_cast<std::size_t>(offset);
  }

  Result<std::string> key_at(std::size_t at) const {
    if (at >= data_.size()) return bad(at, "key out of range");
    const std::string_view rest = as_chars(data_.subspan(at));
    const std::size_t end = rest.find('\0');
    if (end == std::string_view::npos) return bad(at, "unterminated key");
    std::size_t repaired = 0;
    return text_value(rest.substr(0, end), repaired).as_string();
  }

  Result<Json> scalar(FlexType type, std::size_t at, std::size_t width) const {
    switch (type) {
      case Int: {
        CVT_TRY_ASSIGN(const std::int64_t v, read_int(at, width));
        return Json(v);
      }
      case UInt: {
        CVT_TRY_ASSIGN(const std::uint64_t v, read_uint(at, width));
        if (v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return Json(static_cast<std::int64_t>(v));
        return Json(v);
      }
      case Float: {
        CVT_TRY_ASSIGN(const double v, read_float(at, width));
        return Json(v);
      }
      case Bool: {
        CVT_TRY_ASSIGN(const std::uint64_t v, read_uint(at, width));
        return Json(v != 0);
      }
      default: break;
    }
    return bad(at, "unexpected scalar type");
  }

  Result<Json> value(std::size_t at, std::size_t parent_width, std::uint8_t packed, std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the FlexBuffer is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    CVT_TRY(budget_.take());
    const auto type = static_cast<FlexType>(packed >> 2);
    const std::size_t child_width = std::size_t{1} << (packed & 3);
    switch (type) {
      case Null: return Json();
      case Int:
      case UInt:
      case Float:
      case Bool: return scalar(type, at, parent_width);
      default: break;
    }
    CVT_TRY_ASSIGN(const std::size_t target, indirect(at, parent_width));
    switch (type) {
      case IndirectInt: return scalar(Int, target, child_width);
      case IndirectUInt: return scalar(UInt, target, child_width);
      case IndirectFloat: return scalar(Float, target, child_width);
      case Key: {
        CVT_TRY_ASSIGN(std::string key, key_at(target));
        return Json(std::move(key));
      }
      case String:
      case Blob: {
        if (target < child_width) return bad(target, "missing length");
        CVT_TRY_ASSIGN(const std::uint64_t size, read_uint(target - child_width, child_width));
        if (size > data_.size() - target) return bad(target, "string runs past the end");
        const ByteView bytes = data_.subspan(target, static_cast<std::size_t>(size));
        if (type == Blob) return make_binary(bytes);
        std::size_t repaired = 0;
        return text_value(as_chars(bytes), repaired);
      }
      case Map:
      case Vector:
      case VectorInt:
      case VectorUInt:
      case VectorFloat:
      case VectorKey:
      case VectorStringDeprecated:
      case VectorBool: return vector(type, target, child_width, depth);
      case VectorInt2:
      case VectorUInt2:
      case VectorFloat2:
      case VectorInt3:
      case VectorUInt3:
      case VectorFloat3:
      case VectorInt4:
      case VectorUInt4:
      case VectorFloat4: {
        const int index = type - VectorInt2;
        const std::size_t count = static_cast<std::size_t>(index / 3) + 2;
        const auto element = static_cast<FlexType>(Int + index % 3);
        Json array = Json::array();
        for (std::size_t i = 0; i < count; ++i) {
          CVT_TRY_ASSIGN(Json v, scalar(element, target + i * child_width, child_width));
          array.push_back(std::move(v));
        }
        return array;
      }
      default: break;
    }
    return bad(at, std::format("unknown type {}", static_cast<int>(type)));
  }

  Result<Json> vector(FlexType type, std::size_t at, std::size_t width, std::size_t depth) {
    if (at < width) return bad(at, "missing vector length");
    CVT_TRY_ASSIGN(const std::uint64_t count, read_uint(at - width, width));
    if (count > (data_.size() - at) / width) return bad(at, "vector runs past the end");
    CVT_TRY(budget_.take(count));
    const auto n = static_cast<std::size_t>(count);
    const bool untyped = type == Vector || type == Map;
    if (untyped && n > data_.size() - at - n * width) return bad(at, "vector types run past the end");
    Json out = type == Map ? Json::object() : Json::array();
    std::size_t keys = 0;
    std::size_t key_width = 0;
    if (type == Map) {
      if (at < 3 * width) return bad(at, "missing map keys");
      CVT_TRY_ASSIGN(keys, indirect(at - 3 * width, width));
      CVT_TRY_ASSIGN(const std::uint64_t kw, read_uint(at - 2 * width, width));
      key_width = static_cast<std::size_t>(kw);
      if (!valid_width(key_width) || keys < key_width) return bad(at, "invalid map keys");
      CVT_TRY_ASSIGN(const std::uint64_t key_count, read_uint(keys - key_width, key_width));
      if (key_count != count || count > (data_.size() - keys) / key_width) return bad(at, "map keys do not match its values");
    }
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t element = at + i * width;
      Json item;
      switch (type) {
        case Map:
        case Vector: {
          const std::uint8_t packed = byte_at(at + n * width + i);
          CVT_TRY_ASSIGN(item, value(element, width, packed, depth + 1));
          break;
        }
        case VectorInt: {
          CVT_TRY_ASSIGN(item, scalar(Int, element, width));
          break;
        }
        case VectorUInt: {
          CVT_TRY_ASSIGN(item, scalar(UInt, element, width));
          break;
        }
        case VectorFloat: {
          CVT_TRY_ASSIGN(item, scalar(Float, element, width));
          break;
        }
        case VectorBool: {
          CVT_TRY_ASSIGN(item, scalar(Bool, element, width));
          break;
        }
        case VectorKey: {
          CVT_TRY_ASSIGN(const std::size_t target, indirect(element, width));
          CVT_TRY_ASSIGN(std::string key, key_at(target));
          item = Json(std::move(key));
          break;
        }
        case VectorStringDeprecated: {
          CVT_TRY_ASSIGN(item, value(element, width, static_cast<std::uint8_t>((String << 2) | (packed_width(width))), depth + 1));
          break;
        }
        default: break;
      }
      if (type == Map) {
        CVT_TRY_ASSIGN(const std::size_t key_target, indirect(keys + i * key_width, key_width));
        CVT_TRY_ASSIGN(std::string key, key_at(key_target));
        CVT_TRY(put_member(out, std::move(key), std::move(item), policy_));
      } else {
        out.push_back(std::move(item));
      }
    }
    return out;
  }

  static std::uint8_t packed_width(std::size_t width) {
    return width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3;
  }

  ByteView data_;
  ReadPolicy policy_;
  NodeBudget budget_;
};

Result<Json> parse_flexbuffers(ByteView input, ReadContext& ctx) {
  FlexReader reader(input, ctx);
  return reader.parse();
}

}  // namespace

void register_flexbuffers(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "flexbuffers",
      .name = "FlexBuffers",
      .description = "The schema-less binary format of the FlatBuffers project.",
      .category = Category::Data,
      .extensions = {"flexbuf", "flexbuffers", "flx"},
      .mime = "application/octet-stream",
      .popularity = 5,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) {
        return has_extension(filename, {"flexbuf", "flexbuffers", "flx"}) ? 75 : 0;
      },
  });
  add_codec(registry, {.format_id = "flexbuffers",
                       .read = binary_reader(parse_flexbuffers),
                       .write = nullptr,
                       .read_options = common_read_options(true, false)});
}

}  // namespace cvt::data
