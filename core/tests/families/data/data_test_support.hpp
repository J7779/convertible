// Helpers shared by the data family tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ir/data.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

namespace cvt::test::data {

/// Parse JSON text with the family's own JSON reader (so tagged objects
/// such as {"$float": "NaN"} and exact big numbers work). Fails the test on
/// error and returns null.
inline Json json(std::string_view text) {
  auto parsed = read_as<ir::Data>("json", as_bytes(text));
  if (!parsed) {
    FAIL_CHECK("test JSON does not parse: ", parsed.error().message);
    return Json();
  }
  return std::move(parsed->value);
}

/// Read `text` (or bytes) as `format` into a Data value.
inline Result<Json> read(std::string_view format, std::string_view text, std::string_view options = {},
                         std::vector<std::string>* warnings = nullptr) {
  CVT_TRY_ASSIGN(ir::Data data, read_as<ir::Data>(format, as_bytes(text), options, warnings));
  return std::move(data.value);
}
inline Result<Json> read_bytes(std::string_view format, ByteView bytes, std::string_view options = {},
                               std::vector<std::string>* warnings = nullptr) {
  CVT_TRY_ASSIGN(ir::Data data, read_as<ir::Data>(format, bytes, options, warnings));
  return std::move(data.value);
}

/// Write a Data value as `format`; the text of the result.
inline Result<std::string> write(std::string_view format, const Json& value, std::string_view options = {},
                                 std::vector<std::string>* warnings = nullptr) {
  CVT_TRY_ASSIGN(Bytes bytes, write_from(format, ir::Data{value}, options, warnings));
  return str(bytes);
}
inline Result<Bytes> write_bytes(std::string_view format, const Json& value, std::string_view options = {},
                                 std::vector<std::string>* warnings = nullptr) {
  return write_from(format, ir::Data{value}, options, warnings);
}

/// write -> read with the same format.
inline Result<Json> round_trip(std::string_view format, const Json& value, std::string_view write_options = {},
                               std::string_view read_options = {}) {
  CVT_TRY_ASSIGN(Bytes bytes, write_from(format, ir::Data{value}, write_options));
  CVT_TRY_ASSIGN(ir::Data data, read_as<ir::Data>(format, bytes, read_options));
  return std::move(data.value);
}

/// "invalid YAML at line 3, column 5: ..." -> true when the message names that position.
inline bool mentions_position(const Error& error, std::size_t line, std::size_t column) {
  return error.message.find(std::format("line {}, column {}", line, column)) != std::string::npos;
}

}  // namespace cvt::test::data
