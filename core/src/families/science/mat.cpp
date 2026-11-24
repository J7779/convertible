// Convertible core - science family: MATLAB MAT-files. Level 5 (v5/v6/v7:
// numeric, logical, char, cell, struct, object, sparse; compressed elements)
// read and write (numeric, logical, char, struct, cell); level 4 read.
//
// Data: {"variables":[...]} where numeric/logical/char variables use the
// array model (C order, char matrices as one string per row) and containers
// are {"name","class":"struct","fields":{...}} (1x1),
// {"class":"struct","shape":[..],"data":[{...}]} or
// {"class":"cell","shape":[..],"data":[nested lists of values]}.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <format>

#include "engine/standard_options.hpp"
#include "families/science/arrays.hpp"
#include "families/science/science.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::science::arrays {

namespace {

enum MiType : std::uint32_t {
  miINT8 = 1, miUINT8 = 2, miINT16 = 3, miUINT16 = 4, miINT32 = 5, miUINT32 = 6, miSINGLE = 7, miDOUBLE = 9,
  miINT64 = 12, miUINT64 = 13, miMATRIX = 14, miCOMPRESSED = 15, miUTF8 = 16, miUTF16 = 17, miUTF32 = 18,
};

enum MxClass : std::uint8_t {
  mxCELL = 1, mxSTRUCT = 2, mxOBJECT = 3, mxCHAR = 4, mxSPARSE = 5, mxDOUBLE = 6, mxSINGLE = 7, mxINT8 = 8,
  mxUINT8 = 9, mxINT16 = 10, mxUINT16 = 11, mxINT32 = 12, mxUINT32 = 13, mxINT64 = 14, mxUINT64 = 15,
};

std::optional<DType> mi_dtype(std::uint32_t type) {
  switch (type) {
    case miINT8: return DType{ScalarKind::Int, 1};
    case miUINT8:
    case miUTF8: return DType{ScalarKind::UInt, 1};
    case miINT16: return DType{ScalarKind::Int, 2};
    case miUINT16:
    case miUTF16: return DType{ScalarKind::UInt, 2};
    case miINT32: return DType{ScalarKind::Int, 4};
    case miUINT32:
    case miUTF32: return DType{ScalarKind::UInt, 4};
    case miSINGLE: return DType{ScalarKind::Float, 4};
    case miDOUBLE: return DType{ScalarKind::Float, 8};
    case miINT64: return DType{ScalarKind::Int, 8};
    case miUINT64: return DType{ScalarKind::UInt, 8};
    default: return std::nullopt;
  }
}

std::optional<DType> class_dtype(std::uint8_t mx) {
  switch (mx) {
    case mxDOUBLE: return DType{ScalarKind::Float, 8};
    case mxSINGLE: return DType{ScalarKind::Float, 4};
    case mxINT8: return DType{ScalarKind::Int, 1};
    case mxUINT8: return DType{ScalarKind::UInt, 1};
    case mxINT16: return DType{ScalarKind::Int, 2};
    case mxUINT16: return DType{ScalarKind::UInt, 2};
    case mxINT32: return DType{ScalarKind::Int, 4};
    case mxUINT32: return DType{ScalarKind::UInt, 4};
    case mxINT64: return DType{ScalarKind::Int, 8};
    case mxUINT64: return DType{ScalarKind::UInt, 8};
    default: return std::nullopt;
  }
}

std::uint8_t dtype_class(DType t) {
  switch (t.kind) {
    case ScalarKind::Float:
    case ScalarKind::Complex: return (t.kind == ScalarKind::Complex ? t.size / 2 : t.size) == 4 ? mxSINGLE : mxDOUBLE;
    case ScalarKind::Int: return t.size == 1 ? mxINT8 : t.size == 2 ? mxINT16 : t.size == 4 ? mxINT32 : mxINT64;
    case ScalarKind::UInt:
    case ScalarKind::Bool: return t.size == 1 ? mxUINT8 : t.size == 2 ? mxUINT16 : t.size == 4 ? mxUINT32 : mxUINT64;
    default: return mxDOUBLE;
  }
}

std::uint32_t class_mi(std::uint8_t mx) {
  switch (mx) {
    case mxDOUBLE: return miDOUBLE;
    case mxSINGLE: return miSINGLE;
    case mxINT8: return miINT8;
    case mxUINT8: return miUINT8;
    case mxINT16: return miINT16;
    case mxUINT16: return miUINT16;
    case mxINT32: return miINT32;
    case mxUINT32: return miUINT32;
    case mxINT64: return miINT64;
    default: return miUINT64;
  }
}

struct Element {
  std::uint32_t type = 0;
  ByteView data;
};

class Mat5Reader {
 public:
  Mat5Reader(const Context& ctx, bool big_endian) : ctx_(ctx), order_(big_endian ? std::endian::big : std::endian::little) {}

