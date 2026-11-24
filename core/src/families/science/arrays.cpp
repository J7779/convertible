// Convertible core - science family: n-dimensional array model.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "families/science/arrays.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

#include "util/text.hpp"

namespace cvt::science::arrays {

namespace {

constexpr std::size_t max_rank = 32;

std::uint64_t load_le(const std::byte* p, std::uint32_t size) noexcept {
  std::uint64_t v = 0;
  for (std::uint32_t i = 0; i < size && i < 8; ++i) v |= std::uint64_t{static_cast<std::uint8_t>(p[i])} << (8 * i);
  return v;
}

void store_le(std::byte* p, std::uint32_t size, std::uint64_t v) noexcept {
  for (std::uint32_t i = 0; i < size && i < 8; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
}

std::int64_t sign_extend(std::uint64_t v, std::uint32_t size) noexcept {
  if (size >= 8) return static_cast<std::int64_t>(v);
  const std::uint64_t sign = std::uint64_t{1} << (size * 8 - 1);
  return static_cast<std::int64_t>((v ^ sign) - sign);
}

double float_value(const std::byte* p, std::uint32_t size) noexcept {
  if (size == 2) return half_to_double(static_cast<std::uint16_t>(load_le(p, 2)));
  if (size == 4) return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(load_le(p, 4))));
  if (size == 8) return std::bit_cast<double>(load_le(p, 8));
  return std::numeric_limits<double>::quiet_NaN();
}

std::string text_value(DType type, const std::byte* p) {
  std::string out;
  if (type.kind == ScalarKind::Bytes) {
    for (std::uint32_t i = 0; i < type.size; ++i) {
      const char c = static_cast<char>(p[i]);
      if (c == '\0') break;
      out.push_back(c);
    }
    return util::sanitize_utf8(out);
  }
  for (std::uint32_t i = 0; i + 4 <= type.size; i += 4) {
    const auto cp = static_cast<char32_t>(load_le(p + i, 4));
    if (cp == 0) break;
    util::append_utf8(out, util::is_scalar_value(cp) ? cp : U'�');
  }
  return out;
}

}  // namespace

double half_to_double(std::uint16_t h) noexcept {
  const int sign = (h >> 15) & 1;
  const int exponent = (h >> 10) & 0x1F;
  const int mantissa = h & 0x3FF;
  double value;
  if (exponent == 0) {
    value = std::ldexp(static_cast<double>(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa == 0 ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
  } else {
    value = std::ldexp(static_cast<double>(mantissa + 1024), exponent - 25);
  }
  return sign ? -value : value;
}

std::uint16_t double_to_half(double v) noexcept {
  const std::uint32_t f = std::bit_cast<std::uint32_t>(static_cast<float>(v));
  const std::uint32_t sign = (f >> 16) & 0x8000;
  const int exponent = static_cast<int>((f >> 23) & 0xFF) - 127 + 15;
  std::uint32_t mantissa = f & 0x7FFFFF;
  if (((f >> 23) & 0xFF) == 0xFF) return static_cast<std::uint16_t>(sign | 0x7C00 | (mantissa ? 0x200 : 0));
  if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00);
  if (exponent <= 0) {
    if (exponent < -10) return static_cast<std::uint16_t>(sign);
    mantissa |= 0x800000;
    const int shift = 14 - exponent;
    std::uint32_t half = mantissa >> shift;
    const std::uint32_t rest = mantissa & ((1u << shift) - 1);
    const std::uint32_t halfway = 1u << (shift - 1);
    if (rest > halfway || (rest == halfway && (half & 1))) ++half;
    return static_cast<std::uint16_t>(sign | half);
  }
  std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
  const std::uint32_t rest = mantissa & 0x1FFF;
  if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) ++half;
  return static_cast<std::uint16_t>(half);
}

std::uint64_t Array::count() const noexcept {
  std::uint64_t n = 1;
  for (const auto d : shape) n *= d;
  return n;
}

double Array::number(std::uint64_t index) const noexcept { return scalar_double(dtype, element(index)); }

std::string dtype_name(DType type) {
  switch (type.kind) {
    case ScalarKind::Bool: return "bool";
    case ScalarKind::Int: return "int" + std::to_string(type.size * 8);
    case ScalarKind::UInt: return "uint" + std::to_string(type.size * 8);
    case ScalarKind::Float: return "float" + std::to_string(type.size * 8);
    case ScalarKind::Complex: return "complex" + std::to_string(type.size * 8);
    case ScalarKind::Bytes: return "S" + std::to_string(type.size);
    case ScalarKind::Unicode: return "U" + std::to_string(type.size / 4);
    case ScalarKind::Record: return "record";
  }
  return "record";
}

