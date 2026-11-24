// Convertible core - science family: NetCDF classic files (CDF-1 classic,
// CDF-2 64-bit offset, CDF-5 64-bit data), read and write, including record
// (unlimited-dimension) variables and attributes.
//
// Arrays carry "_dimensions" (names) in their attributes; the collection's
// attributes hold the global attributes plus "_dimensions" {name: length}.
// Character variables become string arrays over their last dimension.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <map>

#include "util/bytes.hpp"
#include "families/science/arrays.hpp"
#include "families/science/science.hpp"
#include "util/text.hpp"

namespace cvt::science::arrays {

namespace {

enum NcType : std::uint32_t {
  NC_BYTE = 1, NC_CHAR = 2, NC_SHORT = 3, NC_INT = 4, NC_FLOAT = 5, NC_DOUBLE = 6,
  NC_UBYTE = 7, NC_USHORT = 8, NC_UINT = 9, NC_INT64 = 10, NC_UINT64 = 11,
};

std::optional<DType> nc_dtype(std::uint32_t type) {
  switch (type) {
    case NC_BYTE: return DType{ScalarKind::Int, 1};
    case NC_CHAR: return DType{ScalarKind::Bytes, 1};
    case NC_SHORT: return DType{ScalarKind::Int, 2};
    case NC_INT: return DType{ScalarKind::Int, 4};
    case NC_FLOAT: return DType{ScalarKind::Float, 4};
    case NC_DOUBLE: return DType{ScalarKind::Float, 8};
    case NC_UBYTE: return DType{ScalarKind::UInt, 1};
    case NC_USHORT: return DType{ScalarKind::UInt, 2};
    case NC_UINT: return DType{ScalarKind::UInt, 4};
    case NC_INT64: return DType{ScalarKind::Int, 8};
    case NC_UINT64: return DType{ScalarKind::UInt, 8};
    default: return std::nullopt;
  }
}

struct NcDim {
  std::string name;
  std::uint64_t length = 0;
};

struct NcVar {
  std::string name;
  std::vector<std::uint32_t> dims;
  Json attributes = Json::object();
  std::uint32_t type = 0;
  std::uint64_t vsize = 0;
  std::uint64_t begin = 0;
};

class NcReader {
 public:
  NcReader(ByteView data, int version) : reader_(data), version_(version) {}

  std::uint64_t count() { return version_ == 5 ? reader_.u64be() : reader_.u32be(); }
  std::uint64_t offset() { return version_ == 1 ? reader_.u32be() : reader_.u64be(); }

  Result<std::string> name() {
    const std::uint64_t n = count();
    if (n > reader_.remaining() || n > 65536) return fail(Status::Decode, "NetCDF name is truncated");
    std::string out = util::sanitize_utf8(reader_.chars(static_cast<std::size_t>(n)));
    skip_padding(n);
    return out;
  }

  void skip_padding(std::uint64_t n) {
    const std::size_t pad = static_cast<std::size_t>((4 - n % 4) % 4);
    (void)reader_.bytes(std::min(pad, reader_.remaining()));
  }

  Result<Json> attributes(const Context& ctx) {
    Json out = Json::object();
    const std::uint32_t tag = reader_.u32be();
    const std::uint64_t n = count();
    if (tag == 0 && n == 0) return out;
    if (tag != 0x0C) return fail(Status::Decode, "bad NetCDF attribute list");
    if (n > reader_.remaining()) return fail(Status::Decode, "NetCDF attribute list is truncated");
    for (std::uint64_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(std::string key, name());
      const std::uint32_t type = reader_.u32be();
      const std::uint64_t values = count();
      const auto dtype = nc_dtype(type);
      if (!dtype) return fail(Status::Decode, "unknown NetCDF attribute type {}", type);
      if (values > reader_.remaining() / dtype->size) return fail(Status::Decode, "NetCDF attribute is truncated");
      const ByteView raw = reader_.bytes(static_cast<std::size_t>(values * dtype->size));
      skip_padding(values * dtype->size);
      if (type == NC_CHAR) {
        std::string_view text = as_chars(raw);
        while (!text.empty() && text.back() == '\0') text.remove_suffix(1);
        out[key] = util::sanitize_utf8(text);
        continue;
      }
      Array a;
      a.dtype = *dtype;
      a.data.assign(raw.begin(), raw.end());
      a.shape = {values};
      swap_elements(a);
      if (values == 1) {
        out[key] = scalar_json(a.dtype, a.element(0));
      } else {
        Json list = Json::array();
        for (std::uint64_t k = 0; k < values; ++k) list.push_back(scalar_json(a.dtype, a.element(k)));
        out[key] = std::move(list);
      }
      CVT_TRY(check_cells(ctx, values));
    }
    return out;
  }

