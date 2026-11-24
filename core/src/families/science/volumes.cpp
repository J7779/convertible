// Convertible core - science family: medical/scientific volumes. NRRD,
// MetaImage (.mha; .mhd headers), NIfTI-1 (.nii, .nii.gz) and the Analyze 7.5
// header. Each volume is read as one array (C order [t][z][y][x]) with its
// header as attributes, rendered as one picture per slice, and written from
// pictures (slices) or arrays.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "engine/standard_options.hpp"
#include "families/science/arrays.hpp"
#include "families/science/imaging.hpp"
#include "families/science/science.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::science::arrays {

namespace {

/// Raw element bytes -> Array (little-endian result).
Result<Array> raw_array(ByteView raw, DType type, std::vector<std::uint64_t> shape, bool big_endian, const Context& ctx) {
  Array a;
  a.dtype = type;
  a.shape = std::move(shape);
  CVT_TRY_ASSIGN(const std::uint64_t n, shape_count(a.shape));
  CVT_TRY(check_array_bytes(ctx, n, type.size));
  if (raw.size() < n * type.size) return fail(Status::Decode, "volume data is truncated ({} of {} bytes)", raw.size(), n * type.size);
  a.data.assign(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(n * type.size));
  if (big_endian) swap_elements(a);
  return a;
}

Result<Bytes> inflate_any(ByteView data, const Context& ctx) {
  if (data.size() >= 2 && data[0] == std::byte{0x1F} && data[1] == std::byte{0x8B}) {
    return util::gzip_decompress(data, ctx.limits.max_decompressed_bytes);
  }
  return util::zlib_decompress(data, ctx.limits.max_decompressed_bytes);
}

// --------------------------------------------------------------------------- NRRD

std::optional<DType> nrrd_type(std::string_view name) {
  const std::string t = util::to_lower(util::trim(name));
  if (t == "signed char" || t == "int8" || t == "int8_t") return DType{ScalarKind::Int, 1};
  if (t == "uchar" || t == "unsigned char" || t == "uint8" || t == "uint8_t") return DType{ScalarKind::UInt, 1};
  if (t == "short" || t == "short int" || t == "signed short" || t == "signed short int" || t == "int16" || t == "int16_t") return DType{ScalarKind::Int, 2};
  if (t == "ushort" || t == "unsigned short" || t == "unsigned short int" || t == "uint16" || t == "uint16_t") return DType{ScalarKind::UInt, 2};
  if (t == "int" || t == "signed int" || t == "int32" || t == "int32_t") return DType{ScalarKind::Int, 4};
  if (t == "uint" || t == "unsigned int" || t == "uint32" || t == "uint32_t") return DType{ScalarKind::UInt, 4};
  if (t == "longlong" || t == "long long" || t == "long long int" || t == "signed long long" || t == "signed long long int" || t == "int64" || t == "int64_t") return DType{ScalarKind::Int, 8};
  if (t == "ulonglong" || t == "unsigned long long" || t == "unsigned long long int" || t == "uint64" || t == "uint64_t") return DType{ScalarKind::UInt, 8};
  if (t == "float") return DType{ScalarKind::Float, 4};
  if (t == "double") return DType{ScalarKind::Float, 8};
  return std::nullopt;
}

std::string nrrd_type_name(DType t) {
  switch (t.kind) {
    case ScalarKind::Int: return t.size == 1 ? "int8" : t.size == 2 ? "int16" : t.size == 4 ? "int32" : "int64";
    case ScalarKind::UInt:
    case ScalarKind::Bool: return t.size == 1 ? "uint8" : t.size == 2 ? "uint16" : t.size == 4 ? "uint32" : "uint64";
    case ScalarKind::Float: return t.size == 4 ? "float" : "double";
    default: return "";
  }
}

Result<Collection> read_nrrd(ReadContext& ctx) {
  const ByteView input = ctx.input;
  if (!head_text(input, 8).starts_with("NRRD000")) return fail(Status::Decode, "not a NRRD file");
  std::size_t pos = 0;
  Json header = Json::object();
  std::size_t data_start = 0;
  while (pos < input.size()) {
    std::size_t end = pos;
    while (end < input.size() && input[end] != std::byte{'\n'}) ++end;
    std::string_view line = as_chars(input.subspan(pos, end - pos));
    if (line.ends_with('\r')) line.remove_suffix(1);
    pos = end + 1;
    if (line.empty()) {
      data_start = std::min(pos, input.size());
      break;
    }
    if (line.starts_with('#') || line.starts_with("NRRD")) continue;
    if (const std::size_t kv = line.find(":="); kv != std::string_view::npos) {
      header["kv:" + util::sanitize_utf8(line.substr(0, kv))] = util::sanitize_utf8(line.substr(kv + 2));
    } else if (const std::size_t colon = line.find(": "); colon != std::string_view::npos) {
      header[util::to_lower(line.substr(0, colon))] = util::sanitize_utf8(util::trim(line.substr(colon + 2)));
    }
    if (header.size() > 4096) return fail(Status::Limit, "NRRD header is too long");
  }
  if (data_start == 0) return fail(Status::Decode, "NRRD header does not end with a blank line");
  if (header.contains("data file") || header.contains("datafile")) {
    return fail(Status::Decode, "detached NRRD headers need their data file; convert the .nrrd with attached data");
  }
  const auto type = nrrd_type(header["type"].as_string());
  if (!type) return fail(Status::Decode, "unsupported NRRD type '{}'", header["type"].as_string());
  const auto dims = words(header["sizes"].as_string());
  std::vector<std::uint64_t> shape;
  for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
    const auto n = to_int(*it);
    if (!n || *n <= 0) return fail(Status::Decode, "bad NRRD sizes");
    shape.push_back(static_cast<std::uint64_t>(*n));
  }
  if (shape.empty()) return fail(Status::Decode, "NRRD has no sizes");
  const std::string encoding = util::to_lower(header["encoding"].as_string());
  const bool big = util::to_lower(header["endian"].as_string()) == "big";
  std::uint64_t skip_lines = static_cast<std::uint64_t>(std::max<std::int64_t>(0, to_int(header["line skip"].as_string()).value_or(0)));
  std::uint64_t skip_bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, to_int(header["byte skip"].as_string()).value_or(0)));
  ByteView payload = input.subspan(data_start);
  for (; skip_lines > 0 && !payload.empty(); --skip_lines) {
    std::size_t nl = 0;
    while (nl < payload.size() && payload[nl] != std::byte{'\n'}) ++nl;
    payload = payload.subspan(std::min(nl + 1, payload.size()));
  }
  Bytes inflated;
  if (encoding == "gzip" || encoding == "gz") {
    CVT_TRY_ASSIGN(inflated, inflate_any(payload, ctx));
    payload = inflated;
  } else if (encoding == "bzip2" || encoding == "bz2") {
    return fail(Status::Decode, "bzip2-encoded NRRD data is not supported");
  }
  Array a;
  if (encoding == "ascii" || encoding == "text" || encoding == "txt") {
    CVT_TRY_ASSIGN(const std::uint64_t n, shape_count(shape));
    CVT_TRY(check_array_bytes(ctx, n, type->size));
    a.dtype = *type;
    a.shape = shape;
    a.data.assign(static_cast<std::size_t>(n * type->size), std::byte{0});
    const auto values = words(as_chars(payload));
    if (values.size() < n) return fail(Status::Decode, "NRRD text data has {} of {} values", values.size(), n);
    for (std::uint64_t i = 0; i < n; ++i) store_double(*type, a.data.data() + i * type->size, to_double(values[static_cast<std::size_t>(i)]).value_or(NAN));
  } else if (encoding == "raw" || encoding == "gzip" || encoding == "gz") {
    if (skip_bytes > payload.size()) return fail(Status::Decode, "NRRD byte skip beyond the data");
    payload = payload.subspan(static_cast<std::size_t>(skip_bytes));
    CVT_TRY_ASSIGN(a, raw_array(payload, *type, shape, big, ctx));
  } else {
    return fail(Status::Decode, "unsupported NRRD encoding '{}'", encoding);
  }
  a.name = "volume";
  a.attributes = header;
  Collection c;
  c.arrays.push_back(std::move(a));
  return c;
}