std::optional<DType> parse_dtype_name(std::string_view name) {
  const auto bits = [&](std::string_view prefix) -> std::optional<std::uint32_t> {
    const auto n = util::parse_int(name.substr(prefix.size()));
    if (!n || *n <= 0 || *n % 8 != 0 || *n > 128) return std::nullopt;
    return static_cast<std::uint32_t>(*n / 8);
  };
  if (name == "bool") return DType{ScalarKind::Bool, 1};
  if (name.starts_with("uint")) {
    if (auto b = bits("uint"); b && (*b == 1 || *b == 2 || *b == 4 || *b == 8)) return DType{ScalarKind::UInt, *b};
  } else if (name.starts_with("int")) {
    if (auto b = bits("int"); b && (*b == 1 || *b == 2 || *b == 4 || *b == 8)) return DType{ScalarKind::Int, *b};
  } else if (name.starts_with("float")) {
    if (auto b = bits("float"); b && (*b == 2 || *b == 4 || *b == 8)) return DType{ScalarKind::Float, *b};
  } else if (name.starts_with("complex")) {
    if (auto b = bits("complex"); b && (*b == 8 || *b == 16)) return DType{ScalarKind::Complex, *b};
  } else if (name.starts_with('S') || name.starts_with('U')) {
    const auto n = util::parse_int(name.substr(1));
    if (n && *n > 0 && *n < (1 << 20)) {
      return name[0] == 'S' ? DType{ScalarKind::Bytes, static_cast<std::uint32_t>(*n)}
                            : DType{ScalarKind::Unicode, static_cast<std::uint32_t>(*n * 4)};
    }
  } else if (name == "record") {
    return DType{ScalarKind::Record, 0};
  }
  return std::nullopt;
}

std::optional<DType> parse_numpy_descr(std::string_view descr, bool& big_endian) {
  if (descr.size() < 2) return std::nullopt;
  big_endian = descr[0] == '>' || descr[0] == '!';
  if (descr[0] == '<' || descr[0] == '>' || descr[0] == '|' || descr[0] == '=' || descr[0] == '!') descr.remove_prefix(1);
  const char code = descr[0];
  const auto n = util::parse_int(descr.substr(1));
  if (!n || *n <= 0 || *n > (1 << 24)) return std::nullopt;
  const auto size = static_cast<std::uint32_t>(*n);
  switch (code) {
    case 'b': return size == 1 ? std::optional(DType{ScalarKind::Bool, 1}) : std::nullopt;
    case 'i': return size == 1 || size == 2 || size == 4 || size == 8 ? std::optional(DType{ScalarKind::Int, size}) : std::nullopt;
    case 'u': return size == 1 || size == 2 || size == 4 || size == 8 ? std::optional(DType{ScalarKind::UInt, size}) : std::nullopt;
    case 'f': return size == 2 || size == 4 || size == 8 ? std::optional(DType{ScalarKind::Float, size}) : std::nullopt;
    case 'c': return size == 8 || size == 16 ? std::optional(DType{ScalarKind::Complex, size}) : std::nullopt;
    case 'S':
    case 'a': return DType{ScalarKind::Bytes, size};
    case 'U': return DType{ScalarKind::Unicode, size * 4};
    default: return std::nullopt;
  }
}

std::string numpy_descr(DType type) {
  switch (type.kind) {
    case ScalarKind::Bool: return "|b1";
    case ScalarKind::Int: return (type.size == 1 ? "|i" : "<i") + std::to_string(type.size);
    case ScalarKind::UInt: return (type.size == 1 ? "|u" : "<u") + std::to_string(type.size);
    case ScalarKind::Float: return "<f" + std::to_string(type.size);
    case ScalarKind::Complex: return "<c" + std::to_string(type.size);
    case ScalarKind::Bytes: return "|S" + std::to_string(type.size);
    case ScalarKind::Unicode: return "<U" + std::to_string(type.size / 4);
    case ScalarKind::Record: return "|V";
  }
  return "|V";
}

double scalar_double(DType type, const std::byte* p) noexcept {
  switch (type.kind) {
    case ScalarKind::Bool: return p[0] != std::byte{0} ? 1.0 : 0.0;
    case ScalarKind::Int: return static_cast<double>(sign_extend(load_le(p, type.size), type.size));
    case ScalarKind::UInt: return static_cast<double>(load_le(p, type.size));
    case ScalarKind::Float: return float_value(p, type.size);
    case ScalarKind::Complex: return float_value(p, type.size / 2);  // real part
    default: return std::numeric_limits<double>::quiet_NaN();
  }
}