  /// Read one tagged element from `reader`.
  Result<Element> element(util::ByteReader& reader) {
    if (reader.remaining() < 8) return fail(Status::Decode, "MAT element tag is truncated");
    const std::uint32_t first = reader.read<std::uint32_t>(order_);
    Element e;
    if ((first >> 16) != 0) {  // small data element
      e.type = first & 0xFFFF;
      const std::uint32_t n = first >> 16;
      if (n > 4) return fail(Status::Decode, "bad small MAT element");
      e.data = reader.bytes(4).first(n);
      return e;
    }
    e.type = first;
    const std::uint32_t n = reader.read<std::uint32_t>(order_);
    if (n > reader.remaining()) return fail(Status::Decode, "MAT element of {} bytes is truncated", n);
    e.data = reader.bytes(n);
    if (e.type != miCOMPRESSED) {
      const std::size_t pad = (8 - n % 8) % 8;
      (void)reader.bytes(std::min(pad, reader.remaining()));
    }
    return e;
  }

  /// Numeric content of an element converted to doubles (or kept raw).
  Result<Array> numeric(const Element& e) {
    const auto type = mi_dtype(e.type);
    if (!type) return fail(Status::Decode, "unexpected MAT data type {}", e.type);
    Array a;
    a.dtype = *type;
    if (e.data.size() % type->size != 0) return fail(Status::Decode, "MAT data size is not a multiple of its type");
    a.data.assign(e.data.begin(), e.data.end());
    a.shape = {e.data.size() / type->size};
    if (order_ == std::endian::big) swap_elements(a);
    return a;
  }

  Result<std::vector<std::int32_t>> int32s(const Element& e) {
    CVT_TRY_ASSIGN(const Array a, numeric(e));
    std::vector<std::int32_t> out;
    for (std::uint64_t i = 0; i < a.count(); ++i) out.push_back(static_cast<std::int32_t>(a.number(i)));
    return out;
  }