Result<Bytes> write_nrrd_array(const Array& source, const Json& extra, WriteContext& ctx) {
  Array a = source;
  if (a.dtype.kind == ScalarKind::Bool) a.dtype = {ScalarKind::UInt, 1};
  const std::string type = nrrd_type_name(a.dtype);
  if (type.empty()) return fail(Status::Encode, "NRRD cannot store {} values", dtype_name(a.dtype));
  const std::string encoding = ctx.options.get_string("nrrd_encoding", "gzip");
  std::string header = "NRRD0004\n# Complete NRRD file format specification at:\n# http://teem.sourceforge.net/nrrd/format.html\n";
  header += "type: " + type + "\ndimension: " + std::to_string(a.shape.size()) + "\nsizes:";
  for (auto it = a.shape.rbegin(); it != a.shape.rend(); ++it) header += " " + std::to_string(*it);
  header += "\n";
  for (const std::string_view key : {"space", "space directions", "space origin", "spacings", "kinds", "units", "space units"}) {
    if (const Json* v = extra.find(key); v != nullptr && v->is_string()) header += std::string(key) + ": " + v->as_string() + "\n";
  }
  if (a.dtype.size > 1) header += "endian: little\n";
  header += "encoding: " + encoding + "\n\n";
  Bytes out = to_bytes(header);
  if (encoding == "ascii") {
    std::string text;
    const std::uint64_t per_line = a.shape.empty() ? 1 : a.shape.back();
    for (std::uint64_t i = 0; i < a.count(); ++i) {
      text += fmt_num(a.number(i));
      text += (i + 1) % per_line == 0 ? '\n' : ' ';
    }
    append(out, as_bytes(text));
  } else if (encoding == "gzip") {
    CVT_TRY_ASSIGN(const Bytes packed, util::gzip_compress(a.data, options::resolved_compression_level(ctx.options, 9)));
    append(out, ByteView(packed));
  } else {
    append(out, ByteView(a.data));
  }
  return out;
}