Json scalar_json(DType type, const std::byte* p) {
  switch (type.kind) {
    case ScalarKind::Bool: return Json(p[0] != std::byte{0});
    case ScalarKind::Int: return Json(sign_extend(load_le(p, type.size), type.size));
    case ScalarKind::UInt: return Json(load_le(p, type.size));
    case ScalarKind::Float: {
      const double v = float_value(p, type.size);
      if (!std::isfinite(v)) return Json(fmt_num(v));
      return Json(v);
    }
    case ScalarKind::Complex: {
      const double re = float_value(p, type.size / 2);
      const double im = float_value(p + type.size / 2, type.size / 2);
      return Json::array({std::isfinite(re) ? Json(re) : Json(fmt_num(re)), std::isfinite(im) ? Json(im) : Json(fmt_num(im))});
    }
    case ScalarKind::Bytes:
    case ScalarKind::Unicode: return Json(text_value(type, p));
    case ScalarKind::Record: return Json();
  }
  return Json();
}

void store_double(DType type, std::byte* p, double value) noexcept {
  switch (type.kind) {
    case ScalarKind::Bool: p[0] = std::byte{value != 0.0 ? std::uint8_t{1} : std::uint8_t{0}}; break;
    case ScalarKind::Int: {
      const double lo = -std::ldexp(1.0, static_cast<int>(type.size * 8 - 1));
      const double hi = std::ldexp(1.0, static_cast<int>(type.size * 8 - 1)) - 1;
      const double r = std::isnan(value) ? 0.0 : std::clamp(std::round(value), lo, hi);
      store_le(p, type.size, static_cast<std::uint64_t>(static_cast<std::int64_t>(r)));
      break;
    }
    case ScalarKind::UInt: {
      const double hi = std::ldexp(1.0, static_cast<int>(type.size * 8)) - 1;
      const double r = std::isnan(value) ? 0.0 : std::clamp(std::round(value), 0.0, hi);
      store_le(p, type.size, r >= 18446744073709549568.0 ? ~std::uint64_t{0} : static_cast<std::uint64_t>(r));
      break;
    }
    case ScalarKind::Float:
    case ScalarKind::Complex: {
      const std::uint32_t part = type.kind == ScalarKind::Complex ? type.size / 2 : type.size;
      if (part == 2) store_le(p, 2, double_to_half(value));
      if (part == 4) store_le(p, 4, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
      if (part == 8) store_le(p, 8, std::bit_cast<std::uint64_t>(value));
      if (type.kind == ScalarKind::Complex) store_le(p + part, part, 0);
      break;
    }
    default: break;
  }
}

namespace {

void swap_scalar(DType type, std::byte* p) {
  if (type.kind == ScalarKind::Bytes || type.kind == ScalarKind::Bool) return;
  if (type.kind == ScalarKind::Unicode) {
    for (std::uint32_t i = 0; i + 4 <= type.size; i += 4) std::reverse(p + i, p + i + 4);
    return;
  }
  if (type.kind == ScalarKind::Complex) {
    std::reverse(p, p + type.size / 2);
    std::reverse(p + type.size / 2, p + type.size);
    return;
  }
  std::reverse(p, p + type.size);
}

}  // namespace

void swap_elements(Array& array) {
  const std::uint64_t n = array.count();
  for (std::uint64_t i = 0; i < n; ++i) {
    std::byte* p = array.data.data() + i * array.dtype.size;
    if (array.dtype.kind == ScalarKind::Record) {
      for (const Field& f : array.fields) swap_scalar(f.type, p + f.offset);
    } else {
      swap_scalar(array.dtype, p);
    }
  }
}

Result<std::uint64_t> shape_count(const std::vector<std::uint64_t>& shape) {
  if (shape.size() > max_rank) return fail(Status::Decode, "array rank {} is too high", shape.size());
  std::uint64_t n = 1;
  for (const auto d : shape) {
    if (d != 0 && n > std::numeric_limits<std::uint64_t>::max() / d) return fail(Status::Limit, "array dimensions overflow");
    n *= d;
  }
  return n;
}

Result<void> check_array_bytes(const Context& ctx, std::uint64_t count, std::uint64_t size) {
  if (size != 0 && count > ctx.limits.max_decompressed_bytes / size) {
    return fail(Status::Limit, "array of {} elements exceeds the memory limit", count);
  }
  if (count * size > std::numeric_limits<std::size_t>::max() / 2) return fail(Status::Limit, "array is too large for this platform");
  return {};
}

namespace {

void reorder(Array& array, bool to_c) {
  const std::size_t rank = array.shape.size();
  if (rank < 2) return;
  const std::uint64_t n = array.count();
  const std::size_t size = array.dtype.size;
  Bytes out(array.data.size());
  std::vector<std::uint64_t> index(rank, 0);
  for (std::uint64_t i = 0; i < n; ++i) {
    // `index` is the C-order multi-index of element i.
    std::uint64_t f = 0;
    for (std::size_t d = rank; d-- > 0;) f = f * array.shape[d] + index[d];
    if (to_c) {
      ::cvt::util::copy_bytes(out.data() + i * size, array.data.data() + f * size, size);
    } else {
      ::cvt::util::copy_bytes(out.data() + f * size, array.data.data() + i * size, size);
    }
    for (std::size_t d = rank; d-- > 0;) {
      if (++index[d] < array.shape[d]) break;
      index[d] = 0;
    }
  }
  array.data = std::move(out);
}

}  // namespace

void fortran_to_c(Array& array) { reorder(array, true); }
void c_to_fortran(Array& array) { reorder(array, false); }

// --------------------------------------------------------------------------- JSON

namespace {

Json element_json(const Array& array, std::uint64_t i) {
  const std::byte* p = array.element(i);
  if (array.dtype.kind != ScalarKind::Record) return scalar_json(array.dtype, p);
  Json record = Json::object();
  for (const Field& f : array.fields) record[f.name] = scalar_json(f.type, p + f.offset);
  return record;
}

}  // namespace

Result<Json> array_to_json(const Array& array, const Context& ctx) {
  CVT_TRY_ASSIGN(const std::uint64_t n, shape_count(array.shape));
  if (array.data.size() < n * array.dtype.size) return fail(Status::Decode, "array data is shorter than its shape");
  CVT_TRY(check_cells(ctx, n));
  Json out = Json::object();
  if (!array.name.empty()) out["name"] = array.name;
  out["dtype"] = dtype_name(array.dtype);
  if (array.dtype.kind == ScalarKind::Record) {
    Json fields = Json::array();
    for (const Field& f : array.fields) fields.push_back(Json::object({{"name", f.name}, {"dtype", dtype_name(f.type)}}));
    out["fields"] = std::move(fields);
  }
  Json shape = Json::array();
  for (const auto d : array.shape) shape.push_back(d);
  out["shape"] = std::move(shape);
  // Build nested lists from the innermost dimension outwards.
  if (array.shape.empty()) {
    out["data"] = element_json(array, 0);
  } else {
    std::vector<Json> level;
    const std::uint64_t inner = array.shape.back();
    const std::uint64_t outer = inner == 0 ? 0 : n / inner;
    level.reserve(static_cast<std::size_t>(std::max<std::uint64_t>(outer, 1)));
    for (std::uint64_t o = 0; o < std::max<std::uint64_t>(outer, inner == 0 ? 1 : 0); ++o) {
      Json row = Json::array();
      for (std::uint64_t k = 0; k < inner; ++k) row.push_back(element_json(array, o * inner + k));
      level.push_back(std::move(row));
      if ((o & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(o) / static_cast<double>(std::max<std::uint64_t>(outer, 1))));
    }
    if (outer == 0 && inner != 0) level.clear();
    for (std::size_t d = array.shape.size() - 1; d-- > 0;) {
      const std::uint64_t dim = array.shape[d];
      std::vector<Json> next;
      const std::size_t groups = dim == 0 ? 0 : level.size() / static_cast<std::size_t>(dim);
      for (std::size_t g = 0; g < std::max<std::size_t>(groups, dim == 0 ? 1 : 0); ++g) {
        Json list = Json::array();
        for (std::uint64_t k = 0; k < dim; ++k) list.push_back(std::move(level[g * static_cast<std::size_t>(dim) + k]));
        next.push_back(std::move(list));
      }
      level = std::move(next);
    }
    out["data"] = level.empty() ? Json::array() : std::move(level.front());
  }
  if (!array.attributes.empty()) out["attributes"] = array.attributes;
  return out;
}

namespace {

/// Flatten nested lists; records the shape from the first path.
Result<void> flatten(const Json& value, std::vector<const Json*>& leaves, std::vector<std::uint64_t>& shape,
                     const Context& ctx) {
  // Shape from the first element at each level.
  const Json* probe = &value;
  while (probe->is_array()) {
    if (shape.size() >= max_rank) return fail(Status::Encode, "array nesting is too deep");
    shape.push_back(probe->size());
    if (probe->empty()) break;
    probe = &(*probe)[0];
  }
  // Complex numbers are [re, im] pairs at the innermost level: handled by the caller.
  std::vector<std::pair<const Json*, std::size_t>> stack = {{&value, 0}};
  while (!stack.empty()) {
    auto [node, depth] = stack.back();
    stack.pop_back();
    if (depth == shape.size()) {
      leaves.push_back(node);
      continue;
    }
    if (!node->is_array() || node->size() != shape[depth]) return fail(Status::Encode, "the nested lists are ragged");
    for (std::size_t i = node->size(); i-- > 0;) stack.emplace_back(&(*node)[i], depth + 1);
    if (leaves.size() > ctx.limits.max_pixels) return fail(Status::Limit, "array is too large");
  }
  return {};
}

std::string utf32_le(std::string_view text, std::uint32_t chars) {
  std::string out(static_cast<std::size_t>(chars) * 4, '\0');
  const std::u32string u = util::utf8_to_utf32(text);
  for (std::size_t i = 0; i < u.size() && i < chars; ++i) {
    for (int b = 0; b < 4; ++b) out[i * 4 + static_cast<std::size_t>(b)] = static_cast<char>((u[i] >> (8 * b)) & 0xFF);
  }
  return out;
}

Result<void> store_json(DType type, std::byte* p, const Json& v) {
  switch (type.kind) {
    case ScalarKind::Bytes: {
      const std::string s = v.is_string() ? v.as_string() : util::dump_json(v);
      ::cvt::util::copy_bytes(p, s.data(), std::min<std::size_t>(s.size(), type.size));
      return {};
    }
    case ScalarKind::Unicode: {
      const std::string s = utf32_le(v.is_string() ? v.as_string() : util::dump_json(v), type.size / 4);
      ::cvt::util::copy_bytes(p, s.data(), s.size());
      return {};
    }
    case ScalarKind::Complex: {
      const std::uint32_t part = type.size / 2;
      const DType f{ScalarKind::Float, part};
      if (v.is_array() && v.size() == 2) {
        store_double(f, p, v[0].is_number() ? v[0].as_double() : to_double(v[0].as_string()).value_or(NAN));
        store_double(f, p + part, v[1].is_number() ? v[1].as_double() : to_double(v[1].as_string()).value_or(NAN));
      } else {
        store_double(f, p, v.as_double(NAN));
        store_double(f, p + part, 0.0);
      }
      return {};
    }
    case ScalarKind::Int:
      if (type.size == 8 && v.is_integer() && !v.is_unsigned()) {
        store_le(p, 8, static_cast<std::uint64_t>(v.as_int64()));
        return {};
      }
      break;
    case ScalarKind::UInt:
      if (type.size == 8 && v.is_integer()) {
        store_le(p, 8, v.as_uint64());
        return {};
      }
      break;
    default: break;
  }
  double d = std::numeric_limits<double>::quiet_NaN();
  if (v.is_number()) d = v.as_double();
  else if (v.is_bool()) d = v.as_bool() ? 1 : 0;
  else if (v.is_string()) d = to_double(v.as_string()).value_or(d);
  store_double(type, p, d);
  return {};
}

DType infer_dtype(const std::vector<const Json*>& leaves) {
  bool all_bool = !leaves.empty();
  bool all_int = !leaves.empty();
  bool any_string = false;
  std::size_t longest = 1;
  for (const Json* v : leaves) {
    all_bool = all_bool && v->is_bool();
    all_int = all_int && v->is_integer();
    if (v->is_string()) {
      any_string = true;
      longest = std::max(longest, util::utf8_to_utf32(v->as_string()).size());
    }
  }
  if (all_bool) return {ScalarKind::Bool, 1};
  if (all_int) return {ScalarKind::Int, 8};
  if (any_string) {
    bool numeric_strings = true;
    for (const Json* v : leaves) {
      if (v->is_string() && !to_double(v->as_string())) numeric_strings = false;
    }
    if (!numeric_strings) return {ScalarKind::Unicode, static_cast<std::uint32_t>(longest * 4)};
  }
  return {ScalarKind::Float, 8};
}

}  // namespace

Result<Array> json_to_array(const Json& value, const Context& ctx) {
  Array array;
  const Json* data = &value;
  std::optional<DType> declared;
  if (value.is_object()) {
    data = value.find("data");
    if (data == nullptr) data = value.find("values");
    if (data == nullptr) return fail(Status::Encode, "the array has no \"data\"");
    array.name = value["name"].as_string();
    if (const Json* dtype = value.find("dtype")) {
      declared = parse_dtype_name(dtype->as_string());
      if (!declared) return fail(Status::Encode, "unknown dtype '{}'", dtype->as_string());
    }
    if (const Json* attrs = value.find("attributes"); attrs != nullptr && attrs->is_object()) array.attributes = *attrs;
  }
  std::vector<const Json*> leaves;
  std::vector<std::uint64_t> shape;
  CVT_TRY(flatten(*data, leaves, shape, ctx));
  // Records: leaves are objects.
  bool records = !leaves.empty() && leaves.front()->is_object();
  if (declared && declared->kind == ScalarKind::Record) records = true;
  if (records) {
    if (value.is_object() && value.contains("fields")) {
      for (const Json& f : value["fields"].elements()) {
        const auto t = parse_dtype_name(f["dtype"].as_string());
        if (!t || t->kind == ScalarKind::Record) return fail(Status::Encode, "bad field dtype '{}'", f["dtype"].as_string());
        array.fields.push_back(Field{f["name"].as_string(), *t, 0});
      }
    } else {
      std::vector<std::string> names;
      for (const Json* leaf : leaves) {
        for (const auto& m : leaf->items()) {
          if (std::find(names.begin(), names.end(), m.key()) == names.end()) names.push_back(m.key());
        }
      }
      for (const auto& name : names) {
        std::vector<const Json*> column;
        for (const Json* leaf : leaves) {
          if (const Json* v = leaf->find(name)) column.push_back(v);
        }
        array.fields.push_back(Field{name, infer_dtype(column), 0});
      }
    }
    std::uint32_t offset = 0;
    for (Field& f : array.fields) {
      f.offset = offset;
      offset += f.type.size;
    }
    array.dtype = {ScalarKind::Record, offset};
  } else {
    array.dtype = declared ? *declared : infer_dtype(leaves);
  }
  if (array.dtype.kind == ScalarKind::Complex) {
    // Re-flatten to the pair level.
    // The innermost level of 2 holds [re, im]; re-walk the lists at the pair level.
    std::vector<const Json*> pairs;
    if (shape.empty() || shape.back() != 2) return fail(Status::Encode, "complex data must be [re, im] pairs");
    shape.pop_back();
    // Collect the pair arrays by walking the same order.
    std::vector<std::pair<const Json*, std::size_t>> stack = {{data, 0}};
    while (!stack.empty()) {
      auto [node, depth] = stack.back();
      stack.pop_back();
      if (depth == shape.size()) {
        pairs.push_back(node);
        continue;
      }
      for (std::size_t i = node->size(); i-- > 0;) stack.emplace_back(&(*node)[i], depth + 1);
    }
    leaves = std::move(pairs);
  }
  if (value.is_object() && value.contains("shape")) {
    std::vector<std::uint64_t> declared_shape;
    for (const Json& d : value["shape"].elements()) declared_shape.push_back(d.as_uint64());
    CVT_TRY_ASSIGN(const std::uint64_t declared_count, shape_count(declared_shape));
    if (declared_count == leaves.size()) shape = declared_shape;
  }
  array.shape = shape;
  if (!data->is_array() && !records) array.shape.clear();
  CVT_TRY(check_array_bytes(ctx, leaves.size(), array.dtype.size));
  array.data.assign(leaves.size() * array.dtype.size, std::byte{0});
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    std::byte* p = array.data.data() + i * array.dtype.size;
    if (array.dtype.kind == ScalarKind::Record) {
      for (const Field& f : array.fields) {
        if (const Json* v = leaves[i]->find(f.name)) CVT_TRY(store_json(f.type, p + f.offset, *v));
      }
    } else {
      CVT_TRY(store_json(array.dtype, p, *leaves[i]));
    }
  }
  return array;
}

