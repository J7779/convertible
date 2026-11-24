// Convertible core - science family: NumPy .npy (format 1.0-3.0, every
// fixed-size dtype incl. structured records, both byte orders, Fortran order)
// and .npz (ZIP of .npy members, stored or deflated). Object arrays (pickles)
// are refused: they would require executing code.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <format>

#include "engine/standard_options.hpp"
#include "families/science/arrays.hpp"
#include "families/science/science.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::science::arrays {

namespace {

// --------------------------------------------------------------------------- Python literal

/// The subset of Python literals NumPy writes in .npy headers: dicts, lists,
/// tuples, strings, integers, True/False/None.
class PyLiteral {
 public:
  explicit PyLiteral(std::string_view text) : text_(text) {}

  Result<Json> parse() {
    CVT_TRY_ASSIGN(Json value, value_at(0));
    skip();
    return value;
  }

 private:
  void skip() {
    while (pos_ < text_.size() && util::is_ascii_space(text_[pos_])) ++pos_;
  }

  Result<Json> value_at(int depth) {
    if (depth > 16) return fail(Status::Decode, "npy header is nested too deeply");
    skip();
    if (pos_ >= text_.size()) return fail(Status::Decode, "npy header ends early");
    const char c = text_[pos_];
    if (c == '{' || c == '[' || c == '(') {
      const char close = c == '{' ? '}' : c == '[' ? ']' : ')';
      ++pos_;
      Json out = c == '{' ? Json::object() : Json::array();
      while (true) {
        skip();
        if (pos_ < text_.size() && text_[pos_] == close) {
          ++pos_;
          return out;
        }
        CVT_TRY_ASSIGN(Json item, value_at(depth + 1));
        skip();
        if (c == '{') {
          if (pos_ >= text_.size() || text_[pos_] != ':') return fail(Status::Decode, "npy header: expected ':'");
          ++pos_;
          CVT_TRY_ASSIGN(Json v, value_at(depth + 1));
          out[item.is_string() ? item.as_string() : util::dump_json(item)] = std::move(v);
          skip();
        } else {
          out.push_back(std::move(item));
        }
        if (pos_ < text_.size() && text_[pos_] == ',') {
          ++pos_;
          continue;
        }
        skip();
        if (pos_ < text_.size() && text_[pos_] == close) continue;
        return fail(Status::Decode, "npy header: malformed container");
      }
    }
    if (c == '\'' || c == '"') {
      ++pos_;
      std::string s;
      while (pos_ < text_.size() && text_[pos_] != c) {
        if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) ++pos_;
        s.push_back(text_[pos_++]);
      }
      if (pos_ >= text_.size()) return fail(Status::Decode, "npy header: unterminated string");
      ++pos_;
      return Json(std::move(s));
    }
    const std::size_t start = pos_;
    while (pos_ < text_.size() && (util::is_ascii_alpha(text_[pos_]) || util::is_ascii_digit(text_[pos_]) ||
                                   text_[pos_] == '-' || text_[pos_] == '+' || text_[pos_] == 'L')) {
      ++pos_;
    }
    std::string_view word = text_.substr(start, pos_ - start);
    if (word == "True") return Json(true);
    if (word == "False") return Json(false);
    if (word == "None") return Json();
    if (word.ends_with('L')) word.remove_suffix(1);
    if (const auto n = util::parse_int(word); n && *n >= 0) return Json(static_cast<std::uint64_t>(*n));
    return fail(Status::Decode, "npy header: unexpected '{}'", word.substr(0, 20));
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

// --------------------------------------------------------------------------- .npy

Result<Array> parse_npy(ByteView input, const Context& ctx, std::string name) {
  util::ByteReader reader(input);
  if (!reader.consume("\x93NUMPY")) return fail(Status::Decode, "not a NumPy .npy file");
  const std::uint8_t major = reader.u8();
  (void)reader.u8();
  std::uint32_t header_len = 0;
  if (major == 1) {
    header_len = reader.u16le();
  } else if (major == 2 || major == 3) {
    header_len = reader.u32le();
  } else {
    return fail(Status::Decode, "unsupported .npy format version {}", major);
  }
  if (!reader.ok() || header_len > reader.remaining() || header_len > (1u << 24)) {
    return fail(Status::Decode, ".npy header is truncated");
  }
  const std::string_view header_text = reader.chars(header_len);
  CVT_TRY_ASSIGN(const Json header, PyLiteral(header_text).parse());
  if (!header.is_object()) return fail(Status::Decode, ".npy header is not a dictionary");
  Array array;
  array.name = std::move(name);
  const Json& descr = header["descr"];
  bool big_endian = false;
  std::vector<bool> field_big;
  if (descr.is_string()) {
    if (descr.as_string().starts_with("|O") || descr.as_string().starts_with("<O") || descr.as_string() == "O") {
      return fail(Status::Decode, "object arrays (pickled Python objects) are not supported");
    }
    const auto type = parse_numpy_descr(descr.as_string(), big_endian);
    if (!type) return fail(Status::Decode, "unsupported NumPy dtype '{}'", descr.as_string());
    array.dtype = *type;
  } else if (descr.is_array()) {
    std::uint32_t offset = 0;
    for (const Json& f : descr.elements()) {
      if (!f.is_array() || f.size() < 2 || !f[1].is_string()) {
        return fail(Status::Decode, "unsupported structured dtype (nested or sub-array fields)");
      }
      if (f.size() > 2) return fail(Status::Decode, "sub-array fields in structured dtypes are not supported");
      const std::string& type_text = f[1].as_string();
      bool be = false;
      if (type_text.size() >= 2 && (type_text[1] == 'V' || type_text[0] == 'V')) {
        // Padding: skip its bytes.
        const auto n = util::parse_int(std::string_view(type_text).substr(type_text[0] == 'V' ? 1 : 2));
        if (!n || *n < 0 || *n > (1 << 20)) return fail(Status::Decode, "bad padding field");
        offset += static_cast<std::uint32_t>(*n);
        continue;
      }
      if (type_text.find('O') != std::string::npos) return fail(Status::Decode, "object fields are not supported");
      const auto type = parse_numpy_descr(type_text, be);
      if (!type) return fail(Status::Decode, "unsupported field dtype '{}'", type_text);
      array.fields.push_back(Field{f[0].as_string(), *type, offset});
      field_big.push_back(be);
      offset += type->size;
      if (offset > (1u << 24)) return fail(Status::Decode, "record too large");
    }
    if (array.fields.empty()) return fail(Status::Decode, "structured dtype without fields");
    array.dtype = DType{ScalarKind::Record, offset};
  } else {
    return fail(Status::Decode, ".npy header has no descr");
  }
  for (const Json& d : header["shape"].elements()) array.shape.push_back(d.as_uint64());
  if (!header["shape"].is_array()) return fail(Status::Decode, ".npy header has no shape");
  CVT_TRY_ASSIGN(const std::uint64_t count, shape_count(array.shape));
  CVT_TRY(check_array_bytes(ctx, count, array.dtype.size));
  const std::uint64_t bytes = count * array.dtype.size;
  if (reader.remaining() < bytes) return fail(Status::Decode, ".npy data is truncated");
  const ByteView raw = reader.bytes(static_cast<std::size_t>(bytes));
  array.data.assign(raw.begin(), raw.end());
  if (array.dtype.kind == ScalarKind::Record) {
    bool any_big = false;
    for (const bool b : field_big) any_big = any_big || b;
    if (any_big) {
      // Swap only the big-endian fields.
      Array copy = array;
      std::vector<Field> big_fields;
      for (std::size_t i = 0; i < array.fields.size(); ++i) {
        if (field_big[i]) big_fields.push_back(array.fields[i]);
      }
      copy.fields = big_fields;
      swap_elements(copy);
      array.data = std::move(copy.data);
    }
  } else if (big_endian) {
    swap_elements(array);
  }
  if (header["fortran_order"].as_bool(false)) fortran_to_c(array);
  return array;
}

std::string shape_text(const std::vector<std::uint64_t>& shape) {
  std::string out = "(";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    out += std::to_string(shape[i]);
    if (shape.size() == 1 || i + 1 < shape.size()) out += ",";
    if (i + 1 < shape.size()) out += " ";
  }
  return out + ")";
}

Result<Array> cast_to(const Array& source, std::string_view dtype_option, const Context& ctx) {
  if (dtype_option.empty() || dtype_option == "auto") return source;
  const auto target = parse_dtype_name(dtype_option);
  if (!target || target->kind == ScalarKind::Record || target->kind == ScalarKind::Bytes || target->kind == ScalarKind::Unicode) {
    return fail(Status::InvalidArgument, "unsupported array_dtype '{}'", dtype_option);
  }
  if (source.dtype.kind == ScalarKind::Record || source.dtype.kind == ScalarKind::Bytes ||
      source.dtype.kind == ScalarKind::Unicode) {
    ctx.warn("Only numeric arrays are converted to another dtype; '" + source.name + "' kept its type.");
    return source;
  }
  Array out = source;
  out.dtype = *target;
  const std::uint64_t n = source.count();
  CVT_TRY(check_array_bytes(ctx, n, target->size));
  out.data.assign(static_cast<std::size_t>(n * target->size), std::byte{0});
  for (std::uint64_t i = 0; i < n; ++i) {
    if (source.dtype.kind == ScalarKind::Complex && target->kind == ScalarKind::Complex) {
      const std::uint32_t sp = source.dtype.size / 2;
      const std::uint32_t tp = target->size / 2;
      store_double(DType{ScalarKind::Float, tp}, out.data.data() + i * target->size,
                   scalar_double(DType{ScalarKind::Float, sp}, source.element(i)));
      store_double(DType{ScalarKind::Float, tp}, out.data.data() + i * target->size + tp,
                   scalar_double(DType{ScalarKind::Float, sp}, source.element(i) + sp));
    } else {
      store_double(*target, out.data.data() + i * target->size, source.number(i));
    }
  }
  return out;
}

Result<Bytes> encode_npy(const Array& array, bool fortran) {
  std::string descr;
  if (array.dtype.kind == ScalarKind::Record) {
    descr = "[";
    std::uint32_t at = 0;
    for (const Field& f : array.fields) {
      if (f.offset > at) descr += std::format("('', '|V{}'), ", f.offset - at);
      descr += "('" + f.name + "', '" + numpy_descr(f.type) + "'), ";
      at = f.offset + f.type.size;
    }
    if (descr.size() > 2) descr.resize(descr.size() - 2);
    descr += "]";
  } else {
    descr = "'" + numpy_descr(array.dtype) + "'";
  }
  std::string header = "{'descr': " + descr + ", 'fortran_order': " + (fortran ? "True" : "False") +
                       ", 'shape': " + shape_text(array.shape) + ", }";
  // Pad with spaces so that magic + version + length + header is a multiple of 64.
  const bool v2 = header.size() + 11 > 65535;
  const std::size_t prefix = v2 ? 12 : 10;
  std::size_t total = prefix + header.size() + 1;
  header.append((64 - total % 64) % 64, ' ');
  header += '\n';
  util::ByteWriter out(prefix + header.size() + array.data.size());
  out.u8(0x93).text("NUMPY").u8(v2 ? 2 : 1).u8(0);
  if (v2) {
    out.u32le(static_cast<std::uint32_t>(header.size()));
  } else {
    out.u16le(static_cast<std::uint16_t>(header.size()));
  }
  out.text(header);
  if (fortran) {
    Array copy = array;
    c_to_fortran(copy);
    out.bytes(copy.data);
  } else {
    out.bytes(array.data);
  }
  return out.take();
}

Result<Collection> read_npy(ReadContext& ctx) {
  std::string name = ctx.filename;
  if (const std::size_t dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
  if (name.empty()) name = "array";
  CVT_TRY_ASSIGN(Array array, parse_npy(ctx.input, ctx, name));
  Collection c;
  c.arrays.push_back(std::move(array));
  return c;
}

Result<Bytes> write_npy(const Collection& collection, WriteContext& ctx) {
  if (collection.arrays.size() > 1) ctx.warn("A .npy file holds one array; only the first was written (use .npz for several).");
  CVT_TRY_ASSIGN(const Array array, cast_to(collection.arrays.front(), ctx.options.get_string("array_dtype", "auto"), ctx));
  return encode_npy(array, ctx.options.get_bool("fortran_order", false));
}

Result<Collection> read_npz(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
  Collection c;
  for (std::size_t i = 0; i < zip.size(); ++i) {
    const util::ZipEntry& entry = zip.entries()[i];
    if (entry.is_directory || !entry.name.ends_with(".npy")) continue;
    CVT_TRY_ASSIGN(const Bytes member, zip.read(i));
    std::string name = entry.name.substr(0, entry.name.size() - 4);
    CVT_TRY_ASSIGN(Array array, parse_npy(member, ctx, std::move(name)));
    c.arrays.push_back(std::move(array));
    CVT_TRY(ctx.checkpoint(static_cast<double>(i + 1) / static_cast<double>(zip.size())));
  }
  if (c.arrays.empty()) return fail(Status::Decode, "the .npz archive holds no .npy members");
  return c;
}

Result<Bytes> write_npz(const Collection& collection, WriteContext& ctx) {
  util::ZipWriter zip;
  const int level = options::resolved_compression_level(ctx.options, 9);
  std::vector<std::string> used;
  for (const Array& source : collection.arrays) {
    CVT_TRY_ASSIGN(const Array array, cast_to(source, ctx.options.get_string("array_dtype", "auto"), ctx));
    CVT_TRY_ASSIGN(const Bytes npy, encode_npy(array, ctx.options.get_bool("fortran_order", false)));
    std::string name = array.name;
    while (name.starts_with('/')) name.erase(0, 1);  // HDF5 paths
    if (name.empty()) name = "arr_" + std::to_string(used.size());
    while (std::find(used.begin(), used.end(), name) != used.end()) name += "_";
    used.push_back(name);
    util::ZipEntryOptions entry;
    entry.level = level;
    CVT_TRY(zip.add(name + ".npy", npy, entry));
  }
  return zip.finish();
}

int sniff_npz(ByteView data, std::string_view name) {
  if (!head_text(data, 4).starts_with("PK\x03\x04")) return 0;
  // The first member name follows the 30-byte local header.
  if (data.size() < 34) return 0;
  util::ByteReader reader(data.subspan(26));
  const std::uint16_t name_len = reader.u16le();
  const std::string_view first = as_chars(data.subspan(30, std::min<std::size_t>(name_len, data.size() - 30)));
  if (first.ends_with(".npy")) return 90;
  return has_extension(name, "npz") ? 80 : 0;
}

std::vector<OptionSpec> numpy_write_options(bool npz) {
  std::vector<OptionSpec> out = {
      OptionSpec::choice("array_dtype", "Element type", "auto",
                         {"auto", "float64", "float32", "float16", "int64", "int32", "int16", "int8", "uint64", "uint32",
                          "uint16", "uint8", "bool", "complex128", "complex64"},
                         "Convert numeric arrays to this type; auto keeps the source type."),
      OptionSpec::boolean("fortran_order", "Fortran order", false, "Store elements column-major.").advanced(),
  };
  if (npz) {
    out.push_back(options::compression_level(9));
    out.push_back(options::preset());
  }
  return out;
}

}  // namespace
}  // namespace cvt::science::arrays

namespace cvt::science {

void register_numpy(Registry& registry) {
  using namespace arrays;
  add_format(registry, {.id = "npy",
                        .name = "NumPy array",
                        .description = "One NumPy array (.npy format 1.0-3.0).",
                        .extensions = {"npy"},
                        .mime = "application/x-npy",
                        .popularity = 30,
                        .signatures = {Signature::at(0, "93 'NUMPY'")}});
  add_collection_codecs(registry, "npy", read_npy, write_npy, {}, numpy_write_options(false));
  add_format(registry, {.id = "npz",
                        .name = "NumPy array archive",
                        .description = "Several named NumPy arrays in a ZIP archive (savez).",
                        .extensions = {"npz"},
                        .mime = "application/x-npz",
                        .popularity = 22,
                        .sniff = sniff_npz});
  add_collection_codecs(registry, "npz", read_npz, write_npz, {}, numpy_write_options(true));
}

}  // namespace cvt::science