// --------------------------------------------------------------------------- MetaImage

std::optional<DType> met_type(std::string_view name) {
  static constexpr std::pair<std::string_view, DType> table[] = {
      {"MET_CHAR", {ScalarKind::Int, 1}},   {"MET_UCHAR", {ScalarKind::UInt, 1}},   {"MET_SHORT", {ScalarKind::Int, 2}},
      {"MET_USHORT", {ScalarKind::UInt, 2}}, {"MET_INT", {ScalarKind::Int, 4}},      {"MET_UINT", {ScalarKind::UInt, 4}},
      {"MET_LONG", {ScalarKind::Int, 4}},   {"MET_ULONG", {ScalarKind::UInt, 4}},   {"MET_LONG_LONG", {ScalarKind::Int, 8}},
      {"MET_ULONG_LONG", {ScalarKind::UInt, 8}}, {"MET_FLOAT", {ScalarKind::Float, 4}}, {"MET_DOUBLE", {ScalarKind::Float, 8}}};
  for (const auto& [n, t] : table) {
    if (n == name) return t;
  }
  return std::nullopt;
}

std::string met_type_name(DType t) {
  switch (t.kind) {
    case ScalarKind::Int: return t.size == 1 ? "MET_CHAR" : t.size == 2 ? "MET_SHORT" : t.size == 4 ? "MET_INT" : "MET_LONG_LONG";
    case ScalarKind::UInt:
    case ScalarKind::Bool: return t.size == 1 ? "MET_UCHAR" : t.size == 2 ? "MET_USHORT" : t.size == 4 ? "MET_UINT" : "MET_ULONG_LONG";
    case ScalarKind::Float: return t.size == 4 ? "MET_FLOAT" : "MET_DOUBLE";
    default: return "";
  }
}

Result<Collection> read_metaimage(ReadContext& ctx) {
  const ByteView input = ctx.input;
  std::size_t pos = 0;
  Json header = Json::object();
  std::size_t data_start = std::string_view::npos;
  while (pos < input.size()) {
    std::size_t end = pos;
    while (end < input.size() && input[end] != std::byte{'\n'}) ++end;
    std::string_view line = as_chars(input.subspan(pos, end - pos));
    if (line.ends_with('\r')) line.remove_suffix(1);
    pos = std::min(end + 1, input.size());
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string key(util::trim(line.substr(0, eq)));
    const std::string value = util::sanitize_utf8(util::trim(line.substr(eq + 1)));
    header[key] = value;
    if (key == "ElementDataFile") {
      data_start = pos;
      break;
    }
    if (header.size() > 1024) return fail(Status::Limit, "MetaImage header is too long");
  }
  if (!header.contains("NDims") || !header.contains("DimSize")) return fail(Status::Decode, "not a MetaImage header");
  if (data_start == std::string_view::npos || header["ElementDataFile"].as_string() != "LOCAL") {
    return fail(Status::Decode, "the MetaImage header refers to a separate data file ('{}'); convert the .mha with embedded data",
                header["ElementDataFile"].as_string());
  }
  const auto type = met_type(header["ElementType"].as_string());
  if (!type) return fail(Status::Decode, "unsupported MetaImage type '{}'", header["ElementType"].as_string());
  std::vector<std::uint64_t> shape;
  const auto dims = words(header["DimSize"].as_string());
  for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
    const auto n = to_int(*it);
    if (!n || *n <= 0) return fail(Status::Decode, "bad MetaImage DimSize");
    shape.push_back(static_cast<std::uint64_t>(*n));
  }
  const auto channels = to_int(header["ElementNumberOfChannels"].as_string()).value_or(1);
  if (channels > 1) shape.push_back(static_cast<std::uint64_t>(channels));
  const auto msb = [&](std::string_view key) { return util::iequals(header[key].as_string(), "True"); };
  const bool big = msb("BinaryDataByteOrderMSB") || msb("ElementByteOrderMSB");
  ByteView payload = input.subspan(data_start);
  Bytes inflated;
  if (msb("CompressedData")) {
    CVT_TRY_ASSIGN(inflated, inflate_any(payload, ctx));
    payload = inflated;
  }
  CVT_TRY_ASSIGN(Array a, raw_array(payload, *type, shape, big, ctx));
  a.name = "volume";
  a.attributes = header;
  Collection c;
  c.arrays.push_back(std::move(a));
  return c;
}