// --------------------------------------------------------------------------- tables

namespace {

ir::Cell scalar_cell(DType type, const std::byte* p) {
  switch (type.kind) {
    case ScalarKind::Bool: return ir::Cell::boolean(p[0] != std::byte{0});
    case ScalarKind::Int: {
      const std::int64_t v = sign_extend(load_le(p, type.size), type.size);
      return ir::Cell::numeric(static_cast<double>(v), std::to_string(v));
    }
    case ScalarKind::UInt: {
      const std::uint64_t v = load_le(p, type.size);
      return ir::Cell::numeric(static_cast<double>(v), std::to_string(v));
    }
    case ScalarKind::Float: {
      const double v = float_value(p, type.size);
      // Shortest text that round-trips at the element's own precision.
      std::string text = type.size == 4 ? fmt_num(v, 9) : fmt_num(v);
      if (type.size == 4) {
        const float f = static_cast<float>(v);
        for (int digits = 6; digits <= 9; ++digits) {
          const std::string candidate = fmt_num(v, digits);
          if (static_cast<float>(to_double(candidate).value_or(0)) == f) {
            text = candidate;
            break;
          }
        }
      } else if (type.size == 2) {
        text = fmt_num(v, 5);
      }
      return ir::Cell::numeric(v, text);
    }
    case ScalarKind::Complex: {
      const double re = float_value(p, type.size / 2);
      const double im = float_value(p + type.size / 2, type.size / 2);
      return ir::Cell::string(fmt_num(re) + (im < 0 || std::signbit(im) ? "-" : "+") + fmt_num(std::fabs(im)) + "j");
    }
    case ScalarKind::Bytes:
    case ScalarKind::Unicode: return text_cell(text_value(type, p));
    case ScalarKind::Record: return {};
  }
  return {};
}

}  // namespace