  /// Parse a miMATRIX element body into JSON (named when `with_name`).
  Result<Json> matrix(ByteView body, int depth, std::string* name_out) {
    if (depth > max_nesting) return fail(Status::Limit, "MAT cells/structs are nested too deeply");
    util::ByteReader reader(body);
    if (reader.remaining() == 0) return Json::object({{"class", "empty"}});  // empty cell placeholder
    CVT_TRY_ASSIGN(const Element flags_el, element(reader));
    if (flags_el.data.size() < 8) return fail(Status::Decode, "MAT array flags are truncated");
    util::ByteReader flags(flags_el.data);
    const std::uint32_t flag_word = flags.read<std::uint32_t>(order_);
    const std::uint8_t mx = static_cast<std::uint8_t>(flag_word & 0xFF);
    const bool is_complex = (flag_word & 0x0800) != 0;
    const bool is_logical = (flag_word & 0x0200) != 0;
    CVT_TRY_ASSIGN(const Element dims_el, element(reader));
    CVT_TRY_ASSIGN(const std::vector<std::int32_t> dims32, int32s(dims_el));
    std::vector<std::uint64_t> dims;
    for (const auto d : dims32) {
      if (d < 0) return fail(Status::Decode, "negative MAT dimension");
      dims.push_back(static_cast<std::uint64_t>(d));
    }
    CVT_TRY_ASSIGN(const std::uint64_t count, shape_count(dims));
    CVT_TRY_ASSIGN(const Element name_el, element(reader));
    const std::string name = util::sanitize_utf8(as_chars(name_el.data));
    if (name_out != nullptr) *name_out = name;
    Json out;
    if (const auto type = class_dtype(mx); type || mx == mxCHAR) {
      // Numeric / logical / char: real part (stored type may be narrower) and optional imaginary part.
      CVT_TRY_ASSIGN(const Element real_el, element(reader));
      CVT_TRY_ASSIGN(const Array real, numeric(real_el));
      if (real.count() != count) return fail(Status::Decode, "MAT variable '{}' has {} values for {} elements", name, real.count(), count);
      CVT_TRY(check_array_bytes(ctx_, count, 16));
      Array a;
      a.name = name;
      a.shape = dims;
      if (mx == mxCHAR) {
        // Column-major code units -> one string per row (2-D) .
        const std::uint64_t rows = dims.empty() ? 0 : dims[0];
        const std::uint64_t cols = rows == 0 ? 0 : count / rows;
        std::vector<std::string> lines(static_cast<std::size_t>(rows));
        std::u16string units;
        for (std::uint64_t r = 0; r < rows; ++r) {
          std::u32string row;
          for (std::uint64_t c = 0; c < cols; ++c) row.push_back(static_cast<char32_t>(real.number(c * rows + r)));
          if (real_el.type == miUTF8) {
            std::string bytes;
            for (const char32_t ch : row) bytes.push_back(static_cast<char>(ch));
            lines[static_cast<std::size_t>(r)] = util::sanitize_utf8(bytes);
          } else if (real_el.type == miUTF16 || real_el.type == miUINT16) {
            std::u16string u16(row.begin(), row.end());
            lines[static_cast<std::size_t>(r)] = util::utf16_to_utf8(u16);
          } else {
            for (char32_t& ch : row) {
              if (!util::is_scalar_value(ch)) ch = U'�';
            }
            lines[static_cast<std::size_t>(r)] = util::utf32_to_utf8(row);
          }
        }
        out = Json::object();
        out["name"] = name;
        out["class"] = "char";
        out["shape"] = Json::array({rows, cols});
        Json data = Json::array();
        for (auto& line : lines) data.push_back(std::move(line));
        out["data"] = rows == 1 ? Json(data[0]) : std::move(data);
        return out;
      }
      if (is_complex) {
        CVT_TRY_ASSIGN(const Element imag_el, element(reader));
        CVT_TRY_ASSIGN(const Array imag, numeric(imag_el));
        if (imag.count() != count) return fail(Status::Decode, "MAT imaginary part has the wrong size");
        a.dtype = DType{ScalarKind::Complex, type->size == 4 ? 8u : 16u};
        a.data.assign(static_cast<std::size_t>(count * a.dtype.size), std::byte{0});
        const DType part{ScalarKind::Float, a.dtype.size / 2};
        for (std::uint64_t i = 0; i < count; ++i) {
          store_double(part, a.data.data() + i * a.dtype.size, real.number(i));
          store_double(part, a.data.data() + i * a.dtype.size + part.size, imag.number(i));
        }
      } else {
        a.dtype = is_logical ? DType{ScalarKind::Bool, 1} : *type;
        if (real.dtype == a.dtype) {
          a.data = real.data;
        } else {
          a.data.assign(static_cast<std::size_t>(count * a.dtype.size), std::byte{0});
          for (std::uint64_t i = 0; i < count; ++i) {
            // Integers stored in a narrower type are widened exactly.
            if ((a.dtype.kind == ScalarKind::Int || a.dtype.kind == ScalarKind::UInt) && a.dtype.size == 8 &&
                (real.dtype.kind == ScalarKind::Int || real.dtype.kind == ScalarKind::UInt) && real.dtype.size == 8) {
              std::memcpy(a.data.data() + i * 8, real.element(i), 8);
            } else {
              store_double(a.dtype, a.data.data() + i * a.dtype.size, real.number(i));
            }
          }
        }
      }
      fortran_to_c(a);
      CVT_TRY_ASSIGN(out, array_to_json(a, ctx_));
      return out;
    }
    if (mx == mxCELL) {
      Json values = Json::array();
      for (std::uint64_t i = 0; i < count; ++i) {
        CVT_TRY_ASSIGN(const Element child, element(reader));
        if (child.type != miMATRIX) return fail(Status::Decode, "MAT cell element is not a matrix");
        CVT_TRY_ASSIGN(Json v, matrix(child.data, depth + 1, nullptr));
        v.erase("name");
        values.push_back(std::move(v));
      }
      out = Json::object({{"name", name}, {"class", "cell"}});
      Json shape = Json::array();
      for (const auto d : dims) shape.push_back(d);
      out["shape"] = std::move(shape);
      out["data"] = c_order_nested(values, dims);
      return out;
    }
    if (mx == mxSTRUCT || mx == mxOBJECT) {
      std::string class_name;
      if (mx == mxOBJECT) {
        CVT_TRY_ASSIGN(const Element cls, element(reader));
        class_name = util::sanitize_utf8(as_chars(cls.data));
      }
      CVT_TRY_ASSIGN(const Element len_el, element(reader));
      CVT_TRY_ASSIGN(const std::vector<std::int32_t> len, int32s(len_el));
      if (len.empty() || len[0] <= 0 || len[0] > 4096) return fail(Status::Decode, "bad MAT struct field-name length");
      CVT_TRY_ASSIGN(const Element names_el, element(reader));
      const std::size_t width = static_cast<std::size_t>(len[0]);
      std::vector<std::string> fields;
      for (std::size_t at = 0; at + width <= names_el.data.size(); at += width) {
        const std::string_view raw = as_chars(names_el.data.subspan(at, width));
        fields.push_back(util::sanitize_utf8(raw.substr(0, raw.find('\0'))));
      }
      if (fields.size() > 65536) return fail(Status::Limit, "too many MAT struct fields");
      Json records = Json::array();
      for (std::uint64_t i = 0; i < count; ++i) {
        Json record = Json::object();
        for (const auto& field : fields) {
          CVT_TRY_ASSIGN(const Element child, element(reader));
          if (child.type != miMATRIX) return fail(Status::Decode, "MAT struct field is not a matrix");
          CVT_TRY_ASSIGN(Json v, matrix(child.data, depth + 1, nullptr));
          v.erase("name");
          record[field] = std::move(v);
        }
        records.push_back(std::move(record));
      }
      out = Json::object({{"name", name}, {"class", mx == mxOBJECT ? "object" : "struct"}});
      if (!class_name.empty()) out["class_name"] = class_name;
      if (count == 1) {
        out["fields"] = std::move(records[0]);
      } else {
        Json shape = Json::array();
        for (const auto d : dims) shape.push_back(d);
        out["shape"] = std::move(shape);
        out["data"] = c_order_nested(records, dims);
      }
      return out;
    }
    if (mx == mxSPARSE) {
      CVT_TRY_ASSIGN(const Element ir_el, element(reader));
      CVT_TRY_ASSIGN(const Element jc_el, element(reader));
      CVT_TRY_ASSIGN(const Element pr_el, element(reader));
      CVT_TRY_ASSIGN(const std::vector<std::int32_t> ir, int32s(ir_el));
      CVT_TRY_ASSIGN(const std::vector<std::int32_t> jc, int32s(jc_el));
      CVT_TRY_ASSIGN(const Array pr, numeric(pr_el));
      if (dims.size() != 2) return fail(Status::Decode, "sparse MAT matrix must be 2-D");
      // Keep sparse matrices sparse: coordinate lists.
      Json rows = Json::array(), cols = Json::array(), values = Json::array();
      for (std::size_t c = 0; c + 1 < jc.size(); ++c) {
        for (std::int32_t k = jc[c]; k < jc[c + 1]; ++k) {
          if (k < 0 || static_cast<std::size_t>(k) >= ir.size() || static_cast<std::uint64_t>(k) >= pr.count()) {
            return fail(Status::Decode, "sparse MAT index out of range");
          }
          rows.push_back(ir[static_cast<std::size_t>(k)]);
          cols.push_back(static_cast<std::uint64_t>(c));
          values.push_back(is_logical ? Json(pr.number(static_cast<std::uint64_t>(k)) != 0) : json_number(pr.number(static_cast<std::uint64_t>(k))));
        }
      }
      return Json::object({{"name", name},
                           {"class", "sparse"},
                           {"shape", Json::array({dims[0], dims[1]})},
                           {"row", std::move(rows)},
                           {"col", std::move(cols)},
                           {"value", std::move(values)}});
    }
    return Json::object({{"name", name}, {"class", "unsupported"}, {"mx_class", static_cast<int>(mx)}});
  }