Result<Bytes> write_metaimage_array(const Array& source, const Json& extra, WriteContext& ctx) {
  Array a = source;
  if (a.dtype.kind == ScalarKind::Bool) a.dtype = {ScalarKind::UInt, 1};
  const std::string type = met_type_name(a.dtype);
  if (type.empty()) return fail(Status::Encode, "MetaImage cannot store {} values", dtype_name(a.dtype));
  const int level = options::resolved_compression_level(ctx.options, 9);
  std::string header = "ObjectType = Image\nNDims = " + std::to_string(a.shape.size()) + "\nBinaryData = True\n"
                       "BinaryDataByteOrderMSB = False\nCompressedData = " + (level > 0 ? "True" : "False") + "\n";
  for (const std::string_view key : {"TransformMatrix", "Offset", "CenterOfRotation", "AnatomicalOrientation", "ElementSpacing"}) {
    if (const Json* v = extra.find(key); v != nullptr && v->is_string()) header += std::string(key) + " = " + v->as_string() + "\n";
  }
  header += "DimSize =";
  for (auto it = a.shape.rbegin(); it != a.shape.rend(); ++it) header += " " + std::to_string(*it);
  Bytes packed;
  if (level > 0) {
    CVT_TRY_ASSIGN(packed, util::zlib_compress(a.data, level));
    header += "\nCompressedDataSize = " + std::to_string(packed.size());
  }
  header += "\nElementType = " + type + "\nElementDataFile = LOCAL\n";
  Bytes out = to_bytes(header);
  append(out, level > 0 ? ByteView(packed) : ByteView(a.data));
  return out;
}

// --------------------------------------------------------------------------- NIfTI-1 / Analyze 7.5

std::optional<DType> nifti_type(std::int16_t code) {
  switch (code) {
    case 2: return DType{ScalarKind::UInt, 1};
    case 4: return DType{ScalarKind::Int, 2};
    case 8: return DType{ScalarKind::Int, 4};
    case 16: return DType{ScalarKind::Float, 4};
    case 32: return DType{ScalarKind::Complex, 8};
    case 64: return DType{ScalarKind::Float, 8};
    case 256: return DType{ScalarKind::Int, 1};
    case 512: return DType{ScalarKind::UInt, 2};
    case 768: return DType{ScalarKind::UInt, 4};
    case 1024: return DType{ScalarKind::Int, 8};
    case 1280: return DType{ScalarKind::UInt, 8};
    case 1792: return DType{ScalarKind::Complex, 16};
    default: return std::nullopt;
  }
}

std::int16_t nifti_code(DType t) {
  switch (t.kind) {
    case ScalarKind::Bool:
    case ScalarKind::UInt: return t.size == 1 ? 2 : t.size == 2 ? 512 : t.size == 4 ? 768 : 1280;
    case ScalarKind::Int: return t.size == 1 ? 256 : t.size == 2 ? 4 : t.size == 4 ? 8 : 1024;
    case ScalarKind::Float: return t.size == 4 ? 16 : 64;
    case ScalarKind::Complex: return t.size == 8 ? 32 : 1792;
    default: return 0;
  }
}