Result<ir::Sheet> array_to_sheet(const Array& array, const Context& ctx) {
  CVT_TRY_ASSIGN(const std::uint64_t n, shape_count(array.shape));
  if (array.data.size() < n * array.dtype.size) return fail(Status::Decode, "array data is shorter than its shape");
  CVT_TRY(check_cells(ctx, n * std::max<std::size_t>(1, array.fields.size())));
  ir::Sheet sheet;
  sheet.name = array.name;
  if (array.dtype.kind == ScalarKind::Record) {
    sheet.has_header = true;
    std::vector<ir::Cell> header;
    for (const Field& f : array.fields) header.push_back(ir::Cell::string(f.name));
    sheet.rows.push_back(std::move(header));
    for (std::uint64_t i = 0; i < n; ++i) {
      std::vector<ir::Cell> row;
      for (const Field& f : array.fields) row.push_back(scalar_cell(f.type, array.element(i) + f.offset));
      sheet.rows.push_back(std::move(row));
    }
    return sheet;
  }
  const std::uint64_t columns = array.shape.size() >= 2 ? array.shape.back() : 1;
  const std::uint64_t rows = columns == 0 ? 0 : n / columns;
  for (std::uint64_t r = 0; r < rows; ++r) {
    if ((r & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(rows)));
    std::vector<ir::Cell> row;
    row.reserve(static_cast<std::size_t>(columns));
    for (std::uint64_t c = 0; c < columns; ++c) row.push_back(scalar_cell(array.dtype, array.element(r * columns + c)));
    sheet.rows.push_back(std::move(row));
  }
  return sheet;
}