  /// Column-major list of values -> nested C-order lists for `dims`.
  static Json c_order_nested(const Json& values, const std::vector<std::uint64_t>& dims) {
    Array index;
    index.dtype = {ScalarKind::UInt, 8};
    index.shape = dims;
    const std::uint64_t n = values.size();
    index.data.resize(static_cast<std::size_t>(n * 8));
    for (std::uint64_t i = 0; i < n; ++i) {
      for (int b = 0; b < 8; ++b) index.data[static_cast<std::size_t>(i * 8 + static_cast<std::uint64_t>(b))] = static_cast<std::byte>((i >> (8 * b)) & 0xFF);
    }
    fortran_to_c(index);
    // Build nested lists over the C-order index.
    std::vector<Json> flat;
    for (std::uint64_t i = 0; i < n; ++i) flat.push_back(values[static_cast<std::size_t>(index.number(i))]);
    if (dims.empty()) return flat.empty() ? Json() : flat.front();
    std::vector<Json> level = std::move(flat);
    for (std::size_t d = dims.size(); d-- > 0;) {
      const std::uint64_t dim = dims[d];
      std::vector<Json> next;
      if (dim == 0) {
        next.push_back(Json::array());
      } else {
        for (std::size_t g = 0; g < level.size() / dim; ++g) {
          Json list = Json::array();
          for (std::uint64_t k = 0; k < dim; ++k) list.push_back(std::move(level[g * dim + k]));
          next.push_back(std::move(list));
        }
      }
      level = std::move(next);
    }
    return level.empty() ? Json::array() : std::move(level.front());
  }

 private:
  const Context& ctx_;
  std::endian order_;
};

Result<Json> read_mat5(ByteView input, const Context& ctx) {
  if (input.size() < 128) return fail(Status::Decode, "MAT-file header is truncated");
  const char e0 = static_cast<char>(input[126]);
  const char e1 = static_cast<char>(input[127]);
  bool big = false;
  if (e0 == 'M' && e1 == 'I') big = true;
  else if (!(e0 == 'I' && e1 == 'M')) return fail(Status::Decode, "bad MAT-file endian indicator");
  Mat5Reader mat(ctx, big);
  util::ByteReader reader(input.subspan(128));
  Json variables = Json::array();
  std::uint64_t inflated = 0;
  while (reader.remaining() >= 8) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(reader.position()) / static_cast<double>(reader.size())));
    CVT_TRY_ASSIGN(const Element e, mat.element(reader));
    Bytes buffer;
    ByteView body = e.data;
    std::uint32_t type = e.type;
    if (e.type == miCOMPRESSED) {
      CVT_TRY_ASSIGN(buffer, util::zlib_decompress(e.data, ctx.limits.max_decompressed_bytes - std::min(inflated, ctx.limits.max_decompressed_bytes)));
      inflated += buffer.size();
      CVT_TRY(ctx.limits.check_decompressed(inflated));
      util::ByteReader inner(buffer);
      CVT_TRY_ASSIGN(const Element unpacked, mat.element(inner));
      type = unpacked.type;
      body = unpacked.data;
    }
    if (type != miMATRIX) continue;
    std::string name;
    CVT_TRY_ASSIGN(Json v, mat.matrix(body, 0, &name));
    variables.push_back(std::move(v));
  }
  const std::string_view text = as_chars(input.first(116));
  Json out = Json::object({{"variables", std::move(variables)}});
  out["attributes"] = Json::object({{"format", "MAT 5.0"}, {"header", std::string(util::trim(util::sanitize_utf8(text)))}});
  return out;
}

// --------------------------------------------------------------------------- level 4