Result<Collection> read_nifti(ReadContext& ctx) {
  ByteView input = ctx.input;
  Bytes inflated;
  if (input.size() >= 2 && input[0] == std::byte{0x1F} && input[1] == std::byte{0x8B}) {
    CVT_TRY_ASSIGN(inflated, util::gzip_decompress(input, ctx.limits.max_decompressed_bytes));
    input = inflated;
  }
  if (input.size() < 348) return fail(Status::Decode, "NIfTI/Analyze header is truncated");
  util::ByteReader probe(input);
  const std::int32_t size_le = probe.i32le();
  bool big = false;
  if (size_le != 348) {
    if (std::byteswap(size_le) != 348) return fail(Status::Decode, "not a NIfTI-1 or Analyze 7.5 header");
    big = true;
  }
  const std::endian order = big ? std::endian::big : std::endian::little;
  const auto i16 = [&](std::size_t at) {
    util::ByteReader r(input.subspan(at, 2));
    return r.read<std::int16_t>(order);
  };
  const auto f32 = [&](std::size_t at) {
    util::ByteReader r(input.subspan(at, 4));
    return static_cast<double>(std::bit_cast<float>(r.read<std::uint32_t>(order)));
  };
  const std::string_view magic = as_chars(input.subspan(344, 4));
  const bool nifti = magic.starts_with("n+1") || magic.starts_with("ni1");
  const std::int16_t ndim = i16(40);
  if (ndim < 1 || ndim > 7) return fail(Status::Decode, "bad NIfTI dimension count {}", ndim);
  std::vector<std::uint64_t> shape;
  Json dims = Json::array();
  for (int d = ndim; d >= 1; --d) {
    const std::int16_t n = i16(40 + 2 * static_cast<std::size_t>(d));
    if (n <= 0) return fail(Status::Decode, "bad NIfTI dimension {}", n);
    shape.push_back(static_cast<std::uint64_t>(n));
  }
  for (int d = 1; d <= ndim; ++d) dims.push_back(i16(40 + 2 * static_cast<std::size_t>(d)));
  const std::int16_t datatype = i16(70);
  Json header = Json::object();
  header["format"] = nifti ? (magic.starts_with("n+1") ? "NIfTI-1 (single file)" : "NIfTI-1 (header/image pair)") : "Analyze 7.5";
  header["dim"] = std::move(dims);
  header["datatype"] = datatype;
  header["bitpix"] = i16(72);
  Json pixdim = Json::array();
  for (int d = 1; d <= ndim; ++d) pixdim.push_back(json_number(f32(76 + 4 * static_cast<std::size_t>(d))));
  header["pixdim"] = std::move(pixdim);
  const double vox_offset = f32(108);
  double slope = nifti ? f32(112) : 0.0;
  double inter = nifti ? f32(116) : 0.0;
  if (nifti) {
    header["scl_slope"] = json_number(slope);
    header["scl_inter"] = json_number(inter);
    header["qform_code"] = i16(252);
    header["sform_code"] = i16(254);
    header["xyzt_units"] = static_cast<int>(static_cast<std::uint8_t>(input[123]));
    std::string descrip(as_chars(input.subspan(148, 80)));
    descrip = descrip.substr(0, descrip.find('\0'));
    header["descrip"] = util::sanitize_utf8(descrip);
    Json srow = Json::array();
    for (std::size_t k = 0; k < 12; ++k) srow.push_back(json_number(f32(280 + 4 * k)));
    header["srow"] = std::move(srow);
  }
  Collection c;
  if (nifti && magic.starts_with("ni1")) {
    // Header of a pair: no voxels here.
    c.attributes = header;
    c.arrays.push_back(Array{"header", DType{ScalarKind::UInt, 1}, {}, {0}, {}, header});
    ctx.warn("This NIfTI header refers to a separate .img file; only the header was read.");
    return c;
  }
  if (!nifti) {
    if (input.size() <= 348) {
      c.arrays.push_back(Array{"header", DType{ScalarKind::UInt, 1}, {}, {0}, {}, header});
      ctx.warn("Analyze 7.5 headers (.hdr) hold no voxels; the matching .img file is needed for the image.");
      return c;
    }
  }
  if (datatype == 128 || datatype == 2304) {
    shape.push_back(datatype == 128 ? 3 : 4);
  }
  const auto type = datatype == 128 || datatype == 2304 ? std::optional<DType>(DType{ScalarKind::UInt, 1}) : nifti_type(datatype);
  if (!type) return fail(Status::Decode, "unsupported NIfTI datatype {}", datatype);
  const std::size_t offset = nifti ? static_cast<std::size_t>(std::max(352.0, vox_offset)) : 348;
  if (!std::isfinite(vox_offset) || offset > input.size()) return fail(Status::Decode, "bad NIfTI vox_offset");
  CVT_TRY_ASSIGN(Array a, raw_array(input.subspan(offset), *type, shape, big, ctx));
  if (slope != 0.0 && std::isfinite(slope) && std::isfinite(inter) && !(slope == 1.0 && inter == 0.0) &&
      type->kind != ScalarKind::Complex) {
    Array scaled;
    scaled.shape = a.shape;
    scaled.dtype = {ScalarKind::Float, 8};
    scaled.data.assign(static_cast<std::size_t>(a.count() * 8), std::byte{0});
    for (std::uint64_t i = 0; i < a.count(); ++i) store_double(scaled.dtype, scaled.data.data() + i * 8, slope * a.number(i) + inter);
    a = std::move(scaled);
  }
  a.name = "volume";
  a.attributes = header;
  c.arrays.push_back(std::move(a));
  return c;
}