Result<Array> sheet_to_array(const ir::Sheet& sheet) {
  Array array;
  array.name = sheet.name;
  std::size_t first = 0;
  const std::size_t columns = sheet.column_count();
  const auto numeric = [](const ir::Cell& c) {
    return c.type == ir::CellType::Number || c.type == ir::CellType::Bool || c.empty() || to_double(c.text).has_value();
  };
  if (!sheet.rows.empty()) {
    bool header = sheet.has_header;
    if (!header && sheet.rows.size() > 1) {
      bool first_text = false;
      for (const auto& c : sheet.rows[0]) first_text = first_text || !numeric(c);
      bool rest_numeric = true;
      for (std::size_t r = 1; r < sheet.rows.size() && rest_numeric; ++r) {
        for (const auto& c : sheet.rows[r]) rest_numeric = rest_numeric && numeric(c);
      }
      header = first_text && rest_numeric;
    }
    if (header) {
      Json names = Json::array();
      for (const auto& c : sheet.rows[0]) names.push_back(c.text);
      array.attributes["column_names"] = std::move(names);
      first = 1;
    }
  }
  bool all_numeric = true;
  bool all_int = true;
  for (std::size_t r = first; r < sheet.rows.size(); ++r) {
    for (std::size_t c = 0; c < columns; ++c) {
      const ir::Cell* cell = c < sheet.rows[r].size() ? &sheet.rows[r][c] : nullptr;
      if (cell == nullptr || cell->empty()) {
        all_int = false;
        continue;
      }
      double v = cell->number;
      if (cell->type != ir::CellType::Number && cell->type != ir::CellType::Bool) {
        const auto parsed = to_double(cell->text);
        if (!parsed) {
          all_numeric = false;
          continue;
        }
        v = *parsed;
      }
      if (v != std::trunc(v) || std::fabs(v) > 9.0e15) all_int = false;
    }
  }
  const std::uint64_t rows = sheet.rows.size() - first;
  array.shape = {rows, columns};
  if (!all_numeric) {
    std::size_t longest = 1;
    for (std::size_t r = first; r < sheet.rows.size(); ++r) {
      for (const auto& c : sheet.rows[r]) longest = std::max(longest, util::utf8_to_utf32(c.text).size());
    }
    array.dtype = {ScalarKind::Unicode, static_cast<std::uint32_t>(longest * 4)};
  } else {
    array.dtype = all_int ? DType{ScalarKind::Int, 8} : DType{ScalarKind::Float, 8};
  }
  array.data.assign(static_cast<std::size_t>(rows * columns * array.dtype.size), std::byte{0});
  for (std::size_t r = first; r < sheet.rows.size(); ++r) {
    for (std::size_t c = 0; c < columns; ++c) {
      std::byte* p = array.data.data() + ((r - first) * columns + c) * array.dtype.size;
      const ir::Cell* cell = c < sheet.rows[r].size() ? &sheet.rows[r][c] : nullptr;
      if (array.dtype.kind == ScalarKind::Unicode) {
        const std::string s = utf32_le(cell != nullptr ? cell->text : std::string(), array.dtype.size / 4);
        ::cvt::util::copy_bytes(p, s.data(), s.size());
        continue;
      }
      double v = std::numeric_limits<double>::quiet_NaN();
      if (cell != nullptr && !cell->empty()) {
        v = cell->type == ir::CellType::Number || cell->type == ir::CellType::Bool ? cell->number
                                                                                  : to_double(cell->text).value_or(v);
      }
      if (array.dtype.kind == ScalarKind::Int && std::isnan(v)) v = 0;
      store_double(array.dtype, p, v);
    }
  }
  return array;
}