Result<Json> read_mat4(ByteView input, const Context& ctx) {
  util::ByteReader reader(input);
  Json variables = Json::array();
  while (reader.remaining() >= 20) {
    std::uint32_t mopt = reader.u32le();
    std::endian order = std::endian::little;
    if (mopt > 9999) {
      mopt = std::byteswap(mopt);
      order = std::endian::big;
    }
    if (mopt > 4052) return fail(Status::Decode, "not a level 4 MAT-file");
    const std::uint32_t m = mopt / 1000;
    const std::uint32_t p = (mopt / 10) % 10;
    const std::uint32_t t = mopt % 10;
    if (m > 1 || p > 5 || t > 2) return fail(Status::Decode, "unsupported level 4 MAT type {}", mopt);
    if (m == 1) order = std::endian::big;
    const auto rd = [&]() { return reader.read<std::uint32_t>(order); };
    const std::uint32_t rows = rd(), cols = rd(), imagf = rd(), namelen = rd();
    if (namelen == 0 || namelen > reader.remaining()) return fail(Status::Decode, "bad level 4 MAT name");
    std::string_view name = reader.chars(namelen);
    if (const std::size_t nul = name.find('\0'); nul != std::string_view::npos) name = name.substr(0, nul);
    static constexpr DType types[] = {{ScalarKind::Float, 8}, {ScalarKind::Float, 4}, {ScalarKind::Int, 4},
                                      {ScalarKind::Int, 2},   {ScalarKind::UInt, 2},  {ScalarKind::UInt, 1}};
    const DType type = types[p];
    const std::uint64_t count = std::uint64_t{rows} * cols;
    CVT_TRY(check_array_bytes(ctx, count, 16));
    const std::uint64_t bytes = count * type.size * (imagf ? 2 : 1);
    if (bytes > reader.remaining()) return fail(Status::Decode, "level 4 MAT data is truncated");
    Array real;
    real.dtype = type;
    real.shape = {rows, cols};
    const ByteView raw = reader.bytes(static_cast<std::size_t>(count * type.size));
    real.data.assign(raw.begin(), raw.end());
    if (order == std::endian::big) swap_elements(real);
    Array a = real;
    if (imagf) {
      Array imag = real;
      const ByteView im = reader.bytes(static_cast<std::size_t>(count * type.size));
      imag.data.assign(im.begin(), im.end());
      if (order == std::endian::big) swap_elements(imag);
      a.dtype = {ScalarKind::Complex, 16};
      a.data.assign(static_cast<std::size_t>(count * 16), std::byte{0});
      for (std::uint64_t i = 0; i < count; ++i) {
        store_double({ScalarKind::Float, 8}, a.data.data() + i * 16, real.number(i));
        store_double({ScalarKind::Float, 8}, a.data.data() + i * 16 + 8, imag.number(i));
      }
    }
    if (t == 1) {
      // Text: one string per row.
      Json lines = Json::array();
      for (std::uint32_t r = 0; r < rows; ++r) {
        std::u32string row;
        for (std::uint32_t c = 0; c < cols; ++c) {
          const auto ch = static_cast<char32_t>(real.number(std::uint64_t{c} * rows + r));
          row.push_back(util::is_scalar_value(ch) ? ch : U'�');
        }
        lines.push_back(util::utf32_to_utf8(row));
      }
      variables.push_back(Json::object({{"name", std::string(name)}, {"class", "char"}, {"shape", Json::array({rows, cols})},
                                        {"data", rows == 1 ? Json(lines[0]) : std::move(lines)}}));
      continue;
    }
    a.name = std::string(name);
    fortran_to_c(a);
    CVT_TRY_ASSIGN(Json json, array_to_json(a, ctx));
    if (t == 2) json["class"] = "sparse-coordinates";
    variables.push_back(std::move(json));
  }
  if (variables.empty()) return fail(Status::Decode, "no variables in the level 4 MAT-file");
  return Json::object({{"variables", std::move(variables)}, {"attributes", Json::object({{"format", "MAT 4"}})}});
}

Result<Json> read_mat_json(ByteView input, const Context& ctx) {
  if (input.size() >= 128 && as_chars(input.first(10)) == "MATLAB 7.3") {
    // MAT 7.3 is HDF5 behind a 512-byte user block.
    CVT_TRY_ASSIGN(const Collection c, read_hdf5_collection(input, ctx));
    CVT_TRY_ASSIGN(Json data, collection_to_json(c, ctx));
    data["attributes"]["format"] = "MAT 7.3 (HDF5)";
    return data;
  }
  if (input.size() >= 128 && (as_chars(input.subspan(126, 2)) == "IM" || as_chars(input.subspan(126, 2)) == "MI") &&
      (input[124] == std::byte{0x00} || input[125] == std::byte{0x00})) {
    return read_mat5(input, ctx);
  }
  return read_mat4(input, ctx);
}

// --------------------------------------------------------------------------- writing

class Mat5Writer {
 public:
  explicit Mat5Writer(const Context& ctx) : ctx_(ctx) {}

  static void tag(util::ByteWriter& out, std::uint32_t type, ByteView data) {
    if (data.size() <= 4 && type != miMATRIX && !data.empty()) {
      out.u32le(type | (static_cast<std::uint32_t>(data.size()) << 16));
      out.bytes(data);
      out.zeros(4 - data.size());
      return;
    }
    out.u32le(type).u32le(static_cast<std::uint32_t>(data.size()));
    out.bytes(data);
    out.zeros((8 - data.size() % 8) % 8);
  }