Result<Bytes> write_nifti_array(const Array& source, const Json& extra, WriteContext& ctx, bool gzip) {
  Array a = source;
  bool rgb = false;
  if (a.dtype.kind == ScalarKind::Bool) a.dtype = {ScalarKind::UInt, 1};
  std::vector<std::uint64_t> dims(a.shape.rbegin(), a.shape.rend());  // fastest first
  if (extra["rgb"].as_bool(false) && !dims.empty() && dims.front() == 3 && a.dtype == DType{ScalarKind::UInt, 1}) {
    rgb = true;
    dims.erase(dims.begin());
  }
  const std::int16_t code = rgb ? std::int16_t{128} : nifti_code(a.dtype);
  if (code == 0) return fail(Status::Encode, "NIfTI cannot store {} values", dtype_name(a.dtype));
  if (dims.empty() || dims.size() > 7) return fail(Status::Encode, "NIfTI volumes have 1 to 7 dimensions");
  util::ByteWriter out(352 + a.data.size());
  out.i32le(348);
  out.zeros(36);  // data_type, db_name, extents, session_error, regular, dim_info
  out.i16le(static_cast<std::int16_t>(dims.size()));
  for (std::size_t d = 0; d < 7; ++d) {
    const std::uint64_t n = d < dims.size() ? dims[d] : 1;
    if (n > 32767) return fail(Status::Encode, "NIfTI-1 dimensions are limited to 32767");
    out.i16le(static_cast<std::int16_t>(n));
  }
  out.f32le(0).f32le(0).f32le(0);  // intent_p1..3
  out.i16le(0);                     // intent_code
  out.i16le(code);
  out.i16le(static_cast<std::int16_t>(rgb ? 24 : a.dtype.size * 8));
  out.i16le(0);  // slice_start
  const Json& pixdim = extra["pixdim"];
  out.f32le(1.0f);  // qfac
  for (std::size_t d = 0; d < 7; ++d) {
    out.f32le(d < pixdim.size() ? static_cast<float>(pixdim[d].as_double(1.0)) : 1.0f);
  }
  out.f32le(352.0f);  // vox_offset
  out.f32le(1.0f).f32le(0.0f);  // scl_slope, scl_inter
  out.i16le(0);                 // slice_end
  out.u8(0);                    // slice_code
  out.u8(static_cast<std::uint8_t>(extra["xyzt_units"].as_int64(10)));  // mm + s
  out.f32le(0).f32le(0).f32le(0).f32le(0).i32le(0).i32le(0);  // cal_max/min, slice_duration, toffset, glmax/min
  std::string descrip;
  if (!options::resolved_strip_metadata(ctx.options)) descrip = extra["descrip"].as_string().substr(0, 79);
  descrip.resize(80, '\0');
  out.text(descrip);
  out.zeros(24);  // aux_file
  const Json& srow = extra["srow"];
  const bool has_srow = srow.is_array() && srow.size() == 12;
  out.i16le(0);                            // qform_code
  out.i16le(has_srow ? std::int16_t{1} : std::int16_t{0});  // sform_code
  out.zeros(4 * 6);                        // quatern b,c,d, qoffset x,y,z
  for (std::size_t k = 0; k < 12; ++k) {
    float v = 0;
    if (has_srow) {
      v = static_cast<float>(srow[k].as_double());
    } else if (k == 0 || k == 5 || k == 10) {
      v = 1;
    }
    out.f32le(v);
  }
  out.zeros(16);  // intent_name
  out.text(std::string_view("n+1\0", 4));
  out.zeros(4);  // extension flag
  out.bytes(a.data);
  if (!gzip) return out.take();
  return util::gzip_compress(out.view(), options::resolved_compression_level(ctx.options, 9));
}

// --------------------------------------------------------------------------- shared codecs