// --------------------------------------------------------------------------- collections

Result<Json> collection_to_json(const Collection& collection, const Context& ctx) {
  Json variables = Json::array();
  for (const Array& a : collection.arrays) {
    CVT_TRY_ASSIGN(Json json, array_to_json(a, ctx));
    variables.push_back(std::move(json));
  }
  Json out = Json::object();
  out["variables"] = std::move(variables);
  if (!collection.attributes.empty()) out["attributes"] = collection.attributes;
  return out;
}

Result<Collection> json_to_collection(const Json& value, const Context& ctx) {
  Collection c;
  const auto add = [&](const Json& item, std::string_view fallback_name) -> Result<void> {
    CVT_TRY_ASSIGN(Array a, json_to_array(item, ctx));
    if (a.name.empty()) a.name = std::string(fallback_name);
    c.arrays.push_back(std::move(a));
    return {};
  };
  if (value.is_object() && value.contains("variables")) {
    const Json& vars = value["variables"];
    if (vars.is_array()) {
      std::size_t i = 0;
      for (const Json& v : vars.elements()) CVT_TRY(add(v, "array" + std::to_string(++i)));
    } else {
      for (const auto& [k, v] : vars.items()) CVT_TRY(add(v, k));
    }
    if (const Json* attrs = value.find("attributes"); attrs != nullptr && attrs->is_object()) c.attributes = *attrs;
  } else if (value.is_object() && (value.contains("data") || value.contains("values"))) {
    CVT_TRY(add(value, "array"));
  } else if (value.is_object()) {
    for (const auto& [k, v] : value.items()) {
      if (v.is_array() || (v.is_object() && (v.contains("data") || v.contains("values"))) || v.is_number()) CVT_TRY(add(v, k));
    }
  } else if (value.is_array() || value.is_number()) {
    CVT_TRY(add(value, "array"));
  }
  if (c.arrays.empty()) return fail(Status::Encode, "the data holds no arrays");
  return c;
}