  util::ByteReader& raw() { return reader_; }

 private:
  util::ByteReader reader_;
  int version_;
};

Result<Collection> read_netcdf(ReadContext& ctx) {
  const ByteView input = ctx.input;
  if (input.size() < 8 || as_chars(input.first(3)) != "CDF") return fail(Status::Decode, "not a NetCDF classic file");
  const int version = static_cast<int>(input[3]);
  if (version != 1 && version != 2 && version != 5) return fail(Status::Decode, "unsupported NetCDF version {}", version);
  NcReader nc(input.subspan(4), version);
  std::uint64_t numrecs = nc.count();
  const bool streaming = (version == 5 ? numrecs == ~std::uint64_t{0} : numrecs == 0xFFFFFFFFu);
  // Dimensions.
  std::vector<NcDim> dims;
  {
    const std::uint32_t tag = nc.raw().u32be();
    const std::uint64_t n = nc.count();
    if (!(tag == 0 && n == 0)) {
      if (tag != 0x0A || n > nc.raw().remaining()) return fail(Status::Decode, "bad NetCDF dimension list");
      for (std::uint64_t i = 0; i < n; ++i) {
        CVT_TRY_ASSIGN(std::string name, nc.name());
        dims.push_back({std::move(name), nc.count()});
      }
    }
  }
  CVT_TRY_ASSIGN(Json globals, nc.attributes(ctx));
  std::vector<NcVar> vars;
  {
    const std::uint32_t tag = nc.raw().u32be();
    const std::uint64_t n = nc.count();
    if (!(tag == 0 && n == 0)) {
      if (tag != 0x0B || n > nc.raw().remaining()) return fail(Status::Decode, "bad NetCDF variable list");
      for (std::uint64_t i = 0; i < n; ++i) {
        NcVar v;
        CVT_TRY_ASSIGN(v.name, nc.name());
        const std::uint64_t rank = nc.count();
        if (rank > 64) return fail(Status::Decode, "NetCDF variable rank {} is too high", rank);
        for (std::uint64_t d = 0; d < rank; ++d) {
          const std::uint64_t id = nc.count();
          if (id >= dims.size()) return fail(Status::Decode, "NetCDF variable refers to a missing dimension");
          v.dims.push_back(static_cast<std::uint32_t>(id));
        }
        CVT_TRY_ASSIGN(v.attributes, nc.attributes(ctx));
        v.type = nc.raw().u32be();
        v.vsize = nc.count();
        v.begin = nc.offset();
        if (!nc.raw().ok()) return fail(Status::Decode, "NetCDF header is truncated");
        vars.push_back(std::move(v));
      }
    }
  }
  if (!nc.raw().ok()) return fail(Status::Decode, "NetCDF header is truncated");
  const auto is_record = [&](const NcVar& v) { return !v.dims.empty() && dims[v.dims[0]].length == 0; };
  std::uint64_t record_size = 0;
  std::size_t record_vars = 0;
  for (const NcVar& v : vars) {
    if (!is_record(v)) continue;
    ++record_vars;
    record_size += v.vsize;
  }
  if (streaming) {
    // Infer the record count from the file size.
    std::uint64_t first = ~std::uint64_t{0};
    for (const NcVar& v : vars) {
      if (is_record(v)) first = std::min(first, v.begin);
    }
    numrecs = record_size == 0 || first >= input.size() ? 0 : (input.size() - first) / record_size;
  }
  Collection c;
  Json dimension_table = Json::object();
  for (const NcDim& d : dims) dimension_table[d.name] = d.length == 0 ? numrecs : d.length;
  c.attributes = std::move(globals);
  c.attributes["_dimensions"] = std::move(dimension_table);
  for (std::size_t vi = 0; vi < vars.size(); ++vi) {
    const NcVar& v = vars[vi];
    CVT_TRY(ctx.checkpoint(static_cast<double>(vi) / static_cast<double>(vars.size())));
    const auto dtype = nc_dtype(v.type);
    if (!dtype) return fail(Status::Decode, "unknown NetCDF variable type {}", v.type);
    Array a;
    a.name = v.name;
    a.dtype = *dtype;
    Json dim_names = Json::array();
    for (const auto id : v.dims) {
      a.shape.push_back(dims[id].length == 0 ? numrecs : dims[id].length);
      dim_names.push_back(dims[id].name);
    }
    CVT_TRY_ASSIGN(const std::uint64_t n, shape_count(a.shape));
    CVT_TRY(check_array_bytes(ctx, n, a.dtype.size));
    a.data.assign(static_cast<std::size_t>(n * a.dtype.size), std::byte{0});
    if (is_record(v)) {
      const std::uint64_t per_record = numrecs == 0 ? 0 : n / numrecs * a.dtype.size;
      const std::uint64_t stride = record_vars == 1 ? per_record : record_size;
      for (std::uint64_t r = 0; r < numrecs; ++r) {
        const std::uint64_t at = v.begin + r * stride;
        if (at > input.size() || per_record > input.size() - at) return fail(Status::Decode, "NetCDF record data is truncated");
        ::cvt::util::copy_bytes(a.data.data() + r * per_record, input.data() + at, static_cast<std::size_t>(per_record));
      }
    } else {
      const std::uint64_t bytes = n * a.dtype.size;
      if (v.begin > input.size() || bytes > input.size() - v.begin) return fail(Status::Decode, "NetCDF variable '{}' is truncated", v.name);
      ::cvt::util::copy_bytes(a.data.data(), input.data() + v.begin, static_cast<std::size_t>(bytes));
    }
    swap_elements(a);
    if (a.dtype.kind == ScalarKind::Bytes && !a.shape.empty()) {
      // Characters along the last dimension form strings.
      const std::uint64_t len = a.shape.back();
      a.shape.pop_back();
      a.dtype = DType{ScalarKind::Bytes, static_cast<std::uint32_t>(std::max<std::uint64_t>(len, 1))};
      if (len == 0) a.data.assign(static_cast<std::size_t>(a.count()), std::byte{0});
    }
    a.attributes = v.attributes;
    a.attributes["_dimensions"] = std::move(dim_names);
    c.arrays.push_back(std::move(a));
  }
  return c;
}

// --------------------------------------------------------------------------- writing

class NcWriter {
 public:
  explicit NcWriter(int version) : version_(version) {}
  void count(std::uint64_t n) {
    if (version_ == 5) out.u64be(n);
    else out.u32be(static_cast<std::uint32_t>(n));
  }
  void name(std::string_view s) {
    count(s.size());
    out.text(s);
    out.zeros((4 - s.size() % 4) % 4);
  }
  void pad() { out.zeros((4 - out.size() % 4) % 4); }
  util::ByteWriter out{1024};
  int version_;
};

std::uint32_t nc_type_for(DType t, int version) {
  switch (t.kind) {
    case ScalarKind::Bool: return NC_BYTE;
    case ScalarKind::Int: return t.size == 1 ? NC_BYTE : t.size == 2 ? NC_SHORT : t.size == 4 ? NC_INT : (version == 5 ? NC_INT64 : NC_DOUBLE);
    case ScalarKind::UInt:
      if (version == 5) return t.size == 1 ? NC_UBYTE : t.size == 2 ? NC_USHORT : t.size == 4 ? NC_UINT : NC_UINT64;
      return t.size == 1 ? NC_SHORT : t.size == 2 ? NC_INT : NC_DOUBLE;
    case ScalarKind::Float: return t.size == 8 ? NC_DOUBLE : NC_FLOAT;
    case ScalarKind::Bytes:
    case ScalarKind::Unicode: return NC_CHAR;
    default: return 0;
  }
}

bool needs_cdf5(DType t) {
  return (t.kind == ScalarKind::UInt) || (t.kind == ScalarKind::Int && t.size == 8);
}

Bytes encode_values(const Array& a, std::uint32_t type) {
  const DType target = *nc_dtype(type);
  const std::uint64_t n = a.count();
  util::ByteWriter out(static_cast<std::size_t>(n * target.size));
  Array tmp;
  tmp.dtype = target;
  tmp.shape = {1};
  tmp.data.resize(target.size);
  for (std::uint64_t i = 0; i < n; ++i) {
    if (target == a.dtype) {
      ::cvt::util::copy_bytes(tmp.data.data(), a.element(i), target.size);
    } else {
      store_double(target, tmp.data.data(), a.number(i));
    }
    swap_elements(tmp);
    out.bytes(tmp.data);
  }
  return out.take();
}

struct PlannedVar {
  const Array* array;
  std::vector<std::uint32_t> dims;
  std::uint32_t type;
  Bytes payload;
  Json attributes;
};

Result<Bytes> write_netcdf(const Collection& collection, WriteContext& ctx) {
  const std::string option = ctx.options.get_string("netcdf_format", "auto");
  int version = option == "cdf5" ? 5 : option == "64bit-offset" ? 2 : 1;
  if (option == "auto") {
    for (const Array& a : collection.arrays) {
      if (needs_cdf5(a.dtype)) version = 5;
    }
  } else if (version != 5) {
    for (const Array& a : collection.arrays) {
      if (needs_cdf5(a.dtype)) ctx.warn("'" + a.name + "' was widened: classic NetCDF has no unsigned or 64-bit integers.");
    }
  }
  std::vector<NcDim> dims;
  const auto dim_id = [&dims](const std::string& name, std::uint64_t length) -> std::uint32_t {
    for (std::size_t i = 0; i < dims.size(); ++i) {
      if (dims[i].name == name && dims[i].length == length) return static_cast<std::uint32_t>(i);
    }
    std::string unique = name;
    for (int k = 2; std::any_of(dims.begin(), dims.end(), [&](const NcDim& d) { return d.name == unique; }); ++k) {
      unique = name + "_" + std::to_string(k);
    }
    dims.push_back({unique, length});
    return static_cast<std::uint32_t>(dims.size() - 1);
  };
  std::vector<PlannedVar> planned;
  for (const Array& a : collection.arrays) {
    if (a.dtype.kind == ScalarKind::Record || a.dtype.kind == ScalarKind::Complex) {
      ctx.warn("'" + a.name + "' (" + dtype_name(a.dtype) + ") cannot be stored in NetCDF classic and was skipped.");
      continue;
    }
    PlannedVar v{&a, {}, nc_type_for(a.dtype, version), {}, a.attributes};
    v.attributes.erase("_dimensions");
    const Json& names = a.attributes["_dimensions"];
    std::vector<std::uint64_t> shape = a.shape;
    std::uint32_t chars = 0;
    if (v.type == NC_CHAR) {
      std::uint32_t longest = 1;
      for (std::uint64_t i = 0; i < a.count(); ++i) {
        longest = std::max<std::uint32_t>(longest, static_cast<std::uint32_t>(scalar_json(a.dtype, a.element(i)).as_string().size()));
      }
      chars = longest;
    }
    for (std::size_t d = 0; d < shape.size(); ++d) {
      const std::string name = d < names.size() && names[d].is_string() ? names[d].as_string() : a.name + "_dim" + std::to_string(d);
      v.dims.push_back(dim_id(name, shape[d]));
      if (shape[d] == 0) return fail(Status::Encode, "zero-length dimensions cannot be written (they mean 'unlimited')");
    }
    if (v.type == NC_CHAR) {
      const std::string name = names.size() > shape.size() && names[shape.size()].is_string() ? names[shape.size()].as_string()
                                                                                           : a.name + "_strlen";
      v.dims.push_back(dim_id(name, chars));
      // Pad strings to the common length.
      const std::uint64_t n = a.count();
      util::ByteWriter text(static_cast<std::size_t>(n * chars));
      for (std::uint64_t i = 0; i < n; ++i) {
        std::string s = scalar_json(a.dtype, a.element(i)).as_string();
        s.resize(chars, '\0');
        text.text(s);
      }
      v.payload = text.take();
    } else {
      v.payload = encode_values(a, v.type);
    }
    planned.push_back(std::move(v));
  }
  if (planned.empty()) return fail(Status::Encode, "no variable can be stored in NetCDF");
  const auto write_attributes = [&](NcWriter& w, const Json& attrs) -> Result<void> {
    std::vector<std::pair<std::string, const Json*>> list;
    for (const auto& m : attrs.items()) {
      if (m.key().starts_with('_') && m.key() != "_FillValue") continue;
      list.emplace_back(m.key(), &m.value());
    }
    if (list.empty()) {
      w.out.u32be(0);
      w.count(0);
      return {};
    }
    w.out.u32be(0x0C);
    w.count(list.size());
    for (const auto& [key, value] : list) {
      w.name(key);
      if (value->is_string() || value->is_object() || value->is_null() ||
          (value->is_array() && std::any_of(value->elements().begin(), value->elements().end(), [](const Json& e) { return !e.is_number(); }))) {
        const std::string text = value->is_string() ? value->as_string() : util::dump_json(*value);
        w.out.u32be(NC_CHAR);
        w.count(text.size());
        w.out.text(text);
        w.pad();
        continue;
      }
      std::vector<const Json*> numbers;
      if (value->is_array()) {
        for (const Json& e : value->elements()) numbers.push_back(&e);
      } else {
        numbers.push_back(value);
      }
      bool ints = true;
      for (const Json* n : numbers) ints = ints && n->is_integer() && n->as_int64() >= std::numeric_limits<std::int32_t>::min() && n->as_int64() <= std::numeric_limits<std::int32_t>::max();
      w.out.u32be(ints ? NC_INT : NC_DOUBLE);
      w.count(numbers.size());
      for (const Json* n : numbers) {
        if (ints) w.out.i32be(static_cast<std::int32_t>(n->as_int64()));
        else w.out.f64be(n->as_double());
      }
    }
    return {};
  };
  // Header (twice: first to learn its size, then with the final offsets).
  const auto header = [&](std::uint64_t data_start) -> Result<Bytes> {
    NcWriter w(version);
    w.out.text("CDF").u8(static_cast<std::uint8_t>(version));
    w.count(0);  // no records
    w.out.u32be(0x0A);
    w.count(dims.size());
    for (const NcDim& d : dims) {
      w.name(d.name);
      w.count(d.length);
    }
    Json globals = collection.attributes;
    globals.erase("_dimensions");
    CVT_TRY(write_attributes(w, globals));
    w.out.u32be(0x0B);
    w.count(planned.size());
    std::uint64_t at = data_start;
    for (const PlannedVar& v : planned) {
      std::string var_name = v.array->name;
      while (var_name.starts_with('/')) var_name.erase(0, 1);
      std::replace(var_name.begin(), var_name.end(), '/', '_');
      w.name(var_name.empty() ? "var" : var_name);
      w.count(v.dims.size());
      for (const auto id : v.dims) w.count(id);
      CVT_TRY(write_attributes(w, v.attributes));
      w.out.u32be(v.type);
      const std::uint64_t padded = (v.payload.size() + 3) / 4 * 4;
      w.count(std::min<std::uint64_t>(padded, version == 5 ? padded : 0xFFFFFFFFu));
      if (version == 1) {
        if (at > 0x7FFFFFFF) return fail(Status::Encode, "too large for classic NetCDF; choose the 64bit-offset format");
        w.out.u32be(static_cast<std::uint32_t>(at));
      } else {
        w.out.u64be(at);
      }
      at += padded;
    }
    return w.out.take();
  };
  CVT_TRY_ASSIGN(const Bytes probe, header(0));
  CVT_TRY_ASSIGN(Bytes out, header(probe.size()));
  for (const PlannedVar& v : planned) {
    append(out, ByteView(v.payload));
    out.resize((out.size() + 3) / 4 * 4, std::byte{0});
  }
  return out;
}

}  // namespace
}  // namespace cvt::science::arrays

namespace cvt::science {

void register_netcdf(Registry& registry) {
  using namespace arrays;
  add_format(registry, {.id = "nc",
                        .name = "NetCDF classic",
                        .description = "Network Common Data Form, classic / 64-bit offset / CDF-5 encoding.",
                        .extensions = {"nc", "cdf", "netcdf"},
                        .mime = "application/x-netcdf",
                        .popularity = 28,
                        .signatures = {Signature::at(0, "'CDF' 01"), Signature::at(0, "'CDF' 02"), Signature::at(0, "'CDF' 05")}});
  add_collection_codecs(registry, "nc", read_netcdf, write_netcdf, {},
                        {OptionSpec::choice("netcdf_format", "NetCDF format", "auto", {"auto", "classic", "64bit-offset", "cdf5"},
                                            "Classic is the most compatible; CDF-5 adds unsigned and 64-bit integers; auto picks the "
                                            "oldest that holds the data.")});
}

}  // namespace cvt::science