  static Bytes header_flags(std::uint8_t mx, bool complex, bool logical) {
    util::ByteWriter f(8);
    f.u32le(mx | (complex ? 0x0800u : 0u) | (logical ? 0x0200u : 0u)).u32le(0);
    return f.take();
  }

  static Bytes dims_bytes(const std::vector<std::uint64_t>& dims) {
    util::ByteWriter d(dims.size() * 4);
    for (const auto v : dims) d.i32le(static_cast<std::int32_t>(std::min<std::uint64_t>(v, 0x7FFFFFFF)));
    return d.take();
  }

  /// Body of a miMATRIX element for a value.
  Result<Bytes> matrix(const Json& value, std::string_view name, int depth) {
    if (depth > max_nesting) return fail(Status::Limit, "data is nested too deeply for a MAT-file");
    util::ByteWriter body(64);
    const std::string cls = value.is_object() ? value["class"].as_string() : std::string();
    const auto put_header = [&](std::uint8_t mx, bool complex, bool logical, const std::vector<std::uint64_t>& dims) {
      tag(body, miUINT32, header_flags(mx, complex, logical));
      tag(body, miINT32, dims_bytes(dims));
      tag(body, miINT8, as_bytes(name));
    };
    // Strings and char matrices.
    if (value.is_string() || cls == "char") {
      std::vector<std::u16string> rows;
      const Json& data = value.is_string() ? value : value["data"];
      if (data.is_string()) {
        rows.push_back(util::utf8_to_utf16(data.as_string()));
      } else {
        for (const Json& r : data.elements()) rows.push_back(util::utf8_to_utf16(r.as_string()));
      }
      std::size_t cols = 0;
      for (const auto& r : rows) cols = std::max(cols, r.size());
      put_header(mxCHAR, false, false, {rows.size(), cols});
      util::ByteWriter chars(rows.size() * cols * 2);
      for (std::size_t c = 0; c < cols; ++c) {
        for (const auto& r : rows) chars.u16le(c < r.size() ? static_cast<std::uint16_t>(r[c]) : u' ');
      }
      tag(body, miUINT16, chars.view());
      return body.take();
    }
    if (cls == "struct" || cls == "object" || (value.is_object() && !value.contains("data") && !value.contains("values") && cls.empty())) {
      const Json& fields = cls.empty() ? value : value["fields"];
      if (cls == "struct" && !value.contains("fields") && value.contains("data")) {
        return fail(Status::Encode, "struct arrays are written as 1x1 structs only");
      }
      std::vector<std::string> names;
      for (const auto& m : fields.items()) {
        std::string n;
        for (const char ch : m.key()) n.push_back(util::is_ascii_alpha(ch) || util::is_ascii_digit(ch) ? ch : '_');
        if (n.empty() || !util::is_ascii_alpha(n[0])) n = "f_" + n;
        names.push_back(n.substr(0, 63));
      }
      put_header(mxSTRUCT, false, false, {1, 1});
      std::size_t width = 1;
      for (const auto& n : names) width = std::max(width, n.size() + 1);
      util::ByteWriter len(4);
      len.i32le(static_cast<std::int32_t>(width));
      tag(body, miINT32, len.view());
      util::ByteWriter all(names.size() * width);
      for (const auto& n : names) {
        all.text(n);
        all.zeros(width - n.size());
      }
      tag(body, miINT8, all.view());
      for (const auto& m : fields.items()) {
        CVT_TRY_ASSIGN(const Bytes child, matrix(m.value(), "", depth + 1));
        tag(body, miMATRIX, child);
      }
      return body.take();
    }
    if (cls == "cell" || (value.is_array() && !looks_numeric(value))) {
      const Json& data = cls == "cell" ? value["data"] : value;
      std::vector<std::uint64_t> dims;
      for (const Json& d : value["shape"].elements()) dims.push_back(d.as_uint64());
      std::vector<const Json*> items;  // C order
      if (cls == "cell" && dims.size() >= 2) {
        std::vector<std::pair<const Json*, std::size_t>> stack = {{&data, 0}};
        while (!stack.empty()) {
          auto [node, depth_at] = stack.back();
          stack.pop_back();
          if (depth_at == dims.size()) {
            items.push_back(node);
            continue;
          }
          if (!node->is_array() || node->size() != dims[depth_at]) return fail(Status::Encode, "cell data does not match its shape");
          for (std::size_t i = node->size(); i-- > 0;) stack.emplace_back(&(*node)[i], depth_at + 1);
        }
      } else {
        for (const Json& v : data.elements()) items.push_back(&v);
        dims = {1, items.size()};
      }
      // MAT stores cells column-major.
      std::vector<const Json*> ordered(items.size());
      {
        std::vector<std::uint64_t> index(dims.size(), 0);
        for (std::size_t i = 0; i < items.size(); ++i) {
          std::uint64_t f = 0;
          for (std::size_t d = dims.size(); d-- > 0;) f = f * dims[d] + index[d];
          ordered[static_cast<std::size_t>(f)] = items[i];
          for (std::size_t d = dims.size(); d-- > 0;) {
            if (++index[d] < dims[d]) break;
            index[d] = 0;
          }
        }
      }
      put_header(mxCELL, false, false, dims);
      for (const Json* v : ordered) {
        CVT_TRY_ASSIGN(const Bytes child, matrix(*v, "", depth + 1));
        tag(body, miMATRIX, child);
      }
      return body.take();
    }
    if (cls == "sparse") {
      const Json& shape = value["shape"];
      const std::uint64_t rows = shape[0].as_uint64(), cols = shape[1].as_uint64();
      const Json& r = value["row"];
      const Json& c = value["col"];
      const Json& v = value["value"];
      if (r.size() != c.size() || r.size() != v.size() || cols > 0x7FFFFFFF || rows > 0x7FFFFFFF) {
        return fail(Status::Encode, "malformed sparse matrix");
      }
      std::vector<std::vector<std::pair<std::int32_t, double>>> by_column(static_cast<std::size_t>(cols));
      bool logical = v.size() > 0;
      for (std::size_t k = 0; k < r.size(); ++k) {
        const std::uint64_t col = c[k].as_uint64();
        if (col >= cols || r[k].as_uint64() >= rows) return fail(Status::Encode, "sparse index out of range");
        logical = logical && v[k].is_bool();
        by_column[static_cast<std::size_t>(col)].emplace_back(static_cast<std::int32_t>(r[k].as_int64()),
                                                              v[k].is_bool() ? (v[k].as_bool() ? 1.0 : 0.0) : v[k].as_double());
      }
      put_header(mxSPARSE, false, logical, {rows, cols});
      util::ByteWriter ir(r.size() * 4), jc((cols + 1) * 4), pr(r.size() * 8);
      std::int32_t at = 0;
      for (auto& column : by_column) {
        std::sort(column.begin(), column.end());
        jc.i32le(at);
        for (const auto& [row, x] : column) {
          ir.i32le(row);
          pr.f64le(x);
          ++at;
        }
      }
      jc.i32le(at);
      tag(body, miINT32, ir.view());
      tag(body, miINT32, jc.view());
      tag(body, miDOUBLE, pr.view());
      return body.take();
    }
    // Numeric / logical arrays (and plain numbers / booleans).
    Array a;
    if (value.is_number() || value.is_bool()) {
      CVT_TRY_ASSIGN(a, json_to_array(Json::array({value}), ctx_));
      a.shape = {};
    } else {
      CVT_TRY_ASSIGN(a, json_to_array(value, ctx_));
    }
    if (a.dtype.kind == ScalarKind::Unicode || a.dtype.kind == ScalarKind::Bytes) {
      Json rows = Json::array();
      for (std::uint64_t i = 0; i < a.count(); ++i) rows.push_back(scalar_json(a.dtype, a.element(i)));
      return matrix(Json::object({{"class", "char"}, {"data", std::move(rows)}}), name, depth);
    }
    if (a.dtype.kind == ScalarKind::Record) return fail(Status::Encode, "record arrays cannot be written to a MAT-file");
    std::vector<std::uint64_t> dims = a.shape;
    if (dims.empty()) dims = {1, 1};
    if (dims.size() == 1) dims = {1, dims[0]};
    a.shape = dims;
    c_to_fortran(a);
    const bool complex = a.dtype.kind == ScalarKind::Complex;
    const bool logical = a.dtype.kind == ScalarKind::Bool;
    const std::uint8_t mx = dtype_class(a.dtype);
    put_header(mx, complex, logical, dims);
    const std::uint64_t n = a.count();
    if (complex) {
      const std::uint32_t part = a.dtype.size / 2;
      Bytes re(static_cast<std::size_t>(n * part)), im(static_cast<std::size_t>(n * part));
      for (std::uint64_t i = 0; i < n; ++i) {
        std::memcpy(re.data() + i * part, a.element(i), part);
        std::memcpy(im.data() + i * part, a.element(i) + part, part);
      }
      tag(body, class_mi(mx), re);
      tag(body, class_mi(mx), im);
    } else {
      tag(body, class_mi(mx), a.data);
    }
    return body.take();
  }