Result<ir::Table> collection_to_table(const Collection& collection, const Context& ctx) {
  ir::Table table;
  for (std::size_t i = 0; i < collection.arrays.size(); ++i) {
    CVT_TRY_ASSIGN(ir::Sheet sheet, array_to_sheet(collection.arrays[i], ctx));
    if (sheet.name.empty()) sheet.name = "array" + std::to_string(i + 1);
    table.sheets.push_back(std::move(sheet));
  }
  if (!collection.attributes.empty()) table.sheets.push_back(key_value_sheet("attributes", collection.attributes));
  return table;
}

Result<Collection> table_to_collection(const ir::Table& table) {
  Collection c;
  for (std::size_t i = 0; i < table.sheets.size(); ++i) {
    const ir::Sheet& sheet = table.sheets[i];
    if (util::iequals(sheet.name, "attributes") && i > 0) {
      for (std::size_t r = 1; r < sheet.rows.size(); ++r) {
        c.attributes[cell_text(sheet, r, 0)] = std::string(cell_text(sheet, r, 1));
      }
      continue;
    }
    if (sheet.rows.empty()) continue;
    CVT_TRY_ASSIGN(Array a, sheet_to_array(sheet));
    if (a.name.empty()) a.name = table.sheets.size() == 1 ? "data" : "array" + std::to_string(i + 1);
    std::string name;
    for (const char ch : a.name) name.push_back(util::is_ascii_alpha(ch) || util::is_ascii_digit(ch) ? ch : '_');
    a.name = name;
    c.arrays.push_back(std::move(a));
  }
  if (c.arrays.empty()) return fail(Status::Encode, "the table has no data");
  return c;
}

void add_collection_codecs(Registry& registry, const std::string& id, CollectionReader read, CollectionWriter write,
                           std::vector<OptionSpec> read_options, std::vector<OptionSpec> write_options) {
  ReadFn read_data;
  ReadFn read_table;
  WriteFn write_data;
  WriteFn write_table;
  if (read) {
    read_data = [read](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const Collection c, read(ctx));
      CVT_TRY_ASSIGN(Json json, collection_to_json(c, ctx));
      return ir::Data{std::move(json)};
    };
    read_table = [read](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const Collection c, read(ctx));
      CVT_TRY_ASSIGN(ir::Table table, collection_to_table(c, ctx));
      return table;
    };
  }
  if (write) {
    write_data = [write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(const Collection c, json_to_collection(value.get<ir::Data>().value, ctx));
      return write(c, ctx);
    };
    write_table = [write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(const Collection c, table_to_collection(value.get<ir::Table>()));
      return write(c, ctx);
    };
  }
  add_codec(registry, id, Kind::Data, read_data, write_data, read_options, write_options, 10, 10);
  add_codec(registry, id, Kind::Table, read_table, write_table, read_options, write_options, 12, 12);
}

}  // namespace cvt::science::arrays