Result<Array> image_to_volume(const ir::Image& image, const Context& ctx, bool allow_rgb) {
  if (!image.valid()) return fail(Status::Encode, "the image has no pixels");
  bool grey = true;
  for (const auto& f : image.frames) grey = grey && imaging::is_grey(f);
  Array a;
  a.name = "volume";
  const std::uint64_t w = image.width(), h = image.height(), z = image.frames.size();
  if (grey || !allow_rgb) {
    if (!grey) ctx.warn("Colour slices were converted to grey.");
    a.dtype = {ScalarKind::UInt, 1};
    a.shape = z > 1 ? std::vector<std::uint64_t>{z, h, w} : std::vector<std::uint64_t>{h, w};
    a.data.resize(static_cast<std::size_t>(z * h * w));
    for (std::uint64_t f = 0; f < z; ++f) {
      const ir::Frame& frame = image.frames[static_cast<std::size_t>(f)];
      for (std::uint64_t i = 0; i < w * h; ++i) {
        a.data[static_cast<std::size_t>(f * w * h + i)] =
            static_cast<std::byte>(grey ? frame.rgba[static_cast<std::size_t>(i * 4)] : std::lround(imaging::luma(frame, static_cast<std::size_t>(i * 4))));
      }
    }
  } else {
    a.dtype = {ScalarKind::UInt, 1};
    a.shape = z > 1 ? std::vector<std::uint64_t>{z, h, w, 3} : std::vector<std::uint64_t>{h, w, 3};
    a.attributes["rgb"] = true;
    a.data.resize(static_cast<std::size_t>(z * h * w * 3));
    for (std::uint64_t f = 0; f < z; ++f) {
      const ir::Frame& frame = image.frames[static_cast<std::size_t>(f)];
      for (std::uint64_t i = 0; i < w * h; ++i) {
        for (std::uint64_t c = 0; c < 3; ++c) a.data[static_cast<std::size_t>((f * w * h + i) * 3 + c)] = static_cast<std::byte>(frame.rgba[static_cast<std::size_t>(i * 4 + c)]);
      }
    }
  }
  return a;
}

/// Flip each slice vertically (images are top-down, volumes bottom-up).
void flip_slices(Array& a, bool rgb) {
  if (a.shape.size() < 2) return;
  const std::size_t channels = rgb ? static_cast<std::size_t>(a.shape.back()) : 1;
  const std::size_t base = rgb ? a.shape.size() - 1 : a.shape.size();
  if (base < 2) return;
  const std::uint64_t h = a.shape[base - 2];
  const std::uint64_t row = a.shape[base - 1] * channels * a.dtype.size;
  const std::uint64_t slice = h * row;
  for (std::uint64_t s = 0; s + slice <= a.data.size(); s += slice) {
    for (std::uint64_t y = 0; y < h / 2; ++y) {
      std::swap_ranges(a.data.begin() + static_cast<std::ptrdiff_t>(s + y * row),
                       a.data.begin() + static_cast<std::ptrdiff_t>(s + (y + 1) * row),
                       a.data.begin() + static_cast<std::ptrdiff_t>(s + (h - 1 - y) * row));
    }
  }
}

using VolumeReader = Result<Collection> (*)(ReadContext&);
using VolumeWriter = std::function<Result<Bytes>(const Array&, const Json&, WriteContext&)>;

void register_volume(Registry& registry, const std::string& id, VolumeReader read, VolumeWriter write, bool flip_default,
                     std::vector<OptionSpec> write_options) {
  std::vector<OptionSpec> image_read = imaging::window_options("linear", 0.0);
  image_read.push_back(OptionSpec::boolean("flip_vertical", "Flip vertically", flip_default,
                                           "Show the first row of each slice at the bottom (radiological/patient view)."));
  std::vector<OptionSpec> image_write = write_options;
  image_write.push_back(OptionSpec::boolean("flip_vertical", "Flip vertically", flip_default, "Store the bottom row first."));
  add_codec(
      registry, id, Kind::Image,
      [read](ReadContext& ctx) -> Result<AnyIR> {
        CVT_TRY_ASSIGN(const Collection c, read(ctx));
        const Array& a = c.arrays.front();
        if (a.count() == 0) return fail(Status::Decode, "the file holds a header but no voxels");
        imaging::Window window = imaging::window_from_options(ctx.options);
        Array view = a;
        // Collapse a trailing time/channel axis of 1 and keep 4-D as more slices.
        while (view.shape.size() > 3 && view.shape.front() == 1) view.shape.erase(view.shape.begin());
        if (view.shape.size() > 3 && !(view.shape.back() == 3 && a.dtype == DType{ScalarKind::UInt, 1})) {
          std::uint64_t slices = 1;
          for (std::size_t d = 0; d + 2 < view.shape.size(); ++d) slices *= view.shape[d];
          view.shape = {slices, view.shape[view.shape.size() - 2], view.shape.back()};
        }
        return imaging::array_to_image(view, window, ctx.options.get_bool("flip_vertical", true), ctx);
      },
      [write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
        CVT_TRY_ASSIGN(Array a, image_to_volume(value.get<ir::Image>(), ctx, true));
        if (ctx.options.get_bool("flip_vertical", true)) flip_slices(a, a.attributes["rgb"].as_bool(false));
        return write(a, a.attributes, ctx);
      },
      image_read, image_write, 20, 15, true);
  add_collection_codecs(
      registry, id, read,
      [write](const Collection& c, WriteContext& ctx) -> Result<Bytes> {
        for (const Array& a : c.arrays) {
          if (a.count() > 0 && a.dtype.kind != ScalarKind::Record && a.dtype.kind != ScalarKind::Unicode &&
              a.dtype.kind != ScalarKind::Bytes) {
            if (c.arrays.size() > 1) ctx.warn("Only the first numeric array was written.");
            return write(a, a.attributes, ctx);
          }
        }
        return fail(Status::Encode, "no numeric array to write");
      },
      {}, write_options);
}

