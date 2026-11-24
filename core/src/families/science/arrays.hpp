// Convertible core - science family: the n-dimensional array model shared by
// NumPy, MATLAB, NetCDF, FITS, HDF5 and the volume formats.
//
// Data: {"variables":[{"name","dtype","shape":[...],"data":[nested lists],
// "attributes":{...}}],"attributes":{...}}. dtype names follow NumPy
// ("float64", "int16", "uint8", "bool", "complex128", "S8", "U4"); structured
// arrays use "record" with "fields":[{"name","dtype"}] and data as a list of
// objects. Complex numbers are [re, im] pairs.
// Table: one sheet per variable; 2-D arrays as a grid, 1-D as one column,
// higher ranks flattened to rows of the last dimension; structured arrays get
// a header row of field names.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "families/science/common.hpp"

namespace cvt::science::arrays {

enum class ScalarKind { Bool, Int, UInt, Float, Complex, Bytes, Unicode, Record };

struct DType {
  ScalarKind kind = ScalarKind::Float;
  std::uint32_t size = 8;  ///< bytes per element (complex: both parts; Unicode: 4 per character)
  friend bool operator==(const DType&, const DType&) = default;
};

struct Field {
  std::string name;
  DType type;
  std::uint32_t offset = 0;
};

struct Array {
  std::string name;
  DType dtype;
  std::vector<Field> fields;         ///< for ScalarKind::Record
  std::vector<std::uint64_t> shape;  ///< C (row-major) order; empty = scalar
  Bytes data;                        ///< little-endian elements in C order
  Json attributes = Json::object();

  [[nodiscard]] std::uint64_t count() const noexcept;
  [[nodiscard]] double number(std::uint64_t index) const noexcept;
  [[nodiscard]] const std::byte* element(std::uint64_t index) const noexcept {
    return data.data() + index * dtype.size;
  }
};

/// "float64", "int8", "complex64", "S5", "U3", "bool", "record".
[[nodiscard]] std::string dtype_name(DType type);
[[nodiscard]] std::optional<DType> parse_dtype_name(std::string_view name);
/// NumPy array-protocol type strings such as "<f8", ">i2", "|b1", "<U4".
/// Sets `big_endian` (false for single-byte types).
[[nodiscard]] std::optional<DType> parse_numpy_descr(std::string_view descr, bool& big_endian);
[[nodiscard]] std::string numpy_descr(DType type);

/// Read one element of a scalar type as double / JSON.
[[nodiscard]] double scalar_double(DType type, const std::byte* p) noexcept;
[[nodiscard]] Json scalar_json(DType type, const std::byte* p);
/// Store a double in a scalar element (saturating/rounding for integers).
void store_double(DType type, std::byte* p, double value) noexcept;
/// Reverse the byte order of every element in place (records: per field).
void swap_elements(Array& array);
/// Overflow-checked element count of a shape.
[[nodiscard]] Result<std::uint64_t> shape_count(const std::vector<std::uint64_t>& shape);
/// Check that `count * size` bytes may be allocated.
[[nodiscard]] Result<void> check_array_bytes(const Context& ctx, std::uint64_t count, std::uint64_t size);
/// Transpose Fortran-order (column-major) data into C order in place.
void fortran_to_c(Array& array);
void c_to_fortran(Array& array);

[[nodiscard]] Result<Json> array_to_json(const Array& array, const Context& ctx);
[[nodiscard]] Result<Array> json_to_array(const Json& value, const Context& ctx);
[[nodiscard]] Result<ir::Sheet> array_to_sheet(const Array& array, const Context& ctx);
[[nodiscard]] Result<Array> sheet_to_array(const ir::Sheet& sheet);

/// A collection of named arrays with global attributes.
struct Collection {
  std::vector<Array> arrays;
  Json attributes = Json::object();
};
[[nodiscard]] Result<Json> collection_to_json(const Collection& collection, const Context& ctx);
[[nodiscard]] Result<Collection> json_to_collection(const Json& value, const Context& ctx);
[[nodiscard]] Result<ir::Table> collection_to_table(const Collection& collection, const Context& ctx);
[[nodiscard]] Result<Collection> table_to_collection(const ir::Table& table);

using CollectionReader = std::function<Result<Collection>(ReadContext& ctx)>;
using CollectionWriter = std::function<Result<Bytes>(const Collection& collection, WriteContext& ctx)>;
/// Register Data and Table codecs.
void add_collection_codecs(Registry& registry, const std::string& id, CollectionReader read, CollectionWriter write,
                           std::vector<OptionSpec> read_options = {}, std::vector<OptionSpec> write_options = {});

/// HDF5 reader shared with MATLAB 7.3 files.
[[nodiscard]] Result<Collection> read_hdf5_collection(ByteView input, const Context& ctx);

/// Half-precision helpers.
[[nodiscard]] double half_to_double(std::uint16_t h) noexcept;
[[nodiscard]] std::uint16_t double_to_half(double v) noexcept;

}  // namespace cvt::science::arrays