  static bool looks_numeric(const Json& value) {
    std::vector<const Json*> stack = {&value};
    while (!stack.empty()) {
      const Json* v = stack.back();
      stack.pop_back();
      if (v->is_array()) {
        for (const Json& e : v->elements()) stack.push_back(&e);
      } else if (!v->is_number() && !v->is_bool()) {
        return false;
      }
    }
    return true;
  }

 private:
  const Context& ctx_;
};

Result<Bytes> write_mat(const Json& data, WriteContext& ctx) {
  std::vector<std::pair<std::string, const Json*>> variables;
  if (const Json* vars = data.find("variables"); vars != nullptr && vars->is_array()) {
    std::size_t i = 0;
    for (const Json& v : vars->elements()) {
      std::string name = v["name"].as_string();
      if (name.empty()) name = "var" + std::to_string(++i);
      variables.emplace_back(name, &v);
    }
  } else if (data.is_object() && (data.contains("data") || data.contains("values"))) {
    variables.emplace_back(data["name"].as_string().empty() ? "data" : data["name"].as_string(), &data);
  } else if (data.is_object()) {
    for (const auto& m : data.items()) variables.emplace_back(m.key(), &m.value());
  } else {
    variables.emplace_back("data", &data);
  }
  const int level = options::resolved_compression_level(ctx.options, 9);
  util::ByteWriter out(1024);
  std::string text = "MATLAB 5.0 MAT-file";
  text.resize(116, ' ');
  out.text(text).zeros(8).u16le(0x0100).text("IM");
  Mat5Writer writer(ctx);
  for (auto& [raw_name, value] : variables) {
    std::string name;
    for (const char ch : raw_name) name.push_back(util::is_ascii_alpha(ch) || util::is_ascii_digit(ch) ? ch : '_');
    if (name.empty() || !util::is_ascii_alpha(name[0])) name = "v_" + name;
    name = name.substr(0, 63);
    CVT_TRY_ASSIGN(const Bytes body, writer.matrix(*value, name, 0));
    util::ByteWriter element(body.size() + 16);
    Mat5Writer::tag(element, miMATRIX, body);
    if (level > 0) {
      CVT_TRY_ASSIGN(const Bytes packed, util::zlib_compress(element.view(), level));
      out.u32le(miCOMPRESSED).u32le(static_cast<std::uint32_t>(packed.size())).bytes(packed);
    } else {
      out.bytes(element.view());
    }
  }
  return out.take();
}