int sniff_nifti(ByteView data, std::string_view name) {
  if (data.size() >= 348) {
    const std::string_view magic = as_chars(data.subspan(344, 4));
    if (magic == std::string_view("n+1\0", 4) || magic == std::string_view("ni1\0", 4)) return 95;
    util::ByteReader r(data);
    const std::int32_t size = r.i32le();
    if ((size == 348 || std::byteswap(size) == 348) && (has_extension(name, "hdr") || has_extension(name, "img"))) return 85;
  }
  if (data.size() >= 2 && data[0] == std::byte{0x1F} && data[1] == std::byte{0x8B} && has_extension(name, "gz") &&
      util::iends_with(name, ".nii.gz")) {
    return 92;
  }
  return 0;
}

}  // namespace
}  // namespace cvt::science::arrays

namespace cvt::science {

void register_volumes(Registry& registry) {
  using namespace arrays;
  add_format(registry, {.id = "nrrd",
                        .name = "NRRD volume",
                        .description = "Nearly Raw Raster Data (Teem, 3D Slicer).",
                        .category = Category::Image,
                        .extensions = {"nrrd", "nhdr"},
                        .mime = "application/x-nrrd",
                        .popularity = 12,
                        .signatures = {Signature::at(0, "'NRRD000'")}});
  register_volume(registry, "nrrd", read_nrrd, write_nrrd_array, false,
                  {OptionSpec::choice("nrrd_encoding", "Encoding", "gzip", {"gzip", "raw", "ascii"}, "How voxels are stored."),
                   options::compression_level(9)});
  add_format(registry, {.id = "mha",
                        .name = "MetaImage",
                        .description = "ITK MetaImage with embedded data (.mha); .mhd headers name a separate file.",
                        .category = Category::Image,
                        .extensions = {"mha", "mhd"},
                        .mime = "application/x-metaimage",
                        .popularity = 12,
                        .sniff = [](ByteView d, std::string_view) {
                          const std::string_view h = head_text(d, 1024);
                          return (h.starts_with("ObjectType") || h.starts_with("NDims")) && h.find("DimSize") != std::string_view::npos ? 92 : 0;
                        }});
  register_volume(registry, "mha", read_metaimage, write_metaimage_array, false, {options::compression_level(9), options::preset()});
  add_format(registry, {.id = "nii",
                        .name = "NIfTI-1 volume",
                        .description = "Neuroimaging Informatics Technology Initiative volume (.nii); Analyze 7.5 headers.",
                        .category = Category::Image,
                        .extensions = {"nii", "hdr"},
                        .mime = "application/x-nifti",
                        .popularity = 18,
                        .sniff = sniff_nifti});
  register_volume(registry, "nii", read_nifti,
                  [](const Array& a, const Json& extra, WriteContext& ctx) { return write_nifti_array(a, extra, ctx, false); },
                  true, {options::strip_metadata()});
  add_format(registry, {.id = "nii.gz",
                        .name = "NIfTI-1 volume (gzip)",
                        .description = "Gzip-compressed NIfTI-1 volume.",
                        .category = Category::Image,
                        .extensions = {"nii.gz"},
                        .mime = "application/x-nifti",
                        .popularity = 18,
                        .sniff = sniff_nifti});
  register_volume(registry, "nii.gz", read_nifti,
                  [](const Array& a, const Json& extra, WriteContext& ctx) { return write_nifti_array(a, extra, ctx, true); },
                  true, {options::compression_level(9), options::strip_metadata()});
}

}  // namespace cvt::science