int sniff_mat(ByteView data, std::string_view name) {
  const std::string_view head = head_text(data, 128);
  if (head.starts_with("MATLAB 5.0 MAT-file") || head.starts_with("MATLAB 7.3 MAT-file")) return 96;
  if (has_extension(name, "mat") && data.size() >= 20) {
    util::ByteReader reader(data);
    std::uint32_t mopt = reader.u32le();
    if (mopt > 9999) mopt = std::byteswap(mopt);
    if (mopt <= 1052 && (mopt % 10) <= 2) return 70;  // plausible level 4
  }
  return 0;
}

/// Flatten MAT JSON variables into array sheets (struct fields as "a.b").
Result<void> mat_sheets(const Json& value, const std::string& name, ir::Table& table, const Context& ctx, int depth) {
  if (depth > max_nesting) return {};
  const std::string cls = value["class"].as_string();
  if (cls == "struct" || cls == "object") {
    for (const auto& [k, v] : value["fields"].items()) CVT_TRY(mat_sheets(v, name + "." + k, table, ctx, depth + 1));
    return {};
  }
  if (cls == "char") {
    ir::Sheet sheet;
    sheet.name = name;
    if (value["data"].is_string()) sheet.rows.push_back({text_cell(value["data"].as_string())});
    for (const Json& r : value["data"].elements()) sheet.rows.push_back({text_cell(r.as_string())});
    table.sheets.push_back(std::move(sheet));
    return {};
  }
  if (cls == "cell" || cls == "sparse" || cls == "unsupported" || cls == "empty") {
    ctx.warn("MAT " + cls + " variable '" + name + "' has no table form; use JSON output to keep it.");
    return {};
  }
  CVT_TRY_ASSIGN(const Array a, json_to_array(value, ctx));
  CVT_TRY_ASSIGN(ir::Sheet sheet, array_to_sheet(a, ctx));
  sheet.name = name;
  table.sheets.push_back(std::move(sheet));
  return {};
}

}  // namespace
}  // namespace cvt::science::arrays

namespace cvt::science {

void register_mat(Registry& registry) {
  using namespace arrays;
  add_format(registry, {.id = "mat",
                        .name = "MATLAB MAT-file",
                        .description = "MATLAB workspace variables (level 5 / v7, level 4).",
                        .extensions = {"mat"},
                        .mime = "application/x-matlab-data",
                        .popularity = 30,
                        .sniff = sniff_mat});
  const std::vector<OptionSpec> write_options = {options::compression_level(9), options::preset()};
  add_codec(
      registry, "mat", Kind::Data,
      [](ReadContext& ctx) -> Result<AnyIR> {
        CVT_TRY_ASSIGN(Json data, read_mat_json(ctx.input, ctx));
        return ir::Data{std::move(data)};
      },
      [](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> { return write_mat(value.get<ir::Data>().value, ctx); },
      {}, write_options, 10, 10);
  add_codec(
      registry, "mat", Kind::Table,
      [](ReadContext& ctx) -> Result<AnyIR> {
        CVT_TRY_ASSIGN(const Json data, read_mat_json(ctx.input, ctx));
        ir::Table table;
        for (const Json& v : data["variables"].elements()) CVT_TRY(mat_sheets(v, v["name"].as_string(), table, ctx, 0));
        if (table.sheets.empty()) return fail(Status::Decode, "the MAT-file has no variables with a table form");
        return table;
      },
      [](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
        CVT_TRY_ASSIGN(const Collection c, table_to_collection(value.get<ir::Table>()));
        CVT_TRY_ASSIGN(Json data, collection_to_json(c, ctx));
        return write_mat(data, ctx);
      },
      {}, write_options, 12, 12, true);
}

}  // namespace cvt::science
